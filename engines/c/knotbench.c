/* knotbench.c — the knot route, measured: comparisons only where strands cross.
 *
 * Runs as strands, value-range overlaps as crossings (see knot_plan in logos_psort.c). The shipped relocation (J5)
 * merges every element of a chain of overlapping runs and declines the array when chains hold more than half of it.
 * The knot route cuts strands at their neighbours' endpoints, copies what lies alone, and merges only what crosses.
 * One binary, switched at runtime by LOGOS_NO_KNOT (off = J5 exactly as shipped), so there is no code-layout bias
 * between the halves of a pair; every row runs under the paired harness and is verified against qsort first.
 *
 * The target shape is a STAIRCASE: sorted blocks whose value ranges each overlap the next (time-series shards with
 * clock skew, rotated logs, sorted files whose boundaries overlap). One chain covers everything, so J5 declines and
 * the array pays for a full sort. stairs_xNN has NN% of its elements inside crossings; block b covers [b, b+1+o)
 * block-widths of value, which puts 2o/(1+o) of the elements in a crossing.
 *
 * PREDICTIONS, written before this ran (ratio = route off / route on at 8.4M; > 1 means the knot route is faster):
 *   block_perm64    1.00 at both thread counts (no crossings: the plan is J5's, piece for piece)
 *   block_overlap   1 thread 1.10-1.25, 14 threads 1.03-1.15 (J5 merges the 25% in chains, the knot route the 2.5% that cross)
 *   stairs_x10      1 thread 2.2-3.5,   14 threads 1.5-2.2
 *   stairs_x20      1 thread 2.0-3.2,   14 threads 1.4-2.0
 *   stairs_x40      1 thread 1.8-2.8,   14 threads 1.1-1.6
 *   stairs_x60, x80, x100 and triple: exactly 1.00 under the default gate (cost above n/2: declined).
 *     With the gate lifted (third argument `lift`, LOGOS_KNOT_MAX=8):
 *     x60  1 thread 1.6-2.5, 14 threads 0.95-1.3       x80   1 thread 1.4-2.2, 14 threads 0.85-1.15
 *     x100 1 thread 1.3-2.0, 14 threads 0.75-1.05      triple (three strands live at once) 1 thread 0.9-1.5, 14 threads 0.6-0.95
 *     So the break-even at 14 threads is predicted between 60% and 100% of the array inside crossings, and at one
 *     thread the route is predicted to win across the whole two-strand ladder (its fallback is a full sequential sort).
 *   stairs_ord_x20  1 thread 2.0-3.2; 14 threads 1.00: blocks in value order look nearly sorted, and the near-sorted
 *                   route is asked before relocation, so the knot route never sees it
 *   zigzag_x20      1 thread 2.0-3.2, 14 threads 1.4-2.0 (every other block descends, so the near-sorted route declines)
 *   shards14_x20    1 thread 2.0-3.2, 14 threads 1.3-1.9 (fourteen strands, few large crossings: cut into grains)
 *   uniform, few_runs, segmented, near_1pct: 1.00 (0.97-1.03) everywhere. few_runs and segmented are fully
 *                   interleaved (every element crosses), which the early gates must still refuse at once.
 *
 * GRADED (8.4M, 40 pairs, floor 3.2%; full tables in CHANGELOG and FINDINGS 3ac):
 *   Right: block_perm64 1.000 / 0.986; block_overlap 1.099 / 1.131; stairs_ord_x20 at 14 threads 1.02; every guard 0.99-1.06.
 *   One thread, too LOW everywhere: x10 4.44, x20 4.05, x40 3.42, x60 2.93, x80 2.62, x100 2.64, triple 2.58, zigzag 3.73.
 *   shards14 1.85 (3.5-3.7 under another shuffle: the shipped fallback is the run merge at 8 fused runs, the leaf at 9).
 *   Fourteen threads, too HIGH on the light rows: x10 1.20, x20 1.32, x40 1.35, zigzag 1.05, shards14 1.09.
 *   The break-even, wrong in kind: lifted, x60 1.44, x80 1.24, x100 1.52, triple 1.12 at 14 threads; it loses from 3-4 live strands
 *   (quad 0.82, few_runs 0.82, segmented 0.77). And "exactly 1.00 past the gate" was 0.92-0.96: a decline comes after a full run scan.
 *   So the gates are measured: 1.0n parallel, 3.0n sequential, 1.0n sequential with at most MERGE_MAX_RUNS runs.
 *   CAUTION, the 14-thread column is against the SHIPPED engine, whose relocation scans a shuffled staircase and then declines.
 *   Against the pure partition (-DLOGOS_NO_RELOCATE copy, both orientations) the route is 1.8-4.1x at one thread and a TIE at 14
 *   (0.96-1.11; three strands 0.89, hence the 1.0n parallel gate). quad and k8 were added after the first run.
 *
 * Usage: ./knotbench [pairs] [n] [lift|once|-] [name]   LOGOS_THREADS=N overrides the thread count.
 *   name: run only the shapes whose name contains it (how a borderline row gets more pairs cheaply).
 *   lift: the "on" side also sets LOGOS_KNOT_MAX=8, so the cost gate never declines (finds the break-even).
 *   once: no timing; every shape is sorted once at 1 and P threads with the route on. Link against a -DLOGOS_TRACE
 *         engine (make knottrace) to read which route ran. A trace build says WHETHER a path runs, never how fast. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

/* nb sorted blocks. Block b covers values [b, b+1+o) in units of the block length, so consecutive blocks overlap by o
 * of a block's range. shuffle: block order in the array is random. zig: every other block (by position) descends. */
static void stairs(double *a, size_t n, size_t nb, double o, int shuffle, int zig) {
    size_t blk = (n + nb - 1) / nb; size_t *perm = malloc(nb * sizeof *perm);
    if (!perm) return;
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    if (shuffle) for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) {
        size_t b = perm[i / blk], j = i % blk;
        if (zig && ((i / blk) & 1)) j = blk - 1 - j;
        a[i] = (double)b * (double)blk + (double)j * (1.0 + o) + fr() * 0.5;
    }
    free(perm);
}
/* J5's own shapes (j5bench.c): shuffled blocks, disjoint, or with every value-block 1 mod 8 starting 10% inside its predecessor */
static void blocks(double *a, size_t n, size_t nb, int overlap_every) {
    size_t blk = (n + nb - 1) / nb; size_t *perm = malloc(nb * sizeof *perm);
    if (!perm) return;
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
        if (overlap_every && (b % (size_t)overlap_every) == 1) base -= (double)blk * 0.1;
        a[i] = base + (double)(i % blk) + fr() * 0.5; }
    free(perm);
}
#define XO(x) ((x) / (2.0 - (x)))                  /* overlap o that puts a share x of the elements inside crossings */
static void g_bp64(double *a, size_t n)    { blocks(a, n, 64, 0); }
static void g_bover(double *a, size_t n)   { blocks(a, n, 64, 8); }
static void g_x10(double *a, size_t n)     { stairs(a, n, 64, XO(0.10), 1, 0); }
static void g_x20(double *a, size_t n)     { stairs(a, n, 64, XO(0.20), 1, 0); }
static void g_x40(double *a, size_t n)     { stairs(a, n, 64, XO(0.40), 1, 0); }
static void g_x60(double *a, size_t n)     { stairs(a, n, 64, XO(0.60), 1, 0); }
static void g_x80(double *a, size_t n)     { stairs(a, n, 64, XO(0.80), 1, 0); }
static void g_x100(double *a, size_t n)    { stairs(a, n, 64, 1.0, 1, 0); }
static void g_triple(double *a, size_t n)  { stairs(a, n, 64, 1.5, 1, 0); }
static void g_quad(double *a, size_t n)    { stairs(a, n, 64, 2.5, 1, 0); }   /* 3-4 strands live everywhere: two merge levels, cost 2n */
static void g_k8(double *a, size_t n)      { stairs(a, n, 64, 7.0, 1, 0); }   /* 8 strands live: three merge levels, cost 3n */
static void g_ord20(double *a, size_t n)   { stairs(a, n, 64, XO(0.20), 0, 0); }
static void g_zig20(double *a, size_t n)   { stairs(a, n, 64, XO(0.20), 0, 1); }
static void g_sh14(double *a, size_t n)    { stairs(a, n, 14, XO(0.20), 1, 0); }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_fewruns(double *a, size_t n) { size_t blk = n / 4;  for (size_t i = 0; i < n; i++) a[i] = (double)(i % blk) * 4.0 + (double)(i / blk) + fr() * 0.5; }
static void g_segm(double *a, size_t n)    { size_t blk = n / 64; for (size_t i = 0; i < n; i++) a[i] = (double)(i % blk) * 64.0 + (double)(i / blk) + fr() * 0.5; }
static void g_near(double *a, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
    for (size_t i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}

/* 2026-09-21: ONE sparse strand spanning every value, over 32 shuffled disjoint dense blocks (a late column, a sparse index
   over dense pages). Every block's interval is a two-strand crossing of a few sparse elements and a whole block, so the
   branch-free merge touches every element where a galloping one would copy the block between sparse points. tw: the sparse
   strand lies descending (the twisted gallop). every: one sparse element per `every` dense ones. */
static void sparse_over(double *a, size_t n, size_t every, int tw) {
    size_t g = n / (every + 1); if (g < 2) g = 2;
    blocks(a + g, n - g, 32, 0);
    double span = (double)(n - g);
    for (size_t i = 0; i < g; i++) a[tw ? g - 1 - i : i] = ((double)i + 0.5) * span / (double)g + 0.123;
}
static void g_sp64(double *a, size_t n)   { sparse_over(a, n, 64, 0); }
static void g_sp512(double *a, size_t n)  { sparse_over(a, n, 512, 0); }
static void g_sp4096(double *a, size_t n) { sparse_over(a, n, 4096, 0); }
static void g_sp64tw(double *a, size_t n) { sparse_over(a, n, 64, 1); }
static void g_sp8(double *a, size_t n)    { sparse_over(a, n, 8, 0); }

typedef void (*gen_fn)(double *, size_t);
static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "block_perm64",   g_bp64,    "J5's shape: no crossings, same plan" },
    { "block_overlap",  g_bover,   "J5's overlap shape: 25% in chains, 2.5% crosses" },
    { "stairs_x10",     g_x10,     "64 shuffled blocks, 10% of elements cross" },
    { "stairs_x20",     g_x20,     "20% cross" },
    { "stairs_x40",     g_x40,     "40% cross" },
    { "stairs_x60",     g_x60,     "60%: past the default gate" },
    { "stairs_x80",     g_x80,     "80%" },
    { "stairs_x100",    g_x100,    "every element in a two-strand crossing" },
    { "triple",         g_triple,  "three strands live at once (k = 3 merges)" },
    { "quad",           g_quad,    "3-4 strands live everywhere (cost 2n)" },
    { "k8",             g_k8,      "8 strands live everywhere (cost 3n)" },
    { "stairs_ord_x20", g_ord20,   "blocks in value order, 20% cross" },
    { "zigzag_x20",     g_zig20,   "in order, every other block descending" },
    { "shards14_x20",   g_sh14,    "14 shuffled strands: few, large crossings" },
    { "sparse_1in8",    g_sp8,     "a strand of 1 in 9 over 32 dense blocks" },
    { "sparse_1in64",   g_sp64,    "a strand of 1 in 65 over 32 dense blocks" },
    { "sparse_1in64tw", g_sp64tw,  "the same, the sparse strand descending" },
    { "sparse_1in512",  g_sp512,   "1 in 513" },
    { "sparse_1in4096", g_sp4096,  "1 in 4097: a few thousand late records" },
    { "uniform",        g_uniform, "guard" },
    { "few_runs",       g_fewruns, "guard: 4 fully interleaved runs" },
    { "segmented",      g_segm,    "guard: 64 fully interleaved runs" },
    { "near_1pct",      g_near,    "guard" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static int g_lift = 0;
/* mode `ceiling` (2026-09-21): a sparse shape Z against its twin Z', the same array with the sparse strand's values moved BELOW
   every block, so the same runs lie in the same places and nothing crosses. t(Z) / t(Z') is what the two-strand merges cost over
   plain copies: the most a galloping merge could save. Both sides as shipped. */
static double pair2(const double *A, const double *B, double *w, size_t n, int P, int reps, double *p10, double *p90, int *ok) {
    double *r = malloc((size_t)reps * sizeof(double)); *ok = r != NULL; if (!r) return 0;
    for (int i = 0; i < reps; i++) { double t2[2];
        for (int k = 0; k < 2; k++) { int side = (i & 1) ? 1 - k : k; memcpy(w, side ? B : A, n * 8);
            struct timespec s0, s1; clock_gettime(CLOCK_MONOTONIC, &s0); logos_sort_f64(w, n, P); clock_gettime(CLOCK_MONOTONIC, &s1);
            t2[side] = (double)(s1.tv_sec - s0.tv_sec) * 1e3 + (double)(s1.tv_nsec - s0.tv_nsec) / 1e6;
            for (size_t q = 1; q < n; q++) if (w[q] < w[q - 1]) { *ok = 0; break; } }
        r[i] = t2[0] / t2[1]; }
    qsort(r, (size_t)reps, sizeof(double), cmp_d);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]); *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; free(r); return med;
}
static int g_gallop_ab = 0;       /* mode `gallop`: the OFF side is the knot route with the branch-free merge alone (LOGOS_NO_KNOT_GALLOP=1) */
static void sort_off(double *a, size_t n, int t) {
    if (g_gallop_ab) { unsetenv("LOGOS_NO_KNOT"); setenv("LOGOS_NO_KNOT_GALLOP", "1", 1); logos_sort_f64(a, n, t); unsetenv("LOGOS_NO_KNOT_GALLOP"); return; }
    setenv("LOGOS_NO_KNOT", "1", 1); logos_sort_f64(a, n, t); }
static void sort_on(double *a, size_t n, int t)  { unsetenv("LOGOS_NO_KNOT"); if (g_lift) setenv("LOGOS_KNOT_MAX", "8", 1); else unsetenv("LOGOS_KNOT_MAX"); logos_sort_f64(a, n, t); }

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int once = argc > 3 && !strcmp(argv[3], "once");
    const char *only = argc > 4 ? argv[4] : NULL;               /* run only shapes whose name contains this */
    g_lift = argc > 3 && !strcmp(argv[3], "lift");
    g_gallop_ab = argc > 3 && !strcmp(argv[3], "gallop");
    (void)bench_threads_source;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;

    if (argc > 3 && !strcmp(argv[3], "ceiling")) {
        const size_t EV[] = { 8, 64, 64, 512, 4096 }; const int TW[] = { 0, 0, 1, 0, 0 };
        printf("CEILING of a galloping two-strand merge, n=%zu, %d pairs: t(sparse strand over 32 dense blocks) / t(the same, the sparse values\n"
               "moved below every block: same runs, nothing crosses). Above 1 = what the merges cost over copies.\n\n", n, pairs);
        printf("  %-22s | %-24s | %-24s\n", "shape", "1 thread", "P threads");
        for (int k = 0; k < 5; k++) {
            RS = 77 + (uint64_t)k; sparse_over(base, n, EV[k], TW[k]); memcpy(ref, base, n * 8);
            size_t g = n / (EV[k] + 1); if (g < 2) g = 2;
            for (size_t i = 0; i < g; i++) ref[i] -= (double)n * 2.0;          /* below every block: a lone piece, copied */
            double l1, h1, lp, hp; int o1, op;
            double c1 = pair2(base, ref, w, n, 1, pairs, &l1, &h1, &o1), cp = pair2(base, ref, w, n, P, pairs, &lp, &hp, &op);
            char nm[64]; snprintf(nm, sizeof nm, "1 in %zu%s", EV[k] + 1, TW[k] ? ", descending" : "");
            printf("  %-22s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", nm, c1, l1, h1, o1 ? "  " : " X", cp, lp, hp, op ? "  " : " X"); fflush(stdout);
        }
        free(base); free(w); free(ref); return 0;
    }
    if (once) {
        for (int k = 0; k < NSH; k++) {
            if (only && !strstr(SH[k].name, only)) continue;
            RS = 1 + (uint64_t)k; SH[k].g(base, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                fprintf(stderr, "==== %s, %d thread%s%s\n", SH[k].name, tc[ti], tc[ti] > 1 ? "s" : "", g_lift ? " (gate lifted)" : "");
                memcpy(w, base, n * 8); sort_on(w, n, tc[ti]);
            }
        }
        free(base); free(w); free(ref);
        return 0;
    }

    if (g_gallop_ab) printf("THE GALLOPING TWO-STRAND MERGE, A/B: the knot route with the branch-free merge alone (LOGOS_NO_KNOT_GALLOP=1) vs shipped. One binary.\n");
    printf("knot route: off (J5 as shipped) vs on, n=%zu, %d pairs, threads 1 and %d, one binary (LOGOS_NO_KNOT)%s\n", n, pairs, P,
           g_lift ? ", COST GATE LIFTED (LOGOS_KNOT_MAX=8)" : "");
    printf("ratio > 1.000 means the knot route is that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-15s %2s %9s %9s %9s  %-48s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NSH; k++) {
        if (only && !strstr(SH[k].name, only)) continue;
        RS = 1 + (uint64_t)k;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_off(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, P);  ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, 1);  ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_off(w, n, 1); ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_off, sort_on, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-15s %2d %9.3f %9.3f %9.3f  %-48s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    unsetenv("LOGOS_NO_KNOT"); unsetenv("LOGOS_KNOT_MAX");
    free(base); free(w); free(ref);
    return 0;
}
