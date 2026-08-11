// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/gfx/renderer.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <utility>
#include <array>
#include <algorithm>

#include "sokol/sokol_gfx.h"
#include "toe/gfx/gpu.hpp"
#include "toe/gfx/box_glyphs.hpp"
#include "shaders.glsl.h" // sokol-shdc: cell_shader_desc / image_shader_desc + slots

namespace toe::gfx {

namespace {

// The shaders now live in src/gfx/shaders.glsl, compiled by sokol-shdc into
// shaders.glsl.h (cell + image programs) for every backend. See that file.

// Byte -> normalized float, via a 256-entry table (no per-cell division).
const std::array<float, 256> &fr_table() {
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) a[static_cast<std::size_t>(i)] = static_cast<float>(i) / 255.0f;
        return a;
    }();
    return t;
}
inline float fr(std::uint8_t v) noexcept { return fr_table()[v]; }

// Relative luminance (WCAG-ish, cheap linear approximation — good enough for a
// contrast decision, no need for the full sRGB gamma curve per cell).
inline float luma(Rgb c) noexcept {
    return (0.2126f * static_cast<float>(c.r) + 0.7152f * static_cast<float>(c.g) +
            0.0722f * static_cast<float>(c.b)) /
           255.0f;
}

// Guarantee selected text stays readable: keep the cell's own foreground when
// it CLEARLY contrasts with the selection background, otherwise flip it to
// black or white — whichever the selection bg is farther from. This is what
// makes selection "look good ALWAYS", independent of the theme or of colours
// the running program picks for individual cells.
inline Rgb contrast_fg(Rgb fg, Rgb sel_bg) noexcept {
    const float lf = luma(fg), lb = luma(sel_bg);
    // WCAG contrast ratio on luminance (+0.05). Require a solid ratio so text
    // never looks muddy on the highlight — 3.0:1 is the AA large-text floor and
    // a good perceptual threshold for a monospaced grid. Below it we don't try
    // to nudge the cell colour; we go straight to pure black/white (whichever
    // is farther from the selection bg) for MAXIMUM legibility.
    const float hi = std::max(lf, lb) + 0.05f, lo = std::min(lf, lb) + 0.05f;
    if (hi / lo >= 3.0f) return fg; // already clearly legible — keep the colour
    return lb > 0.45f ? rgb(16, 18, 24) : rgb(244, 246, 250);
}

// Procedural block-element and box-drawing rendering. The Unicode block
// characters (U+2580–259F) and line-drawing characters (U+2500–257F) are exact
// geometric shapes on the cell — but a font's bitmaps for them are usually a
// pixel or two shy of the cell metrics, so adjacent cells leave visible seams
// (disconnected blocks in ASCII-art, and broken vertical/horizontal rules in
// TUI borders). Draw them as precise cell-relative rectangles instead, so they
// tile and CONNECT pixel-perfectly at any font size.
//
// fills[] receive rects in [0,1] cell coordinates (x, y, w, h), y from the TOP.
// A line/junction is built from stubs that run exactly to the cell edge, so a
// stub in one cell meets its neighbour's stub with no gap. Returns the number
// of rects written (0 = not a procedural glyph; use the font atlas).
// The full procedural glyph table (box drawing, blocks, quadrants, braille,
// double lines) lives in box_glyphs.hpp — crisp, uniform, gap-free TUI chrome.
// (CellRect, cell_fills, kMaxFills are in the toe::gfx namespace, included above.)

} // namespace

namespace {
// Retained for API compatibility; sokol streams the instance buffer for us, so
// there's no persistent-mapped-ring toggle anymore. Kept as a no-op so hosts
// calling set_persistent_mapping() still link.
bool g_persistent_mapping_allowed = true;
} // namespace

void Renderer::set_persistent_mapping(bool enabled) noexcept {
    g_persistent_mapping_allowed = enabled;
}

Result<Renderer> Renderer::create(FontAtlas &&atlas) {
    Renderer r{std::move(atlas)};
    r.ensure_buffers();
    if (r.pip_ == 0) return fail("renderer: sokol pipeline creation failed");
    return r;
}

Renderer::~Renderer() {
    // Destroy the pipeline BEFORE its shader (sokol: the pipeline references it).
    if (pip_) sg_destroy_pipeline(sg_pipeline{pip_});
    if (shd_) sg_destroy_shader(sg_shader{shd_});
    if (quad_vbuf_) sg_destroy_buffer(sg_buffer{quad_vbuf_});
    if (inst_vbuf_) sg_destroy_buffer(sg_buffer{inst_vbuf_});
    if (smp_) sg_destroy_sampler(sg_sampler{smp_});
    if (smp_linear_) sg_destroy_sampler(sg_sampler{smp_linear_});
    if (image_pip_) sg_destroy_pipeline(sg_pipeline{image_pip_});
    if (image_quad_vbuf_) sg_destroy_buffer(sg_buffer{image_quad_vbuf_});
    for (auto &[id, t] : image_tex_) {
        if (t.view) gpu::destroy_view(t.view);
        if (t.image) gpu::destroy_image(t.image);
    }
}

Renderer::Renderer(Renderer &&o) noexcept
    : atlas_{std::move(o.atlas_)}, palette_{o.palette_},
      palette_epoch_seen_{o.palette_epoch_seen_}, palette_applied_{o.palette_applied_},
      pip_{std::exchange(o.pip_, 0)}, shd_{std::exchange(o.shd_, 0)},
      quad_vbuf_{std::exchange(o.quad_vbuf_, 0)},
      inst_vbuf_{std::exchange(o.inst_vbuf_, 0)}, inst_capacity_{o.inst_capacity_},
      smp_{std::exchange(o.smp_, 0)}, smp_linear_{std::exchange(o.smp_linear_, 0)},
      image_pip_{std::exchange(o.image_pip_, 0)},
      image_quad_vbuf_{std::exchange(o.image_quad_vbuf_, 0)},
      image_tex_{std::move(o.image_tex_)}, images_revision_{o.images_revision_},
      instances_{std::move(o.instances_)}, glyphs_{std::move(o.glyphs_)} {}

Renderer &Renderer::operator=(Renderer &&o) noexcept {
    if (this != &o) {
        // Free this renderer's GL resources before adopting o's. Pipeline before
        // its shader (sokol dependency order); leaking shd_ here is what caused
        // "shader pool exhausted" (id 154) on repeated font changes.
        if (pip_) sg_destroy_pipeline(sg_pipeline{pip_});
        if (shd_) sg_destroy_shader(sg_shader{shd_});
        if (quad_vbuf_) sg_destroy_buffer(sg_buffer{quad_vbuf_});
        if (inst_vbuf_) sg_destroy_buffer(sg_buffer{inst_vbuf_});
        if (smp_) sg_destroy_sampler(sg_sampler{smp_});
        if (smp_linear_) sg_destroy_sampler(sg_sampler{smp_linear_});
        if (image_pip_) sg_destroy_pipeline(sg_pipeline{image_pip_});
        if (image_quad_vbuf_) sg_destroy_buffer(sg_buffer{image_quad_vbuf_});
        for (auto &[id, t] : image_tex_) {
            if (t.view) gpu::destroy_view(t.view);
            if (t.image) gpu::destroy_image(t.image);
        }
        atlas_ = std::move(o.atlas_);
        palette_ = o.palette_;
        palette_epoch_seen_ = o.palette_epoch_seen_;
        palette_applied_ = o.palette_applied_;
        pip_ = std::exchange(o.pip_, 0);
        shd_ = std::exchange(o.shd_, 0);
        quad_vbuf_ = std::exchange(o.quad_vbuf_, 0);
        inst_vbuf_ = std::exchange(o.inst_vbuf_, 0);
        inst_capacity_ = o.inst_capacity_;
        smp_ = std::exchange(o.smp_, 0);
        smp_linear_ = std::exchange(o.smp_linear_, 0);
        image_pip_ = std::exchange(o.image_pip_, 0);
        image_quad_vbuf_ = std::exchange(o.image_quad_vbuf_, 0);
        image_tex_ = std::move(o.image_tex_);
        images_revision_ = o.images_revision_;
        instances_ = std::move(o.instances_);
        glyphs_ = std::move(o.glyphs_);
    }
    return *this;
}

Extent Renderer::cells_for(PixelSize px) const noexcept {
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    // Reserve the window padding on every edge (2*pad total per axis).
    const int w = std::max(0, px.w - 2 * pad_);
    const int h = std::max(0, px.h - 2 * pad_);
    return Extent{cw > 0 ? std::max(1, w / cw) : 1, ch > 0 ? std::max(1, h / ch) : 1};
}

// Build the cell pipeline + static quad + a dynamic instance buffer sokol
// streams for us (sg_append_buffer packs both batches per frame). Two samplers:
// NEAREST for the coverage atlas, LINEAR for colour emoji.
void Renderer::ensure_buffers() {
    static constexpr float kQuad[] = {0, 0, 1, 0, 0, 1, 1, 1}; // triangle strip
    sg_buffer_desc qb = {};
    qb.data = SG_RANGE(kQuad);
    quad_vbuf_ = sg_make_buffer(&qb).id;

    // Instance stream: sized for a big screen (~160k instances). stream_update
    // lets us sg_append_buffer twice per frame (bg then glyphs).
    inst_capacity_ = std::size_t{160000} * sizeof(Instance);
    sg_buffer_desc ib = {};
    ib.size = inst_capacity_;
    ib.usage.stream_update = true;
    inst_vbuf_ = sg_make_buffer(&ib).id;

    sg_sampler_desc sd = {};
    sd.min_filter = SG_FILTER_NEAREST; sd.mag_filter = SG_FILTER_NEAREST;
    sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE; sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp_ = sg_make_sampler(&sd).id;
    sd.min_filter = SG_FILTER_LINEAR; sd.mag_filter = SG_FILTER_LINEAR;
    smp_linear_ = sg_make_sampler(&sd).id;

    sg_pipeline_desc pd = {};
    // Own the shader handle so it can be destroyed on rebuild (font/ligature
    // change move-assigns a fresh Renderer over this one). Leaking it here fills
    // sokol's shader pool and eventually spams "shader pool exhausted" (id 154).
    shd_ = sg_make_shader(cell_shader_desc(sg_query_backend())).id;
    pd.shader = sg_shader{shd_};
    pd.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_VERTEX;   // unit quad
    pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE; // instances
    pd.layout.buffers[1].stride = sizeof(Instance);
    // Packed Instance: rect f32x4 @0, uv f32x4 @16, color u8x4-norm @32,
    // is_glyph u8-norm... but the shader needs FLOAT is_glyph/radius, so the
    // sokol vertex format normalizes u8->[0,1] then the shader scales back.
    pd.layout.attrs[ATTR_cell_aCorner]  = {0, 0,  SG_VERTEXFORMAT_FLOAT2};
    pd.layout.attrs[ATTR_cell_aRect]    = {1, 0,  SG_VERTEXFORMAT_FLOAT4};
    pd.layout.attrs[ATTR_cell_aUV]      = {1, 16, SG_VERTEXFORMAT_FLOAT4};
    pd.layout.attrs[ATTR_cell_aColor]   = {1, 32, SG_VERTEXFORMAT_UBYTE4N};
    // Flags packed as raw u8 at offset 36: [mode, radius, shape, spare]. UBYTE4N
    // is the only ubyte format that yields a FLOAT vec4 shader attr (UBYTE4
    // yields int, rejected against the float `in`); it normalizes each byte to
    // [0,1] and the shader multiplies by 255 to recover the raw values.
    pd.layout.attrs[ATTR_cell_aFlags]   = {1, 36, SG_VERTEXFORMAT_UBYTE4N};
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    // The host swapchain is colour-only (no depth/stencil attachment), so the
    // pipeline must declare no depth format — otherwise sokol's validation
    // layer rejects sg_apply_pipeline (APIP_SWAPCHAIN_DEPTH_FORMAT).
    pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pip_ = sg_make_pipeline(&pd).id;
}

// Apply the pipeline + screen-size uniform + atlas bindings for a flush.
void Renderer::bind_common(PixelSize px) {
    atlas_.sync_gpu(); // upload any newly-packed glyphs to the GPU images
    sg_apply_pipeline(sg_pipeline{pip_});
    sg_bindings bind = {};
    bind.vertex_buffers[0] = sg_buffer{quad_vbuf_};
    bind.vertex_buffers[1] = sg_buffer{inst_vbuf_};
    bind.views[VIEW_uAtlas] = sg_view{atlas_.glyph_view()};
    // Colour atlas: if none yet (no emoji), reuse the glyph view so the slot is
    // valid; the shader only samples it for is_color instances.
    const std::uint32_t cv = atlas_.color_view();
    bind.views[VIEW_uColorAtlas] = sg_view{cv ? cv : atlas_.glyph_view()};
    bind.samplers[SMP_uSmp] = sg_sampler{smp_};
    sg_apply_bindings(&bind);
    cell_vs_params_t vsp = {};
    vsp.uScreen[0] = static_cast<float>(px.w);
    vsp.uScreen[1] = static_cast<float>(px.h);
    vsp.uScreen[2] = static_cast<float>(pad_); // origin x (window padding)
    vsp.uScreen[3] = static_cast<float>(pad_); // origin y
    sg_apply_uniforms(UB_cell_vs_params, SG_RANGE(vsp));
    cell_fs_params_t fsp = {};
    fsp.uOpacity = opacity_;
    sg_apply_uniforms(UB_cell_fs_params, SG_RANGE(fsp));
}

// Clean-frame fast path is handled by the host re-presenting; with sokol we
// simply re-flush the cached instances (cheap). Returns false to force a
// normal flush (the instance data must be re-appended into the frame's buffer).
bool Renderer::redraw_from_cache(PixelSize) { return false; }

// Append `insts` into the streaming instance buffer and issue one instanced
// draw. sokol tracks the append offset within the frame, so calling flush twice
// (bg then glyphs) packs them contiguously. Must be inside an active sg pass
// (the host begins/ends the swapchain pass around Session::render).
void Renderer::flush(std::span<const Instance> insts, PixelSize px) {
    const std::size_t count = insts.size();
    if (count == 0) return;
    const int off = sg_append_buffer(
        sg_buffer{inst_vbuf_},
        [&] { sg_range r = {insts.data(), count * sizeof(Instance)}; return r; }());
    bind_common(px);
    // Re-bind with the append offset so this draw reads its own instances.
    sg_bindings bind = {};
    bind.vertex_buffers[0] = sg_buffer{quad_vbuf_};
    bind.vertex_buffers[1] = sg_buffer{inst_vbuf_};
    bind.vertex_buffer_offsets[1] = off;
    bind.views[VIEW_uAtlas] = sg_view{atlas_.glyph_view()};
    const std::uint32_t cv = atlas_.color_view();
    bind.views[VIEW_uColorAtlas] = sg_view{cv ? cv : atlas_.glyph_view()};
    bind.samplers[SMP_uSmp] = sg_sampler{smp_};
    sg_apply_bindings(&bind);
    sg_draw(0, 4, static_cast<int>(count));
}

namespace {
// Fast row fingerprint. Cells are 16 bytes, 4-byte aligned, so we fold the
// row as a stream of uint32 words with a wyhash-style multiply-mix — ~4x fewer
// iterations than a per-byte FNV and far cheaper than the glyph/palette work it
// lets us skip on unchanged rows.
inline std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}
inline void hash_words(std::uint64_t &h, const void *p, std::size_t bytes) noexcept {
    const auto *w = static_cast<const std::uint32_t *>(p);
    const std::size_t n = bytes / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ w[i]) * 0x100000001b3ULL;
    }
}
template <class T> inline void hash_val(std::uint64_t &h, const T &v) noexcept {
    std::uint64_t x = 0;
    __builtin_memcpy(&x, &v, sizeof(T) < 8 ? sizeof(T) : 8);
    h = (h ^ x) * 0x100000001b3ULL;
}
} // namespace

// Rebuild rows_[r] from the current cell contents unless its fingerprint is
// unchanged. Dirty rows are (re)built into the row's shadow vectors and
// appended to the flat draw buffers (instances_ / glyphs_). Clean rows skip all
// per-cell work and are spliced straight from the shadow. Returns true if the
// row was rebuilt.
bool Renderer::build_row(const term::Screen &screen, int r, std::uint64_t key,
                         bool row_has_cursor, bool cursor_block, int cur_col, std::int64_t abs_row,
                         bool any_selection, bool any_search, bool blink_on,
                         term::Screen::LineAttr la) {
    RowCache &rc = rows_[static_cast<std::size_t>(r)];
    // Clean row: reuse the shadow — unless it holds blinking cells and the
    // blink phase just flipped, which needs a rebuild to hide/show them.
    if (rc.valid && rc.key == key && !(rc.has_blink && blink_flip_)) return false;

    const int cw = cache_cw_, ch = cache_ch_, ascent = cache_ascent_;
    const auto cells = screen.row(Row{r});
    const float ry = static_cast<float>(r * ch);
    const int base_gy = r * ch + ascent;
    rc.bg.clear();
    rc.glyphs.clear();
    rc.has_blink = false;

    // Shape the row's ASCII runs so programming ligatures merge into one glyph.
    shape_row(cells, cache_cols_);

    for (int c = 0; c < cache_cols_; ++c) {
        const auto &cell = cells[static_cast<std::size_t>(c)];
        const bool selected = any_selection && screen.is_selected(abs_row, c);
        const bool searched = any_search && screen.is_search_match(abs_row, c);
        const bool search_cur = searched && screen.is_current_search_match(abs_row, c);
        const Rgb match_bg = search_cur ? search_cur_bg_ : search_bg_;
        const bool reverse = term::has(cell.pen.attr, term::Attr::Reverse);
        const bool on_cursor = row_has_cursor && cursor_block && c == cur_col;

        if (selected) {
            // Round only the OUTER corners of the selection region so a
            // multi-cell selection reads as one smooth shape while interior
            // cell seams stay flush (no lumpy per-cell rounding). A corner is
            // exterior when both cells adjacent to it are unselected.
            const bool up = screen.is_selected(abs_row - 1, c);
            const bool dn = screen.is_selected(abs_row + 1, c);
            const bool lf = c > 0 && screen.is_selected(abs_row, c - 1);
            const bool rt = c + 1 < cache_cols_ && screen.is_selected(abs_row, c + 1);
            std::uint8_t corners = 0;
            if (!up && !lf) corners |= kCornerTL;
            if (!up && !rt) corners |= kCornerTR;
            if (!dn && !rt) corners |= kCornerBR;
            if (!dn && !lf) corners |= kCornerBL;
            // Radius scaled to the cell so the selection reads as a smooth,
            // rounded region at ANY font size (a fixed small cap looked like a
            // hard rectangle on large fonts). ~28% of the smaller cell extent,
            // clamped to a sane pixel range — matches the rounded block cursor.
            const std::uint8_t rad = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(std::min(cw, ch) * 0.28f), 2, 10));
            // Reverse-video selection: the highlight bg is the cell's OWN
            // foreground colour (so the text, drawn in the cell's bg below,
            // reads as inverted). Otherwise the configured selection colour.
            Rgb selbg = selection_bg_;
            if (selection_invert_) {
                selbg = palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);
            }
            if (corners == 0 || rad == 0) {
                rc.bg.push_back(rect_inst(static_cast<float>(c * cw), ry, static_cast<float>(cw),
                                          static_cast<float>(ch), selbg.r, selbg.g,
                                          selbg.b, /*radius=*/0));
            } else {
                rc.bg.push_back(rect_round_inst(
                    static_cast<float>(c * cw), ry, static_cast<float>(cw), static_cast<float>(ch),
                    selbg.r, selbg.g, selbg.b, rad, corners));
            }
        } else if (searched) {
            // Search-match highlight: same rounded-outer-corner treatment as the
            // selection region, but the current match uses the brighter colour.
            // Corners round against OTHER cells in the SAME kind of match run
            // (a current match sits inside the all-match run, so it rounds
            // against non-current cells too — keep it simple: round against any
            // match neighbour).
            const bool up = screen.is_search_match(abs_row - 1, c);
            const bool dn = screen.is_search_match(abs_row + 1, c);
            const bool lf = c > 0 && screen.is_search_match(abs_row, c - 1);
            const bool rt = c + 1 < cache_cols_ && screen.is_search_match(abs_row, c + 1);
            std::uint8_t corners = 0;
            if (!up && !lf) corners |= kCornerTL;
            if (!up && !rt) corners |= kCornerTR;
            if (!dn && !rt) corners |= kCornerBR;
            if (!dn && !lf) corners |= kCornerBL;
            const std::uint8_t rad = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(std::min(cw, ch) * 0.28f), 2, 10));
            if (corners == 0 || rad == 0) {
                rc.bg.push_back(rect_inst(static_cast<float>(c * cw), ry, static_cast<float>(cw),
                                          static_cast<float>(ch), match_bg.r, match_bg.g,
                                          match_bg.b, /*radius=*/0));
            } else {
                rc.bg.push_back(rect_round_inst(
                    static_cast<float>(c * cw), ry, static_cast<float>(cw), static_cast<float>(ch),
                    match_bg.r, match_bg.g, match_bg.b, rad, corners));
            }
        } else if (reverse || !std::holds_alternative<term::DefaultColor>(cell.pen.bg)) {
            const term::Color bg = reverse ? cell.pen.fg : cell.pen.bg;
            const Rgb col = palette_.resolve(bg, /*is_fg=*/reverse);
            rc.bg.push_back(rect_inst(static_cast<float>(c * cw), ry, static_cast<float>(cw),
                                      static_cast<float>(ch), col.r, col.g, col.b, /*radius=*/0));
        }

        const char32_t cp = cell.cp;

        // Selected text must stay legible no matter what colour the running
        // program picked for this cell. Compute the fg override once per cell:
        // an explicit selection_fg_ wins; otherwise auto-contrast against the
        // selection background. Applied to glyphs, decorations, SDF and block
        // fills below so EVERYTHING under the highlight reads well.
        const auto sel_adjust = [&](Rgb fg) -> Rgb {
            if (searched) return contrast_fg(fg, match_bg);
            if (!selected) return fg;
            // Reverse-video: the glyph takes the cell's own BACKGROUND colour so
            // it sits legibly on the inverted (fg-coloured) highlight.
            if (selection_invert_)
                return palette_.resolve(reverse ? cell.pen.fg : cell.pen.bg, reverse);
            if (selection_fg_) return *selection_fg_;
            return contrast_fg(fg, selection_bg_);
        };
        // Blink (SGR 5): during the OFF phase the glyph is hidden. Track that
        // the row has blinking cells so the phase flip forces a rebuild.
        const bool blink = term::has(cell.pen.attr, term::Attr::Blink);
        if (blink) rc.has_blink = true;

        // Text decorations (underline styles, strikethrough, overline) are thin
        // fg-coloured bars that must render even on blank cells — git diff and
        // spell-checkers underline runs that include spaces. Emit them before
        // the space-skip below. They go into rc.bg; the glyph draws on top.
        const term::Attr at = cell.pen.attr;
        const bool has_ul = term::has(at, term::Attr::Underline);
        const bool has_st = term::has(at, term::Attr::Strike);
        const bool has_ol = term::has(at, term::Attr::Overline);
        // A hovered OSC 8 link gets a single underline so it reads as clickable,
        // even if the text itself wasn't underlined.
        const bool hovered = hover_link_ != 0 && cell.link == hover_link_;
        if (has_ul || has_st || has_ol || hovered) {
            Rgb dc = on_cursor ? palette_.resolve(cell.pen.bg, /*is_fg=*/false)
                               : palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);
            if (term::has(at, term::Attr::Faint)) {
                const Rgb bg = palette_.resolve(reverse ? cell.pen.fg : cell.pen.bg, reverse);
                auto dim = [](std::uint8_t f, std::uint8_t b) {
                    return static_cast<std::uint8_t>((f * 45 + b * 55) / 100);
                };
                dc = {dim(dc.r, bg.r), dim(dc.g, bg.g), dim(dc.b, bg.b)};
            }
            dc = sel_adjust(dc); // keep underlines/strikes legible under selection
            // Underline gets its own colour (SGR 58) when set; strike/overline
            // always use the text colour above.
            Rgb ulc = dc;
            if (!std::holds_alternative<term::DefaultColor>(cell.pen.underline_color)) {
                ulc = sel_adjust(palette_.resolve(cell.pen.underline_color, /*is_fg=*/true));
            }
            const float cx = static_cast<float>(c * cw);
            const float fcw = static_cast<float>(cw);
            // Stroke thickness scales with cell height (min 1px).
            const float thick = std::max(1.0f, static_cast<float>(ch) / 14.0f);
            auto bar = [&](Rgb col, float y0, float x, float w) {
                rc.bg.push_back(rect_inst(cx + x, ry + y0, w, thick, col.r, col.g, col.b, 0));
            };
            if (has_ul) {
                // Underline sits just below the baseline.
                const float uy = static_cast<float>(ascent) + thick;
                switch (cell.pen.underline) {
                case term::Underline::Double:
                    bar(ulc, uy, 0, fcw);
                    bar(ulc, uy + thick * 2.0f, 0, fcw);
                    break;
                case term::Underline::Dotted: {
                    const float d = thick * 2.0f;
                    for (float x = 0; x + thick <= fcw; x += d) bar(ulc, uy, x, thick);
                    break;
                }
                case term::Underline::Dashed: {
                    const float seg = fcw / 3.0f;
                    bar(ulc, uy, 0, seg);
                    bar(ulc, uy, 2.0f * seg, seg);
                    break;
                }
                case term::Underline::Curly: {
                    // Approximate a wave with short segments alternating y.
                    const int seg = 4;
                    const float sw = fcw / static_cast<float>(seg);
                    for (int s = 0; s < seg; ++s) {
                        const float yoff = (s % 2 == 0) ? 0.0f : thick * 1.5f;
                        bar(ulc, uy + yoff, static_cast<float>(s) * sw, sw);
                    }
                    break;
                }
                default: // Single
                    bar(ulc, uy, 0, fcw);
                    break;
                }
            } else if (hovered) {
                // Hover underline (link not otherwise underlined): a single line
                // in the text colour, with ROUNDED CAPS at the ends of the
                // hovered run so a link reads as one pill-shaped underline
                // instead of a chain of flat bars. A cap rounds only where the
                // adjacent cell isn't part of the same link.
                const bool left_hov =
                    c > 0 && cells[static_cast<std::size_t>(c - 1)].link == hover_link_;
                const bool right_hov = c + 1 < cache_cols_ &&
                                       cells[static_cast<std::size_t>(c + 1)].link == hover_link_;
                const float hy = static_cast<float>(ascent) + thick;
                // Underline bar is `thick` tall; a radius up to ~thick rounds it
                // into a semicircular cap at the run ends.
                const std::uint8_t hrad = static_cast<std::uint8_t>(std::max(1.0f, thick));
                std::uint8_t mask = 0;
                if (!left_hov) mask |= kCornerTL | kCornerBL;
                if (!right_hov) mask |= kCornerTR | kCornerBR;
                if (mask == 0) {
                    bar(dc, hy, 0, fcw);
                } else {
                    rc.bg.push_back(rect_round_inst(cx, ry + hy, fcw, thick, dc.r, dc.g, dc.b,
                                                    hrad, mask));
                }
            }
            if (has_st) bar(dc, static_cast<float>(ascent) * 0.62f, 0, fcw); // strike ~mid-x-height
            if (has_ol) bar(dc, 0.0f, 0, fcw);                               // overline at cell top
        }

        if (cp == U' ' || cp == 0 || cell.spacer()) continue;
        if (blink && !blink_on) continue; // blink OFF phase: hide the glyph
        Rgb fgcol = on_cursor
                        ? palette_.resolve(cell.pen.bg, /*is_fg=*/false)
                        : palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);

        // Faint (SGR 2 / dim): mix the foreground ~55% toward the background so
        // de-emphasised text (ls, git, prompts) reads as dimmed, not normal.
        if (term::has(cell.pen.attr, term::Attr::Faint)) {
            const Rgb bg = palette_.resolve(reverse ? cell.pen.fg : cell.pen.bg, reverse);
            auto dim = [](std::uint8_t f, std::uint8_t b) {
                return static_cast<std::uint8_t>((f * 45 + b * 55) / 100);
            };
            fgcol = {dim(fgcol.r, bg.r), dim(fgcol.g, bg.g), dim(fgcol.b, bg.b)};
        }
        // Contrast-guarantee the glyph colour against the selection highlight
        // (after faint, so dimmed selected text is still lifted to legible).
        fgcol = sel_adjust(fgcol);

        // Analytic SDF glyphs (Powerline separators, rounded corners) render
        // as a single distance-field cell — crisp and antialiased at ANY size,
        // no atlas. Checked before the rect path; goes into rc.bg like fills.
        if (std::uint8_t sh = cell_sdf(cp)) {
            const float fcw = static_cast<float>(cw), fch = static_cast<float>(ch);
            const float cx = static_cast<float>(c * cw);
            rc.bg.push_back(sdf_inst(cx, ry, fcw, fch, fgcol.r, fgcol.g, fgcol.b, sh));
            continue;
        }

        // Geometric block elements and box-drawing lines tile/connect pixel-
        // perfectly only when drawn as exact cell-relative rects — the font's
        // bitmaps are a pixel shy and leave seams (broken vertical rules,
        // disconnected blocks). Draw them procedurally in the fg colour; they go
        // into rc.bg so any real glyph still layers on top and z-order holds.
        if (CellRect rects[kMaxFills]; int nfills = cell_fills(cp, rects)) {
            const float fcw = static_cast<float>(cw), fch = static_cast<float>(ch);
            const float cx = static_cast<float>(c * cw);
            for (int i = 0; i < nfills; ++i) {
                const CellRect &b = rects[i];
                rc.bg.push_back(rect_inst(cx + b.x * fcw, ry + b.y * fch, b.w * fcw, b.h * fch,
                                          fgcol.r, fgcol.g, fgcol.b, /*radius=*/0));
            }
            continue;
        }

        // Style: synthesize bold/italic from the cell's SGR attributes.
        const auto style = static_cast<gfx::FontStyle>(
            (term::has(cell.pen.attr, term::Attr::Bold) ? 1u : 0u) |
            (term::has(cell.pen.attr, term::Attr::Italic) ? 2u : 0u));

        // Ligatures: a cell covered by a preceding ligature draws nothing; the
        // first cell of a ligature draws the merged glyph (by glyph index).
        const ShapedCell &sh = shape_scratch_[static_cast<std::size_t>(c)];
        if (sh.skip) continue;
        const GlyphInfo *gi = sh.gindex != 0 ? atlas_.glyph_by_index(sh.gindex, style)
                                             : atlas_.glyph(cp, style);
        if (!gi || gi->width == 0 || gi->height == 0) continue;
        const Rgb col = fgcol;
        const float gx = static_cast<float>(c * cw + gi->bearing_x);
        const float gy = static_cast<float>(base_gy - gi->bearing_y);
        rc.glyphs.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                     static_cast<float>(gi->height),
                                     gi->u0, gi->v0, gi->u1, gi->v1,
                                     col.r, col.g, col.b, 255,
                                     /*is_glyph=*/static_cast<std::uint8_t>(gi->is_color ? 2 : 1),
                                     0, 0, 0});
    }
    rc.key = key;
    rc.valid = true;

    // DEC line attributes: scale the row's primitives. Double-width (and both
    // halves of double-height) doubles each cell's horizontal extent; double-
    // height additionally scales vertically 2x and shows only its half.
    if (la != term::Screen::LineAttr::normal) {
        using LA = term::Screen::LineAttr;
        const float row_top = ry;
        const float row_h = static_cast<float>(ch);
        const auto xform = [&](Instance &in) {
            in.x = in.x * 2.0f;   // origin is column 0 of the row (x=0-based)
            in.w = in.w * 2.0f;
            if (la == LA::double_top || la == LA::double_bottom) {
                // Map the glyph into a 2x-tall line, then show only this half.
                const float rel = (in.y - row_top) * 2.0f;
                float y2 = row_top + rel;
                in.h *= 2.0f;
                if (la == LA::double_bottom) y2 -= row_h; // bottom half slides up
                in.y = y2;
            }
        };
        for (auto &in : rc.bg) xform(in);
        for (auto &in : rc.glyphs) xform(in);
    }
    return true;
}

// Shape one row's ASCII runs with HarfBuzz so programming ligatures (=> != ->
// >= <= == === |> and friends) render correctly. Programming fonts implement
// these as CONTEXTUAL ALTERNATES (calt): each character is substituted with a
// connected glyph *variant*, so the glyph count matches the cell count but the
// glyph indices differ from the plain codepoint. We shape each same-style ASCII
// run and record the resulting glyph index per cell; the codepoint path is used
// where shaping yields nothing special. Only runs when a row is (re)built.
void Renderer::shape_row(std::span<const term::Cell> cells, int cols) {
    shape_scratch_.assign(static_cast<std::size_t>(cols), ShapedCell{});
    if (!ligatures_ || !atlas_.has_shaper()) return;

    std::vector<char32_t> run;
    std::vector<std::uint32_t> gids;
    int c = 0;
    while (c < cols) {
        // A run is contiguous same-style printable ASCII (ligatures are ASCII).
        const term::Cell &c0 = cells[static_cast<std::size_t>(c)];
        const bool printable = c0.cp >= 0x21 && c0.cp < 0x7f && c0.width == 1;
        if (!printable) { ++c; continue; }
        const term::Attr style0 = c0.pen.attr & (term::Attr::Bold | term::Attr::Italic);
        int e = c;
        while (e < cols) {
            const term::Cell &ce = cells[static_cast<std::size_t>(e)];
            const bool ep = ce.cp >= 0x21 && ce.cp < 0x7f && ce.width == 1;
            if (!ep || (ce.pen.attr & (term::Attr::Bold | term::Attr::Italic)) != style0) break;
            ++e;
        }
        const int len = e - c;
        if (len >= 2) {
            run.assign(static_cast<std::size_t>(len), 0);
            gids.assign(static_cast<std::size_t>(len), 0);
            for (int i = 0; i < len; ++i)
                run[static_cast<std::size_t>(i)] = cells[static_cast<std::size_t>(c + i)].cp;
            atlas_.shape_run(run, gids);
            for (int i = 0; i < len; ++i) {
                const std::uint32_t g = gids[static_cast<std::size_t>(i)];
                if (g == 0) {
                    // Ligated-away cell: hidden, its glyph merged into an
                    // earlier one. Skip drawing it.
                    shape_scratch_[static_cast<std::size_t>(c + i)].skip = true;
                } else {
                    // Draw this glyph by index (may be a connected ligature
                    // variant even when it maps 1:1 to the cell).
                    shape_scratch_[static_cast<std::size_t>(c + i)].gindex = g;
                }
            }
            // The first cell of a run is never a skip (glyph 0 there means the
            // shaper produced nothing special — fall back to the codepoint).
            shape_scratch_[static_cast<std::size_t>(c)].skip = false;
        }
        c = e;
    }
}

// --- inline image (kitty graphics) pipeline --------------------------------
// NOTE: the inline-image pass (kitty graphics protocol) is temporarily a no-op
// under the sokol backend — text, colour, and everything else render fully; only
// transmitted bitmap images don't display yet. The image shader is already in
// shaders.glsl (image program); wiring the sokol image pipeline is a follow-up.
void Renderer::ensure_image_pipeline() {}
void Renderer::draw_images(const term::Screen &, PixelSize) {}
void Renderer::draw_placeholders(const term::Screen &, PixelSize) {}

bool Renderer::animating() const noexcept {
    if (cursor_in_flight_) return true;
    if (bell_until_us_ == 0) return false;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    return now < bell_until_us_;
}

// Ease the rendered cursor toward its target cell and emit the cursor rect(s).
// Exponential smoothing (~55ms time constant) gives a snappy-but-smooth glide;
// on a big jump we lay down a few trail rects between old and new, faded by
// lerping their colour toward the background (the pipeline has no per-instance
// alpha, and lerp-to-bg reads identically on the opaque terminal). Sets
// cursor_in_flight_ while still moving so the host keeps animating.
void Renderer::animate_cursor(float tgt_x, float tgt_y, float cw, float ch, Rgb col, Rgb bg,
                              std::vector<Instance> &out) {
    // Corner radius scaled to the cell (like the selection highlight) so the
    // block cursor reads as a soft rounded chip at any font size, not a hard
    // rectangle. Capped so tiny cells stay sensible.
    const std::uint8_t crad = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::min(cw, ch) * 0.22f), 2, 6));
    const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    auto lerp8 = [](std::uint8_t a, std::uint8_t b, float t) {
        return static_cast<std::uint8_t>(a + (static_cast<float>(b) - a) * t + 0.5f);
    };
    if (cur_anim_x_ < 0.0f || !cursor_anim_enabled_) {
        cur_anim_x_ = tgt_x; cur_anim_y_ = tgt_y; cur_last_us_ = now;
        cursor_in_flight_ = false;
        out.push_back(rect_inst(tgt_x, tgt_y, cw, ch, col.r, col.g, col.b, crad));
        return;
    }
    float dt = static_cast<float>(now - cur_last_us_) / 1e6f;
    cur_last_us_ = now;
    dt = std::clamp(dt, 0.0f, 0.05f); // ignore long stalls (tab switch / sleep)

    const float px0 = cur_anim_x_, py0 = cur_anim_y_;
    const float a = 1.0f - std::exp(-dt / cursor_tau_); // exponential approach
    cur_anim_x_ += (tgt_x - cur_anim_x_) * a;
    cur_anim_y_ += (tgt_y - cur_anim_y_) * a;

    const float dx = tgt_x - cur_anim_x_, dy = tgt_y - cur_anim_y_;
    if (dx * dx + dy * dy < 0.35f) {
        cur_anim_x_ = tgt_x; cur_anim_y_ = tgt_y;
        cursor_in_flight_ = false;
    } else {
        cursor_in_flight_ = true;
    }

    // Comet trail only for moves spanning > ~1.5 cells (typing one cell over
    // shouldn't smear), and only when enabled.
    const float mvx = cur_anim_x_ - px0, mvy = cur_anim_y_ - py0;
    if (cursor_trail_ && mvx * mvx + mvy * mvy > (1.5f * cw) * (1.5f * cw)) {
        constexpr int kTrail = 3;
        for (int i = 1; i <= kTrail; ++i) {
            const float t = static_cast<float>(i) / (kTrail + 1);
            const float tx = px0 + (cur_anim_x_ - px0) * t;
            const float ty = py0 + (cur_anim_y_ - py0) * t;
            const float fade = 0.25f + 0.45f * t;      // tail dimmer than head
            const float inset = (1.0f - t) * ch * 0.16f; // tail slightly smaller
            const std::uint8_t r = lerp8(bg.r, col.r, fade);
            const std::uint8_t g = lerp8(bg.g, col.g, fade);
            const std::uint8_t b = lerp8(bg.b, col.b, fade);
            out.push_back(rect_inst(tx + inset, ty + inset, cw - 2 * inset, ch - 2 * inset,
                                    r, g, b, 3));
        }
    }
    out.push_back(rect_inst(cur_anim_x_, cur_anim_y_, cw, ch, col.r, col.g, col.b, crad));
}

DamageRect Renderer::draw(const term::Screen &screen, PixelSize px, bool cursor_on, bool blink_on) {
    // Apply any dynamic-colour edits (OSC 4/104/10/11/12/110-112) the model has
    // recorded since the last frame, then invalidate the row cache if the
    // palette actually moved (every resolved colour may have changed).
    if (screen.palette_epoch() != palette_epoch_seen_) {
        for (std::size_t i = palette_applied_; i < screen.color_edits().size(); ++i) {
            const auto &e = screen.color_edits()[i];
            using T = term::Screen::ColorEdit::Target;
            switch (e.target) {
            case T::index:  e.reset ? palette_.reset_index(e.index) : palette_.set_index(e.index, e.rgb); break;
            case T::fg:     e.reset ? palette_.set_default_fg(Palette{}.default_fg()) : palette_.set_default_fg(e.rgb); break;
            case T::bg:     e.reset ? palette_.set_default_bg(Palette{}.default_bg()) : palette_.set_default_bg(e.rgb); break;
            case T::cursor: palette_.set_cursor_color(e.reset ? std::optional<Rgb>{} : std::optional<Rgb>{e.rgb}); break;
            case T::all:    palette_.reset(); break;
            }
        }
        palette_applied_ = screen.color_edits().size();
        palette_epoch_seen_ = screen.palette_epoch();
        for (auto &rc : rows_) rc.valid = false; // recolour everything
    }

    // The clear happens in the host's begin_pass (with the default bg colour);
    // blending is set in the pipeline. draw() only builds + flushes instances
    // inside the already-open swapchain pass.
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    const int ascent = atlas_.ascent();
    const Extent grid = screen.size();
    const Pos cur = screen.cursor();
    const bool any_selection = screen.has_selection();
    const bool any_search = screen.searching();
    // Either overlay needs absolute row coords + disables the fast epoch path,
    // because neither selection nor search is part of the per-row cell epoch.
    const bool any_overlay = any_selection || any_search;

    const bool cursor_visible =
        cursor_on && screen.cursor_shown() && screen.cursor_visible() && cur.row.get() >= 0 &&
        cur.row.get() < grid.rows && cur.col.get() >= 0 && cur.col.get() < grid.cols;
    const int cur_row = cur.row.get();
    const int cur_col = cur.col.get();

    // Invalidate the whole cache if geometry (grid size / font metrics) moved.
    if (grid.cols != cache_cols_ || grid.rows != cache_rows_ || cw != cache_cw_ ||
        ch != cache_ch_ || ascent != cache_ascent_) {
        rows_.assign(static_cast<std::size_t>(std::max(grid.rows, 0)), RowCache{});
        for (auto &rc : rows_) {
            rc.bg.reserve(static_cast<std::size_t>(std::max(grid.cols, 0)));
            rc.glyphs.reserve(static_cast<std::size_t>(std::max(grid.cols, 0)));
        }
        instances_.reserve(static_cast<std::size_t>(grid.cols) *
                               static_cast<std::size_t>(std::max(grid.rows, 0)) + 2);
        glyphs_.reserve(static_cast<std::size_t>(grid.cols) *
                        static_cast<std::size_t>(std::max(grid.rows, 0)));
        cache_cols_ = grid.cols;
        cache_rows_ = grid.rows;
        cache_cw_ = cw;
        cache_ch_ = ch;
        cache_ascent_ = ascent;
    }

    // A blink-phase flip must rebuild the rows that contain blinking cells (so
    // their glyphs appear/disappear); non-blink rows stay cached, keeping idle
    // blink cheap. Detected before the build loop so build_row can act on it.
    const bool blink_flipped = (blink_on != blink_on_);
    blink_on_ = blink_on;
    blink_flip_ = blink_flipped;

    // A hover-link change (pointer moved onto/off an OSC 8 link) toggles the
    // hover underline. It's a rare, coarse event, so invalidate the whole row
    // cache once rather than track which rows hold the link.
    if (screen.hover_link() != hover_link_) {
        hover_link_ = screen.hover_link();
        for (auto &rc : rows_) rc.valid = false;
    }

    // Rebuild only the rows whose fingerprint changed; the rest keep their
    // shadow instances untouched. We reassemble the flat draw buffers only when
    // something actually changed (or on the very first frame).
    //
    // The fast path is the whole game: the Screen stamps a per-row damage epoch
    // on every write, so for an unchanged row we take its row_version() token
    // as the cache key WITHOUT touching a single cell — no hashing, no read of
    // screen.row(). That collapses an idle/typing frame from O(cells) to
    // O(rows) with a ~2ns-per-row array compare. We fall back to fingerprinting
    // the actual cells only when the epoch is unavailable (scrolled into
    // history, token 0) or a selection is active (selection isn't part of the
    // cell epoch), which are the rare interactive cases.
    bool any_row_dirty = false;
    int dirty_top = INT32_MAX, dirty_bot = -1; // pixel-row damage bounds
    const bool cursor_block =
        screen.cursor_style().shape == term::Screen::CursorShape::block;
    for (int r = 0; r < grid.rows; ++r) {
        const bool row_has_cursor = cursor_visible && r == cur_row;
        const std::int64_t abs_row = any_overlay ? screen.viewport_to_abs(r) : 0;

        std::uint64_t key;
        const std::uint64_t ver = any_overlay ? 0 : screen.row_version(r);
        if (ver != 0) {
            // Fast path: fold the cursor column into the epoch token so a cursor
            // move on this row still invalidates it. Tag the high bit so an
            // epoch value can never collide with a hashed key (which is mixed).
            key = (ver << 1) ^ (row_has_cursor
                                     ? (static_cast<std::uint64_t>(cur_col) + 1) |
                                           (cursor_block ? 0x4000000000000000ULL : 0)
                                     : 0);
            key |= 0x8000000000000000ULL;
        } else {
            // Fallback: fingerprint the row's cells + cursor + selection.
            const auto cells = screen.row(Row{r});
            std::uint64_t h = 0xcbf29ce484222325ULL;
            hash_words(h, cells.data(), static_cast<std::size_t>(grid.cols) * sizeof(term::Cell));
            if (row_has_cursor) hash_val(h, cur_col);
            else                hash_val(h, -1);
            if (any_selection) {
                for (int c = 0; c < grid.cols; ++c) {
                    if (screen.is_selected(abs_row, c)) hash_val(h, c);
                }
                // The rounded selection corners depend on the row ABOVE and
                // BELOW (a corner rounds only when its vertical neighbour is
                // unselected). Fold both neighbours' selection edges into the
                // key so growing/shrinking a selection vertically rebuilds the
                // adjacent row and its corners never go stale.
                for (int c = 0; c < grid.cols; ++c) {
                    if (screen.is_selected(abs_row - 1, c)) hash_val(h, 0x10000 | c);
                    if (screen.is_selected(abs_row + 1, c)) hash_val(h, 0x20000 | c);
                }
            }
            if (any_search) {
                // Fold match + current-match membership so highlighting (and a
                // current-match change from n/N) repaints the affected rows.
                for (int c = 0; c < grid.cols; ++c) {
                    if (screen.is_search_match(abs_row, c)) hash_val(h, 0x40000 | c);
                    if (screen.is_current_search_match(abs_row, c)) hash_val(h, 0x80000 | c);
                }
            }
            key = mix(h) & 0x7fffffffffffffffULL; // clear tag bit: distinct from epoch keys
        }

        const term::Screen::LineAttr la = screen.line_attr(r);
        // Fold the line attribute into the key so a DECDWL/DECDHL change on this
        // row forces a rebuild. Mask to the low 32 bits so the epoch/hash tag
        // bits at the top are preserved.
        key ^= (static_cast<std::uint64_t>(la) * 0x9E3779B1u) & 0x00000000FFFFFFFFULL;

        const bool row_rebuilt = build_row(screen, r, key, row_has_cursor, cursor_block, cur_col,
                                            abs_row, any_selection, any_search, blink_on, la);
        if (row_rebuilt) {
            any_row_dirty = true;
            dirty_top = std::min(dirty_top, r * cache_ch_);
            dirty_bot = std::max(dirty_bot, (r + 1) * cache_ch_);
        }
    }


    // Reassemble the flat draw buffers only when a row changed. Clean frames
    // (only reached when the host detected damage elsewhere) reuse the buffers
    // already staged — no per-row copies at all.
    if (any_row_dirty || instances_.empty()) {
        instances_.clear();
        glyphs_.clear();
        for (int r = 0; r < grid.rows; ++r) {
            const RowCache &rc = rows_[static_cast<std::size_t>(r)];
            instances_.insert(instances_.end(), rc.bg.begin(), rc.bg.end());
            glyphs_.insert(glyphs_.end(), rc.glyphs.begin(), rc.glyphs.end());
        }
        // The block cursor inverts its cell inside the row cache above (glyph
        // shown in the bg colour). The moving block itself is drawn as an
        // animated overlay below, unified with bar/underline — so it glides.
        // Remember where the cache-assembled buffer ends so the animated caret
        // can be re-appended fresh each frame without duplicating.
        base_instance_n_ = instances_.size();
    }

    // Animated caret: glides to its target cell. Appended AFTER the cached
    // assembly every frame (trimmed back to the base first) so it moves even on
    // otherwise-clean frames. All three shapes ride the same glide; block draws
    // the full cell, bar/underline inset to a thin edge. Sets cursor_in_flight_.
    cursor_in_flight_ = false;
    const auto cstyle = screen.cursor_style().shape;
    if (base_instance_n_ <= instances_.size())
        instances_.resize(base_instance_n_); // drop last frame's caret
    if (cursor_visible) {
        const Rgb cc = palette_.cursor_color();
        const Rgb bg = palette_.default_bg();
        const float fw = static_cast<float>(cw), fh = static_cast<float>(ch);
        std::vector<Instance> caret;
        animate_cursor(static_cast<float>(cur_col * cw), static_cast<float>(cur_row * ch), fw, fh,
                       cc, bg, caret);
        for (const Instance &ci : caret) {
            switch (cstyle) {
            case term::Screen::CursorShape::bar: {
                const float t = std::max(1.0f, fw * 0.15f);
                instances_.push_back(rect_inst(ci.x, ci.y, t, ci.h, ci.r, ci.g, ci.b, ci.radius));
                break;
            }
            case term::Screen::CursorShape::underline: {
                const float t = std::max(1.0f, fh * 0.12f);
                instances_.push_back(
                    rect_inst(ci.x, ci.y + ci.h - t, ci.w, t, ci.r, ci.g, ci.b, ci.radius));
                break;
            }
            case term::Screen::CursorShape::block:
            default:
                instances_.push_back(rect_inst(ci.x, ci.y, ci.w, ci.h, ci.r, ci.g, ci.b, ci.radius));
                break;
            }
        }
    }

    // Visual-bell flash: a fading fg-tinted full-screen overlay for ~150ms.
    // Appended after the caret (also every frame while active) so it fades even
    // on otherwise-clean frames; animating() keeps the host presenting.
    if (bell_until_us_ != 0) {
        const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
        const std::int64_t left = bell_until_us_ - now;
        if (left <= 0) {
            bell_until_us_ = 0;
        } else {
            // Tint toward fg, strongest at trigger, fading to nothing. Since the
            // pipeline has no per-instance alpha, lerp bg->fg by the fade factor.
            const float k = 0.35f * (static_cast<float>(left) / static_cast<float>(kBellFlashUs));
            const Rgb bg = palette_.default_bg();
            const Rgb fg = palette_.default_fg();
            auto mix = [k](std::uint8_t a, std::uint8_t b) {
                return static_cast<std::uint8_t>(a + (static_cast<float>(b) - a) * k + 0.5f);
            };
            instances_.push_back(rect_inst(0, 0, static_cast<float>(px.w), static_cast<float>(px.h),
                                           mix(bg.r, fg.r), mix(bg.g, fg.g), mix(bg.b, fg.b), 0));
        }
    }

    // Command minimap: a live map of the session on the right edge. Instead of
    // a dumb scrollbar, each OSC-133 command is a coloured segment positioned by
    // where it sits in scrollback — green (ok), red (failed), amber (running) —
    // and the viewport thumb rides on top. It's a scrollbar that's also a visual
    // index of your work. Shown whenever there's history or recorded commands.
    {
        const std::int32_t hist = screen.history_rows();
        const std::int64_t total_rows = static_cast<std::int64_t>(hist) + grid.rows;
        const auto &marks = screen.scroll_marks();
        const std::int32_t off = screen.scroll_offset();
        if (hist > 0 || !marks.empty()) {
            const float track_h = static_cast<float>(px.h);
            // Is the pointer over the rail right now? If so the whole minimap
            // gently expands + brightens (an editor-style "come look at me").
            const bool rail_hot = screen.rail_hover_row() >= 0;
            const float railw = rail_hot ? 11.0f : 7.0f;     // wider than the old 5px
            const float margin = 4.0f;
            const float x = static_cast<float>(px.w) - railw - margin;
            const std::uint8_t rr = static_cast<std::uint8_t>(railw / 2.0f);
            // Rounded track: a soft dark channel the segments sit inside, so the
            // minimap reads as a distinct UI element and not stray pixels.
            {
                const Rgb bg = palette_.default_bg();
                // subtle lighter groove first (halo), then the darker channel
                const Rgb fg = palette_.default_fg();
                instances_.push_back(rect_round_inst(x - 1.0f, -1.0f, railw + 2.0f, track_h + 2.0f,
                                                     fg.r, fg.g, fg.b, rr + 1, 0,
                                                     rail_hot ? 28 : 16));
                instances_.push_back(rect_round_inst(x, 0.0f, railw, track_h,
                                                     bg.r, bg.g, bg.b, rr, 0, 150));
            }
            // Command segments. Map absolute-row span -> pixel span. Row 0 is the
            // oldest history line at the TOP; total_rows-1 is the newest.
            const auto row_to_y = [&](std::int64_t row) -> float {
                if (total_rows <= 1) return 0.0f;
                return static_cast<float>(row) / static_cast<float>(total_rows) * track_h;
            };
            const float segw = railw - 2.0f;
            const float segx = x + 1.0f;
            const std::uint8_t segr = static_cast<std::uint8_t>(segw / 2.0f);
            const std::int64_t foc0 = screen.focused_span_start();
            const std::int64_t foc1 = screen.focused_span_end();
            for (const auto &m : marks) {
                const float y0 = row_to_y(m.start);
                const float y1 = std::max(y0 + 4.0f, row_to_y(m.end));
                const bool hov = screen.rail_hover_row() >= m.start &&
                                 screen.rail_hover_row() < m.end;
                const bool focused = foc0 >= 0 && m.start < foc1 && m.end > foc0;
                Rgb c;
                std::uint8_t a = 210;
                switch (m.status) {
                case term::Screen::MarkStatus::ok: c = rgb(80, 200, 130); break;
                case term::Screen::MarkStatus::failed: c = rgb(235, 90, 90); break;
                case term::Screen::MarkStatus::running:
                default:
                    c = rgb(240, 190, 70); // amber; a running command in flight
                    a = 235;
                    break;
                }
                // Focused (jumped-to) command: a bright white outline halo so
                // it's unmistakable which command you're viewing.
                if (focused) {
                    const Rgb w = rgb(245, 247, 255);
                    instances_.push_back(rect_round_inst(segx - 3.0f, y0 - 2.0f, segw + 6.0f,
                                                         (y1 - y0) + 4.0f, w.r, w.g, w.b,
                                                         segr + 3, 0, 200));
                }
                // Hovered = full alpha + slightly wider so it reads as the
                // command under the pointer. A gentle halo only on hover.
                if (hov) {
                    instances_.push_back(rect_round_inst(segx - 2.0f, y0 - 1.0f, segw + 4.0f,
                                                         (y1 - y0) + 2.0f, c.r, c.g, c.b,
                                                         segr + 2, 0, 90));
                }
                const float sw = (hov || focused) ? segw + 3.0f : segw;
                const float sx = (hov || focused) ? segx - 1.5f : segx;
                if (hov || focused) a = 255;
                instances_.push_back(rect_round_inst(sx, y0, sw, y1 - y0, c.r, c.g, c.b,
                                                     (hov || focused) ? segr + 1 : segr, 0, a));
            }
            // Viewport indicator: a BRACKET framing the current view — thin caps
            // at the top & bottom of the viewport plus faint side rails — so it
            // shows where you are WITHOUT covering the command segments beneath.
            if (total_rows > grid.rows) {
                const float view_frac = static_cast<float>(grid.rows) /
                                        static_cast<float>(total_rows);
                const float thumb_h = std::max(20.0f, track_h * view_frac);
                const std::int64_t top_row = static_cast<std::int64_t>(hist) - off;
                const float frac = static_cast<float>(top_row) /
                                   static_cast<float>(std::max<std::int64_t>(1, total_rows - grid.rows));
                const float ty = frac * (track_h - thumb_h);
                const Rgb fg = palette_.default_fg();
                const std::uint8_t edge = off != 0 ? 235 : 120; // brighter while scrolled
                const std::uint8_t side = off != 0 ? 70 : 30;
                const float capw = railw + 2.0f;
                const float capx = x - 1.0f;
                // Top & bottom caps (rounded) mark the viewport extent.
                instances_.push_back(rect_round_inst(capx, ty, capw, 2.5f,
                                                     fg.r, fg.g, fg.b, 1, 0, edge));
                instances_.push_back(rect_round_inst(capx, ty + thumb_h - 2.5f, capw, 2.5f,
                                                     fg.r, fg.g, fg.b, 1, 0, edge));
                // Faint side rails connecting the caps (a subtle bracket).
                instances_.push_back(rect_inst(capx, ty, 1.5f, thumb_h, fg.r, fg.g, fg.b, 0, side));
                instances_.push_back(rect_inst(capx + capw - 1.5f, ty, 1.5f, thumb_h,
                                               fg.r, fg.g, fg.b, 0, side));
            }
        }
    }

    // Focused-block gutter: when the command you JUMPED to (via the flyout,
    // rail click, or keyboard block-nav) is on-screen, paint a bright bar down
    // the left edge over its rows — an unmistakable "you are here" marker tying
    // the click to the terminal. viewport_to_abs tracks it as you scroll.
    {
        const std::int64_t f0 = screen.focused_span_start();
        const std::int64_t f1 = screen.focused_span_end();
        if (f0 >= 0 && f1 > f0) {
            const Rgb acc = rgb(120, 170, 255);
            for (int r = 0; r < grid.rows; ++r) {
                const std::int64_t ar = screen.viewport_to_abs(r);
                if (ar < f0 || ar >= f1) continue;
                const float ry = static_cast<float>(r * ch);
                instances_.push_back(rect_round_inst(1.0f, ry + 1.0f, 3.0f,
                                                     static_cast<float>(ch) - 2.0f,
                                                     acc.r, acc.g, acc.b, 1, 0, 235));
            }
        }
    }

    const bool has_images = !screen.graphics().placements().empty();
    // Placeholder cells reference transmitted images that may have no placement,
    // so draw them whenever the graphics store holds any image.
    const bool has_any_image = has_images || screen.graphics().has_images();

    if (!any_row_dirty && !has_any_image && !animating() && redraw_from_cache(px)) {
        // Nothing changed: the GPU buffers already hold this exact frame. We
        // replayed the recorded draws with zero uploads / fences / memcpy.
        return {}; // empty damage: the host can skip the commit entirely
    }

    // Dirty frame: re-stage both batches (this also re-records the draw calls).
    last_draw_n_ = 0;
    flush(instances_, px);
    flush(glyphs_, px);

    // Inline images (kitty graphics) draw over the glyph layer.
    if (has_images) draw_images(screen, px);
    if (has_any_image) draw_placeholders(screen, px);

    // IME composition string, overlaid at the cursor on top of everything.
    if (!screen.preedit().empty()) draw_preedit(screen, px);

    // Report damage. We CLEAR + redraw the ENTIRE frame into the back buffer
    // every dirty frame, so the whole surface is authoritative. Reporting only
    // the rebuilt-row sub-rect made the compositor keep the rest from its last
    // composite of THIS surface — which is stale for any row whose content
    // changed without its render key changing (the "some rows get missed" bug).
    // A full-surface damage is correct and, since the draw is already full,
    // effectively free. (The old dirty_top/dirty_bot bounds are kept computed
    // above for potential future buffer-age-aware partial damage.)
    (void)dirty_top;
    (void)dirty_bot;
    return DamageRect::full(px);
}

// Draw the IME composition string inline at the cursor: a background box, the
// composing glyphs in the default foreground, and a full underline (the
// universal "this text isn't committed yet" cue). A separate pass so it never
// pollutes the row cache and updates independently of the grid.
void Renderer::draw_preedit(const term::Screen &screen, PixelSize px) {
    const std::string_view s = screen.preedit();
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    const int ascent = atlas_.ascent();
    const Pos cur = screen.cursor();
    const Extent grid = screen.size();

    // Decode UTF-8 into codepoints (minimal, tolerant of malformed input).
    std::vector<char32_t> cps;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (int k = 1; k < len && i + static_cast<std::size_t>(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        cps.push_back(cp);
        i += static_cast<std::size_t>(len);
    }
    if (cps.empty()) return;

    std::vector<Instance> bg, glyphs;
    const Rgb fg = palette_.default_fg();
    const Rgb boxbg = palette_.default_bg();
    int col = cur.col.get();
    const int row = cur.row.get();
    const float ry = static_cast<float>(row * ch);
    // Radius scaled to the cell, consistent with the selection region + cursor.
    const std::uint8_t prad = static_cast<std::uint8_t>(std::clamp(std::min(cw, ch) * 4 / 10, 2, 8));
    const float uthick = std::max(1.0f, static_cast<float>(ch) / 12.0f);
    // First pass: how many cells the composition actually occupies (so we can
    // round only the OUTER corners of the whole run, not each cell).
    struct Seg { float cx; int w; };
    std::vector<Seg> segs;
    {
        int probe = col;
        for (char32_t cp : cps) {
            const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                              (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                              (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1FAFF);
            const int w = wide ? 2 : 1;
            if (probe + w > grid.cols) break;
            segs.push_back({static_cast<float>(probe * cw), w});
            probe += w;
        }
    }

    for (std::size_t si = 0; si < segs.size(); ++si) {
        const float cx = segs[si].cx;
        const int w = segs[si].w;
        const float bw = static_cast<float>(cw * w);
        // Outer-corner mask: round the run's left edge on the first cell and its
        // right edge on the last cell, so the box is a single rounded chip.
        std::uint8_t bmask = 0, umask = 0;
        if (si == 0) { bmask |= kCornerTL | kCornerBL; umask |= kCornerBL; }
        if (si + 1 == segs.size()) { bmask |= kCornerTR | kCornerBR; umask |= kCornerBR; }
        // Background box behind the composing cells.
        if (bmask == 0)
            bg.push_back(rect_inst(cx, ry, bw, static_cast<float>(ch), boxbg.r, boxbg.g, boxbg.b, 0));
        else
            bg.push_back(rect_round_inst(cx, ry, bw, static_cast<float>(ch), boxbg.r, boxbg.g,
                                         boxbg.b, prad, bmask));
        // Underline across the cell width, with rounded caps at the run ends.
        const float uy = ry + static_cast<float>(ch) - uthick;
        const std::uint8_t urad = static_cast<std::uint8_t>(std::max(1.0f, uthick));
        if (umask == 0)
            bg.push_back(rect_inst(cx, uy, bw, uthick, fg.r, fg.g, fg.b, 0));
        else
            bg.push_back(rect_round_inst(cx, uy, bw, uthick, fg.r, fg.g, fg.b, urad, umask));
        // The glyph.
        const char32_t cp = cps[si];
        if (const GlyphInfo *gi = atlas_.glyph(cp); gi && gi->width && gi->height) {
            const float gx = cx + static_cast<float>(gi->bearing_x);
            const float gy = static_cast<float>(row * ch + ascent - gi->bearing_y);
            glyphs.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                      static_cast<float>(gi->height), gi->u0, gi->v0, gi->u1,
                                      gi->v1, fg.r, fg.g, fg.b, 255,
                                      /*is_glyph=*/static_cast<std::uint8_t>(gi->is_color ? 2 : 1),
                                      0, 0, 0});
        }
    }

    // Draw the box+underline layer, then the glyphs, via the normal pipeline.
    std::vector<Instance> all;
    all.reserve(bg.size() + glyphs.size());
    all.insert(all.end(), bg.begin(), bg.end());
    all.insert(all.end(), glyphs.begin(), glyphs.end());
    flush(all, px);
}

void Renderer::draw_cells(const term::Cell *cells, int cols, int rows, PixelSize px, int ox,
                          int oy, float bg_alpha, const std::uint8_t *alpha) {
    if (!cells || cols <= 0 || rows <= 0) return;
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    const int ascent = atlas_.ascent();
    const auto ba = static_cast<std::uint8_t>(
        std::clamp(bg_alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    // The overlay's translucency comes from bg_alpha alone (per-instance), not
    // window opacity — a settings pane should read the same whether or not the
    // terminal itself is transparent. Force uOpacity=1 for these flushes and
    // restore after.
    const float saved_opacity = opacity_;
    opacity_ = 1.0f;

    // Resolve a cell Color to concrete RGB via the palette (handles TrueColor,
    // Indexed and Default uniformly).
    const auto resolve = [&](const term::Color &c, bool is_fg) -> Rgb {
        return palette_.resolve(c, is_fg);
    };

    std::vector<Instance> bg, glyphs;
    bg.reserve(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    glyphs.reserve(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));

    for (int r = 0; r < rows; ++r) {
        const float y = static_cast<float>(oy + r * ch);
        for (int c = 0; c < cols; ++c) {
            const term::Cell &cell = cells[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                                           static_cast<std::size_t>(c)];
            if (cell.width == 0) continue; // wide-glyph spacer
            const float x = static_cast<float>(ox + c * cw);
            const int span = cell.width == 2 ? 2 : 1;
            // Per-cell background alpha: the optional plane wins over bg_alpha,
            // so an overlay can be a faint scrim outside a near-opaque panel.
            const std::uint8_t cell_a =
                alpha ? alpha[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                              static_cast<std::size_t>(c)]
                      : ba;
            // Background rect for every cell, scaled so an overlay pane reads as
            // frosted glass over the terminal (glyphs stay opaque). alpha==0
            // draws no rect at all (a true see-through hole).
            if (cell_a != 0) {
                const Rgb bgc = resolve(cell.pen.bg, /*is_fg=*/false);
                bg.push_back(rect_inst(x, y, static_cast<float>(cw * span),
                                       static_cast<float>(ch), bgc.r, bgc.g, bgc.b, 0, cell_a));
            }
            const char32_t cp = cell.cp;
            if (cp == 0 || cp == U' ') continue;
            const Rgb fgc = resolve(cell.pen.fg, /*is_fg=*/true);
            if (const GlyphInfo *gi = atlas_.glyph(cp); gi && gi->width && gi->height) {
                const float gx = x + static_cast<float>(gi->bearing_x);
                const float gy = y + static_cast<float>(ascent - gi->bearing_y);
                glyphs.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                          static_cast<float>(gi->height), gi->u0, gi->v0, gi->u1,
                                          gi->v1, fgc.r, fgc.g, fgc.b, 255,
                                          static_cast<std::uint8_t>(gi->is_color ? 2 : 1), 0, 0, 0});
            }
        }
    }

    flush(bg, px);
    flush(glyphs, px);
    opacity_ = saved_opacity;
}

} // namespace toe::gfx
