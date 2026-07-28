#include "include_all.h"

// Enable/disable detailed DCO debug report (including OSC1 frequency stages)
#define DCO_DEBUG_REPORT 0


#ifdef RUNNING_AVERAGE
// RunningAverage object definitions for timing measurements
RunningAverage ra_pitchbend(2000);
RunningAverage ra_osc2_detune(2000);
RunningAverage ra_portamento(2000);
RunningAverage ra_adsr_modifier(2000);
RunningAverage ra_unison_modifier(2000);
RunningAverage ra_drift_multiplier(2000);
RunningAverage ra_modifiers_combination(2000);
RunningAverage ra_freq_scaling_x(2000);
RunningAverage ra_freq_scaling_ratio(2000);
RunningAverage ra_freq_scaling_post(2000);
RunningAverage ra_get_chan_level(2000);
RunningAverage ra_pwm_calculations(2000);
RunningAverage ra_voice_task_total(2000);
RunningAverage ra_clk_div_calc(2000);

unsigned long last_timing_print = 0;
unsigned long voice_task_max_time = 0;
const unsigned long TIMING_PRINT_INTERVAL = 1000;  // Print every 5 seconds
#endif

void init_voices() {

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    VOICE_NOTES[i] = DCO_calibration_start_note;
  }

  initMultiplierTables();
  setVoiceMode();
  voice_task();
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

inline void voice_task() {
#ifdef RUNNING_AVERAGE
  unsigned long voice_task_start_time = micros();
#endif

  // Track portamento-time and mode changes between calls so we can smoothly
  // retime the glide without introducing pitch discontinuities.
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_TIME;
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  // Pre-calculate pitch bend as a Q24 value. This is done once per voice_task call.
  int32_t calcPitchbend_q24;

#ifdef RUNNING_AVERAGE
  unsigned long t_start = micros();
#endif
  // Optimized: Perform pitch bend calculation entirely in fixed-point Q24.
  // ((bend / 8192.0) - 1.0) * pitchBendMultiplier
  // This avoids float conversions and multiplications in the hot path.
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  calcPitchbend_q24 = (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
#ifdef RUNNING_AVERAGE
  ra_pitchbend.addValue((float)(micros() - t_start));
#endif

  last_midi_pitch_bend = midi_pitch_bend;

  // Hoist PWM parameters out of the loop. This is critical for performance,
  // as it reads the volatile LFO2toPW variable only once per task run.
  const int16_t local_ADSR1toPWM = ADSR1toPWM;
  const int16_t local_LFO2toPW = LFO2toPW;

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {

#if DCO_DEBUG_REPORT
    // Debug: track OSC1 frequency at key stages of the pipeline for DCO report.
    float dbg_freq_base_Hz = 0.0f;       // After portamento, before modifiers
    float dbg_freq_after_mod_Hz = 0.0f;  // After all modifiers applied (freq_q24_A)
#endif

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
    }

    if (VOICE_NOTES[i] >= 0) {
      uint8_t note1 = VOICE_NOTES[i] - 36 + OSC1_interval;
      if (note1 > highestNote) {
        note1 -= ((uint8_t(note1 - highestNote) / 12) * 12);
      }
      uint8_t note2 = note1 - 36 + OSC2_interval;
      if (note2 > highestNote) {
        note2 -= ((uint8_t(note2 - highestNote) / 12) * 12);
      }
      uint8_t note3 = note1 - 36 + OSC3_interval;
      if (note3 > highestNote) {
        note3 -= ((uint8_t(note3 - highestNote) / 12) * 12);
      }
      // Clamp note indexes to table (defensive)
      const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
      if (note1 >= NOTE_TABLE_LEN) note1 = (uint8_t)(NOTE_TABLE_LEN - 1);
      if (note2 >= NOTE_TABLE_LEN) note2 = (uint8_t)(NOTE_TABLE_LEN - 1);
      if (note3 >= NOTE_TABLE_LEN) note3 = (uint8_t)(NOTE_TABLE_LEN - 1);

#ifdef RUNNING_AVERAGE
      unsigned long t_osc2 = micros();
#endif
      // Optimized: Calculate OSC2/OSC3 detune in Q24 and keep it there.
      // The float conversion has been removed as it is no longer needed.
      // detune = 1.0 + 0.0002 * (256 - val)
      static constexpr int32_t DETUNE_SCALE_Q24 = (int32_t)(0.0002f * (float)(1 << 24) + 0.5f);
      int32_t detune_steps = ((int)256 - OSC2DetuneVal);
      int32_t detune_q24 = (1 << 24) + (detune_steps * DETUNE_SCALE_Q24);
      int32_t detune3_steps = ((int)256 - OSC3DetuneVal);
      int32_t detune3_q24 = (1 << 24) + (detune3_steps * DETUNE_SCALE_Q24);
#ifdef RUNNING_AVERAGE
      ra_osc2_detune.addValue((float)(micros() - t_osc2));
#endif

      int64_t freq_q24_A;
      int64_t freq_q24_B;
      int64_t freq_q24_C;

      // Fixed osc indices for current mono hardware (3 oscs on voice 0).
      // Future paraphonic mode can remap osc ownership per voice without gutting allocation.
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;

      // Serial.println("VOICE TASK 2");
      ////***********************    PORTAMENTO CODE   ****************************************/////
#ifdef RUNNING_AVERAGE
      unsigned long t_portamento = micros();
#endif
      if (portaTime > 0 /*&& portamento_start != 0 && portamento_stop != 0*/) {
        uint32_t now_us = micros();
        portamentoTimer[i] = now_us - portamentoStartMicros[i];

        if (note_on_flag_flag[i]) {
          // Serial.println("NOTE ON");
          portamentoStartMicros[i] = now_us;

          portamentoTimer[i] = 0;

          // Derive endpoints for portamento
          int64_t stopA_q24 = sNotePitches_q24[note1];
          int64_t stopB_q24 = sNotePitches_q24[note2];
          int64_t stopC_q24 = sNotePitches_q24[note3];
          portamento_stop_q24[DCO_A] = stopA_q24;
          portamento_stop_q24[DCO_B] = stopB_q24;
          portamento_stop_q24[DCO_C] = stopC_q24;

          int32_t T = (portaTime == 0) ? 1 : (int32_t)portaTime;

          if (portaMode == PORTA_MODE_TIME) {
            // Time-based mode: glide linearly in frequency.
            int64_t startA_q24 = portamento_cur_freq_q24[DCO_A];
            int64_t startB_q24 = portamento_cur_freq_q24[DCO_B];
            int64_t startC_q24 = portamento_cur_freq_q24[DCO_C];
            portamento_start_q24[DCO_A] = startA_q24;
            portamento_start_q24[DCO_B] = startB_q24;
            portamento_start_q24[DCO_C] = startC_q24;
            portamento_cur_freq_q24[DCO_A] = startA_q24;
            portamento_cur_freq_q24[DCO_B] = startB_q24;
            portamento_cur_freq_q24[DCO_C] = startC_q24;

            int64_t dA = stopA_q24 - startA_q24;
            int64_t dB = stopB_q24 - startB_q24;
            int64_t dC = stopC_q24 - startC_q24;

            // Fixed-time glide: span is covered in approximately portaTime microseconds.
            int64_t halfT = (int64_t)T >> 1;
            int64_t numA = (dA >= 0) ? (dA + halfT) : (dA - halfT);
            int64_t numB = (dB >= 0) ? (dB + halfT) : (dB - halfT);
            int64_t numC = (dC >= 0) ? (dC + halfT) : (dC - halfT);
            freqPortaStep_q24[DCO_A] = (numA / (int64_t)T);
            freqPortaStep_q24[DCO_B] = (numB / (int64_t)T);
            freqPortaStep_q24[DCO_C] = (numC / (int64_t)T);
          } else {
            // Slew-rate (musical) mode: glide linearly in note-space (semitones).
            // Use current note position as start; if uninitialized, fall back to target note.
            int32_t startNoteA_q16 = porta_note_cur_q16[DCO_A];
            int32_t startNoteB_q16 = porta_note_cur_q16[DCO_B];
            int32_t startNoteC_q16 = porta_note_cur_q16[DCO_C];
            int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
            int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
            int32_t targetNoteC_q16 = ((int32_t)note3) << 16;

            if (startNoteA_q16 == 0) startNoteA_q16 = targetNoteA_q16;
            if (startNoteB_q16 == 0) startNoteB_q16 = targetNoteB_q16;
            if (startNoteC_q16 == 0) startNoteC_q16 = targetNoteC_q16;

            porta_note_start_q16[DCO_A] = startNoteA_q16;
            porta_note_start_q16[DCO_B] = startNoteB_q16;
            porta_note_start_q16[DCO_C] = startNoteC_q16;
            porta_note_stop_q16[DCO_A] = targetNoteA_q16;
            porta_note_stop_q16[DCO_B] = targetNoteB_q16;
            porta_note_stop_q16[DCO_C] = targetNoteC_q16;

            int32_t dNoteA_q16 = porta_note_stop_q16[DCO_A] - porta_note_start_q16[DCO_A];
            int32_t dNoteB_q16 = porta_note_stop_q16[DCO_B] - porta_note_start_q16[DCO_B];
            int32_t dNoteC_q16 = porta_note_stop_q16[DCO_C] - porta_note_start_q16[DCO_C];

            // Per-microsecond step in Q16 notes.
            // Use symmetric rounding for step magnitude.
            int64_t halfT = (int64_t)T >> 1;
            int64_t numA = (dNoteA_q16 >= 0) ? ((int64_t)dNoteA_q16 + halfT) : ((int64_t)dNoteA_q16 - halfT);
            int64_t numB = (dNoteB_q16 >= 0) ? ((int64_t)dNoteB_q16 + halfT) : ((int64_t)dNoteB_q16 - halfT);
            int64_t numC = (dNoteC_q16 >= 0) ? ((int64_t)dNoteC_q16 + halfT) : ((int64_t)dNoteC_q16 - halfT);
            porta_note_step_q16[DCO_A] = (int32_t)(numA / (int64_t)T);
            porta_note_step_q16[DCO_B] = (int32_t)(numB / (int64_t)T);
            porta_note_step_q16[DCO_C] = (int32_t)(numC / (int64_t)T);

            // Ensure we always move for non-zero intervals; otherwise tiny intervals
            // with long times could quantize to zero step and "stick".
            if (dNoteA_q16 != 0 && porta_note_step_q16[DCO_A] == 0) {
              porta_note_step_q16[DCO_A] = (dNoteA_q16 > 0) ? 1 : -1;
            }
            if (dNoteB_q16 != 0 && porta_note_step_q16[DCO_B] == 0) {
              porta_note_step_q16[DCO_B] = (dNoteB_q16 > 0) ? 1 : -1;
            }
            if (dNoteC_q16 != 0 && porta_note_step_q16[DCO_C] == 0) {
              porta_note_step_q16[DCO_C] = (dNoteC_q16 > 0) ? 1 : -1;
            }

            // Initialize current note and frequency at start of glide
            porta_note_cur_q16[DCO_A] = startNoteA_q16;
            porta_note_cur_q16[DCO_B] = startNoteB_q16;
            porta_note_cur_q16[DCO_C] = startNoteC_q16;
            portamento_cur_freq_q24[DCO_A] = noteQ16_to_freqQ24(startNoteA_q16);
            portamento_cur_freq_q24[DCO_B] = noteQ16_to_freqQ24(startNoteB_q16);
            portamento_cur_freq_q24[DCO_C] = noteQ16_to_freqQ24(startNoteC_q16);
          }
        }

        // Compute current glide position using existing timing/slope
        int32_t elapsed_us = (int32_t)portamentoTimer[i];
        int64_t curA;
        int64_t curB;
        int64_t curC;

        if (portaMode == PORTA_MODE_TIME) {
          if ((uint32_t)elapsed_us > portaTime) {
            // Snap to target once we have exceeded the (current) portamento time
            curA = portamento_stop_q24[DCO_A];
            curB = portamento_stop_q24[DCO_B];
            curC = portamento_stop_q24[DCO_C];
          } else {
            // Absolute-time base in Q24
            curA = portamento_start_q24[DCO_A] + freqPortaStep_q24[DCO_A] * (int64_t)elapsed_us;
            curB = portamento_start_q24[DCO_B] + freqPortaStep_q24[DCO_B] * (int64_t)elapsed_us;
            curC = portamento_start_q24[DCO_C] + freqPortaStep_q24[DCO_C] * (int64_t)elapsed_us;
          }
        } else {
          // Slew-rate (musical) mode: step is constant in note-space; stop when we reach the target.
          int32_t dNoteA_q16 = porta_note_stop_q16[DCO_A] - porta_note_start_q16[DCO_A];
          int32_t dNoteB_q16 = porta_note_stop_q16[DCO_B] - porta_note_start_q16[DCO_B];
          int32_t dNoteC_q16 = porta_note_stop_q16[DCO_C] - porta_note_start_q16[DCO_C];

          int64_t curNoteA_q16 = (int64_t)porta_note_start_q16[DCO_A] + (int64_t)porta_note_step_q16[DCO_A] * (int64_t)elapsed_us;
          int64_t curNoteB_q16 = (int64_t)porta_note_start_q16[DCO_B] + (int64_t)porta_note_step_q16[DCO_B] * (int64_t)elapsed_us;
          int64_t curNoteC_q16 = (int64_t)porta_note_start_q16[DCO_C] + (int64_t)porta_note_step_q16[DCO_C] * (int64_t)elapsed_us;

          // Clamp when passing the target
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

          curA = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_A]);
          curB = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_B]);
          curC = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_C]);
        }

        portamento_cur_freq_q24[DCO_A] = curA;
        portamento_cur_freq_q24[DCO_B] = curB;
        portamento_cur_freq_q24[DCO_C] = curC;

        // If the portamento time or mode control changed while gliding, retime the glide
        // from the *current* position so there is no pitch jump, only a change
        // in glide speed / curve.
        if (portaTimeChanged || portaModeChanged) {
          int32_t T = (portaTime == 0) ? 1 : (int32_t)portaTime;

          portamentoStartMicros[i] = now_us;
          portamentoTimer[i] = 0;

          if (portaMode == PORTA_MODE_TIME) {
            // Recompute time-based glide from current frequency.
            int64_t targetA = sNotePitches_q24[note1];
            int64_t targetB = sNotePitches_q24[note2];
            int64_t targetC = sNotePitches_q24[note3];

            portamento_start_q24[DCO_A] = curA;
            portamento_start_q24[DCO_B] = curB;
            portamento_start_q24[DCO_C] = curC;
            portamento_stop_q24[DCO_A] = targetA;
            portamento_stop_q24[DCO_B] = targetB;
            portamento_stop_q24[DCO_C] = targetC;

            int64_t dA = targetA - curA;
            int64_t dB = targetB - curB;
            int64_t dC = targetC - curC;
            int64_t halfT = (int64_t)T >> 1;
            int64_t numA = (dA >= 0) ? (dA + halfT) : (dA - halfT);
            int64_t numB = (dB >= 0) ? (dB + halfT) : (dB - halfT);
            int64_t numC = (dC >= 0) ? (dC + halfT) : (dC - halfT);
            freqPortaStep_q24[DCO_A] = (numA / (int64_t)T);
            freqPortaStep_q24[DCO_B] = (numB / (int64_t)T);
            freqPortaStep_q24[DCO_C] = (numC / (int64_t)T);
          } else {
            // Recompute slew-rate glide from current note position.
            int32_t currentNoteA_q16 = porta_note_cur_q16[DCO_A];
            int32_t currentNoteB_q16 = porta_note_cur_q16[DCO_B];
            int32_t currentNoteC_q16 = porta_note_cur_q16[DCO_C];
            int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
            int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
            int32_t targetNoteC_q16 = ((int32_t)note3) << 16;

            porta_note_start_q16[DCO_A] = currentNoteA_q16;
            porta_note_start_q16[DCO_B] = currentNoteB_q16;
            porta_note_start_q16[DCO_C] = currentNoteC_q16;
            porta_note_stop_q16[DCO_A] = targetNoteA_q16;
            porta_note_stop_q16[DCO_B] = targetNoteB_q16;
            porta_note_stop_q16[DCO_C] = targetNoteC_q16;

            int32_t dNoteA_q16 = porta_note_stop_q16[DCO_A] - porta_note_start_q16[DCO_A];
            int32_t dNoteB_q16 = porta_note_stop_q16[DCO_B] - porta_note_start_q16[DCO_B];
            int32_t dNoteC_q16 = porta_note_stop_q16[DCO_C] - porta_note_start_q16[DCO_C];

            int64_t halfT = (int64_t)T >> 1;
            int64_t numA = (dNoteA_q16 >= 0) ? ((int64_t)dNoteA_q16 + halfT) : ((int64_t)dNoteA_q16 - halfT);
            int64_t numB = (dNoteB_q16 >= 0) ? ((int64_t)dNoteB_q16 + halfT) : ((int64_t)dNoteB_q16 - halfT);
            int64_t numC = (dNoteC_q16 >= 0) ? ((int64_t)dNoteC_q16 + halfT) : ((int64_t)dNoteC_q16 - halfT);
            porta_note_step_q16[DCO_A] = (int32_t)(numA / (int64_t)T);
            porta_note_step_q16[DCO_B] = (int32_t)(numB / (int64_t)T);
            porta_note_step_q16[DCO_C] = (int32_t)(numC / (int64_t)T);

            if (dNoteA_q16 != 0 && porta_note_step_q16[DCO_A] == 0) {
              porta_note_step_q16[DCO_A] = (dNoteA_q16 > 0) ? 1 : -1;
            }
            if (dNoteB_q16 != 0 && porta_note_step_q16[DCO_B] == 0) {
              porta_note_step_q16[DCO_B] = (dNoteB_q16 > 0) ? 1 : -1;
            }
            if (dNoteC_q16 != 0 && porta_note_step_q16[DCO_C] == 0) {
              porta_note_step_q16[DCO_C] = (dNoteC_q16 > 0) ? 1 : -1;
            }
          }
        }
      } else {
        portamento_cur_freq_q24[DCO_A] = sNotePitches_q24[note1];
        portamento_start_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
        portamento_stop_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];

        portamento_cur_freq_q24[DCO_B] = sNotePitches_q24[note2];
        portamento_start_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
        portamento_stop_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];

        portamento_cur_freq_q24[DCO_C] = sNotePitches_q24[note3];
        portamento_start_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
        portamento_stop_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
      }

#if DCO_DEBUG_REPORT
      // Debug: OSC1 base frequency after portamento, before modifiers (in Hz)
      dbg_freq_base_Hz = (float)portamento_cur_freq_q24[DCO_A] / (float)(1 << 24);
#endif
#ifdef RUNNING_AVERAGE
      ra_portamento.addValue((float)(micros() - t_portamento));
#endif
      ////***********************    PORTAMENTO CODE  END    ****************************************/////

#ifdef RUNNING_AVERAGE
      unsigned long t_adsr = micros();
#endif
      // Fixed-point ADSR modifier in Q24: ((linToLog * ADSR1toDETUNE1) / 1080000)
      int64_t ADSRModifier_q24 = 0;
      if (ADSR1toDETUNE1 != 0) {
        // Use precomputed Q24 scale: ADSR1toDETUNE1_scale_q24 = round(ADSR1toDETUNE1 * 2^24 / 1080000)
        ADSRModifier_q24 = (int64_t)linToLogLookup[ADSR1Level[i]] * (int32_t)ADSR1toDETUNE1_scale_q24;
      }
      // ADSR3→pitch select:
      //   0 = OSC1, 1 = OSC2, 2 = OSC1+OSC2 (legacy), 3 = OSC3, 4 = all three
      int64_t ADSRModifierOSC1_q24 = (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
      int64_t ADSRModifierOSC2_q24 = (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
      int64_t ADSRModifierOSC3_q24 = (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
#ifdef RUNNING_AVERAGE
      ra_adsr_modifier.addValue((float)(micros() - t_adsr));
      unsigned long t_unison = micros();
#endif

      // Fixed-point unison modifier in Q24: 0.00006 * unisonDetune * step
      static constexpr int32_t UNISON_SCALE_Q24 = (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
      // Per-osc spread for monosynth (voice i always 0 today): OSC1=0, OSC2=+1, OSC3=-1.
      // When NUM_VOICES_TOTAL > 1, also apply classic voice-indexed alternating pattern.
      int32_t voiceMag = (i >> 1) + 1;
      int32_t voiceSign = ((i & 0x01) == 0) ? 1 : -1;
      int32_t voiceUnisonStep = voiceSign * voiceMag;
      static constexpr int32_t OSC_UNISON_STEP[3] = { 0, 1, -1 };
      int64_t unisonMODIFIER_q24 = (int64_t)unisonDetune * (int64_t)UNISON_SCALE_Q24 * (int64_t)voiceUnisonStep;
      int64_t unisonMODIFIER_OSC1_q24 = unisonMODIFIER_q24 + (int64_t)unisonDetune * (int64_t)UNISON_SCALE_Q24 * (int64_t)OSC_UNISON_STEP[0];
      int64_t unisonMODIFIER_OSC2_q24 = unisonMODIFIER_q24 + (int64_t)unisonDetune * (int64_t)UNISON_SCALE_Q24 * (int64_t)OSC_UNISON_STEP[1];
      int64_t unisonMODIFIER_OSC3_q24 = unisonMODIFIER_q24 + (int64_t)unisonDetune * (int64_t)UNISON_SCALE_Q24 * (int64_t)OSC_UNISON_STEP[2];
#ifdef RUNNING_AVERAGE
      ra_unison_modifier.addValue((float)(micros() - t_unison));
      unsigned long t_drift = micros();
#endif

      // Fixed-point drift modifiers in Q24: LFO_LEVEL * (0.0000005 * analogDrift)
      static constexpr int32_t DRIFT_UNIT_Q24 = (int32_t)(0.0000005f * (float)(1 << 24) + 0.5f);
      int32_t driftScale_q24 = (int32_t)((int32_t)analogDrift * DRIFT_UNIT_Q24);
      int64_t DETUNE_DRIFT_OSC1_q24 = (analogDrift != 0) ? ((int64_t)LFO_DRIFT_LEVEL[DCO_A] * (int64_t)driftScale_q24) : 0;
      int64_t DETUNE_DRIFT_OSC2_q24 = (analogDrift != 0) ? ((int64_t)LFO_DRIFT_LEVEL[DCO_B] * (int64_t)driftScale_q24) : 0;
      int64_t DETUNE_DRIFT_OSC3_q24 = (analogDrift != 0) ? ((int64_t)LFO_DRIFT_LEVEL[DCO_C] * (int64_t)driftScale_q24) : 0;
#ifdef RUNNING_AVERAGE
      ra_drift_multiplier.addValue((float)(micros() - t_drift));
#endif

#ifdef RUNNING_AVERAGE
      unsigned long t_modifiers = micros();
#endif
      // Combine modifiers in Q24 (faithful to original float path)
      int32_t detune_fifo_q24 = DETUNE_INTERNAL_FIFO_q24;

      // 1.00001f in Q24 (epsilon ≈ 168 LSBs)
      // Fixed-point equivalent of:
      //   modifiersAll = DETUNE_INTERNAL_FIFO_float + unisonMODIFIER + calcPitchbend + 1.00001f;
      // Unison is applied per-osc below; shared part is LFO1 FIFO + pitchbend + epsilon.
      int64_t modifiersBase_q24 =
        (int64_t)detune_fifo_q24 + (int64_t)calcPitchbend_q24 + (int64_t)Q24_ONE_EPS;
      int64_t freqModifiers_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + unisonMODIFIER_OSC1_q24;
      int64_t freq2Modifiers_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + unisonMODIFIER_OSC2_q24 + (int64_t)DETUNE_INTERNAL2_q24;
      int64_t freq3Modifiers_q24 = ADSRModifierOSC3_q24 + DETUNE_DRIFT_OSC3_q24 + modifiersBase_q24 + unisonMODIFIER_OSC3_q24 + (int64_t)DETUNE_INTERNAL3_q24;
#ifdef RUNNING_AVERAGE
      ra_modifiers_combination.addValue((float)(micros() - t_modifiers));
      unsigned long t_freq_scaling_x = micros();
#endif



      // Fast fixed-point equivalent of:
      //   freq  *= interpolatePitchMultiplier(freqModifiers)/multiplierTableScale;
      //   freq2 *= OSC2_detune * interpolatePitchMultiplier(freq2Modifiers)/multiplierTableScale;
      //   freq3 *= OSC3_detune * interpolatePitchMultiplier(freq3Modifiers)/multiplierTableScale;
      // High-resolution fixed-point x with truncation toward zero (matches original float cast):
      // xQ16 = trunc((q24 * scale) / 2^8) to carry 16 fractional bits of table-units
      int64_t x1_q24s = (freqModifiers_q24 * (int64_t)multiplierTableScale);   // Q24 * int -> Q24
      int64_t x2_q24s = (freq2Modifiers_q24 * (int64_t)multiplierTableScale);  // Q24 * int -> Q24
      int64_t x3_q24s = (freq3Modifiers_q24 * (int64_t)multiplierTableScale);
      int32_t xScaled1_Q16 = (x1_q24s >= 0) ? (int32_t)(x1_q24s >> 8) : (int32_t)(-((-x1_q24s) >> 8));
      int32_t xScaled2_Q16 = (x2_q24s >= 0) ? (int32_t)(x2_q24s >> 8) : (int32_t)(-((-x2_q24s) >> 8));
      int32_t xScaled3_Q16 = (x3_q24s >= 0) ? (int32_t)(x3_q24s >> 8) : (int32_t)(-((-x3_q24s) >> 8));

#ifdef RUNNING_AVERAGE
      ra_freq_scaling_x.addValue((float)(micros() - t_freq_scaling_x));
      unsigned long t_freq_scaling_ratio = micros();
#endif

#if PITCH_USE_RATIO_Q16
      int32_t ratio1_Q16 = interpolateRatioQ16_cached(xScaled1_Q16, DCO_A);
      int32_t ratio2_Q16 = interpolateRatioQ16_cached(xScaled2_Q16, DCO_B);
      int32_t ratio3_Q16 = interpolateRatioQ16_cached(xScaled3_Q16, DCO_C);
#ifdef RUNNING_AVERAGE
      ra_freq_scaling_ratio.addValue((float)(micros() - t_freq_scaling_ratio));
      unsigned long t_freq_scaling_post = micros();
#endif

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
#else
#ifdef RUNNING_AVERAGE
      ra_freq_scaling_ratio.addValue((float)(micros() - t_freq_scaling_ratio));
      unsigned long t_freq_scaling_post = micros();
#endif

      int32_t yTab1 = interpolatePitchMultiplierIntQ16_cached(xScaled1_Q16, DCO_A);
      int32_t yTab2 = interpolatePitchMultiplierIntQ16_cached(xScaled2_Q16, DCO_B);
      int32_t yTab3 = interpolatePitchMultiplierIntQ16_cached(xScaled3_Q16, DCO_C);
      // Convert yTab -> ratioQ16 using reciprocal-multiply (round((yTab<<16)/10000))
      uint64_t numA = ((uint64_t)(uint32_t)yTab1 << 16) + 5000u;
      int32_t ratio1_Q16_fallback = (int32_t)((numA * 0xD1B71759ULL) >> 45);
      uint64_t numB = ((uint64_t)(uint32_t)yTab2 << 16) + 5000u;
      int32_t ratio2_Q16_fallback = (int32_t)((numB * 0xD1B71759ULL) >> 45);
      uint64_t numC = ((uint64_t)(uint32_t)yTab3 << 16) + 5000u;
      int32_t ratio3_Q16_fallback = (int32_t)((numC * 0xD1B71759ULL) >> 45);
      // Scale A with ratioQ16
      freq_q24_A = (portamento_cur_freq_q24[DCO_A] * (int64_t)ratio1_Q16_fallback) >> 16;
      // Combine OSC2 ratio with detune into one Q16 factor
      int32_t detune_Q16_fb = (int32_t)((((int64_t)detune_q24) + 128) >> 8);
      int32_t combined_Q16_fb = (int32_t)((((int64_t)ratio2_Q16_fallback * (int64_t)detune_Q16_fb) + (1LL << 15)) >> 16);
      freq_q24_B = (portamento_cur_freq_q24[DCO_B] * (int64_t)combined_Q16_fb) >> 16;
      // Combine OSC3 ratio with detune into one Q16 factor
      int32_t detune3_Q16_fb = (int32_t)((((int64_t)detune3_q24) + 128) >> 8);
      int32_t combined3_Q16_fb = (int32_t)((((int64_t)ratio3_Q16_fallback * (int64_t)detune3_Q16_fb) + (1LL << 15)) >> 16);
      freq_q24_C = (portamento_cur_freq_q24[DCO_C] * (int64_t)combined3_Q16_fb) >> 16;
#endif

#if DCO_DEBUG_REPORT
      // Debug: OSC1 frequency after all modifiers applied (in Hz)
      dbg_freq_after_mod_Hz = (float)freq_q24_A / (float)(1 << 24);
#endif


      // Per-cycle: no caching; compute dividers directly from current Q18 frequency

#ifdef RUNNING_AVERAGE
      ra_freq_scaling_post.addValue((float)(micros() - t_freq_scaling_post));
#endif
      // Convert from Q24 fixed-point to a compact Q4 (Hz * 2^4) representation.
      // freq_q24_X is Hz * 2^24, so shifting right by 20 yields Hz * 2^4.
      uint32_t freqA_Q4 = (uint32_t)((freq_q24_A + (1LL << 19)) >> 20);  // round to nearest
      uint32_t freqB_Q4 = (uint32_t)((freq_q24_B + (1LL << 19)) >> 20);  // round to nearest
      uint32_t freqC_Q4 = (uint32_t)((freq_q24_C + (1LL << 19)) >> 20);
      if (freqA_Q4 == 0) freqA_Q4 = 1;
      if (freqB_Q4 == 0) freqB_Q4 = 1;
      if (freqC_Q4 == 0) freqC_Q4 = 1;

      uint8_t pioNumberA = VOICE_TO_PIO[DCO_A];
      uint8_t pioNumberB = VOICE_TO_PIO[DCO_B];
      uint8_t pioNumberC = VOICE_TO_PIO[DCO_C];
      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t smAN = VOICE_TO_SM[DCO_A];
      uint8_t smBN = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      // voice_task_3_time = micros() - voice_task_start_time;

#ifdef RUNNING_AVERAGE
      unsigned long t_clk_div = micros();
#endif

      register uint32_t clk_div1, clk_div2, clk_div3;

      uint8_t arbitrary_measured_correction_value = 0; // 60 is a measured correction for the PIO
      
      uint32_t phaseDelay = 0;

      uint32_t total_cycles1, total_cycles2, total_cycles3;

#if HIGH_PRECISION_CLKDIV
      // High-precision path: use full Q24 frequency with 64-bit intermediate divide.
      if (freq_q24_A > 0) {
        uint64_t num1 = ((uint64_t)sysClock_Hz << 24) + (uint64_t)(freq_q24_A / 2);
        total_cycles1 = (uint32_t)(num1 / (uint64_t)freq_q24_A);
      } else {
        total_cycles1 = 0;
      }

      if (freq_q24_B > 0) {
        uint64_t num2 = ((uint64_t)sysClock_Hz << 24) + (uint64_t)(freq_q24_B / 2);
        total_cycles2 = (uint32_t)(num2 / (uint64_t)freq_q24_B);
      } else {
        total_cycles2 = 0;
      }

      if (freq_q24_C > 0) {
        uint64_t num3 = ((uint64_t)sysClock_Hz << 24) + (uint64_t)(freq_q24_C / 2);
        total_cycles3 = (uint32_t)(num3 / (uint64_t)freq_q24_C);
      } else {
        total_cycles3 = 0;
      }
#else
      // --- Oscillator 1: Fixed-point Calculation (no float / 64-bit divide) ---
      // freqA_Q4 represents Hz * 2^4, so multiply sysClock_Hz by 2^4 and divide.
      total_cycles1 = (sysClock_Hz * 16u + (freqA_Q4 / 2u)) / freqA_Q4;  // rounded

      // --- Oscillator 2: Fixed-point Calculation (no float / 64-bit divide) ---
      total_cycles2 = (sysClock_Hz * 16u + (freqB_Q4 / 2u)) / freqB_Q4;  // rounded

      // --- Oscillator 3: Fixed-point Calculation (no float / 64-bit divide) ---
      total_cycles3 = (sysClock_Hz * 16u + (freqC_Q4 / 2u)) / freqC_Q4;
#endif

      // Use rounded division when computing clk_div to minimise bias.
      uint32_t total_osr_val1 = total_cycles1 - T_HIGH_TOTAL_CYCLES - T_LOW_OVERHEAD_CYCLES + arbitrary_measured_correction_value;  
      clk_div1 = (total_osr_val1 + (NUM_OSR_CHUNKS / 2u)) / NUM_OSR_CHUNKS;

      // 1. Calculate the dynamic phase and high period on EVERY call.
      //    Use a single high-precision multiply/divide to avoid compounding
      //    rounding error from per-degree quantisation.
      // Phase align applies to OSC2 only (OSC1↔OSC2 sync); OSC3 is free-running.
      if (oscSync > 1 && phaseAlignOSC2 != 0) {
        // phaseDelay ~= total_cycles2 * phaseAlignOSC2 / 360
        uint64_t phase_num = (uint64_t)total_cycles2 * (uint64_t)phaseAlignOSC2;
        phaseDelay = (uint32_t)((phase_num + 180u) / 360u);
      } else {
        phaseDelay = 0;
      }
      uint32_t y_val2 = pioPulseLength + phaseDelay;
      uint32_t high_total_cycles2 = y_val2 + T_HIGH_OVERHEAD_CYCLES;

      // 2. Calculate the low period using the CORRECT, potentially phase-delayed high period.
      //    This is the critical fix.
      uint32_t total_osr_val2 = total_cycles2 - high_total_cycles2 - T_LOW_OVERHEAD_CYCLES + arbitrary_measured_correction_value;
      clk_div2 = (total_osr_val2 + (NUM_OSR_CHUNKS / 2u)) / NUM_OSR_CHUNKS;

      uint32_t total_osr_val3 = total_cycles3 - T_HIGH_TOTAL_CYCLES - T_LOW_OVERHEAD_CYCLES + arbitrary_measured_correction_value;
      clk_div3 = (total_osr_val3 + (NUM_OSR_CHUNKS / 2u)) / NUM_OSR_CHUNKS;

#ifdef RUNNING_AVERAGE
      ra_clk_div_calc.addValue((float)(micros() - t_clk_div));
#endif


#ifdef RUNNING_AVERAGE
      unsigned long t_chan_level = micros();
#endif

      uint16_t chanLevel, chanLevel2, chanLevel3;

      // Derive Q16 from Q24 for amp-comp, then to Hz*2^FREQ_FRAC_BITS (get_chan_level does not need higher precision)
      int32_t freq_q16_A = (int32_t)((freq_q24_A + (1LL << 7)) >> 8);
      int32_t freq_q16_B = (int32_t)((freq_q24_B + (1LL << 7)) >> 8);
      int32_t freq_q16_C = (int32_t)((freq_q24_C + (1LL << 7)) >> 8);
      const int Q16_TO_FREQ_SHIFT = (16 - FREQ_FRAC_BITS);
      int32_t freqFx_A = (freq_q16_A >= 0) ? (freq_q16_A >> Q16_TO_FREQ_SHIFT)
                                           : -((-freq_q16_A) >> Q16_TO_FREQ_SHIFT);
      int32_t freqFx_B = (freq_q16_B >= 0) ? (freq_q16_B >> Q16_TO_FREQ_SHIFT)
                                           : -((-freq_q16_B) >> Q16_TO_FREQ_SHIFT);
      int32_t freqFx_C = (freq_q16_C >= 0) ? (freq_q16_C >> Q16_TO_FREQ_SHIFT)
                                           : -((-freq_q16_C) >> Q16_TO_FREQ_SHIFT);
      switch (syncMode) {
        case 0:
          chanLevel = get_chan_level_lookup_fast(freqFx_A, DCO_A);
          chanLevel2 = get_chan_level_lookup_fast(freqFx_B, DCO_B);
          chanLevel3 = get_chan_level_lookup_fast(freqFx_C, DCO_C);
          break;
        case 1:
          chanLevel = get_chan_level_lookup_fast((freqFx_A > freqFx_B ? freqFx_A : freqFx_B), DCO_A);
          chanLevel2 = get_chan_level_lookup_fast(freqFx_B, DCO_B);
          chanLevel3 = get_chan_level_lookup_fast(freqFx_C, DCO_C);
          break;
        case 2:
          chanLevel = get_chan_level_lookup_fast(freqFx_A, DCO_A);
          chanLevel2 = get_chan_level_lookup_fast((freqFx_A > freqFx_B ? freqFx_A : freqFx_B), DCO_B);
          chanLevel3 = get_chan_level_lookup_fast(freqFx_C, DCO_C);
          break;
        default:
          chanLevel = get_chan_level_lookup_fast(freqFx_A, DCO_A);
          chanLevel2 = get_chan_level_lookup_fast(freqFx_B, DCO_B);
          chanLevel3 = get_chan_level_lookup_fast(freqFx_C, DCO_C);
          break;
      }
#ifdef RUNNING_AVERAGE
      ra_get_chan_level.addValue((float)(micros() - t_chan_level));
#endif

      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));

      if (note_on_flag_flag[i]) {
        // --- Reverse Calculation to find the expected output frequency ---
        uint32_t actual_total_osr_val = (clk_div1 * NUM_OSR_CHUNKS);  // This is what the PIO actually gets
        uint32_t actual_total_period = T_HIGH_TOTAL_CYCLES + actual_total_osr_val + T_LOW_OVERHEAD_CYCLES;
        float expected_freq = (double)sysClock_Hz / (double)actual_total_period;

#if DCO_DEBUG_REPORT
        // --- Print Diagnostic Report ---
        Serial.println("----------------[ DCO DEBUG REPORT ]----------------");
        Serial.printf("Target Freq In:   %.2f Hz\n", (float)freq_q24_A / (float)(1 << 24));
        Serial.printf("Total Cycles Calc:  %lu (Target for the whole period)\n", total_cycles1);
        Serial.printf("High Period Fixed:  %lu cycles (From constants)\n", T_HIGH_TOTAL_CYCLES);
        Serial.printf("Low Overhead Fixed: %lu cycles (From constants)\n", T_LOW_OVERHEAD_CYCLES);
        Serial.printf("Total OSR Delay:    %lu cycles (Remaining for loops)\n", total_osr_val1);
        Serial.printf("clk_div (Average):  %lu (Value sent to PIO)\n", clk_div1);
        Serial.println("---");
        Serial.printf("Actual Period Gen:  %lu cycles (High + (clk_div*%u) + Low)\n",
                      actual_total_period, (unsigned)NUM_OSR_CHUNKS);
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
        Serial.printf("  detune_fifo_q24:           %.6f\n", (double)detune_fifo_q24 / (double)(1 << 24));
        Serial.printf("  unisonMODIFIER_q24:        %.6f\n", (double)unisonMODIFIER_q24 / (double)(1 << 24));
        Serial.printf("  pitchbend_q24:             %.6f\n", (double)calcPitchbend_q24 / (double)(1 << 24));
        Serial.printf("  Q24_ONE_EPS:               %.6f\n", (double)Q24_ONE_EPS / (double)(1 << 24));
        Serial.printf("  modifiersAll_q24:          %.6f\n", (double)modifiersAll_q24 / (double)(1 << 24));
        Serial.printf("  freqModifiers_q24:         %.6f\n", (double)freqModifiers_q24 / (double)(1 << 24));
        Serial.println("---");

        Serial.println("OSC1 Multiplier Table Inputs:");
        Serial.printf("  x1_q24s (table-units*Q24): %.6f\n", (double)x1_q24s / (double)(1 << 24));
        Serial.printf("  xScaled1_Q16:              %ld (int)\n", (long)xScaled1_Q16);
        Serial.printf("  ratio1_Q16:                %.6f\n", (double)ratio1_Q16 / (double)(1 << 16));
        Serial.println("----------------------------------------------------\n");

#endif

        if (oscSync == 1) {
          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(10 + offset[pioNumberA]));  // OSC Sync MODE
          pio_sm_exec(pioN_B, smBN, pio_encode_jmp(10 + offset[pioNumberB]));
        }

        if (oscSync > 1) {
          // OSC1/OSC2 live on different PIO blocks — enable/disable each SM separately.
          pio_sm_set_enabled(pioN_A, smAN, false);
          pio_sm_set_enabled(pioN_B, smBN, false);

          pio_sm_clear_fifos(pioN_B, smBN);
          pio_sm_clear_fifos(pioN_A, smAN);

          pio_sm_put(pioN_B, smBN, y_val2);
          pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
          pio_sm_exec(pioN_B, smBN, pio_encode_out(pio_y, 31));

          pio_sm_put(pioN_A, smAN, clk_div1);
          pio_sm_put(pioN_B, smBN, clk_div2);
          pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, true));
          pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, true));

          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(10 + offset[pioNumberA]));  // OSC Sync MODE
          pio_sm_exec(pioN_B, smBN, pio_encode_jmp(10 + offset[pioNumberB]));

          pio_sm_set_enabled(pioN_A, smAN, true);
          pio_sm_set_enabled(pioN_B, smBN, true);
        }

        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);
      }

      if (timer99microsFlag) {
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);

        if (sqr1Status) {
#ifdef RUNNING_AVERAGE
          unsigned long t_pwm = micros();
#endif
          // Optimized: This version avoids storing large intermediate products.
          // The multiplication and shift are combined into one expression per modulator,
          // allowing the compiler to make better use of registers.
          int32_t adsr1_delta = ((int32_t)ADSR1Level[i] * local_ADSR1toPWM) >> 11;
          int32_t lfo2_delta = ((int32_t)LFO2Level * local_LFO2toPW) >> 9;
          int32_t pw_calc = (int32_t)DIV_COUNTER_PW - 1 - lfo2_delta - PW[0] + adsr1_delta;

          if (pw_calc < 0) pw_calc = 0;
          if (pw_calc > (int32_t)DIV_COUNTER_PW - 1) pw_calc = (int32_t)DIV_COUNTER_PW - 1;
          PW_PWM[i] = (uint16_t)pw_calc;
#ifdef RUNNING_AVERAGE
          ra_pwm_calculations.addValue((float)(micros() - t_pwm));
#endif
          // PW_PWM[i] = (uint16_t)constrain(DIV_COUNTER_PW - 1 - /*((float)ADSR3Level[i] * ADSR3toPWM_formula)*/ - ((float)LFO2Level * LFO2toPWM_formula) - PW /*+ RANDOMNESS1 + RANDOMNESS2*/, 0, DIV_COUNTER_PW-1);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), get_PW_level_interpolated(PW_PWM[i], i));

        } else {
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
        }
      }
    }
    note_on_flag_flag[i] = false;
  }

#ifdef RUNNING_AVERAGE
  unsigned long voice_task_duration = micros() - voice_task_start_time;
  ra_voice_task_total.addValue((float)voice_task_duration);
  if (voice_task_duration > voice_task_max_time) {
    voice_task_max_time = voice_task_duration;
  }
#endif

  // Update cached portamento parameters for next call
  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}

inline uint8_t get_free_voice_sequential() {
  uint8_t nextVoice;
  uint8_t freeVoices = 0;

  if (VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL - 1]] == 1 || VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL - 1]] == 0) {
    for (int voiceIndex = NUM_VOICES_TOTAL - 1; voiceIndex > 0; voiceIndex--) {
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
    if (VOICES[VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL - 1]] == 0) {
      nextVoice = VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL - 1];

      for (int voiceIndex = NUM_VOICES_TOTAL - 1; voiceIndex > 0; voiceIndex--) {
        VOICES_LAST_SEQUENCE[voiceIndex] = VOICES_LAST_SEQUENCE[voiceIndex - 1];
      }

      VOICES_LAST_SEQUENCE[0] = nextVoice;

      return nextVoice;
    }
  }
  if (freeVoices == 0) {
    nextVoice = VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL - 1];

    for (int voiceIndex = NUM_VOICES_TOTAL - 1; voiceIndex > 0; voiceIndex--) {
      VOICES_LAST_SEQUENCE[voiceIndex] = VOICES_LAST_SEQUENCE[voiceIndex - 1];
    }

    VOICES_LAST_SEQUENCE[0] = nextVoice;
  }
  return nextVoice;
}

inline uint8_t get_free_voice() {
  uint32_t oldest_time = millis();
  uint8_t oldest_voice = 0;

  for (int i = 0; i < NUM_VOICES_TOTAL; i++)  // REVISAR!!
  {
    uint8_t n = (NEXT_VOICE + i) % NUM_VOICES_TOTAL;

    if (VOICES[n] == 0) {
      NEXT_VOICE = (n + 1) % NUM_VOICES_TOTAL;
      return n;
    }

    if (VOICES[i] < oldest_time) {
      oldest_time = VOICES[i];
      oldest_voice = i;
    }
  }

  NEXT_VOICE = (oldest_voice + 1) % NUM_VOICES_TOTAL;
  return oldest_voice;
}

inline void setVoiceMode() {
  switch (voiceMode) {
    case 0:
      NUM_VOICES = 1;
      STACK_VOICES = 1;
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

void setSyncMode() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    uint8_t sidesetPin;
    switch (syncMode) {
      case 0:
        sidesetPin = RESET_PINS[i];
        break;
      case 1:
        // OSC2 syncs from OSC1; OSC3 free-running
        if (i == 1) {
          sidesetPin = RESET_PINS[0];
        } else {
          sidesetPin = RESET_PINS[i];
        }
        break;
      case 2:
        // OSC1 syncs from OSC2; OSC3 free-running
        if (i == 0) {
          sidesetPin = RESET_PINS[1];
        } else {
          sidesetPin = RESET_PINS[i];
        }
        break;
      default:
        sidesetPin = RESET_PINS[i];
        break;
    }

    pio_sm_set_sideset_pins(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], sidesetPin);
    pio_gpio_init(pio[VOICE_TO_PIO[i]], sidesetPin);
    pio_sm_restart(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i]);  // IS THIS NEEDED ?
  }

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

static inline uint16_t get_chan_level_fast_from_float(float freqHz, uint8_t voiceN) {
  if (freqHz <= 0.0f) return 0;
  int32_t fx = (int32_t)(freqHz * (float)(1u << FREQ_FRAC_BITS) + 0.5f);
  return get_chan_level_lookup_fast(fx, voiceN);
}

/**
 * @brief Fast amplitude compensation lookup tuned for the RP2040.
 *
 * This function is optimized for the simple, in-order pipeline of the ARM Cortex-M0+ core.
 * It replaces complex, unpredictable branching with a simple, predictable linear scan
 * to find the correct interpolation window. For small tables, this approach is often
 * faster as it avoids processor pipeline stalls. The core calculation uses the proven,
 * numerically stable fixed-point quadratic method.
 */
static inline uint16_t get_chan_level_lookup_fast(int32_t x, uint8_t voiceN) {
  static uint8_t lastWindow[NUM_OSCILLATORS] = { 0 };

  // --- 1. Load pointers to this oscillator's table rows (cache friendly) ---
  const int32_t* freqRow = ampCompFrequencyArray[voiceN];
  const int32_t* ampRow = ampCompArray[voiceN];
  const int32_t* xBaseRow = xBaseWIN[voiceN];
  const int32_t* spanRow = dxWIN[voiceN];
  const uint32_t* invRow_q28 = invDxWIN_q28[voiceN];
  const int32_t* aRow = aQWIN_fast[voiceN];
  const int32_t* bRow = bQWIN_fast[voiceN];
  const uint16_t* cRow = cQWIN[voiceN];
  const bool* plateauRow = plateauWindow[voiceN];

  // --- 2. Handle boundary conditions ---
  if (x <= freqRow[0]) return (uint16_t)ampRow[0];
  const int lastIdx = ampCompTableSize;
  if (x >= freqRow[lastIdx]) return (uint16_t)ampRow[lastIdx];

  // --- 3. Find the correct window using a cached search ---
  int window = lastWindow[voiceN];
  const int maxWindow = ampCompTableSize - 2;
  if (window > maxWindow) window = maxWindow;

  // These small loops are very fast for gliding frequencies (common case).
  while (window > 0 && x < freqRow[window]) {
    --window;
  }
  while (window < maxWindow && x > freqRow[window + 2]) {
    ++window;
  }
  lastWindow[voiceN] = (uint8_t)window;

  // --- 4. Check for and handle plateaus ---
  if (plateauRow[window] && x >= freqRow[window + 1]) {
    return (uint16_t)DIV_COUNTER;
  }

  // --- 5. Core quadratic calculation ---
  // This path is now fully branchless for maximum speed.
  int32_t dx = x - xBaseRow[window];
  const int32_t span = spanRow[window];
  if (dx < 0) dx = 0;
  if (dx > span) dx = span;

  // Calculate t in Q(T_FRAC) using a clean 64-bit multiply-shift.
  // T_FRAC is 14, matching the legacy high-precision path.
  const uint32_t inv_q28 = invRow_q28[window];
  uint32_t t_q = (uint32_t)(((uint64_t)dx * inv_q28) >> (28 - T_FRAC));

  // y(t) = a*t^2 + b*t + c
  // All intermediate math uses 64-bit to prevent overflow.
  int64_t a = aRow[window];
  int64_t b = bRow[window];
  int32_t c = cRow[window];

  // Perform the quadratic evaluation with correct scaling at each step.
  uint32_t t2 = (uint32_t)(((uint32_t)t_q * t_q) >> T_FRAC);
  int32_t term_a = (int32_t)((a * t2) >> T_FRAC);
  int32_t term_b = (int32_t)((b * t_q) >> T_FRAC);

  // Sum the terms (all are Q(T_FRAC)) and then scale back to Q0 with rounding.
  int32_t y_q = term_a + term_b + (c << T_FRAC);
  int32_t y = (y_q + (1 << (T_FRAC - 1))) >> T_FRAC;

  // --- 6. Clamp and return final value ---
  if (y < 0) y = 0;
  if (y > (int32_t)DIV_COUNTER) y = (int32_t)DIV_COUNTER;

  return (uint16_t)y;
}


// Float reference version (kept for fallback/testing)
inline uint16_t get_chan_level_lookup_float(int32_t x, uint8_t voiceN) {
  if (x <= ampCompFrequencyArray[voiceN][0]) return ampCompArray[voiceN][0];
  if (x >= ampCompFrequencyArray[voiceN][ampCompTableSize]) return ampCompArray[voiceN][ampCompTableSize];
  int low = 0, high = ampCompTableSize;
  while (low <= high) {
    int mid = (low + high) / 2;
    if (ampCompFrequencyArray[voiceN][mid] < x) low = mid + 1;
    else high = mid - 1;
  }
  int k = low - 1;
  if (k < 0) k = 0;
  if (k > ampCompTableSize - 2) k = ampCompTableSize - 2;
  float xf = (float)x / (float)(1u << FREQ_FRAC_BITS);
  float yf = (aCoeff[voiceN][k] * xf + bCoeff[voiceN][k]) * xf + cCoeff[voiceN][k];
  int32_t y = (int32_t)lrintf(yf);
  if (y < 0) y = 0;
  if (y > (int32_t)DIV_COUNTER) y = DIV_COUNTER;
  return (uint16_t)y;
}

inline uint16_t get_PW_level_interpolated(uint16_t PWval, uint8_t voiceN) {

  uint16_t chanLevel;

  if (PWval >= DIV_COUNTER) {
    chanLevel = PW_HIGH_LIMIT[voiceN];
    return chanLevel;
  } else if (PWval <= 0) {
    chanLevel = PW_LOW_LIMIT[voiceN];
    return chanLevel;
  } else {

    if (PWval >= PW_LOOKUP[1]) {
      chanLevel = map(PWval, PW_LOOKUP[1], PW_LOOKUP[2], PW_CENTER[voiceN], PW_HIGH_LIMIT[voiceN]);
      return chanLevel;
    } else {
      chanLevel = map(PWval, PW_LOOKUP[0], PW_LOOKUP[1], PW_LOW_LIMIT[voiceN], PW_CENTER[voiceN]);
      return chanLevel;
    }

    return chanLevel;
  }
}

// PER OSCILLATOR AUTOTUNE FUNCTION
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

  if (manualCalibrationFlag == true) {  // One Ocillator at a time to get correct gap

    // Stage is the oscillator index (0..NUM_OSCILLATORS-1), not a DCO4 pair index.
    uint8_t currentCalibrationOscillator = (uint8_t)manualCalibrationStage;
    if (currentCalibrationOscillator >= NUM_OSCILLATORS) {
      currentCalibrationOscillator = NUM_OSCILLATORS - 1;
    }

    // ALL AT ONCE
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
      uint8_t pioNumber = VOICE_TO_PIO[i];
      PIO pioN = pio[VOICE_TO_PIO[i]];
      uint8_t sm1N = VOICE_TO_SM[i];

      if (i != currentCalibrationOscillator) {
        uint32_t clk_div1 = 200;

        pio_sm_put(pioN, sm1N, clk_div1);
        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
        pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), 0);
      } else {
        register uint32_t clk_div1 = (uint32_t)(((float)sysClock_Hz / freq) - pioPulseLength) / NUM_OSR_CHUNKS;

        if (freq == 0)
          clk_div1 = 0;

        pio_sm_put(pioN, sm1N, clk_div1);

        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

        pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), calibrationValue);

        pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), 0);

        Serial.println((String) "currentCalibrationOscillator: " + (int)currentCalibrationOscillator + (String) "        calibrationValue: " + (int)calibrationValue);
      }
    }
  } else {

    uint8_t pioNumber = VOICE_TO_PIO[currentDCO];
    PIO pioN = pio[VOICE_TO_PIO[currentDCO]];
    uint8_t sm1N = VOICE_TO_SM[currentDCO];

    uint32_t clk_div1 = (int)((float)(eightSysClock_Hz_u - eightPioPulseLength - (freq * 500)) / freq);

    if (freq == 0)
      clk_div1 = 0;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

    switch (taskAutotuneVoiceMode) {
      case 0:
        pwm_set_chan_level(RANGE_PWM_SLICES[currentDCO], pwm_gpio_to_channel(RANGE_PINS[currentDCO]), calibrationValue);
        break;
      case 1:
        pwm_set_chan_level(RANGE_PWM_SLICES[currentDCO], pwm_gpio_to_channel(RANGE_PINS[currentDCO]), calibrationValue);
        pio_sm_exec(pioN, sm1N, pio_encode_jmp(10 + offset[pioNumber]));
        break;
      case 2:
        pwm_set_chan_level(RANGE_PWM_SLICES[currentDCO], pwm_gpio_to_channel(RANGE_PINS[currentDCO]), chanLevel);
        break;
      case 3:
        chanLevel = get_chan_level_lookup_float(freq, currentDCO);
        pwm_set_chan_level(RANGE_PWM_SLICES[currentDCO], pwm_gpio_to_channel(RANGE_PINS[currentDCO]), chanLevel);
      case 4:
        pwm_set_chan_level(RANGE_PWM_SLICES[currentDCO], pwm_gpio_to_channel(RANGE_PINS[currentDCO]), calibrationValue);
        break;
    }

    voiceFreq[currentDCO] = freq;
    //}
    // Serial.println((String) "| currentDCO: " + currentDCO + (String) " | freq: " + freq + (String) " | clk_div1: " + clk_div1 + (String) " | ampCompCalibrationVal: " + ampCompCalibrationVal);
  }
}

// Cached variant: pass DCO index to reuse last segment and avoid binary search
inline int32_t interpolatePitchMultiplierIntQ16_cached(int32_t xQ16, int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  // Clamp to bounds using integer part
  if (xInt <= xMultiplierTable[0]) {
    return yMultiplierTable[0];
  }
  if (xInt >= xMultiplierTable[multiplierTableSize - 1]) {
    return yMultiplierTable[multiplierTableSize - 1];
  }
  int low = interpSegCache[dcoIndex];
  // Validate cache; adjust locally if possible
  if (low < 0 || low > multiplierTableSize - 2 || !(xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
    // Try step toward correct segment
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (xInt >= xMultiplierTable[low + 1]) {
        while (low < multiplierTableSize - 2 && xInt >= xMultiplierTable[low + 1]) low++;
      } else if (xInt < xMultiplierTable[low]) {
        while (low > 0 && xInt < xMultiplierTable[low]) low--;
      }
    }
    // If still wrong, do binary search
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
  int32_t slope = slopeQ20[low];
#ifdef PITCH_INTERP_USE_Q8
  // 32-bit friendly path: slope in Q8, delta in Q8; total 16 frac bits
  int32_t deltaQ8 = (xQ16 - (x0 << 16)) >> 8;
  int32_t slope8 = slopeQ8[low];
  // Product is Q16; shift by 16 to return table units (with rounding)
  int32_t y = y0 + (int32_t)((((int64_t)deltaQ8 * (int64_t)slope8) + (1LL << 15)) >> 16);
#elif defined(PITCH_INTERP_USE_Q12)
  // Medium-precision path: slope in Q12, delta in Q12; total 24 frac bits
  int32_t deltaQ12 = (xQ16 - (x0 << 16)) >> 4;
  int32_t slope12 = slopeQ12[low];
  int32_t y = y0 + (int32_t)((((int64_t)deltaQ12 * (int64_t)slope12) + (1LL << 23)) >> 24);
#else
  // High-precision path: slope in Q20, delta in Q16
  int32_t deltaQ16 = xQ16 - (x0 << 16);
  int32_t y = y0 + (int32_t)((((int64_t)deltaQ16 * (int64_t)slope) + (1LL << 35)) >> 36);
#endif
  return y;
}

// Cached Q16 ratio interpolator: returns multiplier ratio in Q16 without divide
inline int32_t interpolateRatioQ16_cached(int32_t xQ16, int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  // Clamp to bounds using integer part
  if (xInt <= xMultiplierTable[0]) {
    // Convert table y->Q16 ratio with rounding using reciprocal-multiply (n/10000 ≈ (n * M) >> 45)
    uint64_t num0 = ((uint64_t)(uint32_t)yMultiplierTable[0] << 16) + 5000u;
    return (int32_t)((num0 * 0xD1B71759ULL) >> 45);
  }
  if (xInt >= xMultiplierTable[multiplierTableSize - 1]) {
    uint64_t numN = ((uint64_t)(uint32_t)yMultiplierTable[multiplierTableSize - 1] << 16) + 5000u;
    return (int32_t)((numN * 0xD1B71759ULL) >> 45);
  }
  int low = interpSegCache[dcoIndex];
  // Validate cache; adjust locally if possible
  if (low < 0 || low > multiplierTableSize - 2 || !(xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
    // Try step toward correct segment
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (xInt >= xMultiplierTable[low + 1]) {
        while (low < multiplierTableSize - 2 && xInt >= xMultiplierTable[low + 1]) low++;
      } else if (xInt < xMultiplierTable[low]) {
        while (low > 0 && xInt < xMultiplierTable[low]) low--;
      }
    }
    // If still wrong, do binary search
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
  // Interpolate y in table units using high-precision slope (same as IntQ16 path)
  int32_t x0 = xMultiplierTable[low];
  int32_t y0 = yMultiplierTable[low];
  int32_t slope = slopeQ20[low];
  int32_t deltaQ16 = xQ16 - (x0 << 16);
  int32_t yTab = y0 + (int32_t)((((int64_t)deltaQ16 * (int64_t)slope) + (1LL << 35)) >> 36);
  // Convert y (table units) to ratio Q16 with rounding using reciprocal-multiply
  uint64_t num = ((uint64_t)(uint32_t)yTab << 16) + 5000u;  // scale/2
  int32_t ratioQ16 = (int32_t)((num * 0xD1B71759ULL) >> 45);
  return ratioQ16;
}
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
      x = 3;
      y_value = 4;
    } else {
      x = (-1.00d + (fraction * (double)i));

      y_value = (expInterpolationSolveY(x + 1.00d, 1.00d, 3.00d, 0.50d, 2.00d));
    }

    xMultiplierTable[i] = (int32_t)(x * (double)multiplierTableScale);
    yMultiplierTable[i] = (int32_t)(y_value * (double)multiplierTableScale);
    x0Q16_tbl[i] = xMultiplierTable[i] << 16;
  }
  // Precompute slopes for fast integer interpolation
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
    int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
    if (dx == 0) dx = 1;
    // Precompute slope in Q20 for fast multiply-only interpolation
    int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
    int64_t numSlope = ((int64_t)dy << 20) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ20[i] = (int32_t)(numSlope / (int64_t)dx);
#ifdef PITCH_INTERP_USE_Q8
    // Optional lower-precision slope for 32-bit fast path
    int64_t numSlope8 = ((int64_t)dy << 8) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ8[i] = (int32_t)(numSlope8 / (int64_t)dx);
#endif
#ifdef PITCH_INTERP_USE_Q12
    // Medium-precision slope for balanced speed/accuracy
    int64_t numSlope12 = ((int64_t)dy << 12) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ12[i] = (int32_t)(numSlope12 / (int64_t)dx);
#endif
  }
  // Initialize per-DCO cache to invalid
  for (int d = 0; d < NUM_OSCILLATORS; ++d) interpSegCache[d] = -1;
}

#ifdef RUNNING_AVERAGE
void print_voice_task_timings() {
  Serial.println("\n=== VOICE_TASK TIMING STATISTICS (microseconds) ===");
  Serial.print("Pitch Bend Calc:      ");
  if (ra_pitchbend.getCount() > 0) Serial.println(ra_pitchbend.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("OSC2 Detune:          ");
  if (ra_osc2_detune.getCount() > 0) Serial.println(ra_osc2_detune.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Portamento:           ");
  if (ra_portamento.getCount() > 0) Serial.println(ra_portamento.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("ADSR Modifier:        ");
  if (ra_adsr_modifier.getCount() > 0) Serial.println(ra_adsr_modifier.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Unison Modifier:      ");
  if (ra_unison_modifier.getCount() > 0) Serial.println(ra_unison_modifier.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Drift Modifier:       ");
  if (ra_drift_multiplier.getCount() > 0) Serial.println(ra_drift_multiplier.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Modifiers Combination:");
  if (ra_modifiers_combination.getCount() > 0) Serial.println(ra_modifiers_combination.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Freq Scaling x:       ");
  if (ra_freq_scaling_x.getCount() > 0) Serial.println(ra_freq_scaling_x.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Freq Scaling ratio:   ");
  if (ra_freq_scaling_ratio.getCount() > 0) Serial.println(ra_freq_scaling_ratio.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Freq Scaling post:    ");
  if (ra_freq_scaling_post.getCount() > 0) Serial.println(ra_freq_scaling_post.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Get Chan Level:       ");
  if (ra_get_chan_level.getCount() > 0) Serial.println(ra_get_chan_level.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Clock Div Calc:       ");
  if (ra_clk_div_calc.getCount() > 0) Serial.println(ra_clk_div_calc.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("PWM Calculations:     ");
  if (ra_pwm_calculations.getCount() > 0) Serial.println(ra_pwm_calculations.getFastAverage(), 2);
  else Serial.println("N/A");

  Serial.print("Voice Task Total:     ");
  Serial.print(ra_voice_task_total.getFastAverage(), 2);
  Serial.print(" avg, max ");
  Serial.println(voice_task_max_time);

  Serial.println("===================================================\n");
}
#endif

