// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/term/screen.hpp"

#include <algorithm>
#include <cassert>

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
}

std::size_t Screen::index(Row r, Col c) const noexcept {
    return static_cast<std::size_t>(r.get()) * static_cast<std::size_t>(size_.cols) +
           static_cast<std::size_t>(c.get());
}

Cell &Screen::at(Row r, Col c) {
    assert(r.get() >= 0 && r.get() < size_.rows);
    assert(c.get() >= 0 && c.get() < size_.cols);
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
    clamp_cursor();
    touch();
}

void Screen::clamp_cursor() noexcept {
    cursor_.row = Row{std::clamp(cursor_.row.get(), 0, size_.rows - 1)};
    cursor_.col = Col{std::clamp(cursor_.col.get(), 0, size_.cols - 1)};
}

// --- the reduction ---------------------------------------------------------

void Screen::apply(const vt::Action &action) {
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
                // OSC (title etc.) — not reflected in the grid; ignored here.
            }
        },
        action);
}

void Screen::put(char32_t cp) {
    if (wrap_pending_) {
        cursor_.col = Col{0};
        line_feed();
        wrap_pending_ = false;
    }
    at(cursor_.row, cursor_.col) = Cell{cp, pen_};
    if (cursor_.col.get() + 1 >= size_.cols) {
        wrap_pending_ = true; // defer the wrap until the next glyph (DEC semantics)
    } else {
        ++cursor_.col;
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
    case 0x07: break;                       // BEL — audible; no grid effect
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
    // Advance to the next multiple-of-8 column, clamped to the last column.
    const std::int32_t next = ((cursor_.col.get() / 8) + 1) * 8;
    cursor_.col = Col{std::min(next, size_.cols - 1)};
    touch();
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

    if (full_screen) {
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
    std::fill(bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot, Cell{});
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
    std::fill(top, top + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), Cell{});
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
    std::fill(cur, cur + static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), Cell{});
    cursor_.col = Col{0};
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
    std::fill(bot - static_cast<std::ptrdiff_t>(stride * static_cast<std::size_t>(n)), bot, Cell{});
    cursor_.col = Col{0};
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
    for (std::int32_t c = start; c < start + n; ++c) at(r, Col{c}) = Cell{};
    touch();
}

void Screen::delete_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start);
    if (n == 0) return;
    for (std::int32_t c = start; c < size_.cols - n; ++c) at(r, Col{c}) = at(r, Col{c + n});
    for (std::int32_t c = size_.cols - n; c < size_.cols; ++c) at(r, Col{c}) = Cell{};
    touch();
}

void Screen::erase_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start);
    for (std::int32_t c = start; c < start + n; ++c) at(r, Col{c}) = Cell{};
    touch();
}

void Screen::cursor_tab(std::int32_t n) {
    for (std::int32_t i = 0; i < n; ++i) tab();
}

void Screen::cursor_back_tab(std::int32_t n) {
    for (std::int32_t i = 0; i < n && cursor_.col.get() > 0; ++i) {
        const std::int32_t prev = ((cursor_.col.get() - 1) / 8) * 8;
        cursor_.col = Col{std::max(prev, 0)};
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
        for (std::int32_t c = cursor_.col.get(); c < size_.cols; ++c) at(r, Col{c}) = Cell{};
        break;
    case 1: // start of line to cursor
        for (std::int32_t c = 0; c <= cursor_.col.get() && c < size_.cols; ++c) at(r, Col{c}) = Cell{};
        break;
    case 2: // whole line
        for (std::int32_t c = 0; c < size_.cols; ++c) at(r, Col{c}) = Cell{};
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
            for (std::int32_t c = 0; c < size_.cols; ++c) at(Row{rr}, Col{c}) = Cell{};
        break;
    case 1: // start of screen to cursor
        for (std::int32_t rr = 0; rr < cursor_.row.get(); ++rr)
            for (std::int32_t c = 0; c < size_.cols; ++c) at(Row{rr}, Col{c}) = Cell{};
        erase_in_line(1);
        break;
    case 2: // entire screen
    case 3:
        std::fill(cells_.begin(), cells_.end(), Cell{});
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
    case 'd': move_cursor_abs(Row{param_or(p, 0, 1) - 1}, cursor_.col); break; // VPA
    case 'r': // DECSTBM (set scroll region) — ignore private '?' variants
        if (!d.private_marker) set_scroll_region(param_raw(p, 0, 0), param_raw(p, 1, 0));
        break;
    case 's': save_cursor(); break;    // ANSI.SYS save cursor
    case 'u': restore_cursor(); break; // ANSI.SYS restore cursor
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
    case 'm': apply_sgr(p); break;                          // SGR
    default: break; // unhandled — silently ignore for now
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
            touch();
            return;
        case '7': save_cursor(); return;    // DECSC
        case '8': restore_cursor(); return; // DECRC
        case 'D': line_feed(); return;      // IND (index)
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

} // namespace gvte::term
