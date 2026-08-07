// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/term/screen.hpp"

#include <algorithm>
#include <cassert>
#include <clocale>
#include <cwchar>

namespace gvte::term {

namespace {
// A CSI parameter with a default when absent or zero-as-omitted.
int param_or(std::span<const int> p, std::size_t i, int def) {
    return (i < p.size() && p[i] != 0) ? p[i] : def;
}
int param_raw(std::span<const int> p, std::size_t i, int def) {
    return i < p.size() ? p[i] : def;
}
} // namespace

Screen::Screen(Extent size) : size_{size}, cells_(size.area()) {
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    row_epoch_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), 0);
    tab_stops_.assign(static_cast<std::size_t>(std::max(size_.cols, 1)), false);
    for (std::int32_t c = 0; c < size_.cols; c += 8) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
}

std::size_t Screen::index(Row r, Col c) const noexcept {
    return static_cast<std::size_t>(r.get()) * static_cast<std::size_t>(size_.cols) +
           static_cast<std::size_t>(c.get());
}

Cell &Screen::at(Row r, Col c) {
    assert(r.get() >= 0 && r.get() < size_.rows);
    assert(c.get() >= 0 && c.get() < size_.cols);
    // Non-const access is the write funnel: stamp the row's damage epoch so the
    // renderer knows it changed. (Over-stamping on the rare non-const read is
    // harmless — it only costs one extra row rebuild.)
    stamp(r.get());
    return cells_[index(r, c)];
}

const Cell &Screen::at(Row r, Col c) const {
    assert(r.get() >= 0 && r.get() < size_.rows);
    assert(c.get() >= 0 && c.get() < size_.cols);
    return cells_[index(r, c)];
}

std::span<const Cell> Screen::row(Row r) const {
    // Viewport row r (0 = top of visible area). When scrolled back by
    // scroll_offset_ rows, the first scroll_offset_ viewport rows are drawn
    // from history; the remainder from the top of the live grid.
    const std::int32_t vr = r.get();
    if (scroll_offset_ > 0) {
        // Index into history: the visible window starts scroll_offset_ rows
        // above the live grid's top.
        const std::int32_t hist_start =
            static_cast<std::int32_t>(history_.size()) - scroll_offset_;
        const std::int32_t hist_index = hist_start + vr;
        if (hist_index >= 0 && hist_index < static_cast<std::int32_t>(history_.size())) {
            return std::span<const Cell>{history_[static_cast<std::size_t>(hist_index)].data(),
                                         static_cast<std::size_t>(size_.cols)};
        }
        // Past the end of history -> into the live grid.
        const std::int32_t live_row = hist_index - static_cast<std::int32_t>(history_.size());
        const std::size_t base = index(Row{live_row}, Col{0});
        return std::span<const Cell>{cells_.data() + base, static_cast<std::size_t>(size_.cols)};
    }
    const std::size_t base = index(r, Col{0});
    return std::span<const Cell>{cells_.data() + base, static_cast<std::size_t>(size_.cols)};
}

void Screen::scroll(std::int32_t delta) {
    const std::int32_t max = static_cast<std::int32_t>(history_.size());
    scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max);
    touch();
}

void Screen::scroll_to_bottom() {
    if (scroll_offset_ != 0) {
        scroll_offset_ = 0;
        touch();
    }
}

void Screen::resize(Extent size) {
    if (size == size_ || size.cols <= 0 || size.rows <= 0) {
        if (size == size_) return;
    }
    std::vector<Cell> next(size.area());
    const std::int32_t copy_rows = std::min(size.rows, size_.rows);
    const std::int32_t copy_cols = std::min(size.cols, size_.cols);
    for (std::int32_t r = 0; r < copy_rows; ++r) {
        for (std::int32_t c = 0; c < copy_cols; ++c) {
            next[static_cast<std::size_t>(r) * static_cast<std::size_t>(size.cols) +
                 static_cast<std::size_t>(c)] = at(Row{r}, Col{c});
        }
    }
    cells_ = std::move(next);
    size_ = size;
    // A resize resets the scroll region to the full screen (xterm behavior).
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    // Rebuild default tab stops for the new width.
    tab_stops_.assign(static_cast<std::size_t>(std::max(size_.cols, 1)), false);
    for (std::int32_t c = 0; c < size_.cols; c += 8) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
    clamp_cursor();
    row_epoch_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), 0);
    stamp_all();
    touch();
}

void Screen::clamp_cursor() noexcept {
    cursor_.row = Row{std::clamp(cursor_.row.get(), 0, size_.rows - 1)};
    cursor_.col = Col{std::clamp(cursor_.col.get(), 0, size_.cols - 1)};
}

// --- the reduction ---------------------------------------------------------

void Screen::apply(const vt::Action &action, Cmds &out) {
    pending_ = &out; // query handlers push effects here for the duration of this call
    std::visit(
        [&](auto &&a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, vt::Print>) {
                put(a.cp);
            } else if constexpr (std::is_same_v<T, vt::Execute>) {
                execute(a.byte);
            } else if constexpr (std::is_same_v<T, vt::CsiDispatch>) {
                csi(a);
            } else if constexpr (std::is_same_v<T, vt::EscDispatch>) {
                esc(a);
            } else if constexpr (std::is_same_v<T, vt::OscDispatch>) {
                // OSC (title, clipboard, colour queries) is handled by the
                // owning Terminal (which knows the palette); ignored here.
            } else if constexpr (std::is_same_v<T, vt::DcsDispatch>) {
                dcs(a.prefix, a.data);
            }
        },
        action);
    pending_ = nullptr;
}

namespace {
// Display width of a codepoint: 2 for double-width (CJK, emoji, fullwidth),
// 0 for combining/zero-width marks, 1 otherwise. Backed by wcwidth; a one-time
// setlocale makes it honor the Unicode East-Asian-Width tables.
int char_width(char32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20) return 0; // controls never occupy a cell here
    // ASCII printables (the overwhelming majority of terminal output) are
    // always single-width — skip the expensive libc wcwidth() table lookup,
    // which otherwise dominates parse time under a flood of plain text.
    if (cp < 0x7f) return 1;
    static const bool locale_set = [] {
        std::setlocale(LC_CTYPE, "");
        return true;
    }();
    (void)locale_set;
    const int w = ::wcwidth(static_cast<wchar_t>(cp));
    if (w < 0) return 1;      // unknown -> assume single-width rather than drop
    return w > 2 ? 2 : w;
}
} // namespace

void Screen::put(char32_t cp) {
    const int w = char_width(cp);

    // Combining / zero-width marks attach to the preceding cell rather than
    // advancing the cursor (best-effort: we keep the base glyph as-is).
    if (w == 0) {
        touch();
        return;
    }

    if (wrap_pending_) {
        cursor_.col = Col{0};
        line_feed();
        wrap_pending_ = false;
    }

    // A double-width glyph that won't fit before the right margin wraps to the
    // next line first (leaving the last column blank), as real terminals do.
    if (w == 2 && cursor_.col.get() + 1 >= size_.cols) {
        if (autowrap_) {
            // Blank the final column, then wrap.
            at(cursor_.row, cursor_.col) = Cell{};
            cursor_.col = Col{0};
            line_feed();
        } else {
            // No room and no wrap: overwrite in place as a single cell.
            at(cursor_.row, cursor_.col) = Cell{cp, pen_, 1};
            touch();
            return;
        }
    }

    at(cursor_.row, cursor_.col) = Cell{cp, pen_, static_cast<std::uint8_t>(w)};
    if (w == 2) {
        // The second half is a spacer the renderer skips.
        at(cursor_.row, cursor_.col + 1) = Cell{U' ', pen_, 0};
    }

    const int advance = w;
    if (cursor_.col.get() + advance >= size_.cols) {
        if (autowrap_) {
            wrap_pending_ = true;
        }
    } else {
        cursor_.col += advance;
    }
    touch();
}

void Screen::execute(std::uint8_t c0) {
    switch (c0) {
    case 0x08: backspace(); break;         // BS
    case 0x09: tab(); break;               // HT
    case 0x0A:                              // LF
    case 0x0B:                              // VT
    case 0x0C: line_feed(); break;         // FF
    case 0x0D: carriage_return(); break;   // CR
    case 0x07: // BEL
        if (pending_) pending_->emplace_back(RingBell{});
        break;
    default: break;
    }
}

void Screen::line_feed() {
    wrap_pending_ = false;
    if (cursor_.row.get() == scroll_bottom_) {
        scroll_up(1); // at the bottom margin: scroll the region
    } else if (cursor_.row.get() + 1 < size_.rows) {
        ++cursor_.row;
    }
    touch();
}

void Screen::carriage_return() {
    wrap_pending_ = false;
    cursor_.col = Col{0};
    touch();
}

void Screen::backspace() {
    wrap_pending_ = false;
    if (cursor_.col.get() > 0) {
        --cursor_.col;
        touch();
    }
}

void Screen::tab() {
    const std::int32_t next = next_tab_stop(cursor_.col.get());
    cursor_.col = Col{std::min(next, size_.cols - 1)};
    touch();
}

std::int32_t Screen::next_tab_stop(std::int32_t col) const noexcept {
    for (std::int32_t c = col + 1; c < size_.cols; ++c) {
        if (c < static_cast<std::int32_t>(tab_stops_.size()) && tab_stops_[static_cast<std::size_t>(c)]) {
            return c;
        }
    }
    return size_.cols - 1;
}

std::int32_t Screen::prev_tab_stop(std::int32_t col) const noexcept {
    for (std::int32_t c = col - 1; c > 0; --c) {
        if (c < static_cast<std::int32_t>(tab_stops_.size()) && tab_stops_[static_cast<std::size_t>(c)]) {
            return c;
        }
    }
    return 0;
}

void Screen::set_tab_stop() {
    const std::int32_t c = cursor_.col.get();
    if (c >= 0 && c < static_cast<std::int32_t>(tab_stops_.size())) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
}

void Screen::clear_tab_stop(int mode) {
    if (mode == 3) {
        std::fill(tab_stops_.begin(), tab_stops_.end(), false);
    } else { // mode 0: clear the stop at the cursor column
        const std::int32_t c = cursor_.col.get();
        if (c >= 0 && c < static_cast<std::int32_t>(tab_stops_.size())) {
            tab_stops_[static_cast<std::size_t>(c)] = false;
        }
    }
}

// Scroll the region [scroll_top_, scroll_bottom_] up by n rows. Rows leaving
// the top of a FULL-SCREEN region (top==0) are pushed to scrollback history;
// within a restricted region they're simply discarded (VTE/xterm semantics).
void Screen::scroll_up(std::int32_t n) {
    const std::int32_t region = scroll_bottom_ - scroll_top_ + 1;
    n = std::clamp(n, 0, region);
    if (n == 0) return;
    const std::size_t stride = static_cast<std::size_t>(size_.cols);
    const bool full_screen = (scroll_top_ == 0 && scroll_bottom_ == size_.rows - 1);

    // Lines that scroll off the top of a FULL-screen scroll go into the
    // scrollback — but ONLY on the primary buffer. The alternate screen (htop,
    // vim, less, …) has no scrollback: it repaints by scrolling, so pushing its
    // lines to history would flood the scrollback with the app's frames and
    // leave them behind after it exits.
    if (full_screen && !on_alt_) {
        for (std::int32_t i = 0; i < n; ++i) {
            const std::size_t off = stride * static_cast<std::size_t>(i);
            history_.emplace_back(cells_.begin() + static_cast<std::ptrdiff_t>(off),
                                  cells_.begin() + static_cast<std::ptrdiff_t>(off + stride));
        }
        while (history_.size() > max_history_) history_.pop_front();
    }

    const auto top = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_top_));
    const auto bot = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_bottom_ + 1));
    std::move(top + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot, top);
    std::fill(bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot,
              blank_cell());
    stamp_all();
    touch();
}

// Scroll the region down by n rows (reverse index / SD); blanks the top.
void Screen::scroll_down(std::int32_t n) {
    const std::int32_t region = scroll_bottom_ - scroll_top_ + 1;
    n = std::clamp(n, 0, region);
    if (n == 0) return;
    const std::size_t stride = static_cast<std::size_t>(size_.cols);
    const auto top = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_top_));
    const auto bot = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_bottom_ + 1));
    std::move_backward(top, bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)),
                       bot);
    std::fill(top, top + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)),
              blank_cell());
    stamp_all();
    touch();
}

void Screen::insert_lines(std::int32_t n) {
    // IL only acts inside the scroll region and only when the cursor is within.
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    const std::size_t stride = static_cast<std::size_t>(size_.cols);
    const auto cur = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(cursor_.row.get()));
    const auto bot = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_bottom_ + 1));
    std::move_backward(cur, bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)),
                       bot);
    std::fill(cur, cur + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)),
              blank_cell());
    cursor_.col = Col{0};
    stamp_all();
    touch();
}

void Screen::delete_lines(std::int32_t n) {
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    const std::size_t stride = static_cast<std::size_t>(size_.cols);
    const auto cur = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(cursor_.row.get()));
    const auto bot = cells_.begin() + static_cast<std::ptrdiff_t>(
                                          stride * static_cast<std::size_t>(scroll_bottom_ + 1));
    std::move(cur + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot, cur);
    std::fill(bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot,
              blank_cell());
    cursor_.col = Col{0};
    stamp_all();
    touch();
}

void Screen::insert_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start);
    if (n == 0) return;
    // Shift cells [start, cols-n) right by n; blank the gap.
    for (std::int32_t c = size_.cols - 1; c >= start + n; --c) {
        at(r, Col{c}) = at(r, Col{c - n});
    }
    for (std::int32_t c = start; c < start + n; ++c) at(r, Col{c}) = blank_cell();
    touch();
}

void Screen::delete_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start);
    if (n == 0) return;
    for (std::int32_t c = start; c < size_.cols - n; ++c) at(r, Col{c}) = at(r, Col{c + n});
    for (std::int32_t c = size_.cols - n; c < size_.cols; ++c) at(r, Col{c}) = blank_cell();
    touch();
}

void Screen::erase_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start);
    for (std::int32_t c = start; c < start + n; ++c) at(r, Col{c}) = blank_cell();
    touch();
}

void Screen::cursor_tab(std::int32_t n) {
    for (std::int32_t i = 0; i < n; ++i) tab();
}

void Screen::cursor_back_tab(std::int32_t n) {
    for (std::int32_t i = 0; i < n && cursor_.col.get() > 0; ++i) {
        cursor_.col = Col{prev_tab_stop(cursor_.col.get())};
    }
    touch();
}

void Screen::set_scroll_region(int top, int bottom) {
    // DECSTBM params are 1-based; 0/absent means the extremes.
    const int t = (top <= 0) ? 1 : top;
    const int b = (bottom <= 0) ? size_.rows : bottom;
    if (t < b && b <= size_.rows) {
        scroll_top_ = t - 1;
        scroll_bottom_ = b - 1;
        // DECSTBM homes the cursor.
        cursor_ = Pos{Row{0}, Col{0}};
        touch();
    }
}

void Screen::save_cursor() {
    saved_cursor_ = cursor_;
    saved_pen_ = pen_;
}

void Screen::restore_cursor() {
    cursor_.row = Row{std::clamp(saved_cursor_.row.get(), 0, size_.rows - 1)};
    cursor_.col = Col{std::clamp(saved_cursor_.col.get(), 0, size_.cols - 1)};
    pen_ = saved_pen_;
    wrap_pending_ = false;
    touch();
}

// CSI ? Pm h / l — DEC private mode set/reset.
void Screen::set_private_mode(int mode, bool set) {
    switch (mode) {
    case 1: // DECCKM — application cursor keys.
        app_cursor_keys_ = set;
        break;
    case 7: // DECAWM — autowrap at right margin.
        autowrap_ = set;
        break;
    case 25: // DECTCEM — show/hide cursor.
        cursor_shown_ = set;
        break;
    case 1000: // X11 mouse: report button press/release.
        mouse_mode_ = set ? MouseMode::normal : MouseMode::off;
        break;
    case 1002: // button-event tracking (drag).
        mouse_mode_ = set ? MouseMode::button : MouseMode::off;
        break;
    case 1003: // any-event tracking (motion).
        mouse_mode_ = set ? MouseMode::any : MouseMode::off;
        break;
    case 1006: // SGR extended mouse coordinates.
        mouse_sgr_ = set;
        break;
    case 2004: // bracketed paste.
        bracketed_paste_ = set;
        break;
    case 1047: // alternate screen buffer (no clear-on-enter).
    case 1049: // alternate screen + save/restore cursor (the common one).
    case 47:   // legacy alternate screen.
        if (set) {
            enter_alt_screen();
        } else {
            leave_alt_screen();
        }
        break;
    default:
        break; // unhandled private mode
    }
    touch();
}

void Screen::enter_alt_screen() {
    if (on_alt_) return;
    saved_primary_ = cells_;             // stash the primary buffer
    saved_primary_cursor_ = cursor_;
    std::fill(cells_.begin(), cells_.end(), Cell{}); // alt screen starts blank
    cursor_ = Pos{};
    scroll_offset_ = 0;                  // alt screen has no scrollback
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    on_alt_ = true;
    stamp_all();
    touch();
}

void Screen::leave_alt_screen() {
    if (!on_alt_) return;
    if (saved_primary_.size() == cells_.size()) {
        cells_ = saved_primary_;         // restore the primary buffer
    }
    cursor_ = saved_primary_cursor_;
    saved_primary_.clear();
    on_alt_ = false;
    clamp_cursor();
    stamp_all();
    touch();
}

void Screen::move_cursor_abs(Row r, Col c) {
    wrap_pending_ = false;
    cursor_.row = Row{std::clamp(r.get(), 0, size_.rows - 1)};
    cursor_.col = Col{std::clamp(c.get(), 0, size_.cols - 1)};
    touch();
}

void Screen::erase_in_line(int mode) {
    const Row r = cursor_.row;
    switch (mode) {
    case 0: // cursor to end of line
        for (std::int32_t c = cursor_.col.get(); c < size_.cols; ++c) at(r, Col{c}) = blank_cell();
        break;
    case 1: // start of line to cursor
        for (std::int32_t c = 0; c <= cursor_.col.get() && c < size_.cols; ++c)
            at(r, Col{c}) = blank_cell();
        break;
    case 2: // whole line
        for (std::int32_t c = 0; c < size_.cols; ++c) at(r, Col{c}) = blank_cell();
        break;
    default: break;
    }
    touch();
}

void Screen::erase_in_display(int mode) {
    switch (mode) {
    case 0: // cursor to end of screen
        erase_in_line(0);
        for (std::int32_t rr = cursor_.row.get() + 1; rr < size_.rows; ++rr)
            for (std::int32_t c = 0; c < size_.cols; ++c) at(Row{rr}, Col{c}) = blank_cell();
        break;
    case 1: // start of screen to cursor
        for (std::int32_t rr = 0; rr < cursor_.row.get(); ++rr)
            for (std::int32_t c = 0; c < size_.cols; ++c) at(Row{rr}, Col{c}) = blank_cell();
        erase_in_line(1);
        break;
    case 2: // entire screen
    case 3:
        std::fill(cells_.begin(), cells_.end(), blank_cell());
        stamp_all();
        break;
    default: break;
    }
    touch();
}

void Screen::csi(const vt::CsiDispatch &d) {
    const auto p = d.params;
    switch (d.final) {
    case 'A': move_cursor_abs(cursor_.row - param_or(p, 0, 1), cursor_.col); break; // CUU
    case 'B': move_cursor_abs(cursor_.row + param_or(p, 0, 1), cursor_.col); break; // CUD
    case 'C': move_cursor_abs(cursor_.row, cursor_.col + param_or(p, 0, 1)); break; // CUF
    case 'D': move_cursor_abs(cursor_.row, cursor_.col - param_or(p, 0, 1)); break; // CUB
    case 'E': move_cursor_abs(cursor_.row + param_or(p, 0, 1), Col{0}); break;      // CNL
    case 'F': move_cursor_abs(cursor_.row - param_or(p, 0, 1), Col{0}); break;      // CPL
    case 'G': move_cursor_abs(cursor_.row, Col{param_or(p, 0, 1) - 1}); break;      // CHA
    case 'H':
    case 'f': // CUP (1-based params)
        move_cursor_abs(Row{param_or(p, 0, 1) - 1}, Col{param_or(p, 1, 1) - 1});
        break;
    case 'J': erase_in_display(param_raw(p, 0, 0)); break; // ED
    case 'K': erase_in_line(param_raw(p, 0, 0)); break;    // EL
    case 'L': insert_lines(param_or(p, 0, 1)); break;      // IL
    case 'M': delete_lines(param_or(p, 0, 1)); break;      // DL
    case '@': insert_chars(param_or(p, 0, 1)); break;      // ICH
    case 'P': delete_chars(param_or(p, 0, 1)); break;      // DCH
    case 'X': erase_chars(param_or(p, 0, 1)); break;       // ECH
    case 'S': scroll_up(param_or(p, 0, 1)); break;         // SU
    case 'T': scroll_down(param_or(p, 0, 1)); break;       // SD
    case 'I': cursor_tab(param_or(p, 0, 1)); break;        // CHT
    case 'Z': cursor_back_tab(param_or(p, 0, 1)); break;   // CBT
    case 'g': clear_tab_stop(param_raw(p, 0, 0)); break;   // TBC
    case 'd': move_cursor_abs(Row{param_or(p, 0, 1) - 1}, cursor_.col); break; // VPA
    case 'r': // DECSTBM (set scroll region) — ignore private '?' variants
        if (!d.private_marker) set_scroll_region(param_raw(p, 0, 0), param_raw(p, 1, 0));
        break;
    case 's':
        if (!d.private_marker) save_cursor(); // ANSI.SYS save cursor
        break;
    case 'u':
        if (d.private_marker && d.marker == '?') {
            // CSI ? u — Kitty keyboard protocol: query current flags. We don't
            // implement progressive enhancement, so report flags = 0.
            reply("\x1b[?0u");
        } else if (!d.private_marker) {
            restore_cursor(); // ANSI.SYS / DECRC restore cursor
        }
        // CSI = u (push), CSI < u (pop), CSI > u (set): Kitty flag changes we
        // don't implement — ignore rather than mistaking them for cursor ops.
        break;
    case 'h': // SM / DECSET — set mode(s)
        if (d.private_marker) {
            for (int m : p) set_private_mode(m, true);
        }
        break;
    case 'l': // RM / DECRST — reset mode(s)
        if (d.private_marker) {
            for (int m : p) set_private_mode(m, false);
        }
        break;
    case 'c': // Device Attributes
        if (d.private_marker && d.marker == '>') {
            // DA2 (secondary): report a VT220-ish terminal, version, keyboard.
            reply("\x1b[>1;95;0c");
        } else if (!d.private_marker || d.marker == '?') {
            // DA1 (primary). fish sends this last as a fence and waits 10s for
            // the reply; the response must start with '?' and end with 'c'.
            // \e[?62;...c advertises a VT220 with 132-col, colour, and
            // selective-erase support (features we actually implement).
            reply("\x1b[?62;1;6;22c");
        }
        break;
    case 'n': // Device Status Report
        if (!d.private_marker) {
            const int req = param_raw(p, 0, 0);
            if (req == 5) {
                reply("\x1b[0n"); // terminal OK
            } else if (req == 6) {
                // CPR: cursor position, 1-based row;col.
                std::string r = "\x1b[";
                r += std::to_string(cursor_.row.get() + 1);
                r += ';';
                r += std::to_string(cursor_.col.get() + 1);
                r += 'R';
                reply(r);
            }
        }
        break;
    case 'q': // XTVERSION: CSI > 0 q  -> report name+version as a DCS string.
        if (d.private_marker && d.marker == '>') {
            // DCS > | gvte(0.1) ST
            reply("\x1bP>|gvte(0.1)\x1b\\");
        }
        break;
    case 'm': apply_sgr(p); break;                          // SGR
    default: break; // unhandled — silently ignore for now
    }
}

// XTGETTCAP (DCS + q ...): the app asks for terminfo capabilities by hex-
// encoded name. We answer a small, high-value subset so shells stop probing.
void Screen::dcs(std::string_view prefix, std::string_view data) {
    if (prefix != "+q") {
        return; // only XTGETTCAP is answered; DECRQSS etc. are ignored for now
    }

    auto from_hex = [](std::string_view hex) {
        std::string out;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
        }
        return out;
    };
    auto to_hex = [](std::string_view s) {
        static const char *h = "0123456789ABCDEF";
        std::string out;
        for (char sc : s) {
            const unsigned char c = static_cast<unsigned char>(sc);
            out.push_back(h[c >> 4]);
            out.push_back(h[c & 0xF]);
        }
        return out;
    };

    // The payload is a ';'-separated list of hex-encoded capability names.
    std::size_t start = 0;
    while (start <= data.size()) {
        const std::size_t semi = data.find(';', start);
        std::string_view tok =
            data.substr(start, semi == std::string_view::npos ? std::string_view::npos
                                                              : semi - start);
        if (!tok.empty()) {
            const std::string name = from_hex(tok);
            // Known caps we can honestly answer.
            std::string value;
            bool known = true;
            if (name == "Co" || name == "colors") {
                value = "256"; // number of colours
            } else if (name == "TN" || name == "name") {
                value = "xterm-256color";
            } else if (name == "RGB") {
                value = "8/8/8"; // direct-colour depth
            } else {
                known = false;
            }
            if (known) {
                // DCS 1 + r <name-hex> = <value-hex> ST
                std::string r = "\x1bP1+r";
                r += to_hex(name);
                r += '=';
                r += to_hex(value);
                r += "\x1b\\";
                reply(r);
            } else {
                // DCS 0 + r <name-hex> ST  (unknown/invalid)
                std::string r = "\x1bP0+r";
                r += to_hex(name);
                r += "\x1b\\";
                reply(r);
            }
        }
        if (semi == std::string_view::npos) break;
        start = semi + 1;
    }
}

void Screen::esc(const vt::EscDispatch &d) {
    if (d.intermediates.empty()) {
        switch (d.final) {
        case 'c': // RIS — reset to initial state.
            std::fill(cells_.begin(), cells_.end(), Cell{});
            cursor_ = Pos{};
            pen_ = Pen{};
            wrap_pending_ = false;
            scroll_top_ = 0;
            scroll_bottom_ = size_.rows - 1;
            stamp_all();
            touch();
            return;
        case '7': save_cursor(); return;    // DECSC
        case '8': restore_cursor(); return; // DECRC
        case 'D': line_feed(); return;      // IND (index)
        case 'H': set_tab_stop(); return;   // HTS (set tab stop)
        case '=': app_keypad_ = true; return;  // DECKPAM (application keypad)
        case '>': app_keypad_ = false; return; // DECKPNM (normal keypad)
        case 'M': // RI (reverse index)
            if (cursor_.row.get() == scroll_top_) {
                scroll_down(1);
            } else if (cursor_.row.get() > 0) {
                --cursor_.row;
                touch();
            }
            return;
        case 'E': // NEL (next line)
            carriage_return();
            line_feed();
            return;
        default: break;
        }
    }
    // Charset designations (ESC ( B etc.) are accepted and ignored for now.
}

// --- SGR (Select Graphic Rendition) ---------------------------------------

void Screen::apply_sgr(std::span<const int> params) {
    if (params.empty()) {
        pen_ = Pen{}; // ESC[m == ESC[0m == reset
        return;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const int c = params[i];
        switch (c) {
        case 0: pen_ = Pen{}; break;
        case 1: pen_.attr |= Attr::Bold; break;
        case 2: pen_.attr |= Attr::Faint; break;
        case 3: pen_.attr |= Attr::Italic; break;
        case 4: pen_.attr |= Attr::Underline; break;
        case 5: pen_.attr |= Attr::Blink; break;
        case 6: pen_.attr |= Attr::Blink; break; // rapid blink -> blink
        case 7: pen_.attr |= Attr::Reverse; break;
        case 8: pen_.attr |= Attr::Hidden; break;
        case 9: pen_.attr |= Attr::Strike; break;
        case 21: pen_.attr |= Attr::Underline; break; // double underline -> underline
        case 53: pen_.attr |= Attr::Overline; break;
        case 22: pen_.attr &= ~(Attr::Bold | Attr::Faint); break;
        case 23: pen_.attr &= ~Attr::Italic; break;
        case 24: pen_.attr &= ~Attr::Underline; break;
        case 25: pen_.attr &= ~Attr::Blink; break;
        case 27: pen_.attr &= ~Attr::Reverse; break;
        case 28: pen_.attr &= ~Attr::Hidden; break;
        case 29: pen_.attr &= ~Attr::Strike; break;
        case 55: pen_.attr &= ~Attr::Overline; break;
        case 39: pen_.fg = DefaultColor{}; break;
        case 49: pen_.bg = DefaultColor{}; break;
        default:
            if (c >= 30 && c <= 37) {
                pen_.fg = IndexedColor{static_cast<std::uint8_t>(c - 30)};
            } else if (c >= 40 && c <= 47) {
                pen_.bg = IndexedColor{static_cast<std::uint8_t>(c - 40)};
            } else if (c >= 90 && c <= 97) {
                pen_.fg = IndexedColor{static_cast<std::uint8_t>(c - 90 + 8)};
            } else if (c >= 100 && c <= 107) {
                pen_.bg = IndexedColor{static_cast<std::uint8_t>(c - 100 + 8)};
            } else if (c == 38 || c == 48) {
                // Extended color: 38;5;n (indexed) or 38;2;r;g;b (truecolor).
                Color *target = (c == 38) ? &pen_.fg : &pen_.bg;
                if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
                    *target = IndexedColor{static_cast<std::uint8_t>(params[i + 2])};
                    i += 2;
                } else if (i + 4 < params.size() && params[i + 1] == 2) {
                    *target = TrueColor{Rgb{static_cast<std::uint8_t>(params[i + 2]),
                                            static_cast<std::uint8_t>(params[i + 3]),
                                            static_cast<std::uint8_t>(params[i + 4])}};
                    i += 4;
                }
            }
            break;
        }
    }
}

// --- selection -------------------------------------------------------------

std::int64_t Screen::viewport_to_abs(std::int32_t vrow) const noexcept {
    // The visible window's top is scroll_offset_ rows above the live grid top.
    const std::int64_t live_top = static_cast<std::int64_t>(history_.size());
    return live_top - scroll_offset_ + vrow;
}

const Cell *Screen::cell_at_abs(std::int64_t abs_row, std::int32_t col) const noexcept {
    if (col < 0 || col >= size_.cols) return nullptr;
    const std::int64_t hist = static_cast<std::int64_t>(history_.size());
    if (abs_row < 0) return nullptr;
    if (abs_row < hist) {
        const auto &line = history_[static_cast<std::size_t>(abs_row)];
        return &line[static_cast<std::size_t>(col)];
    }
    const std::int64_t live = abs_row - hist;
    if (live >= size_.rows) return nullptr;
    return &cells_[index(Row{static_cast<std::int32_t>(live)}, Col{col})];
}

void Screen::selection_begin(AbsPos p, SelectMode mode) {
    sel_mode_ = mode;
    sel_anchor_ = p;
    sel_active_ = p;
    touch();
}

void Screen::selection_extend(AbsPos p) {
    if (sel_mode_ == SelectMode::none) return;
    sel_active_ = p;
    touch();
}

namespace {
// A codepoint that counts as part of a "word" for double-click selection:
// alphanumerics plus a few path/URL-ish punctuation characters.
bool is_word_cp(char32_t cp) {
    if (cp == U' ' || cp == 0) return false;
    if ((cp >= U'0' && cp <= U'9') || (cp >= U'A' && cp <= U'Z') ||
        (cp >= U'a' && cp <= U'z') || cp >= 0x80) {
        return true;
    }
    switch (cp) {
    case U'_': case U'-': case U'.': case U'/': case U'~': case U':':
    case U'@': case U'+': case U'=': case U'%': case U'#':
        return true;
    default:
        return false;
    }
}
} // namespace

void Screen::selection_word(AbsPos p) {
    auto word_at = [&](std::int32_t col) {
        const Cell *c = cell_at_abs(p.row, col);
        return c && is_word_cp(c->cp);
    };
    if (!word_at(p.col)) {
        // Not on a word: fall back to a single-cell character selection.
        selection_begin(p, SelectMode::character);
        return;
    }
    std::int32_t lo = p.col, hi = p.col;
    while (lo > 0 && word_at(lo - 1)) --lo;
    while (hi < size_.cols - 1 && word_at(hi + 1)) ++hi;
    sel_mode_ = SelectMode::character;
    sel_anchor_ = AbsPos{p.row, lo};
    sel_active_ = AbsPos{p.row, hi};
    touch();
}

void Screen::selection_line(AbsPos p) {
    sel_mode_ = SelectMode::line;
    sel_anchor_ = AbsPos{p.row, 0};
    sel_active_ = AbsPos{p.row, size_.cols - 1};
    touch();
}

void Screen::selection_clear() {
    if (sel_mode_ != SelectMode::none) {
        sel_mode_ = SelectMode::none;
        touch();
    }
}

std::pair<Screen::AbsPos, Screen::AbsPos> Screen::selection_span() const noexcept {
    AbsPos a = sel_anchor_, b = sel_active_;
    if (b < a) std::swap(a, b);
    return {a, b};
}

bool Screen::is_selected(std::int64_t abs_row, std::int32_t col) const noexcept {
    if (sel_mode_ == SelectMode::none) return false;
    const auto [a, b] = selection_span();

    if (sel_mode_ == SelectMode::line) {
        return abs_row >= a.row && abs_row <= b.row;
    }
    if (sel_mode_ == SelectMode::block) {
        const std::int32_t lo = std::min(a.col, b.col);
        const std::int32_t hi = std::max(a.col, b.col);
        return abs_row >= a.row && abs_row <= b.row && col >= lo && col <= hi;
    }
    // character mode: a contiguous run from a to b.
    if (abs_row < a.row || abs_row > b.row) return false;
    const std::int32_t start = (abs_row == a.row) ? a.col : 0;
    const std::int32_t end = (abs_row == b.row) ? b.col : size_.cols - 1;
    return col >= start && col <= end;
}

std::string Screen::selected_text() const {
    if (sel_mode_ == SelectMode::none) return {};
    const auto [a, b] = selection_span();
    std::string out;

    for (std::int64_t r = a.row; r <= b.row; ++r) {
        std::int32_t start = 0, end = size_.cols - 1;
        if (sel_mode_ == SelectMode::character) {
            if (r == a.row) start = a.col;
            if (r == b.row) end = b.col;
        } else if (sel_mode_ == SelectMode::block) {
            start = std::min(a.col, b.col);
            end = std::max(a.col, b.col);
        }

        // Collect the row, trimming trailing blanks for tidy copies.
        std::string line;
        for (std::int32_t c = start; c <= end && c < size_.cols; ++c) {
            const Cell *cell = cell_at_abs(r, c);
            const char32_t cp = cell ? cell->cp : U' ';
            // Encode the codepoint as UTF-8.
            if (cp < 0x80) {
                line.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                line.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                line.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                line.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                line.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                line.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                line.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                line.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                line.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                line.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        // Trim trailing spaces on each line.
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (r != b.row) out.push_back('\n');
    }
    return out;
}

} // namespace gvte::term
