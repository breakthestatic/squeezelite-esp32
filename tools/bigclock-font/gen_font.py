#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py - generate an X-GLCD font from the LMS `standard.1` bitmap.

Produces components/display/fonts/font_squeezebox_standard.c, defining
`const struct GDS_FontDef Font_squeezebox_standard`, derived from the original
Lyrion Music Server (LMS, formerly SlimServer/SqueezeCenter) `standard.1`
screensaver font used by the Squeezebox Classic / SB3 VFD clock.

The BMP decode and glyph segmentation follow Slim/Display/Lib/Fonts.pm
(authoritative). Standard library only - no PIL / numpy.

Provenance / license: standard.1 is GPLv2 (Lyrion Music Server) and is derived
from the FreeSans font. See the generated .c header comment.

Run from the repo root:

    python3 tools/bigclock-font/gen_font.py

Output is deterministic; re-running regenerates a byte-identical .c file.
"""

import os
import struct
import sys

# ---- paths (resolved relative to repo root = two levels up from this file) --
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, os.pardir, os.pardir))
BMP_PATH = os.path.join(HERE, "standard.1.font.bmp")
OUT_PATH = os.path.join(REPO, "components", "display", "fonts",
                        "font_squeezebox_standard.c")

# ---- target font parameters -------------------------------------------------
# The source glyphs ink only the top ~10 visual rows of a 32-row cell. We keep
# Height = 16 (RoundUp -> 16, so 2 bytes/column) which captures all ink (rows
# 0..9) with margin while keeping records compact. bigclock.c scales this 16px
# cell by 4 to fill the 64px panel. See the generated header comment.
OUT_HEIGHT = 16
COL_BYTES = (OUT_HEIGHT + 7) // 8      # bytes per column after RoundUp/8

START_CHAR = 0x20                      # ' '
END_CHAR = 0x5A                        # 'Z'


def read_bmp(path):
    """Decode a 1bpp uncompressed Windows BMP into a top-down pixel grid.

    Returns (width, height, grid) where grid[row][col] is 0/1 with row 0 at the
    visual top. Honors palette direction exactly as Fonts.pm does.
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[0:2] != b"BM":
        raise ValueError("not a BMP (missing 'BM' magic)")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    planes = struct.unpack_from("<H", data, 26)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    if bpp != 1:
        raise ValueError("expected 1bpp, got %d" % bpp)
    if compression != 0:
        raise ValueError("expected uncompressed BMP, got compression=%d"
                         % compression)
    if planes != 1:
        raise ValueError("expected 1 plane, got %d" % planes)

    # Palette follows the DIB header. First entry governs bit polarity: Fonts.pm
    # treats a first entry that is NOT white (0xFFFFFF) as a reversed palette and
    # inverts each bit. We read the 4-byte BGRX entry at offset 14+header_size.
    pal0 = struct.unpack_from("<I", data, 14 + header_size)[0] & 0xFFFFFF
    invert = (pal0 != 0xFFFFFF)

    row_bytes = width if width <= 0 else width
    # 1bpp row stride is padded to a 32-bit (4-byte) boundary.
    stride = ((width + 31) // 32) * 4

    abs_height = abs(height)
    bottom_up = height > 0

    grid = [[0] * width for _ in range(abs_height)]
    for file_row in range(abs_height):
        base = pixel_offset + file_row * stride
        # Visual row: BMP bottom-up rows map so file row 0 is the visual bottom.
        visual_row = (abs_height - 1 - file_row) if bottom_up else file_row
        dst = grid[visual_row]
        for x in range(width):
            byte = data[base + (x >> 3)]
            bit = (byte >> (7 - (x & 7))) & 1   # MSB-first within each byte
            if invert:
                bit ^= 1
            dst[x] = bit
    return width, abs_height, grid, invert


def segment_glyphs(width, height, grid):
    """Segment the bitmap into glyphs using the marker row (last visual row).

    Per Fonts.pm: scan marker columns left->right; a column where the marker
    pixel is SET is a boundary/gap (skip); a contiguous run of UNSET marker
    columns is one glyph. charIndex starts at -1 and increments at the start of
    each run, so codepoint == charIndex directly (no 0x20 offset).

    Returns dict codepoint -> list of column indices (into rows 0..height-2).
    """
    marker_row = height - 1
    marker = grid[marker_row]

    glyphs = {}
    char_index = -1
    in_glyph = False
    cur_cols = []
    for x in range(width):
        if marker[x] == 0:          # UNSET -> part of a glyph
            if not in_glyph:
                in_glyph = True
                char_index += 1
                cur_cols = []
            cur_cols.append(x)
        else:                       # SET -> boundary
            if in_glyph:
                glyphs[char_index] = cur_cols
                in_glyph = False
    if in_glyph:
        glyphs[char_index] = cur_cols
    return glyphs


def encode_glyph(cols, grid):
    """Encode one glyph's columns into X-GLCD bytes (COL_BYTES per column).

    pixel(row i) in a column = bit (i & 7) of byte (i >> 3), rows 0..OUT_HEIGHT-1
    taken from the top of the cell.
    """
    out = []
    for x in cols:
        col_bytes = [0] * COL_BYTES
        for row in range(OUT_HEIGHT):
            if grid[row][x]:
                col_bytes[row >> 3] |= (1 << (row & 7))
        out.extend(col_bytes)
    return out


def ascii_art(cols, grid, rows=12):
    """Return an ASCII-art rendering of a glyph's top `rows` visual rows."""
    lines = []
    for row in range(rows):
        line = "".join("#" if grid[row][x] else "." for x in cols)
        lines.append(line)
    return lines


def main():
    width, height, grid, invert = read_bmp(BMP_PATH)
    glyphs = segment_glyphs(width, height, grid)

    # Determine the cell width (max glyph pixel width across the emitted range).
    # Every record is padded to this so GetCharPtr's fixed stride works.
    widths = {}
    for cp in range(START_CHAR, END_CHAR + 1):
        cols = glyphs.get(cp, [])
        widths[cp] = len(cols)
    cell_width = max(widths.values()) if widths else 0

    # Build the contiguous byte table: for each codepoint, [width][padded cols].
    records = []
    for cp in range(START_CHAR, END_CHAR + 1):
        cols = glyphs.get(cp, [])
        w = len(cols)
        data = encode_glyph(cols, grid)
        # Pad out to cell_width columns worth of bytes.
        pad = (cell_width - w) * COL_BYTES
        data.extend([0] * pad)
        records.append((cp, w, data))

    emit_c(records, cell_width)

    # ---- stdout diagnostics -------------------------------------------------
    print("standard.1 -> X-GLCD font generator")
    print("palette inverted: %s" % ("yes" if invert else "no"))
    print("segmented glyphs: %d" % len(glyphs))
    print("emitted codepoints: 0x%02X..0x%02X (%d glyphs), cell width %d, height %d"
          % (START_CHAR, END_CHAR, END_CHAR - START_CHAR + 1, cell_width, OUT_HEIGHT))
    print("")
    print("per-glyph widths:")
    for ch in "0123456789":
        print("  '%s' = %d" % (ch, widths[ord(ch)]))
    print("  ':' = %d" % widths[ord(":")])
    print("")
    for ch in ["0", "1", ":", "A"]:
        cols = glyphs.get(ord(ch), [])
        print("glyph '%s' (width %d):" % (ch, len(cols)))
        for line in ascii_art(cols, grid):
            print("  " + line)
        print("")


def emit_c(records, cell_width):
    name = "Squeezebox_standard"
    lines = []
    lines.append("#include <gds_font.h>")
    lines.append("")
    lines.append("/*")
    lines.append(" * Generated from the LMS standard.1 bitmap by")
    lines.append(" * tools/bigclock-font/gen_font.py.  Do not edit by hand; re-run the")
    lines.append(" * generator (python3 tools/bigclock-font/gen_font.py) to regenerate.")
    lines.append(" *")
    lines.append(" * Source: standard.1 - the original Squeezebox Classic / SB3 VFD")
    lines.append(" * screensaver font from Lyrion Music Server (LMS, formerly SlimServer).")
    lines.append(" * standard.1 is GPLv2 (LMS) and is derived from the FreeSans font.")
    lines.append(" *")
    lines.append(" * Format: X-GLCD, proportional (Monospace = false). The first byte of")
    lines.append(" * each glyph record is that glyph's own pixel width; the engine honors")
    lines.append(" * it (see gds_font.c GetCharPtr / bigclock.c draw_glyph_scaled).")
    lines.append(" *")
    lines.append(" * Height choice: the source glyphs ink only the top ~10 rows of a 32-row")
    lines.append(" * cell. We keep Height = %d (RoundUp -> %d, %d bytes/column), which" % (OUT_HEIGHT, OUT_HEIGHT, COL_BYTES))
    lines.append(" * captures all ink with margin and keeps records compact. bigclock.c")
    lines.append(" * scales this cell 4x to fill the 64px panel and centers on the inked")
    lines.append(" * rows.  StartChar = 0x%02X (' '), EndChar = 0x%02X ('Z'); codepoints in" % (START_CHAR, END_CHAR))
    lines.append(" * range with no inked glyph are emitted as valid blank (zero-width) cells")
    lines.append(" * so the table stays contiguous and indexable by fixed stride.")
    lines.append(" */")
    lines.append("")
    lines.append("static const uint8_t %s[ ] = {" % name)

    for cp, w, data in records:
        ch = chr(cp)
        label = ch if (0x20 < cp <= 0x7E) else ("0x%02X" % cp)
        hexbytes = ", ".join("0x%02X" % b for b in ([w] + data))
        lines.append("    %s, // Code for char %s" % (hexbytes, label))

    lines.append("};")
    lines.append("")
    lines.append("const struct GDS_FontDef Font_squeezebox_standard = {")
    lines.append("    %s," % name)
    lines.append("    %d,  // Width (cell)" % cell_width)
    lines.append("    %d,  // Height" % OUT_HEIGHT)
    lines.append("    0x%02X,  // StartChar ' '" % START_CHAR)
    lines.append("    0x%02X,  // EndChar 'Z'" % END_CHAR)
    lines.append("    false  // Monospace (proportional font)")
    lines.append("};")
    lines.append("")

    with open(OUT_PATH, "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
