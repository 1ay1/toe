// SPDX-License-Identifier: LGPL-2.0-or-later
//
// FontDiscovery — find a system font that covers any Unicode codepoint, with NO
// fontconfig, no threads, no heavy deps. This is what makes "every UTF character
// renders": when the primary font and the loaded fallbacks lack a glyph, the
// FaceStack asks discovery for a font FILE that has it, loads it, and appends it
// to the fallback chain.
//
// How real terminals do it (kitty, ghostty, alacritty): fontconfig precomputes a
// per-font coverage bitmap (FcCharSet) and answers "which font has codepoint U+X"
// by scanning that cache. toe deliberately has no fontconfig, so we reproduce the
// same query ourselves — and cache it smarter:
//
//   1. BLOCK-KEYED laziness. Codepoints cluster by Unicode block (CJK, Emoji,
//      Cyrillic, Braille, …). We resolve a whole block at once: the first miss
//      in a block triggers a scan, and once a covering font is found, EVERY
//      codepoint in that block resolves from memory with no further work. A
//      Japanese document pays exactly one scan.
//
//   2. PERSISTENT cache. The block→font map is written to
//      $XDG_CACHE_HOME/toe/fontmap so the scan cost is paid once ACROSS runs,
//      not once per launch. Subsequent launches resolve every seen block
//      instantly from the file.
//
//   3. PRIORITISED candidates. Broad-coverage families (the Noto set, DejaVu,
//      emoji) are probed first, so a scan almost always hits on the first file
//      it opens rather than walking the whole font tree.
//
//   4. GUARANTEED result contract. resolve() returns a path or nullopt; the
//      FaceStack turns a nullopt into a visible .notdef box, so SOMETHING always
//      renders — tofu is a drawn box, never a blank cell.
//
// The scan itself opens a candidate font, reads only its `cmap` coverage (via a
// tiny bounds-checked reader), and asks "does it map this codepoint to a non-zero
// glyph?" — no full face load, no rasterizer, cheap.

#ifndef TOE_GFX_FONT_DISCOVERY_HPP
#define TOE_GFX_FONT_DISCOVERY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace toe::gfx {

// Resolves a codepoint to a system font file that covers it. One instance is
// held by the FontAtlas; it owns the persistent cache and the lazily-built
// candidate list. Not thread-safe (toe is single-threaded by design).
class FontDiscovery {
public:
    FontDiscovery();

    // A font file path that covers `cp`, or nullopt if the whole system font
    // tree has nothing (extremely rare — usually a private-use codepoint). The
    // result is cached by Unicode block, so the next codepoint in the same block
    // is free. `exclude` lets the caller skip files already in the face chain.
    [[nodiscard]] std::optional<std::string>
    resolve(char32_t cp, const std::vector<std::string> &exclude);

    // Persist the in-memory block→font map to the cache file. Called on a fresh
    // resolution so knowledge survives the process.
    void flush() const;

private:
    // A Unicode "block" here is a coarse 256-codepoint page (cp >> 8): fine
    // enough to separate scripts, coarse enough to keep the cache tiny and make
    // one scan cover a lot of ground.
    [[nodiscard]] static std::uint32_t block_of(char32_t cp) noexcept {
        return static_cast<std::uint32_t>(cp) >> 8;
    }

    // Build (once) the prioritised list of candidate font files on the system.
    void ensure_candidates();
    // Load the persistent block→path cache from disk (once).
    void load_cache();
    [[nodiscard]] std::string cache_path() const;

    // Does the font FILE at `path` cover `cp`? Reads only its cmap. Cheap.
    [[nodiscard]] static bool file_covers(const std::string &path, char32_t cp);

    std::unordered_map<std::uint32_t, std::string> block_font_; // block -> font file
    std::vector<std::string> candidates_;                       // prioritised font files
    bool candidates_ready_ = false;
    bool cache_loaded_ = false;
    mutable bool dirty_ = false; // block_font_ changed since last flush
};

} // namespace toe::gfx

#endif // TOE_GFX_FONT_DISCOVERY_HPP
