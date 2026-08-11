# DCO Build Flags Catalog

Complete inventory of **live compile-time flags** that change codegen, RAM, IO, or A/B math paths.

- **Deep float/fixed math:** [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- **Profiler usage:** [`BENCHMARKING.md`](BENCHMARKING.md)
- **SRAM / heap / stack:** [`MEMORY.md`](MEMORY.md) (`__not_in_flash_func`, dump cmd 13, pin policy)
- **Live source of truth for engine/IO toggles:** top of [`../DCO.ino`](../DCO.ino) before includes

**Override order:** flags set in `DCO.ino` before `#include` win. Header `#ifndef` / library fallbacks apply only when the sketch left them unset.

**Out of scope here:** include guards, X-macro / dirty-bit / note-frequency tables, `_removed/`, MIDI_Library / PID examples. [`../tusb_config.h`](../tusb_config.h) exists but is **not included** (commented in `DCO.ino` / `include_all.h`).

---

## Shipping snapshot (both MCUs)

Unless overridden, RP2350 and RP2040 ship the same engine shape:

| Area | Shipping |
|------|----------|
| Voice / amp / CV | Fixed (`USE_FLOAT_*` undefined) |
| Pitch | `PITCH_INTERP_RATIO_Q16` |
| Amp method default | `2` (`FIXED`) |
| Clkdiv | `CLKDIV_MODE CLKDIV_Q16` |
| Noise | `NOISE_ENGINE 1`, `ENABLE_NOISE_OUT` off |
| ADSR | fixed Q22 phase, micros, native Q15 |
| USB panel | `ENABLE_USB_CONTROL` on |
| CV / mux / aux HW | off (`ENABLE_CV_OUTS` / `WAVE_MUX` / `VOICE_AUX` commented) |
| Sub-osc engine2 | **on RP2350** (two subs + combiner), unavailable RP2040 (`ENABLE_SUBOSC_ENGINE2`) |
| PIO RESET | `ENABLE_PIO_RESET_INVERT` on |
| RANGE amp PWM | `RANGE0_PIO_DITHER_TEST` on (PIO dither; comment out = slice PWM) |
| Profiler (tree as checked in) | `RUNNING_AVERAGE` + `RUNNING_AVERAGE_PERIOD` on; FINE off |
| Mem diag (dump 13) | `ENABLE_MEM_DIAG` on; runtime polls on |

Confirm with the `engine:` / `io:` lines in the profiler banner ([`bench.h`](../bench.h)).

---

## 1. [`DCO.ino`](../DCO.ino) — before includes

### 1.1 Engine — pitch mode ids

| Define | Value | Role |
|--------|------:|------|
| `PITCH_INTERP_FLOAT` | 0 | Walk-find float interp (needs float voice) |
| `PITCH_INTERP_RATIO_Q16` | 1 | Native Q16 slope path (shipping) |
| `PITCH_INTERP_Q12` | 2 | Legacy ×10000 slope A/B |
| `PITCH_INTERP_FLOAT_FAST` | 3 | Trunc+clamp±1 float find (needs float voice) |

### 1.2 Engine — board defaults (`PICO_RP2350` / else)

Both branches currently set the same values via `#ifndef`:

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `AMP_COMP_METHOD_DEFAULT` | `2` (FIXED) | Initial `amp_comp_method` | [`amp_comp.h`](../amp_comp.h), runtime cmds 20–22 |
| `PITCH_INTERP_MODE` | `PITCH_INTERP_RATIO_Q16` | Selects one interpolator + its tables | [`voices.ino`](../voices.ino), pitch benches |
| `CLKDIV_MODE` | `CLKDIV_Q16` (`2`) | Clkdiv (accuracy order): `0` GOLD (double `llround`); `1` FLOAT (Q24 → float Hz / native Hz on float voice); `2` Q16 (shipping); `3` Q8 (Q8 32/32+corr); `4` FAST_Q4 (Q4 32/32). Value `0` is GOLD, not old HP0. Value `1` is FLOAT, not old PRECISE_Q8. Both voice engines: fixed via `clkdiv_live_total_cycles`, float via `clkdiv_live_hz_total_cycles`. Cmds 32–33 A/B all five live + GOLD_REF. | `voice_task_fixed_point` / `voice_task_float`, [`clkdiv.h`](../clkdiv.h), [`clkdiv_bench.ino`](../clkdiv_bench.ino) |

Float voice / amp / CV are **not** defined by board defaults (enable only in overrides).

### 1.3 Engine — overrides (commented A/B)

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `USE_FLOAT_VOICE_TASK` | off | Compile `voice_task_float` instead of fixed | [`voices.h`](../voices.h) / [`voices.ino`](../voices.ino) |
| `USE_FLOAT_AMP_COMP` | off | Float amp tables + LUT (~42 KB) dual-build | [`amp_comp.h`](../amp_comp.h), [`FS.ino`](../FS.ino) |
| `USE_FLOAT_CV_OUTS` | off | Float VCA/VCF/keytrack/drift path | [`cv_out.ino`](../cv_out.ino), [`cv_state.h`](../cv_state.h) |

Pitch A/B: `#undef PITCH_INTERP_MODE` then redefine. Guard: `FLOAT` / `FLOAT_FAST` without float voice → `#error`.

Math detail: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).

### 1.4 Noise

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `NOISE_ENGINE` | `1` (FastNoiseGen) | Which `DcoNoiseGen` implementation | [`noise.h`](../noise.h), [`DCO_Noise.h`](../_build_libs/DCO_Noise/DCO_Noise.h) |
| `ENABLE_NOISE_OUT` | off | PIO1 LFSR 1-bit white on GP2 | DCO_Noise, [`state_machines.ino`](../state_machines.ino) |

`NOISE_ENGINE` values: `0` ColoredNoise, `1` FastNoiseGen, `2` PrimeHybridNoise, `3` ProNoise32.

### 1.5 Profiling / bench

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `RUNNING_AVERAGE` | **on** | Hot-path profiler (`bench.h`); paced `bench_out_*` | [`bench.h`](../bench.h), benches |
| `RUNNING_AVERAGE_FINE` | off | Extra tiny-stage probes (opt barrier) | `BENCH_FBEGIN` / `FEND` |
| `RUNNING_AVERAGE_PERIOD` | **on** | Period probes only; stage probes compile out; overrides FINE | `bench.h` |
| `BENCH_STAGE_STRIDE` | `9` | MAIN/FINE stage probes every Nth loop (`1` = every iter); note-on always; needs `RUNNING_AVERAGE` | [`bench.h`](../bench.h), [`DCO.ino`](../DCO.ino) |
| `BENCH_USE_SYSTICK` | `1` | SysTick for PERIOD + stages; `0` → 1 µs timer for all probes. Dump window still 1 µs. | [`bench.h`](../bench.h), [`DCO.ino`](../DCO.ino) |
| `BENCH_PERIOD_MAX_US` | `2000` | Discard PERIOD samples longer than this (autotune / wrap-looking stalls) | [`bench.h`](../bench.h), [`DCO.ino`](../DCO.ino) |
| `BENCH_PATH_STATS` | off | All path bumps + `-- Path counters --` dump; needs `RUNNING_AVERAGE`; no-op under PERIOD | [`bench.h`](../bench.h), [`voices.ino`](../voices.ino) |
| `AMP_COMP_BENCHMARK` | off | Amp speed/accuracy cmds 24–25; needs `RUNNING_AVERAGE` + `USE_FLOAT_AMP_COMP` | [`amp_comp_bench.ino`](../amp_comp_bench.ino) |
| `ENABLE_MEM_DIAG` | **on** | Cmd 13 RAM dump + `loop`/`loop1` polls; comment out = empty inlines. Runtime 14/15 disable/enable polls | [`mem_diag.h`](../mem_diag.h), [`MEMORY.md`](MEMORY.md) |

Pitch interp cmds **28–29** and fixed clkdiv cmds **32–33** (`CLKDIV_MODE` A/B) need `RUNNING_AVERAGE` only (no extra flag): [`pitch_interp_bench.ino`](../pitch_interp_bench.ino), [`clkdiv_bench.ino`](../clkdiv_bench.ino).

### 1.6 Board / IO

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `ENABLE_USB_CONTROL` | **on** | Panel protocol on USB CDC (`dco_control`) | [`Serial.h`](../Serial.h) / [`Serial.ino`](../Serial.ino), bench TX |
| `SERIAL_FRAMING_COBS` | **off** | On-wire COBS(`inner`)+`0x00` instead of RAW inner. A/B vs default; host: `dco_control --cobs` or `DCO_SERIAL_COBS=1`. | [`serial_frame.h`](../serial_frame.h) / [`serial_parser.h`](../serial_parser.h), [`DCO.ino`](../DCO.ino) |
| `ENABLE_CV_OUTS` | off | Hardware CV / OSC level PWM writers | [`PWM.ino`](../PWM.ino), [`cv_out.ino`](../cv_out.ino), [`globals.h`](../globals.h) pins |
| `ENABLE_WAVE_MUX` | off | Wave mux GPIO / shift-register path | globals pin block, wave mux |
| `ENABLE_VOICE_AUX` | off | Skip local Dist/filter writers (aux owns them) | [`PWM.ino`](../PWM.ino), globals |
| `ENABLE_PIO_RESET_INVERT` | **on** | Active-low RESET pad via GPIO OVER | [`state_machines.ino`](../state_machines.ino) |
| `ENABLE_SUBOSC_ENGINE2` | **on RP2350**; off / `#error` on RP2040 | Two subs on pio2 SM0/SM1 (`subosc_seg` + DMA, per-sub master select) + boolean combiner on SM3 whose output is what gets mixed; `#undef` to A/B classic pio1 sub | [`subosc.h`](../subosc.h) / [`subosc.ino`](../subosc.ino), [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9, [`PINOUT.md`](PINOUT.md) |
| `RANGE0_PIO_DITHER_TEST` | **on** | All `RANGE_PINS[]` via PIO dither PWM; comment out = HW slice `wrap=DIV_COUNTER` | [`PWM.h`](../PWM.h) / [`PWM.ino`](../PWM.ino), [`autotune.ino`](../autotune.ino), `setup1()` |
| `NOTE_RETRIG_MODE_DEFAULT` | `0` (EXACT_Y) | Note-on sync retrig default; runtime 26/27 | [`globals.h`](../globals.h) |

---

## 2. [`adsr.h`](../adsr.h) → [`ADSR_Bezier.h`](../_build_libs/ADSR_Bezier/ADSR_Bezier.h)

Sketch sets these **before** including the library (library has its own fallbacks).

| Define | DCO shipping | Library fallback if unset | Effect |
|--------|-------------:|---------------------------|--------|
| `ADSR_BEZIER_PHASE_SHIFT` | `22` | `22` | `22` = uint32 phase; `>22` (e.g. 24) = uint64 phase |
| `ADSR_BEZIER_USE_FLOAT` | `0` | `0` | `1` = float time index (soft-float on RP2040) |
| `ADSR_BEZIER_USE_MICROS` | `1` | `1` | `1` micros / `0` millis timebase |
| `ADSR_BEZIER_NATIVE_Q15` | `1` | **`0`** | `1` = primary amp domain Q15; `0` = DAC-primary + optional cache |
| `ADSR_BEZIER_Q15_DYADIC` | `1` | `1` | NATIVE=1: peak 32768 vs 32767 A/B |
| `ADSR_BEZIER_UPDATE_Q15_CACHE` | `1` | `1` | Refresh Q15 in `getWave`; ignored when NATIVE=1 |
| `ADSR_BEZIER_SRAM_HOT` | **`1`** | **`0`** | `1` = RP2040 `__not_in_flash_func` on `getWave` / `noteOn` / `noteOff` (static SRAM, not heap — [`MEMORY.md`](MEMORY.md)) |

Derived: `ADSR_BEZIER_PHASE_SCALE_U64` (`1` when shift > 22).

---

## 3. [`LFO.h`](../LFO.h) → [`mo-lfo.h`](../_build_libs/mo-lfo/mo-lfo.h)

| Define | DCO shipping | Library fallback if unset | Effect |
|--------|-------------:|---------------------------|--------|
| `MO_LFO_USE_Q15` | `1` | **`0`** | Preferred-path hint / pragma only; API always has `getWave` + `getWaveQ15` |
| `MO_LFO_SRAM_HOT` | **`1`** | **`0`** | `1` = RP2040 `__not_in_flash_func` on `getWaveQ15` + `_advanceUnitQ15` (static SRAM — [`MEMORY.md`](MEMORY.md)) |
| `LFO_SINE_TABLE_BITS` | `9` | `9` (in mo-lfo if unset) | Sine LUT size `1<<bits` |

---

## 4. [`globals.h`](../globals.h)

### Feature flags

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `ENABLE_FS_CALIBRATION` | **on** | Load LittleFS `voiceTables` / PW cal into amp-comp | [`FS.ino`](../FS.ino) |
| `NOTE_RETRIG_MODE_DEFAULT` | `0` if unset | Fallback if `DCO.ino` omitted it | `note_retrig_mode` |
| `USE_ADC_STACK_VOICES` | commented | Legacy ADC stack voices (GPIO 28) | unused when commented |
| `USE_ADC_DETUNE` | commented | Legacy ADC detune (GPIO 27) | unused when commented |

Comment-only (not a live define): `ENABLE_PIO_MIDI` — not compiled today. With
`ENABLE_SUBOSC_ENGINE2`, pio1 SM0 is free for it; without engine2, PIO2 was the reserved
block (classic sub occupies pio1 SM0). See [`PINOUT.md`](PINOUT.md) / [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §3.2.

### Capacity constants (not feature toggles)

| Define | Value | Role |
|--------|------:|------|
| `NUM_VOICES_TOTAL` | 3 | Voice-slot capacity (MIDI / ADSR / flags) |
| `NUM_VOICES_VOICE_TASK` | 1 | Legacy mono bind |
| `NUM_OSCILLATORS` | 3 | Physical DCOs |
| `NUM_FILTERS` | 2 (`#ifndef`) | Filter count (also defaulted in `cv_out.h` / `PWM.h` / `cv_state.h`) |
| `MIDI_CHANNEL` | 1 | Default MIDI channel |

---

## 5. Other live headers

| File | Define | Shipping | Effect |
|------|--------|----------|--------|
| [`noise.h`](../noise.h) | `NOISE_ENGINE` | fallback `0` if sketch omit | DCO sets `1` in `DCO.ino` |
| [`bench.h`](../bench.h) | `BENCH_USE_SYSTICK` / `BENCH_PERIOD_MAX_US` | sketch wins | Header `#ifndef` fallbacks only if unset in [`DCO.ino`](../DCO.ino) |
| [`amp_comp.h`](../amp_comp.h) | `AMP_COMP_METHOD_DEFAULT` | fallback `AMP_COMP_FIXED` | Only if unset before include |
| [`voices.ino`](../voices.ino) | `DCO_DEBUG_REPORT` | `0` | `1` = serial dump of OSC1 frequency stages |

Headers that only **consume** flags (no new feature `#define`): `voices.h`, `cv_state.h`, `cv_out.h`, `PWM.h`, `Serial.h`, `FS.h`, `mod_matrix.h`, `include_all.h`, etc.

---

## 6. Vendored library notes

| Library | Flag interaction |
|---------|------------------|
| ADSR_Bezier | Defaults differ on `NATIVE_Q15` (`0` in lib vs `1` in DCO) and `SRAM_HOT` (`0` vs `1`). Always set via [`adsr.h`](../adsr.h). |
| mo-lfo | `MO_LFO_USE_Q15` / `MO_LFO_SRAM_HOT` lib default `0`; DCO forces `1` in [`LFO.h`](../LFO.h). |
| DCO_Noise | Reads `NOISE_ENGINE` / `ENABLE_NOISE_OUT`; fallback engine `0`. |

---

## 7. Where to change what

| Goal | Where |
|------|--------|
| Float/fixed voice, pitch, amp, CV, HP clkdiv | `DCO.ino` **ENGINE — overrides** (and board defaults for MCU policy) |
| Noise engine / PIO white pin | `DCO.ino` noise block |
| Profiler on/off / period-only | `DCO.ino` profiling block |
| USB panel / CV HW / aux / RESET invert | `DCO.ino` BOARD / IO |
| ADSR phase / native Q15 / SRAM hot A/B | [`adsr.h`](../adsr.h) before library include |
| LFO Q15 hint / SRAM hot / sine table bits | [`LFO.h`](../LFO.h) |
| Heap / stack / RAM-text dump | Diagnostics cmd **13**; polls flag `ENABLE_MEM_DIAG` + runtime 14/15 — [`MEMORY.md`](MEMORY.md) |
| FS cal load | [`globals.h`](../globals.h) `ENABLE_FS_CALIBRATION` |
| OSC1 debug prints | [`voices.ino`](../voices.ino) `DCO_DEBUG_REPORT` |

After flag changes: clean rebuild; confirm LittleFS amp-comp load; listen low notes + amp plateau; optional profiler dump (cmd **10**) — see [`BENCHMARKING.md`](BENCHMARKING.md).
