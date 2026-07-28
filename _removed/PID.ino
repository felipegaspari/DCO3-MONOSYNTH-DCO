// Removed unused code from PID.ino

// --- from original:9-148 ---

bool PID_dco_calibration() {

  PIDInput = (double)constrain(DCO_calibration_difference, -1500, 1500);

  double PIDgap = abs(PIDSetpoint - PIDInput);  //distance away from setpoint

  if (micros() - currentNoteCalibrationStart > 15000000 && ampCompCalibrationVal > (DIV_COUNTER * 0.98)) {
    Serial.println("Find highest freq");
    Serial.println((String) "Total time: " + (millis() - DCOCalibrationStart));
    calibrationFlag = false;
    DCO_calibration_difference = 2;
    return true;

  } else if (micros() - currentNoteCalibrationStart > 20000000 && micros() - PIDComputeTimer > sampleTime) {
    PIDMinGap = PIDMinGap * 1.02;
  } else if (micros() - currentNoteCalibrationStart > 10000000 && micros() - PIDComputeTimer > sampleTime) {
    PIDMinGap = PIDMinGap * 1.01;
  }

  if (PIDgap < bestGap) {
    bestGap = PIDgap;
    bestCandidate = ampCompCalibrationVal;
  }

  bool calibrationSwing = false;
  if ((DCO_calibration_difference < 0 && lastDCODifference > 0) || (DCO_calibration_difference > 0 && lastDCODifference < 0)) {
    lastGapFlipCount++;
    if (lastGapFlipCount >= 4) {
      if (DCO_calibration_current_note > 50) {
        PIDMinGap = PIDMinGap * 1.01;
      } else {
        PIDMinGap = PIDMinGap * 1.05;
      }
    }
    if (lastGapFlipCount >= 8) {
      Serial.println("*********************/*/*/*/*/*/  FLIP !!! *********************/*/*/*/*/*/*********");

      calibrationSwing = true;
    }
  } else {
    lastGapFlipCount = 0;
  }

  if (PIDgap < PIDMinGap || calibrationSwing == true) {

    PIDMinGapCounter++;

    if (PIDMinGapCounter >= 2) {
      if (autotuneDebug >= 1) {
        Serial.println((String)(String) " - Gap = " + PIDgap + " - MIN Gap: " + (PIDMinGap) + (String) " - " + DCO_calibration_current_note);
      }

      calibrationData[arrayPos] = (uint32_t)(sNotePitches[DCO_calibration_current_note - 12] * 100);
      calibrationData[arrayPos + 1] = (uint32_t)bestCandidate;  //bestCandidate;
      arrayPos += 2;

      Serial.println((uint32_t)(sNotePitches[DCO_calibration_current_note - 12] * 100) + (String) ", " + ampCompCalibrationVal + (String) ",");
      Serial.println((String) "Final gap = " + PIDgap + (String) " |||| NOTE: " + DCO_calibration_current_note + (String) " |||| Note calibration time(s): " + ((micros() - currentNoteCalibrationStart) / 1000000));


      DCO_calibration_current_note = DCO_calibration_current_note + calibration_note_interval;
      VOICE_NOTES[0] = DCO_calibration_current_note;

      currentNoteCalibrationStart = micros();
      //PIDMinGap = (1240.6114554 * pow(0.9924189, (double)ampCompCalibrationVal)) * 0.05;

      PIDMinGap = (37701.182837 * pow(0.855327, (double)DCO_calibration_current_note));

      PIDLimitsFormula = ((1.364 * (double)(ampCompCalibrationVal)) - 12) * 1.05;
      PIDOutputLowerLimit = PIDLimitsFormula * 0.8;
      PIDOutputHigherLimit = PIDLimitsFormula * 1.05;

      Serial.println((String) "Next MinGap: " + PIDMinGap);
      Serial.println((String) "PIDOutputLowerLimit: " + PIDOutputLowerLimit + (String) " PIDOutputHigherLimit: " + PIDOutputHigherLimit);
      Serial.println("  ----------------------------------------------------------------- ");

      if (PIDOutputHigherLimit >= (DIV_COUNTER * 0.98)) {
        Serial.println("Find highest freq");
        Serial.println((String) "Total time: " + ((millis() - DCOCalibrationStart) / 1000));
        calibrationFlag = false;
        DCO_calibration_difference = 2;
        return true;
      }

      ampCompCalibrationVal = PIDLimitsFormula;

      sampleTime = (1000000 / sNotePitches[DCO_calibration_current_note - 12]) * ((samplesNumber - 1) / 2);
      if (sampleTime < 8000) sampleTime = 8000;

      voice_task_autotune(0, ampCompCalibrationVal);
      delay(50);

      DCO_calibration_difference = 4000;
      if (DCO_calibration_current_note == 12) DCO_calibration_difference = 3000;
      


      lastDCODifference = 50000;
      lastGapFlipCount = 0;
      lastPIDgap = 50000;
      bestGap = 50000;
      bestCandidate = 50000;
      lastampCompCalibrationVal = 0;
      PIDMinGapCounter = 0;

      return false;
    }
  }

  if (autotuneDebug >= 1) {
    Serial.println((String) " - GAP = " + PIDgap + " - MIN GAP: " + (PIDMinGap) + (String) " -  NOTE: " + DCO_calibration_current_note);
  }

  lastDCODifference = DCO_calibration_difference;
  lastPIDgap = PIDgap;
  lastampCompCalibrationVal = ampCompCalibrationVal;

  if (DCO_calibration_difference > 0.00) {
    if (DCO_calibration_difference > PIDMinGap * 20) {
      ampCompCalibrationVal += 2;
    } else {
      ampCompCalibrationVal++;
    }
  } else if (DCO_calibration_difference < 0.00) {
    if (abs(DCO_calibration_difference) > PIDMinGap * 20) {
      ampCompCalibrationVal -= 2;
    } else {
      ampCompCalibrationVal--;
    }
  }

  //ampCompCalibrationVal = constrain(ampCompCalibrationVal, PIDOutputLowerLimit, PIDOutputHigherLimit);

  if (autotuneDebug >= 1) {
    Serial.println((String) "ampCompCalibrationVal: " + ampCompCalibrationVal + (String) " -- PIDOutputLowerLimit: " + PIDOutputLowerLimit);
    Serial.print((String) " PIDOutputHigherLimit: " + PIDOutputHigherLimit + (String) " - DCO: " + currentDCO);
  }
  return false;
}

// --- from original:149-182 ---

void PID_find_highest_freq() {

  ampCompCalibrationVal = DIV_COUNTER;
  PIDTuningMultiplier = 0.28752775 * pow(1.00408722, 1779);
  PIDTuningMultiplierKi = 0.33936558 * pow(1.00702176, 1779);
  PIDInput = 100;
  myPID.SetOutputLimits(sNotePitches[DCO_calibration_current_note - 12 - calibration_note_interval], sNotePitches[DCO_calibration_current_note - 12 + calibration_note_interval]);
  myPID.SetTunings(0.01, 1, 0.0005);
  myPID.SetSampleTime(5);
  while (abs(DCO_calibration_difference) > 0.5) {
    voice_task_autotune(1, DIV_COUNTER);

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
}

// --- from original:219-312 ---

float find_lowest_freq() {

  DCO_calibration_difference = 1000;

  ampCompCalibrationVal = ampCompLowestFreqVal;
  PIDTuningMultiplier = 0.28752775 * pow(1.00408722, 1779);
  PIDTuningMultiplierKi = 0.33936558 * pow(1.00702176, 1779);
  PIDInput = 100;

  double PIDLowerLimit = 2;
  double PIDUpperLimit = sNotePitches[manual_DCO_calibration_start_note ];
  uint16_t targetGap = 100;

  myPID.SetOutputLimits(PIDLowerLimit, PIDLowerLimit);
  myPID.SetTunings(0.000004, 0.000007, 0.0000001);
  myPID.SetSampleTime(300);

  int validValuesArraySize = 50;
  double gapArray[validValuesArraySize];
  double validValuesArray[validValuesArraySize];

  double newLowerLimit = PIDLowerLimit;
  double newUpperLimit = PIDUpperLimit;

  double closestToZeroNegativeGap = -1000000;
  double closestToZeroPositiveGap = 1000000;
  double closestToZero = 1000000;
  double bestGap = 1000000;

  bool lowestFreqFound = false;

  voice_task_autotune(4, PIDLowerLimit);
  delay(300);

  while (abs(bestGap) > targetGap) {
    if (lowestFreqFound) {
      break;
    }
    for (int i = 0; i < validValuesArraySize; i++) {

      PIDOutput = (double)newLowerLimit + ((double)i * (double)((newUpperLimit - newLowerLimit) / (double)validValuesArraySize));
      validValuesArray[i] = PIDOutput;

      voice_task_autotune(4, PIDOutput);
      delay(150);
      gapArray[i] = (double)find_gap(2);

      Serial.println((String) "validValuesArray: " + validValuesArray[i]);
      Serial.println((String) "gapArray: " + gapArray[i]);
      Serial.println((String) "PIDOutput: " + PIDOutput);
      Serial.println((String) "i: " + i);



      if (gapArray[i] != 1.16999f) {
        if (abs(gapArray[i]) < targetGap) {
          closestToZero = PIDOutput;
          lowestFreqFound = true;
          break;
        }
        if (gapArray[i] < 0 && closestToZeroNegativeGap < gapArray[i]) {
          closestToZeroNegativeGap = gapArray[i];
          newLowerLimit = validValuesArray[i] - (validValuesArray[i] * 0.02);
        }
        if (gapArray[i] > 0 && gapArray[i] < closestToZeroPositiveGap) {
          closestToZeroPositiveGap = gapArray[i];
          newUpperLimit = validValuesArray[i] + (validValuesArray[i] * 0.02);
        }

        if (abs(newUpperLimit) < abs(newLowerLimit)) {
          closestToZero = newUpperLimit;
        } else {
          closestToZero = newLowerLimit;
        }

        if (abs(closestToZeroNegativeGap) < abs(closestToZeroPositiveGap)) {
          bestGap = closestToZeroNegativeGap;
        } else {
          bestGap = closestToZeroPositiveGap;
        }
      }
    }

    Serial.println((String) "newLowerLimit: " + newLowerLimit);
    Serial.println((String) "newUpperLimit: " + newUpperLimit);

    myPID.SetOutputLimits(newLowerLimit, newUpperLimit);
    PIDInput = (double)-100;
    PIDOutput = (double)newUpperLimit - (double)newLowerLimit;
  }
  Serial.println((String) "Lowest freq found: " + PIDOutput);
  return closestToZero * 100;
}

// --- from original:480-495 ---

uint16_t exponentialInterpolation(float x0, float y0, float x1, float y1, float x) {
  // Ensure y0 and y1 are not zero to avoid log(0)
  if (y0 <= 0 || y1 <= 0) {
    return 0;  // or handle the error as needed
  }

  // Calculate the constants a and b
  float b = log(y1 / y0) / (x1 - x0);
  float a = y0 / exp(b * x0);

  // Calculate the y value at the given x
  float y = a * exp(b * x);

  return round(y);
}

// --- from original:512-527 ---

float logarithmicInterpolationFloat(float x0, float y0, float x1, float y1, float x) {
  // Ensure x0 and x1 are not zero or negative to avoid log(0) or log of negative number
  if (x0 <= 0 || x1 <= 0) {
    return 0;  // or handle the error as needed
  }

  // Calculate the constants a and b
  float a = (y1 - y0) / (logf(x1) - logf(x0));
  float b = y0 - a * logf(x0);

  // Calculate the y value at the given x
  float y = a * logf(x) + b;

  return y;
}

// --- from original:528-543 ---

double logarithmicInterpolationDouble(double x0, double y0, double x1, double y1, double x) {
  // Ensure x0 and x1 are not zero or negative to avoid log(0) or log of negative number
  if (x0 <= 0 || x1 <= 0) {
    return 0;  // or handle the error as needed
  }

  // Calculate the constants a and b
  double a = (y1 - y0) / (logf(x1) - logf(x0));
  double b = y0 - a * logf(x0);

  // Calculate the y value at the given x
  double y = a * logf(x) + b;

  return y;
}

// --- from original:544-561 ---

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
