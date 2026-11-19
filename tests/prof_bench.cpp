// prof_bench — loop one throughput workload for a profiler.
#include "toe/term/update.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "ascii";
    std::string s;
    if (which == "ascii") for (int i = 0; i < 4000; ++i) s += "the quick brown fox jumps over the lazy dog 0123456789\r\n";
    else if (which == "scroll") for (int i = 0; i < 8000; ++i) s += "line of scrolling output\n";
    else if (which == "crlf") for (int i = 0; i < 8000; ++i) s += "line of scrolling output\r\n";
    else for (int i = 0; i < 3000; ++i) { s += "\x1b[38;5;" + std::to_string(i % 256) + "m token \x1b[0m"; if (i % 8 == 7) s += "\r\n"; }
    toe::term::Config cfg;
    toe::term::Model m{cfg, toe::Extent{200, 50}};
    const int reps = argc > 2 ? std::atoi(argv[2]) : 3000;
    for (int i = 0; i < reps; ++i) (void)toe::term::feed_output(m, s);
    std::puts("done");
}
