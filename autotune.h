#ifndef __AUTOTUNE_H__
#define __AUTOTUNE_H__

#include "include_all.h"

bool calibrationFlag = false;
bool manualCalibrationFlag = false;
bool firstTuneFlag = false;

uint8_t manualCalibrationStage;
int8_t manualCalibrationOffset[NUM_OSCILLATORS] = { 0, 0, 0 };
/************************************************/
/****************** DCO calibration ******************/



uint32_t calibrationData[chanLevelVoiceDataSize];

uint8_t currentDCO;

unsigned long edgeDetectionLastTime;
unsigned long microsNow;
unsigned long currentNoteCalibrationStart;
unsigned long DCOCalibrationStart;

bool edgeDetectionLastVal = 0;

volatile uint16_t ampCompCalibrationVal;
int8_t initManualAmpCompCalibrationValPreset = 30;
// weact rp2040 dco //volatile int8_t initManualAmpCompCalibrationVal[NUM_OSCILLATORS] = {24,26,25,25,25,18,20,25};
int8_t initManualAmpCompCalibrationVal[NUM_OSCILLATORS] = {
  initManualAmpCompCalibrationValPreset,
  initManualAmpCompCalibrationValPreset,
  initManualAmpCompCalibrationValPreset
};
volatile uint16_t ampCompLowestFreqVal = 10;


int pulseCounter = 0;
int samplesCounter = 0;

double risingEdgeTimeSum, fallingEdgeTimeSum;
float DCO_calibration_difference;


uint16_t samplesNumber;


static constexpr uint8_t DCO_calibration_start_note = 23;
static constexpr uint8_t calibration_note_interval = 5;
static constexpr uint8_t manual_DCO_calibration_start_note = DCO_calibration_start_note - 5;

uint8_t DCO_calibration_current_note;
uint8_t DCO_calibration_current_voice;
uint8_t DCO_calibration_current_OSC;

uint8_t highestNoteOSC[NUM_OSCILLATORS];

double lastDCODifference;
uint8_t lastGapFlipCount;
double lastPIDgap;
uint16_t lastampCompCalibrationVal;

uint16_t PWCalibrationVal;

byte autotuneDebug = 4;



#endif
