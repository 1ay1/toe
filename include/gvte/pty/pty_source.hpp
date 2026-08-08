// SPDX-License-Identifier: LGPL-2.0-or-later
//
// PtySource — where the child comes from, as a closed sum type.
//
// gvte used to unconditionally forkpty($SHELL) and hard-code TERM inside the
// library. That welds policy the host owns into the engine, and it excludes
// every case where the host already HAS a terminal fd: an SSH channel, a
// container's pty, a tmux/multiplexer server, a recorded session for replay.
//
// Now `Config::source` is a variant. The host chooses:
//
//   SpawnCommand — the batteries-included path: gvte forkpty()s for you, but
//                  TERM is a field (not hard-coded) and you may supply a
//                  pre_exec hook that runs in the child after fork, before
//                  exec (set env, chdir, drop privileges, setsid, ...).
//
//   AdoptFd      — you already own a PTY master fd (and know the child pid).
//                  gvte NEVER forks; it adopts the fd and drives it. Ownership
//                  of the fd transfers to gvte unless you set `owns_fd=false`.
//
// Illegal instances (empty argv, negative fd) surface through the existing
// Result<T> channel at Terminal::create — not through UB.

#ifndef GVTE_PTY_PTY_SOURCE_HPP
#define GVTE_PTY_PTY_SOURCE_HPP

#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <sys/types.h>

namespace gvte {

// The batteries-included spawn path.
struct SpawnCommand {
    // The child argv. Empty -> resolved to $SHELL, then /bin/sh.
    std::vector<std::string> argv{};

    // The TERM value advertised to the child. No longer hard-coded; a host can
    // advertise xterm-kitty, xterm-256color, its own terminfo, etc.
    std::string term = "xterm-256color";

    // Runs in the CHILD process after fork(), before exec(). The library
    // guarantees it is called with the child pty already set up. Use it to
    // setenv/chdir/setsid/drop-privs. Must be async-signal-safe. Optional.
    std::function<void()> pre_exec{};
};

// The bring-your-own-fd path.
struct AdoptFd {
    int master_fd = -1;      // an open PTY master. Must be >= 0.
    ::pid_t child = -1;      // the child pid (for exit detection / reaping), or
                             // -1 if the host manages the child's lifetime.
    bool owns_fd = true;     // gvte close()s the fd on teardown when true.
};

// The closed set of ways to obtain the child terminal.
using PtySource = std::variant<SpawnCommand, AdoptFd>;

} // namespace gvte

#endif // GVTE_PTY_PTY_SOURCE_HPP
