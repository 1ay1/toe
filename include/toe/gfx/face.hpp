// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Face / FaceStack — toe's general font-rendering core.
//
// This is the one place that speaks to the underlying rasterizer (stb_truetype).
// Everything font-related — loading a face from bytes, its metrics, glyph
// lookup, rasterization, and an ORDERED FALLBACK CHAIN — lives behind these two
// types. The rest of the engine (FontAtlas, renderer) never touches an
// stbtt_* symbol or a raw font blob again; it asks a FaceStack for glyphs.
//
// Design goals:
//   • TYPED, not void*. The old code punned stbtt_fontinfo* through void*; here
//     a Face owns a typed (pimpl) handle — stb stays out of this header, but the
//     abstraction is a real class, not an opaque pointer the caller must cast.
//   • GENERAL FALLBACK. Not "primary + one fallback" but an ordered chain
//     (mono → CJK → emoji → symbols → …): the first face that has the codepoint
//     wins, and its own scale/metrics travel WITH the glyph (a fallback face is
//     sized to match the primary's cap/x-height, and its advance is honoured).
//   • FAST. Per-face codepoint→glyph-index resolution is the hot path; a Face
//     caches its scale + v-metrics once and every rasterize is a couple of stb
//     calls with no allocation beyond stb's own bitmap (freed immediately).
//   • CORRECT. Metrics are computed in device pixels with explicit rounding;
//     the cell size is derived from the PRIMARY face only (a monospace grid is
//     defined by the primary), while fallback glyphs are clipped/placed into
//     that cell rather than redefining it.
//
// A Face is move-only (owns the blob + the rasterizer handle). A FaceStack owns
// the whole chain and is the object a FontAtlas holds.

#ifndef TOE_GFX_FACE_HPP
#define TOE_GFX_FACE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "toe/core/types.hpp"

namespace toe::gfx {

// A rasterized coverage bitmap for one glyph: 8-bit alpha, tightly packed
// (row-major, `width` bytes per row), plus its placement relative to the pen
// origin and the horizontal advance. `pixels` is empty for a blank glyph
// (e.g. space) — a valid, non-error result with width==height==0.
struct GlyphBitmap {
    std::vector<std::uint8_t> pixels; // width*height alpha, or empty if blank
    int width = 0, height = 0;        // bitmap dimensions in device pixels
    int bearing_x = 0, bearing_y = 0; // pen origin -> bitmap top-left (y up)
    int advance = 0;                  // horizontal advance in device pixels
};

// Vertical metrics of a face at its chosen pixel size, in device pixels.
struct FaceMetrics {
    int ascent = 0;   // baseline -> top (positive)
    int descent = 0;  // baseline -> bottom (positive magnitude)
    int line_gap = 0; // extra leading between lines
    int advance = 0;  // reference monospace advance (from 'M', then ' ')
};

// One typed font face over an owned byte blob. Wraps the rasterizer without
// leaking it into this header (the stbtt handle is an incomplete pimpl type).
class Face {
public:
    // Parse a face from font-file bytes at `pixel_height`. The Face TAKES
    // OWNERSHIP of `bytes` (the rasterizer points into them for the Face's
    // life). Returns nullopt if the blob isn't a font the rasterizer accepts.
    [[nodiscard]] static std::optional<Face> load(std::vector<std::uint8_t> bytes,
                                                  int pixel_height);

    Face(const Face &) = delete;
    Face &operator=(const Face &) = delete;
    Face(Face &&) noexcept;
    Face &operator=(Face &&) noexcept;
    ~Face();

    // Vertical metrics at the loaded pixel size.
    [[nodiscard]] const FaceMetrics &metrics() const noexcept { return metrics_; }

    // The rasterizer scale factor mapping font units -> device pixels.
    [[nodiscard]] float scale() const noexcept { return scale_; }

    // The glyph index for a codepoint, or 0 if this face has no glyph for it
    // (the caller then tries the next face in the stack). O(1)-ish stb lookup.
    [[nodiscard]] std::uint32_t glyph_index(char32_t cp) const noexcept;

    // Rasterize a glyph selected BY INDEX (used for shaped/ligature glyphs and
    // by the codepoint path once the index is known). Never returns an error;
    // a blank glyph yields an empty-pixels GlyphBitmap.
    [[nodiscard]] GlyphBitmap rasterize(std::uint32_t glyph_index) const;

    // The raw handle, for the (in-tree) shaper that needs GSUB tables. Typed as
    // const void* so callers outside face.cpp don't depend on stb; face.cpp and
    // the shaper reinterpret it. Kept minimal — prefer the methods above.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return data_; }

private:
    Face() = default;

    struct Handle;                          // opaque: wraps stbtt_fontinfo
    std::unique_ptr<Handle> h_;             // typed pimpl (no void*)
    std::vector<std::uint8_t> data_;        // owned blob; stb points into it
    FaceMetrics metrics_{};
    float scale_ = 0.0f;
};

// An ordered chain of faces: the primary defines the cell + metrics; subsequent
// faces are consulted, in order, for codepoints the primary lacks (CJK, emoji,
// symbols). `resolve()` returns the first face that owns the codepoint plus its
// glyph index. When NO loaded face has the codepoint, the stack LAZILY discovers
// a system font that covers it (FontDiscovery), loads it, and appends it — so
// "every UTF character renders" without preloading every font on the machine.
class FaceStack {
public:
    FaceStack();
    ~FaceStack();
    FaceStack(FaceStack &&) noexcept;
    FaceStack &operator=(FaceStack &&) noexcept;
    FaceStack(const FaceStack &) = delete;
    FaceStack &operator=(const FaceStack &) = delete;

    // Add a face to the end of the chain. The FIRST face added is the primary
    // and fixes the reference metrics. Ignored if `face` is nullopt. `path` (if
    // given) is remembered so lazy discovery never re-loads an already-chained
    // file.
    void push(std::optional<Face> face, std::string path = {}) {
        if (face) {
            faces_.push_back(std::move(*face));
            if (!path.empty()) loaded_paths_.push_back(std::move(path));
        }
    }

    // The pixel height every lazily-discovered fallback face is loaded at, and
    // whether lazy discovery is enabled. Set once after the primary is pushed.
    void enable_discovery(int pixel_height) noexcept { pixel_height_ = pixel_height; }

    [[nodiscard]] bool empty() const noexcept { return faces_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return faces_.size(); }

    // The primary face (index 0) — defines the monospace cell. Precondition:
    // !empty().
    [[nodiscard]] const Face &primary() const noexcept { return faces_.front(); }
    [[nodiscard]] Face &primary() noexcept { return faces_.front(); }

    // Where a codepoint lives: the first face in the chain that has a glyph for
    // it, and that glyph's index. On a total miss, lazily discovers + appends a
    // covering system font (so a later codepoint in the same script is instant).
    // Falls back to {primary, .notdef} only when the whole system has nothing,
    // so the caller always gets a drawable result (a visible box).
    struct Resolved {
        const Face *face;    // the face that owns the glyph
        std::uint32_t index; // its glyph index within that face
    };
    [[nodiscard]] Resolved resolve(char32_t cp);

private:
    std::vector<Face> faces_;
    std::unique_ptr<class FontDiscovery> discovery_;
    std::vector<std::string> loaded_paths_; // files already in the chain (dedup)
    int pixel_height_ = 0;                   // 0 = discovery disabled
};

} // namespace toe::gfx

#endif // TOE_GFX_FACE_HPP
