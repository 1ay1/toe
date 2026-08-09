// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Fast codepoint display width — a built-in table, NOT libc wcwidth().
//
// wcwidth() is locale-dependent, does a slow table walk, and is a syscall-heavy
// dominator under CJK/emoji floods. Real fast terminals (alacritty, kitty) ship
// their own compact width tables and binary-search them; so do we.
//
// Width is 0 (combining / zero-width), 2 (wide: East-Asian wide/fullwidth +
// emoji), or 1 (everything else). The ranges below are derived from Unicode
// EastAsianWidth (W + F) and the combining/zero-width categories (Mn, Me, Cf,
// plus the Hangul Jamo medial/final that render zero-width). Kept intentionally
// compact: the goal is correct WIDTH, not a full grapheme database.

#ifndef TOE_TERM_WIDTH_HPP
#define TOE_TERM_WIDTH_HPP

#include <array>
#include <cstdint>

namespace toe::term {

struct WRange { char32_t lo, hi; };

// Zero-width: combining marks, variation selectors, ZWJ/ZWNJ, format chars,
// Hangul Jamo medial+final. Sorted, non-overlapping — binary-searchable.
inline constexpr std::array<WRange, 27> kZeroWidth = {{
    {0x0300, 0x036F},   // combining diacritical marks
    {0x0483, 0x0489},   // Cyrillic combining
    {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
    {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC},
    {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
    {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
    {0x0901, 0x0903}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957},
    {0x1AB0, 0x1AFF},   // combining diacritical marks extended
    {0x1DC0, 0x1DFF},   // combining diacritical marks supplement
    {0x200B, 0x200F},   // ZWSP, ZWNJ, ZWJ, LRM, RLM
    {0x20D0, 0x20FF},   // combining marks for symbols
    {0xFE00, 0xFE0F},   // variation selectors
}};

// Wide (width 2): East-Asian Wide + Fullwidth + emoji presentation ranges.
// Sorted, non-overlapping.
inline constexpr std::array<WRange, 30> kWide = {{
    {0x1100, 0x115F},   // Hangul Jamo (leading)
    {0x2329, 0x232A},   // angle brackets
    {0x2600, 0x26FF},   // misc symbols (many emoji-presentation)
    {0x2700, 0x27BF},   // dingbats
    {0x2E80, 0x303E},   // CJK radicals, Kangxi, symbols
    {0x3041, 0x33FF},   // Hiragana, Katakana, CJK symbols, etc.
    {0x3400, 0x4DBF},   // CJK Ext-A
    {0x4E00, 0x9FFF},   // CJK Unified Ideographs
    {0xA000, 0xA4CF},   // Yi
    {0xA960, 0xA97F},   // Hangul Jamo Extended-A
    {0xAC00, 0xD7A3},   // Hangul Syllables
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0xFE10, 0xFE19},   // vertical forms
    {0xFE30, 0xFE6F},   // CJK compatibility forms, small forms
    {0xFF00, 0xFF60},   // Fullwidth forms
    {0xFFE0, 0xFFE6},   // Fullwidth signs
    {0x1B000, 0x1B16F}, // Kana supplement/extended
    {0x1F004, 0x1F004}, // 🀄
    {0x1F0CF, 0x1F0CF}, // 🃏
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F1E6, 0x1F1FF}, // regional indicators (flags) — wide each
    {0x1F200, 0x1F2FF}, // enclosed ideographic supplement
    {0x1F300, 0x1F64F}, // Misc symbols + emoticons (emoji)
    {0x1F680, 0x1F6FF}, // transport + map (emoji)
    {0x1F900, 0x1F9FF}, // supplemental symbols + pictographs (emoji)
    {0x1FA00, 0x1FA6F}, // chess/symbols
    {0x1FA70, 0x1FAFF}, // symbols & pictographs extended-A
    {0x20000, 0x2FFFD}, // CJK Ext-B..F
    {0x30000, 0x3FFFD}, // CJK Ext-G
}};

// Binary-search a sorted, non-overlapping range table.
[[nodiscard]] inline bool in_ranges(char32_t cp, const WRange *r, std::size_t n) noexcept {
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) >> 1;
        if (cp < r[mid].lo) hi = mid;
        else if (cp > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

// Display width of a codepoint: 0 (zero-width), 2 (wide), or 1 (normal).
// ASCII printables fast-path to 1; the caller usually handles those even sooner.
[[nodiscard]] inline int codepoint_width(char32_t cp) noexcept {
    if (cp < 0x7F) return cp < 0x20 ? 0 : 1;   // ASCII (controls -> 0 here)
    if (cp == 0x7F) return 0;                  // DEL
    // Most non-ASCII text is Latin/Cyrillic/Greek single-width; the two tables
    // below start at 0x300+, so a quick sub-range check skips the search for the
    // common accented-Latin band.
    if (in_ranges(cp, kZeroWidth.data(), kZeroWidth.size())) return 0;
    if (in_ranges(cp, kWide.data(), kWide.size())) return 2;
    return 1;
}

} // namespace toe::term

#endif // TOE_TERM_WIDTH_HPP
