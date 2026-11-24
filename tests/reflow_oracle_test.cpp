// SPDX-License-Identifier: LGPL-2.0-or-later
//
// reflow_oracle_test — a resize is a pure function of (state, new size), and
// this pins that function down bit for bit.
//
// Thousands of seeded random screens (hard and soft-wrapped lines, wide
// glyphs, colours and BCE erases, prompts protected and not, scrolled into
// history or live, histories under and AT the scrollback cap) are each put
// through a run of random resizes. After every resize EVERYTHING observable
// is folded into one FNV-1a digest: every cell of every row (history and
// live), each row's soft-wrap flag, the cursor, the scroll offset, the
// history length, and where every pre-resize absolute row went
// (remap_abs_after_resize).
//
// The digest is checked against kGolden, recorded from the reference
// implementation. A change to reflow that is meant to be pure engineering
// (faster, less memory, incremental) must leave it unchanged; one that means
// to change behaviour updates kGolden in the same commit, on purpose.
//
//   reflow_oracle_test            check against kGolden
//   reflow_oracle_test --print    print the digest (to record a new golden)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"

using namespace toe;

namespace {

// Recorded from the reference reflow (toe 4a78ede, before any of the speed
// work; 06fd611 reproduces it exactly). See the header comment.
constexpr std::uint64_t kGolden = 0x88129721eaa71eaaull;

struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void byte(std::uint8_t b) noexcept { h = (h ^ b) * 0x100000001b3ull; }
    void u64(std::uint64_t v) noexcept { for (int i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i))); }
    void i64(std::int64_t v) noexcept { u64(static_cast<std::uint64_t>(v)); }
    // A Color's value, by its public face (never its padding bytes).
    void color(const term::Color &c) noexcept {
        u64(c.index());
        if (auto t = c.as<term::TrueColor>()) { byte(t->rgb.r); byte(t->rgb.g); byte(t->rgb.b); }
        if (auto x = c.as<term::IndexedColor>()) byte(x->index);
    }
    void cell(const term::Cell &c) noexcept {
        u64(c.cp);
        byte(c.width);
        byte(c.marks);
        u64(c.link);
        u64(static_cast<std::uint64_t>(c.pen.attr));
        u64(static_cast<std::uint64_t>(c.pen.underline));
        color(c.pen.fg);
        color(c.pen.bg);
        color(c.pen.underline_color);
    }
};

void feed(term::Screen &s, std::string_view b) {
    vt::Parser p;
    p.feed(std::span<const char>{b.data(), b.size()}, [&](const vt::Action &a) { toe::Cmds o; s.apply(a, o); });
}

/// Everything a host can observe, after a resize that came from `old_total` rows.
void digest(Fnv &f, term::Screen &s, std::int64_t old_total) {
    const auto sz = s.size();
    f.i64(sz.cols);
    f.i64(sz.rows);
    f.i64(s.history_rows());
    f.i64(s.scroll_offset());
    f.i64(s.cursor().row.get());
    f.i64(s.cursor().col.get());
    const std::int64_t total = s.total_rows();
    f.i64(total);
    for (std::int64_t a = 0; a < total; ++a) {
        for (const auto &c : s.abs_row_cells(a)) f.cell(c);
        f.byte(s.wrapped_abs(a) ? 1 : 0);
    }
    for (std::int64_t a = 0; a < old_total + 2; ++a) f.i64(s.remap_abs_after_resize(a));
}

/// A random screen: `lines` lines of random shape at width `cols`.
std::string random_history(std::mt19937_64 &rng, int lines, int maxlen) {
    std::string out;
    std::uniform_int_distribution<int> len(0, maxlen), kind(0, 99), ch(0, 25), col(0, 255);
    static constexpr const char *kWide[] = {"\xe4\xb8\xad", "\xe6\x96\x87", "\xf0\x9f\x98\x80", "\xe5\xad\x97"};
    for (int i = 0; i < lines; ++i) {
        const int k = kind(rng);
        if (k < 6) out += "\x1b[48;5;" + std::to_string(col(rng)) + "m\x1b[K\x1b[0m";   // BCE erase: coloured blanks
        if (k < 20) out += "\x1b[3" + std::to_string(ch(rng) % 8) + "m";
        if (k >= 90) out += "\x1b[38;2;" + std::to_string(col(rng)) + ";" + std::to_string(col(rng)) + ";9m";
        const int n = len(rng);
        for (int j = 0; j < n; ++j) {
            const int r = kind(rng);
            if (r < 8) out += kWide[r % 4];
            else if (r < 12) out += ' ';
            else out += static_cast<char>('a' + ch(rng));
        }
        out += "\x1b[0m";
        if (k % 7 == 0) out += "   ";   // trailing spaces: real text, not padding
        out += (i + 1 < lines || k % 3) ? "\r\n" : "";
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
    Fnv f;
    std::uint64_t cases = 0, resizes = 0;
    for (std::uint64_t seed = 1; seed <= 600; ++seed) {
        std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull);
        std::uniform_int_distribution<int> cols(4, 90), rows(2, 24), cap(0, 3), count(1, 6), coin(0, 9);
        const int c0 = cols(rng), r0 = rows(rng);
        term::Screen s(Extent{c0, r0});
        // scrollback: unlimited-ish, small (so the cap evicts), or none
        const int cp = cap(rng);
        s.set_max_history(cp == 0 ? 0u : cp == 1 ? 7u : cp == 2 ? 40u : 1000u);
        const int lines = std::uniform_int_distribution<int>(0, 120)(rng);
        feed(s, random_history(rng, lines, c0 * 3));
        // sometimes leave the cursor mid-line, at a prompt-like position
        if (coin(rng) < 4) feed(s, "$ " + std::string(static_cast<std::size_t>(std::uniform_int_distribution<int>(0, c0 * 2)(rng)), 'p'));
        // sometimes read history
        if (coin(rng) < 3) s.scroll(std::uniform_int_distribution<int>(1, 50)(rng));
        ++cases;
        const int n = count(rng);
        for (int i = 0; i < n; ++i) {
            const Extent to{cols(rng), coin(rng) < 6 ? s.size().rows : rows(rng)};
            const std::int64_t old_total = s.total_rows();
            // a protected prompt: from the cursor's row, or a few rows above it
            std::int64_t keep = -1;
            if (coin(rng) < 3) {
                const std::int64_t cur = s.history_rows() + s.cursor().row.get();
                keep = cur - std::uniform_int_distribution<int>(0, 2)(rng);
            }
            s.resize(to, keep);
            ++resizes;
            digest(f, s, old_total);
        }
    }
    std::printf("reflow oracle: %llu screens, %llu resizes, digest 0x%016llx\n", static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(resizes), static_cast<unsigned long long>(f.h));
    if (print) return 0;
    if (f.h != kGolden) {
        std::printf("FAIL reflow output changed (golden 0x%016llx). Intended? Re-record with --print.\n",
                    static_cast<unsigned long long>(kGolden));
        return 1;
    }
    std::printf("ok   every resize is bit-identical to the reference\n");
    return 0;
}
