// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Font atlas backed by the Face/FaceStack rasterizer core (face.hpp, over
// stb_truetype) + a tiny GSUB shaper
// (ligatures). No FreeType, no HarfBuzz, no Fontconfig — the font file is read
// directly and glyphs are cached into a single R8 GL atlas on first use.

#include "toe/gfx/font.hpp"
#include "toe/gfx/opentype.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <utility>

#include <epoxy/gl.h>

namespace toe::gfx {

namespace {

std::vector<std::uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

// Draw an 8-bit coverage bitmap into the atlas, returning packed GlyphInfo.
// `bold`/`italic` are synthesized: bold by OR-dilating the coverage 1px right,
// italic by horizontal shear during the copy. Cheap, and fine at cell sizes.
const GlyphInfo *pack_bitmap(const unsigned char *src, int w, int h, int off_x, int off_y,
                             int advance, bool bold, bool italic, std::uint64_t key,
                             std::unordered_map<std::uint64_t, GlyphInfo> &cache, std::uint32_t tex,
                             int atlas_dim, int &pen_x, int &pen_y, int &shelf_h, int cell_h) {
    GlyphInfo info;
    info.bearing_x = off_x;
    info.bearing_y = -off_y; // stb gives y-down top offset; GlyphInfo wants y-up bearing
    info.advance = advance;

    if (w == 0 || h == 0 || !src) {
        info.width = 0;
        info.height = 0;
        auto [it, _] = cache.emplace(key, info);
        return &it->second;
    }

    // Synthesize into a scratch buffer (bold: +1px width; italic: +shear px).
    const int shear = italic ? (h * 2) / 10 : 0; // ~0.2 slope
    const int bw = w + (bold ? 1 : 0) + shear;
    std::vector<unsigned char> buf(static_cast<std::size_t>(bw) * static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const int sx = italic ? ((h - 1 - y) * shear) / std::max(h - 1, 1) : 0;
        for (int x = 0; x < w; ++x) {
            const unsigned char v = src[y * w + x];
            if (!v) continue;
            const int dx = x + sx;
            unsigned char &d = buf[static_cast<std::size_t>(y) * bw + dx];
            d = std::max(d, v);
            if (bold && dx + 1 < bw) {
                unsigned char &d2 = buf[static_cast<std::size_t>(y) * bw + dx + 1];
                d2 = std::max(d2, v);
            }
        }
    }

    info.width = bw;
    info.height = h;
    if (pen_x + bw + 1 > atlas_dim) { pen_x = 1; pen_y += shelf_h + 1; shelf_h = 0; }
    if (pen_y + h + 1 > atlas_dim) { // atlas full
        info.width = 0; info.height = 0;
        auto [it, _] = cache.emplace(key, info);
        return &it->second;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, pen_x, pen_y, bw, h, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    const float inv = 1.0f / static_cast<float>(atlas_dim);
    info.u0 = static_cast<float>(pen_x) * inv;
    info.v0 = static_cast<float>(pen_y) * inv;
    info.u1 = static_cast<float>(pen_x + bw) * inv;
    info.v1 = static_cast<float>(pen_y + h) * inv;
    pen_x += bw + 1;
    shelf_h = std::max(shelf_h, h);
    (void)cell_h;
    auto [it, _] = cache.emplace(key, info);
    return &it->second;
}

} // namespace

Result<FontAtlas> FontAtlas::create(std::string font_path, int pixel_size,
                                    std::string fallback_path, bool ligatures) {
    if (font_path.empty()) return fail("font: no font file path given");
    auto data = read_file(font_path);
    if (data.empty()) return fail("font: cannot read '" + font_path + "'");

    // The primary face fixes the cell + metrics. It's the head of the fallback
    // chain; further faces (CJK/emoji/symbols) are consulted for missing glyphs.
    auto primary = Face::load(std::move(data), pixel_size);
    if (!primary) return fail("font: '" + font_path + "' is not a valid TTF/OTF");

    FontAtlas a;
    a.pixel_size_ = pixel_size;
    a.ligatures_ = ligatures;

    const FaceMetrics &m = primary->metrics();
    a.ascent_ = m.ascent;
    a.cell_h_ = m.ascent + m.descent + m.line_gap;
    a.cell_w_ = m.advance;
    if (a.cell_w_ <= 0) a.cell_w_ = pixel_size / 2 + 1;
    if (a.cell_h_ <= 0) a.cell_h_ = pixel_size + 2;

    a.faces_.push(std::move(primary), font_path);

    // Optional fallback face(s), appended in order. load() sizes each to the
    // same pixel height so their glyphs share the primary's baseline.
    if (!fallback_path.empty()) {
        if (auto fb = read_file(fallback_path); !fb.empty()) {
            a.faces_.push(Face::load(std::move(fb), pixel_size), fallback_path);
        }
    }

    // Enable LAZY, discovery-backed fallback: any codepoint no loaded face has
    // triggers a one-time system-font search (cached by Unicode block), so every
    // UTF character renders — CJK, emoji, symbols, Braille, math — without
    // preloading the whole font tree.
    a.faces_.enable_discovery(pixel_size);

    // Ligature shaper (GSUB calt/liga) over the primary face's bytes. Optional;
    // identity if unavailable.
    if (ligatures) {
        auto *sh = new ot::Shaper{};
        if (sh->parse(a.faces_.primary().bytes()))
            a.shaper_ = sh;
        else
            delete sh;
    }

    // R8 atlas.
    a.atlas_dim_ = 1024;
    glGenTextures(1, &a.tex_);
    glBindTexture(GL_TEXTURE_2D, a.tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a.atlas_dim_, a.atlas_dim_, 0, GL_RED, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    a.pen_x_ = 1;
    a.pen_y_ = 1;
    a.shelf_h_ = 0;
    return a;
}

FontAtlas::FontAtlas(FontAtlas &&o) noexcept
    : faces_{std::move(o.faces_)}, shaper_{std::exchange(o.shaper_, nullptr)},
      pixel_size_{o.pixel_size_}, ligatures_{o.ligatures_}, tex_{std::exchange(o.tex_, 0)},
      atlas_dim_{o.atlas_dim_}, pen_x_{o.pen_x_}, pen_y_{o.pen_y_}, shelf_h_{o.shelf_h_},
      color_tex_{std::exchange(o.color_tex_, 0)}, color_dim_{o.color_dim_}, cpen_x_{o.cpen_x_},
      cpen_y_{o.cpen_y_}, cshelf_h_{o.cshelf_h_},
      cell_w_{o.cell_w_}, cell_h_{o.cell_h_}, ascent_{o.ascent_}, cache_{std::move(o.cache_)} {
    fast_ = o.fast_;
}

FontAtlas &FontAtlas::operator=(FontAtlas &&o) noexcept {
    if (this != &o) {
        destroy();
        faces_ = std::move(o.faces_);
        shaper_ = std::exchange(o.shaper_, nullptr);
        pixel_size_ = o.pixel_size_;
        ligatures_ = o.ligatures_;
        tex_ = std::exchange(o.tex_, 0);
        atlas_dim_ = o.atlas_dim_;
        color_tex_ = std::exchange(o.color_tex_, 0);
        color_dim_ = o.color_dim_;
        cpen_x_ = o.cpen_x_;
        cpen_y_ = o.cpen_y_;
        cshelf_h_ = o.cshelf_h_;
        pen_x_ = o.pen_x_;
        pen_y_ = o.pen_y_;
        shelf_h_ = o.shelf_h_;
        cell_w_ = o.cell_w_;
        cell_h_ = o.cell_h_;
        ascent_ = o.ascent_;
        cache_ = std::move(o.cache_);
        fast_ = o.fast_;
    }
    return *this;
}

FontAtlas::~FontAtlas() { destroy(); }

void FontAtlas::destroy() noexcept {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    if (color_tex_) { glDeleteTextures(1, &color_tex_); color_tex_ = 0; }
    delete static_cast<ot::Shaper *>(shaper_);
    shaper_ = nullptr;
    // Faces (blobs + rasterizer handles) free themselves via FaceStack.
}

bool FontAtlas::has_shaper() const noexcept { return shaper_ != nullptr; }

const GlyphInfo *FontAtlas::rasterize(char32_t cp, FontStyle style) {
    const bool bold = (static_cast<std::uint8_t>(style) & 1) != 0;
    const bool italic = (static_cast<std::uint8_t>(style) & 2) != 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(style) << 32) | cp;

    // The fallback chain picks the first face that owns the codepoint; the
    // glyph is rasterized with THAT face's own scale, and its advance travels
    // with it (pack_bitmap clips into the monospace cell).
    const FaceStack::Resolved r = faces_.resolve(cp);
    const GlyphBitmap g = r.face->rasterize(r.index);
    if (g.is_color) return pack_color(g, key); // emoji -> RGBA atlas
    return pack_bitmap(g.pixels.empty() ? nullptr : g.pixels.data(), g.width, g.height,
                       g.bearing_x, g.bearing_y, g.advance, bold, italic, key, cache_, tex_,
                       atlas_dim_, pen_x_, pen_y_, shelf_h_, cell_h_);
}

const GlyphInfo *FontAtlas::rasterize_index(std::uint32_t gindex, FontStyle style) {
    // Shaped/ligature glyphs come from the PRIMARY face by glyph index.
    const bool bold = (static_cast<std::uint8_t>(style) & 1) != 0;
    const bool italic = (static_cast<std::uint8_t>(style) & 2) != 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(style) << 32) | 0x80000000ull | gindex;

    const GlyphBitmap g = faces_.primary().rasterize(gindex);
    return pack_bitmap(g.pixels.empty() ? nullptr : g.pixels.data(), g.width, g.height,
                       g.bearing_x, g.bearing_y, g.advance, bold, italic, key, cache_, tex_,
                       atlas_dim_, pen_x_, pen_y_, shelf_h_, cell_h_);
}

void FontAtlas::shape_run(std::span<const char32_t> cps, std::span<std::uint32_t> out) const {
    const std::size_t n = cps.size();
    auto *sh = static_cast<ot::Shaper *>(shaper_);
    if (!sh) {
        // No shaper: identity (map each codepoint to its glyph id so the
        // renderer can still draw by glyph index if it wants). Leave 0 to mean
        // "use the codepoint path" — the renderer handles that.
        for (std::size_t i = 0; i < n && i < out.size(); ++i) out[i] = 0;
        return;
    }
    std::vector<std::uint16_t> glyphs(n);
    for (std::size_t i = 0; i < n; ++i) glyphs[i] = sh->glyph_for(cps[i]);
    sh->shape(glyphs);
    for (std::size_t i = 0; i < n && i < out.size(); ++i) out[i] = glyphs[i];
}

void FontAtlas::ensure_color_atlas() {
    if (color_tex_) return;
    color_dim_ = 1024;
    glGenTextures(1, &color_tex_);
    glBindTexture(GL_TEXTURE_2D, color_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, color_dim_, color_dim_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    // LINEAR for colour emoji (they're scaled to the cell; nearest looks blocky).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cpen_x_ = 1;
    cpen_y_ = 1;
    cshelf_h_ = 0;
}

const GlyphInfo *FontAtlas::pack_color(const GlyphBitmap &g, std::uint64_t key) {
    ensure_color_atlas();
    GlyphInfo info{};
    info.is_color = true;
    info.width = g.width;
    info.height = g.height;
    // Centre the emoji in the monospace cell (emoji are ~square, cells are tall
    // and narrow); place it on the baseline like a normal glyph otherwise.
    info.advance = cell_w_;
    info.bearing_x = (cell_w_ - g.width) / 2;
    info.bearing_y = ascent_ + (g.height - ascent_) / 2; // roughly vertically centred

    if (g.width > 0 && g.height > 0 && !g.pixels.empty()) {
        // Shelf-allocate in the RGBA atlas.
        if (cpen_x_ + g.width + 1 > color_dim_) { // new shelf
            cpen_x_ = 1;
            cpen_y_ += cshelf_h_ + 1;
            cshelf_h_ = 0;
        }
        if (cpen_y_ + g.height + 1 <= color_dim_) {
            glBindTexture(GL_TEXTURE_2D, color_tex_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, cpen_x_, cpen_y_, g.width, g.height, GL_RGBA,
                            GL_UNSIGNED_BYTE, g.pixels.data());
            const float inv = 1.0f / static_cast<float>(color_dim_);
            info.u0 = static_cast<float>(cpen_x_) * inv;
            info.v0 = static_cast<float>(cpen_y_) * inv;
            info.u1 = static_cast<float>(cpen_x_ + g.width) * inv;
            info.v1 = static_cast<float>(cpen_y_ + g.height) * inv;
            cpen_x_ += g.width + 1;
            if (g.height > cshelf_h_) cshelf_h_ = g.height;
        }
    }
    auto [it, _] = cache_.emplace(key, info);
    return &it->second;
}

} // namespace toe::gfx
