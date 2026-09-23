/* logos_extra.c — parity features ported from the JS engine:
 *   argSort (index-carrying stable radix), permutation utilities,
 *   findOutliers, and a multikey-quicksort string sort.
 * Kept in a separate translation unit so the verified numeric engine
 * (logos_psort.c) is untouched. Depends only on the public header + libc. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- order-preserving keys (local copies; trivial) ---- */
static inline uint64_t ex_tkey(double d) { uint64_t u; memcpy(&u, &d, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
static inline uint32_t ex_ikey(int32_t x) { return (uint32_t)x ^ 0x80000000u; }

/* ---- comparison fallback (only on allocation failure) ---- */
static _Thread_local const double *g_farr;
static _Thread_local const int32_t *g_iarr;
static int cmp_arg_f(const void *x, const void *y) {
    uint32_t i = *(const uint32_t *)x, j = *(const uint32_t *)y;
    double a = g_farr[i], b = g_farr[j];
    if (a < b) return -1;
    if (a > b) return 1;
    return i < j ? -1 : 1;   /* stable */
}
static int cmp_arg_i(const void *x, const void *y) {
    uint32_t i = *(const uint32_t *)x, j = *(const uint32_t *)y;
    int32_t a = g_iarr[i], b = g_iarr[j];
    if (a < b) return -1;
    if (a > b) return 1;
    return i < j ? -1 : 1;
}

/* ---- argSort: stable permutation p with a[p[0]] <= a[p[1]] <= ...; a unmodified.
 * Index-carrying LSD radix on the order-preserving key (keys carried alongside
 * indices so no random gather per pass), constant bytes skipped. n < 2^32. ---- */
void logos_argsort_f64(const double *a, size_t n, uint32_t *perm) {
    for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
    if (n < 2) return;
    uint64_t *k0 = (uint64_t *)malloc(n * 8), *k1 = (uint64_t *)malloc(n * 8);
    uint32_t *i1 = (uint32_t *)malloc(n * 4);
    if (!k0 || !k1 || !i1) { free(k0); free(k1); free(i1); g_farr = a; qsort(perm, n, 4, cmp_arg_f); return; }
    uint64_t orv = 0, andv = ~0ULL;
    for (size_t i = 0; i < n; i++) { uint64_t k = ex_tkey(a[i]); k0[i] = k; orv |= k; andv &= k; }
    uint64_t diff = orv ^ andv;
    uint64_t *ks = k0, *kd = k1; uint32_t *is = perm, *id = i1;
    for (int p = 0; p < 8; p++) {
        int sh = p * 8; if (((diff >> sh) & 0xff) == 0) continue;
        size_t c[257]; memset(c, 0, sizeof c);
        for (size_t i = 0; i < n; i++) c[((ks[i] >> sh) & 0xff) + 1]++;
        for (int b = 0; b < 256; b++) c[b + 1] += c[b];
        for (size_t i = 0; i < n; i++) { size_t b = (ks[i] >> sh) & 0xff, pos = c[b]++; kd[pos] = ks[i]; id[pos] = is[i]; }
        { uint64_t *t = ks; ks = kd; kd = t; } { uint32_t *t = is; is = id; id = t; }
    }
    if (is != perm) memcpy(perm, is, n * 4);
    free(k0); free(k1); free(i1);
}
void logos_argsort_i32(const int32_t *a, size_t n, uint32_t *perm) {
    for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
    if (n < 2) return;
    uint32_t *k0 = (uint32_t *)malloc(n * 4), *k1 = (uint32_t *)malloc(n * 4);
    uint32_t *i1 = (uint32_t *)malloc(n * 4);
    if (!k0 || !k1 || !i1) { free(k0); free(k1); free(i1); g_iarr = a; qsort(perm, n, 4, cmp_arg_i); return; }
    uint32_t orv = 0, andv = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { uint32_t k = ex_ikey(a[i]); k0[i] = k; orv |= k; andv &= k; }
    uint32_t diff = orv ^ andv;
    uint32_t *ks = k0, *kd = k1; uint32_t *is = perm, *id = i1;
    for (int p = 0; p < 4; p++) {
        int sh = p * 8; if (((diff >> sh) & 0xff) == 0) continue;
        size_t c[257]; memset(c, 0, sizeof c);
        for (size_t i = 0; i < n; i++) c[((ks[i] >> sh) & 0xff) + 1]++;
        for (int b = 0; b < 256; b++) c[b + 1] += c[b];
        for (size_t i = 0; i < n; i++) { size_t b = (ks[i] >> sh) & 0xff, pos = c[b]++; kd[pos] = ks[i]; id[pos] = is[i]; }
        { uint32_t *t = ks; ks = kd; kd = t; } { uint32_t *t = is; is = id; id = t; }
    }
    if (is != perm) memcpy(perm, is, n * 4);
    free(k0); free(k1); free(i1);
}

/* ---- permutation utilities ---- */
void logos_apply_perm_f64(const double *src, const uint32_t *perm, size_t n, double *out) { for (size_t i = 0; i < n; i++) out[i] = src[perm[i]]; }
void logos_apply_perm_i32(const int32_t *src, const uint32_t *perm, size_t n, int32_t *out) { for (size_t i = 0; i < n; i++) out[i] = src[perm[i]]; }
void logos_inverse_perm(const uint32_t *perm, size_t n, uint32_t *inv) { for (size_t i = 0; i < n; i++) inv[perm[i]] = (uint32_t)i; }

/* ---- findOutliers: elements whose sorted rank differs from their original
 * position by more than `budget` (0 => max(3, floor(sqrt n))). Returns the total
 * count; writes up to `cap` original indices into `out`. Input unmodified. ---- */
size_t logos_find_outliers_f64(const double *a, size_t n, uint32_t *out, size_t cap, size_t budget) {
    if (n == 0) return 0;
    if (budget == 0) { budget = (size_t)sqrt((double)n); if (budget < 3) budget = 3; }
    uint32_t *perm = (uint32_t *)malloc(n * 4), *inv = (uint32_t *)malloc(n * 4);
    if (!perm || !inv) { free(perm); free(inv); return SIZE_MAX; }   /* not 0: that reads as 'no outliers'. SIZE_MAX = could not be computed */
    logos_argsort_f64(a, n, perm);
    logos_inverse_perm(perm, n, inv);
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) { size_t r = inv[i]; size_t d = r > i ? r - i : i - r; if (d > budget) { if (cnt < cap) out[cnt] = (uint32_t)i; cnt++; } }
    free(perm); free(inv);
    return cnt;
}

/* ---- multikey quicksort for C strings (Bentley–Sedgewick). Lexicographic by
 * byte. This is the algorithmic core of the JS string engine; the JS fast paths
 * (per-level LCP skip, packed-prefix radix, galloping run-merge) are NOT ported. ---- */
static inline int ex_chr(const char *s, long d) { return (unsigned char)s[d]; }
static void mkqs(const char **a, long lo, long hi, long d) {
    while (hi - lo > 16) {
        int pv = ex_chr(a[(lo + hi) / 2], d);
        long lt = lo, gt = hi, i = lo;
        while (i <= gt) {
            int c = ex_chr(a[i], d);
            if (c < pv) { const char *t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
            else if (c > pv) { const char *t = a[gt]; a[gt] = a[i]; a[i] = t; gt--; }
            else i++;
        }
        mkqs(a, lo, lt - 1, d);
        mkqs(a, gt + 1, hi, d);
        if (pv == 0) return;                 /* the ==0 region ended here: done */
        lo = lt; hi = gt; d++;               /* recurse the equal region one char deeper */
    }
    for (long i = lo + 1; i <= hi; i++) { const char *t = a[i]; long j = i; while (j > lo && strcmp(a[j - 1], t) > 0) { a[j] = a[j - 1]; j--; } a[j] = t; }
}
void logos_sort_strings(const char **a, size_t n) { if (n > 1) mkqs(a, 0, (long)n - 1, 0); }
