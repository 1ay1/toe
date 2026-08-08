# toe Architecture

This document explains *how* toe is built and *why* it's built that way. For
embedding it, see [INTEGRATION.md](INTEGRATION.md); for the exact public
surface, see [API.md](API.md).

## The one big idea: The Elm Architecture

A terminal emulator is a machine that turns two byte streams (child output, user
input) into a grid of cells and a handful of side effects (write to the child,
set the title, ring the bell, touch the clipboard). The classic way to write
one is a tangle of callbacks that mutate shared state and perform I/O inline —
untestable, and full of "forgot to send the reply" bugs.

toe instead uses **The Elm Architecture (TEA)**: a *pure* core and a *thin*
impure runtime.

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

### The pieces

| Piece | Where | What it is |
|-------|-------|-----------|
| `Model` | `term::Model` (`term/screen.hpp`) | all terminal state: grid, scrollback, alt screen, scroll regions, selection, charsets, modes |
| `Msg` | `core/tea.hpp` | closed sum of every input: `ChildOutput`, `Key`, `Paste`, `Resized`, `MouseDown/Up/Drag/Wheel`, `ChildExited`, `Tick` |
| `Cmd` | `core/tea.hpp` | closed sum of every *effect as data*: `WriteChild`, `SetTitle`, `SetClipboard`, `ResizePty`, `RingBell`, `Quit` |
| `update` | `term::feed_output`, `Session::update` | **pure, total** — mutates `Model`, *returns* the `Cmd`s it wants performed. Never does I/O. |
| `view` | `gfx::Renderer::draw` | **pure** — turns `Model` into GPU draw calls. |
| Runtime | `Session::Impl` interpreter | the **only** impure code — executes `Cmd`s against the PTY / clipboard / window. |

### Why it pays off

**Testability.** The terminal's response to any byte stream is a pure function:

```cpp
term::Model m{cfg, {80, 24}};
Cmds fx = term::feed_output(m, "\x1b[c");     // fish's DA1 query
assert(writes(fx) == "\x1b[?62;1;6;22c");      // the reply — as returned data
```

No PTY, no GL, no mocks. The whole `tea_test`, `parser_test`, and `screen_test`
suites run headless this way.

**Auditability.** Every effect the terminal can produce is one of a closed list
of `Cmd` alternatives. You can't accidentally bury a `pty.write()` deep in a
parser branch — a reply is a `WriteChild` value handed back to the runtime, or
it doesn't happen. (This is a real bug toe fixed: fish hung 10s on an
unanswered Primary Device Attributes query; the fix was to *return* the reply
as a `Cmd` instead of forgetting to write it.)

**Localised impurity.** All I/O lives in one small interpreter. Everything else
is a value transformation you can reason about in isolation.

**Single-threaded on purpose.** That one impure interpreter, the parse, and the
render all run on **one cooperative thread** — the model (the one hot shared
object) is therefore lock-free by construction. This is a measured decision, not
an oversight: a terminal is I/O-bound (~99% of a flood is spent in `poll()`
waiting on the child, not in CPU), so a second thread overlaps nothing and only
adds a lock on the critical path. The full argument, the profiling data, and the
two-thread experiment we reverted are in [SINGLE_THREADED.md](SINGLE_THREADED.md).

## The layering: core vs platform

toe is two libraries with a one-way dependency.

```
        ┌─────────────── your host (hand, demo, your app) ──────────────┐
        │        owns the event loop; wires the surface to the session   │
        └───────┬───────────────────────────────────────────┬──────────┘
                │ uses                                        │ links (optional)
                ▼                                             ▼
        toe::core                                     toe::platform
   pty · vt · screen · update · graphics ·        WaylandSurface / X11Surface /
   gfx renderer · keymap · terminal facade        OffscreenSurface + open_surface()
   + the Surface CONCEPT + RenderContext          (models the concept from core)
                ▲                                             │
                └───────────── does NOT depend on ────────────┘
```

- **`toe::core` never names a windowing type.** No `wl_*`, no `xcb_*`, no EGL.
  It depends only on content libraries (freetype, harfbuzz, fontconfig, libpng)
  and the GL *loader* (epoxy) — all as **private** link deps, so no consumer
  inherits their headers.
- **`toe::platform` is one implementation of core's `Surface` contract.** It is
  optional (`TOE_BUILD_PLATFORM`). A host on another OS provides its own model
  of the concept and never builds it.

The `Surface` *contract* lives in core (it's the API); the *concrete backends*
live in platform (they're a convenience). That distinction is the whole design.

## The three type-enforced host boundaries

toe hands the host control over the three things a terminal library should not
dictate. Each is a type, so misuse fails to compile rather than at runtime.

### 1. `Surface` — a concept (`platform/surface.hpp`)

A window is anything that can present a frame, report its pixel size, drain
native events into the neutral `Event` sum type, and expose a pollable fd. That
is expressed as a **C++23 concept**, not a base class:

```cpp
template <typename S>
concept Surface = requires(S s, const S cs, const EventSink &sink) {
    { s.swap() }             -> std::same_as<void>;
    { cs.pixel_size() }      -> std::same_as<PixelSize>;
    { s.poll_events(sink) }  -> std::same_as<void>;
    { cs.event_fd() }        -> std::convertible_to<int>;
    { cs.should_close() }    -> std::convertible_to<bool>;
};
```

A host models it structurally — no inheritance, no vtable, no toe type in the
host's window class. Optional capabilities are refinement concepts
(`TitledSurface`, `ClipboardSurface`, `RepeatingSurface`, `FlushableSurface`)
resolved through uniform free functions (`platform::title(s, …)` etc.), so a
minimal host implements only the five required methods. `AnySurface` is an
opt-in type-erased wrapper for hosts that need runtime polymorphism; it *also*
models `Surface`, so it flows anywhere the concept is accepted.

### 2. `RenderContext` — a capability token (`gfx/render_target.hpp`)

`Session::render()` issues GL calls, which has an invisible precondition: a GL
context must be current on the calling thread. toe turns that into a value:

```cpp
class RenderContext {              // move-only, non-owning
  static RenderContext adopt_current(Framebuffer target = default_framebuffer);
  Framebuffer target() const;
  RenderContext &retarget(Framebuffer);
};
void Session::render(gfx::RenderContext &rc, PixelSize px, ...);
```

You obtain a token by *claiming* the context is live (`adopt_current()`), and
`render()` consumes it. No token, no render — the precondition is now a type.
The token also carries a strong-typed target `Framebuffer` (0 = the window's
back buffer, or any FBO the host names), giving the host a type-checked
"composite the terminal into a surface I control" path.

### 3. `PtySource` — a sum type (`pty/pty_source.hpp`)

Where the child comes from is the host's policy, so it's a closed sum:

```cpp
struct SpawnCommand { std::vector<std::string> argv; std::string term; std::function<void()> pre_exec; };
struct AdoptFd      { int master_fd; pid_t child; bool owns_fd; };
using  PtySource    = std::variant<SpawnCommand, AdoptFd>;
```

`SpawnCommand` is the batteries-included path — toe `forkpty`s, but `TERM` and
a `pre_exec` child hook are fields, not hard-coded. `AdoptFd` hands toe a PTY
you already own (SSH, container, replay) and it never forks. `Config::source`
selects between them.

## The terminal lifecycle: illegal states unrepresentable

`Terminal` is a two-state machine encoded as a `variant`, not a `bool`:

```
Terminal = Running(Session) | Exited(code)
```

Operations that only make sense on a live terminal — `render`, `resize`,
`send_key`, `update` — live on `Session` and are simply *absent* from `Exited`.
The only transition is `poll()`, which returns a `Poll{ Session* running;
const Exited* exited; }` where exactly one pointer is non-null:

```cpp
Terminal::Poll p = term.poll();
if (p.exited) return p.exited->code;   // dead: can't render or type into it
Session &s = *p.running;               // alive: borrow it for this frame
```

You cannot render a dead terminal — there is no `Session` to call it on. It's a
compile error, not a runtime guard.

## Type-theoretic throughline

- **Strong coordinate newtypes** (`Row`, `Col`, `Extent`, `PixelSize`,
  `Framebuffer`): a phantom `Tag` makes each axis a distinct type, so grid math
  can't swap a row for a column or a cell for a pixel.
- **`std::expected<T, Error>`** at every fallible boundary (PTY, GL, fonts,
  surface). No exceptions across the seams; no half-constructed objects.
- **Sum types** wherever illegal states would otherwise be representable: VT
  parser action, cell colour (`Default | Indexed | TrueColor`), `KeyEvent`
  (`Text | SpecialKey`), the `Event` stream, `PtySource`, the terminal
  lifecycle.
- **RAII** over every C handle; **`std::span`/`std::string_view`** over raw
  pointer+length pairs.

## Rendering

A GPUI-style instanced renderer: one draw call emits every cell as an instanced
quad; backgrounds are SDF rounded-rects; glyphs come from a shelf-packed R8
atlas (FreeType rasterization, HarfBuzz shaping, Fontconfig fallback for
CJK/emoji/symbols). It targets OpenGL 3.3 core, with an optional GL 4.4
persistent-mapped instance ring for zero-copy uploads (auto-detected; opt out
with `Renderer::set_persistent_mapping(false)`). Rendering is damage-driven —
`Session::generation()` is a monotonic counter the host watches to skip
unchanged frames.
