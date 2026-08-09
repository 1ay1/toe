// SPDX-License-Identifier: LGPL-2.0-or-later
//
// box_glyphs_test — the procedural TUI glyph table must stay CORRECT and
// UNIFORM: every box-drawing / block / quadrant / braille codepoint that should
// be drawn by rects returns rects, within the budget, in-bounds, and with the
// shared light/heavy stroke weights so the whole set looks tidy. A regression
// here makes TUIs (vim splits, lazygit borders, btop graphs) look broken.

#include "toe/gfx/box_glyphs.hpp"

#include <cstdio>

using namespace toe::gfx;

static int fails = 0;
static void ck(bool ok, const char *what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++fails; }
}

// Count rects for cp and assert they're all inside the unit cell.
static int fills(char32_t cp) {
    CellRect r[kMaxFills];
    const int n = cell_fills(cp, r);
    for (int i = 0; i < n; ++i) {
        const bool inb = r[i].x >= -0.001f && r[i].y >= -0.001f &&
                         r[i].x + r[i].w <= 1.001f && r[i].y + r[i].h <= 1.001f &&
                         r[i].w > 0 && r[i].h > 0;
        if (!inb) { std::printf("  out-of-bounds rect for U+%04X\n", (unsigned)cp); ++fails; }
    }
    if (n > kMaxFills) { std::printf("  budget overflow U+%04X\n", (unsigned)cp); ++fails; }
    return n;
}

int main() {
    // Blocks + eighths.
    ck(fills(U'\u2588') == 1, "full block");
    ck(fills(U'\u2580') == 1, "upper half");
    ck(fills(U'\u258F') == 1, "left eighth");
    // All 8 block eighths present.
    for (char32_t cp = 0x2581; cp <= 0x2588; ++cp) ck(fills(cp) >= 1, "vertical eighth");
    for (char32_t cp = 0x2589; cp <= 0x258F; ++cp) ck(fills(cp) >= 1, "horizontal eighth");

    // Quadrants — every one draws something.
    for (char32_t cp = 0x2596; cp <= 0x259F; ++cp) ck(fills(cp) >= 1, "quadrant");

    // Light + heavy straight lines, corners, junctions, cross.
    ck(fills(U'\u2500') == 1, "light horiz");
    ck(fills(U'\u2501') == 1, "heavy horiz");
    ck(fills(U'\u250C') == 2, "light corner");
    ck(fills(U'\u250F') == 2, "heavy corner");
    ck(fills(U'\u253C') == 2, "light cross");
    ck(fills(U'\u254B') == 2, "heavy cross");
    ck(fills(U'\u251C') >= 2, "light tee");

    // Rounded corners are drawn as SDF arcs (asserted below), not rect fills.

    // Double lines — the classic menu set.
    ck(fills(U'\u2550') == 2, "double horiz (2 rails)");
    ck(fills(U'\u2551') == 2, "double vert (2 rails)");
    ck(fills(U'\u2554') >= 3, "double corner");
    ck(fills(U'\u256C') == 4, "double cross (4 rails)");

    // Braille — the full cell is 8 distinct dots; a few sparse ones too.
    ck(fills(U'\u28FF') == 8, "braille full = 8 dots");
    ck(fills(U'\u2801') == 1, "braille single dot");
    ck(fills(U'\u2800') == 0, "braille blank falls through");

    // A plain letter must NOT be procedural (fall through to the font).
    ck(fills(U'A') == 0, "letter A -> font");
    ck(fills(U' ') == 0, "space -> font");

    // Analytic SDF shapes: rounded corners + Powerline separators resolve to a
    // shape id (>=1) and are NOT drawn as rect fills (they take the SDF path).
    ck(cell_sdf(U'\u256D') == kSdfArcTL, "rounded TL -> SDF arc");
    ck(cell_sdf(U'\u256F') == kSdfArcBR, "rounded BR -> SDF arc");
    ck(cell_sdf(U'\uE0B0') == kSdfTriRight, "powerline solid right -> SDF tri");
    ck(cell_sdf(U'\uE0B2') == kSdfTriLeft, "powerline solid left -> SDF tri");
    ck(cell_sdf(U'\uE0B1') == kSdfArrowRight, "powerline chevron right -> SDF");
    ck(cell_sdf(U'A') == kSdfNone, "letter is not an SDF shape");
    ck(cell_sdf(U'\u2500') == kSdfNone, "straight line stays rect fill");
    // Rounded corners must be SDF-only (no double-draw as rects).
    ck(fills(U'\u256D') == 0, "rounded corner not in rect path");

    // Uniform weight sanity: light and heavy strokes are the shared constants.
    ck(kLight < kHeavy, "light thinner than heavy");

    std::printf(fails ? "\nbox glyphs: %d FAILURES\n" : "\nbox glyphs: PASS\n", fails);
    return fails ? 1 : 0;
}
