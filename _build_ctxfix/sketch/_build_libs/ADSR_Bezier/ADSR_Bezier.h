#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/ADSR_Bezier/ADSR_Bezier.h"
//----------------------------------//
// ADSR class for Arduino
// by mo-thunderz
// version 1.2
// last update: 14.08.2022
// modified to exploit hardware FPU + Fixed Point Hybrid
//----------------------------------//

// Use Arduino timing functions internally
#include "Arduino.h"

// Select timebase:
// 1 -> use micros() internally (high resolution)
// 0 -> use millis() internally (backwards-compatible behavior)
#ifndef ADSR_BEZIER_USE_MICROS
#define ADSR_BEZIER_USE_MICROS 1
#endif

// Math backend: 0 = fixed phase index + Q16 amp (default), 1 = hardware FPU time / fixed-point amp hybrid
// ADSR_BEZIER_PHASE_SHIFT: 22 = uint32 mul (fast); 24 = uint64 mul (smoother long A/D/R, A/B).
#ifndef ADSR_BEZIER_USE_FLOAT
#define ADSR_BEZIER_USE_FLOAT 0
#endif

#ifndef ADSR_BEZIER_PHASE_SHIFT
#define ADSR_BEZIER_PHASE_SHIFT 22
#endif

#if ADSR_BEZIER_PHASE_SHIFT > 22
#define ADSR_BEZIER_PHASE_SCALE_U64 1
#else
#define ADSR_BEZIER_PHASE_SCALE_U64 0
#endif

// 1 = refresh Q15 in getWave (default). 0 = skip for ADSR_update A/B (u12 path only).
// Ignored when ADSR_BEZIER_NATIVE_Q15=1 (primary output is already Q15).
#ifndef ADSR_BEZIER_UPDATE_Q15_CACHE
#define ADSR_BEZIER_UPDATE_Q15_CACHE 1
#endif

// Amplitude domain:
// 0 = DAC-primary (ctor vertical_resolution) + optional Q15 cache (default / DCO shipping).
// 1 = native Q15 amp (peak ADSR_Q15_PEAK); getWave returns Q15 tap 0..ADSR_Q15_ONE; setSustain units are peak.
#ifndef ADSR_BEZIER_NATIVE_Q15
#define ADSR_BEZIER_NATIVE_Q15 0
#endif

// NATIVE=1 only: amp peak 32768 (dyadic >>15-friendly scales). Tap/bus full scale stays 32767.
// Set 0 to A/B peak=32767 (non-dyadic divides in setters).
#ifndef ADSR_BEZIER_Q15_DYADIC
#define ADSR_BEZIER_Q15_DYADIC 1
#endif

// 1 = RP2040 __not_in_flash_func on getWave / noteOn / noteOff (define before include).
// 0 = portable / flash (library default). No-op if the attribute is missing (AVR).
// Curve tables stay BSS RAM either way; adsrBezierInitTables is boot-only (not pinned).
#ifndef ADSR_BEZIER_SRAM_HOT
#define ADSR_BEZIER_SRAM_HOT 0
#endif
#if ADSR_BEZIER_SRAM_HOT
#ifndef __not_in_flash_func
#define __not_in_flash_func(fn) fn
#endif
#define ADSR_BEZIER_HOT(fn) __not_in_flash_func(fn)
#else
#define ADSR_BEZIER_HOT(fn) fn
#endif

// Emit active config once per translation unit (visible in arduino-cli / IDE compile log).
#ifndef ADSR_BEZIER_CONFIG_REPORTED
#define ADSR_BEZIER_CONFIG_REPORTED
#if ADSR_BEZIER_USE_FLOAT
#pragma message("ADSR_Bezier: math=native float time / Q16 amp hybrid (Optimized for FPU) (ADSR_BEZIER_USE_FLOAT=1)")
#else
#if ADSR_BEZIER_PHASE_SCALE_U64
#pragma message("ADSR_Bezier: math=fixed Q24/Q16 uint64 phase (ADSR_BEZIER_PHASE_SHIFT>22)")
#else
#pragma message("ADSR_Bezier: math=fixed Q22/Q16 uint32 phase (ADSR_BEZIER_USE_FLOAT=0)")
#endif
#endif
#if ADSR_BEZIER_USE_MICROS
#pragma message("ADSR_Bezier: timebase=micros (ADSR_BEZIER_USE_MICROS=1)")
#else
#pragma message("ADSR_Bezier: timebase=millis (ADSR_BEZIER_USE_MICROS=0)")
#endif
#if ADSR_BEZIER_NATIVE_Q15
#if ADSR_BEZIER_Q15_DYADIC
#pragma message("ADSR_Bezier: amp=native Q15 dyadic peak 32768 (ADSR_BEZIER_NATIVE_Q15=1, Q15_DYADIC=1)")
#else
#pragma message("ADSR_Bezier: amp=native Q15 peak 32767 (ADSR_BEZIER_NATIVE_Q15=1, Q15_DYADIC=0)")
#endif
#elif !ADSR_BEZIER_UPDATE_Q15_CACHE
#pragma message("ADSR_Bezier: Q15 cache OFF (ADSR_BEZIER_UPDATE_Q15_CACHE=0) — A/B only")
#else
#pragma message("ADSR_Bezier: amp=DAC primary + Q15 cache (ADSR_BEZIER_NATIVE_Q15=0)")
#endif
#if ADSR_BEZIER_SRAM_HOT
#pragma message("ADSR_Bezier: SRAM hot path ON (ADSR_BEZIER_SRAM_HOT=1) — getWave/noteOn/noteOff .time_critical")
#else
#pragma message("ADSR_Bezier: SRAM hot path OFF (ADSR_BEZIER_SRAM_HOT=0) — library default")
#endif
#endif

#ifndef ADSR
#define ADSR

// for array for lookup table
#ifndef ARRAY_SIZE
#define ARRAY_SIZE 1024
#endif

// Unipolar Q15 bus full scale (0..ADSR_Q15_ONE ≈ 0..1). Matches mo-lfo MO_LFO_Q15_ONE magnitude.
static constexpr int16_t ADSR_Q15_ONE = 32767;
#if ADSR_BEZIER_NATIVE_Q15 && ADSR_BEZIER_Q15_DYADIC
// Internal amp / table peak (1<<15); getWave/levelQ15 still publish 0..ADSR_Q15_ONE.
static constexpr int ADSR_Q15_PEAK = 32768;
#else
static constexpr int ADSR_Q15_PEAK = (int)ADSR_Q15_ONE;
#endif

// Global curve table pointers (defined later in this header)
extern int *_curve_tables[8];
extern int *_curve_attack_tables[8];

// Midi trigger -> on/off
class adsr
{

public:
    // constructor

    struct Point
    {
        float x, y;
    };

    adsr(int l_vertical_resolution, float attack_alpha, float attack_decay_release, bool bezier, int bezier_attack_type, int bezier_decay_type, int bezier_release_type)
    {
        _dac_export_vr = (l_vertical_resolution > 0) ? l_vertical_resolution : (int)ADSR_Q15_ONE;
#if ADSR_BEZIER_NATIVE_Q15
        // Amp peak is Q15 domain (ADSR_Q15_PEAK); ctor arg kept only for levelDac() export scale.
        _vertical_resolution = (int)ADSR_Q15_PEAK;
        _sustain = (int)ADSR_Q15_PEAK / 2;
        _to_q15_mul = 0;
        if (_dac_export_vr > 0)
            _to_dac_mul =
              ((uint32_t)_dac_export_vr << 16) / (uint32_t)ADSR_Q15_PEAK;
        else
            _to_dac_mul = 0;
#else
        _vertical_resolution = l_vertical_resolution; // DAC_Size
        _sustain = l_vertical_resolution / 2;
        // Q15 cache: (level * mul) >> 16 ≈ level * ADSR_Q15_ONE / vertical_resolution
        if (_vertical_resolution > 0)
            _to_q15_mul =
              ((uint32_t)ADSR_Q15_ONE << 16) / (uint32_t)_vertical_resolution;
        else
            _to_q15_mul = 0;
        _to_dac_mul = 0;
#endif
        _attack = 100000;  // take 100ms as initial value for Attack
        _decay = 100000;   // take 100ms as initial value for Decay
        _release = 100000; // take 100ms as initial value for Release

        _bezier_attack_type = bezier_attack_type;
        _bezier_decay_type = bezier_decay_type;
        _bezier_release_type = bezier_release_type;
        bindCurvePtrs();

        _adsr_output = 0;
        _adsr_output_q15 = 0;
        _adsr_output_q15_src = -1;
    }

#if ADSR_BEZIER_NATIVE_Q15
    void invalidateQ15Cache() {}
#else
    void invalidateQ15Cache() { _adsr_output_q15_src = -1; }
#endif

    void adsrCurveAttack(uint8_t curveType)
    {
        _bezier_attack_type = curveType;
        bindCurvePtrs();
    }

    void adsrCurveDecay(uint8_t curveType)
    {
        _bezier_decay_type = curveType;
        bindCurvePtrs();
    }

    void adsrCurveRelease(uint8_t curveType)
    {
        _bezier_release_type = curveType;
        bindCurvePtrs();
    }

    void setResetAttack(bool l_reset_attack)
    {
        _reset_attack = l_reset_attack;
    }

    // Attack time in milliseconds
    void setAttack(unsigned long l_attack_ms)
    {
#if ADSR_BEZIER_USE_MICROS
        unsigned long attack_ticks = l_attack_ms * 1000UL;
#else
        unsigned long attack_ticks = l_attack_ms;
#endif
        _attack = attack_ticks;

#if ADSR_BEZIER_USE_FLOAT
        if (_attack > 0)
            _attack_rate_f = (float)(ARRAY_SIZE - 1) / (float)_attack;
        else
            _attack_rate_f = 0.0f;
#else
        // Phase scale: idx = (delta * scale) >> PHASE_SHIFT; scale=0 → divide fallback.
        _attack_scale_phase = phaseScale(_attack);
#endif
    }

    // Decay time in milliseconds
    void setDecay(unsigned long l_decay_ms)
    {
#if ADSR_BEZIER_USE_MICROS
        unsigned long decay_ticks = l_decay_ms * 1000UL; 
#else
        unsigned long decay_ticks = l_decay_ms;          
#endif
        _decay = decay_ticks;

#if ADSR_BEZIER_USE_FLOAT
        if (_decay > 0)
            _decay_rate_f = (float)(ARRAY_SIZE - 1) / (float)_decay;
        else
            _decay_rate_f = 0.0f;
#else
        _decay_scale_phase = phaseScale(_decay);
#endif
    }

    // Sustain level: DAC counts 0..vr when NATIVE_Q15=0; 0..ADSR_Q15_PEAK when NATIVE_Q15=1.
    void setSustain(int l_sustain)
    {
        if (l_sustain < 0)
            l_sustain = 0;
        if (l_sustain >= _vertical_resolution)
            l_sustain = _vertical_resolution;
        _sustain = l_sustain;

        int32_t range = (int32_t)_vertical_resolution - (int32_t)_sustain;
        if (range < 0) range = 0;

        _decay_range_scale_q16 = rangeScaleQ16(range, _vertical_resolution);
        invalidateQ15Cache();
    }

    // Release time in milliseconds
    void setRelease(unsigned long l_release_ms)
    {
#if ADSR_BEZIER_USE_MICROS
        unsigned long release_ticks = l_release_ms * 1000UL; 
#else
        unsigned long release_ticks = l_release_ms;          
#endif
        _release = release_ticks;

#if ADSR_BEZIER_USE_FLOAT
        if (_release > 0)
            _release_rate_f = (float)(ARRAY_SIZE - 1) / (float)_release;
        else
            _release_rate_f = 0.0f;
#else
        _release_scale_phase = phaseScale(_release);
#endif
    }

    // Use current micros() timestamp internally
    void ADSR_BEZIER_HOT(noteOn)()
    {
        unsigned long now;
#if ADSR_BEZIER_USE_MICROS
        now = micros();
#else
        now = millis();
#endif
        _t_note_on = now; 
        if (_reset_attack)
            _attack_start = 0;
        else
            _attack_start = _adsr_output;
        // One gate per instance. Retrigger without a leading noteOff (else count
        // accumulates and a single noteOff never reaches release).
        _notes_pressed = 1;

        _phase = ADSR_PHASE_ATTACK;
        _t_phase_start = now;

        int32_t range = (int32_t)_vertical_resolution - (int32_t)_attack_start;
        if (range < 0) range = 0;

        _attack_range_scale_q16 = rangeScaleQ16(range, _vertical_resolution);
        invalidateQ15Cache();
    }

    void ADSR_BEZIER_HOT(noteOff)()
    {
        _notes_pressed--;
        if (_notes_pressed <= 0)
        {
            unsigned long now;
#if ADSR_BEZIER_USE_MICROS
            now = micros();
#else
            now = millis();
#endif
            _t_note_off = now;
            _release_start = _adsr_output;
            _notes_pressed = 0;

            _phase = ADSR_PHASE_RELEASE;
            _t_phase_start = now;

            int32_t rs = (int32_t)_release_start;
            if (rs < 0) rs = 0;
            if (rs > _vertical_resolution) rs = _vertical_resolution;

            _release_range_scale_q16 = rangeScaleQ16(rs, _vertical_resolution);
            invalidateQ15Cache();
        }
    }

    // Advance using internal timebase (micros or millis).
    int ADSR_BEZIER_HOT(getWave)()
    {
#if ADSR_BEZIER_USE_MICROS
        return getWave(micros());
#else
        return getWave(millis());
#endif
    }

    // Advance using caller-supplied timestamp (same units as USE_MICROS).
    // Prefer getWave() when noteOn/noteOff also use the internal timebase.
    int ADSR_BEZIER_HOT(getWave)(unsigned long l_ticks)
    {
#if ADSR_BEZIER_NATIVE_Q15
        // Sustain/idle: no index/table/mul; publish Q15 tap (0..ADSR_Q15_ONE).
        if (_phase == ADSR_PHASE_SUSTAIN) {
            _adsr_output = _sustain;
            _adsr_output_q15 = nativeTapQ15(_sustain);
            return (int)_adsr_output_q15;
        }
        if (_phase == ADSR_PHASE_IDLE) {
            _adsr_output = 0;
            _adsr_output_q15 = 0;
            return 0;
        }
#endif
        unsigned long delta = 0;

        switch (_phase)
        {
        case ADSR_PHASE_ATTACK:
        {
            if (_attack == 0)
            {
                _adsr_output = _vertical_resolution;
                if (_decay > 0) {
                    _phase = ADSR_PHASE_DECAY;
                    _t_phase_start = l_ticks;
                } else {
                    _phase = ADSR_PHASE_SUSTAIN;
                }
                break;
            }

            delta = l_ticks - _t_phase_start;

            if (delta >= _attack)
            {
                _adsr_output = _vertical_resolution;
                if (_decay > 0) {
                    _phase = ADSR_PHASE_DECAY;
                    _t_phase_start = l_ticks;
                } else {
                    _phase = ADSR_PHASE_SUSTAIN;
                }
                break;
            }

#if ADSR_BEZIER_USE_FLOAT
            uint32_t idx = (uint32_t)((float)delta * _attack_rate_f);
            if (idx >= ARRAY_SIZE) idx = ARRAY_SIZE - 1;
#else
            uint32_t idx = phaseIndexFixed(delta, _attack, _attack_scale_phase);
#endif
            int curveVal = _curve_attack_ptr[(int)idx];

            int32_t out = (int32_t)_attack_start +
              (int32_t)(((uint32_t)curveVal * _attack_range_scale_q16) >> 16);

            _adsr_output = (int)out;
            break;
        }

        case ADSR_PHASE_DECAY:
        {
            if (_decay == 0)
            {
                _adsr_output = _sustain;
                _phase = ADSR_PHASE_SUSTAIN;
                break;
            }

            delta = l_ticks - _t_phase_start;

            if (delta >= _decay)
            {
                _adsr_output = _sustain;
                _phase = ADSR_PHASE_SUSTAIN;
                break;
            }

#if ADSR_BEZIER_USE_FLOAT
            uint32_t idx = (uint32_t)((float)delta * _decay_rate_f);
            if (idx >= ARRAY_SIZE) idx = ARRAY_SIZE - 1;
#else
            uint32_t idx = phaseIndexFixed(delta, _decay, _decay_scale_phase);
#endif

            int curveVal = _curve_decay_ptr[(int)idx];

            int32_t out = (int32_t)_sustain +
              (int32_t)(((uint32_t)curveVal * _decay_range_scale_q16) >> 16);

            _adsr_output = (int)out;
            break;
        }

        case ADSR_PHASE_SUSTAIN:
        {
            _adsr_output = _sustain;
            break;
        }

        case ADSR_PHASE_RELEASE:
        {
            if (_release == 0)
            {
                _adsr_output = 0;
                _phase = ADSR_PHASE_IDLE;
                break;
            }

            delta = l_ticks - _t_phase_start;

            if (delta >= _release)
            {
                _adsr_output = 0;
                _phase = ADSR_PHASE_IDLE;
                break;
            }

#if ADSR_BEZIER_USE_FLOAT
            uint32_t idx = (uint32_t)((float)delta * _release_rate_f);
            if (idx >= ARRAY_SIZE) idx = ARRAY_SIZE - 1;
#else
            uint32_t idx = phaseIndexFixed(delta, _release, _release_scale_phase);
#endif

            int curveVal = _curve_release_ptr[(int)idx];

            int32_t out =
              (int32_t)(((uint32_t)curveVal * _release_range_scale_q16) >> 16);

            _adsr_output = (int)out;
            break;
        }

#if !ADSR_BEZIER_NATIVE_Q15
        case ADSR_PHASE_IDLE:
        default:
        {
            _adsr_output = 0;
            break;
        }
#else
        default:
            _adsr_output = 0;
            break;
#endif
        }
#if ADSR_BEZIER_NATIVE_Q15
        // A/D/R: publish bus tap (curve×scale stays in [0, vr] when tables/scales valid).
        _adsr_output_q15 = nativeTapQ15(_adsr_output);
        return (int)_adsr_output_q15;
#elif ADSR_BEZIER_UPDATE_Q15_CACHE
        // Q15 cache: skip mul when DAC level unchanged (sustain/idle).
        if (_adsr_output != _adsr_output_q15_src) {
            _adsr_output_q15_src = _adsr_output;
            uint32_t q = ((uint32_t)_adsr_output * _to_q15_mul) >> 16;
            if (q > (uint32_t)ADSR_Q15_ONE) q = (uint32_t)ADSR_Q15_ONE;
            _adsr_output_q15 = (int16_t)q;
        }
#endif
        return _adsr_output;
    }

    // Q15 from last getWave() (0..ADSR_Q15_ONE ≈ 0..1).
    // NATIVE_Q15=0: cached remap from DAC. NATIVE_Q15=1: same as getWave domain.
    int16_t levelQ15() const
    {
        return _adsr_output_q15;
    }

    // DAC-domain level: identity when NATIVE_Q15=0; Q15→ctor-vr export when NATIVE_Q15=1.
    int levelDac() const
    {
#if ADSR_BEZIER_NATIVE_Q15
        return (int)(((uint32_t)_adsr_output * _to_dac_mul) >> 16);
#else
        return _adsr_output;
#endif
    }

    // Advance envelope and return Q15 level.
    // Do not call getWave(t) and getWaveQ15(t) in the same tick (double advance).
    int16_t getWaveQ15()
    {
        getWave();
        return _adsr_output_q15;
    }

    int16_t getWaveQ15(unsigned long t)
    {
        getWave(t);
        return _adsr_output_q15;
    }

private:
#if ADSR_BEZIER_NATIVE_Q15
    // Publish 0..ADSR_Q15_ONE (int16 bus); internal peak may be ADSR_Q15_PEAK (32768).
    static int16_t nativeTapQ15(int level)
    {
        if (level <= 0)
            return 0;
        if (level >= (int)ADSR_Q15_ONE)
            return ADSR_Q15_ONE;
        return (int16_t)level;
    }
#endif

    void bindCurvePtrs()
    {
        uint8_t a = (uint8_t)_bezier_attack_type;
        uint8_t d = (uint8_t)_bezier_decay_type;
        uint8_t r = (uint8_t)_bezier_release_type;
        if (a > 7) a = 7;
        if (d > 7) d = 7;
        if (r > 7) r = 7;
        _curve_attack_ptr = _curve_attack_tables[a];
        _curve_decay_ptr = _curve_tables[d];
        _curve_release_ptr = _curve_tables[r];
    }

    // Q16 amplitude scale: (range << 16) / vr. Dyadic peak 32768 → range << 1 (no divide).
    static uint32_t rangeScaleQ16(int32_t range, int vr)
    {
        if (range <= 0 || vr <= 0)
            return 0;
#if ADSR_BEZIER_NATIVE_Q15 && ADSR_BEZIER_Q15_DYADIC
        if (vr == ADSR_Q15_PEAK)
            return (uint32_t)range << 1;
#endif
        return (uint32_t)(((uint64_t)range << 16) / (uint32_t)vr);
    }

#if !ADSR_BEZIER_USE_FLOAT
#if ADSR_BEZIER_PHASE_SCALE_U64
    using phase_scale_t = uint64_t;
#else
    using phase_scale_t = uint32_t;
#endif

    // Rounded phase scale: ((ARRAY_SIZE-1)<<SHIFT)/ticks.
    static phase_scale_t phaseScale(unsigned long phase_ticks)
    {
        if (phase_ticks == 0)
            return 0;
        return (phase_scale_t)((((uint64_t)(ARRAY_SIZE - 1) << ADSR_BEZIER_PHASE_SHIFT) +
                                (phase_ticks >> 1)) /
                               (uint64_t)phase_ticks);
    }

    // idx = (delta * scale) >> SHIFT. U64 mul when PHASE_SHIFT>22 (Q24 A/B).
    static uint32_t phaseIndexFixed(unsigned long delta, unsigned long phase_ticks, phase_scale_t scale)
    {
        uint32_t idx;
        if (scale != 0)
        {
#if ADSR_BEZIER_PHASE_SCALE_U64
            idx = (uint32_t)(((uint64_t)delta * scale) >> ADSR_BEZIER_PHASE_SHIFT);
#else
            idx = ((uint32_t)delta * (uint32_t)scale) >> ADSR_BEZIER_PHASE_SHIFT;
#endif
        }
        else if (phase_ticks > 0)
        {
            idx = (uint32_t)(((uint64_t)(ARRAY_SIZE - 1) * (uint64_t)delta) / (uint64_t)phase_ticks);
        }
        else
        {
            idx = 0;
        }
        if (idx >= ARRAY_SIZE)
            idx = ARRAY_SIZE - 1;
        return idx;
    }
#endif

    int _bezier_attack_type;
    int _bezier_decay_type;
    int _bezier_release_type;
    int *_curve_attack_ptr = nullptr;
    int *_curve_decay_ptr = nullptr;
    int *_curve_release_ptr = nullptr;

    int _vertical_resolution; // amp peak: DAC vr (NATIVE=0) or ADSR_Q15_PEAK (NATIVE=1)
    int _dac_export_vr;       // ctor DAC size; used by levelDac() when NATIVE=1
    uint32_t _to_dac_mul = 0; // (dac_export_vr << 16) / ADSR_Q15_PEAK when NATIVE=1
    unsigned long _attack = 0;
    unsigned long _decay = 0; 
    int _sustain = 0;         // DAC counts (NATIVE=0) or Q15 (NATIVE=1)
    unsigned long _release = 0;
    bool _reset_attack = false;

#if !ADSR_BEZIER_USE_FLOAT
    phase_scale_t _attack_scale_phase = 0;
    phase_scale_t _decay_scale_phase = 0;
    phase_scale_t _release_scale_phase = 0;
#endif

    enum ADSRPhase
    {
        ADSR_PHASE_IDLE = 0,
        ADSR_PHASE_ATTACK,
        ADSR_PHASE_DECAY,
        ADSR_PHASE_SUSTAIN,
        ADSR_PHASE_RELEASE
    };

    ADSRPhase _phase = ADSR_PHASE_IDLE;
    unsigned long _t_phase_start = 0;

#if ADSR_BEZIER_USE_FLOAT
    float _attack_rate_f = 0.0f;
    float _decay_rate_f = 0.0f;
    float _release_rate_f = 0.0f;
#endif

    // Unified Amplitude scaling - universally fast on all platforms
    uint32_t _attack_range_scale_q16 = 0;
    uint32_t _decay_range_scale_q16 = 0;
    uint32_t _release_range_scale_q16 = 0;

    unsigned long _t_note_on = 0;
    unsigned long _t_note_off = 0;

    int _adsr_output;
    int16_t _adsr_output_q15 = 0;
    int _adsr_output_q15_src = -1;  // last DAC level converted to Q15
    // (ADSR_Q15_ONE << 16) / vertical_resolution — set in ctor
    uint32_t _to_q15_mul = 0;
    int _release_start;
    int _attack_start;
    int _notes_pressed = 0;
};

// ---------------------------------------------------------------------------
// Bézier table generation helpers
// ---------------------------------------------------------------------------

// Global Bézier lookup tables shared by all ADSR instances.
// ARRAY_SIZE is provided by the including project before this header.
int _curve0_table[ARRAY_SIZE];
int _curve1_table[ARRAY_SIZE];
int _curve2_table[ARRAY_SIZE];
int _curve3_table[ARRAY_SIZE];
int _curve4_table[ARRAY_SIZE];
int _curve5_table[ARRAY_SIZE];
int _curve6_table[ARRAY_SIZE];
int _curve7_table[ARRAY_SIZE];

int _curve0_attack_table[ARRAY_SIZE];
int _curve1_attack_table[ARRAY_SIZE];
int _curve2_attack_table[ARRAY_SIZE];
int _curve3_attack_table[ARRAY_SIZE];
int _curve4_attack_table[ARRAY_SIZE];
int _curve5_attack_table[ARRAY_SIZE];
int _curve6_attack_table[ARRAY_SIZE];
int _curve7_attack_table[ARRAY_SIZE];

int *_curve_tables[8] = {
    _curve0_table, _curve1_table, _curve2_table, _curve3_table,
    _curve4_table, _curve5_table, _curve6_table, _curve7_table};

int *_curve_attack_tables[8] = {
    _curve0_attack_table, _curve1_attack_table, _curve2_attack_table, _curve3_attack_table,
    _curve4_attack_table, _curve5_attack_table, _curve6_attack_table, _curve7_attack_table};

// Lightweight point type used for table generation
struct ADSRBezierPoint
{
    float x, y;
};

// Evaluate a cubic Bézier at parameter t in [0, 1]
inline ADSRBezierPoint adsrBezierCubic(const ADSRBezierPoint &A,
                                       const ADSRBezierPoint &P1,
                                       const ADSRBezierPoint &P2,
                                       const ADSRBezierPoint &B,
                                       float t)
{
    float one_minus_t = 1.0f - t;
    float one_minus_t_squared = one_minus_t * one_minus_t;
    float t_squared = t * t;

    float x = one_minus_t_squared * one_minus_t * A.x +
              3.0f * one_minus_t_squared * t * P1.x +
              3.0f * one_minus_t * t_squared * P2.x +
              t_squared * t * B.x;

    float y = one_minus_t_squared * one_minus_t * A.y +
              3.0f * one_minus_t_squared * t * P1.y +
              3.0f * one_minus_t * t_squared * P2.y +
              t_squared * t * B.y;

    return {x, y};
}

// Find y for a given x on the cubic Bézier using binary search on t
inline float adsrBezierFindYForX(const ADSRBezierPoint &A,
                                 const ADSRBezierPoint &P1,
                                 const ADSRBezierPoint &P2,
                                 const ADSRBezierPoint &B,
                                 float xTarget,
                                 float tol = 1e-5f)
{
    float tLow = 0.0f;
    float tHigh = 1.0f;
    float tMid = 0.0f;

    for (int iter = 0; iter < 64 && (tHigh - tLow) > tol; ++iter)
    {
        tMid = (tLow + tHigh) * 0.5f;
        ADSRBezierPoint midPoint = adsrBezierCubic(A, P1, P2, B, tMid);
        if (midPoint.x < xTarget)
        {
            tLow = tMid;
        }
        else
        {
            tHigh = tMid;
        }
    }

    ADSRBezierPoint resultPoint = adsrBezierCubic(A, P1, P2, B, tMid);
    return resultPoint.y;
}

// Generate 8 Bézier curves into the provided curve_tables (size [8][numPoints])
// maxVal: maximum y value (e.g. vertical_resolution or ADSR_Q15_ONE)
// numPoints: number of points per curve (ARRAY_SIZE)
//
// P1/P2 literals are authored near 12-bit CV (~4095); scale by maxVal/4096 (2^12)
// so shapes stay consistent at Q15 peak (NATIVE_Q15) with an exact dyadic factor.
inline void adsrBezierInitTables(float maxVal, int numPoints, int *curve_tables[8])
{
    ADSRBezierPoint A = {0.0f, maxVal};
    ADSRBezierPoint B = {maxVal, 0.0f};

    // Scale reference = 2^12 (not 4095): exact float reciprocal; ≈ authored frame.
    static constexpr float kAuthPeak = 4096.0f;
    const float s = maxVal / kAuthPeak;

    const ADSRBezierPoint P1_auth[8] = {
        {250.0f, 1500.0f}, {840.0f, 1780.0f}, {400.0f, 430.0f},  {2170.0f, 3610.0f},
        {400.0f, 1380.0f}, {1140.0f, 3750.0f}, {200.0f, 2700.0f}, {0.0f, 4095.0f}};

    const ADSRBezierPoint P2_auth[8] = {
        {1500.0f, 250.0f}, {1160.0f, 210.0f}, {920.0f, 420.0f},  {3730.0f, 2610.0f},
        {3830.0f, 2890.0f}, {1850.0f, 1080.0f}, {720.0f, 3050.0f}, {4095.0f, 0.0f}};

    for (int j = 0; j < 8; ++j)
    {
        ADSRBezierPoint P1 = {P1_auth[j].x * s, P1_auth[j].y * s};
        ADSRBezierPoint P2 = {P2_auth[j].x * s, P2_auth[j].y * s};

        float multiplier = (float)(maxVal + 1.0f) / (float)(numPoints - 1);

        for (int i = 0; i < numPoints; ++i)
        {
            float xTarget = multiplier * (float)i;
            float yResult = adsrBezierFindYForX(A, P1, P2, B, xTarget);

            curve_tables[j][i] = (int)roundf(yResult);
        }
        for (int i = 0; i < numPoints; ++i)
        {
            _curve_attack_tables[j][i] = curve_tables[j][numPoints - 1 - i];
        }
    }
}

#endif