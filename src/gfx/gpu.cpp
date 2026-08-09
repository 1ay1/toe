// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gpu.cpp — the sokol_gfx wrapper impl. The one non-SOKOL_IMPL TU that includes
// sokol_gfx.h, so the header stays confined here.

#include "toe/gfx/gpu.hpp"

#include <cstddef>

#include "sokol/sokol_gfx.h"

namespace toe::gfx::gpu {

namespace {
sg_pixel_format sg_fmt(Fmt f) { return f == Fmt::R8 ? SG_PIXELFORMAT_R8 : SG_PIXELFORMAT_RGBA8; }
int bpp(Fmt f) { return f == Fmt::R8 ? 1 : 4; }
} // namespace

std::uint32_t make_image(int w, int h, Fmt fmt, const void *pixels) {
    if (w <= 0 || h <= 0) return 0;
    sg_image_desc d = {};
    d.width = w;
    d.height = h;
    d.pixel_format = sg_fmt(fmt);
    d.usage.dynamic_update = true; // updated via update_image()
    // Dynamic images can't carry initial data in sokol; upload after create.
    sg_image img = sg_make_image(&d);
    if (img.id != SG_INVALID_ID && pixels) {
        sg_image_data data = {};
        data.mip_levels[0] = {pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(bpp(fmt))};
        sg_update_image(img, &data);
    }
    return img.id;
}

void update_image(std::uint32_t image_id, int w, int h, Fmt fmt, const void *pixels) {
    if (!image_id || !pixels) return;
    sg_image img = {image_id};
    sg_image_data data = {};
    data.mip_levels[0] = {pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(bpp(fmt))};
    sg_update_image(img, &data);
}

std::uint32_t make_texture_view(std::uint32_t image_id) {
    if (!image_id) return 0;
    sg_view_desc v = {};
    v.texture.image = sg_image{image_id};
    return sg_make_view(&v).id;
}

void destroy_image(std::uint32_t image_id) {
    if (image_id) sg_destroy_image(sg_image{image_id});
}
void destroy_view(std::uint32_t view_id) {
    if (view_id) sg_destroy_view(sg_view{view_id});
}

} // namespace toe::gfx::gpu
