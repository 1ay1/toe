// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Font atlas backed by the Face/FaceStack rasterizer core (face.hpp, over
// stb_truetype) + a tiny GSUB shaper
// (ligatures). No FreeType, no HarfBuzz, no Fontconfig — the font file is read
// directly and glyphs are cached into a single R8 GL atlas on first use.

#include "toe/gfx/font.hpp"
#include "toe/gfx/opentype.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include "toe/gfx/gpu.hpp"

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
                             std::unordered_map<std::uint64_t, GlyphInfo> &cache,
                             std::vector<std::uint8_t> &shadow, bool &dirty, int atlas_dim,
                             int &pen_x, int &pen_y, int &shelf_h, int cell_h,
                             std::vector<unsigned char> &scratch) {
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

    // Synthesize into a reusable scratch buffer (bold: +1px width; italic:
    // +shear px). Reusing the caller's buffer avoids a heap allocation for
    // every never-seen glyph.
    const int shear = italic ? (h * 2) / 10 : 0; // ~0.2 slope
    const int bw = w + (bold ? 1 : 0) + shear;
    const std::size_t need = static_cast<std::size_t>(bw) * static_cast<std::size_t>(h);
    if (scratch.size() < need) scratch.resize(need);
    unsigned char *buf = scratch.data();
    std::memset(buf, 0, need);
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
    if (pen_y + h + 1 > atlas_dim) {
        // Atlas full. Signal the caller (rasterize) to grow the atlas and retry;
        // returning nullptr keeps NOTHING cached for this key so the retry is
        // clean. Only at the hard size cap does the caller fall back to blank.
        return nullptr;
    }
    // Blit the synthesized coverage into the CPU shadow at the shelf position.
    // The renderer uploads the shadow to the sg_image once per frame if dirty.
    for (int y = 0; y < h; ++y) {
        std::uint8_t *dst = shadow.data() + static_cast<std::size_t>(pen_y + y) * atlas_dim + pen_x;
        std::memcpy(dst, buf + static_cast<std::size_t>(y) * bw, static_cast<std::size_t>(bw));
    }
    dirty = true;
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

PixelSize FontAtlas::probe_cell_size(const std::string &font_path, int pixel_size) {
    if (font_path.empty()) return PixelSize{0, 0};
    auto data = read_file(font_path);
    if (data.empty()) return PixelSize{0, 0};
    auto primary = Face::load(std::move(data), pixel_size);
    if (!primary) return PixelSize{0, 0};

    // Must stay in lockstep with create() below — same metrics, same fallbacks.
    const FaceMetrics &m = primary->metrics();
    int w = m.advance;
    int h = m.ascent + m.descent + m.line_gap;
    if (w <= 0) w = pixel_size / 2 + 1;
    if (h <= 0) h = pixel_size + 2;
    return PixelSize{w, h};
}

Result<FontAtlas> FontAtlas::create(std::string font_path, int pixel_size,
                                    std::string fallback_path, bool ligatures,
                                    StyleFiles styles) {
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

    // Optional REAL styled faces (bold / italic / bold-italic). Load each into
    // its own stack indexed by style bits; discovery is enabled so styled text
    // still gets CJK/emoji/symbol fallback. An absent file leaves has_styled_
    // false, so rasterize() synthesizes that style from the regular face.
    const std::string style_paths[4] = {
        {}, std::move(styles.bold), std::move(styles.italic), std::move(styles.bold_italic)};
    for (int i = 1; i < 4; ++i) {
        if (style_paths[i].empty()) continue;
        auto sd = read_file(style_paths[i]);
        if (sd.empty()) continue;
        auto sf = Face::load(std::move(sd), pixel_size);
        if (!sf) continue;
        a.styled_[static_cast<std::size_t>(i)].push(std::move(sf), style_paths[i]);
        a.styled_[static_cast<std::size_t>(i)].enable_discovery(pixel_size);
        a.has_styled_[static_cast<std::size_t>(i)] = true;
    }

    // Ligature shaper (GSUB calt/liga) over the primary face's bytes. Optional;
    // identity if unavailable.
    if (ligatures) {
        auto *sh = new ot::Shaper{};
        if (sh->parse(a.faces_.primary().bytes()))
            a.shaper_ = sh;
        else
            delete sh;
    }

    // R8 atlas: a CPU shadow, uploaded to a lazily-created sg_image by sync_gpu.
    a.atlas_dim_ = 1024;
    a.shadow_.assign(static_cast<std::size_t>(a.atlas_dim_) * a.atlas_dim_, 0);
    a.atlas_dirty_ = true; // create the image on first sync
    a.pen_x_ = 1;
    a.pen_y_ = 1;
    a.shelf_h_ = 0;
    return a;
}

FontAtlas::FontAtlas(FontAtlas &&o) noexcept
    : faces_{std::move(o.faces_)}, styled_{std::move(o.styled_)},
      has_styled_{o.has_styled_}, shaper_{std::exchange(o.shaper_, nullptr)},
      pixel_size_{o.pixel_size_}, ligatures_{o.ligatures_},
      shadow_{std::move(o.shadow_)}, tex_id_{std::exchange(o.tex_id_, 0)},
      glyph_view_id_{std::exchange(o.glyph_view_id_, 0)}, atlas_dirty_{o.atlas_dirty_},
      synth_scratch_{std::move(o.synth_scratch_)},
      atlas_dim_{o.atlas_dim_}, pen_x_{o.pen_x_}, pen_y_{o.pen_y_}, shelf_h_{o.shelf_h_},
      color_shadow_{std::move(o.color_shadow_)}, color_tex_id_{std::exchange(o.color_tex_id_, 0)},
      color_view_id_{std::exchange(o.color_view_id_, 0)}, color_dirty_{o.color_dirty_},
      color_dim_{o.color_dim_}, cpen_x_{o.cpen_x_}, cpen_y_{o.cpen_y_}, cshelf_h_{o.cshelf_h_},
      cell_w_{o.cell_w_}, cell_h_{o.cell_h_}, ascent_{o.ascent_}, cache_{std::move(o.cache_)} {
    fast_ = o.fast_;
}

FontAtlas &FontAtlas::operator=(FontAtlas &&o) noexcept {
    if (this != &o) {
        destroy();
        faces_ = std::move(o.faces_);
        styled_ = std::move(o.styled_);
        has_styled_ = o.has_styled_;
        synth_scratch_ = std::move(o.synth_scratch_);
        shaper_ = std::exchange(o.shaper_, nullptr);
        pixel_size_ = o.pixel_size_;
        ligatures_ = o.ligatures_;
        shadow_ = std::move(o.shadow_);
        tex_id_ = std::exchange(o.tex_id_, 0);
        glyph_view_id_ = std::exchange(o.glyph_view_id_, 0);
        atlas_dirty_ = o.atlas_dirty_;
        atlas_dim_ = o.atlas_dim_;
        color_shadow_ = std::move(o.color_shadow_);
        color_tex_id_ = std::exchange(o.color_tex_id_, 0);
        color_view_id_ = std::exchange(o.color_view_id_, 0);
        color_dirty_ = o.color_dirty_;
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
    if (glyph_view_id_) { gpu::destroy_view(glyph_view_id_); glyph_view_id_ = 0; }
    if (tex_id_) { gpu::destroy_image(tex_id_); tex_id_ = 0; }
    if (color_view_id_) { gpu::destroy_view(color_view_id_); color_view_id_ = 0; }
    if (color_tex_id_) { gpu::destroy_image(color_tex_id_); color_tex_id_ = 0; }
    delete static_cast<ot::Shaper *>(shaper_);
    shaper_ = nullptr;
    // Faces (blobs + rasterizer handles) free themselves via FaceStack.
}

// Upload any pending CPU-shadow changes to the GPU images (lazy-create them).
// Called by the renderer once per frame before it binds the atlas.
// Double the square glyph atlas when it fills, up to kMaxAtlasDim, instead of
// dropping new glyphs. Glyph *pixel* positions are preserved (old shadow rows
// are copied to the top of the new, taller buffer), so we only need to rescale
// the NORMALIZED UVs already cached in fast_/cache_ by old/new. Returns false
// only at the hard cap (then the caller blanks that one glyph, as before).
bool FontAtlas::grow_atlas() {
    if (atlas_dim_ >= kMaxAtlasDim) return false;
    const int old_dim = atlas_dim_;
    const int new_dim = old_dim * 2;

    // New shadow, old content copied row-by-row into the top-left.
    std::vector<std::uint8_t> ns(static_cast<std::size_t>(new_dim) * new_dim, 0);
    for (int y = 0; y < old_dim; ++y) {
        std::memcpy(ns.data() + static_cast<std::size_t>(y) * new_dim,
                    shadow_.data() + static_cast<std::size_t>(y) * old_dim,
                    static_cast<std::size_t>(old_dim));
    }
    shadow_ = std::move(ns);
    atlas_dim_ = new_dim;

    // Rescale every cached UV (pixel pos unchanged, divisor doubled).
    const float s = static_cast<float>(old_dim) / static_cast<float>(new_dim);
    for (auto &row : fast_) {
        for (auto &slot : row) {
            if (slot.state == FastSlot::Ready && !slot.info.is_color) {
                slot.info.u0 *= s; slot.info.v0 *= s;
                slot.info.u1 *= s; slot.info.v1 *= s;
            }
        }
    }
    for (auto &kv : cache_) {
        GlyphInfo &gi = kv.second;
        if (!gi.is_color) { gi.u0 *= s; gi.v0 *= s; gi.u1 *= s; gi.v1 *= s; }
    }

    // The GPU image must be recreated at the new size on the next sync.
    if (glyph_view_id_) { gpu::destroy_view(glyph_view_id_); glyph_view_id_ = 0; }
    if (tex_id_) { gpu::destroy_image(tex_id_); tex_id_ = 0; }
    atlas_dirty_ = true;
    return true;
}

void FontAtlas::sync_gpu() {
    if (atlas_dirty_) {
        if (!tex_id_) {
            tex_id_ = gpu::make_image(atlas_dim_, atlas_dim_, gpu::Fmt::R8, shadow_.data());
            glyph_view_id_ = gpu::make_texture_view(tex_id_);
        } else {
            gpu::update_image(tex_id_, atlas_dim_, atlas_dim_, gpu::Fmt::R8, shadow_.data());
        }
        atlas_dirty_ = false;
    }
    if (color_dirty_ && !color_shadow_.empty()) {
        if (!color_tex_id_) {
            color_tex_id_ =
                gpu::make_image(color_dim_, color_dim_, gpu::Fmt::RGBA8, color_shadow_.data());
            color_view_id_ = gpu::make_texture_view(color_tex_id_);
        } else {
            gpu::update_image(color_tex_id_, color_dim_, color_dim_, gpu::Fmt::RGBA8,
                              color_shadow_.data());
        }
        color_dirty_ = false;
    }
}

bool FontAtlas::has_shaper() const noexcept { return shaper_ != nullptr; }

const GlyphInfo *FontAtlas::rasterize(char32_t cp, FontStyle style) {
    const auto sbits = static_cast<std::uint8_t>(style) & 3u;
    bool bold = (sbits & 1) != 0;
    bool italic = (sbits & 2) != 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(style) << 32) | cp;

    // Prefer a REAL styled face when one is loaded for this style. Falling back
    // through the exact-then-lesser styles matches how designers ship families:
    // e.g. bold-italic text uses the bold-italic file if present, else the
    // italic file with synthesized bold, else bold with synthesized italic,
    // else the regular with both synthesized. Whatever we DON'T have a real
    // face for stays flagged for synthesis in pack_bitmap.
    FaceStack *stack = &faces_;
    if (sbits != 0) {
        if (has_styled_[sbits]) { // exact styled face
            stack = &styled_[sbits];
            bold = italic = false; // fully real, synthesize nothing
        } else if (sbits == 3u && has_styled_[2]) { // bold-italic -> italic + synth bold
            stack = &styled_[2];
            italic = false;
        } else if (sbits == 3u && has_styled_[1]) { // bold-italic -> bold + synth italic
            stack = &styled_[1];
            bold = false;
        }
    }

    // The fallback chain picks the first face that owns the codepoint; the
    // glyph is rasterized with THAT face's own scale, and its advance travels
    // with it (pack_bitmap clips into the monospace cell).
    const FaceStack::Resolved r = stack->resolve(cp);
    const GlyphBitmap g = r.face->rasterize(r.index);
    if (g.is_color) return pack_color(g, key); // emoji -> RGBA atlas
    const unsigned char *src = g.pixels.empty() ? nullptr : g.pixels.data();
    for (;;) {
        if (const GlyphInfo *gi =
                pack_bitmap(src, g.width, g.height, g.bearing_x, g.bearing_y, g.advance, bold,
                            italic, key, cache_, shadow_, atlas_dirty_, atlas_dim_, pen_x_, pen_y_,
                            shelf_h_, cell_h_, synth_scratch_))
            return gi;
        // Atlas full: grow (doubles, rescales cached UVs) and retry. At the hard
        // cap grow_atlas() fails once and we cache a blank so we don't loop.
        if (!grow_atlas()) return cache_blank(key);
    }
}

const GlyphInfo *FontAtlas::rasterize_index(std::uint32_t gindex, FontStyle style) {
    // Shaped/ligature glyphs come from the PRIMARY face by glyph index.
    const bool bold = (static_cast<std::uint8_t>(style) & 1) != 0;
    const bool italic = (static_cast<std::uint8_t>(style) & 2) != 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(style) << 32) | 0x80000000ull | gindex;

    const GlyphBitmap g = faces_.primary().rasterize(gindex);
    const unsigned char *src = g.pixels.empty() ? nullptr : g.pixels.data();
    for (;;) {
        if (const GlyphInfo *gi =
                pack_bitmap(src, g.width, g.height, g.bearing_x, g.bearing_y, g.advance, bold,
                            italic, key, cache_, shadow_, atlas_dirty_, atlas_dim_, pen_x_, pen_y_,
                            shelf_h_, cell_h_, synth_scratch_))
            return gi;
        if (!grow_atlas()) return cache_blank(key);
    }
}

// Cache and return a zero-size (blank) glyph for `key` — the last resort once
// the atlas has grown to its hard cap and still can't fit. Rare in practice.
const GlyphInfo *FontAtlas::cache_blank(std::uint64_t key) {
    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr,
                     "toe: font atlas at max size (%dx%d) — some rare glyphs may render blank\n",
                     atlas_dim_, atlas_dim_);
        warned = true;
    }
    GlyphInfo info{};
    auto [it, _] = cache_.emplace(key, info);
    return &it->second;
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
    if (!color_shadow_.empty()) return;
    color_dim_ = 1024;
    color_shadow_.assign(static_cast<std::size_t>(color_dim_) * color_dim_ * 4, 0);
    color_dirty_ = true;
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
            // Blit into the RGBA shadow (row by row; 4 bytes/px).
            for (int y = 0; y < g.height; ++y) {
                std::uint8_t *dst = color_shadow_.data() +
                    (static_cast<std::size_t>(cpen_y_ + y) * color_dim_ + cpen_x_) * 4;
                std::memcpy(dst, g.pixels.data() + static_cast<std::size_t>(y) * g.width * 4,
                            static_cast<std::size_t>(g.width) * 4);
            }
            color_dirty_ = true;
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
