// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The damage keys are a promise to every cache built on them: a renderer that
// reuses a row because row_version() held still, or a minimap that keeps its
// history bins because history_epoch() held still, draws WRONG pixels the
// moment a mutation forgets to bump them. So this drives the model with
// random, escape-heavy streams (scrolls, regions, erases, alt screen, RIS,
// resizes, scrollback clears) and checks the promise after every chunk:
//
//   row_version(r) unchanged and nonzero  =>  row r's cells are unchanged
//   history_epoch() unchanged             =>  every history row is unchanged
//
// Deterministic (fixed seeds) so a failure reproduces.

#include "toe/term/update.hpp"

#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace toe;

static int failures = 0;

namespace {

using Rows = std::vector<std::vector<term::Cell>>;

Rows live_rows(const term::Model &m) {
    Rows out;
    for (std::int32_t r = 0; r < m.screen.size().rows; ++r) {
        const auto row = m.screen.row(Row{r});
        out.emplace_back(row.begin(), row.end());
    }
    return out;
}

Rows history_rows(const term::Model &m) {
    Rows out;
    for (std::int64_t r = 0; r < m.screen.history_rows(); ++r) {
        const auto row = m.screen.abs_row_cells(r);
        out.emplace_back(row.begin(), row.end());
    }
    return out;
}

std::vector<std::uint64_t> versions(const term::Model &m) {
    std::vector<std::uint64_t> v;
    for (std::int32_t r = 0; r < m.screen.size().rows; ++r) v.push_back(m.screen.row_version(r));
    return v;
}

void check(const term::Model &before_m, const Rows &live0, const Rows &hist0, const std::vector<std::uint64_t> &ver0,
           std::uint64_t hist_epoch0, const term::Model &m, const char *what, unsigned seed, int step) {
    const auto live1 = live_rows(m);
    const auto ver1 = versions(m);
    (void)before_m;
    if (live1.size() == live0.size() && m.screen.scroll_offset() == 0) {
        for (std::size_t r = 0; r < live1.size(); ++r) {
            if (ver1[r] == 0 || ver1[r] != ver0[r]) continue;
            if (live1[r] != live0[r]) {
                std::printf("FAIL %s (seed %u step %d): row %zu changed but row_version held at %llu\n", what, seed, step, r,
                            static_cast<unsigned long long>(ver1[r]));
                ++failures;
                return;
            }
        }
    }
    if (m.screen.history_epoch() == hist_epoch0 && history_rows(m) != hist0) {
        std::printf("FAIL %s (seed %u step %d): history changed but history_epoch held at %llu\n", what, seed, step,
                    static_cast<unsigned long long>(hist_epoch0));
        ++failures;
    }
}

}  // namespace

int main() {
    // Escape-heavy alphabet: scrolls (IND/RI/SU/SD), regions, erases (ED/EL/
    // ECH/DCH/ICH/IL/DL), alt screen, RIS, DECALN, scrollback clear (3J).
    static const char *kOps[] = {
        "\r\n", "\n", "\x1bM", "\x1b" "D", "\x1b[2S", "\x1b[3T", "\x1b[3;8r", "\x1b[r", "\x1b[2J", "\x1b[3J",
        "\x1b[J", "\x1b[1J", "\x1b[K", "\x1b[1K", "\x1b[2K", "\x1b[3X", "\x1b[2P", "\x1b[2@", "\x1b[2L", "\x1b[2M",
        "\x1b[?1049h", "\x1b[?1049l", "\x1b[?47h", "\x1b[?47l", "\x1b" "c", "\x1b#8", "\x1b[H", "\x1b[5;3H",
        "\x1b[31m", "\x1b[0m", "\x1b[44m", "\xe4\xb8\xad", "e\xcc\x81", "\x1b]8;;http://x\x1b\\", "\x1b]8;;\x1b\\",
        "\x1b[?69h\x1b[2;9s", "\x1b[?69l", "\x1b[?6h", "\x1b[?6l", "\t", "\b", "\x1b[10C", "\x1b[3A",
    };
    for (unsigned seed = 1; seed <= 60; ++seed) {
        std::mt19937 rng{seed};
        term::Config cfg;
        cfg.scrollback_lines = 40;
        term::Model m{cfg, Extent{24, 10}};
        for (int step = 0; step < 300; ++step) {
            const auto live0 = live_rows(m);
            const auto hist0 = history_rows(m);
            const auto ver0 = versions(m);
            const std::uint64_t he0 = m.screen.history_epoch();
            std::string s;
            const int n = 1 + static_cast<int>(rng() % 6);
            for (int i = 0; i < n; ++i) {
                if (rng() % 3 == 0) s += kOps[rng() % (sizeof kOps / sizeof *kOps)];
                else s.append(1 + rng() % 12, static_cast<char>('a' + rng() % 26));
            }
            (void)term::feed_output(m, s);
            if (step % 37 == 36) {   // a resize (reflow or not) now and then
                m.screen.resize(Extent{8 + static_cast<std::int32_t>(rng() % 30), 3 + static_cast<std::int32_t>(rng() % 12)});
            }
            check(m, live0, hist0, ver0, he0, m, "random stream", seed, step);
            if (failures > 5) break;
        }
    }
    std::printf(failures ? "damage: %d FAILURES\n" : "damage: PASS\n", failures);
    return failures ? 1 : 0;
}
