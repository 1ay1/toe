// SPDX-License-Identifier: LGPL-2.0-or-later
//
// command_log.hpp — the terminal's structured record of shell commands.
//
// A raw VT stream is write-only: an agent (or a "jump to previous prompt" UI)
// can't ask "what command ran, where is its output, did it succeed?". The
// FinalTerm / iTerm2 OSC 133 shell-integration marks recover that structure:
//
//   OSC 133 ; A ST          prompt starts      (FTCS_PROMPT)
//   OSC 133 ; B ST          command input starts, after the prompt
//   OSC 133 ; C ST          command output starts (the user pressed Enter)
//   OSC 133 ; D ; <code> ST command finished, with its exit code
//
// CommandLog folds those transitions into a ring of CommandBlock records, each
// pinned to ABSOLUTE row coordinates (measured from the oldest history row) so
// a block keeps pointing at the right lines as content scrolls into history and
// eventually drops out. Text (the command line, the output) is NOT copied here:
// blocks store coordinates + metadata, and a reader slices the live Screen on
// demand (see Screen::text_between_abs). That keeps folding O(1) per mark and
// avoids duplicating the scrollback.
//
// This is the substrate for OSC 7 cwd-per-block, the DEC 2034 Semantic Block
// Query (JSON blocks for AI agents), and a host-side block UI.

#ifndef TOE_TERM_COMMAND_LOG_HPP
#define TOE_TERM_COMMAND_LOG_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace toe::term {

// The lifecycle phase the cursor is currently in, per OSC 133. `unknown` is the
// state before any mark (a shell without integration stays here forever, and
// CommandLog simply records nothing — a clean no-op).
enum class ShellZone : std::uint8_t { unknown, prompt, command, output };

// One shell command and everything the terminal knows about it. Rows are
// absolute (0 == oldest history row); -1 means "not marked / not yet known".
struct CommandBlock {
    std::uint64_t id{0};        // monotonic, stable for this block's lifetime

    std::int64_t prompt_row{-1}; // A: first row of the prompt
    std::int64_t input_row{-1};  // B: row where the typed command begins
    std::int64_t output_row{-1}; // C: first row of command output
    std::int64_t end_row{-1};    // D: row after the last output row (exclusive)

    std::int32_t input_col{0};   // B: column where the command text starts

    std::optional<int> exit_code{}; // D: set on completion; nullopt while running
    std::string cwd{};              // OSC 7 working dir captured at prompt time

    std::int64_t start_ms{0};    // wall-clock at C (output start), 0 if unknown
    std::int64_t end_ms{0};      // wall-clock at D (finish),        0 if unknown

    [[nodiscard]] bool finished() const noexcept { return exit_code.has_value(); }
    [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
    [[nodiscard]] std::int64_t duration_ms() const noexcept {
        return (start_ms && end_ms && end_ms >= start_ms) ? end_ms - start_ms : 0;
    }
};

// Folds OSC 133 / OSC 7 marks into a bounded ring of CommandBlocks. Pure state,
// no I/O — call sites feed it marks with the current absolute cursor row.
class CommandLog {
public:
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit CommandLog(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    [[nodiscard]] ShellZone zone() const noexcept { return zone_; }
    [[nodiscard]] bool empty() const noexcept { return blocks_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return blocks_.size(); }

    // Newest-last view of the retained blocks.
    [[nodiscard]] const std::deque<CommandBlock> &blocks() const noexcept { return blocks_; }

    // The block currently executing (has C, no D), or nullptr.
    [[nodiscard]] const CommandBlock *in_progress() const noexcept {
        if (!blocks_.empty() && blocks_.back().output_row >= 0 && !blocks_.back().finished())
            return &blocks_.back();
        return nullptr;
    }
    // The most recently COMPLETED block (has D), or nullptr.
    [[nodiscard]] const CommandBlock *last_completed() const noexcept {
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
            if (it->finished()) return &*it;
        return nullptr;
    }

    // --- OSC 133 marks (abs_row = history_rows() + cursor.row at the mark) ---

    // A: a new prompt begins → start a fresh block. `cwd` is the OSC 7 dir known
    // at this point (may be empty). Bumps generation.
    //
    // Shells (fish especially) RE-EMIT the A mark every time they repaint the
    // prompt — on each keystroke, resize, or completion — which would spawn a
    // flood of empty duplicate blocks. So if the current (newest) block is still
    // an UNUSED prompt (A seen, but no command submitted: no C/output yet),
    // reuse it: just refresh its prompt row + cwd instead of pushing a new one.
    // A block only "commits" to being a real command once output starts (C).
    void mark_prompt(std::int64_t abs_row, std::string cwd) {
        if (!blocks_.empty()) {
            CommandBlock &back = blocks_.back();
            const bool committed = back.output_row >= 0 || back.finished();
            if (!committed) {
                // Reuse the pending prompt block (fish redrew its prompt).
                back.prompt_row = abs_row;
                back.input_row = -1;
                back.input_col = 0;
                if (!cwd.empty()) back.cwd = std::move(cwd);
                zone_ = ShellZone::prompt;
                ++generation_;
                return;
            }
        }
        CommandBlock b;
        b.id = ++next_id_;
        b.prompt_row = abs_row;
        b.cwd = std::move(cwd);
        push(std::move(b));
        zone_ = ShellZone::prompt;
        ++generation_;
    }

    // B: the command line starts here (after the prompt).
    void mark_command(std::int64_t abs_row, std::int32_t abs_col) {
        if (auto *b = open_block()) {
            b->input_row = abs_row;
            b->input_col = abs_col;
        }
        zone_ = ShellZone::command;
        ++generation_;
    }

    // C: output begins; the command has been submitted.
    void mark_output(std::int64_t abs_row, std::int64_t now_ms) {
        if (auto *b = open_block()) {
            b->output_row = abs_row;
            b->start_ms = now_ms;
        }
        zone_ = ShellZone::output;
        ++generation_;
    }

    // D: the command finished with `code` (nullopt if the shell reported none).
    // `abs_row` is where the next prompt will land — the exclusive end of output.
    void mark_finished(std::int64_t abs_row, std::optional<int> code, std::int64_t now_ms) {
        if (auto *b = open_block()) {
            b->end_row = abs_row;
            b->exit_code = code ? code : std::optional<int>{0};
            b->end_ms = now_ms;
        }
        zone_ = ShellZone::unknown;
        ++generation_;
    }

    // Drop the oldest N history rows (called when scrollback trims) so blocks
    // whose rows fell out of the buffer are pruned and survivors shift down.
    void on_history_trimmed(std::int64_t rows_dropped) {
        if (rows_dropped <= 0) return;
        for (auto &b : blocks_) shift(b, -rows_dropped);
        while (!blocks_.empty() && blocks_.front().end_row >= 0 &&
               blocks_.front().end_row <= 0)
            blocks_.pop_front();
    }

    // Bumped on every mark; lets a host/agent cheaply poll "did blocks change?".
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    void clear() {
        blocks_.clear();
        zone_ = ShellZone::unknown;
        ++generation_;
    }

private:
    // The block a B/C/D mark applies to: the newest one still open (no D yet).
    // If none is open (marks arriving without a preceding A, e.g. integration
    // enabled mid-session), synthesize one so we never silently drop a command.
    CommandBlock *open_block() {
        if (blocks_.empty() || blocks_.back().finished()) {
            CommandBlock b;
            b.id = ++next_id_;
            push(std::move(b));
        }
        return &blocks_.back();
    }

    void push(CommandBlock &&b) {
        blocks_.push_back(std::move(b));
        while (blocks_.size() > capacity_) blocks_.pop_front();
    }

    static void shift(CommandBlock &b, std::int64_t d) {
        auto mv = [d](std::int64_t &r) { if (r >= 0) r += d; };
        mv(b.prompt_row);
        mv(b.input_row);
        mv(b.output_row);
        mv(b.end_row);
    }

    std::deque<CommandBlock> blocks_{};
    ShellZone zone_{ShellZone::unknown};
    std::size_t capacity_;
    std::uint64_t next_id_{0};
    std::uint64_t generation_{0};
};

} // namespace toe::term

#endif // TOE_TERM_COMMAND_LOG_HPP
