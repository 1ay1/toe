// SPDX-License-Identifier: LGPL-2.0-or-later
//
// LatencyMeter — measures true input-to-photon latency: the time from a
// keystroke being written to the child to the present() that first reflects the
// child's response on screen. This is the metric that decides whether a
// terminal "feels" instant under a TUI (vim, tmux, a REPL), and almost no
// terminal reports it. hand does.
//
// The run loop stamps a monotonic clock when it writes input to the PTY, and
// again at the present that follows the child's echo/redraw; the delta is one
// sample. We keep a small rolling ring and expose min / avg / p99 / max so a
// HUD (or a bench) can display a live, honest number. Allocation-free, O(1) add.

#ifndef TOE_LATENCY_HPP
#define TOE_LATENCY_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace toe {

class LatencyMeter {
public:
    using Clock = std::chrono::steady_clock;

    // Monotonic "now" in microseconds — the stamp source for both ends.
    [[nodiscard]] static std::int64_t now_us() noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    // Mark that input was just written to the child at `ts` (us). Only the
    // FIRST pending mark per present matters (coalesced keystrokes share the
    // frame that answers them), so we keep the earliest outstanding stamp.
    void mark_input(std::int64_t ts) noexcept {
        if (pending_ < 0) pending_ = ts;
    }

    // Called at the present that may reflect the child's response. If an input
    // is outstanding, record (now - stamp) as one latency sample and clear it.
    void mark_present(std::int64_t ts) noexcept {
        if (pending_ < 0) return;
        const std::int64_t us = ts - pending_;
        pending_ = -1;
        if (us < 0) return;
        ring_[head_] = us;
        head_ = (head_ + 1) % kN;
        if (count_ < kN) ++count_;
        ++total_;
    }

    // A cheap late-frame drop of a stale pending mark: if input was written but
    // no present followed within `budget_us` (e.g. a rate-capped flood frame),
    // don't attribute that whole gap to input latency — reset it.
    void drop_if_stale(std::int64_t now_ts, std::int64_t budget_us) noexcept {
        if (pending_ >= 0 && now_ts - pending_ > budget_us) pending_ = -1;
    }

    [[nodiscard]] bool has_samples() const noexcept { return count_ > 0; }
    [[nodiscard]] std::uint64_t total_samples() const noexcept { return total_; }

    struct Stats {
        double min_ms{}, avg_ms{}, p99_ms{}, max_ms{};
        std::size_t n{};
    };

    // Snapshot the rolling window. O(N log N) over a ≤256 ring — trivial, and
    // only called when a HUD/bench asks (never in the hot loop).
    [[nodiscard]] Stats stats() const noexcept {
        Stats s;
        s.n = count_;
        if (count_ == 0) return s;
        std::array<std::int64_t, kN> tmp{};
        std::copy_n(ring_.begin(), count_, tmp.begin());
        std::sort(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(count_));
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < count_; ++i) sum += tmp[i];
        const auto idx99 = static_cast<std::size_t>(
            std::min<std::size_t>(count_ - 1, static_cast<std::size_t>(0.99 * (count_ - 1) + 0.5)));
        s.min_ms = tmp[0] / 1000.0;
        s.max_ms = tmp[count_ - 1] / 1000.0;
        s.avg_ms = (static_cast<double>(sum) / static_cast<double>(count_)) / 1000.0;
        s.p99_ms = tmp[idx99] / 1000.0;
        return s;
    }

private:
    static constexpr std::size_t kN = 256; // rolling window (a few seconds typing)
    std::array<std::int64_t, kN> ring_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint64_t total_ = 0;
    std::int64_t pending_ = -1; // outstanding input stamp, or -1
};

} // namespace toe

#endif // TOE_LATENCY_HPP
