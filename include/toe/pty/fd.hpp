// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Fd — a move-only RAII file descriptor. Ownership is the TYPE, not a bool.
//
// A raw `int` fd carries no ownership information: whether you must close it,
// whether it's still open, whether two copies alias the same kernel object are
// all conventions a comment tries (and fails) to enforce. `Fd` makes ownership
// unforgeable:
//
//   • it is MOVE-ONLY — copying a descriptor (and thus double-close) does not
//     compile;
//   • it closes on destruction — a leak requires explicit release();
//   • it converts to a borrowed `int` for syscalls but never surrenders
//     ownership through that path;
//   • `Fd::borrowed(fd)` adopts a descriptor the caller does NOT own (an
//     AdoptFd master owned by the host) — the close is suppressed.
//
// This is the substrate every OS-object type in toe's PTY layer is built on.

#ifndef TOE_PTY_FD_HPP
#define TOE_PTY_FD_HPP

#include <utility>

#include <unistd.h>

namespace toe {

class Fd {
public:
    // The null descriptor: owns nothing, closes nothing.
    constexpr Fd() noexcept = default;

    // Take OWNERSHIP of `fd` (will be closed on destruction). Use for fds this
    // object is responsible for (a forkpty master, a pidfd, a self-pipe end).
    [[nodiscard]] static Fd owned(int fd) noexcept { return Fd{fd, /*owns=*/true}; }

    // BORROW `fd` (never closed here). Use for a host-owned AdoptFd master where
    // ownership stays with the caller.
    [[nodiscard]] static Fd borrowed(int fd) noexcept { return Fd{fd, /*owns=*/false}; }

    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    Fd(Fd &&o) noexcept : fd_(o.fd_), owns_(o.owns_) {
        o.fd_ = -1;
        o.owns_ = false;
    }
    Fd &operator=(Fd &&o) noexcept {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            owns_ = o.owns_;
            o.fd_ = -1;
            o.owns_ = false;
        }
        return *this;
    }
    ~Fd() { reset(); }

    // Borrow the descriptor for a syscall. Does NOT transfer ownership — the Fd
    // stays responsible for closing it.
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // Relinquish ownership: return the raw fd and stop tracking it (the caller
    // is now responsible for closing). Leaves this Fd null.
    [[nodiscard]] int release() noexcept {
        const int f = fd_;
        fd_ = -1;
        owns_ = false;
        return f;
    }

    // Close now (if owned) and become null.
    void reset() noexcept {
        if (owns_ && fd_ >= 0) ::close(fd_);
        fd_ = -1;
        owns_ = false;
    }

private:
    constexpr Fd(int fd, bool owns) noexcept : fd_(fd), owns_(owns) {}
    int fd_{-1};
    bool owns_{false};
};

} // namespace toe

#endif // TOE_PTY_FD_HPP
