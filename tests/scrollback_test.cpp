// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Scrollback store: append + wrap-on-demand projection at multiple widths from
// ONE store (the whole point — resize just re-projects), plus ring-cap trimming.

#include <cstdio>
#include <string>
#include <vector>

#include "toe/term/scrollback.hpp"

using namespace toe::term;

static int failures = 0;
static void expect(bool c, const char *n) {
    std::printf(c ? "ok   %s\n" : "FAIL %s\n", n);
    if (!c) ++failures;
}

static std::vector<Cell> mkline(const std::string &s) {
    std::vector<Cell> v;
    for (char c : s) {
        Cell cell;
        cell.cp = static_cast<char32_t>(static_cast<unsigned char>(c));
        v.push_back(cell);
    }
    return v;
}

static std::string proj_row(const Scrollback &sb, std::size_t line, int wrap, int cols) {
    std::vector<Cell> out(static_cast<std::size_t>(cols));
    sb.project(line, wrap, cols, out.data(), Cell{});
    std::string s;
    for (const Cell &c : out) s += (c.cp >= 0x20 && c.cp < 0x7f) ? static_cast<char>(c.cp) : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int main() {
    Scrollback sb(1000);
    // "abcdefghij" (10 chars) + "hi" + empty line.
    auto a = mkline("abcdefghij");
    auto b = mkline("hi");
    sb.push_line(a.data(), a.size());
    sb.push_line(b.data(), b.size());
    sb.push_line(nullptr, 0);

    expect(sb.line_count() == 3, "3 lines stored");

    // At width 10: line 0 is 1 row, exactly "abcdefghij".
    expect(sb.wrap_rows(0, 10) == 1, "10-char line = 1 row at width 10");
    expect(proj_row(sb, 0, 0, 10) == "abcdefghij", "project full at width 10");

    // At width 4: line 0 wraps to 3 rows: abcd|efgh|ij.
    expect(sb.wrap_rows(0, 4) == 3, "10-char line = 3 rows at width 4");
    expect(proj_row(sb, 0, 0, 4) == "abcd", "wrap row 0 at width 4");
    expect(proj_row(sb, 0, 1, 4) == "efgh", "wrap row 1 at width 4");
    expect(proj_row(sb, 0, 2, 4) == "ij", "wrap row 2 at width 4");

    // At width 3: 4 rows: abc|def|ghi|j.
    expect(sb.wrap_rows(0, 3) == 4, "10-char line = 4 rows at width 3");
    expect(proj_row(sb, 0, 3, 3) == "j", "wrap row 3 at width 3");

    // The SAME store projects correctly at any width with no rebuild — that's
    // the resize property. total_rows reflects the current width.
    expect(sb.total_rows(10) == 1 + 1 + 1, "total rows at width 10 = 3");   // 1+1+1(empty)
    expect(sb.total_rows(4) == 3 + 1 + 1, "total rows at width 4 = 5");     // 3+1+1
    expect(sb.total_rows(3) == 4 + 1 + 1, "total rows at width 3 = 6");     // 4+1+1
    // Re-query width 10 (cache path): still correct.
    expect(sb.total_rows(10) == 3, "total rows cache re-query width 10");

    // Soft-wrap flag: project returns true when a continuation follows.
    {
        std::vector<Cell> out(4);
        bool more0 = sb.project(0, 0, 4, out.data(), Cell{});
        bool more2 = sb.project(0, 2, 4, out.data(), Cell{});
        expect(more0 == true, "wrap row 0 reports soft-wrap continuation");
        expect(more2 == false, "final wrap row reports no continuation");
    }

    // Ring cap: cap to 3 lines, push 5, oldest two drop, content stays correct.
    {
        Scrollback cap(3);
        for (int i = 0; i < 5; ++i) {
            std::string s = "L" + std::to_string(i);
            auto l = mkline(s);
            cap.push_line(l.data(), l.size());
        }
        expect(cap.line_count() == 3, "ring cap holds 3 lines");
        // Oldest retained is L2, then L3, L4.
        expect(proj_row(cap, 0, 0, 10) == "L2", "oldest after cap is L2");
        expect(proj_row(cap, 2, 0, 10) == "L4", "newest after cap is L4");
    }

    // Stress: push many lines, force arena compaction, verify integrity.
    {
        Scrollback s2(100);
        for (int i = 0; i < 1000; ++i) {
            std::string line(50, 'a' + (i % 26));
            auto l = mkline(line);
            s2.push_line(l.data(), l.size());
        }
        expect(s2.line_count() == 100, "capped at 100 after 1000 pushes");
        // Last line is 'a'+(999%26).
        char expect_ch = 'a' + (999 % 26);
        std::string last = proj_row(s2, 99, 0, 60);
        expect(!last.empty() && last[0] == expect_ch, "newest line content intact after compaction");
    }

    std::printf(failures ? "scrollback: %d FAILURES\n" : "scrollback: PASS\n", failures);
    return failures ? 1 : 0;
}
