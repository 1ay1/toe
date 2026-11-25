// toe/tests/resize_bench.cpp -- what a resize costs, at three scrollback
// sizes: the first resize after the history was written (its rows join the
// line log) and then a drag (a new width every column). The drag cost must
// not grow with the scrollback: history is logical lines, never re-wrapped.
#include "toe/term/update.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace toe;

int main() {
    using clk = std::chrono::steady_clock;
    for (int lines : {1000, 10000, 100000}) {
        term::Config cfg;
        cfg.scrollback_lines = lines;
        term::Model m{cfg, Extent{170, 42}};
        std::string hist;
        for (int i = 0; i < lines; ++i)
            hist += "\x1b[32m" + std::to_string(i) + "\x1b[0m " + std::string(110, static_cast<char>('a' + i % 26)) + "\r\n";
        (void)term::feed_output(m, hist);
        auto a = clk::now();
        term::resize(m, Extent{169, 42});
        const double cold = std::chrono::duration<double, std::milli>(clk::now() - a).count();
        // a drag: 120 column changes, each to a width not seen just before
        constexpr int N = 120;
        a = clk::now();
        for (int i = 0; i < N; ++i) term::resize(m, Extent{168 - (i % 60) - (i / 60) * 3, 42});
        const double drag = std::chrono::duration<double, std::milli>(clk::now() - a).count() / N;
        std::printf("%6d lines: first resize %.3f ms, then %.3f ms per column (a drag)\n", lines, cold, drag);
    }
}
