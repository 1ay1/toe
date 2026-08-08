// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Exhaustive keyboard-encoding tests. Every assertion is checked against the
// xterm convention (verified with `showkey -a` / infocmp on a real xterm):
// the modifier param is 1 + shift + 2*alt + 4*ctrl, cursor keys use SS3 in
// application mode, Alt prefixes ESC, Ctrl maps letters/symbols to C0.

#include <cstdio>
#include <string>
#include <string_view>

#include "toe/input/keymap.hpp"

using namespace toe;

namespace {

int failures = 0;

std::string enc(const KeyEvent &ev, bool app_cursor = false) {
    KeyContext ctx{app_cursor};
    KeyBuf buf;
    auto s = encode_key(ev, ctx, buf);
    return std::string(s.data(), s.size());
}

// Human-readable form: ESC shown as "\e".
std::string show(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == 27) o += "\\e";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (static_cast<unsigned char>(c) < 32)
            o += "^" + std::string(1, static_cast<char>('@' + c));
        else o += c;
    }
    return o;
}

KeyEvent special(SpecialKey k, bool shift = false, bool alt = false, bool ctrl = false) {
    KeyEvent e;
    e.key = k;
    e.mods = {ctrl, alt, shift};
    return e;
}
KeyEvent text(std::string s, bool shift = false, bool alt = false, bool ctrl = false) {
    KeyEvent e;
    e.key = TextInput{std::move(s)};
    e.mods = {ctrl, alt, shift};
    return e;
}

void check(const std::string &got, std::string_view want, const char *name) {
    if (got == want) {
        std::printf("ok   %s = %s\n", name, show(got).c_str());
    } else {
        std::printf("FAIL %s: got [%s] want [%s]\n", name, show(got).c_str(),
                    show(std::string{want}).c_str());
        ++failures;
    }
}

std::string enc_kitty(const KeyEvent &ev, std::uint8_t flags) {
    KeyContext ctx{false, flags};
    KeyBuf buf;
    auto s = encode_key(ev, ctx, buf);
    return std::string(s.data(), s.size());
}

} // namespace

int main() {
    // --- plain text ---
    check(enc(text("a")), "a", "plain 'a'");
    check(enc(text("A", true)), "A", "'A' (shift already applied to text)");

    // --- Ctrl + letters -> C0 ---
    check(enc(text("a", false, false, true)), std::string(1, '\x01'), "Ctrl-a = ^A");
    check(enc(text("c", false, false, true)), std::string(1, '\x03'), "Ctrl-c = ^C");
    check(enc(text(" ", false, false, true)), std::string(1, '\0'), "Ctrl-Space = NUL");
    check(enc(text("[", false, false, true)), std::string(1, '\x1b'), "Ctrl-[ = ESC");
    check(enc(text("\\", false, false, true)), std::string(1, '\x1c'), "Ctrl-\\ = FS");
    check(enc(text("]", false, false, true)), std::string(1, '\x1d'), "Ctrl-] = GS");
    check(enc(text("?", false, false, true)), std::string(1, '\x7f'), "Ctrl-? = DEL");

    // --- Alt/Meta prefixes ESC ---
    check(enc(text("x", false, true, false)), "\x1bx", "Alt-x = ESC x");
    check(enc(text("a", false, true, true)), std::string("\x1b") + '\x01', "Ctrl-Alt-a = ESC ^A");

    // --- arrows: plain, and every modifier ---
    check(enc(special(SpecialKey::Up)), "\x1b[A", "Up");
    check(enc(special(SpecialKey::Up, /*shift*/ true)), "\x1b[1;2A", "Shift-Up = 1;2A");
    check(enc(special(SpecialKey::Up, false, /*alt*/ true)), "\x1b[1;3A", "Alt-Up = 1;3A");
    check(enc(special(SpecialKey::Up, false, false, /*ctrl*/ true)), "\x1b[1;5A", "Ctrl-Up = 1;5A");
    check(enc(special(SpecialKey::Left, false, false, true)), "\x1b[1;5D", "Ctrl-Left = 1;5D");
    check(enc(special(SpecialKey::Right, true, false, true)), "\x1b[1;6C",
          "Ctrl-Shift-Right = 1;6C");
    check(enc(special(SpecialKey::Up, true, true, true)), "\x1b[1;8A",
          "Ctrl-Alt-Shift-Up = 1;8A");

    // --- application cursor keys (DECCKM): SS3 when unmodified, CSI when mod ---
    check(enc(special(SpecialKey::Up), /*app*/ true), "\x1bOA", "Up (app mode) = ESC O A");
    check(enc(special(SpecialKey::Up, false, false, true), true), "\x1b[1;5A",
          "Ctrl-Up (app mode) still CSI");

    // --- Home / End ---
    check(enc(special(SpecialKey::Home)), "\x1b[H", "Home");
    check(enc(special(SpecialKey::End, false, false, true)), "\x1b[1;5F", "Ctrl-End = 1;5F");

    // --- tilde cluster ---
    check(enc(special(SpecialKey::Delete)), "\x1b[3~", "Delete");
    check(enc(special(SpecialKey::Delete, true)), "\x1b[3;2~", "Shift-Delete = 3;2~");
    check(enc(special(SpecialKey::PageUp)), "\x1b[5~", "PageUp");
    check(enc(special(SpecialKey::Insert)), "\x1b[2~", "Insert");

    // --- function keys ---
    check(enc(special(SpecialKey::F1)), "\x1bOP", "F1 = ESC O P");
    check(enc(special(SpecialKey::F4)), "\x1bOS", "F4 = ESC O S");
    check(enc(special(SpecialKey::F1, false, false, true)), "\x1b[1;5P", "Ctrl-F1 = 1;5P");
    check(enc(special(SpecialKey::F5)), "\x1b[15~", "F5 = 15~");
    check(enc(special(SpecialKey::F12)), "\x1b[24~", "F12 = 24~");
    check(enc(special(SpecialKey::F5, true)), "\x1b[15;2~", "Shift-F5 = 15;2~");

    // --- editing keys ---
    check(enc(special(SpecialKey::Enter)), "\r", "Enter = CR");
    check(enc(special(SpecialKey::Backspace)), "\x7f", "Backspace = DEL");
    check(enc(special(SpecialKey::Backspace, false, false, true)), "\x08", "Ctrl-Backspace = BS");
    check(enc(special(SpecialKey::Tab)), "\t", "Tab = HT");
    check(enc(special(SpecialKey::Tab, true)), "\x1b[Z", "Shift-Tab = CBT");
    check(enc(special(SpecialKey::Escape)), "\x1b", "Escape");
    check(enc(special(SpecialKey::Enter, false, true)), "\x1b\r", "Alt-Enter = ESC CR");

    // --- Kitty keyboard protocol (Disambiguate flag = 1) -------------------
    constexpr std::uint8_t kDisamb = 1;
    // Legacy vs kitty: Escape is bare ESC legacy, but CSI 27 u under kitty.
    check(enc_kitty(special(SpecialKey::Escape), kDisamb), "\x1b[27u", "kitty Escape = CSI 27 u");
    check(enc_kitty(special(SpecialKey::Tab), kDisamb), "\x1b[9u", "kitty Tab = CSI 9 u");
    check(enc_kitty(special(SpecialKey::Enter), kDisamb), "\x1b[13u", "kitty Enter = CSI 13 u");
    check(enc_kitty(special(SpecialKey::Backspace), kDisamb), "\x1b[127u",
          "kitty Backspace = CSI 127 u");
    // Modified arrows carry the modifier param: Ctrl+Up = CSI 1;5 A.
    check(enc_kitty(special(SpecialKey::Up, false, false, true), kDisamb), "\x1b[1;5A",
          "kitty Ctrl+Up = CSI 1;5 A");
    // Ctrl+letter disambiguates to CSI <cp> ; <mods> u (Ctrl-A = 97;5u).
    check(enc_kitty(text("\x01", false, false, true), kDisamb), "\x1b[97;5u",
          "kitty Ctrl-A = CSI 97;5 u");
    // With no flags active, encoding stays legacy.
    check(enc(special(SpecialKey::Escape)), "\x1b", "legacy Escape stays bare ESC");

    // Report-event-types (flag 2): a release event carries :3; a press with only
    // flag 2 still needs the modifier slot so the event subparam has an anchor.
    {
        KeyEvent e = special(SpecialKey::Up);
        e.kind = KeyEvent::Kind::release;
        KeyContext ctx{false, 0x03}; // disambiguate + report-events
        KeyBuf b; auto s = encode_key(e, ctx, b);
        check(std::string(s.data(), s.size()), "\x1b[1;1:3A", "kitty release event = CSI 1;1:3 A");
    }
    // A release event WITHOUT report-events produces nothing (legacy press-only).
    {
        KeyEvent e = special(SpecialKey::Up);
        e.kind = KeyEvent::Kind::release;
        check(enc_kitty(e, 0x01), "", "kitty release suppressed without report-events");
    }
    // Report-all-keys (flag 8): even a plain letter routes through CSI-u.
    check(enc_kitty(text("a"), 0x08), "\x1b[97u", "kitty report-all: 'a' = CSI 97 u");

    if (failures == 0) {
        std::printf("all keymap tests passed\n");
        return 0;
    }
    std::printf("%d keymap test(s) failed\n", failures);
    return 1;
}
