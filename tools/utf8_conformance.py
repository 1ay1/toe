#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Exhaustive UTF-8 decoder conformance: toe vs Python's own codec.

Python's UTF-8 decoder with errors="replace" implements the Unicode standard's
well-formed byte-sequence definition (Table 3-7) including overlong, surrogate
and out-of-range rejection. That makes it a strong oracle.

We only compare the SET of codepoints produced, not the count of U+FFFD, since
the standard permits a decoder latitude in how many replacements it emits for a
given bad run ("maximal subpart" is recommended, not required). What must never
happen: a malformed sequence decoding to a real codepoint, or a valid sequence
failing to decode.

Usage:  tools/utf8_conformance.py /path/to/toe_dump
"""
import itertools
import subprocess
import sys

COLS = 40


def toe_text(dump_bin, raw):
    out = subprocess.run([dump_bin, str(COLS), "1"], input=raw,
                         capture_output=True).stdout
    return out.split(b"\n")[0].rstrip(b" ")


def py_text(raw):
    return raw.decode("utf-8", "replace").rstrip(" ").encode("utf-8")


def norm(b):
    """Collapse runs of U+FFFD so replacement-count latitude isn't a failure."""
    s = b.decode("utf-8", "replace")
    out = []
    for ch in s:
        if ch == "\ufffd" and out and out[-1] == "\ufffd":
            continue
        out.append(ch)
    return "".join(out)


def main():
    dump_bin = sys.argv[1] if len(sys.argv) > 1 else "/tmp/toe_dump"
    bad = []
    checked = 0

    # 1. Every 1- and 2-byte sequence (exhaustive). 0x7F is DEL, which a
    #    terminal ignores rather than prints, so it's not a decoder question.
    seqs = [bytes([a]) for a in range(0x20, 0x100) if a != 0x7F]
    seqs += [bytes([a, b]) for a in range(0xC0, 0x100) for b in range(0x80, 0xC0)]

    # 2. All 3-byte sequences with structurally plausible leads, sampling the
    #    third byte (the second byte is where the interesting rejections are).
    for a in range(0xE0, 0xF0):
        for b in range(0x80, 0xC0):
            for c in (0x80, 0xA0, 0xBF):
                seqs.append(bytes([a, b, c]))

    # 3. 4-byte sequences across the F0..F7 lead range.
    for a in range(0xF0, 0xF8):
        for b in (0x80, 0x8F, 0x90, 0xBF):
            for c in (0x80, 0xBF):
                seqs.append(bytes([a, b, c, 0x80]))

    # 4. Known-tricky published vectors (Markus Kuhn's UTF-8 stress cases).
    seqs += [
        b"\xc0\x80", b"\xc1\xbf", b"\xe0\x80\x80", b"\xe0\x9f\xbf",
        b"\xf0\x80\x80\x80", b"\xf0\x8f\xbf\xbf",      # overlong
        b"\xed\xa0\x80", b"\xed\xbf\xbf",              # surrogates
        b"\xf4\x90\x80\x80", b"\xf7\xbf\xbf\xbf",      # > U+10FFFF
        b"\xf8\x88\x80\x80\x80", b"\xfc\x84\x80\x80\x80\x80",  # 5/6-byte
        b"\xfe", b"\xff", b"\xfe\xfe\xff\xff",
        b"\xc2", b"\xe2\x82", b"\xf0\x9f\x98",         # truncated
        b"\x80", b"\xbf", b"\x80\xbf",                 # stray continuations
        b"\xc2\xa9", b"\xe4\xb8\xad", b"\xf0\x9f\x98\x80",  # valid
        b"\xed\x9f\xbf", b"\xee\x80\x80", b"\xf4\x8f\xbf\xbf",  # valid edges
    ]

    for seq in seqs:
        raw = b"[" + seq + b"]"
        t, p = norm(toe_text(dump_bin, raw)), norm(py_text(raw))
        checked += 1
        if t != p:
            bad.append((seq, t, p))

    print(f"checked {checked} sequences")
    print(f"mismatches: {len(bad)}")
    for seq, t, p in bad[:40]:
        print(f"  {seq.hex(' '):<20} toe={t!r:<14} python={p!r}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
