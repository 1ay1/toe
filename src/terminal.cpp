// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gvte::Terminal / Session implementation. The engine (PTY -> parser -> grid
// -> GPU) lives in Session::Impl; Terminal is the {Running, Exited} state
// machine whose sole transition is poll().

#include "gvte/terminal.hpp"

#include <array>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <span>
#include <utility>

#include "gvte/gfx/font.hpp"
#include "gvte/gfx/renderer.hpp"
#include "gvte/pty/pty.hpp"
#include "gvte/term/screen.hpp"
#include "gvte/vt/parser.hpp"

namespace gvte {

namespace {
// Decode a base64 string (OSC 52 payload). Invalid chars are skipped; returns
// the decoded bytes.
std::string decode_base64(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
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
} // namespace

// ---------------------------------------------------------------------------
// The live engine. Only reachable through a Session, which only exists while
// the child is alive — so every field here is valid by construction.
struct Session::Impl {
    Config cfg;
    Extent grid;
    term::Screen screen;
    vt::Parser parser;
    gfx::Renderer renderer;
    Pty pty;
    std::string title{"gvte"};
    std::optional<std::string> clipboard_request{};
    int cell_w;
    int cell_h;

    Impl(Config c, Extent g, gfx::Renderer r, Pty p, int cw, int ch)
        : cfg(std::move(c)), grid(g), screen(g), renderer(std::move(r)), pty(std::move(p)),
          cell_w(cw), cell_h(ch) {}

    // Drain child output through the parser into the grid. Returns false when
    // the child has hung up (the transition trigger).
    bool drain() {
        std::array<char, 16384> buf{};
        for (;;) {
            auto n = pty.read(std::span<char>{buf});
            if (!n) {
                return false; // EIO / EOF -> child gone
            }
            if (*n == 0) {
                return true; // nothing pending right now
            }
            // New output arrived: snap the view back to the live bottom, the
            // conventional behavior so output isn't missed while scrolled up.
            screen.scroll_to_bottom();
            parser.feed(std::span<const char>{buf.data(), *n}, [&](const vt::Action &a) {
                if (auto *osc = std::get_if<vt::OscDispatch>(&a)) {
                    std::string_view d = osc->data;
                    if (d.size() > 2 && (d.starts_with("0;") || d.starts_with("2;"))) {
                        // OSC 0/2 ; <title>  sets the window title.
                        title = std::string{d.substr(2)};
                    } else if (d.starts_with("52;")) {
                        // OSC 52 ; <selection> ; <base64>  sets the clipboard.
                        const auto semi = d.find(';', 3);
                        if (semi != std::string_view::npos) {
                            std::string_view b64 = d.substr(semi + 1);
                            if (b64 != "?") { // '?' is a read request; we only set
                                clipboard_request = decode_base64(b64);
                            }
                        }
                    }
                } else {
                    screen.apply(a);
                }
            });
        }
    }
};

// ---------------------------------------------------------------------------
// Session — the Running state's methods. Holding one means the child is alive.

Session::Session(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;
Session::~Session() = default;

void Session::render(PixelSize px) { impl_->renderer.draw(impl_->screen, px); }

void Session::resize(PixelSize px) {
    const Extent ng = impl_->renderer.cells_for(px);
    if (ng.cols != impl_->grid.cols || ng.rows != impl_->grid.rows) {
        impl_->grid = ng;
        impl_->screen.resize(ng);
        (void)impl_->pty.resize(ng);
    }
}

void Session::send_text(std::string_view utf8) { (void)impl_->pty.write(utf8); }

void Session::scroll(int lines) { impl_->screen.scroll(lines); }
void Session::scroll_to_bottom() { impl_->screen.scroll_to_bottom(); }

void Session::select_begin(int vrow, int col, int mode) {
    using SM = term::Screen::SelectMode;
    const SM m = (mode == 1) ? SM::line : (mode == 2) ? SM::block : SM::character;
    const std::int64_t abs = impl_->screen.viewport_to_abs(vrow);
    impl_->screen.selection_begin({abs, col}, m);
}

void Session::select_extend(int vrow, int col) {
    const std::int64_t abs = impl_->screen.viewport_to_abs(vrow);
    impl_->screen.selection_extend({abs, col});
}

void Session::select_word(int vrow, int col) {
    const std::int64_t abs = impl_->screen.viewport_to_abs(vrow);
    impl_->screen.selection_word({abs, col});
}

void Session::select_line(int vrow, int col) {
    const std::int64_t abs = impl_->screen.viewport_to_abs(vrow);
    impl_->screen.selection_line({abs, col});
}

void Session::select_clear() { impl_->screen.selection_clear(); }
bool Session::has_selection() const noexcept { return impl_->screen.has_selection(); }
std::string Session::selected_text() const { return impl_->screen.selected_text(); }

// --- mouse reporting -------------------------------------------------------
bool Session::wants_mouse() const noexcept {
    return impl_->screen.mouse_mode() != term::Screen::MouseMode::off;
}
bool Session::wants_mouse_motion() const noexcept {
    return impl_->screen.mouse_mode() == term::Screen::MouseMode::any;
}
bool Session::wants_mouse_drag() const noexcept {
    const auto m = impl_->screen.mouse_mode();
    return m == term::Screen::MouseMode::button || m == term::Screen::MouseMode::any;
}

void Session::report_mouse(MouseEvent kind, int button, int col, int row, bool shift, bool alt,
                           bool ctrl) {
    const auto mode = impl_->screen.mouse_mode();
    if (mode == term::Screen::MouseMode::off) return;

    // Compose the button byte: low bits select the button (or wheel/motion),
    // high bits carry modifiers, per the xterm protocol.
    int cb = button;
    if (kind == MouseEvent::motion) {
        cb += 32; // motion flag (bit 5)
    }
    if (shift) cb += 4;
    if (alt) cb += 8;
    if (ctrl) cb += 16;

    const int cx = col + 1; // protocol coords are 1-based
    const int cy = row + 1;

    std::string seq;
    if (impl_->screen.mouse_sgr()) {
        // SGR extended: ESC [ < cb ; cx ; cy (M press / m release)
        seq = "\x1b[<";
        seq += std::to_string(cb);
        seq += ';';
        seq += std::to_string(cx);
        seq += ';';
        seq += std::to_string(cy);
        seq += (kind == MouseEvent::release) ? 'm' : 'M';
    } else {
        // Legacy X10: ESC [ M  (cb+32) (cx+32) (cy+32) as bytes. Release is
        // reported as button 3 (cb base already set by the caller for release).
        seq = "\x1b[M";
        seq.push_back(static_cast<char>(std::clamp(cb + 32, 32, 255)));
        seq.push_back(static_cast<char>(std::clamp(cx + 32, 32, 255)));
        seq.push_back(static_cast<char>(std::clamp(cy + 32, 32, 255)));
    }
    (void)impl_->pty.write(seq);
}

void Session::send_key(const KeyEvent &ev) {
    // Text branch: forward the UTF-8 as-is (Ctrl-<letter> is folded to a C0
    // control below only for the special-less letter case a host may send).
    if (const auto *t = std::get_if<TextInput>(&ev.key)) {
        if (ev.mods.ctrl && t->utf8.size() == 1) {
            const char c = t->utf8[0];
            if (c >= 'a' && c <= 'z') {
                const char ctl = static_cast<char>(c - 'a' + 1);
                (void)impl_->pty.write(std::string_view{&ctl, 1});
                return;
            }
            if (c >= 'A' && c <= 'Z') {
                const char ctl = static_cast<char>(c - 'A' + 1);
                (void)impl_->pty.write(std::string_view{&ctl, 1});
                return;
            }
        }
        (void)impl_->pty.write(t->utf8);
        return;
    }

    // Special-key branch: map to its escape/control sequence. A switch over the
    // scoped enum is exhaustive — adding a SpecialKey without handling it is a
    // -Wswitch warning, so no key can be silently dropped.
    const SpecialKey sk = std::get<SpecialKey>(ev.key);
    std::string_view bytes;
    switch (sk) {
    case SpecialKey::Enter: bytes = "\r"; break;
    case SpecialKey::Backspace: bytes = "\x7f"; break;
    case SpecialKey::Tab: bytes = "\t"; break;
    case SpecialKey::Escape: bytes = "\x1b"; break;
    case SpecialKey::Up: bytes = "\x1b[A"; break;
    case SpecialKey::Down: bytes = "\x1b[B"; break;
    case SpecialKey::Right: bytes = "\x1b[C"; break;
    case SpecialKey::Left: bytes = "\x1b[D"; break;
    case SpecialKey::Home: bytes = "\x1b[H"; break;
    case SpecialKey::End: bytes = "\x1b[F"; break;
    case SpecialKey::PageUp: bytes = "\x1b[5~"; break;
    case SpecialKey::PageDown: bytes = "\x1b[6~"; break;
    case SpecialKey::Delete: bytes = "\x1b[3~"; break;
    case SpecialKey::Insert: bytes = "\x1b[2~"; break;
    }
    (void)impl_->pty.write(bytes);
}

Extent Session::grid_size() const noexcept { return impl_->grid; }
Pos Session::cursor() const noexcept { return impl_->screen.cursor(); }
std::string Session::window_title() const { return impl_->title; }
int Session::cell_width() const noexcept { return impl_->cell_w; }
int Session::cell_height() const noexcept { return impl_->cell_h; }
bool Session::bracketed_paste() const noexcept { return impl_->screen.bracketed_paste(); }
bool Session::on_alt_screen() const noexcept { return impl_->screen.on_alt_screen(); }

std::optional<std::string> Session::take_clipboard_request() {
    if (!impl_->clipboard_request) return std::nullopt;
    std::optional<std::string> req = std::move(impl_->clipboard_request);
    impl_->clipboard_request.reset();
    return req;
}

// ---------------------------------------------------------------------------
// Terminal — construction and the single transition.

Result<Terminal> Terminal::create(const Config &cfg, PixelSize px) {
    auto atlas = gfx::FontAtlas::create(cfg.font_family, cfg.font_pixel_size);
    if (!atlas) {
        return std::unexpected(atlas.error());
    }
    const int cw = atlas->cell_width();
    const int ch = atlas->cell_height();

    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) {
        return std::unexpected(renderer.error());
    }

    const Extent grid = renderer->cells_for(px);

    // Build argv from the config (or default to $SHELL / /bin/sh).
    std::vector<const char *> argv;
    if (cfg.command.empty()) {
        const char *shell = std::getenv("SHELL");
        argv.push_back(shell ? shell : "/bin/sh");
    } else {
        for (const auto &s : cfg.command) {
            argv.push_back(s.c_str());
        }
    }
    argv.push_back(nullptr);

    auto pty = Pty::spawn(std::span<const char *const>{argv.data(), argv.size() - 1}, grid);
    if (!pty) {
        return std::unexpected(pty.error());
    }

    auto impl = std::make_unique<Session::Impl>(cfg, grid, std::move(*renderer), std::move(*pty),
                                                cw, ch);
    return Terminal{Session{std::move(impl)}};
}

Terminal::Poll Terminal::poll() {
    Poll result;
    if (auto *session = std::get_if<Session>(&state_)) {
        const bool alive = session->impl_->drain();
        if (alive) {
            result.running = session;
        } else {
            const int code = session->impl_->pty.child_exit_code();
            state_ = Exited{code};
            result.exited = &std::get<Exited>(state_);
        }
    } else {
        result.exited = &std::get<Exited>(state_);
    }
    return result;
}

} // namespace gvte
