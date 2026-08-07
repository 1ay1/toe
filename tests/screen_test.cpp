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

    if (failures == 0) {
        std::printf("all screen tests passed\n");
        return 0;
    }
    std::printf("%d screen test(s) failed\n", failures);
    return 1;
}
