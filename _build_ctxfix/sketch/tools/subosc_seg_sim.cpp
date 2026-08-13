#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/tools/subosc_seg_sim.cpp"
// Host-side check of the subosc_seg segment math (ENABLE_SUBOSC_ENGINE2).
//
// Mirrors subosc_compute_words() from subosc.ino, then runs a cycle-accurate simulation of
// the 18-instruction PIO program against a synthetic master oscillator to measure what the
// sub actually does, over every divide, every degree of phase and every width at four
// pitches. What it is looking for:
//
//   * frequency lock:  every sub period is exactly divide * master period, for every
//                      phase / width / divide / pitch combination
//   * phase:           the rising edge lands where the phase parameter asked
//   * width:           the high time matches the duty parameter
//
// The point is the first one. Phase and width are allowed to be a few cycles off (fixed
// point rounding, the 5-cycle instruction lead, the fine clamp near the master's reset
// pulse); a period that is off by even one cycle would be a detuned sub.
//
//   g++ -O2 -o /tmp/subosc_seg_sim tools/subosc_seg_sim.cpp && /tmp/subosc_seg_sim

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static const uint32_t SUBOSC_FINE_LEAD = 5;
static const uint32_t SUBOSC_FINE_GUARD = 32;

struct Words {
  uint32_t low_count, low_fine, high_count, high_fine;
};

static inline uint32_t mul_q16(uint32_t q16, uint32_t cycles) {
  return (uint32_t)(((uint64_t)q16 * (uint64_t)cycles) >> 16);
}

// Verbatim port of subosc_compute_words().
static Words compute(uint32_t divide, uint32_t master_cycles, uint16_t deg, uint8_t width,
                     uint32_t pulse_len) {
  if (deg >= 360u) deg = (uint16_t)(deg % 360u);
  const uint32_t RECIP_360_Q24 = (uint32_t)(((1ULL << 24) + 180) / 360);
  const uint32_t phase_q16 = ((uint32_t)deg * RECIP_360_Q24) >> 8;
  const uint32_t duty_q16 = (uint32_t)width << 8;

  const uint32_t rise = phase_q16;
  const uint32_t fall = rise + duty_q16 * divide;
  const uint32_t rise_next = rise + (divide << 16);

  const uint32_t high_count = fall >> 16;
  const uint32_t low_count = divide - high_count;

  (void)pulse_len;
  uint32_t high_fine, low_fine, high_max, low_max;
  if (high_count > 0) {
    high_fine = mul_q16(fall & 0xFFFFu, master_cycles);
    high_max = master_cycles;
  } else {
    high_fine = mul_q16(fall - rise, master_cycles);
    high_max = mul_q16(65536u - rise, master_cycles);
  }
  if (low_count > 0) {
    low_fine = mul_q16(rise & 0xFFFFu, master_cycles);
    low_max = master_cycles;
  } else {
    low_fine = mul_q16(rise_next - fall, master_cycles);
    low_max = mul_q16(65536u - (fall & 0xFFFFu), master_cycles);
  }
  high_max = (high_max > 2u * SUBOSC_FINE_GUARD) ? (high_max - SUBOSC_FINE_GUARD) : 1u;
  low_max = (low_max > 2u * SUBOSC_FINE_GUARD) ? (low_max - SUBOSC_FINE_GUARD) : 1u;
  if (high_fine > high_max) high_fine = high_max;
  if (low_fine > low_max) low_fine = low_max;

  Words w;
  w.low_count = low_count;
  w.low_fine = (low_fine > SUBOSC_FINE_LEAD) ? low_fine - SUBOSC_FINE_LEAD : 0u;
  w.high_count = high_count;
  w.high_fine = (high_fine > SUBOSC_FINE_LEAD) ? high_fine - SUBOSC_FINE_LEAD : 0u;
  return w;
}

// Master oscillator as the sub sees it: reset asserted for the first pulse_len cycles of
// every period (PIO logical sense; ENABLE_PIO_RESET_INVERT only flips the pad).
struct Master {
  uint64_t period, pulse;
  bool high(uint64_t t) const { return (t % period) < pulse; }
  uint64_t next_high(uint64_t t) const {
    if (high(t)) return t;
    return (t / period + 1) * period;
  }
  uint64_t next_low(uint64_t t) const {
    if (!high(t)) return t;
    return (t / period) * period + pulse;
  }
};

struct Edges {
  std::vector<uint64_t> rises, falls;
};

// Cycle-accurate run of the 18 instructions. One cycle per instruction; a wait completes on
// the cycle its condition is true, so the next instruction starts the cycle after.
static Edges run(const Master& m, const Words& w, int periods) {
  Edges e;
  uint64_t t = 0;
  // Start mid-ramp, like an SM enabled while the master is already running.
  t = m.pulse + 10;

  for (int i = 0; i < periods; i++) {
    t++;                   // 0: pull Cl | Ch<<16
    e.falls.push_back(t);  // 1: out y, 16 side 0  -> Y = Cl, sub goes low
    t++;
    t++;  // 2: out x, 16  -> X = Ch
    uint32_t y = w.low_count;
    for (;;) {
      t++;  // jmp !y
      if (y == 0) break;
      t = m.next_high(t) + 1;  // wait 1 pin 0
      t = m.next_low(t) + 1;   // wait 0 pin 0
      y--;
      t++;  // jmp y--
    }
    t++;                  // pull Fl
    t++;                  // mov y, osr
    t += w.low_fine + 1;  // fine loop: y+1 cycles

    e.rises.push_back(t);  // 10: mov y, x side 1 -> Y = Ch, sub goes high
    t++;
    y = w.high_count;
    for (;;) {
      t++;  // jmp !y
      if (y == 0) break;
      t = m.next_high(t) + 1;
      t = m.next_low(t) + 1;
      y--;
      t++;
    }
    t++;                   // pull Fh
    t++;                   // mov y, osr
    t += w.high_fine + 1;  // fine loop
  }
  return e;
}

int main() {
  // Every divide, every degree of phase, every width, against 12 Hz / 450 Hz / 7 kHz and one
  // absurd 250 kHz master where the reset pulse is most of the period.
  const uint32_t masters[] = { 18750000u, 500000u, 32000u, 900u };
  const uint32_t pulse = 3200;

  int checked = 0, period_bad = 0;
  double worst_phase_cyc = 0, worst_duty_cyc = 0;
  const char* worst_phase_at = "";
  static char buf[256];

  for (uint32_t d = 1; d <= 8; d++) {
    for (uint16_t ph = 0; ph < 360; ph++) {
      for (uint32_t wd = 1; wd <= 255; wd++) {
        for (uint32_t M : masters) {
          const uint32_t pl = (M > pulse * 2) ? pulse : M / 4;
          Master master{ M, pl };
          Words w = compute(d, M, ph, (uint8_t)wd, pl);
          Edges e = run(master, w, 12);
          checked++;

          const uint64_t want_period = (uint64_t)d * M;

          // Frequency lock: every rise-to-rise interval must be exact.
          bool bad = false;
          for (size_t i = 2; i + 1 < e.rises.size(); i++) {
            if (e.rises[i + 1] - e.rises[i] != want_period) {
              bad = true;
              break;
            }
          }
          if (bad) {
            period_bad++;
            printf("PERIOD FAIL d=%u ph=%u w=%u M=%u: got %llu want %llu "
                   "(Cl=%u Fl=%u Ch=%u Fh=%u)\n",
                   d, ph, wd, M, (unsigned long long)(e.rises[4] - e.rises[3]),
                   (unsigned long long)want_period, w.low_count, w.low_fine, w.high_count,
                   w.high_fine);
            continue;
          }

          // Phase is degrees of the master period, measured from the reset release, so the
          // check is where the rise sits inside its master period. Which master period of the
          // group it lands in is not observable (see subosc_compute_words) and not checked.
          const uint64_t r = e.rises[5];
          const uint64_t into_master = (r - master.pulse) % M;
          const double want_phase = (double)ph / 360.0 * (double)M;
          double phase_err = (double)into_master - want_phase;
          if (phase_err > (double)M / 2) phase_err -= (double)M;
          if (phase_err < -(double)M / 2) phase_err += (double)M;
          if (phase_err < 0) phase_err = -phase_err;

          // Duty: high time versus the requested fraction of the sub period.
          uint64_t f = 0;
          for (uint64_t fx : e.falls) {
            if (fx > r) {
              f = fx;
              break;
            }
          }
          const double want_high = (double)wd / 256.0 * (double)want_period;
          double duty_err = (double)(f - r) - want_high;
          if (duty_err < 0) duty_err = -duty_err;

          if (phase_err > worst_phase_cyc) {
            worst_phase_cyc = phase_err;
            snprintf(buf, sizeof buf, "d=%u ph=%u w=%u M=%u", d, ph, wd, M);
            worst_phase_at = buf;
          }
          if (duty_err > worst_duty_cyc) worst_duty_cyc = duty_err;
        }
      }
    }
  }

  printf("combinations checked: %d\n", checked);
  printf("period mismatches:    %d\n", period_bad);
  printf("worst phase error:    %.0f cycles (%s)\n", worst_phase_cyc, worst_phase_at);
  printf("worst duty error:     %.0f cycles\n", worst_duty_cyc);
  return period_bad ? 1 : 0;
}
