/* sodobench.c — "sample once, decide once" A/B. Two engines in ONE binary, interleaved on identical
 * data: the old fixed-order dispatcher with a sample per gate (-DLOGOS_NO_SODO) against the shipped one
 * two-stage sorted sample that answers every gate, adds a Chao1 estimate of the true cardinality, and is
 * reused by the partition. The parallel entry only (P and P/2 rows; a 1-thread call is identical code), at 1M
 * and 8M because the probe savings are a fixed cost. Every output verified. Run: make sodobench && ./sodobench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void so0_sort_f64(double *, size_t, int); int so0_num_cpus(void);
void so1_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 2
static sf F[NV] = { so0_sort_f64, so1_sort_f64 };
static const char *VN[NV] = { "per-gate samples", "sample once" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
static void gen_blocks(double *a, size_t n, size_t nb) {
    size_t blk = (n + nb - 1) / nb; size_t *perm = malloc(nb * sizeof(size_t));
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; a[i] = (double)b * (double)blk + (double)(i % blk) + fr() * 0.5; }
    free(perm);
}
enum { S_UNIFORM, S_NEARSORT, S_FEWDIST, S_QUANT, S_MODCARD, S_ZIPF, S_ZIPF2, S_COLLATZ, S_BLOCK, S_HEAVY, S_INTWIDE, S_JITTER, S_SAWTOOTH, S_N };
static const char *NM[S_N] = { "uniform", "near_sorted", "few_distinct", "quantized", "mod_card_30k", "zipf", "zipf_s2 (fools gate)", "collatz", "block_perm", "heavy_tail", "int_wide", "jitter_ms", "sawtooth_stride" };
static void gen(double *a, size_t n, int d, int P) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_FEWDIST:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    case S_QUANT:    for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_MODCARD:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 30000); break;
    case S_ZIPF:     for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_ZIPF2:    for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 2.0)); break;   /* ~40 distinct in a 2048-sample, thousands in the data */
    case S_COLLATZ:  for (i = 0; i < n; i++) { uint64_t v = i + 1; int steps = 0; while (v != 1) { v = (v & 1) ? 3 * v + 1 : v >> 1; steps++; } a[i] = (double)steps; } break;
    case S_BLOCK:    gen_blocks(a, n, 64); break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_JITTER:   for (i = 0; i < n; i++) { double v = floor(fr() * 1e6); if ((nr() & 15) == 0) v += fr(); a[i] = v; } break;
    default:         { size_t m = (size_t)P * 12 * 32; size_t period = n / m; if (period == 0) period = 1; for (i = 0; i < n; i++) { a[i] = (double)(i % period) + fr() * 0.01; } break; }
    }
}
int main(void) {
    int P = bench_threads(so0_num_cpus());
    const size_t sizes[2] = { 1000000, 8000000 };
    double *base = malloc(sizes[1] * 8), *a = malloc(sizes[1] * 8);
    printf("sample-once A/B — interleaved in one process, %d threads; P and P/2 calls; best of R\n\n", P);
    printf("  %-22s %9s %3s  %12s %12s   ratio\n", "shape", "n", "thr", VN[0], VN[1]);
    for (int d = 0; d < S_N; d++) for (int si = 0; si < 2; si++) {
        size_t N = sizes[si]; int R = N < 4000000 ? 12 : 5;
        RS = 7; gen(base, N, d, P); uint64_t before = xsum(base, N); int tset[2] = { P, P / 2 > 1 ? P / 2 : 2 };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int q = 0; q < NV; q++) {
                int v = (q + r) % NV;
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
            }
            printf("  %-22s %9zu %3d  %10.2fms %10.2fms   %.2fx\n", NM[d], N, t, b[0], b[1], b[0] / b[1]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
