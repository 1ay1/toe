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

    // A font can have BOTH real outlines and colour tables — that is exactly
    // what Segoe UI Emoji is (COLR/CPAL layers over ordinary glyphs), and it is
    // every Windows machine's emoji font. Since stb loads it fine, the branch
    // above never runs, so we must attach the colour backend HERE too or every
    // emoji silently renders as a flat monochrome silhouette.
    //
    // This is additive: if the font has no colour tables the backend is null
    // and nothing changes. Outline metrics (computed below) are kept either
    // way, since a layered face is drawn from those same outlines.
    f.color_ = try_load_color_face(f.data_);

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
    // Prefer stb's cmap whenever the face has real outlines: it handles every
    // cmap format and is the same lookup the rasteriser uses. The colour
    // backend's own cmap is only the authority for a BITMAP-only face, where
    // stb couldn't parse the font at all.
    if (h_) return static_cast<std::uint32_t>(stbtt_FindGlyphIndex(&h_->info, static_cast<int>(cp)));
    if (color_) return color_glyph_index(*color_, cp);
    return 0;
}

int Face::cap_height() const noexcept {
    if (!h_) return metrics_.ascent; // colour-bitmap face: no outlines
    const int g = stbtt_FindGlyphIndex(&h_->info, 'H');
    if (g == 0) return metrics_.ascent;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!stbtt_GetGlyphBox(&h_->info, g, &x0, &y0, &x1, &y1)) return metrics_.ascent;
    return static_cast<int>(std::lround((y1 - y0) * scale_));
}

GlyphBitmap Face::rasterize(std::uint32_t glyph_index) const {
    GlyphBitmap out;
    if (color_) {
        // Bitmap (CBDT) faces decode a PNG strike directly.
        if (GlyphBitmap bm = color_rasterize(*color_, glyph_index, pixel_height_);
            !bm.pixels.empty()) {
            return bm;
        }
        // Layered (COLR) faces: each layer is an ORDINARY outline glyph with a
        // palette colour, so we rasterise them with stb and alpha-composite
        // back-to-front into one RGBA bitmap. This is what makes Windows'
        // Segoe UI Emoji render in colour instead of as a flat silhouette.
        if (auto layers = color_->layers_of(glyph_index); !layers.empty() && h_) {
            // Union bbox over all layers, so nothing is clipped.
            int bx0 = INT_MAX, by0 = INT_MAX, bx1 = INT_MIN, by1 = INT_MIN;
            for (const auto &L : layers) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphBitmapBox(&h_->info, L.gid, scale_, scale_, &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0) continue;
                bx0 = std::min(bx0, x0); by0 = std::min(by0, y0);
                bx1 = std::max(bx1, x1); by1 = std::max(by1, y1);
            }
            if (bx1 > bx0 && by1 > by0) {
                const int w = bx1 - bx0, ht = by1 - by0;
                out.is_color = true;
                out.width = w;
                out.height = ht;
                out.bearing_x = bx0;
                out.bearing_y = by0;
                out.pixels.assign(static_cast<std::size_t>(w) * ht * 4u, 0);

                int adv = 0, lsb = 0;
                stbtt_GetGlyphHMetrics(&h_->info, static_cast<int>(glyph_index), &adv, &lsb);
                out.advance = static_cast<int>(std::lround(adv * scale_));

                std::vector<std::uint8_t> cov;
                for (const auto &L : layers) {
                    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    stbtt_GetGlyphBitmapBox(&h_->info, L.gid, scale_, scale_, &x0, &y0, &x1, &y1);
                    const int lw = x1 - x0, lh = y1 - y0;
                    if (lw <= 0 || lh <= 0) continue;
                    cov.assign(static_cast<std::size_t>(lw) * lh, 0);
                    stbtt_MakeGlyphBitmap(&h_->info, cov.data(), lw, lh, lw, scale_, scale_, L.gid);

                    // Source-over composite of this layer's flat colour.
                    for (int y = 0; y < lh; ++y) {
                        const int dy = (y0 - by0) + y;
                        if (dy < 0 || dy >= ht) continue;
                        for (int x = 0; x < lw; ++x) {
                            const int dx = (x0 - bx0) + x;
                            if (dx < 0 || dx >= w) continue;
                            const std::uint32_t c = cov[static_cast<std::size_t>(y) * lw + x];
                            if (!c) continue;
                            const std::uint32_t sa = c * L.a / 255u; // layer alpha
                            if (!sa) continue;
                            std::uint8_t *d =
                                &out.pixels[(static_cast<std::size_t>(dy) * w + dx) * 4u];
                            const std::uint32_t ia = 255u - sa;
                            d[0] = static_cast<std::uint8_t>((L.r * sa + d[0] * ia) / 255u);
                            d[1] = static_cast<std::uint8_t>((L.g * sa + d[1] * ia) / 255u);
                            d[2] = static_cast<std::uint8_t>((L.b * sa + d[2] * ia) / 255u);
                            d[3] = static_cast<std::uint8_t>(sa + d[3] * ia / 255u);
                        }
                    }
                }
                return out;
            }
        }
        // Neither format produced anything: fall through to the mono outline
        // path below so the glyph still draws rather than vanishing.
    }
    if (!h_) return out;

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&h_->info, static_cast<int>(glyph_index), &adv, &lsb);
    out.advance = static_cast<int>(std::lround(adv * scale_));

    int w = 0, ht = 0, ox = 0, oy = 0;
    // Zero-copy: query the bbox, size out.pixels once, and let stb rasterize
    // DIRECTLY into it. The old stbtt_GetGlyphBitmap path mallocs internally,
    // then we'd copy + free — three heap ops per never-seen glyph. MakeGlyphBitmap
    // renders into a caller buffer, so it's one allocation (the output itself,
    // reused across frames by the atlas cache since each glyph is packed once).
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&h_->info, static_cast<int>(glyph_index), scale_, scale_,
                            &x0, &y0, &x1, &y1);
    w = x1 - x0;
    ht = y1 - y0;
    ox = x0;
    oy = y0; // stb: top edge relative to baseline, y DOWN positive
    if (w > 0 && ht > 0) {
        out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(ht));
        stbtt_MakeGlyphBitmap(&h_->info, out.pixels.data(), w, ht, /*stride=*/w, scale_, scale_,
                              static_cast<int>(glyph_index));
    }
    out.width = w;
    out.height = ht;
    out.bearing_x = ox;
    out.bearing_y = oy;
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
                    // Size-match the fallback to the primary's cap-height. Two
                    // fonts at the same pixel_height often have very different
                    // cap heights, so a raw fallback looks noticeably bigger or
                    // smaller than the surrounding text. Re-load at an adjusted
                    // pixel_height so the caps line up. (Colour-bitmap emoji
                    // faces have no outlines/cap-height and are scaled to the
                    // cell by the colour rasterizer instead, so skip them.)
                    if (primary_cap_ > 0 && !face->is_color()) {
                        const int fc = face->cap_height();
                        if (fc > 0) {
                            const int adj = static_cast<int>(
                                std::lround(static_cast<double>(pixel_height_) * primary_cap_ / fc));
                            // Only reload when it actually differs and stays sane
                            // (guard against pathological fonts / rounding).
                            if (adj > 0 && adj != pixel_height_ &&
                                adj >= pixel_height_ / 3 && adj <= pixel_height_ * 3) {
                                if (auto rescaled = Face::load(slurp(*path), adj)) {
                                    if (rescaled->glyph_index(cp) != 0) face = std::move(rescaled);
                                }
                            }
                        }
                    }
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
