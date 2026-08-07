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

using Event = std::variant<CloseRequested, Resized, KeyPressed, TextEntered>;

// --- the abstract surface --------------------------------------------------
class Surface {
public:
    virtual ~Surface() = default;

    // Make the EGL context current and present the back buffer.
    virtual void swap() = 0;

    // Current drawable size in pixels.
    [[nodiscard]] virtual PixelSize pixel_size() const = 0;

    // Drain pending native events, invoking `sink` for each translated Event.
    // Non-blocking: returns after dispatching whatever is queued.
    virtual void poll_events(const std::function<void(const Event &)> &sink) = 0;

    // True once the compositor/server has closed the connection.
    [[nodiscard]] virtual bool should_close() const = 0;
};

// Open a surface using the best available backend for the environment
// (Wayland when WAYLAND_DISPLAY is set, else X11). Requires the GL context to
// be current on return.
Result<std::unique_ptr<Surface>> open_surface(std::string_view title, PixelSize initial);

} // namespace gvte::platform

#endif // GVTE_PLATFORM_SURFACE_HPP
