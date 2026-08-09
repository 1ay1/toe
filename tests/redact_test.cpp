// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Redactor test: secret patterns + high-entropy heuristic scrub, and ordinary
// text is left intact.

#include <cstdio>
#include <string>
#include "toe/term/redact.hpp"

using namespace toe::term;

static int fails = 0;
static void ok(bool c, const char *n) {
    std::printf("%s %s\n", c ? "ok  " : "FAIL", n);
    if (!c) fails++;
}
static bool masked(const std::string &s) { return s.find("«redacted»") != std::string::npos; }
static bool contains(const std::string &s, const char *sub) { return s.find(sub) != std::string::npos; }

int main() {
    Redactor r{true};

    // Well-known key shapes get masked.
    ok(masked(r.apply("export OPENAI_API_KEY=sk-abcdefghijklmnop0123456789")), "sk- api key");
    ok(masked(r.apply("token ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345")), "github pat");
    ok(masked(r.apply("aws AKIAIOSFODNN7EXAMPLE key")), "AWS access key");
    ok(masked(r.apply("Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.abc.def")),
       "JWT / bearer");
    ok(masked(r.apply("DATABASE_PASSWORD=hunter2horse")), "PASSWORD= assignment");
    ok(masked(r.apply("Password: correcthorsebatterystaple")), "password prompt echo");

    // High-entropy unknown token gets masked by the entropy pass.
    ok(masked(r.apply("here is a blob Zx9Qk3Lm7Rp2Vt8Yw1Nc4Bf6Hd0Ke5 done")),
       "high-entropy token");

    // Ordinary text is UNTOUCHED (no false positives on normal output).
    std::string prose = r.apply("total 42\ndrwxr-xr-x  5 ayush staff 160 file.txt\nhello world");
    ok(!masked(prose), "ordinary ls output not masked");
    ok(contains(prose, "hello world"), "prose preserved");
    ok(!masked(r.apply("the quick brown fox jumps over the lazy dog")), "sentence not masked");
    ok(!masked(r.apply("/usr/local/bin/some/long/path/to/a/file")), "path not masked");

    // Disabled redactor is an identity.
    Redactor off{false};
    ok(off.apply("sk-abcdefghijklmnop0123456789") == "sk-abcdefghijklmnop0123456789",
       "disabled = identity");

    // The heuristic directly.
    ok(looks_like_secret("Zx9Qk3Lm7Rp2Vt8Yw1Nc4Bf6Hd0Ke5"), "heuristic flags a key");
    ok(!looks_like_secret("hello"), "heuristic spares short word");
    ok(!looks_like_secret("aaaaaaaaaaaaaaaaaaaaaaaa"), "heuristic spares low-entropy run");

    std::printf(fails ? "%d redact test(s) failed\n" : "all redact tests passed\n", fails);
    return fails ? 1 : 0;
}
