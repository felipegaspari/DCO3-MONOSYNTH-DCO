## Autotune Refactor – Current Architecture

This document describes how the autotune / DCO calibration code is **organized** after the refactor and the 2026 cleanup pass. It complements [`AUTOTUNE.md`](AUTOTUNE.md), which explains the algorithms, the measurement core, and lists the cleanup changelog / bug fixes.

Active files: `autotune.h` (includes `autotune_constants.h`, `autotune_context.h`, `autotune_measurement.h`), `autotune.ino`, `autotune_search.ino`, plus consumers `amp_comp.h` and `FS.*`. After tables are written, `precompute_amp_comp_for_engine()` prepares float or fixed lookup data per [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).

The old `PID.h` / `PID.ino` no longer exist: the live calibration path never used the PID controller, so the cleanup removed `PID_v1` entirely and renamed the file to match what it actually contains (the amp-comp search).

---

## High-Level Overview

### Goals (unchanged)

- Build, for each DCO, a calibration table:
  - **Input**: note frequency.
  - **Output**: range PWM value (`ampCompCalibrationVal`) that yields the desired duty behaviour (≈50%) / amplitude.
- Use these tables at runtime for fast amplitude compensation via interpolation.

### Structure at a glance

| File | Role |
|------|------|
| `autotune.h` | Module globals, `note_to_freq()`, PW-search types, prototypes |
| `autotune_constants.h` | Named `constexpr` constants (no magic numbers) |
| `autotune_context.h` | `DCOCalibrationContext` view over the per-DCO globals |
| `autotune_measurement.h` | `GapMeasurement` + `measure_gap()` wrapper over `find_gap()` |
| `autotune.ino` | Orchestration, PW center/limit searches, `find_gap()` core, manual-cal debug |
| `autotune_search.ino` | `calibrate_DCO()` amp search, `find_highest_freq`/`find_lowest_freq`, interpolation helpers |

The **calibration algorithm** itself is intentionally preserved (aside from the explicit bug fixes listed in `AUTOTUNE.md`); the refactor focuses on readability, separation of concerns, and making state dependencies explicit.

---

## Files and Their Roles

### `autotune.h`

Defines the module's **global state** and shared types:

- **Flags**: `calibrationFlag`, `manualCalibrationFlag`, `firstTuneFlag`.
- **Manual calibration state**: `manualCalibrationStage`, `manualCalibrationOffset[NUM_OSCILLATORS]`.
- **DCO calibration state**:
  - `calibrationData[chanLevelVoiceDataSize]` – temporary buffer for `[freq, pwm]` pairs.
  - `currentDCO`, `DCO_calibration_current_note`.
  - `DCOCalibrationStart` – millis at pass start (feeds the PW searches' 60 s timeouts).
  - `ampCompCalibrationVal`, `initManualAmpCompCalibrationVal*`, `ampCompLowestFreqVal`.
  - `calibrationFreqHz` – frequency override for `voice_task_autotune()` mode 4 (highest-freq search).
  - Note schedule constants: `DCO_calibration_start_note`, `calibration_note_interval`, `manual_DCO_calibration_start_note`.
- **Helpers**: `note_to_freq(midiNote)` (single place for the `sNotePitches[note - 12]` indexing).
- **PW search types**: `PWSearchState`, `PWRecordMode`, `kPWMaxSamples`, `PWLimitDir`, `PWLimitSearchResult`.
- **Prototypes** for the cross-file functions (`calibrate_DCO`, `compute_gap_tolerance_for_freq`, `find_highest_freq`, `find_lowest_freq`, `search_PW_limit_from_center`, `find_PW_limit_v2`).

> **Why are `PWSearchState` / `PWRecordMode` in the header?** The Arduino builder auto-generates prototypes for `.ino` functions and inserts them near the top of the combined translation unit. Functions taking these types as parameters therefore need the types defined in a header, before the generated prototypes.

Everything that used to be a cross-call global for the edge measurement (`pulseCounter`, `samplesCounter`, `risingEdgeTimeSum`/`fallingEdgeTimeSum`, `edgeDetectionLastTime`, `edgeDetectionLastVal`, `microsNow`, `samplesNumber`, `DCO_calibration_difference`) is now **local to `find_gap()`**.

### `autotune_constants.h`

Defines **common constants** to avoid magic numbers:

- `kGapTimeoutSentinel` – special float value returned when gap measurements time out.
- `kGapTimeoutUs` – max time without an edge before timeout.
- `kEdgeDebounceMinUs` – min time between edges to consider them valid.
- `kGapPolarityInverted` – compensate inverted hardware polarity on the cal-sense input.
- PW duty targets/tolerances: `kPWCenterDutyFraction`, `kPWLowDutyFraction`, `kPWHighDutyFraction`, `kPWLimitDutyTolerance`.
- `kManualGapTimeoutDutyErrTimes100` – sentinel duty error for the manual-cal UI on timeout.

### `autotune_measurement.h`

Wraps the low-level measurement function and exposes a structured interface:

- Forward declaration: `float find_gap(byte specialMode);`
- `struct GapMeasurement { bool timedOut; float value; };`
- `measure_gap(byte specialMode)`:
  - Calls `find_gap`, sets `timedOut = (value == kGapTimeoutSentinel)`, returns `{timedOut, value}`.

Used by all search code (PW searches via `set_pw_and_measure`, amp search via `measure_gap_for_amp`, the frequency estimators, and `DCO_calibration_debug`).

### `autotune_context.h`

Introduces a **context struct** for DCO calibration:

- `struct DCOCalibrationContext`:
  - `uint8_t& dcoIndex;` – reference to `currentDCO`.
  - `uint8_t& currentNote;` – reference to `DCO_calibration_current_note`.
  - `uint32_t* calibrationData;` – pointer to `calibrationData[]`.
  - `int8_t* manualOffsetByOsc;` – pointer to `manualCalibrationOffset[]`.
  - `int8_t* initManualAmpByOsc;` – pointer to `initManualAmpCompCalibrationVal[]`.

This is a **view** over existing globals; it does not change storage, but lets `calibrate_DCO` take a clear parameter representing the current calibration target.

### `autotune.ino`

Orchestration, PW calibration, and the measurement core:

- **Hardware helpers**: `disable_all_oscillators_and_range_pwm()`, `reset_pw_to_DIV_COUNTER_PW()`.
- **`DCO_calibration()`** – main entry point (PW pass once on voice 0, then `calibrate_DCO` + FS persist per oscillator; reload + precompute at the end).
- **`restart_DCO_calibration()`** – the single reset routine (the legacy `init_DCO_calibration()` twin was unreachable and has been deleted): resets the note schedule, writes the `calibrationData[0..3]` header, re-arms the RANGE pin/PIO for `currentDCO`.
- **PW search stack** (bottom-up):
  - `set_pw_and_measure(voiceIdx, pw)` – the one "program PW → settle → measure" helper (replaces six copy-pasted blocks).
  - `pw_search_state_init` / `pw_record_sample` – `PWSearchState` bookkeeping (best candidate, in-tolerance count, valid-sample table with append/replace-worst modes).
  - `pw_coarse_scan` → `pw_bisect_bracket` (bracket found) or `pw_fine_scan_around_best` (no bracket) → `pw_select_and_lock` (uses `pw_lock_in` for the 3-consecutive-readings requirement and the PW±2 local refinement).
  - `find_PW_for_target_duty(targetDuty, targetGap, pwMin, pwMax, fallbackPW)` – thin orchestrator over the phases; returns `fallbackPW` when the search fails.
- **`find_PW_center(mode)`** – sets up note/amp, picks the starting PW, runs the target-duty search at 50%, persists the result.
- **`search_PW_limit_from_center` / `find_PW_limit_v2`** – duty-limit searches (≈2% / ≈98%), persisted to FS.
- **`find_gap(specialMode)`** – edge-timing measurement core, fully local state.
- **`cal_sense_probe_log()` / `DCO_calibration_debug()`** – manual-cal diagnostics (see `AUTOTUNE.md`).

### `autotune_search.ino`

The **search-based amp-comp calibration** (formerly `PID.ino`, without the PID):

- Small, single-purpose helpers:
  - `compute_gap_tolerance_for_freq(freqHz, dutyErrorFraction)` – `|gap|max = 2εT` (non-static: also used by `find_PW_center`).
  - `did_sign_change(previous, current)`.
  - `measure_gap_for_amp(ampPwm)` – apply amp PWM, settle, `measure_gap(0)`, normalize sign (positive = amplitude too low); preserves the timeout sentinel exactly.
  - `update_best_from_neighbours(...)` – neighbour probing bookkeeping.
  - `step_amp_from_error(avgValue, tolerance)` – returns the signed step (±1 near target, ±2 far); the caller clamps to the per-note bounds in int32 (no uint16 wraparound).
  - `compute_initial_amp_for_note(ctx, j)` – manual preset ×1.35 / log interp / quadratic interp.
  - `store_note_result(ctx, j, bestAmpComp, closestToZero)`.
- **`calibrate_DCO(ctx, dutyErrorFraction)`** – the per-note search loop, now with explicit guards (iteration/time cap, consecutive-timeout cap, bound clamping with break-with-best). See `AUTOTUNE.md` for the algorithm walk-through.
- **`find_highest_freq()`** – bisection over frequency at full RANGE PWM, driven through `calibrationFreqHz` → `voice_task_autotune(4, …)`. Replaces the old PID_v1 loop with the same acceptance threshold (0.5 µs).
- **`find_lowest_freq()`** – quadratic extrapolation of the first table points to PWM 0.
- **Interpolation helpers**: `quadraticInterpolation`, `logarithmicInterpolation`, `linearInterpolation`, `expInterpolationSolveY` (the last one is used by `initMultiplierTables()`, not by calibration). The unused float/double variants were deleted.

---

## Behaviour vs Original Version

- The numerical algorithm is preserved: same initial guesses, step sizes, tolerance computation and relaxation strategy, same stopping conditions, same PW search sample bookkeeping (including the quirk that bisection midpoints refine the best candidate but don't enter the candidate table).
- The **only intended behaviour changes** are the bug fixes listed in the "Cleanup changelog" section of [`AUTOTUNE.md`](AUTOTUNE.md) — verify them on hardware by comparing a fresh `DCO_calibration()` dump against a known-good table.

## Suggested Future Work (improvement phase)

- Better amp search strategy (bisection over PWM like the PW path already has).
- Adaptive settle times; more calibration points; a smarter timeout-recovery strategy.
- Feed bisection midpoints into the PW candidate table so `pw_select_and_lock` can pick them.
- Runtime table quality checks after calibration.
- A minimal logging abstraction (instead of raw `Serial.println`) to control verbosity centrally.
