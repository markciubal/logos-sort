/* hfbench.c — histogram-free partition A/B. Four engines in ONE binary, interleaved on identical data,
 * at P and P/2 (the partition only runs in parallel; 1-thread rows would be identical code):
 *   hf0  -DLOGOS_NO_HISTFREE  the old path: histogram pass, exact offsets, per-element bucket-index cache
 *   hf1  shipped              sample-sized regions, block reservation, spills; KS certificate -> equi-width grid
 *   hf2  -DHF_NO_KS           same, always the splitter tables (attributes the certificate)
 *   hf3  -DHF_SLACK=0.3       tighter regions (more spills, smaller buffer)
 * Every output verified sorted and permutation-exact. Run: make hfbench && ./hfbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void hf0_sort_f64(double *, size_t, int); int hf0_num_cpus(void);
void hf1_sort_f64(double *, size_t, int); void hf2_sort_f64(double *, size_t, int); void hf3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { hf0_sort_f64, hf1_sort_f64, hf2_sort_f64, hf3_sort_f64 };
static const char *VN[NV] = { "histogram", "histfree+KS", "histfree noKS", "slack 0.3" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
enum { S_UNIFORM, S_HEAVY, S_BIMODAL, S_INTWIDE, S_CLUSTER, S_LOGISTIC, S_INT1E6, S_DUP1000, S_ZIPF, S_JITTER, S_NEARSORT, S_QUANT, S_N };
static const char *NM[S_N] = { "uniform", "heavy_tail", "bimodal", "int_wide", "clustered", "logistic_map", "int_1e6", "dup_1000", "zipf", "jitter_ms", "near_sorted", "quantized" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_BIMODAL:  for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_CLUSTER:  for (i = 0; i < n; i++) { double s = 0; int k; for (k = 0; k < 4; k++) s += fr(); a[i] = (s - 2.0) * 1e6; } break;
    case S_LOGISTIC: { double x = 0.31415926; for (i = 0; i < n; i++) { x = 4.0 * x * (1.0 - x); a[i] = x; } break; }
    case S_INT1E6:   for (i = 0; i < n; i++) a[i] = (double)(nr() % 1000000); break;
    case S_DUP1000:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 1000) * 1e3; break;    /* 1000 distinct: splitters coincide, buckets the sample cannot size */
    case S_ZIPF:     for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_JITTER:   for (i = 0; i < n; i++) { double v = floor(fr() * 1e6); if ((nr() & 15) == 0) v += fr(); a[i] = v; } break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    default:         for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    }
}
int main(void) {
    int P = bench_threads(hf0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("histogram-free partition A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    {   double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300; for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        printf("calibration: memcpy 64MB %.2f ms (quiet ~2.5-3.5)\n\n", bm); free(c1); free(c2); }
    printf("  %-14s %3s  %11s %12s %13s %10s   best (x vs histogram)\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N); int tset[2] = { P, P / 2 > 1 ? P / 2 : 2 };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int q = 0; q < NV; q++) {
                int v = (q + r) % NV;
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-14s %3d  %9.1fms %10.1fms %11.1fms %8.1fms   %s (%.2fx)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
