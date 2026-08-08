// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Input event value types, shared by the platform layer (which produces them)
// and the terminal (which consumes them). Kept free of both Wayland/X11 and
// PTY concerns so it can sit at the bottom of the dependency graph.
//
// A key press is *either* text or a special key — never both, never neither.
// Encoding that as a sum type makes the degenerate states of a
// struct-with-two-fields unrepresentable.

#ifndef TOE_INPUT_HPP
#define TOE_INPUT_HPP

#include <string>
#include <variant>
#include <cstdint>

namespace toe {

enum class SpecialKey {
    // Editing / navigation
    Enter, Backspace, Tab, Escape, Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Delete, Insert,
    // Function keys
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // Keypad (numeric enter is distinct so apps in keypad mode can tell)
    KpEnter,
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
    // The kind of key transition. Only meaningful to hosts/apps that opt into
    // the Kitty keyboard protocol's "report event types" flag; press is the
    // universal default every legacy path assumes.
    enum class Kind : std::uint8_t { press = 1, repeat = 2, release = 3 };

    std::variant<TextInput, SpecialKey> key;
    Modifiers mods{};
    Kind kind{Kind::press};
};

} // namespace toe

#endif // TOE_INPUT_HPP
