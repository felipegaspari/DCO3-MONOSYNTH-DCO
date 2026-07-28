
#include "include_all.h"
void init_DCO_calibration() {

  currentDCO = 0;

  VOICE_NOTES[0] = DCO_calibration_start_note;
  DCO_calibration_current_note = DCO_calibration_start_note;

  arrayPos = 0;
  calibrationData[arrayPos] = 0;
  calibrationData[arrayPos + 1] = ampCompLowestFreqVal;
  arrayPos += 2;

  calibrationData[arrayPos] = (uint32_t)(sNotePitches[manual_DCO_calibration_start_note - 12] * 100);
  calibrationData[arrayPos + 1] = initManualAmpCompCalibrationVal[currentDCO];

  arrayPos += 2;

  currentNoteCalibrationStart = micros();
  DCOCalibrationStart = millis();
  DCO_calibration_difference = 10000;
  PIDMinGap = 300;

  samplesNumber = 52;

  sampleTime = (1000000 / sNotePitches[DCO_calibration_current_note - 12]) * ((samplesNumber - 1) / 2);

  PIDLimitsFormula = 78;
  PIDOutputLowerLimit = 70;
  PIDOutputHigherLimit = 100;

  // TURN OFF ALL OSCILLATORS:
  for (int i = 0; i < NUM_OSCILLATORS; i++) {

    uint8_t pioNumber = VOICE_TO_PIO[i];
    PIO pioN = pio[VOICE_TO_PIO[i]];
    uint8_t sm1N = VOICE_TO_SM[i];

    uint32_t clk_div1 = 200;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
    pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), 0);
  }

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    PW[i] = DIV_COUNTER_PW / 2;
    pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), PW[i]);
  }

  // DISABLE PW PWM
  // for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
  //   PW[i] = PW_CENTER[i];
  //   pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), PW[i]);
  //   pwm_set_enabled(PW_PWM_SLICES[i], false);
  // }

  voice_task_autotune(0, PIDLimitsFormula);  //what value goes here?

  delay(100);

  DCO_calibration_difference = 4000;
  lastDCODifference = 50000;
  lastGapFlipCount = 0;
  lastPIDgap = 50000;
  bestGap = 50000;
  bestCandidate = 50000;
  lastampCompCalibrationVal = 0;
  edgeDetectionLastTime = 0;
  PIDMinGapCounter = 0;
  pulseCounter = 0;
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/
void DCO_calibration() {

  // TURN OFF ALL OSCILLATORS:
  for (int i = 0; i < NUM_OSCILLATORS; i++) {

    uint8_t pioNumber = VOICE_TO_PIO[i];
    PIO pioN = pio[VOICE_TO_PIO[i]];
    uint8_t sm1N = VOICE_TO_SM[i];

    uint32_t clk_div1 = 200;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
    pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), 0);
  }

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    PW[i] = DIV_COUNTER_PW / 2;
    pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), PW[i]);
  }

  // PW is per-voice (not per-osc): calibrate once before amp-comp loop.
  currentDCO = 0;
  find_PW_center(0);
  pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), PW_CENTER[0]);

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    currentDCO = i;

    restart_DCO_calibration();

    ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];
    pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), ampCompCalibrationVal);

    DCO_calibration_current_note = DCO_calibration_start_note;
    VOICE_NOTES[0] = DCO_calibration_current_note;

    // calibrationData[0] = lowestFrequency;

    calibrate_DCO();

    for (int j = 0; j < chanLevelVoiceDataSize; j++) {
      Serial.println(calibrationData[j]);
    }

    update_FS_voice(currentDCO);

    Serial.println((String) "DCO " + currentDCO + (String) " calibration finished.");

    restart_DCO_calibration();
  }
  calibrationFlag = false;
  init_FS();
  precomputeCoefficients();
}
/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

void restart_DCO_calibration() {

  VOICE_NOTES[0] = DCO_calibration_start_note;
  DCO_calibration_current_note = DCO_calibration_start_note;

  arrayPos = 0;
  calibrationData[arrayPos] = 0;
  calibrationData[arrayPos + 1] = ampCompLowestFreqVal;
  arrayPos += 2;

  calibrationData[arrayPos] = (uint32_t)(sNotePitches[DCO_calibration_current_note - calibration_note_interval - 12] * 100);
  calibrationData[arrayPos + 1] = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  arrayPos += 2;

  currentNoteCalibrationStart = micros();
  DCOCalibrationStart = millis();
  DCO_calibration_difference = 10000;
  PIDMinGap = 300;


  // TURN OFF ALL OSCILLATORS:
  for (int i = 0; i < NUM_OSCILLATORS; i++) {

    uint8_t pioNumber = VOICE_TO_PIO[i];
    PIO pioN = pio[VOICE_TO_PIO[i]];
    uint8_t sm1N = VOICE_TO_SM[i];

    uint32_t clk_div1 = 200;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
    pwm_set_chan_level(RANGE_PWM_SLICES[i], pwm_gpio_to_channel(RANGE_PINS[i]), 0);
  }

  delay(100);

  DCO_calibration_difference = 4000;
  lastDCODifference = 50000;
  lastGapFlipCount = 0;
  lastPIDgap = 50000;
  bestGap = 50000;
  bestCandidate = 50000;
  lastampCompCalibrationVal = 0;
  edgeDetectionLastTime = 0;
  PIDMinGapCounter = 0;
  pulseCounter = 0;
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

void find_PW_center(uint8_t mode) {

  uint16_t targetGap;
  uint8_t voiceTaskMode;

  if (mode == 0) {
    DCO_calibration_current_note = manual_DCO_calibration_start_note;
    VOICE_NOTES[0] = DCO_calibration_current_note;
    targetGap = 20;
    voiceTaskMode = 2;
  } else {
    DCO_calibration_current_note = 76;
    VOICE_NOTES[0] = DCO_calibration_current_note;
    targetGap = 5;
    voiceTaskMode = 3;
  }

  currentNoteCalibrationStart = micros();
  DCOCalibrationStart = millis();



  PIDOutputLowerLimit = 0;
  PIDOutputHigherLimit = DIV_COUNTER_PW;

  ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  if (firstTuneFlag == true) {
    PW[0] = DIV_COUNTER_PW / 2;
    PWCalibrationVal = DIV_COUNTER_PW / 2;
    PW_CENTER[0] = DIV_COUNTER_PW / 2;
  } else {

    PW[0] = PW_CENTER[0];
    PWCalibrationVal = PW_CENTER[0];
  }

  voice_task_autotune(voiceTaskMode, ampCompCalibrationVal);

  DCO_calibration_difference = 4000;
  lastDCODifference = 50000;
  lastGapFlipCount = 0;
  lastPIDgap = 50000;
  bestGap = 50000;
  bestCandidate = 50000;
  lastampCompCalibrationVal = 0;
  edgeDetectionLastTime = 0;
  PIDMinGapCounter = 0;
  pulseCounter = 0;

  while (bestGap > targetGap) {

    pwm_set_chan_level(PW_PWM_SLICES[0], pwm_gpio_to_channel(PW_PINS[0]), PWCalibrationVal);

    delay(30);

    double PIDgap;
    double difference;
    double currentGap = find_gap(2);  // distance away from setpoint


    if (currentGap != 1.16999f) {
      PIDgap = abs(currentGap);
      difference = currentGap;
    } else {
      difference = lastDCODifference;
      PIDgap = lastPIDgap;
    }

    if (PIDgap < bestGap) {
      bestGap = PIDgap;
      bestCandidate = PWCalibrationVal;
    }

    bool calibrationSwing = false;

    if ((difference < 0 && lastDCODifference > 0) || (difference > 0 && lastDCODifference < 0)) {
      lastGapFlipCount++;
      if (lastGapFlipCount >= 4) {
        Serial.println("*********************/*/*/*/*/*/  FLIP !!! *********************/*/*/*/*/*/*********");

        calibrationSwing = true;
      }
    } else {
      lastGapFlipCount = 0;
    }

    if (bestGap < targetGap || calibrationSwing == true) {

      PIDMinGapCounter++;

      if (PIDMinGapCounter >= 2) {
        if (autotuneDebug >= 1) {
          Serial.println((String)(String) " - Gap = " + PIDgap + " - MIN Gap: " + (targetGap) + (String) " - " + DCO_calibration_current_note);
        }
        Serial.println((String) "Final gap = " + PIDgap);
        break;
      }
    }

    if (autotuneDebug >= 1) {
      Serial.println((String) " - GAP = " + PIDgap + " - MIN GAP: " + (targetGap) + (String) " -  NOTE: " + DCO_calibration_current_note);
    }

    lastDCODifference = difference;
    lastPIDgap = PIDgap;

    if (difference > 0.00) {
      if (PIDgap >= 4000) {
        PWCalibrationVal += 5;
      } else if (PIDgap > 2000) {
        PWCalibrationVal += 5;
      } else if (PIDgap > 1200) {
        PWCalibrationVal += 2;
      } else {
        PWCalibrationVal++;
      }
    } else if (DCO_calibration_difference < 0.00) {
      if (PIDgap >= 4000) {
        PWCalibrationVal -= 5;
      } else if (PIDgap > 2000) {
        PWCalibrationVal -= 3;
      } else if (PIDgap > 1200) {
        PWCalibrationVal -= 2;
      } else {
        PWCalibrationVal--;
      }
    }
    if (PWCalibrationVal > DIV_COUNTER_PW) { PWCalibrationVal = 0; }

    if (autotuneDebug >= 1) {
      Serial.println((String) "PWCalibrationVal: " + PWCalibrationVal);
    }
  }
  Serial.println("PW center found !!!");
  update_FS_PWCenter(0, bestCandidate);  // PW is per-voice
  PW_CENTER[0] = bestCandidate;
}

/////////////////////////////////
////////////////////////////////
////////////////////////////////

float find_gap(byte specialMode) {
  if (specialMode == 2) {  // find lowest freq mode
    samplesNumber = 14;
  } else {
    samplesNumber = 10;
  }

  edgeDetectionLastTime = micros();
  

  while (samplesCounter < samplesNumber) {

    bool val = digitalRead(DCO_calibration_pin);
    microsNow = micros();
    if ((microsNow - edgeDetectionLastTime) > 100000) {

      pulseCounter = 0;
      samplesCounter = 0;
      DCO_calibration_difference = 1.16999f;
      val = 0;
      edgeDetectionLastVal = 0;

      if (autotuneDebug >= 3) {
        Serial.println("Timeout loop");
        Serial.println((String) "ampCompCalibrationVal: " + ampCompCalibrationVal);
      }

      microsNow = micros();
      edgeDetectionLastTime = microsNow;

      return 1.16999f;
    }
    if (val != edgeDetectionLastVal) {
      if ((microsNow - edgeDetectionLastTime) >= 30) {

        edgeDetectionLastVal = val;

        if (pulseCounter == 1 && val == 0) {
          pulseCounter == 0;
        }
        if (pulseCounter > 2) {
          if (val == 0) {
            fallingEdgeTimeSum += microsNow - edgeDetectionLastTime;
          } else {
            risingEdgeTimeSum += microsNow - edgeDetectionLastTime;
          }
          samplesCounter++;
        }
        edgeDetectionLastTime = microsNow;
        pulseCounter++;
      }
    }
  }

  if (samplesCounter == samplesNumber) {

    DCO_calibration_difference = float((float)fallingEdgeTimeSum / float((float)samplesNumber / 4.00f) 
    - float((float)risingEdgeTimeSum / float((float)samplesNumber / 4.00f))); // this is the difference between 50% duty cyclce and the actual duty cycle, divided by 4 to allow more coarse calibration.

    if (autotuneDebug >= 1) {
      Serial.println((String) "DCO_calibration_difference: " + DCO_calibration_difference);
      Serial.println((String) "NOTE: " + DCO_calibration_current_note);
    }

    
    pulseCounter = 0;
    samplesCounter = 0;
    risingEdgeTimeSum = 0;
    fallingEdgeTimeSum = 0;
    edgeDetectionLastVal = 0;

  } else {
    return 1.16999f;
  }
  return (float)DCO_calibration_difference;
}
