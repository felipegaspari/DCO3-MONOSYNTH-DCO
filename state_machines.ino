void init_pio() {

  offset[0] = pio_add_program(pio[0], &frequency_sync_4_jumps_program);
  offset[1] = pio_add_program(pio[1], &frequency_sync_4_jumps_program);
  offset[2] = pio_add_program(pio[2], &frequency_sync_4_jumps_program);
  // offset[0] = pio_add_program(pio[0], &frequency_program);
  // offset[1] = pio_add_program(pio[1], &frequency_program);
  // offset[2] = pio_add_program(pio[2], &frequency_program);
  start_voice_sms();
}

void start_voice_sms() {

  for (int i = 0; i < NUM_OSCILLATORS; i++) {

    uint8_t sidesetPin;
    switch (syncMode) {
      case 0:
        // Free-running: self sideset (same policy as setSyncMode)
        sidesetPin = RESET_PINS[i];
        break;
      case 1:
        // OSC2 syncs from OSC1; OSC3 free-running
        if (i == 1) {
          sidesetPin = RESET_PINS[0];
        } else {
          sidesetPin = RESET_PINS[i];
        }
        break;
      case 2:
        // OSC1 syncs from OSC2; OSC3 free-running
        if (i == 0) {
          sidesetPin = RESET_PINS[1];
        } else {
          sidesetPin = RESET_PINS[i];
        }
        break;
      default:
        sidesetPin = RESET_PINS[i];
        break;
    }

    // Freq only on SM0; amplitude uses RANGE PWM (not PIO).
    init_sm_sync(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], offset[VOICE_TO_PIO[i]], RESET_PINS[i], sidesetPin);

    pio_sm_put(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], pioPulseLength);

    pio_sm_exec(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], pio_encode_pull(false, false));

    pio_sm_exec(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], pio_encode_out(pio_y, 31));
  }
}

void init_sm(PIO pio, uint sm, uint offset, uint pin) {
  init_sm_pin(pio, sm, offset, pin);
  pio_sm_set_enabled(pio, sm, true);
}

void init_sm_sync(PIO pio, uint sm, uint offset, uint pin, uint pin2) {
  frequency_sync_4_jumps(pio, sm, offset, pin, pin2);
  pio_sm_set_enabled(pio, sm, true);
}

void set_frequency(PIO pio, uint sm, float freq) {
  uint32_t clk_div = sysClock_Hz / freq;
  if (freq == 0)
    clk_div = 0;
  pio_sm_put(pio, sm, clk_div);
  pio_sm_exec(pio, sm, pio_encode_pull(false, false));
  pio_sm_exec(pio, sm, pio_encode_out(pio_osr, 31));
}
