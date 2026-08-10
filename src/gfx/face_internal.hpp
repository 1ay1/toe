// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Internal definitions of Face's pimpl types, shared by face.cpp (outline
// rasterization via stb) and face_color.cpp (CBDT/CBLC colour glyphs) so both
// TUs see the COMPLETE types — required for unique_ptr destruction/move-assign
// and for each TU to touch the members it owns. Not a public header.

#ifndef TOE_GFX_FACE_INTERNAL_HPP
#define TOE_GFX_FACE_INTERNAL_HPP

#include <cstdint>

#include "stb/stb_truetype.h"

#include "toe/gfx/face.hpp"

namespace toe::gfx {

// The stb outline handle.
struct Face::Handle {
    stbtt_fontinfo info{};
};

// Bounds-checked big-endian reader over a font blob (used by the colour backend).
struct ColorBE {
    const std::uint8_t *d = nullptr;
    std::size_t n = 0;
    [[nodiscard]] bool ok(std::size_t o, std::size_t len) const noexcept { return o + len <= n; }
    [[nodiscard]] std::uint8_t u8(std::size_t o) const noexcept { return ok(o, 1) ? d[o] : 0; }
    [[nodiscard]] std::uint16_t u16(std::size_t o) const noexcept {
        return ok(o, 2) ? static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]) : 0;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t o) const noexcept {
        return ok(o, 4) ? (static_cast<std::uint32_t>(d[o]) << 24 |
                           static_cast<std::uint32_t>(d[o + 1]) << 16 |
                           static_cast<std::uint32_t>(d[o + 2]) << 8 | d[o + 3])
                        : 0;
    }
};

// The colour-emoji decoder. Two formats are supported, because the platforms
// disagree:
//
//   * CBDT/CBLC - embedded PNG bitmap strikes. Used by Noto Color Emoji, i.e.
//     most Linux installs.
//   * COLR/CPAL - LAYERED VECTOR: each colour glyph is a list of (base glyph,
//     palette index) layers, each an ordinary outline the normal rasteriser can
//     draw. Used by Segoe UI Emoji, i.e. EVERY Windows install. Without it,
//     Windows emoji fall back to a flat monochrome outline.
//
// Full definition lives in face_color.cpp via its methods, but the DATA layout
// is here so the type is complete in both TUs.
struct Face::ColorBackend {
    ColorBE r;                 // over the owned blob
    std::size_t cmap = 0;      // chosen unicode cmap subtable
    std::uint16_t cmap_fmt = 0;

    // --- CBDT/CBLC (bitmap) ---
    std::size_t cbdt = 0;              // CBDT table offset
    std::size_t best_strike = 0;       // chosen CBLC bitmapSizeTable offset
    std::size_t strike_table_base_ = 0; // CBLC table start (for array offsets)
    std::uint16_t ppem = 0;            // pixels-per-em of the chosen strike

    // --- COLR/CPAL (layered vector) ---
    // When colr_ is set this face draws emoji as stacked outlines instead, and
    // the host rasterises each layer with the normal glyph path.
    std::size_t colr = 0;          // COLR table offset (0 = not present)
    std::size_t colr_base_recs = 0; // BaseGlyphRecord array
    std::uint16_t colr_num_base = 0;
    std::size_t colr_layer_recs = 0; // LayerRecord array
    std::uint16_t colr_num_layers = 0;
    std::size_t cpal_colors = 0;    // first palette's BGRA colour array
    std::uint16_t cpal_num_colors = 0;

    [[nodiscard]] bool is_bitmap() const noexcept { return cbdt != 0; }
    [[nodiscard]] bool is_layered() const noexcept { return colr != 0; }

    [[nodiscard]] std::uint32_t glyph_of(char32_t cp) const noexcept;

    struct Loc {
        std::size_t off = 0, len = 0;
        std::uint16_t fmt = 0;
    };
    [[nodiscard]] Loc locate(std::uint32_t gid) const noexcept;

    // One COLR layer: an outline glyph id plus the palette colour to fill it.
    struct Layer {
        std::uint16_t gid = 0;
        std::uint8_t r = 0, g = 0, b = 0, a = 255;
    };
    // Resolve `gid` into its colour layers. Empty when the glyph has no COLR
    // entry (the caller then draws it as a normal monochrome glyph).
    [[nodiscard]] std::vector<Layer> layers_of(std::uint32_t gid) const;
};

} // namespace toe::gfx

#endif // TOE_GFX_FACE_INTERNAL_HPP
