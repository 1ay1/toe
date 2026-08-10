// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::run<App> — the terminal, driven to completion over one window.
//
// This is the engine's ENTRY POINT and the line `main` writes:
//
//     int main() { return toe::run<hand::App>(cfg, {"hand", {800, 500}}); }
//
// `App` is the concrete window type for this build target (a build-time
// typedef). toe OPENS it (App::open), builds the Terminal inside it, and owns
// everything from there — PTY, VT parse, screen model, the GL renderer, input
// policy (EventRouter), timing/blink, and the shape of a frame. The host's only
// job is to be an `App`: bring a window, with a current GL context.
//
// Everything is compile-time dispatched on the concrete `App`; there is no
// AnySurface, no vtable, no std::function in the hot loop. Every App call
// inlines and optional refinements (title, partial damage, key-repeat fd)
// resolve through the uniform `if constexpr` accessors in app.hpp — folding to
// nothing on backends that don't provide them.

#ifndef TOE_RUN_HPP
#define TOE_RUN_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "toe/app.hpp"
#include "toe/latency.hpp"
#include "toe/core/blink.hpp"
#include "toe/core/event_router.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

namespace toe {

// How long we wait for the child's echo before rendering a keystroke, and the
// idle cursor-blink cadence. Named so the frame loop reads as intent.
inline constexpr int kEchoWaitMs = 3;    // local shells echo in µs; this is a ceiling
inline constexpr int kIdlePollMs = 250;  // keeps cursor blink crisp when nothing else wakes us
inline constexpr int kMinAnimMs = 16;    // don't spin faster than ~60fps on animations
inline constexpr int kFloodPresentMs = 33; // ~30Hz present cap while flooding
inline constexpr int kStreamWaitMs = 4;    // ride inter-burst gaps without dropping to idle poll
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

// Compute the deadline for an idle wait: the blink cadence, tightened to an
// inline-image animation's next-frame deadline when one is playing. Returned as
// a portable WaitDeadline (nanoseconds) — the host's wait honours it precisely.
[[nodiscard]] inline WaitDeadline idle_deadline(const toe::Session &s, Millis now) noexcept {
    if (const std::uint64_t deadline = s.next_animation_deadline(); deadline != 0) {
        const std::int64_t wait =
            static_cast<std::int64_t>(deadline) - static_cast<std::int64_t>(now.value);
        const int clamped =
            static_cast<int>(std::clamp<std::int64_t>(wait, kMinAnimMs, kIdlePollMs));
        return WaitDeadline::millis(clamped);
    }
    return WaitDeadline::millis(kIdlePollMs);
}

// The frame loop over one live Terminal and one concrete App. Runs until the
// child exits or the window closes; returns the child's exit code. Called by
// `run()` below once the App is open and the Terminal created.
template <App A>
[[nodiscard]] int run_loop(A &surf, toe::Terminal &term, toe::PixelSize px) {
    bool running = true;
    std::string last_title;
    std::optional<RenderKey> drawn;    // key of the last rendered frame
    std::uint64_t last_present_ms = 0; // for the flood frame-rate cap
    bool prev_overlay_on = false;      // overlay state last frame (edge detect)

    // Input-to-photon latency: stamp when we write input to the child, measure
    // at the present that reflects it. A HUD prints live min/avg/p99 when
    // HAND_LATENCY_HUD is set in the environment — the number that decides how
    // "instant" a TUI feels, which almost no terminal reports.
    LatencyMeter latency;
    const bool latency_hud = [] {
        const char *v = std::getenv("HAND_LATENCY_HUD");
        return v && *v;
    }();
    // Where the HUD writes. stderr is useless for a GUI-subsystem build on
    // Windows (no console attached, output silently discarded), so
    // HAND_LATENCY_HUD may name a FILE instead of just being a flag: set it to
    // any path containing a separator or ending in .log/.txt to capture there.
    std::FILE *hud_out = stderr;
    if (latency_hud) {
        const char *v = std::getenv("HAND_LATENCY_HUD");
        const std::string_view sv{v};
        if (sv.find('/') != std::string_view::npos || sv.find('\\') != std::string_view::npos ||
            sv.ends_with(".log") || sv.ends_with(".txt")) {
            if (std::FILE *f = std::fopen(v, "w")) {
                hud_out = f;
                std::setvbuf(f, nullptr, _IOLBF, 0); // line-buffered: readable live
            }
        }
    }

    // Optionally let the App bind to the live Terminal once (e.g. to install a
    // live-resize render hook that draws mid-drag, when AppKit's modal resize
    // loop has the main thread and this loop is blocked). Folds away for hosts
    // that don't provide it.
    if constexpr (requires { surf.bind_terminal(term, px); }) {
        surf.bind_terminal(term, px);
    }

    while (running && !surf.should_close()) {
        // 1. The lifecycle's only transition: Running -> (Running | Exited).
        //    A dead terminal has no Session — return its exit code and stop.
        const toe::Terminal::Poll p = term.poll();
        if (p.exited) return p.exited->code;
        toe::Session &session = *p.running;

        // 2. Route window events -> Session actions via the exhaustive
        //    visitor. It reports whether any event handed bytes to the child.
        //    When an in-terminal overlay (settings panel, search) is active, it
        //    captures input first; events it consumes never reach the terminal.
        EventRouter<A> router{session, surf, px, running};
        surf.poll_events([&](const Event &ev) {
            if constexpr (OverlayApp<A>) {
                if (surf.overlay_active() && surf.overlay_event(ev)) return;
            }
            std::visit(router, ev);
        });

        const bool overlay_on = [&] {
            if constexpr (OverlayApp<A>) return surf.overlay_active();
            else return false;
        }();

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
        bool pumped = false; // did we drain any child output this turn?
        {
            // Non-blocking probe, then drain while the PTY stays readable.
            Readiness rr = toe::wait_readable(surf, session.pty_fd(), WaitDeadline::nanos(0));
            int coalesce_budget = kCoalesceRounds;
            while (rr.pty) {
                pumped = true;
                if (!session.pump_output()) { child_gone = true; break; }
                if (session.output_pending()) break;   // hit the byte budget: yield
                if (--coalesce_budget <= 0) break;     // yield to render/input
                rr = toe::wait_readable(surf, session.pty_fd(), WaitDeadline::millis(kCoalesceWaitMs));
            }
        }

        // 3. Zero-latency local echo: after sending input, flush and give the
        //    PTY a few ms to echo, draining what returns so the typed glyph
        //    lands in THIS frame instead of a vsync later.
        const bool just_typed = router.take_wrote_input();
        if (just_typed) {
            latency.mark_input(LatencyMeter::now_us());
            toe::flush(surf);
            const Readiness echo =
                toe::wait_readable(surf, session.pty_fd(), WaitDeadline::millis(kEchoWaitMs));
            if (echo.pty && !session.pump_output()) child_gone = true;
        }
        if (child_gone) continue; // re-poll to observe the exit transition

        // 4. Render — but only if something visible changed and the flood
        //    frame-rate cap allows it. The RenderKey folds damage + blink
        //    into one comparison.
        const Millis now = Millis::now();
        const BlinkState blink = BlinkState::at(now, static_cast<std::uint64_t>(session.cursor_blink_ms()));
        const RenderKey key{session.generation(), blink};

        // Title follows the child (OSC 0/2). Cheap string compare gates it.
        if (std::string t = session.window_title(); t != last_title) {
            toe::title(surf, t);
            last_title = std::move(t);
        }

        // A stream is "active" if we drained ANY child output this turn, not
        // just when a drain overflowed the byte budget. A workload like altstorm
        // (150k tiny alt-screen toggles) fully drains each turn — output_pending
        // stays false — yet bumps the generation on every toggle. Gating the
        // present cap on output_pending alone would then render+present per
        // toggle (300k compositor roundtrips). `pumped` catches it: while the
        // child produces output at all, cap presents to ~kFloodPresentMs and
        // don't drop to the idle poll — loop straight back to keep draining.
        const bool streaming = pumped || session.output_pending();
        // The flood cap must NOT apply to a keystroke's own echo. Typing makes
        // the PTY produce output, so `streaming` goes true on the very turn we
        // are trying to answer the user — and the present that carries the
        // echoed glyph was then held back until the cap expired, adding up to a
        // whole kFloodPresentMs to input->photon latency (measured: p99 31 ms
        // against a 33 ms cap). `just_typed` exempts that frame. It cannot
        // cause a present storm: keystrokes arrive at human rate, and a genuine
        // flood (no input) is still capped.
        const bool rate_ok =
            !streaming || just_typed || (now.value - last_present_ms) >= kFloodPresentMs;
        // The overlay animates + responds to input every frame, so force a
        // repaint while it's active (the terminal grid may be unchanged). ALSO
        // force ONE repaint on the frame it CLOSES (overlay_on just went false):
        // the grid is unchanged so the RenderKey matches, but the pane pixels
        // are still on screen and must be erased NOW — otherwise closing looks
        // laggy (the pane lingers until the next unrelated damage).
        const bool overlay_closed = prev_overlay_on && !overlay_on;
        prev_overlay_on = overlay_on;
        if ((child_gone || drawn != key || overlay_on || overlay_closed) && rate_ok) {
            // The host begins the swapchain frame (sokol pass + clear to the
            // terminal's default background), toe renders the grid + overlay
            // into it, then the host ends the pass and presents.
            const toe::Rgb bg = session.default_bg();
            surf.begin_frame(px, bg.r, bg.g, bg.b, session.opacity());
            auto rc = toe::gfx::RenderContext::adopt_current();
            const toe::DamageRect dmg = session.render(rc, px, blink.cursor_on, blink.text_on);
            if constexpr (OverlayApp<A>) {
                if (overlay_on) surf.overlay_render(term, px);
            }
            surf.end_frame();
            toe::present(surf, (overlay_on || overlay_closed || dmg.empty())
                                   ? toe::DamageRect::full(px)
                                   : dmg);
            drawn = key;
            last_present_ms = now.value;

            // Keep the frames coming while the caret is still gliding: the grid
            // hasn't changed (key is stable) but the cursor moved, so invalidate
            // the drawn-key to force a repaint next iteration until it settles.
            if (session.cursor_animating()) drawn.reset();

            // This present may reflect the child's response to the last input.
            // Record the input->photon sample; the HUD reports a live rolling
            // min/avg/p99 to stderr a few times a second when enabled.
            if (latency_hud) {
                // A present within a sane budget reflects the keystroke's echo.
                // If a huge output flood delayed this present far past a couple
                // of frames, that gap is throughput, not input latency — drop
                // the pending mark so the HUD stays honest.
                latency.drop_if_stale(LatencyMeter::now_us(), 50 * 1000);
                latency.mark_present(LatencyMeter::now_us());
                static std::uint64_t last_report_ms = 0;
                if (latency.has_samples() && now.value - last_report_ms >= 500) {
                    const auto s = latency.stats();
                    std::fprintf(hud_out,
                                 "[hand latency] n=%zu  min=%.1f  avg=%.1f  p99=%.1f  max=%.1f ms\n",
                                 s.n, s.min_ms, s.avg_ms, s.p99_ms, s.max_ms);
                    std::fflush(hud_out);
                    last_report_ms = now.value;
                }
            }
        }

        // 5. Sleep until real work arrives — child output, a window event, or
        //    the blink/animation timer — instead of busy-spinning. EXCEPT while
        //    streaming: more output is on its way, so loop straight back to
        //    drain it (a present may have been rate-capped above) without paying
        //    an idle poll-wait + compositor roundtrip per chunk.
        toe::flush(surf);
        if (session.cursor_animating()) {
            // Cursor glide in flight: pace at ~60fps so it stays smooth without
            // busy-spinning. A pending PTY read still wakes us earlier.
            toe::wait_readable(surf, session.pty_fd(), WaitDeadline::millis(kMinAnimMs));
        } else if (!streaming) {
            // The App's readiness wait covers the PTY, the window and (if any) the
            // key-repeat timer; toe passes only the PTY fd. Idle deadline paces
            // the cursor blink / inline-image animation.
            toe::wait_readable(surf, session.pty_fd(), idle_deadline(session, now));
        } else if (!session.output_pending()) {
            // Streaming but the PTY is momentarily dry (a micro-gap between the
            // producer's writes). Don't busy-spin re-probing: block briefly on
            // the PTY so the next burst resumes the tight loop immediately,
            // while still yielding the core. A real end-of-stream just falls
            // through to the idle wait next turn (pumped will be false).
            toe::wait_readable(surf, session.pty_fd(), WaitDeadline::millis(kStreamWaitMs));
        }
    }
    // Final summary, so a short session (or one that never crossed a reporting
    // interval) still yields its numbers instead of an empty log.
    if (latency_hud && latency.has_samples()) {
        const auto s = latency.stats();
        std::fprintf(hud_out,
                     "[hand latency FINAL] n=%zu total=%llu  min=%.1f  avg=%.1f  p99=%.1f  "
                     "max=%.1f ms\n",
                     s.n, static_cast<unsigned long long>(latency.total_samples()), s.min_ms,
                     s.avg_ms, s.p99_ms, s.max_ms);
        std::fflush(hud_out);
    }
    return 0;
}

// dereference a Result<App> or Result<unique_ptr<App>> to an App& uniformly, so
// a host may return its App by value OR by owning pointer from open().
template <class T> [[nodiscard]] inline auto &deref_app(T &opened) {
    if constexpr (requires { *opened; }) return *opened; // smart-pointer payload
    else return opened;                                  // by-value payload
}

// The single, top-level entry point — the line `main` writes. `App` is the
// concrete window type for this build (hand::App). toe opens it via the App's
// own factory (App::open), builds the Config's Terminal at the window's pixel
// size, and drives the loop to completion. Everything is monomorphic on the one
// concrete App — no vtable. Returns the child's exit code, or a negative value
// if the window or terminal couldn't be created (message already on stderr).
//
//     int main() { return toe::run<hand::App>(cfg, {"hand", {800, 500}}); }
template <App A>
[[nodiscard]] int run(const toe::Config &cfg, const WindowConfig &win = {}) {
    auto opened = A::open(win);
    if (!opened) {
        std::fprintf(stderr, "toe: %s\n", opened.error().message.c_str());
        return -1;
    }
    A &app = deref_app(*opened);

    const toe::PixelSize px = app.pixel_size();
    auto term = toe::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "toe: %s\n", term.error().message.c_str());
        return -1;
    }
    return run_loop(app, *term, px);
}

} // namespace toe

#endif // TOE_RUN_HPP
