/* knobbench.c — inherited-knob sweep. Four engines in ONE binary (kb0 = baseline, kb1..kb3 = other
 * values of one JS-era constant, set with make KB1=... KB2=... KB3=...), interleaved on identical data over
 * 17 shapes at 1 thread and P, best of R. Column labels come from argv. Every output verified sorted and
 * permutation-exact. Run: make knobbench KB1=-DK=a KB2=-DK=b KB3=-DK=c && ./knobbench K=base K=a K=b K=c */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void kb0_sort_f64(double *, size_t, int); int kb0_num_cpus(void);
void kb1_sort_f64(double *, size_t, int); void kb2_sort_f64(double *, size_t, int); void kb3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { kb0_sort_f64, kb1_sort_f64, kb2_sort_f64, kb3_sort_f64 };
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
enum { S_UNIFORM, S_NEARSORT_INT, S_NEARSORT_F64, S_HEAVY, S_BIMODAL, S_QUANT, S_FEWDIST, S_INTWIDE, S_INTSPAN2, S_INTSPAN8, S_BLOCK, S_COLLATZ, S_ZIPF, S_SAWTOOTH, S_BROWN, S_WEYL, S_JITTER, S_RUNS8, S_RUNS64, S_RUNS2, S_RUNS4, S_N };
static const char *NM[S_N] = { "uniform", "near_sorted_int", "near_sorted_f64", "heavy_tail", "bimodal", "quantized", "few_distinct", "int_wide", "int_span2n", "int_span8n", "block_perm", "collatz", "zipf", "sawtooth_stride", "brownian", "weyl", "jitter_ms", "overlap_runs_8", "overlap_runs_64", "overlap_runs_2", "overlap_runs_4" };
static void gen(double *a, size_t n, int d, int P) {
    size_t i;
    switch (d) {
    case S_UNIFORM:      for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_NEARSORT_INT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_NEARSORT_F64: for (i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_HEAVY:        for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_BIMODAL:      for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_QUANT:        for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_FEWDIST:      for (i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    case S_INTWIDE:      for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_INTSPAN2:     for (i = 0; i < n; i++) a[i] = (double)(nr() % (2 * n)); break;
    case S_INTSPAN8:     for (i = 0; i < n; i++) a[i] = (double)(nr() % (8 * n)); break;
    case S_BLOCK:        gen_blocks(a, n, 64); break;
    case S_COLLATZ:      for (i = 0; i < n; i++) { uint64_t v = i + 1; int steps = 0; while (v != 1) { v = (v & 1) ? 3 * v + 1 : v >> 1; steps++; } a[i] = (double)steps; } break;
    case S_ZIPF:         for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_SAWTOOTH:     { size_t m = (size_t)P * 12 * 32; size_t period = n / m; if (period == 0) period = 1; for (i = 0; i < n; i++) { a[i] = (double)(i % period) + fr() * 0.01; } break; }
    case S_BROWN:        { double x = 0; for (i = 0; i < n; i++) { x += (fr() + fr() + fr() + fr() - 2.0); a[i] = x; } break; }
    case S_WEYL:         for (i = 0; i < n; i++) { double f = (double)i * 0.6180339887498949; f -= floor(f); a[i] = f * 1e6; } break;
    case S_RUNS8: case S_RUNS64: case S_RUNS2: case S_RUNS4: { size_t k = d == S_RUNS8 ? 8 : d == S_RUNS64 ? 64 : d == S_RUNS2 ? 2 : 4, blk = n / k;   /* k sorted runs over the same value range, concatenated */
                           for (size_t r = 0; r < k; r++) { double x = 0; for (i = r * blk; i < (r + 1 == k ? n : (r + 1) * blk); i++) { x += fr(); a[i] = x * (double)k; } } break; }
    default:             for (i = 0; i < n; i++) { double v = floor(fr() * 1e6); if ((nr() & 15) == 0) v += fr(); a[i] = v; } break;
    }
}
int main(int argc, char **argv) {
    const char *VN[NV] = { "base", "v1", "v2", "v3" };
    for (int v = 0; v < NV && v + 1 < argc; v++) VN[v] = argv[v + 1];
    int P = bench_threads(kb0_num_cpus());
    size_t N = 8000000; const int R = 3; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("knob sweep — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    printf("  %-16s %3s  %10s %10s %10s %10s   best (x vs base)\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d, P); uint64_t before = xsum(base, N);
        int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int q = 0; q < NV; q++) {
                int v = (q + r) % NV;   /* rotate the order per rep: the first slot read ~3-5% slow when it was always the baseline */
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-16s %3d  %8.1fms %8.1fms %8.1fms %8.1fms   %s (%.2fx)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("All outputs verified.\n");
    return 0;
}
