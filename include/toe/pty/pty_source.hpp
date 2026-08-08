// SPDX-License-Identifier: LGPL-2.0-or-later
//
// AdoptFd — the ONE way toe obtains its child terminal: the host hands it an
// already-open PTY master fd.
//
// toe is the portable engine (VT parse · screen · GPU render · keymap). It does
// NOT create processes — forkpty is a POSIX facility on Linux/macOS, ConPTY on
// Windows, and "no fork at all" for an SSH channel, a container pty, a
// tmux/multiplexer server, or a recorded session for replay. Baking any one of
// those into the engine would weld host policy into the library and drag an OS
// branch through toe's source.
//
// So process creation lives entirely in the HOST. The host opens the master fd
// by whatever native means, spawns the child, and hands both to toe in an
// AdoptFd. toe never forks; it adopts the fd and drives it. Ownership of the fd
// transfers to toe unless `owns_fd == false`. Illegal instances (negative fd)
// surface through Result<T> at Terminal::create — never as UB.

#ifndef TOE_PTY_PTY_SOURCE_HPP
#define TOE_PTY_PTY_SOURCE_HPP

#include <sys/types.h>

namespace toe {

// The bring-your-own-fd terminal source — the sole way to obtain the child.
struct AdoptFd {
    int master_fd = -1;      // an open PTY master. Must be >= 0.
    ::pid_t child = -1;      // the child pid (for exit detection / reaping), or
                             // -1 if the host manages the child's lifetime.
    bool owns_fd = true;     // toe close()s the fd on teardown when true.
};

} // namespace toe

#endif // TOE_PTY_PTY_SOURCE_HPP
