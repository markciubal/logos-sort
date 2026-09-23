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

#define DENSE_SPAN_MULT_BENCH 16     /* the engine's DENSE_SPAN_MULT, for the header line */
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

/* ---- mode `span` (2026-09-21): the dense-integer bitmap's span budget, laddered. n DISTINCT integers spread over s x n (one per
 * stride of s, jittered inside it, then shuffled), sorted at ONE thread (the route is sequential-only) with the shipped budget
 * (DENSE_SPAN_MULT, 4n) against the gate lifted to 64n (LOGOS_DENSE_SPAN=64). Rows up to s = 4 take the route on both sides and
 * are the null; above it the lifted side takes the bitmap and the shipped side the leaf, so each row is the bitmap against the
 * leaf at that span, and the ladder says where the budget should be. Lift the gate, measure the ladder, then set it.
 * PREDICTION, written first (8.4M): s = 6 1.4-1.8x, s = 8 1.3-1.7x, s = 12 1.1-1.4x, s = 16 1.0-1.3x (the map is 16 MB, most of
 * the last-level cache), s = 32 0.7-1.0 (32 MB, past it). At 1M everything fits, so the win should last to s = 32. */
static const char *g_sp_lift = "256";   /* the lifted side's budget; ./mcbench P N span <k> sets it */
static const char *g_sp_ship = NULL;  /* ./mcbench P N span <lift> <A>: A, when given, is the budget of the first side (e.g. the old 4) */
static void sp_ship(double *a, size_t n, int t) { if (g_sp_ship) setenv("LOGOS_DENSE_SPAN", g_sp_ship, 1); else unsetenv("LOGOS_DENSE_SPAN"); logos_sort_f64(a, n, t); unsetenv("LOGOS_DENSE_SPAN"); }
static void sp_lift(double *a, size_t n, int t) { if (!strcmp(g_sp_lift, "-")) { unsetenv("LOGOS_DENSE_SPAN"); logos_sort_f64(a, n, t); return; } setenv("LOGOS_DENSE_SPAN", g_sp_lift, 1); logos_sort_f64(a, n, t); unsetenv("LOGOS_DENSE_SPAN"); }
/* guards for the widened gate: n draws WITH repeats over s x n (abandoned at the first repeat, which the birthday bound puts early),
   and a clean spread with ONE repeat planted in the middle (abandoned after half a pass: the worst case the gate lets in) */
static void g_withrep(double *a, size_t n, size_t s) { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % (s * n)); }
static void g_spread(double *a, size_t n, size_t s);
static void g_planted(double *a, size_t n, size_t s) { g_spread(a, n, s); size_t x = n / 2, y = n / 3; a[x] = a[y]; }
static void g_spread(double *a, size_t n, size_t s) {
    for (size_t i = 0; i < n; i++) a[i] = (double)(i * s + (s > 1 ? nr() % s : 0));
    for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); double x = a[i]; a[i] = a[j]; a[j] = x; }
}

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 24; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    if (argc > 3 && !strcmp(argv[3], "span")) {
        double *b0 = malloc(n * 8), *w0 = malloc(n * 8), *r0 = malloc(n * 8); if (!b0 || !w0 || !r0) return 1;
        if (argc > 4) g_sp_lift = argv[4];
        if (argc > 5) { g_sp_ship = argv[5]; printf("(first side: LOGOS_DENSE_SPAN=%s; second side: %s)\n", g_sp_ship, strcmp(g_sp_lift, "-") ? g_sp_lift : "shipped"); }
        const size_t S[] = { 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128 };
        printf("DENSE-INTEGER BITMAP, SPAN LADDER, n = %zu, %d pairs, ONE thread: t(shipped budget, %dn) / t(gate lifted to %sn). Above 1 = the bitmap wins\n"
               "at that span. Rows up to 4n take the route on both sides: they are the null.\n\n", n, pairs, (int)DENSE_SPAN_MULT_BENCH, g_sp_lift);
        printf("  %-10s %9s %9s %9s  %s\n", "span", "ratio", "p10", "p90", "verify");
        for (size_t k = 0; k < sizeof S / sizeof *S; k++) {
            RS = 5 + k; g_spread(b0, n, S[k]); memcpy(r0, b0, n * 8); qsort(r0, n, 8, cmp_d);
            int ok = 1; memcpy(w0, b0, n * 8); sp_ship(w0, n, 1); ok &= !memcmp(w0, r0, n * 8); memcpy(w0, b0, n * 8); sp_lift(w0, n, 1); ok &= !memcmp(w0, r0, n * 8);
            double p10 = 0, p90 = 0, med = bench_paired(sp_ship, sp_lift, b0, w0, n, 1, pairs, &p10, &p90);
            printf("  %2zun       %9.3f %9.3f %9.3f  %s\n", S[k], med, p10, p90, ok ? "ok" : "FAILED"); fflush(stdout);
        }
        struct { const char *nm; int kind; size_t s; } GD[] = { { "repeats 16n", 1, 16 }, { "repeats 64n", 1, 64 }, { "planted 16n", 2, 16 },
            { "planted 64n", 2, 64 }, { "int_wide", 3, 0 }, { "uniform", 4, 0 } };
        printf("\n  guards: the gate lets the first four in when lifted and the bitmap must give them back (repeats early, planted after half a pass)\n");
        for (int k = 0; k < 6; k++) {
            RS = 50 + (uint64_t)k;
            if (GD[k].kind == 1) g_withrep(b0, n, GD[k].s); else if (GD[k].kind == 2) g_planted(b0, n, GD[k].s);
            else if (GD[k].kind == 3) { for (size_t i = 0; i < n; i++) b0[i] = (double)(int32_t)nr(); } else { for (size_t i = 0; i < n; i++) b0[i] = (double)(nr() >> 11) / 9007199254740992.0; }
            memcpy(r0, b0, n * 8); qsort(r0, n, 8, cmp_d);
            int ok = 1; memcpy(w0, b0, n * 8); sp_ship(w0, n, 1); ok &= !memcmp(w0, r0, n * 8); memcpy(w0, b0, n * 8); sp_lift(w0, n, 1); ok &= !memcmp(w0, r0, n * 8);
            double p10 = 0, p90 = 0, med = bench_paired(sp_ship, sp_lift, b0, w0, n, 1, pairs, &p10, &p90);
            printf("  %-12s %8.3f %9.3f %9.3f  %s\n", GD[k].nm, med, p10, p90, ok ? "ok" : "FAILED"); fflush(stdout);
        }
        free(b0); free(w0); free(r0); return 0;
    }
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
