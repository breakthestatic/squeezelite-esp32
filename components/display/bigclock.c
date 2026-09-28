/*
 * bigclock.c - native full-height (64px) standby clock for squeezelite-esp32
 *
 * See bigclock.h for the public interface and overview.
 *
 * Design notes
 * ------------
 *  - The device has no wall clock of its own, so we run an SNTP client. Until
 *    the first sync we render "--:--" so we never show a wrong time.
 *  - Drawing uses the display component's text engine with the built-in
 *    Font_Tarable7Seg_32x64 glyph set (32 wide x 64 tall). That font already
 *    covers ' '..'Z', which includes the digits and ':'. It requires the
 *    display component to be built with USE_LARGE_FONTS defined (otherwise
 *    GDS_FONT_SEGMENT falls back to a small font - see gds_text.c).
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

static const char *TAG = "bigclock";

/* The display handle is created by the display component. It is exported there
 * as a global "display" (see components/display/display.c). We reference it. */
extern struct GDS_Device *display;

/* ---- configuration (from NVS "clock_config") -------------------------- */
/* Syntax: tz=<POSIX TZ>[,ntp=<server>][,fmt=12|24]
 * Example: tz=EST5EDT,M3.2.0,M11.1.0,ntp=pool.ntp.org,fmt=12
 */
static char  s_tz[64]   = "UTC0";
static char  s_ntp[64]  = "pool.ntp.org";
static bool  s_fmt12    = false;

static bool        s_active      = false;
static bool        s_sntp_started = false;
static TimerHandle_t s_timer      = NULL;

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
        free(cfg);
    }

    /* Apply timezone so localtime() is correct. */
    setenv("TZ", s_tz, 1);
    tzset();

    ESP_LOGI(TAG, "init tz='%s' ntp='%s' fmt=%s", s_tz, s_ntp, s_fmt12 ? "12h" : "24h");

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
    /* Font is monospace 7-seg; keep it plain HH:MM. Leading space instead of
     * leading zero in 12h mode so single-digit hours look natural. */
    if (s_fmt12 && hour < 10) {
        snprintf(buf, len, "%d:%02d", hour, tm.tm_min);
    } else {
        snprintf(buf, len, "%02d:%02d", hour, tm.tm_min);
    }
}

/* Paint the clock centered on the full display height. */
static void draw(void) {
    if (!display) return;

    char buf[8];
    format_time(buf, sizeof(buf));

    /* Clear the whole panel, then draw the big font centered (both axes).
     * GDS_TEXT_CENTERED anchors at the middle of the display, so on a 64px
     * panel the 64px-tall glyphs fill the height. */
    GDS_TextPos(display, GDS_FONT_SEGMENT, GDS_TEXT_CENTERED,
                GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, buf);
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
