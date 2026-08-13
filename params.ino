// Central parameter router for shared parameters.
//
// This module maps numeric parameter IDs (used over Serial / between MCUs)
// to concrete synth state changes on this MCU. It is the single place where
// "what does parameter X actually do?" is implemented.
//
// The stable parameter ID definitions live in params_def.h so all MCUs and
// tools can share the same mapping.
//
// High-level flow:
//   1) Some control source (front panel, STM32, MIDI, editor) decides that
//      parameter P should change to value V.
//   2) It sends P and V over the link (Serial 'p', or a 1 ms ADSR/filter block).
//   3) The receiver ends up calling:
//         update_parameters(paramNumber, paramValue);
//   4) update_parameters() looks up paramNumber in paramTable[] and calls
//      the corresponding apply_param_*() function.
//   5) That function updates internal state and performs any required
//      precomputations (fixed-point scales, LFO frequencies, etc.).
//
// How to add or modify a parameter on this MCU:
//   1) Define or reuse a ParamId in params_def.h.
//   2) Implement a new apply_param_*() function below that:
//        - Accepts int16_t (the raw transport value).
//        - Updates the appropriate globals / DSP structures.
//        - Computes any derived values (e.g. Q24 scales, Hz values).
//   3) Add an entry to paramTable[] that maps your ParamId to the new
//      apply_param_*() function.
//   4) Make sure the sending side (other MCU / UI) uses the same ParamId,
//      and sends values in the range/format your apply function expects.
//
// Notes:
//   - The transport is 16-bit (int16_t) for this router.
//   - If an unknown paramNumber is received, update_parameters() simply
//     ignores it.


// ---- Apply functions for each parameter (invoked only via paramTable / update_parameters) ----

// Per-osc Saw/Pulse/Tri enables → 74HC595 / DG411 (see docs/WAVE_MUX.md).
static void apply_wave_enable(uint8_t osc, uint8_t wave, int16_t v) {
  if (osc > 2 || wave > 2) return;
  waveEnable[osc][wave] = (v != 0);
  update_waveSelector();
}

static void apply_param_osc1_saw_enable(int16_t v) { apply_wave_enable(0, 0, v); }
static void apply_param_osc1_pulse_enable(int16_t v) { apply_wave_enable(0, 1, v); }
static void apply_param_osc1_tri_enable(int16_t v) { apply_wave_enable(0, 2, v); }
static void apply_param_osc2_saw_enable(int16_t v) { apply_wave_enable(1, 0, v); }
static void apply_param_osc2_pulse_enable(int16_t v) { apply_wave_enable(1, 1, v); }
static void apply_param_osc2_tri_enable(int16_t v) { apply_wave_enable(1, 2, v); }
static void apply_param_osc3_saw_enable(int16_t v) { apply_wave_enable(2, 0, v); }
static void apply_param_osc3_pulse_enable(int16_t v) { apply_wave_enable(2, 1, v); }
static void apply_param_osc3_tri_enable(int16_t v) { apply_wave_enable(2, 2, v); }

// PARAM_SINE_STATUS: deprecated — no mux role.
static void apply_param_sine_status(int16_t v) {
  (void)v;
}

static void apply_param_resonance_comp(int16_t v) {
  RESONANCEAmpCompensation = (v != 0);
}

static void apply_param_vca_adsr_restart(int16_t v) {
  VCAADSRRestart = (v != 0);
  ADSR_VCA_set_restart();
}

static void apply_param_vcf_adsr_restart(int16_t v) {
  VCFADSRRestart = (v != 0);
  ADSR_VCF_set_restart();
}

// PARAM_ADSR3_TO_OSC_SELECT: which osc(s) receive ADSR3→detune/PWM routing.
static void apply_param_adsr3_to_osc_select(int16_t v) {
  ADSR3ToOscSelect = v;
}

// PARAM_LFO1_WAVEFORM: set LFO1 waveform and refresh rate.
static void apply_param_lfo1_waveform(int16_t v) {
  LFO1Waveform = v;
  LFO1_class.setWaveForm(LFO1Waveform);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

// PARAM_LFO2_WAVEFORM: set LFO2 waveform and refresh rate.
static void apply_param_lfo2_waveform(int16_t v) {
  LFO2Waveform = v;
  LFO2_class.setWaveForm(LFO2Waveform);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

// PARAM_OSC1_INTERVAL (13): global octave_shift (semitones). Kept id for wire compat.
static void apply_param_octave_shift(int16_t v) {
  octave_shift = v;
}

// PARAM_OSC2_INTERVAL: OSC2 transpose interval.
static void apply_param_osc2_interval(int16_t v) {
  OSC2_interval = v;
}

// PARAM_OSC3_INTERVAL: OSC3 transpose interval.
static void apply_param_osc3_interval(int16_t v) {
  OSC3_interval = v;
}

// PARAM_OSC2_DETUNE_VAL: OSC2 fine detune (stored inverted from UI value).
static void apply_param_osc2_detune_val(int16_t v) {
  OSC2DetuneVal = 512 - v;
}

// PARAM_OSC3_DETUNE_VAL: OSC3 fine detune (stored inverted from UI value).
static void apply_param_osc3_detune_val(int16_t v) {
  OSC3DetuneVal = 512 - v;
}

static void apply_param_lfo2_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO2_PITCH_DEPTH_SCALE);
}

// PARAM_LFO2_TO_OSC2: LFO2 → OSC2 pitch depth (Q24).
static void apply_param_lfo2_to_osc2(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC2_q24);
}

// PARAM_LFO2_TO_OSC3: LFO2 → OSC3 pitch depth (Q24).
static void apply_param_lfo2_to_osc3(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC3_q24);
}

// LFO2 coarse pitch depth (0..511; same travel scale as LFO1 pitch).
static void apply_param_lfo2_to_osc_coarse_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint16_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo2_to_osc2_coarse(int16_t v) {
  apply_param_lfo2_to_osc_coarse_depth(v, LFO2toOSC2_coarse_q24);
}

static void apply_param_lfo2_to_osc3_coarse(int16_t v) {
  apply_param_lfo2_to_osc_coarse_depth(v, LFO2toOSC3_coarse_q24);
}

// PARAM_CHARACTER: master scale (0..128) for Character-tab noise jitters (see character_jitter.h).
static void apply_param_character(int16_t v) {
  character = (uint8_t)constrain((int)v, 0, 128);
  character_recompute_scales();
}

// PARAM_OSC_SYNC_MODE: osc sync / phase-align (updates phaseAlignOSC2, retriggers notes).
// oscSync also gates the note-on restart in voices.ino: 0 leaves the oscillators running
// through note-on (free running), 1 stops and restarts OSC1 and OSC2 together with no
// offset, and above 1 adds an OSC2 phase offset on top (2..8 = 45..315 degrees, >8 = v * 2).
static void apply_param_osc_sync_mode(int16_t v) {
  oscSync = v;
  if (oscSync < 2) {
    // Live phase-align no longer widens Y; still reload the plain pulse in case a
    // previous note left a leftover exact-split remainder or an old firmware Y.
    phaseAlignOSC2 = 0;
    pio_defer_request_reset_pulse_all();
  } else {
    if (oscSync > 8) {
      phaseAlignOSC2 = oscSync * 2;
    } else {
      switch (oscSync) {
        case 2:
          phaseAlignOSC2 = 45;
          break;
        case 3:
          phaseAlignOSC2 = 90;
          break;
        case 4:
          phaseAlignOSC2 = 135;
          break;
        case 5:
          phaseAlignOSC2 = 180;
          break;
        case 6:
          phaseAlignOSC2 = 225;
          break;
        case 7:
          phaseAlignOSC2 = 270;
          break;
        case 8:
          phaseAlignOSC2 = 315;
          break;
        default:
          break;
      }
    }
  }
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

// PARAM_PORTAMENTO_TIME: map UI → TIME fixed duration (µs) and SLEW octave period (µs/12st).
static void apply_param_portamento_time(int16_t v) {
  portamento_parameter_value = (uint8_t)v;
  if (portamento_parameter_value == 0) {
    portamento_time_fixed = 0;
    portamento_time_slew = 0;
  } else if (portamento_parameter_value < 200) {
    // Shared smaller minimum (~1 ms at v=1).
    const uint32_t t = (uint32_t)expConverter(portamento_parameter_value + 15, 100) * 500u;
    portamento_time_fixed = t;
    portamento_time_slew = t;
  } else {
    // TIME max 10 s (any interval); SLEW max 20 s per octave.
    portamento_time_fixed =
        (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 10000000);
    portamento_time_slew =
        (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 20000000);
  }
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

static void apply_param_vcf_keytrack(int16_t v) {
  VCFKeytrack = v;
#ifdef USE_FLOAT_CV_OUTS
  if (VCFKeytrack != 0) {
    VCFKeytrackModifier = (float)VCFKeytrack / 8000.0f;
  } else {
    VCFKeytrackModifier = 1.0f;
  }
#else
  if (VCFKeytrack != 0) {
    VCFKeytrackModifier_q15 = ((int32_t)VCFKeytrack * 32768) / 8000;
  } else {
    VCFKeytrackModifier_q15 = 32768;
  }
#endif
}

static void apply_param_velocity_to_vcf(int16_t v) {
  velocityToVCFVal = (int8_t)v;
#ifdef USE_FLOAT_CV_OUTS
  velocityToVCF = velocityToVCFVal * 0.0003935f;
#else
  // ≈ val * 0.0003935 * 32768
  velocityToVCF_q15 = ((int32_t)velocityToVCFVal * 825) >> 6;
#endif
}

static void apply_param_velocity_to_vca(int16_t v) {
  velocityToVCAVal = (int8_t)v;
#ifdef USE_FLOAT_CV_OUTS
  velocityToVCA = velocityToVCAVal * 0.0003935f;
#else
  velocityToVCA_q15 = ((int32_t)velocityToVCAVal * 825) >> 6;
#endif
}

// Panel bases only — PWM written from mod_matrix_apply_cv() in update_CV_outs.
static void apply_param_osc1_level(int16_t v) {
  OSC1LevelVal = v;
  if (OSC1LevelVal < 0) OSC1LevelVal = 0;
  if (OSC1LevelVal > 128) OSC1LevelVal = 128;
  OSC1Level = lin_to_log_128[OSC1LevelVal];
}

static void apply_param_osc2_level(int16_t v) {
  OSC2LevelVal = v;
  if (OSC2LevelVal < 0) OSC2LevelVal = 0;
  if (OSC2LevelVal > 128) OSC2LevelVal = 128;
  OSC2Level = lin_to_log_128[OSC2LevelVal];
}

static void apply_param_osc3_level(int16_t v) {
  OSC3LevelVal = v;
  if (OSC3LevelVal < 0) OSC3LevelVal = 0;
  if (OSC3LevelVal > 128) OSC3LevelVal = 128;
  OSC3Level = lin_to_log_128[OSC3LevelVal];
}

static void apply_param_sub_level(int16_t v) {
  SubLevelVal = v;
  SubLevel = (uint16_t)constrain((int)SubLevelVal * 32, 0, 4095);
}

#define DECL_MOD_SLOT_APPLIERS(N) \
  static void apply_param_mod_slot##N##_source(int16_t v) { mod_matrix_set_source(N, v); } \
  static void apply_param_mod_slot##N##_dest(int16_t v) { mod_matrix_set_dest(N, v); } \
  static void apply_param_mod_slot##N##_depth(int16_t v) { mod_matrix_set_depth(N, v); }

DECL_MOD_SLOT_APPLIERS(0)
DECL_MOD_SLOT_APPLIERS(1)
DECL_MOD_SLOT_APPLIERS(2)
DECL_MOD_SLOT_APPLIERS(3)
DECL_MOD_SLOT_APPLIERS(4)
DECL_MOD_SLOT_APPLIERS(5)
DECL_MOD_SLOT_APPLIERS(6)
DECL_MOD_SLOT_APPLIERS(7)

#undef DECL_MOD_SLOT_APPLIERS

// PARAM_PORTAMENTO_MODE: 0 = TIME (fixed duration any interval),
// else SLEW (constant semitone rate; one octave = portamento_time_slew).
static void apply_param_portamento_mode(int16_t v) {
  portamento_mode = (v == 0) ? PORTA_MODE_TIME : PORTA_MODE_SLEW;
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

// PARAM_CALIBRATION_VALUE: reserved ID (no behavior).
static void apply_param_calibration_value(int16_t /*v*/) {
  // Placeholder: original code did nothing but kept the ID reserved.
}

// PARAM_VOICE_MODE: mono/poly/stack → setVoiceMode().
static void apply_param_voice_mode(int16_t v) {
  voiceMode = v;
  setVoiceMode();
}

// PARAM_VOICE_ALLOC_MODE: para steal policy / mono note priority (VoiceAllocMode).
static void apply_param_voice_alloc_mode(int16_t v) {
  if (v < 0) return;
  voiceAlloc.setMode((uint8_t)v);
}

// PARAM_UNISON_DETUNE: unison detune amount.
static void apply_param_unison_detune(int16_t v) {
  unisonDetune = v;
}

// PARAM_ANALOG_DRIFT_AMOUNT: drift modulation depth.
static void apply_param_analog_drift_amount(int16_t v) {
  analogDrift = v;
  // Full-scale Q15 → same travel as legacy (cc_level * unit * analogDrift).
  drift_pitch_scale_q24 =
    (int32_t)((int32_t)analogDrift * DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE);
#ifndef USE_FLOAT_CV_OUTS
  vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}

// PARAM_ANALOG_DRIFT_SPEED: recompute all drift LFO rates.
static void apply_param_analog_drift_speed(int16_t v) {
  analogDriftSpeed = v;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_SPEED_OFFSET[i] =
      (float)(1.00f - (float)((float)analogDriftSpread * 0.005f) +
              (float)((float)analogDriftSpread * 0.00125f * (float)i)) *
      (float)expConverterFloat((float)analogDriftSpeed, 5000);
    LFO_DRIFT_CLASS[i].setMode0Freq(LFO_DRIFT_SPEED_OFFSET[i], micros());
  }
}

// PARAM_ANALOG_DRIFT_SPREAD: recompute per-osc drift speed offsets.
static void apply_param_analog_drift_spread(int16_t v) {
  analogDriftSpread = v;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_SPEED_OFFSET[i] =
      (float)(1.00f - (float)((float)analogDriftSpread * 0.005f) +
              (float)((float)analogDriftSpread * 0.00125f * (float)i)) *
      (float)expConverterFloat((float)analogDriftSpeed, 5000);
    LFO_DRIFT_CLASS[i].setMode0Freq(LFO_DRIFT_SPEED_OFFSET[i], micros());
  }
}

// PARAM_SYNC_MODE: PIO sync topology → setSyncMode() (deferred to core 1).
static void apply_param_sync_mode(int16_t v) {
  if (manualCalibrationFlag) {
    // The walk needs every oscillator to own its own RESET pin, so a preset load
    // mid-walk only books the topology for when manual cal exits.
    manualCalSavedSyncMode = (uint8_t)v;
    return;
  }
  syncMode = v;
  pio_defer_request_sync_mode();
}

// PARAM_SOFT_SYNC: 0 = hard sync (sideset, weight 4); 1..3 = soft sync with that many
// trailing polled chunks (weights 5/6/7, receptive ~40%/67%/86%). Changing among 1..3
// reloads the poll program image on pio0.
static void apply_param_soft_sync(int16_t v) {
  if (v < 0) v = 0;
  if (v > 3) v = 3;
  if (manualCalibrationFlag) {
    manualCalSavedSoftSyncChunks = (uint8_t)v;
    return;
  }
  softSyncChunks = (uint8_t)v;
  pio_defer_request_sync_mode();
}

// PARAM_SUBOSC_DIVIDE: sub-oscillator divide ratio off / 2 / 4.
static void apply_param_subosc_divide(int16_t v) {
  uint8_t divide = 0;
  if (v >= 4) {
    divide = 4;
  } else if (v >= 2) {
    divide = 2;
  }
  pio_defer_request_subosc(divide);
}

// PARAM_SUB1/2_DIVIDE / _MASTER / _PHASE / _WIDTH and the combiner operator. Ranges, clamping
// and the core-0 handoff all live in subosc.h; without ENABLE_SUBOSC_ENGINE2 sub 1's divide
// drives the single legacy sub and the rest are ignored.
static void apply_param_sub1_divide(int16_t v) { subosc_param_divide(0, v); }
static void apply_param_sub2_divide(int16_t v) { subosc_param_divide(1, v); }
static void apply_param_sub1_master(int16_t v) { subosc_param_master(0, v); }
static void apply_param_sub2_master(int16_t v) { subosc_param_master(1, v); }
static void apply_param_sub1_phase(int16_t v) { subosc_param_phase(0, v); }
static void apply_param_sub2_phase(int16_t v) { subosc_param_phase(1, v); }
static void apply_param_sub1_width(int16_t v) { subosc_param_width(0, v); }
static void apply_param_sub2_width(int16_t v) { subosc_param_width(1, v); }
static void apply_param_sub_logic_op(int16_t v) { subosc_param_logic_op(v); }

// PARAM_LFO1_TO_DCO: LFO1 → DCO detune depth (full-scale Q24 for Q15 wave).
static void apply_param_lfo1_to_dco(int16_t v) {
  LFO1toDCOVal = v;
  float lfo1_amt = (float)expConverterFloat(LFO1toDCOVal, 500) / 275000.0f;
  LFO1toDCO_q24 = lfo_pitch_depth_q24(lfo1_amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc1(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC1_q24);
}

static void apply_param_lfo1_to_osc2(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC2_q24);
}

static void apply_param_lfo1_to_osc3(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC3_q24);
}

// PARAM_LFO1_SPEED: LFO1 rate in Hz (via expConverterFloat).
static void apply_param_lfo1_speed(int16_t v) {
  LFO1SpeedVal = v;
  LFO1Speed = expConverterFloat(LFO1SpeedVal, 5000);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

// PARAM_LFO2_SPEED: LFO2 rate in Hz (via expConverterFloat).
static void apply_param_lfo2_speed(int16_t v) {
  LFO2SpeedVal = v;
  LFO2Speed = expConverterFloat(LFO2SpeedVal, 5000);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

// PARAM_VCA_LEVEL: panel sends 0..128; scale to the 12-bit CV domain.
static void apply_param_vca_level(int16_t v) {
  VCALevel = (uint16_t)constrain((int)v * 32, 0, 4095);
}

// PARAM_DIST_DRIVE / PARAM_DIST_MIX: post-LP distortion CVs (0..4095).
static void apply_param_dist_drive(int16_t v) {
  DIST_DRIVE = (uint16_t)constrain((int)v, 0, 4095);
}

static void apply_param_dist_mix(int16_t v) {
  DIST_MIX = (uint16_t)constrain((int)v, 0, 4095);
}

// PARAM_FILTER_MODE: AS3320 multimode index (0..N). Pin drive is solo-B / not ENABLE_VOICE_AUX.
static void apply_param_filter_mode(int16_t v) {
  FILTER_MODE = (uint8_t)constrain((int)v, 0, 255);
}

static void apply_param_lfo1_to_vca(int16_t v) {
  LFO1toVCA = (uint16_t)constrain((int)v, 0, 4095);
  cv_bake_lfo1_to_vca_scale();
}

// PARAM_LFO2_TO_PW: LFO2 → pulse-width depth (PWM counts at full-scale Q15).
static void apply_param_lfo2_to_pw(int16_t v) {
  LFO2toPW = (int16_t)v;
}

// PARAM_ADSR3_TO_PWM: ADSR → PWM depth (centered around 512).
// Precompute full-scale PWM counts so hot path is (level_q15 * scale) >> 15.
// Legacy: (level_u12 * depth) >> 11 with level 0..ADSR_1_CC.
static void apply_param_adsr1_to_pwm(int16_t v) {
  ADSR1toPWM = (int16_t)v - 512;
  ADSR1toPWM_scale = (int32_t)(((int64_t)ADSR1toPWM * (int64_t)ADSR_1_CC) >> 11);
}

// PARAM_ADSR3_TO_DETUNE1: ADSR → pitch depth (exp on knob, linear env hot).
// Full CW × full env → ADSR_PITCH_MAX_OCTAVES (tune in LFO.h).
static void apply_param_adsr1_to_detune1(int16_t v) {
  ADSR1toDETUNE1 = (int16_t)v;
  if (ADSR1toDETUNE1 == 0) {
    ADSR1toDETUNE1_scale_q24 = 0;
  } else {
    const uint16_t mag_u =
      (ADSR1toDETUNE1 < 0) ? (uint16_t)(-ADSR1toDETUNE1) : (uint16_t)ADSR1toDETUNE1;
    const float mag = expConverterFloat(mag_u, 500);
    const float mag_full = expConverterFloat(ADSR_PITCH_DEPTH_PANEL_FULL, 500);
    float norm = (mag_full > 0.0f) ? (mag / mag_full) : 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const float signed_oct = (ADSR1toDETUNE1 < 0) ? -norm : norm;
    ADSR1toDETUNE1_scale_q24 =
      (int32_t)(signed_oct * ADSR_PITCH_MAX_OCTAVES * (float)(1 << 24) +
                ((signed_oct >= 0.0f) ? 0.5f : -0.5f));
  }
}

// PARAM_ADSR3_PITCH_MODE: EnvDCO → pitch tap. 0 unipolar (default), 1 centered.
static void apply_param_adsr3_pitch_mode(int16_t v) {
  env_dco_pitch_centered = (v != 0) ? 1 : 0;
}

// PARAM_ADSR1_ATTACK_CURVE / DECAY: EnvVCA curve shape.
static void apply_param_adsr1_attack_curve(int16_t v) {
  ADSR1AttackCurveVal = (uint8_t)v;
  ADSR_VCA_change_attack_curve(ADSR1AttackCurveVal);
}

static void apply_param_adsr1_decay_curve(int16_t v) {
  ADSR1DecayCurveVal = (uint8_t)v;
  ADSR_VCA_change_decay_curve(ADSR1DecayCurveVal);
}

// PARAM_ADSR2_ATTACK_CURVE / DECAY: EnvVCF curve shape.
static void apply_param_adsr2_attack_curve(int16_t v) {
  ADSR2AttackCurveVal = (uint8_t)v;
  ADSR_VCF_change_attack_curve(ADSR2AttackCurveVal);
}

static void apply_param_adsr2_decay_curve(int16_t v) {
  ADSR2DecayCurveVal = (uint8_t)v;
  ADSR_VCF_change_decay_curve(ADSR2DecayCurveVal);
}

// PARAM_PW_VALUE: pulse width 0..4095; voice engine uses PW[0] at /4 scale.
static void apply_param_pw_value(int16_t v) {
  uint16_t pwRaw = (uint16_t)constrain((int)v, 0, 4095);
  PW[0] = pwRaw / 4;
}

// PARAM_ADSR1_TO_VCA: EnvVCA → VCA amount (was the 'e' block).
static void apply_param_adsr1_to_vca(int16_t v) {
  ADSR1toVCA = v;
}

// PARAM_PWM_POTS_CONTROL_MANUAL: manual PWM pot control flag.
static void apply_param_pwm_pots_manual(int16_t v) {
  PWMPotsControlManual = (v != 0);
}

static void apply_param_adsr3_enabled(int16_t v) {
  ADSR3Enabled = (v != 0);
}

// PARAM_FUNCTION_KEY: reserved / handled elsewhere.
static void apply_param_function_key(int16_t /*v*/) {
}

// Gap is generated by DCO (autotune) and TX'd via serialSendParam32 → Screen.
static void apply_param_gap_from_dco(int16_t /*v*/) {
}

// PARAM_CALIBRATION_FLAG: start/stop auto-cal (loop1 runs DCO_calibration when
// set). The value selects the stage and how carefully it measures:
// 1 = amp-comp only, 2 = PW only, 3 = full at NORMAL precision (build from
// scratch, fast); 5/6/7 = the same three stages at FINE precision, where the
// amp stage re-measures the stored table instead of building a new one;
// 9/10/11 = the same stages at FAST precision (quickest from-scratch build,
// for a testing table).
static void apply_param_calibration_flag(int16_t v) {
  if (v == 0) {
    // Runs on core 0 while DCO_calibration() may be blocking core 1: request
    // a cancel; the calibration loops poll this and unwind cleanly.
    calibrationCancelRequested = true;
    calibrationFlag = false;
    return;
  }

  if (v >= 9 && v <= 11) {
    calibrationPrecision = CAL_PRECISION_FAST;
    v -= 8;
  } else if (v >= 5 && v <= 7) {
    calibrationPrecision = CAL_PRECISION_FINE;
    v -= 4;
  } else {
    calibrationPrecision = CAL_PRECISION_NORMAL;
  }

  calibrationScope = (v == CAL_SCOPE_AMP || v == CAL_SCOPE_PW)
                       ? (uint8_t)v
                       : (uint8_t)CAL_SCOPE_FULL;
  calibrationFlag = true;
}

// PARAM_MANUAL_CALIBRATION_FLAG: enter/exit manual cal; rising edge TX offsets to Input/Mainboard.
static void apply_param_manual_calibration_flag(int16_t v) {
  // When manual calibration is active, both flags follow this param.
  // Rising edge (0 -> non-zero): broadcast current offsets upstream (Input hub or Mainboard).
  if (v != 0 && !manualCalibrationFlag) {
    // The solo stops every other SM, which a synced pair cannot survive, so the
    // walk runs a neutral topology (see manualCalSavedSyncMode in autotune.h).
    manualCalSavedSyncMode = syncMode;
    manualCalSavedSoftSyncChunks = softSyncChunks;
    if (syncMode != 0) {
      Serial.println((String)"[MANUAL_CAL] sync neutralised: syncMode " + syncMode +
                     " -> 0, softSyncChunks " + softSyncChunks + " -> 0 (restored on exit)");
    }
    syncMode = 0;
    softSyncChunks = 0;
    calSyncNeutralRequested = true;

    for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
      uint8_t idx    = osc;
      uint8_t offset = (uint8_t)manualCalibrationOffset[osc];
      uint16_t packed = ((uint16_t)idx << 8) | offset;
      // Send as 32-bit frame; receivers use lower 16 bits [index:8|offset:8].      
      serialSendParam32(PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO, (uint32_t)packed);
    }
  }

  // Falling edge: manual cal forced the SQR levels and wave mux — restore panel state.
  // It also left every un-soloed oscillator's SM stopped and the PW channels at 0,
  // which only core 1 may undo (PIO), so that part goes through the deferred queue.
  if (v == 0 && manualCalibrationFlag) {
    // Hand the sync topology back before the restore below: its start_voice_sms()
    // is what rebuilds the SMs from syncMode / softSyncChunks.
    syncMode = manualCalSavedSyncMode;
    softSyncChunks = manualCalSavedSoftSyncChunks;
    calSyncNeutralRequested = false;
    apply_param_osc1_level(OSC1LevelVal);
    apply_param_osc2_level(OSC2LevelVal);
    apply_param_osc3_level(OSC3LevelVal);
    apply_param_sub_level(SubLevelVal);
    update_waveSelector();
    pio_defer_request_cal_restore();
  }

  // Every manual-cal entry starts at the trimpot step; the UI switches to the
  // 440 Hz step explicitly via PARAM_MANUAL_CALIBRATION_STEP.
  if (v != 0 && !manualCalibrationFlag) {
    manualCalibrationStep = 0;
  }

  manualCalibrationFlag = v;
  calibrationFlag       = v;
}

// PARAM_MANUAL_CALIBRATION_STEP: 0 = trimpot stage at the low starting note,
// 1 = 440 Hz amp-set stage (adjust PARAM_AMP_COMP_440 until duty = 50%).
static void apply_param_manual_calibration_step(int16_t v) {
  manualCalibrationStep = (v != 0) ? 1 : 0;
}

// PARAM_AMP_COMP_440: absolute range-PWM at 440 Hz for the oscillator selected
// by manualCalibrationStage (osc = stage / 3). Persisted by STORE.
static void apply_param_amp_comp_440(int16_t v) {
  uint8_t osc = cal_manual_osc();
  if (v < 0) v = 0;
  uint16_t val = (uint16_t)v;
  if (val > DIV_COUNTER) val = DIV_COUNTER;
  ampComp440[osc] = val;
}

// PARAM_AMP_COMP_DUTY_OFFSET: duty target trim (hundredths of a percent) for
// the oscillator selected by manualCalibrationStage. Nulls the difference
// between the sense pin's 50% and the scope's 50%; both amp-comp methods and
// the manual duty readout aim at 50% + this value. Persisted by
// PARAM_MANUAL_CALIBRATION_STORE.
static void apply_param_amp_comp_duty_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  if (v < -500) v = -500;
  if (v >  500) v =  500;
  ampCompDutyOffset[osc] = v;
}

// PARAM_CAL_PW_CENTER: live PW_CENTER for the calibrated oscillator's PW channel.
static void apply_param_cal_pw_center(int16_t v) {
  const uint8_t ch = cal_pw_channel(cal_manual_osc());
  if (v < 0) v = 0;
  if (v > (int16_t)CAL_PW_CENTER_MAX) v = (int16_t)CAL_PW_CENTER_MAX;
  PW_CENTER[ch] = (uint16_t)v;
}

// PARAM_MANUAL_CALIBRATION_STAGE: packed walk (see cal_stage_*_n). Do not
// clamp to NUM_OSCILLATORS-1 — that collapsed DCO3 stages 3–5 onto osc 2.
static void apply_param_manual_calibration_stage(int16_t v) {
  int16_t stage = v;
  if (stage < 0) stage = 0;
  const int16_t maxStage = (int16_t)cal_stage_max();
  if (stage > maxStage) stage = maxStage;
  manualCalibrationStage = (uint8_t)stage;
  manualCalibrationStep = cal_stage_is_440((uint8_t)stage) ? 1 : 0;
  if (cal_stage_is_440((uint8_t)stage)) {
    serialSendParam16(PARAM_AMP_COMP_440, (int16_t)ampComp440[cal_manual_osc()], true);
  } else if (cal_stage_is_pw_edit((uint8_t)stage)) {
    serialSendParam16(PARAM_CAL_PW_CENTER,
                      (int16_t)PW_CENTER[cal_pw_channel(cal_manual_osc())], true);
  }
}

// PARAM_MANUAL_CALIBRATION_OFFSET: per-osc manual amp offset for current osc.
static void apply_param_manual_calibration_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  manualCalibrationOffset[osc] = (int8_t)v;
}

// Explicit "store manual calibration" command. This is called when the user
// confirms manual calibration, and is the only place where we persist
// manualCalibrationOffset[], ampComp440[] and ampCompDutyOffset[] to the
// filesystem.
static void apply_param_manual_calibration_store(int16_t /*v*/) {
  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    update_FS_ManualCalibrationOffset(osc, manualCalibrationOffset[osc]);
    update_FS_AmpComp440(osc, ampComp440[osc]);
    update_FS_AmpCompDutyOffset(osc, ampCompDutyOffset[osc]);
  }
  for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ++ch) {
    update_FS_PWCenter(ch, PW_CENTER[ch]);
  }
}

// Preset store commands (see preset_store.h / preset_store.ino). These are
// command triggers, not patch state: preset_param_is_persistable() excludes
// them, so they are never captured into the shadow or stored in a record.
static void apply_param_preset_save(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_save((uint8_t)v);
}

static void apply_param_preset_load(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_load((uint8_t)v);
}

static void apply_param_preset_dump(int16_t v) {
  preset_store_dump(v);
}

static void apply_param_cal_dump(int16_t v) {
  preset_store_cal_dump(v);
}

// PARAM_DEBUG_COMMAND: bench / debug opcodes for tools/dco_control (Diagnostics + Calibration
// + Character). See params_def.h near PARAM_DEBUG_COMMAND for the opcode list.
//
// Values 200..50000 (uint16 on the wire) set pioPulseLength and reload running SMs
// via pio_defer_request_reset_pulse_all(). Small opcodes 1..30 stay below that range.
//
// Packed Character-tab jitter setters (unsigned 16-bit, hi|lo with lo in 0..128):
//   0xC8xx ampCompJitter, 0xCAxx pitchJitter, 0xCBxx pulsewidthJitter.
// These sit above 50000 so they do not collide with the pioPulseLength window.
//
// The period probe parks an oscillator at a fixed clk_div, so it only holds while no
// note is playing — voice_task_main() pushes a fresh divider every frame for a held note.
//
// 10 / 11 / 12 drive the profiler in bench.h and only do anything in a RUNNING_AVERAGE
// build. The dump is asynchronous: it asks both cores for a snapshot and core 0 prints
// once both have answered, so this handler never blocks the audio core.
// 13 dumps heap + per-core stack (mem_diag; needs ENABLE_MEM_DIAG; runtime polls on).
// 14 / 15 disable / enable mem_diag loop polls (A/B vs profiler without rebuild).
// 43 / 44 MCP4728 probe / reattach when ENABLE_MCP4728; else prints compiled-out.
//  4 dumps the sub engine: each sub's master, segment words, DMA channels, FIFO level and why
// the logic combiner is or is not running. Says so and stops without ENABLE_SUBOSC_ENGINE2.
static void apply_param_debug_command(int16_t v) {
  // Wire may pack unsigned 16-bit (param16u); reinterpret before small-opcode switch.
  uint32_t n = (uint16_t)v;
  uint8_t hi = (uint8_t)(n >> 8);
  uint8_t lo = (uint8_t)n;
  if ((hi == 0xC8u || hi == 0xCAu || hi == 0xCBu) && lo <= 128u) {
    switch (hi) {
      case 0xC8: ampCompJitter = lo; break;
      case 0xCA: pitchJitter = lo; break;
      case 0xCB: pulsewidthJitter = lo; break;
      default: break;
    }
    character_recompute_scales();
    return;
  }
  if (n >= 200u && n <= 50000u) {
    pioPulseLength = n;
    // Reload Y into running SMs on core 1 (same path as clearing phase-align).
    pio_defer_request_reset_pulse_all();
    return;
  }

  switch (v) {
    case 1:
      pio_topology_report();
      break;
    case 2:
      pio_period_probe(0, 2000);
      break;
    case 3:
      pio_period_probe(0, 20000);
      break;
    case 4:
      subosc2_report();
      break;
#ifdef ENABLE_MEM_DIAG
    case 13:
      if (!mem_diag_runtime_enabled) {
#ifdef RUNNING_AVERAGE
        bench_out_reset();
        bench_out_printf("mem_diag polls=off\n");
        bench_out_active = true;
#else
        Serial.println("mem_diag polls=off");
#endif
      } else {
        mem_diag_request();
      }
      break;
    case 14:
      mem_diag_runtime_enabled = false;
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=off\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=off");
#endif
      break;
    case 15:
      mem_diag_runtime_enabled = true;
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=on\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=on");
#endif
      break;
#else
    case 13:
    case 14:
    case 15:
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=compiled out\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=compiled out");
#endif
      break;
#endif
#ifdef RUNNING_AVERAGE
    case 10:
      bench_dump_request = true;
      break;
    case 11:
      bench_reset_all();
      break;
    case 12:
      bench_periodic = !bench_periodic;
      bench_out_reset();
      bench_out_printf("bench periodic %s\n", bench_periodic ? "on" : "off");
      bench_out_active = true;
      break;
#endif
#ifdef ENABLE_MCP4728
    case 43:
      mcp_dac_probe();
      break;
    case 44:
      mcp_dac_reattach();
      break;
#else
    case 43:
    case 44:
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mcp4728 compiled out\n");
      bench_out_active = true;
#else
      Serial.println("mcp4728 compiled out");
#endif
      break;
#endif
    // Amp-comp live method (USE_FLOAT_AMP_COMP). Ack via paced Board pane when
    // RUNNING_AVERAGE (same path as profiler / amp benches); else Serial.
    // Reset profiler so amp-comp means are not dominated by the previous method.
    case 20:
      amp_comp_set_method(AMP_COMP_FLOAT_QUAD);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
    case 21:
      amp_comp_set_method(AMP_COMP_LUT);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
    case 22:
      amp_comp_set_method(AMP_COMP_FIXED);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
#if defined(RUNNING_AVERAGE) && defined(AMP_COMP_BENCHMARK)
    case 24:
      amp_comp_bench_speed_pending = true;
      break;
    case 25:
      amp_comp_bench_accuracy_pending = true;
      break;
#endif
#ifdef RUNNING_AVERAGE
    case 28:
      pitch_interp_bench_speed_pending = true;
      break;
    case 29:
      pitch_interp_bench_accuracy_pending = true;
      break;
    case 32:
      clkdiv_hp_bench_speed_pending = true;
      break;
    case 33:
      clkdiv_hp_bench_accuracy_pending = true;
      break;
#endif
    // Force-write fake amp-comp + PW tables (dev placeholder; dco_control Calibration).
    case 30:
      seed_fake_calibration_tables(true);
      break;
    // Read-only [CAL_VERIFY] pass over the stored tables. Every probe blocks on
    // a duty measurement, so core 1 runs it from loop1(); this only asks.
    case 36:
      calibrationVerifyRequested = true;
      break;
    // PW CV probe: needs manual cal running, since the soloed oscillator's
    // pulse on the cal-sense pin is what tells us the CV arrived.
    case 46:
      if (!manualCalibrationFlag) {
        Serial.println("[PW_PROBE] start manual calibration first (param 151), "
                       "on a pulse (PW) substage");
      } else {
        pwCvProbeRequested = true;
      }
      break;
    // Amp-comp calibration method A/B (runtime-only, classic is boot default).
    case 34:
    case 35:
      autotuneAmpMethod = (v == 35) ? AMP_METHOD_FREQ_TRACE : AMP_METHOD_CLASSIC;
      Serial.printf("autotuneAmpMethod=%s (%s)\n",
                    autotune_amp_method_name(autotuneAmpMethod),
                    (autotuneAmpMethod == AMP_METHOD_FREQ_TRACE)
                      ? "fixed-PWM freq bisection"
                      : "per-note PWM search");
      break;
    // Frequency-search convergence A/B: how find_freq_for_duty50() picks the
    // next probe once the answer is bracketed. Compare on the probes= and
    // elapsed= figures in the [CAL_REPORT] footer.
    case 37:
    case 38:
    case 39:
      autotuneSearchMode = (v == 37) ? SEARCH_BISECT
                         : (v == 38) ? SEARCH_INTERP
                                     : SEARCH_GATED;
      Serial.printf("autotuneSearchMode=%s (%s)\n",
                    autotune_search_mode_name(autotuneSearchMode),
                    (autotuneSearchMode == SEARCH_BISECT)
                      ? "geometric midpoint, sign only"
                      : (autotuneSearchMode == SEARCH_INTERP)
                          ? "Illinois secant in log-frequency"
                          : "secant above the noise, midpoint below");
      break;
    // Amp-comp-0 endpoint A/B: hunt for the lowest reachable frequency live,
    // or skip the hunt and store the least-squares fit through the bottom
    // rungs (runtime-only, MEASURE is the boot default).
    case 40:
    case 41:
      autotuneAmp0Mode = (v == 41) ? AMP0_MODE_CALC : AMP0_MODE_MEASURE;
      Serial.printf("autotuneAmp0Mode=%s (%s)\n",
                    autotune_amp0_mode_name(autotuneAmp0Mode),
                    (autotuneAmp0Mode == AMP0_MODE_CALC)
                      ? "store the bottom-rung fit, no live hunt"
                      : "scan + bounded search at amp comp 0");
      break;
    // Note-on sync retrigger A/B (oscSync >= 1): EXACT_Y vs SYNC_JMP.
    case 26:
      note_retrig_set_mode(NOTE_RETRIG_EXACT_Y);
#ifdef RUNNING_AVERAGE
      note_retrig_mode_ack_pending = true;
#else
      Serial.print("note_retrig=");
      Serial.println(note_retrig_mode_name(note_retrig_mode));
#endif
      break;
    case 27:
      note_retrig_set_mode(NOTE_RETRIG_SYNC_JMP);
#ifdef RUNNING_AVERAGE
      note_retrig_mode_ack_pending = true;
#else
      Serial.print("note_retrig=");
      Serial.println(note_retrig_mode_name(note_retrig_mode));
#endif
      break;
    default:
      break;
  }
}

// ---- Parameter table ------------------------------------------------

static const ParamDescriptorT<int16_t> paramTable[] = {
  { PARAM_OSC1_SAW_ENABLE,           apply_param_osc1_saw_enable },
  { PARAM_OSC1_PULSE_ENABLE,         apply_param_osc1_pulse_enable },
  { PARAM_OSC1_TRI_ENABLE,           apply_param_osc1_tri_enable },
  { PARAM_OSC2_SAW_ENABLE,           apply_param_osc2_saw_enable },
  { PARAM_OSC2_PULSE_ENABLE,         apply_param_osc2_pulse_enable },
  { PARAM_OSC2_TRI_ENABLE,           apply_param_osc2_tri_enable },
  { PARAM_OSC3_SAW_ENABLE,           apply_param_osc3_saw_enable },
  { PARAM_OSC3_PULSE_ENABLE,         apply_param_osc3_pulse_enable },
  { PARAM_OSC3_TRI_ENABLE,           apply_param_osc3_tri_enable },
  { PARAM_SINE_STATUS,               apply_param_sine_status },
  { PARAM_RESONANCE_COMPENSATION,    apply_param_resonance_comp },
  { PARAM_VCA_ADSR_RESTART,          apply_param_vca_adsr_restart },
  { PARAM_VCF_ADSR_RESTART,          apply_param_vcf_adsr_restart },
  { PARAM_ADSR3_TO_OSC_SELECT,       apply_param_adsr3_to_osc_select },
  { PARAM_LFO1_WAVEFORM,             apply_param_lfo1_waveform },
  { PARAM_LFO2_WAVEFORM,             apply_param_lfo2_waveform },
  { PARAM_OSC1_INTERVAL,             apply_param_octave_shift },
  { PARAM_OSC2_INTERVAL,             apply_param_osc2_interval },
  { PARAM_OSC3_INTERVAL,             apply_param_osc3_interval },
  { PARAM_OSC2_DETUNE_VAL,           apply_param_osc2_detune_val },
  { PARAM_OSC3_DETUNE_VAL,           apply_param_osc3_detune_val },
  { PARAM_LFO2_TO_OSC2,              apply_param_lfo2_to_osc2 },
  { PARAM_LFO2_TO_OSC3,              apply_param_lfo2_to_osc3 },
  { PARAM_LFO2_TO_OSC2_COARSE,       apply_param_lfo2_to_osc2_coarse },
  { PARAM_LFO2_TO_OSC3_COARSE,       apply_param_lfo2_to_osc3_coarse },
  { PARAM_CHARACTER,                 apply_param_character },
  { PARAM_OSC_SYNC_MODE,             apply_param_osc_sync_mode },
  { PARAM_PORTAMENTO_TIME,           apply_param_portamento_time },
  { PARAM_PORTAMENTO_MODE,           apply_param_portamento_mode },
  { PARAM_VCF_KEYTRACK,              apply_param_vcf_keytrack },
  { PARAM_VELOCITY_TO_VCF,           apply_param_velocity_to_vcf },
  { PARAM_VELOCITY_TO_VCA,           apply_param_velocity_to_vca },
  { PARAM_OSC1_LEVEL,                apply_param_osc1_level },
  { PARAM_OSC2_LEVEL,                apply_param_osc2_level },
  { PARAM_OSC3_LEVEL,                apply_param_osc3_level },
  { PARAM_SUB_LEVEL,                 apply_param_sub_level },
  { PARAM_CALIBRATION_VALUE,         apply_param_calibration_value },
  { PARAM_VOICE_MODE,                apply_param_voice_mode },
  { PARAM_VOICE_ALLOC_MODE,          apply_param_voice_alloc_mode },
  { PARAM_UNISON_DETUNE,             apply_param_unison_detune },
  { PARAM_ANALOG_DRIFT_AMOUNT,       apply_param_analog_drift_amount },
  { PARAM_ANALOG_DRIFT_SPEED,        apply_param_analog_drift_speed },
  { PARAM_ANALOG_DRIFT_SPREAD,       apply_param_analog_drift_spread },
  { PARAM_SYNC_MODE,                 apply_param_sync_mode },
  { PARAM_SOFT_SYNC,                 apply_param_soft_sync },
  { PARAM_SUBOSC_DIVIDE,             apply_param_subosc_divide },
  { PARAM_SUB1_DIVIDE,               apply_param_sub1_divide },
  { PARAM_SUB2_DIVIDE,               apply_param_sub2_divide },
  { PARAM_SUB1_MASTER,               apply_param_sub1_master },
  { PARAM_SUB2_MASTER,               apply_param_sub2_master },
  { PARAM_SUB1_PHASE,                apply_param_sub1_phase },
  { PARAM_SUB2_PHASE,                apply_param_sub2_phase },
  { PARAM_SUB1_WIDTH,                apply_param_sub1_width },
  { PARAM_SUB2_WIDTH,                apply_param_sub2_width },
  { PARAM_SUB_LOGIC_OP,              apply_param_sub_logic_op },
  { PARAM_LFO1_TO_DCO,               apply_param_lfo1_to_dco },
  { PARAM_LFO1_TO_OSC1,              apply_param_lfo1_to_osc1 },
  { PARAM_LFO1_TO_OSC2,              apply_param_lfo1_to_osc2 },
  { PARAM_LFO1_TO_OSC3,              apply_param_lfo1_to_osc3 },
  { PARAM_LFO1_SPEED,                apply_param_lfo1_speed },
  { PARAM_LFO2_SPEED,                apply_param_lfo2_speed },
  { PARAM_VCA_LEVEL,                 apply_param_vca_level },
  { PARAM_LFO1_TO_VCA,               apply_param_lfo1_to_vca },
  { PARAM_LFO2_TO_PW,                apply_param_lfo2_to_pw },
  { PARAM_ADSR3_TO_PWM,              apply_param_adsr1_to_pwm },
  { PARAM_ADSR3_TO_DETUNE1,          apply_param_adsr1_to_detune1 },
  { PARAM_ADSR3_PITCH_MODE,          apply_param_adsr3_pitch_mode },
  { PARAM_ADSR1_ATTACK_CURVE,        apply_param_adsr1_attack_curve },
  { PARAM_ADSR1_DECAY_CURVE,         apply_param_adsr1_decay_curve },
  { PARAM_ADSR2_ATTACK_CURVE,        apply_param_adsr2_attack_curve },
  { PARAM_ADSR2_DECAY_CURVE,         apply_param_adsr2_decay_curve },
  { PARAM_DIST_DRIVE,                apply_param_dist_drive },
  { PARAM_DIST_MIX,                  apply_param_dist_mix },
  { PARAM_FILTER_MODE,               apply_param_filter_mode },
  { PARAM_MOD_SLOT0_SOURCE,          apply_param_mod_slot0_source },
  { PARAM_MOD_SLOT0_DEST,            apply_param_mod_slot0_dest },
  { PARAM_MOD_SLOT0_DEPTH,           apply_param_mod_slot0_depth },
  { PARAM_MOD_SLOT1_SOURCE,          apply_param_mod_slot1_source },
  { PARAM_MOD_SLOT1_DEST,            apply_param_mod_slot1_dest },
  { PARAM_MOD_SLOT1_DEPTH,           apply_param_mod_slot1_depth },
  { PARAM_MOD_SLOT2_SOURCE,          apply_param_mod_slot2_source },
  { PARAM_MOD_SLOT2_DEST,            apply_param_mod_slot2_dest },
  { PARAM_MOD_SLOT2_DEPTH,           apply_param_mod_slot2_depth },
  { PARAM_MOD_SLOT3_SOURCE,          apply_param_mod_slot3_source },
  { PARAM_MOD_SLOT3_DEST,            apply_param_mod_slot3_dest },
  { PARAM_MOD_SLOT3_DEPTH,           apply_param_mod_slot3_depth },
  { PARAM_MOD_SLOT4_SOURCE,          apply_param_mod_slot4_source },
  { PARAM_MOD_SLOT4_DEST,            apply_param_mod_slot4_dest },
  { PARAM_MOD_SLOT4_DEPTH,           apply_param_mod_slot4_depth },
  { PARAM_MOD_SLOT5_SOURCE,          apply_param_mod_slot5_source },
  { PARAM_MOD_SLOT5_DEST,            apply_param_mod_slot5_dest },
  { PARAM_MOD_SLOT5_DEPTH,           apply_param_mod_slot5_depth },
  { PARAM_MOD_SLOT6_SOURCE,          apply_param_mod_slot6_source },
  { PARAM_MOD_SLOT6_DEST,            apply_param_mod_slot6_dest },
  { PARAM_MOD_SLOT6_DEPTH,           apply_param_mod_slot6_depth },
  { PARAM_MOD_SLOT7_SOURCE,          apply_param_mod_slot7_source },
  { PARAM_MOD_SLOT7_DEST,            apply_param_mod_slot7_dest },
  { PARAM_MOD_SLOT7_DEPTH,           apply_param_mod_slot7_depth },
  { PARAM_PW_VALUE,                  apply_param_pw_value },
  { PARAM_ADSR1_TO_VCA,              apply_param_adsr1_to_vca },
  { PARAM_PWM_POTS_CONTROL_MANUAL,   apply_param_pwm_pots_manual },
  { PARAM_ADSR3_ENABLED,             apply_param_adsr3_enabled },
  { PARAM_FUNCTION_KEY,              apply_param_function_key },
  { PARAM_CALIBRATION_FLAG,          apply_param_calibration_flag },
  { PARAM_MANUAL_CALIBRATION_FLAG,   apply_param_manual_calibration_flag },
  { PARAM_MANUAL_CALIBRATION_STAGE,  apply_param_manual_calibration_stage },
  { PARAM_MANUAL_CALIBRATION_OFFSET, apply_param_manual_calibration_offset },
  { PARAM_MANUAL_CALIBRATION_STEP,   apply_param_manual_calibration_step },
  { PARAM_AMP_COMP_440,              apply_param_amp_comp_440 },
  { PARAM_CAL_PW_CENTER,             apply_param_cal_pw_center },
  { PARAM_AMP_COMP_DUTY_OFFSET,      apply_param_amp_comp_duty_offset },
  { PARAM_GAP_FROM_DCO,              apply_param_gap_from_dco },
  { PARAM_MANUAL_CALIBRATION_STORE,  apply_param_manual_calibration_store },
  { PARAM_PRESET_SAVE,               apply_param_preset_save },
  { PARAM_PRESET_LOAD,               apply_param_preset_load },
  { PARAM_PRESET_DUMP,               apply_param_preset_dump },
  { PARAM_CAL_DUMP,                  apply_param_cal_dump },
  { PARAM_DEBUG_COMMAND,             apply_param_debug_command }
};

static const size_t paramTableSize =
  sizeof(paramTable) / sizeof(paramTable[0]);

static void (*paramApplyJump[PARAM_ROUTER_JUMP_SIZE])(int16_t);

void init_param_router() {
  param_router_build_jump(paramApplyJump, paramTable, paramTableSize);
}

// Public entry point: called from Serial/MIDI/UI code.
inline void update_parameters(uint16_t paramNumber, int16_t paramValue) {
  // Shadow every persistable param so presets can be captured without read-back.
  preset_shadow_capture(paramNumber, paramValue);
  param_router_apply_jump(paramApplyJump, paramNumber, paramValue);
}


