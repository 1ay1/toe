// SPDX-License-Identifier: LGPL-2.0-or-later
//
// A minimal OpenType GSUB `calt`/`liga` shaper — just enough to render
// programming ligatures (=> != -> >= == === <=> |> :: ...) without HarfBuzz.
//
// Scope, deliberately narrow (this is a terminal, not a word processor):
//   * cmap format 4 + 12 (codepoint -> glyph id)
//   * GSUB features `calt` and `liga`
//   * Lookup Type 6 Format 3 (Chained Context) driving
//     Lookup Type 1 (Single) and Type 4 (Ligature) substitutions
//   * Coverage format 1 & 2
// This is exactly the subtable shape JetBrains Mono / Fira Code / Cascadia
// Code / Iosevka use. Fonts that ligate via other formats simply don't ligate
// here (graceful: the raw glyphs render) — nothing crashes on unknown data.
//
// The parser is bounds-checked against the font blob length; malformed or
// truncated tables abort the affected lookup rather than reading out of range.

#ifndef TOE_GFX_OPENTYPE_HPP
#define TOE_GFX_OPENTYPE_HPP

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace toe::gfx::ot {

// A read-only view over the raw font bytes with big-endian accessors that
// return 0 on out-of-range instead of reading past the end.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : d_(data) {}

    [[nodiscard]] std::size_t size() const noexcept { return d_.size(); }
    [[nodiscard]] bool ok(std::size_t off, std::size_t n) const noexcept {
        return off + n <= d_.size();
    }
    [[nodiscard]] std::uint16_t u16(std::size_t o) const noexcept {
        if (!ok(o, 2)) return 0;
        return static_cast<std::uint16_t>((d_[o] << 8) | d_[o + 1]);
    }
    [[nodiscard]] std::int16_t s16(std::size_t o) const noexcept {
        return static_cast<std::int16_t>(u16(o));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t o) const noexcept {
        if (!ok(o, 4)) return 0;
        return (static_cast<std::uint32_t>(d_[o]) << 24) | (static_cast<std::uint32_t>(d_[o + 1]) << 16) |
               (static_cast<std::uint32_t>(d_[o + 2]) << 8) | d_[o + 3];
    }
    [[nodiscard]] const std::uint8_t *ptr(std::size_t o) const noexcept { return d_.data() + o; }

private:
    std::span<const std::uint8_t> d_;
};

// The parsed shaping tables. Built once per font; shape() is called per run.
class Shaper {
public:
    // Parse the sfnt: locate cmap + GSUB. Returns false if the font has no
    // usable GSUB (ligatures unavailable; caller falls back to cmap-only).
    bool parse(std::span<const std::uint8_t> font);

    [[nodiscard]] bool has_gsub() const noexcept { return gsub_ != 0; }

    // codepoint -> glyph id via cmap (0 = .notdef / not present).
    [[nodiscard]] std::uint16_t glyph_for(char32_t cp) const noexcept;

    // Apply calt/liga to a run of glyph ids IN PLACE. Positions that a ligature
    // consumes are set to 0 (the renderer skips glyph 0), matching how a
    // ligature spans multiple cells with one visible glyph at the first cell.
    void shape(std::vector<std::uint16_t> &glyphs) const;

private:
    Reader r_{{}};
    std::size_t cmap_ = 0;     // offset of the chosen cmap subtable
    std::uint16_t cmap_fmt_ = 0;
    std::size_t gsub_ = 0;     // offset of the GSUB table
    std::vector<std::uint32_t> lookups_; // absolute offsets of each GSUB lookup
    std::vector<std::uint16_t> calt_lookups_; // indices of calt/liga lookups, in order

    // Coverage: returns the coverage index of `g`, or -1 if not covered.
    [[nodiscard]] int coverage_index(std::size_t cov, std::uint16_t g) const noexcept;
    // Apply one lookup (by index) at glyph position `pos`. Returns how many
    // input glyphs were consumed (>=1 on a substitution, 0 if it didn't apply).
    [[nodiscard]] std::size_t apply_lookup(std::uint16_t li, std::vector<std::uint16_t> &g,
                                           std::size_t pos) const;
    [[nodiscard]] std::size_t apply_subtable(std::uint16_t type, std::size_t sub,
                                             std::vector<std::uint16_t> &g, std::size_t pos) const;
};

// ---------------------------------------------------------------------------
// Implementation (header-only; small enough, and only one TU includes it).
// ---------------------------------------------------------------------------

inline bool Shaper::parse(std::span<const std::uint8_t> font) {
    r_ = Reader{font};
    // sfnt header: u32 tag, u16 numTables, ... then 16-byte table records.
    const std::uint16_t num = r_.u16(4);
    std::size_t cmap_tab = 0, gsub_tab = 0;
    for (std::uint16_t i = 0; i < num; ++i) {
        const std::size_t rec = 12 + 16u * i;
        if (!r_.ok(rec, 16)) break;
        const std::uint32_t tag = r_.u32(rec);
        const std::uint32_t off = r_.u32(rec + 8);
        if (tag == 0x636d6170u) cmap_tab = off;      // 'cmap'
        else if (tag == 0x47535542u) gsub_tab = off; // 'GSUB'
    }

    // --- pick a Unicode cmap subtable (prefer format 12, then 4) ---
    if (cmap_tab) {
        const std::uint16_t nsub = r_.u16(cmap_tab + 2);
        std::size_t best4 = 0, best12 = 0;
        for (std::uint16_t i = 0; i < nsub; ++i) {
            const std::size_t e = cmap_tab + 4 + 8u * i;
            const std::uint16_t plat = r_.u16(e), enc = r_.u16(e + 2);
            const std::size_t sub = cmap_tab + r_.u32(e + 4);
            const bool unicode = (plat == 0) || (plat == 3 && (enc == 1 || enc == 10));
            if (!unicode) continue;
            const std::uint16_t fmt = r_.u16(sub);
            if (fmt == 12) best12 = sub;
            else if (fmt == 4 && !best4) best4 = sub;
        }
        if (best12) { cmap_ = best12; cmap_fmt_ = 12; }
        else if (best4) { cmap_ = best4; cmap_fmt_ = 4; }
    }

    if (!gsub_tab) return false;
    gsub_ = gsub_tab;

    // GSUB header: u16 major, minor, u16 scriptListOff, featureListOff, lookupListOff.
    const std::size_t featOff = gsub_ + r_.u16(gsub_ + 6);
    const std::size_t lookOff = gsub_ + r_.u16(gsub_ + 8);

    // Collect all lookup offsets.
    const std::uint16_t nlook = r_.u16(lookOff);
    lookups_.reserve(nlook);
    for (std::uint16_t i = 0; i < nlook; ++i)
        lookups_.push_back(static_cast<std::uint32_t>(lookOff + r_.u16(lookOff + 2 + 2u * i)));

    // Features named calt / liga -> the lookup indices they drive, kept in
    // feature order (approximates the correct application order for our subset).
    const std::uint16_t nfeat = r_.u16(featOff);
    for (std::uint16_t i = 0; i < nfeat; ++i) {
        const std::size_t rec = featOff + 2 + 6u * i;
        const std::uint32_t tag = r_.u32(rec);
        if (tag != 0x63616c74u && tag != 0x6c696761u) continue; // 'calt' / 'liga'
        const std::size_t f = featOff + r_.u16(rec + 4);
        const std::uint16_t nl = r_.u16(f + 2);
        for (std::uint16_t j = 0; j < nl; ++j) calt_lookups_.push_back(r_.u16(f + 4 + 2u * j));
    }
    return !calt_lookups_.empty();
}

inline std::uint16_t Shaper::glyph_for(char32_t cp) const noexcept {
    if (!cmap_) return 0;
    if (cmap_fmt_ == 4) {
        const std::size_t s = cmap_;
        const std::uint16_t segX2 = r_.u16(s + 6);
        const std::uint16_t segc = segX2 / 2;
        const std::size_t endO = s + 14;
        const std::size_t startO = endO + segX2 + 2;
        const std::size_t deltaO = startO + segX2;
        const std::size_t rangeO = deltaO + segX2;
        if (cp > 0xFFFF) return 0;
        for (std::uint16_t i = 0; i < segc; ++i) {
            if (cp <= r_.u16(endO + 2u * i)) {
                const std::uint16_t start = r_.u16(startO + 2u * i);
                if (cp < start) return 0;
                const std::int16_t delta = r_.s16(deltaO + 2u * i);
                const std::uint16_t rangeOff = r_.u16(rangeO + 2u * i);
                if (rangeOff == 0)
                    return static_cast<std::uint16_t>((cp + delta) & 0xFFFF);
                const std::size_t gi = rangeO + 2u * i + rangeOff +
                                       2u * (static_cast<std::uint16_t>(cp) - start);
                const std::uint16_t g = r_.u16(gi);
                return g ? static_cast<std::uint16_t>((g + delta) & 0xFFFF) : 0;
            }
        }
        return 0;
    }
    if (cmap_fmt_ == 12) {
        const std::size_t s = cmap_;
        const std::uint32_t ngroups = r_.u32(s + 12);
        for (std::uint32_t i = 0; i < ngroups; ++i) {
            const std::size_t grp = s + 16 + 12u * i;
            const std::uint32_t lo = r_.u32(grp), hi = r_.u32(grp + 4);
            if (static_cast<std::uint32_t>(cp) >= lo && static_cast<std::uint32_t>(cp) <= hi)
                return static_cast<std::uint16_t>(r_.u32(grp + 8) + (static_cast<std::uint32_t>(cp) - lo));
        }
        return 0;
    }
    return 0;
}

inline int Shaper::coverage_index(std::size_t cov, std::uint16_t g) const noexcept {
    const std::uint16_t fmt = r_.u16(cov);
    if (fmt == 1) {
        const std::uint16_t n = r_.u16(cov + 2);
        for (std::uint16_t i = 0; i < n; ++i)
            if (r_.u16(cov + 4 + 2u * i) == g) return i;
        return -1;
    }
    if (fmt == 2) {
        const std::uint16_t n = r_.u16(cov + 2);
        for (std::uint16_t i = 0; i < n; ++i) {
            const std::size_t rec = cov + 4 + 6u * i;
            const std::uint16_t start = r_.u16(rec), end = r_.u16(rec + 2), sci = r_.u16(rec + 4);
            if (g >= start && g <= end) return sci + (g - start);
        }
        return -1;
    }
    return -1;
}

// Single substitution (Type 1): replace one glyph with another in place.
inline std::size_t Shaper::apply_subtable(std::uint16_t type, std::size_t sub,
                                          std::vector<std::uint16_t> &g, std::size_t pos) const {
    const std::uint16_t fmt = r_.u16(sub);
    if (type == 1) {
        const std::size_t cov = sub + r_.u16(sub + 2);
        const int ci = coverage_index(cov, g[pos]);
        if (ci < 0) return 0;
        if (fmt == 1) {
            const std::int16_t delta = r_.s16(sub + 4);
            g[pos] = static_cast<std::uint16_t>((g[pos] + delta) & 0xFFFF);
        } else { // fmt 2: array of substitute glyphs
            g[pos] = r_.u16(sub + 6 + 2u * static_cast<std::size_t>(ci));
        }
        return 1;
    }
    if (type == 4) { // Ligature substitution
        const std::size_t cov = sub + r_.u16(sub + 2);
        const int ci = coverage_index(cov, g[pos]);
        if (ci < 0) return 0;
        const std::uint16_t nsets = r_.u16(sub + 4);
        if (ci >= nsets) return 0;
        const std::size_t setOff = sub + r_.u16(sub + 6 + 2u * static_cast<std::size_t>(ci));
        const std::uint16_t ncnt = r_.u16(setOff);
        for (std::uint16_t i = 0; i < ncnt; ++i) {
            const std::size_t lig = setOff + r_.u16(setOff + 2 + 2u * i);
            const std::uint16_t out = r_.u16(lig);
            const std::uint16_t comp = r_.u16(lig + 2); // includes the first glyph
            if (pos + comp > g.size()) continue;
            bool match = true;
            for (std::uint16_t c = 1; c < comp; ++c)
                if (g[pos + c] != r_.u16(lig + 4 + 2u * (c - 1))) { match = false; break; }
            if (!match) continue;
            g[pos] = out;                               // ligature glyph at the first cell
            for (std::uint16_t c = 1; c < comp; ++c) g[pos + c] = 0; // spacers
            return comp;
        }
        return 0;
    }
    return 0;
}

inline std::size_t Shaper::apply_lookup(std::uint16_t li, std::vector<std::uint16_t> &g,
                                        std::size_t pos) const {
    if (li >= lookups_.size()) return 0;
    const std::size_t lk = lookups_[li];
    const std::uint16_t type = r_.u16(lk);
    const std::uint16_t nsub = r_.u16(lk + 4);

    // Type 6: Chained Contexts Substitution. We handle Format 3 (the one
    // programming fonts use): backtrack/input/lookahead coverage arrays + a
    // set of (sequenceIndex, lookupIndex) actions applied on a match.
    if (type == 6) {
        for (std::uint16_t s = 0; s < nsub; ++s) {
            const std::size_t sub = lk + r_.u16(lk + 6 + 2u * s);
            if (r_.u16(sub) != 3) continue; // only format 3
            std::size_t o = sub + 2;
            const std::uint16_t btc = r_.u16(o); o += 2;
            std::size_t btArr = o; o += 2u * btc;
            const std::uint16_t inc = r_.u16(o); o += 2;
            std::size_t inArr = o; o += 2u * inc;
            const std::uint16_t lac = r_.u16(o); o += 2;
            std::size_t laArr = o; o += 2u * lac;
            const std::uint16_t actc = r_.u16(o); o += 2;
            const std::size_t actArr = o;

            if (inc == 0 || pos + inc > g.size()) continue;
            // Input coverage: g[pos + k] must be covered by input[k].
            bool ok = true;
            for (std::uint16_t k = 0; k < inc && ok; ++k) {
                const std::size_t cov = sub + r_.u16(inArr + 2u * k);
                if (coverage_index(cov, g[pos + k]) < 0) ok = false;
            }
            if (!ok) continue;
            // Backtrack: g[pos-1-k] covered by backtrack[k].
            for (std::uint16_t k = 0; k < btc && ok; ++k) {
                if (pos < static_cast<std::size_t>(k) + 1) { ok = false; break; }
                const std::size_t cov = sub + r_.u16(btArr + 2u * k);
                if (coverage_index(cov, g[pos - 1 - k]) < 0) ok = false;
            }
            if (!ok) continue;
            // Lookahead: g[pos+inc+k] covered by lookahead[k].
            for (std::uint16_t k = 0; k < lac && ok; ++k) {
                const std::size_t idx = pos + inc + k;
                if (idx >= g.size()) { ok = false; break; }
                const std::size_t cov = sub + r_.u16(laArr + 2u * k);
                if (coverage_index(cov, g[idx]) < 0) ok = false;
            }
            if (!ok) continue;

            // Matched: run the substitution actions.
            std::size_t consumed = inc;
            for (std::uint16_t a = 0; a < actc; ++a) {
                const std::uint16_t seqIdx = r_.u16(actArr + 4u * a);
                const std::uint16_t lookIdx = r_.u16(actArr + 4u * a + 2);
                if (pos + seqIdx < g.size()) (void)apply_lookup(lookIdx, g, pos + seqIdx);
            }
            return consumed; // advance past the matched input
        }
        return 0;
    }

    // Directly-referenced Type 1 / Type 4 (called from a Type 6 action).
    for (std::uint16_t s = 0; s < nsub; ++s) {
        const std::size_t sub = lk + r_.u16(lk + 6 + 2u * s);
        if (const std::size_t c = apply_subtable(type, sub, g, pos)) return c;
    }
    return 0;
}

inline void Shaper::shape(std::vector<std::uint16_t> &glyphs) const {
    if (glyphs.empty() || calt_lookups_.empty()) return;
    for (std::uint16_t li : calt_lookups_) {
        std::size_t pos = 0;
        while (pos < glyphs.size()) {
            const std::size_t consumed = apply_lookup(li, glyphs, pos);
            pos += consumed ? consumed : 1;
        }
    }
}

} // namespace toe::gfx::ot

#endif // TOE_GFX_OPENTYPE_HPP
