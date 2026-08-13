#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/ADSR_Bezier/examples/compare_fixed_float/compare.cpp"
// Host-side regression: fixed Q24/Q16, FLOAT=1 FPU floor index + Q16 amp hybrid, vs golden.
// Build: g++ -std=c++17 -O2 -o compare compare.cpp && ./compare
// Flags: default = full tables + worst-case coords; -q summary only

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE 512
#endif

static constexpr unsigned long TIME_INDEX_MAX_US = 20000000UL; // fixed Q24 cap (micros)
static constexpr unsigned long TIME_INDEX_MAX_MS = 2000UL;    // fixed Q24 cap (millis)

static bool g_verbose = true;
static bool g_quiet = false;

struct Float1IndexRate
{
    float rate;
    bool zero_phase;
};

struct AccuracyStats
{
    uint64_t samples = 0;
    uint64_t exact = 0;
    uint64_t drift1 = 0;
    uint64_t drift_gt1 = 0;
    int max_drift = 0;
    unsigned long w_delta = 0;
    unsigned long w_phase = 0;
    int w_curve = 0;
    int w_range = 0;
    int32_t w_start = 0;
    int32_t w_ref = 0;
    int32_t w_got = 0;
    const char *w_path = nullptr;
};

struct CrossStats
{
    uint64_t samples = 0;
    uint64_t differing = 0;
    int max_abs_diff = 0;
};

struct GoldenRaceStats
{
    uint64_t samples = 0;
    uint64_t fixed_wins = 0;
    uint64_t float1_wins = 0;
    uint64_t ties = 0;
    uint64_t fixed_exact = 0;
    uint64_t float1_exact = 0;
    uint64_t fixed_drift_sum = 0;
    uint64_t float1_drift_sum = 0;
};

static void statsReset(AccuracyStats &s)
{
    s = AccuracyStats{};
}

static void statsUpdateIndex(AccuracyStats &s,
                             unsigned long delta,
                             unsigned long phase_ticks,
                             uint32_t ref,
                             uint32_t got,
                             const char *path)
{
    s.samples++;
    const int diff = (got >= ref) ? (int)(got - ref) : (int)(ref - got);
    if (diff == 0)
        s.exact++;
    else if (diff == 1)
        s.drift1++;
    else
        s.drift_gt1++;

    if (diff > s.max_drift)
    {
        s.max_drift = diff;
        s.w_delta = delta;
        s.w_phase = phase_ticks;
        s.w_ref = (int32_t)ref;
        s.w_got = (int32_t)got;
        s.w_path = path;
    }
}

static void statsUpdateOutput(AccuracyStats &s,
                              int32_t start,
                              int curve,
                              int32_t range,
                              int32_t ref,
                              int32_t got,
                              const char *path)
{
    s.samples++;
    const int diff = (got >= ref) ? (int)(got - ref) : (int)(ref - got);
    if (diff == 0)
        s.exact++;
    else if (diff == 1)
        s.drift1++;
    else
        s.drift_gt1++;

    if (diff > s.max_drift)
    {
        s.max_drift = diff;
        s.w_start = start;
        s.w_curve = curve;
        s.w_range = (int)range;
        s.w_ref = ref;
        s.w_got = got;
        s.w_path = path;
    }
}

static void crossUpdate(CrossStats &s, int32_t a, int32_t b)
{
    s.samples++;
    const int diff = (a >= b) ? (int)(a - b) : (int)(b - a);
    if (diff != 0)
        s.differing++;
    if (diff > s.max_abs_diff)
        s.max_abs_diff = diff;
}

static int absDiff32(int32_t a, int32_t b)
{
    return (a >= b) ? (int)(a - b) : (int)(b - a);
}

static void goldenRaceReset(GoldenRaceStats &s)
{
    s = GoldenRaceStats{};
}

static void goldenRaceUpdate(GoldenRaceStats &s, int32_t fixed_val, int32_t float1_val, int32_t golden)
{
    s.samples++;
    const int d_fixed = absDiff32(fixed_val, golden);
    const int d_float1 = absDiff32(float1_val, golden);
    s.fixed_drift_sum += (uint64_t)d_fixed;
    s.float1_drift_sum += (uint64_t)d_float1;
    if (d_fixed == 0)
        s.fixed_exact++;
    if (d_float1 == 0)
        s.float1_exact++;
    if (d_fixed < d_float1)
        s.fixed_wins++;
    else if (d_float1 < d_fixed)
        s.float1_wins++;
    else
        s.ties++;
}

static double goldenRaceExactPct(const GoldenRaceStats &s, bool fixed_backend)
{
    if (s.samples == 0)
        return 100.0;
    const uint64_t exact = fixed_backend ? s.fixed_exact : s.float1_exact;
    return 100.0 * (double)exact / (double)s.samples;
}

static double goldenRaceMeanDrift(const GoldenRaceStats &s, bool fixed_backend)
{
    if (s.samples == 0)
        return 0.0;
    const uint64_t sum = fixed_backend ? s.fixed_drift_sum : s.float1_drift_sum;
    return (double)sum / (double)s.samples;
}

static double statsExactPct(const AccuracyStats &s)
{
    if (s.samples == 0)
        return 100.0;
    return 100.0 * (double)s.exact / (double)s.samples;
}

static void printIndexHeader()
{
    if (g_quiet)
        return;
    printf("backend   phase     path        samples   exact%%  drift=1  drift>1  max  worst_case\n");
}

static void printIndexRow(const char *backend,
                          unsigned long phase_ticks,
                          const char *path,
                          const AccuracyStats &s)
{
    if (g_quiet)
        return;
    char worst[128];
    if (s.max_drift == 0)
        snprintf(worst, sizeof(worst), "(exact)");
    else
        snprintf(worst, sizeof(worst), "delta=%lu ref=%ld got=%ld",
                 s.w_delta, (long)s.w_ref, (long)s.w_got);

    printf("%-9s %9lu  %-11s %8llu  %6.1f%%  %7llu  %7llu  %3d  %s\n",
           backend, phase_ticks, path, (unsigned long long)s.samples,
           statsExactPct(s), (unsigned long long)s.drift1,
           (unsigned long long)s.drift_gt1, s.max_drift, worst);
}

static void printOutputHeader()
{
    if (g_quiet)
        return;
    printf("backend   vert  path        samples   exact%%  drift=1  drift>1  max  worst_case\n");
}

static void printPairIndexHeader()
{
    if (g_quiet)
        return;
    printf("phase     samples  match%%  drift=1  drift>1  max  worst_case\n");
}

static void printPairIndexRow(unsigned long phase_ticks, const AccuracyStats &s)
{
    if (g_quiet)
        return;
    char worst[160];
    if (s.max_drift == 0)
        snprintf(worst, sizeof(worst), "(identical)");
    else
        snprintf(worst, sizeof(worst), "delta=%lu fixed=%ld float1=%ld",
                 s.w_delta, (long)s.w_ref, (long)s.w_got);

    printf("%9lu  %8llu  %6.1f%%  %7llu  %7llu  %3d  %s\n",
           phase_ticks, (unsigned long long)s.samples, statsExactPct(s),
           (unsigned long long)s.drift1, (unsigned long long)s.drift_gt1,
           s.max_drift, worst);
}

static void printPairOutputHeader()
{
    if (g_quiet)
        return;
    printf("vert  samples  match%%  drift=1  drift>1  max  worst_case\n");
}

static void printPairOutputRow(int vertical_resolution, const AccuracyStats &s)
{
    if (g_quiet)
        return;
    char worst[160];
    if (s.max_drift == 0)
        snprintf(worst, sizeof(worst), "(identical)");
    else
        snprintf(worst, sizeof(worst), "curve=%d range=%d fixed=%ld float1=%ld",
                 s.w_curve, s.w_range, (long)s.w_ref, (long)s.w_got);

    printf("%5d  %8llu  %6.1f%%  %7llu  %7llu  %3d  %s\n",
           vertical_resolution, (unsigned long long)s.samples, statsExactPct(s),
           (unsigned long long)s.drift1, (unsigned long long)s.drift_gt1,
           s.max_drift, worst);
}

static void printGoldenRaceRow(const char *label, const GoldenRaceStats &s)
{
    if (g_quiet)
        return;
    printf("%-28s %8llu  fixed %5.1f%%  float1 %5.1f%%  "
           "fixed_wins %6llu  float1_wins %6llu  tie %6llu  "
           "mean_drift fixed %.4f  float1 %.4f\n",
           label, (unsigned long long)s.samples,
           goldenRaceExactPct(s, true), goldenRaceExactPct(s, false),
           (unsigned long long)s.fixed_wins, (unsigned long long)s.float1_wins,
           (unsigned long long)s.ties,
           goldenRaceMeanDrift(s, true), goldenRaceMeanDrift(s, false));
}

static void printOutputRow(const char *backend,
                           int vertical_resolution,
                           const char *path,
                           const AccuracyStats &s)
{
    if (g_quiet)
        return;
    char worst[160];
    if (s.max_drift == 0)
        snprintf(worst, sizeof(worst), "(exact)");
    else
        snprintf(worst, sizeof(worst), "curve=%d range=%d ref=%ld got=%ld",
                 s.w_curve, s.w_range, (long)s.w_ref, (long)s.w_got);

    printf("%-9s %5d  %-11s %8llu  %6.1f%%  %7llu  %7llu  %3d  %s\n",
           backend, vertical_resolution, path, (unsigned long long)s.samples,
           statsExactPct(s), (unsigned long long)s.drift1,
           (unsigned long long)s.drift_gt1, s.max_drift, worst);
}

static const char *dominantIndexPath(const AccuracyStats *paths, int count)
{
    const char *best = "unknown";
    uint64_t best_n = 0;
    for (int i = 0; i < count; i++)
    {
        if (paths[i].samples > best_n && paths[i].w_path)
        {
            best_n = paths[i].samples;
            best = paths[i].w_path;
        }
    }
    return best;
}

static void printIndexSubRowsIfNeeded(const char *backend,
                                      unsigned long phase_ticks,
                                      const AccuracyStats *paths,
                                      int count,
                                      const AccuracyStats &row)
{
    int active = 0;
    for (int i = 0; i < count; i++)
    {
        if (paths[i].samples > 0)
            active++;
    }
    if (active <= 1)
        return;
    for (int i = 0; i < count; i++)
    {
        if (paths[i].samples == 0)
            continue;
        if (paths[i].w_path && row.w_path && strcmp(paths[i].w_path, row.w_path) == 0 &&
            paths[i].max_drift == row.max_drift)
            continue;
        printIndexRow(backend, phase_ticks, paths[i].w_path ? paths[i].w_path : "?", paths[i]);
    }
}

static void printVerboseIndexWorst(const char *label, const AccuracyStats &s, int cap)
{
    if (g_quiet || !g_verbose || s.max_drift == 0)
        return;
    printf("  [%s worst via %s] delta=%lu phase=%lu ref=%ld got=%ld diff=%d\n",
           label, s.w_path ? s.w_path : "?", s.w_delta, s.w_phase,
           (long)s.w_ref, (long)s.w_got, s.max_drift);
    (void)cap;
}

static void printVerboseOutputWorst(const char *label, const AccuracyStats &s, int cap)
{
    if (g_quiet || !g_verbose || s.max_drift == 0)
        return;
    printf("  [%s worst via %s] start=%ld curve=%d range=%d ref=%ld got=%ld diff=%d\n",
           label, s.w_path ? s.w_path : "?", (long)s.w_start, s.w_curve, s.w_range,
           (long)s.w_ref, (long)s.w_got, s.max_drift);
    (void)cap;
}

static Float1IndexRate precomputeFloat1IndexRate(unsigned long phase_ticks, int array_size)
{
    if (phase_ticks == 0)
        return {0.0f, true};
    return {(float)(array_size - 1) / (float)phase_ticks, false};
}

static uint32_t referenceIndexFromDelta(unsigned long delta,
                                        unsigned long phase_ticks,
                                        int array_size)
{
    if (phase_ticks == 0)
        return 0;
    uint32_t idx = (uint32_t)(((uint64_t)(array_size - 1) * (uint64_t)delta) /
                              (uint64_t)phase_ticks);
    if (idx >= (uint32_t)array_size)
        idx = (uint32_t)array_size - 1;
    return idx;
}

static uint32_t fixedIndexEval(unsigned long delta,
                               unsigned long phase_ticks,
                               int array_size,
                               unsigned long index_max_ticks,
                               const char **path_out)
{
    uint32_t idx;
    if (phase_ticks > 0 && phase_ticks <= index_max_ticks)
    {
        if (path_out)
            *path_out = "q24";
        const uint64_t scale_q24 =
            (((uint64_t)(array_size - 1)) << 24) / (uint64_t)phase_ticks;
        idx = (uint32_t)(((uint64_t)delta * scale_q24) >> 24);
    }
    else if (phase_ticks > 0)
    {
        if (path_out)
            *path_out = "uint64";
        idx = (uint32_t)(((uint64_t)(array_size - 1) * (uint64_t)delta) /
                         (uint64_t)phase_ticks);
    }
    else
    {
        if (path_out)
            *path_out = "zero";
        idx = 0;
    }
    if (idx >= (uint32_t)array_size)
        idx = (uint32_t)array_size - 1;
    return idx;
}

static uint32_t float1IndexEval(unsigned long delta,
                                int array_size,
                                const Float1IndexRate &r,
                                const char **path_out)
{
    if (r.zero_phase)
    {
        if (path_out)
            *path_out = "zero";
        return 0;
    }
    if (path_out)
        *path_out = "fpu_trunc";
    uint32_t idx = (uint32_t)((float)delta * r.rate);
    if (idx >= (uint32_t)array_size)
        idx = (uint32_t)array_size - 1;
    return idx;
}

static int32_t referenceRangeOutput(int32_t start,
                                    int curve_val,
                                    int32_t range,
                                    int vertical_resolution)
{
    if (vertical_resolution <= 0 || range <= 0 || curve_val <= 0)
        return start;
    const int64_t num = (int64_t)curve_val * (int64_t)range;
    const int64_t half = (int64_t)vertical_resolution / 2;
    return start + (int32_t)((num + half) / (int64_t)vertical_resolution);
}

static int32_t fixedOutputEval(int32_t start,
                               int curve_val,
                               int32_t range,
                               int vertical_resolution,
                               const char **path_out)
{
    if (path_out)
        *path_out = "q16_trunc";
    if (vertical_resolution <= 0 || range <= 0 || curve_val <= 0)
        return start;
    const int32_t range_q16 = (int32_t)(((int32_t)range << 16) / vertical_resolution);
    return start + (int32_t)(((int32_t)curve_val * range_q16) >> 16);
}

static uint32_t precomputeRangeScaleQ16(int32_t range, int vertical_resolution)
{
    if (vertical_resolution <= 0 || range <= 0)
        return 0;
    return (uint32_t)(((uint64_t)range << 16) / (uint64_t)vertical_resolution);
}

static int32_t float1OutputEval(int32_t start,
                                int curve_val,
                                int32_t range,
                                int vertical_resolution,
                                const char **path_out)
{
    if (vertical_resolution <= 0)
    {
        if (path_out)
            *path_out = "skip";
        return start;
    }
    if (path_out)
        *path_out = "q16_trunc";
    const uint32_t scale_q16 = precomputeRangeScaleQ16(range, vertical_resolution);
    return start + (int32_t)(((uint64_t)curve_val * scale_q16) >> 16);
}

static void collectIndexDelta(unsigned long phase_ticks,
                              unsigned long *deltas,
                              int *count,
                              int max_count)
{
    int n = 0;
    if (phase_ticks == 0)
    {
        deltas[n++] = 0;
        *count = n;
        return;
    }

    const unsigned long boundaries[] = {
        0UL,
        1UL,
        phase_ticks / 2UL,
        (phase_ticks > 1) ? phase_ticks - 1UL : 0UL,
    };
    for (unsigned long b : boundaries)
    {
        if (b >= phase_ticks)
            continue;
        bool dup = false;
        for (int i = 0; i < n; i++)
        {
            if (deltas[i] == b)
            {
                dup = true;
                break;
            }
        }
        if (!dup && n < max_count)
            deltas[n++] = b;
    }

    unsigned long step = phase_ticks / (unsigned long)(ARRAY_SIZE * 4);
    if (step == 0)
        step = 1;
    for (unsigned long delta = 0; delta < phase_ticks; delta += step)
    {
        bool dup = false;
        for (int i = 0; i < n; i++)
        {
            if (deltas[i] == delta)
            {
                dup = true;
                break;
            }
        }
        if (!dup && n < max_count)
            deltas[n++] = delta;
    }
    *count = n;
}

static void sweepFixedIndex(unsigned long phase_ticks,
                            int array_size,
                            unsigned long index_max_ticks,
                            AccuracyStats *by_path,
                            int *max_vs_ref)
{
    unsigned long deltas[8192];
    int delta_count = 0;
    collectIndexDelta(phase_ticks, deltas, &delta_count, (int)(sizeof(deltas) / sizeof(deltas[0])));

    AccuracyStats row;
    statsReset(row);
    statsReset(by_path[0]); // q24
    statsReset(by_path[1]); // uint64
    statsReset(by_path[2]); // zero
    by_path[0].w_path = "q24";
    by_path[1].w_path = "uint64";
    by_path[2].w_path = "zero";

    for (int i = 0; i < delta_count; i++)
    {
        const unsigned long delta = deltas[i];
        const char *path = nullptr;
        const uint32_t got = fixedIndexEval(delta, phase_ticks, array_size, index_max_ticks, &path);
        const uint32_t ref = referenceIndexFromDelta(delta, phase_ticks, array_size);
        statsUpdateIndex(row, delta, phase_ticks, ref, got, path);
        if (strcmp(path, "q24") == 0)
            statsUpdateIndex(by_path[0], delta, phase_ticks, ref, got, path);
        else if (strcmp(path, "uint64") == 0)
            statsUpdateIndex(by_path[1], delta, phase_ticks, ref, got, path);
        else
            statsUpdateIndex(by_path[2], delta, phase_ticks, ref, got, path);
    }

    row.w_path = dominantIndexPath(by_path, 3);

    if (row.max_drift > *max_vs_ref)
        *max_vs_ref = row.max_drift;

    printIndexRow("fixed", phase_ticks, row.w_path ? row.w_path : "?", row);
    printIndexSubRowsIfNeeded("fixed", phase_ticks, by_path, 3, row);
    printVerboseIndexWorst("fixed", row, 5);
}

static void sweepFloat1Index(unsigned long phase_ticks,
                             int array_size,
                             AccuracyStats *by_path,
                             int *max_vs_ref)
{
    unsigned long deltas[8192];
    int delta_count = 0;
    collectIndexDelta(phase_ticks, deltas, &delta_count, (int)(sizeof(deltas) / sizeof(deltas[0])));

    const Float1IndexRate index_rate = precomputeFloat1IndexRate(phase_ticks, array_size);

    AccuracyStats row;
    statsReset(row);
    statsReset(by_path[0]); // fpu_trunc
    statsReset(by_path[1]); // zero
    by_path[0].w_path = "fpu_trunc";
    by_path[1].w_path = "zero";

    for (int i = 0; i < delta_count; i++)
    {
        const unsigned long delta = deltas[i];
        const char *path = nullptr;
        const uint32_t got = float1IndexEval(delta, array_size, index_rate, &path);
        const uint32_t ref = referenceIndexFromDelta(delta, phase_ticks, array_size);
        statsUpdateIndex(row, delta, phase_ticks, ref, got, path);
        if (strcmp(path, "fpu_trunc") == 0)
            statsUpdateIndex(by_path[0], delta, phase_ticks, ref, got, path);
        else
            statsUpdateIndex(by_path[1], delta, phase_ticks, ref, got, path);
    }

    row.w_path = dominantIndexPath(by_path, 2);

    if (row.max_drift > *max_vs_ref)
        *max_vs_ref = row.max_drift;

    printIndexRow("float1", phase_ticks, row.w_path ? row.w_path : "?", row);
    printIndexSubRowsIfNeeded("float1", phase_ticks, by_path, 2, row);
    printVerboseIndexWorst("float1", row, 5);
}

static bool outputGridValue(int value, int max_val, int coarse_step)
{
    if (value <= 4 || value >= max_val - 4)
        return true;
    if (value == max_val / 2 || value == max_val / 4 || value == (3 * max_val) / 4)
        return true;
    return (value % coarse_step) == 0;
}

static void sweepFixedOutput(int vertical_resolution, AccuracyStats *max_vs_ref)
{
    AccuracyStats row;
    statsReset(row);

    const int32_t start = 1000;
    const int coarse = 137;

    for (int range = 0; range <= vertical_resolution; range++)
    {
        if (!outputGridValue(range, vertical_resolution, coarse))
            continue;
        for (int curve = 0; curve <= vertical_resolution; curve++)
        {
            if (!outputGridValue(curve, vertical_resolution, 211))
                continue;
            const char *path = nullptr;
            const int32_t got = fixedOutputEval(start, curve, range, vertical_resolution, &path);
            const int32_t ref = referenceRangeOutput(start, curve, range, vertical_resolution);
            statsUpdateOutput(row, start, curve, range, ref, got, path);
        }
    }

    if (row.max_drift > max_vs_ref->max_drift)
        *max_vs_ref = row;

    printOutputRow("fixed", vertical_resolution, "q16_trunc", row);
    printVerboseOutputWorst("fixed", row, 5);
}

static void sweepFloat1Output(int vertical_resolution, AccuracyStats *max_vs_ref)
{
    AccuracyStats row;
    AccuracyStats q16_trunc;
    statsReset(row);
    statsReset(q16_trunc);

    const int32_t start = 1000;
    const int coarse = 137;

    for (int range = 0; range <= vertical_resolution; range++)
    {
        if (!outputGridValue(range, vertical_resolution, coarse))
            continue;
        for (int curve = 0; curve <= vertical_resolution; curve++)
        {
            if (!outputGridValue(curve, vertical_resolution, 211))
                continue;
            const char *path = nullptr;
            const int32_t got = float1OutputEval(start, curve, range, vertical_resolution, &path);
            const int32_t ref = referenceRangeOutput(start, curve, range, vertical_resolution);
            statsUpdateOutput(row, start, curve, range, ref, got, path);
            if (strcmp(path, "q16_trunc") == 0)
                statsUpdateOutput(q16_trunc, start, curve, range, ref, got, path);
        }
    }

    if (row.max_drift > max_vs_ref->max_drift)
        *max_vs_ref = row;

    printOutputRow("float1", vertical_resolution, "all", row);
    if (q16_trunc.samples > 0)
        printOutputRow("float1", vertical_resolution, "q16_trunc", q16_trunc);
    printVerboseOutputWorst("float1", row, 5);
}

static void sweepIndexFixedVsFloat1(unsigned long phase_ticks,
                                    int array_size,
                                    unsigned long index_max_ticks,
                                    AccuracyStats *pair_stats,
                                    GoldenRaceStats *race_stats,
                                    int *max_pair_diff)
{
    unsigned long deltas[8192];
    int delta_count = 0;
    collectIndexDelta(phase_ticks, deltas, &delta_count, (int)(sizeof(deltas) / sizeof(deltas[0])));

    const Float1IndexRate index_rate = precomputeFloat1IndexRate(phase_ticks, array_size);
    AccuracyStats row;
    statsReset(row);

    for (int i = 0; i < delta_count; i++)
    {
        const unsigned long delta = deltas[i];
        const char *path = nullptr;
        const uint32_t fixed_got =
            fixedIndexEval(delta, phase_ticks, array_size, index_max_ticks, &path);
        const uint32_t float1_got =
            float1IndexEval(delta, array_size, index_rate, &path);
        const uint32_t golden = referenceIndexFromDelta(delta, phase_ticks, array_size);

        statsUpdateIndex(row, delta, phase_ticks, fixed_got, float1_got, "pair");
        if (race_stats)
            goldenRaceUpdate(*race_stats, (int32_t)fixed_got, (int32_t)float1_got, (int32_t)golden);
    }

    if (row.max_drift > *max_pair_diff)
        *max_pair_diff = row.max_drift;
    if (pair_stats && row.max_drift > pair_stats->max_drift)
        *pair_stats = row;
    printPairIndexRow(phase_ticks, row);
}

static void sweepOutputFixedVsFloat1(int vertical_resolution,
                                     AccuracyStats *pair_stats,
                                     GoldenRaceStats *race_stats,
                                     int *max_pair_diff,
                                     uint64_t *pair_samples,
                                     uint64_t *pair_exact)
{
    AccuracyStats row;
    statsReset(row);

    const int32_t start = 1000;
    const int coarse = 137;

    for (int range = 0; range <= vertical_resolution; range++)
    {
        if (!outputGridValue(range, vertical_resolution, coarse))
            continue;
        for (int curve = 0; curve <= vertical_resolution; curve++)
        {
            if (!outputGridValue(curve, vertical_resolution, 211))
                continue;
            const char *path = nullptr;
            const int32_t fixed_got = fixedOutputEval(start, curve, range, vertical_resolution, &path);
            const int32_t float1_got =
                float1OutputEval(start, curve, range, vertical_resolution, &path);
            const int32_t golden = referenceRangeOutput(start, curve, range, vertical_resolution);

            statsUpdateOutput(row, start, curve, range, fixed_got, float1_got, "pair");
            if (race_stats)
                goldenRaceUpdate(*race_stats, fixed_got, float1_got, golden);
        }
    }

    if (pair_samples)
        *pair_samples += row.samples;
    if (pair_exact)
        *pair_exact += row.exact;

    if (row.max_drift > *max_pair_diff)
        *max_pair_diff = row.max_drift;
    if (pair_stats && row.max_drift > pair_stats->max_drift)
        *pair_stats = row;
    printPairOutputRow(vertical_resolution, row);
}

static void runFixedVsFloat1Sections(int array_size,
                                     int *max_idx_pair,
                                     int *max_out_pair,
                                     GoldenRaceStats *idx_race_micros,
                                     GoldenRaceStats *idx_race_millis,
                                     GoldenRaceStats *out_race,
                                     uint64_t *out_pair_samples,
                                     uint64_t *out_pair_exact)
{
    const unsigned long micros_ticks[] = {
        1000UL, 100000UL, 2000000UL, 10000000UL, 20000000UL, 60000000UL,
    };
    const unsigned long millis_ticks[] = {
        1UL, 10UL, 50UL, 100UL, 500UL, 2000UL, 10000UL, 60000UL,
    };

    if (!g_quiet)
    {
        printf("\n=== Fixed vs FLOAT=1 agreement (direct) ===\n");
        printf("  Index (micros-scale ticks):\n");
        printPairIndexHeader();
    }
    for (unsigned long phase : micros_ticks)
        sweepIndexFixedVsFloat1(phase, array_size, TIME_INDEX_MAX_US, nullptr,
                                idx_race_micros, max_idx_pair);

    if (!g_quiet)
    {
        printf("\n  Index (millis-scale ticks / DCO):\n");
        printPairIndexHeader();
    }
    for (unsigned long phase : millis_ticks)
        sweepIndexFixedVsFloat1(phase, array_size, TIME_INDEX_MAX_MS, nullptr,
                                idx_race_millis, max_idx_pair);

    if (!g_quiet)
    {
        printf("\n  Output (vert 4000/4095):\n");
        printPairOutputHeader();
    }
    sweepOutputFixedVsFloat1(4000, nullptr, out_race, max_out_pair, out_pair_samples, out_pair_exact);
    sweepOutputFixedVsFloat1(4095, nullptr, out_race, max_out_pair, out_pair_samples, out_pair_exact);

    if (g_quiet)
        return;

    GoldenRaceStats idx_all;
    goldenRaceReset(idx_all);
    idx_all.samples = idx_race_micros->samples + idx_race_millis->samples;
    idx_all.fixed_wins = idx_race_micros->fixed_wins + idx_race_millis->fixed_wins;
    idx_all.float1_wins = idx_race_micros->float1_wins + idx_race_millis->float1_wins;
    idx_all.ties = idx_race_micros->ties + idx_race_millis->ties;
    idx_all.fixed_exact = idx_race_micros->fixed_exact + idx_race_millis->fixed_exact;
    idx_all.float1_exact = idx_race_micros->float1_exact + idx_race_millis->float1_exact;
    idx_all.fixed_drift_sum = idx_race_micros->fixed_drift_sum + idx_race_millis->fixed_drift_sum;
    idx_all.float1_drift_sum = idx_race_micros->float1_drift_sum + idx_race_millis->float1_drift_sum;

    printf("\n=== Accuracy vs golden (fixed vs float1) ===\n");
    printf("domain                       samples  exact%%           closer_to_golden          mean_drift\n");
    printGoldenRaceRow("index micros", *idx_race_micros);
    printGoldenRaceRow("index millis (DCO)", *idx_race_millis);
    printGoldenRaceRow("index all sweeps", idx_all);
    printGoldenRaceRow("output vert 4000/4095", *out_race);
}

static void runCrossBackendDco(int array_size)
{
    const unsigned long dco_phases_ms[] = {1UL, 10UL, 50UL, 100UL, 500UL, 2000UL, 10000UL, 60000UL};
    const int verts[] = {4000, 4095};

    CrossStats idx_fixed_golden;
    CrossStats idx_float1_golden;
    CrossStats idx_fixed_float1;
    CrossStats out_fixed_golden;
    CrossStats out_float1_golden;
    CrossStats out_fixed_float1;

    for (unsigned long phase : dco_phases_ms)
    {
        unsigned long deltas[8192];
        int delta_count = 0;
        collectIndexDelta(phase, deltas, &delta_count, (int)(sizeof(deltas) / sizeof(deltas[0])));
        const Float1IndexRate index_rate = precomputeFloat1IndexRate(phase, array_size);

        for (int i = 0; i < delta_count; i++)
        {
            const unsigned long delta = deltas[i];
            const char *path = nullptr;
            const uint32_t fixed_got =
                fixedIndexEval(delta, phase, array_size, TIME_INDEX_MAX_MS, &path);
            const uint32_t float1_got =
                float1IndexEval(delta, array_size, index_rate, &path);
            const uint32_t ref = referenceIndexFromDelta(delta, phase, array_size);
            crossUpdate(idx_fixed_golden, (int32_t)fixed_got, (int32_t)ref);
            crossUpdate(idx_float1_golden, (int32_t)float1_got, (int32_t)ref);
            crossUpdate(idx_fixed_float1, (int32_t)fixed_got, (int32_t)float1_got);
        }
    }

    const int32_t start = 1000;
    const int coarse = 137;
    for (int vi = 0; vi < 2; vi++)
    {
        const int vert = verts[vi];
        for (int range = 0; range <= vert; range++)
        {
            if (!outputGridValue(range, vert, coarse))
                continue;
            for (int curve = 0; curve <= vert; curve++)
            {
                if (!outputGridValue(curve, vert, 211))
                    continue;
                const char *path = nullptr;
                const int32_t fixed_got = fixedOutputEval(start, curve, range, vert, &path);
                const int32_t float1_got =
                    float1OutputEval(start, curve, range, vert, &path);
                const int32_t ref = referenceRangeOutput(start, curve, range, vert);
                crossUpdate(out_fixed_golden, fixed_got, ref);
                crossUpdate(out_float1_golden, float1_got, ref);
                crossUpdate(out_fixed_float1, fixed_got, float1_got);
            }
        }
    }

    if (g_quiet)
        return;

    printf("\n=== Cross-backend (DCO profile grid; semantic, not PASS/FAIL) ===\n");
    printf("comparison                      samples  differing  max_abs_diff\n");
    printf("fixed index vs golden           %8llu  %9llu  %14d\n",
           (unsigned long long)idx_fixed_golden.samples,
           (unsigned long long)idx_fixed_golden.differing, idx_fixed_golden.max_abs_diff);
    printf("float1 index vs golden          %8llu  %9llu  %14d\n",
           (unsigned long long)idx_float1_golden.samples,
           (unsigned long long)idx_float1_golden.differing, idx_float1_golden.max_abs_diff);
    printf("fixed index vs float1 index     %8llu  %9llu  %14d\n",
           (unsigned long long)idx_fixed_float1.samples,
           (unsigned long long)idx_fixed_float1.differing, idx_fixed_float1.max_abs_diff);
    printf("fixed output vs golden          %8llu  %9llu  %14d\n",
           (unsigned long long)out_fixed_golden.samples,
           (unsigned long long)out_fixed_golden.differing, out_fixed_golden.max_abs_diff);
    printf("float1 output vs golden         %8llu  %9llu  %14d\n",
           (unsigned long long)out_float1_golden.samples,
           (unsigned long long)out_float1_golden.differing, out_float1_golden.max_abs_diff);
    printf("fixed output vs float1 output   %8llu  %9llu  %14d\n",
           (unsigned long long)out_fixed_float1.samples,
           (unsigned long long)out_fixed_float1.differing, out_fixed_float1.max_abs_diff);
    printf("(fixed vs float1 output both use Q16 trunc; diffs from int32 vs uint64 multiply in fixed helper)\n");
}

static void runDcoProfile(int array_size, int *max_idx_float1, int *max_out_float1)
{
    const unsigned long dco_phases_ms[] = {1UL, 10UL, 50UL, 100UL, 500UL, 2000UL, 10000UL, 60000UL};

    if (!g_quiet)
    {
        printf("\n=== DCO profile (ARRAY_SIZE=%d, millis, vert 4000/4095) ===\n", array_size);
        printIndexHeader();
    }

    AccuracyStats by_path[4];
    for (unsigned long phase : dco_phases_ms)
        sweepFloat1Index(phase, array_size, by_path, max_idx_float1);

    AccuracyStats out4000;
    AccuracyStats out4095;
    statsReset(out4000);
    statsReset(out4095);

    if (!g_quiet)
    {
        printOutputHeader();
    }
    sweepFloat1Output(4000, &out4000);
    sweepFloat1Output(4095, &out4095);

    if (out4000.max_drift > *max_out_float1)
        *max_out_float1 = out4000.max_drift;
    if (out4095.max_drift > *max_out_float1)
        *max_out_float1 = out4095.max_drift;
}

static void parseArgs(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-q") == 0)
            g_quiet = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: compare [-q]\n");
            printf("  default: full tables, path breakdowns, worst-case coordinates, summary\n");
            printf("  -q  quiet: summary and PASS/FAIL only\n");
            exit(0);
        }
    }
}

int main(int argc, char **argv)
{
    parseArgs(argc, argv);

    const unsigned long micros_ticks[] = {
        1000UL, 100000UL, 2000000UL, 10000000UL, 20000000UL, 60000000UL,
    };
    const unsigned long millis_ticks[] = {
        1UL, 10UL, 50UL, 100UL, 500UL, 2000UL, 10000UL, 60000UL,
    };

    int max_idx_fixed_vs_ref = 0;
    int max_idx_float1_vs_ref = 0;
    AccuracyStats max_out_fixed_vs_ref;
    AccuracyStats max_out_float1_vs_ref;
    statsReset(max_out_fixed_vs_ref);
    statsReset(max_out_float1_vs_ref);

    if (!g_quiet)
    {
        printf("ADSR_Bezier backend comparison (ARRAY_SIZE=%d)\n", ARRAY_SIZE);
        printf("Golden index:  uint64 floor((N-1)*delta/phase)\n");
        printf("Golden output: int64 round-nearest (curve*range + vert/2) / vert\n");
        printf("FLOAT=1 path:  FPU floor index + Q16 amp hybrid (matches ADSR_Bezier.h)\n\n");
        printf("=== Index accuracy ===\n");
        printf("  fixed Q24 (micros-scale ticks):\n");
        printIndexHeader();
    }

    AccuracyStats by_path[4];
    for (unsigned long t : micros_ticks)
        sweepFixedIndex(t, ARRAY_SIZE, TIME_INDEX_MAX_US, by_path, &max_idx_fixed_vs_ref);

    if (!g_quiet)
    {
        printf("\n  optimized FLOAT=1 (micros-scale ticks):\n");
        printIndexHeader();
    }
    for (unsigned long t : micros_ticks)
        sweepFloat1Index(t, ARRAY_SIZE, by_path, &max_idx_float1_vs_ref);

    if (!g_quiet)
    {
        printf("\n  optimized FLOAT=1 (millis-scale ticks):\n");
        printIndexHeader();
    }
    for (unsigned long t : millis_ticks)
        sweepFloat1Index(t, ARRAY_SIZE, by_path, &max_idx_float1_vs_ref);

    if (!g_quiet)
    {
        printf("\n=== Output accuracy ===\n");
        printf("  fixed Q16 trunc:\n");
        printOutputHeader();
    }
    sweepFixedOutput(4000, &max_out_fixed_vs_ref);
    sweepFixedOutput(4095, &max_out_fixed_vs_ref);

    if (!g_quiet)
    {
        printf("\n  optimized FLOAT=1:\n");
        printOutputHeader();
    }
    sweepFloat1Output(4000, &max_out_float1_vs_ref);
    sweepFloat1Output(4095, &max_out_float1_vs_ref);

    runDcoProfile(ARRAY_SIZE, &max_idx_float1_vs_ref, &max_out_float1_vs_ref.max_drift);

    int max_idx_pair = 0;
    int max_out_pair = 0;
    uint64_t out_pair_samples = 0;
    uint64_t out_pair_exact = 0;
    GoldenRaceStats idx_race_micros;
    GoldenRaceStats idx_race_millis;
    GoldenRaceStats out_race;
    goldenRaceReset(idx_race_micros);
    goldenRaceReset(idx_race_millis);
    goldenRaceReset(out_race);
    runFixedVsFloat1Sections(ARRAY_SIZE, &max_idx_pair, &max_out_pair,
                             &idx_race_micros, &idx_race_millis, &out_race,
                             &out_pair_samples, &out_pair_exact);
    runCrossBackendDco(ARRAY_SIZE);

    GoldenRaceStats idx_race_all;
    goldenRaceReset(idx_race_all);
    idx_race_all.samples = idx_race_micros.samples + idx_race_millis.samples;
    idx_race_all.fixed_wins = idx_race_micros.fixed_wins + idx_race_millis.fixed_wins;
    idx_race_all.float1_wins = idx_race_micros.float1_wins + idx_race_millis.float1_wins;
    idx_race_all.ties = idx_race_micros.ties + idx_race_millis.ties;
    idx_race_all.fixed_exact = idx_race_micros.fixed_exact + idx_race_millis.fixed_exact;
    idx_race_all.float1_exact = idx_race_micros.float1_exact + idx_race_millis.float1_exact;
    idx_race_all.fixed_drift_sum = idx_race_micros.fixed_drift_sum + idx_race_millis.fixed_drift_sum;
    idx_race_all.float1_drift_sum = idx_race_micros.float1_drift_sum + idx_race_millis.float1_drift_sum;

    int fail = 0;
    printf("\n=== Summary ===\n");
    printf("  max index diff fixed vs golden:       %d\n", max_idx_fixed_vs_ref);
    printf("  max index diff FLOAT=1 vs golden:     %d\n", max_idx_float1_vs_ref);
    printf("  max output diff fixed vs golden:      %d\n", max_out_fixed_vs_ref.max_drift);
    printf("  max output diff FLOAT=1 vs golden:    %d\n", max_out_float1_vs_ref.max_drift);
    printf("  max index diff fixed vs FLOAT=1:      %d\n", max_idx_pair);
    printf("  max output diff fixed vs FLOAT=1:     %d\n", max_out_pair);
    printf("  index exact%% vs golden: fixed %.1f%%  float1 %.1f%%  (all sweeps)\n",
           goldenRaceExactPct(idx_race_all, true), goldenRaceExactPct(idx_race_all, false));
    printf("  index mean drift vs golden: fixed %.4f  float1 %.4f\n",
           goldenRaceMeanDrift(idx_race_all, true), goldenRaceMeanDrift(idx_race_all, false));
    printf("  index closer to golden: fixed wins %llu  float1 wins %llu  tie %llu\n",
           (unsigned long long)idx_race_all.fixed_wins,
           (unsigned long long)idx_race_all.float1_wins,
           (unsigned long long)idx_race_all.ties);
    printf("  output exact%% vs golden: fixed %.1f%%  float1 %.1f%%\n",
           goldenRaceExactPct(out_race, true), goldenRaceExactPct(out_race, false));
    if (out_pair_samples > 0)
        printf("  output match fixed vs float1: %.1f%% identical (%llu/%llu)\n",
               100.0 * (double)out_pair_exact / (double)out_pair_samples,
               (unsigned long long)out_pair_exact, (unsigned long long)out_pair_samples);
    else
        printf("  output match fixed vs float1: n/a\n");

    if (max_idx_float1_vs_ref > 1 || max_out_float1_vs_ref.max_drift > 1)
    {
        printf("\nFAIL: optimized FLOAT=1 drift exceeds +/-1 vs golden reference.\n");
        fail = 1;
    }
    else
    {
        printf("\nPASS: optimized FLOAT=1 within +/-1 of golden reference.\n");
    }

    if (max_idx_fixed_vs_ref > 0)
        printf("NOTE: fixed Q24 can drift up to %d index step(s) vs golden.\n",
               max_idx_fixed_vs_ref);
    if (max_out_fixed_vs_ref.max_drift > 0)
        printf("NOTE: fixed Q16 can drift up to %d output LSB vs golden.\n",
               max_out_fixed_vs_ref.max_drift);

    return fail ? 1 : 0;
}
