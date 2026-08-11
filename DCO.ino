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
#define ENABLE_PIO_RESET_INVERT

// ENABLE_SUBOSC_ENGINE2 — one sub-oscillator per main oscillator on pio2: three SMs share
// the subosc_seg program (edge-locked divide + programmable phase offset and pulse width,
// three segment words streamed per period by DMA), plus pio2 SM3 for the boolean logic
// combiner. Off = the classic pio1 subosc_div2 / subosc_div4 fixed 50% sub on OSC1 only.
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

#include "PID.h"
#include "autotune.h"


// ****************************************************************************************** //

// Core 0 boot: USB, UART serial, MIDI handlers, LFOs, board fix pins, calibration input pin.
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

  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);

  pinMode(24, OUTPUT);  // Fix pin on DCO BOARD
  digitalWrite(24, HIGH);

  pinMode(DCO_calibration_pin, INPUT);

  // gpio_init(11);
  // gpio_set_dir(11, GPIO_IN);
  // gpio_pull_down(11);
}

// Core 1 boot: PID, LittleFS cal load, ADSR, amp-comp precompute, PWM/PIO, voices.
// Clears calibrationFlag so the init_DCO_calibration block below is currently unreachable.
void setup1() {

  sys_clock_hz_refresh();  // Arduino already set clk_sys; cache real Hz for clkdiv

  bench_init_core();
  init_micros_timers();

  init_PID();

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

  if (calibrationFlag == true) {
    init_DCO_calibration();
    voice_task_autotune(0, ampCompCalibrationVal);
  }
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
      VOICE_NOTES[0] = manual_DCO_calibration_start_note;
      DCO_calibration_current_note = manual_DCO_calibration_start_note;
      // Keep currentDCO in sync so [GAP_MEASURE]/[GAP_TIMEOUT] logs match the soloed osc.
      currentDCO = manualCalibrationStage;
      if (currentDCO >= NUM_OSCILLATORS) {
        currentDCO = NUM_OSCILLATORS - 1;
      }
      ampCompCalibrationVal = initManualAmpCompCalibrationValPreset + manualCalibrationOffset[manualCalibrationStage];
      voice_task_autotune(0, ampCompCalibrationVal);
      update_CV_outs_manual_calibration();
      // In manual calibration mode, continuously measure and report the duty
      // difference so the screen can display live feedback for the user.
      DCO_calibration_debug();
      //Serial.println((String) "PW value: " + (PW[0] / 4));

    } else {
      DCO_calibration();
    }
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