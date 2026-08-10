// Throughput benchmark for the parse+screen pipeline — the path that decides
// whether a terminal is fast. No GPU, no window: this isolates the CPU-side
// work (VT parse -> screen model) so a change can be attributed rather than
// guessed at.
//
// Workloads mirror tools/bench.py: plain ASCII, heavy SGR, scrolling, CJK, and
// a mixed stream.
#include "toe/term/update.hpp"
#include "toe/terminal.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double mib_per_s(std::size_t bytes, double secs) {
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / secs;
}

static void run(const char *name, const std::string &payload, int reps) {
    // Best-of-N, not a single timing. Run-to-run spread on a modern laptop is
    // easily 10% (turbo/thermal drift, scheduler noise, E-core vs P-core
    // placement), which is larger than most real optimisations — so a single
    // sample can "prove" a change that did nothing. The BEST run is the least
    // noise-contaminated estimate of the true cost, since noise only ever adds
    // time.
    constexpr int kRounds = 7;
    double best = 1e30;

    for (int round = 0; round < kRounds; ++round) {
        toe::Config cfg;
        toe::term::Model model{cfg, toe::Extent{200, 50}};
        (void)toe::term::feed_output(model, payload); // warm caches + branch predictors

        const auto t0 = Clock::now();
        for (int i = 0; i < reps; ++i) (void)toe::term::feed_output(model, payload);
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        if (secs < best) best = secs;
    }

    const std::size_t total = payload.size() * static_cast<std::size_t>(reps);
    std::printf("%-14s %8.1f MiB/s  (best of %d)\n", name, mib_per_s(total, best), kRounds);
}

int main() {
    // 1. Plain ASCII lines — the `cat a big file` case.
    std::string ascii;
    for (int i = 0; i < 4000; ++i)
        ascii += "the quick brown fox jumps over the lazy dog 0123456789\r\n";

    // 2. Heavy SGR — colour-per-token output (ls --color, a build log).
    std::string sgr;
    for (int i = 0; i < 3000; ++i) {
        sgr += "\x1b[38;5;";
        sgr += std::to_string(i % 256);
        sgr += "m token \x1b[0m";
        if (i % 8 == 7) sgr += "\r\n";
    }

    // 3. Scroll storm — forces the rowring to recycle constantly.
    std::string scroll;
    for (int i = 0; i < 8000; ++i) scroll += "line of scrolling output\n";

    // 4. Dense CJK — double-width path + UTF-8 decode.
    std::string cjk;
    for (int i = 0; i < 3000; ++i) cjk += "\xe6\xbc\xa2\xe5\xad\x97\xe3\x81\x8b\xe3\x81\xaa\r\n";

    // 5. Cursor addressing — absolute moves, the TUI/vim pattern.
    std::string cup;
    for (int i = 0; i < 6000; ++i) {
        cup += "\x1b[";
        cup += std::to_string((i % 50) + 1);
        cup += ";";
        cup += std::to_string((i % 200) + 1);
        cup += "H*";
    }

    run("ascii", ascii, 40);
    run("sgr", sgr, 40);
    run("scroll", scroll, 40);
    run("cjk", cjk, 40);
    run("cursor", cup, 40);
    return 0;
}
