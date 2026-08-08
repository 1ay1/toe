// SPDX-License-Identifier: LGPL-2.0-or-later
//
// PTY: spawn a child shell attached to a pseudo-terminal and shuttle bytes.
// The master fd is non-blocking so the render loop can poll it without ever
// stalling on a read.

#ifndef TOE_PTY_PTY_HPP
#define TOE_PTY_PTY_HPP

#include <span>
#include <string_view>

#include <sys/types.h>

#include "toe/core/types.hpp"
#include "toe/pty/pty_source.hpp"

namespace toe {

class Pty {
public:
    // Spawn `argv[0]` (a shell) on a fresh PTY sized to `size` cells.
    static Result<Pty> spawn(std::span<const char *const> argv, Extent size);

    // Spawn from a SpawnCommand: honors its TERM value and pre_exec hook.
    static Result<Pty> spawn(const SpawnCommand &cmd, Extent size);

    // Adopt a PTY master fd the host already owns (SSH, container, replay).
    // toe never forks. See AdoptFd for ownership semantics.
    static Result<Pty> adopt(const AdoptFd &src);

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

    // Set the per-cell pixel size, so the winsize carries ws_xpixel/ws_ypixel
    // (apps read this via TIOCGWINSZ to size images). Applied on the next
    // resize(); pass the current grid to push it immediately.
    void set_cell_pixels(int cw, int ch) noexcept { cell_w_ = cw; cell_h_ = ch; }

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
    int cell_w_{0}, cell_h_{0}; // for ws_xpixel/ws_ypixel
    bool owns_fd_{true};       // false when adopting a host-owned fd
    bool owns_child_{true};    // false when the host manages the child lifetime
};

} // namespace toe

#endif // TOE_PTY_PTY_HPP
