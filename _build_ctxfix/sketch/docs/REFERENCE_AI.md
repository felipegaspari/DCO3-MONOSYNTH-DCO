#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/REFERENCE_AI.md"
## DCO Project: AI Codebase Reference

This document is a **semantic map** of the DCO board firmware for the DCO3 monosynth (RP2040 / RP2350-class, **1 voice × 3 oscillators**).  
It explains what each file does and how the main subsystems (voices, modulation, calibration, storage, I/O) fit together.

Related docs:
- Flat file + function + call-site inventory: [`FILE_INDEX.md`](FILE_INDEX.md) (same `docs/` folder)
- System topology (other boards): [`SYSTEM_OVERVIEW.md`](SYSTEM_OVERVIEW.md)
- Complete compile-time flag catalog: [`BUILD_FLAGS.md`](BUILD_FLAGS.md)
- Float vs fixed engine math (depth): [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- Hot-path profiling: [`BENCHMARKING.md`](BENCHMARKING.md)
- SRAM / heap / stack: [`MEMORY.md`](MEMORY.md)
- Autotune algorithms: [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md) (this board: [`AUTOTUNE.md`](AUTOTUNE.md))
- MCU presets / cal dump-restore: [`PRESET_STORE.md`](PRESET_STORE.md)
- Repo entry point: [`../README.md`](../README.md)

---

## 1. Top-Level Sketch, Cores and Aggregated Includes

- **`DCO.ino`**  
  - Main application for the RP2040 / RP2350.  
  - Runs on both cores using the Arduino dual-core API:
    - `setup()` / `loop()` (core 0): USB/serial/MIDI I/O, LFO evaluation (~50 µs tick), one-shot `preset_store_boot_task()` after ~1.5 s.
    - `setup1()` / `loop1()` (core 1): FS init, ADSR init, DCO calibration/autotune, real‑time voice engine.  
  - **Engine build options** (top of file: **pitch ids** → **board defaults** → **overrides** → **guards** → profiling / board):
    - Board defaults: RP2350 float voice/amp, `PITCH_INTERP_FLOAT_FAST`, amp method `FLOAT_QUAD`, `CLKDIV_FLOAT`; RP2040 fixed voice/amp/CV, `PITCH_INTERP_RATIO_Q16`, amp method `FIXED`, `CLKDIV_Q16`. No `USE_FLOAT_ENGINE` umbrella.
    - Overrides can `#undef` / `#define` those flags (pitch A/B needs `#undef PITCH_INTERP_MODE` first).
    - Full catalog: [`BUILD_FLAGS.md`](BUILD_FLAGS.md). Math depth: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).
  - Configures USB product strings in `setup()` (via Adafruit TinyUSB; product **DCO3-MONO**), toggles board pins (23/24) for hardware fixes, and selects DCO calibration mode.
  - Core 0 writes LFO pitch mods into `lfo1_pitch_mod_q24[]` / `lfo2_pitch_mod_q24[]` every ~50 µs; core 1 reads them in the voice task (float path converts Q24 → float each frame).
  - `loop1()` calls `voice_task_main()` (dispatch to float or fixed), then `bench_service(1)` to hand its profiler counters to core 0, then `mem_diag_poll_core1()` (empty without `ENABLE_MEM_DIAG`; runtime 14/15 can disable).
  - **Profiling** (`RUNNING_AVERAGE`, optionally `RUNNING_AVERAGE_FINE`): both loops are bracketed by `BENCH_*` probes from [`bench.h`](../bench.h); core 0 does all the printing from `bench_poll_core0()`. See [`BENCHMARKING.md`](BENCHMARKING.md).
  - **RAM dump** (`PARAM_DEBUG_COMMAND` 13, needs `ENABLE_MEM_DIAG`): `mem_diag_request` → Core1 stack snapshot → Core0 prints heap/stacks. Runtime 14/15 disable/enable polls. See [`MEMORY.md`](MEMORY.md). `__not_in_flash_func` is static `.time_critical` SRAM (not heap); pin policy is hot leaves only (`loop` / serial tasks stay in flash).

- **`include_all.h`**  
  - Convenience umbrella header used by most `.ino` implementation files.  
  - Pulls in RP2040/Arduino headers and project modules: `params_def`, `param_router`, `globals`, `FS`, `noteList`, `amp_comp`, `Serial`, `midi`, `voices`, `state_machines`, `PWM`, `utils`, `Timer_micros`, `mem_diag`, `LFO`, `adsr`, `PID`, `autotune`.

- **`globals.h`**  
  - System‑wide constants and state:
    - Voice/osc counts: `NUM_VOICES_TOTAL = 3`, `NUM_OSCILLATORS = 3`, `NUM_VOICES_VOICE_TASK = 1` (mono engine bound; PW per-osc; para planned via `setVoiceMode`).
    - Clock and PIO timing constants (`sysClock_Hz` = Arduino `F_CPU` until boot, then `clock_get_hz(clk_sys)` via `sys_clock_hz_refresh()`; runtime `pioPulseLength` default 1600 / debug 160 ∈ [200, 50000], OSR chunk sizes, timing overheads).
    - **Period model** `period = Y + weight*clk_div + overhead`, with weights/overheads `{4,5,6,7}` / `{12,13,14,15}` indexed by `softSyncChunks` (`PIO_*_BY_CHUNKS[]`; N=1 aliases `PIO_RAMP_WEIGHT_SYNC` / `PIO_PERIOD_OVERHEAD_SYNC`). Each trailing polled chunk adds one weight and one overhead cycle.
    - Per-osc PIO state: `osc_uses_sync_program[]`, `osc_last_y[]`, `osc_last_clk_div[]`, `softSyncChunks`, `pio_loaded_sync_chunks`, `subOscDivide`.
    - With `ENABLE_SUBOSC_ENGINE2` (RP2350 default): `SUBOSC_COUNT = 2`, `subOscMaster[]` / `subOscDivides[]` / `subOscPhaseDeg[]` / `subOscWidth[]`, `subOscLogicOp` (0..8), `subosc_mod_phase` / `subosc_mod_pw` (sub 2 only); `SUBOSC_PINS = {8,9}`, `SUBOSC_LOGIC_PIN = 10`.
    - Fixed‑point pitch‑bend multipliers (`pitchBendMultiplier_q24`); LFO pitch mods live in `LFO.h` (`lfo1_pitch_mod_q24[]`, `lfo2_pitch_mod_q24[]`).
    - Global voice arrays (`VOICE_NOTES`, `VOICES`, `note_on_flag`, shared `PW[0]`, etc.).
    - Hardware pin mappings from `DCO_MCU_BOARD` in `project_config.h` (default Pico 2): `RESET_PINS = {19,18,15}`, `RANGE_PINS = {17,16,14}` (DCO4 osc 3/4/5 — identical on WeAct, Pico, and Pico 2), `PW_PINS[0] = 3`, classic `SUBOSC_PIN = 8`. WeAct KEY GP23 = A440; Pico/Pico 2 `SMPS_PS_PIN` 23 HIGH.
    - `VOICE_TO_PIO = {0,0,0}` — **all three oscillators share pio0.** A GPIO's function select names exactly one PIO block, so oscillators on separate blocks cannot share a reset pin: `pio_gpio_init()` on the second block silently steals the pin from the first, which is what broke hard sync when the layout was `{0,1,2}`. `pio_topology_report()` asserts this.
    - `VOICE_TO_SM` is **mutable**, rewritten by `assign_sm_mapping()`: the slave takes the lower SM index because when two SMs write a pin on the same cycle the higher-numbered one wins, so the master must outrank its slave or it drops the occasional sync edge.
    - `DCO_calibration_pin = 6` (temporary A/B on Pico header; was 10; GP25 aborted); `ENABLE_FS_CALIBRATION`.
    - Shared PIO array `pio[]` (length 3 when engine2, else 2), timer variables, MIDI pitch bend state and helper prototypes.

---

## 2. Voice Architecture & Real-Time Engine

- **`voices.h`**  
  - Declares `init_voices()` and core voice‑engine globals:
    - Portamento configuration and mode (`PORTA_MODE_TIME` / `PORTA_MODE_SLEW`).
    - Per‑DCO portamento state in **Q24 Hz** (`portamento_*_q24`) and in **Q16 semitone space** for slew‑rate mode.
    - When `USE_FLOAT_VOICE_TASK`: parallel float portamento state (`porta_*_f`) in Hz and semitone domains.
    - Pitch multiplier storage gated by `PITCH_INTERP_MODE`: float tables + `slopeF` (`FLOAT` / `FLOAT_FAST`), or int tables + `slopeQ20` (RATIO) / `slopeQ12` (Q12); plus `interpSegCache`.
    - No profiler declarations. Probe storage is generated from the `BENCH_PROBES` table in [`bench.h`](../bench.h); the extern block that used to live here named probes that had been renamed or removed and compiled regardless, since an unused `extern` needs no definition.

- **`voices.ino`**  
  - Central **voice engine** and DCO front‑end with a **compile-time dual implementation**:
    - `init_voices()` sets initial notes, builds pitch multiplier tables for the active `PITCH_INTERP_MODE` (`initMultiplierTables()`), sets voice mode and runs an initial `voice_task_main()`.
    - `voice_task_main()` → `voice_task_float()` **or** `voice_task_fixed_point()` depending on `USE_FLOAT_VOICE_TASK`.

    - **`voice_task_fixed_point()`** (fixed hot path, when `!USE_FLOAT_VOICE_TASK`):
      - For each active MIDI voice (monosynth: one voice driving **OSC1/OSC2/OSC3**):
        - Computes per‑voice portamento in either **time‑based frequency space** or **slew‑rate note space** (Q24 / Q16).
        - Combines fixed‑point modulators:
          - Pitch‑bend (`calcPitchbend_q24`).
          - LFO1 per‑osc pitch mod (`lfo1_pitch_mod_q24[LFO1_PITCH_OSCn]`, includes `LFO1toDCO` + extra) and LFO2 OSC2/3 mods.
          - Unison detune per osc (`OSC_UNISON_STEP` = `{0, +1, -1}`; optional poly voice-index term if `NUM_VOICES_TOTAL > 1`).
          - Per‑osc drift LFO (`LFO_DRIFT_LEVEL`) with analog drift amount.
          - ADSR‑to‑detune in Q24 using `ADSR1toDETUNE1_scale_q24` and `linToLogLookup` (select includes OSC3 / all).
        - Evaluates the pitch multiplier table per `PITCH_INTERP_MODE`: `interpolateRatioQ16_cached` (`RATIO_Q16`, slopeQ20 fused) or `interpolatePitchMultiplierIntQ16_cached` (`Q12`). See [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).
        - Produces final osc frequencies in **Q24 Hz**, then clock‑dividers via `CLKDIV_MODE`:
          - **0 GOLD**: Q24 → double Hz → `llround(sys / hz)` (gold standard / A/B).
          - **1 FLOAT**: Q24 → float Hz → `fminf(sys/hz + 0.5)` (float-engine math).
          - **2 Q16**: Q16 Hz → 64/32 (shipping).
          - **3 Q8**: Q8 Hz, two 32/32 + remainder (`f_int < 16` → internal precise Q8).
          - **4 FAST_Q4**: compact **Q4 Hz** then 32‑bit divide (~1 µs/voice).
          - Corrected OSR clock dividers for OSC1–3 including OSC2 phase‑alignment; OSC3 free-running.
        - Performs **amplitude compensation** via `get_chan_level_lookup_fast()` (Q8 Hz domain) using precomputed quadratic windows (`amp_comp.h`) → **RANGE PWM** via `write_range_pwm()` (slice or PIO dither; not a PIO oscillator).
        - Writes new dividers into the three PIO SMs on pio0 and amp levels into RANGE channels.
        - With `ENABLE_SUBOSC_ENGINE2`: `subosc2_update_periods()` recomputes each running sub's 3-word DMA group from the period of the oscillator that sub follows (`vt_subosc`).
        - At 99 µs intervals (`timer99microsFlag`), updates shared PW PWM, combining ADSR1 and LFO2 modulation in integer math and using `get_PW_level_interpolated()`.

    - **`voice_task_float()`** (float hot path, when `USE_FLOAT_VOICE_TASK` — **current default**):
      - Same overall structure (portamento → modifiers → ratio → clkdiv → amp → PIO/PWM/PW), but in **Hz / float**:
        - Float portamento state; pitch bend / LFO / ADSR / drift / OSC3 interval+detune converted from Q24 globals where needed.
        - Pitch table: `interpolateRatioFloat_cached_fast` when `PITCH_INTERP_FLOAT_FAST`; walk `interpolateRatioFloat_cached` when `PITCH_INTERP_FLOAT`; or fixed `RATIO_Q16` / IntQ16 via `PITCH_INTERP_MODE` (`×10000`→Q16 glue) for A/B. Shipping default is fixed voice + `RATIO_Q16` (this float path is override-only).
        - Clkdiv via `clkdiv_live_hz_total_cycles` (`CLKDIV_MODE`; Q16/Q8/FAST_Q4 convert Hz→Q24).
        - Amp via `get_chan_level_for_engine()` → float or fixed facade depending on `USE_FLOAT_AMP_COMP`.
      - Details: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §6.

    - Legacy `voice_task_simple` / `voice_task_debug` / gold reference: **removed** (see `_removed/` if needed).
    - Voice allocation helpers (scaffolding; with `NUM_VOICES = 1` they collapse to mono):
      - `voice_alloc()`, `voice_mark_on()` and `voice_mark_off()` adapt the shared allocator in `_shared/voice_alloc.h` (see `note_on()` below).
      - `setVoiceMode()` configures `NUM_VOICES` / `STACK_VOICES` and resyncs the allocator to the gates.
      - `setSyncMode()` calls `assign_sm_mapping()` + `start_voice_sms()` to rebuild the whole sync topology (OSC1↔OSC2; OSC3 free-running), then forces a re-trigger. It no longer pokes sideset pins in place or calls `pio_sm_restart()` — that cleared the shift counters but left PC/X/Y, which could strand an SM mid-loop with a stale X for one glitched period.
    - Amplitude compensation helpers:
      - `get_chan_level_lookup_fast()` – optimized fixed‑point quadratic interpolation per DCO (always built; live FIXED method under float engine), using cached window indices and Q28 reciprocals.
      - `get_chan_level_float_quad()` – cached-walk float quadratic (live FLOAT_QUAD; also LUT fill / accuracy gold); `get_chan_level_lut()` – dense nearest-Hz LUT.
      - `get_chan_level_float()` / `get_chan_level_for_engine()` – dispatch on `amp_comp_method` (default FIXED).
      - `get_PW_level_interpolated()` – maps PW counts into calibrated limits and center values (shared by both engines).
    - Calibration front‑end:
      - `voice_task_autotune()` – dedicated per‑oscillator routine used during DCO/DCO+PW calibration to drive the PIO and PWM into specific measurement or calibration modes (float-style clkdiv + `get_chan_level_for_engine`).
    - Timing diagnostics:
      - Every stage of both voice tasks is bracketed by `BENCH_*` probes; the report is produced by [`bench.h`](../bench.h) on core 0. See [`BENCHMARKING.md`](BENCHMARKING.md).
      - Cmds 32/33 (`clkdiv_bench.ino`) – all six methods on both voice engines vs GOLD_REF (`pctVsGOLD_REF`; `RUNNING_AVERAGE`). See [`BENCHMARKING.md`](BENCHMARKING.md) §10.

---

## 3. Oscillator, PIO State Machines and PWM

- **`pico-dco.pio` / `pico-dco.pio.h`**  
  - PIO programs that generate the actual DCO rectangular wave trains with support for:
    - High and low periods controlled via OSR loads.
    - Optional oscillator sync (reset / phase‑aligned modes).
  - `frequency_sync_4_jumps_program` is the primary program used for production.

- **`state_machines.h` / `state_machines.ino`**  
  - **Full subsystem reference: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md)** — programs, period model, sync modes, phase align, sub-osc, invariants and bench procedures. Read it before changing anything in this section.
  - `init_pio()` loads `frequency_sync_4_jumps` plus one soft-sync poll image into **pio0**, noise on pio1, then either classic `subosc_div2`/`div4` on pio1 **or** `subosc2_init()` on pio2 (`ENABLE_SUBOSC_ENGINE2`), then `assign_sm_mapping()` / `start_voice_sms()`.

- **`subosc.h` / `subosc.ino`** (`ENABLE_SUBOSC_ENGINE2`)  
  - Two edge-locked subs on pio2 SM0/SM1 (`subosc_seg` @ offset 8, 3-word DMA), each following any oscillator's reset via `subOscMaster[]`, combined by the boolean logic combiner on SM3 (`subosc_logic` @ origin 0) whose output — including pass-throughs of either sub — is the pad that gets mixed. ParamIds 90–99; mod dests 10/11 (sub 2 only); debug opcode 4. Pins GP8/9, combiner out GP10. Detail: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9, [`PINOUT.md`](PINOUT.md).
  - `start_voice_sms()`:
    - Calls `ensure_soft_sync_program()` so the resident poll image matches `softSyncChunks` when soft sync is on (swap via remove/add; hard sync leaves the current image unused).
    - Picks each oscillator's program: the slave runs the poll variant when `softSyncChunks > 0`, everything else runs `frequency_sync_4_jumps`.
    - Picks the sideset pin. For **hard sync** the master's sideset points at the *slave's* reset pin, so the master discharges the slave's integrator while the slave keeps its own schedule. For **soft sync** the master leaves its own pin alone and the slave polls it through `jmp pin` instead.
    - Preloads Y with `pioPulseLength` and re-pushes `osc_last_clk_div[]`, because writing Y consumes the OSR that also feeds the chunk reads.
    - Starts every SM on the same cycle with `pio_enable_sm_mask_in_sync()`, removing the inter-oscillator skew that capped phase-align accuracy.
  - **Sync flavours:** hard sync costs nothing (weight 4) but is analog-cap-only — it discharges the slave without restarting its counter. Soft sync (weights 5/6/7 for 1/2/3 trailing polled chunks) restarts the slave's own count; receptive windows are ~40% / ~67% / ~86% of the ramp because polled chunks run at half speed.
  - **Phase offset** (`phaseAlignOSC2`) is a one-shot X countdown to `loop_final` (`osc_phase_align_hold_stopped` / `osc_phase_hold_target`, addr 9 on `frequency_sync_4_jumps`). That is the old 8-chunk `jmp 10; out x` recipe retargeted after the program cut moved address 10 to restart. Y stays the real pulse; later cycles are undistorted. SYNC_JMP is 0° only (X preload needs a stopped SM). Detail: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §8.
  - **Diagnostics:** `pio_topology_report()` (roles + reset-pin ownership), `pio_period_probe()` / `pio_solve_period_model()` (confirm weight/overhead against a frequency counter), `mem_diag` dump cmd **13** / polls 14–15 (heap/stack — [`MEMORY.md`](MEMORY.md)).

- **`PWM.h` / `PWM.ino`**  
  - Voice and cal write amplitude through **`write_range_pwm(osc, level)`** (domain still `0..DIV_COUNTER` = 14000).
  - **`RANGE0_PIO_DITHER_TEST` on (shipping):** dithered PIO PWM on the same `RANGE_PINS[]` (GP17/16/14) — wrap 4666, 3-frame dither → 13998 levels, ~54 kHz @ 250 MHz. `init_range_pio_dither()` after `init_pio()`; 6 DMA channels. Detail: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §4.4.
  - **Flag off:** `init_pwm()` configures hardware PWM slices (`RANGE_PWM_SLICES` / `RANGE_PWM_CHANNELS`, `wrap = DIV_COUNTER`).
  - Shared PW PWM (voice 0): `PW_PINS` mapped to `PW_PWM_SLICES`, `wrap = DIV_COUNTER_PW`.

---

## 4. Envelope Generator (ADSR) and Modulation (LFO & Drift)

- **Vendored library: `ADSR_Bezier`** (`#include <ADSR_Bezier.h>`)  
  - Located at `_build_libs/ADSR_Bezier` (symlink to monorepo-root `ADSR_Bezier`, branch **`main`**).  
  - Compile-time: `ADSR_BEZIER_USE_FLOAT` (`0` = Q24/Q16 fixed default, `1` = float time index); **`ADSR_BEZIER_NATIVE_Q15=1`** (DCO shipping — amp domain Q15).  
  - Unipolar Q15 primary (`ADSR_Q15_ONE`, `getWave()` / `levelQ15`; `levelDac` only if needed off hot path).  
  - DCO constants: **`ADSR_CV_CC=4095`** (max CV / ctor `levelDac` export); **`ADSR_CV_SCALE=4096`** (panel sustain → Q15 via `>>12`).  
  - RP2040‑friendly ADSR class (`adsr`) using **Bézier‑based curve lookup tables**.

- **`adsr.h` / `adsr.ino`**  
  - DCO4‑specific wiring of the ADSR Bezier library:
    - Defines the main ADSR resolution (`ADSR_1_DACSIZE = 4000`, `ARRAY_SIZE = 512`) and log/exp lookup tables (`linToLogLookup`).
    - Instantiates one `adsr` object per voice (`adsr1_voice_0 .. 3`) and wraps them in `ADSRVoices[]`.
    - Global ADSR parameters: `ADSR1_attack`, `ADSR1_decay`, `ADSR1_sustain`, `ADSR1_release`, curve parameters, restart flag and modulation depths (`ADSR1toDETUNE1`, `ADSR1toPWM`), including precomputed Q24 scale `ADSR1toDETUNE1_scale_q24`.
  - `init_ADSR()`:
    - Generates Bézier tables via `adsrBezierInitTables()`.
    - Fills `linToLogLookup` using `linearToLogarithmic()`.
    - Applies initial A/D/S/R and restart settings to all voices.
  - `ADSR_update()`:
    - Called from `loop1()` at ~10 kHz (100 µs gate).
    - Parameterless `noteOn`/`noteOff`/`getWave()` (each reads time); EnvDCO/EnvVCA per voice; EnvVCF/EnvVCF2 once.
    - With `NATIVE_Q15=1`, `getWave` fills `*_q15` only (no per-tick `levelDac`).
    - Processes `noteStart[]` / `noteEnd[]`; calls `ADSR_set_parameters()` lazily (VCA/VCF sustain via `ADSR_CV_SCALE` → Q15).
  - `ADSR_set_parameters()`:
    - Debounces A/D/S/R changes (checks against last values every 5 ms).
    - Efficiently re‑applies updates only to parameters that changed, across all voices.
  - Helper functions:
    - `ADSR1_set_restart()` – toggles legato vs per‑trigger behaviour for all voices.
    - `ADSR1_change_curves()` – re‑applies timing and restart settings after curve changes (hook point for future curve editing).

- **CV u12 edge (`cv_out.ino`)** — `CV_U12_MAX=4095` (clamp / legal PWM codes); `CV_U12_SCALE=4096` (Q15→u12 in `cv_q15_to_u12`: `(q15 * SCALE) >> 15`). Details: [`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md), [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).

- **External library: `mo-lfo`** (`#include <mo-lfo.h>`)  
  - LFO class from **mo‑thunderz**, installed as an Arduino library (not under `src/`).
  - Uses a 32‑bit fixed‑point phase accumulator driven by `micros()`.
  - Supports waveforms: off, saw, triangle, sine (lookup table), square.
  - Works in free‑running or BPM‑synced mode; exposes phase and amplitude control.
  - Used as the basis for LFO1, LFO2 and per‑DCO drift LFOs.

- **`LFO.h` / `LFO.ino`** — full reference: [`LFO.md`](LFO.md).  
  - Live bus is **full-scale Q15** (`getWaveQ15` / `setAmplQ15`); ctor `dacSize` is unused.
  - Pitch/drift depth scales in `LFO.h` (`LFO1_PITCH_DEPTH_SCALE` 1700, `LFO2_PITCH_DEPTH_SCALE` 512, `DRIFT_PITCH_DEPTH_SCALE` 1000) + `lfo_pitch_depth_q24` + synth-side `applyDepthQ24` (not in mo-lfo).
  - Instances: `LFO1_class`, `LFO2_class`, `LFO_DRIFT_CLASS[NUM_OSCILLATORS]`.
  - Globals: `LFO1Level` / `LFO2Level` / `LFO_DRIFT_LEVEL[]` (Q15); pitch depths `LFO1toDCO_q24`, `LFO1toOSC*_q24`, `LFO2toOSC*_q24` (+ coarse), `LFO2toPW`.
  - **LFO1 pitch routing:** `PARAM_LFO1_TO_DCO` folded into each `lfo1_pitch_mod_q24[OSCn]` (`applyDepthQ24` of `LFO1toDCO_q24 + LFO1toOSCn_q24`). Per‑osc extras `PARAM_LFO1_TO_OSC1/2/3`. LFO2 fine + coarse → `lfo2_pitch_mod_q24[]`; coarse uses `LFO1_PITCH_DEPTH_SCALE`.
  - `init_*` / `LFO1` / `LFO2` / `DRIFT_LFOs` on Core 0 (~100 µs with LFO2 + drift).

---

## 5. Tuning, Calibration & Amplitude Compensation

- **`amp_comp.h`**  
  - Defines data structures and precomputation for **per‑DCO amplitude compensation**, with a dual runtime engine:
    - Shared: `freq_to_amp_comp_array`, plateau metadata, float coeffs `aCoeff` / `bCoeff` / `cCoeff`, `AMP_COMP_MAX_HZ = 7000`.
    - Shared `ampCompArray` as `int32_t`. Fixed Q8 tables always present; under float also `ampCompFrequencyHz`, `ampCompLut[osc][0..7000]`, and selectable `amp_comp_method` (`FLOAT_QUAD` / `LUT` / `FIXED`; live default FIXED).
    - Fixed path: `ampCompFrequencyArray` in **Q8 Hz** (`FREQ_FRAC_BITS = 8`), per‑window `y(t) = a*t^2 + b*t + c` with `T_FRAC = 12`, `invDxWIN_q28`, `aQWIN_fast` / `bQWIN_fast`.
    - Float quadratic: runtime `y = (a*x+b)*x+c` in Hz (`get_chan_level_float_quad` cached walk).
  - `precompute_amp_comp_for_engine()` — float precompute + LUT fill + fixed Q8 seed/precompute (or fixed-only) after FS load in `setup1()`.
  - `precomputeCoefficients_OLD()` — legacy precomputation path retained for reference and debugging.
  - Flag / format details: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §7.

- **`autotune.h` / `autotune.ino`** (+ helper headers)  
  - Lives in the shared library: the sketch files are one-line shims onto `_shared/autotune.h`, `_shared/autotune_impl.h` and `_shared/autotune_search_impl.h`. Edit the shared copies; see [`_shared/README.md`](../_shared/README.md), [`FILE_INDEX.md`](FILE_INDEX.md) §4, and [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md).
  - DCO and PW **autocalibration subsystem**:
    - Flags and state: `calibrationFlag`, `manualCalibrationFlag`, `firstTuneFlag`, `manualCalibrationStage`, offsets per oscillator, PW calibration values, note indices.
    - Calibration arrays (`calibrationData[]`) store [frequency, amplitude] pairs used to rebuild amp‑comp tables.
  - Included helpers (file roles: [`FILE_INDEX.md`](FILE_INDEX.md) §4):
    - **`autotune_constants.h`** — shared constants / sizes.
    - **`autotune_context.h`** — `DCOCalibrationContext` grouping for `calibrate_DCO`.
    - **`autotune_measurement.h`** — structured `GapMeasurement` wrappers around edge timing.
  - Boot default amp method is `FREQ_TRACE` (`AUTOTUNE_AMP_METHOD_DEFAULT` = 1); method / search / amp-0 enums live in `_shared/autotune.h`.
  - `DCO_calibration()`:
    - High‑level procedure (called from `loop1()` on `calibrationFlag`):
      - Calibrates PW once per assigned channel via `cal_pw_channel(osc)` (`find_PW_center()`, `find_PW_limit_v2()` low/high). Osc 1/2 are `PW_PIN_UNASSIGNED`, so one cal on channel 0.
      - For each oscillator, `restart_DCO_calibration()` then `calibrate_DCO()` or `calibrate_DCO_freq_trace()` to populate `calibrationData[]`.
      - Persists data using `update_FS_voice()` and refreshes amp‑comp tables with `init_FS()` and `precompute_amp_comp_for_engine()`.
  - `restart_DCO_calibration()`:
    - Reset the note schedule and `calibrationData` header between oscillators; re‑arms RANGE pin/PIO.
  - `find_PW_center()` / `find_PW_limit_v2()`:
    - PW target-duty searches built on the phased `find_PW_for_target_duty()` (coarse scan → bisection or fine scan → lock‑in) and `search_PW_limit_from_center()`; all probes go through `set_pw_and_measure()`.
    - Persist PW calibration values into LittleFS via `update_FS_PWCenter()` / `update_FS_PW_Low_Limit()` / `update_FS_PW_High_Limit()`.
  - `find_gap()` / `DCO_calibration_debug()`:
    - Edge‑timing measurement core (state fully local) that measures the DCO duty cycle at the calibration pin; consumed via the `measure_gap()` wrapper.

- **`autotune_search.ino`** (formerly `PID.ino`; the `PID_v1` dependency was removed)  
  - `calibrate_DCO()`:
    - Main search‑based amp‑comp loop: interpolated initial guess per note, sign‑change detection with neighbour probing, ±1/±2 stepping clamped to per‑note bounds, iteration/time/timeout guards.
  - `find_highest_freq()` / `find_lowest_freq()`:
    - Bisection search at full RANGE PWM (driven via `calibrationFreqHz` → `voice_task_autotune(4, …)`) and quadratic extrapolation to PWM 0, used when the table hits the top of the PWM range.
  - `amp0_search_band()` / `amp0_prescan()` / `measure_lowest_freq_at_amp0()` / `apply_measured_lowest_freq()`:
    - The shared amp-comp-0 endpoint used by all three amp paths: a wide band under the first measured pair (`kAmp0BandRatio`, floored at `kAmp0MinFreqHz`), a scan across it for two readings that bracket 50% duty, a bounded frequency search between them with the amp comp fixed at 0, and acceptance only within `kEndpointAcceptDutyPct` — otherwise the extrapolation stands. `apply_measured_lowest_freq()` is the classic wrapper that writes `calibrationData[0..1]`; `FREQ_TRACE` and the fine pass call the measurement directly.
  - `calibrate_DCO_freq_trace()`:
    - `FREQ_TRACE` table builder: two manual points (440 Hz anchor + trim note) and a bootstrap cluster feed a curve model, the ladder spacing is derived from it, rungs are traced with fixed-amp frequency bisection, and the full-amp / amp-comp-0 endpoints are measured last.
  - Interpolation helpers (`quadraticInterpolation`, `logarithmicInterpolation`, `linearInterpolation`, `expInterpolationSolveY()`):
    - Derive good amplitude starting points between measured calibration anchors.

---

## 6. Storage & State Persistence (LittleFS)

- **`FS.h` / `FS.ino` — one-line shims over the shared library.** The code is
  `_shared/FS.h` (sizes, buffers, prototypes) and `_shared/FS_impl.h`
  (definitions), both consumed by DCO3 and DCO4. Format, sizing rules and the
  invariants that keep existing calibration readable:
  [`../_shared/docs/CALIBRATION_STORAGE.md`](../_shared/docs/CALIBRATION_STORAGE.md).
  - Seven flat little-endian LittleFS banks, no header or version byte, index =
    oscillator (or PW channel). On this board: `voiceTables` 528 B (3 osc × 22
    `[freq_x100:u32][range_pwm:u32]` pairs), `PWCenter` / `PWHighLimit` /
    `PWLowLimit` 6 B each (`NUM_PW_CHANNELS` = 3), `ManualOffset` 3 B (`i8`/osc),
    `AmpComp440` 6 B (`u16`/osc), `AmpCompDutyOffset` 6 B (`i16`/osc, 0.01 %).
  - `init_FS()` — the only reader. Mounts LittleFS, creates any missing bank,
    reads the leading `FS*BankSize` bytes (never the file's real on-disk length)
    and unpacks into `ampCompArray` + `ampCompFrequencyHz` (float) or
    `ampCompFrequencyArray` (fixed, Q8 at precompute), `PW_CENTER` /
    `PW_LOW_LIMIT` / `PW_HIGH_LIMIT`, `manualCalibrationOffset`, `ampComp440`,
    `ampCompDutyOffset`. Idempotent; every write path ends by calling it.
  - `update_FS_voice()` — seeks and rewrites one oscillator's 176 B slice from
    `calibrationData[]`.
  - `update_FS_PWCenter()` / `_PW_High_Limit()` / `_PW_Low_Limit()` — one `u16`
    at a **PW channel** index (`cal_pw_channel(osc)`), bounds-checked. Opened
    `"r+"`, so the bank must already exist — `init_FS()` guarantees that.
  - `update_FS_ManualCalibrationOffset()` / `_AmpComp440()` /
    `_AmpCompDutyOffset()` — the per-oscillator manual trims, from
    `apply_param_manual_calibration_store()`.
  - `write_fs_bank()` — truncate/create a file and write a full bank (shared with
    bulk restore; also how the fake seed repairs a wrong-sized leftover file).
  - `seed_fake_calibration_tables(force)` — plausible amp-comp curve + PW
    defaults (`kPwCenterDefault` in `globals.h`) + `AmpComp440` = 1400 so a
    virgin board boots and plays. Boot calls it with `false` (no-op once
    `voiceTables` exists); debug command 30 forces it.
  - **Careful:** bank sizes are compile-time constants that `preset_bulk_commit()`,
    `dump_fs_file()` and the host `DCO-CONTROL-PANEL` model all derive
    independently. Changing one silently breaks stored calibration. The DCO4-only
    PW bank repair (`ensure_pw_fs_banks()`) is compiled out here by
    `#if PROJECT_INSTRUMENT == 4` and must stay that way.

- **`preset_store.h` / `preset_store.ino`** — MCU **256-slot patch store** + host dump/restore. Deep doc: [`PRESET_STORE.md`](PRESET_STORE.md).
  - Fixed **598-byte** records packed 4-per-file in `pb00`…`pb63` (512 KB FS); `pstLast` for boot/MIDI recall.
  - Live shadow: `update_parameters()` → `preset_shadow_capture()` for every persistable ParamId; blocks read from globals at save.
  - Host control: `'p'` ParamIds 170–173 (save/load/dump/cal-dump), `'B'`/`'C'` bulk chunk/commit, `'q'` 16-char name before save.
  - Board → host: structured CDC text (`[dump]` / `[pdir]` / `[preset]` / `[bulk]`).
  - Recall: MIDI Program Change, `PARAM_PRESET_LOAD`, and `preset_store_boot_task()` (~1.5 s after boot).
  - Needs a LittleFS flash partition large enough for ~76 KB of preset files + cal banks.
  - **Input has no LittleFS preset storage of its own** — the DCO's 256 slots are the
    single source of truth system-wide. `preset_store_send_directory_to_mb()` answers
    Input's `'N'` directory request with 128 `'O'` frames (`[slot][name:16]`);
    `serial_send_preset_loaded_to_mb()` sends `'L'` `[slot]` at the end of every
    `preset_store_load()` so Input's Screen display tracks the DCO's actual current slot
    regardless of who triggered the load.

---

## 7. MIDI, Serial Protocols and Parameter Updates

- **`midi.h` / `midi.ino`**  
  - Uses Adafruit TinyUSB MIDI and `MIDI.h` to expose both USB and DIN MIDI inputs:
    - `Adafruit_USBD_MIDI usb_midi` + `MIDI_USB` instance.
    - `MIDI_SERIAL` instance bound to `Serial1`.
  - `init_midi()` registers handlers for note on/off, CC, program change and pitch bend for both ports.
  - Handlers:
    - `handleNoteOn()` / `handleNoteOff()` forward events to the internal `note_on()` / `note_off()` functions (voice allocator).
    - `handleControlChange()` uses CC 42 to adjust pitch‑bend range and recompute `pitchBendMultiplier_q24`, then passes every other controller to `midi_cc_handle()`.
    - `handleProgramChange()` → `preset_store_load(midiPresetBank*128 + program)` (CC 0/32 bank select for 128..255).
    - `handlePitchBend()` updates `midi_pitch_bend` in globals.
  - **MIDI CC control surface** (`midi_cc.h` + the generated `midi_cc_map.h`, chart in `docs/MIDI_CC_MAP.md`):
    - `midi_cc_handle()` finds the controller in `midiCcMap[]`, scales it as `lo + ((hi - lo) * cc + 63) / 127`, and runs envelope attack/decay/release through `linearToExponential(v, 50, 25000)` so a CC lands in the same exp domain the `'a'`-`'c'` block frames carry.
    - `midi_cc_apply()` dispatches: targets at or above `CC_LOCAL_FIRST` (224) are the 1 ms ADSR/filter block values (`'a'`–`'d'`) that have no `ParamId`, so they are written to their globals here exactly as `input_handle_*()` writes them and the touched block is then mirrored to Input (the codes are grouped per block, so one range test after the switch picks the sender); PW (`PARAM_PW_VALUE`) and EnvVCA→VCA (`PARAM_ADSR1_TO_VCA`) and everything else go to `update_parameters()`.
    - The map, the chart and the Open Stage Control session in `tools/panels/` are all generated from `tools/dco_control/params.py` by `gen_midi_map.py`, which also verifies that each mapped `ParamId` is routed by `paramTable[]` and each `CC_LOCAL_*` has a case in `midi_cc_apply()`.
  - `note_on()` / `note_off()`:
    - Voice allocation by `voiceMode`, with the policy from `voiceAlloc.mode()` (`PARAM_VOICE_ALLOC_MODE`). Note edges stay on the board (`noteStart[]` / `noteEnd[]` → EnvDCO/EnvVCA/EnvVCF on Core1); nothing is sent over serial for notes.
    - **Shared implementation**: the allocator and the mono stack live in `DCO-SHARED-LIBRARIES/voice_alloc.h`, reached through the `_shared` symlink and wrapped by `voice_alloc_state.h`, which sets `VOICE_ALLOC_SRAM_HOT 1` and declares `voiceAlloc` (`VoiceAllocator<NUM_VOICES_TOTAL>`) and `monoStack` (`MonoNoteStack<8>`). DCO4-REBORN compiles the same header. `voices.ino` keeps only three adapters — `voice_alloc()`, `voice_mark_on()`, `voice_mark_off()` — which mirror the sketch's gate flag, pitch table and ADSR edge flags into the allocator. This replaced `get_free_voice()` / `get_free_voice_sequential()` and the `polyMode` global that chose between them.
    - **Allocation state** (owned by `voiceAlloc`, Core0-only writer): each slot is `VOICE_IDLE` / `VOICE_HELD` / `VOICE_RELEASING`, with a trigger stamp for age and a release stamp for the tail. `VOICES[]` and `VOICE_NOTES[]` stay in `globals.h` because `voice_task` and autotune read them; `VOICES[]` is just the gate flag, no longer a `millis()` stamp. `alloc()` derives `RELEASING → IDLE` itself from `ADSR_VCA_Level_q15[]` (registered by `init_voices()`) rather than letting Core1 write the state, so there is no window where Core1 frees a slot Core0 just took.
    - **The allocation mode** is one setting doing two jobs — para steal policy and mono note priority: `0` round-robin / last, `1` oldest / first, `2` quietest / last, `3` quietest keep-lowest / low, `4` quietest keep-highest / high, `5` no stealing / first with denial. Every para mode prefers an idle voice, then the release tails, and only steals a held note last; `5` drops the note-on instead.
    - **Mono (`voiceMode == 0`) — held stack** (Core0 MIDI path only, `monoStack`, depth 8):
      - NoteOn: `monoStack.push()` re-strikes to the top (dropping the oldest when full) and returns `false` when mode `5` denies the key. `monoStack.pick()` then picks the sounding pitch per the mode; if it is unchanged the new key is held silently (first/low/high priority), otherwise `voice_mark_on(0, …)` sets `VOICE_NOTES[0]`, gate `VOICES[0]`, `note_on_flag[0]` and `noteStart[0]`.
      - NoteOff: `monoStack.remove()` (ignore if not held). Stack empty → `voice_mark_off(0)` (keep `VOICE_NOTES` for release pitch). Otherwise re-pick; a changed winner calls `voiceAlloc.regate(0, …)` and pulses `note_on_flag[0]` for porta, and **does not** set `noteStart` (envelopes continue).
      - Simultaneous / overlapping keys: which one sounds is the priority above; releasing it while others remain held glides to the new winner via the existing voice_task porta edge (`note_on_flag` → `note_on_flag_flag` → restart from current glide pitch to `VOICE_NOTES`). Porta TIME/SLEW math is unchanged.
      - `mono_note_stack_clear()` (`midi.ino`, clears `monoStack` and `mono_sounding_note`) runs from `setVoiceMode()` when entering mono so held notes do not leak across mode switches.
    - **Para (`voiceMode == 1`)**: `voiceAlloc.findNote()` first (reuse the voice already on that pitch, including one in release), then `voice_alloc()`. **Stack stub (`voiceMode == 2`)**: same note on every slot, nothing to allocate.

- **`Serial.h` / `Serial.ino`**  
  - Configures UARTs:
    - `Serial1`: MIDI DIN input — RX **1** / TX **0** @ 31.25 kbps.
    - `Serial2`: high‑speed link to the Input board — RX **21** / TX **20** @ ~2.5 Mbps. This is the DCO's only peer link; the Screen is reached by Input relaying gap 154. It pairs with the Input's `Serial1`: RX 21 is driven by the Input's TX (GP0), and TX 20 drives the Input's RX (GP1). The Input talks to the Screen on its `Serial2` TX (GP4); that port's RX (GP5) is unwired.
    - `Serial`: USB CDC debug console.
  - Implements a **non‑blocking inner-frame parser** for Serial2 / USB (`serial_parser.h` + `serial_frame.h`), speaking the slim panel protocol (`serial_input_protocol.h`):
    - Commands (LE, no finish byte; `0x00` reserved as COBS delimiter):
      - `'a'` / `'b'` / `'c'` – 4×16‑bit ADSR blocks → EnvVCA / EnvVCF / EnvDCO (`ADSR1_*`) times.
      - `'d'` – filter block → `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF`, then `cv_bake_adsr2_to_vcf_scale()` + `cv_bake_lfo2_to_vcf_scale()`. Depth bake / peak math: [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).
      - `'p'` – ParamId + int16 LE → `update_parameters()` (includes PW 210, EnvVCA→VCA 222, EnvDCO pitch mode 223, preset/cal cmds 170–173). USB/`dco_control` and MIDI also echo persistable `'p'` back to Input; panel Serial2 ingress never echoes (loop prevention).
      - `'q'` – 16‑char preset name → `presetName[]`.
      - `'B'` / `'C'` – bulk restore chunk (36 B payload) / commit (8 B) for MCU presets and cal tables ([`PRESET_STORE.md`](PRESET_STORE.md)).
      - `'N'` – Input→DCO only: directory request (1 unused/pad byte — a true 0-byte
        payload can't dispatch in this parser, see [`PRESET_STORE.md`](PRESET_STORE.md)).
        Answered with 128 `'O'` frames (`[slot][name:16]`) via `preset_store_send_directory_to_mb()`.
      - `'L'` – DCO→Input only: `[slot]`, sent once at the end of every successful preset load.
    - O(1) command LUT; 500 µs timeout only when mid-frame and the stream is idle; drain budget 64.
    - On-wire default is RAW (= inner). `#define SERIAL_FRAMING_COBS` wraps the same inner payloads as `COBS(inner)+0x00`; host: `dco_control --cobs` / `DCO_SERIAL_COBS=1`. Must match Input/Screen. Buffer codec in `serial_frame.h` is reusable for SPI later. DCO sets `SERIAL_INNER_MAX_PAYLOAD` to 36 for `'B'`.
  - Outgoing helper:
    - `serialSendParam32()` – slim `'x'` via `serial_frame_write` (gap 154, cal offsets 155) out on Serial2 TX 20, received by the Input on its `Serial1`. Payload 5 = `[id][u32 LE]`. Drops if `availableForWrite() < 1`.
    - `serialSendParam16()` / `serial_echo_persistable_param16()` – slim `'p'` `[id][i16 LE]` for persistable USB/MIDI applies (`preset_param_is_persistable`). Input stores locals only (ADSR3→PWM wire − 512) and forwards the same `'p'` to Screen toasts.
    - `serial_send_adsr_*_block_to_mb()` / `serial_send_filter_block_to_mb()` – mirror block globals to Input after a preset load or a block MIDI CC. A USB-origin `'a'`–`'d'` is mirrored by `serial_forward_input_block_to_mb()` from inside the handler instead, re-sending the frame it just parsed. Input inverts the ADSR times back to fader units, forwards `'a'`/`'b'` to the Screen, and sends the filter fields as UI ids 191-194.
  - `serial_panel_task()` / `serial_usb_task()` are the parser pumps, called from `loop()` on `timer1msFlag`. USB/DIN MIDI `.read()` still runs every iteration (`turnThruOff`).
  - Shared headers: `serial_input_protocol.h`, `serial_frame.h`, `serial_param_protocol.h`, `serial_parser.h`. How-to: [`README_serial_and_params.md`](README_serial_and_params.md).

- **`params_def.h` / `param_router.h` / `params.ino`**  
  - Canonical `ParamId` enum and table‑driven router.
  - Central **parameter apply** (`init_param_router()` + O(1) `update_parameters(uint16_t, int16_t)`) for UI/MIDI‑driven changes; every call also `preset_shadow_capture()` for persistable ids:
    - Oscillator configuration (wave on/off, intervals, OSC2 detune, sync modes).
    - LFO settings (waveforms, speeds, routing depths, drift spread/speed).
    - Voice/stack mode, unison detune, analog drift amount.
    - Portamento time and mode (time‑based vs slew‑rate) – updates `portamento_time` and `portamento_mode`.
    - ADSR mods (ADSR1→detune, ADSR1→PWM) with precomputed fixed‑point scales (`ADSR1toDETUNE1_scale_q24`). EnvDCO pitch tap: `PARAM_ADSR3_PITCH_MODE` 223 unipolar/centered — [`LFO.md`](LFO.md).
    - Calibration control flags (`calibrationFlag`, `manualCalibrationFlag`, stages, offsets).
    - Preset store / dump (`PARAM_PRESET_SAVE` 170, `_LOAD` 171, `_DUMP` 172, `PARAM_CAL_DUMP` 173) — DCO-local, not on the MIDI CC map.
    - **Character** (`PARAM_CHARACTER` 221): master scale for noise-driven imperfection; diagnostic axes on `PARAM_DEBUG_COMMAND` 0xC8–0xCB. Deep doc: [`CHARACTER.md`](CHARACTER.md).
  - Converts raw UI values into:
    - Exponential or logarithmic curves using `expConverter*()` helpers.
    - Fixed‑point Q24 modulation depths (`LFO1toDCO_q24`, `LFO2toOSC2_q24`, `LFO2toOSC3_q24`, `LFO2toOSC2/3_coarse_q24`).
    - Per‑oscillator drift speed offsets and LFO frequency updates.

---

## 8. Timing, Utility Functions and Note Tables

- **`Timer_millis.h` / `Timer_millis.ino`**  
  - Provides a set of millisecond and microsecond timers:
    - 99 µs, 223 µs, ~1 ms, 200 ms, 1000 ms (others commented out).
  - `millisTimer()`:
    - Resets and updates flag variables (`timer99microsFlag`, `timer200msFlag`, `timer1000msFlag`, etc.) used by:
      - `loop1()` (core 1) to rate‑limit ADSR updates.
      - The voice task to schedule PW updates.
  - The profiler no longer hangs off `timer1000msFlag`: its periodic dump is timed on core 0 in `bench_poll_core0()`, so nothing is printed from the audio core.

- **`utils.h` / `utils.ino`**  
  - Small helper utilities:
    - `uintToStr()` – integer to C‑string conversion.
    - Mapping helpers from linear to logarithmic/exponential parameter curves (`linearToLogarithmic`, `linearToExponential` — the Input board's fader curve, used by the MIDI CC layer — and `expConverter*`).
    - `controls_formula_update()` – converts raw control values into float parameters (e.g. LFO speeds, LFO1→DCO depth).
    - `led_blinking_task()` – simple LED heartbeat and activity feedback using `LED_BLINK_START`.

- **`noteList.h`**  
  - Static tables for MIDI note → frequency mapping:
    - Individual note defines (e.g. `NOTE_A4 = 440.00`) and a contiguous `sNotePitches[]` float array (float engine / helpers).
    - `sNotePitches_q24[]` – 64‑bit **Q24 fixed‑point** version of the same table (fixed engine).

---

## 9. USB, System Config and Metadata

- **`tusb_config.h`**  
  - TinyUSB configuration for the RP2040 USB stack (endpoints, buffer sizes, etc.), shared with Adafruit TinyUSB.

- **`usb_descriptors.c`**  
  - USB MIDI device descriptors (much of the file is commented / legacy).  
  - Product identity is also set from `setup()` (USB product **DCO3-MONO** via TinyUSB APIs).

- **`irq_tuner.*`**  
  - Experimental IRQ tuner — excised to `_removed/`; not in the live build.

---

## 10. External Libraries

**Vendored in `_build_libs/`** (via `--libraries ./_build_libs`):

- **`ADSR_Bezier`** — symlink to monorepo-root repo, branch **`main`**; used by `adsr.*` (see section 4).
- **`mo-lfo`** — vendored from repo-root `mo-lfo/` (always dual `getWave`/`getWaveQ15`); used by `LFO.*` (see section 4).
- **`MIDI_Library`**, **`PID_v1`** — vendored copies.

**Other dependencies** (Arduino core / sketchbook, not under `_build_libs`):

- **Adafruit TinyUSB**, **LittleFS** (RP2040 core).

---

## 11. Conventions

- `*.h` – Declarations, constants, global state and struct/class definitions.  
- `*.ino` – Implementation files with function bodies and logic.  
- Engine / IO / ADSR / LFO flags – catalog [`BUILD_FLAGS.md`](BUILD_FLAGS.md); math depth [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) (`DCO.ino` pitch ids / board defaults / overrides).  
- Shared serial/param headers – keep `ParamId` numbers stable across boards; see [`README_serial_and_params.md`](README_serial_and_params.md).

---

### Summary

This firmware implements a **dual‑core 1-voice × 3-osc DCO monosynth** (RP2040 / RP2350-class) with:
- A compile-time **float or fixed-point** voice engine (**fixed is the shipping default** on both MCUs) and matching amplitude-compensation paths — see [`BUILD_FLAGS.md`](BUILD_FLAGS.md) / [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).
- A table‑driven pitch path (portamento, LFOs, drift, ADSR, OSC2/OSC3 interval+detune) feeding PIO clock dividers and RANGE/PW PWM.
- Robust DCO and PW calibration via edge‑timing (and optional PID), persisted in LittleFS.
- 256-slot MCU presets (LittleFS chunks) with MIDI Bank Select + Program Change, boot recall, and USB dump/restore for patches and cal tables.
- MIDI over USB and DIN, plus a high‑speed UART protocol to a main controller for parameters and UI.
- Clean separation between the hot voice/control loops and slower calibration, storage and UI-facing code.

Use this reference to quickly locate subsystems, understand data flow, and safely extend or optimize specific parts of the DCO4 firmware.
