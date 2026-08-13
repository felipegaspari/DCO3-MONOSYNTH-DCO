#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_removed/autotune.h"

// --- lastDCO tracking vars ---
// History used to detect sign flips and convergence behaviour.
double lastDCODifference;
uint8_t lastGapFlipCount;
double lastPIDgap;
uint16_t lastampCompCalibrationVal;


// --- VCO comment dump ---
/*********************** VCO calibration  ********************/
/**********************                    *******************/

// float pwm_to_vco_array[] = {
//   1,
//   50,
//   100,
//   200,
//   400,
//   600,
//   800,
//   1000,
//   1200,
//   1400,
//   1600,
//   1800,
//   2000,
//   2200,
//   2400,
//   2600,
//   2800,
//   3000,
//   3200,
//   3400,
//   3600,
//   3800,
//   4000,
//   4200,
//   4400,
//   4600,
//   4800,
//   5000,
//   5200,
//   5400,
//   5600,
//   5800,
//   6000,
//   6200,
//   6400,
//   6600,
//   6800,
//   7000,
//   7200,
//   7400,
//   7600,
//   7800,
//   8000,
//   8200,
//   8400,
//   8500,
//   8600,
//   8700,
//   8800,
//   8900,
//   9000,
//   9100,
//   9200,
//   9300,
//   9400,
//   9500,
//   9600,
//   9700,
//   9800,
//   9999,
//   9999
// };

// const uint16_t pwm_to_vco_array_size = (sizeof(pwm_to_vco_array) / sizeof(float)) - 1;

// float pwm_to_vco_euler_array[pwm_to_vco_array_size + 1];

// float freq_to_vco_array[pwm_to_vco_array_size + 1];

// uint8_t freq_to_vco_array_memcopy[(pwm_to_vco_array_size + 1) * 4];

