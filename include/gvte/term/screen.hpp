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
#include <span>
#include <vector>

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
    // Scroll by `delta` rows (positive = into history / up). Clamped.
    void scroll(std::int32_t delta);
    // Jump back to the live view.
    void scroll_to_bottom();
    // True when the cursor is within the currently-visible region.
    [[nodiscard]] bool cursor_visible() const noexcept { return scroll_offset_ == 0; }

    // The single reduction step: fold one parser Action into the screen.
    void apply(const vt::Action &action);

    // Monotonic damage counter — bumped on any mutation so the renderer can
    // skip re-uploading an unchanged grid.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

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

    void line_feed();
    void carriage_return();
    void backspace();
    void tab();
    void scroll_up(std::int32_t n);
    void erase_in_display(int mode);
    void erase_in_line(int mode);
    void move_cursor_abs(Row r, Col c);
    void apply_sgr(std::span<const int> params);
    void clamp_cursor() noexcept;
    void touch() noexcept { ++generation_; }

    Extent size_{};
    std::vector<Cell> cells_{}; // row-major, size_.area() cells (the live grid)
    Pos cursor_{};
    Pen pen_{};
    bool wrap_pending_{false};   // DEC-style deferred wrap at right margin
    std::uint64_t generation_{1};

    // Scrollback: completed lines that scrolled off the top, newest at back.
    std::deque<std::vector<Cell>> history_{};
    std::int32_t scroll_offset_{0};                 // rows scrolled into history
    std::size_t max_history_{10000};                // ring-buffer cap
};

} // namespace gvte::term

#endif // GVTE_TERM_SCREEN_HPP
