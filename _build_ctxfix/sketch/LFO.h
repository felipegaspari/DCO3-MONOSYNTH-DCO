#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.h"
#ifndef __LFO_H__
#define __LFO_H__

// Bipolar Q15 bus (±32767 ≈ ±1.0). mo-lfo always exposes getWaveQ15/setAmplQ15;
// MO_LFO_USE_Q15 is only a preferred-path hint (set here for the sketch TU).
// Live waves are always full-scale Q15 — ctor dacSize does not affect getWaveQ15.
// Depth scales below set musical travel; see docs/LFO.md.
#ifndef MO_LFO_USE_Q15
#define MO_LFO_USE_Q15 1
#endif
#ifndef MO_LFO_SRAM_HOT
#define MO_LFO_SRAM_HOT 1
#endif

#define LFO_SINE_TABLE_BITS 9

// Same isolated-project pattern as ADSR_Bezier (relative _build_libs path).
// mo-lfo has a .cpp — it is compiled via #include in LFO.ino (not sketchbook).
#include "_build_libs/mo-lfo/mo-lfo.h"
// After Mainboard absorption, DCO is the sole LFO clock:
//   LFO1 → pitch (volatile Q24 arrays) + EnvVCA depth (LFO1toVCA)
//   LFO2 → OSC2/3 pitch (fine+coarse folded) + PW + EnvVCF depth (LFO2toVCF)
//   drift LFOs → pitch drift + soft VCF_DRIFT
// Do not run a second LFO engine on Mainboard in hub mode (Phase 5 archives it).

enum Lfo1PitchSlot : uint8_t {
  LFO1_PITCH_OSC1 = 0,
  LFO1_PITCH_OSC2 = 1,
  LFO1_PITCH_OSC3 = 2,
  LFO1_PITCH_SLOT_COUNT = 3,
};

enum Lfo2PitchSlot : uint8_t {
  LFO2_PITCH_OSC2 = 0,
  LFO2_PITCH_OSC3 = 1,
  LFO2_PITCH_SLOT_COUNT = 2,
};

// =============================================================================
// Depth scales (tune musical travel — live wave is always full-scale Q15)
// Hot:  mod_q24 = (wave_q15 * depth_q24) >> 15
// Bake: depth_q24 = lfo_pitch_depth_q24(amt, SCALE)
//       amt = expConverterFloat(panel) / 275000  (octave-fraction unit)
// Larger SCALE → deeper mod at the same panel setting.
// =============================================================================

// LFO1 → pitch (LFO1toDCO + per-osc). Also used for LFO2 coarse (same travel as LFO1).
static constexpr int32_t LFO1_PITCH_DEPTH_SCALE = 1700;

// LFO2 → fine pitch (OSC2 / OSC3).
static constexpr int32_t LFO2_PITCH_DEPTH_SCALE = 512;

// EnvDCO (ADSR3) → pitch. Tune max travel here (docs/LFO.md).
// Full env × full |PARAM_ADSR3_TO_DETUNE1| (511) → this many octaves in Q24
// (additive: depth_q24 ≈ N<<24 moves the pitch sum by ~N octaves).
// Knob uses signed expConverterFloat(|v|, 500), normalized so |v|=511 hits exactly this max.
// Mid-knob is quieter than the old linear /1080000 bake; env is linear Q15 (no linToLog).
static constexpr float ADSR_PITCH_MAX_OCTAVES = 2.0f;
static constexpr uint16_t ADSR_PITCH_DEPTH_PANEL_FULL = 511;

// Analog drift → pitch:
//   drift_pitch_scale_q24 = analogDrift * DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE
// Hot: (LFO_DRIFT_LEVEL_q15 * drift_pitch_scale_q24) >> 15
static constexpr int32_t DRIFT_PITCH_DEPTH_SCALE = 1000;
static constexpr int32_t DRIFT_PITCH_UNIT_Q24 =
  (int32_t)(0.0000005f * (float)(1 << 24) + 0.5f);

static inline int32_t lfo_pitch_depth_q24(float amt, int32_t depth_scale) {
  return (int32_t)(amt * (float)depth_scale * (float)(1 << 24) + 0.5f);
}

// Synth-side helper (not mo-lfo): mod_q24 = (wave_q15 * depth_q24) >> 15.
// 32-bit split avoids int64 mul on Cortex-M0+.
static inline int32_t applyDepthQ24(int16_t wave_q15, int32_t depth_q24) {
  const int32_t w = (int32_t)wave_q15;
  const int32_t hi = depth_q24 >> 15;
  const int32_t lo = depth_q24 - (hi << 15);
  return w * hi + ((w * lo) >> 15);
}

// mo-lfo ctor dacSize is ignored on the Q15 path (getWaveQ15 / setAmplQ15).
static constexpr int LFO_DAC_SIZE_UNUSED = 1;

// Runtime drift pitch scale (baked in apply_param_analog_drift_amount).
int32_t drift_pitch_scale_q24 = 0;

//////////////// LFO instances ////////////////////////////////////////

lfo LFO1_class(LFO_DAC_SIZE_UNUSED);
lfo LFO2_class(LFO_DAC_SIZE_UNUSED);

lfo LFO_DRIFT_CLASS[NUM_OSCILLATORS] = {
  lfo(LFO_DAC_SIZE_UNUSED),
  lfo(LFO_DAC_SIZE_UNUSED),
  lfo(LFO_DAC_SIZE_UNUSED)
};


/////////////////////////////////////////////////////////////////
byte LFO_DRIFT_WAVEFORM = 2;
float LFO_DRIFT_SPEED_OFFSET[NUM_OSCILLATORS];
// Drift LFO mailbox: Core0 DRIFT_LFOs() publishes Q15 then DMB; Core1 snapshots
// once per voice_task into locals. Polarity: negate Q15 for Mainboard polarity.
volatile int16_t LFO_DRIFT_LEVEL[NUM_OSCILLATORS];


// LFO1/LFO2 mailbox: Core0 LFO1()+LFO2() publish Q15 + pitch_mod then DMB;
// Core1 snapshots once per voice_task / update_CV_outs into locals.
volatile int16_t LFO1Level;
byte LFO1Waveform = 3;
float LFO1Speed = 50;
// Full-scale octave travel in Q24: applyDepthQ24(level_q15, depth_q24).
// Param apply: lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE).
int32_t LFO1toDCO_q24 = 0;
// Additive per-osc LFO1 pitch depths (each stacks on LFO1toDCO_q24 in LFO1()).
int32_t LFO1toOSC1_q24 = 0;
int32_t LFO1toOSC2_q24 = 0;
int32_t LFO1toOSC3_q24 = 0;
// Core 0 writes, Core 1 reads (mailbox + DMB). Q24 log-frequency additive pitch modifiers.
volatile int32_t lfo1_pitch_mod_q24[LFO1_PITCH_SLOT_COUNT];
volatile int32_t lfo2_pitch_mod_q24[LFO2_PITCH_SLOT_COUNT];

volatile int16_t LFO2Level;
byte LFO2Waveform = 2;  // default triangle
float LFO2Speed;
// Per-osc LFO2 fine pitch depths (OSC2/3; param 0..255).
int32_t LFO2toOSC2_q24 = 0;
int32_t LFO2toOSC3_q24 = 0;
// Coarse LFO2 pitch depths (0..511; baked with LFO1_PITCH_DEPTH_SCALE).
int32_t LFO2toOSC2_coarse_q24 = 0;
int32_t LFO2toOSC3_coarse_q24 = 0;
volatile uint16_t LFO2toPW;

uint16_t LFO1SpeedVal;
uint16_t LFO2SpeedVal;
uint16_t LFO1toDCOVal;

void LFO1();
void LFO2();
void DRIFT_LFOs();


#endif
