// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SGR conformance, with the focus on extended colour.
//
// 38/48/58 have TWO wire forms that are not the same length:
//
//   semicolon: 38;2;R;G;B       legacy xterm
//   colon:     38:2::R:G:B      ECMA-48, and what kitty / neovim / iTerm2 send
//
// The colon form carries an extra (usually empty) colour-space-id slot after
// the selector. Treating the two identically reads the colour one position
// early -- 38:2::1:2:3 came out as rgb(0,1,2), and 58:2::R:G:B was ignored
// entirely, so undercurl colours in neovim did nothing.

#include "toe/term/update.hpp"

#include <cstdio>
#include <string_view>

using namespace toe;

static int failures = 0;

static const term::Cell &first_cell(term::Model &m) { return m.screen.row(Row{0})[0]; }

// Feed an SGR sequence then one character, and inspect that cell's pen. A
// fresh model per case so attributes can't leak between assertions.
static term::Pen pen_after(std::string_view sgr) {
    term::Config cfg;
    term::Model m{cfg, Extent{10, 1}};
    (void)term::feed_output(m, sgr);
    (void)term::feed_output(m, "X");
    return first_cell(m).pen;
}

static void expect_fg(std::string_view sgr, Rgb want, const char *what) {
    const term::Palette pal;
    const Rgb got = pal.resolve(pen_after(sgr).fg, true);
    if (!(got == want)) {
        std::printf("FAIL %s\n       got  %02x%02x%02x\n       want %02x%02x%02x\n", what,
                    got.r, got.g, got.b, want.r, want.g, want.b);
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

static void expect_ulcol(std::string_view sgr, Rgb want, const char *what) {
    const term::Palette pal;
    const Rgb got = pal.resolve(pen_after(sgr).underline_color, true);
    if (!(got == want)) {
        std::printf("FAIL %s\n       got  %02x%02x%02x\n       want %02x%02x%02x\n", what,
                    got.r, got.g, got.b, want.r, want.g, want.b);
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

static void expect(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

int main() {
    const Rgb kDefaultFg = rgb(220, 220, 220);

    // --- direct colour, both wire forms -----------------------------------
    expect_fg("\033[38;2;1;2;3m", rgb(1, 2, 3), "38;2;R;G;B (semicolon form)");
    expect_fg("\033[38:2::1:2:3m", rgb(1, 2, 3), "38:2::R:G:B (colon form, ECMA-48)");
    expect_fg("\033[38:2:1:2:3m", rgb(1, 2, 3), "38:2:R:G:B (colon, no colour-space slot)");
    expect_fg("\033[38;2;255;128;0m", rgb(255, 128, 0), "38;2 with full-range values");

    // --- indexed colour: same shape in both forms --------------------------
    expect_fg("\033[38;5;196m", rgb(0xff, 0, 0), "38;5;196 (semicolon indexed)");
    expect_fg("\033[38:5:196m", rgb(0xff, 0, 0), "38:5:196 (colon indexed)");

    // --- underline colour (SGR 58) ----------------------------------------
    // 58:2:: was silently ignored, which is why neovim's undercurl colours
    // didn't show up.
    expect_ulcol("\033[58;2;255;0;0m", rgb(255, 0, 0), "58;2;R;G;B sets underline colour");
    expect_ulcol("\033[58:2::255:0:0m", rgb(255, 0, 0), "58:2::R:G:B sets underline colour");
    expect_ulcol("\033[58;5;46m", rgb(0, 0xff, 0), "58;5;n sets underline colour");
    expect_ulcol("\033[58:5:46m", rgb(0, 0xff, 0), "58:5:n sets underline colour");
    // 59 resets it to "follow the foreground".
    expect_ulcol("\033[58;2;255;0;0m\033[59m", kDefaultFg, "59 resets the underline colour");

    // --- underline styles --------------------------------------------------
    expect(pen_after("\033[4m").underline == term::Underline::Single, "SGR 4 = single underline");
    expect(pen_after("\033[4:1m").underline == term::Underline::Single, "SGR 4:1 = single");
    expect(pen_after("\033[4:2m").underline == term::Underline::Double, "SGR 4:2 = double");
    expect(pen_after("\033[4:3m").underline == term::Underline::Curly, "SGR 4:3 = curly");
    expect(pen_after("\033[21m").underline == term::Underline::Double, "SGR 21 = double");
    expect(pen_after("\033[4m\033[24m").underline == term::Underline::None, "SGR 24 clears it");
    expect(has(pen_after("\033[4m").attr, term::Attr::Underline), "SGR 4 sets the Underline attr");

    // --- an extended colour must not desync the rest of the run -----------
    {
        const term::Pen p = pen_after("\033[1;38:2::10:20:30;4:3m");
        const term::Palette pal;
        expect(has(p.attr, term::Attr::Bold), "bold survives a colon colour mid-run");
        expect(pal.resolve(p.fg, true) == rgb(10, 20, 30), "colon colour parsed mid-run");
        expect(p.underline == term::Underline::Curly, "4:3 after a colon colour still applies");
    }
    {
        const term::Pen p = pen_after("\033[38;2;10;20;30;4;58;5;21m");
        const term::Palette pal;
        expect(pal.resolve(p.fg, true) == rgb(10, 20, 30), "semicolon colour in a long run");
        expect(p.underline == term::Underline::Single, "4 after a semicolon colour applies");
        expect(pal.resolve(p.underline_color, true) == rgb(0, 0, 0xff), "58 later in the run");
    }

    // --- truncated extended colour must degrade safely --------------------
    // Reading past the parameter list here would be an out-of-bounds read.
    expect_fg("\033[38m", kDefaultFg, "bare 38 leaves fg alone");
    expect_fg("\033[38;2m", kDefaultFg, "38;2 with no components leaves fg alone");
    expect_fg("\033[38;2;1m", kDefaultFg, "38;2;1 (truncated) leaves fg alone");
    expect_fg("\033[38;2;1;2m", kDefaultFg, "38;2;1;2 (truncated) leaves fg alone");
    expect_fg("\033[38:2::1m", kDefaultFg, "38:2::1 (truncated colon) leaves fg alone");
    expect_fg("\033[38;5m", kDefaultFg, "38;5 with no index leaves fg alone");

    // --- basic and bright colours -----------------------------------------
    expect_fg("\033[31m", rgb(0xcc, 0, 0), "SGR 31 = red");
    expect_fg("\033[91m", rgb(0xef, 0x29, 0x29), "SGR 91 = bright red");
    expect_fg("\033[31m\033[39m", kDefaultFg, "SGR 39 resets fg to default");
    // SGR 0 and an empty SGR both reset everything.
    expect(pen_after("\033[1;4;31m\033[0m") == term::Pen{}, "SGR 0 resets the whole pen");
    expect(pen_after("\033[1;4;31m\033[m") == term::Pen{}, "empty SGR resets the whole pen");

    // --- attributes --------------------------------------------------------
    expect(has(pen_after("\033[1m").attr, term::Attr::Bold), "SGR 1 = bold");
    expect(has(pen_after("\033[2m").attr, term::Attr::Faint), "SGR 2 = faint");
    expect(has(pen_after("\033[3m").attr, term::Attr::Italic), "SGR 3 = italic");
    expect(has(pen_after("\033[5m").attr, term::Attr::Blink), "SGR 5 = blink");
    expect(has(pen_after("\033[53m").attr, term::Attr::Overline), "SGR 53 = overline");
    expect(has(pen_after("\033[7m").attr, term::Attr::Reverse), "SGR 7 = reverse");
    expect(has(pen_after("\033[8m").attr, term::Attr::Hidden), "SGR 8 = hidden");
    expect(has(pen_after("\033[9m").attr, term::Attr::Strike), "SGR 9 = strike");
    expect(!has(pen_after("\033[1m\033[22m").attr, term::Attr::Bold), "SGR 22 clears bold");
    expect(!has(pen_after("\033[3m\033[23m").attr, term::Attr::Italic), "SGR 23 clears italic");
    expect(!has(pen_after("\033[7m\033[27m").attr, term::Attr::Reverse), "SGR 27 clears reverse");
    expect(!has(pen_after("\033[53m\033[55m").attr, term::Attr::Overline), "SGR 55 clears overline");
    expect(!has(pen_after("\033[5m\033[25m").attr, term::Attr::Blink), "SGR 25 clears blink");

    std::printf(failures ? "sgr: %d FAILURES\n" : "sgr: PASS\n", failures);
    return failures ? 1 : 0;
}
