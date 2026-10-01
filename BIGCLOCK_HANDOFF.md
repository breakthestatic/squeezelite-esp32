# Bigclock feature — session handoff

Context document for continuing the "native full-height standby clock" work in a
new Kiro session. Everything needed to resume is here.

## Goal

Give a squeezelite-esp32 device (3.12" SSD1322, 256x64 OLED) a large standby
clock that fills the full 64px height. The LMS-driven display is hard-capped at
32px (top half) by firmware `SB_HEIGHT=32`, so filling the full panel requires
native firmware rendering. We added a firmware module (`bigclock`) that natively
draws a full-height HH:MM clock when the player is powered off, using SNTP for
time.

Deliverable: a squeezelite-esp32 I2S firmware `.bin` flashable via the device's
web OTA updater (no USB).

## Current status: WORKING (base feature); three changes in progress, UNCOMMITTED

The base feature builds in CI and runs on the device: the clock shows when
powered off, and a power off/on toggle hands control back to the normal
Squeezebox UI. Earlier refinements replaced the original 7-segment font (whose
`:` rendered as a blank gap) with a scaled monospace font plus a real colon, and
12-hour format works via NVS config.

### Changes made this session (NOT yet committed — left in the working tree for review)

The user prefers to review and commit diffs themselves, so these edits are
UNCOMMITTED. They have NOT been built in CI or verified on-device yet; local
verification was limited to inspection + host syntax checks (the xtensa toolchain
segfaults under emulation on the user's arm64 Mac, so idf.py is not run locally).

1. **Authentic VFD font.** Replaced the scaled Droid Sans Mono with
   `Font_squeezebox_standard`, extracted pixel-for-pixel from the real LMS
   `standard.1` bitmap font (the actual Squeezebox Classic/SB3 VFD clock
   typeface — a FreeSans-derived proportional sans-serif). Rendered at integer
   4x scale (16px cell -> 64px). See "The font" section below.
2. **Cold-start handback fix.** On a fresh boot the clock would not release to
   the Lyrion UI on power-on until music had played once. Root cause: the
   takeover keyed off `output.state == OUTPUT_OFF`, but on cold boot
   `output.state` is `OUTPUT_STOPPED` (0), not `OUTPUT_OFF` (-1), until the first
   LMS power `aude` command. Fix (Approach A): latch the real LMS power bit
   (`aude->enable_spdif`) into a new `bool bigclock_lms_power_off` and key the
   displayer takeover off that instead. See "Cold-start fix" below.
3. **NVS vertical offset (`yoff`).** The panel is physically mounted shifted so
   the top-32px UI region is centered in the case; the full-height clock looked
   bottom-weighted. Added a `yoff=<pixels>` key to `clock_config` (delta from
   centered, negative = up, positive = down) so the clock can be repositioned
   live via NVS + reboot, no rebuild. See "Configuration" below.

## Where everything lives

- **Fork (source of truth):** `~/Projects/personal/squeezelite-esp32`
  - Remote: `origin = https://github.com/breakthestatic/squeezelite-esp32.git`
  - Branch: **`bigclock-4.3`** (named to match the `**4.3` CI trigger glob)
  - GitHub account: personal, `breakthestatic`. Git Credential Manager provides
    the token automatically for `github.com` (identity via includeIf in
    `~/.gitconfig` -> `~/.gitconfig-personal`).
  - Latest commit at handoff: `9eed1102` "bigclock: use scaled normal font with
    real colon"
- **Actions:** https://github.com/breakthestatic/squeezelite-esp32/actions
  (workflow builds only the I2S-4MFlash / depth-16 target)
- **OTA image previously downloaded to:** `/tmp/bigclock-ota/squeezelite.bin`
  (temp; may be gone after reboot — re-download from the latest successful run's
  `...-I2S-4MFlash-16-<n>.bin` artifact)
- **Cleanup candidates (superseded, safe to delete):**
  - `~/Downloads/BMPFonts/firmware-bigclock/` — older staging copies of the
    feature files (the fork has newer versions)
  - `~/Downloads/squeezelite-esp32-build/` (~238M) — abandoned local Docker-build
    clone; points at upstream `sle118`, only has throwaway v4.4/emulation hacks
    we chose never to push, no unpushed commits
  - Keep the rest of `~/Downloads/BMPFonts/` (the `.bmp` LMS bitmap fonts etc.)

## Build route (important)

Build via **GitHub Actions CI on the fork**, NOT locally.
- User is on Apple M5 Pro (arm64). The project's Docker images
  (`sle118/idf:release-v4.0`, `sle118/squeezelite-esp32-idfv43`) are amd64-only;
  the xtensa GCC 8.4 toolchain SEGFAULTS under QEMU emulation on arm64.
- Native arm64 `espressif/idf:v4.4.7` exists but porting the project v4.0->v4.4
  broke multiple vendored IDF components. Rejected.
- So: push to `bigclock-4.3`, let CI build, download the `.bin` artifact.

CI details:
- Workflow `esp-idf-v4.3-build.yml` -> image
  `sle118/squeezelite-esp32-idfv4-master` (IDF **v4.3.2**), target
  `I2S-4MFlash`, depth 16.
- Target branch is `master-v4.3` (upstream default). Do NOT use `master-cmake`.
- A working build produces two artifacts:
  `...-I2S-4MFlash-16-<n>.bin` (the ~1.8MB raw squeezelite.bin OTA image — this
  is what you flash) and `...-16-<n>.zip` (full build output).

### CI fixes already applied (so the build is green)
Committed on `bigclock-4.3`:
- Fork-safe build number (`einaregilsson/build-number@v3`, continue-on-error,
  fallback '0'); build only I2S/16; disabled `Platform_build`.
- Bumped `actions/checkout`, `actions/cache`, `actions/upload-artifact` to v4.
- Removed a broken whole-workspace cache (static key restored a stale `build/`
  so the app never recompiled).
- `buildFirmware.sh`: `pip install protobuf==4.25.6 grpcio-tools==1.62.3`
  (fallback to unpinned) before `idf.py build` — cspot's nanopb needs it
  (was `ModuleNotFoundError: No module named 'google'`).
- `components/_override/esp32/i2s.c`: this override was copied from a newer IDF
  and `#include`s `soc/chip_revision.h` + `hal/efuse_hal.h`, absent in v4.3.2.
  Guarded them with `__has_include` and fell back to
  `esp_efuse_get_chip_ver() >= 1` for the ESP32 rev0 APLL workaround.

## The feature — how it works

New module: `components/display/bigclock.c` and `bigclock.h` (placed in the
display component because it has the GDS headers and the global `display`).

- **Trigger:** powered-off only. Detected in
  `components/squeezelite/displayer.c` `grfe_handler` (LMS keeps sending grfe
  screensaver frames while off). When the player is off and
  `GDS_GetHeight > SB_HEIGHT`, call `bigclock_activate()`; otherwise
  `bigclock_deactivate()`. While active, the displayer skips the normal LMS
  frame draw (`bigclock_is_active()` guards on the scroller `GDS_DrawBitmapCBR`
  and the final `GDS_Update`).
  - **Power signal (changed this session):** the takeover now keys off
    `bigclock_lms_power_off` (the latched `aude->enable_spdif` bit), NOT
    `output.state == OUTPUT_OFF`. See "Cold-start fix" below for why. The old
    `output.state == OUTPUT_OFF` test is what caused the cold-boot handback bug.
- **Time:** SNTP started lazily from `bigclock_activate()` (network is
  necessarily up by the time the player is off). Uses classic `sntp_*` API
  (`esp_sntp.h`) for v4.x compat. Shows `--:--` until first sync (time_is_valid
  = year >= 2020).
- **Rendering:** a 1 Hz FreeRTOS timer repaints while active (costs nothing
  during playback since it's stopped then). Draws `Font_squeezebox_standard`
  (the authentic VFD font, see below) at integer **4x scale** (16px cell ->
  64px) by decoding the X-GLCD column-major glyph format directly and blitting
  each source pixel as a scale x scale block via `GDS_DrawPixel`. Vertically
  centered on the font's *inked band* (`BC_INK_H`), not the padded cell, then
  nudged by the NVS `yoff` offset; horizontally centered. `GDS_Clear` +
  `GDS_Update`. Fonts are compiled unconditionally (`SRC_DIRS` includes
  `fonts`), so no `USE_LARGE_FONTS` needed.
  - Glyph format reference (from `components/display/core/gds_font.c`):
    `glyph = [width byte][col0 bytes][col1 bytes]...`, `RoundUpHeight/8` bytes
    per column, `pixel(row i) = bit (i & 7) of byte (i / 8)`. `Font_squeezebox_standard`
    is **proportional** (`Monospace = false`), so the engine draws each glyph's
    own width byte; `glyph_src_width()` and the draw loop honor that.

### The font (authentic Squeezebox VFD reproduction)

`components/display/fonts/font_squeezebox_standard.c` defines
`Font_squeezebox_standard`, generated from the real LMS `standard.1` bitmap font
— the exact typeface the original Squeezebox Classic/SB3 clock screensaver used
(a FreeSans-derived proportional sans-serif, confirmed from Lyrion/SlimServer
`Slim::Plugin::DateTime` which renders the clock with `standard.1`). It is NOT
7-segment and NOT monospace.

- **Generator:** `tools/bigclock-font/gen_font.py` (Python 3 stdlib only) decodes
  `tools/bigclock-font/standard.1.font.bmp` (the real 2296x33x1-bit LMS bitmap,
  vendored into the repo) per the format in SlimServer's `Slim/Display/Lib/Fonts.pm`:
  rows 0-31 are glyphs (column-major), row 32 is the marker row delimiting
  characters; mapping is codepoint == charIndex directly (NOT 0x20+index).
  Emits a contiguous X-GLCD font for `0x20`-`0x5A` (space..`Z`), cell height 16,
  so all needed glyphs (`0`-`9`, `:`, space, `A`/`P`/`M`, `-`) are present.
  Re-run with `python3 tools/bigclock-font/gen_font.py` from the repo root
  (deterministic). Prints an ASCII-art sanity dump of `0 1 : A`.
- **Provenance:** `standard.1` is GPLv2 (LMS) / FreeSans-derived. Noted in the
  generated file header. Fine for a personal fork; flag before any redistribution.
- **12h hour padding:** single-digit hours render with no leading zero and no
  leading space (e.g. `9:05`), then centered — this matches the authentic LMS
  behavior (US/English default format `|%I:%M %p`, whose `|` strips the leading
  zero with no replacement). 24h mode keeps the zero-padded `09:05`. AM/PM is
  intentionally not drawn so the digits stay maximally large.
- **Vertical centering constants** (in `bigclock.c`): `BC_SCALE 4`,
  `BC_INK_TOP 0`, `BC_INK_H 10`. `BC_INK_H` is a slight over-estimate of the real
  inked band (~7px); harmless, affects only centering by a couple of px. Tune if
  the clock sits slightly off-center before reaching for `yoff`.

### Cold-start fix (Approach A)

Bug: on a fresh boot the clock would not release to the Lyrion UI on power-on
until music had played once (after that, power toggling worked until the next
reboot). Root cause: `grfe_handler` keyed off `output.state == OUTPUT_OFF`, but
on cold boot `output.state` initializes to `OUTPUT_STOPPED` (0), not `OUTPUT_OFF`
(-1) — the ESP32 build runs without `-C`, so `output_init_common` picks
`OUTPUT_STOPPED`. `output.state` only reaches `OUTPUT_OFF` via an LMS power-off
`aude` command, and the power-ON `aude` branch only converts `OUTPUT_OFF ->
OUTPUT_STOPPED`. So from cold boot the native takeover sat dormant until a real
play/stop/power-off cycle first drove state to `OUTPUT_OFF`.

Fix: latch the real LMS power bit. In `process_aude` (slimproto.c), set
`bigclock_lms_power_off = !aude->enable_spdif;` unconditionally inside `LOCK_O`
(independent of `output.state`). `grfe_handler` keys the clock takeover off this
flag instead of `output.state`. The flag is declared `extern bool
bigclock_lms_power_off;` in `bigclock.h` (included by both slimproto.c and
displayer.c) and defined `= true` (powered-OFF default) in `bigclock.c`, so a
cold boot shows the clock and the first power-ON `aude` releases it. The physical
power GPIO (`powering(aude->enable_spdif)`) and all `output.state` semantics are
unchanged. Full diagnosis in `.agents/tasks/bigclock-coldstart-investigation.md`.

### displayer.c edits (in `components/squeezelite/displayer.c`)
- `#include "bigclock.h"`
- `extern struct outputstate output;`
- `bigclock_init()` after `sendSETD` in `sb_displayer_init`
- edge-detect activate/deactivate block after `scroller.active = false;` in
  `grfe_handler`; skip the frame if `bigclock_is_active()`. The condition is now
  `if (bigclock_lms_power_off)` (was `if (output.state == OUTPUT_OFF)`).
- `&& !bigclock_is_active()` guards on the scroller `GDS_DrawBitmapCBR` and the
  final `GDS_Update`

### slimproto.c edits (in `components/squeezelite/slimproto.c`) — cold-start fix
- `#include "bigclock.h"`
- in `process_aude`, inside `LOCK_O`: `bigclock_lms_power_off = !aude->enable_spdif;`
  (unconditional, before the existing `output.state` branches)

### CMakeLists (in `components/display/CMakeLists.txt`)
- added `lwip` to `PRIV_REQUIRES`
- `add_compile_definitions(USE_LARGE_FONTS)` (kept; harmless — the scaled font
  no longer depends on it)

## Configuration (NVS key `clock_config`)

Syntax: `tz=<POSIX TZ>[,ntp=<server>][,fmt=12|24][,yoff=<pixels>]`
- `tz` is a **POSIX** TZ string, NOT an Olson name.
- Pacific US + 12-hour (the value the user wanted):
  `tz=PST8PDT,M3.2.0,M11.1.0,ntp=pool.ntp.org,fmt=12`
- `yoff=<pixels>` (added this session): signed vertical offset, a **delta from
  the vertically-centered position**. Negative moves the clock **up**, positive
  **down**, `0` (default) is centered. Clamped so the glyph cell stays on-panel.
  Purpose: the panel is physically mounted shifted (the top-32px LMS UI region is
  centered in the case), so the full-height clock looked bottom-weighted; `yoff`
  repositions it live without a rebuild. Starting guess `yoff=-16` (shifts the
  clock's center up to roughly the panel's top-half center); eyeball and nudge.
  Example: `...,fmt=12,yoff=-16`.
- Set via web UI NVS editor (key `clock_config`) or serial console
  `config set clock_config "..."`. **Reboot** to apply; `bigclock_init()` reads
  NVS once at init (not re-read on each activation).
- 12h mode uses **no padding** for single-digit hours (e.g. `9:05`), matching the
  authentic Squeezebox clock. 24h mode keeps the leading zero (`09:05`). (The old
  leading-space behavior was removed this session.)

## How to flash (OTA, no USB)

1. Download the `...-I2S-4MFlash-16-<n>.bin` artifact from the latest green run
   (GitHub artifact downloads need auth — a token from Git Credential Manager
   works: `git credential fill` for github.com, then curl the
   `archive_download_url` with `Authorization: token <tok>`, unzip to get
   `squeezelite.bin`). Verify first byte is `0xE9` (valid ESP32 image).
2. In the device web UI, open the Firmware/Update tab. Either upload the local
   `squeezelite.bin` file directly, or point the URL field at it served over
   HTTP from the Mac. It writes to `ota_0` (0x150000) and reboots.

## Open items / next steps

- **Three changes are uncommitted in the working tree** (authentic font,
  cold-start fix, `yoff` offset). The user reviews/commits diffs themselves.
  When committing, note the `bigclock.c` diff mixes the font work and the power
  flag — use `git add -p` if separate commits are wanted. Also untracked:
  `components/display/fonts/font_squeezebox_standard.c`, `tools/` (generator +
  vendored bitmap), `.agents/` (workflow reports), this doc.
- **Not yet built in CI or verified on-device.** Push `bigclock-4.3`, let CI
  build, OTA-flash, then verify on the device:
  1. Font looks like the authentic VFD clock (chunky but correct glyph shapes).
  2. Cold boot -> power ON immediately shows the Lyrion UI (no need to play music
     first). This is the key cold-start fix to confirm.
  3. Dial in `clock_config` `yoff` (start `-16`) so the clock is visually
     centered in the case; reboot between tries.
- Set the NVS `clock_config` to the Pacific/12h value above (+ `yoff`) to test.
- Possible follow-up tune: `BC_INK_H` (currently 10) if vertical centering is
  slightly off before applying `yoff`.
- Cleanup of `~/Downloads` build artifacts is pending user's go-ahead.
