// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::run<S> — the terminal's frame loop, MONOMORPHIC over the concrete
// surface type S. This is the engine's ENTRY POINT: a host opens a window that
// models `toe::Surface`, creates nothing else, and calls `toe::run(surface,
// cfg)`. toe owns everything from here — PTY, VT parse, screen model, the GL
// renderer, input policy (EventRouter), timing/blink, and the shape of a frame.
//
// The host (hand, or any Qt/GLFW/SDL/Win32/Cocoa shell) is left with exactly
// one job: bring a window that satisfies the `Surface` concept, with a current
// GL context. It does NOT own the loop — inverting the old split where the
// frontend drove the engine.
//
// Everything here is compile-time dispatched. `S` is a concrete `Surface` model
// chosen once at startup by the host; there is no AnySurface, no vtable, no
// std::function in the hot loop. Every surface call inlines and optional surface
// refinements (title, partial damage, key-repeat fd) resolve through the uniform
// `if constexpr` accessors in surface.hpp — folding to nothing on backends that
// don't provide them.

#ifndef TOE_RUN_HPP
#define TOE_RUN_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include <epoxy/gl.h>

#include "toe/app.hpp"
#include "toe/core/blink.hpp"
#include "toe/core/event_router.hpp"
#include "toe/core/poll_set.hpp"
#include "toe/core/surface.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

namespace toe {

// How long we wait for the child's echo before rendering a keystroke, and the
// idle cursor-blink cadence. Named so the frame loop reads as intent.
inline constexpr int kEchoWaitMs = 3;    // local shells echo in µs; this is a ceiling
inline constexpr int kIdlePollMs = 250;  // keeps cursor blink crisp when nothing else wakes us
inline constexpr int kMinAnimMs = 16;    // don't spin faster than ~60fps on animations
inline constexpr int kFloodPresentMs = 33; // ~30Hz present cap while flooding
inline constexpr int kCoalesceWaitMs = 2;  // brief refill window to coalesce a bursty stream
inline constexpr int kCoalesceRounds = 8;  // cap the coalesce so render/input never starve

// The render gate: a terminal frame is drawn only when something the user can
// see has changed. That "something" is fully captured by the damage generation
// plus the current blink state — so equality of this value across frames means
// "nothing to redraw". Comparing whole RenderKeys replaces ad-hoc last_*
// variables with one total comparison.
struct RenderKey {
    std::uint64_t generation{0};
    BlinkState blink{};
    constexpr auto operator<=>(const RenderKey &) const = default;
};

// Compute the poll timeout for an idle wait: the blink cadence, tightened to an
// inline-image animation's next-frame deadline when one is playing.
[[nodiscard]] inline Timeout idle_timeout(const toe::Session &s, Millis now) noexcept {
    if (const std::uint64_t deadline = s.next_animation_deadline(); deadline != 0) {
        const std::int64_t wait =
            static_cast<std::int64_t>(deadline) - static_cast<std::int64_t>(now.value);
        const int clamped =
            static_cast<int>(std::clamp<std::int64_t>(wait, kMinAnimMs, kIdlePollMs));
        return Timeout::millis(clamped);
    }
    return Timeout::millis(kIdlePollMs);
}

// The frame loop over one live Terminal and one concrete surface. Runs until the
// child exits or the window closes; returns the child's exit code. Called by
// `run()` below once the Terminal has been created.
template <Surface S>
[[nodiscard]] int run_loop(S &surf, toe::Terminal &term, toe::PixelSize px) {
    bool running = true;
    std::string last_title;
    std::optional<RenderKey> drawn;    // key of the last rendered frame
    std::uint64_t last_present_ms = 0; // for the flood frame-rate cap

    while (running && !surf.should_close()) {
        // 1. The lifecycle's only transition: Running -> (Running | Exited).
        //    A dead terminal has no Session — return its exit code and stop.
        const toe::Terminal::Poll p = term.poll();
        if (p.exited) return p.exited->code;
        toe::Session &session = *p.running;

        // 2. Route window events -> Session actions via the exhaustive
        //    visitor. It reports whether any event handed bytes to the child.
        EventRouter<S> router{session, surf, px, running};
        surf.poll_events([&](const Event &ev) { std::visit(router, ev); });

        // 2b. Drain child output HERE, decoupled from the window event
        //     cadence. poll_events fires a ChildOutput at most once per
        //     dispatch, so under a bursty stream relying on it alone paces
        //     throughput to one gulp per event-loop turn — each turn paying a
        //     compositor roundtrip + a sleep/wake (≈0 CPU, huge wall time).
        //     Instead, once the PTY fd is readable, drain in a tight loop
        //     until it's dry (drain() yields at a byte budget so input/render
        //     still get a turn under a real flood), coalescing a couple ms so
        //     a bursty producer's writes merge into one drain. Throughput is
        //     PTY/model-bound, not event-loop-bound.
        bool child_gone = false;
        {
            PollSet ready;
            ready.add(session.pty_fd());
            ready.wait(Timeout::millis(0)); // non-blocking probe
            int coalesce_budget = kCoalesceRounds;
            while (ready.ready(session.pty_fd())) {
                if (!session.pump_output()) { child_gone = true; break; }
                if (session.output_pending()) break;   // hit the byte budget: yield
                if (--coalesce_budget <= 0) break;     // yield to render/input
                ready.wait(Timeout::millis(kCoalesceWaitMs));
            }
        }

        // 3. Zero-latency local echo: after sending input, flush and give the
        //    PTY a few ms to echo, draining what returns so the typed glyph
        //    lands in THIS frame instead of a vsync later.
        if (router.take_wrote_input()) {
            toe::flush(surf);
            PollSet echo;
            echo.add(session.pty_fd());
            echo.wait(Timeout::millis(kEchoWaitMs));
            if (echo.ready(session.pty_fd()) && !session.pump_output()) child_gone = true;
        }
        if (child_gone) continue; // re-poll to observe the exit transition

        // 4. Render — but only if something visible changed and the flood
        //    frame-rate cap allows it. The RenderKey folds damage + blink
        //    into one comparison.
        const Millis now = Millis::now();
        const BlinkState blink = BlinkState::at(now);
        const RenderKey key{session.generation(), blink};

        // Title follows the child (OSC 0/2). Cheap string compare gates it.
        if (std::string t = session.window_title(); t != last_title) {
            toe::title(surf, t);
            last_title = std::move(t);
        }

        const bool flood = session.output_pending();
        const bool rate_ok = !flood || (now.value - last_present_ms) >= kFloodPresentMs;
        if ((child_gone || drawn != key) && rate_ok) {
            glViewport(0, 0, px.w, px.h);
            auto rc = toe::gfx::RenderContext::adopt_current();
            const toe::DamageRect dmg = session.render(rc, px, blink.cursor_on, blink.text_on);
            toe::present(surf, dmg.empty() ? toe::DamageRect::full(px) : dmg);
            drawn = key;
            last_present_ms = now.value;
        }

        // 5. Sleep until real work arrives — child output, a window event, or
        //    the blink/animation timer — instead of busy-spinning. EXCEPT
        //    under a flood: if output is still queued we loop straight back to
        //    drain it, having just rendered an intermediate frame.
        toe::flush(surf);
        if (!session.output_pending()) {
            PollSet wake;
            wake.add(surf.event_fd())
                .add(session.pty_fd())
                .add(toe::repeat_fd(surf)); // -1 on backends without a repeat timer; skipped
            wake.wait(idle_timeout(session, now));
        }
    }
    return 0;
}

// The low-level entry: given an already-open surface that models `Surface`
// (with a current GL context) and a build recipe, create the Terminal at the
// surface's pixel size and run the monomorphic frame loop to completion. Returns
// the child's exit code, or a negative value if the Terminal couldn't be created
// (message already printed to stderr).
//
// Most frontends implement the `App` contract and call the `run(App&)` overload
// below instead; this surface+config form is the primitive it delegates to.
template <Surface S>
[[nodiscard]] int run(S &surface, const toe::Config &cfg) {
    const toe::PixelSize px = surface.pixel_size();
    auto term = toe::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "toe: %s\n", term.error().message.c_str());
        return -1;
    }
    return run_loop(surface, *term, px);
}

// The canonical engine entry point. A frontend that models the `App` contract
// (toe/app.hpp) hands its whole self to toe: toe pulls the window out of
// `app.surface()` and the build recipe out of `app.config()`, then drives the
// terminal to completion. This is "toe owns the contract; the host implements
// it" made literal — the host supplies exactly what the contract declares and
// nothing more, and toe owns the loop. Instantiated once per concrete App, so
// the whole thing is monomorphic — no vtable.
template <App A>
[[nodiscard]] int run(A &app) {
    return run(app.surface(), app.config());
}

} // namespace toe

#endif // TOE_RUN_HPP
