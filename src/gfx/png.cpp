// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Shared libpng-backed PNG decoder (see png.hpp).

#include "toe/gfx/png.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstring>

#include <png.h>

namespace toe::gfx {

bool decode_png(const std::uint8_t *data, std::size_t len, int &w, int &h,
                std::vector<std::uint8_t> &rgba) {
    if (len < 8 || png_sig_cmp(data, 0, 8) != 0) return false;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) { // libpng error handler
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    struct Src {
        const std::uint8_t *p;
        std::size_t left;
    } src{data, len};
    png_set_read_fn(png, &src, [](png_structp pp, png_bytep out, png_size_t n) {
        auto *s = static_cast<Src *>(png_get_io_ptr(pp));
        const std::size_t take = std::min<std::size_t>(n, s->left);
        std::memcpy(out, s->p, take);
        s->p += take;
        s->left -= take;
    });
    png_read_info(png, info);

    w = static_cast<int>(png_get_image_width(png, info));
    h = static_cast<int>(png_get_image_height(png, info));
    const png_byte color = png_get_color_type(png, info);
    const png_byte depth = png_get_bit_depth(png, info);

    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY ||
        color == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > (64LL << 20)) return false;
    rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    std::vector<png_bytep> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        rows[static_cast<std::size_t>(y)] =
            rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

} // namespace toe::gfx
