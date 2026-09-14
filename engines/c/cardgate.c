/* cardgate.c — which shapes does the hash-count route help, and which does it cost? The ground truth for its gate.
 *
 * The chaos map found user_ids_zipf (200k sparse ids, Zipf activity) spending 30.5 of 68.9 ms at 14 threads in a
 * hash-count attempt that aborted at MODCARD_CAP, because Chao1 on a Zipf sample badly under-estimates the true
 * cardinality. Its proposed fix, "decline when singletons/m > 0.3", would ALSO decline uniform data over 5k-60k
 * distinct values, whose samples are mostly singletons too. Before designing a gate, this measures the truth on
 * today's engine: for every shape, the default dispatcher against the same binary with the route switched off at
 * runtime (LOGOS_MC_OFF=1). A ratio above 1 means the route COSTS that much (the gate should decline); below 1
 * means it earns it (the gate must keep it). Each shape also prints its sample's frequency-of-frequencies profile
 * (2048 random positions): distinct d, singletons f1, doubletons f2, tripletons f3, Chao1, the heterogeneity ratio
 * f1*f3/f2^2 (2/3 for a Poisson profile, i.e. values equally likely; larger means skew), and today's gate verdict.
 *
 * PREDICTIONS, written before this ran (ratio = default time / route-off time):
 *   user_ids_zipf: 1 thread 1.00-1.03 (a single map aborts after ~3% of the array); 14 threads 1.3-1.7.
 *   hash_5k: 0.3-0.6 at both counts (counting beats partitioning easily at 5k distinct).
 *   hash_30k: 0.5-0.8 at 1 thread, 0.8-1.1 at 14 (the serial merge tail grows with distinct count).
 *   hash_60k: exactly 1.00 at 1 thread (sequential gate 0.98 declines it: d/m ~ 0.985); 14 threads 1.0-1.5.
 *   zipf_pareto (floor of a Pareto, ~10-30k distinct small integers): 0.5-0.9 at both.
 *   prices_cents (lognormal cents, ~50k distinct): 1 thread 0.7-1.0, 14 threads 1.5-3.0 (the explorer measured
 *   modcard's serial tail at 103-109 ms against ~35 for the partition).
 *   yyyymmdd and mantissa16: taken by the lattice before this route; 1.00 unless the lattice attempt inside the
 *   route is what takes them.
 *   uniform and few_5: exactly 1.00 (never gated in / taken earlier by the low-cardinality route).
 *   Heterogeneity: user_ids_zipf above 2, hash_* within 0.4-1.0, zipf_pareto above 2.
 *
 * GRADED (8.4M, 20 pairs, floor 2.5% at 1M and 2.7% at 8M): mostly WRONG, and all in one direction, the route costs
 *   far more than predicted. 14 threads: hash_60k 5.30 (predicted 1.0-1.5), prices_cents 3.81 (1.5-3.0), hash_30k 2.45
 *   (0.8-1.1), user_ids_zipf 1.90 (1.3-1.7), mantissa16 1.62 and yyyymmdd 1.18 (both predicted 1.00), hash_5k 1.10
 *   (0.3-0.6). Only zipf_pareto earns it, 0.28. 1 thread: zipf_pareto 0.33 and yyyymmdd 0.49 earn it (predicted 0.5-0.9
 *   and 1.00); hash_5k COSTS 1.16 (predicted 0.3-0.6); prices_cents 1.13 and user_ids_zipf 1.11 abort (p90s above 2).
 *   The heterogeneity ratio came out as predicted: 2.96-4.39 on the Zipf shapes, 0.00-0.89 on equal-odds ones.
 *   The routes trace showed the 1-thread dispatcher samples 384 points, not 2048, and that prices_cents aborts at 1 thread.
 *   So the Zipf abort is the SMALL problem; at 14 threads the hash map loses to the partition above a few thousand values.
 *
 * LADDER (./cardgate pairs n ladder): where are the crossovers? Hash map only, default against LOGOS_MC_HASH_OFF=1 (the
 * lattice attempt still runs), on d random doubles with equal odds; and the whole route, default against LOGOS_MC_OFF=1,
 * on integer lattices 0..d-1. PREDICTIONS, written before it ran (ratio > 1: that part costs, < 1: it earns):
 *   hash, 1 thread:   d=300 0.4-0.7, 600 0.5-0.8, 1200 0.6-0.9, 2500 0.8-1.0, 5000 1.05-1.2, 10000 1.1-1.3 (crossover ~3k)
 *   hash, 14 threads: d=300 0.4-0.8, 600 0.6-0.9, 1200 0.8-1.0, 2500 0.9-1.1, 5000 1.0-1.2, 10000 1.3-1.8 (crossover ~2k)
 *   lattice, 1 thread:   d=1000 0.3-0.6, 4000 0.3-0.6, 16000 0.4-0.7, 65536 0.8-1.0, 200000 0.9-1.1 (over the slot ceiling)
 *   lattice, 14 threads: d=1000 0.5-0.9, 4000 0.8-1.1, 16000 1.0-1.3, 65536 1.4-1.8, 200000 1.0-2.0
 *
 * LADDER GRADED, two runs (20 pairs contaminated at 1 thread by an external load, 40 pairs usable, 8M floor 4.1-5.5%):
 *   hash, 14 threads: 0.60/0.51, 0.82/0.65, 0.87/0.78, 0.99/0.98, 1.21/1.27, 1.64/1.40 for d=300..10000: crossover ~2.5k,
 *   as predicted. hash, 1 thread: 1.09, 1.13, 1.30, 1.18, 1.14, 1.15 (40 pairs), WRONG: on equal-odds doubles the hash
 *   map costs 9-30% at EVERY size tested, it never earns. lattice, 1 thread: 0.44, 0.44, 0.98, 1.02, 1.00 (earns 2.3x to
 *   4k, predicted to earn at 16k too: wrong there). lattice, 14 threads: 0.80, 0.83, 0.91, 1.90, 1.01 (costs at 65536
 *   as predicted, but earns a little at 16k, predicted to cost).
 *   Open question this raises: zipf_pareto earned 3x at 1 thread in round one. Skew, or the lattice attempt?
 *
 * SKEW (./cardgate pairs n skew): Zipf(1.1) activity over d random doubles, hash map on against off, d = 1k, 4k, 16k,
 * 64k; zipf_pareto the same way; and a 32k integer lattice, whole route on against off, to place the 14-thread lattice
 * crossover between 16k and 65536. PREDICTIONS, written before it ran:
 *   zipf over d, 1 thread:   1k 0.4-0.7, 4k 0.6-0.9, 16k 0.9-1.1, 64k 1.0-1.2 (skew keeps the hot entries in L1)
 *   zipf over d, 14 threads: 1k 0.3-0.6, 4k 0.6-0.9, 16k 1.0-1.4, 64k 1.5-3.0 (the serial merge grows with d)
 *   zipf_pareto, hash map only: 0.3-0.5 at both counts (its round-one win was the hash map, not the lattice)
 *   lat_32000: 1 thread 0.9-1.1, 14 threads 1.1-1.5
 *   Heterogeneity f1*f3/f2^2 above 2 on every zipf row that has doubletons.
 *
 * SKEW GRADED (40 pairs, 8M floor 7.8-8.1%): 1 thread zipf over 1k/4k/16k/64k 1.15, 1.14, 1.03, 1.24: WRONG, skew does
 *   not make the hash map pay at 1 thread (predicted 0.4-0.7 at 1k). zipf_pareto 0.34 at 1 thread and 0.35 at 14, as
 *   predicted: its win IS the hash map, and its sample Chao1 is 31-50 against 300+ for every shape that lost. 14 threads:
 *   0.22, 0.32, 0.53, 1.12, right in direction and better than predicted at 16k. lat_32000 1.02 / 0.99, neutral where
 *   14 threads was predicted to cost.
 *
 * THE RULE, fixed from the training shapes above before the held-out mode ever ran (mc_hash_allowed, lattice_try_f64):
 *   hash map only if sample Chao1 <= 128 below 8 threads, <= 3300 at 8 or more; at 8+ threads no lattice table above
 *   65536 slots. LOGOS_MCGATE_OLD=1 restores the old gate.
 * GATE mode (./cardgate pairs n gate), the training shapes, old gate over new. PREDICTIONS: 14 threads, where the new gate
 *   declines, about the round-one route-off ratios: user_ids_zipf ~1.9, hash_5k ~1.1, hash_30k ~2.4, hash_60k ~5, prices
 *   ~3.8, mantissa16 ~1.6, yyyymmdd ~1.2; zipf_pareto, uniform, few_5 1.00. 1 thread: hash_5k ~1.15, user_ids_zipf ~1.1,
 *   prices_cents ~1.1, everything else 1.00.
 * HELD-OUT mode (./cardgate pairs n heldout), ten shapes written before the rule existed. PREDICTIONS: old/new >= 0.97 on
 *   all 20 cells (no regression beyond noise); new/off <= 1.10 on at least 18 of 20 (little left to gain by switching the
 *   route off entirely); old/new >= 1.2 on at least 4 cells.
 *
 * GATE GRADED (30 pairs, 8M floor 6.9-7.4%): training shapes as predicted at 14 threads (user_ids_zipf 1.88, hash_30k
 *   2.45, hash_60k 4.46, prices 2.70, mantissa16 1.40, hash_5k 1.23; route winners 1.00). HELD-OUT: new/off <= 1.10 on 18
 *   of 20 cells (predicted at least 18: right, exactly); old/new >= 1.2 on 4 clear cells (latency 1.39, words 14T 2.10,
 *   hash_2k 1.59, mixed 1.66: right); old/new >= 0.97 on all 20 WRONG: session_sec at 1 thread 0.505. gdp_3sig at 14T
 *   read 0.94 but the trace shows the same route under both gates there, so that one is noise.
 *   Two held-out rows printed FAILED: an oracle artifact, not a wrong sort (signed zeros; see out_ok). Fixed.
 *   The one real miss: the trace shows session_sec (exponential whole seconds, sample Chao1 711) declined by the new
 *   1-thread threshold into the leaf, where under the old gate the hash map ran and won 2x.
 * INTNESS (./cardgate pairs n intness). Every 1-thread hash-map WIN so far was on integer values (zipf_pareto 3x, ages 4x,
 *   session_sec 2x) and every loss on non-integers. Same cardinality and skew, integer against non-integer values.
 *   PREDICTIONS, before it ran, if integrality is the cause: 1 thread eq2k_int 0.5-0.8 and eq2k_real 1.1-1.4; zipf4k_int
 *   0.5-0.8 and zipf4k_real 1.05-1.25; session_sec 0.4-0.6; session_half (the same data + 0.5) 1.0-1.3. If the cause is
 *   instead the lattice-like layout, session_half reads like session_sec. 14 threads: each pair alike.
 *   A COMPETING PREDICTION, written while the intness run was already going but before any of its results were read:
 *   the 1-thread cause is not integrality but the FALLBACK. leaf_f64 sends data whose sampled key bytes vary in at most
 *   QUANT_PROBE_MAX (4) of 8 byte positions to radix_f64_full, a multi-pass float radix, and everything else to the AVX2
 *   quicksort. The hash map beats the radix and loses to the quicksort. Worked out per shape: session_sec (3 bytes) and
 *   session_half (4 bytes) go to the radix, so both EARN (0.4-0.7); eq2k_int and zipf4k_int are integers below 2^30
 *   using 5 bytes, so they go to the quicksort and COST like their real twins (1.05-1.4). Integrality predicts the
 *   opposite on exactly those three rows.
 *
 * INTNESS GRADED (30 pairs, 8M floor 4.0-7.7%): 1 thread eq2k_real 1.60, eq2k_int 1.17, zipf4k_real 1.06, zipf4k_int 1.25,
 *   session_sec 0.54, session_half 0.42. The FALLBACK prediction was right on all three rows where the two disagreed (the
 *   integer twins cost, the half-integer copy earns); integrality was wrong on all three. 14 threads: every pair alike.
 *
 * V2 (./cardgate pairs n v2): the 1-thread gate also allows the hash map when the leaf's fallback would be the float radix
 *   (sample key bytes varying in <= 4 of 8 positions, n within the radix window, Chao1 <= 3300). Version 1 against version
 *   2 at ONE thread (they are identical at 8+), on seven FRESH shapes used by no earlier mode. PREDICTIONS, before it ran
 *   (v1/v2 > 1: version 2 faster; v2/off <= 1: what version 2 lets in earns):
 *     counts_exp20 (whole numbers, mean 20): Chao1 maybe near 128; 1.00-1.8 depending which side of 128 it lands
 *     int_skew_2k (Zipf over 2000 integers below 2^16):   v1/v2 1.5-2.5, v2/off 0.4-0.7
 *     int_skew_20k (Zipf over 20000 integers below 2^20): v1/v2 1.2-2.0, v2/off 0.5-0.85
 *     int_eq_8k (equal odds over 8000 integers below 2^16): Chao1 above 3300, declined by both: 1.00; v2/off 1.00
 *     halves_skew (Zipf over 3000 multiples of 0.5 below 2^14): v1/v2 1.5-2.5, v2/off 0.4-0.7
 *     cents_2dp (prices to the cent, full mantissa, AVX2 fallback): 1.00 and 1.00
 *     ratings_half (1.0..5.0 by halves): the low-cardinality route first: 1.00 and 1.00
 *   GRADED (30 pairs, 8M floor 3.3% / 2.7%): ALL SEVEN inside their predictions. v1/v2: counts_exp20 1.02 (Chao1 103,
 *   under 128, so the same route both ways), int_skew_2k 1.76, int_skew_20k 1.84, int_eq_8k 0.99, halves_skew 2.02,
 *   cents_2dp 0.98, ratings_half 0.99. v2/off: 1.00, 0.57, 0.60, 0.97, 0.52, 1.01, 0.98.
 *
 * Usage: ./cardgate [pairs] [n]   ./cardgate [pairs] [n] ladder|skew|gate|heldout|intness|verify|v2   ./cardgate 1 [n] routes   (one sort per shape and thread count, for
 *                                                                     a -DLOGOS_TRACE build: shows the route) */
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
static double gauss(void) { double u = fr(), v = fr(); if (u < 1e-300) u = 1e-300; return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v); }
static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static int cmp_u64(const void *x, const void *y) { uint64_t a, b; memcpy(&a, x, 8); memcpy(&b, y, 8); return (a > b) - (a < b); }
/* The oracle. qsort's comparator calls -0.0 and +0.0 equal and leaves them in arbitrary order, so a byte compare against
 * its answer FAILED two correct sorts (temp_tenths, ages: every variant ascending, same bitwise multiset, every byte
 * difference a signed-zero swap; ./cardgate 1 n verify). Correct means: ascending under '<', value for value equal to the
 * qsort answer, the same bits wherever both sides are not zero, and as many -0.0 as the input. */
static int out_ok(const double *w, const double *ref, size_t n) {
    size_t nz_w = 0, nz_r = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && w[i] < w[i - 1]) return 0;
        if (memcmp(&w[i], &ref[i], 8) && !(w[i] == 0.0 && ref[i] == 0.0)) return 0;
        uint64_t a, b; memcpy(&a, &w[i], 8); memcpy(&b, &ref[i], 8);
        nz_w += a == 0x8000000000000000ULL; nz_r += b == 0x8000000000000000ULL;
    }
    return nz_w == nz_r;
}

typedef void (*gen_fn)(double *, size_t);
static void g_userids(double *a, size_t n) {             /* chaoshunt's S_USERIDS */
    double *ids = malloc(200000 * sizeof(double)); if (!ids) return;
    for (size_t k = 0; k < 200000; k++) ids[k] = floor(fr() * 2147483648.0);
    for (size_t i = 0; i < n; i++) { double r = exp(fr() * 12.79) - 1; size_t k = (size_t)r; if (k >= 200000) k = 199999; a[i] = ids[k]; }
    free(ids);
}
static void g_pareto(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = floor(pow(1.0 - fr() * 0.999999, -1.0 / 1.2)); }
static void gen_hash_d(double *a, size_t n, size_t d) {
    double *tab = malloc(d * sizeof(double)); if (!tab) return;
    for (size_t i = 0; i < d; i++) tab[i] = fr() * 1e9;
    for (size_t i = 0; i < n; i++) a[i] = tab[nr() % d];
    free(tab);
}
/* Zipf(s) activity over d random doubles: value k drawn with odds proportional to 1/(k+1)^s */
static void gen_zipf_d(double *a, size_t n, size_t d, double s) {
    double *tab = malloc(d * sizeof(double)), *cdf = malloc(d * sizeof(double));
    if (!tab || !cdf) { free(tab); free(cdf); return; }
    double acc = 0;
    for (size_t k = 0; k < d; k++) { tab[k] = fr() * 1e9; acc += 1.0 / pow((double)(k + 1), s); cdf[k] = acc; }
    for (size_t i = 0; i < n; i++) {
        double u = fr() * acc; size_t lo = 0, hi = d - 1;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (cdf[mid] < u) lo = mid + 1; else hi = mid; }
        a[i] = tab[lo];
    }
    free(tab); free(cdf);
}
static void g_hash5k(double *a, size_t n)  { gen_hash_d(a, n, 5000); }
/* the same generators with INTEGER values (sparse, so no lattice can take them) */
static void gen_hash_i(double *a, size_t n, size_t d) {
    double *tab = malloc(d * sizeof(double)); if (!tab) return;
    for (size_t i = 0; i < d; i++) tab[i] = floor(fr() * 1e9);
    for (size_t i = 0; i < n; i++) a[i] = tab[nr() % d];
    free(tab);
}
static void gen_zipf_i(double *a, size_t n, size_t d, double s) {
    double *tab = malloc(d * sizeof(double)), *cdf = malloc(d * sizeof(double));
    if (!tab || !cdf) { free(tab); free(cdf); return; }
    double acc = 0;
    for (size_t k = 0; k < d; k++) { tab[k] = floor(fr() * 1e9); acc += 1.0 / pow((double)(k + 1), s); cdf[k] = acc; }
    for (size_t i = 0; i < n; i++) {
        double u = fr() * acc; size_t lo = 0, hi = d - 1;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (cdf[mid] < u) lo = mid + 1; else hi = mid; }
        a[i] = tab[lo];
    }
    free(tab); free(cdf);
}
/* ---- held-out shapes: written before the gate rule, never used to design it ---- */
static void h_product_ids(double *a, size_t n) { gen_zipf_d(a, n, 20000, 0.8); }                 /* weaker skew */
static void h_gdp3sig(double *a, size_t n) {                                                     /* lognormal, 3 significant digits */
    for (size_t i = 0; i < n; i++) { double v = exp(10.0 + 2.0 * gauss()); double e = pow(10.0, floor(log10(v)) - 2.0); a[i] = round(v / e) * e; }
}
static void h_temp_tenths(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = round((15.0 + 10.0 * gauss()) * 10.0) / 10.0; }
static void h_session_sec(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = floor(-300.0 * log(1.0 - fr())); }
static void h_latency_2dp(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = round(exp(3.0 + 0.8 * gauss()) * 100.0) / 100.0; }
static void h_words(double *a, size_t n)       { gen_zipf_d(a, n, 100000, 1.0); }                /* vocabulary, hapax-heavy */
static void h_hash2k(double *a, size_t n)      { gen_hash_d(a, n, 2000); }                       /* equal odds near the 14T crossover */
static void h_mixed(double *a, size_t n) {                                                       /* half lattice, half off-lattice */
    double tab[3000]; for (int k = 0; k < 3000; k++) tab[k] = fr() * 1e9;
    for (size_t i = 0; i < n; i++) a[i] = (nr() & 1) ? (double)(nr() % 1000) : tab[nr() % 3000];
}
static void h_status(double *a, size_t n)      { gen_zipf_d(a, n, 60, 1.5); }                    /* guard: low cardinality */
static void h_ages(double *a, size_t n) { for (size_t i = 0; i < n; i++) { double v = round(40.0 + 18.0 * gauss()); a[i] = v < 0 ? 0 : v > 100 ? 100 : v; } }
static void g_hash30k(double *a, size_t n) { gen_hash_d(a, n, 30000); }
static void g_hash60k(double *a, size_t n) { gen_hash_d(a, n, 60000); }
static void g_cents(double *a, size_t n)   { for (size_t i = 0; i < n; i++) { double v = floor(exp(3.5 + 1.3 * gauss()) * 100.0 + 0.5); if (v > 1e7) v = 1e7; a[i] = v; } }
static void g_dates(double *a, size_t n) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (size_t i = 0; i < n; i++) { int y = 2006 + (int)(fr() * 20), mo = (int)(fr() * 12), dd = 1 + (int)(fr() * dm[mo]); a[i] = (double)(y * 10000 + (mo + 1) * 100 + dd); }
}
static void g_mant16(double *a, size_t n)  { for (size_t i = 0; i < n; i++) a[i] = 1.0 + floor(fr() * 65536) / 65536.0; }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }
static void g_few5(double *a, size_t n)    { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; }

static const struct { const char *name; gen_fn g; const char *note; } SH[] = {
    { "user_ids_zipf", g_userids, "200k sparse ids, Zipf activity" },
    { "hash_5k",       g_hash5k,  "5k random doubles, equal odds" },
    { "hash_30k",      g_hash30k, "30k random doubles, equal odds" },
    { "hash_60k",      g_hash60k, "60k random doubles, equal odds" },
    { "zipf_pareto",   g_pareto,  "floor of a Pareto(1.2)" },
    { "prices_cents",  g_cents,   "lognormal prices in cents" },
    { "yyyymmdd",      g_dates,   "dates as integers, 20 years" },
    { "mantissa16",    g_mant16,  "65536-value lattice" },
    { "uniform",       g_uniform, "guard: never gated in" },
    { "few_5",         g_few5,    "guard: low-cardinality route" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static void sort_default(double *a, size_t n, int t) { unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF"); unsetenv("LOGOS_MCGATE_OLD"); logos_sort_f64(a, n, t); }
static void sort_mcoff(double *a, size_t n, int t)   { setenv("LOGOS_MC_OFF", "1", 1); unsetenv("LOGOS_MC_HASH_OFF"); unsetenv("LOGOS_MCGATE_OLD"); logos_sort_f64(a, n, t); }
static void sort_hashoff(double *a, size_t n, int t) { unsetenv("LOGOS_MC_OFF"); setenv("LOGOS_MC_HASH_OFF", "1", 1); setenv("LOGOS_MCGATE_OLD", "1", 1); logos_sort_f64(a, n, t); }
static void sort_oldgate(double *a, size_t n, int t) { unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF"); setenv("LOGOS_MCGATE_OLD", "1", 1); logos_sort_f64(a, n, t); }
static void sort_gate_v1(double *a, size_t n, int t) { unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF"); unsetenv("LOGOS_MCGATE_OLD"); setenv("LOGOS_MCGATE_V1", "1", 1); logos_sort_f64(a, n, t); unsetenv("LOGOS_MCGATE_V1"); }

/* Chao1 from m random positions: the dispatcher's estimate at the sample size it really uses (384 at 1 thread) */
static double het_of(const double *a, size_t n, size_t m, double *samp) {
    for (size_t i = 0; i < m; i++) samp[i] = a[nr() % n];
    qsort(samp, m, 8, cmp_d);
    size_t f1 = 0, f2 = 0, f3 = 0;
    for (size_t i = 0; i < m;) { size_t j = i + 1; while (j < m && samp[j] == samp[i]) j++; size_t c = j - i; if (c == 1) f1++; else if (c == 2) f2++; else if (c == 3) f3++; i = j; }
    return f2 ? (double)f1 * (double)f3 / ((double)f2 * (double)f2) : -1;
}
static double chao1_of(const double *a, size_t n, size_t m, double *samp) {
    for (size_t i = 0; i < m; i++) samp[i] = a[nr() % n];
    qsort(samp, m, 8, cmp_d);
    size_t d = 0, f1 = 0, f2 = 0;
    for (size_t i = 0; i < m;) { size_t j = i + 1; while (j < m && samp[j] == samp[i]) j++; d++; if (j - i == 1) f1++; else if (j - i == 2) f2++; i = j; }
    return (double)d + (double)f1 * (double)(f1 ? f1 - 1 : 0) / (2.0 * (double)(f2 + 1));
}

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8388608;
    int routes = argc > 3 && strcmp(argv[3], "routes") == 0;
    int ladder = argc > 3 && strcmp(argv[3], "ladder") == 0;
    int skew = argc > 3 && strcmp(argv[3], "skew") == 0;
    int heldout = argc > 3 && strcmp(argv[3], "heldout") == 0;
    int gate = argc > 3 && strcmp(argv[3], "gate") == 0;
    int verify = argc > 3 && strcmp(argv[3], "verify") == 0;
    int intness = argc > 3 && strcmp(argv[3], "intness") == 0;
    int v2 = argc > 3 && strcmp(argv[3], "v2") == 0;
    int hroutes = argc > 3 && strcmp(argv[3], "hroutes") == 0;
    if (!routes && pairs < 5) pairs = 5;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    enum { M = 2048 };
    double samp[M];
    if (!base || !w || !ref) return 1;

    if (v2) {
        printf("gate version 1 over version 2 at ONE thread, and version 2 over the route off, on fresh shapes, n=%zu, %d pairs\n", n, pairs);
        printf("v1/v2 > 1.000: version 2 faster.  v2/off < 1.000: what version 2 lets in earns.\n\n");
        printf("  %-13s %9s %7s %8s %13s %8s %13s  %s\n", "shape", "chao1@384", "het@2k", "v1/v2", "p10-p90", "v2/off", "p10-p90", "verify");
        for (int r = 0; r < 7; r++) {
            RS = 9301 + (uint64_t)r;
            const char *name;
            switch (r) {
            case 0: for (size_t i = 0; i < n; i++) base[i] = floor(-20.0 * log(1.0 - fr())); name = "counts_exp20"; break;
            case 1: gen_zipf_i(base, n, 2000, 1.1); for (size_t i = 0; i < n; i++) base[i] = fmod(base[i], 65536.0); name = "int_skew_2k"; break;
            case 2: gen_zipf_i(base, n, 20000, 1.1); for (size_t i = 0; i < n; i++) base[i] = fmod(base[i], 1048576.0); name = "int_skew_20k"; break;
            case 3: { double tab[8000]; for (int k = 0; k < 8000; k++) tab[k] = floor(fr() * 65536.0); for (size_t i = 0; i < n; i++) base[i] = tab[nr() % 8000]; name = "int_eq_8k"; break; }
            case 4: gen_zipf_i(base, n, 3000, 1.1); for (size_t i = 0; i < n; i++) base[i] = fmod(base[i], 32768.0) * 0.5; name = "halves_skew"; break;
            case 5: for (size_t i = 0; i < n; i++) base[i] = round(exp(3.0 + 1.0 * gauss()) * 100.0) / 100.0; name = "cents_2dp"; break;
            default: for (size_t i = 0; i < n; i++) base[i] = 1.0 + 0.5 * (double)(nr() % 9); name = "ratings_half"; break;
            }
            double c384 = chao1_of(base, n, 384, samp), het = het_of(base, n, 2048, samp);
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            int ok = 1;
            memcpy(w, base, n * 8); sort_default(w, n, 1); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_gate_v1(w, n, 1); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_mcoff(w, n, 1);   ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
            double a10 = 0, a90 = 0, b10 = 0, b90 = 0;
            double vv = bench_paired(sort_gate_v1, sort_default, base, w, n, 1, pairs, &a10, &a90);
            double vo = bench_paired(sort_default, sort_mcoff, base, w, n, 1, pairs, &b10, &b90);
            printf("  %-13s %9.0f %7.2f %8.3f %6.3f-%-6.3f %8.3f %6.3f-%-6.3f  %s\n", name, c384, het, vv, a10, a90, vo, b10, b90, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
        unsetenv("LOGOS_MCGATE_V1"); unsetenv("LOGOS_MC_OFF");
        free(base); free(w); free(ref);
        return 0;
    }
    if (intness) {
        printf("integer vs non-integer values at equal cardinality and skew: hash map allowed (old gate) over hash map off,\n");
        printf("n=%zu, %d pairs, threads 1 and %d, one binary. ratio > 1.000 means the hash map COSTS that much.\n\n", n, pairs, P);
        printf("  %-14s %9s %9s %7s %4s %2s %9s %9s %9s  %s\n", "shape", "chao1@384", "chao1@2k", "het@2k", "int", "T", "ratio", "p10", "p90", "verify");
        for (int r = 0; r < 6; r++) {
            RS = 8101 + (uint64_t)(r < 4 ? r / 2 : 2);                    /* each int/real pair shares its RNG stream */
            const char *name;
            switch (r) {
            case 0: gen_hash_d(base, n, 2000);       name = "eq2k_real"; break;
            case 1: gen_hash_i(base, n, 2000);       name = "eq2k_int"; break;
            case 2: gen_zipf_d(base, n, 4000, 1.1);  name = "zipf4k_real"; break;
            case 3: gen_zipf_i(base, n, 4000, 1.1);  name = "zipf4k_int"; break;
            case 4: h_session_sec(base, n);          name = "session_sec"; break;
            default: h_session_sec(base, n); for (size_t i = 0; i < n; i++) base[i] += 0.5; name = "session_half"; break;
            }
            double c384 = chao1_of(base, n, 384, samp), c2k = chao1_of(base, n, 2048, samp), het = het_of(base, n, 2048, samp);
            int integral = 1; for (int i = 0; i < 2048; i++) { double v = base[nr() % n]; if (v != floor(v)) { integral = 0; break; } }
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            int ok = 1;
            memcpy(w, base, n * 8); sort_oldgate(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_hashoff(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_oldgate(w, n, 1); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_hashoff(w, n, 1); ok &= out_ok(w, ref, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                double p10 = 0, p90 = 0;
                double med = bench_paired(sort_oldgate, sort_hashoff, base, w, n, tc[ti], pairs, &p10, &p90);
                if (ti == 0) printf("  %-14s %9.0f %9.0f %7.2f %4s %2d %9.3f %9.3f %9.3f  %s\n", name, c384, c2k, het, integral ? "yes" : "no", tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
                else         printf("  %-14s %9s %9s %7s %4s %2d %9.3f %9.3f %9.3f\n", "", "", "", "", "", tc[ti], med, p10, p90);
                fflush(stdout);
            }
        }
        unsetenv("LOGOS_MCGATE_OLD"); unsetenv("LOGOS_MC_HASH_OFF");
        free(base); free(w); free(ref);
        return 0;
    }
    if (verify) {
        /* Two held-out shapes FAILED the memcmp-against-qsort check. Both round a normal draw, so both contain -0.0
         * next to +0.0, which qsort's comparator calls equal and leaves in arbitrary order. Separate a real defect
         * from an oracle artifact: for every variant check (1) ascending under '<', (2) the bitwise multiset equals
         * the input's, (3) how many -0.0 there are, (4) where the first byte mismatch against qsort sits. */
        static const struct { const char *name; gen_fn g; uint64_t seed; } VS[] = {
            { "temp_tenths", h_temp_tenths, 7003 }, { "session_sec", h_session_sec, 7004 }, { "ages", h_ages, 7010 } };
        static const struct { const char *name; bench_sort_fn f; } VV[] = {
            { "default(new gate)", sort_default }, { "route off", sort_mcoff }, { "old gate", sort_oldgate } };
        uint64_t *bi = malloc(n * 8), *bo = malloc(n * 8);
        if (!bi || !bo) return 1;
        for (int s = 0; s < 3; s++) {
            RS = VS[s].seed; VS[s].g(base, n);
            size_t negz = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &base[i], 8); if (u == 0x8000000000000000ULL) negz++; }
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            memcpy(bi, base, n * 8); qsort(bi, n, 8, cmp_u64);
            printf("%s: %zu of %zu values are -0.0\n", VS[s].name, negz, n);
            int tc[2] = { 1, P };
            for (int v = 0; v < 3; v++) for (int ti = 0; ti < 2; ti++) {
                memcpy(w, base, n * 8); VV[v].f(w, n, tc[ti]);
                size_t desc = 0, first_desc = n; for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { if (!desc) first_desc = i; desc++; }
                memcpy(bo, w, n * 8); qsort(bo, n, 8, cmp_u64);
                int same_multiset = memcmp(bi, bo, n * 8) == 0;
                size_t mism = 0, first_m = n; for (size_t i = 0; i < n; i++) if (memcmp(&w[i], &ref[i], 8)) { if (!mism) first_m = i; mism++; }
                size_t nonzero_mism = 0; for (size_t i = 0; i < n; i++) if (memcmp(&w[i], &ref[i], 8) && !(w[i] == 0.0 && ref[i] == 0.0)) nonzero_mism++;
                printf("  %-18s T=%-2d ascending:%s (%zu descents, first at %zu)  multiset:%s  bytes differ from qsort at %zu positions (first %zu), %zu of them NOT a signed-zero swap\n",
                       VV[v].name, tc[ti], desc ? "NO" : "yes", desc, first_desc, same_multiset ? "same" : "DIFFERENT", mism, first_m, nonzero_mism);
                if (desc && first_desc < n) printf("      at the first descent: [%zu]=%.17g  [%zu]=%.17g\n", first_desc - 1, w[first_desc - 1], first_desc, w[first_desc]);
                fflush(stdout);
            }
        }
        unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MCGATE_OLD");
        free(bi); free(bo); free(base); free(w); free(ref);
        return 0;
    }
    if (hroutes) {
        /* which route each held-out shape takes under the new and the old gate: run against a -DLOGOS_TRACE build */
        static const struct { const char *name; gen_fn g; } HR[] = {
            { "product_ids", h_product_ids }, { "gdp_3sig", h_gdp3sig }, { "temp_tenths", h_temp_tenths },
            { "session_sec", h_session_sec }, { "latency_2dp", h_latency_2dp }, { "words_100k", h_words },
            { "hash_2k", h_hash2k }, { "mixed_lat_hash", h_mixed }, { "status_codes", h_status }, { "ages", h_ages } };
        for (int r = 0; r < 10; r++) {
            RS = 7001 + (uint64_t)r; HR[r].g(base, n);
            int tc[2] = { 1, P };
            for (int g = 0; g < 2; g++) for (int ti = 0; ti < 2; ti++) {
                memcpy(w, base, n * 8);
                fprintf(stderr, "=== %s %s T=%d\n", HR[r].name, g ? "old" : "new", tc[ti]);
                (g ? sort_oldgate : sort_default)(w, n, tc[ti]);
            }
        }
        unsetenv("LOGOS_MCGATE_OLD");
        free(base); free(w); free(ref);
        return 0;
    }
    if (gate) {
        printf("training shapes: old gate (LOGOS_MCGATE_OLD=1) over the new gate, n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
        printf("ratio > 1.000 means the new gate is that much faster.\n\n");
        printf("  %-14s %2s %9s %9s %9s  %s\n", "shape", "T", "old/new", "p10", "p90", "verify");
        for (int k = 0; k < NSH; k++) {
            RS = 101 + (uint64_t)k; SH[k].g(base, n);
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            int ok = 1;
            memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_default(w, n, 1); ok &= out_ok(w, ref, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                double p10 = 0, p90 = 0;
                double med = bench_paired(sort_oldgate, sort_default, base, w, n, tc[ti], pairs, &p10, &p90);
                printf("  %-14s %2d %9.3f %9.3f %9.3f  %s\n", SH[k].name, tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
                fflush(stdout);
            }
        }
        unsetenv("LOGOS_MCGATE_OLD");
        free(base); free(w); free(ref);
        return 0;
    }
    if (heldout) {
        static const struct { const char *name; gen_fn g; } HO[] = {
            { "product_ids", h_product_ids }, { "gdp_3sig", h_gdp3sig }, { "temp_tenths", h_temp_tenths },
            { "session_sec", h_session_sec }, { "latency_2dp", h_latency_2dp }, { "words_100k", h_words },
            { "hash_2k", h_hash2k }, { "mixed_lat_hash", h_mixed }, { "status_codes", h_status }, { "ages", h_ages } };
        printf("held-out shapes (never used to design the gate), n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
        printf("old/new = old gate (LOGOS_MCGATE_OLD=1) over the new gate: > 1.000 means the new gate is that much faster.\n");
        printf("new/off = new gate over the route switched off: > 1.000 means what the new gate still lets in COSTS that much.\n\n");
        printf("  %-15s %9s %9s %7s %6s %2s %8s %13s %8s %13s  %s\n", "shape", "chao1@384", "chao1@2k", "het@2k", "d/m2k", "T", "old/new", "p10-p90", "new/off", "p10-p90", "verify");
        for (int r = 0; r < (int)(sizeof HO / sizeof HO[0]); r++) {
            RS = 7001 + (uint64_t)r; HO[r].g(base, n);
            double c384 = chao1_of(base, n, 384, samp), c2k = chao1_of(base, n, 2048, samp), het = het_of(base, n, 2048, samp);
            for (int i = 0; i < M; i++) samp[i] = base[nr() % n];
            qsort(samp, M, 8, cmp_d);
            size_t dd = 0; for (int i = 0; i < M; i++) if (i == 0 || samp[i] != samp[i - 1]) dd++;
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            int ok = 1;
            memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_mcoff(w, n, P);   ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_default(w, n, 1); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_mcoff(w, n, 1);   ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_oldgate(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); sort_oldgate(w, n, 1); ok &= out_ok(w, ref, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                double a10 = 0, a90 = 0, b10 = 0, b90 = 0;
                double on = bench_paired(sort_oldgate, sort_default, base, w, n, tc[ti], pairs, &a10, &a90);
                double nf = bench_paired(sort_default, sort_mcoff, base, w, n, tc[ti], pairs, &b10, &b90);
                if (ti == 0) printf("  %-15s %9.0f %9.0f %7.2f %6.3f %2d %8.3f %6.3f-%-6.3f %8.3f %6.3f-%-6.3f  %s\n", HO[r].name, c384, c2k, het, (double)dd / M, tc[ti], on, a10, a90, nf, b10, b90, ok ? "ok" : "FAILED");
                else         printf("  %-15s %9s %9s %7s %6s %2d %8.3f %6.3f-%-6.3f %8.3f %6.3f-%-6.3f\n", "", "", "", "", "", tc[ti], on, a10, a90, nf, b10, b90);
                fflush(stdout);
            }
        }
        unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF");
        free(base); free(w); free(ref);
        return 0;
    }
    if (skew) {
        static const size_t ZD[] = { 1000, 4000, 16000, 64000 };
        printf("skew ladder, n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
        printf("ratio > 1.000 means the switched-off part COSTS that much when on; < 1.000 means it earns it.\n\n");
        printf("  %-12s %-9s %9s %9s %7s %2s %9s %9s %9s  %s\n", "shape", "switched", "chao1@384", "chao1@2k", "het@2k", "T", "ratio", "p10", "p90", "verify");
        for (int r = 0; r < 6; r++) {
            RS = 901 + (uint64_t)r;
            char name[32]; bench_sort_fn off = sort_hashoff; const char *sw = "hash map";
            if (r < 4) { gen_zipf_d(base, n, ZD[r], 1.1); snprintf(name, sizeof name, "zipf_%zuk", ZD[r] / 1000); }
            else if (r == 4) { g_pareto(base, n); snprintf(name, sizeof name, "zipf_pareto"); }
            else { for (size_t i = 0; i < n; i++) base[i] = (double)(nr() % 32000); snprintf(name, sizeof name, "lat_32000"); off = sort_mcoff; sw = "route"; }
            double c384 = chao1_of(base, n, 384, samp), c2k = chao1_of(base, n, 2048, samp), het = het_of(base, n, 2048, samp);
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            int ok = 1;
            memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); off(w, n, P);          ok &= out_ok(w, ref, n);
            memcpy(w, base, n * 8); off(w, n, 1);          ok &= out_ok(w, ref, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                double p10 = 0, p90 = 0;
                double med = bench_paired(sort_default, off, base, w, n, tc[ti], pairs, &p10, &p90);
                if (ti == 0) printf("  %-12s %-9s %9.0f %9.0f %7.2f %2d %9.3f %9.3f %9.3f  %s\n", name, sw, c384, c2k, het, tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
                else         printf("  %-12s %-9s %9s %9s %7s %2d %9.3f %9.3f %9.3f\n", "", "", "", "", "", tc[ti], med, p10, p90);
                fflush(stdout);
            }
        }
        unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF");
        free(base); free(w); free(ref);
        return 0;
    }
    if (ladder) {
        static const size_t HD[] = { 300, 600, 1200, 2500, 5000, 10000 };
        static const size_t LD[] = { 1000, 4000, 16000, 65536, 200000 };
        printf("cardinality ladder, n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
        printf("ratio > 1.000 means the switched-off part COSTS that much when on; < 1.000 means it earns it.\n");
        for (int s = 0; s < 2; s++) {
            size_t cnt = s == 0 ? sizeof HD / sizeof *HD : sizeof LD / sizeof *LD;
            printf(s == 0 ? "\n  hash map only: default vs LOGOS_MC_HASH_OFF=1 (lattice attempt still runs), d random doubles, equal odds\n"
                          : "\n  whole route: default vs LOGOS_MC_OFF=1, integer lattice 0..d-1, equal odds\n");
            printf("  %-12s %9s %9s %2s %9s %9s %9s  %s\n", "shape", "chao1@384", "chao1@2k", "T", "ratio", "p10", "p90", "verify");
            for (size_t q = 0; q < cnt; q++) {
                size_t d = s == 0 ? HD[q] : LD[q];
                RS = 501 + (uint64_t)(s * 100) + (uint64_t)q;
                if (s == 0) gen_hash_d(base, n, d); else for (size_t i = 0; i < n; i++) base[i] = (double)(nr() % d);
                double c384 = chao1_of(base, n, 384, samp), c2k = chao1_of(base, n, 2048, samp);
                memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
                bench_sort_fn off = s == 0 ? sort_hashoff : sort_mcoff;
                int ok = 1;
                memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
                memcpy(w, base, n * 8); off(w, n, P);          ok &= out_ok(w, ref, n);
                memcpy(w, base, n * 8); off(w, n, 1);          ok &= out_ok(w, ref, n);
                char name[32]; snprintf(name, sizeof name, "%s_%zu", s == 0 ? "hash" : "lat", d);
                int tc[2] = { 1, P };
                for (int ti = 0; ti < 2; ti++) {
                    double p10 = 0, p90 = 0;
                    double med = bench_paired(sort_default, off, base, w, n, tc[ti], pairs, &p10, &p90);
                    if (ti == 0) printf("  %-12s %9.0f %9.0f %2d %9.3f %9.3f %9.3f  %s\n", name, c384, c2k, tc[ti], med, p10, p90, ok ? "ok" : "FAILED");
                    else         printf("  %-12s %9s %9s %2d %9.3f %9.3f %9.3f\n", "", "", "", tc[ti], med, p10, p90);
                    fflush(stdout);
                }
            }
        }
        unsetenv("LOGOS_MC_OFF"); unsetenv("LOGOS_MC_HASH_OFF");
        free(base); free(w); free(ref);
        return 0;
    }
    if (routes) {
        for (int k = 0; k < NSH; k++) {
            RS = 101 + (uint64_t)k; SH[k].g(base, n);
            int tc[2] = { 1, P };
            for (int ti = 0; ti < 2; ti++) {
                memcpy(w, base, n * 8);
                fprintf(stderr, "=== %s T=%d\n", SH[k].name, tc[ti]);
                sort_default(w, n, tc[ti]);
            }
        }
        return 0;
    }

    printf("hash-count route: default dispatcher vs LOGOS_MC_OFF=1, n=%zu, %d pairs, threads 1 and %d, one binary\n", n, pairs, P);
    printf("ratio = default / route off: > 1.000 means the route COSTS that much, < 1.000 means it earns it.\n\n");
    printf("  sample profile, 2048 random positions:\n");
    printf("  %-14s %6s %6s %6s %6s %9s %7s  %-22s\n", "shape", "d/m", "f1/m", "f2", "f3", "chao1", "f1f3/f2^2", "today's gate (1T / MT)");
    for (int k = 0; k < NSH; k++) {
        RS = 101 + (uint64_t)k; SH[k].g(base, n);
        for (int i = 0; i < M; i++) samp[i] = base[nr() % n];
        qsort(samp, M, 8, cmp_d);
        size_t d = 0, f1 = 0, f2 = 0, f3 = 0;
        for (size_t i = 0; i < M;) { size_t j = i + 1; while (j < M && samp[j] == samp[i]) j++; d++; size_t c = j - i; if (c == 1) f1++; else if (c == 2) f2++; else if (c == 3) f3++; i = j; }
        double chao1 = (double)d + (double)f1 * (double)(f1 ? f1 - 1 : 0) / (2.0 * (double)(f2 + 1));
        double het = f2 ? (double)f1 * (double)f3 / ((double)f2 * (double)f2) : -1;
        int g1 = (double)d <= 0.98 * M && chao1 <= 65536, gm = (double)d <= 0.995 * M && chao1 <= 65536;
        printf("  %-14s %6.3f %6.3f %6zu %6zu %9.0f %7.2f    %-6s / %-6s\n", SH[k].name, (double)d / M, (double)f1 / M, f2, f3, chao1, het,
               g1 ? "try" : "skip", gm ? "try" : "skip");
    }
    printf("\n  %-14s %2s %9s %9s %9s  %-32s %s\n", "shape", "T", "ratio", "p10", "p90", "what it is", "verify");
    for (int k = 0; k < NSH; k++) {
        RS = 101 + (uint64_t)k; SH[k].g(base, n);
        memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
        int ok = 1;
        memcpy(w, base, n * 8); sort_default(w, n, P); ok &= out_ok(w, ref, n);
        memcpy(w, base, n * 8); sort_mcoff(w, n, P);   ok &= out_ok(w, ref, n);
        memcpy(w, base, n * 8); sort_default(w, n, 1); ok &= out_ok(w, ref, n);
        memcpy(w, base, n * 8); sort_mcoff(w, n, 1);   ok &= out_ok(w, ref, n);
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double p10 = 0, p90 = 0;
            double med = bench_paired(sort_default, sort_mcoff, base, w, n, tc[ti], pairs, &p10, &p90);
            printf("  %-14s %2d %9.3f %9.3f %9.3f  %-32s %s\n", SH[k].name, tc[ti], med, p10, p90, ti ? "" : SH[k].note, ok ? "ok" : "FAILED");
            fflush(stdout);
        }
    }
    unsetenv("LOGOS_MC_OFF");
    free(base); free(w); free(ref);
    return 0;
}
