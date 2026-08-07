# gvte — a GPU-accelerated terminal engine, built on The Elm Architecture

`gvte` is a from-scratch terminal emulator **library** — no VTE, no GTK, no
SDL. It owns the entire stack (PTY → escape-sequence parser → grid model →
GPU renderer) and talks to Wayland and X11 directly. It is the engine behind
[`hand`](../hand).

Its defining feature is not a feature at all — it's the **architecture**.

## The Elm Architecture, in C++

The terminal core is a **pure state machine**. Nothing in the model performs
I/O; every side effect is *reified as data* and returned from a pure `update`
function that a thin runtime interprets.

```
             ┌────────────────────────── the pure core ──────────────────────────┐
   Msg  ───▶ │  update : (Model, Msg) -> (Model, [Cmd])        view : Model -> Scene │ ──▶ GPU
             └───────────────────────────────────────────────────────────────────┘
   ▲                                    │
   │  ChildOutput, Key, Paste,          │  WriteChild, SetTitle, SetClipboard,
   │  Resized, Mouse*, ChildExited      │  ResizePty, RingBell, Quit
   │                                    ▼
   └──────────────── Runtime (the ONLY impure code) executes [Cmd] ───────────────┘
```

- **`Msg`** (`core/tea.hpp`) — a closed sum type of every input: a chunk of
  child output, a keypress, a paste, a resize, a mouse event, the child
  exiting.
- **`Cmd`** — a closed sum type of every effect: `WriteChild`, `SetTitle`,
  `SetClipboard`, `ResizePty`, `RingBell`, `Quit`. Effects are **data**, not
  actions.
- **`update`** (`term::feed_output`, `Session::update`) — pure and total. It
  mutates the `Model` and *returns* the `Cmd`s it wants performed. It never
  touches the PTY, clipboard, window or clock.
- **Runtime** (`Session::Impl::interpret`) — the single, small interpreter that
  executes `Cmd`s. It is the only code in the library that does I/O.

### Why this matters

The terminal's response to *any* byte stream is now a pure function you can
assert on with zero mocks:

```cpp
term::Model m{cfg, {80, 24}};
Cmds fx = term::feed_output(m, "\x1b[c");          // fish's DA1 query
assert(writes(fx) == "\x1b[?62;1;6;22c");           // the reply, as data
```

The whole `tea_test` suite is written this way. When fish hung for 10s on an
unanswered Primary Device Attributes query, the fix was to make the reply a
returned `WriteChild` `Cmd` instead of a buried `pty.write()` — pure,
auditable, and impossible to forget to wire up.

## Type-theoretic design

- Strong coordinate newtypes (`Row`, `Col`, `Extent`, `PixelSize`) — grid math
  cannot mix axes or confuse cells with pixels.
- `std::expected<T, Error>` at every fallible boundary (PTY, GL, fonts,
  surface).
- Sum types everywhere illegal states would otherwise be representable: the VT
  parser action, cell colour (`Default | Indexed | TrueColor`), the terminal
  lifecycle (`Running(Session) | Exited(code)`), `KeyEvent`
  (`Text | SpecialKey`).
- RAII over every C handle; `std::span` / `std::string_view` over raw
  pointer+length pairs.

## Stack

- **C++23**
- **Wayland + EGL** and **X11 (Xlib/xcb) + EGL** — direct, runtime-selected;
  no SDL, no GTK
- **OpenGL 3.3 core** — instanced quads, SDF rounded-rects (GPUI-style),
  glyph atlas
- **FreeType + HarfBuzz + Fontconfig** — rasterization and shaping
- **PCRE2**, **xkbcommon** — search regex, keymaps
- glibc **forkpty** — the child shell

## Building

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # parser, screen, render, tea
./build/gvte-demo               # a minimal host that drives the library
```

## Layout

| Path | Role |
|------|------|
| `core/tea.hpp` | `Cmd` / `Msg` sum types — the architecture's spine |
| `core/types.hpp` | strong coords, `Result<T>`, `Rgb` |
| `vt/parser.*` | Williams ANSI state machine → typed actions (incl. DCS) |
| `term/cell.hpp` | `Cell` / `Pen` / `Color` value types |
| `term/screen.*` | the grid `Model`: scroll regions, alt screen, selection |
| `term/update.*` | the **pure reducer** — `feed_output(Model, bytes) -> Cmds` |
| `gfx/*` | font atlas, palette, SDF shaders, instanced renderer (`view`) |
| `platform/*` | Wayland / X11 surfaces + input, behind one `Surface` interface |
| `terminal.*` | `Terminal` lifecycle SM + `Session` (`update` / `run`) + Runtime |

## Status

A real terminal: runs vim, tmux, htop; scroll regions, alt screen, selection
(char/word/line/block), mouse in and out, clipboard (Wayland + X11), OSC
titles/hyperlinks/52, full device-query replies (DA/DSR/XTVERSION/XTGETTCAP).
