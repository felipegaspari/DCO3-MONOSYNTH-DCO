#ifndef __AUTOTUNE_H__
#define __AUTOTUNE_H__

#include "include_all.h"
#include "autotune_constants.h"
#include "autotune_measurement.h"
#include "autotune_context.h"

// Global flags controlling calibration routines.
//  - calibrationFlag: a calibration process is currently running.
//  - manualCalibrationFlag: manual calibration mode is active.
//  - firstTuneFlag: true on the very first calibration run after boot/flash.
bool calibrationFlag = false;
bool manualCalibrationFlag = false;
bool firstTuneFlag = false;

// Manual DCO calibration workflow state and per-oscillator manual offsets
// that are added on top of automatic amp compensation.
uint8_t manualCalibrationStage;
int8_t manualCalibrationOffset[NUM_OSCILLATORS] = { 0, 0, 0 };

/************************************************/
/****************** DCO calibration ******************/

// Temporary buffer used during calibration to build [frequency, range-PWM]
// pairs for a single DCO. Persisted via update_FS_voice() when an osc is done.
uint32_t calibrationData[chanLevelVoiceDataSize];

// Index of the DCO currently being calibrated.
uint8_t currentDCO;

// millis() timestamp when the current calibration pass started. Used by the
// PW search phases for their 60 s safety timeouts.
unsigned long DCOCalibrationStart;

// Current range-PWM value used during calibration for the active DCO.
volatile uint16_t ampCompCalibrationVal;

// Frequency override (Hz) consumed by voice_task_autotune() mode 4 during the
// highest-frequency search (replaces the old PID_v1 PIDOutput coupling).
float calibrationFreqHz = 0.0f;

// Baseline manual amp-comp starting value for all oscillators.
int8_t initManualAmpCompCalibrationValPreset = 35;
// weact rp2040 dco //volatile int8_t initManualAmpCompCalibrationVal[NUM_OSCILLATORS] = {24,26,25,25,25,18,20,25};
// Per-oscillator baseline manual amp-comp starting values.
int8_t initManualAmpCompCalibrationVal[NUM_OSCILLATORS] = {
  initManualAmpCompCalibrationValPreset,
  initManualAmpCompCalibrationValPreset,
  initManualAmpCompCalibrationValPreset
};
// Range-PWM value stored as the "lowest frequency" anchor in the calibration
// table header (also persisted by FS.ino when seeding fake tables).
volatile uint16_t ampCompLowestFreqVal = 10;

// Note from which DCO calibration starts (MIDI note index).
static constexpr uint8_t DCO_calibration_start_note = 29; // 29 == C0
// Interval in semitones between successive calibration notes.
static constexpr uint8_t calibration_note_interval = 5;
// Starting note used for manual/PW-centered calibration passes.
static constexpr uint8_t manual_DCO_calibration_start_note = DCO_calibration_start_note - 5;

// Current note used during calibration.
uint8_t DCO_calibration_current_note;

// Global debug verbosity level for autotune routines.
byte autotuneDebug = 4;

// Convert a MIDI note number to its frequency in Hz.
// sNotePitches[] starts at MIDI note 12, hence the offset.
static inline float note_to_freq(uint8_t midiNote) {
  return sNotePitches[midiNote - 12];
}

// --- Implemented in autotune_search.ino ---

// Allowed |gap| in microseconds for a frequency and duty-error fraction.
double compute_gap_tolerance_for_freq(double freqHz, double dutyErrorFraction);

// Main DCO amp-comp calibration routine (search-based).
// dutyErrorFraction specifies the allowed duty-cycle error (e.g. 0.005 = 0.5%).
void calibrate_DCO(DCOCalibrationContext& ctx, double dutyErrorFraction);

// Highest usable frequency at full range PWM (returns Hz * 100).
float find_highest_freq();

// Estimated lowest reachable frequency at range PWM = 0 (returns Hz * 100).
float find_lowest_freq();

// --- PW target-duty search (autotune.ino) ---
// These types live in the header (rather than the .ino) so that the Arduino
// builder's auto-generated prototypes for the search-phase helpers compile.

// Maximum number of valid samples remembered by a PW search.
static constexpr int kPWMaxSamples = 40;

// State shared by the PW target-duty search phases (coarse scan, bisection,
// fine scan, candidate selection). All gap differences are relative to the
// target gap (gap - gapTarget), so "0" always means "exactly on target duty".
struct PWSearchState {
  uint16_t validPW[kPWMaxSamples];       // PW of each stored valid sample
  double   validGapDiff[kPWMaxSamples];  // gap - gapTarget for each sample
  int      validCount;
  int      inToleranceCount;  // valid samples measured within targetGap
  bool     haveBest;          // at least one valid sample was seen
  double   bestGapAbs;        // smallest |gap - gapTarget| seen so far
  uint16_t bestPW;            // PW that produced bestGapAbs
  bool     haveBracket;       // sign-change bracket found during coarse scan
  uint16_t pwLow, pwHigh;     // bracket bounds
  double   gapLow;            // raw gap measured at pwLow
};

// How a sample should enter the valid-samples table.
enum PWRecordMode {
  PW_RECORD_NO_TABLE,       // update best/in-tolerance counters only
  PW_RECORD_APPEND,         // append while there is room
  PW_RECORD_REPLACE_WORST,  // append, or replace the worst entry when full
};

// --- PW limit search (autotune.ino) ---

// Direction selector for the unified PW limit search.
enum PWLimitDir {
  PW_LIMIT_LOW,
  PW_LIMIT_HIGH
};

// Result structure used by the PW-limit search helpers.
struct PWLimitSearchResult {
  bool     ok;                  // true if at least one valid sample was found
  uint16_t limitPW;             // PW value chosen as limit
  double   finalDutyPercent;    // measured duty at limitPW in percent, or < 0 if unknown
};

// Low-level search routine that assumes the DCO is already configured for
// PW calibration on the desired note/voice. It scans from centerPW toward
// the requested direction and returns the PW that best matches targetDuty.
PWLimitSearchResult search_PW_limit_from_center(
  uint8_t     voiceIdx,
  uint16_t    centerPW,
  PWLimitDir  dir,
  double      periodUs,
  double      targetDuty
);

// High-level wrapper that configures the calibration context and commits the
// found limit (LOW or HIGH) to the filesystem and runtime tables.
void find_PW_limit_v2(PWLimitDir dir);


#endif
