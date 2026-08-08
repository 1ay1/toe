// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Face — the sole wrapper over stb_truetype. All stbtt_* calls live here; the
// rest of the engine sees only the typed Face / FaceStack in face.hpp.

#include "toe/gfx/face.hpp"

#include <cmath>

#include "stb/stb_truetype.h"

namespace toe::gfx {

// The opaque handle Face pimpl-owns: a real stbtt_fontinfo, so no void*.
struct Face::Handle {
    stbtt_fontinfo info{};
};

Face::Face(Face &&) noexcept = default;
Face &Face::operator=(Face &&) noexcept = default;
Face::~Face() = default;

std::optional<Face> Face::load(std::vector<std::uint8_t> bytes, int pixel_height) {
    if (bytes.empty() || pixel_height <= 0) return std::nullopt;

    auto h = std::make_unique<Handle>();
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&h->info, bytes.data(), offset)) {
        return std::nullopt;
    }

    Face f;
    f.data_ = std::move(bytes);
    // stbtt_fontinfo stored a pointer into the caller's buffer; re-point it at
    // our now-owned copy so it stays valid for the Face's whole life.
    h->info.data = f.data_.data();
    f.h_ = std::move(h);
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
    if (!h_) return 0;
    return static_cast<std::uint32_t>(stbtt_FindGlyphIndex(&h_->info, static_cast<int>(cp)));
}

GlyphBitmap Face::rasterize(std::uint32_t glyph_index) const {
    GlyphBitmap out;
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

} // namespace toe::gfx
