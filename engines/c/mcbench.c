/* mcbench.c — the counting routes: the lattice slot budget, and the hash route's serial tail.
 *
 * Two changes are measured here, both aimed at what the overnight chaos map found: the
 * moderate-cardinality route sitting 3-7x off a hand-routed ceiling on 50-65k-distinct data with the
 * workers idle for most of the wall.
 *
 *  1. The lattice gate. Its slot ceiling was a flat 65536 and a table that did not fit was declined
 *     outright, so a PERFECT 60000-value lattice fell all the way to the hash route because its
 *     eighth-of-span margin pushed 60000 slots to 75128. The ceiling is now a memory budget that
 *     scales with the thread count, and an oversized table shrinks its margin before giving up.
 *     `LOGOS_LATTICE_OLD=1` restores the old rule, so ONE binary measures both and there is no
 *     code-layout bias between them.
 *  2. The hash route's tail. Its merge built one global hash by re-inserting every entry of every
 *     thread's table, single-threaded and rehashing as it grew, and then sorted the result with libc
 *     qsort. Traced on a 60k lattice: 221 ms of merge and 147 ms of qsort inside a 2268 ms sort.
 *     Both are replaced by concatenate, radix-sort by key, fold, which is what the low-cardinality
 *     route already did. That one is not a toggle; it is measured against the tree before it.
 *
 * Every row is verified against qsort before it is timed, and every timing runs under the paired
 * harness, so machine drift cancels inside a pair. Run ./pairprobe first for the day's floor.
 *
 * Usage: ./mcbench [pairs] [n]                                     LOGOS_THREADS=N overrides. */
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

typedef void (*gen_fn)(double *, size_t);
/* the shapes that reach the counting routes, either side of the old 65536-slot ceiling */
static void g_lat30k(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 1.5; }
static void g_lat60k(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 60000) * 1.5; }
static void g_lat120k(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 120000) * 1.5; }
static void g_mant16(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 65536) / 64.0; }
static void g_zipf(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); }
static void g_few5(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }
static void g_quant(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; }
/* guards: these never reach the counting routes and must not move */
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_heavy(double *a, size_t n)   { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); } }

static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "lattice_30k",  g_lat30k,  "fits the old ceiling too" },
    { "lattice_60k",  g_lat60k,  "60000 slots, margin pushed it to 75128" },
    { "lattice_120k", g_lat120k, "past the old ceiling by span alone" },
    { "mantissa_64k", g_mant16,  "65536 slots: over by the margin alone" },
    { "zipf",         g_zipf,    "hash route, long tail" },
    { "few_5",        g_few5,    "guard: low-cardinality route" },
    { "quantized",    g_quant,   "guard: small lattice, 4096 slots" },
    { "uniform",      g_uniform, "guard: partition route" },
    { "heavy_tail",   g_heavy,   "guard: partition route" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

/* bench_paired wants two sort functions; the toggle is read by the engine per call, so the two
 * halves of a pair differ only in one getenv. */
static void sort_old(double *a, size_t n, int t) { setenv("LOGOS_LATTICE_OLD", "1", 1); logos_sort_f64(a, n, t); }
static void sort_new(double *a, size_t n, int t) { unsetenv("LOGOS_LATTICE_OLD");      logos_sort_f64(a, n, t); }
/* section 2: the hash route's tail, on shapes that still reach it */
static void mc_old(double *a, size_t n, int t) { setenv("LOGOS_MC_QSORT", "1", 1); logos_sort_f64(a, n, t); }
static void mc_new(double *a, size_t n, int t) { unsetenv("LOGOS_MC_QSORT");      logos_sort_f64(a, n, t); }

/* Values that are NOT evenly spaced, so the lattice route cannot take them and the hash route must:
 * d distinct doubles at random positions, drawn once and then sampled with repeats. */
static void gen_hash_d(double *a, size_t n, size_t d) {
    double *tab = malloc(d * sizeof(double));
    if (!tab) return;
    for (size_t i = 0; i < d; i++) tab[i] = fr() * 1e9;
    for (size_t i = 0; i < n; i++) a[i] = tab[nr() % d];
    free(tab);
}
static void g_hash5k(double *a, size_t n)  { gen_hash_d(a, n, 5000); }
static void g_hash30k(double *a, size_t n) { gen_hash_d(a, n, 30000); }
static void g_hash60k(double *a, size_t n) { gen_hash_d(a, n, 60000); }
static const struct { const char *name; gen_fn g; } MC[] = {
    { "hash_5k", g_hash5k }, { "hash_30k", g_hash30k }, { "hash_60k", g_hash60k }, { "zipf", g_zipf },
};
#define NMC ((int)(sizeof MC / sizeof MC[0]))

/* ---- section 3: dense consecutive integers, the single-thread gap the chaos map found ---- */
static void dn_old(double *a, size_t n, int t) { setenv("LOGOS_NO_DENSE", "1", 1); logos_sort_f64(a, n, t); }
static void dn_new(double *a, size_t n, int t) { unsetenv("LOGOS_NO_DENSE");       logos_sort_f64(a, n, t); }

/* All permutations of 0..n-1, so the sorted answer is "every integer is present". n is a power of two
 * in the default run, which is what makes the multiplicative map a permutation. */
static void g_gray(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)(i ^ (i >> 1)); }
static void g_hashord(double *a, size_t n){ for (size_t i = 0; i < n; i++) a[i] = (double)((i * 2654435761u) & (n - 1)); }
static void g_shard(double *a, size_t n)  {          /* 8 consumers taking 1024-id blocks round robin */
    size_t blk = 1024, nb = n / blk, w = 0;
    for (size_t c = 0; c < 8; c++) for (size_t b = c; b < nb; b += 8)
        for (size_t j = 0; j < blk && w < n; j++) a[w++] = (double)(b * blk + j);
    while (w < n) a[w++] = (double)w;
}
static void g_sparse(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (double)(i * 3); }   /* span 3n: declines */
static void g_dup1(double *a, size_t n)   { g_gray(a, n); a[n / 2] = a[n / 3]; }                        /* one repeat: declines */
static const struct { const char *name; gen_fn g; const char *note; } DN[] = {
    { "gray_code",   g_gray,    "permutation of 0..n-1" },
    { "hash_order",  g_hashord, "permutation of 0..n-1" },
    { "shard_ids",   g_shard,   "1024-id blocks, 8 consumers" },
    { "span_3n",     g_sparse,  "guard: span 3n, still dense enough" },
    { "one_repeat",  g_dup1,    "guard: must decline, one value twice" },
    { "uniform",     g_uniform, "guard: not integral, never gated in" },
};
#define NDN ((int)(sizeof DN / sizeof DN[0]))

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 24; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;

    printf("counting routes: old flat 65536-slot ceiling vs a thread-scaled memory budget\n");
    printf("n=%zu, %d pairs, threads 1 and %d, one binary switched by LOGOS_LATTICE_OLD\n", n, pairs, P);
    printf("ratio > 1.000 means the budget is that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-14s %2s %9s %9s %9s  %-34s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");

    for (int k = 0; k < NSH; k++) {
        RS = 1;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_old(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_new(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_new(w, n, 1); ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_old, sort_new, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-14s %2d %9.3f %9.3f %9.3f  %-34s %s\n", SH[k].name, tc[ti], med, p10, p90,
                   ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }

    printf("\n\nthe hash route's tail: serial global hash + libc qsort, vs concatenate/radix/fold\n");
    printf("ratio > 1.000 means the new tail is that much faster. These shapes are NOT lattices,\n");
    printf("so they reach the hash route in both halves of every pair.\n\n");
    printf("  %-14s %2s %9s %9s %9s  %s\n", "shape", "T", "ratio", "p10", "p90", "verify");
    for (int k = 0; k < NMC; k++) {
        RS = 1;
        MC[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); mc_old(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); mc_new(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(mc_old, mc_new, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-14s %2d %9.3f %9.3f %9.3f  %s\n", MC[k].name, tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }

    printf("\n\ndense consecutive integers: presence bits vs the general path\n");
    printf("ratio > 1.000 means the dense route is that much faster. LOGOS_NO_DENSE switches it off,\n");
    printf("same binary. The last three rows must sit at 1.000: the route has to decline them.\n\n");
    printf("  %-14s %2s %9s %9s %9s  %-30s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NDN; k++) {
        RS = 1;
        DN[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); dn_old(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); dn_new(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); dn_new(w, n, 1); ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(dn_old, dn_new, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-14s %2d %9.3f %9.3f %9.3f  %-30s %s\n", DN[k].name, tc[ti], med, p10, p90,
                   ti ? "" : DN[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    free(base); free(w); free(ref);
    return 0;
}
