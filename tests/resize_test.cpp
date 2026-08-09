// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Resize robustness: content integrity across reflow, and scroll-position
// STABILITY — a resize / font change must not lose data, corrupt scrollback, or
// jump the viewport to the bottom. Reads history via the exact path the renderer
// uses (scroll to the offset that puts an absolute row at viewport top).

#include <cstdio>
#include <cstdlib>
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

    std::printf(failures ? "\nresize test: %d FAILURES\n" : "\nresize test: PASS\n", failures);
    return failures ? 1 : 0;
}
