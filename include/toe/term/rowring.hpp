// SPDX-License-Identifier: LGPL-2.0-or-later
//
// A unified, zero-copy row ring for the terminal grid + scrollback.
//
// The scrollback and the live grid share ONE contiguous cell arena, organised
// as a ring of fixed-width row slots. Scrolling a line off the top of the
// visible grid does NOT copy it anywhere — that row is already in the ring; it
// simply becomes scrollback, and the visible window slides down by one slot. A
// fresh blank row is claimed at the ring's tail (reusing the oldest slot's
// storage once the scrollback cap is reached). The result: a scroll is O(1)
// pointer arithmetic plus blanking ONE new row, with zero per-line memcpy of
// existing content. This is the difference between memory-bandwidth-bound
// scrolling (copying whole rows) and index-bound scrolling.
//
// Terminology:
//   * `rows`      — visible grid height.
//   * `scroll`    — max scrollback lines retained above the visible grid.
//   * a "slot"    — one physical row of `cols` cells in the arena.
//   * total(i)    — total live rows = scrollback lines + `rows`.
//   Logical addressing is by ABSOLUTE row: 0 = oldest scrollback line,
//   total-1 = the bottom visible line. The visible grid occupies the last
//   `rows` absolute rows.
//
// The ring never moves cell data on scroll; only `head_` and `count_` change.

#ifndef TOE_TERM_ROWRING_HPP
#define TOE_TERM_ROWRING_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "toe/term/cell.hpp"

namespace toe::term {

class RowRing {
public:
    // (Re)allocate for `rows` visible × `cols` wide, retaining up to `scroll`
    // scrollback lines. Fills every slot blank. Starts with exactly `rows` live
    // rows (no scrollback yet). `blank` is the fill cell.
    void reset(std::int32_t rows, std::int32_t cols, std::size_t scroll, const Cell &blank) {
        rows_ = rows < 0 ? 0 : rows;
        cols_ = cols < 0 ? 0 : cols;
        cap_slots_ = static_cast<std::size_t>(rows_) + scroll;
        if (cap_slots_ == 0) cap_slots_ = 1;
        blank_ = blank;
        fill_ = blank;
        blank_line_valid_ = false;
        arena_.assign(cap_slots_ * static_cast<std::size_t>(cols_), blank_);
        wrapped_.assign(cap_slots_, false);
        used_.assign(cap_slots_, 0); // freshly-filled arena: every row all-blank
        head_ = 0;
        high_water_ = static_cast<std::size_t>(rows_); // reset exposes rows_ slots
        count_ = static_cast<std::size_t>(rows_); // the visible grid, no scrollback
    }

    [[nodiscard]] std::int32_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::int32_t cols() const noexcept { return cols_; }
    // Scrollback line count (live rows above the visible grid).
    [[nodiscard]] std::size_t scrollback() const noexcept {
        return count_ - static_cast<std::size_t>(rows_);
    }
    [[nodiscard]] std::size_t total() const noexcept { return count_; }

    // Physical slot backing absolute row `abs` (0 = oldest). Ring arithmetic.
    [[nodiscard]] std::size_t slot_of(std::size_t abs) const noexcept {
        return (head_ + abs) % cap_slots_;
    }
    // Pointer to the `cols` cells of absolute row `abs`.
    [[nodiscard]] Cell *abs_row(std::size_t abs) noexcept {
        return arena_.data() + slot_of(abs) * static_cast<std::size_t>(cols_);
    }
    [[nodiscard]] const Cell *abs_row(std::size_t abs) const noexcept {
        return arena_.data() + slot_of(abs) * static_cast<std::size_t>(cols_);
    }

    // The visible grid starts at this absolute row (= scrollback count).
    [[nodiscard]] std::size_t view_base() const noexcept {
        return count_ - static_cast<std::size_t>(rows_);
    }
    // Pointer to visible grid row `r` (0 = top of visible area).
    [[nodiscard]] Cell *view_row(std::int32_t r) noexcept {
        return abs_row(view_base() + static_cast<std::size_t>(r));
    }
    [[nodiscard]] const Cell *view_row(std::int32_t r) const noexcept {
        return abs_row(view_base() + static_cast<std::size_t>(r));
    }

    [[nodiscard]] bool wrapped_abs(std::size_t abs) const noexcept {
        return wrapped_[slot_of(abs)];
    }
    void set_wrapped_abs(std::size_t abs, bool w) noexcept { wrapped_[slot_of(abs)] = w; }

    // View-relative wrapped accessors (0 = top of visible grid).
    [[nodiscard]] bool view_wrapped(std::int32_t r) const noexcept {
        return wrapped_abs(view_base() + static_cast<std::size_t>(r));
    }
    void set_view_wrapped(std::int32_t r, bool w) noexcept {
        set_wrapped_abs(view_base() + static_cast<std::size_t>(r), w);
    }

    // Blank a single visible-grid row (0 = top). Honours BCE via `blank`.
    void blank_view_row(std::int32_t r) noexcept {
        blank_row(view_base() + static_cast<std::size_t>(r));
    }
    void blank_view_row(std::int32_t r, const Cell &blank) noexcept {
        set_blank(blank);
        blank_row(view_base() + static_cast<std::size_t>(r));
    }

    // Rotate the visible rows [top,bottom] (inclusive, view-relative) so that
    // the row `k` steps down moves to `top`. Positive k scrolls the region UP
    // by k (content moves toward the top); the bottom k rows are left as-is for
    // the caller to blank. Physically moves cell contents + wrapped flags — the
    // ring has no per-region head, so region-local scrolls copy. This is the
    // cold path; full-screen scroll uses scroll_up_one (O(1)).
    void rotate_view_up(std::int32_t top, std::int32_t bottom, std::int32_t k) noexcept {
        rotate_region(top, bottom, k);
    }
    // Scroll region [top,bottom] DOWN by k: content moves toward the bottom;
    // the top k rows are left for the caller to blank.
    void rotate_view_down(std::int32_t top, std::int32_t bottom, std::int32_t k) noexcept {
        const std::int32_t span = bottom - top + 1;
        if (span <= 0) return;
        rotate_region(top, bottom, span - (k % span == 0 ? span : k % span));
    }

    // Scroll the WHOLE view up by one line: the top visible row becomes the
    // newest scrollback line (already in place — no copy), and a fresh blank
    // row appears at the bottom. When scrollback is at capacity the oldest slot
    // is recycled as the new blank row. `push_scrollback` controls whether the
    // evicted top row is kept (primary buffer) or discarded (alt screen).
    // Returns the absolute index of the newly blanked bottom row.
    std::size_t scroll_up_one(bool push_scrollback, const Cell &blank) noexcept {
        set_blank(blank);
        if (push_scrollback && count_ < cap_slots_) {
            // Grow the scrollback by one: claim a fresh tail slot. Until the ring
            // has wrapped once (high_water_ tracks the furthest slot ever used),
            // that slot is still in its pristine post-reset blank state, so we
            // can skip re-blanking it entirely — the single biggest cost under a
            // newline flood. Only blank when the blank cell differs from the one
            // the arena was filled with, or the slot was previously written.
            const std::size_t newbot = count_; // absolute index of the new row
            ++count_;
            const std::size_t slot = slot_of(newbot);
            if (slot >= high_water_ && !blank_differs_from_fill()) {
                high_water_ = slot + 1;
                wrapped_[slot] = false; // arena is already blank here
            } else {
                blank_row(newbot);
            }
            return newbot;
        }
        // At capacity (or alt screen): recycle the oldest slot as the new bottom.
        // Advancing head_ drops the oldest scrollback line; count_ is unchanged,
        // so the view (last `rows`) shifts to include the recycled slot.
        head_ = (head_ + 1) % cap_slots_;
        const std::size_t newbot = count_ - 1; // last absolute row = new bottom
        blank_row(newbot);
        return newbot;
    }

    // Blank absolute row `abs` (bulk memcpy from a cached blank line).
    void blank_row(std::size_t abs) noexcept {
        Cell *dst = abs_row(abs);
        const std::size_t slot = slot_of(abs);
        // Only the prefix [0, used) can hold non-blank cells — terminal rows are
        // written left-to-right and never partially un-written, so everything
        // past the high-water column is already blank. Clearing just the prefix
        // turns a full-width row blank (the flood hot path) into a few-cell
        // memcpy. Falls back to a full clear when the blank cell differs from
        // the arena fill (BCE with a non-default background).
        ensure_blank_line();
        std::size_t n = used_[slot];
        if (blank_differs_from_fill()) n = static_cast<std::size_t>(cols_);
        if (n > static_cast<std::size_t>(cols_)) n = static_cast<std::size_t>(cols_);
        std::memcpy(dst, blank_line_.data(), n * sizeof(Cell));
        used_[slot] = 0;
        wrapped_[slot] = false;
    }

    // Record that visible row `r` has content out to (exclusive) column `endcol`,
    // so a later blank_row only has to clear that prefix. Called by the Screen
    // write path. Cheap monotonic max.
    void note_used(std::int32_t r, std::int32_t endcol) noexcept {
        const std::size_t slot = slot_of(view_base() + static_cast<std::size_t>(r));
        const std::size_t e = static_cast<std::size_t>(endcol < 0 ? 0 : endcol);
        if (e > used_[slot]) used_[slot] = e;
    }

    // Mark visible row `r` as fully dirty (used == cols): bulk-copy paths that
    // write a whole row without going through note_used call this so a later
    // blank_row clears the entire width.
    void mark_view_full(std::int32_t r) noexcept {
        used_[slot_of(view_base() + static_cast<std::size_t>(r))] =
            static_cast<std::size_t>(cols_);
    }

    // Drop all scrollback (used on RIS / alt-screen enter): keep only the
    // visible grid as the live rows.
    void clear_scrollback() noexcept {
        // Move the visible rows to the front conceptually by making them the
        // only live rows. Their slots stay put; we just forget the scrollback.
        head_ = view_base_slot();
        count_ = static_cast<std::size_t>(rows_);
    }

    // Resize to `new_rows` x `new_cols`, preserving as much content as possible.
    // Rows are copied in absolute order (oldest scrollback first) into a fresh
    // linear arena, cropped/blank-padded to new_cols. The last `new_rows` rows
    // become the visible grid; the rest stay as scrollback (capped by `scroll`).
    // Used for non-reflow resizes (alt screen, or height-only changes).
    void resize_keep(std::int32_t new_rows, std::int32_t new_cols, std::size_t scroll,
                     const Cell &blank) {
        new_rows = new_rows < 0 ? 0 : new_rows;
        new_cols = new_cols < 0 ? 0 : new_cols;
        const std::size_t old_total = count_;
        const std::int32_t copy_cols = std::min(cols_, new_cols);
        // Snapshot the existing rows (absolute order) before reallocating.
        std::vector<std::vector<Cell>> saved;
        std::vector<bool> saved_wrapped;
        saved.reserve(old_total);
        saved_wrapped.reserve(old_total);
        for (std::size_t i = 0; i < old_total; ++i) {
            std::vector<Cell> row(static_cast<std::size_t>(new_cols), blank);
            const Cell *src = abs_row(i);
            for (std::int32_t c = 0; c < copy_cols; ++c) row[static_cast<std::size_t>(c)] = src[c];
            saved.push_back(std::move(row));
            saved_wrapped.push_back(wrapped_abs(i));
        }
        // Reallocate the arena for the new geometry.
        rows_ = new_rows;
        cols_ = new_cols;
        cap_slots_ = static_cast<std::size_t>(rows_) + scroll;
        if (cap_slots_ == 0) cap_slots_ = 1;
        blank_ = blank;
        fill_ = blank;
        blank_line_valid_ = false;
        arena_.assign(cap_slots_ * static_cast<std::size_t>(cols_), blank_);
        wrapped_.assign(cap_slots_, false);
        used_.assign(cap_slots_, 0);
        blank_line_.clear();
        head_ = 0;
        // Keep the last (rows_ + scroll) saved rows; the visible grid is always
        // the final rows_ of them (blank-padded if we had fewer rows before).
        const std::size_t want = cap_slots_;
        const std::size_t have = saved.size();
        const std::size_t drop = have > want ? have - want : 0;
        const std::size_t kept = have - drop;
        // We need at least rows_ live rows; if we kept fewer than rows_, the
        // deficit is fresh blank rows appended at the bottom.
        const std::size_t live_from_saved = std::min(kept, static_cast<std::size_t>(rows_));
        const std::size_t sb = kept - live_from_saved;
        count_ = sb + static_cast<std::size_t>(rows_);
        for (std::size_t i = 0; i < kept; ++i) {
            Cell *dst = abs_row(i);
            const auto &row = saved[drop + i];
            std::memcpy(dst, row.data(), static_cast<std::size_t>(cols_) * sizeof(Cell));
            set_wrapped_abs(i, saved_wrapped[drop + i]);
            used_[slot_of(i)] = static_cast<std::size_t>(cols_); // restored: assume full
        }
        // Slots [0,kept) now hold restored content; the rest are pristine fill.
        high_water_ = kept;
        // Any trailing visible rows beyond what we restored are already blank.
    }

    [[nodiscard]] const Cell &blank_cell() const noexcept { return blank_; }

private:
    [[nodiscard]] std::size_t view_base_slot() const noexcept { return slot_of(view_base()); }

    // Left-rotate visible rows [top,bottom] (inclusive) by k, moving both cell
    // contents and wrapped flags. std::rotate semantics: the row originally at
    // top+k ends up at top. Rows are non-contiguous in the ring (modular), so
    // this cycles through them via a scratch row. O(span*cols) copy.
    void rotate_region(std::int32_t top, std::int32_t bottom, std::int32_t k) noexcept {
        if (top < 0) top = 0;
        if (bottom > rows_ - 1) bottom = rows_ - 1;
        const std::int32_t span = bottom - top + 1;
        if (span <= 1) return;
        k %= span;
        if (k < 0) k += span;
        if (k == 0) return;
        const std::size_t rowbytes = static_cast<std::size_t>(cols_) * sizeof(Cell);
        std::vector<bool> new_wrapped(static_cast<std::size_t>(span));
        std::vector<std::size_t> new_used(static_cast<std::size_t>(span));
        // Compute the destination image in a temp, then write back. Simpler and
        // safe against overlap in the modular arena. dst row i <- src row (i+k).
        std::vector<Cell> image(static_cast<std::size_t>(span) *
                                static_cast<std::size_t>(cols_));
        for (std::int32_t i = 0; i < span; ++i) {
            const std::int32_t src = top + ((i + k) % span);
            std::memcpy(image.data() + static_cast<std::size_t>(i) *
                                           static_cast<std::size_t>(cols_),
                        view_row(src), rowbytes);
            new_wrapped[static_cast<std::size_t>(i)] = view_wrapped(src);
            new_used[static_cast<std::size_t>(i)] =
                used_[slot_of(view_base() + static_cast<std::size_t>(src))];
        }
        for (std::int32_t i = 0; i < span; ++i) {
            std::memcpy(view_row(top + i),
                        image.data() + static_cast<std::size_t>(i) *
                                           static_cast<std::size_t>(cols_),
                        rowbytes);
            set_view_wrapped(top + i, new_wrapped[static_cast<std::size_t>(i)]);
            used_[slot_of(view_base() + static_cast<std::size_t>(top + i))] =
                new_used[static_cast<std::size_t>(i)];
        }
    }

    void ensure_blank_line() {
        if (!blank_line_valid_ || static_cast<std::int32_t>(blank_line_.size()) != cols_) {
            blank_line_.assign(static_cast<std::size_t>(cols_), blank_);
            blank_line_valid_ = true;
        }
    }

    // True when the current blank cell (BCE-coloured) differs from the fill the
    // arena was assign()ed with, so a pristine slot is NOT already the right
    // blank and must be memcpy-blanked. Compares by value (Cell::operator==) so
    // struct padding bytes never cause a spurious mismatch.
    [[nodiscard]] bool blank_differs_from_fill() const noexcept {
        return !(blank_ == fill_);
    }

    // Set the fill/blank cell, invalidating the cached blank line if it changed.
    void set_blank(const Cell &b) noexcept {
        if (!(blank_ == b)) {
            blank_ = b;
            blank_line_valid_ = false;
        }
    }

    std::vector<Cell> arena_{};        // cap_slots_ * cols_ cells (the ring)
    std::vector<bool> wrapped_{};      // per-slot soft-wrap flag
    std::vector<std::size_t> used_{};  // per-slot high-water column (dirty prefix)
    std::vector<Cell> blank_line_{};   // cached blank row for fast blanking
    bool blank_line_valid_ = false;    // is blank_line_ current for blank_?
    Cell blank_{};
    Cell fill_{};                      // cell the arena was assign()ed with
    std::size_t high_water_ = 0;       // # of slots ever exposed (pristine above)
    std::size_t cap_slots_ = 0;        // rows + scrollback capacity
    std::int32_t rows_ = 0, cols_ = 0;
    std::size_t head_ = 0;             // slot of absolute row 0 (oldest)
    std::size_t count_ = 0;            // total live rows (scrollback + visible)
};

} // namespace toe::term

#endif // TOE_TERM_ROWRING_HPP
