#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO_Noise/README.md"
# DCO_Noise

Arduino-style noise engines. Every engine / color returns full-resolution
**int16 Q15** (`−32768…32767`). There is no amplitude-range mapping in the
library — scale in the consumer if you need a narrower band (e.g. `sample >> 3`
for roughly ±4096).

## Engines

| Class | Notes |
|-------|--------|
| `ColoredNoise` | Voss pink / 1-pole brown / white; whites from `PioNoiseWhite` |
| `FastNoiseGen` | Economy Voss pink / leaky brown / local xorshift white |
| `PrimeHybridNoise` | Three prime tables per gen (997/1499/1999); dither + rephase |
| `ProNoise32` | Q16.15 Kellett pink / DC-corrected brown / xorshift white |

## Flags (define before `#include`)

| Define | Effect |
|--------|--------|
| `NOISE_ENGINE` `0`…`3` | Selects `DcoNoiseGen` alias (default `0` = ColoredNoise) |
| `ENABLE_NOISE_OUT` | Also enable PIO LFSR path for pin listen (engines 1–3) |

Library provides:

- `DcoNoiseGen` — typedef to the selected engine class
- `dcoNoiseUsesPioWhite()` — true for engine 0 or `ENABLE_NOISE_OUT`
- `dcoNoisePioBegin(pio, sm)` / `dcoNoisePioRefill()` — no-op when unused
- `minOut()` / `maxOut()` — always `-32768` / `32767` (for mod-matrix style APIs)

## Usage

```cpp
#include <DCO_Noise.h>

// Ctor / begin: (color, seed) only
ProNoise32 noisePink(NOISE_PINK, 0xC0FFEE02u);
ProNoise32 noiseWhite(NOISE_WHITE, 0xC0FFEE01u);

void setup() {
  dcoNoisePioBegin(pio1, 1);  // after loading LFSR program if needed
}

void loop() {
  dcoNoisePioRefill();
  int16_t s = noisePink.next();  // Q15
  (void)s;
}
```

DCO3 declares `noise0`…`noise1` in `DCO/noise.h` that way (`NUM_NOISE_GENS = 2`).
