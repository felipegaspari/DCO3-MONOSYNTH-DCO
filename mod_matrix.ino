#include "include_all.h"
#include <string.h>

static ModSlot g_mod_slots[MOD_SLOT_COUNT];
static int16_t mod_random_snh_q15 = 0;
static uint8_t mod_aftertouch = 0;
static uint8_t mod_wheel = 0;
// Bit i set when slot i has source, dest, nonzero depth. Hot path skips empty matrix.
static uint8_t g_mod_live_mask = 0;
static uint8_t g_mod_pitch_mask = 0;
static uint8_t g_mod_subosc_mask = 0;

static void mod_refresh_slot_live(uint8_t slot) {
  const ModSlot& s = g_mod_slots[slot];
  const uint8_t bit = (uint8_t)(1u << slot);
  const bool live = (s.source != MOD_SRC_EMPTY && s.dest != MOD_DEST_EMPTY && s.dest < MOD_DEST_COUNT &&
                     s.depth != 0);
  if (live) {
    g_mod_live_mask |= bit;
    if (s.dest == MOD_DEST_PITCH) {
      g_mod_pitch_mask |= bit;
    } else {
      g_mod_pitch_mask &= (uint8_t)~bit;
    }
    if (s.dest == MOD_DEST_SUB_PHASE || s.dest == MOD_DEST_SUB_PW) {
      g_mod_subosc_mask |= bit;
    } else {
      g_mod_subosc_mask &= (uint8_t)~bit;
    }
  } else {
    g_mod_live_mask &= (uint8_t)~bit;
    g_mod_pitch_mask &= (uint8_t)~bit;
    g_mod_subosc_mask &= (uint8_t)~bit;
  }
}

// |src_q15|≤32768, |depth|≤32767 → product fits int32 (no __aeabi_lmul).
static inline int32_t mod_depth_mul_q15(int32_t src_q15, int16_t depth) {
  return (src_q15 * (int32_t)depth) >> 15;
}

volatile int32_t matrix_pitch_mod_q24 = 0;

// Reciprocal of MOD_PITCH_DEPTH_FULL for Q24 octave: (pitch_s << 24) / 1023
// ≈ pitch_s * (2^24 / 1023). Constant = round(2^32 / 1023) used as (s * C) >> 32 after <<24...
// Simpler: (pitch_s * MOD_PITCH_TO_Q24_MUL) >> 15 with MUL = round(2^24 * 32768 / 1023).
static constexpr int32_t MOD_PITCH_TO_Q24_MUL =
  (int32_t)(((int64_t)1 << 24) * 32768 / (int64_t)MOD_PITCH_DEPTH_FULL);

static inline uint16_t mod_clamp_u16(int32_t v) {
  if (v < 0) return 0;
  if (v > 4095) return 4095;
  return (uint16_t)v;
}

static inline int32_t mod_clamp_q15(int32_t v) {
  if (v < -32768) return -32768;
  if (v > 32768) return 32768;
  return v;
}

void mod_matrix_init() {
  for (uint8_t i = 0; i < MOD_SLOT_COUNT; i++) {
    g_mod_slots[i].source = MOD_SRC_EMPTY;
    g_mod_slots[i].dest = MOD_DEST_EMPTY;
    g_mod_slots[i].depth = 0;
  }
  g_mod_live_mask = 0;
  g_mod_pitch_mask = 0;
  g_mod_subosc_mask = 0;
  subosc_mod_phase = 0;
  subosc_mod_pw = 0;
  mod_random_snh_q15 = 0;
  mod_aftertouch = 0;
  mod_wheel = 0;
  matrix_pitch_mod_q24 = 0;
}

void mod_matrix_set_source(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  if (v < 0 || v >= (int16_t)MOD_SRC_COUNT) {
    g_mod_slots[slot].source = MOD_SRC_EMPTY;
  } else {
    g_mod_slots[slot].source = (uint8_t)v;
  }
  mod_refresh_slot_live(slot);
}

void mod_matrix_set_dest(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  if (v < 0 || v >= (int16_t)MOD_DEST_COUNT) {
    g_mod_slots[slot].dest = MOD_DEST_EMPTY;
  } else {
    g_mod_slots[slot].dest = (uint8_t)v;
  }
  mod_refresh_slot_live(slot);
}

void mod_matrix_set_depth(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  g_mod_slots[slot].depth = v;
  mod_refresh_slot_live(slot);
}

void mod_matrix_on_note_on() {
  // ±1.0 Q15; random(0..2000)-1000 → scale to ±32768.
  mod_random_snh_q15 = (int16_t)(((int32_t)random(0, 2001) - 1000) * 32);
}

void mod_matrix_set_aftertouch(uint8_t pressure) {
  mod_aftertouch = pressure;
}

void mod_matrix_set_mod_wheel(uint8_t value) {
  mod_wheel = value;
}

// Source as Q15 (±32768 ≈ ±1.0). LFO/ADSR/noise are already Q15 pass-through.
static int32_t mod_matrix_read_source_q15(uint8_t src, int16_t lfo1_q15, int16_t lfo2_q15) {
  switch (src) {
    case MOD_SRC_ADSR3:
      return (int32_t)ADSR1Level_q15[0];
    case MOD_SRC_ADSR4:
    case MOD_SRC_LFO3:
    case MOD_SRC_LFO4:
      return 0;
    case MOD_SRC_VELOCITY:
      return (int32_t)midi_velocity[0] * 258;
    case MOD_SRC_KEYTRACK: {
      const uint8_t note = VOICE_NOTES[0];
      if (note == 0) return 0;
      // 32768 / 48 ≈ 682
      return mod_clamp_q15(((int32_t)note - 60) * 682);
    }
    case MOD_SRC_RANDOM:
      return (int32_t)mod_random_snh_q15;
    case MOD_SRC_AFTERTOUCH:
      return (int32_t)mod_aftertouch * 258;
    case MOD_SRC_LFO1:
      return (int32_t)lfo1_q15;
    case MOD_SRC_LFO2:
      return (int32_t)lfo2_q15;
    case MOD_SRC_PITCH_BEND:
      return ((int32_t)midi_pitch_bend - 8192) << 2;
    case MOD_SRC_MOD_WHEEL:
      return (int32_t)mod_wheel * 258;
    case MOD_SRC_NOISE0:
    case MOD_SRC_NOISE1:
    case MOD_SRC_NOISE2:
    case MOD_SRC_NOISE3: {
      const uint8_t i = (uint8_t)(src - MOD_SRC_NOISE0);
      if (i >= NUM_NOISE_GENS) return 0;
      return (int32_t)noiseLevel[i];
    }
    default:
      return 0;
  }
}

void mod_matrix_accumulate(int32_t dest_sums[MOD_DEST_COUNT], int16_t lfo1_q15, int16_t lfo2_q15) {
  memset(dest_sums, 0, sizeof(int32_t) * MOD_DEST_COUNT);
  if (g_mod_live_mask == 0) {
    return;
  }

  uint8_t mask = g_mod_live_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
#ifndef ENABLE_CV_OUTS
    if (s.dest != MOD_DEST_PITCH) {
      continue;
    }
#endif
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    dest_sums[s.dest] += mod_depth_mul_q15(src_q15, s.depth);
  }
}

int32_t __not_in_flash_func(mod_matrix_eval_pitch_q24)(int16_t lfo1_q15, int16_t lfo2_q15) {
  if (g_mod_pitch_mask == 0) {
    return 0;
  }
  int32_t pitch_s = 0;
  uint8_t mask = g_mod_pitch_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    pitch_s += mod_depth_mul_q15(src_q15, s.depth);
  }
  return mod_matrix_pitch_to_q24(pitch_s);
}

// Sub phase and PW, left in matrix units for subosc_compute_words() to scale and clamp. Both
// are always assigned, so clearing the last slot that targeted them clears the offset too.
void __not_in_flash_func(mod_matrix_eval_subosc)(int16_t lfo1_q15, int16_t lfo2_q15) {
  int32_t phase_s = 0;
  int32_t pw_s = 0;
  uint8_t mask = g_mod_subosc_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    const int32_t amount = mod_depth_mul_q15(src_q15, s.depth);
    if (s.dest == MOD_DEST_SUB_PHASE) {
      phase_s += amount;
    } else {
      pw_s += amount;
    }
  }
  subosc_mod_phase = phase_s;
  subosc_mod_pw = pw_s;
}

// Convert clamped pitch dest sum (±1023) to Q24 octave without a hot divide.
static inline int32_t mod_pitch_sum_to_q24(int32_t pitch_s) {
  if (pitch_s > MOD_PITCH_DEPTH_FULL) pitch_s = MOD_PITCH_DEPTH_FULL;
  if (pitch_s < -MOD_PITCH_DEPTH_FULL) pitch_s = -MOD_PITCH_DEPTH_FULL;
  return (int32_t)(((int64_t)pitch_s * (int64_t)MOD_PITCH_TO_Q24_MUL) >> 15);
}

// Convert clamped pitch dest sum (±1023) to Q24 octave without a hot divide.
int32_t mod_matrix_pitch_to_q24(int32_t pitch_s) {
  return mod_pitch_sum_to_q24(pitch_s);
}

void mod_matrix_apply_cv(const int32_t dest_sums[MOD_DEST_COUNT], uint16_t* dist_drive_out,
                         uint16_t* dist_mix_out) {
#ifdef ENABLE_CV_OUTS
  write_level_pwm_raw(
    mod_clamp_u16((int32_t)OSC1Level - dest_sums[MOD_DEST_OSC1_LEVEL]),
    mod_clamp_u16((int32_t)OSC2Level - dest_sums[MOD_DEST_OSC2_LEVEL]),
    mod_clamp_u16((int32_t)OSC3Level - dest_sums[MOD_DEST_OSC3_LEVEL]),
    mod_clamp_u16((int32_t)SubLevel - dest_sums[MOD_DEST_SUB_LEVEL]));

  RESONANCE_PWM[0] = mod_clamp_u16((int32_t)RESONANCE + dest_sums[MOD_DEST_VCF1_RESO]);
  RESONANCE_PWM[1] = mod_clamp_u16((int32_t)RESONANCE + dest_sums[MOD_DEST_VCF2_RESO]);
#else
  (void)dest_sums;
#endif

  if (dist_drive_out) {
    *dist_drive_out = mod_clamp_u16((int32_t)DIST_DRIVE + dest_sums[MOD_DEST_DIST_DRIVE]);
  }
  if (dist_mix_out) {
    *dist_mix_out = mod_clamp_u16((int32_t)DIST_MIX + dest_sums[MOD_DEST_DIST_MIX]);
  }
}
