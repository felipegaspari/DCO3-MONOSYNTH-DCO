// Removed unused code from state_machines.ino

// --- from original:53-57 ---

void init_sm(PIO pio, uint sm, uint offset, uint pin) {
  init_sm_pin(pio, sm, offset, pin);
  pio_sm_set_enabled(pio, sm, true);
}

// --- from original:63-71 ---

void set_frequency(PIO pio, uint sm, float freq) {
  uint32_t clk_div = sysClock_Hz / freq;
  if (freq == 0)
    clk_div = 0;
  pio_sm_put(pio, sm, clk_div);
  pio_sm_exec(pio, sm, pio_encode_pull(false, false));
  pio_sm_exec(pio, sm, pio_encode_out(pio_osr, 31));
}
