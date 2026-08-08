// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/pty/child.hpp"

#include <cerrno>

#include <sys/wait.h>

namespace toe {

namespace {

// Normalise a waitpid status into the shell's exit-code convention.
[[nodiscard]] ExitCode normalise(int status) noexcept {
    if (WIFEXITED(status)) return ExitCode{WEXITSTATUS(status)};
    if (WIFSIGNALED(status)) return ExitCode{128 + WTERMSIG(status)};
    return ExitCode{0};
}

} // namespace

Child Child::spawned(::pid_t pid) noexcept { return Child{pid, /*reap=*/true}; }
Child Child::adopted(::pid_t pid) noexcept { return Child{pid, /*reap=*/false}; }

std::optional<ExitCode> Child::try_reap() noexcept {
    if (reaped_) return code_; // idempotent: exit already observed
    if (pid_ <= 0) return std::nullopt;

    // Non-blocking poll. When WE own the reap, consume the zombie. For an
    // adopted child we DON'T own the reap, so use WNOWAIT to peek without
    // consuming it — the host reaps.
    int status = 0;
    const int flags = reap_ ? WNOHANG : (WNOHANG | WNOWAIT);
    const ::pid_t r = ::waitpid(pid_, &status, flags);
    if (r == pid_) {
        code_ = normalise(status);
        reaped_ = true;
        return code_;
    }
    if (r < 0 && errno == ECHILD) { // gone and already reaped by someone else
        reaped_ = true;
        return code_;
    }
    return std::nullopt; // still running
}

} // namespace toe
