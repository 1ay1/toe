// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The Surface contract, as a *concept* — the seam between toe's engine and a
// window system. This header lives in toe::core and names NO platform type
// (no wl_*, no xcb_*, no EGL). It defines only:
//
//   1. the platform-neutral `Event` closed sum type,
//   2. a C++23 `concept Surface` — the STRUCTURAL contract a host's window
//      must satisfy (by shape, not by inheritance),
//   3. uniform free-function accessors that fill in no-op defaults for the
//      optional surface refinements, so toe's runtime never has to ask whether
//      a given host implemented the optional bits.
//
// Design: a host brings its own window (GLFW, Qt, SDL, Win32, Cocoa, or hand's
// Wayland/X11 backends) by making a type that *models* `Surface`. There is no
// base class to inherit, no vtable forced on the host's type, and no window
// header pulled into toe. The engine's `toe::run<S>` (run.hpp) is templated on
// the concrete surface, so every surface call inlines and the optional
// refinements resolve at compile time — the shipped Wayland/X11 backends in
// `hand` are merely one set of models of this same concept.

#ifndef TOE_CORE_SURFACE_HPP
#define TOE_CORE_SURFACE_HPP

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

// A closed sum type: a host's dispatch is an exhaustive std::visit, so a new
// event kind can't be silently ignored. There is no "empty"/"invalid" event.
using Event = std::variant<CloseRequested, Resized, KeyPressed, TextEntered, Preedit, MouseDown,
                           MouseUp, MouseMove, MouseWheel, FocusChanged>;

// The callback shape toe hands to a surface to receive drained events.
using EventSink = std::function<void(const Event &)>;

} // namespace win

// Bring the windowing-event vocabulary into `toe` under unambiguous handles so
// the concept, the accessors and hosts can name them without the TEA clash.
using win::Event;
using win::EventSink;
using win::MouseButton;

// ===========================================================================
// concept Surface — the structural contract.
//
// A type S models Surface iff it provides these operations with these shapes.
// Optional operations (title, clipboard, timers, flush) are expressed via
// separate refinement concepts so a minimal host need not implement them.
// ===========================================================================
template <typename S>
concept Surface = requires(S s, const S cs, const EventSink &sink) {
    // Present the back buffer (the host's swap-buffers / present).
    { s.swap() } -> std::same_as<void>;

    // Current drawable size in pixels.
    { cs.pixel_size() } -> std::same_as<PixelSize>;

    // Drain pending native events, invoking `sink` for each translated Event.
    // Non-blocking: returns after dispatching whatever is queued.
    { s.poll_events(sink) } -> std::same_as<void>;

    // The fd the windowing connection multiplexes on (-1 if none), so a host
    // — or toe's own reference loop — can poll() it to block idle.
    { cs.event_fd() } -> std::convertible_to<int>;

    // True once the server/compositor has closed the connection.
    { cs.should_close() } -> std::convertible_to<bool>;
};

// --- optional refinements --------------------------------------------------
// A surface that can carry the window title (OSC 0/2).
template <typename S>
concept TitledSurface = Surface<S> && requires(S s, std::string_view t) {
    { s.set_title(t) } -> std::same_as<void>;
};

// A surface with a CLIPBOARD selection.
template <typename S>
concept ClipboardSurface = Surface<S> && requires(S s, std::string_view t) {
    { s.set_clipboard(t) } -> std::same_as<void>;
    { s.get_clipboard() } -> std::convertible_to<std::string>;
};

// A surface that needs a key-repeat timer fd folded into the host's poll set.
template <typename S>
concept RepeatingSurface = Surface<S> && requires(const S cs) {
    { cs.repeat_fd() } -> std::convertible_to<int>;
};

// A surface that can present only a changed sub-rectangle (partial damage),
// so the compositor recomposites less. Falls back to a full swap() otherwise.
template <typename S>
concept DamageableSurface = Surface<S> && requires(S s, DamageRect d) {
    { s.swap_damaged(d) } -> std::same_as<void>;
};

// A surface that buffers outgoing protocol and must flush before blocking.
template <typename S>
concept FlushableSurface = Surface<S> && requires(S s) {
    { s.flush() } -> std::same_as<void>;
};

// --- uniform accessors -----------------------------------------------------
// Free functions that work on ANY model, filling in sensible no-op defaults
// for the optional refinements. toe's runtime calls these, so it never has to
// know whether a given host implemented the optional bits.

template <Surface S>
inline void title(S &s, std::string_view t) {
    if constexpr (TitledSurface<S>) s.set_title(t);
}
template <Surface S>
inline void clipboard_set(S &s, std::string_view t) {
    if constexpr (ClipboardSurface<S>) s.set_clipboard(t);
}
template <Surface S>
[[nodiscard]] inline std::string clipboard_get(S &s) {
    if constexpr (ClipboardSurface<S>) return s.get_clipboard();
    else return {};
}
template <Surface S>
[[nodiscard]] inline int repeat_fd(const S &s) {
    if constexpr (RepeatingSurface<S>) return s.repeat_fd();
    else return -1;
}
template <Surface S>
inline void flush(S &s) {
    if constexpr (FlushableSurface<S>) s.flush();
}

// Present, damaging only `d` if the surface supports partial damage; otherwise
// a full swap(). Empty damage still presents (the caller decides whether to).
template <Surface S>
inline void present(S &s, DamageRect d) {
    if constexpr (DamageableSurface<S>) s.swap_damaged(d);
    else s.swap();
}

} // namespace toe

#endif // TOE_CORE_SURFACE_HPP
