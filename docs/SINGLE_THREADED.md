# Why toe Is Single-Threaded

> *"A terminal emulator is an I/O-bound state machine, not a compute farm.
> Its one shared object — the screen grid — is touched on every keystroke,
> every byte of child output, and every frame. The correct number of threads
> contending for that object is one."*

This document explains **why toe drives an entire terminal — PTY, VT parse,
screen model, GPU render, input — from a single thread**, and why that is a
*performance* decision as much as a correctness one. It is deliberate, it is
load-bearing, and it was re-confirmed the hard way (see
[The experiment that failed](#the-experiment-that-failed)).

For *how* the loop is structured, see [ARCHITECTURE.md](ARCHITECTURE.md); the
loop itself lives in `toe/run.hpp` (`run_loop`).

---

## The model: cooperative, like Node

toe's runtime is a **single-threaded cooperative event loop** — the same shape
as Node.js, redis, nginx's worker, libuv, and every GUI toolkit's main loop.
One thread owns everything and services a small, fixed set of sources by
polling them and running each to a yield point:

```
        ┌──────────────────────── one thread, forever ───────────────────────┐
        │                                                                     │
  poll ─┤  window events → input policy → Session.update()  (mutate model)    │
        │  PTY readable   → drain bytes  → feed_output()     (mutate model)    │
        │  model changed  → render(screen) → present()       (read model)     │
        │  nothing ready  → block in poll() until something wakes us           │
        │                                                                     │
        └─────────────────────────────────────────────────────────────────────┘
```

There is exactly **one** piece of mutable state that matters — the screen
`Model` (grid, scrollback, alt screen, selection, modes). In this design it is
touched by exactly one thread, so it needs **no lock, no atomic, no memory
fence, ever**. That is the entire thesis.

---

## Why this is the *right* model for a terminal

### 1. The workload is I/O-bound, not CPU-bound

A terminal spends its life waiting: for the child to write, for the user to
type, for the compositor's next frame. The actual CPU work — parsing VT
sequences and updating a grid — is trivial: toe's model digests a **15 MB**
shred in ~82 ms (~195 MB/s) on one core, and a realistic 4 MB flood parses in
**effectively zero measured time** (see the data below). You do not reach for
threads to speed up a task that is already 100× faster than the pipe feeding it.

Threads buy you **CPU parallelism**. A terminal doesn't need CPU parallelism;
it needs **I/O concurrency** — the ability to wait on several fds at once
without blocking. A single thread with `poll(2)` already provides exactly that.
This is the classic result from the events-vs-threads literature: *non-blocking
I/O on one thread overlaps multiple I/O operations without requiring CPU
parallelism* (von Behren, Ousterhout). Threads solve a problem a terminal does
not have.

### 2. The shared state is hot and indivisible

Every input path and the render path all converge on the **same** grid:

- child output mutates it (the common case, millions of cells/sec under a flood),
- keystrokes mutate it (selection, scroll, local echo),
- the renderer reads *all* of it, every frame.

This is the worst possible shape for multithreading. The screen isn't
partitionable into independent shards you can hand to separate threads — a
single `\n` at the bottom of a scroll region physically rotates rows the
renderer is about to read. Shared, hot, and indivisible state means any
multithreaded design pays **lock contention on the critical path** and gains
nothing, because there's no independent work to run in parallel anyway.

### 3. Cooperative scheduling makes invariants free

toe's model is **The Elm Architecture**: `update(Model, Msg) -> (Model, [Cmd])`
is a pure, total function that runs to completion before the next `Msg`. On a
single cooperative thread this gives a guarantee that is *expensive* to
reconstruct under threads:

> Between any two yield points, the model is internally consistent and no other
> flow of control can observe or corrupt it.

Adya et al. call this the sweet spot of **cooperative task management** —
atomicity between yields, without locks. A frame renders a grid that is
*always* a coherent snapshot of some real terminal state, never a half-applied
scroll or a torn row, and we never wrote a line of synchronization code to make
that true. It falls out of "one thread, run each event to completion."

### 4. Determinism → testability

Because there is no preemption, toe's response to a byte stream is a
**deterministic pure function**, which is exactly what makes the whole test
suite possible:

```cpp
term::Model m{cfg, {80, 24}};
Cmds fx = term::feed_output(m, "\x1b[c");    // fish's DA1 query
assert(writes(fx) == "\x1b[?62;1;6;22c");     // the reply, as returned data
```

No PTY, no GL, no mocks, no "run it 1000× and hope the race doesn't fire."
`tea_test`, `parser_test`, and `screen_test` all run headless and
reproducibly. Under a multithreaded model this test is meaningless — the result
would depend on interleaving.

### 5. The bug you can't write

Lee, *The Problem with Threads* (2006): preemptive threads make the reachable
state space explode combinatorially with every shared access, so "wildly
nondeterministic" programs become the default and correctness the exception.
Ousterhout, *Why Threads Are a Bad Idea* (1996): even experts get locking
wrong; deadlocks, livelocks, and races are not edge cases but the normal
failure mode. A terminal has one shared object touched on every event — it is
precisely the scenario those papers warn against. In toe, a screen data race
isn't *hard to write* — it is **unrepresentable**, because there is no second
thread to race with.

---

## "But the render blocks the parse!" — measuring the real bottleneck

The seductive argument for threads is: *"while the GPU/compositor is busy
presenting a frame, the parse thread could be draining the next megabyte."* It
sounds right. It is wrong for this workload, and we have the numbers.

Profiling a 4 MB `scrollregion` flood in a real window, phase by phase
(wall-clock, one representative run):

| Phase | Time | What it is |
|-------|------|-----------|
| `poll_events` | 2.1 ms | window/input dispatch |
| **drain (read + VT parse + model update)** | **~0.0 ms** | the "work" a render thread would overlap |
| `render` | 0.3 ms | build GPU instances from the grid |
| `present` (swap) | 0.7 ms | hand the frame to the compositor |
| **`poll()` wait** | **~198 ms** | **blocked waiting for the child to produce bytes** |

The parse is *unmeasurably fast*. The render + present is ~1 ms. **~99% of the
wall time is the single thread sleeping in `poll()`, waiting for the PTY to
become readable** — i.e. waiting on the *child process and the kernel PTY
buffer*, which no amount of terminal-side threading can accelerate. A render
thread would overlap the 1 ms of GPU work with the 0 ms of parse work, hiding
*at most* a millisecond, while adding a lock on the hot grid and a cross-thread
shutdown handshake. That is a strictly losing trade.

The lesson generalises: **when the bottleneck is the pipe, not the CPU, adding
threads on the consumer side moves nothing.** You cannot parse bytes that
haven't arrived.

---

## The experiment that failed

We did not take this on faith. toe once grew a full two-thread design — an IO
thread parsing the PTY and a main thread rendering — with a *type-theoretic*
capability system (`WriteCap`/`ReadCap`/`GlCap`) engineered so that a screen
data race would be a **compile error**. It was elegant. It was also a net loss,
and it taught the sharpest version of the argument:

- **It did not improve the benchmark.** Still ~190 ms, because — per the table
  above — the time was in the `poll()` wait on the child, not in anything a
  second thread could overlap.
- **It hung on shutdown.** The capability tokens correctly made data races
  uncompilable, but *liveness is not a data-race property*. "The main thread
  must eventually observe the IO thread's stop" is a **protocol** invariant the
  type system said nothing about — it lived in raw `std::atomic<bool>` flags
  outside the types. So it compiled and **deadlocked**. This is the crux: even a
  strong type system for shared-memory access does not buy you the coordination
  correctness that a single thread gives you *for free*.
- **It added a lock, a façade, a second event router, and a thread-handoff
  protocol** — hundreds of lines — to guard a race **that only existed because
  we added the second thread.** All of that complexity was self-inflicted.

The entire experiment was reverted. The single-threaded loop it replaced was
faster to reason about, had no shutdown handshake to get wrong, and performed
identically. That is the empirical core of this document.

---

## Where the real speed comes from (single-threaded levers)

Because the bottleneck is I/O cadence and the compositor, the wins are all
*scheduling* wins on the one thread — not parallelism:

- **Drain the PTY dry per wake.** When the fd is readable, loop `pump_output()`
  until it's empty (yielding at a byte budget so input/render still get a turn),
  instead of one gulp per event-loop turn. Throughput becomes PTY-bound, not
  loop-bound.
- **Coalesce, don't present-per-chunk.** Under a flood, cap presents to a frame
  cadence and skip the idle `poll` — one frame can absorb many megabytes of
  model change. Rendering the *latest* grid once beats rendering every
  intermediate state.
- **Render only on a damage-generation change.** A monotonic `generation`
  counter + blink phase fold into one `RenderKey`; equal keys across frames mean
  "nothing to draw," so an idle terminal costs zero GPU work.
- **Zero-copy model.** The `RowRing` storage means a scroll rotates row pointers
  instead of copying cells — the parse stays fast enough that it never becomes
  the bottleneck, which is *precisely what lets single-threaded work*.

None of these needs a second thread. All of them would be *harder* with one
(each becomes a cross-thread protocol).

---

## When we would reconsider

Intellectual honesty: single-threaded is right *for this workload*. It would
stop being right if the per-frame CPU work grew past a frame budget on one
core — e.g. heavy per-glyph shaping of complex scripts, or full-window
subpixel/gamma compositing of enormous surfaces. If `render`'s *CPU* cost (not
its GPU/compositor wait) ever exceeded ~8 ms/frame, a render thread would start
to pay for itself. Today it is ~0.3 ms. Until the profiler says otherwise, a
second thread is complexity we have measured to be worthless.

The rule, stated once: **add a thread only when a profiler shows a CPU-bound
phase longer than a frame that can run independently of the shared grid.**
A terminal has no such phase.

---

## Summary

| Claim | Why it holds for a terminal |
|-------|-----------------------------|
| Workload is I/O-bound | Parse is ~195 MB/s; the pipe and compositor are the limits |
| Shared state is hot & indivisible | One grid, touched by every event and every frame; not shardable |
| Cooperative → invariants are free | TEA `update` runs to completion; no torn frames, no locks |
| Deterministic → testable | Response to bytes is a pure function; the whole suite depends on it |
| Races are unrepresentable | No second thread ⇒ no data race to guard against |
| Threads wouldn't even help | 99% of flood time is `poll()` waiting on the child, not CPU |

toe is single-threaded the way Node is single-threaded: on purpose, because the
job is to wait on I/O and mutate one hot object — and for that job, one thread
is not a limitation, it is the design.

### References

- H. C. Lauer, R. M. Needham, *On the Duality of Operating System Structures* (1979) — threads and events are duals; equal performance under equal scheduling.
- J. Ousterhout, *Why Threads Are a Bad Idea (for most purposes)* (1996).
- E. A. Lee, *The Problem with Threads* (2006) — preemptive interleaving explodes the state space.
- A. Adya et al., *Cooperative Task Management without Manual Stack Management* (2002) — the cooperative sweet spot.
- R. von Behren et al., *Why Events Are a Bad Idea (for high-concurrency servers)* (2003) — the counterpoint; note its case is thousands of independent connections, the opposite of a terminal's single hot grid.
