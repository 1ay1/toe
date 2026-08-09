// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gpu — a thin internal wrapper over sokol_gfx, so the atlas and renderer speak
// a tiny vocabulary (create/update an image, get a bindable view) without every
// TU pulling in the 27k-line sokol header. Implemented in gpu.cpp (the only
// non-impl TU that includes sokol_gfx.h). IDs are the raw sokol handle .id, so
// they cross the header boundary as plain uint32.

#ifndef TOE_GFX_GPU_HPP
#define TOE_GFX_GPU_HPP

#include <cstdint>

namespace toe::gfx::gpu {

// Pixel formats we use for atlases.
enum class Fmt { R8, RGBA8 };

// Create an image of `w`x`h` in `fmt`, initialised from `pixels` (may be null).
// Returns the sg_image .id (0 on failure). `filter_linear` picks LINEAR vs
// NEAREST sampling for the view's use.
[[nodiscard]] std::uint32_t make_image(int w, int h, Fmt fmt, const void *pixels);

// Replace an image's entire contents from `pixels` (w*h*bpp bytes). sokol
// requires whole-image updates; the atlas keeps a CPU shadow and calls this.
void update_image(std::uint32_t image_id, int w, int h, Fmt fmt, const void *pixels);

// A texture view over an image, for binding. Created once per image.
[[nodiscard]] std::uint32_t make_texture_view(std::uint32_t image_id);

// Destroy an image and/or view (0 = no-op).
void destroy_image(std::uint32_t image_id);
void destroy_view(std::uint32_t view_id);

} // namespace toe::gfx::gpu

#endif // TOE_GFX_GPU_HPP
