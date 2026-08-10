// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Fast, correct codepoint display width — a GENERATED two-stage table (NOT libc
// wcwidth, which is locale-dependent, slow, and often out of date).
//
// The table is derived from the authoritative Unicode Character Database
// (EastAsianWidth + emoji-data + UnicodeData general categories) by
// tools/gen_width.py, so it needs no hand-maintained ranges and stays correct
// as Unicode evolves. Lookup is O(1) and branch-free (two array indexings into
// a ~11 KB table), replacing the older O(log n) double binary search.
//
// Width is 0 (combining / zero-width / controls), 2 (East-Asian wide/fullwidth
// + emoji-presentation), or 1 (everything else, incl. EAW-Ambiguous which we
// render narrow, as alacritty/kitty do).

#ifndef TOE_TERM_WIDTH_HPP
#define TOE_TERM_WIDTH_HPP

#include "toe/term/width_table.hpp"

namespace toe::term {

// Display width of a codepoint: 0, 1, or 2. ASCII printables fast-path to 1
// without touching the table (the overwhelmingly common case under any flood).
[[nodiscard]] inline int codepoint_width(char32_t cp) noexcept {
    if (cp - 0x20u < 0x5Fu) return 1;   // 0x20..0x7E printable ASCII
    return wtab::width_of(cp);
}

} // namespace toe::term

#endif // TOE_TERM_WIDTH_HPP
