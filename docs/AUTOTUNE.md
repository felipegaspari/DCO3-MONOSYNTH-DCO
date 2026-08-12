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
  - `calibrationCancelRequested` (volatile) — set on core 0 by `PARAM_CALIBRATION_FLAG` = 0 while auto-cal blocks core 1; every calibration loop polls it and unwinds, skipping the interrupted stage's FS persist (previous values kept). Cleared by `DCO_calibration()` on entry.
  - `calibrationScope` (`CalibrationScope`: `CAL_SCOPE_AMP` 1, `CAL_SCOPE_PW` 2, `CAL_SCOPE_FULL` 3) — which stage the next run performs, carried by the value of `PARAM_CALIBRATION_FLAG`. PW and amp-comp are independent; an amp-only run reuses the stored PW center.
  - `currentDCO`, `DCO_calibration_current_note`
  - `manualCalibrationStage`, `manualCalibrationOffset[NUM_OSCILLATORS]`
  - `manualCalibrationStep` (0 = trimpot stage at note 24, 1 = 440 Hz amp-set stage), `ampComp440[NUM_OSCILLATORS]` (440 Hz manual anchor, 0 = never set; persisted as `AmpComp440`)
- **Calibration table**:
  - `calibrationData[chanLevelVoiceDataSize]`
    - Flat array of `uint32_t` pairs: `[freq0, pwm0, freq1, pwm1, ...]`
    - `freq` values are `note_to_freq(note) * 100`
    - `pwm` values are the range PWM that produced the best duty at that frequency
    - Entries `[0..1]` are the "lowest frequency" anchor, `[2..3]` the manual starting point
- **Timers**: `DCOCalibrationStart` (millis at pass start; feeds the PW searches' 60 s timeouts)
- **Frequency probing**: `calibrationFreqHz` — frequency override consumed by `voice_task_autotune()` mode 4; `gapGateFreqHz` — when > 0, `find_gap()` gates edge intervals against this probe frequency instead of the current note
- **Method selection**: `autotuneAmpMethod` (in `globals.h` beside `note_retrig_mode`, so `bench.h` can report it) — boot value from the `AUTOTUNE_AMP_METHOD_DEFAULT` build flag (`0` CLASSIC shipping), switched at runtime with `PARAM_DEBUG_COMMAND` 34/35

The edge-timing state that used to live in header globals (`pulseCounter`, `samplesCounter`, `risingEdgeTimeSum`, `edgeDetectionLastTime`, `DCO_calibration_difference`, …) is now local to `find_gap()`.

---

## Measurement Core

### `find_gap(byte specialMode)`

The **core measurement primitive** used by every calibration routine (normally via the `measure_gap()` wrapper, which converts the timeout sentinel into a `GapMeasurement{value, timedOut}`).

- **Inputs**: `specialMode == 2` uses 12 accepted samples (PW searches), anything else 6.
- **Process**:
  - Computes the ideal period and derives a per-edge interval gate (≈1%–99% of the period) to reject glitches. The period comes from `DCO_calibration_current_note`, unless `gapGateFreqHz` is set (arbitrary-frequency probes via `measure_duty_at_freq()`), in which case the probe frequency is used.
  - Loops reading `digitalRead(DCO_calibration_pin)` (polarity-compensated via `kGapPolarityInverted`), timing debounced edges (`kEdgeDebounceMinUs`).
  - If no edge is accepted for `kGapTimeoutUs`, it logs `[GAP_TIMEOUT] note= freq= raw= edges= rejected= accepted= …` and returns `kGapTimeoutSentinel`.
  - After the first aligned pulses, each edge-to-edge interval that passes the gate is accumulated into the falling/rising sums.
  - At `autotuneDebug >= 2` every reading logs `[GAP_MEASURE] mode= note= freq= DCO= AMP= … duty_meas≈ …`. **`freq=` is the frequency actually being driven** — the probe frequency during a `FREQ_TRACE` search, the note's frequency otherwise. `note=` is only meaningful in the second case: an arbitrary-frequency probe leaves it at whatever note the calibration last set.
- **Output**: `avgHighUs − avgLowUs` in microseconds (0 for a symmetric 50% duty), following the relation `duty − 0.5 = diff / (2·T)`.
- **Sign chain**: the segment attribution and polarity handling are the field-validated legacy convention. `measure_gap_for_amp()` (amp search) flips the sign so that **positive = amplitude too low**; the PW searches use the raw value with `duty = 0.5 + gap/(2T)` throughout.

### Related helpers

- **`measure_gap(specialMode)`** (`autotune_measurement.h`): wraps `find_gap`, sets `timedOut` when the sentinel comes back.
- **`DCO_calibration_debug()`**: manual-cal path; wraps `measure_gap(0)`, prints `[MANUAL_GAP]` (`gapUs` / `dutyErr` or `TIMEOUT`), TXes `PARAM_GAP_FROM_DCO` (154). On timeout also runs `cal_sense_probe_log()` → `[CAL_SENSE]`.
- **`cal_sense_probe_log()`**: 40 ms raw `digitalRead` window (no period gate). Logs `raw`, `edges`, `minDt`/`maxDt`, pull-up/invert flags, note and `expectHz`. Throttled ~2 Hz.

### Cal-sense bench checks (manual cal)

Pin: `DCO_calibration_pin` = **GP6** (temporary A/B on Pico header; was **GP10**; GP25 aborted — Pico LED / not on header). Feed the signal into the pin shown in `[CAL_SENSE] pin=`. Setup uses `pinMode` on that GPIO (`INPUT` / `INPUT_PULLUP` as configured in `DCO.ino`). Manual cal runs at `manual_DCO_calibration_start_note` (24) in step 0 and `manual_cal_reference_note` (69, **A4 = 440 Hz**) in step 1; PW calibration uses note 24. RP2040/RP2350 at 3.3 V IO: **VIH ≥ 2.0 V**, **VIL ≤ 0.8 V** (2.48 V is a valid HIGH; detection still needs edges that go below VIL).

Enter manual cal (param 151). On timeout USB shows `[MANUAL_GAP] … TIMEOUT`, `[GAP_TIMEOUT] … raw= edges= rejected= accepted= …`, and throttled `[CAL_SENSE] pin=6 …`.

| Test | Expected Serial |
|------|-----------------|
| Float cal pin (pull-up only) | `[CAL_SENSE] pin=6 raw=1 edges=0 …`; `[GAP_TIMEOUT] edges=0 rejected=0` |
| Tie cal pin to GND | `[CAL_SENSE] pin=6 raw=0 edges=0 …`; same zero-edge timeout |
| Toggle GND↔3.3 by hand | `[CAL_SENSE] edges` increases; gap may still TIMEOUT (too slow / wrong period) |
| ~440 Hz 0–3.3 V square into GP6 | `[CAL_SENSE] edges` high; `[MANUAL_GAP]` with real `gapUs` / `dutyErr` |

**Read the counters:** `edges=0` → pin stuck / no digital swing (HW or level). `edges>0` and `rejected` high / `accepted=0` → period gate (wrong frequency vs `expectHz`). Scope-confirmed swing but `edges=0` → wrong pad or MCU not seeing the net. If GP6 works and GP10 never did → pad/routing on 10.

---

## Calibration Entry Point: `DCO_calibration()`

Main (blocking, one-shot) auto-cal routine, called from `loop1()` when `calibrationFlag && !manualCalibrationFlag`:

1. **Global shutdown / reset**: `disable_all_oscillators_and_range_pwm()` (parks RANGE GPIOs and shared PW at max wrap). The stage and the measurement profile are logged as `[DCO_CAL] scope: AMP|PW|FULL precision: NORMAL|FINE`.
2. **Shared PW calibration (once, voice 0)** — only when `calibrationScope` includes PW: `find_PW_center(0)`, `find_PW_limit_v2(PW_LIMIT_LOW)`, `find_PW_limit_v2(PW_LIMIT_HIGH)`. `PW_CENTER[0]` is applied either way, so an amp-only run uses the stored center.
3. **Per-oscillator amp-comp loop** — only when the scope includes amp-comp (`currentDCO` = 0 .. `NUM_OSCILLATORS − 1`):
   - `restart_DCO_calibration()` — resets the note schedule, writes the `calibrationData` header (lowest-freq anchor + manual starting point), re-arms the RANGE pin/PIO for this DCO.
   - Set `ampCompCalibrationVal` from `initManualAmpCompCalibrationVal + manualCalibrationOffset` and apply to RANGE PWM.
   - Run the amp-comp stage: at `CAL_PRECISION_FINE` always `refine_DCO_amp_table(ctx)` (re-measure the stored table); otherwise per `autotuneAmpMethod`, `calibrate_DCO(ctx, 0.001)` (classic, default) or `calibrate_DCO_freq_trace(ctx)` (curve tracing — see the improvement-phase section below).
   - `apply_measured_lowest_freq(ctx)` — classic normal runs only: replace the table's amp-comp-0 anchor with a measured point (see below); skipped for `FREQ_TRACE` (which measures its own bottom endpoint) and for the fine pass (which re-measures pair 0 directly), when the run was cancelled, or when the table was rejected.
   - Print `calibrationData`; `update_FS_voice(currentDCO)` (skipped if a `FREQ_TRACE` table fails its monotonicity check).
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

Runs when the table reaches the top of the PWM range. Now a thin wrapper over `find_freq_for_duty50(DIV_COUNTER, note_to_freq(current note), interval ratio)` — the generalized frequency search shared with the `FREQ_TRACE` method (probes via `measure_duty_at_freq()`, timeout = “frequency too high”, bounded outward steps until the answer is bracketed, then interpolation, 24 probes max). Returns the best frequency × 100, falling back to the window's low edge if no signal was seen. This replaces the old PID_v1-based loop (same acceptance threshold, bounded, and immune to the old `sizeof(sNotePitches)` out-of-bounds note lookup).

### `find_lowest_freq()`

Estimates the frequency reachable at amp comp 0 by fitting a quadratic through the first three `[amp comp → freq]` calibration points (linear fallback for degenerate cases). Purely computational — no live search. It is now only the seed/fallback for the measured anchor below.

### The amp-comp-0 endpoint: `amp0_search_band()` / `amp0_prescan()` / `measure_lowest_freq_at_amp0()`

Pair 0 is the one point of the curve the oscillator may simply not have, and the only one no model has an anchor under, so all three amp-comp paths — classic (`apply_measured_lowest_freq()`), `FREQ_TRACE`'s bottom endpoint and the fine pass's pair 0 — measure it through the same three steps.

**The band** (`amp0_search_band(firstPairHz)`) is where the point is allowed to be: from `firstPairHz × 0.99` down to `firstPairHz / kAmp0BandRatio` (2.5), floored at `kAmp0MinFreqHz` (5 Hz). Wide, because a measured table puts the point at roughly *pair 1 / 2.2* — a band of one ladder rung, which is what this used to be, looks for it about an octave above where it is, and then rejects everything it finds. The floor is where a reading stops meaning anything: `kGapTimeoutPeriods` periods of it exceed the `kGapTimeoutMaxUs` deadline, so a lopsided pulse there cannot be told from silence and each probe costs the full 400 ms. The band bounds the seed, the search and what may be stored, which also keeps the table monotonic.

**The scan** (`amp0_prescan()`) walks `kAmp0ScanPoints` (10) log-spaced frequencies down the band at amp comp 0, one quick reading each (`scan_duty_at_freq()`: no adaptive settle, but at least a whole period of wait — 20 ms at 8 Hz is a seventh of one, and what comes back then describes the previous frequency). What it is looking for is two readings of **opposite sign**: that pair brackets the answer, and a bracket is worth much more to the search than any seed, since it can only interpolate inward and no probe can wander into a region already known to be silent. The seed is then the secant crossing of the bracket in log-frequency — a point the scan has *not* already measured. Every point is probed: at amp comp 0 the pulse can be lost at either end (above, the amplitude collapses under the comparator threshold; below, the duty goes so lopsided that one segment outlasts the deadline), so a silent point is no evidence that everything under it is silent too. The bracket's edges are protected by the one-sided rule above — a pegged waveform reads as "no pulse" here rather than as a tiny duty error that would close a fake bracket and drag the secant seed into the degenerate zone. Serial: one `[AMP0_SCAN] f=… dutyErr=…%` or `no pulse` per point, then `bracketed …` / `no sign change …` / `no pulse anywhere …`.

**The search** is `find_freq_for_duty50(0, seed, 2.0, refine = true, &bracket)` — bounded, so it spends its whole probe budget inside the bracket instead of stopping at the timeout allowance an unbounded search gets. Because the scan hands it real evidence, it needs no assumption about which way a timeout points.

**Acceptance**: the result has to land inside the band *and* within `kEndpointAcceptDutyPct` (0.5%) of 50%. This point is different in kind from a rung — if the pulse dies before the duty ever reaches 50%, the search converges on the border of the dead zone and returns a frequency whose duty is nowhere near the target, and storing that as pair 0 is worse for the runtime lookup than the extrapolation it was seeded with. A rejection keeps the previous estimate and logs `[LOWEST_FREQ] … rejected` / `[FREQ_TRACE_GUARD] … bottom endpoint rejected` / `[CAL_REFINE_GUARD] pair=0 … rejected` with the number it did not believe.

`apply_measured_lowest_freq(ctx)` is the classic-method wrapper: it seeds from whatever the method left in entry 0 (clamped into the band), and writes `[freq × 100, 0]` on acceptance. `find_lowest_freq()`'s quadratic is not used as the seed — it is fitted to the three lowest points and aimed past all of them, which is exactly where it is least reliable.

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

## Improvement phase: frequency-bisection curve tracing (`FREQ_TRACE`)

The amp-comp table is one monotonic curve — *frequency at 50% duty as a function of amp comp*. The classic method samples it by fixing the frequency (per note) and hunting the integer amp-comp value. That axis is the bad one to search on: amp comp is quantized (the true 50% point almost always falls between two integer steps), duty measurement at low frequencies needs several long periods per probe, and the stepping search takes many probes per note.

`FREQ_TRACE` (selected with `PARAM_DEBUG_COMMAND` 35, back to classic with 34 — the two buttons in the panel's Calibration tab, "Amp-comp calibration method") samples the same curve on the other axis: **fix the amp comp, bisect the frequency** until duty = 50%. The PIO clock divider gives near-continuous frequency resolution, so every stored `[freq, amp comp]` pair is exact — no quantization error is baked into the table. The runtime consumers (`FS.ino`, `precomputeCoefficients()`) treat the table as arbitrary ascending breakpoints, so nothing downstream changes.

### Building blocks (`autotune_search.ino`)

- `drive_freq(freqHz, amp)` — write the probe frequency (once) and remember it in `g_lastDrivenFreqHz` (`autotune.h`). There is deliberately **no glide**: walking to the target in small steps changes the divider again before a full waveform has come out at the previous frequency, which is not settling, it is a frequency ramp — and a ramp is exactly what the duty measurement cannot read. One write, then a wait long enough to produce whole periods. `restart_DCO_calibration()` and `disable_all_oscillators_and_range_pwm()` clear the tracker, so a cold start counts as the largest possible move and gets the full settle budget.
- `measure_duty_at_freq(freqHz, amp, hiRes = false)` — set the frequency, wait the profile's `settlePeriods` periods (floored at `settleMinMs`), then measure with `find_gap()` gated against the probe frequency (`g_gapGateFreqHz`) instead of the current note. `hiRes` selects `find_gap()` mode 3. Rather than trusting one reading, it **reads until the waveform stops moving**: two readings that agree within `settleStableMult ×` the search's own acceptance are averaged and accepted (more waiting could not change what the bisection decides), a disagreement means the core is still catching up so the newer reading becomes the reference and it reads again. How many re-readings are allowed comes from the move itself — none below `kSettleSkipCents` (5 cents, i.e. the late bisection iterations cost exactly what they used to), one below `kSettleBigMoveCents` (a semitone), `settleMaxChecks` above it. A timeout only counts when **nothing valid was measured**: the first reading gets one retry after a move of a semitone or more before a timeout is believed, but once a valid reading is in hand, a re-read discarded by the gap gates (one-sided, off-period, a genuine glitch) merely consumes a settle check and the valid reading stands (`[FREQ_SETTLE] re-read discarded` at debug ≥ 2). Marginal waveforms flicker between clean and glitchy readings, and letting one discarded re-read outvote a valid measurement is how a probe that had already measured the far side of the crossing once became a "no pulse" wall that stopped the pair-1 search 1% short of the answer. Every reading bumps `g_lastFreqBisectProbes` (`probes=`) and the extra ones bump `g_lastSettleChecks` (`settle=`).
- **Adaptive averaging** (`find_gap()` modes 2 and 3): instead of a fixed segment count, hi-res probes hold the profile's measurement window — `samples = clamp(gapWindowMs / halfPeriod, gapSamplesMin, gapSamplesMax)`. High notes get many more segments for almost no extra time (which is exactly where a single segment is only microseconds long and the duty resolution suffers), while the low notes are held to the floor so the slow end does not dominate. Mode 0 (classic search) keeps its 6 segments.
- **One-sided readings are discarded** (`find_gap()` mode 3 only): a reading where every accepted segment has the same polarity means the duty is pegged at 0/100% — the other side's blips fell under the segment floor — and `avgHigh − avgLow` then measures the blips, not the waveform. Converted against the huge ideal period at the bottom of the range, a pegged waveform once scored −0.72% "duty error" and closed a fake amp-0 bracket at 5.43 Hz. Mode 3 returns the timeout sentinel instead (`[GAP_ONESIDED]` at debug ≥ 2), which the scan reads as "no pulse" and the search's evidence logic places on the correct side. The classic search (mode 0) and the PW limit search (mode 2) read extreme duties on purpose and are untouched.
- **Off-period readings are discarded** (`find_gap()` mode 3 only): a healthy reading's segments sum to the ideal period within ~0.5%; when `avgLow + avgHigh` deviates by more than `kGapPeriodTolRatio` (15%), the pin is not toggling at the requested frequency and the reading describes something else. Seen at amp comp 0 near 6 Hz: the pin toggled at ~58% of the requested period (a comparator double-trigger), whose near-symmetric sub-segments read ~49.5% duty at *any* frequency — the search chased a 50% crossing the scope could not see. The 1%..99%-of-period segment gate cannot catch this; only the sum can. Logged as `[GAP_OFFPERIOD]` at debug ≥ 2, then the timeout sentinel.
- `find_freq_for_duty50(amp, fGuess, windowRatio, refine = false, bounds = nullptr)` — **measure the seed, then walk to the answer in bounded steps and interpolate.** `fGuess` is probed first, since every caller passes a modelled seed and that is the point most likely to already be the answer. If it is not, the search steps outward in the direction the duty error indicates by at most `search_step_cap_cents(f)` — 400 cents above 440 Hz, 200 from 100 Hz up, **100 below that** (including the amp-0 hunt under 30 Hz: a 50-cent step barely moved the duty, so the denser 10-point pre-scan is what finds the sign change), growing by `kSearchStepGrowth` until the sign flips. Tighter than any range cap, a real reading sizes its own step: the duty error moves ~3–4% per 100 cents across the whole range, so the step is capped at `dutyErr% × 100 / kSearchSlopeMinPctPer100Cents` (slope assumed 1.5, deliberately flat so the step still overshoots ~2× and brackets in one hop), floored at `kSearchStepFloorCents` (3). A seed that reads −0.07% now steps ~5 cents instead of the full 100 and needs a third of the probes; timeouts have no magnitude and keep the range cap. Once bracketed, a bracket narrower than `kBracketMinWidthCents` (3 cents) is exhausted — the answer is pinned within the measurement noise — and the search stops with its best reading instead of re-probing the same frequency until the budget runs out.

  **Where a timeout points** is decided from evidence where there is any: below a frequency that did produce a signal it can only be the bottom of the range (the duty has gone so lopsided that one segment outlasts the deadline), above one it is the amplitude collapsing under the comparator threshold. With no reading yet it is read as "frequency too high", the collapse, which is by far the common case; the one search that lives at the bottom instead is handed a measured bracket (see the amp-comp-0 endpoint above). An **unbounded** search that spends `kMaxSearchTimeouts` (6) probes *in a row* finding no pulse gives up with its best reading — in a row, because one that keeps producing readings between the dead probes is converging, not lost. A search with `bounds` is exempt and instead stops at the edges of the band; while it has yet to see a pulse at all it also strides up to `kHuntStepMaxCents` (600) per step rather than the per-range cap, since there is no reading to protect and the bounds stop it from overshooting. Once the answer is bracketed it stops halving and **interpolates**: duty error against log-frequency is nearly linear over a small bracket, so an Illinois secant step usually lands inside the acceptance in one or two small moves; a candidate that degenerates or lands within `kBracketEdgeGuard` of a bracket edge falls back to the geometric midpoint `√(fLo·fHi)`, which halves the bracket in *cents* rather than in Hz. `windowRatio` is no longer a window to halve — it is how far the caller expects to travel, and `(bisectWindows + 1) ×` that is the allowance before the search gives up and returns its best reading (the same reach the old window-shift retry had) (`[FREQ_BISECT] … no bracket within … cents`). Probe-capped; returns the best frequency found. `find_highest_freq()` is a thin wrapper over this (amp = `DIV_COUNTER`, note-interval window, no refinement).

  Why it matters at the bottom of the range: the old version took the *arithmetic* midpoint of a geometric window, so the first probe landed 70 cents (a ladder rung) to 386 cents (the amp-0 hunt, an octave-wide window) above the seed and every early step was hundreds of cents — at 16 Hz that is a probe that only sees a waveform still chasing the divider. It also had no early exit when a window failed to bracket, so it spent its whole iteration budget collapsing onto one edge before shifting.

#### Precision profiles (`CalPrecisionProfile`, `autotune_constants.h`)

Everything that trades run time against measurement quality lives in two profiles, picked by the value of `PARAM_CALIBRATION_FLAG` (1/2/3 = NORMAL, 5/6/7 = FINE) and read through `cal_precision()` in `autotune.h`. Nothing else in the calibration code knows which mode is active.

| Knob | NORMAL | FINE |
|------|--------|------|
| `find_gap` modes 2/3 window | 25 ms, 6..32 segments | 60 ms, 12..64 segments |
| settle after a frequency write | 1 period, min 3 ms | 1 period, min 4 ms |
| stability re-readings after a big move | 1, at 3x the acceptance | 3, at 2x the acceptance |
| search acceptance | 0.05% duty, 0.5 µs floor | 0.02% duty, 0.25 µs floor |
| search budget | 24 probes, 3 x window travel | 32 probes, 4 x window travel |
| post-search re-measurement | 3 candidates × 1 reading | 5 candidates × 3 readings |
| 440 Hz anchor corrections | 2 | 3 |

The segment floor is the knob that matters at the bottom of the range: at 16 Hz one segment already takes 31 ms, so 6 instead of 12 halves the cost of every probe down there. The tight acceptance is only meaningful with the long averaging window behind it, which is why the two move together. What makes a reading honest right after a frequency move is the settle plus the stability checks: the frequency is written once and then left alone, and the checks refuse to accept a number until it stops changing. The second half is why the fixed wait can be as short as it is — nothing depends on a constant being generous enough. If probes come back unsettled at the bottom of the range, `settlePeriods` (whole waveforms waited before reading) is the knob, not the step size.

`refine = true` (every `FREQ_TRACE` call, the fine pass and both endpoints) is what switches a search onto the profile's hi-res probes, budget and acceptance; the classic method's `find_highest_freq()` does not use it, so its timing is unchanged. After the search converges, that frequency and its ±0.1% (and, at FINE, ±0.05%) neighbours are each re-measured and averaged, and the smallest absolute duty error wins — so noise on the single converging probe cannot set the stored pair. Candidates that time out are dropped; if all of them do, the search's own result stands.

The achieved (signed) error is kept in `g_lastFreqBisectGapUs` and printed on every `[FREQ_TRACE]`, `[CAL_REFINE]`, `[FREQ_TRACE_MANUAL]`, `[FREQ_TRACE_BOOT]` and `[LOWEST_FREQ]` line as `gapUs=… dutyErr=…% probes=… settle=…`. `dutyErr` is the same number in the unit a scope reads (`100·gap/(2T)`), `probes` is how many duty measurements the search spent (a suspiciously fast run shows up as a low probe count) and `settle` is how many of those went into waiting for the waveform after a frequency change. At `autotuneDebug >= 2` a `[FREQ_SETTLE]` line reports a probe whose readings never agreed within its budget — the oscillator needs longer than the profile allows, so raise `settleMaxChecks` or `settlePeriods`.

### `calibrate_DCO_freq_trace(ctx)`

Slot budget: pair 0 always holds the amp-comp-0 endpoint and the last pair the full-amp endpoint, so the ladder rungs live in `[1, numPairs − 2]`.

1. **Anchor probe**: the stored 440 Hz manual value — amp = `ampComp440[dco]` (set during manual calibration step 1, `PARAM_AMP_COMP_440`), bisected around 440 Hz. If the value is unset (0), the method logs `[FREQ_TRACE_GUARD]` and aborts, keeping the previous table.
2. **Manual trim note**: the other point the user set by hand — amp = `initManualAmpCompCalibrationVal[dco] + manualCalibrationOffset[dco]`, frequency bisected in a tight window (`kManualNoteWindowRatio` = 1.15) around `manual_DCO_calibration_start_note`. It gives the model a ~45-semitone baseline (slope) to go with the anchor cluster (curvature), and `[FREQ_TRACE_MANUAL] … dev=… cents` shows how far the trim actually sits from the nominal note. Model-only, and a failure here is logged and ignored.
3. **Re-anchor at 440 Hz**: the dialled value is a hand measurement taken at a different pulse width (manual mode drives the PW channel at 0, auto-cal at `PW_CENTER[0]`), so it can be far from a real 440 Hz operating point — and everything downstream assumes the anchor really is 440 Hz. With the anchor probe and the long manual-note baseline in the model, up to the profile's `anchorTries` corrections are tried: guess the amp for 440 Hz, re-measure in a `kAnchorWindowRatio` (1.15) window, keep the candidate closest to 440 Hz, stop once it is within `kAnchorToleranceCents` (15). A corrected value is written back to `ampComp440[]` and persisted (`update_FS_AmpComp440`), so the next run starts from a true anchor. Logged as `[FREQ_TRACE_ANCHOR] … stored=… refined=… dev=… cents`.
4. **Bootstrap cluster**: probe 4 extra fixed amps around the anchor — `kBootstrapSemitones` (±3 then ±6 semitones, applied as `anchorAmp × 2^(st/12)`, i.e. about ±19% and ±41% of the amp comp), inner pair first — bisecting the frequency for each and logging `[FREQ_TRACE_BOOT]`. Musical intervals rather than small percentages: the cluster has to span enough of the curve for a quadratic through it to mean something, and a few percent of amp comp does not. Each probe's frequency seed comes from the points measured so far (1 point: proportional, 2: log, 3+: quadratic), so the model sharpens with every measurement. Failed probes are skipped, not fatal. These points feed the extrapolation model only; they are not table entries.
5. **Derived ladder**: the rung spacing is a property of the oscillator, not a compile-time constant. The model is asked where amp comp 1 and `DIV_COUNTER · 0.98` land, and the rungs are spread over that span: `interval = clamp(ceil(spanSemitones / (nRungs + 1)), 3, 12)`, rounded to whole semitones so the rungs stay musically spaced and comparable with the classic tables. Both estimates are extrapolations far outside the measured cluster, so they are bounded: the low one never goes under `manual_DCO_calibration_start_note` (below that the duty probe needs longer than `kGapTimeoutUs` between edges), and the high one falls back to scaling the highest measured point when the quadratic disagrees by more than 2×. The anchor keeps a real slot at the rung matching its own position in log-frequency inside the span. A degenerate model falls back to `calibration_note_interval` and a centred anchor rung. Logged as `[FREQ_TRACE] … ladder interval=N semitones anchorPair=k span=X octaves (fLowEst=… fHighEst=…)`.
6. **Trace upward**: for each next rung, extrapolate the amp for a ×interval-semitone frequency step via `freq_trace_guess()` — quadratic through **3 known points** (manual point + bootstrap cluster + previously traced pairs) — fix that integer amp, bisect the frequency, store the exact resulting pair and add it to the known set. The ladder stops (without measuring anything there) when the extrapolated amp reaches `DIV_COUNTER · 0.98`.
   - `freq_trace_guess()` **brackets and spreads**: it prefers one point below and one above the target (interpolating beats extrapolating) and rejects a candidate that sits closer than `kGuessMinSpread` (10% in x) to an already chosen one. Without that rule a bootstrap cluster tighter than `kGuessMinSpread` alone could drive a quadratic ~25% away from the data. Fewer than 3 qualifying points fall back to a log fit (2) or proportional scaling (1).
   - **Rung retry**: a rung is always stored where it was measured, but if it lands more than `kRungToleranceCents` (25) from its target the amp is corrected (up to the profile's `rungRetries`, 1) with the local log-log slope (`freq_trace_local_slope()`, clamped to 0.5..2.0) and re-measured; the closer of the two measurements wins, and the monotonicity guard against the neighbouring rung still applies. Logged as `[FREQ_TRACE] pair=… retry amp=… dev=… cents`.
7. **Trace downward** from the anchor the same way (amp guesses and frequency seeds both come from the nearest known points, same retry rule), down to the last rung or until the integer amp floor is reached — so the synthetic linear fill below the floor is a last resort rather than the norm.
8. **Endpoints last**: full amp comp and amp comp 0 are the only probes whose frequency is unknown up front and where the pulse can collapse, i.e. the ones that can burn 100 ms timeouts. Running them after the ladder means ~20 measured points are available to seed them, so each searches a tight `kEndpointWindowRatio` (1.25) window instead of an octave. The top endpoint takes the slot right above the last traced rung (remaining slots sentinel-filled, keeping the classic table shape and the runtime plateau / `AMP_COMP_MAX_HZ` behaviour); the bottom endpoint goes into pair 0 and is accepted only when it lands below the lowest rung. Either failure keeps the model estimate and logs `[FREQ_TRACE_GUARD]`.
9. Points are emitted **ascending** into `calibrationData` (22 pairs), with a monotonicity sanity check before `update_FS_voice()`.

Guards: per-point probe caps and keep-best behaviour mirror the classic method; every phase polls `calibrationCancelRequested`.

### Fine pass: `refine_DCO_amp_table(ctx)`

The amp stage of a FINE run (param 150 values 5/6/7) does not build a table — it re-measures the one the oscillator already has. Every amp-comp value is kept exactly as stored and only the frequency it really sits at is measured again, so there is no anchor, no bootstrap cluster, no ladder derivation and nothing extrapolated. It is method-agnostic: fixing an amp and finding its 50%-duty frequency is just as valid for a classic table.

1. Read the oscillator's 22 pairs from `freq_to_amp_comp_array[]` (the raw table `init_FS()` loads at boot) and find `topPair`, the first pair at full amp comp — above it the table is sentinel padding with no operating point behind it.
2. Refuse to run on a table that was never calibrated: `topPair` must be at least 4, frequencies strictly increasing, amp comp non-decreasing, and at least 4 distinct amp values (a seeded/fake table is flat). Otherwise `[CAL_REFINE_GUARD] … run a normal calibration first` and the previous table is kept.
3. For pairs 0..`topPair`, `find_freq_for_duty50(storedAmp, storedFreq, kRefineWindowRatio /*1.02*/, true)`. The stored frequency is the previous answer for that exact amp, so the window is deliberately tight — ±34 cents, giving ~8.5-cent opening steps. A wide window is what let one noisy first reading send a pair hunting far from a value that was already right; with the tight one a pair that really drifted shows up as the search giving up at the window edge and a large `moved=` in the report, which is worth seeing. A pair with no signal keeps its stored frequency and is tagged `filled`.
4. Sentinels are copied through untouched, and the emitted table goes through the same monotonicity check (`[CAL_REFINE_ERROR]` on failure, table not persisted).

Each pair logs `[CAL_REFINE] DCO=n pair=p amp=… stored=… -> found=… moved=… cents gapUs=… dutyErr=…% probes=…`, and the oscillator ends with a `measured=N/M`, average duty error and largest move summary. A large `moved=` on one pair is the interesting signal: that point was wrong before, and now it is not.

### Reading the results: `[CAL_REPORT]` and `[CAL_VERIFY]`

Both are printed at `autotuneDebug >= 1`, so a quiet run stays quiet.

**`[CAL_REPORT]`** — printed by `DCO_calibration()` for each oscillator right after the raw 44-value dump (which stays, the panel parses it). Every pair reports where it came from and the duty error achieved when it was measured, so a bad point is visible without re-measuring anything. All three paths fill it: `FREQ_TRACE` records each stored point as it is measured, the classic method uses the per-note `closestToZero` it already tracks, and the fine pass tags every re-measured pair `refined`.

```
[CAL_REPORT] DCO=0 method=FREQ_TRACE precision=NORMAL ladder=5 semitones anchorPair=9
[CAL_REPORT] pair    freqHz  ampComp  dutyErr%     gapUs    1cnt%  src
[CAL_REPORT]    0      7.53        0     0.030     39.84        -  endpoint-amp0
[CAL_REPORT]    1     16.26       28    -0.050    -61.50    1.786  rung
[CAL_REPORT]   21         -    14000         -         -        -  sentinel
[CAL_REPORT] DCO=0 lowest=7.53 Hz highest=3938.62 Hz span=9.03 octaves measured=21/22
[CAL_REPORT] DCO=0 dutyErr avg=0.05% worst=0.12% at pair 9 (161.20 Hz)
```

- `src` is `rung`, `anchor`, `endpoint-full`, `endpoint-amp0`, `manual`, `refined`, `filled` or `sentinel`. Synthetic and sentinel slots print `-` instead of a number, so a padded table cannot be mistaken for a fully measured one, and `measured=N/22` counts only real measurements. `method=REFINE precision=FINE` in the header means the table came out of the fine pass.
- `1cnt%` is the duty change one count of amp comp causes at that point (first order: duty − 0.5 scales with the relative amplitude error, so one count ≈ `50/amp` percentage points). It is the floor for that frequency: a `dutyErr%` already below it is as good as the hardware allows, which is why the lowest notes cannot be improved by more averaging.
- The `lowest` / `highest` line is the amp-comp-0 and full-amp endpoints, i.e. the reachable frequency range of the oscillator.

**`[CAL_VERIFY]`** — `PARAM_DEBUG_COMMAND` **36** (panel: Calibration tab → Dev tables → "Verify sweep"), a read-only pass that measures the finished tables the way the engine uses them: walk MIDI notes in 3-semitone steps up to the table's plateau, take the amp from the **runtime lookup** `get_chan_level_for_engine()` (not from the table row) and measure the duty. It runs on core 1 like a calibration and honours the cancel flag.

```
[CAL_VERIFY] DCO=0 note=24 freq=32.70 amp=105 dutyErr=0.312% gapUs=9.55 1cnt=0.476%
[CAL_VERIFY] DCO=0 points=31 dutyErr avg=0.180% worst=0.612% at 32.70 Hz
```

Reading the sweep: a **constant** error is a frame-of-reference offset (use the duty trim below); error **peaking between breakpoints** is interpolation; error **growing toward low notes** is the one-count floor shown by `1cnt=`.

### Duty target trim (`PARAM_AMP_COMP_DUTY_OFFSET` = 161)

The calibration sense pin is a digital input with its own thresholds, while a scope measures the analog pulse at its own 50% level, so "50% on the board" and "50% on the scope" can differ by a fixed amount across the whole range. `ampCompDutyOffset[osc]` (hundredths of a percent of duty, −500..500, default 0 = historical behaviour) shifts the target both amp-comp methods aim at, applied as a target *gap* rather than a target duty: from duty − 0.5 = gap/(2T), `gapTarget = 2·T·offsetFraction` (`duty_trim_gap_us()`). `measure_duty_at_freq()` (frequency probes), `measure_gap_for_amp()` (classic) and the manual `DCO_calibration_debug()` readout all subtract it, so manual trimming and auto-cal share one frame of reference. It is per oscillator, selected like `PARAM_AMP_COMP_440` by `PARAM_MANUAL_CALIBRATION_STAGE`, persisted by `PARAM_MANUAL_CALIBRATION_STORE` into the LittleFS `AmpCompDutyOffset` bank.

### Why it's faster and better

- Two speeds, one algorithm: NORMAL builds the whole table quickly, FINE spends its budget only on re-measuring pairs that already exist, so the careful settings never have to pay for the model-building phases.
- Bisection converges in ~10–14 probes per point vs. dozens of stepped integer probes.
- Most probes run at mid/high frequencies where 3 periods are milliseconds, not tens of milliseconds.
- Both manual operating points are used: the trimpot point at the low note and the dialled 440 Hz point, so nothing the user set by hand is thrown away — and the trim's deviation is reported in cents at the start of every run. The 440 Hz value is treated as a seed and re-measured, then corrected in flash if it was off (see step 3), so a hand measurement taken at the wrong pulse width does not tilt the whole curve.
- Manual calibration gained a second step at 440 Hz (`manual_cal_reference_note` = 69): the trimpot stage stays at note 24 (so PW cal and the classic method keep their baseline), then the user dials in the absolute amp value `ampComp440` at 440 Hz where duty feedback refreshes ~13× faster. That stored value is the curve anchor (see [`CALIBRATION_PROCEDURE.md`](CALIBRATION_PROCEDURE.md)).
- PW calibration is unchanged and stays at its current note; moving it up is pending a hardware check that the PW center is frequency-independent.

The classic method remains the shipping build default (`AUTOTUNE_AMP_METHOD_DEFAULT` = 0 in [`DCO.ino`](../DCO.ino)) until `FREQ_TRACE` is validated on hardware: run both on the same board, dump and compare tables and total runtime, then flip the flag. The 34/35 buttons only change the live method; every auto-cal run prints `[DCO_CAL] amp-comp method: …` as its first line, and `amp_cal=` on the profiler `engine:` line (debug cmd 10) shows the live selection at any time.

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

The improvement phase (frequency-bisection curve tracing, adaptive settle times, 440 Hz manual stage) is described in its own section above. Still open: more calibration points, runtime table quality checks, feeding bisection midpoints into the PW candidate table.

**Hardware verification:** run a full `DCO_calibration()` and compare the dumped `calibrationData` tables against a known-good run (e.g. `dco_calibration_DCO3_first prototype_2.json`).
