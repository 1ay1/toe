// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/gfx/font.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <fontconfig/fontconfig.h>
#include <epoxy/gl.h>

#include <hb.h>
#include <hb-ft.h>

namespace gvte::gfx {

namespace {

// Resolve a font family (or the default monospace) to a file path via
// Fontconfig. Returns an owned std::string path.
Result<std::string> resolve_font(const std::string &family) {
    if (!FcInit()) {
        return fail("fontconfig: FcInit failed");
    }
    FcPattern *pat = FcNameParse(
        reinterpret_cast<const FcChar8 *>(family.empty() ? "monospace" : family.c_str()));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(nullptr, pat, &result);
    std::string path;
    if (match) {
        FcChar8 *file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            path = reinterpret_cast<const char *>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);

    if (path.empty()) {
        return fail("fontconfig: no match for '" + family + "'");
    }
    return path;
}

// Find a font file that contains `cp` (used for CJK/emoji/symbol fallback).
// Returns an empty string if fontconfig can't suggest one.
std::string resolve_fallback(char32_t cp) {
    FcPattern *pat = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(nullptr, pat, &result);
    std::string path;
    if (match) {
        FcChar8 *file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            path = reinterpret_cast<const char *>(file);
        }
        FcPatternDestroy(match);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    return path;
}

} // namespace

Result<FontAtlas> FontAtlas::create(std::string family, int pixel_size) {
    auto path = resolve_font(family);
    if (!path) {
        return std::unexpected(path.error());
    }

    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft) != 0) {
        return fail("freetype: FT_Init_FreeType failed");
    }
    FT_Face face = nullptr;
    if (FT_New_Face(ft, path->c_str(), 0, &face) != 0) {
        FT_Done_FreeType(ft);
        return fail("freetype: FT_New_Face failed for " + *path);
    }
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
        FT_Done_Face(face);
        FT_Done_FreeType(ft);
        return fail("freetype: FT_Set_Pixel_Sizes failed");
    }

    FontAtlas a;
    a.ft_ = ft;
    a.face_ = face;
    a.pixel_size_ = pixel_size;

    // Cell metrics. For a monospace grid the cell width is the advance of a
    // representative glyph, NOT face->metrics.max_advance (which reflects the
    // widest possible glyph, e.g. wide CJK, and would over-space the grid).
    // Measure the advance of 'M' (or space) directly.
    a.cell_h_ = static_cast<int>(face->size->metrics.height >> 6);
    a.ascent_ = static_cast<int>(face->size->metrics.ascender >> 6);

    int advance = 0;
    if (FT_Load_Char(face, U'M', FT_LOAD_DEFAULT) == 0) {
        advance = static_cast<int>(face->glyph->advance.x >> 6);
    }
    if (advance <= 0 && FT_Load_Char(face, U' ', FT_LOAD_DEFAULT) == 0) {
        advance = static_cast<int>(face->glyph->advance.x >> 6);
    }
    a.cell_w_ = advance;

    if (a.cell_w_ <= 0) a.cell_w_ = pixel_size / 2 + 1;
    if (a.cell_h_ <= 0) a.cell_h_ = pixel_size + 2;

    // Allocate a square R8 atlas. 1024px holds a few thousand ASCII/box glyphs.
    a.atlas_dim_ = 1024;
    glGenTextures(1, &a.tex_);
    glBindTexture(GL_TEXTURE_2D, a.tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a.atlas_dim_, a.atlas_dim_, 0, GL_RED, GL_UNSIGNED_BYTE,
                 nullptr);
    // Glyph coverage textures are single-channel; nearest-neighbour keeps
    // small text crisp (linear blurs sub-pixel glyph edges).
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
    : ft_{std::exchange(o.ft_, nullptr)}, face_{std::exchange(o.face_, nullptr)},
      hb_font_{std::exchange(o.hb_font_, nullptr)},
      fallback_faces_{std::move(o.fallback_faces_)}, pixel_size_{o.pixel_size_},
      tex_{std::exchange(o.tex_, 0)}, atlas_dim_{o.atlas_dim_}, pen_x_{o.pen_x_}, pen_y_{o.pen_y_},
      shelf_h_{o.shelf_h_}, cell_w_{o.cell_w_}, cell_h_{o.cell_h_}, ascent_{o.ascent_},
      cache_{std::move(o.cache_)} {
    fast_ = o.fast_;
    o.fallback_faces_.clear();
}

FontAtlas &FontAtlas::operator=(FontAtlas &&o) noexcept {
    if (this != &o) {
        destroy();
        ft_ = std::exchange(o.ft_, nullptr);
        face_ = std::exchange(o.face_, nullptr);
        hb_font_ = std::exchange(o.hb_font_, nullptr);
        fallback_faces_ = std::move(o.fallback_faces_);
        o.fallback_faces_.clear();
        pixel_size_ = o.pixel_size_;
        tex_ = std::exchange(o.tex_, 0);
        atlas_dim_ = o.atlas_dim_;
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
    if (tex_) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    // Free fallback faces first (some map entries alias the primary as a
    // "use primary" sentinel — skip those), then the primary itself.
    for (auto &[path, fb] : fallback_faces_) {
        if (fb && fb != face_) FT_Done_Face(static_cast<FT_Face>(fb));
    }
    fallback_faces_.clear();
    if (hb_font_) {
        hb_font_destroy(static_cast<hb_font_t *>(hb_font_));
        hb_font_ = nullptr;
    }
    if (face_) {
        FT_Done_Face(static_cast<FT_Face>(face_));
        face_ = nullptr;
    }
    if (ft_) {
        FT_Done_FreeType(static_cast<FT_Library>(ft_));
        ft_ = nullptr;
    }
}

// Pick the FT_Face that actually contains `cp`: the primary if it has the
// glyph, otherwise a fallback font (opened + cached on first use), otherwise
// the primary (which renders .notdef). This is what gives CJK/emoji coverage.
void *FontAtlas::face_for(char32_t cp) {
    auto primary = static_cast<FT_Face>(face_);
    if (FT_Get_Char_Index(primary, cp) != 0) {
        return face_;
    }
    const std::string path = resolve_fallback(cp);
    if (path.empty()) {
        return face_;
    }
    if (auto it = fallback_faces_.find(path); it != fallback_faces_.end()) {
        return it->second;
    }
    FT_Face fb = nullptr;
    if (FT_New_Face(static_cast<FT_Library>(ft_), path.c_str(), 0, &fb) != 0 || !fb) {
        fallback_faces_.emplace(path, face_); // remember the failure as "use primary"
        return face_;
    }
    FT_Set_Pixel_Sizes(fb, 0, static_cast<FT_UInt>(pixel_size_));
    fallback_faces_.emplace(path, fb);
    return fb;
}

// Apply synthetic bold/italic to an outline glyph slot, render it, and pack the
// bitmap into the atlas, returning the cached GlyphInfo (keyed by `key`).
static const GlyphInfo *pack_slot_impl(FT_GlyphSlot g, bool bold, bool italic, FT_Pos bold_strength,
                                       std::uint64_t key,
                                       std::unordered_map<std::uint64_t, GlyphInfo> &cache,
                                       std::uint32_t tex, int atlas_dim, int &pen_x, int &pen_y,
                                       int &shelf_h) {
    if (g->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (italic) {
            FT_Matrix shear{0x10000, static_cast<FT_Fixed>(0.2 * 0x10000), 0, 0x10000};
            FT_Outline_Transform(&g->outline, &shear);
        }
        if (bold) FT_Outline_Embolden(&g->outline, bold_strength);
    }
    if (FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL) != 0) return nullptr;

    const int w = static_cast<int>(g->bitmap.width);
    const int h = static_cast<int>(g->bitmap.rows);
    GlyphInfo info;
    info.width = w;
    info.height = h;
    info.bearing_x = g->bitmap_left;
    info.bearing_y = g->bitmap_top;
    info.advance = static_cast<int>(g->advance.x >> 6);
    if (w == 0 || h == 0) {
        auto [it, _] = cache.emplace(key, info);
        return &it->second;
    }
    if (pen_x + w + 1 > atlas_dim) { pen_x = 1; pen_y += shelf_h + 1; shelf_h = 0; }
    if (pen_y + h + 1 > atlas_dim) return nullptr; // atlas full
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, pen_x, pen_y, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    g->bitmap.buffer);
    const float inv = 1.0f / static_cast<float>(atlas_dim);
    info.u0 = static_cast<float>(pen_x) * inv;
    info.v0 = static_cast<float>(pen_y) * inv;
    info.u1 = static_cast<float>(pen_x + w) * inv;
    info.v1 = static_cast<float>(pen_y + h) * inv;
    pen_x += w + 1;
    shelf_h = std::max(shelf_h, h);
    auto [it, _] = cache.emplace(key, info);
    return &it->second;
}

const GlyphInfo *FontAtlas::rasterize(char32_t cp, FontStyle style) {
    auto face = static_cast<FT_Face>(face_for(cp));
    const bool bold = (static_cast<std::uint8_t>(style) & 1) != 0;
    const bool italic = (static_cast<std::uint8_t>(style) & 2) != 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(style) << 32) | cp;
    if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) != 0) return nullptr;
    const FT_Pos strength = (face->size->metrics.x_ppem * 64) / 14;
    return pack_slot_impl(face->glyph, bold, italic, strength, key, cache_, tex_, atlas_dim_,
                          pen_x_, pen_y_, shelf_h_);
}

const GlyphInfo *FontAtlas::rasterize_index(std::uint32_t gindex, FontStyle style) {
    // Ligature/shaped glyphs come from the PRIMARY face by glyph index.
    auto face = static_cast<FT_Face>(face_);
    const bool bold = (static_cast<std::uint8_t>(style) & 1) != 0;
    const bool italic = (static_cast<std::uint8_t>(style) & 2) != 0;
    const std::uint64_t key =
        (static_cast<std::uint64_t>(style) << 32) | 0x80000000ull | gindex;
    if (FT_Load_Glyph(face, gindex, FT_LOAD_DEFAULT) != 0) return nullptr;
    const FT_Pos strength = (face->size->metrics.x_ppem * 64) / 14;
    return pack_slot_impl(face->glyph, bold, italic, strength, key, cache_, tex_, atlas_dim_,
                          pen_x_, pen_y_, shelf_h_);
}

void *FontAtlas::hb_font() {
    if (!hb_font_ && face_) {
        hb_font_ = hb_ft_font_create(static_cast<FT_Face>(face_), nullptr);
    }
    return hb_font_;
}

} // namespace gvte::gfx
