// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Face::ColorBackend — render COLOUR emoji glyphs from a CBDT/CBLC font (Noto
// Color Emoji and friends), which stb_truetype cannot load at all.
//
// CBLC is the "bitmap location" index: strikes (sizes), each with IndexSubTables
// mapping a glyph-id range to its image in CBDT. CBDT holds the images; for emoji
// the format is 17/18/19 = embedded PNG (with a small metrics header for 17/18).
// We pick the largest strike, find the glyph's PNG, decode it (decode_png), and
// scale to the target cell height. cmap (format 4/12) gives codepoint -> glyph
// id, since stb is absent for colour-bitmap fonts.

#include <algorithm>
#include <cstring>
#include <utility>

#include "toe/gfx/png.hpp"

#include "face_internal.hpp"

namespace toe::gfx {

namespace {
// Locate a table by tag -> (offset,length). `sfnt` = sfnt header offset.
[[nodiscard]] std::pair<std::size_t, std::size_t>
find_table(const ColorBE &r, std::uint32_t tag, std::size_t sfnt) {
    const std::uint16_t num = r.u16(sfnt + 4);
    for (std::uint16_t i = 0; i < num; ++i) {
        const std::size_t rec = sfnt + 12 + 16u * i;
        if (!r.ok(rec, 16)) break;
        if (r.u32(rec) == tag) return {r.u32(rec + 8), r.u32(rec + 12)};
    }
    return {0, 0};
}
} // namespace

// --- ColorBackend methods --------------------------------------------------
std::uint32_t Face::ColorBackend::glyph_of(char32_t cp) const noexcept {
    if (cmap_fmt == 12) {
        const std::uint32_t ng = r.u32(cmap + 12);
        std::size_t lo = 0, hi = ng;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2, g = cmap + 16 + 12u * mid;
            const std::uint32_t s = r.u32(g), e = r.u32(g + 4);
            if (cp < s) hi = mid;
            else if (cp > e) lo = mid + 1;
            else return r.u32(g + 8) + (static_cast<std::uint32_t>(cp) - s);
        }
        return 0;
    }
    if (cmap_fmt == 4 && cp <= 0xFFFF) {
        const std::uint16_t segX2 = r.u16(cmap + 6), segs = segX2 / 2;
        const std::size_t endB = cmap + 14, startB = endB + segX2 + 2;
        const std::size_t deltaB = startB + segX2, roB = deltaB + segX2;
        for (std::uint16_t s = 0; s < segs; ++s) {
            if (cp > r.u16(endB + 2u * s)) continue;
            const std::uint16_t st = r.u16(startB + 2u * s);
            if (cp < st) return 0;
            const std::uint16_t ro = r.u16(roB + 2u * s);
            if (ro == 0) return static_cast<std::uint16_t>(cp + r.u16(deltaB + 2u * s));
            const std::size_t gi = roB + 2u * s + ro + 2u * (cp - st);
            const std::uint16_t g = r.u16(gi);
            return g ? static_cast<std::uint16_t>(g + r.u16(deltaB + 2u * s)) : 0;
        }
    }
    return 0;
}

Face::ColorBackend::Loc Face::ColorBackend::locate(std::uint32_t gid) const noexcept {
    if (!best_strike) return {};
    const std::size_t cblc = strike_table_base_;
    const std::size_t arrayOff = cblc + r.u32(best_strike + 0); // indexSubTableArrayOffset
    const std::uint32_t nsub = r.u32(best_strike + 8);          // numberOfIndexSubTables
    for (std::uint32_t i = 0; i < nsub; ++i) {
        const std::size_t rec = arrayOff + 8u * i;
        const std::uint16_t first = r.u16(rec), last = r.u16(rec + 2);
        if (gid < first || gid > last) continue;
        const std::size_t ist = arrayOff + r.u32(rec + 4); // IndexSubTable header
        const std::uint16_t idxFmt = r.u16(ist);
        const std::uint16_t imgFmt = r.u16(ist + 2);
        const std::uint32_t imageDataOffset = r.u32(ist + 4); // from CBDT start
        const std::uint32_t k = gid - first;
        if (idxFmt == 1) { // variable 4-byte offsets
            const std::uint32_t o0 = r.u32(ist + 8 + 4u * k);
            const std::uint32_t o1 = r.u32(ist + 8 + 4u * (k + 1));
            if (o1 <= o0) return {};
            return {cbdt + imageDataOffset + o0, o1 - o0, imgFmt};
        }
        if (idxFmt == 2) { // constant image size
            const std::uint32_t sz = r.u32(ist + 8);
            return {cbdt + imageDataOffset + static_cast<std::size_t>(sz) * k, sz, imgFmt};
        }
        return {};
    }
    return {};
}

// --- construction hook + free helpers used by face.cpp ---------------------
static std::unique_ptr<Face::ColorBackend>
build_backend(const std::vector<std::uint8_t> &blob, std::size_t sfnt) {
    ColorBE r{blob.data(), blob.size()};
    const auto [cblcOff, cblcLen] = find_table(r, 0x43424c43u, sfnt); // 'CBLC'
    const auto [cbdtOff, cbdtLen] = find_table(r, 0x43424454u, sfnt); // 'CBDT'
    (void)cblcLen;
    (void)cbdtLen;
    if (!cblcOff || !cbdtOff) return nullptr;

    auto cb = std::make_unique<Face::ColorBackend>();
    cb->r = r;
    cb->cbdt = cbdtOff;
    cb->strike_table_base_ = cblcOff;

    // Largest strike by ppemX (bitmapSizeTable is 48 bytes; ppemX at +44).
    const std::uint32_t numSizes = r.u32(cblcOff + 4);
    std::size_t best = 0;
    std::uint16_t bestPpem = 0;
    for (std::uint32_t i = 0; i < numSizes; ++i) {
        const std::size_t bst = cblcOff + 8 + 48u * i;
        const std::uint16_t ppem = r.u8(bst + 44);
        if (ppem >= bestPpem) { bestPpem = ppem; best = bst; }
    }
    if (!best) return nullptr;
    cb->best_strike = best;
    cb->ppem = bestPpem ? bestPpem : 1;

    // Unicode cmap (prefer format 12).
    const auto [cmapOff, cmapLen] = find_table(r, 0x636d6170u, sfnt); // 'cmap'
    (void)cmapLen;
    if (cmapOff) {
        const std::uint16_t nsub = r.u16(cmapOff + 2);
        std::size_t s4 = 0, s12 = 0;
        for (std::uint16_t i = 0; i < nsub; ++i) {
            const std::size_t e = cmapOff + 4 + 8u * i;
            const std::uint16_t plat = r.u16(e), enc = r.u16(e + 2);
            const std::size_t sub = cmapOff + r.u32(e + 4);
            const bool uni = (plat == 0) || (plat == 3 && (enc == 1 || enc == 10));
            if (!uni) continue;
            const std::uint16_t fmt = r.u16(sub);
            if (fmt == 12) s12 = sub;
            else if (fmt == 4 && !s4) s4 = sub;
        }
        if (s12) { cb->cmap = s12; cb->cmap_fmt = 12; }
        else if (s4) { cb->cmap = s4; cb->cmap_fmt = 4; }
    }
    if (!cb->cmap) return nullptr;
    return cb;
}

std::unique_ptr<Face::ColorBackend> try_load_color_face(const std::vector<std::uint8_t> &blob) {
    if (blob.size() < 12) return nullptr;
    ColorBE r{blob.data(), blob.size()};
    std::size_t sfnt = 0;
    if (r.u32(0) == 0x74746366u) sfnt = r.u32(12); // 'ttcf' -> first sub-font
    return build_backend(blob, sfnt);
}

void color_metrics(const Face::ColorBackend &cb, FaceMetrics &m) {
    m.ascent = cb.ppem;
    m.descent = 0;
    m.line_gap = 0;
    m.advance = cb.ppem;
}

std::uint32_t color_glyph_index(const Face::ColorBackend &cb, char32_t cp) {
    return cb.glyph_of(cp);
}

GlyphBitmap color_rasterize(const Face::ColorBackend &cb, std::uint32_t gid, int target_px) {
    GlyphBitmap out;
    out.is_color = true;
    const Face::ColorBackend::Loc loc = cb.locate(gid);
    if (!loc.off || loc.len < 8) return out;

    // Formats 17/18 prefix a glyph-metrics header before the PNG; 19 is PNG
    // directly. Scan a short window for the PNG signature rather than hard-code
    // each header size.
    const std::uint8_t *base = cb.r.d + loc.off;
    const std::size_t span = loc.len;
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::size_t pngAt = span;
    for (std::size_t i = 0; i + 8 <= span && i < 64; ++i)
        if (std::memcmp(base + i, sig, 8) == 0) { pngAt = i; break; }
    if (pngAt >= span) return out;

    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    if (!decode_png(base + pngAt, span - pngAt, w, h, rgba) || w <= 0 || h <= 0) return out;

    // Scale to target height (nearest-neighbour is fine at emoji sizes).
    const int th = std::max(1, target_px);
    const int tw = std::max(1, static_cast<int>(static_cast<long long>(w) * th / h));
    out.width = tw;
    out.height = th;
    out.advance = tw;
    out.bearing_x = 0;
    out.bearing_y = th;
    out.pixels.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
    for (int y = 0; y < th; ++y) {
        const int sy = y * h / th;
        for (int x = 0; x < tw; ++x) {
            const int sx = x * w / tw;
            const std::uint8_t *sp = rgba.data() + (static_cast<std::size_t>(sy) * w + sx) * 4;
            std::uint8_t *dp = out.pixels.data() + (static_cast<std::size_t>(y) * tw + x) * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
    return out;
}

} // namespace toe::gfx
