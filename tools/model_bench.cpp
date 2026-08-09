// SPDX-License-Identifier: LGPL-2.0-or-later
//
// model_bench — measures the MODEL pipeline (parser + Screen apply) in
// isolation, no GL, no PTY. This is where scroll/alt-screen/wide-char costs
// live; the renderer is benched separately. Build is wired by CMake as a test.
//   Reports MB/s per workload so we can see model-side bottlenecks directly.

#include <chrono>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"

using namespace toe::term;
using Clock = std::chrono::steady_clock;

namespace {

// Feed `data` `reps` times through parser -> screen. Returns MB/s.
double run(const char *name, const std::string &data, int reps) {
    Screen scr{toe::Extent{80, 24}};
    toe::vt::Parser parser;
    // warm
    parser.feed(std::span<const char>{data.data(), data.size()},
                [&](const toe::vt::Action &a) { toe::Cmds out; scr.apply(a, out); });
    const auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i)
        parser.feed(std::span<const char>{data.data(), data.size()},
                    [&](const toe::vt::Action &a) { toe::Cmds out; scr.apply(a, out); });
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const double mb = double(data.size()) * reps / 1e6;
    std::printf("  %-11s %8.1f MB/s   (%.1f MB in %.3fs)\n", name, mb / secs, mb, secs);
    return mb / secs;
}

std::string rep(const std::string &s, int n) {
    std::string o; o.reserve(s.size() * n);
    for (int i = 0; i < n; ++i) o += s;
    return o;
}

} // namespace

int main() {
    std::mt19937 rng(1);
    const std::string CSI = "\x1b[";

    // ascii
    std::string ascii = rep("The quick brown fox jumps over the lazy dog.\r\n", 2000);
    // scroll (lots of newlines)
    std::string scroll = rep("line of text content goes here and here\r\n", 4000);
    // sgr truecolor per cell
    std::string sgr;
    for (int i = 0; i < 4000; ++i) {
        int r = (i*7)&255, g=(i*13)&255, b=(i*29)&255;
        sgr += "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
               std::to_string(b) + "m#";
    }
    // cursor warps
    std::string cursor;
    for (int i = 0; i < 4000; ++i)
        cursor += CSI + std::to_string((i%24)+1) + ";" + std::to_string((i%80)+1) + "H@";
    // dense cjk
    const char *cjk[] = {"漢","字","日","本","語","中","文","韓","國","言"};
    std::string dense;
    for (int i = 0; i < 4000; ++i) { dense += cjk[i%10]; if (i%40==39) dense += "\r\n"; }
    // scroll region ping-pong
    std::string regions;
    for (int i = 0; i < 3000; ++i) {
        int top=(i%20)+1, bot=top+3;
        regions += CSI + std::to_string(top) + ";" + std::to_string(bot) + "r" +
                   CSI + std::to_string(bot) + ";1H\n\ndata";
    }
    regions += CSI + "r";
    // alt-screen thrash
    std::string alt = rep(CSI + "?1049h" + CSI + "2J" + CSI + "H" + "content\r\n" +
                          CSI + "?1049l", 4000);
    // insert/delete lines
    std::string idl;
    for (int i = 0; i < 3000; ++i)
        idl += CSI + "5;1H" + CSI + "3L" + CSI + "2M";
    // combining marks
    std::string comb;
    for (int i = 0; i < 3000; ++i) { comb += "a\xcc\x80\xcc\x81\xcc\x82"; if (i%40==39) comb += "\r\n"; }

    std::printf("model pipeline throughput (parser + Screen::apply, 80x24):\n");
    run("ascii", ascii, 200);
    run("scroll", scroll, 100);
    run("sgr-color", sgr, 100);
    run("cursor", cursor, 100);
    run("dense-cjk", dense, 100);
    run("regions", regions, 100);
    run("altscreen", alt, 100);
    run("ins/del-ln", idl, 100);
    run("combining", comb, 100);
    return 0;
}
