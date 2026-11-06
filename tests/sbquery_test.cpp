// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Semantic Block Query (DEC 2034) conformance -- the agent read-out.
//
// This surface hands a shell session's command blocks to a program as JSON, so
// the output has to be *parseable* no matter what the child printed. Two ways
// that can fail, and both were real:
//
//   1. Injection -- a command containing `","evil":"` must not be able to add
//      fields to the document.
//   2. Encoding -- RFC 8259 requires a JSON text to be valid UTF-8. Passing
//      bytes >= 0x80 through verbatim meant one stray 0xFF from a binary file
//      produced a document every strict parser rejects, killing the whole
//      read-out rather than mangling one field.
//
// Byte strings here use octal escapes: "\xff" followed by a hex digit is
// absorbed greedily by the compiler ("\xffz" is ONE out-of-range char), which
// silently changes what you're testing.

#include "toe/term/sbquery.hpp"
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

static void expect_eq(std::string_view got, std::string_view want, const char *what) {
    if (got != want) {
        std::printf("FAIL %s\n       got  |%.*s|\n       want |%.*s|\n", what,
                    static_cast<int>(got.size()), got.data(),
                    static_cast<int>(want.size()), want.data());
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

// Is `s` well-formed UTF-8? A JSON text must be (RFC 8259 §8.1).
static bool valid_utf8(std::string_view s) {
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int need = 0;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) need = 3;
        else return false;
        if (i + static_cast<std::size_t>(need) >= n) return false;
        for (int k = 1; k <= need; ++k)
            if ((static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0xC0) != 0x80)
                return false;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if (need == 2 && ((c == 0xE0 && b1 < 0xA0) || (c == 0xED && b1 >= 0xA0))) return false;
        if (need == 3 && ((c == 0xF0 && b1 < 0x90) || (c == 0xF4 && b1 >= 0x90))) return false;
        i += static_cast<std::size_t>(need) + 1;
    }
    return true;
}

// Every byte in an escaped string must be legal inside a JSON string literal:
// no raw control characters, and no unescaped quote or backslash run.
static bool json_string_safe(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20) return false; // raw control char: illegal in a JSON string
        if (c == '"') return false; // an unescaped quote would close the string
        if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            const char n = s[i + 1];
            if (n != '"' && n != '\\' && n != '/' && n != 'b' && n != 'f' && n != 'n' &&
                n != 'r' && n != 't' && n != 'u')
                return false;
            i += (n == 'u') ? 5 : 1;
        }
    }
    return true;
}

int main() {
    // --- the escapes JSON requires ----------------------------------------
    expect_eq(term::json_escape("plain"), "plain", "plain text is unchanged");
    expect_eq(term::json_escape("a\"b"), "a\\\"b", "double quote is escaped");
    expect_eq(term::json_escape("a\\b"), "a\\\\b", "backslash is escaped");
    expect_eq(term::json_escape("a\nb"), "a\\nb", "newline is escaped");
    expect_eq(term::json_escape("a\rb"), "a\\rb", "carriage return is escaped");
    expect_eq(term::json_escape("a\tb"), "a\\tb", "tab is escaped");
    expect_eq(term::json_escape("a\bb"), "a\\bb", "backspace is escaped");
    expect_eq(term::json_escape("a\fb"), "a\\fb", "form feed is escaped");
    expect_eq(term::json_escape("\001\037"), "\\u0001\\u001f", "C0 controls become \\u00xx");
    expect_eq(term::json_escape("a\033[31mb"), "a\\u001b[31mb", "ESC is escaped, not raw");
    // DEL used to be silently DROPPED, losing a byte from the transcript.
    expect_eq(term::json_escape("a\177b"), "a\\u007fb", "DEL is escaped, not dropped");

    // --- valid UTF-8 passes through unharmed -------------------------------
    expect_eq(term::json_escape("caf\303\251"), "caf\303\251", "2-byte UTF-8 survives");
    expect_eq(term::json_escape("\344\270\255"), "\344\270\255", "3-byte UTF-8 survives");
    expect_eq(term::json_escape("\360\237\230\200"), "\360\237\230\200",
              "4-byte UTF-8 (emoji) survives");

    // --- malformed UTF-8 must become U+FFFD, never pass through -----------
    // This is the bug: a single 0xFF made the whole document unparseable.
    {
        const char *bad[][2] = {
            {"a\377\376 z",            "stray 0xFF/0xFE"},
            {"a\355\240\200 b",        "UTF-16 surrogate"},
            {"a\300\200b",             "overlong C0 80"},
            {"a\340\200\200b",         "overlong E0 80 80"},
            {"a\365\200\200\200b",     "F5 lead (> U+10FFFF)"},
            {"a\364\220\200\200b",     "F4 90 (> U+10FFFF)"},
            {"a\370\210b",             "5-byte form"},
            {"\200\201\202",           "stray continuations"},
            {"a\303",                  "truncated 2-byte"},
            {"a\342\202",              "truncated 3-byte"},
        };
        bool all_utf8 = true, all_safe = true, none_raw = true;
        for (const auto &c : bad) {
            const std::string esc = term::json_escape(c[0]);
            if (!valid_utf8(esc)) { all_utf8 = false; std::printf("     (bad: %s)\n", c[1]); }
            if (!json_string_safe(esc)) { all_safe = false; std::printf("     (unsafe: %s)\n", c[1]); }
            // No byte >= 0x80 may survive except as part of U+FFFD (EF BF BD).
            for (std::size_t i = 0; i < esc.size(); ++i) {
                const auto b = static_cast<unsigned char>(esc[i]);
                if (b >= 0x80 && b != 0xEF && b != 0xBF && b != 0xBD) none_raw = false;
            }
        }
        expect(all_utf8, "every malformed input escapes to valid UTF-8");
        expect(all_safe, "every malformed input escapes to a legal JSON string");
        expect(none_raw, "no raw invalid byte survives escaping");
    }

    // --- injection ---------------------------------------------------------
    // A command containing JSON syntax must stay inside its string.
    {
        const std::string esc = term::json_escape("a\",\"evil\":\"injected");
        expect(esc.find("\",\"") == std::string::npos, "injection: no bare quote-comma-quote");
        expect(json_string_safe(esc), "injection attempt stays a legal JSON string");
    }

    // --- end to end: the real query path ----------------------------------
    // Drive DEC 2034 the way an agent would: enable, run a command, query.
    {
        term::Config cfg;
        term::Model m{cfg, Extent{80, 24}};
        auto replies = [&m](std::string_view s) {
            std::string r;
            for (const Cmd &c : term::feed_output(m, s))
                if (const auto *w = std::get_if<WriteChild>(&c)) r += w->bytes;
            return r;
        };
        const std::string enable = replies("\033[?2034h");
        expect(!enable.empty(), "DEC 2034 enable returns a session token");

        // Pull the 4 token groups out of the enable reply.
        unsigned tok[4] = {0, 0, 0, 0};
        int seen = 0;
        for (const char *p = enable.c_str(); *p;) {
            if (*p >= '0' && *p <= '9') {
                unsigned v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<unsigned>(*p - '0'); ++p; }
                if (seen >= 4) { tok[0] = tok[1]; tok[1] = tok[2]; tok[2] = tok[3]; tok[3] = v; }
                else tok[seen] = v;
                ++seen;
            } else {
                ++p;
            }
        }

        // A command whose text is hostile on both axes at once.
        (void)replies("\033]133;A\033\\$ \033]133;B\033\\");
        (void)replies("echo \"a\",\"evil\":\"x\" \377\200");
        (void)replies("\r\n\033]133;C\033\\out\r\n\033]133;D;0\033\\");

        char q[128];
        std::snprintf(q, sizeof q, "\033[>1;1;%u;%u;%u;%ub", tok[0], tok[1], tok[2], tok[3]);
        const std::string reply = replies(q);
        expect(!reply.empty(), "a tokened query returns a reply");
        expect(valid_utf8(reply), "the query reply is valid UTF-8 end to end");
        const std::size_t open = reply.find('{');
        expect(open != std::string::npos, "the reply carries a JSON body");
        if (open != std::string::npos) {
            // Strip the DCS terminator and check the body is a legal string
            // sequence (no raw controls, balanced escapes).
            std::string body = reply.substr(open);
            const std::size_t st = body.rfind("\033\\");
            if (st != std::string::npos) body = body.substr(0, st);
            expect(valid_utf8(body), "the JSON body is valid UTF-8");
            expect(body.find("\"evil\":") == std::string::npos ||
                       body.find("\\\"evil\\\"") != std::string::npos,
                   "a hostile command cannot inject a JSON field");
        }

        // An untokened or wrong-token query must be refused, not answered.
        const std::string bad = replies("\033[>1;1;1;2;3;4b");
        expect(bad.find("\"blocks\"") == std::string::npos,
               "a query with the wrong token does not return blocks");
    }

    std::printf(failures ? "sbquery: %d FAILURES\n" : "sbquery: PASS\n", failures);
    return failures ? 1 : 0;
}
