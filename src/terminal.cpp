// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::Terminal / Session implementation. The engine (PTY -> parser -> grid
// -> GPU) lives in Session::Impl; Terminal is the {Running, Exited} state
// machine whose sole transition is poll().

#include "toe/terminal.hpp"
#include "toe/input/keymap.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <span>
#include <utility>

#include "toe/gfx/font.hpp"
#include "toe/gfx/renderer.hpp"
#include "toe/pty/pty.hpp"
#include "toe/term/screen.hpp"
#include "toe/term/update.hpp"
#include "toe/vt/parser.hpp"

namespace toe {

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

    // Retained font parameters so the host can rescale the font at runtime
    // (Cmd +/- zoom) by rebuilding the atlas + renderer at a new pixel size.
    std::string font_path_;
    std::string font_fallback_;
    gfx::FontAtlas::StyleFiles style_files_; // real bold/italic/bold-italic paths
    bool ligatures_ = false;
    int font_px_ = 0;
    Config::CursorAnim cursor_anim_{}; // retained so font rebuilds keep the setting
    Rgb selection_bg_{rgb(66, 84, 112)}; // retained selection colour
    int cursor_blink_ms_ = 530;          // cursor blink half-period (0 = steady)
    Behavior behavior_{};                // scroll/selection host policy
    std::function<void()> on_bell_{};    // host bell handler (audible/visual)
    std::uint64_t focused_block = 0; // command block the block-nav UI is on (0=none)

    Impl(Config c, Extent g, gfx::Renderer r, Pty p, int cw, int ch)
        : model(std::move(c), g), renderer(std::move(r)), pty(std::move(p)), grid(g), cell_w(cw),
          cell_h(ch) {
        model.screen.set_cell_size(cw, ch); // for kitty-graphics placement sizing
        pty.set_cell_pixels(cw, ch);        // ws_xpixel/ypixel for TIOCGWINSZ
        (void)pty.resize(g);                // push the pixel dims to the child now
    }

    // The Cmd interpreter — the sole side-effecting code in the core. Runs of
    // WriteChild bytes are coalesced into ONE pty.write() so a burst of input
    // (fast typing, a multi-key sequence, a paste) costs a single syscall.
    std::string write_batch_; // reused; never shrinks -> no per-call alloc
    void interpret(const Cmds &cmds) {
        write_batch_.clear();
        auto flush = [&] {
            if (!write_batch_.empty()) {
                (void)pty.write(write_batch_);
                write_batch_.clear();
            }
        };
        for (const Cmd &c : cmds) {
            if (const auto *w = std::get_if<WriteChild>(&c)) {
                write_batch_ += w->bytes; // accumulate; flush lazily
                continue;
            }
            // Any non-write effect must observe writes already issued in order.
            flush();
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, SetClipboard>) {
                        clipboard_request = e.text;
                    } else if constexpr (std::is_same_v<T, SetTitle>) {
                        // title lives in the model; nothing extra to do here.
                    } else if constexpr (std::is_same_v<T, ResizePty>) {
                        (void)pty.resize(e.size);
                    } else if constexpr (std::is_same_v<T, RingBell>) {
                        // Bell is host policy (audible beep and/or visual flash);
                        // the host wires on_bell_ from the behavior config.
                        if (on_bell_) on_bell_();
                    } else if constexpr (std::is_same_v<T, Quit>) {
                        // handled by the poll transition when the child exits.
                    }
                },
                c);
        }
        flush();
    }

    // Drain child output through the pure reducer, interpreting effects.
    // Returns false when the child has hung up (the transition trigger).
    //
    // Bounded per call: under a flood (`yes`, `cat huge`) an unbounded drain
    // would feed megabytes to the parser before returning, freezing input and
    // redraw for the duration. We cap each call to a byte budget and set
    // `more_pending` when the PTY still had data, so the host can render an
    // intermediate frame and process input, then come straight back (the fd
    // stays readable, so the loop won't sleep). This keeps the UI responsive
    // and shows progressive output no matter how fast the app writes.
    bool more_pending = false;
    bool hung_up_ = false; // set once the child closes the pty (Hungup)
    bool drain() {
        // Drain aggressively: a big budget so a flood is consumed in a few
        // gulps (throughput), while still returning periodically so the host
        // can check input and present a frame (latency). The host paces the
        // actual *rendering* (its ~144 Hz flood cap), so a large drain here
        // doesn't cause wasted frames — it just stops us round-tripping the
        // read loop 40x for one `cat`. 8 MiB keeps a multi-MiB flood to ~1-2
        // yields while a burst of typing (few KiB) still returns immediately.
        constexpr std::size_t kBudget = 8u * 1024 * 1024;
        std::size_t consumed = 0;
        more_pending = false;
        for (;;) {
            // read() hands back a closed sum: Data (zero-copy view into the
            // Pty's own buffer), WouldBlock (dry now), or Hungup (child gone).
            // std::visit forces us to handle the hangup — no !Result convention.
            bool cont = std::visit(
                [&](auto &&r) -> bool {
                    using R = std::decay_t<decltype(r)>;
                    if constexpr (std::is_same_v<R, toe::pty::Data>) {
                        Cmds effects = term::feed_output(
                            model, std::string_view{r.bytes.data(), r.bytes.size()});
                        interpret(effects);
                        // scroll_on_output: follow the tail when new output lands
                        // (opt-in; the default keeps your scrollback position).
                        if (behavior_.scroll_on_output) model.screen.scroll_to_bottom();
                        consumed += r.bytes.size();
                        if (consumed >= kBudget) {
                            more_pending = true; // yield: let the host render + poll input
                            return false;
                        }
                        return true; // keep draining
                    } else if constexpr (std::is_same_v<R, toe::pty::WouldBlock>) {
                        return false; // nothing pending right now
                    } else { // toe::pty::Hungup
                        hung_up_ = true;
                        return false; // child gone
                    }
                },
                pty.read());
            if (!cont) break;
        }
        return !hung_up_;
    }
};

// ---------------------------------------------------------------------------
// Session — the Running state's methods. Holding one means the child is alive.

Session::Session(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;
Session::~Session() = default;

DamageRect Session::render(gfx::RenderContext &rc, PixelSize px, bool cursor_on, bool blink_on) {
    // The host has already begun the swapchain pass (with the clear); we draw
    // into it. `rc` is just the capability token proving a GPU frame is active.
    (void)rc;
    return impl_->renderer.draw(impl_->model.screen, px, cursor_on, blink_on);
}

void Session::render_overlay(gfx::RenderContext &rc, const term::Cell *cells, int cols, int rows,
                             PixelSize px, int ox, int oy) {
    (void)rc;
    impl_->renderer.draw_cells(cells, cols, rows, px, ox, oy);
}

Extent Session::cell_size() const noexcept { return Extent{impl_->cell_w, impl_->cell_h}; }
Rgb Session::default_bg() const noexcept { return impl_->renderer.default_bg(); }
bool Session::cursor_animating() const noexcept { return impl_->renderer.animating(); }

void Session::set_cursor_animation(bool enabled, int time_ms, bool trail) noexcept {
    impl_->cursor_anim_ = {enabled, time_ms, trail};
    impl_->renderer.set_cursor_animation(enabled, time_ms, trail);
}

void Session::set_selection_color(Rgb c) noexcept {
    impl_->selection_bg_ = c;
    impl_->renderer.set_selection_color(c);
}

int Session::cursor_blink_ms() const noexcept { return impl_->cursor_blink_ms_; }
void Session::set_cursor_blink_ms(int ms) noexcept { impl_->cursor_blink_ms_ = ms < 0 ? 0 : ms; }

Session::Behavior Session::behavior() const noexcept { return impl_->behavior_; }
void Session::set_behavior(const Behavior &b) noexcept { impl_->behavior_ = b; }
void Session::set_on_bell(std::function<void()> cb) noexcept { impl_->on_bell_ = std::move(cb); }
void Session::flash_visual_bell() noexcept { impl_->renderer.flash_bell(); }

void Session::resize(PixelSize px) {
    const Extent ng = impl_->renderer.cells_for(px);
    if (ng.cols != impl_->grid.cols || ng.rows != impl_->grid.rows) {
        impl_->grid = ng;
        impl_->model.screen.resize(ng);
        (void)impl_->pty.resize(ng);
    }
}

int Session::font_pixel_size() const noexcept { return impl_->font_px_; }

bool Session::set_font_pixel_size(int px, PixelSize surface_px) {
    px = std::clamp(px, 6, 200);
    if (px == impl_->font_px_) return false;

    // Rebuild the glyph atlas at the new size, then the renderer over it. A GL
    // context must be current (the atlas + renderer allocate GL objects) — the
    // host guarantees this, same as at create(). On any failure we keep the old
    // renderer untouched, so a bad size never breaks a live terminal.
    auto atlas = gfx::FontAtlas::create(impl_->font_path_, px, impl_->font_fallback_,
                                        impl_->ligatures_, impl_->style_files_);
    if (!atlas) return false;
    const int cw = atlas->cell_width();
    const int ch = atlas->cell_height();
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) return false;

    impl_->renderer = std::move(*renderer);
    impl_->renderer.set_cursor_animation(impl_->cursor_anim_.enabled, impl_->cursor_anim_.time_ms,
                                         impl_->cursor_anim_.trail);
    impl_->renderer.set_selection_color(impl_->selection_bg_);
    impl_->cell_w = cw;
    impl_->cell_h = ch;
    impl_->font_px_ = px;
    impl_->model.screen.set_cell_size(cw, ch);
    impl_->pty.set_cell_pixels(cw, ch);

    // The cell size changed, so the grid dimensions for the same surface change:
    // recompute and push the new geometry to the model and the child.
    const Extent ng = impl_->renderer.cells_for(surface_px);
    impl_->grid = ng;
    impl_->model.screen.resize(ng);
    (void)impl_->pty.resize(ng);
    return true;
}

void Session::set_default_colors(Rgb fg, Rgb bg) {
    // Record fg + bg edits the renderer applies next frame (same channel OSC
    // 10/11 use), and recolor the whole grid.
    using CE = term::Screen::ColorEdit;
    impl_->model.screen.edit_color(CE{CE::Target::fg, 0, false, fg});
    impl_->model.screen.edit_color(CE{CE::Target::bg, 0, false, bg});
}

bool Session::set_font(std::string_view family_or_file, PixelSize surface_px) {
    // The host resolves a family name to a concrete file (it knows the OS font
    // dirs); we take a path directly. An empty/unchanged path is a no-op.
    std::string path{family_or_file};
    if (path.empty() || path == impl_->font_path_) return false;

    auto atlas = gfx::FontAtlas::create(path, impl_->font_px_, impl_->font_fallback_,
                                        impl_->ligatures_, impl_->style_files_);
    if (!atlas) return false;
    const int cw = atlas->cell_width();
    const int ch = atlas->cell_height();
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) return false;

    impl_->renderer = std::move(*renderer);
    impl_->renderer.set_cursor_animation(impl_->cursor_anim_.enabled, impl_->cursor_anim_.time_ms,
                                         impl_->cursor_anim_.trail);
    impl_->renderer.set_selection_color(impl_->selection_bg_);
    impl_->cell_w = cw;
    impl_->cell_h = ch;
    impl_->font_path_ = path;
    impl_->model.screen.set_cell_size(cw, ch);
    impl_->pty.set_cell_pixels(cw, ch);

    const Extent ng = impl_->renderer.cells_for(surface_px);
    impl_->grid = ng;
    impl_->model.screen.resize(ng);
    (void)impl_->pty.resize(ng);
    return true;
}

void Session::set_cursor_shape(int shape) noexcept {
    using CS = term::Screen::CursorShape;
    const CS cs = shape == 1 ? CS::bar : shape == 2 ? CS::underline : CS::block;
    auto st = impl_->model.screen.cursor_style(); // keep current blink phase
    st.shape = cs;
    impl_->model.screen.set_cursor_style(st);
}

bool Session::set_ligatures(bool on, PixelSize surface_px) {
    if (on == impl_->ligatures_) return true; // no-op
    impl_->ligatures_ = on;
    // Rebuild the atlas with the new ligature flag (the shaper is created/
    // destroyed inside FontAtlas::create), same path as a font change.
    auto atlas = gfx::FontAtlas::create(impl_->font_path_, impl_->font_px_, impl_->font_fallback_,
                                        on, impl_->style_files_);
    if (!atlas) return false;
    const int cw = atlas->cell_width();
    const int ch = atlas->cell_height();
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) return false;
    impl_->renderer = std::move(*renderer);
    impl_->renderer.set_cursor_animation(impl_->cursor_anim_.enabled, impl_->cursor_anim_.time_ms,
                                         impl_->cursor_anim_.trail);
    impl_->renderer.set_selection_color(impl_->selection_bg_);
    impl_->cell_w = cw;
    impl_->cell_h = ch;
    impl_->model.screen.set_cell_size(cw, ch);
    impl_->pty.set_cell_pixels(cw, ch);
    const Extent ng = impl_->renderer.cells_for(surface_px);
    impl_->grid = ng;
    impl_->model.screen.resize(ng);
    (void)impl_->pty.resize(ng);
    return true;
}

void Session::send_text(std::string_view utf8) { (void)impl_->pty.write(utf8); }
void Session::set_preedit(std::string_view utf8, int cursor_cells) {
    impl_->model.screen.set_preedit(std::string{utf8}, cursor_cells);
}

// --- pure input encoding ---------------------------------------------------
// Key encoding lives in toe/input/keymap.cpp: a zero-allocation, fixed-buffer
// encoder covering the full modifier matrix, function keys, Alt/Meta and the
// application-cursor-keys mode. update() just supplies the terminal context.

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
                // Typing snaps the view back to the live prompt (every terminal
                // does this) so you never type blindly into scrollback.
                impl_->model.screen.scroll_to_bottom();
                KeyContext kctx{impl_->model.screen.app_cursor_keys(),
                                impl_->model.screen.kitty_keyboard_flags()};
                KeyBuf kb;
                std::span<const char> bytes = encode_key(m.event, kctx, kb);
                if (!bytes.empty()) {
                    out.emplace_back(WriteChild{std::string(bytes.data(), bytes.size())});
                }
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

bool Session::pump_output() { return impl_->drain(); }

bool Session::output_pending() const noexcept { return impl_->more_pending; }

void Session::scroll(int lines) { impl_->model.screen.scroll(lines); }
void Session::scroll_to_bottom() { impl_->model.screen.scroll_to_bottom(); }
int Session::scroll_offset() const noexcept { return impl_->model.screen.scroll_offset(); }

// --- command-block navigation ----------------------------------------------
std::uint64_t Session::focused_block() const noexcept { return impl_->focused_block; }

bool Session::jump_to_prev_command() {
    const auto &blocks = impl_->model.commands.blocks();
    if (blocks.empty()) return false;
    auto &scr = impl_->model.screen;
    // The absolute row currently at the top of the viewport; we want the newest
    // block whose prompt is strictly ABOVE it (older). If none is focused yet,
    // start from the current view top.
    const std::int64_t top_abs = scr.viewport_to_abs(0);
    const term::CommandBlock *target = nullptr;
    for (const auto &b : blocks) { // newest-last; scan for the closest-above
        if (b.prompt_row >= 0 && b.prompt_row < top_abs) target = &b;
    }
    if (!target) return false;
    scr.scroll_to_abs_row(target->prompt_row, /*margin=*/0);
    impl_->focused_block = target->id;
    return true;
}

bool Session::jump_to_next_command() {
    const auto &blocks = impl_->model.commands.blocks();
    if (blocks.empty()) return false;
    auto &scr = impl_->model.screen;
    const std::int64_t top_abs = scr.viewport_to_abs(0);
    const term::CommandBlock *target = nullptr;
    for (const auto &b : blocks) { // first block whose prompt is BELOW the top
        if (b.prompt_row >= 0 && b.prompt_row > top_abs) { target = &b; break; }
    }
    if (!target) { // past the newest block -> return to the live view
        scr.scroll_to_bottom();
        impl_->focused_block = 0;
        return scr.scroll_offset() == 0;
    }
    scr.scroll_to_abs_row(target->prompt_row, /*margin=*/0);
    impl_->focused_block = target->id;
    return true;
}

bool Session::jump_to_last_failed() {
    const auto &blocks = impl_->model.commands.blocks();
    const term::CommandBlock *target = nullptr;
    for (const auto &b : blocks) { // newest-last: last non-zero-exit wins
        if (b.finished() && !b.succeeded() && b.prompt_row >= 0) target = &b;
    }
    if (!target) return false;
    impl_->model.screen.scroll_to_abs_row(target->prompt_row, /*margin=*/0);
    impl_->focused_block = target->id;
    return true;
}

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

std::string_view Session::link_at(int vrow, int col) const noexcept {
    return impl_->model.screen.link_at(vrow, col);
}

bool Session::tick_animations(std::uint64_t now_ms) {
    return impl_->model.screen.tick_animations(now_ms);
}

std::uint64_t Session::next_animation_deadline() const noexcept {
    return impl_->model.screen.next_animation_deadline();
}

bool Session::set_hover(int vrow, int col) noexcept {
    return impl_->model.screen.set_hover(vrow, col);
}

void Session::report_focus(bool focused) {
    if (std::string_view seq = impl_->model.screen.report_focus(focused); !seq.empty()) {
        (void)impl_->pty.write(seq);
    }
}

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
std::string Session::working_dir() const { return impl_->model.working_dir; }

namespace {
// Resolve a stored CommandBlock into a public CommandView, slicing the live
// Screen for the command line and output text. Coordinates are absolute rows.
CommandView resolve_block(const term::CommandBlock &b, const term::Screen &scr) {
    CommandView v;
    v.id = b.id;
    v.cwd = b.cwd;
    v.exit_code = b.exit_code;
    v.finished = b.finished();
    v.duration_ms = b.duration_ms();

    // Command line: from the B mark (input_row/col). The C mark (output_row)
    // often fires on the SAME row as the command (before the newline), so the
    // command always spans at least its own row; output begins on the next row.
    if (b.input_row >= 0) {
        std::int64_t cmd_end = (b.output_row >= 0) ? b.output_row : scr.total_rows();
        if (cmd_end <= b.input_row) cmd_end = b.input_row + 1; // same-row C
        v.command = scr.text_between_abs(b.input_row, cmd_end, b.input_col);
    }
    // Output: the rows after the command line up to the D mark (end_row); while
    // running, read to the current bottom of the buffer.
    if (b.output_row >= 0) {
        // If C landed on the command's own row, real output starts one row down.
        std::int64_t out_start = b.output_row;
        if (b.input_row >= 0 && out_start <= b.input_row) out_start = b.input_row + 1;
        const std::int64_t out_end = (b.end_row >= 0) ? b.end_row : scr.total_rows();
        v.output = scr.text_between_abs(out_start, out_end);
        v.output_lines = v.output.empty()
                             ? 0
                             : 1 + std::count(v.output.begin(), v.output.end(), '\n');
    }
    return v;
}
} // namespace

std::vector<CommandView> Session::commands() const {
    const auto &log = impl_->model.commands;
    const auto &scr = impl_->model.screen;
    std::vector<CommandView> out;
    out.reserve(log.size());
    for (const auto &b : log.blocks()) out.push_back(resolve_block(b, scr));
    return out;
}

std::optional<CommandView> Session::last_command() const {
    const auto *b = impl_->model.commands.last_completed();
    if (!b) return std::nullopt;
    return resolve_block(*b, impl_->model.screen);
}

std::optional<CommandView> Session::current_command() const {
    const auto *b = impl_->model.commands.in_progress();
    if (!b) return std::nullopt;
    return resolve_block(*b, impl_->model.screen);
}

std::uint64_t Session::commands_generation() const noexcept {
    return impl_->model.commands.generation();
}

bool Session::frame_settled() const noexcept {
    return !impl_->model.screen.sync_active();
}

std::string Session::snapshot_text(bool include_scrollback) const {
    const auto &scr = impl_->model.screen;
    const Extent g = impl_->grid;
    const std::int64_t total = scr.total_rows();
    const std::int64_t bottom = total;                     // exclusive
    const std::int64_t vis_top = total - g.rows;           // first visible abs row
    const std::int64_t top = include_scrollback ? 0 : (vis_top < 0 ? 0 : vis_top);
    return scr.text_between_abs(top, bottom);
}

std::vector<int> Session::changed_rows(std::uint64_t since_generation) const {
    const auto &scr = impl_->model.screen;
    const Extent g = impl_->grid;
    std::vector<int> rows;
    // A row counts as changed when its per-row version exceeds the caller's
    // token. row_version() returns 0 when scrolled into history (can't tell) —
    // treat that as "changed" so the caller re-reads conservatively.
    for (int r = 0; r < g.rows; ++r) {
        const std::uint64_t v = scr.row_version(r);
        if (v == 0 || v > since_generation) rows.push_back(r);
    }
    return rows;
}
std::uint64_t Session::generation() const noexcept { return impl_->model.screen.generation(); }
int Session::pty_fd() const noexcept { return impl_->pty.fd(); }
int Session::cell_width() const noexcept { return impl_->cell_w; }
int Session::cell_height() const noexcept { return impl_->cell_h; }
bool Session::bracketed_paste() const noexcept { return impl_->model.screen.bracketed_paste(); }
bool Session::on_alt_screen() const noexcept { return impl_->model.screen.on_alt_screen(); }
bool Session::cursor_blinks() const noexcept {
    return impl_->model.screen.cursor_style().blink;
}

std::optional<std::string> Session::take_clipboard_request() {
    if (!impl_->clipboard_request) return std::nullopt;
    std::optional<std::string> req = std::move(impl_->clipboard_request);
    impl_->clipboard_request.reset();
    return req;
}

// ---------------------------------------------------------------------------
// Terminal — construction and the single transition.

namespace {
// Resolve a font family to a file path by globbing the standard font dirs.
// Generic aliases (monospace/sans/serif) map to a monospace search, preferring
// well-known programming fonts. No fontconfig — zero deps, zero threads.
std::string resolve_font_path(std::string family) {
    std::string needle;
    for (char c : family)
        if (c != ' ') needle += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool generic = needle.empty() || needle == "monospace" || needle == "mono" ||
                         needle == "sans" || needle == "serif" || needle == "sans-serif";
    if (generic) needle = "mono";

    const char *home = std::getenv("HOME");
    std::vector<std::filesystem::path> roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (home) roots.emplace_back(std::string{home} + "/.local/share/fonts");
    if (home) roots.emplace_back(std::string{home} + "/.fonts");

    // Preferred programming/mono fonts, best first. We record the best-ranked
    // match found and only return after scanning, so priority order wins over
    // filesystem order. Bold/italic files are never chosen as the base face.
    static const char *prefer[] = {
        "jetbrainsmono", "firacode", "cascadiacode", "cascadiamono", "iosevka",
        "hack", "dejavusansmono", "notosansmono", "liberationmono", "adwaitamono", "ubuntumono",
    };
    int best_rank = 9999;
    std::string best_prefer, first_mono, first_any, named_best;
    for (const auto &root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto &p = it->path();
            const auto ext = p.extension().string();
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".TTF" &&
                ext != ".OTF" && ext != ".TTC")
                continue;
            std::string name;
            for (char c : p.filename().string())
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            // Reject any bold/italic/light/oblique/condensed variant as a base.
            const bool plain = name.find("bold") == std::string::npos &&
                               name.find("italic") == std::string::npos &&
                               name.find("oblique") == std::string::npos &&
                               name.find("light") == std::string::npos &&
                               name.find("thin") == std::string::npos &&
                               name.find("medium") == std::string::npos &&
                               name.find("condensed") == std::string::npos &&
                               name.find("extra") == std::string::npos &&
                               name.find("semi") == std::string::npos;
            if (!plain) continue; // only regular weights are eligible
            if (generic) {
                for (int rank = 0; rank < static_cast<int>(std::size(prefer)); ++rank) {
                    if (name.find(prefer[rank]) != std::string::npos && rank < best_rank) {
                        best_rank = rank;
                        best_prefer = p.string();
                    }
                }
                if (first_any.empty()) first_any = p.string();
                if (first_mono.empty() && name.find("mono") != std::string::npos)
                    first_mono = p.string();
            } else if (name.find(needle) != std::string::npos) {
                if (named_best.empty() || name.find("regular") != std::string::npos)
                    named_best = p.string();
            }
        }
    }
    if (generic) {
        if (!best_prefer.empty()) return best_prefer; // highest-ranked known font
        if (!first_mono.empty()) return first_mono;
        return first_any;
    }
    return named_best;
}
} // namespace

Result<Terminal> Terminal::create(const Config &cfg, PixelSize px) {
    std::string font_path = cfg.font_file;
    if (font_path.empty()) font_path = resolve_font_path(cfg.font_family);
    if (font_path.empty())
        return fail("font: no font file for '" + cfg.font_family +
                    "' (set font.file to a .ttf/.otf path)");
    gfx::FontAtlas::StyleFiles style_files{cfg.font_file_bold, cfg.font_file_italic,
                                           cfg.font_file_bold_italic};
    auto atlas = gfx::FontAtlas::create(font_path, cfg.font_pixel_size, cfg.font_fallback,
                                        cfg.ligatures, style_files);
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

    // Adopt the child PTY the host opened. toe owns no process-creation policy;
    // it just drives the fd it was handed (see toe/pty/pty_source.hpp).
    auto pty = Pty::adopt(cfg.source);
    if (!pty) {
        return std::unexpected(pty.error());
    }

    auto impl = std::make_unique<Session::Impl>(cfg, grid, std::move(*renderer), std::move(*pty),
                                                cw, ch);
    impl->font_path_ = font_path;
    impl->font_fallback_ = cfg.font_fallback;
    impl->style_files_ = {cfg.font_file_bold, cfg.font_file_italic, cfg.font_file_bold_italic};
    impl->ligatures_ = cfg.ligatures;
    impl->font_px_ = cfg.font_pixel_size;
    impl->cursor_anim_ = cfg.cursor_anim;
    impl->selection_bg_ = cfg.selection_bg;
    impl->cursor_blink_ms_ = cfg.cursor_blink_ms;
    impl->behavior_ = {cfg.wheel_lines, cfg.scroll_on_output, cfg.scroll_on_keystroke,
                       cfg.copy_on_select};
    // Initial cursor shape from config (apps may override via DECSCUSR).
    {
        using CS = term::Screen::CursorShape;
        const CS cs = cfg.cursor_shape == 1 ? CS::bar
                      : cfg.cursor_shape == 2 ? CS::underline
                                              : CS::block;
        auto st = impl->model.screen.cursor_style();
        st.shape = cs;
        impl->model.screen.set_cursor_style(st);
    }
    impl->renderer.set_cursor_animation(cfg.cursor_anim.enabled, cfg.cursor_anim.time_ms,
                                        cfg.cursor_anim.trail);
    impl->renderer.set_selection_color(cfg.selection_bg);
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
            // The child closed the pty. It is terminating; give the zombie a
            // brief window to appear so we capture the real exit code rather
            // than racing waitpid (EOF on the master can precede the zombie).
            std::optional<ExitCode> ec;
            for (int i = 0; i < 200 && !(ec = session->impl_->pty.child().try_reap()); ++i) {
                struct timespec ts{0, 500'000}; // 0.5ms
                ::nanosleep(&ts, nullptr);
            }
            state_ = Exited{ec.value_or(ExitCode{0}).value};
            result.exited = &std::get<Exited>(state_);
        }
    } else {
        result.exited = &std::get<Exited>(state_);
    }
    return result;
}

} // namespace toe
