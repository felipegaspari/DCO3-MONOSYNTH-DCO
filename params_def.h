#ifndef PARAMS_DEF_H
#define PARAMS_DEF_H

#include <stdint.h>

// Central definition of all parameter IDs used across MCUs.
// This DCO header mirrors the mainboard's params_def.h numeric values
// so that parameters sent over Serial are interpreted consistently.
//
// IMPORTANT:
//   - Do not change numeric values of existing IDs.
//   - New parameters should get new, unused numbers.
//   - The meaning of each ID (name + number) should be stable across MCUs.

enum ParamId : uint16_t {
  // --- Per-osc analog wave enables (74HC595 → DG411). See docs/WAVE_MUX.md ---
  PARAM_OSC1_SAW_ENABLE          = 1,   // was PARAM_SAW_STATUS
  PARAM_OSC1_PULSE_ENABLE        = 2,   // was PARAM_SAW2_STATUS
  PARAM_OSC1_TRI_ENABLE          = 3,   // was PARAM_TRI_STATUS
  PARAM_SINE_STATUS              = 4,   // deprecated (no mux role); keep ID
  // 5, 6: unused (were PARAM_SQR1/SQR2_STATUS) — reserved, do not reuse casually

  PARAM_RESONANCE_COMPENSATION   = 7,   // mainboard-local
  PARAM_VCA_ADSR_RESTART         = 8,   // mainboard-local
  PARAM_VCF_ADSR_RESTART         = 9,   // mainboard-local

  // --- Shared routing / oscillator parameters -------------------------
  PARAM_ADSR3_TO_OSC_SELECT      = 10,

  PARAM_LFO1_WAVEFORM            = 11,
  PARAM_LFO2_WAVEFORM            = 12,

  PARAM_OSC1_INTERVAL            = 13,  // octave_shift (global); id kept for wire compat
  PARAM_OSC2_INTERVAL            = 14,

  PARAM_OSC2_DETUNE_VAL          = 15,
  PARAM_LFO2_TO_OSC2              = 16,

  PARAM_OSC_SYNC_MODE            = 17,

  PARAM_PORTAMENTO_TIME          = 18,

  // --- Mainboard-local filter/velocity routing ------------------------
  PARAM_VCF_KEYTRACK             = 19,
  PARAM_VELOCITY_TO_VCF          = 20,
  PARAM_VELOCITY_TO_VCA          = 21,
  // Oscillator / sub mix levels (PWM → level VCAs; not per-waveform).
  PARAM_OSC1_LEVEL               = 22,
  PARAM_OSC2_LEVEL               = 23,
  PARAM_SUB_LEVEL                = 24,
  PARAM_OSC3_LEVEL               = 38,

  // --- Shared calibration / voice mode --------------------------------
  PARAM_CALIBRATION_VALUE        = 25,

  PARAM_VOICE_MODE               = 26,
  PARAM_UNISON_DETUNE            = 27,

  PARAM_ANALOG_DRIFT_AMOUNT      = 28,
  PARAM_ANALOG_DRIFT_SPEED       = 29,
  PARAM_ANALOG_DRIFT_SPREAD      = 30,

  PARAM_SYNC_MODE                = 31,

  // 32: DCO-only portamento mode selector (currently local to DCO)
  PARAM_PORTAMENTO_MODE          = 32,

  // DCO3 monosynth OSC3 (new IDs — wire on Mainboard/Input/Screen later)
  PARAM_OSC3_INTERVAL            = 33,
  PARAM_OSC3_DETUNE_VAL          = 34,
  PARAM_LFO2_TO_OSC3              = 35,

  // 36: sync flavour. 0 = hard sync (master sidesets onto the slave's reset pin);
  // 1..3 = soft sync with that many trailing polled ramp chunks (~40%/67%/86% receptive).
  PARAM_SOFT_SYNC                = 36,

  // 37: sub-oscillator divide. 0 = off, 2 = one octave down, 4 = two octaves.
  PARAM_SUBOSC_DIVIDE            = 37,

  // --- LFO routing (shared) -------------------------------------------
  PARAM_LFO1_TO_DCO              = 40,
  PARAM_LFO1_SPEED               = 41,
  PARAM_LFO2_SPEED               = 42,

  // --- Mainboard-only VCA routing ------------------------------------
  PARAM_VCA_LEVEL                = 43,
  PARAM_LFO1_TO_VCA              = 44,

  // --- PWM / ADSR to PWM / detune (shared with DCO) -------------------
  PARAM_LFO2_TO_PW               = 45,
  PARAM_ADSR3_TO_PWM             = 46,
  PARAM_ADSR3_TO_DETUNE1         = 47,

  // ADSR curve shaping (mainboard-local only)
  PARAM_ADSR1_ATTACK_CURVE       = 48,
  PARAM_ADSR1_DECAY_CURVE        = 49,
  PARAM_ADSR2_ATTACK_CURVE       = 50,
  PARAM_ADSR2_DECAY_CURVE        = 51,

  // Post-LP distortion CVs (Drive VCA + dry/wet Mix). See docs/DISTORTION.md.
  PARAM_DIST_DRIVE               = 52,
  PARAM_DIST_MIX                 = 53,

  // AS3320 multimode select (0..N). Dual-MCU: RP2040 aux; solo-B: DCO. See docs/FILTER_ROUTING.md.
  PARAM_FILTER_MODE              = 54,

  // FX placeholders (RP2040 aux in dual-MCU builds). IDs reserved; not wired yet.
  // PARAM_FX_PROGRAM             = 55,
  // PARAM_FX_MIX                 = 56,

  // Mod matrix: 8 slots × (source, dest, depth). See docs/MOD_MATRIX.md.
  // Source 0..15 (0xFF/out-of-range = empty); dest 0..9; depth bipolar int16.
  // Pitch dest (9): ±1023 → ±1 octave (see mod_matrix.h MOD_PITCH_DEPTH_FULL).
  PARAM_MOD_SLOT0_SOURCE         = 60,
  PARAM_MOD_SLOT0_DEST           = 61,
  PARAM_MOD_SLOT0_DEPTH          = 62,
  PARAM_MOD_SLOT1_SOURCE         = 63,
  PARAM_MOD_SLOT1_DEST           = 64,
  PARAM_MOD_SLOT1_DEPTH          = 65,
  PARAM_MOD_SLOT2_SOURCE         = 66,
  PARAM_MOD_SLOT2_DEST           = 67,
  PARAM_MOD_SLOT2_DEPTH          = 68,
  PARAM_MOD_SLOT3_SOURCE         = 69,
  PARAM_MOD_SLOT3_DEST           = 70,
  PARAM_MOD_SLOT3_DEPTH          = 71,
  PARAM_MOD_SLOT4_SOURCE         = 72,
  PARAM_MOD_SLOT4_DEST           = 73,
  PARAM_MOD_SLOT4_DEPTH          = 74,
  PARAM_MOD_SLOT5_SOURCE         = 75,
  PARAM_MOD_SLOT5_DEST           = 76,
  PARAM_MOD_SLOT5_DEPTH          = 77,
  PARAM_MOD_SLOT6_SOURCE         = 78,
  PARAM_MOD_SLOT6_DEST           = 79,
  PARAM_MOD_SLOT6_DEPTH          = 80,
  PARAM_MOD_SLOT7_SOURCE         = 81,
  PARAM_MOD_SLOT7_DEST           = 82,
  PARAM_MOD_SLOT7_DEPTH          = 83,

  // OSC2/OSC3 wave enables (OSC1 uses IDs 1–3)
  PARAM_OSC2_SAW_ENABLE          = 84,
  PARAM_OSC2_PULSE_ENABLE        = 85,
  PARAM_OSC2_TRI_ENABLE          = 86,
  PARAM_OSC3_SAW_ENABLE          = 87,
  PARAM_OSC3_PULSE_ENABLE        = 88,
  PARAM_OSC3_TRI_ENABLE          = 89,

  // --- Misc / control / UI flags ------------------------------------
  // Calibration mode selector (screen/UI only for now)
  PARAM_CALIBRATION_MODE         = 101,

  // Global/manual control flags (input+screen; DCO may ignore)
  PARAM_FADERS_CONTROL_MANUAL    = 120,
  PARAM_FADER_ROW1_CONTROL_MANUAL= 121,
  PARAM_FADER_ROW2_CONTROL_MANUAL= 122,
  PARAM_VCF_POTS_CONTROL_MANUAL  = 123,
  PARAM_PWM_POTS_CONTROL_MANUAL  = 124,
  PARAM_ALL_CONTROLS_MANUAL      = 125,

  PARAM_ADSR3_ENABLED            = 126,
  PARAM_FUNCTION_KEY             = 127,

  PARAM_VCA_POTS_CONTROL_MANUAL  = 128,
  PARAM_POTS_CONTROL_MANUAL      = 129,

  // UI navigation / calibration helper parameters (screen-focused)
  PARAM_UI_MENU_POSITION         = 190,
  PARAM_UI_CALIBRATION_DISMISS   = 199,
  PARAM_UI_CALIBRATION_MENU_MODE = 200,

  // Pulse width (was Input 'f' block). Voice engine stores PW[0] = value / 4.
  PARAM_PW_VALUE                 = 210,
  PARAM_LFO3_SPEED               = 211,
  PARAM_LFO3_WAVEFORM            = 212,
  PARAM_ADSR3_RESTART            = 214,
  PARAM_VCA_LEVEL_ALT            = 215,

  // Additive LFO1 pitch depth per osc (stacks on PARAM_LFO1_TO_DCO global bus).
  PARAM_LFO1_TO_OSC1             = 216,
  PARAM_LFO1_TO_OSC2             = 217,
  PARAM_LFO1_TO_OSC3             = 218,
  // LFO2 coarse pitch per osc (0..511; LFO1 curve + amp scale baked into depth at apply).
  PARAM_LFO2_TO_OSC2_COARSE      = 219,
  PARAM_LFO2_TO_OSC3_COARSE      = 220,

  // Character amount (0..128). dco_control Character tab; storage only for now.
  PARAM_CHARACTER                = 221,

  // EnvVCA → VCA amount (was Input 'e' block).
  PARAM_ADSR1_TO_VCA             = 222,

  // EnvDCO → pitch tap: 0 unipolar (default), 1 centered ((env−16384)<<1; mid S ≈ note, ±2 oct @ full CW).
  PARAM_ADSR3_PITCH_MODE         = 223,

  // --- Calibration flags (shared) ------------------------------------
  PARAM_CALIBRATION_FLAG         = 150,
  PARAM_MANUAL_CALIBRATION_FLAG  = 151,
  PARAM_MANUAL_CALIBRATION_STAGE = 152,
  PARAM_MANUAL_CALIBRATION_OFFSET= 153,

  // 154: gap from DCO — TX to Input on Serial2; Input relays it to the Screen
  PARAM_GAP_FROM_DCO             = 154,

  // 155: manual calibration offsets reported from DCO back to Input.
  PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO = 155,

  // 156: explicit "store manual calibration offsets" command.
  PARAM_MANUAL_CALIBRATION_STORE = 156,

  // 160: bench / debug trigger (DCO-local; dco_control Diagnostics + Calibration + Character).
  // 1 = PIO topology report, 2 = period probe at a low divider,
  // 3 = period probe at a high divider. See DCO/docs/PIO_OSCILLATORS.md section 12.
  // 10 = dump profiler once, 11 = reset profiler, 12 = toggle ~1 Hz dump
  // (RUNNING_AVERAGE builds only). See DCO/docs/BENCHMARKING.md.
  // 13 = SRAM / heap / per-core stack dump (ENABLE_MEM_DIAG; runtime polls on).
  // 14 / 15 = mem_diag loop polls off / on. See MEMORY.md.
  // 20–22 amp-comp method, 24–25 amp benches, 26–27 note retrig, 28–29 pitch benches.
  // 30 = force-seed fake amp-comp + PW tables.
  // 32–33 clkdiv GOLD_REF / GOLD_LIVE / FLOAT_LIVE / Q16 / Q8 / FAST_Q4
  // (both voice engines; RUNNING_AVERAGE). 2=Q16 shipping, 3=Q8 A/B.
  // 200–50000 = set pioPulseLength (reset pulse Y cycles); unsigned 16-bit on wire.
  // Also reloads running SMs via pio_defer_request_reset_pulse_all().
  // Packed Character jitter setters (unsigned 16-bit, hi|lo, lo = 0..128):
  //   0xC8xx ampCompJitter, 0xCAxx pitchJitter, 0xCBxx pulsewidthJitter.
  PARAM_DEBUG_COMMAND            = 160
};

#endif  // PARAMS_DEF_H



