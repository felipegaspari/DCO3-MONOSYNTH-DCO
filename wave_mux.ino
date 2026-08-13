// Dual 74HC595 → 3× DG411 per-osc Saw/Pulse/Tri enables.
// Active-low: bit 0 = wave on (DG411 IN low closes switch). See docs/WAVE_MUX.md.

#ifdef ENABLE_WAVE_MUX

static uint16_t waveMuxBits = 0xFFFF;  // all off (1 = disabled)

// Provisional bit map: OSC1 Saw/Pulse/Tri = 0..2, OSC2 = 3..5, OSC3 = 6..8.
// Bits 9–15 unused (left high). Remap when PCB is frozen.
static const uint8_t WAVE_MUX_BIT[3][3] = {
  { 0, 1, 2 },  // OSC1 Saw, Pulse, Tri
  { 3, 4, 5 },  // OSC2
  { 6, 7, 8 },  // OSC3
};

static inline void waveMuxWritePin(uint8_t pin, bool high) {
  if (pin > 15) return;
  if (high) {
    waveMuxBits |= (uint16_t)(1u << pin);
  } else {
    waveMuxBits &= (uint16_t)~(1u << pin);
  }
}

static void waveMuxShiftOut() {
  // Shift MSB first into daisy-chain (chip2 then chip1).
  for (int i = 15; i >= 0; --i) {
    gpio_put(HC595_DATA_PIN, (waveMuxBits >> i) & 1u);
    gpio_put(HC595_CLK_PIN, 1);
    busy_wait_us_32(1);
    gpio_put(HC595_CLK_PIN, 0);
  }
  gpio_put(HC595_LATCH_PIN, 1);
  busy_wait_us_32(1);
  gpio_put(HC595_LATCH_PIN, 0);
}

void init_waveSelector() {
  pinMode(HC595_LATCH_PIN, OUTPUT);
  pinMode(HC595_DATA_PIN, OUTPUT);
  pinMode(HC595_CLK_PIN, OUTPUT);
  digitalWrite(HC595_LATCH_PIN, LOW);
  digitalWrite(HC595_CLK_PIN, LOW);
  digitalWrite(HC595_DATA_PIN, LOW);
  waveMuxBits = 0xFFFF;
  waveMuxShiftOut();
}

void update_waveSelector() {
  waveMuxBits = 0xFFFF;  // unused bits stay high (off)
  for (uint8_t osc = 0; osc < 3; osc++) {
    for (uint8_t wave = 0; wave < 3; wave++) {
      // Active-low: enabled → write 0
      waveMuxWritePin(WAVE_MUX_BIT[osc][wave], waveEnable[osc][wave] ? 0 : 1);
    }
  }
  waveMuxShiftOut();
}

void waveSelector_manual_calibration(byte stage) {
  waveMuxBits = 0xFFFF;
  uint8_t osc = cal_stage_to_osc(stage);
  if (osc > 2) osc = 2;
  // Saw only on sub 0; pulse and 440 Hz substages play the square (wave 1).
  // Never TRI.
  uint8_t wave = 1;
  if (cal_stage_is_saw(stage)) wave = 0;
  else if (cal_stage_is_tri(stage)) wave = 2;
  waveMuxWritePin(WAVE_MUX_BIT[osc][wave], 0);
  waveMuxShiftOut();
}

#else  // !ENABLE_WAVE_MUX

void init_waveSelector() {}
void update_waveSelector() {}
void waveSelector_manual_calibration(byte) {}

#endif
