#include "include_all.h"

// Synchronized flag for pulse waveform activity

// ---- 1. Wave Selection (74HC595 / DG411) ----
static void apply_wave_enable(uint8_t osc, uint8_t wave, int16_t v) {
  if (osc > 2 || wave > 2) return;
  waveEnable[osc][wave] = (v != 0);
  update_waveSelector();
}

static void apply_param_osc1_saw_enable(int16_t v)   { apply_wave_enable(0, 0, v); }
static void apply_param_osc1_pulse_enable(int16_t v) { apply_wave_enable(0, 1, v); pulseWaveOn[0] = (v != 0);}
static void apply_param_osc1_tri_enable(int16_t v)   { apply_wave_enable(0, 2, v); }
static void apply_param_osc2_saw_enable(int16_t v)   { apply_wave_enable(1, 0, v); }
static void apply_param_osc2_pulse_enable(int16_t v) { apply_wave_enable(1, 1, v); pulseWaveOn[1] = (v != 0);}
static void apply_param_osc2_tri_enable(int16_t v)   { apply_wave_enable(1, 2, v); }
static void apply_param_osc3_saw_enable(int16_t v)   { apply_wave_enable(2, 0, v); }
static void apply_param_osc3_pulse_enable(int16_t v) { apply_wave_enable(2, 1, v); pulseWaveOn[2] = (v != 0);}
static void apply_param_osc3_tri_enable(int16_t v)   { apply_wave_enable(2, 2, v); }

static void apply_param_sine_status(int16_t /*v*/)   {}

// ---- 2. Dynamic Routing & Curves ----
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

static void apply_param_adsr3_to_osc_select(int16_t v) {
  ADSR3ToOscSelect = v;
}

static void apply_param_lfo1_waveform(int16_t v) {
  LFO1Waveform = v;
  LFO1_class.setWaveForm(LFO1Waveform);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

static void apply_param_lfo2_waveform(int16_t v) {
  LFO2Waveform = v;
  LFO2_class.setWaveForm(LFO2Waveform);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

static void apply_param_octave_shift(int16_t v) {
  octave_shift = v;
}

static void apply_param_osc2_interval(int16_t v) {
  OSC2_interval = v;
}

static void apply_param_osc3_interval(int16_t v) {
  OSC3_interval = v;
}

static void apply_param_osc2_detune_val(int16_t v) {
  OSC2DetuneVal = 512 - v;
}

static void apply_param_osc3_detune_val(int16_t v) {
  OSC3DetuneVal = 512 - v;
}

static void apply_param_lfo2_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO2_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo2_to_osc2(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC2_q24);
}

static void apply_param_lfo2_to_osc3(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC3_q24);
}

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

static void apply_param_character(int16_t v) {
  character = (uint8_t)constrain((int)v, 0, 128);
  character_recompute_scales();
}

static void apply_param_osc_sync_mode(int16_t v) {
  oscSync = v;
  if (oscSync < 2) {
    phaseAlignOSC2 = 0;
    pio_defer_request_reset_pulse_all();
  } else {
    if (oscSync > 8) {
      phaseAlignOSC2 = oscSync * 2;
    } else {
      switch (oscSync) {
        case 2: phaseAlignOSC2 = 45;  break;
        case 3: phaseAlignOSC2 = 90;  break;
        case 4: phaseAlignOSC2 = 135; break;
        case 5: phaseAlignOSC2 = 180; break;
        case 6: phaseAlignOSC2 = 225; break;
        case 7: phaseAlignOSC2 = 270; break;
        case 8: phaseAlignOSC2 = 315; break;
        default: break;
      }
    }
  }
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

static void apply_param_portamento_time(int16_t v) {
  portamento_parameter_value = (uint8_t)v;
  if (portamento_parameter_value == 0) {
    portamento_time_fixed = 0;
    portamento_time_slew = 0;
  } else if (portamento_parameter_value < 200) {
    const uint32_t t = (uint32_t)expConverter(portamento_parameter_value + 15, 100) * 500u;
    portamento_time_fixed = t;
    portamento_time_slew = t;
  } else {
    portamento_time_fixed = (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 10000000);
    portamento_time_slew  = (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 20000000);
  }
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

static void apply_param_vcf_keytrack(int16_t v) {
  VCFKeytrack = v;
#ifdef USE_FLOAT_CV_OUTS
  VCFKeytrackModifier = (VCFKeytrack != 0) ? ((float)VCFKeytrack / 8000.0f) : 1.0f;
#else
  VCFKeytrackModifier_q15 = (VCFKeytrack != 0) ? (((int32_t)VCFKeytrack * 32768) / 8000) : 32768;
#endif
}

static void apply_param_velocity_to_vcf(int16_t v) {
  velocityToVCFVal = (int8_t)v;
#ifdef USE_FLOAT_CV_OUTS
  velocityToVCF = velocityToVCFVal * 0.0003935f;
#else
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

// ---- 3. Local Mixer & Level Control ----
static void apply_param_osc1_level(int16_t v) {
  OSC1LevelVal = constrain(v, 0, 128);
  OSC1Level = lin_to_log_128[OSC1LevelVal];
}

static void apply_param_osc2_level(int16_t v) {
  OSC2LevelVal = constrain(v, 0, 128);
  OSC2Level = lin_to_log_128[OSC2LevelVal];
}

static void apply_param_osc3_level(int16_t v) {
  OSC3LevelVal = constrain(v, 0, 128);
  OSC3Level = lin_to_log_128[OSC3LevelVal];
}

static void apply_param_sub_level(int16_t v) {
  SubLevelVal = v;
  SubLevel = (uint16_t)constrain((int)SubLevelVal * 32, 0, 4095);
}

// ---- 4. Modulation Matrix ----
#define DECL_MOD_SLOT_APPLIERS(N) \
  static void apply_param_mod_slot##N##_source(int16_t v) { mod_matrix_set_source(N, v); } \
  static void apply_param_mod_slot##N##_dest(int16_t v)   { mod_matrix_set_dest(N, v); }   \
  static void apply_param_mod_slot##N##_depth(int16_t v)  { mod_matrix_set_depth(N, v); }

DECL_MOD_SLOT_APPLIERS(0)
DECL_MOD_SLOT_APPLIERS(1)
DECL_MOD_SLOT_APPLIERS(2)
DECL_MOD_SLOT_APPLIERS(3)
DECL_MOD_SLOT_APPLIERS(4)
DECL_MOD_SLOT_APPLIERS(5)
DECL_MOD_SLOT_APPLIERS(6)
DECL_MOD_SLOT_APPLIERS(7)
#undef DECL_MOD_SLOT_APPLIERS

static void apply_param_portamento_mode(int16_t v) {
  portamento_mode = (v == 0) ? PORTA_MODE_TIME : PORTA_MODE_SLEW;
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

static void apply_param_calibration_value(int16_t /*v*/) {}

static void apply_param_voice_mode(int16_t v) {
  voiceMode = v;
  setVoiceMode();
}

static void apply_param_voice_alloc_mode(int16_t v) {
  if (v < 0) return;
  voiceAlloc.setMode((uint8_t)v);
}

static void apply_param_unison_detune(int16_t v) {
  unisonDetune = v;
}

static void apply_param_analog_drift_amount(int16_t v) {
  analogDrift = v;
  drift_pitch_scale_q24 =
    (int32_t)((int32_t)analogDrift * DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE);
#ifndef USE_FLOAT_CV_OUTS
  vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}

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

static void apply_param_sync_mode(int16_t v) {
  if (manualCalibrationFlag) {
    manualCalSavedSyncMode = (uint8_t)v;
    return;
  }
  syncMode = v;
  pio_defer_request_sync_mode();
}

static void apply_param_soft_sync(int16_t v) {
  v = constrain(v, 0, 3);
  if (manualCalibrationFlag) {
    manualCalSavedSoftSyncChunks = (uint8_t)v;
    return;
  }
  softSyncChunks = (uint8_t)v;
  pio_defer_request_sync_mode();
}

static void apply_param_subosc_divide(int16_t v) {
  uint8_t divide = 0;
  if (v >= 4)      divide = 4;
  else if (v >= 2) divide = 2;
  pio_defer_request_subosc(divide);
}

static void apply_param_sub1_divide(int16_t v)   { subosc_param_divide(0, v); }
static void apply_param_sub2_divide(int16_t v)   { subosc_param_divide(1, v); }
static void apply_param_sub1_master(int16_t v)   { subosc_param_master(0, v); }
static void apply_param_sub2_master(int16_t v)   { subosc_param_master(1, v); }
static void apply_param_sub1_phase(int16_t v)    { subosc_param_phase(0, v); }
static void apply_param_sub2_phase(int16_t v)    { subosc_param_phase(1, v); }
static void apply_param_sub1_width(int16_t v)    { subosc_param_width(0, v); }
static void apply_param_sub2_width(int16_t v)    { subosc_param_width(1, v); }
static void apply_param_sub_logic_op(int16_t v)  { subosc_param_logic_op(v); }

static void apply_param_lfo1_to_dco(int16_t v) {
  LFO1toDCOVal = v;
  float lfo1_amt = (float)expConverterFloat(LFO1toDCOVal, 500) / 275000.0f;
  LFO1toDCO_q24 = lfo_pitch_depth_q24(lfo1_amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc1(int16_t v) { apply_param_lfo1_to_osc_depth(v, LFO1toOSC1_q24); }
static void apply_param_lfo1_to_osc2(int16_t v) { apply_param_lfo1_to_osc_depth(v, LFO1toOSC2_q24); }
static void apply_param_lfo1_to_osc3(int16_t v) { apply_param_lfo1_to_osc_depth(v, LFO1toOSC3_q24); }

static void apply_param_lfo1_speed(int16_t v) {
  LFO1SpeedVal = v;
  LFO1Speed = expConverterFloat(LFO1SpeedVal, 5000);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

static void apply_param_lfo2_speed(int16_t v) {
  LFO2SpeedVal = v;
  LFO2Speed = expConverterFloat(LFO2SpeedVal, 5000);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

static void apply_param_vca_level(int16_t v) {
  VCALevel = (uint16_t)constrain((int)v * 32, 0, 4095);
}

static void apply_param_dist_drive(int16_t v) {
  DIST_DRIVE = (uint16_t)constrain((int)v, 0, 4095);
}

static void apply_param_dist_mix(int16_t v) {
  DIST_MIX = (uint16_t)constrain((int)v, 0, 4095);
}

static void apply_param_filter_mode(int16_t v) {
  FILTER_MODE = (uint8_t)constrain((int)v, 0, 255);
}

static void apply_param_lfo1_to_vca(int16_t v) {
  LFO1toVCA = (uint16_t)constrain((int)v, 0, 4095);
  cv_bake_lfo1_to_vca_scale();
}

static void apply_param_lfo2_to_pw(int16_t v) {
  LFO2toPW = (int16_t)v;
}

// BUGFIX: Direct 1:1 scale for voices.ino PWM calculations
static void apply_param_adsr3_to_pwm(int16_t v) {
  ADSR1toPWM = (int16_t)v - 512;
  ADSR1toPWM_scale = ADSR1toPWM;
}

static void apply_param_adsr3_to_detune1(int16_t v) {
  ADSR1toDETUNE1 = (int16_t)v;
  if (ADSR1toDETUNE1 == 0) {
    ADSR1toDETUNE1_scale_q24 = 0;
  } else {
    const uint16_t mag_u = (ADSR1toDETUNE1 < 0) ? (uint16_t)(-ADSR1toDETUNE1) : (uint16_t)ADSR1toDETUNE1;
    const float mag = expConverterFloat(mag_u, 500);
    const float mag_full = expConverterFloat(ADSR_PITCH_DEPTH_PANEL_FULL, 500);
    float norm = (mag_full > 0.0f) ? (mag / mag_full) : 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const float signed_oct = (ADSR1toDETUNE1 < 0) ? -norm : norm;
    ADSR1toDETUNE1_scale_q24 = (int32_t)(signed_oct * ADSR_PITCH_MAX_OCTAVES * (float)(1 << 24) +
                                        ((signed_oct >= 0.0f) ? 0.5f : -0.5f));
  }
}

static void apply_param_adsr3_pitch_mode(int16_t v) {
  env_dco_pitch_centered = (v != 0) ? 1 : 0;
}

static void apply_param_adsr1_attack_curve(int16_t v) {
  ADSR1AttackCurveVal = (uint8_t)v;
  ADSR_VCA_change_attack_curve(ADSR1AttackCurveVal);
}

static void apply_param_adsr1_decay_curve(int16_t v) {
  ADSR1DecayCurveVal = (uint8_t)v;
  ADSR_VCA_change_decay_curve(ADSR1DecayCurveVal);
}

static void apply_param_adsr2_attack_curve(int16_t v) {
  ADSR2AttackCurveVal = (uint8_t)v;
  ADSR_VCF_change_attack_curve(ADSR2AttackCurveVal);
}

static void apply_param_adsr2_decay_curve(int16_t v) {
  ADSR2DecayCurveVal = (uint8_t)v;
  ADSR_VCF_change_decay_curve(ADSR2DecayCurveVal);
}

static void apply_param_pw_value(int16_t v) {
  uint16_t pwRaw = (uint16_t)constrain((int)v, 0, 4095);
  PW[0] = pwRaw / 4;
}

static void apply_param_adsr1_to_vca(int16_t v) {
  ADSR1toVCA = v;
}

static void apply_param_pwm_pots_manual(int16_t v) {
  PWMPotsControlManual = (v != 0);
}

static void apply_param_adsr3_enabled(int16_t v) {
  ADSR3Enabled = (v != 0);
}

static void apply_param_function_key(int16_t /*v*/) {}
static void apply_param_gap_from_dco(int16_t /*v*/) {}

// ---- 5. Calibration Decoders ----
static void apply_param_calibration_flag(int16_t v) {
  if (v == 0) {
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

static void apply_param_manual_calibration_flag(int16_t v) {
  if (v != 0 && !manualCalibrationFlag) {
    manualCalSavedSyncMode = syncMode;
    manualCalSavedSoftSyncChunks = softSyncChunks;
    syncMode = 0;
    softSyncChunks = 0;
    calSyncNeutralRequested = true;

    for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
      uint16_t packed = ((uint16_t)osc << 8) | (uint8_t)manualCalibrationOffset[osc];
      serialSendParam32(PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO, (uint32_t)packed);
    }
  }

  if (v == 0 && manualCalibrationFlag) {
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

  if (v != 0 && !manualCalibrationFlag) {
    manualCalibrationStep = 0;
  }

  manualCalibrationFlag = v;
  calibrationFlag       = v;
}

static void apply_param_manual_calibration_step(int16_t v) {
  manualCalibrationStep = (v != 0) ? 1 : 0;
}

static void apply_param_amp_comp_440(int16_t v) {
  uint8_t osc = cal_manual_osc();
  ampComp440[osc] = (uint16_t)constrain(v, 0, (int16_t)DIV_COUNTER);
}

static void apply_param_amp_comp_duty_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  ampCompDutyOffset[osc] = constrain(v, -500, 500);
}

static void apply_param_cal_pw_center(int16_t v) {
  const uint8_t ch = cal_pw_channel(cal_manual_osc());
  PW_CENTER[ch] = (uint16_t)constrain(v, 0, (int16_t)CAL_PW_CENTER_MAX);
}

static void apply_param_manual_calibration_stage(int16_t v) {
  int16_t stage = constrain(v, 0, (int16_t)cal_stage_max_n(NUM_OSCILLATORS));
  manualCalibrationStage = (uint8_t)stage;
  manualCalibrationStep = cal_stage_is_440_n((uint8_t)stage, NUM_OSCILLATORS) ? 1 : 0;
  if (cal_stage_is_440_n((uint8_t)stage, NUM_OSCILLATORS)) {
    serialSendParam16(PARAM_AMP_COMP_440, (int16_t)ampComp440[cal_manual_osc()], true);
  }
}

static void apply_param_manual_calibration_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  manualCalibrationOffset[osc] = (int8_t)v;
}

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

// ---- 6. Presets & Bench Opcodes ----
static void apply_param_preset_save(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_save((uint8_t)v);
}

static void apply_param_preset_load(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_load((uint8_t)v);
}

static void apply_param_preset_dump(int16_t v) { preset_store_dump(v); }
static void apply_param_cal_dump(int16_t v)    { preset_store_cal_dump(v); }

static void apply_param_debug_command(int16_t v) {
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
    pio_defer_request_reset_pulse_all();
    return;
  }

  switch (v) {
    case 1:  pio_topology_report(); break;
    case 2:  pio_period_probe(0, 2000); break;
    case 3:  pio_period_probe(0, 20000); break;
    case 4:  subosc2_report(); break;
#ifdef ENABLE_MEM_DIAG
    case 13: mem_diag_request(); break;
    case 14: mem_diag_runtime_enabled = false; break;
    case 15: mem_diag_runtime_enabled = true; break;
#endif
#ifdef RUNNING_AVERAGE
    case 10: bench_dump_request = true; break;
    case 11: bench_reset_all(); break;
    case 12: bench_periodic = !bench_periodic; break;
#endif
    case 20: amp_comp_set_method(AMP_COMP_FLOAT_QUAD); break;
    case 21: amp_comp_set_method(AMP_COMP_LUT); break;
    case 22: amp_comp_set_method(AMP_COMP_FIXED); break;
    case 26: note_retrig_set_mode(NOTE_RETRIG_EXACT_Y); break;
    case 27: note_retrig_set_mode(NOTE_RETRIG_SYNC_JMP); break;
    case 30: seed_fake_calibration_tables(true); break;
    case 34: autotuneAmpMethod = AMP_METHOD_CLASSIC; break;
    case 35: autotuneAmpMethod = AMP_METHOD_FREQ_TRACE; break;
    case 36: calibrationVerifyRequested = true; break;
    case 37: autotuneSearchMode = SEARCH_BISECT; break;
    case 38: autotuneSearchMode = SEARCH_INTERP; break;
    case 39: autotuneSearchMode = SEARCH_GATED; break;
    case 40: autotuneAmp0Mode = AMP0_MODE_MEASURE; break;
    case 41: autotuneAmp0Mode = AMP0_MODE_CALC; break;
    case 46:
      if (!manualCalibrationFlag) {
        Serial.println("[PW_PROBE] start manual calibration first");
      } else {
        pwCvProbeRequested = true;
      }
      break;
    default: break;
  }
}

// ---- Parameter Jump Table ----
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
  { PARAM_ADSR3_TO_PWM,              apply_param_adsr3_to_pwm },
  { PARAM_ADSR3_TO_DETUNE1,          apply_param_adsr3_to_detune1 },
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

static const size_t paramTableSize = sizeof(paramTable) / sizeof(paramTable[0]);
static void (*paramApplyJump[PARAM_ROUTER_JUMP_SIZE])(int16_t);

void init_param_router() {
  param_router_build_jump(paramApplyJump, paramTable, paramTableSize);
}

void update_parameters(uint8_t paramNumber, int16_t paramValue) {
  preset_shadow_capture(paramNumber, paramValue);
  param_router_apply_jump(paramApplyJump, paramNumber, paramValue);
}