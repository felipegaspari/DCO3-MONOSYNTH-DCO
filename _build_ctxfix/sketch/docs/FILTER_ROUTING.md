#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/docs/FILTER_ROUTING.md"
# Filter routing and AS3320 multimode (concept)

**Status:** hardware concept for breadboard / PCB — not a finished schematic. Firmware: `PARAM_FILTER_MODE` **54**; dual-MCU GPIO stub on [`VOICE-AUX/`](../../VOICE-AUX/).

Related: [`DISTORTION.md`](DISTORTION.md) (Drive/Mix stage), [`PINOUT.md`](PINOUT.md) (Cut0/1, Res0/1 PWM), [`DUAL_MCU.md`](DUAL_MCU.md) (AS3320 mode GPIOs → RP2040 aux in dual-MCU build; DCO keeps code for solo-B).

References:

- [AS3320 datasheet](https://cabintechglobal.com/pdf/ALFA_RPAR_AS3320.pdf) (Figs. 1–3: LP / HP / BP)
- [Electric Druid — CEM3320 filter designs](https://electricdruid.net/cem3320-filter-designs/)
- [Electric Druid — Multimode filters, Part 1 (reconfigurable)](https://electricdruid.net/multimode-filters-part-1-reconfigurable-filters/)
- Elka Synthex / Craig Anderton “Multiple Identity” / hermflink 3320VCF patterns

---

## Intended voice chain

```text
Osc / sub mix (level VCAs)
  → SSI2144          fixed 24 dB/oct lowpass     Cut0 / Res0
  → Distortion       AS2164 Drive + Mix          dry tap = SSI2144 out
  → AS3320           digitally switched multimode Cut1 / Res1
  → Main Amp VCA
  → FV-1 (later)
  → outs
```

| IC | Role |
|----|------|
| **SSI2144** | Ladder-family 4-pole **LP only** — grit and character into distortion |
| **AS2164** | Distortion Drive + Mix VCAs (see [`DISTORTION.md`](DISTORTION.md)) |
| **AS3320** | CEM3320-class **reconfigurable** 4-pole — LP / BP / hybrid after grit |
| **DG411** (or 4066-class) | Analog switches that rewire AS3320 stages 1–2 |

One AS3320 **is** multimode; it does not need a second AS3320 for that. Multimode here means **rewiring the four OTA stages**, not picking simultaneous SVF taps.

---

## Why not “swap LP and HP around distortion” with one DG411?

A clean **LP → dist → HP** vs **HP → dist → LP** order swap needs **eight** SPST paths (four closed per mode). One DG411 has only four. That idea is **out of scope**.

The DG411 (or cheaper analog quad) is used only to **digitally select AS3320 mode**.

---

## How AS3320 multimode works

### Inside the chip

Four independent filter cells + shared expo frequency CV (`VCFI`) + on-chip resonance VCA (`Vres` / `Ires`).

Each cell (conceptually):

```text
          ┌─ OTA (gm set by freq CV) ─┐
IN_n ──►──┤                           ├──► buffer ──► OUT_n
          └─ external C on Cap_n ─────┘
```

Cascade externally: `OUT1 → IN2 → OUT2 → IN3 → OUT3 → IN4` (with datasheet input attenuators). PDIP-18 pins: `INn`, `Capn`, `OUTn` for n = 1…4.

**Multimode is not a post-mux of three live outs.** It is switching each pole between the datasheet **lowpass** and **highpass** stage circuits.

### One pole: LP vs HP

**Lowpass** (datasheet Fig. 1) — “cap to ground”:

```text
          Rin                 ┌── OTA ──┐
VIN ──►──/\/\/──●─────────────┤         ├──► buffer ──► VOUT
                │             └────┬────┘
               Rf                  C → GND
            (feedback)
```

**Highpass** (datasheet Fig. 2) — R and C roles swapped:

```text
                    C
VIN ──►─────────────┤├────────●──► buffer ──► VOUT
                              │
                         OTA as "R to gnd" (gm ← freq CV)
                              │
                             GND
```

**Bandpass** (Fig. 3) is not a third cell type: cascade **HP poles then LP poles** (HP first so the LP kills HF noise). Example: stages 1–2 HP + stages 3–4 LP → ~12 dB/oct BP.

### Per-stage SPDT

| State | Cap / network | Stage |
|-------|---------------|--------|
| LP | `Cap` node → **GND** (LP `Rin` / `Rf`) | Lowpass |
| HP | `Cap` **in series** with audio into the cell | Highpass |

Each stage needs an **SPDT** (two complementary SPST). One quad SPST package = two SPDT → **two stages** switchable.

```text
                    SW_A (on for LP)
 Cap_pin ──────────────●──────────────── GND
                       │
                    SW_B (on for HP)
                       │
                       └── HP series path / stage input
```

Copy datasheet Fig. 1 vs Fig. 2 around the same `INn` / `Capn` / `OUTn`, then replace the hardwired C with that SPDT. Drive complementary controls; **break-before-make** (~50–200 µs) when changing mode.

---

## Switches are in the audio path

The mode switches sit on **signal nodes**, not on digital-only lines.

- **LP:** throw ties the integrator / `Cap` node to GND — that node carries filtered **audio AC**.
- **HP:** throw puts C in series with the signal — **audio current goes through the switch**.

| Part | OK? | Notes |
|------|-----|--------|
| **DG411 / DG412** | Yes | Best audio: low Ron, ±15 V, low charge injection |
| **CD4066 / 74HC4066** | Yes | Cheaper analog switch; common in CEM3320 multimode builds. Watch Ron, supply range, distortion |
| Other bilateral quads (DG212, …) | Yes | Same role |
| Logic mux / bare GPIO (`74HC157`, …) | **No** | Wrong domain; not for audio nodes |

“Cheaper” means a cheaper **analog** switch (4066-class), not a digital logic IC.

---

## v1 switching scope (one quad SPST)

- Stages **1–2**: switched LP ↔ HP  
- Stages **3–4**: hardwired lowpass  

```text
dist out → [S1 sw] → [S2 sw] → [S3 LP fixed] → [S4 LP fixed] → buffer → VCA
                ↑ resonance VCA (may need invert in BP — see below)
```

| Mode | S1 | S2 | S3 | S4 | Response |
|------|----|----|----|----|----------|
| `LP24` | LP | LP | LP | LP | 24 dB/oct lowpass |
| `BP12` | HP | HP | LP | LP | 12 dB/oct bandpass |
| `HP6_LP18` | HP | LP | LP | LP | 6 dB HP + 18 dB LP |
| *(optional)* | LP | HP | LP | LP | Extra hybrid color |

Example 2-bit GPIO encode (each bit steers one stage’s SPDT pair via `ctrl` / `!ctrl`):

| Code | Stage1 | Stage2 | Mode |
|------|--------|--------|------|
| 00 | LP | LP | `LP24` |
| 01 | HP | LP | `HP6_LP18` |
| 10 | LP | HP | optional |
| 11 | HP | HP | `BP12` |

**Dual MCU:** mode GPIOs live on the **RP2040** aux ([`DUAL_MCU.md`](DUAL_MCU.md)); cutoff/reso PWM stay on the RP2350. **Solo RP2350B:** use DCO spares from [`PINOUT.md`](PINOUT.md) (**GP2 / GP6 / GP25**). Match logic sense to part: DG411 is normally closed when IN is low; DG412 is NO — firmware must match.

---

## Resonance

- Loop uses the chip resonance VCA from the cascade output back toward the input (datasheet LP arrangement as starting point).
- In **BP** (HP then LP), the feedback path often needs an **op-amp invert** or Q collapses / misbehaves.
- High Q in BP can get loud; optional later: duck input with a spare AS2164 section as resonance rises. v1: invert if needed, trim levels.

Taking the output after stage 2 vs stage 4 (full Synthex) needs **extra** switches — not in the one-quad budget. Park for v2.

---

## Parts (mode section only)

| Qty | Part | Role |
|----:|------|------|
| 1 | AS3320 | Four poles + resonance |
| 1 | DG411 (or 4066-class quad) | Two SPDT for stages 1–2 |
| 2 | Inverters (or 2 extra GPIO) | Complementary switch drive |
| 1 | Op-amp section | Output buffer; optional reso invert |
| — | Passives | Datasheet Rin / Rf / C; `Ree` on VEE; matched Caps (≤1%) |
| 2 | Pico GPIO | Mode select |

SSI2144 + distortion passives/ICs are separate (see distortion sheet).

---

## Explicitly out of scope (this concept)

- Pole-mixing multimode (Xpander-style math on fixed taps)
- State-variable simultaneous LP + BP + HP from one core used as two series stages around dist
- Audio matrix to swap SSI2144 and AS3320 order around distortion
- Firmware param / MIDI CC for mode (add when GPIO is frozen)

---

## Next hardware steps

1. Pick switch IC (DG411 vs 4066) for ±12 V rail and level into AS3320.
2. Draw AS3320 from datasheet Fig. 1/2/3 with SPDTs on Cap1 / Cap2 networks.
3. Breadboard `LP24` / `BP12`; confirm reso invert for BP.
4. Assign mode GPIOs in [`PINOUT.md`](PINOUT.md); then a small firmware mode enum + break-before-make.
