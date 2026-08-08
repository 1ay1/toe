// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/term/screen.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <clocale>
#include <cwchar>

namespace toe::term {

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
    live_wrapped_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), false);
    line_attr_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), LineAttr::normal);
    reset_row_map();
    row_epoch_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), 0);
    tab_stops_.assign(static_cast<std::size_t>(std::max(size_.cols, 1)), false);
    for (std::int32_t c = 0; c < size_.cols; c += 8) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
}

// Reset the logical->physical row map to the identity (row r lives at physical
// row r). Called after any op that repacks cells_ into logical order.
void Screen::reset_row_map() {
    row_of_.resize(static_cast<std::size_t>(std::max(size_.rows, 0)));
    for (std::int32_t r = 0; r < size_.rows; ++r)
        row_of_[static_cast<std::size_t>(r)] = static_cast<std::uint32_t>(r);
}

std::size_t Screen::index(Row r, Col c) const noexcept {
    return static_cast<std::size_t>(row_of_[static_cast<std::size_t>(r.get())]) *
               static_cast<std::size_t>(size_.cols) +
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

    // On the alt screen (vim/tmux/htop own the whole grid) reflow is wrong: the
    // app redraws on SIGWINCH. Reflow only the primary screen, and only when the
    // width actually changes (a height-only change needs no rewrap).
    const Extent old_size = size_;
    const bool do_reflow = !on_alt_ && size.cols != size_.cols && size_.cols > 0 &&
                           size_.rows > 0;

    if (do_reflow) {
        reflow(old_size, size);
    } else {
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
        // `next` was written in logical row order, so the map is now the identity.
        reset_row_map();
        live_wrapped_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), false);
    }

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
    line_attr_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), LineAttr::normal);
    stamp_all();
    touch();
}

// Rewrap all content when the column count changes. Flattens history + the live
// grid into logical lines (joining soft-wrapped runs), then re-lays them out at
// the new width, keeping the last `rows` lines live and the rest in scrollback.
// The cursor's logical position is tracked across the transform.
void Screen::reflow(Extent old_size, Extent new_size) {
    struct LogLine {
        std::vector<Cell> cells;
        bool from_live = false; // originated in the live grid (vs history)
    };

    // Trim a physical row to its last non-blank cell (a soft-wrapped row keeps
    // its full width; a hard line keeps only up to its content).
    const auto trimmed = [&](std::span<const Cell> row, bool wrapped) {
        std::vector<Cell> v(row.begin(), row.end());
        if (!wrapped) {
            while (!v.empty() && v.back().blank()) v.pop_back();
        }
        return v;
    };

    // 1. Gather logical lines from history, joining soft-wrapped runs.
    std::vector<LogLine> logical;
    {
        std::vector<Cell> cur;
        bool have = false;
        for (std::size_t i = 0; i < history_.size(); ++i) {
            const bool wrapped = i < hist_wrapped_.size() && hist_wrapped_[i];
            auto piece = trimmed(history_[i], wrapped);
            cur.insert(cur.end(), piece.begin(), piece.end());
            have = true;
            if (!wrapped) { logical.push_back({std::move(cur), false}); cur.clear(); have = false; }
        }
        if (have) logical.push_back({std::move(cur), false});
    }

    // 2. Append the live grid's logical rows, tracking the cursor's flat offset.
    const std::int32_t cur_row = cursor_.row.get();
    std::int64_t cursor_flat = -1; // index into the live logical line + column
    std::size_t cursor_line = 0;
    std::int32_t cursor_col_in_line = 0;
    {
        std::vector<Cell> cur;
        std::int32_t line_start_row = 0;
        for (std::int32_t r = 0; r < old_size.rows; ++r) {
            const bool wrapped = wrapped_at(r);
            std::vector<Cell> piece;
            for (std::int32_t c = 0; c < old_size.cols; ++c) piece.push_back(at(Row{r}, Col{c}));
            if (!wrapped) while (!piece.empty() && piece.back().blank()) piece.pop_back();
            if (r == cur_row) {
                cursor_line = logical.size();
                cursor_col_in_line = static_cast<std::int32_t>(cur.size()) + cursor_.col.get();
            }
            cur.insert(cur.end(), piece.begin(), piece.end());
            if (!wrapped) {
                logical.push_back({std::move(cur), true});
                cur.clear();
                line_start_row = r + 1;
            }
        }
        (void)line_start_row;
        (void)cursor_flat;
        if (!cur.empty()) logical.push_back({std::move(cur), true});
    }

    // 3. Re-wrap each logical line at the new width into physical rows.
    const std::int32_t ncols = new_size.cols;
    std::vector<std::vector<Cell>> rows;
    std::vector<bool> rows_wrapped;
    std::int32_t new_cursor_row = 0, new_cursor_col = 0;
    bool cursor_placed = false;
    for (std::size_t li = 0; li < logical.size(); ++li) {
        const auto &ln = logical[li];
        std::size_t off = 0;
        const std::size_t n = ln.cells.size();
        // do/while emits at least one row, so an empty logical line yields one
        // blank physical row (no separate empty-line branch, which double-counts).
        do {
            const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(ncols), n - off);
            std::vector<Cell> row(static_cast<std::size_t>(ncols));
            for (std::size_t c = 0; c < take; ++c) row[c] = ln.cells[off + c];
            const bool more = (off + take) < n;
            // Place the cursor when we reach its logical line + column.
            if (ln.from_live && li == cursor_line && !cursor_placed) {
                const std::int32_t col = cursor_col_in_line;
                if (col >= static_cast<std::int32_t>(off) &&
                    (col < static_cast<std::int32_t>(off + static_cast<std::size_t>(ncols)) || !more)) {
                    new_cursor_row = static_cast<std::int32_t>(rows.size());
                    new_cursor_col = std::clamp(col - static_cast<std::int32_t>(off), 0, ncols - 1);
                    cursor_placed = true;
                }
            }
            rows.push_back(std::move(row));
            rows_wrapped.push_back(more);
            off += take;
        } while (off < n);
    }
    if (!cursor_placed && !rows.empty()) {
        new_cursor_row = static_cast<std::int32_t>(rows.size()) - 1;
        new_cursor_col = 0;
    }

    // 4. Split into scrollback (all but the last `rows` lines) + live grid.
    const std::int32_t nrows = new_size.rows;
    const std::int32_t total = static_cast<std::int32_t>(rows.size());
    const std::int32_t live_count = std::min(total, nrows);
    const std::int32_t live_first = total - live_count;

    history_.clear();
    hist_wrapped_.clear();
    for (std::int32_t i = 0; i < live_first; ++i) {
        history_.push_back(std::move(rows[static_cast<std::size_t>(i)]));
        hist_wrapped_.push_back(rows_wrapped[static_cast<std::size_t>(i)]);
    }
    while (history_.size() > max_history_) { history_.pop_front(); hist_wrapped_.pop_front(); }

    cells_.assign(static_cast<std::size_t>(new_size.area()), Cell{});
    live_wrapped_.assign(static_cast<std::size_t>(std::max(nrows, 0)), false);
    for (std::int32_t r = 0; r < live_count; ++r) {
        const auto &src = rows[static_cast<std::size_t>(live_first + r)];
        for (std::int32_t c = 0; c < ncols; ++c)
            cells_[static_cast<std::size_t>(r) * static_cast<std::size_t>(ncols) +
                   static_cast<std::size_t>(c)] = src[static_cast<std::size_t>(c)];
        live_wrapped_[static_cast<std::size_t>(r)] = rows_wrapped[static_cast<std::size_t>(live_first + r)];
    }

    size_ = new_size;
    reset_row_map();
    scroll_offset_ = 0;
    // Cursor: map into the live region (clamp if it scrolled into history).
    cursor_.row = Row{std::clamp(new_cursor_row - live_first, 0, std::max(nrows - 1, 0))};
    cursor_.col = Col{std::clamp(new_cursor_col, 0, std::max(ncols - 1, 0))};
    wrap_pending_ = false;
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
                // Sixel is DCS <numeric params> q — a bare 'q' final preceded
                // only by digits/';'. XTGETTCAP ('+q') and DECRQSS ('$q') also
                // end in 'q' but carry an intermediate ('+'/'$'), so route by
                // checking the char before the final.
                const bool is_sixel =
                    !a.prefix.empty() && a.prefix.back() == 'q' &&
                    (a.prefix.size() == 1 ||
                     (a.prefix[a.prefix.size() - 2] >= '0' && a.prefix[a.prefix.size() - 2] <= '9') ||
                     a.prefix[a.prefix.size() - 2] == ';');
                if (is_sixel) {
                    const std::int64_t abs = viewport_to_abs(cursor_.row.get());
                    if (graphics_.handle_sixel(a.prefix, a.data, abs, cursor_.col.get(), cell_w_,
                                               cell_h_)) {
                        touch();
                    }
                } else {
                    dcs(a.prefix, a.data);
                }
            } else if constexpr (std::is_same_v<T, vt::ApcDispatch>) {
                // Kitty graphics: anchor a display at the cursor's absolute row.
                const std::int64_t abs = viewport_to_abs(cursor_.row.get());
                std::string response;
                if (graphics_.handle_apc(a.data, abs, cursor_.col.get(), cell_w_, cell_h_,
                                         &response)) {
                    touch();
                }
                if (!response.empty()) reply(std::move(response));
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
    cp = map_charset(cp); // VT100 line-drawing charset, if active
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
            at(cursor_.row, cursor_.col) = Cell{cp, pen_, 1, cur_link_};
            touch();
            return;
        }
    }

    at(cursor_.row, cursor_.col) = Cell{cp, pen_, static_cast<std::uint8_t>(w), cur_link_};
    last_char_ = cp; // remember for REP (CSI Ps b)
    if (w == 2) {
        // The second half is a spacer the renderer skips.
        at(cursor_.row, cursor_.col + 1) = Cell{U' ', pen_, 0, cur_link_};
    }

    const int advance = w;
    if (cursor_.col.get() + advance >= size_.cols) {
        if (autowrap_) {
            wrap_pending_ = true;
            // This logical row's content continues onto the next (soft wrap):
            // record it so a later resize can rejoin and rewrap the line.
            set_wrapped(cursor_.row.get(), true);
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
    case 0x0E: charset_use_g1_ = true; break;  // SO — invoke G1
    case 0x0F: charset_use_g1_ = false; break; // SI — invoke G0
    case 0x07: // BEL
        if (pending_) pending_->emplace_back(RingBell{});
        break;
    default: break;
    }
}

// DEC Special Graphics: ASCII 0x5F..0x7E map to line-drawing / block glyphs.
// This is the classic VT100 alternate charset every ncurses TUI uses for
// borders when it isn't emitting UTF-8. Indexed by (cp - 0x5F).
char32_t Screen::map_charset(char32_t cp) const noexcept {
    const Charset cs = charset_use_g1_ ? charset_g1_ : charset_g0_;
    if (cs != Charset::DecGraphics || cp < 0x5F || cp > 0x7E) return cp;
    static constexpr char32_t kDec[] = {
        // 0x5F ' '        0x60 ◆        0x61 ▒        0x62 ␉(HT)   0x63 ␌(FF)
        0x00A0,          0x25C6,        0x2592,        0x2409,        0x240C,
        // 0x64 ␍(CR)   0x65 ␊(LF)   0x66 °        0x67 ±        0x68 ␤(NL)
        0x240D,          0x240A,        0x00B0,        0x00B1,        0x2424,
        // 0x69 ␋(VT)   0x6A ┘        0x6B ┐        0x6C ┌        0x6D └
        0x240B,          0x2518,        0x2510,        0x250C,        0x2514,
        // 0x6E ┼        0x6F ⎺        0x70 ⎻        0x71 ─        0x72 ⎼
        0x253C,          0x23BA,        0x23BB,        0x2500,        0x23BC,
        // 0x73 ⎽        0x74 ├        0x75 ┤        0x76 ┴        0x77 ┬
        0x23BD,          0x251C,        0x2524,        0x2534,        0x252C,
        // 0x78 │        0x79 ≤        0x7A ≥        0x7B π        0x7C ≠
        0x2502,          0x2264,        0x2265,        0x03C0,        0x2260,
        // 0x7D £        0x7E ·
        0x00A3,          0x00B7,
    };
    return kDec[cp - 0x5F];
}

// OSC 8 open/close. `params` is the (possibly empty) id= section, `uri` the
// target. An empty uri closes the current link. We intern each URI into links_
// and stamp cur_link_ onto subsequent put()s. To keep hovering a whole link
// working, a repeated open with the same key/uri reuses the same id.
void Screen::set_hyperlink(std::string_view params, std::string_view uri) {
    if (uri.empty()) {
        cur_link_ = 0;
        cur_link_key_.clear();
        return;
    }
    // Reuse the current id if the same link is re-opened (id= param or exact
    // URI match), so a link split across writes stays one clickable region.
    if (cur_link_ != 0 &&
        ((!params.empty() && params == cur_link_key_) ||
         (cur_link_ - 1u < links_.size() && links_[cur_link_ - 1u] == uri))) {
        return;
    }
    if (links_.size() >= 0xFFFE) {
        links_.clear(); // pathological: reset the table rather than overflow u16
    }
    links_.emplace_back(uri);
    cur_link_ = static_cast<std::uint16_t>(links_.size()); // id = index+1
    cur_link_key_.assign(params);
}

std::string_view Screen::link_at(std::int32_t vrow, std::int32_t col) const noexcept {
    if (vrow < 0 || vrow >= size_.rows || col < 0 || col >= size_.cols) return {};
    const auto cells = row(Row{vrow});
    const std::uint16_t id = cells[static_cast<std::size_t>(col)].link;
    if (id == 0 || id - 1u >= links_.size()) return {};
    return links_[id - 1u];
}

bool Screen::set_hover(std::int32_t vrow, std::int32_t col) noexcept {
    std::uint16_t id = 0;
    if (vrow >= 0 && vrow < size_.rows && col >= 0 && col < size_.cols) {
        id = row(Row{vrow})[static_cast<std::size_t>(col)].link;
    }
    if (id == hover_link_) return false;
    hover_link_ = id;
    touch(); // the hovered link's underline appears/moves — needs a redraw
    return true;
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
// Scroll the region up by n rows (SU / index at bottom). Rows that leave the
// TOP of a full-screen scroll go into the scrollback (primary buffer only).
void Screen::scroll_up(std::int32_t n) {
    const std::int32_t region = scroll_bottom_ - scroll_top_ + 1;
    n = std::clamp(n, 0, region);
    if (n == 0) return;
    const bool full_screen = (scroll_top_ == 0 && scroll_bottom_ == size_.rows - 1);

    // Lines that scroll off the top of a FULL-screen scroll go into the
    // scrollback — but ONLY on the primary buffer. The alternate screen (htop,
    // vim, less, …) has no scrollback: it repaints by scrolling, so pushing its
    // lines to history would flood the scrollback with the app's frames.
    if (full_screen && !on_alt_) {
        const std::size_t cols = static_cast<std::size_t>(size_.cols);
        for (std::int32_t i = 0; i < n; ++i) {
            const std::size_t base = index(Row{i}, Col{0});
            const auto begin = cells_.begin() + static_cast<std::ptrdiff_t>(base);
            const auto end = begin + static_cast<std::ptrdiff_t>(cols);
            // The evicted row's soft-wrap flag follows it into scrollback.
            const bool w = wrapped_at(i);
            if (history_.size() >= max_history_) {
                std::vector<Cell> recycled = std::move(history_.front());
                history_.pop_front();
                if (!hist_wrapped_.empty()) hist_wrapped_.pop_front();
                recycled.assign(begin, end);
                history_.push_back(std::move(recycled));
            } else {
                history_.emplace_back(begin, end);
            }
            hist_wrapped_.push_back(w);
        }
    }

    // Rotate the row map: the top n logical rows move to the bottom of the
    // region (their physical storage is reused for the freshly-blanked lines).
    // This is an O(region) index shuffle rather than an O(region*cols) memmove.
    auto first = row_of_.begin() + scroll_top_;
    auto last = row_of_.begin() + scroll_bottom_ + 1;
    std::rotate(first, first + n, last);

    // Shift the live soft-wrap flags up by n within the region; blank the last n.
    if (scroll_top_ == 0 && scroll_bottom_ == size_.rows - 1 &&
        static_cast<std::int32_t>(live_wrapped_.size()) == size_.rows) {
        for (std::int32_t r = 0; r + n < size_.rows; ++r)
            live_wrapped_[static_cast<std::size_t>(r)] =
                live_wrapped_[static_cast<std::size_t>(r + n)];
        for (std::int32_t r = std::max(0, size_.rows - n); r < size_.rows; ++r)
            live_wrapped_[static_cast<std::size_t>(r)] = false;
    }
    // Line attributes follow their rows up the screen (blank the new bottom n).
    if (scroll_top_ == 0 && scroll_bottom_ == size_.rows - 1 &&
        static_cast<std::int32_t>(line_attr_.size()) == size_.rows) {
        for (std::int32_t r = 0; r + n < size_.rows; ++r)
            line_attr_[static_cast<std::size_t>(r)] = line_attr_[static_cast<std::size_t>(r + n)];
        for (std::int32_t r = std::max(0, size_.rows - n); r < size_.rows; ++r)
            line_attr_[static_cast<std::size_t>(r)] = LineAttr::normal;
    }
    for (std::int32_t r = scroll_bottom_ - n + 1; r <= scroll_bottom_; ++r) {
        const std::size_t base = index(Row{r}, Col{0});
        std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(base),
                  cells_.begin() +
                      static_cast<std::ptrdiff_t>(base + static_cast<std::size_t>(size_.cols)),
                  blank_cell());
    }
    stamp_all();
    touch();
}

// Scroll the region down by n rows (reverse index / SD); blanks the top.
void Screen::scroll_down(std::int32_t n) {
    const std::int32_t region = scroll_bottom_ - scroll_top_ + 1;
    n = std::clamp(n, 0, region);
    if (n == 0) return;
    // Rotate the row map the other way: bottom n rows wrap to the top.
    auto first = row_of_.begin() + scroll_top_;
    auto last = row_of_.begin() + scroll_bottom_ + 1;
    std::rotate(first, last - n, last);
    for (std::int32_t r = scroll_top_; r < scroll_top_ + n; ++r) {
        const std::size_t base = index(Row{r}, Col{0});
        std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(base),
                  cells_.begin() +
                      static_cast<std::ptrdiff_t>(base + static_cast<std::size_t>(size_.cols)),
                  blank_cell());
    }
    stamp_all();
    touch();
}

void Screen::insert_lines(std::int32_t n) {
    // IL only acts inside the scroll region and only when the cursor is within.
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    // Rotate the row map so rows [cursor, bottom-n] shift DOWN by n; the bottom
    // n rows fall off and are reused for the freshly-blanked inserted lines.
    auto first = row_of_.begin() + cursor_.row.get();
    auto last = row_of_.begin() + scroll_bottom_ + 1;
    std::rotate(first, last - n, last);
    for (std::int32_t r = cursor_.row.get(); r < cursor_.row.get() + n; ++r) {
        const std::size_t base = index(Row{r}, Col{0});
        std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(base),
                  cells_.begin() +
                      static_cast<std::ptrdiff_t>(base + static_cast<std::size_t>(size_.cols)),
                  blank_cell());
    }
    cursor_.col = Col{0};
    stamp_all();
    touch();
}

void Screen::delete_lines(std::int32_t n) {
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    // Rotate so rows [cursor+n, bottom] shift UP by n; the vacated bottom n
    // rows are reused for the blanks.
    auto first = row_of_.begin() + cursor_.row.get();
    auto last = row_of_.begin() + scroll_bottom_ + 1;
    std::rotate(first, first + n, last);
    for (std::int32_t r = scroll_bottom_ - n + 1; r <= scroll_bottom_; ++r) {
        const std::size_t base = index(Row{r}, Col{0});
        std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(base),
                  cells_.begin() +
                      static_cast<std::ptrdiff_t>(base + static_cast<std::size_t>(size_.cols)),
                  blank_cell());
    }
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
    case 2026: // synchronized output: batch a frame, present atomically.
        if (set) {
            if (!sync_output_) sync_frozen_gen_ = generation_; // freeze at the
            sync_output_ = true;                                // start of the batch
        } else {
            sync_output_ = false; // unfreeze -> generation() jumps, host draws once
        }
        break;
    case 1004: // focus reporting (CSI I / CSI O on focus in/out).
        focus_events_ = set;
        break;
    case 12: // cursor blink (att610). Accepted; the host drives blink timing.
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

// DECRQM: report whether a mode is set. Reply is CSI [?] Ps ; Pv $ y, where Pv
// is 0 (unrecognized), 1 (set), 2 (reset), 3 (permanently set), 4 (perm reset).
void Screen::report_mode(int mode, bool priv) {
    enum : int { kUnknown = 0, kSet = 1, kReset = 2 };
    int pv = kUnknown;

    const auto b = [](bool on) { return on ? kSet : kReset; };

    if (priv) {
        switch (mode) {
        case 1:    pv = b(app_cursor_keys_); break;          // DECCKM
        case 7:    pv = b(autowrap_); break;                 // DECAWM
        case 25:   pv = b(cursor_shown_); break;            // DECTCEM
        case 1000: pv = b(mouse_mode_ == MouseMode::normal); break;
        case 1002: pv = b(mouse_mode_ == MouseMode::button); break;
        case 1003: pv = b(mouse_mode_ == MouseMode::any); break;
        case 1006: pv = b(mouse_sgr_); break;
        case 1049: pv = b(on_alt_); break;                  // alt screen
        case 2004: pv = b(bracketed_paste_); break;
        default:   pv = kUnknown; break;
        }
    } else {
        // ANSI modes: none of IRM/LNM are modelled, so they read unrecognized.
        pv = kUnknown;
    }

    std::string r = "\x1b[";
    if (priv) r += '?';
    r += std::to_string(mode);
    r += ';';
    r += std::to_string(pv);
    r += "$y";
    reply(r);
}

// Kitty keyboard protocol flag management. The private marker selects the op:
//   CSI ? u        query   -> reply CSI ? <flags> u
//   CSI > flags u  push    (a new stack level with `flags`)
//   CSI < n u      pop     n levels (default 1)
//   CSI = flags ; mode u   set current level's flags (mode 1=set,2=or,3=and-not)
void Screen::kitty_keyboard(const vt::CsiDispatch &d) {
    const auto p = d.params;
    switch (d.marker) {
    case '?': { // query the active flags
        std::string r = "\x1b[?";
        r += std::to_string(static_cast<int>(kitty_stack_.back()));
        r += 'u';
        reply(r);
        break;
    }
    case '>': { // push a new level
        const auto flags = static_cast<std::uint8_t>(param_raw(p, 0, 0) & 0x1F);
        // Bound the stack so a hostile stream can't grow it without limit.
        if (kitty_stack_.size() < 256) kitty_stack_.push_back(flags);
        else kitty_stack_.back() = flags;
        break;
    }
    case '<': { // pop n levels (but never the base level)
        int n = param_or(p, 0, 1);
        while (n-- > 0 && kitty_stack_.size() > 1) kitty_stack_.pop_back();
        break;
    }
    case '=': { // set the active level's flags per mode
        const auto flags = static_cast<std::uint8_t>(param_raw(p, 0, 0) & 0x1F);
        const int mode = param_or(p, 1, 1);
        std::uint8_t &cur = kitty_stack_.back();
        switch (mode) {
        case 1: cur = flags; break;                    // set
        case 2: cur = static_cast<std::uint8_t>(cur | flags); break;   // or
        case 3: cur = static_cast<std::uint8_t>(cur & ~flags); break;  // and-not
        default: cur = flags; break;
        }
        break;
    }
    default: break;
    }
}

// DECSTR (CSI ! p): soft reset. Restores the ANSI/DEC state a well-behaved app
// expects at startup WITHOUT clearing the screen (that's RIS's job): SGR pen,
// cursor modes, scroll region, cursor shape, saved cursor, and kitty flags.
void Screen::soft_reset() {
    pen_ = Pen{};
    saved_pen_ = Pen{};
    cursor_shown_ = true;
    cursor_style_ = {};
    autowrap_ = true;
    app_cursor_keys_ = false;
    app_keypad_ = false;
    charset_g0_ = charset_g1_ = Charset::Ascii;
    charset_use_g1_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    saved_cursor_ = Pos{};
    kitty_stack_.assign(1, 0);
    wrap_pending_ = false;
    touch();
}

// DEC rectangular area ops. Coordinates are 1-based inclusive; a 0 or absent
// value means the corresponding screen edge.
void Screen::fill_rect(int top, int left, int bottom, int right, char32_t cp) {
    const int t = std::clamp((top ? top : 1) - 1, 0, size_.rows - 1);
    const int l = std::clamp((left ? left : 1) - 1, 0, size_.cols - 1);
    const int b = std::clamp((bottom ? bottom : size_.rows) - 1, 0, size_.rows - 1);
    const int r = std::clamp((right ? right : size_.cols) - 1, 0, size_.cols - 1);
    if (t > b || l > r) return;
    const std::uint8_t w = static_cast<std::uint8_t>(char_width(cp) == 0 ? 1 : char_width(cp));
    for (int row = t; row <= b; ++row)
        for (int col = l; col <= r; ++col)
            at(Row{row}, Col{col}) = Cell{cp, pen_, w, 0};
    touch();
}

// DECCARA / DECRARA: apply (or, for reverse=true, toggle) a set of SGR
// character attributes over a rectangle, leaving the glyphs untouched. Only the
// on/off boolean attributes are meaningful here (colour SGRs are ignored, per
// the DEC spec's "selected character attributes").
void Screen::change_rect_attrs(int top, int left, int bottom, int right,
                               std::span<const int> attrs, bool reverse) {
    const int t = std::clamp((top ? top : 1) - 1, 0, size_.rows - 1);
    const int l = std::clamp((left ? left : 1) - 1, 0, size_.cols - 1);
    const int b = std::clamp((bottom ? bottom : size_.rows) - 1, 0, size_.rows - 1);
    const int r = std::clamp((right ? right : size_.cols) - 1, 0, size_.cols - 1);
    if (t > b || l > r) return;

    // Map an SGR code to its Attr bit (0 = not an attribute we toggle).
    const auto bit = [](int code) -> Attr {
        switch (code) {
        case 1: return Attr::Bold;
        case 4: return Attr::Underline;
        case 5: return Attr::Blink;
        case 7: return Attr::Reverse;
        case 8: return Attr::Hidden;
        case 9: return Attr::Strike;
        default: return Attr::None;
        }
    };
    Attr mask = Attr::None;
    if (attrs.empty()) {
        // No params: DECCARA clears all; DECRARA toggles all listed attrs.
        mask = Attr::Bold | Attr::Underline | Attr::Blink | Attr::Reverse |
               Attr::Hidden | Attr::Strike;
    } else {
        for (int code : attrs) {
            if (code == 0) { mask = Attr::Bold | Attr::Underline | Attr::Blink |
                                    Attr::Reverse | Attr::Hidden | Attr::Strike; }
            else mask |= bit(code);
        }
    }

    for (int row = t; row <= b; ++row) {
        for (int col = l; col <= r; ++col) {
            Cell &cell = at(Row{row}, Col{col});
            if (reverse) cell.pen.attr ^= mask;   // DECRARA toggles
            else if (attrs.empty()) cell.pen.attr &= ~mask; // DECCARA no-param clears
            else cell.pen.attr |= mask;           // DECCARA sets
        }
    }
    touch();
}

void Screen::enter_alt_screen() {
    if (on_alt_) return;
    saved_primary_ = cells_;             // stash the primary buffer
    saved_primary_row_of_ = row_of_;     // and its row map
    saved_primary_cursor_ = cursor_;
    std::fill(cells_.begin(), cells_.end(), Cell{}); // alt screen starts blank
    reset_row_map();                     // fresh identity mapping
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
        if (saved_primary_row_of_.size() == row_of_.size())
            row_of_ = saved_primary_row_of_; // and its row map
    }
    cursor_ = saved_primary_cursor_;
    saved_primary_.clear();
    saved_primary_row_of_.clear();
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
        set_wrapped(r.get(), false); // an erased line no longer soft-wraps
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
        reset_row_map();
        std::fill(live_wrapped_.begin(), live_wrapped_.end(), false);
        graphics_.clear(); // clearing the screen removes inline images too
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
    case 'b': { // REP — repeat the last printed char Ps times
        if (last_char_ != 0) {
            int n = param_or(p, 0, 1);
            n = std::clamp(n, 0, size_.rows * size_.cols);
            for (int i = 0; i < n; ++i) put(last_char_);
        }
        break;
    }
    case 'x': // DECFRA — fill rectangle with a character: CSI Pch;t;l;b;r $ x
        if (d.intermediates.size() == 1 && d.intermediates[0] == '$') {
            fill_rect(param_raw(p, 1, 1), param_raw(p, 2, 1), param_raw(p, 3, 0),
                      param_raw(p, 4, 0), static_cast<char32_t>(param_raw(p, 0, 32)));
        }
        break;
    case 'z': // DECERA — erase rectangle to blanks: CSI t;l;b;r $ z
        if (d.intermediates.size() == 1 && d.intermediates[0] == '$') {
            fill_rect(param_raw(p, 0, 1), param_raw(p, 1, 1), param_raw(p, 2, 0),
                      param_raw(p, 3, 0), U' ');
        }
        break;
    case 'y': // DECRQCRA — report a rectangle's checksum: CSI Pid;Pp;t;l;b;r * y
        if (d.intermediates.size() == 1 && d.intermediates[0] == '*') {
            const int pid = param_raw(p, 0, 0);
            // param 1 is the page (ignored, single page). Rect is params 2..5.
            const int t = std::clamp((param_raw(p, 2, 1) ? param_raw(p, 2, 1) : 1) - 1, 0,
                                     size_.rows - 1);
            const int l = std::clamp((param_raw(p, 3, 1) ? param_raw(p, 3, 1) : 1) - 1, 0,
                                     size_.cols - 1);
            const int b = std::clamp((param_raw(p, 4, 0) ? param_raw(p, 4, 0) : size_.rows) - 1,
                                     0, size_.rows - 1);
            const int r = std::clamp((param_raw(p, 5, 0) ? param_raw(p, 5, 0) : size_.cols) - 1,
                                     0, size_.cols - 1);
            std::uint16_t sum = 0;
            for (int row = t; row <= b; ++row)
                for (int col = l; col <= r; ++col) {
                    const char32_t cp = at(Row{row}, Col{col}).cp;
                    sum = static_cast<std::uint16_t>(sum + (cp ? cp : U' '));
                }
            // DEC reports the negation of the sum, as 4 uppercase hex digits.
            const std::uint16_t chk = static_cast<std::uint16_t>(-static_cast<int>(sum));
            char buf[32];
            std::snprintf(buf, sizeof buf, "\x1bP%d!~%04X\x1b\\", pid, chk);
            reply(buf);
        }
        break;
    case 'd': move_cursor_abs(Row{param_or(p, 0, 1) - 1}, cursor_.col); break; // VPA
    case 'r': // DECSTBM (set scroll region) or DECCARA (change attrs in rect)
        if (d.intermediates.size() == 1 && d.intermediates[0] == '$') {
            // DECCARA: CSI t;l;b;r ; Ps... $ r  -> apply SGR attrs to a rect.
            change_rect_attrs(param_raw(p, 0, 1), param_raw(p, 1, 1), param_raw(p, 2, 0),
                              param_raw(p, 3, 0), p.subspan(std::min<std::size_t>(4, p.size())),
                              /*reset=*/false);
        } else if (!d.private_marker) {
            set_scroll_region(param_raw(p, 0, 0), param_raw(p, 1, 0));
        }
        break;
    case 's':
        if (!d.private_marker) save_cursor(); // ANSI.SYS save cursor
        break;
    case 'u':
        if (d.private_marker && (d.marker == '?' || d.marker == '>' || d.marker == '<' ||
                                 d.marker == '=')) {
            // Kitty keyboard protocol: CSI ? u (query), > u (push), < u (pop),
            // = u (set). The private marker distinguishes these from CSI u.
            kitty_keyboard(d);
        } else if (!d.private_marker) {
            restore_cursor(); // ANSI.SYS / DECRC restore cursor
        }
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
            // \e[?62;...c advertises a VT220 with 132-col, colour, selective
            // erase, and SIXEL graphics (4) — features we actually implement.
            reply("\x1b[?62;1;4;6;22c");
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
    case 'q':
        if (d.private_marker && d.marker == '>') {
            // XTVERSION: CSI > 0 q -> report name+version as a DCS string.
            reply("\x1bP>|toe(0.1)\x1b\\");
        } else if (d.intermediates.size() == 1 && d.intermediates[0] == ' ') {
            // DECSCUSR: CSI Ps SP q -> set cursor shape + blink.
            //   0/1 blinking block, 2 steady block, 3 blinking underline,
            //   4 steady underline, 5 blinking bar, 6 steady bar.
            const int ps = param_raw(p, 0, 0);
            CursorStyle st;
            switch (ps) {
            case 0: case 1: st = {CursorShape::block, true}; break;
            case 2:         st = {CursorShape::block, false}; break;
            case 3:         st = {CursorShape::underline, true}; break;
            case 4:         st = {CursorShape::underline, false}; break;
            case 5:         st = {CursorShape::bar, true}; break;
            case 6:         st = {CursorShape::bar, false}; break;
            default:        st = {CursorShape::block, true}; break;
            }
            if (st != cursor_style_) {
                cursor_style_ = st;
                touch();
            }
        }
        break;
    case 'm': apply_sgr(p, d.sub); break;                   // SGR
    case 't': { // XTWINOPS window ops, or DECRARA (reverse attrs in rect)
        if (d.intermediates.size() == 1 && d.intermediates[0] == '$') {
            // DECRARA: CSI t;l;b;r ; Ps... $ t -> toggle SGR attrs in a rect.
            change_rect_attrs(param_raw(p, 0, 1), param_raw(p, 1, 1), param_raw(p, 2, 0),
                              param_raw(p, 3, 0), p.subspan(std::min<std::size_t>(4, p.size())),
                              /*reset=*/true);
            break;
        }
        if (d.private_marker) break;
        const int op = param_raw(p, 0, 0);
        if (op == 14) { // report text-area size in pixels: CSI 4 ; h ; w t
            std::string r = "\x1b[4;";
            r += std::to_string(size_.rows * cell_h_);
            r += ';';
            r += std::to_string(size_.cols * cell_w_);
            r += 't';
            reply(r);
        } else if (op == 16) { // report cell size in pixels: CSI 6 ; h ; w t
            std::string r = "\x1b[6;";
            r += std::to_string(cell_h_);
            r += ';';
            r += std::to_string(cell_w_);
            r += 't';
            reply(r);
        } else if (op == 18) { // report text-area size in cells: CSI 8 ; r ; c t
            std::string r = "\x1b[8;";
            r += std::to_string(size_.rows);
            r += ';';
            r += std::to_string(size_.cols);
            r += 't';
            reply(r);
        }
        // Other window ops (resize/move/iconify) are honoured by the host, not
        // the model — ignored here.
        break;
    }
    case 'p':
        // DECRQM: CSI ? Ps $ p (private) or CSI Ps $ p (ANSI) -> report mode.
        if (d.intermediates.size() == 1 && d.intermediates[0] == '$') {
            report_mode(param_raw(p, 0, 0), d.private_marker);
        } else if (d.intermediates.size() == 1 && d.intermediates[0] == '!') {
            // DECSTR (CSI ! p): soft reset. Unlike RIS it keeps screen content;
            // it restores modes, attributes, the scroll region and the cursor.
            soft_reset();
        }
        break;
    default: break; // unhandled — silently ignore for now
    }
}

// XTGETTCAP (DCS + q ...): the app asks for terminfo capabilities by hex-
// encoded name. We answer a small, high-value subset so shells stop probing.
void Screen::dcs(std::string_view prefix, std::string_view data) {
    // DECRQSS: DCS $ q <setting> ST  -> report the current value of a setting.
    // The request body is the setting's final byte(s), verbatim (not hex).
    if (prefix == "$q") {
        decrqss(data);
        return;
    }
    if (prefix != "+q") {
        return; // XTGETTCAP (+q) and DECRQSS ($q) are the answered DCS queries.
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

void Screen::decrqss(std::string_view req) {
    // Build the DECRQSS response body for a known setting, or report invalid.
    //   valid:   DCS 1 $ r <body> ST
    //   invalid: DCS 0 $ r ST
    std::string body;
    bool valid = true;

    if (req == "q") {
        // DECSCUSR: <ps> SP q, where ps encodes shape+blink (see the setter).
        int ps = 1;
        switch (cursor_style_.shape) {
        case CursorShape::block:     ps = cursor_style_.blink ? 1 : 2; break;
        case CursorShape::underline: ps = cursor_style_.blink ? 3 : 4; break;
        case CursorShape::bar:       ps = cursor_style_.blink ? 5 : 6; break;
        }
        body = std::to_string(ps) + " q";
    } else if (req == "r") {
        // DECSTBM: <top> ; <bottom> r, 1-based.
        body = std::to_string(scroll_top_ + 1) + ";" +
               std::to_string(scroll_bottom_ + 1) + "r";
    } else if (req == "m") {
        // SGR: the active pen as a ';'-joined parameter list, ending in 'm'.
        body = current_sgr() + "m";
    } else {
        valid = false;
    }

    std::string r = valid ? "\x1bP1$r" : "\x1bP0$r";
    r += body;
    r += "\x1b\\";
    reply(r);
}

// Serialize the active pen as a ';'-joined SGR parameter list (no leading CSI,
// no trailing 'm') for DECRQSS 'm'. Always starts at 0 (reset) so the list is
// absolute, then adds each active attribute and non-default colour.
std::string Screen::current_sgr() const {
    std::string s = "0";
    const auto add = [&](std::string_view code) { s += ';'; s += code; };

    const Attr a = pen_.attr;
    if (has(a, Attr::Bold)) add("1");
    if (has(a, Attr::Faint)) add("2");
    if (has(a, Attr::Italic)) add("3");
    if (has(a, Attr::Underline)) add("4");
    if (has(a, Attr::Blink)) add("5");
    if (has(a, Attr::Reverse)) add("7");
    if (has(a, Attr::Hidden)) add("8");
    if (has(a, Attr::Strike)) add("9");
    if (has(a, Attr::Overline)) add("53");

    const auto add_color = [&](const Color &c, int base) {
        // base 30 = fg (38 extended), 40 = bg (48 extended).
        if (const auto *idx = std::get_if<IndexedColor>(&c)) {
            const int i = idx->index;
            if (i < 8) add(std::to_string(base + i));
            else if (i < 16) add(std::to_string(base + 60 + (i - 8))); // bright
            else { add(std::to_string(base + 8)); add("5"); add(std::to_string(i)); }
        } else if (const auto *tc = std::get_if<TrueColor>(&c)) {
            add(std::to_string(base + 8)); add("2");
            add(std::to_string(tc->rgb.r));
            add(std::to_string(tc->rgb.g));
            add(std::to_string(tc->rgb.b));
        }
        // DefaultColor emits nothing (the leading 0 already reset it).
    };
    add_color(pen_.fg, 30);
    add_color(pen_.bg, 40);
    return s;
}

void Screen::esc(const vt::EscDispatch &d) {
    if (d.intermediates.empty()) {
        switch (d.final) {
        case 'c': // RIS — reset to initial state.
            std::fill(cells_.begin(), cells_.end(), Cell{});
            reset_row_map();
            cursor_ = Pos{};
            pen_ = Pen{};
            wrap_pending_ = false;
            scroll_top_ = 0;
            scroll_bottom_ = size_.rows - 1;
            charset_g0_ = charset_g1_ = Charset::Ascii;
            charset_use_g1_ = false;
            sync_output_ = false; // never leave rendering frozen after a reset
            focus_events_ = false;
            kitty_stack_.assign(1, 0); // reset kitty keyboard flags to base
            cursor_style_ = {};        // reset cursor shape
            std::fill(line_attr_.begin(), line_attr_.end(), LineAttr::normal);
            graphics_.clear();
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
    // Character-set designation: ESC ( <f> selects G0, ESC ) <f> selects G1.
    // '0' = DEC Special Graphics (line drawing), 'B'/'A'/'~' etc. = ASCII-ish.
    // This is how VT100-era and ncurses apps (tmux, dialog, mc) draw borders.
    if (d.intermediates.size() == 1 && (d.intermediates[0] == '(' || d.intermediates[0] == ')')) {
        const Charset cs = (d.final == '0') ? Charset::DecGraphics : Charset::Ascii;
        if (d.intermediates[0] == '(') charset_g0_ = cs;
        else charset_g1_ = cs;
        return;
    }
    // DEC line attributes + alignment test: ESC # <f>. These set a rendition on
    // the current row (double width/height) or run the DECALN self-test.
    if (d.intermediates.size() == 1 && d.intermediates[0] == '#') {
        const std::int32_t r = cursor_.row.get();
        switch (d.final) {
        case '3': set_line_attr(r, LineAttr::double_top); return;    // DECDHL top
        case '4': set_line_attr(r, LineAttr::double_bottom); return; // DECDHL bottom
        case '5': set_line_attr(r, LineAttr::normal); return;        // DECSWL single
        case '6': set_line_attr(r, LineAttr::double_width); return;  // DECDWL
        case '8': // DECALN — fill the whole screen with 'E' (alignment test)
            for (std::int32_t row = 0; row < size_.rows; ++row)
                for (std::int32_t col = 0; col < size_.cols; ++col)
                    at(Row{row}, Col{col}) = Cell{U'E', Pen{}, 1, 0};
            std::fill(line_attr_.begin(), line_attr_.end(), LineAttr::normal);
            cursor_ = Pos{};
            touch();
            return;
        default: return;
        }
    }
}

Screen::LineAttr Screen::line_attr(std::int32_t vrow) const noexcept {
    // Viewport rows drawn from scrollback have no line attribute (normal).
    if (scroll_offset_ > 0) {
        const std::int32_t live_row = vrow - scroll_offset_;
        if (live_row < 0) return LineAttr::normal;
        vrow = live_row;
    }
    if (vrow >= 0 && vrow < static_cast<std::int32_t>(line_attr_.size()))
        return line_attr_[static_cast<std::size_t>(vrow)];
    return LineAttr::normal;
}

void Screen::set_line_attr(std::int32_t row, LineAttr a) {
    if (row >= 0 && row < static_cast<std::int32_t>(line_attr_.size())) {
        if (line_attr_[static_cast<std::size_t>(row)] != a) {
            line_attr_[static_cast<std::size_t>(row)] = a;
            stamp(row);
            touch();
        }
    }
}

// --- SGR (Select Graphic Rendition) ---------------------------------------

void Screen::apply_sgr(std::span<const int> params, std::span<const std::uint8_t> sub) {
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
        case 4:
            pen_.attr |= Attr::Underline;
            // SGR 4:N selects the underline style (4:1 single .. 4:5 dashed).
            // A bare 4 (no colon subparam) is a plain single underline.
            if (i + 1 < params.size() && i + 1 < sub.size() && sub[i + 1]) {
                switch (params[i + 1]) {
                case 0: pen_.attr &= ~Attr::Underline; pen_.underline = Underline::None; break;
                case 1: pen_.underline = Underline::Single; break;
                case 2: pen_.underline = Underline::Double; break;
                case 3: pen_.underline = Underline::Curly; break;
                case 4: pen_.underline = Underline::Dotted; break;
                case 5: pen_.underline = Underline::Dashed; break;
                default: pen_.underline = Underline::Single; break;
                }
                ++i; // consume the subparam
            } else {
                pen_.underline = Underline::Single;
            }
            break;
        case 5: pen_.attr |= Attr::Blink; break;
        case 6: pen_.attr |= Attr::Blink; break; // rapid blink -> blink
        case 7: pen_.attr |= Attr::Reverse; break;
        case 8: pen_.attr |= Attr::Hidden; break;
        case 9: pen_.attr |= Attr::Strike; break;
        case 21: // double underline
            pen_.attr |= Attr::Underline;
            pen_.underline = Underline::Double;
            break;
        case 53: pen_.attr |= Attr::Overline; break;
        case 22: pen_.attr &= ~(Attr::Bold | Attr::Faint); break;
        case 23: pen_.attr &= ~Attr::Italic; break;
        case 24: pen_.attr &= ~Attr::Underline; pen_.underline = Underline::None; break;
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
            } else if (c == 38 || c == 48 || c == 58) {
                // Extended color: 38/48/58 ;5;n (indexed) or ;2;r;g;b (true).
                // 58 is the underline colour (rendered in its own colour when
                // set; falls back to fg when default). Colon- and semicolon-
                // separated forms are both handled by scanning ahead.
                Color *target = (c == 38) ? &pen_.fg : (c == 48) ? &pen_.bg : &pen_.underline_color;
                if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
                    *target = IndexedColor{static_cast<std::uint8_t>(params[i + 2])};
                    i += 2;
                } else if (i + 4 < params.size() && params[i + 1] == 2) {
                    *target = TrueColor{Rgb{static_cast<std::uint8_t>(params[i + 2]),
                                            static_cast<std::uint8_t>(params[i + 3]),
                                            static_cast<std::uint8_t>(params[i + 4])}};
                    i += 4;
                }
            } else if (c == 59) {
                pen_.underline_color = DefaultColor{}; // reset underline colour to fg
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
            // Skip the trailing spacer of a wide (CJK/emoji) glyph — its
            // codepoint is a placeholder space; the real glyph is in the lead
            // cell, so emitting the spacer would append a spurious space.
            if (cell && cell->width == 0 && cell->cp == U' ') continue;
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

} // namespace toe::term
