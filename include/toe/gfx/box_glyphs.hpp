// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Procedural TUI glyphs — box-drawing, block elements, quadrants, and braille
// drawn as axis-aligned rectangles instead of font glyphs. This is what makes a
// TUI look CRISP and UNIFORM: every line is exactly the same weight, corners
// meet with no seams, blocks tile without gaps, and it all scales pixel-perfect
// with the cell — none of the hairline gaps, weight drift, or misalignment you
// get when these come from an arbitrary font.
//
// cell_fills(cp, out) writes up to kMaxFills normalized rects (0..1 within the
// cell) in the FOREGROUND colour and returns the count, or 0 to fall through to
// the font atlas. The renderer scales each rect by the cell size.
//
// Coverage (Unicode):
//   U+2500–257F  box drawing: light/heavy/double lines, all corners + junctions
//   U+2580–259F  block elements incl. eighths + quadrants
//   U+2800–28FF  braille (2×4 dot matrix) — plotting / btop-style graphs
//   U+256D–2570  rounded corners (approximated with stepped rects)

#ifndef TOE_GFX_BOX_GLYPHS_HPP
#define TOE_GFX_BOX_GLYPHS_HPP

#include <cstdint>

namespace toe::gfx {

struct CellRect { float x, y, w, h; };

// Max rects one procedural glyph can emit. Braille (8 dots) is the worst case.
inline constexpr int kMaxFills = 12;

// Stroke half/full-width fractions of the cell. Light ≈ 1/8, heavy ≈ 1/4. These
// are shared by every line-drawing glyph so the whole set has ONE uniform light
// weight and ONE uniform heavy weight — the key to a tidy TUI.
inline constexpr float kLight = 1.f / 8;
inline constexpr float kHeavy = 1.f / 4;

[[nodiscard]] inline int cell_fills(char32_t cp, CellRect out[kMaxFills]) noexcept {
    int n = 0;
    auto push = [&](float x, float y, float w, float h) {
        if (n < kMaxFills) out[n++] = {x, y, w, h};
    };

    // ─── block elements: solid fractions of the cell ────────────────────────
    switch (cp) {
    case U'\u2588': push(0, 0, 1, 1); return n;             // █ full
    case U'\u2580': push(0, 0, 1, .5f); return n;           // ▀ upper half
    case U'\u2584': push(0, .5f, 1, .5f); return n;         // ▄ lower half
    case U'\u258C': push(0, 0, .5f, 1); return n;           // ▌ left half
    case U'\u2590': push(.5f, 0, .5f, 1); return n;         // ▐ right half
    case U'\u2581': push(0, 7.f/8, 1, 1.f/8); return n;     // lower 1/8..7/8
    case U'\u2582': push(0, 6.f/8, 1, 2.f/8); return n;
    case U'\u2583': push(0, 5.f/8, 1, 3.f/8); return n;
    case U'\u2585': push(0, 3.f/8, 1, 5.f/8); return n;
    case U'\u2586': push(0, 2.f/8, 1, 6.f/8); return n;
    case U'\u2587': push(0, 1.f/8, 1, 7.f/8); return n;
    case U'\u2589': push(0, 0, 7.f/8, 1); return n;         // left 7/8..1/8
    case U'\u258A': push(0, 0, 6.f/8, 1); return n;
    case U'\u258B': push(0, 0, 5.f/8, 1); return n;
    case U'\u258D': push(0, 0, 3.f/8, 1); return n;
    case U'\u258E': push(0, 0, 2.f/8, 1); return n;
    case U'\u258F': push(0, 0, 1.f/8, 1); return n;
    // Quadrant blocks — fill every 2×2 combination so partial fills tile.
    case U'\u2596': push(0, .5f, .5f, .5f); return n;                    // ▖ lower-left
    case U'\u2597': push(.5f, .5f, .5f, .5f); return n;                  // ▗ lower-right
    case U'\u2598': push(0, 0, .5f, .5f); return n;                      // ▘ upper-left
    case U'\u2599': push(0, 0, .5f, 1); push(.5f, .5f, .5f, .5f); return n; // ▙
    case U'\u259A': push(0, 0, .5f, .5f); push(.5f, .5f, .5f, .5f); return n; // ▚
    case U'\u259B': push(0, 0, 1, .5f); push(0, .5f, .5f, .5f); return n; // ▛
    case U'\u259C': push(0, 0, 1, .5f); push(.5f, .5f, .5f, .5f); return n; // ▜
    case U'\u259D': push(.5f, 0, .5f, .5f); return n;                   // ▝ upper-right
    case U'\u259E': push(.5f, 0, .5f, .5f); push(0, .5f, .5f, .5f); return n; // ▞
    case U'\u259F': push(.5f, 0, .5f, 1); push(0, .5f, .5f, .5f); return n; // ▟
    default: break;
    }

    // ─── braille (U+2800–28FF): 2 cols × 4 rows dot matrix ──────────────────
    // Bit layout (Unicode): 1 4 / 2 5 / 3 6 / 7 8, columns left/right.
    if (cp >= 0x2800 && cp <= 0x28FF) {
        const unsigned bits = cp - 0x2800;
        // Dot centres on a 2×4 grid; dots must be small enough to stay DISTINCT
        // (row spacing is ~0.19, so keep the dot well under that).
        constexpr float ds = 0.16f;            // dot size (cell fraction)
        const float colx[2] = {0.30f, 0.70f};
        const float rowy[4] = {0.14f, 0.38f, 0.62f, 0.86f};
        // Map the 8 braille bits to (col,row).
        struct Dot { int bit, col, row; };
        static constexpr Dot dots[8] = {
            {0,0,0},{1,0,1},{2,0,2},{3,1,0},{4,1,1},{5,1,2},{6,0,3},{7,1,3}};
        for (const Dot &d : dots)
            if (bits & (1u << d.bit))
                push(colx[d.col] - ds/2, rowy[d.row] - ds/2, ds, ds);
        return n; // 0 if no dots set (U+2800 blank) — falls through, fine
    }

    // ─── box-drawing lines (U+2500–257F) from centred stubs ─────────────────
    const float t = kLight, T = kHeavy;
    const float lo = 0.5f - t/2, hlo = 0.5f - T/2; // light / heavy start
    // Full-length bars (span the whole cell so neighbours connect seamlessly).
    auto Hbar = [&](float y0, float w) { push(0, y0, 1, w); };
    auto Vbar = [&](float x0, float w) { push(x0, 0, w, 1); };
    // Half-stubs from centre to an edge (+half width so arms overlap at corners).
    auto Lst = [&](float y0, float w) { push(0, y0, 0.5f + w/2, w); };
    auto Rst = [&](float y0, float w) { push(0.5f - w/2, y0, 0.5f + w/2, w); };
    auto Ust = [&](float x0, float w) { push(x0, 0, w, 0.5f + w/2); };
    auto Dst = [&](float x0, float w) { push(x0, 0.5f - w/2, w, 0.5f + w/2); };

    switch (cp) {
    // Straight lines (light + heavy).
    case U'\u2500': Hbar(lo, t); return n;   // ─
    case U'\u2501': Hbar(hlo, T); return n;  // ━
    case U'\u2502': Vbar(lo, t); return n;   // │
    case U'\u2503': Vbar(hlo, T); return n;  // ┃
    // Dashed/dotted horizontals + verticals (2/3/4 dash, light).
    case U'\u2504': case U'\u2508': { const float s=1.f/6; for(float x=0;x<1;x+=2*s) push(x,lo,s,t); return n; } // ┄ ┈
    case U'\u2505': case U'\u2509': { const float s=1.f/6; for(float x=0;x<1;x+=2*s) push(x,hlo,s,T); return n; } // ┅ ┉ heavy
    case U'\u2506': case U'\u250A': { const float s=1.f/6; for(float y=0;y<1;y+=2*s) push(lo,y,t,s); return n; } // ┆ ┊
    case U'\u2507': case U'\u250B': { const float s=1.f/6; for(float y=0;y<1;y+=2*s) push(hlo,y,T,s); return n; } // ┇ ┋ heavy
    // Light corners.
    case U'\u250C': Dst(lo,t); Rst(lo,t); return n; // ┌
    case U'\u2510': Dst(lo,t); Lst(lo,t); return n; // ┐
    case U'\u2514': Ust(lo,t); Rst(lo,t); return n; // └
    case U'\u2518': Ust(lo,t); Lst(lo,t); return n; // ┘
    // (Rounded corners ╭╮╯╰ are drawn as true SDF arcs — see cell_sdf.)
    // Heavy corners.
    case U'\u250F': Dst(hlo,T); Rst(hlo,T); return n; // ┏
    case U'\u2513': Dst(hlo,T); Lst(hlo,T); return n; // ┓
    case U'\u2517': Ust(hlo,T); Rst(hlo,T); return n; // ┗
    case U'\u251B': Ust(hlo,T); Lst(hlo,T); return n; // ┛
    // Light T-junctions.
    case U'\u251C': Vbar(lo,t); Rst(lo,t); return n; // ├
    case U'\u2524': Vbar(lo,t); Lst(lo,t); return n; // ┤
    case U'\u252C': Hbar(lo,t); Dst(lo,t); return n; // ┬
    case U'\u2534': Hbar(lo,t); Ust(lo,t); return n; // ┴
    // Heavy T-junctions.
    case U'\u2523': Vbar(hlo,T); Rst(hlo,T); return n; // ┣
    case U'\u252B': Vbar(hlo,T); Lst(hlo,T); return n; // ┫
    case U'\u2533': Hbar(hlo,T); Dst(hlo,T); return n; // ┳
    case U'\u253B': Hbar(hlo,T); Ust(hlo,T); return n; // ┻
    // Cross (light + heavy).
    case U'\u253C': Hbar(lo,t); Vbar(lo,t); return n;  // ┼
    case U'\u254B': Hbar(hlo,T); Vbar(hlo,T); return n; // ╋
    default: break;
    }

    // ─── double-line box drawing (U+2550–256C) ──────────────────────────────
    // Two thin parallel rails, offset by ±d from centre, at light weight.
    const float d = 3.f / 16;          // rail offset from centre
    const float w2 = kLight * 0.85f;   // slightly thinner so two rails ≈ one heavy
    const float a = 0.5f - d, b = 0.5f + d; // the two rail centres
    const float av = a - w2/2, bv = b - w2/2;
    auto Hrail = [&](float yc) { push(0, yc - w2/2, 1, w2); };
    auto Vrail = [&](float xc) { push(xc - w2/2, 0, w2, 1); };
    // Half rails (stubs) for corners/junctions: from a rail's centre to an edge.
    auto HrailL = [&](float yc){ push(0, yc-w2/2, 0.5f+w2, w2); };
    auto HrailR = [&](float yc){ push(0.5f-w2, yc-w2/2, 0.5f+w2, w2); };
    auto VrailU = [&](float xc){ push(xc-w2/2, 0, w2, 0.5f+w2); };
    auto VrailD = [&](float xc){ push(xc-w2/2, 0.5f-w2, w2, 0.5f+w2); };
    (void)av; (void)bv;
    switch (cp) {
    case U'\u2550': Hrail(a); Hrail(b); return n;                      // ═
    case U'\u2551': Vrail(a); Vrail(b); return n;                      // ║
    case U'\u2554': HrailR(b); HrailR(a); VrailD(b); VrailD(a); return n; // ╔ (approx)
    case U'\u2557': HrailL(b); HrailL(a); VrailD(a); VrailD(b); return n; // ╗
    case U'\u255A': HrailR(a); HrailR(b); VrailU(b); VrailU(a); return n; // ╚
    case U'\u255D': HrailL(a); HrailL(b); VrailU(a); VrailU(b); return n; // ╝
    case U'\u2560': Vrail(a); Vrail(b); HrailR(a); HrailR(b); return n;   // ╠
    case U'\u2563': Vrail(a); Vrail(b); HrailL(a); HrailL(b); return n;   // ╣
    case U'\u2566': Hrail(a); Hrail(b); VrailD(a); VrailD(b); return n;   // ╦
    case U'\u2569': Hrail(a); Hrail(b); VrailU(a); VrailU(b); return n;   // ╩
    case U'\u256C': Hrail(a); Hrail(b); Vrail(a); Vrail(b); return n;     // ╬
    default: break;
    }

    return 0; // not procedural — use the font atlas
}

// SDF shape ids for cell_sdf() — must match the fragment shader's sdf_shape().
enum : std::uint8_t {
    kSdfNone = 0,
    kSdfTriRight = 1,   // right-pointing solid triangle (Powerline )
    kSdfTriLeft = 2,    // left-pointing solid triangle (Powerline )
    kSdfArrowRight = 3, // right chevron (Powerline )
    kSdfArrowLeft = 4,  // left chevron (Powerline )
    kSdfArcTL = 5,      // rounded corner ╭
    kSdfArcTR = 6,      // ╮
    kSdfArcBL = 7,      // ╰
    kSdfArcBR = 8,      // ╯
};

// If `cp` is drawn by an analytic SDF (Powerline separators, rounded corners),
// return its shape id (>=1); else 0. This is the resolution-independent path:
// the fragment shader evaluates the exact shape per pixel, so these glyphs are
// mathematically perfect and crisp at ANY size / zoom with zero atlas memory.
[[nodiscard]] inline std::uint8_t cell_sdf(char32_t cp) noexcept {
    switch (cp) {
    case U'\u256D': return kSdfArcTL; // ╭
    case U'\u256E': return kSdfArcTR; // ╮
    case U'\u2570': return kSdfArcBL; // ╰
    case U'\u256F': return kSdfArcBR; // ╯
    // Powerline separators (private-use area, the de-facto standard codepoints).
    case U'\uE0B0': return kSdfTriRight;   // solid right
    case U'\uE0B2': return kSdfTriLeft;    // solid left
    case U'\uE0B1': return kSdfArrowRight; // chevron right
    case U'\uE0B3': return kSdfArrowLeft;  // chevron left
    default: return kSdfNone;
    }
}

} // namespace toe::gfx

#endif // TOE_GFX_BOX_GLYPHS_HPP
