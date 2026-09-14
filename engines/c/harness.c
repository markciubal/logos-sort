/* harness.c — correctness + benchmark harness: logos_psort vs C stdlib qsort.
 *
 * Build:  make harness            (or: cc -O3 -pthread logos_psort.c harness.c -o harness -lm)
 * Run:    ./harness               exit code 0 = every correctness check passed
 *
 * CORRECTNESS: for every (distribution x size x thread count), the data is sorted
 * by logos_sort* and, independently, by qsort. The two outputs must agree
 * element-for-element, and an XOR checksum proves the output is a permutation
 * of the input (nothing lost, nothing invented). qsort is the oracle.
 *
 * BENCHMARK: best-of-k wall time of qsort vs logos at 1 thread and at N
 * threads, across data shapes that exercise different paths (uniform, nearly
 * sorted, clustered, heavy-tailed, low-cardinality).
 */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "logos_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- deterministic RNG (xorshift64) ---- */
static uint64_t RS;
static void seed(uint64_t s) { RS = s ? s : 1; }
static uint64_t nextr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double frand(void) { return (double)(nextr() >> 11) / (double)(1ULL << 53); }

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- qsort comparators (the reference) ---- */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int cmp_i(const void *a, const void *b) { int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }

/* ---- data shapes ---- */
enum { D_UNIFORM, D_SORTED, D_REVERSE, D_NEARSORT, D_FEWDIST, D_EQUAL, D_NEG, D_SKEWED, D_CLUSTER,
       D_SEGMENTED, D_FEWRUNS, D_PREFIXSHUF, D_BIMODAL, D_QUANTIZED, D_INTWIDE, D_MODCARD, D_BLOCKPERM, D_BLOCKOVER, D_N };
static const char *DNAME[D_N] = { "uniform", "sorted", "reverse", "near_sorted", "few_distinct",
                                  "all_equal", "negatives", "heavy_tail", "clustered",
                                  "segmented", "few_runs", "prefix_shuffle", "bimodal", "quantized", "int_wide", "mod_card_30k",
                                  "block_perm", "block_overlap" };

static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case D_UNIFORM:  for (i = 0; i < n; i++) a[i] = frand() * 2e9 - 1e9; break;
    case D_SORTED:   for (i = 0; i < n; i++) a[i] = (double)i; break;
    case D_REVERSE:  for (i = 0; i < n; i++) a[i] = (double)(n - i); break;
    case D_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i;
                     for (i = 0; n > 0 && i < n / 100; i++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
                     break;
    case D_FEWDIST:  for (i = 0; i < n; i++) a[i] = (double)(nextr() % 5) * 1e6; break;
    case D_EQUAL:    for (i = 0; i < n; i++) a[i] = 42.0; break;
    case D_NEG:      for (i = 0; i < n; i++) a[i] = frand() * 2e6 - 1e6; break;
    case D_SKEWED:   for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2); break; /* Pareto tail */
    case D_CLUSTER:  for (i = 0; i < n; i++) { double s = 0; int k; for (k = 0; k < 4; k++) s += frand(); a[i] = (s - 2.0) * 1e6; } break;
    /* many (>=16) sorted blocks -> 4-way merge path */
    case D_SEGMENTED: { size_t blk = n / 64 ? n / 64 : 1; for (i = 0; i < n; i++) a[i] = (double)(i % blk) * 64.0 + (double)(i / blk); } break;
    /* 2-8 long runs -> galloping 2-way merge */
    case D_FEWRUNS:  { size_t blk = n / 4 ? n / 4 : 1; for (i = 0; i < n; i++) a[i] = (double)(i % blk) * 4.0 + (double)(i / blk); } break;
    /* sorted with the first 10% shuffled -> momentum insertion must ABORT and fall through */
    case D_PREFIXSHUF: for (i = 0; i < n; i++) a[i] = (double)i;
                       { size_t m = n / 10; for (i = m; i > 1; i--) { size_t j = nextr() % i; double t = a[i - 1]; a[i - 1] = a[j]; a[j] = t; } } break;
    /* two tight clusters far apart -> gap-split partition */
    case D_BIMODAL:  for (i = 0; i < n; i++) a[i] = (nextr() & 1 ? 1e9 : -1e9) + frand() * 10.0; break;
    /* dyadic fractions (few active mantissa bytes) -> quantized float radix */
    case D_QUANTIZED: for (i = 0; i < n; i++) a[i] = (double)(nextr() % 4096) / 64.0; break;
    /* integer-valued doubles across the full int32 range -> byte-skip int32 radix */
    case D_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nextr(); break;
    /* ~30k distinct non-integer values -> moderate-cardinality hash-map counting */
    case D_MODCARD:  for (i = 0; i < n; i++) a[i] = (double)(nextr() % 30000) * 1.5; break;
    /* 64 sorted blocks with DISJOINT value ranges, concatenated in shuffled order (merged log
     * partitions) -> disjoint-run relocation (J5): memcpy, no merge. Fractional values so the
     * quantized-radix probe does not claim them first. block_overlap: every value-block whose
     * index is 1 mod 8 starts 10% inside its predecessor, so a quarter of the data sits in
     * two-run chains and the chain-merge path runs. */
    case D_BLOCKPERM: case D_BLOCKOVER: {
        size_t nb = 64, blk = (n + nb - 1) / nb; if (blk < 1) blk = 1;
        size_t perm[64]; for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t r = nextr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
        for (i = 0; i < n; i++) {
            size_t b = perm[i / blk]; double base = (double)b * (double)blk;
            if (d == D_BLOCKOVER && (b & 7) == 1) base -= (double)blk * 0.1;
            a[i] = base + (double)(i % blk) + frand() * 0.5;
        }
    } break;
    }
}

/* XOR of raw bits: invariant under permutation, so equal-before/after proves
   the output is a rearrangement of the input. */
static uint64_t xsum(const double *a, size_t n) {
    uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x;
}

/* ================= correctness ================= */
static int correctness(void) {
    const size_t sizes[] = { 0, 1, 2, 63, 64, 65, 1000, 50000, 1000000 };
    const int threads[] = { 1, 2, 4, 8 };
    int checks = 0, fails = 0;
    seed(0x9E3779B97F4A7C15ULL);

    printf("== correctness: logos vs qsort (double) ==\n");
    for (int d = 0; d < D_N; d++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc((n ? n : 1) * sizeof(double));
            double *ref  = malloc((n ? n : 1) * sizeof(double));
            gen(orig, n, d);
            memcpy(ref, orig, n * sizeof(double));
            qsort(ref, n, sizeof(double), cmp_d);              /* the oracle */
            uint64_t before = xsum(orig, n);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                double *a = malloc((n ? n : 1) * sizeof(double));
                memcpy(a, orig, n * sizeof(double));
                logos_sort_f64(a, n, threads[ti]);
                int ok = xsum(a, n) == before;
                for (size_t i = 0; ok && i < n; i++) if (a[i] != ref[i]) ok = 0;
                checks++;
                if (!ok) { fails++; printf("  FAIL  shape=%-12s n=%-8zu threads=%d\n", DNAME[d], n, threads[ti]); }
                free(a);
            }
            free(orig); free(ref);
        }
    }
    printf("  double: %d checks, %d failures\n", checks, fails);

    printf("== correctness: logos vs qsort (int32) ==\n");
    int ichecks = 0, ifails = 0;
    const size_t isz[] = { 0, 1, 1000, 8192, 200000, 1000000 };
    /* Modes 3..11 exercise the parallel counting route (count_try_i32) and its edges: the span-1 and
       span-2 ends, the old fixed table cap and one past it, all-negative and zero-straddling data, the
       INT32_MIN/INT32_MAX pair whose span overflows 32-bit arithmetic, and far sentinels mixed into a
       narrow bulk. See FINDINGS 3w. Modes 12..16 (experiments/team/i32-sentinel-seq.md): the table-budget
       band above the old cap (span n/2), and the sequential leaf's windowed counting at 1 thread: a rare
       multi-valued low tail that needs its own recursive sort, tails on BOTH sides whose span overflows
       32-bit arithmetic, values on the exact window edges, and a tail too frequent for the trim (declines
       at the sample). n = 8192 sits just above LEAF_I32_WINDOW_MIN_N and below MIN_PARALLEL. */
    for (int mode = 0; mode < 17; mode++) {
        for (size_t si = 0; si < sizeof isz / sizeof *isz; si++) {
            size_t n = isz[si];
            int32_t *orig = malloc((n ? n : 1) * sizeof(int32_t));
            int32_t *ref  = malloc((n ? n : 1) * sizeof(int32_t));
            for (size_t i = 0; i < n; i++) {
                uint64_t r = nextr();
                switch (mode) {
                case 0:  orig[i] = (int32_t)r;                  break;   /* full range */
                case 1:  orig[i] = (int32_t)(r % 7) - 3;        break;   /* few distinct, signed */
                case 2:  orig[i] = (int32_t)i;                  break;   /* already sorted */
                case 3:  orig[i] = 42;                          break;   /* span 1: route declines */
                case 4:  orig[i] = (int32_t)(r & 1);            break;   /* span 2: smallest accepted */
                case 5:  orig[i] = (int32_t)(r % 262144);       break;   /* span == the table cap */
                case 6:  orig[i] = (int32_t)(r % 262145);       break;   /* one past the cap: declines */
                case 7:  orig[i] = -(int32_t)(r % 100) - 1;     break;   /* all negative */
                case 8:  orig[i] = (int32_t)(r % 100) - 50;     break;   /* straddles zero */
                case 9:  orig[i] = (r & 1) ? INT32_MIN : INT32_MAX; break; /* span overflows in 32-bit */
                case 10: orig[i] = (int32_t)(r % 100);          break;   /* narrow bulk + sentinels below */
                case 11: orig[i] = INT32_MIN;                   break;   /* every element the minimum */
                case 12: orig[i] = (int32_t)(r % (n / 2 + 1));  break;   /* span n/2: past the old cap, inside the budget */
                case 13: orig[i] = (i % 1024 == 7) ? INT32_MIN + (int32_t)(r % 1000000) : (int32_t)(r % 100); break; /* 0.1% low tail, 1e6 wide, far away */
                case 14: orig[i] = (i % 1024 == 7) ? INT32_MIN + (int32_t)(r % 1000) : (i % 1024 == 500) ? INT32_MAX - (int32_t)(r % 1000) : (int32_t)(r % 100); break; /* both tails, span 2^32 */
                case 15: orig[i] = (i % 4096 == 1) ? -1 : (i % 4096 == 2) ? 100 : (int32_t)(r % 100); break; /* window edges: -1 and 100 next to a 0..99 bulk */
                default: orig[i] = (i % 40 == 3) ? INT32_MIN : (int32_t)(r % 100); break;   /* 2.5% sentinel: the sample sees it, declines */
                }
            }
            if (mode == 10 && n > 3) { orig[n / 2] = INT32_MIN; orig[n / 3] = INT32_MAX; }
            if (mode == 15 && n > 3) { orig[n / 2] = INT32_MIN; }
            memcpy(ref, orig, n * sizeof(int32_t));
            qsort(ref, n, sizeof(int32_t), cmp_i);
            for (int t = 1; t <= 16; t *= 4) {
                int32_t *a = malloc((n ? n : 1) * sizeof(int32_t));
                memcpy(a, orig, n * sizeof(int32_t));
                logos_sort_i32(a, n, t);
                int ok = memcmp(a, ref, n * sizeof(int32_t)) == 0;
                ichecks++;
                if (!ok) { ifails++; printf("  FAIL  int32 mode=%d n=%zu threads=%d\n", mode, n, t); }
                free(a);
            }
            free(orig); free(ref);
        }
    }
    printf("  int32:  %d checks, %d failures\n\n", ichecks, ifails);
    return fails + ifails;
}

/* ================= benchmark ================= */
static double best_qsort(const double *base, size_t n, int reps) {
    double best = 1e300; double *a = malloc(n * sizeof(double));
    for (int r = 0; r < reps; r++) { memcpy(a, base, n * sizeof(double)); double t = now_ms(); qsort(a, n, sizeof(double), cmp_d); t = now_ms() - t; if (t < best) best = t; }
    free(a); return best;
}
static double best_logos(const double *base, size_t n, int threads, int reps) {
    double best = 1e300; double *a = malloc(n * sizeof(double));
    for (int r = 0; r < reps; r++) {
        memcpy(a, base, n * sizeof(double)); double t = now_ms(); logos_sort_f64(a, n, threads); t = now_ms() - t;
        for (size_t i = 1; i < n; i++) if (a[i - 1] > a[i]) { printf("  !! unsorted output in benchmark\n"); break; }
        if (t < best) best = t;
    }
    free(a); return best;
}

static void benchmark(void) {
    int cpus = logos_num_cpus();
    int P = cpus > 1 ? cpus : 4;            /* on a 1-core box, P=4 shows overhead honestly */
    const size_t sizes[] = { 1000000, 8000000 };
    const int dists[] = { D_UNIFORM, D_NEARSORT, D_CLUSTER, D_FEWDIST, D_SKEWED, D_SEGMENTED, D_PREFIXSHUF, D_BIMODAL, D_QUANTIZED, D_INTWIDE, D_MODCARD, D_BLOCKPERM, D_BLOCKOVER };

    printf("== benchmark: double, best-of-k wall time (online CPUs: %d) ==\n", cpus);
    printf("  %-12s %6s   %9s   %11s %7s   %11s %7s\n", "shape", "n", "qsort", "logos t=1", "x", "logos t=" , "x");
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si]; int reps = n >= 8000000 ? 2 : 3;
        double *base = malloc(n * sizeof(double));
        for (size_t di = 0; di < sizeof dists / sizeof *dists; di++) {
            seed(7); gen(base, n, dists[di]);
            double tq = best_qsort(base, n, reps);
            double t1 = best_logos(base, n, 1, reps);
            double tp = best_logos(base, n, P, reps);
            printf("  %-12s %5zuM   %7.1fms   %9.1fms %6.2fx   %7.1fms(%d) %6.2fx\n",
                   DNAME[dists[di]], n / 1000000, tq, t1, tq / t1, tp, P, tq / tp);
        }
        free(base);
    }

    /* int32 */
    size_t n = 8000000; int32_t *bi = malloc(n * sizeof(int32_t));
    seed(11); for (size_t i = 0; i < n; i++) bi[i] = (int32_t)nextr();
    double tq = 1e300, tl = 1e300; int32_t *a = malloc(n * sizeof(int32_t));
    for (int r = 0; r < 2; r++) { memcpy(a, bi, n * 4); double t = now_ms(); qsort(a, n, 4, cmp_i); t = now_ms() - t; if (t < tq) tq = t; }
    for (int r = 0; r < 2; r++) { memcpy(a, bi, n * 4); double t = now_ms(); logos_sort_i32(a, n, 1); t = now_ms() - t; if (t < tl) tl = t; }
    printf("\n== benchmark: int32 8M uniform ==\n  qsort %7.1fms   logos t=1 %7.1fms   %.2fx\n", tq, tl, tq / tl);
    free(a); free(bi);

    if (cpus == 1)
        printf("\n  note: this machine has ONE core. The t=%d column measures thread-coordination\n"
               "  overhead, not parallel speedup; on real cores it drops toward t=1 / cores.\n", P);
}


/* ---- parity extras: argSort family, findOutliers, strings ---- */
static int extras_checks(void) {
    int fails = 0, checks = 0;
    seed(0xC0FFEE);
    for (size_t n = 1; n <= 200000; n = n * 17 + 1) {
        double *a = malloc(n * 8); uint32_t *p = malloc(n * 4);
        for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % 40);
        logos_argsort_f64(a, n, p);
        int ok = 1; char *seen = calloc(n, 1);
        for (size_t i = 0; i < n; i++) { if (p[i] >= n || seen[p[i]]) { ok = 0; break; } seen[p[i]] = 1; }
        for (size_t i = 1; ok && i < n; i++) { if (a[p[i-1]] > a[p[i]]) ok = 0; else if (a[p[i-1]] == a[p[i]] && p[i-1] > p[i]) ok = 0; }
        free(seen); checks++; if (!ok) { fails++; printf("  FAIL argsort_f64 n=%zu\n", n); }
        free(a); free(p);
    }
    { size_t n = 200000; double *a = malloc(n * 8); for (size_t i = 0; i < n; i++) a[i] = (double)i;
      a[7] = 1e18; a[n - 4] = -1e18; uint32_t out[16]; size_t c = logos_find_outliers_f64(a, n, out, 16, 0);
      int f7 = 0, fn = 0; for (size_t i = 0; i < c && i < 16; i++) { if (out[i] == 7) f7 = 1; if (out[i] == n - 4) fn = 1; }
      checks++; if (!(c >= 2 && f7 && fn)) { fails++; printf("  FAIL findOutliers\n"); } free(a); }
    { const char *s[] = {"delta","alpha","charlie","alpha","","bravo","al"}; size_t n = 7; logos_sort_strings(s, n);
      int ok = 1; for (size_t i = 1; i < n; i++) if (strcmp(s[i-1], s[i]) > 0) ok = 0; checks++; if (!ok) { fails++; printf("  FAIL strings\n"); } }
    printf("  extras: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the sorted view (logos_view.h): every query is checked against the qsort reference ----
 * Shapes are chosen so both outcomes of the partition are exercised: ones that reach the lazy
 * partition (a view with many buckets) and ones a route sorts outright (a view with one finished
 * bucket: few_distinct and near_sorted for f64; span <= 2^18, few distinct, sorted for int32). The
 * quantile list has out-of-range, duplicate and unordered cuts on purpose, and the rank/count probes
 * sit on values that exist in the data as well as between them. */
static size_t lb_d(const double *r, size_t n, double x) { size_t l = 0, h = n; while (l < h) { size_t m = (l + h) >> 1; if (r[m] < x) l = m + 1; else h = m; } return l; }
static size_t lb_i(const int32_t *r, size_t n, int32_t x) { size_t l = 0, h = n; while (l < h) { size_t m = (l + h) >> 1; if (r[m] < x) l = m + 1; else h = m; } return l; }
static const double VQ[13] = { 0.5, 0.0, 1.0, 0.5, 0.25, 0.75, 0.1, 0.9, -1.0, 2.0, 0.501, 0.499, 0.333 };
static size_t vq_rank(double q, size_t n) { if (!(q > 0)) q = 0; if (q > 1) q = 1; return (size_t)(q * (double)(n - 1) + 0.5); }
static int view_checks(void) {
    int fails = 0, checks = 0;
    const size_t sizes[] = { 2, 1000, 5000, 50000, 300000, 1000000 };
    const int threads[] = { 1, 4, 16 };
    const int dists[] = { D_UNIFORM, D_FEWDIST, D_NEARSORT, D_BIMODAL, D_SKEWED, D_CLUSTER, D_INTWIDE, D_EQUAL };
    seed(0x5EEDF00DULL);
    for (size_t di = 0; di < sizeof dists / sizeof *dists; di++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * 8), *ref = malloc(n * 8), *a = malloc(n * 8);
            gen(orig, n, dists[di]); memcpy(ref, orig, n * 8); qsort(ref, n, 8, cmp_d);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * 8);
                logos_view *v = logos_view_create_f64(a, n, threads[ti]);
                int ok = v != NULL;
                if (ok) {
                    size_t rk[5] = { n / 2, 0, n - 1, n / 3, (7 * n) / 9 };
                    for (int i = 0; ok && i < 5; i++) ok = logos_view_at(v, rk[i]) == ref[rk[i]];
                    double qo[13]; logos_view_quantiles(v, VQ, 13, qo);
                    for (int i = 0; ok && i < 13; i++) ok = qo[i] == ref[vq_rank(VQ[i], n)];
                    double xs[4] = { ref[n / 3], ref[n / 3] + 0.5, ref[0] - 1.0, ref[n - 1] + 1.0 };
                    for (int i = 0; ok && i < 4; i++) ok = logos_view_rank(v, xs[i]) == lb_d(ref, n, xs[i]);
                    ok = ok && logos_view_count_in_range(v, ref[n / 4], ref[(3 * n) / 4]) == lb_d(ref, n, ref[(3 * n) / 4]) - lb_d(ref, n, ref[n / 4]);
                    size_t k = n < 200 ? n : 200; const double *top = logos_view_topk(v, k);
                    ok = ok && top && memcmp(top, ref + n - k, k * 8) == 0;
                    logos_view_all(v);
                    ok = ok && memcmp(a, ref, n * 8) == 0 && logos_view_materialised(v) == logos_view_nbuckets(v);
                    logos_view_free(v);
                }
                checks++; if (!ok) { fails++; printf("  FAIL  view f64 shape=%s n=%zu threads=%d\n", DNAME[dists[di]], n, threads[ti]); }
            }
            free(orig); free(ref); free(a);
        }
    }
    printf("  view f64: %d checks, %d failures\n", checks, fails);
    int ichecks = 0, ifails = 0;
    for (int mode = 0; mode < 8; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            int32_t *orig = malloc(n * 4), *ref = malloc(n * 4), *a = malloc(n * 4);
            for (size_t i = 0; i < n; i++) {
                uint64_t r = nextr();
                switch (mode) {
                case 0:  orig[i] = (int32_t)r;                          break;   /* full range: partition */
                case 1:  orig[i] = (int32_t)(r % 100000000);            break;   /* span 1e8: partition */
                case 2:  orig[i] = (int32_t)(r % 200000);               break;   /* span 2e5: counting sorts outright */
                case 3:  orig[i] = (int32_t)(r % 7) - 3;                break;   /* few distinct */
                case 4:  orig[i] = (int32_t)i;                          break;   /* sorted */
                case 5:  orig[i] = (r & 1) ? INT32_MIN : INT32_MAX;     break;   /* the two extremes only */
                case 6:  orig[i] = (int32_t)((r % 16) * 100000000u + (nextr() % 100000)); break; /* 16 clusters */
                default: orig[i] = (int32_t)(r % 100);                  break;   /* narrow bulk + far sentinels */
                }
            }
            if (mode == 7 && n > 3) { orig[n / 2] = INT32_MIN; orig[n / 3] = INT32_MAX; }
            memcpy(ref, orig, n * 4); qsort(ref, n, 4, cmp_i);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * 4);
                logos_view_i32 *v = logos_view_create_i32(a, n, threads[ti]);
                int ok = v != NULL;
                if (ok) {
                    size_t rk[5] = { n / 2, 0, n - 1, n / 3, (7 * n) / 9 };
                    for (int i = 0; ok && i < 5; i++) ok = logos_view_at_i32(v, rk[i]) == ref[rk[i]];
                    int32_t qo[13]; logos_view_quantiles_i32(v, VQ, 13, qo);
                    for (int i = 0; ok && i < 13; i++) ok = qo[i] == ref[vq_rank(VQ[i], n)];
                    int32_t xs[4] = { ref[n / 3], ref[n / 3] == INT32_MAX ? INT32_MAX : ref[n / 3] + 1, INT32_MIN, INT32_MAX };
                    for (int i = 0; ok && i < 4; i++) ok = logos_view_rank_i32(v, xs[i]) == lb_i(ref, n, xs[i]);
                    ok = ok && logos_view_count_in_range_i32(v, ref[n / 4], ref[(3 * n) / 4]) == lb_i(ref, n, ref[(3 * n) / 4]) - lb_i(ref, n, ref[n / 4]);
                    size_t k = n < 200 ? n : 200; const int32_t *top = logos_view_topk_i32(v, k);
                    ok = ok && top && memcmp(top, ref + n - k, k * 4) == 0;
                    logos_view_all_i32(v);
                    ok = ok && memcmp(a, ref, n * 4) == 0 && logos_view_materialised_i32(v) == logos_view_nbuckets_i32(v);
                    logos_view_free_i32(v);
                }
                ichecks++; if (!ok) { ifails++; printf("  FAIL  view i32 mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
            }
            free(orig); free(ref); free(a);
        }
    }
    printf("  view i32: %d checks, %d failures\n", ichecks, ifails);
    return fails + ifails;
}

/* ---- the dense-integer route: it must fire when it should, DECLINE when it must, and be invisible
 * either way. Every case is checked against the qsort oracle, so a wrong decision shows up as a wrong
 * answer rather than as a slow one. The decline cases are the ones worth having: a single missing or
 * repeated value, a span just past the budget, and a non-integer hidden in the middle. ---- */
static int dense_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4 };
    /* n below and above DENSE_MIN_N (65536), and one that is not a power of two */
    const size_t sizes[] = { 65535, 65536, 100003, 300000 };
    for (int mode = 0; mode < 9; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double));
            if (!orig || !ref) { free(orig); free(ref); continue; }
            seed(9876 + mode * 31 + si);
            for (size_t i = 0; i < n; i++) {
                switch (mode) {
                case 0: orig[i] = (double)(i ^ (i >> 1)); break;                  /* dense permutation */
                case 1: orig[i] = (double)(n - 1 - i); break;                     /* descending 0..n-1 */
                case 2: orig[i] = (double)i * 4.0; break;                         /* span 4n: at the budget */
                case 3: orig[i] = (double)i * 8.0; break;                         /* span 8n: must decline */
                case 4: orig[i] = (double)i - (double)(n / 2); break;             /* negative base */
                case 5: orig[i] = (double)(i ^ (i >> 1)); break;                  /* + one repeat, below */
                case 6: orig[i] = (double)(i ^ (i >> 1)); break;                  /* + one gap, below */
                case 7: orig[i] = (double)(i ^ (i >> 1)); break;                  /* + one non-integer */
                default: orig[i] = (double)(nextr() % (n / 2 ? n / 2 : 1)); break;/* dense with many repeats */
                }
            }
            if (mode == 5 && n > 3) orig[n / 2] = orig[n / 3];                     /* exactly one repeat */
            if (mode == 6 && n > 3) orig[n / 2] = (double)(2 * n);                 /* one value far outside */
            if (mode == 7 && n > 3) orig[n / 2] = 0.5;                             /* one non-integer */
            memcpy(ref, orig, n * sizeof(double));
            qsort(ref, n, sizeof(double), cmp_d);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                double *a = malloc(n * sizeof(double));
                if (!a) continue;
                memcpy(a, orig, n * sizeof(double));
                logos_sort_f64(a, n, threads[ti]);
                int ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++;
                if (!ok) { fails++; printf("  FAIL  dense mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
                free(a);
            }
            free(orig); free(ref);
        }
    }
    printf("  dense int: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the sorted-prefix-plus-tail route: correct at the gate's edges, and unchanged when it declines.
 * Every ordinary case is checked against the qsort oracle. The NaN case cannot use qsort as an oracle, since
 * its comparator has no total order over NaN, so it checks the property that matters for this route instead:
 * with a NaN present the route must decline, so the output must be bit-identical to a run with the route
 * switched off. One thread keeps that comparison deterministic. ---- */
static int tail_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 4 };              /* the third runs with LOGOS_TAIL_SEQ=1: parallel sorts decline the route */
    const size_t sizes[] = { 1000, 65536, 300001, 1000003 };
    for (int mode = 0; mode < 10; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double));
            if (!orig || !ref) { free(orig); free(ref); continue; }
            seed(4242 + (uint64_t)mode * 17 + si);
            size_t d;
            switch (mode) {
            case 0: d = n - n / 20; break;          /* 95% sorted + 5% tail */
            case 1: d = n / 2; break;               /* exactly half: the gate's edge */
            case 2: d = n - 1; break;               /* one-element tail */
            case 8: d = n / 2 - 1; break;           /* just under half: the route must NOT fire */
            case 9: d = n / 4; break;               /* short prefix: must not fire */
            default: d = n - n / 10; break;
            }
            for (size_t i = 0; i < d; i++) orig[i] = (double)i;                 /* ascending prefix 0..d-1 */
            for (size_t i = d; i < n; i++) {
                switch (mode) {
                case 3: orig[i] = (double)d + 1000.0 + frand() * 1e6; break;   /* tail all above the prefix */
                case 4: orig[i] = -1.0 - frand() * 1e6; break;                 /* tail all below the prefix */
                case 5: orig[i] = (double)(n - i) * 3.0; break;                /* tail descending, overlapping */
                case 6: orig[i] = (double)(d / 2); break;                      /* tail of one repeated key */
                default: orig[i] = frand() * (double)n; break;                 /* tail interleaving the prefix */
                }
            }
            if (mode == 6 && d > 2) orig[d / 2 - 1] = (double)(d / 2);         /* equal keys across the merge */
            if (mode == 7) {                                                   /* NaN: must decline, bit-identical */
                if (n > 10) orig[n - 3] = NAN;
                double *a1 = malloc(n * sizeof(double)), *a2 = malloc(n * sizeof(double));
                if (a1 && a2) {
                    memcpy(a1, orig, n * sizeof(double)); unsetenv("LOGOS_NO_TAIL");      logos_sort_f64(a1, n, 1);
                    memcpy(a2, orig, n * sizeof(double)); setenv("LOGOS_NO_TAIL", "1", 1); logos_sort_f64(a2, n, 1);
                    unsetenv("LOGOS_NO_TAIL");
                    int ok = memcmp(a1, a2, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  tail NaN n=%zu\n", n); }
                    /* and in the parallel form, with a second NaN inside the PREFIX, which the prescan walks past */
                    orig[n / 3] = NAN;
                    memcpy(a1, orig, n * sizeof(double));                                     logos_sort_f64(a1, n, 4);
                    memcpy(a2, orig, n * sizeof(double)); setenv("LOGOS_NO_TAIL", "1", 1);      logos_sort_f64(a2, n, 4);
                    unsetenv("LOGOS_NO_TAIL");
                    ok = memcmp(a1, a2, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  tail NaN parallel n=%zu\n", n); }
                }
                free(a1); free(a2); free(orig); free(ref);
                continue;
            }
            memcpy(ref, orig, n * sizeof(double));
            qsort(ref, n, sizeof(double), cmp_d);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                double *a = malloc(n * sizeof(double)); if (!a) continue;
                memcpy(a, orig, n * sizeof(double));
                if (ti == 2) setenv("LOGOS_TAIL_SEQ", "1", 1);
                logos_sort_f64(a, n, threads[ti]);
                if (ti == 2) unsetenv("LOGOS_TAIL_SEQ");
                int ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  tail mode=%d n=%zu threads=%d%s\n", mode, n, threads[ti], ti == 2 ? " tail_seq" : ""); }
                free(a);
            }
            free(orig); free(ref);
        }
    }
    printf("  tail route: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the interleaved-streams route: every mode count it must handle, the one it must refuse, a stream that is
 * only mostly ascending (the per-bucket fallback), ties, negatives, and a gate that must decline on bimodal random
 * data. Checked against the qsort oracle; the NaN case instead checks the route leaves behaviour bit-identical
 * to running with it switched off. ---- */
static int streams_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4 };
    const size_t sizes[] = { 1000, 65536, 300001 };
    for (int mode = 0; mode < 11; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double));
            size_t S = mode == 2 ? 2 : mode == 3 ? 60 : mode == 4 ? 70 : mode == 9 ? 3 : 8;
            size_t *ctr = calloc(S, sizeof *ctr);
            if (!orig || !ref || !ctr) { free(orig); free(ref); free(ctr); continue; }
            seed(777 + (uint64_t)mode * 31 + si);
            for (size_t i = 0; i < n; i++) {
                if (mode == 6) { orig[i] = (nextr() & 1 ? 1e9 : -1e9) + frand() * 10.0; continue; }   /* bimodal random */
                size_t st = (size_t)(nextr() % S);
                double base = (double)st * 1e9 - (mode == 7 ? 4e10 : 0.0);
                if (mode == 9) base = st == 0 ? 0.0 : st == 1 ? 1e6 : 1e12;   /* two boundaries share the table's first cell */
                orig[i] = base + (double)(mode == 5 ? ctr[st] / 3 : ctr[st]);                       /* mode 5: ties */
                ctr[st]++;
            }
            if (mode == 1)                                          /* a few values of stream 0 out of arrival order */
                for (int k = 0, done = 0; done < 5 && k < 100000; k++) { size_t i = (size_t)(nextr() % n); if (orig[i] < 1e9 && orig[i] >= 0) { orig[i] = frand() * (double)(ctr[0] ? ctr[0] : 1); done++; } }
            if (mode == 10) {                                       /* +inf ends the last stream: an infinite gulf */
                size_t seen = 0;
                for (size_t i = 0; i < n; i++) if (orig[i] >= (double)(S - 1) * 1e9) { if (seen++ >= ctr[S - 1] / 2) orig[i] = INFINITY; }
            }
            if (mode == 8) {                                        /* NaN: the route must decline, bit-identical */
                if (n > 10) orig[n / 2] = NAN;
                double *a1 = malloc(n * sizeof(double)), *a2 = malloc(n * sizeof(double));
                if (a1 && a2) {
                    memcpy(a1, orig, n * sizeof(double)); unsetenv("LOGOS_NO_STREAMS");      logos_sort_f64(a1, n, 1);
                    memcpy(a2, orig, n * sizeof(double)); setenv("LOGOS_NO_STREAMS", "1", 1); logos_sort_f64(a2, n, 1);
                    unsetenv("LOGOS_NO_STREAMS");
                    int ok = memcmp(a1, a2, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  streams NaN n=%zu\n", n); }
                }
                free(a1); free(a2); free(orig); free(ref); free(ctr);
                continue;
            }
            memcpy(ref, orig, n * sizeof(double));
            qsort(ref, n, sizeof(double), cmp_d);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                double *a = malloc(n * sizeof(double)); if (!a) continue;
                memcpy(a, orig, n * sizeof(double));
                logos_sort_f64(a, n, threads[ti]);
                int ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  streams mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
                free(a);
            }
            free(orig); free(ref); free(ctr);
        }
    }
    printf("  streams route: %d checks, %d failures\n", checks, fails);
    return fails;
}

static int cmp_bits(const void *x, const void *y) { uint64_t a, b; memcpy(&a, x, 8); memcpy(&b, y, 8); return (a > b) - (a < b); }
/* ---- signed zeros. -0.0 == +0.0 under '<', so a byte compare against qsort cannot judge them (it failed two correct
 * sorts in make cardgate). Checked here as: ascending under '<', and the output's bitwise multiset equals the input's,
 * so no -0.0 is lost, invented or turned into +0.0 by any route (hash count keys on the bits, lattice, partition). ---- */
static int signed_zero_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4 };
    const size_t sizes[] = { 1000, 65536, 300001 };
    for (int mode = 0; mode < 6; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
            seed(9100 + (uint64_t)mode * 13 + si);
            for (size_t i = 0; i < n; i++) {
                double g = frand() + frand() + frand() + frand() - 2.0;     /* roughly normal around 0 */
                double x; long k; double v;
                switch (mode) {
                case 0: x = 60.0 * g; k = (long)(x + (x >= 0 ? 0.5 : -0.5)); v = (double)k / 10.0; if (k == 0 && x < 0) v = -0.0; break;  /* tenths */
                case 1: x = 30.0 * g; k = (long)(x + (x >= 0 ? 0.5 : -0.5)); v = (double)k; if (k == 0 && x < 0) v = -0.0; break;         /* integers */
                case 2: { static const double few[4] = { -0.0, 0.0, 1.0, 2.0 }; v = few[nextr() % 4]; break; }                           /* few distinct */
                case 3: v = (nextr() % 100 == 0) ? ((nextr() & 1) ? -0.0 : 0.0) : frand() * 2e6 - 1e6; break;                           /* random + 1% zeros */
                case 4: v = (double)i; break;                                                                                             /* dense permutation, below */
                default: v = (nextr() % 100 == 0) ? -0.0 : (double)((long)(nextr() % 1000001) - 500000); break;                           /* wide integers + 1% -0.0 */
                }
                orig[i] = v;
            }
            if (mode == 4) {                           /* a shuffled permutation of 0..n-1 whose zero is -0.0: the dense-integer route */
                for (size_t i = n - 1; i > 0; i--) { size_t j = (size_t)(nextr() % (i + 1)); double t = orig[i]; orig[i] = orig[j]; orig[j] = t; }
                for (size_t i = 0; i < n; i++) if (orig[i] == 0.0) orig[i] = -0.0;
            }
            memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                logos_sort_f64(a, n, threads[ti]);
                int ok = 1;
                for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  signed zeros mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
            }
            free(orig); free(a); free(bi); free(bo);
        }
    }
    printf("  signed zeros: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the key grid (mode 10 of the histogram-free partition): heavy tails, both signs, and a range too narrow for the grid,
 * through a parallel sort. Checked as ascending under '<' plus an identical bitwise multiset. ---- */
static int kgrid_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 4, 8 };
    const size_t sizes[] = { 300001, 1000003 };
    for (int mode = 0; mode < 8; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
            seed(9900 + (uint64_t)mode * 7 + si);
            for (size_t i = 0; i < n; i++) {
                double u = 1.0 - frand(), g = sqrt(-2.0 * log(1.0 - frand())) * cos(6.283185307179586 * frand()), v;
                switch (mode) {
                case 0: v = pow(u, -1.0 / 0.5); break;                                  /* Pareto 0.5: very heavy */
                case 1: v = 1000.0 * pow(u, -1.0 / 1.2); break;                         /* the bench's heavy_tail */
                case 2: v = pow(u, -1.0 / 3.0); break;                                  /* Pareto 3: light tail */
                case 3: v = exp(2.0 * g); break;                                        /* lognormal */
                case 4: v = -log(u) * 1e3; break;                                       /* exponential */
                case 5: v = -1000.0 * pow(u, -1.0 / 1.2); break;                        /* negative heavy tail */
                case 6: v = ((nextr() & 1) ? 1.0 : -1.0) * pow(u, -1.0 / 1.2); break;   /* heavy tails on both signs */
                default: v = 1.0 + 1e-12 * frand(); break;                              /* a range the grid cannot resolve */
                }
                orig[i] = v;
            }
            memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                logos_sort_f64(a, n, threads[ti]);
                int ok = 1;
                for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  key grid mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
            }
            free(orig); free(a); free(bi); free(bo);
        }
    }
    printf("  key grid: %d checks, %d failures\n", checks, fails);
    return fails;
}

int main(void) {
    printf("logos-sort C harness — reference oracle: stdlib qsort\n\n");
    int fails = correctness();
    printf("== parity extras ==\n");
    fails += extras_checks();
    printf("\n");
    printf("== sorted prefix + tail ==\n");
    fails += tail_checks();
    printf("== interleaved streams ==\n");
    fails += streams_checks();
    printf("== key grid ==\n");
    fails += kgrid_checks();
    printf("== signed zeros ==\n");
    fails += signed_zero_checks();
    printf("== dense integers ==\n");
    fails += dense_checks();
    printf("== sorted view (f64 and int32) ==\n");
    fails += view_checks();
    printf("\n");
    benchmark();
    printf("\n%s\n", fails ? "RESULT: CORRECTNESS FAILURES — do not ship" : "RESULT: all correctness checks passed");
    return fails ? 1 : 0;
}
