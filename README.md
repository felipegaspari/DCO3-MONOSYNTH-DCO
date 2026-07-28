# DCO3-MONOSYNTH – Pico 2 DCO Voice Board

Firmware for the **DCO voice board** of DCO3-MONOSYNTH: a **1-voice × 3-oscillator** digitally controlled analog monosynth, based on the DCO4 4-voice dual-DCO design.

Target MCU: **Raspberry Pi Pico 2 (RP2350)** — 3 PIO blocks, 12 state machines.

The aim is a fully digitally controlled analog synth with patch saving for all parameters. This repo is the voice/DCO board only; other boards in the system:

- A **main controller board** (brain/master)
- An **input controller** (pots, knobs, encoders)
- A **screen board** (TFT display)

Those boards talk to this firmware over MIDI and a high-speed UART link.

---

## Target model (vs DCO4)

| Item | DCO4 | This board |
|------|------|------------|
| Voices | 4 | **1** (`NUM_VOICES_TOTAL`) |
| Oscillators | 8 (2 per voice) | **3** |
| Freq generation | PIO SMs | **PIO0/1/2 SM0** (one block per osc) |
| Amplitude compensation | Hardware RANGE PWM | **Same — hardware RANGE PWM** |
| Sync | OSC1↔OSC2 per voice | OSC1↔OSC2; **OSC3 free-running** for now |

Poly/unison allocation (`get_free_voice_sequential`, `setVoiceMode`) is kept so a later **3-voice paraphonic** mode can reuse it. Effective polyphony is still one voice today.

---

## Features

- **Oscillators**
  - Three DCOs on one voice (intervals, OSC2/OSC3 fine detune).
  - Mono / poly / unison mode scaffolding (polyphony limited by `NUM_VOICES_TOTAL`).
  - Oscillator sync modes (hard sync / phase-style) between OSC1 and OSC2.

- **Sound shaping & modulation**
  - Bezier-based ADSR, LFO1/LFO2, per-oscillator analog drift.
  - ADSR→pitch and ADSR→PWM paths; velocity, pitch bend, MIDI CC.

- **Calibration & stability**
  - Per-oscillator DCO calibration (edge timing + PID).
  - Frequency-dependent **amplitude compensation via RANGE PWM** (`amp_comp`, `pwm_set_chan_level`).
  - PW center / low-limit calibration; LittleFS storage.

- **Performance**
  - Dual-core: Core 0 I/O + LFO; Core 1 envelopes, cal, real-time voice engine.
  - Fixed-point math on the hot path.
  - PIO clock dividers + phase for frequency; PWM for range amp and PW.

---

## High-level architecture

- **Voice engine (`voices.*`)** — MIDI notes → per-DCO frequency (portamento, bend, LFO, ADSR) → PIO clkdiv + RANGE PWM levels.
- **Modulation (`adsr.*`, `LFO.*`)** — envelopes and LFOs / drift.
- **Calibration (`autotune.*`, `amp_comp.*`, `PID.*`, `FS.*`)** — measure, build amp tables, store in LittleFS.
- **I/O (`midi.*`, `Serial.*`, `params.ino`)** — USB/DIN MIDI and Serial2 param protocol.

For a file-by-file map, see `REFERENCE_AI.md` (still largely DCO4-oriented; treat voice/osc counts carefully).

---

## Hardware overview

- 3 DCO frequency outputs from PIO SM0 on PIO0, PIO1, PIO2.
- Per-DCO **RANGE PWM** for amplitude compensation.
- Per-voice **PW PWM** for pulse-width.
- Calibration input pin for autotune.
- `Serial1`: DIN MIDI; `Serial2`: main controller; USB MIDI.

Pin maps and constants: `globals.h` (provisional Pico 2 map = first 3 oscs of legacy WEACT DCO4; confirm against final PCB).

---

## Building and flashing

### Prerequisites

- Earle Philhower RP2040/RP2350 Arduino core (Pico 2 supported).
- Libraries: Adafruit TinyUSB, MIDI (FortySevenEffects), PID_v1, LittleFS (core), plus ADSR/LFO under `src/`.
- Optional local deps: `_build_libs/` (gitignored) for offline `arduino-cli` builds.

### Compile (Pico 2)

```bash
arduino-cli compile \
  --fqbn rp2040:rp2040:rpipico2:usbstack=tinyusb \
  --libraries ./_build_libs \
  .
```

Main sketch: `DCO.ino`.

---

## Calibration workflow (overview)

1. Controller requests calibration over Serial2.
2. Firmware sweeps amplitude/PW, measures DCO edges, builds amp-comp tables.
3. Data written to LittleFS; loaded at boot via `init_FS()` / `precomputeCoefficients()`.

---

## Repository layout

- **`DCO.ino`** — main sketch, core-0/1 setup and loops
- **`voices.*`** — voice allocation, pitch → clkdiv / RANGE PWM
- **`state_machines.*`** — PIO load/init, sync sideset
- **`PWM.*`** — RANGE / PW PWM setup
- **`adsr.*`, `LFO.*`** — envelopes and modulation
- **`autotune.*`, `amp_comp.*`, `PID.*`, `FS.*`** — calibration
- **`midi.*`, `Serial.*`, `params.ino`** — I/O and parameters
- **`globals.h`** — voice/osc counts, PIO maps, pins

---

## Design notes

- **Amplitude stays on PWM.** Do not move range amp to PIO unless the hardware design changes again.
- **Freq uses one PIO block per oscillator** so each osc has a dedicated SM0; remaining SMs are free for future (non-amp) features.
- **Paraphonic follow-up:** remap which osc belongs to which voice; keep the existing allocator instead of replacing it with mono stubs.

See the repo root [`README.md`](../README.md) for project-wide purpose and change summary.
