/* kgbench.c — the key grid (mode 10 of the histogram-free partition), measured.
 *
 * heavy_tail at 14 threads spent 57 CPU ns per element in the scatter against uniform's 28 (htprof, 2026-09-13): uniform
 * passes a certificate and buckets with one multiply, a heavy tail passes none and pays the key splitter table's root load,
 * child descent and compares. The key grid gives such data a certified-grade lookup: equal-width cells in key space (a
 * roughly logarithmic scale), bucket edges snapped to the cells of the sample's quantiles, one byte load per element. It is
 * kept only when the balance check passes on the sample. One binary, switched at runtime by LOGOS_NO_KGRID; every row is
 * verified ascending with an identical bitwise multiset before it is timed under the paired harness.
 *
 * PREDICTIONS, written before this ran (ratio = grid off / grid on, > 1 means the grid is faster):
 *   heavy_tail 14T 1.2-1.35 (scatter CPU 57 ns/elem toward uniform's 28); lognormal 1.1-1.3; exponential 1.1-1.25;
 *   pareto_0.8 1.1-1.3, or 1.00 if the balance check refuses it;
 *   bimodal and mixed_sign 1.00: their key range spans both signs, the grid is too coarse, the check refuses it;
 *   clustered and int_wide 0.95-1.2 (uncertain); uniform exactly 1.00 (its certificate comes first);
 *   every 1-thread row exactly 1.00 (the partition only runs in parallel sorts).
 *
 * Usage: ./kgbench [pairs] [n]                                        LOGOS_THREADS=N overrides. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static double gauss(void) { double u = 1.0 - fr(), v = fr(); return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v); }
static int cmp_u64(const void *x, const void *y) { uint64_t a, b; memcpy(&a, x, 8); memcpy(&b, y, 8); return (a > b) - (a < b); }

typedef void (*gen_fn)(double *, size_t);
static void g_heavy(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
static void g_par08(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = pow(1.0 - fr() * 0.999999, -1.0 / 0.8); }
static void g_lognorm(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = exp(2.0 * gauss()); }
static void g_expo(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = -log(1.0 - fr()) * 1e3; }
static void g_bimodal(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 1e6; }
static void g_mixed(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1.0 : -1.0) * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
static void g_clust(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 16) * 1e8 + fr() * 1e5; }
static void g_intwide(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }

static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "heavy_tail",  g_heavy,   "Pareto 1.2 x 1000, the scale report's" },
    { "pareto_0.8",  g_par08,   "heavier tail" },
    { "lognormal",   g_lognorm, "sigma 2" },
    { "exponential", g_expo,    "mean 1000" },
    { "bimodal",     g_bimodal, "guard: key range spans both signs" },
    { "mixed_sign",  g_mixed,   "guard: heavy tails on both signs" },
    { "clustered",   g_clust,   "16 clusters" },
    { "int_wide",    g_intwide, "full int32 range" },
    { "uniform",     g_uniform, "guard: its certificate comes first" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static void sort_off(double *a, size_t n, int t) { setenv("LOGOS_NO_KGRID", "1", 1); logos_sort_f64(a, n, t); }
static void sort_on(double *a, size_t n, int t)  { unsetenv("LOGOS_NO_KGRID");      logos_sort_f64(a, n, t); }

static int verify(const double *w, const uint64_t *bits_sorted, size_t n, uint64_t *tmp) {
    for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) return 0;
    memcpy(tmp, w, n * 8); qsort(tmp, n, 8, cmp_u64);
    return memcmp(tmp, bits_sorted, n * 8) == 0;
}

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8);
    uint64_t *bits = malloc(n * 8), *tmp = malloc(n * 8);
    if (!base || !w || !bits || !tmp) return 1;

    printf("key grid: off vs on, n=%zu, %d pairs, threads 1 and %d, one binary (LOGOS_NO_KGRID)\n", n, pairs, P);
    printf("ratio > 1.000 means the grid is that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-12s %2s %9s %9s %9s  %-38s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NSH; k++) {
        RS = 31 + (uint64_t)k; SH[k].g(base, n);
        memcpy(bits, base, n * 8); qsort(bits, n, 8, cmp_u64);
        int ok = 1;
        memcpy(w, base, n * 8); sort_on(w, n, P);  ok &= verify(w, bits, n, tmp);
        memcpy(w, base, n * 8); sort_off(w, n, P); ok &= verify(w, bits, n, tmp);
        memcpy(w, base, n * 8); sort_on(w, n, 1);  ok &= verify(w, bits, n, tmp);
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_off, sort_on, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-12s %2d %9.3f %9.3f %9.3f  %-38s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    unsetenv("LOGOS_NO_KGRID");
    free(base); free(w); free(bits); free(tmp);
    return 0;
}
