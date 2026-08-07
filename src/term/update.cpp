// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The pure terminal reducer. See update.hpp. This translation unit performs no
// I/O; it only mutates the Model and returns Cmds.

#include "gvte/term/update.hpp"

#include <cstdio>

namespace gvte::term {

namespace {

// OSC 10/11 (default fg/bg colour) query reply: OSC N ; rgb:RR/GG/BB ST, with
// each channel doubled to the 16-bit form apps expect.
Cmd colour_reply(int osc, Rgb c) {
    char rep[64];
    std::snprintf(rep, sizeof rep, "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", osc, c.r, c.r,
                  c.g, c.g, c.b, c.b);
    return WriteChild{rep};
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
    }
}

} // namespace

Cmds feed_output(Model &m, std::string_view bytes) {
    Cmds out;
    // New output arrived: snap the view to the live bottom (conventional).
    m.screen.scroll_to_bottom();
    m.parser.feed(std::span<const char>{bytes.data(), bytes.size()}, [&](const vt::Action &a) {
        if (const auto *osc = std::get_if<vt::OscDispatch>(&a)) {
            handle_osc(m, osc->data, out);
        } else {
            m.screen.apply(a, out); // Screen emits its own effects (replies, bell)
        }
    });
    return out;
}

} // namespace gvte::term
