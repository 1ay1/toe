// SPDX-License-Identifier: LGPL-2.0-or-later
//
// UTF-8 decoder conformance. The Unicode standard (Table 3-7) defines which
// byte sequences are well-formed; a decoder MUST reject overlong encodings,
// UTF-16 surrogates and anything above U+10FFFF, substituting U+FFFD. These
// aren't cosmetic rules: an overlong "/" or a smuggled NUL can slip past a
// filter that inspects the decoded text.
//
// Cross-checked exhaustively against Python's codec by
// tools/utf8_conformance.py (7482 sequences); this file pins the cases that
// matter so they can't regress.

#include "toe/term/update.hpp"

#include <cstdio>
#include <string>
#include <string_view>

using namespace toe;

static int failures = 0;

static void expect(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

// Feed `bytes` and return row 0 as UTF-8, trailing blanks trimmed.
static std::string row0(std::string_view bytes) {
    term::Config cfg;
    term::Model m{cfg, Extent{40, 2}};
    (void)term::feed_output(m, bytes);
    std::string out;
    for (const term::Cell &c : m.screen.row(Row{0})) {
        if (c.width == 0 && c.cp == U' ') continue;
        const char32_t cp = c.cp;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        for (char32_t mk : m.screen.marks_of(c)) {
            if (mk < 0x800) {
                out += static_cast<char>(0xC0 | (mk >> 6));
                out += static_cast<char>(0x80 | (mk & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (mk >> 12));
                out += static_cast<char>(0x80 | ((mk >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (mk & 0x3F));
            }
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Does the decoded row contain NO U+FFFD?
static bool clean(std::string_view bytes) {
    return row0(bytes).find("\xEF\xBF\xBD") == std::string::npos;
}
// Does it contain a replacement char (i.e. the input was rejected)?
static bool rejected(std::string_view bytes) {
    return row0(bytes).find("\xEF\xBF\xBD") != std::string::npos;
}

int main() {
    // --- valid sequences must decode exactly -------------------------------
    expect(row0("caf\xC3\xA9") == "caf\xC3\xA9", "2-byte: U+00E9 decodes");
    expect(row0("\xE2\x82\xAC") == "\xE2\x82\xAC", "3-byte: U+20AC decodes");
    expect(row0("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80", "4-byte: U+1F600 decodes");
    expect(clean("\xED\x9F\xBF"), "U+D7FF (just below surrogates) is valid");
    expect(clean("\xEE\x80\x80"), "U+E000 (just above surrogates) is valid");
    expect(clean("\xF4\x8F\xBF\xBF"), "U+10FFFF (the maximum) is valid");
    expect(clean("\xC2\x80"), "U+0080 (shortest 2-byte) is valid");
    expect(clean("\xE0\xA0\x80"), "U+0800 (shortest 3-byte) is valid");
    expect(clean("\xF0\x90\x80\x80"), "U+10000 (shortest 4-byte) is valid");

    // --- overlong encodings must be rejected -------------------------------
    // These encode a codepoint in more bytes than needed. Accepting them lets
    // an attacker smuggle a NUL or a '/' past a filter checking decoded text.
    expect(rejected("\xC0\x80"), "overlong C0 80 (NUL) rejected");
    expect(rejected("\xC0\xAF"), "overlong C0 AF ('/') rejected");
    expect(rejected("\xC1\xBF"), "overlong C1 BF rejected");
    expect(rejected("\xE0\x80\x80"), "overlong E0 80 80 rejected");
    expect(rejected("\xE0\x9F\xBF"), "overlong E0 9F BF rejected");
    expect(rejected("\xF0\x80\x80\x80"), "overlong F0 80 80 80 rejected");
    expect(rejected("\xF0\x8F\xBF\xBF"), "overlong F0 8F BF BF rejected");

    // --- UTF-16 surrogates are not valid UTF-8 -----------------------------
    expect(rejected("\xED\xA0\x80"), "surrogate U+D800 rejected");
    expect(rejected("\xED\xBF\xBF"), "surrogate U+DFFF rejected");

    // --- beyond U+10FFFF ---------------------------------------------------
    expect(rejected("\xF4\x90\x80\x80"), "U+110000 rejected");
    expect(rejected("\xF5\x80\x80\x80"), "F5 lead (out of range) rejected");
    expect(rejected("\xF7\xBF\xBF\xBF"), "F7 lead (out of range) rejected");

    // --- bytes that are never valid UTF-8 ----------------------------------
    expect(rejected("\xF8\x88\x80\x80\x80"), "5-byte form rejected");
    expect(rejected("\xFC\x84\x80\x80\x80\x80"), "6-byte form rejected");
    expect(rejected("\xFE"), "0xFE rejected");
    expect(rejected("\xFF"), "0xFF rejected");

    // --- stray continuations -----------------------------------------------
    expect(rejected("\x80"), "lone continuation byte rejected");
    expect(rejected("\xBF"), "lone continuation 0xBF rejected");

    // --- truncation --------------------------------------------------------
    // A sequence cut off at the END of a chunk stays PENDING, because the
    // continuation bytes legitimately arrive in the next feed_output call --
    // flushing U+FFFD here would corrupt every multi-byte character that
    // happens to straddle a read boundary. It is rejected as soon as a byte
    // that cannot continue it shows up.
    expect(clean("\xC2"), "2-byte truncated at end of chunk stays pending");
    expect(clean("\xE2\x82"), "3-byte truncated at end of chunk stays pending");
    expect(clean("\xF0\x9F\x98"), "4-byte truncated at end of chunk stays pending");
    expect(rejected("\xC2z"), "truncated 2-byte rejected once a bad byte follows");
    expect(rejected("\xE2\x82z"), "truncated 3-byte rejected once a bad byte follows");
    expect(rejected("\xF0\x9F\x98z"), "truncated 4-byte rejected once a bad byte follows");

    // A multi-byte character split across feed_output calls must still decode:
    // this is the normal case when a read() lands mid-character.
    {
        term::Config cfg;
        term::Model m{cfg, Extent{10, 1}};
        (void)term::feed_output(m, "\xE2");
        (void)term::feed_output(m, "\x82");
        (void)term::feed_output(m, "\xAC");
        expect(m.screen.text_between_abs(0, 1) == "\xE2\x82\xAC",
               "a 3-byte char split across three feeds decodes");
    }
    {
        term::Config cfg;
        term::Model m{cfg, Extent{10, 1}};
        (void)term::feed_output(m, "\xF0\x9F");
        (void)term::feed_output(m, "\x98\x80");
        expect(m.screen.text_between_abs(0, 1) == "\xF0\x9F\x98\x80",
               "a 4-byte char split across two feeds decodes");
    }

    // A bad sequence must not swallow the following good character: this was a
    // real bug -- "a\xC0\x80b" lost the 'b' entirely.
    expect(row0("a\xC0\x80z").find('z') != std::string::npos,
           "a malformed sequence does not consume the next character");
    expect(row0("a\xED\xA0\x80z").find('z') != std::string::npos,
           "a rejected surrogate does not consume the next character");
    expect(row0("a\xFFz").find('z') != std::string::npos,
           "an invalid lead does not consume the next character");

    // --- combining marks attach rather than vanish -------------------------
    // "e" + U+0301 must stay one cluster; dropping the mark silently mangles
    // any decomposed accented text.
    expect(row0("e\xCC\x81") == "e\xCC\x81", "combining acute attaches to its base");
    expect(row0("e\xCC\x81x") == "e\xCC\x81x", "combining mark does not advance the cursor");
    expect(row0("\xE4\xB8\xAD\xCC\x81") == "\xE4\xB8\xAD\xCC\x81",
           "combining mark attaches to a wide glyph");
    // A mark with no base glyph must not corrupt anything.
    expect(clean("\xCC\x81"), "a leading combining mark is harmless");

    std::printf(failures ? "utf8: %d FAILURES\n" : "utf8: PASS\n", failures);
    return failures ? 1 : 0;
}
