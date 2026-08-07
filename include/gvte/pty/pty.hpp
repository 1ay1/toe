// SPDX-License-Identifier: LGPL-2.0-or-later
//
// PTY: spawn a child shell attached to a pseudo-terminal and shuttle bytes.
// The master fd is non-blocking so the render loop can poll it without ever
// stalling on a read.

#ifndef GVTE_PTY_PTY_HPP
#define GVTE_PTY_PTY_HPP

#include <span>
#include <string_view>

#include <sys/types.h>

#include "gvte/core/types.hpp"

namespace gvte {

class Pty {
public:
    // Spawn `argv[0]` (a shell) on a fresh PTY sized to `size` cells.
    static Result<Pty> spawn(std::span<const char *const> argv, Extent size);

    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;
    Pty(Pty &&other) noexcept;
    Pty &operator=(Pty &&other) noexcept;
    ~Pty();

    // Master fd, for polling in an event loop (e.g. SDL_WaitEventTimeout side
    // channel or poll()).
    [[nodiscard]] int fd() const noexcept { return master_; }
    [[nodiscard]] ::pid_t child() const noexcept { return child_; }

    // Read available output into `buf`. Returns bytes read; 0 means "nothing
    // right now" (EAGAIN); an error means the child hung up / real failure.
    [[nodiscard]] Result<std::size_t> read(std::span<char> buf);

    // Write user input to the child. Handles partial writes.
    [[nodiscard]] Result<std::size_t> write(std::string_view bytes);

    // Inform the kernel + child of a new cell grid size (TIOCSWINSZ + SIGWINCH).
    [[nodiscard]] Result<void> resize(Extent size);

    // True once the child process has exited.
    [[nodiscard]] bool child_exited() const noexcept;

    // Reap the child and return its exit code (128+signal if signalled). Only
    // meaningful once the child has actually exited.
    [[nodiscard]] int child_exit_code() noexcept;

private:
    Pty() = default;
    void close_master() noexcept;

    int master_{-1};
    ::pid_t child_{-1};
};

} // namespace gvte

#endif // GVTE_PTY_PTY_HPP
