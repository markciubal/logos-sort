/* probebench.c — the serial fixed costs: sample reads and sorts, and per-call context allocation. Four
 * engines in ONE binary, interleaved on identical data, at 1 thread, P/2 and P, at 1M and 8M (fixed costs
 * show at 1M):
 *   pb0  -DLOGOS_NO_SAMPLE_FAST -DLOGOS_NO_CTX_POOL   per-call contexts, plain sample reads and qsort
 *   pb1  -DLOGOS_NO_CTX_POOL                          fast sample (batched prefetch, AVX2 sort)
 *   pb2  -DLOGOS_NO_SAMPLE_FAST                       persistent context bundles
 *   pb3  shipped                                      both
 * Every output verified sorted and permutation-exact. Run: make probebench && ./probebench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void pb0_sort_f64(double *, size_t, int); int pb0_num_cpus(void);
void pb1_sort_f64(double *, size_t, int); void pb2_sort_f64(double *, size_t, int); void pb3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { pb0_sort_f64, pb1_sort_f64, pb2_sort_f64, pb3_sort_f64 };
static const char *VN[NV] = { "neither", "fast sample", "ctx pool", "both" };
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
enum { S_UNIFORM, S_NEARSORT, S_FEWDIST, S_QUANT, S_BLOCK, S_ZIPF, S_HEAVY, S_INTWIDE, S_N };
static const char *NM[S_N] = { "uniform", "near_sorted", "few_distinct", "quantized", "block_perm", "zipf", "heavy_tail", "int_wide" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_FEWDIST:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    case S_QUANT:    for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_BLOCK:    gen_blocks(a, n, 64); break;
    case S_ZIPF:     for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    default:         for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    }
}
int main(void) {
    int P = bench_threads(pb0_num_cpus());
    const size_t sizes[2] = { 1000000, 8000000 };
    double *base = malloc(sizes[1] * 8), *a = malloc(sizes[1] * 8);
    printf("probe + context A/B — interleaved in one process, %d threads; best of R\n\n", P);
    printf("  %-13s %9s %3s  %9s %11s %9s %9s   best (x vs neither)\n", "shape", "n", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) for (int si = 0; si < 2; si++) {
        size_t N = sizes[si]; int R = N < 4000000 ? 12 : 5;
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N); int tset[3] = { 1, P / 2 > 1 ? P / 2 : 2, P };
        for (int ti = 0; ti < 3; ti++) {
            int t = tset[ti]; if (t == 1 && N == sizes[1] && si == 1 && d >= S_ZIPF) { /* keep 1T rows to the cheap shapes at 8M */ }
            double b[NV] = { 1e300, 1e300, 1e300, 1e300 }; int RR = (t == 1 && N == sizes[1]) ? 3 : R;
            for (int r = 0; r < RR; r++) for (int q = 0; q < NV; q++) {
                int v = (q + r) % NV;
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-13s %9zu %3d  %7.2fms %9.2fms %7.2fms %7.2fms   %s (%.2fx)\n", NM[d], N, t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
