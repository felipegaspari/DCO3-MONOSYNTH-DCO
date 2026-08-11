// Soft VCA/VCF/reso CV math (~10 kHz with ADSR). Matrix→pitch always.
// Analog VCA/VCF/AS2164/PWM only when ENABLE_CV_OUTS. Always-on: Q15 matrix, lerp>>12, note-60.
// USE_FLOAT_CV_OUTS: float VCA/VCF/keytrack/drift (A/B override).
// Else: fixed Q15 / integer path (shipping default both MCUs).
#include "include_all.h"
#include <string.h>

// Panel depth 0..512 → CV units. LFO Q15 peak uses *2 (legacy HALF/CC).
static constexpr int32_t CV_U12_MAX = 4095;           // last valid 12-bit CV code
static constexpr int32_t CV_U12_SCALE = 4096;         // 1<<12 — divisors / Q15→u12
static constexpr int32_t CV_PANEL_DEPTH_FULL = 512;
static constexpr int32_t CV_LFO_Q15_PEAK_DIV = CV_PANEL_DEPTH_FULL * 2;  // 1024

// Resonance → VCA compensation (1 ms path in update_CV_outs).
static constexpr int CV_VCA_COMP_DEFAULT = 100;
static constexpr int CV_RESO_COMP_MAX_RESONANCE = 2300;
static constexpr int CV_RESO_COMP_MIN_RESONANCE = 50;
static constexpr int CV_RESO_COMP_MAX_VCA = 316;
static constexpr int CV_RESO_COMP_SLOPE_Q8 = 36;  // ≈ 0.14 via (span * 36) >> 8

// Always-on: divide by CV_U12_SCALE via >>12 (not /4095).
static inline uint16_t lerp_0_4095(uint16_t value, uint16_t y0, uint16_t y1) {
  return (uint16_t)((int32_t)y0 + ((((int32_t)y1 - (int32_t)y0) * (int32_t)value) >> 12));
}

static inline uint16_t cv_clamp_u12(int32_t v) {
  if (v < 0) return 0;
  if (v > CV_U12_MAX) return (uint16_t)CV_U12_MAX;
  return (uint16_t)v;
}

// (q15 * CV_U12_SCALE) >> 15; with SCALE=4096 ≡ q15 >> 3 (peak → 4095).
static inline uint16_t cv_q15_to_u12(int16_t q15) {
  return cv_clamp_u12(((int32_t)q15 * CV_U12_SCALE) >> 15);
}

// Boot: build AS2164 VCA linearize table (same control points as Mainboard).
void init_cv_out() {
  generateBezierArray({ 0, 4095 }, { 4095, 0 }, { 150, 1420 }, { -235, 815 }, 4096, AS2164_VCA_linearize_table);
  cv_update_mod_scales();
#ifndef USE_FLOAT_CV_OUTS
  // Full-scale Q15 drift → VCF_DRIFT CV units ≈ analogDrift (legacy peak).
  vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}

// LFO scales carry the negative sign that restores the Mainboard's LFO polarity.
void cv_bake_adsr2_to_vcf_scale() {
#ifdef USE_FLOAT_CV_OUTS
  // Float A/B: u12 ADSR levels × (depth / panel_full).
  ADSR2toVCF_scale = (float)ADSR2toVCF / (float)CV_PANEL_DEPTH_FULL;
#else
  // Q15: (src_q15 * scale) >> 15 → depth * CV_U12_MAX / PANEL_DEPTH_FULL at +1.0.
  ADSR2toVCF_scale_q15 =
    (int32_t)(((int64_t)ADSR2toVCF * (int64_t)CV_U12_MAX) / CV_PANEL_DEPTH_FULL);
#endif
}

void cv_bake_lfo2_to_vcf_scale() {
#ifdef USE_FLOAT_CV_OUTS
  LFO2toVCF_scale =
    -(float)LFO2toVCF *
    ((float)CV_U12_MAX / ((float)CV_LFO_Q15_PEAK_DIV * 32767.0f));
#else
  LFO2toVCF_scale_q15 =
    (int32_t)(-((int64_t)LFO2toVCF * (int64_t)CV_U12_MAX) / CV_LFO_Q15_PEAK_DIV);
#endif
}

void cv_bake_lfo1_to_vca_scale() {
#ifdef USE_FLOAT_CV_OUTS
  LFO1toVCA_scale =
    -(float)LFO1toVCA *
    ((float)CV_U12_MAX / ((float)CV_LFO_Q15_PEAK_DIV * 32767.0f));
#else
  LFO1toVCA_scale_q15 =
    (int32_t)(-((int64_t)LFO1toVCA * (int64_t)CV_U12_MAX) / CV_LFO_Q15_PEAK_DIV);
#endif
}

void cv_update_mod_scales() {
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
  cv_bake_lfo1_to_vca_scale();
}

// Hot path (~10 kHz with ADSR): matrix→pitch always; analog VCA/VCF only if ENABLE_CV_OUTS.
void __not_in_flash_func(update_CV_outs)() {
#ifndef ENABLE_CV_OUTS
  if (manualCalibrationFlag) {
    matrix_pitch_mod_q24 = 0;
    return;
  }
  matrix_pitch_mod_q24 = mod_matrix_eval_pitch_q24(LFO1Level, LFO2Level);
  return;
#else
  // Snapshot Core0 LFO mailbox once (VCA/VCF + matrix LFO1/LFO2 sources).
  const int16_t local_LFO1Level = LFO1Level;
  const int16_t local_LFO2Level = LFO2Level;

  if (timer1msFlag2) {
    if (RESONANCEAmpCompensation) {
      // Always-on integer (both float/fixed CV builds).
      const int resonance_in = min((int)RESONANCE, CV_RESO_COMP_MAX_RESONANCE);
      VCAResonanceCompensation =
        (resonance_in >= CV_RESO_COMP_MIN_RESONANCE)
          ? (int16_t)(CV_RESO_COMP_MAX_VCA -
                      (((resonance_in - CV_RESO_COMP_MIN_RESONANCE) * CV_RESO_COMP_SLOPE_Q8) >> 8))
          : (int16_t)CV_RESO_COMP_MAX_VCA;
    } else {
      VCAResonanceCompensation = CV_VCA_COMP_DEFAULT;
    }

#ifdef USE_FLOAT_CV_OUTS
    if (VCFKeytrack != 0) {
      for (byte i = 0; i < NUM_VOICES; i++) {
        // map(note, 0, 150, -60, 90) ≡ note - 60
        VCFKeytrackPerVoice[i] =
          1.00f + (float)(VCFKeytrackModifier * ((int)VOICE_NOTES[i] - 60));
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCFKeytrackPerVoice[i] = 1.0f;
      }
    }

    if (analogDrift != 0) {
      // LFO_DRIFT_LEVEL is mo-lfo Q15 (±32767); full scale → ≈ analogDrift CV units.
      const int16_t local_drift0 = LFO_DRIFT_LEVEL[0];
      const float drift_scale = (float)analogDrift * (1.0f / 32767.0f);
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = (float)local_drift0 * drift_scale;
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = 0.0f;
      }
    }
#else
    if (VCFKeytrack != 0) {
      for (byte i = 0; i < NUM_VOICES; i++) {
        const int32_t dn = (int)VOICE_NOTES[i] - 60;
        VCFKeytrackPerVoice_q15[i] =
          32768 + (int32_t)(((int64_t)VCFKeytrackModifier_q15 * dn));
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCFKeytrackPerVoice_q15[i] = 32768;
      }
    }

    if (analogDrift != 0) {
      const int16_t local_drift0 = LFO_DRIFT_LEVEL[0];
      const int16_t drift_cv =
        (int16_t)(((int64_t)local_drift0 * (int64_t)vcf_drift_scale_q15) >> 15);
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = drift_cv;
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = 0;
      }
    }
#endif
  }

  int32_t mod_sums[MOD_DEST_COUNT];
  if (!manualCalibrationFlag) {
    mod_matrix_accumulate(mod_sums, local_LFO1Level, local_LFO2Level);
    matrix_pitch_mod_q24 = mod_matrix_pitch_to_q24(mod_sums[MOD_DEST_PITCH]);
  } else {
    memset(mod_sums, 0, sizeof(mod_sums));
    matrix_pitch_mod_q24 = 0;
  }
  const int32_t matrix_cutoff = mod_sums[MOD_DEST_VCF_CUTOFF];

#ifdef USE_FLOAT_CV_OUTS
  const int16_t LFO1toVCA_calc = (int16_t)((float)local_LFO1Level * LFO1toVCA_scale);
  const float LFO2toVCF_mod = (float)local_LFO2Level * LFO2toVCF_scale;
  // Q15 env → u12-ish for float depth formulas (same as >> 3).
  static constexpr float ADSR_Q15_TO_U12 = 1.0f / 8.0f;
  const float ADSR2toVCFcalculated =
    (float)ADSR_VCF_Level_q15 * ADSR_Q15_TO_U12 * ADSR2toVCF_scale;
  const float ADSR2toVCF2calculated =
    (float)ADSR_VCF2_Level_q15 * ADSR_Q15_TO_U12 * ADSR2toVCF_scale;
  const float matrix_cutoff_f = (float)matrix_cutoff;

  for (byte i = 0; i < NUM_VOICES; i++) {
    float VCA_velocityFactor = 1.0f;
    if (velocityToVCAVal != 0) {
      VCA_velocityFactor = 1.0f - ((float)velocityToVCA * (127 - midi_velocity[i]));
    }

    const uint16_t env_u12 = cv_q15_to_u12(ADSR_VCA_Level_q15[i]);
    int16_t LFO1toVCA_current = (ADSR_VCA_Level_q15[i] == 0) ? 0 : LFO1toVCA_calc;
    uint16_t VCA_Calculated =
      (uint16_t)constrain((float)(env_u12 + LFO1toVCA_current) * VCA_velocityFactor, 0, 4095);
    VCA_PWM[i] = lerp_0_4095(AS2164_VCA_linearize_table[VCA_Calculated],
                             (uint16_t)VCAResonanceCompensation, (uint16_t)(4095 - VCALevel));

    float VCF_velocityFactor = 1.0f;
    if (velocityToVCFVal != 0) {
      VCF_velocityFactor = 1.0f - ((float)velocityToVCF * (127 - midi_velocity[i]));
    }
    if (i == 0) {
      float combinedValue =
        ADSR2toVCFcalculated + LFO2toVCF_mod + (float)CUTOFF + VCF_DRIFT[i] + matrix_cutoff_f;
      float finalValue = combinedValue * VCF_velocityFactor * VCFKeytrackPerVoice[i];
      VCF_PWM[0] = (uint16_t)(4095 - (int)constrain(finalValue, 0, 4095));

      float combinedValue2 =
        ADSR2toVCF2calculated + LFO2toVCF_mod + (float)CUTOFF + VCF_DRIFT[i] + matrix_cutoff_f;
      float finalValue2 = combinedValue2 * VCF_velocityFactor * VCFKeytrackPerVoice[i];
      VCF_PWM[1] = (uint16_t)(4095 - (int)constrain(finalValue2, 0, 4095));
    }
  }
#else
  const int16_t LFO1toVCA_calc =
    (int16_t)(((int64_t)local_LFO1Level * (int64_t)LFO1toVCA_scale_q15) >> 15);
  const int32_t LFO2toVCF_mod =
    (int32_t)(((int64_t)local_LFO2Level * (int64_t)LFO2toVCF_scale_q15) >> 15);
  const int32_t ADSR2toVCFcalculated =
    (int32_t)(((int64_t)ADSR_VCF_Level_q15 * (int64_t)ADSR2toVCF_scale_q15) >> 15);
  const int32_t ADSR2toVCF2calculated =
    (int32_t)(((int64_t)ADSR_VCF2_Level_q15 * (int64_t)ADSR2toVCF_scale_q15) >> 15);

  for (byte i = 0; i < NUM_VOICES; i++) {
    int32_t vca_q15 = 32768;
    if (velocityToVCAVal != 0) {
      vca_q15 = 32768 - velocityToVCA_q15 * (127 - (int32_t)midi_velocity[i]);
      if (vca_q15 < 0) vca_q15 = 0;
    }

    const uint16_t env_u12 = cv_q15_to_u12(ADSR_VCA_Level_q15[i]);
    const int16_t LFO1toVCA_current = (ADSR_VCA_Level_q15[i] == 0) ? 0 : LFO1toVCA_calc;
    const int32_t vca_pre = (int32_t)env_u12 + (int32_t)LFO1toVCA_current;
    const uint16_t VCA_Calculated =
      cv_clamp_u12((int32_t)(((int64_t)vca_pre * vca_q15) >> 15));
    VCA_PWM[i] = lerp_0_4095(AS2164_VCA_linearize_table[VCA_Calculated],
                             (uint16_t)VCAResonanceCompensation, (uint16_t)(4095 - VCALevel));

    int32_t vcf_vel_q15 = 32768;
    if (velocityToVCFVal != 0) {
      vcf_vel_q15 = 32768 - velocityToVCF_q15 * (127 - (int32_t)midi_velocity[i]);
      if (vcf_vel_q15 < 0) vcf_vel_q15 = 0;
    }
    if (i == 0) {
      int32_t combined =
        ADSR2toVCFcalculated + LFO2toVCF_mod + (int32_t)CUTOFF + (int32_t)VCF_DRIFT[i] +
        matrix_cutoff;
      int32_t scaled = (int32_t)(((int64_t)combined * vcf_vel_q15) >> 15);
      scaled = (int32_t)(((int64_t)scaled * VCFKeytrackPerVoice_q15[i]) >> 15);
      VCF_PWM[0] = (uint16_t)(4095 - (int)cv_clamp_u12(scaled));

      int32_t combined2 =
        ADSR2toVCF2calculated + LFO2toVCF_mod + (int32_t)CUTOFF + (int32_t)VCF_DRIFT[i] +
        matrix_cutoff;
      int32_t scaled2 = (int32_t)(((int64_t)combined2 * vcf_vel_q15) >> 15);
      scaled2 = (int32_t)(((int64_t)scaled2 * VCFKeytrackPerVoice_q15[i]) >> 15);
      VCF_PWM[1] = (uint16_t)(4095 - (int)cv_clamp_u12(scaled2));
    }
  }
#endif

  uint16_t dist_out = DIST_DRIVE;
  uint16_t dist_mix_out = DIST_MIX;
  if (!manualCalibrationFlag) {
    mod_matrix_apply_cv(mod_sums, &dist_out, &dist_mix_out);
  } else {
    RESONANCE_PWM[0] = RESONANCE;
    RESONANCE_PWM[1] = RESONANCE;
  }

  write_cv_pwm_raw(VCF_PWM[0], RESONANCE_PWM, VCA_PWM[0], dist_out, dist_mix_out);
#endif  // ENABLE_CV_OUTS
}

// Manual calibration (Mainboard setPWMOutsManualCalibration): filter wide open, VCA barely
// cracked, and only the oscillator being calibrated audible. Runs instead of update_CV_outs.
void update_CV_outs_manual_calibration() {
  static constexpr uint16_t CAL_CUTOFF_COMPARE = 0;
  static constexpr uint16_t CAL_RESONANCE_COMPARE = 0;
  static constexpr uint16_t CAL_VCA_COMPARE = 150;
  static constexpr uint16_t CAL_SQR_ON = 50;
  static constexpr uint16_t CAL_SQR_MUTED = 4095;

  byte stage = (byte)manualCalibrationStage;
  if (stage > 2) stage = 2;

  waveSelector_manual_calibration(stage);

  OSC1Level = (stage == 0) ? CAL_SQR_ON : CAL_SQR_MUTED;
  OSC2Level = (stage == 1) ? CAL_SQR_ON : CAL_SQR_MUTED;
  OSC3Level = (stage == 2) ? CAL_SQR_ON : CAL_SQR_MUTED;
  SubLevel = CAL_SQR_MUTED;
#ifdef ENABLE_CV_OUTS
  write_level_pwm();
#endif

#ifdef ENABLE_CV_OUTS
  const uint16_t cal_reso[NUM_FILTERS] = { CAL_RESONANCE_COMPARE, CAL_RESONANCE_COMPARE };
  write_cv_pwm_raw(CAL_CUTOFF_COMPARE, cal_reso, CAL_VCA_COMPARE, 0, 0);
#endif
}
