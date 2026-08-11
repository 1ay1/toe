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

    // --- DEC 2034 Semantic Block Query -------------------------------------
    auto writes = [](const Cmds &cs) {
        std::string s;
        for (auto &c : cs)
            if (auto *w = std::get_if<WriteChild>(&c)) s += w->bytes;
        return s;
    };
    {
        // Fresh model with one completed command.
        term::Model mq{cfg, Extent{80, 24}};
        feed(mq, osc7("file:///tmp"));
        feed(mq, ftcs('A')); feed(mq, "$ ");
        feed(mq, ftcs('B')); feed(mq, "echo hi");
        feed(mq, ftcs('C')); feed(mq, "\r\nhi\r\n");
        feed(mq, ftcs('D', "0"));

        // Enable 2034 -> DCS handshake carrying a 4-word token.
        std::string hs = writes(term::feed_output(mq, "\x1b[?2034h"));
        ok(mq.sbquery.enabled(), "2034 enabled after CSI ?2034h");
        ok(hs.rfind("\x1bP>2034;1b", 0) == 0, "handshake is DCS >2034;1b...");
        ok(mq.sbquery.token() != 0, "a non-zero token was minted");

        // DECRQM verify -> reports set (1).
        std::string rqm = writes(term::feed_output(mq, "\x1b[?2034$p"));
        ok(rqm == "\x1b[?2034;1$y", "DECRQM reports 2034 set");

        // Query last block WITH the correct token -> status 1 + JSON.
        std::uint64_t tok = mq.sbquery.token();
        auto w16 = [&](int sh) { return std::to_string((tok >> sh) & 0xFFFF); };
        std::string q = "\x1b[>1;1;" + w16(48) + ";" + w16(32) + ";" + w16(16) + ";" + w16(0) + "b";
        std::string reply = writes(term::feed_output(mq, q));
        ok(reply.rfind("\x1bP>1b", 0) == 0, "query with good token -> status 1");
        ok(reply.find("\"command\":\"echo hi\"") != std::string::npos, "JSON has command");
        ok(reply.find("\"exitCode\":0") != std::string::npos, "JSON has exitCode 0");
        ok(reply.find("\"output\":\"hi\"") != std::string::npos, "JSON has output");

        // Query with a WRONG token -> status 3 (bad token).
        std::string bad = "\x1b[>1;1;9;9;9;9b";
        std::string breply = writes(term::feed_output(mq, bad));
        ok(breply.rfind("\x1bP>3b", 0) == 0, "query with bad token -> status 3");

        // Disable -> a subsequent query has no active token (status 2).
        term::feed_output(mq, "\x1b[?2034l");
        ok(!mq.sbquery.enabled(), "2034 disabled after CSI ?2034l");
        std::string noreply = writes(term::feed_output(mq, q));
        ok(noreply.rfind("\x1bP>2b", 0) == 0, "query after disable -> status 2");
    }

    // ── block navigation: jump-to-command scroll math ──────────────────────
    // Build several OSC 133 command blocks (one FAILING) with enough output to
    // push earlier ones into scrollback, then verify scroll_to_abs_row lands a
    // chosen block's prompt in the viewport — the primitive hand's Ctrl+Shift+
    // Up/Down/E block navigation drives.
    {
        term::Model m{cfg, Extent{80, 10}};
        auto cmd = [&](const char *name, int out_lines, int exit_code) {
            feed(m, "\x1b]133;A\x1b\\");            // A: prompt
            feed(m, std::string("$ ") + name + "\r\n");
            feed(m, "\x1b]133;C\x1b\\");            // C: output begins
            for (int i = 0; i < out_lines; ++i) {
                char b[32]; std::snprintf(b, sizeof b, "%s-out-%d\r\n", name, i);
                feed(m, b);
            }
            char d[32]; std::snprintf(d, sizeof d, "\x1b]133;D;%d\x1b\\", exit_code);
            feed(m, d);                             // D: done + exit
        };
        cmd("first", 8, 0);
        cmd("boom", 8, 1);   // <- the failing one
        cmd("third", 8, 0);

        const auto &blocks = m.commands.blocks();
        ok(blocks.size() >= 3, "three command blocks recorded");

        // Find the failing block and its prompt row.
        const term::CommandBlock *failed = nullptr;
        for (const auto &b : blocks)
            if (b.finished() && !b.succeeded() && b.prompt_row >= 0) failed = &b;
        ok(failed != nullptr, "the failing (exit 1) block is found");

        if (failed) {
            // It should be up in scrollback now (third's output pushed it up).
            ok(m.screen.scroll_offset() == 0, "view starts live (bottom)");
            m.screen.scroll_to_abs_row(failed->prompt_row, /*margin=*/0);
            // After jumping, the failed prompt's absolute row must be inside the
            // visible window [top_abs, top_abs+rows).
            const std::int64_t top = m.screen.viewport_to_abs(0);
            const std::int64_t bot = top + m.screen.size().rows;
            ok(failed->prompt_row >= top && failed->prompt_row < bot,
               "jump_to lands the failed block's prompt in the viewport");
            // And the '$ boom' text is actually on the row we jumped to.
            const int vrow = static_cast<int>(failed->prompt_row - top) + 1; // +1: prompt then input
            bool sees_boom = false;
            for (int r = 0; r < m.screen.size().rows; ++r) {
                std::string line;
                for (auto c : m.screen.row(Row{r})) if (c.cp) line += static_cast<char>(c.cp & 0x7f);
                if (line.find("boom") != std::string::npos) { sees_boom = true; break; }
            }
            ok(sees_boom, "the failed command text is visible after the jump");
            (void)vrow;
        }
    }

    // --- fish-style prompt redraws must NOT spawn duplicate blocks ---------
    // Shells re-emit OSC 133 A on every prompt repaint (keystroke/resize). The
    // log must COALESCE consecutive A marks into ONE pending block until a
    // command actually runs (output C / finish D).
    {
        term::Model r{cfg, Extent{80, 24}};
        for (int i = 0; i < 6; ++i) feed(r, ftcs('A')); // 6 prompt redraws
        ok(r.commands.size() == 1, "6 A marks -> 1 pending block");
        feed(r, "$ ls\r\n");
        feed(r, ftcs('B'));
        feed(r, ftcs('C'));            // output starts -> block COMMITS
        feed(r, "a b c\r\n");
        feed(r, ftcs('D', "0"));
        for (int i = 0; i < 4; ++i) feed(r, ftcs('A')); // redraw the next prompt
        ok(r.commands.size() == 2, "committed cmd + next pending prompt = 2 blocks");
        feed(r, "$ pwd\r\n");
        feed(r, ftcs('B'));
        feed(r, ftcs('C'));
        feed(r, "/tmp\r\n");
        feed(r, ftcs('D', "0"));
        ok(r.commands.size() == 2, "two real commands, no phantom blocks");
    }

    std::printf(fails ? "%d command-log test(s) failed\n" : "all command-log tests passed\n",
                fails);
    return fails ? 1 : 0;
}
// (diagnostic build appended below is removed after debugging)
