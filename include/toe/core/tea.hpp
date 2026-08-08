// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The Elm Architecture, in C++.
//
// toe's terminal core is a pure state machine. Nothing in the model performs
// I/O directly; instead, every side effect is *reified as data* — a value of
// the `Cmd` sum type below — and returned from the pure `update` function. A
// thin Runtime (the only impure code in the library) interprets those Cmds:
// writing to the PTY, setting the clipboard, retitling the window, ringing the
// bell. Symmetrically, every input to the system — a keypress, a chunk of
// child output, a resize, the child exiting — is a value of the `Msg` sum type.
//
//     update : (Model, Msg) -> (Model, [Cmd])      // pure, total, testable
//     view   : Model -> Scene                       // pure
//     Runtime executes [Cmd] and produces the next [Msg]
//
// This makes the terminal trivially testable (assert on the returned Cmds, no
// mocks), makes effects auditable (they're a closed list), and localizes all
// impurity to one interpreter.

#ifndef TOE_CORE_TEA_HPP
#define TOE_CORE_TEA_HPP

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "toe/core/types.hpp"
#include "toe/input.hpp"

namespace toe {

// ===========================================================================
// Cmd — effects, as data. Produced by `update`, interpreted by the Runtime.
// ===========================================================================

// Write raw bytes to the child process (query replies, key input, pastes).
struct WriteChild {
    std::string bytes;
};

// Set the OS clipboard (from OSC 52 or an explicit copy).
struct SetClipboard {
    std::string text;
};

// Set the window/toplevel title (from OSC 0/2).
struct SetTitle {
    std::string title;
};

// Resize the child's PTY to a new cell grid (after a window resize).
struct ResizePty {
    Extent size;
};

// Ring the terminal bell (BEL / audible or visual).
struct RingBell {};

// The child has requested the terminal quit / the session should end.
struct Quit {
    int code = 0;
};

// The closed set of effects. Adding a new kind of side effect means adding a
// variant here and a case in the Runtime — the compiler enforces completeness.
using Cmd = std::variant<WriteChild, SetClipboard, SetTitle, ResizePty, RingBell, Quit>;

// A batch of effects returned from one update step (often empty).
using Cmds = std::vector<Cmd>;

// ===========================================================================
// Msg — inputs, as data. Everything that can drive a state transition.
// ===========================================================================

// A chunk of bytes read from the child (its output stream).
struct ChildOutput {
    std::string bytes;
};

// The child process exited.
struct ChildExited {
    int code = 0;
};

// A key the host translated from its windowing layer.
struct Key {
    KeyEvent event;
};

// Pasted / IME-committed UTF-8 text.
struct Paste {
    std::string text;
};

// The drawable was resized (host reports the new pixel size; the runtime maps
// it to a cell grid via the font metrics).
struct Resized {
    PixelSize pixels;
};

// --- pointer messages ---
enum class Button { left, middle, right };

struct MouseDown {
    Button button;
    std::int32_t x, y; // pixels
    int click_count;
    Modifiers mods;
};
struct MouseUp {
    Button button;
    std::int32_t x, y;
    Modifiers mods;
};
struct MouseDrag {
    std::int32_t x, y;
};
struct MouseWheel {
    std::int32_t dy; // +1 = up
};

// Wall-clock tick, for cursor blink and other time-driven state.
struct Tick {
    std::uint64_t millis = 0;
};

using Msg = std::variant<ChildOutput, ChildExited, Key, Paste, Resized, MouseDown, MouseUp,
                         MouseDrag, MouseWheel, Tick>;

} // namespace toe

#endif // TOE_CORE_TEA_HPP
