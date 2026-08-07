// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The GPU renderer. Each frame it walks the Screen grid and builds two batches
// of instances: solid background rectangles, and textured glyph quads. Both
// are drawn with a single instanced draw call over a unit quad, so the whole
// screen is a couple of GL calls regardless of cell count.

#ifndef GVTE_GFX_RENDERER_HPP
#define GVTE_GFX_RENDERER_HPP

#include <cstdint>
#include <span>
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
    void draw(const term::Screen &screen, PixelSize px, bool cursor_on = true);

private:
    // One instance: a colored (and optionally textured) quad in pixel space.
    //   rect   16B  x,y,w,h in pixels (float — needs sub-pixel precision)
    //   uv     16B  u0,v0,u1,v1 as float (atlas coords in [0,1]) — float, not
    //              packed u16: normalized-u16 UVs sampled the wrong atlas texel
    //              on some drivers and rendered glyphs as solid blocks.
    //   color   4B  r,g,b,_ as normalized u8
    //   flags   4B  is_glyph (u8), radius-in-px (u8), 2B pad
    struct Instance {
        float x, y, w, h;                    // pixel rect
        float u0, v0, u1, v1;                // atlas UVs in [0,1]
        std::uint8_t r, g, b, a;             // color, normalized u8
        std::uint8_t is_glyph, radius, pad0, pad1;
    };
    static_assert(sizeof(Instance) == 40, "Instance layout drifted");

    explicit Renderer(FontAtlas &&atlas, Program &&prog)
        : atlas_{std::move(atlas)}, prog_{std::move(prog)} {}

    void ensure_buffers();
    void flush(std::span<const Instance> insts, PixelSize px);

    // --- per-row damage cache ------------------------------------------------
    // State-of-the-art incremental rebuild. draw() is only called when the
    // screen's generation advanced, but usually only a handful of rows actually
    // changed (a keystroke, a cursor move, one scrolled line). We keep each
    // viewport row's built instances plus a 64-bit key that fingerprints
    // everything affecting that row's pixels; on a frame we rebuild ONLY the
    // rows whose key changed and reuse the rest verbatim — skipping the glyph
    // lookups, palette resolves and push_backs for the unchanged majority.
    struct RowCache {
        std::uint64_t key{0};              // fingerprint of cells + render state
        bool valid{false};
        std::vector<Instance> bg;          // background / selection rects
        std::vector<Instance> glyphs;      // textured glyph quads
    };
    std::vector<RowCache> rows_{};
    // Geometry the cache was built against; a change invalidates everything.
    int cache_cols_{-1}, cache_rows_{-1}, cache_cw_{0}, cache_ch_{0}, cache_ascent_{0};
    // Build (or reuse) one row's instances into rows_[r]. Returns true if the
    // row was rebuilt (its key changed).
    bool build_row(const term::Screen &screen, int r, std::uint64_t key,
                   bool row_has_cursor, int cur_col, std::int64_t abs_row,
                   bool any_selection);

    FontAtlas atlas_;
    Palette palette_{};
    Program prog_;
    int u_screen_{-1}; // cached glGetUniformLocation results (per-frame lookups
    int u_atlas_{-1};  // into the driver were a measurable cost)

    std::uint32_t vao_{0};
    std::uint32_t quad_vbo_{0};
    std::uint32_t inst_vbo_{0};
    std::size_t inst_bytes_capacity_{0};

    // Persistent-mapped streaming ring (GL 4.4 / GL_ARB_buffer_storage). When
    // available we write instances straight into GPU-visible memory with no
    // glBufferSubData copy, cycling through kRing sub-regions and fencing each
    // so the CPU never overwrites a region the GPU is still reading. We issue
    // two draws per frame (backgrounds then glyphs), so 4 regions keeps ~2
    // frames of slack before a slot is reused.
    static constexpr int kRing = 4;
    bool persistent_{false};
    unsigned char *inst_map_{nullptr};      // base of the persistent mapping
    std::size_t inst_region_bytes_{0};      // capacity of one ring region
    int ring_slot_{0};
    void *fences_[kRing]{};                  // GLsync per region (opaque here)
    void setup_instance_attribs();

    // Remember the last two batches' draw parameters so a clean frame (nothing
    // changed) can re-issue the exact same draws without re-uploading a single
    // byte or touching a fence — the GPU buffers already hold the right data.
    struct DrawCall { std::uint32_t first{0}; std::uint32_t count{0}; };
    DrawCall last_draws_[2]{};
    int last_draw_n_{0};
    bool redraw_from_cache(PixelSize px); // returns false if no cached draws yet
    // Cache the last uniform/texture state so repeated flushes in a frame (and
    // across static frames) skip redundant GL calls.
    float u_px_w_{-1.0f}, u_px_h_{-1.0f};
    std::uint32_t bound_tex_{0};
    void bind_common(PixelSize px);

    // Packed-instance builders (colors are raw bytes; the shader normalizes).
    static Instance rect_inst(float x, float y, float w, float h,
                              std::uint8_t r, std::uint8_t g, std::uint8_t b,
                              std::uint8_t radius) noexcept {
        return Instance{x, y, w, h, 0, 0, 0, 0, r, g, b, 255, 0, radius, 0, 0};
    }

    std::vector<Instance> instances_{};
    std::vector<Instance> glyphs_{}; // scratch for the fused build pass
};

} // namespace gvte::gfx

#endif // GVTE_GFX_RENDERER_HPP
