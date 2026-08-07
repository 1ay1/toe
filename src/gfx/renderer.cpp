// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/gfx/renderer.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include <epoxy/gl.h>

namespace gvte::gfx {

namespace {

// Vertex shader: a unit quad [0,1]^2, offset+scaled by per-instance pixel rect,
// projected to clip space via a screen-size uniform. Per-instance UVs and color
// pass through to the fragment stage.
constexpr const char *kVert = R"(#version 330 core
layout(location = 0) in vec2 aCorner;   // unit quad corner
layout(location = 1) in vec4 aRect;      // x,y,w,h in pixels
layout(location = 2) in vec4 aUV;        // u0,v0,u1,v1
layout(location = 3) in vec3 aColor;
layout(location = 4) in float aIsGlyph;

uniform vec2 uScreen;                    // viewport size in pixels

out vec2 vUV;
out vec3 vColor;
out float vIsGlyph;

void main() {
    vec2 px = aRect.xy + aCorner * aRect.zw;
    vec2 ndc = vec2((px.x / uScreen.x) * 2.0 - 1.0,
                    1.0 - (px.y / uScreen.y) * 2.0);  // y-down pixels -> y-up ndc
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = mix(aUV.xy, aUV.zw, aCorner);
    vColor = aColor;
    vIsGlyph = aIsGlyph;
}
)";

// Fragment shader: solid fill for background quads; for glyphs, sample the R8
// atlas as coverage (alpha) and blend the text color over.
constexpr const char *kFrag = R"(#version 330 core
in vec2 vUV;
in vec3 vColor;
in float vIsGlyph;

uniform sampler2D uAtlas;

out vec4 FragColor;

void main() {
    if (vIsGlyph > 0.5) {
        float a = texture(uAtlas, vUV).r;
        FragColor = vec4(vColor, a);
    } else {
        FragColor = vec4(vColor, 1.0);
    }
}
)";

float fr(std::uint8_t v) { return static_cast<float>(v) / 255.0f; }

} // namespace

Result<Renderer> Renderer::create(FontAtlas &&atlas) {
    auto prog = Program::build(kVert, kFrag);
    if (!prog) {
        return std::unexpected(prog.error());
    }
    Renderer r{std::move(atlas), std::move(*prog)};
    r.ensure_buffers();
    return r;
}

Renderer::~Renderer() {
    if (inst_vbo_) glDeleteBuffers(1, &inst_vbo_);
    if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

Renderer::Renderer(Renderer &&o) noexcept
    : atlas_{std::move(o.atlas_)}, palette_{o.palette_}, prog_{std::move(o.prog_)},
      vao_{std::exchange(o.vao_, 0)}, quad_vbo_{std::exchange(o.quad_vbo_, 0)},
      inst_vbo_{std::exchange(o.inst_vbo_, 0)}, inst_capacity_{o.inst_capacity_},
      instances_{std::move(o.instances_)} {}

Renderer &Renderer::operator=(Renderer &&o) noexcept {
    if (this != &o) {
        if (inst_vbo_) glDeleteBuffers(1, &inst_vbo_);
        if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        atlas_ = std::move(o.atlas_);
        palette_ = o.palette_;
        prog_ = std::move(o.prog_);
        vao_ = std::exchange(o.vao_, 0);
        quad_vbo_ = std::exchange(o.quad_vbo_, 0);
        inst_vbo_ = std::exchange(o.inst_vbo_, 0);
        inst_capacity_ = o.inst_capacity_;
        instances_ = std::move(o.instances_);
    }
    return *this;
}

Extent Renderer::cells_for(PixelSize px) const noexcept {
    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    return Extent{cw > 0 ? px.w / cw : 1, ch > 0 ? px.h / ch : 1};
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

    const GLsizei stride = sizeof(Instance);
    auto off = [](std::size_t n) { return reinterpret_cast<void *>(n); };
    // aRect (loc1), aUV (loc2), aColor (loc3), aIsGlyph (loc4)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, off(0));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, off(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, off(8 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, off(11 * sizeof(float)));
    glVertexAttribDivisor(4, 1);

    glBindVertexArray(0);
}

void Renderer::flush(std::size_t count, PixelSize px) {
    if (count == 0) return;

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    const std::size_t bytes = count * sizeof(Instance);
    if (count > inst_capacity_) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), instances_.data(),
                     GL_DYNAMIC_DRAW);
        inst_capacity_ = count;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), instances_.data());
    }

    prog_.use();
    glUniform2f(prog_.uniform("uScreen"), static_cast<float>(px.w), static_cast<float>(px.h));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_.texture());
    glUniform1i(prog_.uniform("uAtlas"), 0);

    while (glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    glBindVertexArray(0);
}

void Renderer::draw(const term::Screen &screen, PixelSize px) {
    const Rgb bgc = palette_.default_bg();
    glClearColor(fr(bgc.r), fr(bgc.g), fr(bgc.b), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const int cw = atlas_.cell_width();
    const int ch = atlas_.cell_height();
    const int ascent = atlas_.ascent();
    const Extent grid = screen.size();

    instances_.clear();

    // Pass 1: background rectangles (only for non-default backgrounds).
    for (int r = 0; r < grid.rows; ++r) {
        const auto cells = screen.row(Row{r});
        for (int c = 0; c < grid.cols; ++c) {
            const auto &cell = cells[static_cast<std::size_t>(c)];
            const bool reverse = term::has(cell.pen.attr, term::Attr::Reverse);
            term::Color bg = reverse ? cell.pen.fg : cell.pen.bg;
            if (std::holds_alternative<term::DefaultColor>(bg) && !reverse) {
                continue; // default bg already cleared
            }
            const Rgb col = palette_.resolve(bg, /*is_fg=*/reverse);
            instances_.push_back(Instance{static_cast<float>(c * cw), static_cast<float>(r * ch),
                                          static_cast<float>(cw), static_cast<float>(ch), 0, 0, 0, 0,
                                          fr(col.r), fr(col.g), fr(col.b), 0.0f});
        }
    }

    // Pass 2: glyphs.
    for (int r = 0; r < grid.rows; ++r) {
        const auto cells = screen.row(Row{r});
        for (int c = 0; c < grid.cols; ++c) {
            const auto &cell = cells[static_cast<std::size_t>(c)];
            if (cell.cp == U' ' || cell.cp == 0) continue;
            const GlyphInfo *gi = atlas_.glyph(cell.cp);
            if (!gi || gi->width == 0 || gi->height == 0) continue;

            const bool reverse = term::has(cell.pen.attr, term::Attr::Reverse);
            const Rgb col = palette_.resolve(reverse ? cell.pen.bg : cell.pen.fg, !reverse);

            const float gx = static_cast<float>(c * cw + gi->bearing_x);
            const float gy = static_cast<float>(r * ch + ascent - gi->bearing_y);
            instances_.push_back(Instance{gx, gy, static_cast<float>(gi->width),
                                          static_cast<float>(gi->height), gi->u0, gi->v0, gi->u1,
                                          gi->v1, fr(col.r), fr(col.g), fr(col.b), 1.0f});
        }
    }

    flush(instances_.size(), px);
}

} // namespace gvte::gfx
