// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gvte::Terminal / Session implementation. The engine (PTY -> parser -> grid
// -> GPU) lives in Session::Impl; Terminal is the {Running, Exited} state
// machine whose sole transition is poll().

#include "gvte/terminal.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <span>
#include <utility>

#include "gvte/gfx/font.hpp"
#include "gvte/gfx/renderer.hpp"
#include "gvte/pty/pty.hpp"
#include "gvte/term/screen.hpp"
#include "gvte/term/update.hpp"
#include "gvte/vt/parser.hpp"

namespace gvte {

// ---------------------------------------------------------------------------
// The live engine. Only reachable through a Session, which only exists while
// the child is alive — so every field here is valid by construction.
//
// TEA runtime: the Model (pure state) lives in `model`; every child-output
// drain produces a Cmds list that `interpret()` — the only impure code —
// executes against the PTY / clipboard / title.
struct Session::Impl {
    term::Model model;
    gfx::Renderer renderer;
    Pty pty;
    Extent grid;
    int cell_w;
    int cell_h;
    std::optional<std::string> clipboard_request{}; // last SetClipboard, for the host to pull

    Impl(Config c, Extent g, gfx::Renderer r, Pty p, int cw, int ch)
        : model(std::move(c), g), renderer(std::move(r)), pty(std::move(p)), grid(g), cell_w(cw),
          cell_h(ch) {}

    // The Cmd interpreter — the sole side-effecting code in the core. Every
    // effect the pure update produced is performed here.
    void interpret(const Cmds &cmds) {
        for (const Cmd &c : cmds) {
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, WriteChild>) {
                        (void)pty.write(e.bytes);
                    } else if constexpr (std::is_same_v<T, SetClipboard>) {
                        clipboard_request = e.text; // host pulls this and sets the OS clipboard
                    } else if constexpr (std::is_same_v<T, SetTitle>) {
                        // title lives in the model; nothing extra to do here.
                    } else if constexpr (std::is_same_v<T, ResizePty>) {
                        (void)pty.resize(e.size);
                    } else if constexpr (std::is_same_v<T, RingBell>) {
                        // no audible bell yet; a visual-bell Cmd could hook here.
                    } else if constexpr (std::is_same_v<T, Quit>) {
                        // handled by the poll transition when the child exits.
                    }
                },
                c);
        }
    }

    // Drain child output through the pure reducer, interpreting effects.
    // Returns false when the child has hung up (the transition trigger).
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
            Cmds effects = term::feed_output(model, std::string_view{buf.data(), *n});
            interpret(effects);
        }
    }
};

// ---------------------------------------------------------------------------
// Session — the Running state's methods. Holding one means the child is alive.

Session::Session(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;
Session::~Session() = default;

void Session::render(PixelSize px) { impl_->renderer.draw(impl_->model.screen, px); }

void Session::resize(PixelSize px) {
    const Extent ng = impl_->renderer.cells_for(px);
    if (ng.cols != impl_->grid.cols || ng.rows != impl_->grid.rows) {
        impl_->grid = ng;
        impl_->model.screen.resize(ng);
        (void)impl_->pty.resize(ng);
    }
}

void Session::send_text(std::string_view utf8) { (void)impl_->pty.write(utf8); }

// --- pure input encoding ---------------------------------------------------
namespace {

// Encode a key event to the bytes a terminal sends the child. Pure: no I/O.
std::string encode_key(const KeyEvent &ev) {
    if (const auto *t = std::get_if<TextInput>(&ev.key)) {
        if (ev.mods.ctrl && t->utf8.size() == 1) {
            const char c = t->utf8[0];
            if (c >= 'a' && c <= 'z') return std::string(1, static_cast<char>(c - 'a' + 1));
            if (c >= 'A' && c <= 'Z') return std::string(1, static_cast<char>(c - 'A' + 1));
        }
        return t->utf8;
    }
    switch (std::get<SpecialKey>(ev.key)) {
    case SpecialKey::Enter: return "\r";
    case SpecialKey::Backspace: return "\x7f";
    case SpecialKey::Tab: return "\t";
    case SpecialKey::Escape: return "\x1b";
    case SpecialKey::Up: return "\x1b[A";
    case SpecialKey::Down: return "\x1b[B";
    case SpecialKey::Right: return "\x1b[C";
    case SpecialKey::Left: return "\x1b[D";
    case SpecialKey::Home: return "\x1b[H";
    case SpecialKey::End: return "\x1b[F";
    case SpecialKey::PageUp: return "\x1b[5~";
    case SpecialKey::PageDown: return "\x1b[6~";
    case SpecialKey::Delete: return "\x1b[3~";
    case SpecialKey::Insert: return "\x1b[2~";
    }
    return {};
}

} // namespace

// --- The Elm Architecture: update + interpreter ----------------------------
Cmds Session::update(const Msg &msg) {
    Cmds out;
    std::visit(
        [&](auto &&m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, ChildOutput>) {
                Cmds fx = term::feed_output(impl_->model, m.bytes);
                out.insert(out.end(), std::make_move_iterator(fx.begin()),
                           std::make_move_iterator(fx.end()));
            } else if constexpr (std::is_same_v<T, Key>) {
                std::string bytes = encode_key(m.event);
                if (!bytes.empty()) out.emplace_back(WriteChild{std::move(bytes)});
            } else if constexpr (std::is_same_v<T, Paste>) {
                if (impl_->model.screen.bracketed_paste()) {
                    out.emplace_back(WriteChild{"\x1b[200~" + m.text + "\x1b[201~"});
                } else {
                    out.emplace_back(WriteChild{m.text});
                }
            } else if constexpr (std::is_same_v<T, Resized>) {
                const Extent ng = impl_->renderer.cells_for(m.pixels);
                if (ng.cols != impl_->grid.cols || ng.rows != impl_->grid.rows) {
                    impl_->grid = ng;
                    impl_->model.screen.resize(ng);
                    out.emplace_back(ResizePty{ng});
                }
            } else if constexpr (std::is_same_v<T, ChildExited>) {
                out.emplace_back(Quit{m.code});
            }
            // Mouse/Tick Msgs are routed by the host via the dedicated helpers
            // for now; they can migrate into update() the same way.
        },
        msg);
    return out;
}

void Session::run(const Cmds &cmds) { impl_->interpret(cmds); }

void Session::scroll(int lines) { impl_->model.screen.scroll(lines); }
void Session::scroll_to_bottom() { impl_->model.screen.scroll_to_bottom(); }

void Session::select_begin(int vrow, int col, int mode) {
    using SM = term::Screen::SelectMode;
    const SM m = (mode == 1) ? SM::line : (mode == 2) ? SM::block : SM::character;
    const std::int64_t abs = impl_->model.screen.viewport_to_abs(vrow);
    impl_->model.screen.selection_begin({abs, col}, m);
}

void Session::select_extend(int vrow, int col) {
    const std::int64_t abs = impl_->model.screen.viewport_to_abs(vrow);
    impl_->model.screen.selection_extend({abs, col});
}

void Session::select_word(int vrow, int col) {
    const std::int64_t abs = impl_->model.screen.viewport_to_abs(vrow);
    impl_->model.screen.selection_word({abs, col});
}

void Session::select_line(int vrow, int col) {
    const std::int64_t abs = impl_->model.screen.viewport_to_abs(vrow);
    impl_->model.screen.selection_line({abs, col});
}

void Session::select_clear() { impl_->model.screen.selection_clear(); }
bool Session::has_selection() const noexcept { return impl_->model.screen.has_selection(); }
std::string Session::selected_text() const { return impl_->model.screen.selected_text(); }

// --- mouse reporting -------------------------------------------------------
bool Session::wants_mouse() const noexcept {
    return impl_->model.screen.mouse_mode() != term::Screen::MouseMode::off;
}
bool Session::wants_mouse_motion() const noexcept {
    return impl_->model.screen.mouse_mode() == term::Screen::MouseMode::any;
}
bool Session::wants_mouse_drag() const noexcept {
    const auto m = impl_->model.screen.mouse_mode();
    return m == term::Screen::MouseMode::button || m == term::Screen::MouseMode::any;
}

void Session::report_mouse(MouseEvent kind, int button, int col, int row, bool shift, bool alt,
                           bool ctrl) {
    const auto mode = impl_->model.screen.mouse_mode();
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
    if (impl_->model.screen.mouse_sgr()) {
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

// send_key is now a thin convenience over the TEA pipeline: build a Key Msg,
// let update() produce the WriteChild Cmd, and run() interpret it.
void Session::send_key(const KeyEvent &ev) { run(update(Key{ev})); }

Extent Session::grid_size() const noexcept { return impl_->grid; }
Pos Session::cursor() const noexcept { return impl_->model.screen.cursor(); }
std::string Session::window_title() const { return impl_->model.title; }
int Session::cell_width() const noexcept { return impl_->cell_w; }
int Session::cell_height() const noexcept { return impl_->cell_h; }
bool Session::bracketed_paste() const noexcept { return impl_->model.screen.bracketed_paste(); }
bool Session::on_alt_screen() const noexcept { return impl_->model.screen.on_alt_screen(); }

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
    // Query replies (DA1/DSR/…) no longer need a wired sink: the pure reducer
    // returns them as WriteChild Cmds, which Impl::interpret writes to the PTY.
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
