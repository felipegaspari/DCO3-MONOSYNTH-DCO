#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/ADSR_Bezier/examples/compare_fixed_float/README.md"
# Backend drift regression test

Host-side check that fixed Q24/Q16 and FLOAT=1 hybrid path (FPU floor index + Q16 amp) stay within ±1 of golden uint64 reference math.

## Build and run

```bash
g++ -std=c++17 -O2 -o compare compare.cpp && ./compare
./compare -q    # optional: summary + PASS/FAIL only
```

Use `-DARRAY_SIZE=512` to match DCO table size (default).

`ADSR_BEZIER_SRAM_HOT` is **N/A** here (host g++ test, no RP2040 `.time_critical`).

## Output sections

| Section | What it measures |
|---------|------------------|
| **Index accuracy** | `floor((N-1)*delta/phase)` vs fixed Q24 / FLOAT=1 FPU trunc |
| **Output accuracy** | Round-nearest `(curve*range + vert/2)/vert` vs Q16 trunc (fixed and float1) |
| **DCO profile** | Millis phases 1–60000 ms, vert 4000/4095 — production-relevant subset |
| **Cross-backend** | Semantic diffs on DCO grid (not PASS/FAIL) |
| **Fixed vs FLOAT=1** | Direct agreement per phase/vert (match%, drift histogram) |
| **Accuracy vs golden** | Side-by-side exact%, mean drift, and which backend is closer |
| **Summary** | Global max drift + PASS/FAIL gate + float vs fixed accuracy |

### Column meanings

- **samples** — test points (boundary deltas + stepped sweep)
- **exact%** — fraction with zero drift vs golden
- **drift=1 / drift>1** — histogram buckets
- **max** — worst absolute error
- **worst_case** — coordinates at max drift (`delta`, `curve`, `range`, `ref`, `got`)

### Index path tags

| Path | Backend | When |
|------|---------|------|
| `q24` | fixed | phase ≤ 2 s (active timebase) |
| `uint64` | fixed | long phases |
| `fpu_trunc` | float1 | `(uint32_t)((float)delta * rate)` (matches ADSR_Bezier.h) |
| `zero` | float1 | phase_ticks == 0 |

### Output path tags

| Path | Backend | When |
|------|---------|------|
| `q16_trunc` | fixed | int32 multiply + truncating shift |
| `q16_trunc` | float1 | uint64 precomputed `range_scale_q16` + `>> 16` (matches ADSR_Bezier.h) |

## Pass criteria

- **PASS** when optimized FLOAT=1 index/output drift vs golden is ≤ 1 step / ≤ 1 LSB across all sweeps.
- Fixed backend drift vs golden is informational only.
- FLOAT=1 index uses **FPU floor** — should track golden floor closely; occasional ±1 from float32 precision.
- **Cross-backend** `fixed output vs float1 output` should differ minimally (both Q16 trunc; any gap is from int32 vs uint64 multiply in the fixed compare helper).

## Interpreting fixed vs float1

### Direct agreement (`Fixed vs FLOAT=1 agreement`)

- **match%** — fraction where fixed and float1 produce identical index/output.
- **drift=1 / max** — when they differ, by how much (index steps or output LSBs).
- **worst_case** — `fixed=` and `float1=` values at max drift.

### Accuracy vs golden (`Accuracy vs golden`)

For each sample, compares distance to golden reference:

- **exact%** — how often each backend hits golden exactly.
- **fixed_wins / float1_wins / tie** — which backend is closer to golden (smaller absolute error).
- **mean_drift** — average absolute error vs golden (lower is better).

Use **index millis (DCO)** and **output** rows for production-relevant conclusions.

## Interpreting cross-backend

On the DCO profile grid you may see:

- **fixed index vs float1 index** — may differ (Q24/floor vs FPU trunc at float boundaries).
- **fixed output vs golden** — up to 1 LSB from Q16 truncation vs round-nearest golden.
- **float1 output vs golden** — same as fixed (Q16 trunc); up to 1 LSB vs golden.
- **fixed output vs float1 output** — should match or differ by at most 1 LSB (same formula, different multiply width in compare helpers).
