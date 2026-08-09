// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The GPU renderer. Each frame it walks the Screen grid and builds two batches
// of instances: solid background rectangles, and textured glyph quads. Both
// are drawn with a single instanced draw call over a unit quad, so the whole
// screen is a couple of GL calls regardless of cell count.

#ifndef TOE_GFX_RENDERER_HPP
#define TOE_GFX_RENDERER_HPP

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "toe/core/types.hpp"
#include "toe/gfx/font.hpp"
#include "toe/gfx/palette.hpp"
#include "toe/term/screen.hpp"

namespace toe::gfx {

class Renderer {
public:
    static Result<Renderer> create(FontAtlas &&atlas);

    // Opt out of the GL 4.4 persistent-mapped instance ring (a debug/driver
    // escape hatch — some broken drivers advertise GL_ARB_buffer_storage but
    // mishandle coherent mapping). Off by default; the library auto-detects the
    // capability and already falls back on a failed map. Call before create().
    // Replaces the former TOE_NO_PERSISTENT environment variable: policy is a
    // host decision, not an env read inside the library.
    static void set_persistent_mapping(bool enabled) noexcept;

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    Renderer(Renderer &&) noexcept;
    Renderer &operator=(Renderer &&) noexcept;
    ~Renderer();

    // Number of cells that fit in a pixel viewport, given the font's cell size.
    [[nodiscard]] Extent cells_for(PixelSize px) const noexcept;

    [[nodiscard]] const FontAtlas &font() const noexcept { return atlas_; }

    // Draw the whole screen into the current framebuffer of size `px`.
    // Draw the frame; returns the region that changed (in pixels) so the host
    // can damage only that area on the compositor. DamageRect::full when the
    // whole surface was repainted, empty() when nothing changed.
    DamageRect draw(const term::Screen &screen, PixelSize px, bool cursor_on = true,
                    bool blink_on = true);

    // Draw a raw CELL GRID as an overlay pass, on top of whatever is already in
    // the framebuffer, at pixel offset (ox, oy). `cells` is row-major, `cols`
    // wide by `rows` tall (a width-0 cell is a wide-glyph spacer, skipped).
    // This is the host's hook for in-terminal UI (a settings panel, a search
    // bar, notifications) rendered with the SAME font + pipeline as the grid.
    // No damage cache: overlays are transient and repaint wholesale. Requires a
    // current GL context and blending enabled (draw() leaves it on).
    void draw_cells(const term::Cell *cells, int cols, int rows, PixelSize px, int ox = 0,
                    int oy = 0);

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

    explicit Renderer(FontAtlas &&atlas) : atlas_{std::move(atlas)} {}

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
        bool has_blink{false};             // row contains SGR-blink cells
        std::vector<Instance> bg;          // background / selection rects
        std::vector<Instance> glyphs;      // textured glyph quads
    };
    std::vector<RowCache> rows_{};
    bool blink_on_{true};   // last blink phase the cache was built against
    bool blink_flip_{false}; // true for the frame where the blink phase flipped
    std::uint16_t hover_link_{0}; // last hovered OSC-8 link id the cache saw
    // Geometry the cache was built against; a change invalidates everything.
    int cache_cols_{-1}, cache_rows_{-1}, cache_cw_{0}, cache_ch_{0}, cache_ascent_{0};
    // Build (or reuse) one row's instances into rows_[r]. Returns true if the
    // row was rebuilt (its key changed).
    bool build_row(const term::Screen &screen, int r, std::uint64_t key,
                   bool row_has_cursor, bool cursor_block, int cur_col, std::int64_t abs_row,
                   bool any_selection, bool blink_on, term::Screen::LineAttr la);

    FontAtlas atlas_;
    Palette palette_{};
    std::uint64_t palette_epoch_seen_{0}; // last model palette_epoch() applied
    std::size_t palette_applied_{0};      // count of color_edits() consumed
    // sokol resources (opaque .id handles so this header avoids the sokol
    // include). One cell pipeline, a static unit-quad vbo, and a dynamic
    // instance buffer sokol streams for us (no manual persistent-ring/fences).
    std::uint32_t pip_{0};        // sg_pipeline: the cell pass
    std::uint32_t quad_vbuf_{0};  // sg_buffer: unit quad (per-vertex)
    std::uint32_t inst_vbuf_{0};  // sg_buffer: instances (per-instance, stream)
    std::size_t inst_capacity_{0}; // instance-buffer capacity in bytes
    std::uint32_t smp_{0};        // sg_sampler (nearest for coverage glyphs)
    std::uint32_t smp_linear_{0}; // sg_sampler (linear for colour emoji)

    // Remember the last frame's two batch sizes so a clean frame re-issues the
    // same draws without re-uploading (the instance buffer already holds them).
    struct DrawCall { std::uint32_t first{0}; std::uint32_t count{0}; };
    DrawCall last_draws_[2]{};
    int last_draw_n_{0};
    bool redraw_from_cache(PixelSize px); // false if no cached draws yet

    // --- inline images (kitty graphics) ------------------------------------
    // A separate RGBA-textured-quad pass drawn over the glyphs. Each image id
    // gets one sokol image + view, uploaded lazily as the revision advances.
    std::uint32_t image_pip_{0};
    std::uint32_t image_quad_vbuf_{0};
    struct ImageTex { std::uint32_t image{0}; std::uint32_t view{0}; };
    std::unordered_map<std::uint32_t, ImageTex> image_tex_{}; // image id -> sokol tex
    std::uint64_t images_revision_{0};
    void ensure_image_pipeline();
    void draw_images(const term::Screen &screen, PixelSize px);
    void draw_preedit(const term::Screen &screen, PixelSize px);
    // Draw kitty Unicode-placeholder cells (U+10EEEE): tile the image whose id
    // is in the cell fg colour across a contiguous block of placeholder cells.
    void draw_placeholders(const term::Screen &screen, PixelSize px);

    // --- ligature shaping (HarfBuzz) ---------------------------------------
    // Per-cell shaped-glyph override for a row. When a ligature (e.g. => != ->)
    // spans several cells, HarfBuzz yields one glyph for the cluster; we record
    // its glyph index on the FIRST cell and mark the covered trailing cells as
    // "skip" so they don't draw their own glyph.
    struct ShapedCell {
        std::uint32_t gindex{0}; // FT glyph index to draw (0 = use codepoint path)
        bool skip{false};        // covered by a preceding ligature; draw nothing
    };
    std::vector<ShapedCell> shape_scratch_{};
    bool ligatures_{true};
    // Fill shape_scratch_ for one row by shaping its ASCII runs; no-op (all
    // zero) when ligatures are off or the row has none.
    void shape_row(std::span<const term::Cell> cells, int cols);
    // The last screen-size uniform the flush pass used, so a static frame skips
    // redundant uniform work.
    float u_px_w_{-1.0f}, u_px_h_{-1.0f};
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

} // namespace toe::gfx

#endif // TOE_GFX_RENDERER_HPP
