// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe public API — a GPU-accelerated terminal, no GTK, no VTE.
//
// Design principle: illegal states are unrepresentable. A terminal's lifecycle
// is a two-state machine — Running or Exited — encoded as a sum type, not a
// boolean flag. Operations that only make sense on a live terminal (render,
// resize, key input) live on the `Session` state and are simply *absent* from
// the exited state, so calling them on a dead terminal is a compile error, not
// a runtime check. The one transition, `poll()`, is the sole way to observe a
// Running -> Exited change; you cannot fabricate the reverse.

#ifndef TOE_TERMINAL_HPP
#define TOE_TERMINAL_HPP

#include <memory>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "toe/core/tea.hpp"
#include "toe/core/types.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/input.hpp"
#include "toe/pty/pty_source.hpp"

namespace toe {

namespace term { struct Cell; } // for render_overlay's raw cell grid

// A resolved shell command block (OSC 133 shell integration): the structured,
// agent- and UI-friendly view of one command — its text, output, exit code,
// cwd and timing. Built on demand from the CommandLog + the live Screen, so the
// `command` / `output` strings are the actual rendered cells, not the raw bytes.
struct CommandView {
    std::uint64_t id{0};
    std::string command{};          // the command line the user ran
    std::string output{};           // its output (empty while nothing printed)
    std::string cwd{};              // working dir at prompt time (OSC 7)
    std::optional<int> exit_code{}; // nullopt while still running
    std::int64_t duration_ms{0};    // C→D wall-clock, 0 if unknown
    std::int64_t output_lines{0};   // line count of `output`
    bool finished{false};           // has a D mark (exit code known)
    // Absolute-row span of the block (for host UI: the command minimap flyout,
    // click-to-jump). -1 when unknown. `prompt_row` is where the command line
    // sits; `end_row` is exclusive (the live bottom while still running).
    std::int64_t prompt_row{-1};
    std::int64_t end_row{-1};

    [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
};

// --- configuration ---------------------------------------------------------
struct Config {
    std::string font_family = "monospace"; // empty -> system default monospace
    // Explicit font-file path. When set, used directly (no font discovery).
    // When empty, a small built-in resolver globs the system font dirs for a
    // file whose name contains `font_family`. No fontconfig, no threads.
    std::string font_file{};
    // Optional fallback font (CJK/emoji/symbols) for codepoints the primary
    // font lacks. A file path; empty disables fallback.
    std::string font_fallback{};
    // Optional REAL styled faces. When set, bold/italic/bold-italic text renders
    // from these actual font files (far better than synthesis). Each empty field
    // falls back to synthesizing that style from the regular face (embolden /
    // shear), so this is purely additive. The host resolves the family's style
    // variants and fills these in.
    std::string font_file_bold{};
    std::string font_file_italic{};
    std::string font_file_bold_italic{};
    // GSUB calt/liga shaping. Off by default: the built-in shaper handles the
    // simple ligature subtable forms, but complex programming fonts (JetBrains
    // Mono, Fira Code) use multi-pass contextual chains it can't fully evaluate,
    // so leave ligatures opt-in until that's addressed. Most terminals default
    // ligatures off anyway.
    bool ligatures = false;
    int font_pixel_size = 18;
    Rgb default_fg = rgb(220, 220, 220);
    Rgb default_bg = rgb(23, 23, 28);
    Rgb selection_bg = rgb(66, 84, 112); // selection highlight colour
    // Reverse-video selection: swap each selected cell's fg/bg instead of a
    // coloured highlight bg (the classic terminal look). Off = coloured bg.
    bool selection_invert = false;
    // Cursor blink half-period in ms; 0 = steady (no blink). Host policy, but
    // the engine's run loop reads it to pace the blink wave.
    int cursor_blink_ms = 530;
    // Initial cursor shape (apps override it live via DECSCUSR). 0 block, 1 bar,
    // 2 underline — matches hand's CursorShape enum order.
    int cursor_shape = 0;

    // Scroll behaviour (host policy applied by the EventRouter). Live-tunable.
    //   wheel_lines         — rows advanced per mouse-wheel notch
    //   scroll_on_output    — jump to the live bottom when new output arrives
    //   scroll_on_keystroke — jump to the live bottom when you type
    int wheel_lines = 3;
    bool scroll_on_output = false;
    bool scroll_on_keystroke = true;
    // Auto-copy a selection to the clipboard as soon as it's made.
    bool copy_on_select = false;
    // Extra codepoints (UTF-8) that count as part of a word for double-click
    // selection, beyond the built-in alnum + path/URL set. Empty by default.
    std::string word_separators{};
    // Inner window padding in pixels (grid inset on every edge). Live-settable.
    int padding = 0;
    // Window opacity in [0,1]; 1 = opaque. Live-settable.
    float opacity = 1.0f;

    // Cursor glide animation (the caret eases to its new cell instead of
    // snapping). Fully tunable so a host/config can turn it off or retune feel:
    //   enabled     — master on/off (off = instant snap, the classic behaviour)
    //   time_ms     — approach time constant; smaller = snappier, larger = floatier
    //   trail       — draw a fading comet trail on jumps > ~1.5 cells
    struct CursorAnim {
        bool enabled = true;
        int time_ms = 55;
        bool trail = true;
        int trail_len = 3; // number of fading comet ghosts on a long jump (0..6)
    } cursor_anim{};

    // Scrollback-search match highlight colours: every match uses match_bg, the
    // one the user is ON uses the brighter current_bg. Live-settable.
    Rgb search_match_bg = rgb(120, 96, 40);
    Rgb search_current_bg = rgb(255, 176, 32);

    // Command-minimap rail (OSC-133 shell integration): the right-edge visual
    // index of the session's commands. All host-tunable.
    bool rail_enabled = true;
    int rail_width = 7;                    // px (expands on hover)
    Rgb rail_ok = rgb(80, 200, 130);       // succeeded command segment
    Rgb rail_failed = rgb(235, 90, 90);    // failed command segment
    Rgb rail_running = rgb(240, 190, 70);  // in-flight command segment
    int rail_alpha = 210;                  // resting segment opacity (0..255)
    int rail_hover_halo = 90;              // hovered segment halo opacity

    // Selection fine-tuning: text-contrast floor, corner rounding fraction, and
    // the min luma stand-off before a selection colour is nudged to stay visible.
    float selection_contrast = 3.0f;
    float selection_radius = 0.28f;
    float selection_min_visibility = 0.11f;

    // The child terminal, adopted from the host. toe NEVER forks: the host
    // opens the PTY master (forkpty/ConPTY/ssh/tmux) and hands the fd + child
    // pid here — see toe/pty/pty_source.hpp. Must carry a valid fd (>= 0);
    // Terminal::create returns an error otherwise.
    AdoptFd source{};
};

// --- lifecycle states ------------------------------------------------------
class Session; // the Running state (defined below)

// The Exited state. Terminal; carries the child's exit code. Deliberately has
// no render/input methods — you cannot drive a dead terminal.
struct Exited {
    int code = 0;
};

// --- the live session ------------------------------------------------------
// Obtained only by polling a Running terminal. While you hold a Session&, the
// child is alive by construction. All the "only valid while running" verbs
// live here.
class Session {
public:
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) noexcept;
    Session &operator=(Session &&) noexcept;
    ~Session();

    // Draw the current grid into `rc`'s target framebuffer. `rc` is the
    // capability token proving a GL context is current on this thread (see
    // toe/gfx/render_target.hpp): render is now impossible to call without it,
    // and the host chooses the destination FBO. `cursor_on` lets the host drive
    // cursor blink from a wall-clock phase; pass true for a steady cursor.
    // Draw the current grid into `rc`'s target framebuffer. Returns the region
    // that changed (in pixels) so the host can damage only that area on the
    // compositor; empty() means nothing was redrawn (skip the present).
    DamageRect render(gfx::RenderContext &rc, PixelSize px, bool cursor_on = true,
                      bool blink_on = true);

    // Composite a raw cell grid over the terminal this frame (a settings panel,
    // search bar, notification — any in-terminal UI). Drawn with the same font
    // and pipeline as the grid, at pixel offset (ox, oy). Call AFTER render().
    // `bg_alpha` scales every cell's background (frosted-glass overlay). An
    // optional per-cell `alpha` plane (0..255, row-major, same dims) overrides
    // it per cell — e.g. a faint scrim outside a near-opaque panel.
    void render_overlay(gfx::RenderContext &rc, const term::Cell *cells, int cols, int rows,
                        PixelSize px, int ox = 0, int oy = 0, float bg_alpha = 1.0f,
                        const std::uint8_t *alpha = nullptr);

    // The current cell size in pixels (for laying out an overlay in cells).
    [[nodiscard]] Extent cell_size() const noexcept;
    // The default background colour (tracks live colour edits) — the host clears
    // the swapchain to this each frame.
    [[nodiscard]] Rgb default_bg() const noexcept;
    // True while the caret is still gliding to its new cell — the host keeps
    // presenting ~60fps frames until it settles (like inline-image animation).
    [[nodiscard]] bool cursor_animating() const noexcept;
    // Live-toggle/retune the caret glide (settings panel / config reload).
    void set_cursor_animation(bool enabled, int time_ms = 55, bool trail = true) noexcept;
    // Live-set the selection highlight colour.
    void set_selection_color(Rgb c) noexcept;
    // Reverse-video selection: swap each selected cell's fg/bg (classic terminal
    // look) instead of a coloured highlight. Live-toggle; rebuilds the cache.
    void set_selection_invert(bool on) noexcept;
    // Live-set extra word-joining codepoints (UTF-8) for double-click select.
    void set_word_separators(std::string_view utf8);
    // Cursor blink half-period (ms); 0 = steady. Read by the host run loop to
    // pace the blink; live-settable from the settings panel.
    [[nodiscard]] int cursor_blink_ms() const noexcept;
    void set_cursor_blink_ms(int ms) noexcept;

    // Host scroll/selection behaviour the EventRouter honours. Live-settable so
    // config edits (pane or file) take effect immediately.
    struct Behavior {
        int wheel_lines = 3;
        bool scroll_on_output = false;
        bool scroll_on_keystroke = true;
        bool copy_on_select = false;
    };
    [[nodiscard]] Behavior behavior() const noexcept;
    void set_behavior(const Behavior &b) noexcept;
    // Host bell handler, invoked on BEL (RingBell). The host decides audible
    // and/or visual, per the behavior config. Pass {} to disable.
    void set_on_bell(std::function<void()> cb) noexcept;
    // Trigger a brief visual-bell flash (a fading full-screen tint the renderer
    // draws). animating() stays true while it fades so the host keeps painting.
    void flash_visual_bell() noexcept;
    // Set the DEFAULT cursor shape (0 block, 1 bar, 2 underline). Apps still
    // override live via DECSCUSR; this is the config default, live-settable.
    void set_cursor_shape(int shape) noexcept;
    // Toggle programming ligatures live (rebuilds the atlas at the current px).
    bool set_ligatures(bool on, PixelSize surface_px);
    // Set inner window padding (px per edge) live; re-grids to the new area.
    void set_padding(int px, PixelSize surface_px) noexcept;
    // Current window padding (px per edge) — the EventRouter subtracts it when
    // mapping pointer pixels to cells.
    [[nodiscard]] int padding() const noexcept;
    // Window opacity in [0,1]: the host clears the swapchain at this alpha and
    // the renderer scales the background alpha, so a compositor shows the
    // desktop through the terminal bg. Live-settable.
    void set_opacity(float o) noexcept;
    [[nodiscard]] float opacity() const noexcept;
    void resize(PixelSize px);

    // Runtime font zoom. Rebuilds the glyph atlas + renderer at `px` pixels and
    // re-flows the grid to `surface_px`. Requires a current GL context (like
    // render/create). Returns true if the size actually changed. Clamped to a
    // sane range; on any rebuild failure the old renderer is kept and it returns
    // false, so a bad size can never break a live terminal.
    bool set_font_pixel_size(int px, PixelSize surface_px);
    [[nodiscard]] int font_pixel_size() const noexcept;

    // Live-apply the default foreground/background colors (like an OSC 10/11
    // from the app). Takes effect on the next frame; recolors the whole grid.
    void set_default_colors(Rgb fg, Rgb bg);

    // Live-apply the 16 ANSI palette colours (indices 0-15: 0-7 normal, 8-15
    // bright), like a batch of OSC 4 edits. Fewer than 16 sets a prefix; more
    // are ignored. Recolors the whole grid next frame. Powers theme switching.
    void set_palette(std::span<const Rgb> colors);

    // Live-apply the cursor colour (like OSC 12). Recolors next frame.
    void set_cursor_color(Rgb c);

    // Live-apply a new font by FAMILY (resolved to a file by the host) or an
    // explicit file path, rebuilding the atlas + renderer at the current pixel
    // size and re-flowing to `surface_px`. Requires a current GL context.
    // Returns true on success; on failure the old font is kept.
    bool set_font(std::string_view family_or_file, PixelSize surface_px);
    void send_key(const KeyEvent &ev);
    void send_text(std::string_view utf8);
    // Set the IME composition (preedit) string shown inline at the cursor while
    // the user is composing; call with an empty string to clear it on commit.
    // `cursor_cells` is the caret position within the string (-1 = at the end).
    void set_preedit(std::string_view utf8, int cursor_cells = -1);

    // --- The Elm Architecture entry point ----------------------------------
    // The single, pure-ish transition: fold a Msg into the terminal and return
    // the effects it demands (bytes to the child, clipboard/title changes, …).
    // The host is expected to interpret the returned Cmds via run(). Every
    // other input method below is a thin convenience over update().
    [[nodiscard]] Cmds update(const Msg &msg);

    // Execute a batch of Cmds (the impure interpreter). Host convenience so it
    // needn't reach into the runtime itself.
    void run(const Cmds &cmds);

    // Drain whatever child output is already readable on the PTY, folding it
    // into the grid, WITHOUT blocking. Returns false if the child has hung up.
    // The host calls this right after writing input so the child's echo lands
    // in the SAME frame it renders — collapsing local-echo latency from two
    // vsync intervals to one. Idempotent and cheap when nothing is pending.
    bool pump_output();

    // True when the last drain hit its per-call budget and the child still has
    // output queued. The host should render + poll input, then loop again
    // WITHOUT sleeping (the PTY fd is still readable) to keep a flood flowing.
    [[nodiscard]] bool output_pending() const noexcept;

    // Scrollback: move the view by `lines` (positive = up/into history).
    void scroll(int lines);
    void scroll_to_bottom();
    [[nodiscard]] int scroll_offset() const noexcept;

    // Command-minimap interaction: if the pixel (x,y) lands on the right-edge
    // rail, jump the view to the command block there (or to that scroll
    // position) and return true. `px` is the current drawable size. Lets a host
    // wire click-to-jump on the minimap with no knowledge of its geometry.
    bool rail_click(int x, int y, PixelSize px);
    // True if pixel x is within the rail's hit zone (for cursor/hover cues).
    [[nodiscard]] bool on_rail(int x, PixelSize px) const noexcept;
    // Update the rail hover highlight from a pointer at (x,y); clears it when
    // the pointer isn't on the rail. Cheap; call from mouse-move.
    void rail_hover(int x, int y, PixelSize px);
    // Live scrollbar drag: scroll so the pointer's rail row is at the viewport
    // top (a smooth scrub, distinct from rail_click's snap-to-command). The
    // host arms this on a rail mouse-down and calls it on each drag move (y may
    // leave the rail vertically). Returns true if it scrolled.
    bool rail_scrub(int x, int y, PixelSize px);

    // --- command-block navigation (OSC 133) --------------------------------
    // Jump the scroll view to a shell command's prompt. These power a
    // human-facing block UI over full-screen scrollback: step through past
    // commands, or jump straight to the last one that FAILED. Each returns true
    // if it moved the view (false = no such block). The `id` of the block
    // jumped to is reported so a host can highlight it.
    bool jump_to_prev_command();  // toward older commands (up)
    bool jump_to_next_command();  // toward newer commands (down); at the newest, go live
    bool jump_to_last_failed();   // most recent non-zero-exit command
    [[nodiscard]] std::uint64_t focused_block() const noexcept; // 0 = none
    // The absolute row the pointer is hovering on the rail (-1 = not on rail).
    // Set by rail_hover(); a host reads it to drive a command-list flyout.
    [[nodiscard]] std::int64_t rail_hover_row() const noexcept;
    // Total rows in the buffer (history + live) — the rail's coordinate space.
    [[nodiscard]] std::int64_t total_rows() const noexcept;
    // Jump the view so the command block `id`'s prompt sits near the top.
    // Returns true if the block exists. Powers flyout click-to-jump.
    bool jump_to_command(std::uint64_t id);

    // --- selection ---------------------------------------------------------
    // Begin/extend a selection at a VISIBLE cell (viewport row/col). mode: 0
    // character, 1 line, 2 block.
    void select_begin(int vrow, int col, int mode);
    void select_extend(int vrow, int col);
    void select_word(int vrow, int col);   // double-click: whole word
    void select_line(int vrow, int col);   // triple-click: whole line
    void select_clear();
    [[nodiscard]] bool has_selection() const noexcept;
    [[nodiscard]] std::string selected_text() const;

    // --- scrollback search -------------------------------------------------
    // Set/refresh the search query; scans the whole buffer, highlights matches,
    // scrolls the current match into view. Returns the match count.
    std::size_t search(std::string_view query, bool case_sensitive = false);
    void search_next();   // jump to the next match (wraps)
    void search_prev();   // jump to the previous match (wraps)
    void search_clear();  // drop the query + highlighting
    [[nodiscard]] bool searching() const noexcept;
    [[nodiscard]] std::size_t search_count() const noexcept;   // total matches
    [[nodiscard]] std::size_t search_current() const noexcept; // 1-based, 0 = none

    // The OSC 8 hyperlink URI under a viewport cell, or empty if none. The host
    // opens it (browser / xdg-open) on click.
    [[nodiscard]] std::string_view link_at(int vrow, int col) const noexcept;

    // Advance inline-image animations to wall-clock now_ms; returns true if a
    // frame changed (the host should redraw). Also drives the damage counter.
    bool tick_animations(std::uint64_t now_ms);
    // The soonest wall-clock ms an animation needs its next frame, or 0 if no
    // animation is active — the host caps its poll wait to this.
    [[nodiscard]] std::uint64_t next_animation_deadline() const noexcept;

    // Set the hover cell for OSC 8 link highlighting; pass (-1,-1) to clear.
    // Returns true if the hovered link changed (host should redraw).
    bool set_hover(int vrow, int col) noexcept;

    // Focus in/out from the window system. If the app enabled focus reporting
    // (DEC 1004), sends CSI I / CSI O to the child; otherwise a no-op.
    void report_focus(bool focused);

    // --- mouse reporting ---------------------------------------------------
    // True when the running app has requested mouse tracking (?1000/1002/1003).
    // While true, the host should forward pointer events to the app via
    // report_mouse() instead of doing local selection.
    [[nodiscard]] bool wants_mouse() const noexcept;
    // Does the app want reports for pure motion (?1003) / drags (?1002)?
    [[nodiscard]] bool wants_mouse_motion() const noexcept;
    [[nodiscard]] bool wants_mouse_drag() const noexcept;

    enum class MouseEvent { press, release, motion };
    // Encode a pointer event at cell (col,row0-based) for the app and write it
    // to the child. `button`: 0 left, 1 middle, 2 right, 3 none/release,
    // 64/65 wheel up/down. No-op when mouse tracking is off.
    void report_mouse(MouseEvent kind, int button, int col, int row, bool shift, bool alt,
                      bool ctrl);

    [[nodiscard]] Extent grid_size() const noexcept;
    [[nodiscard]] Pos cursor() const noexcept;
    [[nodiscard]] std::string window_title() const;
    // OSC 7: the child's reported working directory (empty until reported). A
    // host reads this to open new tabs/splits in the same directory.
    [[nodiscard]] std::string working_dir() const;

    // --- shell command blocks (OSC 133) ------------------------------------
    // Structured, resolved view of the recorded shell commands, oldest first.
    // Requires the shell to emit OSC 133 marks (shell integration); without it
    // the list is empty. This is the substrate for a block UI and for agent
    // read-out / the DEC 2034 Semantic Block Query.
    [[nodiscard]] std::vector<CommandView> commands() const;
    // The most recently COMPLETED command (has an exit code), or nullopt.
    [[nodiscard]] std::optional<CommandView> last_command() const;
    // The command currently executing (output started, not finished), or nullopt.
    [[nodiscard]] std::optional<CommandView> current_command() const;
    // Bumped whenever the command log changes — poll to avoid re-resolving.
    [[nodiscard]] std::uint64_t commands_generation() const noexcept;

    // --- agent / automation read-out ---------------------------------------
    // True when the screen is safe to read: not mid-frame under DEC 2026
    // synchronized output. A driver waits for this before snapshotting so it
    // never captures a torn, half-drawn frame.
    [[nodiscard]] bool frame_settled() const noexcept;

    // The visible screen as clean UTF-8 text (one line per row, trailing blanks
    // trimmed) — the token-frugal default read for an agent, versus the raw
    // ANSI byte log. `include_scrollback` extends upward through history.
    [[nodiscard]] std::string snapshot_text(bool include_scrollback = false) const;

    // Damage-delta read: the viewport row indices whose content changed since
    // `since_generation` (0 = all rows). Lets a driver re-read only what moved
    // instead of the whole grid. Pair with generation() as the token.
    [[nodiscard]] std::vector<int> changed_rows(std::uint64_t since_generation) const;
    [[nodiscard]] int cell_width() const noexcept;
    [[nodiscard]] int cell_height() const noexcept;

    // Monotonic damage counter: bumped on every state change that affects the
    // rendered output. A host renders only when this differs from the value it
    // last drew — no wasted frames when the terminal is idle.
    [[nodiscard]] std::uint64_t generation() const noexcept;

    // The child PTY's file descriptor. A host polls this alongside the surface
    // event fd to block idle instead of busy-spinning.
    [[nodiscard]] int pty_fd() const noexcept;

    // Terminal modes the host may need to honor.
    [[nodiscard]] bool bracketed_paste() const noexcept;
    [[nodiscard]] bool on_alt_screen() const noexcept;
    // Whether the app requested a *blinking* cursor (DECSCUSR). A host driving
    // its own blink timing should hold the cursor steady when this is false.
    [[nodiscard]] bool cursor_blinks() const noexcept;

    // OSC 52: an app may ask to set the system clipboard. If one is pending,
    // returns the UTF-8 text and clears the request; else returns nullopt. The
    // host is expected to poll this each frame and forward to its clipboard.
    [[nodiscard]] std::optional<std::string> take_clipboard_request();

private:
    friend class Terminal;
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// --- the terminal (the state machine) --------------------------------------
// Holds exactly one of {Running, Exited}. You reach the live Session only by
// matching the current state via poll(); there is no way to obtain a Session
// for an exited terminal.
class Terminal {
public:
    // Fallible construction (font load, GPU objects, PTY spawn). Requires a
    // current GL context on the calling thread. On success the terminal starts
    // in the Running state.
    static Result<Terminal> create(const Config &cfg, PixelSize px);

    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;
    Terminal(Terminal &&) noexcept = default;
    Terminal &operator=(Terminal &&) noexcept = default;
    ~Terminal() = default;

    // The single transition. Drains child output into the grid; if the child
    // has exited, transitions Running -> Exited. Returns a view of the state
    // AFTER the transition:
    //   - Session*  : still running (borrow it to render / send input)
    //   - Exited*   : it exited (host should tear down)
    // Exactly one pointer is non-null. Non-owning; valid until the next poll().
    struct Poll {
        Session *running = nullptr;
        const Exited *exited = nullptr;
    };
    [[nodiscard]] Poll poll();

    [[nodiscard]] bool running() const noexcept {
        return std::holds_alternative<Session>(state_);
    }

private:
    explicit Terminal(Session &&s) : state_{std::move(s)} {}
    std::variant<Session, Exited> state_;
};

} // namespace toe

#endif // TOE_TERMINAL_HPP
