// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Screen-model assertions. Drives Parser -> Screen with byte strings and
// checks the resulting grid, cursor and pen. No SDL/GL/PTY. Exit 0 == pass.

#include <cstdio>
#include <string>
#include <string_view>

#include "toe/term/screen.hpp"
#include "toe/term/update.hpp"
#include "toe/vt/parser.hpp"

using namespace toe;

namespace {

// Feed bytes through a fresh parser into `scr`, discarding effects.
void feed(term::Screen &scr, std::string_view bytes) {
    vt::Parser p;
    p.feed(std::span<const char>{bytes.data(), bytes.size()},
           [&](const vt::Action &a) {
               toe::Cmds out;
               scr.apply(a, out);
           });
}

// Feed bytes and return the concatenated WriteChild reply bytes (the terminal's
// answers to queries), so tests can assert on them directly — the TEA payoff.
std::string feed_replies(term::Screen &scr, std::string_view bytes) {
    vt::Parser p;
    std::string replies;
    p.feed(std::span<const char>{bytes.data(), bytes.size()}, [&](const vt::Action &a) {
        toe::Cmds out;
        scr.apply(a, out);
        for (const toe::Cmd &c : out) {
            if (const auto *w = std::get_if<toe::WriteChild>(&c)) {
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
        expect(feed_replies(s, "\x1b[c") == "\x1b[?62;1;4;6;22c",
               "DA1 primary device attributes reply");
        expect(feed_replies(s, "\x1b[>c") == "\x1b[>1;95;0c",
               "DA2 secondary device attributes reply");
        expect(feed_replies(s, "\x1b[5n") == "\x1b[0n", "DSR status reply (terminal OK)");
        // CPR: move cursor to row 3 col 7 (1-based CUP), then request position.
        expect(feed_replies(s, "\x1b[3;7H\x1b[6n") == "\x1b[3;7R",
               "CPR cursor position reply (1-based)");
        expect(feed_replies(s, "\x1b[>0q") == "\x1bP>|toe(0.1)\x1b\\", "XTVERSION reply (DCS)");
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

    // Copying a selection that spans a wide char must NOT emit the spacer as a
    // spurious space — the lead cell already carries the full glyph.
    {
        term::Screen s{Extent{10, 2}};
        feed(s, "a\xe4\xb8\xad" "b"); // 'a', 中 (wide), 'b'
        using AbsPos = term::Screen::AbsPos;
        s.selection_begin(AbsPos{0, 0}, term::Screen::SelectMode::character);
        s.selection_extend(AbsPos{0, 3}); // a, 中(lead), spacer, b
        expect(s.selected_text() == "a\xe4\xb8\xad" "b",
               "wide-char copy skips the spacer cell (no spurious space)");
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

    // DEC Special Graphics charset (VT100 line drawing): ESC ( 0 makes ASCII
    // l/q/k/x/j map to box-drawing glyphs — how tmux/dialog/mc draw borders.
    {
        term::Screen s{Extent{20, 3}};
        feed(s, "\x1b(0lqk\x1b(B"); // enter DEC graphics, draw top border, back to ASCII
        auto row = s.row(Row{0});
        expect(row[0].cp == 0x250C, "'l' -> upper-left corner (U+250C)");
        expect(row[1].cp == 0x2500, "'q' -> horizontal line (U+2500)");
        expect(row[2].cp == 0x2510, "'k' -> upper-right corner (U+2510)");
        // After ESC ( B, ASCII is literal again.
        feed(s, "abc");
        expect(s.row(Row{0})[3].cp == U'a', "ESC ( B restores literal ASCII");
    }
    // SO/SI shift between G1 (line drawing) and G0 (ASCII).
    {
        term::Screen s{Extent{20, 3}};
        feed(s, "\x1b)0");                  // designate G1 = DEC graphics
        feed(s, "a\x0e" "q\x0f" "b");        // 'a', SO, 'q'->line, SI, 'b'
        auto row = s.row(Row{0});
        expect(row[0].cp == U'a', "before SO: literal 'a'");
        expect(row[1].cp == 0x2500, "after SO (G1 graphics): 'q' -> line");
        expect(row[2].cp == U'b', "after SI (back to G0 ascii): literal 'b'");
    }

    // Underline styles via SGR 4 and its 4:N colon sub-parameters, plus the
    // 21 (double) form and 24 (off). Curly/dotted/dashed need colon subparams,
    // which exercise the parser's ':' handling.
    {
        auto ul = [](const term::Cell &c) { return static_cast<int>(c.pen.underline); };
        auto has_ul = [](const term::Cell &c) {
            return term::has(c.pen.attr, term::Attr::Underline);
        };
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[4mA\x1b[4:3mB\x1b[4:5mC\x1b[21mD\x1b[24mE");
            auto r = s.row(Row{0});
            expect(has_ul(r[0]) && ul(r[0]) == 1, "SGR 4 -> single underline");
            expect(has_ul(r[1]) && ul(r[1]) == 3, "SGR 4:3 -> curly underline");
            expect(has_ul(r[2]) && ul(r[2]) == 5, "SGR 4:5 -> dashed underline");
            expect(has_ul(r[3]) && ul(r[3]) == 2, "SGR 21 -> double underline");
            expect(!has_ul(r[4]), "SGR 24 turns underline off");
        }
        // 4:0 turns the underline off via the subparam form.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[4mX\x1b[4:0mY");
            expect(has_ul(s.row(Row{0})[0]), "4 on");
            expect(!has_ul(s.row(Row{0})[1]), "4:0 off");
        }
        // A truecolor SGR with COLON separators (38:2:r:g:b) must still parse
        // — the ':' handling can't break the color path.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[38:2:10:20:30mZ");
            auto &cell = s.row(Row{0})[0];
            bool tc = false;
            if (auto *t = std::get_if<term::TrueColor>(&cell.pen.fg))
                tc = t->rgb == Rgb{10, 20, 30};
            expect(tc && cell.cp == U'Z', "38:2:r:g:b colon truecolor parses");
        }
        // Underline colour (SGR 58) is stored separately from fg; 59 resets it.
        {
            term::Screen s{Extent{20, 2}};
            feed(s, "\x1b[4;58;2;200;50;50mA\x1b[59mB");
            auto &a = s.row(Row{0})[0];
            bool uc = false;
            if (auto *t = std::get_if<term::TrueColor>(&a.pen.underline_color))
                uc = t->rgb == Rgb{200, 50, 50};
            expect(uc, "SGR 58;2;r;g;b sets a distinct underline colour");
            expect(std::holds_alternative<term::DefaultColor>(a.pen.fg),
                   "58 does not touch the fg colour");
            auto &b = s.row(Row{0})[1];
            expect(std::holds_alternative<term::DefaultColor>(b.pen.underline_color),
                   "SGR 59 resets the underline colour to default");
        }
    }

    // OSC 8 hyperlinks: glyphs written while a link is open carry its URI,
    // reachable via link_at(). Closing the link (empty URI) stops stamping.
    {
        term::Screen s{Extent{20, 2}};
        s.set_hyperlink("id=1", "https://example.com");
        feed(s, "link");
        s.set_hyperlink({}, {}); // close
        feed(s, "X");
        expect(s.link_at(0, 0) == "https://example.com", "cell under an open link carries its URI");
        expect(s.link_at(0, 3) == "https://example.com", "whole link run is clickable");
        expect(s.link_at(0, 4).empty(), "text after the link has no URI");
        expect(s.link_at(1, 0).empty(), "unlinked cell returns empty");
        // Re-opening the SAME uri reuses the id (a link split across writes
        // stays one region).
        s.set_hyperlink("id=2", "https://a.test");
        feed(s, "\r\nAB");
        expect(s.link_at(1, 0) == "https://a.test" && s.link_at(1, 1) == "https://a.test",
               "second link stamps its own URI");
        // Hover tracking: moving onto a link cell reports a change; moving off
        // reports a change; staying on the same link reports none.
        expect(s.set_hover(0, 0), "hovering a link cell registers a change");
        expect(s.hover_link() != 0, "a link id is active while hovered");
        expect(!s.set_hover(0, 1), "staying within the same link: no change");
        expect(s.set_hover(0, 4), "moving off the link registers a change");
        expect(s.hover_link() == 0, "no link hovered off the link run");
    }

    // DEC 2026 synchronized output: while active, the reported damage counter
    // is frozen so the host doesn't draw a partial frame; it jumps once when
    // the batch ends, presenting the whole update atomically.
    {
        term::Screen s{Extent{20, 3}};
        feed(s, "before");
        const std::uint64_t g0 = s.generation();
        feed(s, "\x1b[?2026h"); // begin synchronized update
        feed(s, "aaa\r\nbbb\r\nccc"); // several mutations mid-batch
        expect(s.generation() == g0, "generation frozen during synchronized output");
        feed(s, "\x1b[?2026l"); // end batch
        expect(s.generation() != g0, "generation jumps once when the batch ends");
    }

    // Kitty graphics (APC): transmit+display an RGBA image, then delete it.
    {
        term::Screen s{Extent{40, 10}};
        s.set_cell_size(8, 16);
        // 2x2 RGBA (red,green / blue,white), displayed at the cursor.
        auto b64 = [](const std::string &in) {
            static const char *a =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            unsigned val = 0;
            int bits = 0;
            for (unsigned char c : in) {
                val = (val << 8) | c;
                bits += 8;
                while (bits >= 6) { bits -= 6; out += a[(val >> bits) & 0x3F]; }
            }
            if (bits) out += a[(val << (6 - bits)) & 0x3F];
            while (out.size() % 4) out += '=';
            return out;
        };
        std::string pix;
        for (int k = 0; k < 4; ++k) { pix += char(255); pix += char(0); pix += char(0); pix += char(255); }
        std::string apc = "\x1b_Ga=T,f=32,s=2,v=2;";
        apc += b64(pix);
        apc += '\x1b';
        apc += '\\';
        feed(s, apc);
        expect(s.graphics().placements().size() == 1, "kitty a=T creates a placement");
        if (!s.graphics().placements().empty()) {
            const auto &pl = s.graphics().placements()[0];
            const auto *img = s.graphics().image(pl.image_id);
            expect(img && img->width == 2 && img->height == 2, "image decoded to 2x2");
            expect(img && img->rgba.size() == 16 && img->rgba[0] == 255 && img->rgba[1] == 0,
                   "image RGBA pixels intact (first = red)");
        }
        std::string del = "\x1b_Ga=d";
        del += '\x1b';
        del += '\\';
        feed(s, del);
        expect(s.graphics().placements().empty(), "kitty a=d removes the placement");
    }

    // Sixel graphics (DCS q ... ST): decode a small image with a defined colour,
    // run-length and a band newline.
    {
        term::Screen s{Extent{40, 10}};
        s.set_cell_size(8, 16);
        std::string six = "\x1bPq";        // DCS q
        six += "#0;2;100;0;0";              // define colour 0 = red (RGB 0..100)
        six += "#0!4~";                     // colour 0, 4 full 6px columns
        six += "-";                         // next band (down 6px)
        six += "#0!4~";                     // another 6px band
        six += "\x1b";
        six += "\\";                        // ST
        feed(s, six);
        expect(s.graphics().placements().size() == 1, "sixel DCS creates a placement");
        if (!s.graphics().placements().empty()) {
            const auto *img = s.graphics().image(s.graphics().placements()[0].image_id);
            expect(img && img->width == 4 && img->height == 12, "sixel decodes to 4x12");
            expect(img && img->rgba[0] == 255 && img->rgba[1] == 0 && img->rgba[2] == 0,
                   "sixel colour definition decoded to red");
        }
        // Clearing the whole screen (ED 2) removes inline images.
        feed(s, "\x1b[2J");
        expect(s.graphics().placements().empty(), "ED 2 clears inline images");
    }

    // Kitty animation (a=f frames): the current frame advances on a tick.
    {
        term::Screen s{Extent{40, 10}};
        s.set_cell_size(8, 16);
        auto b64 = [](const std::string &in) {
            static const char *a =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out; unsigned val = 0; int bits = 0;
            for (unsigned char c : in) { val = (val << 8) | c; bits += 8;
                while (bits >= 6) { bits -= 6; out += a[(val >> bits) & 0x3F]; } }
            if (bits) out += a[(val << (6 - bits)) & 0x3F];
            while (out.size() % 4) out += '=';
            return out;
        };
        auto px = [](int r, int g, int bl) { std::string p;
            for (int i = 0; i < 4; ++i) { p += char(r); p += char(g); p += char(bl); p += char(255); }
            return p; };
        auto apc = [&](std::string ctl, std::string data) {
            std::string seq = "\x1b_G" + ctl + ";" + data; seq += '\x1b'; seq += '\\';
            feed(s, seq);
        };
        apc("i=5,a=T,f=32,s=2,v=2", b64(px(255, 0, 0)));      // base red, displayed
        apc("i=5,a=f,f=32,s=2,v=2,r=50", b64(px(0, 255, 0)));  // frame green
        const auto *img = s.graphics().image(5);
        expect(img && img->frames.size() == 2, "a=f appends an animation frame");
        s.tick_animations(0);   // start the clock
        s.tick_animations(60);  // gap 50ms elapsed -> advance
        const auto *img2 = s.graphics().image(5);
        expect(img2 && img2->current == 1 && img2->rgba[1] == 255,
               "animation advances to the next frame after its gap");
    }

    // Focus reporting (DEC 1004): report_focus() emits CSI I / CSI O only when
    // the app enabled it.
    {
        term::Screen s{Extent{20, 3}};
        expect(s.report_focus(true).empty(), "no focus report when 1004 is off");
        feed(s, "\x1b[?1004h");
        expect(s.report_focus(true) == "\x1b[I", "focus-in -> CSI I when 1004 on");
        expect(s.report_focus(false) == "\x1b[O", "focus-out -> CSI O when 1004 on");
        feed(s, "\x1b[?1004l");
        expect(s.report_focus(true).empty(), "no focus report after 1004 off");
    }

    // XTWINOPS size reports (apps use these for image sizing / layout).
    {
        term::Screen s{Extent{80, 24}};
        s.set_cell_size(8, 16);
        expect(feed_replies(s, "\x1b[16t") == "\x1b[6;16;8t", "CSI 16 t -> cell size in px");
        expect(feed_replies(s, "\x1b[14t") == "\x1b[4;384;640t",
               "CSI 14 t -> text area px (24*16 x 80*8)");
        expect(feed_replies(s, "\x1b[18t") == "\x1b[8;24;80t", "CSI 18 t -> size in cells");
    }

    // Kitty Unicode placeholder (U+10EEEE): the image is transmitted with U=1
    // (no placement), then the placeholder char is printed with the image id in
    // the fg colour. The model just stores the cell + colour; the renderer
    // tiles the image across the placeholder block.
    {
        term::Screen s{Extent{20, 3}};
        s.set_cell_size(8, 16);
        // Transmit 2x2 image id=1, U=1 (no display).
        auto b64 = [](const std::string &in) {
            static const char *a =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out; unsigned val = 0; int bits = 0;
            for (unsigned char c : in) {
                val = (val << 8) | c;
                bits += 8;
                while (bits >= 6) { bits -= 6; out += a[(val >> bits) & 0x3F]; }
            }
            if (bits) out += a[(val << (6 - bits)) & 0x3F];
            while (out.size() % 4) out += '=';
            return out;
        };
        std::string px;
        for (int i = 0; i < 4; ++i) { px += char(0); px += char(255); px += char(255); px += char(255); }
        std::string t = "\x1b_Gi=1,a=t,U=1,f=32,s=2,v=2;" + b64(px);
        t += '\x1b'; t += '\\';
        feed(s, t);
        expect(s.graphics().has_images() && s.graphics().placements().empty(),
               "U=1 transmit stores the image with no placement");
        // Set fg = truecolor 0;0;1 (image id 1), print the placeholder char.
        feed(s, "\x1b[38;2;0;0;1m\xF4\x8E\xBB\xAE");
        auto row = s.row(Row{0});
        expect(row[0].cp == 0x10EEEE, "placeholder char (U+10EEEE) stored in the cell");
        bool id_ok = false;
        if (auto *tc = std::get_if<term::TrueColor>(&row[0].pen.fg))
            id_ok = tc->rgb == Rgb{0, 0, 1};
        expect(id_ok, "image id encoded in the placeholder cell's fg colour");
    }

    // --- DECSCUSR: cursor shape + blink ------------------------------------
    {
        term::Screen s{Extent{10, 3}};
        expect(s.cursor_style().shape == term::Screen::CursorShape::block &&
                   s.cursor_style().blink,
               "default cursor is a blinking block");
        feed(s, "\x1b[4 q"); // steady underline
        expect(s.cursor_style().shape == term::Screen::CursorShape::underline &&
                   !s.cursor_style().blink,
               "CSI 4 SP q -> steady underline");
        feed(s, "\x1b[5 q"); // blinking bar
        expect(s.cursor_style().shape == term::Screen::CursorShape::bar &&
                   s.cursor_style().blink,
               "CSI 5 SP q -> blinking bar");
        feed(s, "\x1b[0 q"); // reset -> blinking block
        expect(s.cursor_style().shape == term::Screen::CursorShape::block,
               "CSI 0 SP q -> block");
    }

    // --- DECRQSS: report settings ------------------------------------------
    {
        term::Screen s{Extent{20, 6}};
        feed(s, "\x1b[3 q"); // blinking underline -> Ps 3
        expect(feed_replies(s, "\x1bP$qq\x1b\\") == "\x1bP1$r3 q\x1b\\",
               "DECRQSS q -> current DECSCUSR (3 q)");

        feed(s, "\x1b[2;5r"); // DECSTBM rows 2..5
        expect(feed_replies(s, "\x1bP$qr\x1b\\") == "\x1bP1$r2;5r\x1b\\",
               "DECRQSS r -> scroll region (2;5r)");

        feed(s, "\x1b[1;4m"); // bold + underline
        {
            const std::string sgr = feed_replies(s, "\x1bP$qm\x1b\\");
            expect(sgr == "\x1bP1$r0;1;4m\x1b\\", "DECRQSS m -> active SGR (0;1;4m)");
        }

        expect(feed_replies(s, "\x1bP$qZZ\x1b\\") == "\x1bP0$r\x1b\\",
               "DECRQSS unknown -> invalid (0$r)");
    }

    // --- DECRQM: report mode state -----------------------------------------
    {
        term::Screen s{Extent{10, 3}};
        feed(s, "\x1b[?25l");  // hide cursor
        expect(feed_replies(s, "\x1b[?25$p") == "\x1b[?25;2$y",
               "DECRQM ?25 reports reset (2) when cursor hidden");
        feed(s, "\x1b[?25h");  // show cursor
        expect(feed_replies(s, "\x1b[?25$p") == "\x1b[?25;1$y",
               "DECRQM ?25 reports set (1) when cursor shown");
        feed(s, "\x1b[?2004h"); // bracketed paste on
        expect(feed_replies(s, "\x1b[?2004$p") == "\x1b[?2004;1$y",
               "DECRQM ?2004 reports set for bracketed paste");
        expect(feed_replies(s, "\x1b[?9999$p") == "\x1b[?9999;0$y",
               "DECRQM unknown mode -> unrecognized (0)");
    }

    // --- Kitty keyboard protocol: flag stack -------------------------------
    {
        term::Screen s{Extent{10, 3}};
        expect(feed_replies(s, "\x1b[?u") == "\x1b[?0u", "kitty query: base flags = 0");
        feed(s, "\x1b[>5u"); // push flags 5 (disambiguate|alternate)
        expect(s.kitty_keyboard_flags() == 5, "kitty push sets active flags");
        expect(feed_replies(s, "\x1b[?u") == "\x1b[?5u", "kitty query reflects pushed flags");
        feed(s, "\x1b[=2;2u"); // OR in flag 2 (report events)
        expect(s.kitty_keyboard_flags() == 7, "kitty set mode 2 (or) merges flags");
        feed(s, "\x1b[=4;3u"); // AND-NOT flag 4
        expect(s.kitty_keyboard_flags() == 3, "kitty set mode 3 (and-not) clears flags");
        feed(s, "\x1b[<1u"); // pop 1 level
        expect(s.kitty_keyboard_flags() == 0, "kitty pop restores previous level");
        feed(s, "\x1b[<9u"); // over-pop never underflows base
        expect(s.kitty_keyboard_flags() == 0, "kitty over-pop keeps base level");
    }

    // --- OSC 4 / 104: palette set + reset ----------------------------------
    // OSC is routed by feed_output (which owns the palette), not Screen::apply,
    // so these drive a Model and assert on model.screen's recorded edits.
    {
        term::Model m{Config{}, Extent{6, 2}};
        const std::uint64_t e0 = m.screen.palette_epoch();
        (void)term::feed_output(m, "\x1b]4;1;rgb:ff/00/00\x1b\\"); // set index 1 = red
        expect(m.screen.palette_epoch() > e0, "OSC 4 set bumps the palette epoch");
        bool edit_ok = false;
        for (const auto &e : m.screen.color_edits())
            if (e.target == term::Screen::ColorEdit::Target::index && e.index == 1 &&
                !e.reset && e.rgb == Rgb{255, 0, 0})
                edit_ok = true;
        expect(edit_ok, "OSC 4 records an index colour edit (red)");

        Cmds q = term::feed_output(m, "\x1b]4;1;?\x1b\\");
        bool query_ok = false;
        for (const auto &c : q)
            if (const auto *w = std::get_if<WriteChild>(&c))
                if (w->bytes.find("rgb:") != std::string::npos) query_ok = true;
        expect(query_ok, "OSC 4 query replies with an rgb: spec");

        (void)term::feed_output(m, "\x1b]104\x1b\\"); // reset all
        bool reset_all = false;
        for (const auto &e : m.screen.color_edits())
            if (e.target == term::Screen::ColorEdit::Target::all) reset_all = true;
        expect(reset_all, "OSC 104 (no args) records a full palette reset");
    }

    // --- OSC 11 set + OSC 12 cursor colour ---------------------------------
    {
        term::Model m{Config{}, Extent{6, 2}};
        (void)term::feed_output(m, "\x1b]11;#101828\x1b\\"); // default bg via #rrggbb
        bool bg_ok = false;
        for (const auto &e : m.screen.color_edits())
            if (e.target == term::Screen::ColorEdit::Target::bg && e.rgb == Rgb{0x10, 0x18, 0x28})
                bg_ok = true;
        expect(bg_ok, "OSC 11 sets default background (#rrggbb form)");

        (void)term::feed_output(m, "\x1b]12;rgb:00/ff/00\x1b\\"); // cursor colour green
        bool cur_ok = false;
        for (const auto &e : m.screen.color_edits())
            if (e.target == term::Screen::ColorEdit::Target::cursor && e.rgb == Rgb{0, 255, 0})
                cur_ok = true;
        expect(cur_ok, "OSC 12 sets the cursor colour");
    }

    // --- reflow on resize --------------------------------------------------
    {
        // A line longer than the width soft-wraps; narrowing then widening must
        // preserve the logical text (join + rewrap), not truncate it.
        term::Screen s{Extent{10, 4}};
        feed(s, "ABCDEFGHIJKLMNO"); // 15 chars in a 10-wide grid -> wraps once
        expect(glyph_at(s, 0, 0) == U'A' && glyph_at(s, 1, 0) == U'K',
               "pre-reflow: 15 chars wrap at col 10");
        s.resize(Extent{5, 6}); // narrow to 5 cols
        // The logical line "ABCDE FGHIJ KLMNO" now occupies 3 rows of 5.
        expect(glyph_at(s, 0, 0) == U'A' && glyph_at(s, 1, 0) == U'F' &&
                   glyph_at(s, 2, 0) == U'K',
               "reflow narrow: line rewraps to 5-col rows");
        s.resize(Extent{15, 4}); // widen to 15 cols
        expect(glyph_at(s, 0, 0) == U'A' && glyph_at(s, 0, 14) == U'O',
               "reflow widen: line rejoins onto one 15-col row");
    }
    {
        // A hard line break (real newline) must NOT be joined during reflow.
        term::Screen s{Extent{10, 4}};
        feed(s, "abc\r\ndef");
        s.resize(Extent{20, 4});
        expect(glyph_at(s, 0, 0) == U'a' && glyph_at(s, 1, 0) == U'd',
               "reflow keeps hard line breaks separate");
    }

    // --- DECSTR soft reset -------------------------------------------------
    {
        term::Screen s{Extent{10, 4}};
        feed(s, "\x1b[4 q");   // steady underline cursor
        feed(s, "\x1b[?7l");   // autowrap off
        feed(s, "\x1b[2;3r");  // scroll region
        feed(s, "hi");         // content that should SURVIVE a soft reset
        feed(s, "\x1b[!p");    // DECSTR
        expect(s.cursor_style().shape == term::Screen::CursorShape::block,
               "DECSTR restores block cursor");
        expect(glyph_at(s, 0, 0) == U'h', "DECSTR keeps screen content");
        // Autowrap restored: 12 chars now wrap instead of overprinting col 9.
        feed(s, "\r\nABCDEFGHIJKL");
        expect(glyph_at(s, 2, 0) == U'K', "DECSTR restored autowrap (line wraps)");
    }

    // --- REP + DEC rectangular ops -----------------------------------------
    {
        term::Screen s{Extent{10, 3}};
        feed(s, "X\x1b[4b"); // print X, then REP 4 -> XXXXX total
        expect(glyph_at(s, 0, 0) == U'X' && glyph_at(s, 0, 4) == U'X' &&
                   glyph_at(s, 0, 5) == U' ',
               "REP repeats the last char Ps times");
    }
    {
        term::Screen s{Extent{8, 5}};
        // DECFRA: fill rows 2..4, cols 2..5 with '*' (Pch=42).
        feed(s, "\x1b[42;2;2;4;5$x");
        expect(glyph_at(s, 1, 1) == U'*' && glyph_at(s, 3, 4) == U'*' &&
                   glyph_at(s, 0, 0) == U' ',
               "DECFRA fills a rectangle with a character");
        // DECERA: erase rows 2..3, cols 2..3.
        feed(s, "\x1b[2;2;3;3$z");
        expect(glyph_at(s, 1, 1) == U' ' && glyph_at(s, 3, 4) == U'*',
               "DECERA erases a rectangle, leaving the rest");
    }

    // --- DEC line attributes (ESC # 3/4/5/6) + DECALN (# 8) ----------------
    {
        term::Screen s{Extent{10, 4}};
        feed(s, "\x1b#6"); // DECDWL on row 0 (cursor there)
        expect(s.line_attr(0) == term::Screen::LineAttr::double_width,
               "ESC # 6 sets double-width on the cursor row");
        feed(s, "\r\n\x1b#3"); // row 1: DECDHL top
        expect(s.line_attr(1) == term::Screen::LineAttr::double_top,
               "ESC # 3 sets double-height top half");
        feed(s, "\x1b#5"); // DECSWL back to normal on row 1
        expect(s.line_attr(1) == term::Screen::LineAttr::normal,
               "ESC # 5 restores single-width");
        // DECALN fills the screen with 'E' and homes the cursor.
        feed(s, "\x1b#8");
        expect(glyph_at(s, 0, 0) == U'E' && glyph_at(s, 3, 9) == U'E' &&
                   s.cursor().row.get() == 0 && s.cursor().col.get() == 0,
               "DECALN fills screen with E and homes cursor");
    }

    // --- OSC 7 (cwd) + OSC 133 (shell integration) -------------------------
    {
        term::Model m{Config{}, Extent{10, 3}};
        (void)term::feed_output(m, "\x1b]7;file:///home/u/proj\x1b\\");
        expect(m.working_dir == "file:///home/u/proj", "OSC 7 records working directory");
        (void)term::feed_output(m, "\x1b]133;A\x1b\\");
        expect(m.shell_zone == term::Model::ShellZone::prompt, "OSC 133;A -> prompt zone");
        (void)term::feed_output(m, "\x1b]133;C\x1b\\");
        expect(m.shell_zone == term::Model::ShellZone::output, "OSC 133;C -> output zone");
    }

    // --- DECRQCRA rectangular checksum -------------------------------------
    {
        term::Screen s{Extent{4, 2}};
        feed(s, "AB"); // row 0: 'A'(65) 'B'(66) then blanks
        // Checksum of the full screen = -(sum of all cell codepoints) & 0xFFFF.
        // 'A'+'B' + 6 spaces(32) = 65+66+192 = 323; -(323) & 0xFFFF = 0xFEBD.
        const std::string rep = feed_replies(s, "\x1b[1;0;1;1;2;4*y");
        expect(rep.rfind("\x1bP1!~", 0) == 0 && rep.find('~') != std::string::npos,
               "DECRQCRA replies DCS Pid ! ~ <hex> ST");
    }

    // --- left/right margins (DECLRMM ?69 + DECSLRM) ------------------------
    {
        term::Screen s{Extent{10, 4}};
        feed(s, "\x1b[?69h");    // enable DECLRMM
        feed(s, "\x1b[3;7s");    // DECSLRM: left=col3(0-based 2), right=col7(6)
        // Cursor homed to the region origin (top row, left margin).
        expect(s.cursor().col.get() == 2 && s.cursor().row.get() == 0,
               "DECSLRM homes cursor to left margin");
        // Type past the right margin: it wraps at the margin, not the screen edge.
        feed(s, "ABCDEF"); // 6 chars into a 5-wide margin region [2..6]
        expect(glyph_at(s, 0, 2) == U'A' && glyph_at(s, 0, 6) == U'E' &&
                   glyph_at(s, 1, 2) == U'F',
               "text wraps at the right margin and CR-to-left-margin");
        // Insert-chars shifts only within the margins.
        feed(s, "\x1b[H"); // home (origin off -> absolute 0,0)... clamp
    }
    {
        term::Screen s{Extent{10, 3}};
        feed(s, "\x1b[?69h\x1b[2;5s"); // margins cols 2..5 (0-based 1..4)
        feed(s, "\x1b[1;2H");          // move to row0 col1 (the left margin)
        feed(s, "XYZW");               // fill the margin region
        feed(s, "\x1b[1;2H\x1b[@");    // ICH 1 at left margin: shift right within margin
        expect(glyph_at(s, 0, 2) == U'X' && glyph_at(s, 0, 4) == U'Z',
               "ICH shifts only within left/right margins");
        expect(glyph_at(s, 0, 1) == U' ', "ICH opens a blank at the left margin");
    }

    if (failures == 0) {
        std::printf("all screen tests passed\n");
        return 0;
    }
    std::printf("%d screen test(s) failed\n", failures);
    return 1;
}
