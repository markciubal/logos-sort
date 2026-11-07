/* tailladder: the sequential tail route's backward merge, three loops, across tail fractions (2026-10-20). A sorted prefix of
 * random values and a random tail spanning the same range; the tail fraction f decides how many prefix elements each tail
 * element moves past (about (1 - f) / f). Paired against the OLD loop (one branch per element, LOGOS_TAIL_BRANCHY=1), one thread:
 *   vector       LOGOS_TAIL_VEC=1e30    (always: both pieces sorted where they lie, the two-chain AVX2 merge into a buffer, copied back)
 *   gallop       LOGOS_TAIL_VEC=0 LOGOS_TAIL_GALLOP=0 (always: an exponential search per tail element and one memmove per block)
 * (a first column, BRANCH-FREE backward merge, measured 0.34-0.66 below 10% tails and 1.12 at a half: removed, see the engine)
 *   shipped      the gate as compiled (TAIL_GALLOP_RATIO)
 * Column: median t(old loop) / t(variant): ABOVE 1 = the variant is faster. Every output is checked against the old loop's.
 *   ./tailladder [pairs] [n] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double run(const double *base, double *w, size_t n, const char *gal) {   /* the sort's time, the copy excluded */
    memcpy(w, base, n * 8);
    /* gal: NULL = the old loop; "" = shipped; "V G" = LOGOS_TAIL_VEC=V, LOGOS_TAIL_GALLOP=G */
    if (!gal) setenv("LOGOS_TAIL_BRANCHY", "1", 1);
    else if (*gal) { char v[32], g[32]; if (sscanf(gal, "%31s %31s", v, g) == 2) { setenv("LOGOS_TAIL_VEC", v, 1); setenv("LOGOS_TAIL_GALLOP", g, 1); } }
    double t0 = now_ms(); logos_sort_f64(w, n, 1); double dt = now_ms() - t0;
    unsetenv("LOGOS_TAIL_BRANCHY"); unsetenv("LOGOS_TAIL_GALLOP"); unsetenv("LOGOS_TAIL_VEC");
    return dt;
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 30;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    double *a = malloc(n * 8), *w = malloc(n * 8), *r = malloc(n * 8), *rt = malloc((size_t)pairs * 8);
    const double fr[] = { 0.5, 0.25, 0.10, 0.05, 0.03, 0.02, 0.01, 0.003, 0.001, 0.0001 };
    const char *VN[] = { "vector", "gallop", "shipped" }, *VG[] = { "1e30 1e30", "0 0", "" };
    printf("# tailladder: n = %zu, one thread, %d pairs. Columns: median t(old branchy loop) / t(variant), p10-p90. > 1: the variant is faster.\n", n, pairs);
    printf("%-8s %-6s", "tail", "gap");
    for (int v = 0; v < 3; v++) printf("  %-22s", VN[v]);
    printf("\n");
    for (size_t q = 0; q < sizeof fr / sizeof fr[0]; q++) {
        size_t tl = (size_t)(fr[q] * (double)n); if (tl < 1) tl = 1; size_t d = n - tl;
        for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6;
        qsort(a, d, 8, cmpd);
        run(a, r, n, NULL);
        printf("%-8.4f %-6.0f", fr[q], (double)d / (double)tl);
        for (int v = 0; v < 3; v++) {
            run(a, w, n, VG[v]);
            if (memcmp(w, r, n * 8)) { printf("  %-22s", "MISMATCH"); continue; }
            for (int i = 0; i < pairs; i++) {
                double tb = 0, tv = 0;
                for (int k = 0; k < 2; k++) { int bfirst = (i & 1) == 0, dob = (k == 0) == bfirst;
                    double dt = run(a, w, n, dob ? NULL : VG[v]); if (dob) tb = dt; else tv = dt; }
                rt[i] = tb / tv;
            }
            qsort(rt, (size_t)pairs, 8, cmpd);
            char cell[64]; snprintf(cell, sizeof cell, "%.3f (%.2f-%.2f)", rt[pairs / 2], rt[(int)(0.1 * pairs)], rt[(int)(0.9 * pairs)]);
            printf("  %-22s", cell); fflush(stdout);
        }
        printf("\n");
    }
    return 0;
}
