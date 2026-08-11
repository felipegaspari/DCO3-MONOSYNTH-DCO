#include "include_all.h"

// =============================================================================
// autotune_search.ino — search-based DCO amplitude-compensation calibration.
//
// This file holds the per-note search that builds each oscillator's
// [frequency -> range PWM] table (calibrate_DCO), the highest/lowest
// frequency estimators used when the table reaches the top of the PWM range,
// and the interpolation helpers shared by those routines.
//
// Orchestration (DCO_calibration) and the PW center/limit searches live in
// autotune.ino; the edge-timing measurement core (find_gap) lives there too.
// =============================================================================

// Compute allowed |gap| (in microseconds) for a given frequency (Hz) and
// duty-cycle error fraction (e.g. 0.005 = 0.5% duty error).
// From duty_high - 0.5 = gap / (2*T): |gap|max = 2 * epsilon * T.
double compute_gap_tolerance_for_freq(double freqHz, double dutyErrorFraction) {
  if (freqHz <= 0.0) {
    return 1e6;  // Very loose tolerance if frequency is invalid.
  }
  double periodUs = 1e6 / freqHz;
  return 2.0 * dutyErrorFraction * periodUs;
}

// Return true if the two values have opposite signs (simple sign change test).
// Used by calibrate_DCO() to detect when the duty-cycle error has crossed
// through zero between successive measurements (indicating we've passed the
// ideal PWM point and should probe neighbours more carefully).
static bool did_sign_change(float previous, float current) {
  return (previous > 0.0f && current < 0.0f) ||
         (previous < 0.0f && current > 0.0f);
}

// Helper: set the current DCO amplitude, wait for the waveform to settle,
// and return the measured duty-cycle gap (or timeout sentinel value).
// IMPORTANT: We normalize the sign here so that a *positive* value means
// "amplitude too low" and a *negative* value means "amplitude too high".
static float measure_gap_for_amp(uint16_t ampPwm) {
  voice_task_autotune(0, ampPwm);
  delay(10);
  GapMeasurement gm = measure_gap(0);

  // Preserve the timeout sentinel exactly so downstream code can reliably
  // detect "no signal" vs a real small error.
  if (gm.timedOut) {
    return kGapTimeoutSentinel;
  }

  // find_gap() returns avgHighUs - avgLowUs; flip the sign so the search
  // moves the PWM in the correct direction regardless of edge polarity.
  return -gm.value;
}

// Helper: evaluate neighbour measurements (lower/higher) around the current
// PWM and update closestToZero / bestAmpComp if any of them are better.
// The caller passes in the measurements taken one step below and above the
// current PWM value; this routine picks the best candidate among those and
// the current PWM, based purely on closeness of the duty error to zero.
static void update_best_from_neighbours(
  int rangeSamples,
  const float* lowerMeasurements,
  const uint16_t* lowerVoltages,
  const float* higherMeasurements,
  const uint16_t* higherVoltages,
  float avgValue,
  float& closestToZero,
  uint16_t& bestAmpComp,
  uint16_t currentAmpCompCalibrationVal
) {
  for (int i = 0; i < rangeSamples; i++) {
    if (abs(lowerMeasurements[i]) < abs(closestToZero)) {
      closestToZero = lowerMeasurements[i];
      bestAmpComp = lowerVoltages[i];
    }
    if (abs(higherMeasurements[i]) < abs(closestToZero)) {
      closestToZero = higherMeasurements[i];
      bestAmpComp = higherVoltages[i];
    }
  }

  // Check the current voltage again
  if (abs(avgValue) < abs(closestToZero)) {
    closestToZero = avgValue;
    bestAmpComp = currentAmpCompCalibrationVal;
  }
}

// Helper: PWM step for the next probe based on the current error.
// For large errors step by 2; once close to the target (within tolerance * 20)
// step by 1 to avoid overshooting. Sign follows the error direction.
static int step_amp_from_error(float avgValue, double tolerance) {
  int magnitude = (abs(avgValue) < tolerance * 20) ? 1 : 2;
  return (avgValue > 0) ? magnitude : -magnitude;
}

// Helper: compute the initial amplitude (range PWM) guess for a given table
// index j and note, using the same interpolation strategy as the original code:
//  - j == 4: manual preset scaled by 1.35,
//  - j == 6: logarithmic interpolation between the first two entries,
//  - else : quadratic interpolation based on the previous three calibration points.
static uint16_t compute_initial_amp_for_note(
  const DCOCalibrationContext& ctx,
  int j
) {
  if (j == 4) {
    return (ctx.initManualAmpByOsc[ctx.dcoIndex] + ctx.manualOffsetByOsc[ctx.dcoIndex]) * 1.35;
  } else if (j == 6) {
    return logarithmicInterpolation(
      ctx.calibrationData[2],
      ctx.calibrationData[3],
      ctx.calibrationData[4],
      ctx.calibrationData[5],
      note_to_freq(ctx.currentNote) * 100
    );
  } else {
    return quadraticInterpolation(
      ctx.calibrationData[j - 6],
      ctx.calibrationData[j - 5],
      ctx.calibrationData[j - 4],
      ctx.calibrationData[j - 3],
      ctx.calibrationData[j - 2],
      ctx.calibrationData[j - 1],
      note_to_freq(ctx.currentNote) * 100
    );
  }
}

// Helper: store the final calibration pair for the current note into the
// calibration table and print a short summary to Serial.
static void store_note_result(
  DCOCalibrationContext& ctx,
  int j,
  uint16_t bestAmpComp,
  float closestToZero
) {
  ctx.calibrationData[j]     = note_to_freq(ctx.currentNote) * 100;
  ctx.calibrationData[j + 1] = bestAmpComp;

  Serial.print("DCO_calibration_current_note ");
  Serial.println(ctx.currentNote);
  Serial.print("Best calibration voltage: ");
  Serial.println(bestAmpComp);
  Serial.print("Closest measurement to zero: ");
  Serial.println(closestToZero);
}

// Search the highest usable DCO frequency at full range PWM (returns Hz*100).
// Called from calibrate_DCO() when the table reaches the top of the PWM range.
//
// At full amplitude PWM, a positive normalized duty error ("amplitude too
// low") means the frequency is too high for the oscillator to reach full
// amplitude, so we bisect the frequency window downward; a negative error
// means there is headroom, so we bisect upward.
float find_highest_freq() {
  ampCompCalibrationVal = DIV_COUNTER;

  // Search window: one calibration interval below/above the current note.
  double fLow  = note_to_freq(DCO_calibration_current_note - calibration_note_interval);
  double fHigh = note_to_freq(DCO_calibration_current_note + calibration_note_interval);

  const double kDutyGapToleranceUs = 0.5;  // same acceptance as the legacy PID loop
  const int    kMaxIterations      = 24;   // bisection resolution guard

  float bestFreq   = (float)fLow;
  float bestAbsGap = 1e9f;
  bool  sawSignal  = false;

  for (int iter = 0; iter < kMaxIterations; ++iter) {
    double fMid = 0.5 * (fLow + fHigh);
    calibrationFreqHz = (float)fMid;
    voice_task_autotune(4, DIV_COUNTER);
    delay(4);

    GapMeasurement gm = measure_gap(0);
    if (gm.timedOut) {
      // No usable signal: the amplitude has collapsed below the comparator
      // threshold, which happens past the top frequency — search lower.
      fHigh = fMid;
      continue;
    }
    sawSignal = true;

    float diff = -gm.value;  // positive => amplitude too low => freq too high

    if (fabsf(diff) < bestAbsGap) {
      bestAbsGap = fabsf(diff);
      bestFreq   = (float)fMid;
    }

    if (autotuneDebug >= 1) {
      Serial.println((String)"[HIGHEST_FREQ] f=" + fMid + (String)" gap=" + diff);
    }

    if (fabsf(diff) <= kDutyGapToleranceUs) {
      break;
    }
    if (diff > 0) {
      fHigh = fMid;
    } else {
      fLow = fMid;
    }
  }

  if (!sawSignal) {
    Serial.println((String)"[HIGHEST_FREQ] no valid signal in search window; using " + bestFreq);
  }
  Serial.println((String)"Highest freq found: " + bestFreq);

  // Report the nearest note at/below the found frequency.
  constexpr int kNoteCount = (int)(sizeof(sNotePitches) / sizeof(sNotePitches[0]));
  for (int i = 0; i < kNoteCount - 1; i++) {
    if (bestFreq >= sNotePitches[i] && bestFreq < sNotePitches[i + 1]) {
      Serial.println((String)"Highest note found: " + i + (String)" - Note freq: " + sNotePitches[i]);
      break;
    }
  }

  return bestFreq * 100.0f;
}

// Estimate the lowest reachable frequency for the current DCO using the
// latest [freq -> PWM] calibration data and a polynomial fit, assuming
// an amp compensation (range PWM) of 0. This is conceptually symmetric
// to find_highest_freq(), but instead of a live search we derive the
// estimate from the same interpolation strategy used in calibrate_DCO().
//
// Return value: estimated lowest frequency * 100 (same units as
// calibrationData[] entries and find_highest_freq()).
float find_lowest_freq() {
  // Use amp compensation (range PWM) = 0 as requested.
  ampCompCalibrationVal = 0;

  // We require at least three calibration points (six entries) to build
  // a quadratic fit in the [PWM -> freq] direction. The layout of
  // calibrationData is:
  //   [0]  reserved / lowestFreq placeholder
  //   [1]  reserved
  //   [2]  freq0 * 100
  //   [3]  pwm0
  //   [4]  freq1 * 100
  //   [5]  pwm1
  //   [6]  freq2 * 100
  //   [7]  pwm2
  //   ...
  //
  // If we don't have enough data, just return 0.
  if (chanLevelVoiceDataSize < 8) {
    return 0.0f;
  }

  float f0 = (float)calibrationData[2];  // already freq * 100
  float p0 = (float)calibrationData[3];
  float f1 = (float)calibrationData[4];
  float p1 = (float)calibrationData[5];
  float f2 = (float)calibrationData[6];
  float p2 = (float)calibrationData[7];

  // Guard against degenerate cases where the PWMs are identical.
  if (p0 == p1 || p1 == p2 || p0 == p2) {
    // Fall back to a simple linear extrapolation using the first segment.
    float y = linearInterpolation(p0, f0, p1, f1, 0.0f);
    return y;
  }

  // Fit a quadratic in the space PWM -> (freq * 100) and evaluate it at
  // PWM = 0 to estimate the lowest reachable frequency at amp=0.
  float estFreqTimes100 = quadraticInterpolation(
    p0, f0,
    p1, f1,
    p2, f2,
    0.0f
  );

  // Clamp to a sensible minimum to avoid negative or zero frequencies
  // from extreme extrapolation.
  if (estFreqTimes100 < 0.0f) {
    estFreqTimes100 = 0.0f;
  }

  Serial.println((String)"[LOWEST_FREQ_EST] DCO=" + currentDCO +
                 (String)" estFreq*100=" + estFreqTimes100 +
                 (String)" using PWM points {" + p0 + "," + p1 + "," + p2 + "}");

  return estFreqTimes100;
}

// Build the [frequency -> amplitude PWM] calibration table for the DCO in ctx.
// For each calibration note it:
//  - Picks an initial PWM guess (via interpolation),
//  - Searches locally for the PWM that makes the duty error closest to zero,
//  - Stores the best PWM together with the note frequency in ctx.calibrationData.
// dutyErrorFraction controls how much duty-cycle error (e.g. 0.005 = 0.5%)
// is tolerated before the search stops for each note.
void calibrate_DCO(DCOCalibrationContext& ctx, double dutyErrorFraction) {

  const int rangeSamples = 2;  // Number of neighbour voltages to probe around a sign change.
  const int numPresetVoltages = chanLevelVoiceDataSize;  // Size of the [freq, pwm] table.

  // Per-note search guards: a dead oscillator or an unreachable tolerance
  // must not hang the whole calibration run.
  const int           kMaxSearchIterations   = 300;
  const unsigned long kMaxNoteSearchMs       = 30000;
  const int           kMaxConsecutiveTimeouts = 20;

  for (int j = 4; j < numPresetVoltages; j += 2) {  // Start from the 3rd preset voltage

    ctx.currentNote = DCO_calibration_start_note + (calibration_note_interval * (j - 4) / 2);
    VOICE_NOTES[0] = ctx.currentNote;
    uint16_t currentAmpCompCalibrationVal = compute_initial_amp_for_note(ctx, j);

    if (currentAmpCompCalibrationVal > DIV_COUNTER * 0.98) {
      // When we hit the top of the usable PWM range, stop the table here.
      // Record the highest reachable frequency at the current PWM, and also
      // estimate the lowest reachable frequency at PWM=0 so that the first
      // table entry remains a true "lowest note" anchor.
      float highestFreqFound = find_highest_freq();  // Hz * 100
      float lowestFreqCalc   = find_lowest_freq();   // Hz * 100, at PWM=0

      // Store the highest reachable point at this index.
      ctx.calibrationData[j]     = (uint32_t)highestFreqFound;
      ctx.calibrationData[j + 1] = DIV_COUNTER;

      // Ensure entry 0 continues to represent the lowest frequency at PWM=0.
      ctx.calibrationData[0] = (uint32_t)lowestFreqCalc;
      ctx.calibrationData[1] = 0;

      for (int i = j + 2; i < numPresetVoltages; i += 2) {
        ctx.calibrationData[i] = 20000000;
        ctx.calibrationData[i + 1] = DIV_COUNTER;
      }
      break;
    }

    const uint16_t minAmpComp = currentAmpCompCalibrationVal * 0.8;  // Lower limit for this note.
    const uint16_t maxAmpComp = currentAmpCompCalibrationVal * 1.3;  // Upper limit for this note.

    const double freqHz = note_to_freq(VOICE_NOTES[0]);
    double tolerance = compute_gap_tolerance_for_freq(freqHz, dutyErrorFraction);

    // For debugging, report the effective duty-cycle tolerance in percent.
    const double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;
    double toleranceDutyPercent = 0.0;
    if (periodUs > 0.0) {
      toleranceDutyPercent = (tolerance / (2.0 * periodUs)) * 100.0;
    }

    Serial.println((String) "Current DCO: " + ctx.dcoIndex);
    Serial.println((String) "Calibration note: " + VOICE_NOTES[0]);
    Serial.println((String) "Calibration note freq: " + freqHz);
    Serial.println((String) "Calibration note amplitude: " + currentAmpCompCalibrationVal);
    Serial.println((String) "Tolerance (us): " + tolerance);
    Serial.println((String) "Tolerance duty approx (%): " + toleranceDutyPercent);
    Serial.println((String) "MinAmpComp: " + minAmpComp);
    Serial.println((String) "MaxAmpComp: " + maxAmpComp);

    voice_task_autotune(0, currentAmpCompCalibrationVal);  // Send the preset voltage
    delay(10);

    uint16_t bestAmpComp = currentAmpCompCalibrationVal;  // Best PWM found so far for this note.
    float closestToZero = 50000;   // Smallest absolute duty error seen so far.
    float previousAvgValue = 0.0;  // Duty error from the previous iteration (for sign-change detection).

    float lowerMeasurements[rangeSamples];   // Duty errors measured at lower neighbour PWMs.
    float higherMeasurements[rangeSamples];  // Duty errors measured at higher neighbour PWMs.
    uint16_t lowerVoltages[rangeSamples];    // PWM values used for lowerMeasurements[].
    uint16_t higherVoltages[rangeSamples];   // PWM values used for higherMeasurements[].

    int flipCounter = 0;  // Count of successive sign changes; used to relax tolerance if the search oscillates.
    int consecutiveTimeouts = 0;
    unsigned long noteSearchStartMs = millis();

    for (int iteration = 0;; ++iteration) {
      if (iteration >= kMaxSearchIterations ||
          (millis() - noteSearchStartMs) > kMaxNoteSearchMs) {
        Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                       (String)" DCO=" + ctx.dcoIndex +
                       (String)" search guard tripped after " + iteration +
                       (String)" iterations; keeping best AMP=" + bestAmpComp);
        break;
      }

      float avgValue = measure_gap_for_amp(currentAmpCompCalibrationVal);

      // Optional debug: report current duty and tolerance when enabled.
      // Treat timeout sentinel specially so we don't fake a 50% duty reading.
      if (autotuneDebug >= 2 && periodUs > 0.0) {
        if (avgValue == kGapTimeoutSentinel) {
          Serial.println((String)"[DCO_AMP_SCAN] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" AMP=" + currentAmpCompCalibrationVal +
                         (String)" gap=TIMEOUT" +
                         (String)" duty=NA target=50% tol≈" + toleranceDutyPercent + "%");
        } else {
          // avgValue sign convention: positive => amplitude too low.
          double dutyErrorFrac = (double)avgValue / (2.0 * periodUs);
          double dutyPercent   = (0.5 + dutyErrorFrac) * 100.0;
          Serial.println((String)"[DCO_AMP_SCAN] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" AMP=" + currentAmpCompCalibrationVal +
                         (String)" gap=" + avgValue +
                         (String)"us duty=" + dutyPercent +
                         (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
        }
      }

      // Timeout: no usable signal at this PWM. The most common cause is an
      // amplitude too low for the calibration comparator, so nudge the PWM up
      // one step and measure again. previousAvgValue is deliberately left
      // untouched so the sentinel cannot fake a sign change, and the sentinel
      // is never allowed into the best-candidate tracking below.
      if (avgValue == kGapTimeoutSentinel) {
        ++consecutiveTimeouts;
        if (consecutiveTimeouts >= kMaxConsecutiveTimeouts) {
          Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" too many consecutive timeouts; keeping best AMP=" + bestAmpComp);
          break;
        }
        if (currentAmpCompCalibrationVal < maxAmpComp) {
          currentAmpCompCalibrationVal += 1;
        }
        continue;
      }
      consecutiveTimeouts = 0;

      // Update best candidate if this measurement is closer to zero.
      if (abs(avgValue) < abs(closestToZero)) {
        closestToZero = avgValue;
        bestAmpComp = currentAmpCompCalibrationVal;
      }

      // Detect sign change
      if (did_sign_change(previousAvgValue, avgValue)) {
        // Store measurements around the current voltage
        for (int i = 0; i < rangeSamples; i++) {
          uint16_t lowerVoltage = currentAmpCompCalibrationVal - (i + 1);
          uint16_t higherVoltage = currentAmpCompCalibrationVal + (i + 1);

          lowerMeasurements[i] = measure_gap_for_amp(lowerVoltage);
          lowerVoltages[i] = lowerVoltage;

          higherMeasurements[i] = measure_gap_for_amp(higherVoltage);
          higherVoltages[i] = higherVoltage;
        }

        update_best_from_neighbours(
          rangeSamples,
          lowerMeasurements,
          lowerVoltages,
          higherMeasurements,
          higherVoltages,
          avgValue,
          closestToZero,
          bestAmpComp,
          currentAmpCompCalibrationVal
        );

        // Break the loop if the closest value is within tolerance
        if (abs(closestToZero) <= tolerance) {
          break;
        } else {
          tolerance = tolerance * 1.2;
        }
        flipCounter++;
        if (flipCounter >= 3 && abs(closestToZero) <= tolerance * 2) {
          break;
        } else {
          tolerance = tolerance * 1.5;
        }
      }

      // Step the PWM toward the target and enforce the allowed search window.
      // Stepping is done in int32 so it cannot wrap below zero.
      int32_t nextAmp = (int32_t)currentAmpCompCalibrationVal + step_amp_from_error(avgValue, tolerance);
      if (nextAmp < (int32_t)minAmpComp) nextAmp = (int32_t)minAmpComp;
      if (nextAmp > (int32_t)maxAmpComp) nextAmp = (int32_t)maxAmpComp;

      if ((uint16_t)nextAmp == currentAmpCompCalibrationVal &&
          (nextAmp == (int32_t)minAmpComp || nextAmp == (int32_t)maxAmpComp)) {
        // Stuck at a search bound with the error still pushing outward:
        // the target is not reachable inside the window; keep the best found.
        Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                       (String)" DCO=" + ctx.dcoIndex +
                       (String)" stuck at bound AMP=" + currentAmpCompCalibrationVal +
                       (String)"; keeping best AMP=" + bestAmpComp);
        break;
      }
      currentAmpCompCalibrationVal = (uint16_t)nextAmp;

      previousAvgValue = avgValue;
    }

    store_note_result(ctx, j, bestAmpComp, closestToZero);
  }
}


// 3-point quadratic interpolate y at x. Used by calibrate_DCO helpers / find_lowest_freq.
float quadraticInterpolation(float x0, float y0, float x1, float y1, float x2, float y2, float x) {
  // Calculate the coefficients of the quadratic polynomial
  float a = ((y2 - (x2 * (y1 - y0) + x1 * y0 - x0 * y1) / (x1 - x0)) / (x2 * (x2 - x0 - x1) + x0 * x1));
  float b = ((y1 - y0) / (x1 - x0) - a * (x0 + x1));
  float c = y0 - x0 * (b + a * x0);

  // Use the polynomial to estimate the next value
  return a * x * x + b * x + c;
}

// Log interpolate between two points → uint16. Used by compute_initial_amp_for_note().
uint16_t logarithmicInterpolation(float x0, float y0, float x1, float y1, float x) {
  // Ensure x0 and x1 are not zero or negative to avoid log(0) or log of negative number
  if (x0 <= 0 || x1 <= 0) {
    return 0;  // or handle the error as needed
  }

  // Calculate the constants a and b
  float a = (y1 - y0) / (log(x1) - log(x0));
  float b = y0 - a * log(x0);

  // Calculate the y value at the given x
  float y = a * log(x) + b;

  return (uint16_t)round(y);
}

// Linear interpolate between two points. Used by find_lowest_freq().
float linearInterpolation(float x0, float y0, float x1, float y1, float x) {
  // Ensure x0 and x1 are not the same to avoid division by zero
  if (x0 == x1) {
    return 0;  // or handle the error as needed
  }

  // Calculate the slope (m) of the line
  float m = (y1 - y0) / (x1 - x0);

  // Calculate the y-intercept (b) of the line
  float b = y0 - m * x0;

  // Calculate the y value at the given x
  float y = m * x + b;

  return y;
}

// Solve exponential interpolation for y at x (log-space lerp). Used by initMultiplierTables().
double expInterpolationSolveY(double x, double x0, double x1, double y0, double y1) {
  if (x0 <= 0 || x1 <= 0) {
    // Handle error: x0 and x1 must be greater than 0 for exponential interpolation
    return NAN;
  }

  double log_y0 = log(y0);
  double log_y1 = log(y1);

  double log_y = log_y0 + (log_y1 - log_y0) * (x - x0) / (x1 - x0);

  return exp(log_y);
}
