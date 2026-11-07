/* headladder: the head route (an unsorted head before a sorted suffix, the tail route's mirror) against itself switched off
 * (LOGOS_NO_HEAD=1), across head fractions, one thread (2026-10-20). Each array is the tail ladder's array negated and reversed,
 * the order-isomorph that symprobe found 5x slower than its original. Column: median t(LOGOS_NO_HEAD=1) / t(shipped), p10-p90,
 * and the form the head merge took (logos_twist_last(-12): 1 loop, 2 gallop, 3 vector). ABOVE 1: the route is faster.
 * Every output is checked against the route-off output.
 *   ./headladder [pairs] [n] */
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
static double run(const double *base, double *w, size_t n, int off) {
    memcpy(w, base, n * 8); if (off) setenv("LOGOS_NO_HEAD", "1", 1);
    double t0 = now_ms(); logos_sort_f64(w, n, 1); double dt = now_ms() - t0;
    unsetenv("LOGOS_NO_HEAD"); return dt;
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 30;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    double *x = malloc(n * 8), *a = malloc(n * 8), *w = malloc(n * 8), *r = malloc(n * 8), *rt = malloc((size_t)pairs * 8);
    const double fr[] = { 0.5, 0.25, 0.10, 0.05, 0.01, 0.001, 0.0001 };
    printf("# headladder: n = %zu, one thread, %d pairs. Column: median t(LOGOS_NO_HEAD=1) / t(shipped), p10-p90. > 1: the route is faster.\n", n, pairs);
    printf("%-8s %-24s %s\n", "head", "ratio", "form");
    for (size_t q = 0; q < sizeof fr / sizeof fr[0]; q++) {
        size_t hl = (size_t)(fr[q] * (double)n); if (hl < 1) hl = 1; size_t d = n - hl;
        for (size_t i = 0; i < n; i++) x[i] = u01() * 1e6;
        qsort(x, d, 8, cmpd);
        for (size_t i = 0; i < n; i++) a[i] = -x[n - 1 - i];            /* a random head, then a sorted suffix */
        run(a, r, n, 1);
        (void)logos_twist_last(-12); run(a, w, n, 0); int form = (int)logos_twist_last(-12);
        if (memcmp(w, r, n * 8)) { printf("%-8.4f MISMATCH\n", fr[q]); continue; }
        for (int i = 0; i < pairs; i++) {
            double toff = 0, ton = 0;
            for (int k = 0; k < 2; k++) { int offfirst = (i & 1) == 0, dooff = (k == 0) == offfirst; double dt = run(a, w, n, dooff); if (dooff) toff = dt; else ton = dt; }
            rt[i] = toff / ton;
        }
        qsort(rt, (size_t)pairs, 8, cmpd);
        printf("%-8.4f %.3f (%.2f-%.2f)        %d\n", fr[q], rt[pairs / 2], rt[(int)(0.1 * pairs)], rt[(int)(0.9 * pairs)], form); fflush(stdout);
    }
    return 0;
}
