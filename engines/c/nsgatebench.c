/* nsgatebench.c -- the near-sorted route's two gates, repaired 2026-09-20, measured against the gates as they were.
 *
 * Found while building the short-reach gate, which wanted the shared sample's inversion statistic at other lags:
 *   D1. sodo_sample's inversion rate read SEVEN pairs. Sample indices i and i+144 sit 0.31% of n apart, and the later
 *       index is the EARLIER position (frac(144 phi) = 0.99689), so the `p1 <= p0` skip that was meant to drop the few
 *       pairs wrapping around the array dropped all 1,897 adjacent pairs and kept the 7 wrapped ones, a whole array apart.
 *       With 7 pairs it passed block-permuted, sawtooth and grouped-run data as nearly sorted and turned away one
 *       genuinely near-sorted input in seven.
 *   D3. the pre-gate's greedy scan over four 4096-element windows had no poison recovery, the route's own scan has. On
 *       sorted data with random swaps it declined 12 of 40 seeds at 1% swaps and 38 of 40 at 2%.
 * The gate now reads every pair in position order (inv_all) and the pre-gate runs the route's own scan on the windows.
 * One binary, switched at runtime: LOGOS_NS_GATE_OLD=1 restores both old gates. Paired harness, every row verified.
 *
 * PREDICTIONS, written before this ran (ratio = old gates / new gates at 8.4M and 14 threads; > 1: the repair is faster):
 *   groups_of_runs   3-4: the old gate lets the route run, it rejects 73% of the array and declines after 57 ms
 *   near_1pct, a seed the old gates turned away   1.1-1.4: the route instead of the partition
 *   near_1pct, a seed the old gates passed        1.00
 *   near_2pct, near_3pct   1.0-1.3: newly admitted (true rate 3.4-5%); whether the route beats the partition there is unmeasured
 *   near_5pct, near_10pct  1.00-1.05: the true rate (8-17%) is above the 5% gate
 *   block_perm64, stairs_x20, sawtooth, gray_code, sorted_runs_256   1.00-1.02: turned away within 0.2 ms before, too
 *   uniform, heavy_tail, few_distinct, and EVERY 1-thread row   1.00 (the route is parallel only)
 * GRADED (30 pairs, the borderline rows again with 100; 14 threads): groups_of_runs 3.92; near_1pct 1.34-1.57 on all four seeds
 *   (the two the 7-pair statistic had passed were being turned away by the pre-gate); near_2pct 1.38-1.40 (predicted too low);
 *   near_0.5pct, near_3pct, near_5pct, near_10pct and every guard 0.98-1.01; every 1-thread row 0.96-1.04.
 *   Then the ROUTE was measured against the partition across sizes (NSG_ROUTE=1: LOGOS_NO_NEARSORTED / repaired gates), which the old
 *   pre-gate had made impossible: <= 1% swaps win everywhere (1.07-2.1), 2% swaps lose at 1.5M (0.84-0.87) and win from 6M (1.33-1.41),
 *   3% swaps lose at 1.5-2M (0.75-0.80), 5% swaps lose at 8.4M with the gate lifted (0.71). The gate is 2.5% below 6M elements and
 *   5% from there on; with it the mid-size rows read 1.00-1.36 and groups_of_runs 2.76-3.81 (300 pairs at 1.5M, 2M and 4M).
 *
 * Usage: ./nsgatebench [pairs] [n] [name]   LOGOS_THREADS=N overrides the thread count; BENCH_PAR_ONLY=1 skips the 1-thread rows;
 *        NSG_ROUTE=1 compares the route switched off against the repaired gates instead of old gates against repaired. */
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
static void near(double *a, size_t n, double f, uint64_t sd) { RS = sd; for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
    size_t k = (size_t)(f * (double)n); for (size_t q = 0; q < k; q++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } }
static void blocks64(double *a, size_t n, double o) { size_t nb = 64, blk = (n + nb - 1) / nb, perm[64]; for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5; }
static void g_gruns(double *a, size_t n)   { size_t pos = 0; double key = 0; while (pos < n) { size_t len = (size_t)exp(log(2000.0) + fr() * log(100.0)); if (len > n - pos) len = n - pos;
        for (size_t i = 0; i < len; i++) { a[pos + i] = key + fr(); }
        key += 1.0; pos += len; }
    for (size_t p = 0; p < n; p += 33421) qsort(a + p, p + 33421 <= n ? 33421 : n - p, 8, cmp_d); }
static void g_n1a(double *a, size_t n)     { near(a, n, 0.01, 1000); }   /* seeds 1000 and 1001 read 0.143 on the 7-pair statistic: turned away */
static void g_n1b(double *a, size_t n)     { near(a, n, 0.01, 1001); }
static void g_n1c(double *a, size_t n)     { near(a, n, 0.01, 1002); }   /* reads 0.000: passed */
static void g_n1d(double *a, size_t n)     { near(a, n, 0.01, 1003); }
static void g_n05(double *a, size_t n)     { near(a, n, 0.005, 1000); }
static void g_n2a(double *a, size_t n)     { near(a, n, 0.02, 1002); }
static void g_n2b(double *a, size_t n)     { near(a, n, 0.02, 1003); }
static void g_n3(double *a, size_t n)      { near(a, n, 0.03, 1002); }
static void g_n5(double *a, size_t n)      { near(a, n, 0.05, 1002); }
static void g_n10(double *a, size_t n)     { near(a, n, 0.10, 1002); }
static void g_bp64(double *a, size_t n)    { blocks64(a, n, 0.0); }
static void g_stairs(double *a, size_t n)  { blocks64(a, n, 1.0 / 9.0); }
static void g_saw(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)(i % 100000) + fr() * 0.5; }
static void g_gray(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)(i ^ (i >> 1)) + 0.25; }
static void g_runs256(double *a, size_t n) { size_t k = 256, rl = n / k; for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; for (size_t r = 0; r < k; r++) qsort(a + r * rl, (r + 1 == k ? n : (r + 1) * rl) - r * rl, 8, cmp_d); }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_heavy(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
static void g_few(double *a, size_t n)     { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }

typedef void (*gen_fn)(double *, size_t);
static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "groups_of_runs",  g_gruns,   "sorted 33k blocks inside coarse-key groups: the old gate lets the route run and it declines late" },
    { "near_1pct_a",     g_n1a,     "sorted + 1% swaps; the 7-pair statistic read 0.143 and turned it away" },
    { "near_1pct_b",     g_n1b,     "the same, another seed it turned away" },
    { "near_1pct_c",     g_n1c,     "a seed the 7-pair statistic passed (the pre-gate may still have declined it)" },
    { "near_1pct_d",     g_n1d,     "another" },
    { "near_0.5pct",     g_n05,     "" },
    { "near_2pct_a",     g_n2a,     "newly admitted: true rate ~3.5%" },
    { "near_2pct_b",     g_n2b,     "" },
    { "near_3pct",       g_n3,      "true rate ~4.5-5%: at the gate" },
    { "near_5pct",       g_n5,      "true rate ~8%: above the gate" },
    { "near_10pct",      g_n10,     "" },
    { "block_perm64",    g_bp64,    "guard: passed by the old statistic, turned away by the thresholds" },
    { "stairs_x20",      g_stairs,  "guard" },
    { "sawtooth_100k",   g_saw,     "guard" },
    { "gray_code",       g_gray,    "guard" },
    { "sorted_runs_256", g_runs256, "guard" },
    { "uniform",         g_uniform, "guard" },
    { "heavy_tail",      g_heavy,   "guard" },
    { "few_distinct",    g_few,     "guard" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))
static void sort_old(double *a, size_t n, int t) { setenv("LOGOS_NS_GATE_OLD", "1", 1); logos_sort_f64(a, n, t); }
static void sort_new(double *a, size_t n, int t) { unsetenv("LOGOS_NS_GATE_OLD"); logos_sort_f64(a, n, t); }
/* NSG_ROUTE=1: what the ROUTE is worth, not the gates: first side the near-sorted route switched off (LOGOS_NO_NEARSORTED), second side the
   repaired gates. A row where the gates decline reads 1.00; a row under 1.00 is an input the gates admit and the route loses. */
static void sort_noroute(double *a, size_t n, int t) { unsetenv("LOGOS_NS_GATE_OLD"); setenv("LOGOS_NO_NEARSORTED", "1", 1); logos_sort_f64(a, n, t); unsetenv("LOGOS_NO_NEARSORTED"); }

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 5) pairs = 5;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    const char *only = argc > 3 ? argv[3] : NULL;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 1;
    printf("near-sorted gates: as they were (LOGOS_NS_GATE_OLD=1) vs repaired, n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
    printf("ratio > 1.000 means the repaired gates are that much faster. Compare against ./pairprobe.\n\n");
    printf("  %-16s %2s %9s %9s %9s  %-96s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    int bad = 0;
    for (int k = 0; k < NSH; k++) {
        if (only && !strstr(SH[k].name, only)) continue;
        RS = 1 + (uint64_t)k;
        SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_old(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_new(w, n, P); ok &= memcmp(w, ref, n * 8) == 0;
        memcpy(w, base, n * 8); sort_new(w, n, 1); ok &= memcmp(w, ref, n * 8) == 0;
        if (!ok) bad++;
        int tc[2] = { 1, P };
        for (int ti = getenv("BENCH_PAR_ONLY") ? 1 : 0; ti < 2; ti++) {          /* BENCH_PAR_ONLY=1: the P-thread rows only, to give a borderline row more pairs cheaply */
            double p10 = 0, p90 = 0;
            double med = getenv("NSG_ROUTE") ? bench_paired(sort_noroute, sort_new, base, w, n, tc[ti], pairs, &p10, &p90) : bench_paired(sort_old, sort_new, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-16s %2d %9.3f %9.3f %9.3f  %-96s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    unsetenv("LOGOS_NS_GATE_OLD");
    free(base); free(w); free(ref);
    (void)bench_threads_source;
    return bad != 0;
}
