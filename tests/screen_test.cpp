// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Screen-model assertions. Drives Parser -> Screen with byte strings and
// checks the resulting grid, cursor and pen. No SDL/GL/PTY. Exit 0 == pass.

#include <cstdio>
#include <string>
#include <string_view>

#include "gvte/term/screen.hpp"
#include "gvte/vt/parser.hpp"

using namespace gvte;

namespace {

// Feed bytes through a fresh parser into `scr`.
void feed(term::Screen &scr, std::string_view bytes) {
    vt::Parser p;
    p.feed(std::span<const char>{bytes.data(), bytes.size()},
           [&](const vt::Action &a) { scr.apply(a); });
}

int failures = 0;

void expect(bool cond, const char *name) {
    if (cond) {
        std::printf("ok   %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

char32_t glyph_at(const term::Screen &s, int r, int c) {
    return s.row(Row{r})[static_cast<std::size_t>(c)].cp;
}

} // namespace

int main() {
    // Basic print advances the cursor.
    {
        term::Screen s{Extent{10, 3}};
        feed(s, "abc");
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 0, 1) == U'b' && glyph_at(s, 0, 2) == U'c',
               "print writes glyphs");
        expect(s.cursor().col.get() == 3 && s.cursor().row.get() == 0, "cursor advances");
    }

    // CR + LF.
    {
        term::Screen s{Extent{10, 3}};
        feed(s, "ab\r\nc");
        expect(glyph_at(s, 1, 0) == U'c', "lf+cr moves to next line start");
        expect(s.cursor().row.get() == 1 && s.cursor().col.get() == 1, "cursor after newline");
    }

    // CUP: ESC[2;3H places cursor at row 1, col 2 (1-based -> 0-based).
    {
        term::Screen s{Extent{10, 5}};
        feed(s, "\x1b[2;3HX");
        expect(glyph_at(s, 1, 2) == U'X', "CUP positions glyph");
    }

    // Erase in line (ESC[2K) clears the row.
    {
        term::Screen s{Extent{5, 2}};
        feed(s, "hello\r\x1b[2K");
        bool cleared = true;
        for (int c = 0; c < 5; ++c) cleared &= glyph_at(s, 0, c) == U' ';
        expect(cleared, "EL 2 clears line");
    }

    // Deferred wrap: writing past the last column wraps on the next glyph.
    {
        term::Screen s{Extent{3, 2}};
        feed(s, "abcd"); // 'abc' fills row 0; 'd' wraps to row 1
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 0, 2) == U'c', "row 0 filled");
        expect(glyph_at(s, 1, 0) == U'd', "wrap to next row");
    }

    // Scroll: fill all rows, then one more LF scrolls content up.
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "top\r\nbot\r\n"); // second \n scrolls: 'bot' -> row 0
        expect(glyph_at(s, 0, 0) == U'b', "scroll moves content up");
    }

    // SGR truecolor foreground: ESC[38;2;10;20;30m.
    {
        term::Screen s{Extent{4, 1}};
        feed(s, "\x1b[38;2;10;20;30mZ");
        const auto &cell = s.row(Row{0})[0];
        bool ok = std::holds_alternative<term::TrueColor>(cell.pen.fg);
        if (ok) {
            auto tc = std::get<term::TrueColor>(cell.pen.fg);
            ok = tc.rgb == Rgb{10, 20, 30};
        }
        expect(ok && cell.cp == U'Z', "SGR truecolor fg");
    }

    // SGR indexed + bold: ESC[1;31m.
    {
        term::Screen s{Extent{4, 1}};
        feed(s, "\x1b[1;31mR");
        const auto &cell = s.row(Row{0})[0];
        bool bold = has(cell.pen.attr, term::Attr::Bold);
        bool red = std::holds_alternative<term::IndexedColor>(cell.pen.fg) &&
                   std::get<term::IndexedColor>(cell.pen.fg).index == 1;
        expect(bold && red, "SGR bold + indexed fg");
    }

    // SGR reset returns pen to default.
    {
        term::Screen s{Extent{4, 1}};
        feed(s, "\x1b[1;31m\x1b[0mP");
        const auto &cell = s.row(Row{0})[0];
        expect(cell.pen == term::Pen{}, "SGR reset");
    }

    // Scrollback: rows that scroll off the top land in history.
    {
        term::Screen s{Extent{4, 2}}; // 2 visible rows
        feed(s, "AAA\r\nBBB\r\nCCC\r\n"); // 3 CRLFs -> 2 lines scrolled to history
        expect(s.history_rows() == 2, "history accrues scrolled lines");
        // Live view shows the newest content at the bottom.
        expect(glyph_at(s, 0, 0) == U'C', "live view bottom-anchored");
    }

    // Scroll back reveals history; scroll to bottom restores live view.
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "one\r\ntwo\r\nsix\r\n"); // history: 'one','two'; live top: 'six'
        s.scroll(1); // reveal one history row at the top of the viewport
        expect(s.scroll_offset() == 1, "scroll offset advances");
        expect(glyph_at(s, 0, 0) == U't', "scrolled view shows history (two)");
        expect(!s.cursor_visible(), "cursor hidden while scrolled back");
        s.scroll_to_bottom();
        expect(s.scroll_offset() == 0, "scroll_to_bottom resets offset");
        expect(s.cursor_visible(), "cursor visible at bottom");
    }

    // New output snaps the view back to the live bottom is a Terminal-level
    // behavior; here we assert scroll clamps to available history.
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "x\r\ny\r\nz\r\n"); // 2 history rows
        s.scroll(100); // over-scroll
        expect(s.scroll_offset() == s.history_rows(), "scroll clamps to history");
        s.scroll(-100);
        expect(s.scroll_offset() == 0, "scroll clamps to bottom");
    }

    // ICH inserts blanks at the cursor, shifting the rest right.
    {
        term::Screen s{Extent{6, 1}};
        feed(s, "abcd\x1b[3G\x1b[2@"); // cursor to col 3, insert 2 blanks
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 0, 1) == U'b', "ICH keeps prefix");
        expect(glyph_at(s, 0, 2) == U' ' && glyph_at(s, 0, 3) == U' ', "ICH inserts blanks");
        expect(glyph_at(s, 0, 4) == U'c', "ICH shifts tail right");
    }

    // DCH deletes chars at the cursor, shifting the tail left.
    {
        term::Screen s{Extent{6, 1}};
        feed(s, "abcdef\x1b[2G\x1b[2P"); // cursor col 2, delete 2
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 0, 1) == U'd', "DCH shifts tail left");
    }

    // ECH erases n chars in place (no shift).
    {
        term::Screen s{Extent{6, 1}};
        feed(s, "abcdef\x1b[2G\x1b[3X");
        expect(glyph_at(s, 0, 1) == U' ' && glyph_at(s, 0, 3) == U' ' && glyph_at(s, 0, 4) == U'e',
               "ECH erases in place");
    }

    // IL/DL within the scroll region.
    {
        term::Screen s{Extent{4, 4}};
        feed(s, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD"); // 4 rows filled
        feed(s, "\x1b[1;1H\x1b[1L");             // home, insert 1 line at top
        expect(glyph_at(s, 0, 0) == U' ', "IL blanks new top line");
        expect(glyph_at(s, 1, 0) == U'A', "IL pushes content down");
    }

    // DECSTBM scroll region confines scrolling.
    {
        term::Screen s{Extent{4, 4}};
        feed(s, "\x1b[2;3r");   // region = rows 2..3
        feed(s, "\x1b[2;1Hxx\r\nyy\r\nzz"); // fill within region, force a scroll
        // Row 0 (outside region) must be untouched (blank).
        expect(glyph_at(s, 0, 0) == U' ', "DECSTBM leaves rows above region");
    }

    // DECSC/DECRC save & restore cursor + pen.
    {
        term::Screen s{Extent{8, 4}};
        feed(s, "\x1b[3;4H\x1b[31m\x1b" "7"); // move, red, DECSC
        feed(s, "\x1b[1;1H\x1b[0m\x1b" "8Z");  // home, reset, DECRC, print Z
        expect(glyph_at(s, 2, 3) == U'Z', "DECRC restores cursor position");
        const auto &cell = s.row(Row{2})[3];
        expect(std::holds_alternative<term::IndexedColor>(cell.pen.fg), "DECRC restores pen");
    }

    if (failures == 0) {
        std::printf("all screen tests passed\n");
        return 0;
    }
    std::printf("%d screen test(s) failed\n", failures);
    return 1;
}
