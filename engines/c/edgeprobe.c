/* edgeprobe: "one more element must not cost ten percent more" (2026-10-20, the second invented round).
 *
 * The dispatcher switches routes, leaves and thread counts at fixed sizes (the thin front at 128, flash at 2048, the sample and
 * the run routes at 4096, a dozen gates at 65536, two threads at 200,000 for a 14-thread call, the short-reach route at 2^18,
 * coarse at 500,000, the wave and cuts routes at 2^20, the wide near-sorted gate at 6M, the 64-bit radix's ceiling at 8M). The scale
 * report steps in half-decades and cannot see an edge between two of its points. Here each edge E is crossed by ONE element: the
 * same instances, the second size with one extra element appended, and the cost per element compared,
 *   ratio = (t(E_hi) / E_hi) / (t(E_lo) / E_lo)
 * paired and alternated. Below E = 65536 a measurement is a batch of distinct instances sorted back to back (one is shorter than the
 * clock can resolve). ABOVE 1.10 is a cliff: the thing that turns on at the edge is worse, for that shape, than what it replaced --
 * a gate set from other shapes. Below 0.90 is a gate earning from exactly there. Every output is checked ascending with the same
 * bit patterns.
 * PREDICTION, written first: no cliff above 1.10 anywhere at one thread; at 14 threads the one-thread-to-two-threads edge (200,000)
 * is the place to look, since a parallel call has a fixed cost that one element cannot pay for.
 *   ./edgeprobe [pairs] [P list] [shape filter] [edge filter] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"

static uint64_t rs;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static int cmpb(const void *x, const void *y) { uint64_t a, b; memcpy(&a, x, 8); memcpy(&b, y, 8); return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const char *SH[] = { "uniform", "heavy_tail", "near_sorted", "swaps3pct", "few_distinct", "quantized", "int_wide", "zipf_ids",
                            "block_perm64", "prefix95", "kinks", "streams4", "sine_slope", "coarse64", "groups16", "two_halves" };
#define NSH ((int)(sizeof SH / sizeof SH[0]))
/* one instance of n elements; the generators are defined by position so that the instance of n + 1 extends the one of n */
static void gen(int s, double *a, size_t n, uint64_t seed) {
    rs = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL + (uint64_t)s;
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - u01() * 0.999999, -1.0 / 1.2); break;
    case 2: case 3: { double f = s == 2 ? 0.01 : 0.03;
        for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
        for (size_t k = 0; k < (size_t)(f * (double)n); k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break; }
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5); break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; break;
    case 6: for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); break;
    case 7: for (size_t i = 0; i < n; i++) a[i] = floor(pow(1e6, u01() * u01())); break;
    case 8: { size_t B = 64, L = n / B ? n / B : 1; size_t perm[64]; for (size_t b = 0; b < B; b++) perm[b] = b;
              for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
              for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } break; }
    case 9: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = n - n / 20; i < n; i++) a[i] = u01() * (double)n; break;
    case 10: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break;
    case 11: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 4) + (double)(i % 4) * 0.37 * (double)(n / 4); break;
    case 12: { double T = (double)n / 32.0, A = 0.6 * T / 2.0; for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (u01() * 2.0 - 1.0) * 50.0; break; }
    case 13: { size_t W = n / 64 ? n / 64 : 1; for (size_t i = 0; i < n; i++) a[i] = (double)(i / W) * (double)W + u01() * (double)W; break; }
    case 14: { double v = 0; for (size_t i = 0; i < n; i++) { if (i % 16 == 0) v = u01() * 1e9; a[i] = v; } break; }
    case 15: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; qsort(a, n / 2, 8, cmpd); qsort(a + n / 2, n - n / 2, 8, cmpd); break;
    }
}
static const struct { const char *name; size_t lo; } EDGE[] = {
    { "thin front 128", 128 }, { "flash 2048", 2047 }, { "sample/runs 4096", 4095 }, { "momentum 50000", 50000 },
    { "the 65536 gates", 65535 }, { "AVX2/flash 65536", 65536 }, { "two threads 200000", 199999 }, { "short reach 2^18", 262143 },
    { "coarse 500000", 499999 }, { "wave/cuts 2^20", 1048575 }, { "near-sorted wide 6M", 5999999 }, { "radix64 8M", 8388608 } };
#define NED ((int)(sizeof EDGE / sizeof EDGE[0]))
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    const char *pl = argc > 2 ? argv[2] : "1,14", *sf = argc > 3 ? argv[3] : "", *ef = argc > 4 ? argv[4] : "";
    printf("# edgeprobe: %d pairs. Cell: per-element cost at the edge's upper size over its lower size (one element more), median.\n"
           "# ABOVE 1.10: a cliff. Below 0.90: a gate earning from exactly there.\n", pairs);
    for (const char *pp = pl; *pp; ) {
        int P = atoi(pp); while (*pp && *pp != ',') pp++; if (*pp == ',') pp++;
        printf("\n## P = %d\n%-14s", P, "shape"); for (int e = 0; e < NED; e++) if (!*ef || strstr(EDGE[e].name, ef)) printf(" %8.8s", EDGE[e].name); printf("\n");
        for (int s = 0; s < NSH; s++) {
            if (*sf && !strstr(SH[s], sf)) continue;
            printf("%-14s", SH[s]);
            for (int e = 0; e < NED; e++) {
                if (*ef && !strstr(EDGE[e].name, ef)) continue;
                size_t lo = EDGE[e].lo, hi = lo + 1, R = lo >= 65536 ? 1 : (size_t)(1u << 21) / lo; if (R < 1) R = 1;
                double *base = malloc(R * (hi) * 8), *w = malloc(R * hi * 8), *rt = malloc((size_t)pairs * 8);
                uint64_t *b1 = malloc(hi * 8), *b2 = malloc(hi * 8);
                if (!base || !w || !rt || !b1 || !b2) { printf(" %8s", "nomem"); free(base); free(w); free(rt); free(b1); free(b2); continue; }
                for (size_t r = 0; r < R; r++) gen(s, base + r * hi, hi, r + 1);
                /* the lower size is the first lo elements of each instance, which for these generators is the instance of lo plus
                   one element dropped from the end: identical up to that element */
                int bad = 0;
                for (int k = 0; k < 2 && !bad; k++) { size_t m = k ? hi : lo;
                    memcpy(w, base, m * 8); memcpy(b1, w, m * 8); logos_sort_f64(w, m, P);
                    for (size_t i = 1; i < m; i++) if (w[i] < w[i - 1]) bad = 1;
                    memcpy(b2, w, m * 8); qsort(b1, m, 8, cmpb); qsort(b2, m, 8, cmpb); if (memcmp(b1, b2, m * 8)) bad = 1; }
                if (bad) { printf(" %8s", "MISMATCH"); free(base); free(w); free(rt); free(b1); free(b2); continue; }
                for (int i = 0; i < pairs; i++) {
                    double t[2] = { 0, 0 };
                    for (int k = 0; k < 2; k++) { int which = ((i & 1) == 0) == (k == 0) ? 0 : 1; size_t m = which ? hi : lo;
                        for (size_t r = 0; r < R; r++) memcpy(w + r * hi, base + r * hi, m * 8);
                        double t0 = now_ms(); for (size_t r = 0; r < R; r++) logos_sort_f64(w + r * hi, m, P); t[which] = now_ms() - t0; }
                    rt[i] = (t[1] / (double)hi) / (t[0] / (double)lo);
                }
                qsort(rt, (size_t)pairs, 8, cmpd);
                double med = rt[pairs / 2];
                printf(" %7.3f%s", med, med > 1.10 ? "!" : med < 0.90 ? "<" : " "); fflush(stdout);
                free(base); free(w); free(rt); free(b1); free(b2);
            }
            printf("\n");
        }
    }
    return 0;
}
