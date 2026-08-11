#ifndef __SUBOSC_H__
#define __SUBOSC_H__

#include <stdint.h>

// Parameter entry points (PARAM_SUB1/2_DIVIDE / _MASTER / _PHASE / _WIDTH). Safe to call from
// core 0: divide and master go through the deferred PIO request queue, phase and width only
// write mailboxes core 1 reads on its next control frame. Raw wire values in, all clamping done
// here. `sub` is 0 or 1, not an oscillator index.
//
// These exist on builds without the engine too, so params.ino needs no flags: divide on sub 0
// drives the single legacy sub, and everything else is ignored.
void subosc_param_divide(uint8_t sub, int16_t v);
void subosc_param_master(uint8_t sub, int16_t v);
void subosc_param_phase(uint8_t sub, int16_t v);
void subosc_param_width(uint8_t sub, int16_t v);
void subosc_param_logic_op(int16_t v);

// PARAM_DEBUG_COMMAND 4. Core 0; prints through the paced bench_out buffer.
void subosc2_report();

#ifdef ENABLE_SUBOSC_ENGINE2

// Two-sub engine on pio2 (see docs/PIO_OSCILLATORS.md section 9).
//
// One resident copy of subosc_seg serves both subs: pio2 SM index == sub index, each SM
// counting flybacks on the reset pin of whichever oscillator that sub follows. Frequency is
// edge-locked, so the programmable phase offset and pulse width can never detune the sub.
// Three segment words per sub period reach the SM by DMA, so nothing here runs per period on
// the CPU.
//
// Everything below is core-1 only: it touches PIO and DMA registers directly. Core 0 must
// go through the pio_defer_request_subosc_*() / pio_defer_service() pair like the rest of the
// PIO surface does.

void subosc2_init();

// divide: 0 = off, 1 = master rate (phase/PWM only), 2/4/8 = octaves down.
// osc: which oscillator's reset this sub locks to, 0..2. Same master on both subs gives
//      harmonic pulse patterns; different masters gives beating that tracks their detune.
// deg: rising-edge delay after the master's reset, in degrees of the *master* period, 0..359
//      (shifting the sub by whole master periods is not observable - see subosc.ino).
// width: duty in 1/256ths of the sub period, 1..255 (128 = the classic 50% square).
void subosc2_set_divide(uint8_t sub, uint8_t divide);
void subosc2_set_master(uint8_t sub, uint8_t osc);
void subosc2_set_phase_deg(uint8_t sub, uint16_t deg);
void subosc2_set_width(uint8_t sub, uint8_t width);

// Boolean logic combiner on SM3: both sub squares in, one combined square out on
// SUBOSC_LOGIC_PIN. op: 0 = off, 1 = XOR, 2 = AND, 3 = OR, 4 = XNOR, 5 = NAND, 6 = NOR,
// 7 = sub 1 alone, 8 = sub 2 alone. The pass-throughs make this pad the only sub output the
// carrier has to mix. The pair is fixed (SUBOSC_PINS[0]/[1], which must be adjacent GPIOs with
// sub 2 the upper one) so every change after the first is a jump-table rewrite that never
// interrupts the output. If the pins cannot support it the combiner stays off and says why in
// subosc2_report(), with the operator still remembered.
void subosc2_set_logic(uint8_t op);

// Recompute the segment words from each sub's master's current period. Called once per
// control frame from both voice engines, with the oscillator indices the frame just used.
void subosc2_update_periods(uint8_t osc_a, uint32_t total_a,
                            uint8_t osc_b, uint32_t total_b,
                            uint8_t osc_c, uint32_t total_c);

#endif  // ENABLE_SUBOSC_ENGINE2

#endif
