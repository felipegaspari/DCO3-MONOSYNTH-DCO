#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_removed/globals.h"
// Removed unused from globals.h
// Unused: amplitude stays on RANGE PWM (not PIO). Kept only if SM1 is reused later.
static constexpr uint8_t AMP_TO_SM[NUM_OSCILLATORS] = { 1, 1, 1 };

// --- unused globals/decls ---
static constexpr uint32_t pioPulseLengthTimesEight = pioPulseLength * 8;
static constexpr uint32_t eightPioPulseLength = pioPulseLength / 8;
static constexpr uint32_t halfSysClock_Hz = sysClock_Hz / 2;
static constexpr uint32_t eightSysClock_Hz_u = sysClock_Hz / 8;
static constexpr uint32_t eightSysClockMinusPulseLength_Hz_u = (sysClock_Hz - pioPulseLength - 8) / 8;
// Q24-scaled clock constants to avoid per-loop shifts
// (removed) Q24-scaled clock constants; direct shift used at call-site
float DETUNE = 0.0f, LAST_DETUNE = 0.0f;
float DETUNE2 = 1.00f;
float BASE_NOTE = 440.0f;
uint8_t VCO_PWM_SLICES[NUM_OSCILLATORS];
volatile uint8_t VOICES_LAST[NUM_VOICES_TOTAL];
uint32_t LED_BLINK_START = 0;
uint8_t midi_serial_status = 0;
// Precompute 1/360 in Q24 for fast phaseDelay calculation (full 0–360° range)
static constexpr uint32_t RECIP_360_Q24 = (uint32_t)(((1ULL << 24) + 180) / 360);
uint16_t raw;
void init_sm(PIO pio, uint sm, uint offset, uint pin);
void set_frequency(PIO pio, uint sm, float freq);
float get_freq_from_midi_note(uint8_t note);
void led_blinking_task();
void usb_midi_task();
void serial_midi_task();
void adc_task();
uint8_t dataArray[4];
float LFOMultiplier = 1;
float voiceFreq[NUM_OSCILLATORS];
uint16_t dato_serial;
float dato_serial_float;
uint8_t OSC2_serial_detune = 127;
float OSC2_detune = 127;
bool PWMPotsControlManual;
void serial_send_voice_freq();
void serial_send_note_on(uint8_t voice_n, uint8_t note_velo);
float get_chan_level(float freq_to_amp_comp);
bool ledstat = false;
