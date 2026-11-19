// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Standalone assertions for the VT parser. Runs without SDL/GL/PTY — the
// parser is a pure (state, byte) -> action function, so we can drive it with
// literal byte strings and check the emitted Action stream. Exit 0 == pass.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
                } else if constexpr (std::is_same_v<T, PrintRun>) {
                    // A batched ASCII run — expand to the same P(..) form so the
                    // existing expectations (one Print per char) still hold.
                    for (char c : x.text) {
                        out += "P(";
                        out += std::to_string(static_cast<unsigned>(
                            static_cast<unsigned char>(c)));
                        out += ")";
                    }
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

    // The fast paths (whole-buffer run / control / one-pass CSI) must emit the
    // SAME actions as the byte-at-a-time state machine, for every input --
    // a buffer fed one byte per call never takes them.
    {
        const auto bytewise = [](std::string_view in) {
            std::string all;
            Parser p;
            std::string out;
            for (const char c : in) {
                const std::string one(1, c);
                const std::string t = [&] {
                    std::string o;
                    p.feed(std::span<const char>{one.data(), 1}, [&](const Action &a) {
                        std::visit([&](auto &&x) {
                            using T = std::decay_t<decltype(x)>;
                            if constexpr (std::is_same_v<T, PrintRun>) for (char ch : x.text) o += "P(" + std::to_string(static_cast<unsigned char>(ch)) + ")";
                            else if constexpr (std::is_same_v<T, Print>) o += "P(" + std::to_string(static_cast<unsigned>(x.cp)) + ")";
                            else if constexpr (std::is_same_v<T, Execute>) o += "X(" + std::to_string(x.byte) + ")";
                            else if constexpr (std::is_same_v<T, CsiDispatch>) {
                                o += "CSI["; if (x.private_marker) o += x.marker;
                                for (std::size_t i = 0; i < x.params.size(); ++i) { if (i) o += x.sub[i] ? ':' : ';'; o += std::to_string(x.params[i]); }
                                o += x.intermediates; o += x.final; o += "]";
                            } else if constexpr (std::is_same_v<T, EscDispatch>) { o += "ESC["; o += x.intermediates; o += x.final; o += "]"; }
                            else o += "?";
                        }, a);
                    });
                    return o;
                }();
                out += t;
            }
            return out;
        };
        const auto whole = [](std::string_view in) {
            Parser p;
            std::string o;
            p.feed(std::span<const char>{in.data(), in.size()}, [&](const Action &a) {
                std::visit([&](auto &&x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, PrintRun>) for (char ch : x.text) o += "P(" + std::to_string(static_cast<unsigned char>(ch)) + ")";
                    else if constexpr (std::is_same_v<T, Print>) o += "P(" + std::to_string(static_cast<unsigned>(x.cp)) + ")";
                    else if constexpr (std::is_same_v<T, Execute>) o += "X(" + std::to_string(x.byte) + ")";
                    else if constexpr (std::is_same_v<T, CsiDispatch>) {
                        o += "CSI["; if (x.private_marker) o += x.marker;
                        for (std::size_t i = 0; i < x.params.size(); ++i) { if (i) o += x.sub[i] ? ':' : ';'; o += std::to_string(x.params[i]); }
                        o += x.intermediates; o += x.final; o += "]";
                    } else if constexpr (std::is_same_v<T, EscDispatch>) { o += "ESC["; o += x.intermediates; o += x.final; o += "]"; }
                    else o += "?";
                }, a);
            });
            return o;
        };
        const char *cases[] = {
            "\x1b[m", "\x1b[;m", "\x1b[0;;1m", "\x1b[38:2::10:20:30m", "\x1b[38;5;196mred\x1b[0m",
            "\x1b[?1049h\x1b[?25l", "\x1b[>4;2m", "\x1b[99999999999999999999H", "\x1b[1 q", "\x1b[!p",
            "\x1b[1\n2H", "\x1b[12\x1b[3m", "\x1b[", "\x1b", "a\rb\nc\x08\td", "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33;34;35m",
            "\x1b[?u\x1b[=1;1u", "\x1b[" "\x7f" "2J", "\xc3\xa9\x1b[2K\xe2\x82\xac", "\x1b]0;t\x07" "x",
        };
        bool same = true;
        for (const char *cs : cases) {
            const std::string a = bytewise(cs), b = whole(cs);
            if (a != b) { std::printf("FAIL fast path differs on %zu-byte case:\n  slow %s\n  fast %s\n", std::string_view{cs}.size(), a.c_str(), b.c_str()); same = false; }
        }
        // fuzz: random mixes of the bytes that matter
        std::uint64_t s = 0x9E3779B97F4A7C15ull;
        const char alphabet[] = "\x1b[;:0123456789?>mHJKhlq !\r\n\x08\x07" "abc\xc3\xa9]P\\";
        for (int t = 0; t < 40000 && same; ++t) {
            std::string in;
            const int len = 1 + static_cast<int>((s >> 33) % 40);
            for (int i = 0; i < len; ++i) { s = s * 6364136223846793005ull + 1442695040888963407ull; in += alphabet[(s >> 33) % (sizeof alphabet - 1)]; }
            if (bytewise(in) != whole(in)) {
                std::printf("FAIL fast path differs on fuzz input #%d (%zu bytes):", t, in.size());
                for (unsigned char ch : in) std::printf(" %02x", ch);
                std::printf("\n  slow %s\n  fast %s\n", bytewise(in).c_str(), whole(in).c_str());
                same = false;
            }
        }
        if (!same) std::exit(1);
        std::printf("ok   fast paths match the state machine (cases + 40000 fuzz inputs)\n");
    }

    std::printf("all parser tests passed\n");
    return 0;
}
