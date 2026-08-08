// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Child — a process handle whose exit is harvested via a non-blocking waitpid.
//
// toe NEVER creates the process: the host owns forkpty/ConPTY/ssh/tmux and
// hands toe an already-open PTY master fd plus (optionally) the child pid in an
// AdoptFd. So `Child` is deliberately minimal and 100% portable POSIX: it holds
// a pid and answers one question — "has it exited, and with what code?" — with a
// non-blocking waitpid. No pidfd, no Linux syscalls, no #ifdef; every Unix host
// shares this exact code, and a non-Unix host simply passes pid == -1 and owns
// exit detection itself.
//
//   • a child WE reap (reap == true)   — try_reap() consumes the zombie;
//   • a child the host reaps (adopted) — try_reap() peeks with WNOWAIT so the
//                                        host still gets to reap it.
//
// A pid of -1 means "no child here" — try_reap() is then a no-op returning
// nullopt, and exit detection is entirely the host's responsibility.

#ifndef TOE_PTY_CHILD_HPP
#define TOE_PTY_CHILD_HPP

#include <optional>

#include <sys/types.h>

namespace toe {

// The child's terminal exit status, normalised: a plain exit code, or 128+signal
// for a signalled death (shell convention). A value, minted only by a real reap.
struct ExitCode {
    int value = 0;
};

class Child {
public:
    // A child we own the reap for. try_reap() consumes the zombie via waitpid.
    [[nodiscard]] static Child spawned(::pid_t pid) noexcept;

    // A host-managed child (AdoptFd). `pid` may be -1 (host owns the lifetime,
    // no exit detection here) or a real pid we merely OBSERVE with WNOWAIT so we
    // can report the exit without stealing the reap from the host.
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

    // Non-blocking: if the child has exited, reap it (if we own the reap) and
    // return its ExitCode; otherwise std::nullopt. Idempotent after exit.
    [[nodiscard]] std::optional<ExitCode> try_reap() noexcept;

private:
    Child() = default;
    Child(::pid_t pid, bool reap) noexcept : pid_(pid), reap_(reap) {}

    ::pid_t pid_{-1};
    bool reap_{false};   // do WE reap (spawned), or does the host (adopted)?
    bool reaped_{false}; // exit already observed + reaped
    ExitCode code_{};    // cached once reaped
};

} // namespace toe

#endif // TOE_PTY_CHILD_HPP
