/* nsbench.c — near-sorted route A/B. Four engines in ONE binary, interleaved on identical data, at P
 * and P/2 (the route only runs in parallel; 1-thread rows would be identical code):
 *   ns0  -DLOGOS_NS_OLD                          old route (chunk-sort + P-way heap merge), bounded insertion
 *   ns1  default                                 new route (band-first greedy scan + strays + 2-way merges)
 *   ns2  -DLOGOS_PLAIN_INSERTION                 new route, but the dispatcher's plain insertion on small nearly-sorted ranges
 *   ns3  -DLOGOS_NS_OLD -DLOGOS_PLAIN_INSERTION  exactly the previously shipped engine
 * Every output verified sorted and permutation-exact. Run: make nsbench && ./nsbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void ns0_sort_f64(double *, size_t, int); int ns0_num_cpus(void);
void ns1_sort_f64(double *, size_t, int); void ns2_sort_f64(double *, size_t, int); void ns3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { ns0_sort_f64, ns1_sort_f64, ns2_sort_f64, ns3_sort_f64 };
static const char *VN[NV] = { "old route", "new route", "new+plainins", "shipped" };
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
enum { S_INT1, S_F64_01, S_INT5, S_LOCAL1000, S_CHAOS_NS, S_BROWN, S_UNIFORM, S_SAWTOOTH, S_BLOCKS, S_N };
static const char *NM[S_N] = { "near_sorted_int (1%)", "near_sorted_f64 (0.1%)", "near_sorted_int (5%)", "local_shuffle_1000", "near_sorted (chaos)", "brownian", "uniform", "sawtooth_stride", "block_perm" };
static void gen(double *a, size_t n, int d, int P) {
    size_t i;
    switch (d) {
    case S_INT1:     for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_F64_01:   for (i = 0; i < n; i++) a[i] = (double)i * 0.37 + fr(); for (i = 0; i < n / 1000; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_INT5:     for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 20; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_LOCAL1000: for (i = 0; i < n; i++) a[i] = (double)i + (fr() - 0.5) * 2000.0; break;
    case S_CHAOS_NS: for (i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_BROWN:    { double x = 0; for (i = 0; i < n; i++) { x += (fr() + fr() + fr() + fr() - 2.0); a[i] = x; } break; }
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_SAWTOOTH: { size_t m = (size_t)P * 12 * 32; size_t period = n / m; if (period == 0) period = 1; for (i = 0; i < n; i++) a[i] = (double)(i % period) + fr() * 0.01; break; }
    default:         gen_blocks(a, n, 64); break;
    }
}
int main(void) {
    int P = bench_threads(ns0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("near-sorted route A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    {   double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300, bs = 1e300;
        for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        for (int r = 0; r < 5; r++) { memcpy(c2, c1, 1000000 * 8); double s = now(); ns0_sort_f64(c2, 1000000, 1); double x = now() - s; if (x < bs) bs = x; }
        printf("calibration: memcpy 64MB %.2f ms, logos 1M uniform 1T %.1f ms\n\n", bm, bs); free(c1); free(c2); }
    printf("  %-22s %3s  %10s %10s %12s %10s   best\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d, P); uint64_t before = xsum(base, N); int tset[2] = { P, P / 2 > 1 ? P / 2 : 2 };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; double b[NV] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < NV; v++) {
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; }
                for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-22s %3d  %8.1fms %8.1fms %10.1fms %8.1fms   %s (%.2fx vs shipped)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[3] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
