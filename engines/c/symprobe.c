/* symprobe: "an order-isomorphism must not change the cost" (2026-10-20, the second invented round).
 *
 * Three transformations that keep the permutation pattern of an array exactly, so that a comparison sort does the same work on
 * both sides and the only thing that changes is how the VALUES look to the engine's gates, certificates and key maps:
 *   negrev   y[i] = -x[n-1-i]   the order pattern unchanged, the distribution mirrored through zero (a right tail becomes a left
 *                               tail, positive data becomes negative, a lattice keeps its step)
 *   x2^20    y[i] = x[i] * 2^20 exact: the same mantissas, every exponent shifted
 *   x2^-30   y[i] = x[i] * 2^-30 exact on these shapes (no value falls into the subnormal range)
 * Paired, alternating, at one thread and at P: median of t(transformed) / t(original). A row away from 1 in both thread counts names
 * an ASYMMETRY: a certificate that sees only one sign, a key map with a preferred binade, a gate that reads magnitudes. Every
 * transformed output is checked against the transformation of the original's sorted output, bit for bit.
 * PREDICTION, written first: x2^20 and x2^-30 read 1.00 everywhere; negrev reads 1.00 on the symmetric shapes and is the one column
 * that can move, on heavy_tail (the key grid in total-order key space) and the lattice shapes.
 *   ./symprobe [pairs] [n] [P list] [shape filter] */
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
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const char *SH[] = { "uniform", "heavy_tail", "lognormal", "pareto08", "bimodal", "clustered", "normal", "int_wide", "quantized",
                            "few_distinct", "zipf_ids", "near_sorted", "block_perm64", "concat8", "stairs_x20", "saw64", "streams4",
                            "prefix95", "kinks", "coarse64k", "groups16", "sine_slope", "exp_noise" };
#define NSH ((int)(sizeof SH / sizeof SH[0]))
static void gen(int s, double *a, size_t n) {
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6 + 1.0; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - u01() * 0.999999, -1.0 / 1.2); break;
    case 2: for (size_t i = 0; i < n; i++) { double u = u01() + 1e-300, v = u01(); a[i] = exp(2.0 * sqrt(-2 * log(u)) * cos(6.283185307179586 * v)); } break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = pow(1.0 - u01() * 0.999999, -1.0 / 0.8); break;
    case 4: for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 2e9 : 1.0) + u01() * 10.0; break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 64 + 1) * 100.0 + u01() * 0.01; break;
    case 6: for (size_t i = 0; i < n; i++) { double u = u01() + 1e-300, v = u01(); a[i] = 50.0 + sqrt(-2 * log(u)) * cos(6.283185307179586 * v); } break;
    case 7: for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)(nr() | 1); break;
    case 8: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000 + 1) * 0.25; break;
    case 9: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5 + 1); break;
    case 10: for (size_t i = 0; i < n; i++) a[i] = floor(pow(1e6, u01() * u01())) + 1.0; break;
    case 11: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
             for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 12: { size_t B = 64, L = n / B; size_t perm[64]; for (size_t b = 0; b < B; b++) perm[b] = b;
               for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
               for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } break; }
    case 13: { size_t R = 8, L = n / R; for (size_t i = 0; i < n; i++) { size_t r = i / L < R ? i / L : R - 1, k = i - r * L; a[i] = (double)k / (double)L * 1e6 + u01() * 0.5 + (double)r * 0.01 + 1.0; } break; }
    case 14: { size_t B = 20, L = n / B; for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1, k = i - b * L; a[i] = (double)b * (double)L * 0.9 + (double)k + 0.125; } break; }
    case 15: { size_t L = n / 64; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) + (double)(i / L) * 1e-7 + 1.0; break; }
    case 16: { size_t S = 4; for (size_t i = 0; i < n; i++) a[i] = (double)(i / S) + (double)(i % S) * 0.37 * (double)(n / S) + 1.0; break; }
    case 17: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = n - n / 20; i < n; i++) a[i] = u01() * (double)n + 0.5; break;
    case 18: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } break;
    case 19: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 65536) * 65536.0 + u01() * 65536.0 + 1.0; break;
    case 20: { double v = 1; for (size_t i = 0; i < n; i++) { if (i % 16 == 0) v = u01() * 1e9 + 1.0; a[i] = v; } break; }
    case 21: { double T = (double)n / 32.0, A = 0.6 * T / 2.0; for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (u01() * 2.0 - 1.0) * 50.0 + 1e6; break; }
    case 22: for (size_t i = 0; i < n; i++) a[i] = exp((double)i * 20.0 / (double)n) * (1.0 + (u01() - 0.5) * 0.002); break;
    }
}
static void xform(int t, const double *x, double *y, size_t n) {
    if (t == 0) for (size_t i = 0; i < n; i++) y[i] = -x[n - 1 - i];
    else if (t == 1) for (size_t i = 0; i < n; i++) y[i] = x[i] * 1048576.0;
    else for (size_t i = 0; i < n; i++) y[i] = x[i] * 0x1p-30;
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    const char *pl = argc > 3 ? argv[3] : "1,14", *filt = argc > 4 ? argv[4] : "";
    int once = argc > 5 && !strcmp(argv[5], "once");   /* one sort each, original and negrev, for a -DLOGOS_TRACE build */
    double *x = malloc(n * 8), *y = malloc(n * 8), *w = malloc(n * 8), *sx = malloc(n * 8), *ry = malloc(n * 8), *rt = malloc((size_t)pairs * 8);
    const char *TN[] = { "negrev", "x2^20", "x2^-30" };
    printf("# symprobe: n = %zu, %d pairs. Cells: median t(transformed) / t(original), p10-p90. 1.00 = the engine cannot tell them apart.\n", n, pairs);
    for (const char *pp = pl; *pp; ) {
        int P = atoi(pp); while (*pp && *pp != ',') pp++; if (*pp == ',') pp++;
        printf("\n## P = %d\n%-14s", P, "shape");
        for (int t = 0; t < 3; t++) printf("  %-22s", TN[t]);
        printf("\n");
        for (int s = 0; s < NSH; s++) {
            if (*filt && !strstr(SH[s], filt)) continue;
            rs = 0x9E3779B97F4A7C15ULL + (uint64_t)s * 0x2545F4914F6CDD1DULL;
            gen(s, x, n);
            if (once) { fprintf(stderr, "\n===== %s, original, P = %d =====\n", SH[s], P); memcpy(w, x, n * 8); logos_sort_f64(w, n, P);
                        xform(0, x, y, n); fprintf(stderr, "\n===== %s, negrev, P = %d =====\n", SH[s], P); logos_sort_f64(y, n, P); continue; }
            memcpy(sx, x, n * 8); logos_sort_f64(sx, n, P);
            printf("%-14s", SH[s]);
            for (int t = 0; t < 3; t++) {
                xform(t, x, y, n);
                memcpy(w, y, n * 8); logos_sort_f64(w, n, P);
                if (t == 0) { for (size_t i = 0; i < n; i++) ry[i] = -sx[n - 1 - i]; } else xform(t, sx, ry, n);
                if (memcmp(w, ry, n * 8)) { printf("  %-22s", "MISMATCH"); continue; }
                for (int i = 0; i < pairs; i++) {
                    double tx = 0, ty = 0;
                    for (int k = 0; k < 2; k++) { int xf = (i & 1) == 0, dox = (k == 0) == xf;
                        memcpy(w, dox ? x : y, n * 8); double t0 = now_ms(); logos_sort_f64(w, n, P); double dt = now_ms() - t0; if (dox) tx = dt; else ty = dt; }
                    rt[i] = ty / tx;
                }
                qsort(rt, (size_t)pairs, 8, cmpd);
                char cell[64]; snprintf(cell, sizeof cell, "%.3f (%.2f-%.2f)", rt[pairs / 2], rt[(int)(0.1 * pairs)], rt[(int)(0.9 * pairs)]);
                printf("  %-22s", cell); fflush(stdout);
            }
            printf("\n");
        }
    }
    return 0;
}
