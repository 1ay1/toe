// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The GPU renderer. Each frame it walks the Screen grid and builds two batches
// of instances: solid background rectangles, and textured glyph quads. Both
// are drawn with a single instanced draw call over a unit quad, so the whole
// screen is a couple of GL calls regardless of cell count.

#ifndef GVTE_GFX_RENDERER_HPP
#define GVTE_GFX_RENDERER_HPP

#include <cstdint>
#include <vector>

#include "gvte/core/types.hpp"
#include "gvte/gfx/font.hpp"
#include "gvte/gfx/palette.hpp"
#include "gvte/gfx/shader.hpp"
#include "gvte/term/screen.hpp"

namespace gvte::gfx {

class Renderer {
public:
    static Result<Renderer> create(FontAtlas &&atlas);

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    Renderer(Renderer &&) noexcept;
    Renderer &operator=(Renderer &&) noexcept;
    ~Renderer();

    // Number of cells that fit in a pixel viewport, given the font's cell size.
    [[nodiscard]] Extent cells_for(PixelSize px) const noexcept;

    [[nodiscard]] const FontAtlas &font() const noexcept { return atlas_; }

    // Draw the whole screen into the current framebuffer of size `px`.
    void draw(const term::Screen &screen, PixelSize px);

private:
    // One instance: a colored (and optionally textured) quad in pixel space.
    struct Instance {
        float x, y, w, h;     // pixel rect
        float u0, v0, u1, v1; // atlas UVs (bg quads use 0s)
        float r, g, b;        // color
        float is_glyph;       // 1.0 for textured glyph, 0.0 for solid bg
    };

    explicit Renderer(FontAtlas &&atlas, Program &&prog)
        : atlas_{std::move(atlas)}, prog_{std::move(prog)} {}

    void ensure_buffers();
    void flush(std::size_t count, PixelSize px);

    FontAtlas atlas_;
    Palette palette_{};
    Program prog_;

    std::uint32_t vao_{0};
    std::uint32_t quad_vbo_{0};
    std::uint32_t inst_vbo_{0};
    std::size_t inst_capacity_{0};

    std::vector<Instance> instances_{};
};

} // namespace gvte::gfx

#endif // GVTE_GFX_RENDERER_HPP
