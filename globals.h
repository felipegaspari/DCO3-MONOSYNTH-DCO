#include <cstddef>
#include "include_all.h"

#ifndef __GLOBALS_H__
#define __GLOBALS_H__


#define NUM_VOICES_TOTAL 1
#define NUM_OSCILLATORS 3


#define MIDI_CHANNEL 1
//#define USE_ADC_STACK_VOICES // gpio 28 (adc 2)
//#define USE_ADC_DETUNE       // gpio 27 (adc 1)

// Unused SPI (kept commented — MOSI collided with PW_PINS[0]=3).
// #define SCK 2
// #define MOSI 3
// #define MISO 4
// #define CS 5

#define ENABLE_FS_CALIBRATION

static constexpr uint32_t sysClock = 225000;
static constexpr uint32_t sysClock_Hz = sysClock * 1000;

static constexpr uint16_t DIV_COUNTER = 14000;
static constexpr uint16_t DIV_COUNTER_PW = 1024;

static constexpr uint32_t pioPulseLength = 3000;
static constexpr uint32_t pioPulseLengthTimesEight = pioPulseLength * 8;
static constexpr uint32_t eightPioPulseLength = pioPulseLength / 8;

// --- PIO Program Timing Constants ---
static constexpr uint32_t T_HIGH_OVERHEAD_CYCLES = 2;
static constexpr uint32_t T_LOW_OVERHEAD_CYCLES = 5;
static constexpr uint32_t NUM_OSR_CHUNKS = 4;

// --- DERIVED CONSTANTS (Pre-calculated at compile time ) ---
// The total, real duration of the high pulse in cycles.
static constexpr uint32_t T_HIGH_TOTAL_CYCLES = pioPulseLength + T_HIGH_OVERHEAD_CYCLES;

static constexpr uint32_t eightSysClock_Hz_u = sysClock_Hz / 8;
// Q24-scaled clock constants to avoid per-loop shifts
// (removed) Q24-scaled clock constants; direct shift used at call-site


uint32_t loop0_micros;
uint32_t loop1_micros;
uint32_t loop0_microsLast;
uint32_t loop1_microsLast;

volatile uint8_t NUM_VOICES = 1;
volatile uint8_t STACK_VOICES = 1;

volatile uint8_t voiceMode = 1;
uint8_t syncMode = 0;
uint8_t oscSync = 0;
volatile uint8_t polyMode = 1;

volatile uint16_t phaseAlignOSC2 = 0;
// (removed) phaseAlignScale_Q16; use direct computation at call-site

uint8_t unisonDetune = 0;
uint8_t analogDrift = 0;
uint8_t analogDriftSpeed = 0;
uint8_t analogDriftSpread = 0;


// LFO1 detune modulation (previously float) is now stored as Q24 fixed-point.
// This value represents the additive log-frequency modifier produced by LFO1.
int32_t DETUNE_INTERNAL_q24 = 0;

// LFO2 detune modulation for OSC2 / OSC3, in Q24 fixed-point.
volatile int32_t DETUNE_INTERNAL2_q24 = 0;
volatile int32_t DETUNE_INTERNAL3_q24 = 0;

// Raw 32-bit container used to transfer DETUNE_INTERNAL_q24 between cores via FIFO.
uint32_t DETUNE_INTERNAL_FIFO = 1;
uint32_t* detune_fifo_variable = &DETUNE_INTERNAL_FIFO;

// Detune value as received on core 1, in Q24 fixed-point.
int32_t DETUNE_INTERNAL_FIFO_q24 = (1 << 24);


// WEACT RP2040 (legacy 8-osc map — kept for reference):
// static constexpr uint8_t RESET_PINS[8] = { 29, 27, 19, 18, 15, 13, 12, 8 };
// static constexpr uint8_t RANGE_PINS[8] = { 28, 22, 17, 16, 14, 11, 9, 7 };

// Raspberry Pi Pico (legacy):
// static constexpr uint8_t RESET_PINS[8] = { 28, 26, 19, 18, 15, 13, 12, 8 };
// static constexpr uint8_t RANGE_PINS[8] = { 27, 22, 17, 16, 14, 11,  9,  7 };

// Pico 2 provisional pinout: OSC1–3 taken from the legacy WEACT DCO4 map
// (first three oscillators). Replace when the monosynth PCB pinout is final.
// GPIO 24 is board fix-rail (see DCO.ino), not a DCO output.
static constexpr uint8_t RESET_PINS[NUM_OSCILLATORS] = { 29, 27, 19 };
static constexpr uint8_t RANGE_PINS[NUM_OSCILLATORS] = { 28, 22, 17 };

// Freq SMs: pio0 SM0 = OSC1, pio1 SM0 = OSC2, pio2 SM0 = OSC3.
static constexpr uint8_t VOICE_TO_PIO[NUM_OSCILLATORS] = { 0, 1, 2 };
static constexpr uint8_t VOICE_TO_SM[NUM_OSCILLATORS] = { 0, 0, 0 };

static constexpr uint8_t PW_PINS[NUM_VOICES_TOTAL] = { 3 };

static constexpr int DCO_calibration_pin = 10;

uint8_t RANGE_PWM_SLICES[NUM_OSCILLATORS];
uint8_t PW_PWM_SLICES[NUM_VOICES_TOTAL];

uint16_t PW_CENTER[NUM_VOICES_TOTAL] = { 570 };
uint16_t PW_LOW_LIMIT[NUM_VOICES_TOTAL] = { 0 };
uint16_t PW_HIGH_LIMIT[NUM_VOICES_TOTAL] = { DIV_COUNTER_PW };
uint16_t PW_LOOKUP[3] = { 0, (DIV_COUNTER_PW / 2) - 1, DIV_COUNTER_PW - 1 };
uint16_t PW_PWM[NUM_VOICES_TOTAL];

volatile uint32_t VOICES[NUM_VOICES_TOTAL];
volatile uint8_t VOICES_LAST[NUM_VOICES_TOTAL];
volatile uint8_t VOICES_LAST_SEQUENCE[NUM_VOICES_TOTAL] = { 0 };
volatile uint8_t VOICE_NOTES[NUM_VOICES_TOTAL];
volatile uint8_t NEXT_VOICE = 0;


PIO pio[3] = { pio0, pio1, pio2 };

int midi_pitch_bend = 8192, last_midi_pitch_bend = 8192;
uint8_t pitchBendRange = 2;

// Precompute 1/12 in Q24 for fast multiplier calculation
static constexpr int32_t RECIP_TWELVE_Q24 = (int32_t)((1.0f / 12.0f) * (float)(1 << 24));
float pitchBendMultiplier = 1.00f / 12.00f * (float)pitchBendRange;
int32_t pitchBendMultiplier_q24 = 1 << 24;


uint8_t get_free_voice();
void note_on(uint8_t note, uint8_t velocity);
void note_off(uint8_t note);
void voice_task();


uint32_t offset[3];

float voiceFreq[NUM_OSCILLATORS];
uint8_t OSC1_interval = 24;
uint8_t OSC2_interval = 36;
uint8_t OSC3_interval = 36;
uint16_t OSC2DetuneVal = 256;
uint16_t OSC3DetuneVal = 256;

bool PWMPotsControlManual;

uint16_t PW[NUM_VOICES_TOTAL];

void serial_STM32_task();
void serial_send_note_on(uint8_t voice_n, uint8_t note_velo);
void serial_send_note_off(uint8_t voice_n);

volatile uint8_t note_on_flag[NUM_VOICES_TOTAL];


#endif
