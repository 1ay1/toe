// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/pty/child.hpp"

#include <cerrno>

#if defined(_WIN32)
#include "toe/pty/win_io.hpp"
#else
#include <sys/wait.h>
#endif

namespace toe {

#if !defined(_WIN32)
namespace {

// Normalise a waitpid status into the shell's exit-code convention.
[[nodiscard]] ExitCode normalise(int status) noexcept {
    if (WIFEXITED(status)) return ExitCode{WEXITSTATUS(status)};
    if (WIFSIGNALED(status)) return ExitCode{128 + WTERMSIG(status)};
    return ExitCode{0};
}

} // namespace
#endif

Child Child::spawned(pid_type pid) noexcept { return Child{pid, /*reap=*/true}; }
Child Child::adopted(pid_type pid) noexcept { return Child{pid, /*reap=*/false}; }

#if defined(_WIN32)

// Windows: no zombies, no waitpid. The child's process HANDLE lives in the
// ConPTY registry slot, so exit detection is a zero-timeout wait on it. There
// is nothing to "reap" — the handle is closed with the rest of the slot.
std::optional<ExitCode> Child::try_reap(int pty_fd) noexcept {
    if (reaped_) return code_;
    if (pty_fd < 0) return std::nullopt;
    int code = 0;
    if (!win::try_exit_code(pty_fd, code)) return std::nullopt;
    code_ = ExitCode{code};
    reaped_ = true;
    return code_;
}

#else

std::optional<ExitCode> Child::try_reap(int) noexcept {
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

#endif // _WIN32

} // namespace toe
