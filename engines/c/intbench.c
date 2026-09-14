/* intbench.c — what the int32 path KNOWS versus what it asks, and what negatives do to that.
 *
 * An int32 array is always a lattice: step exactly 1, base min, no tolerance, no NaN, no two values
 * sharing a slot. That is a fact of the TYPE, not of the data, so it never has to be probed. Both
 * paths now use it: the sequential leaf counts when span <= n/4 (was n; the crossover against the
 * 3-pass radix was measured between n/4 and n/2, experiments/team/i32-sentinel-seq.md), and the parallel
 * path reaches count_try_i32. Both paths now window their table from the sample when the raw span
 * overflows it, so a rare far sentinel costs neither column anything.
 *
 * Section 1 races 1 thread against P on a span ladder. It is what found the defect: before
 * count_try_i32 existed the parallel path lost to a SINGLE core for every span under about n/8.
 * Section 2 asks what happens once you assume integers come with negatives. The sign itself is free
 * (the bias map is affine and the byte-skip already discovers whether the top bit varies). What is NOT
 * free is that real negatives are usually SENTINELS -- -1 for "missing", INT32_MIN for "unset" -- and a
 * range gate keyed on min/max is destroyed by one of them. A sample bounds the bulk, never the tail,
 * and a sentinel is a tail of size one. The parallel route now windows its table from the sample and
 * spills the tail, so a rare far sentinel costs nothing; a FREQUENT one still declines, because a tail
 * worth more than a thirty-second of the array is not a tail.
 *
 * Rows are min-of-REPS, taken with the two thread counts ADJACENT so they share the machine's thermal
 * state (methodology rule 2: never compare across rows, only within one).
 *
 * Usage: ./intbench [reps] [n]                                       LOGOS_THREADS=N overrides P.  */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t RS = 12345;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static int verify(const int32_t *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

static const struct { const char *name; long span; } LAD[] = {
    { "100 (categories)",      100L }, { "1e3 (scores)",  1000L }, { "1e4",          10000L },
    { "1e5 (zip-ish)",      100000L }, { "1e6",        1000000L }, { "n = 8e6",    8000000L },
    { "1e8 (> n)",       100000000L }, { "full 2^32",        0L },
};
#define NLAD ((int)(sizeof LAD / sizeof LAD[0]))

/* Section 2: the same BULK every time (0..99), so any difference is the negatives, not the work. */
typedef void (*gen_fn)(int32_t *, size_t);
static void s_clean(int32_t *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 100); }
static void s_signed(int32_t *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 100) - 50; }
static void s_allneg(int32_t *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = -(int32_t)(nr() % 100) - 1; }
static void s_one_min(int32_t *a, size_t n)  { s_clean(a, n); a[n / 2] = INT32_MIN; }              /* ONE sentinel */
static void s_one_neg1(int32_t *a, size_t n) { s_clean(a, n); a[n / 2] = -1; }                     /* adjacent, harmless */
static void s_many_min(int32_t *a, size_t n) { s_clean(a, n); for (size_t i = 0; i < n; i += 20) a[i] = INT32_MIN; }
static void s_both_ends(int32_t *a, size_t n){ s_clean(a, n); a[n / 3] = INT32_MIN; a[2 * n / 3] = INT32_MAX; }

static const struct { const char *name; gen_fn g; const char *note; } SIG[] = {
    { "bulk 0..99",            s_clean,     "baseline: span 100, counting" },
    { "bulk -50..49",          s_signed,    "genuinely signed, span 100" },
    { "bulk -100..-1",         s_allneg,    "all negative, span 100" },
    { "bulk + one -1",         s_one_neg1,  "sentinel next to the bulk" },
    { "bulk + one INT32_MIN",  s_one_min,   "ONE far sentinel, span 2^31" },
    { "bulk + 5% INT32_MIN",   s_many_min,  "frequent far sentinel" },
    { "bulk + both extremes",  s_both_ends, "span 2^32: the overflow case" },
};
#define NSIG ((int)(sizeof SIG / sizeof SIG[0]))

static void race(const int32_t *base, int32_t *w, size_t n, int P, int reps, double *t1, double *tp, int *ok) {
    *t1 = 1e30; *tp = 1e30; *ok = 1;
    for (int r = 0; r < reps; r++) {
        memcpy(w, base, n * 4); double s0 = bench_now_(); logos_sort_i32(w, n, 1); double d = bench_now_() - s0;
        if (d < *t1) *t1 = d; *ok &= verify(w, n);
        memcpy(w, base, n * 4);        s0 = bench_now_(); logos_sort_i32(w, n, P);        d = bench_now_() - s0;
        if (d < *tp) *tp = d; *ok &= verify(w, n);
    }
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 7; if (reps < 3) reps = 3;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8000000;
    int P = bench_threads(logos_num_cpus());
    int32_t *base = malloc(n * 4), *w = malloc(n * 4);
    if (!base || !w) return 1;
    double t1, tp; int ok;

    /* The arithmetic claim, checked rather than asserted: span in 32-bit signed wraps. */
    {   int32_t lo = INT32_MIN, hi = INT32_MAX;
        int32_t bad = (int32_t)((uint32_t)hi - (uint32_t)lo);          /* what a 32-bit gate computes */
        int64_t good = (int64_t)hi - (int64_t)lo;
        printf("span arithmetic: INT32_MAX - INT32_MIN is %d in 32-bit, %lld in 64-bit\n", bad, (long long)good);
        printf("  a `span <= budget` gate written in 32 bits therefore PASSES on the widest\n"
               "  possible input and then sizes a table from %d. Any such gate must be 64-bit.\n\n", bad);
    }

    printf("int32 n=%zu, min of %d, 1 thread vs %d threads on a span ladder\n", n, reps, P);
    printf("the type already guarantees a lattice of step 1; only the parallel path still asks\n\n");
    printf("  %-18s %11s %11s %8s   %s\n", "span", "1 thread", "P threads", "P/1", "verify");
    for (int s = 0; s < NLAD; s++) {
        RS = 12345; long sp = LAD[s].span;
        if (sp > 0) for (size_t i = 0; i < n; i++) base[i] = (int32_t)(nr() % (uint64_t)sp);
        else        for (size_t i = 0; i < n; i++) base[i] = (int32_t)(uint32_t)nr();
        race(base, w, n, P, reps, &t1, &tp, &ok);
        printf("  %-18s %10.1fms %10.1fms %8.2f   %s%s\n", LAD[s].name, t1, tp, tp > 0 ? t1 / tp : 0.0,
               ok ? "ok" : "FAILED", (tp > 0 && t1 / tp < 1.0) ? "   <-- P loses to one core" : "");
        fflush(stdout);
    }

    printf("\nnegatives and sentinels: the BULK is 100 distinct values in every row below,\n");
    printf("so any difference is what the negatives do to the gate, not extra work.\n\n");
    printf("  %-24s %11s %11s   %-30s %s\n", "shape", "1 thread", "P threads", "what it is", "verify");
    double tp_clean = 0;
    for (int s = 0; s < NSIG; s++) {
        RS = 12345; SIG[s].g(base, n);
        race(base, w, n, P, reps, &t1, &tp, &ok);
        if (s == 0) tp_clean = tp;
        printf("  %-24s %10.1fms %10.1fms   %-30s %s%s\n", SIG[s].name, t1, tp, SIG[s].note, ok ? "ok" : "FAILED",
               (tp_clean > 0 && tp > 2.0 * tp_clean) ? "   <-- P gate destroyed" : "");
        fflush(stdout);
    }
    printf("\n  P/1 below 1.00 means %d threads lose to a single core. A row more than 2x the\n"
           "  baseline means one outlier pushed the span past the counting gate and the sort\n"
           "  fell back to a full radix or partition it did not need. The PARALLEL path absorbs\n"
           "  rare far sentinels: count_try_i32 windows the table from the sample and spills the\n"
           "  tail, and since experiments/team/i32-sentinel-seq.md the 1-thread leaf does the same\n"
           "  (leaf_i32_windowed), so only the 5%% row should stay slow. The parallel count route\n"
           "  now fits its thread count to a table budget instead of a fixed span cap, which is why\n"
           "  the 1e6 row no longer loses to one core. See FINDINGS 3w.\n", P);
    free(base); free(w);
    return 0;
}
