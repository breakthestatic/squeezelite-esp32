/*
 * bigclock.h - native full-height (64px) standby clock for squeezelite-esp32
 *
 * Draws a large HH:MM clock using the built-in Font_Tarable7Seg_32x64 when the
 * player is powered off, filling the full height of a tall display (e.g. the
 * 256x64 SSD1322). Time is obtained via SNTP; timezone / 12-24h format come
 * from the NVS "clock_config" parameter.
 *
 * This software is released under the MIT License.
 */

#ifndef BIGCLOCK_H
#define BIGCLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init: reads clock_config from NVS, applies TZ. Safe to call before
 * the network is up. Does NOT start SNTP (see bigclock_network_up). */
void bigclock_init(void);

/* Call once the network (WiFi or Ethernet) has an IP address. Starts the SNTP
 * client so the device can learn the wall-clock time. Idempotent. */
void bigclock_network_up(void);

/* Activate / deactivate the big clock. When active, a 1 Hz timer repaints the
 * clock. Activation is driven by the displayer when the player powers off, and
 * deactivation when it powers back on. Idempotent. */
void bigclock_activate(void);
void bigclock_deactivate(void);

/* True while the big clock owns the panel. The displayer uses this to suppress
 * its own (LMS-driven) drawing while the clock is showing. */
bool bigclock_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BIGCLOCK_H */
