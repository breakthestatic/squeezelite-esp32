/*
 * bigclock.c - native full-height (64px) standby clock for squeezelite-esp32
 *
 * See bigclock.h for the public interface and overview.
 *
 * Design notes
 * ------------
 *  - The device has no wall clock of its own, so we run an SNTP client. Until
 *    the first sync we render "--:--" so we never show a wrong time.
 *  - Drawing uses Font_squeezebox_standard, derived from the original LMS
 *    `standard.1` bitmap - the real Squeezebox Classic / SB3 VFD screensaver
 *    font (a FreeSans-derived proportional sans-serif). It is blitted at an
 *    integer 4x scale: the 16px cell * 4 = 64px fills the panel height, giving
 *    a chunky "VFD pixel" look that matches the authentic Squeezebox clock
 *    while keeping real glyph shapes (not 7-segment, not monospace). We read
 *    the X-GLCD glyph format directly (see gds_font.c) and plot each source
 *    pixel as a scale x scale block via GDS_DrawPixel. This renders a proper
 *    ':' colon between hours and minutes.
 *  - A 1 Hz FreeRTOS timer repaints while active. The clock is only active
 *    when the player is powered off, so this costs nothing during playback.
 *
 * This software is released under the MIT License.
 */

#include "bigclock.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "platform_config.h"  /* config_alloc_get_str - NVS access */
#include "gds.h"
#include "gds_text.h"
#include "gds_font.h"
#include "gds_draw.h"        /* GDS_DrawPixel */

static const char *TAG = "bigclock";

/* The display handle is created by the display component. It is exported there
 * as a global "display" (see components/display/display.c). We reference it. */
extern struct GDS_Device *display;

/* ---- configuration (from NVS "clock_config") -------------------------- */
/* Syntax: tz=<POSIX TZ>[,ntp=<server>][,fmt=12|24][,yoff=<pixels>][,scale=<n>]
 * Example: tz=EST5EDT,M3.2.0,M11.1.0,ntp=pool.ntp.org,fmt=12,yoff=-16,scale=7
 *
 * yoff is a signed vertical offset in panel pixels applied as a delta from the
 * vertically-centered position: negative moves the clock UP, positive moves it
 * DOWN, 0 (default) is centered.
 *
 * scale is the integer font magnification (source px -> panel px), default
 * BIGCLOCK_SCALE_DEF, clamped to BIGCLOCK_SCALE_MIN..BIGCLOCK_SCALE_MAX. The
 * digit ink is ~7px tall, so on-panel digit height ~= 7 * scale (scale 7 ~=
 * 49px, 8 ~= 56px).
 *
 * Both yoff and scale let you tune the clock live to suit how the panel is
 * physically mounted, without rebuilding firmware - edit the NVS value and
 * reboot (bigclock_init re-reads it at startup).
 */

/* Font scale default and clamp bounds. These are declared here (before
 * bigclock_init, which clamps against them) and use a BIGCLOCK_ prefix rather
 * than BC_ because the toolchain's <sys/syslimits.h> already defines BC_*
 * macros (e.g. BC_SCALE_MAX for the bc(1) calculator), which collide. */
#define BIGCLOCK_SCALE_DEF  7
#define BIGCLOCK_SCALE_MIN  2
#define BIGCLOCK_SCALE_MAX  9   /* keeps the widest "12:34" within 256px */

static char  s_tz[64]   = "UTC0";
static char  s_ntp[64]  = "pool.ntp.org";
static bool  s_fmt12    = false;
static int   s_yoff     = 0;    /* vertical offset, px; -up / +down, delta from center */
static int   s_scale    = 0;    /* font magnification; set from NVS 'scale=' in init, clamped */

static bool        s_active      = false;
static bool        s_sntp_started = false;
static TimerHandle_t s_timer      = NULL;

/* LMS power intent (see bigclock.h). Initialized to powered-OFF so the first
 * grfe frame after a cold boot shows the clock and the first power-ON aude
 * releases it. Static-initialized so it is correct before any code runs. */
bool bigclock_lms_power_off = true;

/* forward */
static void draw(void);
static void timer_cb(TimerHandle_t t);

/* Parse a "key=value" token out of the config string into dst. Returns true if
 * found. Copies up to dstlen-1 chars, stopping at ',' or end. */
static bool parse_kv(const char *cfg, const char *key, char *dst, size_t dstlen) {
    const char *p = strstr(cfg, key);
    if (!p) return false;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != ',' && i < dstlen - 1) dst[i++] = *p++;
    dst[i] = '\0';
    return true;
}

void bigclock_init(void) {
    char *cfg = config_alloc_get_str("clock_config", NULL, "tz=UTC0,ntp=pool.ntp.org,fmt=24");
    if (cfg) {
        char tmp[8];
        parse_kv(cfg, "tz=",  s_tz,  sizeof(s_tz));
        parse_kv(cfg, "ntp=", s_ntp, sizeof(s_ntp));
        if (parse_kv(cfg, "fmt=", tmp, sizeof(tmp))) {
            s_fmt12 = (strncmp(tmp, "12", 2) == 0);
        }
        if (parse_kv(cfg, "yoff=", tmp, sizeof(tmp))) {
            s_yoff = atoi(tmp);     /* signed; negative = up, positive = down */
        }
        if (parse_kv(cfg, "scale=", tmp, sizeof(tmp))) {
            s_scale = atoi(tmp);
        }
        free(cfg);
    }

    /* Default and clamp the font scale to a drawable range. */
    if (s_scale <= 0) s_scale = BIGCLOCK_SCALE_DEF;
    if (s_scale < BIGCLOCK_SCALE_MIN) s_scale = BIGCLOCK_SCALE_MIN;
    if (s_scale > BIGCLOCK_SCALE_MAX) s_scale = BIGCLOCK_SCALE_MAX;

    /* Apply timezone so localtime() is correct. */
    setenv("TZ", s_tz, 1);
    tzset();

    ESP_LOGI(TAG, "init tz='%s' ntp='%s' fmt=%s yoff=%d scale=%d", s_tz, s_ntp, s_fmt12 ? "12h" : "24h", s_yoff, s_scale);

    /* Create (but do not start) the 1 Hz repaint timer. */
    if (!s_timer) {
        s_timer = xTimerCreate("bigclock", pdMS_TO_TICKS(1000), pdTRUE, NULL, timer_cb);
    }
}

void bigclock_network_up(void) {
    if (s_sntp_started) return;
    s_sntp_started = true;

    /* Use the classic sntp_* API from esp_sntp.h. It is available across
     * ESP-IDF 4.x (this codebase is v4.3-era) and remains supported in 5.x.
     * If you build against a newer IDF that has deprecated these in favor of
     * esp_sntp_*, swap the three calls below accordingly. */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, s_ntp);
    sntp_init();
    ESP_LOGI(TAG, "SNTP started (server %s)", s_ntp);
}

/* True once the RTC has a plausible wall-clock time (post-2020). */
static bool time_is_valid(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (tm.tm_year + 1900) >= 2020;
}

/* Format the current HH:MM into buf. Uses "--:--" until time is synced. */
static void format_time(char *buf, size_t len) {
    if (!time_is_valid()) {
        snprintf(buf, len, "--:--");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    int hour = tm.tm_hour;
    if (s_fmt12) {
        hour %= 12;
        if (hour == 0) hour = 12;
    }
    /* 12h mode: single-digit hours have no leading zero and no leading space
     * (e.g. "9:05"), matching the original Squeezebox clock. 24h mode (and 12h
     * two-digit hours) keep the zero-padded "09:05"/"21:05" form. AM/PM is
     * intentionally not shown so the big digits can stay maximally large. */
    if (s_fmt12 && hour < 10) {
        snprintf(buf, len, "%d:%02d", hour, tm.tm_min);
    } else {
        snprintf(buf, len, "%02d:%02d", hour, tm.tm_min);
    }
}

/* ---- scaled normal-font rendering ------------------------------------- */
/* We draw a regular monospace font at an integer scale so it looks like a
 * normal clock (with a real colon) but still fills the tall panel. The font
 * data uses the X-GLCD column-major format described in gds_font.h:
 *   glyph = [width][col0 bytes][col1 bytes]... , RoundUpHeight/8 bytes/col,
 *   pixel(row i) = bit (i & 7) of byte (i / 8) within the column.        */

#define BC_FONT   (&Font_squeezebox_standard)

/* standard.1 is a SMALL source font: the digit glyphs ink only rows 1..7 of the
 * 16px cell (~7px of real content). On-panel digit height ~= 7 * scale, so the
 * default (BIGCLOCK_SCALE_DEF = 7) gives ~49px, close to the previous
 * Droid-Sans-Mono-at-2x look while staying within the 256px width even for the
 * widest "12:34". The scale is runtime-tunable via the NVS 'scale=' key; the
 * default and clamp bounds (BIGCLOCK_SCALE_*) are defined up near the config
 * block above, before bigclock_init() clamps against them. */

/* The real inked band within the cell, measured from the generated glyphs
 * (see tools/bigclock-font: digits ink rows 1..7). Centering and the on-panel
 * clamp use THIS band, not the full 16px cell (most of which is empty). */
#define BC_INK_TOP 1          /* first inked row in the cell */
#define BC_INK_H   7          /* height of the inked band, in source pixels */

/* Gap between adjacent glyphs, in SOURCE pixels (scaled with the font scale). The
 * standard.1 glyph widths are tight with no built-in side bearing, so without
 * this the digits and colon touch. 1 source px * 8 = 8px on-panel. */
#define BC_GAP    1

static int round_up8(int h) { return (h % 8) ? (((h + 7) / 8) * 8) : h; }

/* Width in source pixels of one glyph (monospace fonts report a fixed width,
 * but the per-glyph width byte is authoritative and matches the drawing). */
static int glyph_src_width(const struct GDS_FontDef *f, char c) {
    if (c < f->StartChar || c > f->EndChar) return 0;
    if (f->Monospace) return f->Width;
    int colBytes = round_up8(f->Height) / 8;
    const uint8_t *g = &f->FontData[(c - f->StartChar) * (f->Width * colBytes + 1)];
    return *g;                  /* first byte is this glyph's width */
}

/* Draw one glyph with its top-left at (x0,y0), each source pixel expanded to
 * a scale x scale block. */
static void draw_glyph_scaled(char c, int x0, int y0, int scale) {
    const struct GDS_FontDef *f = BC_FONT;
    if (c < f->StartChar || c > f->EndChar) return;

    int colBytes = round_up8(f->Height) / 8;
    const uint8_t *g = &f->FontData[(c - f->StartChar) * (f->Width * colBytes + 1)];
    g++;                        /* skip per-glyph width byte; column data follows */
    /* For monospace fonts the engine draws the full cell width (f->Width);
     * for proportional fonts it draws the glyph's own width byte. Match that
     * so alignment is identical to the normal text engine. */
    int w = f->Monospace ? f->Width : glyph_src_width(f, c);

    for (int col = 0; col < w; col++) {
        const uint8_t *colData = g + col * colBytes;
        for (int row = 0; row < f->Height; row++) {
            int yByte = row / 8, yBit = row & 7;
            if (colData[yByte] & (1 << yBit)) {
                int px = x0 + col * scale;
                int py = y0 + row * scale;
                for (int dx = 0; dx < scale; dx++)
                    for (int dy = 0; dy < scale; dy++)
                        GDS_DrawPixel(display, px + dx, py + dy, GDS_COLOR_WHITE);
            }
        }
    }
}

/* Total rendered width of a string at the given scale, including the BC_GAP
 * inter-character gap between (but not after) glyphs. Must match the advance
 * used in draw() so horizontal centering is correct. */
static int string_scaled_width(const char *s, int scale) {
    int w = 0;
    for (; *s; s++) {
        if (w) w += BC_GAP * scale;         /* gap before every glyph except the first */
        w += glyph_src_width(BC_FONT, *s) * scale;
    }
    return w;
}

/* Paint the clock centered on the full display height. */
static void draw(void) {
    if (!display) return;

    char buf[8];
    format_time(buf, sizeof(buf));

    int scale = s_scale;        /* from NVS 'scale=', defaulted/clamped in bigclock_init */
    if (scale <= 0) scale = BIGCLOCK_SCALE_DEF;   /* guard if draw() ever runs before init */
    int panelW = GDS_GetWidth(display);
    int panelH = GDS_GetHeight(display);
    int textW  = string_scaled_width(buf, scale);
    int textH  = BC_INK_H * scale;      /* center on inked pixels, not padding */

    int x0 = (panelW - textW) / 2;
    /* y0 places the inked band centered; subtract BC_INK_TOP*scale so the top
     * of the glyph cell lands above the band by the same offset the font uses.
     * s_yoff (from NVS) nudges it: negative = up, positive = down, delta from
     * centered. */
    int y0 = (panelH - textH) / 2 - BC_INK_TOP * scale + s_yoff;
    if (x0 < 0) x0 = 0;
    /* Clamp so the INKED band stays on-panel regardless of s_yoff. The cell is
     * much taller than the ink (mostly empty rows below), so clamp against the
     * ink extent, not BC_FONT->Height: keep the inked band's top (y0 +
     * BC_INK_TOP*scale) within [0, panelH - BC_INK_H*scale]. */
    int inkTopMin = 0 - BC_INK_TOP * scale;                 /* y0 that puts ink top at panel 0 */
    int inkTopMax = (panelH - BC_INK_H * scale) - BC_INK_TOP * scale;
    if (inkTopMax < inkTopMin) inkTopMax = inkTopMin;
    if (y0 > inkTopMax) y0 = inkTopMax;
    if (y0 < inkTopMin) y0 = inkTopMin;

    GDS_Clear(display, GDS_COLOR_BLACK);

    int x = x0;
    for (const char *p = buf; *p; p++) {
        if (p != buf) x += BC_GAP * scale;      /* gap before every glyph except the first */
        draw_glyph_scaled(*p, x, y0, scale);
        x += glyph_src_width(BC_FONT, *p) * scale;
    }

    GDS_Update(display);
}

static void timer_cb(TimerHandle_t t) {
    if (s_active) draw();
}

void bigclock_activate(void) {
    if (s_active) return;
    s_active = true;
    ESP_LOGI(TAG, "clock activated");
    /* Start SNTP lazily on first activation. By the time the player is powered
     * off and LMS is pushing screensaver frames, the network is necessarily
     * up, so this is a safe, self-contained place to kick off time sync and
     * avoids adding a cross-component hook into the network bring-up code.
     * (bigclock_network_up() is idempotent, so an explicit earlier call from
     * the network layer is also fine if you prefer eager sync.) */
    bigclock_network_up();
    draw();                     /* paint immediately (shows --:-- until synced) */
    if (s_timer) xTimerStart(s_timer, 0);
}

void bigclock_deactivate(void) {
    if (!s_active) return;
    s_active = false;
    ESP_LOGI(TAG, "clock deactivated");
    if (s_timer) xTimerStop(s_timer, 0);
    /* Leave the panel as-is; the displayer will repaint the normal UI on the
     * next LMS frame once the player is back on. */
}

bool bigclock_is_active(void) {
    return s_active;
}
