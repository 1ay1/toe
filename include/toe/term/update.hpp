// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The pure core of the terminal, in Elm-Architecture form.
//
// `Model` is all the terminal's state (the grid Screen + the config-derived
// palette needed to answer colour queries). `update(Model&, Msg) -> Cmds` is
// the one pure transition function: it never touches the PTY, clipboard,
// window or clock — it only mutates the model and *returns* the effects it
// wants performed, as a list of Cmd values. The Runtime (terminal.cpp) is the
// sole interpreter of those Cmds.
//
// Purity here is what made the query-reply and cursor bugs tractable: the
// terminal's response to any byte stream is now `update(model, ChildOutput{…})`
// returning a deterministic `Cmds` we can assert on directly.

#ifndef TOE_TERM_UPDATE_HPP
#define TOE_TERM_UPDATE_HPP

#include <string>
#include <cstdint>

#include "toe/core/tea.hpp"
#include "toe/core/types.hpp"
#include "toe/term/command_log.hpp"
#include "toe/term/palette.hpp"
#include "toe/term/sbquery.hpp"
#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"

namespace toe::term {

// What the reducer needs to know about terminal semantics. Everything here is
// consulted while folding bytes into the model — nothing about fonts, windows
// or input policy, which are the host's business.
struct Config {
    // The default fg/bg, used to answer OSC 10/11/12 colour queries and to
    // seed the palette.
    Rgb default_fg = rgb(220, 220, 220);
    Rgb default_bg = rgb(23, 23, 28);
    // Extra codepoints (UTF-8) that count as part of a word for word-wise
    // selection, beyond the built-in alnum + path/URL set.
    std::string word_separators{};
    // Scrollback lines retained above the visible grid.
    int scrollback_lines = 10000;
    // On a resize at a prompt, crop the prompt's rows instead of rewrapping
    // them -- for shells that repaint by counting rows up from the cursor
    // (zsh, bash). Off for a shell that leaves reflow to the terminal (fish
    // with fish_handle_reflow 0): then the prompt rewraps like any text.
    bool protect_prompt_on_resize = true;
    // Seed for the DEC 2034 session tokens. The reducer never reads the OS
    // for randomness; a host that exposes 2034 to untrusted programs seeds
    // this from the OS once, when it makes the Model.
    std::uint64_t entropy = 0;
};

// Decode UTF-8 to codepoints, skipping malformed bytes rather than failing: a
// bad character in a config string must never break word selection.
[[nodiscard]] std::u32string decode_utf8(std::string_view utf8);

// The terminal Model: pure state. No I/O handles live here.
struct Model {
    Config cfg;
    Screen screen;
    vt::Parser parser;
    std::string title{"toe"};
    // OSC 7: the child's reported working directory, as a file:// URI or path.
    // Empty until the shell reports one. A host uses it to open new tabs there.
    std::string working_dir{};
    // OSC 133 shell integration: the semantic zone the cursor is currently in.
    // A host can use prompt marks to implement jump-to-previous-prompt, and the
    // command zone to time command duration.
    enum class ShellZone : std::uint8_t { unknown, prompt, command, output };
    ShellZone shell_zone{ShellZone::unknown};

    // Structured record of shell commands, folded from OSC 133/7 marks: each
    // block carries its prompt/input/output/end rows (absolute), exit code,
    // cwd and timing. This is the substrate for the DEC 2034 Semantic Block
    // Query, a host block UI, and agent read-out. `commands.zone()` is the
    // authoritative live zone (shell_zone mirrors it for back-compat).
    CommandLog commands{};

    // DEC 2034 Semantic Block Query session state (token + enabled flag). The
    // JSON-over-DCS agent read-out of `commands`.
    SemanticBlockQuery sbquery;

    explicit Model(Config c, Extent grid) : cfg(std::move(c)), screen(grid), sbquery(cfg.entropy) {
        if (cfg.scrollback_lines >= 0)
            screen.set_max_history(static_cast<std::size_t>(cfg.scrollback_lines));
        if (!cfg.word_separators.empty())
            screen.set_word_separators_extra(decode_utf8(cfg.word_separators));
        // Seed the live palette so a colour query before any OSC reports the
        // configured defaults rather than the built-in ones.
        screen.set_default_palette_colors(cfg.default_fg, cfg.default_bg);
    }
};

// Fold a chunk of child output into the model, returning the effects it
// demands (query replies as WriteChild, OSC-0/2 as SetTitle, OSC-52 as
// SetClipboard, colour-query replies, bells). Pure: the model is the only
// thing mutated; everything else is returned data.
//
// `now_ms` is when the chunk arrived, on the host's wall clock (ms since the
// unix epoch): command blocks (OSC 133 C/D) are timed in it. Time is an
// argument, not something the reducer reads, so the same bytes at the same
// time always fold to the same model. 0 (tests) times every command as 0 ms.
[[nodiscard]] Cmds feed_output(Model &m, std::string_view bytes, std::int64_t now_ms = 0);

// Resize the terminal. Everything a resize must keep consistent, in one
// place: the screen rewraps (protecting the prompt being edited when shell
// integration says the cursor is at one), and the command log's absolute
// rows follow the rewrap. Hosts call THIS, not Screen::resize.
void resize(Model &m, Extent size);

} // namespace toe::term

#endif // TOE_TERM_UPDATE_HPP
