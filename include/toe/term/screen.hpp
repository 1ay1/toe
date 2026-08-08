// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The screen: a fixed grid of cells plus a cursor and the active pen. It is
// the reduction target for the parser's Action stream — `apply(Action)` is the
// one entry point that mutates terminal state. All indexing goes through
// bounds-checked accessors keyed on the strong Row/Col types, so an out-of-
// range or axis-swapped access is a logic error we can localize, not UB.

#ifndef TOE_TERM_SCREEN_HPP
#define TOE_TERM_SCREEN_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "toe/core/tea.hpp"
#include "toe/core/types.hpp"
#include "toe/term/graphics.hpp"
#include "toe/term/cell.hpp"
#include "toe/vt/parser.hpp"

namespace toe::term {

class Screen {
public:
    explicit Screen(Extent size);

    // Resize the grid, preserving overlapping content (top-left anchored).
    void resize(Extent size);

    [[nodiscard]] Extent size() const noexcept { return size_; }
    [[nodiscard]] Pos cursor() const noexcept { return cursor_; }

    // Read-only access to a row's cells, for the renderer. When scrolled back,
    // the top rows come from history; the request is always in viewport
    // coordinates (row 0 = top of the visible area).
    [[nodiscard]] std::span<const Cell> row(Row r) const;

    // --- scrollback --------------------------------------------------------
    // How many history rows exist above the live view.
    [[nodiscard]] std::int32_t history_rows() const noexcept {
        return static_cast<std::int32_t>(history_.size());
    }
    // Current scroll position: 0 = live (bottom), up to history_rows().
    [[nodiscard]] std::int32_t scroll_offset() const noexcept { return scroll_offset_; }
    // Scroll by `delta` rows (positive = up/into history). Clamped.
    void scroll(std::int32_t delta);
    // Jump back to the live view.
    void scroll_to_bottom();
    // True when the cursor is within the currently-visible region.
    [[nodiscard]] bool cursor_visible() const noexcept { return scroll_offset_ == 0; }

    // --- modes -------------------------------------------------------------
    // Whether the text cursor should be drawn (DECTCEM, CSI ?25 h/l).
    [[nodiscard]] bool cursor_shown() const noexcept { return cursor_shown_; }

    // Cursor shape (DECSCUSR, CSI Ps SP q). `blink` is the app's requested
    // blink state; a host that drives its own blink timing may ignore it.
    enum class CursorShape { block, underline, bar };
    struct CursorStyle {
        CursorShape shape{CursorShape::block};
        bool blink{true};
        constexpr auto operator<=>(const CursorStyle &) const = default;
    };
    [[nodiscard]] CursorStyle cursor_style() const noexcept { return cursor_style_; }

    // --- Kitty keyboard protocol -------------------------------------------
    // Progressive-enhancement flags, as a bitset (kitty spec):
    //   1 disambiguate escape codes, 2 report event types (press/repeat/release),
    //   4 report alternate keys, 8 report all keys as escape codes,
    //   16 report associated text. The active flags are the top of a stack the
    //   app pushes/pops so nested programs restore cleanly.
    enum KittyFlags : std::uint8_t {
        KittyDisambiguate = 1,
        KittyReportEvents = 2,
        KittyReportAlternate = 4,
        KittyReportAllKeys = 8,
        KittyReportText = 16,
    };
    [[nodiscard]] std::uint8_t kitty_keyboard_flags() const noexcept {
        return kitty_stack_.back();
    }

    // --- dynamic colours (OSC 4/104, 10/11, 12/112) ------------------------
    // Recorded as data in the model; the renderer syncs its palette from these
    // each frame. `palette_epoch()` bumps on any change so the renderer knows
    // to re-pull cheaply. index<256 = a palette slot; the specials use tags.
    struct ColorEdit {
        enum class Target : std::uint8_t { index, fg, bg, cursor, all } target{};
        std::uint8_t index{0};        // valid when target==index
        bool reset{false};            // true => restore default (rgb ignored)
        Rgb rgb{};
    };
    [[nodiscard]] std::uint64_t palette_epoch() const noexcept { return palette_epoch_; }
    [[nodiscard]] const std::vector<ColorEdit> &color_edits() const noexcept {
        return color_edits_;
    }
    // Record a dynamic-colour change (called from OSC handling).
    void edit_color(const ColorEdit &e) {
        color_edits_.push_back(e);
        ++palette_epoch_;
        touch();
    }
    // OSC 104 with no params: reset the entire palette to defaults.
    void reset_all_palette() {
        color_edits_.push_back({ColorEdit::Target::all, 0, true, {}});
        ++palette_epoch_;
        touch();
    }
    // Whether the alternate screen is active (no scrollback while on it).
    [[nodiscard]] bool on_alt_screen() const noexcept { return on_alt_; }
    // Mouse tracking mode requested by the app (CSI ?1000/1002/1003 + ?1006).
    enum class MouseMode { off, x10, normal, button, any };
    [[nodiscard]] MouseMode mouse_mode() const noexcept { return mouse_mode_; }
    [[nodiscard]] bool mouse_sgr() const noexcept { return mouse_sgr_; }
    // Bracketed paste (CSI ?2004): wrap pasted text in ESC[200~ / ESC[201~.
    [[nodiscard]] bool bracketed_paste() const noexcept { return bracketed_paste_; }
    // DECCKM (CSI ?1): cursor/nav keys send SS3 (ESC O A) instead of CSI
    // (ESC [ A). vim/less/readline toggle this.
    [[nodiscard]] bool app_cursor_keys() const noexcept { return app_cursor_keys_; }
    // DECKPAM/DECKPNM (ESC =/ESC >): application keypad mode.
    [[nodiscard]] bool app_keypad() const noexcept { return app_keypad_; }

    // --- selection ---------------------------------------------------------
    // Selection granularity. Char = arbitrary span; Line = whole rows; Block =
    // rectangular column range.
    enum class SelectMode { none, character, line, block };

    // An absolute grid position: row is measured from the oldest history line
    // (history row 0), so a selection survives scrolling. Live grid row r maps
    // to absolute row history_rows() + r.
    struct AbsPos {
        std::int64_t row = 0;
        std::int32_t col = 0;
        constexpr auto operator<=>(const AbsPos &) const = default;
    };

    // Begin a selection at an absolute position (e.g. mouse-down).
    void selection_begin(AbsPos p, SelectMode mode);
    // Extend the active selection to p (e.g. mouse-drag).
    void selection_extend(AbsPos p);
    // Select the whole word under an absolute position (double-click).
    void selection_word(AbsPos p);
    // Select the whole line at an absolute position (triple-click).
    void selection_line(AbsPos p);
    // Clear the selection.
    void selection_clear();
    [[nodiscard]] bool has_selection() const noexcept { return sel_mode_ != SelectMode::none; }
    [[nodiscard]] SelectMode selection_mode() const noexcept { return sel_mode_; }

    // True if the cell at absolute (row,col) is within the current selection.
    [[nodiscard]] bool is_selected(std::int64_t abs_row, std::int32_t col) const noexcept;

    // Extract the selected text as UTF-8 (trailing blanks trimmed per line).
    [[nodiscard]] std::string selected_text() const;

    // Convert a viewport (visible) row to an absolute row, and back.
    [[nodiscard]] std::int64_t viewport_to_abs(std::int32_t vrow) const noexcept;

    // The single reduction step: fold one parser Action into the screen. Any
    // effects the action demands (query replies, bell, …) are appended to
    // `out` — the screen performs no I/O itself; it only produces effect data.
    void apply(const vt::Action &action, Cmds &out);

    // Convenience for tests / simple call sites: apply and return the effects.
    [[nodiscard]] Cmds apply(const vt::Action &action) {
        Cmds out;
        apply(action, out);
        return out;
    }

    // Monotonic damage counter — bumped on any mutation so the renderer can
    // skip re-uploading an unchanged grid. While synchronized output (DEC mode
    // 2026) is active the reported value is FROZEN: the app is mid-frame and
    // doesn't want the host to draw a partial update. It jumps once when the
    // app ends the batch (?2026l), so the whole frame appears atomically.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return sync_output_ ? sync_frozen_gen_ : generation_;
    }

    // Per-row damage token for the renderer's cache. Returns a 64-bit value
    // that changes iff viewport row `vrow`'s displayed content changed since
    // last frame — letting the renderer skip re-fingerprinting untouched rows.
    // Returns 0 ("unknown — please rehash") when scrolled into history, where
    // the fast path isn't worth the complexity. Live rows carry a per-row epoch
    // bumped by every mutation that writes them.
    [[nodiscard]] std::uint64_t row_version(std::int32_t vrow) const noexcept {
        if (scroll_offset_ != 0) return 0;
        if (vrow < 0 || vrow >= size_.rows) return 0;
        return row_epoch_[static_cast<std::size_t>(vrow)];
    }

private:
    // --- bounds-checked cell access (the only raw indexing in the class) ---
    [[nodiscard]] Cell &at(Row r, Col c);
    [[nodiscard]] const Cell &at(Row r, Col c) const;
    [[nodiscard]] std::size_t index(Row r, Col c) const noexcept;
    void reset_row_map(); // set row_of_ to identity

    // --- primitive operations the Actions decompose into ---
    void put(char32_t cp);           // write glyph at cursor, advance
    void execute(std::uint8_t c0);   // handle a C0 control
    void csi(const vt::CsiDispatch &d);
    void esc(const vt::EscDispatch &d);
    void dcs(std::string_view prefix, std::string_view data);
    void decrqss(std::string_view req);           // DECRQSS: report a setting
    [[nodiscard]] std::string current_sgr() const; // active pen as SGR params
    void report_mode(int mode, bool priv);         // DECRQM: report mode state
    // Kitty keyboard protocol CSI u variants: push/pop/set/query flags.
    void kitty_keyboard(const vt::CsiDispatch &d);
    void soft_reset(); // DECSTR: reset modes/attrs/region, keep screen content

    void line_feed();
    void carriage_return();
    void backspace();
    void tab();
    void scroll_up(std::int32_t n);
    void scroll_down(std::int32_t n);      // SD / reverse scroll within region
    void insert_lines(std::int32_t n);     // IL
    void delete_lines(std::int32_t n);     // DL
    void insert_chars(std::int32_t n);     // ICH
    void delete_chars(std::int32_t n);     // DCH
    void erase_chars(std::int32_t n);      // ECH
    void cursor_tab(std::int32_t n);       // CHT (forward tab stops)
    void cursor_back_tab(std::int32_t n);  // CBT (backward tab stops)
    void set_tab_stop();                   // HTS (set tab at cursor col)
    void clear_tab_stop(int mode);         // TBC (0 = at cursor, 3 = all)
    [[nodiscard]] std::int32_t next_tab_stop(std::int32_t col) const noexcept;
    [[nodiscard]] std::int32_t prev_tab_stop(std::int32_t col) const noexcept;
    void set_scroll_region(int top, int bottom); // DECSTBM
    void save_cursor();                    // DECSC / CSI s
    void restore_cursor();                 // DECRC / CSI u
    void set_private_mode(int mode, bool set); // CSI ? Pm h/l
    void enter_alt_screen();
    void leave_alt_screen();
    void erase_in_display(int mode);
    void erase_in_line(int mode);
    void move_cursor_abs(Row r, Col c);
    void apply_sgr(std::span<const int> params, std::span<const std::uint8_t> sub);
    void clamp_cursor() noexcept;
    void touch() noexcept { ++generation_; }

    // Background Color Erase (BCE). Every erase/clear op (ECH, EL, ED, DCH,
    // ICH gap, scroll-blank) fills with THIS, not a default cell: xterm-family
    // terminals fill erased cells with the CURRENT background colour, which is
    // how full-screen TUIs (htop meter bars, powerline fills, colour panes)
    // paint solid coloured regions with a single erase. We keep the pen's bg
    // and the reverse flag (a reversed blank shows the fg colour) but drop the
    // fg colour and text-decoration attrs, which don't affect a space.
    [[nodiscard]] Cell blank_cell() const noexcept {
        Pen p;
        p.bg = pen_.bg;
        p.attr = pen_.attr & Attr::Reverse;
        return Cell{U' ', p, 1};
    }

    // --- per-row damage epochs (renderer cache fast-path) ------------------
    // Each live grid row carries a 64-bit epoch drawn from a monotonic global
    // sequence; a write to the row stamps it with a fresh value. The renderer
    // reads row_version() to skip re-fingerprinting rows that didn't change.
    std::vector<std::uint64_t> row_epoch_{};
    std::uint64_t epoch_seq_{1};
    void stamp(std::int32_t live_row) noexcept {
        if (live_row >= 0 && live_row < static_cast<std::int32_t>(row_epoch_.size()))
            row_epoch_[static_cast<std::size_t>(live_row)] = ++epoch_seq_;
    }
    void stamp_all() noexcept {
        const std::uint64_t base = ++epoch_seq_;
        for (auto &e : row_epoch_) e = base;
        epoch_seq_ += row_epoch_.size();
    }

    Extent size_{};
    std::vector<Cell> cells_{}; // physical cell store, size_.area() cells
    // Logical-to-physical row map. cells_ holds the live grid's rows in some
    // physical order; row_of_[r] is the physical row backing logical row r.
    // Scrolling rotates this map (O(rows) index shuffle) instead of moving
    // O(rows*cols) cells — a `cols`x speedup on the flood/scroll hot path. Each
    // physical row is still contiguous, so row() hands out a valid span.
    std::vector<std::uint32_t> row_of_{};
    Pos cursor_{};
    Pen pen_{};
    bool wrap_pending_{false};   // DEC-style deferred wrap at right margin
    std::uint64_t generation_{1};
    bool sync_output_{false};          // DEC 2026 synchronized-output active?
    std::uint64_t sync_frozen_gen_{1}; // generation reported while sync is on
    bool focus_events_{false};         // DEC 1004 focus reporting requested?

    // Transient effect accumulator: valid only for the duration of an apply()
    // call, so query handlers (csi/esc/dcs) can emit Cmds without an I/O sink.
    Cmds *pending_{nullptr};
    void reply(std::string bytes) const {
        if (pending_) pending_->emplace_back(WriteChild{std::move(bytes)});
    }

    // Scroll region (DECSTBM), 0-based inclusive. Defaults to the whole grid.
    std::int32_t scroll_top_{0};
    std::int32_t scroll_bottom_{0}; // set to rows-1 in ctor/resize

    // Saved cursor state (DECSC/DECRC).
    Pos saved_cursor_{};
    Pen saved_pen_{};

    // Terminal modes (DEC private).
    bool cursor_shown_{true};
    CursorStyle cursor_style_{};
    // Kitty keyboard flag stack; back() is active. Never empty (base = 0).
    std::vector<std::uint8_t> kitty_stack_{0};
    // Dynamic-colour edits (OSC 4/104/10/11/12/112), applied by the renderer.
    std::vector<ColorEdit> color_edits_{};
    std::uint64_t palette_epoch_{0};
    bool on_alt_{false};
    bool bracketed_paste_{false};
    bool app_cursor_keys_{false};
    bool app_keypad_{false};
    bool autowrap_{true}; // DECAWM (?7): wrap at right margin (on by default)
    bool mouse_sgr_{false};
    MouseMode mouse_mode_{MouseMode::off};

    // Saved primary-screen state while the alternate screen is active.
    std::vector<Cell> saved_primary_{};
    std::vector<std::uint32_t> saved_primary_row_of_{};
    Pos saved_primary_cursor_{};

    // OSC 8 hyperlinks. cur_link_ is the id stamped onto glyphs written while a
    // link is open; 0 = none. links_ interns the URIs (links_[id-1]); an id
    // never repeats a URI within a run so hovering/clicking a whole link works.
    std::uint16_t cur_link_{0};
    std::string cur_link_key_{};              // the OSC 8 id= param, for coalescing
    std::vector<std::string> links_{};        // id-1 -> URI
    std::uint16_t hover_link_{0};             // link id under the pointer, 0=none

    // Inline images (kitty graphics). Placements are anchored in absolute rows.
    Graphics graphics_{};
    int cell_w_{0}, cell_h_{0}; // host-provided, for sizing image placements

    // Tab stops: one flag per column (default every 8th).
    std::vector<bool> tab_stops_{};

public:
    // OSC 8 hyperlink open/close (called by the OSC handler). params = the id=
    // section, uri = target; an empty uri closes the current link.
    void set_hyperlink(std::string_view params, std::string_view uri);

    // The OSC 8 URI under a viewport cell, or empty if none. The host opens it
    // on click.
    [[nodiscard]] std::string_view link_at(std::int32_t vrow, std::int32_t col) const noexcept;

    // Highlight (hover-underline) the link under a viewport cell so it reads as
    // clickable. Pass (-1,-1) to clear. Returns true if the hovered link id
    // changed (the host uses that to trigger a redraw).
    bool set_hover(std::int32_t vrow, std::int32_t col) noexcept;
    [[nodiscard]] std::uint16_t hover_link() const noexcept { return hover_link_; }

    // Focus reporting (DEC 1004): when the app enabled it, returns the bytes to
    // send the child on focus in/out (CSI I / CSI O), else empty. The host
    // writes them to the PTY.
    [[nodiscard]] std::string_view report_focus(bool focused) const noexcept {
        if (!focus_events_) return {};
        return focused ? "\x1b[I" : "\x1b[O";
    }

    // Inline images (kitty graphics protocol). The renderer reads placements
    // and image pixels to draw them over the grid.
    [[nodiscard]] const Graphics &graphics() const noexcept { return graphics_; }
    // Cell metrics the graphics layer needs to size placements; set by the host.
    void set_cell_size(int w, int h) noexcept { cell_w_ = w; cell_h_ = h; }
    // Advance image animations to now_ms; bumps damage on a frame change.
    bool tick_animations(std::uint64_t now_ms) {
        if (graphics_.advance_animations(now_ms)) { touch(); return true; }
        return false;
    }
    [[nodiscard]] std::uint64_t next_animation_deadline() const noexcept {
        return graphics_.next_animation_deadline();
    }

private:
    // Character-set state (VT100 line-drawing). G0/G1 each hold ASCII or the
    // DEC Special Graphics set; SI (^O) selects G0, SO (^N) selects G1. When
    // the active set is DecGraphics, ASCII 0x5F..0x7E are mapped to the box-
    // drawing / block Unicode glyphs — this is how tmux, dialog, mc and older
    // ncurses apps draw borders without UTF-8.
    enum class Charset : std::uint8_t { Ascii, DecGraphics };
    Charset charset_g0_{Charset::Ascii};
    Charset charset_g1_{Charset::Ascii};
    bool charset_use_g1_{false}; // SO active?
    [[nodiscard]] char32_t map_charset(char32_t cp) const noexcept;

    // Selection state, in absolute (history-aware) coordinates.
    SelectMode sel_mode_{SelectMode::none};
    AbsPos sel_anchor_{};
    AbsPos sel_active_{};

    // Normalized [begin, end] of the current selection (begin <= end).
    [[nodiscard]] std::pair<AbsPos, AbsPos> selection_span() const noexcept;
    // Fetch the cell at an absolute row/col (history or live), or nullptr.
    [[nodiscard]] const Cell *cell_at_abs(std::int64_t abs_row, std::int32_t col) const noexcept;

    // Scrollback: completed lines that scrolled off the top, newest at back.
    std::deque<std::vector<Cell>> history_{};
    // Parallel to history_: true if that line wrapped into the next (soft wrap,
    // no newline) rather than ending at a real line break. Drives reflow.
    std::deque<bool> hist_wrapped_{};
    // Parallel to the LIVE grid's logical rows: soft-wrap flag per row.
    std::vector<bool> live_wrapped_{};
    std::int32_t scroll_offset_{0};                 // rows scrolled into history
    std::size_t max_history_{10000};                // ring-buffer cap

    // Rewrap all content (history + live) from old_cols to the new width when a
    // resize changes the column count. Preserves logical lines + the cursor.
    void reflow(Extent old_size, Extent new_size);

    // Soft-wrap flag helpers for the live grid (indexed by logical row).
    void set_wrapped(std::int32_t row, bool w) noexcept {
        if (row >= 0 && row < static_cast<std::int32_t>(live_wrapped_.size()))
            live_wrapped_[static_cast<std::size_t>(row)] = w;
    }
    [[nodiscard]] bool wrapped_at(std::int32_t row) const noexcept {
        return row >= 0 && row < static_cast<std::int32_t>(live_wrapped_.size()) &&
               live_wrapped_[static_cast<std::size_t>(row)];
    }
};

} // namespace toe::term

#endif // TOE_TERM_SCREEN_HPP
