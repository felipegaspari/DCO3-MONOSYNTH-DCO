## DCO “Autotune” Overview

This document explains how the autotune (calibration) code for the DCO synth oscillators works after the 2026 cleanup pass. For the file split and structural conventions (`autotune_context.h`, `autotune_measurement.h`, etc.), see [`AUTOTUNE_REFACTORED.md`](AUTOTUNE_REFACTORED.md). Runtime amp-comp after calibration depends on the active engine flags in [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).

### Objectives

- **Primary goal**: Build, for each DCO, a mapping from **note frequency → range PWM value** (`ampCompCalibrationVal`) so that:
  - The DCO waveform’s duty cycle is close to the desired (≈50% or other target), and
  - The perceived amplitude remains more consistent across the keyboard.
- **Secondary goals**:
  - Find and store **PW center** and low/high limits for the voice.
  - Discover the oscillator’s **highest usable frequency** when the table hits the top of the PWM range.
  - Persist calibration tables so that the runtime engine can perform fast interpolation instead of re-measuring.

### Hardware Context (high level)

- **MCU**: RP2040 / RP2350 (Pico 2 monosynth board).
- **Architecture**: 1 MIDI voice × **3 oscillators** (OSC1–3).
- **Control signals**:
  - A **range PWM** per oscillator (amplitude / duty compensation).
  - A shared **PW PWM** on voice 0.
  - Oscillators are driven by **three SMs on pio0** (indices from `VOICE_TO_SM`, permuted by `assign_sm_mapping()`; OSC1↔OSC2 sync, OSC3 free-running).
- **Measurement input**:
  - A digital pin (`DCO_calibration_pin`) receives the DCO signal.
  - The code times rising and falling edges using `micros()` to infer duty cycle.

### File layout

| File | Contents |
|------|----------|
| `autotune.h` | Module globals, `note_to_freq()`, PW-search types (`PWSearchState`, `PWRecordMode`, `PWLimitDir`, `PWLimitSearchResult`), prototypes |
| `autotune_constants.h` | `constexpr` constants (`kGapTimeoutSentinel`, `kGapTimeoutUs`, `kEdgeDebounceMinUs`, duty targets/tolerances) |
| `autotune_context.h` | `DCOCalibrationContext` (bundles refs to the per-DCO calibration state) |
| `autotune_measurement.h` | `GapMeasurement` + `measure_gap()` wrapper over `find_gap()` |
| `autotune.ino` | Orchestration (`DCO_calibration`), PW center/limit searches, `find_gap()` measurement core, manual-cal debug |
| `autotune_search.ino` | `calibrate_DCO()` per-note amp search, `find_highest_freq` / `find_lowest_freq`, interpolation helpers |

The old `PID.h` / `PID.ino` are gone: the live calibration never actually used the PID controller, and the one PID user (`find_highest_freq`) was rewritten as a bisection search. The `PID_v1` library under `_build_libs/` is no longer referenced.

---

## Data Structures and Globals

- **Flags and indices** (from `autotune.h`):
  - `calibrationFlag`, `manualCalibrationFlag`, `firstTuneFlag`
  - `currentDCO`, `DCO_calibration_current_note`
  - `manualCalibrationStage`, `manualCalibrationOffset[NUM_OSCILLATORS]`
- **Calibration table**:
  - `calibrationData[chanLevelVoiceDataSize]`
    - Flat array of `uint32_t` pairs: `[freq0, pwm0, freq1, pwm1, ...]`
    - `freq` values are `note_to_freq(note) * 100`
    - `pwm` values are the range PWM that produced the best duty at that frequency
    - Entries `[0..1]` are the "lowest frequency" anchor, `[2..3]` the manual starting point
- **Timers**: `DCOCalibrationStart` (millis at pass start; feeds the PW searches' 60 s timeouts)
- **Highest-frequency search**: `calibrationFreqHz` — frequency override consumed by `voice_task_autotune()` mode 4

The edge-timing state that used to live in header globals (`pulseCounter`, `samplesCounter`, `risingEdgeTimeSum`, `edgeDetectionLastTime`, `DCO_calibration_difference`, …) is now local to `find_gap()`.

---

## Measurement Core

### `find_gap(byte specialMode)`

The **core measurement primitive** used by every calibration routine (normally via the `measure_gap()` wrapper, which converts the timeout sentinel into a `GapMeasurement{value, timedOut}`).

- **Inputs**: `specialMode == 2` uses 12 accepted samples (PW searches), anything else 6.
- **Process**:
  - Computes the ideal period for `DCO_calibration_current_note` and derives a per-edge interval gate (≈1%–99% of the period) to reject glitches.
  - Loops reading `digitalRead(DCO_calibration_pin)` (polarity-compensated via `kGapPolarityInverted`), timing debounced edges (`kEdgeDebounceMinUs`).
  - If no edge is accepted for `kGapTimeoutUs`, it logs `[GAP_TIMEOUT] raw= edges= rejected= accepted= …` and returns `kGapTimeoutSentinel`.
  - After the first aligned pulses, each edge-to-edge interval that passes the gate is accumulated into the falling/rising sums.
- **Output**: `avgHighUs − avgLowUs` in microseconds (0 for a symmetric 50% duty), following the relation `duty − 0.5 = diff / (2·T)`.
- **Sign chain**: the segment attribution and polarity handling are the field-validated legacy convention. `measure_gap_for_amp()` (amp search) flips the sign so that **positive = amplitude too low**; the PW searches use the raw value with `duty = 0.5 + gap/(2T)` throughout.

### Related helpers

- **`measure_gap(specialMode)`** (`autotune_measurement.h`): wraps `find_gap`, sets `timedOut` when the sentinel comes back.
- **`DCO_calibration_debug()`**: manual-cal path; wraps `measure_gap(0)`, prints `[MANUAL_GAP]` (`gapUs` / `dutyErr` or `TIMEOUT`), TXes `PARAM_GAP_FROM_DCO` (154). On timeout also runs `cal_sense_probe_log()` → `[CAL_SENSE]`.
- **`cal_sense_probe_log()`**: 40 ms raw `digitalRead` window (no period gate). Logs `raw`, `edges`, `minDt`/`maxDt`, pull-up/invert flags, note and `expectHz`. Throttled ~2 Hz.

### Cal-sense bench checks (manual cal)

Pin: `DCO_calibration_pin` = **GP6** (temporary A/B on Pico header; was **GP10**; GP25 aborted — Pico LED / not on header). Feed the signal into the pin shown in `[CAL_SENSE] pin=`. Setup uses `pinMode` on that GPIO (`INPUT` / `INPUT_PULLUP` as configured in `DCO.ino`). Manual cal note = `manual_DCO_calibration_start_note` (24) → **C0 ≈ 16.35 Hz**. RP2040/RP2350 at 3.3 V IO: **VIH ≥ 2.0 V**, **VIL ≤ 0.8 V** (2.48 V is a valid HIGH; detection still needs edges that go below VIL).

Enter manual cal (param 151). On timeout USB shows `[MANUAL_GAP] … TIMEOUT`, `[GAP_TIMEOUT] … raw= edges= rejected= accepted= …`, and throttled `[CAL_SENSE] pin=6 …`.

| Test | Expected Serial |
|------|-----------------|
| Float cal pin (pull-up only) | `[CAL_SENSE] pin=6 raw=1 edges=0 …`; `[GAP_TIMEOUT] edges=0 rejected=0` |
| Tie cal pin to GND | `[CAL_SENSE] pin=6 raw=0 edges=0 …`; same zero-edge timeout |
| Toggle GND↔3.3 by hand | `[CAL_SENSE] edges` increases; gap may still TIMEOUT (too slow / wrong period) |
| ~16 Hz 0–3.3 V square into GP6 | `[CAL_SENSE] edges` high; `[MANUAL_GAP]` with real `gapUs` / `dutyErr` |

**Read the counters:** `edges=0` → pin stuck / no digital swing (HW or level). `edges>0` and `rejected` high / `accepted=0` → period gate (wrong frequency vs `expectHz`). Scope-confirmed swing but `edges=0` → wrong pad or MCU not seeing the net. If GP6 works and GP10 never did → pad/routing on 10.

---

## Calibration Entry Point: `DCO_calibration()`

Main (blocking, one-shot) auto-cal routine, called from `loop1()` when `calibrationFlag && !manualCalibrationFlag`:

1. **Global shutdown / reset**: `disable_all_oscillators_and_range_pwm()` (parks RANGE GPIOs and shared PW at max wrap).
2. **Shared PW calibration (once, voice 0)**: `find_PW_center(0)`, `find_PW_limit_v2(PW_LIMIT_LOW)`, `find_PW_limit_v2(PW_LIMIT_HIGH)`, restore `PW_CENTER[0]`.
3. **Per-oscillator amp-comp loop** (`currentDCO` = 0 .. `NUM_OSCILLATORS − 1`):
   - `restart_DCO_calibration()` — resets the note schedule, writes the `calibrationData` header (lowest-freq anchor + manual starting point), re-arms the RANGE pin/PIO for this DCO.
   - Set `ampCompCalibrationVal` from `initManualAmpCompCalibrationVal + manualCalibrationOffset` and apply to RANGE PWM.
   - Run `calibrate_DCO(ctx, 0.001)`; print `calibrationData`; `update_FS_voice(currentDCO)`.
4. **Finalization**: `calibrationFlag = false`; `init_FS()` reload; `precompute_amp_comp_for_engine()`.

---

## Amp-Comp Search: `calibrate_DCO(ctx, dutyErrorFraction)`

Builds the `[frequency, PWM]` table for one DCO. For each calibration note (start `DCO_calibration_start_note`, step `calibration_note_interval` semitones):

1. **Initial PWM guess** (`compute_initial_amp_for_note`):
   - First note: manual preset × 1.35.
   - Second note: logarithmic interpolation from the two header entries.
   - Later notes: quadratic interpolation from the previous three table points.
2. **Top-of-range check**: if the guess exceeds `DIV_COUNTER * 0.98`, run `find_highest_freq()` (bisection at full PWM), store that endpoint, anchor entry 0 with `find_lowest_freq()` (extrapolation to PWM 0), fill the remaining entries with sentinels, and stop.
3. **Tolerance**: `compute_gap_tolerance_for_freq(f, dutyErrorFraction)` = `2 · ε · T` µs — tighter at higher frequencies.
4. **Search loop** (with guards — see below):
   - `measure_gap_for_amp(pwm)` → signed duty error (positive = amplitude too low).
   - Track the measurement closest to zero (`bestAmpComp`).
   - On a **sign change**, probe ±1 and ±2 neighbours (`update_best_from_neighbours`), then stop if within tolerance; otherwise relax the tolerance (×1.2 / ×1.5) and count the flip (max 3 flips with a near-tolerance error also stops).
   - Otherwise **step** the PWM ±1 (near target) or ±2 (far), clamped to the per-note window `[0.8 × guess, 1.3 × guess]`.
5. **Store** `[note_to_freq(note) × 100, bestAmpComp]` in the table.

### Guards (added in the cleanup)

- **Timeouts**: a `kGapTimeoutSentinel` measurement no longer participates in sign-change detection or error-proportional stepping (the old code fed the sentinel ±1.17 µs into the stepper via a `avgValue == 0;` no-op bug). Instead the PWM is nudged up one step (no signal usually means the amplitude is below the comparator threshold) and the measurement is retried. 20 consecutive timeouts abort the note, keeping the best candidate.
- **Iteration/time guard**: max 300 iterations or 30 s per note, logged as `[DCO_AMP_GUARD]`, keeping the best candidate found.
- **Bounds**: the PWM is clamped to `[minAmpComp, maxAmpComp]` (stepping is done in int32, so no uint16 wraparound). If the search is stuck at a bound with the error still pushing outward, the note is finished with the best candidate.

### `find_highest_freq()`

Runs when the table reaches the top of the PWM range. At full RANGE PWM, it bisects a frequency window (one `calibration_note_interval` below/above the current note, driven through `calibrationFreqHz` → `voice_task_autotune(4, …)`) until the duty error is ≤ 0.5 µs or 24 iterations elapse. A measurement timeout is treated as “frequency too high” (amplitude collapsed). Returns the best frequency × 100. This replaces the old PID_v1-based loop (same acceptance threshold, bounded, and immune to the old `sizeof(sNotePitches)` out-of-bounds note lookup).

### `find_lowest_freq()`

Estimates the frequency reachable at RANGE PWM 0 by fitting a quadratic through the first three `[PWM → freq]` calibration points (linear fallback for degenerate cases). Purely computational — no live search.

---

## PW Center and Limits

All PW routines share one probe helper: `set_pw_and_measure(voiceIdx, pw)` — program the PW PWM channel, sync `PW[]` and the debug tracker, settle 30 ms, `measure_gap(2)`.

### `find_PW_center(mode)`

Finds the PW value that yields ≈50% duty at `manual_DCO_calibration_start_note` (mode 0, the live path). Starting PW is the stored `PW_CENTER[0]` (or mid-range on `firstTuneFlag`). Runs `find_PW_for_target_duty(kPWCenterDutyFraction, tolerance, 0, DIV_COUNTER_PW, startPW)` and persists the result via `update_FS_PWCenter`, keeping the previous center if the search fails.

### `find_PW_for_target_duty(...)` — phased search

Orchestrates four small phases over a shared `PWSearchState` (valid-sample table, best candidate, bracket):

1. **`pw_coarse_scan`** — steps through `[pwMin, pwMax]` (span/16 for center, span/32 for limit-style targets), recording valid samples (replace-worst when the 40-entry table is full) until a **sign-change bracket** around the target is found; then probes the linearly interpolated crossing point and stops.
2. **`pw_bisect_bracket`** — up to 14 bisection iterations inside the bracket (midpoint samples refine the best candidate but don't enter the table — same as the original code).
   **`pw_fine_scan_around_best`** — fallback when no bracket was found: fine scan around the best coarse sample.
3. **`pw_select_and_lock`** — candidates are tried best-first; each must pass **`pw_lock_in`** (3 consecutive in-band readings, 8 tries max), then is refined locally (PW±2, each with its own mini lock-in). A candidate that fails lock-in is deprioritized and the next one is tried. Aborts (keeping the caller's fallback PW) when the best gap is >10× the tolerance or all candidates fail.

Every phase respects a 60 s timeout from `DCOCalibrationStart`. Duty is computed as `duty = 0.5 + gap/(2T)` everywhere (the cleanup removed the places that disagreed on the sign; the ideal-gap formula is `gapTarget = T·(2p − 1)`).

### `find_PW_limit_v2(dir)` / `search_PW_limit_from_center(...)`

Finds the PW at which duty reaches `kPWLowDutyFraction` / `kPWHighDutyFraction` (≈2% / ≈98%):

- Coarse walk from the center toward the requested side (step = span/64), tracking the duty closest to target; early-out within `kPWLimitDutyTolerance`.
- Fine ±1 refinement around the best candidate (bounded radius 4–32; stops after 4 consecutive timeouts when walking deeper into the dead zone).
- If the target duty is unreachable, the hardware boundary PW is used as the limit.
- Results persist via `update_FS_PW_Low_Limit` / `update_FS_PW_High_Limit` and update `PW_LOW_LIMIT[]` / `PW_HIGH_LIMIT[]`.

---

## How Calibration Data is Used at Runtime

After a full `DCO_calibration()` pass:

- For each DCO: `update_FS_voice(currentDCO)` writes `calibrationData` into LittleFS.
- Once all oscillators are processed: `init_FS()` reloads the tables and `precompute_amp_comp_for_engine()` builds the runtime **amp-comp lookup tables** (see `amp_comp.h`).

At runtime, the engine interpolates the DCO’s table for any requested frequency and writes the resulting value to the range PWM.

### Fake / development tables

`setup1()` calls `seed_fake_calibration_tables(false)` **before** `init_FS()`. That plants fake amp-comp + PW tables only if the LittleFS `voiceTables` file is **missing**. An existing file (even if all zeros) is left alone; then `init_FS()` loads and `setup1` precomputes as usual.

To **force-overwrite** at any time, send `PARAM_DEBUG_COMMAND` **30** (dco_control → Calibration tab → **Seed fake calibration tables**), which calls `seed_fake_calibration_tables(true)`:

- Synthesizes per-oscillator 22-pair amp-comp curves (`generate_fake_calibration_data`) using the real note schedule and a scaled historical curve shape
- Writes full LittleFS banks (`voiceTables` + PW files) in one shot, plus sane PW defaults (`PW_CENTER≈570`, low `0`, high `DIV_COUNTER_PW`)
- Reloads with `init_FS()` and rebuilds runtime tables with `precompute_amp_comp_for_engine()`

These are **development placeholders**, not a substitute for a real hardware `DCO_calibration()` pass.

---

## Cleanup changelog (2026 pass)

Structural (no intended behaviour change):

- Deleted legacy/dead code: `init_DCO_calibration()` (unreachable), `init_PID()`, the PID_v1 globals (`PIDSetpoint/Input/Output`, `myPID`, `PIDMinGap*`, `PIDOutputLowerLimit/HigherLimit`, `PIDLimitsFormula`, `sampleTime`, `arrayPos`), the write-only `invalidPW[]` bookkeeping, commented-out interpolation stubs, and the write-only `highestNoteOSC[]`.
- `PID.ino` → `autotune_search.ino`; `PID.h` retired (remaining declarations moved into `autotune.h`); `#include <PID_v1.h>` dropped.
- `find_gap()` measurement state made local (no more cross-call globals); `samplesNumber` folded into the function.
- Extracted `set_pw_and_measure`, `pw_lock_in`, `note_to_freq`; split `find_PW_for_target_duty` into coarse-scan / bisect / fine-scan / select+lock phases with an explicit `PWSearchState`.
- `voice_task_autotune`: dead mode 1 removed; mode 4 now reads `calibrationFreqHz` instead of `PIDOutput`.

Bug fixes (**verify on hardware** — these are the only intended behaviour changes):

1. Timeout sentinel handling in `calibrate_DCO()` (`avgValue == 0;` no-op) — sentinel no longer feeds the stepper/sign detection; explicit nudge-up + retry with a consecutive-timeout cap.
2. Iteration/time guards on `calibrate_DCO()`'s inner loop and `find_highest_freq()` (formerly unbounded `while` loops).
3. `minAmpComp`/`maxAmpComp` enforced by clamping (was log-only, with shadowed duplicate declarations; uint16 wraparound below 0 also fixed).
4. `sizeof(sNotePitches)` element-count bug in the highest-note lookup (read past the array).
5. Duty sign convention unified to `duty = 0.5 + gap/(2T)`; `gapTarget = T·(2p − 1)` (previous code disagreed between coarse scan, bisection and result logging — only log output was affected at the live 50% target).
6. `find_PW_center()` wrote its starting PW to the **RANGE** PWM (`write_range_pwm(currentDCO, PW[0])`); it now programs the PW PWM channel as intended.
7. `voice_task_autotune` case 3 was missing a `break` and fell through into case 4 (mode 3 is not used by the live path).

Deferred to the improvement phase: better amp search strategy (bisection like the PW path), adaptive settle times, more calibration points, timeout-recovery strategy, runtime table quality checks, feeding bisection midpoints into the PW candidate table.

**Hardware verification:** run a full `DCO_calibration()` and compare the dumped `calibrationData` tables against a known-good run (e.g. `dco_calibration_DCO3_first prototype_2.json`).
