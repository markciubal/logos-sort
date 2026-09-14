/* i32abbench.c — paired A/B for the int32 changes in experiments/team/i32-sentinel-seq.md.
 *
 * Every variant here is a RUNTIME toggle read uncached from the environment, so ONE binary (one code
 * layout) measures both sides of each pair: no copy-swap is needed and the ~1.24x layout bias of
 * separately compiled copies (methodology rule 3) does not exist. Drift cancels inside a pair
 * (bench_paired's design, ported to int32 below). Ratio > 1.000 means the NEW behaviour is faster.
 *
 *   section A  sequential leaf, windowed counting     LOGOS_I32_NO_LEAF_WINDOW=1  vs  unset
 *   section B  parallel count route, table budget     LOGOS_I32_FIXED_CAP=1       vs  unset (64 MB)
 *              plus intbench-style adjacent 1T vs PT min-of-reps, which is the P/1 the target names
 *   section C  sequential counting gate divisor       LOGOS_I32_COUNT_DIV=1       vs  2, 4, 8
 *   section D  table budget sweep, P threads           LOGOS_I32_TABLE_MB=64       vs  32, 128, 256
 *   section F  persistent count tables, P threads      LOGOS_I32_NO_TABLE_POOL=1   vs  unset
 *   section G  the target's own metric: sentinel row / clean row, the two DATA sets sorted adjacently by
 *              the same engine (no toggle), 1 thread, window on and then window off
 *   section E  the gate divisor's one PARALLEL side-effect: partition leaves of ~n/168 elements count
 *              under DIV 1 and radix under DIV 4 when the span is in (n/4, n], i.e. spans that decline
 *              the count route (6e6, 8e6 at 64 MB)                LOGOS_I32_COUNT_DIV=1 vs 4, P threads
 *
 * Usage: ./i32abbench [pairs] [n] [section: A|B|C|all]        LOGOS_THREADS=N overrides P. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t RS = 12345;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static int cmp_i(const void *a, const void *b) { int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }

typedef void (*isort_fn)(int32_t *, size_t, int);
/* the two sides of a pair: environment set for the call, cleared after (all workers are joined by then) */
static const char *g_var_a = NULL, *g_val_a = NULL, *g_var_b = NULL, *g_val_b = NULL;
static void side_a(int32_t *a, size_t n, int t) { if (g_var_a) setenv(g_var_a, g_val_a, 1); logos_sort_i32(a, n, t); if (g_var_a) unsetenv(g_var_a); }
static void side_b(int32_t *a, size_t n, int t) { if (g_var_b) setenv(g_var_b, g_val_b, 1); logos_sort_i32(a, n, t); if (g_var_b) unsetenv(g_var_b); }

/* bench_paired from bench_common.h, for int32: median of tA/tB over adjacent pairs, alternating order */
static double paired_i32(isort_fn A, isort_fn B, const int32_t *base, int32_t *work, size_t n, int threads, int reps, double *p10, double *p90) {
    double *r = malloc((size_t)reps * sizeof(double)); if (!r) return 0.0;
    for (int i = 0; i < reps; i++) {
        double ta, tb, s;
        if (i & 1) { memcpy(work, base, n * 4); s = bench_now_(); B(work, n, threads); tb = bench_now_() - s;
                     memcpy(work, base, n * 4); s = bench_now_(); A(work, n, threads); ta = bench_now_() - s; }
        else       { memcpy(work, base, n * 4); s = bench_now_(); A(work, n, threads); ta = bench_now_() - s;
                     memcpy(work, base, n * 4); s = bench_now_(); B(work, n, threads); tb = bench_now_() - s; }
        r[i] = tb > 0 ? ta / tb : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), bench_cmp_d_);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    if (p10) *p10 = r[(int)(0.10 * reps)]; if (p90) *p90 = r[(int)(0.90 * reps)];
    free(r); return med;
}
/* two different inputs, one engine: median of t(base1)/t(base2) over adjacent pairs, alternating order */
static double paired_data(isort_fn f, const int32_t *b1, const int32_t *b2, int32_t *work, size_t n, int threads, int reps, double *p10, double *p90) {
    double *r = malloc((size_t)reps * sizeof(double)); if (!r) return 0.0;
    for (int i = 0; i < reps; i++) {
        double t1, t2, s;
        if (i & 1) { memcpy(work, b2, n * 4); s = bench_now_(); f(work, n, threads); t2 = bench_now_() - s;
                     memcpy(work, b1, n * 4); s = bench_now_(); f(work, n, threads); t1 = bench_now_() - s; }
        else       { memcpy(work, b1, n * 4); s = bench_now_(); f(work, n, threads); t1 = bench_now_() - s;
                     memcpy(work, b2, n * 4); s = bench_now_(); f(work, n, threads); t2 = bench_now_() - s; }
        r[i] = t2 > 0 ? t1 / t2 : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), bench_cmp_d_);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    if (p10) *p10 = r[(int)(0.10 * reps)]; if (p90) *p90 = r[(int)(0.90 * reps)];
    free(r); return med;
}
/* rule 1: both sides against qsort before anything is timed */
static int verify_both(const int32_t *base, int32_t *work, int32_t *ref, size_t n, int t) {
    int ok = 1;
    memcpy(work, base, n * 4); side_a(work, n, t); ok &= memcmp(work, ref, n * 4) == 0;
    memcpy(work, base, n * 4); side_b(work, n, t); ok &= memcmp(work, ref, n * 4) == 0;
    return ok;
}
static double min_of(isort_fn f, const int32_t *base, int32_t *work, size_t n, int t, int reps) {
    double best = 1e30;
    for (int r = 0; r < reps; r++) { memcpy(work, base, n * 4); double s = bench_now_(); f(work, n, t); double d = bench_now_() - s; if (d < best) best = d; }
    return best;
}

typedef void (*gen_fn)(int32_t *, size_t);
static void s_clean(int32_t *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 100); }
static void s_one_min(int32_t *a, size_t n)  { s_clean(a, n); a[n / 2] = INT32_MIN; }
static void s_both(int32_t *a, size_t n)     { s_clean(a, n); a[n / 3] = INT32_MIN; a[2 * n / 3] = INT32_MAX; }
static void s_one_neg1(int32_t *a, size_t n) { s_clean(a, n); a[n / 2] = -1; }
static void s_many_min(int32_t *a, size_t n) { s_clean(a, n); for (size_t i = 0; i < n; i += 20) a[i] = INT32_MIN; }
static void s_tail01(int32_t *a, size_t n)   { s_clean(a, n); for (size_t i = 7; i < n; i += 1024) a[i] = INT32_MIN + (int32_t)(nr() % 1000000); }  /* 0.1% wide FAR tail: trimmed window + recursive tail sort (a near tail of span 1e6 fits the plain gate at 8M and never reaches the window) */
static void s_full(int32_t *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(uint32_t)nr(); }
static void s_1e8(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 100000000); }
static void s_1e5(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 100000); }
static void s_5e5(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 500000); }
static void s_1e6(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 1000000); }
static void s_2e6(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 2000000); }
static void s_4e6(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 4000000); }
static void s_8e6(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 8000000); }
static void s_6e6(int32_t *a, size_t n)      { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 6000000); }
static void s_16e6(int32_t *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % 16000000); }
static void s_n2(int32_t *a, size_t n)       { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % (n / 2 + 1)); }
static void s_n(int32_t *a, size_t n)        { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(nr() % (n + 1)); }

typedef struct { const char *name; gen_fn g; const char *note; } shape_t;
static const shape_t SA[] = {
    { "bulk 0..99",             s_clean,    "guard: plain counting, window never consulted" },
    { "bulk + one INT32_MIN",   s_one_min,  "THE target: one far sentinel" },
    { "bulk + both extremes",   s_both,     "span 2^32, tails on both sides" },
    { "bulk + one -1",          s_one_neg1, "guard: span 101, plain counting" },
    { "bulk + 0.1% wide tail",  s_tail01,   "trimmed window, tail sorted recursively" },
    { "bulk + 5% INT32_MIN",    s_many_min, "guard: declines at the sample" },
    { "span 1e8",               s_1e8,      "guard: pays the sample, then radix" },
    { "full 2^32",              s_full,     "guard: pays the sample, then radix" },
};
static const shape_t SB[] = {
    { "span 1e5",               s_1e5,      "guard: under the old cap, same tables" },
    { "span 5e5",               s_5e5,      "band: 14 threads x 2 MB" },
    { "span 1e6",               s_1e6,      "THE target: 14 threads x 4 MB" },
    { "span 2e6",               s_2e6,      "band: 8 threads x 8 MB" },
    { "span 4e6",               s_4e6,      "band: 4 threads x 16 MB" },
    { "span 8e6",               s_8e6,      "declines (2 threads): partition, as before" },
    { "full 2^32",              s_full,     "guard: declines at the sample" },
};
static const shape_t SC[] = {
    { "span 1e6",  s_1e6, "counts under every DIV <= 8" },
    { "span 2e6",  s_2e6, "" }, { "span 4e6", s_4e6, "" }, { "span n/2", s_n2, "" }, { "span n", s_n, "the inherited gate's top edge" },
};
#define NA ((int)(sizeof SA / sizeof SA[0]))
#define NB ((int)(sizeof SB / sizeof SB[0]))
#define NC ((int)(sizeof SC / sizeof SC[0]))

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 4) pairs = 4;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8000000;
    const char *sec = argc > 3 ? argv[3] : "all";
    int P = bench_threads(logos_num_cpus());
    int32_t *base = malloc(n * 4), *work = malloc(n * 4), *ref = malloc(n * 4);
    if (!base || !work || !ref) return 1;
    double p10, p90;
    printf("i32abbench: n=%zu, %d pairs, P=%d. One binary, runtime toggles: no layout bias. ratio > 1 = NEW faster.\n\n", n, pairs, P);

    if (!strcmp(sec, "all") || !strcmp(sec, "A")) {
        g_var_a = "LOGOS_I32_NO_LEAF_WINDOW"; g_val_a = "1"; g_var_b = NULL;
        printf("== A: sequential leaf, windowed counting (old = window off), 1 thread ==\n");
        printf("  %-26s %8s %8s %8s  %s\n", "shape", "old/new", "p10", "p90", "note");
        for (int s = 0; s < NA; s++) {
            RS = 12345; SA[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            int ok = verify_both(base, work, ref, n, 1);
            double med = paired_i32(side_a, side_b, base, work, n, 1, pairs, &p10, &p90);
            printf("  %-26s %8.3f %8.3f %8.3f  %s%s\n", SA[s].name, med, p10, p90, SA[s].note, ok ? "" : "   VERIFY FAILED");
            printf("ROW\tA\t%s\t1\t%.4f\t%.4f\t%.4f\t%d\n", SA[s].name, med, p10, p90, ok); fflush(stdout);
        }
        printf("\n");
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "B")) {
        g_var_a = "LOGOS_I32_FIXED_CAP"; g_val_a = "1"; g_var_b = NULL;
        printf("== B: parallel count route, table budget (old = fixed 262144-slot cap), %d threads ==\n", P);
        printf("  %-14s %8s %8s %8s | %9s %9s %9s %6s  %s\n", "shape", "old/new", "p10", "p90", "1T min", "PT old", "PT new", "P/1new", "note");
        for (int s = 0; s < NB; s++) {
            RS = 12345; SB[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            int ok = verify_both(base, work, ref, n, P);
            double med = paired_i32(side_a, side_b, base, work, n, P, pairs, &p10, &p90);
            /* intbench-style: 1T and PT adjacent, min of reps, so the P/1 column is the one the target names */
            int reps = pairs / 2 < 3 ? 3 : pairs / 2; double t1 = 1e30, to = 1e30, tn = 1e30;
            for (int r = 0; r < reps; r++) {
                double d;
                d = min_of(side_b, base, work, n, 1, 1); if (d < t1) t1 = d;
                d = min_of(side_a, base, work, n, P, 1); if (d < to) to = d;
                d = min_of(side_b, base, work, n, P, 1); if (d < tn) tn = d;
            }
            printf("  %-14s %8.3f %8.3f %8.3f | %8.1fms %8.1fms %8.1fms %6.2f  %s%s\n", SB[s].name, med, p10, p90, t1, to, tn, t1 / tn, SB[s].note, ok ? "" : "   VERIFY FAILED");
            printf("ROW\tB\t%s\t%d\t%.4f\t%.4f\t%.4f\t%d\t%.2f\t%.2f\t%.2f\n", SB[s].name, P, med, p10, p90, ok, t1, to, tn); fflush(stdout);
        }
        printf("\n");
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "C")) {
        static const char *DIVS[] = { "2", "4", "8" };
        printf("== C: sequential counting gate span <= n/DIV (old = DIV 1), 1 thread ==\n");
        printf("  %-12s %6s %8s %8s %8s  %s\n", "shape", "DIV", "old/new", "p10", "p90", "note");
        for (int s = 0; s < NC; s++) {
            RS = 12345; SC[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            for (int d = 0; d < 3; d++) {
                g_var_a = "LOGOS_I32_COUNT_DIV"; g_val_a = "1"; g_var_b = "LOGOS_I32_COUNT_DIV"; g_val_b = DIVS[d];
                int ok = verify_both(base, work, ref, n, 1);
                double med = paired_i32(side_a, side_b, base, work, n, 1, pairs, &p10, &p90);
                printf("  %-12s %6s %8.3f %8.3f %8.3f  %s%s\n", SC[s].name, DIVS[d], med, p10, p90, SC[s].note, ok ? "" : "   VERIFY FAILED");
                printf("ROW\tC\t%s\t%s\t%.4f\t%.4f\t%.4f\t%d\n", SC[s].name, DIVS[d], med, p10, p90, ok); fflush(stdout);
            }
        }
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "D")) {
        static const char *MBS[] = { "32", "128", "256" };
        static const shape_t SD[] = { { "span 1e6", s_1e6, "" }, { "span 2e6", s_2e6, "" }, { "span 4e6", s_4e6, "" }, { "span 8e6", s_8e6, "declines at 64 MB" } };
        printf("== D: table budget sweep (old = 64 MB, the default), %d threads ==\n", P);
        printf("  %-12s %6s %8s %8s %8s  %s\n", "shape", "MB", "64/new", "p10", "p90", "note");
        for (int s = 0; s < 4; s++) {
            RS = 12345; SD[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            for (int d = 0; d < 3; d++) {
                g_var_a = "LOGOS_I32_TABLE_MB"; g_val_a = "64"; g_var_b = "LOGOS_I32_TABLE_MB"; g_val_b = MBS[d];
                int ok = verify_both(base, work, ref, n, P);
                double med = paired_i32(side_a, side_b, base, work, n, P, pairs, &p10, &p90);
                printf("  %-12s %6s %8.3f %8.3f %8.3f  %s%s\n", SD[s].name, MBS[d], med, p10, p90, SD[s].note, ok ? "" : "   VERIFY FAILED");
                printf("ROW\tD\t%s\t%s\t%.4f\t%.4f\t%.4f\t%d\n", SD[s].name, MBS[d], med, p10, p90, ok); fflush(stdout);
            }
        }
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "E")) {
        static const shape_t SE[] = { { "span 6e6", s_6e6, "leaves ~36k wide: count at DIV 1, radix at DIV 4" }, { "span 8e6", s_8e6, "leaves ~48k wide: same" }, { "span 1.6e7", s_16e6, "guard: leaves ~95k wide, radix under both" } };
        printf("== E: gate divisor in the PARALLEL leaves (old = DIV 1, new = DIV 4), %d threads ==\n", P);
        printf("  %-12s %8s %8s %8s  %s\n", "shape", "old/new", "p10", "p90", "note");
        for (int s = 0; s < 3; s++) {
            RS = 12345; SE[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            g_var_a = "LOGOS_I32_COUNT_DIV"; g_val_a = "1"; g_var_b = "LOGOS_I32_COUNT_DIV"; g_val_b = "4";
            int ok = verify_both(base, work, ref, n, P);
            double med = paired_i32(side_a, side_b, base, work, n, P, pairs, &p10, &p90);
            printf("  %-12s %8.3f %8.3f %8.3f  %s%s\n", SE[s].name, med, p10, p90, SE[s].note, ok ? "" : "   VERIFY FAILED");
            printf("ROW\tE\t%s\t%d\t%.4f\t%.4f\t%.4f\t%d\n", SE[s].name, P, med, p10, p90, ok); fflush(stdout);
        }
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "F")) {
        static const shape_t SF[] = { { "span 1e5", s_1e5, "14 x 0.4 MB" }, { "span 1e6", s_1e6, "14 x 4 MB" }, { "span 2e6", s_2e6, "8 x 8 MB" }, { "span 4e6", s_4e6, "4 x 16 MB" } };
        printf("== F: persistent count tables (old = calloc/free per call), %d threads ==\n", P);
        printf("  %-12s %8s %8s %8s  %s\n", "shape", "old/new", "p10", "p90", "note");
        for (int s = 0; s < 4; s++) {
            RS = 12345; SF[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            g_var_a = "LOGOS_I32_NO_TABLE_POOL"; g_val_a = "1"; g_var_b = NULL;
            int ok = verify_both(base, work, ref, n, P);
            double med = paired_i32(side_a, side_b, base, work, n, P, pairs, &p10, &p90);
            printf("  %-12s %8.3f %8.3f %8.3f  %s%s\n", SF[s].name, med, p10, p90, SF[s].note, ok ? "" : "   VERIFY FAILED");
            printf("ROW\tF\t%s\t%d\t%.4f\t%.4f\t%.4f\t%d\n", SF[s].name, P, med, p10, p90, ok); fflush(stdout);
        }
    }
    if (!strcmp(sec, "all") || !strcmp(sec, "G")) {
        static const shape_t SG[] = { { "bulk + one INT32_MIN", s_one_min, "" }, { "bulk + both extremes", s_both, "" }, { "bulk + 0.1% wide tail", s_tail01, "" }, { "bulk + one -1", s_one_neg1, "control: plain counting both" } };
        int32_t *clean = malloc(n * 4); if (!clean) return 1;
        RS = 12345; s_clean(clean, n);
        printf("== G: sentinel row / clean row, same engine, adjacent, 1 thread (the target's metric) ==\n");
        printf("  %-26s %8s %8s %8s | %8s %8s %8s\n", "shape", "win ON", "p10", "p90", "win OFF", "p10", "p90");
        for (int s = 0; s < 4; s++) {
            RS = 12345; SG[s].g(base, n); memcpy(ref, base, n * 4); qsort(ref, n, 4, cmp_i);
            g_var_a = "LOGOS_I32_NO_LEAF_WINDOW"; g_val_a = "1"; g_var_b = NULL;
            int ok = verify_both(base, work, ref, n, 1);
            double q10, q90;
            double on  = paired_data(side_b, base, clean, work, n, 1, pairs, &p10, &p90);
            double off = paired_data(side_a, base, clean, work, n, 1, pairs, &q10, &q90);
            printf("  %-26s %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f  %s%s\n", SG[s].name, on, p10, p90, off, q10, q90, SG[s].note, ok ? "" : "   VERIFY FAILED");
            printf("ROW\tG\t%s\t1\t%.4f\t%.4f\t%.4f\t%.4f\t%d\n", SG[s].name, on, p10, p90, off, ok); fflush(stdout);
        }
        free(clean);
    }
    free(base); free(work); free(ref);
    return 0;
}
