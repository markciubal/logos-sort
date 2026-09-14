/* streamsbench.c — interleaved ascending streams, measured.
 *
 * Several streams, each ascending in arrival order, in value ranges separated by wide gulfs, interleaved element by
 * element: sharded ID allocators, tenant-prefixed sequences, per-sensor keys. The route finds the streams as gulfs
 * in the dispatcher's sample, checks they ascend on a small position-ordered sample, and does a stable scatter by
 * stream, which leaves every bucket already sorted. One binary, switched at runtime by LOGOS_NO_STREAMS, so there is
 * no code-layout bias; every row is verified against qsort and timed under the paired harness.
 *
 * PREDICTIONS, written before this ran:
 *   shard_ids (8 shards at rates 1/(s+1), the zoo's shape) at 1 thread 4-6x;
 *   16 equal-rate streams similar; 4 streams larger (a cheaper lookup); 60 streams smaller, perhaps 2-3x;
 *   8 streams with 1% of values perturbed smaller again, since dirty buckets fall back to the leaf;
 *   uniform, bimodal random, clustered, heavy_tail, few_distinct and sorted_runs exactly 1.00;
 *   every 14-thread row exactly 1.00, because the route is sequential only.
 *
 * SECOND ROUND, the mode lookup (./streamsbench pairs n lookup): the per-element compare against every boundary
 * against a 4096-cell table, both with the route on, one thread only. PREDICTIONS, written before it ran:
 *   streams_4 1.00-1.10 (three compares already cost about one load); shard_ids and streams_dirty 1.05-1.20;
 *   streams_16 1.15-1.35; streams_60 1.5-2.2, which would lift it from 1.2x to about 2-2.5x over the route off;
 *   every guard exactly 1.00, since they all decline at the ascent gate before the lookup runs.
 *
 * GRADED (8.4M, 20 pairs, run twice): streams_60 2.74 / 2.72 and streams_16 1.21 / 1.23, above and inside the
 *   prediction; shard_ids 1.10 / 1.04 inside; streams_4 0.81 / 0.80 and streams_dirty 0.68 / 0.66 WRONG, both losses.
 *   A -DLOGOS_TRACE build timed the table FASTER on 8 streams (lookup pass 14 vs 22 ms on the perturbed shape) and the
 *   normal build timed it slower (109 vs 101 ms for the whole sort): the trace build's codegen inverted the answer.
 *
 * THIRD ROUND: the table only from 12 streams (STREAMS_TABLE_MIN_MODES). Lookup mode now FORCES the table at every
 * count (LOGOS_STREAMS_TABLE=1), so its rows still measure table against compares. PREDICTIONS, before it ran:
 *   lookup rows repeat round two (4 about 0.8, perturbed about 0.66, 16 about 1.2, 60 about 2.7); streams_12, the
 *   threshold, 0.95-1.10; guards 1.00.
 *   off vs on: where the table no longer runs, back to round one: streams_4 2.8-3.1, shard_ids 2.5-2.9, streams_dirty
 *   2.1-2.4; where it still runs, its gain kept: streams_16 2.5-2.9, streams_60 3.0-3.4; 14 threads and guards 1.00.
 *
 * Usage: ./streamsbench [pairs] [n] [lookup]                       LOGOS_THREADS=N overrides. */
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

/* S streams at equal rates, each ascending, separated by gulfs of 2^30; perturb a fraction of values in stream 0 */
static void streams(double *a, size_t n, size_t S, double perturb) {
    size_t ctr[64] = { 0 };
    for (size_t i = 0; i < n; i++) { size_t s = (size_t)(nr() % S); a[i] = (double)s * 1073741824.0 + (double)ctr[s]++; }
    if (perturb > 0) for (size_t k = 0; k < (size_t)(perturb * (double)n); k++) { size_t i = (size_t)(nr() % n); if (a[i] < 1073741824.0) a[i] = fr() * (double)ctr[0]; }
}
static void g_shard(double *a, size_t n) {           /* chaoshunt's shard_ids_disjoint: rates 1/(s+1) */
    size_t ctr[8] = { 0 };
    for (size_t i = 0; i < n; i++) { int s = 0; double u = fr() * 2.7178, acc = 0; for (s = 0; s < 7; s++) { acc += 1.0 / (s + 1); if (u < acc) break; } a[i] = (double)s * 1073741824.0 + (double)ctr[s]++; }
}
static void g_s4(double *a, size_t n)     { streams(a, n, 4, 0); }
static void g_s12(double *a, size_t n)    { streams(a, n, 12, 0); }
static void g_s16(double *a, size_t n)    { streams(a, n, 16, 0); }
static void g_s60(double *a, size_t n)    { streams(a, n, 60, 0); }
static void g_dirty(double *a, size_t n)  { streams(a, n, 8, 0.01); }
static void g_uniform(double *a, size_t n){ for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_bimodal(double *a, size_t n){ for (size_t i = 0; i < n; i++) a[i] = ((nr() & 1) ? 1e9 : -1e9) + fr() * 1e6; }
static void g_clust(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 16) * 1e8 + fr() * 1e5; }
static void g_heavy(double *a, size_t n)  { for (size_t i = 0; i < n; i++) { double u = fr(); if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); } }
static void g_few(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }
static void g_runs(double *a, size_t n)   { size_t k = 16, rl = n / k; for (size_t i = 0; i < n; i++) a[i] = -1e9 + 2e9 * (double)(i % rl) / (double)rl; }

typedef void (*gen_fn)(double *, size_t);
static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "shard_ids",    g_shard,   "8 shards, the zoo's shape" },
    { "streams_4",    g_s4,      "4 equal streams" },
    { "streams_12",   g_s12,     "12 streams: table threshold" },
    { "streams_16",   g_s16,     "16 equal streams" },
    { "streams_60",   g_s60,     "60 streams, near the cap" },
    { "streams_dirty",g_dirty,   "8 streams, 1% perturbed" },
    { "uniform",      g_uniform, "guard" },
    { "bimodal",      g_bimodal, "guard: two gulf modes, random" },
    { "clustered",    g_clust,   "guard: 16 modes, random" },
    { "heavy_tail",   g_heavy,   "guard" },
    { "few_distinct", g_few,     "guard: counting route" },
    { "sorted_runs",  g_runs,    "guard: 16 runs, no gulfs" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static int LOOKUP = 0;       /* 0: route off vs on. 1: compare lookup vs table lookup, route on both sides */
static void sort_off(double *a, size_t n, int t) {
    unsetenv("LOGOS_STREAMS_TABLE");
    if (LOOKUP) { unsetenv("LOGOS_NO_STREAMS"); setenv("LOGOS_STREAMS_CMP", "1", 1); }
    else        { setenv("LOGOS_NO_STREAMS", "1", 1); unsetenv("LOGOS_STREAMS_CMP"); }
    logos_sort_f64(a, n, t);
}
static void sort_on(double *a, size_t n, int t)  {
    unsetenv("LOGOS_NO_STREAMS"); unsetenv("LOGOS_STREAMS_CMP");
    if (LOOKUP) setenv("LOGOS_STREAMS_TABLE", "1", 1); else unsetenv("LOGOS_STREAMS_TABLE");   /* lookup mode: table at every count */
    logos_sort_f64(a, n, t);
}

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int P = bench_threads(logos_num_cpus());
    LOOKUP = argc > 3 && strcmp(argv[3], "lookup") == 0;
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;

    if (LOOKUP) printf("interleaved streams: compare lookup vs table lookup, route on, n=%zu, %d pairs, 1 thread, one binary (LOGOS_STREAMS_CMP)\n", n, pairs);
    else printf("interleaved streams: route off vs on, n=%zu, %d pairs, threads 1 and %d, one binary (LOGOS_NO_STREAMS)\n", n, pairs, P);
    printf("ratio > 1.000 means the route is that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-13s %2s %9s %9s %9s  %-30s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NSH; k++) {
        RS = 11 + (uint64_t)k;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_off(w, n, 1); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, 1);  ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_on(w, n, P);  ok &= memcmp(w, ref, n * 8) == 0;
        int tc[2] = { 1, P };
        for (int ti = 0; ti < (LOOKUP ? 1 : 2); ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_off, sort_on, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-13s %2d %9.3f %9.3f %9.3f  %-30s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    free(base); free(w); free(ref);
    return 0;
}
