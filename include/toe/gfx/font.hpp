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
    bool is_color{};              // sample the RGBA colour atlas (emoji) vs R8
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
    // Optional REAL styled font files (bold / italic / bold-italic). Any empty
    // path means "synthesize that style from the regular face" (embolden/shear),
    // preserving the old behaviour. When present, that style renders from the
    // real face — the biggest single glyph-quality upgrade.
    struct StyleFiles {
        std::string bold;
        std::string italic;
        std::string bold_italic;
    };

    // Build the atlas from a font FILE path at `pixel_size`. `fallback_path`
    // (may be empty) is a secondary font used for codepoints the primary lacks
    // (CJK/emoji/symbols). `ligatures` enables GSUB calt/liga shaping. `styles`
    // supplies optional real bold/italic faces.
    [[nodiscard]] static Result<FontAtlas> create(std::string font_path, int pixel_size,
                                                  std::string fallback_path = {},
                                                  bool ligatures = true,
                                                  StyleFiles styles = StyleFiles{});

    // The cell geometry `create()` WOULD produce for this font+size, computed
    // WITHOUT a GPU context or an atlas allocation — it only parses the face's
    // metrics. Returns {0,0} if the file can't be loaded.
    //
    // This exists so a host can size the child's terminal grid BEFORE opening a
    // window. That matters on Windows: ConPTY repaints its entire viewport on
    // every resize, so a pty spawned at a placeholder size and corrected later
    // makes the shell paint everything twice. Computing the real grid up front
    // makes that first resize a no-op.
    [[nodiscard]] static PixelSize probe_cell_size(const std::string &font_path, int pixel_size);

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

    // The atlas glyph texture, as a sokol view for the renderer to bind. The
    // atlas is packed incrementally into a CPU shadow (`shadow_`) and re-uploaded
    // to the sg_image on change (sokol replaces whole images). `atlas_dirty()`
    // reports whether an upload is pending; the renderer flushes it before draw.
    [[nodiscard]] std::uint32_t glyph_image_id() const noexcept { return tex_id_; }
    [[nodiscard]] std::uint32_t color_image_id() const noexcept { return color_tex_id_; }
    [[nodiscard]] int atlas_size() const noexcept { return atlas_dim_; }
    [[nodiscard]] int color_atlas_size() const noexcept { return color_dim_; }
    // Sync any pending CPU-shadow changes to the GPU images (called by the
    // renderer once per frame before binding). Creates the images lazily.
    void sync_gpu();
    // The sokol view handles (opaque uint32 ids from sg_view.id) for binding.
    // Valid after sync_gpu(); 0 = not yet created.
    [[nodiscard]] std::uint32_t glyph_view() const noexcept { return glyph_view_id_; }
    [[nodiscard]] std::uint32_t color_view() const noexcept { return color_view_id_; }

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
    // Optional REAL styled stacks, indexed by style bits (1=bold, 2=italic,
    // 3=bold-italic); index 0 is unused (regular lives in faces_). An empty
    // stack means "synthesize this style from faces_" (the old path).
    std::array<FaceStack, 4> styled_{};
    std::array<bool, 4> has_styled_{}; // true if styled_[i] holds a real face
    void *shaper_{nullptr};         // ot::Shaper* over the primary face's GSUB
    int pixel_size_{0};
    bool ligatures_{true};

    // GPU atlas: a CPU shadow (the authoritative pixels, packed incrementally)
    // plus a lazily-created sokol image the shadow is uploaded into. `tex_id_`
    // is the sg_image id, `glyph_view_id_` the sg_view id; 0 until created.
    std::vector<std::uint8_t> shadow_{};    // R8 glyph atlas pixels
    std::uint32_t tex_id_{0};
    std::uint32_t glyph_view_id_{0};
    bool atlas_dirty_{false};
    // Reusable scratch for glyph synthesis (bold/italic) — avoids a heap alloc
    // per never-seen glyph in the rasterize hot path. Grown, never shrunk.
    std::vector<unsigned char> synth_scratch_{};
    int atlas_dim_{0};
    // Grow the (square) glyph atlas by doubling when it fills, up to this cap,
    // instead of dropping new glyphs. Cached UVs are normalized, so on grow we
    // rescale every stored UV by old/new — pixel positions are preserved.
    static constexpr int kMaxAtlasDim = 4096;
    bool grow_atlas();
    const GlyphInfo *cache_blank(std::uint64_t key); // last-resort blank at hard cap
    int pen_x_{0}, pen_y_{0}, shelf_h_{0}; // shelf allocator cursor

    // Separate RGBA colour (emoji) atlas, same shadow+image scheme, lazy.
    std::vector<std::uint8_t> color_shadow_{};
    std::uint32_t color_tex_id_{0};
    std::uint32_t color_view_id_{0};
    bool color_dirty_{false};
    int color_dim_{0};
    int cpen_x_{0}, cpen_y_{0}, cshelf_h_{0};
    const GlyphInfo *pack_color(const GlyphBitmap &g, std::uint64_t key);
    void ensure_color_atlas();

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
