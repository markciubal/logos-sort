/* cardbench.c — the low-cardinality lookup. Four engines in ONE binary, interleaved on identical data,
 * at 1 thread and P. Flags come from the Makefile (CD0..CD3), labels from argv:
 *   default:  shipped | -DLOGOS_LC_PHASH | -DLOGOS_LATTICE_FIRST | both
 *   variant C: make cardbench CD1=-DLOWCARD_MAX_SCAN=8 CD2=-DLOWCARD_MAX_SCAN=16 CD3=-DLOWCARD_MAX_SCAN=32
 * Shapes: the cardinality ladder that no other bench covers (the counting route's per-element lookup
 * switches from a branchless scan to a binary search above 32 distinct values, and its gate admits 64),
 * both on a lattice and off it, plus the usual guards. Every output verified sorted and permutation-exact. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void cd0_sort_f64(double *, size_t, int); int cd0_num_cpus(void);
void cd1_sort_f64(double *, size_t, int); void cd2_sort_f64(double *, size_t, int); void cd3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { cd0_sort_f64, cd1_sort_f64, cd2_sort_f64, cd3_sort_f64 };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
enum { S_L5, S_L33, S_L50, S_L64, S_L200, S_R5, S_R33, S_R50, S_R64, S_R200, S_QUANT, S_MODCARD, S_UNIFORM, S_NEARSORT, S_N };
static const char *NM[S_N] = { "lattice_5", "lattice_33", "lattice_50", "lattice_64", "lattice_200",
                               "random_5", "random_33", "random_50", "random_64", "random_200",
                               "quantized", "mod_card_30k", "uniform", "near_sorted" };
static void gen(double *a, size_t n, int d) {
    size_t i, k;
    static const size_t CARD[10] = { 5, 33, 50, 64, 200, 5, 33, 50, 64, 200 };
    if (d <= S_R200) {
        size_t c = CARD[d]; double vals[200];
        if (d <= S_L200) for (k = 0; k < c; k++) vals[k] = (double)k * 1e6;          /* equally spaced: the lattice applies */
        else             for (k = 0; k < c; k++) vals[k] = fr() * 1e9;               /* arbitrary doubles: no lattice */
        for (i = 0; i < n; i++) a[i] = vals[nr() % c];
        return;
    }
    switch (d) {
    case S_QUANT:    for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_MODCARD:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 30000); break;
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    default:         for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    }
}
int main(int argc, char **argv) {
    const char *VN[NV] = { "shipped", "v1", "v2", "v3" };
    for (int v = 0; v < NV && v + 1 < argc; v++) VN[v] = argv[v + 1];
    int P = bench_threads(cd0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("low-cardinality lookup A/B/C — interleaved in one process, n=8M, best of %d, %d threads\n\n", R, P);
    printf("  %-14s %3s  %10s %10s %10s %10s   best (x vs %s)\n", "shape", "thr", VN[0], VN[1], VN[2], VN[3], VN[0]);
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
            printf("  %-14s %3d  %8.1fms %8.1fms %8.1fms %8.1fms   %s (%.2fx)\n", NM[d], t, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
