/* The two-strand merge KERNELS in isolation (2026-09-26): the shipped branch-free scalar merge against the AVX2 bitonic
 * merge, on two sorted halves of random doubles, each orientation pair (an up-strand is stored ascending, a down-strand
 * descending and read from its far end), two sizes (cache-resident, and beyond the last-level cache). Paired: the two
 * kernels alternate on the same input, the median of t(scalar) / t(vector) with p10 / p90. Every output is checked:
 * ascending under <, and the same multiset of bit patterns as the input (signed zeros and a few NaNs are planted, so a
 * kernel that loses -0.0 against +0.0, or duplicates a lane, fails here and not in a sort). Built against an engine object
 * compiled -DLOGOS_MERGE_HOOK, which exposes knot_merge2x with the gallop held off. Prediction, written first: 1.5-2.0x on
 * the large size, less on the small one (the scalar chain is latency-bound either way; the vector kernel is shuffle-bound).
 * usage: ./mergebench [pairs] [n]   (n = both strands together) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "logos_psort.h"
void logos_merge2_hook(const double *x, size_t nx, int xr, const double *y, size_t ny, int yr, double *o, int simd);
static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmp_u(const void *a, const void *b) { uint64_t x, y; memcpy(&x, a, 8); memcpy(&y, b, 8); return x < y ? -1 : x > y; }
static void rev(double *a, size_t n) { for (size_t l = 0, r = n ? n - 1 : 0; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }
/* ascending under <, and the same bit patterns as the input (sorted as integers, compared byte for byte) */
static int check(const double *o, size_t n, const uint64_t *ref_bits, uint64_t *tmp, int side) {
    for (size_t i = 1; i < n; i++) if (o[i] < o[i - 1]) {
        fprintf(stderr, "    side %d (%s): descent at %zu: o[%zu]=%.17g o[%zu]=%.17g (o[%zu]=%.17g)\n", side, side ? "vector" : "scalar", i, i - 1, o[i - 1], i, o[i], i + 1 < n ? i + 1 : i, i + 1 < n ? o[i + 1] : o[i]); return 0; }
    memcpy(tmp, o, n * 8); qsort(tmp, n, 8, cmp_u);
    if (memcmp(tmp, ref_bits, n * 8)) { for (size_t i = 0; i < n; i++) if (tmp[i] != ref_bits[i]) { fprintf(stderr, "    side %d (%s): not a permutation: rank %zu has bits %016llx, expected %016llx\n", side, side ? "vector" : "scalar", i, (unsigned long long)tmp[i], (unsigned long long)ref_bits[i]); break; } return 0; }
    return 1;
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 40; if (pairs < 5) pairs = 5;
    size_t nbig = argc > 2 ? strtoull(argv[2], 0, 10) : 8388608;
    const size_t NS[] = { 65536, nbig };
    printf("TWO-STRAND MERGE KERNELS, %d pairs: t(scalar branch-free) / t(AVX2 bitonic), median [p10 p90]; above 1 = the vector kernel is faster.\n"
           "Every output checked (ascending, same bit patterns; -0.0, +0.0 and NaNs planted).\n\n", pairs);
    for (int si = 0; si < 2; si++) {
        size_t n = NS[si], nx = n / 2, ny = n - nx;
        double *x = malloc(nx * 8), *y = malloc(ny * 8), *o = malloc(n * 8), *r = malloc((size_t)pairs * 8);
        uint64_t *bits = malloc(n * 8), *tmp = malloc(n * 8);
        if (!x || !y || !o || !r || !bits || !tmp) return 2;
        printf("  n = %zu (%zu + %zu)\n    %-14s %12s %12s %8s   %s\n", n, nx, ny, "orientation", "scalar ns/el", "vector ns/el", "ratio", "[p10 p90]");
        for (int ori = 0; ori < 4; ori++) {
            int xr = ori & 1, yr = ori >> 1;
            RS = 0x9E3779B97F4A7C15ULL + (uint64_t)ori;
            for (size_t i = 0; i < nx; i++) x[i] = (double)(nr() >> 11) / 9007199254740992.0 * 1e6;
            for (size_t i = 0; i < ny; i++) y[i] = (double)(nr() >> 11) / 9007199254740992.0 * 1e6;
            for (size_t i = 0; i < 16 && i < nx; i++) x[nr() % nx] = (i & 1) ? -0.0 : 0.0;          /* signed zeros, both strands */
            for (size_t i = 0; i < 16 && i < ny; i++) y[nr() % ny] = (i & 1) ? -0.0 : 0.0;
            for (size_t i = 0; i < 8; i++) x[nr() % nx] = y[nr() % ny] = 1234.5;                     /* ties across the strands */
            qsort(x, nx, 8, cmp_d); qsort(y, ny, 8, cmp_d);
            memcpy(bits, x, nx * 8); memcpy(bits + nx, y, ny * 8); qsort(bits, n, 8, cmp_u);
            if (xr) rev(x, nx);
            if (yr) rev(y, ny);
            double ts = 0, tv = 0; int bad = 0;
            for (int p = 0; p < pairs; p++) {
                double t2[2] = { 0, 0 };
                for (int k = 0; k < 2; k++) {
                    int side = (p & 1) ? 1 - k : k;                     /* alternate which kernel goes first */
                    double t0 = ms(); logos_merge2_hook(x, nx, xr, y, ny, yr, o, side); t2[side] = ms() - t0;
                    if (!check(o, n, bits, tmp, side)) bad = 1;
                }
                r[p] = t2[0] / t2[1]; ts += t2[0]; tv += t2[1];
            }
            qsort(r, (size_t)pairs, 8, cmp_d);
            double med = (pairs & 1) ? r[pairs / 2] : 0.5 * (r[pairs / 2 - 1] + r[pairs / 2]);
            printf("    %-14s %12.3f %12.3f %8.3f   [%.3f %.3f]%s\n", ori == 0 ? "up / up" : ori == 1 ? "DOWN / up" : ori == 2 ? "up / DOWN" : "DOWN / DOWN",
                   ts / pairs * 1e6 / (double)n, tv / pairs * 1e6 / (double)n, med, r[(int)(0.10 * pairs)], r[(int)(0.90 * pairs)], bad ? "   WRONG OUTPUT" : "");
            if (bad) { printf("WRONG OUTPUT: stop\n"); return 1; }
        }
        printf("\n");
        free(x); free(y); free(o); free(r); free(bits); free(tmp);
    }
    return 0;
}
