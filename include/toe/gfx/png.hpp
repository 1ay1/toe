// SPDX-License-Identifier: LGPL-2.0-or-later
//
// decode_png — shared in-memory PNG -> RGBA8 decode (libpng). Used by the kitty/
// sixel graphics protocol and by the CBDT/CBLC colour-emoji glyph decoder.

#ifndef TOE_GFX_PNG_HPP
#define TOE_GFX_PNG_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toe::gfx {

// Decode a PNG from memory into tightly-packed RGBA8 (w*h*4). Returns false on
// any error (bad signature, oversize, libpng failure). Never throws.
[[nodiscard]] bool decode_png(const std::uint8_t *data, std::size_t len, int &w, int &h,
                              std::vector<std::uint8_t> &rgba);

} // namespace toe::gfx

#endif // TOE_GFX_PNG_HPP
