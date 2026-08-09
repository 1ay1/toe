// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Command-log test: drive OSC 133 shell-integration marks (+ OSC 7) through the
// pure reducer and assert the resulting CommandBlock ring — command text, output
// text, exit code, cwd and timing coordinates. No PTY, no rendering.

#include <cstdio>
#include <string>
#include "toe/term/update.hpp"

using namespace toe;

static int fails = 0;
static void ok(bool c, const char *n) {
    std::printf("%s %s\n", c ? "ok  " : "FAIL", n);
    if (!c) fails++;
}

// Feed a string of child output into the model (discarding Cmds).
static void feed(term::Model &m, const std::string &s) { (void)term::feed_output(m, s); }

// OSC 133 marks as raw escape strings.
static std::string ftcs(char which, const std::string &arg = "") {
    std::string s = "\x1b]133;";
    s.push_back(which);
    if (!arg.empty()) { s += ";"; s += arg; }
    s += "\x1b\\"; // ST
    return s;
}
static std::string osc7(const std::string &dir) { return "\x1b]7;" + dir + "\x1b\\"; }

int main() {
    Config cfg;
    term::Model m{cfg, Extent{80, 24}};

    // --- a full command lifecycle ------------------------------------------
    feed(m, osc7("file:///home/ayush/projects"));
    feed(m, ftcs('A'));                 // prompt starts
    feed(m, "user@host:~$ ");           // the (fake) prompt text
    feed(m, ftcs('B'));                 // command input starts
    feed(m, "echo hi");                 // the typed command
    feed(m, ftcs('C'));                 // Enter -> output starts
    feed(m, "\r\nhi\r\n");              // command output
    feed(m, ftcs('D', "0"));            // finished, exit 0

    ok(m.commands.size() == 1, "one block recorded");
    ok(m.commands.zone() == term::ShellZone::unknown, "zone back to unknown after D");

    auto cmds = m.commands.blocks();
    const auto &b = cmds.front();
    ok(b.exit_code == 0, "exit code 0 captured");
    ok(b.finished(), "block finished");
    ok(b.succeeded(), "block succeeded");
    ok(b.cwd == "file:///home/ayush/projects", "cwd captured from OSC 7");
    ok(b.prompt_row >= 0 && b.input_row >= 0 && b.output_row >= 0 && b.end_row >= 0,
       "all four marks got absolute rows");
    ok(b.input_row <= b.output_row && b.output_row <= b.end_row, "rows are ordered");

    // --- resolved text (mirrors Session::commands() resolve logic) ---------
    // C fired on the command's own row, so command = that row from input_col,
    // and output starts on the next row.
    std::int64_t cmd_end = (b.output_row > b.input_row) ? b.output_row : b.input_row + 1;
    std::string cmd_text = m.screen.text_between_abs(b.input_row, cmd_end, b.input_col);
    ok(cmd_text == "echo hi", "command text resolves to 'echo hi'");
    std::int64_t out_start = (b.output_row > b.input_row) ? b.output_row : b.input_row + 1;
    std::string out_text = m.screen.text_between_abs(out_start, b.end_row);
    ok(out_text == "hi", "output text resolves to 'hi'");

    // --- a failing command --------------------------------------------------
    feed(m, ftcs('A'));
    feed(m, "$ ");
    feed(m, ftcs('B'));
    feed(m, "false");
    feed(m, ftcs('C'));
    feed(m, "\r\n");
    feed(m, ftcs('D', "1"));

    ok(m.commands.size() == 2, "second block recorded");
    ok(m.commands.last_completed() != nullptr, "last_completed present");
    ok(m.commands.last_completed()->exit_code == 1, "failing command exit 1");
    ok(!m.commands.last_completed()->succeeded(), "failing command not succeeded");

    // --- in-progress detection ---------------------------------------------
    feed(m, ftcs('A'));
    feed(m, "$ ");
    feed(m, ftcs('B'));
    feed(m, "sleep 10");
    feed(m, ftcs('C')); // output started, no D yet
    ok(m.commands.in_progress() != nullptr, "in-progress command detected");
    ok(m.commands.zone() == term::ShellZone::output, "zone is output while running");

    // --- a shell WITHOUT integration records nothing -----------------------
    term::Model m2{cfg, Extent{80, 24}};
    feed(m2, "just some output\r\nno marks here\r\n");
    ok(m2.commands.empty(), "no blocks without OSC 133 (clean no-op)");

    // --- generation bumps on marks -----------------------------------------
    auto g0 = m2.commands.generation();
    feed(m2, ftcs('A'));
    ok(m2.commands.generation() > g0, "generation bumps on a mark");

    std::printf(fails ? "%d command-log test(s) failed\n" : "all command-log tests passed\n",
                fails);
    return fails ? 1 : 0;
}
// (diagnostic build appended below is removed after debugging)
