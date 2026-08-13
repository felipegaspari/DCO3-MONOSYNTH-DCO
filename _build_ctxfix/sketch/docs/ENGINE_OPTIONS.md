#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/ENGINE_OPTIONS.md"
# DCO Engine Options (Float vs Fixed-Point)

This firmware can run the real-time voice engine in **float** or **fixed-point** form. The goal is **maximum real-time speed without losing pitch / amplitude precision**. Which path you get depends on compile-time flags at the top of [`DCO.ino`](DCO.ino).

**Complete flag catalog** (engine + noise + profiler + board IO + ADSR/LFO/lib): [`BUILD_FLAGS.md`](BUILD_FLAGS.md).  
**Live source of truth for engine flags:** the top of [`DCO.ino`](../DCO.ino) before includes — **pitch mode ids**, **board defaults** (including `PITCH_INTERP_MODE`), **overrides**, **guards**, then profiling / board IO.  
There is **no** `USE_FLOAT_ENGINE` umbrella; voice and amp are separate compile flags. Pitch A/B overrides use `#undef PITCH_INTERP_MODE` then `#define` (default is already set with board defaults).

---

## 1. Purpose and MCU guidance

| Target | Board defaults (`PICO_RP2350` / else) | Why |
|--------|--------------------------------------|-----|
| **RP2350** | `USE_FLOAT_VOICE_TASK` + `USE_FLOAT_AMP_COMP`; `PITCH_INTERP_FLOAT_FAST`; amp method `FLOAT_QUAD` (0); `CLKDIV_FLOAT` (override `CLKDIV_Q16` for integer clkdiv on float voice) | FPU: float voice + float amp dual-build; FLOAT_FAST trunc±1 find |
| **RP2040** (no FPU) | No float voice/amp/CV flags; `PITCH_INTERP_RATIO_Q16`; amp method `FIXED`; HP clkdiv `1` | Soft-float is expensive; lean Q8 amp; 64-bit clkdiv (override `0` for fast Q4); **fixed** `update_CV_outs` |

Overrides (after board defaults) can `#undef` / `#define` those flags. See the commented examples in `DCO.ino`.

---

## 2. Quick-pick flag sets

Normally **do nothing** — board defaults apply. To force behaviour, use the **ENGINE — overrides** block in `DCO.ino`.

| Goal | What to set (overrides) |
|------|-------------------------|
| **Stock** | (leave overrides commented — RP2350 float+FLOAT_FAST+FLOAT_QUAD; RP2040 fixed+RATIO_Q16+FIXED) |
| **Float voice A/B** | `#define USE_FLOAT_VOICE_TASK` (+ pitch `FLOAT` / `FLOAT_FAST` if desired; FPU preferred) |
| **Float amp A/B** | `#define USE_FLOAT_AMP_COMP` (large LUT RAM; pick method via `AMP_COMP_METHOD_DEFAULT` / cmds 20–22) |
| **Float CV outs A/B** | `#define USE_FLOAT_CV_OUTS` (soft-float tax on RP2040; expect worse Core1) |
| **Speed clkdiv (fixed voice)** | `#define CLKDIV_MODE CLKDIV_FAST_Q4` or `CLKDIV_Q8` (faster than Q16); `CLKDIV_GOLD` / `CLKDIV_FLOAT` for A/B |
| **Pitch interp A/B** | `#undef PITCH_INTERP_MODE` then `#define PITCH_INTERP_MODE PITCH_INTERP_FLOAT` (walk) / `FLOAT_FAST` / `RATIO_Q16` / `Q12` |

After changing flags: clean rebuild, confirm LittleFS amp-comp tables still load, listen to low notes and amp plateau behaviour.

---

## 3. Engine flags

`DCO.ino` layout:

```text
ENGINE — pitch mode ids     // FLOAT / FLOAT_FAST / RATIO_Q16 / Q12
ENGINE — board defaults     // per-MCU: voice/amp/pitch/clkdiv/amp-method (pitch inside each branch)
ENGINE — overrides          // #undef / #define to force (pitch needs #undef first)
ENGINE — guards             // FLOAT / FLOAT_FAST require float voice
PROFILING / BOARD / IO
NOTE_RETRIG_MODE_DEFAULT
```

| Define | Effect |
|--------|--------|
| `USE_FLOAT_VOICE_TASK` | Compiles `voice_task_float()`; omits fixed `voice_task_fixed_point()`. Float portamento in `voices.h`. **Default on RP2350**, off RP2040. |
| `PITCH_INTERP_MODE` | Pitch table path (ids above). Board default: **`FLOAT_FAST` on RP2350**, **`RATIO_Q16` on RP2040**. |
| `USE_FLOAT_AMP_COMP` | **Compile-time** float amp dual-build: Hz tables, LUT (`NUM_OSCILLATORS × 7001 × 2` bytes; ~42 KB at 3 osc), float precompute + Q8 seed. Not the same as method (see §7). **Default on RP2350**, off RP2040. |
| `USE_FLOAT_CV_OUTS` | Float VCA/VCF/keytrack/drift/velocity math in `update_CV_outs`. Off → Q15/integer path. **Always-on** (both builds): Q15 mod matrix, `lerp>>12`, `note-60` keytrack, PWM wrap LUT. **Default off** on both MCUs (enable only for A/B). |
| `AMP_COMP_METHOD_DEFAULT` | Live method when float amp is built: `0 FLOAT_QUAD` / `1 LUT` / `2 FIXED` (runtime cmds 20–22). Board default: **`0 FLOAT_QUAD` on RP2350**, **`2 FIXED` on RP2040**. |
| `CLKDIV_MODE` | Clkdiv (accuracy order): `0` GOLD (double `llround`); `1` FLOAT (Q24 → float Hz / native Hz on float voice); `2` Q16 (Q16 Hz → 64/32, shipping); `3` Q8 (Q8 32/32+corr); `4` FAST_Q4 (~1 µs). Value `0` is GOLD, not old HP0. Value `1` is FLOAT, not old PRECISE_Q8. Both engines: fixed `clkdiv_live_total_cycles`, float `clkdiv_live_hz_total_cycles`. Board default **Q16** on RP2040, **FLOAT** on RP2350. |

**Voice vs amp:** independent. Float voice with `#undef USE_FLOAT_AMP_COMP` uses lean Q8 amp via `get_chan_level_for_engine`. Fixed voice with float amp is unusual (extra RAM).

**CV outs:** independent of voice/amp. On RP2040, leave `USE_FLOAT_CV_OUTS` undefined — Core1 already runs fixed voice; float CV math would reintroduce `__aeabi_*` soft-float into the ~10 kHz path. Bench banner prints `cv=FLOAT|FIXED`.

### Dispatch

```text
loop1() → voice_task_main()
            ├─ USE_FLOAT_VOICE_TASK → voice_task_float()
            └─ else                 → voice_task_fixed_point()
```

```mermaid
flowchart TD
  loop1["loop1()"] --> main["voice_task_main()"]
  main -->|USE_FLOAT_VOICE_TASK| vf["voice_task_float()"]
  main -->|else| vx["voice_task_fixed_point()"]
  vf --> fPitch["float modifiers + FLOAT_FAST (RP2350) or FLOAT / RATIO / Q12"]
  vf --> fClk["clkdiv_live_hz_total_cycles"]
  vf --> fAmp["get_chan_level_for_engine → get_chan_level_float"]
  vx --> xPitch["Q24 modifiers + RATIO_Q16 (RP2040) or Q12"]
  vx --> xClk["CLKDIV_MODE GOLD / FLOAT / Q16 / Q8 / FAST_Q4"]
  vx --> xAmp["get_chan_level_lookup_fast Q8"]
```

Related (not engine math, but often used together):

| Define | Default (see [`BUILD_FLAGS.md`](BUILD_FLAGS.md)) | Role |
|--------|---------|------|
| `RUNNING_AVERAGE` | on in tree | Cycle-accurate hot-path profiler (`bench.h`) — see [`BENCHMARKING.md`](BENCHMARKING.md) |
| `RUNNING_AVERAGE_FINE` | off | Adds probes on the smallest stages; needs `RUNNING_AVERAGE` |
| `RUNNING_AVERAGE_PERIOD` | on in tree | Loop periods only (no stage probes); needs `RUNNING_AVERAGE`; overrides FINE |
| `BENCH_STAGE_STRIDE` | `9` | Stage probes every Nth loop; note-on always; `1` = every-iter MAIN |
| `BENCH_USE_SYSTICK` | `1` | SysTick for PERIOD + stages; `0` = 1 µs timer for all probes |
| `BENCH_PERIOD_MAX_US` | `2000` | Discard PERIOD samples longer than this (autotune / wrap-looking stalls) |
| `BENCH_PATH_STATS` | off | All path bumps + `-- Path counters --` dump; needs `RUNNING_AVERAGE`; no-op under PERIOD |
| `DCO_DEBUG_REPORT` | `0` in `voices.ino` | Serial dump of OSC1 frequency stages |
| `ENABLE_FS_CALIBRATION` | on in `globals.h` | Load LittleFS voiceTables / PW cal into amp-comp arrays |

---

## 4. Pitch interpolation (`PITCH_INTERP_MODE`)

One compile-time enum selects **exactly one** interpolator and allocates **only that mode’s** slope / table storage. No runtime branch on the hot path. Live call sites use **`interpolate_live_ratio_q16`** (fixed voice) / **`interpolate_live_ratio_f`** (float voice) — `static inline` wrappers with `#if PITCH_INTERP_MODE` inside (not function pointers). Named interpolators stay for cmds 28/29.

Defined in `DCO.ino`:

| Mode | Value | Hot-path function | Storage | Engine |
|------|-------|-------------------|---------|--------|
| `PITCH_INTERP_FLOAT` | 0 | `interpolateRatioFloat_cached` (walk+bsearch) | `x/yMultiplierTableF`, `slopeF` | Float voice **required**; walk A/B |
| `PITCH_INTERP_RATIO_Q16` | 1 | `interpolateRatioQ16_cached` (trunc±1) | int `x/y` **native Q16**, **`slopeQ16`** | RP2040 default; float voice A/B |
| `PITCH_INTERP_Q12` | 2 | `interpolatePitchMultiplierIntQ16_cached` + reciprocal | int tables ×10000, `slopeQ12` | Slope A/B only |
| `PITCH_INTERP_FLOAT_FAST` | 3 | `interpolateRatioFloat_cached_fast` (trunc+clamp±1, `noinline`) | same float tables as FLOAT | **RP2350 default**; float voice required |

There is **no** selectable `PITCH_INTERP_Q20` or `PITCH_INTERP_Q8`. Live `RATIO_Q16` uses **Q16 slopes** (32-bit lerp when the stock table fits). Accuracy bench cmd 29 may still use a private higher-res reference.

**Defaults** (set inside each MCU branch in **ENGINE — board defaults**, `#ifndef`):

| Board | Default mode |
|--------|----------------|
| `PICO_RP2350` | `PITCH_INTERP_FLOAT_FAST` (float tables, trunc±1, `noinline`) |
| else (RP2040 / fallback) | `PITCH_INTERP_RATIO_Q16` (native Q16 x/y, `slopeQ16`, trunc±1) |

**Guards:** `FLOAT` / `FLOAT_FAST` without float voice → `#error`. Float voice may use any mode via **`interpolate_live_ratio_f`** (fixed interpolators convert scaled `float x → Q16` then back to a float ratio for A/B).

**Float domain (`FLOAT` / `FLOAT_FAST`):** tables store natural modifier `x ∈ [-1, 3]` and frequency ratio `y` directly. Call site passes `freqModifiers` with **no** `×10000`; interpolator returns the ratio (no `/scale`). Same storage for both; only the segment find differs. The scale exists only for int tables.

**Fixed-voice flow** (`!USE_FLOAT_VOICE_TASK`):

1. Portamento → current frequency in **Q24 Hz** (`Hz × 2^24`).
2. Sum pitch modifiers in **Q24** (pitch bend, LFO, unison, ADSR→detune, drift, matrix, epsilon ≈ 1.00001).
3. **`modifiers_q24_to_xQ16`:** `RATIO_Q16` = `trunc(modifiers_q24 / 2^8)` (native domain; **no** ×10000). **`Q12` only:** scale × `multiplierTableScale` (10000) into legacy table-units Q16.
4. **`interpolate_live_ratio_q16`** → **ratio Q16** (`RATIO_Q16` table `y` is already frequency ratio; `Q12` returns table `y` then reciprocal-multiply ÷10000).
5. `freq_q24 = (portamento_cur_freq_q24 * ratioQ16) >> 16` (OSC2/3 also fold detune into the Q16 factor).

| Mode | Slope / precision notes |
|------|-------------------------|
| `RATIO_Q16` | RP2040 production: native Q16 x/y, `slopeQ16` lerp, trunc±1 |
| `Q12` | Legacy ×10000 tables; return `y` then reciprocal |

Override examples (float voice): `#undef PITCH_INTERP_MODE` then `#define PITCH_INTERP_MODE PITCH_INTERP_FLOAT` (walk A/B) or `PITCH_INTERP_RATIO_Q16`. Float porta / clkdiv stay float; only the table interpolator changes.

Speed/accuracy one-shots for FLOAT / FLOAT_FAST / RATIO / Q12 (private tables in `pitch_interp_bench.ino`): debug **28/29** with `RUNNING_AVERAGE` — see [BENCHMARKING.md](BENCHMARKING.md) §9. Cmd **28** prints **seq** + **jump** speed tables. Private rows always include both float finds; `live_pitch=` in the header is the compiled `PITCH_INTERP_MODE`.

---

## 5. Fixed-point clock divider

Active in both voice engines via `CLKDIV_MODE` (board default **`CLKDIV_Q16`** on RP2040, **`CLKDIV_FLOAT`** on RP2350). Override **`CLKDIV_FAST_Q4`** / **`CLKDIV_Q8`** for speed, **`CLKDIV_GOLD`** / **`CLKDIV_FLOAT`** / **`CLKDIV_Q16`** for A/B.

| Value | Math | Notes |
|-------|------|-------|
| **0 GOLD** | Q24 → double Hz → `llround(sys / hz)` | Gold standard / A/B. Soft-double on M0+ and RP2350. |
| **1 FLOAT** | Q24 → float Hz → `fminf(sys/hz + 0.5)` | Same math as float voice (`clkdiv_float_hz_total_cycles`). Soft-float on M0+. |
| **2 Q16** | Q16 Hz denom → 64/32: `freq_q16 = (freq_q24 + 1<<7) >> 8`, then `(sys<<16 + q16/2) / q16` | ±1/65536 Hz vs full Q24. Shipping. |
| **3 Q8** | Q8 Hz; `q = sys/f_int` then remainder correction (two 32/32). `f_int < 16` falls back to internal `precise_q8` (64/32, not a `CLKDIV_MODE`). | Faster than Q16 on M0+; low-Hz tail ~3¢ @ ~1 Hz. |
| **4 FAST_Q4** | Round to **Q4 Hz** (`(freq_q24 + 1<<19) >> 20`), then 32-bit `(sys*16 + Q4/2) / Q4` | Fastest; ~**1 µs/voice** — aggressive modulation / slower MCUs. Old HP0. |

Fixed `vt_clk_div` calls `clkdiv_live_total_cycles` (Q24; compile-time alias). Float `vt_clk_div` calls `clkdiv_live_hz_total_cycles` (native Hz; FLOAT/GOLD stay Hz, else Hz→Q24 then the Q24 alias). All five then add a measured correction (currently 0) and `pio_clk_div_for_y` (Y locked every frame). Note-on uses `pio_period_split` (can rewrite Y).

Speed/accuracy one-shots (debug **32/33**, `RUNNING_AVERAGE`): all six methods on **both** voice engines. Glue matches live (fixed = Q24 helpers; float = native Hz for GOLD/FLOAT, Hz→Q24 then helper for integer). Speed **`pctVsGOLD_REF`**. See [BENCHMARKING.md](BENCHMARKING.md) §10.

---

## 6. Float engine pipeline

Active when `USE_FLOAT_VOICE_TASK` is defined.

| Stage | Behaviour |
|-------|-----------|
| Portamento | Separate float state in `voices.h` (`porta_*_f`): TIME = linear Hz; SLEW = linear semitones via `noteIndex_to_freqFloat` |
| Pitch bend / LFO / ADSR / drift | Computed in float; many depths still arrive as Q24 from core 0 / params and are scaled by `1/2^24` each frame |
| Multiplier table | Board default `interpolateRatioFloat_cached_fast` (`FLOAT_FAST`). Override to `FLOAT` (walk), `RATIO_Q16` (native Q16), or `Q12` (×10000 glue). |
| Clkdiv | `clkdiv_live_hz_total_cycles` (`CLKDIV_MODE`; Q16/Q8/FAST_Q4 convert Hz→Q24), then OSR / phase math |
| Amp-comp | `get_chan_level_for_engine(freqHz, dco)` → `get_chan_level_float` when float amp-comp is on |
| PW | Float intermediates, then integer `get_PW_level_interpolated` (shared) |

**A/B:** under float voice, non-`FLOAT` modes keep the float voice shell and swap only the interpolator. The float→Q16 convert tax is part of those candidates; native `FLOAT` stays convert-free.

There are **no** additional float precision `#define`s beyond voice/amp flags and `PITCH_INTERP_MODE`.

---

## 7. Amplitude compensation

Gated by **compile-time** `USE_FLOAT_AMP_COMP` (**default on RP2350**, off RP2040). That flag **builds** the float amp stack; `AMP_COMP_METHOD_*` only **picks** among methods when the stack is built.

Flash format is shared: frequencies stored as **`freq × 100`** (`freq_x100`). Runtime representation diverges after `init_FS()`.

| Mode | After FS load | Precompute | Runtime lookup |
|------|---------------|------------|----------------|
| **Fixed** (`!USE_FLOAT_AMP_COMP`) | `ampCompFrequencyArray` in **Q8 Hz** (`FREQ_FRAC_BITS = 8`), `ampCompArray` as `int32_t` | `precomputeCoefficients()` — `FixedQuadWindow` with `T_FRAC = 12` | `get_chan_level_lookup_fast(xQ8, voiceN)` |
| **Float** (`USE_FLOAT_AMP_COMP`) | `ampCompFrequencyHz` as `float`, shared `ampCompArray` as `int32_t` | Float quadratic + dense LUT fill; also seed Q8 and run fixed precompute for `FIXED` | Selected by `amp_comp_method` (see below) |

Shared constants: `ampCompTableSize = 22`, `AMP_COMP_MAX_HZ = 7000`, plateau metadata per oscillator, shared `ampCompArray` (`int32_t`). Per-window coeffs are `FixedQuadWindow fixedWin[][]` and `FloatQuadCoeffs floatCoeffs[][]` (array-of-structs, same layout on DCO3 and DCO4).

**Domain-max early-out:** float quadratic and FIXED both return `DIV_COUNTER` at `freq >= AMP_COMP_MAX_HZ` / `x >= AMP_COMP_MAX_HZ_Q` (cal sentinel = full RANGE). Prevents a window-scan miss at exact max from falling through to window 0.

**Plateau early-out:** float / LUT use `plateauStartIndex >= 0` plus `plateauStartFreqHz`. FIXED (`get_chan_level_lookup_fast`) does **not** use the shared index after dual-build; validity is `plateauStartFreqQ < AMP_COMP_MAX_HZ_Q` (precompute leaves Q at max when no real plateau was found — same role as `index < 0`).

`precompute_amp_comp_for_engine()` runs the active precompute(s) at startup. Under float amp-comp it also builds `ampCompLut[osc][0..7000]` (`NUM_OSCILLATORS × 7001 × 2` bytes; ~42 KB at 3 osc) from `get_chan_level_float_quad` and keeps fixed Q8 tables for A/B.

### Live methods (`USE_FLOAT_AMP_COMP`)

| Id | Name | Behaviour |
|----|------|-----------|
| 0 | `FLOAT_QUAD` | Cached walk + `y = (a*x+b)*x+c` (needs `USE_FLOAT_AMP_COMP`) |
| 1 | `LUT` | Nearest Hz → `ampCompLut` index (speed A/B only; needs float amp) |
| 2 | `FIXED` | Q8 `get_chan_level_lookup_fast` (RP2040 default; only path without float amp) |

- **Compile-time default:** board defaults in [`DCO.ino`](../DCO.ino) — **RP2350 → FLOAT_QUAD (`0`)**, **RP2040 → FIXED (`2`)**. Override with `#define AMP_COMP_METHOD_DEFAULT` in **ENGINE — overrides** (ids 0/1 need `USE_FLOAT_AMP_COMP`).
- **Runtime:** `PARAM_DEBUG_COMMAND` values **20–22** (`amp_comp_set_method`). Profiler dump (10) appends `amp_comp method=…`.
- Facade: `get_chan_level_for_engine` / `get_chan_level_float` dispatch on `amp_comp_method`. Fixed voice: `amp_level_q24` / `amp_chan_levels_fixed` — shipping stays Q24→Q8→`lookup_fast`; with `USE_FLOAT_AMP_COMP`, non-FIXED methods use Q24→Hz then `get_chan_level_by_method`.
- **Speed order:** on RP2350 (FPU) typically **LUT ≪ FLOAT_QUAD ≲ FIXED**. On RP2040 soft-float, FLOAT_QUAD is usually slowest. See [BENCHMARKING.md](BENCHMARKING.md) §8.

Without `USE_FLOAT_AMP_COMP`, only FIXED exists; method selects collapse to FIXED.

Float `voice_task_float` and autotune use `get_chan_level_for_engine`.

Speed/accuracy one-shots: `#define AMP_COMP_BENCHMARK` + `RUNNING_AVERAGE`, debug **24/25** — see [BENCHMARKING.md](BENCHMARKING.md) §8.

---

## 8. Formats cheat-sheet

| Format | Representation | Typical use |
|--------|----------------|-------------|
| float Hz | `float` | Float voice path; float amp-comp; `sNotePitches[]` |
| Q24 Hz | `int64_t` / `int32_t`, `Hz × 2^24` | Fixed portamento / final freq; LFO/ADSR/pitchbend depths |
| Q16 semitone | `int32_t`, note × `2^16` | Fixed slew portamento |
| Q16 table / ratio | `int32_t` | Pitch table `x` and frequency ratio (`RATIO_Q16` native; `Q12` after `/10000`) |
| Q4 Hz | `uint32_t`, `Hz × 16` | Fast fixed clkdiv (`CLKDIV_FAST_Q4`) |
| Q16 Hz | `uint32_t`, `Hz × 65536` | Fixed clkdiv denom (`CLKDIV_Q16`) |
| Q8 Hz | `int32_t`, `Hz × 256` | Fixed amp-comp frequency domain; Q8 clkdiv denom (`CLKDIV_Q8` + internal precise_q8 fallback) |
| Q12 `t` | `T_FRAC = 12` | Fixed amp-comp quadratic parameter |
| Q16 / Q12 slopes | `slopeQ16` (live RATIO) / `slopeQ12` (Q12 A/B) | Multiplier table interpolation. Cmd 29 may use a private Q20 lerp on native Q16 knots (not a live mode). |
| Q28 reciprocal | `FixedQuadWindow.invDx_q28` | Fixed amp-comp `1/dx` |
| `freq_x100` | `int32_t` | FS / calibration storage |
| Table scale | `multiplierTableScale = 10000` | Int pitch tables only (`FLOAT` is unscaled) |

System clock used by clkdiv: `sysClock_Hz` from Arduino `F_CPU`, then `clock_get_hz(clk_sys)` at boot (`sys_clock_hz_refresh()` in `globals.h`).

---

## 9. Shared behaviour (both engines)

- **LFO pitch mods:** Core 0 writes `lfo1_pitch_mod_q24[]` / `lfo2_pitch_mod_q24[]` every ~50 µs; core 1 reads volatiles in the voice task (float path converts each frame).
- **PW PWM:** both engines end in `get_PW_level_interpolated` (integer map using calibrated center/limits).
- **Autotune:** `voice_task_autotune()` uses float-style clkdiv math and `get_chan_level_for_engine`; it is compiled regardless of engine and is not the production note path.
- **Legacy helpers:** `voice_task_simple()` / `voice_task_debug()` / gold reference are **removed** (see `_removed/` if needed).

---

## 10. Known traps

1. Voice / amp / pitch / HP / amp-method defaults are set **inside** each MCU branch in board defaults; force changes in **overrides** (`#undef` then `#define` when replacing a default).
2. Under float voice, `RATIO_Q16` / `Q12` are valid A/B overrides; they allocate int tables (not `slopeF`).
3. Only the active mode’s slope array is allocated — no inert `slopeQ12` RAM on the float / `RATIO_Q16` defaults.
4. `USE_FLOAT_AMP_COMP` without float methods you need still costs LUT RAM; `#undef` it for lean Q8-only amp.
5. README / older docs that mention `USE_FLOAT_ENGINE` / `PITCH_USE_RATIO_Q16` / `PITCH_INTERP_USE_Q*` are obsolete.
6. Stale “Q18 frequency” / `T_FRAC is 14` remarks in places — trust `T_FRAC = 12` in `amp_comp.h`.

---

## 11. Change checklist

1. Prefer **ENGINE — overrides** in `DCO.ino` (before includes); leave board defaults alone unless changing MCU policy.
2. Full rebuild (both cores / clean if IDE caches oddly).
3. Confirm `ENABLE_FS_CALIBRATION` load path matches amp-comp mode (no assert / empty tables).
4. Play low and high notes; check for zippering, beating, or amp dropouts at plateau.
5. Optional: enable `RUNNING_AVERAGE`, send debug command 10, and compare the `%win` budget before and after the flag change ([`BENCHMARKING.md`](BENCHMARKING.md)).

---

## 12. Where the code lives

| Concern | Files |
|---------|-------|
| Flag definitions | `DCO.ino` |
| Dispatch + both voice tasks | `voices.ino` / `voices.h` |
| Amp-comp dual path | `amp_comp.h`, `FS.ino` |
| Note tables | `noteList.h` (`sNotePitches`, `sNotePitches_q24`) |
| Loop call site | `DCO.ino` `loop1()` → `voice_task_main()` |
