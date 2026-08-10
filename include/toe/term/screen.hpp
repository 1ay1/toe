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
#include "toe/term/rowring.hpp"
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
        return static_cast<std::int32_t>(ring_.scrollback());
    }
    // Current scroll position: 0 = live (bottom), up to history_rows().
    [[nodiscard]] std::int32_t scroll_offset() const noexcept { return scroll_offset_; }
    // Scroll by `delta` rows (positive = up/into history). Clamped.
    void scroll(std::int32_t delta);
    // Jump back to the live view.
    void scroll_to_bottom();
    // Scroll so absolute row `abs_row` sits `margin` rows below the viewport
    // top (0 = at the very top). Used to jump to a command block's prompt.
    // Clamps to the valid scroll range.
    void scroll_to_abs_row(std::int64_t abs_row, std::int32_t margin = 1);
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
    // Set the cursor shape/blink. Apps override this live via DECSCUSR; the host
    // uses it to apply the configured default (initial value, and on a config
    // reload). Touches the screen so the change is drawn.
    void set_cursor_style(CursorStyle st) noexcept {
        if (st != cursor_style_) { cursor_style_ = st; touch(); }
    }

    // --- IME preedit (composition) -----------------------------------------
    // The in-progress composition string shown inline at the cursor before the
    // host commits it (dead keys, compose sequences, CJK/emoji IME). The host
    // sets it as it changes and clears it on commit; the renderer overlays it.
    void set_preedit(std::string utf8, int cursor_cells = -1) {
        if (preedit_ != utf8 || preedit_cursor_ != cursor_cells) {
            preedit_ = std::move(utf8);
            preedit_cursor_ = cursor_cells; // -1 => caret at end of the string
            touch();
        }
    }
    [[nodiscard]] std::string_view preedit() const noexcept { return preedit_; }
    [[nodiscard]] int preedit_cursor() const noexcept { return preedit_cursor_; }

    // --- DEC line attributes (ESC # 3/4/5/6) -------------------------------
    // Per-row rendition: normal, double-width, or the top/bottom half of a
    // double-height line. The renderer scales the row's glyphs accordingly.
    enum class LineAttr : std::uint8_t { normal, double_width, double_top, double_bottom };
    [[nodiscard]] LineAttr line_attr(std::int32_t vrow) const noexcept;
    void set_line_attr(std::int32_t row, LineAttr a);

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
    // Extend the active selection to p (e.g. mouse-drag). Honours the
    // granularity set by the last begin/word/line: a drag after a double-click
    // extends by whole WORDS and after a triple-click by whole LINES, snapping
    // both ends outward — exactly like iTerm2/kitty. A plain drag is per-cell.
    void selection_extend(AbsPos p);
    // Select the whole word under an absolute position (double-click). The span
    // follows soft-wrapped continuation rows so a wrapped word/URL selects whole.
    void selection_word(AbsPos p);
    // Select the whole line at an absolute position (triple-click). Spans the
    // full logical (soft-wrapped) line, not just the one physical row.
    void selection_line(AbsPos p);
    // Clear the selection.
    void selection_clear();
    [[nodiscard]] bool has_selection() const noexcept { return sel_mode_ != SelectMode::none; }
    [[nodiscard]] SelectMode selection_mode() const noexcept { return sel_mode_; }

    // Extra codepoints (beyond the built-in alnum + path/URL set) that count as
    // part of a word for double-click selection. Host wires this from config so
    // e.g. "'" or "," can be made word-joining. Empty by default.
    void set_word_separators_extra(std::u32string_view cps) { word_extra_ = cps; }

    // True if the cell at absolute (row,col) is within the current selection.
    [[nodiscard]] bool is_selected(std::int64_t abs_row, std::int32_t col) const noexcept;

    // Extract the selected text as UTF-8 (trailing blanks trimmed per line).
    [[nodiscard]] std::string selected_text() const;

    // --- scrollback search -------------------------------------------------
    // One match: a run of `len` cells starting at an absolute position.
    struct SearchMatch {
        AbsPos start{};
        std::int32_t len{0};
    };
    // Scan the whole ring (history + live) for `query` (UTF-8, case-insensitive
    // unless `case_sensitive`). Rebuilds the match list, keeps the current
    // index near the previous position when possible, and scrolls the current
    // match into view. Returns the number of matches (0 clears highlighting).
    std::size_t search(std::string_view query, bool case_sensitive = false);
    // Advance/retreat the current match (wraps around) and scroll it into view.
    void search_next();
    void search_prev();
    // Drop all search state (query, matches, highlighting).
    void search_clear();
    [[nodiscard]] bool searching() const noexcept { return !search_matches_.empty(); }
    [[nodiscard]] std::size_t search_count() const noexcept { return search_matches_.size(); }
    // 1-based index of the current match (0 when none), for a "3/17" readout.
    [[nodiscard]] std::size_t search_current() const noexcept {
        return search_matches_.empty() ? 0 : search_cur_ + 1;
    }
    // Highlight predicates for the renderer (parallel to is_selected).
    [[nodiscard]] bool is_search_match(std::int64_t abs_row, std::int32_t col) const noexcept;
    [[nodiscard]] bool is_current_search_match(std::int64_t abs_row,
                                               std::int32_t col) const noexcept;

    // Total rows in the ring (history + live). An absolute row is valid in
    // [0, total_rows()). Used to clamp CommandBlock coordinates before slicing.
    [[nodiscard]] std::int64_t total_rows() const noexcept;

    // Extract UTF-8 text spanning absolute rows [row0, row1) (row1 exclusive),
    // starting at `col0` on the FIRST row (0 elsewhere). Trailing blanks are
    // trimmed per line; rows are joined with '\n'. Out-of-range rows are
    // skipped. This is how a command block's input line / output is read on
    // demand from its coordinates without the log copying the scrollback.
    [[nodiscard]] std::string text_between_abs(std::int64_t row0, std::int64_t row1,
                                               std::int32_t col0 = 0) const;

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

    // True while the app is mid-frame under DEC 2026 synchronized output — i.e.
    // it has begun a batch (?2026h) and not yet committed it (?2026l). A host
    // or agent should treat the screen as NOT settled until this is false, so a
    // read/snapshot never captures a torn, half-drawn frame.
    [[nodiscard]] bool sync_active() const noexcept { return sync_output_; }

    // Per-row damage token for the renderer's cache. Returns a 64-bit value
    // that changes iff viewport row `vrow`'s displayed content changed since
    // last frame — letting the renderer skip re-fingerprinting untouched rows.
    // Returns 0 ("unknown — please rehash") when scrolled into history, where
    // the fast path isn't worth the complexity. Live rows carry a per-row epoch
    // bumped by every mutation that writes them.
    [[nodiscard]] std::uint64_t row_version(std::int32_t vrow) const noexcept {
        if (scroll_offset_ != 0) return 0;
        if (vrow < 0 || vrow >= size_.rows) return 0;
        // The row's own epoch OR the last screen-wide stamp, whichever is newer,
        // so stamp_all() can be O(1) (bump all_stamp_) instead of O(rows).
        const std::uint64_t e = row_epoch_[static_cast<std::size_t>(vrow)];
        return e > all_stamp_ ? e : all_stamp_;
    }

private:
    // --- bounds-checked cell access (the only raw indexing in the class) ---
    [[nodiscard]] Cell &at(Row r, Col c);
    [[nodiscard]] const Cell &at(Row r, Col c) const;
    [[nodiscard]] std::size_t index(Row r, Col c) const noexcept;
    [[nodiscard]] Cell *cell_ptr(Row r, Col c) noexcept; // ring row pointer for bulk writes

    // Before overwriting a cell, dissolve any double-width PAIR it belongs to:
    // a wide glyph is a LEAD (width 2) + a SPACER (width 0). Overwriting either
    // half must blank the other, or an orphan lead/spacer lingers and renders as
    // a gap or a stale half-glyph. Cheap: touches at most one neighbour, only
    // when the target is actually part of a pair.
    void clean_wide_at(Row r, std::int32_t c) noexcept;

    // --- primitive operations the Actions decompose into ---
    void put(char32_t cp);
    // Fast bulk write of a printable-ASCII run at the cursor (the flood hot
    // path): fills whole spans within the current row's margin in one stamped
    // pass, wrapping via put() only at the boundary.
    void put_ascii_run(std::string_view ascii);           // write glyph at cursor, advance
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
    // DEC rectangular area ops (1-based inclusive coords; 0 => screen edge).
    void fill_rect(int top, int left, int bottom, int right, char32_t cp);
    void change_rect_attrs(int top, int left, int bottom, int right,
                           std::span<const int> attrs, bool reverse);

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
    void set_lr_margins(int left, int right);     // DECSLRM
    // The active right/left column bound: the margin when DECLRMM is on, else
    // the screen edge. Used by wrap, CR, insert/delete, and horizontal scroll.
    [[nodiscard]] std::int32_t left_bound() const noexcept {
        return lr_margins_ ? scroll_left_ : 0;
    }
    [[nodiscard]] std::int32_t right_bound() const noexcept {
        return lr_margins_ ? scroll_right_ : size_.cols - 1;
    }
    // True if the cursor is within the vertical scroll region (margins only
    // constrain operations when the cursor is inside the region).
    [[nodiscard]] bool cursor_in_region() const noexcept {
        const std::int32_t r = cursor_.row.get(), c = cursor_.col.get();
        return r >= scroll_top_ && r <= scroll_bottom_ && c >= left_bound() && c <= right_bound();
    }
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
    std::uint64_t all_stamp_{0}; // last screen-wide stamp (O(1) stamp_all)
    void stamp(std::int32_t live_row) noexcept {
        if (live_row >= 0 && live_row < static_cast<std::int32_t>(row_epoch_.size()))
            row_epoch_[static_cast<std::size_t>(live_row)] = ++epoch_seq_;
    }
    // Stamp a contiguous range of rows [lo, hi] (inclusive) with ONE fresh
    // epoch. Used by region scrolls / insert-delete-lines so only the rows that
    // actually moved are re-fingerprinted by the renderer — vs stamp_all(), which
    // invalidates the whole grid and makes the renderer re-upload every row.
    void stamp_range(std::int32_t lo, std::int32_t hi) noexcept {
        const std::uint64_t e = ++epoch_seq_;
        const std::int32_t n = static_cast<std::int32_t>(row_epoch_.size());
        if (lo < 0) lo = 0;
        if (hi >= n) hi = n - 1;
        for (std::int32_t r = lo; r <= hi; ++r)
            row_epoch_[static_cast<std::size_t>(r)] = e;
    }
    void stamp_all() noexcept {
        // O(1): bump the screen-wide stamp; row_version() folds it in per read.
        all_stamp_ = ++epoch_seq_;
    }

    Extent size_{};
    // Unified zero-copy row ring: the live grid AND scrollback share one arena
    // (see toe/term/rowring.hpp). A full-screen scroll is O(1) index work — no
    // row copy — because the scrolled-off row is already in the ring.
    RowRing ring_{};
    // The INACTIVE screen buffer, swapped in/out on alt-screen enter/leave. We
    // std::swap(ring_, alt_ring_) instead of copying the whole grid in and out
    // — O(1) vector swaps vs a per-cell memcpy of the entire screen on every
    // toggle (vim/less/htop each do it, and a torture toggle-storm did it 100k+
    // times). The primary's scrollback rides along in the swapped-out ring,
    // preserved intact.
    RowRing alt_ring_{};
    mutable std::vector<Cell> scratch_row_{}; // scratch for padded reads if needed
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
    // Left/right margins (DECSLRM), 0-based inclusive. Only active when the
    // DECLRMM mode (?69) is enabled; otherwise the full width [0, cols-1].
    std::int32_t scroll_left_{0};
    std::int32_t scroll_right_{0};  // set to cols-1 in ctor/resize
    bool lr_margins_{false};        // DECLRMM (?69): left/right margins active

    // Saved cursor state (DECSC/DECRC).
    Pos saved_cursor_{};
    Pen saved_pen_{};

    // Terminal modes (DEC private).
    bool cursor_shown_{true};
    CursorStyle cursor_style_{};
    char32_t last_char_{0}; // last printed codepoint, for REP (CSI b)
    std::string preedit_{};       // IME composition string (empty = none)
    int preedit_cursor_{-1};      // caret position within preedit, in cells
    // Per logical row DEC line attribute (ESC # 3/4/5/6). Sized to rows.
    std::vector<LineAttr> line_attr_{};
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

    // Saved primary cursor position while the alternate screen is active (the
    // grid itself lives in the swapped-out alt_ring_, above).
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

    // Drag granularity: after a double/triple click the drag snaps to whole
    // words / lines. `sel_pivot_lo_`/`sel_pivot_hi_` hold the ORIGINAL clicked
    // word (or line) span so extension pins the far edge to it as the pointer
    // sweeps past the anchor in either direction.
    enum class Grain { cell, word, line };
    Grain sel_grain_{Grain::cell};
    AbsPos sel_pivot_lo_{};
    AbsPos sel_pivot_hi_{};

    // Extra word-joining codepoints from config (see set_word_separators_extra).
    std::u32string word_extra_{};

    // Scrollback search state. Matches are in absolute coords, sorted by row
    // then col. search_cur_ indexes the "current" (strongly highlighted) match.
    std::vector<SearchMatch> search_matches_{};
    std::size_t search_cur_{0};
    std::u32string search_query_{}; // decoded query, retained for re-scan on new output
    bool search_case_{false};

    // Normalized [begin, end] of the current selection (begin <= end).
    [[nodiscard]] std::pair<AbsPos, AbsPos> selection_span() const noexcept;

    // Compute the soft-wrap-aware word span containing p. Returns {p,p} when p
    // is not on a word codepoint. Shared by double-click and word-drag.
    [[nodiscard]] std::pair<AbsPos, AbsPos> word_bounds_at(AbsPos p) const noexcept;
    // The full logical (soft-wrap-joined) line span containing absolute row.
    [[nodiscard]] std::pair<AbsPos, AbsPos> line_bounds_at(std::int64_t abs_row) const noexcept;
    // Auto-detect a bare URL (http/https/ftp/file/mailto) under a viewport
    // cell, for click-to-open when there's no OSC 8 link. Result cached in
    // detected_url_ (the returned view is valid until the next link_at call).
    [[nodiscard]] std::string_view detect_url_at(std::int32_t vrow,
                                                 std::int32_t col) const noexcept;
    mutable std::string detected_url_{};
    // Fetch the cell at an absolute row/col (history or live), or nullptr.
    [[nodiscard]] const Cell *cell_at_abs(std::int64_t abs_row, std::int32_t col) const noexcept;

    // (scrollback + soft-wrap flags now live in ring_)
    bool any_line_attr_ = false; // fast-skip flag: any non-normal line_attr_?
    std::int32_t scroll_offset_{0};                 // rows scrolled into history
    std::size_t max_history_{10000};                // ring-buffer cap

    // Rewrap all content (history + live) from old_cols to the new width when a
    // resize changes the column count. Preserves logical lines + the cursor.
    void reflow(Extent old_size, Extent new_size);

    // Reusable scratch for reflow, so a drag-resize / font-zoom storm reuses one
    // allocation instead of churning ~2 heap allocs per row every frame. The
    // whole buffer is represented FLAT: `reflow_cells_` is a single cell arena,
    // and `reflow_lines_`/`reflow_rows_` index into it as (offset,len) spans —
    // no per-line/per-row std::vector, so a resize is O(cells) copies with O(1)
    // allocations (amortised) rather than O(rows) heap traffic.
    struct Span {
        std::uint32_t off;   // start index into reflow_cells_
        std::uint32_t len;   // cell count
        bool wrapped;        // soft-wrap continuation follows (physical rows)
        bool from_live;      // originated in the live grid (logical lines)
        std::uint32_t line;  // source logical-line index (physical rows only)
    };
    std::vector<Cell> reflow_cells_{};
    std::vector<Span> reflow_lines_{}; // logical lines
    std::vector<Span> reflow_rows_{};  // rewrapped physical rows

    // Soft-wrap flag helpers for the live grid (indexed by visible row). These
    // route straight to the ring so wrapped flags travel with the row on scroll.
    void set_wrapped(std::int32_t row, bool w) noexcept {
        if (row >= 0 && row < size_.rows) ring_.set_view_wrapped(row, w);
    }
    [[nodiscard]] bool wrapped_at(std::int32_t row) const noexcept {
        return row >= 0 && row < size_.rows && ring_.view_wrapped(row);
    }
};

} // namespace toe::term

#endif // TOE_TERM_SCREEN_HPP
