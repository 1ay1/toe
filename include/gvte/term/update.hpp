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

#ifndef GVTE_TERM_UPDATE_HPP
#define GVTE_TERM_UPDATE_HPP

#include <string>

#include "gvte/core/tea.hpp"
#include "gvte/terminal.hpp" // Config
#include "gvte/term/screen.hpp"
#include "gvte/vt/parser.hpp"

namespace gvte::term {

// The terminal Model: pure state. No I/O handles live here.
struct Model {
    Config cfg;
    Screen screen;
    vt::Parser parser;
    std::string title{"gvte"};

    explicit Model(Config c, Extent grid) : cfg(std::move(c)), screen(grid) {}
};

// Fold a chunk of child output into the model, returning the effects it
// demands (query replies as WriteChild, OSC-0/2 as SetTitle, OSC-52 as
// SetClipboard, colour-query replies, bells). Pure: the model is the only
// thing mutated; everything else is returned data.
[[nodiscard]] Cmds feed_output(Model &m, std::string_view bytes);

} // namespace gvte::term

#endif // GVTE_TERM_UPDATE_HPP
