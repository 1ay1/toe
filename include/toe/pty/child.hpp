// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Child — a process handle built on a pidfd (Linux ≥ 5.3), so the child's exit
// is a POLLABLE fd event, not a waitpid() poll.
//
// The old design asked "has the child exited?" by calling waitpid(WNOHANG) on
// every host poll — a syscall per frame, and a classic race between the check
// and the reap. `pidfd_open(pid)` returns a descriptor that becomes READABLE
// exactly when the process terminates; the host's reactor waits on it like any
// other source (see hand's TerminalWait), so exit detection costs ZERO polling
// and can never miss or double-fire. Reaping via waitid(P_PIDFD) is likewise
// race-free — the pidfd names the exact process, immune to pid reuse.
//
// Two flavours, chosen at construction:
//   • a child WE spawned (forkpty) — we own the pidfd and reap it;
//   • an AdoptFd child the host manages — we may or may not have a pid; if we do
//     we still open a pidfd for exit notification, else exit is the host's job.
//
// pidfd_open / waitid(P_PIDFD) degrade gracefully: if the kernel lacks pidfd
// support, exit_fd() is -1 (the reactor simply won't have that source) and
// try_reap() falls back to waitpid on the pid.

#ifndef TOE_PTY_CHILD_HPP
#define TOE_PTY_CHILD_HPP

#include <optional>

#include <sys/types.h>

#include "toe/pty/fd.hpp"

namespace toe {

// The child's terminal exit status, normalised: a plain exit code, or 128+signal
// for a signalled death (shell convention). A value, minted only by a real reap.
struct ExitCode {
    int value = 0;
};

class Child {
public:
    // A child we spawned and will reap. Opens a pidfd for `pid` when the kernel
    // supports it (exit becomes pollable); owns_pid means try_reap() calls
    // waitid on it.
    [[nodiscard]] static Child spawned(::pid_t pid) noexcept;

    // A host-managed child (AdoptFd). `pid` may be -1 (host owns the lifetime,
    // no exit detection here) or a real pid we merely observe (open a pidfd for
    // notification but do NOT reap — the host reaps).
    [[nodiscard]] static Child adopted(::pid_t pid) noexcept;

    // No child at all (an adopted fd with pid == -1).
    [[nodiscard]] static Child none() noexcept { return Child{}; }

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Child(Child &&) noexcept = default;
    Child &operator=(Child &&) noexcept = default;
    ~Child() = default;

    // The child pid, or -1 if unknown/none.
    [[nodiscard]] ::pid_t pid() const noexcept { return pid_; }

    // A descriptor that becomes readable when the child exits, for the host's
    // reactor to wait on. -1 if unavailable (no pid, or no kernel pidfd support)
    // — then the host must fall back to periodic try_reap().
    [[nodiscard]] int exit_fd() const noexcept { return pidfd_.get(); }

    // Non-blocking: if the child has exited, reap it (if we own the reap) and
    // return its ExitCode; otherwise std::nullopt. Idempotent after exit.
    [[nodiscard]] std::optional<ExitCode> try_reap() noexcept;

private:
    Child() = default;
    Child(::pid_t pid, bool reap) noexcept;

    ::pid_t pid_{-1};
    Fd pidfd_{};          // pollable exit notification, when available
    bool reap_{false};    // do WE reap (spawned), or does the host (adopted)?
    bool reaped_{false};  // exit already observed + reaped
    ExitCode code_{};     // cached once reaped
};

} // namespace toe

#endif // TOE_PTY_CHILD_HPP
