/* ledgerbench.c — re-try the ledger's "neutral" and "mixed" verdicts against the new noise floor.
 *
 * Every one of those verdicts was reached when an interleaved A/B here could not resolve better than
 * about 1.25x (methodology rule 3: separately compiled copies differ by up to 1.24x purely from code
 * alignment). The paired harness now resolves about 4%. So anything once dismissed as noise in the
 * 1.05-1.25 band was never actually tested, and the ledger's "do not re-run" rule does not apply:
 * a tenfold improvement in resolution IS the new angle.
 *
 * Two engine copies are linked in, lg0 and lg1, with their flags set from the Makefile (LB0, LB1).
 * bench_paired returns t(lg0)/t(lg1), so with the baseline in lg0 a number ABOVE 1.000 means the
 * feature in lg1 is that much faster.
 *
 * Pairing alone does not remove the layout bias: both halves of a pair still live in fixed copies.
 * What removes it is running the whole thing twice with the assignment swapped and taking the
 * geometric mean. With layout factors L0 and L1 and a true effect E, one orientation measures
 * (1/E)(L0/L1) and the other (1/E)(L1/L0), so the geometric mean is exactly 1/E. tools/ledger_rerun.py
 * drives both orientations and does that arithmetic.
 *
 * Usage: ./ledgerbench [pairs] [n] [shape] [threads]    LOGOS_THREADS=N overrides the parallel count.
 * The last two narrow the run to one cell, which is how a borderline row gets more pairs cheaply. */
#define _GNU_SOURCE
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

void lg0_sort_f64(double *, size_t, int); int lg0_num_cpus(void);
void lg1_sort_f64(double *, size_t, int);

static uint64_t RS = 88172645463325252ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

typedef void (*gen_fn)(double *, size_t);
static void g_uniform(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_heavy(double *a, size_t n)     { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); } }
static void g_bimodal(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 1e6; }
static void g_clustered(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 16) * 1e8 + fr() * 1e5; }
static void g_intwide(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)nr(); }

static const struct { const char *name; gen_fn g; } SH[] = {
    { "uniform", g_uniform }, { "heavy_tail", g_heavy }, { "bimodal", g_bimodal },
    { "clustered", g_clustered }, { "int_wide", g_intwide },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40; if (reps < 8) reps = 8;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8000000;
    const char *only = argc > 3 && argv[3][0] ? argv[3] : NULL;   /* one shape, to settle a borderline row */
    int onlyT = argc > 4 ? atoi(argv[4]) : 0;
    int P = bench_threads(lg0_num_cpus());
    double *base = malloc(n * 8), *work = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !work || !ref) return 1;

    printf("ledger re-run: lg0 vs lg1, n=%zu, %d pairs, threads 1 and %d\n", n, reps, P);
    printf("ratio > 1.000 means the lg1 build is faster. Run ./pairprobe for today's floor,\n");
    printf("and read the geometric mean of BOTH orientations, not one (see the header).\n\n");
    printf("  %-12s %2s %9s %9s %9s  %s\n", "shape", "T", "ratio", "p10", "p90", "verify");

    for (int k = 0; k < NSH; k++) {
        if (only && strcmp(only, SH[k].name) != 0) continue;
        RS = 88172645463325252ULL;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);      /* rule 1: verify before timing */
        int ok = 1;
        memcpy(work, base, n * 8); lg0_sort_f64(work, n, P); ok &= memcmp(work, ref, n * 8) == 0;
        memcpy(work, base, n * 8); lg1_sort_f64(work, n, P); ok &= memcmp(work, ref, n * 8) == 0;
        memcpy(work, base, n * 8); lg0_sort_f64(work, n, 1); ok &= memcmp(work, ref, n * 8) == 0;
        memcpy(work, base, n * 8); lg1_sort_f64(work, n, 1); ok &= memcmp(work, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            if (onlyT && tc[ti] != onlyT) continue;
            double p10 = 0, p90 = 0;
            double med = bench_paired(lg0_sort_f64, lg1_sort_f64, base, work, n, tc[ti], reps, &p10, &p90);
            printf("  %-12s %2d %9.3f %9.3f %9.3f  %s\n", SH[k].name, tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
            printf("ROW\t%s\t%d\t%.5f\t%.5f\t%.5f\t%d\n", SH[k].name, tc[ti], med, p10, p90, ok);
            fflush(stdout);
        }
    }
    free(base); free(work); free(ref);
    return 0;
}
