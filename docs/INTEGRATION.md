# Integrating toe

How to embed toe in your application. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the design and [API.md](API.md) for the full reference.

There are two ways to use toe, depending on whether you want its Linux
window backends or bring your own window.

- **Path A — batteries included** (Linux, use `toe::platform`): link both
  targets, call `open_surface()`, and you have a working terminal in ~60 lines.
- **Path B — bring your own window** (any OS, `toe::core` only): model the
  `Surface` concept over your own windowing/GL layer (GLFW, Qt, SDL, Win32,
  Cocoa) and never link a windowing library through toe.

## Getting toe into your build

### With an installed package

```cmake
find_package(toe CONFIG REQUIRED COMPONENTS Core Platform)   # Path A
# find_package(toe CONFIG REQUIRED COMPONENTS Core)          # Path B
target_link_libraries(app PRIVATE toe::core toe::platform)
```

Install toe first:

```sh
cmake -S toe -B build -DTOE_INSTALL=ON
cmake --build build -j
cmake --install build --prefix /your/prefix
```

### As a subdirectory / FetchContent

```cmake
set(TOE_BUILD_PLATFORM ON  CACHE BOOL "" FORCE)   # OFF for Path B
set(TOE_BUILD_DEMO     OFF CACHE BOOL "" FORCE)
set(TOE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(toe)
target_link_libraries(app PRIVATE toe::core toe::platform)
```

(When toe is `add_subdirectory`'d, `DEMO`/`TESTS`/`INSTALL` default off
automatically; only `TOE_BUILD_PLATFORM` needs a choice.)

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

To stay idle-efficient, `poll()` on the fds toe gives you (`pty_fd()`, the
surface's `event_fd()`/`repeat_fd()`) instead of spinning.

## Path A — a complete minimal host

This is essentially `src/demo.cpp`. It is a full, working terminal.

```cpp
#include <epoxy/gl.h>
#include "toe/gfx/render_target.hpp"
#include "toe/platform/backend.hpp"
#include "toe/terminal.hpp"

int main() {
    // 1. A batteries-included Linux surface (Wayland → X11 → offscreen).
    //    The GL context is current on return.
    auto surface = toe::platform::open_surface("myterm", toe::PixelSize{960, 600});
    if (!surface) { std::fprintf(stderr, "%s\n", surface.error().message.c_str()); return 1; }
    toe::platform::AnySurface &surf = *surface;

    toe::PixelSize px = surf.pixel_size();

    // 2. The terminal. cfg.source defaults to spawning $SHELL; override for a
    //    specific shell, a custom TERM, a pre-exec hook, or an adopted fd.
    toe::Config cfg;
    auto term = toe::Terminal::create(cfg, px);
    if (!term) { std::fprintf(stderr, "%s\n", term.error().message.c_str()); return 1; }

    bool running = true;
    while (running && !surf.should_close()) {
        // (a) the lifecycle's only transition: Running -> (Running | Exited).
        toe::Terminal::Poll p = term->poll();
        if (p.exited) return p.exited->code;   // dead: nothing to render/type into
        toe::Session &session = *p.running;

        // (b) drain window events into Session actions.
        surf.poll_events([&](const toe::platform::Event &ev) {
            std::visit([&](auto &&e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, toe::platform::CloseRequested>) running = false;
                else if constexpr (std::is_same_v<T, toe::platform::Resized>) {
                    px = e.size; session.resize(px);
                } else if constexpr (std::is_same_v<T, toe::platform::KeyPressed>) {
                    session.send_key(e.key);
                } else if constexpr (std::is_same_v<T, toe::platform::TextEntered>) {
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
        auto rc = toe::gfx::RenderContext::adopt_current();
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

Link `toe::core` only. Provide a type that **models the `Surface` concept** —
no base class to inherit. Anything satisfying the five required methods works;
add the optional ones for title/clipboard/repeat/flush.

```cpp
#include "toe/platform/surface.hpp"   // the concept + Event, from core
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

// Your window over GLFW / Qt / SDL / Win32 / Cocoa. Must make a GL context
// current before you render.
struct MyWindow {
    // --- required by concept Surface ---
    void swap();                                             // present the back buffer
    toe::PixelSize pixel_size() const;                     // drawable size in px
    void poll_events(const toe::platform::EventSink &sink); // drain -> translate -> sink(Event)
    int  event_fd() const;                                  // pollable fd, or -1
    bool should_close() const;

    // --- optional refinements (implement the ones you support) ---
    void set_title(std::string_view);
    void set_clipboard(std::string_view);
    std::string get_clipboard();
    int  repeat_fd() const;
    void flush();
};
static_assert(toe::platform::Surface<MyWindow>);   // enforce the contract at compile time
```

Your `poll_events` translates native events into toe's neutral `Event` sum
type (`KeyPressed{KeyEvent}`, `TextEntered{utf8}`, `Resized{PixelSize}`,
`MouseDown/Up/Move/Wheel`, `FocusChanged`, `CloseRequested`) and calls `sink`
for each. The rest of the loop is identical to Path A — you own the window,
toe owns everything from PTY to pixels.

If you want runtime backend selection, wrap any model in `AnySurface`:

```cpp
toe::platform::AnySurface surf{ MyWindow{...} };   // erased; still a Surface
```

## Configuring the child (`AdoptFd`)

toe never forks — the HOST opens the PTY master and hands it in. Do the
`forkpty` (or ConPTY / SSH / replay) yourself, then:

```cpp
toe::Config cfg;

// Adopt the PTY you opened. toe drives your fd and reaps `child` on exit
// (set owns_fd=false to keep ownership of the fd).
cfg.source = toe::AdoptFd{ .master_fd = my_master, .child = my_pid, .owns_fd = true };
```

argv/`$SHELL`, `TERM`, and any post-fork child hook are yours to set in the
child before `execvp` — see hand's `posix_pty.cpp` for a complete `forkpty`
helper that returns an `AdoptFd`.

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
