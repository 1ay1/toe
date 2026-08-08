// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Face — the sole wrapper over stb_truetype. All stbtt_* calls live here; the
// rest of the engine sees only the typed Face / FaceStack in face.hpp.

#include "toe/gfx/face.hpp"

#include <cmath>
#include <fstream>

#include "toe/gfx/font_discovery.hpp"

#include "face_internal.hpp" // complete Handle + ColorBackend (includes stb)

namespace toe::gfx {

// Colour (CBDT/CBLC emoji) hooks, implemented in face_color.cpp.
std::unique_ptr<Face::ColorBackend> try_load_color_face(const std::vector<std::uint8_t> &blob);
void color_metrics(const Face::ColorBackend &cb, FaceMetrics &m);
std::uint32_t color_glyph_index(const Face::ColorBackend &cb, char32_t cp);
GlyphBitmap color_rasterize(const Face::ColorBackend &cb, std::uint32_t gid, int target_px);

// Face's special members: Handle + ColorBackend are complete here (via
// face_internal.hpp), so unique_ptr destruction/move-assign is well-formed.
Face::Face(Face &&) noexcept = default;
Face &Face::operator=(Face &&) noexcept = default;
Face::~Face() = default;

std::optional<Face> Face::load(std::vector<std::uint8_t> bytes, int pixel_height) {
    if (bytes.empty() || pixel_height <= 0) return std::nullopt;

    auto h = std::make_unique<Handle>();
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&h->info, bytes.data(), offset)) {
        // stb rejects colour-bitmap fonts (CBDT/CBLC emoji). Try the colour
        // backend before giving up — this is what makes Noto Color Emoji load.
        if (auto cb = try_load_color_face(bytes)) {
            Face f;
            f.data_ = std::move(bytes);
            f.pixel_height_ = pixel_height;
            // rebuild the backend over the now-owned blob (its reader pointed
            // into the moved-from vector).
            f.color_ = try_load_color_face(f.data_);
            if (f.color_) {
                color_metrics(*f.color_, f.metrics_);
                return f;
            }
        }
        return std::nullopt;
    }

    Face f;
    f.data_ = std::move(bytes);
    // stbtt_fontinfo stored a pointer into the caller's buffer; re-point it at
    // our now-owned copy so it stays valid for the Face's whole life.
    h->info.data = f.data_.data();
    f.h_ = std::move(h);
    f.pixel_height_ = pixel_height;
    f.scale_ = stbtt_ScaleForPixelHeight(&f.h_->info, static_cast<float>(pixel_height));

    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.h_->info, &asc, &desc, &gap);
    f.metrics_.ascent = static_cast<int>(std::lround(asc * f.scale_));
    f.metrics_.descent = static_cast<int>(std::lround(-desc * f.scale_)); // desc is negative
    f.metrics_.line_gap = static_cast<int>(std::lround(gap * f.scale_));

    // Reference advance: prefer 'M' (the classic monospace measure), then space.
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f.h_->info, 'M', &adv, &lsb);
    if (adv <= 0) stbtt_GetCodepointHMetrics(&f.h_->info, ' ', &adv, &lsb);
    f.metrics_.advance = static_cast<int>(std::lround(adv * f.scale_));

    return f;
}

std::uint32_t Face::glyph_index(char32_t cp) const noexcept {
    if (color_) return color_glyph_index(*color_, cp);
    if (!h_) return 0;
    return static_cast<std::uint32_t>(stbtt_FindGlyphIndex(&h_->info, static_cast<int>(cp)));
}

GlyphBitmap Face::rasterize(std::uint32_t glyph_index) const {
    GlyphBitmap out;
    if (color_) return color_rasterize(*color_, glyph_index, pixel_height_);
    if (!h_) return out;

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&h_->info, static_cast<int>(glyph_index), &adv, &lsb);
    out.advance = static_cast<int>(std::lround(adv * scale_));

    int w = 0, ht = 0, ox = 0, oy = 0;
    unsigned char *bmp =
        stbtt_GetGlyphBitmap(&h_->info, scale_, scale_, static_cast<int>(glyph_index), &w, &ht, &ox, &oy);
    if (bmp) {
        out.width = w;
        out.height = ht;
        out.bearing_x = ox;
        out.bearing_y = oy; // stb: top edge relative to baseline, y DOWN positive
        if (w > 0 && ht > 0) {
            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(ht);
            out.pixels.assign(bmp, bmp + n);
        }
        stbtt_FreeBitmap(bmp, nullptr);
    }
    return out;
}

// --- FaceStack --------------------------------------------------------------

namespace {
[[nodiscard]] std::vector<std::uint8_t> slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char *>(b.data()), sz);
    return b;
}
} // namespace

FaceStack::FaceStack() = default;
FaceStack::~FaceStack() = default;
FaceStack::FaceStack(FaceStack &&) noexcept = default;
FaceStack &FaceStack::operator=(FaceStack &&) noexcept = default;

FaceStack::Resolved FaceStack::resolve(char32_t cp) {
    // 1. Fast path: a loaded face already covers it (ASCII hits the primary).
    for (const Face &f : faces_) {
        if (const std::uint32_t gi = f.glyph_index(cp); gi != 0) return {&f, gi};
    }

    // 2. Total miss: lazily discover a system font that covers cp, load it, and
    //    append it to the chain. Cached by Unicode block, so the next codepoint
    //    in the same script skips straight to step 1. We may find a font whose
    //    cmap covers cp but that the rasterizer can't load (e.g. a CBDT/sbix
    //    COLOUR-BITMAP emoji font stb_truetype rejects) — skip those and keep
    //    asking discovery to exclude them until we get a rasterizable face.
    if (pixel_height_ > 0) {
        if (!discovery_) discovery_ = std::make_unique<FontDiscovery>();
        std::vector<std::string> tried = loaded_paths_;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto path = discovery_->resolve(cp, tried);
            if (!path) break; // nothing more on the system covers it
            if (auto face = Face::load(slurp(*path), pixel_height_)) {
                if (const std::uint32_t gi = face->glyph_index(cp); gi != 0) {
                    loaded_paths_.push_back(*path);
                    faces_.push_back(std::move(*face));
                    const Face &nf = faces_.back();
                    return {&nf, nf.glyph_index(cp)};
                }
            }
            tried.push_back(std::move(*path)); // covers-but-unloadable: skip, try next
        }
    }

    // 3. Nothing on the system has it: return the primary's .notdef (glyph 0),
    //    which the atlas draws as a visible box — never a blank cell.
    const Face &pf = faces_.front();
    return {&pf, pf.glyph_index(cp)};
}

} // namespace toe::gfx
