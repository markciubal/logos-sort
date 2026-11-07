/* rleprobe: what runs of EQUAL values are worth (2026-10-20). orderprobe found groups16 -- runs of 16 equal values, the runs in
 * random order, n/16 distinct values -- sorting 1.20x SLOWER than its own values shuffled at one thread, and nothing in the engine
 * compresses runs: the counting routes ignore order and stop near 3300 distinct values. Such arrays are common: re-sorting a
 * table by a column that is constant inside the groups of its previous order, held sensor readings, run-length data expanded.
 *
 * CEILING, told the structure: compress the array into (run value, run length), argsort the run values (logos_argsort_f64), write
 * each run back out in that order. Paired against logos_sort_f64 on the same input, one thread. The L = 1 row prices the teller on
 * data with no runs (it must lose there: the route's gate has to keep it away). PREDICTION, written first: L = 16 3-5x, L = 4
 * 1.8-2.5x, L = 2 1.0-1.3x, L = 1 0.6-0.8; random run lengths (mean 6) 2-3x. Above 1 = the told sort is faster.
 *   ./rleprobe [pairs] [n] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"

static uint64_t rs = 0x2545F4914F6CDD1DULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int g_kv, g_ab;
static double *g_v; static uint32_t *g_c, *g_perm;
static void told(double *a, size_t n) {
    size_t m = 0; uint64_t prev = 0; int have = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8);
        if (have && b == prev && g_c[m - 1] != UINT32_MAX) g_c[m - 1]++; else { g_v[m] = a[i]; g_c[m] = 1; m++; prev = b; have = 1; } }
    logos_argsort_f64(g_v, m, g_perm);
    size_t w = 0;
    for (size_t r = 0; r < m; r++) { uint32_t k = g_perm[r]; double v = g_v[k]; for (uint32_t c = g_c[k]; c; c--) a[w++] = v; }
}
/* the route's own kernel: (total-order key, run length) pairs, MSD byte radix with constant-byte skip (str_msd's shape) */
static inline uint64_t tkey(double d) { uint64_t u; memcpy(&u, &d, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
static inline double untkey(uint64_t k) { uint64_t u = (k >> 63) ? (k & 0x7FFFFFFFFFFFFFFFULL) : ~k; double d; memcpy(&d, &u, 8); return d; }
static void kv_msd(uint64_t *k, uint32_t *v, uint64_t *k2, uint32_t *v2, size_t n, int out2) {
    if (n <= 48) {
        for (size_t i = 1; i < n; i++) { uint64_t x = k[i]; uint32_t p = v[i]; size_t j = i; while (j > 0 && k[j - 1] > x) { k[j] = k[j - 1]; v[j] = v[j - 1]; j--; } k[j] = x; v[j] = p; }
        if (out2) { memcpy(k2, k, n * 8); memcpy(v2, v, n * 4); }
        return;
    }
    uint64_t o = 0, an = ~0ULL;
    for (size_t i = 0; i < n; i++) { o |= k[i]; an &= k[i]; }
    uint64_t diff = o ^ an;
    if (!diff) { if (out2) { memcpy(k2, k, n * 8); memcpy(v2, v, n * 4); } return; }
    int sh = (63 - __builtin_clzll(diff)) & ~7;
    size_t c[257]; memset(c, 0, sizeof c);
    for (size_t i = 0; i < n; i++) c[((k[i] >> sh) & 0xff) + 1]++;
    for (int b = 0; b < 256; b++) c[b + 1] += c[b];
    size_t st[257]; memcpy(st, c, sizeof st);
    for (size_t i = 0; i < n; i++) { size_t pos = c[(k[i] >> sh) & 0xff]++; k2[pos] = k[i]; v2[pos] = v[i]; }
    for (int b = 0; b < 256; b++) { size_t s = st[b], m = st[b + 1] - s; if (m) kv_msd(k2 + s, v2 + s, k + s, v + s, m, !out2); }
}
static uint64_t *g_k, *g_k2; static uint32_t *g_c2;
static void told2(double *a, size_t n) {
    size_t m = 0; uint64_t prev = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8);
        if (m && b == prev && g_c[m - 1] != UINT32_MAX) g_c[m - 1]++; else { g_k[m] = tkey(a[i]); g_c[m] = 1; m++; prev = b; } }
    kv_msd(g_k, g_c, g_k2, g_c2, m, 0);
    size_t w = 0;
    for (size_t r = 0; r < m; r++) { double v = untkey(g_k[r]); for (uint32_t c = g_c[r]; c; c--) a[w++] = v; }
}
static void gen(int L, int geo, double *a, size_t n) {
    size_t i = 0;
    while (i < n) { size_t len = geo ? 1 + (size_t)(-log(u01() + 1e-12) * (double)(L - 1)) : (size_t)L; double v = u01() * 1e9;
                    for (size_t k = 0; k < len && i < n; k++) a[i++] = v; }
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    g_kv = argc > 3 && !strcmp(argv[3], "kv");
    g_ab = argc > 3 && !strcmp(argv[3], "ab");   /* the route itself: LOGOS_NO_RLE=1 against shipped (> 1: shipped faster) */
    double *a = malloc(n * 8), *w = malloc(n * 8), *r = malloc(n * 8), *rt = malloc((size_t)pairs * 8);
    g_v = malloc(n * 8); g_c = malloc(n * 4); g_perm = malloc(n * 4); g_k = malloc(n * 8); g_k2 = malloc(n * 8); g_c2 = malloc(n * 4);
    if (g_ab) printf("# rleprobe ab: n = %zu, one thread, %d pairs. Column: median t(LOGOS_NO_RLE=1) / t(shipped). > 1: the route is faster.\n", n, pairs);
    else printf("# rleprobe: n = %zu, one thread, %d pairs. Column: median t(logos_sort_f64) / t(told: compress + %s + expand). > 1: told faster.\n", n, pairs, g_kv ? "KV MSD radix" : "argsort");
    printf("%-14s %8s  %15s  %10s %10s\n", "runs", "ratio", "p10-p90", "logos ns/e", "told ns/e");
    struct { int L, geo; const char *name; } rows[] = { {1, 0, "L=1 (no runs)"}, {2, 0, "L=2"}, {3, 0, "L=3"}, {4, 0, "L=4"}, {8, 0, "L=8"}, {16, 0, "L=16"},
                                                        {64, 0, "L=64"}, {6, 1, "geometric 6"}, {3, 1, "geometric 3"}, {4, 1, "geometric 4"}, {12, 1, "geometric 12"} };
    for (size_t q = 0; q < sizeof rows / sizeof rows[0]; q++) {
        gen(rows[q].L, rows[q].geo, a, n);
        memcpy(w, a, n * 8); setenv("LOGOS_NO_RLE", "1", 1); logos_sort_f64(w, n, 1); unsetenv("LOGOS_NO_RLE"); memcpy(r, a, n * 8); (void)logos_twist_last(28); if (g_ab) logos_sort_f64(r, n, 1); else if (g_kv) told2(r, n); else told(r, n);
        size_t fired = logos_twist_last(28);
        if (memcmp(w, r, n * 8)) { printf("%-14s MISMATCH\n", rows[q].name); continue; }
        double sl = 0, st = 0;
        for (int i = 0; i < pairs; i++) {
            double tl = 0, tt = 0;
            for (int k = 0; k < 2; k++) { int first_l = (i & 1) == 0, do_l = (k == 0) == first_l;
                memcpy(w, a, n * 8); if (g_ab && do_l) setenv("LOGOS_NO_RLE", "1", 1); double t0 = now_ms(); if (do_l || g_ab) logos_sort_f64(w, n, 1); else if (g_kv) told2(w, n); else told(w, n); double dt = now_ms() - t0; unsetenv("LOGOS_NO_RLE");
                if (do_l) tl = dt; else tt = dt; }
            rt[i] = tl / tt; sl += tl; st += tt;
        }
        qsort(rt, (size_t)pairs, 8, cmpd);
        printf("%-14s %8.3f  %6.3f - %6.3f  %10.2f %10.2f%s\n", rows[q].name, rt[pairs / 2], rt[(int)(0.1 * pairs)], rt[(int)(0.9 * pairs)], sl / pairs * 1e6 / n, st / pairs * 1e6 / n, g_ab ? (fired ? "  (route fired)" : "  (route declined)") : "");
        fflush(stdout);
    }
    return 0;
}
