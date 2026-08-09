// SPDX-License-Identifier: LGPL-2.0-or-later
//
// concept App — the WHOLE host boundary, as one contract.
//
// The engine owns every interface the system speaks in; a frontend (hand, or a
// Qt/GLFW/SDL/Win32/Cocoa shell) is nothing but an IMPLEMENTATION of this one
// contract. An `App` is "the window this build targets" — it owns its own
// construction (a static `open()` factory) and the operations toe drives it
// with (present, input, lifecycle). There is no separate Surface, Platform, or
// runner: those all collapse into App, because on a real build there is exactly
// one window type and its job is to exist and be driven.
//
// `main` names the concrete App (a build-time typedef, e.g. hand::App) and hands
// it to toe:
//
//     int main() { return toe::run<hand::App>(cfg, {"hand", {800, 500}}); }
//
// This header names NO platform type (no wl_*, no xcb_*, no EGL). It defines:
//
//   1. the platform-neutral windowing `Event` closed sum type (in toe::win, to
//      avoid clashing with the engine's identically-named TEA messages),
//   2. `WindowConfig` — the parameters an App is opened with,
//   3. the C++23 `concept App` — the STRUCTURAL contract a host's window must
//      satisfy (by shape, not inheritance), including its `open()` factory,
//   4. optional refinement concepts (title, clipboard, timers, damage, flush)
//      and uniform accessors that fold to no-ops when a host omits them.
//
// UX principle: the REQUIRED surface is tiny (open + 5 driving ops), so a
// "hello world" host is trivial; every nice-to-have (clipboard, title, partial
// damage, key-repeat, IME) is an OPTIONAL refinement that toe fills with a
// sensible default. Great hosts opt into more; minimal hosts still work.
//
// Because App is a concept, `toe::run<App>` is templated on the concrete type,
// so every call inlines and the whole loop is monomorphic — no vtable.

#ifndef TOE_APP_HPP
#define TOE_APP_HPP

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

#include "toe/core/types.hpp"
#include "toe/input.hpp"

namespace toe {

// The windowing-event types live in their own nested namespace so they don't
// collide with the engine's internal TEA messages of the same name (toe::Resized,
// toe::MouseDown, … in tea.hpp), which are a DIFFERENT layer: TEA messages are
// what update() consumes, these are what a window system produces. EventRouter
// is the bridge that translates one into the other.
namespace win {

// --- windowing events (platform-neutral) ----------------------------------
struct CloseRequested {};

struct Resized {
    PixelSize size;
};

struct KeyPressed {
    KeyEvent key;
};

struct TextEntered {
    // UTF-8 committed text (from the IME / compose). Distinct from KeyPressed
    // so the host can route ordinary typing separately from special keys.
    std::string_view utf8;
};

struct Preedit {
    // UTF-8 composition (preedit) string the IME is still assembling, shown
    // inline at the cursor. An empty string clears the preedit (commit/cancel).
    // `cursor` is the caret's cell offset within the string (-1 = at the end).
    // Valid only during the sink call, like TextEntered.
    std::string_view utf8;
    int cursor = -1;
};

// --- pointer events (in pixels, top-left origin) ---------------------------
enum class MouseButton { left, middle, right };

struct MouseDown {
    MouseButton button;
    std::int32_t x, y;   // pixels
    int click_count;     // 1 = single, 2 = double, 3 = triple
    Modifiers mods;
};
struct MouseUp {
    MouseButton button;
    std::int32_t x, y;
    Modifiers mods;
};
struct MouseMove {
    std::int32_t x, y;
    bool button_down;    // true while a button is held (a drag)
};
struct MouseWheel {
    std::int32_t dx, dy; // discrete steps; dy>0 = up
};
struct FocusChanged {
    bool focused; // window gained (true) or lost (false) keyboard focus
};

// A live font-size change request from the host (e.g. macOS Cmd +/- /0). `delta`
// steps the size up (+1) or down (-1) by one pixel-ish notch; `absolute`, when
// >= 0, sets an exact pixel size (0 handled by the host as "reset to default").
// The engine rebuilds the atlas and re-flows the grid. A window-level gesture,
// like Resized — so it lives in the windowing event set, not the child stream.
struct FontZoom {
    int delta = 0;       // +1 / -1 notch; ignored when absolute >= 0
    int absolute = -1;   // exact pixel size when >= 0
};

// A closed sum type: a host's dispatch is an exhaustive std::visit, so a new
// event kind can't be silently ignored. There is no "empty"/"invalid" event.
using Event = std::variant<CloseRequested, Resized, KeyPressed, TextEntered, Preedit, MouseDown,
                           MouseUp, MouseMove, MouseWheel, FocusChanged, FontZoom>;

// The callback shape toe hands to an App to receive drained events.
using EventSink = std::function<void(const Event &)>;

} // namespace win

// Bring the windowing-event vocabulary into `toe` under unambiguous handles so
// the concept, the accessors and hosts can name them without the TEA clash.
using win::Event;
using win::EventSink;
using win::MouseButton;

// How to create the window: the parameters an App is opened with (as opposed to
// toe::Config, which is how to build the terminal INSIDE it). A value toe owns
// the type of; the host fills it at the top, usually in main.
struct WindowConfig {
    std::string_view title = "toe";
    PixelSize size{800, 500};
};

// --- readiness waiting (part of the App contract) --------------------------
// The ONE place the loop blocks is a readiness wait on a tiny fd set: the child
// PTY, the window connection, and an optional key-repeat timer. That wait is the
// single genuinely OS-specific step, so toe OWNS NONE OF IT — no epoll, no
// poll.h (poll is POSIX; Windows has neither). toe declares the interface; the
// host implements the wait with whatever its OS offers (hand: epoll) and hands
// it back through the App. This keeps the engine fully portable.

// A wait deadline as a value: block forever, or up to N nanoseconds. Portable;
// the host maps it onto whatever its wait primitive accepts.
struct WaitDeadline {
    // < 0 means "block forever". Otherwise a nanosecond bound (0 = non-blocking).
    std::int64_t ns = -1;
    [[nodiscard]] static constexpr WaitDeadline forever() noexcept { return {-1}; }
    [[nodiscard]] static constexpr WaitDeadline nanos(std::int64_t n) noexcept {
        return {n < 0 ? 0 : n};
    }
    [[nodiscard]] static constexpr WaitDeadline millis(int ms) noexcept {
        return ms < 0 ? forever() : WaitDeadline{static_cast<std::int64_t>(ms) * 1'000'000};
    }
    [[nodiscard]] constexpr bool blocks_forever() const noexcept { return ns < 0; }
};

// Which sources woke the wait. `pty` and `window` are what the loop acts on;
// a spurious wakeup (all false) is fine — the loop just re-probes.
struct Readiness {
    bool pty = false;    // the child PTY fd is readable
    bool window = false; // the window connection fd is readable (events pending)
};

// concept App — the structural contract.
//
// A type A models App iff it provides these operations with these shapes.
// `A::open(WindowConfig)` is the factory (App owns its own construction);
// the instance ops present/poll/size/lifecycle drive it. Optional operations
// (title, clipboard, timers, damage, flush) are separate refinement concepts,
// so a minimal host need not implement them.
// ===========================================================================
template <typename A>
concept App = requires(A a, const A ca, const EventSink &sink, const WindowConfig &win) {
    // Factory: open the window for this build target. May pick a backend
    // internally. Returns a Result carrying the App (by value or smart
    // pointer). Requires a current GL context on return. This is what
    // toe::run<App> calls — the host never news the App itself.
    { A::open(win) };

    // Present the back buffer (the host's swap-buffers / present).
    { a.swap() } -> std::same_as<void>;

    // Current drawable size in pixels.
    { ca.pixel_size() } -> std::same_as<PixelSize>;

    // Drain pending native events, invoking `sink` for each translated Event.
    // Non-blocking: returns after dispatching whatever is queued.
    { a.poll_events(sink) } -> std::same_as<void>;

    // The fd the windowing connection multiplexes on (-1 if none). The host's
    // wait_readable folds it into its readiness wait; toe never touches it.
    { ca.event_fd() } -> std::convertible_to<int>;

    // True once the server/compositor has closed the connection.
    { ca.should_close() } -> std::convertible_to<bool>;

    // Block until the child PTY, the window, or the App's key-repeat timer is
    // readable, or the deadline elapses; report which of {pty, window} woke us.
    // This is the ONE place the loop blocks and the ONE OS-specific step — toe
    // owns none of it, the host implements it (hand: epoll). toe passes only the
    // PTY fd; the App already owns its window/repeat fds.
    { a.wait_readable(int{}, WaitDeadline{}) } -> std::same_as<Readiness>;
};

// --- optional refinements --------------------------------------------------
// An App that can carry the window title (OSC 0/2).
template <typename A>
concept TitledApp = App<A> && requires(A a, std::string_view t) {
    { a.set_title(t) } -> std::same_as<void>;
};

// An App with a CLIPBOARD selection.
template <typename A>
concept ClipboardApp = App<A> && requires(A a, std::string_view t) {
    { a.set_clipboard(t) } -> std::same_as<void>;
    { a.get_clipboard() } -> std::convertible_to<std::string>;
};

// An App that can open a URL in the desktop's default handler (OSC 8 links,
// Ctrl+Click). Opening a URL is an OS action — like the clipboard — so it's a
// host capability, not engine work. A host that doesn't provide it simply has
// non-clickable links (open_url() below is a no-op then).
template <typename A>
concept UrlOpenerApp = App<A> && requires(A a, std::string_view u) {
    { a.open_url(u) } -> std::same_as<void>;
};

// An App that needs a key-repeat timer fd folded into toe's poll set.
template <typename A>
concept RepeatingApp = App<A> && requires(const A ca) {
    { ca.repeat_fd() } -> std::convertible_to<int>;
};

// An App that can present only a changed sub-rectangle (partial damage), so the
// compositor recomposites less. Falls back to a full swap() otherwise.
template <typename A>
concept DamageableApp = App<A> && requires(A a, DamageRect d) {
    { a.swap_damaged(d) } -> std::same_as<void>;
};

// An App that buffers outgoing protocol and must flush before blocking.
template <typename A>
concept FlushableApp = App<A> && requires(A a) {
    { a.flush() } -> std::same_as<void>;
};

// --- uniform accessors -----------------------------------------------------
// Free functions that work on ANY model, filling in sensible no-op defaults for
// the optional refinements. toe's runtime calls these, so it never has to know
// whether a given host implemented the optional bits.

template <App A>
inline void title(A &a, std::string_view t) {
    if constexpr (TitledApp<A>) a.set_title(t);
}
template <App A>
inline void clipboard_set(A &a, std::string_view t) {
    if constexpr (ClipboardApp<A>) a.set_clipboard(t);
}
template <App A>
[[nodiscard]] inline std::string clipboard_get(A &a) {
    if constexpr (ClipboardApp<A>) return a.get_clipboard();
    else return {};
}
template <App A>
inline void open_url(A &a, std::string_view u) {
    if constexpr (UrlOpenerApp<A>) a.open_url(u);
}
template <App A>
[[nodiscard]] inline int repeat_fd(const A &a) {
    if constexpr (RepeatingApp<A>) return a.repeat_fd();
    else return -1;
}
template <App A>
inline void flush(A &a) {
    if constexpr (FlushableApp<A>) a.flush();
}

// Present, damaging only `d` if the App supports partial damage; otherwise a
// full swap(). Empty damage still presents (the caller decides whether to).
template <App A>
inline void present(A &a, DamageRect d) {
    if constexpr (DamageableApp<A>) a.swap_damaged(d);
    else a.swap();
}

// Block until the PTY, the window, or the App's key-repeat timer is readable, or
// the deadline elapses; report which of {pty, window} woke us. Pure forward to
// the App's required wait_readable — toe owns no wait mechanism of its own.
template <App A>
[[nodiscard]] inline Readiness wait_readable(A &a, int pty_fd, WaitDeadline d) {
    return a.wait_readable(pty_fd, d);
}

} // namespace toe

#endif // TOE_APP_HPP
