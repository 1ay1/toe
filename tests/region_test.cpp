// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Scroll region + origin mode (DECSTBM / DECOM / DECSLRM) conformance.
//
// These interact with nearly every cursor operation, and their edge rules are
// easy to get subtly wrong: an inverted region must be IGNORED (not clamped),
// origin mode makes CUP relative AND confines the cursor, and both resets
// have to put it all back. DECOM in particular was accepted and then ignored
// entirely, so an app using it drew in the wrong place.
//
// Rules are from DEC STD 070 and xterm's ctlseqs.

#include "toe/term/update.hpp"

#include <cstdio>
#include <string>
#include <string_view>

using namespace toe;

static int failures = 0;

static void expect(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

// Feed `bytes` into a cols x rows model and report where the cursor ended up.
static Pos cursor_after(std::string_view bytes, std::int32_t cols = 10,
                        std::int32_t rows = 6) {
    term::Config cfg;
    term::Model m{cfg, Extent{cols, rows}};
    (void)term::feed_output(m, bytes);
    return m.screen.cursor();
}

static void expect_cursor(std::string_view bytes, std::int32_t row, std::int32_t col,
                          const char *what) {
    const Pos p = cursor_after(bytes);
    if (p.row.get() != row || p.col.get() != col) {
        std::printf("FAIL %s\n       got  row=%d col=%d\n       want row=%d col=%d\n", what,
                    p.row.get(), p.col.get(), row, col);
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

// Which row holds `ch`? -1 if absent.
static std::int32_t row_of(std::string_view bytes, char ch, std::int32_t cols = 10,
                           std::int32_t rows = 6) {
    term::Config cfg;
    term::Model m{cfg, Extent{cols, rows}};
    (void)term::feed_output(m, bytes);
    for (std::int32_t r = 0; r < rows; ++r)
        for (const term::Cell &c : m.screen.row(Row{r}))
            if (c.cp == static_cast<char32_t>(ch)) return r;
    return -1;
}

int main() {
    // --- DECSTBM basics ----------------------------------------------------
    expect_cursor("\x1b[2;4r", 0, 0, "DECSTBM homes the cursor");
    // An inverted or degenerate region must be IGNORED, leaving the previous
    // region intact -- NOT clamped into something valid.
    expect(row_of("\x1b[5;2r\x1b[1;1Hx\n\n\n\n\n\n\ny", 'y') == 5,
           "DECSTBM with top > bottom is ignored (region stays full screen)");
    expect(row_of("\x1b[3;3r\x1b[1;1Hx\n\n\n\n\n\n\ny", 'y') == 5,
           "DECSTBM with top == bottom is ignored");
    // DECSTBM with no params restores the full screen.
    expect(row_of("\x1b[2;3r\x1b[r\x1b[1;1Hx\n\n\n\n\n\n\ny", 'y') == 5,
           "DECSTBM with no params restores the full screen");

    // --- scrolling respects the region -------------------------------------
    {
        // Region rows 2..4 (0-based 1..3). Filling past the bottom scrolls
        // only inside it, so row 0 and row 5 keep their content.
        term::Config cfg;
        term::Model m{cfg, Extent{10, 6}};
        (void)term::feed_output(m, "TOP\x1b[6;1HBOT\x1b[2;4r\x1b[2;1H"
                                   "a\r\nb\r\nc\r\nd\r\ne");
        bool top_kept = false, bot_kept = false;
        for (const term::Cell &c : m.screen.row(Row{0})) if (c.cp == U'T') top_kept = true;
        for (const term::Cell &c : m.screen.row(Row{5})) if (c.cp == U'B') bot_kept = true;
        expect(top_kept, "scrolling in a region leaves rows above untouched");
        expect(bot_kept, "scrolling in a region leaves rows below untouched");
    }

    // --- DECOM (origin mode) ------------------------------------------------
    // This is the bug this file was written for: DECOM was accepted and then
    // ignored, so every coordinate an app sent landed in the wrong row.
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[1;1H", 1, 0,
                  "DECOM: CUP 1;1 is the region top, not the screen top");
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[2;3H", 2, 2,
                  "DECOM: CUP row is region-relative");
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[9;1H", 3, 0,
                  "DECOM: CUP clamps to the region bottom, not the screen");
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[5d", 3, 0,
                  "DECOM: VPA is region-relative and clamped");
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[2;3f", 2, 2,
                  "DECOM: HVP (f) is region-relative too");
    // Setting or clearing DECOM homes the cursor (to the region / the screen).
    expect_cursor("\x1b[2;4r\x1b[3;3H\x1b[?6h", 1, 0, "DECOM set homes to the region");
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[3;3H\x1b[?6l", 0, 0, "DECOM reset homes to the screen");
    // With DECOM off, coordinates are absolute again.
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[?6l\x1b[1;1H", 0, 0,
                  "DECOM off: CUP 1;1 is the screen top");
    // CUU/CUD are NOT affected by DECOM (they're relative already), but they
    // must still stop at the region edge once the cursor is inside it.
    expect_cursor("\x1b[2;4r\x1b[?6h\x1b[1;1H\x1b[9A", 1, 0,
                  "CUU stops at the region top under DECOM");

    // --- DECOM must be reported and reset ----------------------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{10, 6}};
        auto ask = [&m](std::string_view q) {
            std::string r;
            for (const Cmd &c : term::feed_output(m, q))
                if (const auto *w = std::get_if<WriteChild>(&c)) r += w->bytes;
            return r;
        };
        expect(ask("\x1b[?6$p") == "\x1b[?6;2$y", "DECRQM 6 reports reset by default");
        (void)term::feed_output(m, "\x1b[?6h");
        expect(ask("\x1b[?6$p") == "\x1b[?6;1$y", "DECRQM 6 reports set");
        (void)term::feed_output(m, "\x1b[!p"); // DECSTR
        expect(ask("\x1b[?6$p") == "\x1b[?6;2$y", "DECSTR resets DECOM");
        (void)term::feed_output(m, "\x1b[?6h\033c"); // RIS
        expect(ask("\x1b[?6$p") == "\x1b[?6;2$y", "RIS resets DECOM");
    }

    // --- left/right margins (DECSLRM, needs DECLRMM) -----------------------
    // Without DECLRMM (?69h), CSI s is SCOSC (save cursor), not a margin set.
    expect_cursor("\x1b[3;5H\x1b[s\x1b[1;1H\x1b[u", 2, 4,
                  "CSI s/u is save/restore cursor when DECLRMM is off");
    {
        term::Config cfg;
        term::Model m{cfg, Extent{10, 6}};
        auto ask = [&m](std::string_view q) {
            std::string r;
            for (const Cmd &c : term::feed_output(m, q))
                if (const auto *w = std::get_if<WriteChild>(&c)) r += w->bytes;
            return r;
        };
        (void)term::feed_output(m, "\x1b[?69h\x1b[3;7s");
        expect(ask("\x1b[?69$p") == "\x1b[?69;1$y", "DECLRMM reports set");
        // Under DECOM, column 1 is the LEFT MARGIN, not column 0.
        (void)term::feed_output(m, "\x1b[?6h\x1b[1;1H");
        const Pos p = m.screen.cursor();
        expect(p.col.get() == 2, "DECOM: CUP column 1 is the left margin");
    }

    // --- rectangle attribute ops (DECCARA / DECRARA) ----------------------
    // These had NO test coverage at all, which is how DECCARA's reset form
    // ended up doing the exact opposite of what it should.
    {
        // Fill 6x4 with 'a', then DECCARA bold over rows 2-3, cols 2-4.
        term::Config cfg;
        term::Model m{cfg, Extent{6, 4}};
        (void)term::feed_output(m, "\033[1;1Haaaaaa\033[2;1Haaaaaa"
                                   "\033[3;1Haaaaaa\033[4;1Haaaaaa");
        (void)term::feed_output(m, "\033[2;2;3;4;1$r");
        auto bold_at = [&m](std::int32_t r, std::int32_t c) {
            return has(m.screen.row(Row{r})[static_cast<std::size_t>(c)].pen.attr,
                       term::Attr::Bold);
        };
        expect(bold_at(1, 1) && bold_at(1, 3) && bold_at(2, 1) && bold_at(2, 3),
               "DECCARA sets the attribute inside the rectangle");
        expect(!bold_at(0, 1) && !bold_at(3, 1) && !bold_at(1, 0) && !bold_at(1, 4),
               "DECCARA leaves cells outside the rectangle alone");
    }
    {
        // DECRARA toggles: on an all-bold screen the rectangle becomes plain.
        term::Config cfg;
        term::Model m{cfg, Extent{6, 4}};
        (void)term::feed_output(m, "\033[1m\033[1;1Haaaaaa\033[2;1Haaaaaa"
                                   "\033[3;1Haaaaaa\033[4;1Haaaaaa");
        (void)term::feed_output(m, "\033[2;2;3;4;1$t");
        auto bold_at = [&m](std::int32_t r, std::int32_t c) {
            return has(m.screen.row(Row{r})[static_cast<std::size_t>(c)].pen.attr,
                       term::Attr::Bold);
        };
        expect(!bold_at(1, 1) && !bold_at(2, 3), "DECRARA toggles the rectangle off");
        expect(bold_at(0, 1) && bold_at(1, 0), "DECRARA leaves the outside alone");
    }
    {
        // DECCARA with SGR 0 must CLEAR, not set. This was inverted: it ORed
        // the full attribute mask, so "reset this rectangle" set every
        // attribute in it instead.
        term::Config cfg;
        term::Model m{cfg, Extent{6, 3}};
        (void)term::feed_output(m, "\033[4m\033[1;1Haaaaaa\033[2;1Haaaaaa\033[3;1Haaaaaa");
        (void)term::feed_output(m, "\033[1;2;2;4;0$r");
        auto attr_at = [&m](std::int32_t r, std::int32_t c) {
            return m.screen.row(Row{r})[static_cast<std::size_t>(c)].pen.attr;
        };
        expect(attr_at(0, 1) == term::Attr::None && attr_at(1, 3) == term::Attr::None,
               "DECCARA with SGR 0 clears attributes in the rectangle");
        expect(has(attr_at(0, 0), term::Attr::Underline) &&
                   has(attr_at(2, 1), term::Attr::Underline),
               "DECCARA with SGR 0 leaves the outside underlined");
    }

    // --- left/right margins confine column-wise edits ----------------------
    // All of this was unreachable-by-test before, and all of it was broken:
    // writes outside the margins were clipped, and IL/DL moved whole rows.
    {
        term::Config cfg;
        term::Model m{cfg, Extent{8, 3}};
        // Margins cols 3-6 (0-based 2-5).
        (void)term::feed_output(m, "\033[?69h\033[3;6s");
        (void)term::feed_output(m, "\033[1;1HABCDEFGH");
        std::string row0;
        for (const term::Cell &c : m.screen.row(Row{0})) row0 += static_cast<char>(c.cp);
        expect(row0 == "ABCDEFGH",
               "a write starting outside the margins is not clipped by them");
    }
    {
        term::Config cfg;
        term::Model m{cfg, Extent{8, 3}};
        (void)term::feed_output(m, "\033[?69h\033[3;6s\033[1;3HABCDEF");
        // Starting inside, it wraps at the right margin (col 5) back to col 2.
        expect(m.screen.row(Row{0})[2].cp == U'A' && m.screen.row(Row{0})[5].cp == U'D' &&
                   m.screen.row(Row{1})[2].cp == U'E',
               "inside the margins, a write wraps at the right margin");
    }
    {
        term::Config cfg;
        term::Model m{cfg, Extent{8, 3}};
        (void)term::feed_output(m, "\033[?69h\033[3;6s");
        (void)term::feed_output(m, "\033[1;1HABCDEFGH\033[2;1Habcdefgh\033[3;1HIJKLMNOP");
        (void)term::feed_output(m, "\033[2;3H\033[1L"); // IL inside the margins
        auto at = [&m](std::int32_t r, std::int32_t c) {
            return m.screen.row(Row{r})[static_cast<std::size_t>(c)].cp;
        };
        expect(at(1, 0) == U'a' && at(1, 1) == U'b' && at(1, 6) == U'g' && at(1, 7) == U'h',
               "IL with margins leaves the columns outside them untouched");
        expect(at(1, 2) == U' ' && at(1, 5) == U' ',
               "IL with margins blanks only the margin columns");
        expect(at(2, 2) == U'c' && at(2, 5) == U'f',
               "IL with margins shifts the margin columns down");
        expect(at(2, 0) == U'I' && at(2, 7) == U'P',
               "IL with margins does not disturb the next row's outer columns");
    }
    {
        term::Config cfg;
        term::Model m{cfg, Extent{8, 3}};
        (void)term::feed_output(m, "\033[?69h\033[3;6s");
        (void)term::feed_output(m, "\033[1;1HABCDEFGH\033[2;1Habcdefgh\033[3;1HIJKLMNOP");
        (void)term::feed_output(m, "\033[2;3H\033[1M"); // DL inside the margins
        auto at = [&m](std::int32_t r, std::int32_t c) {
            return m.screen.row(Row{r})[static_cast<std::size_t>(c)].cp;
        };
        expect(at(1, 2) == U'K' && at(1, 5) == U'N', "DL with margins shifts them up");
        expect(at(1, 0) == U'a' && at(1, 7) == U'h',
               "DL with margins leaves the outer columns alone");
        expect(at(2, 2) == U' ' && at(2, 5) == U' ',
               "DL with margins blanks the margin columns of the last row");
    }
    {
        // DECBI / DECFI shift content within the margin columns only.
        term::Config cfg;
        term::Model m{cfg, Extent{8, 1}};
        (void)term::feed_output(m, "\033[?69h\033[3;6s\033[1;3HWXYZ\033[1;3H\0336");
        expect(m.screen.row(Row{0})[2].cp == U' ' && m.screen.row(Row{0})[3].cp == U'W',
               "DECBI at the left margin shifts the margin columns right");
    }
    {
        term::Config cfg;
        term::Model m{cfg, Extent{8, 1}};
        (void)term::feed_output(m, "\033[?69h\033[3;6s\033[1;3HWXYZ\033[1;6H\0339");
        expect(m.screen.row(Row{0})[2].cp == U'X' && m.screen.row(Row{0})[5].cp == U' ',
               "DECFI at the right margin shifts the margin columns left");
    }

    std::printf(failures ? "region: %d FAILURES\n" : "region: PASS\n", failures);
    return failures ? 1 : 0;
}
