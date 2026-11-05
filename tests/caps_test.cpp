// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Capability-reporting conformance: what toe TELLS applications must match
// what it actually does.
//
// This is the bug class that breaks real programs in ways that are hard to
// diagnose: an app queries a mode, toe says "not recognised", so the app
// falls back to a slower or uglier path even though the feature works
// (synchronized output was exactly this). The reverse -- claiming a feature
// that isn't implemented -- makes an app emit sequences that do nothing.
//
// So every mode in the DECRQM switch must be backed by real state, and every
// feature advertised in DA1 must be implemented.

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

// Feed `q` to a fresh model and concatenate everything it writes back.
static std::string ask(std::string_view q) {
    term::Config cfg;
    term::Model m{cfg, Extent{80, 24}};
    std::string r;
    for (const Cmd &c : term::feed_output(m, q))
        if (const auto *w = std::get_if<WriteChild>(&c)) r += w->bytes;
    return r;
}

static void expect_reply(std::string_view query, std::string_view want, const char *what) {
    const std::string got = ask(query);
    if (got != want) {
        std::printf("FAIL %s\n       got  %s\n       want %s\n", what,
                    std::string(got).c_str(), std::string(want).c_str());
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

int main() {
    // --- device attributes --------------------------------------------------
    // DA1 must start with "\e[?" and end with 'c': fish sends it as a fence and
    // blocks for 10s on a malformed answer.
    {
        const std::string da1 = ask("\x1b[c");
        expect(da1.starts_with("\x1b[?") && da1.ends_with("c"), "DA1 is well-formed");
        // Every advertised extension must be real. 4 = SIXEL, 6 = selective
        // erase, 22 = colour.
        expect(da1.find(";4") != std::string::npos, "DA1 advertises SIXEL (we implement it)");
        expect(da1.find(";22") != std::string::npos, "DA1 advertises colour");
    }
    expect(ask("\x1b[>c").starts_with("\x1b[>"), "DA2 answers");
    // DA3 must not be silent -- an app can block waiting for it.
    expect(!ask("\x1b[=c").empty(), "DA3 answers (not silently dropped)");

    // --- device status -----------------------------------------------------
    expect_reply("\x1b[5n", "\x1b[0n", "DSR 5 reports OK");
    expect_reply("\x1b[6n", "\x1b[1;1R", "DSR 6 reports cursor at home");
    expect_reply("\x1b[3;7H\x1b[6n", "\x1b[3;7R", "DSR 6 reports a moved cursor");

    // --- DECRQM: 0 = unrecognised, 1 = set, 2 = reset ----------------------
    // An unknown mode must say "unrecognised", not "reset".
    expect_reply("\x1b[?9999$p", "\x1b[?9999;0$y", "DECRQM: unknown mode -> 0");

    // Each of these is implemented, so NONE may answer 0.
    struct ModeCase { const char *set; const char *query; const char *want; const char *what; };
    static const ModeCase modes[] = {
        {"",            "\x1b[?7$p",    "\x1b[?7;1$y",    "DECRQM 7 (autowrap) defaults set"},
        {"\x1b[?7l",    "\x1b[?7$p",    "\x1b[?7;2$y",    "DECRQM 7 tracks reset"},
        {"",            "\x1b[?25$p",   "\x1b[?25;1$y",   "DECRQM 25 (cursor) defaults set"},
        {"\x1b[?25l",   "\x1b[?25$p",   "\x1b[?25;2$y",   "DECRQM 25 tracks reset"},
        {"\x1b[?1h",    "\x1b[?1$p",    "\x1b[?1;1$y",    "DECRQM 1 (DECCKM) tracks set"},
        {"\x1b[?69h",   "\x1b[?69$p",   "\x1b[?69;1$y",   "DECRQM 69 (DECLRMM) tracks set"},
        {"\x1b[?1000h", "\x1b[?1000$p", "\x1b[?1000;1$y", "DECRQM 1000 (mouse) tracks set"},
        {"\x1b[?1002h", "\x1b[?1002$p", "\x1b[?1002;1$y", "DECRQM 1002 (mouse btn) tracks set"},
        {"\x1b[?1003h", "\x1b[?1003$p", "\x1b[?1003;1$y", "DECRQM 1003 (mouse any) tracks set"},
        {"\x1b[?1004h", "\x1b[?1004$p", "\x1b[?1004;1$y", "DECRQM 1004 (focus) tracks set"},
        {"\x1b[?1006h", "\x1b[?1006$p", "\x1b[?1006;1$y", "DECRQM 1006 (SGR mouse) tracks set"},
        {"\x1b[?1049h", "\x1b[?1049$p", "\x1b[?1049;1$y", "DECRQM 1049 (alt) tracks set"},
        {"\x1b[?2004h", "\x1b[?2004$p", "\x1b[?2004;1$y", "DECRQM 2004 (paste) tracks set"},
        // The bug that motivated this file: 2026 is implemented but was
        // reported as unrecognised, so apps skipped synchronized output.
        {"",            "\x1b[?2026$p", "\x1b[?2026;2$y", "DECRQM 2026 (sync) is recognised"},
        {"\x1b[?2026h", "\x1b[?2026$p", "\x1b[?2026;1$y", "DECRQM 2026 tracks set"},
    };
    for (const ModeCase &mc : modes) {
        std::string q = mc.set;
        q += mc.query;
        expect_reply(q, mc.want, mc.what);
    }

    // --- DECRQSS: report the current setting of a sequence -----------------
    expect(ask("\x1bP$qm\x1b\\").starts_with("\x1bP1$r"), "DECRQSS SGR reports valid");
    expect(ask("\x1bP$qr\x1b\\").find("1;24r") != std::string::npos,
           "DECRQSS DECSTBM reports the full-screen region");
    expect(ask("\x1b[2;10r\x1bP$qr\x1b\\").find("2;10r") != std::string::npos,
           "DECRQSS DECSTBM reports a set region");
    // An unknown request must report INVALID (0$r), not fabricate a value.
    expect(ask("\x1bP$qZZ\x1b\\").starts_with("\x1bP0$r"),
           "DECRQSS reports invalid for an unknown setting");

    // --- XTVERSION / XTGETTCAP ---------------------------------------------
    expect(ask("\x1b[>0q").find("toe") != std::string::npos, "XTVERSION names toe");
    // XTGETTCAP TN (544e) -> the terminal name, hex-encoded, with a 1 = valid.
    expect(ask("\x1bP+q544e\x1b\\").starts_with("\x1bP1+r"), "XTGETTCAP TN answers valid");

    // --- queries must not leave anything on screen -------------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 2}};
        (void)term::feed_output(m, "\x1b[c\x1b[>c\x1b[=c\x1b[5n\x1b[6n\x1b[?2026$p"
                                   "\x1bP$qm\x1b\\\x1b[>0q");
        std::string txt = m.screen.text_between_abs(0, 1);
        while (!txt.empty() && txt.back() == ' ') txt.pop_back();
        expect(txt.empty(), "no query leaks text onto the grid");
        expect(m.screen.cursor().col.get() == 0 && m.screen.cursor().row.get() == 0,
               "no query moves the cursor");
    }

    std::printf(failures ? "caps: %d FAILURES\n" : "caps: PASS\n", failures);
    return failures ? 1 : 0;
}
