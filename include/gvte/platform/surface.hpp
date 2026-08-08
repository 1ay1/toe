// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Platform surface abstraction. The engine (renderer + terminal) depends only
// on this interface, never on Wayland or X11 types directly. A concrete
// backend (WaylandSurface, X11Surface) owns the native window, an EGL context,
// and the input source, and translates native events into the platform-neutral
// Event sum type below.
//
// Design: events are a closed sum type, so a host's dispatch is an exhaustive
// std::visit — a new event kind can't be silently ignored. There is no "empty"
// or "invalid" event; poll_events yields only well-formed alternatives.

#ifndef GVTE_PLATFORM_SURFACE_HPP
#define GVTE_PLATFORM_SURFACE_HPP

#include <functional>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

#include "gvte/core/types.hpp"
#include "gvte/input.hpp"

namespace gvte::platform {

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

using Event = std::variant<CloseRequested, Resized, KeyPressed, TextEntered, MouseDown, MouseUp,
                           MouseMove, MouseWheel, FocusChanged>;

// --- the abstract surface --------------------------------------------------
class Surface {
public:
    virtual ~Surface() = default;

    // Make the EGL context current and present the back buffer.
    virtual void swap() = 0;

    // Current drawable size in pixels.
    [[nodiscard]] virtual PixelSize pixel_size() const = 0;

    // Update the window/toplevel title (from OSC 0/2).
    virtual void set_title(std::string_view title) = 0;

    // --- clipboard (CLIPBOARD selection) -----------------------------------
    // Offer `utf8` as the clipboard contents (copy).
    virtual void set_clipboard(std::string_view utf8) = 0;
    // Request the clipboard contents (paste). Returns empty if unavailable.
    // Synchronous best-effort: on Wayland/X11 this round-trips the server.
    [[nodiscard]] virtual std::string get_clipboard() = 0;

    // Drain pending native events, invoking `sink` for each translated Event.
    // Non-blocking: returns after dispatching whatever is queued.
    virtual void poll_events(const std::function<void(const Event &)> &sink) = 0;

    // The file descriptor the windowing connection multiplexes on (Wayland
    // display fd / X11 connection fd). A host can poll() this together with the
    // PTY fd to block idle instead of busy-spinning. -1 if none.
    [[nodiscard]] virtual int event_fd() const = 0;

    // A timer fd that fires when a held key should repeat (Wayland synthesizes
    // repeats; X11 auto-repeats natively). Host adds it to its poll set. -1 if
    // the backend needs no timer.
    [[nodiscard]] virtual int repeat_fd() const { return -1; }

    // Flush any buffered outgoing protocol requests before blocking on the fd
    // (Wayland requires this so the compositor sees pending commits).
    virtual void flush() {}

    // True once the compositor/server has closed the connection.
    [[nodiscard]] virtual bool should_close() const = 0;
};

// Open a surface using the best available backend for the environment
// (Wayland when WAYLAND_DISPLAY is set, else X11). Requires the GL context to
// be current on return.
Result<std::unique_ptr<Surface>> open_surface(std::string_view title, PixelSize initial);

} // namespace gvte::platform

#endif // GVTE_PLATFORM_SURFACE_HPP
