/* bandbatch: the leaf's AVX2-or-flash window (2048..65536) re-measured on DISTINCT inputs (2026-10-20).
 *
 * bandbench set that window by sorting ONE input repeatedly (400 copies of the same array below 4096, 120 up to 65536): flash's
 * classification and insertion fix-up are data-dependent branches, and a predictor that sees the same input again and again can
 * learn them, which the AVX2 quicksort (branch-free) cannot gain from. edgeprobe, on batches of distinct instances, found the
 * opposite of bandbench at 2048: one element across FLASH_SORT_THRESHOLD made random data 1.3-1.4x dearer per element. Here the
 * same two engine copies as bandbench (bb0 / bb1, one built -DLOGOS_AVX2_OVER_FLASH_FORCE: the AVX2 kernel ahead of flash; one as
 * shipped: flash first inside the window), each measurement a batch of DISTINCT instances (2^21 / n of them), paired and
 * alternated, twelve shapes. Run both builds (make bandbatch) and read the geometric mean of the two orientations.
 * Cell: t(AVX2 first) / t(flash first): ABOVE 1 = flash (shipped) is faster.
 * PREDICTION, written first: on distinct inputs the AVX2 kernel wins the random shapes across the window (0.75-0.9), flash keeps
 * the ordered ones (block_perm, prefix95, streams, kinks: 1.3-3x), so the right gate is structure, not size.
 *   ./bandbatch <new_slot 0|1> [pairs] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
void bb0_sort_f64(double *a, size_t n, int threads);
void bb1_sort_f64(double *a, size_t n, int threads);
static uint64_t rs;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *SH[] = { "uniform", "heavy_tail", "normal", "bimodal", "clustered", "quantized", "near_sorted", "block_perm64",
                            "prefix95", "streams4", "sine_slope", "kinks" };
#define NSH ((int)(sizeof SH / sizeof SH[0]))
static void gen(int s, double *a, size_t n, uint64_t seed) {
    rs = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL + (uint64_t)s;
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e9; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - u01() * 0.999999, -1.0 / 1.2); break;
    case 2: for (size_t i = 0; i < n; i++) { double u = u01() + 1e-300, v = u01(); a[i] = sqrt(-2 * log(u)) * cos(6.283185307179586 * v); } break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + u01() * 10.0; break;
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 64) * 100.0 + u01() * 0.01; break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; break;
    case 6: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 7: { size_t B = 64, L = n / B; size_t perm[64]; for (size_t b = 0; b < B; b++) perm[b] = b;
              for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
              for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } break; }
    case 8: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = n - n / 20; i < n; i++) a[i] = u01() * (double)n; break;
    case 9: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 4) + (double)(i % 4) * 0.37 * (double)(n / 4); break;
    case 10: { double T = (double)n / 32.0, A = 0.6 * T / 2.0; for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (u01() * 2.0 - 1.0) * 50.0; break; }
    case 11: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break;
    }
}
int main(int argc, char **argv) {
    int new_slot = argc > 1 ? atoi(argv[1]) : 0, pairs = argc > 2 ? atoi(argv[2]) : 15;
    const size_t SZ[] = { 2048, 3000, 4096, 6000, 8192, 16384, 32768, 65536 };
    const int NSZ = (int)(sizeof SZ / sizeof SZ[0]);
    printf("# bandbatch (copy bb%d holds flash-first): cell = t(AVX2 first) / t(flash first), distinct instances; > 1: flash is faster\n%-14s", new_slot, "shape");
    for (int k = 0; k < NSZ; k++) printf(" %7zu", SZ[k]);
    printf("\n");
    for (int s = 0; s < NSH; s++) {
        printf("%-14s", SH[s]);
        for (int k = 0; k < NSZ; k++) {
            size_t n = SZ[k], R = (size_t)(1u << 21) / n;
            double *base = malloc(R * n * 8), *w = malloc(R * n * 8), *rt = malloc((size_t)pairs * 8), *ref = malloc(n * 8);
            for (size_t r = 0; r < R; r++) gen(s, base + r * n, n, r + 1);
            int bad = 0;
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmpd);
            for (int v = 0; v < 2 && !bad; v++) { memcpy(w, base, n * 8); if (v) bb1_sort_f64(w, n, 1); else bb0_sort_f64(w, n, 1); if (memcmp(w, ref, n * 8)) bad = 1; }
            if (bad) { printf(" %7s", "MISMATCH"); free(base); free(w); free(rt); free(ref); continue; }
            for (int i = 0; i < pairs; i++) {
                double t[2] = { 0, 0 };
                for (int q = 0; q < 2; q++) { int v = ((i & 1) == 0) == (q == 0) ? 0 : 1;
                    memcpy(w, base, R * n * 8);
                    double t0 = now_ms(); for (size_t r = 0; r < R; r++) { if (v) bb1_sort_f64(w + r * n, n, 1); else bb0_sort_f64(w + r * n, n, 1); } t[v] = now_ms() - t0; }
                rt[i] = t[1 - new_slot] / t[new_slot];               /* t(AVX2 first) / t(flash first) */
            }
            qsort(rt, (size_t)pairs, 8, cmpd);
            printf(" %7.3f", rt[pairs / 2]); fflush(stdout);
            free(base); free(w); free(rt); free(ref);
        }
        printf("\n");
    }
    return 0;
}
