// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Backend selection. open_surface() picks the right platform::Surface
// implementation for the environment: Wayland when a compositor is present,
// otherwise X11. Each backend lives in its own TU (wayland.cpp / x11.cpp) and
// exposes an open_*_surface() factory; this file is the only place that knows
// both exist.

#include "gvte/platform/surface.hpp"

#include <cstdlib>

namespace gvte::platform {

// Defined in the per-backend translation units.
Result<std::unique_ptr<Surface>> open_wayland_surface(std::string_view title, PixelSize initial);
Result<std::unique_ptr<Surface>> open_x11_surface(std::string_view title, PixelSize initial);

Result<std::unique_ptr<Surface>> open_surface(std::string_view title, PixelSize initial) {
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const char *x = std::getenv("DISPLAY");

    // Prefer Wayland when its socket is advertised.
    if (wl && wl[0] != '\0') {
        auto s = open_wayland_surface(title, initial);
        if (s) {
            return s;
        }
        // Wayland advertised but failed; fall through to X11 if available.
        if (!(x && x[0] != '\0')) {
            return s; // no X fallback: surface the Wayland error
        }
    }

    if (x && x[0] != '\0') {
        return open_x11_surface(title, initial);
    }

    return fail("platform: neither WAYLAND_DISPLAY nor DISPLAY is set");
}

} // namespace gvte::platform
