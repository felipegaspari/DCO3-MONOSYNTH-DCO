// Removed unused code from voices.ino

// --- from original:840-1003 ---

inline void voice_task_simple() {
  for (int i = 0; i < NUM_VOICES; i++) {

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

      if (OSC2DetuneVal == 256) {
        OSC2_detune = 1;
      } else {
        OSC2_detune = 1.00f + (0.0002f * ((int)256 - OSC2DetuneVal));
      }

      float freq;
      float freq2;
      float freq3;

      // Fixed osc indices for current mono hardware (3 oscs on voice 0).
      // Future paraphonic mode can remap osc ownership per voice without gutting allocation.
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;

      freq = sNotePitches[note1];
      freq2 = sNotePitches[note2];
      freq3 = sNotePitches[note3];

      // Serial.println("VOICE TASK 2");

      if ((uint16_t)freq > maxFrequency) {
        freq = maxFrequency;
      } else if ((uint16_t)freq < 6) {
        freq = 6;
      }
      if ((uint16_t)freq2 >= maxFrequency) {
        freq2 = maxFrequency;
      } else if ((uint16_t)freq2 < 6) {
        freq2 = 6;
      }
      if ((uint16_t)freq3 >= maxFrequency) {
        freq3 = maxFrequency;
      } else if ((uint16_t)freq3 < 6) {
        freq3 = 6;
      }

      // voice_task_2_time = micros() - voice_task_start_time;

      uint8_t pioNumberA = VOICE_TO_PIO[DCO_A];
      uint8_t pioNumberB = VOICE_TO_PIO[DCO_B];
      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t smAN = VOICE_TO_SM[DCO_A];
      uint8_t smBN = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      // voice_task_3_time = micros() - voice_task_start_time;

      uint32_t clk_div1 = (uint32_t)((sysClock_Hz / freq) - pioPulseLength) / NUM_OSR_CHUNKS;
      if (freq == 0)
        clk_div1 = 0;

      uint32_t clk_div2;
      uint32_t phaseDelay;

      if (oscSync > 1) {

        clk_div2 = (uint32_t)(sysClock_Hz / freq2);
        phaseDelay = (clk_div2 - pioPulseLength) / 180 * phaseAlignOSC2;
        clk_div2 = (uint32_t)((clk_div2 - phaseDelay) / NUM_OSR_CHUNKS);
      } else {
        clk_div2 = (uint32_t)((sysClock_Hz / freq2) - pioPulseLength) / NUM_OSR_CHUNKS;
      }
      if (freq2 == 0)
        clk_div2 = 0;

      uint32_t clk_div3 = (uint32_t)((sysClock_Hz / freq3) - pioPulseLength) / NUM_OSR_CHUNKS;
      if (freq3 == 0)
        clk_div3 = 0;

      // voice_task_4_time = micros() - voice_task_start_time;

      uint16_t chanLevel, chanLevel2, chanLevel3;

      switch (syncMode) {
        case 0:
          chanLevel = get_chan_level_fast_from_float(freq, DCO_A);
          chanLevel2 = get_chan_level_fast_from_float(freq2, DCO_B);
          break;
        case 1:
          chanLevel = get_chan_level_fast_from_float(max(freq, freq2), DCO_A);
          chanLevel2 = get_chan_level_fast_from_float(freq2, DCO_B);
          break;
        case 2:
          chanLevel = get_chan_level_fast_from_float(freq, DCO_A);
          chanLevel2 = get_chan_level_fast_from_float(max(freq, freq2), DCO_B);
          break;
        default:
          chanLevel = get_chan_level_fast_from_float(freq, DCO_A);
          chanLevel2 = get_chan_level_fast_from_float(freq2, DCO_B);
          break;
      }
      chanLevel3 = get_chan_level_fast_from_float(freq3, DCO_C);

      // VCO LEVEL //uint16_t vcoLevel = get_vco_level(freq);

      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));

      // Serial.println("VOICE TASK 5a");

      if (note_on_flag_flag[i]) {
        if (oscSync > 0) {
          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(10 + offset[pioNumberA]));  // OSC Sync MODE
          pio_sm_exec(pioN_B, smBN, pio_encode_jmp(10 + offset[pioNumberB]));

          if (oscSync > 1) {
            pio_sm_put(pioN_B, smBN, pioPulseLength + phaseDelay);
            pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
            pio_sm_exec(pioN_B, smBN, pio_encode_out(pio_y, 31));
            pio_sm_exec(pioN_B, smBN, pio_encode_out(pio_x, 31));
          }
        }

        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);
      }

      if (timer99microsFlag) {
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);
      }

      if (sqr1Status) {
        pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), PW_CENTER[i]);
      } else {
        pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
      }
    }
    note_on_flag_flag[i] = false;
  }
}

// --- from original:1210-1276 ---


inline uint16_t get_chan_level_lookup(int32_t x, uint8_t voiceN) {
  static int lastSegIdx[NUM_OSCILLATORS] = { 0 };
  // If frequency is at/above max comp band, drive full-scale level
  if (x >= AMP_COMP_MAX_HZ_Q) {
    return (uint16_t)DIV_COUNTER;
  }
  // Clamp bounds
  if (x <= ampCompFrequencyArray[voiceN][0]) return ampCompArray[voiceN][0];
  if (x >= ampCompFrequencyArray[voiceN][ampCompTableSize]) return ampCompArray[voiceN][ampCompTableSize];

  // Use cached window first
  int i = lastSegIdx[voiceN];
  if (i < 0) i = 0;
  if (i > ampCompTableSize - 2) i = ampCompTableSize - 2;
  if (ampCompFrequencyArray[voiceN][i] <= x && x <= ampCompFrequencyArray[voiceN][i + 2]) {
    // ok
  } else if ((i + 1) <= (ampCompTableSize - 2) && ampCompFrequencyArray[voiceN][i + 1] <= x && x <= ampCompFrequencyArray[voiceN][i + 3]) {
    i = i + 1;
  } else if ((i - 1) >= 0 && ampCompFrequencyArray[voiceN][i - 1] <= x && x <= ampCompFrequencyArray[voiceN][i + 1]) {
    i = i - 1;
  } else {
    // Short linear scan (rare)
    for (int k = 0; k < ampCompTableSize - 1; k++) {
      if (ampCompFrequencyArray[voiceN][k] <= x && x <= ampCompFrequencyArray[voiceN][k + 2]) {
        i = k;
        break;
      }
    }
  }
  lastSegIdx[voiceN] = i;

  // Fixed-point eval in window i..i+2
  int32_t x0 = xBaseWIN[voiceN][i];
  int32_t dx02 = dxWIN[voiceN][i];
  // Instrument high window data for voice 0 (diff handler calls amp_comp_debug_window)
  const uint16_t y1_top = ampCompArray[voiceN][i + 1];
  const uint16_t y2_top = ampCompArray[voiceN][i + 2];
  const int32_t x1 = ampCompFrequencyArray[voiceN][i + 1];
  if ((y1_top >= DIV_COUNTER && y2_top >= DIV_COUNTER) && x >= x1) {
    return (uint16_t)DIV_COUNTER;
  }
  const uint16_t y0 = ampCompArray[voiceN][i];
  int32_t dx = x - x0;
  if (dx < 0) dx = 0;
  if (dx > dx02) dx = dx02;
  // Q28 reciprocal path: t_q = round((dx / dx02) * 2^T_FRAC)
  // invDxWIN_q28 = round(2^28 / dx02) precomputed per window
  const uint32_t invDx_q28 = invDxWIN_q28[voiceN][i];
  uint32_t t_q = (uint32_t)(((uint64_t)(uint32_t)dx * (uint64_t)invDx_q28) >> (28 - T_FRAC));
  if (t_q > (uint32_t)(1u << T_FRAC)) t_q = (uint32_t)(1u << T_FRAC);

  // 32-bit polynomial using aQ/bQ (Q(T_FRAC)) and t in Q(T_FRAC):
  int64_t aQ = (int64_t)aQWIN[voiceN][i];
  int64_t bQ = (int64_t)bQWIN[voiceN][i];
  int32_t cQ = (int32_t)cQWIN[voiceN][i];
  // Fast Q(T_FRAC) polynomial: compute t2 once, do one rounding at the end
  uint32_t t2 = (uint32_t)(((uint64_t)t_q * (uint64_t)t_q) >> T_FRAC);       // Q(T_FRAC)
  int32_t quadQ = (int32_t)(((aQ * (int64_t)t2) >> T_FRAC));                 // Q(T_FRAC)
  int32_t linQ = (int32_t)(((bQ * (int64_t)t_q) >> T_FRAC));                 // Q(T_FRAC)
  int32_t yQ = quadQ + linQ + (cQ << T_FRAC);                                // Q(T_FRAC)
  int32_t y = (int32_t)((((int64_t)yQ) + (1LL << (T_FRAC - 1))) >> T_FRAC);  // Q0
  if (y < 0) y = 0;
  if (y > (int32_t)DIV_COUNTER) y = DIV_COUNTER;
  return (uint16_t)y;
}

// --- from original:1416-1521 ---

void voice_task_debug() {

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
    }
  }

  last_midi_pitch_bend = midi_pitch_bend;
  LAST_DETUNE = DETUNE;
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
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

      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;

      register float freq;
      register float freq2;
      register float freq3;

      freq = (float)sNotePitches[note1];
      freq2 = (float)sNotePitches[note2];
      freq3 = (float)sNotePitches[note3];

      uint8_t pioNumberA = VOICE_TO_PIO[DCO_A];
      uint8_t pioNumberB = VOICE_TO_PIO[DCO_B];
      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t smAN = VOICE_TO_SM[DCO_A];
      uint8_t smBN = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      register uint32_t clk_div1 = (uint32_t)(((float)sysClock_Hz / freq) - pioPulseLength - 1) / NUM_OSR_CHUNKS;

      if (freq == 0)
        clk_div1 = 0;

      register uint32_t clk_div2 = (uint32_t)(((float)sysClock_Hz / freq2) - pioPulseLength - 1) / NUM_OSR_CHUNKS;
      register uint32_t clk_div3 = (uint32_t)(((float)sysClock_Hz / freq3) - pioPulseLength - 1) / NUM_OSR_CHUNKS;

      uint16_t chanLevel = get_chan_level_lookup_float((int32_t)(freq * 100), DCO_A);
      uint16_t chanLevel2 = get_chan_level_lookup_float((int32_t)(freq2 * 100), DCO_B);
      uint16_t chanLevel3 = get_chan_level_lookup_float((int32_t)(freq3 * 100), DCO_C);

      if (oscSync == 0) {

        pio_sm_put(pioN_A, smAN, clk_div1);
        pio_sm_put(pioN_B, smBN, clk_div2);
        pio_sm_put(pioN_C, smCN, clk_div3);
        pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
        pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
        pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
      } else {
        pio_sm_put(pioN_A, smAN, clk_div1);
        pio_sm_put(pioN_B, smBN, clk_div2);
        pio_sm_put(pioN_C, smCN, clk_div3);
        pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
        pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
        pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
        if (note_on_flag_flag[i]) {
          switch (oscSync) {
            case 1:
              pio_sm_exec(pioN_A, smAN, pio_encode_jmp(10 + offset[pioNumberA]));  // OSC Sync MODE
              pio_sm_exec(pioN_B, smBN, pio_encode_jmp(10 + offset[pioNumberB]));
              break;
            case 2:
              pio_sm_exec(pioN_A, smAN, pio_encode_jmp(4 + offset[pioNumberA]));  // OSC Half Sync MODE
              pio_sm_exec(pioN_B, smBN, pio_encode_jmp(12 + offset[pioNumberB]));
              break;
            case 3:
              pio_sm_exec(pioN_A, smAN, pio_encode_jmp(4 + offset[pioNumberA]));  // OSC 3rd-quarter Sync MODE
              pio_sm_exec(pioN_B, smBN, pio_encode_jmp(10 + offset[pioNumberB]));
              break;
            default:
              break;
          }
          pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
          pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
          pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);
        }
      }
      if (timer99microsFlag) {
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
        pwm_set_chan_level(RANGE_PWM_SLICES[DCO_C], pwm_gpio_to_channel(RANGE_PINS[DCO_C]), chanLevel3);
      }
    }
    note_on_flag_flag[i] = false;
  }
}

// --- from original:1750-1798 ---

static void amp_comp_debug_window(int32_t x, uint8_t voiceN) {
  int window = 0;
  for (int k = 0; k < ampCompTableSize - 2; ++k) {
    if (ampCompFrequencyArray[voiceN][k] <= x && x <= ampCompFrequencyArray[voiceN][k + 2]) {
      window = k;
      break;
    }
  }
  if (window < 0) window = 0;
  if (window > ampCompTableSize - 3) window = ampCompTableSize - 3;

  int32_t x0 = ampCompFrequencyArray[voiceN][window];
  int32_t x1 = ampCompFrequencyArray[voiceN][window + 1];
  int32_t x2 = ampCompFrequencyArray[voiceN][window + 2];
  uint16_t y0 = ampCompArray[voiceN][window];
  uint16_t y1 = ampCompArray[voiceN][window + 1];
  uint16_t y2 = ampCompArray[voiceN][window + 2];
  bool useDouble = useDoubleWindow[voiceN][window];
  double aD = aCoeffD[voiceN][window];
  double bD = bCoeffD[voiceN][window];
  double cD = cCoeffD[voiceN][window];

  Serial.print("[AMPWINDBG] v=");
  Serial.print(voiceN);
  Serial.print(" win=");
  Serial.print(window);
  Serial.print(" useDouble=");
  Serial.print(useDouble ? "1" : "0");
  Serial.print(" x0=");
  Serial.print(x0);
  Serial.print(" x1=");
  Serial.print(x1);
  Serial.print(" x2=");
  Serial.print(x2);
  Serial.print(" y0=");
  Serial.print(y0);
  Serial.print(" y1=");
  Serial.print(y1);
  Serial.print(" y2=");
  Serial.print(y2);
  Serial.print(" aD=");
  Serial.print(aD, 6);
  Serial.print(" bD=");
  Serial.print(bD, 6);
  Serial.print(" cD=");
  Serial.print(cD, 6);
  Serial.println();
}

// --- from original:1800-1926 ---
#if 0  // Legacy 2-osc reference path — not maintained for DCO3 3-osc monosynth
inline void voice_task_gold_reference() {
  for (int i = 0; i < NUM_VOICES; i++) {
    uint32_t phaseDelay;

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
    }

    const uint8_t DCO_A = 0;
    const uint8_t DCO_B = 1;
    const uint8_t DCO_C = 2;

    // --- High Precision Frequency Calculation (compatible with voice_task_simple) ---
    uint8_t note1 = VOICE_NOTES[i] - 36 + OSC1_interval;
    if (note1 > highestNote) {
      note1 -= ((uint8_t(note1 - highestNote) / 12) * 12);
    }
    long double freq_ld = sNotePitches[note1];

    // Base frequency for OSC2 is the original note, which unison will modify.
    long double freq2_ld = sNotePitches[VOICE_NOTES[i] - 36];

    // High-precision unison detune calculation from first principles.
    if (unisonDetune != 0) {
      // Unison detune steps are 0.25 semitones = 25 cents.
      long double cents_offset = (long double)unisonDetune * 0.12L;
      long double unison_ratio_ld = powl(2.0L, cents_offset / 1200.0L);
      freq2_ld *= unison_ratio_ld;
      freq_ld *= unison_ratio_ld;
    }

    uint8_t pioNumberA = VOICE_TO_PIO[DCO_A];
    uint8_t pioNumberB = VOICE_TO_PIO[DCO_B];
    PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
    PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
    uint8_t sm1N = VOICE_TO_SM[DCO_A];
    uint8_t sm2N = VOICE_TO_SM[DCO_B];

    // --- High Precision Clock Divider Calculation (Inlined) ---
    uint32_t clk_div1;
    long double raw_div1_ld = (long double)eightSysClock_Hz_u / freq_ld;
    clk_div1 = (raw_div1_ld > eightPioPulseLength) ? (uint32_t)(raw_div1_ld - eightPioPulseLength) : 0;

    uint32_t clk_div2;

    if (oscSync > 1) {
      // This path uses sysClock_Hz
      long double raw_div2_ld = (long double)sysClock_Hz / freq2_ld;
      if (raw_div2_ld > pioPulseLength) {
        long double phaseDelay_ld = (raw_div2_ld - (long double)pioPulseLength) / 180.0L * (long double)phaseAlignOSC2;
        long double clk_div2_ld = (raw_div2_ld - (long double)pioPulseLength - phaseDelay_ld) / 8.0L;
        clk_div2 = (uint32_t)clk_div2_ld;
      } else {
        clk_div2 = 0;
      }
    } else {
      // This path uses eightSysClock_Hz_u
      long double raw_div2_ld = (long double)eightSysClock_Hz_u / freq2_ld;
      clk_div2 = (raw_div2_ld > eightPioPulseLength) ? (uint32_t)(raw_div2_ld - eightPioPulseLength) : 0;
    }

    // voice_task_4_time = micros() - voice_task_start_time;

    uint16_t chanLevel, chanLevel2;

    // Convert back to Q8 for amp comp lookup
    int32_t fxA = (int32_t)(freq_ld > 0.0L ? (freq_ld * (long double)(1u << FREQ_FRAC_BITS) + 0.5L) : 0.0L);
    int32_t fxB = (int32_t)(freq2_ld > 0.0L ? (freq2_ld * (long double)(1u << FREQ_FRAC_BITS) + 0.5L) : 0.0L);

    switch (syncMode) {
      case 0:
        chanLevel = get_chan_level_lookup_fast(fxA, DCO_A);
        chanLevel2 = get_chan_level_lookup_fast(fxB, DCO_B);
        break;
      case 1:
        chanLevel = get_chan_level_lookup_fast((fxA > fxB ? fxA : fxB), DCO_A);
        chanLevel2 = get_chan_level_lookup_fast(fxB, DCO_B);
        break;
      case 2:
        chanLevel = get_chan_level_lookup_fast(fxA, DCO_A);
        chanLevel2 = get_chan_level_lookup_fast((fxA > fxB ? fxA : fxB), DCO_B);
        break;
    }

    // VCO LEVEL //uint16_t vcoLevel = get_vco_level(freq);

    pio_sm_put(pioN_A, sm1N, clk_div1);
    pio_sm_put(pioN_B, sm2N, clk_div2);
    pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, false));
    pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));

    // Serial.println("VOICE TASK 5a");

    if (note_on_flag_flag[i]) {
      if (oscSync > 0) {
        pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(10 + offset[pioNumberA]));  // OSC Sync MODE
        pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(10 + offset[pioNumberB]));

        if (oscSync > 1) {
          pio_sm_put(pioN_B, sm2N, pioPulseLength + phaseDelay);
          pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));
          pio_sm_exec(pioN_B, sm2N, pio_encode_out(pio_y, 31));
          pio_sm_exec(pioN_B, sm2N, pio_encode_out(pio_x, 31));
        }
      }

      pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
      pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
    }

    if (timer99microsFlag) {
      pwm_set_chan_level(RANGE_PWM_SLICES[DCO_A], pwm_gpio_to_channel(RANGE_PINS[DCO_A]), chanLevel);
      pwm_set_chan_level(RANGE_PWM_SLICES[DCO_B], pwm_gpio_to_channel(RANGE_PINS[DCO_B]), chanLevel2);
    }

    if (sqr1Status) {
      pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), PW_CENTER[i]);
    } else {
      pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
    }

    note_on_flag_flag[i] = false;
  }
}
#endif  // voice_task_gold_reference

// --- from voice_task ---
  LAST_DETUNE = DETUNE;
