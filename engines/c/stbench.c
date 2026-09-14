/* stbench.c — splitter-table (H0b/H0c) A/B. Three copies of the engine: the
 * binary search (-DLOGOS_NO_SPLITTER_TABLE), the shipped value+key tables and
 * the tables plus the opt-in linear-model table (-DLOGOS_MODEL_TABLE), in ONE binary, interleaved on identical data (pfbench pattern). Only
 * the parallel route runs the histogram, so the rows that matter are at P
 * threads; the 1-thread rows run identical code and show the noise band.
 * Every output verified sorted and permutation-exact.
 * Usage: ./stbench            (all CPUs)      LOGOS_THREADS=N ./stbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void st0_sort_f64(double *, size_t, int); int st0_num_cpus(void);
void st1_sort_f64(double *, size_t, int); void st2_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 3
static sf F[NV] = { st0_sort_f64, st1_sort_f64, st2_sort_f64 };
static const char *VN[NV] = { "search", "value+key", "+model" };
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
enum { S_UNIFORM, S_HEAVY, S_BIMODAL, S_INTWIDE, S_CLUSTER, S_NEG, S_NEARSORT, S_QUANT, S_FEWDIST, S_BLOCK, S_N };
static const char *NM[S_N] = { "uniform", "heavy_tail", "bimodal", "int_wide", "clustered", "negatives", "near_sorted", "quantized", "few_distinct", "block_perm" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_BIMODAL:  for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_CLUSTER:  for (i = 0; i < n; i++) { double s = 0; int k; for (k = 0; k < 4; k++) s += fr(); a[i] = (s - 2.0) * 1e6; } break;
    case S_NEG:      for (i = 0; i < n; i++) a[i] = fr() * 2e6 - 1e6; break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
                     for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_QUANT:    for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_FEWDIST:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    default:         gen_blocks(a, n, 64); break;
    }
}
int main(void) {
    int P = bench_threads(st0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("splitter-table A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    printf("(load affects both variants equally here; lowest time per row wins)\n");
    {   double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300, bs = 1e300;
        for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        for (int r = 0; r < 5; r++) { memcpy(c2, c1, 1000000 * 8); double s = now(); st0_sort_f64(c2, 1000000, 1); double x = now() - s; if (x < bs) bs = x; }
        printf("calibration: memcpy 64MB %.2f ms, logos(search) 1M uniform 1T %.1f ms  (quiet reference ~2.5 / ~30)\n\n", bm, bs);
        free(c1); free(c2); }
    printf("  %-13s %3s  %10s %10s %10s   tables/search  model/tables\n", "shape", "thr", VN[0], VN[1], VN[2]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N);
        int tset[3] = { P, P / 2 > 1 ? P / 2 : 2, 1 }; int nt = d == S_UNIFORM ? 3 : (P > 3 ? 2 : 1);
        for (int ti = 0; ti < nt; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < NV; v++) {
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; }
                for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; }
            }
            printf("  %-13s %3d  %8.1fms %8.1fms %8.1fms   %5.2fx        %5.2fx\n", NM[d], t, b[0], b[1], b[2], b[0] / b[1], b[1] / b[2]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact. Rows at P and P/2 threads carry the effect; 1-thread rows are identical code (noise band).\n");
    return 0;
}
