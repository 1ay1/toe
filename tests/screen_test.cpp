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

// Feed bytes through a fresh parser into `scr`, discarding effects.
void feed(term::Screen &scr, std::string_view bytes) {
    vt::Parser p;
    p.feed(std::span<const char>{bytes.data(), bytes.size()},
           [&](const vt::Action &a) {
               gvte::Cmds out;
               scr.apply(a, out);
           });
}

// Feed bytes and return the concatenated WriteChild reply bytes (the terminal's
// answers to queries), so tests can assert on them directly — the TEA payoff.
std::string feed_replies(term::Screen &scr, std::string_view bytes) {
    vt::Parser p;
    std::string replies;
    p.feed(std::span<const char>{bytes.data(), bytes.size()}, [&](const vt::Action &a) {
        gvte::Cmds out;
        scr.apply(a, out);
        for (const gvte::Cmd &c : out) {
            if (const auto *w = std::get_if<gvte::WriteChild>(&c)) {
                replies += w->bytes;
            }
        }
    });
    return replies;
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

    // DECAWM off (?7l): glyphs past the right margin overwrite the last column
    // instead of wrapping.
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "\x1b[?7l"); // autowrap off
        feed(s, "abcdef");   // 4-wide row; d/e/f pile onto the last column
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 0, 3) == U'f', "DECAWM off: no wrap");
        expect(glyph_at(s, 1, 0) == U' ', "DECAWM off: row 1 untouched");
        feed(s, "\x1b[?7h\x1b[2J\x1b[H"); // autowrap on, clear
        feed(s, "abcde");                  // now wraps: 'e' -> row 1
        expect(glyph_at(s, 1, 0) == U'e', "DECAWM on: wraps again");
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

    // DECTCEM: CSI ?25 l hides the cursor, h shows it.
    {
        term::Screen s{Extent{4, 2}};
        expect(s.cursor_shown(), "cursor shown by default");
        feed(s, "\x1b[?25l");
        expect(!s.cursor_shown(), "?25l hides cursor");
        feed(s, "\x1b[?25h");
        expect(s.cursor_shown(), "?25h shows cursor");
    }

    // Alternate screen (?1049): content is swapped out and restored on exit.
    {
        term::Screen s{Extent{6, 2}};
        feed(s, "primary");                 // 'primar' fills row 0, 'y' wraps
        feed(s, "\x1b[?1049h");             // enter alt screen (blank)
        expect(s.on_alt_screen(), "?1049h enters alt screen");
        expect(glyph_at(s, 0, 0) == U' ', "alt screen starts blank");
        feed(s, "\x1b[HALT");               // draw on alt screen
        expect(glyph_at(s, 0, 0) == U'A', "alt screen accepts drawing");
        feed(s, "\x1b[?1049l");             // leave -> primary restored
        expect(!s.on_alt_screen(), "?1049l leaves alt screen");
        expect(glyph_at(s, 0, 0) == U'p', "primary content restored");
    }

    // Bracketed paste + mouse mode flags.
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "\x1b[?2004h");
        expect(s.bracketed_paste(), "?2004h enables bracketed paste");
        feed(s, "\x1b[?1000h\x1b[?1006h");
        expect(s.mouse_mode() == term::Screen::MouseMode::normal && s.mouse_sgr(),
               "?1000h + ?1006h set mouse tracking");
        feed(s, "\x1b[?1000l");
        expect(s.mouse_mode() == term::Screen::MouseMode::off, "?1000l disables mouse");
    }

    // Selection: character mode spanning part of one row.
    {
        term::Screen s{Extent{10, 2}};
        feed(s, "hello world"); // 'hello worl' row0, 'd' wraps to row1
        using AbsPos = term::Screen::AbsPos;
        s.selection_begin(AbsPos{0, 0}, term::Screen::SelectMode::character);
        s.selection_extend(AbsPos{0, 4}); // select 'hello'
        expect(s.has_selection(), "selection active");
        expect(s.is_selected(0, 2) && !s.is_selected(0, 5), "char selection bounds");
        expect(s.selected_text() == "hello", "char selection text");
    }

    // Selection: line mode grabs the whole row (trailing blanks trimmed).
    {
        term::Screen s{Extent{10, 2}};
        feed(s, "abc\r\ndef");
        using AbsPos = term::Screen::AbsPos;
        s.selection_begin(AbsPos{0, 1}, term::Screen::SelectMode::line);
        expect(s.selected_text() == "abc", "line selection trims blanks");
        expect(s.is_selected(0, 9), "line selection spans full width");
    }

    // Selection: block mode is a rectangular column range.
    {
        term::Screen s{Extent{6, 3}};
        feed(s, "12345\r\nabcde\r\nZZZZZ");
        using AbsPos = term::Screen::AbsPos;
        s.selection_begin(AbsPos{0, 1}, term::Screen::SelectMode::block);
        s.selection_extend(AbsPos{1, 3}); // cols 1..3, rows 0..1
        expect(s.is_selected(0, 2) && s.is_selected(1, 3) && !s.is_selected(2, 2),
               "block selection rectangle");
        expect(s.selected_text() == "234\nbcd", "block selection text");
    }

    // Clearing a selection.
    {
        term::Screen s{Extent{4, 1}};
        feed(s, "test");
        s.selection_begin(term::Screen::AbsPos{0, 0}, term::Screen::SelectMode::line);
        s.selection_clear();
        expect(!s.has_selection() && s.selected_text().empty(), "selection cleared");
    }

    // Double-click word selection expands over word chars.
    {
        term::Screen s{Extent{20, 1}};
        feed(s, "foo bar/baz qux");
        s.selection_word(term::Screen::AbsPos{0, 5}); // inside 'bar/baz'
        expect(s.selected_text() == "bar/baz", "word select spans word punctuation");
        s.selection_word(term::Screen::AbsPos{0, 1}); // inside 'foo'
        expect(s.selected_text() == "foo", "word select stops at space");
    }

    // Triple-click / line selection grabs the whole row.
    {
        term::Screen s{Extent{20, 1}};
        feed(s, "hello there");
        s.selection_line(term::Screen::AbsPos{0, 3});
        expect(s.selected_text() == "hello there", "line select grabs whole row");
    }

    // Custom tab stops: HTS sets, TBC clears, tab honors them.
    {
        term::Screen s{Extent{40, 1}};
        // Clear all default stops, set one at column 5, then TAB from col 0.
        feed(s, "\x1b[3g");            // TBC 3: clear all
        feed(s, "\x1b[6G\x1bH");       // move to col 6 (1-based), HTS sets stop there
        feed(s, "\x1b[1G\tX");         // home, TAB, print X
        expect(glyph_at(s, 0, 5) == U'X', "TAB honors custom stop at col 5");
    }

    // Device queries must be answered (fish blocks 10s on an unanswered DA1).
    // With TEA, replies are WriteChild Cmds we read straight off feed_replies.
    {
        term::Screen s{Extent{80, 24}};
        expect(feed_replies(s, "\x1b[c") == "\x1b[?62;1;6;22c",
               "DA1 primary device attributes reply");
        expect(feed_replies(s, "\x1b[>c") == "\x1b[>1;95;0c",
               "DA2 secondary device attributes reply");
        expect(feed_replies(s, "\x1b[5n") == "\x1b[0n", "DSR status reply (terminal OK)");
        // CPR: move cursor to row 3 col 7 (1-based CUP), then request position.
        expect(feed_replies(s, "\x1b[3;7H\x1b[6n") == "\x1b[3;7R",
               "CPR cursor position reply (1-based)");
        expect(feed_replies(s, "\x1b[>0q") == "\x1bP>|gvte(0.1)\x1b\\", "XTVERSION reply (DCS)");
        // XTGETTCAP for 'Co' (colours): 436f hex. Reply advertises 256.
        expect(feed_replies(s, "\x1bP+q436f\x1b\\") == "\x1bP1+r436F=323536\x1b\\",
               "XTGETTCAP Co -> 256");
    }

    // CSI ? u (Kitty keyboard query) must NOT be treated as DECRC restore-cursor
    // — that bug homed the cursor and made fish wipe its greeting.
    {
        term::Screen s{Extent{80, 24}};
        feed(s, "\x1b[10;5H"); // move cursor to row 10, col 5 (1-based)
        const std::string reply = feed_replies(s, "\x1b[?u"); // Kitty keyboard query
        expect(s.cursor().row.get() == 9 && s.cursor().col.get() == 4,
               "CSI ?u does not move the cursor");
        expect(reply == "\x1b[?0u", "CSI ?u replies with flags=0");
        // Plain CSI u (DECRC) still restores a saved cursor.
        feed(s, "\x1b[3;3H\x1b" "7\x1b[20;20H\x1b[u"); // move, DECSC via ESC7, move, DECRC
        expect(s.cursor().row.get() == 2 && s.cursor().col.get() == 2,
               "plain CSI u still restores cursor");
    }

    // Wide (CJK) characters occupy two cells: a width-2 lead + a spacer.
    {
        term::Screen s{Extent{10, 2}};
        feed(s, "\xe4\xb8\xad" "X"); // U+4E2D (中) then 'X'
        auto row = s.row(Row{0});
        expect(row[0].cp == 0x4e2d && row[0].width == 2, "wide char is width 2");
        expect(row[1].width == 0 && row[1].spacer(), "wide char trailing cell is a spacer");
        expect(row[2].cp == U'X', "next glyph lands after the wide char");
        expect(s.cursor().col.get() == 3, "cursor advanced by 2+1");
    }

    // A combining mark (width 0) does not advance the cursor.
    {
        term::Screen s{Extent{10, 2}};
        feed(s, "e\xcc\x81"); // 'e' + U+0301 combining acute
        expect(s.row(Row{0})[0].cp == U'e', "base glyph placed");
        expect(s.cursor().col.get() == 1, "combining mark does not advance");
    }

    // A wide char at the last column wraps to the next line first.
    {
        term::Screen s{Extent{3, 2}};
        feed(s, "ab\xe4\xb8\xad"); // cols: a b, then 中 can't fit at col 2 -> wraps
        expect(s.row(Row{1})[0].cp == 0x4e2d, "wide char wraps when it won't fit");
    }

    // Background Color Erase (BCE): erase ops fill with the CURRENT background,
    // not the default. This is how htop's meter bars / TUI colour fills work.
    {
        auto bg_idx = [](const term::Cell &c) -> int {
            if (auto *ic = std::get_if<term::IndexedColor>(&c.pen.bg)) return ic->index;
            return -1; // default or truecolor
        };
        // ECH: set bg to index 4, erase 5 chars, they must carry bg=4.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[44m\x1b[5X"); // SGR bg=blue(idx4), ECH 5
            expect(bg_idx(s.row(Row{0})[0]) == 4 && bg_idx(s.row(Row{0})[4]) == 4,
                   "ECH fills erased cells with current bg (BCE)");
            expect(s.row(Row{0})[0].cp == U' ', "ECH-erased cell is blank");
        }
        // EL to end of line carries the bg.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[41mhi\x1b[0K"); // bg=red(idx1), 'hi', erase to EOL
            expect(bg_idx(s.row(Row{0})[10]) == 1, "EL fills with current bg (BCE)");
        }
        // A default background after reset is NOT indexed (default), so a plain
        // erase does not paint a stray colour.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[41m\x1b[0m\x1b[5X"); // set red then reset then erase
            expect(bg_idx(s.row(Row{0})[0]) == -1,
                   "erase after SGR reset uses default bg");
        }
        // Scroll-blank uses BCE too: fill bg, scroll the region up, the new
        // bottom line inherits the bg.
        {
            term::Screen s{Extent{10, 4}};
            feed(s, "\x1b[42m\x1b[4S"); // bg=green(idx2), scroll up 4 (whole screen)
            expect(bg_idx(s.row(Row{3})[0]) == 2, "scroll-blank uses current bg (BCE)");
        }
    }

    // The alternate screen has NO scrollback: a full-screen app (htop, vim)
    // repaints by scrolling, and those lines must never leak into history —
    // otherwise the app's frames are left behind after it exits.
    {
        term::Screen s{Extent{10, 4}};
        // Put a few lines of "shell" output on the primary screen.
        feed(s, "one\r\ntwo\r\nthree\r\n");
        const std::int32_t hist_before = s.history_rows();
        // Enter the alternate screen (as htop does: CSI ?1049h).
        feed(s, "\x1b[?1049h");
        expect(s.on_alt_screen(), "CSI ?1049h enters the alt screen");
        // The app scrolls the full screen many times (its repaint loop).
        for (int i = 0; i < 50; ++i) feed(s, "\x1b[10S"); // SU 10 == whole screen
        feed(s, "line\r\nline\r\nline\r\nline\r\n");      // and normal newlines
        expect(s.history_rows() == hist_before,
               "alt-screen scrolling does not grow the scrollback");
        // Leave the alt screen (CSI ?1049l) — primary is restored, history intact.
        feed(s, "\x1b[?1049l");
        expect(!s.on_alt_screen(), "CSI ?1049l leaves the alt screen");
        expect(s.history_rows() == hist_before,
               "leaving the alt screen leaves the scrollback unchanged");
        // The restored primary still shows the pre-htop content.
        expect(s.row(Row{0})[0].cp == U'o' && s.row(Row{0})[1].cp == U'n',
               "primary buffer restored after leaving alt screen");
    }

    if (failures == 0) {
        std::printf("all screen tests passed\n");
        return 0;
    }
    std::printf("%d screen test(s) failed\n", failures);
    return 1;
}
