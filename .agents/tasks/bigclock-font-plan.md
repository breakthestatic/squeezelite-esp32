# Implementation Plan — Authentic Squeezebox VFD font for bigclock

Goal: replace `Font_droid_sans_mono_16x31` (2x scaled) in the bigclock standby
clock with a font derived from the original LMS `standard.1` bitmap (the real
Squeezebox Classic/SB3 screensaver font — a FreeSans-derived PROPORTIONAL
sans-serif), and fix 12h/24h hour padding.

## Verified facts from exploration (ground truth — do not re-derive, do not assume otherwise)

These were confirmed by decoding the staged BMP and reading
`Slim/Display/Lib/Fonts.pm` (LMS, authoritative). They CORRECT the task brief
where it differs.

1. **BMP header** (`tools/bigclock-font/standard.1.font.bmp`): `BM`, 40-byte
   BITMAPINFOHEADER, `biWidth=2296`, `biHeight=33`, `biPlanes=1`,
   `biBitCount=1`, `biCompression=0`, pixel-data offset = **62**, row stride
   padded to 32-bit = `((2296+31)//32)*4 = 288` bytes/row.
2. **Palette direction**: first palette entry (4 bytes at file offset 54, read
   as little-endian u32) = `0xFFFFFF` → **normal palette, NO inversion**. (If a
   future BMP has first entry != 0xFFFFFF, invert each bit: `bit?0:1`. Fonts.pm
   does exactly this.) Bit order within a byte is **MSB-first**
   (`bit = (byte >> (7-(x&7))) & 1`).
3. **Row order**: BMP is bottom-up. Build a top-down grid: visual row `r`
   (0=top) comes from file row `biHeight-1-r`. After flip, rows 0..31 are glyph
   pixels, **row 32 (bottom) is the MARKER row**.
4. **Marker semantics** (per Fonts.pm `parseFont`): scan columns left→right.
   A column where marker pixel is **SET (1)** is a gap/boundary — SKIP it. A
   contiguous run of columns where marker is **UNSET (0)** is ONE glyph; its
   pixels are rows 0..31 of those columns, column-major. `charIndex` starts at
   -1 and increments at the start of each glyph run; the first glyph (index 0)
   is the interspace char.
5. **CRITICAL mapping correction**: the glyph→codepoint mapping is
   **`codepoint == charIndex` directly** (NOT `0x20 + charIndex` as the brief
   guessed). Verified by rendering: charIndex 48 renders '0', 49→'1', …
   57→'9', 58→':', 45→'-', 65→'A', 77→'M', 80→'P', 90→'Z', 32→' ' (blank).
   The low indices (0..31) are control-char/placeholder cells and are empty or
   box glyphs. **DO NOT offset by 0x20.** The implementer MUST re-confirm this
   with the ASCII-art dump before emitting (see step 2 verification).
6. **Inked vertical band**: across codepoints 0x20..0x5A the ink occupies
   **visual rows 0..9 only** (10px tall); rows 10..31 are blank. The glyph is
   top-aligned in the 32-row cell.
7. **Glyph widths** (proportional): digits '0'..'9' are all width **5**, ':' is
   width **1**, ' ' is width **2**, '-' is width 3. `"12:34"` = 21px wide at
   scale 1; `"09:05"` = 21px. These feed the BC_SCALE/centering math.
8. **Target panel**: 256x64 (SSD1322). `GDS_GetWidth`/`GDS_GetHeight` return
   256/64.
9. **Toolchain present**: `python3` (3.9.6, stdlib only — no PIL/numpy),
   `clang` as `gcc` for host syntax-check. ESP-IDF build CANNOT run locally
   (do NOT run `idf.py`; do NOT push/trigger CI).
10. **GDS_FontDef field order** (from `components/display/core/gds_font.h`,
    exact): `{ const uint8_t* FontData; int Width; int Height; int StartChar;
    int EndChar; bool Monospace; }`.
11. **X-GLCD glyph encoding** (from `gds_font.c` `GetCharPtr`/`GDS_FontDrawChar`
    and bigclock.c `draw_glyph_scaled`): per-glyph record =
    `[width byte][col0 bytes][col1 bytes]...`; bytes per column =
    `RoundUpHeight/8` where `RoundUpHeight` rounds `Height` up to a multiple of
    8; `pixel(row i) = bit (i & 7) of byte (i/8)` within a column. Record
    stride = `Width * colBytes + 1` (uses the font's cell `Width`, NOT per-glyph
    width — see `GetCharPtr`). So **every glyph record is padded to the cell
    `Width`** even for a proportional font; the first byte holds the glyph's own
    pixel width. bigclock's `draw_glyph_scaled`/`glyph_src_width` already decode
    this and honor the width byte when `Monospace==false`.

## Design decisions (made here, with rationale)

- **Keep Height = 16, trim to the inked band.** The ink is only in rows 0..9.
  Emit Height=16 (RoundUp→16, colBytes=2) so each column is 2 bytes covering
  rows 0..15 — captures all ink (rows 0..9) with margin, and keeps records
  compact. Rationale: emitting Height=32 wastes 2 extra blank bytes/column and
  forces a smaller integer scale to fit 64px. 16 rows of real content (10
  inked + 6 slack) scaled 4x = 64px fills the panel exactly. *(If the
  implementer prefers Height=32 for fidelity to the source cell, that is
  acceptable but then BC_SCALE must be recomputed and centering done on inked
  pixels; document whichever is chosen in the .c header.)*
- **StartChar=0x20 (' '), EndChar=0x5A ('Z').** Covers digits, ':', ' ', '-',
  'A','P','M' (for a future AM/PM option) and all caps. Any codepoint in range
  that has no inked glyph is emitted as a valid blank cell (width byte + zero
  columns-worth of bytes) so the table stays contiguous and indexable by
  `GetCharPtr`. Rationale: contiguous StartChar..EndChar is what the engine
  requires; a full 0x20..0x5A table is small and future-proof.
- **BC_SCALE = 4, Height = 16.** 16*4 = 64 = panel height exactly. Centering
  uses the real inked rows (0..9 → 40px tall at 4x) so the clock sits vertically
  centered rather than top-stuck. Rationale: integer scale keeps the crisp
  "chunky VFD" look the user asked for; 4x on a 16px cell is the clean fit.
- **Proportional (Monospace=false).** The source font is proportional; the brief
  requires honoring per-glyph widths. bigclock already handles this path.

## Plan

- [ ] 1. Write the generator `tools/bigclock-font/gen_font.py` (Python 3 stdlib only).
      Implement, in order: (a) read the BMP; parse header fields at the documented
      offsets; assert `BM`, bpp=1, compression=0, planes=1. (b) Read first palette
      entry u32 at offset 54; set `invert = (entry != 0xFFFFFF)`. (c) Build the
      top-down grid `g[row][col]` for rows 0..32 using stride=288, MSB-first bit
      extraction, applying `invert`, and the bottom-up→top-down flip
      (`g[biHeight-1-i]` ← file row i). (d) Segment glyphs via the marker row
      (row 32): runs of UNSET columns are glyphs; increment charIndex per run;
      **map codepoint = charIndex** (verified fact 5 — no 0x20 offset). (e) For
      each codepoint 0x20..0x5A, locate its glyph run (or blank), take Height=16
      rows (0..15), encode X-GLCD: for each of the glyph's own-width columns emit
      `colBytes=2` bytes where `byte0`=rows 0..7, `byte1`=rows 8..15, each
      `pixel(i)=bit(i&7) of byte(i>>3)`; prepend the glyph width byte; **pad the
      record out to cell `Width` columns** (Width = max glyph width in range) with
      zero bytes so `GetCharPtr`'s fixed stride works. (f) Emit
      `components/display/fonts/font_squeezebox_standard.c` defining
      `const struct GDS_FontDef Font_squeezebox_standard` (see step 3 for exact
      shape). (g) Print to stdout: total glyph count, per-glyph widths for
      '0'..'9' and ':', and ASCII-art of '0','1',':','A'. Make output
      deterministic (fixed byte formatting, sorted iteration).
      Files: `tools/bigclock-font/gen_font.py`
      Verify: `python3 tools/bigclock-font/gen_font.py` runs clean; the ASCII-art
      shows a recognizable '0', '1', ':' (two stacked dots), 'A'; printed digit
      widths are all 5 and ':' width is 1 (matches verified fact 7). If any glyph
      is garbled or blank, the codepoint mapping is wrong — fix before proceeding.

- [ ] 2. Run the generator and eyeball-verify the ASCII-art dump matches the
      verified glyph shapes (digits as FreeSans ovals/strokes, ':' two dots in
      the upper band, 'A' a peaked cap). This is the mapping gate from fact 5.
      Files: none (produces `components/display/fonts/font_squeezebox_standard.c`)
      Verify: visual confirmation of correct glyphs in stdout; widths line up
      with fact 7. Re-run after any gen_font.py change.

- [ ] 3. Confirm the emitted `font_squeezebox_standard.c` matches existing-font
      style so it links: starts with `#include <gds_font.h>`; defines a
      `static const uint8_t <Name>[] = { ... };` byte table; then
      `const struct GDS_FontDef Font_squeezebox_standard = { <Name>, Width,
      Height, 0x20, 0x5A, false };` using the EXACT field order from fact 10
      (FontData, Width, Height, StartChar, EndChar, Monospace). Header comment:
      "Generated from the LMS standard.1 bitmap by tools/bigclock-font/gen_font.py.
      Source standard.1 is GPLv2 (Lyrion Music Server) / FreeSans-derived; see
      provenance." Document the Height=16 trim choice in the comment.
      Files: `components/display/fonts/font_squeezebox_standard.c` (generated)
      Verify: file visually matches the structure of
      `components/display/fonts/font_droid_sans_mono_16x31.c`.

- [ ] 4. Host syntax-check the generated .c with a minimal shim (the real
      gds_font.h needs none of the engine, just the struct + stdint/stdbool).
      Create `tools/bigclock-font/shim/gds_font.h` containing only `#include
      <stdint.h>`, `#include <stdbool.h>`, and the `struct GDS_FontDef`
      definition copied verbatim from `components/display/core/gds_font.h`
      (fact 10). Then compile with the shim on the include path.
      Files: `tools/bigclock-font/shim/gds_font.h`
      Verify: `gcc -fsyntax-only -I tools/bigclock-font/shim
      components/display/fonts/font_squeezebox_standard.c` exits 0 (clang aliased
      as gcc is fine). No warnings about struct init order/count.

- [ ] 5. Declare the font so bigclock.c can reference it. Add
      `extern const struct GDS_FontDef Font_squeezebox_standard;` to
      `components/display/core/gds_font.h` alongside the other `extern const
      struct GDS_FontDef Font_*;` declarations (keeps the pattern; bigclock.c
      includes gds_font.h). Verify the display component already compiles the
      `fonts` dir: `components/display/CMakeLists.txt` has `fonts` in `SRC_DIRS`
      (confirmed present) so the new .c is picked up automatically — no
      CMakeLists edit needed; note this in the commit.
      Files: `components/display/core/gds_font.h`
      Verify: `grep -n Font_squeezebox_standard
      components/display/core/gds_font.h` shows the extern; re-run the step-4
      gcc syntax-check still passes.

- [ ] 6. Swap the font and scale in `components/display/bigclock.c`. Change
      `#define BC_FONT (&Font_droid_sans_mono_16x31)` →
      `(&Font_squeezebox_standard)` and `#define BC_SCALE 2` → `4` (fact: 16px
      cell * 4 = 64px). Update the top-of-file comment that references
      "Font_droid_sans_mono_16x31 ... 2x" to describe the new font. The existing
      `draw_glyph_scaled`, `glyph_src_width`, and `string_scaled_width` already
      decode the X-GLCD proportional format correctly (fact 11) and need no
      change for the swap.
      Files: `components/display/bigclock.c`
      Verify: `grep -n "BC_FONT\|BC_SCALE" components/display/bigclock.c` shows
      the new values; no remaining reference to `Font_droid_sans_mono_16x31` in
      bigclock.c.

- [ ] 7. Center on INKED pixels, not the padded cell, in `draw()` of
      `components/display/bigclock.c`. The glyphs ink only rows 0..9 of the 16px
      cell. Define `#define BC_INK_TOP 0` and `#define BC_INK_H 10` (the verified
      inked band). Compute `textH = BC_INK_H * scale` for vertical centering:
      `y0 = (panelH - textH)/2 - BC_INK_TOP*scale`. Horizontal centering keeps
      `textW = string_scaled_width(buf, scale)` and `x0 = (panelW - textW)/2`.
      Keep the `if (x0<0) x0=0; if (y0<0) y0=0;` guards. Rationale: without this
      the clock renders in the top ~40px with a 24px gap below.
      Files: `components/display/bigclock.c`
      Verify: `grep -n "BC_INK_H\|textH" components/display/bigclock.c` shows the
      inked-band centering; reason through the arithmetic:
      `"09:05"` width = 21*4 = 84px → x0 = (256-84)/2 = 86; textH = 10*4 = 40 →
      y0 = (64-40)/2 - 0 = 12. On-panel and centered.

- [ ] 8. Fix hour padding in `format_time()` of `components/display/bigclock.c`.
      Current code emits a leading SPACE for single-digit 12h hours. Change so:
      12h single-digit hour → `"%d:%02d"` (e.g. "9:05", no leading zero, no
      leading space); 12h two-digit and ALL 24h → `"%02d:%02d"` (e.g. "09:05",
      "21:05"). The current branch already does `%d` vs `%02d` but the comment
      claims a "leading space" — ensure the code truly produces no leading
      space/zero in 12h single-digit, remove/replace the misleading comment, and
      add a comment: "AM/PM is intentionally not shown." Keep the `"--:--"`
      unsynced path untouched. Do NOT touch trigger logic, SNTP, NVS parsing, or
      displayer.c (fact: brief constraint 4).
      Files: `components/display/bigclock.c`
      Verify: `grep -n -A6 "format_time" components/display/bigclock.c` shows the
      12h branch uses `"%d:%02d"` and the 24h/else branch `"%02d:%02d"`, no `" "`
      literal prefix, and the AM/PM comment present. Reason through: fmt12 &&
      hour=9,min=5 → "9:05"; fmt12 && hour=12 → "12:05"; 24h hour=9 → "09:05".

- [ ] 9. Final consistency pass. Re-run the generator (step 1 verify) to confirm
      the checked-in .c is byte-identical to a fresh regeneration (determinism),
      and re-run the step-4 gcc syntax-check on the final .c. Confirm bigclock.c
      has no leftover references to the old font and the hour-padding + AM/PM
      comment are in place.
      Files: none
      Verify: `python3 tools/bigclock-font/gen_font.py` then
      `git diff --stat components/display/fonts/font_squeezebox_standard.c` shows
      no diff (deterministic); `gcc -fsyntax-only -I tools/bigclock-font/shim
      components/display/fonts/font_squeezebox_standard.c` exits 0;
      `grep -n "Font_droid_sans_mono_16x31" components/display/bigclock.c`
      returns nothing.

## Notes / assumptions

- The ESP-IDF firmware build is NOT run here (toolchain segfaults under
  emulation on arm64). Full on-device verification (clock fills 64px, glyphs
  look like the VFD, colon renders, 9:05 vs 09:05) happens via CI + OTA flash,
  per `BIGCLOCK_HANDOFF.md`. That is outside this task's local scope.
- If the implementer chooses Height=32 instead of 16: set colBytes=4, keep
  Height=32, pick BC_SCALE=2 (32*2=64), and center on inked rows (BC_INK_H
  still ~10, scaled 2x = 20px). Document the choice. The Height=16/scale-4 path
  above is the recommended one.
- `standard.1` provenance (GPLv2, LMS / FreeSans) is noted in the generated .c
  header comment, matching how the repo documents font licensing
  (see `components/display/fonts/LICENSE-*`).
