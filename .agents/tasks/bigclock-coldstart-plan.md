# VERIFICATION NOTE (appended by implementer — Approach A, first iteration)

Local toolchain build was intentionally NOT run: the xtensa toolchain segfaults
on this arm64 Mac, and the modified `.c` files pull in heavy ESP-IDF/squeezelite
headers so a host gcc compile is not meaningful. Verified by inspection + greps.
On-device verification after OTA flashing is still required (cold boot -> power
on should immediately show the Lyrion UI without needing to play music first).

## Flag declared once, set unconditionally, initialized OFF, read in grfe_handler

```
$ grep -rn "bigclock_lms_power_off" components/
components/display/bigclock.c:64:bool bigclock_lms_power_off = true;              # definition, boot default = OFF
components/display/bigclock.h:45:extern bool bigclock_lms_power_off;              # single extern declaration
components/squeezelite/displayer.c:713: // ...key off bigclock_lms_power_off...   # comment
components/squeezelite/displayer.c:719:  if (bigclock_lms_power_off) {            # READ in grfe_handler
components/squeezelite/slimproto.c:468:  bigclock_lms_power_off = !aude->enable_spdif;  # SET in process_aude
```

- Declared exactly once (`extern` in bigclock.h:45), defined exactly once
  (bigclock.c:64) with the powered-OFF boot default `= true`.
- Set in `process_aude` at slimproto.c:468 UNCONDITIONALLY from
  `aude->enable_spdif`, inside `LOCK_O`/`UNLOCK_O` but OUTSIDE both existing
  `OUTPUT_OFF` `if` blocks (it is the first statement after `LOCK_O;`), so it
  tracks LMS power intent on every aude regardless of `output.state`.
- Read in `grfe_handler` at displayer.c:719, inside the unchanged
  `GDS_GetHeight(display) > SB_HEIGHT` guard.

## #include lines proving the symbol is visible in BOTH translation units

```
$ grep -n '#include "bigclock.h"' components/squeezelite/slimproto.c components/squeezelite/displayer.c
components/squeezelite/slimproto.c:27:#include "bigclock.h"   # NEW — added after slimproto.h (sets the flag)
components/squeezelite/displayer.c:19:#include "bigclock.h"   # pre-existing (reads the flag)
```

The `squeezelite` component already lists `display` in PRIV_REQUIRES and the
`display` component exports `.` in INCLUDE_DIRS, so `#include "bigclock.h"`
resolves from slimproto.c with no CMake change.

## No OTHER use of output.state was removed or altered

```
$ grep -n "output.state == OUTPUT_OFF" components/squeezelite/displayer.c components/squeezelite/slimproto.c
components/squeezelite/displayer.c:714:  // ...rather than output.state == OUTPUT_OFF: on a cold boot   # now only in a comment
components/squeezelite/slimproto.c:472:  if (aude->enable_spdif && output.state == OUTPUT_OFF && !output.idle_to) {  # UNCHANGED
components/squeezelite/slimproto.c:762:  if (_start_output && (output.state == OUTPUT_STOPPED || output.state == OUTPUT_OFF)) {  # UNCHANGED
```

The only functional `output.state == OUTPUT_OFF` test in `grfe_handler` was
replaced by `bigclock_lms_power_off`; the surviving displayer.c:714 match is the
explanatory comment. slimproto.c's own `output.state` lifecycle (incl. the two
blocks in `process_aude`) is untouched. No change to amp-GPIO
(`powering(aude->enable_spdif)` kept), i2s thread, LED, SNTP, NVS, font, or
`format_time`.

## git status / git diff --stat (changes present and UNCOMMITTED)

```
$ git status
On branch bigclock-4.3
Your branch is up to date with 'origin/bigclock-4.3'.
Changes not staged for commit:
	modified:   components/display/bigclock.c
	modified:   components/display/bigclock.h
	modified:   components/display/core/gds_font.h
	modified:   components/squeezelite/displayer.c
	modified:   components/squeezelite/slimproto.c
Untracked files:
	.agents/
	BIGCLOCK_HANDOFF.md
	components/display/fonts/font_squeezebox_standard.c
	tools/
no changes added to commit (use "git add" and/or "git commit -a")

$ git diff --stat
 components/display/bigclock.c      | 43 +++++++++++++++++++++++++------------
 components/display/bigclock.h      |  8 +++++++
 components/display/core/gds_font.h |  2 ++
 components/squeezelite/displayer.c | 10 ++++++---
 components/squeezelite/slimproto.c |  7 +++++++
 5 files changed, 54 insertions(+), 16 deletions(-)
```

Cold-start fix touches only: bigclock.h (+8, the extern), bigclock.c (the single
`bigclock_lms_power_off = true;` global + comment), displayer.c (the one
condition + comment), slimproto.c (the include + the one assignment + comment).
The other hunks in bigclock.c and gds_font.h plus the untracked
font_squeezebox_standard.c / .agents / tools / BIGCLOCK_HANDOFF.md are
pre-existing uncommitted work from the earlier font task, not this change.
NOTHING is committed or staged.

---

# Bigclock cold-start handback — implementation plan (Approach A)

Repo: `/Users/cdonovan/Projects/personal/squeezelite-esp32` · branch `bigclock-4.3`
Source of truth: `.agents/tasks/bigclock-coldstart-investigation.md` (read first).
Scope: implement Approach A only. Do NOT touch `output.state` lifecycle, the i2s
thread, amp/LED/standby, SNTP, NVS parsing, the font, or `format_time`. Do NOT
commit.

## Design decisions (locked, with rationale)

- **Flag storage = displayer-owned extern bool, declared in `bigclock.h`, defined
  in `bigclock.c`.** Rationale: keeps the new signal out of core squeezelite
  semantics (does not widen `struct outputstate` or add meaning to `output.state`
  values), and `bigclock.h` is the one header both edit sites can already see.
  The flag is named `bigclock_lms_power_off` (true = LMS says powered OFF = clock
  should show).
- **Cross-file visibility is already satisfied for displayer.c and only needs one
  new `#include` in slimproto.c.** Confirmed: `displayer.c:19` already does
  `#include "bigclock.h"`. `slimproto.c` currently includes only
  `squeezelite.h` (`:25`) and `slimproto.h` (`:26`). The `squeezelite` component
  `CMakeLists.txt` already lists `display` in `PRIV_REQUIRES`, and the `display`
  component (`components/display/CMakeLists.txt`) exports `.` in `INCLUDE_DIRS`,
  so `bigclock.h` resolves from slimproto.c with a plain `#include "bigclock.h"`
  and **no build-config change**.
- **Boot default = powered OFF (`true`).** Rationale (from investigation): at cold
  boot the device is "off" from the user's view and LMS has not sent an `aude`
  yet; defaulting the flag to OFF makes the first `grfe` frame activate the clock,
  and the first real power-ON `aude` (`enable_spdif == 1`) clears it. We set the
  default at the C static-initializer level so it is correct before any code runs,
  independent of init ordering — no init function needed.
- **Set the flag UNCONDITIONALLY in `process_aude` from `enable_spdif`**, outside
  the existing `OUTPUT_OFF` guards, so it tracks LMS power intent on every command
  regardless of `output.state`. The existing `output.state` logic and the
  `powering(aude->enable_spdif)` GPIO call are left exactly as-is.

## Plan

- [ ] 1. Declare the shared power-off flag in `bigclock.h`.
      Add, inside the `extern "C"` block (e.g. just after `bigclock_is_active()`'s
      declaration), a documented `extern bool bigclock_lms_power_off;` with a
      comment: LMS power intent latched from the `aude` command's `enable_spdif`
      bit; true = powered OFF (show clock). Note it defaults to OFF at cold boot
      because `output.state` is `OUTPUT_STOPPED` (not `OUTPUT_OFF`) before the
      first `aude`. (`<stdbool.h>` is already included at `bigclock.h:16`.)
      Files: `components/display/bigclock.h`
      Verify: covered by the build in item 5 (header compiles as part of both
      translation units).

- [ ] 2. Define the flag with its boot default in `bigclock.c`.
      Add `bool bigclock_lms_power_off = true;` at file scope (near the other
      module globals, around `bigclock.c:56-58` where `s_active` etc. live; place
      it as a non-static global so it has external linkage matching the header).
      Add a one-line comment: initialized to powered-OFF so the first `grfe` frame
      after cold boot shows the clock and the first power-ON `aude` releases it.
      Files: `components/display/bigclock.c`
      Verify: covered by the build in item 5.

- [ ] 3. Include `bigclock.h` and set the flag in `process_aude` (slimproto.c).
      Add `#include "bigclock.h"` to the include block (after `#include
      "slimproto.h"` at `slimproto.c:26`). In `process_aude` (`slimproto.c:452`,
      body `:462-470`), add a single assignment that runs every call,
      UNCONDITIONALLY and OUTSIDE the two `OUTPUT_OFF` `if` blocks:
      `bigclock_lms_power_off = !aude->enable_spdif;` — place it right after the
      `LOCK_O;` / before (or after) the existing `output.state` blocks is fine;
      it does not depend on `output.state`. Add a WHY comment: on cold boot
      `output.state` is `OUTPUT_STOPPED`, not `OUTPUT_OFF`, so the displayer must
      key the clock off this real LMS power signal (`enable_spdif`) instead of
      `output.state`. Leave the existing `output.state` blocks and the
      `powering(aude->enable_spdif)` call untouched.
      Files: `components/squeezelite/slimproto.c`
      Verify: covered by the build in item 5 (confirms the symbol links from the
      squeezelite component into the display component).

- [ ] 4. Switch the `grfe_handler` takeover test to the new flag (displayer.c).
      In the block at `displayer.c:713-726`, replace the condition
      `if (output.state == OUTPUT_OFF) {` with
      `if (bigclock_lms_power_off) {`. Keep everything else in the block exactly
      as-is: the outer `if (GDS_GetHeight(display) > SB_HEIGHT)` guard, the
      `else if (bigclock_is_active())` deactivate branch with
      `displayer.dirty = true;`, and the following
      `if (bigclock_is_active()) { ...give mutex; return; }`. Update the two
      comment lines above the block to say activation now follows the LMS power
      signal (`bigclock_lms_power_off`) rather than `output.state == OUTPUT_OFF`,
      and note WHY (cold-boot `output.state` is `OUTPUT_STOPPED`). No new include
      needed — `bigclock.h` is already included at `displayer.c:19`.
      Files: `components/squeezelite/displayer.c`
      Verify: covered by the build in item 5.

- [ ] 5. Build the firmware and confirm it compiles and links cleanly.
      This is the real verification: there is no firmware unit-test harness in
      this repo, so a clean full build that resolves `bigclock_lms_power_off`
      across the `squeezelite` and `display` components is the pass condition.
      Command (needs an ESP-IDF v4.3.x environment; `idf.py` is not on PATH in
      this shell):
      - With a local ESP-IDF env sourced: `./buildFirmware.sh` (sets target
        sdkconfig, then runs `idf.py build -DDEPTH=16 -DBUILD_NUMBER=500-16`), or
        directly `idf.py build` after copying a `build-scripts/*-sdkconfig.defaults`
        to `sdkconfig`.
      - Or via the pinned maintainer image:
        `docker run -it -v "$PWD":/workspace/squeezelite-esp32 sle118/squeezelite-esp32-idfv435`
        then `cd /workspace/squeezelite-esp32 && idf.py build`.
      Files: none (build only)
      Expected outcome: build completes and produces `build/squeezelite.bin` with
      no compile or link errors; in particular no undefined-reference to
      `bigclock_lms_power_off` (proves the header include chain works) and no
      `OUTPUT_OFF`-related warnings from the changed `grfe_handler` block.
      If no ESP-IDF environment is available to the implementer, state that the
      change could not be built locally and that the GitHub Actions
      `Platform_build.yml` workflow (same Docker image) is the fallback gate.

## Confirmed file:line anchors (verified during exploration)

- `components/display/bigclock.h:16` — `#include <stdbool.h>` present;
  `:35` — `bool bigclock_is_active(void);` (declare the extern just after this).
- `components/display/bigclock.c:23` — `#include "bigclock.h"`;
  `:56-58` — existing `s_active` / `s_sntp_started` / `s_timer` globals
  (define the new global here).
- `components/squeezelite/slimproto.c:25-26` — current includes
  (`squeezelite.h`, `slimproto.h`); add `#include "bigclock.h"` after `:26`.
- `components/squeezelite/slimproto.c:452` — `static void process_aude(...)`;
  `:460` — `LOCK_O;`; `:461-464` — power-OFF `OUTPUT_OFF` block;
  `:465-468` — power-ON block; `:469` — `UNLOCK_O;` (add the unconditional
  `bigclock_lms_power_off = !aude->enable_spdif;` within LOCK/UNLOCK, outside both
  `if` blocks).
- `components/squeezelite/displayer.c:19` — `#include "bigclock.h"` already present.
- `components/squeezelite/displayer.c:710-726` — the big-clock edge block;
  `:714` — `if (output.state == OUTPUT_OFF) {` (the line to change).
- Build wiring: `components/squeezelite/CMakeLists.txt` PRIV_REQUIRES includes
  `display`; `components/display/CMakeLists.txt` INCLUDE_DIRS includes `.` —
  so `#include "bigclock.h"` resolves from slimproto.c with no CMake change.

## Notes / assumptions

- The investigation's line hints (`452-470`, `713-726`) match the current files
  within a line or two; anchors above are the re-verified current positions.
- No change to `squeezelite.h`/`slimproto.h` is required or made.
- The flag is a plain `bool` written under `LOCK_O` in `process_aude` and read in
  `grfe_handler`; both run on controller/display task context. A single aligned
  `bool` read/write is atomic on the ESP32 (xtensa) and the displayer only needs
  the latest level, so no additional locking is introduced — consistent with the
  investigation's "localized, minimal" intent. If the implementer wants belt-and-
  braces, `volatile bool` is acceptable, but not required and not core to the fix.
