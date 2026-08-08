# toe — a GPU terminal engine that renders anywhere

`toe` is a from-scratch terminal emulator **library** — no VTE, no GTK, no
SDL. It owns the entire stack (PTY → escape-sequence parser → grid model →
GPU renderer) and is the engine behind [`hand`](../hand).

It renders a live terminal into **anything that can hold an OpenGL context** —
a Wayland or X11 window, an offscreen framebuffer, a texture inside your own
engine's UI, a headless CI job taking screenshots. Not "any project": any
*surface*, anywhere. The engine (`toe::core`) knows nothing about your window
system, does not own your GL context, and does not decide how your shell is
spawned. The three things a terminal library should *not* dictate — **the
surface, the GL context, and the child process** — are handed to the host, and
each boundary is encoded in the type system so illegal wiring is a compile
error.

```cmake
find_package(toe CONFIG REQUIRED COMPONENTS Core)          # bring your own window
find_package(toe CONFIG REQUIRED COMPONENTS Core Platform) # or use the Linux backends
target_link_libraries(app PRIVATE toe::core [toe::platform])
```

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the Elm-architecture core and the type-theoretic design.
- **[docs/INTEGRATION.md](docs/INTEGRATION.md)** — how to embed toe, with a complete minimal host.
- **[docs/API.md](docs/API.md)** — the public surface, header by header.

## Two libraries, one strict boundary

| Target | What it is | Links |
|--------|-----------|-------|
| **`toe::core`** | the whole terminal — pty, VT parser, grid model, TEA reducer, inline graphics, GPU renderer, keymap — **plus the `Surface` contract** | freetype, harfbuzz, fontconfig, libpng, epoxy (all **private**) |
| **`toe::platform`** *(optional)* | batteries-included Linux `Surface` backends: Wayland / X11 / offscreen EGL + `open_surface()` | core + wayland, x11, xcb, xkbcommon, egl |

`toe::core` links **zero** windowing libraries. A host on macOS, Windows, or an
embedded compositor consumes `toe::core` and provides its own `Surface`; a
Linux terminal like `hand` also links `toe::platform` and gets a window for
free. The dependency arrow only ever points `core ← platform ← host`.

## The three boundaries that give the host all the power

### 1. `Surface` is a concept, not a base class

Bring your own window (GLFW, Qt, SDL, Win32, Cocoa) by *modelling* the
`toe::platform::Surface` **C++23 concept** — a structural contract. No
inheritance, no vtable forced on your type, no toe header pulled into your
window class beyond the contract. Optional capabilities (title, clipboard,
key-repeat, flush) are refinement concepts, so a minimal host implements five
methods. `AnySurface` offers opt-in type erasure for hosts that want runtime
polymorphism. The shipped Wayland/X11 backends are just models of the same
concept.

### 2. `RenderContext` — a capability token for the GL context

`Session::render()` requires a `gfx::RenderContext&`. Its existence is the
*proof* that a GL context is current on this thread — a fact the compiler now
checks instead of a comment nobody read. The token also carries a strong-typed
target `Framebuffer`, so the host says exactly where the terminal composites
(default: the window's back buffer; or an offscreen FBO / a texture you own).

```cpp
auto rc = toe::gfx::RenderContext::adopt_current();  // "my GL context is live"
session.render(rc, px);                                // impossible to call without proof
```

### 3. `AdoptFd` — the host opens the child, toe drives it

toe never `fork`s. The host opens a PTY master by whatever native means —
`forkpty` on Linux/macOS, ConPTY on Windows, an SSH channel, a container, a
recorded session — and hands it to toe. `Config::source` is an `AdoptFd`:

```cpp
Config cfg;
cfg.source = AdoptFd{ .master_fd = my_master, .child = my_pid, .owns_fd = true };
```

So the engine carries no `<pty.h>`, no `forkpty`, no OS branch. argv/`$SHELL`,
`TERM`, and any post-fork child hook are host policy (see hand's
`posix_pty.cpp`).

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

The terminal's response to *any* byte stream is a pure function you can assert
on with zero mocks:

```cpp
term::Model m{cfg, {80, 24}};
Cmds fx = term::feed_output(m, "\x1b[c");     // fish's DA1 query
assert(writes(fx) == "\x1b[?62;1;6;22c");      // the reply, as data
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full treatment.

## Type-theoretic design

- Strong coordinate newtypes (`Row`, `Col`, `Extent`, `PixelSize`) — grid math
  cannot mix axes or confuse cells with pixels.
- `std::expected<T, Error>` at every fallible boundary (PTY, GL, fonts,
  surface). No exceptions across the seams; no half-built objects.
- Sum types everywhere illegal states would otherwise be representable: VT
  parser action, cell colour (`Default | Indexed | TrueColor`), terminal
  lifecycle (`Running(Session) | Exited(code)`), `KeyEvent`
  (`Text | SpecialKey`), and the `Surface` `Event`.
- RAII over every C handle; `std::span` / `std::string_view` over raw
  pointer+length pairs.

## Library hygiene

toe::core behaves like a guest in the host's process:

- **No environment reads for policy.** Backend selection is an explicit
  `Backend` argument; the persistent-mapping escape hatch is
  `Renderer::set_persistent_mapping()`, not an env var. toe never forks, so it
  reads neither `$SHELL` nor `$TERM` — the host resolves those before handing
  toe an `AdoptFd`.
- **No `abort()` / `exit()`** across the API. Fallible paths return
  `Result<T>`; internal invariants use debug `assert` that compiles out.
- **No stray stderr.** Diagnostics travel in `Error::message`. The one
  post-`fork` write is async-signal-safe.
- **Per-instance C handles.** Each `FontAtlas` owns its own `FT_Library`; the
  only process-global touch is Fontconfig's idempotent, refcounted `FcInit()`.
- **Fully packaged.** `install()` + `find_package(toe COMPONENTS Core [Platform])`,
  versioned SONAMEs, pkg-config. Private deps stay private — no GL type leaks
  through a public header.

## Building

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # parser, screen, tea, keymap, render, cache
./build/toe-demo               # a minimal host that drives the library
```

Useful options:

| Option | Default | Effect |
|--------|---------|--------|
| `TOE_BUILD_PLATFORM` | `ON` | build the Linux Surface backends |
| `TOE_BUILD_DEMO` | top-level | build `toe-demo` (needs platform) |
| `TOE_BUILD_TESTS` | top-level | build the test suite |
| `TOE_INSTALL` | top-level | emit install + `find_package` rules |

A core-only build has no windowing dependencies:

```sh
cmake -S . -B build -DTOE_BUILD_PLATFORM=OFF -DTOE_BUILD_DEMO=OFF
# libtoe links no wayland / xcb / x11 / egl
```

## Layout

| Path | Role |
|------|------|
| `core/tea.hpp` | `Cmd` / `Msg` sum types — the architecture's spine |
| `core/types.hpp` | strong coords, `Result<T>`, `Rgb` |
| `vt/parser.*` | Williams ANSI state machine → typed actions (incl. DCS) |
| `term/screen.*` | the grid `Model`: scroll regions, alt screen, selection |
| `term/update.*` | the **pure reducer** — `feed_output(Model, bytes) -> Cmds` |
| `pty/pty_source.hpp` | `PtySource` — spawn a shell or adopt an fd (**boundary 3**) |
| `gfx/render_target.hpp` | `RenderContext` capability token (**boundary 2**) |
| `gfx/*` | font atlas, palette, SDF shaders, instanced renderer (`view`) |
| `platform/surface.hpp` | the `Surface` **concept** + `Event` + `AnySurface` (**boundary 1**) |
| `platform/*.cpp` | Wayland / X11 / offscreen backends (`toe::platform`) |
| `terminal.*` | `Terminal` lifecycle SM + `Session` + Runtime |

## Status

A real terminal: runs vim, tmux, htop; **reflow on resize** (logical lines
rewrap when the width changes, scrollback included), scroll regions, left/right
margins (DECSLRM/DECLRMM, DECBI/DECFI), alt screen,
selection (char/word/line/block), mouse in and out, clipboard (Wayland + X11),
OSC titles/hyperlinks/52, working directory (OSC 7), shell integration marks
(OSC 133), dynamic colours (OSC 4/104 palette, 10/11 default
fg/bg, 12/112 cursor), cursor shapes (DECSCUSR: block/underline/bar, steady or
blinking), the Kitty keyboard protocol end-to-end (progressive-enhancement flag
stack + disambiguating CSI-u encoding, event types incl. key-release from the
Wayland/X11 backends, report-all-keys), programming ligatures (HarfBuzz calt
shaping), curly/coloured underlines, synchronized output, soft reset (DECSTR),
rectangular area ops (DECFRA/DECERA/DECCARA/DECRARA/DECRQCRA), REP, DEC line
attributes (double-width/height, DECALN), full device-query
replies (DA/DSR/XTVERSION/XTGETTCAP/DECRQSS/DECRQM), inline images (sixel +
kitty graphics) with animation. IME preedit: a host-drivable composition string
(`Session::set_preedit`) rendered inline at the cursor; the Wayland backend
drives it from **text-input-v3** (real fcitx/ibus input methods) when the
compositor offers it, falling back to xkb dead-key / Compose sequences
(´+e = é) otherwise.

## License

LGPL-2.0-or-later.
