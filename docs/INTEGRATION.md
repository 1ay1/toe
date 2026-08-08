# Integrating gvte

How to embed gvte in your application. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the design and [API.md](API.md) for the full reference.

There are two ways to use gvte, depending on whether you want its Linux
window backends or bring your own window.

- **Path A — batteries included** (Linux, use `gvte::platform`): link both
  targets, call `open_surface()`, and you have a working terminal in ~60 lines.
- **Path B — bring your own window** (any OS, `gvte::core` only): model the
  `Surface` concept over your own windowing/GL layer (GLFW, Qt, SDL, Win32,
  Cocoa) and never link a windowing library through gvte.

## Getting gvte into your build

### With an installed package

```cmake
find_package(gvte CONFIG REQUIRED COMPONENTS Core Platform)   # Path A
# find_package(gvte CONFIG REQUIRED COMPONENTS Core)          # Path B
target_link_libraries(app PRIVATE gvte::core gvte::platform)
```

Install gvte first:

```sh
cmake -S gvte -B build -DGVTE_INSTALL=ON
cmake --build build -j
cmake --install build --prefix /your/prefix
```

### As a subdirectory / FetchContent

```cmake
set(GVTE_BUILD_PLATFORM ON  CACHE BOOL "" FORCE)   # OFF for Path B
set(GVTE_BUILD_DEMO     OFF CACHE BOOL "" FORCE)
set(GVTE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(gvte)
target_link_libraries(app PRIVATE gvte::core gvte::platform)
```

(When gvte is `add_subdirectory`'d, `DEMO`/`TESTS`/`INSTALL` default off
automatically; only `GVTE_BUILD_PLATFORM` needs a choice.)

## The host contract

Whichever path, a host does the same four things every frame:

1. **`poll()` the terminal** → get a `Session&` (running) or an exit code.
2. **Drain window events** → translate each into a `Session` call
   (`send_key`, `send_text`, `resize`, mouse/selection, …).
3. **Pump child output & effects** → `pump_output()` reads the PTY; the runtime
   applies the resulting `Cmd`s; you pull host-side effects like
   `take_clipboard_request()` and `window_title()`.
4. **Render if changed** → watch `generation()`; when it moves, `render(rc, px)`
   and present.

To stay idle-efficient, `poll()` on the fds gvte gives you (`pty_fd()`, the
surface's `event_fd()`/`repeat_fd()`) instead of spinning.

## Path A — a complete minimal host

This is essentially `src/demo.cpp`. It is a full, working terminal.

```cpp
#include <epoxy/gl.h>
#include "gvte/gfx/render_target.hpp"
#include "gvte/platform/backend.hpp"
#include "gvte/terminal.hpp"

int main() {
    // 1. A batteries-included Linux surface (Wayland → X11 → offscreen).
    //    The GL context is current on return.
    auto surface = gvte::platform::open_surface("myterm", gvte::PixelSize{960, 600});
    if (!surface) { std::fprintf(stderr, "%s\n", surface.error().message.c_str()); return 1; }
    gvte::platform::AnySurface &surf = *surface;

    gvte::PixelSize px = surf.pixel_size();

    // 2. The terminal. cfg.source defaults to spawning $SHELL; override for a
    //    specific shell, a custom TERM, a pre-exec hook, or an adopted fd.
    gvte::Config cfg;
    auto term = gvte::Terminal::create(cfg, px);
    if (!term) { std::fprintf(stderr, "%s\n", term.error().message.c_str()); return 1; }

    bool running = true;
    while (running && !surf.should_close()) {
        // (a) the lifecycle's only transition: Running -> (Running | Exited).
        gvte::Terminal::Poll p = term->poll();
        if (p.exited) return p.exited->code;   // dead: nothing to render/type into
        gvte::Session &session = *p.running;

        // (b) drain window events into Session actions.
        surf.poll_events([&](const gvte::platform::Event &ev) {
            std::visit([&](auto &&e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, gvte::platform::CloseRequested>) running = false;
                else if constexpr (std::is_same_v<T, gvte::platform::Resized>) {
                    px = e.size; session.resize(px);
                } else if constexpr (std::is_same_v<T, gvte::platform::KeyPressed>) {
                    session.send_key(e.key);
                } else if constexpr (std::is_same_v<T, gvte::platform::TextEntered>) {
                    session.send_text(e.utf8);
                }
                // ...mouse events -> session.report_mouse / select_* as desired
            }, ev);
        });

        // (c) child output + effects (title, clipboard) are applied by the
        //     runtime inside poll()/pump_output(); pull host-side results:
        if (auto clip = session.take_clipboard_request()) surf.set_clipboard(*clip);
        surf.set_title(session.window_title());

        // (d) render into the window's back buffer (FBO 0).
        glViewport(0, 0, px.w, px.h);
        auto rc = gvte::gfx::RenderContext::adopt_current();
        session.render(rc, px);
        surf.swap();
    }
    return 0;
}
```

`hand`'s `main.cpp` is the production version of this loop: it adds `poll()`ing
the fds to sleep when idle, a 3 ms local-echo coalesce, damage-gated rendering
via `generation()`, and cursor-blink timing.

## Path B — bring your own window

Link `gvte::core` only. Provide a type that **models the `Surface` concept** —
no base class to inherit. Anything satisfying the five required methods works;
add the optional ones for title/clipboard/repeat/flush.

```cpp
#include "gvte/platform/surface.hpp"   // the concept + Event, from core
#include "gvte/gfx/render_target.hpp"
#include "gvte/terminal.hpp"

// Your window over GLFW / Qt / SDL / Win32 / Cocoa. Must make a GL context
// current before you render.
struct MyWindow {
    // --- required by concept Surface ---
    void swap();                                             // present the back buffer
    gvte::PixelSize pixel_size() const;                     // drawable size in px
    void poll_events(const gvte::platform::EventSink &sink); // drain -> translate -> sink(Event)
    int  event_fd() const;                                  // pollable fd, or -1
    bool should_close() const;

    // --- optional refinements (implement the ones you support) ---
    void set_title(std::string_view);
    void set_clipboard(std::string_view);
    std::string get_clipboard();
    int  repeat_fd() const;
    void flush();
};
static_assert(gvte::platform::Surface<MyWindow>);   // enforce the contract at compile time
```

Your `poll_events` translates native events into gvte's neutral `Event` sum
type (`KeyPressed{KeyEvent}`, `TextEntered{utf8}`, `Resized{PixelSize}`,
`MouseDown/Up/Move/Wheel`, `FocusChanged`, `CloseRequested`) and calls `sink`
for each. The rest of the loop is identical to Path A — you own the window,
gvte owns everything from PTY to pixels.

If you want runtime backend selection, wrap any model in `AnySurface`:

```cpp
gvte::platform::AnySurface surf{ MyWindow{...} };   // erased; still a Surface
```

## Configuring the child (`PtySource`)

```cpp
gvte::Config cfg;

// Default: spawn $SHELL (then /bin/sh) with TERM=xterm-256color.
// cfg.source is already SpawnCommand{}.

// A specific shell + terminfo + a child hook (runs after fork, before exec):
cfg.source = gvte::SpawnCommand{
    .argv     = {"/usr/bin/fish"},
    .term     = "xterm-kitty",
    .pre_exec = [] { ::setsid(); ::setenv("MYVAR", "1", 1); },   // async-signal-safe
};

// Or adopt a PTY you already own (SSH channel, container, replay) — gvte
// never forks; it drives your fd:
cfg.source = gvte::AdoptFd{ .master_fd = my_master, .child = my_pid, .owns_fd = true };
```

## Efficiency checklist

- **Sleep, don't spin.** `poll()` on `session.pty_fd()`, `surf.event_fd()`, and
  `surf.repeat_fd()` (use the `platform::repeat_fd(s)` accessor for a generic
  surface). Wake only on input, a window event, or a timer.
- **Render only on change.** Compare `session.generation()` to the last value
  you rendered; skip the frame if it's unchanged (except a periodic wake to
  blink the cursor).
- **Coalesce local echo.** After sending input, `poll()` the PTY briefly (a few
  ms) so the child's echo lands in the same frame — no one-vsync lag on
  keystrokes.
- **Inline-image animation.** `session.next_animation_deadline()` tells you when
  the next animated-image frame is due, so you can arm your timer precisely.
