#ifndef _GDS_FONT_SHIM_H_
#define _GDS_FONT_SHIM_H_

/*
 * Minimal host shim for syntax-checking the generated font .c files.
 *
 * This supplies only `struct GDS_FontDef` (field order/names copied verbatim
 * from components/display/core/gds_font.h) plus the integer types the generated
 * table uses. It is NOT the real gds_font.h and is only on the include path for
 * `gcc -fsyntax-only` host checks of the generated font source.
 */

#include <stdint.h>
#include <stdbool.h>

struct GDS_FontDef {
    const uint8_t* FontData;

    int Width;
    int Height;

    int StartChar;
    int EndChar;

    bool Monospace;
};

#endif
