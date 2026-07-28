// Removed unused code from amp_comp.h

// --- from original:186-281 ---
void precomputeCoefficients_OLD() {
  for (int j = 0; j < NUM_OSCILLATORS; j++) {
    // Precompute precise float quadratic coefficients (original model)
    for (int i = 0; i < ampCompTableSize - 1; i++) {
      // Detect sentinel points and synthesize a reasonable third point near the cap
      if (ampCompFrequencyArray[j][i + 1] >= AMP_COMP_SENTINEL_FREQ_Q) {
        ampCompFrequencyArray[j][i + 1] = AMP_COMP_MAX_HZ_Q;
        ampCompArray[j][i + 1] = ampCompArray[j][i];
      }
      if (ampCompFrequencyArray[j][i + 2] >= AMP_COMP_SENTINEL_FREQ_Q) {
        int32_t f0 = ampCompFrequencyArray[j][i];
        int32_t f1 = ampCompFrequencyArray[j][i + 1];
        int32_t step = f1 - f0;
        if (step <= 0) step = (AMP_COMP_MAX_HZ_Q - f1);
        if (step <= 0) step = 1;
        int32_t synthetic = f1 + step;
        if (synthetic > AMP_COMP_MAX_HZ_Q) synthetic = AMP_COMP_MAX_HZ_Q;
        ampCompFrequencyArray[j][i + 2] = synthetic;
        ampCompArray[j][i + 2] = ampCompArray[j][i + 1];
      }
      // Detect onset of plateau (multiple consecutive DIV_COUNTER values)
      bool plateauWin = (ampCompArray[j][i + 1] >= DIV_COUNTER && ampCompArray[j][i + 2] >= DIV_COUNTER);

      double x0f = (double)ampCompFrequencyArray[j][i]     / (double)(1u << FREQ_FRAC_BITS);
      double x1f = (double)ampCompFrequencyArray[j][i + 1] / (double)(1u << FREQ_FRAC_BITS);
      double x2f = (double)ampCompFrequencyArray[j][i + 2] / (double)(1u << FREQ_FRAC_BITS);
      double y0f = (double)ampCompArray[j][i];
      double y1f = (double)ampCompArray[j][i + 1];
      double y2f = (double)ampCompArray[j][i + 2];

      if (plateauWin) {
        double xCap = (double)AMP_COMP_MAX_HZ;
        double xMid = (x0f + xCap) * 0.5;
        double yMid = (y0f + (double)DIV_COUNTER) * 0.5;
        ampCompFrequencyArray[j][i + 1] = (int32_t)llround(xMid * (double)(1u << FREQ_FRAC_BITS));
        ampCompArray[j][i + 1] = (uint16_t)llround(yMid);
        ampCompFrequencyArray[j][i + 2] = AMP_COMP_MAX_HZ_Q;
        ampCompArray[j][i + 2] = (uint16_t)DIV_COUNTER;
        x1f = xMid;
        x2f = xCap;
        y1f = yMid;
        y2f = (double)DIV_COUNTER;
      }
      // Solve for a, b, c in y = a*x^2 + b*x + c using three points
      double denom = (x0f - x1f) * (x0f - x2f) * (x1f - x2f);
      if (denom == 0.0) denom = 1.0;
      double aVal = (x2f * (y1f - y0f) + x1f * (y0f - y2f) + x0f * (y2f - y1f)) / denom;
      double bVal = (x2f * x2f * (y0f - y1f) + x1f * x1f * (y2f - y0f) + x0f * x0f * (y1f - y2f)) / denom;
      double cVal = (x1f * x2f * (x1f - x2f) * y0f + x2f * x0f * (x2f - x0f) * y1f + x0f * x1f * (x0f - x1f) * y2f) / denom;
      aCoeff[j][i] = (float)aVal;
      bCoeff[j][i] = (float)bVal;
      cCoeff[j][i] = (float)cVal;
      aCoeffD[j][i] = aVal;
      bCoeffD[j][i] = bVal;
      cCoeffD[j][i] = cVal;
      double slopeSpan = fabs((y2f - y0f) / (x2f - x0f));
      useDoubleWindow[j][i] =
        (fabs(aCoeffD[j][i]) > 1e6) ||
        (fabs(bCoeffD[j][i]) > 1e3) ||
        (slopeSpan > 0.4);
    }
    // Precompute per-window normalized quadratic coefficients (32-bit runtime)
    for (int i = 0; i < ampCompTableSize - 1; i++) {
      double x0f = (double)ampCompFrequencyArray[j][i]     / (double)(1u << FREQ_FRAC_BITS);
      double x1f = (double)ampCompFrequencyArray[j][i + 1] / (double)(1u << FREQ_FRAC_BITS);
      double x2f = (double)ampCompFrequencyArray[j][i + 2] / (double)(1u << FREQ_FRAC_BITS);
      double y0f = (double)ampCompArray[j][i];
      double y1f = (double)ampCompArray[j][i + 1];
      double y2f = (double)ampCompArray[j][i + 2];
      double dx02f = x2f - x0f;
      if (dx02f == 0.0) dx02f = 1.0;
      double t1 = (x1f - x0f) / dx02f; // in (0,1)
      double d20 = y2f - y0f;
      double d10 = y1f - y0f;
      double denom = (t1 * t1 - t1);
      if (denom == 0.0) denom = 1.0;
      double aN = (d10 - d20 * t1) / denom;
      double bN = d20 - aN;
      // Store for runtime 32-bit evaluation
      xBaseWIN[j][i] = (int32_t)ampCompFrequencyArray[j][i];
      dxWIN[j][i] = (int32_t)(ampCompFrequencyArray[j][i + 2] - ampCompFrequencyArray[j][i]);
      if (dxWIN[j][i] == 0) dxWIN[j][i] = 1;
      // Reciprocal of dx in Q28 for fast t computation without underflow
      invDxWIN_q28[j][i] = (uint32_t)llround((double)268435456.0 / (double)dxWIN[j][i]); // 2^28 / dx
      // Coefficients in Q(T_FRAC) for 32-bit eval (stored as int32_t, no saturation)
      aQWIN[j][i] = (int64_t)llround((double)aN * (double)(1LL << T_FRAC));
      bQWIN[j][i] = (int64_t)llround((double)bN * (double)(1LL << T_FRAC));
      // (removed legacy shift/scale and integer coeff precompute)
      // c in y units, clamp to valid range
      int32_t ctmp = (int32_t)lrintf(y0f);
      if (ctmp < 0) ctmp = 0;
      if (ctmp > (int32_t)DIV_COUNTER) ctmp = DIV_COUNTER;
      cQWIN[j][i] = (uint16_t)ctmp;
    }
  }
}

// --- from original:282-386 ---


// #else

// uint_fast32_t freq_to_amp_comp_array[] = {
// 0,0,
// 1635,40,
// 2183,50,
// 2914,62,
// 3889,79,
// 5191,101,
// 6930,130,
// 9250,170,
// 12347,222,
// 16481,292,
// 22000,386,
// 29366,511,
// 39200,675,
// 52325,924,
// 69846,1252,
// 93233,1688,
// 124451,2231,
// 166122,3034,
// 221746,4132,
// 295996,5632,
// 395107,7676,
// 505830,10000,
// 20000000,10000,
// };
// uint8_t ampCompArraySize = sizeof(freq_to_amp_comp_array);
// #endif

// float freq_to_amp_comp_array_float[] = {
// 18.35, 0,
// 20.60, 2,
// 27.50, 6,
// 32.70, 9,
// 43.65, 16,
// 55.00, 23,
// 65.41, 29,
// 87.31, 42,
// 110.00, 56,
// 130.81, 68,
// 174.61, 96,
// 220.00, 122,
// 261.63, 148,
// 349.23, 201,
// 440.00, 255,
// 659.25, 389,
// 880.00, 516,
// 1318.51, 768,
// 1786.00, 1024,
// 2094.00, 1198,
// 2798.00, 1670,
// 3520.00, 1944,
// 3734.00, 2052,
// 4352.00, 2342,
// 4717.00, 2499,
// 30000.00, 2500.00,
// };

// float freq_to_vco_array[] = {
// 20.60, 22,
// 21.83, 41,
// 32.70, 200,
// 43.65, 296,
// 65.41, 462,
// 87.31, 572,
// 130.81, 732,
// 174.61, 847,
// 261.63, 1019,
// 349.23, 1147,
// 523.25, 1322,
// 698.46, 1449,
// 1046.50, 1621,
// 1396.91, 1739,
// 2093.00, 1894,
// 4186.00, 2164,
// 8372.01, 2425,
// 30000.00, 2500.00,
// };

// float freq_to_vco_array[] = {    // 10000 divisions
// 0, 1,
// 16.35, 69,	// C0
// 20.60, 352,  	// E0
// 21.83, 441,  	// F0
// 27.50, 777,		// A0
// 32.70, 1037, 	// C1
// 43.65, 1464, 	// F1
// 65.41, 2080, 	// C2
// 87.31, 2543, 	// F2
// 130.81, 3180, 	// C3
// 174.61, 3651,	// F3
// 261.63, 4333,	// C4
// 349.23, 4827,	// F4
// 523.25, 5521,	// C5
// 698.46, 6035,	// F5
// 1046.50, 6715,	// C6
// 1396.91, 7189,	// F6
// 2093.00, 7826,	// C7
// 4186.00, 8910,	// C8
// 8372.01, 9999,	// C9
// 30000.00, 9999,
// };
