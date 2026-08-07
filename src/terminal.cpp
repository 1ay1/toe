// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gvte::Terminal / Session implementation. The engine (PTY -> parser -> grid
// -> GPU) lives in Session::Impl; Terminal is the {Running, Exited} state
// machine whose sole transition is poll().

#include "gvte/terminal.hpp"

#include <array>
#include <cstdlib>
#include <span>
#include <utility>

#include "gvte/gfx/font.hpp"
#include "gvte/gfx/renderer.hpp"
#include "gvte/pty/pty.hpp"
#include "gvte/term/screen.hpp"
#include "gvte/vt/parser.hpp"

namespace gvte {

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
                    // OSC 0/2 ; <title>  sets the window title.
                    std::string_view d = osc->data;
                    if (d.size() > 2 && (d.starts_with("0;") || d.starts_with("2;"))) {
                        title = std::string{d.substr(2)};
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
