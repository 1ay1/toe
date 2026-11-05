// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Robustness fuzz: a terminal is fed bytes by an untrusted child, so NO input
// may crash it, hang it, or corrupt memory. This drives the model with random
// and adversarially-shaped byte streams and asserts only that we survive with
// the invariants intact -- it is meant to be run under ASan/UBSan, where any
// out-of-bounds cell write or signed overflow becomes a hard failure.
//
//   cmake -DCMAKE_BUILD_TYPE=Debug -DTOE_SANITIZE=ON && ctest -R fuzz
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

static void expect(bool cond, const char *what) {
    if (!cond) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Everything a terminal must still be true about itself, whatever it was fed.
static void check_invariants(const term::Model &m, const char *what) {
    const Extent sz = m.screen.size();
    const Pos cur = m.screen.cursor();
    expect(cur.row.get() >= 0 && cur.row.get() < sz.rows, what);
    expect(cur.col.get() >= 0 && cur.col.get() <= sz.cols, what);
    expect(m.screen.history_rows() >= 0, what);
    expect(m.screen.total_rows() >= sz.rows, what);
    // Every visible row must be exactly `cols` wide and readable.
    for (std::int32_t r = 0; r < sz.rows; ++r) {
        const std::span<const term::Cell> row = m.screen.row(Row{r});
        expect(static_cast<std::int32_t>(row.size()) == sz.cols, what);
        // Touch every cell so a sanitizer sees any bad read.
        std::uint32_t acc = 0;
        for (const term::Cell &c : row) acc += c.cp + c.width + c.marks + c.link;
        (void)acc;
    }
    // Text extraction must not walk off the ring.
    (void)m.screen.text_between_abs(0, m.screen.total_rows());
}

int main() {
    // --- 1. purely random bytes -------------------------------------------
    for (unsigned seed = 1; seed <= 24; ++seed) {
        std::mt19937 rng{seed};
        term::Config cfg;
        cfg.scrollback_lines = 100;
        term::Model m{cfg, Extent{40, 12}};
        for (int chunk = 0; chunk < 40; ++chunk) {
            std::string s;
            const int n = static_cast<int>(rng() % 256);
            for (int i = 0; i < n; ++i) s += static_cast<char>(rng() & 0xFF);
            (void)term::feed_output(m, s);
        }
        check_invariants(m, "random bytes keep invariants");
    }

    // --- 2. random bytes biased toward escape sequences -------------------
    // Pure random rarely produces a valid CSI; bias the alphabet so the
    // sequence machinery actually gets exercised.
    static const char kAlpha[] = "\x1b[]();?>=!$\"'0123456789;:mHJKLMPSTXrhlqnctfdABCDEGZ@bu"
                                 "\x07\x08\x09\x0a\x0d\x0e\x0f\x18\x1a\x7f\x9b\x90\x9d"
                                 "abcxyz \xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80\xff\x80";
    for (unsigned seed = 100; seed <= 140; ++seed) {
        std::mt19937 rng{seed};
        term::Config cfg;
        cfg.scrollback_lines = 50;
        term::Model m{cfg, Extent{30, 8}};
        for (int chunk = 0; chunk < 60; ++chunk) {
            std::string s;
            const int n = static_cast<int>(rng() % 128);
            for (int i = 0; i < n; ++i) s += kAlpha[rng() % (sizeof kAlpha - 1)];
            (void)term::feed_output(m, s);
        }
        check_invariants(m, "escape-biased fuzz keeps invariants");
    }

    // --- 3. adversarial shapes --------------------------------------------
    // Each of these has broken a terminal somewhere: unbounded parameter
    // counts, huge values, deeply nested or never-terminated strings.
    const std::string huge_params = [] {
        std::string s = "\x1b[";
        for (int i = 0; i < 5000; ++i) s += "1;";
        return s + "H";
    }();
    const std::string huge_value   = "\x1b[" + std::string(400, '9') + "H";
    const std::string huge_osc     = "\x1b]0;" + std::string(200000, 'A') + "\x07";
    const std::string unterm_osc   = "\x1b]0;" + std::string(100000, 'B');
    const std::string huge_dcs     = "\x1bP" + std::string(200000, 'C') + "\x1b\\";
    const std::string unterm_dcs   = "\x1bP" + std::string(100000, 'D');
    const std::string huge_apc     = "\x1b_G" + std::string(200000, 'E') + "\x1b\\";
    const std::string esc_storm    = [] {
        std::string s;
        for (int i = 0; i < 20000; ++i) s += "\x1b";
        return s;
    }();
    const std::string csi_storm    = [] {
        std::string s;
        for (int i = 0; i < 20000; ++i) s += "\x1b[";
        return s;
    }();
    const std::string sub_storm = [] {
        std::string s = "\x1b[38";
        for (int i = 0; i < 5000; ++i) s += ":1";
        return s + "m x";
    }();
    const std::string ris_storm = [] {
        std::string s;
        for (int i = 0; i < 20000; ++i) s += "\033c";
        return s;
    }();
    const std::string margin_abuse =
        "\x1b[?69h\x1b[9999;1s\x1b[9999;1r\x1b[1;1H" + std::string(200, 'x');
    const std::string rect_abuse =
        "\x1b[?69h\x1b[1;1;9999;9999$x\x1b[0;0;0;0$z\x1b[9999;9999;1;1$v";
    const std::string tab_abuse = [] {
        std::string s = "\x1b[3g";
        for (int i = 0; i < 2000; ++i) s += "\x1bH\t";
        return s;
    }();
    const std::string scroll_abuse = [] {
        std::string s = "\x1b[2;5r";
        for (int i = 0; i < 5000; ++i) s += "\x1bM\033D";
        return s;
    }();
    const std::string wide_edge = [] {
        std::string s;
        for (int i = 0; i < 2000; ++i) s += "\xe4\xb8\xad"; // CJK, forces wrap
        return s;
    }();
    const std::string mark_storm = [] {
        std::string s = "a";
        for (int i = 0; i < 5000; ++i) s += "\xcc\x81"; // combining acute
        return s;
    }();
    const std::string link_storm = [] {
        std::string s;
        for (int i = 0; i < 2000; ++i)
            s += "\x1b]8;;http://e/" + std::to_string(i) + "\x1b\\x";
        return s;
    }();

    const struct { const char *name; const std::string *data; } shapes[] = {
        {"5000 CSI params", &huge_params},
        {"400-digit param", &huge_value},
        {"200KB OSC", &huge_osc},
        {"unterminated OSC", &unterm_osc},
        {"200KB DCS", &huge_dcs},
        {"unterminated DCS", &unterm_dcs},
        {"200KB APC", &huge_apc},
        {"20000 bare ESC", &esc_storm},
        {"20000 bare CSI", &csi_storm},
        {"5000 SGR subparams", &sub_storm},
        {"20000 RIS", &ris_storm},
        {"out-of-range margins", &margin_abuse},
        {"out-of-range rectangles", &rect_abuse},
        {"2000 tab stops", &tab_abuse},
        {"5000 RI/IND in region", &scroll_abuse},
        {"2000 wide glyphs", &wide_edge},
        {"5000 combining marks", &mark_storm},
        {"2000 hyperlinks", &link_storm},
    };
    for (const auto &sh : shapes) {
        term::Config cfg;
        cfg.scrollback_lines = 200;
        term::Model m{cfg, Extent{40, 10}};
        (void)term::feed_output(m, *sh.data);
        check_invariants(m, sh.name);
        // And again byte-at-a-time: the parser must reach the same kind of
        // consistent state regardless of how the stream is chunked.
        term::Model m2{cfg, Extent{40, 10}};
        for (char c : *sh.data) (void)term::feed_output(m2, std::string_view{&c, 1});
        check_invariants(m2, sh.name);
    }

    // --- 4. resize under adversarial content ------------------------------
    // Reflow is the most intricate code in the library; drive it hard.
    {
        std::mt19937 rng{7};
        term::Config cfg;
        cfg.scrollback_lines = 80;
        term::Model m{cfg, Extent{40, 10}};
        for (int i = 0; i < 300; ++i) {
            std::string s;
            const int n = static_cast<int>(rng() % 64);
            for (int k = 0; k < n; ++k) s += kAlpha[rng() % (sizeof kAlpha - 1)];
            (void)term::feed_output(m, s);
            const std::int32_t cols = 1 + static_cast<std::int32_t>(rng() % 120);
            const std::int32_t rows = 1 + static_cast<std::int32_t>(rng() % 40);
            m.screen.resize(Extent{cols, rows});
            check_invariants(m, "resize under fuzz keeps invariants");
        }
    }

    // --- 5. a 1x1 terminal ------------------------------------------------
    // Degenerate geometry: every margin/wrap calculation has no room at all.
    {
        term::Config cfg;
        term::Model m{cfg, Extent{1, 1}};
        (void)term::feed_output(m, "abc\r\n\x1b[2J\xe4\xb8\xad\x1b[5;5Hx\t\t\x1bM\033D");
        check_invariants(m, "1x1 terminal survives");
    }

    std::printf(failures ? "fuzz: %d FAILURES\n" : "fuzz: PASS\n", failures);
    return failures ? 1 : 0;
}
