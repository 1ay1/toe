// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Scrollback — the logical-line history store.
//
// The scrollback is the terminal's long tail: potentially millions of lines the
// app has printed, of which only a viewport-sized window is ever on screen. The
// crucial property a good terminal needs: a RESIZE or FONT CHANGE must not touch
// this store at all — it must be O(viewport), not O(history) — and it must never
// lose data or move the user's scroll position.
//
// The design that delivers all three:
//
//   * Lines are stored LOGICAL and WIDTH-FREE. A "logical line" is exactly the
//     run of cells the app wrote before a hard newline — no column count baked
//     in. Soft-wrap continuations that scroll off the live grid are JOINED back
//     into one logical line here, so history holds truth, not a width-specific
//     projection of it.
//
//   * Cells live in ONE flat arena (std::vector<Cell>); each line is an (offset,
//     length) span into it. Appending a line is a memcpy into the arena tail +
//     one span push — no per-line allocation, cache-friendly, ultra-fast.
//
//   * The arena is a RING capped at `max_cells`: once full, appending trims the
//     oldest lines (advancing a logical `base_`), so memory is bounded and the
//     oldest history falls off first — no compaction pass, no realloc.
//
//   * WRAPPING TO A WIDTH IS COMPUTED ON DEMAND, never stored. `wrap_rows(line,
//     cols)` says how many physical rows a logical line occupies at `cols`;
//     `project(line, wrap_row, cols, out)` fills one physical row. A tiny LRU
//     cache of recently projected (line -> row-count) keeps scrolling and
//     re-rendering at the same width O(1). Because nothing width-specific is
//     stored, a resize just changes `cols` — the store is untouched.
//
// Ownership: the Screen appends whole logical lines as they leave the live grid,
// and reads them back (projected to the current width) when the user scrolls
// into history. The live grid itself stays a physical RowRing — every VT op
// (cursor addressing, scroll regions, SGR) keeps working unchanged.

#ifndef TOE_TERM_SCROLLBACK_HPP
#define TOE_TERM_SCROLLBACK_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "toe/term/cell.hpp"

namespace toe::term {

class Scrollback {
public:
    // `max_lines` caps the number of logical lines retained; `max_cells` caps
    // the flat arena (defends against a few pathologically long lines eating
    // unbounded memory). Either 0 disables that cap.
    explicit Scrollback(std::size_t max_lines = 10000,
                        std::size_t max_cells = std::size_t{8} << 20 /* 8Mi cells ~192MiB */)
        : max_lines_(max_lines), max_cells_(max_cells) {}

    // Number of logical lines currently retained.
    [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
    [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }

    void set_max_lines(std::size_t n) noexcept { max_lines_ = n; trim(); }
    [[nodiscard]] std::size_t max_lines() const noexcept { return max_lines_; }

    void clear() noexcept {
        cells_.clear();
        lines_.clear();
        arena_head_ = 0;
    }

    // Append one logical line: the cells [data, data+len). Trailing blank cells
    // are dropped (a hard line has no meaningful trailing blanks); the empty
    // line is legal (len 0). O(len) memcpy + O(1) bookkeeping.
    void push_line(const Cell *data, std::size_t len) {
        // Trim trailing blanks — width-free storage keeps only real content.
        while (len > 0 && data[len - 1].blank()) --len;

        // Ring-arena: if appending would overflow max_cells_, drop oldest lines
        // until it fits (or compact when the head has drifted far).
        if (max_cells_ && len <= max_cells_) {
            while (!lines_.empty() && used_cells() + len > max_cells_) drop_oldest();
        }

        const std::size_t off = cells_.size();
        cells_.insert(cells_.end(), data, data + len);
        lines_.push_back(Line{off, static_cast<std::uint32_t>(len)});

        if (max_lines_ && lines_.size() > max_lines_) drop_oldest();

        // Periodic compaction: when dropped lines have left a big dead prefix in
        // the arena, reclaim it in one shift so cells_ doesn't grow unbounded.
        if (arena_head_ > 0 && arena_head_ * 2 >= cells_.size()) compact();
    }

    // Cells of logical line `i` (0 = oldest retained). Valid until the next
    // push/clear. May be empty.
    [[nodiscard]] std::span<const Cell> line(std::size_t i) const noexcept {
        const Line &l = lines_[i];
        return {cells_.data() + l.off, l.len};
    }
    [[nodiscard]] std::uint32_t line_len(std::size_t i) const noexcept { return lines_[i].len; }

    // How many physical rows logical line `i` occupies at width `cols`.
    // A width-0 (empty) line still occupies one row. O(1).
    [[nodiscard]] std::int32_t wrap_rows(std::size_t i, std::int32_t cols) const noexcept {
        if (cols <= 0) return 1;
        const std::uint32_t len = lines_[i].len;
        if (len == 0) return 1;
        return static_cast<std::int32_t>((len + static_cast<std::uint32_t>(cols) - 1) /
                                         static_cast<std::uint32_t>(cols));
    }

    // Fill `out` (>= cols cells) with physical row `wrap_row` of logical line
    // `i`, projected at width `cols`. Blank-pads the tail. Returns true if this
    // physical row is soft-wrapped (a continuation follows). O(cols).
    bool project(std::size_t i, std::int32_t wrap_row, std::int32_t cols, Cell *out,
                 const Cell &blank) const noexcept {
        for (std::int32_t c = 0; c < cols; ++c) out[c] = blank;
        const Line &l = lines_[i];
        const std::size_t start = static_cast<std::size_t>(wrap_row) * static_cast<std::size_t>(cols);
        if (start >= l.len) return false;
        const std::size_t avail = l.len - start;
        const std::size_t take = avail < static_cast<std::size_t>(cols)
                                     ? avail
                                     : static_cast<std::size_t>(cols);
        const Cell *src = cells_.data() + l.off + start;
        for (std::size_t c = 0; c < take; ++c) out[c] = src[c];
        return (start + take) < l.len; // more cells => this row soft-wraps
    }

    // Total physical rows the whole scrollback occupies at width `cols`. Cached
    // per width so repeated queries (scroll clamp, viewport math) are O(1) after
    // the first. The cache invalidates when `cols` changes or lines are added/
    // dropped (tracked by an epoch).
    [[nodiscard]] std::int64_t total_rows(std::int32_t cols) const noexcept {
        if (cols == cache_cols_ && epoch_ == cache_epoch_) return cache_total_rows_;
        std::int64_t total = 0;
        for (const Line &l : lines_)
            total += (l.len == 0)
                         ? 1
                         : (l.len + static_cast<std::uint32_t>(cols) - 1) / static_cast<std::uint32_t>(cols);
        cache_cols_ = cols;
        cache_epoch_ = epoch_;
        cache_total_rows_ = total;
        return total;
    }

private:
    struct Line {
        std::size_t off;   // start offset into cells_
        std::uint32_t len; // logical length (cells)
    };

    [[nodiscard]] std::size_t used_cells() const noexcept { return cells_.size() - arena_head_; }

    void drop_oldest() noexcept {
        if (lines_.empty()) return;
        arena_head_ = lines_.front().off + lines_.front().len; // dead prefix grows
        lines_.erase(lines_.begin());
        ++epoch_;
    }

    // Reclaim the dead prefix [0, arena_head_) in one shift; rebase offsets.
    void compact() {
        if (arena_head_ == 0) return;
        cells_.erase(cells_.begin(), cells_.begin() + static_cast<std::ptrdiff_t>(arena_head_));
        for (Line &l : lines_) l.off -= arena_head_;
        arena_head_ = 0;
        ++epoch_;
    }

    void trim() noexcept {
        if (!max_lines_) return;
        while (lines_.size() > max_lines_) drop_oldest();
    }

    std::vector<Cell> cells_;     // flat arena (ring: dead prefix at front)
    std::vector<Line> lines_;     // one span per logical line, oldest first
    std::size_t arena_head_ = 0;  // start of live data in cells_ (dead prefix len)
    std::size_t max_lines_;
    std::size_t max_cells_;
    std::uint64_t epoch_ = 0;     // bumps on any structural change (for caches)

    // total_rows cache
    mutable std::int32_t cache_cols_ = -1;
    mutable std::uint64_t cache_epoch_ = ~0ull;
    mutable std::int64_t cache_total_rows_ = 0;
};

} // namespace toe::term

#endif // TOE_TERM_SCROLLBACK_HPP
