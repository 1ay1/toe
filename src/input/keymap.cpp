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
    // Write a non-negative integer of any width (kitty codepoints reach 6 digits).
    void num(int v) noexcept {
        if (v < 0) v = 0;
        char tmp[10];
        int i = 0;
        do { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; } while (v > 0);
        while (i > 0) put(tmp[--i]);
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

namespace {

// --- Kitty keyboard protocol encoding --------------------------------------
// When the Disambiguate flag (1) is active, keys are encoded unambiguously.
// Functional/text keys become CSI <number> [; <mods>[:<event>]] <final>, where
// the number is a CSI-u keycode (unicode for text, kitty's functional codes for
// special keys) and the final is 'u' for text/most functional keys or a legacy
// letter (ABCDEFHPQS)/'~' for keys that keep their legacy final.

// The kitty modifier param: 1 + bitmask(shift1 alt2 ctrl4 super8). We only
// carry shift/alt/ctrl from Modifiers; super is not tracked here.
int kitty_mod(Modifiers m) noexcept {
    int bits = 0;
    if (m.shift) bits |= 1;
    if (m.alt) bits |= 2;
    if (m.ctrl) bits |= 4;
    return bits + 1;
}

// Emit CSI <num> [; <mod>[:<event>]] <final>. The modifier section is omitted
// only when there are no modifiers AND no event type to report (kitty's minimal
// form). `event` is 0 (omit), or the kitty event-type (1 press, 2 repeat,
// 3 release) when report-event-types is active and the kind isn't a plain press.
std::span<const char> kitty_emit(int num, int mod, char final, KeyBuf &buf,
                                 int event = 0) noexcept {
    Writer w{buf};
    w.put('\x1b');
    w.put('[');
    w.num(num);
    if (mod != 1 || event > 0) {
        w.put(';');
        w.num(mod);
        if (event > 0) {
            w.put(':');
            w.num(event);
        }
    }
    w.put(final);
    return w.done();
}

// Encode a special key in kitty form. Returns false if this key has no kitty
// mapping (caller falls back to legacy). `event` is the kitty event-type or 0.
bool kitty_special(SpecialKey sk, int mod, int event, KeyBuf &buf,
                   std::span<const char> &out) noexcept {
    switch (sk) {
    case SpecialKey::Up:    out = kitty_emit(1, mod, 'A', buf, event); return true;
    case SpecialKey::Down:  out = kitty_emit(1, mod, 'B', buf, event); return true;
    case SpecialKey::Right: out = kitty_emit(1, mod, 'C', buf, event); return true;
    case SpecialKey::Left:  out = kitty_emit(1, mod, 'D', buf, event); return true;
    case SpecialKey::Home:  out = kitty_emit(1, mod, 'H', buf, event); return true;
    case SpecialKey::End:   out = kitty_emit(1, mod, 'F', buf, event); return true;
    case SpecialKey::F1:    out = kitty_emit(1, mod, 'P', buf, event); return true;
    case SpecialKey::F2:    out = kitty_emit(1, mod, 'Q', buf, event); return true;
    case SpecialKey::F3:    out = kitty_emit(13, mod, '~', buf, event); return true;
    case SpecialKey::F4:    out = kitty_emit(1, mod, 'S', buf, event); return true;
    case SpecialKey::Insert:   out = kitty_emit(2, mod, '~', buf, event); return true;
    case SpecialKey::Delete:   out = kitty_emit(3, mod, '~', buf, event); return true;
    case SpecialKey::PageUp:   out = kitty_emit(5, mod, '~', buf, event); return true;
    case SpecialKey::PageDown: out = kitty_emit(6, mod, '~', buf, event); return true;
    case SpecialKey::F5:  out = kitty_emit(15, mod, '~', buf, event); return true;
    case SpecialKey::F6:  out = kitty_emit(17, mod, '~', buf, event); return true;
    case SpecialKey::F7:  out = kitty_emit(18, mod, '~', buf, event); return true;
    case SpecialKey::F8:  out = kitty_emit(19, mod, '~', buf, event); return true;
    case SpecialKey::F9:  out = kitty_emit(20, mod, '~', buf, event); return true;
    case SpecialKey::F10: out = kitty_emit(21, mod, '~', buf, event); return true;
    case SpecialKey::F11: out = kitty_emit(23, mod, '~', buf, event); return true;
    case SpecialKey::F12: out = kitty_emit(24, mod, '~', buf, event); return true;
    case SpecialKey::Enter:     out = kitty_emit(13, mod, 'u', buf, event); return true;
    case SpecialKey::Tab:       out = kitty_emit(9, mod, 'u', buf, event); return true;
    case SpecialKey::Backspace: out = kitty_emit(127, mod, 'u', buf, event); return true;
    case SpecialKey::Escape:    out = kitty_emit(27, mod, 'u', buf, event); return true;
    case SpecialKey::KpEnter:   out = kitty_emit(13, mod, 'u', buf, event); return true;
    }
    return false;
}

} // namespace

std::span<const char> encode_key(const KeyEvent &ev, const KeyContext &ctx, KeyBuf &buf) noexcept {
    const int mod = modifier_param(ev.mods);
    const bool disambiguate = (ctx.kitty_flags & 0x01) != 0; // flag 1
    const bool report_events = (ctx.kitty_flags & 0x02) != 0; // flag 2
    const bool report_all = (ctx.kitty_flags & 0x08) != 0;    // flag 8
    const bool kitty = disambiguate || report_all;

    // Event type: only emitted when report-events is active. A release event
    // with report-events OFF produces nothing (legacy terminals see press-only).
    const int event = report_events ? static_cast<int>(ev.kind) : 0;
    if (ev.kind == KeyEvent::Kind::release && !report_events) return {};

    if (const auto *t = std::get_if<TextInput>(&ev.key)) {
        if (kitty && t->utf8.size() == 1) {
            const unsigned char c = static_cast<unsigned char>(t->utf8[0]);
            const bool ambiguous = ev.mods.ctrl || ev.mods.alt;
            // Disambiguate: only ctrl/alt-modified ASCII goes CSI-u.
            // Report-all-keys: EVERY single-byte key goes CSI-u.
            if ((report_all || ambiguous) && c < 0x80) {
                // Recover the base codepoint. Ctrl-<letter> arrives as the
                // control byte (Ctrl-A = 0x01); map 0x01..0x1A back to 'a'..'z'.
                int cp = c;
                if (ev.mods.ctrl && c >= 0x01 && c <= 0x1A) cp = c - 1 + 'a';
                else if (c >= 'A' && c <= 'Z') cp = c - 'A' + 'a';
                else if (c == '\r') cp = 13;
                else if (c == '\t') cp = 9;
                else if (c == 0x7f) cp = 127;
                return kitty_emit(cp, kitty_mod(ev.mods), 'u', buf, event);
            }
        }
        // Plain text under report-events still needs the release suppressed
        // (handled above); otherwise emit the bytes as-is.
        return text_key(t->utf8, ev.mods, buf);
    }

    const SpecialKey sk = std::get<SpecialKey>(ev.key);

    if (kitty) {
        std::span<const char> out;
        if (kitty_special(sk, kitty_mod(ev.mods), event, buf, out)) return out;
    }

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
