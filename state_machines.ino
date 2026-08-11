// Which oscillator has its reset driven by another, and which one drives it.
// syncMode 1: OSC2's sideset drives OSC1's reset pin, so OSC1 is the slave.
// syncMode 2: OSC1's sideset drives OSC2's reset pin, so OSC2 is the slave.
// OSC3 is always free-running.
static int sync_slave_osc() {
  if (syncMode == 1) return 0;
  if (syncMode == 2) return 1;
  return -1;
}

static int sync_master_osc() {
  if (syncMode == 1) return 1;
  if (syncMode == 2) return 0;
  return -1;
}

// Give the slave a lower state machine index than its master. When two SMs write the
// same pin on the same cycle the higher-numbered one wins, so a master that outranks
// its slave never loses a sync edge to a tie.
void assign_sm_mapping() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    VOICE_TO_SM[i] = i;
  }

  int slave = sync_slave_osc();
  int master = sync_master_osc();
  if (slave >= 0 && master >= 0 && slave > master) {
    VOICE_TO_SM[slave] = master;
    VOICE_TO_SM[master] = slave;
  }
}

// Pick the soft-sync poll program image for N trailing polled chunks (1..3).
static const pio_program_t *soft_sync_program_for_chunks(uint8_t chunks) {
  if (chunks >= 3) return &frequency_sync_poll_3_program;
  if (chunks == 2) return &frequency_sync_poll_2_program;
  return &frequency_sync_poll_program;
}

// Keep free-running + exactly one poll image on pio0. Swapping among N=1/2/3 removes the
// old poll program and loads the new one (12 + 13..15 <= 27 of 32 slots). Hard sync
// (softSyncChunks == 0) leaves whatever poll image is already resident.
static void ensure_soft_sync_program(uint8_t chunks) {
  if (chunks < 1) chunks = 1;
  if (chunks > 3) chunks = 3;
  if (chunks == pio_loaded_sync_chunks) return;

  if (pio_loaded_sync_chunks != 0) {
    pio_remove_program(pio[0], soft_sync_program_for_chunks(pio_loaded_sync_chunks),
                       pio_offset_sync);
  }
  pio_offset_sync = pio_add_program(pio[0], soft_sync_program_for_chunks(chunks));
  pio_loaded_sync_chunks = chunks;
}

// Load the oscillator programs into pio0 and start all voice SMs. Called from setup1().
void init_pio() {
  // Claim SMs through the SDK so other PIOProgram users cannot steal them.
  // Oscillators always occupy pio0 SM0–2; pio1 SM0 = sub-osc, SM1 = noise LFSR.
  for (int sm = 0; sm < NUM_OSCILLATORS; sm++) {
    pio_sm_claim(pio[0], sm);
  }
#ifndef ENABLE_SUBOSC_ENGINE2
  pio_sm_claim(pio[SUBOSC_PIO], SUBOSC_SM);
#endif
  pio_sm_claim(pio[NOISE_PIO], NOISE_SM);

  // Free-running program plus one soft-sync poll image (default N=1). Switching hard↔soft
  // with the same N is a re-init; changing N among 1/2/3 reloads the poll image.
  pio_offset_free = pio_add_program(pio[0], &frequency_sync_4_jumps_program);
  pio_loaded_sync_chunks = 0;
  ensure_soft_sync_program(softSyncChunks > 0 ? softSyncChunks : 1);

  // pio1: noise LFSR must load first at origin 0 (out pc,1 XOR); then sub-osc.
  // With ENABLE_SUBOSC_ENGINE2 the sub moves to pio2 and the div2/div4 images are not
  // loaded at all, leaving pio1 SM0 and 12 instruction slots free (that is where
  // ENABLE_PIO_MIDI belongs now). Otherwise pio2 stays free for it.
  noise_lfsr_offset = pio_add_program(pio[NOISE_PIO], &noise_lfsr_program);
#ifndef ENABLE_SUBOSC_ENGINE2
  subosc_offset_div2 = pio_add_program(pio[SUBOSC_PIO], &subosc_div2_program);
  subosc_offset_div4 = pio_add_program(pio[SUBOSC_PIO], &subosc_div4_program);
#endif
  if (dcoNoiseUsesPioWhite()) {
    const int out_pin =
#ifdef ENABLE_NOISE_OUT
        (int)NOISE_OUT_PIN;
#else
        -1;
#endif
    noise_lfsr_init(pio[NOISE_PIO], NOISE_SM, noise_lfsr_offset, 0xC0FFEE01u,
                    out_pin);
  }

  assign_sm_mapping();
  start_voice_sms();
#ifdef ENABLE_SUBOSC_ENGINE2
  subosc2_init();
#endif
  set_subosc_divide(subOscDivide);
}

// Match RESET pad polarity to the analog discharge switch. When ENABLE_PIO_RESET_INVERT
// is set, logical PIO "1 = assert" becomes pad low (DG411 on) while INOVER keeps jmp_pin
// / wait readers on the logical sense. Cleared explicitly when the flag is off so a
// rebuild without the define does not leave stale overrides after a soft reset.
static void pio_reset_pin_apply_polarity(uint pin) {
#ifdef ENABLE_PIO_RESET_INVERT
  gpio_set_outover(pin, GPIO_OVERRIDE_INVERT);
  gpio_set_inover(pin, GPIO_OVERRIDE_INVERT);
#else
  gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
  gpio_set_inover(pin, GPIO_OVERRIDE_NORMAL);
#endif
}

// Configure every oscillator state machine on pio0 and start them on the same cycle.
// Safe to call again whenever the sync topology changes.
void start_voice_sms() {
  int slave = sync_slave_osc();
  int master = sync_master_osc();
  bool softSync = (softSyncChunks > 0) && (slave >= 0) && (master >= 0);
  uint8_t chunks = soft_sync_chunks_clamped();
  if (chunks < 1) chunks = 1;

  // Stop every voice SM before a possible poll-program reload (instruction memory must
  // not change under a running SM).
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    pio_sm_set_enabled(pio[0], VOICE_TO_SM[i], false);
  }
  if (softSync) {
    ensure_soft_sync_program(chunks);
  }

  uint32_t enableMask = 0;

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    uint8_t sm = VOICE_TO_SM[i];

    // Hard sync: the master's sideset drives the slave's reset pin as well as its own,
    // discharging the slave's integrator on the master's cycle while the slave's SM
    // keeps its own schedule. This only works because every SM is on pio0 — a GPIO's
    // function select names one block, so oscillators on separate blocks could not
    // share a pin.
    //
    // Soft sync drives nothing extra: the slave polls the master's pin instead, so the
    // master leaves its sideset on its own reset pin.
    uint8_t sidesetPin = RESET_PINS[i];
    if (!softSync && master >= 0 && i == master) {
      sidesetPin = RESET_PINS[slave];
    }

    pio_sm_clear_fifos(pio[0], sm);

    if (softSync && i == slave) {
      frequency_sync_poll_init(pio[0], sm, pio_offset_sync, RESET_PINS[i], sidesetPin,
                               RESET_PINS[master], chunks);
      osc_uses_sync_program[i] = true;
    } else {
      frequency_sync_4_jumps(pio[0], sm, pio_offset_free, RESET_PINS[i], sidesetPin);
      osc_uses_sync_program[i] = false;
    }

    // Preload the reset pulse width into Y, then restore the divider that Y's write
    // consumed out of the OSR.
    osc_load_period_stopped(i, pioPulseLength, osc_last_clk_div[i]);

    enableMask |= (1u << sm);
  }

  // After pio_gpio_init in the SM inits: apply (or clear) pad polarity on every RESET.
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    pio_reset_pin_apply_polarity(RESET_PINS[i]);
  }

  // Same-cycle start. Separate pio_sm_set_enabled calls used to leave a few hundred
  // nanoseconds of skew between oscillators, which capped phase-align accuracy.
  pio_enable_sm_mask_in_sync(pio[0], enableMask);
}

// Reload every oscillator's reset pulse width (Y), preserving the running period.
//
// Period was Y_old + weight*clk_div + overhead. With a new Y the divider must be
// recomputed before re-enable; keeping the old clk_div stretches/shrinks pitch until
// the next voice_task frame. All SMs are stopped, loaded, then started in the same
// cycle so sync pairs do not tear.
void osc_reload_reset_pulse_all(uint32_t y) {
  uint32_t enableMask = 0;

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    const uint8_t sm = VOICE_TO_SM[i];
    pio_sm_set_enabled(pio[0], sm, false);
    enableMask |= (1u << sm);
  }

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    const uint32_t weight = osc_ramp_weight(i);
    const uint32_t overhead = osc_period_overhead(i);
    const uint32_t total =
        osc_last_y[i] + weight * osc_last_clk_div[i] + overhead;
    const uint32_t clk_div = pio_clk_div_for_y(total, y, weight, overhead);
    osc_load_period_stopped(i, y, clk_div);
  }

  pio_enable_sm_mask_in_sync(pio[0], enableMask);
}

// Report the sync topology and, importantly, which PIO block owns each reset pin.
//
// This is the check that catches the bug this rework fixes: a GPIO's function select can
// name only one block, so when the oscillators lived on pio0/pio1/pio2 the master's
// pio_gpio_init() silently re-pointed the slave's reset pin at the master's block and the
// slave stopped driving its own core. Every RESET pin must read back as PIO0 here.
void pio_topology_report() {
  int slave = sync_slave_osc();
  int master = sync_master_osc();

  bench_out_reset();
  bench_out_printf("[pio topology] syncMode=%u softSyncChunks=%u slave=%d master=%d\n",
                   syncMode, softSyncChunks, slave, master);

  bool allOnPio0 = true;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    gpio_function_t fn = gpio_get_function(RESET_PINS[i]);
    bool onPio0 = (fn == GPIO_FUNC_PIO0);
    if (!onPio0) allOnPio0 = false;

    const char *prog = "free";
    if (osc_uses_sync_program[i]) {
      switch (soft_sync_chunks_clamped()) {
        case 2:  prog = "poll2"; break;
        case 3:  prog = "poll3"; break;
        default: prog = "poll1"; break;
      }
    }
    bench_out_printf("  OSC%d reset=GP%-2u sm=%u program=%s funcsel=%d%s\n",
                     i + 1, RESET_PINS[i], VOICE_TO_SM[i], prog,
                     (int)fn, onPio0 ? "" : "  <-- NOT PIO0");
  }

  bench_out_printf("  reset pin ownership: %s\n",
                   allOnPio0 ? "OK (all PIO0)" : "BROKEN (a pin was stolen by another block)");

  if (master >= 0 && slave >= 0) {
    bench_out_printf("  master OSC%d is sm=%u, slave OSC%d is sm=%u -> tie-break %s\n",
                     master + 1, VOICE_TO_SM[master], slave + 1, VOICE_TO_SM[slave],
                     VOICE_TO_SM[master] > VOICE_TO_SM[slave] ? "OK (master outranks slave)"
                                                              : "WRONG (master can lose edges)");
  }
  bench_out_active = true;
}

// Park one oscillator at clk_div (core 1 only — called from pio_defer_service).
static void pio_period_probe_run(uint8_t osc, uint32_t clk_div) {
  uint8_t sm = VOICE_TO_SM[osc];

  pio_sm_set_enabled(pio[0], sm, false);
  osc_load_period_stopped(osc, pioPulseLength, clk_div);
  pio_sm_set_enabled(pio[0], sm, true);
}

void pio_period_probe(uint8_t osc, uint32_t clk_div) {
  pio_defer_request_period_probe(osc, clk_div);
}

// Solve period = Y + weight*clk_div + overhead from two frequency-counter readings taken
// with pio_period_probe() at the same Y. Prints the measured weight and overhead so they
// can be compared against PIO_RAMP_WEIGHT_* / PIO_PERIOD_OVERHEAD_*.
void pio_solve_period_model(uint32_t clk_div_a, double measured_hz_a,
                            uint32_t clk_div_b, double measured_hz_b,
                            uint32_t y) {
  if (clk_div_a == clk_div_b || measured_hz_a <= 0.0 || measured_hz_b <= 0.0) {
    Serial.println("[period solve] need two distinct clk_div values and non-zero readings");
    return;
  }

  double period_a = (double)sysClock_Hz / measured_hz_a;
  double period_b = (double)sysClock_Hz / measured_hz_b;

  double weight = (period_a - period_b) / ((double)clk_div_a - (double)clk_div_b);
  double overhead = period_a - (double)y - weight * (double)clk_div_a;

  Serial.printf("[period solve] weight = %.4f (expected %u/%u/%u/%u for chunks 0..3)\n",
                weight,
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[0],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[1],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[2],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[3]);
  Serial.printf("[period solve] overhead = %.2f cycles (expected %u/%u/%u/%u)\n",
                overhead,
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[0],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[1],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[2],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[3]);
}

// (Re)configure the sub-oscillator. divide 0 stops it, 2 and 4 give a square one and
// two octaves below OSC1. Needs SUBOSC_PIN wired to a mixer input to be audible.
//
// With ENABLE_SUBOSC_ENGINE2 there is one sub per oscillator and this legacy entry point
// (PARAM_SUBOSC_DIVIDE, which predates the per-oscillator parameters) sets the same ratio on
// all three. Subs whose pin is still unwired stay stopped either way.
void set_subosc_divide(uint8_t divide) {
#ifdef ENABLE_SUBOSC_ENGINE2
  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; osc++) {
    subosc2_set_divide(osc, divide);
  }
  return;
#else
  subOscDivide = divide;

  PIO p = pio[SUBOSC_PIO];
  pio_sm_set_enabled(p, SUBOSC_SM, false);

  if (divide == 0) {
    return;
  }

  bool div4 = (divide >= 4);
  subosc_init(p, SUBOSC_SM, div4 ? subosc_offset_div4 : subosc_offset_div2,
              RESET_PINS[0], SUBOSC_PIN, div4);
  pio_sm_set_enabled(p, SUBOSC_SM, true);
#endif  // ENABLE_SUBOSC_ENGINE2
}

// ---- Core-0 → core-1 deferred PIO requests -----------------------------------
// Serial/MIDI handlers on core 0 must not touch PIO while voice_task_main on core 1
// is driving the same state machines.

static volatile uint8_t pio_defer_pending = 0;
static volatile uint8_t pio_defer_subosc_value = 0;
static volatile uint8_t pio_defer_probe_osc = 0;
static volatile uint32_t pio_defer_probe_clk_div = 0;

volatile bool pio_probe_report_pending = false;
static volatile uint8_t pio_probe_report_osc = 0;
static volatile uint8_t pio_probe_report_sm = 0;
static volatile uint32_t pio_probe_report_clk_div = 0;
static volatile uint32_t pio_probe_report_weight = 0;
static volatile uint32_t pio_probe_report_overhead = 0;
static volatile uint32_t pio_probe_report_predicted = 0;

static constexpr uint8_t PIO_DEFER_SYNC   = 1u << 0;
static constexpr uint8_t PIO_DEFER_RESET  = 1u << 1;
static constexpr uint8_t PIO_DEFER_SUBOSC = 1u << 2;
static constexpr uint8_t PIO_DEFER_PROBE  = 1u << 3;
#ifdef ENABLE_SUBOSC_ENGINE2
// Per-oscillator sub divides need one pending value each, plus a bit saying which arrived:
// three sliders can move between two services and none of them should be lost.
static constexpr uint8_t PIO_DEFER_SUBOSC2 = 1u << 4;
static volatile uint8_t pio_defer_subosc2_divide[NUM_OSCILLATORS] = { 0, 0, 0 };
static volatile uint8_t pio_defer_subosc2_mask = 0;
// The combiner's operator and input pair are applied together, so both pending values live
// here: core 0 can then set one without reading back the other from a global core 1 owns.
// Both start where subOscLogicOp / subOscLogicPair do.
static constexpr uint8_t PIO_DEFER_SUBOSC_LOGIC = 1u << 5;
static volatile uint8_t pio_defer_subosc_logic_op = 0;
static volatile uint8_t pio_defer_subosc_logic_pair = SUBOSC_LOGIC_PAIR_DEFAULT;
// The per-voice master combine rewrites six words of the segment program's instruction
// memory, which is core 1's to touch.
static constexpr uint8_t PIO_DEFER_SUBOSC_MASTER = 1u << 6;
static volatile uint8_t pio_defer_subosc_master_op = 0;
#endif

void setSyncMode();  // voices.ino — must run on core 1 only

void pio_defer_request_sync_mode() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SYNC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_reset_pulse_all() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_RESET, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc(uint8_t divide) {
  pio_defer_subosc_value = divide;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC, __ATOMIC_SEQ_CST);
}

#ifdef ENABLE_SUBOSC_ENGINE2
void pio_defer_request_subosc_divide(uint8_t osc, uint8_t divide) {
  if (osc >= NUM_OSCILLATORS) {
    return;
  }
  pio_defer_subosc2_divide[osc] = divide;
  __dmb();
  __atomic_fetch_or(&pio_defer_subosc2_mask, (uint8_t)(1u << osc), __ATOMIC_SEQ_CST);
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC2, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc_logic_op(uint8_t op) {
  pio_defer_subosc_logic_op = op;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC_LOGIC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc_logic_pair(uint8_t pair) {
  pio_defer_subosc_logic_pair = pair;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC_LOGIC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc_master_op(uint8_t op) {
  pio_defer_subosc_master_op = op;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC_MASTER, __ATOMIC_SEQ_CST);
}
#endif

void pio_defer_request_period_probe(uint8_t osc, uint32_t clk_div) {
  pio_defer_probe_osc = osc;
  pio_defer_probe_clk_div = clk_div;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_PROBE, __ATOMIC_SEQ_CST);
}

void pio_probe_report_flush() {
  if (!pio_probe_report_pending) {
    return;
  }
  pio_probe_report_pending = false;

  const uint8_t osc = pio_probe_report_osc;
  bench_out_reset();
  bench_out_printf("[period probe] osc=%u sm=%u pin=%u\n",
                   (unsigned)osc, (unsigned)pio_probe_report_sm,
                   (unsigned)RESET_PINS[osc]);
  bench_out_printf("  Y=%lu clk_div=%lu weight=%lu overhead=%lu\n",
                   (unsigned long)pioPulseLength,
                   (unsigned long)pio_probe_report_clk_div,
                   (unsigned long)pio_probe_report_weight,
                   (unsigned long)pio_probe_report_overhead);
  bench_out_printf("  predicted period = %lu cycles = %.4f Hz\n",
                   (unsigned long)pio_probe_report_predicted,
                   (double)sysClock_Hz / (double)pio_probe_report_predicted);
  bench_out_active = true;
}

void pio_defer_service() {
  const uint8_t pending =
      __atomic_exchange_n(&pio_defer_pending, 0, __ATOMIC_SEQ_CST);
  if (pending == 0) {
    return;
  }

  if (pending & PIO_DEFER_SYNC) {
    setSyncMode();
  }
  if (pending & PIO_DEFER_RESET) {
    // Y only; do not force note_on_flag — that retriggered every slider step (~50 Hz)
    // and was the main source of mid-drag pitch collapse / sync tear.
    osc_reload_reset_pulse_all(pioPulseLength);
  }
  if (pending & PIO_DEFER_SUBOSC) {
    set_subosc_divide(pio_defer_subosc_value);
  }
#ifdef ENABLE_SUBOSC_ENGINE2
  if (pending & PIO_DEFER_SUBOSC2) {
    const uint8_t mask = __atomic_exchange_n(&pio_defer_subosc2_mask, 0, __ATOMIC_SEQ_CST);
    for (uint8_t osc = 0; osc < NUM_OSCILLATORS; osc++) {
      if (mask & (1u << osc)) {
        subosc2_set_divide(osc, pio_defer_subosc2_divide[osc]);
      }
    }
  }
  if (pending & PIO_DEFER_SUBOSC_LOGIC) {
    subosc2_set_logic(pio_defer_subosc_logic_op, pio_defer_subosc_logic_pair);
  }
  if (pending & PIO_DEFER_SUBOSC_MASTER) {
    subosc2_set_master_op(pio_defer_subosc_master_op);
  }
#endif
  if (pending & PIO_DEFER_PROBE) {
    const uint8_t osc = pio_defer_probe_osc;
    const uint32_t clk_div = pio_defer_probe_clk_div;
    pio_period_probe_run(osc, clk_div);
    pio_probe_report_osc = osc;
    pio_probe_report_sm = VOICE_TO_SM[osc];
    pio_probe_report_clk_div = clk_div;
    pio_probe_report_weight = osc_ramp_weight(osc);
    pio_probe_report_overhead = osc_period_overhead(osc);
    pio_probe_report_predicted =
        pioPulseLength + pio_probe_report_weight * clk_div + pio_probe_report_overhead;
    pio_probe_report_pending = true;
  }
}
