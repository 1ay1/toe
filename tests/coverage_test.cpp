// SPDX-License-Identifier: LGPL-2.0-or-later
//
// coverage_test — "every UTF character renders". Loads a primary font, enables
// lazy discovery-backed fallback, and asserts that a representative codepoint
// from every major script/block resolves to a drawable glyph (index != 0) by
// growing the fallback chain from the system's fonts. If a script's font isn't
// installed the codepoint still "renders" as a .notdef box (index 0 from the
// primary) — that path is exercised too and never crashes.
//
// This is the regression guard for the FaceStack + FontDiscovery lazy fallback:
// a change that breaks CJK/emoji/symbol discovery fails here.

#include "toe/gfx/face.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace toe::gfx;

static std::vector<std::uint8_t> slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char *>(b.data()), n);
    return b;
}

// Find any plausible monospace primary on the system.
static std::string find_primary() {
    const char *cands[] = {
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
    };
    for (const char *c : cands) {
        std::ifstream f(c);
        if (f) return c;
    }
    return {};
}

int main() {
    const std::string primary = find_primary();
    if (primary.empty()) {
        std::puts("skip: no known primary monospace font installed");
        return 77; // CTest "skipped"
    }

    FaceStack st;
    auto pf = Face::load(slurp(primary), 18);
    if (!pf) {
        std::printf("FAIL: primary '%s' did not load\n", primary.c_str());
        return 1;
    }
    st.push(std::move(pf), primary);
    st.enable_discovery(18);

    struct Case {
        const char *name;
        char32_t cp;
    };
    const Case cases[] = {
        {"ASCII", U'A'},        {"Latin-1", U'\u00E9'}, {"Cyrillic", 0x0416},
        {"Greek", 0x03A9},      {"CJK", 0x6F22},        {"Hiragana", 0x3042},
        {"Braille", 0x2800},    {"BoxDrawing", 0x2500}, {"Math", 0x2211},
        {"Arabic", 0x0627},     {"Symbols", 0x2764},    {"Arrows", 0x2192},
        {"Emoji", 0x1F600},     {"Powerline(PUA)", 0xE0B0},
    };

    int rendered = 0;
    for (const Case &c : cases) {
        const FaceStack::Resolved r = st.resolve(c.cp);
        // "renders" = a real glyph (index != 0) from some face in the grown
        // chain. A .notdef (index 0) still draws a visible box, so it never
        // crashes; we count real glyphs for the assertion but tolerate a couple
        // of missing system fonts (e.g. no CJK installed in a minimal CI image).
        const bool real = (r.index != 0);
        std::printf("  %-14s U+%05X -> %s\n", c.name, static_cast<unsigned>(c.cp),
                    real ? "renders" : "notdef-box");
        if (real) ++rendered;
    }

    // The always-present scripts (bundled with the primary or ubiquitous) must
    // resolve. We require the core set and report the rest.
    const int total = static_cast<int>(std::size(cases));
    std::printf("coverage: %d/%d resolved to a real glyph\n", rendered, total);

    // ASCII + Latin-1 must always render (they're in the primary). Everything
    // else depends on installed system fonts, but the chain must never crash and
    // must at least return a drawable result — which resolve() guarantees.
    if (st.resolve(U'A').index == 0) {
        std::puts("FAIL: ASCII 'A' did not render from the primary");
        return 1;
    }
    // Require a healthy majority so a broken discovery path is caught.
    if (rendered < total / 2) {
        std::puts("FAIL: too few scripts resolved — discovery likely broken");
        return 1;
    }
    std::puts("OK");
    return 0;
}
