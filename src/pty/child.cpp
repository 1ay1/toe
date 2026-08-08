// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/pty/child.hpp"

#include <cerrno>

#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace toe {

namespace {

// pidfd_open(2) — no glibc wrapper on older toolchains, so go via syscall. On a
// kernel without it (< 5.3) this returns -1/ENOSYS and we degrade to waitpid.
[[nodiscard]] int open_pidfd(::pid_t pid) noexcept {
#ifdef __NR_pidfd_open
    if (pid <= 0) return -1;
    const long fd = ::syscall(__NR_pidfd_open, pid, 0u);
    return fd < 0 ? -1 : static_cast<int>(fd);
#else
    (void)pid;
    return -1;
#endif
}

// Normalise a waitpid/waitid status into the shell's exit-code convention.
[[nodiscard]] ExitCode normalise(int status) noexcept {
    if (WIFEXITED(status)) return ExitCode{WEXITSTATUS(status)};
    if (WIFSIGNALED(status)) return ExitCode{128 + WTERMSIG(status)};
    return ExitCode{0};
}

} // namespace

Child::Child(::pid_t pid, bool reap) noexcept : pid_(pid), reap_(reap) {
    if (const int fd = open_pidfd(pid); fd >= 0) pidfd_ = Fd::owned(fd);
}

Child Child::spawned(::pid_t pid) noexcept { return Child{pid, /*reap=*/true}; }
Child Child::adopted(::pid_t pid) noexcept { return Child{pid, /*reap=*/false}; }

std::optional<ExitCode> Child::try_reap() noexcept {
    if (reaped_) return code_; // idempotent: exit already observed
    if (pid_ <= 0) return std::nullopt;

    // Race-free reap via the pidfd when we have one and we own the reap.
    // P_PIDFD is an idtype_t enumerator (always declared); the runtime guard is
    // pidfd_.valid(), which is false on kernels without pidfd_open support.
    if (reap_ && pidfd_.valid()) {
        ::siginfo_t si{};
        si.si_pid = 0;
        // WNOHANG: don't block; WEXITED: report a terminated child. P_PIDFD
        // names the exact process — immune to pid reuse.
        if (::waitid(P_PIDFD, static_cast<::id_t>(pidfd_.get()), &si, WEXITED | WNOHANG) == 0) {
            if (si.si_pid == 0) return std::nullopt; // still running
            code_ = (si.si_code == CLD_EXITED) ? ExitCode{si.si_status}
                                               : ExitCode{128 + si.si_status};
            reaped_ = true;
            return code_;
        }
        if (errno != ECHILD) return std::nullopt; // transient; try again later
        reaped_ = true;                            // ECHILD: already reaped elsewhere
        return code_;
    }

    // Fallback (no pidfd, or we merely observe an adopted child): poll waitpid.
    // For an adopted child we DON'T own the reap, so use WNOWAIT to peek without
    // consuming the zombie — the host will reap it.
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
