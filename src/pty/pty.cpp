// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/pty/pty.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
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
    // The host handed us a child pid but does not itself waitpid() it, so WE own
    // the reap (consume the zombie on exit). If no pid, there's no child here.
    pty.child_ = (src.child >= 0) ? Child::spawned(src.child) : Child::none();
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
        // EIO on the master reports the child's exit / pty teardown.
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
