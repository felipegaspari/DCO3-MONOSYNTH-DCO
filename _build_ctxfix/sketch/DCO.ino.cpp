#include <Arduino.h>
#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/DCO.ino"
// #include <stdint.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <math.h>

/*  *** TO DO ***
- Fix PULSE PWN not received or updated when loading patches.
- Ask the AI to optimize and clean the autotune code. 
*/

// =============================================================================
// ENGINE — pitch mode ids (needed by board defaults + overrides below)
// =============================================================================
// Deep detail: docs/ENGINE_OPTIONS.md
//   0 FLOAT (walk find; natural modifier→ratio; needs float voice)
//   1 RATIO_Q16 (slopeQ16 + fused y→ratio; fixed default / float A/B)
//   2 Q12 (slope A/B: IntQ16 y + reciprocal; float A/B OK)
//   3 FLOAT_FAST (trunc+clamp±1 find; same float tables; needs float voice)
#define PITCH_INTERP_FLOAT 0
#define PITCH_INTERP_RATIO_Q16 1
#define PITCH_INTERP_Q12 2
#define PITCH_INTERP_FLOAT_FAST 3

// Clkdiv methods (CLKDIV_MODE). Accuracy order. Fixed: Q24 via clkdiv_live_total_cycles.
// Float: Hz via clkdiv_live_hz_total_cycles (Q16/Q8/FAST_Q4 convert Hz→Q24).
//   0 GOLD     — double llround(sys / Hz) from Q24 (gold standard / A/B)
//   1 FLOAT    — Q24 → float Hz → fminf(sys/hz + 0.5) (float-engine math)
//   2 Q16      — Q16 Hz → 64/32 (shipping)
//   3 Q8       — Q8 Hz → 32/32 + remainder; <16 Hz → internal precise Q8
//   4 FAST_Q4  — Q4 Hz → 32/32 (fastest, least accurate)
// Value 0 is GOLD, not the old HP0 Q4 path (that is FAST_Q4 = 4).
// Value 1 is FLOAT, not the old PRECISE_Q8 path (Q8 is 3).
#define CLKDIV_GOLD        0
#define CLKDIV_FLOAT       1
#define CLKDIV_Q16         2
#define CLKDIV_Q8          3
#define CLKDIV_FAST_Q4     4

// =============================================================================
// ENGINE — board defaults (Arduino core: PICO_RP2350 / else)
// =============================================================================
#if defined(PICO_RP2350)
  // RP2350 has an FPU: float voice + float amp-comp dual-build (LUT + Q8 for A/B).
  #ifndef USE_FLOAT_VOICE_TASK
    #define USE_FLOAT_VOICE_TASK
  #endif
  #ifndef PITCH_INTERP_MODE
    #define PITCH_INTERP_MODE PITCH_INTERP_FLOAT_FAST
  #endif
  #ifndef USE_FLOAT_AMP_COMP
    #define USE_FLOAT_AMP_COMP
  #endif
  #ifndef AMP_COMP_METHOD_DEFAULT
    #define AMP_COMP_METHOD_DEFAULT 0   // FLOAT_QUAD (0); LUT=1, FIXED=2 — cmds 20–22
  #endif
  #ifndef CLKDIV_MODE
    #define CLKDIV_MODE CLKDIV_FLOAT  // native Hz on float voice
  #endif
#else
// RP2040 / fallback: fixed voice + lean Q8 amp (no float amp tables / LUT RAM).
// CV outs stay fixed-point (no USE_FLOAT_CV_OUTS) — soft-float would choke Core1.
#ifndef AMP_COMP_METHOD_DEFAULT
#define AMP_COMP_METHOD_DEFAULT 2  // FIXED
#endif
#ifndef PITCH_INTERP_MODE
#define PITCH_INTERP_MODE PITCH_INTERP_RATIO_Q16
#endif
#ifndef CLKDIV_MODE
#define CLKDIV_MODE CLKDIV_Q16  // Q16 64/32
#endif
#endif

// Note-on sync retrigger (oscSync >= 1): 0 = EXACT_Y (Y load + phase hold), 1 = SYNC_JMP
// (restart jmp only; degree offsets need EXACT_Y). Runtime: cmds 26/27.
#ifndef NOTE_RETRIG_MODE_DEFAULT
#define NOTE_RETRIG_MODE_DEFAULT 0
#endif

// =============================================================================
// ENGINE — overrides (uncomment to force; after board defaults)
// =============================================================================
// #define USE_FLOAT_VOICE_TASK         // float voice (needs FPU; soft-float on RP2040)
// #define USE_FLOAT_AMP_COMP           // float amp dual-build (large RAM)
// #define USE_FLOAT_CV_OUTS            // float VCA/VCF path (soft-float tax on RP2040)
// #define CLKDIV_MODE CLKDIV_FAST_Q4   // Q4 32/32
// #define CLKDIV_MODE CLKDIV_Q8        // Q8 32/32+corr (faster than Q16)
// #define CLKDIV_MODE CLKDIV_Q16       // Q16 64/32 (shipping)
// #define CLKDIV_MODE CLKDIV_GOLD      // double llround; gold standard / A/B
// #define CLKDIV_MODE CLKDIV_FLOAT     // Q24 → float Hz (same math as float voice)
// #define AMP_COMP_METHOD_DEFAULT 1    // 0 FLOAT_QUAD / 1 LUT / 2 FIXED; needs USE_FLOAT_AMP_COMP for 0/1
// Pitch A/B (ids above; default already set — #undef then redefine):
// #undef PITCH_INTERP_MODE
// #define PITCH_INTERP_MODE PITCH_INTERP_FLOAT       // walk find A/B (needs float voice)
// #define PITCH_INTERP_MODE PITCH_INTERP_FLOAT_FAST  // trunc+clamp±1 (needs float voice)
// #define PITCH_INTERP_MODE PITCH_INTERP_RATIO_Q16   // shipping default both MCUs
// #define PITCH_INTERP_MODE PITCH_INTERP_Q12

// =============================================================================
// ENGINE — guards
// =============================================================================
#if (PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST) && !defined(USE_FLOAT_VOICE_TASK)
#error "PITCH_INTERP_FLOAT / FLOAT_FAST require USE_FLOAT_VOICE_TASK (board default or override)"
#endif
#if CLKDIV_MODE > 4
#error "CLKDIV_MODE must be CLKDIV_GOLD, FLOAT, Q16, Q8, or FAST_Q4"
#endif

// =============================================================================
// ENGINE — noise (see noise.h)
// =============================================================================
// NOISE_ENGINE — which DCO_Noise class noise0..1 use (see noise.h):
//   0 ColoredNoise     — Voss pink / 1-pole brown / white; whites from PioNoiseWhite
//   1 FastNoiseGen     — economy Voss pink / leaky brown / local xorshift white
//   2 PrimeHybridNoise — per-gen prime tables (997/1499/1999); dither + rephase
//   3 ProNoise32       — Q16.15 Kellett pink / DC-corrected brown / xorshift white
// Objects: noise0..1 in noise.h (ctor sets min/max/color/seed); next() in loop1.
// PIO white: dcoNoisePioBegin / dcoNoisePioRefill (library reads these flags).
// Bench: parent noise_gens + "noise refill".
#define NOISE_ENGINE 1
// #undef NOISE_ENGINE
// #define NOISE_ENGINE 0
// #define NOISE_ENGINE 1
// #define NOISE_ENGINE 2
// #define NOISE_ENGINE 3

// ENABLE_NOISE_OUT — PIO1 LFSR 1-bit white on GP2 (listen/scope). Comment out to
// free the pin. Engine 0 still uses LFSR FIFO seed (no GPIO). Engines 1/2/3 with
// this off skip PIO noise MMIO (clean benches). Library reads this flag.
//#define ENABLE_NOISE_OUT

// =============================================================================
// CALIBRATION — auto-cal boot defaults (runtime: Calibration tab debug cmds)
// =============================================================================
// Amp-comp method: 0 CLASSIC (per-note range-PWM search), 1 FREQ_TRACE
// (fixed-PWM frequency bisection; needs the manual 440 Hz anchor). Cmds 34/35.
#ifndef AUTOTUNE_AMP_METHOD_DEFAULT
#define AUTOTUNE_AMP_METHOD_DEFAULT 1
#endif
// Frequency-search close-in: 0 BISECT, 1 INTERP, 2 GATED. Cmds 37/38/39.
#ifndef AUTOTUNE_SEARCH_MODE_DEFAULT
#define AUTOTUNE_SEARCH_MODE_DEFAULT 1
#endif
// Amp-comp-0 endpoint (pair 0): 0 MEASURE (live hunt), 1 CALC (bottom-rung fit).
// Cmds 40/41.
#ifndef AUTOTUNE_AMP0_MODE_DEFAULT
#define AUTOTUNE_AMP0_MODE_DEFAULT 1
#endif
// Overrides (uncomment to force; #undef first):
// #undef AUTOTUNE_AMP_METHOD_DEFAULT
// #define AUTOTUNE_AMP_METHOD_DEFAULT 0   // CLASSIC
// #define AUTOTUNE_AMP_METHOD_DEFAULT 1   // FREQ_TRACE
// #undef AUTOTUNE_SEARCH_MODE_DEFAULT
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 0  // BISECT
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 1  // INTERP
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 2  // GATED
// #undef AUTOTUNE_AMP0_MODE_DEFAULT
// #define AUTOTUNE_AMP0_MODE_DEFAULT 0    // MEASURE
// #define AUTOTUNE_AMP0_MODE_DEFAULT 1    // CALC

// =============================================================================
// PROFILING / BENCH (see docs/BENCHMARKING.md)
// =============================================================================
// RUNNING_AVERAGE: hot-path profiler in bench.h (count/mean/min/max/total + core share).
// Off = zero cost. Needed for paced bench_out_* TX (profiler dump, amp/pitch benches).
// RUNNING_AVERAGE_FINE: also probes tiny stages; every probe is an opt barrier — changes
// codegen; for measuring that distortion, not for leaving on.
// RUNNING_AVERAGE_PERIOD: only loop/loop1 BENCH_PERIOD; stage probes compile out.
// Overrides FINE. Needs RUNNING_AVERAGE.
// BENCH_PATH_STATS: all path bumps (amp/ratio/porta + walk-step sums) and dump
// `-- Path counters --`. Needs RUNNING_AVERAGE; still no-op under PERIOD. Leave off for shipping.
// BENCH_STAGE_STRIDE: MAIN/FINE stage probes every Nth loop (default 9). 1 = every iter.
// BENCH_PERIOD is always every iter (speed truth). Note-on family always records.
// BENCH_USE_SYSTICK: 1 = SysTick for PERIOD + stages; 0 = 1 us timer for all probes.
// Dump window (1 s gate) always uses bench_us_now(). BENCH_PERIOD_MAX_US: discard PERIOD
// samples longer than this (autotune / wrap-looking stalls).
#define RUNNING_AVERAGE
// #define RUNNING_AVERAGE_FINE
#define RUNNING_AVERAGE_PERIOD
// #define BENCH_PATH_STATS
#ifndef BENCH_STAGE_STRIDE
#define BENCH_STAGE_STRIDE 1
#endif
#ifndef BENCH_USE_SYSTICK
#define BENCH_USE_SYSTICK 1
#endif
#ifndef BENCH_PERIOD_MAX_US
#define BENCH_PERIOD_MAX_US 20000
#endif
// ENABLE_MEM_DIAG: SRAM/heap dump (cmd 13) + loop/loop1 polls. Default on.
// Comment out for a zero-cost match to pre-mem_diag period-only dumps.
// Runtime 14/15 disable/enable polls without rebuild (dump 13 ignored while off).
#define ENABLE_MEM_DIAG

// Amp-comp speed/accuracy reports (debug cmds 24–25); needs RUNNING_AVERAGE + USE_FLOAT_AMP_COMP.
// #define AMP_COMP_BENCHMARK

#ifdef AMP_COMP_BENCHMARK
  #define USE_FLOAT_AMP_COMP
#endif
// =============================================================================
// BOARD / IO
// =============================================================================
// Serial hub: Serial2 GP20/21 is the only peer link (Input panel protocol + 'x'
// gap/cal TX). Screen is reached by Input relaying gap 154 on its Screen port.

// Accept slim panel protocol on USB CDC too (tools/dco_control). Comment out for
// production: stray terminal bytes are read as frame headers while enabled.
#define ENABLE_USB_CONTROL

// #define SERIAL_FRAMING_COBS  // A/B vs default RAW; host: dco_control --cobs

// All RANGE pins via dithered PIO PWM (wrap 4666, 3-frame → ~14000). Comment out
// to restore hardware PWM slices on RANGE_PINS[].
#define RANGE0_PIO_DITHER_TEST

// Phase 3 CV hardware (provisional pins in globals.h / docs/PINOUT.md).
// Leave commented on benches without filter/VCA/mux/DAC attached.
// #define ENABLE_CV_OUTS
// #define ENABLE_WAVE_MUX

// Dual-MCU: RP2040 voice-aux owns Dist Drive/Mix PWM + filter mode GPIO (later FX).
// Keep apply handlers/state; skip local pin writers so they do not fight the aux.
// Leave commented for solo RP2350B / single-MCU (full local IO). See docs/DUAL_MCU.md.
// #define ENABLE_VOICE_AUX

// Oscillator RESET pad polarity. Uncomment when discharge is through an active-low
// switch (e.g. DG411: IN low = on). PIO still uses logical 1 = assert / discharge;
// GPIO OUTOVER+INOVER invert the pad so soft sync jmp_pin and sub-osc wait keep
// working. Leave commented for active-high / direct FET discharge. See PIO_OSCILLATORS.md.
// DCO3 (DG411) defines this; DCO4 (active-high / FET) does not.
#include "project_config.h"
#if PROJECT_INSTRUMENT == 3
#define ENABLE_PIO_RESET_INVERT
#endif

// ENABLE_SUBOSC_ENGINE2 — two sub-oscillators on pio2, each following whichever main
// oscillator it is pointed at: two SMs share the subosc_seg program (edge-locked divide plus
// programmable phase offset and pulse width, three segment words streamed per period by DMA),
// and pio2 SM3 combines them with a boolean operator - the combined square, or either sub
// passed through, is what the carrier mixes. Off = the classic pio1 subosc_div2 / subosc_div4
// fixed 50% sub on OSC1 only.
//
// Needs a third PIO block, so it is a board default like the engine flags above: on for
// RP2350, unavailable on RP2040 (RP2040 keeps the old sub with no source edits). Uncomment
// the #undef to A/B the old sub on RP2350. See docs/PIO_OSCILLATORS.md.
#if defined(PICO_RP2350)
  #ifndef ENABLE_SUBOSC_ENGINE2
    #define ENABLE_SUBOSC_ENGINE2
  #endif
#endif
// #undef ENABLE_SUBOSC_ENGINE2
#if defined(ENABLE_SUBOSC_ENGINE2) && !defined(PICO_RP2350)
#error "ENABLE_SUBOSC_ENGINE2 needs a third PIO block (pio2), which RP2040 does not have"
#endif



#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
//#include "tusb_config.h"

#include "pico/stdlib.h"
// #include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico-dco.pio.h"
#include "hardware/pwm.h"
// #include "hardware/spi.h"

#include "LittleFS.h"
// #include <SingleFileDrive.h>
// #include <EEPROM.h>

#include <stdint.h>
#include "params_def.h"
#include "param_router.h"

#include "globals.h"
#include "amp_comp.h"
#include "cv_state.h"
#include "cv_out.h"

#include "FS.h"
#include "preset_store.h"

#include "noteList.h"

#include "Serial.h"
#include "midi.h"
#include "voices.h"
#include "state_machines.h"
#include "subosc.h"
#include "PWM.h"
#include "utils.h"
#include "Timer_micros.h"
#include "mem_diag.h"

#include "LFO.h"
#include "adsr.h"
#include "bench.h"
#include "midi_cc.h"
#include "midi_cc_map.h"  // generated; defines midiCcMap[], so include it once, here
#include "wave_mux.h"

#include "autotune.h"


// ****************************************************************************************** //

// Core 0 boot: USB, UART serial, MIDI handlers, LFOs, calibration input pin.
#line 310 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/DCO.ino"
void setup();
#line 335 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/DCO.ino"
void setup1();
#line 5 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
void init_LFOs();
#line 11 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
void init_DRIFT_LFOs();
#line 18 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
void init_DRIFT_LFO(lfo &LFO, byte LFONumber);
#line 27 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
void init_LFO1();
#line 36 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
void init_LFO2();
#line 14 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/PWM.ino"
void range_pio_set_level(uint8_t osc, uint16_t level);
#line 31 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/PWM.ino"
static void range_pio_enable_pin(uint8_t osc);
#line 39 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/PWM.ino"
void init_range_pio_dither();
#line 98 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/PWM.ino"
void init_pwm();
#line 12 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void serial_forward_input_block_to_mb(char cmd, const uint8_t* payload, uint8_t len);
#line 18 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void serial_send_adsr_block_to_mb(uint8_t cmd, uint16_t a, uint16_t d, uint16_t s, uint16_t r);
#line 28 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_send_adsr_vca_block_to_mb();
#line 34 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_send_adsr_vcf_block_to_mb();
#line 40 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_send_adsr_dco_block_to_mb();
#line 46 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_send_filter_block_to_mb();
#line 56 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_send_preset_loaded_to_mb(uint8_t slot);
#line 62 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_adsr1(char, const uint8_t* payload, uint8_t len);
#line 84 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_adsr2(char, const uint8_t* payload, uint8_t len);
#line 107 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_adsr3(char, const uint8_t* payload, uint8_t len);
#line 129 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_filter_block(char, const uint8_t* payload, uint8_t len);
#line 143 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serialSendParam16(byte paramNumber, int16_t paramValue, bool force);
#line 155 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serial_echo_persistable_param16(uint8_t id, int16_t value);
#line 162 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_param16(char, const uint8_t* payload, uint8_t len);
#line 172 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_preset_name(char, const uint8_t* payload, uint8_t len);
#line 180 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_bulk_chunk(char, const uint8_t* payload, uint8_t len);
#line 184 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_bulk_commit(char, const uint8_t* payload, uint8_t len);
#line 189 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
static void input_handle_preset_dir_request(char, const uint8_t*, uint8_t);
#line 208 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void init_serial();
#line 231 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void init_usb();
#line 283 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
void serialSendParam32(byte paramNumber, uint32_t paramValue, bool force);
#line 2 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Timer_micros.ino"
void init_micros_timers();
#line 3 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
static int adsr_sustain_for_set(uint16_t panel, uint16_t panel_full);
#line 17 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void init_ADSR();
#line 99 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_set_parameters();
#line 163 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR1_set_restart();
#line 169 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCA_set_restart();
#line 175 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCF_set_restart();
#line 181 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCA_change_attack_curve(uint8_t adsrCurveAttack);
#line 194 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCA_change_decay_curve(uint8_t adsrCurveDecay);
#line 207 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCF_change_attack_curve(uint8_t adsrCurveAttack);
#line 225 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR_VCF_change_decay_curve(uint8_t adsrCurveDecay);
#line 242 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
void ADSR1_change_curves();
#line 264 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/amp_comp_bench.ino"
void amp_comp_bench_run_speed();
#line 265 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/amp_comp_bench.ino"
void amp_comp_bench_run_accuracy();
#line 266 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/amp_comp_bench.ino"
void print_amp_comp_bench();
#line 45 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static uint32_t cdb_seq_osc_n();
#line 49 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static const char * cdb_method_name(uint8_t method);
#line 60 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static const char * cdb_live_clkdiv_name();
#line 76 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static const char * cdb_voice_name();
#line 84 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static int64_t cdb_hz_to_q24(double hz);
#line 88 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static int64_t cdb_float_hz_to_q24(float hz);
#line 92 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static double cdb_q24_to_hz(int64_t freq_q24);
#line 96 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static uint32_t cdb_pio(uint32_t total, uint32_t y, uint32_t w, uint32_t k);
#line 101 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static uint32_t cdb_clk_div_gold_ref(uint32_t sys_hz, double hz, uint32_t y, uint32_t w, uint32_t k);
#line 142 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static double cdb_out_hz(uint32_t sys_hz, uint32_t div, uint32_t y, uint32_t w, uint32_t k);
#line 149 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static double cdb_cents_abs(double out_hz, double target_hz);
#line 154 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static int cdb_hz_band(double hz);
#line 160 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static void cdb_hist_add(uint32_t *h, double cents);
#line 168 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static double cdb_hist_percentile(const uint32_t *h, uint32_t n, double pct);
#line 182 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static void cdb_ensure_jump_grid();
#line 211 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static void cdb_speed_run(uint8_t method, const int64_t *q24, const double *hz, uint32_t nPts, uint32_t repeats, uint32_t sys_hz, uint32_t y, uint32_t w, uint32_t k, uint32_t *outFrames, uint64_t *outUs);
#line 235 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static void cdb_speed_run_seq(uint8_t method, uint32_t sys_hz, uint32_t y, uint32_t w, uint32_t k, uint32_t *outFrames, uint64_t *outUs);
#line 276 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
static void cdb_speed_print_table(const char *pattern, uint32_t unique, uint32_t repeats, const uint32_t *frames, const uint64_t *totalUs);
#line 303 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
void clkdiv_hp_bench_run_speed();
#line 354 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
void clkdiv_hp_bench_run_accuracy();
#line 592 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
void print_clkdiv_hp_bench();
#line 22 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
static uint16_t lerp_0_4095(uint16_t value, uint16_t y0, uint16_t y1);
#line 26 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
static uint16_t cv_clamp_u12(int32_t v);
#line 33 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
static uint16_t cv_q15_to_u12(int16_t q15);
#line 38 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void init_cv_out();
#line 48 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void cv_bake_adsr2_to_vcf_scale();
#line 59 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void cv_bake_lfo2_to_vcf_scale();
#line 70 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void cv_bake_lfo1_to_vca_scale();
#line 81 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void cv_update_mod_scales();
#line 280 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
void update_CV_outs_manual_calibration();
#line 29 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mem_diag.ino"
void mem_diag_request();
#line 39 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mem_diag.ino"
void mem_diag_poll_core1_work();
#line 45 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mem_diag.ino"
static int mem_diag_stack_used(int free_b, unsigned bank);
#line 56 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mem_diag.ino"
void mem_diag_poll_core0_work();
#line 12 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void mono_note_stack_clear();
#line 18 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void init_midi();
#line 40 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handleNoteOn(byte channel, byte pitch, byte velocity);
#line 44 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handleNoteOff(byte channel, byte pitch, byte velocity);
#line 50 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handleControlChange(byte channel, byte number, byte value);
#line 76 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void midi_cc_handle(uint8_t number, uint8_t value);
#line 93 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void midi_cc_apply(uint8_t target, int16_t value);
#line 139 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handleProgramChange(byte channel, byte program);
#line 148 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handlePitchBend(byte channel, int pitchBend);
#line 153 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void handleAfterTouchChannel(byte channel, byte pressure);
#line 161 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void note_on(uint8_t note, uint8_t velocity);
#line 217 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
void note_off(uint8_t note);
#line 13 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static void mod_refresh_slot_live(uint8_t slot);
#line 38 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static int32_t mod_depth_mul_q15(int32_t src_q15, int16_t depth);
#line 50 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static uint16_t mod_clamp_u16(int32_t v);
#line 56 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static int32_t mod_clamp_q15(int32_t v);
#line 62 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_init();
#line 79 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_set_source(uint8_t slot, int16_t v);
#line 89 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_set_dest(uint8_t slot, int16_t v);
#line 99 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_set_depth(uint8_t slot, int16_t v);
#line 105 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_on_note_on();
#line 110 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_set_aftertouch(uint8_t pressure);
#line 114 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_set_mod_wheel(uint8_t value);
#line 119 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static int32_t mod_matrix_read_source_q15(uint8_t src, int16_t lfo1_q15, int16_t lfo2_q15);
#line 160 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_accumulate(int32_t dest_sums[MOD_DEST_COUNT], int16_t lfo1_q15, int16_t lfo2_q15);
#line 223 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
static int32_t mod_pitch_sum_to_q24(int32_t pitch_s);
#line 230 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
int32_t mod_matrix_pitch_to_q24(int32_t pitch_s);
#line 234 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
void mod_matrix_apply_cv(const int32_t dest_sums[MOD_DEST_COUNT], uint16_t* dist_drive_out, uint16_t* dist_mix_out);
#line 41 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_wave_enable(uint8_t osc, uint8_t wave, int16_t v);
#line 47 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc1_saw_enable(int16_t v);
#line 48 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc1_pulse_enable(int16_t v);
#line 49 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc1_tri_enable(int16_t v);
#line 50 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_saw_enable(int16_t v);
#line 51 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_pulse_enable(int16_t v);
#line 52 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_tri_enable(int16_t v);
#line 53 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_saw_enable(int16_t v);
#line 54 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_pulse_enable(int16_t v);
#line 55 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_tri_enable(int16_t v);
#line 58 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sine_status(int16_t v);
#line 62 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_resonance_comp(int16_t v);
#line 66 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_vca_adsr_restart(int16_t v);
#line 71 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_vcf_adsr_restart(int16_t v);
#line 77 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr3_to_osc_select(int16_t v);
#line 82 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_waveform(int16_t v);
#line 89 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_waveform(int16_t v);
#line 96 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_octave_shift(int16_t v);
#line 101 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_interval(int16_t v);
#line 106 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_interval(int16_t v);
#line 111 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_detune_val(int16_t v);
#line 116 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_detune_val(int16_t v);
#line 120 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc_depth(int16_t v, int32_t& depth_q24);
#line 126 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc2(int16_t v);
#line 131 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc3(int16_t v);
#line 136 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc_coarse_depth(int16_t v, int32_t& depth_q24);
#line 141 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc2_coarse(int16_t v);
#line 145 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_osc3_coarse(int16_t v);
#line 150 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_character(int16_t v);
#line 159 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc_sync_mode(int16_t v);
#line 203 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_portamento_time(int16_t v);
#line 225 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_vcf_keytrack(int16_t v);
#line 242 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_velocity_to_vcf(int16_t v);
#line 252 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_velocity_to_vca(int16_t v);
#line 262 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc1_level(int16_t v);
#line 269 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc2_level(int16_t v);
#line 276 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_osc3_level(int16_t v);
#line 283 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub_level(int16_t v);
#line 293 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot0_source(int16_t v);
#line 293 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot0_dest(int16_t v);
#line 293 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot0_depth(int16_t v);
#line 294 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot1_source(int16_t v);
#line 294 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot1_dest(int16_t v);
#line 294 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot1_depth(int16_t v);
#line 295 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot2_source(int16_t v);
#line 295 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot2_dest(int16_t v);
#line 295 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot2_depth(int16_t v);
#line 296 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot3_source(int16_t v);
#line 296 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot3_dest(int16_t v);
#line 296 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot3_depth(int16_t v);
#line 297 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot4_source(int16_t v);
#line 297 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot4_dest(int16_t v);
#line 297 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot4_depth(int16_t v);
#line 298 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot5_source(int16_t v);
#line 298 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot5_dest(int16_t v);
#line 298 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot5_depth(int16_t v);
#line 299 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot6_source(int16_t v);
#line 299 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot6_dest(int16_t v);
#line 299 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot6_depth(int16_t v);
#line 300 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot7_source(int16_t v);
#line 300 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot7_dest(int16_t v);
#line 300 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_mod_slot7_depth(int16_t v);
#line 306 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_portamento_mode(int16_t v);
#line 319 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_voice_mode(int16_t v);
#line 325 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_voice_alloc_mode(int16_t v);
#line 331 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_unison_detune(int16_t v);
#line 336 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_analog_drift_amount(int16_t v);
#line 347 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_analog_drift_speed(int16_t v);
#line 359 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_analog_drift_spread(int16_t v);
#line 371 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sync_mode(int16_t v);
#line 385 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_soft_sync(int16_t v);
#line 397 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_subosc_divide(int16_t v);
#line 410 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub1_divide(int16_t v);
#line 411 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub2_divide(int16_t v);
#line 412 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub1_master(int16_t v);
#line 413 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub2_master(int16_t v);
#line 414 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub1_phase(int16_t v);
#line 415 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub2_phase(int16_t v);
#line 416 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub1_width(int16_t v);
#line 417 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub2_width(int16_t v);
#line 418 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_sub_logic_op(int16_t v);
#line 421 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_dco(int16_t v);
#line 427 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_osc_depth(int16_t v, int32_t& depth_q24);
#line 432 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_osc1(int16_t v);
#line 436 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_osc2(int16_t v);
#line 440 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_osc3(int16_t v);
#line 445 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_speed(int16_t v);
#line 452 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_speed(int16_t v);
#line 459 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_vca_level(int16_t v);
#line 464 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_dist_drive(int16_t v);
#line 468 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_dist_mix(int16_t v);
#line 473 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_filter_mode(int16_t v);
#line 477 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo1_to_vca(int16_t v);
#line 483 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_lfo2_to_pw(int16_t v);
#line 490 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr1_to_pwm(int16_t v);
#line 497 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr1_to_detune1(int16_t v);
#line 516 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr3_pitch_mode(int16_t v);
#line 521 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr1_attack_curve(int16_t v);
#line 526 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr1_decay_curve(int16_t v);
#line 532 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr2_attack_curve(int16_t v);
#line 537 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr2_decay_curve(int16_t v);
#line 543 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_pw_value(int16_t v);
#line 549 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr1_to_vca(int16_t v);
#line 554 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_pwm_pots_manual(int16_t v);
#line 558 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_adsr3_enabled(int16_t v);
#line 577 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_calibration_flag(int16_t v);
#line 603 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_manual_calibration_flag(int16_t v);
#line 657 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_manual_calibration_step(int16_t v);
#line 663 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_amp_comp_440(int16_t v);
#line 676 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_amp_comp_duty_offset(int16_t v);
#line 684 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_cal_pw_center(int16_t v);
#line 693 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_manual_calibration_stage(int16_t v);
#line 709 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_manual_calibration_offset(int16_t v);
#line 732 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_preset_save(int16_t v);
#line 736 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_preset_load(int16_t v);
#line 740 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_preset_dump(int16_t v);
#line 744 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_cal_dump(int16_t v);
#line 769 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
static void apply_param_debug_command(int16_t v);
#line 1149 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
void init_param_router();
#line 1154 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
void update_parameters(uint16_t paramNumber, int16_t paramValue);
#line 63 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static const char * pitch_bench_method_name(uint8_t m);
#line 73 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static const char * pitch_bench_live_mode_name();
#line 87 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static void pib_reset_cache();
#line 91 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static void pib_ensure_tables();
#line 139 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int pib_find_seg_int(const int32_t *xTab, int32_t x, int dcoIndex);
#line 172 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static float pib_interp_float(float modifier, int dcoIndex);
#line 211 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static float pib_interp_float_fast(float modifier, int dcoIndex);
#line 237 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int32_t pib_interp_ratio_q16(int32_t xQ16, int dcoIndex);
#line 262 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int32_t pib_interp_y_q12(int32_t xQ16, int dcoIndex);
#line 272 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int32_t pib_interp_y_q20(int32_t xQ16, int dcoIndex);
#line 284 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int32_t pib_y_to_ratio_q16(int32_t yTab);
#line 291 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static float pitch_bench_call(uint8_t method, float modifier, int dco);
#line 316 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static double pib_cents_abs(float cand, float ref);
#line 322 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static bool pib_is_knot(float modifier);
#line 335 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static int pib_x_band(float modifier);
#line 341 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static void pib_hist_add(uint32_t *h, double cents);
#line 349 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static double pib_hist_percentile(const uint32_t *h, uint32_t n, double pct);
#line 364 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static uint32_t pib_mod_samples_per_osc(float modStep);
#line 374 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static uint32_t pib_q16_samples(int32_t step);
#line 382 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static void pib_speed_run(uint8_t method, float modStep, int32_t xIntStep, int32_t xQ16Step, uint32_t repeats, uint32_t *outCalls, uint64_t *outUs);
#line 463 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
static void pib_speed_print_table(const char *pattern, float modStep, int32_t xIntStep, int32_t xQ16Step, uint32_t repeats, const uint32_t *totalCalls, const uint64_t *totalUs);
#line 493 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
void pitch_interp_bench_run_speed();
#line 556 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
void pitch_interp_bench_run_accuracy();
#line 740 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
void print_pitch_interp_bench();
#line 17 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void preset_chunk_filename(uint8_t chunkIndex, char* out, size_t cap);
#line 21 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static uint8_t preset_chunk_index(uint8_t slot);
#line 25 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static uint16_t preset_slot_offset(uint8_t slot);
#line 29 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static const char * preset_bulk_target_name(uint8_t target);
#line 45 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_print_begin(const char* target, int slot, uint32_t size);
#line 55 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_print_data_line(uint16_t offset, const uint8_t* data, uint16_t len);
#line 64 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_print_end(const char* target, uint32_t crc);
#line 68 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_print_err(const char* target, const char* reason);
#line 73 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_buffer(const char* target, int slot, const uint8_t* data, uint16_t size);
#line 88 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void dump_fs_file(const char* target, const char* filename, uint32_t expectedSize);
#line 125 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void preset_record_build(uint8_t* buf);
#line 153 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static bool preset_record_validate(const uint8_t* buf);
#line 162 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void preset_record_apply(const uint8_t* buf);
#line 204 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static void preset_store_write_last(uint8_t slot);
#line 216 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static bool preset_chunk_write_record(uint8_t slot, const uint8_t* record, const char* errTag);
#line 282 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
static bool preset_chunk_read_record(uint8_t slot, uint8_t* out);
#line 304 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_store_save(uint8_t slot);
#line 313 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
bool preset_store_load(uint8_t slot);
#line 336 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_store_send_directory_to_mb();
#line 375 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_store_dump(int16_t sel);
#line 416 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_store_cal_dump(int16_t sel);
#line 432 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_bulk_chunk(const uint8_t* payload, uint8_t len);
#line 440 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_bulk_commit(const uint8_t* payload, uint8_t len);
#line 502 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
void preset_store_boot_recall();
#line 22 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
static void serial2_dma_poll_unlocked();
#line 45 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
void serial2_dma_init();
#line 57 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
void serial2_dma_poll();
#line 63 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
bool serial2_dma_tx_ready();
#line 76 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
size_t serial2_dma_write(const uint8_t *p, size_t n);
#line 5 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static int sync_slave_osc();
#line 11 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static int sync_master_osc();
#line 20 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void assign_sm_mapping();
#line 34 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static const pio_program_t* soft_sync_program_for_chunks(uint8_t chunks);
#line 43 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static void ensure_soft_sync_program(uint8_t chunks);
#line 57 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void init_pio();
#line 106 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static void pio_reset_pin_apply_polarity(uint pin);
#line 118 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void start_voice_sms();
#line 186 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void osc_reload_reset_pulse_all(uint32_t y);
#line 213 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_topology_report();
#line 253 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
static void pio_period_probe_run(uint8_t osc, uint32_t clk_div);
#line 261 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_period_probe(uint8_t osc, uint32_t clk_div);
#line 268 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_solve_period_model(uint32_t clk_div_a, double measured_hz_a, uint32_t clk_div_b, double measured_hz_b, uint32_t y);
#line 303 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void set_subosc_divide(uint8_t divide);
#line 363 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_sync_mode();
#line 367 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_cal_restore();
#line 371 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_reset_pulse_all();
#line 375 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_subosc(uint8_t divide);
#line 382 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_subosc_divide(uint8_t sub, uint8_t divide);
#line 392 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_subosc_master(uint8_t sub, uint8_t osc);
#line 402 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_subosc_logic_op(uint8_t op);
#line 409 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_request_period_probe(uint8_t osc, uint32_t clk_div);
#line 416 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_probe_report_flush();
#line 438 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
void pio_defer_service();
#line 48 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static int32_t subosc_mod_clamped(int32_t v);
#line 66 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static bool subosc_pin_wired(uint8_t sub);
#line 72 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static uint32_t subosc_mul_q16(uint32_t q16, uint32_t cycles);
#line 88 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_compute_words(uint8_t sub, uint32_t master_cycles);
#line 171 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_seed_words(uint8_t sub);
#line 181 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_dma_stop(uint8_t sub);
#line 198 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_dma_start(uint8_t sub);
#line 230 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_stop_one(uint8_t sub);
#line 243 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_start_one(uint8_t sub);
#line 293 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static bool subosc_logic_wiring_ok(const char** why);
#line 319 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_logic_write_table(uint8_t op);
#line 327 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
static void subosc_logic_stop();
#line 335 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_set_logic(uint8_t op);
#line 364 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_init();
#line 402 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_set_divide(uint8_t sub, uint8_t divide);
#line 428 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_set_master(uint8_t sub, uint8_t osc);
#line 438 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_set_phase_deg(uint8_t sub, uint16_t deg);
#line 445 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_set_width(uint8_t sub, uint8_t width);
#line 459 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_update_periods(uint8_t osc_a, uint32_t total_a, uint8_t osc_b, uint32_t total_b, uint8_t osc_c, uint32_t total_c);
#line 489 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc_param_divide(uint8_t sub, int16_t v);
#line 498 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc_param_master(uint8_t sub, int16_t v);
#line 507 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc_param_phase(uint8_t sub, int16_t v);
#line 515 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc_param_width(uint8_t sub, int16_t v);
#line 526 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc_param_logic_op(int16_t v);
#line 535 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
void subosc2_report();
#line 3 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/utils.ino"
uint16_t linearToLogarithmic(uint16_t linearValue, float base, uint16_t maxValue);
#line 17 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/utils.ino"
uint16_t linearToExponential(uint16_t linearValue, float base, uint16_t maxValue);
#line 28 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/utils.ino"
float expConverterFloat(uint16_t readingValue, uint16_t curve);
#line 38 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/utils.ino"
uint16_t expConverter(uint16_t readingValue, uint16_t curve);
#line 52 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static float interpolate_live_ratio_f(float modifiers, int dcoIndex);
#line 69 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void init_voices();
#line 86 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static int64_t noteQ16_to_freqQ24(int32_t note_q16);
#line 118 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static int32_t freqQ24_to_noteQ16(int64_t freq_q24);
#line 151 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static int64_t float_to_q24(float f);
#line 157 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static uint8_t midi_offset_to_table_index(int midi_or_base, int offset, size_t table_len);
#line 167 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static int32_t porta_resolve_start_note_q16(uint8_t osc, int32_t target_q16);
#line 180 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_latch_endpoints_q16(uint8_t osc, int32_t start_q16, int32_t target_q16);
#line 192 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_time_q16(uint8_t osc, int32_t start_q16, int32_t target_q16, int32_t T_fixed);
#line 207 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_slew_q16(uint8_t osc, int32_t start_q16, int32_t target_q16, int32_t T_slew);
#line 222 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_glide_q16(uint8_t osc, int32_t start_q16, int32_t target_q16, uint8_t mode);
#line 235 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static float noteIndex_to_freqFloat(float noteIndex);
#line 250 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static float freqFloat_to_noteIndex(float hz);
#line 275 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static float porta_resolve_start_note_f(uint8_t osc, float target);
#line 287 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_latch_endpoints_f(uint8_t osc, float startNote, float targetNote);
#line 300 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_time_f(uint8_t osc, float startNote, float targetNote, float T_fixed);
#line 317 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_slew_f(uint8_t osc, float startNote, float targetNote, float T_slew);
#line 331 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static void porta_setup_glide_f(uint8_t osc, float startNote, float targetNote, uint8_t mode);
#line 344 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static float q24_to_float(int32_t q);
#line 1031 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void voice_task_main();
#line 1675 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
uint8_t voice_alloc();
#line 1681 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void voice_mark_on(uint8_t voice, uint8_t note, uint8_t velocity);
#line 1692 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void voice_mark_off(uint8_t voice);
#line 1703 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void setVoiceMode();
#line 1730 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void setSyncMode();
#line 1749 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
static uint16_t amp_level_q24(int64_t freq_q24, uint8_t osc);
#line 1994 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
uint16_t get_PW_level_interpolated(uint16_t PWval, uint8_t oscN);
#line 2029 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void voice_task_autotune(uint8_t taskAutotuneVoiceMode, uint16_t calibrationValue);
#line 2364 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
void initMultiplierTables();
#line 75 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/wave_mux.ino"
void init_waveSelector();
#line 76 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/wave_mux.ino"
void update_waveSelector();
#line 77 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/wave_mux.ino"
void waveSelector_manual_calibration(byte);
#line 310 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/DCO.ino"
void setup() {
  sys_clock_hz_refresh();  // Arduino already set clk_sys; cache real Hz for clkdiv
  // EEPROM.begin(512);
  bench_init_core();  // SysTick is per core; core 1 arms its own in setup1()
  init_micros_timers();
  init_usb();
  init_serial();
  init_param_router();
  init_midi();

  init_LFOs();
  init_DRIFT_LFOs();


  // init_tuner();
  // init_tuning_tables();

  pinMode(DCO_calibration_pin, INPUT);

  // gpio_init(11);
  // gpio_set_dir(11, GPIO_IN);
  // gpio_pull_down(11);
}

// Core 1 boot: LittleFS cal load, ADSR, amp-comp precompute, PWM/PIO, voices.
void setup1() {

  sys_clock_hz_refresh();  // Arduino already set clk_sys; cache real Hz for clkdiv

  bench_init_core();
  init_micros_timers();

  // Create voiceTables only if the file is missing (before init_FS stubs it).
  // Force overwrite: PARAM_DEBUG_COMMAND 30 / dco_control Calibration tab.
  seed_fake_calibration_tables(false);
  init_FS();

  init_ADSR();
  init_cv_out();
  mod_matrix_init();
  init_waveSelector();

  // Select amplitude-compensation precompute based on engine type.
  precompute_amp_comp_for_engine();

  calibrationFlag = false;
  manualCalibrationFlag = false;
  firstTuneFlag = false;

  init_pwm();
  init_pio();
#ifdef RANGE0_PIO_DITHER_TEST
  init_range_pio_dither();
#endif
  dcoNoisePioBegin(pio[NOISE_PIO], NOISE_SM);
  init_voices();
}

// Core 0 forever loop: MIDI every iter; Serial2 + USB CDC on 1 ms; ~50 µs LFO1 + LFO2 + drift.
void __not_in_flash_func(loop)() {
  BENCH_PERIOD(loop0_period);
  BENCH_SAMPLE_TICK();

  {
    BENCH_BEGIN(loop0_microsTimer);
    microsTimer();
    BENCH_END(loop0_microsTimer);
  }

  {
    BENCH_BEGIN(loop0_midi);
    uint8_t midi_budget = MIDI_DRAIN_BYTE_BUDGET;
    if (TinyUSBDevice.mounted()) {
      while (midi_budget > 0 && MIDI_USB.read()) {
        midi_budget--;
      }
    }
    midi_budget = MIDI_DRAIN_BYTE_BUDGET;
    while (midi_budget > 0 && MIDI_SERIAL.read()) {
      midi_budget--;
    }
    BENCH_END(loop0_midi);
  }

  {
    BENCH_BEGIN(loop0_serial);
    if (timer1msFlag) {
      serial_panel_task();
#ifdef ENABLE_USB_CONTROL
      serial_usb_task();
#endif
      // One-shot recall of the last saved/loaded preset once both cores are up.
      preset_store_boot_task();
    }
    BENCH_END(loop0_serial);
  }

  if (timer50microsFlag == 1) {
    {
      BENCH_BEGIN(loop0_lfo1);
      LFO1();
      BENCH_END(loop0_lfo1);
    }


    {
      BENCH_BEGIN(loop0_lfo2);
      LFO2();
      BENCH_END(loop0_lfo2);
    }
  }
  if (timer51microsFlag == 1) {
    {
      BENCH_BEGIN(loop0_drift);
      DRIFT_LFOs();
      BENCH_END(loop0_drift);
    }
  }

  // Snapshot core 0's probes and print once core 1 has handed its own over. All profiler
  // serial traffic happens here, never on the audio core.
  bench_poll_core0();
  mem_diag_poll_core0();
}

// Core 1 forever loop: soft timers; auto/manual calibration OR ADSR + voice_task_main.
void __not_in_flash_func(loop1)() {
  BENCH_PERIOD(loop1_period);
  BENCH_SAMPLE_TICK();

  {
    BENCH_BEGIN(loop1_microsTimer);
    microsTimer2();
    BENCH_END(loop1_microsTimer);
  }

  {
    BENCH_BEGIN(loop1_noise);
    {
      BENCH_BEGIN(loop1_noise_refill);
      dcoNoisePioRefill();
      BENCH_END(loop1_noise_refill);
    }
    noiseLevel[0] = noise0.next();
    noiseLevel[1] = noise1.next();
    BENCH_END(loop1_noise);
  }

  if (calibrationFlag == true) {
    if (manualCalibrationFlag == true) {
      // voice_task_autotune() solos by stopping every other SM, so the pair must
      // be unsynced first or the soloed oscillator loses its RESET pin to a
      // stopped partner. Core 0 only books it (autotune.h); the rebuild is PIO
      // work and this branch never reaches pio_defer_service().
      if (calSyncNeutralRequested) {
        calSyncNeutralRequested = false;
        setSyncMode();
      }

      // Keep currentDCO in sync so [GAP_MEASURE]/[GAP_TIMEOUT] logs match the soloed osc.
      currentDCO = cal_manual_osc();

      if (manualCalibrationStep == 1) {
        // Step 2: dial in the per-osc amp-comp value at 440 Hz (A4). The
        // stored value anchors the FREQ_TRACE calibration curve, and the
        // fast duty feedback (~27x quicker than the low note) makes the
        // adjustment feel live.
        VOICE_NOTES[0] = manual_cal_reference_note;
        DCO_calibration_current_note = manual_cal_reference_note;
        if (ampComp440[currentDCO] != 0) {
          ampCompCalibrationVal = ampComp440[currentDCO];
        } else {
          // First entry for this osc: drive near the expected operating point
          // by scaling the trimmed low-note value with the frequency ratio
          // (charge current, hence range PWM, is roughly proportional to
          // frequency). Do not write ampComp440[] — 0 means "never set" and
          // Store would persist a seed the user never confirmed.
          float scale = note_to_freq(manual_cal_reference_note) /
                        note_to_freq(manual_DCO_calibration_start_note);
          ampCompCalibrationVal = (uint16_t)(
            (initManualAmpCompCalibrationValPreset + manualCalibrationOffset[currentDCO]) * scale + 0.5f);
        }
      } else {
        // Step 1: trimpot stage at the low starting note (same reference the
        // PW calibration and the classic amp-comp method assume).
        VOICE_NOTES[0] = manual_DCO_calibration_start_note;
        DCO_calibration_current_note = manual_DCO_calibration_start_note;
        ampCompCalibrationVal = initManualAmpCompCalibrationValPreset + manualCalibrationOffset[currentDCO];
      }
      voice_task_autotune(0, ampCompCalibrationVal);
      update_CV_outs_manual_calibration();
      // In manual calibration mode, continuously measure and report the duty
      // difference so the screen can display live feedback for the user.
      DCO_calibration_debug();
      //Serial.println((String) "PW value: " + (PW[0] / 4));

      // Runs between two manual passes, so the oscillator it measures is
      // already soloed and the next pass restores the substage's PW.
      if (pwCvProbeRequested) {
        pwCvProbeRequested = false;
        run_pw_cv_probe();
      }

    } else {
      DCO_calibration();
    }
  } else if (calibrationVerifyRequested) {
    // Debug command 36 (core 0). The sweep blocks this core for as long as it
    // takes and raises calibrationFlag itself, so the voice task stays off
    // these oscillators until it is done.
    calibrationVerifyRequested = false;
    run_calibration_verify_sweep();
  } else {

    pio_defer_service();

    if (timer99microsFlag2 == 1) {
      {
        BENCH_BEGIN(loop1_adsr);
        ADSR_update();
        BENCH_END(loop1_adsr);
      }
      {
        BENCH_BEGIN(loop1_cv_outs);
        update_CV_outs();
        BENCH_END(loop1_cv_outs);
      }
    }

    {
      BENCH_BEGIN(voice_task);
      voice_task_main();
      BENCH_END(voice_task);
    }
  }

  // Hand this core's counters to core 0, which does all the printing.
  bench_service(1);
  mem_diag_poll_core1();
}
#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/FS.ino"
// Shim: calibration storage definitions live in the shared library.
// Sorts first among the sketch .ino files, so these definitions precede the
// autotune impls that call them. Format: _shared/docs/CALIBRATION_STORAGE.md
#include "_shared/FS_impl.h"

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/LFO.ino"
// Compile mo-lfo into the sketch TU (Arduino IDE does not link _build_libs/*.cpp).
#include "_build_libs/mo-lfo/mo-lfo.cpp"

// Init LFO1 and LFO2 instances. Called from setup() (Core 0).
void init_LFOs() {
  init_LFO1();
  init_LFO2();
}

// Init all per-oscillator drift LFOs. Called from setup() (Core 0).
void init_DRIFT_LFOs() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    init_DRIFT_LFO(LFO_DRIFT_CLASS[i], i);
  }
}

// Configure one drift LFO. Wave bus is full-scale Q15 (dacSize unused).
void init_DRIFT_LFO(lfo &LFO, byte LFONumber) {
  LFO_DRIFT_SPEED_OFFSET[LFONumber] = (float)(1.00f - (float)((float)analogDriftSpread * 0.005) + (float)((float)analogDriftSpread * 0.00125f * (float)LFONumber)) * (float)expConverterFloat((float)analogDriftSpeed, 5000);
  LFO.setWaveForm(LFO_DRIFT_WAVEFORM);
  LFO.setAmplQ15(MO_LFO_Q15_ONE);
  LFO.setMode(false);               // free-running (Hz)
  LFO.setMode0Freq(LFO_DRIFT_SPEED_OFFSET[LFONumber], micros());
}

// Configure main detune LFO (LFO1). Amplitude is Q15 full-scale; depths live in *_q24.
void init_LFO1() {
  LFO1_class.setWaveForm(LFO1Waveform);
  LFO1_class.setAmplQ15(MO_LFO_Q15_ONE);
  LFO1_class.setMode(false);
  LFO1_class.setMode0Freq(0.5f);
}


// Configure secondary LFO (LFO2: PW / OSC2 detune, etc.).
void init_LFO2() {
  LFO2_class.setWaveForm(LFO2Waveform);
  LFO2_class.setAmplQ15(MO_LFO_Q15_ONE);
  LFO2_class.setMode(false);
  LFO2_class.setMode0Freq(5.0f);
}

// Update LFO1 Q15 level + per-osc pitch mods (depth_q24 is full-scale octave travel).
void __not_in_flash_func(LFO1)() {
  LFO1Level = LFO1_class.getWaveQ15(micros());
  // Common case: only global LFO1→DCO — one mul, broadcast to all osc slots.
  if (LFO1toOSC1_q24 == 0 && LFO1toOSC2_q24 == 0 && LFO1toOSC3_q24 == 0) {
    const int32_t m = applyDepthQ24(LFO1Level, LFO1toDCO_q24);
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] = m;
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] = m;
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC3] = m;
  } else {
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] =
      applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC1_q24);
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] =
      applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC2_q24);
    lfo1_pitch_mod_q24[LFO1_PITCH_OSC3] =
      applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC3_q24);
  }
}

// Update LFO2 Q15 level + OSC2/3 pitch mods (fine + coarse folded).
// DMB after LFO1()+LFO2() stores so Core1 snapshots a coherent mailbox (not SIO FIFO).
void __not_in_flash_func(LFO2)() {
  LFO2Level = LFO2_class.getWaveQ15(micros());
  lfo2_pitch_mod_q24[LFO2_PITCH_OSC2] =
    applyDepthQ24(LFO2Level, LFO2toOSC2_q24 + LFO2toOSC2_coarse_q24);
  lfo2_pitch_mod_q24[LFO2_PITCH_OSC3] =
    applyDepthQ24(LFO2Level, LFO2toOSC3_q24 + LFO2toOSC3_coarse_q24);
  __dmb();
}

// Update per-oscillator drift LFO levels as Q15 (negate wave = legacy polarity).
// Publish mailbox for Core 1: three stores then DMB (not SIO FIFO).
void __not_in_flash_func(DRIFT_LFOs)() {
  unsigned long currentMicros = micros();
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_LEVEL[i] = (int16_t)(-LFO_DRIFT_CLASS[i].getWaveQ15(currentMicros));
  }
  __dmb();
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/PWM.ino"
#ifdef RANGE0_PIO_DITHER_TEST
#include "hardware/dma.h"

// osc0/1: pio1 SM2/SM3 (SM0=subosc, SM1=noise). osc2: pio0 SM3 (SM0–2=voices).
static const uint8_t RANGE_PIO_BLOCK[NUM_OSCILLATORS] = { 1, 1, 0 };
static const uint8_t RANGE_PIO_SM[NUM_OSCILLATORS] = { 2, 3, 3 };

static uint32_t range_pio_duty[NUM_OSCILLATORS][RANGE_PIO_FRAMES];
static uint32_t range_dma_src_addr[NUM_OSCILLATORS];
static int range_dma_data[NUM_OSCILLATORS] = { -1, -1, -1 };
static int range_dma_ctrl[NUM_OSCILLATORS] = { -1, -1, -1 };
static bool range_pio_ready = false;

void range_pio_set_level(uint8_t osc, uint16_t level) {
  if (osc >= NUM_OSCILLATORS) {
    return;
  }
  if (level > DIV_COUNTER) {
    level = DIV_COUNTER;
  }
  const uint32_t t =
      ((uint32_t)level * RANGE_PIO_LEVELS + (DIV_COUNTER / 2u)) / DIV_COUNTER;
  const uint16_t base = (uint16_t)(t / RANGE_PIO_FRAMES);
  const uint16_t rem = (uint16_t)(t % RANGE_PIO_FRAMES);
  for (uint32_t i = 0; i < RANGE_PIO_FRAMES; i++) {
    const uint16_t d = (uint16_t)(base + (i < rem ? 1u : 0u));
    range_pio_duty[osc][i] = ((uint32_t)(RANGE_PIO_PERIOD - d) << 16) | d;
  }
}

static void range_pio_enable_pin(uint8_t osc) {
  PIO p = pio[RANGE_PIO_BLOCK[osc]];
  const uint sm = RANGE_PIO_SM[osc];
  pio_gpio_init(p, RANGE_PINS[osc]);
  pio_sm_set_consecutive_pindirs(p, sm, RANGE_PINS[osc], 1, true);
  pio_sm_set_enabled(p, sm, true);
}

void init_range_pio_dither() {
  if (range_pio_ready) {
    for (uint8_t osc = 0; osc < NUM_OSCILLATORS; osc++) {
      range_pio_enable_pin(osc);
    }
    return;
  }

  const uint offset1 = pio_add_program(pio[1], &range_pwm_dither_program);
  const uint offset0 = pio_add_program(pio[0], &range_pwm_dither_program);

  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; osc++) {
    PIO p = pio[RANGE_PIO_BLOCK[osc]];
    const uint sm = RANGE_PIO_SM[osc];
    const uint offset = (RANGE_PIO_BLOCK[osc] == 1) ? offset1 : offset0;
    const uint pin = RANGE_PINS[osc];

    pio_sm_claim(p, sm);
    pio_gpio_init(p, pin);
    pio_sm_set_consecutive_pindirs(p, sm, pin, 1, true);

    pio_sm_config c = range_pwm_dither_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, pin);
    pio_sm_init(p, sm, offset, &c);

    range_pio_set_level(osc, 0);
    for (uint32_t i = 0; i < RANGE_PIO_FRAMES; i++) {
      pio_sm_put(p, sm, range_pio_duty[osc][i]);
    }

    range_dma_src_addr[osc] = (uint32_t)(uintptr_t)range_pio_duty[osc];
    range_dma_data[osc] = dma_claim_unused_channel(true);
    range_dma_ctrl[osc] = dma_claim_unused_channel(true);

    dma_channel_config data_c = dma_channel_get_default_config(range_dma_data[osc]);
    channel_config_set_transfer_data_size(&data_c, DMA_SIZE_32);
    channel_config_set_read_increment(&data_c, true);
    channel_config_set_write_increment(&data_c, false);
    channel_config_set_dreq(&data_c, pio_get_dreq(p, sm, true));
    channel_config_set_chain_to(&data_c, (uint)range_dma_ctrl[osc]);
    dma_channel_configure(range_dma_data[osc], &data_c, &p->txf[sm], range_pio_duty[osc],
                          RANGE_PIO_FRAMES, false);

    dma_channel_config ctrl_c = dma_channel_get_default_config(range_dma_ctrl[osc]);
    channel_config_set_transfer_data_size(&ctrl_c, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_c, false);
    channel_config_set_write_increment(&ctrl_c, false);
    dma_channel_configure(range_dma_ctrl[osc], &ctrl_c,
                          &dma_hw->ch[range_dma_data[osc]].al3_read_addr_trig,
                          &range_dma_src_addr[osc], 1, false);

    dma_channel_start(range_dma_ctrl[osc]);
    pio_sm_set_enabled(p, sm, true);
  }
  range_pio_ready = true;
}
#endif  // RANGE0_PIO_DITHER_TEST

// Configure range PWM (per DCO, DIV_COUNTER) and PW PWM (per osc, DIV_COUNTER_PW). Called from setup1().
void init_pwm()
{
  for (int i = 0; i < NUM_OSCILLATORS; i++)
  {
#ifdef RANGE0_PIO_DITHER_TEST
    RANGE_PWM_SLICES[i] = 0xFF;
    RANGE_PWM_CHANNELS[i] = 0;
    continue;
#endif
    gpio_set_function(RANGE_PINS[i], GPIO_FUNC_PWM);
    RANGE_PWM_SLICES[i] = pwm_gpio_to_slice_num(RANGE_PINS[i]);
    RANGE_PWM_CHANNELS[i] = pwm_gpio_to_channel(RANGE_PINS[i]);
    pwm_set_wrap(RANGE_PWM_SLICES[i], DIV_COUNTER);
    pwm_set_enabled(RANGE_PWM_SLICES[i], true);
  }

  for (int i = 0; i < NUM_OSCILLATORS; i++)
  {
    if (PW_PINS[i] == PW_PIN_UNASSIGNED) {
      PW_PWM_SLICES[i] = 0xFF;  // not a real slice — level_pwm share checks must ignore
      continue;
    }
    gpio_set_function(PW_PINS[i], GPIO_FUNC_PWM);
    PW_PWM_SLICES[i] = pwm_gpio_to_slice_num(PW_PINS[i]);
    pwm_set_wrap(PW_PWM_SLICES[i], DIV_COUNTER_PW);
    pwm_set_enabled(PW_PWM_SLICES[i], true);
  }

#ifdef ENABLE_CV_OUTS
  init_cv_pwm();
#endif
}

#ifdef ENABLE_CV_OUTS

// Init cutoff / resonance / VCA PWM (12-bit CV domain). Reso1 shares slice with RANGE OSC2.
void init_cv_pwm() {
  for (int i = 0; i < NUM_FILTERS; i++) {
    gpio_set_function(CUTOFF_PINS[i], GPIO_FUNC_PWM);
    CUTOFF_PWM_SLICES[i] = pwm_gpio_to_slice_num(CUTOFF_PINS[i]);
    CUTOFF_PWM_CHANS[i] = pwm_gpio_to_channel(CUTOFF_PINS[i]);
    // Cut1+Reso0 share slice 2 — both CV, wrap 4095.
    pwm_set_wrap(CUTOFF_PWM_SLICES[i], DIV_COUNTER_CV);
    pwm_set_enabled(CUTOFF_PWM_SLICES[i], true);

    gpio_set_function(RESO_PINS[i], GPIO_FUNC_PWM);
    RESO_PWM_SLICES[i] = pwm_gpio_to_slice_num(RESO_PINS[i]);
    RESO_PWM_CHANS[i] = pwm_gpio_to_channel(RESO_PINS[i]);
    // Reso1 (GP7) shares slice 3 with RANGE OSC2 (GP22): keep DIV_COUNTER wrap from init_pwm.
    if (RESO_PWM_SLICES[i] != RANGE_PWM_SLICES[1]) {
      pwm_set_wrap(RESO_PWM_SLICES[i], DIV_COUNTER_CV);
    }
    pwm_set_enabled(RESO_PWM_SLICES[i], true);
  }

  gpio_set_function(VCA_PIN, GPIO_FUNC_PWM);
  VCA_PWM_SLICE = pwm_gpio_to_slice_num(VCA_PIN);
  VCA_PWM_CHAN = pwm_gpio_to_channel(VCA_PIN);
  pwm_set_wrap(VCA_PWM_SLICE, DIV_COUNTER_CV);
  pwm_set_enabled(VCA_PWM_SLICE, true);

  // Dist Mix shares slice 5 with VCA (wrap already 4095). Drive is alone on slice 4 B.
  // Dual-MCU (ENABLE_VOICE_AUX): RP2040 owns these pins — do not claim them here.
#ifndef ENABLE_VOICE_AUX
  gpio_set_function(DIST_DRIVE_PIN, GPIO_FUNC_PWM);
  DIST_DRIVE_PWM_SLICE = pwm_gpio_to_slice_num(DIST_DRIVE_PIN);
  DIST_DRIVE_PWM_CHAN = pwm_gpio_to_channel(DIST_DRIVE_PIN);
  pwm_set_wrap(DIST_DRIVE_PWM_SLICE, DIV_COUNTER_CV);
  pwm_set_enabled(DIST_DRIVE_PWM_SLICE, true);

  gpio_set_function(DIST_MIX_PIN, GPIO_FUNC_PWM);
  DIST_MIX_PWM_SLICE = pwm_gpio_to_slice_num(DIST_MIX_PIN);
  DIST_MIX_PWM_CHAN = pwm_gpio_to_channel(DIST_MIX_PIN);
  if (DIST_MIX_PWM_SLICE != VCA_PWM_SLICE) {
    pwm_set_wrap(DIST_MIX_PWM_SLICE, DIV_COUNTER_CV);
  }
  pwm_set_enabled(DIST_MIX_PWM_SLICE, true);
#endif

  init_level_pwm();
}

// True if this slice already owns a RANGE or PW wrap that must not become 4095.
static bool level_pwm_slice_shares_voice_wrap(uint8_t slice) {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    if (RANGE_PWM_SLICES[i] == 0xFF) continue;
    if (slice == RANGE_PWM_SLICES[i]) return true;
  }
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    if (PW_PWM_SLICES[i] == 0xFF) continue;
    if (slice == PW_PWM_SLICES[i]) return true;
  }
  return false;
}

static void init_one_level_pwm(uint8_t pin, uint8_t& slice, uint8_t& chan) {
  gpio_set_function(pin, GPIO_FUNC_PWM);
  slice = pwm_gpio_to_slice_num(pin);
  chan = pwm_gpio_to_channel(pin);
  // OSC1 level (GP16) shares slice 0 with RANGE OSC3; OSC2 (GP18) shares slice 1 with PW.
  if (!level_pwm_slice_shares_voice_wrap(slice)) {
    pwm_set_wrap(slice, DIV_COUNTER_CV);
  }
  pwm_set_enabled(slice, true);
}

// Per-slice wrap for shared RANGE/PW domains; 0 = CV wrap (no scale). Filled in init_level_pwm.
static uint16_t level_wrap_for_slice[8] = { 0 };

static inline uint16_t scale_level_cv_to_wrap(uint16_t level_cv, uint8_t slice) {
  const uint16_t wrap = level_wrap_for_slice[slice & 7];
  if (wrap) {
    // ÷4096 via >>12 (same as lerp_0_4095); avoids hot /4095 on M0+.
    return (uint16_t)(((uint32_t)level_cv * (uint32_t)wrap) >> 12);
  }
  return level_cv;
}

void init_level_pwm() {
  for (int s = 0; s < 8; s++) {
    level_wrap_for_slice[s] = 0;
  }
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    if (RANGE_PWM_SLICES[i] != 0xFF) {
      level_wrap_for_slice[RANGE_PWM_SLICES[i] & 7] = DIV_COUNTER;
    }
    if (PW_PWM_SLICES[i] != 0xFF) {
      level_wrap_for_slice[PW_PWM_SLICES[i] & 7] = DIV_COUNTER_PW;
    }
  }

  init_one_level_pwm(OSC1_LEVEL_PIN, OSC1_LEVEL_PWM_SLICE, OSC1_LEVEL_PWM_CHAN);
  init_one_level_pwm(OSC2_LEVEL_PIN, OSC2_LEVEL_PWM_SLICE, OSC2_LEVEL_PWM_CHAN);
  init_one_level_pwm(OSC3_LEVEL_PIN, OSC3_LEVEL_PWM_SLICE, OSC3_LEVEL_PWM_CHAN);
  init_one_level_pwm(SUB_LEVEL_PIN, SUB_LEVEL_PWM_SLICE, SUB_LEVEL_PWM_CHAN);
  write_level_pwm();
}

void write_level_pwm_raw(uint16_t osc1, uint16_t osc2, uint16_t osc3, uint16_t sub) {
  pwm_set_chan_level(OSC1_LEVEL_PWM_SLICE, OSC1_LEVEL_PWM_CHAN,
                     scale_level_cv_to_wrap(osc1, OSC1_LEVEL_PWM_SLICE));
  pwm_set_chan_level(OSC2_LEVEL_PWM_SLICE, OSC2_LEVEL_PWM_CHAN,
                     scale_level_cv_to_wrap(osc2, OSC2_LEVEL_PWM_SLICE));
  pwm_set_chan_level(OSC3_LEVEL_PWM_SLICE, OSC3_LEVEL_PWM_CHAN,
                     scale_level_cv_to_wrap(osc3, OSC3_LEVEL_PWM_SLICE));
  pwm_set_chan_level(SUB_LEVEL_PWM_SLICE, SUB_LEVEL_PWM_CHAN,
                     scale_level_cv_to_wrap(sub, SUB_LEVEL_PWM_SLICE));
}

void write_level_pwm() {
  write_level_pwm_raw(OSC1Level, OSC2Level, OSC3Level, SubLevel);
}

// Push raw compare values to the cutoff / per-filter resonance / VCA / dist slices.
void write_cv_pwm_raw(uint16_t cutoff, const uint16_t resonance[NUM_FILTERS], uint16_t vca,
                      uint16_t dist_drive, uint16_t dist_mix) {
  for (int i = 0; i < NUM_FILTERS; i++) {
    pwm_set_chan_level(CUTOFF_PWM_SLICES[i], CUTOFF_PWM_CHANS[i], cutoff);

    uint16_t reso_level = resonance[i];
    if (RESO_PWM_SLICES[i] == RANGE_PWM_SLICES[1]) {
      // Shared wrap DIV_COUNTER with RANGE OSC2 — scale 0..4095 → 0..DIV_COUNTER via >>12.
      reso_level = (uint16_t)(((uint32_t)resonance[i] * (uint32_t)DIV_COUNTER) >> 12);
    }
    pwm_set_chan_level(RESO_PWM_SLICES[i], RESO_PWM_CHANS[i], reso_level);
  }
  pwm_set_chan_level(VCA_PWM_SLICE, VCA_PWM_CHAN, vca);
#ifndef ENABLE_VOICE_AUX
  pwm_set_chan_level(DIST_DRIVE_PWM_SLICE, DIST_DRIVE_PWM_CHAN, dist_drive);
  pwm_set_chan_level(DIST_MIX_PWM_SLICE, DIST_MIX_PWM_CHAN, dist_mix);
#else
  (void)dist_drive;
  (void)dist_mix;
#endif
}

// Push soft VCF/VCA/reso/dist levels to Pico PWM compares (panel Dist Drive; matrix may override).
void write_cv_pwm() {
  write_cv_pwm_raw(VCF_PWM[0], RESONANCE_PWM, VCA_PWM[0], DIST_DRIVE, DIST_MIX);
}

#endif  // ENABLE_CV_OUTS

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Serial.ino"
// Serial1 = MIDI DIN @ 31250; Serial2 = Input hub @ 2.5M (inner panel protocol).
// Screen has no DCO port: gap 'x' rides the Input link and Input relays it.

// Input / USB share one LUT. Tag the drain so USB 'p'/'a'-'d' can mirror to
// Input without echoing the panel's own stream back onto the Input link.
enum ParamIngress : uint8_t {
  PARAM_SRC_INPUT = 0,
  PARAM_SRC_USB   = 1,
};
static ParamIngress g_param_ingress = PARAM_SRC_INPUT;

static void serial_forward_input_block_to_mb(char cmd, const uint8_t* payload, uint8_t len) {
  if (g_param_ingress != PARAM_SRC_USB) return;
  if (!serial2_dma_tx_ready()) return;
  serial_frame_write(Serial2Dma, (uint8_t)cmd, payload, len);
}

static void serial_send_adsr_block_to_mb(uint8_t cmd, uint16_t a, uint16_t d, uint16_t s, uint16_t r) {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[INPUT_SERIAL_LEN_ADSR_BLOCK];
  encode_u16_le(payload + 0, a);
  encode_u16_le(payload + 2, d);
  encode_u16_le(payload + 4, s);
  encode_u16_le(payload + 6, r);
  serial_frame_write(Serial2Dma, cmd, payload, INPUT_SERIAL_LEN_ADSR_BLOCK);
}

void serial_send_adsr_vca_block_to_mb() {
  serial_send_adsr_block_to_mb(
    INPUT_CMD_ADSR1_BLOCK,
    ADSR_VCA_attack, ADSR_VCA_decay, ADSR_VCA_sustain, ADSR_VCA_release);
}

void serial_send_adsr_vcf_block_to_mb() {
  serial_send_adsr_block_to_mb(
    INPUT_CMD_ADSR2_BLOCK,
    ADSR_VCF_attack, ADSR_VCF_decay, ADSR_VCF_sustain, ADSR_VCF_release);
}

void serial_send_adsr_dco_block_to_mb() {
  serial_send_adsr_block_to_mb(
    INPUT_CMD_ADSR3_BLOCK,
    ADSR1_attack, ADSR1_decay, ADSR1_sustain, ADSR1_release);
}

void serial_send_filter_block_to_mb() {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[INPUT_SERIAL_LEN_FILTER_BLOCK];
  encode_u16_le(payload + 0, CUTOFF);
  encode_u16_le(payload + 2, RESONANCE);
  encode_u16_le(payload + 4, (uint16_t)ADSR2toVCF);
  encode_u16_le(payload + 6, LFO2toVCF);
  serial_frame_write(Serial2Dma, INPUT_CMD_FILTER_BLOCK, payload, INPUT_SERIAL_LEN_FILTER_BLOCK);
}

void serial_send_preset_loaded_to_mb(uint8_t slot) {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[INPUT_SERIAL_LEN_PRESET_LOADED] = { slot };
  serial_frame_write(Serial2Dma, INPUT_CMD_PRESET_LOADED, payload, INPUT_SERIAL_LEN_PRESET_LOADED);
}

static void input_handle_adsr1(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCA_attack)  { ADSR_VCA_attack  = v; dirty |= ADSR_DIRTY_VCA_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCA_decay)   { ADSR_VCA_decay   = v; dirty |= ADSR_DIRTY_VCA_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCA_sustain) { ADSR_VCA_sustain = v; dirty |= ADSR_DIRTY_VCA_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCA_release) { ADSR_VCA_release = v; dirty |= ADSR_DIRTY_VCA_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb('a', payload, len);
}

static void input_handle_adsr2(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCF_attack)  { ADSR_VCF_attack  = v; dirty |= ADSR_DIRTY_VCF_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCF_decay)   { ADSR_VCF_decay   = v; dirty |= ADSR_DIRTY_VCF_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCF_sustain) { ADSR_VCF_sustain = v; dirty |= ADSR_DIRTY_VCF_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCF_release) { ADSR_VCF_release = v; dirty |= ADSR_DIRTY_VCF_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb('b', payload, len);
}

// EnvDCO times ('c') → existing ADSR1_* engine (pitch/PW)
static void input_handle_adsr3(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR1_attack)  { ADSR1_attack  = v; dirty |= ADSR_DIRTY_DCO_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR1_decay)   { ADSR1_decay   = v; dirty |= ADSR_DIRTY_DCO_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR1_sustain) { ADSR1_sustain = v; dirty |= ADSR_DIRTY_DCO_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR1_release) { ADSR1_release = v; dirty |= ADSR_DIRTY_DCO_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb(INPUT_CMD_ADSR3_BLOCK, payload, len);
}

static void input_handle_filter_block(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_FILTER_BLOCK) return;
  CUTOFF     = decode_u16_le(payload + 0);
  RESONANCE  = decode_u16_le(payload + 2);
  ADSR2toVCF = decode_i16_le(payload + 4);
  LFO2toVCF  = decode_u16_le(payload + 6);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
  serial_forward_input_block_to_mb(INPUT_CMD_FILTER_BLOCK, payload, len);
}

// The persistable patch-param set lives in preset_store.h now
// (preset_param_is_persistable), shared with the preset shadow capture.

void serialSendParam16(byte paramNumber, int16_t paramValue, bool force) {
  if (!force && !serial2_dma_tx_ready()) {
    return;
  }
  while (force && !serial2_dma_tx_ready()) {
    tight_loop_contents();
  }
  uint8_t payload[INPUT_SERIAL_LEN_PARAM_16];
  encode_param_p(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2Dma, INPUT_CMD_PARAM_16, payload, INPUT_SERIAL_LEN_PARAM_16);
}

void serial_echo_persistable_param16(uint8_t id, int16_t value) {
  if (!preset_param_is_persistable(id)) {
    return;
  }
  serialSendParam16(id, value);
}

static void input_handle_param16(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_PARAM_16) return;
  ParamFrame frame;
  decode_param_p(payload, frame);
  update_parameters(frame.id, (int16_t)frame.value);
  if (g_param_ingress != PARAM_SRC_INPUT) {
    serial_echo_persistable_param16(frame.id, (int16_t)frame.value);
  }
}

static void input_handle_preset_name(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_PRESET_NAME) return;
  for (int i = 0; i < 16; ++i) {
    presetName[i] = payload[i];
  }
}

// Bulk restore staging ('B') and commit ('C') → preset_store.ino.
static void input_handle_bulk_chunk(char, const uint8_t* payload, uint8_t len) {
  preset_bulk_chunk(payload, len);
}

static void input_handle_bulk_commit(char, const uint8_t* payload, uint8_t len) {
  preset_bulk_commit(payload, len);
}

// 'N': Input asking for the whole preset directory → preset_store.ino.
static void input_handle_preset_dir_request(char, const uint8_t*, uint8_t) {
  preset_store_send_directory_to_mb();
}

static const SerialCommandDef inputSerialCommands[] = {
  { INPUT_CMD_ADSR1_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr1        },
  { INPUT_CMD_ADSR2_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr2        },
  { INPUT_CMD_ADSR3_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr3        },
  { INPUT_CMD_FILTER_BLOCK,  INPUT_SERIAL_LEN_FILTER_BLOCK, input_handle_filter_block },
  { INPUT_CMD_PARAM_16,      INPUT_SERIAL_LEN_PARAM_16,     input_handle_param16      },
  { INPUT_CMD_PRESET_NAME,   INPUT_SERIAL_LEN_PRESET_NAME,  input_handle_preset_name  },
  { INPUT_CMD_BULK_CHUNK,    INPUT_SERIAL_LEN_BULK_CHUNK,   input_handle_bulk_chunk   },
  { INPUT_CMD_BULK_COMMIT,   INPUT_SERIAL_LEN_BULK_COMMIT,  input_handle_bulk_commit  },
  { INPUT_CMD_PRESET_DIR_REQUEST, INPUT_SERIAL_LEN_PRESET_DIR_REQUEST, input_handle_preset_dir_request },
};

static SerialCommandTable inputSerialLut;
static SerialParserContext inputSerialParser = {};

void init_serial() {
  Serial1.setFIFOSize(256);
  Serial1.setPollingMode(false);
  Serial1.setRX(1);
  Serial1.setTX(0);
  Serial1.begin(31250);

  Serial2.setFIFOSize(512);
  Serial2.setPollingMode(false);
  Serial2.setRX(21);
  Serial2.setTX(20);
  Serial2.begin(2500000);
  serial2_dma_init();

  serial_command_table_init(
    inputSerialLut,
    inputSerialCommands,
    sizeof(inputSerialCommands) / sizeof(inputSerialCommands[0])
  );
}

// USB composite: CDC serial + MIDI. Descriptors first, then detach/attach so the
// host re-enumerates after flash/soft reset (avoids missing /dev/ttyACM* on Linux).
void init_usb() {
  USBDevice.setManufacturerDescriptor("FELA         ");
  USBDevice.setProductDescriptor("DCO3-MONO   ");

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  Serial.begin(2000000);
  usb_midi.setStringDescriptor("DCO3-MONO MIDI");
  MIDI_USB.begin(MIDI_CHANNEL_OMNI);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void __not_in_flash_func(serial_panel_task)() {
  serial2_dma_poll();
  g_param_ingress = PARAM_SRC_INPUT;
  serial_parser_drain(
    inputSerialParser,
    inputSerialLut,
    Serial2,
    SERIAL_DRAIN_BYTE_BUDGET
  );
}

// USB CDC bench link: same inner frames as Serial2. Only host → DCO is framed;
// DCO → host stays plain debug text.
#ifdef ENABLE_USB_CONTROL

static SerialParserContext usbSerialParser = {};

void __not_in_flash_func(serial_usb_task)() {
  if (!Serial) return;
  g_param_ingress = PARAM_SRC_USB;
  serial_parser_drain(
    usbSerialParser,
    inputSerialLut,
    Serial,
    SERIAL_DRAIN_BYTE_BUDGET
  );
}

#endif  // ENABLE_USB_CONTROL

// TX slim 'x' to Input: gap (154) and cal offsets (155). Input relays 154 on to Screen.
// Drop the frame if Serial2 TX is not ready (USB-only bench with no Input board).
// Persistable USB/MIDI 'p' echo is serialSendParam16 / serial_echo_persistable_param16.
void serialSendParam32(byte paramNumber, uint32_t paramValue, bool force) {
  if (!force && !serial2_dma_tx_ready()) {
    return;
  }
  // force = the live GAP readout during calibration: wait for the DMA rather
  // than dropping the frame, so the Screen keeps updating while trimming.
  while (force && !serial2_dma_tx_ready()) {
    tight_loop_contents();
  }
  uint8_t payload[INPUT_SERIAL_LEN_PARAM_32];
  encode_param32(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2Dma, INPUT_CMD_PARAM_32, payload, INPUT_SERIAL_LEN_PARAM_32);
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Timer_micros.ino"
// Seed last-fire stamps for both cores. Periods live as constexpr in Timer_micros.h.
void init_micros_timers() {
  const unsigned long now = micros();

  timer50micros = now;  timer50microsFlag = 0;
  timer51micros = now;  timer51microsFlag = 0;
  timer99micros = now;  timer99microsFlag = 0;
  // timer100micros = now;  timer100microsFlag = 0;
  timer1ms = now;        timer1msFlag = 0;
  // timer223micros = now;  timer223microsFlag = 0;
  // timer5ms = now;        timer5msFlag = 0;
  // timer11ms = now;       timer11msFlag = 0;
  // timer23ms = now;       timer23msFlag = 0;
  // timer31ms = now;       timer31msFlag = 0;
  // timer67ms = now;       timer67msFlag = 0;
  // timer200ms = now;      timer200msFlag = 0;
  // timer500ms = now;      timer500msFlag = 0;
  // timer1000ms = now;     timer1000msFlag = 0;

  timer50micros2 = now;  timer50microsFlag2 = 0;
  timer51micros2 = now;  timer51microsFlag2 = 0;
  timer99micros2 = now;  timer99microsFlag2 = 0;
  // timer100micros2 = now;  timer100microsFlag2 = 0;
  // timer223micros2 = now;  timer223microsFlag2 = 0;
  //timer1ms2 = now;         timer1msFlag2 = 0;
  // timer5ms2 = now;        timer5msFlag2 = 0;
  // timer11ms2 = now;       timer11msFlag2 = 0;
  // timer23ms2 = now;       timer23msFlag2 = 0;
  // timer31ms2 = now;       timer31msFlag2 = 0;
  // timer67ms2 = now;       timer67msFlag2 = 0;
  // timer200ms2 = now;      timer200msFlag2 = 0;
  // timer500ms2 = now;      timer500msFlag2 = 0;
  // timer1000ms2 = now;     timer1000msFlag2 = 0;
}

// Core 0 — unrolled µs timers. Called every loop().
void __not_in_flash_func(microsTimer)() {
  const unsigned long now = micros();

  timer50microsFlag = 0;  if (now - timer50micros > kTimer50us) { timer50micros = now; timer50microsFlag = 1; }
  timer51microsFlag = 0;  if (now - timer51micros > kTimer51us) { timer51micros = now; timer51microsFlag = 1; }
  timer99microsFlag = 0;  if (now - timer99micros > kTimer99us) { timer99micros = now; timer99microsFlag = 1; }
  // timer100microsFlag = 0;  if (now - timer100micros > kTimer100us) { timer100micros = now; timer100microsFlag = 1; }
  timer1msFlag = 0;        if (now - timer1ms > kTimer1ms)         { timer1ms = now;       timer1msFlag = 1; }
  // timer223microsFlag = 0;  if (now - timer223micros > kTimer223us) { timer223micros = now; timer223microsFlag = 1; }
  // timer5msFlag = 0;        if (now - timer5ms > kTimer5ms)         { timer5ms = now;       timer5msFlag = 1; }
  // timer11msFlag = 0;       if (now - timer11ms > kTimer11ms)       { timer11ms = now;      timer11msFlag = 1; }
  // timer23msFlag = 0;       if (now - timer23ms > kTimer23ms)       { timer23ms = now;      timer23msFlag = 1; }
  // timer31msFlag = 0;       if (now - timer31ms > kTimer31ms)       { timer31ms = now;      timer31msFlag = 1; }
  // timer67msFlag = 0;       if (now - timer67ms > kTimer67ms)       { timer67ms = now;      timer67msFlag = 1; }
  // timer200msFlag = 0;      if (now - timer200ms > kTimer200ms)     { timer200ms = now;     timer200msFlag = 1; }
  // timer500msFlag = 0;      if (now - timer500ms > kTimer500ms)     { timer500ms = now;     timer500msFlag = 1; }
  // timer1000msFlag = 0;     if (now - timer1000ms > kTimer1000ms)   { timer1000ms = now;    timer1000msFlag = 1; }
}

// Core 1 — same periods, separate state (*2). Called every loop1().
void __not_in_flash_func(microsTimer2)() {
  const unsigned long now = micros();

  //timer50microsFlag2 = 0;  if (now - timer50micros2 > kTimer50us) { timer50micros2 = now; timer50microsFlag2 = 1; }
  //timer51microsFlag2 = 0;  if (now - timer51micros2 > kTimer51us) { timer51micros2 = now; timer51microsFlag2 = 1; }
  timer99microsFlag2 = 0;  if (now - timer99micros2 > kTimer99us) { timer99micros2 = now; timer99microsFlag2 = 1; }
  // timer100microsFlag2 = 0;  if (now - timer100micros2 > kTimer100us) { timer100micros2 = now; timer100microsFlag2 = 1; }
  timer1msFlag2 = 0;         if (now - timer1ms2 > kTimer1ms)         { timer1ms2 = now;       timer1msFlag2 = 1; }
  // timer223microsFlag2 = 0;  if (now - timer223micros2 > kTimer223us) { timer223micros2 = now; timer223microsFlag2 = 1; }
  // timer5msFlag2 = 0;        if (now - timer5ms2 > kTimer5ms)         { timer5ms2 = now;       timer5msFlag2 = 1; }
  // timer11msFlag2 = 0;       if (now - timer11ms2 > kTimer11ms)       { timer11ms2 = now;      timer11msFlag2 = 1; }
  // timer23msFlag2 = 0;       if (now - timer23ms2 > kTimer23ms)       { timer23ms2 = now;      timer23msFlag2 = 1; }
  // timer31msFlag2 = 0;       if (now - timer31ms2 > kTimer31ms)       { timer31ms2 = now;      timer31msFlag2 = 1; }
  // timer67msFlag2 = 0;       if (now - timer67ms2 > kTimer67ms)       { timer67ms2 = now;      timer67msFlag2 = 1; }
  // timer200msFlag2 = 0;      if (now - timer200ms2 > kTimer200ms)     { timer200ms2 = now;     timer200msFlag2 = 1; }
  // timer500msFlag2 = 0;      if (now - timer500ms2 > kTimer500ms)     { timer500ms2 = now;     timer500msFlag2 = 1; }
  // timer1000msFlag2 = 0;     if (now - timer1000ms2 > kTimer1000ms)   { timer1000ms2 = now;    timer1000msFlag2 = 1; }
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/adsr.ino"
// Panel sustain (MIDI/CC domain) → library setSustain units.
// NATIVE_Q15=1: 0..ADSR_Q15_PEAK. NATIVE_Q15=0: DAC counts (panel_full == peak).
static inline int adsr_sustain_for_set(uint16_t panel, uint16_t panel_full) {
#if ADSR_BEZIER_NATIVE_Q15
  if (panel_full == 0) return 0;
  // CV panel scale 4096 → (panel * ADSR_Q15_PEAK) >> 12 (no divide).
  if (panel_full == ADSR_CV_SCALE)
    return (int)(((uint32_t)panel * (uint32_t)ADSR_Q15_PEAK) >> 12);
  return (int)(((uint32_t)panel * (uint32_t)ADSR_Q15_PEAK) / (uint32_t)panel_full);
#else
  (void)panel_full;
  return (int)panel;
#endif
}

// Boot: build Bézier tables and apply initial A/D/S/R to EnvDCO + EnvVCA + EnvVCF.
void init_ADSR() {
#if ADSR_BEZIER_NATIVE_Q15
  adsrBezierInitTables((float)ADSR_Q15_PEAK, ARRAY_SIZE, _curve_tables);
#else
  adsrBezierInitTables(ADSR_1_CC, ARRAY_SIZE, _curve_tables);
#endif

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    ADSRVoices[i].adsr1_voice.setAttack(ADSR1_attack);
    ADSRVoices[i].adsr1_voice.setDecay(ADSR1_decay);
    ADSRVoices[i].adsr1_voice.setSustain(adsr_sustain_for_set(ADSR1_sustain, ADSR_CV_SCALE));
    ADSRVoices[i].adsr1_voice.setRelease(ADSR1_release);
    ADSRVoices[i].adsr1_voice.setResetAttack(ADSRRestart);

    ADSRVoices[i].adsr_vca_voice.setAttack(ADSR_VCA_attack);
    ADSRVoices[i].adsr_vca_voice.setDecay(ADSR_VCA_decay);
    ADSRVoices[i].adsr_vca_voice.setSustain(adsr_sustain_for_set(ADSR_VCA_sustain, ADSR_CV_SCALE));
    ADSRVoices[i].adsr_vca_voice.setRelease(ADSR_VCA_release);
    ADSRVoices[i].adsr_vca_voice.setResetAttack(VCAADSRRestart);
  }

  adsr_vcf_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf_voice.setSustain(adsr_sustain_for_set(ADSR_VCF_sustain, ADSR_CV_SCALE));
  adsr_vcf_voice.setRelease(ADSR_VCF_release);
  adsr_vcf_voice.setResetAttack(VCFADSRRestart);

  adsr_vcf2_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf2_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf2_voice.setSustain(adsr_sustain_for_set(ADSR_VCF_sustain, ADSR_CV_SCALE));
  adsr_vcf2_voice.setRelease(ADSR_VCF_release);
  adsr_vcf2_voice.setResetAttack(VCFADSRRestart);
}

// ~10 kHz: note edges → EnvDCO + EnvVCA + EnvVCF; sample levels.
// Each noteOn/noteOff/getWave reads micros()/millis() itself — do not share one
// timestamp across edges + getWave (unsigned delta underflow skips A/R).
void __not_in_flash_func(ADSR_update)() {
  for (int i = 0; i < NUM_VOICES; i++) {
    if (noteEnd[i] == 1) {
      ADSRVoices[i].adsr1_voice.noteOff();
      ADSRVoices[i].adsr_vca_voice.noteOff();
      adsr_vcf_voice.noteOff();
      adsr_vcf2_voice.noteOff();
      noteEnd[i] = 0;
    } else if (noteStart[i] == 1) {
      // A/D/R (and sustain) stay current via ADSR_set_parameters / init / curve helpers.
      // Re-set* here used to cost ~9x 64-bit divides per note and spiked ADSR_update max.
      // noteOn() retriggers attack; do not noteOff() first (phantom RELEASE from _notes_pressed==0).
      ADSRVoices[i].adsr1_voice.noteOn();
      ADSRVoices[i].adsr_vca_voice.noteOn();
      adsr_vcf_voice.noteOn();
      adsr_vcf2_voice.noteOn();

      noteStart[i] = 0;
    }
#if ADSR_BEZIER_NATIVE_Q15
    // getWave returns Q15; do not call levelDac() here (divide; u12 mirrors unused).
    ADSR1Level_q15[i] = (int16_t)ADSRVoices[i].adsr1_voice.getWave();
    ADSR_VCA_Level_q15[i] = (int16_t)ADSRVoices[i].adsr_vca_voice.getWave();
#else
    ADSR1Level[i] = ADSRVoices[i].adsr1_voice.getWave();
    ADSR1Level_q15[i] = ADSRVoices[i].adsr1_voice.levelQ15();
    ADSR_VCA_Level[i] = ADSRVoices[i].adsr_vca_voice.getWave();
    ADSR_VCA_Level_q15[i] = ADSRVoices[i].adsr_vca_voice.levelQ15();
#endif
  }
  // Shared EnvVCF / EnvVCF2: once per tick (not inside the voice loop).
#if ADSR_BEZIER_NATIVE_Q15
  ADSR_VCF_Level_q15 = (int16_t)adsr_vcf_voice.getWave();
  ADSR_VCF2_Level_q15 = (int16_t)adsr_vcf2_voice.getWave();
#else
  ADSR_VCF_Level = adsr_vcf_voice.getWave();
  ADSR_VCF_Level_q15 = adsr_vcf_voice.levelQ15();
  ADSR_VCF2_Level = adsr_vcf2_voice.getWave();
  ADSR_VCF2_Level_q15 = adsr_vcf2_voice.levelQ15();
#endif

  ADSR_set_parameters();
}

// ~200 Hz: push dirty EnvDCO / EnvVCA / EnvVCF A/D/S/R to all voices.
inline void ADSR_set_parameters() {
  static uint8_t tick = 0;
  if (++tick < 50) return;
  tick = 0;

  uint16_t ch = adsr_params_dirty;
  if (!ch) return;
  adsr_params_dirty = 0;

  if (ch & ADSR_DIRTY_DCO_A) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr1_voice.setAttack(ADSR1_attack);
  }
  if (ch & ADSR_DIRTY_DCO_D) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr1_voice.setDecay(ADSR1_decay);
  }
  if (ch & ADSR_DIRTY_DCO_S) {
    const int s = adsr_sustain_for_set(ADSR1_sustain, ADSR_CV_SCALE);
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr1_voice.setSustain(s);
  }
  if (ch & ADSR_DIRTY_DCO_R) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr1_voice.setRelease(ADSR1_release);
  }

  if (ch & ADSR_DIRTY_VCA_A) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr_vca_voice.setAttack(ADSR_VCA_attack);
  }
  if (ch & ADSR_DIRTY_VCA_D) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr_vca_voice.setDecay(ADSR_VCA_decay);
  }
  if (ch & ADSR_DIRTY_VCA_S) {
    const int s = adsr_sustain_for_set(ADSR_VCA_sustain, ADSR_CV_SCALE);
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr_vca_voice.setSustain(s);
  }
  if (ch & ADSR_DIRTY_VCA_R) {
    for (int i = 0; i < NUM_VOICES; i++)
      ADSRVoices[i].adsr_vca_voice.setRelease(ADSR_VCA_release);
  }

  if (ch & ADSR_DIRTY_VCF_A) {
    adsr_vcf_voice.setAttack(ADSR_VCF_attack);
    adsr_vcf2_voice.setAttack(ADSR_VCF_attack);
  }
  if (ch & ADSR_DIRTY_VCF_D) {
    adsr_vcf_voice.setDecay(ADSR_VCF_decay);
    adsr_vcf2_voice.setDecay(ADSR_VCF_decay);
  }
  if (ch & ADSR_DIRTY_VCF_S) {
    const int s = adsr_sustain_for_set(ADSR_VCF_sustain, ADSR_CV_SCALE);
    adsr_vcf_voice.setSustain(s);
    adsr_vcf2_voice.setSustain(s);
  }
  if (ch & ADSR_DIRTY_VCF_R) {
    adsr_vcf_voice.setRelease(ADSR_VCF_release);
    adsr_vcf2_voice.setRelease(ADSR_VCF_release);
  }
}

void ADSR1_set_restart() {
  for (int i = 0; i < NUM_VOICES; i++) {
    ADSRVoices[i].adsr1_voice.setResetAttack(ADSRRestart);
  }
}

void ADSR_VCA_set_restart() {
  for (int i = 0; i < NUM_VOICES; i++) {
    ADSRVoices[i].adsr_vca_voice.setResetAttack(VCAADSRRestart);
  }
}

void ADSR_VCF_set_restart() {
  adsr_vcf_voice.setResetAttack(VCFADSRRestart);
  adsr_vcf2_voice.setResetAttack(VCFADSRRestart);
}

// EnvVCA attack curve → engine; timing params must be re-applied after a curve change.
void ADSR_VCA_change_attack_curve(uint8_t adsrCurveAttack) {
  const int s = adsr_sustain_for_set(ADSR_VCA_sustain, ADSR_CV_SCALE);
  for (int i = 0; i < NUM_VOICES; i++) {
    ADSRVoices[i].adsr_vca_voice.adsrCurveAttack(adsrCurveAttack);
    ADSRVoices[i].adsr_vca_voice.setAttack(ADSR_VCA_attack);
    ADSRVoices[i].adsr_vca_voice.setDecay(ADSR_VCA_decay);
    ADSRVoices[i].adsr_vca_voice.setSustain(s);
    ADSRVoices[i].adsr_vca_voice.setRelease(ADSR_VCA_release);
    ADSRVoices[i].adsr_vca_voice.setResetAttack(VCAADSRRestart);
  }
}

// EnvVCA decay curve → engine.
void ADSR_VCA_change_decay_curve(uint8_t adsrCurveDecay) {
  const int s = adsr_sustain_for_set(ADSR_VCA_sustain, ADSR_CV_SCALE);
  for (int i = 0; i < NUM_VOICES; i++) {
    ADSRVoices[i].adsr_vca_voice.adsrCurveDecay(adsrCurveDecay);
    ADSRVoices[i].adsr_vca_voice.setAttack(ADSR_VCA_attack);
    ADSRVoices[i].adsr_vca_voice.setDecay(ADSR_VCA_decay);
    ADSRVoices[i].adsr_vca_voice.setSustain(s);
    ADSRVoices[i].adsr_vca_voice.setRelease(ADSR_VCA_release);
    ADSRVoices[i].adsr_vca_voice.setResetAttack(VCAADSRRestart);
  }
}

// EnvVCF attack curve → engine.
void ADSR_VCF_change_attack_curve(uint8_t adsrCurveAttack) {
  const int s = adsr_sustain_for_set(ADSR_VCF_sustain, ADSR_CV_SCALE);
  adsr_vcf_voice.adsrCurveAttack(adsrCurveAttack);
  adsr_vcf_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf_voice.setSustain(s);
  adsr_vcf_voice.setRelease(ADSR_VCF_release);
  adsr_vcf_voice.setResetAttack(VCFADSRRestart);

  adsr_vcf2_voice.adsrCurveAttack(adsrCurveAttack);
  adsr_vcf2_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf2_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf2_voice.setSustain(s);
  adsr_vcf2_voice.setRelease(ADSR_VCF_release);
  adsr_vcf2_voice.setResetAttack(VCFADSRRestart);
}

// EnvVCF decay curve → engine.
void ADSR_VCF_change_decay_curve(uint8_t adsrCurveDecay) {
  const int s = adsr_sustain_for_set(ADSR_VCF_sustain, ADSR_CV_SCALE);
  adsr_vcf_voice.adsrCurveDecay(adsrCurveDecay);
  adsr_vcf_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf_voice.setSustain(s);
  adsr_vcf_voice.setRelease(ADSR_VCF_release);
  adsr_vcf_voice.setResetAttack(VCFADSRRestart);

  adsr_vcf2_voice.adsrCurveDecay(adsrCurveDecay);
  adsr_vcf2_voice.setAttack(ADSR_VCF_attack);
  adsr_vcf2_voice.setDecay(ADSR_VCF_decay);
  adsr_vcf2_voice.setSustain(s);
  adsr_vcf2_voice.setRelease(ADSR_VCF_release);
  adsr_vcf2_voice.setResetAttack(VCFADSRRestart);
}

void ADSR1_change_curves() {
  const int s = adsr_sustain_for_set(ADSR1_sustain, ADSR_CV_SCALE);
  for (int i = 0; i < NUM_VOICES; i++) {
    ADSRVoices[i].adsr1_voice.setAttack(ADSR1_attack);
    ADSRVoices[i].adsr1_voice.setDecay(ADSR1_decay);
    ADSRVoices[i].adsr1_voice.setSustain(s);
    ADSRVoices[i].adsr1_voice.setRelease(ADSR1_release);
    ADSRVoices[i].adsr1_voice.setResetAttack(ADSRRestart);
  }
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/amp_comp_bench.ino"
#include "include_all.h"

// Amp-comp speed / accuracy benches. Needs AMP_COMP_BENCHMARK + RUNNING_AVERAGE.
// Results go through bench_out_* and paced Core 0 TX (never Serial from Core 1).
// Cmds 24/25 install a linear synthetic cal (not LittleFS), measure, then restore.

#if defined(AMP_COMP_BENCHMARK) && defined(RUNNING_AVERAGE) && defined(USE_FLOAT_AMP_COMP)

volatile bool amp_comp_bench_speed_pending = false;
volatile bool amp_comp_bench_accuracy_pending = false;

static constexpr int AMP_COMP_BENCH_METHODS = 3; // FLOAT_QUAD, LUT, FIXED
// One osc is enough to rank methods (synthetic cal is identical per osc). Cuts wall time ~3×.
static constexpr int AMP_COMP_BENCH_OSCS = 1;
// Shared centi-Hz grid with accuracy: 1.00 … AMP_COMP_MAX_HZ inclusive.
static constexpr int32_t AMP_COMP_BENCH_CHZ_MIN = 100;

// Snapshot of live FS-derived breakpoints so benches can restore after synth.
static float   s_liveFreqHz[NUM_OSCILLATORS][ampCompTableSize + 1];
static int32_t s_liveAmp[NUM_OSCILLATORS][ampCompTableSize + 1];
static bool    s_liveSnapValid = false;

static void amp_comp_bench_snapshot_live() {
  memcpy(s_liveFreqHz, ampCompFrequencyHz, sizeof(s_liveFreqHz));
  memcpy(s_liveAmp, ampCompArray, sizeof(s_liveAmp));
  s_liveSnapValid = true;
}

static void amp_comp_bench_restore_live() {
  if (!s_liveSnapValid) return;
  memcpy(ampCompFrequencyHz, s_liveFreqHz, sizeof(s_liveFreqHz));
  memcpy(ampCompArray, s_liveAmp, sizeof(s_liveAmp));
  precompute_amp_comp_for_engine();
}

// Linear fake cal: Hz 1 … AMP_COMP_MAX_HZ, levels 1 … DIV_COUNTER (same for all oscs).
static void amp_comp_bench_install_synthetic_linear() {
  const int n = ampCompTableSize; // 22 breakpoints; slot n is sentinel from precompute
  for (uint8_t o = 0; o < NUM_OSCILLATORS; ++o) {
    for (int i = 0; i < n; ++i) {
      // Inclusive endpoints: i=0 → 1 Hz / level 1; i=n-1 → MAX_HZ / DIV_COUNTER
      float t = (n <= 1) ? 0.0f : (float)i / (float)(n - 1);
      float hz = 1.0f + t * ((float)AMP_COMP_MAX_HZ - 1.0f);
      float lvl = 1.0f + t * ((float)DIV_COUNTER - 1.0f);
      ampCompFrequencyHz[o][i] = hz;
      ampCompArray[o][i] = (int32_t)lroundf(lvl);
    }
    ampCompFrequencyHz[o][n] = (float)AMP_COMP_MAX_HZ;
    ampCompArray[o][n] = (int32_t)DIV_COUNTER;
  }
  precompute_amp_comp_for_engine();
}

static void amp_comp_bench_with_synthetic(void (*fn)()) {
  amp_comp_bench_snapshot_live();
  amp_comp_bench_install_synthetic_linear();
  fn();
  amp_comp_bench_restore_live();
}

static void amp_comp_bench_reset_win_cache() {
  for (int i = 0; i < NUM_OSCILLATORS; ++i) ampWinCache[i] = -1;
}

static void amp_comp_bench_run_speed_body() {
  // Same 0.01 Hz grid as accuracy (one pass, AMP_COMP_BENCH_OSCS). Heavy one-shot.
  const int32_t cHzMax = AMP_COMP_MAX_HZ * 100;
  const uint8_t saved_method = amp_comp_method;

  uint32_t totalCalls[AMP_COMP_BENCH_METHODS] = {0};
  uint64_t totalUs[AMP_COMP_BENCH_METHODS] = {0};

  for (uint8_t method = 0; method < AMP_COMP_BENCH_METHODS; ++method) {
    amp_comp_method = method;
    amp_comp_bench_reset_win_cache();
    uint32_t t0 = micros();
    uint32_t calls = 0;
    volatile uint16_t sink = 0;

    for (uint8_t o = 0; o < AMP_COMP_BENCH_OSCS; ++o) {
      for (int32_t cHz = AMP_COMP_BENCH_CHZ_MIN; cHz <= cHzMax; ++cHz) {
        float f = (float)cHz * 0.01f;
        sink = (uint16_t)(sink + get_chan_level_by_method(f, o));
        ++calls;
      }
    }

    uint32_t t1 = micros();
    totalCalls[method] = calls;
    totalUs[method] = (uint64_t)(t1 - t0);
    (void)sink;
  }
  amp_comp_method = saved_method;

  double refUs = (double)totalUs[AMP_COMP_FLOAT_QUAD];
  if (refUs < 1.0) refUs = 1.0;

  bench_out_println("=== AMP COMP BENCH ===");
  bench_out_println("cal=SYNTHETIC linear Hz/level 1..MAX / 1..DIV_COUNTER");
  bench_out_printf("grid=0.01Hz 1..%.0f oscs=%d (same as accuracy)\n",
                   (double)AMP_COMP_MAX_HZ, AMP_COMP_BENCH_OSCS);
  bench_out_printf("live_method=%s (restored after sweep)\n",
                   amp_comp_method_name(amp_comp_method));
  bench_out_println("dispatch=get_chan_level_by_method; ampWinCache reset per method");
  bench_out_println("method       calls    totalUs   meanNs  pctVsFloat");
  bench_out_println("------------ -------- --------- -------- ----------");
  for (uint8_t method = 0; method < AMP_COMP_BENCH_METHODS; ++method) {
    double meanNs = ((double)totalUs[method] * 1000.0) / (double)totalCalls[method];
    double pct = 100.0 * (double)totalUs[method] / refUs;
    bench_out_printf("%-12s %8lu %9lu %8.2f %10.1f\n",
                     amp_comp_method_name(method),
                     (unsigned long)totalCalls[method],
                     (unsigned long)totalUs[method],
                     meanNs,
                     pct);
  }
  bench_out_println("======================");
}

static void amp_comp_bench_run_accuracy_body() {
  struct Acc {
    double sumAbs;
    double maxInBand;
    float hzAtMaxInBand;
    double tipMax;
    uint32_t tipErrGt1;
    uint32_t n;
    uint32_t errGt1;
    uint32_t errEq1;
  } acc[AMP_COMP_BENCH_METHODS];

  memset(acc, 0, sizeof(acc));

  const uint8_t saved_method = amp_comp_method;
  amp_comp_bench_reset_win_cache();

  // Inclusive 1.00 … AMP_COMP_MAX_HZ (matches synthetic / typical first cal knot at 1 Hz).
  const int32_t cHzMax = AMP_COMP_MAX_HZ * 100;
  const float tipHz = (float)AMP_COMP_MAX_HZ;
  for (uint8_t o = 0; o < AMP_COMP_BENCH_OSCS; ++o) {
    for (int32_t cHz = AMP_COMP_BENCH_CHZ_MIN; cHz <= cHzMax; ++cHz) {
      float f = (float)cHz * 0.01f; // exact 2-decimal musical grid

      uint16_t ref = get_chan_level_float_quad(f, o);

      for (uint8_t method = 0; method < AMP_COMP_BENCH_METHODS; ++method) {
        if (method == AMP_COMP_FLOAT_QUAD) continue;
        amp_comp_method = method;
        ampWinCache[o] = -1; // gold just filled the shared cache
        uint16_t y = get_chan_level_by_method(f, o);
        double e = fabs((double)y - (double)ref);
        Acc &a = acc[method];
        a.sumAbs += e;
        a.n++;
        if (e > 1.0) a.errGt1++;
        if (e == 1.0) a.errEq1++;

        if (f >= tipHz) {
          if (e > a.tipMax) a.tipMax = e;
          if (e > 1.0) a.tipErrGt1++;
        } else if (e > a.maxInBand) {
          a.maxInBand = e;
          a.hzAtMaxInBand = f;
        }
      }
    }
  }
  amp_comp_method = saved_method;

  uint32_t lutIntMaxErr = 0;
  for (uint8_t o = 0; o < AMP_COMP_BENCH_OSCS; ++o) {
    for (int32_t hz = 0; hz <= AMP_COMP_MAX_HZ; ++hz) {
      uint16_t ref = get_chan_level_float_quad((float)hz, o);
      uint16_t y = get_chan_level_lut((float)hz, o);
      uint32_t e = (y > ref) ? (uint32_t)(y - ref) : (uint32_t)(ref - y);
      if (e > lutIntMaxErr) lutIntMaxErr = e;
    }
  }

  // n is the same for every compared method.
  uint32_t nSamples = 0;
  for (uint8_t method = 0; method < AMP_COMP_BENCH_METHODS; ++method) {
    if (method == AMP_COMP_FLOAT_QUAD) continue;
    nSamples = acc[method].n;
    break;
  }

  bench_out_println("=== AMP COMP ACCURACY ===");
  bench_out_printf("Errors vs FLOAT_QUAD in RANGE PWM counts (full scale = %u)\n",
                   (unsigned)DIV_COUNTER);
  bench_out_printf("cal=SYNTHETIC linear Hz/level 1..MAX / 1..DIV_COUNTER | "
                   "grid=0.01Hz 1..%.0f oscs=%d | n=%lu samples/method\n",
                   (double)AMP_COMP_MAX_HZ, AMP_COMP_BENCH_OSCS, (unsigned long)nSamples);
  bench_out_printf("LUT integer-Hz sanity: max err %lu PWM (want 0)\n",
                   (unsigned long)lutIntMaxErr);
  bench_out_println("dual-build plateau: FLOAT_QUAD/LUT early-out from original plateau Hz; "
                    "FIXED often interpolates until 7000 Hz domain clamp (expected gap)");

  for (uint8_t method = 0; method < AMP_COMP_BENCH_METHODS; ++method) {
    if (method == AMP_COMP_FLOAT_QUAD) continue;
    Acc &a = acc[method];
    double mean = (a.n > 0) ? (a.sumAbs / (double)a.n) : 0.0;
    double gt1Pct = (a.n > 0) ? (100.0 * (double)a.errGt1 / (double)a.n) : 0.0;
    double eq1Pct = (a.n > 0) ? (100.0 * (double)a.errEq1 / (double)a.n) : 0.0;
    double meanPct = 100.0 * mean / (double)DIV_COUNTER;
    double inBandPct = 100.0 * a.maxInBand / (double)DIV_COUNTER;
    double tipPct = 100.0 * a.tipMax / (double)DIV_COUNTER;

    bench_out_printf("\n--- %s ---\n", amp_comp_method_name(method));
    bench_out_printf("  typical (mean):     %.2f PWM   (%.3f%% full)\n",
                     mean, meanPct);

    if (a.maxInBand > 0.0) {
      bench_out_printf("  worst in-band:      %.0f PWM at %.2f Hz   (%.3f%% full)\n",
                       a.maxInBand, (double)a.hzAtMaxInBand, inBandPct);
    } else {
      bench_out_println("  worst in-band:      none >0 PWM");
    }

    // Tip outliers only when exact AMP_COMP_MAX_HZ disagrees by more than 1 PWM.
    if (a.tipErrGt1 > 0) {
      bench_out_printf("  tip outlier:        %.0f PWM at %.2f Hz  (%.1f%% full)  x%lu samples\n",
                       a.tipMax, (double)tipHz, tipPct,
                       (unsigned long)a.tipErrGt1);
    }

    if (a.errGt1 > 0 && a.errGt1 == a.tipErrGt1) {
      bench_out_printf("  worse than 1 PWM:   %lu / %lu  (%.3f%%)  (all at tip)\n",
                       (unsigned long)a.errGt1, (unsigned long)a.n, gt1Pct);
    } else {
      bench_out_printf("  worse than 1 PWM:   %lu / %lu  (%.3f%%)\n",
                       (unsigned long)a.errGt1, (unsigned long)a.n, gt1Pct);
    }
    bench_out_printf("  exactly 1 PWM:      %lu / %lu  (%.3f%%)\n",
                     (unsigned long)a.errEq1, (unsigned long)a.n, eq1Pct);
  }
  bench_out_println("=========================");
}

void amp_comp_bench_run_speed() {
  amp_comp_bench_with_synthetic(amp_comp_bench_run_speed_body);
}

void amp_comp_bench_run_accuracy() {
  amp_comp_bench_with_synthetic(amp_comp_bench_run_accuracy_body);
}

void print_amp_comp_bench() {
  if (amp_comp_bench_speed_pending) {
    amp_comp_bench_speed_pending = false;
    amp_comp_bench_run_speed();
  }
  if (amp_comp_bench_accuracy_pending) {
    amp_comp_bench_accuracy_pending = false;
    amp_comp_bench_run_accuracy();
  }
}

#else  // !AMP_COMP_BENCHMARK || !RUNNING_AVERAGE || !USE_FLOAT_AMP_COMP

volatile bool amp_comp_bench_speed_pending = false;
volatile bool amp_comp_bench_accuracy_pending = false;

void amp_comp_bench_run_speed() {}
void amp_comp_bench_run_accuracy() {}
void print_amp_comp_bench() {}

#endif

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/autotune.ino"
#include "_shared/autotune_impl.h"

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/autotune_search.ino"
#include "_shared/autotune_search_impl.h"

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/clkdiv_bench.ino"
#include "include_all.h"
#include <math.h>
#include "clkdiv.h"

// Clkdiv speed / accuracy (cmds 32 / 33). All six methods on both voice engines.
// Glue matches live: fixed Q24 helpers; float native Hz for GOLD/FLOAT, Hz→Q24 then helper
// for integer (same as clkdiv_live_hz_total_cycles). GOLD_REF is true-Hz llround (not a
// CLKDIV_MODE). Speed pctVsGOLD_REF. Needs RUNNING_AVERAGE for paced bench_out_* TX.

#if defined(RUNNING_AVERAGE)

volatile bool clkdiv_hp_bench_speed_pending = false;
volatile bool clkdiv_hp_bench_accuracy_pending = false;

enum : uint8_t {
  CDB_GOLD_REF = 0,
  CDB_GOLD_LIVE,
  CDB_FLOAT_LIVE,
  CDB_Q16,
  CDB_Q8,
  CDB_FAST_Q4,
  CDB_METHODS
};

// Accuracy + speed seq: 1.00 … 7000.00 Hz @ 0.01 Hz inclusive.
static constexpr int32_t CDB_HZ_MAX = 7000;
static constexpr int32_t CDB_CHZ_MIN = 100;
static constexpr uint32_t CDB_CORRECTION = 0; // live arbitrary_measured_correction_value

static constexpr int CDB_HIST_BINS = 100;
static constexpr double CDB_HIST_MAX_CENTS = 50.0;

// Speed seq: scratch chunk (div by 3). Jump: 10 geometric steps/semitone (~10× notes).
static constexpr uint32_t CDB_CHUNK = 384;
static constexpr uint32_t CDB_JUMP_STEPS = 10;
static constexpr uint32_t CDB_NOTE_N =
    (uint32_t)(sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]));
static constexpr uint32_t CDB_JUMP_MAX =
    ((CDB_JUMP_STEPS * (CDB_NOTE_N > 1u ? CDB_NOTE_N - 1u : 0u) + 1u + 2u) / 3u) * 3u;
static int64_t cdb_jump_q24[CDB_JUMP_MAX];
static double cdb_jump_hz[CDB_JUMP_MAX];
static uint32_t cdb_jump_n = 0;
static bool cdb_jump_ready = false;

static uint32_t cdb_seq_osc_n() {
  return (uint32_t)(CDB_HZ_MAX * 100 - CDB_CHZ_MIN + 1);
}

static const char *cdb_method_name(uint8_t method) {
  switch (method) {
    case CDB_GOLD_REF:    return "GOLD_REF";
    case CDB_GOLD_LIVE:   return "GOLD_LIVE";
    case CDB_FLOAT_LIVE:  return "FLOAT_LIVE";
    case CDB_Q16:         return "Q16";
    case CDB_Q8:          return "Q8";
    default:              return "FAST_Q4";
  }
}

static const char *cdb_live_clkdiv_name() {
#ifdef USE_FLOAT_VOICE_TASK
  return "FLOAT";
#elif CLKDIV_MODE == CLKDIV_GOLD
  return "GOLD";
#elif CLKDIV_MODE == CLKDIV_FLOAT
  return "FLOAT";
#elif CLKDIV_MODE == CLKDIV_Q16
  return "Q16";
#elif CLKDIV_MODE == CLKDIV_Q8
  return "Q8";
#else
  return "FAST_Q4";
#endif
}

static const char *cdb_voice_name() {
#ifdef USE_FLOAT_VOICE_TASK
  return "FLOAT";
#else
  return "FIXED";
#endif
}

static inline int64_t cdb_hz_to_q24(double hz) {
  return (int64_t)llround(hz * 16777216.0);
}

static inline int64_t cdb_float_hz_to_q24(float hz) {
  return (int64_t)llround((double)hz * 16777216.0);
}

static inline double cdb_q24_to_hz(int64_t freq_q24) {
  return (double)freq_q24 * (1.0 / 16777216.0);
}

static inline uint32_t cdb_pio(uint32_t total, uint32_t y, uint32_t w, uint32_t k) {
  return pio_clk_div_for_y(total + CDB_CORRECTION, y, w, k);
}

// GOLD_REF: true-Hz llround (both engines).
static inline uint32_t cdb_clk_div_gold_ref(uint32_t sys_hz, double hz,
                                            uint32_t y, uint32_t w, uint32_t k) {
  return cdb_pio(clkdiv_gold_hz_total_cycles(sys_hz, hz), y, w, k);
}

#ifdef USE_FLOAT_VOICE_TASK
// Float voice: native Hz. Integer CLKDIV_MODE rows pay Hz→Q24 (matches live clkdiv_live_hz).
static inline uint32_t cdb_clk_div_by_method(uint8_t method, uint32_t sys_hz,
                                             int64_t /*freq_q24*/, double hz,
                                             uint32_t y, uint32_t w, uint32_t k) {
  if (method == CDB_GOLD_REF) return cdb_clk_div_gold_ref(sys_hz, hz, y, w, k);
  const float freq_f = (float)hz;
  if (method == CDB_GOLD_LIVE)
    return cdb_pio(clkdiv_gold_hz_total_cycles(sys_hz, (double)freq_f), y, w, k);
  if (method == CDB_FLOAT_LIVE)
    return cdb_pio(clkdiv_float_hz_total_cycles(sys_hz, freq_f), y, w, k);
  const int64_t q24 = cdb_float_hz_to_q24(freq_f);
  if (method == CDB_Q16)
    return cdb_pio(clkdiv_q16_total_cycles(sys_hz, q24), y, w, k);
  if (method == CDB_Q8)
    return cdb_pio(clkdiv_q8_total_cycles(sys_hz, q24), y, w, k);
  return cdb_pio(clkdiv_fast_q4_total_cycles(sys_hz, q24), y, w, k);
}
#else
// Fixed voice: Q24 domain. GOLD_REF uses true Hz; others use precomputed q24 (no extra convert).
static inline uint32_t cdb_clk_div_by_method(uint8_t method, uint32_t sys_hz,
                                             int64_t freq_q24, double hz,
                                             uint32_t y, uint32_t w, uint32_t k) {
  if (method == CDB_GOLD_REF) return cdb_clk_div_gold_ref(sys_hz, hz, y, w, k);
  if (method == CDB_GOLD_LIVE)
    return cdb_pio(clkdiv_gold_total_cycles(sys_hz, freq_q24), y, w, k);
  if (method == CDB_FLOAT_LIVE)
    return cdb_pio(clkdiv_float_total_cycles(sys_hz, freq_q24), y, w, k);
  if (method == CDB_Q16)
    return cdb_pio(clkdiv_q16_total_cycles(sys_hz, freq_q24), y, w, k);
  if (method == CDB_Q8)
    return cdb_pio(clkdiv_q8_total_cycles(sys_hz, freq_q24), y, w, k);
  return cdb_pio(clkdiv_fast_q4_total_cycles(sys_hz, freq_q24), y, w, k);
}
#endif

static inline double cdb_out_hz(uint32_t sys_hz, uint32_t div,
                                uint32_t y, uint32_t w, uint32_t k) {
  uint64_t period = (uint64_t)y + (uint64_t)div * (uint64_t)w + (uint64_t)k;
  if (period == 0) return 0.0;
  return (double)sys_hz / (double)period;
}

static inline double cdb_cents_abs(double out_hz, double target_hz) {
  if (!(out_hz > 0.0) || !(target_hz > 0.0)) return 0.0;
  return fabs(1200.0 * log2(out_hz / target_hz));
}

static int cdb_hz_band(double hz) {
  if (hz < 100.0) return 0;
  if (hz < 1000.0) return 1;
  return 2;
}

static void cdb_hist_add(uint32_t *h, double cents) {
  if (cents < 0.0) cents = 0.0;
  int b = (int)(cents * (double)CDB_HIST_BINS / CDB_HIST_MAX_CENTS);
  if (b < 0) b = 0;
  if (b >= CDB_HIST_BINS) b = CDB_HIST_BINS - 1;
  h[b]++;
}

static double cdb_hist_percentile(const uint32_t *h, uint32_t n, double pct) {
  if (n == 0) return 0.0;
  uint64_t target = (uint64_t)((pct / 100.0) * (double)n);
  if (target >= n) target = n - 1u;
  uint64_t cum = 0;
  for (int b = 0; b < CDB_HIST_BINS; ++b) {
    cum += h[b];
    if (cum > target) {
      return ((double)b + 0.5) * CDB_HIST_MAX_CENTS / (double)CDB_HIST_BINS;
    }
  }
  return CDB_HIST_MAX_CENTS;
}

static void cdb_ensure_jump_grid() {
  if (cdb_jump_ready) return;

  uint32_t n = 0;
  for (uint32_t i = 0; i + 1u < CDB_NOTE_N && n + CDB_JUMP_STEPS <= CDB_JUMP_MAX; ++i) {
    const double f0 = (double)sNotePitches_q24[i];
    for (uint32_t k = 0; k < CDB_JUMP_STEPS; ++k) {
      int64_t f = (int64_t)llround(f0 * pow(2.0, (double)k / 120.0));
      if (f < 1) f = 1;
      cdb_jump_q24[n] = f;
      cdb_jump_hz[n] = cdb_q24_to_hz(f);
      ++n;
    }
  }
  if (CDB_NOTE_N > 0 && n < CDB_JUMP_MAX) {
    cdb_jump_q24[n] = sNotePitches_q24[CDB_NOTE_N - 1u];
    cdb_jump_hz[n] = cdb_q24_to_hz(cdb_jump_q24[n]);
    ++n;
  }
  while (n % 3u != 0 && n < CDB_JUMP_MAX) {
    cdb_jump_q24[n] = (n > 0) ? cdb_jump_q24[n - 1u] : (int64_t)(1LL << 24);
    cdb_jump_hz[n] = cdb_q24_to_hz(cdb_jump_q24[n]);
    ++n;
  }
  cdb_jump_n = n;
  cdb_jump_ready = true;
}

// Timed 3-osc clone. q24 + true Hz precomputed; GOLD_REF uses hz only.
static void cdb_speed_run(uint8_t method, const int64_t *q24, const double *hz, uint32_t nPts,
                          uint32_t repeats, uint32_t sys_hz,
                          uint32_t y, uint32_t w, uint32_t k,
                          uint32_t *outFrames, uint64_t *outUs) {
  uint32_t t0 = micros();
  uint32_t frames = 0;
  volatile uint32_t sink = 0;

  for (uint32_t r = 0; r < repeats; ++r) {
    for (uint32_t i = 0; i + 2u < nPts; i += 3u) {
      sink += cdb_clk_div_by_method(method, sys_hz, q24[i], hz[i], y, w, k);
      sink += cdb_clk_div_by_method(method, sys_hz, q24[i + 1], hz[i + 1], y, w, k);
      sink += cdb_clk_div_by_method(method, sys_hz, q24[i + 2], hz[i + 2], y, w, k);
      ++frames;
    }
  }

  uint32_t t1 = micros();
  *outFrames = frames;
  *outUs = (uint64_t)(t1 - t0);
  (void)sink;
}

// Same 0.01 Hz grid as accuracy. Fill q24 + true Hz outside t0/t1.
static void cdb_speed_run_seq(uint8_t method, uint32_t sys_hz,
                              uint32_t y, uint32_t w, uint32_t k,
                              uint32_t *outFrames, uint64_t *outUs) {
  int64_t chunk_q24[CDB_CHUNK];
  double chunk_hz[CDB_CHUNK];
  const int32_t cHzMax = CDB_HZ_MAX * 100;
  uint32_t frames = 0;
  uint64_t totalUs = 0;
  volatile uint32_t sink = 0;

  for (int32_t cHz = CDB_CHZ_MIN; cHz <= cHzMax; ) {
    uint32_t n = 0;
    while (n < CDB_CHUNK && cHz <= cHzMax) {
      double hz = (double)cHz * 0.01;
      chunk_hz[n] = hz;
      chunk_q24[n] = cdb_hz_to_q24(hz);
      ++n;
      ++cHz;
    }
    while (n % 3u != 0 && n < CDB_CHUNK) {
      chunk_hz[n] = chunk_hz[n - 1u];
      chunk_q24[n] = chunk_q24[n - 1u];
      ++n;
    }

    uint32_t t0 = micros();
    for (uint32_t i = 0; i + 2u < n; i += 3u) {
      sink += cdb_clk_div_by_method(method, sys_hz, chunk_q24[i], chunk_hz[i], y, w, k);
      sink += cdb_clk_div_by_method(method, sys_hz, chunk_q24[i + 1], chunk_hz[i + 1], y, w, k);
      sink += cdb_clk_div_by_method(method, sys_hz, chunk_q24[i + 2], chunk_hz[i + 2], y, w, k);
      ++frames;
    }
    uint32_t t1 = micros();
    totalUs += (uint64_t)(t1 - t0);
  }

  *outFrames = frames;
  *outUs = totalUs;
  (void)sink;
}

static void cdb_speed_print_table(const char *pattern, uint32_t unique, uint32_t repeats,
                                  const uint32_t *frames, const uint64_t *totalUs) {
  double refMeanUs = (frames[CDB_GOLD_REF] > 0)
                         ? ((double)totalUs[CDB_GOLD_REF] / (double)frames[CDB_GOLD_REF])
                         : 1.0;
  if (refMeanUs < 1e-9) refMeanUs = 1e-9;

  bench_out_printf("-- pattern=%s  unique=%lu repeats=%lu oscs=3\n",
                   pattern, (unsigned long)unique, (unsigned long)repeats);
  bench_out_println("method      frames    totalUs   meanUs  meanNs/osc  pctVsGOLD_REF");
  bench_out_println("----------- -------- --------- -------- ---------- --------------");
  for (uint8_t method = 0; method < CDB_METHODS; ++method) {
    double meanUs = (frames[method] > 0)
                        ? ((double)totalUs[method] / (double)frames[method])
                        : 0.0;
    double meanNsOsc = meanUs * 1000.0 / 3.0;
    double pct = 100.0 * meanUs / refMeanUs;
    bench_out_printf("%-11s %8lu %9lu %8.3f %10.1f %14.1f\n",
                     cdb_method_name(method),
                     (unsigned long)frames[method],
                     (unsigned long)totalUs[method],
                     meanUs,
                     meanNsOsc,
                     pct);
  }
}

void clkdiv_hp_bench_run_speed() {
  cdb_ensure_jump_grid();

  const uint32_t sys_hz = sysClock_Hz;
  const uint32_t y = pioPulseLength;
  const uint32_t w = PIO_RAMP_WEIGHT_FREE;
  const uint32_t k = PIO_PERIOD_OVERHEAD_FREE;
  const uint32_t seqUnique = cdb_seq_osc_n();

  uint32_t seqFramesEst = (seqUnique + 2u) / 3u;
  const uint32_t jumpFrames = (cdb_jump_n >= 3u) ? (cdb_jump_n / 3u) : 1u;
  uint32_t jumpRepeats = 1;
  if (seqFramesEst > jumpFrames)
    jumpRepeats = (seqFramesEst + jumpFrames - 1u) / jumpFrames;

  bench_out_println("=== CLKDIV BENCH ===");
  bench_out_printf("live_clkdiv=%s voice=%s (engine-domain glue + GOLD_REF; does not touch PIO)\n",
                   cdb_live_clkdiv_name(), cdb_voice_name());
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_println("domain=FLOAT Hz  GOLD_LIVE=native double on freq_f  FLOAT_LIVE=native float");
  bench_out_println("integer=Hz→Q24 then helper (matches live clkdiv_live_hz)");
#else
  bench_out_println("domain=FIXED Q24  GOLD_LIVE=Q24 double  FLOAT_LIVE=Q24 float  integer=Q24 helpers");
#endif
  bench_out_printf("seq=0.01Hz 1..%d (same grid as accuracy)  jump=10 steps/semitone (~10× notes)\n",
                   CDB_HZ_MAX);
  bench_out_println("compare dump10 clkdiv math mean ≈ meanUs/frame; ranking from pctVsGOLD_REF (jump)");
  bench_out_printf("y=%lu w=%lu k=%lu (free-run) sys=%lu Hz\n",
                   (unsigned long)y, (unsigned long)w, (unsigned long)k,
                   (unsigned long)sys_hz);

  {
    uint32_t frames[CDB_METHODS] = {0};
    uint64_t totalUs[CDB_METHODS] = {0};
    for (uint8_t method = 0; method < CDB_METHODS; ++method) {
      cdb_speed_run_seq(method, sys_hz, y, w, k, &frames[method], &totalUs[method]);
    }
    cdb_speed_print_table("seq", seqUnique, 1u, frames, totalUs);
  }
  {
    uint32_t frames[CDB_METHODS] = {0};
    uint64_t totalUs[CDB_METHODS] = {0};
    for (uint8_t method = 0; method < CDB_METHODS; ++method) {
      cdb_speed_run(method, cdb_jump_q24, cdb_jump_hz, cdb_jump_n, jumpRepeats,
                    sys_hz, y, w, k, &frames[method], &totalUs[method]);
    }
    cdb_speed_print_table("jump", cdb_jump_n, jumpRepeats, frames, totalUs);
  }
  bench_out_println("=======================");
}

void clkdiv_hp_bench_run_accuracy() {
  const uint32_t sys_hz = sysClock_Hz;
  const uint32_t y = pioPulseLength;
  const uint32_t w = PIO_RAMP_WEIGHT_FREE;
  const uint32_t k = PIO_PERIOD_OVERHEAD_FREE;
  const int32_t cHzMax = CDB_HZ_MAX * 100;

  struct Acc {
    double sumCents;
    double maxCents;
    float hzAtMax;
    uint32_t n;
    uint32_t worse01;   // > 0.1¢
    uint32_t worse05;   // > 0.5¢
    uint32_t worse10;   // > 1.0¢
    uint32_t maxDivDelta;
    uint32_t hist[CDB_HIST_BINS];
    double bandSum[3];
    double bandMax[3];
    uint32_t bandN[3];
  } acc[CDB_METHODS];

  memset(acc, 0, sizeof(acc));

  uint32_t disagreeQ8PreciseN = 0, maxQ8Precise = 0;
  uint32_t disagreeLiveRefN = 0, maxLiveRef = 0;
  uint32_t disagreeFloatGoldN = 0, maxFloatGold = 0;

  for (int32_t cHz = CDB_CHZ_MIN; cHz <= cHzMax; ++cHz) {
    double hz = (double)cHz * 0.01;
    int64_t freq_q24 = cdb_hz_to_q24(hz);

    uint32_t divs[CDB_METHODS];
    for (uint8_t method = 0; method < CDB_METHODS; ++method) {
      divs[method] = cdb_clk_div_by_method(method, sys_hz, freq_q24, hz, y, w, k);
    }
    const uint32_t ref_div = divs[CDB_GOLD_REF];

#ifdef USE_FLOAT_VOICE_TASK
    const int64_t q24_int = cdb_float_hz_to_q24((float)hz);
#else
    const int64_t q24_int = freq_q24;
#endif
    const uint32_t precise8_div =
        cdb_pio(clkdiv_precise_q8_total_cycles(sys_hz, q24_int), y, w, k);
    uint32_t dQ8P = (divs[CDB_Q8] > precise8_div)
                        ? (divs[CDB_Q8] - precise8_div)
                        : (precise8_div - divs[CDB_Q8]);
    if (dQ8P != 0) ++disagreeQ8PreciseN;
    if (dQ8P > maxQ8Precise) maxQ8Precise = dQ8P;

    uint32_t dLive = (divs[CDB_GOLD_LIVE] > ref_div)
                         ? (divs[CDB_GOLD_LIVE] - ref_div)
                         : (ref_div - divs[CDB_GOLD_LIVE]);
    if (dLive != 0) ++disagreeLiveRefN;
    if (dLive > maxLiveRef) maxLiveRef = dLive;

    uint32_t dFG = (divs[CDB_FLOAT_LIVE] > divs[CDB_GOLD_LIVE])
                       ? (divs[CDB_FLOAT_LIVE] - divs[CDB_GOLD_LIVE])
                       : (divs[CDB_GOLD_LIVE] - divs[CDB_FLOAT_LIVE]);
    if (dFG != 0) ++disagreeFloatGoldN;
    if (dFG > maxFloatGold) maxFloatGold = dFG;

    const int band = cdb_hz_band(hz);
    for (uint8_t method = 0; method < CDB_METHODS; ++method) {
      double out = cdb_out_hz(sys_hz, divs[method], y, w, k);
      double c = cdb_cents_abs(out, hz);
      Acc &a = acc[method];
      a.sumCents += c;
      a.n++;
      if (c > 0.1) a.worse01++;
      if (c > 0.5) a.worse05++;
      if (c > 1.0) a.worse10++;
      if (c > a.maxCents) {
        a.maxCents = c;
        a.hzAtMax = (float)hz;
      }
      uint32_t dDelta = (divs[method] > ref_div)
                            ? (divs[method] - ref_div)
                            : (ref_div - divs[method]);
      if (dDelta > a.maxDivDelta) a.maxDivDelta = dDelta;
      cdb_hist_add(a.hist, c);
      a.bandSum[band] += c;
      a.bandN[band]++;
      if (c > a.bandMax[band]) a.bandMax[band] = c;
    }
  }

  uint32_t nSamples = acc[CDB_GOLD_REF].n;

  bench_out_println("=== CLKDIV ACCURACY ===");
  bench_out_printf("live_clkdiv=%s voice=%s | ref=GOLD_REF true-Hz llround + pio_clk_div_for_y\n",
                   cdb_live_clkdiv_name(), cdb_voice_name());
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_println("GOLD_LIVE=native double on freq_f  FLOAT_LIVE=native float  integer=Hz→Q24");
#else
  bench_out_println("GOLD_LIVE=Q24→double  FLOAT_LIVE=Q24→float  Q4≈±1/32 Hz  Q8≈±1/512 Hz before PIO");
#endif
  bench_out_printf("y=%lu w=%lu k=%lu (free-run) sys=%lu Hz\n",
                   (unsigned long)y, (unsigned long)w, (unsigned long)k,
                   (unsigned long)sys_hz);
  bench_out_printf("grid=0.01Hz 1..%d | n=%lu | cents vs target Hz; max|Δdiv| vs GOLD_REF\n",
                   CDB_HZ_MAX, (unsigned long)nSamples);

  bench_out_println("\n-- vs target Hz (Δdiv vs GOLD_REF) --");
  bench_out_println("method      mean¢    max¢   p50¢   p95¢   p99¢  >0.1¢%  >0.5¢%  >1.0¢%  max|Δdiv|");
  for (uint8_t method = 0; method < CDB_METHODS; ++method) {
    Acc &a = acc[method];
    double mean = (a.n > 0) ? (a.sumCents / (double)a.n) : 0.0;
    double p50 = cdb_hist_percentile(a.hist, a.n, 50.0);
    double p95 = cdb_hist_percentile(a.hist, a.n, 95.0);
    double p99 = cdb_hist_percentile(a.hist, a.n, 99.0);
    double pct01 = (a.n > 0) ? (100.0 * (double)a.worse01 / (double)a.n) : 0.0;
    double pct05 = (a.n > 0) ? (100.0 * (double)a.worse05 / (double)a.n) : 0.0;
    double pct10 = (a.n > 0) ? (100.0 * (double)a.worse10 / (double)a.n) : 0.0;
    bench_out_printf("%-11s %6.3f %7.3f %6.3f %6.3f %6.3f %6.2f %6.2f %6.2f %10lu\n",
                     cdb_method_name(method),
                     mean, a.maxCents, p50, p95, p99, pct01, pct05, pct10,
                     (unsigned long)a.maxDivDelta);
    bench_out_printf("            max@%.2f Hz\n", (double)a.hzAtMax);
  }

  bench_out_println("\n-- vs target by Hz-band (low<100, mid<1000, high) --");
  bench_out_println("method      low mean/max¢      mid mean/max¢      high mean/max¢");
  for (uint8_t method = 0; method < CDB_METHODS; ++method) {
    Acc &a = acc[method];
    double m0 = (a.bandN[0] > 0) ? (a.bandSum[0] / (double)a.bandN[0]) : 0.0;
    double m1 = (a.bandN[1] > 0) ? (a.bandSum[1] / (double)a.bandN[1]) : 0.0;
    double m2 = (a.bandN[2] > 0) ? (a.bandSum[2] / (double)a.bandN[2]) : 0.0;
    bench_out_printf("%-11s %7.3f/%7.3f   %7.3f/%7.3f   %7.3f/%7.3f\n",
                     cdb_method_name(method),
                     m0, a.bandMax[0], m1, a.bandMax[1], m2, a.bandMax[2]);
  }

  auto pctN = [nSamples](uint32_t n) -> double {
    return (nSamples > 0) ? (100.0 * (double)n / (double)nSamples) : 0.0;
  };
  bench_out_println("\n-- vs GOLD_REF clk_div --");
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_printf("GOLD_LIVE vs GOLD_REF   disagree=%lu / %lu (%.3f%%)  max|Δdiv|=%lu  (float-Hz quant)\n",
                   (unsigned long)disagreeLiveRefN, (unsigned long)nSamples,
                   pctN(disagreeLiveRefN), (unsigned long)maxLiveRef);
#else
  bench_out_printf("GOLD_LIVE vs GOLD_REF   disagree=%lu / %lu (%.3f%%)  max|Δdiv|=%lu  (Q24 floor)\n",
                   (unsigned long)disagreeLiveRefN, (unsigned long)nSamples,
                   pctN(disagreeLiveRefN), (unsigned long)maxLiveRef);
#endif
  bench_out_printf("FLOAT_LIVE vs GOLD_REF  max|Δdiv|=%lu\n",
                   (unsigned long)acc[CDB_FLOAT_LIVE].maxDivDelta);
  bench_out_printf("Q16 vs GOLD_REF         max|Δdiv|=%lu\n",
                   (unsigned long)acc[CDB_Q16].maxDivDelta);
  bench_out_printf("Q8 vs GOLD_REF          max|Δdiv|=%lu\n",
                   (unsigned long)acc[CDB_Q8].maxDivDelta);
  bench_out_printf("FAST_Q4 vs GOLD_REF     max|Δdiv|=%lu\n",
                   (unsigned long)acc[CDB_FAST_Q4].maxDivDelta);
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_println("-- FLOAT_LIVE vs GOLD_LIVE clk_div (float mantissa vs double on freq_f) --");
#else
  bench_out_println("-- FLOAT_LIVE vs GOLD_LIVE clk_div (float mantissa vs double) --");
#endif
  bench_out_printf("disagree=%lu / %lu (%.3f%%)  max|Δdiv|=%lu\n",
                   (unsigned long)disagreeFloatGoldN, (unsigned long)nSamples,
                   pctN(disagreeFloatGoldN), (unsigned long)maxFloatGold);
  bench_out_println("-- Q8 vs internal precise_q8 clk_div (same Q8 identity) --");
  bench_out_printf("disagree=%lu / %lu (%.3f%%)  max|Δdiv|=%lu  (expect 0 above ~16 Hz)\n",
                   (unsigned long)disagreeQ8PreciseN, (unsigned long)nSamples,
                   pctN(disagreeQ8PreciseN), (unsigned long)maxQ8Precise);

  bench_out_println("\n-- human --");
  bench_out_printf("Y-locked free-run (y=%lu w=%lu k=%lu): cents vs target Hz = Q quant + PIO.\n",
                   (unsigned long)y, (unsigned long)w, (unsigned long)k);
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_println("GOLD_REF Δdiv is 0. GOLD_LIVE vs GOLD_REF is float-Hz quantize.");
  bench_out_println("FLOAT_LIVE vs GOLD_LIVE is float mantissa vs double on the same freq_f.");
#else
  bench_out_println("GOLD_REF Δdiv is 0 by definition. GOLD_LIVE vs GOLD_REF is Q24 quantize.");
  bench_out_println("FLOAT_LIVE vs GOLD_LIVE is float mantissa vs Q24 double.");
#endif
  bench_out_println("p50/p95/p99 use 0.5¢ hist bins (0.250=bin0); trust mean / max / >x¢%.");
  bench_out_println("max|Δdiv| at ~1 Hz is huge clk_div counts, not pitch. Disagree% ≠ audible error.");

  for (uint8_t method = 0; method < CDB_METHODS; ++method) {
    Acc &a = acc[method];
    double mean = (a.n > 0) ? (a.sumCents / (double)a.n) : 0.0;
    double m0 = (a.bandN[0] > 0) ? (a.bandSum[0] / (double)a.bandN[0]) : 0.0;
    double m1 = (a.bandN[1] > 0) ? (a.bandSum[1] / (double)a.bandN[1]) : 0.0;
    double m2 = (a.bandN[2] > 0) ? (a.bandSum[2] / (double)a.bandN[2]) : 0.0;
    double playMax = (a.bandMax[1] > a.bandMax[2]) ? a.bandMax[1] : a.bandMax[2];
    const char *worstWhere = (a.hzAtMax < 100.0f) ? "sub-audio/LFO (<100 Hz)"
                                                   : "keyboard range";
    bench_out_printf("\n%s:\n", cdb_method_name(method));
    bench_out_printf("  typical (mean):   %.3f¢\n", mean);
    bench_out_printf("  worst:            %.3f¢ at %.2f Hz  (%s)\n",
                     a.maxCents, (double)a.hzAtMax, worstWhere);
    bench_out_printf("  low <100 Hz:      mean %.3f¢  max %.3f¢\n",
                     m0, a.bandMax[0]);
    bench_out_printf("  mid 100-1000 Hz:  mean %.3f¢  max %.3f¢\n", m1, a.bandMax[1]);
    bench_out_printf("  high >1000 Hz:    mean %.3f¢  max %.3f¢  (PIO ±2 cyc, not clkdiv mode)\n",
                     m2, a.bandMax[2]);
    bench_out_printf("  playing range:    max %.3f¢ (mid+high)\n", playMax);
    bench_out_printf("  vs GOLD_REF:      max|Δdiv|=%lu\n", (unsigned long)a.maxDivDelta);
  }

  {
    Acc &ref = acc[CDB_GOLD_REF];
    Acc &live = acc[CDB_GOLD_LIVE];
    Acc &fl = acc[CDB_FLOAT_LIVE];
    Acc &q16 = acc[CDB_Q16];
    Acc &q8 = acc[CDB_Q8];
    Acc &f4 = acc[CDB_FAST_Q4];
    auto playMax = [](const Acc &a) -> double {
      return (a.bandMax[1] > a.bandMax[2]) ? a.bandMax[1] : a.bandMax[2];
    };
    bench_out_println("\nverdict:");
    bench_out_printf("  playing range (mid+high) vs target Hz: GOLD_REF %.3f¢  GOLD_LIVE %.3f¢  FLOAT_LIVE %.3f¢  Q16 %.3f¢  Q8 %.3f¢  FAST_Q4 %.3f¢\n",
                     playMax(ref), playMax(live), playMax(fl), playMax(q16), playMax(q8), playMax(f4));
#ifdef USE_FLOAT_VOICE_TASK
    bench_out_printf("  GOLD_LIVE vs GOLD_REF Δdiv disagree %.3f%% (max|Δdiv|=%lu) — float-Hz quant.\n",
                     pctN(disagreeLiveRefN), (unsigned long)maxLiveRef);
#else
    bench_out_printf("  GOLD_LIVE vs GOLD_REF Δdiv disagree %.3f%% (max|Δdiv|=%lu) — Q24 floor.\n",
                     pctN(disagreeLiveRefN), (unsigned long)maxLiveRef);
#endif
    bench_out_printf("  FLOAT_LIVE vs GOLD_LIVE Δdiv disagree %.3f%% (max|Δdiv|=%lu) — float vs double.\n",
                     pctN(disagreeFloatGoldN), (unsigned long)maxFloatGold);
    if (playMax(f4) <= playMax(q16) * 1.5 + 0.05) {
      bench_out_printf("  FAST_Q4 ≈ Q16 on playing range — PIO-limited.\n");
    } else {
      bench_out_printf("  Q16 closer to GOLD_REF than FAST_Q4 on playing range.\n");
    }
    bench_out_printf("  low-Hz tail: GOLD_REF %.3f¢  GOLD_LIVE %.3f¢  FLOAT_LIVE %.3f¢  Q16 %.3f¢  Q8 %.3f¢  FAST_Q4 %.3f¢ at <100 Hz.\n",
                     ref.bandMax[0], live.bandMax[0], fl.bandMax[0], q16.bandMax[0], q8.bandMax[0], f4.bandMax[0]);
    bench_out_printf("  Q8 vs internal precise_q8 clk_div disagree %.3f%% (max|Δdiv|=%lu).\n",
                     pctN(disagreeQ8PreciseN), (unsigned long)maxQ8Precise);
  }
  bench_out_println("==========================");
}

void print_clkdiv_hp_bench() {
  if (clkdiv_hp_bench_speed_pending) {
    clkdiv_hp_bench_speed_pending = false;
    clkdiv_hp_bench_run_speed();
  }
  if (clkdiv_hp_bench_accuracy_pending) {
    clkdiv_hp_bench_accuracy_pending = false;
    clkdiv_hp_bench_run_accuracy();
  }
}

#else  // !RUNNING_AVERAGE

volatile bool clkdiv_hp_bench_speed_pending = false;
volatile bool clkdiv_hp_bench_accuracy_pending = false;

void clkdiv_hp_bench_run_speed() {}
void clkdiv_hp_bench_run_accuracy() {}
void print_clkdiv_hp_bench() {}

#endif

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/cv_out.ino"
// Soft VCA/VCF/reso CV math (~10 kHz with ADSR). Matrix→pitch always.
// Analog VCA/VCF/AS2164/PWM only when ENABLE_CV_OUTS. Always-on: Q15 matrix, lerp>>12, note-60.
// USE_FLOAT_CV_OUTS: float VCA/VCF/keytrack/drift (A/B override).
// Else: fixed Q15 / integer path (shipping default both MCUs).
#include "include_all.h"
#include <string.h>

// Panel depth 0..512 → CV units. LFO Q15 peak uses *2 (legacy HALF/CC).
static constexpr int32_t CV_U12_MAX = 4095;           // last valid 12-bit CV code
static constexpr int32_t CV_U12_SCALE = 4096;         // 1<<12 — divisors / Q15→u12
static constexpr int32_t CV_PANEL_DEPTH_FULL = 512;
static constexpr int32_t CV_LFO_Q15_PEAK_DIV = CV_PANEL_DEPTH_FULL * 2;  // 1024

// Resonance → VCA compensation (1 ms path in update_CV_outs).
static constexpr int CV_VCA_COMP_DEFAULT = 100;
static constexpr int CV_RESO_COMP_MAX_RESONANCE = 2300;
static constexpr int CV_RESO_COMP_MIN_RESONANCE = 50;
static constexpr int CV_RESO_COMP_MAX_VCA = 316;
static constexpr int CV_RESO_COMP_SLOPE_Q8 = 36;  // ≈ 0.14 via (span * 36) >> 8

// Always-on: divide by CV_U12_SCALE via >>12 (not /4095).
static inline uint16_t lerp_0_4095(uint16_t value, uint16_t y0, uint16_t y1) {
  return (uint16_t)((int32_t)y0 + ((((int32_t)y1 - (int32_t)y0) * (int32_t)value) >> 12));
}

static inline uint16_t cv_clamp_u12(int32_t v) {
  if (v < 0) return 0;
  if (v > CV_U12_MAX) return (uint16_t)CV_U12_MAX;
  return (uint16_t)v;
}

// (q15 * CV_U12_SCALE) >> 15; with SCALE=4096 ≡ q15 >> 3 (peak → 4095).
static inline uint16_t cv_q15_to_u12(int16_t q15) {
  return cv_clamp_u12(((int32_t)q15 * CV_U12_SCALE) >> 15);
}

// Boot: build AS2164 VCA linearize table (same control points as Mainboard).
void init_cv_out() {
  generateBezierArray({ 0, 4095 }, { 4095, 0 }, { 150, 1420 }, { -235, 815 }, 4096, AS2164_VCA_linearize_table);
  cv_update_mod_scales();
#ifndef USE_FLOAT_CV_OUTS
  // Full-scale Q15 drift → VCF_DRIFT CV units ≈ analogDrift (legacy peak).
  vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}

// LFO scales carry the negative sign that restores the Mainboard's LFO polarity.
void cv_bake_adsr2_to_vcf_scale() {
#ifdef USE_FLOAT_CV_OUTS
  // Float A/B: u12 ADSR levels × (depth / panel_full).
  ADSR2toVCF_scale = (float)ADSR2toVCF / (float)CV_PANEL_DEPTH_FULL;
#else
  // Q15: (src_q15 * scale) >> 15 → depth * CV_U12_MAX / PANEL_DEPTH_FULL at +1.0.
  ADSR2toVCF_scale_q15 =
    (int32_t)(((int64_t)ADSR2toVCF * (int64_t)CV_U12_MAX) / CV_PANEL_DEPTH_FULL);
#endif
}

void cv_bake_lfo2_to_vcf_scale() {
#ifdef USE_FLOAT_CV_OUTS
  LFO2toVCF_scale =
    -(float)LFO2toVCF *
    ((float)CV_U12_MAX / ((float)CV_LFO_Q15_PEAK_DIV * 32767.0f));
#else
  LFO2toVCF_scale_q15 =
    (int32_t)(-((int64_t)LFO2toVCF * (int64_t)CV_U12_MAX) / CV_LFO_Q15_PEAK_DIV);
#endif
}

void cv_bake_lfo1_to_vca_scale() {
#ifdef USE_FLOAT_CV_OUTS
  LFO1toVCA_scale =
    -(float)LFO1toVCA *
    ((float)CV_U12_MAX / ((float)CV_LFO_Q15_PEAK_DIV * 32767.0f));
#else
  LFO1toVCA_scale_q15 =
    (int32_t)(-((int64_t)LFO1toVCA * (int64_t)CV_U12_MAX) / CV_LFO_Q15_PEAK_DIV);
#endif
}

void cv_update_mod_scales() {
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
  cv_bake_lfo1_to_vca_scale();
}

// Hot path (~10 kHz with ADSR): matrix→pitch always; analog VCA/VCF only if ENABLE_CV_OUTS.
void __not_in_flash_func(update_CV_outs)() {
#ifndef ENABLE_CV_OUTS
  if (manualCalibrationFlag) {
    matrix_pitch_mod_q24 = 0;
    return;
  }
  matrix_pitch_mod_q24 = mod_matrix_eval_pitch_q24(LFO1Level, LFO2Level);
  return;
#else
  // Snapshot Core0 LFO mailbox once (VCA/VCF + matrix LFO1/LFO2 sources).
  const int16_t local_LFO1Level = LFO1Level;
  const int16_t local_LFO2Level = LFO2Level;

  if (timer1msFlag2) {
    if (RESONANCEAmpCompensation) {
      // Always-on integer (both float/fixed CV builds).
      const int resonance_in = min((int)RESONANCE, CV_RESO_COMP_MAX_RESONANCE);
      VCAResonanceCompensation =
        (resonance_in >= CV_RESO_COMP_MIN_RESONANCE)
          ? (int16_t)(CV_RESO_COMP_MAX_VCA -
                      (((resonance_in - CV_RESO_COMP_MIN_RESONANCE) * CV_RESO_COMP_SLOPE_Q8) >> 8))
          : (int16_t)CV_RESO_COMP_MAX_VCA;
    } else {
      VCAResonanceCompensation = CV_VCA_COMP_DEFAULT;
    }

#ifdef USE_FLOAT_CV_OUTS
    if (VCFKeytrack != 0) {
      for (byte i = 0; i < NUM_VOICES; i++) {
        // map(note, 0, 150, -60, 90) ≡ note - 60
        VCFKeytrackPerVoice[i] =
          1.00f + (float)(VCFKeytrackModifier * ((int)VOICE_NOTES[i] - 60));
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCFKeytrackPerVoice[i] = 1.0f;
      }
    }

    if (analogDrift != 0) {
      // LFO_DRIFT_LEVEL is mo-lfo Q15 (±32767); full scale → ≈ analogDrift CV units.
      const int16_t local_drift0 = LFO_DRIFT_LEVEL[0];
      const float drift_scale = (float)analogDrift * (1.0f / 32767.0f);
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = (float)local_drift0 * drift_scale;
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = 0.0f;
      }
    }
#else
    if (VCFKeytrack != 0) {
      for (byte i = 0; i < NUM_VOICES; i++) {
        const int32_t dn = (int)VOICE_NOTES[i] - 60;
        VCFKeytrackPerVoice_q15[i] =
          32768 + (int32_t)(((int64_t)VCFKeytrackModifier_q15 * dn));
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCFKeytrackPerVoice_q15[i] = 32768;
      }
    }

    if (analogDrift != 0) {
      const int16_t local_drift0 = LFO_DRIFT_LEVEL[0];
      const int16_t drift_cv =
        (int16_t)(((int64_t)local_drift0 * (int64_t)vcf_drift_scale_q15) >> 15);
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = drift_cv;
      }
    } else {
      for (byte i = 0; i < NUM_VOICES; i++) {
        VCF_DRIFT[i] = 0;
      }
    }
#endif
  }

  int32_t mod_sums[MOD_DEST_COUNT];
  if (!manualCalibrationFlag) {
    mod_matrix_accumulate(mod_sums, local_LFO1Level, local_LFO2Level);
    matrix_pitch_mod_q24 = mod_matrix_pitch_to_q24(mod_sums[MOD_DEST_PITCH]);
  } else {
    memset(mod_sums, 0, sizeof(mod_sums));
    matrix_pitch_mod_q24 = 0;
  }
  const int32_t matrix_cutoff = mod_sums[MOD_DEST_VCF_CUTOFF];

#ifdef USE_FLOAT_CV_OUTS
  const int16_t LFO1toVCA_calc = (int16_t)((float)local_LFO1Level * LFO1toVCA_scale);
  const float LFO2toVCF_mod = (float)local_LFO2Level * LFO2toVCF_scale;
  // Q15 env → u12-ish for float depth formulas (same as >> 3).
  static constexpr float ADSR_Q15_TO_U12 = 1.0f / 8.0f;
  const float ADSR2toVCFcalculated =
    (float)ADSR_VCF_Level_q15 * ADSR_Q15_TO_U12 * ADSR2toVCF_scale;
  const float ADSR2toVCF2calculated =
    (float)ADSR_VCF2_Level_q15 * ADSR_Q15_TO_U12 * ADSR2toVCF_scale;
  const float matrix_cutoff_f = (float)matrix_cutoff;

  for (byte i = 0; i < NUM_VOICES; i++) {
    float VCA_velocityFactor = 1.0f;
    if (velocityToVCAVal != 0) {
      VCA_velocityFactor = 1.0f - ((float)velocityToVCA * (127 - midi_velocity[i]));
    }

    const uint16_t env_u12 = cv_q15_to_u12(ADSR_VCA_Level_q15[i]);
    int16_t LFO1toVCA_current = (ADSR_VCA_Level_q15[i] == 0) ? 0 : LFO1toVCA_calc;
    uint16_t VCA_Calculated =
      (uint16_t)constrain((float)(env_u12 + LFO1toVCA_current) * VCA_velocityFactor, 0, 4095);
    VCA_PWM[i] = lerp_0_4095(AS2164_VCA_linearize_table[VCA_Calculated],
                             (uint16_t)VCAResonanceCompensation, (uint16_t)(4095 - VCALevel));

    float VCF_velocityFactor = 1.0f;
    if (velocityToVCFVal != 0) {
      VCF_velocityFactor = 1.0f - ((float)velocityToVCF * (127 - midi_velocity[i]));
    }
    if (i == 0) {
      float combinedValue =
        ADSR2toVCFcalculated + LFO2toVCF_mod + (float)CUTOFF + VCF_DRIFT[i] + matrix_cutoff_f;
      float finalValue = combinedValue * VCF_velocityFactor * VCFKeytrackPerVoice[i];
      VCF_PWM[0] = (uint16_t)(4095 - (int)constrain(finalValue, 0, 4095));

      float combinedValue2 =
        ADSR2toVCF2calculated + LFO2toVCF_mod + (float)CUTOFF + VCF_DRIFT[i] + matrix_cutoff_f;
      float finalValue2 = combinedValue2 * VCF_velocityFactor * VCFKeytrackPerVoice[i];
      VCF_PWM[1] = (uint16_t)(4095 - (int)constrain(finalValue2, 0, 4095));
    }
  }
#else
  const int16_t LFO1toVCA_calc =
    (int16_t)(((int64_t)local_LFO1Level * (int64_t)LFO1toVCA_scale_q15) >> 15);
  const int32_t LFO2toVCF_mod =
    (int32_t)(((int64_t)local_LFO2Level * (int64_t)LFO2toVCF_scale_q15) >> 15);
  const int32_t ADSR2toVCFcalculated =
    (int32_t)(((int64_t)ADSR_VCF_Level_q15 * (int64_t)ADSR2toVCF_scale_q15) >> 15);
  const int32_t ADSR2toVCF2calculated =
    (int32_t)(((int64_t)ADSR_VCF2_Level_q15 * (int64_t)ADSR2toVCF_scale_q15) >> 15);

  for (byte i = 0; i < NUM_VOICES; i++) {
    int32_t vca_q15 = 32768;
    if (velocityToVCAVal != 0) {
      vca_q15 = 32768 - velocityToVCA_q15 * (127 - (int32_t)midi_velocity[i]);
      if (vca_q15 < 0) vca_q15 = 0;
    }

    const uint16_t env_u12 = cv_q15_to_u12(ADSR_VCA_Level_q15[i]);
    const int16_t LFO1toVCA_current = (ADSR_VCA_Level_q15[i] == 0) ? 0 : LFO1toVCA_calc;
    const int32_t vca_pre = (int32_t)env_u12 + (int32_t)LFO1toVCA_current;
    const uint16_t VCA_Calculated =
      cv_clamp_u12((int32_t)(((int64_t)vca_pre * vca_q15) >> 15));
    VCA_PWM[i] = lerp_0_4095(AS2164_VCA_linearize_table[VCA_Calculated],
                             (uint16_t)VCAResonanceCompensation, (uint16_t)(4095 - VCALevel));

    int32_t vcf_vel_q15 = 32768;
    if (velocityToVCFVal != 0) {
      vcf_vel_q15 = 32768 - velocityToVCF_q15 * (127 - (int32_t)midi_velocity[i]);
      if (vcf_vel_q15 < 0) vcf_vel_q15 = 0;
    }
    if (i == 0) {
      int32_t combined =
        ADSR2toVCFcalculated + LFO2toVCF_mod + (int32_t)CUTOFF + (int32_t)VCF_DRIFT[i] +
        matrix_cutoff;
      int32_t scaled = (int32_t)(((int64_t)combined * vcf_vel_q15) >> 15);
      scaled = (int32_t)(((int64_t)scaled * VCFKeytrackPerVoice_q15[i]) >> 15);
      VCF_PWM[0] = (uint16_t)(4095 - (int)cv_clamp_u12(scaled));

      int32_t combined2 =
        ADSR2toVCF2calculated + LFO2toVCF_mod + (int32_t)CUTOFF + (int32_t)VCF_DRIFT[i] +
        matrix_cutoff;
      int32_t scaled2 = (int32_t)(((int64_t)combined2 * vcf_vel_q15) >> 15);
      scaled2 = (int32_t)(((int64_t)scaled2 * VCFKeytrackPerVoice_q15[i]) >> 15);
      VCF_PWM[1] = (uint16_t)(4095 - (int)cv_clamp_u12(scaled2));
    }
  }
#endif

  uint16_t dist_out = DIST_DRIVE;
  uint16_t dist_mix_out = DIST_MIX;
  if (!manualCalibrationFlag) {
    mod_matrix_apply_cv(mod_sums, &dist_out, &dist_mix_out);
  } else {
    RESONANCE_PWM[0] = RESONANCE;
    RESONANCE_PWM[1] = RESONANCE;
  }

  write_cv_pwm_raw(VCF_PWM[0], RESONANCE_PWM, VCA_PWM[0], dist_out, dist_mix_out);
#endif  // ENABLE_CV_OUTS
}

// Manual calibration (Mainboard setPWMOutsManualCalibration): filter wide open, VCA barely
// cracked, and only the oscillator being calibrated audible. Runs instead of update_CV_outs.
void update_CV_outs_manual_calibration() {
  static constexpr uint16_t CAL_CUTOFF_COMPARE = 0;
  static constexpr uint16_t CAL_RESONANCE_COMPARE = 0;
  static constexpr uint16_t CAL_VCA_COMPARE = 150;
  static constexpr uint16_t CAL_SQR_ON = 50;
  static constexpr uint16_t CAL_SQR_MUTED = 4095;
  // The SQR levels are inverted (lin_to_log_128[] — 4095 is silent), the sub CV
  // is direct (SubLevelVal * 32), so its mute is the other end of the scale.
  static constexpr uint16_t CAL_SUB_MUTED = 0;

  byte stage = (byte)manualCalibrationStage;
  uint8_t osc = cal_stage_to_osc(stage);
  if (osc > 2) osc = 2;

  waveSelector_manual_calibration(stage);

  // Saw: mute every pulse DAC so the analog square does not mix with saw.
  // Pulse and 440: only the calibrated oscillator's SQR DAC is open.
  const bool square = cal_stage_is_square(stage);
  OSC1Level = (osc == 0 && square) ? CAL_SQR_ON : CAL_SQR_MUTED;
  OSC2Level = (osc == 1 && square) ? CAL_SQR_ON : CAL_SQR_MUTED;
  OSC3Level = (osc == 2 && square) ? CAL_SQR_ON : CAL_SQR_MUTED;
  SubLevel = CAL_SUB_MUTED;
#ifdef ENABLE_CV_OUTS
  write_level_pwm();
#endif

#ifdef ENABLE_CV_OUTS
  const uint16_t cal_reso[NUM_FILTERS] = { CAL_RESONANCE_COMPARE, CAL_RESONANCE_COMPARE };
  write_cv_pwm_raw(CAL_CUTOFF_COMPARE, cal_reso, CAL_VCA_COMPARE, 0, 0);
#endif
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mem_diag.ino"
#include "include_all.h"
#include "mem_diag.h"

#ifdef ENABLE_MEM_DIAG

#include "hardware/sync.h"
#include <malloc.h>

#ifndef SRAM_BASE
#define SRAM_BASE 0x20000000u
#endif

// Linker symbols (Arduino-Pico / Pico SDK memmap). Heap span = StackLimit - bss_end.
// Arduino-Pico 6 already declares __bss_end__ / __StackLimit (char) and
// __scratch_x_start__ / __scratch_y_start__ (uint32_t) in RP2040Support.h.
extern char __bss_end__;
extern char __StackLimit;

// Present on some Pico SDK scripts when .time_critical is its own VMA. Weak: 0 if absent.
extern char __ram_text_start__ __attribute__((weak));
extern char __ram_text_end__ __attribute__((weak));

volatile bool mem_diag_runtime_enabled = true;
volatile bool mem_diag_pending = false;
volatile bool mem_diag_core1_ready = false;
static volatile int mem_diag_core1_free_stack = -1;

// PARAM_DEBUG_COMMAND 13: ask Core 1 for stack, then print from Core 0.
void mem_diag_request() {
  if (!mem_diag_runtime_enabled) {
    return;
  }
  mem_diag_core1_ready = false;
  __dmb();
  mem_diag_pending = true;
}

// loop1: instantaneous remaining stack on this core (not a high-water mark).
void mem_diag_poll_core1_work() {
  mem_diag_core1_free_stack = rp2040.getFreeStack();
  __dmb();
  mem_diag_core1_ready = true;
}

static int mem_diag_stack_used(int free_b, unsigned bank) {
  if (free_b < 0 || bank == 0u) {
    return -1;
  }
  if ((unsigned)free_b >= bank) {
    return 0;
  }
  return (int)(bank - (unsigned)free_b);
}

// loop: print once Core 1 has answered. Core 1 never Serial.print.
void mem_diag_poll_core0_work() {
  mem_diag_pending = false;
  mem_diag_core1_ready = false;

  const int heap_total = rp2040.getTotalHeap();
  const int heap_used = rp2040.getUsedHeap();
  const int heap_free = rp2040.getFreeHeap();
  const int core0_free = rp2040.getFreeStack();
  const int core1_free = mem_diag_core1_free_stack;
  const struct mallinfo mi = mallinfo();

  const uintptr_t sram_base = (uintptr_t)SRAM_BASE;
  const uintptr_t bss_end = (uintptr_t)&__bss_end__;
  const uintptr_t stack_limit = (uintptr_t)&__StackLimit;
  const unsigned long main_b = (stack_limit > sram_base) ? (unsigned long)(stack_limit - sram_base) : 0ul;
  const unsigned long static_b = (bss_end > sram_base) ? (unsigned long)(bss_end - sram_base) : 0ul;
  const unsigned static_pct = (main_b > 0ul) ? (unsigned)((static_b * 100ul) / main_b) : 0u;
  const unsigned heap_pct = (main_b > 0ul) ? (unsigned)(((unsigned long)heap_total * 100ul) / main_b) : 0u;

  const uintptr_t sx0 = (uintptr_t)&__scratch_x_start__;
  const uintptr_t sy0 = (uintptr_t)&__scratch_y_start__;
  unsigned bank = 0u;
  if (sy0 > sx0) {
    bank = (unsigned)(sy0 - sx0);
  }
  if (bank == 0u) {
    bank = 4096u;
  }
  const uintptr_t sx1 = sx0 + bank;
  const uintptr_t sy1 = sy0 + bank;
  const int core0_used = mem_diag_stack_used(core0_free, bank);
  const int core1_used = mem_diag_stack_used(core1_free, bank);

  Serial.println(F("=== DCO RAM ==="));
#if defined(PICO_RP2350)
  Serial.printf("mcu    RP2350  clk=%luMHz  polls=%s\n",
#else
  Serial.printf("mcu    RP2040  clk=%luMHz  polls=%s\n",
#endif
                (unsigned long)(rp2040.f_cpu() / 1000000ul),
                mem_diag_runtime_enabled ? "on" : "off");
  Serial.printf("sram   main=%lu static=%lu (%u%%) heap=%d (%u%%)\n",
                main_b, static_b, static_pct, heap_total, heap_pct);
  Serial.printf("heap   total=%d used=%d free=%d  arena=%d free_chunks=%d\n",
                heap_total, heap_used, heap_free, (int)mi.arena, (int)mi.ordblks);
  Serial.printf("stack  core0 %d free / %d used / %u   core1 %d free / %d used / %u\n",
                core0_free, core0_used, bank, core1_free, core1_used, bank);
  Serial.printf("layout sram=0x%08lx bss_end=0x%08lx stack_limit=0x%08lx\n",
                (unsigned long)sram_base, (unsigned long)bss_end, (unsigned long)stack_limit);
  Serial.printf("scratch_x=0x%08lx..0x%08lx (%u)  scratch_y=0x%08lx..0x%08lx (%u)\n",
                (unsigned long)sx0, (unsigned long)sx1, bank,
                (unsigned long)sy0, (unsigned long)sy1, bank);

  const uintptr_t rt0 = (uintptr_t)&__ram_text_start__;
  const uintptr_t rt1 = (uintptr_t)&__ram_text_end__;
  if (rt0 != 0 && rt1 != 0 && rt1 >= rt0) {
    Serial.printf("ram_text=%lu  (0x%08lx..0x%08lx)\n",
                  (unsigned long)(rt1 - rt0), (unsigned long)rt0, (unsigned long)rt1);
  } else {
    Serial.println(F("ram_text=(no __ram_text_* symbols — use the .map for .time_critical)"));
  }
  Serial.println(F("================"));
}

#endif  // ENABLE_MEM_DIAG

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/midi.ino"
// Mono held-key stack lives in the shared library (monoStack in
// voice_alloc_state.h); which entry sounds depends on the allocation mode,
// which doubles as the mono note priority.
// Voice_task porta still restarts only on note_on_flag →
// note_on_flag_flag from VOICE_NOTES (no pitch queue on Core1).
static constexpr uint8_t MONO_NOTE_NONE = VOICE_ALLOC_NONE;
// Pitch currently gated on voice 0, so a key that loses priority can be stacked
// silently instead of retriggering the envelope.
static uint8_t mono_sounding_note = MONO_NOTE_NONE;

// Empty held stack when entering mono so notes do not leak across voice modes.
void mono_note_stack_clear() {
  monoStack.clear();
  mono_sounding_note = MONO_NOTE_NONE;
}

// Register note/CC/program/pitch-bend handlers on USB + DIN MIDI. USB begin is in init_usb().
void init_midi() {
  MIDI_USB.setHandleNoteOn(handleNoteOn);
  MIDI_USB.setHandleNoteOff(handleNoteOff);
  MIDI_USB.setHandleControlChange(handleControlChange);
  MIDI_USB.setHandleProgramChange(handleProgramChange);
  MIDI_USB.setHandlePitchBend(handlePitchBend);
  MIDI_USB.setHandleAfterTouchChannel(handleAfterTouchChannel);

  MIDI_SERIAL.begin(MIDI_CHANNEL_OMNI);
  MIDI_SERIAL.setHandleNoteOn(handleNoteOn);
  MIDI_SERIAL.setHandleNoteOff(handleNoteOff);
  MIDI_SERIAL.setHandleControlChange(handleControlChange);
  MIDI_SERIAL.setHandleProgramChange(handleProgramChange);
  MIDI_SERIAL.setHandlePitchBend(handlePitchBend);
  MIDI_SERIAL.setHandleAfterTouchChannel(handleAfterTouchChannel);

  MIDI_USB.turnThruOff();
  MIDI_SERIAL.turnThruOff();
}


// MIDI library callback → note_on(). Invoked from loop via MIDI_*.read().
void handleNoteOn(byte channel, byte pitch, byte velocity) {
  note_on(pitch, velocity);
}
// MIDI library callback → note_off().
void handleNoteOff(byte channel, byte pitch, byte velocity) {
  note_off(pitch);
}

// MIDI CC handler: CC 0/32 latch preset bank, CC 42 sets pitch-bend range,
// everything else goes through the generated map in midi_cc_map.h.
void handleControlChange(byte channel, byte number, byte value) {
  (void)channel;
  // CC 1 (mod wheel) → mod matrix source 11.
  if (number == 1) {
    mod_matrix_set_mod_wheel(value);
    return;
  }
  // CC 0 (Bank Select MSB) / CC 32 (LSB): latch the upper/lower 128-slot bank
  // for the next Program Change. Nonzero = bank 1 (slots 128..255).
  if (number == 0 || number == 32) {
    midiPresetBank = (value != 0) ? 1 : 0;
    return;
  }
  // CC #42 is used to set the pitch bend range in semitones.
  if (number == MIDI_CC_PITCH_BEND_RANGE) {
    pitchBendRange = value;
    // Optimized: Use fast fixed-point multiplication instead of float division.
    pitchBendMultiplier_q24 = (int32_t)(((int64_t)pitchBendRange * RECIP_TWELVE_Q24));
    pitchBendMultiplier = (float)pitchBendMultiplier_q24 / (float)(1 << 24);
    return;
  }
  midi_cc_handle(number, value);
}

// Scale a controller into its parameter's native range and apply it. Unmapped CCs are
// ignored. Linear search over ~70 entries, which at MIDI's 3125 bytes/s is free.
void midi_cc_handle(uint8_t number, uint8_t value) {
  for (size_t i = 0; i < midiCcMapSize; ++i) {
    const MidiCcEntry& entry = midiCcMap[i];
    if (entry.cc != number) continue;

    int16_t scaled = entry.lo + (int16_t)(((int32_t)(entry.hi - entry.lo) * value + 63) / 127);
    if (entry.curve == MIDI_CC_EXP_TIME) {
      scaled = (int16_t)linearToExponential((uint16_t)scaled, MIDI_CC_EXP_BASE, MIDI_CC_EXP_MAX);
    }
    midi_cc_apply(entry.target, scaled);
    return;
  }
}

// Route a scaled value to its target. Table parameters take the normal router; ADSR/filter
// block values have no ParamId (1 ms packed 'a'–'d' frames), so they are written here
// exactly as input_handle_*() in Serial.ino writes them. PW and EnvVCA→VCA are ParamIds.
void midi_cc_apply(uint8_t target, int16_t value) {
  switch (target) {
    case CC_LOCAL_ADSR_VCA_ATTACK:      ADSR_VCA_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_A); break;
    case CC_LOCAL_ADSR_VCA_DECAY:       ADSR_VCA_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_D); break;
    case CC_LOCAL_ADSR_VCA_SUSTAIN:     ADSR_VCA_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_S); break;
    case CC_LOCAL_ADSR_VCA_RELEASE:     ADSR_VCA_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_R); break;

    case CC_LOCAL_ADSR_VCF_ATTACK:      ADSR_VCF_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_A); break;
    case CC_LOCAL_ADSR_VCF_DECAY:       ADSR_VCF_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_D); break;
    case CC_LOCAL_ADSR_VCF_SUSTAIN:     ADSR_VCF_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_S); break;
    case CC_LOCAL_ADSR_VCF_RELEASE:     ADSR_VCF_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_R); break;

    case CC_LOCAL_ADSR_DCO_ATTACK:      ADSR1_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_A); break;
    case CC_LOCAL_ADSR_DCO_DECAY:       ADSR1_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_D); break;
    case CC_LOCAL_ADSR_DCO_SUSTAIN:     ADSR1_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_S); break;
    case CC_LOCAL_ADSR_DCO_RELEASE:     ADSR1_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_R); break;

    // CUTOFF/RESONANCE are used live in update_CV_outs; only mod depths need scale bake.
    // Input 'd' bakes ADSR2+LFO2 once for the filter block (depths in the same payload).
    case CC_LOCAL_FILTER_CUTOFF:        CUTOFF     = (uint16_t)value; break;
    case CC_LOCAL_FILTER_RESONANCE:     RESONANCE  = (uint16_t)value; break;
    case CC_LOCAL_FILTER_ADSR2_TO_VCF:  ADSR2toVCF = value;           cv_bake_adsr2_to_vcf_scale(); break;
    case CC_LOCAL_FILTER_LFO2_TO_VCF:   LFO2toVCF  = (uint16_t)value; cv_bake_lfo2_to_vcf_scale(); break;

    default:
      update_parameters((uint16_t)target, value);
      serial_echo_persistable_param16(target, value);
      return;
  }

  // A block CC has no ParamId to echo, so mirror the whole block the way a
  // preset recall does; otherwise the panel and the Screen keep showing the
  // values they last sent. The CC_LOCAL_* codes are grouped per block.
  if (target <= CC_LOCAL_ADSR_VCA_RELEASE) {
    serial_send_adsr_vca_block_to_mb();
  } else if (target <= CC_LOCAL_ADSR_VCF_RELEASE) {
    serial_send_adsr_vcf_block_to_mb();
  } else if (target <= CC_LOCAL_ADSR_DCO_RELEASE) {
    serial_send_adsr_dco_block_to_mb();
  } else {
    serial_send_filter_block_to_mb();
  }
}

// MIDI program-change callback → recall preset slot
// (midiPresetBank * 128 + program), covering 0..255 (preset_store.ino).
void handleProgramChange(byte channel, byte program) {
  (void)channel;
  const uint16_t slot = (uint16_t)midiPresetBank * 128u + (uint16_t)program;
  if (slot < PRESET_NUM_SLOTS) {
    preset_store_load((uint8_t)slot);
  }
}

// MIDI pitch-bend callback → midi_pitch_bend (offset to 0..16383 style).
void handlePitchBend(byte channel, int pitchBend) {
  midi_pitch_bend = pitchBend + 8192;
}

// Channel aftertouch → mod matrix source.
void handleAfterTouchChannel(byte channel, byte pressure) {
  (void)channel;
  mod_matrix_set_aftertouch(pressure);
}

// Allocate voice(s) from MIDI note-on per voiceMode; set ADSR flags (EnvDCO/VCA/VCF are local).
// Para steal policy and mono note priority both come from the allocation mode.
// Mono: stack push, then VOICE_NOTES/gate/note_on_flag + noteStart (porta + ADSR).
void note_on(uint8_t note, uint8_t velocity) {
  const uint8_t alloc_mode = voiceAlloc.mode();

  switch (voiceMode) {
    case 0: {
      // push() denies the key under VOICE_ALLOC_NO_STEAL: while one is down the
      // rest are ignored outright, so they do not take over later either. That
      // is what separates mode 5 from first-note priority.
      if (!monoStack.push(note, alloc_mode)) return;

      const uint8_t winner = monoStack.pick(alloc_mode);
      // A key that loses priority (first/low/high modes) is held but stays silent.
      if (winner == MONO_NOTE_NONE || winner == mono_sounding_note) return;

      mod_matrix_on_note_on();
      mono_sounding_note = winner;
      voice_mark_on(0, winner, velocity);
      return;
    }

    case 1: {
      // Same-note retrigger: reuse the voice already on this pitch rather than
      // allocating a second one and stealing something else.
      const uint8_t held = voiceAlloc.findNote(note);
      if (held != VOICE_ALLOC_NONE) {
        mod_matrix_on_note_on();
        voice_mark_on(held, note, velocity);
        return;
      }

      const uint8_t voice_num = voice_alloc();
      if (voice_num == VOICE_ALLOC_NONE) return;  // VOICE_ALLOC_NO_STEAL: drop the note

      mod_matrix_on_note_on();
      voice_mark_on(voice_num, note, velocity);
      break;
    }

    case 2:
      // Mode 2 stub: same note on every capacity slot (DCO4 stack), so nothing
      // to allocate. Engine still only runs NUM_VOICES_VOICE_TASK.
      mod_matrix_on_note_on();
      for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
        voice_mark_on((uint8_t)i, note, velocity);
      }
      break;

    default:
      return;
  }
  last_midi_pitch_bend = 0;
}

// Release matching voice(s) on MIDI note-off; set noteEnd flags for the local envelopes.
// Mono: stack remove; empty → gate off; else fall back to the new priority winner
// + note_on_flag (porta, no ADSR retrigger).
void note_off(uint8_t note) {
  if (voiceMode == 0) {
    if (!monoStack.remove(note)) {
      return;  // note was not held
    }
    if (monoStack.empty()) {
      // Keep VOICE_NOTES so voice_task holds last pitch through release.
      mono_sounding_note = MONO_NOTE_NONE;
      voice_mark_off(0);
      return;
    }
    const uint8_t winner = monoStack.pick(voiceAlloc.mode());
    // Releasing a key that was never sounding leaves the gated pitch alone.
    if (winner == MONO_NOTE_NONE || winner == mono_sounding_note) return;

    // Still holding other keys: sound the new winner; porta via note_on_flag only.
    mono_sounding_note = winner;
    VOICE_NOTES[0] = winner;
    VOICES[0] = 1;
    voiceAlloc.regate(0, winner);
    note_on_flag[0] = 1;
    noteEnd[0] = 0;
    return;
  }

  // Para/poly: scan full capacity so a release is not missed if NUM_VOICES shrank mid-note.
  for (int i = 0; i < NUM_VOICES_TOTAL; i++)
  {
    if (VOICE_NOTES[i] == note && VOICES[i] != 0) {
      // Keep VOICE_NOTES so voice_task holds last pitch through release
      // (portaTime==0 snaps portamento_cur_freq from VOICE_NOTES each frame).
      voice_mark_off((uint8_t)i);
    }
  }
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/mod_matrix.ino"
#include "include_all.h"
#include <string.h>

static ModSlot g_mod_slots[MOD_SLOT_COUNT];
static int16_t mod_random_snh_q15 = 0;
static uint8_t mod_aftertouch = 0;
static uint8_t mod_wheel = 0;
// Bit i set when slot i has source, dest, nonzero depth. Hot path skips empty matrix.
static uint8_t g_mod_live_mask = 0;
static uint8_t g_mod_pitch_mask = 0;
static uint8_t g_mod_subosc_mask = 0;

static void mod_refresh_slot_live(uint8_t slot) {
  const ModSlot& s = g_mod_slots[slot];
  const uint8_t bit = (uint8_t)(1u << slot);
  const bool live = (s.source != MOD_SRC_EMPTY && s.dest != MOD_DEST_EMPTY && s.dest < MOD_DEST_COUNT &&
                     s.depth != 0);
  if (live) {
    g_mod_live_mask |= bit;
    if (s.dest == MOD_DEST_PITCH) {
      g_mod_pitch_mask |= bit;
    } else {
      g_mod_pitch_mask &= (uint8_t)~bit;
    }
    if (s.dest == MOD_DEST_SUB_PHASE || s.dest == MOD_DEST_SUB_PW) {
      g_mod_subosc_mask |= bit;
    } else {
      g_mod_subosc_mask &= (uint8_t)~bit;
    }
  } else {
    g_mod_live_mask &= (uint8_t)~bit;
    g_mod_pitch_mask &= (uint8_t)~bit;
    g_mod_subosc_mask &= (uint8_t)~bit;
  }
}

// |src_q15|≤32768, |depth|≤32767 → product fits int32 (no __aeabi_lmul).
static inline int32_t mod_depth_mul_q15(int32_t src_q15, int16_t depth) {
  return (src_q15 * (int32_t)depth) >> 15;
}

volatile int32_t matrix_pitch_mod_q24 = 0;

// Reciprocal of MOD_PITCH_DEPTH_FULL for Q24 octave: (pitch_s << 24) / 1023
// ≈ pitch_s * (2^24 / 1023). Constant = round(2^32 / 1023) used as (s * C) >> 32 after <<24...
// Simpler: (pitch_s * MOD_PITCH_TO_Q24_MUL) >> 15 with MUL = round(2^24 * 32768 / 1023).
static constexpr int32_t MOD_PITCH_TO_Q24_MUL =
  (int32_t)(((int64_t)1 << 24) * 32768 / (int64_t)MOD_PITCH_DEPTH_FULL);

static inline uint16_t mod_clamp_u16(int32_t v) {
  if (v < 0) return 0;
  if (v > 4095) return 4095;
  return (uint16_t)v;
}

static inline int32_t mod_clamp_q15(int32_t v) {
  if (v < -32768) return -32768;
  if (v > 32768) return 32768;
  return v;
}

void mod_matrix_init() {
  for (uint8_t i = 0; i < MOD_SLOT_COUNT; i++) {
    g_mod_slots[i].source = MOD_SRC_EMPTY;
    g_mod_slots[i].dest = MOD_DEST_EMPTY;
    g_mod_slots[i].depth = 0;
  }
  g_mod_live_mask = 0;
  g_mod_pitch_mask = 0;
  g_mod_subosc_mask = 0;
  subosc_mod_phase = 0;
  subosc_mod_pw = 0;
  mod_random_snh_q15 = 0;
  mod_aftertouch = 0;
  mod_wheel = 0;
  matrix_pitch_mod_q24 = 0;
}

void mod_matrix_set_source(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  if (v < 0 || v >= (int16_t)MOD_SRC_COUNT) {
    g_mod_slots[slot].source = MOD_SRC_EMPTY;
  } else {
    g_mod_slots[slot].source = (uint8_t)v;
  }
  mod_refresh_slot_live(slot);
}

void mod_matrix_set_dest(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  if (v < 0 || v >= (int16_t)MOD_DEST_COUNT) {
    g_mod_slots[slot].dest = MOD_DEST_EMPTY;
  } else {
    g_mod_slots[slot].dest = (uint8_t)v;
  }
  mod_refresh_slot_live(slot);
}

void mod_matrix_set_depth(uint8_t slot, int16_t v) {
  if (slot >= MOD_SLOT_COUNT) return;
  g_mod_slots[slot].depth = v;
  mod_refresh_slot_live(slot);
}

void mod_matrix_on_note_on() {
  // ±1.0 Q15; random(0..2000)-1000 → scale to ±32768.
  mod_random_snh_q15 = (int16_t)(((int32_t)random(0, 2001) - 1000) * 32);
}

void mod_matrix_set_aftertouch(uint8_t pressure) {
  mod_aftertouch = pressure;
}

void mod_matrix_set_mod_wheel(uint8_t value) {
  mod_wheel = value;
}

// Source as Q15 (±32768 ≈ ±1.0). LFO/ADSR/noise are already Q15 pass-through.
static int32_t mod_matrix_read_source_q15(uint8_t src, int16_t lfo1_q15, int16_t lfo2_q15) {
  switch (src) {
    case MOD_SRC_ADSR3:
      return (int32_t)ADSR1Level_q15[0];
    case MOD_SRC_ADSR4:
    case MOD_SRC_LFO3:
    case MOD_SRC_LFO4:
      return 0;
    case MOD_SRC_VELOCITY:
      return (int32_t)midi_velocity[0] * 258;
    case MOD_SRC_KEYTRACK: {
      const uint8_t note = VOICE_NOTES[0];
      if (note == 0) return 0;
      // 32768 / 48 ≈ 682
      return mod_clamp_q15(((int32_t)note - 60) * 682);
    }
    case MOD_SRC_RANDOM:
      return (int32_t)mod_random_snh_q15;
    case MOD_SRC_AFTERTOUCH:
      return (int32_t)mod_aftertouch * 258;
    case MOD_SRC_LFO1:
      return (int32_t)lfo1_q15;
    case MOD_SRC_LFO2:
      return (int32_t)lfo2_q15;
    case MOD_SRC_PITCH_BEND:
      return ((int32_t)midi_pitch_bend - 8192) << 2;
    case MOD_SRC_MOD_WHEEL:
      return (int32_t)mod_wheel * 258;
    case MOD_SRC_NOISE0:
    case MOD_SRC_NOISE1:
    case MOD_SRC_NOISE2:
    case MOD_SRC_NOISE3: {
      const uint8_t i = (uint8_t)(src - MOD_SRC_NOISE0);
      if (i >= NUM_NOISE_GENS) return 0;
      return (int32_t)noiseLevel[i];
    }
    default:
      return 0;
  }
}

void mod_matrix_accumulate(int32_t dest_sums[MOD_DEST_COUNT], int16_t lfo1_q15, int16_t lfo2_q15) {
  memset(dest_sums, 0, sizeof(int32_t) * MOD_DEST_COUNT);
  if (g_mod_live_mask == 0) {
    return;
  }

  uint8_t mask = g_mod_live_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
#ifndef ENABLE_CV_OUTS
    if (s.dest != MOD_DEST_PITCH) {
      continue;
    }
#endif
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    dest_sums[s.dest] += mod_depth_mul_q15(src_q15, s.depth);
  }
}

int32_t __not_in_flash_func(mod_matrix_eval_pitch_q24)(int16_t lfo1_q15, int16_t lfo2_q15) {
  if (g_mod_pitch_mask == 0) {
    return 0;
  }
  int32_t pitch_s = 0;
  uint8_t mask = g_mod_pitch_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    pitch_s += mod_depth_mul_q15(src_q15, s.depth);
  }
  return mod_matrix_pitch_to_q24(pitch_s);
}

// Sub phase and PW, left in matrix units for subosc_compute_words() to scale and clamp. Both
// are always assigned, so clearing the last slot that targeted them clears the offset too.
void __not_in_flash_func(mod_matrix_eval_subosc)(int16_t lfo1_q15, int16_t lfo2_q15) {
  int32_t phase_s = 0;
  int32_t pw_s = 0;
  uint8_t mask = g_mod_subosc_mask;
  for (uint8_t i = 0; mask != 0; i++, mask >>= 1) {
    if ((mask & 1u) == 0) {
      continue;
    }
    const ModSlot& s = g_mod_slots[i];
    const int32_t src_q15 = mod_matrix_read_source_q15(s.source, lfo1_q15, lfo2_q15);
    const int32_t amount = mod_depth_mul_q15(src_q15, s.depth);
    if (s.dest == MOD_DEST_SUB_PHASE) {
      phase_s += amount;
    } else {
      pw_s += amount;
    }
  }
  subosc_mod_phase = phase_s;
  subosc_mod_pw = pw_s;
}

// Convert clamped pitch dest sum (±1023) to Q24 octave without a hot divide.
static inline int32_t mod_pitch_sum_to_q24(int32_t pitch_s) {
  if (pitch_s > MOD_PITCH_DEPTH_FULL) pitch_s = MOD_PITCH_DEPTH_FULL;
  if (pitch_s < -MOD_PITCH_DEPTH_FULL) pitch_s = -MOD_PITCH_DEPTH_FULL;
  return (int32_t)(((int64_t)pitch_s * (int64_t)MOD_PITCH_TO_Q24_MUL) >> 15);
}

// Convert clamped pitch dest sum (±1023) to Q24 octave without a hot divide.
int32_t mod_matrix_pitch_to_q24(int32_t pitch_s) {
  return mod_pitch_sum_to_q24(pitch_s);
}

void mod_matrix_apply_cv(const int32_t dest_sums[MOD_DEST_COUNT], uint16_t* dist_drive_out,
                         uint16_t* dist_mix_out) {
#ifdef ENABLE_CV_OUTS
  write_level_pwm_raw(
    mod_clamp_u16((int32_t)OSC1Level - dest_sums[MOD_DEST_OSC1_LEVEL]),
    mod_clamp_u16((int32_t)OSC2Level - dest_sums[MOD_DEST_OSC2_LEVEL]),
    mod_clamp_u16((int32_t)OSC3Level - dest_sums[MOD_DEST_OSC3_LEVEL]),
    mod_clamp_u16((int32_t)SubLevel - dest_sums[MOD_DEST_SUB_LEVEL]));

  RESONANCE_PWM[0] = mod_clamp_u16((int32_t)RESONANCE + dest_sums[MOD_DEST_VCF1_RESO]);
  RESONANCE_PWM[1] = mod_clamp_u16((int32_t)RESONANCE + dest_sums[MOD_DEST_VCF2_RESO]);
#else
  (void)dest_sums;
#endif

  if (dist_drive_out) {
    *dist_drive_out = mod_clamp_u16((int32_t)DIST_DRIVE + dest_sums[MOD_DEST_DIST_DRIVE]);
  }
  if (dist_mix_out) {
    *dist_mix_out = mod_clamp_u16((int32_t)DIST_MIX + dest_sums[MOD_DEST_DIST_MIX]);
  }
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/params.ino"
// Central parameter router for shared parameters.
//
// This module maps numeric parameter IDs (used over Serial / between MCUs)
// to concrete synth state changes on this MCU. It is the single place where
// "what does parameter X actually do?" is implemented.
//
// The stable parameter ID definitions live in params_def.h so all MCUs and
// tools can share the same mapping.
//
// High-level flow:
//   1) Some control source (front panel, STM32, MIDI, editor) decides that
//      parameter P should change to value V.
//   2) It sends P and V over the link (Serial 'p', or a 1 ms ADSR/filter block).
//   3) The receiver ends up calling:
//         update_parameters(paramNumber, paramValue);
//   4) update_parameters() looks up paramNumber in paramTable[] and calls
//      the corresponding apply_param_*() function.
//   5) That function updates internal state and performs any required
//      precomputations (fixed-point scales, LFO frequencies, etc.).
//
// How to add or modify a parameter on this MCU:
//   1) Define or reuse a ParamId in params_def.h.
//   2) Implement a new apply_param_*() function below that:
//        - Accepts int16_t (the raw transport value).
//        - Updates the appropriate globals / DSP structures.
//        - Computes any derived values (e.g. Q24 scales, Hz values).
//   3) Add an entry to paramTable[] that maps your ParamId to the new
//      apply_param_*() function.
//   4) Make sure the sending side (other MCU / UI) uses the same ParamId,
//      and sends values in the range/format your apply function expects.
//
// Notes:
//   - The transport is 16-bit (int16_t) for this router.
//   - If an unknown paramNumber is received, update_parameters() simply
//     ignores it.


// ---- Apply functions for each parameter (invoked only via paramTable / update_parameters) ----

// Per-osc Saw/Pulse/Tri enables → 74HC595 / DG411 (see docs/WAVE_MUX.md).
static void apply_wave_enable(uint8_t osc, uint8_t wave, int16_t v) {
  if (osc > 2 || wave > 2) return;
  waveEnable[osc][wave] = (v != 0);
  update_waveSelector();
}

static void apply_param_osc1_saw_enable(int16_t v) { apply_wave_enable(0, 0, v); }
static void apply_param_osc1_pulse_enable(int16_t v) { apply_wave_enable(0, 1, v); }
static void apply_param_osc1_tri_enable(int16_t v) { apply_wave_enable(0, 2, v); }
static void apply_param_osc2_saw_enable(int16_t v) { apply_wave_enable(1, 0, v); }
static void apply_param_osc2_pulse_enable(int16_t v) { apply_wave_enable(1, 1, v); }
static void apply_param_osc2_tri_enable(int16_t v) { apply_wave_enable(1, 2, v); }
static void apply_param_osc3_saw_enable(int16_t v) { apply_wave_enable(2, 0, v); }
static void apply_param_osc3_pulse_enable(int16_t v) { apply_wave_enable(2, 1, v); }
static void apply_param_osc3_tri_enable(int16_t v) { apply_wave_enable(2, 2, v); }

// PARAM_SINE_STATUS: deprecated — no mux role.
static void apply_param_sine_status(int16_t v) {
  (void)v;
}

static void apply_param_resonance_comp(int16_t v) {
  RESONANCEAmpCompensation = (v != 0);
}

static void apply_param_vca_adsr_restart(int16_t v) {
  VCAADSRRestart = (v != 0);
  ADSR_VCA_set_restart();
}

static void apply_param_vcf_adsr_restart(int16_t v) {
  VCFADSRRestart = (v != 0);
  ADSR_VCF_set_restart();
}

// PARAM_ADSR3_TO_OSC_SELECT: which osc(s) receive ADSR3→detune/PWM routing.
static void apply_param_adsr3_to_osc_select(int16_t v) {
  ADSR3ToOscSelect = v;
}

// PARAM_LFO1_WAVEFORM: set LFO1 waveform and refresh rate.
static void apply_param_lfo1_waveform(int16_t v) {
  LFO1Waveform = v;
  LFO1_class.setWaveForm(LFO1Waveform);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

// PARAM_LFO2_WAVEFORM: set LFO2 waveform and refresh rate.
static void apply_param_lfo2_waveform(int16_t v) {
  LFO2Waveform = v;
  LFO2_class.setWaveForm(LFO2Waveform);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

// PARAM_OSC1_INTERVAL (13): global octave_shift (semitones). Kept id for wire compat.
static void apply_param_octave_shift(int16_t v) {
  octave_shift = v;
}

// PARAM_OSC2_INTERVAL: OSC2 transpose interval.
static void apply_param_osc2_interval(int16_t v) {
  OSC2_interval = v;
}

// PARAM_OSC3_INTERVAL: OSC3 transpose interval.
static void apply_param_osc3_interval(int16_t v) {
  OSC3_interval = v;
}

// PARAM_OSC2_DETUNE_VAL: OSC2 fine detune (stored inverted from UI value).
static void apply_param_osc2_detune_val(int16_t v) {
  OSC2DetuneVal = 512 - v;
}

// PARAM_OSC3_DETUNE_VAL: OSC3 fine detune (stored inverted from UI value).
static void apply_param_osc3_detune_val(int16_t v) {
  OSC3DetuneVal = 512 - v;
}

static void apply_param_lfo2_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO2_PITCH_DEPTH_SCALE);
}

// PARAM_LFO2_TO_OSC2: LFO2 → OSC2 pitch depth (Q24).
static void apply_param_lfo2_to_osc2(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC2_q24);
}

// PARAM_LFO2_TO_OSC3: LFO2 → OSC3 pitch depth (Q24).
static void apply_param_lfo2_to_osc3(int16_t v) {
  apply_param_lfo2_to_osc_depth(v, LFO2toOSC3_q24);
}

// LFO2 coarse pitch depth (0..511; same travel scale as LFO1 pitch).
static void apply_param_lfo2_to_osc_coarse_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint16_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo2_to_osc2_coarse(int16_t v) {
  apply_param_lfo2_to_osc_coarse_depth(v, LFO2toOSC2_coarse_q24);
}

static void apply_param_lfo2_to_osc3_coarse(int16_t v) {
  apply_param_lfo2_to_osc_coarse_depth(v, LFO2toOSC3_coarse_q24);
}

// PARAM_CHARACTER: master scale (0..128) for Character-tab noise jitters (see character_jitter.h).
static void apply_param_character(int16_t v) {
  character = (uint8_t)constrain((int)v, 0, 128);
  character_recompute_scales();
}

// PARAM_OSC_SYNC_MODE: osc sync / phase-align (updates phaseAlignOSC2, retriggers notes).
// oscSync also gates the note-on restart in voices.ino: 0 leaves the oscillators running
// through note-on (free running), 1 stops and restarts OSC1 and OSC2 together with no
// offset, and above 1 adds an OSC2 phase offset on top (2..8 = 45..315 degrees, >8 = v * 2).
static void apply_param_osc_sync_mode(int16_t v) {
  oscSync = v;
  if (oscSync < 2) {
    // Live phase-align no longer widens Y; still reload the plain pulse in case a
    // previous note left a leftover exact-split remainder or an old firmware Y.
    phaseAlignOSC2 = 0;
    pio_defer_request_reset_pulse_all();
  } else {
    if (oscSync > 8) {
      phaseAlignOSC2 = oscSync * 2;
    } else {
      switch (oscSync) {
        case 2:
          phaseAlignOSC2 = 45;
          break;
        case 3:
          phaseAlignOSC2 = 90;
          break;
        case 4:
          phaseAlignOSC2 = 135;
          break;
        case 5:
          phaseAlignOSC2 = 180;
          break;
        case 6:
          phaseAlignOSC2 = 225;
          break;
        case 7:
          phaseAlignOSC2 = 270;
          break;
        case 8:
          phaseAlignOSC2 = 315;
          break;
        default:
          break;
      }
    }
  }
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

// PARAM_PORTAMENTO_TIME: map UI → TIME fixed duration (µs) and SLEW octave period (µs/12st).
static void apply_param_portamento_time(int16_t v) {
  portamento_parameter_value = (uint8_t)v;
  if (portamento_parameter_value == 0) {
    portamento_time_fixed = 0;
    portamento_time_slew = 0;
  } else if (portamento_parameter_value < 200) {
    // Shared smaller minimum (~1 ms at v=1).
    const uint32_t t = (uint32_t)expConverter(portamento_parameter_value + 15, 100) * 500u;
    portamento_time_fixed = t;
    portamento_time_slew = t;
  } else {
    // TIME max 10 s (any interval); SLEW max 20 s per octave.
    portamento_time_fixed =
        (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 10000000);
    portamento_time_slew =
        (uint32_t)map(portamento_parameter_value, 200, 255, 1000000, 20000000);
  }
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

static void apply_param_vcf_keytrack(int16_t v) {
  VCFKeytrack = v;
#ifdef USE_FLOAT_CV_OUTS
  if (VCFKeytrack != 0) {
    VCFKeytrackModifier = (float)VCFKeytrack / 8000.0f;
  } else {
    VCFKeytrackModifier = 1.0f;
  }
#else
  if (VCFKeytrack != 0) {
    VCFKeytrackModifier_q15 = ((int32_t)VCFKeytrack * 32768) / 8000;
  } else {
    VCFKeytrackModifier_q15 = 32768;
  }
#endif
}

static void apply_param_velocity_to_vcf(int16_t v) {
  velocityToVCFVal = (int8_t)v;
#ifdef USE_FLOAT_CV_OUTS
  velocityToVCF = velocityToVCFVal * 0.0003935f;
#else
  // ≈ val * 0.0003935 * 32768
  velocityToVCF_q15 = ((int32_t)velocityToVCFVal * 825) >> 6;
#endif
}

static void apply_param_velocity_to_vca(int16_t v) {
  velocityToVCAVal = (int8_t)v;
#ifdef USE_FLOAT_CV_OUTS
  velocityToVCA = velocityToVCAVal * 0.0003935f;
#else
  velocityToVCA_q15 = ((int32_t)velocityToVCAVal * 825) >> 6;
#endif
}

// Panel bases only — PWM written from mod_matrix_apply_cv() in update_CV_outs.
static void apply_param_osc1_level(int16_t v) {
  OSC1LevelVal = v;
  if (OSC1LevelVal < 0) OSC1LevelVal = 0;
  if (OSC1LevelVal > 128) OSC1LevelVal = 128;
  OSC1Level = lin_to_log_128[OSC1LevelVal];
}

static void apply_param_osc2_level(int16_t v) {
  OSC2LevelVal = v;
  if (OSC2LevelVal < 0) OSC2LevelVal = 0;
  if (OSC2LevelVal > 128) OSC2LevelVal = 128;
  OSC2Level = lin_to_log_128[OSC2LevelVal];
}

static void apply_param_osc3_level(int16_t v) {
  OSC3LevelVal = v;
  if (OSC3LevelVal < 0) OSC3LevelVal = 0;
  if (OSC3LevelVal > 128) OSC3LevelVal = 128;
  OSC3Level = lin_to_log_128[OSC3LevelVal];
}

static void apply_param_sub_level(int16_t v) {
  SubLevelVal = v;
  SubLevel = (uint16_t)constrain((int)SubLevelVal * 32, 0, 4095);
}

#define DECL_MOD_SLOT_APPLIERS(N) \
  static void apply_param_mod_slot##N##_source(int16_t v) { mod_matrix_set_source(N, v); } \
  static void apply_param_mod_slot##N##_dest(int16_t v) { mod_matrix_set_dest(N, v); } \
  static void apply_param_mod_slot##N##_depth(int16_t v) { mod_matrix_set_depth(N, v); }

DECL_MOD_SLOT_APPLIERS(0)
DECL_MOD_SLOT_APPLIERS(1)
DECL_MOD_SLOT_APPLIERS(2)
DECL_MOD_SLOT_APPLIERS(3)
DECL_MOD_SLOT_APPLIERS(4)
DECL_MOD_SLOT_APPLIERS(5)
DECL_MOD_SLOT_APPLIERS(6)
DECL_MOD_SLOT_APPLIERS(7)

#undef DECL_MOD_SLOT_APPLIERS

// PARAM_PORTAMENTO_MODE: 0 = TIME (fixed duration any interval),
// else SLEW (constant semitone rate; one octave = portamento_time_slew).
static void apply_param_portamento_mode(int16_t v) {
  portamento_mode = (v == 0) ? PORTA_MODE_TIME : PORTA_MODE_SLEW;
  portamento_time = (portamento_mode == PORTA_MODE_TIME)
                        ? portamento_time_fixed
                        : portamento_time_slew;
}

// PARAM_CALIBRATION_VALUE: reserved ID (no behavior).
static void apply_param_calibration_value(int16_t /*v*/) {
  // Placeholder: original code did nothing but kept the ID reserved.
}

// PARAM_VOICE_MODE: mono/poly/stack → setVoiceMode().
static void apply_param_voice_mode(int16_t v) {
  voiceMode = v;
  setVoiceMode();
}

// PARAM_VOICE_ALLOC_MODE: para steal policy / mono note priority (VoiceAllocMode).
static void apply_param_voice_alloc_mode(int16_t v) {
  if (v < 0) return;
  voiceAlloc.setMode((uint8_t)v);
}

// PARAM_UNISON_DETUNE: unison detune amount.
static void apply_param_unison_detune(int16_t v) {
  unisonDetune = v;
}

// PARAM_ANALOG_DRIFT_AMOUNT: drift modulation depth.
static void apply_param_analog_drift_amount(int16_t v) {
  analogDrift = v;
  // Full-scale Q15 → same travel as legacy (cc_level * unit * analogDrift).
  drift_pitch_scale_q24 =
    (int32_t)((int32_t)analogDrift * DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE);
#ifndef USE_FLOAT_CV_OUTS
  vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}

// PARAM_ANALOG_DRIFT_SPEED: recompute all drift LFO rates.
static void apply_param_analog_drift_speed(int16_t v) {
  analogDriftSpeed = v;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_SPEED_OFFSET[i] =
      (float)(1.00f - (float)((float)analogDriftSpread * 0.005f) +
              (float)((float)analogDriftSpread * 0.00125f * (float)i)) *
      (float)expConverterFloat((float)analogDriftSpeed, 5000);
    LFO_DRIFT_CLASS[i].setMode0Freq(LFO_DRIFT_SPEED_OFFSET[i], micros());
  }
}

// PARAM_ANALOG_DRIFT_SPREAD: recompute per-osc drift speed offsets.
static void apply_param_analog_drift_spread(int16_t v) {
  analogDriftSpread = v;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_SPEED_OFFSET[i] =
      (float)(1.00f - (float)((float)analogDriftSpread * 0.005f) +
              (float)((float)analogDriftSpread * 0.00125f * (float)i)) *
      (float)expConverterFloat((float)analogDriftSpeed, 5000);
    LFO_DRIFT_CLASS[i].setMode0Freq(LFO_DRIFT_SPEED_OFFSET[i], micros());
  }
}

// PARAM_SYNC_MODE: PIO sync topology → setSyncMode() (deferred to core 1).
static void apply_param_sync_mode(int16_t v) {
  if (manualCalibrationFlag) {
    // The walk needs every oscillator to own its own RESET pin, so a preset load
    // mid-walk only books the topology for when manual cal exits.
    manualCalSavedSyncMode = (uint8_t)v;
    return;
  }
  syncMode = v;
  pio_defer_request_sync_mode();
}

// PARAM_SOFT_SYNC: 0 = hard sync (sideset, weight 4); 1..3 = soft sync with that many
// trailing polled chunks (weights 5/6/7, receptive ~40%/67%/86%). Changing among 1..3
// reloads the poll program image on pio0.
static void apply_param_soft_sync(int16_t v) {
  if (v < 0) v = 0;
  if (v > 3) v = 3;
  if (manualCalibrationFlag) {
    manualCalSavedSoftSyncChunks = (uint8_t)v;
    return;
  }
  softSyncChunks = (uint8_t)v;
  pio_defer_request_sync_mode();
}

// PARAM_SUBOSC_DIVIDE: sub-oscillator divide ratio off / 2 / 4.
static void apply_param_subosc_divide(int16_t v) {
  uint8_t divide = 0;
  if (v >= 4) {
    divide = 4;
  } else if (v >= 2) {
    divide = 2;
  }
  pio_defer_request_subosc(divide);
}

// PARAM_SUB1/2_DIVIDE / _MASTER / _PHASE / _WIDTH and the combiner operator. Ranges, clamping
// and the core-0 handoff all live in subosc.h; without ENABLE_SUBOSC_ENGINE2 sub 1's divide
// drives the single legacy sub and the rest are ignored.
static void apply_param_sub1_divide(int16_t v) { subosc_param_divide(0, v); }
static void apply_param_sub2_divide(int16_t v) { subosc_param_divide(1, v); }
static void apply_param_sub1_master(int16_t v) { subosc_param_master(0, v); }
static void apply_param_sub2_master(int16_t v) { subosc_param_master(1, v); }
static void apply_param_sub1_phase(int16_t v) { subosc_param_phase(0, v); }
static void apply_param_sub2_phase(int16_t v) { subosc_param_phase(1, v); }
static void apply_param_sub1_width(int16_t v) { subosc_param_width(0, v); }
static void apply_param_sub2_width(int16_t v) { subosc_param_width(1, v); }
static void apply_param_sub_logic_op(int16_t v) { subosc_param_logic_op(v); }

// PARAM_LFO1_TO_DCO: LFO1 → DCO detune depth (full-scale Q24 for Q15 wave).
static void apply_param_lfo1_to_dco(int16_t v) {
  LFO1toDCOVal = v;
  float lfo1_amt = (float)expConverterFloat(LFO1toDCOVal, 500) / 275000.0f;
  LFO1toDCO_q24 = lfo_pitch_depth_q24(lfo1_amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc_depth(int16_t v, int32_t& depth_q24) {
  float amt = (float)expConverterFloat((uint8_t)v, 500) / 275000.0f;
  depth_q24 = lfo_pitch_depth_q24(amt, LFO1_PITCH_DEPTH_SCALE);
}

static void apply_param_lfo1_to_osc1(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC1_q24);
}

static void apply_param_lfo1_to_osc2(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC2_q24);
}

static void apply_param_lfo1_to_osc3(int16_t v) {
  apply_param_lfo1_to_osc_depth(v, LFO1toOSC3_q24);
}

// PARAM_LFO1_SPEED: LFO1 rate in Hz (via expConverterFloat).
static void apply_param_lfo1_speed(int16_t v) {
  LFO1SpeedVal = v;
  LFO1Speed = expConverterFloat(LFO1SpeedVal, 5000);
  LFO1_class.setMode0Freq((float)LFO1Speed, micros());
}

// PARAM_LFO2_SPEED: LFO2 rate in Hz (via expConverterFloat).
static void apply_param_lfo2_speed(int16_t v) {
  LFO2SpeedVal = v;
  LFO2Speed = expConverterFloat(LFO2SpeedVal, 5000);
  LFO2_class.setMode0Freq((float)LFO2Speed, micros());
}

// PARAM_VCA_LEVEL: panel sends 0..128; scale to the 12-bit CV domain.
static void apply_param_vca_level(int16_t v) {
  VCALevel = (uint16_t)constrain((int)v * 32, 0, 4095);
}

// PARAM_DIST_DRIVE / PARAM_DIST_MIX: post-LP distortion CVs (0..4095).
static void apply_param_dist_drive(int16_t v) {
  DIST_DRIVE = (uint16_t)constrain((int)v, 0, 4095);
}

static void apply_param_dist_mix(int16_t v) {
  DIST_MIX = (uint16_t)constrain((int)v, 0, 4095);
}

// PARAM_FILTER_MODE: AS3320 multimode index (0..N). Pin drive is solo-B / not ENABLE_VOICE_AUX.
static void apply_param_filter_mode(int16_t v) {
  FILTER_MODE = (uint8_t)constrain((int)v, 0, 255);
}

static void apply_param_lfo1_to_vca(int16_t v) {
  LFO1toVCA = (uint16_t)constrain((int)v, 0, 4095);
  cv_bake_lfo1_to_vca_scale();
}

// PARAM_LFO2_TO_PW: LFO2 → pulse-width depth (PWM counts at full-scale Q15).
static void apply_param_lfo2_to_pw(int16_t v) {
  LFO2toPW = (int16_t)v;
}

// PARAM_ADSR3_TO_PWM: ADSR → PWM depth (centered around 512).
// Precompute full-scale PWM counts so hot path is (level_q15 * scale) >> 15.
// Legacy: (level_u12 * depth) >> 11 with level 0..ADSR_1_CC.
static void apply_param_adsr1_to_pwm(int16_t v) {
  ADSR1toPWM = (int16_t)v - 512;
  ADSR1toPWM_scale = (int32_t)(((int64_t)ADSR1toPWM * (int64_t)ADSR_1_CC) >> 11);
}

// PARAM_ADSR3_TO_DETUNE1: ADSR → pitch depth (exp on knob, linear env hot).
// Full CW × full env → ADSR_PITCH_MAX_OCTAVES (tune in LFO.h).
static void apply_param_adsr1_to_detune1(int16_t v) {
  ADSR1toDETUNE1 = (int16_t)v;
  if (ADSR1toDETUNE1 == 0) {
    ADSR1toDETUNE1_scale_q24 = 0;
  } else {
    const uint16_t mag_u =
      (ADSR1toDETUNE1 < 0) ? (uint16_t)(-ADSR1toDETUNE1) : (uint16_t)ADSR1toDETUNE1;
    const float mag = expConverterFloat(mag_u, 500);
    const float mag_full = expConverterFloat(ADSR_PITCH_DEPTH_PANEL_FULL, 500);
    float norm = (mag_full > 0.0f) ? (mag / mag_full) : 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const float signed_oct = (ADSR1toDETUNE1 < 0) ? -norm : norm;
    ADSR1toDETUNE1_scale_q24 =
      (int32_t)(signed_oct * ADSR_PITCH_MAX_OCTAVES * (float)(1 << 24) +
                ((signed_oct >= 0.0f) ? 0.5f : -0.5f));
  }
}

// PARAM_ADSR3_PITCH_MODE: EnvDCO → pitch tap. 0 unipolar (default), 1 centered.
static void apply_param_adsr3_pitch_mode(int16_t v) {
  env_dco_pitch_centered = (v != 0) ? 1 : 0;
}

// PARAM_ADSR1_ATTACK_CURVE / DECAY: EnvVCA curve shape.
static void apply_param_adsr1_attack_curve(int16_t v) {
  ADSR1AttackCurveVal = (uint8_t)v;
  ADSR_VCA_change_attack_curve(ADSR1AttackCurveVal);
}

static void apply_param_adsr1_decay_curve(int16_t v) {
  ADSR1DecayCurveVal = (uint8_t)v;
  ADSR_VCA_change_decay_curve(ADSR1DecayCurveVal);
}

// PARAM_ADSR2_ATTACK_CURVE / DECAY: EnvVCF curve shape.
static void apply_param_adsr2_attack_curve(int16_t v) {
  ADSR2AttackCurveVal = (uint8_t)v;
  ADSR_VCF_change_attack_curve(ADSR2AttackCurveVal);
}

static void apply_param_adsr2_decay_curve(int16_t v) {
  ADSR2DecayCurveVal = (uint8_t)v;
  ADSR_VCF_change_decay_curve(ADSR2DecayCurveVal);
}

// PARAM_PW_VALUE: pulse width 0..4095; voice engine uses PW[0] at /4 scale.
static void apply_param_pw_value(int16_t v) {
  uint16_t pwRaw = (uint16_t)constrain((int)v, 0, 4095);
  PW[0] = pwRaw / 4;
}

// PARAM_ADSR1_TO_VCA: EnvVCA → VCA amount (was the 'e' block).
static void apply_param_adsr1_to_vca(int16_t v) {
  ADSR1toVCA = v;
}

// PARAM_PWM_POTS_CONTROL_MANUAL: manual PWM pot control flag.
static void apply_param_pwm_pots_manual(int16_t v) {
  PWMPotsControlManual = (v != 0);
}

static void apply_param_adsr3_enabled(int16_t v) {
  ADSR3Enabled = (v != 0);
}

// PARAM_FUNCTION_KEY: reserved / handled elsewhere.
static void apply_param_function_key(int16_t /*v*/) {
}

// Gap is generated by DCO (autotune) and TX'd via serialSendParam32 → Screen.
static void apply_param_gap_from_dco(int16_t /*v*/) {
}

// PARAM_CALIBRATION_FLAG: start/stop auto-cal (loop1 runs DCO_calibration when
// set). The value selects the stage and how carefully it measures:
// 1 = amp-comp only, 2 = PW only, 3 = full at NORMAL precision (build from
// scratch, fast); 5/6/7 = the same three stages at FINE precision, where the
// amp stage re-measures the stored table instead of building a new one;
// 9/10/11 = the same stages at FAST precision (quickest from-scratch build,
// for a testing table).
static void apply_param_calibration_flag(int16_t v) {
  if (v == 0) {
    // Runs on core 0 while DCO_calibration() may be blocking core 1: request
    // a cancel; the calibration loops poll this and unwind cleanly.
    calibrationCancelRequested = true;
    calibrationFlag = false;
    return;
  }

  if (v >= 9 && v <= 11) {
    calibrationPrecision = CAL_PRECISION_FAST;
    v -= 8;
  } else if (v >= 5 && v <= 7) {
    calibrationPrecision = CAL_PRECISION_FINE;
    v -= 4;
  } else {
    calibrationPrecision = CAL_PRECISION_NORMAL;
  }

  calibrationScope = (v == CAL_SCOPE_AMP || v == CAL_SCOPE_PW)
                       ? (uint8_t)v
                       : (uint8_t)CAL_SCOPE_FULL;
  calibrationFlag = true;
}

// PARAM_MANUAL_CALIBRATION_FLAG: enter/exit manual cal; rising edge TX offsets to Input/Mainboard.
static void apply_param_manual_calibration_flag(int16_t v) {
  // When manual calibration is active, both flags follow this param.
  // Rising edge (0 -> non-zero): broadcast current offsets upstream (Input hub or Mainboard).
  if (v != 0 && !manualCalibrationFlag) {
    // The solo stops every other SM, which a synced pair cannot survive, so the
    // walk runs a neutral topology (see manualCalSavedSyncMode in autotune.h).
    manualCalSavedSyncMode = syncMode;
    manualCalSavedSoftSyncChunks = softSyncChunks;
    if (syncMode != 0) {
      Serial.println((String)"[MANUAL_CAL] sync neutralised: syncMode " + syncMode +
                     " -> 0, softSyncChunks " + softSyncChunks + " -> 0 (restored on exit)");
    }
    syncMode = 0;
    softSyncChunks = 0;
    calSyncNeutralRequested = true;

    for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
      uint8_t idx    = osc;
      uint8_t offset = (uint8_t)manualCalibrationOffset[osc];
      uint16_t packed = ((uint16_t)idx << 8) | offset;
      // Send as 32-bit frame; receivers use lower 16 bits [index:8|offset:8].      
      serialSendParam32(PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO, (uint32_t)packed);
    }
  }

  // Falling edge: manual cal forced the SQR levels and wave mux — restore panel state.
  // It also left every un-soloed oscillator's SM stopped and the PW channels at 0,
  // which only core 1 may undo (PIO), so that part goes through the deferred queue.
  if (v == 0 && manualCalibrationFlag) {
    // Hand the sync topology back before the restore below: its start_voice_sms()
    // is what rebuilds the SMs from syncMode / softSyncChunks.
    syncMode = manualCalSavedSyncMode;
    softSyncChunks = manualCalSavedSoftSyncChunks;
    calSyncNeutralRequested = false;
    apply_param_osc1_level(OSC1LevelVal);
    apply_param_osc2_level(OSC2LevelVal);
    apply_param_osc3_level(OSC3LevelVal);
    apply_param_sub_level(SubLevelVal);
    update_waveSelector();
    pio_defer_request_cal_restore();
  }

  // Every manual-cal entry starts at the trimpot step; the UI switches to the
  // 440 Hz step explicitly via PARAM_MANUAL_CALIBRATION_STEP.
  if (v != 0 && !manualCalibrationFlag) {
    manualCalibrationStep = 0;
  }

  manualCalibrationFlag = v;
  calibrationFlag       = v;
}

// PARAM_MANUAL_CALIBRATION_STEP: 0 = trimpot stage at the low starting note,
// 1 = 440 Hz amp-set stage (adjust PARAM_AMP_COMP_440 until duty = 50%).
static void apply_param_manual_calibration_step(int16_t v) {
  manualCalibrationStep = (v != 0) ? 1 : 0;
}

// PARAM_AMP_COMP_440: absolute range-PWM at 440 Hz for the oscillator selected
// by manualCalibrationStage (osc = stage / 3). Persisted by STORE.
static void apply_param_amp_comp_440(int16_t v) {
  uint8_t osc = cal_manual_osc();
  if (v < 0) v = 0;
  uint16_t val = (uint16_t)v;
  if (val > DIV_COUNTER) val = DIV_COUNTER;
  ampComp440[osc] = val;
}

// PARAM_AMP_COMP_DUTY_OFFSET: duty target trim (hundredths of a percent) for
// the oscillator selected by manualCalibrationStage. Nulls the difference
// between the sense pin's 50% and the scope's 50%; both amp-comp methods and
// the manual duty readout aim at 50% + this value. Persisted by
// PARAM_MANUAL_CALIBRATION_STORE.
static void apply_param_amp_comp_duty_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  if (v < -500) v = -500;
  if (v >  500) v =  500;
  ampCompDutyOffset[osc] = v;
}

// PARAM_CAL_PW_CENTER: live PW_CENTER for the calibrated oscillator's PW channel.
static void apply_param_cal_pw_center(int16_t v) {
  const uint8_t ch = cal_pw_channel(cal_manual_osc());
  if (v < 0) v = 0;
  if (v > (int16_t)CAL_PW_CENTER_MAX) v = (int16_t)CAL_PW_CENTER_MAX;
  PW_CENTER[ch] = (uint16_t)v;
}

// PARAM_MANUAL_CALIBRATION_STAGE: packed walk (see cal_stage_*_n). Do not
// clamp to NUM_OSCILLATORS-1 — that collapsed DCO3 stages 3–5 onto osc 2.
static void apply_param_manual_calibration_stage(int16_t v) {
  int16_t stage = v;
  if (stage < 0) stage = 0;
  const int16_t maxStage = (int16_t)cal_stage_max();
  if (stage > maxStage) stage = maxStage;
  manualCalibrationStage = (uint8_t)stage;
  manualCalibrationStep = cal_stage_is_440((uint8_t)stage) ? 1 : 0;
  if (cal_stage_is_440((uint8_t)stage)) {
    serialSendParam16(PARAM_AMP_COMP_440, (int16_t)ampComp440[cal_manual_osc()], true);
  } else if (cal_stage_is_pw_edit((uint8_t)stage)) {
    serialSendParam16(PARAM_CAL_PW_CENTER,
                      (int16_t)PW_CENTER[cal_pw_channel(cal_manual_osc())], true);
  }
}

// PARAM_MANUAL_CALIBRATION_OFFSET: per-osc manual amp offset for current osc.
static void apply_param_manual_calibration_offset(int16_t v) {
  uint8_t osc = cal_manual_osc();
  manualCalibrationOffset[osc] = (int8_t)v;
}

// Explicit "store manual calibration" command. This is called when the user
// confirms manual calibration, and is the only place where we persist
// manualCalibrationOffset[], ampComp440[] and ampCompDutyOffset[] to the
// filesystem.
static void apply_param_manual_calibration_store(int16_t /*v*/) {
  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    update_FS_ManualCalibrationOffset(osc, manualCalibrationOffset[osc]);
    update_FS_AmpComp440(osc, ampComp440[osc]);
    update_FS_AmpCompDutyOffset(osc, ampCompDutyOffset[osc]);
  }
  for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ++ch) {
    update_FS_PWCenter(ch, PW_CENTER[ch]);
  }
}

// Preset store commands (see preset_store.h / preset_store.ino). These are
// command triggers, not patch state: preset_param_is_persistable() excludes
// them, so they are never captured into the shadow or stored in a record.
static void apply_param_preset_save(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_save((uint8_t)v);
}

static void apply_param_preset_load(int16_t v) {
  if (v >= 0 && v < (int16_t)PRESET_NUM_SLOTS) preset_store_load((uint8_t)v);
}

static void apply_param_preset_dump(int16_t v) {
  preset_store_dump(v);
}

static void apply_param_cal_dump(int16_t v) {
  preset_store_cal_dump(v);
}

// PARAM_DEBUG_COMMAND: bench / debug opcodes for tools/dco_control (Diagnostics + Calibration
// + Character). See params_def.h near PARAM_DEBUG_COMMAND for the opcode list.
//
// Values 200..50000 (uint16 on the wire) set pioPulseLength and reload running SMs
// via pio_defer_request_reset_pulse_all(). Small opcodes 1..30 stay below that range.
//
// Packed Character-tab jitter setters (unsigned 16-bit, hi|lo with lo in 0..128):
//   0xC8xx ampCompJitter, 0xCAxx pitchJitter, 0xCBxx pulsewidthJitter.
// These sit above 50000 so they do not collide with the pioPulseLength window.
//
// The period probe parks an oscillator at a fixed clk_div, so it only holds while no
// note is playing — voice_task_main() pushes a fresh divider every frame for a held note.
//
// 10 / 11 / 12 drive the profiler in bench.h and only do anything in a RUNNING_AVERAGE
// build. The dump is asynchronous: it asks both cores for a snapshot and core 0 prints
// once both have answered, so this handler never blocks the audio core.
// 13 dumps heap + per-core stack (mem_diag; needs ENABLE_MEM_DIAG; runtime polls on).
// 14 / 15 disable / enable mem_diag loop polls (A/B vs profiler without rebuild).
// 43 / 44 MCP4728 probe / reattach when ENABLE_MCP4728; else prints compiled-out.
//  4 dumps the sub engine: each sub's master, segment words, DMA channels, FIFO level and why
// the logic combiner is or is not running. Says so and stops without ENABLE_SUBOSC_ENGINE2.
static void apply_param_debug_command(int16_t v) {
  // Wire may pack unsigned 16-bit (param16u); reinterpret before small-opcode switch.
  uint32_t n = (uint16_t)v;
  uint8_t hi = (uint8_t)(n >> 8);
  uint8_t lo = (uint8_t)n;
  if ((hi == 0xC8u || hi == 0xCAu || hi == 0xCBu) && lo <= 128u) {
    switch (hi) {
      case 0xC8: ampCompJitter = lo; break;
      case 0xCA: pitchJitter = lo; break;
      case 0xCB: pulsewidthJitter = lo; break;
      default: break;
    }
    character_recompute_scales();
    return;
  }
  if (n >= 200u && n <= 50000u) {
    pioPulseLength = n;
    // Reload Y into running SMs on core 1 (same path as clearing phase-align).
    pio_defer_request_reset_pulse_all();
    return;
  }

  switch (v) {
    case 1:
      pio_topology_report();
      break;
    case 2:
      pio_period_probe(0, 2000);
      break;
    case 3:
      pio_period_probe(0, 20000);
      break;
    case 4:
      subosc2_report();
      break;
#ifdef ENABLE_MEM_DIAG
    case 13:
      if (!mem_diag_runtime_enabled) {
#ifdef RUNNING_AVERAGE
        bench_out_reset();
        bench_out_printf("mem_diag polls=off\n");
        bench_out_active = true;
#else
        Serial.println("mem_diag polls=off");
#endif
      } else {
        mem_diag_request();
      }
      break;
    case 14:
      mem_diag_runtime_enabled = false;
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=off\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=off");
#endif
      break;
    case 15:
      mem_diag_runtime_enabled = true;
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=on\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=on");
#endif
      break;
#else
    case 13:
    case 14:
    case 15:
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mem_diag polls=compiled out\n");
      bench_out_active = true;
#else
      Serial.println("mem_diag polls=compiled out");
#endif
      break;
#endif
#ifdef RUNNING_AVERAGE
    case 10:
      bench_dump_request = true;
      break;
    case 11:
      bench_reset_all();
      break;
    case 12:
      bench_periodic = !bench_periodic;
      bench_out_reset();
      bench_out_printf("bench periodic %s\n", bench_periodic ? "on" : "off");
      bench_out_active = true;
      break;
#endif
#ifdef ENABLE_MCP4728
    case 43:
      mcp_dac_probe();
      break;
    case 44:
      mcp_dac_reattach();
      break;
#else
    case 43:
    case 44:
#ifdef RUNNING_AVERAGE
      bench_out_reset();
      bench_out_printf("mcp4728 compiled out\n");
      bench_out_active = true;
#else
      Serial.println("mcp4728 compiled out");
#endif
      break;
#endif
    // Amp-comp live method (USE_FLOAT_AMP_COMP). Ack via paced Board pane when
    // RUNNING_AVERAGE (same path as profiler / amp benches); else Serial.
    // Reset profiler so amp-comp means are not dominated by the previous method.
    case 20:
      amp_comp_set_method(AMP_COMP_FLOAT_QUAD);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
    case 21:
      amp_comp_set_method(AMP_COMP_LUT);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
    case 22:
      amp_comp_set_method(AMP_COMP_FIXED);
#ifdef RUNNING_AVERAGE
      bench_reset_all();
      amp_comp_method_ack_pending = true;
#else
      Serial.print("amp_comp method=");
      Serial.println(amp_comp_method_name(amp_comp_method));
#endif
      break;
#if defined(RUNNING_AVERAGE) && defined(AMP_COMP_BENCHMARK)
    case 24:
      amp_comp_bench_speed_pending = true;
      break;
    case 25:
      amp_comp_bench_accuracy_pending = true;
      break;
#endif
#ifdef RUNNING_AVERAGE
    case 28:
      pitch_interp_bench_speed_pending = true;
      break;
    case 29:
      pitch_interp_bench_accuracy_pending = true;
      break;
    case 32:
      clkdiv_hp_bench_speed_pending = true;
      break;
    case 33:
      clkdiv_hp_bench_accuracy_pending = true;
      break;
#endif
    // Force-write fake amp-comp + PW tables (dev placeholder; dco_control Calibration).
    case 30:
      seed_fake_calibration_tables(true);
      break;
    // Read-only [CAL_VERIFY] pass over the stored tables. Every probe blocks on
    // a duty measurement, so core 1 runs it from loop1(); this only asks.
    case 36:
      calibrationVerifyRequested = true;
      break;
    // PW CV probe: needs manual cal running, since the soloed oscillator's
    // pulse on the cal-sense pin is what tells us the CV arrived.
    case 46:
      if (!manualCalibrationFlag) {
        Serial.println("[PW_PROBE] start manual calibration first (param 151), "
                       "on a pulse (PW) substage");
      } else {
        pwCvProbeRequested = true;
      }
      break;
    // Amp-comp calibration method A/B (runtime-only, classic is boot default).
    case 34:
    case 35:
      autotuneAmpMethod = (v == 35) ? AMP_METHOD_FREQ_TRACE : AMP_METHOD_CLASSIC;
      Serial.printf("autotuneAmpMethod=%s (%s)\n",
                    autotune_amp_method_name(autotuneAmpMethod),
                    (autotuneAmpMethod == AMP_METHOD_FREQ_TRACE)
                      ? "fixed-PWM freq bisection"
                      : "per-note PWM search");
      break;
    // Frequency-search convergence A/B: how find_freq_for_duty50() picks the
    // next probe once the answer is bracketed. Compare on the probes= and
    // elapsed= figures in the [CAL_REPORT] footer.
    case 37:
    case 38:
    case 39:
      autotuneSearchMode = (v == 37) ? SEARCH_BISECT
                         : (v == 38) ? SEARCH_INTERP
                                     : SEARCH_GATED;
      Serial.printf("autotuneSearchMode=%s (%s)\n",
                    autotune_search_mode_name(autotuneSearchMode),
                    (autotuneSearchMode == SEARCH_BISECT)
                      ? "geometric midpoint, sign only"
                      : (autotuneSearchMode == SEARCH_INTERP)
                          ? "Illinois secant in log-frequency"
                          : "secant above the noise, midpoint below");
      break;
    // Amp-comp-0 endpoint A/B: hunt for the lowest reachable frequency live,
    // or skip the hunt and store the least-squares fit through the bottom
    // rungs (runtime-only, MEASURE is the boot default).
    case 40:
    case 41:
      autotuneAmp0Mode = (v == 41) ? AMP0_MODE_CALC : AMP0_MODE_MEASURE;
      Serial.printf("autotuneAmp0Mode=%s (%s)\n",
                    autotune_amp0_mode_name(autotuneAmp0Mode),
                    (autotuneAmp0Mode == AMP0_MODE_CALC)
                      ? "store the bottom-rung fit, no live hunt"
                      : "scan + bounded search at amp comp 0");
      break;
    // Note-on sync retrigger A/B (oscSync >= 1): EXACT_Y vs SYNC_JMP.
    case 26:
      note_retrig_set_mode(NOTE_RETRIG_EXACT_Y);
#ifdef RUNNING_AVERAGE
      note_retrig_mode_ack_pending = true;
#else
      Serial.print("note_retrig=");
      Serial.println(note_retrig_mode_name(note_retrig_mode));
#endif
      break;
    case 27:
      note_retrig_set_mode(NOTE_RETRIG_SYNC_JMP);
#ifdef RUNNING_AVERAGE
      note_retrig_mode_ack_pending = true;
#else
      Serial.print("note_retrig=");
      Serial.println(note_retrig_mode_name(note_retrig_mode));
#endif
      break;
    default:
      break;
  }
}

// ---- Parameter table ------------------------------------------------

static const ParamDescriptorT<int16_t> paramTable[] = {
  { PARAM_OSC1_SAW_ENABLE,           apply_param_osc1_saw_enable },
  { PARAM_OSC1_PULSE_ENABLE,         apply_param_osc1_pulse_enable },
  { PARAM_OSC1_TRI_ENABLE,           apply_param_osc1_tri_enable },
  { PARAM_OSC2_SAW_ENABLE,           apply_param_osc2_saw_enable },
  { PARAM_OSC2_PULSE_ENABLE,         apply_param_osc2_pulse_enable },
  { PARAM_OSC2_TRI_ENABLE,           apply_param_osc2_tri_enable },
  { PARAM_OSC3_SAW_ENABLE,           apply_param_osc3_saw_enable },
  { PARAM_OSC3_PULSE_ENABLE,         apply_param_osc3_pulse_enable },
  { PARAM_OSC3_TRI_ENABLE,           apply_param_osc3_tri_enable },
  { PARAM_SINE_STATUS,               apply_param_sine_status },
  { PARAM_RESONANCE_COMPENSATION,    apply_param_resonance_comp },
  { PARAM_VCA_ADSR_RESTART,          apply_param_vca_adsr_restart },
  { PARAM_VCF_ADSR_RESTART,          apply_param_vcf_adsr_restart },
  { PARAM_ADSR3_TO_OSC_SELECT,       apply_param_adsr3_to_osc_select },
  { PARAM_LFO1_WAVEFORM,             apply_param_lfo1_waveform },
  { PARAM_LFO2_WAVEFORM,             apply_param_lfo2_waveform },
  { PARAM_OSC1_INTERVAL,             apply_param_octave_shift },
  { PARAM_OSC2_INTERVAL,             apply_param_osc2_interval },
  { PARAM_OSC3_INTERVAL,             apply_param_osc3_interval },
  { PARAM_OSC2_DETUNE_VAL,           apply_param_osc2_detune_val },
  { PARAM_OSC3_DETUNE_VAL,           apply_param_osc3_detune_val },
  { PARAM_LFO2_TO_OSC2,              apply_param_lfo2_to_osc2 },
  { PARAM_LFO2_TO_OSC3,              apply_param_lfo2_to_osc3 },
  { PARAM_LFO2_TO_OSC2_COARSE,       apply_param_lfo2_to_osc2_coarse },
  { PARAM_LFO2_TO_OSC3_COARSE,       apply_param_lfo2_to_osc3_coarse },
  { PARAM_CHARACTER,                 apply_param_character },
  { PARAM_OSC_SYNC_MODE,             apply_param_osc_sync_mode },
  { PARAM_PORTAMENTO_TIME,           apply_param_portamento_time },
  { PARAM_PORTAMENTO_MODE,           apply_param_portamento_mode },
  { PARAM_VCF_KEYTRACK,              apply_param_vcf_keytrack },
  { PARAM_VELOCITY_TO_VCF,           apply_param_velocity_to_vcf },
  { PARAM_VELOCITY_TO_VCA,           apply_param_velocity_to_vca },
  { PARAM_OSC1_LEVEL,                apply_param_osc1_level },
  { PARAM_OSC2_LEVEL,                apply_param_osc2_level },
  { PARAM_OSC3_LEVEL,                apply_param_osc3_level },
  { PARAM_SUB_LEVEL,                 apply_param_sub_level },
  { PARAM_CALIBRATION_VALUE,         apply_param_calibration_value },
  { PARAM_VOICE_MODE,                apply_param_voice_mode },
  { PARAM_VOICE_ALLOC_MODE,          apply_param_voice_alloc_mode },
  { PARAM_UNISON_DETUNE,             apply_param_unison_detune },
  { PARAM_ANALOG_DRIFT_AMOUNT,       apply_param_analog_drift_amount },
  { PARAM_ANALOG_DRIFT_SPEED,        apply_param_analog_drift_speed },
  { PARAM_ANALOG_DRIFT_SPREAD,       apply_param_analog_drift_spread },
  { PARAM_SYNC_MODE,                 apply_param_sync_mode },
  { PARAM_SOFT_SYNC,                 apply_param_soft_sync },
  { PARAM_SUBOSC_DIVIDE,             apply_param_subosc_divide },
  { PARAM_SUB1_DIVIDE,               apply_param_sub1_divide },
  { PARAM_SUB2_DIVIDE,               apply_param_sub2_divide },
  { PARAM_SUB1_MASTER,               apply_param_sub1_master },
  { PARAM_SUB2_MASTER,               apply_param_sub2_master },
  { PARAM_SUB1_PHASE,                apply_param_sub1_phase },
  { PARAM_SUB2_PHASE,                apply_param_sub2_phase },
  { PARAM_SUB1_WIDTH,                apply_param_sub1_width },
  { PARAM_SUB2_WIDTH,                apply_param_sub2_width },
  { PARAM_SUB_LOGIC_OP,              apply_param_sub_logic_op },
  { PARAM_LFO1_TO_DCO,               apply_param_lfo1_to_dco },
  { PARAM_LFO1_TO_OSC1,              apply_param_lfo1_to_osc1 },
  { PARAM_LFO1_TO_OSC2,              apply_param_lfo1_to_osc2 },
  { PARAM_LFO1_TO_OSC3,              apply_param_lfo1_to_osc3 },
  { PARAM_LFO1_SPEED,                apply_param_lfo1_speed },
  { PARAM_LFO2_SPEED,                apply_param_lfo2_speed },
  { PARAM_VCA_LEVEL,                 apply_param_vca_level },
  { PARAM_LFO1_TO_VCA,               apply_param_lfo1_to_vca },
  { PARAM_LFO2_TO_PW,                apply_param_lfo2_to_pw },
  { PARAM_ADSR3_TO_PWM,              apply_param_adsr1_to_pwm },
  { PARAM_ADSR3_TO_DETUNE1,          apply_param_adsr1_to_detune1 },
  { PARAM_ADSR3_PITCH_MODE,          apply_param_adsr3_pitch_mode },
  { PARAM_ADSR1_ATTACK_CURVE,        apply_param_adsr1_attack_curve },
  { PARAM_ADSR1_DECAY_CURVE,         apply_param_adsr1_decay_curve },
  { PARAM_ADSR2_ATTACK_CURVE,        apply_param_adsr2_attack_curve },
  { PARAM_ADSR2_DECAY_CURVE,         apply_param_adsr2_decay_curve },
  { PARAM_DIST_DRIVE,                apply_param_dist_drive },
  { PARAM_DIST_MIX,                  apply_param_dist_mix },
  { PARAM_FILTER_MODE,               apply_param_filter_mode },
  { PARAM_MOD_SLOT0_SOURCE,          apply_param_mod_slot0_source },
  { PARAM_MOD_SLOT0_DEST,            apply_param_mod_slot0_dest },
  { PARAM_MOD_SLOT0_DEPTH,           apply_param_mod_slot0_depth },
  { PARAM_MOD_SLOT1_SOURCE,          apply_param_mod_slot1_source },
  { PARAM_MOD_SLOT1_DEST,            apply_param_mod_slot1_dest },
  { PARAM_MOD_SLOT1_DEPTH,           apply_param_mod_slot1_depth },
  { PARAM_MOD_SLOT2_SOURCE,          apply_param_mod_slot2_source },
  { PARAM_MOD_SLOT2_DEST,            apply_param_mod_slot2_dest },
  { PARAM_MOD_SLOT2_DEPTH,           apply_param_mod_slot2_depth },
  { PARAM_MOD_SLOT3_SOURCE,          apply_param_mod_slot3_source },
  { PARAM_MOD_SLOT3_DEST,            apply_param_mod_slot3_dest },
  { PARAM_MOD_SLOT3_DEPTH,           apply_param_mod_slot3_depth },
  { PARAM_MOD_SLOT4_SOURCE,          apply_param_mod_slot4_source },
  { PARAM_MOD_SLOT4_DEST,            apply_param_mod_slot4_dest },
  { PARAM_MOD_SLOT4_DEPTH,           apply_param_mod_slot4_depth },
  { PARAM_MOD_SLOT5_SOURCE,          apply_param_mod_slot5_source },
  { PARAM_MOD_SLOT5_DEST,            apply_param_mod_slot5_dest },
  { PARAM_MOD_SLOT5_DEPTH,           apply_param_mod_slot5_depth },
  { PARAM_MOD_SLOT6_SOURCE,          apply_param_mod_slot6_source },
  { PARAM_MOD_SLOT6_DEST,            apply_param_mod_slot6_dest },
  { PARAM_MOD_SLOT6_DEPTH,           apply_param_mod_slot6_depth },
  { PARAM_MOD_SLOT7_SOURCE,          apply_param_mod_slot7_source },
  { PARAM_MOD_SLOT7_DEST,            apply_param_mod_slot7_dest },
  { PARAM_MOD_SLOT7_DEPTH,           apply_param_mod_slot7_depth },
  { PARAM_PW_VALUE,                  apply_param_pw_value },
  { PARAM_ADSR1_TO_VCA,              apply_param_adsr1_to_vca },
  { PARAM_PWM_POTS_CONTROL_MANUAL,   apply_param_pwm_pots_manual },
  { PARAM_ADSR3_ENABLED,             apply_param_adsr3_enabled },
  { PARAM_FUNCTION_KEY,              apply_param_function_key },
  { PARAM_CALIBRATION_FLAG,          apply_param_calibration_flag },
  { PARAM_MANUAL_CALIBRATION_FLAG,   apply_param_manual_calibration_flag },
  { PARAM_MANUAL_CALIBRATION_STAGE,  apply_param_manual_calibration_stage },
  { PARAM_MANUAL_CALIBRATION_OFFSET, apply_param_manual_calibration_offset },
  { PARAM_MANUAL_CALIBRATION_STEP,   apply_param_manual_calibration_step },
  { PARAM_AMP_COMP_440,              apply_param_amp_comp_440 },
  { PARAM_CAL_PW_CENTER,             apply_param_cal_pw_center },
  { PARAM_AMP_COMP_DUTY_OFFSET,      apply_param_amp_comp_duty_offset },
  { PARAM_GAP_FROM_DCO,              apply_param_gap_from_dco },
  { PARAM_MANUAL_CALIBRATION_STORE,  apply_param_manual_calibration_store },
  { PARAM_PRESET_SAVE,               apply_param_preset_save },
  { PARAM_PRESET_LOAD,               apply_param_preset_load },
  { PARAM_PRESET_DUMP,               apply_param_preset_dump },
  { PARAM_CAL_DUMP,                  apply_param_cal_dump },
  { PARAM_DEBUG_COMMAND,             apply_param_debug_command }
};

static const size_t paramTableSize =
  sizeof(paramTable) / sizeof(paramTable[0]);

static void (*paramApplyJump[PARAM_ROUTER_JUMP_SIZE])(int16_t);

void init_param_router() {
  param_router_build_jump(paramApplyJump, paramTable, paramTableSize);
}

// Public entry point: called from Serial/MIDI/UI code.
inline void update_parameters(uint16_t paramNumber, int16_t paramValue) {
  // Shadow every persistable param so presets can be captured without read-back.
  preset_shadow_capture(paramNumber, paramValue);
  param_router_apply_jump(paramApplyJump, paramNumber, paramValue);
}



#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/pitch_interp_bench.ino"
#include "include_all.h"

// Pitch-interpolator speed / accuracy benches. Self-contained: private tables and
// interpolators (does not touch live voice storage). Needs RUNNING_AVERAGE only for
// paced bench_out_* TX. Debug cmds 28 / 29.

#if defined(RUNNING_AVERAGE)

volatile bool pitch_interp_bench_speed_pending = false;
volatile bool pitch_interp_bench_accuracy_pending = false;

enum : uint8_t {
  PITCH_BENCH_FLOAT = 0,       // legacy walk + bsearch
  PITCH_BENCH_FLOAT_FAST,      // trunc+clamp±1 find (matches live _fast)
  PITCH_BENCH_RATIO_Q16,
  PITCH_BENCH_Q12,
  PITCH_BENCH_METHODS,         // speed rows: FLOAT / FLOAT_FAST / RATIO / Q12
  PITCH_BENCH_Q20_REF = 5      // accuracy-only private int reference (not a live mode)
};

static constexpr int PIB_SIZE = 200;
static constexpr int32_t PIB_SCALE = 10000;
// One osc is enough to rank methods (per-DCO cache is identical). Cuts wall time ~3×.
static constexpr int PIB_BENCH_OSCS = 1;

// Private tables (filled once on first bench run).
static float   pib_xF[PIB_SIZE];
static float   pib_yF[PIB_SIZE];
static float   pib_slopeF[PIB_SIZE - 1];
static int32_t pib_x[PIB_SIZE];           // Q12 ×10000 table-units
static int32_t pib_y[PIB_SIZE];
static int32_t pib_slopeQ12[PIB_SIZE - 1];
static int32_t pib_xQ16[PIB_SIZE];        // live RATIO: native Q16 (1.0 = 65536)
static int32_t pib_yQ16[PIB_SIZE];
static int32_t pib_slopeQ16[PIB_SIZE - 1];
static int32_t pib_slopeQ20[PIB_SIZE - 1]; // accuracy oracle on native Q16 knots (not live)
static int16_t pib_cache[NUM_OSCILLATORS];
static bool    pib_ready = false;

// Modifier-domain grid (matches live FLOAT).
// seq: step 0.0001 = walk-favorable (hit / +1 segment). Accuracy cmd 29 uses this.
// jump: step 0.05 ≈ 2.5 segments — miss-heavy; ranks closer to live under pitch mod.
static constexpr float PITCH_BENCH_MOD_MIN = -1.0f;
static constexpr float PITCH_BENCH_MOD_MAX = 3.0f;
static constexpr float PITCH_BENCH_MOD_STEP = 0.0001f;       // seq (+ accuracy)
static constexpr float PITCH_BENCH_MOD_STEP_JUMP = 0.05f;
// Fixed-voice Q12 speed grid: ×10000 table-units.
static constexpr int32_t PIB_XINT_MIN = -PIB_SCALE;       // -10000
static constexpr int32_t PIB_XINT_MAX = 3 * PIB_SCALE;    //  30000
static constexpr int32_t PIB_XINT_STEP_SEQ = 1;
static constexpr int32_t PIB_XINT_STEP_JUMP =
    (int32_t)(PITCH_BENCH_MOD_STEP_JUMP * (float)PIB_SCALE + 0.5f); // 500
// Fixed-voice RATIO speed grid: native xQ16 (live clamp domain).
static constexpr int32_t PIB_XQ16_MIN = -65536;   // -1.0
static constexpr int32_t PIB_XQ16_MAX = 196608;   //  3.0
static constexpr int32_t PIB_XQ16_STEP_SEQ = 7;     // ≈0.0001
static constexpr int32_t PIB_XQ16_STEP_JUMP = 3277; // ≈0.05

// |cents| histogram for p50/p95/p99 (linear bins over 0..HIST_MAX).
static constexpr int PIB_HIST_BINS = 64;
static constexpr double PIB_HIST_MAX_CENTS = 2.0;

static const char *pitch_bench_method_name(uint8_t m) {
  switch (m) {
    case PITCH_BENCH_FLOAT:       return "FLOAT";
    case PITCH_BENCH_FLOAT_FAST:  return "FLOAT_FAST";
    case PITCH_BENCH_RATIO_Q16:   return "RATIO_Q16";
    case PITCH_BENCH_Q12:         return "Q12";
    default:                      return "?";
  }
}

static const char *pitch_bench_live_mode_name() {
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
  return "FLOAT";
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  return "FLOAT_FAST";
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return "RATIO_Q16";
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  return "Q12";
#else
  return "?";
#endif
}

static void pib_reset_cache() {
  for (int d = 0; d < NUM_OSCILLATORS; ++d) pib_cache[d] = -1;
}

static void pib_ensure_tables() {
  if (pib_ready) return;

  double fraction = 4.00 / (double)PIB_SIZE;
  for (int i = 0; i < PIB_SIZE; i++) {
    double x;
    float y_value;
    if (i == 0) {
      x = -1.00;
      y_value = 0.25f;
    } else if (i == PIB_SIZE - 1) {
      x = 3.0;
      y_value = 4.0f;
    } else {
      x = (-1.00 + (fraction * (double)i));
      y_value = (float)expInterpolationSolveY(x + 1.00, 1.00, 3.00, 0.50, 2.00);
    }
    // FLOAT: natural modifier / ratio. Q12: ×10000. RATIO: native Q16 (1.0 = 65536).
    pib_xF[i] = (float)x;
    pib_yF[i] = y_value;
    pib_x[i]  = (int32_t)(x * (double)PIB_SCALE);
    pib_y[i]  = (int32_t)(y_value * (double)PIB_SCALE);
    pib_xQ16[i] = (int32_t)(x * 65536.0 + (x >= 0.0 ? 0.5 : -0.5));
    pib_yQ16[i] = (int32_t)((double)y_value * 65536.0 + 0.5);
  }

  for (int i = 0; i < PIB_SIZE - 1; ++i) {
    int32_t dx12 = pib_x[i + 1] - pib_x[i];
    if (dx12 == 0) dx12 = 1;
    int32_t dy12 = pib_y[i + 1] - pib_y[i];
    pib_slopeQ12[i] = (int32_t)((((int64_t)dy12 << 12) + (dx12 > 0 ? dx12 / 2 : -dx12 / 2)) / (int64_t)dx12);

    int32_t dx16 = pib_xQ16[i + 1] - pib_xQ16[i];
    if (dx16 == 0) dx16 = 1;
    int32_t dy16 = pib_yQ16[i + 1] - pib_yQ16[i];
    pib_slopeQ16[i] = (int32_t)((((int64_t)dy16 << 16) + (dx16 > 0 ? dx16 / 2 : -dx16 / 2)) / (int64_t)dx16);
    // Non-live accuracy oracle: Q20 lerp on the same native Q16 knots.
    pib_slopeQ20[i] = (int32_t)((((int64_t)dy16 << 20) + (dx16 > 0 ? dx16 / 2 : -dx16 / 2)) / (int64_t)dx16);

    float dxF = pib_xF[i + 1] - pib_xF[i];
    if (dxF == 0.0f) dxF = 1.0f;
    pib_slopeF[i] = (pib_yF[i + 1] - pib_yF[i]) / dxF;
  }

  pib_reset_cache();
  pib_ready = true;
}

static int pib_find_seg_int(const int32_t *xTab, int32_t x, int dcoIndex) {
  int low = pib_cache[dcoIndex];
  if (low < 0 || low > PIB_SIZE - 2 || !(xTab[low] <= x && x < xTab[low + 1])) {
    if (low >= 0 && low < PIB_SIZE - 1) {
      if (x >= xTab[low + 1]) {
        while (low < PIB_SIZE - 2 && x >= xTab[low + 1]) low++;
      } else if (x < xTab[low]) {
        while (low > 0 && x < xTab[low]) low--;
      }
    }
    if (!(low >= 0 && low < PIB_SIZE - 1 && xTab[low] <= x && x < xTab[low + 1])) {
      int l = 0, h = PIB_SIZE - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xTab[m] <= x && x < xTab[m + 1]) {
          low = m;
          break;
        } else if (x < xTab[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > PIB_SIZE - 2) low = PIB_SIZE - 2;
    }
    pib_cache[dcoIndex] = (int16_t)low;
  }
  return low;
}

// modifier in [-1,3] → frequency ratio (natural float tables).
// Legacy segment find: walk from cache, then binary search (previous FLOAT).
static float pib_interp_float(float modifier, int dcoIndex) {
  if (modifier <= pib_xF[0]) return pib_yF[0];
  if (modifier >= pib_xF[PIB_SIZE - 1]) return pib_yF[PIB_SIZE - 1];

  int low = pib_cache[dcoIndex];
  if (low < 0 || low > PIB_SIZE - 2 ||
      !(pib_xF[low] <= modifier && modifier < pib_xF[low + 1])) {
    if (low >= 0 && low < PIB_SIZE - 1) {
      if (modifier >= pib_xF[low + 1]) {
        while (low < PIB_SIZE - 2 && modifier >= pib_xF[low + 1]) ++low;
      } else if (modifier < pib_xF[low]) {
        while (low > 0 && modifier < pib_xF[low]) --low;
      }
    }
    if (!(low >= 0 && low < PIB_SIZE - 1 &&
          pib_xF[low] <= modifier && modifier < pib_xF[low + 1])) {
      int l = 0, h = PIB_SIZE - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (pib_xF[m] <= modifier && modifier < pib_xF[m + 1]) {
          low = m;
          break;
        } else if (modifier < pib_xF[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > PIB_SIZE - 2) low = PIB_SIZE - 2;
    }
    pib_cache[dcoIndex] = (int16_t)low;
  }
  return pib_yF[low] + pib_slopeF[low] * (modifier - pib_xF[low]);
}

// Same lerp as pib_interp_float; find matches live interpolateRatioFloat_cached_fast
// (cache hit or trunc+clamp±1, plain lerp). noinline matches live codegen isolation.
__attribute__((noinline))
static float pib_interp_float_fast(float modifier, int dcoIndex) {
  if (modifier <= -1.0f) return pib_yF[0];
  if (modifier >= 3.0f) return pib_yF[PIB_SIZE - 1];

  static constexpr float kPitchX0 = -1.0f;
  static constexpr float kPitchInvDx = (float)PIB_SIZE / 4.0f;
  const int lastSeg = PIB_SIZE - 2;

  int low = pib_cache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      pib_xF[low] <= modifier && modifier < pib_xF[low + 1]) {
    // cache hit
  } else {
    int cand = (int)((modifier - kPitchX0) * kPitchInvDx);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
    if (cand < lastSeg && modifier >= pib_xF[cand + 1]) ++cand;
    else if (cand > 0 && modifier < pib_xF[cand]) --cand;
    low = cand;
    pib_cache[dcoIndex] = (int16_t)low;
  }
  return pib_yF[low] + pib_slopeF[low] * (modifier - pib_xF[low]);
}

// Live interpolateRatioQ16_cached clone: native Q16 x/y, slopeQ16, trunc±1 find,
// fused ratio return (y is already frequency ratio Q16).
static int32_t pib_interp_ratio_q16(int32_t xQ16, int dcoIndex) {
  static constexpr int32_t kPitchX0_Q16 = -65536;
  static constexpr int32_t kPitchX1_Q16 = 196608;
  if (xQ16 <= kPitchX0_Q16) return pib_yQ16[0];
  if (xQ16 >= kPitchX1_Q16) return pib_yQ16[PIB_SIZE - 1];

  const int lastSeg = PIB_SIZE - 2;
  int low = pib_cache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      pib_xQ16[low] <= xQ16 && xQ16 < pib_xQ16[low + 1]) {
    // cache hit
  } else {
    static constexpr int kPitchInvDx = PIB_SIZE / 4; // 50
    int cand = (int)(((int64_t)(xQ16 - kPitchX0_Q16) * (int64_t)kPitchInvDx) >> 16);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
    if (cand < lastSeg && xQ16 >= pib_xQ16[cand + 1]) ++cand;
    else if (cand > 0 && xQ16 < pib_xQ16[cand]) --cand;
    low = cand;
    pib_cache[dcoIndex] = (int16_t)low;
  }
  const int32_t delta = xQ16 - pib_xQ16[low];
  return pib_yQ16[low] + (((delta * pib_slopeQ16[low]) + (1 << 15)) >> 16);
}

static int32_t pib_interp_y_q12(int32_t xQ16, int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  if (xInt <= pib_x[0]) return pib_y[0];
  if (xInt >= pib_x[PIB_SIZE - 1]) return pib_y[PIB_SIZE - 1];
  int low = pib_find_seg_int(pib_x, xInt, dcoIndex);
  int32_t deltaQ12 = (xQ16 - (pib_x[low] << 16)) >> 4;
  return pib_y[low] + (int32_t)((((int64_t)deltaQ12 * (int64_t)pib_slopeQ12[low]) + (1LL << 23)) >> 24);
}

// Non-live accuracy oracle: Q20 lerp on native Q16 knots. Returns ratio Q16.
static int32_t pib_interp_y_q20(int32_t xQ16, int dcoIndex) {
  static constexpr int32_t kPitchX0_Q16 = -65536;
  static constexpr int32_t kPitchX1_Q16 = 196608;
  if (xQ16 <= kPitchX0_Q16) return pib_yQ16[0];
  if (xQ16 >= kPitchX1_Q16) return pib_yQ16[PIB_SIZE - 1];
  int low = pib_find_seg_int(pib_xQ16, xQ16, dcoIndex);
  int32_t delta = xQ16 - pib_xQ16[low];
  return pib_yQ16[low] +
         (int32_t)((((int64_t)delta * (int64_t)pib_slopeQ20[low]) + (1LL << 19)) >> 20);
}

// Live fixed-voice yTab → ratioQ16 (same reciprocal as voices.ino vt_freq_scale_post).
static inline int32_t pib_y_to_ratio_q16(int32_t yTab) {
  uint64_t num = ((uint64_t)(uint32_t)yTab << 16) + 5000u;
  return (int32_t)((num * 0xD1B71759ULL) >> 45);
}

// All methods take pitch modifier in [-1,3]. FLOAT uses it directly.
// RATIO / Q20 ref: lroundf(mod*65536) like float-voice A/B. Q12: ×10000 glue.
static float pitch_bench_call(uint8_t method, float modifier, int dco) {
  switch (method) {
    case PITCH_BENCH_FLOAT:
      return pib_interp_float(modifier, dco);
    case PITCH_BENCH_FLOAT_FAST:
      return pib_interp_float_fast(modifier, dco);
    case PITCH_BENCH_RATIO_Q16: {
      int32_t xQ16 = (int32_t)lroundf(modifier * 65536.0f);
      return (float)pib_interp_ratio_q16(xQ16, dco) * (1.0f / 65536.0f);
    }
    case PITCH_BENCH_Q12: {
      float xTab = modifier * (float)PIB_SCALE;
      int32_t xQ16 = (int32_t)lroundf(xTab * 65536.0f);
      return (float)pib_interp_y_q12(xQ16, dco) / (float)PIB_SCALE;
    }
    case PITCH_BENCH_Q20_REF: {
      // Private accuracy reference (Q20 lerp on native Q16 knots); not a live mode.
      int32_t xQ16 = (int32_t)lroundf(modifier * 65536.0f);
      return (float)pib_interp_y_q20(xQ16, dco) * (1.0f / 65536.0f);
    }
    default:
      return 1.0f;
  }
}

static double pib_cents_abs(float cand, float ref) {
  if (!(ref > 1e-12f) || !(cand > 1e-12f)) return 0.0;
  return fabs(1200.0 * log2((double)cand / (double)ref));
}

// True when modifier maps to a native-Q16 knot (live RATIO / Q20-ref domain).
static bool pib_is_knot(float modifier) {
  int32_t xi = (int32_t)lroundf(modifier * 65536.0f);
  int l = 0, h = PIB_SIZE - 1;
  while (l <= h) {
    int m = (l + h) >> 1;
    if (pib_xQ16[m] == xi) return true;
    if (pib_xQ16[m] < xi) l = m + 1;
    else h = m - 1;
  }
  return false;
}

// Bands on modifier (maps to old table bands [-10000,0) / [0,15000) / [15000,30000]).
static int pib_x_band(float modifier) {
  if (modifier < 0.0f) return 0;      // low
  if (modifier < 1.5f) return 1;      // mid
  return 2;                           // high
}

static void pib_hist_add(uint32_t *h, double cents) {
  if (cents < 0.0) cents = 0.0;
  int b = (int)(cents * (double)PIB_HIST_BINS / PIB_HIST_MAX_CENTS);
  if (b < 0) b = 0;
  if (b >= PIB_HIST_BINS) b = PIB_HIST_BINS - 1;
  h[b]++;
}

static double pib_hist_percentile(const uint32_t *h, uint32_t n, double pct) {
  if (n == 0) return 0.0;
  uint64_t target = (uint64_t)((pct / 100.0) * (double)n);
  if (target >= n) target = n - 1u;
  uint64_t cum = 0;
  for (int b = 0; b < PIB_HIST_BINS; ++b) {
    cum += h[b];
    if (cum > target) {
      return ((double)b + 0.5) * PIB_HIST_MAX_CENTS / (double)PIB_HIST_BINS;
    }
  }
  return PIB_HIST_MAX_CENTS;
}

// Count mod-domain samples for one osc over [MIN, MAX] with the given step.
static uint32_t pib_mod_samples_per_osc(float modStep) {
  uint32_t n = 0;
  for (float mod = PITCH_BENCH_MOD_MIN;
       mod <= PITCH_BENCH_MOD_MAX + 0.5f * modStep;
       mod += modStep) {
    ++n;
  }
  return n;
}

static uint32_t pib_q16_samples(int32_t step) {
  uint32_t n = 0;
  for (int32_t x = PIB_XQ16_MIN; x <= PIB_XQ16_MAX; x += step) ++n;
  return n;
}

// Timed flag-path speed run for one method. `repeats` replays the grid (jump uses
// many passes so call count ≈ seq). Cache reset once at start — jump steps still miss.
static void pib_speed_run(uint8_t method, float modStep, int32_t xIntStep, int32_t xQ16Step,
                          uint32_t repeats, uint32_t *outCalls, uint64_t *outUs) {
  pib_reset_cache();
  uint32_t t0 = micros();
  uint32_t calls = 0;

#ifdef USE_FLOAT_VOICE_TASK
  (void)xIntStep;
  (void)xQ16Step;
  volatile float sink = 0.0f;
  static constexpr float Q16_TO_F = 1.0f / 65536.0f;
  for (uint32_t r = 0; r < repeats; ++r) {
    for (uint8_t o = 0; o < PIB_BENCH_OSCS; ++o) {
      for (float mod = PITCH_BENCH_MOD_MIN;
           mod <= PITCH_BENCH_MOD_MAX + 0.5f * modStep;
           mod += modStep) {
        if (method == PITCH_BENCH_FLOAT) {
          sink += pib_interp_float(mod, o);
        } else if (method == PITCH_BENCH_FLOAT_FAST) {
          sink += pib_interp_float_fast(mod, o);
        } else if (method == PITCH_BENCH_RATIO_Q16) {
          int32_t xQ16 = (int32_t)lroundf(mod * 65536.0f);
          sink += (float)pib_interp_ratio_q16(xQ16, o) * Q16_TO_F;
        } else if (method == PITCH_BENCH_Q12) {
          float xTab = mod * (float)PIB_SCALE;
          int32_t xQ16 = (int32_t)lroundf(xTab * 65536.0f);
          sink += (float)pib_interp_y_q12(xQ16, o) / (float)PIB_SCALE;
        }
        ++calls;
      }
    }
  }
  (void)sink;
#else
  // Fixed voice: FLOAT / FLOAT_FAST are soft-float refs (not selectable pitch modes).
  if (method == PITCH_BENCH_FLOAT || method == PITCH_BENCH_FLOAT_FAST) {
    volatile float sink = 0.0f;
    for (uint32_t r = 0; r < repeats; ++r) {
      for (uint8_t o = 0; o < PIB_BENCH_OSCS; ++o) {
        for (float mod = PITCH_BENCH_MOD_MIN;
             mod <= PITCH_BENCH_MOD_MAX + 0.5f * modStep;
             mod += modStep) {
          sink += (method == PITCH_BENCH_FLOAT_FAST)
                      ? pib_interp_float_fast(mod, o)
                      : pib_interp_float(mod, o);
          ++calls;
        }
      }
    }
    (void)sink;
  } else if (method == PITCH_BENCH_RATIO_Q16) {
    volatile int32_t sink = 0;
    for (uint32_t r = 0; r < repeats; ++r) {
      for (uint8_t o = 0; o < PIB_BENCH_OSCS; ++o) {
        for (int32_t xQ16 = PIB_XQ16_MIN; xQ16 <= PIB_XQ16_MAX; xQ16 += xQ16Step) {
          sink += pib_interp_ratio_q16(xQ16, o);
          ++calls;
        }
      }
    }
    (void)sink;
  } else {
    volatile int32_t sink = 0;
    for (uint32_t r = 0; r < repeats; ++r) {
      for (uint8_t o = 0; o < PIB_BENCH_OSCS; ++o) {
        for (int32_t xInt = PIB_XINT_MIN; xInt <= PIB_XINT_MAX; xInt += xIntStep) {
          const int32_t xQ16 = xInt << 16;
          sink += pib_y_to_ratio_q16(pib_interp_y_q12(xQ16, o));
          ++calls;
        }
      }
    }
    (void)sink;
  }
#endif

  uint32_t t1 = micros();
  *outCalls = calls;
  *outUs = (uint64_t)(t1 - t0);
}

static void pib_speed_print_table(const char *pattern, float modStep, int32_t xIntStep,
                                  int32_t xQ16Step, uint32_t repeats,
                                  const uint32_t *totalCalls, const uint64_t *totalUs) {
  double refMeanNs = ((double)totalUs[PITCH_BENCH_FLOAT] * 1000.0) /
                     (double)totalCalls[PITCH_BENCH_FLOAT];
  if (refMeanNs < 1e-9) refMeanNs = 1e-9;

  bench_out_printf("-- pattern=%s  step=%.4f repeats=%lu oscs=%d",
                   pattern, (double)modStep, (unsigned long)repeats, PIB_BENCH_OSCS);
#ifndef USE_FLOAT_VOICE_TASK
  bench_out_printf("  xIntStep=%ld xQ16Step=%ld", (long)xIntStep, (long)xQ16Step);
#else
  (void)xIntStep;
  (void)xQ16Step;
#endif
  bench_out_println("");
  bench_out_println("method       calls    totalUs   meanNs  pctVsFloat");
  bench_out_println("------------ -------- --------- -------- ----------");
  for (uint8_t method = 0; method < PITCH_BENCH_METHODS; ++method) {
    double meanNs = ((double)totalUs[method] * 1000.0) / (double)totalCalls[method];
    double pct = 100.0 * meanNs / refMeanNs;
    bench_out_printf("%-12s %8lu %9lu %8.2f %10.1f\n",
                     pitch_bench_method_name(method),
                     (unsigned long)totalCalls[method],
                     (unsigned long)totalUs[method],
                     meanNs,
                     pct);
  }
}

void pitch_interp_bench_run_speed() {
  pib_ensure_tables();

  // Jump grid is coarse; repeat until call count ≈ one seq pass.
  const uint32_t seqPerOsc = pib_mod_samples_per_osc(PITCH_BENCH_MOD_STEP);
  const uint32_t jumpPerOsc = pib_mod_samples_per_osc(PITCH_BENCH_MOD_STEP_JUMP);
  uint32_t jumpRepeats = 1;
  if (jumpPerOsc > 0 && seqPerOsc > jumpPerOsc) {
    jumpRepeats = (seqPerOsc + jumpPerOsc - 1u) / jumpPerOsc;
  }
  const uint32_t q16Seq = pib_q16_samples(PIB_XQ16_STEP_SEQ);
  const uint32_t q16Jump = pib_q16_samples(PIB_XQ16_STEP_JUMP);
  uint32_t ratioJumpRepeats = 1;
  if (q16Jump > 0 && q16Seq > q16Jump) {
    ratioJumpRepeats = (q16Seq + q16Jump - 1u) / q16Jump;
  }

  struct Pattern {
    const char *name;
    float modStep;
    int32_t xIntStep;
    int32_t xQ16Step;
    uint32_t repeats;
  };
  const Pattern patterns[2] = {
    {"seq",  PITCH_BENCH_MOD_STEP,      PIB_XINT_STEP_SEQ,  PIB_XQ16_STEP_SEQ,  1u},
    {"jump", PITCH_BENCH_MOD_STEP_JUMP, PIB_XINT_STEP_JUMP, PIB_XQ16_STEP_JUMP, jumpRepeats},
  };

  bench_out_println("=== PITCH INTERP BENCH ===");
  bench_out_printf("live_pitch=%s (private tables; does not touch voice)\n",
                   pitch_bench_live_mode_name());
#ifdef USE_FLOAT_VOICE_TASK
  bench_out_printf("speed=flag-path voice=FLOAT (to float ratio; RATIO lroundf(mod*65536))\n");
#else
  bench_out_printf("speed=flag-path voice=FIXED (RATIO native xQ16; Q12 y->ratio; FLOAT*=ref)\n");
#endif
  bench_out_printf("RATIO=native Q16 slopeQ16 trunc±1  Q12=×10000 slopeQ12\n");
  bench_out_printf("FLOAT=walk find  FLOAT_FAST=live trunc+clamp±1 noinline  pctVsFloat vs FLOAT meanNs\n");
  bench_out_printf("seq=walk-favorable  jump=miss-heavy (live-like under pitch mod)\n");
  bench_out_printf("grid=mod %.4f..%.4f\n",
                   (double)PITCH_BENCH_MOD_MIN, (double)PITCH_BENCH_MOD_MAX);

  for (uint8_t p = 0; p < 2; ++p) {
    uint32_t totalCalls[PITCH_BENCH_METHODS] = {0};
    uint64_t totalUs[PITCH_BENCH_METHODS] = {0};
    for (uint8_t method = 0; method < PITCH_BENCH_METHODS; ++method) {
      uint32_t reps = patterns[p].repeats;
#ifndef USE_FLOAT_VOICE_TASK
      if (method == PITCH_BENCH_RATIO_Q16 && p == 1) reps = ratioJumpRepeats;
#else
      (void)ratioJumpRepeats;
#endif
      pib_speed_run(method, patterns[p].modStep, patterns[p].xIntStep,
                    patterns[p].xQ16Step, reps,
                    &totalCalls[method], &totalUs[method]);
    }
    pib_speed_print_table(patterns[p].name, patterns[p].modStep, patterns[p].xIntStep,
                          patterns[p].xQ16Step, patterns[p].repeats, totalCalls, totalUs);
  }
  bench_out_println("=========================");
}

void pitch_interp_bench_run_accuracy() {
  pib_ensure_tables();

  // vs FLOAT (legacy walk): FLOAT_FAST should be ~0¢; RATIO/Q12 show table gap.
  // vs private Q20 ref: float + int modes (FLOAT/FLOAT_FAST ≈ same table-gap cents).
  static const uint8_t kVsFloat[] = {
    PITCH_BENCH_FLOAT_FAST, PITCH_BENCH_RATIO_Q16, PITCH_BENCH_Q12
  };
  static const uint8_t kSlopeCand[] = {
    PITCH_BENCH_FLOAT, PITCH_BENCH_FLOAT_FAST, PITCH_BENCH_RATIO_Q16, PITCH_BENCH_Q12
  };
  static constexpr int N_VS_FLOAT = 3;
  static constexpr int N_SLOPE = 4;

  struct Acc {
    double sumCents;
    double maxCents;
    float xAtMax;
    uint32_t n;
    uint32_t worse05;   // > 0.5¢
    uint32_t worse10;   // > 1.0¢
    uint32_t worse001;  // > 0.01¢ (vs Q20 only)
    uint32_t worse01;   // > 0.1¢  (vs Q20 only)
    double sumKnot;
    double sumMid;
    uint32_t nKnot;
    uint32_t nMid;
    uint32_t hist[PIB_HIST_BINS];
    double bandSum[3];
    double bandMax[3];
    uint32_t bandN[3];
  };

  Acc vsFloat[N_VS_FLOAT];
  Acc vsQ20[N_SLOPE];
  memset(vsFloat, 0, sizeof(vsFloat));
  memset(vsQ20, 0, sizeof(vsQ20));

  // Section 4: knot-only |Q20 - FLOAT| (pure int-table quantization floor).
  double knotQuantSum = 0.0;
  double knotQuantMax = 0.0;
  uint32_t knotQuantN = 0;

  pib_reset_cache();

  for (uint8_t o = 0; o < PIB_BENCH_OSCS; ++o) {
    for (float mod = PITCH_BENCH_MOD_MIN;
         mod <= PITCH_BENCH_MOD_MAX + 0.5f * PITCH_BENCH_MOD_STEP;
         mod += PITCH_BENCH_MOD_STEP) {
      float rFloat = pitch_bench_call(PITCH_BENCH_FLOAT, mod, o);
      float rQ20   = pitch_bench_call(PITCH_BENCH_Q20_REF, mod, o);
      if (!(rFloat > 1e-12f) || !(rQ20 > 1e-12f)) continue;

      const bool atKnot = pib_is_knot(mod);
      const int band = pib_x_band(mod);

      if (atKnot) {
        double cq = pib_cents_abs(rQ20, rFloat);
        knotQuantSum += cq;
        if (cq > knotQuantMax) knotQuantMax = cq;
        knotQuantN++;
      }

      for (int i = 0; i < N_VS_FLOAT; ++i) {
        float y = pitch_bench_call(kVsFloat[i], mod, o);
        double c = pib_cents_abs(y, rFloat);
        Acc &a = vsFloat[i];
        a.sumCents += c;
        a.n++;
        if (c > 0.5) a.worse05++;
        if (c > 1.0) a.worse10++;
        if (c > a.maxCents) {
          a.maxCents = c;
          a.xAtMax = mod;
        }
        pib_hist_add(a.hist, c);
      }

      for (int i = 0; i < N_SLOPE; ++i) {
        float y = pitch_bench_call(kSlopeCand[i], mod, o);
        double c = pib_cents_abs(y, rQ20);
        Acc &a = vsQ20[i];
        a.sumCents += c;
        a.n++;
        if (c > 0.01) a.worse001++;
        if (c > 0.1) a.worse01++;
        if (c > a.maxCents) {
          a.maxCents = c;
          a.xAtMax = mod;
        }
        pib_hist_add(a.hist, c);
        if (atKnot) {
          a.sumKnot += c;
          a.nKnot++;
        } else {
          a.sumMid += c;
          a.nMid++;
        }
        a.bandSum[band] += c;
        a.bandN[band]++;
        if (c > a.bandMax[band]) a.bandMax[band] = c;
      }
    }
  }

  uint32_t nSamples = vsFloat[0].n;

  bench_out_println("=== PITCH INTERP ACCURACY ===");
  bench_out_printf("live_pitch=%s | RATIO=native Q16 slopeQ16 trunc±1 | "
                   "Q20 ref=non-live Q20 lerp on Q16 knots\n",
                   pitch_bench_live_mode_name());
  bench_out_printf("grid=mod %.4f..%.4f step=%.4f oscs=%d | n=%lu\n",
                   (double)PITCH_BENCH_MOD_MIN, (double)PITCH_BENCH_MOD_MAX,
                   (double)PITCH_BENCH_MOD_STEP, PIB_BENCH_OSCS, (unsigned long)nSamples);

  // --- Section 4 first as context for Section 1 ---
  bench_out_println("\n-- Table quantization floor (knot |Q20-FLOAT|) --");
  if (knotQuantN > 0) {
    bench_out_printf("  mean=%.4f¢  max=%.4f¢  n_knots=%lu\n",
                     knotQuantSum / (double)knotQuantN, knotQuantMax,
                     (unsigned long)knotQuantN);
    bench_out_println("  (int knot y vs float knot y; independent of slope mode)");
  } else {
    bench_out_println("  (no knot samples)");
  }

  // --- Section 1: vs FLOAT (legacy walk) ---
  bench_out_println("\n-- vs FLOAT walk (FLOAT_FAST≈0¢; RATIO/Q12 = table gap) --");
  bench_out_println("method       mean¢    max¢   p50¢   p95¢   p99¢  >0.5¢%  >1.0¢%");
  for (int i = 0; i < N_VS_FLOAT; ++i) {
    Acc &a = vsFloat[i];
    double mean = (a.n > 0) ? (a.sumCents / (double)a.n) : 0.0;
    double p50 = pib_hist_percentile(a.hist, a.n, 50.0);
    double p95 = pib_hist_percentile(a.hist, a.n, 95.0);
    double p99 = pib_hist_percentile(a.hist, a.n, 99.0);
    double pct05 = (a.n > 0) ? (100.0 * (double)a.worse05 / (double)a.n) : 0.0;
    double pct10 = (a.n > 0) ? (100.0 * (double)a.worse10 / (double)a.n) : 0.0;
    bench_out_printf("%-12s %6.3f %7.3f %6.3f %6.3f %6.3f %6.2f %6.2f\n",
                     pitch_bench_method_name(kVsFloat[i]),
                     mean, a.maxCents, p50, p95, p99, pct05, pct10);
  }

  // --- Section 2: vs Q20 (float table-gap + int slope A/B) ---
  bench_out_println("\n-- vs Q20 ref (native Q16 knots; FLOAT*≈table gap; RATIO=slopeQ16 vs Q20) --");
  bench_out_println("method       mean¢    max¢   p50¢   p95¢   p99¢  >0.01¢% >0.1¢%");
  for (int i = 0; i < N_SLOPE; ++i) {
    Acc &a = vsQ20[i];
    double mean = (a.n > 0) ? (a.sumCents / (double)a.n) : 0.0;
    double p50 = pib_hist_percentile(a.hist, a.n, 50.0);
    double p95 = pib_hist_percentile(a.hist, a.n, 95.0);
    double p99 = pib_hist_percentile(a.hist, a.n, 99.0);
    double pct001 = (a.n > 0) ? (100.0 * (double)a.worse001 / (double)a.n) : 0.0;
    double pct01 = (a.n > 0) ? (100.0 * (double)a.worse01 / (double)a.n) : 0.0;
    bench_out_printf("%-12s %6.4f %7.4f %6.4f %6.4f %6.4f %7.3f %6.3f\n",
                     pitch_bench_method_name(kSlopeCand[i]),
                     mean, a.maxCents, p50, p95, p99, pct001, pct01);
  }
  bench_out_println("knot vs mid mean¢ (slope error should appear in mid):");
  for (int i = 0; i < N_SLOPE; ++i) {
    Acc &a = vsQ20[i];
    double mk = (a.nKnot > 0) ? (a.sumKnot / (double)a.nKnot) : 0.0;
    double mm = (a.nMid > 0) ? (a.sumMid / (double)a.nMid) : 0.0;
    bench_out_printf("  %-12s knot=%.5f¢ (n=%lu)  mid=%.5f¢ (n=%lu)  max@mod=%.4f\n",
                     pitch_bench_method_name(kSlopeCand[i]),
                     mk, (unsigned long)a.nKnot, mm, (unsigned long)a.nMid,
                     (double)a.xAtMax);
  }

  // --- Section 3: modifier bands vs Q20 ---
  bench_out_println("\n-- vs Q20 ref by mod-band (low<0, mid<1.5, high) --");
  bench_out_println("method       low mean/max¢     mid mean/max¢     high mean/max¢");
  for (int i = 0; i < N_SLOPE; ++i) {
    Acc &a = vsQ20[i];
    double m0 = (a.bandN[0] > 0) ? (a.bandSum[0] / (double)a.bandN[0]) : 0.0;
    double m1 = (a.bandN[1] > 0) ? (a.bandSum[1] / (double)a.bandN[1]) : 0.0;
    double m2 = (a.bandN[2] > 0) ? (a.bandSum[2] / (double)a.bandN[2]) : 0.0;
    bench_out_printf("%-12s %6.4f/%6.4f   %6.4f/%6.4f   %6.4f/%6.4f\n",
                     pitch_bench_method_name(kSlopeCand[i]),
                     m0, a.bandMax[0], m1, a.bandMax[1], m2, a.bandMax[2]);
  }

  bench_out_println("============================");
}

void print_pitch_interp_bench() {
  if (pitch_interp_bench_speed_pending) {
    pitch_interp_bench_speed_pending = false;
    pitch_interp_bench_run_speed();
  }
  if (pitch_interp_bench_accuracy_pending) {
    pitch_interp_bench_accuracy_pending = false;
    pitch_interp_bench_run_accuracy();
  }
}

#else  // !RUNNING_AVERAGE

volatile bool pitch_interp_bench_speed_pending = false;
volatile bool pitch_interp_bench_accuracy_pending = false;

void pitch_interp_bench_run_speed() {}
void pitch_interp_bench_run_accuracy() {}
void print_pitch_interp_bench() {}

#endif

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/preset_store.ino"
#include "include_all.h"

// MCU-side preset store: LittleFS chunk files (4 records each), host text dumps,
// bulk restore. See preset_store.h for the record layout and the protocol summary.
//
// Everything here runs on core 0 (serial task / MIDI / boot one-shot), the same
// context that applies live parameter changes. LittleFS writes briefly stall the
// other core (flash-safe), exactly like the existing calibration FS writers.

static const char PRESET_LAST_FILE[] = "pstLast";

// Shared scratch for one record (save / load / dump) and the bulk staging area.
static uint8_t presetRecordBuf[PRESET_RECORD_SIZE];
static uint8_t presetBulkStaging[PRESET_BULK_STAGING_SIZE];

// "pb00".."pb63" — one chunk file holds PRESET_RECORDS_PER_FILE records.
static void preset_chunk_filename(uint8_t chunkIndex, char* out, size_t cap) {
  snprintf(out, cap, "pb%02u", (unsigned)chunkIndex);
}

static inline uint8_t preset_chunk_index(uint8_t slot) {
  return (uint8_t)(slot >> 2);
}

static inline uint16_t preset_slot_offset(uint8_t slot) {
  return (uint16_t)(slot & 3u) * PRESET_RECORD_SIZE;
}

static const char* preset_bulk_target_name(uint8_t target) {
  switch (target) {
    case PRESET_BULK_PRESET:        return "preset";
    case PRESET_BULK_VOICE_TABLES:  return "voiceTables";
    case PRESET_BULK_PW_CENTER:     return "PWCenter";
    case PRESET_BULK_PW_HIGH_LIMIT: return "PWHighLimit";
    case PRESET_BULK_PW_LOW_LIMIT:  return "PWLowLimit";
    case PRESET_BULK_MANUAL_OFFSET: return "ManualOffset";
    case PRESET_BULK_AMP_COMP_440:  return "AmpComp440";
    case PRESET_BULK_AMP_COMP_DUTY: return "AmpCompDutyOffset";
    default:                        return "unknown";
  }
}

// --- dump text helpers -------------------------------------------------------

static void dump_print_begin(const char* target, int slot, uint32_t size) {
  if (slot >= 0) {
    Serial.printf("[dump] begin target=%s slot=%d size=%lu\n",
                  target, slot, (unsigned long)size);
  } else {
    Serial.printf("[dump] begin target=%s size=%lu\n",
                  target, (unsigned long)size);
  }
}

static void dump_print_data_line(uint16_t offset, const uint8_t* data, uint16_t len) {
  char line[96];
  int n = snprintf(line, sizeof(line), "[dump] d %04X ", (unsigned)offset);
  for (uint16_t i = 0; i < len && n + 2 < (int)sizeof(line); ++i) {
    n += snprintf(line + n, sizeof(line) - n, "%02X", data[i]);
  }
  Serial.println(line);
}

static void dump_print_end(const char* target, uint32_t crc) {
  Serial.printf("[dump] end target=%s crc=%08lX\n", target, (unsigned long)crc);
}

static void dump_print_err(const char* target, const char* reason) {
  Serial.printf("[dump] err target=%s reason=%s\n", target, reason);
}

// Hex-dump a RAM buffer with begin/data/end framing (used for preset records).
static void dump_buffer(const char* target, int slot, const uint8_t* data, uint16_t size) {
  dump_print_begin(target, slot, size);
  for (uint16_t off = 0; off < size; off += PRESET_BULK_CHUNK_DATA) {
    uint16_t n = size - off;
    if (n > PRESET_BULK_CHUNK_DATA) n = PRESET_BULK_CHUNK_DATA;
    dump_print_data_line(off, data + off, n);
  }
  dump_print_end(target, preset_crc32(data, size));
}

// Stream a LittleFS file as a dump without loading it whole (calibration banks).
// expectedSize is the compile-time bank size (see FS.h) rather than the raw
// on-disk file size: older/larger cal files can linger on flash across board
// revisions (e.g. a NUM_OSCILLATORS change), but init_FS() only ever reads the
// leading expectedSize bytes at boot, so that's the data that is actually live.
static void dump_fs_file(const char* target, const char* filename, uint32_t expectedSize) {
  if (!LittleFS.exists(filename)) {
    dump_print_err(target, "missing");
    return;
  }
  File f = LittleFS.open(filename, "r");
  if (!f) {
    dump_print_err(target, "open");
    return;
  }
  const uint32_t fileSize = f.size();
  if (fileSize < expectedSize) {
    f.close();
    dump_print_err(target, "short");
    return;
  }
  const uint32_t size = expectedSize;  // ignore stale trailing bytes from an older, larger bank
  dump_print_begin(target, -1, size);
  uint8_t chunk[PRESET_BULK_CHUNK_DATA];
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t off = 0;
  while (off < size) {
    uint16_t n = (uint16_t)((size - off > PRESET_BULK_CHUNK_DATA)
                                ? PRESET_BULK_CHUNK_DATA
                                : (size - off));
    f.read(chunk, n);
    crc = preset_crc32_update(crc, chunk, n);
    dump_print_data_line((uint16_t)off, chunk, n);
    off += n;
  }
  f.close();
  dump_print_end(target, crc ^ 0xFFFFFFFFu);
}

// --- record build / validate / apply ------------------------------------------

// Snapshot the live patch (param shadow + block globals + presetName) into buf.
static void preset_record_build(uint8_t* buf) {
  memset(buf, 0, PRESET_RECORD_SIZE);
  buf[PRESET_OFF_MAGIC]   = PRESET_MAGIC;
  buf[PRESET_OFF_VERSION] = PRESET_VERSION;

  // Name: the 16 ASCII chars from the last 'q' frame.
  for (int i = 0; i < 16; ++i) {
    buf[PRESET_OFF_NAME + i] = presetName[i];
  }

  memcpy(buf + PRESET_OFF_BITMAP, presetParamSetBitmap, sizeof(presetParamSetBitmap));
  for (uint16_t id = 0; id < PRESET_PARAM_COUNT; ++id) {
    encode_u16_le(buf + PRESET_OFF_PARAMS + id * 2, (uint16_t)presetParamShadow[id]);
  }

  const uint16_t blocks[PRESET_BLOCK_FIELDS] = {
    ADSR_VCA_attack, ADSR_VCA_decay, ADSR_VCA_sustain, ADSR_VCA_release,
    ADSR_VCF_attack, ADSR_VCF_decay, ADSR_VCF_sustain, ADSR_VCF_release,
    ADSR1_attack,    ADSR1_decay,    ADSR1_sustain,    ADSR1_release,
    CUTOFF,          RESONANCE,      (uint16_t)ADSR2toVCF, LFO2toVCF,
  };
  for (uint8_t i = 0; i < PRESET_BLOCK_FIELDS; ++i) {
    encode_u16_le(buf + PRESET_OFF_BLOCKS + i * 2, blocks[i]);
  }

  encode_u32_le(buf + PRESET_OFF_CRC, preset_crc32(buf, PRESET_OFF_CRC));
}

static bool preset_record_validate(const uint8_t* buf) {
  if (buf[PRESET_OFF_MAGIC] != PRESET_MAGIC) return false;
  if (buf[PRESET_OFF_VERSION] != PRESET_VERSION) return false;
  return decode_u32_le(buf + PRESET_OFF_CRC) == preset_crc32(buf, PRESET_OFF_CRC);
}

// Replay a validated record into the live synth state (params via the normal
// router, blocks straight into their globals like the 'a'-'d' handlers do).
// Persistable params and all four blocks are also mirrored to the Input board.
static void preset_record_apply(const uint8_t* buf) {
  for (uint16_t id = 0; id < PRESET_PARAM_COUNT; ++id) {
    if (!(buf[PRESET_OFF_BITMAP + (id >> 3)] & (1u << (id & 7u)))) continue;
    if (!preset_param_is_persistable((uint8_t)id)) continue;
    const int16_t value = (int16_t)decode_u16_le(buf + PRESET_OFF_PARAMS + id * 2);
    update_parameters(id, value);
    serial_echo_persistable_param16((uint8_t)id, value);
  }

  const uint8_t* b = buf + PRESET_OFF_BLOCKS;
  ADSR_VCA_attack  = decode_u16_le(b + 0);
  ADSR_VCA_decay   = decode_u16_le(b + 2);
  ADSR_VCA_sustain = decode_u16_le(b + 4);
  ADSR_VCA_release = decode_u16_le(b + 6);
  ADSR_VCF_attack  = decode_u16_le(b + 8);
  ADSR_VCF_decay   = decode_u16_le(b + 10);
  ADSR_VCF_sustain = decode_u16_le(b + 12);
  ADSR_VCF_release = decode_u16_le(b + 14);
  ADSR1_attack     = decode_u16_le(b + 16);
  ADSR1_decay      = decode_u16_le(b + 18);
  ADSR1_sustain    = decode_u16_le(b + 20);
  ADSR1_release    = decode_u16_le(b + 22);
  CUTOFF           = decode_u16_le(b + 24);
  RESONANCE        = decode_u16_le(b + 26);
  ADSR2toVCF       = (int16_t)decode_u16_le(b + 28);
  LFO2toVCF        = decode_u16_le(b + 30);
  mark_adsr_params_dirty(ADSR_DIRTY_VCA_ALL | ADSR_DIRTY_VCF_ALL | ADSR_DIRTY_DCO_ALL);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();

  serial_send_adsr_vca_block_to_mb();
  serial_send_adsr_vcf_block_to_mb();
  serial_send_adsr_dco_block_to_mb();
  serial_send_filter_block_to_mb();

  for (int i = 0; i < 16; ++i) {
    presetName[i] = buf[PRESET_OFF_NAME + i];
  }
}

// --- last-slot persistence (boot recall) --------------------------------------

static void preset_store_write_last(uint8_t slot) {
  File f = LittleFS.open(PRESET_LAST_FILE, "w");
  if (!f) return;
  f.write(&slot, 1);
  f.close();
}

// --- chunked record I/O -------------------------------------------------------

// Write one validated 598-byte record into its chunk file (create full-size if
// missing / wrong size, otherwise in-place "r+" seek). Returns false on I/O
// failure; prints a [preset]/[bulk] err line via the caller's context.
static bool preset_chunk_write_record(uint8_t slot, const uint8_t* record,
                                      const char* errTag) {
  const uint8_t chunk = preset_chunk_index(slot);
  const uint16_t offset = preset_slot_offset(slot);
  char fname[8];
  preset_chunk_filename(chunk, fname, sizeof(fname));

  const bool exists = LittleFS.exists(fname);
  bool needCreate = !exists;
  if (exists) {
    File check = LittleFS.open(fname, "r");
    if (!check || check.size() != PRESET_CHUNK_SIZE) {
      needCreate = true;
    }
    if (check) check.close();
  }

  if (needCreate) {
    // Create a full-size chunk: write four records in one pass. The target
    // slot gets `record`; the other three get zeros (empty = invalid magic).
    // File::seek refuses past-EOF, so the file must be born at full size.
    File f = LittleFS.open(fname, "w");
    if (!f) {
      FSInfo info;
      if (LittleFS.info(info) && info.usedBytes + PRESET_CHUNK_SIZE > info.totalBytes) {
        Serial.printf("[%s] err slot=%u reason=nospace\n", errTag, (unsigned)slot);
      } else {
        Serial.printf("[%s] err slot=%u reason=open\n", errTag, (unsigned)slot);
      }
      return false;
    }
    static uint8_t emptyRecord[PRESET_RECORD_SIZE];  // zeroed BSS; empty = no magic
    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      const uint8_t* src =
          ((slot & 3u) == i) ? record : emptyRecord;
      if (f.write(src, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
        f.close();
        Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
        return false;
      }
    }
    f.close();
    return true;
  }

  File f = LittleFS.open(fname, "r+");
  if (!f) {
    Serial.printf("[%s] err slot=%u reason=open\n", errTag, (unsigned)slot);
    return false;
  }
  if (!f.seek(offset)) {
    f.close();
    Serial.printf("[%s] err slot=%u reason=seek\n", errTag, (unsigned)slot);
    return false;
  }
  if (f.write(record, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
    f.close();
    Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
    return false;
  }
  f.close();
  return true;
}

// Read one 598-byte record from its chunk. Returns false if the chunk is
// missing, short, or the seek/read fails (caller treats that as empty/corrupt).
static bool preset_chunk_read_record(uint8_t slot, uint8_t* out) {
  const uint8_t chunk = preset_chunk_index(slot);
  const uint16_t offset = preset_slot_offset(slot);
  char fname[8];
  preset_chunk_filename(chunk, fname, sizeof(fname));
  if (!LittleFS.exists(fname)) return false;
  File f = LittleFS.open(fname, "r");
  if (!f || f.size() != PRESET_CHUNK_SIZE) {
    if (f) f.close();
    return false;
  }
  if (!f.seek(offset) || f.read(out, PRESET_RECORD_SIZE) != (int)PRESET_RECORD_SIZE) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

// --- public API ----------------------------------------------------------------

// PARAM_PRESET_SAVE: snapshot live state into slot N and mark it as boot-recall.
void preset_store_save(uint8_t slot) {
  preset_record_build(presetRecordBuf);
  if (!preset_chunk_write_record(slot, presetRecordBuf, "preset")) return;
  preset_store_write_last(slot);
  Serial.printf("[preset] saved slot=%u name=\"%.16s\"\n",
                (unsigned)slot, (const char*)presetName);
}

// PARAM_PRESET_LOAD / MIDI program change / boot recall.
bool preset_store_load(uint8_t slot) {
  if (!preset_chunk_read_record(slot, presetRecordBuf)) {
    Serial.printf("[preset] err slot=%u reason=empty\n", (unsigned)slot);
    return false;
  }

  if (!preset_record_validate(presetRecordBuf)) {
    Serial.printf("[preset] err slot=%u reason=corrupt\n", (unsigned)slot);
    return false;
  }

  preset_record_apply(presetRecordBuf);
  preset_store_write_last(slot);

  Serial.printf("[preset] loaded slot=%u name=\"%.16s\"\n",
                (unsigned)slot, (const char*)presetName);
  serial_send_preset_loaded_to_mb(slot);
  return true;
}

// 'N' handler: push the whole 256-slot directory to the Input board as 'O'
// frames. Opens each chunk once and seeks for the 4 name heads — 64 opens for
// 256 slots. Blank (all-zero) name = unused slot.
void preset_store_send_directory_to_mb() {
  if (!serial2_dma_tx_ready()) return;  // no Input board on this link

  char fname[8];
  uint8_t entry[1 + PRESET_NAME_LEN];
  uint8_t head[PRESET_OFF_BITMAP];

  for (uint8_t chunk = 0; chunk < PRESET_CHUNK_COUNT; ++chunk) {
    preset_chunk_filename(chunk, fname, sizeof(fname));
    File f;
    bool openOk = false;
    if (LittleFS.exists(fname)) {
      f = LittleFS.open(fname, "r");
      openOk = f && f.size() == PRESET_CHUNK_SIZE;
      if (f && !openOk) f.close();
    }

    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      const uint8_t slot = (uint8_t)((chunk << 2) | i);
      memset(entry, 0, sizeof(entry));
      entry[0] = slot;

      if (openOk) {
        if (f.seek((uint32_t)i * PRESET_RECORD_SIZE) &&
            f.read(head, sizeof(head)) == (int)sizeof(head) &&
            head[PRESET_OFF_MAGIC] == PRESET_MAGIC) {
          memcpy(entry + 1, head + PRESET_OFF_NAME, PRESET_NAME_LEN);
        }
      }

      while (!serial2_dma_tx_ready()) {
      }
      serial_frame_write(Serial2Dma, INPUT_CMD_PRESET_DIR_ENTRY, entry, sizeof(entry));
    }
    if (openOk) f.close();
  }
}

// PARAM_PRESET_DUMP: -1 = directory listing ([pdir] lines), 0..255 = slot record.
void preset_store_dump(int16_t sel) {
  if (sel < 0) {
    Serial.println("[pdir] begin");
    uint16_t count = 0;
    char fname[8];
    uint8_t head[PRESET_OFF_BITMAP];

    for (uint8_t chunk = 0; chunk < PRESET_CHUNK_COUNT; ++chunk) {
      preset_chunk_filename(chunk, fname, sizeof(fname));
      if (!LittleFS.exists(fname)) continue;
      File f = LittleFS.open(fname, "r");
      if (!f || f.size() != PRESET_CHUNK_SIZE) {
        if (f) f.close();
        continue;
      }
      for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
        if (!f.seek((uint32_t)i * PRESET_RECORD_SIZE)) continue;
        if (f.read(head, sizeof(head)) != (int)sizeof(head)) continue;
        if (head[PRESET_OFF_MAGIC] != PRESET_MAGIC) continue;
        char name[PRESET_NAME_LEN + 1];
        memcpy(name, head + PRESET_OFF_NAME, PRESET_NAME_LEN);
        name[PRESET_NAME_LEN] = 0;
        const uint8_t slot = (uint8_t)((chunk << 2) | i);
        Serial.printf("[pdir] slot=%03u name=\"%s\"\n", (unsigned)slot, name);
        ++count;
      }
      f.close();
    }
    Serial.printf("[pdir] end count=%u\n", (unsigned)count);
    return;
  }

  if (sel >= (int16_t)PRESET_NUM_SLOTS) return;
  if (!preset_chunk_read_record((uint8_t)sel, presetRecordBuf)) {
    dump_print_err("preset", "empty");
    return;
  }
  dump_buffer("preset", sel, presetRecordBuf, PRESET_RECORD_SIZE);
}

// PARAM_CAL_DUMP: dump calibration LittleFS files as hex (0 / -1 = all seven).
void preset_store_cal_dump(int16_t sel) {
  const bool all = (sel <= CAL_DUMP_ALL);
  if (all || sel == CAL_DUMP_VOICE_TABLES)  dump_fs_file("voiceTables", "voiceTables", FSBankSize);
  if (all || sel == CAL_DUMP_PW_CENTER)     dump_fs_file("PWCenter", "PWCenter", FSPWBankSize);
  if (all || sel == CAL_DUMP_PW_HIGH_LIMIT) dump_fs_file("PWHighLimit", "PWHighLimit", FSPWBankSize);
  if (all || sel == CAL_DUMP_PW_LOW_LIMIT)  dump_fs_file("PWLowLimit", "PWLowLimit", FSPWBankSize);
  if (all || sel == CAL_DUMP_MANUAL_OFFSET) dump_fs_file("ManualOffset", "ManualOffset", FSManualOffsetBankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_440)  dump_fs_file("AmpComp440", "AmpComp440", FSAmpComp440BankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_DUTY) dump_fs_file("AmpCompDutyOffset", "AmpCompDutyOffset", FSAmpCompDutyOffsetBankSize);
}

// --- bulk restore ('B' chunks + 'C' commit) ------------------------------------

// 'B': [target:u8][slot:u8][offset:u16 LE][32 data] → stage. Target/slot ride
// along for symmetry only; the commit frame is authoritative (a mixed-up
// transfer fails its CRC there anyway).
void preset_bulk_chunk(const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_BULK_CHUNK) return;
  const uint16_t offset = decode_u16_le(payload + 2);
  if ((uint32_t)offset + PRESET_BULK_CHUNK_DATA > PRESET_BULK_STAGING_SIZE) return;
  memcpy(presetBulkStaging + offset, payload + 4, PRESET_BULK_CHUNK_DATA);
}

// 'C': [target:u8][slot:u8][size:u16 LE][crc32 LE] → verify staging and persist.
void preset_bulk_commit(const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_BULK_COMMIT) return;
  const uint8_t  target = payload[0];
  const uint8_t  slot   = payload[1];
  const uint16_t size   = decode_u16_le(payload + 2);
  const uint32_t crc    = decode_u32_le(payload + 4);
  const char* tname = preset_bulk_target_name(target);

  if (size == 0 || size > PRESET_BULK_STAGING_SIZE) {
    Serial.printf("[bulk] err target=%s reason=size\n", tname);
    return;
  }
  if (preset_crc32(presetBulkStaging, size) != crc) {
    Serial.printf("[bulk] err target=%s reason=crc\n", tname);
    return;
  }

  uint16_t want = 0;
  const char* calFile = nullptr;
  switch (target) {
    case PRESET_BULK_PRESET:        want = PRESET_RECORD_SIZE; break;
    case PRESET_BULK_VOICE_TABLES:  want = FSBankSize;             calFile = "voiceTables"; break;
    case PRESET_BULK_PW_CENTER:     want = FSPWBankSize;           calFile = "PWCenter"; break;
    case PRESET_BULK_PW_HIGH_LIMIT: want = FSPWBankSize;           calFile = "PWHighLimit"; break;
    case PRESET_BULK_PW_LOW_LIMIT:  want = FSPWBankSize;           calFile = "PWLowLimit"; break;
    case PRESET_BULK_MANUAL_OFFSET: want = FSManualOffsetBankSize; calFile = "ManualOffset"; break;
    case PRESET_BULK_AMP_COMP_440:  want = FSAmpComp440BankSize;   calFile = "AmpComp440"; break;
    case PRESET_BULK_AMP_COMP_DUTY: want = FSAmpCompDutyOffsetBankSize; calFile = "AmpCompDutyOffset"; break;
    default:
      Serial.printf("[bulk] err target=%s reason=target\n", tname);
      return;
  }
  if (size != want) {
    Serial.printf("[bulk] err target=%s reason=size\n", tname);
    return;
  }

  if (target == PRESET_BULK_PRESET) {
    if (!preset_record_validate(presetBulkStaging)) {
      Serial.printf("[bulk] err target=%s reason=record\n", tname);
      return;
    }
    if (!preset_chunk_write_record(slot, presetBulkStaging, "bulk")) return;
    Serial.printf("[bulk] ok target=%s slot=%u\n", tname, (unsigned)slot);
    return;
  }

  // Calibration targets: rewrite the file, then reload the runtime tables the
  // same way debug command 30 (seed fakes) does.
  write_fs_bank(calFile, presetBulkStaging, size);
  init_FS();
  if (target == PRESET_BULK_VOICE_TABLES) {
    precompute_amp_comp_for_engine();
  }
  Serial.printf("[bulk] ok target=%s\n", tname);
}

// --- boot recall -----------------------------------------------------------------

// Recall the last saved/loaded slot ~1.5 s after boot (both cores up, FS mounted).
// No pstLast file (fresh board / never used) = keep firmware defaults.
// Gate on a successful 1-byte read (not a 0xFF sentinel — slot 255 is valid).
void preset_store_boot_recall() {
  if (calibrationFlag) return;
  if (!LittleFS.exists(PRESET_LAST_FILE)) return;
  File f = LittleFS.open(PRESET_LAST_FILE, "r");
  if (!f) return;
  uint8_t slot = 0;
  const bool ok = (f.read(&slot, 1) == 1);
  f.close();
  if (!ok) return;
  preset_store_load(slot);
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/serial2_dma.ino"
#include "include_all.h"
#include "hardware/dma.h"
#include "hardware/uart.h"
#include "pico/mutex.h"
#include <string.h>

// Ping-pong so an ADSR/filter block or a 4-slot directory chunk can queue
// while the previous transfer drains. 256 B holds several stuffed frames.
// Serial2 is Arduino-Pico uart1 (GP20 TX / GP21 RX).
static constexpr uint16_t SERIAL2_DMA_BUF_SIZE = 256;

static mutex_t serial2_dma_mutex;
static int serial2_dma_chan = -1;
static dma_channel_config serial2_dma_cfg;
static uint8_t serial2_dma_buf[2][SERIAL2_DMA_BUF_SIZE];
static uint16_t serial2_dma_len[2];
static uint8_t serial2_dma_fill;
static bool serial2_dma_sending;

Serial2DmaTx Serial2Dma;

static void serial2_dma_poll_unlocked() {
  if (serial2_dma_chan < 0) {
    return;
  }
  if (serial2_dma_sending) {
    if (dma_channel_is_busy((uint)serial2_dma_chan)) {
      return;
    }
    serial2_dma_sending = false;
  }
  const uint16_t count = serial2_dma_len[serial2_dma_fill];
  if (count == 0) {
    return;
  }
  const uint8_t send = serial2_dma_fill;
  serial2_dma_len[send] = 0;
  serial2_dma_fill ^= 1u;
  serial2_dma_sending = true;
  dma_channel_set_write_addr((uint)serial2_dma_chan, &uart_get_hw(uart1)->dr, false);
  dma_channel_set_read_addr((uint)serial2_dma_chan, serial2_dma_buf[send], false);
  dma_channel_set_trans_count((uint)serial2_dma_chan, count, true);
}

void serial2_dma_init() {
  mutex_init(&serial2_dma_mutex);
  serial2_dma_chan = dma_claim_unused_channel(true);
  serial2_dma_cfg = dma_channel_get_default_config((uint)serial2_dma_chan);
  channel_config_set_transfer_data_size(&serial2_dma_cfg, DMA_SIZE_8);
  channel_config_set_read_increment(&serial2_dma_cfg, true);
  channel_config_set_write_increment(&serial2_dma_cfg, false);
  channel_config_set_dreq(&serial2_dma_cfg, uart_get_dreq(uart1, true));
  dma_channel_configure((uint)serial2_dma_chan, &serial2_dma_cfg,
                        &uart_get_hw(uart1)->dr, nullptr, 0, false);
}

void serial2_dma_poll() {
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  mutex_exit(&serial2_dma_mutex);
}

bool serial2_dma_tx_ready() {
  if (serial2_dma_chan < 0) {
    return false;
  }
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  const bool ok =
      (uint16_t)(serial2_dma_len[serial2_dma_fill] + SERIAL_STUFFED_MAX) <=
      SERIAL2_DMA_BUF_SIZE;
  mutex_exit(&serial2_dma_mutex);
  return ok;
}

size_t serial2_dma_write(const uint8_t *p, size_t n) {
  if (serial2_dma_chan < 0 || p == nullptr || n == 0) {
    return 0;
  }
  if (n > SERIAL2_DMA_BUF_SIZE) {
    return 0;
  }
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  if ((size_t)serial2_dma_len[serial2_dma_fill] + n > SERIAL2_DMA_BUF_SIZE) {
    if (!serial2_dma_sending && serial2_dma_len[serial2_dma_fill] > 0) {
      serial2_dma_poll_unlocked();
    }
    if ((size_t)serial2_dma_len[serial2_dma_fill] + n > SERIAL2_DMA_BUF_SIZE) {
      mutex_exit(&serial2_dma_mutex);
      return 0;
    }
  }
  memcpy(serial2_dma_buf[serial2_dma_fill] + serial2_dma_len[serial2_dma_fill], p, n);
  serial2_dma_len[serial2_dma_fill] =
      (uint16_t)(serial2_dma_len[serial2_dma_fill] + n);
  serial2_dma_poll_unlocked();
  mutex_exit(&serial2_dma_mutex);
  return n;
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/state_machines.ino"
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
// With ENABLE_SUBOSC_ENGINE2 there are two subs and this legacy entry point
// (PARAM_SUBOSC_DIVIDE, which predates the per-sub parameters) sets the same ratio on both, so
// that one control still gives an audible sub through the combiner's pass-through operators.
// Subs whose pin is still unwired stay stopped either way.
void set_subosc_divide(uint8_t divide) {
#ifdef ENABLE_SUBOSC_ENGINE2
  for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
    subosc2_set_divide(sub, divide);
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
// Per-sub divides and masters need one pending value each, plus a bit saying which arrived:
// several controls can move between two services and none of them should be lost.
static constexpr uint8_t PIO_DEFER_SUBOSC2 = 1u << 4;
static volatile uint8_t pio_defer_subosc2_divide[SUBOSC_COUNT] = { 0, 0 };
static volatile uint8_t pio_defer_subosc2_mask = 0;
static constexpr uint8_t PIO_DEFER_SUBOSC_MASTER = 1u << 6;
static volatile uint8_t pio_defer_subosc_master[SUBOSC_COUNT] = { 0, 0 };
static volatile uint8_t pio_defer_subosc_master_mask = 0;
// The combiner is one value now that the pair is fixed at SUBOSC_PINS[0]/[1].
static constexpr uint8_t PIO_DEFER_SUBOSC_LOGIC = 1u << 5;
static volatile uint8_t pio_defer_subosc_logic_op = 0;
#endif
// Bit 7: bits 4-6 belong to the per-sub requests above, wired or not.
static constexpr uint8_t PIO_DEFER_CAL_RESTORE = 1u << 7;

void pio_defer_request_sync_mode() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SYNC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_cal_restore() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_CAL_RESTORE, __ATOMIC_SEQ_CST);
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
void pio_defer_request_subosc_divide(uint8_t sub, uint8_t divide) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  pio_defer_subosc2_divide[sub] = divide;
  __dmb();
  __atomic_fetch_or(&pio_defer_subosc2_mask, (uint8_t)(1u << sub), __ATOMIC_SEQ_CST);
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC2, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc_master(uint8_t sub, uint8_t osc) {
  if (sub >= SUBOSC_COUNT) {
    return;
  }
  pio_defer_subosc_master[sub] = osc;
  __dmb();
  __atomic_fetch_or(&pio_defer_subosc_master_mask, (uint8_t)(1u << sub), __ATOMIC_SEQ_CST);
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC_MASTER, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc_logic_op(uint8_t op) {
  pio_defer_subosc_logic_op = op;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC_LOGIC, __ATOMIC_SEQ_CST);
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

  if (pending & PIO_DEFER_CAL_RESTORE) {
    // Manual cal stops every oscillator SM but the soloed one and zeroes the PW
    // channels; nothing in the play path ever starts them again.
    restore_voice_engine_after_calibration();
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
    for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
      if (mask & (1u << sub)) {
        subosc2_set_divide(sub, pio_defer_subosc2_divide[sub]);
      }
    }
  }
  if (pending & PIO_DEFER_SUBOSC_MASTER) {
    const uint8_t mask = __atomic_exchange_n(&pio_defer_subosc_master_mask, 0, __ATOMIC_SEQ_CST);
    for (uint8_t sub = 0; sub < SUBOSC_COUNT; sub++) {
      if (mask & (1u << sub)) {
        subosc2_set_master(sub, pio_defer_subosc_master[sub]);
      }
    }
  }
  if (pending & PIO_DEFER_SUBOSC_LOGIC) {
    subosc2_set_logic(pio_defer_subosc_logic_op);
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

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/subosc.ino"
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

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/utils.ino"

// Linear → logarithmic mapping (0..maxValue). Kept for reuse; pitch ADSR no longer calls it.
uint16_t linearToLogarithmic(uint16_t linearValue, float base, uint16_t maxValue) {
  if (linearValue > maxValue) linearValue = maxValue;

  float normalizedValue = (float)linearValue / (float)maxValue;
  float logValue = log(normalizedValue * (base - 1) + 1) / log(base);
  float maxLogValue = log(1 + (base - 1)) / log(base);
  uint16_t scaledLogValue = (uint16_t)(logValue * ((float)maxValue / maxLogValue));

  return scaledLogValue;
}

// Linear 0..4095 → exponential 0..maxValue. Same curve the Input board applies to the
// envelope A/D/R faders before sending them (auxiliary.h), so a MIDI CC
// lands in the exp domain the 'a'-'c' block frames carry.
uint16_t linearToExponential(uint16_t linearValue, float base, uint16_t maxValue) {
  if (linearValue > 4095) linearValue = 4095;

  float normalizedValue = (float)linearValue / 4095.0f;
  float expValue = pow(base, normalizedValue) - 1.0f;
  float maxExpValue = base - 1.0f;

  return (uint16_t)(expValue * ((float)maxValue / maxExpValue));
}

// Exp curve → float (x^2 / curve). Used by params/LFO drift and related control mapping.
float expConverterFloat(uint16_t readingValue, uint16_t curve) {
  uint16_t pow3Calc = readingValue;
  float expValOut = (float)pow3Calc * pow3Calc / curve;
  if (expValOut < 0.005) {
    expValOut = 0;
  }
  return expValOut;
}

// Exp curve → uint16 (x^2 / curve). Used e.g. by apply_param_portamento_time.
uint16_t expConverter(uint16_t readingValue, uint16_t curve) {
  uint16_t pow3Calc = readingValue;
  uint16_t expValOut = (float)pow3Calc * pow3Calc / curve;
  if (expValOut < 0.1) {
    expValOut = 0;
  }
  return expValOut;
}

#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voices.ino"
#include "include_all.h"
#include <limits.h>
#include <math.h>
#include "clkdiv.h"

// Enable/disable detailed DCO debug report (including OSC1 frequency stages)
#define DCO_DEBUG_REPORT 0

static void amp_chan_levels_fixed(int64_t freq_q24_A, int64_t freq_q24_B, int64_t freq_q24_C,
                                  uint8_t oscA, uint8_t oscB, uint8_t oscC,
                                  uint16_t *outA, uint16_t *outB, uint16_t *outC);
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
int32_t interpolateRatioQ16_cached(int32_t xQ16, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t interpolatePitchMultiplierIntQ16_cached(int32_t xQ16, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
float interpolateRatioFloat_cached(float x, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
float interpolateRatioFloat_cached_fast(float x, int dcoIndex);
#endif

// Live pitch interp: compile-time wrappers (always_inline; not function pointers).
// Fixed-voice wrappers only — float voice uses interpolate_live_ratio_f (FLOAT_FAST would
// otherwise type-check the Q12 #else and fail: IntQ16 is not compiled).
#ifndef USE_FLOAT_VOICE_TASK
static inline __attribute__((always_inline)) int32_t modifiers_q24_to_xQ16(int64_t modifiers_q24) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return (modifiers_q24 >= 0) ? (int32_t)(modifiers_q24 >> 8)
                              : (int32_t)(-((-modifiers_q24) >> 8));
#else
  int64_t x_q24s = modifiers_q24 * (int64_t)multiplierTableScale;
  return (x_q24s >= 0) ? (int32_t)(x_q24s >> 8) : (int32_t)(-((-x_q24s) >> 8));
#endif
}

static inline __attribute__((always_inline)) int32_t interpolate_live_ratio_q16(int32_t xQ16, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return interpolateRatioQ16_cached(xQ16, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  int32_t yTab = interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex);
  uint64_t num = ((uint64_t)(uint32_t)yTab << 16) + 5000u;
  return (int32_t)((num * 0xD1B71759ULL) >> 45);
#else
#error "interpolate_live_ratio_q16: PITCH_INTERP_FLOAT / FLOAT_FAST require USE_FLOAT_VOICE_TASK"
#endif
}
#endif  // !USE_FLOAT_VOICE_TASK

static inline __attribute__((always_inline)) float interpolate_live_ratio_f(float modifiers, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
  return interpolateRatioFloat_cached(modifiers, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  return interpolateRatioFloat_cached_fast(modifiers, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  int32_t xQ16 = (int32_t)lroundf(modifiers * 65536.0f);
  return (float)interpolateRatioQ16_cached(xQ16, dcoIndex) * (1.0f / 65536.0f);
#else
  float x = modifiers * (float)multiplierTableScale;
  int32_t xQ16 = (int32_t)lroundf(x * 65536.0f);
  return (float)interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex)
         / (float)multiplierTableScale;
#endif
}

// Boot init: seed notes, build pitch tables, apply voice mode, run one voice_task_main().
void init_voices() {
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    VOICE_NOTES[i] = DCO_calibration_start_note;
    VOICES[i] = 0;
  }

  // EnvVCA levels let the allocator rank release tails by loudness and tell a
  // finished tail from a live one.
  voiceAlloc.begin(ADSR_VCA_Level_q15);

  initMultiplierTables();
  setVoiceMode();
  voice_task_main();
}

// Fast helper: convert a Q16 note (semitones) to Q24 frequency using linear
// interpolation on the sNotePitches_q24 table. Used in slew-rate mode.
static inline int64_t noteQ16_to_freqQ24(int32_t note_q16) {
  const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
  if (NOTE_TABLE_LEN == 0) return 0;

  int32_t noteInt = note_q16 >> 16;
  uint32_t frac = (uint32_t)note_q16 & 0xFFFF;

  if (noteInt <= 0) {
    if (NOTE_TABLE_LEN == 1) return sNotePitches_q24[0];
    if (frac == 0) return sNotePitches_q24[0];
    int64_t f0 = sNotePitches_q24[0];
    int64_t f1 = sNotePitches_q24[1];
    int64_t df = f1 - f0;
    return f0 + ((df * (int64_t)frac) >> 16);
  }
  if ((size_t)noteInt >= NOTE_TABLE_LEN - 1) {
    // Clamp to top of table
    return sNotePitches_q24[NOTE_TABLE_LEN - 1];
  }

  if (frac == 0) {
    // Exact semitone, just return table entry (common case).
    return sNotePitches_q24[noteInt];
  }

  int64_t f0 = sNotePitches_q24[noteInt];
  int64_t f1 = sNotePitches_q24[noteInt + 1];
  int64_t df = f1 - f0;
  return f0 + ((df * (int64_t)frac) >> 16);
}

// Inverse of noteQ16_to_freqQ24: Q24 Hz → Q16 note via binary search + linear frac.
static inline int32_t freqQ24_to_noteQ16(int64_t freq_q24) {
  const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
  if (NOTE_TABLE_LEN == 0) return 0;
  if (NOTE_TABLE_LEN == 1) return 0;
  if (freq_q24 <= sNotePitches_q24[0]) return 0;
  if (freq_q24 >= sNotePitches_q24[NOTE_TABLE_LEN - 1]) {
    return ((int32_t)(NOTE_TABLE_LEN - 1)) << 16;
  }

  size_t lo = 0;
  size_t hi = NOTE_TABLE_LEN - 1;
  while (hi - lo > 1) {
    size_t mid = lo + ((hi - lo) >> 1);
    if (sNotePitches_q24[mid] <= freq_q24) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  int64_t f0 = sNotePitches_q24[lo];
  int64_t f1 = sNotePitches_q24[hi];
  int64_t df = f1 - f0;
  if (df <= 0) return ((int32_t)lo) << 16;

  int64_t num = (freq_q24 - f0) << 16;
  int32_t frac = (int32_t)(num / df);
  if (frac < 0) frac = 0;
  if (frac > 0xFFFF) frac = 0xFFFF;
  return (((int32_t)lo) << 16) + frac;
}

// Helper: convert float Hz to Q24 fixed-point (Hz * 2^24)
static inline int64_t float_to_q24(float f) {
  return (int64_t)lrintf(f * (float)(1 << 24));
}

// midi/base + offset → table index using signed math (no uint8 wrap-to-top).
// High notes still fold down by octaves to stay within highestNote, then clamp.
static inline uint8_t midi_offset_to_table_index(int midi_or_base, int offset, size_t table_len) {
  int n = midi_or_base - 36 + offset;
  if (n < 0) n = 0;
  while (n > (int)highestNote) n -= 12;
  if (table_len == 0) return 0;
  if (n >= (int)table_len) n = (int)table_len - 1;
  return (uint8_t)n;
}

// Resolve porta start note (Q16). First edge snaps; later edges use cur note (incl. index 0).
static inline int32_t porta_resolve_start_note_q16(uint8_t osc, int32_t target_q16) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_q16[osc];
  }
  if (portamento_cur_freq_q24[osc] > 0) {
    porta_note_valid[osc] = true;
    return freqQ24_to_noteQ16(portamento_cur_freq_q24[osc]);
  }
  porta_note_valid[osc] = true;
  return target_q16;
}

// Common endpoint latch for note-space porta setup.
static inline void porta_latch_endpoints_q16(uint8_t osc, int32_t start_q16, int32_t target_q16) {
  porta_note_start_q16[osc] = start_q16;
  porta_note_stop_q16[osc] = target_q16;
  porta_note_cur_q16[osc] = start_q16;
  porta_note_valid[osc] = true;
  int64_t freq_q24 = noteQ16_to_freqQ24(start_q16);
  portamento_start_q24[osc] = freq_q24;
  portamento_stop_q24[osc] = noteQ16_to_freqQ24(target_q16);
  portamento_cur_freq_q24[osc] = freq_q24;
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static inline void porta_setup_time_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                       int32_t T_fixed) {
  if (T_fixed < 1) T_fixed = 1;
  porta_latch_endpoints_q16(osc, start_q16, target_q16);

  int32_t dNote_q16 = target_q16 - start_q16;
  int64_t halfT = (int64_t)T_fixed >> 1;
  int64_t num = (dNote_q16 >= 0) ? ((int64_t)dNote_q16 + halfT) : ((int64_t)dNote_q16 - halfT);
  porta_note_step_q16[osc] = (int32_t)(num / (int64_t)T_fixed);
  if (dNote_q16 != 0 && porta_note_step_q16[osc] == 0) {
    porta_note_step_q16[osc] = (dNote_q16 > 0) ? 1 : -1;
  }
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static inline void porta_setup_slew_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                       int32_t T_slew) {
  if (T_slew < 1) T_slew = 1;
  porta_latch_endpoints_q16(osc, start_q16, target_q16);

  int32_t dNote_q16 = target_q16 - start_q16;
  if (dNote_q16 == 0) {
    porta_note_step_q16[osc] = 0;
  } else {
    int32_t rate = (int32_t)(((int64_t)12 << 16) / (int64_t)T_slew);
    if (rate == 0) rate = 1;
    porta_note_step_q16[osc] = (dNote_q16 > 0) ? rate : -rate;
  }
}

static inline void porta_setup_glide_q16(uint8_t osc, int32_t start_q16, int32_t target_q16,
                                        uint8_t mode) {
  if (mode == PORTA_MODE_TIME) {
    int32_t T = (portamento_time_fixed == 0) ? 1 : (int32_t)portamento_time_fixed;
    porta_setup_time_q16(osc, start_q16, target_q16, T);
  } else {
    int32_t T = (portamento_time_slew == 0) ? 1 : (int32_t)portamento_time_slew;
    porta_setup_slew_q16(osc, start_q16, target_q16, T);
  }
}

#ifdef USE_FLOAT_VOICE_TASK
// Helper: convert a semitone index (float) to Hz using sNotePitches[] with linear interpolation.
static inline float noteIndex_to_freqFloat(float noteIndex) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0) return 0.0f;
  if (noteIndex <= 0.0f) return sNotePitches[0];
  if (noteIndex >= (float)(LEN - 1)) return sNotePitches[LEN - 1];

  int n0 = (int)floorf(noteIndex);
  int n1 = n0 + 1;
  float t = noteIndex - (float)n0;
  float f0 = sNotePitches[n0];
  float f1 = sNotePitches[n1];
  return f0 + (f1 - f0) * t;
}

// Inverse of noteIndex_to_freqFloat: Hz → semitone index via binary search + linear frac.
static inline float freqFloat_to_noteIndex(float hz) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0) return 0.0f;
  if (LEN == 1) return 0.0f;
  if (hz <= sNotePitches[0]) return 0.0f;
  if (hz >= sNotePitches[LEN - 1]) return (float)(LEN - 1);

  size_t lo = 0;
  size_t hi = LEN - 1;
  while (hi - lo > 1) {
    size_t mid = lo + ((hi - lo) >> 1);
    if (sNotePitches[mid] <= hz) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  float f0 = sNotePitches[lo];
  float f1 = sNotePitches[hi];
  float df = f1 - f0;
  if (df <= 0.0f) return (float)lo;
  return (float)lo + (hz - f0) / df;
}

static inline float porta_resolve_start_note_f(uint8_t osc, float target) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_f[osc];
  }
  if (porta_freq_cur_f[osc] > 0.0f) {
    porta_note_valid[osc] = true;
    return freqFloat_to_noteIndex(porta_freq_cur_f[osc]);
  }
  porta_note_valid[osc] = true;
  return target;
}

static inline void porta_latch_endpoints_f(uint8_t osc, float startNote, float targetNote) {
  porta_note_start_f[osc] = startNote;
  porta_note_stop_f[osc] = targetNote;
  porta_note_cur_f[osc] = startNote;
  porta_note_valid[osc] = true;
  float startHz = noteIndex_to_freqFloat(startNote);
  float stopHz = noteIndex_to_freqFloat(targetNote);
  porta_freq_start_f[osc] = startHz;
  porta_freq_stop_f[osc] = stopHz;
  porta_freq_cur_f[osc] = startHz;
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static inline void porta_setup_time_f(uint8_t osc, float startNote, float targetNote, float T_fixed) {
  if (T_fixed < 1.0f) T_fixed = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);

  float dNote = targetNote - startNote;
  const float SCALE = 65536.0f;
  float halfT = 0.5f * T_fixed;
  float d_q16 = dNote * SCALE;
  float num = (d_q16 >= 0.0f) ? (d_q16 + halfT) : (d_q16 - halfT);
  float stepNote = (num / T_fixed) / SCALE;
  if (dNote != 0.0f && stepNote == 0.0f) {
    stepNote = (dNote > 0.0f) ? (1.0f / SCALE) : (-1.0f / SCALE);
  }
  porta_note_step_f[osc] = stepNote;
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static inline void porta_setup_slew_f(uint8_t osc, float startNote, float targetNote, float T_slew) {
  if (T_slew < 1.0f) T_slew = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);

  float dNote = targetNote - startNote;
  if (dNote == 0.0f) {
    porta_note_step_f[osc] = 0.0f;
  } else {
    float rate = 12.0f / T_slew;
    if (rate == 0.0f) rate = 1.0f / 65536.0f;
    porta_note_step_f[osc] = (dNote > 0.0f) ? rate : -rate;
  }
}

static inline void porta_setup_glide_f(uint8_t osc, float startNote, float targetNote, uint8_t mode) {
  if (mode == PORTA_MODE_TIME) {
    float T = (portamento_time_fixed == 0) ? 1.0f : (float)portamento_time_fixed;
    porta_setup_time_f(osc, startNote, targetNote, T);
  } else {
    float T = (portamento_time_slew == 0) ? 1.0f : (float)portamento_time_slew;
    porta_setup_slew_f(osc, startNote, targetNote, T);
  }
}

// Q24 → float modifier/Hz scale (multiply avoids per-sample divide by 2^24).
static constexpr float Q24_TO_FLOAT = 1.0f / 16777216.0f;

static inline float q24_to_float(int32_t q) {
  return (float)q * Q24_TO_FLOAT;
}
#endif

#ifndef USE_FLOAT_VOICE_TASK
// Fixed-point realtime voice engine (portamento, modifiers, clkdiv, amp, PIO/PWM/PW).
// Selected by voice_task_main() when USE_FLOAT_VOICE_TASK is not defined.
void __not_in_flash_func(voice_task_fixed_point)() {
  // Track portamento-time and mode changes between calls so we can smoothly
  // retime the glide without introducing pitch discontinuities.
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_TIME;
  static uint8_t lastNote1 = 0, lastNote2 = 0, lastNote3 = 0;
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  // Pre-calculate pitch bend as a Q24 value. This is done once per voice_task_fixed_point call.
  int32_t calcPitchbend_q24;

  BENCH_BEGIN(vt_pitchbend);
  // Optimized: Perform pitch bend calculation entirely in fixed-point Q24.
  // ((bend / 8192.0) - 1.0) * pitchBendMultiplier
  // This avoids float conversions and multiplications in the hot path.
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  calcPitchbend_q24 = (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  // Latch note-on edges for every voice slot (para may set flags on 1/2).
  for (int k = 0; k < NUM_VOICES; k++) {
    if (note_on_flag[k] == 1) {
      note_on_flag_flag[k] = true;
      note_on_flag[k] = 0;
    }
  }

  for (int i = 0; i < NUM_VOICES_VOICE_TASK; i++) {

#if DCO_DEBUG_REPORT
    // Debug: track OSC1 frequency at key stages of the pipeline for DCO report.
    float dbg_freq_base_Hz = 0.0f;       // After portamento, before modifiers
    float dbg_freq_after_mod_Hz = 0.0f;  // After all modifiers applied (freq_q24_A)
#endif

    // note_off keeps VOICE_NOTES (release pitch); free/steal uses VOICES[].
    // Never-played slots stay 0 — gate midi→table math (uint8 underflow → shriek).

    // Note index convention: table_index = midi - 36 + offset (36 ⇒ unison).
    //   Mono: octave_shift = global octave; OSC2/3 = relative to note1 (36 ⇒ unison with OSC1).
    //   Poly/stack: every osc uses octave_shift only (shared octave); not OSC2/3.
    const size_t NOTE_TABLE_LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
    uint8_t note1, note2, note3;
    if (voiceMode == 0) {
      const uint8_t vn = VOICE_NOTES[i];
      if (vn == 0) {
        note1 = note2 = note3 = 0;
      } else {
        note1 = midi_offset_to_table_index((int)vn, (int)octave_shift, NOTE_TABLE_LEN);
        note2 = midi_offset_to_table_index((int)note1, (int)OSC2_interval, NOTE_TABLE_LEN);
        note3 = midi_offset_to_table_index((int)note1, (int)OSC3_interval, NOTE_TABLE_LEN);
      }
    } else {
      const uint8_t vn0 = VOICE_NOTES[0];
      const uint8_t vn1 = VOICE_NOTES[1];
      const uint8_t vn2 = VOICE_NOTES[2];
      note1 = (vn0 == 0) ? 0 : midi_offset_to_table_index((int)vn0, (int)octave_shift, NOTE_TABLE_LEN);
      note2 = (vn1 == 0) ? 0 : midi_offset_to_table_index((int)vn1, (int)octave_shift, NOTE_TABLE_LEN);
      note3 = (vn2 == 0) ? 0 : midi_offset_to_table_index((int)vn2, (int)octave_shift, NOTE_TABLE_LEN);
    }

      const bool pitchTargetChanged =
          note1 != lastNote1 || note2 != lastNote2 || note3 != lastNote3;
      lastNote1 = note1;
      lastNote2 = note2;
      lastNote3 = note3;

      BENCH_BEGIN(vt_osc_detune);
      // Optimized: Calculate OSC2/OSC3 detune in Q24 and keep it there.
      // The float conversion has been removed as it is no longer needed.
      // detune = 1.0 + 0.0002 * (256 - val)
      static constexpr int32_t DETUNE_SCALE_Q24 = (int32_t)(0.0002f * (float)(1 << 24) + 0.5f);
      int32_t detune_steps = ((int)256 - OSC2DetuneVal);
      int32_t detune_q24 = (1 << 24) + (detune_steps * DETUNE_SCALE_Q24);
      int32_t detune3_steps = ((int)256 - OSC3DetuneVal);
      int32_t detune3_q24 = (1 << 24) + (detune3_steps * DETUNE_SCALE_Q24);
      BENCH_END(vt_osc_detune);

      int64_t freq_q24_A;
      int64_t freq_q24_B;
      int64_t freq_q24_C;

      // Fixed osc indices for current mono hardware (3 oscs on voice 0).
      // Future paraphonic mode can remap osc ownership per voice without gutting allocation.
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;

      ////***********************    PORTAMENTO CODE   ****************************************/////
      BENCH_BEGIN(vt_portamento);
      if (portaTime > 0 /*&& portamento_start != 0 && portamento_stop != 0*/) {
        uint32_t now_us = micros();
        if (voiceMode == 0) {
          portamentoTimer[i] = now_us - portamentoStartMicros[i];
        } else {
          for (int k = 0; k < NUM_VOICES; k++) {
            portamentoTimer[k] = now_us - portamentoStartMicros[k];
          }
        }

        // Mono: one edge restarts all three oscs. Para: each note_on_flag_flag[k] restarts osc k only.
        // TIME = fixed duration any interval; SLEW = constant semitone rate (time ∝ interval).
        if (voiceMode == 0) {
          if (note_on_flag_flag[i]) {
            portamentoStartMicros[i] = now_us;
            portamentoTimer[i] = 0;

            int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
            int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
            int32_t targetNoteC_q16 = ((int32_t)note3) << 16;
            porta_setup_glide_q16(DCO_A, porta_resolve_start_note_q16(DCO_A, targetNoteA_q16),
                                  targetNoteA_q16, portaMode);
            porta_setup_glide_q16(DCO_B, porta_resolve_start_note_q16(DCO_B, targetNoteB_q16),
                                  targetNoteB_q16, portaMode);
            porta_setup_glide_q16(DCO_C, porta_resolve_start_note_q16(DCO_C, targetNoteC_q16),
                                  targetNoteC_q16, portaMode);
          }
        } else {
          // Paraphonic: restart porta per voice slot → osc k (1:1).
          const uint8_t targetNotes[3] = { note1, note2, note3 };
          for (int k = 0; k < NUM_VOICES; k++) {
            if (!note_on_flag_flag[k]) continue;
            const uint8_t osc = (uint8_t)k;
            portamentoStartMicros[k] = now_us;
            portamentoTimer[k] = 0;
            int32_t target_q16 = ((int32_t)targetNotes[k]) << 16;
            porta_setup_glide_q16(osc, porta_resolve_start_note_q16(osc, target_q16),
                                  target_q16, portaMode);
          }
        }

        // Control-change: skip steady (stale start/step + huge elapsed → pitch jump).
        const bool portaNoteOnEdge =
            (voiceMode == 0) ? note_on_flag_flag[i]
                             : (note_on_flag_flag[0] || note_on_flag_flag[1] || note_on_flag_flag[2]);
        const bool portaDoRetime =
            (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !portaNoteOnEdge;

        int64_t curA;
        int64_t curB;
        int64_t curC;

        if (portaDoRetime) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i] = 0;
          if (voiceMode != 0) {
            for (int k = 0; k < NUM_VOICES; k++) {
              portamentoStartMicros[k] = now_us;
              portamentoTimer[k] = 0;
            }
          }

          int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
          int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
          int32_t targetNoteC_q16 = ((int32_t)note3) << 16;
          // Retime from true current Hz so a mid-glide CC change stays continuous.
          int32_t curNoteA_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_A]);
          int32_t curNoteB_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_B]);
          int32_t curNoteC_q16 = freqQ24_to_noteQ16(portamento_cur_freq_q24[DCO_C]);
          porta_setup_glide_q16(DCO_A, curNoteA_q16, targetNoteA_q16, portaMode);
          porta_setup_glide_q16(DCO_B, curNoteB_q16, targetNoteB_q16, portaMode);
          porta_setup_glide_q16(DCO_C, curNoteC_q16, targetNoteC_q16, portaMode);
          curA = portamento_cur_freq_q24[DCO_A];
          curB = portamento_cur_freq_q24[DCO_B];
          curC = portamento_cur_freq_q24[DCO_C];
        } else if (porta_note_cur_q16[DCO_A] == porta_note_stop_q16[DCO_A] &&
                   porta_note_cur_q16[DCO_B] == porta_note_stop_q16[DCO_B] &&
                   porta_note_cur_q16[DCO_C] == porta_note_stop_q16[DCO_C]) {
          // Glide done, portaTime still > 0: reuse stop Hz (no int64 advance / table lerp).
          curA = portamento_stop_q24[DCO_A];
          curB = portamento_stop_q24[DCO_B];
          curC = portamento_stop_q24[DCO_C];
        } else {
          int32_t elapsedA = (voiceMode == 0) ? (int32_t)portamentoTimer[i] : (int32_t)portamentoTimer[0];
          int32_t elapsedB = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[1];
          int32_t elapsedC = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[2];

          int32_t dNoteA_q16 = porta_note_stop_q16[DCO_A] - porta_note_start_q16[DCO_A];
          int32_t dNoteB_q16 = porta_note_stop_q16[DCO_B] - porta_note_start_q16[DCO_B];
          int32_t dNoteC_q16 = porta_note_stop_q16[DCO_C] - porta_note_start_q16[DCO_C];

          int64_t curNoteA_q16 = (int64_t)porta_note_start_q16[DCO_A] + (int64_t)porta_note_step_q16[DCO_A] * (int64_t)elapsedA;
          int64_t curNoteB_q16 = (int64_t)porta_note_start_q16[DCO_B] + (int64_t)porta_note_step_q16[DCO_B] * (int64_t)elapsedB;
          int64_t curNoteC_q16 = (int64_t)porta_note_start_q16[DCO_C] + (int64_t)porta_note_step_q16[DCO_C] * (int64_t)elapsedC;

          if ((dNoteA_q16 >= 0 && curNoteA_q16 >= (int64_t)porta_note_stop_q16[DCO_A]) ||
              (dNoteA_q16 < 0 && curNoteA_q16 <= (int64_t)porta_note_stop_q16[DCO_A])) {
            curNoteA_q16 = porta_note_stop_q16[DCO_A];
          }
          if ((dNoteB_q16 >= 0 && curNoteB_q16 >= (int64_t)porta_note_stop_q16[DCO_B]) ||
              (dNoteB_q16 < 0 && curNoteB_q16 <= (int64_t)porta_note_stop_q16[DCO_B])) {
            curNoteB_q16 = porta_note_stop_q16[DCO_B];
          }
          if ((dNoteC_q16 >= 0 && curNoteC_q16 >= (int64_t)porta_note_stop_q16[DCO_C]) ||
              (dNoteC_q16 < 0 && curNoteC_q16 <= (int64_t)porta_note_stop_q16[DCO_C])) {
            curNoteC_q16 = porta_note_stop_q16[DCO_C];
          }

          porta_note_cur_q16[DCO_A] = (int32_t)curNoteA_q16;
          porta_note_cur_q16[DCO_B] = (int32_t)curNoteB_q16;
          porta_note_cur_q16[DCO_C] = (int32_t)curNoteC_q16;

          // At stop: latched stop Hz. Mid-glide: noteQ16_to_freqQ24 (frac==0 → single table load).
          if (curNoteA_q16 == (int64_t)porta_note_stop_q16[DCO_A]) {
            curA = portamento_stop_q24[DCO_A];
          } else {
            curA = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_A]);
          }
          if (curNoteB_q16 == (int64_t)porta_note_stop_q16[DCO_B]) {
            curB = portamento_stop_q24[DCO_B];
          } else {
            curB = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_B]);
          }
          if (curNoteC_q16 == (int64_t)porta_note_stop_q16[DCO_C]) {
            curC = portamento_stop_q24[DCO_C];
          } else {
            curC = noteQ16_to_freqQ24(porta_note_cur_q16[DCO_C]);
          }
        }

        portamento_cur_freq_q24[DCO_A] = curA;
        portamento_cur_freq_q24[DCO_B] = curB;
        portamento_cur_freq_q24[DCO_C] = curC;
      } else {
        portamento_cur_freq_q24[DCO_A] = sNotePitches_q24[note1];
        portamento_start_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
        portamento_stop_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
        porta_note_cur_q16[DCO_A] = ((int32_t)note1) << 16;
        porta_note_stop_q16[DCO_A] = porta_note_cur_q16[DCO_A];
        porta_note_valid[DCO_A] = true;

        portamento_cur_freq_q24[DCO_B] = sNotePitches_q24[note2];
        portamento_start_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
        portamento_stop_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
        porta_note_cur_q16[DCO_B] = ((int32_t)note2) << 16;
        porta_note_stop_q16[DCO_B] = porta_note_cur_q16[DCO_B];
        porta_note_valid[DCO_B] = true;

        portamento_cur_freq_q24[DCO_C] = sNotePitches_q24[note3];
        portamento_start_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
        portamento_stop_q24[DCO_C] = portamento_cur_freq_q24[DCO_C];
        porta_note_cur_q16[DCO_C] = ((int32_t)note3) << 16;
        porta_note_stop_q16[DCO_C] = porta_note_cur_q16[DCO_C];
        porta_note_valid[DCO_C] = true;
      }

#if defined(BENCH_PATH_STATS)
      // Exclusive path tag for jitter attribution (note_on > retime > steady > off).
      if (portaTime == 0) {
        BENCH_PATH_INC(porta_off);
      } else if (note_on_flag_flag[i]) {
        BENCH_PATH_INC(porta_note_on);
      } else if (portaTimeChanged || portaModeChanged || pitchTargetChanged) {
        BENCH_PATH_INC(porta_retime);
      } else if (portaMode == PORTA_MODE_TIME) {
        BENCH_PATH_INC(porta_steady_time);
      } else {
        BENCH_PATH_INC(porta_steady_slew);
      }
#endif
#if DCO_DEBUG_REPORT
      // Debug: OSC1 base frequency after portamento, before modifiers (in Hz)
      dbg_freq_base_Hz = (float)portamento_cur_freq_q24[DCO_A] / (float)(1 << 24);
#endif
      BENCH_END(vt_portamento);
      ////***********************    PORTAMENTO CODE  END    ****************************************/////

      BENCH_BEGIN(vt_adsr_mod);
      // Same as LFO/drift: linear Q15 env × baked depth_q24 (exp is on the knob).
      // ADSR3→pitch select: 0=OSC1, 1=OSC2, 2=OSC1+OSC2, 3=OSC3, 4=all.
      // Mono: one tap. Para/stack: osc k ← ADSR1Level_q15[k].
      int32_t ADSRModifierOSC1_q24 = 0;
      int32_t ADSRModifierOSC2_q24 = 0;
      int32_t ADSRModifierOSC3_q24 = 0;
      if (ADSR1toDETUNE1_scale_q24 != 0) {
        if (voiceMode == 0) {
          const int32_t m = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[i]),
                                          ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1_q24 = m;
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2_q24 = m;
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3_q24 = m;
        } else {
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[0]),
                                                 ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[1]),
                                                 ADSR1toDETUNE1_scale_q24);
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3_q24 = applyDepthQ24(env_dco_pitch_wave_q15(ADSR1Level_q15[2]),
                                                 ADSR1toDETUNE1_scale_q24);
        }
      }
      BENCH_END(vt_adsr_mod);

      BENCH_BEGIN(vt_unison_mod);
      // Fixed-point unison modifier in Q24: 0.0001 * unisonDetune * step
      // unisonDetune is uint8_t; product fits int32 (no int64 on M0+).
      static constexpr int32_t UNISON_SCALE_Q24 = (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
      // Per-osc spread (matches float path): OSC1=0, OSC2=+1, OSC3=-1.
      static constexpr int32_t OSC_UNISON_STEP[3] = { 0, 1, -1 };
      const int32_t unisonBase = (int32_t)unisonDetune * UNISON_SCALE_Q24;
      int32_t unisonMODIFIER_OSC1_q24 = unisonBase * OSC_UNISON_STEP[0];
      int32_t unisonMODIFIER_OSC2_q24 = unisonBase * OSC_UNISON_STEP[1];
      int32_t unisonMODIFIER_OSC3_q24 = unisonBase * OSC_UNISON_STEP[2];
#if NUM_VOICES_TOTAL > 1
      // Classic poly voice-index alternating pattern on top of per-osc spread.
      int32_t voiceMag = (i >> 1) + 1;
      int32_t voiceSign = ((i & 0x01) == 0) ? 1 : -1;
      int32_t voiceUnison_q24 = unisonBase * (voiceSign * voiceMag);
      unisonMODIFIER_OSC1_q24 += voiceUnison_q24;
      unisonMODIFIER_OSC2_q24 += voiceUnison_q24;
      unisonMODIFIER_OSC3_q24 += voiceUnison_q24;
#endif
      BENCH_END(vt_unison_mod);

      BENCH_BEGIN(vt_drift_mod);
      // Snapshot Core 0 mailbox once, then applyDepthQ24 on locals (no volatile-in-mul).
      const int32_t driftScale_q24 = drift_pitch_scale_q24;
      const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
      const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
      const int16_t driftC = LFO_DRIFT_LEVEL[DCO_C];
      int32_t DETUNE_DRIFT_OSC1_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftA, driftScale_q24) : 0;
      int32_t DETUNE_DRIFT_OSC2_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftB, driftScale_q24) : 0;
      int32_t DETUNE_DRIFT_OSC3_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftC, driftScale_q24) : 0;
      BENCH_END(vt_drift_mod);

      int32_t modifiersBase_q24;
      int32_t freqModifiers_q24;
      int32_t freq2Modifiers_q24;
      int32_t freq3Modifiers_q24;
      {
        // Snapshot LFO pitch mailbox here only (short live range; not across clkdiv/amp/RANGE).
        const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
        const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
        const int32_t local_lfo1_osc3 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC3];
        const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
        const int32_t local_lfo2_osc3 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC3];
        BENCH_BEGIN(vt_modifiers);
        // Combine modifiers in Q24 (faithful to original float path)
        // Unison is applied per-osc below; shared part is pitchbend + epsilon only (LFO1 per osc).
        // Character pitch jitter stacks after matrix when scale nonzero.
        // Q24 octave-fraction sums fit int32 for normal depths (no int64).
        modifiersBase_q24 =
          calcPitchbend_q24 + Q24_ONE_EPS + matrix_pitch_mod_q24;
        if (char_pitch_scale_q15) {
          modifiersBase_q24 += character_pitch_delta_q24();
        }
        freqModifiers_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + unisonMODIFIER_OSC1_q24 + local_lfo1_osc1;
        freq2Modifiers_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + unisonMODIFIER_OSC2_q24 + local_lfo1_osc2 + local_lfo2_osc2;
        freq3Modifiers_q24 = ADSRModifierOSC3_q24 + DETUNE_DRIFT_OSC3_q24 + modifiersBase_q24 + unisonMODIFIER_OSC3_q24 + local_lfo1_osc3 + local_lfo2_osc3;
        BENCH_END(vt_modifiers);
      }

      BENCH_BEGIN(vt_freq_scale_x);


      // Fast fixed-point equivalent of:
      //   freq  *= interpolatePitchMultiplier(freqModifiers)/multiplierTableScale;
      //   freq2 *= OSC2_detune * interpolatePitchMultiplier(freq2Modifiers)/multiplierTableScale;
      //   freq3 *= OSC3_detune * interpolatePitchMultiplier(freq3Modifiers)/multiplierTableScale;
      int32_t xScaled1_Q16 = modifiers_q24_to_xQ16(freqModifiers_q24);
      int32_t xScaled2_Q16 = modifiers_q24_to_xQ16(freq2Modifiers_q24);
      int32_t xScaled3_Q16 = modifiers_q24_to_xQ16(freq3Modifiers_q24);
      BENCH_END(vt_freq_scale_x);

      BENCH_BEGIN(vt_ratio_interp);
      int32_t ratio1_Q16 = interpolate_live_ratio_q16(xScaled1_Q16, DCO_A);
      int32_t ratio2_Q16 = interpolate_live_ratio_q16(xScaled2_Q16, DCO_B);
      int32_t ratio3_Q16 = interpolate_live_ratio_q16(xScaled3_Q16, DCO_C);
      BENCH_END(vt_ratio_interp);

      BENCH_BEGIN(vt_freq_scale_post);
      freq_q24_A = (portamento_cur_freq_q24[DCO_A] * (int64_t)ratio1_Q16) >> 16;
      // Combine OSC2 ratio with detune into one Q16 factor
      // detune_Q16 = round(detune_q24 / 2^8)
      int32_t detune_Q16 = (int32_t)((((int64_t)detune_q24) + 128) >> 8);
      // combined_Q16 = round((ratio2_Q16 * detune_Q16) / 2^16)
      int32_t combined_Q16 = (int32_t)((((int64_t)ratio2_Q16 * (int64_t)detune_Q16) + (1LL << 15)) >> 16);
      freq_q24_B = (portamento_cur_freq_q24[DCO_B] * (int64_t)combined_Q16) >> 16;
      // Combine OSC3 ratio with detune into one Q16 factor
      int32_t detune3_Q16 = (int32_t)((((int64_t)detune3_q24) + 128) >> 8);
      int32_t combined3_Q16 = (int32_t)((((int64_t)ratio3_Q16 * (int64_t)detune3_Q16) + (1LL << 15)) >> 16);
      freq_q24_C = (portamento_cur_freq_q24[DCO_C] * (int64_t)combined3_Q16) >> 16;

#if DCO_DEBUG_REPORT
      // Debug: OSC1 frequency after all modifiers applied (in Hz)
      dbg_freq_after_mod_Hz = (float)freq_q24_A / (float)(1 << 24);
#endif


      BENCH_END(vt_freq_scale_post);

      // freq→divider inputs + dividers (was unattributed between post and clkdiv).
      BENCH_BEGIN(vt_clk_div);

      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t smAN = VOICE_TO_SM[DCO_A];
      uint8_t smBN = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      register uint32_t clk_div1, clk_div2, clk_div3;

      uint8_t arbitrary_measured_correction_value = 0; // 60 is a measured correction for the PIO

      uint32_t total_cycles1, total_cycles2, total_cycles3;
      // One load of cached clk_sys (see sys_clock_hz_refresh); never call clock_get_hz here.
      const uint32_t sys_hz = sysClock_Hz;

      // Period model per oscillator: period = Y + weight*clk_div + overhead.
      const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
      const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);
      const uint32_t wC = osc_ramp_weight(DCO_C), kC = osc_period_overhead(DCO_C);
      const uint32_t yA = osc_last_y[DCO_A];
      const uint32_t yB = osc_last_y[DCO_B];
      const uint32_t yC = osc_last_y[DCO_C];

      total_cycles1 = clkdiv_live_total_cycles(sys_hz, freq_q24_A);
      total_cycles2 = clkdiv_live_total_cycles(sys_hz, freq_q24_B);
      total_cycles3 = clkdiv_live_total_cycles(sys_hz, freq_q24_C);
      total_cycles1 += arbitrary_measured_correction_value;
      total_cycles2 += arbitrary_measured_correction_value;
      total_cycles3 += arbitrary_measured_correction_value;
      clk_div1 = pio_clk_div_for_y(total_cycles1, yA, wA, kA);
      clk_div2 = pio_clk_div_for_y(total_cycles2, yB, wB, kB);
      clk_div3 = pio_clk_div_for_y(total_cycles3, yC, wC, kC);
      BENCH_END(vt_clk_div);

      // Measured apart from the divider math because it only runs with sync on and a
      // non-zero phase offset; averaging the two together hides both.
      // Phase align + exact split: note-on only (I-cache next to each other).
      uint32_t phaseHoldX = 0;
      PioPeriod retrig_p1{};
      PioPeriod retrig_p2{};
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync > 1 && phaseAlignOSC2 != 0) {
        BENCH_BEGIN(vt_phase_align);
        phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
        BENCH_END(vt_phase_align);
      }
      // Mono EXACT_Y A+B stash only. Para splits inside its per-osc note-on loop.
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync >= 1 &&
          note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
        BENCH_BEGIN(vt_retrig_split);
        retrig_p1 = pio_period_split(total_cycles1, wA, kA);
        retrig_p2 = pio_period_split(total_cycles2, wB, kB);
        BENCH_END(vt_retrig_split);
      }

      BENCH_BEGIN(vt_chan_level);
      uint16_t chanLevel, chanLevel2, chanLevel3;
      amp_chan_levels_fixed(freq_q24_A, freq_q24_B, freq_q24_C, DCO_A, DCO_B, DCO_C,
                            &chanLevel, &chanLevel2, &chanLevel3);
      BENCH_END(vt_chan_level);

      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      osc_last_clk_div[DCO_C] = clk_div3;
      BENCH_END(vt_pio_write);

#ifdef ENABLE_SUBOSC_ENGINE2
      // Each sub counts flybacks on its own oscillator's RESET pin, so it needs that
      // oscillator's period, not this voice's DCO_A period.
      BENCH_BEGIN(vt_subosc);
      subosc2_update_periods(DCO_A, total_cycles1, DCO_B, total_cycles2, DCO_C, total_cycles3);
      BENCH_END(vt_subosc);
#endif

      // Mono: A+B oscSync retrig + RANGE A/B/C. Para: osc k only, no oscSync.
      if (voiceMode == 0) {
      if (note_on_flag_flag[i]) {
        BENCH_BEGIN(vt_note_retrig);
        // --- Reverse Calculation to find the expected output frequency ---
        uint32_t actual_total_osr_val = clk_div1 * wA;
        uint32_t actual_total_period = osc_last_y[DCO_A] + actual_total_osr_val + kA;
        float expected_freq = (double)sysClock_Hz / (double)actual_total_period;

#if DCO_DEBUG_REPORT
        // Exact split for the report only (live path splits inside oscSync below).
        PioPeriod p1dbg = pio_period_split(total_cycles1, wA, kA);
        // --- Print Diagnostic Report ---
        Serial.println("----------------[ DCO DEBUG REPORT ]----------------");
        Serial.printf("Target Freq In:   %.2f Hz\n", (float)freq_q24_A / (float)(1 << 24));
        Serial.printf("Total Cycles Calc:  %lu (Target for the whole period)\n", total_cycles1);
        Serial.printf("Reset pulse (Y):    %lu cycles (incl. period remainder)\n", p1dbg.y);
        Serial.printf("Period Overhead:    %lu cycles (program constant)\n", kA);
        Serial.printf("Total OSR Delay:    %lu cycles (Remaining for loops)\n", p1dbg.clk_div * wA);
        Serial.printf("clk_div (Exact):    %lu (Value sent to PIO)\n", p1dbg.clk_div);
        Serial.println("---");
        Serial.printf("Actual Period Gen:  %lu cycles (Y + (clk_div*%u) + overhead)\n",
                      actual_total_period, (unsigned)wA);
        Serial.printf("==> Expected Freq Out: %.2f Hz\n", expected_freq);
        Serial.println("---");

        Serial.println("OSC1 Frequency Stages:");
        Serial.printf("  Base after portamento:     %.4f Hz\n", dbg_freq_base_Hz);
        Serial.printf("  After modifiers (Q24):     %.4f Hz\n", dbg_freq_after_mod_Hz);
        Serial.printf("  Quantized by PIO (clkdiv): %.4f Hz\n", expected_freq);
        Serial.println("---");

        Serial.println("OSC1 Modifier Breakdown (Q24/Q16):");
        Serial.printf("  ADSRModifierOSC1_q24:      %.6f\n", (double)ADSRModifierOSC1_q24 / (double)(1 << 24));
        Serial.printf("  DETUNE_DRIFT_OSC1_q24:     %.6f\n", (double)DETUNE_DRIFT_OSC1_q24 / (double)(1 << 24));
        Serial.printf("  lfo1_pitch_mod OSC1:      %.6f\n", (double)lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] / (double)(1 << 24));
        Serial.printf("  unisonMODIFIER_q24:        %.6f\n", (double)unisonMODIFIER_OSC1_q24 / (double)(1 << 24));
        Serial.printf("  pitchbend_q24:             %.6f\n", (double)calcPitchbend_q24 / (double)(1 << 24));
        Serial.printf("  Q24_ONE_EPS:               %.6f\n", (double)Q24_ONE_EPS / (double)(1 << 24));
        Serial.printf("  modifiersBase_q24:         %.6f\n", (double)modifiersBase_q24 / (double)(1 << 24));
        Serial.println("OSC2 LFO2 pitch (fine + coarse):");
        Serial.printf("  lfo2_fine OSC2:            %.6f\n",
                      (double)applyDepthQ24(LFO2Level, LFO2toOSC2_q24) / (double)(1 << 24));
        Serial.printf("  lfo2_coarse OSC2:          %.6f\n",
                      (double)applyDepthQ24(LFO2Level, LFO2toOSC2_coarse_q24) / (double)(1 << 24));
        Serial.printf("  freq2Modifiers_q24:        %.6f\n", (double)freq2Modifiers_q24 / (double)(1 << 24));
        Serial.println("---");

        Serial.println("OSC1 Multiplier Table Inputs:");
        Serial.printf("  xScaled1_Q16 (natural):    %.6f\n", (double)xScaled1_Q16 / 65536.0);
        Serial.printf("  ratio1_Q16:                %.6f\n", (double)ratio1_Q16 / (double)(1 << 16));
        Serial.println("----------------------------------------------------\n");

#endif

        if (oscSync >= 1) {
          // One probe for the whole SM apply path — fine MAIN slices mis-attribute cold XIP.
          BENCH_BEGIN(vt_retrig_sm_apply);
          if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
            // Exact split was computed after phase_align (retrig_p1 / retrig_p2).
            // OSC3 is deliberately absent: never retriggered; Y stays at pioPulseLength.
            uint32_t maskAB = (1u << smAN) | (1u << smBN);
            pio_set_sm_mask_enabled(pio[0], maskAB, false);

            // Note-on is the one point where Y can be rewritten safely. Frame already did
            // put+pull, so TX is empty — fused noclear load (no FJOIN).
            osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
                                             DCO_B, retrig_p2.y, retrig_p2.clk_div);

            pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));

            if (phaseHoldX != 0) {
              osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
            } else {
              pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
            }

            pio_enable_sm_mask_in_sync(pio[0], maskAB);
          } else {
            // SYNC_JMP: phase resync only — no disable / Y load / enable_in_sync.
            // X preload needs a stopped SM, so degree offsets require EXACT_Y.
            pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));
            pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
          }
          BENCH_END(vt_retrig_sm_apply);
        }

        BENCH_BEGIN(vt_retrig_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_retrig_pwm);
        BENCH_END(vt_note_retrig);
      }

      } else {
        // Paraphonic: edge k → RANGE osc k only (same as mono with oscSync off).
        // No SM stop/restart — that phase-resets and clicks when porta keeps the amp open.
        // Pitch continues via the per-frame put/pull above. No oscSync / phase-align.
        uint16_t chanLevels[3];
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          chanLevels[0] = character_clamp_amp((int32_t)chanLevel + amp_j);
          chanLevels[1] = character_clamp_amp((int32_t)chanLevel2 + amp_j);
          chanLevels[2] = character_clamp_amp((int32_t)chanLevel3 + amp_j);
        } else {
          chanLevels[0] = chanLevel;
          chanLevels[1] = chanLevel2;
          chanLevels[2] = chanLevel3;
        }

        for (int k = 0; k < NUM_VOICES; k++) {
          if (!note_on_flag_flag[k]) continue;
          BENCH_BEGIN(vt_note_retrig);
          const uint8_t osc = (uint8_t)k;
          BENCH_BEGIN(vt_retrig_pwm);
          write_range_pwm(osc, chanLevels[k]);
          BENCH_END(vt_retrig_pwm);
          BENCH_END(vt_note_retrig);
        }
      }

      if (timer99microsFlag2) {
        BENCH_BEGIN(vt_range_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_range_pwm);

        // Shared PW PWM when any oscillator has analog Pulse enabled (DG411).
        const bool pulseOn = waveEnable[0][1] || waveEnable[1][1] || waveEnable[2][1];
        if (pulseOn) {
          const int16_t local_LFO2Level = LFO2Level;
          const int16_t local_LFO2toPW = LFO2toPW;
          BENCH_FBEGIN(vt_pwm_calc);
          // Optimized: This version avoids storing large intermediate products.
          // The multiplication and shift are combined into one expression per modulator,
          // allowing the compiler to make better use of registers.
          // int32: |q15|*|scale| and |LFO|*|LFO2toPW| fit before >> 15 (no int64 on M0+).
          int32_t adsr1_delta =
            ((int32_t)ADSR1Level_q15[i] * ADSR1toPWM_scale) >> 15;
          int32_t lfo2_delta =
            ((int32_t)local_LFO2Level * (int32_t)local_LFO2toPW) >> 15;
          int32_t pw_calc = (int32_t)DIV_COUNTER_PW - 1 - lfo2_delta - PW[0] + adsr1_delta
                            + character_pw_delta();

          if (pw_calc < 0) pw_calc = 0;
          if (pw_calc > (int32_t)DIV_COUNTER_PW - 1) pw_calc = (int32_t)DIV_COUNTER_PW - 1;
          PW_PWM[i] = (uint16_t)pw_calc;
          BENCH_FEND(vt_pwm_calc);

          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), get_PW_level_interpolated(PW_PWM[i], i));
          BENCH_END(vt_pw_update);

        } else {
          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
          BENCH_END(vt_pw_update);
        }
      }
  }

  for (int k = 0; k < NUM_VOICES; k++) {
    note_on_flag_flag[k] = false;
  }

  // Whole-task timing is taken by the BENCH_voice_task probe around voice_task_main() in
  // loop1(), so both engines are measured at the same boundary.

  // Update cached portamento parameters for next call
  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}

#endif  // !USE_FLOAT_VOICE_TASK

// Dispatch entry point: select float vs fixed-point implementation at compile time.
inline void voice_task_main() {
#ifdef USE_FLOAT_VOICE_TASK
  voice_task_float();
#else
  voice_task_fixed_point();
#endif
}

#ifdef USE_FLOAT_VOICE_TASK
// Float realtime voice engine (same stages as voice_task_fixed_point, in Hz). Board default on RP2350.
void __not_in_flash_func(voice_task_float)() {
    // --- Track portamento control changes exactly as in original ---
    static uint32_t last_portamento_time = 0;
    static uint8_t  last_portamento_mode = PORTA_MODE_SLEW;
    static uint8_t lastNote1 = 0, lastNote2 = 0, lastNote3 = 0;
    uint32_t portaTime = portamento_time;
    uint8_t  portaMode = portamento_mode;
    bool portaTimeChanged = (portaTime != last_portamento_time);
    bool portaModeChanged = (portaMode != last_portamento_mode);
  
    // --- 1. Pitch bend as float, equivalent to Q24 math ---
    BENCH_BEGIN(vt_pitchbend);
    // Use original float pitch bend behaviour, but derive multiplier from Q24
    float pitchBendMultiplier = q24_to_float(pitchBendMultiplier_q24);
    float calcPitchbend;

    if (midi_pitch_bend == 8192) {
      calcPitchbend = 0.0f;
    } else if (midi_pitch_bend < 8192) {
      calcPitchbend = (((float)midi_pitch_bend / 8190.99f) - 1.0f) * pitchBendMultiplier;
    } else {  // midi_pitch_bend > 8192
      calcPitchbend = (((float)midi_pitch_bend / 8192.99f) - 1.0f) * pitchBendMultiplier;
    }
    BENCH_END(vt_pitchbend);

    last_midi_pitch_bend = midi_pitch_bend;

    // Latch note-on edges for every voice slot (para may set flags on 1/2).
    for (int k = 0; k < NUM_VOICES; k++) {
      if (note_on_flag[k] == 1) {
        note_on_flag_flag[k] = true;
        note_on_flag[k] = 0;
      }
    }
  
    // --- 2. Per-voice loop (mirror original structure) ---
    for (int i = 0; i < NUM_VOICES_VOICE_TASK; ++i) {
  
  #if DCO_DEBUG_REPORT
      float dbg_freq_base_Hz      = 0.0f;
      float dbg_freq_after_mod_Hz = 0.0f;
  #endif

      // note_off keeps VOICE_NOTES (release pitch); free/steal uses VOICES[].
      // Never-played slots stay 0 — gate midi→table math (uint8 underflow → shriek).

      // --- 2.1 Note indices ---
      // Note index convention: table_index = midi - 36 + offset (36 ⇒ unison).
      //   Mono: octave_shift = global octave; OSC2/3 = relative to note1 (36 ⇒ unison with OSC1).
      //   Poly/stack: every osc uses octave_shift only (shared octave); not OSC2/3.
      const size_t NOTE_TABLE_LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
      uint8_t note1, note2, note3;
      if (voiceMode == 0) {
        const uint8_t vn = VOICE_NOTES[i];
        if (vn == 0) {
          note1 = note2 = note3 = 0;
        } else {
          note1 = midi_offset_to_table_index((int)vn, (int)octave_shift, NOTE_TABLE_LEN);
          note2 = midi_offset_to_table_index((int)note1, (int)OSC2_interval, NOTE_TABLE_LEN);
          note3 = midi_offset_to_table_index((int)note1, (int)OSC3_interval, NOTE_TABLE_LEN);
        }
      } else {
        const uint8_t vn0 = VOICE_NOTES[0];
        const uint8_t vn1 = VOICE_NOTES[1];
        const uint8_t vn2 = VOICE_NOTES[2];
        note1 = (vn0 == 0) ? 0 : midi_offset_to_table_index((int)vn0, (int)octave_shift, NOTE_TABLE_LEN);
        note2 = (vn1 == 0) ? 0 : midi_offset_to_table_index((int)vn1, (int)octave_shift, NOTE_TABLE_LEN);
        note3 = (vn2 == 0) ? 0 : midi_offset_to_table_index((int)vn2, (int)octave_shift, NOTE_TABLE_LEN);
      }

      const bool pitchTargetChanged =
          note1 != lastNote1 || note2 != lastNote2 || note3 != lastNote3;
      lastNote1 = note1;
      lastNote2 = note2;
      lastNote3 = note3;
  
      BENCH_BEGIN(vt_osc_detune);
      // --- 2.2 OSC2/OSC3 detune (float equivalent of Q24) ---
      float detuneSteps = (float)((int)256 - OSC2DetuneVal);
      float osc2DetuneRatio = 1.0f + 0.0002f * detuneSteps;
      float detune3Steps = (float)((int)256 - OSC3DetuneVal);
      float osc3DetuneRatio = 1.0f + 0.0002f * detune3Steps;
      BENCH_END(vt_osc_detune);
  
      // base note frequencies from float table
      float noteFreq1 = sNotePitches[note1];
      float noteFreq2 = sNotePitches[note2];
      float noteFreq3 = sNotePitches[note3];
  
      float freqA, freqB, freqC;      // will hold the portamento-processed base freqs
  
      const uint8_t DCO_A = 0;
      const uint8_t DCO_B = 1;
      const uint8_t DCO_C = 2;
  
      // --- 2.3 Portamento (time mode & slew mode), float-only implementation ---
      BENCH_BEGIN(vt_portamento);

      if (portaTime > 0) {
        uint32_t now_us = micros();
        if (voiceMode == 0) {
          portamentoTimer[i] = now_us - portamentoStartMicros[i];
        } else {
          for (int k = 0; k < NUM_VOICES; k++) {
            portamentoTimer[k] = now_us - portamentoStartMicros[k];
          }
        }

        // Mono: one edge restarts all three oscs. Para: each note_on_flag_flag[k] restarts osc k only.
        // TIME = fixed duration any interval; SLEW = constant semitone rate (time ∝ interval).
        if (voiceMode == 0) {
        if (note_on_flag_flag[i]) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i]       = 0;

          float targetNoteA = (float)note1;
          float targetNoteB = (float)note2;
          float targetNoteC = (float)note3;
          porta_setup_glide_f(DCO_A, porta_resolve_start_note_f(DCO_A, targetNoteA),
                              targetNoteA, portaMode);
          porta_setup_glide_f(DCO_B, porta_resolve_start_note_f(DCO_B, targetNoteB),
                              targetNoteB, portaMode);
          porta_setup_glide_f(DCO_C, porta_resolve_start_note_f(DCO_C, targetNoteC),
                              targetNoteC, portaMode);
        }
        } else {
          // Paraphonic: restart porta per voice slot → osc k (1:1).
          const int targetNotes[3] = { note1, note2, note3 };
          for (int k = 0; k < NUM_VOICES; k++) {
            if (!note_on_flag_flag[k]) continue;
            const uint8_t osc = (uint8_t)k;
            portamentoStartMicros[k] = now_us;
            portamentoTimer[k] = 0;
            float targetNote = (float)targetNotes[k];
            porta_setup_glide_f(osc, porta_resolve_start_note_f(osc, targetNote),
                                targetNote, portaMode);
          }
        }

        // Control-change frames: skip steady (stale start/step + huge elapsed → pitch jump).
        const bool portaNoteOnEdge =
            (voiceMode == 0) ? note_on_flag_flag[i]
                             : (note_on_flag_flag[0] || note_on_flag_flag[1] || note_on_flag_flag[2]);
        const bool portaDoRetime =
            (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !portaNoteOnEdge;

        float curA, curB, curC;
        if (portaDoRetime) {
          portamentoStartMicros[i] = now_us;
          portamentoTimer[i]       = 0;
          if (voiceMode != 0) {
            for (int k = 0; k < NUM_VOICES; k++) {
              portamentoStartMicros[k] = now_us;
              portamentoTimer[k]       = 0;
            }
          }

          float curNoteA = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_A]);
          float curNoteB = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_B]);
          float curNoteC = freqFloat_to_noteIndex(porta_freq_cur_f[DCO_C]);
          porta_setup_glide_f(DCO_A, curNoteA, (float)note1, portaMode);
          porta_setup_glide_f(DCO_B, curNoteB, (float)note2, portaMode);
          porta_setup_glide_f(DCO_C, curNoteC, (float)note3, portaMode);
          curA = porta_freq_cur_f[DCO_A];
          curB = porta_freq_cur_f[DCO_B];
          curC = porta_freq_cur_f[DCO_C];
        } else if (porta_note_cur_f[DCO_A] == porta_note_stop_f[DCO_A] &&
                   porta_note_cur_f[DCO_B] == porta_note_stop_f[DCO_B] &&
                   porta_note_cur_f[DCO_C] == porta_note_stop_f[DCO_C]) {
          curA = porta_freq_stop_f[DCO_A];
          curB = porta_freq_stop_f[DCO_B];
          curC = porta_freq_stop_f[DCO_C];
        } else {
          int32_t elapsedA = (voiceMode == 0) ? (int32_t)portamentoTimer[i] : (int32_t)portamentoTimer[0];
          int32_t elapsedB = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[1];
          int32_t elapsedC = (voiceMode == 0) ? elapsedA : (int32_t)portamentoTimer[2];

          float startNoteA = porta_note_start_f[DCO_A];
          float startNoteB = porta_note_start_f[DCO_B];
          float startNoteC = porta_note_start_f[DCO_C];
          float stopNoteA  = porta_note_stop_f [DCO_A];
          float stopNoteB  = porta_note_stop_f [DCO_B];
          float stopNoteC  = porta_note_stop_f [DCO_C];

          float dNoteA = stopNoteA - startNoteA;
          float dNoteB = stopNoteB - startNoteB;
          float dNoteC = stopNoteC - startNoteC;

          float curNoteA = startNoteA + porta_note_step_f[DCO_A] * (float)elapsedA;
          float curNoteB = startNoteB + porta_note_step_f[DCO_B] * (float)elapsedB;
          float curNoteC = startNoteC + porta_note_step_f[DCO_C] * (float)elapsedC;

          if ((dNoteA >= 0.0f && curNoteA >= stopNoteA) ||
              (dNoteA <  0.0f && curNoteA <= stopNoteA)) {
            curNoteA = stopNoteA;
          }
          if ((dNoteB >= 0.0f && curNoteB >= stopNoteB) ||
              (dNoteB <  0.0f && curNoteB <= stopNoteB)) {
            curNoteB = stopNoteB;
          }
          if ((dNoteC >= 0.0f && curNoteC >= stopNoteC) ||
              (dNoteC <  0.0f && curNoteC <= stopNoteC)) {
            curNoteC = stopNoteC;
          }

          porta_note_cur_f[DCO_A] = curNoteA;
          porta_note_cur_f[DCO_B] = curNoteB;
          porta_note_cur_f[DCO_C] = curNoteC;

          // At stop: use stop Hz (never live noteFreq* — that raced with note_on_flag).
          curA = (curNoteA == stopNoteA) ? porta_freq_stop_f[DCO_A]
                                         : noteIndex_to_freqFloat(curNoteA);
          curB = (curNoteB == stopNoteB) ? porta_freq_stop_f[DCO_B]
                                         : noteIndex_to_freqFloat(curNoteB);
          curC = (curNoteC == stopNoteC) ? porta_freq_stop_f[DCO_C]
                                         : noteIndex_to_freqFloat(curNoteC);

          porta_freq_cur_f[DCO_A] = curA;
          porta_freq_cur_f[DCO_B] = curB;
          porta_freq_cur_f[DCO_C] = curC;
        }

        freqA = curA;
        freqB = curB;
        freqC = curC;

      } else {
        // No portamento
        freqA = noteFreq1;
        freqB = noteFreq2;
        freqC = noteFreq3;

        // Keep float portamento state coherent when portamento is off.
        porta_freq_cur_f[DCO_A] = freqA;
        porta_freq_cur_f[DCO_B] = freqB;
        porta_freq_cur_f[DCO_C] = freqC;
        porta_freq_stop_f[DCO_A] = freqA;
        porta_freq_stop_f[DCO_B] = freqB;
        porta_freq_stop_f[DCO_C] = freqC;
        porta_note_cur_f[DCO_A] = (float)note1;
        porta_note_cur_f[DCO_B] = (float)note2;
        porta_note_cur_f[DCO_C] = (float)note3;
        porta_note_stop_f[DCO_A] = (float)note1;
        porta_note_stop_f[DCO_B] = (float)note2;
        porta_note_stop_f[DCO_C] = (float)note3;
        porta_note_valid[DCO_A] = true;
        porta_note_valid[DCO_B] = true;
        porta_note_valid[DCO_C] = true;
      }

#if defined(BENCH_PATH_STATS)
      // Exclusive path tag for jitter attribution (note_on > retime > steady > off).
      if (portaTime == 0) {
        BENCH_PATH_INC(porta_off);
      } else if (note_on_flag_flag[i]) {
        BENCH_PATH_INC(porta_note_on);
      } else if (portaTimeChanged || portaModeChanged || pitchTargetChanged) {
        BENCH_PATH_INC(porta_retime);
      } else if (portaMode == PORTA_MODE_TIME) {
        BENCH_PATH_INC(porta_steady_time);
      } else {
        BENCH_PATH_INC(porta_steady_slew);
      }
#endif

  #if DCO_DEBUG_REPORT
      dbg_freq_base_Hz = freqA;
  #endif
  
      BENCH_END(vt_portamento);

      // --- 2.4 ADSR detune (float equivalent of Q24) ---
      BENCH_BEGIN(vt_adsr_mod);
      float ADSRModifierOSC1 = 0.0f;
      float ADSRModifierOSC2 = 0.0f;
      float ADSRModifierOSC3 = 0.0f;
      if (ADSR1toDETUNE1_scale_q24 != 0) {
        if (voiceMode == 0) {
          const float m = q24_to_float(applyDepthQ24(
              env_dco_pitch_wave_q15(ADSR1Level_q15[i]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1 = m;
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2 = m;
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3 = m;
        } else {
          if (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC1 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[0]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC2 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[1]), ADSR1toDETUNE1_scale_q24));
          if (ADSR3ToOscSelect == 3 || ADSR3ToOscSelect == 4)
            ADSRModifierOSC3 = q24_to_float(applyDepthQ24(
                env_dco_pitch_wave_q15(ADSR1Level_q15[2]), ADSR1toDETUNE1_scale_q24));
        }
      }
      BENCH_END(vt_adsr_mod);

      BENCH_BEGIN(vt_unison_mod);
      // --- 2.5 Unison modifier (float equivalent of Q24 0.0001 * unisonDetune * step) ---
      static constexpr float UNISON_SCALE = 0.0001f;
      static constexpr int32_t OSC_UNISON_STEP[3] = { 0, 1, -1 };
      const float unisonBase = (float)unisonDetune * UNISON_SCALE;
      float unisonMODIFIER_OSC1 = unisonBase * (float)OSC_UNISON_STEP[0];
      float unisonMODIFIER_OSC2 = unisonBase * (float)OSC_UNISON_STEP[1];
      float unisonMODIFIER_OSC3 = unisonBase * (float)OSC_UNISON_STEP[2];
#if NUM_VOICES_TOTAL > 1
      // Classic poly voice-index alternating pattern on top of per-osc spread.
      float voiceMag = (float)((i >> 1) + 1);
      float voiceSign = ((i & 0x01) == 0) ? 1.0f : -1.0f;
      float voiceUnison = unisonBase * (voiceSign * voiceMag);
      unisonMODIFIER_OSC1 += voiceUnison;
      unisonMODIFIER_OSC2 += voiceUnison;
      unisonMODIFIER_OSC3 += voiceUnison;
#endif
      BENCH_END(vt_unison_mod);

      BENCH_BEGIN(vt_drift_mod);
      // Snapshot Core 0 mailbox once, then applyDepthQ24 (same as fixed / ADSR detune).
      const int32_t driftScale_q24 = drift_pitch_scale_q24;
      const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
      const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
      const int16_t driftC = LFO_DRIFT_LEVEL[DCO_C];
      float DETUNE_DRIFT_OSC1 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftA, driftScale_q24)) : 0.0f;
      float DETUNE_DRIFT_OSC2 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftB, driftScale_q24)) : 0.0f;
      float DETUNE_DRIFT_OSC3 =
        (driftScale_q24 != 0) ? q24_to_float(applyDepthQ24(driftC, driftScale_q24)) : 0.0f;
      BENCH_END(vt_drift_mod);

      float freqModifiers1;
      float freqModifiers2;
      float freqModifiers3;
      {
        // Snapshot LFO pitch mailbox here only (short live range; same as fixed).
        const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
        const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
        const int32_t local_lfo1_osc3 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC3];
        const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
        const int32_t local_lfo2_osc3 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC3];
        BENCH_BEGIN(vt_modifiers);
        float lfo1_osc1        = q24_to_float(local_lfo1_osc1);
        float lfo1_osc2        = q24_to_float(local_lfo1_osc2);
        float lfo1_osc3        = q24_to_float(local_lfo1_osc3);
        float lfo2_osc2        = q24_to_float(local_lfo2_osc2);
        float lfo2_osc3        = q24_to_float(local_lfo2_osc3);
        float eps              = q24_to_float(Q24_ONE_EPS);
        float pitchBendF   = calcPitchbend;
  
        // Character pitch jitter stacks after matrix when scale nonzero.
        float modifiersBase = pitchBendF + eps + q24_to_float(matrix_pitch_mod_q24);
        if (char_pitch_scale_q15) {
          modifiersBase += q24_to_float(character_pitch_delta_q24());
        }
        freqModifiers1 = ADSRModifierOSC1 + DETUNE_DRIFT_OSC1 + modifiersBase + unisonMODIFIER_OSC1 + lfo1_osc1;
        freqModifiers2 = ADSRModifierOSC2 + DETUNE_DRIFT_OSC2 + modifiersBase + unisonMODIFIER_OSC2 + lfo1_osc2 + lfo2_osc2;
        freqModifiers3 = ADSRModifierOSC3 + DETUNE_DRIFT_OSC3 + modifiersBase + unisonMODIFIER_OSC3 + lfo1_osc3 + lfo2_osc3;
        BENCH_END(vt_modifiers);
      }

      BENCH_BEGIN(vt_freq_scale_x);
      // Q12 ×10000 glue (if any) lives inside interpolate_live_ratio_f.
      BENCH_END(vt_freq_scale_x);

      BENCH_BEGIN(vt_ratio_interp);
      float ratio1 = interpolate_live_ratio_f(freqModifiers1, DCO_A);
      float ratio2 = interpolate_live_ratio_f(freqModifiers2, DCO_B);
      float ratio3 = interpolate_live_ratio_f(freqModifiers3, DCO_C);
      BENCH_END(vt_ratio_interp);

      BENCH_BEGIN(vt_freq_scale_post);
      // Apply ratios to portamento frequencies, with osc2/osc3 detune (Hz domain).
      float freqA_Hz = freqA * ratio1;
      float freqB_Hz = freqB * (ratio2 * osc2DetuneRatio);
      float freqC_Hz = freqC * (ratio3 * osc3DetuneRatio);

  #if DCO_DEBUG_REPORT
      dbg_freq_after_mod_Hz = freqA_Hz;
  #endif

      BENCH_END(vt_freq_scale_post);

      // --- 2.8 Clock divider calculation (float equivalent) ---
      BENCH_BEGIN(vt_clk_div);

      float correction = 0.0f;   // keep your measured correction if needed
      const uint32_t sys_hz = sysClock_Hz;
      uint32_t total_cycles1 = clkdiv_live_hz_total_cycles(sys_hz, freqA_Hz)
                               + (uint32_t)correction;
      uint32_t total_cycles2 = clkdiv_live_hz_total_cycles(sys_hz, freqB_Hz)
                               + (uint32_t)correction;
      uint32_t total_cycles3 = clkdiv_live_hz_total_cycles(sys_hz, freqC_Hz)
                               + (uint32_t)correction;

      const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
      const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);
      const uint32_t wC = osc_ramp_weight(DCO_C), kC = osc_period_overhead(DCO_C);

      uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
      uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);
      uint32_t clk_div3 = pio_clk_div_for_y(total_cycles3, osc_last_y[DCO_C], wC, kC);
      BENCH_END(vt_clk_div);

      // Measured apart from the divider math because it only runs with sync on and a
      // non-zero phase offset; averaging the two together hides both.
      // Phase align + exact split: note-on only (I-cache next to each other).
      uint32_t phaseHoldX = 0;
      PioPeriod retrig_p1{};
      PioPeriod retrig_p2{};
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync > 1 && phaseAlignOSC2 != 0) {
        BENCH_BEGIN(vt_phase_align);
        phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
        BENCH_END(vt_phase_align);
      }
      // Mono EXACT_Y A+B stash only. Para splits inside its per-osc note-on loop.
      if (voiceMode == 0 && note_on_flag_flag[i] && oscSync >= 1 &&
          note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
        BENCH_BEGIN(vt_retrig_split);
        retrig_p1 = pio_period_split(total_cycles1, wA, kA);
        retrig_p2 = pio_period_split(total_cycles2, wB, kB);
        BENCH_END(vt_retrig_split);
      }

      // --- 2.9 Amplitude compensation using engine-agnostic helper ---
      BENCH_BEGIN(vt_chan_level);

      uint16_t chanLevel, chanLevel2, chanLevel3;
      switch (syncMode) {
        case 0:
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        case 1: {
          float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
          chanLevel  = get_chan_level_for_engine(maxFreq, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        }
        case 2: {
          float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(maxFreq, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
        }
        default:
          chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
          chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
          chanLevel3 = get_chan_level_for_engine(freqC_Hz, DCO_C);
          break;
      }
      BENCH_END(vt_chan_level);

      // --- 2.10 PIO + PWM + PW math (very close to original, but float inside PW calc) ---
      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      PIO pioN_C = pio[VOICE_TO_PIO[DCO_C]];
      uint8_t sm1N = VOICE_TO_SM[DCO_A];
      uint8_t sm2N = VOICE_TO_SM[DCO_B];
      uint8_t smCN = VOICE_TO_SM[DCO_C];

      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, sm1N, clk_div1);
      pio_sm_put(pioN_B, sm2N, clk_div2);
      pio_sm_put(pioN_C, smCN, clk_div3);
      pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_C, smCN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      osc_last_clk_div[DCO_C] = clk_div3;
      BENCH_END(vt_pio_write);

#ifdef ENABLE_SUBOSC_ENGINE2
      // Each sub counts flybacks on its own oscillator's RESET pin, so it needs that
      // oscillator's period, not this voice's DCO_A period.
      BENCH_BEGIN(vt_subosc);
      subosc2_update_periods(DCO_A, total_cycles1, DCO_B, total_cycles2, DCO_C, total_cycles3);
      BENCH_END(vt_subosc);
#endif

      // Mono: A+B oscSync retrig + RANGE A/B/C. Para: osc k only, no oscSync.
      if (voiceMode == 0) {
      if (note_on_flag_flag[i]) {
        BENCH_BEGIN(vt_note_retrig);
        // Sync logic mirrored from fixed-point voice_task_fixed_point, using float-derived periods.
        if (oscSync >= 1) {
          // One probe for the whole SM apply path — fine MAIN slices mis-attribute cold XIP.
          BENCH_BEGIN(vt_retrig_sm_apply);
          if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
            // Exact split was computed after phase_align (retrig_p1 / retrig_p2).
            // OSC3 is deliberately absent: never retriggered; Y stays at pioPulseLength.
            uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
            pio_set_sm_mask_enabled(pio[0], maskAB, false);

            // Frame already did put+pull, so TX is empty — fused noclear load (no FJOIN).
            osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
                                             DCO_B, retrig_p2.y, retrig_p2.clk_div);

            pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

            if (phaseHoldX != 0) {
              osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
            } else {
              pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
            }

            pio_enable_sm_mask_in_sync(pio[0], maskAB);
          } else {
            // SYNC_JMP: phase resync only — no disable / Y load / enable_in_sync.
            // X preload needs a stopped SM, so degree offsets require EXACT_Y.
            pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
            pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
          }
          BENCH_END(vt_retrig_sm_apply);
        }

        BENCH_BEGIN(vt_retrig_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_retrig_pwm);
        BENCH_END(vt_note_retrig);
      }

      } else {
        // Paraphonic: edge k → RANGE osc k only (same as mono with oscSync off).
        // No SM stop/restart — that phase-resets and clicks when porta keeps the amp open.
        // Pitch continues via the per-frame put/pull above. No oscSync / phase-align.
        uint16_t chanLevels[3];
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          chanLevels[0] = character_clamp_amp((int32_t)chanLevel + amp_j);
          chanLevels[1] = character_clamp_amp((int32_t)chanLevel2 + amp_j);
          chanLevels[2] = character_clamp_amp((int32_t)chanLevel3 + amp_j);
        } else {
          chanLevels[0] = chanLevel;
          chanLevels[1] = chanLevel2;
          chanLevels[2] = chanLevel3;
        }

        for (int k = 0; k < NUM_VOICES; k++) {
          if (!note_on_flag_flag[k]) continue;
          BENCH_BEGIN(vt_note_retrig);
          const uint8_t osc = (uint8_t)k;
          BENCH_BEGIN(vt_retrig_pwm);
          write_range_pwm(osc, chanLevels[k]);
          BENCH_END(vt_retrig_pwm);
          BENCH_END(vt_note_retrig);
        }
      }
  
      if (timer99microsFlag2) {
        BENCH_BEGIN(vt_range_pwm);
        if (char_amp_scale_q15) {
          const int32_t amp_j = character_amp_delta();
          write_range_pwm(DCO_A, character_clamp_amp((int32_t)chanLevel + amp_j));
          write_range_pwm(DCO_B, character_clamp_amp((int32_t)chanLevel2 + amp_j));
          write_range_pwm(DCO_C, character_clamp_amp((int32_t)chanLevel3 + amp_j));
        } else {
          write_range_pwm(DCO_A, chanLevel);
          write_range_pwm(DCO_B, chanLevel2);
          write_range_pwm(DCO_C, chanLevel3);
        }
        BENCH_END(vt_range_pwm);

        const bool pulseOn = waveEnable[0][1] || waveEnable[1][1] || waveEnable[2][1];
        if (pulseOn) {
          const int16_t local_LFO2Level = LFO2Level;
          const int16_t local_LFO2toPW = LFO2toPW;
          BENCH_FBEGIN(vt_pwm_calc);
          float adsr1_delta =
            ((float)ADSR1Level_q15[i] * (float)ADSR1toPWM_scale) * (1.0f / 32768.0f);
          float lfo2_delta =
            ((float)local_LFO2Level * (float)local_LFO2toPW) * (1.0f / 32767.0f);
          float pw_calc =
              (float)DIV_COUNTER_PW - 1.0f
            - (float)PW[0]
            - lfo2_delta
            + adsr1_delta
            + (float)character_pw_delta();
  
          if (pw_calc < 0.0f) pw_calc = 0.0f;
          if (pw_calc > (float)(DIV_COUNTER_PW - 1)) pw_calc = (float)(DIV_COUNTER_PW - 1);
  
          PW_PWM[i] = (uint16_t)pw_calc;
          BENCH_FEND(vt_pwm_calc);

          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i],
                             pwm_gpio_to_channel(PW_PINS[i]),
                             get_PW_level_interpolated(PW_PWM[i], i));
          BENCH_END(vt_pw_update);
        } else {
          BENCH_BEGIN(vt_pw_update);
          pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), 0);
          BENCH_END(vt_pw_update);
        }
      }
    }

    for (int k = 0; k < NUM_VOICES; k++) {
      note_on_flag_flag[k] = false;
    }

  // Whole-task timing is taken by the BENCH_voice_task probe around voice_task_main() in
  // loop1(), so both engines are measured at the same boundary.

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}
#endif  // USE_FLOAT_VOICE_TASK

// --- Voice allocation --------------------------------------------------------
// Thin adapters over the shared allocator (DCO-SHARED-LIBRARIES/voice_alloc.h,
// instance in voice_alloc_state.h), which replaced get_free_voice() and
// get_free_voice_sequential(). Every policy in VoiceAllocMode lives there;
// these keep the sketch's gate flag, pitch table and ADSR edge flags in step
// with the allocator's bookkeeping so no caller has to update both.

// Choose a voice for an incoming note. Returns VOICE_ALLOC_NONE when the mode
// refuses to steal.
uint8_t voice_alloc() {
  return voiceAlloc.alloc();
}

// Mark a voice as sounding a new note. Shared by every note_on path so the
// allocation bookkeeping never drifts from the gate flags.
void voice_mark_on(uint8_t voice, uint8_t note, uint8_t velocity) {
  VOICES[voice] = 1;
  VOICE_NOTES[voice] = note;
  midi_velocity[voice] = velocity;
  note_on_flag[voice] = 1;
  noteStart[voice] = 1;
  noteEnd[voice] = 0;
  voiceAlloc.markOn(voice, note);
}

// Gate a voice off and start tracking its release tail.
void voice_mark_off(uint8_t voice) {
  VOICES[voice] = 0;
  noteEnd[voice] = 1;
  noteStart[voice] = 0;
  voiceAlloc.markOff(voice);
}

// Map voiceMode → NUM_VOICES / STACK_VOICES. Called from init_voices and apply_param_voice_mode.
//   0 mono:       one MIDI voice → oscs 0..2 (engine still bound by NUM_VOICES_VOICE_TASK)
//   1 paraphonic: up to TOTAL notes, voice i → osc i (EnvDCO pitch tap per osc)
//   2 stub:       DCO4 stack leftover — counts only; no new stack behavior
inline void setVoiceMode() {
  // Resync allocation state to the gates: a slot that NUM_VOICES dropped mid-note
  // would otherwise come back HELD when the count grows again.
  voiceAlloc.resyncFromGates(VOICES);

  switch (voiceMode) {
    case 0:
      NUM_VOICES = 1;
      STACK_VOICES = 1;
      // Drop any stale held notes when entering mono.
      mono_note_stack_clear();
      break;
    case 1:
      NUM_VOICES = NUM_VOICES_TOTAL;
      STACK_VOICES = 1;
      break;
    case 2:
      NUM_VOICES = NUM_VOICES_TOTAL;
      STACK_VOICES = NUM_VOICES_TOTAL;
      break;
  }

  voiceAlloc.setVoiceCount(NUM_VOICES);
}

// Rebuild the PIO sync topology and retrigger voices.
// Called from apply_param_sync_mode (Serial2).
void setSyncMode() {
  // assign_sm_mapping() keeps the slave below its master in SM index; start_voice_sms()
  // re-derives every SM's program, set pin and sideset pin from syncMode and
  // softSyncChunks, then starts them all on the same cycle.
  //
  // The old implementation poked sideset pins in place and called pio_sm_restart(),
  // which cleared the shift counters but left PC, X and Y — it could strand an SM
  // mid-loop with a stale X for one glitched period. The note_on_flag retrigger below
  // already re-pushes everything, so the restart was never needed.
  assign_sm_mapping();
  start_voice_sms();

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

// One osc: shipping FIXED = Q24→Q8 lookup. With USE_FLOAT_AMP_COMP, non-FIXED
// methods take Q24→Hz then get_chan_level_by_method (cmds 20–22).
static inline __attribute__((always_inline)) uint16_t amp_level_q24(int64_t freq_q24, uint8_t osc) {
#ifdef USE_FLOAT_AMP_COMP
  if (amp_comp_method != AMP_COMP_FIXED) {
    float hz = (float)freq_q24 * (1.0f / 16777216.0f);
    return get_chan_level_by_method(hz, osc);
  }
#endif
  int32_t freqFx = (int32_t)((freq_q24 + (1LL << 15)) >> 16);
  return get_chan_level_lookup_fast(freqFx, osc);
}

// Q24 + syncMode switch + 3× lookup. SRAM so vt_chan_level is not a flash caller.
static void __not_in_flash_func(amp_chan_levels_fixed)(int64_t freq_q24_A, int64_t freq_q24_B,
                                                      int64_t freq_q24_C, uint8_t oscA, uint8_t oscB,
                                                      uint8_t oscC, uint16_t *outA, uint16_t *outB,
                                                      uint16_t *outC) {
  const uint8_t sm = syncMode;
  switch (sm) {
    case 1: {
      int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
      *outA = amp_level_q24(maxAB, oscA);
      *outB = amp_level_q24(freq_q24_B, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
    }
    case 2: {
      int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
      *outA = amp_level_q24(freq_q24_A, oscA);
      *outB = amp_level_q24(maxAB, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
    }
    default:
      *outA = amp_level_q24(freq_q24_A, oscA);
      *outB = amp_level_q24(freq_q24_B, oscB);
      *outC = amp_level_q24(freq_q24_C, oscC);
      break;
  }
}

/**
 * @brief Fast amplitude compensation lookup (Q8 Hz) for fixed path / AMP_COMP_FIXED.
 *
 * Window rule matches float: first i with freqRow[i] <= x < freqRow[i+2].
 * Find: per-osc ampWinCache → walk → full scan (same as FLOAT_QUAD).
 */
uint16_t __not_in_flash_func(get_chan_level_lookup_fast)(int32_t x, uint8_t voiceN) {
  const int32_t* freqRow = ampCompFrequencyArray[voiceN];
  const int32_t* ampRow  = ampCompArray[voiceN];
  const FixedQuadWindow* winRow = fixedWin[voiceN];

  if (x <= freqRow[0]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)ampRow[0];
  }

  // Domain ceiling: at/above AMP_COMP_MAX_HZ the cal sentinel is full RANGE.
  // Without this, x == MAX_Q matches no window (exclusive upper bound) and the
  // fallback window-0 path clamps t∈[0,1] to the first segment — ~12k PWM off
  // float, which extrapolates absolute Hz on that window to ~DIV_COUNTER.
  if (x >= AMP_COMP_MAX_HZ_Q) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  // Real plateau only: precompute leaves plateauStartFreqQ at AMP_COMP_MAX_HZ_Q
  // when none was found (same role as plateauStartIndex < 0 in the original code).
  // Do not use plateauStartIndex here — dual-build restores that for the float path.
  if (plateauStartFreqQ[voiceN] < AMP_COMP_MAX_HZ_Q &&
      x >= plateauStartFreqQ[voiceN]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  const int maxWindow = ampCompTableSize - 2;
  int window = ampWinCache[voiceN];

  if (window >= 0 && window <= maxWindow &&
      x >= freqRow[window] && x < freqRow[window + 2]) {
    BENCH_PATH_INC(amp_hit);
  } else {
    int cand = window;
    if (cand < 0 || cand > maxWindow) cand = 0;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (x >= freqRow[cand + 2]) {
      while (cand < maxWindow && x >= freqRow[cand + 2]) {
        ++cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    } else if (x < freqRow[cand]) {
      while (cand > 0 && x < freqRow[cand]) {
        --cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    }
    if (cand >= 0 && cand <= maxWindow &&
        x >= freqRow[cand] && x < freqRow[cand + 2]) {
      window = cand;
      BENCH_PATH_INC(amp_miss_walk);
#if defined(BENCH_PATH_STATS)
      bench_path_amp_walk_steps(steps);
#endif
    } else {
      window = 0;
      for (int i = 0; i <= maxWindow; ++i) {
        if (x >= freqRow[i] && x < freqRow[i + 2]) {
          window = i;
          break;
        }
      }
      BENCH_PATH_INC(amp_miss_scan);
    }
    ampWinCache[voiceN] = (int16_t)window;
  }

  int32_t dx = x - winRow[window].xBase;
  const int32_t span = winRow[window].dx;
  if (dx < 0) dx = 0;
  if (dx > span) dx = span;

  const uint32_t inv_q28 = winRow[window].invDx_q28;
  uint32_t t_q = (uint32_t)(((uint64_t)dx * inv_q28) >> (28 - T_FRAC));

  const int32_t a = winRow[window].aQ_fast;
  const int32_t b = winRow[window].bQ_fast;
  const int32_t c = (int32_t)winRow[window].cQ;

  uint32_t t2 = (uint32_t)(((uint32_t)t_q * t_q) >> T_FRAC);
  int32_t term_a, term_b;
  if (amp_quad_muls_i32) {
    term_a = (a * (int32_t)t2) >> T_FRAC;
    term_b = (b * (int32_t)t_q) >> T_FRAC;
  } else {
    term_a = (int32_t)(((int64_t)a * (int64_t)t2) >> T_FRAC);
    term_b = (int32_t)(((int64_t)b * (int64_t)t_q) >> T_FRAC);
  }

  int32_t y_q = term_a + term_b + (c << T_FRAC);
  int32_t y = (y_q + (1 << (T_FRAC - 1))) >> T_FRAC;

  if (y < 0) y = 0;
  if (y > (int32_t)DIV_COUNTER) y = (int32_t)DIV_COUNTER;

  return (uint16_t)y;
}

#ifdef USE_FLOAT_AMP_COMP
/**
 * @brief Pure-float quadratic amp-comp (Hz domain). Live FLOAT_QUAD.
 * Window find: per-osc cache → walk → full scan (first-match [Hz[w], Hz[w+2]]).
 */
uint16_t __not_in_flash_func(get_chan_level_float_quad)(float freqHz, uint8_t voiceN) {
  if (freqHz <= ampCompFrequencyHz[voiceN][0]) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)ampCompArray[voiceN][0];
  }

  // Same domain ceiling as FIXED (see get_chan_level_lookup_fast).
  if (freqHz >= (float)AMP_COMP_MAX_HZ) {
    BENCH_PATH_INC(amp_clamp);
    return (uint16_t)DIV_COUNTER;
  }

  if (plateauStartIndex[voiceN] >= 0) {
    float plateauFreqHz = plateauStartFreqHz[voiceN];
    if (freqHz >= plateauFreqHz) {
      BENCH_PATH_INC(amp_clamp);
      return (uint16_t)DIV_COUNTER;
    }
  }

  const int maxWindow = ampCompTableSize - 2;
  const float *hzRow = ampCompFrequencyHz[voiceN];
  int window = ampWinCache[voiceN];

  if (window >= 0 && window <= maxWindow &&
      freqHz >= hzRow[window] && freqHz < hzRow[window + 2]) {
    BENCH_PATH_INC(amp_hit);
  } else {
    int cand = window;
    if (cand < 0 || cand > maxWindow) cand = 0;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (freqHz >= hzRow[cand + 2]) {
      while (cand < maxWindow && freqHz >= hzRow[cand + 2]) {
        ++cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    } else if (freqHz < hzRow[cand]) {
      while (cand > 0 && freqHz < hzRow[cand]) {
        --cand;
#if defined(BENCH_PATH_STATS)
        ++steps;
#endif
      }
    }
    if (cand >= 0 && cand <= maxWindow &&
        freqHz >= hzRow[cand] && freqHz < hzRow[cand + 2]) {
      window = cand;
      BENCH_PATH_INC(amp_miss_walk);
#if defined(BENCH_PATH_STATS)
      bench_path_amp_walk_steps(steps);
#endif
    } else {
      window = 0;
      for (int i = 0; i <= maxWindow; ++i) {
        if (freqHz >= hzRow[i] && freqHz < hzRow[i + 2]) {
          window = i;
          break;
        }
      }
      BENCH_PATH_INC(amp_miss_scan);
    }
    ampWinCache[voiceN] = (int16_t)window;
  }

  const FloatQuadCoeffs& coeff = floatCoeffs[voiceN][window];

  float interpolatedValue = (coeff.a * freqHz + coeff.b) * freqHz + coeff.c;
  return (uint16_t)round(interpolatedValue);
}

uint16_t __not_in_flash_func(get_chan_level_lut)(float freqHz, uint8_t voiceN) {
  if (freqHz <= 0.0f) return ampCompLut[voiceN][0];
  if (freqHz >= (float)AMP_COMP_MAX_HZ) return ampCompLut[voiceN][AMP_COMP_MAX_HZ];
  // Nearest integer Hz (not trunc) — same single table load, lower quantize bias.
  // Stay off LUT[MAX] until the hard clamp above: rounding 6999.75→7000 would
  // return the plateau bin while float is still on the last window (~200 counts).
  int32_t hz = (int32_t)(freqHz + 0.5f);
  if (hz < 0) hz = 0;
  if (hz >= AMP_COMP_MAX_HZ) hz = AMP_COMP_MAX_HZ - 1;
  return ampCompLut[voiceN][hz];
}
#endif  // USE_FLOAT_AMP_COMP

// Map raw PW counter into calibrated center/limits for one oscillator. Used on the 99 µs PW path.
inline uint16_t get_PW_level_interpolated(uint16_t PWval, uint8_t oscN) {

  uint16_t chanLevel;

  // Horizontal PW axis: 0 .. DIV_COUNTER_PW-1 (pot/LFO/ADSR domain)
  // Vertical axis (output): mapped to calibrated low/center/high PWM limits.

  if (PWval >= (DIV_COUNTER_PW - 1)) {
    // Above max PW, clamp to calibrated high limit.
    return PW_HIGH_LIMIT[oscN];
  } else if (PWval <= 0) {
    // Below min PW, clamp to calibrated low limit.
    return PW_LOW_LIMIT[oscN];
  } else {
    uint16_t pwLowBreak  = PW_LOOKUP[0];  // usually 0
    uint16_t pwMidBreak  = PW_LOOKUP[1];  // mid-point
    uint16_t pwHighBreak = PW_LOOKUP[2];  // usually DIV_COUNTER_PW-1

    if (PWval >= pwMidBreak) {
      // Upper half: interpolate from center to high limit.
      chanLevel = map(PWval,
                      pwMidBreak, pwHighBreak,
                      PW_CENTER[oscN], PW_HIGH_LIMIT[oscN]);
    } else {
      // Lower half: interpolate from low limit to center.
      chanLevel = map(PWval,
                      pwLowBreak, pwMidBreak,
                      PW_LOW_LIMIT[oscN], PW_CENTER[oscN]);
    }

    return chanLevel;
  }
}

// Drive one oscillator for calibration measurement (manual cal and nested auto-cal probes).
void voice_task_autotune(uint8_t taskAutotuneVoiceMode, uint16_t calibrationValue) {

  float freq;
  uint8_t note1;  // = 57;
  int chanLevel = ampCompCalibrationVal;

  if (VOICE_NOTES[0] > 0) {
    note1 = VOICE_NOTES[0] - 12;
  }

  if (taskAutotuneVoiceMode == 4) {
    // Highest-frequency search drives an explicit frequency instead of a note.
    freq = calibrationFreqHz;
  } else {
    freq = (float)sNotePitches[note1];
  }

  // Target period in cycles for the calibration tone. Guarded because freq can be 0,
  // which would make the float division infinite and the cast undefined.
  uint32_t autotune_total_cycles =
      (freq > 0.0f) ? (uint32_t)fminf(((float)sysClock_Hz / freq) + 0.5f, 4.0e9f) : 0u;

  if (manualCalibrationFlag == true) {  // One Ocillator at a time to get correct gap

    uint8_t currentCalibrationOscillator = cal_manual_osc();

    // ALL AT ONCE
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
      PIO pioN = pio[VOICE_TO_PIO[i]];
      uint8_t sm1N = VOICE_TO_SM[i];

      if (i != currentCalibrationOscillator) {
        // Parked at a clk_div the SM keeps toggling RESET; only a stopped one is
        // really silent. pio_sm_exec runs on a stopped SM, so the pull still
        // drains what was put (same idiom as osc_load_period_stopped).
        pio_sm_set_enabled(pioN, sm1N, false);
        pio_sm_put(pioN, sm1N, 0);
        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));
        write_range_pwm((uint8_t)i, 0);
      } else {
        pio_sm_set_enabled(pioN, sm1N, true);  // an earlier stage may have stopped it

        uint32_t clk_div1 = autotune_total_cycles
                              ? pio_clk_div_for_y(autotune_total_cycles, osc_last_y[i],
                                                  osc_ramp_weight(i), osc_period_overhead(i))
                              : 0u;

        pio_sm_put(pioN, sm1N, clk_div1);

        pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

        write_range_pwm((uint8_t)i, calibrationValue);

        //Serial.println((String) "currentCalibrationOscillator: " + (int)currentCalibrationOscillator + (String) "   calibrationValue: " + (int)calibrationValue);
      }
    }

    // The pulse is the one wave with no analog switch, so its PW CV is its
    // on/off and every channel has to be written: one left at the value the
    // panel gave it keeps that voice sounding through the whole walk. The
    // calibrated voice opens only on the pulse / 440 substages, at the same
    // PW center every FREQ_TRACE anchor probe measures at, so an ampComp440
    // dialled by ear is not offset from what the trace later finds.
    {
      const uint8_t pwCh = cal_pw_channel(currentCalibrationOscillator);
      const bool wantPulse = cal_stage_is_square((uint8_t)manualCalibrationStage);
      for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ch++) {
        if (PW_PINS[ch] == PW_PIN_UNASSIGNED) continue;
        uint16_t pwLevel = 0;
        if (wantPulse && ch == pwCh) {
          pwLevel = (PW_CENTER[ch] != 0) ? PW_CENTER[ch]
                                         : (uint16_t)(DIV_COUNTER_PW / 2);
        }
        pwm_set_chan_level(PW_PWM_SLICES[ch],
                           pwm_gpio_to_channel(PW_PINS[ch]), pwLevel);
      }
    }
  } else {

    PIO pioN = pio[VOICE_TO_PIO[currentDCO]];
    uint8_t sm1N = VOICE_TO_SM[currentDCO];

    uint32_t clk_div1 = autotune_total_cycles
                          ? pio_clk_div_for_y(autotune_total_cycles, osc_last_y[currentDCO],
                                              osc_ramp_weight(currentDCO),
                                              osc_period_overhead(currentDCO))
                          : 0u;

    pio_sm_put(pioN, sm1N, clk_div1);
    pio_sm_exec(pioN, sm1N, pio_encode_pull(false, false));

    switch (taskAutotuneVoiceMode) {
      case 0:
      case 4:
        write_range_pwm(currentDCO, calibrationValue);
        break;
      case 2:
        write_range_pwm(currentDCO, chanLevel);
        break;
      case 3:
        chanLevel = get_chan_level_for_engine(freq, currentDCO);
        write_range_pwm(currentDCO, chanLevel);
        break;
    }

    //}
    // Serial.println((String) "| currentDCO: " + currentDCO + (String) " | freq: " + freq + (String) " | clk_div1: " + clk_div1 + (String) " | ampCompCalibrationVal: " + ampCompCalibrationVal);
  }
}

// Cached variant: pass DCO index to reuse last segment and avoid binary search
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t __not_in_flash_func(interpolatePitchMultiplierIntQ16_cached)(int32_t xQ16, int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  if (xInt <= xMultiplierTable[0]) {
    return yMultiplierTable[0];
  }
  if (xInt >= xMultiplierTable[multiplierTableSize - 1]) {
    return yMultiplierTable[multiplierTableSize - 1];
  }
  int low = interpSegCache[dcoIndex];
  if (low < 0 || low > multiplierTableSize - 2 || !(xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (xInt >= xMultiplierTable[low + 1]) {
        while (low < multiplierTableSize - 2 && xInt >= xMultiplierTable[low + 1]) low++;
      } else if (xInt < xMultiplierTable[low]) {
        while (low > 0 && xInt < xMultiplierTable[low]) low--;
      }
    }
    if (!(low >= 0 && low < multiplierTableSize - 1 && xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
      int l = 0, h = multiplierTableSize - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xMultiplierTable[m] <= xInt && xInt < xMultiplierTable[m + 1]) {
          low = m;
          break;
        } else if (xInt < xMultiplierTable[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > multiplierTableSize - 2) low = multiplierTableSize - 2;
    }
    interpSegCache[dcoIndex] = (int16_t)low;
  }
  int32_t x0 = xMultiplierTable[low];
  int32_t y0 = yMultiplierTable[low];
  int32_t deltaQ12 = (xQ16 - (x0 << 16)) >> 4;
  int32_t y = y0 + (int32_t)((((int64_t)deltaQ12 * (int64_t)slopeQ12[low]) + (1LL << 23)) >> 24);
  return y;
}
#endif // Q12

#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
// xQ16 and tables are natural Q16 (1.0 = 65536). Returns frequency ratio as Q16.
// Miss path: O(1) trunc±1 (same idea as FLOAT_FAST) — avoids walk under LFO thrash.
int32_t __not_in_flash_func(interpolateRatioQ16_cached)(int32_t xQ16, int dcoIndex) {
  // Endpoints match initMultiplierTables (-1 / 3) in Q16.
  static constexpr int32_t kPitchX0_Q16 = -65536; // -1.0
  static constexpr int32_t kPitchX1_Q16 = 196608; // 3.0
  if (xQ16 <= kPitchX0_Q16) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTable[0];
  }
  if (xQ16 >= kPitchX1_Q16) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTable[multiplierTableSize - 1];
  }
  const int lastSeg = multiplierTableSize - 2;
  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      xMultiplierTable[low] <= xQ16 && xQ16 < xMultiplierTable[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
    // cand ≈ (x - (-1)) * (N/4); N=200 → *50, then >>16 for Q16.
    static constexpr int kPitchInvDx = multiplierTableSize / 4; // 50
    int cand = (int)(((int64_t)(xQ16 - kPitchX0_Q16) * (int64_t)kPitchInvDx) >> 16);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (cand < lastSeg && xQ16 >= xMultiplierTable[cand + 1]) {
      ++cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    } else if (cand > 0 && xQ16 < xMultiplierTable[cand]) {
      --cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    }
    low = cand;
    BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
    bench_path_walk_steps(steps);
#endif
    interpSegCache[dcoIndex] = (int16_t)low;
  }
  const int32_t x0 = xMultiplierTable[low];
  const int32_t y0 = yMultiplierTable[low];
  const int32_t delta = xQ16 - x0;
  // slopeQ16 = (dy << 16) / dx  →  y = y0 + (delta * slope) >> 16
  // Boot proves |delta_max*slope| fits int32 (Q20 product does not).
  return y0 + (((delta * slopeQ16[low]) + (1 << 15)) >> 16);
}
#endif // PITCH_INTERP_RATIO_Q16

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
// x = pitch modifier in ~[-1, 3]; returns frequency ratio directly (tables are unscaled).
// Walk + bsearch find (A/B vs FLOAT_FAST).
float __not_in_flash_func(interpolateRatioFloat_cached)(float x, int dcoIndex) {
  if (x <= xMultiplierTableF[0]) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[0];
  }
  if (x >= xMultiplierTableF[multiplierTableSize - 1]) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[multiplierTableSize - 1];
  }

  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= multiplierTableSize - 2 &&
      xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (x >= xMultiplierTableF[low + 1]) {
        while (low < multiplierTableSize - 2 && x >= xMultiplierTableF[low + 1]) {
          ++low;
#if defined(BENCH_PATH_STATS)
          steps++;
#endif
        }
      } else if (x < xMultiplierTableF[low]) {
        while (low > 0 && x < xMultiplierTableF[low]) {
          --low;
#if defined(BENCH_PATH_STATS)
          steps++;
#endif
        }
      }
    }
    if (!(low >= 0 && low < multiplierTableSize - 1 &&
          xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1])) {
      int l = 0;
      int h = multiplierTableSize - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xMultiplierTableF[m] <= x && x < xMultiplierTableF[m + 1]) {
          low = m;
          break;
        } else if (x < xMultiplierTableF[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0) low = 0;
      if (low > multiplierTableSize - 2) low = multiplierTableSize - 2;
      BENCH_PATH_INC(ratio_miss_bsearch);
    } else {
      BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
      bench_path_walk_steps(steps);
#endif
    }
    interpSegCache[dcoIndex] = (int16_t)low;
  }

  return yMultiplierTableF[low] + slopeF[low] * (x - xMultiplierTableF[low]);
}
#endif // PITCH_INTERP_FLOAT

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
// Trunc+clamp±1 find; same lerp as walk. Keep ±1 even when walk_steps≈0 (live ballast).
// noinline: isolate codegen from voice_task_float (distinct SRAM symbol).
__attribute__((noinline))
float __not_in_flash_func(interpolateRatioFloat_cached_fast)(float x, int dcoIndex) {
  // Endpoints match initMultiplierTables (-1 / 3).
  if (x <= -1.0f) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[0];
  }
  if (x >= 3.0f) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[multiplierTableSize - 1];
  }

  static constexpr float kPitchX0 = -1.0f;
  static constexpr float kPitchInvDx = (float)multiplierTableSize / 4.0f; // N/4 = 50
  const int lastSeg = multiplierTableSize - 2;

  int low = interpSegCache[dcoIndex];
  if (low >= 0 && low <= lastSeg &&
      xMultiplierTableF[low] <= x && x < xMultiplierTableF[low + 1]) {
    BENCH_PATH_INC(ratio_hit);
  } else {
    int cand = (int)((x - kPitchX0) * kPitchInvDx);
    if (cand < 0) cand = 0;
    else if (cand > lastSeg) cand = lastSeg;
    // Float rounding / live ballast: keep both ±1 compares even if steps stay 0.
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif
    if (cand < lastSeg && x >= xMultiplierTableF[cand + 1]) {
      ++cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    } else if (cand > 0 && x < xMultiplierTableF[cand]) {
      --cand;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    }
    low = cand;
    BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
    bench_path_walk_steps(steps);
#endif
    interpSegCache[dcoIndex] = (int16_t)low;
  }

  return yMultiplierTableF[low] + slopeF[low] * (x - xMultiplierTableF[low]);
}
#endif // PITCH_INTERP_FLOAT_FAST

// Build integer/float pitch-multiplier tables and slopes (boot). Called from init_voices().
void initMultiplierTables() {

  float y_value;
  double divisor = multiplierTableSize;
  double fraction = 4.00d / divisor;

  for (int i = 0; i < multiplierTableSize; i++) {
    double x;

    if (i == 0) {
      x = -1.00d;
      y_value = 0.25d;
    } else if (i == multiplierTableSize - 1) {
      x = 3.0d;
      y_value = 4.0d;
    } else {
      x = (-1.00d + (fraction * (double)i));
      y_value = expInterpolationSolveY(x + 1.00d, 1.00d, 3.00d, 0.50d, 2.00d);
    }

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || \
    PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
    // Natural domain: x = modifier [-1,3], y = frequency ratio (no int-table scale).
    xMultiplierTableF[i] = (float)x;
    yMultiplierTableF[i] = y_value;
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
    // Native Q16: x and y store natural units (1.0 = 65536). No ×10000.
    xMultiplierTable[i] = (int32_t)(x * 65536.0 + (x >= 0.0 ? 0.5 : -0.5));
    yMultiplierTable[i] = (int32_t)((double)y_value * 65536.0 + 0.5);
    x0Q16_tbl[i] = xMultiplierTable[i];
#else
    // Q12 A/B: legacy ×10000 table-units.
    xMultiplierTable[i] = (int32_t)(x * (double)multiplierTableScale);
    yMultiplierTable[i] = (int32_t)(y_value * (double)multiplierTableScale);
    x0Q16_tbl[i]        = xMultiplierTable[i] << 16;
#endif
  }

#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  // Q20×(dx-1) overflows int32 on upper segments (~7.5e9). Q16 matches Q20 y on
  // this 200-knot set and keeps |delta*slope| in signed 32-bit for MULS lerp.
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
    int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
    if (dx == 0) dx = 1;
    int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
    int64_t numSlope = ((int64_t)dy << 16) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ16[i] = (int32_t)(numSlope / (int64_t)dx);
    const int32_t delta_max = (dx > 0) ? (dx - 1) : 0;
    const int64_t prod = (int64_t)delta_max * (int64_t)slopeQ16[i];
    // One-time boot proof only (no live branch). Stock table always fits.
    if (prod > (int64_t)INT32_MAX || prod < (int64_t)INT32_MIN) {
#if defined(RUNNING_AVERAGE)
      bench_out_printf("ratio slopeQ16*delta overflows int32 at seg %d\n", i);
#endif
    }
  }
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
    int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
    if (dx == 0) dx = 1;
    int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
    int64_t numSlope12 = ((int64_t)dy << 12) + (dx > 0 ? dx / 2 : -dx / 2);
    slopeQ12[i] = (int32_t)(numSlope12 / (int64_t)dx);
  }
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || \
      PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  for (int i = 0; i < multiplierTableSize - 1; ++i) {
    float dxF = xMultiplierTableF[i + 1] - xMultiplierTableF[i];
    if (dxF == 0.0f) dxF = 1.0f;
    slopeF[i] = (yMultiplierTableF[i + 1] - yMultiplierTableF[i]) / dxF;
  }
#endif

  for (int d = 0; d < NUM_OSCILLATORS; ++d) interpSegCache[d] = -1;
}
#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/wave_mux.ino"
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

