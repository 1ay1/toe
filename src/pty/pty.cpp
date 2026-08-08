// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/pty/pty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pty.h>       // forkpty (glibc / util-linux)
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gvte {

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
        std::perror("execvp");
        ::_exit(127);
    }

    // --- parent ---
    if (auto nb = set_nonblocking(master); !nb) {
        ::close(master);
        ::kill(pid, SIGHUP);
        return std::unexpected(nb.error());
    }

    Pty pty;
    pty.master_ = master;
    pty.child_ = pid;
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
    pty.master_ = src.master_fd;
    pty.child_ = src.child;
    pty.owns_fd_ = src.owns_fd;
    pty.owns_child_ = (src.child >= 0); // only reap children we were told about
    return pty;
}

Pty::Pty(Pty &&other) noexcept
    : master_{other.master_}, child_{other.child_}, cell_w_{other.cell_w_},
      cell_h_{other.cell_h_}, owns_fd_{other.owns_fd_}, owns_child_{other.owns_child_} {
    other.master_ = -1;
    other.child_ = -1;
}

Pty &Pty::operator=(Pty &&other) noexcept {
    if (this != &other) {
        close_master();
        master_ = std::exchange(other.master_, -1);
        child_ = std::exchange(other.child_, -1);
        cell_w_ = other.cell_w_;
        cell_h_ = other.cell_h_;
        owns_fd_ = other.owns_fd_;
        owns_child_ = other.owns_child_;
    }
    return *this;
}

Pty::~Pty() { close_master(); }

void Pty::close_master() noexcept {
    if (master_ >= 0 && owns_fd_) {
        ::close(master_);
    }
    master_ = -1;
}

Result<std::size_t> Pty::read(std::span<char> buf) {
    const ssize_t n = ::read(master_, buf.data(), buf.size());
    if (n > 0) {
        return static_cast<std::size_t>(n);
    }
    if (n == 0) {
        return fail("pty: child closed the connection");
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return static_cast<std::size_t>(0);
    }
    if (errno == EIO) {
        // Linux reports the child's exit as EIO on the master side.
        return fail("pty: child exited (EIO)");
    }
    return fail(std::string{"pty read failed: "} + std::strerror(errno));
}

Result<std::size_t> Pty::write(std::string_view bytes) {
    std::size_t total = 0;
    while (total < bytes.size()) {
        const ssize_t n = ::write(master_, bytes.data() + total, bytes.size() - total);
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
    if (::ioctl(master_, TIOCSWINSZ, &ws) < 0) {
        return fail(std::string{"ioctl(TIOCSWINSZ) failed: "} + std::strerror(errno));
    }
    return {};
}

bool Pty::child_exited() const noexcept {
    if (child_ < 0) {
        return true;
    }
    if (!owns_child_) {
        return false; // host owns the child; we don't reap or judge it
    }
    int status = 0;
    const ::pid_t r = ::waitpid(child_, &status, WNOHANG);
    return r == child_ || (r < 0 && errno == ECHILD);
}

int Pty::child_exit_code() noexcept {
    if (child_ < 0) {
        return 0;
    }
    int status = 0;
    const ::pid_t r = ::waitpid(child_, &status, 0); // child is known-dead; reap it
    child_ = -1;
    if (r <= 0) {
        return 0;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 0;
}

} // namespace gvte
