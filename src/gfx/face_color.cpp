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

// COLR v0 + CPAL v0: enough to draw every emoji in Segoe UI Emoji and the other
// layered-vector fonts. COLR maps a base glyph to a run of (glyph, palette
// index) layers; CPAL holds the actual BGRA colours. We deliberately ignore
// COLR v1's gradients/transforms — v1 fonts still carry a v0 layer list, so
// this renders them correctly, just without gradient fills.
static bool build_colr(ColorBE r, std::size_t sfnt, Face::ColorBackend &cb) {
    const auto [colrOff, colrLen] = find_table(r, 0x434f4c52u, sfnt); // 'COLR'
    const auto [cpalOff, cpalLen] = find_table(r, 0x4350414cu, sfnt); // 'CPAL'
    (void)colrLen;
    (void)cpalLen;
    if (!colrOff || !cpalOff) return false;

    // COLR header: version(2) numBaseGlyphRecords(2) baseGlyphRecordsOffset(4)
    //              layerRecordsOffset(4) numLayerRecords(2)
    cb.colr_num_base = r.u16(colrOff + 2);
    cb.colr_base_recs = colrOff + r.u32(colrOff + 4);
    cb.colr_layer_recs = colrOff + r.u32(colrOff + 8);
    cb.colr_num_layers = r.u16(colrOff + 12);
    if (!cb.colr_num_base || !cb.colr_num_layers) return false;

    // CPAL header: version(2) numPaletteEntries(2) numPalettes(2)
    //              numColorRecords(2) colorRecordsArrayOffset(4)
    //              colorRecordIndices[numPalettes](2 each)
    cb.cpal_num_colors = r.u16(cpalOff + 6); // total colour records
    const std::size_t recsOff = cpalOff + r.u32(cpalOff + 8);
    const std::uint16_t firstIdx = r.u16(cpalOff + 12); // palette 0's first record
    cb.cpal_colors = recsOff + static_cast<std::size_t>(firstIdx) * 4u;
    if (!cb.cpal_num_colors) return false;

    cb.colr = colrOff;
    return true;
}

std::vector<Face::ColorBackend::Layer> Face::ColorBackend::layers_of(std::uint32_t gid) const {
    std::vector<Layer> out;
    if (!colr || gid > 0xFFFFu) return out;

    // BaseGlyphRecord[]: glyphID(2) firstLayerIndex(2) numLayers(2), sorted by
    // glyphID — so binary search rather than a scan over ~1400 emoji.
    std::uint32_t lo = 0, hi = colr_num_base;
    std::size_t rec = 0;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        const std::size_t e = colr_base_recs + 6u * mid;
        const std::uint16_t g = r.u16(e);
        if (g == gid) { rec = e; break; }
        if (g < gid) lo = mid + 1;
        else hi = mid;
    }
    if (!rec) return out; // not a colour glyph: caller draws it monochrome

    const std::uint16_t first = r.u16(rec + 2);
    const std::uint16_t n = r.u16(rec + 4);
    out.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::uint32_t li = static_cast<std::uint32_t>(first) + i;
        if (li >= colr_num_layers) break;
        // LayerRecord: glyphID(2) paletteIndex(2)
        const std::size_t le = colr_layer_recs + 4u * li;
        Layer L;
        L.gid = r.u16(le);
        const std::uint16_t pi = r.u16(le + 2);
        if (pi == 0xFFFFu) {
            // 0xFFFF means "use the text foreground colour". Encode that as
            // opaque white so the caller can tint it with the cell's fg.
            L.r = L.g = L.b = 0xFF;
            L.a = 0xFF;
        } else if (pi < cpal_num_colors) {
            // CPAL colour records are BGRA byte order.
            const std::size_t c = cpal_colors + 4u * pi;
            L.b = r.u8(c);
            L.g = r.u8(c + 1);
            L.r = r.u8(c + 2);
            L.a = r.u8(c + 3);
        }
        out.push_back(L);
    }
    return out;
}

static std::unique_ptr<Face::ColorBackend>
build_backend(const std::vector<std::uint8_t> &blob, std::size_t sfnt) {
    ColorBE r{blob.data(), blob.size()};
    const auto [cblcOff, cblcLen] = find_table(r, 0x43424c43u, sfnt); // 'CBLC'
    const auto [cbdtOff, cbdtLen] = find_table(r, 0x43424454u, sfnt); // 'CBDT'
    (void)cblcLen;
    (void)cbdtLen;

    auto cb = std::make_unique<Face::ColorBackend>();
    cb->r = r;

    // Prefer the bitmap strikes when present (they carry full-detail artwork);
    // otherwise try the layered-vector format. A font with neither is not a
    // colour face at all.
    const bool have_bitmap = cblcOff && cbdtOff;
    if (have_bitmap) {
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
    } else if (!build_colr(r, sfnt, *cb)) {
        return nullptr;
    }

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
    // Only a BITMAP face dictates its own metrics (its strike ppem is the whole
    // glyph box). A COLR face is drawn from ordinary outlines, so it keeps the
    // face's real hhea/OS2 metrics that the caller already computed — clobbering
    // them with ppem=0 would collapse the cell to nothing.
    if (!cb.is_bitmap()) return;
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
    // A layered (COLR) face has no bitmap to decode here: the caller composites
    // its outline layers via layers_of(). Returning empty tells it to do that.
    if (!cb.is_bitmap()) return out;
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
    // Area-averaging (box filter) downscale. Emoji strikes are large (often
    // 109-136px) and we shrink them to the cell (~32px); nearest-neighbour
    // would throw away ~90% of the pixels and alias badly. Averaging every
    // source texel that maps into a destination texel gives smooth, crisp
    // emoji. Colours are averaged in PREMULTIPLIED space so transparent edge
    // texels don't bleed dark halos into the result.
    for (int y = 0; y < th; ++y) {
        const int sy0 = static_cast<int>(static_cast<long long>(y) * h / th);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<long long>(y + 1) * h / th));
        for (int x = 0; x < tw; ++x) {
            const int sx0 = static_cast<int>(static_cast<long long>(x) * w / tw);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<long long>(x + 1) * w / tw));
            std::uint32_t ar = 0, ag = 0, ab = 0, aa = 0, cnt = 0;
            for (int syy = sy0; syy < sy1; ++syy) {
                const std::uint8_t *row =
                    rgba.data() + static_cast<std::size_t>(syy) * static_cast<std::size_t>(w) * 4;
                for (int sxx = sx0; sxx < sx1; ++sxx) {
                    const std::uint8_t *sp = row + static_cast<std::size_t>(sxx) * 4;
                    const std::uint32_t a = sp[3];
                    // Premultiply so translucent texels contribute proportionally.
                    ar += sp[0] * a;
                    ag += sp[1] * a;
                    ab += sp[2] * a;
                    aa += a;
                    ++cnt;
                }
            }
            std::uint8_t *dp =
                out.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(x)) * 4;
            if (cnt == 0) { dp[0] = dp[1] = dp[2] = dp[3] = 0; continue; }
            const std::uint32_t alpha = aa / cnt; // average coverage
            if (aa == 0) { dp[0] = dp[1] = dp[2] = 0; dp[3] = 0; continue; }
            // Un-premultiply the averaged colour by the summed alpha.
            dp[0] = static_cast<std::uint8_t>(ar / aa);
            dp[1] = static_cast<std::uint8_t>(ag / aa);
            dp[2] = static_cast<std::uint8_t>(ab / aa);
            dp[3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return out;
}

} // namespace toe::gfx
