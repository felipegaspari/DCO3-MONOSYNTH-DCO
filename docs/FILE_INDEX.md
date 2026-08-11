# DCO File Index

Purpose of **every file**, and for each source function: **what it does**, **who calls it**, and **when**.

> `params_def.h`, `param_router.h`, `serial_input_protocol.h`,
> `serial_param_protocol.h`, `serial_frame.h` and `serial_parser.h` are no longer
> files in this folder. They come from the shared
> [`DCO-PROTOCOL`](../../DCO-PROTOCOL/README.md) library, symlinked in as
> `_build_libs/DCO-PROTOCOL`. Their entries below still describe the code this
> board compiles; edit them in the library, once, for every board.

- Deep narrative: [`REFERENCE_AI.md`](REFERENCE_AI.md)
- Build flags catalog: [`BUILD_FLAGS.md`](BUILD_FLAGS.md)
- Engine float/fixed math: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- Hot-path profiling: [`BENCHMARKING.md`](BENCHMARKING.md)
- SRAM / heap / stack: [`MEMORY.md`](MEMORY.md)
- Character / noise jitter: [`CHARACTER.md`](CHARACTER.md)
- MCU presets / cal dump: [`PRESET_STORE.md`](PRESET_STORE.md)
- CV mod depth scales: [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md)
- LFO Q15 bus / pitch depth scales: [`LFO.md`](LFO.md)
- Repo entry / doc index: [`../README.md`](../README.md)

Headers with no bodies are marked **no function definitions**.  
**Dead** = no live callers. **Unreachable** = call site exists but cannot run as currently gated. **`#if 0`** = compiled out.

---

## Call-flow overview

```mermaid
flowchart TD
  fw0["Arduino Core0"] --> setup0["setup()"]
  fw0 --> loop0["loop()"]
  fw1["Arduino Core1"] --> setup1["setup1()"]
  fw1 --> loop1["loop1()"]

  setup0 --> initSerial["init_serial / init_param_router / init_midi / init_LFOs"]
  setup1 --> initCore1["seed_fake_if_missing / init_FS / init_ADSR / init_pwm / init_pio / init_range_pio_dither / init_voices"]

  loop0 --> midiRead["MIDI_*.read every loop → handle* → note_on/off / PC→preset_load"]
  loop0 --> serialTask["serial_panel/USB on timer1msFlag"]
  loop0 --> bootPreset["preset_store_boot_task once ~1.5s"]
  loop0 --> lfo["LFO1/LFO2 ~50µs; drift ~51µs"]
  lfo --> fifo["FIFO Q24 detune → Core1"]

  serialTask --> updParam["update_parameters → shadow + apply_param_*"]
  updParam -->|calibrationFlag| calBranch
  serialTask --> bulk["'B'/'C' → preset_bulk_*"]
  bootPreset --> pstLoad["preset_store_load"]

  loop1 --> millis["microsTimer2()"]
  loop1 --> calBranch{"calibrationFlag?"}
  calBranch -->|auto| dcoCal["DCO_calibration() blocking"]
  calBranch -->|manual| manCal["voice_task_autotune + DCO_calibration_debug"]
  calBranch -->|play| play["ADSR_update ~10kHz + voice_task_main"]
  fifo --> play
```

| Context tag | Meaning |
|-------------|---------|
| Framework | Arduino invokes `setup` / `loop` / `setup1` / `loop1` |
| Boot Core0 / Core1 | Inside `setup` / `setup1` |
| Every `loop` / `loop1` | Realtime forever loops |
| MIDI callback | Dispatched from `MIDI_*.read()` in `loop` |
| Serial2 | Parser command on the Input link (DCO `Serial2`) |
| Param table | Only via `paramTable[]` / `param_router_apply` |
| Auto-cal / Manual-cal | `loop1` calibration branches |
| Hot path | Inside `voice_task_fixed_point` / `voice_task_float` |

---

## 1. Entry / build / globals

### `DCO.ino`

Main sketch: dual-core setup/loops, USB init (product DCO3-MONO), engine flags (**pitch ids** / **board defaults** / **overrides** / **guards** at top). Monosynth: 1 voice × 3 oscillators.

**Functions**
- `setup()` — Core 0 init: serial, MIDI, LFOs, pins, USB strings, cal pin.
  - **Called from:** Arduino framework (Core 0).
  - **When:** Boot once.
- `setup1()` — Core 1 init: `seed_fake_calibration_tables(false)` if `voiceTables` missing, FS, ADSR, `mod_matrix_init`, amp-comp precompute, PWM, PIO, optional `init_range_pio_dither`, voices; clears cal flags.
  - **Called from:** Arduino framework (Core 1).
  - **When:** Boot once.
- `loop()` — Core 0: MIDI USB+DIN every iteration; Serial2 + USB CDC pumps on `timer1msFlag`; `preset_store_boot_task()` (one-shot ~1.5 s); LFO1/LFO2 ~50 µs; drift ~51 µs; `bench_poll_core0` + `mem_diag_poll_core0`. `__not_in_flash_func`.
  - **Called from:** Arduino framework (Core 0).
  - **When:** Forever.
- `loop1()` — Core 1: `microsTimer2`; noise fleet; auto/manual cal **or** ADSR + FIFO pop + `voice_task_main`; ends with `bench_service(1)` + `mem_diag_poll_core1`. `__not_in_flash_func`.
  - **Called from:** Arduino framework (Core 1).
  - **When:** Forever.

**Key macros:** full catalog in [`BUILD_FLAGS.md`](BUILD_FLAGS.md). Engine: `USE_FLOAT_VOICE_TASK`, `USE_FLOAT_AMP_COMP`, `USE_FLOAT_CV_OUTS`, `PITCH_INTERP_MODE` (`FLOAT` / `FLOAT_FAST` / `RATIO_Q16` / `Q12`), `CLKDIV_MODE` (`GOLD` / `FLOAT` / `Q16` / `Q8` / `FAST_Q4`), `AMP_COMP_METHOD_DEFAULT`. Noise: `NOISE_ENGINE`, `ENABLE_NOISE_OUT`. Profiler: `RUNNING_AVERAGE`, `RUNNING_AVERAGE_FINE`, `RUNNING_AVERAGE_PERIOD`, `BENCH_STAGE_STRIDE`, `BENCH_USE_SYSTICK`, `BENCH_PERIOD_MAX_US`, `BENCH_PATH_STATS`, `ENABLE_MEM_DIAG`. Board/IO: `ENABLE_USB_CONTROL`, `ENABLE_CV_OUTS`, `ENABLE_WAVE_MUX`, `ENABLE_VOICE_AUX`, `ENABLE_PIO_RESET_INVERT`, `RANGE0_PIO_DITHER_TEST`, `NOTE_RETRIG_MODE_DEFAULT`.

### `bench.h`

Hot-path profiler. All storage, ids, nesting and labels are generated from the single
`BENCH_PROBES` X-macro table, so a probe cannot exist at a call site and be missing from the
report. Time source is SysTick read as a 24-bit cycle counter (RP2040's M0+ has no DWT).
`BENCH_PERIOD()` uses the same SysTick clock (`BENCH_USE_SYSTICK`); samples longer than
`BENCH_PERIOD_MAX_US` are discarded. The 1 µs timer is the dump-window gate, or all probes
when `BENCH_USE_SYSTICK` is `0`. Compiles to nothing without `RUNNING_AVERAGE`. Core0 IO is
split into `MIDI read` vs `serial panel/USB`. Note-on: `retrig period split` under
voice_task_fixed_point; under note-on, `retrig SM apply` + `retrig RANGE PWM` (do not slice
SM apply — cold XIP mis-ranks kids).

**Subsystem reference:** [`BENCHMARKING.md`](BENCHMARKING.md).

**Functions**
- `bench_init_core()` — Arm this core's SysTick and calibrate the probe overhead.
  - **Called from:** `setup()` and `setup1()`.
  - **When:** Boot, once per core (SysTick is core-local).
- `bench_now()` / `bench_span()` / `bench_us_now()` — Raw counter reads and wrap-safe deltas.
  - **Called from:** `BENCH_*` macros; one-shot benches.
- `bench_service()` — Snapshot and clear this core's probes on request.
  - **Called from:** `bench_poll_core0()` for core 0; `loop1()` for core 1.
- `bench_poll_core0()` — Drive the handshake and print the report; calls `print_amp_comp_bench()`, `print_pitch_interp_bench()`, `print_clkdiv_hp_bench()`.
  - **Called from:** `loop()`.
  - **When:** Every iteration; prints only once both cores have answered a dump request.
- `bench_reset_all()` — Clear every accumulator.
  - **Called from:** `apply_param_debug_command()` value 11.
- `bench_print_report()` / `bench_print_core()` / `bench_print_row()` / `bench_print_unattributed()` — Format the budget table.
  - **Called from:** `bench_poll_core0()`.

### `mem_diag.h`

SRAM/heap/stack dump (cmd **13**). `ENABLE_MEM_DIAG` off → empty `static inline` polls/request. Flag on → inline idle check (`mem_diag_runtime_enabled` then pending) then `.ino` work. See [`MEMORY.md`](MEMORY.md).

**Functions** (inlines when flag on/off)
- `mem_diag_poll_core1()` / `mem_diag_poll_core0()` — Idle early-out; call `_work` only when a dump is pending and polls are runtime-enabled.
  - **Called from:** `loop1()` / `loop()`.
  - **When:** Every iteration; compiles out without `ENABLE_MEM_DIAG`.

### `mem_diag.ino`

Heap + per-core stack snapshot (`ENABLE_MEM_DIAG` only). Works without `RUNNING_AVERAGE`. Core 1 never prints.

**Functions**
- `mem_diag_request()` — Set pending flag for a dump (no-op if runtime polls off).
  - **Called from:** `apply_param_debug_command()` value **13**.
  - **When:** Diagnostics button / USB param.
- `mem_diag_poll_core1_work()` — Snapshot `rp2040.getFreeStack()` on Core 1.
  - **Called from:** inline `mem_diag_poll_core1()` when pending.
- `mem_diag_poll_core0_work()` — Print heap / stacks / layout over USB CDC.
  - **Called from:** inline `mem_diag_poll_core0()` when Core 1 has answered.

### `include_all.h`

Umbrella include (`mem_diag.h`, `preset_store.h`, …). **No function definitions.**

### `globals.h`

Shared constants, pins, state, prototypes. Includes Character knobs / `char_*_scale_q15` (see [`CHARACTER.md`](CHARACTER.md)). **No function definitions** (inlines for period split live here).

### `character_jitter.h`

Character-tab noise jitter: precomputed scales, per-axis deltas, amp clamp. Deep doc: [`CHARACTER.md`](CHARACTER.md).

**Functions**
- `character_axis_scale()` — `(axis * character) >> 7` then scale by max delta.
  - **Called from:** `character_recompute_scales()`.
  - **When:** Param / diag path.
- `character_recompute_scales()` — Refresh `char_*_scale_q15` from knobs.
  - **Called from:** `apply_param_character`; `apply_param_debug_command` (0xC8 / 0xCA / 0xCB).
  - **When:** Character / jitter change.
- `character_pitch_delta_q24()` — `noiseLevel[1] * char_pitch_scale_q15 >> 15` (Q24).
  - **Called from:** `voice_task_fixed_point` / `voice_task_float` (gated).
  - **When:** Hot path, pitch jitter on.
- `character_amp_delta()` — `noiseLevel[0] * char_amp_scale_q15 >> 15` absolute PWM counts (caller gates scale ≠ 0).
  - **Called from:** RANGE PWM sites in both voice engines.
  - **When:** Hot path / note-on, amp jitter on.
- `character_clamp_amp()` — Saturate level to `0..DIV_COUNTER`.
  - **Called from:** same RANGE PWM sites after `chanLevel* + amp_j`.
  - **When:** Amp jitter on.
- `character_pw_delta()` — `noiseLevel[0] * char_pw_scale_q15 >> 15`.
  - **Called from:** PW calc in both voice engines.
  - **When:** ~10 kHz PW update.

### `tusb_config.h`

TinyUSB `#define`s. **No function definitions.**

### `usb_descriptors.c`

Legacy descriptors — fully commented. **No active function definitions.**

---

## 2. Voice / oscillator / PWM path

### `voices.h`

Declarations / portamento & table state. **No function definitions.** Carries no profiler
declarations: probe storage lives in the `bench.h` table instead, because the extern block
that used to be here drifted out of step with `voices.ino` and compiled anyway.

### `voices.ino`

Real-time voice engine (float/fixed), allocation, pitch tables, amp/PW helpers.

**Functions**
- `init_voices()` — Initial notes, tables, voice mode, first `voice_task_main()`.
  - **Called from:** `setup1()`.
  - **When:** Boot Core1.
- `noteQ16_to_freqQ24()` — Q16 semitone → Q24 Hz.
  - **Called from:** `voice_task_fixed_point()` (slew portamento).
  - **When:** Fixed hot path.
- `float_to_q24()` — Float → Q24.
  - **Called from:** **none (dead)** — defined but unused.
- `noteIndex_to_freqFloat()` — Float note index → Hz.
  - **Called from:** `voice_task_float()` (portamento).
  - **When:** Float hot path.
- `voice_task_main()` — Dispatch to float or fixed voice task.
  - **Called from:** `loop1()` (play path); `init_voices()` (boot kick).
  - **When:** Every play-path `loop1` iter + once at boot.
- `voice_task_fixed_point()` — Fixed-point hot path. `__not_in_flash_func`.
  - **Called from:** `voice_task_main()` when `!USE_FLOAT_VOICE_TASK`.
  - **When:** Fixed-engine builds only.
- `voice_task_float()` — Float hot path (RP2350 default). `__not_in_flash_func` (SRAM-pinned like fixed). Clkdiv via `clkdiv_live_hz_total_cycles` (`CLKDIV_MODE`).
  - **Called from:** `voice_task_main()` when `USE_FLOAT_VOICE_TASK`.
  - **When:** Every play-path `loop1` iter (float-engine builds).
- `voice_task_autotune()` — Drive one osc for calibration measurement (mode 4 reads `calibrationFreqHz`).
  - **Called from:** `loop1()` (manual cal); `measure_gap_for_amp` / PW & freq search helpers in `autotune_search.ino` / `autotune.ino`.
  - **When:** Manual-cal every `loop1`; nested during auto-cal measurements.
- `get_free_voice_sequential()` — Round-robin free voice.
  - **Called from:** `note_on()` when `polyMode == 1`.
  - **When:** MIDI note-on.
- `get_free_voice()` — Oldest/steal free voice.
  - **Called from:** `note_on()` when `polyMode == 0`.
  - **When:** MIDI note-on.
- `setVoiceMode()` — Apply `voiceMode` → `NUM_VOICES` / `STACK_VOICES`; on mono also `mono_note_stack_clear()`.
  - **Called from:** `init_voices()`; `apply_param_voice_mode()`.
  - **When:** Boot; Serial2 / MIDI voice-mode param.
- `setSyncMode()` — Rebuild sync topology via `assign_sm_mapping()` + `start_voice_sms()`; retrigger.
  - **Called from:** `apply_param_sync_mode()`, `apply_param_soft_sync()`.
  - **When:** Serial2 param.
- `get_chan_level_lookup_fast()` — Q8 Hz → range PWM (always compiled; live FIXED / fixed `voice_task_fixed_point`). `__not_in_flash_func`.
  - **Called from:** `amp_level_q24()` (FIXED); `get_chan_level_for_engine()` / method FIXED.
  - **When:** Fixed hot path; float-engine FIXED method / benches.
- `get_chan_level_float_quad()` — Float quadratic cached walk (Hz); live FLOAT_QUAD / LUT fill / accuracy gold. `__not_in_flash_func` (SRAM-pinned like fixed lookup).
  - **Called from:** method FLOAT_QUAD; LUT fill; speed/accuracy benches.
  - **When:** `USE_FLOAT_AMP_COMP`.
- `get_chan_level_lut()` — Dense 1 Hz LUT (nearest Hz index). `__not_in_flash_func` (SRAM-pinned like fixed lookup).
  - **Called from:** `get_chan_level_for_engine` when method LUT.
  - **When:** `USE_FLOAT_AMP_COMP` (speed A/B; live default = FIXED).
- `get_PW_level_interpolated()` — Map PW counter into calibrated limits/center.
  - **Called from:** `voice_task_fixed_point()` / `voice_task_float()` (99 µs PW update).
  - **When:** Hot path.
- `modifiers_q24_to_xQ16()` / `interpolate_live_ratio_q16()` — compile-time wrappers for fixed `vt_freq_scale_x` / `vt_ratio_interp` (`PITCH_INTERP_MODE` inside; not function pointers).
  - **Called from:** `voice_task_fixed_point()`.
- `interpolate_live_ratio_f()` — same for float `vt_ratio_interp` (FLOAT / FAST / RATIO / Q12 glue).
  - **Called from:** `voice_task_float()`.
- `interpolatePitchMultiplierIntQ16_cached()` — IntQ16 table interp (`PITCH_INTERP_Q12`). `__not_in_flash_func`.
  - **Called from:** `interpolate_live_ratio_q16` / `_f` when mode is Q12; pitch benches 28/29.
  - **When:** Alternate pitch modes (fixed voice or float-voice A/B).
- `interpolateRatioQ16_cached()` — Table → ratio Q16 (`slopeQ20`). `__not_in_flash_func`.
  - **Called from:** `interpolate_live_ratio_q16` / `_f` when `PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16`; pitch benches.
  - **When:** Default fixed pitch mode; optional float-voice A/B.
- `interpolateRatioFloat_cached()` — Natural modifier `[-1,3]` → float ratio; walk+bsearch find. `__not_in_flash_func`.
  - **Called from:** `interpolate_live_ratio_f` when `PITCH_INTERP_MODE == PITCH_INTERP_FLOAT`; pitch benches.
  - **When:** Float walk A/B (`USE_FLOAT_VOICE_TASK`).
- `interpolateRatioFloat_cached_fast()` — Same lerp; trunc+clamp±1 find (`noinline` + `__not_in_flash_func`).
  - **Called from:** `interpolate_live_ratio_f` when `PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST`; pitch benches.
  - **When:** RP2350 board-default float hot path.
- `initMultiplierTables()` — Build tables/slopes for the active `PITCH_INTERP_MODE` only (uses `expInterpolationSolveY`).
  - **Called from:** `init_voices()`.
  - **When:** Boot Core1.
- Clkdiv total-cycle helpers — see [`clkdiv.h`](../clkdiv.h) (also listed below).

### `noteList.h`

Note → frequency tables. **No function definitions.**

### `state_machines.h`

Prototypes plus the inline period model: `pio_period_split()` (exact split, remainder into
Y), `pio_clk_div_for_y()` (rounded, for a Y that must not move), `osc_ramp_weight()` /
`osc_period_overhead()` (per-program constants), and the jump-target helpers
`osc_restart_target()` / `osc_phase_hold_target()` / `osc_phase_hold_x()`.
`osc_ramp_entry_target()` is unused by live phase-align.

### `state_machines.ino`

PIO load, SM setup, sync topology and diagnostics. **All three oscillators live on pio0**
(SM0/1/2) so they can share a reset pin — see `pio_topology_report()`.

**Subsystem reference:** [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).

**Functions**
- `sync_slave_osc()` / `sync_master_osc()` — Resolve `syncMode` into slave/master indices.
  - **Called from:** `assign_sm_mapping()`, `start_voice_sms()`, `pio_topology_report()`.
- `assign_sm_mapping()` — Rewrite `VOICE_TO_SM` so the slave outranks-below its master.
  - **Called from:** `init_pio()`, `setSyncMode()`.
  - **When:** Boot; sync topology change.
- `init_pio()` — `pio_sm_claim` pio0 SM0–2 + pio1 noise (and classic sub SM if no engine2); load free + one soft-sync poll image into pio0; classic sub programs into pio1 **or** `subosc2_init()` on pio2.
  - **Called from:** `setup1()`.
  - **When:** Boot Core1.
- `ensure_soft_sync_program()` — Swap resident poll image (N=1/2/3) via remove/add.
  - **Called from:** `init_pio()`, `start_voice_sms()`.
- `start_voice_sms()` — Ensure poll image; per-osc program/pin/sideset selection, Y preload,
  RESET pad polarity (`ENABLE_PIO_RESET_INVERT`), same-cycle start via `pio_enable_sm_mask_in_sync()`.
  - **Called from:** `init_pio()`, `setSyncMode()`.
  - **When:** Boot; sync topology / soft-sync threshold change.
- `pio_reset_pin_apply_polarity()` — `static`; GPIO OUTOVER+INOVER invert (or clear) on one RESET pin.
  - **Called from:** `start_voice_sms()`.
- `osc_load_period_stopped()` / `_noclear` / `osc_load_periods_stopped_noclear()` — `static inline` in `state_machines.h`: push Y + clk_div to a
  **stopped** SM via direct TXF/instr MMIO (Y travels through the OSR, which also feeds the
  chunk reads). Clear variant for boot/topology; fused noclear for note-on EXACT_Y.
  - **Called from:** `start_voice_sms()`, `osc_set_reset_pulse()` (clear); both engine note-on
    EXACT_Y paths (fused noclear).
- `osc_phase_align_hold_stopped()` — `static inline` in `state_machines.h`: preload X, restore
  clk_div, `set pins, 0`, jmp `loop_final` (one-shot delay until OSC2's first flyback).
  - **Called from:** both engine note-on EXACT_Y paths when `phaseHoldX != 0`.
  - **When:** Mono note-on, `oscSync > 1`, EXACT_Y. SM already stopped after noclear load.
- `osc_set_reset_pulse()` — Change only Y, reusing `osc_last_clk_div[]`.
  - **Called from:** `apply_param_osc_sync_mode()`.
- `pio_topology_report()` — Print sync roles and assert every RESET pin reads back as PIO0.
  - **Called from:** Bench/diagnostic use.
- `pio_period_probe()` / `pio_solve_period_model()` — Bench helpers for confirming the
  period weight and overhead against a frequency counter.
- `set_subosc_divide()` — Classic: (re)configure pio1 SM0. Engine2: same divide on both subs via `subosc2_set_divide()`.
  - **Called from:** `init_pio()`, deferred `PARAM_SUBOSC_DIVIDE`.
- `pio_defer_request_subosc_divide()` / `_master` / `_logic_op` — Core-0 → core-1 queue (`ENABLE_SUBOSC_ENGINE2`).
  - **Called from:** `subosc_param_*()`.
  - **When:** Serviced in `pio_defer_service()` before `voice_task`.
- `init_pio()` also loads `noise_lfsr` at PIO1 origin 0 (SM1) before any other PIO1 programs
  and starts it via `noise_lfsr_init()` — white words for `noise.h`.

### `subosc.h` / `subosc.ino`

Two-sub engine on pio2 when `ENABLE_SUBOSC_ENGINE2`, plus the boolean combiner that is the
section's output ([`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9). RP2040 builds keep stub
`subosc_param_*` so `params.ino` needs no flags.

**Functions**
- `subosc2_init()` — Claim pio2 SM0/SM1 + SM3; load `subosc_seg` @ 8 + `subosc_logic` @ 0; claim DMA pairs.
  - **Called from:** `init_pio()`.
- `subosc2_set_divide()` / `subosc2_set_phase_deg()` / `subosc2_set_width()` — Per-sub shape.
  - **Called from:** deferred divide; param phase/width mailboxes.
- `subosc2_set_master()` — Point a sub at another oscillator's reset (restarts the SM).
  - **Called from:** `pio_defer_service()`.
- `subosc2_set_logic()` — Combiner table rewrite; starts SM3 the first time.
  - **Called from:** `pio_defer_service()`, `subosc2_init()`.
- `subosc2_update_periods()` — Recompute 3-word segment groups from each sub's master period; calls `mod_matrix_eval_subosc()`.
  - **Called from:** both voice engines each control frame (`vt_subosc`).
- `subosc2_report()` — Paced bench dump (opcode 4).
  - **Called from:** `apply_param_debug_command()` case 4.
- `subosc_param_divide/master/phase/width/logic_op()` — Core-0 ParamId entry points.
  - **Called from:** `params.ino` apply handlers.

### `pico-dco.pio`

PIO assembly source. **Not C functions.** Programs in use: `frequency_sync_4_jumps`
(free-running, weight 4), `frequency_sync_poll` / `_2` / `_3` (soft-sync slave, N=1/2/3
trailing `jmp pin` chunks, weights 5/6/7; one resident at a time), `noise_lfsr` (PIO1
origin 0), `subosc_logic` + `subosc_seg` (PIO2 when engine2), else `subosc_div2` /
`subosc_div4` (PIO1). Comment-only listing for RANGE dither (`range_pwm_dither`; real
encoding in `range_pwm_dither.pio.h`).

Annotated listings and the period model for each program: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).

### `pico-dco.pio.h`

PIO C wrappers. **Hand-maintained** — Arduino does not run `pioasm`, so new programs must be
assembled by hand and kept in step with `pico-dco.pio`. The encoding is documented inline
above `frequency_sync_poll`.

**Functions**
- `frequency_program_get_default_config()` — Default config for basic frequency program.
  - **Called from:** `init_sm_pin()`.
  - **When:** Only if `init_sm` path used (currently dead).
- `init_sm_pin()` — Init SM for basic frequency program.
  - **Called from:** `init_sm()`.
  - **When:** Dead path today.
- `frequency_sync_program_get_default_config()` — Default config for sync program.
  - **Called from:** `frequency_sync()`.
  - **When:** Only if that init used (**dead** — not called from project code).
- `frequency_sync()` — Init SM for older sync program.
  - **Called from:** **none (dead)** — production uses `frequency_sync_4_jumps`.
- `frequency_sync_4_jumps_program_get_default_config()` — Default config for production program.
  - **Called from:** `frequency_sync_4_jumps()`.
  - **When:** Boot SM init.
- `frequency_sync_4_jumps()` — Init SM for production 4-jump sync program.
  - **Called from:** `start_voice_sms()`.
  - **When:** Boot; sync topology change.
- `frequency_sync_poll_init()` — Init soft-sync slave SM (`chunks` selects poll-1/2/3 wrap).
  - **Called from:** `start_voice_sms()`.
  - **When:** Soft sync enabled.
- `frequency_pulse1_program_get_default_config()` — Default config for pulse1 program.
  - **Called from:** `frequency_pulse1_program_init()`.
  - **When:** Only if pulse1 init used.
- `frequency_pulse1_program_init()` — Init SM for pulse1 program.
  - **Called from:** **none (dead)** in this firmware.

### `range_pwm_dither.pio.h`

Hand-encoded 4-inst 1-cycle RANGE PWM (sideset). **Not C functions** besides
`range_pwm_dither_program_get_default_config()`. Loaded by `init_range_pio_dither()` onto
pio0 and pio1 when `RANGE0_PIO_DITHER_TEST`. Detail: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §4.4.

### `PWM.h`

`write_range_pwm()` is **inline** here (PIO dither or slice). Under `RANGE0_PIO_DITHER_TEST`:
prototypes `init_range_pio_dither`, `range_pio_set_level`. Constants `RANGE_PIO_PERIOD` (4666),
`RANGE_PIO_FRAMES` (3), `RANGE_PIO_LEVELS` (13998).

### `PWM.ino`

**Functions**
- `init_pwm()` — Configure range + PW PWM slices (`RANGE_PWM_SLICES` / `RANGE_PWM_CHANNELS`);
  under `RANGE0_PIO_DITHER_TEST` skips all RANGE slices (`0xFF`); under `ENABLE_CV_OUTS` calls `init_cv_pwm()`.
  - **Called from:** `setup1()`.
  - **When:** Boot Core1.
- `init_range_pio_dither()` — Claim RANGE SMs (pio1 SM2/3, pio0 SM3), load `range_pwm_dither`,
  start 3× data+ctrl DMA rings. Idempotent re-enable.
  - **Called from:** `setup1()` when `RANGE0_PIO_DITHER_TEST`.
  - **When:** Boot Core1, after `init_pio()`.
- `range_pio_set_level(osc, level)` — Scale `0..14000` → 3-frame packed DMA words.
  - **Called from:** `write_range_pwm()`; `disable_all_oscillators_and_range_pwm()`.
  - **When:** Live amp-comp / cal RANGE writes when flag on.
- `init_cv_pwm()` — Cutoff / reso / VCA / dist PWM; calls `init_level_pwm()`.
  - **Called from:** `init_pwm()` when `ENABLE_CV_OUTS`.
- `init_level_pwm()` / `write_level_pwm()` / `write_level_pwm_raw()` — OSC1/2/3 + Sub level PWM → mix VCAs.
  - **Called from:** `init_cv_pwm()`; `mod_matrix_apply_cv()`; manual cal.
- `write_cv_pwm()` / `write_cv_pwm_raw()` — Push soft VCF/VCA/per-filter reso/dist compares.

### `wave_mux.h` / `wave_mux.ino`

Per-osc Saw/Pulse/Tri via dual 74HC595 → DG411. Docs: [`WAVE_MUX.md`](WAVE_MUX.md).

**Functions**
- `init_waveSelector()` — GPIO + all bits off.
  - **Called from:** `setup1()`.
- `update_waveSelector()` — Rebuild 9 bits from `waveEnable[3][3]`.
  - **Called from:** wave-enable apply handlers; cal restore.
- `waveSelector_manual_calibration(stage)` — Solo OSC{stage} Saw.
  - **Called from:** `update_CV_outs_manual_calibration()`.

### `mod_matrix.h` / `mod_matrix.ino`

Sparse mod matrix (8 slots). Docs: [`MOD_MATRIX.md`](MOD_MATRIX.md).

**Functions**
- `mod_matrix_init()` — Clear slots / AT / random.
  - **Called from:** `setup1()`.
- `mod_matrix_set_source/dest/depth()` — Slot table from ParamIds 60–83.
  - **Called from:** `params.ino` apply handlers.
- `mod_matrix_on_note_on()` / `mod_matrix_set_aftertouch()` — Random S&H + AT source.
  - **Called from:** `note_on()` / MIDI AT callback.
- `mod_matrix_accumulate()` / `mod_matrix_apply_cv()` — Sum → level PWM + `RESONANCE_PWM[]` + Dist Drive / Dist Mix; cutoff sum in `update_CV_outs()`.
  - **Called from:** `update_CV_outs()`.
- `mod_matrix_eval_pitch_q24()` — Pitch dest only → Q24.
  - **Called from:** voice engines.
- `mod_matrix_eval_subosc()` — Sub phase / PW dests → `subosc_mod_phase` / `subosc_mod_pw` (applied to sub 2 only).
  - **Called from:** `subosc2_update_periods()` (not via `update_CV_outs`, so works without `ENABLE_CV_OUTS`).

### `cv_out.h` / `cv_out.ino` / `cv_state.h`

Soft VCA/VCF/reso CV math (~10 kHz). Depth bakers and peak math: [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md). Hot-path map: [`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md). `cv_q15_to_u12`: Q15→u12 via `CV_U12_SCALE=4096`; clamp with `CV_U12_MAX=4095`.

**Functions**
- `init_cv_out()` — AS2164 linearize table; `cv_update_mod_scales()`; fixed path sets `vcf_drift_scale_q15`.
  - **Called from:** Core1 boot (with soft CV init).
- `cv_bake_adsr2_to_vcf_scale()` / `cv_bake_lfo2_to_vcf_scale()` / `cv_bake_lfo1_to_vca_scale()` — Bake one panel depth → `*_scale(_q15)`.
  - **Called from:** MIDI depth CCs; Input `'d'` (ADSR2+LFO2); `PARAM_LFO1_TO_VCA`; `cv_update_mod_scales`.
- `cv_update_mod_scales()` — All three bakers (boot).
  - **Called from:** `init_cv_out()`.
- `update_CV_outs()` — Env + LFO scales + CUTOFF + drift + matrix → soft PWM levels.
  - **Called from:** `loop1` play path.
- `update_CV_outs_manual_calibration()` — Cal-branch CV writers.

### `amp_comp.h`

Amp-comp tables, LUT, method select, dual precompute/facade.

**Functions**
- `precomputeCoefficients()` — Fixed Q-window precompute (also under float engine for FIXED).
  - **Called from:** `precompute_amp_comp_for_engine()`.
  - **When:** Boot / after auto-cal reload.
- `precomputeCoefficients_float()` — Float Hz quadratic precompute.
  - **Called from:** `precompute_amp_comp_for_engine()` when `USE_FLOAT_AMP_COMP`.
  - **When:** Boot / after auto-cal reload.
- `fill_amp_comp_lut_from_quad()` — Fill `ampCompLut[osc][0..7000]` from float quad.
  - **Called from:** `precompute_amp_comp_for_engine()` when `USE_FLOAT_AMP_COMP`.
  - **When:** Boot / after auto-cal reload.
- `precompute_amp_comp_for_engine()` — Float+LUT+fixed dual build, or fixed-only.
  - **Called from:** `setup1()`; end of `DCO_calibration()`; end of `seed_fake_calibration_tables()`.
  - **When:** Boot Core1; end of auto-cal; after fake seed.
- `amp_comp_set_method()` / `get_chan_level_for_engine()` — Runtime method + Hz → PWM facade.
  - **Called from:** debug cmds 20–22; `voice_task_float()`, `voice_task_autotune()`.
  - **When:** Float play path / cal / A/B.

### `amp_comp_bench.ino`

Amp-comp speed/accuracy one-shots (`AMP_COMP_BENCHMARK` + `RUNNING_AVERAGE`).

**Functions**
- `amp_comp_bench_run_speed()` / `amp_comp_bench_run_accuracy()` / `print_amp_comp_bench()`
  - **Called from:** `bench_poll_core0()` when debug 24/25 pending.
  - **When:** Diagnostics; paced `bench_out_*` TX.

### `pitch_interp_bench.ino`

Pitch-interpolator speed/accuracy one-shots (`RUNNING_AVERAGE`). Self-contained private tables + interpolators. Speed rows: FLOAT / FLOAT_FAST / RATIO_Q16 / Q12; accuracy keeps a private Q20 `y` reference (not a live mode).

**Functions**
- `pitch_interp_bench_run_speed()` / `pitch_interp_bench_run_accuracy()` / `print_pitch_interp_bench()`
  - **Called from:** `bench_poll_core0()` when debug 28/29 pending.
  - **When:** Diagnostics; paced `bench_out_*` TX.

### `clkdiv.h`

Live total-cycle helpers (`static inline`), shared by `voice_task_fixed_point` / `voice_task_float` and cmds 32/33.

- `clkdiv_gold_hz_total_cycles()` — native Hz → `llround(sys / hz)` (GOLD_REF; float-voice GOLD_LIVE).
- `clkdiv_gold_total_cycles()` — Q24 → double Hz → `clkdiv_gold_hz_total_cycles` (`CLKDIV_GOLD` / fixed GOLD_LIVE).
- `clkdiv_q16_total_cycles()` — Q16 Hz → 64/32 (`CLKDIV_Q16`, shipping).
- `clkdiv_precise_q8_total_cycles()` — Q8 Hz → 64/32 (internal; Q8 fallback &lt;16 Hz, not a `CLKDIV_MODE`).
- `clkdiv_q8_total_cycles()` — Same Q8; 32/32 + remainder correction (`CLKDIV_Q8`).
- `clkdiv_fast_q4_total_cycles()` — Q4 Hz → 32-bit `(sys*16 + Q4/2) / Q4` (`CLKDIV_FAST_Q4`).
- `clkdiv_float_hz_total_cycles()` — `fminf(sys/hz + 0.5)` (`CLKDIV_FLOAT` native Hz; float-voice FLOAT_LIVE).
- `clkdiv_float_total_cycles()` — Q24 → float Hz → `clkdiv_float_hz_total_cycles` (`CLKDIV_FLOAT` / fixed FLOAT_LIVE).
- `clkdiv_live_total_cycles` — compile-time alias of `CLKDIV_MODE` (fixed `vt_clk_div`; not a function pointer).
- `clkdiv_live_hz_total_cycles()` — float `vt_clk_div`: FLOAT/GOLD native Hz; else Hz→Q24 then `clkdiv_live_total_cycles`.

### `clkdiv_bench.ino`

GOLD_REF / GOLD_LIVE / FLOAT_LIVE / Q16 / Q8 / FAST_Q4 speed/accuracy on **both** voice engines (`RUNNING_AVERAGE`). Glue matches live (fixed Q24 vs float native Hz; integer on float = Hz→Q24 via `clkdiv_live_hz`). GOLD_REF is true-Hz `llround`. Speed **`pctVsGOLD_REF`**. Accuracy cents + `|Δdiv|` vs GOLD_REF. Q8 vs internal `precise_q8` identity. Does not touch PIO.

**Functions**
- `clkdiv_hp_bench_run_speed()` / `clkdiv_hp_bench_run_accuracy()` / `print_clkdiv_hp_bench()`
  - **Called from:** `bench_poll_core0()` when debug 32/33 pending.
  - **When:** Diagnostics; paced `bench_out_*` TX.

---

## 3. Modulation (ADSR / LFO)

### `adsr.h`

Globals / instances. `env_dco_pitch_centered` + `env_dco_pitch_wave_q15()` (EnvDCO → pitch tap; [`LFO.md`](LFO.md)).

### `adsr.ino`

**Functions**
- `init_ADSR()` — Tables + initial A/D/S/R (uses `linearToLogarithmic`).
  - **Called from:** `setup1()`.
  - **When:** Boot Core1.
- `ADSR_update()` — Parameterless `noteOn`/`noteOff`/`getWave()` (per-call time); `NATIVE_Q15=1`: → `*_q15` only (no per-tick `levelDac`); EnvVCF/EnvVCF2 once; lazy params.
  - **Called from:** `loop1()` play path when `(micros delta) > 100`.
  - **When:** ~10 kHz while playing.
- `ADSR_set_parameters()` — Debounced A/D/S/R apply to all voices.
  - **Called from:** `ADSR_update()`.
  - **When:** Nested in ADSR update.
- `ADSR1_set_restart()` — Set restart/legato mode on all voices.
  - **Called from:** **none (dead)**.
- `ADSR1_change_curves()` — Re-apply after curve change.
  - **Called from:** **none (dead)**.
- `ADSR_VCA_set_restart()` / `ADSR_VCF_set_restart()` — Restart/legato for EnvVCA / EnvVCF.
  - **Called from:** `apply_param_vca_adsr_restart()` / `apply_param_vcf_adsr_restart()` (`params.ino`).
- `ADSR_VCA_change_attack_curve()` / `ADSR_VCA_change_decay_curve()` — EnvVCA curve shape, then re-apply A/D/S/R.
  - **Called from:** `apply_param_adsr1_attack_curve()` / `apply_param_adsr1_decay_curve()` (`params.ino`).
- `ADSR_VCF_change_attack_curve()` / `ADSR_VCF_change_decay_curve()` — EnvVCF curve shape, then re-apply A/D/S/R.
  - **Called from:** `apply_param_adsr2_attack_curve()` / `apply_param_adsr2_decay_curve()` (`params.ino`).

### `LFO.h`

Live LFO instances + Q15 levels + pitch mod arrays. Pitch/drift depth scales (`LFO1_PITCH_DEPTH_SCALE`, `LFO2_PITCH_DEPTH_SCALE`, `DRIFT_PITCH_*`), `lfo_pitch_depth_q24`, and synth-side `applyDepthQ24` (not in mo-lfo). ctor dacSize unused. Prototypes: `LFO1` / `LFO2` / `DRIFT_LFOs`. Docs: [`LFO.md`](LFO.md).

### `LFO.ino`

**Functions**
- `init_LFOs()` — Init LFO1 + LFO2 (`setAmplQ15(MO_LFO_Q15_ONE)`).
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `init_LFO1()` — Configure main detune LFO (`LFO1Waveform`).
  - **Called from:** `init_LFOs()`.
  - **When:** Boot.
- `init_LFO2()` — Configure secondary LFO (`LFO2Waveform`).
  - **Called from:** `init_LFOs()`.
  - **When:** Boot.
- `init_DRIFT_LFOs()` — Init all drift LFOs.
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `init_DRIFT_LFO(lfo&, byte)` — Init one drift LFO (uses `expConverterFloat`).
  - **Called from:** `init_DRIFT_LFOs()`.
  - **When:** Boot Core0.
- `LFO1()` — `getWaveQ15` → `lfo1_pitch_mod_q24[]` via DCO `applyDepthQ24` in `LFO.h` (broadcast when per-osc extras are 0). `__not_in_flash_func`.
  - **Called from:** `loop()` when ~50 µs elapsed (with LFO2 + drift).
  - **When:** Realtime Core0.
- `LFO2()` — `getWaveQ15` → `lfo2_pitch_mod_q24[]`. `__not_in_flash_func`.
  - **Called from:** `loop()` when ~50 µs elapsed.
  - **When:** Realtime Core0.
- `DRIFT_LFOs()` — Update `LFO_DRIFT_LEVEL[]` (negated Q15). `__not_in_flash_func`.
  - **Called from:** `loop()` when ~51 µs elapsed.
  - **When:** Realtime Core0.

---

## 4. Calibration / storage / experimental

### `autotune.h`

Globals / types / prototypes. **No function definitions.**

### `autotune_constants.h`

Constants only. **No function definitions.**

### `autotune_context.h`

**Functions**
- `DCOCalibrationContext::DCOCalibrationContext(...)` — Bind refs for `calibrate_DCO`.
  - **Called from:** `DCO_calibration()` when constructing context.
  - **When:** Auto-cal per oscillator.

### `autotune_measurement.h`

**Functions**
- `measure_gap()` — Wrap `find_gap()` with timeout flag.
  - **Called from:** PW search in `autotune.ino`; `measure_gap_for_amp`, `find_highest_freq`, `find_lowest_freq`, `DCO_calibration_debug`.
  - **When:** Auto-cal / manual-cal measurement.

### `autotune.ino`

**Functions**
- `disable_all_oscillators_and_range_pwm()` — Mute oscs / park RANGE (PIO `range_pio_set_level(DIV_COUNTER)` when `RANGE0_PIO_DITHER_TEST`, else GPIO high); calls `reset_pw_to_DIV_COUNTER_PW`.
  - **Called from:** `DCO_calibration()`, `restart_DCO_calibration()`.
  - **When:** Cal setup.
- `reset_pw_to_DIV_COUNTER_PW()` — Shared PW PWM → max wrap.
  - **Called from:** `disable_all_oscillators_and_range_pwm()`.
  - **When:** Cal setup.
- `DCO_calibration()` — Full auto-cal: PW center/limits once on voice 0, then `calibrate_DCO` + FS write per osc 0..2, reload, precompute; clears `calibrationFlag`.
  - **Called from:** `loop1()` when `calibrationFlag && !manualCalibrationFlag`.
  - **When:** Auto-cal (blocking one-shot).
- `restart_DCO_calibration()` — Reset state/table header between oscillators.
  - **Called from:** `DCO_calibration()` (PW pass and per osc).
  - **When:** Auto-cal.
- `set_pw_and_measure()` — Program PW value, sync `PW[]`/debug tracker, settle, `measure_gap(2)`.
  - **Called from:** all PW search phases (`pw_coarse_scan`, `pw_bisect_bracket`, `pw_fine_scan_around_best`, `pw_lock_in`, `search_PW_limit_from_center`).
  - **When:** Auto-cal PW stage.
- `pw_search_state_init()` / `pw_record_sample()` — `PWSearchState` init and valid-sample bookkeeping (best candidate, in-tolerance count, valid table).
  - **Called from:** `find_PW_for_target_duty()` and its phases.
  - **When:** Auto-cal PW stage.
- `pw_coarse_scan()` — Phase 1: scan PW range for a sign-change bracket; probes the interpolated crossing.
  - **Called from:** `find_PW_for_target_duty()`.
  - **When:** Auto-cal PW stage.
- `pw_bisect_bracket()` — Phase 2a: bisection within the bracket (14 iters max).
  - **Called from:** `find_PW_for_target_duty()` when a bracket was found.
  - **When:** Auto-cal PW stage.
- `pw_fine_scan_around_best()` — Phase 2b: local fine scan when no bracket was found.
  - **Called from:** `find_PW_for_target_duty()`.
  - **When:** Auto-cal PW stage.
- `pw_lock_in()` — Demand 3 consecutive in-band readings at one PW (8 tries max).
  - **Called from:** `pw_select_and_lock()` (candidate + local refinement).
  - **When:** Auto-cal PW stage.
- `pw_select_and_lock()` — Phase 3: pick best candidate from the valid table, lock in, refine PW±2.
  - **Called from:** `find_PW_for_target_duty()`.
  - **When:** Auto-cal PW stage.
- `find_PW_for_target_duty()` — Orchestrates the PW target-duty search phases; returns fallback PW on failure.
  - **Called from:** `find_PW_center()`.
  - **When:** Auto-cal PW stage.
- `find_PW_center()` — Find ~50% PW center; `update_FS_PWCenter`.
  - **Called from:** `DCO_calibration()` (voice 0, once).
  - **When:** Auto-cal.
- `search_PW_limit_from_center()` — Walk PW toward low/high duty limit.
  - **Called from:** `find_PW_limit_v2()`.
  - **When:** Auto-cal.
- `find_PW_limit_v2()` — High-level PW limit; persist low/high via FS.
  - **Called from:** `DCO_calibration()` (LOW then HIGH).
  - **When:** Auto-cal.
- `find_gap()` — Edge-time duty measurement on cal pin (all state local); timeout logs `raw` / `edges` / `rejected` / `accepted`.
  - **Called from:** `measure_gap()`.
  - **When:** Cal measurement (live via wrapper).
- `cal_sense_probe_log()` — 40 ms raw cal-sense edge probe (no period gate); `[CAL_SENSE] pin=…` ~2 Hz.
  - **Called from:** `DCO_calibration_debug()` on gap timeout.
  - **When:** Manual-cal timeout diagnostics. Bench table: [`AUTOTUNE.md`](AUTOTUNE.md) “Cal-sense bench checks” (`DCO_calibration_pin`, currently GP6).
- `DCO_calibration_debug()` — Live gap → `[MANUAL_GAP]` + `serialSendParam32` for UI; probe on TIMEOUT.
  - **Called from:** `loop1()` manual-cal branch every iter.
  - **When:** Manual-cal.

### `autotune_search.ino`

Replaces the old `PID.ino` (the `PID_v1` dependency and legacy PID routines were removed; the file never actually used PID for the live calibration path).

**Functions**
- `compute_gap_tolerance_for_freq()` — Duty tolerance vs frequency.
  - **Called from:** `calibrate_DCO()`; `find_PW_center()`.
  - **When:** Auto-cal.
- `did_sign_change()` — Detect gap error sign flip.
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal amp search.
- `measure_gap_for_amp()` — Set amp PWM, `voice_task_autotune`, `measure_gap`; normalizes sign (positive = amplitude too low).
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal.
- `update_best_from_neighbours()` — Probe neighbour amps; keep best.
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal.
- `step_amp_from_error()` — Return ±1/±2 PWM step from signed error (caller clamps to bounds).
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal.
- `compute_initial_amp_for_note()` — Initial amp guess (uses log/quadratic interp).
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal.
- `store_note_result()` — Write `[freq,pwm]` into `calibrationData`.
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal per note.
- `find_highest_freq()` — Bisection search for highest usable freq at full RANGE PWM (Hz×100); drives `voice_task_autotune(4, …)` via `calibrationFreqHz`. No PID.
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal, when the table reaches the top of the PWM range.
- `find_lowest_freq()` — Estimate lowest usable freq at RANGE PWM 0 (uses `linearInterpolation` / quadratic).
  - **Called from:** `calibrate_DCO()`.
  - **When:** Auto-cal span setup.
- `calibrate_DCO()` — Main per-note amp-table builder. Guards: max 300 iterations / 30 s per note, max 20 consecutive gap timeouts, PWM clamped to the per-note `[minAmpComp, maxAmpComp]` window (break-with-best when stuck at a bound).
  - **Called from:** `DCO_calibration()`.
  - **When:** Auto-cal.
- `quadraticInterpolation()` — 3-point quadratic `y(x)`.
  - **Called from:** `compute_initial_amp_for_note()`; `find_lowest_freq()`.
  - **When:** Auto-cal.
- `logarithmicInterpolation()` — Log interpolate → uint16.
  - **Called from:** `compute_initial_amp_for_note()`.
  - **When:** Auto-cal.
- `linearInterpolation()` — Linear interpolate.
  - **Called from:** `find_lowest_freq()`.
  - **When:** Auto-cal.
- `expInterpolationSolveY()` — Solve exp curve for table building.
  - **Called from:** `initMultiplierTables()`.
  - **When:** Boot (pitch tables), not cal.

### `FS.h`

Constants / buffers; declares fake-calibration helpers under `ENABLE_FS_CALIBRATION`;
declares `write_fs_bank()` (shared with preset bulk restore).

### `FS.ino`

**Functions**
- `write_fs_bank()` — Truncate/create a LittleFS file and write a full bank in one shot.
  - **Called from:** cal `update_FS_*` paths; `preset_bulk_commit()` for cal targets.
  - **When:** Cal persist; host bulk restore.
- `init_FS()` — Mount LittleFS; load tables into float or Q8 arrays.
  - **Called from:** `setup1()`; end of `DCO_calibration()`; end of `seed_fake_calibration_tables()`; `preset_bulk_commit()` after cal restore.
  - **When:** Boot; after auto-cal write; after fake seed; after bulk cal restore.
- `update_FS_voice()` — Persist one osc amp table.
  - **Called from:** `DCO_calibration()` per osc; `seed_fake_calibration_tables()`.
  - **When:** Auto-cal; fake seed.
- `update_FS_PWCenter()` — Persist PW center.
  - **Called from:** `find_PW_center()`; `seed_fake_calibration_tables()`.
  - **When:** Auto-cal; fake seed.
- `update_FS_PW_High_Limit()` — Persist PW high limit.
  - **Called from:** `find_PW_limit_v2()`; `seed_fake_calibration_tables()`.
  - **When:** Auto-cal; fake seed.
- `update_FS_PW_Low_Limit()` — Persist PW low limit.
  - **Called from:** `find_PW_limit_v2()`; `seed_fake_calibration_tables()`.
  - **When:** Auto-cal; fake seed.
- `update_FS_ManualCalibrationOffset()` — Persist manual offset.
  - **Called from:** `apply_param_manual_calibration_store()`.
  - **When:** Serial2 param (user store).
- `generate_fake_calibration_data()` — Build one osc’s 22 `[freq_x100, RANGE PWM]` pairs (archived curve shape, real note schedule).
  - **Called from:** `seed_fake_calibration_tables()`.
  - **When:** Fake seed.
- `seed_fake_calibration_tables(force)` — Write full fake amp-comp + PW banks to LittleFS (`"w"` truncate), then `init_FS()`. Precomputes when `force=true`. Silent (no Serial). `force=false` only if `voiceTables` is missing.
  - **Called from:** `setup1()` with `false` (before `init_FS`); `apply_param_debug_command` case **30** with `true`.
  - **When:** Boot if file missing; on-demand force-overwrite.

### `preset_store.h`

MCU preset record layout, CRC32 helpers, `preset_param_is_persistable()`, shadow
capture, bulk target enums, `preset_store_boot_task()`. Deep doc: [`PRESET_STORE.md`](PRESET_STORE.md).

### `preset_store.ino`

**Functions**
- `preset_store_save()` — Build 598-byte record from shadow + block globals; write into chunk file `pbNN` (4 records/file); update `pstLast`; print `[preset] saved…`.
  - **Called from:** `apply_param_preset_save()`.
  - **When:** `'p'` 170 / host “save board live state”.
- `preset_store_load()` — Read/validate record; apply params + blocks; mirror to Input; update `pstLast`; print `[preset] loaded…`; `serial_send_preset_loaded_to_mb()` (`'L'`).
  - **Called from:** `apply_param_preset_load()`; `handleProgramChange()`; `preset_store_boot_recall()`.
  - **When:** `'p'` 171; MIDI PC; boot recall.
- `preset_store_send_directory_to_mb()` — Send all 256 slots as `'O'` frames (`[slot][name:16]`, blank = unused) over `Serial2`.
  - **Called from:** `input_handle_preset_dir_request()`.
  - **When:** `'N'` Serial RX (Input boot / browse-mode-enter).
- `preset_store_dump()` — −1 → `[pdir]` listing; 0..255 → `[dump]` hex of slot record.
  - **Called from:** `apply_param_preset_dump()`.
  - **When:** `'p'` 172.
- `preset_store_cal_dump()` — Stream one or all five cal LittleFS files as `[dump]` lines.
  - **Called from:** `apply_param_cal_dump()`.
  - **When:** `'p'` 173.
- `preset_bulk_chunk()` — Stage 32 bytes at offset in bulk buffer (`'B'`).
  - **Called from:** `input_handle_bulk_chunk()`.
  - **When:** Serial RX.
- `preset_bulk_commit()` — CRC-verify staging; write preset slot or cal file; reload cal via `init_FS()`; print `[bulk] ok/err…`.
  - **Called from:** `input_handle_bulk_commit()`.
  - **When:** Serial RX.
- `preset_store_boot_recall()` — Load slot from `pstLast` if present and not calibrating.
  - **Called from:** `preset_store_boot_task()`.
  - **When:** Boot one-shot.
- `preset_store_boot_task()` — Inline in header: after `PRESET_BOOT_RECALL_MS`, clear pending and call `preset_store_boot_recall()`.
  - **Called from:** `loop()` on `timer1msFlag`.
  - **When:** Once per boot.

### `irq_tuner.h` / `irq_tuner.ino`

Experimental; bodies commented. **No active function definitions.**

---

## 5. MIDI, serial, parameters

### `midi.h`

Prototypes / instances (`init_midi`, `mono_note_stack_clear`). **No function definitions.**

### `midi_cc.h`

MIDI CC control surface: the `MIDI_CC_LINEAR` / `MIDI_CC_EXP_TIME` curves, the `CC_LOCAL_*` codes (224 up) for the values that arrive as `'a'`–`'d'` block frames and so have no `ParamId`, the `MidiCcEntry` layout, and the prototypes for the two functions in `midi.ino`. Includes `midi_cc_map.h`. **No function definitions.**

### `midi_cc_map.h`

**Generated** — `midiCcMap[]`, one row per controller. Emitted by `tools/dco_control/gen_midi_map.py` from `tools/dco_control/params.py`, together with `docs/MIDI_CC_MAP.md` and the Open Stage Control session in `tools/panels/`. Do not edit; change `params.py` and re-run.

### `midi.ino`

**Functions**
- `init_midi()` — Register handlers on USB + Serial1 MIDI; `turnThruOff()` on both (synth, not a thru box).
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `handleNoteOn()` — → `note_on()`.
  - **Called from:** MIDI library (registered in `init_midi`).
  - **When:** MIDI callback from `loop` `.read()`.
- `handleNoteOff()` — → `note_off()`.
  - **Called from:** MIDI library.
  - **When:** MIDI callback.
- `handleControlChange()` — CC 42 → pitch-bend range / Q24 multiplier; every other controller → `midi_cc_handle()`.
  - **Called from:** MIDI library.
  - **When:** MIDI callback.
- `midi_cc_handle()` — Find the controller in `midiCcMap[]`, scale it into `lo..hi`, apply the exp curve for envelope times, then `midi_cc_apply()`. Unmapped CCs ignored.
  - **Called from:** `handleControlChange()`.
  - **When:** MIDI callback.
- `midi_cc_apply()` — Dispatch: a `CC_LOCAL_*` target writes ADSR/filter block globals here (`cv_bake_adsr2_to_vcf_scale` / `cv_bake_lfo2_to_vcf_scale` for the matching depth CC; CUTOFF/RESONANCE assign without scale bake). PW (`PARAM_PW_VALUE`) and EnvVCA→VCA (`PARAM_ADSR1_TO_VCA`) and every other mapped ParamId go to `update_parameters()` plus `serial_echo_persistable_param16()`.
  - **Called from:** `midi_cc_handle()`.
  - **When:** MIDI callback.
- `handleProgramChange()` — `preset_store_load(midiPresetBank*128 + program)` when slot < 256 (CC 0/32 latch bank).
  - **Called from:** MIDI library.
  - **When:** MIDI program change.
- `handlePitchBend()` — Sets `midi_pitch_bend`.
  - **Called from:** MIDI library.
  - **When:** MIDI callback.
- `handleAfterTouchChannel()` — → `mod_matrix_set_aftertouch()`.
  - **Called from:** MIDI library.
  - **When:** MIDI callback.
- `mono_note_stack_clear()` — Empty the mono last-note held stack (Core0).
  - **Called from:** `setVoiceMode()` when `voiceMode == 0`.
  - **When:** Entering mono (boot / voice-mode param).
- `note_on()` — Allocate voice(s), set ADSR/note flags (local envelopes only, nothing on the wire). Mono: push last-note stack then `VOICE_NOTES[0]` / gate / `note_on_flag` + `noteStart`. Para/poly: existing allocators.
  - **Called from:** `handleNoteOn()`.
  - **When:** MIDI note-on.
- `note_off()` — Mono: remove from held stack; empty → gate off + `noteEnd`; else fall back to stack top + `note_on_flag` (porta, no `noteStart`). Para/poly: scan slots, gate off + `noteEnd`.
  - **Called from:** `handleNoteOff()`.
  - **When:** MIDI note-off.

### `Serial.h`

Prototypes; defines `SERIAL_INNER_MAX_PAYLOAD` **36** (for `'B'` bulk chunk) before
including `serial_frame.h`. Declares block-echo helpers used after preset load.

### `Serial.ino`

**Functions**
- `init_serial()` — Serial1 MIDI baud (RX 1 / TX 0 @ 31250, IRQ/`setPollingMode(false)`), Serial2 2.5M Input link against the Input's `Serial1` (RX 21 from Input TX GP0, TX 20 into Input RX GP1); builds the O(1) command LUT (incl. `'B'`/`'C'`/`'N'`).
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `input_handle_adsr1()` / `input_handle_adsr2()` / `input_handle_adsr3()` — `'a'`/`'b'`/`'c'` LE → EnvVCA / EnvVCF / EnvDCO (`ADSR1_*`) times.
  - **Called from:** Serial2 / USB parser LUT (`inputSerialLut`).
  - **When:** Serial RX.
- `input_handle_filter_block()` — `'d'` LE → `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF`, then `cv_bake_adsr2_to_vcf_scale()` + `cv_bake_lfo2_to_vcf_scale()`.
  - **Called from:** parser LUT.
  - **When:** Serial RX.
- `input_handle_param16()` — `'p'` → `update_parameters` (id + i16 LE); USB ingress also `serial_echo_persistable_param16`.
  - **Called from:** parser LUT.
  - **When:** Serial RX.
- `input_handle_preset_name()` — `'q'` → `presetName[]` (16 chars).
  - **Called from:** parser LUT.
  - **When:** Serial RX.
- `input_handle_bulk_chunk()` / `input_handle_bulk_commit()` — `'B'`/`'C'` → `preset_bulk_chunk` / `preset_bulk_commit`.
  - **Called from:** parser LUT.
  - **When:** Serial RX (USB bench restore).
- `input_handle_preset_dir_request()` — `'N'` → `preset_store_send_directory_to_mb()` (256× `'O'` frames on `Serial2`).
  - **Called from:** parser LUT.
  - **When:** Serial RX (Input boot / browse-mode-enter).
- `serial_send_adsr_vca_block_to_mb()` / `serial_send_adsr_vcf_block_to_mb()` / `serial_send_adsr_dco_block_to_mb()` / `serial_send_filter_block_to_mb()` — Mirror current block globals to Input as `'a'`–`'d'`.
  - **Called from:** `preset_record_apply()` after load.
  - **When:** Preset recall.
- `serial_send_preset_loaded_to_mb()` — `'L'` `[slot]` to Input.
  - **Called from:** `preset_store_load()`.
  - **When:** End of every successful load (boot recall, MIDI PC, USB/`dco_control`, Input).
- `serial_panel_task()` — Non-blocking parser pump (`serial_parser_drain`). Sets ingress `PARAM_SRC_INPUT`. `__not_in_flash_func`.
  - **Called from:** `loop()` when `timer1msFlag`.
  - **When:** Realtime Core0, ~1 ms.
- `init_usb()` — TinyUSB CDC+MIDI descriptors, `Serial.begin`, re-enumerate.
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `serial_usb_task()` — Same pump for USB CDC: second `SerialParserContext`, same LUT. Sets ingress `PARAM_SRC_USB` so persistable `'p'` echoes to Input. Guarded by `ENABLE_USB_CONTROL`; returns if `!Serial`. `__not_in_flash_func`. See [`tools/dco_control`](../tools/dco_control/README.md).
  - **Called from:** `loop()` when `timer1msFlag`.
  - **When:** Realtime Core0, ~1 ms, only when `ENABLE_USB_CONTROL` is defined and CDC is open.
- `serialSendParam16()` — Slim `'p'` TX via `serial_frame_write` (id + i16 LE, 3 B) out Serial2. Drops if `availableForWrite() < 1`.
  - **Called from:** `serial_echo_persistable_param16()`.
  - **When:** USB/MIDI apply of a persistable id.
- `serial_echo_persistable_param16()` — If `preset_param_is_persistable(id)`, `serialSendParam16` (wire i16, not Q24). Skips cal/debug/UI/preset-cmd ids and `'a'`–`'d'`.
  - **Called from:** `input_handle_param16()` (USB ingress); `midi_cc_apply()` default; `preset_record_apply()`.
  - **When:** USB/MIDI apply of a persistable id; after preset load.
- `serialSendParam32()` — Slim `'x'` TX via `serial_frame_write` (id + u32 LE, 5 B) out Serial2 TX 20 into Input `Serial1` RX GP1 (gap 154, cal 155; Input relays 154 to Screen). Drops if `availableForWrite() < 1`.
  - **Called from:** `apply_param_manual_calibration_flag()`; `DCO_calibration_debug()`.
  - **When:** Manual-cal param / live gap report.

### `serial_protocol.h`

Compatibility stub that includes `serial_input_protocol.h`. Mainboard `'n'`/`'o'`/`'s'` retired. **No function definitions.**

### `serial_input_protocol.h`

Command bytes + payload sizes + `serial_input_payload_len()`. Includes `'B'` (36) / `'C'` (8) bulk restore and `'N'` (1, unused pad byte) / `'O'` (17) / `'L'` (1) Input directory sync. `'p'` is Input→DCO apply and DCO→Input persistable mirror. `'q'` is 16 ASCII bytes. **No other function definitions.**

### `serial_frame.h`

Inner pack/unpack + buffer COBS encode/decode + `serial_frame_stuff` / `unstuff` / `write()`. Default `SERIAL_FRAMING_RAW` (on-wire = inner). `#define SERIAL_FRAMING_COBS` in `DCO.ino` wraps `COBS(inner)+0x00`. `#error` if both flags are forced. `SERIAL_INNER_MAX_PAYLOAD` overridable (`#ifndef`, default 8; DCO sets **36** in `Serial.h` for `'B'`; Input/Screen set 17). `SERIAL_FRAME_DELIMITER` `0x00` never a command. Codec has no Stream type — UART today, SPI later.

**Functions**
- `serial_cobs_encode()` / `serial_cobs_decode()` — Buffer COBS; decode src has no trailing `0x00`.
  - **Called from:** `serial_frame_stuff()` / `serial_frame_unstuff()`.
- `serial_inner_pack()` / `serial_inner_unpack()` — `[cmd][payload]` ↔ buffer.
  - **Called from:** `serial_frame_stuff()` / `serial_frame_unstuff()`.
- `serial_frame_stuff()` / `serial_frame_unstuff()` — Inner ↔ on-wire (RAW copy or COBS+`0x00`).
  - **Called from:** `serial_frame_write()`; parser COBS path.
- `serial_frame_write()` — Stuff into a stack buffer, `stream.write(...)`.
  - **Called from:** `serialSendParam32()` (slim `'x'`). Input/Screen TX uses the same helper.

### `serial_param_protocol.h`

**Functions**
- `decode_u16_le()` / `decode_i16_le()` / `decode_u32_le()` / `encode_u16_le()` / `encode_u32_le()` — LE helpers.
  - **Called from:** ADSR/filter handlers; `decode_param_p` / `encode_param32`.
- `decode_param_p()` — Decode `'p'` `[id][i16 LE]`.
  - **Called from:** `input_handle_param16()`.
- `encode_param_p()` — Encode `'p'` payload.
  - **Called from:** `serialSendParam16()` (DCO→Input persistable mirror).
- `encode_param32()` — Encode slim `'x'` payload (id + u32 LE).
  - **Called from:** `serialSendParam32()`.

### `serial_parser.h`

Non-blocking inner-frame parser. RAW: cmd LUT + fixed payload. COBS (`SERIAL_FRAMING_COBS`): accumulate into `rx[]` until `0x00`, then `serial_frame_unstuff` + LUT check. Timeout aborts a partial stuffed frame if the stream is idle `SERIAL_FRAME_TIMEOUT_US` (500 µs).

**Functions**
- `serial_parser_reset()` — Reset parser to idle.
  - **Called from:** `serial_parser_process_byte` / timeout path.
- `serial_command_table_init()` — Fill 256-entry LUT from `SerialCommandDef[]`.
  - **Called from:** `init_serial()`.
- `serial_parser_check_timeout()` — Abort partial frame.
  - **Called from:** `serial_parser_drain()` when mid-frame and stream idle.
- `serial_parser_process_byte()` — Feed one on-wire byte (RAW fixed-length or COBS until `0x00`); invoke handler when complete.
  - **Called from:** `serial_parser_drain()`.
- `serial_parser_drain()` — One `available()` snapshot, then `read()` up to `SERIAL_DRAIN_BYTE_BUDGET` (64); timeout only when idle (incl. partial COBS stuffed frame).
  - **Called from:** `serial_panel_task()` / `serial_usb_task()`.

### `params_def.h`

`enum ParamId` only (incl. `PARAM_PRESET_SAVE` 170 … `PARAM_CAL_DUMP` 173). **No function definitions.**

### `param_router.h`

**Functions**
- `param_router_build_jump()` — Fill 256-entry apply jump table from `paramTable[]`.
  - **Called from:** `init_param_router()`.
  - **When:** Boot Core0.
- `param_router_apply_jump()` — O(1) ParamId → apply callback.
  - **Called from:** `update_parameters()`.
  - **When:** `'p'` frames / MIDI CC.
- `param_router_apply()` — Linear scan fallback.
  - **Called from:** **none on DCO**.

### `params.ino`

**Functions**
- `init_param_router()` — Build O(1) jump table from `paramTable[]`.
  - **Called from:** `setup()`.
  - **When:** Boot Core0.
- `update_parameters()` — `preset_shadow_capture()` then route id/value through the jump table.
  - **Called from:** `input_handle_param16()`; `midi_cc_apply()` default; `preset_record_apply()`.
  - **When:** `'p'` frames / MIDI CC / preset load.
- All `apply_param_*()` below — **Called from:** **param table only** (never direct). **When:** matching ParamId.

- `apply_param_osc*_saw/pulse/tri_enable()` — Per-osc wave enables → `update_waveSelector()`.
- `apply_param_osc1_level()` / `apply_param_osc2_level()` / `apply_param_osc3_level()` / `apply_param_sub_level()` — Mix level **bases** (PWM via matrix in `update_CV_outs`).
- `apply_param_mod_slot*_source/dest/depth()` — Mod matrix slots 0–7 (ParamIds 60–83).
- `apply_param_adsr3_to_osc_select()` — ADSR3 routing select.
- `apply_param_lfo1_waveform()` — LFO1 waveform.
- `apply_param_lfo2_waveform()` — LFO2 waveform.
- `apply_param_octave_shift()` — global `octave_shift` (wire id `PARAM_OSC1_INTERVAL`).
- `apply_param_osc2_interval()` — OSC2 interval.
- `apply_param_osc2_detune_val()` — OSC2 detune.
- `apply_param_lfo2_to_osc2()` — LFO2→OSC2 fine pitch depth (0..255, `expConverterFloat`).
- `apply_param_lfo2_to_osc3()` — LFO2→OSC3 fine pitch depth (0..255).
- `apply_param_lfo2_to_osc2/3_coarse()` — LFO2 coarse pitch (0..511; LFO1 curve + amp scale at apply time).
- `apply_param_osc_sync_mode()` — Osc sync / phase-align: sets `oscSync` + `phaseAlignOSC2`, retriggers notes.
- `apply_param_portamento_time()` — Porta time (uses `expConverter`).
- `apply_param_portamento_mode()` — Time vs slew porta.
- `apply_param_calibration_value()` — Reserved / no-op.
- `apply_param_voice_mode()` — → `setVoiceMode()`.
- `apply_param_unison_detune()` — Unison amount.
- `apply_param_analog_drift_amount()` — Drift depth.
- `apply_param_analog_drift_speed()` — Drift speed (recomputes via `expConverterFloat`).
- `apply_param_analog_drift_spread()` — Drift spread (recomputes speeds).
- `apply_param_sync_mode()` — → `setSyncMode()`.
- `apply_param_soft_sync()` — Soft sync threshold 0..3 (hard / poll N=1/2/3) → `setSyncMode()`.
- `apply_param_subosc_divide()` — Legacy id 37 → deferred `set_subosc_divide()` (engine2: both subs).
- `apply_param_sub1/2_divide/master/phase/width()` — ParamIds 90–97 → `subosc_param_*()`.
- `apply_param_sub_logic_op()` — ParamId 99 → `subosc_param_logic_op()`.
- `apply_param_lfo1_to_dco()` — LFO1→DCO depth (`expConverterFloat`).
- `apply_param_lfo1_to_osc1/2/3()` — additive LFO1 pitch depth per osc (stacks on global FIFO bus).
- `apply_param_lfo1_speed()` — LFO1 rate.
- `apply_param_lfo2_speed()` — LFO2 rate.
- `apply_param_lfo2_to_pw()` — LFO2→PW depth.
- `apply_param_adsr1_to_pwm()` — ADSR→PWM depth (`PARAM_ADSR3_TO_PWM`).
- `apply_param_adsr1_to_detune1()` — ADSR→detune + Q24 scale.
- `apply_param_adsr3_pitch_mode()` — `PARAM_ADSR3_PITCH_MODE` (223): EnvDCO pitch tap unipolar/centered → `env_dco_pitch_centered`. See [`LFO.md`](LFO.md).
- `apply_param_adsr1_curve()` — Attack curve hook (light/reserved).
- `apply_param_adsr2_curve()` — Decay curve hook (light/reserved).
- `apply_param_pw_value()` — Pulse width → `PW[0] = v / 4` (`PARAM_PW_VALUE`).
- `apply_param_adsr1_to_vca()` — EnvVCA → VCA amount (`PARAM_ADSR1_TO_VCA`).
- `apply_param_pwm_pots_manual()` — Manual PWM pots flag.
- `apply_param_function_key()` — Function key (reserved/no-op).
- `apply_param_calibration_flag()` — Sets `calibrationFlag` → next `loop1` auto-cal.
- `apply_param_manual_calibration_flag()` — Manual cal mode; may `serialSendParam32` offsets.
- `apply_param_manual_calibration_stage()` — Manual cal stage index.
- `apply_param_manual_calibration_offset()` — Per-osc manual offset.
- `apply_param_manual_calibration_store()` — → `update_FS_ManualCalibrationOffset`.
- `apply_param_character()` — `PARAM_CHARACTER` (221): master 0..128 → `character_recompute_scales()`. See [`CHARACTER.md`](CHARACTER.md).
- `apply_param_preset_save()` / `apply_param_preset_load()` / `apply_param_preset_dump()` / `apply_param_cal_dump()` — ParamIds 170–173 → `preset_store_*`. See [`PRESET_STORE.md`](PRESET_STORE.md).
- `apply_param_debug_command()` — Bench diagnostics (id 160): 1 → `pio_topology_report()`, 2/3 → `pio_period_probe()` at a low/high divider, **4 → `subosc2_report()`**, 10/11/12 → profiler dump / reset / periodic toggle (`RUNNING_AVERAGE`), **13 → `mem_diag_request()`** (heap/stack; `ENABLE_MEM_DIAG` + runtime polls on; [`MEMORY.md`](MEMORY.md)), **14/15 → mem_diag polls off/on** (ack `mem_diag polls=…`; `compiled out` if flag off), 20–22 → amp-comp method (FLOAT_QUAD / LUT / FIXED), 24/25 → amp-comp speed/accuracy (`AMP_COMP_BENCHMARK` + `RUNNING_AVERAGE`), 28/29 → pitch-interp speed/accuracy (`RUNNING_AVERAGE`), 30 → force-seed fake calibration tables, **32/33 → clkdiv all six vs GOLD_REF** (both voice engines; `RUNNING_AVERAGE`), **200–50000** (uint16) → set `pioPulseLength` and reload running SMs via `pio_defer_request_reset_pulse_all()`, **0xC8xx / 0xCAxx / 0xCBxx** → Character-tab axis jitters (amp / pitch / PW) then recompute scales. Period probes only hold with no note playing.

---

## 6. Timing / utilities

### `Timer_micros.h`

Flag + period constexprs (`kTimer50us`, `kTimer1ms`, …). **No function definitions.**

### `Timer_micros.ino`

**Functions**
- `init_micros_timers()` — Seed last-fire stamps on both cores.
  - **Called from:** `setup()` / `setup1()`.
- `microsTimer()` — Core 0 soft flags: 50 / 51 / 99 µs and **1 ms** (`timer1msFlag`, panel + USB CDC drain). `__not_in_flash_func`.
  - **Called from:** `loop()` every iteration.
  - **When:** Realtime Core0.
- `microsTimer2()` — Core 1 soft flags (99 µs, ~1 ms, …). `__not_in_flash_func`.
  - **Called from:** `loop1()` every iteration.
  - **When:** Realtime Core1.

### `noise.h`

Sketch declarations only (Arduino style). Library: `DCO_Noise`.
Compile `NOISE_ENGINE` in `DCO.ino` selects `DcoNoiseGen` for `noise0`…`noise1` (`NUM_NOISE_GENS = 2`):
- **0 — `ColoredNoise`:** Voss pink / 1-pole brown / white; `PioNoiseWhite` peels.
- **1 — `FastNoiseGen`:** economy Voss pink / leaky brown / local xorshift white.
- **2 — `PrimeHybridNoise`:** three prime tables; color at ctor/`begin` (~9 KB/gen).
- **3 — `ProNoise32`:** Q16.15 Kellett pink / DC-corrected brown / xorshift white.

Ctor args set color/seed; `next()` is always Q15. `setup1` calls `dcoNoisePioBegin`; `loop1`
calls `dcoNoisePioRefill` + `.next()` into `noiseLevel[0..1]`. Mod-matrix sources 14/15
(Noise 2/3) stay reserved and read as 0. Flag gating for PIO white lives in the library
(`dcoNoiseUsesPioWhite`). See [`BENCHMARKING.md`](BENCHMARKING.md) §11.

### `utils.h`

Prototypes. **No function definitions.**

### `utils.ino`

**Functions**
- `uintToStr()` — uint64 → C string.
  - **Called from:** **none (dead)** — only commented use in `irq_tuner.ino`.
- `linearToLogarithmic()` — Linear → log map.
  - **Called from:** `init_ADSR()`.
  - **When:** Boot.
- `linearToExponential()` — Linear 0..4095 → exp 0..maxValue, the Input board's fader curve.
  - **Called from:** `midi_cc_handle()` for `MIDI_CC_EXP_TIME` entries (base 50, max 25000).
  - **When:** MIDI CC on an envelope attack / decay / release.
- `faderExpConverter()` — Fader exp curve.
  - **Called from:** **none (dead)**.
- `expConverterFloat()` — Exp curve → float.
  - **Called from:** `params.ino` applies; `init_DRIFT_LFO()`; also inside dead `controls_formula_update`.
  - **When:** Serial2 params; boot drift init.
- `expConverter()` — Exp curve → uint16.
  - **Called from:** `apply_param_portamento_time()`.
  - **When:** Serial2 param.
- `expConverterReverse()` — Inverse exp.
  - **Called from:** **none (dead)**.
- `expConverterFloatReverse()` — Inverse float exp.
  - **Called from:** **none (dead)**.
- `expConverter2()` — Alternate exp curve.
  - **Called from:** **none (dead)**.
- `formula_update()` — Legacy formula slot update.
  - **Called from:** **none (dead)**.
- `controls_formula_update()` — Map controls to float params.
  - **Called from:** **none (dead)**.
- `led_blinking_task()` — LED heartbeat.
  - **Called from:** **none (dead)** — declared in `globals.h` but never called.

---

## 7. Documentation

All detailed docs live under `docs/` (this file included). Root `README.md` is the entry point.

| File | Purpose |
|------|---------|
| `README.md` (repo root) | Overview, build, documentation index. |
| `docs/DOCUMENTATION_PROCEDURE.md` | Reusable phased guide to document any DCO4 board. |
| `docs/SYSTEM_OVERVIEW.md` | System / UART topology (incl. optional RP2040 voice aux). |
| `docs/DUAL_MCU.md` | RP2350A + RP2040 ownership; Input TX fanout; `ENABLE_VOICE_AUX`; ParamId table. |
| `../VOICE-AUX/` | RP2040 voice-aux sketch (Dist 52/53, mode 54); see `VOICE-AUX/docs/README.md`. |
| `docs/ENGINE_OPTIONS.md` | Float/fixed engine flags. |
| `docs/BUILD_FLAGS.md` | Complete compile-time flag catalog (incl. `ENABLE_SUBOSC_ENGINE2`). |
| `docs/PIO_OSCILLATORS.md` | PIO programs, period model, sync, phase align, sub-osc §9. |
| `docs/PINOUT.md` | Provisional pin map (subs GP8/9, combiner out GP10, CV conflicts). |
| `docs/MOD_MATRIX.md` | Sparse mod matrix (incl. sub phase / PW dests 10/11). |
| `docs/BENCHMARKING.md` | Hot-path profiler: probes, reading the budget, adding a probe. |
| `docs/MEMORY.md` | SRAM / heap / stack: `__not_in_flash_func`, dump cmd 13, pin policy. |
| `docs/DISTORTION.md` | Post-LP Drive/Mix distortion hardware idea and CV prototype. |
| `docs/FILTER_ROUTING.md` | SSI2144 → dist → AS3320 multimode concept; digital stage switching (DG411/4066). |
| `docs/schematics/distortion/` | KiCad 10 project (`distortion.kicad_pro`) for the distortion stage. |
| `docs/REFERENCE_AI.md` | Deep semantic map. |
| `docs/FILE_INDEX.md` | This file — files, functions, call sites. |
| `docs/PRESET_STORE.md` | MCU 256-slot presets, cal dump/restore, `'B'`/`'C'`, text dump protocol. |
| `docs/README_serial_and_params.md` | Slim inner serial / ParamId how-to, including MIDI CC, RAW vs COBS, preset cmds. |
| `docs/MIDI_CC_MAP.md` | **Generated** — MIDI CC implementation chart. |
| `docs/Serial_comms_and_params_reference.txt` | **Archive** — Mainboard-era protocol notes. |
| `docs/AUTOTUNE.md` | Autotune algorithms. |
| `docs/AUTOTUNE_REFACTORED.md` | Autotune refactor structure. |
| `docs/FIXED_POINT_ANALYSIS.md` | **Archive** |
| `docs/FIXED_POINT_PLAN.md` | **Archive** |

---

## 8. Other / non-active

| File | Purpose |
|------|---------|
| `params.ino.backup_old_version` | Old params backup; not compiled. |
| `.gitignore` | Ignores editor/local clutter. |

---

## 9. Vendored libraries (`_build_libs/`)

| Library | Path | Used by |
|---------|------|---------|
| `ADSR_Bezier` | `_build_libs/ADSR_Bezier` (symlink → monorepo root, branch `main`) | `adsr.*` |
| `DCO_Noise` | `_build_libs/DCO_Noise` (symlink → monorepo root) | `noise.h` fleet |
| `mo-lfo` | `_build_libs/mo-lfo` | `LFO.*` |
| `MIDI_Library` | `_build_libs/MIDI_Library` | `midi.*` |
| `PID_v1` | `_build_libs/PID_v1` | **unused** (kept on disk; the autotune cleanup removed the last user) |

## 10. Other external dependencies

| Library | Used by |
|---------|---------|
| Adafruit TinyUSB | USB MIDI |
| LittleFS (RP2040 core) | `FS.*` |

---

## Quick “where do I change X?”

| Goal | Start here |
|------|------------|
| Engine float/fixed | `DCO.ino` flags → `voice_task_main` |
| New ParamId | `params_def.h` → `params.ino` table (only call path); persistable patch ids also `preset_param_is_persistable` |
| Serial command | `serial_input_protocol.h` + `serial_frame.h` + `Serial.ino` handlers ← `serial_panel_task` (Serial2) and `serial_usb_task` (USB CDC) in `loop` |
| MCU preset / cal dump | [`PRESET_STORE.md`](PRESET_STORE.md); `preset_store.*`; host [`tools/dco_control`](../tools/dco_control/README.md) |
| Control the board with no panel | [`tools/dco_control`](../tools/dco_control/README.md) over USB; needs `ENABLE_USB_CONTROL` + LittleFS flash slice |
| Start auto-cal | Param → `apply_param_calibration_flag` → `loop1` → `DCO_calibration` |
| Manual cal UI | `apply_param_manual_calibration_*` → `loop1` manual branch |
| MIDI notes | `loop` → MIDI `.read` → `note_on`/`note_off` → local `noteStart[]`/`noteEnd[]` (no serial note frames) |
| Mono note priority / held notes | `midi.ino` mono stack (`mono_note_stack_*`); porta still via `note_on_flag` → `voices.ino` |
| MIDI CC assignments | `tools/dco_control/params.py` `cc=` field → `gen_midi_map.py` → `midi_cc_map.h` + [`MIDI_CC_MAP.md`](MIDI_CC_MAP.md) |
| Play audio path | `loop1` → `ADSR_update` + `voice_task_main` |
| OSC2 note-on phase align | `PARAM_OSC_SYNC_MODE` → `oscSync` / `phaseAlignOSC2`; EXACT_Y in `voices.ino` → `osc_phase_align_hold_stopped` ([`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §8) |
| Measure where the time goes | `RUNNING_AVERAGE` in `DCO.ino` → probe table in `bench.h` → debug command 10 |
| Measure SRAM / heap / stack | dump cmd **13** → [`MEMORY.md`](MEMORY.md) / `mem_diag.ino`; `ENABLE_MEM_DIAG` + runtime 14/15; pin policy there |
| RANGE carrier / slice vs PIO dither | `RANGE0_PIO_DITHER_TEST` in `DCO.ino` → `PWM.h` / `PWM.ino` / [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §4.4 |
| Subs / logic combiner | `ENABLE_SUBOSC_ENGINE2` → `subosc.ino` / [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9; ParamIds 90–99; debug opcode 4 |
