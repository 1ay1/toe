// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/pty/pty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pty.h> // forkpty (glibc / util-linux)
#include <sys/ioctl.h>
#include <unistd.h>

namespace toe {

namespace {

winsize to_winsize(Extent e, int cell_w = 0, int cell_h = 0) noexcept {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(e.cols);
    ws.ws_row = static_cast<unsigned short>(e.rows);
    ws.ws_xpixel = static_cast<unsigned short>(e.cols * cell_w);
    ws.ws_ypixel = static_cast<unsigned short>(e.rows * cell_h);
    return ws;
}

Result<void> set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return fail(std::string{"fcntl(F_GETFL) failed: "} + std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return fail(std::string{"fcntl(F_SETFL) failed: "} + std::strerror(errno));
    }
    return {};
}

} // namespace

Result<Pty> Pty::spawn(std::span<const char *const> argv, Extent size) {
    SpawnCommand cmd;
    for (const char *const *p = argv.data(); p != argv.data() + argv.size() && *p; ++p) {
        cmd.argv.emplace_back(*p);
    }
    return spawn(cmd, size);
}

Result<Pty> Pty::spawn(const SpawnCommand &cmd, Extent size) {
    // Resolve argv: explicit -> $SHELL -> /bin/sh.
    std::vector<std::string> args = cmd.argv;
    if (args.empty()) {
        const char *shell = ::getenv("SHELL");
        args.emplace_back(shell && *shell ? shell : "/bin/sh");
    }
    std::vector<char *> cargv;
    cargv.reserve(args.size() + 1);
    for (auto &a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    winsize ws = to_winsize(size);
    int master = -1;
    const ::pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);

    if (pid < 0) {
        return fail(std::string{"forkpty failed: "} + std::strerror(errno));
    }

    if (pid == 0) {
        // --- child --- TERM is a host-chosen value, not hard-coded.
        ::setenv("TERM", cmd.term.empty() ? "xterm-256color" : cmd.term.c_str(), 1);
        // Host hook: setenv/chdir/setsid/drop-privs, before exec.
        if (cmd.pre_exec) cmd.pre_exec();
        ::execvp(cargv[0], cargv.data());
        // exec failed. perror is not async-signal-safe after fork; write a
        // fixed diagnostic via the raw syscall, then leave with 127.
        const char msg[] = "toe: exec failed\n";
        ssize_t ignored = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        ::_exit(127);
    }

    // --- parent ---
    if (auto nb = set_nonblocking(master); !nb) {
        ::close(master);
        ::kill(pid, SIGHUP);
        return std::unexpected(nb.error());
    }

    Pty pty;
    pty.master_ = Fd::owned(master);
    pty.child_ = Child::spawned(pid);
    return pty;
}

Result<Pty> Pty::adopt(const AdoptFd &src) {
    if (src.master_fd < 0) {
        return fail("Pty::adopt: master_fd must be >= 0");
    }
    if (auto nb = set_nonblocking(src.master_fd); !nb) {
        return std::unexpected(nb.error());
    }
    Pty pty;
    // Ownership of the master fd is the host's choice, encoded in the Fd type.
    pty.master_ = src.owns_fd ? Fd::owned(src.master_fd) : Fd::borrowed(src.master_fd);
    // We observe (but never reap) a host-managed child; if pid < 0, no child.
    pty.child_ = (src.child >= 0) ? Child::adopted(src.child) : Child::none();
    return pty;
}

ReadResult Pty::read() {
    for (;;) {
        const ssize_t n = ::read(master_.get(), rbuf_.data(), rbuf_.size());
        if (n > 0) {
            return pty::Data{std::span<const char>{rbuf_.data(), static_cast<std::size_t>(n)}};
        }
        if (n == 0) {
            return pty::Hungup{}; // EOF on the master: child closed the pty
        }
        if (errno == EINTR) {
            continue; // interrupted before any byte; retry
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return pty::WouldBlock{};
        }
        // EIO is Linux's way of reporting the child's exit on the master side.
        return pty::Hungup{};
    }
}

Result<std::size_t> Pty::write(std::string_view bytes) {
    std::size_t total = 0;
    while (total < bytes.size()) {
        const ssize_t n = ::write(master_.get(), bytes.data() + total, bytes.size() - total);
        if (n > 0) {
            total += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // kernel buffer full; caller can retry later
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return fail(std::string{"pty write failed: "} + std::strerror(errno));
    }
    return total;
}

Result<void> Pty::resize(Extent size) {
    winsize ws = to_winsize(size, cell_w_, cell_h_);
    if (::ioctl(master_.get(), TIOCSWINSZ, &ws) < 0) {
        return fail(std::string{"ioctl(TIOCSWINSZ) failed: "} + std::strerror(errno));
    }
    return {};
}

} // namespace toe
