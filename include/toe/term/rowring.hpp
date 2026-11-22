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
#include <algorithm>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "toe/term/cell.hpp"

namespace toe::term {

// The ring's cell storage: allocated in full, initialised lazily.
//
// A 10,000-line scrollback at 110 columns is ~20 MB of cells. Filling all of
// it in reset() -- once when the screen is built and AGAIN on every width
// change, since reflow resets the ring -- was ~5 ms each on a terminal's
// startup path, for history that is mostly never written. The OS hands out
// untouched pages for free, so only the prefix actually exposed is filled
// (RowRing::fill_to, in chunks ahead of high_water_): the same total work
// under a flood, none for scrollback nobody reaches.
class CellArena {
    static_assert(std::is_trivially_copyable_v<Cell> && std::is_trivially_destructible_v<Cell>,
                  "CellArena hands out raw storage: Cell must stay a plain value");
public:
    CellArena() = default;
    CellArena(const CellArena &o) {
        if (o.n_ == 0) return;
        p_ = std::allocator<Cell>{}.allocate(o.n_);
        n_ = cap_ = o.n_;
        std::uninitialized_copy(o.p_, o.p_ + o.init_, p_);
        init_ = o.init_;
    }
    CellArena(CellArena &&o) noexcept { swap(o); }
    CellArena &operator=(CellArena o) noexcept { swap(o); return *this; }
    ~CellArena() { release(); }
    void swap(CellArena &o) noexcept {
        std::swap(p_, o.p_);
        std::swap(n_, o.n_);
        std::swap(init_, o.init_);
        std::swap(cap_, o.cap_);
    }

    // `n` cells of storage with only the first `prefix` set to `fill`.
    //
    // The block is KEPT when it is big enough (and not wastefully so): a
    // width change asks for a slightly different size every column, and
    // freeing it each time handed back fresh pages that the reflow then
    // page-faulted in, 40 MB of them per column at 10k lines. Growth comes
    // with headroom so a widening drag reallocates once, not per column.
    void reset(std::size_t n, std::size_t prefix, const Cell &fill) {
        if (n > cap_ || n < cap_ / 2) {
            release();
            const std::size_t want = n + n / 8;
            if (want != 0) p_ = std::allocator<Cell>{}.allocate(want);
            cap_ = want;
        }
        n_ = n;
        init_ = 0;   // the old cells are stale, refilled on demand
        ensure(prefix, fill);
    }
    // Cells [0, upto) valid: the ones not yet initialised become `fill`.
    void ensure(std::size_t upto, const Cell &fill) noexcept {
        if (upto > n_) upto = n_;
        if (upto <= init_) return;
        std::uninitialized_fill(p_ + init_, p_ + upto, fill);
        init_ = upto;
    }
    // Cells [0, upto) count as valid WITHOUT a fill: the caller writes every
    // one of them before anything reads (Cell is trivially copyable).
    void claim(std::size_t upto) noexcept {
        static_assert(std::is_trivially_copyable_v<Cell>);
        if (upto > n_) upto = n_;
        if (upto > init_) init_ = upto;
    }
    [[nodiscard]] Cell *data() noexcept { return p_; }
    [[nodiscard]] const Cell *data() const noexcept { return p_; }

private:
    void release() noexcept {
        if (p_ != nullptr) std::allocator<Cell>{}.deallocate(p_, cap_);
        p_ = nullptr;
        n_ = init_ = cap_ = 0;
    }
    Cell *p_ = nullptr;
    std::size_t n_ = 0, init_ = 0, cap_ = 0;   // cap_: allocated cells (>= n_)
};

class RowRing {
public:
    // (Re)allocate for `rows` visible × `cols` wide, retaining up to `scroll`
    // scrollback lines. Starts with exactly `rows` live blank rows (no
    // scrollback yet); the scrollback slots are filled with `blank` as they
    // are first exposed (fill_to). `blank` is the fill cell.
    void reset(std::int32_t rows, std::int32_t cols, std::size_t scroll, const Cell &blank) {
        rows_ = rows < 0 ? 0 : rows;
        cols_ = cols < 0 ? 0 : cols;
        cap_slots_ = static_cast<std::size_t>(rows_) + scroll;
        if (cap_slots_ == 0) cap_slots_ = 1;
        blank_ = blank;
        fill_ = blank;
        blank_line_valid_ = false;
        blank_differs_ = false;
        arena_.reset(cap_slots_ * static_cast<std::size_t>(cols_),
                     static_cast<std::size_t>(rows_) * static_cast<std::size_t>(cols_), blank_);
        filled_slots_ = std::min(static_cast<std::size_t>(rows_), cap_slots_);
        wrapped_.assign(cap_slots_, false);
        used_.assign(cap_slots_, 0); // freshly-filled arena: every row all-blank
        head_ = 0;
        high_water_ = static_cast<std::size_t>(rows_); // reset exposes rows_ slots
        count_ = static_cast<std::size_t>(rows_); // the visible grid, no scrollback
    }

    [[nodiscard]] std::int32_t rows() const noexcept { return rows_; }

    /// (Re)allocate like reset(), but with `history` scrollback rows already
    /// live (capped at `scroll`): rows [0, history + rows) are addressable at
    /// once, uninitialised, for a caller that writes EVERY cell of each
    /// (a reflow). One pass instead of a scroll per row plus a blank fill per
    /// row it then overwrites. The caller sets used / wrapped per row.
    void reset_with_history(std::int32_t rows, std::int32_t cols, std::size_t scroll, std::size_t history,
                            const Cell &blank) {
        reset(rows, cols, scroll, blank);
        if (history > scroll) history = scroll;
        count_ = static_cast<std::size_t>(rows_) + history;
        high_water_ = count_;
        filled_slots_ = std::max(filled_slots_, count_);
        arena_.claim(count_ * static_cast<std::size_t>(cols_));
    }
    /// A row written whole by the caller: content out to `used`.
    void set_used_abs(std::size_t abs, std::size_t used) noexcept { used_[slot_of(abs)] = used; }
    [[nodiscard]] std::int32_t cols() const noexcept { return cols_; }
    // Scrollback line count (live rows above the visible grid).
    [[nodiscard]] std::size_t scrollback() const noexcept {
        return count_ - static_cast<std::size_t>(rows_);
    }
    [[nodiscard]] std::size_t total() const noexcept { return count_; }

    // Physical slot backing absolute row `abs` (0 = oldest). Ring arithmetic
    // WITHOUT a modulo: head_ and abs are each < cap_slots_, so head_+abs is
    // < 2*cap and one conditional subtract wraps it — far cheaper than the div
    // a `% cap_slots_` compiles to, and slot_of() is on EVERY cell read/write.
    // (This is the trick alacritty's Storage::compute_index uses.)
    [[nodiscard]] std::size_t slot_of(std::size_t abs) const noexcept {
        std::size_t s = head_ + abs;
        return s >= cap_slots_ ? s - cap_slots_ : s;
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

    // Blank columns [c0, c1) of visible row r with the current `blank` cell in
    // one bulk fill — the erase (ED/EL/ECH) fast path, replacing per-cell at()
    // loops. Extends used_ so a later blank_row still clears the full prefix.
    void blank_view_span(std::int32_t r, std::int32_t c0, std::int32_t c1,
                         const Cell &blank) noexcept {
        if (c0 < 0) c0 = 0;
        if (c1 > cols_) c1 = cols_;
        if (c1 <= c0) return;
        set_blank(blank);
        ensure_blank_line();
        Cell *row = view_row(r);
        std::memcpy(row + c0, blank_line_.data(),
                    static_cast<std::size_t>(c1 - c0) * sizeof(Cell));
        const std::size_t slot = slot_of(view_base() + static_cast<std::size_t>(r));
        if (static_cast<std::size_t>(c1) > used_[slot]) used_[slot] = static_cast<std::size_t>(c1);
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
            if (slot >= filled_slots_) [[unlikely]] fill_to(slot + 1);
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
        head_ = head_ + 1 == cap_slots_ ? 0 : head_ + 1;
        const std::size_t newbot = count_ - 1; // last absolute row = new bottom
        blank_row(newbot);
        // The NEXT slot to be recycled (now at head_) was last touched a whole
        // scrollback ago: cold. Pull its written prefix toward the cache now,
        // so the next line's blank + write overlap the misses instead of
        // stalling on each one -- the dominant cost of output once the
        // scrollback is full.
        prefetch_slot(head_);
        return newbot;
    }

    void prefetch_slot(std::size_t slot) const noexcept {
        // Short rows (a few cells: `yes`, short log lines) are one or two
        // lines the hardware prefetcher already handles; prefetching them is
        // pure overhead. Only a wide written prefix is worth it.
        const std::size_t used = used_[slot];
        if (used < 16) return;
        const auto *p = reinterpret_cast<const char *>(arena_.data() + slot * static_cast<std::size_t>(cols_));
        const std::size_t n = std::min(used + 1, static_cast<std::size_t>(cols_)) * sizeof(Cell);
        for (std::size_t off = 0; off < n; off += 128) __builtin_prefetch(p + off, 1, 0);
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

    // RIS: drop the scrollback AND guarantee no stale cell can resurface. The
    // plain clear_scrollback only moves pointers, leaving old (possibly BCE-
    // coloured) cells physically in slots that a later scroll will recycle. Two
    // things then leak that stale colour: (1) the recycle fast path trusts
    // `slot >= high_water_` as pristine `fill_`, and (2) blank_row only clears
    // each slot's `used_` prefix, so colour past a now-smaller used_ survives.
    // Fix both cheaply: force high_water_ to the whole arena (so every recycle
    // takes the blank_row path, never the pristine fast path) and set every
    // slot's used_ to full width (so blank_row clears the ENTIRE row). This is
    // O(cap_slots) integer writes — NOT the O(cap_slots * cols) arena zero-fill a
    // full reset() does, which made a RIS-heavy stream (mixednasty: 800k RIS)
    // pathologically slow.
    void reset_scrollback() noexcept {
        clear_scrollback();
        high_water_ = cap_slots_;                 // no slot is trusted pristine
        std::fill(used_.begin(), used_.end(),     // blank_row will clear full width
                  static_cast<std::size_t>(cols_));
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
        // Snapshot existing rows (absolute order) into ONE flat buffer laid out
        // at the NEW width — crop/blank-pad per row — plus a parallel wrapped[].
        // A single allocation instead of one std::vector per row (the height-
        // only resize used to churn ~old_total heap allocs). Reused across calls.
        const std::size_t ncols = static_cast<std::size_t>(new_cols);
        keep_scratch_.assign(old_total * ncols, blank);
        keep_wrapped_.assign(old_total, false);
        for (std::size_t i = 0; i < old_total; ++i) {
            Cell *dst = keep_scratch_.data() + i * ncols;
            const Cell *src = abs_row(i);
            for (std::int32_t c = 0; c < copy_cols; ++c) dst[static_cast<std::size_t>(c)] = src[c];
            keep_wrapped_[i] = wrapped_abs(i);
        }
        // Reallocate the arena for the new geometry.
        rows_ = new_rows;
        cols_ = new_cols;
        cap_slots_ = static_cast<std::size_t>(rows_) + scroll;
        if (cap_slots_ == 0) cap_slots_ = 1;
        blank_ = blank;
        fill_ = blank;
        blank_line_valid_ = false;
        blank_differs_ = false;
        const std::size_t want = cap_slots_;
        const std::size_t have = old_total;
        const std::size_t drop = have > want ? have - want : 0;
        const std::size_t kept = have - drop;
        const std::size_t live_from_saved = std::min(kept, static_cast<std::size_t>(rows_));
        const std::size_t sb = kept - live_from_saved;
        count_ = sb + static_cast<std::size_t>(rows_);
        // Every live row must be valid (the ones not restored below are
        // blank); the rest of the scrollback fills as it is exposed.
        arena_.reset(cap_slots_ * static_cast<std::size_t>(cols_),
                     std::min(count_, cap_slots_) * static_cast<std::size_t>(cols_), blank_);
        filled_slots_ = std::min(count_, cap_slots_);
        wrapped_.assign(cap_slots_, false);
        used_.assign(cap_slots_, 0);
        blank_line_.clear();
        head_ = 0;
        for (std::size_t i = 0; i < kept; ++i) {
            Cell *dst = abs_row(i);
            const Cell *src = keep_scratch_.data() + (drop + i) * ncols;
            std::memcpy(dst, src, static_cast<std::size_t>(cols_) * sizeof(Cell));
            set_wrapped_abs(i, keep_wrapped_[drop + i]);
            used_[slot_of(i)] = static_cast<std::size_t>(cols_); // restored: assume full
        }
        high_water_ = kept;
    }

    [[nodiscard]] const Cell &blank_cell() const noexcept { return blank_; }

private:
    [[nodiscard]] std::size_t view_base_slot() const noexcept { return slot_of(view_base()); }

    // Initialise the arena out to at least `slots` slots, in chunks so a
    // flood pays one bulk fill per kFillChunk lines instead of a branchy
    // per-line one. Cold: once every kFillChunk new scrollback lines, and
    // never again once the ring has been filled.
    static constexpr std::size_t kFillChunk = 512;
    [[gnu::noinline, gnu::cold]] void fill_to(std::size_t slots) noexcept {
        std::size_t to = std::max(slots, filled_slots_ + kFillChunk);
        if (to > cap_slots_) to = cap_slots_;
        arena_.ensure(to * static_cast<std::size_t>(cols_), fill_);
        filled_slots_ = to;
    }

    // Left-rotate visible rows [top,bottom] (inclusive) by k, moving both cell
    // contents and wrapped flags. std::rotate semantics: the row originally at
    // top+k ends up at top. Rows are non-contiguous in the ring (modular), so
    // we rotate via the cycle-leader (juggling) algorithm using a single
    // reusable scratch row — O(span*cols) copies, ZERO heap allocation per call
    // (the scroll-region hot path fires this hundreds of thousands of times).
    void rotate_region(std::int32_t top, std::int32_t bottom, std::int32_t k) noexcept {
        if (top < 0) top = 0;
        if (bottom > rows_ - 1) bottom = rows_ - 1;
        const std::int32_t span = bottom - top + 1;
        if (span <= 1) return;
        k %= span;
        if (k < 0) k += span;
        if (k == 0) return;
        const std::size_t rowbytes = static_cast<std::size_t>(cols_) * sizeof(Cell);
        const std::size_t vbase = view_base();
        // Reusable scratch row (grows once, then persists across calls).
        if (scratch_.size() < static_cast<std::size_t>(cols_))
            scratch_.resize(static_cast<std::size_t>(cols_));
        Cell *tmp = scratch_.data();

        // Left-rotate by k using cycle leaders: element i receives element
        // (i+k) mod span. gcd(span,k) cycles cover all positions; each element
        // is moved exactly once. Track wrapped/used alongside the cell bytes.
        const std::int32_t g = gcd_i32(span, k);
        for (std::int32_t start = 0; start < g; ++start) {
            const std::int32_t s0 = top + start;
            const std::size_t slot0 = slot_of(vbase + static_cast<std::size_t>(s0));
            std::memcpy(tmp, view_row(s0), rowbytes);
            const bool w0 = wrapped_[slot0];
            const std::size_t u0 = used_[slot0];
            std::int32_t i = start;
            for (;;) {
                const std::int32_t j = (i + k) % span;
                if (j == start) break;
                Cell *di = view_row(top + i);
                const Cell *sj = view_row(top + j);
                const std::size_t sloti = slot_of(vbase + static_cast<std::size_t>(top + i));
                const std::size_t slotj = slot_of(vbase + static_cast<std::size_t>(top + j));
                std::memcpy(di, sj, rowbytes);
                wrapped_[sloti] = wrapped_[slotj];
                used_[sloti] = used_[slotj];
                i = j;
            }
            Cell *dlast = view_row(top + i);
            const std::size_t slotlast = slot_of(vbase + static_cast<std::size_t>(top + i));
            std::memcpy(dlast, tmp, rowbytes);
            wrapped_[slotlast] = w0;
            used_[slotlast] = u0;
        }
    }

    static std::int32_t gcd_i32(std::int32_t a, std::int32_t b) noexcept {
        while (b) { std::int32_t t = a % b; a = b; b = t; }
        return a;
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
        return blank_differs_;
    }

    // Set the fill/blank cell, invalidating the cached blank line if it changed.
    // Called once per scrolled line; Pen== is a handful of integer compares
    // (no variant dispatch), so this is cheap. Not a memcmp: a freshly built
    // blank cell's padding bytes are unspecified.
    void set_blank(const Cell &b) noexcept {
        if (blank_ == b) return;
        blank_ = b;
        blank_line_valid_ = false;
        blank_differs_ = !(blank_ == fill_);
    }

    CellArena arena_{};                // cap_slots_ * cols_ cells (the ring); [0, filled_slots_) initialised
    std::size_t filled_slots_ = 0;     // slots whose cells are valid (>= every slot ever live)
    std::vector<bool> wrapped_{};      // per-slot soft-wrap flag
    std::vector<std::size_t> used_{};  // per-slot high-water column (dirty prefix)
    std::vector<Cell> blank_line_{};   // cached blank row for fast blanking
    std::vector<Cell> scratch_{};      // reusable scratch row for rotate_region
    bool blank_line_valid_ = false;    // is blank_line_ current for blank_?
    bool blank_differs_ = false;       // !(blank_ == fill_), kept by set_blank / reset
    Cell blank_{};
    Cell fill_{};                      // cell the arena was assign()ed with
    std::size_t high_water_ = 0;       // # of slots ever exposed (pristine above)
    std::size_t cap_slots_ = 0;        // rows + scrollback capacity
    std::int32_t rows_ = 0, cols_ = 0;
    std::size_t head_ = 0;             // slot of absolute row 0 (oldest)
    std::size_t count_ = 0;            // total live rows (scrollback + visible)
    // Reusable flat snapshot for resize_keep (one alloc, not one per row).
    std::vector<Cell> keep_scratch_{};
    std::vector<bool> keep_wrapped_{};
};

} // namespace toe::term

#endif // TOE_TERM_ROWRING_HPP
