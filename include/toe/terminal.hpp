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
#include <optional>
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

// --- configuration ---------------------------------------------------------
struct Config {
    std::string font_family = "monospace"; // empty -> system default monospace
    int font_pixel_size = 18;
    Rgb default_fg = rgb(220, 220, 220);
    Rgb default_bg = rgb(23, 23, 28);

    // Where the child terminal comes from. Defaults to spawning $SHELL via
    // forkpty (SpawnCommand{}), but a host may inject an already-open PTY fd
    // (AdoptFd) so toe never forks — see toe/pty/pty_source.hpp.
    PtySource source = SpawnCommand{};

    // Legacy convenience: if non-empty AND `source` still holds a default
    // SpawnCommand, these become the spawned argv. Prefer setting `source`.
    std::vector<std::string> command{};
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
    void resize(PixelSize px);
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
