// yes_bench — toe alone on the `yes` workload (3-byte lines, scroll per line)
// with hand's settings: 119x34, 10k history. Splits parse from the update
// around it so a regression can be attributed.
#include "toe/term/update.hpp"

#include <chrono>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const int hist = argc > 1 ? std::atoi(argv[1]) : 10000;
    std::string y;
    while (y.size() + 3 <= (1u << 20)) y += "y\r\n";
    toe::term::Config cfg;
    cfg.scrollback_lines = hist;
    toe::term::Model m{cfg, toe::Extent{119, 34}};
    m.screen.set_max_history(static_cast<std::size_t>(hist));
    for (int i = 0; i < 12; ++i) (void)toe::term::feed_output(m, y);   // fill history: steady state
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 7;
    double best = 1e30;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) (void)toe::term::feed_output(m, y);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10);
    }
    std::printf("yes: %.2f ms/MB (%.0f MB/s), history %d\n", best, 1000.0 / best, hist);
}
