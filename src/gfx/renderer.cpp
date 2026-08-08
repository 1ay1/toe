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
struct CellRect { float x, y, w, h; };
inline int cell_fills(char32_t cp, CellRect out[5]) noexcept {
    // --- block elements (solid fractions of the cell) ---
    auto one = [&](float x, float y, float w, float h) { out[0] = {x, y, w, h}; return 1; };
    switch (cp) {
    case U'\u2588': return one(0, 0, 1, 1);              // full block
    case U'\u2580': return one(0, 0, 1, .5f);            // upper half
    case U'\u2584': return one(0, .5f, 1, .5f);          // lower half
    case U'\u258C': return one(0, 0, .5f, 1);            // left half
    case U'\u2590': return one(.5f, 0, .5f, 1);          // right half
    case U'\u2581': return one(0, 7.f/8, 1, 1.f/8);
    case U'\u2582': return one(0, 6.f/8, 1, 2.f/8);
    case U'\u2583': return one(0, 5.f/8, 1, 3.f/8);
    case U'\u2585': return one(0, 3.f/8, 1, 5.f/8);
    case U'\u2586': return one(0, 2.f/8, 1, 6.f/8);
    case U'\u2587': return one(0, 1.f/8, 1, 7.f/8);
    case U'\u2589': return one(0, 0, 7.f/8, 1);
    case U'\u258A': return one(0, 0, 6.f/8, 1);
    case U'\u258B': return one(0, 0, 5.f/8, 1);
    case U'\u258D': return one(0, 0, 3.f/8, 1);
    case U'\u258E': return one(0, 0, 2.f/8, 1);
    case U'\u258F': return one(0, 0, 1.f/8, 1);
    default: break;
    }

    // --- box-drawing lines (U+2500–257F) built from centered stubs ---
    // Stroke widths as a fraction of the cell; light = ~1/8, heavy = ~1/4. The
    // centre of the cell is (0.5, 0.5). A stub runs from the centre to an edge
    // (plus half the stroke so the two arms of a corner overlap cleanly).
    constexpr float t = 1.f / 8;  // light stroke half-width fraction ~ actually full width
    constexpr float lo = 0.5f - t / 2, hw = t; // light: x/y start, width
    constexpr float th = 1.f / 4;
    constexpr float hlo = 0.5f - th / 2, hhw = th; // heavy
    // Directional stubs. left/right/up/down extend from cell centre to the edge.
    auto H = [&](int n, float y0, float w) { // horizontal bar full width
        out[n] = {0, y0, 1, w};
    };
    auto V = [&](int n, float x0, float w) { // vertical bar full height
        out[n] = {x0, 0, w, 1};
    };
    auto L = [&](int n, float y0, float w) { out[n] = {0, y0, 0.5f + w / 2, w}; };   // left stub
    auto R = [&](int n, float y0, float w) { out[n] = {0.5f - w / 2, y0, 0.5f + w / 2, w}; };
    auto U = [&](int n, float x0, float w) { out[n] = {x0, 0, w, 0.5f + w / 2}; };     // up stub
    auto D = [&](int n, float x0, float w) { out[n] = {x0, 0.5f - w / 2, w, 0.5f + w / 2}; };

    switch (cp) {
    // Straight lines span the WHOLE cell so neighbours connect.
    case U'\u2500': H(0, lo, hw); return 1;   // ─ light horizontal
    case U'\u2501': H(0, hlo, hhw); return 1; // ━ heavy horizontal
    case U'\u2502': V(0, lo, hw); return 1;   // │ light vertical
    case U'\u2503': V(0, hlo, hhw); return 1; // ┃ heavy vertical
    // Corners: two stubs meeting at the centre.
    case U'\u250C': D(0, lo, hw); R(1, lo, hw); return 2; // ┌
    case U'\u2510': D(0, lo, hw); L(1, lo, hw); return 2; // ┐
    case U'\u2514': U(0, lo, hw); R(1, lo, hw); return 2; // └
    case U'\u2518': U(0, lo, hw); L(1, lo, hw); return 2; // ┘
    // T junctions: a full straight line + one perpendicular stub.
    case U'\u251C': V(0, lo, hw); R(1, lo, hw); return 2; // ├
    case U'\u2524': V(0, lo, hw); L(1, lo, hw); return 2; // ┤
    case U'\u252C': H(0, lo, hw); D(1, lo, hw); return 2; // ┬
    case U'\u2534': H(0, lo, hw); U(1, lo, hw); return 2; // ┴
    // Cross.
    case U'\u253C': H(0, lo, hw); V(1, lo, hw); return 2; // ┼
    default: return 0;
    }
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
    // Packed layout: rect 4xf32 @0, uv 4xf32 @16, color 4xu8(norm) @32,
    // is_glyph u8(norm) @36, radius u8(raw) @37.
    // aRect (loc1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, off(0));
    glVertexAttribDivisor(1, 1);
    // aUV (loc2) — float atlas coords in [0,1]
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, off(16));
    glVertexAttribDivisor(2, 1);
    // aColor (loc3) — 3 of the 4 packed u8, normalized -> [0,1]
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride, off(32));
    glVertexAttribDivisor(3, 1);
    // aIsGlyph (loc4) — u8 raw 0/1 as float (UNnormalized: normalized would map
    // 1 -> 1/255 ≈ 0.004, failing the shader's `> 0.5` test and rendering every
    // glyph as a solid rect).
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, off(36));
    glVertexAttribDivisor(4, 1);
    // aRadius (loc5) — u8 raw px value as float (unnormalized)
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, off(37));
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
    glVertexAttribDivisor(0, 0); // per-VERTEX (the unit-quad corner), not per-instance

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

// Bind program + uniforms + atlas texture, skipping GL calls whose value is
// unchanged from the last flush/redraw (the program and screen size rarely
// move; the atlas texture never rebinds mid-run).
void Renderer::bind_common(PixelSize px) {
    prog_.use();
    const float w = static_cast<float>(px.w), h = static_cast<float>(px.h);
    if (w != u_px_w_ || h != u_px_h_) {
        glUniform2f(u_screen_, w, h);
        u_px_w_ = w;
        u_px_h_ = h;
    }
    const std::uint32_t tex = atlas_.texture();
    if (tex != bound_tex_) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(u_atlas_, 0);
        bound_tex_ = tex;
    }
}

// Clean-frame fast path: nothing about the grid changed since the last draw,
// so the persistent buffer still holds the exact instances we need. Re-issue
// the same draw calls with zero uploads, zero fences, zero memcpy — just the
// GL state + draw commands. Falls back (returns false) if we have no persistent
// mapping or no recorded draws yet.
bool Renderer::redraw_from_cache(PixelSize px) {
    if (!persistent_ || last_draw_n_ == 0) return false;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    bind_common(px);
    for (int i = 0; i < last_draw_n_; ++i) {
        const DrawCall &d = last_draws_[i];
        if (d.count == 0) continue;
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4,
                                          static_cast<GLsizei>(d.count), d.first);
    }
    glBindVertexArray(0);
    return true;
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

        bind_common(px);

        // Point the instanced attributes at this region's slice.
        const GLuint first = static_cast<GLuint>(base / sizeof(Instance));
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4,
                                          static_cast<GLsizei>(count), first);
        fences_[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Remember this draw so a later clean frame can replay it verbatim.
        if (last_draw_n_ < 2)
            last_draws_[last_draw_n_++] = {first, static_cast<std::uint32_t>(count)};
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
                         bool row_has_cursor, int cur_col, std::int64_t abs_row,
                         bool any_selection) {
    RowCache &rc = rows_[static_cast<std::size_t>(r)];
    if (rc.valid && rc.key == key) return false; // clean — shadow already current

    const int cw = cache_cw_, ch = cache_ch_, ascent = cache_ascent_;
    const auto cells = screen.row(Row{r});
    const float ry = static_cast<float>(r * ch);
    const int base_gy = r * ch + ascent;
    rc.bg.clear();
    rc.glyphs.clear();

    for (int c = 0; c < cache_cols_; ++c) {
        const auto &cell = cells[static_cast<std::size_t>(c)];
        const bool selected = any_selection && screen.is_selected(abs_row, c);
        const bool reverse = term::has(cell.pen.attr, term::Attr::Reverse);
        const bool on_cursor = row_has_cursor && c == cur_col;

        if (selected) {
            rc.bg.push_back(rect_inst(static_cast<float>(c * cw), ry, static_cast<float>(cw),
                                      static_cast<float>(ch), 66, 84, 112, /*radius=*/0));
        } else if (reverse || !std::holds_alternative<term::DefaultColor>(cell.pen.bg)) {
            const term::Color bg = reverse ? cell.pen.fg : cell.pen.bg;
            const Rgb col = palette_.resolve(bg, /*is_fg=*/reverse);
            rc.bg.push_back(rect_inst(static_cast<float>(c * cw), ry, static_cast<float>(cw),
                                      static_cast<float>(ch), col.r, col.g, col.b, /*radius=*/0));
        }

        const char32_t cp = cell.cp;

        // Text decorations (underline styles, strikethrough, overline) are thin
        // fg-coloured bars that must render even on blank cells — git diff and
        // spell-checkers underline runs that include spaces. Emit them before
        // the space-skip below. They go into rc.bg; the glyph draws on top.
        const term::Attr at = cell.pen.attr;
        const bool has_ul = term::has(at, term::Attr::Underline);
        const bool has_st = term::has(at, term::Attr::Strike);
        const bool has_ol = term::has(at, term::Attr::Overline);
        if (has_ul || has_st || has_ol) {
            Rgb dc = on_cursor ? palette_.resolve(cell.pen.bg, /*is_fg=*/false)
                               : palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);
            if (term::has(at, term::Attr::Faint)) {
                const Rgb bg = palette_.resolve(reverse ? cell.pen.fg : cell.pen.bg, reverse);
                auto dim = [](std::uint8_t f, std::uint8_t b) {
                    return static_cast<std::uint8_t>((f * 45 + b * 55) / 100);
                };
                dc = {dim(dc.r, bg.r), dim(dc.g, bg.g), dim(dc.b, bg.b)};
            }
            const float cx = static_cast<float>(c * cw);
            const float fcw = static_cast<float>(cw);
            // Stroke thickness scales with cell height (min 1px).
            const float thick = std::max(1.0f, static_cast<float>(ch) / 14.0f);
            auto bar = [&](float y0, float x, float w) {
                rc.bg.push_back(rect_inst(cx + x, ry + y0, w, thick, dc.r, dc.g, dc.b, 0));
            };
            if (has_ul) {
                // Underline sits just below the baseline.
                const float uy = static_cast<float>(ascent) + thick;
                switch (cell.pen.underline) {
                case term::Underline::Double:
                    bar(uy, 0, fcw);
                    bar(uy + thick * 2.0f, 0, fcw);
                    break;
                case term::Underline::Dotted: {
                    const float d = thick * 2.0f;
                    for (float x = 0; x + thick <= fcw; x += d) bar(uy, x, thick);
                    break;
                }
                case term::Underline::Dashed: {
                    const float seg = fcw / 3.0f;
                    bar(uy, 0, seg);
                    bar(uy, 2.0f * seg, seg);
                    break;
                }
                case term::Underline::Curly: {
                    // Approximate a wave with short segments alternating y.
                    const int seg = 4;
                    const float sw = fcw / static_cast<float>(seg);
                    for (int s = 0; s < seg; ++s) {
                        const float yoff = (s % 2 == 0) ? 0.0f : thick * 1.5f;
                        bar(uy + yoff, static_cast<float>(s) * sw, sw);
                    }
                    break;
                }
                default: // Single
                    bar(uy, 0, fcw);
                    break;
                }
            }
            if (has_st) bar(static_cast<float>(ascent) * 0.62f, 0, fcw);   // strike ~mid-x-height
            if (has_ol) bar(0.0f, 0, fcw);                                  // overline at cell top
        }

        if (cp == U' ' || cp == 0 || cell.spacer()) continue;

        // Foreground color for this cell's glyph.
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

        // Geometric block elements and box-drawing lines tile/connect pixel-
        // perfectly only when drawn as exact cell-relative rects — the font's
        // bitmaps are a pixel shy and leave seams (broken vertical rules,
        // disconnected blocks). Draw them procedurally in the fg colour; they go
        // into rc.bg so any real glyph still layers on top and z-order holds.
        if (CellRect rects[5]; int nfills = cell_fills(cp, rects)) {
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
        const GlyphInfo *gi = atlas_.glyph(cp, style);
        if (!gi || gi->width == 0 || gi->height == 0) continue;
        const Rgb col = fgcol;
        const float gx = static_cast<float>(c * cw + gi->bearing_x);
        const float gy = static_cast<float>(base_gy - gi->bearing_y);
        rc.glyphs.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                     static_cast<float>(gi->height),
                                     gi->u0, gi->v0, gi->u1, gi->v1,
                                     col.r, col.g, col.b, 255,
                                     /*is_glyph=*/1, 0, 0, 0});
    }
    rc.key = key;
    rc.valid = true;
    return true;
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
    for (int r = 0; r < grid.rows; ++r) {
        const bool row_has_cursor = cursor_visible && r == cur_row;
        const std::int64_t abs_row = any_selection ? screen.viewport_to_abs(r) : 0;

        std::uint64_t key;
        const std::uint64_t ver = any_selection ? 0 : screen.row_version(r);
        if (ver != 0) {
            // Fast path: fold the cursor column into the epoch token so a cursor
            // move on this row still invalidates it. Tag the high bit so an
            // epoch value can never collide with a hashed key (which is mixed).
            key = (ver << 1) ^ (row_has_cursor ? (static_cast<std::uint64_t>(cur_col) + 1) : 0);
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
            }
            key = mix(h) & 0x7fffffffffffffffULL; // clear tag bit: distinct from epoch keys
        }

        any_row_dirty |= build_row(screen, r, key, row_has_cursor, cur_col, abs_row, any_selection);
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
        // Cursor rect sits above backgrounds, beneath glyphs.
        if (cursor_visible) {
            const Rgb cc = palette_.default_fg();
            instances_.push_back(rect_inst(static_cast<float>(cur_col * cw),
                                           static_cast<float>(cur_row * ch),
                                           static_cast<float>(cw), static_cast<float>(ch),
                                           cc.r, cc.g, cc.b, /*radius=*/2));
        }
    }

    if (!any_row_dirty && redraw_from_cache(px)) {
        // Nothing changed: the GPU buffers already hold this exact frame. We
        // replayed the recorded draws with zero uploads / fences / memcpy.
        return;
    }

    // Dirty frame: re-stage both batches (this also re-records the draw calls).
    last_draw_n_ = 0;
    flush(instances_, px);
    flush(glyphs_, px);
}

} // namespace gvte::gfx
