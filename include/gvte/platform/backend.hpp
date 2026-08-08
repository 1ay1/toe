// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gvte::platform — the OPTIONAL, batteries-included Surface backends for Linux
// (Wayland / X11 / offscreen EGL). This header is part of the `gvte::platform`
// library, NOT `gvte::core`. A host that brings its own window does not link
// this target and never includes this header; it models `gvte::platform::Surface`
// (the concept in surface.hpp) directly.
//
// The concrete backend types are hidden behind AnySurface so this header stays
// free of wl_*/xcb_*/EGL types too — you get a runtime-selected surface that
// models the concept, with zero windowing headers leaking to the caller.

#ifndef GVTE_PLATFORM_BACKEND_HPP
#define GVTE_PLATFORM_BACKEND_HPP

#include <memory>
#include <string_view>

#include "gvte/core/types.hpp"
#include "gvte/platform/surface.hpp"

namespace gvte::platform {

// Which concrete backend to use. `automatic` picks Wayland when a Wayland
// display is reachable, else X11, else offscreen. The host may force one —
// selection is an explicit argument, never an environment-variable guess.
enum class Backend { automatic, wayland, x11, offscreen };

// Open a batteries-included surface. On success the GL context is current on
// the calling thread. The returned AnySurface models `Surface`, so it drops
// straight into gvte's runtime or any concept-constrained API.
//
// This is a CONVENIENCE. It is the only function in gvte that talks to a
// window system, and it lives entirely in the optional gvte::platform target.
[[nodiscard]] Result<AnySurface> open_surface(std::string_view title, PixelSize initial,
                                              Backend backend = Backend::automatic);

} // namespace gvte::platform

#endif // GVTE_PLATFORM_BACKEND_HPP
