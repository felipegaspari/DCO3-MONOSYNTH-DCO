#ifdef ENABLE_SUBOSC_ENGINE2
#include "hardware/dma.h"

// Two-sub engine on pio2, one SM each, either sub free to follow any oscillator's reset. See
// docs/PIO_OSCILLATORS.md section 9 for the program listing and the reasoning behind the
// segment split.
//
// Each sub period is two segments, low then high, and a segment is "Cn whole master
// flybacks, then Fn fine system-clock cycles". The three words per period are pushed in the
// order the program pulls them:
//
//   [0] Cl | Ch<<16  flybacks per segment
//   [1] Fl           fine cycles before the rising edge
//   [2] Fh           fine cycles before the falling edge
//
// Cl + Ch always equals the divide ratio, so the frequency is locked to the master no matter
// what phase and width are doing. That invariant is the whole point of the design:
// modulation can only reshape the sub, never detune it.
//
// The DMA reads these words one at a time as the SM drains the FIFO, so a control frame can
// land in the middle of a group. Both counts therefore share one word - a torn pair would
// not sum to the divide ratio and the sub would drop or gain a master period, and no single
// word can be torn. A mixed pair of fine values only moves an edge a few cycles.

static constexpr uint32_t SUBOSC_SEG_WORDS = 3;

// Cycles between a segment's reference point and the edge it produces: the loop
// fall-through, the pull, the mov, the fine loop's own fall-through and the next pull. The
// fine value is the target offset minus this, so a fine of 0 still means "as soon as
// possible" rather than "one cycle early".
static constexpr uint32_t SUBOSC_FINE_LEAD = 5;

// Slack left at the end of a fine delay. The hazard is only ever the *release* at the end of
// the master period: a fine delay that outlives it costs the next segment one flyback, which
// would genuinely detune the sub. Ending inside the reset pulse instead is harmless — the
// following `wait 1 pin 0` sees the pin already asserted and completes immediately, so the
// flyback still gets counted. The guard therefore only has to cover the handful of
// instructions between the delay and that wait.
static constexpr uint32_t SUBOSC_FINE_GUARD = 32;

// Matrix ±1023 (MOD_PITCH_DEPTH_FULL, the matrix-wide full-scale convention) to parameter
// units, as Q16 multipliers: full depth is one whole master period of phase, or the whole
// duty range. Phase wraps, duty clamps - a duty that wrapped would jump from a hairline
// pulse to a hairline gap mid-sweep.
static constexpr int32_t SUBOSC_MOD_PHASE_MUL = (int32_t)((360.0 * 65536.0) / 1023.0);
static constexpr int32_t SUBOSC_MOD_PW_MUL = (int32_t)((128.0 * 65536.0) / 1023.0);

static inline int32_t subosc_mod_clamped(int32_t v) {
  if (v > MOD_PITCH_DEPTH_FULL) return MOD_PITCH_DEPTH_FULL;
  if (v < -MOD_PITCH_DEPTH_FULL) return -MOD_PITCH_DEPTH_FULL;
  return v;
}

static uint32_t subosc_seg_words[SUBOSC_COUNT][SUBOSC_SEG_WORDS];
static uint32_t subosc_dma_src_addr[SUBOSC_COUNT];
static int subosc_dma_data[SUBOSC_COUNT] = { -1, -1 };
static int subosc_dma_ctrl[SUBOSC_COUNT] = { -1, -1 };
static bool subosc_running[SUBOSC_COUNT] = { false, false };
static bool subosc_pio_ready = false;

// The boolean logic combiner dispatches through `out pc, 2`, which is an absolute jump, so
// its four-entry truth table has to sit at pio2 addresses 0..3. Park subosc_seg above that
// window now so the combiner can still claim origin 0 later.
static constexpr uint SUBOSC_SEG_LOAD_OFFSET = 8;

static inline bool subosc_pin_wired(uint8_t sub) {
  return SUBOSC_PINS[sub] != SUBOSC_PIN_UNASSIGNED;
}

// (q16 * cycles) >> 16 without losing the top of the product: a 12 Hz period is already
// ~19 M cycles, so the intermediate needs 64 bits.
static inline uint32_t subosc_mul_q16(uint32_t q16, uint32_t cycles) {
  return (uint32_t)(((uint64_t)q16 * (uint64_t)cycles) >> 16);
}

// Turn (divide, master period, phase, width) into the three segment words.
//
// Positions are Q16 in *master period* units, so the integer part is a flyback count and the
// fraction is an offset inside that master period. Width is a fraction of the sub period, and
// multiplying it by the divide ratio is what keeps this division-free: it does the work a
// divide by the master period would otherwise have to do.
//
// Phase is degrees of the *master* period, not of the sub period, and that is deliberate:
// shifting the sub by a whole master period is unobservable. The master waveform repeats every
// master period, so moving the sub edge from one flyback to the next leaves the sub-against-saw
// relationship — the thing the ear hears — identical. Only the position inside a master period
// is real, so that is what the parameter controls, and every degree of it counts.
static void subosc_compute_words(uint8_t sub, uint32_t master_cycles) {
  const uint32_t divide = subOscDivides[sub];
  if (divide == 0 || master_cycles == 0) {
    return;
  }

  // Panel value plus the matrix offset, and the offset lands on sub 2 alone. Moving both subs
  // by the same amount leaves their relative phase and their duties matched, which every
  // combiner operator maps back to exactly the waveform it produced before - the modulation
  // would be inaudible on the output that matters. One-sided, the same sweep walks sub 2
  // through sub 1 and the combined shape changes on every degree.
  const bool modulated = (sub == 1);
  int32_t deg = (int32_t)subOscPhaseDeg[sub];
  int32_t width = (int32_t)subOscWidth[sub];
  if (modulated) {
    deg += (subosc_mod_clamped(subosc_mod_phase) * SUBOSC_MOD_PHASE_MUL) >> 16;
    width += (subosc_mod_clamped(subosc_mod_pw) * SUBOSC_MOD_PW_MUL) >> 16;
  }
  // Both stay inside one turn / the 1..255 duty range, so the wrap is two comparisons rather
  // than a modulo.
  if (deg < 0) deg += 360;
  else if (deg >= 360) deg -= 360;
  if (width < 1) width = 1;
  else if (width > 255) width = 255;

  // deg/360 in Q16, by the same reciprocal osc_phase_hold_x() uses. Q24 * deg stays inside
  // 32 bits for deg < 360, so this is one multiply and a shift.
  const uint32_t phase_q16 = ((uint32_t)deg * RECIP_360_Q24) >> 8;
  const uint32_t duty_q16 = (uint32_t)width << 8;

  // Rise, fall and the following rise, unwrapped so that rise < fall < next rise.
  const uint32_t rise = phase_q16;
  const uint32_t fall = rise + duty_q16 * divide;
  const uint32_t rise_next = rise + (divide << 16);

  // Flybacks per segment, summing to the divide ratio by construction. The phase offset is
  // less than one master period, so the rise never crosses a flyback and the fall's flyback
  // index is the high segment's count outright.
  const uint32_t high_count = fall >> 16;
  const uint32_t low_count = divide - high_count;

  // With at least one flyback in the segment the fine value is an absolute offset from that
  // flyback, which is what makes phase mean "delay the edge N cycles after the master
  // resets". With no flyback there is nothing to measure from, so the fine value is the
  // segment's own length, taken from the edge that opened it.
  //
  // The ceiling differs between those two cases, and getting it wrong costs a flyback. An
  // absolute delay starts at a release and has a whole master period ahead of it. A relative
  // delay starts wherever its opening edge fell, so it only has the rest of that master
  // period - which is what bites when an edge lands almost exactly on a flyback and rounding
  // leaves it on the near side. Clamping shortens the segment by a few cycles; the flyback
  // counts, and so the frequency, are untouched.
  uint32_t high_fine, low_fine;
  uint32_t high_max, low_max;
  if (high_count > 0) {
    high_fine = subosc_mul_q16(fall & 0xFFFFu, master_cycles);
    high_max = master_cycles;
  } else {
    high_fine = subosc_mul_q16(fall - rise, master_cycles);
    high_max = subosc_mul_q16(65536u - rise, master_cycles);
  }
  if (low_count > 0) {
    low_fine = subosc_mul_q16(rise & 0xFFFFu, master_cycles);
    low_max = master_cycles;
  } else {
    low_fine = subosc_mul_q16(rise_next - fall, master_cycles);
    low_max = subosc_mul_q16(65536u - (fall & 0xFFFFu), master_cycles);
  }

  high_max = (high_max > 2u * SUBOSC_FINE_GUARD) ? (high_max - SUBOSC_FINE_GUARD) : 1u;
  low_max = (low_max > 2u * SUBOSC_FINE_GUARD) ? (low_max - SUBOSC_FINE_GUARD) : 1u;
  if (high_fine > high_max) high_fine = high_max;
  if (low_fine > low_max) low_fine = low_max;

  subosc_seg_words[sub][0] = low_count | (high_count << 16);
  subosc_seg_words[sub][1] = (low_fine > SUBOSC_FINE_LEAD) ? (low_fine - SUBOSC_FINE_LEAD) : 0u;
  subosc_seg_words[sub][2] = (high_fine > SUBOSC_FINE_LEAD) ? (high_fine - SUBOSC_FINE_LEAD) : 0u;
  __dmb();  // publish the group before the DMA can read it
}

// Safe contents for the window between enabling an SM and the first control frame: one
// minimum-length pulse per sub period. Seeding zeros instead would run the output at tens of
// MHz until the first update lands.
static void subosc_seed_words(uint8_t sub) {
  const uint32_t divide = (subOscDivides[sub] > 0) ? subOscDivides[sub] : 1u;
  subosc_seg_words[sub][0] = divide;  // all flybacks low, none high
  subosc_seg_words[sub][1] = 0;
  subosc_seg_words[sub][2] = 0;
  __dmb();
}

// Break the chain before aborting: while ctrl still points at data, aborting data only
// gets it retriggered. chain_to == self is how the hardware spells "no chaining".
static void subosc_dma_stop(uint8_t sub) {
  const int data = subosc_dma_data[sub];
  const int ctrl = subosc_dma_ctrl[sub];
  if (data < 0 || ctrl < 0) {
    return;
  }
  hw_write_masked(&dma_hw->ch[data].al1_ctrl,
                  ((uint32_t)data << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
                  DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
  dma_channel_abort(ctrl);
  dma_channel_abort(data);
}

// Endless 3-word stream into the SM's TX FIFO, using the same data + control pair as the
// RANGE dither DMA in PWM.ino: data pushes the group, chains to ctrl, ctrl rewrites data's
// read address through al3_read_addr_trig and restarts it. (A read ring would be one channel
// instead of two but needs a power-of-two group, and its transfer count would still run out.)
static void subosc_dma_start(uint8_t sub) {
  PIO p = pio[SUBOSC2_PIO];
  const uint sm = sub;
  const int data = subosc_dma_data[sub];
  const int ctrl = subosc_dma_ctrl[sub];
  if (data < 0 || ctrl < 0) {
    return;
  }

  subosc_dma_src_addr[sub] = (uint32_t)(uintptr_t)subosc_seg_words[sub];

  dma_channel_config data_c = dma_channel_get_default_config(data);
  channel_config_set_transfer_data_size(&data_c, DMA_SIZE_32);
  channel_config_set_read_increment(&data_c, true);
  channel_config_set_write_increment(&data_c, false);
  channel_config_set_dreq(&data_c, pio_get_dreq(p, sm, true));
  channel_config_set_chain_to(&data_c, (uint)ctrl);
  dma_channel_configure(data, &data_c, &p->txf[sm], subosc_seg_words[sub],
                        SUBOSC_SEG_WORDS, false);

  dma_channel_config ctrl_c = dma_channel_get_default_config(ctrl);
  channel_config_set_transfer_data_size(&ctrl_c, DMA_SIZE_32);
  channel_config_set_read_increment(&ctrl_c, false);
  channel_config_set_write_increment(&ctrl_c, false);
  dma_channel_configure(ctrl, &ctrl_c, &dma_hw->ch[data].al3_read_addr_trig,
                        &subosc_dma_src_addr[sub], 1, false);

  dma_channel_start(ctrl);
}

// Stop a sub and park its pad low. OUTOVER works regardless of function select, so the pin
// stays quiet while pio2 still owns it.
static void subosc_stop_one(uint8_t sub) {
  subosc_dma_stop(sub);
  pio_sm_set_enabled(pio[SUBOSC2_PIO], sub, false);
  if (subosc_pin_wired(sub)) {
    gpio_set_outover(SUBOSC_PINS[sub], GPIO_OVERRIDE_LOW);
  }
  subosc_running[sub] = false;
}

// (Re)start a sub with the FIFO and the DMA in step. The SM consumes exactly three words per
// period and the DMA delivers exactly three per group, so they only need aligning once, at
// start: pio_sm_init clears the FIFO and parks the PC at instruction 0 (expecting the count
// word), then the freshly configured DMA begins the next group from word 0.
static void subosc_start_one(uint8_t sub) {
  if (!subosc_pio_ready || !subosc_pin_wired(sub) || subOscDivides[sub] == 0) {
    return;
  }

  PIO p = pio[SUBOSC2_PIO];
  const uint sm = sub;
  const uint pin = SUBOSC_PINS[sub];
  // The master lives in the SM's IN base, which is why changing it is a restart rather than a
  // register write.
  const uint master_pin = RESET_PINS[subOscMaster[sub]];

  subosc_dma_stop(sub);
  subosc_seed_words(sub);
  // Also clears the FIFO, restarts the SM and jumps to the program's first instruction.
  subosc_seg_init(p, sm, subosc_seg_offset, master_pin, pin);
  gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
  subosc_dma_start(sub);
  pio_sm_set_enabled(p, sm, true);
  subosc_running[sub] = true;
}

// ---- Boolean logic combiner (pio2 SM3) -------------------------------------
// Digital ring modulation: two sub squares in, one combined square out. The operator is the
// four-entry jump table at instruction 0..3, so changing it is four writes into instruction
// memory - no reconfiguration, and no need to stop the SM. Each write is a single word, so
// the worst a change can do is let one 5-cycle sample use the old table.

// Output high for each (sub2<<1)|sub1 input combination, bit i = table entry i. Sub 1 is the
// low bit because the IN base is SUBOSC_PINS[0], the lower of the two adjacent pads.
//
// The last two are pass-throughs - the output simply follows one sub - and they exist because
// they cost exactly what every other operator costs, four table entries. With them the combiner
// pad carries everything the engine can make, so the carrier needs one mixer input for the sub
// section rather than three.
//   off  XOR  AND   OR  XNOR NAND  NOR  sub1 sub2
static const uint8_t SUBOSC_LOGIC_MASKS[SUBOSC_LOGIC_OP_MAX + 1] = {
  0x0, 0x6, 0x8, 0xE, 0x9, 0x7, 0x1, 0xA, 0xC
};
static const char* const SUBOSC_LOGIC_NAMES[SUBOSC_LOGIC_OP_MAX + 1] = {
  "off", "XOR", "AND", "OR", "XNOR", "NAND", "NOR", "sub1", "sub2"
};

static bool subosc_logic_running = false;

// Whether the combiner can run at all. With a fixed pair this is pure wiring: both subs need
// pads, those pads have to be adjacent (one IN base, two bits), and the result needs a pad of
// its own. Every one of these holds for the shipped GP8/GP9/GP10 assignment, so the check is
// really a guard for a future board's pin map - it reports the reason in subosc2_report()
// rather than driving a wrong pin or reading a wrong one.
static bool subosc_logic_wiring_ok(const char** why) {
  const uint8_t pin_a = SUBOSC_PINS[0];
  const uint8_t pin_b = SUBOSC_PINS[1];
  if (pin_a == SUBOSC_PIN_UNASSIGNED || pin_b == SUBOSC_PIN_UNASSIGNED) {
    *why = "a sub has no pad";
    return false;
  }
  // Sub 2 has to be the *upper* pad, not merely adjacent: the IN base is sub 1's pad, so the
  // order is what decides which bit of the table index each sub is. Swapping them would leave
  // the six symmetric operators intact and quietly exchange the two pass-throughs.
  if (pin_b != pin_a + 1u) {
    *why = "sub 2's pad is not sub 1's pad + 1";
    return false;
  }
  if (SUBOSC_LOGIC_PIN == SUBOSC_PIN_UNASSIGNED) {
    *why = "output has no pad";
    return false;
  }
  if (SUBOSC_LOGIC_PIN == pin_a || SUBOSC_LOGIC_PIN == pin_b) {
    *why = "output pad collides with a sub";
    return false;
  }
  *why = "ok";
  return true;
}

static void subosc_logic_write_table(uint8_t op) {
  const uint8_t mask = SUBOSC_LOGIC_MASKS[op];
  PIO p = pio[SUBOSC2_PIO];
  for (uint i = 0; i < SUBOSC_LOGIC_TABLE_LEN; i++) {
    p->instr_mem[i] = subosc_logic_entry_instr((mask >> i) & 1u);
  }
}

static void subosc_logic_stop() {
  pio_sm_set_enabled(pio[SUBOSC2_PIO], SUBOSC2_LOGIC_SM, false);
  if (SUBOSC_LOGIC_PIN != SUBOSC_PIN_UNASSIGNED) {
    gpio_set_outover(SUBOSC_LOGIC_PIN, GPIO_OVERRIDE_LOW);
  }
  subosc_logic_running = false;
}

void subosc2_set_logic(uint8_t op) {
  if (op > SUBOSC_LOGIC_OP_MAX) op = SUBOSC_LOGIC_OP_MAX;
  subOscLogicOp = op;

  if (!subosc_pio_ready || op == 0) {
    if (subosc_logic_running) subosc_logic_stop();
    return;
  }

  const char* why = "";
  if (!subosc_logic_wiring_ok(&why)) {
    if (subosc_logic_running) subosc_logic_stop();
    return;
  }

  // Once running, every later change is the table alone: the IN base is fixed, so the SM keeps
  // running and the output never drops out, not even for a cycle.
  subosc_logic_write_table(op);
  if (subosc_logic_running) {
    return;
  }

  subosc_logic_init(pio[SUBOSC2_PIO], SUBOSC2_LOGIC_SM, SUBOSC_PINS[0], SUBOSC_LOGIC_PIN);
  gpio_set_outover(SUBOSC_LOGIC_PIN, GPIO_OVERRIDE_NORMAL);
  pio_sm_set_enabled(pio[SUBOSC2_PIO], SUBOSC2_LOGIC_SM, true);
  subosc_logic_running = true;
}

// Load the segment program and reserve the block. Called from init_pio() on core 1.
void subosc2_init() {
  if (subosc_pio_ready) {
    return;
  }

  PIO p = pio[SUBOSC2_PIO];

  // SM0/SM1 are the subs (SM index == sub index) and SM3 is held for the boolean logic combiner
  // so no other PIOProgram user can take it. SM2 is deliberately left unclaimed - that is the
  // spare a third sub used to occupy.
  for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
    pio_sm_claim(p, sub);
  }
  pio_sm_claim(p, SUBOSC2_LOGIC_SM);

  pio_add_program_at_offset(p, &subosc_seg_program, SUBOSC_SEG_LOAD_OFFSET);
  subosc_seg_offset = SUBOSC_SEG_LOAD_OFFSET;

  // Loads at 0..7 by its own .origin, which is why the segment program starts at 8. Resident
  // even when the combiner is off: the eight words are already paid for, and the operator can
  // then come up as a table rewrite rather than a program load.
  pio_add_program(p, &subosc_logic_program);

  for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
    subosc_seed_words(sub);
    if (!subosc_pin_wired(sub)) {
      continue;
    }
    // Two per wired sub, so four here on top of the six the RANGE dither takes in PWM.ino:
    // 10 of the RP2350's 16 channels. The combiner needs none - it reads pins, not a FIFO.
    subosc_dma_data[sub] = dma_claim_unused_channel(true);
    subosc_dma_ctrl[sub] = dma_claim_unused_channel(true);
  }

  subosc_pio_ready = true;
  subosc2_set_logic(subOscLogicOp);
}

void subosc2_set_divide(uint8_t sub, uint8_t divide) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (divide > SUBOSC_DIVIDE_MAX) {
    divide = SUBOSC_DIVIDE_MAX;
  }

  const bool was_on = (subOscDivides[sub] != 0);
  subOscDivides[sub] = divide;
  if (sub == 0) {
    subOscDivide = divide;  // PARAM_SUBOSC_DIVIDE has always meant the first sub
  }

  // The ratio is per-period data, so only crossing off/on needs the SM and DMA touched.
  if (divide == 0) {
    if (was_on) subosc_stop_one(sub);
    return;
  }
  if (!was_on || !subosc_running[sub]) {
    subosc_start_one(sub);
  }
}

// Point a sub at another oscillator's reset. The master is the SM's IN base, so this is the
// same restart an off→on divide does; a running sub loses at most one period.
void subosc2_set_master(uint8_t sub, uint8_t osc) {
  if (sub >= SUBOSC_COUNT || osc >= NUM_OSCILLATORS || subOscMaster[sub] == osc) {
    return;
  }
  subOscMaster[sub] = osc;
  if (subosc_running[sub]) {
    subosc_start_one(sub);
  }
}

void subosc2_set_phase_deg(uint8_t sub, uint16_t deg) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  subOscPhaseDeg[sub] = (deg >= 360u) ? (uint16_t)(deg % 360u) : deg;
}

void subosc2_set_width(uint8_t sub, uint8_t width) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (width == 0) {
    width = 1;  // a zero-width pulse would be silence, and 1/256 is already a thin spike
  }
  subOscWidth[sub] = width;
}

// One control frame's worth of work: recompute each running sub's three words from the period
// its master was just given. The frame hands over its three (oscillator, period) pairs because
// DCO_A/B/C are not always 0/1/2; they are transposed into oscillator order here so a sub can
// look up whichever master it follows.
void subosc2_update_periods(uint8_t osc_a, uint32_t total_a,
                            uint8_t osc_b, uint32_t total_b,
                            uint8_t osc_c, uint32_t total_c) {
  if (!subosc_pio_ready) {
    return;
  }
  // Pulled here rather than from update_CV_outs because this is the rate the subs consume it
  // at, and it costs nothing when no slot targets them.
  mod_matrix_eval_subosc(LFO1Level, LFO2Level);

  uint32_t period_by_osc[NUM_OSCILLATORS] = { 0, 0, 0 };
  if (osc_a < NUM_OSCILLATORS) period_by_osc[osc_a] = total_a;
  if (osc_b < NUM_OSCILLATORS) period_by_osc[osc_b] = total_b;
  if (osc_c < NUM_OSCILLATORS) period_by_osc[osc_c] = total_c;

  for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
    if (subosc_running[sub]) {
      // A zero period means that oscillator was not in this frame; compute_words ignores it and
      // the sub keeps its previous words rather than being handed a division by nothing.
      subosc_compute_words(sub, period_by_osc[subOscMaster[sub]]);
    }
  }
}

// ---- Parameter entry points (core 0) ---------------------------------------
// Divide and master move state machines and DMA channels, so they go through the deferred queue
// that core 1 drains before voice_task. Phase and width are plain mailboxes: core 1 folds them
// into the segment words on its next frame, and a value that arrives mid-frame is simply used by
// the frame after.

void subosc_param_divide(uint8_t sub, int16_t v) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (v < 0) v = 0;
  if (v > SUBOSC_DIVIDE_MAX) v = SUBOSC_DIVIDE_MAX;
  pio_defer_request_subosc_divide(sub, (uint8_t)v);
}

void subosc_param_master(uint8_t sub, int16_t v) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (v < 0) v = 0;
  if (v >= NUM_OSCILLATORS) v = NUM_OSCILLATORS - 1;
  pio_defer_request_subosc_master(sub, (uint8_t)v);
}

void subosc_param_phase(uint8_t sub, int16_t v) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (v < 0) v = 0;
  subosc2_set_phase_deg(sub, (uint16_t)v);
}

void subosc_param_width(uint8_t sub, int16_t v) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  if (v < 1) v = 1;
  if (v > 255) v = 255;
  subosc2_set_width(sub, (uint8_t)v);
}

// Rewrites the combiner's instruction memory, and starts its state machine the first time, so
// it goes through the deferred queue like divide does rather than touching pio2 from core 0.
void subosc_param_logic_op(int16_t v) {
  if (v < 0) v = 0;
  if (v > SUBOSC_LOGIC_OP_MAX) v = SUBOSC_LOGIC_OP_MAX;
  pio_defer_request_subosc_logic_op((uint8_t)v);
}

// PARAM_DEBUG_COMMAND 4, so core 0, and through the same paced buffer pio_topology_report()
// uses rather than Serial directly - a blocking write here would be a write while core 1 is
// mid-frame. Reading pio2 and DMA registers from core 0 is fine; nothing here writes.
void subosc2_report() {
  bench_out_reset();
  bench_out_printf("[subosc] engine2 on pio%u, seg at %u, logic at 0, %s\n",
                   (unsigned)SUBOSC2_PIO, (unsigned)subosc_seg_offset,
                   subosc_pio_ready ? "ready" : "NOT READY");
  for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
    if (!subosc_pin_wired(sub)) {
      bench_out_printf("  sub%u: pin unwired\n", (unsigned)(sub + 1));
      continue;
    }
    bench_out_printf("  sub%u: sm=%u pin=%u master=osc%u div=%u phase=%u deg width=%u/256 %s\n",
                     (unsigned)(sub + 1), (unsigned)sub, (unsigned)SUBOSC_PINS[sub],
                     (unsigned)(subOscMaster[sub] + 1), (unsigned)subOscDivides[sub],
                     (unsigned)subOscPhaseDeg[sub], (unsigned)subOscWidth[sub],
                     subosc_running[sub] ? "running" : "stopped");
    bench_out_printf("        Cl=%lu Ch=%lu Fl=%lu Fh=%lu  dma=%d/%d txlevel=%u\n",
                     (unsigned long)(subosc_seg_words[sub][0] & 0xFFFFu),
                     (unsigned long)(subosc_seg_words[sub][0] >> 16),
                     (unsigned long)subosc_seg_words[sub][1],
                     (unsigned long)subosc_seg_words[sub][2], subosc_dma_data[sub],
                     subosc_dma_ctrl[sub],
                     (unsigned)pio_sm_get_tx_fifo_level(pio[SUBOSC2_PIO], sub));
  }

  // Checked again while printing so the reason a stopped combiner is stopped is current,
  // rather than whatever it was when the operator was last set.
  const char* why = "";
  subosc_logic_wiring_ok(&why);
  bench_out_printf("  logic: sm=%u %s in=%u/%u out=%d %s (%s)\n",
                   (unsigned)SUBOSC2_LOGIC_SM, SUBOSC_LOGIC_NAMES[subOscLogicOp],
                   (unsigned)SUBOSC_PINS[0], (unsigned)SUBOSC_PINS[1],
                   (SUBOSC_LOGIC_PIN == SUBOSC_PIN_UNASSIGNED) ? -1 : (int)SUBOSC_LOGIC_PIN,
                   subosc_logic_running ? "running" : "stopped", why);
  bench_out_printf("  mod (sub2 only): phase %+ld pw %+ld (matrix units, full scale %d)\n",
                   (long)subosc_mod_phase, (long)subosc_mod_pw, MOD_PITCH_DEPTH_FULL);
  bench_out_active = (bench_out_len > 0u);
}

#else  // !ENABLE_SUBOSC_ENGINE2

// RP2040 / classic-sub builds: there is one sub, fixed at 50% and locked to OSC1's reset, so
// only sub 1's divide means anything. Master, phase and width have nowhere to go. Keeping these
// stubs is what lets params.ino carry the parameters unconditionally.

void subosc_param_divide(uint8_t sub, int16_t v) {
  if (sub != 0) {
    return;
  }
  uint8_t divide = 0;
  if (v >= 4) {
    divide = 4;
  } else if (v >= 2) {
    divide = 2;
  }
  pio_defer_request_subosc(divide);
}

void subosc_param_master(uint8_t sub, int16_t v) {
  (void)sub;
  (void)v;
}

void subosc_param_phase(uint8_t sub, int16_t v) {
  (void)sub;
  (void)v;
}

void subosc_param_width(uint8_t sub, int16_t v) {
  (void)sub;
  (void)v;
}

// No second sub to combine with, so there is nothing for the combiner to do here.
void subosc_param_logic_op(int16_t v) {
  (void)v;
}

// PARAM_DEBUG_COMMAND 4 still answers, so the panel button is never silent.
void subosc2_report() {
  bench_out_reset();
  bench_out_printf("[subosc] engine2 not compiled in; legacy sub on pio%u sm%u pin=%u div=%u\n",
                   (unsigned)SUBOSC_PIO, (unsigned)SUBOSC_SM, (unsigned)SUBOSC_PIN,
                   (unsigned)subOscDivide);
  bench_out_active = (bench_out_len > 0u);
}

#endif  // ENABLE_SUBOSC_ENGINE2
