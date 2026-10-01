# Big-clock cold-start handback via latched LMS power signal (Approach A)

The bigclock takeover previously keyed on `output.state == OUTPUT_OFF`, but on a cold boot `output.state` sits at `OUTPUT_STOPPED` (0) until the first LMS power-off `aude` lands on a state the stream path has moved — in practice only after a real playback cycle. That left the clock unable to release to the Lyrion UI on the first power-on. This change introduces a displayer-owned `bool bigclock_lms_power_off`, latched unconditionally in `process_aude` from the authoritative `aude->enable_spdif` power bit, defaulted to powered-OFF at cold boot, and read by `grfe_handler` in place of the `output.state` test. The core `output.state` lifecycle, the `powering()` GPIO call, and all other subsystems are left untouched.

Watch for: nothing blocking. The one thing worth a mental note is cross-task access to the flag with no lock on the read side (**confirmed**), which the plan already reasoned about and deliberately accepted; it is not a defect for a single-level `bool`.

**Verdict**: APPROVED

## High-level view

The fix swaps the signal, not the mechanism. `grfe_handler` keeps its exact structure — the `GDS_GetHeight(display) > SB_HEIGHT` guard, the activate/deactivate edge branches, the dirty-repaint on deactivate, and the active-frame skip are all intact — and only the activation condition changes from `output.state == OUTPUT_OFF` to `bigclock_lms_power_off`. This is the minimal edit that moves the clock onto a playback-independent power signal.

The new flag lives in the display component (`extern` in bigclock.h, defined in bigclock.c) rather than widening `struct outputstate`, which keeps the change out of upstream squeezelite semantics. `process_aude` sets it unconditionally from `aude->enable_spdif` as the first statement after `LOCK_O`, so it tracks LMS power intent on every command regardless of `output.state`, and the two existing `OUTPUT_OFF` blocks below it are byte-for-byte unchanged.

The boot default of `true` (powered-OFF) is what actually closes the cold-start gap: before any `aude` arrives, the first `grfe` frame now activates the native clock, and the first real power-ON `aude` clears the flag and releases to the LMS UI. The cross-component include chain that makes the symbol visible in slimproto.c resolves through existing CMake wiring with no build-config change.

The diff also carries unrelated font work (bigclock.c font/scale/centering hunks, gds_font.h, the untracked font source) from the earlier task; none of it touches the cold-start logic. Nothing is committed or staged.

<details>
<summary>Issues (2)</summary>

1. **Unlocked cross-task read (informational)** — `bigclock_lms_power_off` is written under `LOCK_O` in `process_aude` and read without a lock in `grfe_handler` on a different task. For a single aligned `bool` this is a benign torn-read-free level signal on xtensa and was explicitly accepted in the plan; `volatile` is optional, not required. No action needed.
2. **Bundled font changes** — the diff mixes the cold-start fix with pre-existing font/centering work in bigclock.c and gds_font.h plus untracked font files. Out of scope for this review and harmless, but worth separating into its own commit when the user stages these.

</details>

<details>
<summary>Details</summary>

### Latched power signal in process_aude

The flag is assigned as the first statement inside `LOCK_O`/`UNLOCK_O`, ahead of both existing `OUTPUT_OFF` conditionals:

```c
LOCK_O;
bigclock_lms_power_off = !aude->enable_spdif;
if (!aude->enable_spdif && output.state != OUTPUT_OFF) {
    output.state = OUTPUT_OFF;
}
if (aude->enable_spdif && output.state == OUTPUT_OFF && !output.idle_to) {
    ...
}
```

Because the assignment has no `output.state` precondition, it reflects LMS power intent on every `aude` — which is exactly the property the `output.state`-based test lacked at cold boot. `aude->enable_spdif` is the same bit `powering()` already consumes for the physical power GPIO, so the clock is now bound to the same authoritative signal that drives hardware power, and that `powering(aude->enable_spdif)` call is left in place (**confirmed**). The two `output.state` blocks below are unchanged, so the player lifecycle is untouched.

### grfe_handler keys off the flag, structure preserved

Only the activation condition changed; the surrounding block is intact (**confirmed** by reading displayer.c:710-730):

```c
if (GDS_GetHeight(display) > SB_HEIGHT) {
    if (bigclock_lms_power_off) {
        if (!bigclock_is_active()) bigclock_activate();
    } else if (bigclock_is_active()) {
        bigclock_deactivate();
        displayer.dirty = true;	// force a clean repaint of the LMS UI
    }
}
if (bigclock_is_active()) {
    xSemaphoreGive(displayer.mutex);
    return;
}
```

The height guard, the deactivate branch with its dirty-repaint, and the active-frame skip are all preserved. The old `output.state == OUTPUT_OFF` wording now survives only in the explanatory comment, which spells out the cold-boot `OUTPUT_STOPPED` reasoning — a WHY comment is present at both this site and the `process_aude` site (**confirmed**).

### Cross-component symbol visibility

The symbol is declared `extern` once in bigclock.h, defined once in bigclock.c, and the only new include is `#include "bigclock.h"` in slimproto.c (displayer.c already had it). The include resolves because `components/display/CMakeLists.txt` exports `.` in `INCLUDE_DIRS` and the squeezelite component already lists `display` in `PRIV_REQUIRES` (**confirmed** by reading the display CMakeLists). So the header path works and the link succeeds with no CMake change. A full compile was intentionally not run (xtensa toolchain unavailable locally; CI is the build gate), so link success is reasoned from the wiring rather than observed — **likely**, not confirmed by a build.

### Boot default closes the cold-start gap

`bool bigclock_lms_power_off = true;` is a static initializer, so it holds the powered-OFF value before any code runs, independent of init ordering (**confirmed**). On the first `grfe` frame after cold boot, `bigclock_lms_power_off` is `true`, the clock activates, and the first power-ON `aude` (`enable_spdif == 1`) sets the flag `false`, releasing to the LMS UI. This is the exact scenario the bug report described. On-device confirmation after flashing is still the real test and remains outstanding, as the implementer noted.

### Concurrency note

The write happens under `LOCK_O` on the slimproto task; the read in `grfe_handler` runs on the display task with no lock (**confirmed**). For a single aligned `bool` carrying a level, the display side only needs the latest value and the worst case is one frame of latency — no torn read, no missed update. Recorded, not blocking.

</details>

<details>
<summary>Files changed</summary>

Cold-start fix:
- `components/display/bigclock.h` — `extern bool bigclock_lms_power_off;` with doc comment.
- `components/display/bigclock.c` — definition `= true` (powered-OFF boot default) with comment.
- `components/squeezelite/slimproto.c` — `#include "bigclock.h"` and the unconditional latch in `process_aude`.
- `components/squeezelite/displayer.c` — `grfe_handler` condition switched to the flag; comments updated.

Unrelated (pre-existing font task, in the same working tree):
- `components/display/bigclock.c` — font/scale/centering hunks.
- `components/display/core/gds_font.h` — `Font_squeezebox_standard` extern.
- untracked: `components/display/fonts/font_squeezebox_standard.c`, `.agents/`, `tools/`, `BIGCLOCK_HANDOFF.md`.

Full diff: `git -C /Users/cdonovan/Projects/personal/squeezelite-esp32 diff`

</details>
