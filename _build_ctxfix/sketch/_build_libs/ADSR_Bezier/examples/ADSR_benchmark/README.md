#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/ADSR_Bezier/examples/ADSR_benchmark/README.md"
# ADSR_benchmark

On-device **self-test** and **`getWave()` speed benchmark** for ADSR_Bezier. Models N envelope instances updated together (default **1**; use **3** for DCO EnvDCO + EnvVCA + EnvVCF load).

Tests run from **`loop()`** (one step per iteration), not blocking `setup()`, so USB Serial on Pico stays responsive.

## What it measures

| Section | Purpose |
|---------|---------|
| Self-test | Phase state machine, clamps, legato, zero-time attack, attack rise |
| Setter sweeps | `setAttack` / `setDecay` / `setRelease`: time grid × curve 0–7; `setSustain`: level grid — **min/max per setter** |
| getWave sweeps | Attack / decay / sustain / release phase × curve 0–7 — **min/max per phase** + global min |
| Speed summary | Grand min/max + **DCO budget** (% CPU @ 10 kHz × N instances) |

Optional: set `ADSR_BENCHMARK_FINGERPRINT` to `1` for a 4 s envelope checksum (compare fixed vs reference builds).

## Configuration

Edit the defines at the top of [`ADSR_benchmark.ino`](ADSR_benchmark.ino) (all must appear **before** `#include <ADSR_Bezier.h>`):

```cpp
#define ARRAY_SIZE 512
// #define ADSR_BEZIER_USE_FLOAT 1
// #define ADSR_BEZIER_NATIVE_Q15 1  // amp domain Q15; also -DADSR_BEZIER_NATIVE_Q15=1
// #define ADSR_BEZIER_SRAM_HOT 1    // RP2040 SRAM pin; also -DADSR_BEZIER_SRAM_HOT=1

#define ADSR_BENCHMARK_SELFTEST 1
#define ADSR_BENCHMARK_SPEED 1
#define ADSR_BENCHMARK_FINGERPRINT 0
#define ADSR_BENCHMARK_INSTANCES 1        // 3 = DCO EnvDCO + EnvVCA + EnvVCF
#define ADSR_BENCHMARK_ITERATIONS 10000UL // getWave timing iterations
#define ADSR_BENCHMARK_SETTER_ITERATIONS 1000UL
#define ADSR_BENCHMARK_WAIT_SERIAL 1
// #define ADSR_BENCHMARK_QUIET 1
```

With `NATIVE_Q15=1`, init tables at `ADSR_Q15_ONE` and pass Q15 sustain levels (see library README §1b2).

Sweep grids (fixed in sketch):

| Setter | Values | Curves |
|--------|--------|--------|
| setAttack / setDecay / setRelease | 1, 10, 50, 100, 500, 2000 ms | 0–7 |
| setSustain | 0, 500, 2000, 4000 | *(none)* |

- **Self-tests** use `g_testEnv`; timed waits call **`getWave()` ~every 1 ms** via `pollEnvelopeMs()` (required — library is poll-driven).
- **Speed bench** runs **one sweep cell per `loop()` call** (~200+ steps total at defaults).
- **getWave pinning** uses `pollEnvelopeMs(env, …)` so envelopes are in the target phase before timing.

## Example speed output

```
--- Setter sweep: setAttack ---
curves 0-7
values ms: 1 10 50 100 500 2000
c0:    1    1    2    2    3    4
...
MIN 1 cycles (setAttack curve=0, 1ms)
MAX 48 cycles (setAttack curve=7, 2000ms)

--- getWave sweep: decay phase ---
c0:154 c1:156 ... c7:160
MIN 154  MAX 160 cycles/getWave (decay phase, curve 0-7)

=== Speed summary ===
setAttack   min=1 max=48
setDecay    min=1 max=52
setSustain  min=1 max=12
setRelease  min=1 max=51
getWave     min=143 max=160 cycles/getWave (phase=sustain curve=3)
DCO budget: 143 cycles/call x 1 instances @ 10kHz => ~0% CPU
```

## Build and upload (Pico 2)

```bash
cd /path/to/ADSR_Bezier
arduino-cli compile --fqbn rp2040:rp2040:rpipico2 --library . examples/ADSR_benchmark
arduino-cli upload --fqbn rp2040:rp2040:rpipico2 -p /dev/ttyACM0 examples/ADSR_benchmark
```

Open Serial Monitor @ **115200**. `setup()` prints init + banner; self-tests and speed sweeps stream from `loop()`.

## DCO budget reference

At `ADSR_BENCHMARK_INSTANCES=3`, DCO calls three `getWave()` per voice tick @ ~10 kHz → **30k calls/s**. The summary **DCO budget** line uses the global **getWave min** × instances × 10 kHz / `F_CPU`.

## Compare fixed vs FPU (`ADSR_BEZIER_USE_FLOAT=1`)

FPU backend uses operand-fit uint32 divide when values fit; FPU reciprocal fallback for long phases (e.g. 60 s micros release index).

```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico2 --library . \
  --build-property build.extra_flags=-DADSR_BEZIER_USE_FLOAT=1 \
  examples/ADSR_benchmark
```

Host math checks: [`../compare_fixed_float/`](../compare_fixed_float/).

## Compare SRAM pin (`ADSR_BEZIER_SRAM_HOT=1`)

Library default is **0**. On Pico, pin `getWave` / `noteOn` / `noteOff` for a speed A/B vs flash:

```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico2 --library . \
  --build-property build.extra_flags=-DADSR_BEZIER_SRAM_HOT=1 \
  examples/ADSR_benchmark
```
