#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/PINOUT.md"
# DCO board pinout (provisional) — hub + CV absorption

**Status:** Phase 0 complete (provisional). **Not frozen for PCB fab** until reviewed against the physical monosynth carrier.

**Platform assumption:** RP2350A (GPIO0–29 class) plus optional helper **RP2040**, or a solo **RP2350B**. Stock Pico 2 header only exposes **26** GPIOs and may omit some pins used below (notably **GPIO29**). MCU module is `DCO_MCU_BOARD` in [`project_config.h`](../../project_config.h) (this tree defaults to **Pico 2**). Dual-MCU architecture: [`DUAL_MCU.md`](DUAL_MCU.md).

Related: [`MAINBOARD_ABSORPTION.md`](MAINBOARD_ABSORPTION.md), [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9.
Live RESET/RANGE/PW/cal/sub pins are in [`globals.h`](../globals.h). Hub/CV pins are behind
`ENABLE_CV_OUTS` / `ENABLE_WAVE_MUX` (commented out in shipping builds).

---

## Phase 0 locked defaults

| Decision | Choice |
|----------|--------|
| Carrier | Prefer **RP2350A + RP2040 aux** for production pin budget; keep **RP2350B solo** as alternate (DCO firmware retains full IO code). Bench may use Pico 2 / WEACT |
| UART split | **Input on HW UART** (only peer link); Screen UI and gap via Input relay; **DIN MIDI on PIO UART** long-term (interim: HW MIDI @ GP0/1) |
| Cut0 vs PW | Cut0 on **GP15** (slice 7 B) — **avoid sharing slice 1 with PW** (wrap 1024 vs CV ~4095) |
| Reso1 | **GP7** (slice 3 B), not GP6 (would collide with RANGE OSC2 slice 3 A) |

---

## UART allocation (only 2 hardware UARTs)

| Role | Peripheral | Pins (provisional) | Baud | Notes |
|------|------------|--------------------|------|-------|
| **Input** (panel + gap) | HW `UART1` / `Serial2` | **GP20 TX / GP21 RX** | 2 500 000 | Only peer link, against the Input's `Serial1`: GP21 RX ← Input TX GP0 (panel data); GP20 TX → Input RX GP1 (`'x'` 154/155). Input relays gap 154 to Screen |
| ~~Screen (direct)~~ | *(removed)* | *(GP8–10 → sub engine)* | 2 500 000 | Was SerialPIO GP8/9; deleted — gap reaches Screen through Input |
| **DIN MIDI** | HW `UART0` interim → **PIO UART** | **GP0 TX / GP1 RX** | 31 250 | Keep USB MIDI |
| ~~Mainboard link~~ | *(removed)* | — | — | Archived STM32; no firmware path remains |

Soft bit-bang at 2.5 M is not acceptable.

**Current:** DIN on HW UART0 @ GP0/1 (`Serial1` in Arduino-Pico); Input on HW UART1 @ GP20/21 (`Serial2`). Both hardware UARTs are spoken for, and the DCO has no Screen port at all — gap display goes out on the Input link and Input forwards it on its own `Serial2`. With `ENABLE_SUBOSC_ENGINE2` (RP2350 default), GP8/GP9 are the two sub-oscillator squares and GP10 is the boolean combiner output — the one pad the sub section is mixed from (see Live outs below).

The DCO's pins on the Input link are fixed at GP20 TX / GP21 RX, and both wires terminate on the Input's `Serial1`: GP21 RX comes from the Input's TX (GP0), GP20 TX goes to the Input's RX (GP1). The Input's other UART, `Serial2`, drives the Screen from GP4; its RX (GP5) is not wired.

---

## Live DCO outs (unchanged until PCB freeze)

From [`globals.h`](../globals.h). OSC1–3 RESET/RANGE are DCO4 global OSC 3/4/5 (indices 2–4 of the 8-osc map). Osc 0/1 differ WeAct vs Pico (GPIO 29 vs 28/26); that slice is unused here, so these three pins are identical on `DCO_MCU_WEACT_RP2040`, `DCO_MCU_PICO`, and `DCO_MCU_PICO2` (this tree’s default). Switch the module in [`project_config.h`](../../project_config.h).

| Function | GPIO | Block | PWM slice (`(gpio>>1)&7` for gpio &lt; 32) | Channel |
|----------|------|-------|---------------------------------------------|---------|
| OSC1 RESET | **19** | PIO0 | — | Active-low pad if `ENABLE_PIO_RESET_INVERT` |
| OSC2 RESET | **18** | PIO0 | — | Active-low pad if `ENABLE_PIO_RESET_INVERT` |
| OSC3 RESET | **15** | PIO0 | — | Active-low pad if `ENABLE_PIO_RESET_INVERT` |
| Sub 1 out | **8** | PIO2 SM0 (`ENABLE_SUBOSC_ENGINE2`); else PIO1 SM0 | — | Classic builds mix this pad directly; under engine2 it only has to exist |
| Sub 2 out | **9** | PIO2 SM1 (engine2) | — | Reuses planned Dist Drive pad while CV flags off |
| Logic combiner | **10** | PIO2 SM3 (engine2) | — | The sub section's mixer input. Was cal sense before GP6 |
| OSC1 RANGE | **17** | PIO1 SM2 or PWM | 0 | B (`RANGE0_PIO_DITHER_TEST` → PIO) |
| OSC2 RANGE | **16** | PIO1 SM3 or PWM | 0 | A (slice mode shares slice 0 with OSC1 RANGE) |
| OSC3 RANGE | **14** | PIO0 SM3 or PWM | 7 | A |
| PW (voice 0) | 3 | PWM | 1 | B |
| Cal sense | **6** (was 10; A/B header spare) | GPIO in | — | — |
| WeAct KEY | **23** | `USER_KEY_PIN`: hold = MIDI 69 / A440 | — | — |
| Analog board-fix | **24** | WeAct `BOARD_FIX_PIN` OUT HIGH. Pico/Pico 2: VBUS sense, not driven | — | — |
| SMPS Power Save | **23** | Pico/Pico 2 `SMPS_PS_PIN` OUT HIGH (RT6150 PWM) | — | — |

**All three oscillators must stay on PIO0.** A GPIO's function select names exactly one PIO
block, so oscillators split across blocks cannot share a reset pin — the second
`pio_gpio_init()` steals the pin from the first, which silently breaks hard sync. OSC1 and
OSC2 swap SM indices depending on `syncMode` (`assign_sm_mapping()`) so the master always
outranks its slave; OSC3 is always SM2. `pio_topology_report()` verifies this at runtime.

**Sub engine (`ENABLE_SUBOSC_ENGINE2`):** GP8/GP9 are adjacent, which is what the combiner's
`in pins, 2` needs, and sub 2 is the upper of the two so sub 1 is the low bit of its truth
table. Its result goes out on GP10, and because the operator list includes pass-throughs of
either sub that is the only pad the carrier has to mix. Enabling `ENABLE_CV_OUTS` /
`ENABLE_VOICE_AUX` would double-book GP9 (Dist Drive / OSC3 level) — renegotiate then; GP26
(Dist Mix / Sub level) is no longer involved. Detail:
[`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §9.

PIO block budget with engine2: **PIO0** oscillators (25–27) + RANGE dither 4 (29–31),
**PIO1** noise only (12) + RANGE dither (16) — SM0 free for `ENABLE_PIO_MIDI`,
**PIO2** `subosc_logic` + `subosc_seg` (26). Without engine2, classic sub stays on PIO1 and
PIO2 stays reserved for MIDI. RANGE SMs: pio1 SM2/SM3 + pio0 SM3. See
[`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §3.2 / §4.4.

Full detail on the programs, the period model, sync modes and phase align:
[`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).

---

## New CV / mux (provisional)

| Function | GPIO | PWM slice | Ch | Notes |
|----------|------|-----------|----|-------|
| Cutoff 0 | **15** | 7 | B | Not on slice 1 with PW |
| Cutoff 1 | **4** | 2 | A | `NUM_FILTERS=2` |
| Resonance 0 | **5** | 2 | B | Same slice as cut1 (shared wrap OK for CV) |
| Resonance 1 | **7** | 3 | B | **Shares slice 3 with RANGE OSC2 (GP22)** — firmware scales duty into `DIV_COUNTER` |
| VCA | **11** | 5 | B | |
| Dist Drive | **9** | 4 | B | **Conflict with Sub 2** while engine2 is on. Solo-B / `ENABLE_CV_OUTS` without aux. Dual-MCU: `ENABLE_VOICE_AUX` (no local drive); aux pins in [`VOICE-AUX/docs/README.md`](../../VOICE-AUX/docs/README.md) |
| Dist Mix | **26** | 5 | A | Shares slice 5 with VCA on solo-B map |
| OSC1 level | **16** | 0 | A | Shares slice 0 with RANGE OSC3 — firmware scales into `DIV_COUNTER` |
| OSC2 level | **18** | 1 | A | Shares slice 1 with PW — firmware scales into `DIV_COUNTER_PW` |
| OSC3 level | **9** / **32** | — | — | Dual-MCU (`ENABLE_VOICE_AUX`): reuse Dist Drive **GP9** (conflicts with Sub 2). Solo-B: **GP32** |
| Sub level | **26** / **33** | — | — | Dual-MCU: reuse Dist Mix **GP26**. Solo-B: **GP33** |

| Function | GPIO | Notes |
|----------|------|-------|
| 74HC595 DATA | **12** | Dual daisy-chain → 3× DG411 wave select ([`WAVE_MUX.md`](WAVE_MUX.md)) |
| 74HC595 LATCH | **13** | |
| 74HC595 CLK | **14** | |
| AS3320 mode | *TBD* | Dual-MCU → **RP2040**; solo-B → DCO spares — [`FILTER_ROUTING.md`](FILTER_ROUTING.md) |

**Wave mux:** 2× 74HC595 drive 3× DG411 (OSC1–3 × Saw/Pulse/Tri). Provisional bits 0–8; 9–15 unused. Active-low.

**I2C level DAC dropped** — osc/sub levels are PWM → analog level VCAs (same 12-bit CV domain as other outs). Soft bases still update with `ENABLE_CV_OUTS` off.

**Do not use for level PWM:** GP2 (aliases GP18), GP6 (aliases RANGE OSC2 GP22).

**Spare / TBD (DCO):** GP25 is Pico LED (not on header — do not use for cal sense). GP6 is temporarily cal sense (`DCO_calibration_pin` A/B). GP27/28 are free ADC pads (only claimed by long-disabled `USE_ADC_*`). Solo-B candidates for AS3320 mode.

**GP2** — PIO1 LFSR white bitstream when `ENABLE_NOISE_OUT` (listen/scope; AC-couple).

---

## Pin occupancy summary (provisional full map)

| GPIO | Role |
|------|------|
| 0,1 | PIO MIDI (or HW MIDI interim) |
| 3 | PW PWM |
| 4 | Cutoff 1 PWM |
| 5 | Resonance 0 PWM |
| 7 | Resonance 1 PWM |
| 8 | Sub 1 square (PIO2 SM0 engine2 / PIO1 SM0 classic; classic builds mix this pad) |
| 9 | Sub 2 square (PIO2 SM1) **or** Dist Drive / OSC3 level when CV/aux flags on |
| 6 | Cal sense A/B (`DCO_calibration_pin`; was spare — avoid as level PWM / aliases RANGE OSC2) |
| 10 | Logic combiner out (PIO2 SM3) — the engine2 sub mixer input; was cal sense |
| 11 | VCA PWM |
| 12–14 | Dual 74HC595 → DG411 wave mux |
| 15 | OSC3 RESET **and** Cutoff 0 PWM (provisional CV) — check carrier before enabling both |
| 16 | OSC1 level PWM (slice 0 A w/ RANGE OSC3) / RANGE OSC2 |
| 14,16,17 | RANGE ×3 (`RANGE_PINS[]`; PIO dither or PWM slice) |
| 18 | OSC2 RESET **and** OSC2 level PWM (slice 1 A w/ PW) — same dual-role caution |
| 19 | OSC1 RESET |
| 20,21 | HW UART Input |
| 23 | WeAct: onboard KEY (A440). Pico/Pico 2: SMPS PS, OUT HIGH |
| 24 | WeAct: analog board-fix OUT HIGH. Pico/Pico 2: VBUS sense, not driven |
| 25 | Pico LED (not on header) |
| 26 | Dist Mix / Sub level when CV/aux flags on |
| 32,33 | OSC3 / Sub level (solo RP2350B provisional) |
| 2 | Noise LFSR out (PIO1 SM1) when `ENABLE_NOISE_OUT` |

Approx **26** GPIOs used with dist CVs → stock Pico 2 is tight; **RP2350B recommended** for production (solo level pins GP32/33).

---

## PWM mux cautions

- Formula (SDK): slice = `(gpio >> 1) & 7` for gpio &lt; 32; channel = `gpio & 1`.
- GPIOs that differ by **16** can alias the same slice/channel — do not use both as PWM.
- Channels on one slice share **wrap/clock**; duty is independent. CV pairs (cut1+reso0 on slice 2) are intentional.

---

## Feature flags (code)

```text
ENABLE_SUBOSC_ENGINE2    // RP2350 default: two subs (GP8/9) + logic combiner out (GP10) on pio2
ENABLE_CV_OUTS           // PWM VCF/VCA/reso + OSC1..3/Sub level writers — landed (PWM.ino / cv_out.ino)
ENABLE_WAVE_MUX          // dual 595 → DG411 per-osc Saw/Pulse/Tri — landed (wave_mux.ino)
ENABLE_VOICE_AUX         // Dist/mode on RP2040; DCO reuses GP9/26 for OSC3/Sub levels (GP9 conflicts with engine2)
ENABLE_PIO_RESET_INVERT  // RESET pad active-low (DG411 discharge); OUTOVER+INOVER — landed (state_machines.ino)
ENABLE_NOISE_OUT         // GP2 = PIO1 LFSR white bitstream (~80 kHz) for listen/scope
ENABLE_PIO_MIDI          // DIN on PIO UART — Phase 5; with engine2, pio1 SM0 is free for it
```
PCM5102 I2S noise listen lives on **VOICE-AUX** (see [`../../VOICE-AUX/docs/I2S_NOISE.md`](../../VOICE-AUX/docs/I2S_NOISE.md)).

Uncomment Phase 3 HW flags in `DCO.ino` as needed. Turning on CV/aux while engine2 is on
requires a pin renegotiation for GP9 (and review RESET vs cutoff/level aliases on
GP15/GP18). The Input link on Serial2 is unconditional: the `ENABLE_INPUT_UART`,
`ENABLE_SCREEN_UART` and `ENABLE_LEGACY_MAINBOARD_LINK` flags were removed along with the
Mainboard and SerialPIO paths.
