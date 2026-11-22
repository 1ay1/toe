// toe/tests/resize_bench.cpp — what a column change costs with a full scrollback.
#include "toe/term/update.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace toe;

int main() {
    term::Model m{term::Config{}, Extent{170, 42}};
    std::string hist;
    for (int i = 0; i < 10000; ++i)
        hist += "\x1b[32m" + std::to_string(i) + "\x1b[0m " + std::string(110, static_cast<char>('a' + i % 26)) + "\r\n";
    (void)term::feed_output(m, hist);
    using clk = std::chrono::steady_clock;
    constexpr int N = 40;
    const auto a = clk::now();
    for (int i = 0; i < N; ++i) term::resize(m, Extent{170 - (i % 2), 42});
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - a).count() / N;
    std::printf("resize, 10k-line scrollback x 170 cols: %.3f ms per column change\n", ms);
    return 0;
}
