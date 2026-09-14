/* prefixbench.c — the sorted-prefix-plus-tail route, measured.
 *
 * Append-mostly data: a sorted prefix with a batch of unsorted values on the end. The route sorts only the
 * tail and merges it back in, gated by where the prescan's walk up the prefix stops. One binary, switched at
 * runtime by LOGOS_NO_TAIL, so there is no code-layout bias between the halves of a pair; every row runs
 * under the paired harness and is verified against qsort first.
 *
 * PREDICTIONS, written before this ran:
 *   prefix_95 at 1 thread 6-9x, at 14 threads 1.3-1.8x (the zoo measured 9.36x and 1.79x off the ceiling);
 *   prefix_99 larger than prefix_95 at 1 thread (less tail to sort), prefix_80 smaller;
 *   prefix_50 still a clear win (the gate's edge); prefix_49 exactly 1.00 (the route must not fire);
 *   uniform, near_1pct, sorted and reverse exactly 1.00 (random stops the prescan at once, scattered swaps
 *   stop it early, and sorted/reverse return before the route is reached).
 *
 * SECOND ROUND, the parallel form (tail_merge_par_f64: parallel NaN scan and copy, tail sorted in scratch, prefix cut
 * at P-1 positions with each cut's value searched in the tail, one merge per thread straight into a[]). Parallel
 * sorts take it only for a tail of at most n/8. PREDICTIONS for the 14-thread rows, written before it ran:
 *   prefix_99 1.3-1.6; prefix_95 1.2-1.5; prefix_90, inside the gate, 1.05-1.3;
 *   prefix_80 and prefix_50 exactly 1.00 (outside the gate); every guard exactly 1.00; the 1-thread rows unchanged.
 *   With LOGOS_TAIL_PAR_ANY=1 exported, lifting the gate: prefix_80 0.9-1.1 and prefix_50 0.5-0.7, because a
 *   tail that large is a full parallel sort of its own plus a scan, a merge and a scratch copy on top.
 * GRADED (8.4M, 20 pairs): 14 threads prefix_99 1.006, prefix_95 1.140, prefix_90 1.190, guards 0.99-1.02, 1-thread
 *   rows unchanged. With the gate lifted prefix_80 1.157 and prefix_50 1.034: the large-tail loss did not happen,
 *   so the gate is gone. Wrong at 99%, where the near-sorted route it displaces is already as fast.
 *   Parallel sorts now take the route at every tail size; LOGOS_TAIL_SEQ=1 restores sequential-only.
 *
 * Usage: ./prefixbench [pairs] [n]                                   LOGOS_THREADS=N overrides. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

/* an evenly spaced ascending prefix over the full value range, then a uniform tail that interleaves it */
static void prefix(double *a, size_t n, double f) {
    size_t d = (size_t)(f * (double)n);
    for (size_t i = 0; i < d; i++) a[i] = -1e9 + 2e9 * (double)i / (double)d;
    for (size_t i = d; i < n; i++) a[i] = fr() * 2e9 - 1e9;
}
static void g_p99(double *a, size_t n) { prefix(a, n, 0.99); }
static void g_p95(double *a, size_t n) { prefix(a, n, 0.95); }
static void g_p90(double *a, size_t n) { prefix(a, n, 0.90); }
static void g_p80(double *a, size_t n) { prefix(a, n, 0.80); }
static void g_p50(double *a, size_t n) { prefix(a, n, 0.50); }
static void g_p49(double *a, size_t n) { prefix(a, n, 0.49); }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_near(double *a, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = (double)i;
    for (size_t i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}
static void g_sorted(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)i; }
static void g_reverse(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(n - i); }

typedef void (*gen_fn)(double *, size_t);
static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "prefix_99",  g_p99,     "1% tail" },
    { "prefix_95",  g_p95,     "the zoo's shape" },
    { "prefix_90",  g_p90,     "10% tail: inside the parallel gate" },
    { "prefix_80",  g_p80,     "20% tail" },
    { "prefix_50",  g_p50,     "the gate's edge" },
    { "prefix_49",  g_p49,     "guard: must not fire" },
    { "uniform",    g_uniform, "guard" },
    { "near_1pct",  g_near,    "guard: early first descent" },
    { "sorted",     g_sorted,  "guard: returns before the route" },
    { "reverse",    g_reverse, "guard: returns before the route" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static void sort_off(double *a, size_t n, int t) { setenv("LOGOS_NO_TAIL", "1", 1); logos_sort_f64(a, n, t); }
static void sort_on(double *a, size_t n, int t)  { unsetenv("LOGOS_NO_TAIL");      logos_sort_f64(a, n, t); }

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;

    printf("sorted prefix + tail: route off vs on, n=%zu, %d pairs, threads 1 and %d, one binary (LOGOS_NO_TAIL)\n", n, pairs, P);
    printf("ratio > 1.000 means the route is that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-11s %2s %9s %9s %9s  %-32s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NSH; k++) {
        RS = 1 + (uint64_t)k;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_off(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, P);  ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, 1);  ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_off, sort_on, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-11s %2d %9.3f %9.3f %9.3f  %-32s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    free(base); free(w); free(ref);
    return 0;
}
