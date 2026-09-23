/* reachbench.c -- the short-reach route, measured: sort index blocks where they lie, repair only the seams.
 *
 * Data whose elements all sit near their final place (an event log with bounded latency, a series shuffled inside small
 * windows, anything jittered around its own order) does not need a scatter: reach_try_f64 in logos_psort.c sorts index
 * blocks in place with the engine's own leaf and merges the few elements near each block boundary. One binary, switched
 * at runtime by LOGOS_NO_REACH, so there is no code-layout bias between the halves of a pair; every row runs under the
 * paired harness (bench_common.h) and is verified against qsort first. The ceilings that endorsed building it are in
 * reachhunt.cpp (make reachhunt).
 *
 * PREDICTIONS, written 2026-09-20 before this ran (ratio = route off / route on at 8.4M; > 1 means the route is faster).
 * The ceilings were 1.28-1.37 at one thread and 1.18-1.24 at 14, through the public API with two extra thread spawns.
 *   latency_4096     1 thread 1.10-1.25, 14 threads 1.15-1.30
 *   window_2048      1 thread 1.15-1.30, 14 threads 1.20-1.40
 *   jitter_1000      1 thread 1.20-1.45, 14 threads 1.15-1.30
 *   latency_512      like latency_4096 (the block size does not follow the reach below 8192)
 *   window_64        added after the first run, to grade TRUTH_TABLE J6 (a d-sized heap, predicted 1.85x at one thread on local shuffles within 64)
 *   ticks_dup        1 thread 2-3, 14 threads 1.3-1.8 (one-second ticks: inside a block the leaf finds ~30 distinct integers and counts)
 *   one_stray        1 thread 0.95-1.10, 14 threads 0.70-0.90: a single late event is found by the block sorts, not by the probe, and
 *                    the buffered stray merge is two more passes
 *   strays_1e-5 .. strays_1e-3, sentinel_zeros: 0.97-1.00 everywhere: eight probe blocks see the strays before anything moves
 *   strays_1e-2      exactly 1.00: the ladder's first rung already fails
 *   reach_40000      1.00: the reach needs blocks above the cap, declined from the sample
 *   guards (uniform, near_1pct, coarse_groups, regime_switch, stairs_x20, block_perm64, heavy_tail, few_distinct): 1.00 within the floor
 * GRADED (8.4M, 30 pairs, null test of the day 6.0%; tables in CHANGELOG and FINDINGS 3af):
 *   targets at one thread TOO LOW everywhere: 1.91, 2.22, 2.05, 2.38 (the ceilings ran the general leaf; the route's flash kernel
 *   is 1.26-1.81x faster than the AVX2 quicksort on these blocks). At 14 threads right: 1.31, 1.38, 1.33, 1.49.
 *   ticks_dup right at one thread (2.80), wrong at 14 (1.02: a counting route wins the dispatch first).
 *   one_stray 1.81 / 1.14 against a predicted 0.95-1.10 / 0.70-0.90: the prediction was for a buffered merge of the strays, which
 *   measured +36 ms for ONE stray and was replaced by an in-place shift pass.
 *   strays 'everywhere 0.97-1.00': right at 14 threads (0.96-1.04, eight probe blocks decline), wrong at one, where strays are
 *   now put back: 1.23-2.35. reach_40000 and the eight guards 0.94-1.03: right.
 *   Rows added after the first run: window_64, jitter_2/8/30, zeros_8/84/840, late_84, and the five *_strays shapes.
 *
 * Usage: ./reachbench [pairs] [n] [once|-] [name]   LOGOS_THREADS=N overrides the thread count.
 *   name: run only the shapes whose name contains it.  once: no timing, every shape sorted once at 1 and P threads with the
 *   route on; link against a -DLOGOS_TRACE engine (make reachtrace) to read what the gate decided. */
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
typedef struct { double arr, send; } ev_t;
static int cmp_ev(const void *x, const void *y) { double a = ((const ev_t *)x)->arr, b = ((const ev_t *)y)->arr; return (a > b) - (a < b); }

/* one event per millisecond, delivered after a latency uniform in [0, L) ms, stored in arrival order: the value is the send time */
static void latency(double *a, size_t n, double L) {
    ev_t *ev = malloc(n * sizeof *ev); if (!ev) return;
    for (size_t i = 0; i < n; i++) { double s = (double)i + fr(); ev[i].send = s; ev[i].arr = s + fr() * L; }
    qsort(ev, n, sizeof *ev, cmp_ev); for (size_t i = 0; i < n; i++) a[i] = ev[i].send; free(ev);
}
static void swaps(double *a, size_t n, size_t k) { for (size_t q = 0; q < k; q++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } }
static void g_lat4096(double *a, size_t n) { latency(a, n, 4096.0); }
static void g_lat512(double *a, size_t n)  { latency(a, n, 512.0); }
static void windows(double *a, size_t n, size_t w) { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr();
    for (size_t b = 0; b < n; b += w) { size_t m = n - b < w ? n - b : w; for (size_t j = m - 1; j > 0; j--) { size_t r = nr() % (j + 1); double t = a[b + j]; a[b + j] = a[b + r]; a[b + r] = t; } } }
static void g_win2048(double *a, size_t n) { windows(a, n, 2048); }
static void g_win64(double *a, size_t n)   { windows(a, n, 64); }
static void g_jitter(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 2000.0 - 1000.0; }
static void g_jit2(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 4.0 - 2.0; }
static void g_jit8(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 16.0 - 8.0; }
static void g_jit30(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 60.0 - 30.0; }
static void g_ticks(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = floor(((double)i + fr() * 3000.0) / 1000.0); }
static void g_one(double *a, size_t n)     { latency(a, n, 4096.0); a[n / 3] = a[n - 7] + 1e6; }
static void g_s5(double *a, size_t n)      { latency(a, n, 4096.0); swaps(a, n, n / 200000 + 1); }
static void g_s4(double *a, size_t n)      { latency(a, n, 4096.0); swaps(a, n, n / 20000 + 1); }
static void g_s3(double *a, size_t n)      { latency(a, n, 4096.0); swaps(a, n, n / 2000 + 1); }
static void g_s2(double *a, size_t n)      { latency(a, n, 4096.0); swaps(a, n, n / 200 + 1); }
static void g_l512s(double *a, size_t n)   { latency(a, n, 512.0); swaps(a, n, n / 20000 + 1); }
static void g_j100s(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 200.0 - 100.0; swaps(a, n, n / 20000 + 1); }
static void g_j20s(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 40.0 - 20.0; swaps(a, n, n / 20000 + 1); }
static void g_j2s(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 4.0 - 2.0; swaps(a, n, n / 20000 + 1); }
static void g_j8s(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 16.0 - 8.0; swaps(a, n, n / 20000 + 1); }
static void g_zeros(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = 1.7e9 + (double)i + fr() * 2000.0; for (size_t q = 0; q < n / 2000; q++) a[nr() % n] = 0.0; }
static void zeros(double *a, size_t n, size_t k) { for (size_t i = 0; i < n; i++) a[i] = 1.7e9 + (double)i + fr() * 2000.0; for (size_t q = 0; q < k; q++) a[nr() % n] = 0.0; }
static void g_z8(double *a, size_t n)      { zeros(a, n, 8); }
static void g_z84(double *a, size_t n)     { zeros(a, n, 84); }
static void g_z840(double *a, size_t n)    { zeros(a, n, 840); }
static void g_late84(double *a, size_t n)  { latency(a, n, 4096.0); for (size_t q = 0; q < 84; q++) { size_t x = n / 4 + nr() % (n / 2); a[x] = a[x] - (double)(n / 8) - fr() * (double)(n / 16); } }
static void g_far(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 80000.0; }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_near(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; swaps(a, n, n / 100); }
static void g_groups(double *a, size_t n)  { size_t pos = 0; double key = 0; while (pos < n) { size_t len = (size_t)exp(log(2000.0) + fr() * log(100.0)); if (len > n - pos) len = n - pos;
    for (size_t i = 0; i < len; i++) { a[pos + i] = key + fr(); }
    key += 1.0; pos += len; } }
static void g_regime(double *a, size_t n)  { size_t pos = 0; int r = 0; while (pos < n) { size_t len = (size_t)exp(log(20000.0) + fr() * log(20.0)); if (len > n - pos) len = n - pos; double base = (double)r * 1099511627776.0;
    for (size_t i = 0; i < len; i++) { a[pos + i] = base + (double)(pos + i) + fr() * 512.0; }
    pos += len; r = (r + 1 + (int)(nr() % 2)) % 3; } }
static void blocks64(double *a, size_t n, double o) { size_t nb = 64, blk = (n + nb - 1) / nb, perm[64]; for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5; }
static void g_stairs(double *a, size_t n)  { blocks64(a, n, 1.0 / 9.0); }
static void g_bp64(double *a, size_t n)    { blocks64(a, n, 0.0); }
static void g_heavy(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
static void g_few(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }

typedef void (*gen_fn)(double *, size_t);
static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "latency_4096",   g_lat4096, "event log, latency < 4096 ms: displacement <= 4096" },
    { "window_2048",    g_win2048, "shuffled inside 2048-element windows" },
    { "jitter_1000",    g_jitter,  "index + uniform noise of +-1000" },
    { "latency_512",    g_lat512,  "event log, latency < 512 ms" },
    { "window_64",      g_win64,   "shuffled inside 64-element windows (TRUTH_TABLE J6's jitter_64)" },
    { "jitter_2",       g_jit2,    "noise of +-2: dense, but within an insertion sort's budget" },
    { "jitter_8",       g_jit8,    "noise of +-8: at that budget" },
    { "jitter_30",      g_jit30,   "noise of +-30: past it" },
    { "ticks_dup",      g_ticks,   "jittered one-second ticks: short reach with heavy duplicates" },
    { "one_stray",      g_one,     "latency_4096 with ONE far element" },
    { "strays_1e-5",    g_s5,      "latency_4096, 0.001% of elements swapped to anywhere" },
    { "strays_1e-4",    g_s4,      "0.01%" },
    { "strays_1e-3",    g_s3,      "0.1%" },
    { "strays_1e-2",    g_s2,      "1%: the ladder's first rung fails" },
    { "sentinel_zeros", g_zeros,   "jittered timestamps, 0.05% zeros" },
    { "zeros_8",        g_z8,      "jittered timestamps with 8 zeros: strays that all travel to one end" },
    { "zeros_84",       g_z84,     "84 zeros" },
    { "zeros_840",      g_z840,    "840 zeros" },
    { "late_84",        g_late84,  "latency_4096 with 84 events delivered an eighth of the log late" },
    { "lat512_strays",  g_l512s,   "latency_512 + 0.01% strays: the route declines, the general leaf meets dense short disorder" },
    { "jit100_strays",  g_j100s,   "noise of +-100 + 0.01% strays" },
    { "jit20_strays",   g_j20s,    "noise of +-20 + 0.01% strays" },
    { "jit8_strays",    g_j8s,     "noise of +-8 + 0.01% strays: the insertion sort's budget is about here" },
    { "jit2_strays",    g_j2s,     "noise of +-2 + 0.01% strays: dense, but the insertion sort's kind" },
    { "reach_40000",    g_far,     "noise of 80000: the reach needs blocks above the cap" },
    { "uniform",        g_uniform, "guard" },
    { "near_1pct",      g_near,    "guard: sorted + 1% swaps (sparse disorder, the near-sorted route's)" },
    { "coarse_groups",  g_groups,  "guard: sorted by a coarse key, random inside" },
    { "regime_switch",  g_regime,  "guard: three value bands, short reach inside a dwell" },
    { "stairs_x20",     g_stairs,  "guard: the knot route's shape" },
    { "block_perm64",   g_bp64,    "guard: relocation's shape" },
    { "heavy_tail",     g_heavy,   "guard" },
    { "few_distinct",   g_few,     "guard" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

/* RB_OFF=NAME=VALUE / RB_ON=NAME=VALUE: compare two settings of a runtime knob (the route on both sides) instead of route off / on */
static char g_off[2][64], g_on[2][64]; static int g_off_set, g_on_set;
static void side_env(const char *e, char kv[2][64], int *set) { if (!e) return; const char *q = strchr(e, '='); if (!q || q - e >= 64) return;
    memcpy(kv[0], e, (size_t)(q - e)); kv[0][q - e] = 0; snprintf(kv[1], 64, "%s", q + 1); *set = 1; }
/* each side first takes the other side's variable away: an environment variable outlives the call that set it */
static void sort_off(double *a, size_t n, int t) { if (g_on_set) unsetenv(g_on[0]); if (g_off_set) { unsetenv("LOGOS_NO_REACH"); setenv(g_off[0], g_off[1], 1); } else setenv("LOGOS_NO_REACH", "1", 1); logos_sort_f64(a, n, t); }
static void sort_on(double *a, size_t n, int t)  { unsetenv("LOGOS_NO_REACH"); if (g_off_set) unsetenv(g_off[0]); if (g_on_set) setenv(g_on[0], g_on[1], 1); logos_sort_f64(a, n, t); }

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int once = argc > 3 && !strcmp(argv[3], "once");
    const char *only = argc > 4 ? argv[4] : NULL;
    int P = bench_threads(logos_num_cpus());
    side_env(getenv("RB_OFF"), g_off, &g_off_set); side_env(getenv("RB_ON"), g_on, &g_on_set);
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;
    if (once) {
        for (int k = 0; k < NSH; k++) {
            if (only && !strstr(SH[k].name, only)) continue;
            RS = 1 + (uint64_t)k; SH[k].g(base, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) { fprintf(stderr, "==== %s, %d thread%s\n", SH[k].name, tc[ti], tc[ti] > 1 ? "s" : ""); memcpy(w, base, n * 8); sort_on(w, n, tc[ti]); }
        }
        free(base); free(w); free(ref);
        return 0;
    }
    printf("short-reach route: off vs on, n=%zu, %d pairs, threads 1 and %d, one binary (LOGOS_NO_REACH)\n", n, pairs, P);
    printf("ratio > 1.000 means the route is that much faster. Compare against ./pairprobe.\n");
    if (g_off_set || g_on_set) printf("VARIANT ROWS, route on both sides: first side %s=%s, second side %s=%s; ratio > 1 means the SECOND is faster\n",
        g_off_set ? g_off[0] : "-", g_off_set ? g_off[1] : "-", g_on_set ? g_on[0] : "-", g_on_set ? g_on[1] : "-");
    printf("\n");
    printf("  %-15s %2s %9s %9s %9s  %-62s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    int bad = 0;
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
        if (!ok) bad++;
        int tc[2] = { 1, P };
        for (int ti = getenv("BENCH_PAR_ONLY") ? 1 : 0; ti < 2; ti++) {          /* BENCH_PAR_ONLY=1: the P-thread rows only, to give a borderline row more pairs cheaply */
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_off, sort_on, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-15s %2d %9.3f %9.3f %9.3f  %-62s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    unsetenv("LOGOS_NO_REACH");
    free(base); free(w); free(ref);
    (void)bench_threads_source;
    return bad != 0;
}
