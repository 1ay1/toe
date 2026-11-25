// SPDX-License-Identifier: LGPL-2.0-or-later
//
// history_resize_test -- nothing in the scrollback is lost on resize, at
// hand's sizes: the full Model (10k scrollback, shell-integration prompts),
// thousands of numbered lines, then window resizes, drags, output between,
// and a trip through the alternate screen. Every line must be readable,
// once, in order, afterwards.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "toe/term/update.hpp"

using namespace toe;

static int failures = 0;

// Every "L<n>." marker in the whole buffer (history + screen), rejoining
// soft-wrapped rows, in order.
static std::vector<int> markers(const term::Model &m) {
    const auto &s = m.screen;
    std::string all;
    for (std::int64_t a = 0; a < s.total_rows(); ++a) {
        for (const auto &c : s.abs_row_cells(a)) all += (c.cp >= 0x20 && c.cp < 0x7f) ? static_cast<char>(c.cp) : ' ';
        if (!s.wrapped_abs(a)) all += '\n';
    }
    std::vector<int> out;
    for (std::size_t i = 0; (i = all.find('L', i)) != std::string::npos; ++i) {
        std::size_t j = i + 1;
        int v = 0;
        bool digits = false;
        while (j < all.size() && all[j] >= '0' && all[j] <= '9') { v = v * 10 + (all[j] - '0'); ++j; digits = true; }
        if (digits && j < all.size() && all[j] == '.') out.push_back(v);
    }
    return out;
}

static void check(const term::Model &m, int n, const char *what) {
    const auto got = markers(m);
    bool ok = static_cast<int>(got.size()) == n;
    for (int i = 0; ok && i < n; ++i) ok = got[static_cast<std::size_t>(i)] == i;
    if (!ok) {
        std::printf("FAIL %s: %zu markers (want %d), history %d rows, size %dx%d", what, got.size(), n,
                    m.screen.history_rows(), m.screen.size().cols, m.screen.size().rows);
        if (!got.empty()) std::printf(", first L%d last L%d", got.front(), got.back());
        std::printf("\n");
        ++failures;
    }
}

static std::string lines(int from, int to, int width) {
    std::string s;
    for (int i = from; i < to; ++i) {
        s += "\x1b]133;A\x07$ \x1b]133;B\x07" "cmd\r\n\x1b]133;C\x07";
        std::string body = "L" + std::to_string(i) + ".";
        while (static_cast<int>(body.size()) < (i % 3 == 0 ? width + 17 : 30 + i % 40)) body += static_cast<char>('a' + i % 26);
        s += body + "\r\n\x1b]133;D;0\x07";
    }
    return s + "\x1b]133;A\x07$ \x1b]133;B\x07";
}

int main() {
    // A pty that repaints the viewport (ConPTY): with pull off, a resize that
    // frees screen rows must not bring history down (the repaint would paint
    // over it). Every history line stays history, at every step.
    {
        term::Config cfg;
        cfg.scrollback_lines = 10000;
        cfg.pull_history_on_resize = false;
        term::Model m{cfg, Extent{100, 30}};
        (void)term::feed_output(m, lines(0, 300, 100));
        // the lines entirely in history before the drag
        std::vector<int> hist_before;
        {
            const auto all = markers(m);
            const auto on_screen = static_cast<std::size_t>(m.screen.size().rows);
            hist_before.assign(all.begin(), all.end() - static_cast<std::ptrdiff_t>(std::min(all.size(), on_screen)));
        }
        for (int k = 0; k < 12; ++k) {
            term::resize(m, Extent{70 + (k * 23) % 60, 20 + (k * 7) % 15});
            // the repaint: the pty redraws the screen it holds -- wiped here; history must survive it
            (void)term::feed_output(m, "\x1b[H\x1b[J");
        }
        const auto after = markers(m);
        bool kept = after.size() >= hist_before.size();
        for (std::size_t i = 0; kept && i < hist_before.size(); ++i) kept = after[i] == hist_before[i];
        if (!kept) {
            std::printf("FAIL no-pull: %zu of %zu history lines survived resizes + viewport repaints\n", after.size(), hist_before.size());
            ++failures;
        }
    }
    for (int scenario = 0; scenario < 6; ++scenario) {
        term::Config cfg;
        cfg.scrollback_lines = 10000;
        term::Model m{cfg, Extent{120, 40}};
        int n = scenario < 3 ? 300 : 3000;
        (void)term::feed_output(m, lines(0, n, 120));
        check(m, n, "written");
        char what[64];
        // a window resize, then a drag back and forth
        term::resize(m, Extent{100, 40});
        std::snprintf(what, sizeof what, "s%d first resize", scenario);
        check(m, n, what);
        for (int c = 100; c >= 60; c -= 3) term::resize(m, Extent{c, 35});
        for (int c = 60; c <= 140; c += 7) term::resize(m, Extent{c, 45});
        std::snprintf(what, sizeof what, "s%d drag", scenario);
        check(m, n, what);
        // output, then resize again (history in the log AND in the ring)
        (void)term::feed_output(m, lines(n, n + 200, 140));
        n += 200;
        term::resize(m, Extent{90, 40});
        std::snprintf(what, sizeof what, "s%d output + resize", scenario);
        check(m, n, what);
        if (scenario % 2) {   // the alternate screen, resized while in it
            (void)term::feed_output(m, "\x1b[?1049h\x1b[2Jalt\x1b[?1049l");
            (void)term::feed_output(m, "\x1b[?1049h");
            term::resize(m, Extent{110, 38});
            (void)term::feed_output(m, "\x1b[?1049l");
            std::snprintf(what, sizeof what, "s%d alt screen", scenario);
            check(m, n, what);
        }
        // many small output+resize rounds (a live window resize while a command runs)
        for (int r = 0; r < 20; ++r) {
            (void)term::feed_output(m, lines(n, n + 7, 80));
            n += 7;
            term::resize(m, Extent{80 + (r % 5) * 9, 40 - (r % 3)});
        }
        std::snprintf(what, sizeof what, "s%d rounds", scenario);
        check(m, n, what);
    }
    std::printf(failures ? "history_resize: %d FAILURES\n" : "history_resize: PASS\n", failures);
    return failures ? 1 : 0;
}
