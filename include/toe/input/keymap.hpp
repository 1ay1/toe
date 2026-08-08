// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Keyboard encoding — the hot path of a terminal.
//
// Turns a KeyEvent into the exact bytes a VT terminal sends the child, per the
// xterm convention. Correctness targets the full matrix real programs rely on:
// modified arrows/nav (CSI 1;<mod> X and CSI <n>;<mod> ~), function keys, the
// Alt/Meta ESC prefix, C0 controls for Ctrl-letters and Ctrl-symbols, and the
// DECCKM application-cursor-keys mode (SS3 vs CSI).
//
// Speed: encode_key writes into a caller-owned fixed buffer and returns a
// span. No allocation, no std::string, on any keystroke. The whole encoding is
// a handful of branches and byte stores.

#ifndef TOE_INPUT_KEYMAP_HPP
#define TOE_INPUT_KEYMAP_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "toe/input.hpp"

namespace toe {

// Terminal state that affects encoding (set by the app via escape sequences).
struct KeyContext {
    bool app_cursor_keys = false;  // DECCKM: cursor/nav keys use SS3 not CSI
    std::uint8_t kitty_flags = 0;  // Kitty keyboard progressive-enhancement flags
};

// A stack buffer large enough for any single key encoding. Legacy longest is a
// modified function key (ESC [ 24 ; 14 ~ = 9 bytes); the Kitty CSI-u form with
// an alternate codepoint + modifiers + event type is longer, so size for that.
using KeyBuf = std::array<char, 48>;

// Encode `ev` into `buf`, returning the written bytes. The span is a view into
// `buf`, valid for as long as `buf` lives. Empty span == nothing to send.
[[nodiscard]] std::span<const char> encode_key(const KeyEvent &ev, const KeyContext &ctx,
                                               KeyBuf &buf) noexcept;

} // namespace toe

#endif // TOE_INPUT_KEYMAP_HPP
