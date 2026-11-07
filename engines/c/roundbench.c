/* roundbench: one per-sort switch paired against the engine as it ships, f64, at a chosen thread count, unstructured shapes
 * and the structured guards that a change in front of the partition must not break (2026-10-19, the "hallucinated" round).
 *
 *   RB_SWITCH=<NAME> [RB_VALUE=<v>] ./roundbench [pairs] [P] [n list] [shape filter]
 *
 * Each pair sorts the same input twice, adjacently, once with the switch set (the feature OFF, or the knob moved) and once
 * without (shipped), alternating which goes first (bench_paired). Printed: the median over pairs of t(switch) / t(shipped),
 * with p10 / p90 -- ABOVE 1 MEANS THE SHIPPED ENGINE (the feature) IS FASTER. RB_SWITCH unset: a null run, the same engine on
 * both sides, which is this box's error floor for these rows. One binary, one engine copy: no code-layout term.
 * Every output is checked against a reference sort of the same input before anything is timed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"
#include "bench_common.h"

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static const char *g_sw, *g_val;
static void sw_on(void) { if (g_sw) setenv(g_sw, g_val ? g_val : "1", 1); }
static void sw_off(void) { if (g_sw) unsetenv(g_sw); }
static void with_switch(double *a, size_t n, int P) { sw_on(); logos_sort_f64(a, n, P); sw_off(); }
static void shipped(double *a, size_t n, int P) { logos_sort_f64(a, n, P); }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

static const char *SHAPES[] = { "uniform", "heavy_tail", "bimodal", "clustered", "int_wide", "normal",
                                "near_sorted", "block_perm64", "concat8", "noisy_ring", "few_distinct", "quantized", "stairs_x20", "prefix95", "saw64", "saw_alias", "streams4", "kinks", "kinks005", "kinks002", "swaps0001", "swaps0005", "swaps001" };
#define NSH ((int)(sizeof SHAPES / sizeof SHAPES[0]))
static void gen(int sh, double *a, size_t n) {
    switch (sh) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01(); break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = exp(10 * u01()) * (u01() < 0.5 ? -1 : 1); break;
    case 2: for (size_t i = 0; i < n; i++) a[i] = (nr() & 1 ? 1000.0 : 0.0) + u01(); break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 64) * 100.0 + u01() * 0.01; break;
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); break;
    case 5: for (size_t i = 0; i < n; i++) { double u = u01() + 1e-300, v = u01(); a[i] = sqrt(-2 * log(u)) * cos(6.283185307179586 * v); } break;
    case 6: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;                      /* 1% random swaps */
            for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 7: { size_t B = 64, L = n / B; size_t *perm = malloc(B * sizeof *perm); for (size_t b = 0; b < B; b++) perm[b] = b;
              for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
              for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } free(perm); break; }
    case 8: { size_t R = 8, L = n / R;                                                  /* eight sorted logs, each spanning the range */
              for (size_t i = 0; i < n; i++) { size_t r = i / L < R ? i / L : R - 1, k = i - r * L; a[i] = (double)k / (double)L * 1e6 + u01() * 0.5 + (double)r * 0.01; } break; }
    case 9: { size_t off = n / 3; for (size_t i = 0; i < n; i++) { size_t j = (i + off) % n; a[i] = (double)j + (u01() - 0.5) * 40.0; } break; }   /* a ring buffer read from the wrong slot, noisy */
    case 10: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5); break;
    case 11: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; break;
    case 12: { size_t B = 20, L = n / B; for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1, k = i - b * L; a[i] = (double)b * (double)L * 0.9 + (double)k + 0.125; } break; }
    case 13: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;                      /* 95% sorted prefix + a random tail */
             for (size_t i = n - n / 20; i < n; i++) a[i] = u01() * (double)n; break;
    case 16: { size_t S = 4;                                                            /* four ascending streams, dealt round robin */
               for (size_t i = 0; i < n; i++) a[i] = (double)(i / S) * 1.0 + (double)(i % S) * 0.37 * (double)(n / S); break; }
    case 17: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;                     /* one strand, 1% adjacent swaps at random spots */
             for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break;
    case 18: case 19: { double f = sh == 18 ? 0.0005 : 0.0002;                          /* one strand, sparse adjacent swaps */
             for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
             for (size_t k = 0; k < (size_t)(f * (double)n); k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break; }
    case 20: case 21: case 22: { double f = sh == 20 ? 0.0001 : sh == 21 ? 0.0005 : 0.001;   /* one strand, sparse RANDOM swaps */
             for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
             for (size_t k = 0; k < (size_t)(f * (double)n); k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break; }
    default: { size_t L = sh == 14 ? n / 64 : (size_t)(0.0062 * (double)n);             /* ascending teeth; saw_alias: a period of twice */
               if (L < 2) L = 2;                                                         /* the sample's lag, so its pairs read ~0.5 */
               for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) + (double)(i / L) * 1e-7; break; }
    }
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 40;
    int P = argc > 2 ? atoi(argv[2]) : 2;
    const char *nl = argc > 3 ? argv[3] : "8388608";
    const char *filt = argc > 4 ? argv[4] : "";
    g_sw = getenv("RB_SWITCH"); g_val = getenv("RB_VALUE");
    printf("# roundbench: switch %s%s%s, P = %d, %d pairs. Column: median t(switch) / t(shipped), p10-p90. > 1: shipped is faster.\n",
           g_sw ? g_sw : "(none: null run)", g_val ? "=" : "", g_val ? g_val : "", P, pairs);
    for (const char *p = nl; *p; ) {
        size_t n = (size_t)atof(p);
        double *base = malloc(n * sizeof(double)), *work = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double));
        if (!base || !work || !ref) { printf("no memory\n"); return 1; }
        for (int sh = 0; sh < NSH; sh++) {
            if (*filt && !strstr(SHAPES[sh], filt)) continue;
            rs = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(sh + 1) * 0xBF58476D1CE4E5B9ULL; gen(sh, base, n);
            memcpy(ref, base, n * sizeof(double)); qsort(ref, n, sizeof(double), cmpd);
            for (int v = 0; v < 2; v++) {                                  /* verify both sides first */
                memcpy(work, base, n * sizeof(double)); if (v) with_switch(work, n, P); else shipped(work, n, P);
                for (size_t i = 0; i < n; i++) if (!(work[i] == ref[i])) { printf("%-13s n=%zu: WRONG OUTPUT (%s) at %zu\n", SHAPES[sh], n, v ? "switch" : "shipped", i); return 1; }
            }
            double p10, p90, med = bench_paired(with_switch, shipped, base, work, n, P, pairs, &p10, &p90);
            printf("%-13s n=%-9zu %6.3f   (%.3f-%.3f)\n", SHAPES[sh], n, med, p10, p90); fflush(stdout);
        }
        free(base); free(work); free(ref);
        const char *c = strchr(p, ','); if (!c) break; p = c + 1;
    }
    return 0;
}
