// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Pty — a child shell on a pseudo-terminal, with a TYPE-THEORETIC read channel.
//
// Two ideas make this fast and correct-by-construction:
//
// 1. READ AS A SUM TYPE. A raw read() conflates three outcomes in one int:
//    n>0 (data), n==0/EAGAIN (nothing now), n<0/EIO (child hung up). The old
//    API smuggled "child gone" through `!Result` and forced every caller to
//    remember the convention. `read()` now returns a closed `ReadResult`:
//
//        Data{bytes}   — a non-empty span INTO the Pty's own reusable buffer
//                        (zero-copy: no per-read allocation, no zero-fill);
//        WouldBlock{}  — the fd is dry right now (EAGAIN);
//        Hungup{}      — the child closed the pty (EIO/EOF).
//
//    std::visit forces the caller to handle the hangup — it can't be forgotten,
//    and it isn't an error-channel abuse.
//
// 2. EXIT AS A CHILD HANDLE. The child is a `Child` (child.hpp): try_reap()
//    harvests its ExitCode via waitpid without blocking. toe never forks — the
//    host owns process creation and hands toe an already-open master fd (see
//    AdoptFd), so the child's lifetime and reaping policy live with the host.
//
// The master fd is non-blocking; ownership of every descriptor is an `Fd`
// (fd.hpp), never a raw int + bool.

#ifndef TOE_PTY_PTY_HPP
#define TOE_PTY_PTY_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/types.h>

#include "toe/core/types.hpp"
#include "toe/pty/child.hpp"
#include "toe/pty/fd.hpp"
#include "toe/pty/pty_source.hpp"

namespace toe {

// --- the closed outcome of a read ------------------------------------------
namespace pty {
// Bytes are available: a view INTO the Pty's reusable read buffer, valid until
// the next read()/write() on that Pty. Never empty.
struct Data {
    std::span<const char> bytes;
};
// Nothing to read right now (EAGAIN). The fd is armed level-triggered, so the
// reactor will wake us again when more arrives.
struct WouldBlock {};
// The child closed the pty (EIO/EOF). The terminal is over; reap via child().
struct Hungup {};
} // namespace pty

using ReadResult = std::variant<pty::Data, pty::WouldBlock, pty::Hungup>;

class Pty {
public:
    // Adopt a PTY master fd the host already owns (see toe/pty/pty_source.hpp).
    // toe NEVER forks: process creation is the host's job — it forkpty()s (or
    // uses ConPTY/ssh/tmux/…) and hands the master fd + child pid in an AdoptFd.
    // This is the SOLE way to build a Pty, and it is pure POSIX fd wiring.
    [[nodiscard]] static Result<Pty> adopt(const AdoptFd &src);

    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;
    Pty(Pty &&) noexcept = default;
    Pty &operator=(Pty &&) noexcept = default;
    ~Pty() = default;

    // The master fd, for the host's reactor. Borrowed — the Pty owns it.
    [[nodiscard]] int fd() const noexcept { return master_.get(); }

    // The child handle: try_reap() harvests the ExitCode via waitpid. Only
    // meaningful when the host passed a child pid in AdoptFd.
    [[nodiscard]] Child &child() noexcept { return child_; }
    [[nodiscard]] const Child &child() const noexcept { return child_; }

    // Read available output. Returns a closed ReadResult (see above). The Data
    // span aliases an internal buffer that survives until the next read/write.
    [[nodiscard]] ReadResult read();

    // Write user input to the child. Handles partial writes.
    [[nodiscard]] Result<std::size_t> write(std::string_view bytes);

    // Inform the kernel + child of a new cell grid size (TIOCSWINSZ + SIGWINCH).
    [[nodiscard]] Result<void> resize(Extent size);

    // Set the per-cell pixel size so the winsize carries ws_xpixel/ws_ypixel.
    void set_cell_pixels(int cw, int ch) noexcept {
        cell_w_ = cw;
        cell_h_ = ch;
    }

private:
    Pty() : rbuf_(kReadBuf) {}

    static constexpr std::size_t kReadBuf = 1u << 17; // 128 KiB: big reads, few syscalls

    Fd master_{};       // the PTY master; ownership is the type
    Child child_{Child::none()};
    std::vector<char> rbuf_;    // reusable read buffer — allocated ONCE, never zero-filled
    int cell_w_{0}, cell_h_{0}; // for ws_xpixel/ws_ypixel
};

} // namespace toe

#endif // TOE_PTY_PTY_HPP
