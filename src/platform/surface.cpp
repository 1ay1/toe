// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Backend selection for the batteries-included gvte::platform surfaces.
// open_surface() picks a concrete backend for the environment; the host may
// force one explicitly via the Backend argument (selection is a parameter,
// never solely an environment-variable guess). This file is the only place
// that knows all three backends exist.

#include "gvte/platform/backend.hpp"

#include <cstdlib>

namespace gvte::platform {

// Defined in the per-backend translation units.
Result<AnySurface> open_wayland_surface(std::string_view title, PixelSize initial);
Result<AnySurface> open_x11_surface(std::string_view title, PixelSize initial);
Result<AnySurface> open_offscreen_surface(PixelSize size);

Result<AnySurface> open_surface(std::string_view title, PixelSize initial, Backend backend) {
    switch (backend) {
    case Backend::wayland:
        return open_wayland_surface(title, initial);
    case Backend::x11:
        return open_x11_surface(title, initial);
    case Backend::offscreen:
        return open_offscreen_surface(initial);
    case Backend::automatic:
        break;
    }

    // Automatic: honor GVTE_HEADLESS, then Wayland, then X11, then offscreen.
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const char *x = std::getenv("DISPLAY");
    const char *headless = std::getenv("GVTE_HEADLESS");

    if (headless && headless[0] != '\0') {
        return open_offscreen_surface(initial);
    }

    if (wl && wl[0] != '\0') {
        auto s = open_wayland_surface(title, initial);
        if (s) {
            return s;
        }
        if (!(x && x[0] != '\0')) {
            return s; // no X fallback: surface the Wayland error
        }
    }

    if (x && x[0] != '\0') {
        return open_x11_surface(title, initial);
    }

    // No display at all: an offscreen context so headless paths still work.
    return open_offscreen_surface(initial);
}

} // namespace gvte::platform
