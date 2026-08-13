#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/WAVE_MUX.md"
# Wave mux — per-osc Saw / Pulse / Tri

Analog waveform switching for **OSC1, OSC2, OSC3**. Each oscillator can enable **Saw**, **Pulse**, and **Triangle** independently (9 paths).

Logic is **not** the AS3320 filter-mode DG411 on VOICE-AUX ([`FILTER_ROUTING.md`](FILTER_ROUTING.md)).

---

## Hardware

```text
DCO GP12 DATA / GP13 LATCH / GP14 CLK
        ↓
  2× 74HC595 daisy-chained (16 bits)
        ↓  active-low IN
  3× DG411 (12 SPST; 9 used, 3 spare)
        ↓
  OSC1 / OSC2 / OSC3 mix buses
```

- **DG411:** switch closes when IN is **low** → 595 bit `0` = wave **on**, `1` = off.
- Flag: `ENABLE_WAVE_MUX` in [`DCO.ino`](../DCO.ino) (default off). Soft `waveEnable[3][3]` always updates.

### Provisional bit map

| Bit | Enable |
|----:|--------|
| 0 | OSC1 Saw |
| 1 | OSC1 Pulse |
| 2 | OSC1 Tri |
| 3 | OSC2 Saw |
| 4 | OSC2 Pulse |
| 5 | OSC2 Tri |
| 6 | OSC3 Saw |
| 7 | OSC3 Pulse |
| 8 | OSC3 Tri |
| 9–15 | unused (held high) |

Shift: MSB first into the daisy-chain. Remap `WAVE_MUX_BIT[3][3]` in [`wave_mux.ino`](../wave_mux.ino) when the PCB is frozen.

---

## ParamIds

| ID | Name |
|---:|------|
| 1 | `PARAM_OSC1_SAW_ENABLE` |
| 2 | `PARAM_OSC1_PULSE_ENABLE` |
| 3 | `PARAM_OSC1_TRI_ENABLE` |
| 84 | `PARAM_OSC2_SAW_ENABLE` |
| 85 | `PARAM_OSC2_PULSE_ENABLE` |
| 86 | `PARAM_OSC2_TRI_ENABLE` |
| 87 | `PARAM_OSC3_SAW_ENABLE` |
| 88 | `PARAM_OSC3_PULSE_ENABLE` |
| 89 | `PARAM_OSC3_TRI_ENABLE` |

- `PARAM_SINE_STATUS` (4): **deprecated** (no mux role).
- ParamIds **5–6**: unused (formerly digital-square enables) — reserved.

State: `bool waveEnable[osc 0..2][wave 0=Saw, 1=Pulse, 2=Tri]` in [`cv_state.h`](../cv_state.h).

Shared PW PWM in `voices.ino` runs when any oscillator has Pulse enabled (`waveEnable[*][1]`); otherwise the PW channel is forced to 0.

---

## Manual calibration

`waveSelector_manual_calibration(stage)` turns all paths off, then enables **OSC{stage} Saw** only (`stage` 0..2).

---

## Code

| File | Role |
|------|------|
| [`wave_mux.ino`](../wave_mux.ino) | Bit-bang dual 595 |
| [`params.ino`](../params.ino) | Nine apply handlers |
| [`PINOUT.md`](PINOUT.md) | GPIO summary |
