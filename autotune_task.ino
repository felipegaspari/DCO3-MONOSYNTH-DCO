// =============================================================================
// DCO AUTOTUNE & CALIBRATION TASK MANAGER (autotune_task.ino)
// =============================================================================
// This file centralizes all calibration routines: manual, automated, and sweeps.
// It completely replaces the legacy 'voice_task_autotune' function.

// -----------------------------------------------------------------------------
// 1. Hardware Calibration Driver
// -----------------------------------------------------------------------------
// Drives a single oscillator at a specific frequency and amplitude (Range PWM).
void autotune_drive_core(uint8_t osc, float freqHz, uint16_t ampValue) {
    PIO pioN = pio[VOICE_TO_PIO[osc]];
    uint8_t sm1N = VOICE_TO_SM[osc];
  
    uint32_t total_cycles = (freqHz > 0.0f) ? (uint32_t)fminf(((float)sysClock_Hz / freqHz) + 0.5f, 4.0e9f) : 0u;
    uint32_t clk_div = total_cycles ? pio_clk_div_for_y(total_cycles, osc_last_y[osc], osc_ramp_weight(osc), osc_period_overhead(osc)) : 0u;
  
    pio_sm_set_enabled(pioN, sm1N, true);
    pio_sm_put(pioN, sm1N, clk_div);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
  
    write_range_pwm(osc, ampValue);
  }
  
  // -----------------------------------------------------------------------------
  // 2. Manual Calibration Loop (UI / Trimpot / PW editing)
  // -----------------------------------------------------------------------------
  void autotune_manual_task() {
    if (calSyncNeutralRequested) {
      calSyncNeutralRequested = false;
      setSyncMode();
    }
  
    currentDCO = cal_manual_osc();
    const uint8_t pwCh = cal_pw_channel(currentDCO);
    
    bool is440Stage = cal_stage_is_440(manualCalibrationStage) || (manualCalibrationStep == 1);
    bool wantPulse  = cal_stage_is_square(manualCalibrationStage);
  
    // A. Calculate Target Pitch & Amplitude based on stage
    if (is440Stage) {
      VOICE_NOTES[0] = manual_cal_reference_note;
      DCO_calibration_current_note = manual_cal_reference_note;
  
      if (ampComp440[currentDCO] != 0) {
        ampCompCalibrationVal = ampComp440[currentDCO];
      } else {
        float scale = note_to_freq(manual_cal_reference_note) / note_to_freq(manual_DCO_calibration_start_note);
        ampCompCalibrationVal = (uint16_t)((initManualAmpCompCalibrationValPreset + manualCalibrationOffset[currentDCO]) * scale + 0.5f);
      }
    } else {
      VOICE_NOTES[0] = manual_DCO_calibration_start_note;
      DCO_calibration_current_note = manual_DCO_calibration_start_note;
      ampCompCalibrationVal = initManualAmpCompCalibrationValPreset + manualCalibrationOffset[currentDCO];
    }
  
    float freqHz = note_to_freq(DCO_calibration_current_note);
  
    // B. Hardware Isolation: Mute inactive oscillators, Drive active oscillator
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
      if (i != currentDCO) {
        PIO pioN = pio[VOICE_TO_PIO[i]];
        uint8_t sm1N = VOICE_TO_SM[i];
        pio_sm_set_enabled(pioN, sm1N, false);
        pio_sm_put(pioN, sm1N, 0);
        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
        write_range_pwm(i, 0); // Mute Range
      } else {
        autotune_drive_core(i, freqHz, ampCompCalibrationVal);
      }
    }
  
    // C. Configure Pulse Width Baseline & Waveform Muting
    if (!cal_stage_is_pw_edit(manualCalibrationStage)) {
      if (wantPulse && osc_has_pw(currentDCO)) {
        // Pulse Stage: Set active channel to perfect 50% baseline (0V for DCO3, Center for DCO4)
        apply_pw_baseline_solo(pwCh);
      } else {
        // Saw/Tri Stage: Mute ALL Pulse channels to completely isolate the analog waveform
        for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ch++) {
          if (PW_PINS[ch] != PW_PIN_UNASSIGNED) {
            pwm_set_chan_level(PW_PWM_SLICES[ch], pwm_gpio_to_channel(PW_PINS[ch]), 0);
            PW[ch] = 0;
          }
        }
      }
    }
  
    // D. Update Peripherals (CV outs & Live Debug Telemetry)
    update_CV_outs_manual_calibration();
    DCO_calibration_debug();
  
    // E. PW CV Limit Probing (Triggers during specific UI actions)
    if (pwCvProbeRequested) {
      pwCvProbeRequested = false;
      run_pw_cv_probe();
    }
  }
  
  // -----------------------------------------------------------------------------
  // 3. Main Entry Point: Called repeatedly from loop1()
  // -----------------------------------------------------------------------------
  void autotune_loop_task() {
    if (calibrationFlag) {
      if (manualCalibrationFlag) {
        // Continuous live task for UI interactions
        autotune_manual_task();
      } else {
        // Blocking automated routine (Full, Amp, or PW auto-sweep)
        DCO_calibration();
      }
    } else if (calibrationVerifyRequested) {
      calibrationVerifyRequested = false;
      // Blocking verification sweep
      run_calibration_verify_sweep();
    }
  }