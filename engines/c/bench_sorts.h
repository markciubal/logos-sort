/* bench_sorts.h — two textbook single-threaded baselines for the benchmarks, header-only C
 * (included by the C++ benches too). Neither is tuned beyond its standard form; they exist so the
 * charts show where a plain radix sort and a plain adaptive merge sort sit against the engine.
 *
 *   radix_sort_f64  LSD radix over the IEEE-754 order-preserving key: 11-bit digits, six passes
 *                   (66 bits >= 64), all six histograms taken in one pass, a pass is skipped when
 *                   every key shares that digit. Exact for every non-NaN double; O(n) memory.
 *   timsort_f64     Tim Peters' listsort as in CPython: minrun 32..64, natural runs (strictly
 *                   descending runs reversed), binary insertion to extend short runs, the corrected
 *                   run-stack invariant (Python 3.5+), galloping merges with adaptive min_gallop.
 *                   Stable. O(n/2) temp memory in the worst case. */
#ifndef LOGOS_BENCH_SORTS_H
#define LOGOS_BENCH_SORTS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- LSD radix ---------------- */
static inline uint64_t bs_key(double v) { uint64_t u; memcpy(&u, &v, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
static inline double bs_unkey(uint64_t k) { uint64_t u = (k >> 63) ? (k & 0x7FFFFFFFFFFFFFFFULL) : ~k; double v; memcpy(&v, &u, 8); return v; }
static void radix_sort_f64(double *a, size_t n) {
    if (n < 2) return;
    enum { W = 11, D = 6, NC = 1 << W, MASK = NC - 1 };
    uint64_t *src = (uint64_t *)malloc(n * sizeof(uint64_t)), *dst = (uint64_t *)malloc(n * sizeof(uint64_t));
    uint32_t *cnt = (uint32_t *)calloc((size_t)D * NC, sizeof(uint32_t));
    if (!src || !dst || !cnt) { free(src); free(dst); free(cnt); return; }
    for (size_t i = 0; i < n; i++) {
        uint64_t k = bs_key(a[i]); src[i] = k;
        for (int d = 0; d < D; d++) cnt[(size_t)d * NC + ((k >> (d * W)) & MASK)]++;
    }
    for (int d = 0; d < D; d++) {
        uint32_t *c = cnt + (size_t)d * NC; int sh = d * W;
        int constant = 0; for (int b = 0; b < NC; b++) if (c[b] == n) { constant = 1; break; }
        if (constant) continue;                                    /* every key shares this digit: no reordering possible */
        uint32_t sum = 0; for (int b = 0; b < NC; b++) { uint32_t t = c[b]; c[b] = sum; sum += t; }   /* exclusive prefix = start offsets */
        for (size_t i = 0; i < n; i++) { uint64_t k = src[i]; dst[c[(k >> sh) & MASK]++] = k; }
        uint64_t *t = src; src = dst; dst = t;
    }
    for (size_t i = 0; i < n; i++) a[i] = bs_unkey(src[i]);
    free(src); free(dst); free(cnt);
}

/* ---------------- timsort ---------------- */
#define TS_MIN_MERGE 32
#define TS_MIN_GALLOP 7
typedef struct { double *a; size_t n; double *tmp; size_t tmp_cap; int min_gallop; struct { size_t base, len; } run[128]; int nrun; } ts_state;

static size_t ts_minrun(size_t n) { size_t r = 0; while (n >= TS_MIN_MERGE) { r |= n & 1; n >>= 1; } return n + r; }
static void ts_reverse(double *a, size_t lo, size_t hi) { while (lo + 1 < hi) { double t = a[lo]; a[lo] = a[hi - 1]; a[hi - 1] = t; lo++; hi--; } }
/* length of the run starting at lo (ascending, or strictly descending which is reversed in place) */
static size_t ts_count_run(double *a, size_t lo, size_t hi) {
    size_t i = lo + 1; if (i == hi) return 1;
    if (a[i] < a[lo]) { while (i + 1 < hi && a[i + 1] < a[i]) i++; ts_reverse(a, lo, i + 1); }
    else { while (i + 1 < hi && !(a[i + 1] < a[i])) i++; }
    return i + 1 - lo;
}
/* binary insertion sort of a[lo,hi) where a[lo,start) is already sorted */
static void ts_binsort(double *a, size_t lo, size_t hi, size_t start) {
    if (start == lo) start++;
    for (; start < hi; start++) {
        double p = a[start]; size_t l = lo, r = start;
        while (l < r) { size_t m = l + ((r - l) >> 1); if (p < a[m]) r = m; else l = m + 1; }
        for (size_t k = start; k > l; k--) a[k] = a[k - 1];
        a[l] = p;
    }
}
/* gallop_left: position where key belongs in sorted a[base, base+len), leftmost among equals */
static size_t ts_gallop_left(double key, const double *a, size_t len, size_t hint) {
    size_t ofs = 1, last = 0;
    if (a[hint] < key) {                                /* gallop right until a[hint+last] < key <= a[hint+ofs] */
        size_t maxofs = len - hint;
        while (ofs < maxofs && a[hint + ofs] < key) { last = ofs; ofs = (ofs << 1) + 1; }
        if (ofs > maxofs) ofs = maxofs;
        last += hint; ofs += hint;
    } else {                                            /* gallop left until a[hint-ofs] < key <= a[hint-last] */
        size_t maxofs = hint + 1;
        while (ofs < maxofs && !(a[hint - ofs] < key)) { last = ofs; ofs = (ofs << 1) + 1; }
        if (ofs > maxofs) ofs = maxofs;
        size_t t = last; last = hint - ofs; ofs = hint - t;
    }
    last++;                                             /* now a[last-1] < key <= a[ofs] */
    while (last < ofs) { size_t m = last + ((ofs - last) >> 1); if (a[m] < key) last = m + 1; else ofs = m; }
    return ofs;
}
/* gallop_right: position where key belongs, rightmost among equals */
static size_t ts_gallop_right(double key, const double *a, size_t len, size_t hint) {
    size_t ofs = 1, last = 0;
    if (key < a[hint]) {                                /* gallop left until a[hint-ofs] <= key < a[hint-last] */
        size_t maxofs = hint + 1;
        while (ofs < maxofs && key < a[hint - ofs]) { last = ofs; ofs = (ofs << 1) + 1; }
        if (ofs > maxofs) ofs = maxofs;
        size_t t = last; last = hint - ofs; ofs = hint - t;
    } else {                                            /* gallop right until a[hint+last] <= key < a[hint+ofs] */
        size_t maxofs = len - hint;
        while (ofs < maxofs && !(key < a[hint + ofs])) { last = ofs; ofs = (ofs << 1) + 1; }
        if (ofs > maxofs) ofs = maxofs;
        last += hint; ofs += hint;
    }
    last++;
    while (last < ofs) { size_t m = last + ((ofs - last) >> 1); if (key < a[m]) ofs = m; else last = m + 1; }
    return ofs;
}
static double *ts_tmp(ts_state *s, size_t need) { if (need > s->tmp_cap) { free(s->tmp); s->tmp = (double *)malloc(need * sizeof(double)); s->tmp_cap = s->tmp ? need : 0; } return s->tmp; }
/* merge runs a[b1,b1+l1) and a[b1+l1, b1+l1+l2) where l1 <= l2, run1 copied to tmp */
static void ts_merge_lo(ts_state *s, size_t b1, size_t l1, size_t b2, size_t l2) {
    double *a = s->a, *t = ts_tmp(s, l1); if (!t) return;
    memcpy(t, a + b1, l1 * sizeof(double));
    size_t i = 0, j = b2, d = b1; int min_gallop = s->min_gallop;
    a[d++] = a[j++]; l2--;
    if (l2 == 0) { memcpy(a + d, t + i, l1 * sizeof(double)); return; }
    if (l1 == 1) { memmove(a + d, a + j, l2 * sizeof(double)); a[d + l2] = t[i]; return; }
    for (;;) {
        size_t acount = 0, bcount = 0;
        for (;;) {                                      /* one-at-a-time until one run wins min_gallop times in a row */
            if (a[j] < t[i]) { a[d++] = a[j++]; l2--; bcount++; acount = 0; if (l2 == 0) goto finish; if (bcount >= (size_t)min_gallop) break; }
            else { a[d++] = t[i++]; l1--; acount++; bcount = 0; if (l1 == 1) goto finish; if (acount >= (size_t)min_gallop) break; }
        }
        min_gallop++;
        do {                                            /* galloping mode */
            min_gallop -= min_gallop > 1;
            s->min_gallop = min_gallop;
            size_t k = ts_gallop_right(a[j], t + i, l1, 0);
            acount = k;
            if (k) { memcpy(a + d, t + i, k * sizeof(double)); d += k; i += k; l1 -= k; if (l1 == 1) goto finish; if (l1 == 0) goto finish; }
            a[d++] = a[j++]; l2--; if (l2 == 0) goto finish;
            k = ts_gallop_left(t[i], a + j, l2, 0);
            bcount = k;
            if (k) { memmove(a + d, a + j, k * sizeof(double)); d += k; j += k; l2 -= k; if (l2 == 0) goto finish; }
            a[d++] = t[i++]; l1--; if (l1 == 1) goto finish;
        } while (acount >= TS_MIN_GALLOP || bcount >= TS_MIN_GALLOP);
        min_gallop++; s->min_gallop = min_gallop;
    }
finish:
    if (l1 == 1 && l2 > 0) { memmove(a + d, a + j, l2 * sizeof(double)); a[d + l2] = t[i]; }
    else if (l1 > 0) memcpy(a + d, t + i, l1 * sizeof(double));
}
/* merge where l1 >= l2: run2 copied to tmp, merged from the right */
static void ts_merge_hi(ts_state *s, size_t b1, size_t l1, size_t b2, size_t l2) {
    double *a = s->a, *t = ts_tmp(s, l2); if (!t) return;
    memcpy(t, a + b2, l2 * sizeof(double));
    size_t i = b1 + l1 - 1, j = l2 - 1, d = b2 + l2 - 1; int min_gallop = s->min_gallop;
    a[d--] = a[i--]; l1--;
    if (l1 == 0) { memcpy(a + d - (l2 - 1), t, l2 * sizeof(double)); return; }
    if (l2 == 1) { d -= l1; i -= l1; memmove(a + d + 1, a + i + 1, l1 * sizeof(double)); a[d] = t[j]; return; }
    for (;;) {
        size_t acount = 0, bcount = 0;
        for (;;) {
            if (t[j] < a[i]) { a[d--] = a[i--]; l1--; acount++; bcount = 0; if (l1 == 0) goto finish; if (acount >= (size_t)min_gallop) break; }
            else { a[d--] = t[j--]; l2--; bcount++; acount = 0; if (l2 == 1) goto finish; if (bcount >= (size_t)min_gallop) break; }
        }
        min_gallop++;
        do {
            min_gallop -= min_gallop > 1;
            s->min_gallop = min_gallop;
            size_t k = ts_gallop_right(t[j], a + b1, l1, l1 - 1);   /* elements of run1 > t[j] */
            k = l1 - k; acount = k;
            if (k) { d -= k; i -= k; memmove(a + d + 1, a + i + 1, k * sizeof(double)); l1 -= k; if (l1 == 0) goto finish; }
            a[d--] = t[j--]; l2--; if (l2 == 1) goto finish;
            k = ts_gallop_left(a[i], t, l2, l2 - 1);                /* elements of run2 >= a[i] */
            k = l2 - k; bcount = k;
            if (k) { d -= k; j -= k; memcpy(a + d + 1, t + j + 1, k * sizeof(double)); l2 -= k; if (l2 == 1) goto finish; if (l2 == 0) goto finish; }
            a[d--] = a[i--]; l1--; if (l1 == 0) goto finish;
        } while (acount >= TS_MIN_GALLOP || bcount >= TS_MIN_GALLOP);
        min_gallop++; s->min_gallop = min_gallop;
    }
finish:
    if (l2 == 1 && l1 > 0) { d -= l1; i -= l1; memmove(a + d + 1, a + i + 1, l1 * sizeof(double)); a[d] = t[j]; }
    else if (l2 > 0) memcpy(a + d - (l2 - 1), t, l2 * sizeof(double));
}
static void ts_merge_at(ts_state *s, int idx) {
    double *a = s->a; size_t b1 = s->run[idx].base, l1 = s->run[idx].len, b2 = s->run[idx + 1].base, l2 = s->run[idx + 1].len;
    s->run[idx].len = l1 + l2;
    if (idx == s->nrun - 3) s->run[idx + 1] = s->run[idx + 2];
    s->nrun--;
    size_t k = ts_gallop_right(a[b2], a + b1, l1, 0); b1 += k; l1 -= k; if (l1 == 0) return;   /* run1's head already in place */
    l2 = ts_gallop_left(a[b1 + l1 - 1], a + b2, l2, l2 - 1); if (l2 == 0) return;              /* run2's tail already in place */
    if (l1 <= l2) ts_merge_lo(s, b1, l1, b2, l2); else ts_merge_hi(s, b1, l1, b2, l2);
}
static void ts_merge_collapse(ts_state *s) {
    while (s->nrun > 1) {
        int n = s->nrun - 2;
        if ((n > 0 && s->run[n - 1].len <= s->run[n].len + s->run[n + 1].len) || (n > 1 && s->run[n - 2].len <= s->run[n - 1].len + s->run[n].len)) {
            if (s->run[n - 1].len < s->run[n + 1].len) n--;
            ts_merge_at(s, n);
        } else if (s->run[n].len <= s->run[n + 1].len) ts_merge_at(s, n);
        else break;
    }
}
static void timsort_f64(double *a, size_t n) {
    if (n < 2) return;
    ts_state s; memset(&s, 0, sizeof s); s.a = a; s.n = n; s.min_gallop = TS_MIN_GALLOP;
    if (n < TS_MIN_MERGE) { size_t r = ts_count_run(a, 0, n); ts_binsort(a, 0, n, r); return; }
    size_t minrun = ts_minrun(n), lo = 0, rem = n;
    while (rem) {
        size_t r = ts_count_run(a, lo, lo + rem);
        if (r < minrun) { size_t force = rem < minrun ? rem : minrun; ts_binsort(a, lo, lo + force, lo + r); r = force; }
        s.run[s.nrun].base = lo; s.run[s.nrun].len = r; s.nrun++;
        ts_merge_collapse(&s);
        lo += r; rem -= r;
    }
    while (s.nrun > 1) { int n2 = s.nrun - 2; if (n2 > 0 && s.run[n2 - 1].len < s.run[n2 + 1].len) n2--; ts_merge_at(&s, n2); }
    free(s.tmp);
}
#endif
