/* thinbench: how far up should the thin front reach? (2026-10-20, edgeprobe: one element past THIN_MAX_N = 128 cost 1.2-4.3x more
 * per element on all sixteen shapes, at one thread and at 14). The engine copy here is built -DLOGOS_THIN_HOOK, so its ceiling can be
 * moved at run time (logos_thin_max_hook); the shipped engine keeps a constant. At each size the same batch of distinct instances is
 * sorted with the ceiling at 128 (the size goes to the dispatcher) and with it above the size (the thin front: prescan, networks, the
 * AVX2 quicksort, insertion or the scalar introsort), paired and alternated. Column: median t(dispatcher) / t(thin front); ABOVE 1:
 * the thin front is faster there. What the dispatcher adds below 4096 is integer counting and the momentum insertion for nearly
 * sorted ranges, so those are the shapes where the thin front can lose.
 * PREDICTION, written first: 1.5-2.5x on unstructured shapes through 2047, sorted-ish shapes 1.0-1.5, few_distinct and int shapes
 * losing from ~512 (counting there is cheaper than a quicksort).
 *   ./thinbench [pairs] [P] [size list] [shape filter] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"
void logos_thin_max_hook(size_t m);

static uint64_t rs;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *SH[] = { "uniform", "heavy_tail", "near_sorted", "swaps3pct", "few_distinct", "quantized", "int_wide", "int_small",
                            "zipf_ids", "block_perm64", "prefix95", "kinks", "sorted_dup", "reversed" };
#define NSH ((int)(sizeof SH / sizeof SH[0]))
static void gen(int s, double *a, size_t n, uint64_t seed) {
    rs = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL + (uint64_t)s;
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - u01() * 0.999999, -1.0 / 1.2); break;
    case 2: case 3: { double f = s == 2 ? 0.01 : 0.03;
        for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
        size_t k = (size_t)(f * (double)n); if (k < 1) k = 1;
        for (size_t q = 0; q < k; q++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break; }
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5); break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; break;
    case 6: for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); break;
    case 7: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % (n / 2 + 1)); break;                  /* integers, span n/2: counting */
    case 8: for (size_t i = 0; i < n; i++) a[i] = floor(pow(1e6, u01() * u01())); break;
    case 9: { size_t B = 64, L = n / B ? n / B : 1; size_t perm[64]; for (size_t b = 0; b < B; b++) perm[b] = b;
              for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
              for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } break; }
    case 10: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = n - n / 20; i < n; i++) a[i] = u01() * (double)n; break;
    case 11: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100 + 1; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break;
    case 12: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 4) + 0.5; break;                   /* sorted with ties */
    case 13: for (size_t i = 0; i < n; i++) a[i] = (double)(n - i) + 0.5; break;                   /* reversed */
    }
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20, P = argc > 2 ? atoi(argv[2]) : 1;
    const char *sl = argc > 3 ? argv[3] : "129,192,256,384,512,768,1024,1536,2047", *sf = argc > 4 ? argv[4] : "";
    size_t sizes[32]; int ns = 0;
    for (const char *p = sl; *p && ns < 32; ) { sizes[ns++] = (size_t)atol(p); while (*p && *p != ',') p++; if (*p == ',') p++; }
    printf("# thinbench: P = %d, %d pairs, batches of distinct instances. Cell: median t(ceiling 128: the dispatcher) / t(ceiling above n:\n"
           "# the thin front). ABOVE 1: the thin front is faster at that size.\n%-14s", P, pairs, "shape");
    for (int k = 0; k < ns; k++) printf(" %7zu", sizes[k]);
    printf("\n");
    for (int s = 0; s < NSH; s++) {
        if (*sf && !strstr(SH[s], sf)) continue;
        printf("%-14s", SH[s]);
        for (int k = 0; k < ns; k++) {
            size_t n = sizes[k], R = (size_t)(1u << 20) / n; if (R < 1) R = 1;
            double *base = malloc(R * n * 8), *w = malloc(R * n * 8), *rt = malloc((size_t)pairs * 8), *ref = malloc(n * 8);
            for (size_t r = 0; r < R; r++) gen(s, base + r * n, n, r + 1);
            int bad = 0;
            for (int v = 0; v < 2 && !bad; v++) { logos_thin_max_hook(v ? 1u << 20 : 128); memcpy(w, base, n * 8); logos_sort_f64(w, n, P);
                memcpy(ref, base, n * 8); qsort(ref, n, 8, cmpd); if (memcmp(w, ref, n * 8)) bad = 1; }
            if (bad) { printf(" %7s", "MISMATCH"); free(base); free(w); free(rt); free(ref); continue; }
            for (int i = 0; i < pairs; i++) {
                double t[2] = { 0, 0 };
                for (int q = 0; q < 2; q++) { int v = ((i & 1) == 0) == (q == 0) ? 0 : 1;
                    memcpy(w, base, R * n * 8); logos_thin_max_hook(v ? 1u << 20 : 128);
                    double t0 = now_ms(); for (size_t r = 0; r < R; r++) logos_sort_f64(w + r * n, n, P); t[v] = now_ms() - t0; }
                rt[i] = t[0] / t[1];
            }
            logos_thin_max_hook(128);
            qsort(rt, (size_t)pairs, 8, cmpd);
            printf(" %7.3f", rt[pairs / 2]); fflush(stdout);
            free(base); free(w); free(rt); free(ref);
        }
        printf("\n");
    }
    return 0;
}
