# Bigclock cold-start handback bug — investigation

Repo: `/Users/cdonovan/Projects/personal/squeezelite-esp32` · branch `bigclock-4.3`
Status: READ-ONLY investigation. No code changed.

## Summary answer

The clock→UI handback is driven entirely by one level check in `grfe_handler`:
`output.state == OUTPUT_OFF` activates the clock, anything else deactivates it
(`components/squeezelite/displayer.c:715-721`).

On a **cold boot before any playback**, `output.state` is **not** `OUTPUT_OFF`. It
is initialized to `OUTPUT_STOPPED` (value `0`), because the ESP32 build runs
squeezelite without the `-C` idle-timeout option, so `idle == 0`, and
`output_init_common` does `output.state = idle ? OUTPUT_OFF : OUTPUT_STOPPED;`
(`components/squeezelite/output.c:383`).

Because the firmware is powered off at the LMS level, LMS keeps sending `grfe`
screensaver (clock) frames. Each frame hits `grfe_handler`, sees
`output.state == OUTPUT_STOPPED` (≠ `OUTPUT_OFF`), and so takes the
**deactivate** branch — but `bigclock_is_active()` is still `false` (it was never
activated, since the activate branch requires `OUTPUT_OFF`). Net result: the
clock is never activated by the displayer at all on cold boot… yet the user sees
the clock. That means the clock the user sees on cold boot is **LMS's own
screensaver frame drawn through the normal path**, and pressing power ON cannot
"release" anything because the native bigclock takeover is not the thing on
screen — and critically the native path is wedged in a state where it will not
behave until `output.state` first reaches `OUTPUT_OFF`.

`output.state` only reaches `OUTPUT_OFF` through one of two routes
(`components/squeezelite/slimproto.c`):

1. The LMS **power-off** command `aude` with `enable_spdif == 0`
   (`process_aude`, slimproto.c:462-464) — but see the asymmetry below.
2. The idle timeout path, which is disabled when `idle_to == 0`
   (slimproto.c:772-775).

The asymmetry in `process_aude` (slimproto.c:462-469) is the mechanism that makes
"play once, then it works forever":

```c
if (!aude->enable_spdif && output.state != OUTPUT_OFF) {
    output.state = OUTPUT_OFF;                 // power OFF: always honored
}
if (aude->enable_spdif && output.state == OUTPUT_OFF && !output.idle_to) {
    output.state = OUTPUT_STOPPED;             // power ON: only from OUTPUT_OFF
    output.stop_time = gettime_ms();
}
```

Power-OFF reliably drives `output.state` to `OUTPUT_OFF`. Once that has happened
even once (which is what a real play→stop→power-off cycle guarantees), the
displayer's `OUTPUT_OFF` check starts matching, the native bigclock activates on
OFF and deactivates on ON, and the handshake works for the rest of the session.
Before any such cycle, `output.state` sits at `OUTPUT_STOPPED` and the native
clock takeover never engages correctly.

Short version: **the activate/deactivate edge logic keys off `OUTPUT_OFF`, but on
cold boot the player's "off" condition is represented as `OUTPUT_STOPPED`, not
`OUTPUT_OFF`. `output.state` does not become `OUTPUT_OFF` until the first LMS
power-off `aude` command lands on a state that the stop/stream path has moved —
which in practice first happens after a real playback cycle.**

## Evidence

### 1. The `output_state` enum and its values

`components/squeezelite/squeezelite.h:649-650`:

```c
typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING,
			   OUTPUT_PAUSE_FRAMES, OUTPUT_SKIP_FRAMES, OUTPUT_START_AT } output_state;
```

Numeric values: `OUTPUT_OFF = -1`, `OUTPUT_STOPPED = 0`, `OUTPUT_BUFFER = 1`,
`OUTPUT_RUNNING = 2`, `OUTPUT_PAUSE_FRAMES = 3`, `OUTPUT_SKIP_FRAMES = 4`,
`OUTPUT_START_AT = 5`.

Note that `OUTPUT_OFF` is `-1`, i.e. it is the ONLY negative value and it is NOT
the zero-initialized value. A `memset(&output, 0, …)` yields `OUTPUT_STOPPED`,
not `OUTPUT_OFF`.

### 2. Initial / boot value of `output.state`

The global is a plain (zero-initialized at load) struct:
`components/squeezelite/output.c:28` — `struct outputstate output;`

The embedded init path explicitly zeroes it, then sets the state:

`components/squeezelite/output_embedded.c:88` —
```c
memset(&output, 0, sizeof(output));          // state := 0 == OUTPUT_STOPPED
output_init_common(level, device, output_buf_size, rates, idle);
```

`components/squeezelite/output.c:383` (inside `output_init_common`):
```c
output.state = idle ? OUTPUT_OFF: OUTPUT_STOPPED;
```

`idle` comes from the `-C <timeout>` CLI option and defaults to `0`:
`components/squeezelite/main.c:301` (`unsigned idle = 0;`) and
`main.c:417-420` (only `-C` sets it, as `atoi(optarg) * 1000`). The ESP32 build
calls `output_init_embedded(..., idle)` at `main.c:770`. Unless the user's
`autoexec` NVS command line passes `-C`, **`idle == 0`, so the boot value of
`output.state` is `OUTPUT_STOPPED` (0), never `OUTPUT_OFF`.**

The i2s output thread does NOT independently push the state to `OUTPUT_OFF` at
boot. It only *reacts* to `output.state` for amp/LED/standby, and its local
`state` tracker starts at `OUTPUT_OFF - 1` (`output_i2s.c:526`) purely to force a
first-iteration log/transition; it never assigns `output.state`
(`output_i2s.c:537-568`).

So at cold boot, before any playback and before any LMS `aude` power command,
`output.state == OUTPUT_STOPPED`.

### 3. The exact edge-detect block in `grfe_handler`

`components/squeezelite/displayer.c:713-726`:

```c
// --- Big Clock power-state edge detection & takeover ------------------
// Activate when the player is off; deactivate as soon as it is back on.
// Only take over on panels tall enough to benefit (height > SB_HEIGHT).
if (GDS_GetHeight(display) > SB_HEIGHT) {
    if (output.state == OUTPUT_OFF) {
        if (!bigclock_is_active()) bigclock_activate();
    } else if (bigclock_is_active()) {
        bigclock_deactivate();
        displayer.dirty = true;	// force a clean repaint of the LMS UI
    }
}
// While the clock owns the panel, ignore this LMS frame entirely.
if (bigclock_is_active()) {
    xSemaphoreGive(displayer.mutex);
    return;
}
```

This is purely **level-based on `output.state`**; there is no remembered previous
state and no dependence on any dedicated power signal. Its full truth table:

| `output.state` | `bigclock_is_active()` before | action |
|---|---|---|
| `OUTPUT_OFF` | false | `bigclock_activate()` → clock on |
| `OUTPUT_OFF` | true | (no-op; clock stays on) |
| any ≠ `OUTPUT_OFF` | true | `bigclock_deactivate()` → clock off, repaint LMS |
| any ≠ `OUTPUT_OFF` | false | nothing — pass through to normal LMS draw |

Cold-boot trace (powered off from the user's view, `output.state ==
OUTPUT_STOPPED`): every `grfe` frame lands in the last row — state ≠ `OUTPUT_OFF`
and clock inactive → the block does nothing, and the frame falls through to the
normal LMS draw path. The native bigclock is never engaged, so there is nothing
for a power-ON to "release", and the normal path keeps rendering whatever LMS
pushes (its clock screensaver). Pressing power ON sends an `aude` with
`enable_spdif == 1`, but `process_aude` only converts `OUTPUT_OFF → OUTPUT_STOPPED`
— from `OUTPUT_STOPPED` it is a no-op, so nothing changes and the display does
not switch to the Lyrion now-playing UI as expected.

Why one playback cycle fixes it permanently (until reboot): a real play then
stop then power-off sequence drives `output.state` through `OUTPUT_BUFFER` /
`OUTPUT_RUNNING` and eventually a power-OFF `aude` sets `output.state =
OUTPUT_OFF` (slimproto.c:463-464) — the first branch has no state precondition
beyond `!= OUTPUT_OFF`, so it always succeeds. From then on the pair is
symmetric: OFF→`OUTPUT_OFF` (clock activates), ON→`OUTPUT_STOPPED` (clock
deactivates), and the displayer's `OUTPUT_OFF` check matches reality on every
toggle. State is held in RAM, so it resets to `OUTPUT_STOPPED` on the next cold
boot and the bug returns.

### 4. Everywhere `output.state` is assigned in response to power on/off

The dedicated LMS power command is `aude` (`struct aude_packet`,
slimproto.h:131-136, fields `enable_spdif` and `enable_dac`). It is handled in
`process_aude` (`components/squeezelite/slimproto.c:452-470`):

```c
#if EMBEDDED
    powering(aude->enable_spdif),          // drives the power_control GPIO (embedded.c:110)
#endif
    LOCK_O;
    if (!aude->enable_spdif && output.state != OUTPUT_OFF) {
        output.state = OUTPUT_OFF;                       // POWER OFF
    }
    if (aude->enable_spdif && output.state == OUTPUT_OFF && !output.idle_to) {
        output.state = OUTPUT_STOPPED;                   // POWER ON (only from OFF)
        output.stop_time = gettime_ms();
    }
    UNLOCK_O;
```

Other assignments of `output.state` that touch `OUTPUT_OFF` / `OUTPUT_STOPPED`
(not power commands per se, but they move state in and out of these values and
therefore affect when the `process_aude` ON-branch can fire):

- `components/squeezelite/output.c:383` — init: `output.state = idle ? OUTPUT_OFF : OUTPUT_STOPPED;`
- `components/squeezelite/output.c:450-451` — `output_flush`: if `!= OUTPUT_OFF`, set `OUTPUT_STOPPED`.
- `components/squeezelite/slimproto.c:334-335` — pause with no interval: if `!= OUTPUT_OFF`, set `OUTPUT_STOPPED`.
- `components/squeezelite/slimproto.c:755-756` — stream start: `OUTPUT_STOPPED`/`OUTPUT_OFF` → `OUTPUT_BUFFER`.
- `components/squeezelite/slimproto.c:764`, `:772-774` — underrun/idle-timeout → `OUTPUT_STOPPED` then (only if `idle_to`) `OUTPUT_OFF`.
- `components/squeezelite/decode_external.c` (BT/RAOP/CSpot) — set `OUTPUT_STOPPED`/`OUTPUT_RUNNING` on external-source events (e.g. :152, :166, :171, :290, :378, :395).

The **power-off GPIO** is driven by `powering()` (`embedded.c:110-115`) directly
from `aude->enable_spdif`, independent of `output.state`. This is important: the
*physical* power signal from LMS is `aude`'s `enable_spdif` bit, which is a clean,
playback-independent indicator — unlike `output.state`.

## Conclusions

- The displayer infers power purely from `output.state == OUTPUT_OFF`, but the
  canonical boot/off representation in this firmware without `-C` is
  `OUTPUT_STOPPED`. `OUTPUT_OFF` is a `-1` sentinel that is reached only via an
  LMS power-off `aude` command (slimproto.c:463) or the idle timeout
  (slimproto.c:773, disabled here). It is NOT the zero-init value.
- There is a true LMS power signal available — `aude->enable_spdif` in
  `process_aude` — that reflects on/off regardless of whether audio has played.
  The displayer currently ignores it and reads `output.state` instead.
- The edge logic is also fragile because it is level-triggered with no latched
  "previous power state", so it can only ever react to whatever `output.state`
  happens to be when a `grfe` frame arrives.

## Candidate fixes (not implemented)

### A. Track the LMS power signal directly in `process_aude`
Add a module-level power flag (e.g. `output.power_on` or a displayer-visible
`bool`) set in `process_aude` from `aude->enable_spdif`, and have `grfe_handler`
key activation off that flag instead of `output.state == OUTPUT_OFF`.
- Pros: uses the authoritative, playback-independent power signal; fixes the
  cold-start case directly; small, localized change.
- Cons: adds a new shared field and a cross-module read; must initialize the flag
  to a sane boot default (powered on vs off) consistent with how the device
  behaves before the first `aude` arrives.

### B. Treat `OUTPUT_STOPPED` as "off" for the clock, with care
Broaden the displayer check so the clock also activates on the off-like idle
state, e.g. activate when `output.state == OUTPUT_OFF || output.state ==
OUTPUT_STOPPED`, deactivate on buffering/running states.
- Pros: no new state; one-line-ish change in `grfe_handler`.
- Cons: changes semantics — `OUTPUT_STOPPED` also occurs between tracks and after
  stop while still powered ON, so the big clock could appear during normal
  powered-on idle, not just standby. Would likely cause the clock to flash on in
  situations the user considers "on". Risky without additional qualification.

### C. Normalize `output.state` at boot / make `aude` power-ON robust
Either initialize the player to `OUTPUT_OFF` at boot when there is no idle
timeout (so "powered off" is represented consistently from frame zero), or relax
the `process_aude` power-ON branch so a power-ON command normalizes any non-OFF
state (and conversely ensure OFF is set) rather than only converting from
`OUTPUT_OFF`.
- Pros: fixes the asymmetry at the source; benefits any other code that assumes
  `OUTPUT_OFF == powered off`.
- Cons: touches core squeezelite output/slimproto semantics shared with the
  upstream project; higher blast radius; the i2s thread, amp GPIO, and LED
  behavior all branch on these states, so changing the boot value could alter
  amp/standby behavior on cold boot and must be validated on hardware.

## Recommended approach

**Approach A** — carry the real LMS power state from `process_aude`
(`aude->enable_spdif`) and have the displayer's bigclock takeover key off that
signal rather than `output.state == OUTPUT_OFF`.

Rationale: `enable_spdif` is exactly the on/off intent LMS is communicating, and
it is already handled in one place (`process_aude`, slimproto.c:452-470) that
also drives the physical power GPIO via `powering()`. Binding the clock to the
same signal makes "clock shows when powered off" true from the first frame after
boot, independent of whether audio has ever played, which is precisely the
cold-start gap. It is more localized and lower-risk than retuning the shared
`output.state` lifecycle (Approach C) and avoids the false-positive standby-clock
behavior that broadening the state test (Approach B) would introduce. Care point:
pick the correct boot default for the new flag so the very first `grfe` frame
before any `aude` behaves as intended (the device is "off" at cold boot from the
user's view, so defaulting the flag to off and letting the first power-ON `aude`
clear it is the natural choice).

## Files examined

- `components/display/bigclock.c`, `bigclock.h`
- `components/squeezelite/displayer.c` (`grfe_handler`, `sb_displayer_init`)
- `components/squeezelite/squeezelite.h` (`output_state` enum, `struct outputstate`)
- `components/squeezelite/output.c` (`output_init_common`, `output_flush`, global `output`)
- `components/squeezelite/output_embedded.c` (`output_init_embedded`, boot `memset`)
- `components/squeezelite/output_i2s.c` (output thread state handling)
- `components/squeezelite/slimproto.c` (`process_aude`, `process_audg`, stream/idle state transitions)
- `components/squeezelite/slimproto.h` (`struct aude_packet`)
- `components/squeezelite/embedded.c` (`powering`)
- `components/squeezelite/main.c` (`idle` / `-C` option, `output_init_embedded` call)
