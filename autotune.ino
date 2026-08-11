
#include "include_all.h"

// =============================================================================
// autotune.ino — DCO calibration orchestration, PW center/limit searches and
// the edge-timing duty measurement core (find_gap).
//
// The per-note amplitude-compensation search (calibrate_DCO) and its helpers
// live in autotune_search.ino.
// =============================================================================

// For debug logging and duty computation in gap measurement: track the last
// PW raw value we explicitly programmed for the current DCO, the duty
// target/period assumed by the current PW search routine, and the most
// recently measured period from find_gap().
static uint16_t g_lastPWMeasurementRaw = 0;
static double   g_gapLogCurrentPeriodUs = 0.0;
static double   g_gapLogTargetDutyFraction = 0.5;  // default 50%

// Helper: turn off all oscillators and set their RANGE outputs to a known
// state, while charging their timing capacitors using the original
// PIO+GPIO sequence. This preserves the analogue behaviour you rely on.
static void disable_all_oscillators_and_range_pwm() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    PIO     pioN      = pio[VOICE_TO_PIO[i]];
    uint8_t smN = VOICE_TO_SM[i];

    // Original "park" frequency used to pre-charge the caps.
    uint32_t clk_div1 = 200;

    // Run the DCO SM at a known slow rate while driving the RANGE PWM.
    pio_sm_set_enabled(pioN, smN, true);
    pio_sm_put(pioN, smN, clk_div1);
    pio_sm_exec(pioN, smN, pio_encode_pull(false, false));

    delay(200);

    // Stop the SM and hold the RANGE pin high as a plain GPIO output.
    pio_sm_set_enabled(pioN, smN, false);
#ifdef RANGE0_PIO_DITHER_TEST
    range_pio_set_level((uint8_t)i, DIV_COUNTER);  // full-on via PIO; do not steal RANGE pin
    continue;
#endif
    gpio_init(RANGE_PINS[i]);
    gpio_set_dir(RANGE_PINS[i], GPIO_OUT);
    gpio_put(RANGE_PINS[i], 1);
  }

  // After all RANGE caps are charged, park shared PW PWM at max wrap so the
  // centre search can start from a known state. (Matches original behaviour.)
  reset_pw_to_DIV_COUNTER_PW();
}



// Helper: park per-osc PW PWM at max wrap (DIV_COUNTER_PW). Called from disable_all_oscillators_and_range_pwm().
// Autotune PW cal still only measures osc 0; unassigned pins are skipped.
static void reset_pw_to_DIV_COUNTER_PW() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    if (PW_PINS[i] == PW_PIN_UNASSIGNED) continue;
    pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), DIV_COUNTER_PW);
  }
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/
// Main DCO amplitude-compensation calibration entry point.
// Monosynth: calibrate shared PW on voice 0 once, then for each oscillator
// run calibrate_DCO() to build a [freq -> range PWM] table and persist via update_FS_voice().
void DCO_calibration() {

  // TURN OFF ALL OSCILLATORS and park shared PW voice.
  disable_all_oscillators_and_range_pwm();

  // PW is per-voice (monosynth: voice 0 only). Calibrate once, then amp-comp per osc.
  currentDCO = 0;
  restart_DCO_calibration();
  DCO_calibration_current_note = manual_DCO_calibration_start_note;
  VOICE_NOTES[0] = DCO_calibration_current_note;
  find_PW_center(0);
  find_PW_limit_v2(PW_LIMIT_LOW);
  find_PW_limit_v2(PW_LIMIT_HIGH);
  pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), PW_CENTER[0]);
  PW[0] = PW_CENTER[0];

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    currentDCO = i;

    restart_DCO_calibration();

    ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];
    write_range_pwm(currentDCO, ampCompCalibrationVal);

    DCO_calibration_current_note = DCO_calibration_start_note;
    VOICE_NOTES[0] = DCO_calibration_current_note;

    // Build a small context for this DCO and run the calibration routine.
    DCOCalibrationContext ctx(
      currentDCO,
      DCO_calibration_current_note,
      calibrationData,
      manualCalibrationOffset,
      initManualAmpCompCalibrationVal
    );
    // Desired duty-cycle error tolerance as a fraction (e.g. 0.005 = 0.5%).
    double dutyErrorFraction = 0.001;
    calibrate_DCO(ctx, dutyErrorFraction);

    for (int j = 0; j < chanLevelVoiceDataSize; j++) {
      Serial.println(calibrationData[j]);
    }

    update_FS_voice(currentDCO);

    Serial.println((String) "DCO " + currentDCO + (String) " calibration finished.");
  }
  calibrationFlag = false;
  init_FS();

  // Rebuild amp-comp tables for the active engine.
  precompute_amp_comp_for_engine();
}
/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

// Reset per-DCO calibration state and header entries in calibrationData.
// This is called once for the PW pass and again before calibrating each DCO.
void restart_DCO_calibration() {

  VOICE_NOTES[0] = DCO_calibration_start_note;
  DCO_calibration_current_note = DCO_calibration_start_note;

  // Table header:
  //  [0..1] "lowest frequency" anchor (freq placeholder 0, PWM ampCompLowestFreqVal)
  //  [2..3] manual starting point, one interval below the first calibrated note.
  calibrationData[0] = 0;
  calibrationData[1] = ampCompLowestFreqVal;
  calibrationData[2] = (uint32_t)(note_to_freq(DCO_calibration_current_note - calibration_note_interval) * 100);
  calibrationData[3] = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  // Reference for the 60 s safety timeouts used by the PW search phases.
  DCOCalibrationStart = millis();

  // TURN OFF ALL OSCILLATORS for a clean restart, and pre-charge the
  // RANGE capacitors using the legacy helper.
  disable_all_oscillators_and_range_pwm();

  // IMPORTANT: disable_all_oscillators_and_range_pwm() leaves RANGE_PINS[]
  // as plain GPIO outputs driven HIGH. Before starting calibration for the
  // currentDCO we must restore its RANGE pin back to PWM function so that
  // voice_task_autotune() and subsequent RANGE PWM writes actually appear
  // on the physical pin.
#ifndef RANGE0_PIO_DITHER_TEST
  gpio_set_function(RANGE_PINS[currentDCO], GPIO_FUNC_PWM);
#endif

  PIO pioN = pio[VOICE_TO_PIO[currentDCO]];
  uint8_t sm1N = VOICE_TO_SM[currentDCO];
  pio_sm_set_enabled(pioN, sm1N, true);

  delay(100);
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/
// PW search — shared low-level helpers
/*************************************************************************************/

// Helper: program a PW value on the given voice, keep PW[] and the debug
// tracker in sync, wait for the waveform to settle and measure the gap.
// This replaces the "set PWM, delay, measure" blocks that used to be
// copy-pasted throughout the PW search code.
static GapMeasurement set_pw_and_measure(uint8_t voiceIdx, uint16_t pw) {
  pwm_set_chan_level(PW_PWM_SLICES[voiceIdx],
                     pwm_gpio_to_channel(PW_PINS[voiceIdx]),
                     pw);
  PW[voiceIdx]           = pw;
  g_lastPWMeasurementRaw = pw;
  delay(30);
  return measure_gap(2);
}

// PWSearchState / PWRecordMode are defined in autotune.h so the Arduino
// builder's auto-generated prototypes for these helpers can see the types.

static void pw_search_state_init(PWSearchState& st) {
  st.validCount       = 0;
  st.inToleranceCount = 0;
  st.haveBest         = false;
  st.bestGapAbs       = 1e12;
  st.bestPW           = 0;
  st.haveBracket      = false;
  st.pwLow            = 0;
  st.pwHigh           = 0;
  st.gapLow           = 0.0;
}

// Record one valid (non-timeout) measurement into the search state.
static void pw_record_sample(PWSearchState& st, uint16_t pw, double gapDiff,
                             double targetGap, PWRecordMode mode) {
  double absGapDiff = fabs(gapDiff);

  if (absGapDiff <= targetGap) {
    st.inToleranceCount++;
  }
  if (!st.haveBest || absGapDiff < st.bestGapAbs) {
    st.haveBest   = true;
    st.bestGapAbs = absGapDiff;
    st.bestPW     = pw;
  }

  if (mode == PW_RECORD_NO_TABLE) {
    return;
  }
  if (st.validCount < kPWMaxSamples) {
    st.validPW[st.validCount]      = pw;
    st.validGapDiff[st.validCount] = gapDiff;
    st.validCount++;
  } else if (mode == PW_RECORD_REPLACE_WORST) {
    int worstIdx = 0;
    double worstAbs = fabs(st.validGapDiff[0]);
    for (int vi = 1; vi < st.validCount; ++vi) {
      double curAbs = fabs(st.validGapDiff[vi]);
      if (curAbs > worstAbs) {
        worstAbs = curAbs;
        worstIdx = vi;
      }
    }
    if (absGapDiff < worstAbs) {
      st.validPW[worstIdx]      = pw;
      st.validGapDiff[worstIdx] = gapDiff;
    }
  }
}

// Phase 1: coarse scan over [pwMin, pwMax] looking for a sign-change bracket
// around the target duty. When a bracket is found, one extra sample at the
// linearly interpolated crossing point is measured and stored, then the scan
// stops.
static void pw_coarse_scan(PWSearchState& st,
                           double gapTarget, double targetGap,
                           uint16_t pwMin, uint16_t pwMax, uint16_t coarseStep,
                           double periodUs, double toleranceDutyPercent) {
  bool     havePrev    = false;
  double   prevGapDiff = 0.0;
  uint16_t prevPW      = 0;

  for (uint16_t pw = pwMin; pw <= pwMax; pw = (uint16_t)(pw + coarseStep)) {

    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW coarse scan timeout (60s)");
      break;
    }

    GapMeasurement gm = set_pw_and_measure(0, pw);
    if (gm.timedOut) {
      continue;  // no usable signal at this PW
    }

    double gap     = (double)gm.value;
    double gapDiff = gap - gapTarget;

    if (autotuneDebug >= 2 && periodUs > 0.0) {
      double dutyPercent = (0.5 + gap / (2.0 * periodUs)) * 100.0;
      Serial.println((String)"[PW_CENTER_COARSE] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW_raw=" + pw +
                     (String)" gap=" + gap +
                     (String)"us duty=" + dutyPercent +
                     (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
    }

    pw_record_sample(st, pw, gapDiff, targetGap, PW_RECORD_REPLACE_WORST);

    if (havePrev &&
        ((gapDiff > 0.0 && prevGapDiff < 0.0) || (gapDiff < 0.0 && prevGapDiff > 0.0))) {
      st.haveBracket = true;
      st.pwLow  = prevPW;
      st.gapLow = prevGapDiff + gapTarget;  // raw gap at pwLow
      st.pwHigh = pw;

      // With two samples straddling the target, probe the crossing point
      // estimated by linear interpolation between them.
      double denom = fabs(prevGapDiff) + fabs(gapDiff);
      if (denom > 0.0) {
        double t = fabs(prevGapDiff) / denom;  // weight towards the closer side
        uint16_t pwEst = (uint16_t)((double)prevPW + ((double)(pw - prevPW) * t));
        if (pwEst >= pwMin && pwEst <= pwMax) {
          GapMeasurement gmEst = set_pw_and_measure(0, pwEst);
          if (!gmEst.timedOut) {
            pw_record_sample(st, pwEst, (double)gmEst.value - gapTarget,
                             targetGap, PW_RECORD_APPEND);
          }
        }
      }
      break;
    }

    havePrev    = true;
    prevGapDiff = gapDiff;
    prevPW      = pw;
  }
}

// Phase 2a (bracket found): bisection search within the sign-change bracket.
// Midpoint samples refine the best candidate but are not added to the valid
// table (same as the original implementation).
static void pw_bisect_bracket(PWSearchState& st,
                              double gapTarget, double targetGap,
                              double periodUs, double toleranceDutyPercent) {
  uint16_t pwLow  = st.pwLow;
  uint16_t pwHigh = st.pwHigh;
  double   gapLow = st.gapLow;

  for (int iter = 0; iter < 14; ++iter) {
    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW bisection timeout (60s)");
      break;
    }

    uint16_t pwMid = (uint16_t)((pwLow + pwHigh) / 2);
    GapMeasurement gm = set_pw_and_measure(0, pwMid);
    if (gm.timedOut) {
      // No valid data at this midpoint; try again on the next iteration.
      if (autotuneDebug >= 2) {
        Serial.println("PW center: timeout during bisection, skipping midpoint.");
      }
      continue;
    }

    double gapMid     = (double)gm.value;
    double gapDiffMid = gapMid - gapTarget;

    pw_record_sample(st, pwMid, gapDiffMid, targetGap, PW_RECORD_NO_TABLE);

    if (autotuneDebug >= 2 && periodUs > 0.0) {
      double dutyPercent = (0.5 + gapMid / (2.0 * periodUs)) * 100.0;
      Serial.println((String)"[PW_CENTER_BISECT] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW_raw=" + pwMid +
                     (String)" gap=" + gapMid +
                     (String)"us duty=" + dutyPercent +
                     (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
    }

    // Maintain the sign-change bracket.
    if ((gapDiffMid > 0.0 && (gapLow - gapTarget) > 0.0) ||
        (gapDiffMid < 0.0 && (gapLow - gapTarget) < 0.0)) {
      pwLow  = pwMid;
      gapLow = gapMid;
    } else {
      pwHigh = pwMid;
    }

    if (pwHigh - pwLow <= 1) {
      break;  // can't refine further in integer PW space
    }
  }
}

// Phase 2b (no bracket): local fine scan around the best coarse candidate so
// that we still gather several near-target samples before deciding.
static void pw_fine_scan_around_best(PWSearchState& st,
                                     double gapTarget, double targetGap,
                                     uint16_t pwMin, uint16_t pwMax,
                                     uint16_t coarseStep) {
  if (autotuneDebug >= 1) {
    Serial.println("PW center: no sign-change bracket found, running local fine scan.");
  }

  uint16_t startPW = (st.haveBest && st.bestPW >= pwMin && st.bestPW <= pwMax)
                       ? st.bestPW
                       : (uint16_t)((pwMin + pwMax) / 2);
  uint16_t span = (coarseStep > 0) ? coarseStep * 2 : 4;
  uint16_t fineMin = (startPW > span) ? (startPW - span) : pwMin;
  uint16_t fineMax = (startPW + span < pwMax) ? (startPW + span) : pwMax;
  if (fineMax < fineMin) {
    uint16_t tmp = fineMin;
    fineMin = fineMax;
    fineMax = tmp;
  }
  uint16_t fineStep = (fineMax > fineMin) ? ((fineMax - fineMin) / 16) : 1;
  if (fineStep == 0) fineStep = 1;

  for (uint16_t pw = fineMin; pw <= fineMax; pw = (uint16_t)(pw + fineStep)) {
    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW local fine scan timeout (60s)");
      break;
    }

    GapMeasurement gm = set_pw_and_measure(0, pw);
    if (gm.timedOut) {
      continue;
    }
    pw_record_sample(st, pw, (double)gm.value - gapTarget, targetGap, PW_RECORD_APPEND);
  }
}

// Lock-in: demand 3 consecutive measurements within targetGap of gapTarget at
// the given PW (up to 8 tries). On success, writes the last locked gap to
// lockedGapOut and returns true.
static bool pw_lock_in(uint8_t voiceIdx, uint16_t pw,
                       double gapTarget, double targetGap,
                       double periodUs, double& lockedGapOut) {
  const int kMaxLockInTries = 8;
  int consecutiveOk = 0;

  for (int li = 0; li < kMaxLockInTries; ++li) {
    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (gm.timedOut || periodUs <= 0.0) {
      consecutiveOk = 0;
      continue;
    }

    double gap = (double)gm.value;
    if (fabs(gap - gapTarget) <= targetGap) {
      consecutiveOk++;
      if (consecutiveOk >= 3) {
        lockedGapOut = gap;
        return true;
      }
    } else {
      consecutiveOk = 0;
    }
  }
  return false;
}

// Phase 3: pick the best candidate from the valid-samples table (smallest gap
// to target first), demanding a lock-in at each candidate. A locked candidate
// is then refined locally (PW-2..PW+2, each with its own mini lock-in).
// Returns true and writes the final PW to chosenPWOut on success; false if
// every candidate failed lock-in or the best gap was hopelessly large.
static bool pw_select_and_lock(PWSearchState& st,
                               double gapTarget, double targetGap,
                               uint16_t pwMin, uint16_t pwMax,
                               double periodUs, uint16_t& chosenPWOut) {
  // Try candidates from best gap to worse. After each failed lock-in the
  // candidate's gap difference is inflated so it won't be chosen again.
  for (int attempt = 0; attempt < st.validCount; ++attempt) {
    int    bestIdx = -1;
    double bestAbs = 1e12;
    int    inTolForThisPass = 0;

    for (int vi = 0; vi < st.validCount; ++vi) {
      double curAbs = fabs(st.validGapDiff[vi]);
      if (curAbs <= targetGap) {
        inTolForThisPass++;
      }
      if (curAbs < bestAbs) {
        bestAbs = curAbs;
        bestIdx = vi;
      }
    }

    if (bestIdx < 0) {
      break;
    }

    // If the best gap is still extremely large compared to the allowed gap
    // (e.g. > 10x), abort early and keep the previous PW center.
    if (bestAbs > targetGap * 10.0) {
      if (autotuneDebug >= 1) {
        Serial.println((String)"[PW_CENTER_ABORT] note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" bestGap=" + bestAbs +
                       (String)"us (> " + targetGap * 10.0 +
                       (String)"us); keeping PW_center=" + PW_CENTER[0]);
      }
      return false;
    }

    uint16_t chosenPW  = st.validPW[bestIdx];
    double   chosenGap = gapTarget + st.validGapDiff[bestIdx];

    double lockedGap = 0.0;
    if (pw_lock_in(0, chosenPW, gapTarget, targetGap, periodUs, lockedGap)) {
      chosenGap = lockedGap;

      // Local refinement: probe a small neighbourhood around the locked-in PW
      // (PW-2..PW+2). Each candidate must pass its own mini lock-in before it
      // can replace the current choice.
      uint16_t bestLocalPW     = chosenPW;
      double   bestLocalGapAbs = bestAbs;

      for (int16_t off = -2; off <= 2; ++off) {
        int32_t testPW32 = (int32_t)chosenPW + off;
        if (testPW32 < (int32_t)pwMin || testPW32 > (int32_t)pwMax) continue;
        uint16_t testPW = (uint16_t)testPW32;

        double gapLocal = 0.0;
        if (pw_lock_in(0, testPW, gapTarget, targetGap, periodUs, gapLocal)) {
          double absGapDiffLocal = fabs(gapLocal - gapTarget);
          if (absGapDiffLocal < bestLocalGapAbs) {
            bestLocalGapAbs = absGapDiffLocal;
            bestLocalPW     = testPW;
            chosenGap       = gapLocal;
          }
        }
      }

      chosenPW = bestLocalPW;
      double chosenDutyPercent = 0.0;
      if (periodUs > 0.0) {
        chosenDutyPercent = (0.5 + chosenGap / (2.0 * periodUs)) * 100.0;
      }

      if (autotuneDebug >= 1) {
        Serial.println((String)"[PW_CENTER_RESULT] note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" PW_center=" + chosenPW +
                       (String)" duty≈" + chosenDutyPercent +
                       (String)"% bestGap=" + bestLocalGapAbs +
                       (String)"us inTolSamples=" + inTolForThisPass +
                       (String)" totalValid=" + st.validCount);
      }
      chosenPWOut = chosenPW;
      return true;
    }

    // This candidate failed lock-in; inflate its gap diff so we try the next
    // best one on the following attempt.
    st.validGapDiff[bestIdx] = targetGap * 20.0;
    if (autotuneDebug >= 1) {
      Serial.println((String)"[PW_CENTER_LOCKIN_REJECT] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW=" + chosenPW +
                     (String)" could not get 3 consecutive in-band readings; trying next candidate.");
    }
  }

  if (autotuneDebug >= 1) {
    Serial.println((String)"[PW_CENTER_ABORT] note=" + DCO_calibration_current_note +
                   (String)" DCO=" + currentDCO +
                   (String)" all candidates failed lock-in; keeping PW_center=" +
                   PW_CENTER[0]);
  }
  return false;
}

// Shared search routine used by PW calibration (currently the center search).
// It looks for the PW value whose duty cycle is closest to targetDutyFraction
// at the current calibration note. targetGap is the allowed absolute gap (in
// microseconds) from the ideal duty at that note. On failure the caller's
// fallbackPW is returned unchanged.
//
// Phases: coarse scan → bisection (bracket) or local fine scan (no bracket)
// → candidate selection with lock-in and local refinement.
static uint16_t find_PW_for_target_duty(double targetDutyFraction,
                                        uint16_t targetGap,
                                        uint16_t pwMin,
                                        uint16_t pwMax,
                                        uint16_t fallbackPW) {

  double freqHz   = (double)note_to_freq(DCO_calibration_current_note);
  double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  // Update global logging context for gap measurements during this search.
  g_gapLogCurrentPeriodUs    = periodUs;
  g_gapLogTargetDutyFraction = targetDutyFraction;

  double toleranceDutyPercent = 0.0;
  double gapTarget = 0.0;
  if (periodUs > 0.0) {
    // Ideal gap for a target HIGH-duty p: gap = avgHigh - avgLow = T*(2p - 1).
    // (Zero for the 50% center target, positive above, negative below.)
    gapTarget = periodUs * (2.0 * targetDutyFraction - 1.0);
    toleranceDutyPercent = ((double)targetGap / (2.0 * periodUs)) * 100.0;
  }

  // Coarse step: use smaller steps for low/high limit searches (target duty
  // far from 50%) and larger steps for the center search.
  uint16_t coarseDiv  = (fabs(targetDutyFraction - 0.5) < 0.05) ? 16 : 32;
  uint16_t coarseStep = (pwMax > pwMin) ? ((pwMax - pwMin) / coarseDiv) : 1;
  if (coarseStep == 0) coarseStep = 1;

  PWSearchState st;
  pw_search_state_init(st);

  pw_coarse_scan(st, gapTarget, (double)targetGap, pwMin, pwMax, coarseStep,
                 periodUs, toleranceDutyPercent);

  if (st.haveBracket) {
    pw_bisect_bracket(st, gapTarget, (double)targetGap, periodUs, toleranceDutyPercent);
  } else {
    pw_fine_scan_around_best(st, gapTarget, (double)targetGap, pwMin, pwMax, coarseStep);
  }

  if (st.validCount == 0) {
    // No valid samples at all in the searched range: keep the caller's PW
    // and log the situation so the user can investigate.
    if (autotuneDebug >= 1) {
      Serial.println("PW search: no valid samples found; keeping current PW.");
    }
    return fallbackPW;
  }

  uint16_t chosenPW = fallbackPW;
  if (pw_select_and_lock(st, gapTarget, (double)targetGap, pwMin, pwMax,
                         periodUs, chosenPW)) {
    return chosenPW;
  }
  return fallbackPW;
}

// Locate PW center for the current DCO's voice by minimizing duty-cycle error
// at a reference note. Mode 0 = low note, mode 1 = higher note refinement.
void find_PW_center(uint8_t mode) {

  DCO_calibration_current_note = manual_DCO_calibration_start_note;
  VOICE_NOTES[0] = DCO_calibration_current_note;
  ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  uint16_t targetGap;
  uint8_t voiceTaskMode;

  if (mode == 0) {
    targetGap = compute_gap_tolerance_for_freq(note_to_freq(DCO_calibration_current_note), 0.005);
    voiceTaskMode = 2;
  } else {
    DCO_calibration_current_note = 76;
    VOICE_NOTES[0] = DCO_calibration_current_note;
    targetGap = 5;
    voiceTaskMode = 3;
  }

  DCOCalibrationStart = millis();

  // Starting PW: middle of the range on the very first tune, otherwise the
  // previously stored center.
  if (firstTuneFlag == true) {
    PW[0] = DIV_COUNTER_PW / 2;
    PW_CENTER[0] = DIV_COUNTER_PW / 2;
  } else {
    PW[0] = PW_CENTER[0];
  }
  uint16_t startPW = PW[0];

  // Apply the starting PW to the PW PWM channel before configuring the DCO.
  pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), startPW);
  g_lastPWMeasurementRaw = startPW;

  voice_task_autotune(voiceTaskMode, ampCompCalibrationVal);

  uint16_t centerPW = find_PW_for_target_duty(
    kPWCenterDutyFraction,
    targetGap,
    0,
    DIV_COUNTER_PW,
    startPW
  );
  Serial.println("PW center found !!!");
  update_FS_PWCenter(0, centerPW);
  PW_CENTER[0] = centerPW;

  // Apply the newly found PW center immediately to the hardware so that the
  // effect is visible on the pulse waveform as soon as calibration finishes.
  pwm_set_chan_level(PW_PWM_SLICES[0],
                     pwm_gpio_to_channel(PW_PINS[0]),
                     centerPW);
  PW[0]                  = centerPW;
  g_lastPWMeasurementRaw = centerPW;
}


// -----------------------------------------------------------------------------
// PW limit search
// -----------------------------------------------------------------------------

PWLimitSearchResult search_PW_limit_from_center(
  uint8_t     voiceIdx,
  uint16_t    centerPW,
  PWLimitDir  dir,
  double      periodUs,
  double      targetDuty
) {
  PWLimitSearchResult result;
  result.ok                  = false;
  result.limitPW             = centerPW;
  result.finalDutyPercent    = -1.0;

  if (periodUs <= 0.0) {
    return result;
  }

  // Hard bounds convention:
  //  - LOW  side scans from center down to 0
  //  - HIGH side scans from center up to DIV_COUNTER_PW
  uint16_t minPW = (dir == PW_LIMIT_LOW)  ? 0           : centerPW;
  uint16_t maxPW = (dir == PW_LIMIT_LOW)  ? centerPW    : DIV_COUNTER_PW;

  // Coarse step size for scanning from center toward the limit.
  uint16_t step = DIV_COUNTER_PW / 64;
  if (step == 0) step = 1;

  bool     haveBest   = false;
  uint16_t bestPW     = centerPW;
  double   bestDelta  = 1e12;
  double   bestDuty   = -1.0;   // duty (0..1) at bestPW when known

  unsigned long searchStartMs = millis();

  // Coarse scan: walk from center toward the requested side, tracking the
  // PW that gets closest to the target duty. We stop when we reach the
  // boundary, run out of time, or find a value within tolerance.
  for (uint16_t pw = centerPW; ; ) {
    if (millis() - searchStartMs > 60000UL) {
      // Safety timeout.
      break;
    }

    if (pw < minPW) pw = minPW;
    if (pw > maxPW) pw = maxPW;

    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (!gm.timedOut) {
      double gap           = (double)gm.value;
      double dutyErrorFrac = gap / (2.0 * periodUs);
      double duty          = 0.5 + dutyErrorFrac;

      double delta = fabs(duty - targetDuty);
      if (!haveBest || delta < bestDelta) {
        haveBest  = true;
        bestDelta = delta;
        bestPW    = pw;
        bestDuty  = duty;
      }

      if (autotuneDebug >= 2) {
        const char *scanTag =
          (dir == PW_LIMIT_LOW) ? "[PW_LOW_SCAN_V2]" : "[PW_HIGH_SCAN_V2]";
        Serial.println((String)scanTag +
                       (String)" note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" PW_raw=" + pw +
                       (String)" duty=" + (duty * 100.0) + "%" +
                       (String)" targetDuty=" + (targetDuty * 100.0) + "%");
      }

      // If we are already within tolerance, we can stop the coarse scan early.
      if (delta <= kPWLimitDutyTolerance) {
        break;
      }
    }

    // Step toward the boundary.
    if (dir == PW_LIMIT_LOW) {
      if (pw <= minPW + step) {
        break;
      }
      pw = (uint16_t)(pw - step);
    } else {  // PW_LIMIT_HIGH
      if (pw >= maxPW - step) {
        break;
      }
      pw = (uint16_t)(pw + step);
    }
  }

  if (!haveBest) {
    // Never saw a valid measurement; caller should keep previous limit.
    return result;
  }

  // Fine refinement around bestPW: search with step = 1 in a relatively
  // tight window around the best coarse candidate. This keeps the search
  // local so we do not wander too far from the best-known PW.
  uint16_t refineRadius = step / 2;
  if (refineRadius < 4)  refineRadius = 4;
  if (refineRadius > 32) refineRadius = 32;

  uint16_t startPW;
  if (bestPW > refineRadius) {
    startPW = bestPW - refineRadius;
  } else {
    startPW = minPW;
  }
  // Enforce the same [minPW, maxPW] bounds used in the coarse scan so that
  // the refinement phase never crosses to the other side of center.
  if (startPW < minPW) startPW = minPW;

  uint16_t endPW = bestPW + refineRadius;
  if (endPW > maxPW) {
    endPW = maxPW;
  }

  int consecutiveTimeouts = 0;
  for (uint16_t pw = startPW; pw <= endPW; ++pw) {
    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (gm.timedOut) {
      // If we are stepping deeper into the "edge" side and accumulate several
      // consecutive timeouts, stop refining in that direction to avoid
      // spending a long time in a region with no measurable signal.
      ++consecutiveTimeouts;
      bool goingDeeperLow  = (dir == PW_LIMIT_LOW)  && (pw < bestPW);
      bool goingDeeperHigh = (dir == PW_LIMIT_HIGH) && (pw > bestPW);
      if ((goingDeeperLow || goingDeeperHigh) && consecutiveTimeouts >= 4) {
        break;
      }
      continue;
    }
    consecutiveTimeouts = 0;

    double gap           = (double)gm.value;
    double dutyErrorFrac = gap / (2.0 * periodUs);
    double duty          = 0.5 + dutyErrorFrac;

    double delta = fabs(duty - targetDuty);
    if (delta < bestDelta) {
      bestDelta = delta;
      bestPW    = pw;
      bestDuty  = duty;
    }
  }

  // Final result: start from the best sample seen during coarse+fine.
  result.ok      = true;
  result.limitPW = bestPW;

  if (bestDuty >= 0.0) {
    result.finalDutyPercent = bestDuty * 100.0;
  }

  // Check whether the target duty is actually reachable within tolerance.
  double currentDutyFrac = result.finalDutyPercent / 100.0;
  if (result.finalDutyPercent <= 0.0 ||
      fabs(currentDutyFrac - targetDuty) > kPWLimitDutyTolerance) {
    // Not within tolerance: push all the way to the hardware boundary for
    // this side and treat that as the "best possible" limit. This matches
    // the specification that the target is considered unreachable only after
    // trying the maximum/minimum PW value.
    uint16_t boundaryPW = (dir == PW_LIMIT_LOW) ? minPW : maxPW;

    GapMeasurement gmEdge = set_pw_and_measure(voiceIdx, boundaryPW);
    if (!gmEdge.timedOut) {
      double gap           = (double)gmEdge.value;
      double dutyErrorFrac = gap / (2.0 * periodUs);
      double duty          = 0.5 + dutyErrorFrac;
      result.limitPW        = boundaryPW;
      result.finalDutyPercent = duty * 100.0;
    } else {
      // If even the boundary cannot be measured reliably, we still honour the
      // boundary PW as the limit but leave finalDutyPercent as-is.
      result.limitPW = boundaryPW;
    }
  }

  return result;
}

void find_PW_limit_v2(PWLimitDir dir) {
  uint8_t voiceTaskMode = 2;

  // Configure the calibration context the same way as the PW center search
  // so that both phases operate on the same note and amplitude.
  DCO_calibration_current_note = manual_DCO_calibration_start_note;
  VOICE_NOTES[0] = DCO_calibration_current_note;
  ampCompCalibrationVal =
    initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  DCOCalibrationStart = millis();

  double freqHz   = (double)note_to_freq(DCO_calibration_current_note);
  double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  uint8_t  voiceIdx = 0;
  uint16_t centerPW = PW_CENTER[voiceIdx];

  // Direction-dependent target HIGH-duty:
  //  - Low limit:  kPWLowDutyFraction  (≈ 2% HIGH)
  //  - High limit: kPWHighDutyFraction (≈98% HIGH)
  double targetDuty = (dir == PW_LIMIT_LOW)
                      ? kPWLowDutyFraction
                      : kPWHighDutyFraction;

  // Update global logging context for gap measurements during PW-limit search.
  g_gapLogCurrentPeriodUs    = periodUs;
  g_gapLogTargetDutyFraction = targetDuty;

  // Configure the DCO for PW calibration mode.
  voice_task_autotune(voiceTaskMode, ampCompCalibrationVal);
  delay(100);

  PWLimitSearchResult res =
    search_PW_limit_from_center(voiceIdx, centerPW, dir, periodUs, targetDuty);

  if (!res.ok) {
    if (autotuneDebug >= 1) {
      const char *abortTag =
        (dir == PW_LIMIT_LOW) ? "[PW_LOW_ABORT_NO_SIGNAL_V2]" : "[PW_HIGH_ABORT_NO_SIGNAL_V2]";
      uint16_t keepPW =
        (dir == PW_LIMIT_LOW) ? PW_LOW_LIMIT[voiceIdx] : PW_HIGH_LIMIT[voiceIdx];
      Serial.println((String)abortTag +
                     (String)" note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" keeping_PW=" + keepPW);
    }
    return;
  }

  // Log result and commit it.
  double targetDutyPercent =
    (dir == PW_LIMIT_LOW)
      ? (kPWLowDutyFraction * 100.0)
      : ((1.0 - kPWHighDutyFraction) * 100.0);
  double targetHighDutyPercent = kPWHighDutyFraction * 100.0;

  if (autotuneDebug >= 1) {
    const char *resultTag =
      (dir == PW_LIMIT_LOW) ? "[PW_LOW_RESULT_V2]" : "[PW_HIGH_RESULT_V2]";
    Serial.println((String)resultTag +
                   (String)" note=" + DCO_calibration_current_note +
                   (String)" DCO=" + currentDCO +
                   (String)" PW_LIMIT=" + res.limitPW +
                   (String)" duty≈" + res.finalDutyPercent + "%" +
                   (String)" targetDuty=" + targetDutyPercent + "%" +
                   (dir == PW_LIMIT_LOW
                      ? (String)""
                      : (String)" targetHighDuty=" + targetHighDutyPercent + "%"));
  }

  if (dir == PW_LIMIT_LOW) {
    Serial.println("--------------------------------");
    Serial.println("PW low limit (v2) found !!!");
    Serial.println(
      (String)" PW_LIMIT=" + res.limitPW +
      (String)" duty≈" + res.finalDutyPercent + "%" +
      (String)" targetDuty=" + (kPWLowDutyFraction * 100.0) + "%");
    Serial.println("--------------------------------");
    update_FS_PW_Low_Limit(voiceIdx, res.limitPW);
    PW_LOW_LIMIT[voiceIdx] = res.limitPW;
  } else {
    Serial.println("--------------------------------");
    Serial.println("PW high limit (v2) found !!!");
    Serial.println(
      (String)" PW_LIMIT=" + res.limitPW +
      (String)" duty≈" + res.finalDutyPercent + "%" +
      (String)" targetDuty=" + ((1.0 - kPWHighDutyFraction) * 100.0) + "%" +
      (String)" targetHighDuty=" + (kPWHighDutyFraction * 100.0) + "%");
    Serial.println("--------------------------------");
    update_FS_PW_High_Limit(voiceIdx, res.limitPW);
    PW_HIGH_LIMIT[voiceIdx] = res.limitPW;
  }
}

//////////////////////////////////////////////////////////////////////////////
// Raw cal-sense probe (no period gate): sample digital level / edge rate so a
// TIMEOUT can be split into "pin stuck" vs "edges exist but find_gap rejects".
// Throttled to ~2 Hz. Called from DCO_calibration_debug on gap timeout.
static void cal_sense_probe_log() {
  static uint32_t lastPrintMs = 0;
  const uint32_t nowMs = millis();
  if ((nowMs - lastPrintMs) < 500u) {
    return;
  }
  lastPrintMs = nowMs;

  constexpr uint32_t kWindowUs = 40000u;  // 40 ms
  const uint32_t t0 = micros();
  bool lastRaw = digitalRead(DCO_calibration_pin);
  uint32_t edges = 0;
  uint32_t minDt = 0xFFFFFFFFu;
  uint32_t maxDt = 0;
  uint32_t lastEdgeUs = t0;
  bool haveEdge = false;

  while ((micros() - t0) < kWindowUs) {
    const bool raw = digitalRead(DCO_calibration_pin);
    if (raw != lastRaw) {
      const uint32_t nowUs = micros();
      const uint32_t dt = nowUs - lastEdgeUs;
      if (haveEdge) {
        if (dt < minDt) {
          minDt = dt;
        }
        if (dt > maxDt) {
          maxDt = dt;
        }
      }
      lastEdgeUs = nowUs;
      haveEdge = true;
      edges++;
      lastRaw = raw;
    }
  }

  const bool rawNow = digitalRead(DCO_calibration_pin);
  double expectHz = 0.0;
  if (DCO_calibration_current_note >= 12) {
    expectHz = (double)note_to_freq(DCO_calibration_current_note);
  }

  Serial.print((String)"[CAL_SENSE] pin=" + DCO_calibration_pin +
               (String)" raw=" + (int)rawNow +
               (String)" edges=" + edges);
  if (edges >= 2 && minDt != 0xFFFFFFFFu) {
    Serial.print((String)" minDt=" + minDt + (String)" maxDt=" + maxDt);
  } else {
    Serial.print(" minDt=- maxDt=-");
  }
  Serial.println((String)" pullup=1 invert=" + (int)kGapPolarityInverted +
                 (String)" note=" + DCO_calibration_current_note +
                 (String)" expectHz≈" + expectHz);
}

//////////////////////////////////////////////////////////////////////////////
// Measure duty-cycle error on DCO_calibration_pin by timing rising/falling
// edges. Returns avgHighUs - avgLowUs (0 when duty is ≈50%), or
// kGapTimeoutSentinel on timeout. All measurement state is local; callers
// normally use the measure_gap() wrapper from autotune_measurement.h.
float find_gap(byte specialMode) {
  // Number of accepted low/high segments per measurement.
  const uint16_t samplesTarget = (specialMode == 2) ? 12 : 6;

  // Estimate ideal period for the current note so we can reject obviously
  // invalid edge intervals (e.g. very short glitches) that do not match the
  // DCO's actual frequency.
  double freqHz = (double)note_to_freq(DCO_calibration_current_note);
  double idealPeriodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;
  double dtMinUs = 0.0;
  double dtMaxUs = 0.0;
  if (idealPeriodUs > 0.0) {
    // Accept any segment between ~1% and ~99% of the ideal period. This covers
    // extreme duty cycles (2%/98%) while rejecting very short/high-frequency
    // glitches that are clearly not the fundamental.
    dtMinUs = idealPeriodUs * 0.01;
    dtMaxUs = idealPeriodUs * 0.99;
    if (dtMinUs < (double)kEdgeDebounceMinUs) {
      dtMinUs = (double)kEdgeDebounceMinUs;
    }
    if (dtMaxUs > (double)kGapTimeoutUs) {
      dtMaxUs = (double)kGapTimeoutUs;
    }
  }

  // Local edge-timing state (was global before the cleanup).
  int      pulseCount      = 0;
  uint16_t acceptedSamples = 0;
  double   risingSumUs     = 0.0;
  double   fallingSumUs    = 0.0;
  bool     lastVal         = 0;
  uint16_t risingCount     = 0;
  uint16_t fallingCount    = 0;
  // Diagnostics: debounced edges vs period-gate rejects (TIMEOUT localization).
  uint16_t edgesSeen     = 0;
  uint16_t edgesRejected = 0;

  unsigned long lastEdgeTime = micros();

  while (acceptedSamples < samplesTarget) {

    bool rawVal = digitalRead(DCO_calibration_pin);
    // Compensate for hardware polarity if needed so that 'val == 1' always
    // represents the same logical DCO level for duty measurements.
    bool val = kGapPolarityInverted ? !rawVal : rawVal;
    unsigned long nowUs = micros();

    if ((nowUs - lastEdgeTime) > kGapTimeoutUs) {
      const bool rawAtTimeout = digitalRead(DCO_calibration_pin);

      // Manual cal: log at debug >= 1. Auto-cal keeps the quieter >= 3 threshold.
      if (autotuneDebug >= 3 || (manualCalibrationFlag && autotuneDebug >= 1)) {
        Serial.println((String)"[GAP_TIMEOUT] note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" raw=" + (int)rawAtTimeout +
                       (String)" edges=" + edgesSeen +
                       (String)" rejected=" + edgesRejected +
                       (String)" accepted=" + acceptedSamples +
                       (String)" TidealUs≈" + (uint32_t)idealPeriodUs +
                       (String)" PW_raw=" + g_lastPWMeasurementRaw +
                       (String)" ampComp=" + ampCompCalibrationVal);
      }

      return kGapTimeoutSentinel;
    }

    if (val != lastVal) {
      if ((nowUs - lastEdgeTime) >= kEdgeDebounceMinUs) {

        lastVal = val;
        edgesSeen++;

        // Re-align so counting starts on a rising edge.
        if (pulseCount == 1 && val == 0) {
          pulseCount = 0;
        }
        if (pulseCount > 2) {
          uint32_t dt = nowUs - lastEdgeTime;
          bool intervalOk = true;
          if (idealPeriodUs > 0.0) {
            // Reject intervals that are incompatible with the ideal period.
            // This prevents very short spurious edges from corrupting the
            // duty measurement at low frequencies.
            if ((double)dt < dtMinUs || (double)dt > dtMaxUs) {
              intervalOk = false;
            }
          }

          // NOTE: segment attribution follows the legacy convention (the
          // segment ending on a falling edge goes into the "falling" sum).
          // The overall sign chain (kGapPolarityInverted here plus the flip
          // in measure_gap_for_amp) is field-validated; keep them in sync if
          // this is ever changed.
          if (intervalOk) {
            if (val == 0) {
              fallingSumUs += dt;
              fallingCount++;
            } else {
              risingSumUs += dt;
              risingCount++;
            }
            acceptedSamples++;
          } else {
            edgesRejected++;
          }
        }
        lastEdgeTime = nowUs;
        pulseCount++;
      }
    }
  }

  // Compute average low and high segment durations directly from the number
  // of segments we actually accumulated.
  float avgLowUs  = (fallingCount > 0) ? (float)fallingSumUs / (float)fallingCount : 0.0f;
  float avgHighUs = (risingCount  > 0) ? (float)risingSumUs  / (float)risingCount  : 0.0f;

  // Derived period and direct HIGH-duty estimate based purely on measured
  // low/high portions (duty cycle = fraction of the period spent HIGH).
  float measuredPeriodUs = avgLowUs + avgHighUs;
  float dutyMeasuredFrac = (measuredPeriodUs > 0.0f) ? (avgHighUs / measuredPeriodUs) : 0.0f;

  // Positive result means the HIGH segment is longer than LOW (duty > 50%);
  // negative means LOW is longer (duty < 50%). This keeps the relation:
  //   duty_high - 0.5 = diff / (2 * periodUs)
  float diffUs = avgHighUs - avgLowUs;

  if (autotuneDebug >= 2) {
    // Log raw gap measurement with context: which mode, note/DCO, the
    // current amplitude compensation value, the last PW we explicitly set,
    // and the inferred duty/target duty if a period is available.

    // Duty estimate using the same "diff vs ideal period" method used by
    // the PW search code.
    double dutyPercentIdeal = 0.0;
    double targetDutyPercent = g_gapLogTargetDutyFraction * 100.0;
    if (g_gapLogCurrentPeriodUs > 0.0) {
      double dutyErrorFrac = (double)diffUs / (2.0 * g_gapLogCurrentPeriodUs);
      dutyPercentIdeal = (0.5 + dutyErrorFrac) * 100.0;
    }

    // Direct duty estimate based only on measured low/high times.
    double dutyPercentMeasured = dutyMeasuredFrac * 100.0;

    Serial.println((String)"[GAP_MEASURE] mode=" + specialMode +
                   (String)" note=" + DCO_calibration_current_note +
                   (String)" DCO=" + currentDCO +
                   (String)" AMP=" + ampCompCalibrationVal +
                   (String)" PW_raw=" + g_lastPWMeasurementRaw +
                   (String)" diff=" + diffUs +
                   (String)" avgLowUs=" + avgLowUs +
                   (String)" avgHighUs=" + avgHighUs +
                   (String)" T_meas=" + measuredPeriodUs +
                   (String)" duty_meas≈" + dutyPercentMeasured + "%" +
                   (String)" duty_ideal≈" + dutyPercentIdeal + "%" +
                   (String)" targetDuty=" + targetDutyPercent + "%");
  }

  return diffUs;
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

// Debug helper used during manual calibration: measure and report the
// duty-cycle difference from the target duty (normally 50%) for the
// current note/DCO. The result is sent to the Input board as a 32-bit
// PARAM_GAP_FROM_DCO value, which it relays to the screen as "GAP".
void DCO_calibration_debug() {
  // Reuse the main gap-measurement path (which already handles polarity,
  // debouncing, and timeouts) so manual calibration sees the same notion
  // of "gap" as the automatic routines.
  GapMeasurement gm = measure_gap(0);  // target is 50% duty

  // Osc under trim is selected by manualCalibrationStage (not currentDCO,
  // which is only advanced during auto-cal).
  uint8_t reportDCO = manualCalibrationStage;
  if (reportDCO >= NUM_OSCILLATORS) {
    reportDCO = NUM_OSCILLATORS - 1;
  }

  // Compute duty error relative to the center target (0.5) using the
  // *ideal* period for the current note. For manual trimming this is
  // sufficient and keeps the math simple.
  int32_t dutyErrorPercentTimes100 = 0;  // duty error [%] * 100

  if (!gm.timedOut) {
    double freqHz = (double)note_to_freq(DCO_calibration_current_note);
    if (freqHz > 0.0) {
      double periodUs = 1000000.0 / freqHz;
      // gm.value is avgHighUs - avgLowUs (same sign as find_gap).
      // For a perfect 50% duty, high and low are equal, so gm.value == 0.
      // Duty error fraction from 50% is:
      //   duty_high - 0.5 = (avgHighUs - avgLowUs) / (2 * periodUs)
      double dutyErrorFrac = (double)gm.value / (2.0 * periodUs);
      double dutyErrorPercent = dutyErrorFrac * 100.0;
      // Scale by 100 for two decimal digits of resolution on the screen.
      dutyErrorPercentTimes100 = (int32_t)(dutyErrorPercent * 100.0);
    }
  } else {
    // On timeout, propagate a large sentinel so the UI/Serial never look
    // like a near-perfect 50% trim.
    dutyErrorPercentTimes100 = kManualGapTimeoutDutyErrTimes100;
  }

  if (autotuneDebug >= 1) {
    if (gm.timedOut) {
      Serial.println((String)"[MANUAL_GAP] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + reportDCO +
                     (String)" AMP=" + ampCompCalibrationVal +
                     (String)" TIMEOUT");
      // Raw cal-sense window (no period gate) — separates stuck pin from rejected freq.
      cal_sense_probe_log();
    } else {
      Serial.println((String)"[MANUAL_GAP] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + reportDCO +
                     (String)" AMP=" + ampCompCalibrationVal +
                     (String)" gapUs=" + gm.value +
                     (String)" dutyErr(%)≈" + (dutyErrorPercentTimes100 / 100.0));
    }
  }

  // Send as a 32-bit PARAM_GAP_FROM_DCO value through the standard
  // param protocol: Serial2 → Input, which relays it to the Screen.
  serialSendParam32(PARAM_GAP_FROM_DCO, dutyErrorPercentTimes100);
}
