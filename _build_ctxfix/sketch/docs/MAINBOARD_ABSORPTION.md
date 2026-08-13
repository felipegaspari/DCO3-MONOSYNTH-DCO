#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/MAINBOARD_ABSORPTION.md"
# Mainboard → DCO absorption (complete through Phase 5)

Engineering notes for ditching the STM32 Mainboard: **DCO (Pico 2 / RP2350) is the serial hub**, and **filter/VCA/reso PWM + OSC1..3/Sub level PWM + 74HC595** land on the DCO PCB (opt-in flags). I2C level DAC dropped (historical on archived Mainboard only). Archived firmware: [`../../_archived/Mainboard/`](../../_archived/Mainboard/).

Pin map: [`PINOUT.md`](PINOUT.md).  
System overview (three boards: DCO ↔ Input → Screen): [`SYSTEM_OVERVIEW.md`](SYSTEM_OVERVIEW.md).

---

## Target topology

```mermaid
flowchart LR
  World["MIDI USB + DIN"] --> DCO["DCO hub"]
  Input["Input"] -->|"2.5M panel\nInput Serial1 TX GP0 to DCO Serial2 RX GP21"| DCO
  DCO -->|"gap 154 + cal offsets 155\nDCO Serial2 TX GP20 to Input Serial1 RX GP1"| Input
  Input -->|"2.5M UI + relayed gap"| Screen["Screen"]
  DCO --> Analog["3x DCO + VCF + VCA + mux + level PWM"]
```

---

## Core0 / Core1 responsibilities (after merge)

| Core | Keep | Add | Remove |
|------|------|-----|--------|
| **Core0** (`setup`/`loop`) | USB MIDI, DIN MIDI (PIO or HW), LFO1/2 + drift sample, FIFO push LFO1→detune Q24 | **Input UART RX** on `Serial2` GP21 (dense `'a'..'f'`, params, from Input `Serial1` TX GP0) + gap/offset `'x'` TX on `Serial2` GP20 (into Input `Serial1` RX GP1); Screen via Input relay | **done:** Mainboard parser and `'n'`/`'o'` note TX are deleted |
| **Core1** (`setup1`/`loop1`) | LittleFS, amp-comp, PIO voices, autotune/manual cal, **EnvDCO** (today’s ADSR1 → pitch/PW) | **EnvVCA** + **EnvVCF** Bézier engines; **`setPWMOuts`-class** CV writers; wave mux + `write_level_pwm` on param/cal edges | **done:** no dependence on remote note edges — all envelopes read local `noteStart`/`noteEnd` |

**Default scheduling:** EnvVCA/EnvVCF + CV PWM on Core1 next to EnvDCO (shared `noteStart`/`noteEnd` from local MIDI). LFO levels for VCA/VCF read volatiles updated on Core0 (same pattern as LFO1 detune FIFO).

**Core0 UART load:** one 2.5 M link (the DCO's `Serial2` against the Input's `Serial1`: RX GP21 from Input TX GP0, TX GP20 into Input RX GP1) plus DIN MIDI, now that the Screen PIO UART is gone.

---

## Envelope naming (do not confuse wire names)

| Wire / ParamId today | Mainboard role | DCO today | After merge |
|----------------------|----------------|-----------|-------------|
| ADSR1 block `'a'` | → VCA CV | — | **EnvVCA** |
| ADSR2 block `'b'` | → VCF CV | — | **EnvVCF** |
| ADSR3 / `'s'` / `PARAM_ADSR3_*` | Forward times/depths to DCO | Local **ADSR1** → pitch/PW | **EnvDCO** (keep ParamId numbers) |

---

## Symbol port matrix

### Already on DCO (keep; stop dual-running with Mainboard)

| Area | Symbols / files |
|------|-----------------|
| MIDI / voices | `note_on`/`note_off`, `voice_task_*`, PIO freq, RANGE/PW PWM |
| EnvDCO | `adsr1_voice_*`, `ADSR1Level`, `ADSR1_*` times, `ADSR1toDETUNE1`, `ADSR1toPWM`, `ADSR3ToOscSelect` |
| LFO | `LFO1`/`LFO2`, drift LFOs, `LFO1toDCO`, `LFO2toPW`, detune2/3 |
| Serial params | `params.ino` DCO apply table, `'p'`/`'w'`/`'x'` |
| Cal | `autotune.*`, LittleFS, manual cal flags |

### Port from Mainboard → DCO

| Area | Port | Source |
|------|------|--------|
| Input protocol | `serial_input_protocol.h` + `'a'..'f'`/`'p'`/`'w'`/`'q'` handlers | `Mainboard/Serial.ino` |
| Screen TX | *not ported* — gap `'x'` goes to Input, which relays it | `Mainboard/Serial2.ino` |
| EnvVCA/EnvVCF | Second/third `ADSR_Bezier` + update/restart/curves | `Mainboard/ADSR.*` |
| CV math | `setPWMOuts`, formulas, VCA Bézier table, keytrack | `Mainboard/PWM.ino`, `formulas.*`, `tables.h`, `auxiliary.h` |
| Wave mux | `waveSelector.*` | Mainboard |
| Osc/sub levels | Was I2C level DAC on Mainboard → **PWM level CVs** on DCO (`init_level_pwm` / `write_level_pwm`) | Mainboard (historical) |
| Params (local-only) | wave statuses, OSC1..3/sub levels, VCA level, reso comp, keytrack, vel→VCF/VCA, LFO1→VCA, ADSR1/2 curves/restart | `Mainboard/params.ino` |
| Manual cal CV park | mute mux / open VCA-low / cutoff-reso 0 — **landed** as `update_CV_outs_manual_calibration()` (`cv_out.ino`), restore on cal exit in `apply_param_manual_calibration_flag` | `setPWMOutsManualCalibration` |

### Delete after cutover (no DCO equivalent needed)

| Item | Why |
|------|-----|
| Mainboard↔DCO `'n'`/`'o'` note notify | Envelopes local |
| `'f'` PW / `'s'` ADSR stream from Mainboard | Produced locally or from Input |
| Param “forward to DCO” branches | No peer |
| Duplicate LFO clocks on Mainboard | Unify on DCO |
| STM32 `Timers.ino` HardwareTimer init | Pico PWM API |
| `ENABLE_SD` / flashData / BU2505 | Already removed or N/A |
| Mainboard sketch as runtime | Archive after Phase 5 |

### Do not port

STM32 pin macros, soft-timer bank as-is (reimplement with Pico timers if needed), `_removed/` dumps, buffered empty serial queues, Screen RX stub unless Screen requires it.

---

## Phased delivery

| Phase | Exit |
|-------|------|
| **0** (docs + [`PINOUT.md`](PINOUT.md)) | Pin/UART/core/symbol plan agreed |
| **1** | Input UART + local params — **landed** (`Serial2` panel protocol, `cv_state.h`) |
| **2** | EnvVCA/EnvVCF + CV math — **landed** (`adsr_vca`/`adsr_vcf`, `cv_out.ino` → `VCA_PWM`/`VCF_PWM`) |
| **3** | Real PWM / 595 — **landed** (opt-in `ENABLE_CV_OUTS` / `ENABLE_WAVE_MUX`; levels are PWM, not I2C DAC) |
| **4** | Unify LFOs; Screen path; drop redundant forwards — **landed** (gap via Input relay; note frames deleted) |
| **5** | Remove Mainboard link; three-board overview; archive Mainboard — **landed** (`_archived/Mainboard`; no escape hatch left) |
| **6** | Collapse the serial topology — **landed** (SerialPIO Screen UART, legacy Serial2 protocol, note senders and all three link flags deleted) |

**Parity fixes after the Phase 6 audit:** `VCALevel` regained the Mainboard's `* 32` panel scale; SQR1/SQR2 use `lin_to_log_128[]` again instead of a linear (and inverted) ramp; resonance amp-compensation is clamped at `MAX_RESONANCE` so it can no longer wrap negative through the unsigned VCA lerp; the four ADSR curve params now reach the EnvVCA/EnvVCF engines; LFO1→VCA and LFO2→VCF depths are normalised to the 4095 full-scale the Mainboard formulas assume and carry the Mainboard's LFO sign (the DCO's own `LFO*_CC` values stay as-is for the pitch/PW paths); drift depth compensates for `LFO_DRIFT_CC` being 2000 here versus 1000 there. On the Input side, preset load now transmits `PARAM_VCF_KEYTRACK`, which previously only moved with the encoder.

**Current state:** Serial2 is unconditionally the Input link, paired with the Input's `Serial1` (RX GP21 from Input TX GP0; TX GP20 into Input RX GP1); gap 154 is relayed Input→Screen. The parser is `serial_panel_task` (the `serial_STM32_task` alias is gone), and the DCO exposes exactly one `'x'` sender, `serialSendParam32`.

---

## Phase 0 defaults (locked for planning)

| Decision | Choice |
|----------|--------|
| Production MCU package | Prefer **RP2350B** (48 GPIO); bench OK on Pico 2 / WEACT if map fits |
| UART | Input on a **HW UART** (only peer link; Screen reached by Input relay); DIN MIDI on **PIO UART** (HW MIDI interim allowed) |
| Cutoff 0 GPIO | **GP15** (not shared with PW slice) |
| Osc levels | **I2C level DAC dropped**; OSC1/2/3 + Sub PWM pins in [`PINOUT.md`](PINOUT.md) |

See [`PINOUT.md`](PINOUT.md) for the full table. Remaining fab freeze is a PCB review, not a firmware blocker for Phase 1.

---

## Open before / during Phase 3 hardware bring-up

1. Confirm physical carrier exposes GP29 (RESET OSC1) or remap RESET/RANGE.
2. **Reso1 GP7 shares PWM slice 3 with RANGE OSC2 GP22** — firmware scales reso duty into `DIV_COUNTER`; consider remapping Reso1 to a free slice on next PCB spin.
3. ~~Finalize I2C level-DAC channel map~~ — **done:** DAC removed; levels are OSC1/2/3 + Sub PWM.
4. Choose interim HW MIDI vs PIO MIDI for first Input UART bring-up.
