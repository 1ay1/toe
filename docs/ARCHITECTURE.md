# toe Architecture

How toe is built, and why. For the surface and a usage example, see the
[README](../README.md).

## Scope, and the line it draws

toe is a terminal emulator with the terminal *emulator* removed from the
product and left as a library. It does one job:

```
bytes in  ──▶  a grid of cells + a list of effects  ──▶  out
```

Everything else is explicitly out of scope, and the test of whether that line
holds is mechanical: **toe links nothing but libstdc++**. If a
`find_package` or `target_link_libraries` ever appears in `src/CMakeLists.txt`,
the line has been crossed.

What lives in the host instead, and why:

| Not in toe | Why |
|---|---|
| the window | Wayland/X11/Win32/Cocoa/offscreen is the host's platform decision, and there are five right answers |
| drawing | a GPU renderer, a TUI, a PNG dump and a test harness are all valid consumers of a cell grid |
| fonts | shaping and rasterizing is a different domain with a different library ecosystem |
| the child process | `forkpty` is POSIX, ConPTY is Windows, and "no process at all" is right for ssh, tmux, a container, or a recorded replay |
| the event loop | whether to poll, epoll, integrate with Qt, or step a frame at a time is a host architecture choice |
| image codecs | decompressing a PNG is not terminal work (see the decoder hook below) |

Pushing these out is what makes toe *testable*: a terminal that can't draw and
can't fork has nothing to mock.

## The one big idea: The Elm Architecture

A terminal is a machine that folds two byte streams (child output, user input)
into a grid plus a handful of side effects — write to the child, set the title,
ring the bell, touch the clipboard. The classic implementation is a tangle of
callbacks that mutate shared state and perform I/O inline: untestable, and
full of "forgot to send the reply" bugs.

toe is a **pure reducer** instead. Every effect is *reified as data* — a value
of the closed `Cmd` sum type — and returned to the caller, who interprets it:

```
            ┌──────────────── pure, in toe ────────────────┐
  bytes ──▶ │  feed_output : (Model, bytes) -> (Model, [Cmd])  │ ──▶ [Cmd]
            └──────────────────────────────────────────────┘        │
                       ▲                                            │
                       │          the host interprets them          ▼
                       └──────── write(pty) · set_title · bell ◀────┘
```

`Cmd` is the complete, auditable list of everything a terminal can *do* to the
outside world (`core/tea.hpp`): `WriteChild`, `SetTitle`, `SetClipboard`,
`RingBell`, and friends. Nothing in toe touches a file descriptor, a GPU, or a
clock the caller doesn't control.

Three things follow:

1. **Tests need no mocks.** A protocol bug is one assertion:
   `writes(feed_output(m, "\x1b[c")) == "\x1b[?62;1;4;6;22c"`. Every test in
   `tests/` runs headless, with no window and no PTY.
2. **Effects are a closed list.** You cannot add a surprise side effect without
   adding a `Cmd` variant, which is a visible, reviewable change.
3. **The reply bug class disappears.** A device query can't "forget" to answer:
   the reply is the reducer's return value, not a call into a sink that may or
   may not be wired up.

The one deliberate exception is `set_png_decoder` (see below) — a hook, not a
hidden effect, and the only global in the library.

## The layers

Each layer depends only on those above it. There are no cycles, and nothing
below depends on anything outside the standard library.

```
  core/types.hpp     strong newtypes, Result<T>
  core/tea.hpp       the Cmd / Msg sum types
        │
  vt/parser.hpp      bytes      -> typed actions
  vt/simd_scan.hpp   vectorized hunt for the next control byte
        │
  term/cell.hpp      a cell: codepoint + pen
  term/width.hpp     codepoint -> display width (generated UCD table)
  term/rowring.hpp   the cell arena: viewport + scrollback as one ring
  term/screen.hpp    actions    -> grid mutations
  term/palette.hpp   Color      -> Rgb
        │
  term/update.hpp    feed_output: the public entry point
  term/command_log   OSC 133 command blocks
  term/sbquery       JSON-over-DCS read-out of those blocks
        │
  input/keymap.hpp   key events -> bytes for the child
```

### Strong types (`core/types.hpp`)

The single most common bug class in emulators is silently swapping a row for a
column, or a pixel for a cell. `Coord<Tag>` is a phantom-tagged integer
newtype, so `Row` and `Col` are distinct types and the confusion is a compile
error rather than a rendering mystery. Arithmetic is deliberately restricted to
what makes sense on a coordinate — offset, compare, difference — not full ring
arithmetic.

Fallible operations return `Result<T> = std::expected<T, Error>` rather than
throwing, so errors cross API boundaries as values.

### The parser (`vt/parser.hpp`)

A byte-driven state machine after [Paul Williams' DEC ANSI
parser](https://vt100.net/emu/dec_ansi_parser). The state set is explicit and
it emits strongly-typed `Action` values through a visitor rather than mutating a
terminal directly, which keeps it a pure function of (state, byte) and
unit-testable in isolation.

Two concessions to throughput, both measured:

- **`PrintRun`** — a contiguous run of printable ASCII becomes *one* action
  instead of one per character, avoiding a variant construction + visit +
  dispatch per byte. This is the hot path under any flood.
- **`simd_scan.hpp`** — finding the end of such a run is a vectorized scan for
  the next control byte, not a per-byte loop.

### The row ring (`term/rowring.hpp`)

The idea that makes scrolling cheap: **the scrollback and the live grid share
one contiguous arena**, organised as a ring of fixed-width row slots.

Scrolling a line off the top does not copy it anywhere. That row is already in
the ring; it simply *becomes* scrollback, and the visible window slides down by
one slot. A fresh blank row is claimed at the tail, reusing the oldest slot's
storage once the cap is reached.

So a scroll is O(1) pointer arithmetic plus blanking one row — the difference
between memory-bandwidth-bound scrolling (memcpy whole rows) and index-bound
scrolling. Addressing is by *absolute* row: 0 is the oldest scrollback line,
and the visible grid is the last `rows` of them.

Resize has two paths: `resize_keep` preserves content for height-only changes
and the alt screen, while a width change triggers soft-wrap reflow that rejoins
continuation rows and re-splits them at the new width, scrollback included.

### The screen (`term/screen.hpp`)

The reduction target, and the biggest piece: modes, margins, tabs, erase/insert
operations, the alternate screen, DEC line attributes, selection, search,
hyperlinks, and reflow. `apply(Action)` is the single mutation entry point.

Two mechanisms exist for the benefit of a host that draws:

- **`generation()`** — a monotonic counter bumped on any change, plus per-row
  epochs. A host can skip unchanged frames and rebuild only dirty rows without
  diffing the grid itself.
- **palette epochs and colour edits** — recorded so a host can react to OSC
  4/10/11/12 without rescanning every cell.

### Width (`term/width.hpp`)

Display width comes from a two-stage table generated by `tools/gen_width.py`
from the Unicode Character Database (EastAsianWidth + emoji-data + general
categories) — *not* libc `wcwidth`, which is locale-dependent, slow, and often
out of date. Lookup is two array indexings into ~11 KB, with ASCII
fast-pathed before the table is touched.

Width is 0 (combining/zero-width/control), 2 (East-Asian wide/fullwidth and
emoji presentation), or 1 (everything else, including EAW-Ambiguous, rendered
narrow as alacritty and kitty do). A wide glyph occupies a lead cell plus a
zero-width spacer; the screen keeps that pair consistent when anything
overwrites either half. Combining marks attach to the preceding cell
best-effort — there is no full grapheme-cluster segmentation.

### Shell integration (`term/command_log.hpp`, `term/sbquery.hpp`)

With OSC 133 marks, the byte stream carries structure: where a prompt begins,
where a command was entered, where its output starts, and what it exited with.
`CommandLog` records those marks and resolves them on demand into command
blocks — command text, output, cwd, exit code, duration, row span — read from
the live grid rather than buffered twice.

This is what makes a terminal queryable rather than a wall of text: jump to the
last failed command, navigate block by block, or let an agent read the session
back as JSON over DCS (`sbquery`). `term/redact.cpp` exists so that read-out
can drop things that look like secrets.

It's in toe, not the host, because it is derived from the byte stream and the
grid — it is terminal state, not presentation.

### Keys (`input/keymap.hpp`)

Key events to child bytes: the legacy encodings plus the Kitty keyboard
protocol, including application cursor mode and modifier encoding.
`encode_key` writes into a caller-owned fixed buffer and returns a span — no
allocation and no `std::string` on any keystroke.

## Images need a decoder from you

toe implements the sixel and kitty graphics *protocols*: transmission, ID
assignment, placement, scrolling with the grid, and eviction. It does not
decompress pictures, because inflate + un-filter is codec work, not terminal
work, and a host that displays images already links a decoder.

Raw RGB/RGBA (kitty `f=24`/`f=32`) therefore works out of the box; PNG
(`f=100`) requires the host to install a decoder with `set_png_decoder`, and is
rejected cleanly if none is installed. This is the only global state in the
library.

## Threading

toe has no threads, no locks, and no atomics. It is a plain state machine, so
concurrency is the host's decision: drive one from a single thread, or own one
per session across a pool.

Single-threaded is the right default for a terminal, and `hand` uses it. The
grid is touched on every keystroke, every byte of child output, and every
frame, so the number of threads that can usefully contend for it is one. A
cooperative loop also buys an invariant that is expensive to reconstruct under
locks: between any two yield points the model is internally consistent, so a
frame always draws a coherent snapshot of some real terminal state rather than
a half-applied one.

But that is a *host* policy, and toe does not impose it.

## Performance notes

The throughput work that mattered, in rough order of effect, all measured with
`tests/throughput_bench.cpp` (best-of-N, because run-to-run noise on a laptop
exceeds most real wins):

- **`PrintRun` batching** — one action per ASCII run instead of per byte.
- **The row ring** — scrolling stopped being memcpy-bound.
- **A generated width table** — replaced a double binary search, and libc
  `wcwidth` before that.
- **SIMD control-byte scan** — finds run boundaries without a per-byte branch.
- **LTO in Release** (`TOE_LTO`, on by default) — the parser, screen and reducer
  live in separate translation units that `-O3` alone can't inline through.
  Measured +5% SGR, +9% CJK, +8% cursor-addressing throughput.
- **Cheap RIS** — `reset_scrollback` does O(slots) integer writes rather than an
  O(slots × cols) arena zero-fill, because a RIS-heavy stream shouldn't be
  pathological.

`Cell` is kept at 24 bytes; the OSC 8 hyperlink id is a `uint16_t` index into a
side table specifically so it fits in existing padding.

## Testing

Sixteen test binaries, all headless, all linking only `toe::toe`:

| Test | Covers |
|---|---|
| `parser_test` | the VT state machine and its typed actions |
| `screen_test` | the grid: modes, margins, erase, SGR, OSC, selection |
| `widechar_test` | CJK/emoji widths, lead/spacer consistency |
| `utf8_test` | UTF-8 validation per Unicode Table 3-7, combining marks |
| `sgr_test` | extended colour in both wire forms, underline styles, attrs |
| `caps_test` | what toe reports matches what it implements |
| `region_test` | scroll regions, origin mode, margins, rectangle ops |
| `graphics_test` | kitty/sixel protocol bookkeeping and animation timing |
| `sbquery_test` | the agent JSON read-out: escaping, injection, encoding |
| `fuzz_test` | no byte stream crashes, hangs or corrupts the model |
| `resize_test` | resize and soft-wrap reflow, scroll position preservation |
| `tea_test` | `feed_output` returns the right `Cmd`s; config reaches the screen |
| `command_log_test` | OSC 133 command-block lifecycle |
| `redact_test` | secret detection in transcript capture |
| `keymap_test` | legacy and Kitty key encodings |
| `simd_scan_test` | the vectorized scan against a scalar reference |

Plus `throughput_bench` and `scroll_bench`, which assert nothing and exist so a
change can be measured instead of guessed at, and the differential harnesses in
`tools/` (see [CONFORMANCE.md](CONFORMANCE.md)).
