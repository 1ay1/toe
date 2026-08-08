// SPDX-License-Identifier: LGPL-2.0-or-later
//
// PollSet — a typed builder over poll(2), replacing raw `pollfd fds[3]` arrays
// and hand-maintained `nfds` counters.
//
// You `add()` named, optional fds (a negative fd is silently skipped, since
// several of ours are conditional — X11 has no repeat timer, offscreen has no
// event fd). `wait(timeout)` blocks; the result answers `ready(fd)` by fd, so
// the caller reads intent ("did the PTY wake us?") rather than array indices.
//
// This is toe's own idle-blocking helper for the reference frame loop: it polls
// the surface's event fd and the child's pty fd together. A host that drives the
// engine on its own event loop need not use it.

#ifndef TOE_CORE_POLL_SET_HPP
#define TOE_CORE_POLL_SET_HPP

#include <array>
#include <chrono>
#include <cstddef>

#include <poll.h>

namespace toe {

// A poll timeout as a first-class value: either "block forever" or a bounded
// number of milliseconds. Beats passing a bare int where -1 secretly means
// "infinite" and 0 secretly means "don't block".
class Timeout {
public:
    static constexpr Timeout forever() noexcept { return Timeout{-1}; }
    static Timeout millis(int ms) noexcept { return Timeout{ms < 0 ? 0 : ms}; }
    static Timeout from(std::chrono::milliseconds d) noexcept {
        return millis(static_cast<int>(d.count()));
    }
    [[nodiscard]] int raw() const noexcept { return ms_; }

private:
    explicit constexpr Timeout(int ms) noexcept : ms_(ms) {}
    int ms_{-1};
};

// Up to `Cap` readable fds. Fixed capacity — a terminal frontend polls a tiny,
// known set (pty, window, key-repeat), so no allocation.
template <std::size_t Cap = 4>
class PollSet {
public:
    // Register `fd` for readability. Negative fds are dropped (absent sources).
    PollSet &add(int fd) noexcept {
        if (fd >= 0 && n_ < Cap) {
            fds_[n_].fd = fd;
            fds_[n_].events = POLLIN;
            fds_[n_].revents = 0;
            ++n_;
        }
        return *this;
    }

    // Block until any registered fd is readable or the timeout elapses.
    void wait(Timeout t) noexcept { ::poll(fds_.data(), n_, t.raw()); }

    // Did `fd` report readable in the last wait()? Absent/negative fds -> false.
    [[nodiscard]] bool ready(int fd) const noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (fds_[i].fd == fd) return (fds_[i].revents & POLLIN) != 0;
        }
        return false;
    }

private:
    std::array<::pollfd, Cap> fds_{};
    std::size_t n_{0};
};

} // namespace toe

#endif // TOE_CORE_POLL_SET_HPP
