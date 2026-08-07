// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Input event value types, shared by the platform layer (which produces them)
// and the terminal (which consumes them). Kept free of both Wayland/X11 and
// PTY concerns so it can sit at the bottom of the dependency graph.
//
// A key press is *either* text or a special key — never both, never neither.
// Encoding that as a sum type makes the degenerate states of a
// struct-with-two-fields unrepresentable.

#ifndef GVTE_INPUT_HPP
#define GVTE_INPUT_HPP

#include <string>
#include <variant>

namespace gvte {

enum class SpecialKey {
    Enter, Backspace, Tab, Escape, Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Delete, Insert,
};

struct TextInput {
    std::string utf8; // one or more UTF-8 codepoints of ordinary input
};

struct Modifiers {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

struct KeyEvent {
    std::variant<TextInput, SpecialKey> key;
    Modifiers mods{};
};

} // namespace gvte

#endif // GVTE_INPUT_HPP
