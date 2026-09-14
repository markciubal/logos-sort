/* leafbench.c — leaf-phase A/B (Devroye's bucket-cost model applied to the
 * per-bucket leaf). After H0b the leaf sorts are the largest parallel phase.
 * On a ~100k-element bucket the leaf runs flash sort: one equi-width
 * classification with m = n classes, a scatter, one insertion pass over the
 * whole buffer. Four engine variants in ONE binary, interleaved on identical
 * data (pfbench pattern):
 *   lb0  as shipped            flash, m = n,     coarse above 500k
 *   lb1  FLASH_M_DIV=4         flash, m = n/4    (counters L2-resident; fix-up cost ~unchanged)
 *   lb2  FLASH_M_DIV=16        flash, m = n/16   (fix-up displacement grows ~16x: expected to lose)
 *   lb3  COARSE_MIN_N=16384    coarse for the buckets (two cache-resident levels, tiny insertions)
 * Only the parallel route reaches the leaf on buckets, so rows are at P and P/2.
 * Every output verified sorted and permutation-exact. Usage: ./leafbench  [LOGOS_THREADS=N] */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void lb0_sort_f64(double *, size_t, int); int lb0_num_cpus(void);
void lb1_sort_f64(double *, size_t, int); void lb2_sort_f64(double *, size_t, int); void lb3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
static sf F[4] = { lb0_sort_f64, lb1_sort_f64, lb2_sort_f64, lb3_sort_f64 };
static const char *VN[4] = { "flash m=n", "m=n/4", "m=n/16", "coarse" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
enum { S_UNIFORM, S_HEAVY, S_BIMODAL, S_INTWIDE, S_CLUSTER, S_NEG, S_N };
static const char *NM[S_N] = { "uniform", "heavy_tail", "bimodal", "int_wide", "clustered", "negatives" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_BIMODAL:  for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_CLUSTER:  for (i = 0; i < n; i++) { double s = 0; int k; for (k = 0; k < 4; k++) s += fr(); a[i] = (s - 2.0) * 1e6; } break;
    default:         for (i = 0; i < n; i++) a[i] = fr() * 2e6 - 1e6; break;
    }
}
int main(void) {
    int P = bench_threads(lb0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("leaf-phase A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    printf("(load affects all variants equally here; lowest time per row wins)\n");
    {   double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300, bs = 1e300;
        for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        for (int r = 0; r < 5; r++) { memcpy(c2, c1, 1000000 * 8); double s = now(); lb0_sort_f64(c2, 1000000, 1); double x = now() - s; if (x < bs) bs = x; }
        printf("calibration: memcpy 64MB %.2f ms, logos 1M uniform 1T %.1f ms  (quiet reference ~2.5 / ~30)\n\n", bm, bs);
        free(c1); free(c2); }
    printf("  %-11s %3s  %10s %10s %10s %10s   best\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N);
        int tset[2] = { P, P / 2 > 1 ? P / 2 : 2 };
        for (int ti = 0; ti < (P > 3 ? 2 : 1); ti++) {
            int t = tset[ti]; double b[4] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < 4; v++) {
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; }
                for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; }
            }
            int best = 0; for (int v = 1; v < 4; v++) if (b[v] < b[best]) best = v;
            printf("  %-11s %3d  %8.1fms %8.1fms %8.1fms %8.1fms   %s (%.2fx vs shipped)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
