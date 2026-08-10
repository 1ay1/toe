// SPDX-License-Identifier: LGPL-2.0-or-later
//
// win_io — the Windows substrate under toe's fd-shaped PTY API.
//
// toe's PTY contract speaks `int`: AdoptFd::master_fd, Session::pty_fd(), and
// App::wait_readable(int pty_fd, …). On POSIX that int IS the kernel object.
// Windows has no such integer — it has HANDLEs, and a ConPTY is not one object
// but four (an output pipe, an input pipe, an HPCON, and the child process).
//
// Rather than smear `#ifdef` through every signature in the engine (and break
// every host that already speaks the fd contract), we keep the contract and
// change what the int MEANS on Windows: it is a dense index into this registry,
// which owns the real handles. One indirection, in one file, at the exact place
// the OS stops being POSIX.
//
// ─── why a reader thread, and why that is still the fast design ────────────
// The tempting optimisation is CreateNamedPipe(FILE_FLAG_OVERLAPPED) so reads
// are asynchronous and no thread is needed. It does not work: ConPTY documents
// its channels as SYNCHRONOUS ("processed using ReadFile and WriteFile with
// synchronous I/O ... as long as an OVERLAPPED structure is not required"), and
// in practice an overlapped output pipe makes the pseudoconsole fail to bind the
// child at all — the pty emits conhost's initial paint and nothing else, while
// the child writes to whatever console it inherited. Verified empirically
// against a canonical CreatePipe baseline.
//
// So the pipe is synchronous, and ONE dedicated reader thread does the blocking
// ReadFile. This is the same structure the docs recommend ("each of the
// communication channels is serviced on a separate thread"). Crucially it keeps
// the property the frontend actually needs: readiness is a kernel EVENT the
// reader signals, so the main loop still blocks in a single
// WaitForMultipleObjects over {pty, window, timer} and never polls.
//
// The cost is one thread and one hand-off, not a copy per byte: the reader owns
// a double buffer and swaps under a short lock, so the parser reads the filled
// buffer while the next ReadFile is already in flight.

#ifndef TOE_PTY_WIN_IO_HPP
#define TOE_PTY_WIN_IO_HPP

#if defined(_WIN32)

#include <cstddef>
#include <span>
#include <string_view>

#include "toe/core/types.hpp"

namespace toe::win {

// An opaque OS handle, carried as void* so this header never includes
// <windows.h> (which would leak `min`/`max`/`Rectangle` macros into the engine).
using Handle = void *;

// Register an already-created ConPTY as an fd-shaped slot. Starts the reader
// thread for `out`.
//   out   — our end of the pty's OUTPUT pipe. A SYNCHRONOUS pipe (see above);
//           the reader thread blocks on it.
//   in    — our end of the pty's INPUT pipe (we write user keystrokes here).
//   hpcon — the HPCON, resized on SIGWINCH-equivalent and closed on teardown.
//   proc  — the child process handle, for exit detection. May be null.
// Returns the `int` the rest of toe (and the host) uses as master_fd, or -1.
[[nodiscard]] int register_pty(Handle out, Handle in, Handle hpcon, Handle proc,
                              int cols, int rows) noexcept;

// The event that is SIGNALLED when child output is ready to be read, for the
// host's readiness wait. Null if `fd` is not a live slot. Not owned by caller.
[[nodiscard]] Handle readable_event(int fd) noexcept;

// The child process handle for `fd` (exit detection), or null.
[[nodiscard]] Handle process_handle(int fd) noexcept;

// True if `fd` names a live registry slot.
[[nodiscard]] bool is_pty_fd(int fd) noexcept;

// --- the operations Pty forwards to on Windows -----------------------------
// These mirror POSIX read/write/ioctl semantics precisely so pty.cpp's logic
// (and its ReadResult sum type) is identical on both platforms.

// Outcome of a non-blocking read, mapped onto the same three cases as POSIX.
enum class ReadStatus { Data, WouldBlock, Hungup };

// Non-blocking. On Data, `out` views the reader's buffer (valid until the next
// read on this fd). Never blocks: it only takes what the reader has already
// collected.
[[nodiscard]] ReadStatus read(int fd, std::span<const char> &out) noexcept;

// Write to the child. Returns bytes written, or -1 on a real error.
[[nodiscard]] std::ptrdiff_t write(int fd, std::string_view bytes) noexcept;

// Resize the pseudoconsole (the ConPTY equivalent of TIOCSWINSZ).
[[nodiscard]] bool resize(int fd, int cols, int rows) noexcept;

// Non-blocking exit check. Returns true and sets `code` once the child has
// exited; false while it still runs (or when there is no child).
[[nodiscard]] bool try_exit_code(int fd, int &code) noexcept;

// Close the pty: stop the reader thread, close the HPCON (which signals the
// child), and release the slot so its index can be reused.
void close(int fd) noexcept;

} // namespace toe::win

#endif // _WIN32
#endif // TOE_PTY_WIN_IO_HPP
