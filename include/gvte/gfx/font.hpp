// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Glyph atlas. A monospace font is rasterized on demand by FreeType, each
// glyph packed into a single-channel GL texture (a shelf allocator), and the
// per-glyph UV + metrics cached by codepoint. The renderer looks up a GlyphInfo
// per cell and emits one textured quad.
//
// Type-theoretic note: pixel metrics (advance, bearing, cell size) are plain
// ints in device pixels here, but they are never mixed with the grid's
// Row/Col cell coordinates — the boundary between "cells" and "pixels" is the
// renderer's job (slice 6), and this module speaks only pixels.

#ifndef GVTE_GFX_FONT_HPP
#define GVTE_GFX_FONT_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

#include "gvte/core/types.hpp"

namespace gvte::gfx {

// UV rect (normalized) + placement metrics for one rasterized glyph.
struct GlyphInfo {
    float u0{}, v0{}, u1{}, v1{}; // atlas texture coords
    int width{}, height{};        // bitmap size in pixels
    int bearing_x{}, bearing_y{}; // offset from pen origin to bitmap top-left
    int advance{};                // horizontal advance in pixels
};

class FontAtlas {
public:
    // Load a monospace font at `pixel_size`. If `family` is empty, Fontconfig
    // picks the system default monospace. Creates the GL atlas texture.
    static Result<FontAtlas> create(std::string family, int pixel_size);

    FontAtlas(const FontAtlas &) = delete;
    FontAtlas &operator=(const FontAtlas &) = delete;
    FontAtlas(FontAtlas &&) noexcept;
    FontAtlas &operator=(FontAtlas &&) noexcept;
    ~FontAtlas();

    // Metrics defining a monospace cell.
    [[nodiscard]] int cell_width() const noexcept { return cell_w_; }
    [[nodiscard]] int cell_height() const noexcept { return cell_h_; }
    [[nodiscard]] int ascent() const noexcept { return ascent_; }

    // Look up (rasterizing + packing on first use) the glyph for a codepoint.
    // Returns nullptr only if the codepoint has no glyph in the face.
    const GlyphInfo *glyph(char32_t cp);

    // The GL atlas texture id (GL_R8), for binding by the renderer.
    [[nodiscard]] std::uint32_t texture() const noexcept { return tex_; }
    [[nodiscard]] int atlas_size() const noexcept { return atlas_dim_; }

private:
    FontAtlas() = default;
    void destroy() noexcept;
    const GlyphInfo *rasterize(char32_t cp);

    // opaque FreeType handles (kept as void* to avoid leaking ft2 into the hdr)
    void *ft_{nullptr};   // FT_Library
    void *face_{nullptr}; // FT_Face

    std::uint32_t tex_{0};
    int atlas_dim_{0};
    int pen_x_{0}, pen_y_{0}, shelf_h_{0}; // shelf allocator cursor

    int cell_w_{0}, cell_h_{0}, ascent_{0};

    std::unordered_map<char32_t, GlyphInfo> cache_{};
};

} // namespace gvte::gfx

#endif // GVTE_GFX_FONT_HPP
