#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO_Noise/DCO_Noise.h"
#ifndef DCO_NOISE_H
#define DCO_NOISE_H

#include <Arduino.h>
#include <stdint.h>
#include "hardware/pio.h"

// DCO_Noise — Arduino-style noise engines (begin / next).
// All engines return full-scale int16 Q15 (−32768…32767). No min/max mapping.
//
//   ColoredNoise      — Voss pink / 1-pole brown / white (PIO seed via PioNoiseWhite)
//   FastNoiseGen      — economy Voss pink / leaky brown / local xorshift white
//   PrimeHybridNoise  — three prime tables per gen; dither + rephase
//   ProNoise32        — Q16.15 Kellett pink / DC-corrected brown / xorshift white
//
// Sketch may #define NOISE_ENGINE (0..3) and/or ENABLE_NOISE_OUT before include.

#ifndef NOISE_ENGINE
#define NOISE_ENGINE 0
#endif

enum NoiseColor : uint8_t {
  NOISE_PINK = 0,
  NOISE_BROWN = 1,
  NOISE_WHITE = 2,
};

enum FastNoiseColor : uint8_t {
  FAST_NOISE_PINK = 0,
  FAST_NOISE_BROWN = 1,
  FAST_NOISE_WHITE = 2,
};

static inline int16_t dcoNoiseClampQ15(int32_t x) {
  if (x < -32768) return -32768;
  if (x > 32767) return 32767;
  return (int16_t)x;
}

// ---------------------------------------------------------------------------
// Shared PIO LFSR white entropy (RP2040).
// ---------------------------------------------------------------------------
class PioNoiseWhite {
public:
  void begin(PIO pio_inst, uint sm) {
    _pio = pio_inst;
    _sm = sm;
    _state = 0xA5A5A5A5u;
    _word = 0;
    _bits = 0;
    _reseed_div = 0;
  }

  inline void refill() {
    if (_pio == nullptr) return;
    uint32_t last = 0;
    while (!pio_sm_is_rx_fifo_empty(_pio, _sm)) {
      last = pio_sm_get(_pio, _sm);
    }
    if ((++_reseed_div & 3u) == 0u && last != 0u) {
      _state = last;
      _bits = 0;
    }
  }

  inline int16_t nextQ15() {
    if (_bits < 16u) {
      uint32_t s = _state;
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      _state = s;
      _word = s;
      _bits = 32u;
    }
    const int16_t out = (int16_t)(_word & 0xFFFFu);
    _word >>= 16;
    _bits -= 16u;
    return out;
  }

  bool ready() const { return _pio != nullptr; }

private:
  PIO _pio = nullptr;
  uint _sm = 0;
  uint32_t _state = 0xA5A5A5A5u;
  uint32_t _word = 0;
  uint8_t _bits = 0;
  uint8_t _reseed_div = 0;
};

inline PioNoiseWhite& pioNoiseWhite() {
  static PioNoiseWhite inst;
  return inst;
}

static inline bool dcoNoiseUsesPioWhite() {
#if (NOISE_ENGINE == 0) || defined(ENABLE_NOISE_OUT)
  return true;
#else
  return false;
#endif
}

static inline void dcoNoisePioBegin(PIO pio, uint sm) {
  if (dcoNoiseUsesPioWhite()) {
    pioNoiseWhite().begin(pio, sm);
  }
}

static inline void dcoNoisePioRefill() {
  if (dcoNoiseUsesPioWhite()) {
    pioNoiseWhite().refill();
  }
}

// ---------------------------------------------------------------------------
class ColoredNoise {
public:
  ColoredNoise() { begin(NOISE_PINK, 1u); }

  ColoredNoise(NoiseColor color, uint32_t seed_val = 1u) {
    begin(color, seed_val);
  }

  void begin(NoiseColor color, uint32_t seed_val = 1u) {
    _color = color;
    seed(seed_val);
  }

  void seed(uint32_t s) {
    _state = (s == 0u) ? 0xA5A5A5A5u : s;
    clearFilter();
  }

  void setColor(NoiseColor c) {
    _color = c;
    clearFilter();
  }

  inline int16_t next() {
    if (_color == NOISE_WHITE) {
      return nextWhiteQ15();
    }
    if (_color == NOISE_BROWN) {
      return nextBrownQ15();
    }
    return nextPinkQ15();
  }

  int16_t minOut() const { return -32768; }
  int16_t maxOut() const { return 32767; }

private:
  static constexpr int BROWN_SHIFT = 5;
  static constexpr unsigned PINK_OCTAVES = 8;

  uint32_t _state;
  uint32_t _pink_counter;
  int16_t _pink_rows[PINK_OCTAVES];
  int32_t _pink_sum;
  int32_t _brown;
  NoiseColor _color;

  void clearFilter() {
    _pink_counter = 0;
    _pink_sum = 0;
    _brown = 0;
    for (unsigned i = 0; i < PINK_OCTAVES; i++) {
      _pink_rows[i] = 0;
    }
  }

  inline int16_t nextWhiteQ15() {
    return pioNoiseWhite().nextQ15();
  }

  inline int16_t nextPinkQ15() {
    int16_t white = nextWhiteQ15();
    const uint32_t prev = _pink_counter++;
    uint32_t changed = prev ^ _pink_counter;
    unsigned idx = 0;
    while (changed != 0u && idx < PINK_OCTAVES) {
      if ((changed & 1u) != 0u) {
        _pink_sum -= (int32_t)_pink_rows[idx];
        _pink_rows[idx] = white;
        _pink_sum += (int32_t)white;
        white = nextWhiteQ15();
      }
      changed >>= 1;
      idx++;
    }
    return dcoNoiseClampQ15((_pink_sum + (int32_t)white) >> 3);
  }

  inline int16_t nextBrownQ15() {
    const int32_t white = (int32_t)nextWhiteQ15();
    _brown += (white - _brown) >> BROWN_SHIFT;
    return dcoNoiseClampQ15(_brown);
  }
};

// ---------------------------------------------------------------------------
class FastNoiseGen {
public:
  FastNoiseGen() { begin(NOISE_PINK, 123456789u); }

  FastNoiseGen(NoiseColor color, uint32_t seed_val = 123456789u) {
    begin(color, seed_val);
  }

  void begin(NoiseColor color, uint32_t seed_val = 123456789u) {
    begin((FastNoiseColor)color, seed_val);
  }

  void begin(FastNoiseColor color, uint32_t seed_val = 123456789u) {
    _color = color;
    seed(seed_val);
  }

  void seed(uint32_t s) {
    _seed = (s == 0u) ? 1u : s;
    clearFilter();
  }

  void setColor(FastNoiseColor c) {
    _color = c;
    clearFilter();
  }

  void setColor(NoiseColor c) {
    setColor((FastNoiseColor)c);
  }

  // Peak ~±18432 before scale; *32767/18432 ≈ *1.78 → use * 7/4 then clamp.
  static constexpr int32_t PINK_Q15_NUM = 7;
  static constexpr int32_t PINK_Q15_DEN = 4;
  // Brown state after leak; *16 brings typical peaks toward Q15 rails.
  static constexpr int BROWN_Q15_SHIFT = 4;

  inline int16_t process() {
    if (_color == FAST_NOISE_BROWN) {
      int32_t white = (int32_t)(xorshift32() & 0x1FFFu) - 4096;
      _brown += white;
      _brown -= (_brown >> 7);
      return dcoNoiseClampQ15(_brown << BROWN_Q15_SHIFT);
    }
    if (_color == FAST_NOISE_PINK) {
      _voss_counter++;
      uint8_t c = (uint8_t)_voss_counter;
      uint8_t index = 0;
      if (c != 0u) {
        while ((c & 1u) == 0u && index < 7u) {
          c >>= 1;
          index++;
        }
      } else {
        index = 7;
      }
      _voss_sum -= _voss_rows[index];
      int16_t newVal = (int16_t)(xorshift32() & 0x0FFFu) - 2048;
      _voss_rows[index] = newVal;
      _voss_sum += newVal;
      int16_t white = (int16_t)(xorshift32() & 0x0FFFu) - 2048;
      const int32_t raw = (int32_t)_voss_sum + (int32_t)white;
      return dcoNoiseClampQ15((raw * PINK_Q15_NUM) / PINK_Q15_DEN);
    }
    // Full-scale white Q15
    return (int16_t)(xorshift32() >> 16);
  }

  inline int16_t next() { return process(); }

  int16_t minOut() const { return -32768; }
  int16_t maxOut() const { return 32767; }

private:
  uint32_t _seed;
  uint32_t _voss_counter;
  int16_t _voss_rows[8];
  int16_t _voss_sum;
  int32_t _brown;
  FastNoiseColor _color;

  void clearFilter() {
    _voss_counter = 0;
    _voss_sum = 0;
    _brown = 0;
    for (int i = 0; i < 8; i++) {
      _voss_rows[i] = 0;
    }
  }

  inline uint32_t xorshift32() {
    uint32_t s = _seed;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    _seed = s;
    return s;
  }
};

// ---------------------------------------------------------------------------
class PrimeHybridNoise {
public:
  static constexpr size_t DEFAULT_LEN1 = 997;
  static constexpr size_t DEFAULT_LEN2 = 1499;
  static constexpr size_t DEFAULT_LEN3 = 1999;

  PrimeHybridNoise()
      : table1(nullptr), table2(nullptr), table3(nullptr),
        len1(0), len2(0), len3(0),
        idx1(0), idx2(0), idx3(0),
        _rng(1u) {}

  PrimeHybridNoise(size_t p1, size_t p2, size_t p3)
      : table1(nullptr), table2(nullptr), table3(nullptr),
        len1(p1), len2(p2), len3(p3),
        idx1(0), idx2(0), idx3(0),
        _rng(1u) {
    allocateTables();
  }

  PrimeHybridNoise(NoiseColor color, uint32_t seed_val = 123456u)
      : PrimeHybridNoise() {
    begin(color, seed_val);
  }

  ~PrimeHybridNoise() {
    delete[] table1;
    delete[] table2;
    delete[] table3;
    table1 = nullptr;
    table2 = nullptr;
    table3 = nullptr;
  }

  PrimeHybridNoise(const PrimeHybridNoise&) = delete;
  PrimeHybridNoise& operator=(const PrimeHybridNoise&) = delete;

  void begin(NoiseColor color, uint32_t seed_val = 123456u,
             size_t p1 = DEFAULT_LEN1, size_t p2 = DEFAULT_LEN2,
             size_t p3 = DEFAULT_LEN3) {
    if (table1 == nullptr || len1 != p1 || len2 != p2 || len3 != p3) {
      delete[] table1;
      delete[] table2;
      delete[] table3;
      table1 = nullptr;
      table2 = nullptr;
      table3 = nullptr;
      len1 = p1;
      len2 = p2;
      len3 = p3;
      allocateTables();
    }
    idx1 = 0;
    idx2 = 0;
    idx3 = 0;
    precompute(color, seed_val);
  }

  void precompute(NoiseColor color, uint32_t seed_val = 123456u) {
    _rng = (seed_val == 0u) ? 1u : seed_val;
    randomSeed(seed_val);
    fillTable(table1, len1, color);
    fillTable(table2, len2, color);
    fillTable(table3, len3, color);
  }

  // Three ±10922 tables → sum ~Q15; dither + rephase disguise loops.
  inline int16_t process() {
    int32_t sum =
        (int32_t)table1[idx1] + (int32_t)table2[idx2] + (int32_t)table3[idx3];
    sum += (int32_t)(xorshift32() & 0x0FFFu) - 2048;

    idx1++;
    idx2++;
    idx3++;

    if (idx1 >= len1) idx1 = rngIndex(xorshift32(), len1);
    if (idx2 >= len2) idx2 = rngIndex(xorshift32(), len2);
    if (idx3 >= len3) idx3 = rngIndex(xorshift32(), len3);

    return dcoNoiseClampQ15(sum);
  }

  inline int16_t next() { return process(); }

  int16_t minOut() const { return -32768; }
  int16_t maxOut() const { return 32767; }

private:
  int16_t* table1;
  int16_t* table2;
  int16_t* table3;
  size_t len1, len2, len3;
  size_t idx1, idx2, idx3;
  uint32_t _rng;

  static inline size_t rngIndex(uint32_t r, size_t len) {
    return (size_t)(((uint64_t)r * (uint64_t)len) >> 32);
  }

  inline uint32_t xorshift32() {
    uint32_t s = _rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    _rng = s;
    return s;
  }

  void allocateTables() {
    table1 = new int16_t[len1];
    table2 = new int16_t[len2];
    table3 = new int16_t[len3];
  }

  static void fillTable(int16_t* table, size_t len, NoiseColor color) {
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    float brown = 0;

    for (size_t i = 0; i < len; i++) {
      float white = random(-32768, 32767) / 32768.0f;
      float outVal = 0.0f;

      if (color == NOISE_WHITE) {
        outVal = white;
      } else if (color == NOISE_BROWN) {
        brown = (0.99f * brown) + (0.02f * white);
        outVal = brown * 3.0f;
      } else if (color == NOISE_PINK) {
        b0 = 0.99886f * b0 + white * 0.0555179f;
        b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f;
        b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f;
        b5 = -0.7616f * b5 - white * 0.0168980f;
        outVal = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.15f;
        b6 = white * 0.115926f;
      }

      // ±10922 so three tables sum to ~full Q15
      table[i] = (int16_t)(constrain(outVal * 32767.0f / 3.0f, -10922.0f, 10922.0f));
    }
  }
};

// ---------------------------------------------------------------------------
class ProNoise32 {
public:
  ProNoise32() { begin(NOISE_PINK, 0x811C9DC5u); }

  ProNoise32(NoiseColor color, uint32_t seed_val = 0x811C9DC5u) {
    begin(color, seed_val);
  }

  void begin(NoiseColor color, uint32_t seed_val = 0x811C9DC5u) {
    seed = (seed_val == 0u) ? 1u : seed_val;
    currentColor = color;
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0;
    brown_state = 0;
  }

  void setColor(NoiseColor newColor) { currentColor = newColor; }

  // Brown: leaky integrator; << 3 after >>7-equivalent scaling → Q15 + clamp.
  static constexpr int BROWN_TO_Q15_SHIFT = 3;
  // Pink: was >> 6 (/64); >> 4 is 4× louder toward white/brown depth.
  static constexpr int PINK_TO_Q15_SHIFT = 4;

  inline int16_t process() {
    int32_t w = (int32_t)(xorshift32() >> 16) - 32768;

    if (currentColor == NOISE_PINK) {
      b0 = b0 - q_mul(b0, 75) + q_mul(w, 3638);
      b1 = b1 - q_mul(b1, 438) + q_mul(w, 4920);
      b2 = b2 - q_mul(b2, 2032) + q_mul(w, 10082);
      b3 = b3 - q_mul(b3, 8749) + q_mul(w, 20347);
      b4 = b4 - q_mul(b4, 29491) + q_mul(w, 34927);
      b5 = -q_mul(b5, 49912) - q_mul(w, 1107);

      int32_t pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + q_mul(w, 35140);
      b6 = q_mul(w, 7597);

      return dcoNoiseClampQ15(pink >> PINK_TO_Q15_SHIFT);
    }
    if (currentColor == NOISE_BROWN) {
      brown_state += w;
      brown_state -= (brown_state + 64) >> 7;
      return dcoNoiseClampQ15(brown_state >> (7 - BROWN_TO_Q15_SHIFT));
    }
    return (int16_t)w;
  }

  inline int16_t next() { return process(); }

  int16_t minOut() const { return -32768; }
  int16_t maxOut() const { return 32767; }

private:
  NoiseColor currentColor;
  uint32_t seed;
  int32_t b0, b1, b2, b3, b4, b5, b6;
  int32_t brown_state;

  inline uint32_t xorshift32() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  }

  inline int32_t q_mul(int32_t val, int32_t mult) {
    return (val * mult + 32768) >> 16;
  }
};

#if NOISE_ENGINE == 3
using DcoNoiseGen = ProNoise32;
#elif NOISE_ENGINE == 2
using DcoNoiseGen = PrimeHybridNoise;
#elif NOISE_ENGINE == 1
using DcoNoiseGen = FastNoiseGen;
#else
using DcoNoiseGen = ColoredNoise;
#endif

#endif  // DCO_NOISE_H
