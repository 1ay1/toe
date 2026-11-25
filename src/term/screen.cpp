// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/term/screen.hpp"
#include "toe/term/width.hpp"

#include <algorithm>
#include <cstring>
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

Screen::Screen(Extent size) : size_{size} {
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    scroll_left_ = 0;
    scroll_right_ = size_.cols - 1;
    lr_margins_ = false;
    ring_.reset(size_.rows, size_.cols, max_history_, Cell{});
    line_attr_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), LineAttr::normal);
    row_epoch_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), 0);
    tab_stops_.assign(static_cast<std::size_t>(std::max(size_.cols, 1)), false);
    for (std::int32_t c = 0; c < size_.cols; c += 8) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
}

void Screen::set_max_history(std::size_t lines) {
    if (lines == max_history_) return;
    max_history_ = lines;
    // The alt screen has no scrollback, so only the primary ring is re-capped;
    // the new cap applies to the primary buffer when it's swapped back in.
    if (!on_alt_) {
        const std::int64_t top_abs =
            static_cast<std::int64_t>(hist_rows()) - scroll_offset_;
        const std::int64_t before = static_cast<std::int64_t>(hist_rows());
        ring_.resize_keep(size_.rows, size_.cols, max_history_, blank_cell());
        // the log holds the OLDEST history: it gives way first
        if (hist_rows() > max_history_) log_.evict_rows(std::min<std::uint64_t>(log_.rows(), hist_rows() - max_history_));
        if (scroll_offset_ > 0) {
            const std::int64_t dropped = before - static_cast<std::int64_t>(hist_rows());
            scroll_offset_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(hist_rows()) - (top_abs - dropped), 0,
                static_cast<std::int64_t>(hist_rows())));
        }
        stamp_all();
        touch();
    }
}

std::size_t Screen::index(Row r, Col c) const noexcept {
    // Offset into the ring arena for the visible row r. Used by bulk ops.
    return static_cast<std::size_t>(ring_.slot_of(ring_.view_base() +
                                                 static_cast<std::size_t>(r.get()))) *
               static_cast<std::size_t>(size_.cols) +
           static_cast<std::size_t>(c.get());
}

// Pointer to the first cell of visible row r at column c — the bulk-write path
// (put_ascii_run / put) writes contiguous cells through this.
Cell *Screen::cell_ptr(Row r, Col c) noexcept {
    return ring_.view_row(r.get()) + c.get();
}

void Screen::clean_wide_at(Row r, std::int32_t c) noexcept {
    if (c < 0 || c >= size_.cols) return;
    Cell *row = ring_.view_row(r.get());
    if (row[c].width == 2) {           // a wide lead: blank its trailing spacer
        if (c + 1 < size_.cols && row[c + 1].width == 0) {
            row[c + 1] = blank_cell();
            ring_.note_used(r.get(), c + 2);
        }
    } else if (row[c].width == 0) {    // a spacer: blank its wide lead to the left
        if (c - 1 >= 0 && row[c - 1].width == 2) {
            row[c - 1] = blank_cell();
            ring_.note_used(r.get(), c + 1);
        }
    }
}

Cell &Screen::at(Row r, Col c) {
    assert(r.get() >= 0 && r.get() < size_.rows);
    assert(c.get() >= 0 && c.get() < size_.cols);
    stamp(r.get());
    ring_.note_used(r.get(), c.get() + 1); // this cell may be written through the ref
    return ring_.view_row(r.get())[c.get()];
}

void Screen::copy_cell(std::int32_t dr, std::int32_t dc, std::int32_t sr, std::int32_t sc) {
    const Cell v = at(Row{sr}, Col{sc});
    at(Row{dr}, Col{dc}) = v;
    // A moved cell may be half of a wide pair: the row's "no wide glyph"
    // promise (what lets history rows be counted as ceil(len / W)) is off.
    if (v.width != 1) ring_.mark_view_wide(dr);
}

const Cell &Screen::at(Row r, Col c) const {
    assert(r.get() >= 0 && r.get() < size_.rows);
    assert(c.get() >= 0 && c.get() < size_.cols);
    return ring_.view_row(r.get())[c.get()];
}

std::span<const Cell> Screen::row(Row r) const {
    // Viewport row r (0 = top of visible area). When scrolled back, the top
    // scroll_offset_ viewport rows come from history: the log's rows (painted
    // on demand at the current width), then the ring's own scrollback.
    const std::int64_t sabs = static_cast<std::int64_t>(hist_rows()) + r.get() - scroll_offset_;
    if (sabs < 0 || sabs >= total_rows()) {
        scratch_row_.assign(static_cast<std::size_t>(size_.cols), blank_cell());
        return std::span<const Cell>{scratch_row_.data(), static_cast<std::size_t>(size_.cols)};
    }
    const Cell *c = cell_at_abs(sabs, 0);
    return std::span<const Cell>{c, static_cast<std::size_t>(size_.cols)};
}

const Screen::LogRowCache &Screen::log_row(std::int64_t abs) const noexcept {
    const std::size_t slots = static_cast<std::size_t>(std::max(size_.rows, 1)) * 2 + 2;
    if (log_cache_.size() != slots) log_cache_.assign(slots, LogRowCache{});
    LogRowCache &e = log_cache_[static_cast<std::size_t>(abs) % slots];
    if (e.abs != abs || e.seq != log_.seq() || e.cols != size_.cols) {
        e.abs = abs;
        e.seq = log_.seq();
        e.cols = size_.cols;
        e.cells.resize(static_cast<std::size_t>(size_.cols));
        (void)log_.paint(static_cast<std::uint64_t>(abs), e.cells.data(), size_.cols, e.soft);
    }
    return e;
}

bool Screen::wrapped_abs(std::int64_t abs_row) const noexcept {
    if (abs_row < 0 || abs_row >= total_rows()) return false;
    const auto lr = static_cast<std::int64_t>(log_rows());
    if (abs_row < lr) return log_row(abs_row).soft;
    return ring_.wrapped_abs(static_cast<std::size_t>(abs_row - lr));
}

void Screen::scroll(std::int32_t delta) {
    const std::int32_t max = static_cast<std::int32_t>(hist_rows());
    scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max);
    touch();
}

void Screen::scroll_to_bottom() {
    if (scroll_offset_ != 0) {
        scroll_offset_ = 0;
        touch();
    }
}

void Screen::scroll_to_abs_row(std::int64_t abs_row, std::int32_t margin) {
    // viewport_to_abs(vrow) = scrollback - scroll_offset + vrow. Solve for the
    // offset that lands abs_row at viewport row `margin`.
    const std::int64_t sb = static_cast<std::int64_t>(hist_rows());
    const std::int64_t want = sb + margin - abs_row;
    const std::int32_t off = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(want, 0, sb));
    if (off != scroll_offset_) {
        scroll_offset_ = off;
        touch();
    }
}

void Screen::resize(Extent size) { resize(size, -1); }

void Screen::resize(Extent size, std::int64_t keep_from) {
    if (size == size_ || size.cols <= 0 || size.rows <= 0) {
        if (size == size_) return;
    }
    reflow_map_.clear();
    resize_keep_from_ = keep_from;

    // On the alt screen (vim/tmux/htop own the whole grid) reflow is wrong: the
    // app redraws on SIGWINCH and every row — including blank ones — is content
    // it manages, so we crop/pad in place. The PRIMARY screen always goes
    // through reflow: it rewraps on a width change AND trims the trailing blank
    // screen space below the cursor on a height shrink (so a small window
    // doesn't push the prompt up into scrollback). reflow is O(cells) and fast.
    const Extent old_size = size_;
    const bool do_reflow = !on_alt_ && size_.cols > 0 && size_.rows > 0;

    if (do_reflow) {
        reflow(old_size, size, keep_from);
    } else {
        // Alt screen: crop/pad in place, anchoring the top-of-viewport to the
        // same absolute row so scroll position doesn't drift.
        const std::int64_t old_view_base = static_cast<std::int64_t>(ring_.view_base());
        const std::int64_t top_abs = old_view_base - scroll_offset_;
        ring_.resize_keep(size.rows, size.cols, 0, blank_cell());
        if (scroll_offset_ > 0) {
            const std::int64_t new_view_base = static_cast<std::int64_t>(ring_.view_base());
            scroll_offset_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                new_view_base - top_abs, 0, static_cast<std::int64_t>(ring_.scrollback())));
        }
        size_ = size;
    }

    // A resize resets the scroll region to the full screen (xterm behavior).
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    scroll_left_ = 0;
    scroll_right_ = size_.cols - 1;
    lr_margins_ = false;
    // Rebuild default tab stops for the new width.
    tab_stops_.assign(static_cast<std::size_t>(std::max(size_.cols, 1)), false);
    for (std::int32_t c = 0; c < size_.cols; c += 8) {
        tab_stops_[static_cast<std::size_t>(c)] = true;
    }
    clamp_cursor();
    row_epoch_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), 0);
    line_attr_.assign(static_cast<std::size_t>(std::max(size_.rows, 0)), LineAttr::normal);
    any_line_attr_ = false;
    stamp_all();
    touch();
}

// Re-lay the screen for a new size.
//
// History is a LineLog: logical lines, held width-free. So a resize never
// re-wraps it. What is still a GRID is re-laid: the rows the ring pushed into
// history since the last resize, and the live screen; and the log is re-indexed
// for the new width (one pass over line lengths -- no cell of a line without
// wide glyphs is read). Cost: O(rows pushed since the last resize + screen +
// history LINES), never O(history CELLS): a 10k-line scrollback resizes as
// fast as an empty one. The rows the ring pushed join the log zero-copy when
// there are many (the ring's arena is frozen and the lines point into it),
// by copy when few (each history cell is copied at most once, ever).
//
// What a host observes is exactly what the grid reflow produced, its quirks
// included: tests/reflow_oracle_test.cpp pins it bit for bit.
void Screen::reflow(Extent old_size, Extent new_size, std::int64_t keep_from) {
    const std::int32_t ocols = old_size.cols, ncols = new_size.cols, nrows = new_size.rows;
    if (log_.width() != ocols) log_.set_width(ocols);
    const std::int64_t old_hist = static_cast<std::int64_t>(hist_rows());
    const bool was_scrolled = scroll_offset_ > 0;
    const std::int64_t anchor_abs = old_hist - scroll_offset_;
    reflow_cells_.clear();
    reflow_lines_.clear();
    reflow_rows_.clear();

    // How much of physical row `rowp` belongs to its logical line, trimming
    // trailing blanks on a HARD (non-wrapped) row. `next`: the following
    // physical row (nullptr if none), to tell a wide glyph's wrap padding
    // from a real space. `pad_spacer`: a wide LEAD left at the end needs its
    // width-0 spacer re-synthesised (see append_row).
    struct Cut { std::int32_t len; bool pad_spacer; };
    const auto cut = [](const Cell *rowp, std::int32_t cols, bool wrapped, const Cell *next) -> Cut {
        std::int32_t len = cols;
        if (!wrapped) {
            while (len > 0 && rowp[len - 1].blank()) --len;
        } else {
            // A soft-wrapped row can still end in blanks that are NOT content:
            // when a double-width glyph won't fit the last column, the terminal
            // leaves it empty and wraps. Those padding cells are an artifact of
            // the OLD width, so carrying them into the arena injected blanks
            // into the middle of the logical line on every rewrap. Trim them,
            // at most one column, and ONLY when the next row really starts with
            // a wide glyph: a plain space that happened to land in the last
            // column is text ("12th Gen Intel" must not become "12th GenIntel").
            if (len > 0 && rowp[len - 1].blank() && next != nullptr && next[0].width == 2) --len;
        }
        return {len, len > 0 && rowp[len - 1].width == 2};
    };
    // Append a row's cut into reflow_cells_ (a line that spans rows). A wide
    // LEAD keeps its width-0 spacer: the pair is one glyph, and the spacer can
    // be lost when it sat in a trimmed column, so re-synthesise it.
    const auto append_row = [&](const Cell *rowp, Cut c) -> std::uint32_t {
        reflow_cells_.insert(reflow_cells_.end(), rowp, rowp + c.len);
        if (c.pad_spacer) {
            Cell spacer{};
            spacer.pen = rowp[c.len - 1].pen;
            spacer.width = 0;
            reflow_cells_.push_back(spacer);
            return static_cast<std::uint32_t>(c.len) + 1u;
        }
        return static_cast<std::uint32_t>(c.len);
    };
    // One logical line under construction. It stays IN PLACE -- `first`
    // plus `run` cells laid row-major in the ring arena -- while every row so
    // far was cut to its full width and the next lies in the next slot (a
    // soft-wrapped line in a history: nearly all of them). Only a break in
    // that (a trim, a spacer to synthesise, a row elsewhere) copies it out.
    // `next_slot`: where an in-place continuation must lie (nullptr: none).
    struct Open {
        bool have = false;
        const Cell *first = nullptr;
        std::uint32_t run = 0;         // cells in place from `first` (while !joined)
        bool tail_spacer = false;      // the in-place run ends in a cut that needs a spacer
        const Cell *next_slot = nullptr;
        bool joined = false;
        std::uint32_t off = 0;
        bool wide = false;
        [[nodiscard]] std::uint32_t len(const std::vector<Cell> &cells) const noexcept {
            if (!have) return 0;
            if (joined) return static_cast<std::uint32_t>(cells.size()) - off;
            return run + (tail_spacer ? 1u : 0u);
        }
    };
    Open open;
    const Cell *const arena0 = ring_.arena_data();
    const Cell *const arena_end = arena0 + ring_.cap_slots() * static_cast<std::size_t>(ocols);
    // The physical row after `rowp` in the ring, or nullptr when rowp is not
    // a ring row or is its last slot: an in-place run never wraps, so it is
    // one flat span (the re-wrap and paint read it so).
    const auto slot_after = [&](const Cell *rowp) -> const Cell * {
        if (rowp < arena0 || rowp >= arena_end) return nullptr;
        const Cell *n = rowp + ocols;
        return n >= arena_end ? nullptr : n;
    };
    // Copy the in-place run out (the line can no longer be read in place).
    const auto join = [&]() {
        if (open.joined) return;
        open.off = static_cast<std::uint32_t>(reflow_cells_.size());
        // the run is flat: whole rows of `ocols` then a partial, contiguous
        reflow_cells_.insert(reflow_cells_.end(), open.first, open.first + open.run);
        if (open.tail_spacer) {
            Cell spacer{};
            spacer.pen = reflow_cells_.back().pen;
            spacer.width = 0;
            reflow_cells_.push_back(spacer);
        }
        open.joined = true;
    };
    const auto add_row = [&](const Cell *rowp, Cut c, bool wide) -> std::uint32_t {
        const bool full = c.len == ocols && !c.pad_spacer;
        if (!open.have) {
            open = Open{};
            open.have = true;
            open.first = rowp;
            open.run = static_cast<std::uint32_t>(c.len);
            open.tail_spacer = c.pad_spacer;
            open.next_slot = full ? slot_after(rowp) : nullptr;
            open.wide = wide;
            return 0;
        }
        open.wide = open.wide || wide;
        if (!open.joined && open.next_slot == rowp && !open.tail_spacer) {   // continues in place
            const std::uint32_t at = open.run;
            open.run += static_cast<std::uint32_t>(c.len);
            open.tail_spacer = c.pad_spacer;
            open.next_slot = full ? slot_after(rowp) : nullptr;
            return at;
        }
        join();
        const std::uint32_t at = static_cast<std::uint32_t>(reflow_cells_.size()) - open.off;
        (void)append_row(rowp, c);
        return at;
    };
    const auto close_line = [&](bool from_live, bool crop) {
        if (!open.have) return;
        if (!open.joined && open.tail_spacer) join();
        Span ln{0, open.len(reflow_cells_), false, from_live};
        ln.crop = crop;
        ln.wide = open.wide;
        if (open.joined) ln.off = open.off;
        else ln.src = open.first;
        reflow_lines_.push_back(ln);
        open = Open{};
    };
    const auto settle_spacer = [&]() {
        if (open.have && !open.joined && open.tail_spacer) join();
    };
    const auto line_cells = [&](const Span &ln) -> const Cell * {
        return ln.src != nullptr ? ln.src : reflow_cells_.data() + ln.off;
    };

    // A. The log's OPEN line (its last row soft-wraps into the ring) joins
    //    the grid's rows below it: paint its rows at the old width -- exactly
    //    the rows the grid reflow would have gathered -- and take it out.
    std::vector<Cell> trows;
    std::size_t tcount = 0;
    if (log_.open()) {
        const std::uint64_t r1 = log_.rows();
        const std::int64_t f = log_.first_row(log_.next_id() - 1);
        const std::uint64_t from = f < 0 ? 0 : static_cast<std::uint64_t>(f);
        tcount = static_cast<std::size_t>(r1 - from);
        trows.resize(tcount * static_cast<std::size_t>(ocols));
        for (std::size_t k = 0; k < tcount; ++k) {
            bool soft = false;
            (void)log_.paint(from + k, trows.data() + k * static_cast<std::size_t>(ocols), ocols, soft);
        }
        log_.drop_back_rows(tcount);
    }
    // B. The log's closed lines, re-cut at the old width as the grid reflow
    //    re-cut every history row (a no-op for lines without wide glyphs or
    //    trailing blanks). Their rows' (line, offset) go to the row map.
    log_map_.clear();
    log_.recut(log_map_);

    // C. Gather the open line's rows, the ring's history and the live grid
    //    into logical lines. Where each OLD row went: (line, cell offset).
    struct RowAt { std::uint32_t line; std::uint32_t off; };
    std::vector<RowAt> row_at;
    const std::size_t rh = ring_.scrollback();
    row_at.reserve(tcount + rh + static_cast<std::size_t>(old_size.rows));
    const std::size_t hn = tcount + rh;
    const auto hrow = [&](std::size_t i) -> const Cell * {
        return i < tcount ? trows.data() + i * static_cast<std::size_t>(ocols) : ring_.abs_row(i - tcount);
    };
    for (std::size_t i = 0; i < hn; ++i) {
        const Cell *rowp = hrow(i);
        const bool from_log = i < tcount;
        const bool wrapped = from_log ? true : ring_.wrapped_abs(i - tcount);
        const Cell *next = i + 1 < hn ? hrow(i + 1) : (old_size.rows > 0 ? ring_.view_row(0) : nullptr);
        // Past a ring row's written high-water every cell is the fill: a hard
        // row's trailing-blank scan starts there (only when the fill IS blank).
        const std::int32_t used = (!from_log && ring_.fill_is_blank())
                                      ? static_cast<std::int32_t>(ring_.used_abs(i - tcount)) : ocols;
        const std::uint32_t line = static_cast<std::uint32_t>(reflow_lines_.size());
        const std::uint32_t at = add_row(rowp, cut(rowp, wrapped ? ocols : used, wrapped, next),
                                         from_log || ring_.wide_abs(i - tcount));
        row_at.push_back({line, at});
        if (!wrapped) {
            settle_spacer();
            close_line(false, false);
        }
    }
    // Lines closed so far are history; one still open continues into the grid.
    const std::size_t H = reflow_lines_.size();

    const std::int32_t cur_row = cursor_.row.get();
    std::size_t cursor_line = 0;
    std::int32_t cursor_col_in_line = 0;
    {
        // The live grid's meaningful content ends at the last non-blank row or
        // the cursor row, whichever is lower: rows below are empty screen, not
        // content (collecting them would push the prompt up on a shrink).
        std::int32_t last_meaningful = cur_row;
        for (std::int32_t r = old_size.rows - 1; r > last_meaningful; --r) {
            bool blank = true;
            const Cell *rowp = ring_.view_row(r);
            for (std::int32_t c = 0; c < ocols; ++c) {
                if (!rowp[c].blank()) { blank = false; break; }
            }
            if (!blank || wrapped_at(r)) { last_meaningful = r; break; }
        }
        const std::int32_t live_rows = last_meaningful + 1;
        // The protected prompt: live rows [protect_row, cur_row] (see resize()).
        const std::int32_t protect_row =
            (keep_from >= old_hist && keep_from - old_hist <= cur_row) ? static_cast<std::int32_t>(keep_from - old_hist) : -1;
        for (std::int32_t r = 0; r < live_rows; ++r) {
            const bool crop = protect_row >= 0 && r >= protect_row && r <= cur_row;
            if (crop && open.have) {
                settle_spacer();
                close_line(true, false);
            }
            const bool wrapped = crop ? false : wrapped_at(r);
            const Cell *rowp = ring_.view_row(r);
            const std::uint32_t line = static_cast<std::uint32_t>(reflow_lines_.size());
            const std::uint32_t at = add_row(rowp, cut(rowp, ocols, wrapped, r + 1 < live_rows ? ring_.view_row(r + 1) : nullptr),
                                             ring_.wide_abs(ring_.view_base() + static_cast<std::size_t>(r)));
            if (r == cur_row) {
                cursor_line = line;
                cursor_col_in_line = static_cast<std::int32_t>(at) + cursor_.col.get();
            }
            row_at.push_back({line, at});
            if (!wrapped) {
                settle_spacer();
                close_line(true, crop);
            }
        }
        settle_spacer();
        close_line(true, false);
    }

    // D. History lines join the log. Many ring rows: freeze the ring's arena
    //    and reference one-row lines in place (zero-copy); few: copy (the ring
    //    keeps its arena, and no mostly-empty arena is kept alive).
    const Cell *arena = ring_.arena_data();
    const std::size_t arena_cells = ring_.cap_slots() * static_cast<std::size_t>(ocols);
    std::shared_ptr<LineLog::Frozen> frozen;
    if (rh > 0 && (rh * 4 >= ring_.cap_slots() || freeze_always_)) {
        frozen = std::make_shared<LineLog::Frozen>();
        frozen->data = arena;
        frozen->cap = ring_.cap_slots();
        frozen->cols = ocols;
    }
    hist_ids_.resize(H);
    for (std::size_t li = 0; li < H; ++li) {
        const Span &ln = reflow_lines_[li];
        if (frozen && ln.src != nullptr && ln.src >= arena && ln.src < arena + arena_cells) {
            const auto at = static_cast<std::size_t>(ln.src - arena);
            hist_ids_[li] = log_.push_inplace(frozen, at / static_cast<std::size_t>(ocols), ln.len, false, ln.wide);
        } else {
            hist_ids_[li] = log_.push_owned(line_cells(ln), ln.len, false, ln.wide);
        }
    }

    // E. The log at the new width: one pass over its lines. P0 history rows
    //    precede the grid's lines (before the cap and the pull below).
    log_.set_width(ncols);
    const std::int64_t P0 = static_cast<std::int64_t>(log_.rows());
    const std::int64_t nc = std::max(ncols, 1);

    // F. Re-wrap the grid's lines at the new width into row Spans.
    std::vector<std::int64_t> first_live(reflow_lines_.size() - H, 0);
    std::int32_t new_cursor_row = 0, new_cursor_col = 0;
    bool cursor_placed = false;
    for (std::size_t li = H; li < reflow_lines_.size(); ++li) {
        const Span ln = reflow_lines_[li];
        const Cell *cells = line_cells(ln);
        first_live[li - H] = static_cast<std::int64_t>(reflow_rows_.size());
        if (ln.crop) {   // a protected prompt row: exactly one row, cropped
            const std::uint32_t take = std::min<std::uint32_t>(static_cast<std::uint32_t>(ncols), ln.len);
            if (ln.from_live && li == cursor_line && !cursor_placed) {
                new_cursor_row = static_cast<std::int32_t>(reflow_rows_.size());
                new_cursor_col = std::clamp(cursor_col_in_line, 0, ncols - 1);
                cursor_placed = true;
            }
            reflow_rows_.push_back({0, take, false, ln.from_live, static_cast<std::uint32_t>(li)});
            continue;
        }
        std::uint32_t off = 0;
        do {   // an empty line still yields one (blank) row
            const std::uint32_t take =
                LineLog::take_at([&](std::uint32_t i) -> const Cell & { return cells[i]; }, ln.len, off, ncols);
            const bool more = (off + take) < ln.len;
            if (ln.from_live && li == cursor_line && !cursor_placed) {
                const std::int32_t col = cursor_col_in_line;
                if (col >= static_cast<std::int32_t>(off) &&
                    (col < static_cast<std::int32_t>(off + static_cast<std::uint32_t>(ncols)) || !more)) {
                    new_cursor_row = static_cast<std::int32_t>(reflow_rows_.size());
                    new_cursor_col = std::clamp(col - static_cast<std::int32_t>(off), 0, ncols - 1);
                    cursor_placed = true;
                }
            }
            reflow_rows_.push_back({off, take, more, ln.from_live, static_cast<std::uint32_t>(li)});
            off += take;
        } while (off < ln.len);
    }
    if (!cursor_placed && !reflow_rows_.empty()) {
        new_cursor_row = static_cast<std::int32_t>(reflow_rows_.size()) - 1;
        new_cursor_col = 0;
    }
    const std::int64_t T = static_cast<std::int64_t>(reflow_rows_.size());
    // The live grid is the last `nrows` rows of everything: history rows come
    // DOWN into it when the grid's lines are short (pull), the grid's top rows
    // go UP into history when they are long (ovf).
    const std::int64_t ovf = std::max<std::int64_t>(0, T - nrows);
    const std::int64_t pull = std::min<std::int64_t>(std::max<std::int64_t>(0, nrows - T), P0);
    const std::int64_t live_first_raw = P0 - pull + ovf;   // history rows before the cap

    // Raw (pre-cap) new rows of the lines the row map and the anchor need.
    const auto raw_live = [&](std::size_t li, std::uint32_t off) -> std::int64_t {
        if (li >= reflow_lines_.size()) return P0 + T;
        if (li < H) return log_.first_row(hist_ids_[li]) + static_cast<std::int64_t>(off) / nc;
        return P0 + first_live[li - H] + (reflow_lines_[li].crop ? 0 : static_cast<std::int64_t>(off) / nc);
    };
    std::vector<std::int64_t> raw(log_map_.size() + row_at.size());
    for (std::size_t k = 0; k < log_map_.size(); ++k)
        raw[k] = log_.first_row(log_map_[k].first) + static_cast<std::int64_t>(log_map_[k].second) / nc;
    for (std::size_t k = 0; k < row_at.size(); ++k)
        raw[log_map_.size() + k] = raw_live(row_at[k].line, row_at[k].off);

    // G. The scroll anchor: the logical line at the viewport's top, found by
    //    counting old rows per line as ceil(len / old cols) -- the grid
    //    reflow's own (approximate, for wide lines) count, kept exactly.
    std::int64_t anchor_raw = 0;
    if (was_scrolled) {
        std::int64_t acc = 0;
        bool found = false, any = false;
        const auto oldrows = [&](std::uint32_t len) -> std::int64_t {
            return len == 0 ? 1 : (static_cast<std::int64_t>(len) + ocols - 1) / ocols;
        };
        log_.each_line([&](std::uint64_t id, std::uint32_t len) {
            if (found) return;
            any = true;
            anchor_raw = log_.first_row(id);
            if (anchor_abs < acc + oldrows(len)) { found = true; return; }
            acc += oldrows(len);
        });
        for (std::size_t li = H; !found && li < reflow_lines_.size(); ++li) {
            any = true;
            anchor_raw = P0 + first_live[li - H];
            if (anchor_abs < acc + oldrows(reflow_lines_[li].len)) found = true;
            acc += oldrows(reflow_lines_[li].len);
        }
        if (!any) anchor_raw = 0;
    }

    // H. Paint the new live grid -- pulled history rows, then the grid's rows
    //    -- and move the grid's overflow into the log.
    size_ = new_size;
    RowRing &out = reflow_ring_;
    out.reset_with_history(nrows, ncols, max_history_, 0, Cell{});
    std::int32_t r = 0;
    for (std::int64_t k = 0; k < pull; ++k, ++r) {
        bool soft = false;
        const std::uint32_t n = log_.paint(static_cast<std::uint64_t>(P0 - pull + k), out.view_row(r), ncols, soft);
        out.note_used(r, static_cast<std::int32_t>(n));
        out.set_view_wrapped(r, soft);
        out.set_view_wide(r, true);
    }
    if (pull > 0) log_.drop_back_rows(static_cast<std::uint64_t>(pull));
    for (std::int64_t i = ovf; i < T && r < nrows; ++i, ++r) {
        const Span &sp = reflow_rows_[static_cast<std::size_t>(i)];
        const Span &ln = reflow_lines_[sp.line];
        Cell *dst = out.view_row(r);
        const std::uint32_t len = std::min<std::uint32_t>(sp.len, static_cast<std::uint32_t>(ncols));
        std::memcpy(dst, line_cells(ln) + sp.off, static_cast<std::size_t>(len) * sizeof(Cell));
        std::fill(dst + len, dst + ncols, Cell{});
        out.note_used(r, static_cast<std::int32_t>(len));
        out.set_view_wrapped(r, sp.wrapped);
        out.set_view_wide(r, ln.wide);
    }
    for (; r < nrows; ++r) {
        Cell *dst = out.view_row(r);
        std::fill(dst, dst + ncols, Cell{});
        out.set_view_wrapped(r, false);
        out.set_view_wide(r, false);
    }
    for (std::int64_t i = 0; i < ovf;) {
        const std::uint32_t L = reflow_rows_[static_cast<std::size_t>(i)].line;
        std::int64_t j = i;
        while (j < ovf && reflow_rows_[static_cast<std::size_t>(j)].line == L) ++j;
        const Span &ln = reflow_lines_[L];
        const bool straddle = j < T && reflow_rows_[static_cast<std::size_t>(j)].line == L;
        if (straddle) {   // its head goes up, OPEN: it soft-wraps into the grid's top
            (void)log_.push_owned(line_cells(ln), reflow_rows_[static_cast<std::size_t>(j)].off, true, ln.wide);
        } else {
            (void)log_.push_owned(line_cells(ln), ln.crop ? reflow_rows_[static_cast<std::size_t>(i)].len : ln.len, false, ln.wide);
        }
        i = j;
    }

    // I. The cap, in rows at the new width: the oldest go.
    const std::int64_t hist = static_cast<std::int64_t>(log_.rows());
    const std::int64_t E = std::max<std::int64_t>(0, hist - static_cast<std::int64_t>(max_history_));
    if (E > 0) log_.evict_rows(static_cast<std::uint64_t>(E));

    // J. The new grid goes live; the old ring is the spare. If the log holds
    //    rows in its arena, the arena is frozen there.
    std::swap(ring_, reflow_ring_);
    ring_.continue_history_seq(reflow_ring_.history_seq());
    if (frozen && frozen.use_count() > 1) frozen->owner = reflow_ring_.take_storage();

    // K. Old absolute row -> new: the grid reflow's map, minus the cap.
    reflow_map_.resize(raw.size());
    for (std::size_t k = 0; k < raw.size(); ++k) reflow_map_[k] = raw[k] - E < 0 ? -1 : raw[k] - E;

    // L. Scroll position: the same logical line back at the viewport's top.
    const std::int64_t H2 = static_cast<std::int64_t>(log_.rows());
    scroll_offset_ = was_scrolled ? static_cast<std::int32_t>(std::clamp<std::int64_t>(live_first_raw - anchor_raw, 0, H2)) : 0;
    cursor_.row = Row{std::clamp(static_cast<std::int32_t>(pull - ovf) + new_cursor_row, 0, std::max(nrows - 1, 0))};
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
            } else if constexpr (std::is_same_v<T, vt::PrintRun>) {
                put_ascii_run(a.text);
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
// 0 for combining/zero-width marks, 1 otherwise. Uses a fast built-in range
// table (toe/term/width.hpp) — NOT libc wcwidth(), which is locale-dependent
// and slow enough to dominate parse time under CJK/emoji floods.
int char_width(char32_t cp) {
    return codepoint_width(cp);
}
} // namespace

// Bulk write of a printable-ASCII run — the flood hot path. Fills as many chars
// as fit within the current row's [left,right] margin in one stamped span, then
// hands the boundary char to put() (which wraps) and continues. This collapses
// the per-char variant/visit/apply overhead into one call per parser run.
void Screen::put_ascii_run(std::string_view ascii) {
    std::size_t i = 0;
    const std::size_t n = ascii.size();
    while (i < n) {
        // The general path handles a pending wrap or a non-default charset.
        if (wrap_pending_ || charset_g0_ != Charset::Ascii || charset_use_g1_) {
            put(static_cast<char32_t>(ascii[i++]));
            continue;
        }
        const std::int32_t r = cursor_.row.get();
        const std::int32_t right = right_bound();
        std::int32_t col = cursor_.col.get();
        if (col > right) { put(static_cast<char32_t>(ascii[i++])); continue; }
        // How many chars fit from `col` up to and including the right margin?
        const std::size_t room = static_cast<std::size_t>(right - col); // advance-without-wrap
        const std::size_t take = std::min(room, n - i);
        if (take > 0) {
            stamp(r);
            Cell *base = cell_ptr(cursor_.row, Col{col});
            // Boundary wide-pair cleanup (the run writes width-1 over [col,
            // col+take)): if it STARTS on a spacer, blank the orphaned wide lead
            // just left of it; if the cell just PAST the run is a spacer, blank
            // it (its lead is being overwritten). Otherwise a prompt redrawing
            // ASCII over old CJK leaves half-glyph orphans (gaps / stale cells).
            if (base[0].width == 0 && col > 0) {
                Cell *lead = cell_ptr(cursor_.row, Col{col - 1});
                if (lead->width == 2) *lead = blank_cell();
            }
            const std::int32_t endc = col + static_cast<std::int32_t>(take);
            if (endc < size_.cols) {
                Cell *after = cell_ptr(cursor_.row, Col{endc});
                if (after->width == 0) *after = blank_cell();
            }
            // One template cell (pen, width, link) built once per span; each
            // cell is then a 24-byte trivially-copyable store plus its char --
            // not a Cell constructed field by field through the Color variants.
            Cell tmpl{U' ', pen_, 1, 0, cur_link_};
            const char *src = ascii.data() + i;
            for (std::size_t k = 0; k < take; ++k) {
                tmpl.cp = static_cast<char32_t>(static_cast<unsigned char>(src[k]));
                std::memcpy(static_cast<void *>(base + k), &tmpl, sizeof(Cell));
            }
            ring_.note_used(r, endc + (endc < size_.cols ? 1 : 0));
            last_char_ = static_cast<char32_t>(ascii[i + take - 1]);
            i += take;
            cursor_.col = Col{col + static_cast<std::int32_t>(take)};
            ++generation_;
        }
        // Now cursor is at the right margin (or run exhausted). One more char
        // via put() writes the margin cell and sets wrap_pending_.
        if (i < n) put(static_cast<char32_t>(ascii[i++]));
    }
}

namespace {
// Append one codepoint to `out` as UTF-8.
void push_utf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
} // namespace

std::uint8_t Screen::intern_marks(const std::u32string &seq) {
    for (std::size_t i = 0; i < marks_.size(); ++i)
        if (marks_[i] == seq) return static_cast<std::uint8_t>(i + 1);
    if (marks_.size() >= 255) return 0; // table full; drop rather than corrupt
    marks_.push_back(seq);
    return static_cast<std::uint8_t>(marks_.size());
}

void Screen::attach_mark(char32_t cp) {
    // The mark belongs to the last glyph written. Normally that's the cell left
    // of the cursor; if a wrap is pending the cursor still sits on it, and a
    // wide glyph puts a spacer in between.
    std::int32_t col = wrap_pending_ ? cursor_.col.get() : cursor_.col.get() - 1;
    if (col < 0 || col >= size_.cols) return; // nothing written yet on this line
    Cell *row = ring_.view_row(cursor_.row.get());
    if (row[col].width == 0 && col > 0) --col; // step back over a wide spacer
    Cell &target = row[col];

    std::u32string seq{marks_of(target)};
    if (seq.size() >= 8) return; // sanity cap: no unbounded growth per cell
    seq.push_back(cp);
    const std::uint8_t idx = intern_marks(seq);
    if (idx != 0) {
        target.marks = idx;
        stamp(cursor_.row.get());
    }
}

void Screen::put(char32_t cp) {
    // Fast path: the overwhelmingly common case is a single-width printable that
    // index() + ONE stamp(), skipping the general machinery below. This is the
    // hot loop under any flood, so it's worth the special-case.
    if (charset_g0_ == Charset::Ascii && !charset_use_g1_ && !wrap_pending_ &&
        cp >= 0x20 && cp < 0x7f) {
        const std::int32_t col = cursor_.col.get();
        if (col < right_bound()) {
            // Room to write and advance without wrapping.
            const std::int32_t r = cursor_.row.get();
            stamp(r);
            *cell_ptr(cursor_.row, cursor_.col) = Cell{cp, pen_, 1, 0, cur_link_};
            ring_.note_used(r, col + 1);
            last_char_ = cp;
            cursor_.col = Col{col + 1};
            ++generation_;
            return;
        }
        // At/over the right margin: fall through to the wrapping logic.
    }

    cp = map_charset(cp); // VT100 line-drawing charset, if active
    const int w = char_width(cp);

    // Combining / zero-width marks attach to the preceding cell rather than
    // advancing the cursor, so "e" + U+0301 stays one cluster the host can
    // render as é. Dropping them (as we used to) silently mangles any accented
    // text that arrives decomposed.
    if (w == 0) {
        attach_mark(cp);
        touch();
        return;
    }

    if (wrap_pending_) {
        // The autowrap happens NOW, so this is when the row becomes a soft
        // wrap. Recording it when the last column was merely filled was
        // wrong: a CR, LF or cursor move cancels a pending wrap, and the row
        // was then hard-terminated but flagged wrapped. fish fills a row with
        // spaces after every command (its missing-newline marker) and then
        // CRs over it, so every prompt row was flagged and a resize rejoined
        // each prompt with the line below it (seen in hand as indented output
        // after splitting a pane).
        set_wrapped(cursor_.row.get(), true);
        cursor_.col = Col{left_bound()};
        line_feed();
        wrap_pending_ = false;
    }

    // A double-width glyph that won't fit before the right margin wraps to the
    // next line first (leaving the last column blank), as real terminals do.
    if (w == 2 && cursor_.col.get() + 1 > right_bound()) {
        if (autowrap_) {
            // Blank the final column, then wrap to the left margin. The row
            // MUST be marked soft-wrapped: it continues on the next line, and
            // without the flag reflow treated it as hard-terminated, so
            // rewrapping split the logical line and injected the unusable
            // padding column into the middle of it.
            at(cursor_.row, cursor_.col) = Cell{};
            set_wrapped(cursor_.row.get(), true);
            cursor_.col = Col{left_bound()};
            line_feed();
        } else {
            // No room and no wrap: overwrite in place as a single cell.
            at(cursor_.row, cursor_.col) = Cell{cp, pen_, 1, 0, cur_link_};
            touch();
            return;
        }
    }

    // Dissolve any wide pair the target cell(s) belong to before overwriting,
    // so a half-overwritten CJK/emoji never leaves an orphan (gap / stale glyph).
    // A wide write occupies two cells, so clean both.
    {
        const std::int32_t c0 = cursor_.col.get();
        clean_wide_at(cursor_.row, c0);
        if (w == 2) clean_wide_at(cursor_.row, c0 + 1);
    }
    at(cursor_.row, cursor_.col) =
        Cell{cp, pen_, static_cast<std::uint8_t>(w), 0, cur_link_};
    last_char_ = cp; // remember for REP (CSI Ps b)
    if (w == 2) {
        // The second half is a spacer the renderer skips. The base cell was just
        // written via at() (which stamped the row + bumped note_used to col+1);
        // write the spacer DIRECTLY through the row pointer and extend note_used
        // to col+2, avoiding a second at() (stamp + view_row) for the same row.
        const std::int32_t c0 = cursor_.col.get();
        cell_ptr(cursor_.row, Col{c0 + 1})[0] = Cell{U' ', pen_, 0, 0, cur_link_};
        ring_.note_used(cursor_.row.get(), c0 + 2);
        ring_.mark_view_wide(cursor_.row.get());   // the only writer of a width-2 cell
    }

    const int advance = w;
    if (cursor_.col.get() + advance > right_bound()) {
        if (autowrap_) {
            // The row is full; it soft-wraps only if another glyph follows
            // (see the wrap_pending_ branch above, which records it).
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
    if (id != 0 && id - 1u < links_.size()) return links_[id - 1u];
    // No explicit OSC 8 link: fall back to auto-detecting a bare URL under the
    // cursor by scanning the row's text. The result is cached in detected_url_
    // so the returned view stays valid until the next link_at call.
    return detect_url_at(vrow, col);
}

namespace {
// A codepoint that can appear in a URL body (RFC 3986 unreserved + sub-delims +
// path/query punctuation). Deliberately excludes whitespace and a few trailing
// punctuation chars handled separately.
bool url_body_cp(char32_t c) {
    if ((c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9'))
        return true;
    switch (c) {
    case U'-': case U'.': case U'_': case U'~': case U':': case U'/': case U'?':
    case U'#': case U'[': case U']': case U'@': case U'!': case U'$': case U'&':
    case U'\'': case U'(': case U')': case U'*': case U'+': case U',': case U';':
    case U'=': case U'%': case U'|':
        return true;
    default:
        return false;
    }
}
} // namespace

std::string_view Screen::detect_url_at(std::int32_t vrow, std::int32_t col) const noexcept {
    const auto cells = row(Row{vrow});
    if (!url_body_cp(cells[static_cast<std::size_t>(col)].cp)) return {};
    // Expand to the maximal URL-char run on this row.
    std::int32_t lo = col, hi = col;
    while (lo > 0 && url_body_cp(cells[static_cast<std::size_t>(lo - 1)].cp)) --lo;
    while (hi + 1 < size_.cols && url_body_cp(cells[static_cast<std::size_t>(hi + 1)].cp)) ++hi;

    // Build the run text.
    std::string run;
    run.reserve(static_cast<std::size_t>(hi - lo + 1));
    for (std::int32_t c = lo; c <= hi; ++c) {
        const char32_t cp = cells[static_cast<std::size_t>(c)].cp;
        if (cp == 0) break;
        // Encode back to UTF-8 (URLs are ASCII in practice, but be safe).
        if (cp < 0x80) run.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            run.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            run.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            run.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            run.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            run.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Require a known scheme prefix; find it within the run and clip to it (the
    // run may start before the scheme if surrounded by url_body punctuation).
    static constexpr std::string_view schemes[] = {"https://", "http://", "ftp://",
                                                    "file://", "mailto:"};
    std::size_t best = std::string::npos;
    for (auto sv : schemes) {
        const std::size_t p = run.find(sv);
        if (p != std::string::npos && p < best) best = p;
    }
    if (best == std::string::npos) return {};
    std::string_view url{run};
    url.remove_prefix(best);
    // Trim trailing punctuation that's usually sentence/markup, not URL.
    while (!url.empty()) {
        const char b = url.back();
        if (b == '.' || b == ',' || b == ';' || b == ':' || b == ')' || b == ']' ||
            b == '\'' || b == '!' || b == '?')
            url.remove_suffix(1);
        else
            break;
    }
    if (url.size() < 8) return {}; // shorter than "http://x" isn't worth it
    detected_url_.assign(url);
    return detected_url_;
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
    // CR returns to the left margin when the cursor is at or right of it, else
    // to column 0. Uses the margin itself, not the cursor-relative bound.
    cursor_.col = Col{(cursor_.col.get() >= margin_left()) ? margin_left() : 0};
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

    // Left/right margins active: scroll only the columns [left, right] within
    // the region, cell by cell. No scrollback in this mode (DEC semantics).
    if (lr_margins_ && (scroll_left_ > 0 || scroll_right_ < size_.cols - 1)) {
        const std::int32_t l = scroll_left_, r = scroll_right_;
        for (std::int32_t row = scroll_top_; row <= scroll_bottom_ - n; ++row)
            for (std::int32_t c = l; c <= r; ++c)
                copy_cell(row, c, row + n, c);
        for (std::int32_t row = scroll_bottom_ - n + 1; row <= scroll_bottom_; ++row)
            for (std::int32_t c = l; c <= r; ++c) at(Row{row}, Col{c}) = blank_cell();
        stamp_range(scroll_top_, scroll_bottom_);
        touch();
        return;
    }

    const bool full_screen = (scroll_top_ == 0 && scroll_bottom_ == size_.rows - 1);

    if (full_screen) {
        // THE zero-copy path. Each scrolled line's storage stays put in the
        // ring: the top visible row simply becomes the newest scrollback line
        // (primary buffer), or is recycled (alt screen), and the view slides
        // down by one — no row is copied. Only the new bottom row is blanked.
        const Cell blank = blank_cell();
        const bool keep = !on_alt_;
        for (std::int32_t i = 0; i < n; ++i) {
            // The cap spans BOTH parts of history. The ring is sized for the
            // whole cap, so while the log still holds rows, a pushed row
            // evicts the log's oldest one; once the log is empty the ring's
            // own recycling takes over, exactly as before.
            if (keep && log_.rows() > 0 && hist_rows() >= max_history_) log_.evict_rows(1);
            ring_.scroll_up_one(keep, blank);
        }
        // If the user is scrolled up reading history, KEEP THEIR VIEW ANCHORED
        // to the same content: each line pushed into scrollback would otherwise
        // slide what they're reading up by one row. Bump scroll_offset_ by the
        // rows added to history (clamped to the cap) so the viewport stays put
        // instead of creeping. At the live bottom (offset 0) we do nothing, so
        // normal output still follows the tail.
        if (keep && scroll_offset_ > 0) {
            scroll_offset_ = std::min(scroll_offset_ + n,
                                      static_cast<std::int32_t>(hist_rows()));
        }
        // Wrapped flags travel with the row storage inside the ring. line_attr_
        // tracks the VISIBLE grid by logical row, so it shifts up with content.
        if (any_line_attr_ && static_cast<std::int32_t>(line_attr_.size()) == size_.rows) {
            for (std::int32_t r = 0; r + n < size_.rows; ++r)
                line_attr_[static_cast<std::size_t>(r)] =
                    line_attr_[static_cast<std::size_t>(r + n)];
            for (std::int32_t r = std::max(0, size_.rows - n); r < size_.rows; ++r)
                line_attr_[static_cast<std::size_t>(r)] = LineAttr::normal;
        }
        stamp_all();
        touch();
        return;
    }

    // Restricted vertical region (top/bottom margins): scroll rows within the
    // region by cell copy. No scrollback (DEC: only a full-screen scroll feeds
    // scrollback). This is the rare case, so a straightforward copy is fine.
    const Cell blank = blank_cell();
    // Each row moves its used prefix only, and the vacated rows blank only
    // theirs (BCE marks them full): a region scroll is O(content), not
    // O(rows x cols).
    for (std::int32_t row = scroll_top_; row <= scroll_bottom_ - n; ++row) ring_.copy_view_row(row, row + n);
    for (std::int32_t row = scroll_bottom_ - n + 1; row <= scroll_bottom_; ++row) ring_.blank_view_row(row, blank);
    stamp_range(scroll_top_, scroll_bottom_);
    touch();
}

// Scroll the region down by n rows (reverse index / SD); blanks the top.
void Screen::scroll_down(std::int32_t n) {
    const std::int32_t region = scroll_bottom_ - scroll_top_ + 1;
    n = std::clamp(n, 0, region);
    if (n == 0) return;

    // Column-restricted scroll when left/right margins are active.
    if (lr_margins_ && (scroll_left_ > 0 || scroll_right_ < size_.cols - 1)) {
        const std::int32_t l = scroll_left_, r = scroll_right_;
        for (std::int32_t row = scroll_bottom_; row >= scroll_top_ + n; --row)
            for (std::int32_t c = l; c <= r; ++c)
                copy_cell(row, c, row - n, c);
        for (std::int32_t row = scroll_top_; row < scroll_top_ + n; ++row)
            for (std::int32_t c = l; c <= r; ++c) at(Row{row}, Col{c}) = blank_cell();
        stamp_range(scroll_top_, scroll_bottom_);
        touch();
        return;
    }

    // Rotate the region the other way: bottom n rows wrap to the top, top n
    // rows are blanked.
    ring_.rotate_view_down(scroll_top_, scroll_bottom_, n);
    for (std::int32_t r = scroll_top_; r < scroll_top_ + n; ++r)
        ring_.blank_view_row(r, blank_cell());
    stamp_range(scroll_top_, scroll_bottom_);
    touch();
}

void Screen::insert_lines(std::int32_t n) {
    // IL only acts inside the scroll region and only when the cursor is within.
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    const std::int32_t top = cursor_.row.get();
    // With left/right margins active, IL shifts ONLY the margin columns — the
    // cells outside them must not move. Rotating whole rows (the fast path
    // below) would drag the untouched columns along with them.
    if (lr_margins_ && (scroll_left_ > 0 || scroll_right_ < size_.cols - 1)) {
        const std::int32_t l = scroll_left_, r = scroll_right_;
        for (std::int32_t row = scroll_bottom_; row >= top + n; --row)
            for (std::int32_t c = l; c <= r; ++c)
                copy_cell(row, c, row - n, c);
        for (std::int32_t row = top; row < top + n && row <= scroll_bottom_; ++row)
            for (std::int32_t c = l; c <= r; ++c) at(Row{row}, Col{c}) = blank_cell();
        cursor_.col = Col{l};
        stamp_range(top, scroll_bottom_);
        touch();
        return;
    }
    // Rows [cursor, bottom-n] shift DOWN by n; the freshly-vacated top n rows
    // of the [cursor,bottom] sub-region are blanked.
    ring_.rotate_view_down(top, scroll_bottom_, n);
    for (std::int32_t r = top; r < top + n; ++r)
        ring_.blank_view_row(r, blank_cell());
    cursor_.col = Col{margin_left()};
    stamp_range(top, scroll_bottom_);
    touch();
}

std::int32_t Screen::collapse_blank_rows(std::int64_t from, std::int64_t to) {
    const auto base = static_cast<std::int64_t>(hist_rows());
    if (on_alt_ || to <= from || from < base) return 0;
    const auto r0 = static_cast<std::int32_t>(from - base), r1 = static_cast<std::int32_t>(to - base);
    if (r1 > cursor_.row.get()) return 0;
    for (std::int32_t r = r0; r < r1; ++r) {
        const Cell *row = ring_.view_row(r);
        for (std::int32_t c = 0; c < size_.cols; ++c)
            if (!row[c].blank()) return 0;
    }
    const Pos saved = cursor_;
    const std::int32_t top = scroll_top_, bottom = scroll_bottom_;
    const bool lr = lr_margins_;
    scroll_top_ = 0; scroll_bottom_ = size_.rows - 1; lr_margins_ = false;
    cursor_.row = Row{r0};
    delete_lines(r1 - r0);
    scroll_top_ = top; scroll_bottom_ = bottom; lr_margins_ = lr;
    cursor_ = saved;
    cursor_.row = Row{saved.row.get() - (r1 - r0)};
    return r1 - r0;
}

void Screen::delete_lines(std::int32_t n) {
    if (cursor_.row.get() < scroll_top_ || cursor_.row.get() > scroll_bottom_) return;
    n = std::clamp(n, 0, scroll_bottom_ - cursor_.row.get() + 1);
    if (n == 0) return;
    const std::int32_t top = cursor_.row.get();
    // Same margin rule as IL: only the margin columns shift.
    if (lr_margins_ && (scroll_left_ > 0 || scroll_right_ < size_.cols - 1)) {
        const std::int32_t l = scroll_left_, r = scroll_right_;
        for (std::int32_t row = top; row <= scroll_bottom_ - n; ++row)
            for (std::int32_t c = l; c <= r; ++c)
                copy_cell(row, c, row + n, c);
        for (std::int32_t row = scroll_bottom_ - n + 1; row <= scroll_bottom_; ++row)
            for (std::int32_t c = l; c <= r; ++c) at(Row{row}, Col{c}) = blank_cell();
        cursor_.col = Col{l};
        stamp_range(top, scroll_bottom_);
        touch();
        return;
    }
    // Rows [cursor+n, bottom] shift UP by n; the vacated bottom n rows blank.
    ring_.rotate_view_up(top, scroll_bottom_, n);
    for (std::int32_t r = scroll_bottom_ - n + 1; r <= scroll_bottom_; ++r)
        ring_.blank_view_row(r, blank_cell());
    cursor_.col = Col{margin_left()};
    stamp_range(top, scroll_bottom_);
    touch();
}

void Screen::insert_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    const std::int32_t right = margin_right();
    if (start < margin_left() || start > right) return; // outside the region
    n = std::clamp(n, 0, right + 1 - start);
    if (n == 0) return;
    // Shift cells [start, right-n] right by n within the margin; blank the gap.
    for (std::int32_t c = right; c >= start + n; --c) {
        copy_cell(r.get(), c, r.get(), c - n);
    }
    for (std::int32_t c = start; c < start + n; ++c) at(r, Col{c}) = blank_cell();
    touch();
}

void Screen::delete_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    const std::int32_t right = margin_right();
    if (start < margin_left() || start > right) return;
    n = std::clamp(n, 0, right + 1 - start);
    if (n == 0) return;
    for (std::int32_t c = start; c <= right - n; ++c) copy_cell(r.get(), c, r.get(), c + n);
    for (std::int32_t c = right - n + 1; c <= right; ++c) at(r, Col{c}) = blank_cell();
    touch();
}

void Screen::erase_chars(std::int32_t n) {
    const Row r = cursor_.row;
    const std::int32_t start = cursor_.col.get();
    n = std::clamp(n, 0, size_.cols - start); // ECH ignores margins (per spec)
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

void Screen::set_lr_margins(int left, int right) {
    // DECSLRM params are 1-based; 0/absent means the extreme columns.
    const int l = (left <= 0) ? 1 : left;
    const int r = (right <= 0) ? size_.cols : right;
    if (l < r && r <= size_.cols) {
        scroll_left_ = l - 1;
        scroll_right_ = r - 1;
        // "DECSLRM moves the cursor to column 1, line 1 of the page" (VT510
        // manual) — the PAGE origin, not the margin corner.
        cursor_ = Pos{Row{0}, Col{0}};
        wrap_pending_ = false;
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
    case 6: // DECOM — origin mode: CUP/HVP/VPA become relative to the scroll
            // region (and left/right margins), and the cursor cannot leave it.
            // Setting or resetting it homes the cursor, per DEC STD 070.
        origin_mode_ = set;
        cursor_ = Pos{Row{origin_mode_ ? scroll_top_ : 0},
                      Col{origin_mode_ ? margin_left() : 0}};
        wrap_pending_ = false;
        touch();
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
    case 69: // DECLRMM (DECVSSM) — enable/disable left+right margins.
        lr_margins_ = set;
        if (!set) { scroll_left_ = 0; scroll_right_ = size_.cols - 1; }
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
        case 6:    pv = b(origin_mode_); break;             // DECOM
        case 7:    pv = b(autowrap_); break;                 // DECAWM
        case 12:   pv = kReset; break;      // cursor blink: accepted, host-driven
        case 25:   pv = b(cursor_shown_); break;            // DECTCEM
        case 69:   pv = b(lr_margins_); break;              // DECLRMM
        case 1000: pv = b(mouse_mode_ == MouseMode::normal); break;
        case 1002: pv = b(mouse_mode_ == MouseMode::button); break;
        case 1003: pv = b(mouse_mode_ == MouseMode::any); break;
        case 1004: pv = b(focus_events_); break;            // focus reporting
        case 1006: pv = b(mouse_sgr_); break;
        case 47:                                            // legacy alt screen
        case 1047:                                          // alt screen, no clear
        case 1049: pv = b(on_alt_); break;                  // alt screen + cursor
        case 2004: pv = b(bracketed_paste_); break;
        case 2026: pv = b(sync_output_); break;             // synchronized output
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
    origin_mode_ = false; // DECOM is in DECSTR's reset list
    app_cursor_keys_ = false;
    app_keypad_ = false;
    charset_g0_ = charset_g1_ = Charset::Ascii;
    charset_use_g1_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = size_.rows - 1;
    scroll_left_ = 0;
    scroll_right_ = size_.cols - 1;
    lr_margins_ = false;
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
            at(Row{row}, Col{col}) = Cell{cp, pen_, w, 0, 0};
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
    // Which attributes this call touches, and whether it SETS or CLEARS them.
    // DECCARA takes SGR codes: 0 means "clear the listed attributes" (and with
    // no parameters at all, clear everything). ORing the mask for a 0 — which
    // is what this used to do — turned "reset the rectangle" into "set every
    // attribute in the rectangle", the exact opposite.
    constexpr Attr kAll = Attr::Bold | Attr::Underline | Attr::Blink | Attr::Reverse |
                          Attr::Hidden | Attr::Strike;
    Attr mask = Attr::None;
    bool clear = false;
    if (attrs.empty()) {
        mask = kAll;
        clear = true;
    } else {
        for (int code : attrs) {
            if (code == 0) {
                // A bare 0 resets everything; 0 alongside other codes still
                // means this call is a clear.
                mask |= kAll;
                clear = true;
            } else {
                mask |= bit(code);
            }
        }
    }

    for (int row = t; row <= b; ++row) {
        for (int col = l; col <= r; ++col) {
            Cell &cell = at(Row{row}, Col{col});
            if (reverse) cell.pen.attr ^= mask;      // DECRARA toggles
            else if (clear) cell.pen.attr &= ~mask;  // DECCARA with 0 / no params
            else cell.pen.attr |= mask;              // DECCARA sets
        }
    }
    touch();
}

void Screen::enter_alt_screen() {
    if (on_alt_) return;
    // O(1) buffer swap: move the primary (grid + scrollback) into alt_ring_ and
    // bring in a blank alt buffer — no per-cell copy. std::swap on RowRing is a
    // handful of vector swaps. The primary is preserved intact for leave().
    saved_primary_cursor_ = cursor_;
    std::swap(ring_, alt_ring_);
    // Bring the alt buffer up as a blank grid at the CURRENT dims (reset reuses
    // the existing allocation; cap is tiny — rows, no scrollback).
    ring_.reset(size_.rows, size_.cols, /*scroll=*/0, blank_cell());
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
    // Swap the primary buffer back in — O(1), and its scrollback + content are
    // exactly as they were on enter(). Stash the (now-inactive) alt buffer back
    // in alt_ring_ so its allocation is reused next time.
    std::swap(ring_, alt_ring_);
    // If the window was resized while on the alt screen, the restored primary
    // ring is at the OLD dims; reflow/reconcile it to the current size.
    if (ring_.rows() != size_.rows || ring_.cols() != size_.cols) {
        const Extent cur = size_;
        const Extent old{ring_.cols(), ring_.rows()};
        size_ = old;              // reflow() reads size_ as the OLD size
        reflow(old, cur);
        size_ = cur;
    }
    cursor_ = saved_primary_cursor_;
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

void Screen::move_cursor_vert(std::int32_t delta, bool to_first_col) {
    // CUU/CUD/CNL/CPL stop at the scroll region's edge when the cursor starts
    // INSIDE it (DEC STD 070). A cursor already outside the region is bounded
    // by the screen instead, so a region can never trap it.
    const std::int32_t r = cursor_.row.get();
    const bool inside = r >= scroll_top_ && r <= scroll_bottom_;
    const std::int32_t lo = inside ? scroll_top_ : 0;
    const std::int32_t hi = inside ? scroll_bottom_ : size_.rows - 1;
    wrap_pending_ = false;
    cursor_.row = Row{std::clamp(r + delta, lo, hi)};
    if (to_first_col) cursor_.col = Col{inside ? margin_left() : 0};
    touch();
}

void Screen::move_cursor_horiz(std::int32_t delta) {
    // CUF/CUB are bounded by the left/right margins when the cursor is within
    // them, and by the screen otherwise.
    const std::int32_t c = cursor_.col.get();
    const bool inside = c >= margin_left() && c <= margin_right();
    const std::int32_t lo = inside ? margin_left() : 0;
    const std::int32_t hi = inside ? margin_right() : size_.cols - 1;
    wrap_pending_ = false;
    cursor_.col = Col{std::clamp(c + delta, lo, hi)};
    touch();
}

void Screen::move_cursor_origin(Row r, Col c) {
    if (!origin_mode_) {
        move_cursor_abs(r, c);
        return;
    }
    // Origin mode: (0,0) is the top-left of the scroll region / left margin,
    // and the cursor is clamped INSIDE the region rather than to the screen.
    wrap_pending_ = false;
    cursor_.row = Row{std::clamp(scroll_top_ + r.get(), scroll_top_, scroll_bottom_)};
    cursor_.col = Col{std::clamp(margin_left() + c.get(), margin_left(), margin_right())};
    touch();
}

void Screen::erase_in_line(int mode) {
    const Row r = cursor_.row;
    const Cell b = blank_cell();
    switch (mode) {
    case 0: // cursor to end of line
        stamp(r.get());
        ring_.blank_view_span(r.get(), cursor_.col.get(), size_.cols, b);
        // The row's tail is now blank, so it can't continue onto the next
        // row. Shells redraw prompts with CR + EL over rows they filled.
        if (!wrap_pending_) set_wrapped(r.get(), false);
        break;
    case 1: // start of line to cursor
        stamp(r.get());
        ring_.blank_view_span(r.get(), 0, cursor_.col.get() + 1, b);
        break;
    case 2: // whole line
        stamp(r.get());
        ring_.blank_view_span(r.get(), 0, size_.cols, b);
        set_wrapped(r.get(), false); // an erased line no longer soft-wraps
        break;
    default: break;
    }
    touch();
}
void Screen::erase_in_display(int mode) {
    const Cell b = blank_cell();
    switch (mode) {
    case 0: // cursor to end of screen
        erase_in_line(0);
        for (std::int32_t rr = cursor_.row.get() + 1; rr < size_.rows; ++rr) {
            stamp(rr);
            ring_.blank_view_span(rr, 0, size_.cols, b);
            ring_.set_view_wrapped(rr, false);
        }
        break;
    case 1: // start of screen to cursor
        for (std::int32_t rr = 0; rr < cursor_.row.get(); ++rr) {
            stamp(rr);
            ring_.blank_view_span(rr, 0, size_.cols, b);
            ring_.set_view_wrapped(rr, false);
        }
        erase_in_line(1);
        break;
    case 2: // entire screen
    case 3:
        for (std::int32_t r = 0; r < size_.rows; ++r) {
            ring_.blank_view_row(r, b);
            ring_.set_view_wrapped(r, false);
        }
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
    case 'A': move_cursor_vert(-param_or(p, 0, 1), false); break;             // CUU
    case 'B': move_cursor_vert(param_or(p, 0, 1), false); break;              // CUD
    case 'C': move_cursor_horiz(param_or(p, 0, 1)); break;                    // CUF
    case 'D': move_cursor_horiz(-param_or(p, 0, 1)); break;                   // CUB
    case 'E': move_cursor_vert(param_or(p, 0, 1), true); break;               // CNL
    case 'F': move_cursor_vert(-param_or(p, 0, 1), true); break;              // CPL
    case 'G': move_cursor_abs(cursor_.row, Col{param_or(p, 0, 1) - 1}); break;      // CHA
    case 'H':
    case 'f': // CUP (1-based params)
        move_cursor_origin(Row{param_or(p, 0, 1) - 1}, Col{param_or(p, 1, 1) - 1});
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
    case 'd': // VPA — row only, so pass the current column through unchanged.
        if (origin_mode_) {
            wrap_pending_ = false;
            cursor_.row = Row{std::clamp(scroll_top_ + param_or(p, 0, 1) - 1,
                                         scroll_top_, scroll_bottom_)};
            touch();
        } else {
            move_cursor_abs(Row{param_or(p, 0, 1) - 1}, cursor_.col);
        }
        break;
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
        if (d.private_marker) break;
        if (lr_margins_) {
            // DECSLRM: CSI l ; r s -> set left/right margins (1-based).
            set_lr_margins(param_raw(p, 0, 0), param_raw(p, 1, 0));
        } else {
            save_cursor(); // ANSI.SYS save cursor (DECSC alias)
        }
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
        } else if (d.private_marker && d.marker == '=') {
            // DA3 (tertiary): a DECRPTUI unit id, as DCS ! | <hex> ST. The ID
            // is site-defined; report all-zero like xterm's default rather
            // than staying silent, since an app may block on the reply.
            reply("\x1bP!|00000000\x1b\\");
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
        if (const auto idx = c.as<IndexedColor>()) {
            const int i = idx->index;
            if (i < 8) add(std::to_string(base + i));
            else if (i < 16) add(std::to_string(base + 60 + (i - 8))); // bright
            else { add(std::to_string(base + 8)); add("5"); add(std::to_string(i)); }
        } else if (const auto tc = c.as<TrueColor>()) {
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
            // Blank the visible grid, then drop the scrollback in a way that no
            // stale (e.g. BCE-coloured) cell can resurface when slots are later
            // recycled — see RowRing::reset_scrollback. This is O(rows+cap), NOT
            // a full arena zero-fill: a RIS-heavy stream (mixednasty emits 800k
            // RIS) must not do O(cap*cols) work per reset.
            for (std::int32_t r = 0; r < size_.rows; ++r) {
                ring_.mark_view_full(r);              // clear the FULL width, not just used_
                ring_.blank_view_row(r, Cell{});      // with the DEFAULT cell (resets the
                ring_.set_view_wrapped(r, false);     // ring's cached blank from any BCE colour)
            }
            ring_.reset_scrollback();
            log_.clear();
            scroll_offset_ = 0;
            cursor_ = Pos{};
            pen_ = Pen{};
            // The side tables are only reachable from cells, and every cell is
            // now blank, so drop them instead of leaking across a reset.
            marks_.clear();
            links_.clear();
            cur_link_ = 0;
            cur_link_key_.clear();
            hover_link_ = 0;
            wrap_pending_ = false;
            origin_mode_ = false;
            scroll_top_ = 0;
            scroll_bottom_ = size_.rows - 1;
            scroll_left_ = 0;
            scroll_right_ = size_.cols - 1;
            lr_margins_ = false;
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
        case '6': // DECBI — back index: cursor left, or shift content right at
                  // the left margin (a new blank column opens at the left).
            if (cursor_.col.get() <= margin_left()) {
                const std::int32_t l = margin_left(), r = margin_right();
                for (std::int32_t row = scroll_top_; row <= scroll_bottom_; ++row) {
                    for (std::int32_t c = r; c > l; --c) copy_cell(row, c, row, c - 1);
                    at(Row{row}, Col{l}) = blank_cell();
                }
                touch();
            } else {
                cursor_.col = Col{cursor_.col.get() - 1};
            }
            return;
        case '9': // DECFI — forward index: cursor right, or shift content left at
                  // the right margin (a new blank column opens at the right).
            if (cursor_.col.get() >= margin_right()) {
                const std::int32_t l = margin_left(), r = margin_right();
                for (std::int32_t row = scroll_top_; row <= scroll_bottom_; ++row) {
                    for (std::int32_t c = l; c < r; ++c) copy_cell(row, c, row, c + 1);
                    at(Row{row}, Col{r}) = blank_cell();
                }
                touch();
            } else {
                cursor_.col = Col{cursor_.col.get() + 1};
            }
            return;
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
                    at(Row{row}, Col{col}) = Cell{U'E', Pen{}, 1, 0, 0};
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
            if (a != LineAttr::normal) any_line_attr_ = true;
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
                // Extended colour: 38/48/58 with an indexed (5;n) or direct
                // (2;r;g;b) selector. 58 is the underline colour.
                //
                // Two wire forms, and they are NOT the same length:
                //   semicolon: 38;2;R;G;B        (legacy xterm)
                //   colon:     38:2::R:G:B       (ECMA-48; kitty, neovim, iTerm2)
                // The colon form carries an extra colour-space-id slot after
                // the 2, which is normally empty. Treating the two alike read
                // the colour one position early, so 38:2::1:2:3 came out as
                // rgb(0,1,2). `sub[k]` is non-zero when params[k] arrived as a
                // colon subparameter, which is how we tell them apart.
                Color *target = (c == 38) ? &pen_.fg : (c == 48) ? &pen_.bg : &pen_.underline_color;
                const auto is_sub = [&](std::size_t k) {
                    return k < sub.size() && sub[k] != 0;
                };
                // A colon-form sequence has its selector as a subparameter.
                const bool colon = is_sub(i + 1);
                if (i + 1 < params.size() && params[i + 1] == 5) {
                    // Indexed: 38;5;n or 38:5:n -- same shape either way.
                    if (i + 2 < params.size()) {
                        *target = IndexedColor{static_cast<std::uint8_t>(params[i + 2])};
                        i += 2;
                    }
                } else if (i + 1 < params.size() && params[i + 1] == 2) {
                    // Direct colour. The colon form inserts a colour-space id;
                    // skip it when the parameter count shows it's there.
                    std::size_t r = i + 2;
                    if (colon && i + 5 < params.size()) r = i + 3; // skip colour-space
                    if (r + 2 < params.size()) {
                        *target = TrueColor{Rgb{static_cast<std::uint8_t>(params[r]),
                                                static_cast<std::uint8_t>(params[r + 1]),
                                                static_cast<std::uint8_t>(params[r + 2])}};
                        i = r + 2;
                    }
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
    const std::int64_t live_top = static_cast<std::int64_t>(hist_rows());
    return live_top - scroll_offset_ + vrow;
}

const Cell *Screen::cell_at_abs(std::int64_t abs_row, std::int32_t col) const noexcept {
    if (col < 0 || col >= size_.cols) return nullptr;
    if (abs_row < 0 || abs_row >= total_rows()) return nullptr;
    const auto lr = static_cast<std::int64_t>(log_rows());
    if (abs_row < lr) return log_row(abs_row).cells.data() + col;
    return ring_.abs_row(static_cast<std::size_t>(abs_row - lr)) + col;
}

void Screen::selection_begin(AbsPos p, SelectMode mode) {
    sel_mode_ = mode;
    sel_anchor_ = p;
    sel_active_ = p;
    sel_grain_ = Grain::cell;
    sel_pivot_lo_ = p;
    sel_pivot_hi_ = p;
    touch();
}

void Screen::selection_extend(AbsPos p) {
    if (sel_mode_ == SelectMode::none) return;

    // Cell granularity (plain drag): the anchor stays put, the free end tracks
    // the pointer exactly.
    if (sel_grain_ == Grain::cell || sel_mode_ == SelectMode::block) {
        sel_active_ = p;
        touch();
        return;
    }

    // Word/line granularity (drag after a double/triple click): snap the span so
    // it always covers the ORIGINAL clicked word/line PLUS the word/line under
    // the pointer, whichever side the pointer is on. This is the iTerm2/kitty
    // feel where sweeping selects whole words at a time.
    std::pair<AbsPos, AbsPos> tgt =
        (sel_grain_ == Grain::word) ? word_bounds_at(p) : line_bounds_at(p.row);

    // Union the pivot span with the target span, then orient anchor/active so
    // the anchor is the edge farther from the pointer (keeps drag reversible).
    const bool forward = !(p < sel_pivot_lo_); // pointer at/after the pivot start
    if (forward) {
        sel_anchor_ = sel_pivot_lo_;
        sel_active_ = (sel_pivot_hi_ < tgt.second) ? tgt.second : sel_pivot_hi_;
    } else {
        sel_anchor_ = sel_pivot_hi_;
        sel_active_ = (tgt.first < sel_pivot_lo_) ? tgt.first : sel_pivot_lo_;
    }
    touch();
}

namespace {
// Character class for double-click word selection. State-of-the-art terminals
// treat a "word" as a maximal run of codepoints of the SAME class, so that a
// boundary falls between e.g. CJK text and ASCII punctuation, or between a
// number and a symbol — not "everything ≥ 0x80 is one blob".
enum class CharClass { space, word, wide, punct };

// Codepoints that glue a word together even though ASCII would call them
// punctuation — the path/URL/identifier set. Double-clicking a file path or a
// URL grabs the whole thing.
constexpr bool is_word_punct(char32_t cp) {
    switch (cp) {
    case U'_': case U'-': case U'.': case U'/': case U'~': case U':':
    case U'@': case U'+': case U'=': case U'%': case U'#':
        return true;
    default:
        return false;
    }
}

// True for CJK / fullwidth ideographic ranges where every glyph is its own
// "word" (double-click selects one character, matching native editors).
constexpr bool is_ideographic(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x2FDF) ||   // CJK radicals / Kangxi
           (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana + Katakana
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compatibility
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth forms
           (cp >= 0x20000 && cp <= 0x3FFFD);   // CJK Ext B..
}

CharClass classify(char32_t cp, std::u32string_view extra) {
    if (cp == 0 || cp == U' ' || cp == U'\t' || cp == 0xA0) return CharClass::space;
    if (is_ideographic(cp)) return CharClass::wide;
    if ((cp >= U'0' && cp <= U'9') || (cp >= U'A' && cp <= U'Z') ||
        (cp >= U'a' && cp <= U'z') || is_word_punct(cp)) {
        return CharClass::word;
    }
    // User-configured joiners are word-class.
    if (extra.find(cp) != std::u32string_view::npos) return CharClass::word;
    // Remaining non-ASCII (accented letters, symbols in other scripts) count as
    // word too — they're overwhelmingly letters; ASCII punctuation stays punct.
    if (cp >= 0x80) return CharClass::word;
    return CharClass::punct;
}
} // namespace


void Screen::selection_word(AbsPos p) {
    const Cell *hit = cell_at_abs(p.row, p.col);
    if (!hit || classify(hit->cp, word_extra_) == CharClass::space) {
        // Not on a word: fall back to a single-cell character selection, but
        // still arm word-granularity drag so sweeping picks up whole words.
        selection_begin(p, SelectMode::character);
        sel_grain_ = Grain::word;
        return;
    }
    const auto [lo, hi] = word_bounds_at(p);
    sel_mode_ = SelectMode::character;
    sel_anchor_ = lo;
    sel_active_ = hi;
    sel_grain_ = Grain::word;
    sel_pivot_lo_ = lo;
    sel_pivot_hi_ = hi;
    touch();
}

void Screen::selection_line(AbsPos p) {
    const auto [lo, hi] = line_bounds_at(p.row);
    sel_mode_ = SelectMode::line;
    sel_anchor_ = lo;
    sel_active_ = hi;
    sel_grain_ = Grain::line;
    sel_pivot_lo_ = lo;
    sel_pivot_hi_ = hi;
    touch();
}

// Soft-wrap-aware word span. Walks left/right across the row, and — when the
// row is a soft-wrap continuation and the run reaches a physical edge — carries
// on into the adjacent physical row so a wrapped word or URL selects whole.
std::pair<Screen::AbsPos, Screen::AbsPos> Screen::word_bounds_at(AbsPos p) const noexcept {
    const Cell *hit = cell_at_abs(p.row, p.col);
    if (!hit) return {p, p};
    const CharClass want = classify(hit->cp, word_extra_);
    if (want == CharClass::space) return {p, p};

    // Each ideograph/fullwidth glyph is its own "word" (matches native editors
    // and CJK IME behaviour): select just the lead cell + its spacer.
    if (want == CharClass::wide) {
        AbsPos lo{p.row, p.col};
        // Land on the lead cell if the click hit a wide spacer.
        if (hit->width == 0 && lo.col > 0) --lo.col;
        AbsPos hi = lo;
        const Cell *sp = cell_at_abs(hi.row, hi.col + 1);
        if (sp && sp->width == 0 && sp->cp == U' ' && hi.col + 1 <= size_.cols - 1) ++hi.col;
        return {lo, hi};
    }

    auto same = [&](std::int64_t r, std::int32_t c) {
        const Cell *cc = cell_at_abs(r, c);
        // A wide glyph's trailing spacer (width 0, cp space) belongs to the
        // lead cell — treat it as continuing the same word, not a break.
        if (cc && cc->width == 0 && cc->cp == U' ') return true;
        return cc && classify(cc->cp, word_extra_) == want;
    };

    const std::int64_t total = total_rows();
    const std::int32_t last = size_.cols - 1;

    AbsPos lo{p.row, p.col};
    while (true) {
        if (lo.col > 0) {
            if (!same(lo.row, lo.col - 1)) break;
            --lo.col;
        } else if (lo.row > 0 && wrapped_abs(lo.row - 1)) {
            // Previous physical row soft-wrapped INTO this one: continue there.
            if (!same(lo.row - 1, last)) break;
            lo = AbsPos{lo.row - 1, last};
        } else {
            break;
        }
    }

    AbsPos hi{p.row, p.col};
    while (true) {
        if (hi.col < last) {
            if (!same(hi.row, hi.col + 1)) break;
            ++hi.col;
        } else if (hi.row + 1 < total &&
                   wrapped_abs(hi.row)) {
            // This row soft-wraps into the next: continue at its column 0.
            if (!same(hi.row + 1, 0)) break;
            hi = AbsPos{hi.row + 1, 0};
        } else {
            break;
        }
    }
    return {lo, hi};
}

// The full logical line: extend up while the previous row wrapped into this one
// and down while this row wraps into the next, so triple-click grabs a wrapped
// command line as one unit.
std::pair<Screen::AbsPos, Screen::AbsPos> Screen::line_bounds_at(std::int64_t abs_row) const noexcept {
    const std::int64_t total = total_rows();
    if (total == 0) return {AbsPos{abs_row, 0}, AbsPos{abs_row, size_.cols - 1}};
    std::int64_t top = abs_row, bot = abs_row;
    while (top > 0 && wrapped_abs(top - 1)) --top;
    while (bot + 1 < total && wrapped_abs(bot)) ++bot;
    return {AbsPos{top, 0}, AbsPos{bot, size_.cols - 1}};
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
            push_utf8(line, cp);
            if (cell)
                for (char32_t mk : marks_of(*cell)) push_utf8(line, mk);
        }
        // Trim trailing spaces on each line — but NOT when this row soft-wraps
        // into the next and we're copying through its right edge in character
        // mode: there the line logically continues, so trailing padding (if
        // any) is kept minimal and we suppress the newline below.
        const bool soft_wrap_join =
            sel_mode_ != SelectMode::block && r < b.row &&
            end >= size_.cols - 1 &&
            r >= 0 && r < total_rows() &&
            wrapped_abs(r);
        // Trim trailing blanks on a real (hard-terminated) line; a soft-wrapped
        // row keeps them — the space at the wrap point is a genuine word gap and
        // dropping it would glue two words together on paste.
        if (!soft_wrap_join)
            while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        // Emit a newline only at a REAL line break. A soft-wrapped row joins the
        // next with no separator, so a wrapped URL/command pastes as one line.
        if (r != b.row && !soft_wrap_join) out.push_back('\n');
    }
    return out;
}

std::int64_t Screen::total_rows() const noexcept {
    return static_cast<std::int64_t>(log_rows() + ring_.total());
}

// --- scrollback search -----------------------------------------------------
namespace {
char32_t lower_cp(char32_t c) {
    if (c >= U'A' && c <= U'Z') return c + 32;
    // Latin-1 supplement upper -> lower (covers accented Latin common in logs).
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}
} // namespace

std::size_t Screen::search(std::string_view query, bool case_sensitive) {
    // Decode the UTF-8 query to codepoints (reuse the word-sep decoder shape).
    std::u32string q;
    for (std::size_t i = 0; i < query.size();) {
        const auto b0 = static_cast<unsigned char>(query[i]);
        char32_t cp = 0; int n = 1;
        if (b0 < 0x80) { cp = b0; n = 1; }
        else if ((b0 >> 5) == 0x6) { cp = b0 & 0x1F; n = 2; }
        else if ((b0 >> 4) == 0xE) { cp = b0 & 0x0F; n = 3; }
        else if ((b0 >> 3) == 0x1E) { cp = b0 & 0x07; n = 4; }
        else { ++i; continue; }
        if (i + static_cast<std::size_t>(n) > query.size()) break;
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const auto bc = static_cast<unsigned char>(query[i + static_cast<std::size_t>(k)]);
            if ((bc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (bc & 0x3F);
        }
        if (ok) q.push_back(case_sensitive ? cp : lower_cp(cp));
        i += static_cast<std::size_t>(n);
    }

    // Remember the current match's row so we can keep the cursor near it.
    const std::int64_t anchor_row =
        search_matches_.empty() ? viewport_to_abs(0)
                                : search_matches_[search_cur_].start.row;
    search_matches_.clear();
    search_query_ = q;
    search_case_ = case_sensitive;
    if (q.empty()) { search_cur_ = 0; touch(); return 0; }

    const std::int64_t total = total_rows();
    const std::int32_t cols = size_.cols;
    const std::size_t qn = q.size();
    // Linear scan. A match may not cross a row boundary (matches the visible
    // grid; soft-wrapped continuations are separate physical rows, which is the
    // pragmatic behaviour every terminal ships).
    for (std::int64_t r = 0; r < total; ++r) {
        for (std::int32_t c = 0; c + static_cast<std::int32_t>(qn) <= cols; ++c) {
            bool hit = true;
            for (std::size_t k = 0; k < qn; ++k) {
                const Cell *cell = cell_at_abs(r, c + static_cast<std::int32_t>(k));
                char32_t cc = cell ? cell->cp : U' ';
                if (!case_sensitive) cc = lower_cp(cc);
                if (cc != q[k]) { hit = false; break; }
            }
            if (hit) {
                search_matches_.push_back({AbsPos{r, c}, static_cast<std::int32_t>(qn)});
                c += static_cast<std::int32_t>(qn) - 1; // non-overlapping
            }
        }
    }

    // Pick the current match: the first at/after the anchor row, else the last.
    search_cur_ = 0;
    for (std::size_t i = 0; i < search_matches_.size(); ++i) {
        if (search_matches_[i].start.row >= anchor_row) { search_cur_ = i; break; }
        search_cur_ = i;
    }
    if (!search_matches_.empty()) scroll_to_abs_row(search_matches_[search_cur_].start.row);
    touch();
    return search_matches_.size();
}

void Screen::search_next() {
    if (search_matches_.empty()) return;
    search_cur_ = (search_cur_ + 1) % search_matches_.size();
    scroll_to_abs_row(search_matches_[search_cur_].start.row);
    touch();
}

void Screen::search_prev() {
    if (search_matches_.empty()) return;
    search_cur_ = (search_cur_ + search_matches_.size() - 1) % search_matches_.size();
    scroll_to_abs_row(search_matches_[search_cur_].start.row);
    touch();
}

void Screen::search_clear() {
    if (search_matches_.empty() && search_query_.empty()) return;
    search_matches_.clear();
    search_query_.clear();
    search_cur_ = 0;
    touch();
}

bool Screen::is_search_match(std::int64_t abs_row, std::int32_t col) const noexcept {
    for (const auto &m : search_matches_) {
        if (m.start.row != abs_row) continue;
        if (col >= m.start.col && col < m.start.col + m.len) return true;
    }
    return false;
}

bool Screen::is_current_search_match(std::int64_t abs_row, std::int32_t col) const noexcept {
    if (search_matches_.empty()) return false;
    const auto &m = search_matches_[search_cur_];
    return m.start.row == abs_row && col >= m.start.col && col < m.start.col + m.len;
}

std::string Screen::text_between_abs(std::int64_t row0, std::int64_t row1,
                                     std::int32_t col0) const {
    const std::int64_t total = total_rows();
    if (row0 < 0) row0 = 0;
    if (row1 > total) row1 = total;
    if (row1 <= row0) return {};

    std::string out;
    for (std::int64_t r = row0; r < row1; ++r) {
        const std::int32_t start = (r == row0) ? col0 : 0;
        std::string line;
        for (std::int32_t c = start; c < size_.cols; ++c) {
            const Cell *cell = cell_at_abs(r, c);
            // Skip the trailing spacer cell of a wide (CJK/emoji) glyph.
            if (cell && cell->width == 0 && cell->cp == U' ') continue;
            const char32_t cp = cell ? cell->cp : U' ';
            push_utf8(line, cp);
            if (cell)
                for (char32_t mk : marks_of(*cell)) push_utf8(line, mk);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (r + 1 < row1) out.push_back('\n');
    }
    // Trim trailing empty lines (unwritten output rows read as blanks).
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

} // namespace toe::term
