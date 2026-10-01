# Authentic Squeezebox VFD font for the bigclock standby clock

Replaces the 2x-scaled `Font_droid_sans_mono_16x31` standby clock with `Font_squeezebox_standard`, a proportional font generated from the original LMS `standard.1` VFD screensaver bitmap, and fixes 12h single-digit hour padding. The generator (`tools/bigclock-font/gen_font.py`, Python 3 stdlib only) hand-decodes the 1bpp bottom-up BMP, segments glyphs via the marker row per LMS `Slim/Display/Lib/Fonts.pm`, and emits an X-GLCD font table. The font is swapped in `bigclock.c` at an integer 4x scale (16px cell → 64px panel) with vertical centering recomputed against the inked band rather than the padded cell.

Watch for: nothing blocking. The glyph decode is correct (independently verified against the emitted bytes for '0','1',':','A' — confirmed). Minor, non-blocking: `BC_INK_H=10` slightly over-estimates the real ink height (digits occupy ~rows 1–8), making vertical centering marginally conservative but still on-panel (likely).

**Verdict**: APPROVED

## High-level view

The generator is the heart of this change and the thing most likely to be wrong, so it got the most scrutiny. It reads the BMP header at documented offsets, derives palette polarity from the first palette entry, flips the bottom-up rows to a top-down grid with 32-bit row-stride padding, and segments glyphs by scanning the bottom marker row — runs of unset marker columns are glyphs, boundaries are set columns. The codepoint mapping is `codepoint == charIndex` with no `0x20` offset, which the plan flagged as the critical correction over the original brief. The emitted bytes decode to the correct characters: I traced '0','1',':','A' from the hex in the font .c and they render as a rounded oval, a flagged 1, two stacked dots, and a peaked A with crossbar — matching both the characters and the ASCII-art dump in the commit message.

The emitted `font_squeezebox_standard.c` defines `const struct GDS_FontDef Font_squeezebox_standard` with a positional initializer whose order matches `gds_font.h` exactly (FontData, Width=9, Height=16, StartChar=0x20, EndChar=0x5A, Monospace=false). The table is contiguous across 0x20–0x5A, every record padded to the 9-column cell width with the per-glyph width in the leading byte, and all glyphs the clock needs ('0'–'9', ':', ' ', '-', 'A','P','M') are present and non-garbled. The GPLv2/FreeSans provenance and the Height=16 trim rationale are documented in the header comment.

The `bigclock.c` swap points `BC_FONT` at the new font, sets `BC_SCALE=4`, and recomputes vertical centering on an explicit inked band (`BC_INK_TOP=0`, `BC_INK_H=10`) instead of the full cell height, so the clock sits centered rather than top-stuck. `format_time` keeps the existing `%d:%02d` vs `%02d:%02d` branch — producing "9:05" in 12h and "09:05" in 24h — and replaces the stale "leading space" comment with an accurate one plus a note that AM/PM is intentionally omitted. The diff touches only `bigclock.c` and `gds_font.h` in the component; trigger/SNTP/NVS/displayer logic is untouched.

Build wiring needs no change: `fonts` is already in the display component's `SRC_DIRS`, so the new .c compiles automatically, and the `extern` declaration was added to `gds_font.h` next to the other font externs. The host syntax-check evidence (clang-as-gcc, `-fsyntax-only -Wall` against a minimal shim supplying `GDS_FontDef`, exit 0, no warnings) is recorded in the commit message, and the shim's struct is a verbatim copy of the real one.

<details>
<summary>Issues (1)</summary>

1. **Inked-band height is conservative** — `BC_INK_H=10` over-estimates the real ink (digits span ~rows 1–8). Centering stays on-panel and visually centered, so this is cosmetic only; no action required unless on-device testing shows the clock sitting slightly high. (non-blocking)

</details>

<details>
<summary>Details</summary>

### BMP decode and the codepoint mapping gate

The generator parses the BITMAPINFOHEADER at the documented offsets (pixel offset at 10, header size at 14, width/height at 18/22, planes/bpp/compression at 26/28/30) and asserts bpp=1, compression=0, planes=1. Row stride is padded to a 32-bit boundary (`((width+31)//32)*4`), bits are extracted MSB-first within each byte, and the bottom-up file rows are flipped so visual row 0 is the top. Palette polarity is read from the first palette entry at `14+header_size`: anything other than white triggers per-bit inversion, mirroring what Fonts.pm does.

Glyph segmentation scans the bottom marker row left-to-right: a contiguous run of unset marker columns is one glyph, set columns are boundaries, and `char_index` increments at the start of each run so `codepoint == char_index` directly. This is the mapping correction the plan called out as the critical fix over the original brief's `0x20 + charIndex` guess — getting it wrong would shift every glyph and garble the output.

I verified the mapping didn't drift by decoding the emitted bytes rather than trusting the ASCII-art. With COL_BYTES=2 (byte0 = rows 0–7, LSB=row0; byte1 = rows 8–15):

```
'0' w5:  col0 7C=rows2-6  col1 82=rows1,7  col2 82  col3 82  col4 7C
         -> left/right verticals rows1-7, top/bottom caps -> rounded oval
'1' w5:  col1 84=rows2,7  col2 FE=rows1-7 (full stem)  col3 80=row7
         -> top-left flag, full stem, base -> flagged 1
':' w1:  col0 D8=rows3,4,6,7 -> two stacked dots
'A' w6:  col0 E0=rows5-7  col1 38=rows3-5  col2 26=rows1,2,5  col3 26
         col4 38  col5 E0 -> peak at center top, crossbar across row5
```

All four match the characters and the commit's ASCII-art dump. The decode is confirmed correct — not inverted, not off-by-one, not garbled.

### X-GLCD encoding and table shape

Each glyph record is `[width][col0 lo][col0 hi][col1 lo][col1 hi]...` with 2 bytes per column (Height=16 rounds up to 16 → 2 bytes), pixel(row i) = bit (i&7) of byte (i>>3). Records are padded out to the 9-column cell width with zero bytes so `GetCharPtr`'s fixed stride works, and the leading width byte carries the glyph's own proportional width for the `Monospace==false` path that `bigclock.c`'s `glyph_src_width`/`draw_glyph_scaled` already honor. The table is contiguous 0x20–0x5A; codepoints with no inked glyph (e.g. control-range gaps) emit as valid blank cells. The `GDS_FontDef` initializer is positional and lands in the right fields (verified against the real struct: FontData, Width, Height, StartChar, EndChar, Monospace), with Width=9, Height=16, 0x20, 0x5A, false.

Coverage check against what the clock renders: ' ' (0x20, w2), '-' (0x2D, w3), '0'–'9' (all w5), ':' (0x3A, w1), 'A' (0x41), 'M' (0x4D), 'P' (0x50) are all present and non-empty. Provenance (GPLv2 / LMS / FreeSans) and the Height=16 trim rationale are in the header comment.

### Inked-band vertical centering

The swap in `bigclock.c` changes `textH` from `BC_FONT->Height * scale` to `BC_INK_H * scale` and offsets `y0` by `-BC_INK_TOP*scale`, so centering is computed on the real ink rather than the 16px padded cell. With `BC_INK_H=10`, scale 4: textH=40, y0=(64-40)/2=12 — centered, not top-stuck, which is the stated goal.

One cosmetic note: decoding the digits shows ink actually spanning roughly rows 1–8 (8 rows), with ':'/';' descenders reaching row 7–8, so 10 is a slight over-estimate of the band. The effect is that the band is centered as if 2px taller than the real ink, nudging the glyphs marginally upward — still comfortably on-panel and visually centered. Not worth changing blind; if on-device testing shows it sitting high, dropping `BC_INK_H` to ~8 would recenter. Non-blocking.

### format_time hour padding

The 12h/24h branch is unchanged in logic — `s_fmt12 && hour < 10` uses `"%d:%02d"` (→ "9:05", no leading zero or space), everything else uses `"%02d:%02d"` (→ "09:05" in 24h, "12:05" for 12h two-digit). The stale comment that claimed a "leading space" was replaced with an accurate description, and a comment now records that AM/PM is intentionally omitted to keep the digits large. The "--:--" unsynced path is untouched.

### Build wiring and scope

`fonts` is already in the display component's `SRC_DIRS` and `INCLUDE_DIRS`, so the generated .c is picked up with no CMakeLists edit, and the `extern const struct GDS_FontDef Font_squeezebox_standard;` was added to `gds_font.h` beside the other font externs — reachable from `bigclock.c`, which includes that header. The commit stat confirms the only component files touched are `bigclock.c` and `gds_font.h`; trigger logic, SNTP, NVS parsing, and `displayer.c` are untouched.

### Host syntax-check evidence

The commit records the host check: `gcc -fsyntax-only -Wall -I tools/bigclock-font/shim components/display/fonts/font_squeezebox_standard.c` → exit 0, no warnings, using clang aliased as gcc. The shim at `tools/bigclock-font/shim/gds_font.h` supplies only the `GDS_FontDef` struct (a verbatim copy of the real field order) plus stdint/stdbool, which is exactly what the generated .c needs. Per instructions I did not re-run it; the evidence is present and the struct the check validates against matches the real header, so the positional initializer is sound. (The ESP-IDF firmware build is not runnable here and was correctly not attempted.)

</details>

<details>
<summary>File map</summary>

- `tools/bigclock-font/gen_font.py` — new stdlib-only BMP decoder + X-GLCD font generator; marker-row segmentation; codepoint==charIndex mapping; prints widths and ASCII-art.
- `tools/bigclock-font/shim/gds_font.h` — minimal host shim (GDS_FontDef verbatim) for the `-fsyntax-only` check.
- `tools/bigclock-font/standard.1.font.bmp` — source LMS bitmap (binary).
- `components/display/fonts/font_squeezebox_standard.c` — generated font table + `Font_squeezebox_standard` def; GPLv2/FreeSans provenance header.
- `components/display/core/gds_font.h` — added `extern` decl for the new font.
- `components/display/bigclock.c` — BC_FONT/BC_SCALE swap, inked-band centering, format_time comment/padding, header comment update.

Full diff: `git show 1ecaf579` on branch `bigclock-4.3`.

</details>
