// SPDX-License-Identifier: LGPL-2.0-or-later
// widechar_test — exhaustive correctness for double-width (CJK/emoji) cells.
// A wide glyph occupies a LEAD cell (width 2) + a SPACER (width 0). Overwriting
// either half must dissolve the whole pair, or an orphan lead/spacer lingers and
// renders as a gap or stale glyph ("missing second column", "weird text").
#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"
#include <cstdio>
#include <string>

using namespace toe;
using namespace toe::term;

static int fails = 0;
static void ck(bool ok, const char *n) { if (!ok) { std::printf("FAIL %s\n", n); ++fails; } }

static void feed(Screen &s, vt::Parser &p, const std::string &d) {
    p.feed(std::span<const char>{d.data(), d.size()},
           [&](const vt::Action &a) { Cmds out; s.apply(a, out); });
}
static const Cell &at(const Screen &s, int r, int c) { return s.row(Row{r})[static_cast<std::size_t>(c)]; }

int main() {
    const std::string CSI = "\x1b[";

    // 1) A wide char lays down lead(width2)+spacer(width0).
    {
        Screen s{Extent{20, 3}}; vt::Parser p;
        feed(s, p, "\x1b[H漢");
        ck(at(s, 0, 0).width == 2, "wide lead width==2");
        ck(at(s, 0, 0).cp == U'漢', "wide lead cp");
        ck(at(s, 0, 1).width == 0, "wide spacer width==0");
    }

    // 2) Overwrite the LEAD with a narrow char -> spacer must be dissolved.
    {
        Screen s{Extent{20, 3}}; vt::Parser p;
        feed(s, p, "\x1b[H漢");
        feed(s, p, "\x1b[HX");                // narrow over the lead (col 0)
        ck(at(s, 0, 0).cp == U'X' && at(s, 0, 0).width == 1, "lead overwritten by X");
        ck(at(s, 0, 1).width != 0, "orphan spacer dissolved");  // must NOT be a spacer
        ck(at(s, 0, 1).blank(), "old spacer now blank");
    }

    // 3) Overwrite the SPACER with a narrow char -> lead must be dissolved.
    {
        Screen s{Extent{20, 3}}; vt::Parser p;
        feed(s, p, "\x1b[H漢");
        feed(s, p, "\x1b[1;2HY");             // narrow over the spacer (col 1)
        ck(at(s, 0, 1).cp == U'Y' && at(s, 0, 1).width == 1, "spacer overwritten by Y");
        ck(at(s, 0, 0).width != 2, "orphan lead dissolved");    // must NOT be a wide lead
        ck(at(s, 0, 0).blank(), "old lead now blank");
    }

    // 4) Overwrite a wide with another wide (shifted by one) -> no orphan.
    {
        Screen s{Extent{20, 3}}; vt::Parser p;
        feed(s, p, "\x1b[H漢字");             // cols 0-1 lead+sp, 2-3 lead+sp
        feed(s, p, "\x1b[1;2H日");            // wide starting at col 1 (over sp+lead)
        ck(at(s, 0, 0).blank(), "col0 lead dissolved when wide lands on its spacer");
        ck(at(s, 0, 1).cp == U'日' && at(s, 0, 1).width == 2, "new wide lead at col1");
        ck(at(s, 0, 2).width == 0, "new wide spacer at col2");
        ck(at(s, 0, 3).blank(), "col3 orphan (old 字 spacer) dissolved");
    }

    // 5) Wide char at the LAST column must wrap (never write out of bounds).
    {
        Screen s{Extent{4, 2}}; vt::Parser p;   // 4 cols
        feed(s, p, "\x1b[HAAA");                 // fill cols 0,1,2; cursor at col 3
        feed(s, p, "漢");                        // wide can't fit at col 3 -> wraps
        // On the wrapped line the wide char sits at col 0.
        ck(at(s, 1, 0).width == 2 && at(s, 1, 0).cp == U'漢', "wide wrapped to next line");
        ck(at(s, 1, 1).width == 0, "wrapped wide spacer");
    }

    // 6) Stream lots of wide + narrow mixed; every spacer has a lead to its left,
    //    every lead has a spacer to its right (no orphans anywhere).
    {
        Screen s{Extent{40, 5}}; vt::Parser p;
        feed(s, p, "\x1b[H漢aB字c日X本語Zz漢字\r\n");
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 40; ++c) {
                const Cell &cell = at(s, r, c);
                if (cell.width == 2) {
                    ck(c + 1 < 40 && at(s, r, c + 1).width == 0, "lead followed by spacer");
                }
                if (cell.width == 0) {
                    ck(c - 1 >= 0 && at(s, r, c - 1).width == 2, "spacer preceded by lead");
                }
            }
        }
    }

    std::printf(fails ? "%d WIDECHAR CHECK(S) FAILED\n" : "ALL WIDECHAR CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
