// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::term::LineLog -- scrollback as LOGICAL lines, physical rows a view.
//
// A grid of fixed-width rows ties every history cell to one width, so a
// column change re-wraps and moves all of them: O(history cells) -- ~25 MB
// for a 10k-line scrollback, every resize step. But history is immutable
// once it scrolls off the screen. So here it is held as logical lines (soft-
// wrapped runs joined, trailing blanks trimmed) and "row k at width W" is
// answered from an index over the lines:
//
//     start[i] = rows of lines before i at W     (prefix sums, one pass)
//     row k    = line upper_bound(start, k) - 1, its row k - start[i]
//
// A width change touches no cell: one pass reading each line's row-boundary
// cells (a double-width glyph never splits across rows, so boundaries are
// walked, O(rows)), and a renderer reads only the rows it shows. Resize is
// O(screen + rows), never O(history cells).
//
// ZERO-COPY in: when the terminal resizes, the rows that scrolled into the
// row ring since the last resize are not copied here -- the ring's arena is
// FROZEN (shared, immutable) and each line references its rows in place
// (row-major, stride = the old width). Only a line that cannot be expressed
// that way (wide-glyph padding mid-line, or one joining two arenas) is
// copied into the log's own arena.
//
// Exactness: every row reads back as re-wrapping its line would paint it
// (take cells, Cell{} padding, soft flag on all rows but a closed line's
// last). tests/reflow_oracle_test.cpp pins Screen bit for bit against the
// grid-based reference.

#ifndef TOE_TERM_LINELOG_HPP
#define TOE_TERM_LINELOG_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "toe/term/cell.hpp"
#include "toe/term/rowring.hpp"

namespace toe::term {

class LineLog {
public:
    /// A row ring's arena, frozen: immutable, shared by the lines on it.
    struct Frozen {
        const Cell *data = nullptr;   // the arena's cells (valid from the start; owned below once handed over)
        CellArena owner;
        std::size_t cap = 1;
        std::int32_t cols = 0;
    };
    using FrozenPtr = std::shared_ptr<const Frozen>;

    /// The take of the row starting at `at` of a `len`-cell line at width
    /// `w`: w cells, one fewer if that would split a double-width glyph.
    template <class At>
    static std::uint32_t take_at(At cell, std::uint32_t len, std::uint32_t at, std::int32_t w) noexcept {
        std::uint32_t take = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(w, 1)), len - at);
        if (take > 1 && cell(at + take - 1).width == 2) --take;
        if (take == 0) take = std::min<std::uint32_t>(1u, len - at);
        return take;
    }

    // ── building ────────────────────────────────────────────────────────────

    /// Append a new line of copied cells. Returns its id. `wide`: it may
    /// hold a double-width glyph (rows may then end a column early: walked);
    /// false promises it holds none, so rows are plain ceil(len / W).
    std::uint64_t push_owned(const Cell *c, std::uint32_t n, bool open, bool wide = true) {
        Line ln;
        ln.off = put_owned(c, n);
        ln.len = n;
        ln.wide = wide;
        return push_line(std::move(ln), open);
    }
    /// Append a new line referencing frozen rows in place: `len` cells laid
    /// row-major from slot `slot` (wrapping at the arena's end).
    std::uint64_t push_inplace(const FrozenPtr &f, std::size_t slot, std::uint32_t len, bool open, bool wide = true) {
        Line ln;
        ln.chunk = f;
        ln.off = slot;
        ln.len = len;
        ln.wide = wide;
        return push_line(std::move(ln), open);
    }
    /// Append cells to the open last line (it moves into the log's own
    /// arena if it was in place or not last there).
    void extend_open(const Cell *c, std::uint32_t n, bool still_open) {
        Line &ln = lines_.back();
        own(ln);
        if (ln.off + ln.len != base_ + cells_.size()) {   // not at the end: move it there
            const std::uint64_t at = base_ + cells_.size();
            cells_.insert(cells_.end(), cells_.begin() + static_cast<std::ptrdiff_t>(ln.off - base_),
                          cells_.begin() + static_cast<std::ptrdiff_t>(ln.off - base_ + ln.len));
            dead_ += ln.len;
            ln.off = at;
        }
        cells_.insert(cells_.end(), c, c + n);
        ln.len += n;
        ln.wide = true;
        open_ = still_open;
        dirty();
    }

    [[nodiscard]] bool open() const noexcept { return open_ && front_ < lines_.size(); }
    /// The open last line's cells, removed from the log (a resize rejoins it
    /// with the grid's rows). Its id is what its cells keep as a line.
    std::uint64_t take_open(std::vector<Cell> &out) {
        const Line &ln = lines_.back();
        const std::uint64_t id = first_id_ + lines_.size() - 1;
        const std::size_t at = out.size();
        out.resize(at + ln.len);
        copy(ln, 0, ln.len, out.data() + at);
        drop_back();
        open_ = false;
        dirty();
        return id;
    }

    void clear() {
        first_id_ += lines_.size();
        lines_.clear();
        start_.clear();
        cells_.clear();
        base_ = 0;
        dead_ = 0;
        front_ = 0;
        head_rows_ = 0;
        open_ = false;
        total_ = 0;
        w_ = 0;
        ++seq_;
    }

    // ── the row view ────────────────────────────────────────────────────────

    /// Rebuild the view at width `w` (prefix sums over the lines). O(rows).
    void set_width(std::int32_t w) {
        settle_head();   // at the OLD width: evicted rows become a content cut
        w_ = std::max(w, 1);
        start_.resize(lines_.size());
        std::uint64_t acc = 0;
        for (std::size_t i = front_; i < lines_.size(); ++i) {
            start_[i] = acc;
            acc += rows_of(lines_[i], w_);
        }
        for (std::size_t i = 0; i < front_; ++i) start_[i] = 0;
        total_ = acc;
        ++seq_;
    }
    [[nodiscard]] std::int32_t width() const noexcept { return w_; }

    /// Visible history rows at the current width.
    [[nodiscard]] std::uint64_t rows() const noexcept {
        return front_ < lines_.size() ? total_ - start_[front_] - head_rows_ : 0;
    }

    /// Paint visible row `k` into dst[0, cols): its cells, Cell{} after.
    /// Returns the content length; `soft`: it wraps into the next row.
    std::uint32_t paint(std::uint64_t k, Cell *dst, std::int32_t cols, bool &soft) const {
        const auto [li, r] = locate(k);
        const Line &ln = lines_[li];
        const auto [at, take] = row_span(li, r);
        const std::uint32_t n = std::min<std::uint32_t>(take, static_cast<std::uint32_t>(std::max(cols, 0)));
        copy(ln, at, n, dst);
        std::fill(dst + n, dst + std::max(cols, 0), Cell{});
        soft = at + take < ln.len || (li + 1 == lines_.size() && open_);
        return n;
    }
    [[nodiscard]] bool soft(std::uint64_t k) const {
        const auto [li, r] = locate(k);
        const auto [at, take] = row_span(li, r);
        return at + take < lines_[li].len || (li + 1 == lines_.size() && open_);
    }
    /// The line holding visible row k, and the row's cell offset in it.
    [[nodiscard]] std::pair<std::uint64_t, std::uint32_t> line_at(std::uint64_t k) const {
        const auto [li, r] = locate(k);
        return {first_id_ + li, row_span(li, r).first};
    }
    /// First row of line `id` (visible indexing; negative if it begins above).
    [[nodiscard]] std::int64_t first_row(std::uint64_t id) const {
        const std::size_t li = static_cast<std::size_t>(id - first_id_);
        return static_cast<std::int64_t>(start_[li]) - static_cast<std::int64_t>(start_[front_] + head_rows_);
    }
    [[nodiscard]] bool has_line(std::uint64_t id) const noexcept {
        return id >= first_id_ + front_ && id < first_id_ + lines_.size();
    }
    [[nodiscard]] std::uint64_t next_id() const noexcept { return first_id_ + lines_.size(); }
    /// Cells of the last line (the open one, when open()).
    [[nodiscard]] std::uint32_t last_len() const noexcept { return lines_.empty() ? 0 : lines_.back().len; }
    /// Every visible line in order: f(id, len).
    template <class F> void each_line(F &&f) const {
        for (std::size_t i = front_; i < lines_.size(); ++i) f(first_id_ + i, lines_[i].len);
    }
    /// Bumps on every change: the history epoch's source.
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    // ── edits a resize makes (all at the current width) ─────────────────────

    /// Drop the oldest `n` visible rows (the scrollback cap). O(1) amortised.
    void evict_rows(std::uint64_t n) {
        while (n > 0 && front_ < lines_.size()) {
            const std::uint64_t end = front_ + 1 < lines_.size() ? start_[front_ + 1] : total_;
            const std::uint64_t left = end - start_[front_] - head_rows_;
            if (n < left) { head_rows_ += static_cast<std::uint32_t>(n); break; }
            n -= left;
            release(lines_[front_]);
            ++front_;
            head_rows_ = 0;
        }
        if (front_ == lines_.size()) open_ = false;
        compact();
        ++seq_;
    }

    /// Remove the newest `n` visible rows (a widen pulls them back into the
    /// grid, which paints them first). A line cut in two keeps its head here,
    /// now OPEN: its last row soft-wraps into the grid.
    void drop_back_rows(std::uint64_t n) {
        while (n > 0 && front_ < lines_.size()) {
            const std::size_t li = lines_.size() - 1;
            const std::uint64_t lrows = total_ - start_[li] - (li == front_ ? head_rows_ : 0);
            if (n >= lrows) {
                n -= lrows;
                total_ = start_[li];
                if (li == front_) head_rows_ = 0;
                drop_back();
                open_ = false;
                continue;
            }
            const std::uint32_t keep = static_cast<std::uint32_t>(total_ - start_[li] - n);
            Line &ln = lines_[li];
            const std::uint32_t cut = row_off(li, keep);
            if (!ln.chunk) dead_ += ln.len - cut;
            ln.len = cut;
            total_ = start_[li] + keep;
            open_ = true;
            n = 0;
        }
        ++seq_;
    }

    /// The reference reflow re-cut every painted history row on each resize:
    /// a soft row ending in a blank whose NEXT row starts with a wide glyph
    /// lost that blank (it reads as the padding a wide glyph leaves) -- or,
    /// when the cell before it is a wide lead, had it replaced by a spacer.
    /// Apply exactly that at the current width, segmenting by the rows as
    /// they are now (the reference cut row by row), and append each visible
    /// row's (line id, cell offset after the edits) to `map`: the old-row
    /// table a resize remaps from. Lines without wide glyphs cannot change
    /// and cost one integer per row; no cell of theirs is read.
    void recut(std::vector<std::pair<std::uint64_t, std::uint32_t>> &map) {
        settle_head();
        bool changed = false;
        const auto w = static_cast<std::uint32_t>(w_);
        for (std::size_t li = front_; li < lines_.size(); ++li) {
            Line &ln = lines_[li];
            const std::uint64_t id = first_id_ + li;
            const bool closed = !(li + 1 == lines_.size() && open_);
            // a closed line's last row is a HARD row: the reference trimmed its
            // trailing blanks again (they can come from a soft row before it)
            const bool trail = closed && ln.len > 0 && cell(ln, ln.len - 1).blank();
            if (!ln.wide && !trail) {
                const std::uint32_t rows = ln.len == 0 ? 1 : (ln.len + w - 1) / w;
                for (std::uint32_t r = 0; r < rows; ++r) map.emplace_back(id, r * w);
                continue;
            }
            edits_.clear();
            const auto cellf = [&](std::uint32_t i) -> const Cell & { return cell(ln, i); };
            std::uint32_t at = 0, shift = 0, last_at = 0;
            do {
                const std::uint32_t take = take_at(cellf, ln.len, at, w_);
                map.emplace_back(id, at - shift);
                last_at = at;
                const std::uint32_t end = at + take;
                if (end < ln.len && take == w && cell(ln, end - 1).blank() && cell(ln, end).width == 2) {
                    if (end >= 2 && cell(ln, end - 2).width == 2) {
                        edits_.push_back({end - 1, true});
                    } else {
                        edits_.push_back({end - 1, false});
                        ++shift;
                    }
                }
                at = end;
            } while (at < ln.len);
            std::uint32_t t = ln.len;
            bool pad = false;
            if (closed) {
                while (t > last_at && cell(ln, t - 1).blank()) --t;
                pad = t > last_at && cell(ln, t - 1).width == 2;
            }
            if (edits_.empty() && t == ln.len && !pad) continue;
            std::vector<Cell> next;
            next.reserve(t + 1);
            std::size_t e = 0;
            for (std::uint32_t i = 0; i < t; ++i) {
                if (e < edits_.size() && edits_[e].at == i) {
                    if (edits_[e].spacer) {
                        Cell sp{};
                        sp.pen = cell(ln, i - 1).pen;
                        sp.width = 0;
                        next.push_back(sp);
                    }
                    ++e;
                    continue;
                }
                next.push_back(cell(ln, i));
            }
            if (pad) {
                Cell sp{};
                sp.pen = cell(ln, t - 1).pen;
                sp.width = 0;
                next.push_back(sp);
            }
            if (next.size() == ln.len) {   // a re-synthesised spacer that was already there
                bool same = true;
                for (std::uint32_t i = 0; same && i < ln.len; ++i) same = next[i] == cell(ln, i);
                if (same) continue;
            }
            release(ln);
            ln.skip = 0;
            ln.off = put_owned(next.data(), static_cast<std::uint32_t>(next.size()));
            ln.len = static_cast<std::uint32_t>(next.size());
            changed = true;
        }
        if (changed) set_width(w_);
    }

private:
    struct Line {
        FrozenPtr chunk;          // null: owned (cells_[off - base_])
        std::uint64_t off = 0;    // owned: virtual offset; in place: slot of the first row
        std::uint32_t len = 0;
        std::uint32_t skip = 0;   // in place: cells of the first slot before the content
        bool wide = true;         // may hold a width-2 glyph: rows are walked
    };
    struct Edit { std::uint32_t at; bool spacer; };
    std::vector<Edit> edits_;

    [[nodiscard]] const Cell &cell(const Line &ln, std::uint32_t i) const noexcept {
        if (!ln.chunk) return cells_[static_cast<std::size_t>(ln.off - base_) + i];
        const Frozen &f = *ln.chunk;
        const std::uint64_t q = ln.skip + static_cast<std::uint64_t>(i);
        const std::size_t cols = static_cast<std::size_t>(f.cols);
        std::size_t slot = static_cast<std::size_t>(ln.off) + static_cast<std::size_t>(q / cols);
        if (slot >= f.cap) slot -= f.cap;
        return f.data[slot * cols + static_cast<std::size_t>(q % cols)];
    }
    void copy(const Line &ln, std::uint32_t at, std::uint32_t n, Cell *dst) const noexcept {
        if (n == 0) return;
        if (!ln.chunk) {
            std::memcpy(dst, cells_.data() + (ln.off - base_) + at, static_cast<std::size_t>(n) * sizeof(Cell));
            return;
        }
        const Frozen &f = *ln.chunk;
        const std::size_t cols = static_cast<std::size_t>(f.cols);
        std::uint64_t q = ln.skip + static_cast<std::uint64_t>(at);
        while (n > 0) {
            std::size_t slot = static_cast<std::size_t>(ln.off) + static_cast<std::size_t>(q / cols);
            if (slot >= f.cap) slot -= f.cap;
            const std::size_t col = static_cast<std::size_t>(q % cols);
            const std::uint32_t k = static_cast<std::uint32_t>(std::min<std::size_t>(n, cols - col));
            std::memcpy(dst, f.data + slot * cols + col, static_cast<std::size_t>(k) * sizeof(Cell));
            dst += k;
            q += k;
            n -= k;
        }
    }

    [[nodiscard]] std::uint32_t rows_of(const Line &ln, std::int32_t w) const noexcept {
        if (ln.len == 0) return 1;
        if (!ln.wide) return (ln.len + static_cast<std::uint32_t>(w) - 1) / static_cast<std::uint32_t>(w);
        std::uint32_t r = 0, at = 0;
        do {
            at += take_at([&](std::uint32_t i) -> const Cell & { return cell(ln, i); }, ln.len, at, w);
            ++r;
        } while (at < ln.len);
        return r;
    }
    /// Cell offset of row `r` of line `li` at the current width.
    [[nodiscard]] std::uint32_t row_off(std::size_t li, std::uint32_t r) const noexcept {
        const Line &ln = lines_[li];
        if (!ln.wide) return std::min(r * static_cast<std::uint32_t>(w_), ln.len);
        std::uint32_t at = 0, i = 0;
        if (hint_line_ == li && hint_w_ == w_ && hint_seq_ == seq_ && hint_row_ <= r) { at = hint_at_; i = hint_row_; }
        for (; i < r && at < ln.len; ++i)
            at += take_at([&](std::uint32_t j) -> const Cell & { return cell(ln, j); }, ln.len, at, w_);
        hint_line_ = li; hint_row_ = r; hint_at_ = at; hint_w_ = w_; hint_seq_ = seq_;
        return at;
    }
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> row_span(std::size_t li, std::uint32_t r) const noexcept {
        const Line &ln = lines_[li];
        const std::uint32_t at = row_off(li, r);
        if (!ln.wide) return {at, std::min<std::uint32_t>(static_cast<std::uint32_t>(w_), ln.len - at)};
        const std::uint32_t take = at < ln.len ? take_at([&](std::uint32_t j) -> const Cell & { return cell(ln, j); }, ln.len, at, w_) : 0;
        return {at, take};
    }
    [[nodiscard]] std::pair<std::size_t, std::uint32_t> locate(std::uint64_t k) const noexcept {
        const std::uint64_t a = start_[front_] + head_rows_ + k;
        const auto it = std::upper_bound(start_.begin() + static_cast<std::ptrdiff_t>(front_), start_.end(), a);
        const std::size_t li = static_cast<std::size_t>(it - start_.begin()) - 1;
        return {li, static_cast<std::uint32_t>(a - start_[li])};
    }

    std::uint64_t push_line(Line ln, bool open) {
        start_.push_back(total_);
        total_ += w_ > 0 ? rows_of(ln, w_) : 1;
        lines_.push_back(std::move(ln));
        open_ = open;
        ++seq_;
        return first_id_ + lines_.size() - 1;
    }
    std::uint64_t put_owned(const Cell *c, std::uint32_t n) {
        const std::uint64_t at = base_ + cells_.size();
        cells_.insert(cells_.end(), c, c + n);
        return at;
    }
    void own(Line &ln) {
        if (!ln.chunk) return;
        std::vector<Cell> tmp(ln.len);
        copy(ln, 0, ln.len, tmp.data());
        ln.chunk.reset();
        ln.skip = 0;
        ln.off = put_owned(tmp.data(), ln.len);
    }
    void erase_at(std::size_t li, std::uint32_t i) {
        Line &ln = lines_[li];
        std::vector<Cell> tmp(ln.len);
        copy(ln, 0, ln.len, tmp.data());
        tmp.erase(tmp.begin() + i);
        release(ln);
        ln.chunk.reset();
        ln.skip = 0;
        ln.off = put_owned(tmp.data(), static_cast<std::uint32_t>(tmp.size()));
        ln.len = static_cast<std::uint32_t>(tmp.size());
        ++seq_;
    }
    void release(Line &ln) {
        if (!ln.chunk) dead_ += ln.len;
        ln.chunk.reset();
        ln.len = 0;   // counted once; a released line holds nothing
    }
    void drop_back() {
        release(lines_.back());
        lines_.pop_back();
        start_.resize(lines_.size());
        if (front_ > lines_.size()) front_ = lines_.size();
    }
    /// Turn rows evicted from the front line into a content cut.
    void settle_head() {
        if (head_rows_ == 0 || front_ >= lines_.size()) { head_rows_ = 0; return; }
        Line &ln = lines_[front_];
        const std::uint32_t cut = row_off(front_, head_rows_);
        if (ln.chunk) {
            ln.skip += cut;
        } else {
            ln.off += cut;
            dead_ += cut;
        }
        ln.len -= cut;
        start_[front_] += head_rows_;
        head_rows_ = 0;
        ++seq_;
    }
    /// Reclaim dead space once it is half the arena: O(1) amortised.
    void compact() {
        if (front_ > 0 && front_ * 2 >= lines_.size()) {
            lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(front_));
            start_.erase(start_.begin(), start_.begin() + static_cast<std::ptrdiff_t>(front_));
            first_id_ += front_;
            front_ = 0;
        }
        if (dead_ * 2 < cells_.size() + 1) return;
        // Only LIVE lines [front_, end) own cells: an evicted line awaiting
        // erasure keeps a stale offset (and, released, no chunk -- it would
        // pass for owned and copy cells that are gone).
        std::vector<Cell> keep;
        std::uint64_t live = 0;
        for (std::size_t i = front_; i < lines_.size(); ++i) if (!lines_[i].chunk) live += lines_[i].len;
        keep.reserve(static_cast<std::size_t>(live));
        const std::uint64_t nb = base_ + cells_.size();   // new virtual base: offsets stay unique
        for (std::size_t i = front_; i < lines_.size(); ++i) {
            Line &ln = lines_[i];
            if (ln.chunk) continue;
            const std::uint64_t at = nb + keep.size();
            keep.insert(keep.end(), cells_.begin() + static_cast<std::ptrdiff_t>(ln.off - base_),
                        cells_.begin() + static_cast<std::ptrdiff_t>(ln.off - base_ + ln.len));
            ln.off = at;
        }
        for (std::size_t i = 0; i < front_; ++i) lines_[i].len = 0;   // dead: nothing to read
        cells_ = std::move(keep);
        base_ = nb;
        dead_ = 0;
    }
    void dirty() { ++seq_; if (w_ > 0) set_width(w_); }

    std::vector<Line> lines_;
    std::vector<std::uint64_t> start_;   // per line: first row (absolute since the last set_width)
    std::size_t front_ = 0;              // lines_[0, front_) evicted, awaiting compaction
    std::uint32_t head_rows_ = 0;        // rows of lines_[front_] evicted at this width
    std::uint64_t total_ = 0;            // rows of lines_[...] at w_, counting evicted ones
    std::uint64_t first_id_ = 0;         // id of lines_[0]
    std::vector<Cell> cells_;            // owned lines' cells (with holes; compacted)
    std::uint64_t base_ = 0, dead_ = 0;
    bool open_ = false;                  // the last line soft-wraps into what follows
    std::int32_t w_ = 0;
    std::uint64_t seq_ = 0;
    mutable std::size_t hint_line_ = ~std::size_t{0};
    mutable std::uint32_t hint_row_ = 0, hint_at_ = 0;
    mutable std::int32_t hint_w_ = 0;
    mutable std::uint64_t hint_seq_ = 0;
};

}  // namespace toe::term

#endif  // TOE_TERM_LINELOG_HPP
