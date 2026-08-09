// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Microbenchmark for the VT parser's ground-state scan (SIMD vs the workload).
// Feeds a large realistic buffer through Parser::feed and reports GB/s. Build:
//   c++ -std=c++23 -O3 -I include tools/parse_bench.cpp -o /tmp/pb && /tmp/pb

#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "toe/vt/parser.hpp"

using namespace toe::vt;

static double run(const std::string &data, int reps, const char *name) {
    Parser p;
    std::size_t sink_calls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) {
        p.feed(std::span<const char>{data.data(), data.size()},
               [&](const Action &) { ++sink_calls; });
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double gb = double(data.size()) * reps / 1e9;
    std::printf("  %-16s %6.2f GB/s   (%.2f GB in %.3fs, %zu sink calls)\n",
                name, gb / secs, gb, secs, sink_calls);
    return gb / secs;
}

int main() {
    const int REPS = 200;

    // 1) Pure printable ASCII — the pure SIMD ground-scan win.
    std::string ascii;
    ascii.reserve(1 << 20);
    while (ascii.size() < (1u << 20))
        ascii += "The quick brown fox jumps over the lazy dog 0123456789 ";

    // 2) Realistic: text with periodic newlines + occasional SGR (mixed).
    std::string mixed;
    mixed.reserve(1 << 20);
    while (mixed.size() < (1u << 20)) {
        mixed += "\x1b[38;5;42m";
        mixed += "some log line with a fair bit of ordinary text content here";
        mixed += "\x1b[0m\r\n";
    }

    // 3) Escape-heavy: short runs between many control sequences (SIMD helps less).
    std::string esc;
    esc.reserve(1 << 20);
    while (esc.size() < (1u << 20))
        esc += "\x1b[1;32mX\x1b[0m\x1b[2;3H\x1b[K";

    std::printf("VT parser throughput (Parser::feed):\n");
    run(ascii, REPS, "ascii");
    run(mixed, REPS, "mixed-sgr");
    run(esc, REPS, "escape-heavy");
    return 0;
}
