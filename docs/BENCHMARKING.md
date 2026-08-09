# Hot-path benchmarking

How to measure where the DCO's realtime budget goes, and how to read the numbers without
fooling yourself.

- Flags: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- Code: [`../bench.h`](../bench.h), probes in [`../DCO.ino`](../DCO.ino) and [`../voices.ino`](../voices.ino)

---

## 1. Turning it on

Uncomment in [`../DCO.ino`](../DCO.ino):

```c
#define RUNNING_AVERAGE         // profiler on (main stage probes)
#define RUNNING_AVERAGE_FINE    // plus the tiny per-stage probes
#define RUNNING_AVERAGE_PERIOD  // only loop / loop1 periods (no stage probes)
#define BENCH_PATH_STATS        // all path bumps + -- Path counters -- dump
#ifndef BENCH_STAGE_STRIDE
#define BENCH_STAGE_STRIDE 9    // MAIN/FINE every Nth loop; 1 = every iter
#endif
#ifndef BENCH_USE_SYSTICK
#define BENCH_USE_SYSTICK 1     // PERIOD + stages on SysTick; 0 = 1 us timer
#endif
#ifndef BENCH_PERIOD_MAX_US
#define BENCH_PERIOD_MAX_US 2000  // discard PERIOD samples longer than this
#endif
```

With `RUNNING_AVERAGE` off, every `BENCH_*` macro expands to nothing. There is no runtime
cost and no storage in the shipping build.

**`RUNNING_AVERAGE_PERIOD`** (needs `RUNNING_AVERAGE`): stage `BENCH_BEGIN`/`END` compile
out; only `BENCH_PERIOD` for `loop period` / `loop1 period` collects. Path counters
(`amp:` / `ratio:` / `porta:`) are also disabled — no bumps and no dump block. Use this for
a true loop-time baseline without intermediate probe tax. The dump banner says `period only`.
Do not combine with FINE expecting stage rows — PERIOD wins and FINE is ignored.

**`BENCH_PATH_STATS`** (needs `RUNNING_AVERAGE`): all `BENCH_PATH_INC` bumps (amp / ratio /
porta) plus walk-step sums and the dump `-- Path counters --` block (`ratio:` / `amp:` /
`porta:`). Leave off for shipping — dump 10 then has no path section. Still no-op under
`RUNNING_AVERAGE_PERIOD`. Banner `bench:` includes `path_stats=0/1`.

**`BENCH_STAGE_STRIDE`** (default **9**, needs `RUNNING_AVERAGE`, ignored under PERIOD):
`BENCH_PERIOD` still fires every `loop` / `loop1`. MAIN/FINE `BENCH_BEGIN`/`END` run only every
Nth iteration; dump **scales `sum`/`n` ×N** so child **mean** and **`%win`** match a full-rate
dump. **min/max** are from sampled iters only. Note-on family (`phase align`, `retrig period
split`, `note-on retrigger`, `retrig SM apply`, `retrig RANGE PWM`) is **`BENCH_T_RARE`** —
always recorded, not scaled. Banner: `stages every 9`. Set stride to **1** for old every-iter
MAIN (high tax).

**`BENCH_USE_SYSTICK`** (default **1**, sketch + `bench.h` fallback): PERIOD and stages share
SysTick. `0` = 1 µs timer for **all** probes. Dump window (1 s gate) always uses
`bench_us_now()`.

**`BENCH_PERIOD_MAX_US`** (default **2000**): discard a PERIOD sample longer than this
(autotune / wrap-looking stalls). Still every-iter PERIOD vs stride-9 stages.

**Period-only vs full MAIN.** Same preset, same play: compare `loop` / `loop1` mean **and
max**. Full MAIN is slower than period-only because of instrumentation, not synth speed
(shipping RP2040 @ 240 MHz Q16: loop1 **25.90 µs** stride-9 MAIN vs **17.47 µs** period-only,
≈ **+8.4 µs** dump-10 tax). Do not mix the two modes.

1. Each child's `BENCH_END` bookkeeping sits in the **parent** `(unattributed)` (~18 MAIN
   kids under `voice_task`). Overhead subtract is only ~2 SysTick cyc.
2. `volatile` BEGIN barriers change codegen.
3. Slower loop1 makes ~99 µs ADSR/CV fire on more iterations.

Use **period-only** for “is the synth faster?”. Use MAIN **`%win` / child means** to rank
where time goes — never compare full vs period absolute loop means. Stride 9 still carries
probe tax (here +8.4 µs); it is not “period-only + 1/9”.

### Shipping RP2040 Q16 baseline

Same machine / play (pitch-mod + porta on): `clk_sys` **240 MHz**,
`mcu=RP2040` `voice=FIXED` `pitch=RATIO_Q16` `clkdiv=Q16` `amp=FIXED` `cv=FIXED`
`amp_method=FIXED` `note_retrig=EXACT_Y`, `sram_hot=1` (ADSR + LFO), `path_stats=0`,
`cv_outs=0`.

**Period-only** (`RUNNING_AVERAGE_PERIOD`):

| Probe | mean | min | max |
|-------|------|-----|-----|
| Core0 `loop period` | **8.79** | 5.80 | **72.18** |
| Core1 `loop1 period` | **17.47** | 14.75 | **71.13** |

**Full MAIN** (`RUNNING_AVERAGE`, `BENCH_STAGE_STRIDE` 9, banner `stages every 9`):

| Probe | mean | min | max | %win |
|-------|------|-----|-----|------|
| Core0 `loop period` | **10.61** | 7.29 | 62.00 | 99.97 |
| Core0 `MIDI read` | 3.20 | | | **30.15** |
| Core1 `loop1 period` | **25.90** | 19.86 | 99.13 | 99.98 |
| `voice_task TOTAL` | 26.21 | 21.28 | 79.42 | 101.18 |
| `amp comp` | 4.73 | | | **18.25** |
| `clkdiv math` | 4.00 | | | **15.42** |
| `portamento` | 2.02 | | | 7.80 |
| `ratio interpolate` | 1.74 | | | 6.69 |
| `ADSR_update` | 2.63 | | | 2.61 |
| `update_CV_outs` | 126c | | | **0.52** |

`voice_task` `%win` > 100 and loop1 `(over-attributed)` (−11.84%) is stride-9 bookkeeping,
not extra DSP. Re-capture after engine/flag changes; keep the dump tree in §3 as shape only.

Reports go out the USB CDC port. They are **off by default even with profiling compiled in**
— ask for them with a debug command.

## 2. Getting a report

On the **Diagnostics** tab of [`../tools/dco_control`](../tools/dco_control/README.md), the
**Hot-path profiler** buttons send `PARAM_DEBUG_COMMAND` (id 160):

| Button | Value | Effect |
|--------|-------|--------|
| Dump profiler once | `10` | Dump once |
| Reset profiler | `11` | Reset all accumulators |
| Toggle ~1 Hz dump | `12` | Toggle the ~1 Hz automatic dump |

A dump is asynchronous. Setting the request flag makes each core snapshot and clear its own
probes at its next loop iteration; core 0 then **formats** the report into a RAM buffer and
drains it over USB in small chunks across later `loop()` turns. **Core 1 never prints.**
The tables land in the tool's Board output pane.

While that paced TX is active, both cores' `BENCH_PERIOD` probes **and** stage cycle probes
skip samples so dump traffic cannot inflate period or stage `max`. Path counters pause too.
After each snapshot/reset, period probes invalidate their previous timestamp so the first
sample of the new window cannot straddle the old one.

One-shot and periodic dumps wait until the collection window is **≥ 1 s** since the last
reset or dump (`BENCH_MIN_WINDOW_US`). Requesting a dump earlier just keeps sampling until
that elapses — the report then shows a ~1000 ms window. Amp-comp method acks still print
immediately (they do not wait on this gate).

## 3. Reading the output

The banner prints grouped flag lines (compile-time + live selectors) so rebuilds / method
switches are obvious:

```
=================== DCO BENCH ===================
clk_sys 250 MHz   probe overhead 2 cyc   stages every 9
engine: mcu=RP2040 voice=FIXED pitch=RATIO_Q16 amp=FIXED cv=FIXED amp_method=FIXED clkdiv=Q16 note_retrig=EXACT_Y
adsr:   phase=22 float=0 micros=1 native_q15=1 dyadic=1 q15_cache=1 sram_hot=1
lfo:    sram_hot=1
noise:  engine=1 out=0
board:  cv_outs=0 wave_mux=0 voice_aux=0 pio_rst_inv=1 fs_cal=1
bench:  amp_comp=0 path_stats=0
```

| Line | Fields |
|------|--------|
| `engine:` | `mcu` (board package); `voice` / `amp` / `cv` (`USE_FLOAT_VOICE_TASK` / `USE_FLOAT_AMP_COMP` / `USE_FLOAT_CV_OUTS`); `pitch` (`PITCH_INTERP_MODE`); `amp_method` (live); `clkdiv` (`CLKDIV_MODE` → `GOLD`/`FLOAT`/`Q16`/`Q8`/`FAST_Q4`; both voice engines); `note_retrig` (live) |
| `adsr:` | `ADSR_BEZIER_*` from [`../adsr.h`](../adsr.h): `phase`, `float`, `micros`, `native_q15`, `dyadic`, `q15_cache`, `sram_hot` |
| `lfo:` | `MO_LFO_SRAM_HOT` from [`../LFO.h`](../LFO.h) |
| `noise:` | `NOISE_ENGINE`, `ENABLE_NOISE_OUT` → `out` |
| `board:` | `ENABLE_CV_OUTS`, `ENABLE_WAVE_MUX`, `ENABLE_VOICE_AUX`, `ENABLE_PIO_RESET_INVERT`, `ENABLE_FS_CALIBRATION` (0/1) |
| `bench:` | `AMP_COMP_BENCHMARK`, `BENCH_PATH_STATS` (0/1; opt-in one-shots / path dump) |

**`clkdiv math` / `sysClock_Hz`:** hot path uses cached `sysClock_Hz_cached` (`sys_clock_hz_refresh()` in `setup`/`setup1`). Do not redefine `sysClock_Hz` as `clock_get_hz` — that expanded three times inside `vt_clk_div`. Idle re-bench: expect mean below the ~19 µs triple-call dump (Q16 64/32 remains until `CLKDIV_MODE` is `CLKDIV_FAST_Q4`, `CLKDIV_Q8`, `CLKDIV_GOLD`, or `CLKDIV_FLOAT`).

Probe mode stays on the `clk_sys` line (`stages every 9` / `period only` / `fine probes on` / `fine probes off` when stride is 1).

**`update_CV_outs`:** after CV absorption + mod matrix, this probe can dominate Core1 on RP2040 (soft-float) and still matter on RP2350. Compare `update_CV_outs` `%win` and period-only `loop1` mean/max before/after CV/matrix changes; older doc samples (~3 µs / ~3%win) are **stale**. A/B with `#undef USE_FLOAT_CV_OUTS` on RP2350 (`cv=FIXED`).

### Q15 / fixed-point migration baseline

Before or after flipping engine defaults, capture a **period-only** dump and a **full MAIN** dump on the target MCU with the same preset and play pattern. Record at least:

| Probe | Why |
|-------|-----|
| `loop period` + Core0 `LFO1` / `LFO2` / `drift LFOs` | LFO **generator** cost (Q15 wave + pitch bake) |
| `loop1 period` mean/max | Overall Core1 budget |
| `update_CV_outs` `%win` | CV + matrix **consumer** path (Q15 wins land here) |
| `voice_task` `%win` | Pitch / clkdiv / amp / PW (adds `*_pitch_mod_q24`) |
| `ADSR_update` `%win` | Envelope hot path (separate from LFO; see ADSR notes) |
| Banner `engine:` / `adsr:` / `noise:` | Compile-time + live flags (see §3) |

**Read Core0 LFO and Core1 CV/voice separately.** A small Core0 LFO mean rise after Q15 is normal; judge conversion success by Core1 `update_CV_outs` / pitch path (`cv=FIXED`) and musical feel, not by LFO µs alone. Do not mix LFO probe means with `ADSR_update` `%win` — different cores, gates, and jobs ([`LFO.md`](LFO.md)).

**ADSR Q15 cache A/B (RP2040):** only meaningful when `ADSR_BEZIER_NATIVE_Q15=0`. Shipping DCO uses **`NATIVE_Q15=1`** (no DAC→Q15 remap). Keep `ADSR_BEZIER_USE_FLOAT=0`. EnvVCF/EnvVCF2 are sampled once per tick outside the voice loop.

**ADSR timebase:** `ADSR_update` uses parameterless `noteOn`/`noteOff`/`getWave()` (each reads `micros()`/`millis()`). Do not share one `t` across edges + sample — unsigned delta underflow skips A/R.

**ADSR native Q15 (shipping):** `ADSR_BEZIER_NATIVE_Q15=1` in [`adsr.h`](../adsr.h). `getWave()` uses per-call `micros()` (4 envs including EnvVCF2). Sustain/idle fast-path; lean Q15 tap publish. Internal amp peak **`ADSR_Q15_PEAK`** (`32768` when `ADSR_BEZIER_Q15_DYADIC=1`, else `32767`); bus taps stay `0…ADSR_Q15_ONE` (32767). Tables init at `ADSR_Q15_PEAK` with P1/P2 × `maxVal/4096`. A/B dyadic: `-DADSR_BEZIER_Q15_DYADIC=0`. Consumers read `*_q15`. **Do not** refresh u12 mirrors via `levelDac()` inside `ADSR_update`. VCA export uses `cv_q15_to_u12` / `CV_U12_SCALE` (not `/4095`). EnvDCO detune uses `ADSR_Q15_TO_DCO_IDX_MUL` mul+shift.

**ADSR phase index:** `ADSR_BEZIER_PHASE_SHIFT` in [`adsr.h`](../adsr.h) — **24** = Q24×uint64 (A/B listen, smoother long A/D/R); **22** = Q22×uint32 (fast). Amp stays Q16. Re-bench / ear-check max attack + env→pitch when flipping.

Save the USB text (or screenshot) as the pre/post reference. Cmds **28/29** (pitch interp) remain the cents/speed check when changing multiplier tables.

### Musical verify after Q15 / Q24-Q16 migration

Play-test (same preset before/after):

1. **LFO1 → pitch** and **LFO2 fine/coarse** — depth at mid/full should match prior feel (param depths were rescaled for Q15×Q24→`>>15`).
2. **LFO2 → PW** and **ADSR → PW** — pulse width travel at full mod.
3. **EnvVCA / EnvVCF + LFO1→VCA / LFO2→VCF** — CV depth and polarity.
4. **Matrix** LFO/ADSR/noise sources and pitch dest (±1 oct at depth 1023).
5. **Analog drift** pitch + VCF drift amount.
6. **Pitch cents** — held notes across the keyboard; optional cmd **29** accuracy report.

```
-- Core 1  (window 1002.113 ms) --
probe                       count      mean       min       max      total   %win
loop1 period                48211     20.78     18.00    412.00  1001800.0   99.9
  ADSR_update                9980      1.42      1.31      8.90    14171.6    1.4
  update_CV_outs             9980      3.07      2.88     11.40    30638.6    3.0
  voice_task TOTAL          48211      9.84      9.11     74.20   474396.2   47.3
    clkdiv math             48211      0.58      0.51      2.13    27962.3    2.7
    PIO put/exec            48211      1.94      1.88      6.40    93529.3    9.3
    note-on retrigger          12     18.40     17.90     31.20      220.8    0.02
    (unattributed)              -         -         -         -    31004.1    3.0
  (unattributed)                -         -         -         -   482593.6   48.1
```

*(Example shape only — re-measure on your build.)*

**`%win` is the column that matters for budget.** Probes fire at wildly different rates —
`ADSR_update` runs on a 100 µs gate, `note-on retrigger` only on note-on, and the
per-oscillator stages run once per voice. A mean in isolation says nothing about a stage's
share of the CPU. **`total` and `%win` nest-add** (parent ≈ children + unattributed);
**`mean` / `min` / `max` do not** — never sum those across rows.

| Column | Adds across rows? | Notes |
|--------|-------------------|-------|
| `total` / `%win` | Yes (same parent) | Trust these for CPU share |
| `mean` | No | Rounded from full `sum/n` so `mean ≈ total/count` for one row; gated stages and idle mean you cannot add sibling means to the parent mean |
| `min` / `max` | No | Each probe’s own best/worst sample over the window (different loop turns). Child maxes need not sum to parent max |

`%win` is percent of the wall-clock collection window with **two decimals** (e.g. `0.04`).
Shares below `0.01%` print as `<0.01` so rare stages (note-on on a long window) stay visible.
While paced dump TX is active, both `BENCH_PERIOD` and stage probes skip samples so parent/child
windows stay aligned.

The dump prints the full parent/child tree (including MAIN children under `note-on retrigger`);
rows with `count == 0` are omitted.

**`note-on retrigger`** is not a flag flip: it stops OSC1/OSC2, optionally reloads exact period
(Y / clk_div), restarts in sync, and writes RANGE PWM — typically tens of µs mean with a
**tiny `%win`** (often well under 0.1% on multi-second windows) because it fires only on
note-on. Mode A/B: debug **26** `EXACT_Y` (default) vs **27** `SYNC_JMP` (jmp only on
running SMs; no disable / Y load / `enable_in_sync`). Dump/ack lines include `note_retrig=…`.

Probes (MAIN; rare hits keep steady-state tax small):

| Probe | Parent | When |
|-------|--------|------|
| `retrig period split` | `voice_task` | EXACT_Y note-on frames, next to `phase align` |
| `retrig SM apply` | `note-on retrigger` | EXACT_Y: disable + fused noclear load + jmp + enable; SYNC_JMP: jmp only |
| `retrig RANGE PWM` | `note-on retrigger` | every note-on flag (even if `oscSync == 0`) |

Do **not** split SM apply into disable/load/jmp/enable — cold XIP on the rare note-on block
slides between those children and mis-ranks them (e.g. fake slow `jmp` after a faster load).
Dense note-ons (count ≫ 4) before trusting means. Attack-frame cost also includes **ADSR**
`noteStart` and **portamento** note-on reset outside this parent.

**Core 0** siblings under `loop period` include `MIDI read`, `serial panel/USB`, and gated
`LFO1` / `LFO2` / `drift LFOs` (~50 µs). MIDI USB+DIN still runs **every** `loop()` (keep
note latency low; TinyUSB MIDI `available()` remains on the idle path). Panel Serial2 and
USB CDC (`dco_control` / dumps) run only on `timer1msFlag` (~1 ms), and CDC is skipped when
`Serial` is not open — expect `serial panel/USB` **mean / %win to collapse** vs a pre-gate
dump (~16 µs / ~27% idle). Large Core0 **max** still often lands in `MIDI read` or a 1 ms
serial burst — compare those maxes (USB CDC RX vs MIDI library). For Q15 A/B: Core0 LFO
rows = generate/bake; Core1 `update_CV_outs` / `voice_task` = consume — see Q15 baseline
table above.

**`(unattributed)`** is the parent's total minus everything its children claimed. It is not
vanished time:

- **MAIN children already put `BENCH_END` tax in the parent gap** — not only FINE. With many
  MAIN kids under `voice_task`, a large `(unattributed)` is mostly probe bookkeeping. Confirm
  with `RUNNING_AVERAGE_PERIOD` (true loop1) vs full MAIN before chasing that hole as DSP.
- Remaining gap with FINE off after MAIN kids: glue + FINE-only work (`PW arithmetic`).
- With **FINE on**, tax grows further; A/B `voice_task TOTAL` with FINE off before chasing.
- If children exceed the parent (rounding / residual skew), the dump prints
  **`(over-attributed)`** with a negative total instead of clamping to zero.

**Gate counts vs loop count.** If `ADSR_update` / PW rows have the same count as
`voice_task TOTAL`, the core 1 loop is slower than their 99/100 µs gates, so the gates fire
every iteration. When the loop is faster (typical with FINE off), those counts drop to about
half — that is correct, not double-counting.

**Sub-µs stages.** Cycle probes whose mean is under 1 µs print mean/min/max as cycles (suffix
`c`); `total` and `%win` stay in microseconds so the budget still adds up. A row of
`0.00` µs was a display floor, not dead code.

**`min` / `max`** are the jitter for that probe alone. For a realtime loop the max is usually
more actionable than the mean; a stage with a 0.6 µs mean and a 40 µs max is a worse problem
than a steady 5 µs one. Do not add sibling maxes and compare to the parent max.

**Path counters** (printed after Core 1, same window, **only if `BENCH_PATH_STATS`**). Integer
bumps only — not SysTick probes — so they do not add measurement barriers. With the flag off,
dump 10 omits the whole `-- Path counters --` block. Families with no bumps for the live path
print `(not instrumented for this build/path)` instead of a wall of zeros. Use them to
attribute stage `max` spikes:

| Counter | Meaning |
|---|---|
| `ratio hit` | Segment cache already valid (RATIO_Q16, FLOAT walk, FLOAT_FAST) |
| `ratio miss_direct` | Miss resolved by walk / trunc±1 (no bsearch) |
| `ratio miss_bsearch` | Miss fell through to binary search (RATIO_Q16 / FLOAT walk; should stay 0 on FLOAT_FAST) |
| `ratio clamp` | Modifier at/outside table ends (early out) |
| `walk_steps max/sum` | Walk length on direct misses |
| `amp hit / miss_walk / miss_scan / clamp` | FLOAT_QUAD and FIXED cache walk (`hit` / `miss_walk` / `miss_scan` / `clamp`); LUT leaves amp counters idle |
| `porta off / note_on / retime / steady_time / steady_slew` | Exclusive portamento path per voice frame |

With `BENCH_PATH_STATS` on, live pitch/amp bump ratio and amp counters (RP2350:
`FLOAT_FAST` / `FLOAT_QUAD`; RP2040: `RATIO_Q16` / `FIXED`). LUT amp leaves amp
counters idle (prints not-instrumented unless FLOAT_QUAD/FIXED ran). Path bumps also
pause while paced dump TX is active (`bench_out_active`).

If `ratio interpolate` max is high **and** miss rate / `walk_steps max` climb under pitch mod,
the spike is algorithmic. If miss rate is ~0 but max stays large, suspect IRQ inside the
probe. Same idea for porta: correlate max with `note_on` / `retime` / `steady_slew` counts.

**Live gate for pitch-path edits (same patch/music test):** accept only if `ratio miss rate`
stays ~70% (not ~90%), `ratio interpolate` mean/max do not worsen, and `loop1` /
`voice_task` means hold. Cmd **28** (esp. jump) ranks find logic but does **not** clear
live — trunc-only and fmaf/x0-reshape won or looked fine in places and lost live. Keep
FLOAT_FAST ±1 even when `walk_steps sum=0` (live ballast). Live `_fast` is
`__attribute__((noinline))` so inlining into `voice_task_float` does not reshape the
whole task. Cmd **29** still gates accuracy (~0¢ vs FLOAT walk).

Segment-find changes (direct index vs walk) do **not** change pitch accuracy — same knot
and lerp. Confirm with pitch accuracy cmd `29` if desired; speed with profiler + path counters.

Reset happens on every report (and on amp-comp method switch), so each dump describes at
least the last 1 s of collection since the previous reset/dump.

## 4. How it measures

**Time source: SysTick.** A free-running 24-bit down-counter clocked from `clk_sys`, read as
a single load from `systick_hw->cvr`. Resolution is one CPU cycle — 4.4 ns at 225 MHz.

RP2040's Cortex-M0+ has **no DWT cycle counter**, so SysTick is the only single-cycle source
available on both RP2040 and RP2350. SysTick is a per-core peripheral, which is why
`bench_init_core()` is called from both `setup()` and `setup1()`.

**Range limit.** 24 bits wraps after 2^24 cycles: about **74.5 ms** at 225 MHz, **126 ms** at
133 MHz. `BENCH_PERIOD()` uses the **same SysTick clock** as stages when `BENCH_USE_SYSTICK`
(kind `BENCH_CYC`); samples longer than `BENCH_PERIOD_MAX_US` are discarded instead of
recorded. The 1 µs timer (`timer_hw->timerawl`) is only for the dump window / 1 s gate, or
for every probe when `BENCH_USE_SYSTICK` is `0`. If a cycle probe's `max` climbs past ~70% of
the counter range (~52 ms at 225 MHz), the report prints a wrap warning under the row rather
than showing a plausible-looking wrong number.

**Overhead.** At boot each core times 32 back-to-back counter reads and keeps the minimum,
which is the cleanest estimate of the probe's own cost (anything larger caught an interrupt).
That constant is subtracted from every cycle sample and printed in the report header, so the
measurement floor is visible.

**Storage.** 20 bytes per probe: `{count, min, max, sum}`, integer only. That matters on the
RP2040, where the M0+ has no FPU and every float operation is an `__aeabi_*` library call —
a float accumulator would cost more than most of the stages being measured.

## 5. What it cannot tell you

**RAM / heap / stack.** This profiler measures **time**, not SRAM. `__not_in_flash_func` copies code into `.time_critical` (static RAM, smaller total heap) — it is not `malloc`. Use Diagnostics **Dump RAM** (`PARAM_DEBUG_COMMAND` **13**, `ENABLE_MEM_DIAG`) and [`MEMORY.md`](MEMORY.md). Comment out `ENABLE_MEM_DIAG` or cmd **14** before period-only dump 10 if you need to match pre-mem_diag benches.

**Probes are optimisation barriers.** The compiler cannot keep values in registers, reorder,
or merge work across a counter read. An instrumented build does not generate the same code as
the shipping build, and the more probes, the wider the gap. This is what
`RUNNING_AVERAGE_FINE` is for: compare `voice_task TOTAL` with fine probes on and off to see
how much the instrumentation is costing you (tens of microseconds per call is normal with
FINE on). Trust the coarse `%win` numbers more than the fine stage means.

**Interrupts land inside probes.** USB and MIDI activity shows up as occasional large `max`
values that have nothing to do with the measured code. Judge stages by `mean` and `%win`,
and treat an isolated large `max` as a question rather than a verdict.

**Loop period is SysTick too** (when `BENCH_USE_SYSTICK`). Parent and children share one unit
before the dump converts to µs, so `(unattributed)` is no longer skewed by 1 µs quantisation
on the parent. Long stalls (autotune / cal) are dropped via `BENCH_PERIOD_MAX_US`, not shown
as a huge `max`. Set `BENCH_USE_SYSTICK` to `0` to restore the old 1 µs PERIOD path.

## 6. Adding a probe

Add one line to `BENCH_PROBES` in [`../bench.h`](../bench.h):

```c
X(vt_my_stage, 1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task, "my stage")
//  id       core   unit        tier          parent          label
```

then bracket the code:

```c
BENCH_BEGIN(vt_my_stage);
...
BENCH_END(vt_my_stage);
```

Storage, the enum id, core ownership, nesting and the printed label all come from that one
line. Nothing else needs editing — which is the point. The previous profiler kept its probe
list in three places (`voices.h` externs, `voices.ino` definitions, and the print function),
they drifted apart during the 3-oscillator refactor, and the mismatch compiled silently
because an `extern` that is never used needs no definition.

Use `BENCH_FBEGIN` / `BENCH_FEND` instead for a fine-tier stage, and `BENCH_PERIOD(id)` for
an interval between arrivals rather than the duration of a block.

## 7. Former `CLKDIV_BENCHMARK` (removed)

The in-loop float-vs-double sample inside `voice_task_float` is gone. Use compile-time
`CLKDIV_FLOAT` / `CLKDIV_GOLD` / `CLKDIV_Q16` and cmds **32/33** **FLOAT_LIVE** vs **GOLD_LIVE**
/ **GOLD_REF** (§10). Float voice honors `CLKDIV_MODE` via `clkdiv_live_hz_total_cycles`.

## 8. Amp-comp methods (`AMP_COMP_BENCHMARK`)

Compare live amp-comp algorithms and check LUT accuracy against the float quadratic.

```c
#define AMP_COMP_BENCHMARK   // requires RUNNING_AVERAGE; useful with USE_FLOAT_AMP_COMP
```

| `PARAM_DEBUG_COMMAND` | Effect |
|----------------------:|--------|
| 20 | Live method → `FLOAT_QUAD` (cached walk) |
| 21 | Live method → `LUT` (speed A/B) |
| 22 | Live method → `FIXED` (RP2040 compile default; A/B on RP2350 float amp) |
| 24 | Speed bench all three methods → `bench_out_*` paced TX |
| 25 | Accuracy vs `FLOAT_QUAD` → same output path |

Method select (20–22) works without `AMP_COMP_BENCHMARK`. With `RUNNING_AVERAGE`, each press **resets the profiler**, then acks in the Board pane as `amp_comp method=… (profiler reset)`. Play ~1 s with pitch motion, then dump (**10**). On **RP2350 (FPU)** expect **amp comp** mean / `%win` roughly **LUT ≪ FLOAT_QUAD ≲ FIXED** (FIXED can lose to FLOAT in the float-Hz speed bench because each call does `lrintf` → Q8 then integer window math). On **RP2040 soft-float** the old intuition **FLOAT_QUAD ≫ FIXED ≫ LUT** still holds. Confirm with speed bench; absolute `meanNs` moves with Core 1's `live_method=` (contention). Reports 24–25 no-op unless both `AMP_COMP_BENCHMARK` and `RUNNING_AVERAGE` are on (same rule as profiler 10–12 for paced output).

Live **FLOAT_QUAD** and **FIXED** window find both use a per-osc `ampWinCache` then walk
(full scan only as rare fallback). With **`BENCH_PATH_STATS`**, dump **10** prints an `amp:`
path line (`hit` / `miss_walk` / `miss_scan` / `clamp`, `find_steps`). **FLOAT_QUAD** and
**FIXED** bump hit/walk/scan/clamp. **LUT** leaves amp counters idle (dump prints
`amp: (not instrumented…)` unless another method ran in the same window). Without the flag,
dump 10 has no path block. LUT fill / accuracy gold call the same
`get_chan_level_float_quad` (precompute resets `ampWinCache` afterward). On dual-build
(RP2350), FIXED often has **no plateau early-out** (smoothed Q table); FLOAT_QUAD/LUT still
early-out from original plateau Hz — expected accuracy gap above that Hz, not a bench bug.

**Bench vs live (why cmd 24 ≫ “feel”)**

Cmd **24** is a microscope: ~700k amp-only calls, so `pctVsFloat` gaps look large (LUT often ≪ 100; FIXED often ≫ 100 on float voice). Live `amp comp` is often only ~10–20 `%win` of the voice window — even a large amp speedup moves total loop1 modestly. Large instrumented `(unattributed)` under `voice_task` is probe bookkeeping; do not judge “synth got faster” from that. Subjective sameness is normal: pitch / PIO still dominate. Leave `#define AMP_COMP_BENCHMARK` commented in `DCO.ino` unless you need cmds **24/25** — method switches **20–22** work without it.

Under float voice, **FIXED** is often slower than FLOAT_QUAD in the speed bench (each FIXED call does `lrintf` → Q8 then integer window math). Rank live methods by `amp comp` **mean / `%win`**, not by ear.

**Synthetic calibration (cmds 24 / 25 only)**

Speed and accuracy do **not** use LittleFS `voiceTables` (which may be empty). They:

1. Snapshot the live breakpoints.
2. Install a **linear fake** table: 22 points, Hz from 1…`AMP_COMP_MAX_HZ`, levels from 1…`DIV_COUNTER` (same on all oscs), then `precompute_amp_comp_for_engine()`.
3. Run the measurement (report lines include `cal=SYNTHETIC`).
4. Restore the snapshot and precompute again so play-mode cal is unchanged.

Do not treat 24/25 as a check of your board’s stored calibration.

**Live A/B with the hot-path profiler**

1. Enable `RUNNING_AVERAGE` (optional `RUNNING_AVERAGE_FINE`). Same preset both sides.
2. Send method (20–22) — Board acks `amp_comp method=… (profiler reset)`.
3. Play with pitch modulation ~1 s; dump profiler (**10**).
4. Check `build: … amp_method=…`, the `amp:` path line, and `amp comp` **mean / `%win`** (not loop1 feel).
5. Rebuild with `RUNNING_AVERAGE_PERIOD` and dump again — trust **period-only** `loop1`
   mean **and max** for whether the synth got faster (ignore instrumented loop means).

Expected signatures after a fresh dump (same play gesture each time):

| Method | `amp:` path counters | `amp comp` vs FLOAT_QUAD |
|--------|----------------------|--------------------------|
| **20 FLOAT_QUAD** | mostly `hit`, some `miss_walk`, rare `miss_scan`; `find_steps` = walk length | baseline `%win` |
| **21 LUT** | idle / `(not instrumented…)` | clearly lower mean / `%win` |
| **22 FIXED** | same cache walk as FLOAT_QUAD (`hit` / `miss_walk` / `miss_scan` / `clamp`) | often higher mean / `%win` on float voice (`lrintf`→Q8) |

If after **21** a fresh window still shows FLOAT_QUAD-style hit/walk counters, the method did
**not** apply (chase param/debug path).

**Dense note-on retrigger A/B** (after flashing note-on MAIN children)

1. `RUNNING_AVERAGE` on, FINE off. Reset (**11**), then play many note-ons for ≥1 s (want
   `note-on retrigger` count ≫ 4). Dump (**10**).
2. Under `note-on retrigger`, read `retrig SM apply` and `retrig RANGE PWM`; split is a
   `voice_task` sibling.
3. Switch mode **26** / **27**, reset, same dense play, dump again. EXACT_Y: SM apply =
   disable+load+jmp+enable; SYNC_JMP: SM apply = jmp only + `RANGE PWM`.

**Speed report** (`=== AMP COMP BENCH ===`): fixed-width comparison table — method, calls, totalUs, meanNs, pctVsFloat (vs `FLOAT_QUAD`). Methods: `FLOAT_QUAD` (cached), `LUT`, `FIXED`. Each method runs through live **`get_chan_level_by_method`** with `ampWinCache` reset first (avoids FLOAT_QUAD poisoning FIXED). Workload is the **same 0.01 Hz grid as accuracy** (`1.00…AMP_COMP_MAX_HZ`, **one osc** / `AMP_COMP_BENCH_OSCS` — synthetic cal is identical per osc; header `grid=… oscs=`). No vibrato / coarse step. ~700k calls per method; wait for paced Board pane output. Absolute `meanNs` depends on Core 1 still running live amp-comp during the Core 0 one-shot; prefer `pctVsFloat` for ranking (LUT cheapest). Opt-in: `#define AMP_COMP_BENCHMARK`. Live method is restored after the sweep (`live_method=` in the header).

**Accuracy report** (`=== AMP COMP ACCURACY ===`): per-method plain-English summary of error vs `FLOAT_QUAD` (gold) for `LUT` / `FIXED`. Errors are in **RANGE PWM counts** (full scale = `DIV_COUNTER`), also shown as **% of full**. Grid is every **0.01 Hz** from **1.00 to `AMP_COMP_MAX_HZ`** on **one osc** (same as speed). Each method prints: typical (mean) PWM, worst in-band (freq below max Hz), tip outlier if exact max Hz disagrees by more than 1 PWM, rate of samples **worse than 1 PWM** (`|e| > 1`), and rate of samples **exactly 1 PWM** (`|e| == 1`). Plus `LUT integer-Hz sanity` over `0…AMP_COMP_MAX_HZ` (want 0). LUT indexes by **nearest** integer Hz (not trunc). One-shot is heavy — wait for paced Board pane output.

Implementation: [`../amp_comp_bench.ino`](../amp_comp_bench.ino), wired from `apply_param_debug_command` and `bench_poll_core0()`. dco_control Diagnostics exposes the buttons via `AMP_COMP_COMMANDS` in [`../tools/dco_control/params.py`](../tools/dco_control/params.py).

## 9. Pitch interpolators (cmds 28 / 29)

Compare FLOAT / FLOAT_FAST / RATIO_Q16 / Q12 for speed and a dual-reference accuracy report. Tables and interpolators live **only** in [`../pitch_interp_bench.ino`](../pitch_interp_bench.ino) (private BSS, lazy-filled); the live voice path is untouched. No extra compile flag — needs `RUNNING_AVERAGE` for paced `bench_out_*` TX (same as the profiler dump).

| `PARAM_DEBUG_COMMAND` | Effect |
|----------------------:|--------|
| 28 | Speed bench all methods → `bench_out_*` paced TX |
| 29 | Dual-ref accuracy (vs FLOAT walk + vs private Q20 slope ref) → same output path |

Live hot path uses compile-time `PITCH_INTERP_MODE`: `FLOAT` (walk), `FLOAT_FAST`
(trunc+clamp±1, `noinline`, **RP2350 default**), `RATIO_Q16` (**RP2040 default**: native Q16
x/y, `slopeQ16`, trunc±1 find, fused ratio return), or `Q12` (×10000 + reciprocal).
Cmd 28/29 private tables always compare FLOAT / FLOAT_FAST / RATIO / Q12; `live_pitch=`
reports the compiled mode. Use 28/29 to rank candidates; **live miss rate / mean gate**
above before accepting find edits.

**Speed report** (`=== PITCH INTERP BENCH ===`): two tables (same four methods), each with calls / totalUs / meanNs / `pctVsFloat` (**that table’s FLOAT** = 100%):

| pattern | step | Role |
|---------|------|------|
| `seq` | `0.0001` | Walk-favorable (hit / +1 segment). FLOAT may beat FLOAT_FAST — not a live regression. |
| `jump` | `0.05` (~2.5 segments), repeats to ≈ seq call count | Miss-heavy; ranking should track live mean under pitch mod. |

Live cross-check: profiler path counters (`miss_direct`, `walk_steps max` ≤ 1, `miss_bsearch` = 0, miss rate often ~60–70% with pitch mod).

**Flag-path** under the compiled voice engine (header `speed=flag-path voice=FIXED|FLOAT`):

- **Fixed voice** (`!USE_FLOAT_VOICE_TASK`): cost to **`ratioQ16`**. `RATIO_Q16` = live clone (native xQ16, `slopeQ16`, trunc±1, fused return); seq xQ16 step 7 / jump 3277. `Q12` = ×10000 `interp_y` + live `y→ratio` reciprocal (`xInt` step 1 / 500). `FLOAT` / `FLOAT_FAST` = soft-float references only. Shared Q24→xQ16 and final `freq×ratio` omitted. `pctVsFloat` is **meanNs** vs FLOAT (call counts can differ across grids).
- **Float voice** (`USE_FLOAT_VOICE_TASK`): cost to **float ratio**. `FLOAT` = walk find; `FLOAT_FAST` = live trunc+clamp±1 `noinline`; `RATIO` = `lroundf(mod*65536)` → native Q16 interp → `*1/65536`; `Q12` = `mod×10000` → `lroundf` → int interp → `/10000`.

**Accuracy report** (`=== PITCH INTERP ACCURACY ===`): fine **seq** modifier grid only (**one osc**) — correctness, not speed ranking. Private **Q20 lerp on native Q16 knots** is the int reference only (not a live mode). Errors in **cents**. Sections:

1. **Table quantization floor** — knot `|Q20_ref − FLOAT|` mean/max cents.
2. **vs FLOAT walk** — FLOAT_FAST (expect ~0¢), RATIO / Q12: mean/max, p50/p95/p99, `>0.5¢` / `>1.0¢`.
3. **vs Q20 ref** — FLOAT / FLOAT_FAST / RATIO / Q12 vs private Q20 on Q16 knots. Mean/max, percentiles, `>0.01¢` / `>0.1¢`, knot vs mid. (Float rows ≈ table gap; RATIO ranks slopeQ16 vs Q20.)
4. **vs Q20 ref by mod-band** — low / mid / high (same four methods).

Diagnostics buttons: `PITCH_INTERP_COMMANDS` in [`../tools/dco_control/params.py`](../tools/dco_control/params.py).

## 10. Clkdiv GOLD_REF / GOLD_LIVE / FLOAT_LIVE / Q16 / Q8 / FAST_Q4 (cmds 32 / 33)

All **six** methods on **both** compiled voice engines. Each row uses the glue a real
implementation would (Q24 convert only when that algorithm needs it on that engine). Live
`voice_task` / PIO / `interpSegCache` untouched. Float live honors `CLKDIV_MODE` via
`clkdiv_live_hz_total_cycles`. No extra compile flag; needs `RUNNING_AVERAGE` for paced `bench_out_*`.

| `PARAM_DEBUG_COMMAND` | Effect |
|----------------------:|--------|
| 32 | Speed all six → `bench_out_*`; column **`pctVsGOLD_REF`** |
| 33 | Accuracy cents vs target Hz + `|Δdiv|` vs **GOLD_REF** |

Header: `voice=FIXED|FLOAT`, `live_clkdiv=` = compile-time `CLKDIV_MODE`. Rank from **jump**.

| Method | Fixed voice (Q24 domain) | Float voice (native Hz) |
|--------|--------------------------|-------------------------|
| **GOLD_REF** | `clkdiv_gold_hz_total_cycles` on **true grid Hz** (no Q24) | same |
| **GOLD_LIVE** | `clkdiv_gold_total_cycles(q24)` (`CLKDIV_GOLD`) | `clkdiv_gold_hz_total_cycles(sys, (double)freq_f)` |
| **FLOAT_LIVE** | `clkdiv_float_total_cycles(q24)` (`CLKDIV_FLOAT`) | `clkdiv_float_hz_total_cycles(sys, freq_f)` |
| **Q16 / Q8 / FAST_Q4** | helper(q24) | **Hz→Q24** then helper (matches live `clkdiv_live_hz`) |

All then + correction + `pio_clk_div_for_y`. Seq + jump precompute true Hz and q24 **outside**
`t0`/`t1`.

**Speed:** `pctVsGOLD_REF` (GOLD_REF = 100). Integer methods ≪ 100 on RP2040; FLOAT_LIVE and
GOLD_LIVE near or above 100 (soft-float / soft-double). On float voice, Q16/Q8/FAST_Q4 include
Hz→Q24 in the timed path. Expect Q16 jump ≪ FLOAT (~70%), ~30% vs GOLD_REF (64/32 with `<<16`).
Q8 ~20%.

**Accuracy:** same Hz grid vs GOLD_REF. GOLD_LIVE Δdiv = Q24 floor (fixed) or float-Hz quant
(float voice). FLOAT_LIVE vs GOLD_LIVE = mantissa vs double. Q8 vs internal `precise_q8`
identity (expect ~0 above 16 Hz). Q16 low-Hz tail ≪ Q8’s ~3¢;
playing range still PIO-limited. Bands: low &lt;100 Hz, mid 100–1000, high &gt;1000 (PIO quant).

**Verify:** dump **10** `clkdiv math` vs cmd **32 jump** `meanUs`. Cmd **33**: GOLD_REF Δdiv 0;
Q8 ≡ internal precise_q8 above ~16 Hz.

Diagnostics buttons: `CLKDIV_HP_COMMANDS` in [`../tools/dco_control/params.py`](../tools/dco_control/params.py).

## 11. Noise engines (`NOISE_ENGINE`)

Engines live in the [`DCO_Noise`](../../DCO_Noise/) Arduino library. The sketch declares `noise0`…`noise1` with ctor args in [`../noise.h`](../noise.h) (`NOISE_ENGINE` in [`../DCO.ino`](../DCO.ino)); `next()` in `loop1`. PIO white via `dcoNoisePioBegin` / `dcoNoisePioRefill`.

| Value | Class | Hot path |
|------:|-------|----------|
| 0 | `ColoredNoise` | Voss pink / 1-pole brown / white; `PioNoiseWhite` FIFO seed → xorshift peels |
| 1 | `FastNoiseGen` | Economy Voss pink / leaky brown / local xorshift white |
| 2 | `PrimeHybridNoise` | Three prime tables summed (`process`); color at `begin` |
| 3 | `ProNoise32` | Q16.15 Kellett pink / DC-corrected brown / xorshift white |

**Reproduce**

1. `#define RUNNING_AVERAGE` (stage probes on).
2. Set `NOISE_ENGINE` to `0`…`3`; leave `ENABLE_NOISE_OUT` **undefined** for a clean A/B (no pin drain on engines 1–3).
3. Set colors via `noise0`…`noise1` ctor args in `noise.h` (default white / pink; Q15 out).
4. Dump with debug command **10**.

**Probes** (under `noise_gens` / `loop1_noise`)

| Probe | What it times |
|-------|----------------|
| `noise_gens` | Refill helper + `noise0`…`noise1.next()` |
| `noise refill` | `dcoNoisePioRefill()` — no-op unless engine **0** or `ENABLE_NOISE_OUT` |

Per-color cost: configure which slots are white/pink/brown and compare parent `%win` across builds, or temporarily time a single `next()` by hand. Prefer `%win` / `total` for share of the window.

**Reference results (RP2040)**

`RUNNING_AVERAGE`, historical four-gen reference (white / pink / brown / alt white); live fleet is two gens (`noise0`/`noise1`). `ENABLE_NOISE_OUT` off.
Means in µs unless suffixed with `c` (cycles). Counts differ across dump sessions — compare **mean** / **`%win`**, not `total`.

| Engine | `noise_gens` mean | `%win` |
|-------:|------------------:|-------:|
| 0 | 11.62 µs | 5.2 |
| 1 | 7.12 µs | 4.4 |
| 2 | 13.62 µs | 6.9 |
| 3 | 10.00 µs | 6.0 |

Engine 0:

```
probe                       count      mean       min       max      total   %win
noise_gens                  75286     11.62      2.85     34.96    874587.12    5.2
  noise refill              75286      2.80      0.41     10.27    210529.33    1.2
```

Engine 1:

```
probe                       count      mean       min       max      total   %win
noise_gens                 780032      7.12      3.11     24.61   5555627.00    4.4
  noise refill             780032        8c        4c       56c     25999.83    0.0
```

Engine 2:

```
probe                       count      mean       min       max      total   %win
noise_gens                  52928     13.62      6.98     34.63    721196.71    6.9
  noise refill              52928       44c        4c      107c      9356.93    0.0
```

Engine 3:

```
probe                       count      mean       min       max      total   %win
noise_gens                  56302     10.00      5.38     33.42    563379.54    6.0
  noise refill              56302       11c        4c      154c      2688.02    0.0
```

**Reading notes**

- **Engine 0:** PIO cost is mostly **`noise refill`** (~2.80 µs here); filter work sits in the parent around the refill child.
- **Engine 1:** Cheapest parent on this RP2040 set (`noise_gens` ~7.12 µs / 4.4%win); refill is a no-op (~8c).
- **Engine 2:** Table-sum path (~9 KB heap per gen); **highest** `noise_gens` mean / `%win` here (~13.62 µs / 6.9).
- **Engine 3:** Kellett pink is heavy per sample, but with the default mixed colors the **parent** mean (~10 µs) sits between engines 0 and 1.
