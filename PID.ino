#include "include_all.h"
void init_PID() {
  //initialize the variables we're linked to
  PIDInput = -2000;
  PIDSetpoint = 0;

  myPID.SetMode(AUTOMATIC);
}

float find_highest_freq() {

  ampCompCalibrationVal = DIV_COUNTER;
  PIDTuningMultiplier = 0.28752775 * pow(1.00408722, 1779);
  PIDTuningMultiplierKi = 0.33936558 * pow(1.00702176, 1779);
  PIDInput = 100;
  myPID.SetOutputLimits(sNotePitches[DCO_calibration_current_note - 12 - calibration_note_interval], sNotePitches[DCO_calibration_current_note - 12 + calibration_note_interval]);
  myPID.SetTunings(0.01, 1.2, 0.002);
  myPID.SetSampleTime(5);

  while (abs(DCO_calibration_difference) > 0.5) {
    voice_task_autotune(4, DIV_COUNTER);
    delay(4);
    find_gap(0);
    PIDInput = 0 - (double)DCO_calibration_difference;

    myPID.Compute();

    if (autotuneDebug >= 1) {
      Serial.println((String) "Pid output: " + PIDOutput + (String) " Pid gap: " + DCO_calibration_difference);
    }
  }
  Serial.println((String) "Highest freq found: " + PIDOutput);

  //find highest note
  for (int i = 0; i < sizeof(sNotePitches); i++) {
    if (PIDOutput > sNotePitches[i] && PIDOutput < sNotePitches[i + 1]) {
      highestNoteOSC[currentDCO] = i;
      Serial.println((String) "Highest note found: " + i + (String) " - Note freq: " + sNotePitches[i]);
      break;
    }
  }

  return PIDOutput * 100;
}

void calibrate_DCO() {

  double tolerance;      // minGap
  uint16_t minAmpComp;   // Lower Limit
  uint16_t maxAmpComp;   //  Higher Limit
  int rangeSamples = 2;  // Number of measurements to store around the sign change
  const int numPresetVoltages = chanLevelVoiceDataSize;

  for (int j = 4; j < numPresetVoltages; j += 2) {  // Start from the 3rd preset voltage
    uint16_t currentAmpCompCalibrationVal;

    DCO_calibration_current_note = DCO_calibration_start_note + (calibration_note_interval * (j - 4) / 2);
    VOICE_NOTES[0] = DCO_calibration_current_note;
    if (j == 4) {
      currentAmpCompCalibrationVal = (initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO])* 1.35;
    } else if (j == 6) {
      currentAmpCompCalibrationVal = logarithmicInterpolation(calibrationData[2], calibrationData[3], calibrationData[4], calibrationData[5], sNotePitches[DCO_calibration_current_note - 12] * 100);
    } else {
      currentAmpCompCalibrationVal = quadraticInterpolation(calibrationData[j - 6], calibrationData[j - 5], calibrationData[j - 4], calibrationData[j - 3], calibrationData[j - 2], calibrationData[j - 1], sNotePitches[DCO_calibration_current_note - 12] * 100);
    }

    if (currentAmpCompCalibrationVal > DIV_COUNTER * 0.98) {
      float highestFreqFound = find_highest_freq();
      calibrationData[j] = highestFreqFound;
      calibrationData[j + 1] = DIV_COUNTER;

      for (int i = j + 2; i < numPresetVoltages; i += 2) {
        calibrationData[i] = 20000000;
        calibrationData[i + 1] = DIV_COUNTER;
      }
      break;
    }

    uint16_t minAmpComp = currentAmpCompCalibrationVal * 0.8;  // Lower Limit
    uint16_t maxAmpComp = currentAmpCompCalibrationVal * 1.3;  //  Higher Limit

    //tolerance = (double)(37701.182837 * pow(0.855327, (double)DCO_calibration_current_note));
    tolerance = (double)1000000.00 / (double)sNotePitches[VOICE_NOTES[0] - 12] /  (double)sNotePitches[VOICE_NOTES[0] - 12] / 4.00d;

    Serial.println((String) "Current DCO: " + currentDCO);
    Serial.println((String) "Calibration note: " + VOICE_NOTES[0]);
    Serial.println((String) "Calibration note freq: " + sNotePitches[VOICE_NOTES[0] - 12]);
    Serial.println((String) "Calibration note amplitude: " + currentAmpCompCalibrationVal);
    Serial.println((String) "Tolerance: " + tolerance);
    Serial.println((String) "MinAmpComp: " + minAmpComp);
    Serial.println((String) "MaxAmpComp: " + maxAmpComp);

    voice_task_autotune(0, currentAmpCompCalibrationVal);  // Send the preset voltage
    delay(10);

    uint16_t bestAmpComp = currentAmpCompCalibrationVal;
    float closestToZero = 50000;  // Initialize with a large value
    float previousAvgValue = 0.0;

    float lowerMeasurements[rangeSamples];
    float higherMeasurements[rangeSamples];
    uint16_t lowerVoltages[rangeSamples];
    uint16_t higherVoltages[rangeSamples];

    int flipCounter = 0;

    while (true) {
      voice_task_autotune(0, currentAmpCompCalibrationVal);
      delay(10);
      float avgValue = find_gap(0);

      if (abs(avgValue) < abs(closestToZero && avgValue != 1.16999f)) {
        closestToZero = avgValue;
        bestAmpComp = currentAmpCompCalibrationVal;
      } else {
        avgValue == 0;
      }

      // Detect sign change
      if ((previousAvgValue > 0 && avgValue < 0) || (previousAvgValue < 0 && avgValue > 0) ) {
        // Store measurements around the current voltage
        for (int i = 0; i < rangeSamples; i++) {
          float lowerVoltage = currentAmpCompCalibrationVal - (i + 1);
          float higherVoltage = currentAmpCompCalibrationVal + (i + 1);

          voice_task_autotune(0, lowerVoltage);
          lowerMeasurements[i] = find_gap(0);
          lowerVoltages[i] = lowerVoltage;

          voice_task_autotune(0, higherVoltage);
          higherMeasurements[i] = find_gap(0);
          higherVoltages[i] = higherVoltage;
        }

        // Evaluate stored measurements including the current voltage
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

      // Adjust the voltage based on the measurement
      if (abs(avgValue) < tolerance * 20) {
        if (avgValue > 0) {
          currentAmpCompCalibrationVal += 1;
        } else {
          currentAmpCompCalibrationVal -= 1;
        }
      } else {
        if (avgValue > 0) {
          currentAmpCompCalibrationVal += 2;
        } else {
          currentAmpCompCalibrationVal -= 2;
        }
      }

      // Ensure the voltage stays within the allowed range
      if (currentAmpCompCalibrationVal < minAmpComp || currentAmpCompCalibrationVal > maxAmpComp) {
        Serial.println((String) "Calibration voltage out of range: " + currentAmpCompCalibrationVal);
      }

      previousAvgValue = avgValue;
    }

    calibrationData[j] = sNotePitches[DCO_calibration_current_note - 12] * 100;
    calibrationData[j + 1] = bestAmpComp;  // Store the best voltage for the current preset voltage

    Serial.print("DCO_calibration_current_note ");
    Serial.println(DCO_calibration_current_note);
    Serial.print("Best calibration voltage: ");
    Serial.println(bestAmpComp);
    Serial.print("Closest measurement to zero: ");
    Serial.println(closestToZero);
  }
}


float quadraticInterpolation(float x0, float y0, float x1, float y1, float x2, float y2, float x) {
  // Calculate the coefficients of the quadratic polynomial
  float a = ((y2 - (x2 * (y1 - y0) + x1 * y0 - x0 * y1) / (x1 - x0)) / (x2 * (x2 - x0 - x1) + x0 * x1));
  float b = ((y1 - y0) / (x1 - x0) - a * (x0 + x1));
  float c = y0 - x0 * (b + a * x0);

  // Use the polynomial to estimate the next value
  return a * x * x + b * x + c;
}

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