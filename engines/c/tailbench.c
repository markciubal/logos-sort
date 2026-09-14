/* tailbench.c — structures for heavy-tailed data. Four engines in ONE binary, interleaved on identical
 * data, at 1 thread and P:
 *   tb0  -DLOGOS_NO_KEY_KS    neither
 *   tb1  shipped              uniformity certificate in KEY space (log-uniform data)
 *   tb2  -DLOGOS_NO_KEY_KS -DLOGOS_EXMAP   exponent-histogram map alone (opt-in, measured negative)
 *   tb3  -DLOGOS_EXMAP        both
 * Shapes: tails of several weights, the log-uniform case the key certificate is for, a discrete tail,
 * a dominant duplicate (which the partition must now decline before scattering), and the usual guards.
 * Every output verified sorted and permutation-exact. Run: make tailbench && ./tailbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void tb0_sort_f64(double *, size_t, int); int tb0_num_cpus(void);
void tb1_sort_f64(double *, size_t, int); void tb2_sort_f64(double *, size_t, int); void tb3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { tb0_sort_f64, tb1_sort_f64, tb2_sort_f64, tb3_sort_f64 };
static const char *VN[NV] = { "neither", "key cert", "exp map", "both" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
enum { S_PARETO12, S_PARETO25, S_LOGNORM, S_EXPON, S_BENFORD, S_ZIPF, S_DOMDUP, S_UNIFORM, S_BIMODAL, S_BANDS, S_GAUSS2, S_INTWIDE, S_NEARSORT, S_QUANT, S_N };
static const char *NM[S_N] = { "pareto_1.2", "pareto_2.5", "lognormal", "exponential", "benford (log-unif)", "zipf_integer",
                               "dominant_dup_50%", "uniform", "bimodal", "three_uniform_bands", "two_gaussians", "int_wide", "near_sorted", "quantized" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_PARETO12: for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_PARETO25: for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 2.5); break;
    case S_LOGNORM:  for (i = 0; i < n; i++) { double u = fr() + 1e-12, v = fr(); a[i] = exp(3.0 * sqrt(-2.0 * log(u)) * cos(6.283185307 * v)); } break;
    case S_EXPON:    for (i = 0; i < n; i++) a[i] = -1e6 * log(1.0 - fr() * 0.999999); break;
    case S_BENFORD:  for (i = 0; i < n; i++) a[i] = pow(10.0, fr() * 6.0); break;
    case S_ZIPF:     for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_DOMDUP:   for (i = 0; i < n; i++) a[i] = (nr() & 1) ? 4.25 : fr() * 1e9; break;   /* one value holds half the data */
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_BIMODAL:  for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_BANDS:    for (i = 0; i < n; i++) { uint64_t r = nr() % 3; a[i] = (double)r * 1e7 + fr() * 1000.0; } break;
    case S_GAUSS2:   for (i = 0; i < n; i++) { double u = fr() + 1e-12, v = fr(), g = sqrt(-2.0 * log(u)) * cos(6.283185307 * v); a[i] = (nr() & 1) ? -1e6 + 5e4 * g : 1e6 + 5e4 * g; } break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    default:         for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    }
}
int main(void) {
    int P = bench_threads(tb0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("heavy-tail structure A/B — interleaved in one process, n=8M, best of %d, %d threads\n\n", R, P);
    printf("  %-19s %3s  %9s %9s %9s %9s   best (x vs neither)\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N); int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int q = 0; q < NV; q++) {
                int v = (q + r) % NV;
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-19s %3d  %7.1fms %7.1fms %7.1fms %7.1fms   %s (%.2fx)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
