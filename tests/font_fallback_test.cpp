// Font fallback coverage test. The promise is "every UTF character renders":
// when the primary font lacks a glyph, FontDiscovery must find a system font
// that has it. This asserts that promise across the scripts a terminal actually
// meets, on whatever platform it's running.
//
// Exit 77 (ctest "skipped") on a machine with no usable font tree, so this is
// safe in a minimal container.
#include "toe/gfx/font_discovery.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    toe::gfx::FontDiscovery fd;
    const std::vector<std::string> none;

    struct Probe {
        char32_t cp;
        const char *what;
        bool required; // must resolve for the test to pass
    };
    // Codepoints deliberately absent from a typical monospace coding font, so
    // each one genuinely exercises the fallback path.
    const Probe probes[] = {
        {0x00E9, "Latin-1 e-acute", true},
        {0x03B1, "Greek alpha", true},
        {0x0416, "Cyrillic Zhe", true},
        {0x4E2D, "CJK ideograph", true},
        {0x3042, "Hiragana A", true},
        {0xAC00, "Hangul GA", true},
        {0x2500, "Box drawing", true},
        {0x2588, "Full block", true},
        {0x2764, "Heavy black heart", true},
        {0x1F600, "Emoji grinning face", false}, // colour emoji: not on every box
        {0x05D0, "Hebrew Alef", false},
        {0x0627, "Arabic Alef", false},
        {0x0905, "Devanagari A", false},
        {0x20AC, "Euro sign", true},
        {0x2192, "Rightwards arrow", true},
    };

    int resolved = 0, required_missing = 0, total = 0;
    for (const auto &p : probes) {
        ++total;
        const auto path = fd.resolve(p.cp, none);
        if (path) {
            ++resolved;
            std::printf("  U+%04X %-24s -> %s\n", static_cast<unsigned>(p.cp), p.what,
                        path->c_str());
        } else {
            std::printf("  U+%04X %-24s -> (none)%s\n", static_cast<unsigned>(p.cp), p.what,
                        p.required ? "  << REQUIRED" : "");
            if (p.required) ++required_missing;
        }
    }

    std::printf("resolved %d/%d\n", resolved, total);

    // No fonts at all on this machine: nothing to assert.
    if (resolved == 0) {
        std::printf("SKIP: no system fonts discoverable\n");
        return 77;
    }
    if (required_missing) {
        std::printf("%d REQUIRED CODEPOINT(S) UNRESOLVED\n", required_missing);
        return 1;
    }
    std::printf("ALL REQUIRED CODEPOINTS RESOLVE\n");
    return 0;
}
