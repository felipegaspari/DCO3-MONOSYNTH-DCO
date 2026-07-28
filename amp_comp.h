#ifndef __AMP_COMP_H__
#define __AMP_COMP_H__

#include "include_all.h"
#include <math.h>
#include <limits.h>

static constexpr int ampCompTableSize = 22;
// Windows with steep slopes (upper band) evaluated in double precision
static constexpr int AMP_COMP_DOUBLE_WINDOW_START = ampCompTableSize - 10;
// Frequency values for amplitude compensation are stored as fixed-point Hz (Q(FREQ_FRAC_BITS))
static constexpr int FREQ_FRAC_BITS = 8;
static constexpr int32_t AMP_COMP_SENTINEL_FREQ_Q = 50000000; // sentinel marker from FS data (Q8)
// Maximum frequency (Hz) for which we apply amplitude compensation.
// At or above this frequency, get_chan_level() returns full scale (DIV_COUNTER).
static constexpr int32_t AMP_COMP_MAX_HZ = 7000;
static constexpr int32_t AMP_COMP_MAX_HZ_Q = (int32_t)(AMP_COMP_MAX_HZ << FREQ_FRAC_BITS);

static constexpr int AMP_COMP_FAST_T_FRAC = 13;
static constexpr int AMP_COMP_FAST_RECIP_EXTRA = 12;
static constexpr int AMP_COMP_FAST_COEFF_FRAC = 5;
static constexpr int AMP_COMP_FAST_SHIFT = AMP_COMP_FAST_COEFF_FRAC + AMP_COMP_FAST_T_FRAC;
static constexpr int AMP_COMP_FAST_SLOPE_FRAC = 12;

int32_t freq_to_amp_comp_array[chanLevelVoiceDataSize * NUM_OSCILLATORS];
uint8_t ampCompArraySize = FSVoiceDataSize / 4;

int32_t ampCompFrequencyArray[NUM_OSCILLATORS][ampCompTableSize + 1];
int32_t ampCompArray[NUM_OSCILLATORS][ampCompTableSize + 1];

// High-precision float coefficients (original model): y = a*x^2 + b*x + c
float aCoeff[NUM_OSCILLATORS][ampCompTableSize - 1];
float bCoeff[NUM_OSCILLATORS][ampCompTableSize - 1];
float cCoeff[NUM_OSCILLATORS][ampCompTableSize - 1];
double aCoeffD[NUM_OSCILLATORS][ampCompTableSize - 1];
double bCoeffD[NUM_OSCILLATORS][ampCompTableSize - 1];
double cCoeffD[NUM_OSCILLATORS][ampCompTableSize - 1];
bool useDoubleWindow[NUM_OSCILLATORS][ampCompTableSize - 1];
bool plateauWindow[NUM_OSCILLATORS][ampCompTableSize - 1];

// Per-window normalized quadratic in t = (x - x0) / (x2 - x0), where x,x0,x2 are integer Hz.
// Runtime uses 32-bit fixed-point t (Q(T_FRAC)) and precomputed integer coefficients.
static constexpr int T_FRAC = 12;
int32_t xBaseWIN[NUM_OSCILLATORS][ampCompTableSize - 1];
int32_t dxWIN[NUM_OSCILLATORS][ampCompTableSize - 1];
// Use Q28 reciprocal to avoid underflow on very large dx while keeping shifts small
uint32_t invDxWIN_q28[NUM_OSCILLATORS][ampCompTableSize - 1];
int64_t aQWIN[NUM_OSCILLATORS][ampCompTableSize - 1]; // Q(T_FRAC) wide
int64_t bQWIN[NUM_OSCILLATORS][ampCompTableSize - 1]; // Q(T_FRAC) wide
uint16_t cQWIN[NUM_OSCILLATORS][ampCompTableSize - 1];
int32_t aQWIN_fast[NUM_OSCILLATORS][ampCompTableSize - 1];
int32_t bQWIN_fast[NUM_OSCILLATORS][ampCompTableSize - 1];
// (removed legacy 32-bit-only fast-path parameters and integer coeffs)


/**
 * @brief Pre-calculates all necessary data for the final, non-hybrid amplitude compensation function.
 *
 * This function's sole purpose is to prepare the data for `get_chan_level_final`.
 * It is called once at startup. For every 3-point window in the compensation table, it:
 * 1.  Loads the raw data into local variables for sanitization.
 * 2.  Optionally cleans the local data by handling sentinels and smoothing plateaus
 *     (new path).
 * 3.  Computes the complete, consistent data package (base, width, reciprocal, and
 *     normalized coefficients) needed for the fast fixed-point quadratic calculation.
 */
static void precomputeCoefficients() {
  static_assert(T_FRAC > 0 && T_FRAC < 28, "T_FRAC must be in a valid range for the math to work.");

  // --- Data Sanitization ---
  // Append a final point to each table to guarantee it reaches the defined maximum.
  // This makes the system robust to incomplete calibration data from the filesystem.
  for (int j = 0; j < NUM_OSCILLATORS; ++j) {
      ampCompFrequencyArray[j][ampCompTableSize] = AMP_COMP_MAX_HZ_Q;
      ampCompArray[j][ampCompTableSize] = DIV_COUNTER;
  }

  const double freqScale    = (double)(1u << FREQ_FRAC_BITS);
  const double invFreqScale = 1.0 / freqScale;
  const double maxFreqHz    = (double)AMP_COMP_MAX_HZ;
  const int32_t maxFreqQ    = AMP_COMP_MAX_HZ_Q;
  const uint32_t fastRecipNumerator = (uint32_t)1 << (AMP_COMP_FAST_T_FRAC + AMP_COMP_FAST_RECIP_EXTRA);
  const int fastCoeffShift = AMP_COMP_FAST_COEFF_FRAC + AMP_COMP_FAST_T_FRAC;

  for (int j = 0; j < NUM_OSCILLATORS; j++) {
    for (int i = 0; i < ampCompTableSize - 1; ++i) {
      double x0_f = (double)ampCompFrequencyArray[j][i]     * invFreqScale;
      double x1_f = (double)ampCompFrequencyArray[j][i + 1] * invFreqScale;
      double x2_f = (double)ampCompFrequencyArray[j][i + 2] * invFreqScale;
      double y0_f = (double)ampCompArray[j][i];
      double y1_f = (double)ampCompArray[j][i + 1];
      double y2_f = (double)ampCompArray[j][i + 2];

      if (ampCompFrequencyArray[j][i + 1] >= AMP_COMP_SENTINEL_FREQ_Q) x1_f = maxFreqHz;
      if (ampCompFrequencyArray[j][i + 2] >= AMP_COMP_SENTINEL_FREQ_Q) x2_f = maxFreqHz;

      if (y1_f >= DIV_COUNTER && y2_f >= DIV_COUNTER) {
        double plateau_end_y = (double)DIV_COUNTER;
        x1_f = (x0_f + maxFreqHz) * 0.5;
        y1_f = (y0_f + plateau_end_y) * 0.5;
        x2_f = maxFreqHz;
        y2_f = plateau_end_y;

        ampCompFrequencyArray[j][i + 1] = (int32_t)llround(x1_f * freqScale);
        ampCompArray[j][i + 1] = (uint16_t)llround(y1_f);
        ampCompFrequencyArray[j][i + 2] = AMP_COMP_MAX_HZ_Q;
        ampCompArray[j][i + 2] = (uint16_t)DIV_COUNTER;
      }

      plateauWindow[j][i] = (ampCompArray[j][i + 1] >= (int32_t)DIV_COUNTER &&
                              ampCompArray[j][i + 2] >= (int32_t)DIV_COUNTER);

      // --- 2. Calculate Float Coefficients (for the fallback) ---
      long double denom_ld = (long double)(x0_f - x1_f) * (long double)(x0_f - x2_f) * (long double)(x1_f - x2_f);
      if (denom_ld == 0.0L) denom_ld = 1.0L;
      long double inv_denom_ld = 1.0L / denom_ld;

      long double aVal_ld = ((long double)x2_f * (long double)(y1_f - y0_f) +
                             (long double)x1_f * (long double)(y0_f - y2_f) +
                             (long double)x0_f * (long double)(y2_f - y1_f)) * inv_denom_ld;
      long double bVal_ld = ((long double)x2_f * (long double)x2_f * (long double)(y0_f - y1_f) +
                             (long double)x1_f * (long double)x1_f * (long double)(y2_f - y0_f) +
                             (long double)x0_f * (long double)x0_f * (long double)(y1_f - y2_f)) * inv_denom_ld;
      long double cVal_ld = ((long double)x1_f * (long double)x2_f * (long double)(x1_f - x2_f) * (long double)y0_f +
                             (long double)x2_f * (long double)x0_f * (long double)(x2_f - x0_f) * (long double)y1_f +
                             (long double)x0_f * (long double)x1_f * (long double)(x0_f - x1_f) * (long double)y2_f) * inv_denom_ld;

      aCoeff[j][i] = (float)aVal_ld;
      bCoeff[j][i] = (float)bVal_ld;
      cCoeff[j][i] = (float)cVal_ld;
      aCoeffD[j][i] = (double)aVal_ld;
      bCoeffD[j][i] = (double)bVal_ld;
      cCoeffD[j][i] = (double)cVal_ld;

      long double dx02_ld = (long double)x2_f - (long double)x0_f;
      if (dx02_ld <= 0.0L) dx02_ld = 1.0L;
      long double inv_dx02_ld = 1.0L / dx02_ld;
      long double t1_ld = ((long double)x1_f - (long double)x0_f) * inv_dx02_ld;
      long double d20_ld = (long double)y2_f - (long double)y0_f;
      long double d10_ld = (long double)y1_f - (long double)y0_f;

      long double denom_norm_ld = (t1_ld * t1_ld - t1_ld);
      if (denom_norm_ld == 0.0L) denom_norm_ld = 1.0L;
      long double inv_denom_norm_ld = 1.0L / denom_norm_ld;

      long double aN_ld = (d10_ld - d20_ld * t1_ld) * inv_denom_norm_ld;
      long double bN_ld = d20_ld - aN_ld;

      xBaseWIN[j][i] = ampCompFrequencyArray[j][i];
      dxWIN[j][i]    = ampCompFrequencyArray[j][i + 2] - ampCompFrequencyArray[j][i];
      if (dxWIN[j][i] <= 0) dxWIN[j][i] = 1;

      // Exact integer rounding for Q28 reciprocal
      {
        uint32_t dxu = (uint32_t)dxWIN[j][i];
        uint64_t num = (uint64_t)1ULL << 28;
        invDxWIN_q28[j][i] = (uint32_t)((num + (dxu >> 1)) / dxu);
      }

      aQWIN[j][i] = (int64_t)llroundl(aN_ld * (long double)(1LL << T_FRAC));
      bQWIN[j][i] = (int64_t)llroundl(bN_ld * (long double)(1LL << T_FRAC));

      int32_t c_temp = (int32_t)lrint(y0_f);
      if (c_temp < 0) c_temp = 0;
      if (c_temp > (int32_t)DIV_COUNTER) c_temp = (int32_t)DIV_COUNTER;
      cQWIN[j][i] = (uint16_t)c_temp;

      int64_t aFastLL = llroundl(aN_ld * (long double)(1 << T_FRAC));
      if (aFastLL > (int64_t)INT32_MAX) aFastLL = (int64_t)INT32_MAX;
      if (aFastLL < (int64_t)INT32_MIN) aFastLL = (int64_t)INT32_MIN;
      aQWIN_fast[j][i] = (int32_t)aFastLL;

      // Derive b so that a + b = (y2 - y0) in fast scaling => exact match at t=1
      int64_t d20_int = ((int64_t)ampCompArray[j][i + 2] - (int64_t)ampCompArray[j][i]) << T_FRAC;
      int64_t bFastLL = d20_int - aFastLL;
      if (bFastLL > (int64_t)INT32_MAX) bFastLL = (int64_t)INT32_MAX;
      if (bFastLL < (int64_t)INT32_MIN) bFastLL = (int64_t)INT32_MIN;
      bQWIN_fast[j][i] = (int32_t)bFastLL;

      useDoubleWindow[j][i] = false;
    }
  }
}

// Function to precompute the coefficients

#endif