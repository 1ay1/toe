# toe — a terminal library

`toe` is a terminal emulator as a **library**: it turns bytes into a grid of
cells. VT/ANSI parsing, the screen model, scrollback, selection, search, shell
integration, and key encoding. That's the whole scope.

It is the engine behind [`hand`](../hand).

**toe links nothing.** No window system, no GPU, no font stack, no image codec,
no process spawning. Its only dependency is the C++23 standard library:

```console
$ ldd your-app-using-toe
    libstdc++.so.6  libm.so.6  libgcc_s.so.1  libc.so.6
```

That is the point. Drawing the grid, owning the PTY, and running an event loop
are *your* job — and they're the parts that are platform-specific, opinionated,
and different for every host. A terminal's hard, portable, reusable core is the
state machine, so that is all that's here.

```cmake
find_package(toe CONFIG REQUIRED)
target_link_libraries(app PRIVATE toe::toe)
```

## The shape of it

```
            your host                        toe
    ┌──────────────────────┐      ┌────────────────────────┐
    │ PTY / ssh / recording│─────▶│ feed_output(model, …)  │
    │                      │ bytes│   vt parse → screen    │
    │ keyboard             │─────▶│ encode_key(…)          │
    │                      │◀─────│ [Cmd]: write, title,   │
    │ GPU / TUI / tests    │      │        clipboard, bell │
    │                      │◀─────│ screen.row(r) → Cells  │
    └──────────────────────┘ read │                        │
                                  └────────────────────────┘
```

Two calls in, a grid and a list of effects out:

```cpp
#include "toe/term/update.hpp"

toe::term::Config cfg;
cfg.scrollback_lines = 10000;
toe::term::Model m{cfg, toe::Extent{80, 24}};

// Feed child output. Returns the side effects it implies — it performs none.
for (const toe::Cmd &c : toe::term::feed_output(m, bytes)) {
    if (auto *w = std::get_if<toe::WriteChild>(&c)) write(pty, w->bytes);   // a DA/DSR reply
    else if (auto *t = std::get_if<toe::SetTitle>(&c)) set_title(t->title);
}

// Read the grid back out and draw it however you like.
toe::term::Palette pal;
for (const toe::term::Cell &cell : m.screen.row(toe::Row{0})) {
    const toe::Rgb fg = pal.resolve(cell.pen.fg, /*is_fg=*/true);
    draw(cell.cp, fg, toe::term::has(cell.pen.attr, toe::term::Attr::Bold));
}
```

## The one big idea: The Elm Architecture

A terminal is a machine that folds a byte stream into a grid plus a handful of
side effects (write to the child, set the title, ring the bell, touch the
clipboard). The usual way to build one is callbacks that mutate shared state
and do I/O inline — untestable, and full of "forgot to send the reply" bugs.

toe is a **pure reducer** instead. Every effect is *reified as data* — a `Cmd`
value returned from `feed_output` — and the caller interprets it:

```
feed_output : (Model, bytes) -> (Model', [Cmd])     // pure, total, testable
```

Nothing in toe writes to a file descriptor, allocates a GPU resource, or reads
a clock you don't control. So a protocol bug is a one-line assertion with no
mocks and no window:

```cpp
assert(writes(feed_output(m, "\x1b[c")) == "\x1b[?62;1;4;6;22c");  // DA1
```

Every test in `tests/` runs headless for exactly this reason, and that is a
property of the library, not of the tests.

## Layers

| Layer | Header | What it is |
|---|---|---|
| value types | `core/types.hpp` | `Coord<Tag>` phantom newtypes — a Row cannot be passed as a Col; `Result<T>` = `expected<T, Error>` |
| effects | `core/tea.hpp` | the closed `Cmd` / `Msg` sum types |
| parser | `vt/parser.hpp` | Williams DEC state machine → typed actions; `PrintRun` batches ASCII; `simd_scan.hpp` vectorizes the scan for the next control byte |
| grid | `term/rowring.hpp` | **one** arena for the viewport and the newest history rows: a scroll is pointer arithmetic plus blanking one row, never a per-line memcpy |
| history | `term/linelog.hpp` | scrollback as **logical lines**, rows a view at the current width: a resize never re-wraps history, so it costs O(screen) at any scrollback size; rows join it zero-copy (the ring's arena is frozen in place) |
| screen | `term/screen.hpp` | the reduction target — modes, margins, tabs, selection, search, reflow |
| colour | `term/palette.hpp` | the xterm 256 table and `Color` → `Rgb` resolution |
| reducer | `term/update.hpp` | `feed_output` — the public entry point |
| shell integration | `term/command_log.hpp`, `term/sbquery.hpp` | OSC 133 command blocks; a JSON-over-DCS read-out |
| keys | `input/keymap.hpp` | legacy + Kitty keyboard protocol, encoded into a caller-owned buffer (no allocation per keystroke) |

## What it emulates

xterm-class VT with the modern extensions: UTF-8 with correct display widths
from a generated Unicode Character Database table (not locale-dependent libc
`wcwidth`), full SGR (256 + truecolour, italic/strike/underline styles,
coloured underlines), the alternate screen, origin/autowrap/insert
modes, bracketed paste, focus reporting, all four mouse encodings, soft-wrap
reflow on resize (scrollback included, in O(screen): history is held as logical
lines and never re-wrapped), scroll regions with left/right margins,
synchronized output (DEC 2026), OSC 8 hyperlinks, OSC 7 cwd, OSC 133 shell
integration, OSC 10/11/12 + 4/104 dynamic colours, DEC line attributes
(double-width/height, DECALN), tab stops, insert/delete/erase/repeat, and the
full device-query set (DA/DSR/XTVERSION/XTGETTCAP/DECRQSS/DECRQM).

Wide glyphs (CJK, emoji) occupy a lead cell plus a zero-width spacer, and
combining marks attach to the preceding cell without advancing the cursor.
There is no full grapheme-cluster segmentation — width is per codepoint, as in
alacritty and kitty.

Also present, because they're model state rather than drawing: Unicode-aware
selection (word/line/block, soft-wrap aware), scrollback search with match
ranking, command-block navigation (jump to the last failed command), inline
image *protocol* handling (sixel and kitty graphics — placement, scrolling and
eviction), and a secret redactor for safe transcript capture.

## Inline images need a decoder from you

toe tracks image placements but does not decompress pictures — that is codec
work, not terminal work. Raw RGB/RGBA (kitty `f=24`/`f=32`) works out of the
box. For PNG (`f=100`), hand it the decoder your host already has:

```cpp
toe::term::set_png_decoder([](const std::uint8_t *d, std::size_t n,
                              int &w, int &h, std::vector<std::uint8_t> &rgba) {
    return my_png_decode(d, n, w, h, rgba);   // → tightly-packed RGBA8
});
```

Without one, PNG transmissions are rejected cleanly.

## Threading

None. toe contains no threads, no locks, and no atomics — it is a plain state
machine, so *you* decide its concurrency. Call it from one thread (what `hand`
does, and what a terminal wants: the grid is touched on every keystroke, every
output byte and every frame, so the right number of threads contending for it
is one), or own it per-session across a pool. toe imposes nothing either way.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Needs a C++23 compiler and nothing else. `-DTOE_BUILD_TESTS=OFF` to skip the
suite; `-DTOE_INSTALL=ON` to emit `find_package` support.

## Docs

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the design and the reasons
- **[docs/CONFORMANCE.md](docs/CONFORMANCE.md)** — what's implemented, how it
  was verified, and what it deliberately doesn't do

## License

LGPL-2.0-or-later.
