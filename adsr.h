#ifndef __ADSR_H__
#define __ADSR_H__

#define ADSR_1_DACSIZE 4000

#define ARRAY_SIZE 512

// ADSR Bezier library (provides curve tables and ADSR class).
// Hot path: Q24 phase A/B (uint64) / Q16 amp; native Q15 (ADSR_BEZIER_NATIVE_Q15=1).
// Set ADSR_BEZIER_PHASE_SHIFT 22 for fast uint32 path after listen A/B.
#ifndef ADSR_BEZIER_PHASE_SHIFT
#define ADSR_BEZIER_PHASE_SHIFT 22
#endif
// RP2040: keep FLOAT=0 (no FPU; soft-float index would be slower).
// Each getWave() uses its own micros(); EnvVCF2 is sampled (reserved for later).
#ifndef ADSR_BEZIER_USE_FLOAT
#define ADSR_BEZIER_USE_FLOAT 0
#endif

#ifndef ADSR_BEZIER_USE_MICROS
#define ADSR_BEZIER_USE_MICROS 1
#endif

#ifndef ADSR_BEZIER_NATIVE_Q15
#define ADSR_BEZIER_NATIVE_Q15 1
#endif

// NATIVE=1: internal peak 32768 (dyadic setter scales). Set 0 to A/B peak=32767.
#ifndef ADSR_BEZIER_Q15_DYADIC
#define ADSR_BEZIER_Q15_DYADIC 1
#endif

// Ignored when NATIVE_Q15=1 (primary output is already Q15).
#ifndef ADSR_BEZIER_UPDATE_Q15_CACHE
#define ADSR_BEZIER_UPDATE_Q15_CACHE 1
#endif

#ifndef ADSR_BEZIER_SRAM_HOT
#define ADSR_BEZIER_SRAM_HOT 1
#endif

#include "_build_libs/ADSR_Bezier/ADSR_Bezier.h"


volatile byte noteStart[NUM_VOICES_TOTAL];
volatile byte noteEnd[NUM_VOICES_TOTAL];

// Legacy u12/DAC mirrors (optional; consumers use *_q15). Not refreshed under NATIVE_Q15.
uint16_t ADSR1Level[NUM_VOICES_TOTAL];
uint16_t ADSR_VCA_Level[NUM_VOICES_TOTAL];
uint16_t ADSR_VCF_Level;
uint16_t ADSR_VCF2_Level;
// Primary mod taps (0..ADSR_Q15_ONE).
int16_t ADSR1Level_q15[NUM_VOICES_TOTAL];
int16_t ADSR_VCA_Level_q15[NUM_VOICES_TOTAL];
int16_t ADSR_VCF_Level_q15;
int16_t ADSR_VCF2_Level_q15;

static constexpr uint16_t ADSR_1_CC = 4000;
// Max CV code / levelDac export for EnvVCA/EnvVCF (legal u12 peak).
static constexpr uint16_t ADSR_CV_CC = 4095;
// Panel→Q15 divisor (1<<12); use with >>12 — not a storable CV level.
static constexpr uint16_t ADSR_CV_SCALE = 4096;

float ADSRMaxLevel = ADSR_1_CC;

uint16_t ADSRMinLevel = 0;

// 0=OSC1, 1=OSC2, 2=OSC1+OSC2, 3=OSC3, 4=all
int8_t ADSR3ToOscSelect = 2;

// EnvDCO → pitch: 0 = unipolar env×depth (default); 1 = centered ((env−16384)<<1; mid S ≈ note, ±2 oct @ full CW).
uint8_t env_dco_pitch_centered = 0;
static constexpr int16_t ENV_DCO_PITCH_CENTER_Q15 = 16384;

static inline int16_t env_dco_pitch_wave_q15(int16_t env_q15) {
  if (!env_dco_pitch_centered)
    return env_q15;
  return (int16_t)(((int32_t)env_q15 - ENV_DCO_PITCH_CENTER_Q15) << 1);
}

uint16_t ADSR1_attack = 0;
uint16_t ADSR1_decay = 0;
uint16_t ADSR1_sustain = 4095;
uint16_t ADSR1_release = 0;

// Core 0 marks dirty on write; Core 1 applies to voices every ~5 ms.
#define ADSR_DIRTY_DCO_A   (1u << 0)
#define ADSR_DIRTY_DCO_D   (1u << 1)
#define ADSR_DIRTY_DCO_S   (1u << 2)
#define ADSR_DIRTY_DCO_R   (1u << 3)
#define ADSR_DIRTY_VCA_A   (1u << 4)
#define ADSR_DIRTY_VCA_D   (1u << 5)
#define ADSR_DIRTY_VCA_S   (1u << 6)
#define ADSR_DIRTY_VCA_R   (1u << 7)
#define ADSR_DIRTY_VCF_A   (1u << 8)
#define ADSR_DIRTY_VCF_D   (1u << 9)
#define ADSR_DIRTY_VCF_S   (1u << 10)
#define ADSR_DIRTY_VCF_R   (1u << 11)
#define ADSR_DIRTY_DCO_ALL (ADSR_DIRTY_DCO_A | ADSR_DIRTY_DCO_D | ADSR_DIRTY_DCO_S | ADSR_DIRTY_DCO_R)
#define ADSR_DIRTY_VCA_ALL (ADSR_DIRTY_VCA_A | ADSR_DIRTY_VCA_D | ADSR_DIRTY_VCA_S | ADSR_DIRTY_VCA_R)
#define ADSR_DIRTY_VCF_ALL (ADSR_DIRTY_VCF_A | ADSR_DIRTY_VCF_D | ADSR_DIRTY_VCF_S | ADSR_DIRTY_VCF_R)

volatile uint16_t adsr_params_dirty = 0;

static inline void mark_adsr_params_dirty(uint16_t mask) {
  adsr_params_dirty |= mask;
}

byte ADSR1_curve2Val = 0;

float ADSR1_curve1 = 0.999;
float ADSR1_curve2 = 0.997;
float ADSR_VCA_curve1 = 0.9995f;
float ADSR_VCA_curve2 = 0.9995f;
float ADSR_VCF_curve1 = 0.997f;
float ADSR_VCF_curve2 = 0.997f;

bool ADSRRestart = true;

int16_t ADSR1toDETUNE1;
// Q24 pitch depth: exp-baked to ADSR_PITCH_MAX_OCTAVES at full CW (see LFO.h / LFO.md).
int32_t ADSR1toDETUNE1_scale_q24;

int16_t ADSR1toPWM;
// Full-scale PW delta (PWM counts) for (ADSR1Level_q15 * scale) >> 15.
int32_t ADSR1toPWM_scale = 0;

adsr adsr1_voice_0(ADSR_1_CC, ADSR1_curve1, ADSR1_curve2, false, 7, 7, 7);
adsr adsr1_voice_1(ADSR_1_CC, ADSR1_curve1, ADSR1_curve2, false, 7, 7, 7);
adsr adsr1_voice_2(ADSR_1_CC, ADSR1_curve1, ADSR1_curve2, false, 7, 7, 7);

adsr adsr_vca_voice_0(ADSR_CV_CC, ADSR_VCA_curve1, ADSR_VCA_curve2, false, 1, 2, 1);
adsr adsr_vca_voice_1(ADSR_CV_CC, ADSR_VCA_curve1, ADSR_VCA_curve2, false, 1, 2, 1);
adsr adsr_vca_voice_2(ADSR_CV_CC, ADSR_VCA_curve1, ADSR_VCA_curve2, false, 1, 2, 1);

// Shared filter envelopes (VCF1 + VCF2); not per voice.
adsr adsr_vcf_voice(ADSR_CV_CC, ADSR_VCF_curve1, ADSR_VCF_curve2, false, 4, 6, 1);
adsr adsr_vcf2_voice(ADSR_CV_CC, ADSR_VCF_curve1, ADSR_VCF_curve2, false, 4, 6, 1);

struct ADSRStruct {
  adsr adsr1_voice;   // EnvDCO → pitch/PW
  adsr adsr_vca_voice;
};

ADSRStruct ADSRVoices[] = {
  { adsr1_voice_0, adsr_vca_voice_0 },
  { adsr1_voice_1, adsr_vca_voice_1 },
  { adsr1_voice_2, adsr_vca_voice_2 },
};

void init_ADSR();
void ADSR_update();
void ADSR_set_parameters();
void ADSR1_set_restart();
void ADSR_VCA_set_restart();
void ADSR_VCF_set_restart();
void ADSR_VCA_change_attack_curve(uint8_t adsrCurveAttack);
void ADSR_VCA_change_decay_curve(uint8_t adsrCurveDecay);
void ADSR_VCF_change_attack_curve(uint8_t adsrCurveAttack);
void ADSR_VCF_change_decay_curve(uint8_t adsrCurveDecay);

#endif
