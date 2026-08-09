// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Resize robustness: content integrity across reflow, and scroll-position
// STABILITY — a resize / font change must not lose data, corrupt scrollback, or
// jump the viewport to the bottom. Reads history via the exact path the renderer
// uses (scroll to the offset that puts an absolute row at viewport top).

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "toe/term/screen.hpp"
#include "toe/term/update.hpp"
#include "toe/vt/parser.hpp"

using namespace toe;

static void feed(term::Screen &s, std::string_view b) {
    vt::Parser p;
    p.feed(std::span<const char>{b.data(), b.size()},
           [&](const vt::Action &a) { toe::Cmds o; s.apply(a, o); });
}

static std::string trim(std::span<const term::Cell> r) {
    std::string o;
    for (const term::Cell &c : r) o += (c.cp >= 0x20 && c.cp < 0x7f) ? static_cast<char>(c.cp) : ' ';
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// Read every logical absolute row top-to-bottom. History rows (absolute
// [0,h)) are read by scrolling so each sits at viewport row 0; the live grid
// (absolute [h, h+rows)) is read directly at offset 0 via row(Row{r}).
static std::vector<std::string> read_all(term::Screen &s) {
    const int h = s.history_rows();
    const int rows = s.size().rows;
    std::vector<std::string> all;
    for (int a = 0; a < h; ++a) {
        s.scroll(-1'000'000);
        s.scroll(h - a);          // offset h-a => viewport row 0 == absolute a
        all.push_back(trim(s.row(Row{0})));
    }
    s.scroll(-1'000'000);          // live
    for (int r = 0; r < rows; ++r) all.push_back(trim(s.row(Row{r})));
    return all;
}

static int failures = 0;
static void ck(bool ok, const char *n) { std::printf(ok ? "ok   %s\n" : "FAIL %s\n", n); if (!ok) ++failures; }

// Fill with n hard lines "LINE-XX...", each padded to `len` cols.
static term::Screen make(int cols, int rows, int n, int len) {
    term::Screen s(Extent{cols, rows});
    for (int i = 0; i < n; ++i) {
        char pre[32];
        std::snprintf(pre, sizeof pre, "LINE-%03d-", i);
        std::string L = pre;
        while (static_cast<int>(L.size()) < len) L += 'x';
        L += "\r\n";
        feed(s, L);
    }
    return s;
}

// Verify every LINE-XXX marker survives exactly once, in order.
static void integrity(term::Screen &s, int n, const char *label) {
    auto lines = read_all(s);
    std::vector<int> cnt(static_cast<std::size_t>(n), 0);
    int disorder = 0, last = -1;
    for (const std::string &l : lines) {
        if (l.rfind("LINE-", 0) != 0) continue;
        int idx = std::atoi(l.c_str() + 5);
        if (idx < 0 || idx >= n) continue;
        ++cnt[static_cast<std::size_t>(idx)];
        if (idx < last) ++disorder;
        last = idx;
    }
    int miss = 0, dup = 0;
    for (int i = 0; i < n; ++i) { if (!cnt[static_cast<std::size_t>(i)]) ++miss; if (cnt[static_cast<std::size_t>(i)] > 1) ++dup; }
    const bool ok = !miss && !dup && !disorder;
    std::printf("%s %-38s (miss=%d dup=%d disorder=%d)\n", ok ? "ok  " : "FAIL", label, miss, dup, disorder);
    if (!ok) ++failures;
}

// The line at the top of the viewport right now (as scrolled).
static std::string top_line(term::Screen &s) { return trim(s.row(Row{0})); }

int main() {
    // --- content integrity across every resize shape ---
    { auto s = make(80, 5, 30, 14); s.resize(Extent{100, 6}); integrity(s, 30, "hard widen 80->100"); }
    { auto s = make(80, 5, 30, 14); s.resize(Extent{40, 5});  integrity(s, 30, "hard shrink 80->40"); }
    { auto s = make(80, 5, 30, 14); s.resize(Extent{80, 10}); integrity(s, 30, "height grow 5->10"); }
    { auto s = make(80, 8, 30, 14); s.resize(Extent{80, 4});  integrity(s, 30, "height shrink 8->4"); }
    { auto s = make(80, 6, 20, 120);s.resize(Extent{160, 6}); integrity(s, 20, "wrapped widen 80->160"); }
    { auto s = make(80, 6, 20, 120);s.resize(Extent{50, 6});  integrity(s, 20, "wrapped shrink 80->50"); }
    { auto s = make(80, 6, 30, 100);
      s.resize(Extent{120,6}); s.resize(Extent{60,8}); s.resize(Extent{90,4}); s.resize(Extent{80,6});
      integrity(s, 30, "repeated resize round-trip"); }

    // --- scroll STABILITY: the viewport must stay on the same line ---
    {
        auto s = make(80, 10, 100, 8);
        s.scroll(40); // into history
        const std::string before = top_line(s);
        s.resize(Extent{100, 10}); // width reflow
        ck(s.scroll_offset() > 0, "width reflow: did not jump to bottom");
        ck(top_line(s) == before, "width reflow: top line preserved");
    }
    {
        auto s = make(80, 10, 100, 8);
        s.scroll(40);
        const std::string before = top_line(s);
        s.resize(Extent{80, 16}); // height grow
        ck(top_line(s) == before, "height grow: top line preserved");
    }
    {
        auto s = make(80, 10, 100, 8);
        s.scroll(40);
        const std::string before = top_line(s);
        s.resize(Extent{80, 6}); // height shrink
        ck(top_line(s) == before, "height shrink: top line preserved");
    }
    {
        // Live view (not scrolled) should stay live after resize.
        auto s = make(80, 10, 100, 8);
        s.resize(Extent{100, 10});
        ck(s.scroll_offset() == 0, "live view stays live after resize");
    }

    // --- the screenshot bug: content that does NOT fill the grid, cursor mid-
    //     screen with blank rows below, then SHRINK. The prompt must stay in the
    //     viewport — trailing blank screen space must not push content into
    //     scrollback and blank the view.
    {
        term::Screen s(Extent{80, 24});
        feed(s, "Welcome line one\r\n");
        feed(s, "second line here\r\n");
        feed(s, "user@host ~> "); // prompt, no newline: cursor mid-screen
        s.resize(Extent{80, 10}); // shrink height (big -> small)
        ck(s.history_rows() == 0, "shrink with blank rows: nothing pushed to scrollback");
        ck(s.scroll_offset() == 0, "shrink with blank rows: view stays live");
        ck(trim(s.row(Row{2})) == "user@host ~>", "shrink: prompt stays visible at row 2");
        ck(trim(s.row(Row{0})) == "Welcome line one", "shrink: first line stays visible");
    }
    {
        // Same, but width shrink too (reflow path with trailing blanks).
        term::Screen s(Extent{80, 24});
        feed(s, "alpha\r\nbeta\r\ngamma> ");
        s.resize(Extent{40, 8});
        ck(s.history_rows() == 0, "width+height shrink: no spurious scrollback");
        ck(trim(s.row(Row{2})) == "gamma>", "width+height shrink: prompt visible");
    }

    // --- scrolled-up stability: reading history must not creep or yank when
    //     new output arrives; typing snaps back to the live bottom.
    {
        term::Screen s(Extent{80, 10});
        for (int i = 0; i < 50; ++i) {
            char b[32]; std::snprintf(b, sizeof b, "LINE-%03d\r\n", i); feed(s, b);
        }
        s.scroll(20);
        const std::string top = trim(s.row(Row{0}));
        // A burst of new output while scrolled up.
        for (int i = 0; i < 5; ++i) feed(s, "NEW-OUTPUT\r\n");
        ck(trim(s.row(Row{0})) == top, "scrolled-up view does not creep on new output");
        ck(s.scroll_offset() > 0, "scrolled-up view does not yank to bottom on new output");
    }

    // The real bug: feed_output() used to scroll_to_bottom() on EVERY chunk,
    // yanking a reader down. Drive output through the FULL update path (parser +
    // feed_output) exactly as the live terminal does, and verify a scrolled-up
    // view holds its place across many output bursts.
    {
        term::Model m(toe::Config{}, Extent{80, 10});
        for (int i = 0; i < 50; ++i) {
            char b[32]; std::snprintf(b, sizeof b, "ROW-%03d\r\n", i);
            (void)term::feed_output(m, b);
        }
        m.screen.scroll(15); // scroll up into history
        const std::string top = trim(m.screen.row(Row{0}));
        const int off0 = m.screen.scroll_offset();
        ck(off0 > 0, "feed_output path: scrolled up");
        for (int i = 50; i < 80; ++i) { // 30 more lines of output
            char b[32]; std::snprintf(b, sizeof b, "ROW-%03d\r\n", i);
            (void)term::feed_output(m, b);
        }
        ck(trim(m.screen.row(Row{0})) == top, "feed_output: view stays on the same line");
        ck(m.screen.scroll_offset() > 0, "feed_output: output does NOT yank to bottom");
    }

    // ── TORTURE: rapid randomized resizes (drag / SIGWINCH storm) ──────────
    // The #1 real-world way terminals break under a TUI is a resize race: a
    // drag fires dozens of arbitrary-size SIGWINCHs while the app is mid-redraw.
    // We hammer resize() with random sizes, interleaving output, on BOTH the
    // primary and the alt screen, and after EVERY step assert the screen's
    // invariants hold (dimensions consistent, cursor in bounds, every row
    // readable — no OOB) and nothing throws/crashes.
    {
        std::mt19937 rng(0xC0FFEE);
        auto rnd = [&](int lo, int hi) {
            return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
        };

        auto invariants = [&](term::Screen &s, const char *where) {
            const Extent sz = s.size();
            bool ok = sz.cols > 0 && sz.rows > 0;
            const auto cur = s.cursor();
            ok = ok && cur.row.get() >= 0 && cur.row.get() < sz.rows;
            ok = ok && cur.col.get() >= 0 && cur.col.get() <= sz.cols;
            // Every visible row must be addressable (OOB/crash if reflow left
            // the grid inconsistent). Touch each row's cells.
            for (int r = 0; r < sz.rows; ++r) { auto row = s.row(Row{r}); (void)row.size(); }
            if (!ok)
                std::printf("  torture invariant FAILED at %s (%dx%d)\n", where, sz.cols, sz.rows);
            return ok;
        };

        // Primary screen: fill with content, then storm-resize.
        {
            term::Screen s = make(80, 24, 200, 40);
            bool all_ok = true;
            for (int i = 0; i < 3000; ++i) {
                // Include extreme aspect ratios (1-col, 1-row) that stress the
                // reflow/cursor-clamp edge cases hardest.
                const int c = (i % 40 == 0) ? 1 : rnd(1, 400);
                const int r = (i % 37 == 0) ? 1 : rnd(1, 200);
                s.resize(Extent{c, r});
                if ((i & 7) == 0) {
                    char b[48];
                    std::snprintf(b, sizeof b, "burst-%d-\x1b[7mX\x1b[0m\r\n", i);
                    feed(s, b);
                }
                if (!invariants(s, "primary")) { all_ok = false; break; }
            }
            ck(all_ok, "primary-screen resize storm keeps all invariants (3000 random resizes)");
        }

        // Alt screen (vim/tmux/htop): the app owns every cell; crop/pad, no
        // reflow. Enter alt, paint, then storm-resize with app repaints.
        {
            term::Screen s(Extent{80, 24});
            feed(s, "\x1b[?1049h");
            for (int r = 0; r < 24; ++r) {
                char b[64];
                std::snprintf(b, sizeof b, "\x1b[%d;1H\x1b[44mpane row %02d filler\x1b[0m", r + 1, r);
                feed(s, b);
            }
            bool all_ok = true;
            for (int i = 0; i < 3000; ++i) {
                const int c = (i % 40 == 0) ? 1 : rnd(1, 400);
                const int r = (i % 37 == 0) ? 1 : rnd(1, 200);
                s.resize(Extent{c, r});
                if ((i & 3) == 0) {
                    const Extent sz = s.size();
                    char b[64];
                    std::snprintf(b, sizeof b, "\x1b[H\x1b[2J\x1b[1;1Hredraw %dx%d", sz.cols, sz.rows);
                    feed(s, b);
                }
                if (!invariants(s, "alt")) { all_ok = false; break; }
            }
            feed(s, "\x1b[?1049l");
            ck(all_ok, "alt-screen resize storm keeps all invariants (3000 random resizes)");
            ck(invariants(s, "post-alt-exit"), "leaving alt screen after a storm is consistent");
        }

        // Round-trip stability: churn the width and confirm marked content is
        // never lost (it may reflow to a different row, but must still exist).
        {
            term::Screen s(Extent{80, 24});
            feed(s, "STABLE-MARKER-abcdef\r\n");
            for (int w : {40, 200, 17, 133, 8, 250, 80}) s.resize(Extent{w, 24});
            auto lines = read_all(s);
            bool found = false;
            for (const std::string &l : lines)
                if (l.find("STABLE-MARKER") != std::string::npos) { found = true; break; }
            ck(found, "content survives a width-resize round trip (no data loss)");
        }
    }

    std::printf(failures ? "\nresize test: %d FAILURES\n" : "\nresize test: PASS\n", failures);
    return failures ? 1 : 0;
}
