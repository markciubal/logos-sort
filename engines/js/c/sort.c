/* logos-sort: C implementation
 *
 * Licensed under the LogosAdaptive Sort Ethical Source License v1.1.
 * Dual-licensed: free for personal/research/educational/non-profit/OSS use,
 * commercial use requires paid license. See LICENSE for prohibited uses
 * (military, surveillance, non-consensual human genetic research,
 * human-rights violations are absolute prohibitions).
 *
 * Dispatcher:
 *   n <= 24             → insertion sort
 *   n > 24, asc/desc    → fast-path: nothing to do / single reverse
 *   n > 24, narrow range → flash sort with insertion-sort cleanup
 *   n > 24, wide/skewed → introsort (quicksort + heapsort fallback)
 *
 * The fall-through to introsort handles pathological distributions where
 * flash sort would create catastrophically imbalanced buckets.
 */

#include "sort.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define LS_INSERTION_THRESHOLD 24
#define LS_FLASH_MIN_N         128
#define LS_FLASH_SAFETY_RATIO  0.05    /* if max bucket > ratio*n after flash → introsort */
#define LS_INTROSORT_DEPTH_K   2       /* depth limit = K * floor(log2(n)) */

/* ---------------- helpers ---------------- */

static inline void ls_swap(double *a, double *b) {
    double t = *a; *a = *b; *b = t;
}

static int ls_floor_log2(size_t n) {
    int r = 0;
    while (n > 1) { n >>= 1; r++; }
    return r;
}

/* ---------------- insertion sort ---------------- */

static void ls_insertion_sort(double *a, size_t lo, size_t hi) {
    for (size_t i = lo + 1; i <= hi; i++) {
        double k = a[i];
        size_t j = i;
        while (j > lo && a[j - 1] > k) { a[j] = a[j - 1]; j--; }
        a[j] = k;
    }
}

/* ---------------- heapsort (introsort fallback) ---------------- */

static void ls_sift_down(double *a, size_t start, size_t end) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end && a[child] < a[child + 1]) child++;
        if (a[root] >= a[child]) return;
        ls_swap(&a[root], &a[child]);
        root = child;
    }
}

static void ls_heapsort(double *a, size_t lo, size_t hi) {
    size_t n = hi - lo + 1;
    double *base = a + lo;
    /* Heapify */
    for (size_t i = n / 2; i > 0;) {
        i--;
        ls_sift_down(base, i, n - 1);
    }
    /* Sort */
    for (size_t i = n - 1; i > 0; i--) {
        ls_swap(&base[0], &base[i]);
        ls_sift_down(base, 0, i - 1);
    }
}

/* ---------------- introsort (quicksort with depth limit → heapsort) ---------------- */

static void ls_introsort_loop(double *a, size_t lo, size_t hi, int depth_limit) {
    while (hi > lo) {
        if (hi - lo + 1 <= LS_INSERTION_THRESHOLD) {
            ls_insertion_sort(a, lo, hi);
            return;
        }
        if (depth_limit == 0) {
            ls_heapsort(a, lo, hi);
            return;
        }
        depth_limit--;

        /* Median-of-three pivot */
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < a[lo]) ls_swap(&a[mid], &a[lo]);
        if (a[hi]  < a[lo]) ls_swap(&a[hi],  &a[lo]);
        if (a[hi]  < a[mid]) ls_swap(&a[hi], &a[mid]);
        double pivot = a[mid];

        /* Hoare-style partition */
        size_t i = lo, j = hi;
        while (1) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i >= j) break;
            ls_swap(&a[i], &a[j]);
            i++; if (j == 0) break;
            j--;
        }

        /* Recurse on the smaller partition, iterate on the larger (limits stack depth) */
        if (j - lo < hi - (j + 1)) {
            if (j > lo) ls_introsort_loop(a, lo, j, depth_limit);
            lo = j + 1;
        } else {
            if (j + 1 < hi) ls_introsort_loop(a, j + 1, hi, depth_limit);
            hi = j;
        }
    }
}

static void ls_introsort(double *a, size_t n) {
    if (n < 2) return;
    int depth_limit = LS_INTROSORT_DEPTH_K * ls_floor_log2(n);
    ls_introsort_loop(a, 0, n - 1, depth_limit);
}

/* ---------------- flash sort (Neubert) ---------------- */

/* Returns 0 on success, 1 if any bucket exceeded the safety ratio (caller
 * should fall back to introsort). */
static int ls_flash_sort(double *a, size_t n, double *scratch) {
    if (n < LS_FLASH_MIN_N) return 1; /* too small to benefit */

    /* 1. Find min and max */
    double mn = a[0], mx = a[0];
    for (size_t i = 1; i < n; i++) {
        if (a[i] < mn) mn = a[i];
        else if (a[i] > mx) mx = a[i];
    }
    if (mn == mx) return 0; /* all equal — already sorted */

    /* 2. Build m buckets; Neubert recommends m = 0.43 * n */
    size_t m = (size_t)(0.43 * (double)n);
    if (m < 2) m = 2;

    /* l[k] = count of values in bucket k, later converted to prefix sums */
    size_t *l = (size_t *)calloc(m + 1, sizeof(size_t));
    if (!l) return 1;

    double c = ((double)(m - 1)) / (mx - mn);

    /* 3. Count phase */
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)(c * (a[i] - mn));
        if (k >= m) k = m - 1;
        l[k]++;
    }

    /* 3a. Safety check: any bucket holding more than safety_ratio * n means
     *     flash sort would degenerate. Bail to introsort. */
    size_t max_bucket = 0;
    for (size_t k = 0; k < m; k++) if (l[k] > max_bucket) max_bucket = l[k];
    if ((double)max_bucket > LS_FLASH_SAFETY_RATIO * (double)n && n > 1000) {
        free(l);
        return 1;
    }

    /* 4. Prefix sum: l[k] becomes the END (exclusive) of bucket k */
    for (size_t k = 1; k < m; k++) l[k] += l[k - 1];

    /* 5. Scatter into scratch using bucket prefix indices.
     *    We use scratch as the destination buffer; copy back when done. */
    size_t *heads = (size_t *)calloc(m, sizeof(size_t));
    if (!heads) { free(l); return 1; }
    heads[0] = 0;
    for (size_t k = 1; k < m; k++) heads[k] = l[k - 1];

    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)(c * (a[i] - mn));
        if (k >= m) k = m - 1;
        scratch[heads[k]++] = a[i];
    }
    memcpy(a, scratch, n * sizeof(double));

    /* 6. Within-bucket insertion sort — buckets are small after distribution */
    size_t start = 0;
    for (size_t k = 0; k < m; k++) {
        size_t end = l[k];
        if (end > start + 1) ls_insertion_sort(a, start, end - 1);
        start = end;
    }

    free(heads);
    free(l);
    return 0;
}

/* ---------------- entry probe (cheap dispatch hint) ---------------- */

typedef enum {
    LS_PROBE_ASCENDING,
    LS_PROBE_DESCENDING,
    LS_PROBE_ALL_EQUAL,
    LS_PROBE_MIXED
} ls_probe_t;

static ls_probe_t ls_probe(const double *a, size_t n) {
    if (n < 2) return LS_PROBE_ASCENDING;
    int asc = 1, desc = 1, eq = 1;
    for (size_t i = 1; i < n; i++) {
        if (a[i] < a[i - 1]) { asc = 0; eq = 0; }
        else if (a[i] > a[i - 1]) { desc = 0; eq = 0; }
        if (!asc && !desc && !eq) return LS_PROBE_MIXED;
    }
    if (eq)  return LS_PROBE_ALL_EQUAL;
    if (asc) return LS_PROBE_ASCENDING;
    return LS_PROBE_DESCENDING;
}

static void ls_reverse(double *a, size_t n) {
    for (size_t i = 0, j = n - 1; i < j; i++, j--) ls_swap(&a[i], &a[j]);
}

/* ---------------- public entry points ---------------- */

int ls_sort_double_with_buffer(double *arr, size_t n,
                               double *scratch, size_t scratch_size) {
    if (n < 2) return 0;
    if (scratch_size < n) return -1;

    /* Tiny arrays: just insertion sort directly */
    if (n <= LS_INSERTION_THRESHOLD) {
        ls_insertion_sort(arr, 0, n - 1);
        return 0;
    }

    /* Cheap probe for fast paths. Probe stops early on mixed input. */
    ls_probe_t shape = ls_probe(arr, n);
    if (shape == LS_PROBE_ASCENDING || shape == LS_PROBE_ALL_EQUAL) return 0;
    if (shape == LS_PROBE_DESCENDING) { ls_reverse(arr, n); return 0; }

    /* Try flash sort first; if it would degenerate, fall through to introsort. */
    if (ls_flash_sort(arr, n, scratch) == 0) return 0;

    ls_introsort(arr, n);
    return 0;
}

int ls_sort_double(double *arr, size_t n) {
    if (n < 2) return 0;
    double *scratch = (double *)malloc(n * sizeof(double));
    if (!scratch) return -1;
    int rc = ls_sort_double_with_buffer(arr, n, scratch, n);
    free(scratch);
    return rc;
}

const char *ls_sort_version(void) {
    return "0.1.3";
}
