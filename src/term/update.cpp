// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The pure terminal reducer. See update.hpp. This translation unit performs no
// I/O; it only mutates the Model and returns Cmds.

#include "toe/term/update.hpp"

#include "toe/gfx/palette.hpp"

#include <chrono>
#include <cstdio>
#include <optional>

namespace toe::term {

namespace {

// Wall-clock milliseconds, for command-block timing (OSC 133 C→D duration).
std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// The cursor's ABSOLUTE row (0 == oldest history row), where a shell-integration
// mark lands. CommandBlocks store absolute rows so they track content as it
// scrolls into history.
std::int64_t cursor_abs_row(const Model &m) {
    return static_cast<std::int64_t>(m.screen.history_rows()) + m.screen.cursor().row.get();
}

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
        // reads m.working_dir to spawn new tabs/splits in the same place; the
        // command log snapshots it per prompt for per-block cwd context.
        m.working_dir = std::string{d.substr(2)};
    } else if (d == "133;A" || d.starts_with("133;A;")) {
        // FTCS_PROMPT: a new prompt begins — open a fresh command block, tagged
        // with the cwd known so far.
        m.commands.mark_prompt(cursor_abs_row(m), m.working_dir);
        m.shell_zone = Model::ShellZone::prompt;
    } else if (d == "133;B" || d.starts_with("133;B;")) {
        // FTCS_COMMAND_START: the typed command begins here (after the prompt).
        m.commands.mark_command(cursor_abs_row(m), m.screen.cursor().col.get());
        m.shell_zone = Model::ShellZone::command;
    } else if (d == "133;C" || d.starts_with("133;C;")) {
        // FTCS_COMMAND_EXECUTED: output starts (Enter pressed).
        m.commands.mark_output(cursor_abs_row(m), now_ms());
        m.shell_zone = Model::ShellZone::output;
    } else if (d == "133;D" || d.starts_with("133;D;")) {
        // FTCS_COMMAND_FINISHED: parse the optional exit code from 133;D;<code>.
        std::optional<int> code{};
        if (d.size() > 6) { // "133;D;" == 6 chars
            std::string_view cs = d.substr(6);
            const auto semi = cs.find(';'); // ignore any trailing ;params
            if (semi != std::string_view::npos) cs = cs.substr(0, semi);
            int v = 0;
            bool ok = !cs.empty();
            bool neg = false;
            std::size_t i = 0;
            if (ok && (cs[0] == '-' )) { neg = true; i = 1; }
            for (; i < cs.size() && ok; ++i) {
                if (cs[i] < '0' || cs[i] > '9') { ok = false; break; }
                v = v * 10 + (cs[i] - '0');
            }
            if (ok) code = neg ? -v : v;
        }
        m.commands.mark_finished(cursor_abs_row(m), code, now_ms());
        m.shell_zone = Model::ShellZone::unknown;
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

// Resolve a block's command / output text against the live Screen. Mirrors the
// split logic in Session::commands() (C often lands on the command's own row).
static std::pair<std::string, std::string> resolve_block_text(const Model &m,
                                                             const CommandBlock &b,
                                                             bool want_output) {
    const Screen &scr = m.screen;
    std::string command, output;
    if (b.input_row >= 0) {
        std::int64_t cmd_end = (b.output_row > b.input_row) ? b.output_row : b.input_row + 1;
        if (b.output_row < 0) cmd_end = scr.total_rows();
        if (cmd_end <= b.input_row) cmd_end = b.input_row + 1;
        command = scr.text_between_abs(b.input_row, cmd_end, b.input_col);
    }
    if (want_output && b.output_row >= 0) {
        std::int64_t out_start = b.output_row;
        if (b.input_row >= 0 && out_start <= b.input_row) out_start = b.input_row + 1;
        const std::int64_t out_end = (b.end_row >= 0) ? b.end_row : scr.total_rows();
        output = scr.text_between_abs(out_start, out_end);
    }
    return {std::move(command), std::move(output)};
}

// DEC 2034 Semantic Block Query CSI handling. Returns true if the sequence was a
// 2034 control/query (and was consumed), so the caller skips Screen::apply.
static bool handle_sbquery_csi(Model &m, const vt::CsiDispatch &c, Cmds &out) {
    auto has_param = [&](int v) {
        for (int p : c.params) if (p == v) return true;
        return false;
    };
    // CSI ? 2034 h / l  (enable / disable), and CSI ? 2034 $ p (DECRQM verify).
    if (c.private_marker && c.marker == '?' && has_param(2034)) {
        if (c.final == 'h') {
            out.emplace_back(WriteChild{m.sbquery.enable()});
            return true;
        }
        if (c.final == 'l') {
            m.sbquery.disable();
            return true;
        }
        if (c.final == 'p' && c.intermediates == "$") {
            out.emplace_back(WriteChild{m.sbquery.decrqm_reply()});
            return true;
        }
    }
    // CSI > Ps ; Pn ; T1;T2;T3;T4 b  — the query itself.
    if (c.marker == '>' && c.final == 'b') {
        std::string params;
        for (std::size_t i = 0; i < c.params.size(); ++i) {
            if (i) params += ';';
            params += std::to_string(c.params[i]);
        }
        BlockTextResolver res{[&m](const CommandBlock &b, bool wo) {
            return resolve_block_text(m, b, wo);
        }};
        out.emplace_back(WriteChild{m.sbquery.query(params, m.commands, res)});
        return true;
    }
    return false;
}


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
        } else if (const auto *csi = std::get_if<vt::CsiDispatch>(&a);
                   csi && handle_sbquery_csi(m, *csi, out)) {
            // DEC 2034 control/query consumed — don't hand it to the Screen.
        } else {
            m.screen.apply(a, out); // Screen emits its own effects (replies, bell)
        }
    });

    // Refresh the command-minimap marks from the OSC-133 command log so the
    // renderer's scroll rail shows a live map of this session's commands
    // (success / failure / running), positioned in scrollback.
    {
        const auto &blocks = m.commands.blocks();
        std::vector<term::Screen::ScrollMark> marks;
        marks.reserve(blocks.size());
        for (const auto &b : blocks) {
            if (b.prompt_row < 0) continue;
            // Only mark blocks that actually ran a command: output started (C)
            // or finished (D). A bare prompt (or a prompt redraw) isn't a
            // command, so it gets no rail segment — matching the flyout list.
            if (b.output_row < 0 && !b.finished()) continue;
            const std::int64_t start = b.prompt_row;
            const std::int64_t end = (b.end_row >= 0) ? b.end_row + 1
                                     : (b.output_row >= 0) ? b.output_row + 1
                                                           : start + 1;
            using MS = term::Screen::MarkStatus;
            const MS st = !b.finished()      ? MS::running
                          : b.succeeded()     ? MS::ok
                                              : MS::failed;
            marks.push_back({start, end, st});
        }
        m.screen.set_scroll_marks(std::move(marks));
    }
    return out;
}

} // namespace toe::term
