/* frontprobe.cpp -- what logos's PROFILE PHASE costs in front of the SIMD kernel (2026-10-05).
 *
 * The question ("piggyback off the SIMD sort with the profile phase"): on data where logos finds no structure, the hybrid
 * (-DLOGOS_XSS_LEAF) hands the array to x86-simd-sort whole. Everything logos does before that call is the profile phase:
 * the prescan, the shared sample and its gates, the relocation probe, the leaf's probes, sort_fn's min/max/int/order loop and
 * the kernel's own NaN/-0.0 scan. FINDINGS 3be put "the remaining quarter of the one-thread gap" there without pricing it.
 *
 * Three contenders, all through the SAME compiled library code (logos_xss.o), paired (bench_paired: adjacent halves, order
 * alternating, median of the ratio with p10/p90):
 *   logos      logos_sort_f64, engine built -DLOGOS_XSS_LEAF: the shipped hybrid, profile phase and all
 *   xss+scan   one vectorized NaN/zero scan (av_scan_nan_negzero, copied) + the library: the FLOOR for data nobody vouched
 *              for, since the library needs hasnan from its caller and its min/max networks lose the sign of -0.0
 *   xss        the library alone, hasnan = false: the ceiling, legal only for data PROMISED NaN- and -0.0-free
 * logos / xss+scan is the price of the profile phase beyond the one pass a careful caller must make anyway; xss+scan / xss is
 * the price of that pass, which only folding the scan INTO the kernel's first partition could recover (the library already
 * folds a min/max profile into its partition pass: partition_unrolled(..., &smallest, &biggest)).
 *
 * PREDICTION (written before the first run): one thread, 8.4M -- logos / xss+scan 1.08-1.14 on uniform, heavy_tail, bimodal,
 * clustered (sort_fn's scalar loop is one more full pass, ~0.4 ns/element, plus ~2 ms of random-read probes against a ~79 ms
 * sort); xss+scan / xss 1.03-1.05 (one bandwidth-bound pass). 1M: the same or larger (the probes are fixed costs). Two
 * threads: logos / xss_par+scan 1.04-1.10 (a serial prescan and probes in front of a parallel sort).
 * STOP RULE: if logos / xss+scan reads <= 1.04 on every shape at 8.4M, the profile phase is already at the floor; record it and
 * build nothing. Otherwise the next step is to fold sort_fn's pass and the NaN/zero scan into one, then into the partition.
 *
 * MEASURED (first run, 40 pairs): one thread 8.4M logos / xss+scan 1.117 uniform, 1.045-1.057 on the three skewed shapes;
 * xss+scan / xss 1.04-1.05. 1M the same (1.141 / 1.054-1.058). TWO threads: logos / xss+scan 0.98-1.00, but the scan, serial
 * in front of a parallel sort, 1.072-1.079. The stop rule did not fire.
 *
 * THE CHANGE (LOGOS_NO_FRONTFUSE=1 restores the old passes, ab mode): (A) the skewed branch of leaf_f64 asks the kernel's own
 * scan whether there is a NaN instead of running a scalar loop first; (B) sort_fn settles all-integer / ascending / descending
 * on a 64-element scalar prefix, then takes min, max and the NaN-or-zero flag in one AVX2 pass (sf_profile_avx2), and a clean
 * verdict lets the kernel skip its scan. PREDICTION, written before the A/B: one thread 8.4M, switched / shipped, uniform
 * 1.08-1.12, heavy_tail / bimodal / clustered 1.04-1.06, int_wide 1.00 (the scalar loop, unchanged), zeros_negz ~1.03 (vector
 * min/max, the kernel still scans), heavy_negz ~1.05; the same on the default engine (in-house kernel). 14 threads: the
 * partition's buckets run sort_fn's pass from L2, a few percent of a ~9 ns/element leaf: 1.00-1.04 on the partition shapes.
 *
 * MEASURED (A + B, 120 pairs, 1 thread 8.4M): library build uniform 1.132, heavy_tail 1.064, bimodal 1.062, clustered 1.066,
 * int_wide 1.009, zeros_negz 1.069, heavy_negz 1.055; default engine 1.117 / 1.059 / 1.056 / 1.062 / 0.995 / 1.078; 14 threads
 * (80 pairs, default engine) 1.066 / 1.052 / 1.053 / 1.033 / 0.990 / 1.042 / 1.032 -- above the 14-thread prediction: the
 * partition's buckets each ran the scalar loop. Re-priced: logos / xss+scan 0.994-1.002, logos / xss 1.028-1.046.
 *
 * LEVER C (LOGOS_NO_PARTFUSE=1 restores B's pass + the kernel's scan): the kernel's FIRST PARTITION carries the NaN-or-zero
 * flag (av_qsort_fused), sort_fn leaves its pass to it when the kernel will take the whole range, leaf_f64's skewed branch
 * calls the kernel without scanning. PREDICTION, written before its A/B: one thread 8.4M, switched / shipped, 1.03-1.05 on
 * the four unstructured shapes on the default engine; 1.01-1.04 on the library build (the top partition is now the in-house
 * one); int_wide 1.00; zeros 1.02-1.04 (the scalar pass that tells NaN from zero still runs when flagged). 14 threads: 1.00
 * (the fused path is sequential only). Re-priced logos / xss: 0.99-1.02.
 *
 * MEASURED (C, 120 pairs, 1 thread 8.4M): default engine 1.037 / 1.041 / 1.038 / 1.031, int_wide 1.003, zeros 1.067; library
 * build 1.037 / 1.035 / 1.047 / 1.050; 1M 1.029-1.035; 14 threads 0.995-1.049 (40 pairs, noise: the path is sequential).
 * Re-priced: logos / xss 0.991-1.010, logos / xss+scan 0.933-0.981. The whole package against the engine as it was (make
 * frontledger, layout cancelled): one thread 1.140 / 1.086 / 1.088 / 1.089, int_wide 1.010; 14 threads 1.021-1.055, 0.994.
 *
 * LOW CARDINALITY (2026-10-07, make scoreboard; FP_SWITCH=LOGOS_NO_LCVEC, the distinct_* and zeros_3 rows): the route's map
 * counts up to 8 sample values in vector registers (lc_vec_count). PREDICTION, written before the A/B: one thread, 8.4M,
 * distinct_2 ~2.4x (1.70 -> ~0.7 ns per element), distinct_5 ~2x, distinct_8 ~1.8x; 14 threads 1.1-1.3x (already parallel
 * and near the memory's pace); the unstructured rows 1.00 (they never reach the route).
 *
 * MEASURED (LOGOS_NO_LCVEC, 60 / 40 pairs): one thread distinct_2 1.969, distinct_5 1.637, distinct_8 1.361, zeros_3 1.976;
 * 14 threads 1.731 / 2.353 / 1.836 / 2.315; the unstructured rows 0.971-1.010. Scoreboard after: 5 values at one thread 1.13 ns
 * against x86-simd-sort's 1.41 (was 1.86 against 1.28); 2 values 1.00 against 0.66, still behind.
 * THE EMIT STREAMS (FP_SWITCH=LOGOS_LC_EMIT_PLAIN): non-temporal stores for the counting routes' second pass from 2M elements.
 * PREDICTION, written before the A/B: one thread distinct_2 ~1.3x, distinct_5 / _8 ~1.2x, the lattice rows (distinct_256,
 * quantized) at most 1.07; 14 threads 1.0-1.3.
 *
 * MEASURED (LOGOS_LC_EMIT_PLAIN, 60 pairs): one thread distinct_2 1.361, _5 1.245, _8 1.211, zeros_3 1.313, lattice 1.049-1.053;
 * 14 threads 1.041-1.132 (wide); the unstructured rows 0.996-1.009.
 * THE PRESCAN IN PARALLEL (FP_SWITCH=LOGOS_NO_PARPRESCAN, 14 threads): PREDICTION, written before the A/B: sorted ~1.9x (0.35 ->
 * ~0.18 ns), reversed ~1.25x (the reversal stays one thread), sorted_late ~1.1x, everything else 1.00.
 *
 * MEASURED (LOGOS_NO_PARPRESCAN, 80 pairs, 14 threads): sorted 1.744, reversed 1.219, sorted_late 1.131, every other row 0.99-1.02.
 * THE MODE GRID (FP_SWITCH=LOGOS_NO_MDGRID, 14 threads): PREDICTION, written before the A/B: clustered (16) ~1.45x, clusters_32
 * ~1.4x, clusters_8 ~1.15x, clusters_4 1.0-1.05, bimodal and every other row 1.00.
 *
 * MEASURED (LOGOS_NO_MDGRID, 80 pairs, 14 threads): clustered (16) 1.311, clusters_32 1.239, clusters_8 1.035, clusters_4 0.933
 * (the grid lost to three compares: MD_LIN_MAX is now 8, so 4 and 8 clusters keep the compares), every other row 0.98-1.03.
 *
 * THE WORKER POOL (FP_SWITCH=LOGOS_NO_POOL, 14 threads): PREDICTION, written before the A/B: 1M -- partition shapes 1.10-1.30, lattice /
 * few distinct / near_sorted 1.2-1.5, sorted 1.5-2.0; 8.4M -- 1.02-1.10.
 * MEASURED (frontprobe_inhouse, 80 pairs at 1M, 40 at 8.4M, 60 at 1M / 4 threads): 1M 14T sorted 2.29, sorted_late 2.44, distinct_2-8
 * 1.47-1.73, near_sorted 1.62, block_perm 1.34, distinct_256 / quantized 1.16, the partition shapes 1.06-1.13; 8.4M 0.99-1.37 (the
 * counting rows highest); 1M at 4 threads 1.02-1.79. The partition at the bottom of its range, everything structured above.
 *
 * 2026-10-08, the rest of that round (FINDINGS 3bk), each switch against shipped, 40 pairs:
 *   LOGOS_NO_LATVEC (the lattice map in vectors; predicted 1.2-1.4 at 1T): 1T distinct_256 2.57, quantized 2.38; 14T 1.77 / 1.60;
 *     guards 0.98-1.03.
 *   LOGOS_NO_F32 (float32 narrowing; predicted 1.25-1.45 at 8M): f32_uniform / f32_normal 1.26 / 1.32 at 8M, 1.20-1.26 at 262k-1M,
 *     1.54 / 1.68 at 65,536; f32_sensor 1.0 (the quantized-radix probe takes it first); uniform, int_wide 0.97-1.00.
 *   LOGOS_NO_AVX2_INT32 (the AVX2 kernel for int32 values at 1T, then a runtime default; predicted 1.05-1.12): int_wide 0.83 at 8M,
 *     0.93-0.94 at 65k-1M -- the radix is faster. Reverted; the switch no longer exists.
 *   LOGOS_PART_INPLACE (the in-place partition; here > 1 means the in-place form is SLOWER): 1.25-1.38 on every shape that reaches
 *     the partition at 2M and 8.4M, the rest 0.96-1.05. Opt-in.
 *
 * Build: make frontprobe XSS=../x86-simd-sort/src     usage: ./frontprobe [pairs] [n] [threads] [shape filter] [ab]
 * ab: FP_SWITCH (default LOGOS_NO_FRONTFUSE) set against the engine as built, one binary, the guards included. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <immintrin.h>
extern "C" {
#include "bench_common.h"
void logos_sort_f64(double *a, size_t n, int nthreads);
size_t logos_twist_last(int which);
void logos_xss_qsort_f64(double *a, size_t n);
void logos_xss_qsort_par_f64(double *a, size_t n, int threads);
}

static uint64_t RS = 88172645463325252ULL;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / (double)(1ULL << 53); }
typedef void (*gen_fn)(double *, size_t);
static void g_uniform(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_heavy(double *a, size_t n)     { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); } }
static void g_bimodal(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 1e6; }
static void g_clustered(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 16) * 1e8 + fr() * 1e5; }
static void g_intwide(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); }
static void g_zeros(double *a, size_t n)     { for (size_t i = 0; i < n; i++) { uint64_t r = nr() % 100; a[i] = r == 0 ? 0.0 : r == 1 ? -0.0 : fr() * 2e9 - 1e9; } }
static void g_heavyz(double *a, size_t n)    { g_heavy(a, n); for (size_t i = 0; i < n; i += 97) a[i] = (i & 1) ? -0.0 : 0.0; }
static void g_d2(double *a, size_t n)        { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 2) * 1e6 + 0.5; }
static void g_d5(double *a, size_t n)        { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }
static void g_d8(double *a, size_t n)        { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 8) * 0.25 - 1.0; }
static void g_d3z(double *a, size_t n)       { for (size_t i = 0; i < n; i++) { uint64_t r = nr() % 3; a[i] = r == 0 ? -0.0 : r == 1 ? 0.0 : 1.5; } }
static void g_sorted(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)i * 1.5; }
static void g_rev(double *a, size_t n)       { for (size_t i = 0; i < n; i++) a[i] = (double)(n - i) * 1.5; }
static void g_sorted_late(double *a, size_t n) { g_sorted(a, n); for (size_t k = 0; k < 64; k++) { size_t i = n - 1 - (nr() % (n / 100)); a[i] = fr() * (double)n; } }
static void g_cl4(double *a, size_t n)       { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 4) * 1e8 + fr() * 1e5; }
static void g_cl8(double *a, size_t n)       { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 8) * 1e8 + fr() * 1e5; }
static void g_cl32(double *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 32) * 1e8 + fr() * 1e5; }
static void g_near(double *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; } }
static void g_blocks(double *a, size_t n)    { size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
                                               for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
                                               for (size_t i = 0; i < n; i++) a[i] = (double)(perm[i / blk] * blk + i % blk) + fr() * 0.5; }
static void g_d256(double *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 256) * 1000.0 + 0.5; }
static void g_quant(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; }
/* float-origin data (the float32 path, LOGOS_NO_F32, 2026-10-08): doubles that are all exactly floats */
static void g_f32u(double *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (double)(float)(fr() * 2e6 - 1e6); }
static void g_f32n(double *a, size_t n)      { for (size_t i = 0; i < n; i++) { double s = 0; for (int k = 0; k < 4; k++) s += fr(); a[i] = (double)(float)((s - 2.0) * 50.0); } }
static void g_f32s(double *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (double)(float)(20.0 + 15.0 * fr() + 1e-3 * (double)(nr() % 1000)); }
static void g_int24(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(nr() % 16777216); }
static const struct { const char *name; gen_fn g; int guard; } SH[] = {
    /* few distinct values (the low-cardinality route; LOGOS_NO_LCVEC, 2026-10-07), ab mode only */
    { "distinct_2", g_d2, 1 }, { "distinct_5", g_d5, 1 }, { "distinct_8", g_d8, 1 }, { "zeros_3", g_d3z, 1 },
    { "distinct_256", g_d256, 1 }, { "quantized", g_quant, 1 },   /* the lattice route */
    { "sorted", g_sorted, 1 }, { "reversed", g_rev, 1 }, { "sorted_late", g_sorted_late, 1 },   /* the prescan (LOGOS_NO_PARPRESCAN) */
    { "clusters_4", g_cl4, 1 }, { "clusters_8", g_cl8, 1 }, { "clusters_32", g_cl32, 1 },   /* the mode grid (LOGOS_NO_MDGRID); clustered = 16 */
    { "near_sorted", g_near, 1 }, { "block_perm", g_blocks, 1 },   /* the worker pool touches every route (LOGOS_NO_POOL) */
    { "uniform", g_uniform, 0 }, { "heavy_tail", g_heavy, 0 }, { "bimodal", g_bimodal, 0 }, { "clustered", g_clustered, 0 },
    /* guards, ab mode only: integers keep sort_fn's scalar loop; zeros and -0.0 take the flagged path, which scans as before */
    { "int_wide", g_intwide, 1 }, { "zeros_negz", g_zeros, 1 }, { "heavy_negz", g_heavyz, 1 },
    { "f32_uniform", g_f32u, 1 }, { "f32_normal", g_f32n, 1 }, { "f32_sensor", g_f32s, 1 }, { "int_2e24", g_int24, 1 },   /* 2026-10-08 */
};

/* the engine's av_scan_nan_negzero, copied: one compare per vector flags NaN and every zero together */
__attribute__((target("avx2"))) static int scan_nan_negzero(const double *a, size_t n) {
    __m256d acc = _mm256_setzero_pd(), zero = _mm256_setzero_pd(); size_t i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm256_or_pd(acc, _mm256_cmp_pd(_mm256_loadu_pd(a + i), zero, _CMP_EQ_UQ));
    int flagged = _mm256_movemask_pd(acc) != 0;
    for (; i < n; i++) flagged |= (a[i] != a[i]) | (a[i] == 0.0);
    if (!flagged) return 0;
    int nan = 0, negz = 0;
    for (size_t k = 0; k < n; k++) { nan |= (a[k] != a[k]); negz |= (a[k] == 0.0 && std::signbit(a[k])); }
    return nan | (negz << 1);
}
/* the shapes here hold no NaN and no -0.0 (checked below), so the scan's verdict never changes the call; it is timed */
static void s_logos(double *a, size_t n, int P)   { logos_sort_f64(a, n, P); }
static void s_xscan(double *a, size_t n, int P)   { if (scan_nan_negzero(a, n)) { std::sort(a, a + n); return; }
                                                    if (P > 1) logos_xss_qsort_par_f64(a, n, P); else logos_xss_qsort_f64(a, n); }
static void s_xss(double *a, size_t n, int P)     { if (P > 1) logos_xss_qsort_par_f64(a, n, P); else logos_xss_qsort_f64(a, n); }
/* ab mode: the engine with one per-sort switch set (FP_SWITCH, default LOGOS_NO_FRONTFUSE) against the engine as built */
static const char *g_sw = "LOGOS_NO_FRONTFUSE";
static void s_off(double *a, size_t n, int P)     { setenv(g_sw, "1", 1); logos_sort_f64(a, n, P); unsetenv(g_sw); }

static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }
static bool verify(bench_sort_fn f, const double *base, double *w, size_t n, int P) {
    memcpy(w, base, n * 8); f(w, n, P);
    for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) return false;
    return bitsum(w, n) == bitsum(base, n);
}

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 40;
    size_t n = argc > 2 ? strtoull(argv[2], 0, 10) : 8388608;
    int P = argc > 3 ? atoi(argv[3]) : 1; if (P < 1) P = 1;
    const char *filt = argc > 4 && argv[4][0] ? argv[4] : NULL;
    int ab = argc > 5 && !strcmp(argv[5], "ab");
    if (getenv("FP_SWITCH")) g_sw = getenv("FP_SWITCH");
    double *base = (double *)malloc(n * 8), *w = (double *)malloc(n * 8);
    if (!base || !w) return 1;
    { double t0 = bench_now_(); memcpy(w, base, n * 8); memcpy(base, w, n * 8); (void)t0; }
    std::vector<double> cal(1 << 20); RS = 1; for (auto &x : cal) x = fr();
    double c0 = bench_now_(); std::sort(cal.begin(), cal.end()); double c1 = bench_now_();
    printf("frontprobe: %d pairs, n = %zu, %d thread(s); calibration std::sort-1M %.1f ms\n", pairs, n, P, c1 - c0);
    printf("ratio = median of tA/tB over adjacent pairs (< 1: A faster), [p10, p90]\n");
    if (ab) {
        printf("A/B: %s=1 against the engine as built; ratio = switched / shipped (> 1: the shipped change is faster)\n", g_sw);
        for (const auto &s : SH) {
            if (filt && !strstr(s.name, filt)) continue;
            RS = 88172645463325252ULL; s.g(base, n);
            bool ok = verify(s_off, base, w, n, P) && verify(s_logos, base, w, n, P);
            double p10, p90, r = bench_paired(s_off, s_logos, base, w, n, P, pairs, &p10, &p90);
            printf("  %-11s %6.3f [%5.3f, %5.3f]%s\n", s.name, r, p10, p90, ok ? "" : "   WRONG OUTPUT");
            fflush(stdout);
        }
        free(base); free(w);
        return 0;
    }
    printf("  %-11s %-28s %-28s %-28s %s\n", "shape", "logos / xss+scan", "xss+scan / xss", "logos / xss", "xss calls");
    for (const auto &s : SH) {
        if (filt && !strstr(s.name, filt)) continue;
        if (s.guard) continue;
        RS = 88172645463325252ULL; s.g(base, n);
        int bad = 0; for (size_t i = 0; i < n; i++) if (base[i] != base[i] || (base[i] == 0.0 && std::signbit(base[i]))) bad = 1;
        if (bad) { printf("  %-11s holds a NaN or -0.0: skipped\n", s.name); continue; }
        logos_twist_last(14);
        bool ok = verify(s_logos, base, w, n, P) && verify(s_xscan, base, w, n, P) && verify(s_xss, base, w, n, P);
        size_t calls = logos_twist_last(14);
        double a10, a90, b10, b90, c10, c90;
        double ra = bench_paired(s_logos, s_xscan, base, w, n, P, pairs, &a10, &a90);
        double rb = bench_paired(s_xscan, s_xss, base, w, n, P, pairs, &b10, &b90);
        double rc = bench_paired(s_logos, s_xss, base, w, n, P, pairs, &c10, &c90);
        printf("  %-11s %6.3f [%5.3f, %5.3f]        %6.3f [%5.3f, %5.3f]        %6.3f [%5.3f, %5.3f]        %zu%s\n",
               s.name, ra, a10, a90, rb, b10, b90, rc, c10, c90, calls, ok ? "" : "   WRONG OUTPUT");
        fflush(stdout);
    }
    free(base); free(w);
    return 0;
}
