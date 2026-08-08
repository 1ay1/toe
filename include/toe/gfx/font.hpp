// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Glyph atlas. A monospace font is rasterized on demand by FreeType, each
// glyph packed into a single-channel GL texture (a shelf allocator), and the
// per-glyph UV + metrics cached by codepoint. The renderer looks up a GlyphInfo
// per cell and emits one textured quad.
//
// Type-theoretic note: pixel metrics (advance, bearing, cell size) are plain
// ints in device pixels here, but they are never mixed with the grid's
// Row/Col cell coordinates — the boundary between "cells" and "pixels" is the
// renderer's job (slice 6), and this module speaks only pixels.

#ifndef TOE_GFX_FONT_HPP
#define TOE_GFX_FONT_HPP

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <cmath>
#include <span>
#include <vector>
#include <unordered_map>

#include "toe/core/types.hpp"
#include "toe/gfx/face.hpp"

namespace toe::gfx {

// UV rect (normalized) + placement metrics for one rasterized glyph.
struct GlyphInfo {
    float u0{}, v0{}, u1{}, v1{}; // atlas texture coords
    int width{}, height{};        // bitmap size in pixels
    int bearing_x{}, bearing_y{}; // offset from pen origin to bitmap top-left
    int advance{};                // horizontal advance in pixels
};

// Rendition style for a glyph. Bold/italic are SYNTHESIZED from the primary
// face (FreeType embolden + shear) so any monospace font gets them. The four
// values are a 2-bit field: bit0 = bold, bit1 = italic.
enum class FontStyle : std::uint8_t {
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3,
};

class FontAtlas {
public:
    // Load a monospace font at `pixel_size`. If `family` is empty, Fontconfig
    // picks the system default monospace. Creates the GL atlas texture.
    // Build the atlas from a font FILE path at `pixel_size`. `fallback_path`
    // (may be empty) is a secondary font used for codepoints the primary lacks
    // (CJK/emoji/symbols). `ligatures` enables GSUB calt/liga shaping.
    [[nodiscard]] static Result<FontAtlas> create(std::string font_path, int pixel_size,
                                                  std::string fallback_path = {},
                                                  bool ligatures = true);

    FontAtlas(const FontAtlas &) = delete;
    FontAtlas &operator=(const FontAtlas &) = delete;
    FontAtlas(FontAtlas &&) noexcept;
    FontAtlas &operator=(FontAtlas &&) noexcept;
    ~FontAtlas();

    // Metrics defining a monospace cell.
    [[nodiscard]] int cell_width() const noexcept { return cell_w_; }
    [[nodiscard]] int cell_height() const noexcept { return cell_h_; }
    [[nodiscard]] int ascent() const noexcept { return ascent_; }

    // Look up (rasterizing + packing on first use) the glyph for a codepoint
    // in a given style. Bold/italic are synthesized from the primary face
    // (embolden + shear) so they work with any monospace font, not just ones
    // shipping separate bold/italic files. Returns nullptr only if the
    // codepoint has no glyph in the face.
    const GlyphInfo *glyph(char32_t cp, FontStyle style = FontStyle::Regular) {
        const auto st = static_cast<std::size_t>(style);
        // Fast path: Latin-1 lives in a flat per-style array — an index, no
        // hashing. This is the overwhelmingly common case (ASCII text) and the
        // renderer's hottest lookup.
        if (cp < kFastCount) {
            FastSlot &s = fast_[st][cp];
            if (s.state == FastSlot::Ready) return &s.info;
            if (s.state == FastSlot::Missing) return nullptr;
            const GlyphInfo *gi = rasterize(cp, style);
            if (gi) {
                s.info = *gi;
                s.state = FastSlot::Ready;
                return &s.info;
            }
            s.state = FastSlot::Missing;
            return nullptr;
        }
        // Rare codepoints: key the hash on (style, codepoint).
        const std::uint64_t key = (static_cast<std::uint64_t>(st) << 32) | cp;
        if (auto it = cache_.find(key); it != cache_.end()) {
            return &it->second;
        }
        return rasterize(cp, style);
    }

    // Look up a glyph by FreeType GLYPH INDEX (not codepoint) in a given style.
    // Used for ligatures / shaped glyphs, which have no single codepoint. Keyed
    // separately from codepoint glyphs (high bit set).
    const GlyphInfo *glyph_by_index(std::uint32_t gindex, FontStyle style = FontStyle::Regular) {
        const auto st = static_cast<std::uint64_t>(style);
        const std::uint64_t key = (st << 32) | 0x80000000ull | gindex;
        if (auto it = cache_.find(key); it != cache_.end()) return &it->second;
        return rasterize_index(gindex, style);
    }

    // Shape a run of codepoints into glyph ids via the primary font's GSUB
    // calt/liga (programming ligatures). `out` is filled with one glyph id per
    // input cell; a ligated-away position gets glyph 0 (the renderer skips it).
    // No-op (identity glyph ids) when ligatures are disabled or unavailable.
    void shape_run(std::span<const char32_t> cps, std::span<std::uint32_t> out) const;
    [[nodiscard]] bool has_shaper() const noexcept;

    // The GL atlas texture id (GL_R8), for binding by the renderer.
    [[nodiscard]] std::uint32_t texture() const noexcept { return tex_; }
    [[nodiscard]] int atlas_size() const noexcept { return atlas_dim_; }

private:
    FontAtlas() = default;
    void destroy() noexcept;
    const GlyphInfo *rasterize(char32_t cp, FontStyle style);
    // Rasterize+pack a glyph selected by GLYPH INDEX (for ligatures/shaping).
    const GlyphInfo *rasterize_index(std::uint32_t gindex, FontStyle style);

    // Owned font blobs live inside the FaceStack now. The stack is the ordered
    // fallback chain (primary first, then CJK/emoji/symbol faces); glyph lookup
    // and rasterization go through it. The primary face fixes the cell metrics.
    FaceStack faces_{};
    void *shaper_{nullptr};         // ot::Shaper* over the primary face's GSUB
    int pixel_size_{0};
    bool ligatures_{true};

    std::uint32_t tex_{0};
    int atlas_dim_{0};
    int pen_x_{0}, pen_y_{0}, shelf_h_{0}; // shelf allocator cursor

    int cell_w_{0}, cell_h_{0}, ascent_{0};

    std::unordered_map<std::uint64_t, GlyphInfo> cache_{}; // key = style<<32 | cp

    // Flat cache for Latin-1 codepoints — the renderer's hot path. Indexed
    // directly by codepoint per style, so no hashing on ASCII text.
    static constexpr char32_t kFastCount = 256;
    static constexpr std::size_t kStyles = 4; // Regular, Bold, Italic, BoldItalic
    struct FastSlot {
        enum State : std::uint8_t { Empty, Ready, Missing } state = Empty;
        GlyphInfo info{};
    };
    std::array<std::array<FastSlot, kFastCount>, kStyles> fast_{};
};

} // namespace toe::gfx

#endif // TOE_GFX_FONT_HPP
