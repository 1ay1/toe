#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Generate toe/term/width_table.hpp: a two-stage lookup table mapping every
Unicode codepoint (0..0x10FFFF) to its terminal cell width (0/1/2), derived from
the authoritative UCD data files (NOT hand-typed ranges).

Width rules (matching wcwidth/alacritty/kitty consensus):
  * 0  = combining/enclosing marks (Mn, Me), format chars (Cf) except a few that
         advance, zero-width space/joiner, and the C0/C1 control range.
  * 2  = East-Asian Wide (W) + Fullwidth (F), plus Emoji_Presentation codepoints.
  * 1  = everything else (incl. East-Asian Ambiguous, which we render narrow).

Two-stage table: cp >> SHIFT indexes stage1 -> block id; (block<<SHIFT)|(cp&MASK)
indexes stage2 -> 2-bit width packed 4-per-byte. O(1), branch-free, cache-tiny
(dedup'd identical blocks). This replaces the O(log n) double binary search.

Run:  python3 tools/gen_width.py /path/to/ucd  include/toe/term/width_table.hpp
UCD dir must contain: EastAsianWidth.txt UnicodeData.txt emoji-data.txt
"""
import sys, os, re

MAXCP = 0x110000

def ranges(path):
    """Yield (lo, hi, prop) from a UCD file with `A..B ; Prop` or `A ; Prop`."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line or ";" not in line:
                continue
            cps, prop = line.split(";", 1)[0].strip(), line.split(";", 1)[1].strip().split()[0]
            if ".." in cps:
                lo, hi = cps.split("..")
            else:
                lo = hi = cps
            yield int(lo, 16), int(hi, 16), prop

def build(ucd):
    width = bytearray(1 for _ in range(MAXCP))  # default 1

    # 1) East-Asian Wide (W) + Fullwidth (F) -> width 2.
    for lo, hi, p in ranges(os.path.join(ucd, "EastAsianWidth.txt")):
        if p in ("W", "F"):
            for c in range(lo, min(hi + 1, MAXCP)):
                width[c] = 2

    # 2) Emoji_Presentation -> width 2 (default-emoji glyphs render double-width).
    for lo, hi, p in ranges(os.path.join(ucd, "emoji-data.txt")):
        if p == "Emoji_Presentation":
            for c in range(lo, min(hi + 1, MAXCP)):
                width[c] = 2

    # 3) Zero-width: combining/enclosing marks (Mn, Me) + format (Cf). Parse the
    #    general category from UnicodeData.txt (handles First/Last range rows).
    def set_zero(lo, hi):
        for c in range(lo, min(hi + 1, MAXCP)):
            width[c] = 0
    prev = None
    with open(os.path.join(ucd, "UnicodeData.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(";")
            if len(parts) < 3:
                continue
            cp = int(parts[0], 16)
            name, cat = parts[1], parts[2]
            if name.endswith(", First>"):
                prev = (cp, cat)
                continue
            if name.endswith(", Last>") and prev:
                lo, pcat = prev
                if pcat in ("Mn", "Me", "Cf"):
                    set_zero(lo, cp)
                prev = None
                continue
            if cat in ("Mn", "Me", "Cf"):
                width[cp] = 0

    # 4) Fix-ups that the categories get "wrong" for terminals:
    #    - C0/C1 controls -> 0 (they occupy no cell).
    for c in range(0x00, 0x20): width[c] = 0
    for c in range(0x7F, 0xA0): width[c] = 0
    #    - Soft hyphen (00AD, Cf) is width 1 in practice.
    width[0x00AD] = 1
    #    - ZWSP/ZWNJ/ZWJ family already 0 via Cf.
    #    - The BOM/word-joiner 0x2060..0x2064, 0xFEFF are Cf -> 0 (correct).
    #    - Hangul Jamo medial/final (1160..11FF, D7B0..D7FF) are 0-width in EAW?
    #      They are category Lo but combine; wcwidth treats them as 0.
    for c in range(0x1160, 0x1200): width[c] = 0
    for c in range(0xD7B0, 0xD800): width[c] = 0
    #    - NUL renders nothing.
    width[0] = 0
    return width

def gen(width, out):
    # Choose a block shift. 8 keeps blocks 256 wide; good dedup + small stage1.
    SHIFT = 8
    MASK = (1 << SHIFT) - 1
    BLK = 1 << SHIFT
    nblocks = MAXCP >> SHIFT

    # Pack each 256-cp block's widths into a tuple; dedup identical blocks.
    stage1 = [0] * nblocks
    blocks = []
    seen = {}
    for b in range(nblocks):
        key = bytes(width[b * BLK:(b + 1) * BLK])
        if key not in seen:
            seen[key] = len(blocks)
            blocks.append(key)
        stage1[b] = seen[key]

    # Pack stage2 widths 4-per-byte (2 bits each) to shrink the table further.
    stage2 = bytearray()
    for blk in blocks:
        for i in range(0, BLK, 4):
            v = (blk[i] & 3) | ((blk[i+1] & 3) << 2) | ((blk[i+2] & 3) << 4) | ((blk[i+3] & 3) << 6)
            stage2.append(v)
    stage1_type = "std::uint16_t" if len(blocks) > 256 else "std::uint8_t"

    def arr(name, data, per=16, t="std::uint8_t"):
        o = [f"inline constexpr {t} {name}[{len(data)}] = {{"]
        for i in range(0, len(data), per):
            o.append("    " + ",".join(str(x) for x in data[i:i+per]) + ",")
        o.append("};")
        return "\n".join(o)

    hdr = f"""// SPDX-License-Identifier: LGPL-2.0-or-later
//
// GENERATED by tools/gen_width.py from the Unicode Character Database
// (Unicode 17.0.0). Do not edit by hand; re-run the generator to update.
//
// Two-stage cell-width table: {len(blocks)} unique 256-cp blocks, widths packed
// 2 bits/codepoint (0/1/2). Lookup is O(1), branch-free.

#ifndef TOE_TERM_WIDTH_TABLE_HPP
#define TOE_TERM_WIDTH_TABLE_HPP

#include <cstdint>

namespace toe::term::wtab {{

inline constexpr unsigned kShift = {SHIFT};
inline constexpr unsigned kMask = {MASK};

{arr("kStage1", stage1, per=16, t=stage1_type)}

{arr("kStage2", list(stage2), per=20)}

// Width of a codepoint in [0,2]. Codepoints past the last plane are width 1.
[[nodiscard]] inline int width_of(char32_t cp) noexcept {{
    if (cp >= 0x{MAXCP:X}u) return 1;
    const unsigned blk = kStage1[cp >> kShift];
    const unsigned idx = (blk << (kShift - 2)) + ((cp & kMask) >> 2);
    const unsigned shift = (cp & 3u) * 2u;
    return (kStage2[idx] >> shift) & 3u;
}}

}} // namespace toe::term::wtab

#endif // TOE_TERM_WIDTH_TABLE_HPP
"""
    with open(out, "w") as f:
        f.write(hdr)
    kb = (len(stage1) * (2 if len(blocks) > 256 else 1) + len(stage2)) / 1024
    print(f"wrote {out}: {len(blocks)} blocks, table ~{kb:.1f} KB")

if __name__ == "__main__":
    ucd = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ucd"
    out = sys.argv[2] if len(sys.argv) > 2 else "include/toe/term/width_table.hpp"
    gen(build(ucd), out)
