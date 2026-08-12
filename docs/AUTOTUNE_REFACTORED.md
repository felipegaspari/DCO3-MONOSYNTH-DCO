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
| `autotune_constants.h` | Named `constexpr` constants (no magic numbers) + the NORMAL/FINE precision profiles |
| `autotune_context.h` | `DCOCalibrationContext` view over the per-DCO globals |
| `autotune_measurement.h` | `GapMeasurement` + `measure_gap()` wrapper over `find_gap()` |
| `autotune.ino` | Orchestration (incl. amp-method dispatch), PW center/limit searches, `find_gap()` core, manual-cal debug |
| `autotune_search.ino` | `calibrate_DCO()` classic amp search, `calibrate_DCO_freq_trace()` curve tracing, `refine_DCO_amp_table()` fine pass, the frequency search (`measure_duty_at_freq`/`find_freq_for_duty50`), `find_highest_freq`/`find_lowest_freq`, interpolation helpers |

The **classic calibration algorithm** is intentionally preserved (aside from the explicit bug fixes listed in `AUTOTUNE.md`); the refactor focuses on readability, separation of concerns, and making state dependencies explicit. The improvement phase then added the `FREQ_TRACE` method alongside it (A/B selectable, classic remains default).

---

## Files and Their Roles

### `autotune.h`

Defines the module's **global state** and shared types:

- **Flags**: `calibrationFlag`, `manualCalibrationFlag`, `firstTuneFlag`.
- **Run selection**: `CalibrationScope` / `calibrationScope` (which stages run) and `CalPrecision` / `calibrationPrecision` with `cal_precision()` and `calibration_precision_name()` (how carefully they measure). Both come from the value of `PARAM_CALIBRATION_FLAG`: 1/2/3 normal, 5/6/7 fine.
- **Manual calibration state**: `manualCalibrationStage`, `manualCalibrationOffset[NUM_OSCILLATORS]`, `manualCalibrationStep` (0 = trimpot stage at note 24, 1 = 440 Hz amp-set stage), `ampComp440[NUM_OSCILLATORS]` (440 Hz manual anchor; 0 = never set, persisted as LittleFS `AmpComp440`), `ampCompDutyOffset[NUM_OSCILLATORS]` (duty target trim in hundredths of a percent, persisted as `AmpCompDutyOffset`) with the `duty_trim_gap_us()` helper that converts it into a target gap at a given frequency.
- **Calibration report state**: `CalPointSource`, `calPointDutyErrPct[]` / `calPointSource[]` (one entry per table pair), `calReportLadderInterval` / `calReportAnchorPair`, `duty_err_pct_from_gap()`, and the `cal_report_*` / `print_calibration_report()` prototypes. `calibrationVerifyRequested` is the core-0 → core-1 request flag for the `[CAL_VERIFY]` sweep.
- **DCO calibration state**:
  - `calibrationData[chanLevelVoiceDataSize]` – temporary buffer for `[freq, pwm]` pairs.
  - `currentDCO`, `DCO_calibration_current_note`.
  - `DCOCalibrationStart` – millis at pass start (feeds the PW searches' 60 s timeouts).
  - `ampCompCalibrationVal`, `initManualAmpCompCalibrationVal*`, `ampCompLowestFreqVal`.
  - `calibrationFreqHz` – frequency override for `voice_task_autotune()` mode 4 (frequency probes).
  - `gapGateFreqHz` – when > 0, `find_gap()` gates edge intervals against this probe frequency instead of the current note.
  - `g_lastDrivenFreqHz` – frequency the oscillator is currently running at, so the next probe knows how far it has to move (settle budget). 0 = nothing running.
  - Note schedule constants: `DCO_calibration_start_note`, `calibration_note_interval`, `manual_DCO_calibration_start_note` (PW pass), `manual_cal_reference_note` (69 → 440 Hz, manual trim + `FREQ_TRACE` anchor).
- **Helpers**: `note_to_freq(midiNote)` (single place for the `sNotePitches[note - 12]` indexing); `settle_for_freq(freqHz)` (period-proportional settle: 2 periods, floored at 4 ms).
- **PW search types**: `PWSearchState`, `PWRecordMode`, `kPWMaxSamples`, `PWLimitDir`, `PWLimitSearchResult`.
- **Prototypes** for the cross-file functions (`calibrate_DCO`, `calibrate_DCO_freq_trace`, `refine_DCO_amp_table`, `measure_duty_at_freq`, `find_freq_for_duty50`, `compute_gap_tolerance_for_freq`, `find_highest_freq`, `find_lowest_freq`, `search_PW_limit_from_center`, `find_PW_limit_v2`).
- **Not here**: the method selector (`AutotuneAmpMethod` / `autotuneAmpMethod`, `PARAM_DEBUG_COMMAND` 34/35, boot value from the `AUTOTUNE_AMP_METHOD_DEFAULT` build flag) lives in [`globals.h`](../globals.h) beside `note_retrig_mode`, because [`bench.h`](../bench.h) is included before `autotune.h` and reports it as `amp_cal=` on the profiler `engine:` line.

> **Why are `PWSearchState` / `PWRecordMode` in the header?** The Arduino builder auto-generates prototypes for `.ino` functions and inserts them near the top of the combined translation unit. Functions taking these types as parameters therefore need the types defined in a header, before the generated prototypes.

Everything that used to be a cross-call global for the edge measurement (`pulseCounter`, `samplesCounter`, `risingEdgeTimeSum`/`fallingEdgeTimeSum`, `edgeDetectionLastTime`, `edgeDetectionLastVal`, `microsNow`, `samplesNumber`, `DCO_calibration_difference`) is now **local to `find_gap()`**.

### `autotune_constants.h`

Defines **common constants** to avoid magic numbers:

- `kGapTimeoutSentinel` – special float value returned when gap measurements time out.
- `kGapTimeoutUs` – max time without an edge before timeout.
- `kEdgeDebounceMinUs` – min time between edges to consider them valid.
- `kGapPolarityInverted` – compensate inverted hardware polarity on the cal-sense input.
- `kGapSamplesDefault` / `kGapSamplesHiRes` – segment counts for `find_gap()` mode 0 and the profiles' floor.
- `CalPrecisionProfile` + `kCalPrecisionNormal` / `kCalPrecisionFine` – every speed-vs-quality knob in one place: hi-res segment floor/ceiling and averaging window, settle after a frequency change, bisection acceptance and budget, post-bisection re-measurement, anchor and rung retries, stability-check budget and tolerance multiplier. Selected by `calibrationPrecision` and read through `cal_precision()`.
- `kSettleSkipCents` – frequency move below which a probe needs no stability check.
- `kSettleSkipCents` (5) / `kSettleBigMoveCents` (100) – how far a probe moved decides how much settling it may pay for: nothing below the first, one confirming reading below the second, the profile's full budget above it.
- `kSearchStepCentsHigh` / `Mid` / `Low` (400 / 200 / 100 cents, split at `kSearchStepHighHz` 440 and `kSearchStepLowHz` 100) – largest step one probe of the frequency search may take.
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
- **`DCO_calibration()`** – main entry point (PW pass once on voice 0, then the amp-comp stage per oscillator — `refine_DCO_amp_table` at FINE precision, otherwise `calibrate_DCO` or `calibrate_DCO_freq_trace` per `autotuneAmpMethod` — + the raw table dump, the `[CAL_REPORT]` table and the FS persist; reload + precompute at the end). A table that fails its monotonicity check is not persisted.
- **Calibration report**: `cal_report_reset()` / `cal_report_set_pair()` / `cal_report_set_pair_from_gap()` record what each pair is and how well it landed while the table is being built; `print_calibration_report(dco, data)` prints it (per-pair duty error, the one-count floor `50/amp`, endpoints, span, avg/worst).
- **`run_calibration_verify_sweep()`** – read-only `[CAL_VERIFY]` pass over the finished tables: amp from the runtime lookup, duty measured every 3 semitones per oscillator. Debug cmd 36 raises `calibrationVerifyRequested` on core 0 and `loop1()` runs it on core 1, because every probe blocks on a duty measurement.
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
  - `measure_gap_for_amp(ampPwm)` – apply amp PWM, `settle_for_freq(current note)`, `measure_gap(0)`, normalize sign (positive = amplitude too low) against the trimmed duty target (`duty_trim_gap_us`); preserves the timeout sentinel exactly.
  - `update_best_from_neighbours(...)` – neighbour probing bookkeeping.
  - `step_amp_from_error(avgValue, tolerance)` – returns the signed step (±1 near target, ±2 far); the caller clamps to the per-note bounds in int32 (no uint16 wraparound).
  - `compute_initial_amp_for_note(ctx, j)` – manual preset ×1.35 / log interp / quadratic interp.
  - `store_note_result(ctx, j, bestAmpComp, closestToZero)`.
  - `calibration_interval_ratio()` – 2^(interval/12); `extrapolate_amp_for_freq(...)` – generic 1/2/3-point y(x) extrapolation; `freq_trace_guess(...)` – 3-point guess used by the curve tracer in both directions (amp-for-freq and freq-for-amp), picking points that bracket the target and are at least `kGuessMinSpread` (10%) apart; `freq_trace_local_slope(...)` – local d(log f)/d(log a) for the rung retry; `freq_trace_quality(...)` – the shared `gapUs= dutyErr= probes= settle=` log tail.
- **`calibrate_DCO(ctx, dutyErrorFraction)`** – the classic per-note search loop, with explicit guards (iteration/time cap, consecutive-timeout cap, bound clamping with break-with-best). See `AUTOTUNE.md` for the algorithm walk-through.
- **`drive_freq(freqHz, amp)`** / **`freq_move_cents(from, to)`** / **`wait_periods(f, periods, minUs)`** – write a probe frequency in one go (no glide: a walk would keep moving the frequency before whole periods have come out of it) and track where the oscillator is in `g_lastDrivenFreqHz` (cleared by `restart_DCO_calibration()` and `disable_all_oscillators_and_range_pwm()`).
- **`measure_duty_at_freq(freqHz, amp, hiRes = false)`** – arbitrary-frequency duty probe: set the frequency → `wait_periods(settlePeriods, settleMinMs)` → `measure_gap(hiRes ? 3 : 0)` repeated until two readings agree within `settleStableMult` x the acceptance (averaged), with the check budget sized by how far the frequency moved and one retry before a post-jump timeout is believed. Classic sign convention, aiming at the trimmed duty target. Modes 2/3 average the profile's window instead of the fixed `kGapSamplesDefault` (6). Every reading bumps `g_lastFreqBisectProbes`, the extra ones `g_lastSettleChecks`.
- **`find_freq_for_duty50(amp, freqGuess, windowRatio, refine = false, bounds = nullptr)`** / **`search_step_cap_cents(f)`** – generalized frequency search at fixed PWM (24 probes). The seed is measured first, then the search steps outward by at most 400/200/100 cents (by range) until the answer is bracketed and interpolates from there (Illinois secant in log-frequency, geometric midpoint as the fallback); `windowRatio` is the expected travel and `(bisectWindows + 1) x` it the allowance before giving up. A timeout is placed from evidence where there is any and read as "frequency too high" otherwise, and `kMaxSearchTimeouts` of them in a row end an unbounded search; `bounds` confines it to a band instead. `refine` (every `FREQ_TRACE` call, the fine pass and both endpoints) switches the probes to hi-res and takes its budget, acceptance and post-search re-measurement from `cal_precision()`, so a single noisy probe cannot set the stored pair; the achieved signed error is kept in `g_lastFreqBisectGapUs` and the probe count in `g_lastFreqBisectProbes`.
- **`calibrate_DCO_freq_trace(ctx)`** – the `FREQ_TRACE` table builder (anchor probe = stored `ampComp440[dco]` bisected around 440 Hz — aborts with `[FREQ_TRACE_GUARD]` when unset; then the manual trim note as a second model point, the anchor re-measured/corrected/persisted against a real 440 Hz, a bootstrap cluster of 4 probes around it, a ladder interval/anchor rung derived from that model, up/down tracing with 3-point guesses and one slope-corrected retry per off-target rung, the full-amp and amp-comp-0 endpoints measured last from a tight model seed, sentinel fill, monotonicity check). See the improvement-phase section in `AUTOTUNE.md`.
- **`refine_DCO_amp_table(ctx)`** – the fine pass: validate the stored table, then keep every stored amp comp and re-measure only the frequency it sits at, in a ±34-cent window (`kRefineWindowRatio` 1.02). No anchor, no bootstrap, no ladder, nothing extrapolated; `[CAL_REFINE]` per pair plus a summary, and the same monotonicity check. Ignores `autotuneAmpMethod`.
- **`cal_table_is_monotonic(data, pairs, dco, tag)`** – shared ascending-columns check used by both table builders.
- **`find_highest_freq()`** – thin wrapper over `find_freq_for_duty50` at full RANGE PWM (legacy note-interval window, 0.5 µs floor).
- **`find_lowest_freq()`** – quadratic extrapolation of the first table points to amp comp 0; seed/fallback for the measured anchor.
- **`amp0_search_band()` / `amp0_prescan()` / `measure_lowest_freq_at_amp0(seedHz, bounds)` / `apply_measured_lowest_freq(ctx)`** – measure the table's bottom anchor instead of extrapolating it: a band under the first measured pair (`kAmp0BandRatio`, floored at `kAmp0MinFreqHz`), a scan across it for two readings bracketing 50% duty, then a bounded search with the amp fixed at 0, accepted only within `kEndpointAcceptDutyPct`. `apply_measured_lowest_freq()` writes `[freq × 100, 0]` into `calibrationData[0..1]` for the classic method; `FREQ_TRACE` and the fine pass call the measurement for their own pair 0.
- **Interpolation helpers**: `quadraticInterpolation`, `logarithmicInterpolation`, `linearInterpolation`, `expInterpolationSolveY` (the last one is used by `initMultiplierTables()`, not by calibration). The unused float/double variants were deleted.

---

## Behaviour vs Original Version

- The numerical algorithm is preserved: same initial guesses, step sizes, tolerance computation and relaxation strategy, same stopping conditions, same PW search sample bookkeeping (including the quirk that bisection midpoints refine the best candidate but don't enter the candidate table).
- The **only intended behaviour changes** are the bug fixes listed in the "Cleanup changelog" section of [`AUTOTUNE.md`](AUTOTUNE.md) — verify them on hardware by comparing a fresh `DCO_calibration()` dump against a known-good table.

## Suggested Future Work

Done in the improvement phase: frequency-bisection amp search (`FREQ_TRACE`), adaptive settle times (`settle_for_freq`), the two-step manual calibration with the stored 440 Hz anchor (`manualCalibrationStep` / `ampComp440`). Still open:

- Remove the classic method once `FREQ_TRACE` is validated on hardware (run both on the same board, compare dumped tables and runtime).
- Move PW calibration to a higher note (pending a hardware check that the PW center is frequency-independent).
- More calibration points; feed bisection midpoints into the PW candidate table so `pw_select_and_lock` can pick them.
- Runtime table quality checks after calibration.
- A minimal logging abstraction (instead of raw `Serial.println`) to control verbosity centrally.
