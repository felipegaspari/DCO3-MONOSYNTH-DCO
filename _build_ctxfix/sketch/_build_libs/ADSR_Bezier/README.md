#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/ADSR_Bezier/README.md"
## ADSR Bezier (millis/micros, dual math backend)

ADSR Bezier is a lightweight, digitally‑controlled envelope generator based on precomputed Bézier lookup tables.  
It is designed to be:

- **Fast at runtime** (fixed-point Q22 time + Q16 amp by default, RP2040‑friendly).
- **Portable** (optional optimized integer backend via `ADSR_BEZIER_USE_FLOAT` for RP2350).
- **Flexible in timing** (supports both `millis()` and `micros()` timebases).
- **Curve‑shaped** (attack/decay/release follow user‑defined Bézier curves).

You can use it as a drop‑in ADSR for any microcontroller synth; the example in this repo is a multi‑voice DCO synth on an RP2040.

For a conceptual introduction to ADSR and this style of lookup‑based envelopes, see the original author’s video:  
[YouTube – mo‑thunderz ADSR class](https://youtu.be/oMxui9rar9M)

---

## 1. Overview

- **Class**: `adsr` (defined in `ADSR_Bezier.h`).
- **Output**: integer envelope level from `0` to `vertical_resolution` (e.g. `0…4000`), plus optional **Q15 tap**.
- **Time parameters**: `attack`, `decay`, `release` are set in **milliseconds**.
- **Timebase**: `ADSR_BEZIER_USE_MICROS` (micros vs millis).
- **Math backend**: `ADSR_BEZIER_USE_FLOAT` (0 = Q22/Q16 fixed; 1 = native **float** time index + Q16 amp).
- **Curves**:
  - Attack, decay and release each read from a Bézier‑generated lookup table.
  - 8 different curve types are supported (`0…7`), selected separately for A/D/R.

Internally, each call to `getWave()` / `getWave(t)`:

1. Uses elapsed time since phase start (`t` from caller, or `micros()`/`millis()`).
2. Converts elapsed time to a **table index** (Q22 `(delta*scale)>>22` when `FLOAT=0`; `(float)delta * rate` when `FLOAT=1`).
3. Reads the appropriate table value for the current stage (attack uses pre-reversed tables).
4. Maps that curve value with Q16 amp scaling.
5. Optionally refreshes a unipolar Q15 cache (`ADSR_BEZIER_UPDATE_Q15_CACHE`).

Scales are precomputed in setters; the fixed hot path has no runtime integer division for short phases.

---

## 1b. Math backend (`ADSR_BEZIER_USE_FLOAT`)

Select at compile time before including the header:

```cpp
#define ADSR_BEZIER_USE_FLOAT 0   // default: Q22/Q16 (RP2040)
// #define ADSR_BEZIER_USE_FLOAT 1 // float time index (needs FPU; e.g. RP2350)
#include <ADSR_Bezier.h>
```

| Value | Hot path | Best for |
|-------|----------|----------|
| `0` (default) | Q22 index + Q16 range | RP2040 / Cortex-M0+ |
| `1` | `(float)delta * rate_f` index + Q16 amp | RP2350 / Pico 2 (hardware FPU) |

**Branches:** `main` is canonical (dual backend). `fixed-point-version` and `float-version` are legacy aliases — use `main` with the define above.

---

## 1b2. Q15 tap and amplitude domain

Unipolar Q15 uses `ADSR_Q15_ONE` (32767 ≈ 1.0), aligned with mo-lfo’s bipolar full-scale magnitude.

### Amplitude domain (`ADSR_BEZIER_NATIVE_Q15`)

| Flag | Default | `getWave` domain | Q15 | `setSustain` units |
|------|---------|------------------|-----|--------------------|
| `0` | **yes** (library) | DAC counts `0…vertical_resolution` | Secondary cache (`_to_q15_mul`) | DAC counts |
| `1` | DCO shipping | Internal peak `ADSR_Q15_PEAK`; return/tap `0…ADSR_Q15_ONE` | Primary (no remap mul) | `0…ADSR_Q15_PEAK` |

```cpp
#define ADSR_BEZIER_NATIVE_Q15 0   // default: DAC primary + Q15 cache
// #define ADSR_BEZIER_NATIVE_Q15 1 // native Q15 amp; A/B: -DADSR_BEZIER_NATIVE_Q15=1
// #define ADSR_BEZIER_Q15_DYADIC 1 // NATIVE=1: peak 32768 (default); 0 = peak 32767
```

When `NATIVE_Q15=1`:

- Ctor DAC-size arg is kept as the **`levelDac()`** export scale; amp peak is **`ADSR_Q15_PEAK`** (`32768` if `ADSR_BEZIER_Q15_DYADIC=1`, else `32767`).
- `getWave` sustain/idle fast-path; A/D/R publishes lean Q15 tap (`0…ADSR_Q15_ONE`).
- Init curve tables at the same peak: `adsrBezierInitTables(ADSR_Q15_PEAK, …)`. Control points are authored near 4095 and **scaled by `maxVal/4096`** (`2^12`) inside `adsrBezierInitTables` so shapes stay correct at Q15 peak.
- `levelDac()` exports internal level→ctor-vr (mul+shift).
- `ADSR_BEZIER_UPDATE_Q15_CACHE` is ignored (no DAC→Q15 remap).

**DCO** ships `ADSR_BEZIER_NATIVE_Q15=1` + `ADSR_BEZIER_Q15_DYADIC=1` + `ADSR_BEZIER_SRAM_HOT=1` in [`DCO/adsr.h`](../DCO/adsr.h) (panel sustain → peak; tables at `ADSR_Q15_PEAK`; per-call `micros()` including EnvVCF2; RP2040 SRAM pin on `getWave` / `noteOn` / `noteOff`). Library defaults for `NATIVE_Q15` and `SRAM_HOT` remain `0` for standalone sketches.

### SRAM hot path (`ADSR_BEZIER_SRAM_HOT`)

| Value | Meaning |
|-------|---------|
| `0` (library default) | Portable / flash. Examples stay here. |
| `1` | RP2040: `__not_in_flash_func` on `getWave`, `getWave(t)`, `noteOn`, `noteOff`. No-op if the attribute is missing (AVR). |

Define **before** `#include <ADSR_Bezier.h>`. Curve LUTs stay BSS RAM either way; `adsrBezierInitTables` is boot-only (not pinned).

```cpp
// #define ADSR_BEZIER_SRAM_HOT 1   // RP2040 A/B; library default is 0
#include <ADSR_Bezier.h>
```

### Q15 API (both modes)

| API / flag | Role |
|------------|------|
| `ADSR_Q15_ONE` | 32767 — mod-bus / tap full scale |
| `ADSR_Q15_PEAK` | Internal/table peak (`32768` dyadic or `32767`) |
| `levelQ15()` | Read Q15 from last `getWave` / `getWave(t)` |
| `levelDac()` | DAC counts: identity when `NATIVE=0`; Q15→ctor-vr when `NATIVE=1` |
| `getWaveQ15()` / `getWaveQ15(t)` | Advance once, return Q15 (do **not** also call `getWave` same tick) |
| `getWave(t)` | Caller-supplied timestamp (optional; DCO uses parameterless `getWave()`) |
| `invalidateQ15Cache()` | Mode 0: invalidate remap cache; mode 1: no-op |
| `ADSR_BEZIER_UPDATE_Q15_CACHE` | Mode 0 only: `1` shipping; `0` = A/B skip remap |
| `ADSR_BEZIER_Q15_DYADIC` | Mode 1 only: `1` = peak 32768 / `<<1` scales; `0` = A/B peak 32767 |
| `ADSR_BEZIER_SRAM_HOT` | `0` library default; `1` = RP2040 pin `getWave` / `noteOn` / `noteOff` |

Skip-unchanged (mode 0): Q15 mul runs only when the DAC level changes (sustain/idle usually free after first sample).

---

## 1c. Examples and testing

| Layer | Example | What it validates |
|-------|---------|-------------------|
| **Host math** | [`examples/compare_fixed_float/`](examples/compare_fixed_float/) | Fixed vs reference index/output formulas (no hardware) |
| **Device integration** | [`examples/ADSR_benchmark/`](examples/ADSR_benchmark/) | Real `adsr` class, phase state machine, `getWave()` cycle timing |
| **Basic usage** | [`examples/ADSR_example/`](examples/ADSR_example/) ([README](examples/ADSR_example/README.md)) | Single envelope, DCO-style boot + loop |

**Host regression** (fixed vs reference math):

```bash
cd examples/compare_fixed_float
g++ -std=c++17 -O2 -o compare compare.cpp && ./compare
```

**On-device self-test + speed bench** (Pico 2):

```bash
arduino-cli compile \
  --fqbn rp2040:rp2040:rpipico2 \
  --library . \
  examples/ADSR_benchmark
```

Open Serial Monitor at **115200** after upload. The benchmark waits until the port is open, then prints PASS/FAIL self-tests and cycles per `getWave()` for attack/decay/sustain/release with **N instances** per iteration (default **1**; set `ADSR_BENCHMARK_INSTANCES` to **3** for DCO EnvDCO + EnvVCA + EnvVCF). At ~10 kHz voice updates with N=3 that is ~30k `getWave()` calls/s — multiply **µs per call** from the benchmark by `10000 × N` to estimate envelope CPU load.

Compare backends: rebuild with `--build-property build.extra_flags=-DADSR_BEZIER_USE_FLOAT=1` or uncomment the define in the sketch. See [`examples/ADSR_benchmark/README.md`](examples/ADSR_benchmark/README.md) for upload and float-toggle details.

---

## 2. Installation

The library is **header-only** (`library.properties` lists `includes=ADSR_Bezier.h` only).

### 2.1. As a generic Arduino library

1. Create a folder in your Arduino libraries directory, e.g. `ADSR_Bezier`.
2. Copy `ADSR_Bezier.h` and `library.properties` into that folder.
3. In your sketch, define `ARRAY_SIZE` **before** the include, then:

```cpp
#define ARRAY_SIZE 512
#include <ADSR_Bezier.h>
```

### 2.2. Inside the DCO monorepo

The DCO firmware pulls this repo in via a symlink:

- `DCO/_build_libs/ADSR_Bezier` → `../../ADSR_Bezier`
- [`DCO/adsr.h`](../DCO/adsr.h) sets `ARRAY_SIZE`, `ADSR_BEZIER_USE_FLOAT`, **`ADSR_BEZIER_NATIVE_Q15=1`**, **`ADSR_BEZIER_SRAM_HOT=1`**, and `#include <ADSR_Bezier.h>`
- DCO: native Q15 amp; `adsrBezierInitTables(ADSR_Q15_ONE, …)` (P1/P2 × `maxVal/4096`); consumers read `*_Level_q15`
- Boot calls `init_ADSR()` from the main sketch (table init + setters on all three envelope instances)

You do not need a separate `.cpp` or `adsrCreateTables` — call `adsrBezierInitTables()` once at startup (see §5.2).

### 2.3. Quick start (standalone sketch)

Minimal pattern (see [`examples/ADSR_example/`](examples/ADSR_example/)):

```cpp
#define ARRAY_SIZE 512
#ifndef ADSR_BEZIER_USE_FLOAT
#define ADSR_BEZIER_USE_FLOAT 0
#endif
#include <ADSR_Bezier.h>

static adsr env(4095, 0.9995f, 0.9995f, false, 1, 2, 1);

void setup() {
  adsrBezierInitTables(4000, ARRAY_SIZE, _curve_tables);
  env.setAttack(100);
  env.setDecay(200);
  env.setSustain(2000);   // 0 .. vertical_resolution
  env.setRelease(300);
  env.setResetAttack(true);
}

void loop() {
  env.noteOn();            // trigger as needed
  int level = env.getWave();
}
```

Table `maxVal` (4000 in DCO) can differ from instance `vertical_resolution` (4095 for EnvVCA/EnvVCF).

---

## 3. Public API

The main class lives in `ADSR_Bezier.h`:

```cpp
class adsr {
public:
    adsr(int vertical_resolution,
         float attack_alpha,
         float attack_decay_release,
         bool bezier,
         int bezier_attack_type,
         int bezier_decay_type,
         int bezier_release_type);

    void setAttack(unsigned long attack_ms);
    void setDecay(unsigned long decay_ms);
    void setSustain(int sustain_level);
    void setRelease(unsigned long release_ms);

    void setResetAttack(bool reset_attack);

    void adsrCurveAttack(uint8_t curveType);
    void adsrCurveDecay(uint8_t curveType);
    void adsrCurveRelease(uint8_t curveType);

    void noteOn();    // uses millis()/micros() internally
    void noteOff();   // uses millis()/micros() internally

    int getWave();                 // advance (internal timebase)
    int getWave(unsigned long t);  // advance (caller timestamp; prefer getWave())
    int16_t levelQ15();            // Q15 after last advance
    int levelDac();                // DAC counts (export when NATIVE_Q15=1)
};
```

### 3.1. Constructor

- **`vertical_resolution`**: maximum envelope value (e.g. `4000` for EnvDCO, `4095` for EnvVCA/EnvVCF in DCO).
- **`attack_alpha`, `attack_decay_release`**: legacy exponential-curve parameters; unused after `adsrBezierInitTables()` — DCO passes nominal values like `0.9995f`.
- **`bezier`**: legacy flag. Both paths use the shared global `_curve_tables` filled by `adsrBezierInitTables()`. DCO uses `false`.
- **Curve types** (`bezier_attack_type`, `bezier_decay_type`, `bezier_release_type`):  
  Indices into `_curve_tables[8]` (0–7). The constructor assigns internal `_bezier_*_type` fields from these args. Change curves at runtime with `adsrCurveAttack()`, `adsrCurveDecay()`, and `adsrCurveRelease()` (as in DCO curve helpers).

### 3.2. Time parameters (milliseconds)

All three functions take **milliseconds** and internally convert to the compiled timebase:

- **`void setAttack(unsigned long attack_ms)`**
- **`void setDecay(unsigned long decay_ms)`**
- **`void setRelease(unsigned long release_ms)`**

When `ADSR_BEZIER_USE_MICROS == 1`:

- 1 tick = 1 µs, so `attack_ms` is multiplied by 1000 internally.

When `ADSR_BEZIER_USE_MICROS == 0`:

- 1 tick = 1 ms, and `attack_ms` is used as‑is.

### 3.3. Sustain level

- **`void setSustain(int sustain_level)`**  
  Saturates into `[0, vertical_resolution]`.  
  Units follow `ADSR_BEZIER_NATIVE_Q15`: **DAC counts** when `0`, **Q15** (`0…ADSR_Q15_ONE`) when `1`.  
  Used as the decay target and hold level after decay.

### 3.4. Reset attack behavior

- **`void setResetAttack(bool reset)`**
  - `true`: every `noteOn()` starts from `0`.
  - `false`: `noteOn()` starts from the current envelope level (legato behavior).

### 3.5. Triggering

- **`void noteOn()`**:
  - Captures the current time (`micros()` or `millis()` depending on the flag).
  - Sets up internal state for the attack stage.
  - Precomputes the **attack range scale** for output mapping.

- **`void noteOff()`**:
  - Decrements the note counter.
  - When all notes are off, captures the current time and starts the release stage.
  - Precomputes the **release range scale** for output mapping.

### 3.6. Reading the envelope

- **`int getWave()`**: Advances using an internal `micros()` / `millis()` read. Returns DAC counts when `NATIVE_Q15=0`, or Q15 when `NATIVE_Q15=1`. Prefer this when `noteOn`/`noteOff` also use the internal timebase (DCO shipping).
- **`int getWave(unsigned long t)`**: Same advance with a **caller-supplied** timestamp. If you use this after `noteOn()`/`noteOff()`, `t` must not be earlier than the edge stamp — unsigned `delta` underflow skips attack/release. Prefer `getWave()` instead.
- **`int16_t levelQ15()`**: Unipolar Q15 from the last advance (`0…ADSR_Q15_ONE`). Prefer this after `getWave` / `getWave(t)` instead of calling `getWaveQ15` in the same tick.
- **`int levelDac()`**: DAC-domain level (identity when `NATIVE_Q15=0`; Q15→ctor-vr export when `NATIVE_Q15=1`).

At a fixed control rate (e.g. ~100 µs on RP2040), call `getWave()` per envelope (each reads time internally), then read `levelQ15()` for mod taps.

---

## 4. Timebase selection (millis vs micros)

Timebase is selected at compile time with `ADSR_BEZIER_USE_MICROS`:

- **Micros mode (default)**:

```cpp
#define ARRAY_SIZE 512
#define ADSR_BEZIER_USE_MICROS 1
#include <ADSR_Bezier.h>
```

  - Internally uses `micros()` for timing.
  - Time resolution: 1 µs.
  - Parameter units remain **milliseconds**; conversion is done behind the scenes.

- **Millis mode (backwards‑compatible)**:

```cpp
#define ARRAY_SIZE 512
#define ADSR_BEZIER_USE_MICROS 0
#include <ADSR_Bezier.h>
```

  - Internally uses `millis()` for timing.
  - Coarser resolution, but closer to the original Arduino ADSR examples.

The rest of your code (parameter units, `noteOn()`, `getWave()`) does not change between modes.

---

## 5. Example: DCO synth ADSR integration (RP2040)

This section mirrors the live DCO firmware ([`DCO/adsr.h`](../DCO/adsr.h), [`DCO/adsr.ino`](../DCO/adsr.ino)).

### 5.1. Global ADSR configuration (`adsr.h`)

Each voice carries **three** envelope instances: EnvDCO (pitch/PW), EnvVCA, EnvVCF. Static prototypes are copied into `ADSRStruct`:

```cpp
#define ARRAY_SIZE 512
#ifndef ADSR_BEZIER_USE_FLOAT
#define ADSR_BEZIER_USE_FLOAT 0
#endif
#include <ADSR_Bezier.h>

static constexpr uint16_t ADSR_1_CC = 4000;
static constexpr uint16_t ADSR_CV_CC = 4095;

adsr adsr1_voice_0(ADSR_1_CC, ADSR1_curve1, ADSR1_curve2, false, 7, 7, 7);
adsr adsr_vca_voice_0(ADSR_CV_CC, ADSR_VCA_curve1, ADSR_VCA_curve2, false, 1, 2, 1);
adsr adsr_vcf_voice_0(ADSR_CV_CC, ADSR_VCF_curve1, ADSR_VCF_curve2, false, 4, 6, 1);

struct ADSRStruct {
  adsr adsr1_voice;
  adsr adsr_vca_voice;
  adsr adsr_vcf_voice;
};

ADSRStruct ADSRVoices[] = {
  { adsr1_voice_0, adsr_vca_voice_0, adsr_vcf_voice_0 },
};
```

Notes:

- With DCO `NATIVE_Q15=1`, `adsrBezierInitTables(ADSR_Q15_ONE, …)` — P1/P2 scaled by `maxVal/4096` from ~4095-authored coords.
- DCO uses `bezier=false`; curve shapes come from `_curve_tables` after init.

### 5.2. Initialization (`adsr.ino`)

Boot builds tables and applies setters to all three envs per voice — **no `noteOn()` at boot**:

```cpp
void init_ADSR() {
  adsrBezierInitTables((float)ADSR_Q15_ONE, ARRAY_SIZE, _curve_tables);

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    ADSRVoices[i].adsr1_voice.setAttack(ADSR1_attack);
    ADSRVoices[i].adsr1_voice.setDecay(ADSR1_decay);
    ADSRVoices[i].adsr1_voice.setSustain(adsr_sustain_for_set(ADSR1_sustain, ADSR_1_CC));
    ADSRVoices[i].adsr1_voice.setRelease(ADSR1_release);
    ADSRVoices[i].adsr1_voice.setResetAttack(ADSRRestart);

    ADSRVoices[i].adsr_vca_voice.setAttack(ADSR_VCA_attack);
    // ... decay, sustain (Q15), release, setResetAttack for VCA and VCF ...
  }
}
```

### 5.3. Per‑voice update loop (~10 kHz)

Note edges only — **no setter spam on `noteOn`** (params stay current via `ADSR_set_parameters` / init / curve helpers). Each call uses its own timebase; EnvVCF/EnvVCF2 sampled once per tick (not inside the voice loop):

```cpp
inline void ADSR_update() {
  for (int i = 0; i < NUM_VOICES; i++) {
    // noteStart[] / noteEnd[] → noteOn/noteOff on EnvDCO, EnvVCA, EnvVCF, EnvVCF2
    ADSR1Level[i] = ADSRVoices[i].adsr1_voice.getWave();
    ADSR1Level_q15[i] = ADSRVoices[i].adsr1_voice.levelQ15();
    ADSR_VCA_Level[i] = ADSRVoices[i].adsr_vca_voice.getWave();
    ADSR_VCA_Level_q15[i] = ADSRVoices[i].adsr_vca_voice.levelQ15();
  }
  ADSR_VCF_Level = adsr_vcf_voice.getWave();
  ADSR_VCF_Level_q15 = adsr_vcf_voice.levelQ15();
  ADSR_VCF2_Level = adsr_vcf2_voice.getWave();
  ADSR_VCF2_Level_q15 = adsr_vcf2_voice.levelQ15();

  ADSR_set_parameters();
}
```

### 5.4. Parameter updates at low rate

Dirty-bit push for EnvDCO / EnvVCA / EnvVCF A/D/S/R (~200 Hz gate inside `ADSR_set_parameters`). Parameter refresh is **not** done on every note edge — see [`DCO/adsr.ino`](../DCO/adsr.ino).

---

## 6. Internal workings (high‑level)

### 6.1. Bézier precomputation

- Bézier curves are defined by:
  - Start point `A = (0, maxVal)`
  - End point `B = (maxVal, 0)`
  - Two control points `P1`, `P2` per curve type.
- For each of the 8 curve types and each `i` in `[0, ARRAY_SIZE-1]`:
  1. Compute a target `x` value along the line.
  2. Use a binary search (`findYForX`) along the Bézier parameter `t` to find the point where the Bézier curve’s `x` matches `xTarget`.
  3. Store the corresponding `y` in `_curve_tables[type][i]`.

This happens once at startup and uses `float`, but it’s out of the runtime hot path.

### 6.2. Time → table index

The ADSR runs as a small state machine with an explicit phase and phase‑start time:

- `ADSR_PHASE_ATTACK`
- `ADSR_PHASE_DECAY`
- `ADSR_PHASE_SUSTAIN`
- `ADSR_PHASE_RELEASE`
- `ADSR_PHASE_IDLE`

For each call to `getWave()` / `getWave(t)`:

1. Take time in **ticks** (µs or ms) — either from the argument or an internal `micros()`/`millis()` — and compute `delta = now - t_phase_start` for the current phase.
2. **Realtime within phase, isolated across phases**:
   - Attack index uses the current `attack` time; changing `attack` while in ATTACK morphs the remaining attack, but does not affect DECAY/RELEASE.
   - Decay index uses the current `decay` time; changing `decay` while in DECAY morphs the remaining decay, but does not affect RELEASE.
   - Release index uses the current `release` time; changing `release` while in RELEASE morphs the tail, but earlier phases are unaffected.
3. Convert `delta` to a table index using the active backend:
   - **Fixed (`ADSR_BEZIER_USE_FLOAT=0`):** Q22 `(delta*scale)>>22` or uint64 division fallback when scale is 0.
   - **Float index (`ADSR_BEZIER_USE_FLOAT=1`):** `(float)delta * rate_f` (needs hardware FPU; e.g. RP2350).
4. Clamp `idx` to `[0, ARRAY_SIZE-1]`.

### 6.3. Table → output level

Per stage (simplified):

- **Attack**:  
  `out = attack_start + curveVal * (vertical_resolution - attack_start) / vertical_resolution`

- **Decay**:  
  `out = sustain + curveVal * (vertical_resolution - sustain) / vertical_resolution`

- **Release**:  
  `out = curveVal * release_start / vertical_resolution`

Both backends map curve → level with precomputed **Q16** amplitude scales (set at setter / `noteOn` / `noteOff`). Attack uses pre-reversed curve tables. `FLOAT=1` only changes the **time→index** path; amplitude stays integer Q16.

---

## 7. Tips for using the library

- **For RP2040 / M0+:** keep `ADSR_BEZIER_USE_FLOAT` at `0` (default) — soft-float makes `FLOAT=1` slower.
- **For RP2350 / Pico 2:** `ADSR_BEZIER_USE_FLOAT=1` can win via hardware FPU on the time index; A/B with the profiler before shipping.
- Prefer parameterless `getWave()` when `noteOn`/`noteOff` also use the internal timebase (see §5.3). Do not pass a shared `t` taken before note edges — unsigned delta underflow skips attack/release.
- **For best quality:** use micros timebase (`ADSR_BEZIER_USE_MICROS=1`).
- Define **`ARRAY_SIZE`** in your project before `#include <ADSR_Bezier.h>` (512 in DCO).
- Adjust `ARRAY_SIZE` for resolution vs RAM trade-off.
- Use `setResetAttack(true)` for percussive lines; `false` for legato.

---

## 8. License / Credits

- Original ADSR concept and early implementation by **mo‑thunderz**.
- This Bezier + RP2040‑optimized variant and documentation adapted for the DCO4 project.

Enjoy shaping envelopes!

