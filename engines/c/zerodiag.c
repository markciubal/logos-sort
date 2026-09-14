/* zerodiag.c — where does a signed zero change sign? The harness's signed-zero checks fail at n=1000 and pass at 65536
 * and 300001, so some small-n path rebuilds values instead of moving them. For each shape and size this counts -0.0 and
 * +0.0 before and after a 1-thread sort and checks the output ascends. A correct sort is a permutation: both counts equal.
 *
 * Usage: ./zerodiag */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

static void count_zeros(const double *a, size_t n, size_t *neg, size_t *pos) {
    *neg = *pos = 0;
    for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); if (u == 0x8000000000000000ULL) (*neg)++; else if (u == 0) (*pos)++; }
}
static double centered_round(double x, double scale) {            /* round to 1/scale, keeping a -0.0 where x was just below zero */
    double y = x * scale; long k = (long)(y + (y >= 0 ? 0.5 : -0.5)); double v = (double)k / scale;
    if (k == 0 && y < 0) v = -0.0;
    return v;
}
static void gen(int mode, double *a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double g = fr() + fr() + fr() + fr() - 2.0;
        switch (mode) {
        case 0: a[i] = centered_round(6.0 * g, 10.0); break;                                            /* tenths near zero */
        case 1: a[i] = centered_round(30.0 * g, 1.0); break;                                            /* integers near zero */
        case 2: { static const double few[4] = { -0.0, 0.0, 1.0, 2.0 }; a[i] = few[nr() % 4]; break; } /* few distinct */
        case 3: a[i] = (nr() % 100 == 0) ? ((nr() & 1) ? -0.0 : 0.0) : fr() * 2e6 - 1e6; break;       /* random + 1% zeros */
        case 4: { double v = centered_round(30.0 * g, 1.0); a[i] = (v == 0.0) ? 0.0 : v; break; }     /* integers, only +0.0 */
        default: { double v = centered_round(30.0 * g, 1.0); a[i] = (v == 0.0) ? -0.0 : v; break; }  /* integers, only -0.0 */
        }
    }
}
static const char *MODE_NAME[] = { "tenths", "integers", "few {-0,0,1,2}", "random+1% zeros", "ints, only +0", "ints, only -0" };

int main(void) {
    static const size_t SIZES[] = { 100, 500, 1000, 2000, 4095, 4096, 10000, 32768, 65535, 65536, 300001 };
    size_t maxn = SIZES[sizeof SIZES / sizeof *SIZES - 1];
    double *a = malloc(maxn * sizeof(double));
    if (!a) return 1;
    printf("  %-16s %7s  %8s %8s  ->  %8s %8s  %-9s %s\n", "shape", "n", "-0 in", "+0 in", "-0 out", "+0 out", "ascending", "verdict");
    for (int mode = 0; mode < 6; mode++) {
        for (size_t si = 0; si < sizeof SIZES / sizeof *SIZES; si++) {
            size_t n = SIZES[si];
            RS = 4242 + (uint64_t)mode * 97 + si;
            gen(mode, a, n);
            size_t ni, pi, no, po;
            count_zeros(a, n, &ni, &pi);
            logos_sort_f64(a, n, 1);
            count_zeros(a, n, &no, &po);
            int asc = 1; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { asc = 0; break; }
            printf("  %-16s %7zu  %8zu %8zu  ->  %8zu %8zu  %-9s %s\n", MODE_NAME[mode], n, ni, pi, no, po, asc ? "yes" : "NO",
                   (ni == no && pi == po && asc) ? "ok" : "SIGN CHANGED");
        }
    }
    free(a);
    return 0;
}
