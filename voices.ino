#include "include_all.h"
#include <limits.h>
#include <math.h>
#include "clkdiv.h"

// Enable/disable detailed DCO debug report (including OSC1 frequency stages)
#define DCO_DEBUG_REPORT 0

static void amp_chan_levels_fixed(int64_t freq_q24_A, int64_t freq_q24_B, int64_t freq_q24_C,
                                  uint8_t oscA, uint8_t oscB, uint8_t oscC,
                                  uint16_t *outA, uint16_t *outB, uint16_t *outC);
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
int32_t interpolateRatioQ16_cached(int32_t xQ16, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t interpolatePitchMultiplierIntQ16_cached(int32_t xQ16, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
float interpolateRatioFloat_cached(float x, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
float interpolateRatioFloat_cached_fast(float x, int dcoIndex);
#endif

// Live pitch interp: compile-time wrappers (always_inline; not function pointers).
// Fixed-voice wrappers only — float voice uses interpolate_live_ratio_f (FLOAT_FAST would
// otherwise type-check the Q12 #else and fail: IntQ16 is not compiled).
#ifndef USE_FLOAT_VOICE_TASK
static inline __attribute__((always_inline)) int32_t modifiers_q24_to_xQ16(int64_t modifiers_q24) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return (modifiers_q24 >= 0) ? (int32_t)(modifiers_q24 >> 8)
                              : (int32_t)(-((-modifiers_q24) >> 8));
#else
  int64_t x_q24s = modifiers_q24 * (int64_t)multiplierTableScale;
  return (x_q24s >= 0) ? (int32_t)(x_q24s >> 8) : (int32_t)(-((-x_q24s) >> 8));
#endif
}

static inline __attribute__((always_inline)) int32_t interpolate_live_ratio_q16(int32_t xQ16, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return interpolateRatioQ16_cached(xQ16, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  int32_t yTab = interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex);
  uint64_t num = ((uint64_t)(uint32_t)yTab << 16) + 5000u;
  return (int32_t)((num * 0xD1B71759ULL) >> 45);
#else
#error "interpolate_live_ratio_q16: PITCH_INTERP_FLOAT / FLOAT_FAST require USE_FLOAT_VOICE_TASK"
#endif
}
#endif  // !USE_FLOAT_VOICE_TASK

static inline __attribute__((always_inline)) float interpolate_live_ratio_f(float modifiers, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
  return interpolateRatioFloat_cached(modifiers, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  return interpolateRatioFloat_cached_fast(modifiers, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  int32_t xQ16 = (int32_t)lroundf(modifiers * 65536.0f);
  return (float)interpolateRatioQ16_cached(xQ16, dcoIndex) * (1.0f / 65536.0f);
#else
  float x = modifiers * (float)multiplierTableScale;
  int32_t xQ16 = (int32_t)lroundf(x * 65536.0f);
  return (float)interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex)
         / (float)multiplierTableScale;
#endif
}

// Boot init: seed notes, build pitch tables, apply voice mode, run one voice_task_main().
void init_voices() {
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    VOICE_NOTES[i] = DCO_calibration_start_note;
  }

  initMultiplierTables();
  setVoiceMode();
  voice_task_main();
}

// Fast helper: convert a Q16 note (semitones) to Q24 frequency using linear
// interpolation on the sNotePitches_q24 table. Used in slew-rate mode.
static inline int64_t noteQ16_to_freqQ24(int32_t note_q16) {
  const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
  if (NOTE_TABLE_LEN == 0) return 0;

  int32_t noteInt = note_q16 >> 16;
  uint32_t frac = (uint32_t)note_q16 & 0xFFFF;

  if (noteInt <= 0) {
    if (NOTE_TABLE_LEN == 1) return sNotePitches_q24[0];
    if (frac == 0) return sNotePitches_q24[0];
    int64_t f0 = sNotePitches_q24[0];
    int64_t f1 = sNotePitches_q24[1];
    int64_t df = f1 - f0;
    return f0 + ((df * (int64_t)frac) >> 16);
  }
  if ((size_t)noteInt >= NOTE_TABLE_LEN - 1) {
    // Clamp to top of table
    return sNotePitches_q24[NOTE_TABLE_LEN - 1];
  }

  if (frac == 0) {
    // Exact semitone, just return table entry (common case).
    return sNotePitches_q24[noteInt];
  }

  int64_t f0 = sNotePitches_q24[noteInt];
  int64_t f1 = sNotePitches_q24[noteInt + 1];
  int64_t df = f1 - f0;
  return f0 + ((df * (int64_t)frac) >> 16);
}

// Inverse of noteQ16_to_freqQ24: Q24 Hz → Q16 note via binary search + linear frac.
static inline int32_t freqQ24_to_noteQ16(int64_t freq_q24) {
  const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
  if (NOTE_TABLE_LEN == 0) return 0;
  if (NOTE_TABLE_LEN == 1) return 0;
  if (freq_q24 <= sNotePitches_q24[0]) return 0;
  if (freq_q24 >= sNotePitches_q24[NOTE_TABLE_LEN - 1]) {
    return ((int32_t)(NOTE_TABLE_LEN - 1)) << 16;
  }

  size_t lo = 0;
  size_t hi = NOTE_TABLE_LEN - 1;
  while (hi - lo > 1) {
    size_t mid = lo + ((hi - lo) >> 1);
    if (sNotePitches_q24[mid] <= freq_q24) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  int64_t f0 = sNotePitches_q24[lo];
  int64_t f1 = sNotePitches_q24[hi];
  int64_t df = f1 - f0;
  if (df <= 0) return ((int32_t)lo) << 16;

  int64_t num = (freq_q24 - f0) << 16;
  int32_t frac = (int32_t)(num / df);
  if (frac < 0) frac = 0;
  if (frac > 0xFFFF) frac = 0xFFFF;
  return (((int32_t)lo) << 16) + frac;
}

// Helper: convert float Hz to Q24 fixed-point (Hz * 2^24)
static inline int64_t float_to_q24(float f) {
  return (int64_t)lrintf(f * (float)(1 << 24));
}

// midi/base + offset → table index using signed math (no uint8 wrap-to-top).
// High notes still fold down by octaves to stay within highestNote, then clamp.
static inline uint8_t midi_offset_to_table_index(int midi_or_base, int offset, size_t table_len) {
  int n = midi_or_base - 36 + offset;
  if (n < 0) n = 0;
  while (n > (int)highestNote) n -= 12;
  if (table_len == 0) return 0;
  if (n >= (int)table_len) n = (int)table_len - 1;
  return (uint8_t)n;
}

// Resolve porta start note (Q16). First edge snaps; later edges use cur note (incl. index 0).
static inline int32_t porta_resolve_start_note_q16(uint8_t osc, int32_t target_q16) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_q16[osc];
  }
  if (portamento_cur_freq_q24[osc] > 0) {
    porta_note_valid[osc] = true;
    return freqQ24_to_noteQ16(portamento_cur_freq_q24[osc]);
  }
  porta_note_valid[osc] = true;
  return target_q16;
}

// Common endpoint latch for note-space porta setup.
static inline void porta_latch_endpoints_q16(uint8_t osc, int32_t start_q16, int32_t target_q16) {
  porta_note_start_q16[osc] = start_q16;
  porta_note_stop_q16[osc] = target_q16;
  porta_note_cur_q16[osc] = start_q16;
  porta_note_valid[osc] = true;
  int64_t freq_q24 = noteQ16_to_freqQ24(start_q16);
  portamento_start_q24[osc] = freq_q24;
  portamento_stop_q24[osc] = noteQ16_to_freqQ24(target_q16);
  portamento_cur_freq_q24[osc] = freq_q24;
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static inline void porta_setup_time_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                       int32_t T_fixed) {
  if (T_fixed < 1) T_fixed = 1;
  porta_latch_endpoints_q16(osc, start_q16, target_q16);

  int32_t dNote_q16 = target_q16 - start_q16;
  int64_t halfT = (int64_t)T_fixed >> 1;
  int64_t num = (dNote_q16 >= 0) ? ((int64_t)dNote_q16 + halfT) : ((int64_t)dNote_q16 - halfT);
  porta_note_step_q16[osc] = (int32_t)(num / (int64_t)T_fixed);
  if (dNote_q16 != 0 && porta_note_step_q16[osc] == 0) {
    porta_note_step_q16[osc] = (dNote_q16 > 0) ? 1 : -1;
  }
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static inline void porta_setup_slew_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                       int32_t T_slew) {
  if (T_slew < 1) T_slew = 1;
  porta_latch_endpoints_q16(osc, start_q16, target_q16);

  int32_t dNote_q16 = target_q16 - start_q16;
  if (dNote_q16 == 0) {
    porta_note_step_q16[osc] = 0;
  } else {
    int32_t rate = (int32_t)(((int64_t)12 << 16) / (int64_t)T_slew);
    if (rate == 0) rate = 1;
    porta_note_step_q16[osc] = (dNote_q16 > 0) ? rate : -rate;
  }
}

static inline void porta_setup_glide_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                        uint8_t mode) {
  if (mode == PORTA_MODE_TIME) {
    int32_t T = (portamento_time_fixed == 0) ? 1 : (int32_t)portamento_time_fixed;
    porta_setup_time_q16(osc, start_q16, target_q16, T);
  } else {
    int32_t T = (portamento_time_slew == 0) ? 1 : (int32_t)portamento_time_slew;
    porta_setup_slew_q16(osc, start_q16, target_q16, T);
  }
}

#ifdef USE_FLOAT_VOICE_TASK
// Helper: convert a semitone index (float) to Hz using sNotePitches[] with linear interpolation.
static inline float noteIndex_to_freqFloat(float noteIndex) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0) return 0.0f;
  if (noteIndex <= 0.0f) return sNotePitches[0];
  if (noteIndex >= (float)(LEN - 1)) return sNotePitches[LEN - 1];

  int n0 = (int)floorf(noteIndex);
  int n1 = n0 + 1;
  float t = noteIndex - (float)n0;
  float f0 = sNotePitches[n0];
  float f1 = sNotePitches[n1];
  return f0 + (f1 - f0) * t;
}

// Inverse of noteIndex_to_freqFloat: Hz → semitone index via binary search + linear frac.
static inline float freqFloat_to_noteIndex(float hz) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0) return 0.0f;
  if (LEN == 1) return 0.0f;
  if (hz <= sNotePitches[0]) return 0.0f;
  if (hz >= sNotePitches[LEN - 1]) return (float)(LEN - 1);

  size_t lo = 0;
  size_t hi = LEN - 1;
  while (hi - lo > 1) {
    size_t mid = lo + ((hi - lo) >> 1);
    if (sNotePitches[mid] <= hz) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  float f0 = sNotePitches[lo];
  float f1 = sNotePitches[hi];
  float df = f1 - f0;
  if (df <= 0.0f) return (float)lo;
  return (float)lo + (hz - f0) / df;
}

static inline float porta_resolve_start_note_f(uint8_t osc, float target) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_f[osc];
  }
  if (porta_freq_cur_f[osc] > 0.0f) {
    porta_note_valid[osc] = true;
    return freqFloat_to_noteIndex(porta_freq_cur_f[osc]);
  }
  porta_note_valid[osc] = true;
  return target;
}

static inline void porta_latch_endpoints_f(uint8_t osc, float startNote, float targetNote) {
  porta_note_start_f[osc] = startNote;
  porta_note_stop_f[osc] = targetNote;
  porta_note_cur_f[osc] = startNote;
  porta_note_valid[osc] = true;
  float startHz = noteIndex_to_freqFloat(startNote);
  float stopHz = noteIndex_to_freqFloat(targetNote);
  porta_freq_start_f[osc] = startHz;
  porta_freq_stop_f[osc] = stopHz;
  porta_freq_cur_f[osc] = startHz;
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static inline void porta_setup_time_f(uint8_t osc, float startNote, float targetNote, float T_fixed) {
  if (T_fixed < 1.0f) T_fixed = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);

  float dNote = targetNote - startNote;
  const float SCALE = 65536.0f;
  float halfT = 0.5f * T_fixed;
  float d_q16 = dNote * SCALE;
  float num = (d_q16 >= 0.0f) ? (d_q16 + halfT) : (d_q16 - halfT);
  float stepNote = (num / T_fixed) / SCALE;
  if (dNote != 0.0f && stepNote == 0.0f) {
    stepNote = (dNote > 0.0f) ? (1.0f / SCALE) : (-1.0f / SCALE);
  }
  porta_note_step_f[osc] = stepNote;
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static inline void porta_setup_slew_f(uint8_t osc, float startNote, float targetNote, float T_slew) {
  if (T_slew < 1.0f) T_slew = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);

  float dNote = targetNote - startNote;
  if (dNote == 0.0f) {
    porta_note_step_f[osc] = 0.0f;
  } else {
    float rate = 12.0f / T_slew;
    if (rate == 0.0f) rate = 1.0f / 65536.0f;
    porta_note_step_f[osc] = (dNote > 0.0f) ? rate : -rate;
  }
}

static inline void porta_setup_glide_f(uint8_t osc, float startNote, float targetNote, uint8_t mode) {
  if (mode == PORTA_MODE_TIME) {
    float T = (portamento_time_fixed == 0) ? 1.0f : (float)portamento_time_fixed;
    porta_setup_time_f(osc, startNote, targetNote, T);
  } else {
    float T = (portamento_time_slew == 0) ? 1.0f : (float)portamento_time_slew;
    porta_setup_slew_f(osc, startNote, targetNote, T);
  }
}

// Q24 → float modifier/Hz scale (multiply avoids per-sample divide by 2^24).
static constexpr float Q24_TO_FLOAT = 1.0f / 16777216.0f;

static inline float q24_to_float(int32_t q) {
  return (float)q * Q24_TO_FLOAT;
}
#endif

#ifndef USE_FLOAT_VOICE_TASK
// Fixed-point realtime voice engine (portamento, modifiers, clkdiv, amp, PIO/PWM/PW).
// Selected by voice_task_main() when USE_FLOAT_VOICE_TASK is not defined.
void __not_in_flash_func(voice_task_fixed_point)() {
  // Track portamento-time and mode changes between calls so we can smoothly
  // retime the glide without introducing pitch discontinuities.
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_TIME;
  static uint8_t lastNote1 = 0, lastNote2 = 0, lastNote3 = 0;
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  // Pre-calculate pitch bend as a Q24 value. This is done once per voice_task_fixed_point call.
  int32_t calcPitchbend_q24;

  BENCH_BEGIN(vt_pitchbend);
  // Optimized: Perform pitch bend calculation entirely in fixed-point Q24.
  // ((bend / 8192.0) - 1.0) * pitchBendMultiplier
  // This avoids float conversions and multiplications in the hot path.
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  calcPitchbend_q24 = (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  // Latch note-on edges for every voice slot (para may set flags on 1/2).
  for (int k = 0; k < NUM_VOICES; k++) {
    if (note_on_flag[k] == 1) {
      note_on_flag_flag[k] = true;
      note_on_flag[k] = 0;
    }
  }

  for (int i = 0; i < NUM_VOICES_VOICE_TASK; i++) {

#if DCO_DEBUG_REPORT
    // Debug: track OSC1 frequency at key stages of the pipeline for DCO report.
    float dbg_freq_base_Hz = 0.0f;       // After portamento, before modifiers
    float dbg_freq_after_mod_Hz = 0.0f;  // After all modifiers applied (freq_q24_A)
#endif

    // note_off keeps VOICE_NOTES (release pitch); free/steal uses VOICES[].
    // Never-played slots stay 0 — gate midi→table math (uint8 underflow → shriek).

    // Note index convention: table_index = midi - 36 + offset (36 ⇒ unison).
    //   Mono: octave_shift = global octave; OSC2/3 = relative to note1 (36 ⇒ unison with OSC1).
    //   Poly/stack: every osc uses octave_shift only (shared octave); not OSC2/3.
    const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
    uint8_t note1, note2, note3;
    if (voiceMode == 0) {
      const uint8_t vn = VOICE_NOTES[i];
      if (vn == 0) {
        note1 = note2 = note3 = 0;
      } else {
        note1 = midi_offset_to_table_index((int)vn, (int)octave_shift, NOTE_TABLE_LEN);
        note2 = midi_offset_to_table_index((int)note1, (int)OSC2_interval, NOTE_TABLE_LEN);
        note3 = midi_offset_to_table_index((int)note1, (int)OSC3_interval, NOTE_TABLE_LEN);
      }
    } else {
      const uint8_t vn0 = VOICE_NOTES[0];
      const uint8_t vn1 = VOICE_NOTES[1];
      const uint8_t vn2 = VOICE_NOTES[2];
      note1 = (vn0 == 0) ? 0 : midi_offset_to_table_index((int)vn0, (int)octave_shift, NOTE_TABLE_LEN);
      note2 = (vn1 == 0) ? 0 : midi_offset_to_table_index((int)vn1, (int)octave_shift, NOTE_TABLE_LEN);
      note3 = (vn2 == 0) ? 0 : midi_offset_to_table_index((int)vn2, (int)octave_shift, NOTE_TABLE_LEN);
    }

      const bool pitchTargetChanged =
          note1 != lastNote1 || note2 != lastNote2 || note3 != lastNote3;
      lastNote1 = note1;
      lastNote2 = note2;
      lastNote3 = note3;

      BENCH_BEGIN(vt_osc_detune);
      // Optimized: Calculate OSC2/OSC3 detune in Q24 and keep it there.
      // The float conversion has been removed as it is no longer needed.
      // detune = 1.0 + 0.0002 * (256 - val)
      static constexpr int32_t DETUNE_SCALE_Q24 = (int32_t)(0.0002f * (float)(1 << 24) + 0.5f);
      int32_t detune_steps = ((int)256 - OSC2DetuneVal);
      int32_t detune_q24 = (1 << 24) + (detune_steps * DETUNE_SCALE_Q24);
      int32_t detune3_steps = ((int)256 - OSC3DetuneVal);
      int32_t detune3_q24 = (1 << 24) + (detune3_steps * DETUNE_SCALE_Q24);
      BENCH_END(vt_osc_detune);

      int64_t freq_q24_A;
      int64_t freq_q24_B;
      int64_t freq_q24_C;

      // Fixed osc indices for current mono hardware (3 oscs on voice 0).
      // Future paraphonic mode can remap osc ownership per voice without gutting allocation.
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;

      ////***********************    PORTAMENTO CODE   ****************************************/////
      BENCH_BEGIN(vt_portamento);
      if (portaTime > 0 /*&& portamento_start != 0 && portamento_stop != 0*/) {
        uint32_t now_us = micros();
        if (voiceMode == 0) {
          portamentoTimer[i] = now_us - portamentoStartMicros[i];
        } else {
          for (int k = 0; k < NUM_VOICES; k++) {
            portamentoTimer[k] = now_us - portamentoStartMicros[k];
          }
        }

        // Mono: one edge restarts all three oscs. Para: each note_on_flag_flag[k] restarts osc k only.
        // TIME = fixed duration any interval; SLEW = constant semitone rate (time ∝ interval).
        if (voiceMode == 0) {
          if (note_on_flag_flag[i]) {
            portamentoStartMicros[i] = now_us;
            portamentoTimer[i] = 0;

            int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
            int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
            int32_t targetNoteC_q16 = ((int32_t)note3) << 16;
            porta_setup_glide_q16(DCO_A, porta_resolve_start_note_q16(DCO_A, targetNoteA_q16),
                                  targetNoteA_q16, portaMode);
            porta_setup_glide_q16(DCO_B, porta_resolve_start_note_q16(DCO_B, targetNoteB_q16),
                                  targetNoteB_q16, portaMode);
            porta_setup_glide_q16(DCO_C, porta_resolve_start_note_q16(DCO_C, targetNoteC_q16),
                                  targetNoteC_q16, portaMode);
          }
        } else {
          // Paraphonic: restart porta per voice slot → osc k (1:1).
          const uint8_t targetNotes[3] = { note1, note2, note3 };
          for (int k = 0; k < NUM_VOICES; k++) {
            if (!note_on_flag_flag[k]) continue;
            const uint8_t osc = (uint8_t)k;
            portamentoStartMicros[k] = now_us;
            portamentoTimer[k] = 0;
            int32_t target_q16 = ((int32_t)targetNotes[k]) << 16;
            porta_setup_glide_q16(osc, porta_resolve_start_note_q16(osc, target_q16),
                                  target_q16, portaMode);
          }
        }

        // Control-change: skip steady (stale start/step + huge elapsed → pitch jump).
        const bool portaNoteOnEdge =
            (voiceMode == 0) ? note_on_flag_flag[i]
                             : (note_on_flag_flag[0] || note_on_flag_flag[1] || note_on_flag_flag[2]);
        const bool portaDoRetime =
            (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !portaNoteOnEdge;

        int64_t curA;
        int64_t curB;
        int64_t curC;

        if (portaDoRetime) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i] = 0;
          if (voiceMode != 0) {
            for (int k = 0; k < NUM_VOICES; k++) {
              portamentoStartMicros[k] = now_us;
              portamentoTimer[k] = 0;
            }
          }

          int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
          int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
          int32_t targetNoteC_q16 = ((int32_t)note3) << 16;
          // Retime from true current Hz so a mid-glide CC change stays continuous.
          int32_t curNoteA_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_A]);
          int32_t curNoteB_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_B]);
          int32_t curNoteC_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_C]);
          porta_setup_glide_q16(DCO_A, curNoteA_q16, targetNoteA_q16, portaMode);
          porta_setup_glide_q16(DCO_B, curNoteB_q16, targetNoteB_q16, portaMode);
          porta_setup_glide_q16(DCO_C, curNoteC_q16, targetNoteC_q16, portaMode);
          curA = portamento_cur_freq_q24[DCO_A];
          curB = portamento_cur_freq_q24[DCO_B];
          curC = portamento_cur_freq_q24[DCO_C];
        } else if (porta_note_cur_q16[DCO_A] == porta_note_stop_q16[DCO_A] &&
                   porta_note_cur_q16[DCO_B] == porta_note_stop_q16[DCO_B] &&
                   porta_note_cur_q16[DCO_C] == porta_note_stop_q16[DCO_C]) {
          // Glide done, portaTime still > 0: reuse stop Hz (no int64 advance / table lerp).
          curA = portamento_stop_q24[DCO_A];
          curB = portamento_stop_q24[DCO_B];
          curC = portamento_stop_q24[DCO_C];
        } else {
          int32_t elapsedA = (voiceMode == 0) ? (int32_t)portamentoTimer[i] : (int32_t)portamentoTimer[0];
          int32_t elapsedB = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[1];
          int32_t elapsedC = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[2];

          int32_t dNoteA_q16 = porta_note_stop_q16[DCO_A] - porta_note_start_q16[DCO_A];
          int32_t dNoteB_q16 = porta_note_stop_q16[DCO_B] - porta_note_start_q16[DCO_B];
          int32_t dNoteC_q16 = porta_note_stop_q16[DCO_C] - porta_note_start_q16[DCO_C];

          int64_t curNoteA_q16 = (int64_t)porta_note_start_q16[DCO_A] + (int64_t)porta_note_step_q16[DCO_A] * (int64_t)elapsedA;
          int64_t curNoteB_q16 = (int64_t)porta_note_start_q16[DCO_B] + (int64_t)porta_note_step_q16[DCO_B] * (int64_t)elapsedB;
          int64_t curNoteC_q16 = (int64_t)porta_note_start_q16[DCO_C] + (int64_t)porta_note_step_q16[DCO_C] * (int64_t)elapsedC;

          if ((dNoteA_q16 >= 0 && curNoteA_q16 >= (int64_t)porta_note_stop_q16[DCO_A]) ||
              (dNoteA_q16 < 0 && curNoteA_q16 <= (int64_t)porta_note_stop_q16[DCO_A])) {
            curNoteA_q16 = porta_note_stop_q16[DCO_A];
          }
          if ((dNoteB_q16 >= 0 && curNoteB_q16 >= (int64_t)porta_note_stop_q16[DCO_B]) ||
              (dNoteB_q16 < 0 && curNoteB_q16 <= (int64_t)porta_note_stop_q16[DCO_B])) {
            curNoteB_q16 = porta_note_stop_q16[DCO_B];
          }
          if ((dNoteC_q16 >= 0 && curNoteC_q16 >= (int64_t)porta_note_stop_q16[DCO_C]) ||
              (dNoteC_q16 < 0 && curNoteC_q16 <= (int64_t)porta_note_stop_q16[DCO_C])) {
            curNoteC_q16 = porta_note_stop_q16[DCO_C];
          }

          porta_note_cur_q16[DCO_A] = (int32_t)curNoteA_q16;
          porta_note_cur_q16[DCO_B] = (int32_t)curNoteB_q16;
          porta_note_cur_q16[DCO_C] = (int32_t)curNoteC_q16;

          // At stop: latched stop Hz. Mid-glide: noteQ16_to_freqQ24 (frac==0 → single table load).
          if (curNoteA_q16 == (int64_t)porta_note_stop_q16[DCO_A]) {
            curA = portamento_stop_q24[DCO_A];
          } else {
            curA = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_A]);
          }
          if (curNoteB_q16 == (int64_t)porta_note_stop_q16[DCO_B]) {
            curB = portamento_stop_q24[DCO_B];
          } else {
            curB = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_B]);
          }
          if (curNoteC_q16 == (int64_t)porta_note_stop_q16[DCO_C]) {
            curC = portamento_stop_q24[DCO_C];
          } else {
            curC = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_C]);
          }
        }

        portamento_cur_freq_q24[DCO_A] = curA;
        portamento_cur_freq_q24[DCO_B] = curB;
        portamento_cur_freq_q24[DCO_C] = curC;
      } else {
        portamento_cur_freq_q24[DCO_A] = sNotePitches_q24[note1];
        portamento_start_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
        portamento_stop_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
        porta_note_cur_q16[DCO_A] = ((int32_t)note1) << 16;
        porta_note_stop_q16[DCO_A] = porta_note_cur_q16[DCO_A];
        porta_note_valid[DCO_A] = true;

        portamento_cur_freq_q24[DCO_B] = sNotePitches_q24[note2];
        portamento_start_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
        portamento_stop_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
        porta_note_cur_q16[DCO_B] = ((int32_t)note2) << 16;
        porta_note_stop_q16[DCO_B] = porta_note_cur_q16[DCO_B];
        porta_note_valid[DCO_B] = true;

        portamento_cur_freq_q24[DCO_C] = sNotePitches_q24[note3];
        portamento_start_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
        portamento_stop_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
        porta_note_cur_q16[DCO_C] = ((int32_t)note3) << 16;
        porta_note_stop_q16[DCO_C] = porta_note_cur_q16[DCO_C];
        porta_note_valid[DCO_C] = true;
      }

#if defined(BENCH_PATH_STATS)
      // Exclusive path tag for jitter attribution (note_on > retime > steady > off).
      if (portaTime == 0) {
        BENCH_PATH_INC(porta_off);
      } else if (note_on_flag_flag[i]) {
        BENCH_PATH_INC(porta_note_on);
      } else if (portaTimeChanged || portaModeChanged || pitchTargetChanged) {
        BENCH_PATH_INC(porta_retime);
      } else if (portaMode == PORTA_MODE_TIME) {
        BENCH_PATH_INC(porta_steady_time);
      } else {
        BENCH_PATH_INC(porta_steady_slew);
      }
#endif
#if DCO_DEBUG_REPORT
      // Debug: OSC1 base frequency after portamento, before modifiers (in Hz)
      dbg_freq_base_Hz = (float)portamento_cur_freq_q24[DCO_A] / (float)(1 << 24);
#endif
      BENCH_END(vt_portamento);
      ////***********************    PORTAMENTO CODE  END    ****************************************/////

      BENCH_BEGIN(vt_adsr_mod);
      // Same as LFO/drift: linear Q15 env × baked depth_q24 (exp is on the knob).
      // ADSR3→pitch select: 0=OSC1, 1=OSC2, 2=OSC1+OSC2, 3=OSC3, 4=all.
      // Mono: one tap. Para/stack: osc k ← ADSR1Level_q15[k].
      int32_t ADSRModifierOSC1_q24 = 0;
      int32_t ADSRModifierOSC2_q24 = 0;
      int32_t ADSRModifierOSC3_q24 = 0;
      if (ADSR1toDETUNE1_scale_q24 != 0) {
        if (voiceMode == 0) {
          const int32_t m = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[i]),
                                          ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1_q24 = m;
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2_q24 = m;
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3_q24 = m;
        } else {
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[0]),
                                                 ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[1]),
                                                 ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[2]),
                                                 ADSR1toDETUNE1_scale_q24);
        }
      }
      BENCH_END(vt_adsr_mod);

      BENCH_BEGIN(vt_unison_mod);
      // Fixed-point unison modifier in Q24: 0.0001 * unisonDetune * step
      // unisonDetune is uint8_t; product fits int32 (no int64 on M0+).
      static constexpr int32_t UNISON_SCALE_Q24 = (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
      // Per-osc spread (matches float path): OSC1=0, OSC2=+1, OSC3=-1.
      static constexpr int32_t OSC_UNISON_STEP[3] = { 0, 1, -1 };
      const int32_t unisonBase = (int32_t)unisonDetune * UNISON_SCALE_Q24;
      int32_t unisonMODIFIER_OSC1_q24 = unisonBase * OSC_UNISON_STEP[0];
      int32_t unisonMODIFIER_OSC2_q24 = unisonBase * OSC_UNISON_STEP[1];
      int32_t unisonMODIFIER_OSC3_q24 = unisonBase * OSC_UNISON_STEP[2];
#if NUM_VOICES_TOTAL > 1
      // Classic poly voice-index alternating pattern on top of per-osc spread.
      int32_t voiceMag = (i >> 1) + 1;
      int32_t voiceSign = ((i & 0x01) == 0) ? 1 : -1;
      int32_t voiceUnison_q24 = unisonBase * (voiceSign * voiceMag);
      unisonMODIFIER_OSC1_q24 += voiceUnison_q24;
      unisonMODIFIER_OSC2_q24 += voiceUnison_q24;
      unisonMODIFIER_OSC3_q24 += voiceUnison_q24;
#endif
      BENCH_END(vt_unison_mod);

      BENCH_BEGIN(vt_drift_mod);
      // Snapshot Core 0 mailbox once, then applyDepthQ24 on locals (no volatile-in-mul).
      const int32_t driftScale_q24 = drift_pitch_scale_q24;
      const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
      const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
      const int16_t driftC = LFO_DRIFT_LEVEL[DCO_C];
      int32_t DETUNE_DRIFT_OSC1_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftA, driftScale_q24) : 0;
      int32_t DETUNE_DRIFT_OSC2_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftB, driftScale_q24) : 0;
      int32_t DETUNE_DRIFT_OSC3_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftC, driftScale_q24) : 0;
      BENCH_END(vt_drift_mod);

      int32_t modifiersBase_q24;
      int32_t freqModifiers_q24;
      int32_t freq2Modifiers_q24;
      int32_t freq3Modifiers_q24;
      {
        // Snapshot LFO pitch mailbox here only (short live range; not across clkdiv/amp/RANGE).
        const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
        const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
        const int32_t local_lfo1_osc3 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC3];
        const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
        const int32_t local_lfo2_osc3 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC3];
        BENCH_BEGIN(vt_modifiers);
        // Combine modifiers in Q24 (faithful to original float path)
        // Unison is applied per-osc below; shared part is pitchbend + epsilon only (LFO1 per osc).
        // Character pitch jitter stacks after matrix when scale nonzero.
        // Q24 octave-fraction sums fit int32 for normal depths (no int64).
        modifiersBase_q24 =
          calcPitchbend_q24 + Q24_ONE_EPS + matrix_pitch_mod_q24;
        if (char_pitch_scale_q15) {
          modifiersBase_q24 += character_pitch_delta_q24();
        }
        freqModifiers_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + unisonMODIFIER_OSC1_q24 + local_lfo1_osc1;
        freq2Modifiers_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + unisonMODIFIER_OSC2_q24 + local_lfo1_osc2 + local_lfo2_osc2;
        freq3Modifiers_q24 = ADSRModifierOSC3_q24 + DETUNE_DRIFT_OSC3_q24 + modifiersBase_q24 + unisonMODIFIER_OSC3_q24 + local_lfo1_osc3 + local_lfo2_osc3;
        BENCH_END(vt_modifiers);
      }

      BENCH_BEGIN(vt_freq_scale_x);


      // Fast fixed-point equivalent of:
      //   freq  *= interpolatePitchMultiplier(freqModifiers)/multiplierTableScale;
      //   freq2 *= OSC2_detune * interpolatePitchMultiplier(freq2Modifiers)/multiplierTableScale;
      //   freq3 *= OSC3_detune * interpolatePitchMultiplier(freq3Modifiers)/multiplierTableScale;
      int32_t xScaled1_Q16 = modifiers_q24_to_xQ16(freqModifiers_q24);
      int32_t xScaled2_Q16 = modifiers_q24_to_xQ16(freq2Modifiers_q24);
      int32_t xScaled3_Q16 = modifiers_q24_to_xQ16(freq3Modifiers_q24);
      BENCH_END(vt_freq_scale_x);

      BENCH_BEGIN(vt_ratio_interp);
      int32_t ratio1_Q16 = interpolate_live_ratio_q16(xScaled1_Q16, DCO_A);
      int32_t ratio2_Q16 = interpolate_live_ratio_q16(xScaled2_Q16, DCO_B);
      int32_t ratio3_Q16 = interpolate_live_ratio_q16(xScaled3_Q16, DCO_C);
      BENCH_END(vt_ratio_interp);

      BENCH_BEGIN(vt_freq_scale_post);
      freq_q24_A = (portamento_cur_freq_q24[DCO_A] * (int64_t)ratio1_Q16) >> 16;
      // Combine OSC2 ratio with detune into one Q16 factor
      // detune_Q16 = round(detune_q24 / 2^8)
      int32_t detune_Q16 = (int32_t)((((int64_t)detune_q24) + 128) >> 8);
      // combined_Q16 = round((ratio2_Q16 * detune_Q16) / 2^16)
      int32_t combined_Q16 = (int32_t)((((int64_t)ratio2_Q16 * (int64_t)detune_Q16) + (1LL << 15)) >> 16);
      freq_q24_B = (portamento_cur_freq_q24[DCO_B] * (int64_t)combined_Q16) >> 16;
      // Combine OSC3 ratio with detune into one Q16 factor
      int32_t detune3_Q16 = (int32_t)((((int64_t)detune3_q24) + 128) >> 8);
      int32_t combined3_Q16 = (int32_t)((((int64_t)ratio3_Q16 * (int64_t)detune3_Q16) + (1LL << 15)) >> 16);
      freq_q24_C = (portamento_cur_freq_q24[DCO_C] * (int64_t)combined3_Q16) >> 16;

#if DCO_DEBUG_REPORT
      // Debug: OSC1 frequency after all modifiers applied (in Hz)
      dbg_freq_after_mod_Hz = (float)freq_q24_A / (float)(1 << 24);
#endif


      BENCH_END(vt_freq_scale_post);

      // freq→divider inputs + dividers (was unattributed between post and clkdiv).
      BENCH_BEGIN(vt_clk_div);

      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t smAN = VOICE_TO_SM[DCO_A];
      uint8_t smBN = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      register uint32_t clk_div1, clk_div2, clk_div3;

      uint8_t arbitrary_measured_correction_value = 0; // 60 is a measured correction for the PIO

      uint32_t total_cycles1, total_cycles2, total_cycles3;
      // One load of cached clk_sys (see sys_clock_hz_refresh); never call clock_get_hz here.
      const uint32_t sys_hz = sysClock_Hz;

      // Period model per oscillator: period = Y + weight*clk_div + overhead.
      const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
      const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);
      const uint32_t wC = osc_ramp_weight(DCO_C), kC = osc_period_overhead(DCO_C);
      const uint32_t yA = osc_last_y[DCO_A];
      const uint32_t yB = osc_last_y[DCO_B];
      const uint32_t yC = osc_last_y[DCO_C];

      total_cycles1 = clkdiv_live_total_cycles(sys_hz, freq_q24_A);
      total_cycles2 = clkdiv_live_total_cycles(sys_hz, freq_q24_B);
      total_cycles3 = clkdiv_live_total_cycles(sys_hz, freq_q24_C);
      total_cycles1 += arbitrary_measured_correction_value;
      total_cycles2 += arbitrary_measured_correction_value;
      total_cycles3 += arbitrary_measured_correction_value;
      clk_div1 = pio_clk_div_for_y(total_cycles1, yA, wA, kA);
      clk_div2 = pio_clk_div_for_y(total_cycles2, yB, wB, kB);
      clk_div3 = pio_clk_div_for_y(total_cycles3, yC, wC, kC);
      BENCH_END(vt_clk_div);

      // Measured apart from the divider math because it only runs with sync on and a
      // non-zero phase offset; averaging the two together hides both.
      // Phase align + exact split: note-on only (I-cache next to each other).
      uint32_t phaseHoldX = 0;
      PioPeriod retrig_p1{};
      PioPeriod retrig_p2{};
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync > 1 && phaseAlignOSC2 != 0) {
        BENCH_BEGIN(vt_phase_align);
        phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
        BENCH_END(vt_phase_align);
      }
      // Mono EXACT_Y A+B stash only. Para splits inside its per-osc note-on loop.
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync >= 1 &&
          note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
        BENCH_BEGIN(vt_retrig_split);
        retrig_p1 = pio_period_split(total_cycles1, wA, kA);
        retrig_p2 = pio_period_split(total_cycles2, wB, kB);
        BENCH_END(vt_retrig_split);
      }

      BENCH_BEGIN(vt_chan_level);
      uint16_t chanLevel, chanLevel2, chanLevel3;
      amp_chan_levels_fixed(freq_q24_A, freq_q24_B, freq_q24_C, DCO_A, DCO_B, DCO_C,
                            &chanLevel, &chanLevel2, &chanLevel3);
      BENCH_END(vt_chan_level);

      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      osc_last_clk_div[DCO_C] = clk_div3;
      BENCH_END(vt_pio_write);

      // Mono: A+B oscSync retrig + RANGE A/B/C. Para: osc k only, no oscSync.
      if (voiceMode == 0) {
      if (note_on_flag_flag[i]) {
        BENCH_BEGIN(vt_note_retrig);
        // --- Reverse Calculation to find the expected output frequency ---
        uint32_t actual_total_osr_val = clk_div1 * wA;
        uint32_t actual_total_period = osc_last_y[DCO_A] + actual_total_osr_val + kA;
        float expected_freq = (double)sysClock_Hz / (double)actual_total_period;

#if DCO_DEBUG_REPORT
        // Exact split for the report only (live path splits inside oscSync below).
        PioPeriod p1dbg = pio_period_split(total_cycles1, wA, kA);
        // --- Print Diagnostic Report ---
        Serial.println("----------------[ DCO DEBUG REPORT ]----------------");
        Serial.printf("Target Freq In:   %.2f Hz\n", (float)freq_q24_A / (float)(1 << 24));
        Serial.printf("Total Cycles Calc:  %lu (Target for the whole period)\n", total_cycles1);
        Serial.printf("Reset pulse (Y):    %lu cycles (incl. period remainder)\n", p1dbg.y);
        Serial.printf("Period Overhead:    %lu cycles (program constant)\n", kA);
        Serial.printf("Total OSR Delay:    %lu cycles (Remaining for loops)\n", p1dbg.clk_div * wA);
        Serial.printf("clk_div (Exact):    %lu (Value sent to PIO)\n", p1dbg.clk_div);
        Serial.println("---");
        Serial.printf("Actual Period Gen:  %lu cycles (Y + (clk_div*%u) + overhead)\n",
                      actual_total_period, (unsigned)wA);
        Serial.printf("==> Expected Freq Out: %.2f Hz\n", expected_freq);
        Serial.println("---");

        Serial.println("OSC1 Frequency Stages:");
        Serial.printf("  Base after portamento:     %.4f Hz\n", dbg_freq_base_Hz);
        Serial.printf("  After modifiers (Q24):     %.4f Hz\n", dbg_freq_after_mod_Hz);
        Serial.printf("  Quantized by PIO (clkdiv): %.4f Hz\n", expected_freq);
        Serial.println("---");

        Serial.println("OSC1 Modifier Breakdown (Q24/Q16):");
        Serial.printf("  ADSRModifierOSC1_q24:      %.6f\n", (double)ADSRModifierOSC1_q24 / (double)(1 << 24));
        Serial.printf("  DETUNE_DRIFT_OSC1_q24:     %.6f\n", (double)DETUNE_DRIFT_OSC1_q24 / (double)(1 << 24));
        Serial.printf("  lfo1_pitch_mod OSC1:      %.6f\n", (double)lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] / (double)(1 << 24));
        Serial.printf("  unisonMODIFIER_q24:        %.6f\n", (double)unisonMODIFIER_OSC1_q24 / (double)(1 << 24));
        Serial.printf("  pitchbend_q24:             %.6f\n", (double)calcPitchbend_q24 / (double)(1 << 24));
        Serial.printf("  Q24_ONE_EPS:               %.6f\n", (double)Q24_ONE_EPS / (double)(1 << 24));
        Serial.printf("  modifiersBase_q24:         %.6f\n", (double)modifiersBase_q24 / (double)(1 << 24));
        Serial.println("OSC2 LFO2 pitch (fine + coarse):");
        Serial.printf("  lfo2_fine OSC2:            %.6f\n",
                      (double)applyDepthQ24(LFO2Level, LFO2toOSC2_q24) / (double)(1 << 24));
        Serial.printf("  lfo2_coarse OSC2:          %.6f\n",
                      (double)applyDepthQ24(LFO2Level, LFO2toOSC2_coarse_q24) / (double)(1 << 24));
        Serial.printf("  freq2Modifiers_q24:        %.6f\n", (double)freq2Modifiers_q24 / (double)(1 << 24));
        Serial.println("---");

        Serial.println("OSC1 Multiplier Table Inputs:");
        Serial.printf("  xScaled1_Q16 (natural):    %.6f\n", (double)xScaled1_Q16 / 65536.0);
        Serial.printf("  ratio1_Q16:                %.6f\n", (double)ratio1_Q16 / (double)(1 << 16));
        Serial.println("----------------------------------------------------\n");

#endif

        if (oscSync >= 1) {
          // One probe for the whole SM apply path — fine MAIN slices mis-attribute cold XIP.
          BENCH_BEGIN(vt_retrig_sm_apply);
          if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
            // Exact split was computed after phase_align (retrig_p1 / retrig_p2).
            // OSC3 is deliberately absent: never retriggered; Y stays at pioPulseLength.
            uint32_t maskAB = (1u << smAN) | (1u << smBN);
            pio_set_sm_mask_enabled(pio[0], maskAB, false);

            // Note-on is the one point where Y can be rewritten safely. Frame already did
            // put+pull, so TX is empty — fused noclear load (no FJOIN).
            osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
                                             DCO_B, retrig_p2.y, retrig_p2.clk_div);

            pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));

            if (phaseHoldX != 0) {
              osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
            } else {
              pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
            }

            pio_enable_sm_mask_in_sync(pio[0], maskAB);
          } else {
            // SYNC_JMP: phase resync only — no disable / Y load / enable_in_sync.
            // X preload needs a stopped SM, so degree offsets require EXACT_Y.
            pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));
            pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
          }
          BENCH_END(vt_retrig_sm_apply);
        }

        BENCH_BEGIN(vt_retrig_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_retrig_pwm);
        BENCH_END(vt_note_retrig);
      }

      } else {
        // Paraphonic: edge k → RANGE osc k only (same as mono with oscSync off).
        // No SM stop/restart — that phase-resets and clicks when porta keeps the amp open.
        // Pitch continues via the per-frame put/pull above. No oscSync / phase-align.
        uint16_t chanLevels[3];
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          chanLevels[0] = character_clamp_amp((int32_t)chanLevel + amp_j);
          chanLevels[1] = character_clamp_amp((int32_t)chanLevel2 + amp_j);
          chanLevels[2] = character_clamp_amp((int32_t)chanLevel3 + amp_j);
        } else {
          chanLevels[0] = chanLevel;
          chanLevels[1] = chanLevel2;
          chanLevels[2] = chanLevel3;
        }

        for (int k = 0; k < NUM_VOICES; k++) {
          if (!note_on_flag_flag[k]) continue;
          BENCH_BEGIN(vt_note_retrig);
          const uint8_t osc = (uint8_t)k;
          BENCH_BEGIN(vt_retrig_pwm);
          write_range_pwm(osc, chanLevels[k]);
          BENCH_END(vt_retrig_pwm);
          BENCH_END(vt_note_retrig);
        }
      }

      if (timer99microsFlag2) {
        BENCH_BEGIN(vt_range_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_range_pwm);

        // Shared PW PWM when any oscillator has analog Pulse enabled (DG411).
        const bool pulseOn = waveEnable[0][1] || waveEnable[1][1] || waveEnable[2][1];
        if (pulseOn) {
          const int16_t local_LFO2Level = LFO2Level;
          const int16_t local_LFO2toPW = LFO2toPW;
          BENCH_FBEGIN(vt_pwm_calc);
          // Optimized: This version avoids storing large intermediate products.
          // The multiplication and shift are combined into one expression per modulator,
          // allowing the compiler to make better use of registers.
          // int32: |q15|*|scale| and |LFO|*|LFO2toPW| fit before >> 15 (no int64 on M0+).
          int32_t adsr1_delta =
            ((int32_t)ADSR1Level_q15[i] * ADSR1toPWM_scale) >> 15;
          int32_t lfo2_delta =
            ((int32_t)local_LFO2Level * (int32_t)local_LFO2toPW) >> 15;
          int32_t pw_calc = (int32_t)DIV_COUNTER_PW - 1 - lfo2_delta - PW[0] + adsr1_delta
                            + character_pw_delta();

          if (pw_calc < 0) pw_calc = 0;
          if (pw_calc > (int32_t)DIV_COUNTER_PW - 1) pw_calc = (int32_t)DIV_COUNTER_PW - 1;
          PW_PWM[i] = (uint16_t)pw_calc;
          BENCH_FEND(vt_pwm_calc);

          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), get_PW_level_interpolated(PW_PWM[i], i));
          BENCH_END(vt_pw_update);

        } else {
          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
          BENCH_END(vt_pw_update);
        }
      }
  }

  for (int k = 0; k < NUM_VOICES; k++) {
    note_on_flag_flag[k] = false;
  }

  // Whole-task timing is taken by the BENCH_voice_task probe around voice_task_main() in
  // loop1(), so both engines are measured at the same boundary.

  // Update cached portamento parameters for next call
  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}

#endif  // !USE_FLOAT_VOICE_TASK

// Dispatch entry point: select float vs fixed-point implementation at compile time.
inline void voice_task_main() {
#ifdef USE_FLOAT_VOICE_TASK
  voice_task_float();
#else
  voice_task_fixed_point();
#endif
}

#ifdef USE_FLOAT_VOICE_TASK
// Float realtime voice engine (same stages as voice_task_fixed_point, in Hz). Board default on RP2350.
void __not_in_flash_func(voice_task_float)() {
    // --- Track portamento control changes exactly as in original ---
    static uint32_t last_portamento_time = 0;
    static uint8_t  last_portamento_mode = PORTA_MODE_SLEW;
    static uint8_t lastNote1 = 0, lastNote2 = 0, lastNote3 = 0;
    uint32_t portaTime = portamento_time;
    uint8_t  portaMode = portamento_mode;
    bool portaTimeChanged = (portaTime != last_portamento_time);
    bool portaModeChanged = (portaMode != last_portamento_mode);
  
    // --- 1. Pitch bend as float, equivalent to Q24 math ---
    BENCH_BEGIN(vt_pitchbend);
    // Use original float pitch bend behaviour, but derive multiplier from Q24
    float pitchBendMultiplier = q24_to_float(pitchBendMultiplier_q24);
    float calcPitchbend;

    if (midi_pitch_bend == 8192) {
      calcPitchbend = 0.0f;
    } else if (midi_pitch_bend < 8192) {
      calcPitchbend = (((float)midi_pitch_bend / 8190.99f) - 1.0f) * pitchBendMultiplier;
    } else {  // midi_pitch_bend > 8192
      calcPitchbend = (((float)midi_pitch_bend / 8192.99f) - 1.0f) * pitchBendMultiplier;
    }
    BENCH_END(vt_pitchbend);

    last_midi_pitch_bend = midi_pitch_bend;

    // Latch note-on edges for every voice slot (para may set flags on 1/2).
    for (int k = 0; k < NUM_VOICES; k++) {
      if (note_on_flag[k] == 1) {
        note_on_flag_flag[k] = true;
        note_on_flag[k] = 0;
      }
    }
  
    // --- 2. Per-voice loop (mirror original structure) ---
    for (int i = 0; i < NUM_VOICES_VOICE_TASK; ++i) {
  
  #if DCO_DEBUG_REPORT
      float dbg_freq_base_Hz      = 0.0f;
      float dbg_freq_after_mod_Hz = 0.0f;
  #endif

      // note_off keeps VOICE_NOTES (release pitch); free/steal uses VOICES[].
      // Never-played slots stay 0 — gate midi→table math (uint8 underflow → shriek).

      // --- 2.1 Note indices ---
      // Note index convention: table_index = midi - 36 + offset (36 ⇒ unison).
      //   Mono: octave_shift = global octave; OSC2/3 = relative to note1 (36 ⇒ unison with OSC1).
      //   Poly/stack: every osc uses octave_shift only (shared octave); not OSC2/3.
      const size_t NOTE_TABLE_LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
      uint8_t note1, note2, note3;
      if (voiceMode == 0) {
        const uint8_t vn = VOICE_NOTES[i];
        if (vn == 0) {
          note1 = note2 = note3 = 0;
        } else {
          note1 = midi_offset_to_table_index((int)vn, (int)octave_shift, NOTE_TABLE_LEN);
          note2 = midi_offset_to_table_index((int)note1, (int)OSC2_interval, NOTE_TABLE_LEN);
          note3 = midi_offset_to_table_index((int)note1, (int)OSC3_interval, NOTE_TABLE_LEN);
        }
      } else {
        const uint8_t vn0 = VOICE_NOTES[0];
        const uint8_t vn1 = VOICE_NOTES[1];
        const uint8_t vn2 = VOICE_NOTES[2];
        note1 = (vn0 == 0) ? 0 : midi_offset_to_table_index((int)vn0, (int)octave_shift, NOTE_TABLE_LEN);
        note2 = (vn1 == 0) ? 0 : midi_offset_to_table_index((int)vn1, (int)octave_shift, NOTE_TABLE_LEN);
        note3 = (vn2 == 0) ? 0 : midi_offset_to_table_index((int)vn2, (int)octave_shift, NOTE_TABLE_LEN);
      }

      const bool pitchTargetChanged =
          note1 != lastNote1 || note2 != lastNote2 || note3 != lastNote3;
      lastNote1 = note1;
      lastNote2 = note2;
      lastNote3 = note3;
  
      BENCH_BEGIN(vt_osc_detune);
      // --- 2.2 OSC2/OSC3 detune (float equivalent of Q24) ---
      float detuneSteps = (float)((int)256 - OSC2DetuneVal);
      float osc2DetuneRatio = 1.0f + 0.0002f * detuneSteps;
      float detune3Steps = (float)((int)256 - OSC3DetuneVal);
      float osc3DetuneRatio = 1.0f + 0.0002f * detune3Steps;
      BENCH_END(vt_osc_detune);
  
      // base note frequencies from float table
      float noteFreq1 = sNotePitches[note1];
      float noteFreq2 = sNotePitches[note2];
      float noteFreq3 = sNotePitches[note3];
  
      float freqA, freqB, freqC;      // will hold the portamento-processed base freqs
  
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;
  
      // --- 2.3 Portamento (time mode & slew mode), float-only implementation ---
      BENCH_BEGIN(vt_portamento);

      if (portaTime > 0) {
        uint32_t now_us = micros();
        if (voiceMode == 0) {
          portamentoTimer[i] = now_us - portamentoStartMicros[i];
        } else {
          for (int k = 0; k < NUM_VOICES; k++) {
            portamentoTimer[k] = now_us - portamentoStartMicros[k];
          }
        }

        // Mono: one edge restarts all three oscs. Para: each note_on_flag_flag[k] restarts osc k only.
        // TIME = fixed duration any interval; SLEW = constant semitone rate (time ∝ interval).
        if (voiceMode == 0) {
        if (note_on_flag_flag[i]) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i]       = 0;

          float targetNoteA = (float)note1;
          float targetNoteB = (float)note2;
          float targetNoteC = (float)note3;
          porta_setup_glide_f(DCO_A, porta_resolve_start_note_f(DCO_A, targetNoteA),
                              targetNoteA, portaMode);
          porta_setup_glide_f(DCO_B, porta_resolve_start_note_f(DCO_B, targetNoteB),
                              targetNoteB, portaMode);
          porta_setup_glide_f(DCO_C, porta_resolve_start_note_f(DCO_C, targetNoteC),
                              targetNoteC, portaMode);
        }
        } else {
          // Paraphonic: restart porta per voice slot → osc k (1:1).
          const int targetNotes[3] = { note1, note2, note3 };
          for (int k = 0; k < NUM_VOICES; k++) {
            if (!note_on_flag_flag[k]) continue;
            const uint8_t osc = (uint8_t)k;
            portamentoStartMicros[k] = now_us;
            portamentoTimer[k] = 0;
            float targetNote = (float)targetNotes[k];
            porta_setup_glide_f(osc, porta_resolve_start_note_f(osc, targetNote),
                                targetNote, portaMode);
          }
        }

        // Control-change frames: skip steady (stale start/step + huge elapsed → pitch jump).
        const bool portaNoteOnEdge =
            (voiceMode == 0) ? note_on_flag_flag[i]
                             : (note_on_flag_flag[0] || note_on_flag_flag[1] || note_on_flag_flag[2]);
        const bool portaDoRetime =
            (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !portaNoteOnEdge;

        float curA, curB, curC;
        if (portaDoRetime) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i]       = 0;
          if (voiceMode != 0) {
            for (int k = 0; k < NUM_VOICES; k++) {
              portamentoStartMicros[k] = now_us;
              portamentoTimer[k]       = 0;
            }
          }

          float curNoteA = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_A]);
          float curNoteB = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_B]);
          float curNoteC = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_C]);
          porta_setup_glide_f(DCO_A, curNoteA, (float)note1, portaMode);
          porta_setup_glide_f(DCO_B, curNoteB, (float)note2, portaMode);
          porta_setup_glide_f(DCO_C, curNoteC, (float)note3, portaMode);
          curA = porta_freq_cur_f[DCO_A];
          curB = porta_freq_cur_f[DCO_B];
          curC = porta_freq_cur_f[DCO_C];
        } else if (porta_note_cur_f[DCO_A] == porta_note_stop_f[DCO_A] &&
                   porta_note_cur_f[DCO_B] == porta_note_stop_f[DCO_B] &&
                   porta_note_cur_f[DCO_C] == porta_note_stop_f[DCO_C]) {
          curA = porta_freq_stop_f[DCO_A];
          curB = porta_freq_stop_f[DCO_B];
          curC = porta_freq_stop_f[DCO_C];
        } else {
          int32_t elapsedA = (voiceMode == 0) ? (int32_t)portamentoTimer[i] : (int32_t)portamentoTimer[0];
          int32_t elapsedB = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[1];
          int32_t elapsedC = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[2];

          float startNoteA = porta_note_start_f[DCO_A];
          float startNoteB = porta_note_start_f[DCO_B];
          float startNoteC = porta_note_start_f[DCO_C];
          float stopNoteA  = porta_note_stop_f [DCO_A];
          float stopNoteB  = porta_note_stop_f [DCO_B];
          float stopNoteC  = porta_note_stop_f [DCO_C];

          float dNoteA = stopNoteA - startNoteA;
          float dNoteB = stopNoteB - startNoteB;
          float dNoteC = stopNoteC - startNoteC;

          float curNoteA = startNoteA + porta_note_step_f[DCO_A] * (float)elapsedA;
          float curNoteB = startNoteB + porta_note_step_f[DCO_B] * (float)elapsedB;
          float curNoteC = startNoteC + porta_note_step_f[DCO_C] * (float)elapsedC;

          if ((dNoteA >= 0.0f && curNoteA >= stopNoteA) ||
              (dNoteA <  0.0f && curNoteA <= stopNoteA)) {
            curNoteA = stopNoteA;
          }
          if ((dNoteB >= 0.0f && curNoteB >= stopNoteB) ||
              (dNoteB <  0.0f && curNoteB <= stopNoteB)) {
            curNoteB = stopNoteB;
          }
          if ((dNoteC >= 0.0f && curNoteC >= stopNoteC) ||
              (dNoteC <  0.0f && curNoteC <= stopNoteC)) {
            curNoteC = stopNoteC;
          }

          porta_note_cur_f[DCO_A] = curNoteA;
          porta_note_cur_f[DCO_B] = curNoteB;
          porta_note_cur_f[DCO_C] = curNoteC;

          // At stop: use stop Hz (never live noteFreq* — that raced with note_on_flag).
          curA = (curNoteA == stopNoteA) ? porta_freq_stop_f[DCO_A]
                                         : noteIndex_to_freqFloat(curNoteA);
          curB = (curNoteB == stopNoteB) ? porta_freq_stop_f[DCO_B]
                                         : noteIndex_to_freqFloat(curNoteB);
          curC = (curNoteC == stopNoteC) ? porta_freq_stop_f[DCO_C]
                                         : noteIndex_to_freqFloat(curNoteC);

          porta_freq_cur_f[DCO_A] = curA;
          porta_freq_cur_f[DCO_B] = curB;
          porta_freq_cur_f[DCO_C] = curC;
        }

        freqA = curA;
        freqB = curB;
        freqC = curC;

      } else {
        // No portamento
        freqA = noteFreq1;
        freqB = noteFreq2;
        freqC = noteFreq3;

        // Keep float portamento state coherent when portamento is off.
        porta_freq_cur_f[DCO_A] = freqA;
        porta_freq_cur_f[DCO_B] = freqB;
        porta_freq_cur_f[DCO_C] = freqC;
        porta_freq_stop_f[DCO_A] = freqA;
        porta_freq_stop_f[DCO_B] = freqB;
        porta_freq_stop_f[DCO_C] = freqC;
        porta_note_cur_f[DCO_A] = (float)note1;
        porta_note_cur_f[DCO_B] = (float)note2;
        porta_note_cur_f[DCO_C] = (float)note3;
        porta_note_stop_f[DCO_A] = (float)note1;
        porta_note_stop_f[DCO_B] = (float)note2;
        porta_note_stop_f[DCO_C] = (float)note3;
        porta_note_valid[DCO_A] = true;
        porta_note_valid[DCO_B] = true;
        porta_note_valid[DCO_C] = true;
      }

#if defined(BENCH_PATH_STATS)
      // Exclusive path tag for jitter attribution (note_on > retime > steady > off).
      if (portaTime == 0) {
        BENCH_PATH_INC(porta_off);
      } else if (note_on_flag_flag[i]) {
        BENCH_PATH_INC(porta_note_on);
      } else if (portaTimeChanged || portaModeChanged || pitchTargetChanged) {
        BENCH_PATH_INC(porta_retime);
      } else if (portaMode == PORTA_MODE_TIME) {
        BENCH_PATH_INC(porta_steady_time);
      } else {
        BENCH_PATH_INC(porta_steady_slew);
      }
#endif

  #if DCO_DEBUG_REPORT
      dbg_freq_base_Hz = freqA;
  #endif
  
      BENCH_END(vt_portamento);

      // --- 2.4 ADSR detune (float equivalent of Q24) ---
      BENCH_BEGIN(vt_adsr_mod);
      float ADSRModifierOSC1 = 0.0f;
      float ADSRModifierOSC2 = 0.0f;
      float ADSRModifierOSC3 = 0.0f;
      if (ADSR1toDETUNE1_scale_q24 != 0) {
        if (voiceMode == 0) {
          const float m = q24_to_float(applyDepthQ24(
              env_dco_pitch_wave_q15(ADSR1Level_q15[i]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1 = m;
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2 = m;
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3 = m;
        } else {
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[0]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[1]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[2]), ADSR1toDETUNE1_scale_q24));
        }
      }
      BENCH_END(vt_adsr_mod);

      BENCH_BEGIN(vt_unison_mod);
      // --- 2.5 Unison modifier (float equivalent of Q24 0.0001 * unisonDetune * step) ---
      static constexpr float UNISON_SCALE = 0.0001f;
      static constexpr int32_t OSC_UNISON_STEP[3] = { 0, 1, -1 };
      const float unisonBase = (float)unisonDetune * UNISON_SCALE;
      float unisonMODIFIER_OSC1 = unisonBase * (float)OSC_UNISON_STEP[0];
      float unisonMODIFIER_OSC2 = unisonBase * (float)OSC_UNISON_STEP[1];
      float unisonMODIFIER_OSC3 = unisonBase * (float)OSC_UNISON_STEP[2];
#if NUM_VOICES_TOTAL > 1
      // Classic poly voice-index alternating pattern on top of per-osc spread.
      float voiceMag = (float)((i >> 1) + 1);
      float voiceSign = ((i & 0x01) == 0) ? 1.0f : -1.0f;
      float voiceUnison = unisonBase * (voiceSign * voiceMag);
      unisonMODIFIER_OSC1 += voiceUnison;
      unisonMODIFIER_OSC2 += voiceUnison;
      unisonMODIFIER_OSC3 += voiceUnison;
#endif
      BENCH_END(vt_unison_mod);

      BENCH_BEGIN(vt_drift_mod);
      // Snapshot Core 0 mailbox once, then applyDepthQ24 (same as fixed / ADSR detune).
      const int32_t driftScale_q24 = drift_pitch_scale_q24;
      const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
      const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
      const int16_t driftC = LFO_DRIFT_LEVEL[DCO_C];
      float DETUNE_DRIFT_OSC1 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftA, driftScale_q24)) : 0.0f;
      float DETUNE_DRIFT_OSC2 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftB, driftScale_q24)) : 0.0f;
      float DETUNE_DRIFT_OSC3 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftC, driftScale_q24)) : 0.0f;
      BENCH_END(vt_drift_mod);

      float freqModifiers1;
      float freqModifiers2;
      float freqModifiers3;
      {
        // Snapshot LFO pitch mailbox here only (short live range; same as fixed).
        const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
        const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
        const int32_t local_lfo1_osc3 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC3];
        const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
        const int32_t local_lfo2_osc3 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC3];
        BENCH_BEGIN(vt_modifiers);
        float lfo1_osc1        = q24_to_float(local_lfo1_osc1);
        float lfo1_osc2        = q24_to_float(local_lfo1_osc2);
        float lfo1_osc3        = q24_to_float(local_lfo1_osc3);
        float lfo2_osc2        = q24_to_float(local_lfo2_osc2);
        float lfo2_osc3        = q24_to_float(local_lfo2_osc3);
        float eps              = q24_to_float(Q24_ONE_EPS);
        float pitchBendF   = calcPitchbend;
  
        // Character pitch jitter stacks after matrix when scale nonzero.
        float modifiersBase = pitchBendF + eps + q24_to_float(matrix_pitch_mod_q24);
        if (char_pitch_scale_q15) {
          modifiersBase += q24_to_float(character_pitch_delta_q24());
        }
        freqModifiers1 = ADSRModifierOSC1 + DETUNE_DRIFT_OSC1 + modifiersBase + unisonMODIFIER_OSC1 + lfo1_osc1;
        freqModifiers2 = ADSRModifierOSC2 + DETUNE_DRIFT_OSC2 + modifiersBase + unisonMODIFIER_OSC2 + lfo1_osc2 + lfo2_osc2;
        freqModifiers3 = ADSRModifierOSC3 + DETUNE_DRIFT_OSC3 + modifiersBase + unisonMODIFIER_OSC3 + lfo1_osc3 + lfo2_osc3;
        BENCH_END(vt_modifiers);
      }

      BENCH_BEGIN(vt_freq_scale_x);
      // Q12 ×10000 glue (if any) lives inside interpolate_live_ratio_f.
      BENCH_END(vt_freq_scale_x);

      BENCH_BEGIN(vt_ratio_interp);
      float ratio1 = interpolate_live_ratio_f(freqModifiers1, DCO_A);
      float ratio2 = interpolate_live_ratio_f(freqModifiers2, DCO_B);
      float ratio3 = interpolate_live_ratio_f(freqModifiers3, DCO_C);
      BENCH_END(vt_ratio_interp);

      BENCH_BEGIN(vt_freq_scale_post);
      // Apply ratios to portamento frequencies, with osc2/osc3 detune (Hz domain).
      float freqA_Hz = freqA * ratio1;
      float freqB_Hz = freqB * (ratio2 * osc2DetuneRatio);
      float freqC_Hz = freqC * (ratio3 * osc3DetuneRatio);

  #if DCO_DEBUG_REPORT
      dbg_freq_after_mod_Hz = freqA_Hz;
  #endif

      BENCH_END(vt_freq_scale_post);

      // --- 2.8 Clock divider calculation (float equivalent) ---
      BENCH_BEGIN(vt_clk_div);

      float correction = 0.0f;   // keep your measured correction if needed
      const uint32_t sys_hz = sysClock_Hz;
      uint32_t total_cycles1 = clkdiv_live_hz_total_cycles(sys_hz, freqA_Hz)
                               + (uint32_t)correction;
      uint32_t total_cycles2 = clkdiv_live_hz_total_cycles(sys_hz, freqB_Hz)
                               + (uint32_t)correction;
      uint32_t total_cycles3 = clkdiv_live_hz_total_cycles(sys_hz, freqC_Hz)
                               + (uint32_t)correction;

      const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
      const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);
      const uint32_t wC = osc_ramp_weight(DCO_C), kC = osc_period_overhead(DCO_C);

      uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
      uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);
      uint32_t clk_div3 = pio_clk_div_for_y(total_cycles3, osc_last_y[DCO_C], wC, kC);
      BENCH_END(vt_clk_div);

      // Measured apart from the divider math because it only runs with sync on and a
      // non-zero phase offset; averaging the two together hides both.
      // Phase align + exact split: note-on only (I-cache next to each other).
      uint32_t phaseHoldX = 0;
      PioPeriod retrig_p1{};
      PioPeriod retrig_p2{};
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync > 1 && phaseAlignOSC2 != 0) {
        BENCH_BEGIN(vt_phase_align);
        phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
        BENCH_END(vt_phase_align);
      }
      // Mono EXACT_Y A+B stash only. Para splits inside its per-osc note-on loop.
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync >= 1 &&
          note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
        BENCH_BEGIN(vt_retrig_split);
        retrig_p1 = pio_period_split(total_cycles1, wA, kA);
        retrig_p2 = pio_period_split(total_cycles2, wB, kB);
        BENCH_END(vt_retrig_split);
      }

      // --- 2.9 Amplitude compensation using engine-agnostic helper ---
      BENCH_BEGIN(vt_chan_level);

      uint16_t chanLevel, chanLevel2, chanLevel3;
      switch (syncMode) {
        case 0:
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        case 1: {
          float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
          chanLevel  = get_chan_level_for_engine(maxFreq, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        }
        case 2: {
          float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(maxFreq, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        }
        default:
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
      }
      BENCH_END(vt_chan_level);

      // --- 2.10 PIO + PWM + PW math (very close to original, but float inside PW calc) ---
      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t sm1N = VOICE_TO_SM[DCO_A];
      uint8_t sm2N = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, sm1N, clk_div1);
      pio_sm_put(pioN_B, sm2N, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      osc_last_clk_div[DCO_C] = clk_div3;
      BENCH_END(vt_pio_write);

      // Mono: A+B oscSync retrig + RANGE A/B/C. Para: osc k only, no oscSync.
      if (voiceMode == 0) {
      if (note_on_flag_flag[i]) {
        BENCH_BEGIN(vt_note_retrig);
        // Sync logic mirrored from fixed-point voice_task_fixed_point, using float-derived periods.
        if (oscSync >= 1) {
          // One probe for the whole SM apply path — fine MAIN slices mis-attribute cold XIP.
          BENCH_BEGIN(vt_retrig_sm_apply);
          if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
            // Exact split was computed after phase_align (retrig_p1 / retrig_p2).
            // OSC3 is deliberately absent: never retriggered; Y stays at pioPulseLength.
            uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
            pio_set_sm_mask_enabled(pio[0], maskAB, false);

            // Frame already did put+pull, so TX is empty — fused noclear load (no FJOIN).
            osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
                                             DCO_B, retrig_p2.y, retrig_p2.clk_div);

            pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

            if (phaseHoldX != 0) {
              osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
            } else {
              pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
            }

            pio_enable_sm_mask_in_sync(pio[0], maskAB);
          } else {
            // SYNC_JMP: phase resync only — no disable / Y load / enable_in_sync.
            // X preload needs a stopped SM, so degree offsets require EXACT_Y.
            pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
            pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
          }
          BENCH_END(vt_retrig_sm_apply);
        }

        BENCH_BEGIN(vt_retrig_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_retrig_pwm);
        BENCH_END(vt_note_retrig);
      }

      } else {
        // Paraphonic: edge k → RANGE osc k only (same as mono with oscSync off).
        // No SM stop/restart — that phase-resets and clicks when porta keeps the amp open.
        // Pitch continues via the per-frame put/pull above. No oscSync / phase-align.
        uint16_t chanLevels[3];
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          chanLevels[0] = character_clamp_amp((int32_t)chanLevel + amp_j);
          chanLevels[1] = character_clamp_amp((int32_t)chanLevel2 + amp_j);
          chanLevels[2] = character_clamp_amp((int32_t)chanLevel3 + amp_j);
        } else {
          chanLevels[0] = chanLevel;
          chanLevels[1] = chanLevel2;
          chanLevels[2] = chanLevel3;
        }

        for (int k = 0; k < NUM_VOICES; k++) {
          if (!note_on_flag_flag[k]) continue;
          BENCH_BEGIN(vt_note_retrig);
          const uint8_t osc = (uint8_t)k;
          BENCH_BEGIN(vt_retrig_pwm);
          write_range_pwm(osc, chanLevels[k]);
          BENCH_END(vt_retrig_pwm);
          BENCH_END(vt_note_retrig);
        }
      }
  
      if (timer99microsFlag2) {
        BENCH_BEGIN(vt_range_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_range_pwm);

        const bool pulseOn = waveEnable[0][1] || waveEnable[1][1] || waveEnable[2][1];
        if (pulseOn) {
          const int16_t local_LFO2Level = LFO2Level;
          const int16_t local_LFO2toPW = LFO2toPW;
          BENCH_FBEGIN(vt_pwm_calc);
          float adsr1_delta =
            ((float)ADSR1Level_q15[i] * (float)ADSR1toPWM_scale) * (1.0f / 32768.0f);
          float lfo2_delta =
            ((float)local_LFO2Level * (float)local_LFO2toPW) * (1.0f / 32767.0f);
          float pw_calc =
              (float)DIV_COUNTER_PW - 1.0f
            - (float)PW[0]
            - lfo2_delta
            + adsr1_delta
            + (float)character_pw_delta();
  
          if (pw_calc < 0.0f) pw_calc = 0.0f;
          if (pw_calc > (float)(DIV_COUNTER_PW - 1)) pw_calc = (float)(DIV_COUNTER_PW - 1);
  
          PW_PWM[i] = (uint16_t)pw_calc;
          BENCH_FEND(vt_pwm_calc);

          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i],
                             pwm_gpio_to_channel(PW_PINS[i]),
                             get_PW_level_interpolated(PW_PWM[i], i));
          BENCH_END(vt_pw_update);
        } else {
          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
          BENCH_END(vt_pw_update);
        }
      }
    }

    for (int k = 0; k < NUM_VOICES; k++) {
      note_on_flag_flag[k] = false;
    }

  // Whole-task timing is taken by the BENCH_voice_task probe around voice_task_main() in
  // loop1(), so both engines are measured at the same boundary.

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}
#endif  // USE_FLOAT_VOICE_TASK

// Round-robin free-voice allocator. Called from note_on() when polyMode == 1.
inline uint8_t get_free_voice_sequential() {
  uint8_t nextVoice;
  uint8_t freeVoices = 0;

  if (VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES - 1]] == 1 || VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES - 1]] == 0) {
    for (int voiceIndex = NUM_VOICES - 1; voiceIndex > 0; voiceIndex--) {
      if (VOICES[VOICES_LAST_SEQUENCE[voiceIndex]] == 0) {
        nextVoice = VOICES_LAST_SEQUENCE[voiceIndex];
        freeVoices = 1;
        for (int freeIndex = voiceIndex; freeIndex > 0; freeIndex--) {
          VOICES_LAST_SEQUENCE[freeIndex] = VOICES_LAST_SEQUENCE[freeIndex - 1];
        }
        VOICES_LAST_SEQUENCE[0] = nextVoice;
        return nextVoice;
      }
    }
  } else {
    if (VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES - 1]] == 0) {
      nextVoice = VOICES_LAST_SEQUENCE[NUM_VOICES - 1];

      for (int voiceIndex = NUM_VOICES - 1; voiceIndex > 0; voiceIndex--) {
        VOICES_LAST_SEQUENCE[voiceIndex] = VOICES_LAST_SEQUENCE[voiceIndex - 1];
      }

      VOICES_LAST_SEQUENCE[0] = nextVoice;

      return nextVoice;
    }
  }
  if (freeVoices == 0) {
    nextVoice = VOICES_LAST_SEQUENCE[NUM_VOICES - 1];

    for (int voiceIndex = NUM_VOICES - 1; voiceIndex > 0; voiceIndex--) {
      VOICES_LAST_SEQUENCE[voiceIndex] = VOICES_LAST_SEQUENCE[voiceIndex - 1];
    }

    VOICES_LAST_SEQUENCE[0] = nextVoice;
  }
  return nextVoice;
}

// Oldest-voice / steal allocator. Called from note_on() when polyMode == 0.
inline uint8_t get_free_voice() {
  uint32_t oldest_time = millis();
  uint8_t oldest_voice = 0;

  for (int i = 0; i < NUM_VOICES; i++)  // REVISAR!!
  {
    uint8_t n = (NEXT_VOICE + i) % NUM_VOICES;

    if (VOICES[n] == 0) {
      NEXT_VOICE = (n + 1) % NUM_VOICES;
      return n;
    }

    if (VOICES[i] < oldest_time) {
      oldest_time = VOICES[i];
      oldest_voice = i;
    }
  }

  NEXT_VOICE = (oldest_voice + 1) % NUM_VOICES;
  return oldest_voice;
}

// Map voiceMode → NUM_VOICES / STACK_VOICES. Called from init_voices and apply_param_voice_mode.
//   0 mono:       one MIDI voice → oscs 0..2 (engine still bound by NUM_VOICES_VOICE_TASK)
//   1 paraphonic: up to TOTAL notes, voice i → osc i (EnvDCO pitch tap per osc)
//   2 stub:       DCO4 stack leftover — counts only; no new stack behavior
inline void setVoiceMode() {
  switch (voiceMode) {
    case 0:
      NUM_VOICES = 1;
      STACK_VOICES = 1;
      // Drop any stale held notes when entering mono.
      mono_note_stack_clear();
      break;
    case 1:
      NUM_VOICES = NUM_VOICES_TOTAL;
      STACK_VOICES = 1;
      break;
    case 2:
      NUM_VOICES = NUM_VOICES_TOTAL;
      STACK_VOICES = NUM_VOICES_TOTAL;
      break;
  }
}

// Rebuild the PIO sync topology and retrigger voices.
// Called from apply_param_sync_mode (Serial2).
void setSyncMode() {
  // assign_sm_mapping() keeps the slave below its master in SM index; start_voice_sms()
  // re-derives every SM's program, set pin and sideset pin from syncMode and
  // softSyncChunks, then starts them all on the same cycle.
  //
  // The old implementation poked sideset pins in place and called pio_sm_restart(),
  // which cleared the shift counters but left PC, X and Y — it could strand an SM
  // mid-loop with a stale X for one glitched period. The note_on_flag retrigger below
  // already re-pushes everything, so the restart was never needed.
  assign_sm_mapping();
  start_voice_sms();

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

// One osc: shipping FIXED = Q24→Q8 lookup. With USE_FLOAT_AMP_COMP, non-FIXED
// methods take Q24→Hz then get_chan_level_by_method (cmds 20–22).
static inline __attribute__((always_inline)) uint16_t amp_level_q24(int64_t freq_q24, uint8_t osc) {
#ifdef USE_FLOAT_AMP_COMP
  if (amp_comp_method != AMP_COMP_FIXED) {
    float hz = (float)freq_q24 * (1.0f / 16777216.0f);
    return get_chan_level_by_method(hz, osc);
  }
#endif
  int32_t freqFx = (int32_t)((freq_q24 + (1LL << 15)) >> 16);
  return get_chan_level_lookup_fast(freqFx, osc);
}

// Q24 + syncMode switch + 3× lookup. SRAM so vt_chan_level is not a flash caller.
static void __not_in_flash_func(amp_chan_levels_fixed)(int64_t freq_q24_A, int64_t freq_q24_B,
                                                      int64_t freq_q24_C, uint8_t oscA, uint8_t oscB,
                                                      uint8_t oscC, uint16_t *outA, uint16_t *outB,
                                                      uint16_t *outC) {
  const uint8_t sm = syncMode;
  switch (sm) {
    case 1: {
      int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
      *outA = amp_level_q24(maxAB, oscA);
      *outB = amp_level_q24(freq_q24_B, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
    }
    case 2: {
      int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
      *outA = amp_level_q24(freq_q24_A, oscA);
      *outB = amp_level_q24(maxAB, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
    }
    default:
      *outA = amp_level_q24(freq_q24_A, oscA);
      *outB = amp_level_q24(freq_q24_B, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
  }
}

/**
 * @brief Fast amplitude compensation lookup (Q8 Hz) for fixed path / AMP_COMP_FIXED.
 *
 * Window rule matches float: first i with freqRow[i] <= x < freqRow[i+2].
 * Find: per-osc ampWinCache → walk → full scan (same as FLOAT_QUAD).
 */
uint16_t __not_in_flash_func(get_chan_level_lookup_fast)(int32_t x, uint8_t voiceN) {
  const int32_t* freqRow   = ampCompFrequencyArray[voiceN];
  const int32_t* ampRow    = ampCompArray[voiceN];
  const int32_t* xBaseRow  = xBaseWIN[voiceN];
  const int32_t* spanRow   = dxWIN[voiceN];
  const uint32_t* invRow_q28 = invDxWIN_q28[voiceN];
  const int32_t* aRow      = aQWIN_fast[voiceN];
  const int32_t* bRow      = bQWIN_fast[voiceN];
  const uint16_t* cRow     = cQWIN[voiceN];

  if (x <= freqRow[0]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)ampRow[0];
  }

  // Domain ceiling: at/above AMP_COMP_MAX_HZ the cal sentinel is full RANGE.
  // Without this, x == MAX_Q matches no window (exclusive upper bound) and the
  // fallback window-0 path clamps t∈[0,1] to the first segment — ~12k PWM off
  // float, which extrapolates absolute Hz on that window to ~DIV_COUNTER.
  if (x >= AMP_COMP_MAX_HZ_Q) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  // Real plateau only: precompute leaves plateauStartFreqQ at AMP_COMP_MAX_HZ_Q
  // when none was found (same role as plateauStartIndex < 0 in the original code).
  // Do not use plateauStartIndex here — dual-build restores that for the float path.
  if (plateauStartFreqQ[voiceN] < AMP_COMP_MAX_HZ_Q &&
      x >= plateauStartFreqQ[voiceN]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  const int maxWindow = ampCompTableSize - 2;
  int window = ampWinCache[voiceN];

  if (window >= 0 && window <= maxWindow &&
      x >= freqRow[window] && x < freqRow[window + 2]) {
    BENCH_PATH_INC(amp_hit);
  } else {
    int cand = window;
    if (cand < 0 || cand > maxWindow) cand = 0;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (x >= freqRow[cand + 2]) {
      while (cand < maxWindow && x >= freqRow[cand + 2]) {
        ++cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    } else if (x < freqRow[cand]) {
      while (cand > 0 && x < freqRow[cand]) {
        --cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    }
    if (cand >= 0 && cand <= maxWindow &&
        x >= freqRow[cand] && x < freqRow[cand + 2]) {
      window = cand;
      BENCH_PATH_INC(amp_miss_walk);
#if defined(BENCH_PATH_STATS)
      bench_path_amp_walk_steps(steps);
#endif
    } else {
      window = 0;
      for (int i = 0; i <= maxWindow; ++i) {
        if (x >= freqRow[i] && x < freqRow[i + 2]) {
          window = i;
          break;
        }
      }
      BENCH_PATH_INC(amp_miss_scan);
    }
    ampWinCache[voiceN] = (int16_t)window;
  }

  int32_t dx = x - xBaseRow[window];
  const int32_t span = spanRow[window];
  if (dx < 0) dx = 0;
  if (dx > span) dx = span;

  const uint32_t inv_q28 = invRow_q28[window];
  uint32_t t_q = (uint32_t)(((uint64_t)dx * inv_q28) >> (28 - T_FRAC));

  const int32_t a = aRow[window];
  const int32_t b = bRow[window];
  const int32_t c = (int32_t)cRow[window];

  uint32_t t2 = (uint32_t)(((uint32_t)t_q * t_q) >> T_FRAC);
  int32_t term_a, term_b;
  if (amp_quad_muls_i32) {
    term_a = (a * (int32_t)t2) >> T_FRAC;
    term_b = (b * (int32_t)t_q) >> T_FRAC;
  } else {
    term_a = (int32_t)(((int64_t)a * (int64_t)t2) >> T_FRAC);
    term_b = (int32_t)(((int64_t)b * (int64_t)t_q) >> T_FRAC);
  }

  int32_t y_q = term_a + term_b + (c << T_FRAC);
  int32_t y = (y_q + (1 << (T_FRAC - 1))) >> T_FRAC;

  if (y < 0) y = 0;
  if (y > (int32_t)DIV_COUNTER) y = (int32_t)DIV_COUNTER;

  return (uint16_t)y;
}

#ifdef USE_FLOAT_AMP_COMP
/**
 * @brief Pure-float quadratic amp-comp (Hz domain). Live FLOAT_QUAD.
 * Window find: per-osc cache → walk → full scan (first-match [Hz[w], Hz[w+2]]).
 */
uint16_t __not_in_flash_func(get_chan_level_float_quad)(float freqHz, uint8_t voiceN) {
  if (freqHz <= ampCompFrequencyHz[voiceN][0]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)ampCompArray[voiceN][0];
  }

  // Same domain ceiling as FIXED (see get_chan_level_lookup_fast).
  if (freqHz >= (float)AMP_COMP_MAX_HZ) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  if (plateauStartIndex[voiceN] >= 0) {
    float plateauFreqHz = plateauStartFreqHz[voiceN];
    if (freqHz >= plateauFreqHz) {
      BENCH_PATH_INC(amp_clamp);
      return (uint16_t)DIV_COUNTER;
    }
  }

  const int maxWindow = ampCompTableSize - 2;
  const float *hzRow = ampCompFrequencyHz[voiceN];
  int window = ampWinCache[voiceN];

  if (window >= 0 && window <= maxWindow &&
      freqHz >= hzRow[window] && freqHz < hzRow[window + 2]) {
    BENCH_PATH_INC(amp_hit);
  } else {
    int cand = window;
    if (cand < 0 || cand > maxWindow) cand = 0;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (freqHz >= hzRow[cand + 2]) {
      while (cand < maxWindow && freqHz >= hzRow[cand + 2]) {
        ++cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    } else if (freqHz < hzRow[cand]) {
      while (cand > 0 && freqHz < hzRow[cand]) {
        --cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    }
    if (cand >= 0 && cand <= maxWindow &&
        freqHz >= hzRow[cand] && freqHz < hzRow[cand + 2]) {
      window = cand;
      BENCH_PATH_INC(amp_miss_walk);
#if defined(BENCH_PATH_STATS)
      bench_path_amp_walk_steps(steps);
#endif
    } else {
      window = 0;
      for (int i = 0; i <= maxWindow; ++i) {
        if (freqHz >= hzRow[i] && freqHz < hzRow[i + 2]) {
          window = i;
          break;
        }
      }
      BENCH_PATH_INC(amp_miss_scan);
    }
    ampWinCache[voiceN] = (int16_t)window;
  }

  float a = aCoeff[voiceN][window];
  float b = bCoeff[voiceN][window];
  float c = cCoeff[voiceN][window];

  float interpolatedValue = (a * freqHz + b) * freqHz + c;
  return (uint16_t)round(interpolatedValue);
}

uint16_t __not_in_flash_func(get_chan_level_lut)(float freqHz, uint8_t voiceN) {
  if (freqHz <= 0.0f) return ampCompLut[voiceN][0];
  if (freqHz >= (float)AMP_COMP_MAX_HZ) return ampCompLut[voiceN][AMP_COMP_MAX_HZ];
  // Nearest integer Hz (not trunc) — same single table load, lower quantize bias.
  // Stay off LUT[MAX] until the hard clamp above: rounding 6999.75→7000 would
  // return the plateau bin while float is still on the last window (~200 counts).
  int32_t hz = (int32_t)(freqHz + 0.5f);
  if (hz < 0) hz = 0;
  if (hz >= AMP_COMP_MAX_HZ) hz = AMP_COMP_MAX_HZ - 1;
  return ampCompLut[voiceN][hz];
}
#endif  // USE_FLOAT_AMP_COMP

// Map raw PW counter into calibrated center/limits for one oscillator. Used on the 99 µs PW path.
inline uint16_t get_PW_level_interpolated(uint16_t PWval, uint8_t oscN) {

  uint16_t chanLevel;

  // Horizontal PW axis: 0 .. DIV_COUNTER_PW-1 (pot/LFO/ADSR domain)
  // Vertical axis (output): mapped to calibrated low/center/high PWM limits.

  if (PWval >= (DIV_COUNTER_PW - 1)) {
    // Above max PW, clamp to calibrated high limit.
    return PW_HIGH_LIMIT[oscN];
  } else if (PWval <= 0) {
    // Below min PW, clamp to calibrated low limit.
    return PW_LOW_LIMIT[oscN];
  } else {
    uint16_t pwLowBreak  = PW_LOOKUP[0];  // usually 0
    uint16_t pwMidBreak  = PW_LOOKUP[1];  // mid-point
    uint16_t pwHighBreak = PW_LOOKUP[2];  // usually DIV_COUNTER_PW-1

    if (PWval >= pwMidBreak) {
      // Upper half: interpolate from center to high limit.
      chanLevel = map(PWval,
                      pwMidBreak, pwHighBreak,
                      PW_CENTER[oscN], PW_HIGH_LIMIT[oscN]);
    } else {
      // Lower half: interpolate from low limit to center.
      chanLevel = map(PWval,
                      pwLowBreak, pwMidBreak,
                      PW_LOW_LIMIT[oscN], PW_CENTER[oscN]);
    }

    return chanLevel;
  }
}

// Drive one oscillator for calibration measurement (manual cal and nested auto-cal probes).
void voice_task_autotune(uint8_t taskAutotuneVoiceMode, uint16_t calibrationValue) {

  float freq;
  uint8_t note1;  // = 57;
  int chanLevel = ampCompCalibrationVal;

  if (VOICE_NOTES[0] > 0) {
    note1 = VOICE_NOTES[0] - 12;
  }

  if (taskAutotuneVoiceMode == 1 || taskAutotuneVoiceMode == 4) {
    freq = PIDOutput;
  } else {
    freq = (float)sNotePitches[note1];
  }

  // Target period in cycles for the calibration tone. Guarded because freq can be 0,
  // which would make the float division infinite and the cast undefined.
  uint32_t autotune_total_cycles =
      (freq > 0.0f) ? (uint32_t)fminf(((float)sysClock_Hz / freq) + 0.5f, 4.0e9f) : 0u;

  if (manualCalibrationFlag == true) {  // One Ocillator at a time to get correct gap

    uint8_t currentCalibrationOscillator = (uint8_t)manualCalibrationStage;
    if (currentCalibrationOscillator >= NUM_OSCILLATORS) {
      currentCalibrationOscillator = NUM_OSCILLATORS - 1;
    }

    // ALL AT ONCE
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
      PIO pioN = pio[VOICE_TO_PIO[i]];
      uint8_t sm1N = VOICE_TO_SM[i];

      if (i != currentCalibrationOscillator) {
        uint32_t silence_clk_div1 = 200;

        pio_sm_put(pioN, sm1N, silence_clk_div1);
        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
        write_range_pwm((uint8_t)i, 0);
      } else {

        uint32_t clk_div1 = autotune_total_cycles
                              ? pio_clk_div_for_y(autotune_total_cycles, osc_last_y[i],
                                                  osc_ramp_weight(i), osc_period_overhead(i))
                              : 0u;

        pio_sm_put(pioN, sm1N, clk_div1);

        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

        write_range_pwm((uint8_t)i, calibrationValue);

        pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), 0);

        //Serial.println((String) "currentCalibrationOscillator: " + (int)currentCalibrationOscillator + (String) "   calibrationValue: " + (int)calibrationValue);
      }
    }
  } else {

    PIO pioN = pio[VOICE_TO_PIO[currentDCO]];
    uint8_t sm1N = VOICE_TO_SM[currentDCO];

    uint32_t clk_div1 = autotune_total_cycles
                          ? pio_clk_div_for_y(autotune_total_cycles, osc_last_y[currentDCO],
                                              osc_ramp_weight(currentDCO),
                                              osc_period_overhead(currentDCO))
                          : 0u;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

    switch (taskAutotuneVoiceMode) {
      case 0:
        write_range_pwm(currentDCO, calibrationValue);
        break;
      case 1:
        write_range_pwm(currentDCO, calibrationValue);
        pio_sm_exec(pioN, sm1N, pio_encode_jmp(osc_restart_target(currentDCO)));
        break;
      case 2:
        write_range_pwm(currentDCO, chanLevel);
        break;
      case 3:
        chanLevel = get_chan_level_for_engine(freq, currentDCO);
        write_range_pwm(currentDCO, chanLevel);
      case 4:
        write_range_pwm(currentDCO, calibrationValue);
        break;
    }

    //}
    // Serial.println((String) "| currentDCO: " + currentDCO + (String) " | freq: " + freq + (String) " | clk_div1: " + clk_div1 + (String) " | ampCompCalibrationVal: " + ampCompCalibrationVal);
  }
}

// Cached variant: pass DCO index to reuse last segment and avoid binary search
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t __not_in_flash_func(interpolatePitchMultiplierIntQ16_cached)(int32_t xQ16, int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  if (xInt <= xMultiplierTable[0]) {
    return yMultiplierTable[0];
  }
  if (xInt >= xMultiplierTable[multiplierTableSize - 1]) {
    return yMultiplierTable[multiplierTableSize - 1];
  }
  int low = interpSegCache[dcoIndex];
  if (low < 0 || low > multiplierTableSize - 2 || !(xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (xInt >= xMultiplierTable[low + 1]) {
        while (low < multiplierTableSize - 2 && xInt >= xMultiplierTable[low + 1]) low++;
      } else if (xInt < xMultiplierTable[low]) {
        while (low > 0 && xInt < xMultiplierTable[low]) low--;
      }
    }
    if (!(low >= 0 && low < multiplierTableSize - 1 && xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
      int l = 0, h = multiplierTableSize - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xMultiplierTable[m] <= xInt && xInt < xMultiplierTable[m + 1]) {
          low = m;
          break;
        } else if (xInt < xMultiplierTable[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > multiplierTableSize - 2) low = multiplierTableSize - 2;
    }
    interpSegCache[dcoIndex] = (int16_t)low;
  }
  int32_t x0 = xMultiplierTable[low];
  int32_t y0 = yMultiplierTable[low];
  int32_t deltaQ12 = (xQ16 - (x0 << 16)) >> 4;
  int32_t y = y0 + (int32_t)((((int64_t)deltaQ12 * (int64_t)slopeQ12[low]) + (1LL << 23)) >> 24);
  return y;
}
#endif // Q12

#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
// xQ16 and tables are natural Q16 (1.0 = 65536). Returns frequency ratio as Q16.
// Miss path: O(1) trunc±1 (same idea as FLOAT_FAST) — avoids walk under LFO thrash.
int32_t __not_in_flash_func(interpolateRatioQ16_cached)(int32_t xQ16, int dcoIndex) {
  // Endpoints match initMultiplierTables (-1 / 3) in Q16.
  static constexpr int32_t kPitchX0_Q16 = -65536; // -1.0
  static constexpr int32_t kPitchX1_Q16 = 196608; // 3.0
  if (xQ16 <= kPitchX0_Q16) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTable[0];
  }
  if (xQ16 >= kPitchX1_Q16) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTable[multiplierTableSize - 1];
  }
  const int lastSeg = multiplierTableSize - 2;
  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      xMultiplierTable[low] <= xQ16 && xQ16 < xMultiplierTable[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
    // cand ≈ (x - (-1)) * (N/4); N=200 → *50, then >>16 for Q16.
    static constexpr int kPitchInvDx = multiplierTableSize / 4; // 50
    int cand = (int)(((int64_t)(xQ16 - kPitchX0_Q16) * (int64_t)kPitchInvDx) >> 16);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (cand < lastSeg && xQ16 >= xMultiplierTable[cand + 1]) {
      ++cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    } else if (cand > 0 && xQ16 < xMultiplierTable[cand]) {
      --cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    }
    low = cand;
    BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
    bench_path_walk_steps(steps);
#endif
    interpSegCache[dcoIndex] = (int16_t)low;
  }
  const int32_t x0 = xMultiplierTable[low];
  const int32_t y0 = yMultiplierTable[low];
  const int32_t delta = xQ16 - x0;
  // slopeQ16 = (dy << 16) / dx  →  y = y0 + (delta * slope) >> 16
  // Boot proves |delta_max*slope| fits int32 (Q20 product does not).
  return y0 + (((delta * slopeQ16[low]) + (1 << 15)) >> 16);
}
#endif // PITCH_INTERP_RATIO_Q16

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
// x = pitch modifier in ~[-1, 3]; returns frequency ratio directly (tables are unscaled).
// Walk + bsearch find (A/B vs FLOAT_FAST).
float __not_in_flash_func(interpolateRatioFloat_cached)(float x, int dcoIndex) {
  if (x <= xMultiplierTableF[0]) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[0];
  }
  if (x >= xMultiplierTableF[multiplierTableSize - 1]) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[multiplierTableSize - 1];
  }

  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= multiplierTableSize - 2 &&
      xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (x >= xMultiplierTableF[low + 1]) {
        while (low < multiplierTableSize - 2 && x >= xMultiplierTableF[low + 1]) {
          ++low;
#if defined(BENCH_PATH_STATS)
          steps++;
#endif
        }
      } else if (x < xMultiplierTableF[low]) {
        while (low > 0 && x < xMultiplierTableF[low]) {
          --low;
#if defined(BENCH_PATH_STATS)
          steps++;
#endif
        }
      }
    }
    if (!(low >= 0 && low < multiplierTableSize - 1 &&
          xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1])) {
      int l = 0;
      int h = multiplierTableSize - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xMultiplierTableF[m] <= x && x < xMultiplierTableF[m + 1]) {
          low = m;
          break;
        } else if (x < xMultiplierTableF[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > multiplierTableSize - 2) low = multiplierTableSize - 2;
      BENCH_PATH_INC(ratio_miss_bsearch);
    } else {
      BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
      bench_path_walk_steps(steps);
#endif
    }
    interpSegCache[dcoIndex] = (int16_t)low;
  }

  return yMultiplierTableF[low] + slopeF[low] * (x - xMultiplierTableF[low]);
}
#endif // PITCH_INTERP_FLOAT

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
// Trunc+clamp±1 find; same lerp as walk. Keep ±1 even when walk_steps≈0 (live ballast).
// noinline: isolate codegen from voice_task_float (distinct SRAM symbol).
__attribute__((noinline))
float __not_in_flash_func(interpolateRatioFloat_cached_fast)(float x, int dcoIndex) {
  // Endpoints match initMultiplierTables (-1 / 3).
  if (x <= -1.0f) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[0];
  }
  if (x >= 3.0f) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[multiplierTableSize - 1];
  }

  static constexpr float kPitchX0 = -1.0f;
  static constexpr float kPitchInvDx = (float)multiplierTableSize / 4.0f; // N/4 = 50
  const int lastSeg = multiplierTableSize - 2;

  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
    int cand = (int)((x - kPitchX0) * kPitchInvDx);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
    // Float rounding / live ballast: keep both ±1 compares even if steps stay 0.
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (cand < lastSeg && x >= xMultiplierTableF[cand + 1]) {
      ++cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    } else if (cand > 0 && x < xMultiplierTableF[cand]) {
      --cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    }
    low = cand;
    BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
    bench_path_walk_steps(steps);
#endif
    interpSegCache[dcoIndex] = (int16_t)low;
  }

  return yMultiplierTableF[low] + slopeF[low] * (x - xMultiplierTableF[low]);
}
#endif // PITCH_INTERP_FLOAT_FAST

// Build integer/float pitch-multiplier tables and slopes (boot). Called from init_voices().
void initMultiplierTables() {

  float y_value;
  double divisor = multiplierTableSize;
  double fraction = 4.00d / divisor;

  for (int i = 0; i < multiplierTableSize; i++) {
    double x;

    if (i == 0) {
      x = -1.00d;
      y_value = 0.25d;
    } else if (i == multiplierTableSize - 1) {
      x = 3.0d;
      y_value = 4.0d;
    } else {
      x = (-1.00d + (fraction * (double)i));
      y_value = expInterpolationSolveY(x + 1.00d, 1.00d, 3.00d, 0.50d, 2.00d);
    }

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || \
    PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
    // Natural domain: x = modifier [-1,3], y = frequency ratio (no int-table scale).
    xMultiplierTableF[i] = (float)x;
    yMultiplierTableF[i] = y_value;
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
    // Native Q16: x and y store natural units (1.0 = 65536). No ×10000.
    xMultiplierTable[i] = (int32_t)(x * 65536.0 + (x >= 0.0 ? 0.5 : -0.5));
    yMultiplierTable[i] = (int32_t)((double)y_value * 65536.0 + 0.5);
    x0Q16_tbl[i] = xMultiplierTable[i];
#else
    // Q12 A/B: legacy ×10000 table-units.
    xMultiplierTable[i] = (int32_t)(x * (double)multiplierTableScale);
    yMultiplierTable[i] = (int32_t)(y_value * (double)multiplierTableScale);
    x0Q16_tbl[i]        = xMultiplierTable[i] << 16;
#endif
  }

#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  // Q20×(dx-1) overflows int32 on upper segments (~7.5e9). Q16 matches Q20 y on
  // this 200-knot set and keeps |delta*slope| in signed 32-bit for MULS lerp.
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
    int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
    if (dx == 0) dx = 1;
    int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
    int64_t numSlope = ((int64_t)dy << 16) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ16[i] = (int32_t)(numSlope / (int64_t)dx);
    const int32_t delta_max = (dx > 0) ? (dx - 1) : 0;
    const int64_t prod = (int64_t)delta_max * (int64_t)slopeQ16[i];
    // One-time boot proof only (no live branch). Stock table always fits.
    if (prod > (int64_t)INT32_MAX || prod < (int64_t)INT32_MIN) {
#if defined(RUNNING_AVERAGE)
      bench_out_printf("ratio slopeQ16*delta overflows int32 at seg %d\n", i);
#endif
    }
  }
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
    int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
    if (dx == 0) dx = 1;
    int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
    int64_t numSlope12 = ((int64_t)dy << 12) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ12[i] = (int32_t)(numSlope12 / (int64_t)dx);
  }
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || \
      PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  for (int i = 0; i < multiplierTableSize - 1; ++i) {
    float dxF = xMultiplierTableF[i + 1] - xMultiplierTableF[i];
    if (dxF == 0.0f) dxF = 1.0f;
    slopeF[i] = (yMultiplierTableF[i + 1] - yMultiplierTableF[i]) / dxF;
  }
#endif

  for (int d = 0; d < NUM_OSCILLATORS; ++d) interpSegCache[d] = -1;
}