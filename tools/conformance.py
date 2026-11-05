#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Differential conformance test: toe vs an independent reference (pyte).

Feeds the same VT byte stream to toe (via tools/toe_dump) and to pyte, then
diffs the resulting grid. Disagreement is not automatically a toe bug -- pyte
has its own gaps -- so every mismatch is reported for a human to adjudicate
against the spec rather than silently "fixed" toward the reference.

Usage:  tools/conformance.py /path/to/toe_dump
"""
import subprocess
import sys

try:
    import pyte
except ImportError:
    sys.exit("needs pyte: python3 -m pip install pyte")

COLS, ROWS = 20, 5

# (name, byte stream). Kept focused: each case isolates one behaviour so a
# mismatch points at a specific sequence.
CASES = [
    # --- cursor motion ---
    ("CUU",            "\n\n\nX\x1b[2Ay"),
    ("CUD",            "X\x1b[2By"),
    ("CUF",            "X\x1b[3Cy"),
    ("CUB",            "XYZ\x1b[2Dy"),
    ("CNL",            "ab\x1b[1Ex"),
    ("CPL",            "\n\nab\x1b[1Fx"),
    ("CHA",            "abcdef\x1b[3Gx"),
    ("CUP",            "\x1b[2;3Hx"),
    ("CUP defaults",   "abc\x1b[Hx"),
    ("HVP (f)",        "\x1b[2;3fx"),
    ("VPA",            "abc\x1b[3dx"),
    ("CUF clamps",     "\x1b[99Cx"),
    ("CUU at top",     "\x1b[5Ax"),
    # --- erase ---
    ("ED 0",           "abc\ndef\x1b[1;2H\x1b[0J"),
    ("ED 1",           "abc\ndef\x1b[2;2H\x1b[1J"),
    ("ED 2",           "abc\ndef\x1b[2J"),
    ("EL 0",           "abcdef\x1b[3G\x1b[0K"),
    ("EL 1",           "abcdef\x1b[3G\x1b[1K"),
    ("EL 2",           "abcdef\x1b[3G\x1b[2K"),
    ("ECH",            "abcdef\x1b[2G\x1b[3X"),
    # --- insert / delete ---
    ("ICH",            "abcdef\x1b[3G\x1b[2@"),
    ("DCH",            "abcdef\x1b[3G\x1b[2P"),
    ("IL",             "aaa\nbbb\nccc\x1b[2;1H\x1b[1L"),
    ("DL",             "aaa\nbbb\nccc\x1b[2;1H\x1b[1M"),
    # --- scrolling ---
    ("SU",             "aaa\nbbb\nccc\x1b[2S"),
    ("SD",             "aaa\nbbb\nccc\x1b[2T"),
    ("scroll region",  "\x1b[2;4r\x1b[2;1Hx\n\n\n\nY"),
    ("RI in region",   "\x1b[2;4r\x1b[2;1H\x1bMz"),
    ("IND",            "ab\x1bDc"),
    ("NEL",            "ab\x1bEc"),
    # --- wrap ---
    ("autowrap",       "x" * (COLS + 3)),
    ("DECAWM off",     "\x1b[?7l" + "x" * (COLS + 3)),
    ("wrap then CR",   "x" * COLS + "\ry"),
    # --- tabs ---
    ("HT",             "a\tb\tc"),
    ("CHT",            "a\x1b[2Ix"),
    ("CBT",            "\x1b[20G\x1b[2Zx"),
    ("TBC + HTS",      "\x1b[3g\x1b[5G\x1bH\x1b[1G\tx"),
    # --- save / restore ---
    ("DECSC/DECRC",    "\x1b[3;3H\x1b7\x1b[1;1Hab\x1b8x"),
    ("SCOSC/SCORC",    "\x1b[3;3H\x1b[s\x1b[1;1Hab\x1b[ux"),
    # --- charset / misc ---
    ("DECALN",         "\x1b#8"),
    ("REP",            "a\x1b[3bx"),
    ("DECSTR",         "\x1b[2;4r\x1b[!p\x1b[1;1Hx"),
    ("RIS",            "abc\x1bcx"),
    ("BS at col 0",    "\x08x"),
    ("CR LF",          "ab\r\ncd"),
    # --- control chars inside sequences (parser conformance) ---
    ("CAN aborts",     "a\x1b[3\x18Cb"),
    ("SUB aborts",     "a\x1b[3\x1aCb"),
    ("ESC re-starts",  "a\x1b[3\x1b[2Cb"),
    ("DEL ignored",    "a\x7fb"),
    ("NUL ignored",    "a\x00b"),
    ("bad final",      "a\x1b[3\x01Cb"),
    ("huge param",     "a\x1b[99999999999Cb"),
    ("many params",    "a\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17Cb"),
    ("empty params",   "a\x1b[;;;Cb"),
    ("colon subparams","\x1b[38:2:1:2:3max"),
    # --- OSC / DCS / strings must not leak into the grid ---
    ("OSC title",      "\x1b]0;hello\x07x"),
    ("OSC ST",         "\x1b]0;hello\x1b\\x"),
    ("OSC unterminated","\x1b]0;hello"),
    ("DCS skipped",    "\x1bPq#0;2;0;0;0\x1b\\x"),
    ("APC skipped",    "\x1b_Gf=100\x1b\\x"),
    ("PM skipped",     "\x1b^junk\x1b\\x"),
    ("SOS skipped",    "\x1bXjunk\x1b\\x"),
    # --- UTF-8 / widths ---
    ("utf8 2-byte",    "caf\xc3\xa9"),
    ("utf8 3-byte",    "a\xe2\x82\xacb"),
    ("CJK wide",       "a\xe4\xb8\xadb"),
    ("CJK at edge",    "x" * (COLS - 1) + "\xe4\xb8\xad"),
    ("combining",      "e\xcc\x81x"),
    ("invalid utf8",   "a\xffb"),
    ("overlong",       "a\xc0\x80b"),
    # --- alt screen ---
    ("alt screen",     "main\x1b[?1049halt\x1b[?1049l"),
    ("alt no history", "\x1b[?1049h" + "x\n" * 10 + "\x1b[?1049l"),
]


# Cases where toe and pyte disagree AND toe has been adjudicated correct
# against the spec (xterm's ctlseqs / DEC STD 070). Listed so the harness
# reports a clean signal and a real regression stands out, rather than hiding
# in a wall of known noise. Each entry says what pyte does instead.
ADJUDICATED = {
    "SU":              "pyte ignores CSI S (pan up) entirely",
    "SD":              "pyte ignores CSI T (pan down) entirely",
    "NEL":             "NEL moves to column 0; pyte keeps the column (LF semantics)",
    "CHT":             "pyte does not implement CSI I (cursor forward tab)",
    "CBT":             "pyte does not implement CSI Z (cursor backward tab)",
    "SCOSC/SCORC":     "pyte does not implement CSI s/u (SCO save/restore)",
    "REP":             "pyte does not implement CSI b (repeat last char)",
    "CAN aborts":      "CAN must abort the sequence; pyte prints the remainder",
    "SUB aborts":      "SUB must abort the sequence; pyte prints the remainder",
    "ESC re-starts":   "ESC mid-sequence restarts it; pyte prints the remainder",
    "bad final":       "a C0 byte mid-CSI is executed, not printed",
    "many params":     "pyte raises TypeError on 17 CSI params",
    "empty params":    "pyte raises TypeError on empty params",
    "colon subparams": "pyte does not parse SGR colon subparameters",
    "DCS skipped":     "DCS payload must not reach the grid; pyte prints it",
    "APC skipped":     "APC payload must not reach the grid; pyte prints it",
    "CJK at edge":     "a wide glyph with 1 column left wraps; pyte truncates it",
    "overlong":        "toe emits one U+FFFD per bad sequence (maximal subpart)",
    "alt screen":      "pyte does not implement 1049 alt-screen save/restore",
    "alt no history":  "pyte has no alt screen, so its scrollback differs",
    # pyte NORMALISES "e"+U+0301 to the precomposed U+00E9. A terminal must not
    # alter the application's text, so toe keeps the bytes the child sent
    # (NFD) and exposes the mark via Screen::marks_of.
    "combining":       "pyte normalises NFD to NFC; a terminal must not normalise",
}


def toe_grid(dump_bin, stream):
    out = subprocess.run([dump_bin, str(COLS), str(ROWS)],
                         input=stream.encode("latin-1"),
                         capture_output=True).stdout.decode("utf-8", "replace")
    return [l.rstrip() for l in out.split("\n")[:ROWS]]


def pyte_grid(stream):
    screen = pyte.Screen(COLS, ROWS)
    try:
        pyte.Stream(screen).feed(stream.encode("latin-1").decode("utf-8", "replace"))
    except Exception as e:  # pyte crashes on some valid-but-unusual input
        return ["<<pyte raised %s: %s>>" % (type(e).__name__, e)]
    return [l.rstrip() for l in screen.display]


def main():
    dump_bin = sys.argv[1] if len(sys.argv) > 1 else "/tmp/toe_dump"
    agree = []
    known = []
    unexpected = []
    for name, stream in CASES:
        t = toe_grid(dump_bin, stream)
        p = pyte_grid(stream)
        if t == p:
            agree.append(name)
        elif name in ADJUDICATED:
            known.append((name, t, p))
        else:
            unexpected.append((name, t, p))

    print(f"agree with reference:        {len(agree)}/{len(CASES)}")
    print(f"differ, toe adjudicated ok:  {len(known)}/{len(CASES)}")
    print(f"UNEXPECTED differences:      {len(unexpected)}/{len(CASES)}\n")

    for name, t, p in unexpected:
        print(f"--- {name}  (NEEDS ADJUDICATION)")
        if p and p[0].startswith("<<pyte raised"):
            print(f"    {p[0]}")
            print(f"    toe  = {t!r}")
            continue
        for i, (a, b) in enumerate(zip(t, p)):
            if a != b:
                print(f"    row{i} toe  = {a!r}")
                print(f"    row{i} pyte = {b!r}")

    if not unexpected:
        print("no unexplained disagreements with the reference")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
