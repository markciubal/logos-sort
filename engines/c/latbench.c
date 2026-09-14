/* latbench.c — lattice window margin A/B: the shipped eighth-of-span margin (-DLATTICE_MARGIN_OLD)
 * against a full span each side, two engines in ONE binary, interleaved, 1 thread and P. Counting
 * shapes carry the effect; the rest are guards. Every output verified. Run: make latbench && ./latbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void lt0_sort_f64(double *, size_t, int); int lt0_num_cpus(void);
void lt1_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 2
static sf F[NV] = { lt0_sort_f64, lt1_sort_f64 };
static const char *VN[NV] = { "margin/8", "margin=span" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
enum { S_UNIFORM, S_HEAVY, S_BIMODAL, S_NEARSORT, S_LOGISTIC, S_LORENZ, S_COLLATZ, S_PRIMEGAP, S_WEYL, S_VDC, S_BENFORD, S_BROWN, S_ZIPF, S_JITTER, S_SAWTOOTH, S_N };
static const char *NM[S_N] = { "uniform", "heavy_tail", "bimodal", "near_sorted", "logistic_map", "lorenz_x", "collatz", "prime_gaps", "weyl", "van_der_corput", "benford", "brownian", "zipf", "jitter_ms", "sawtooth_stride" };
static void gen(double *a, size_t n, int d, int P) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_BIMODAL:  for (i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
    case S_NEARSORT: { for (i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; }
                     for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_LOGISTIC: { double x = 0.31415926; for (i = 0; i < n; i++) { x = 4.0 * x * (1.0 - x); a[i] = x; } break; }
    case S_LORENZ:   { double x = 1, y = 1, z = 1, dt = 0.005; for (i = 0; i < n; i++) { double dx = 10 * (y - x), dy = x * (28 - z) - y, dz = x * y - 8.0 / 3 * z; x += dt * dx; y += dt * dy; z += dt * dz; a[i] = x; } break; }
    case S_COLLATZ:  for (i = 0; i < n; i++) { uint64_t v = i + 1; int steps = 0; while (v != 1) { v = (v & 1) ? 3 * v + 1 : v >> 1; steps++; } a[i] = (double)steps; } break;
    case S_PRIMEGAP: { size_t L = (size_t)1 << 27; uint8_t *sv = calloc(L / 8 + 1, 1);
                       size_t last = 2, k = 0;
                       for (size_t p = 3; p < L && k < n; p += 2) {
                           if (sv[p >> 3] & (1 << (p & 7))) continue;
                           a[k++] = (double)(p - last); last = p;
                           if (p * p < L) { for (size_t q = p * p; q < L; q += 2 * p) sv[q >> 3] |= (uint8_t)(1 << (q & 7)); }
                       }
                       for (i = k; i < n; i++) { a[i] = a[i - k]; }
                       free(sv); break; }
    case S_WEYL:     for (i = 0; i < n; i++) { double f = (double)i * 0.6180339887498949; f -= floor(f); a[i] = f * 1e6; } break;
    case S_VDC:      for (i = 0; i < n; i++) { uint64_t r = 0, v = i; for (int b = 0; b < 32; b++) { r = (r << 1) | (v & 1); v >>= 1; } a[i] = (double)r / 4294967296.0 * 1e6; } break;
    case S_BENFORD:  for (i = 0; i < n; i++) a[i] = pow(10.0, fr() * 6.0); break;
    case S_BROWN:    { double x = 0; for (i = 0; i < n; i++) { x += (fr() + fr() + fr() + fr() - 2.0); a[i] = x; } break; }
    case S_ZIPF:     for (i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); break;
    case S_JITTER:   for (i = 0; i < n; i++) { double v = floor(fr() * 1e6); if ((nr() & 15) == 0) v += fr(); a[i] = v; } break;
    default:         { size_t m = (size_t)P * 12 * 32; size_t period = n / m; if (period == 0) period = 1;   /* the engine's stride at P threads */
                       for (i = 0; i < n; i++) { a[i] = (double)(i % period) + fr() * 0.01; }
                       break; }
    }
}
static const int SHAPES[] = { S_COLLATZ, S_ZIPF, S_PRIMEGAP, S_JITTER, S_UNIFORM, S_HEAVY, S_BIMODAL, S_NEARSORT };
int main(void) {
    int P = bench_threads(lt0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("lattice margin A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    {   double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300, bs = 1e300;
        for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        for (int r = 0; r < 5; r++) { memcpy(c2, c1, 1000000 * 8); double s = now(); lt0_sort_f64(c2, 1000000, 1); double x = now() - s; if (x < bs) bs = x; }
        printf("calibration: memcpy 64MB %.2f ms, logos 1M uniform 1T %.1f ms\n\n", bm, bs); free(c1); free(c2); }
    printf("  %-16s %3s  %11s %11s   ratio\n", "shape", "thr", VN[0], VN[1]);
    /* quantized / few_distinct / mod_card shapes as in parbench */
    for (int extra = 0; extra < 3; extra++) {
        const char *nm = extra == 0 ? "quantized" : extra == 1 ? "few_distinct" : "mod_card_30k";
        RS = 7; for (size_t i = 0; i < N; i++) base[i] = extra == 0 ? (double)(nr() % 4096) / 64.0 : extra == 1 ? (double)(nr() % 5) * 1e6 : (double)(nr() % 30000);
        uint64_t before = xsum(base, N); int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) { int t = tset[ti]; double b[NV] = { 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < NV; v++) { memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s)\n", nm); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s)\n", nm); return 1; } }
            printf("  %-16s %3d  %9.1fms %9.1fms   %.2fx\n", nm, t, b[0], b[1], b[0] / b[1]); }
    }
    for (size_t k = 0; k < sizeof SHAPES / sizeof *SHAPES; k++) {
        int d = SHAPES[k]; RS = 7; gen(base, N, d, P); uint64_t before = xsum(base, N); int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) { int t = tset[ti]; double b[NV] = { 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < NV; v++) { memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s)\n", NM[d]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s)\n", NM[d]); return 1; } }
            printf("  %-16s %3d  %9.1fms %9.1fms   %.2fx\n", NM[d], t, b[0], b[1], b[0] / b[1]); }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
