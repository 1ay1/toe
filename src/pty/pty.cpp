// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/pty/pty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <pty.h>       // forkpty (glibc / util-linux)
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gvte {

namespace {

winsize to_winsize(Extent e) noexcept {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(e.cols);
    ws.ws_row = static_cast<unsigned short>(e.rows);
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
    if (argv.empty() || argv.front() == nullptr) {
        return fail("Pty::spawn: empty argv");
    }

    winsize ws = to_winsize(size);
    int master = -1;
    const ::pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);

    if (pid < 0) {
        return fail(std::string{"forkpty failed: "} + std::strerror(errno));
    }

    if (pid == 0) {
        // --- child ---
        ::setenv("TERM", "xterm-256color", 1);
        // argv must be null-terminated; the caller guarantees this.
        ::execvp(argv[0], const_cast<char *const *>(argv.data()));
        // Only reached if exec failed.
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

Pty::Pty(Pty &&other) noexcept : master_{other.master_}, child_{other.child_} {
    other.master_ = -1;
    other.child_ = -1;
}

Pty &Pty::operator=(Pty &&other) noexcept {
    if (this != &other) {
        close_master();
        master_ = std::exchange(other.master_, -1);
        child_ = std::exchange(other.child_, -1);
    }
    return *this;
}

Pty::~Pty() { close_master(); }

void Pty::close_master() noexcept {
    if (master_ >= 0) {
        ::close(master_);
        master_ = -1;
    }
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
    winsize ws = to_winsize(size);
    if (::ioctl(master_, TIOCSWINSZ, &ws) < 0) {
        return fail(std::string{"ioctl(TIOCSWINSZ) failed: "} + std::strerror(errno));
    }
    return {};
}

bool Pty::child_exited() const noexcept {
    if (child_ < 0) {
        return true;
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
