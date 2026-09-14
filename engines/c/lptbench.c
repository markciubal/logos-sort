/* lptbench.c — does handing the BIGGEST leaf out first make the threads finish together?
 *
 * The parallel partition hands leaves to threads with one atomic increment each, so whichever leaf is
 * picked up last decides when the phase ends. Greedy self-scheduling in an arbitrary order has a
 * makespan of at most (2 - 1/P) times optimal; longest-processing-time-first tightens that to
 * (4/3 - 1/(3P)). The leaf sizes are known exactly by then, so the ordering costs one heapsort over
 * at most 256 indices.
 *
 * Both variants are the SAME compiled code, switched by an environment variable the engine reads once
 * per partition call, so this A/B carries no code-layout bias (methodology rule 3) and is measured with
 * the paired harness. Run `make pairprobe` first to see what this box can resolve today.
 *
 * Usage: ./lptbench [pairs] [n]                                        LOGOS_THREADS=N overrides.  */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t RS = 88172645463325252ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

/* A is the baseline (leaves in value order), B is largest-first. bench_paired returns tA/tB,
 * so a number above 1.000 means largest-first is that much faster. */
static void sort_base(double *a, size_t n, int t) { unsetenv("LOGOS_LPT"); logos_sort_f64(a, n, t); }
static void sort_lpt (double *a, size_t n, int t) { setenv("LOGOS_LPT", "1", 1); logos_sort_f64(a, n, t); }

typedef void (*gen_fn)(double *, size_t);
static void g_uniform(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_pareto(double *a, size_t n)   { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); } }
static void g_expo(double *a, size_t n)     { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = -log(u) * 1e6; } }
static void g_lognorm(double *a, size_t n)  { for (size_t i = 0; i < n; i++) { double u = fr(), v = fr(); if (u < 1e-12) u = 1e-12; a[i] = exp(sqrt(-2 * log(u)) * cos(6.283185307179586 * v) * 3.0) * 1000.0; } }
static void g_bimodal(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (nr() & 1) ? fr() * 1e6 : 1e9 + fr() * 1e6; }
static void g_clustered(double *a, size_t n){ for (size_t i = 0; i < n; i++) { double c = (double)(nr() % 16) * 1e8; a[i] = c + fr() * 1e5; } }
/* one bucket deliberately fat: the case the makespan bound is about */
static void g_skewheavy(double *a, size_t n){ for (size_t i = 0; i < n; i++) a[i] = (fr() < 0.35) ? fr() * 1e6 : fr() * 1e9; }

static const struct { const char *name; gen_fn g; } SH[] = {
    { "uniform (control)", g_uniform }, { "pareto a=1.2", g_pareto }, { "exponential", g_expo },
    { "lognormal", g_lognorm },         { "bimodal", g_bimodal },     { "clustered x16", g_clustered },
    { "one fat region", g_skewheavy },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static int verify(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40; if (reps < 8) reps = 8;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8000000;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *work = malloc(n * 8);
    if (!base || !work) return 1;

    printf("leaf dispatch order: value order (baseline) vs largest-first, n=%zu, %d pairs, %d threads\n", n, reps, P);
    printf("one binary, one copy of the code, switched at runtime -- no code-layout bias.\n");
    printf("ratio > 1.000 means largest-first is faster. Compare against ./pairprobe before believing anything small.\n\n");
    printf("  %-20s %9s %9s %9s   %s\n", "shape", "ratio", "p10", "p90", "verify");

    for (int k = 0; k < NSH; k++) {
        RS = 88172645463325252ULL;
        SH[k].g(base, n);
        /* rule 1: verify both variants before timing either */
        memcpy(work, base, n * 8); sort_base(work, n, P); int ok = verify(work, n);
        memcpy(work, base, n * 8); sort_lpt (work, n, P); ok &= verify(work, n);
        double p10 = 0, p90 = 0;
        double med = bench_paired(sort_base, sort_lpt, base, work, n, P, reps, &p10, &p90);
        printf("  %-20s %9.3f %9.3f %9.3f   %s\n", SH[k].name, med, p10, p90, ok ? "ok" : "FAILED");
        fflush(stdout);
    }
    printf("\n  p10/p90 bound a single pair; the ratio column is the median over %d of them.\n", reps);
    free(base); free(work);
    return 0;
}
