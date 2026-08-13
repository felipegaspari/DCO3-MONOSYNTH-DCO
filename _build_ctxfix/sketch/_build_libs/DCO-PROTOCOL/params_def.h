#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/params_def.h"
#ifndef PARAMS_DEF_H
#define PARAMS_DEF_H

#include <stdint.h>

// Canonical definition of every parameter ID used across MCUs. Every board in
// both DCO3-MONOSYNTH and DCO4-REBORN compiles this one file out of the
// DCO-PROTOCOL library, so there is nothing to copy and nothing to drift.
// It is the union of both synths' parameters: a board simply has no handler for
// the IDs it does not implement (e.g. the sub-oscillator block 90–99 is
// DCO3-only, and 170–174 are handled by the DCO alone). Keeping one enum means a
// given number always means the same thing on every wire in both instruments.
//
// IMPORTANT:
//   - Do not change numeric values of existing IDs.
//   - New parameters should get new, unused numbers.
//   - The meaning of each ID (name + number) should be stable across MCUs.
//   - A new ID is live on every board as soon as it is added here, so bump this
//     library in both superprojects together; `DCO-CONTROL-PANEL/gen_midi_map.py
//     --check` diffs the host-side panel against this enum.

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

  // Voice allocation policy. One value covers both halves of the same decision:
  // in poly it picks which voice gets stolen when all of them are busy, in mono
  // it picks which held key sounds.
  //   0 round-robin        poly least-recently-used   mono last-note
  //   1 oldest             poly oldest trigger        mono first-note
  //   2 quietest           poly lowest EnvVCA level   mono last-note
  //   3 quietest keep low  as 2, spares lowest held   mono low-note
  //   4 quietest keep high as 2, spares highest held  mono high-note
  //   5 no stealing        poly drops the note-on     mono first-note, later keys never sound
  // Every mode prefers an idle voice, then a release tail, before stealing a
  // held note. See DCO/voices.ino voice_alloc() / DCO/midi.ino note_on().
  PARAM_VOICE_ALLOC_MODE         = 102,

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
  // Source 0..15 (0xFF/out-of-range = empty); dest 0..11; depth bipolar int16.
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

  // --- Sub-oscillators (ENABLE_SUBOSC_ENGINE2; RP2350 only) ---------------------------
  // Two subs, and the boolean combination of them is the output that gets mixed. See
  // docs/PIO_OSCILLATORS.md section 9. On builds without the engine, SUB1_DIVIDE falls back to
  // the single legacy sub (same as PARAM_SUBOSC_DIVIDE) and the rest are ignored.
  //
  // Divide: 0 = off, 1 = master rate (phase / PWM only), 2..8 master periods per sub period.
  // Odd ratios are legal and give non-octave subharmonics (3 = an octave and a fifth down).
  PARAM_SUB1_DIVIDE              = 90,
  PARAM_SUB2_DIVIDE              = 91,
  // Master: which oscillator's reset the sub locks to, 0..2 = OSC1..OSC3. Both subs on one
  // master gives harmonic pulse patterns; different masters gives ring-mod beating that tracks
  // their detune. 92 and 95 were the third sub's divide and phase, which never shipped.
  PARAM_SUB1_MASTER              = 92,
  PARAM_SUB2_MASTER              = 95,
  // Phase: rising-edge delay after the master's reset, 0..359 degrees of the *master* period
  // (shifting a sub by whole master periods is inaudible, so that is the useful range).
  PARAM_SUB1_PHASE               = 93,
  PARAM_SUB2_PHASE               = 94,
  // Width: duty in 1/256ths of the sub period, 1..255. 128 = the classic 50% square.
  PARAM_SUB1_WIDTH               = 96,
  PARAM_SUB2_WIDTH               = 97,
  // 98 and 100 are reserved: they were the third sub's width and the combiner's pair selector,
  // and the pair is fixed now that there are exactly two subs on adjacent pads.
  //
  // Boolean logic combiner: both subs in, one square out. Digital ring modulation, plus two
  // pass-throughs so this one pad carries everything the sub section can produce.
  // 0 = off, 1 = XOR, 2 = AND, 3 = OR, 4 = XNOR, 5 = NAND, 6 = NOR, 7 = sub 1, 8 = sub 2.
  PARAM_SUB_LOGIC_OP             = 99,

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

  // 191-194: Input -> Screen only. The filter pots are analog and live solely
  // inside the 'd' block, so they had no ParamId; these give the Screen a name
  // and a toast for a filter change the panel did not make (dco_control, MIDI
  // CC, preset recall). Never sent to the DCO, never persistable.
  PARAM_UI_CUTOFF                = 191,
  PARAM_UI_RESONANCE             = 192,
  PARAM_UI_ADSR2_TO_VCF          = 193,
  PARAM_UI_LFO2_TO_VCF           = 194,

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
  // 150: starts the blocking auto-calibration; the value selects the stage,
  // matching the Screen's calibration menu tabs:
  //   1 = amp-comp tables only  ("AUTO CALIBRATION",  Input menu pos 0)
  //   2 = PW center/limits only ("PW CALIBRATION",    Input menu pos 1)
  //   3 = both, PW then amp     ("FULL CALIBRATION",  Input menu pos 2)
  // Add 4 (5/6/7) to run the same stage at fine precision: slower, far more
  // careful measurements, and the amp stage then re-measures the stored table
  // instead of building a new one (so it needs a calibrated board). Panel
  // only; the Input/Screen menus always send 1/2/3.
  // Any other non-zero value runs the full pass at normal precision.
  // 0 cancels a running calibration (the loops poll the request and keep the
  // previous values of whatever stage was interrupted).
  PARAM_CALIBRATION_FLAG         = 150,
  PARAM_MANUAL_CALIBRATION_FLAG  = 151,
  // 152: manual-cal walk. DCO3: 0..8 (3 osc × saw/pulse/440). DCO4: 0..27
  // packed per voice pair (A: saw/tri/pulse-PW/440, B: saw/pulse/440).
  // Do not treat stage as an oscillator index. See cal_stage_*_n().
  PARAM_MANUAL_CALIBRATION_STAGE = 152,
  PARAM_MANUAL_CALIBRATION_OFFSET= 153,

  // 154: gap from DCO — TX to Input on Serial2; Input relays it to the Screen
  PARAM_GAP_FROM_DCO             = 154,

  // 155: manual calibration offsets reported from DCO back to Input.
  PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO = 155,

  // 156: explicit "store manual calibration offsets" command.
  PARAM_MANUAL_CALIBRATION_STORE = 156,

  // 157: Input -> Screen only, silent (outside the 150..155 toast range).
  // value = NUM_OSCILLATORS (board_model.h): 3 on the monosynth, 8 on the
  // 4x2 voice board. Sent at boot and again on manual-calibration entry so
  // the Screen's screen_cal_topology() (screen_target.h) tracks the synth
  // it's attached to without a per-project build flag.
  PARAM_UI_VOICE_TOPOLOGY        = 157,

  // 158: optional override for the 440 Hz amp-set stage (PC panel). Input
  // derives this from PARAM_MANUAL_CALIBRATION_STAGE (440 kind). 0 = trim
  // at the low starting note, 1 = 440 Hz (adjust PARAM_AMP_COMP_440 until
  // duty = 50%; the stored value anchors the FREQ_TRACE curve).
  PARAM_MANUAL_CALIBRATION_STEP  = 158,

  // 159: absolute range-PWM amp-comp value at 440 Hz for the oscillator
  // selected by PARAM_MANUAL_CALIBRATION_STAGE (osc = stage / 3). Range
  // AMP_COMP_440_MIN..MAX (700..2800), fits int16. Persisted together with
  // the offsets by PARAM_MANUAL_CALIBRATION_STORE.
  PARAM_AMP_COMP_440             = 159,

  // 161: per-oscillator duty target trim, in hundredths of a percent of duty
  // (signed, -500..500), for the oscillator selected by
  // PARAM_MANUAL_CALIBRATION_STAGE. The board's sense pin is a digital input
  // and reads its own thresholds, so "50% here" can be a fixed offset away
  // from "50% on a scope"; both amp-comp methods aim at 50% + this trim, so
  // dialling it once per oscillator nulls the output against the scope.
  // Persisted together with the offsets by PARAM_MANUAL_CALIBRATION_STORE.
  PARAM_AMP_COMP_DUTY_OFFSET     = 161,

  // 162: DCO4 A-oscillator pulse-PW substage. Encoder sets PW_CENTER for
  // that voice's PW channel (0..CAL_PW_CENTER_MAX). Persisted by STORE.
  PARAM_CAL_PW_CENTER            = 162,

  // --- Preset store / dump commands (DCO-local; tools/dco_control) -----
  // See preset_store.h for the record format and the '[dump]'/'[pdir]'/
  // '[preset]'/'[bulk]' text protocol on USB CDC.
  PARAM_PRESET_SAVE              = 170,  // value = slot 0..255: save live state
  PARAM_PRESET_LOAD              = 171,  // value = slot 0..255: recall slot
  PARAM_PRESET_DUMP              = 172,  // -1 = directory, 0..255 = slot record hex
  PARAM_CAL_DUMP                 = 173,  // 0/-1 = all cal tables, 1..5 = one (CAL_DUMP_*)
  // Host / panel: push Screen slot+name from DCO presetName[] without LittleFS recall.
  PARAM_UI_PRESET_SCROLL         = 174,  // value = slot 0..255: 17-byte Screen 'q' + PresetScroll

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
  // 43 = MCP4728 presence report (0x63/0x64/0x65). Diagnostic only — does not
  // gate live DAC writes. 44 = re-attach all three, then rewrite levels.
  // DCO3: handled on the DCO when ENABLE_MCP4728; else prints compiled-out.
  // DCO4: DCO forwards 43/44/45 over Serial2; Mainboard executes (same path as 42).
  // 45 = Mainboard profiler dump once (40/41 are amp-0 on the DCO, so they are
  // not forwarded; Mainboard still accepts 40 locally as dump).
  // 46 = PW CV probe: sweeps every PW channel and prints the duty each level
  // produces on the soloed oscillator, so a dead or mis-mapped PW CV is one
  // command. Needs manual calibration running. See CALIBRATION_PROCEDURE.md.
  // 200–50000 = set pioPulseLength (reset pulse Y cycles); unsigned 16-bit on wire.
  // Also reloads running SMs via pio_defer_request_reset_pulse_all().
  // Packed Character jitter setters (unsigned 16-bit, hi|lo, lo = 0..128):
  //   0xC8xx ampCompJitter, 0xCAxx pitchJitter, 0xCBxx pulsewidthJitter.
  PARAM_DEBUG_COMMAND            = 160
};

// Manual-cal stage walk. nOsc <= 3: uniform 3 (saw, pulse, 440).
// Else (DCO4): packed 7 per voice pair — A saw/tri/pulse-PW/440, B saw/pulse/440.
#ifndef CAL_STAGES_PER_OSC
#define CAL_STAGES_PER_OSC 3
#endif
#ifndef AMP_COMP_440_MIN
#define AMP_COMP_440_MIN 700
#define AMP_COMP_440_MAX 2800
#endif
#ifndef CAL_PW_CENTER_MAX
#define CAL_PW_CENTER_MAX 1023  // DIV_COUNTER_PW - 1
#endif

enum CalStageKind : uint8_t {
  CAL_KIND_SAW = 0,
  CAL_KIND_TRI,
  CAL_KIND_PULSE,
  CAL_KIND_PULSE_PW,
  CAL_KIND_440
};

static inline uint8_t cal_stage_count_n(uint8_t nOsc) {
  if (nOsc <= 3) return (uint8_t)(nOsc * 3u);
  return (uint8_t)((nOsc / 2u) * 7u);
}

static inline uint8_t cal_stage_max_n(uint8_t nOsc) {
  const uint8_t n = cal_stage_count_n(nOsc);
  return (n == 0) ? 0 : (uint8_t)(n - 1u);
}

static inline uint8_t cal_stage_to_osc_n(uint8_t stage, uint8_t nOsc) {
  if (nOsc == 0) return 0;
  if (nOsc <= 3) {
    uint8_t osc = (uint8_t)(stage / 3u);
    return (osc >= nOsc) ? (uint8_t)(nOsc - 1u) : osc;
  }
  uint8_t voice = 0;
  uint8_t remain = stage;
  const uint8_t nVoice = (uint8_t)(nOsc / 2u);
  while (remain >= 7u && (uint8_t)(voice + 1u) < nVoice) {
    remain = (uint8_t)(remain - 7u);
    voice++;
  }
  if (remain < 4u) return (uint8_t)(voice * 2u);
  uint8_t osc = (uint8_t)(voice * 2u + 1u);
  return (osc >= nOsc) ? (uint8_t)(nOsc - 1u) : osc;
}

static inline CalStageKind cal_stage_kind_n(uint8_t stage, uint8_t nOsc) {
  if (nOsc <= 3) {
    switch (stage % 3u) {
      case 1:  return CAL_KIND_PULSE;
      case 2:  return CAL_KIND_440;
      default: return CAL_KIND_SAW;
    }
  }
  uint8_t remain = stage;
  const uint8_t nVoice = (uint8_t)(nOsc / 2u);
  uint8_t voice = 0;
  while (remain >= 7u && (uint8_t)(voice + 1u) < nVoice) {
    remain = (uint8_t)(remain - 7u);
    voice++;
  }
  if (remain < 4u) {
    switch (remain) {
      case 1:  return CAL_KIND_TRI;
      case 2:  return CAL_KIND_PULSE_PW;
      case 3:  return CAL_KIND_440;
      default: return CAL_KIND_SAW;
    }
  }
  switch ((uint8_t)(remain - 4u)) {
    case 1:  return CAL_KIND_PULSE;
    case 2:  return CAL_KIND_440;
    default: return CAL_KIND_SAW;
  }
}

static inline bool cal_stage_is_440_n(uint8_t stage, uint8_t nOsc) {
  return cal_stage_kind_n(stage, nOsc) == CAL_KIND_440;
}
static inline bool cal_stage_is_saw_n(uint8_t stage, uint8_t nOsc) {
  return cal_stage_kind_n(stage, nOsc) == CAL_KIND_SAW;
}
static inline bool cal_stage_is_tri_n(uint8_t stage, uint8_t nOsc) {
  return cal_stage_kind_n(stage, nOsc) == CAL_KIND_TRI;
}
static inline bool cal_stage_is_pw_edit_n(uint8_t stage, uint8_t nOsc) {
  return cal_stage_kind_n(stage, nOsc) == CAL_KIND_PULSE_PW;
}
static inline bool cal_stage_is_square_n(uint8_t stage, uint8_t nOsc) {
  const CalStageKind k = cal_stage_kind_n(stage, nOsc);
  return k == CAL_KIND_PULSE || k == CAL_KIND_PULSE_PW || k == CAL_KIND_440;
}

#endif  // PARAMS_DEF_H



