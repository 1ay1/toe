// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/gfx/renderer.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <array>

#include <epoxy/gl.h>

namespace gvte::gfx {

namespace {

// Vertex shader: a unit quad [0,1]^2, offset+scaled by per-instance pixel rect,
// projected to clip space via a screen-size uniform. It also forwards the
// fragment's local position within the rect (in pixels, centered) so the
// fragment stage can evaluate a signed-distance function for rounded corners
// — the GPUI technique: describe primitives on the CPU, shape them on the GPU.
constexpr const char *kVert = R"(#version 330 core
layout(location = 0) in vec2 aCorner;   // unit quad corner
layout(location = 1) in vec4 aRect;      // x,y,w,h in pixels
layout(location = 2) in vec4 aUV;        // u0,v0,u1,v1
layout(location = 3) in vec3 aColor;
layout(location = 4) in float aIsGlyph;  // 1 glyph, 0 rect
layout(location = 5) in float aRadius;   // corner radius in px (rects only)

uniform vec2 uScreen;                    // viewport size in pixels

out vec2 vUV;
out vec3 vColor;
out float vIsGlyph;
out vec2 vLocal;      // position within the rect, centered, in px
out vec2 vHalf;       // half-extent of the rect, in px
out float vRadius;

void main() {
    vec2 px = aRect.xy + aCorner * aRect.zw;
    vec2 ndc = vec2((px.x / uScreen.x) * 2.0 - 1.0,
                    1.0 - (px.y / uScreen.y) * 2.0);  // y-down pixels -> y-up ndc
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = mix(aUV.xy, aUV.zw, aCorner);
    vColor = aColor;
    vIsGlyph = aIsGlyph;
    vHalf = aRect.zw * 0.5;
    vLocal = (aCorner - 0.5) * aRect.zw;  // [-half, +half]
    vRadius = aRadius;
}
)";

// Fragment shader: glyphs sample the R8 atlas as coverage; rects are filled
// with an anti-aliased rounded-box signed-distance function (radius 0 gives a
// crisp axis-aligned rectangle, so ordinary backgrounds cost nothing extra).
constexpr const char *kFrag = R"(#version 330 core
in vec2 vUV;
in vec3 vColor;
in float vIsGlyph;
in vec2 vLocal;
in vec2 vHalf;
in float vRadius;

uniform sampler2D uAtlas;

out vec4 FragColor;

// Signed distance to a rounded box centered at the origin.
float sd_round_box(vec2 p, vec2 half_ext, float r) {
    vec2 q = abs(p) - half_ext + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (vIsGlyph > 0.5) {
        float a = texture(uAtlas, vUV).r;
        FragColor = vec4(vColor, a);
    } else {
        if (vRadius > 0.0) {
            float d = sd_round_box(vLocal, vHalf, vRadius);
            float a = 1.0 - smoothstep(-0.75, 0.75, d);
            FragColor = vec4(vColor, a);
        } else {
            // Sharp rect: full coverage, no edge softening (avoids seams
            // between tiled background cells).
            FragColor = vec4(vColor, 1.0);
        }
    }
}
)";

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

// Convert a float atlas coord in [0,1] to a normalized u16.
inline std::uint16_t unorm16(float f) noexcept {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return static_cast<std::uint16_t>(f * 65535.0f + 0.5f);
}

} // namespace

Result<Renderer> Renderer::create(FontAtlas &&atlas) {
    auto prog = Program::build(kVert, kFrag);
    if (!prog) {
        return std::unexpected(prog.error());
    }
    Renderer r{std::move(atlas), std::move(*prog)};
    r.u_screen_ = r.prog_.uniform("uScreen");
    r.u_atlas_ = r.prog_.uniform("uAtlas");
    r.ensure_buffers();
    return r;
}

Renderer::~Renderer() {
    for (auto *&f : fences_) {
        if (f) { glDeleteSync(static_cast<GLsync>(f)); f = nullptr; }
    }
    if (inst_map_ && inst_vbo_) {
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        inst_map_ = nullptr;
    }
    if (inst_vbo_) glDeleteBuffers(1, &inst_vbo_);
    if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

Renderer::Renderer(Renderer &&o) noexcept
    : atlas_{std::move(o.atlas_)}, palette_{o.palette_}, prog_{std::move(o.prog_)},
      u_screen_{o.u_screen_}, u_atlas_{o.u_atlas_},
      vao_{std::exchange(o.vao_, 0)}, quad_vbo_{std::exchange(o.quad_vbo_, 0)},
      inst_vbo_{std::exchange(o.inst_vbo_, 0)}, inst_bytes_capacity_{o.inst_bytes_capacity_},
      persistent_{o.persistent_}, inst_map_{std::exchange(o.inst_map_, nullptr)},
      inst_region_bytes_{o.inst_region_bytes_}, ring_slot_{o.ring_slot_},
      instances_{std::move(o.instances_)} {
    for (int i = 0; i < kRing; ++i) fences_[i] = std::exchange(o.fences_[i], nullptr);
    o.persistent_ = false;
}

Renderer &Renderer::operator=(Renderer &&o) noexcept {
    if (this != &o) {
        for (auto *&f : fences_) {
            if (f) { glDeleteSync(static_cast<GLsync>(f)); f = nullptr; }
        }
        if (inst_map_ && inst_vbo_) {
            glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            inst_map_ = nullptr;
        }
        if (inst_vbo_) glDeleteBuffers(1, &inst_vbo_);
        if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        atlas_ = std::move(o.atlas_);
        palette_ = o.palette_;
        prog_ = std::move(o.prog_);
        vao_ = std::exchange(o.vao_, 0);
        quad_vbo_ = std::exchange(o.quad_vbo_, 0);
        inst_vbo_ = std::exchange(o.inst_vbo_, 0);
        inst_bytes_capacity_ = o.inst_bytes_capacity_;
        persistent_ = std::exchange(o.persistent_, false);
        inst_map_ = std::exchange(o.inst_map_, nullptr);
        inst_region_bytes_ = o.inst_region_bytes_;
        ring_slot_ = o.ring_slot_;
        for (int i = 0; i < kRing; ++i) fences_[i] = std::exchange(o.fences_[i], nullptr);
        instances_ = std::move(o.instances_);
    }
    return *this;
}

Extent Renderer::cells_for(PixelSize px) const noexcept {
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    return Extent{cw > 0 ? px.w / cw : 1, ch > 0 ? px.h / ch : 1};
}

void Renderer::setup_instance_attribs() {
    const GLsizei stride = sizeof(Instance);
    auto off = [](std::size_t n) { return reinterpret_cast<void *>(n); };
    // Packed layout: rect 4xf32 @0, uv 4xu16(norm) @16, color 4xu8(norm) @24,
    // is_glyph u8(norm) @28, radius u8(raw) @29.
    // aRect (loc1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, off(0));
    glVertexAttribDivisor(1, 1);
    // aUV (loc2) — u16 normalized -> [0,1]
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_SHORT, GL_TRUE, stride, off(16));
    glVertexAttribDivisor(2, 1);
    // aColor (loc3) — 3 of the 4 packed u8, normalized -> [0,1]
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride, off(24));
    glVertexAttribDivisor(3, 1);
    // aIsGlyph (loc4) — u8 normalized (0/255 -> 0.0/1.0)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride, off(28));
    glVertexAttribDivisor(4, 1);
    // aRadius (loc5) — u8 raw px value as float (unnormalized)
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, off(29));
    glVertexAttribDivisor(5, 1);
}

void Renderer::ensure_buffers() {
    static constexpr float kQuad[] = {0, 0, 1, 0, 0, 1, 1, 1}; // triangle strip

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glGenBuffers(1, &inst_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);

    // Fastest streaming path: on GL 4.4+ (buffer_storage), allocate one large
    // immutable, persistently+coherently mapped ring and write instances
    // straight into GPU-visible memory — no glBufferSubData copy, no per-frame
    // orphan+realloc. We fence each ring region so the CPU never scribbles over
    // instances the GPU is still reading.
    persistent_ = epoxy_gl_version() >= 44 || epoxy_has_gl_extension("GL_ARB_buffer_storage");
    if (std::getenv("GVTE_NO_PERSISTENT")) persistent_ = false;
    if (persistent_) {
        // Worst case is ~2 instances/cell (bg + glyph); size a region to cover
        // a 4K screen with slack so a frame never overflows one region.
        inst_region_bytes_ = std::size_t{160000} * sizeof(Instance); // ~5 MiB/region
        const std::size_t total = inst_region_bytes_ * kRing;
        const GLbitfield flags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(total), nullptr, flags);
        inst_map_ = static_cast<unsigned char *>(
            glMapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(total), flags));
        if (!inst_map_) {
            // Mapping failed — fall back to the classic orphaning path.
            persistent_ = false;
        } else {
            inst_bytes_capacity_ = total;
        }
    }

    setup_instance_attribs();

    glBindVertexArray(0);
}

void Renderer::flush(std::span<const Instance> insts, PixelSize px) {
    std::size_t count = insts.size();
    if (count == 0) return;

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    const std::size_t bytes = count * sizeof(Instance);

    if (persistent_ && bytes <= inst_region_bytes_) {
        // Cycle to the next ring region. Wait on its fence (from kRing frames
        // ago) so we never overwrite instances the GPU is still consuming, then
        // memcpy straight into the coherent mapping — no glBufferSubData copy.
        const int slot = ring_slot_;
        ring_slot_ = (ring_slot_ + 1) % kRing;
        if (auto *f = static_cast<GLsync>(fences_[slot])) {
            glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(f);
            fences_[slot] = nullptr;
        }
        const std::size_t base = static_cast<std::size_t>(slot) * inst_region_bytes_;
        std::memcpy(inst_map_ + base, insts.data(), bytes);

        prog_.use();
        glUniform2f(u_screen_, static_cast<float>(px.w), static_cast<float>(px.h));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas_.texture());
        glUniform1i(u_atlas_, 0);

        // Point the instanced attributes at this region's slice.
        const GLuint first = static_cast<GLuint>(base / sizeof(Instance));
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4,
                                          static_cast<GLsizei>(count), first);
        fences_[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glBindVertexArray(0);
        return;
    }

    // Classic orphaning path (GL 3.3). If we somehow get here with a persistent
    // immutable buffer and an over-large frame, clamp to the region (can't
    // realloc immutable storage) rather than issue an illegal glBufferData.
    if (persistent_) {
        count = inst_region_bytes_ / sizeof(Instance);
        const std::size_t clamped = count * sizeof(Instance);
        std::memcpy(inst_map_, insts.data(), clamped);
        prog_.use();
        glUniform2f(u_screen_, static_cast<float>(px.w), static_cast<float>(px.h));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas_.texture());
        glUniform1i(u_atlas_, 0);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
        glBindVertexArray(0);
        return;
    }

    // Classic orphaning path (GL 3.3).
    if (bytes > inst_bytes_capacity_) {
        inst_bytes_capacity_ = bytes + bytes / 2;
    }
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(inst_bytes_capacity_), nullptr,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), insts.data());

    prog_.use();
    glUniform2f(u_screen_, static_cast<float>(px.w), static_cast<float>(px.h));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_.texture());
    glUniform1i(u_atlas_, 0);

    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    glBindVertexArray(0);
}

void Renderer::draw(const term::Screen &screen, PixelSize px, bool cursor_on) {
    const Rgb bgc = palette_.default_bg();
    glClearColor(fr(bgc.r), fr(bgc.g), fr(bgc.b), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    const int ascent = atlas_.ascent();
    const Extent grid = screen.size();
    const Pos cur = screen.cursor();
    const bool any_selection = screen.has_selection();

    instances_.clear();
    glyphs_.clear();
    const std::size_t cells_total =
        static_cast<std::size_t>(grid.cols) * static_cast<std::size_t>(grid.rows);
    // One-time growth to the worst case so per-frame push_backs never realloc:
    // bg rects (+cursor) in instances_, one glyph/cell in glyphs_.
    instances_.reserve(cells_total + 2);
    glyphs_.reserve(cells_total);

    // Cursor geometry (drawn between bg rects and glyphs so glyphs sit on top).
    const bool cursor_visible =
        cursor_on && screen.cursor_shown() && screen.cursor_visible() && cur.row.get() >= 0 &&
        cur.row.get() < grid.rows && cur.col.get() >= 0 && cur.col.get() < grid.cols;
    const int cur_row = cur.row.get();
    const int cur_col = cur.col.get();

    // Single fused walk: emit background rects / selection into instances_ and
    // glyphs into glyphs_ in one pass over the grid. This halves the grid
    // iteration and the (out-of-line) screen.row() calls versus two passes, and
    // touches each cell exactly once for better cache behavior.
    for (int r = 0; r < grid.rows; ++r) {
        const auto cells = screen.row(Row{r});
        const float ry = static_cast<float>(r * ch);
        const int base_gy = r * ch + ascent;
        const std::int64_t abs_row = any_selection ? screen.viewport_to_abs(r) : 0;
        const bool row_has_cursor = cursor_visible && r == cur_row;
        for (int c = 0; c < grid.cols; ++c) {
            const auto &cell = cells[static_cast<std::size_t>(c)];
            const bool selected = any_selection && screen.is_selected(abs_row, c);
            const bool reverse = term::has(cell.pen.attr, term::Attr::Reverse);
            const bool on_cursor = row_has_cursor && c == cur_col;

            // --- background rect (selection > reverse > explicit bg) ---
            if (selected) {
                instances_.push_back(rect_inst(static_cast<float>(c * cw), ry,
                                               static_cast<float>(cw), static_cast<float>(ch),
                                               66, 84, 112, /*radius=*/0));
            } else if (reverse || !std::holds_alternative<term::DefaultColor>(cell.pen.bg)) {
                const term::Color bg = reverse ? cell.pen.fg : cell.pen.bg;
                const Rgb col = palette_.resolve(bg, /*is_fg=*/reverse);
                instances_.push_back(rect_inst(static_cast<float>(c * cw), ry,
                                               static_cast<float>(cw), static_cast<float>(ch),
                                               col.r, col.g, col.b, /*radius=*/0));
            }

            // --- glyph ---
            const char32_t cp = cell.cp;
            if (cp == U' ' || cp == 0 || cell.spacer()) continue; // blank / wide-spacer
            const GlyphInfo *gi = atlas_.glyph(cp);
            if (!gi || gi->width == 0 || gi->height == 0) continue;
            const Rgb col = on_cursor
                                ? palette_.resolve(cell.pen.bg, /*is_fg=*/false)
                                : palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);
            const float gx = static_cast<float>(c * cw + gi->bearing_x);
            const float gy = static_cast<float>(base_gy - gi->bearing_y);
            glyphs_.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                       static_cast<float>(gi->height),
                                       unorm16(gi->u0), unorm16(gi->v0), unorm16(gi->u1),
                                       unorm16(gi->v1), col.r, col.g, col.b, 255,
                                       /*is_glyph=*/1, 0, 0, 0});
        }
    }

    // Cursor rect on top of backgrounds, beneath glyphs.
    if (cursor_visible) {
        const Rgb cc = palette_.default_fg();
        instances_.push_back(rect_inst(static_cast<float>(cur_col * cw),
                                       static_cast<float>(cur_row * ch),
                                       static_cast<float>(cw), static_cast<float>(ch),
                                       cc.r, cc.g, cc.b, /*radius=*/2));
    }

    // Draw backgrounds+cursor first, then glyphs on top — two instanced draws
    // straight from their own buffers, avoiding a per-frame concat memcpy.
    flush(instances_, px);
    flush(glyphs_, px);
}

} // namespace gvte::gfx
