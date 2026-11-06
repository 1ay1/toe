# Conformance

What toe implements, how that was verified, and what it deliberately does not
do. The claims here are checked by the test suite and the two differential
harnesses in `tools/`; anything not verified is listed as not verified.

## How it's verified

Three independent methods, because asserting against my own reading of the
code only finds the bugs I already thought of.

**1. Differential testing against another implementation.**
`tools/conformance.py` feeds the same byte stream to toe (via
`tools/toe_dump`) and to [pyte](https://github.com/selectel/pyte), an
unrelated terminal emulator, then diffs the resulting grid.

```sh
python3 -m pip install pyte
c++ -std=c++23 -I include tools/toe_dump.cpp build/src/libtoe.a -o /tmp/toe_dump
tools/conformance.py /tmp/toe_dump
```

Disagreement is not automatically a toe bug — pyte has its own gaps — so every
mismatch is adjudicated by hand against xterm's `ctlseqs` and DEC STD 070, and
recorded in the harness with the reason. Current state: 51/71 agree, 20
adjudicated in toe's favour, **0 unexplained**. A new disagreement fails the
run, so a regression can't hide in known noise.

**2. Exhaustive testing against a reference decoder.**
`tools/utf8_conformance.py` checks the UTF-8 decoder against Python's codec
over **7482 byte sequences**: every 1- and 2-byte sequence, all 3-byte
sequences with plausible leads, the F0..F7 4-byte range, and Markus Kuhn's
published stress vectors. Currently 0 mismatches.

**3. Sanitizers over adversarial input.**
A terminal parses bytes from an untrusted child, so an out-of-bounds cell
write is a security bug. `tests/fuzz_test.cpp` drives the model with random
bytes, escape-biased random bytes, and 20 adversarial shapes (5000 CSI
parameters, 200 KB OSC/DCS/APC strings, unterminated strings, ESC/CSI storms,
out-of-range margins and rectangles, margin-confined edits, 5000 combining
marks), each fed both in one chunk and byte-at-a-time, plus randomised resize,
boundary-straddling reflow, wide glyphs across a rewrap, and a 1×1 terminal.

After every step it re-checks the model's invariants — cursor in range, rows
the right width, and every double-width glyph still paired with its spacer.
That last one is what caught three separate reflow bugs that no crash would
have revealed.

```sh
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DTOE_SANITIZE=ON
cmake --build build-san -j && ctest --test-dir build-san
```

The whole suite (16 binaries, ~613 assertions) passes under
AddressSanitizer + UndefinedBehaviorSanitizer.

**4. Coverage-guided probing.**
Untested code is where bugs live, so `gcov` was used to find what the
assertions never execute, and each dead region was then probed by hand against
the spec. That is how the left/right-margin and rectangle-attribute bugs were
found: those paths had **zero** test coverage, and every one of them was wrong.

```sh
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage -O0" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-cov -j && ctest --test-dir build-cov
cd build-cov/src/CMakeFiles/toe.dir/term && gcov *.gcno
```

Current line coverage: `screen.cpp` 90%, `graphics.cpp` 98%, `parser.inl` 97%,
`rowring.hpp` 97%, `update.cpp` 86%, `sbquery.cpp` 85%, `redact.cpp` 100%.

## Implemented

### Parsing
Williams DEC state machine. CAN/SUB abort a sequence; ESC mid-sequence
restarts it; C0 controls are executed mid-sequence rather than printed; DEL is
ignored; parameters saturate rather than overflow; OSC/DCS/APC/PM/SOS payloads
never reach the grid, terminated or not.

### Cursor and editing
CUU CUD CUF CUB CNL CPL CHA CUP HVP VPA CHT CBT HTS TBC · IND RI NEL ·
ICH DCH IL DL ECH EL ED · REP · DECSC/DECRC and SCOSC/SCORC · DECALN ·
scroll regions (DECSTBM) with left/right margins (DECSLRM under DECLRMM) ·
origin mode (DECOM) · SU SD · rectangles: DECFRA DECERA DECCARA DECRARA
DECRQCRA (checksum).

Edge rules that are tested rather than assumed: an inverted or single-line
DECSTBM is **ignored**, not clamped; CUU/CUD stop at the region edge when the
cursor starts inside it but are not trapped by it from outside; CUF/CUB are
bounded by the margins the same way; DECOM confines the cursor and homes it on
both set and reset; DECSTBM and DECSLRM both home to "column 1, line 1 of the
page" (the VT510 manual's wording for both).

Left/right margins (DECSLRM under DECLRMM) bound the cursor only *while it is
within them*, per DEC STD 070 — a write starting left of the left margin is not
clipped by it, and columns beyond the right margin stay reachable. IL, DL,
ICH, DCH, DECBI and DECFI shift only the margin columns, leaving the cells
outside them untouched.

### Text and colour
SGR including 256-colour and truecolour. Both wire forms of 38/48/58 are
handled: the legacy `38;2;R;G;B` and the ECMA-48 `38:2::R:G:B` that kitty,
Neovim and iTerm2 actually send, including the empty colour-space slot that
makes the colon form one parameter longer. Underline colour (58/59), underline
styles (4:1 single, 4:2 double, 4:3 curly, 4:4 dotted, 4:5 dashed, 21 double),
bold/faint/italic/blink/reverse/hidden/strike/overline · the 16/256 palette
with OSC 4/104 and OSC 10/11/12 dynamic colours · DEC line attributes (double
width/height) · the DEC line-drawing charset with G0/G1 designation and SO/SI.

A truncated extended colour (`38;2;1`, `38:2::1`, bare `38`) leaves the pen
alone rather than reading past the parameter list.

### Modes
DECCKM DECOM DECAWM DECTCEM DECLRMM · alternate screen (47, 1047, 1049) ·
bracketed paste (2004) · focus reporting (1004) · mouse (1000, 1002, 1003)
with SGR encoding (1006) · synchronized output (DEC 2026) · DECSTR soft reset
and RIS.

### Queries
DA1 DA2 DA3 · DSR 5/6 · DECRQM (private) · DECRQSS · XTVERSION · XTGETTCAP ·
XTWINOPS 18 (report size).

Capability reporting is held to a rule: **toe must not lie to applications.**
Everything DA1 advertises is implemented, every mode in the DECRQM switch is
backed by real state, and an unknown mode reports "not recognised" (0) rather
than "reset" (2). `tests/caps_test.cpp` enforces this, because the failure mode
is invisible — an app silently falls back to a worse path. DECRQM reporting
2026 as unrecognised while synchronized output worked was exactly this bug.

### Unicode
UTF-8 validated per Unicode Table 3-7: overlong encodings, surrogates and
anything above U+10FFFF become U+FFFD. Display widths come from a generated
Unicode Character Database table (not locale-dependent libc `wcwidth`). Wide
glyphs occupy a lead cell plus a zero-width spacer, kept consistent when
either half is overwritten, and wrap rather than truncate at the right edge.
That pair also survives a rewrap: reflow never splits a glyph across rows,
never orphans a lead, and never loses one. Soft-wrap reflow preserves a
logical line even when it STRADDLES the scrollback boundary (part in history,
part in the live grid), so widening rejoins the whole line instead of
rewrapping the two halves separately.
Combining marks attach to their base cell (`Screen::marks_of`) and survive
text extraction. A multi-byte character split across `feed_output` calls
decodes correctly — the partial sequence stays pending, as streaming requires.

toe does **not** normalise text. `"e" + U+0301` stays decomposed, because a
terminal must not alter what the application sent.

### Beyond the VT spec
OSC 8 hyperlinks · OSC 7 working directory · OSC 133 shell integration with
command blocks (text, output, cwd, exit code, duration, row span) · sixel and
kitty graphics protocol handling · scrollback search · Unicode-aware selection
· a secret redactor for transcript capture.

The DEC 2034 agent read-out emits JSON that is valid per RFC 8259 for *any*
cell content: a command containing `","evil":"` cannot inject a field, and
malformed UTF-8 from the child becomes U+FFFD rather than producing a document
strict parsers reject. Queries require the session token issued by `CSI ?2034h`.

Kitty animation timing is reportable as well as correct: an animation that has
not yet been ticked reports a due deadline rather than "idle", and each frame
is scheduled from the deadline it passed rather than from the current time, so
ticking late does not stretch the loop.

## Not implemented

Deliberate, with reasons:

- **Image decoding.** The graphics protocols are handled, but decompressing a
  PNG is codec work, not terminal work. Raw RGB/RGBA works; PNG needs a host
  decoder via `set_png_decoder`.
- **DECCRA** (copy rectangular area). The rest of the rectangle set is there.
- **8-bit C1 controls** (0x80–0x9F as CSI/OSC/DCS). In a UTF-8 terminal those
  bytes are invalid UTF-8 and become U+FFFD, which is what xterm does in UTF-8
  mode. A non-UTF-8 mode would be needed to interpret them.
- **ANSI modes IRM and LNM.** Not modelled, and DECRQM honestly reports them
  as unrecognised rather than guessing.
- **VT52 compatibility mode.**
- **National replacement character sets** (only ASCII and DEC line drawing are
  designated).
- **Double-width/height line rendering.** The line attributes are tracked and
  reported; drawing them larger is the host's business.
- **Blink, cursor shape and cursor colour as pixels.** Tracked as state and
  exposed; a terminal library does not own timing or drawing.

## Not independently verified

Honest gaps in the *verification*, not necessarily in the code:

- **No `vttest` run.** vttest is interactive and needs a real terminal
  front-end, which toe deliberately doesn't have. The differential harness
  covers much of the same ground programmatically, but not all of it.
- **Sixel and kitty graphics** are covered for protocol bookkeeping (store,
  place, replace, refuse, delete, animate) and robustness, but the decoded
  pixel output is not diffed against a reference implementation.
- **Mouse event encoding** is covered for mode tracking, not byte-for-byte
  against xterm for every button/modifier combination.
- **pyte is the only reference.** It has real gaps (no REP, CHT, CBT, SU, SD,
  alt screen; it crashes on 17 CSI parameters and normalises NFD to NFC), so
  agreement with it is weaker evidence than agreement with xterm would be.

## Bugs this process found

Recorded because they're the argument for testing this way rather than reading
the code and declaring it correct:

| Bug | Found by |
|---|---|
| UTF-8 accepted overlongs, surrogates and >U+10FFFF | Python codec oracle |
| A malformed sequence consumed the following good character | Python codec oracle |
| Combining marks were silently dropped | pyte differential |
| DECRQM reported implemented modes as unrecognised | capability audit |
| Signed overflow on digit accumulation (8 sites) | UBSan + fuzz |
| DECOM accepted and then ignored entirely | hand probe past pyte's gaps |
| CUU/CUD/CUF/CUB ignored the scroll region and margins | region test |
| SGR `38:2::R:G:B` read the colour one parameter early | hand probe of colon forms |
| SGR `58:2::R:G:B` ignored, so undercurl colours did nothing | hand probe of colon forms |
| Left/right margins clipped writes that started outside them | coverage-guided probe |
| IL/DL moved whole rows instead of just the margin columns | coverage-guided probe |
| DECSLRM homed to the margin corner, not the page origin | VT510 manual |
| DECCARA with SGR 0 SET every attribute instead of clearing | coverage-guided probe |
| Agent JSON passed invalid UTF-8 through, so strict parsers rejected the whole document | JSON-parser oracle |
| Agent JSON silently DROPPED DEL, losing a byte from the transcript | JSON-parser oracle |
| An unticked animation reported deadline 0 ("idle"), so a host polling it never started the animation | coverage-guided probe |
| Animation frames were scheduled from now_ms, so late ticks permanently stretched the loop | coverage-guided probe |
| Reflow split a logical line at the scrollback boundary, so widening never rejoined it | coverage-guided probe |
| A wide glyph's forced early wrap never marked the row soft-wrapped, injecting a blank per row on every rewrap | fuzz invariant |
| Reflow could put a wide lead in the last column with its spacer on the next row | fuzz invariant |
| Trimming a trailing blank could orphan a wide lead | fuzz invariant |
| DA3 silently dropped (an app can block on it) | capability audit |
| RIS never cleared the hyperlink table | code audit during the above |
