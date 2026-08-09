# LFO (Q15 bus and depth scales)

DCO is the sole LFO clock after Mainboard absorption. Live waves are **full-scale bipolar Q15**; musical travel is set by **depth scales in `LFO.h`**, baked at param time into Q24 depths.

Code: [`LFO.h`](../LFO.h), [`LFO.ino`](../LFO.ino), bakers in [`params.ino`](../params.ino). Library: repo-root [`mo-lfo/`](../../mo-lfo/) (vendored under `_build_libs/mo-lfo`). CV LFO→VCA/VCF scales: [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).

---

## Depth scales (tune in `LFO.h`)

Hot path:

```text
lfo*_pitch_mod_q24[slot] = applyDepthQ24(level_q15, depth_q24)
                         = (level_q15 * depth_q24) >> 15
```

Bake (params):

```text
depth_q24 = lfo_pitch_depth_q24(amt, SCALE)
amt       = expConverterFloat(panel) / 275000   // octave-fraction unit
```

Larger `SCALE` → deeper mod at the same panel setting.

| Symbol | Value | Role |
|--------|------:|------|
| `LFO1_PITCH_DEPTH_SCALE` | 1700 | LFO1→pitch (`LFO1toDCO` + per-osc); also LFO2 coarse |
| `LFO2_PITCH_DEPTH_SCALE` | 512 | LFO2→fine pitch (OSC2 / OSC3) |
| `ADSR_PITCH_MAX_OCTAVES` | 2.0 | EnvDCO→pitch full-CW max travel (octaves); see below |
| `ADSR_PITCH_DEPTH_PANEL_FULL` | 511 | Panel full scale for ADSR3→detune (`PARAM_ADSR3_TO_DETUNE1`) |
| `DRIFT_PITCH_DEPTH_SCALE` | 1000 | Analog drift→pitch (with unit below) |
| `DRIFT_PITCH_UNIT_Q24` | ~8 | Base octave unit for drift |
| `lfo_pitch_depth_q24(amt, scale)` | helper | `amt * scale * 2^24` → Q24 depth (LFO/drift) |
| `applyDepthQ24(wave, depth)` | helper in **DCO** `LFO.h` | live `wave × depth` (not mo-lfo) |

Drift runtime scale:

```text
drift_pitch_scale_q24 = analogDrift * DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE
```

LFO1 is deeper than LFO2 fine at the same panel curve by design (1700 vs 512). CV LFO→VCA/VCF uses a separate `/1024` domain — see [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).

### EnvDCO (ADSR3) → pitch

Hot path ([`voices.ino`](../voices.ino)):

```text
wave = env_dco_pitch_wave_q15(ADSR1Level_q15)   // unipolar: env; centered: (env−16384)<<1
ADSRModifier_q24 = applyDepthQ24(wave, ADSR1toDETUNE1_scale_q24)
```

`PARAM_ADSR3_PITCH_MODE` (223, default **0 unipolar**) only changes how the Q15 tap becomes octaves; A/D/S/R still run. **Unipolar:** `applyDepthQ24(env, depth)` — sustain > 0 holds a pitch offset; idle (`env == 0`) is the note; full env @ full CW ≈ **+2 oct**. **Centered:** `applyDepthQ24((env − 16384) << 1, depth)` — mid sustain ≈ the played note (or current porta Hz); higher S holds sharp, lower S flat; idle/start ≈ **−2 oct**, peak ≈ **+2 oct**. PW stays unipolar.

- **Env:** linear Q15 (no `linToLog`). Full env ≈ `depth_q24` of travel.
- **Knob bake** ([`params.ino`](../params.ino) `apply_param_adsr1_to_detune1`): signed `expConverterFloat(|v|, 500)`, normalized to `ADSR_PITCH_DEPTH_PANEL_FULL` (511), then × `ADSR_PITCH_MAX_OCTAVES` → Q24.
- **Units:** pitch sum uses Q24 where `1<<24` is the unison table coordinate; adding another `1<<24` at full-scale wave ≈ **+1 octave** (same idea as mod-matrix / character).
- **Tune later:** change `ADSR_PITCH_MAX_OCTAVES` in [`LFO.h`](../LFO.h) (e.g. `1.0f` for one octave at full CW × full env).
- Mid-knob is quieter than the old linear `param/1080000` feel (exp on depth). `linearToLogarithmic` remains in utils but is unused by this path.

---
## Live bus

| Global | Source | Domain |
|--------|--------|--------|
| `LFO1Level` | `LFO1_class.getWaveQ15` | ±32767 (`MO_LFO_Q15_ONE`) |
| `LFO2Level` | `LFO2_class.getWaveQ15` | same |
| `LFO_DRIFT_LEVEL[i]` | `-LFO_DRIFT_CLASS[i].getWaveQ15` | same (negated for Mainboard polarity) |

Init uses `setAmplQ15(MO_LFO_Q15_ONE)` only — not `setAmpl` / `getWave`.

Ctor `dacSize` is unused on the Q15 path (`LFO_DAC_SIZE_UNUSED = 1`). Changing it must not change audio; changing the depth scales above **does**.

---

## Core0 generate vs Core1 consume

| Core | Gate | LFO role |
|------|------|----------|
| **0** | ~50 µs | `getWaveQ15` → `LFO*Level`; LFO1/LFO2 bake pitch via `applyDepthQ24` into `*_pitch_mod_q24[]` |
| **1** | ~100 µs / voice loop | Read Q15 for CV / matrix / PW; **add** `*_pitch_mod_q24` into the pitch sum |

A small rise in Core0 `LFO1` / `LFO2` / `drift` probes after Q15 is expected (wave is already Q15; depth is a few mul-shifts). Big Q15 wins show up on Core1 (`update_CV_outs`, pitch add) — see [`BENCHMARKING.md`](BENCHMARKING.md). Envelope cost is separate (`ADSR_update` on Core1).

`applyDepthQ24` lives in DCO [`LFO.h`](../LFO.h) (synth pitch helper; mo-lfo only supplies `getWaveQ15`). It uses a signed 32-bit split for RP2040. When all `LFO1toOSCn_q24` are 0, `LFO1()` applies global depth once and broadcasts to all three osc slots.

---

## Other sinks

| Sink | Format | Notes |
|------|--------|-------|
| LFO → VCA/VCF | `/1024` on Q15 | [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md) |
| Drift → VCF | `vcf_drift_scale_q15 = analogDrift` | Soft CV |
| LFO2 → PW | `(q15 * LFO2toPW) >> 15` | Full ±1.0 → `LFO2toPW` counts |
| Mod matrix | Q15 pass-through | [`MOD_MATRIX.md`](MOD_MATRIX.md) |

---

## Call sites

| Function | Core | Role |
|----------|------|------|
| `init_LFOs` / `init_DRIFT_LFOs` | 0 boot | Waveform, full-scale Q15 amp, initial Hz |
| `LFO1` / `LFO2` / `DRIFT_LFOs` | 0 ~50 µs | Refresh levels + pitch mods |
| `apply_param_lfo*` | param table | Speeds, waveforms, `*_q24` depth bake |
| `update_CV_outs` / voices / matrix | 1 | Consume Q15 levels |

---

## Related

- [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md) — LFO1→VCA / LFO2→VCF scale bake
- [`MOD_MATRIX.md`](MOD_MATRIX.md) — LFO1/LFO2 as Q15 sources
- [`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md) — live CV combine
