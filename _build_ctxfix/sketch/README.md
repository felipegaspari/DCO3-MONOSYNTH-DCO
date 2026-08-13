#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/README.md"
# DCO3-MONOSYNTH – Pico 2 DCO Voice Board

Firmware for the **DCO voice board** of DCO3-MONOSYNTH: **1 voice × 3 oscillators**, based on DCO4 but retargeted to **Raspberry Pi Pico 2 (RP2350)** with 3 PIO blocks.

Branch baseline: `autotune-improvements` (refactored autotune / amp-comp), then monosynth voicing.

## Target model

| Item | Value |
|------|--------|
| Voices | `NUM_VOICES_TOTAL = 1` |
| Oscillators | `NUM_OSCILLATORS = 3` |
| Freq SMs | PIO0/1/2 SM0 |
| Amplitude | RANGE PWM via `write_range_pwm` — PIO dither (`RANGE0_PIO_DITHER_TEST`) or HW slice |
| Voice entry | `voice_task_main()` → float or fixed engine |
| Sync | OSC1↔OSC2; OSC3 free-running |
| PW | One shared PW channel (voice 0); inverted `PW_LOOKUP` |

Poly/`setVoiceMode` scaffolding kept for a later paraphonic mode.

## Hub mode

Serial1 is DIN MIDI, Serial2 is the Input board — the DCO's only peer link. It pairs with the Input's Serial1: the DCO's Serial2 RX (GP21) is fed by the Input's TX (GP0), while the DCO's Serial2 TX (GP20) drives the Input's RX (GP1). Gap 154 and cal offsets 155 go out as `'x'` frames on that TX, and Input forwards 154 to the Screen; the DCO has no Screen port of its own. Soft EnvVCA/EnvVCF always run; uncomment `ENABLE_CV_OUTS` / `ENABLE_WAVE_MUX` for hardware CV / mux / OSC level PWM writers. Docs: [`docs/MAINBOARD_ABSORPTION.md`](docs/MAINBOARD_ABSORPTION.md), [`docs/SYSTEM_OVERVIEW.md`](docs/SYSTEM_OVERVIEW.md).

## Build

```bash
arduino-cli compile \
  --fqbn rp2040:rp2040:rpipico2:usbstack=tinyusb,flash=4194304_524288 \
  --libraries ./_build_libs \
  .
```

Main sketch: `DCO.ino`. Use a flash size that includes a LittleFS partition (example:
4 MB flash / 512 KB FS) so MCU presets and calibration files have space —
[`docs/PRESET_STORE.md`](docs/PRESET_STORE.md), [`docs/BUILD_FLAGS.md`](docs/BUILD_FLAGS.md).

### Libraries (`_build_libs`)

Vendored Arduino libraries used by the build (passed via `--libraries ./_build_libs`):

| Library | Path | Notes |
|---------|------|--------|
| `DCO-PROTOCOL` | `_build_libs/DCO-PROTOCOL` → `../../DCO-PROTOCOL` | Shared `params_def.h` / serial headers. Sketch-root shims (`params_def.h`, etc.) `#include` the real files so Arduino IDE finds them without `--libraries`. |
| `ADSR_Bezier` | `_build_libs/ADSR_Bezier` → `../../ADSR_Bezier` | Symlink to monorepo root; track **`main`**. Toggle math backend in [`adsr.h`](adsr.h) via `ADSR_BEZIER_USE_FLOAT` (default `0` = fixed-point). |
| `DCO_Noise` | `_build_libs/DCO_Noise` → `../../DCO_Noise` | Symlink to monorepo root; noise engines (`begin`/`next`). Fleet size in [`noise.h`](noise.h). |
| `mo-lfo` | `_build_libs/mo-lfo` → `../../mo-lfo` | Symlink to monorepo root. Isolated like ADSR: `LFO.h` includes the header via `_build_libs/...`; `LFO.ino` `#include`s `mo-lfo.cpp` so Arduino IDE links it (no sketchbook library). |
| `MIDI_Library` | `_build_libs/MIDI_Library` | Vendored copy |
| `PID_v1` | `_build_libs/PID_v1` | Vendored copy |

**Monorepo layout:** clone `DCO3-MONOSYNTH` with sibling [`ADSR_Bezier`](../ADSR_Bezier/), [`DCO_Noise`](../DCO_Noise/), and [`mo-lfo`](../mo-lfo/) at the repo root so the symlinks resolve.

**Standalone DCO clone:** replace the symlink with a submodule:

```bash
cd _build_libs
rm -f ADSR_Bezier
git submodule add -b main https://github.com/felipegaspari/ADSR_Bezier.git ADSR_Bezier
git submodule update --init _build_libs/ADSR_Bezier
```

Ensure `ADSR_Bezier` is on branch **`main`**. To try the float envelope path on Pico 2, set `ADSR_BEZIER_USE_FLOAT` to `1` in `adsr.h` before building.

## Docs

See `docs/` — especially:

- [`docs/BUILD_FLAGS.md`](docs/BUILD_FLAGS.md) — complete compile-time flag catalog (`DCO.ino` + headers + vendored libs)
- [`docs/ENGINE_OPTIONS.md`](docs/ENGINE_OPTIONS.md) — float vs fixed voice/amp/CV math depth
- [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md) — hot-path CPU profiler
- [`docs/MEMORY.md`](docs/MEMORY.md) — SRAM / heap / stack (`__not_in_flash_func`, dump cmd 13)
- [`docs/PINOUT.md`](docs/PINOUT.md) — provisional hub + CV pin / UART map (Phase 0)
- [`docs/MOD_MATRIX.md`](docs/MOD_MATRIX.md) — sparse mod matrix (levels / dual reso / Dist Drive)
- [`docs/CV_MOD_SCALES.md`](docs/CV_MOD_SCALES.md) — VCA/VCF mod depth bakers, Q15 peak math (`/512` vs `/1024`), when to call
- [`docs/LFO.md`](docs/LFO.md) — LFO Q15 bus; pitch/drift depth scales in `LFO.h`
- [`docs/UPDATE_CV_OUTS_HOT_PATH.md`](docs/UPDATE_CV_OUTS_HOT_PATH.md) — `update_CV_outs` ~10 kHz call-graph map
- [`docs/CHARACTER.md`](docs/CHARACTER.md) — Character / noise imperfection knobs, inject sites, and removal checklist
- [`../VOICE-AUX/docs/I2S_NOISE.md`](../VOICE-AUX/docs/I2S_NOISE.md) — PCM5102 I2S noise listen on voice-aux (not on DCO)
- [`docs/WAVE_MUX.md`](docs/WAVE_MUX.md) — per-osc Saw/Pulse/Tri via dual 595 + DG411
- [`docs/DUAL_MCU.md`](docs/DUAL_MCU.md) — RP2350A + RP2040 aux; `ENABLE_VOICE_AUX`; ParamId ownership
- [`../VOICE-AUX/`](../VOICE-AUX/) — RP2040 helper firmware (Dist / filter mode)
- [`docs/DISTORTION.md`](docs/DISTORTION.md) — post-LP Drive/Mix distortion stage (hardware idea + CV prototype)
- [`docs/FILTER_ROUTING.md`](docs/FILTER_ROUTING.md) — SSI2144 → dist → AS3320 multimode concept
- [`docs/PIO_OSCILLATORS.md`](docs/PIO_OSCILLATORS.md) — PIO programs, state machine topology, period model, sync modes, phase align, sub-osc, and the invariants behind them
- [`docs/PRESET_STORE.md`](docs/PRESET_STORE.md) — MCU 256-slot LittleFS presets (4/file chunks), cal dump/restore, `'B'`/`'C'` bulk, text dump protocol
- [`_shared/docs/CALIBRATION_STORAGE.md`](_shared/docs/CALIBRATION_STORAGE.md) — the seven calibration banks on flash: byte format, per-board sizing, and what must never change (`FS.h` / `FS.ino` are shims over the shared library)
- [`docs/README_serial_and_params.md`](docs/README_serial_and_params.md) — slim inner serial / ParamId how-to (incl. preset cmds)
- [`tools/dco_control/`](tools/dco_control/README.md) — Linux bench controller: parameters, preset browser, MCU sync, cal backup over USB
- [`docs/MIDI_CC_MAP.md`](docs/MIDI_CC_MAP.md) — MIDI CC implementation chart: the same control surface over 7-bit CC, for a panel app or a DAW (generated, along with the Open Stage Control session in `tools/panels/`)
- [`docs/MAINBOARD_ABSORPTION.md`](docs/MAINBOARD_ABSORPTION.md) — plan to absorb Mainboard into DCO
- [`docs/AUTOTUNE.md`](docs/AUTOTUNE.md) — this board’s cal facts; algorithms in [`_shared/docs/AUTOTUNE.md`](_shared/docs/AUTOTUNE.md). [`docs/SYSTEM_OVERVIEW.md`](docs/SYSTEM_OVERVIEW.md), [`docs/FILE_INDEX.md`](docs/FILE_INDEX.md)

Removed dead code lives under `_removed/` and is not compiled.
