// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The pure terminal reducer. See update.hpp. This translation unit performs no
// I/O; it only mutates the Model and returns Cmds.

#include "toe/term/update.hpp"

#include "toe/gfx/palette.hpp"

#include <cstdio>
#include <optional>

namespace toe::term {

namespace {

// OSC 10/11 (default fg/bg colour) query reply: OSC N ; rgb:RR/GG/BB ST, with
// each channel doubled to the 16-bit form apps expect.
Cmd colour_reply(int osc, Rgb c) {
    char rep[64];
    std::snprintf(rep, sizeof rep, "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", osc, c.r, c.r,
                  c.g, c.g, c.b, c.b);
    return WriteChild{rep};
}

// Parse an X11 colour spec into Rgb. Accepts the two forms terminals use:
//   rgb:RR/GG/BB      (1-4 hex digits per channel, scaled to 8-bit)
//   #RGB / #RRGGBB    (X11 short/long hash form)
// Returns nullopt on anything unrecognized (e.g. a named colour we don't know).
std::optional<Rgb> parse_color_spec(std::string_view s) {
    auto scale = [](std::string_view h) -> std::optional<int> {
        if (h.empty() || h.size() > 4) return std::nullopt;
        int v = 0;
        for (char c : h) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return std::nullopt;
            v = v * 16 + d;
        }
        // Scale an N-nibble value to 8 bits: 0xF->0xFF, 0xFFFF->0xFF, etc.
        const int bits = static_cast<int>(h.size()) * 4;
        return (v * 255) / ((1 << bits) - 1);
    };

    if (s.starts_with("rgb:")) {
        s.remove_prefix(4);
        const auto a = s.find('/');
        if (a == std::string_view::npos) return std::nullopt;
        const auto b = s.find('/', a + 1);
        if (b == std::string_view::npos) return std::nullopt;
        auto r = scale(s.substr(0, a));
        auto g = scale(s.substr(a + 1, b - a - 1));
        auto bl = scale(s.substr(b + 1));
        if (!r || !g || !bl) return std::nullopt;
        return Rgb{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                   static_cast<std::uint8_t>(*bl)};
    }
    if (s.starts_with("#")) {
        s.remove_prefix(1);
        if (s.size() == 3) {
            auto r = scale(s.substr(0, 1)), g = scale(s.substr(1, 1)), b = scale(s.substr(2, 1));
            if (r && g && b)
                return Rgb{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                           static_cast<std::uint8_t>(*b)};
        } else if (s.size() == 6) {
            auto r = scale(s.substr(0, 2)), g = scale(s.substr(2, 2)), b = scale(s.substr(4, 2));
            if (r && g && b)
                return Rgb{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                           static_cast<std::uint8_t>(*b)};
        }
    }
    return std::nullopt;
}

// Base64 decode for OSC 52 clipboard set. Invalid characters are skipped.
std::string decode_base64(std::string_view in) {
    auto val = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char ch : in) {
        if (ch == '=') break;
        const int v = val(ch);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Handle one OSC string, appending any effects. OSC is handled here (not in
// Screen) because colour replies need the Model's configured palette.
void handle_osc(Model &m, std::string_view d, Cmds &out) {
    if (d.size() > 2 && (d.starts_with("0;") || d.starts_with("2;"))) {
        m.title = std::string{d.substr(2)};
        out.emplace_back(SetTitle{m.title});
    } else if (d.starts_with("52;")) {
        const auto semi = d.find(';', 3);
        if (semi != std::string_view::npos) {
            const std::string_view b64 = d.substr(semi + 1);
            if (b64 != "?") {
                out.emplace_back(SetClipboard{decode_base64(b64)});
            }
        }
    } else if (d.starts_with("11;?")) {
        out.emplace_back(colour_reply(11, m.cfg.default_bg));
    } else if (d.starts_with("10;?")) {
        out.emplace_back(colour_reply(10, m.cfg.default_fg));
    } else if (d.starts_with("4;")) {
        // OSC 4 ; index ; spec [ ; index ; spec ]...  set palette colour(s).
        // A spec of '?' queries: reply OSC 4 ; index ; rgb:.. ST.
        std::string_view rest = d.substr(2);
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            if (semi == std::string_view::npos) break;
            const std::string_view idx_s = rest.substr(0, semi);
            rest.remove_prefix(semi + 1);
            const auto next = rest.find(';');
            const std::string_view spec = rest.substr(0, next);
            int idx = 0;
            bool idx_ok = !idx_s.empty();
            for (char c : idx_s) { if (c < '0' || c > '9') { idx_ok = false; break; } idx = idx * 10 + (c - '0'); }
            if (idx_ok && idx >= 0 && idx < 256) {
                if (spec == "?") {
                    // Query: reply with the current value. The live palette lives
                    // in the renderer; the model answers with the xterm default,
                    // which is correct unless the app already overrode this slot.
                    const Rgb c = toe::gfx::Palette{}.by_index(static_cast<std::uint8_t>(idx));
                    char rep[64];
                    std::snprintf(rep, sizeof rep,
                                  "\x1b]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", idx, c.r,
                                  c.r, c.g, c.g, c.b, c.b);
                    out.emplace_back(WriteChild{rep});
                } else if (auto rgb = parse_color_spec(spec)) {
                    m.screen.edit_color({term::Screen::ColorEdit::Target::index,
                                         static_cast<std::uint8_t>(idx), false, *rgb});
                }
            }
            if (next == std::string_view::npos) break;
            rest.remove_prefix(next + 1);
        }
    } else if (d.starts_with("104")) {
        // OSC 104 [ ; index ]...  reset palette colour(s); no params = reset all.
        std::string_view rest = d.substr(3);
        if (rest.empty() || rest == ";") {
            m.screen.reset_all_palette();
        } else {
            if (rest.starts_with(";")) rest.remove_prefix(1);
            while (!rest.empty()) {
                const auto semi = rest.find(';');
                const std::string_view idx_s = rest.substr(0, semi);
                int idx = 0; bool ok = !idx_s.empty();
                for (char c : idx_s) { if (c < '0' || c > '9') { ok = false; break; } idx = idx * 10 + (c - '0'); }
                if (ok && idx >= 0 && idx < 256)
                    m.screen.edit_color({term::Screen::ColorEdit::Target::index,
                                         static_cast<std::uint8_t>(idx), true, {}});
                if (semi == std::string_view::npos) break;
                rest.remove_prefix(semi + 1);
            }
        }
    } else if (d.starts_with("10;")) {
        if (auto rgb = parse_color_spec(d.substr(3)))
            m.screen.edit_color({term::Screen::ColorEdit::Target::fg, 0, false, *rgb});
    } else if (d.starts_with("11;")) {
        if (auto rgb = parse_color_spec(d.substr(3)))
            m.screen.edit_color({term::Screen::ColorEdit::Target::bg, 0, false, *rgb});
    } else if (d.starts_with("12;")) {
        const std::string_view spec = d.substr(3);
        if (spec == "?") {
            out.emplace_back(colour_reply(12, m.cfg.default_fg)); // best-effort
        } else if (auto rgb = parse_color_spec(spec)) {
            m.screen.edit_color({term::Screen::ColorEdit::Target::cursor, 0, false, *rgb});
        }
    } else if (d == "110" || d.starts_with("110;")) {
        m.screen.edit_color({term::Screen::ColorEdit::Target::fg, 0, true, {}});
    } else if (d == "111" || d.starts_with("111;")) {
        m.screen.edit_color({term::Screen::ColorEdit::Target::bg, 0, true, {}});
    } else if (d == "112" || d.starts_with("112;")) {
        m.screen.edit_color({term::Screen::ColorEdit::Target::cursor, 0, true, {}});
    } else if (d.starts_with("7;")) {
        // OSC 7: report the child's working directory (a file:// URI). The host
        // reads m.working_dir to spawn new tabs/splits in the same place.
        m.working_dir = std::string{d.substr(2)};
    } else if (d == "133;A" || d.starts_with("133;A;")) {
        m.shell_zone = Model::ShellZone::prompt;   // FTCS_PROMPT: prompt start
    } else if (d == "133;B" || d.starts_with("133;B;")) {
        m.shell_zone = Model::ShellZone::command;  // FTCS_COMMAND_START
    } else if (d == "133;C" || d.starts_with("133;C;")) {
        m.shell_zone = Model::ShellZone::output;   // FTCS_COMMAND_EXECUTED
    } else if (d == "133;D" || d.starts_with("133;D;")) {
        m.shell_zone = Model::ShellZone::unknown;  // FTCS_COMMAND_FINISHED
    } else if (d.starts_with("9;")) {
        // OSC 9: a desktop notification body (iTerm2/kitty style). Surface it to
        // the host as a titled notification via SetTitle-adjacent channel; here
        // we route the text through a RingBell + the host can read it if wired.
        // Minimal: ring the bell so the user is alerted even without a daemon.
        out.emplace_back(RingBell{});
    } else if (d.starts_with("8;")) {
        // OSC 8 hyperlink: 8 ; params ; URI. `params` may hold id=... An empty
        // URI (or the whole thing being just "8;;") closes the current link.
        const std::string_view rest = d.substr(2);
        const auto semi = rest.find(';');
        if (semi != std::string_view::npos) {
            m.screen.set_hyperlink(rest.substr(0, semi), rest.substr(semi + 1));
        } else {
            m.screen.set_hyperlink({}, {}); // malformed -> close
        }
    }
}

} // namespace

Cmds feed_output(Model &m, std::string_view bytes) {
    Cmds out;
    // New output does NOT snap the view to the bottom: if the user has scrolled
    // up into history, they stay anchored to what they're reading (Screen's
    // scroll_up bumps the offset to keep it fixed). Only typing snaps to the
    // bottom (see the Key handler). At the live bottom (offset 0) output follows
    // the tail naturally, so both cases are already correct without a forced
    // scroll_to_bottom here — which was yanking readers down on every chunk.
    m.parser.feed(std::span<const char>{bytes.data(), bytes.size()}, [&](const vt::Action &a) {
        if (const auto *osc = std::get_if<vt::OscDispatch>(&a)) {
            handle_osc(m, osc->data, out);
        } else {
            m.screen.apply(a, out); // Screen emits its own effects (replies, bell)
        }
    });
    return out;
}

} // namespace toe::term
