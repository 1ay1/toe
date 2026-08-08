// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The screen: a fixed grid of cells plus a cursor and the active pen. It is
// the reduction target for the parser's Action stream — `apply(Action)` is the
// one entry point that mutates terminal state. All indexing goes through
// bounds-checked accessors keyed on the strong Row/Col types, so an out-of-
// range or axis-swapped access is a logic error we can localize, not UB.

#ifndef GVTE_TERM_SCREEN_HPP
#define GVTE_TERM_SCREEN_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gvte/core/tea.hpp"
#include "gvte/core/types.hpp"
#include "gvte/term/cell.hpp"
#include "gvte/vt/parser.hpp"

namespace gvte::term {

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
    // skip re-uploading an unchanged grid.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

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

    // --- primitive operations the Actions decompose into ---
    void put(char32_t cp);           // write glyph at cursor, advance
    void execute(std::uint8_t c0);   // handle a C0 control
    void csi(const vt::CsiDispatch &d);
    void esc(const vt::EscDispatch &d);
    void dcs(std::string_view prefix, std::string_view data);

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
    std::vector<Cell> cells_{}; // row-major, size_.area() cells (the live grid)
    Pos cursor_{};
    Pen pen_{};
    bool wrap_pending_{false};   // DEC-style deferred wrap at right margin
    std::uint64_t generation_{1};

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
    bool on_alt_{false};
    bool bracketed_paste_{false};
    bool app_cursor_keys_{false};
    bool app_keypad_{false};
    bool autowrap_{true}; // DECAWM (?7): wrap at right margin (on by default)
    bool mouse_sgr_{false};
    MouseMode mouse_mode_{MouseMode::off};

    // Saved primary-screen state while the alternate screen is active.
    std::vector<Cell> saved_primary_{};
    Pos saved_primary_cursor_{};

    // Tab stops: one flag per column (default every 8th).
    std::vector<bool> tab_stops_{};

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
    std::int32_t scroll_offset_{0};                 // rows scrolled into history
    std::size_t max_history_{10000};                // ring-buffer cap
};

} // namespace gvte::term

#endif // GVTE_TERM_SCREEN_HPP
