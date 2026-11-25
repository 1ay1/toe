// SPDX-License-Identifier: LGPL-2.0-or-later
//
// linelog_test -- LineLog's row view against a naive re-wrap, exhaustively:
// random logical lines (wide glyphs, blanks, colours), stored both as owned
// copies and as rows referenced IN PLACE in a frozen ring arena (wrapping
// around its end), read back at every width 1..40, under eviction (the cap),
// drop_back_rows (a widen), extend_open and take_open. Every row's cells,
// padding and soft flag must equal re-wrapping the line by hand.

#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "toe/term/linelog.hpp"

using namespace toe::term;

namespace {

int failures = 0;

struct Ref {   // the reference: lines as plain vectors
    std::vector<std::vector<Cell>> lines;
    bool open = false;
};

/// Rows of the reference at width w: {cells padded to w, soft}.
std::vector<std::pair<std::vector<Cell>, bool>> ref_rows(const Ref &r, std::int32_t w) {
    std::vector<std::pair<std::vector<Cell>, bool>> out;
    for (std::size_t li = 0; li < r.lines.size(); ++li) {
        const auto &ln = r.lines[li];
        const auto len = static_cast<std::uint32_t>(ln.size());
        std::uint32_t at = 0;
        do {
            const std::uint32_t take = LineLog::take_at([&](std::uint32_t i) -> const Cell & { return ln[i]; }, len, at, w);
            std::vector<Cell> row(static_cast<std::size_t>(w), Cell{});
            for (std::uint32_t i = 0; i < take && i < static_cast<std::uint32_t>(w); ++i) row[i] = ln[at + i];
            const bool more = at + take < len;
            out.emplace_back(std::move(row), more || (li + 1 == r.lines.size() && r.open));
            at += take;
        } while (at < len);
    }
    return out;
}

Cell rnd_cell(std::mt19937_64 &rng) {
    Cell c;
    const int k = static_cast<int>(rng() % 100);
    if (k < 15) return c;   // blank
    c.cp = U'a' + static_cast<char32_t>(rng() % 26);
    if (k % 7 == 0) c.pen.fg = IndexedColor{static_cast<std::uint8_t>(rng() % 16)};
    return c;
}

std::vector<Cell> rnd_line(std::mt19937_64 &rng) {
    std::vector<Cell> v;
    const int n = static_cast<int>(rng() % 70);
    for (int i = 0; i < n; ++i) {
        if (rng() % 10 == 0) {   // a wide pair
            Cell lead = rnd_cell(rng);
            lead.cp = U'\x4e2d';
            lead.width = 2;
            Cell sp{};
            sp.width = 0;
            sp.pen = lead.pen;
            v.push_back(lead);
            v.push_back(sp);
        } else {
            v.push_back(rnd_cell(rng));
        }
    }
    return v;
}

void check(const LineLog &log, const Ref &r, std::int32_t w, const char *what, unsigned seed) {
    const auto want = ref_rows(r, w);
    if (log.rows() != want.size()) {
        std::printf("FAIL %s seed %u w %d: rows %llu want %zu\n", what, seed, w,
                    static_cast<unsigned long long>(log.rows()), want.size());
        ++failures;
        return;
    }
    std::vector<Cell> got(static_cast<std::size_t>(w));
    for (std::size_t k = 0; k < want.size(); ++k) {
        bool soft = false;
        (void)log.paint(k, got.data(), w, soft);
        if (got != want[k].first || soft != want[k].second) {
            std::printf("FAIL %s seed %u w %d row %zu: %s\n", what, seed, w, k,
                        soft != want[k].second ? "soft flag" : "cells");
            ++failures;
            return;
        }
    }
}

/// Push `line` into the log in place: lay it out as rows of `cols` in a
/// frozen ring arena of `cap` slots starting at slot `slot` (wrapping).
void push_frozen(LineLog &log, const std::vector<Cell> &line, std::int32_t cols, std::size_t cap, std::size_t slot,
                 bool open, std::vector<std::shared_ptr<LineLog::Frozen>> &keep) {
    auto f = std::make_shared<LineLog::Frozen>();
    f->owner.reset(cap * static_cast<std::size_t>(cols), cap * static_cast<std::size_t>(cols), Cell{});
    f->data = f->owner.data();
    f->cap = cap;
    f->cols = cols;
    auto *data = const_cast<Cell *>(f->data);
    for (std::size_t i = 0; i < line.size(); ++i) {
        std::size_t s = slot + i / static_cast<std::size_t>(cols);
        if (s >= cap) s -= cap;
        data[s * static_cast<std::size_t>(cols) + i % static_cast<std::size_t>(cols)] = line[i];
    }
    keep.push_back(f);
    log.push_inplace(f, slot, static_cast<std::uint32_t>(line.size()), open);
}

}  // namespace

int main() {
    for (unsigned seed = 1; seed <= 400; ++seed) {
        std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull);
        LineLog log;
        Ref ref;
        std::vector<std::shared_ptr<LineLog::Frozen>> keep;
        const int n = 1 + static_cast<int>(rng() % 40);
        log.set_width(1 + static_cast<int>(rng() % 40));
        for (int i = 0; i < n; ++i) {
            auto line = rnd_line(rng);
            const bool last = i + 1 == n;
            const bool open = last && rng() % 3 == 0;
            if (rng() % 2) {
                log.push_owned(line.data(), static_cast<std::uint32_t>(line.size()), open);
            } else {
                const std::int32_t cols = 1 + static_cast<int>(rng() % 30);
                const std::size_t rows = line.empty() ? 1 : (line.size() + static_cast<std::size_t>(cols) - 1) / static_cast<std::size_t>(cols);
                const std::size_t cap = rows + rng() % 5;
                push_frozen(log, line, cols, cap, static_cast<std::size_t>(rng() % cap), open, keep);
            }
            ref.lines.push_back(std::move(line));
            ref.open = open;
        }
        for (std::int32_t w = 1; w <= 40; ++w) {
            log.set_width(w);
            check(log, ref, w, "read", seed);
        }
        if (log.open() && rng() % 2) {   // extend the open line
            auto more = rnd_line(rng);
            const bool still = rng() % 2;
            log.extend_open(more.data(), static_cast<std::uint32_t>(more.size()), still);
            ref.lines.back().insert(ref.lines.back().end(), more.begin(), more.end());
            ref.open = still;
            for (std::int32_t w = 1; w <= 40; w += 3) { log.set_width(w); check(log, ref, w, "extend_open", seed); }
        }
        // eviction at some width: the reference drops the same rows
        {
            const std::int32_t w = 1 + static_cast<int>(rng() % 40);
            log.set_width(w);
            auto rows = ref_rows(ref, w);
            const std::uint64_t ev = rows.empty() ? 0 : rng() % (rows.size() + 1);
            log.evict_rows(ev);
            // the reference: drop whole lines, then cut the front line's head
            std::uint64_t left = ev;
            while (left > 0 && !ref.lines.empty()) {
                auto &ln = ref.lines.front();
                const auto len = static_cast<std::uint32_t>(ln.size());
                std::uint32_t at = 0, r = 0;
                std::vector<std::uint32_t> offs;
                do { offs.push_back(at); at += LineLog::take_at([&](std::uint32_t i) -> const Cell & { return ln[i]; }, len, at, w); ++r; } while (at < len);
                if (left >= r) { left -= r; ref.lines.erase(ref.lines.begin()); if (ref.lines.empty()) ref.open = false; continue; }
                ln.erase(ln.begin(), ln.begin() + offs[left]);
                left = 0;
            }
            check(log, ref, w, "evict", seed);
            for (std::int32_t w2 = 1; w2 <= 40; w2 += 5) { log.set_width(w2); check(log, ref, w2, "evict+rewidth", seed); }
        }
        // drop_back_rows at some width
        {
            const std::int32_t w = 1 + static_cast<int>(rng() % 40);
            log.set_width(w);
            auto rows = ref_rows(ref, w);
            const std::uint64_t d = rows.empty() ? 0 : rng() % (rows.size() + 1);
            log.drop_back_rows(d);
            std::uint64_t left = d;
            while (left > 0 && !ref.lines.empty()) {
                auto &ln = ref.lines.back();
                const auto len = static_cast<std::uint32_t>(ln.size());
                std::uint32_t at = 0, r = 0;
                std::vector<std::uint32_t> offs;
                do { offs.push_back(at); at += LineLog::take_at([&](std::uint32_t i) -> const Cell & { return ln[i]; }, len, at, w); ++r; } while (at < len);
                if (left >= r) { left -= r; ref.lines.pop_back(); ref.open = false; continue; }
                ln.resize(offs[r - left]);
                ref.open = true;
                left = 0;
            }
            check(log, ref, w, "drop_back", seed);
            for (std::int32_t w2 = 1; w2 <= 40; w2 += 7) { log.set_width(w2); check(log, ref, w2, "drop_back+rewidth", seed); }
        }
        if (log.open()) {
            std::vector<Cell> got;
            (void)log.take_open(got);
            if (got != ref.lines.back()) { std::printf("FAIL take_open seed %u\n", seed); ++failures; }
            ref.lines.pop_back();
            ref.open = false;
            log.set_width(13);
            check(log, ref, 13, "take_open", seed);
        }
        if (failures > 8) break;
    }
    std::printf(failures ? "linelog: %d FAILURES\n" : "linelog: PASS\n", failures);
    return failures ? 1 : 0;
}
