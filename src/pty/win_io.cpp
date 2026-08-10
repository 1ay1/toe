// SPDX-License-Identifier: LGPL-2.0-or-later
//
// win_io — implementation. See win_io.hpp for the design rationale.

#if defined(_WIN32)

#include "toe/pty/win_io.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace toe::win {

namespace {

// 64 KiB per ReadFile: big enough that a flood arrives in few syscalls, small
// enough that the first bytes of an interactive echo are handed over promptly.
constexpr std::size_t kChunk = 1u << 16;

struct Slot {
    HANDLE out = nullptr;  // pty output pipe (synchronous) — reader thread reads
    HANDLE in = nullptr;   // pty input pipe — we write keystrokes here
    HPCON hpcon = nullptr; // the pseudoconsole
    HANDLE proc = nullptr; // child process, for exit detection
    HANDLE ready = nullptr; // manual-reset: signalled while bytes are pending

    // Last size pushed to ResizePseudoConsole. ConPTY REPAINTS ITS ENTIRE
    // VIEWPORT on every resize call — even a resize to the size it already has.
    // toe pushes the true grid geometry right after spawn (the pty is created
    // at a placeholder size), and on every font/padding/ligature change, so
    // without this guard a single `dir` renders twice and every command leaves
    // a duplicated block of output on screen.
    int cols = 0, rows = 0;

    std::thread reader;
    std::mutex mu;
    std::string pending;  // bytes collected by the reader, not yet consumed
    std::string consumed; // buffer handed to the parser (kept alive across read)

    std::atomic<bool> hungup{false};  // EOF/broken pipe seen by the reader
    std::atomic<bool> stopping{false};// teardown requested
    bool live = false;
    bool exited = false;
    int code = 0;
};

// Slots are heap-allocated and never moved: the reader thread holds a raw
// pointer for its whole lifetime.
std::vector<Slot *> &slots() noexcept {
    static std::vector<Slot *> v;
    return v;
}

Slot *at(int fd) noexcept {
    auto &v = slots();
    if (fd < 0 || static_cast<std::size_t>(fd) >= v.size()) return nullptr;
    Slot *s = v[static_cast<std::size_t>(fd)];
    return (s && s->live) ? s : nullptr;
}

// The reader thread body: block in ReadFile, append, signal readiness. This is
// the ONLY place that blocks on the pty; the main loop never does.
void reader_body(Slot *s) {
    // Diagnostic: set HAND_PTY_DUMP=<path> to tee the raw child stream to a
    // file. Invaluable for VT-level bugs (spurious blank lines, stray repaints)
    // where the question is "what did ConPTY actually send?".
    FILE *dump = nullptr;
    if (const char *p = std::getenv("HAND_PTY_DUMP"); p && *p) {
        dump = std::fopen(p, "wb");
    }

    std::vector<char> buf(kChunk);
    for (;;) {
        DWORD got = 0;
        const BOOL ok = ::ReadFile(s->out, buf.data(), static_cast<DWORD>(buf.size()), &got,
                                   nullptr);
        if (s->stopping.load(std::memory_order_relaxed)) break;
        if (!ok || got == 0) {
            // EOF / ERROR_BROKEN_PIPE: the child closed the pty.
            s->hungup.store(true, std::memory_order_release);
            ::SetEvent(s->ready); // wake the loop so it observes the hangup
            break;
        }
        if (dump) {
            std::fwrite(buf.data(), 1, got, dump);
            std::fflush(dump);
        }
        {
            std::lock_guard<std::mutex> lk(s->mu);
            s->pending.append(buf.data(), got);
        }
        ::SetEvent(s->ready);
    }
    if (dump) std::fclose(dump);
}

} // namespace

int register_pty(Handle out, Handle in, Handle hpcon, Handle proc, int cols, int rows) noexcept {
    if (!out || !in) return -1;

    auto *s = new (std::nothrow) Slot{};
    if (!s) return -1;
    s->out = static_cast<HANDLE>(out);
    s->in = static_cast<HANDLE>(in);
    s->hpcon = static_cast<HPCON>(hpcon);
    s->proc = static_cast<HANDLE>(proc);
    // Seed the size the pseudoconsole was CREATED with, so a later resize to
    // that same geometry is correctly recognised as a no-op.
    s->cols = cols;
    s->rows = rows;
    s->live = true;

    // Manual-reset, initially unsignalled: this is what the host's readiness
    // wait blocks on, alongside the window and timers.
    s->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s->ready) {
        delete s;
        return -1;
    }

    auto &v = slots();
    int fd = -1;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (!v[i]) {
            v[i] = s;
            fd = static_cast<int>(i);
            break;
        }
    }
    if (fd < 0) {
        v.push_back(s);
        fd = static_cast<int>(v.size() - 1);
    }

    s->reader = std::thread(reader_body, s);
    return fd;
}

Handle readable_event(int fd) noexcept {
    Slot *s = at(fd);
    return s ? static_cast<Handle>(s->ready) : nullptr;
}

Handle process_handle(int fd) noexcept {
    Slot *s = at(fd);
    return s ? static_cast<Handle>(s->proc) : nullptr;
}

bool is_pty_fd(int fd) noexcept { return at(fd) != nullptr; }

ReadStatus read(int fd, std::span<const char> &out) noexcept {
    Slot *s = at(fd);
    if (!s) return ReadStatus::Hungup;

    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (!s->pending.empty()) {
            // Swap rather than copy: the parser gets the filled buffer while the
            // reader immediately starts refilling a fresh one.
            s->consumed.clear();
            s->consumed.swap(s->pending);
            // Nothing buffered now; the reader re-signals when more arrives.
            ::ResetEvent(s->ready);
            out = std::span<const char>{s->consumed.data(), s->consumed.size()};
            return ReadStatus::Data;
        }
    }

    if (s->hungup.load(std::memory_order_acquire)) return ReadStatus::Hungup;
    ::ResetEvent(s->ready);
    return ReadStatus::WouldBlock;
}

std::ptrdiff_t write(int fd, std::string_view bytes) noexcept {
    Slot *s = at(fd);
    if (!s) return -1;
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD wrote = 0;
        const BOOL ok = ::WriteFile(s->in, bytes.data() + total,
                                    static_cast<DWORD>(bytes.size() - total), &wrote, nullptr);
        if (!ok || wrote == 0) {
            return total > 0 ? static_cast<std::ptrdiff_t>(total) : -1;
        }
        total += wrote;
    }
    return static_cast<std::ptrdiff_t>(total);
}

bool resize(int fd, int cols, int rows) noexcept {
    Slot *s = at(fd);
    if (!s || !s->hpcon) return false;
    if (cols <= 0 || rows <= 0) return false;
    // Idempotent: skip the call when nothing changed (see the note on Slot::cols)
    // so we don't trigger a redundant full-viewport repaint.
    if (cols == s->cols && rows == s->rows) return true;
    COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    if (FAILED(::ResizePseudoConsole(s->hpcon, size))) return false;
    s->cols = cols;
    s->rows = rows;
    return true;
}

bool try_exit_code(int fd, int &code) noexcept {
    Slot *s = at(fd);
    if (!s || !s->proc) return false;
    if (s->exited) {
        code = s->code;
        return true;
    }
    if (::WaitForSingleObject(s->proc, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    ::GetExitCodeProcess(s->proc, &c);
    s->exited = true;
    s->code = static_cast<int>(c);
    code = s->code;
    return true;
}

void close(int fd) noexcept {
    auto &v = slots();
    if (fd < 0 || static_cast<std::size_t>(fd) >= v.size()) return;
    Slot *s = v[static_cast<std::size_t>(fd)];
    if (!s) return;

    s->stopping.store(true, std::memory_order_release);

    // Teardown order matters, and the naive version deadlocks.
    //
    // The reader is parked in a blocking ReadFile on `out`. We must not close
    // `out` while it is blocked there (closing a handle under an in-flight
    // synchronous read is undefined and in practice never wakes the thread), and
    // we must not join before something releases that read.
    //
    // ClosePseudoConsole is the release: it terminates the child and tears down
    // the device, so the reader's ReadFile returns with a broken pipe. We follow
    // it with CancelSynchronousIo purely as a belt-and-braces wake for the case
    // where the child is already gone. Only once the reader has JOINED — and is
    // therefore no longer touching `out` — do we close the handles.
    if (s->hpcon) { ::ClosePseudoConsole(s->hpcon); s->hpcon = nullptr; }

    if (s->reader.joinable()) {
        // native_handle() is an integer type on libstdc++/mingw; reinterpret it
        // as the Win32 thread HANDLE it actually is.
        ::CancelSynchronousIo(reinterpret_cast<HANDLE>(s->reader.native_handle()));
        s->reader.join();
    }

    if (s->out) ::CloseHandle(s->out);
    if (s->in) ::CloseHandle(s->in);
    if (s->proc) ::CloseHandle(s->proc);
    if (s->ready) ::CloseHandle(s->ready);

    delete s;
    v[static_cast<std::size_t>(fd)] = nullptr;
}

} // namespace toe::win

#endif // _WIN32
