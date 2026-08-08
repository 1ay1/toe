// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Standalone assertions for the VT parser. Runs without SDL/GL/PTY — the
// parser is a pure (state, byte) -> action function, so we can drive it with
// literal byte strings and check the emitted Action stream. Exit 0 == pass.

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "toe/vt/parser.hpp"

using namespace toe::vt;

namespace {

// Collect the action stream into a compact string for easy comparison.
std::string trace(std::string_view input) {
    Parser p;
    std::string out;
    p.feed(std::span<const char>{input.data(), input.size()}, [&](const Action &a) {
        std::visit(
            [&](auto &&x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, Print>) {
                    out += "P(";
                    out += std::to_string(static_cast<unsigned>(x.cp));
                    out += ")";
                } else if constexpr (std::is_same_v<T, Execute>) {
                    out += "X(";
                    out += std::to_string(static_cast<unsigned>(x.byte));
                    out += ")";
                } else if constexpr (std::is_same_v<T, CsiDispatch>) {
                    out += "CSI[";
                    if (x.private_marker) { out += x.marker; }
                    for (std::size_t i = 0; i < x.params.size(); ++i) {
                        if (i) { out += ';'; }
                        out += std::to_string(x.params[i]);
                    }
                    out += x.intermediates;
                    out += x.final;
                    out += "]";
                } else if constexpr (std::is_same_v<T, EscDispatch>) {
                    out += "ESC[";
                    out += x.intermediates;
                    out += x.final;
                    out += "]";
                } else if constexpr (std::is_same_v<T, OscDispatch>) {
                    out += "OSC[";
                    out += std::string{x.data};
                    out += "]";
                } else if constexpr (std::is_same_v<T, DcsDispatch>) {
                    out += "DCS[";
                    out += std::string{x.prefix};
                    out += ',';
                    out += std::string{x.data};
                    out += "]";
                }
            },
            a);
    });
    return out;
}

void check(std::string_view input, std::string_view expected, const char *name) {
    std::string got = trace(input);
    if (got != expected) {
        std::fprintf(stderr, "FAIL %s\n  input    = %s\n  expected = %.*s\n  got      = %s\n", name,
                     std::string{input}.c_str(), static_cast<int>(expected.size()), expected.data(),
                     got.c_str());
        std::exit(1);
    }
    std::printf("ok   %s\n", name);
}

} // namespace

int main() {
    // Plain ASCII text.
    check("hi", "P(104)P(105)", "ascii print");

    // C0 control (LF).
    check("\n", "X(10)", "lf execute");

    // SGR: ESC [ 1 ; 31 m
    check("\x1b[1;31m", "CSI[1;31m]", "sgr csi");

    // CUP with defaults: ESC [ ; H  -> params 0;0
    check("\x1b[;H", "CSI[0;0H]", "cup default params");

    // Private mode set: ESC [ ? 25 h  (show cursor)
    check("\x1b[?25h", "CSI[?25h]", "private mode");

    // OSC set title, BEL-terminated: ESC ] 0 ; hey BEL
    check("\x1b]0;hey\x07", "OSC[0;hey]", "osc bel");

    // OSC ST-terminated: ESC ] 2 ; x  then  ESC backslash
    check("\x1b]2;x\x1b\\", "OSC[2;x]", "osc st");

    // Simple ESC dispatch: ESC c (RIS). Concatenate so \x1b doesn't absorb 'c'.
    check("\x1b" "c", "ESC[c]", "esc ris");

    // Charset designation: ESC ( B
    check("\x1b(B", "ESC[(B]", "esc charset");

    // UTF-8 multibyte: 'e-acute' = C3 A9 -> U+00E9 (233)
    check("\xc3\xa9", "P(233)", "utf8 2-byte");

    // UTF-8 3-byte: euro = E2 82 AC -> U+20AC (8364). Bound the last hex
    // escape with string concatenation so it doesn't swallow the next char.
    check("\xe2\x82\xac", "P(8364)", "utf8 3-byte");

    // Mixed stream.
    check("a\x1b[32mb", "P(97)CSI[32m]P(98)", "mixed");

    // DCS: ESC P + q 436f ST (XTGETTCAP for 'Co'), ST = ESC backslash.
    check("\x1bP+q436f\x1b\\", "DCS[+q,436f]", "dcs xtgettcap");

    // DCS with a $ intermediate ($q = DECRQSS); the query 'm' is the payload.
    check("\x1bP$qm\x1b\\", "DCS[$q,m]", "dcs decrqss prefix");

    std::printf("all parser tests passed\n");
    return 0;
}
