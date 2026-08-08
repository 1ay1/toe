// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/input/keymap.hpp"

namespace toe {

namespace {

// The xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl. A value of 1 means
// "no modifiers" and is omitted from the sequence.
constexpr int modifier_param(Modifiers m) noexcept {
    return 1 + (m.shift ? 1 : 0) + (m.alt ? 2 : 0) + (m.ctrl ? 4 : 0);
}

// A minimal byte writer over a fixed buffer. No bounds surprises: KeyBuf is
// sized for the worst case and every path writes far less.
struct Writer {
    KeyBuf &buf;
    std::size_t n = 0;
    void put(char c) noexcept { buf[n++] = c; }
    void puts(const char *s) noexcept {
        while (*s) buf[n++] = *s++;
    }
    // Write a small non-negative integer (params are <= 99).
    void num(int v) noexcept {
        if (v >= 10) put(static_cast<char>('0' + v / 10));
        put(static_cast<char>('0' + v % 10));
    }
    std::span<const char> done() const noexcept { return {buf.data(), n}; }
};

// A CSI/SS3 special key with a trailing letter final (arrows, Home/End, F1-F4).
// Emits: ESC [ <letter>  |  ESC [ 1 ; <mod> <letter>  |  (app) ESC O <letter>.
std::span<const char> letter_key(char final, int mod, bool ss3, KeyBuf &buf) noexcept {
    Writer w{buf};
    w.put('\x1b');
    if (mod != 1) {
        // Modified keys always use CSI form with the 1;<mod> prefix.
        w.put('[');
        w.put('1');
        w.put(';');
        w.num(mod);
        w.put(final);
    } else if (ss3) {
        w.put('O');
        w.put(final);
    } else {
        w.put('[');
        w.put(final);
    }
    return w.done();
}

// A CSI-tilde key (Insert/Delete/PageUp/PageDown/F5-F12): ESC [ <n> ~  or
// ESC [ <n> ; <mod> ~.
std::span<const char> tilde_key(int n, int mod, KeyBuf &buf) noexcept {
    Writer w{buf};
    w.put('\x1b');
    w.put('[');
    w.num(n);
    if (mod != 1) {
        w.put(';');
        w.num(mod);
    }
    w.put('~');
    return w.done();
}

// Encode a single-character TextInput, applying Ctrl (-> C0 control) and Alt
// (-> ESC prefix) the way a VT terminal does.
std::span<const char> text_key(const std::string &utf8, Modifiers m, KeyBuf &buf) noexcept {
    Writer w{buf};

    // Alt/Meta prefixes the whole thing with ESC.
    if (m.alt) w.put('\x1b');

    if (m.ctrl && utf8.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(utf8[0]);
        // Ctrl maps a key to its C0 control: mask off bits 6/7.
        //   Ctrl-@ -> NUL(0), Ctrl-A..Z -> 1..26, Ctrl-[ -> 27, Ctrl-\ -> 28,
        //   Ctrl-] -> 29, Ctrl-^ -> 30, Ctrl-_ -> 31, Ctrl-Space -> NUL.
        char ctl;
        if (c == ' ' || c == '@') {
            ctl = 0;
        } else if (c >= 'a' && c <= 'z') {
            ctl = static_cast<char>(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            ctl = static_cast<char>(c - 'A' + 1);
        } else if (c >= '[' && c <= '_') {
            ctl = static_cast<char>(c - '[' + 27); // [ \ ] ^ _ -> 27..31
        } else if (c == '/') {
            ctl = 31; // common Ctrl-/ == Ctrl-_
        } else if (c == '?') {
            ctl = 0x7f; // Ctrl-? == DEL
        } else {
            // No control mapping: send the character unmodified.
            for (char ch : utf8) w.put(ch);
            return w.done();
        }
        w.put(ctl);
        return w.done();
    }

    // Ordinary text (possibly Alt-prefixed above).
    for (char ch : utf8) w.put(ch);
    return w.done();
}

} // namespace

std::span<const char> encode_key(const KeyEvent &ev, const KeyContext &ctx, KeyBuf &buf) noexcept {
    const int mod = modifier_param(ev.mods);

    if (const auto *t = std::get_if<TextInput>(&ev.key)) {
        return text_key(t->utf8, ev.mods, buf);
    }

    const SpecialKey sk = std::get<SpecialKey>(ev.key);

    // Keys that honor DECCKM (application cursor keys) — the "cursor" cluster.
    const bool ss3 = ctx.app_cursor_keys;

    switch (sk) {
    // --- simple controls (modifiers generally ignored / passed as text) ---
    case SpecialKey::Enter:
    case SpecialKey::KpEnter: {
        Writer w{buf};
        if (ev.mods.alt) w.put('\x1b');
        w.put('\r');
        return w.done();
    }
    case SpecialKey::Backspace: {
        Writer w{buf};
        if (ev.mods.alt) w.put('\x1b');
        w.put(ev.mods.ctrl ? '\x08' : '\x7f'); // Ctrl-Backspace -> BS, else DEL
        return w.done();
    }
    case SpecialKey::Tab:
        if (ev.mods.shift) {
            Writer w{buf};
            w.puts("\x1b[Z"); // Shift-Tab -> CBT (back-tab)
            return w.done();
        } else {
            Writer w{buf};
            if (ev.mods.alt) w.put('\x1b');
            w.put('\t');
            return w.done();
        }
    case SpecialKey::Escape: {
        Writer w{buf};
        if (ev.mods.alt) w.put('\x1b');
        w.put('\x1b');
        return w.done();
    }

    // --- cursor cluster: letter-final, SS3 in app mode ---
    case SpecialKey::Up: return letter_key('A', mod, ss3, buf);
    case SpecialKey::Down: return letter_key('B', mod, ss3, buf);
    case SpecialKey::Right: return letter_key('C', mod, ss3, buf);
    case SpecialKey::Left: return letter_key('D', mod, ss3, buf);
    case SpecialKey::Home: return letter_key('H', mod, ss3, buf);
    case SpecialKey::End: return letter_key('F', mod, ss3, buf);

    // --- tilde cluster ---
    case SpecialKey::Insert: return tilde_key(2, mod, buf);
    case SpecialKey::Delete: return tilde_key(3, mod, buf);
    case SpecialKey::PageUp: return tilde_key(5, mod, buf);
    case SpecialKey::PageDown: return tilde_key(6, mod, buf);

    // --- function keys: F1-F4 are SS3-style letters, F5-F12 are tilde ---
    case SpecialKey::F1: return letter_key('P', mod, true, buf); // ESC O P / ESC [1;m P
    case SpecialKey::F2: return letter_key('Q', mod, true, buf);
    case SpecialKey::F3: return letter_key('R', mod, true, buf);
    case SpecialKey::F4: return letter_key('S', mod, true, buf);
    case SpecialKey::F5: return tilde_key(15, mod, buf);
    case SpecialKey::F6: return tilde_key(17, mod, buf);
    case SpecialKey::F7: return tilde_key(18, mod, buf);
    case SpecialKey::F8: return tilde_key(19, mod, buf);
    case SpecialKey::F9: return tilde_key(20, mod, buf);
    case SpecialKey::F10: return tilde_key(21, mod, buf);
    case SpecialKey::F11: return tilde_key(23, mod, buf);
    case SpecialKey::F12: return tilde_key(24, mod, buf);
    }
    return {};
}

} // namespace toe
