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
static int cmp_bits(const void *x, const void *y);     /* defined with the IEEE sections below */
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
                case 2: orig[i] = (double)i * 4.0; break;                         /* span 4n, SORTED: the prescan takes it (the rows by name below reach the route) */
                case 3: orig[i] = (double)i * 8.0; break;                         /* span 8n, SORTED: never reached the gate it was written to test */
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
    /* THE ROUTE BY NAME (2026-09-21). The rows above are mostly SORTED (i * 4, i * 8, i - n/2), so the prescan returns before the route
     * is asked and the 'span 8n must decline' row never tested a gate. These are shuffled, and each says what must have happened
     * through logos_twist_last(6). The budget is DENSE_SPAN_MULT = 16: taken at 6n, 12n and 15n, declined by the gate at 24n and 40n,
     * given back by the bitmap after the gate let it in (one repeat planted mid-array, one value far outside the window, one -0.0),
     * and never offered to a parallel sort. */
    { const size_t N2[] = { 70001, 300007 }; const int T2[] = { 1, 4 };
      struct { size_t s; int plant; int want; const char *what; } R2[] = {
        { 6, 0, 1, "span 6n, taken" }, { 12, 0, 1, "span 12n, taken" }, { 15, 0, 1, "span 15n, taken" },
        { 24, 0, 0, "span 24n, the gate declines" }, { 40, 0, 0, "span 40n, the gate declines" },
        { 12, 1, 0, "span 12n + one repeat, given back" }, { 12, 2, 0, "span 12n + one far value, given back" },
        { 12, 3, 0, "span 12n + one -0.0, given back" }, { 12, 4, 1, "span 12n, negative base, taken" } };
      for (size_t ri = 0; ri < sizeof R2 / sizeof *R2; ri++) for (size_t ni = 0; ni < 2; ni++) {
        size_t n = N2[ni], s = R2[ri].s;
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !ref || !a || !bi || !bo) { free(orig); free(ref); free(a); free(bi); free(bo); continue; }
        seed(4321 + ri * 13 + ni);
        for (size_t i = 0; i < n; i++) orig[i] = (double)(i * s + nextr() % s) - (R2[ri].plant == 4 ? (double)(n * s / 2) : 0.0);
        for (size_t i = n - 1; i > 0; i--) { size_t j = nextr() % (i + 1); double x = orig[i]; orig[i] = orig[j]; orig[j] = x; }
        if (R2[ri].plant == 1) orig[n / 2] = orig[n / 3];
        if (R2[ri].plant == 2) orig[n / 2] = (double)(n * s) * 64.0;
        if (R2[ri].plant == 3) { for (size_t i = 0; i < n; i++) if (orig[i] == 0.0) orig[i] = -0.0; orig[n / 2] = -0.0; }
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < 2; ti++) {
            memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(6);
            logos_sort_f64(a, n, T2[ti]);
            size_t got = logos_twist_last(6);
            int ok = 1; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits); if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  dense, %s, n=%zu threads=%d: wrong output\n", R2[ri].what, n, T2[ti]); }
            int want = (T2[ti] > 1 && n >= 65536 * 2) ? 0 : R2[ri].want;          /* a parallel sort is never offered the route */
            if (T2[ti] > 1 && n < 200000) want = -1;                               /* below one thread per 100k the sort may run sequentially */
            if (!logos_twist_last(-3) && want > 0) want = 0;                       /* the route is compiled out of this build */
            if (want >= 0) { checks++; if ((size_t)want != got) { fails++; printf("  FAIL  dense, %s, n=%zu threads=%d: placed by the bitmap %zu time(s), expected %d\n", R2[ri].what, n, T2[ti], got, want); } }
        }
        free(orig); free(ref); free(a); free(bi); free(bo);
      } }
    printf("  dense int: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the sorted-prefix-plus-tail route: correct at the gate's edges, and unchanged when it declines.
 * Every ordinary case is checked against the qsort oracle. The NaN case cannot use qsort as an oracle, since
 * its comparator has no total order over NaN, so it checks the property that matters for this route instead:
 * with a NaN present the route must decline, so the output must be bit-identical to a run with the route
 * switched off. One thread keeps that comparison deterministic. ---- */
/* ---- runs of equal values (2026-10-20, make rleprobe): the route collapses each run of bit-equal values to (key, length), sorts
 * the pairs, writes the runs back. Every row names what must happen (logos_twist_last(28) counts the sorts it finished) and checks
 * the output ascending under < and the same BIT patterns as the input: runs of -0.0 and +0.0 (equal, but distinct runs), NaN runs
 * with different payloads (order unspecified, bits kept), infinities, runs already in order but one pair swapped, a run longer than
 * the gate's windows. It must decline on runs of two, below RLE_MIN_N and in a parallel sort. ---- */
static int rle_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (getenv("LOGOS_NO_RLE")) { printf("  runs of equal values: LOGOS_NO_RLE set, skipped\n"); return 0; }
    struct { const char *what; size_t n; int L, kind, P, expect; } rows[] = {
        { "runs of 4", 300001, 4, 0, 1, 1 }, { "runs of 8", 1000003, 8, 0, 1, 1 }, { "runs of 16", 70000, 16, 0, 1, 1 }, { "runs of 64", 300001, 64, 0, 1, 1 },
        { "geometric runs, mean 10", 300001, 10, 1, 1, 1 }, { "signed zeros in runs", 300001, 8, 2, 1, 1 }, { "NaN runs, payloads", 300001, 8, 3, 1, 1 },
        { "infinities in runs", 300001, 8, 4, 1, 1 }, { "runs in order, four pairs swapped: an order route takes it, not this one", 300001, 8, 5, 1, 0 }, { "one run of 200,000", 300001, 8, 6, 1, 1 },
        { "runs of 2: must decline", 300001, 2, 0, 1, 0 }, { "below RLE_MIN_N: must decline", 65535, 16, 0, 1, 0 }, { "at RLE_MIN_N", 65536, 16, 0, 1, 1 },
        { "4 threads: must decline", 1000003, 16, 0, 4, 0 } };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n; int L = rows[r].L, kind = rows[r].kind;
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(28000 + (uint64_t)r * 13);
        size_t i = 0, run = 0;
        while (i < n) {
            size_t len = kind == 1 ? 1 + (size_t)(-log(frand() + 1e-12) * (double)(L - 1)) : (size_t)L;
            double v = floor(frand() * 1e6) - 5e5;
            if (kind == 2) v = (run % 3 == 0) ? -0.0 : (run % 3 == 1) ? 0.0 : v;
            if (kind == 3 && run % 7 == 0) { uint64_t b = 0x7FF8000000000000ULL | (uint64_t)(run & 0xFFFF) | ((run & 1) ? 0x8000000000000000ULL : 0); memcpy(&v, &b, 8); }
            if (kind == 4 && run % 5 == 0) v = (run & 1) ? INFINITY : -INFINITY;
            if (kind == 5) v = (double)run * 2.0 + frand();               /* ascending across runs, and not a lattice */
            if (kind == 6 && run == 3) len = 200000;
            for (size_t q = 0; q < len && i < n; q++) orig[i++] = v;
            run++;
        }
        if (kind == 5) for (int q = 1; q < 8; q += 2) {                   /* four pairs of runs swapped, at n/8, 3n/8, 5n/8 and 7n/8: one swap at n/4 is an
                                                                           unsorted head before a sorted suffix, which the head route (2026-10-20) takes */
            size_t x = n * (size_t)q / 8; x -= x % (size_t)L;
            for (size_t z = 0; z < (size_t)L && x + L + z < n; z++) { double t = orig[x + z]; orig[x + z] = orig[x + L + z]; orig[x + L + z] = t; } }
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(28);
        logos_sort_f64(a, n, rows[r].P);
        int fired = logos_twist_last(28) != 0;
        int ok = 1;
        if (kind != 3) { for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; } }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  runs of equal values, %s (n=%zu): not a sorted permutation of the input's bits\n", rows[r].what, n); }
        checks++; asserted++; if (fired != rows[r].expect) { fails++; printf("  FAIL  runs of equal values, %s (n=%zu): the route %s\n", rows[r].what, n, fired ? "ran" : "did not run"); }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  runs of equal values: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- cuts (2026-10-20, orderprobe coarse64k): windows in order, unordered inside. The route finds cuts from the sample, block
 * extremes and, for a window edge inside a block, that block's running extremes; every segment sorted where it lies. Each row
 * names what must happen (logos_twist_last(29)) and checks the output ascending under < (NaN rows excepted) and the same BIT
 * patterns as the input. Windows aligned to the 4096 blocks and not, signed zeros at the cuts, infinities in the end windows, an
 * unseen NaN; random data, too few windows, one far stray and a parallel sort must decline. ---- */
static int cuts_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (getenv("LOGOS_NO_CUTS")) { printf("  cuts: LOGOS_NO_CUTS set, skipped\n"); return 0; }
    struct { const char *what; size_t n, W; int kind, P, expect; } rows[] = {
        { "windows of 65536, aligned", 2097152, 65536, 0, 1, 1 }, { "windows of 50000, edges inside blocks", 2000003, 50000, 0, 1, 1 },
        { "windows of 100000", 1048576, 100000, 0, 1, 1 }, { "signed zeros at every cut", 2000003, 50000, 1, 1, 1 },
        { "infinities in the end windows", 2000003, 50000, 2, 1, 1 }, { "an unseen NaN", 2000003, 50000, 3, 1, -1 },
        { "random: must decline", 2000003, 0, 4, 1, 0 }, { "five windows: must decline", 2000003, 400001, 0, 1, 0 },
        { "one far stray: must decline", 2000003, 50000, 5, 1, 0 }, { "4 threads: must decline", 2000003, 50000, 0, 4, 0 } };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n, W = rows[r].W; int kind = rows[r].kind;
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(29000 + (uint64_t)r * 17);
        for (size_t i = 0; i < n; i++) {
            if (kind == 4) { orig[i] = frand() * 1e6; continue; }
            size_t w = i / W; double base = (double)w * (double)W;
            orig[i] = base + frand() * (double)W * 0.98 + 0.01 * (double)W;
        }
        if (kind == 1) {                                                   /* windows around zero, zeros of both signs at that cut */
            for (size_t i = 0; i < n; i++) { size_t w = i / W; double base = ((double)w - (double)(n / W) / 2.0) * (double)W; orig[i] = base + 0.5 + frand() * ((double)W - 1.0); }
            size_t z = (n / W / 2) * W;                                    /* the window whose base is 0: its edge is a cut at 0 */
            orig[z] = 0.0; orig[z + 1] = -0.0; if (z >= 1) orig[z - 1] = -0.0;
        }
        if (kind == 2) { orig[3] = -INFINITY; orig[n - 4] = INFINITY; }
        if (kind == 3) orig[n / 2 + 7] = NAN;
        if (kind == 5) orig[11] = 1e12;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(29);
        logos_sort_f64(a, n, rows[r].P);
        int fired = logos_twist_last(29) != 0;
        int ok = 1;
        if (kind != 3) { for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; } }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  cuts, %s (n=%zu): not a sorted permutation of the input's bits\n", rows[r].what, n); }
        if (rows[r].expect >= 0) { checks++; asserted++; if (fired != rows[r].expect) { fails++; printf("  FAIL  cuts, %s (n=%zu): the route %s\n", rows[r].what, n, fired ? "ran" : "did not run"); } }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  cuts: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- the other-fields round (2026-10-21, experiments/OTHER_FIELDS.md, make fieldprobe): five decisions it moved. Each row is
 * checked as a sorted permutation of the input's BITS and names the branch that must have run, then runs again with its switch
 * set to show the branch follows the switch. (1) The low-discrepancy probe's coverage test, logos_twist_last(30): legs of 512 take
 * the kernel; a Weyl sequence and legs of 8192 keep the probe. (2) The founder windows (31). (3) The k-way merge in vectors (32).
 * (4) The stray cap of 1/16 at one thread (10 = strays lifted). (5) The short-reach probe's threshold at 0.20, on two sources
 * interleaved at random (logos_reach_last(), three seeds). Rows (2) and (3) need AVX2, and say so when skipped. ---- */
static void of_gen(double *a, size_t n, int kind) {
    switch (kind) {
    case 0: case 2: { size_t p = kind == 0 ? 1024 : 8192, h = p / 2; double sh = 1.0 / (double)(n / p + 2);   /* a triangle, legs of 512; a ramp, legs of 8192 */
        for (size_t i = 0; i < n; i++) { size_t ph = i % p;
            a[i] = kind == 0 ? (double)(ph < h ? ph : p - ph) + (double)(i / p) * sh + (ph < h ? 0.0 : 0.5 * sh) : (double)ph + (double)(i / p) * sh; } } break;
    case 1: for (size_t i = 0; i < n; i++) { double v = (double)i * 0.6180339887498949; a[i] = (v - floor(v)) * 1e6; } break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = i < 64 ? (double)i : frand() * 1e6; break;
    case 4: { size_t L = n / 4 + 1; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) * 4.0 + (double)(i / L) + 0.25; } break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;                  /* + 0.5: a permutation of 0..n-1 would go to the dense bitmap */
        for (size_t q = 0; q < n / 100; q++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 6: { size_t h = n / 2, i0 = 0, i1 = 0;                                     /* each source ascending over the whole range, a coin per element */
        for (size_t i = 0; i < n; i++) { int s = i0 >= h ? 1 : i1 >= n - h ? 0 : (int)(nextr() & 1);
            a[i] = s ? (double)i1++ * 2.0 + 1.25 : (double)i0++ * 2.0 + 0.25; } } break;
    }
}
static size_t of_witness(int w) {
    if (w >= 0) return logos_twist_last(w);
    int r = logos_reach_last(); return r == LOGOS_REACH_SORTED || r == LOGOS_REACH_SORTED_STRAYS;
}
static int other_fields_checks(void) {
    int checks = 0, fails = 0, asserted = 0, avx = logos_twist_last(-4) != 0;
    struct { const char *what; int kind; size_t n; int w; const char *sw, *val; int on, off, needs_avx, seeds; } rows[] = {
        { "triangle, legs of 512: the kernel, not the low-discrepancy path", 0, 1048576, 30, "LOGOS_LOWDISC_LOCAL", "1", 0, 1, 0, 1 },
        { "Weyl sequence: the low-discrepancy path, with the coverage test or without", 1, 1048576, 30, "LOGOS_LOWDISC_LOCAL", "1", 1, 1, 0, 1 },
        { "ramp, legs of 8192: the low-discrepancy path either way (coarse wins on long clean legs)", 2, 1048576, 30, "LOGOS_LOWDISC_LOCAL", "1", 1, 1, 0, 1 },
        { "64 sorted integers in front of uniform data: the founder windows settle the profile", 3, 1048576, 31, "LOGOS_SF_FRONT_ONLY", "1", 1, 0, 1, 1 },
        { "four sorted strands laid end to end: the k-way merge in vectors", 4, 1048576, 32, "LOGOS_RUNMERGE_SCALAR", "1", 1, 0, 1, 1 },
        { "1% random swaps, one thread: strays lifted under the 1/16 cap", 5, 1048576, 10, "LOGOS_STRAY_DIV", "64", 1, 0, 0, 1 },
        { "two sources interleaved at random (25% descents): the short-reach route", 6, 2097152, -1, "LOGOS_REACH_DENSE", "0.25", 1, -1, 0, 3 } };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        if (rows[r].needs_avx && !avx) { printf("  other fields: %s -- no AVX2 here, skipped\n", rows[r].what); continue; }
        for (int s = 0; s < rows[r].seeds; s++) {
            size_t n = rows[r].n;
            double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
            seed(30000 + (uint64_t)r * 17 + (uint64_t)s * 101);
            of_gen(orig, n, rows[r].kind);
            memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            for (int set = 0; set < 2; set++) {
                if (set) setenv(rows[r].sw, rows[r].val, 1); else unsetenv(rows[r].sw);
                memcpy(a, orig, n * sizeof(double)); (void)of_witness(rows[r].w >= 0 ? rows[r].w : 30);
                logos_sort_f64(a, n, 1);
                int got = of_witness(rows[r].w) != 0, want = set ? rows[r].off : rows[r].on;
                unsetenv(rows[r].sw);
                int ok = 1;
                for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  other fields, %s%s (seed %d): not a sorted permutation of the input's bits\n", rows[r].what, set ? ", switched" : "", s); }
                if (want >= 0) { checks++; asserted++;
                    if (got != want) { fails++; printf("  FAIL  other fields, %s%s (seed %d): the branch %s\n", rows[r].what, set ? " with the switch set" : "", s, got ? "ran" : "did not run"); } }
            }
            free(orig); free(a); free(bi); free(bo);
        }
    }
    printf("  other fields: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- the dominant-value route (2026-10-22, the memory round): one value holding a large share is filled in around the rest, which
 * is sorted on its own. Every row checks ascending under < (NaN rows excepted) and the same BIT patterns as the input, and names
 * whether the route must have run (logos_twist_last(33)): the gates at P > 1 (DOM_SHARE_PAR) and at one thread (DOM_SHARE_SEQ), a
 * dominant +0.0 with -0.0 among the rest and the other way round, a dominant +inf, a NaN the sample cannot see, a failed buffer
 * (LOGOS_DOM_FAILALLOC=1: the route declines with nothing moved), the switch. ---- */
static int domroute_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (getenv("LOGOS_NO_DOMINANT")) { printf("  dominant-value route: LOGOS_NO_DOMINANT set, skipped\n"); return 0; }
    struct { const char *what; size_t n; int share100, kind, P, expect; const char *env; } rows[] = {
        { "share 0.5, 4 threads", 1000003, 50, 0, 4, 1, NULL }, { "share 0.25, 8 threads", 2000003, 25, 0, 8, 1, NULL },
        { "share 0.9, 4 threads", 1000003, 90, 0, 4, 1, NULL }, { "share 0.9, one thread", 1000003, 90, 0, 1, 1, NULL },
        { "share 0.5, one thread: the kernel's, not this route", 1000003, 50, 0, 1, 0, NULL },
        { "share 0.1, 4 threads: below the gate", 1000003, 10, 0, 4, 0, NULL },
        { "+0.0 dominant, -0.0 among the rest", 1000003, 60, 1, 4, 1, NULL }, { "-0.0 dominant, +0.0 among the rest", 1000003, 60, 2, 4, 1, NULL },
        { "+inf dominant, -inf and finite values", 1000003, 60, 3, 4, 1, NULL }, { "an unseen NaN", 1000003, 60, 4, 4, -1, NULL },
        { "the buffer fails: declines, nothing moved", 1000003, 60, 0, 4, 0, "LOGOS_DOM_FAILALLOC" },
        { "switched off", 1000003, 60, 0, 4, 0, "LOGOS_NO_DOMINANT" } };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n; int kind = rows[r].kind;
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(33000 + (uint64_t)r * 7);
        double v = kind == 1 ? 0.0 : kind == 2 ? -0.0 : kind == 3 ? INFINITY : 12345.0;
        for (size_t i = 0; i < n; i++) {
            if ((int)(nextr() % 100) < rows[r].share100) { orig[i] = v; continue; }
            double x = frand() * 2e9 - 1e9;
            if (kind == 1 && (nextr() & 7) == 0) x = -0.0;
            if (kind == 2 && (nextr() & 7) == 0) x = 0.0;
            if (kind == 3 && (nextr() & 15) == 0) x = -INFINITY;
            orig[i] = x;
        }
        if (kind == 4) orig[n / 2 + 1] = NAN;                           /* one NaN: the sample of a million elements will not see it */
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(33);
        logos_sort_f64(a, n, rows[r].P);
        int fired = logos_twist_last(33) != 0;
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1;
        if (kind != 4) { for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; } }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  dominant-value route, %s (n=%zu): not a sorted permutation of the input's bits\n", rows[r].what, n); }
        if (rows[r].expect >= 0) { checks++; asserted++; if (fired != rows[r].expect) { fails++; printf("  FAIL  dominant-value route, %s (n=%zu): the route %s\n", rows[r].what, n, fired ? "ran" : "did not run"); } }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  dominant-value route: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- half the scratch (2026-10-24, the second memory round): the knot route at one thread and the tail and head routes' vector
 * merge place with half an n-element scratch (half_place), and the float32 radix keeps its keys in the input. Every row checks
 * ascending order and the input's BIT multiset, and names whether a half placement must have run (logos_twist_last(34)): shuffled
 * blocks, half of them descending, with signed zeros in one; a staircase (two-strand crossings, one split at the midpoint); sorted
 * data with 1% random swaps (strays, merged back from the top, and crossings of three or more); two sorted halves (the tail route:
 * one merge split at its merge path); a random head before a sorted suffix, three ways (the head route; the head above every suffix
 * value and below it put the whole lower or upper half in one piece); sizes on both sides of HALF_MIN_N; LOGOS_PLACE_FULL=1. Then
 * the float32 radix both ways (LOGOS_F32_TWOBUF), with declines at the second, the middle and the last element: a decline widens
 * the keys back, and a wrong widening shows as a changed multiset. ---- */
static int half_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    enum { H_BLOCKS, H_TWIST, H_ZEROS, H_STAIRS, H_NEAR, H_HALVES, H_HEAD, H_HEADHI, H_HEADLO, H_STRANDS, H_STRANDS5, H_STRANDS_TW, H_TIES4, H_SINE };
    struct { const char *what; int kind; size_t n; int expect; const char *env; } rows[] = {
        { "64 shuffled blocks", H_BLOCKS, 1000003, 1, NULL }, { "64 blocks, every other descending", H_TWIST, 1000003, 1, NULL },
        { "blocks, signed zeros in one", H_ZEROS, 300001, 1, NULL }, { "staircase of 20", H_STAIRS, 1000003, 1, NULL },
        { "1% random swaps: strays", H_NEAR, 1000003, 1, NULL }, { "two sorted halves: the tail route", H_HALVES, 1000003, 1, NULL },
        { "a random head of 30%: the head route", H_HEAD, 1000003, 1, NULL }, { "head above the suffix: one piece below", H_HEADHI, 300001, 1, NULL },
        { "head below the suffix: one piece above", H_HEADLO, 300001, 1, NULL }, { "four interleaved strands: one crossing split by value", H_STRANDS, 1000003, 1, NULL },
        { "five interleaved strands", H_STRANDS5, 1000003, 1, NULL }, { "four strands, two of them descending", H_STRANDS_TW, 1000003, 1, NULL },
        { "four identical strands: ties at the cut", H_TIES4, 1000003, 1, NULL }, { "a sloped sine: the wave route", H_SINE, 2000003, 1, NULL },
        { "blocks at HALF_MIN_N", H_BLOCKS, 65536, 1, NULL }, { "blocks one below it", H_BLOCKS, 65535, 0, NULL },
        { "two halves one below it", H_HALVES, 65535, 0, NULL }, { "blocks, LOGOS_PLACE_FULL=1", H_BLOCKS, 1000003, 0, "LOGOS_PLACE_FULL" },
        { "1% swaps, LOGOS_PLACE_FULL=1", H_NEAR, 1000003, 0, "LOGOS_PLACE_FULL" },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n; int kind = rows[r].kind;
        double *a = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!a || !bi || !bo) { free(a); free(bi); free(bo); printf("  half the scratch: skipped a row (no memory)\n"); continue; }
        seed(24100 + r);
        if (kind == H_BLOCKS || kind == H_TWIST || kind == H_ZEROS) {
            size_t nb = 64, blk = n / nb, perm[64];
            for (size_t k = 0; k < nb; k++) perm[k] = k;
            for (size_t k = nb - 1; k > 0; k--) { size_t s = nextr() % (k + 1), x = perm[k]; perm[k] = perm[s]; perm[s] = x; }
            for (size_t i = 0; i < n; i++) {
                size_t k = i / blk < nb ? i / blk : nb - 1, o = i - k * blk, L = k == nb - 1 ? n - k * blk : blk;
                a[i] = (double)(perm[k] * blk) + (kind == H_TWIST && (perm[k] & 1) ? (double)(L - 1 - o) : (double)o) + 0.5;
                if (kind == H_ZEROS) a[i] = (a[i] - (double)(n / 2) - 0.5) * 0.5;      /* half-integers (no dense-integer route), 0 among them */
            }
            if (kind == H_ZEROS) for (size_t i = 0; i + 1 < n; i++) if (a[i] == 0.0 && a[i + 1] == 0.5) {   /* 0, 0.5 become -0.0, +0.0: a tie keeps the run */
                a[i] = -0.0; a[i + 1] = 0.0; break;
            }
        } else if (kind == H_STAIRS) { size_t L = n / 20 + 1; for (size_t i = 0; i < n; i++) { size_t b = i / L; a[i] = (double)b * (double)L * 0.9 + (double)(i - b * L) + 0.25; } }
        else if (kind == H_NEAR) { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
                                   for (size_t k = 0; k < n / 100; k++) { size_t i = nextr() % n, j = nextr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; } }
        else if (kind == H_HALVES) for (size_t i = 0; i < n; i++) a[i] = i < n / 2 ? (double)i * 2.0 + 0.25 : (double)(i - n / 2) * 2.0 + 1.25;
        else if (kind == H_HEAD) for (size_t i = 0; i < n; i++) a[i] = i < n * 3 / 10 ? frand() * (double)n : (double)i + 0.5;
        else if (kind == H_HEADHI) for (size_t i = 0; i < n; i++) a[i] = i < n * 4 / 10 ? 2.0 * (double)n + frand() * (double)n : (double)i + 0.5;
        else if (kind == H_HEADLO) for (size_t i = 0; i < n; i++) a[i] = i < n * 4 / 10 ? -frand() * (double)n - 1.0 : (double)i + 0.5;
        else if (kind == H_STRANDS) { size_t L = n / 4 + 1; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) * 4.0 + (double)(i / L) + 0.25; }
        else if (kind == H_STRANDS5) { size_t L = n / 5 + 1; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) * 5.0 + (double)(i / L) + 0.25; }
        else if (kind == H_STRANDS_TW) { size_t L = n / 4 + 1; for (size_t i = 0; i < n; i++) { size_t r = i / L, o = i % L, Lr = r == 3 ? n - 3 * L : L;
                                         a[i] = (double)((r & 1) ? Lr - 1 - o : o) * 4.0 + (double)r + 0.25; } }
        else if (kind == H_TIES4) { size_t L = n / 4 + 1; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) + 0.5; }
        else { double T = (double)n / 32.0, A = 0.6 * T / 2.0; for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (frand() * 2.0 - 1.0) * 50.0; }
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        (void)logos_twist_last(34);
        logos_sort_f64(a, n, 1);
        int fired = logos_twist_last(34) != 0;
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1; for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  half the scratch, %s (n=%zu): not a sorted permutation of the input's bits\n", rows[r].what, n); }
        if (rows[r].expect >= 0) { checks++; asserted++; if (fired != rows[r].expect) { fails++; printf("  FAIL  half the scratch, %s (n=%zu): the half placement %s\n", rows[r].what, n, fired ? "ran" : "did not run"); } }
        free(a); free(bi); free(bo);
    }
    /* the partition's regions: borrowed from the pool's block (logos_twist_last(35)), or their own with LOGOS_HF_OWN=1 */
    for (int own = 0; own < 2; own++) {
        size_t n2 = 2000003; double *x = malloc(n2 * sizeof(double)); uint64_t *xi = malloc(n2 * sizeof(uint64_t)), *xo = malloc(n2 * sizeof(uint64_t));
        if (!x || !xi || !xo) { free(x); free(xi); free(xo); continue; }
        seed(24300 + (uint64_t)own); for (size_t i = 0; i < n2; i++) x[i] = frand() * 2e9 - 1e9;
        memcpy(xi, x, n2 * sizeof(double)); qsort(xi, n2, sizeof(uint64_t), cmp_bits);
        if (own) setenv("LOGOS_HF_OWN", "1", 1);
        (void)logos_twist_last(35); logos_sort_f64(x, n2, 8); size_t pooled = logos_twist_last(35);
        if (own) unsetenv("LOGOS_HF_OWN");
        int ok = 1; for (size_t q = 1; q < n2; q++) if (x[q] < x[q - 1]) { ok = 0; break; }
        memcpy(xo, x, n2 * sizeof(double)); qsort(xo, n2, sizeof(uint64_t), cmp_bits); if (memcmp(xi, xo, n2 * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  half the scratch, the partition%s: wrong output\n", own ? " LOGOS_HF_OWN=1" : ""); }
        checks++; asserted++; if ((pooled > 0) != !own) { fails++; printf("  FAIL  half the scratch, the partition%s: regions in the pool's block %zu times\n", own ? " LOGOS_HF_OWN=1" : "", pooled); }
        free(x); free(xi); free(xo);
    }
    /* LOGOS_LOWMEM=1: the in-place partition (logos_twist_last(24)) and the hard trim, together; the output as always */
    { size_t n2 = 2000003; double *x = malloc(n2 * sizeof(double)); uint64_t *xi = malloc(n2 * sizeof(uint64_t)), *xo = malloc(n2 * sizeof(uint64_t));
      if (x && xi && xo) {
        seed(24400); for (size_t i = 0; i < n2; i++) x[i] = frand() * 2e9 - 1e9;
        memcpy(xi, x, n2 * sizeof(double)); qsort(xi, n2, sizeof(uint64_t), cmp_bits);
        setenv("LOGOS_LOWMEM", "1", 1); (void)logos_twist_last(24); logos_sort_f64(x, n2, 8); size_t ip = logos_twist_last(24); unsetenv("LOGOS_LOWMEM");
        int ok = 1; for (size_t q = 1; q < n2; q++) if (x[q] < x[q - 1]) { ok = 0; break; }
        memcpy(xo, x, n2 * sizeof(double)); qsort(xo, n2, sizeof(uint64_t), cmp_bits); if (memcmp(xi, xo, n2 * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  half the scratch, LOGOS_LOWMEM=1: wrong output\n"); }
        checks++; asserted++; if (!ip) { fails++; printf("  FAIL  half the scratch, LOGOS_LOWMEM=1: the in-place partition did not run\n"); }
      }
      free(x); free(xi); free(xo); }
    /* the float32 radix, keys in the input (shipped) and in aux (LOGOS_F32_TWOBUF=1): the same multiset, and the path fires exactly
       where every value is a float */
    const size_t n = 300007;
    double *a = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
    if (a && bi && bo) for (int tb = 0; tb < 2; tb++) for (int mode = 0; mode < 6; mode++) {
        seed(24200 + (uint64_t)mode);
        for (size_t i = 0; i < n; i++) a[i] = (double)(float)(frand() * 2e6 - 1e6);
        a[11] = -0.0; a[12] = 0.0;
        int expect = 1, nan = 0;
        if (mode == 1) for (size_t i = 0; i < n; i++) { uint32_t u = 0x3F800000u + (uint32_t)(nextr() % 4000000); float f; memcpy(&f, &u, 4); a[i] = f; }   /* two passes */
        if (mode == 2) { a[1] = 0.1; expect = 0; }                                    /* declines at the second element */
        if (mode == 3) { a[n / 2] = 0.1; expect = 0; }
        if (mode == 4) { a[n - 1] = 0.1; expect = 0; }                                /* at the last: every key widened back */
        if (mode == 5) { a[n - 1] = NAN; expect = 0; nan = 1; }
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (tb) setenv("LOGOS_F32_TWOBUF", "1", 1);
        (void)logos_twist_last(22);
        logos_sort_f64(a, n, 1);
        size_t fired = logos_twist_last(22);
        if (tb) unsetenv("LOGOS_F32_TWOBUF");
        int ok = 1; if (!nan) for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  half the scratch, float32 mode %d%s: wrong output\n", mode, tb ? " LOGOS_F32_TWOBUF=1" : ""); }
        if (logos_twist_last(-8)) { checks++; asserted++;
            if ((fired > 0) != expect) { fails++; printf("  FAIL  half the scratch, float32 mode %d%s: the path fired %zu times, expected %s\n", mode, tb ? " LOGOS_F32_TWOBUF=1" : "", fired, expect ? "once" : "never"); } }
    }
    free(a); free(bi); free(bo);
    printf("  half the scratch: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- the lattice entry (2026-10-31, the boundary round): a lattice the sample cannot see repeat goes to the lattice route when it
 * leaves 8 (P + 1) elements per slot (logos_twist_last(36)), and an element off the lattice, outside its window or a second double
 * on one lattice point is spilled, sorted on its own and merged back, at any thread count (logos_twist_last(37), elements).
 * Every row checks ascending order (unless a NaN is present) and the input's BIT multiset, and names both witnesses. The HIDDEN rows
 * are white-box: they plant off-lattice values everywhere except the sampler's Weyl positions (the first 2048, a superset of the 384
 * a one-thread sort reads), at 2.5% (spilled: under the n/32 budget) and 4% (past it: the route declines). If the sampler changes,
 * those rows fail by name. ---- */
static size_t harness_weyl(size_t i, size_t n) { double f = (double)i * 0.6180339887498949; f -= (double)(uint64_t)f; size_t q = (size_t)(f * (double)n); return q < n ? q : n - 1; }
static int lat1_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    enum { L_LAT, L_LATEOFF, L_SENT, L_ZEROS, L_PRICE, L_HIDDEN, L_NANLATE, L_UNI, L_ONEOFF };
    struct { const char *what; int kind; size_t k, n; int P; const char *env; int e36, e37; } rows[] = {
        { "30,000 lattice values", L_LAT, 30000, 1000003, 1, NULL, 1, 0 },
        { "50,000 lattice values (the step's divisors)", L_LAT, 50000, 1000003, 1, NULL, 1, 0 },
        { "131,072 values: 7.6 a slot, declined", L_LAT, 131072, 1000003, 1, NULL, 0, 0 },
        { "the last 0.05% off the lattice: spilled", L_LATEOFF, 30000, 1000003, 1, NULL, 1, 1 },
        { "integers with 16 far sentinels at the end", L_SENT, 20000, 1000003, 1, NULL, 1, 1 },
        { "integers, one -0.0 among the zeros", L_ZEROS, 20000, 1000003, 1, NULL, 1, 1 },
        { "cents", L_PRICE, 100000, 2000003, 1, NULL, 1, -1 },
        { "2.5% off the lattice, hidden from the sample: spilled", L_HIDDEN, 25, 1000003, 1, NULL, 1, 1 },
        { "the same, LOGOS_NO_LATVEC=1 (the scalar map spills)", L_HIDDEN, 25, 1000003, 1, "LOGOS_NO_LATVEC", 1, 1 },
        { "4% off, hidden: past the spill budget, declined", L_HIDDEN, 40, 1000003, 1, NULL, 0, 0 },
        { "a NaN at the end: declined", L_NANLATE, 30000, 1000003, 1, NULL, 0, 0 },
        { "LOGOS_NO_LAT1=1", L_LAT, 30000, 1000003, 1, "LOGOS_NO_LAT1", 0, 0 },
        { "LOGOS_LAT_NO_SPILL=1, the last 0.05% off: declined", L_LATEOFF, 30000, 1000003, 1, "LOGOS_LAT_NO_SPILL", 0, 0 },
        { "LOGOS_LAT_DIV1=1, 50,000 values", L_LAT, 50000, 1000003, 1, "LOGOS_LAT_DIV1", -1, 0 },
        { "four threads, integers with far sentinels: the counting block's lattice, spilled", L_SENT, 20000, 1000003, 4, NULL, 0, 1 },
        { "four threads, 30,000 values: the counting block's", L_LAT, 30000, 1000003, 4, NULL, 0, 0 },
        { "two threads, 60,000 values at 2M: 33 a slot", L_LAT, 60000, 2000003, 2, NULL, 1, 0 },
        { "two threads, the last 0.05% off: spilled", L_LATEOFF, 60000, 2000003, 2, NULL, 1, 1 },
        { "two threads, 65,536 values at 1M: 15 a slot, declined", L_LAT, 65536, 1000003, 2, NULL, 0, 0 },
        { "eight threads, integers with far sentinels: spilled", L_SENT, 20000, 2000003, 8, NULL, 0, 1 },
        { "eight threads, the same, LOGOS_LAT_NO_SPILL=1: declined", L_SENT, 20000, 2000003, 8, "LOGOS_LAT_NO_SPILL", 0, 0 },
        { "uniform doubles", L_UNI, 0, 1000003, 1, NULL, 0, 0 },
        { "one SAMPLED value off the lattice: a step of 0.05 would be 5x too fine, declined", L_ONEOFF, 30000, 1000003, 1, NULL, 0, 0 },
        { "the same at four threads (the counting block's lattice)", L_ONEOFF, 30000, 1000003, 4, NULL, 0, 0 },
        { "n = 600,000, 30,000 values: 20 a slot", L_LAT, 30000, 600000, 1, NULL, 1, 0 },
        { "n = 300,000, 30,000 values: 10 a slot, declined", L_LAT, 30000, 300000, 1, NULL, 0, 0 },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n, k = rows[r].k; int kind = rows[r].kind, nan = 0;
        double *a = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        unsigned char *hold = kind == L_HIDDEN ? calloc(n, 1) : NULL;
        if (!a || !bi || !bo || (kind == L_HIDDEN && !hold)) { free(a); free(bi); free(bo); free(hold); printf("  lattice entry: skipped a row (no memory)\n"); continue; }
        seed(24500 + r);
        if (kind == L_LAT || kind == L_LATEOFF || kind == L_HIDDEN || kind == L_NANLATE || kind == L_ONEOFF) for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % (kind == L_HIDDEN ? 30000 : k)) * 0.25 + 3.0;
        if (kind == L_LATEOFF) for (size_t i = n - n / 2000; i < n; i++) a[i] += 0.1;
        if (kind == L_HIDDEN) { for (size_t i = 0; i < 2048; i++) hold[harness_weyl(i, n)] = 1;
                                for (size_t i = 0; i < n; i++) if (!hold[i] && nextr() % 1000 < k) a[i] += 0.1; }
        if (kind == L_NANLATE) { a[n - 1] = NAN; nan = 1; }
        if (kind == L_ONEOFF) a[harness_weyl(1, n)] += 0.1;                        /* read by every sampler: the first Weyl point after a[0] */
        if (kind == L_SENT) { for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % k); for (size_t q = 0; q < 16; q++) a[n - 1 - q] = q & 1 ? 1e9 : -1e9; }
        if (kind == L_ZEROS) { for (size_t i = 0; i < n; i++) a[i] = (double)((long long)(nextr() % k) - (long long)(k / 2)); a[5] = -0.0; }
        if (kind == L_PRICE) for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % k) / 100.0;
        if (kind == L_UNI) for (size_t i = 0; i < n; i++) a[i] = frand() * 2e9 - 1e9;
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        (void)logos_twist_last(36); (void)logos_twist_last(37); (void)logos_twist_last(23);
        logos_sort_f64(a, n, rows[r].P);
        size_t fired = logos_twist_last(36), spilled = logos_twist_last(37), mapped = logos_twist_last(23);   /* 23: the vector lattice map ran */
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1; if (!nan) for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  lattice entry, %s (n=%zu): not a sorted permutation of the input's bits\n", rows[r].what, n); }
#if !defined(LOGOS_NO_LATTICE) && !defined(LOGOS_NO_MODCARD) && !defined(LOGOS_NO_SODO)
        if (rows[r].e36 >= 0) { checks++; asserted++; if ((fired > 0) != rows[r].e36) { fails++; printf("  FAIL  lattice entry, %s (n=%zu): the entry %s\n", rows[r].what, n, fired ? "ran" : "did not run"); } }
        if (rows[r].e37 >= 0) { checks++; asserted++; if ((spilled > 0) != rows[r].e37) { fails++; printf("  FAIL  lattice entry, %s (n=%zu): %zu elements spilled\n", rows[r].what, n, spilled); } }
        if ((kind == L_ONEOFF || r == 0) && logos_twist_last(-5)) { checks++; asserted++;       /* a too-fine step declines BEFORE the map */
            if ((mapped > 0) != (r == 0)) { fails++; printf("  FAIL  lattice entry, %s (n=%zu): the lattice map %s\n", rows[r].what, n, mapped ? "ran" : "did not run"); } }
#else
        (void)fired; (void)spilled; (void)mapped;
#endif
        free(a); free(bi); free(bo); free(hold);
    }
    printf("  lattice entry: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- the near-sorted route's strand gate (2026-11-06, FINDINGS 3bw): at P > 1 sorted runs lying IN ORDER and overlapping their
 * neighbours must NOT be taken by the near-sorted route (the pre-gate's windows see no descent, the position-ordered sample sees
 * one per strand boundary: logos_twist_last(38) counts the decline), and sorted data with scattered swaps must still be taken (the
 * windows see the swaps: 38 reads 0). Every row checks ascending order and the input's BIT multiset; the shapes are shuffled or
 * overlapping so that the prescan cannot return them first. ---- */
enum { S_STAIRS, S_LATE, S_SWAPPED, S_NEAR, S_NEARFEW, S_SHORTSWAPS, S_STAIRS_NAN, S_STAIRS_NANSAMP };
/* noinline, in its own function: with the generation inline in nsstrand_checks GCC 13.3 died with an internal compiler error
   (patch_jump_insn, cfgrtl.cc:1295, during the cprop pass), 2026-11-06. Returns 1 when the row holds a NaN. */
static __attribute__((noinline)) int nsstrand_gen(int kind, double *a, size_t n) {
    size_t nb = 64, blk = (n + nb - 1) / nb; int nan = 0;
    if (kind == S_STAIRS || kind == S_LATE || kind == S_STAIRS_NAN || kind == S_STAIRS_NANSAMP) for (size_t i = 0; i < n; i++) a[i] = (double)(i / blk) * (double)blk + (double)(i % blk) * 1.2 + frand() * 0.5;
    else for (size_t i = 0; i < n; i++) a[i] = (double)i + frand() * 0.5;
    if (kind == S_LATE) for (size_t b = 8; b + 4 <= nb && (b + 4) * blk <= n; b += 16) {           /* rotate blocks b..b+3 left by one block */
        size_t lo = b * blk, mid = lo + blk, hi = lo + 4 * blk; double *t = malloc((hi - lo) * sizeof(double));
        if (t) { memcpy(t, a + mid, (hi - mid) * sizeof(double)); memcpy(t + (hi - mid), a + lo, blk * sizeof(double)); memcpy(a + lo, t, (hi - lo) * sizeof(double)); free(t); } }
    if (kind == S_SWAPPED) for (size_t b = 4; b < 4 + 6 * 8 && (b + 2) * blk <= n; b += 8) for (size_t i = 0; i < blk; i++) { double t = a[b * blk + i]; a[b * blk + i] = a[(b + 1) * blk + i]; a[(b + 1) * blk + i] = t; }
    if (kind == S_NEAR || kind == S_NEARFEW) { size_t k = kind == S_NEAR ? n / 100 : n / 1000; for (size_t q = 0; q < k; q++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } }
    if (kind == S_SHORTSWAPS) for (size_t q = 0; q < n / 50; q++) { size_t x = nextr() % n, y = x + 1 + nextr() % 2048; if (y < n) { double t = a[x]; a[x] = a[y]; a[y] = t; } }
    if (kind == S_STAIRS_NAN) { a[n - 1] = NAN; nan = 1; }
    if (kind == S_STAIRS_NANSAMP) { a[harness_weyl(1, n)] = NAN; nan = 1; }               /* the first Weyl point after a[0]: every sampler reads it */
    return nan;
}
static int nsstrand_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    struct { const char *what; int kind; size_t n; int P; const char *env; int e38; } rows[] = {
        { "64 sorted runs in order, overlapping 20%: strands, declined", S_STAIRS, 1000003, 8, NULL, 1 },
        { "the same at two threads", S_STAIRS, 1000003, 2, NULL, 1 },
        { "the same, LOGOS_NO_NSSTRAND=1: the route is not turned away by this gate", S_STAIRS, 1000003, 8, "LOGOS_NO_NSSTRAND", 0 },
        { "the same, LOGOS_NS_GATE_OLD=1: the old pre-gate has no strand gate", S_STAIRS, 1000003, 8, "LOGOS_NS_GATE_OLD", 0 },
        { "the same with a NaN at the END, which no Weyl position reads: the gate fires, the output is a permutation", S_STAIRS_NAN, 1000003, 8, NULL, 1 },
        { "the same with a NaN at a SAMPLED position: no verdict (sample_by_pos declines a NaN), the route declines on its own", S_STAIRS_NANSAMP, 1000003, 8, NULL, 0 },
        { "four batches three places late", S_LATE, 1000003, 8, NULL, 1 },
        /* six pairs, not eight: eight put the sample's lag-144 inversion rate at 0.025, which is the route's own gate below 6M elements
           (it declines before this gate is reached, and the row would not be testing the gate) */
        { "six neighbouring block pairs swapped", S_SWAPPED, 1000003, 8, NULL, 1 },
        { "sorted + 1% random swaps: strays, taken", S_NEAR, 1000003, 8, NULL, 0 },
        { "sorted + 0.1% random swaps: taken", S_NEARFEW, 1000003, 8, NULL, 0 },
        { "sorted + 2% swaps within 2048 places: taken", S_SHORTSWAPS, 1000003, 8, NULL, 0 },
        { "one thread, 64 runs in order (the gate is a parallel decision)", S_STAIRS, 1000003, 1, NULL, 0 },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n;
        double *a = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!a || !bi || !bo) { free(a); free(bi); free(bo); printf("  strand gate: skipped a row (no memory)\n"); continue; }
        seed(26100 + r);
        int nan = nsstrand_gen(rows[r].kind, a, n);
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        (void)logos_twist_last(38);
        logos_sort_f64(a, n, rows[r].P);
        size_t declined = logos_twist_last(38);
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1; if (!nan) for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  strand gate, %s (n=%zu, P=%d): not a sorted permutation of the input's bits\n", rows[r].what, n, rows[r].P); }
#if !defined(LOGOS_NO_NEARSORTED) && !defined(LOGOS_NO_NSSTRAND) && !defined(LOGOS_NS_OLD)
        checks++; asserted++;
        if ((declined > 0) != rows[r].e38) { fails++; printf("  FAIL  strand gate, %s (n=%zu, P=%d): the gate %s\n", rows[r].what, n, rows[r].P, declined ? "declined the route" : "did not fire"); }
#else
        (void)declined;
#endif
        free(a); free(bi); free(bo);
    }
    printf("  strand gate: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
/* ---- the unstructured partition's thread boost (2026-11-09, FINDINGS 3bz): from 300,000 elements an unstructured array that
 * reaches the partition takes one thread per 25,000 elements instead of one per 100,000 (logos_twist_last(39) counts it); ordered
 * and repeating data, one thread, a request the policy already grants, a smaller array and LOGOS_BOOST_PER=1 must not. ---- */
enum { B_UNIFORM, B_HEAVY, B_GAUSS, B_CLUSTER, B_BLOCKS, B_SAW, B_FEW, B_NEAR, B_UNIFORM_NAN };
static __attribute__((noinline)) int boost_gen(int kind, double *a, size_t n) {
    switch (kind) {
    case B_UNIFORM: case B_UNIFORM_NAN: for (size_t i = 0; i < n; i++) a[i] = frand(); break;
    case B_HEAVY: for (size_t i = 0; i < n; i++) a[i] = exp(10.0 * frand()) * (frand() < 0.5 ? -1.0 : 1.0); break;
    case B_GAUSS: for (size_t i = 0; i < n; i++) { double u = frand() + 1e-300, v = frand(); a[i] = sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v); } break;
    case B_CLUSTER: for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % 64) * 100.0 + frand() * 0.01; break;
    case B_BLOCKS: { size_t B = 64, L = n / B; for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(((b * 37) % B) * L + (i - b * L)) + 0.25; } break; }
    case B_SAW: { size_t L = n / 64; for (size_t i = 0; i < n; i++) a[i] = (double)(i % L) + (double)(i / L) * 1e-7; break; }
    case B_FEW: for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % 5); break;
    default: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
             for (size_t q = 0; q < n / 100; q++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    }
    if (kind == B_UNIFORM_NAN) { a[n - 1] = NAN; return 1; }
    return 0;
}
static int boost_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    struct { const char *what; int kind; size_t n; int P; const char *env; int e39; } rows[] = {   /* e39: 1 must fire, 0 must not, -1 either */
        { "uniform, 8 threads asked, the policy gives 4", B_UNIFORM, 400003, 8, NULL, 1 },
        { "uniform at 1M, 14 asked, the policy gives 10", B_UNIFORM, 1000003, 14, NULL, 1 },
        { "heavy tail", B_HEAVY, 400003, 8, NULL, 1 },
        { "gaussian", B_GAUSS, 400003, 8, NULL, 1 },
        { "64 thin clusters", B_CLUSTER, 500003, 8, NULL, 1 },
        { "uniform, LOGOS_BOOST_PER=1: off", B_UNIFORM, 400003, 8, "LOGOS_BOOST_PER", 0 },
        { "uniform under the 300,000 floor", B_UNIFORM, 290003, 8, NULL, 0 },
        { "uniform, 4 threads asked and granted: nothing to add", B_UNIFORM, 400003, 4, NULL, 0 },
        { "uniform, one thread", B_UNIFORM, 400003, 1, NULL, 0 },
        { "64 shuffled sorted blocks: a structured route", B_BLOCKS, 400003, 8, NULL, 0 },
        { "a sawtooth: ordered, the partition without the boost", B_SAW, 400003, 8, NULL, 0 },
        { "five values: a counting route", B_FEW, 400003, 8, NULL, 0 },
        { "sorted + 1% swaps: the near-sorted route", B_NEAR, 400003, 8, NULL, 0 },
        { "uniform with a NaN at the end: a permutation either way", B_UNIFORM_NAN, 400003, 8, NULL, -1 },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n;
        double *a = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!a || !bi || !bo) { free(a); free(bi); free(bo); printf("  thread boost: skipped a row (no memory)\n"); continue; }
        seed(27100 + r);
        int nan = boost_gen(rows[r].kind, a, n);
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        (void)logos_twist_last(39);
        logos_sort_f64(a, n, rows[r].P);
        size_t boosted = logos_twist_last(39);
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1; if (!nan) for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  thread boost, %s (n=%zu, P=%d): not a sorted permutation of the input's bits\n", rows[r].what, n, rows[r].P); }
#if !defined(LOGOS_NO_SODO) && !defined(LOGOS_NO_THREAD_POLICY)
        if (rows[r].e39 >= 0) { checks++; asserted++;
            if ((boosted > 0) != rows[r].e39) { fails++; printf("  FAIL  thread boost, %s (n=%zu, P=%d): the boost %s\n", rows[r].what, n, rows[r].P, boosted ? "fired" : "did not fire"); } }
#else
        (void)boosted;
#endif
        free(a); free(bi); free(bo);
    }
    printf("  thread boost: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the sequential hash map's cap (2026-11-10, FINDINGS 3ca): a map let in on a Chao1 estimate above 128 gives up at
 * MC_HASH_SEQ_CAP distinct values (logos_twist_last(40) counts it) and the sort goes on to the leaf; a map that stays under the
 * cap finishes, and LOGOS_MC_SEQCAP=0 restores the uncapped map. Whole numbers with a long tail are the shape: the sample of
 * 384 sees a few hundred values and the array holds tens of thousands. ---- */
static size_t mccap_zipf(size_t V) { double k = exp(frand() * log((double)V + 1.0)) - 1.0; size_t z = (size_t)k; return z >= V ? V - 1 : z; }
static int mccap_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    struct { const char *what; size_t V, n; int P; const char *env; int e40; double scale; } rows[] = {   /* e40: 1 must give up, 0 must not, -1 either */
        { "42,000 codes, long tail, 100,000 elements (the estimate decides whether the map is tried)", 42000, 100003, 1, NULL, -1, 1.0 },
        { "42,000 codes, 200,000 elements", 42000, 200003, 1, NULL, 1, 1.0 },
        { "the same, LOGOS_MC_SEQCAP=0: the uncapped map", 42000, 100003, 1, "LOGOS_MC_SEQCAP", 0, 1.0 },
        { "3,000 codes: the map stays under the cap", 3000, 100003, 1, NULL, 0, 1.0 },
        { "3,000 half-integers", 3000, 150003, 1, NULL, 0, 0.5 },
        { "42,000 codes at two threads", 42000, 250003, 2, NULL, -1, 1.0 },
        { "42,000 codes at eight threads: the other gate", 42000, 900003, 8, NULL, 0, 1.0 },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n, V = rows[r].V;
        double *a = malloc(n * sizeof(double)), *tab = malloc(V * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!a || !tab || !bi || !bo) { free(a); free(tab); free(bi); free(bo); printf("  hash map cap: skipped a row (no memory)\n"); continue; }
        seed(27300 + r);
        for (size_t k = 0; k < V; k++) tab[k] = (double)(501 + (nextr() % 99450)) * rows[r].scale;
        for (size_t i = 0; i < n; i++) a[i] = tab[mccap_zipf(V)];
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "0", 1);
        (void)logos_twist_last(40);
        logos_sort_f64(a, n, rows[r].P);
        size_t capped = logos_twist_last(40);
        if (rows[r].env) unsetenv(rows[r].env);
        int ok = 1; for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  hash map cap, %s (n=%zu, P=%d): not a sorted permutation of the input's bits\n", rows[r].what, n, rows[r].P); }
#if !defined(LOGOS_NO_SODO)
        if (rows[r].e40 >= 0) { checks++; asserted++;
            if ((capped > 0) != rows[r].e40) { fails++; printf("  FAIL  hash map cap, %s (n=%zu, P=%d): the map %s\n", rows[r].what, n, rows[r].P, capped ? "gave up at the cap" : "did not give up"); } }
#else
        (void)capped;
#endif
        free(a); free(tab); free(bi); free(bo);
    }
    printf("  hash map cap: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- heavy values counted in the histogram-free scatter (2026-11-10, FINDINGS 3ca): a value holding more than a bucket's region
 * used to make the partition decline; now the sample's heavy values are counted per thread and filled in (logos_twist_last(41)
 * counts the partitions that did). Rows: Zipf codes, one 10% value, a heavy +0.0 beside -0.0, a NaN, the switch, the view. ---- */
enum { HV_ZIPF, HV_ONE10, HV_ZERO, HV_ZIPF_NAN, HV_UNIFORM, HV_TWO };
static int hv_gen(int kind, double *a, size_t n) {
    switch (kind) {
    case HV_ZIPF: case HV_ZIPF_NAN: { enum { V = 42000 }; double *tab = malloc(V * sizeof(double)); if (!tab) { for (size_t i = 0; i < n; i++) a[i] = frand(); return 0; }
        for (size_t k = 0; k < V; k++) tab[k] = floor(frand() * 2147483648.0);
        for (size_t i = 0; i < n; i++) a[i] = tab[mccap_zipf(V)];
        free(tab); if (kind == HV_ZIPF_NAN) { a[n / 3] = NAN; return 1; } return 0; }
    case HV_ONE10: for (size_t i = 0; i < n; i++) a[i] = frand() < 0.10 ? 0.4375 : frand(); return 0;
    case HV_ZERO: for (size_t i = 0; i < n; i++) { double u = frand(); a[i] = u < 0.08 ? 0.0 : u < 0.085 ? -0.0 : frand() * 2.0 - 1.0; } return 0;
    case HV_TWO: for (size_t i = 0; i < n; i++) { double u = frand(); a[i] = u < 0.07 ? 1.0 : u < 0.14 ? nextafter(1.0, 2.0) : frand() * 2.0; } return 0;   /* two heavy values that share a bucket */
    default: for (size_t i = 0; i < n; i++) a[i] = frand(); return 0;
    }
}
static int heavy_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    struct { const char *what; int kind; size_t n; int P; const char *env; int e41; int view; } rows[] = {   /* e41: 1 must count heavy values, 0 must not, -1 either */
        { "Zipf codes, 8 threads", HV_ZIPF, 3000003, 8, NULL, 1, 0 },
        { "Zipf codes, 14 threads", HV_ZIPF, 5000003, 14, NULL, 1, 0 },
        { "Zipf codes, LOGOS_NO_HFHEAVY=1: the old decline", HV_ZIPF, 3000003, 8, "LOGOS_NO_HFHEAVY", 0, 0 },
        { "one value holding 10%", HV_ONE10, 2000003, 8, NULL, 1, 0 },
        { "a heavy +0.0 beside -0.0", HV_ZERO, 2000003, 8, NULL, 1, 0 },
        { "two heavy neighbours in one bucket", HV_TWO, 2000003, 8, NULL, 1, 0 },
        { "Zipf codes with a NaN: a permutation", HV_ZIPF_NAN, 3000003, 8, NULL, -1, 0 },
        { "uniform: nothing heavy", HV_UNIFORM, 2000003, 8, NULL, 0, 0 },
        { "Zipf codes, one thread", HV_ZIPF, 1000003, 1, NULL, 0, 0 },
        { "Zipf codes through the lazy view", HV_ZIPF, 3000003, 8, NULL, -1, 1 },
        { "one 10% value through the lazy view", HV_ONE10, 2000003, 8, NULL, -1, 1 },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t n = rows[r].n;
        double *a = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)); uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!a || !ref || !bi || !bo) { free(a); free(ref); free(bi); free(bo); printf("  heavy values: skipped a row (no memory)\n"); continue; }
        seed(27500 + r);
        int nan = hv_gen(rows[r].kind, a, n);
        memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (rows[r].env) setenv(rows[r].env, "1", 1);
        (void)logos_twist_last(41);
        int ok = 1;
        if (rows[r].view) {
            memcpy(ref, a, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d);
            logos_view *v = logos_view_create_f64(a, n, rows[r].P);
            if (!v) ok = 0;
            else {
                size_t ks[6] = { 0, n / 7, n / 3, n / 2, (2 * n) / 3, n - 1 };
                for (int i = 0; ok && i < 6; i++) ok = logos_view_at(v, ks[i]) == ref[ks[i]];
                ok = ok && logos_view_rank(v, ref[n / 2]) == lb_d(ref, n, ref[n / 2]);
                logos_view_all(v); logos_view_free(v);
            }
        } else logos_sort_f64(a, n, rows[r].P);
        size_t heavy = logos_twist_last(41);
        if (rows[r].env) unsetenv(rows[r].env);
        if (!nan) for (size_t q = 1; q < n; q++) if (a[q] < a[q - 1]) { ok = 0; break; }
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  heavy values, %s (n=%zu, P=%d): not a sorted permutation of the input's bits\n", rows[r].what, n, rows[r].P); }
#if !defined(LOGOS_NO_SODO) && !defined(LOGOS_NO_HISTFREE)
        if (rows[r].e41 >= 0) { checks++; asserted++;
            if ((heavy > 0) != rows[r].e41) { fails++; printf("  FAIL  heavy values, %s (n=%zu, P=%d): the scatter %s\n", rows[r].what, n, rows[r].P, heavy ? "counted heavy values" : "counted none"); } }
#else
        (void)heavy;
#endif
        free(a); free(ref); free(bi); free(bo);
    }
    printf("  heavy values: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- logos_argsort_f64_mt (2026-11-10, FINDINGS 3ca): the SAME permutation as the one-thread radix, bit for bit (it is stable),
 * on every shape and thread count; logos_twist_last(42) says whether the parallel partition ran. A reference permutation by
 * qsort on (key, index) checks the one-thread radix itself on the first row of each shape. ---- */
static const double *g_asref;
static int cmp_asref(const void *x, const void *y) {
    uint32_t i = *(const uint32_t *)x, j = *(const uint32_t *)y; uint64_t a, b; memcpy(&a, g_asref + i, 8); memcpy(&b, g_asref + j, 8);
    a = (a >> 63) ? ~a : (a | 0x8000000000000000ULL); b = (b >> 63) ? ~b : (b | 0x8000000000000000ULL);
    return a < b ? -1 : a > b ? 1 : i < j ? -1 : i > j;
}
enum { AS_UNIFORM, AS_STRADDLE, AS_FEW, AS_ZIPF, AS_SORTED, AS_NEAR, AS_SPECIAL, AS_ONEFAR, AS_ALLEQ, AS_DESC };
static void as_gen(int kind, double *a, size_t n) {
    switch (kind) {
    case AS_UNIFORM: for (size_t i = 0; i < n; i++) a[i] = frand(); break;
    case AS_STRADDLE: for (size_t i = 0; i < n; i++) a[i] = exp(20.0 * frand() - 10.0) * (frand() < 0.3 ? -1.0 : 1.0); break;   /* both signs, 29 binades around 1.0 */
    case AS_FEW: for (size_t i = 0; i < n; i++) a[i] = (double)(nextr() % 7) - 3.0; break;
    case AS_ZIPF: for (size_t i = 0; i < n; i++) a[i] = (double)(mccap_zipf(50000) * 7919 % 1000003); break;
    case AS_SORTED: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 3); break;                                              /* ascending with ties */
    case AS_NEAR: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t q = 0; q < n / 100; q++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case AS_SPECIAL: for (size_t i = 0; i < n; i++) { uint64_t r = nextr() % 100; a[i] = r < 2 ? NAN : r < 4 ? -NAN : r < 6 ? INFINITY : r < 8 ? -INFINITY : r < 14 ? 0.0 : r < 20 ? -0.0 : r < 24 ? 4.9e-324 * (double)(nextr() % 9) : frand() * 2.0 - 1.0; } break;
    case AS_ONEFAR: for (size_t i = 0; i < n; i++) a[i] = frand(); a[n / 2] = 1e300; a[n / 3] = -1e300; break;                  /* two far values: nearly every element in one cell */
    case AS_ALLEQ: for (size_t i = 0; i < n; i++) a[i] = 42.0; a[n - 1] = 41.0; break;
    default: for (size_t i = 0; i < n; i++) a[i] = (double)(n - i); break;
    }
}
static int argsort_mt_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    static const char *names[] = { "uniform", "both signs across 29 binades", "seven values", "Zipf whole numbers", "sorted with ties", "sorted + 1% swaps",
                                   "NaNs, infinities, signed zeros, subnormals", "two far values", "one value and a smaller last", "descending" };
    static const size_t ns[] = { 2, 3, 1000, 199999, 200000, 300007, 1500003 }; static const int Ps[] = { 1, 2, 8, 14 };
    for (int kind = 0; kind < 10; kind++) for (size_t si = 0; si < sizeof ns / sizeof ns[0]; si++) {
        size_t n = ns[si];
        double *a = malloc(n * sizeof(double)), *keep = malloc(n * sizeof(double)); uint32_t *ref = malloc(n * 4), *p = malloc(n * 4), *q = malloc(n * 4);
        if (!a || !keep || !ref || !p || !q) { free(a); free(keep); free(ref); free(p); free(q); printf("  argsort mt: skipped a row (no memory)\n"); continue; }
        seed(27700 + (uint64_t)kind * 31 + si);
        as_gen(kind, a, n); memcpy(keep, a, n * sizeof(double));
        logos_argsort_f64(a, n, ref);
        if (n <= 300007) {                                               /* the one-thread radix against a plain reference: (key, index) order */
            for (size_t i = 0; i < n; i++) q[i] = (uint32_t)i;
            g_asref = a; qsort(q, n, 4, cmp_asref);
            checks++; if (memcmp(q, ref, n * 4)) { fails++; printf("  FAIL  argsort mt, %s (n=%zu): logos_argsort_f64 is not the stable key order\n", names[kind], n); }
        }
        for (size_t pi = 0; pi < sizeof Ps / sizeof Ps[0]; pi++) {
            memset(p, 0xFF, n * 4);
            (void)logos_twist_last(42);
            logos_argsort_f64_mt(a, n, p, Ps[pi]);
            size_t par = logos_twist_last(42);
            checks++; if (memcmp(p, ref, n * 4) || memcmp(a, keep, n * sizeof(double))) { fails++; printf("  FAIL  argsort mt, %s (n=%zu, P=%d): not the one-thread radix's permutation, or the keys were written\n", names[kind], n, Ps[pi]); }
            int must = -1;                                               /* the parallel partition: from 200,000 elements at two threads or more, unless the keys are in order */
            if (Ps[pi] == 1 || n < 200000 || kind == AS_SORTED) must = 0;
            else if (n >= 300007) must = 1;
            if (must >= 0) { checks++; asserted++; if ((par > 0) != must) { fails++; printf("  FAIL  argsort mt, %s (n=%zu, P=%d): the parallel partition %s\n", names[kind], n, Ps[pi], par ? "ran" : "did not run"); } }
        }
        if (n == 300007) {                                               /* the switch, and a failed allocation's fallback */
            setenv("LOGOS_ARGSORT_SEQ", "1", 1); (void)logos_twist_last(42); memset(p, 0xFF, n * 4); logos_argsort_f64_mt(a, n, p, 8); unsetenv("LOGOS_ARGSORT_SEQ");
            checks += 2; if (memcmp(p, ref, n * 4)) { fails++; printf("  FAIL  argsort mt, %s: LOGOS_ARGSORT_SEQ=1 changed the permutation\n", names[kind]); }
            if (logos_twist_last(42) && kind != AS_SORTED) { fails++; printf("  FAIL  argsort mt, %s: LOGOS_ARGSORT_SEQ=1 still took the parallel partition\n", names[kind]); }
        }
        free(a); free(keep); free(ref); free(p); free(q);
    }
    printf("  argsort mt: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}
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
                    /* and in the parallel form, with a second NaN inside the PREFIX, which the prescan walks past. This row used to
                       demand that the two outputs be the same BYTES, on the premise that a declined tail route leaves both runs on
                       one deterministic path. That held only while a broken gate sent this input (a tenth of it a random tail) to the
                       near-sorted route: its inversion statistic read 7 pairs (2026-09-20). Read properly it shows about 5.1%, over the gate, and goes to the
                       partition, where the order inside a bucket depends on thread timing, and with a NaN present the leaves turn
                       that into a different output: two runs of the SAME configuration differed in 21 of 40 tries. What the engine
                       promises with a NaN is that the call returns and the output is a permutation of the input's bits, so that is
                       what is checked, for both runs. (The one-thread row above is deterministic and keeps the byte comparison.) */
                    orig[n / 3] = NAN;
                    uint64_t x0 = xsum(orig, n);
                    memcpy(a1, orig, n * sizeof(double));                                     logos_sort_f64(a1, n, 4);
                    memcpy(a2, orig, n * sizeof(double)); setenv("LOGOS_NO_TAIL", "1", 1);      logos_sort_f64(a2, n, 4);
                    unsetenv("LOGOS_NO_TAIL");
                    ok = xsum(a1, n) == x0 && xsum(a2, n) == x0;
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
    /* The sequential merge's three forms (2026-10-20, make tailladder): a half -> the vector merge where the build and CPU have it
       (logos_twist_last(-4)), else the loop; a tail of n/1000 -> the gallop; 5% -> the loop; LOGOS_TAIL_BRANCHY=1 -> the loop at any
       size. Each row names the form it expects (logos_twist_last(-11)) and checks the output ascending under < and the same BIT
       patterns as the input: the prefix and tail carry both signed zeros, which compare equal and must both survive the merge. */
    {
        int vec = logos_twist_last(-4) != 0, named = 0;
        const size_t ns[] = { 70000, 300001 };
        struct { double tf; int branchy, expect; const char *what; } rows[] = {
            { 0.5, 0, 3, "half" }, { 0.001, 0, 2, "n/1000 tail" }, { 0.05, 0, 1, "5% tail" }, { 0.5, 1, 1, "half, LOGOS_TAIL_BRANCHY" } };
        for (size_t ni = 0; ni < 2; ni++) for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
            size_t n = ns[ni], tl = (size_t)(rows[r].tf * (double)n), d = n - tl;
            double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
            seed(5150 + (uint64_t)r * 7 + ni);
            for (size_t i = 0; i < n; i++) { double v = floor(frand() * 2000.0) - 1000.0; orig[i] = v == 0.0 ? ((i & 1) ? -0.0 : 0.0) : v; }
            qsort(orig, d, sizeof(double), cmp_d);
            if (d && orig[d - 1] > orig[d]) {}                                  /* the tail is random: the prescan stops at d or later */
            memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            memcpy(a, orig, n * sizeof(double));
            if (rows[r].branchy) setenv("LOGOS_TAIL_BRANCHY", "1", 1);
            logos_sort_f64(a, n, 1);
            unsetenv("LOGOS_TAIL_BRANCHY");
            int kind = (int)logos_twist_last(-11), expect = rows[r].expect == 3 && !vec ? 1 : rows[r].expect;
            int ok = 1; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  tail merge %s n=%zu: not a sorted permutation of the input's bits\n", rows[r].what, n); }
            checks++; named++; if (kind != expect) { fails++; printf("  FAIL  tail merge %s n=%zu: form %d, expected %d\n", rows[r].what, n, kind, expect); }
            free(orig); free(a); free(bi); free(bo);
        }
        printf("  tail route: the merge's form named in %d rows (vector merge %s)\n", named, vec ? "available" : "NOT in this build or CPU: the loop expected");
    }
    /* The HEAD route (2026-10-20, symprobe): an unsorted head before a sorted suffix, the tail's mirror. Each row is a tail row's
       array negated and reversed; it names the form (logos_twist_last(-12), 0 = the route did not run) and checks the output against
       the input's bits. Signed zeros in both pieces; a NaN in the head must decline; a parallel sort must not take it. */
    {
        int vec = logos_twist_last(-4) != 0, named = 0;
        struct { double hf; size_t n; int P, nan, expect; const char *what; } rows[] = {
            { 0.5, 300001, 1, 0, 3, "half" }, { 0.001, 300001, 1, 0, 2, "n/1000 head" }, { 0.05, 300001, 1, 0, 1, "5% head" },
            { 0.05, 70000, 1, 0, 1, "5% head, 70000" }, { 0.05, 300001, 1, 1, 0, "NaN in the head: must decline" },
            { 0.05, 1000003, 4, 0, 0, "4 threads: must not take it" } };
        for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
            size_t n = rows[r].n, hl = (size_t)(rows[r].hf * (double)n), d = n - hl;
            double *x = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!x || !a || !bi || !bo) { free(x); free(a); free(bi); free(bo); continue; }
            seed(5250 + (uint64_t)r * 11);
            for (size_t i = 0; i < n; i++) { double v = floor(frand() * 2000.0) - 1000.0; x[i] = v == 0.0 ? ((i & 1) ? -0.0 : 0.0) : v + frand() * 0.5; }
            qsort(x, d, sizeof(double), cmp_d);
            for (size_t i = 0; i < n; i++) a[i] = -x[n - 1 - i];
            if (rows[r].nan) a[hl / 2] = NAN;
            memcpy(bi, a, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            (void)logos_twist_last(-12);
            logos_sort_f64(a, n, rows[r].P);
            int kind = (int)logos_twist_last(-12), expect = rows[r].expect == 3 && !vec ? 1 : rows[r].expect;
            int ok = 1;
            if (!rows[r].nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  head route %s n=%zu: not a sorted permutation of the input's bits\n", rows[r].what, n); }
            checks++; named++; if (kind != expect) { fails++; printf("  FAIL  head route %s n=%zu: form %d, expected %d\n", rows[r].what, n, kind, expect); }
            free(x); free(a); free(bi); free(bo);
        }
        printf("  tail route: the head route's form named in %d rows\n", named);
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

/* ---- the knot route: strands, crossings, and merges only where strands cross. Every case runs with the default cost gate
 * and with the gate lifted (LOGOS_KNOT_MAX=8), so the placements the default gate would decline (every element inside a
 * crossing, three strands live at once, one giant crossing cut into grains) are exercised as well. Oracle: qsort plus a byte
 * compare; the signed-zero mode is checked as ascending under '<' plus the bitwise multiset; with a NaN present the route has
 * no plan and must leave the answer bit-identical to a run with the route off. ---- */
static void knot_stairs(double *a, size_t n, size_t nb, double o, int shuffle, int zig) {
    if (nb < 1) nb = 1;
    size_t blk = (n + nb - 1) / nb; if (blk < 1) blk = 1;
    size_t *perm = malloc(nb * sizeof *perm); if (!perm) return;
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    if (shuffle) for (size_t k = nb - 1; k > 0; k--) { size_t r = nextr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) {
        size_t b = perm[(i / blk) % nb], j = i % blk;
        if (zig && ((i / blk) & 1)) j = blk - 1 - j;
        a[i] = (double)b * (double)blk + (double)j * (1.0 + o) + frand() * 0.5;
    }
    free(perm);
}
enum { KM_ORD, KM_SHUF, KM_X40, KM_TRIPLE, KM_ZIG, KM_ZIGSHUF, KM_TIES, KM_ZEROS, KM_INF, KM_NAN, KM_SPARSE, KM_TINY, KM_TWO, KM_DUPS, KM_N };
static void knot_gen(double *a, size_t n, int mode) {
    switch (mode) {
    case KM_ORD:     knot_stairs(a, n, 64, 1.0 / 9.0, 0, 0); break;      /* in value order, 20% of elements cross */
    case KM_SHUF:    knot_stairs(a, n, 64, 1.0 / 9.0, 1, 0); break;      /* shuffled */
    case KM_X40:     knot_stairs(a, n, 16, 0.25, 1, 0); break;           /* few, wide crossings */
    case KM_TRIPLE:  knot_stairs(a, n, 32, 1.5, 1, 0); break;            /* three strands live at once */
    case KM_ZIG:     knot_stairs(a, n, 64, 1.0 / 9.0, 0, 1); break;      /* every other block descends */
    case KM_ZIGSHUF: knot_stairs(a, n, 24, 0.3, 1, 1); break;
    case KM_TIES:    knot_stairs(a, n, 64, 1.0 / 9.0, 1, 0); for (size_t i = 0; i < n; i++) a[i] = floor(a[i]) + 0.25; break;        /* equal keys ACROSS strands */
    case KM_DUPS:    knot_stairs(a, n, 16, 0.25, 1, 0); for (size_t i = 0; i < n; i++) a[i] = floor(a[i] / 8.0) * 8.0 + 0.5; break; /* runs of equal keys, cut values that repeat */
    case KM_ZEROS: {                                                      /* a crossing that sits on zero, holding -0.0 and +0.0 in both strands */
        knot_stairs(a, n, 8, 0.25, 1, 0);
        double mid = 4.0 * (double)((n + 7) / 8) + 0.125 * (double)((n + 7) / 8); int flip = 0;
        for (size_t i = 0; i < n; i++) { a[i] -= mid; if (a[i] > -0.7 && a[i] < 0.7) { a[i] = (flip ^= 1) ? -0.0 : 0.0; } }
        /* a strand must stay ascending under '<=': -0.0 then +0.0 is, and so is the reverse */
    } break;
    case KM_INF:     knot_stairs(a, n, 16, 0.25, 1, 0);                   /* infinite endpoints, and +inf tied across two strands */
                     if (n > 64) { size_t blk = (n + 15) / 16; a[0] = -INFINITY; a[n - 1] = INFINITY; a[blk * 8 - 1] = INFINITY; } break;
    case KM_NAN:     knot_stairs(a, n, 64, 1.0 / 9.0, 0, 0); if (n > 10) a[n / 2] = NAN; break;
    case KM_SPARSE: {                                                     /* one sparse strand spanning everything, over dense disjoint blocks */
        size_t g = n / 65; knot_stairs(a + g, n - g, 32, 0.0, 1, 0);
        for (size_t i = 0; i < g; i++) a[i] = ((double)i + 0.5) * (double)(n - g) / (double)(g ? g : 1) + 0.123;
    } break;
    case KM_TINY:    knot_stairs(a, n, n / 64 ? n / 64 : 1, 0.2, 1, 0); break;  /* strands of 64: the run-length bail must fire */
    case KM_TWO: {                                                        /* two strands over one range, every element crosses; 40/60 so that the
                                                                             sorted-prefix-plus-tail route (prefix >= n/2) does not take it first.
                                                                             At one thread the HEAD route (suffix >= n/2, 2026-10-20) takes it, with
                                                                             the same vector kernel, and counts it in logos_twist_last(13) */
        size_t h = (2 * n) / 5;
        for (size_t i = 0; i < h; i++) a[i] = (double)i * 2.5 + 0.3;
        for (size_t i = h; i < n; i++) a[i] = (double)(i - h) * (5.0 / 3.0) + 1.1;
    } break;
    default: break;
    }
}
static int knot_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 1000, 5000, 70000, 300001, 1000003, 3000017 };
    for (int mode = 0; mode < KM_N; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            if (n > 2000000 && !(mode == KM_SHUF || mode == KM_TRIPLE || mode == KM_TWO)) continue;   /* the large size: many placement threads, grains */
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = NULL, *bo = NULL;
            if (!orig || !ref || !a) { free(orig); free(ref); free(a); continue; }
            seed(7700 + (uint64_t)mode * 19 + si);
            knot_gen(orig, n, mode);
            if (mode == KM_NAN) {
                double *a2 = malloc(n * sizeof(double));
                if (a2) {
                    memcpy(a, orig, n * sizeof(double));  unsetenv("LOGOS_NO_KNOT");       logos_sort_f64(a, n, 1);
                    memcpy(a2, orig, n * sizeof(double)); setenv("LOGOS_NO_KNOT", "1", 1); logos_sort_f64(a2, n, 1);
                    unsetenv("LOGOS_NO_KNOT");
                    int ok = memcmp(a, a2, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  knot NaN n=%zu\n", n); }
                }
                free(a2); free(orig); free(ref); free(a);
                continue;
            }
            if (mode == KM_ZEROS) {
                bi = malloc(n * sizeof(uint64_t)); bo = malloc(n * sizeof(uint64_t));
                if (!bi || !bo) { free(bi); free(bo); free(orig); free(ref); free(a); continue; }
                memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            } else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
            for (int lift = 0; lift < 2; lift++) {
                if (lift) setenv("LOGOS_KNOT_MAX", "8", 1); else unsetenv("LOGOS_KNOT_MAX");
                for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                    if (n > 2000000 && threads[ti] != 8) continue;
                    memcpy(a, orig, n * sizeof(double));
                    logos_sort_f64(a, n, threads[ti]);
                    int ok = 1;
                    if (mode == KM_ZEROS) {
                        for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                    } else ok = memcmp(a, ref, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  knot mode=%d n=%zu threads=%d gate=%s\n", mode, n, threads[ti], lift ? "lifted" : "default"); }
                }
            }
            unsetenv("LOGOS_KNOT_MAX");
            free(orig); free(ref); free(a); free(bi); free(bo);
        }
    }
    printf("  knot route: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the twist: runs that lie DESCENDING are consumed as they lie (one orientation bit in the run descriptor, a backward
 * copy, a two-ended merge, cuts found in logical coordinates) and are never reversed in memory. What had never run before it
 * was built: a twisted strand in a lone piece, in a two-strand crossing beside an untwisted one and beside another twisted
 * one, staged for a crossing of three, cut into grains, clipped by a placement tile, and one descending run that straddles a
 * detection chunk's boundary and has to be stitched back into ONE strand. Every case runs four ways: as shipped, with the
 * cost gate lifted, with LOGOS_NO_TWIST=1 (reverse in place, as before) and with LOGOS_NO_KNOT=1, which is the only way
 * plain relocation's twisted copy is reached. And every row that must take the path says so BY NAME: logos_twist_last()
 * counts the twisted strands a placement consumed, and a row whose expectation is not met fails even if the output is
 * sorted. Oracle: qsort plus a byte compare; signed zeros and the NaN row as the bitwise multiset (ascending under '<' too,
 * except with the NaN, where the engine promises a return and a permutation). ---- */
static void twist_blocks(double *a, size_t n, size_t nb, int every) {
    size_t blk = (n + nb - 1) / nb;
    for (size_t b = 0; b * blk < n; b++) {
        if (every <= 0 || (b % (size_t)every) != 0) continue;
        size_t lo = b * blk, hi = lo + blk > n ? n : lo + blk;
        for (size_t l = lo, r = hi - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }
    }
}
enum { TW_BLOCKS_ALL, TW_BLOCKS_ALT, TW_STAIRS_ALL, TW_STAIRS_ALT, TW_STRADDLE, TW_STRADDLE_X, TW_TRIPLE, TW_BITONIC, TW_DUPS, TW_ZEROS, TW_INF,
       TW_NAN, TW_TINY, TW_KINK_BLOCKS, TW_KINK_STAIRS, TW_N };
static const char *TW_NAME[TW_N] = { "blocks all", "blocks alt", "stairs all", "stairs alt", "straddle", "straddle+cross", "triple", "bitonic pair",
                                     "dups", "zeros", "inf", "nan", "tiny", "kinked blocks", "kinked stairs" };
static void twist_gen(double *a, size_t n, int mode) {
    switch (mode) {
    case TW_BLOCKS_ALL:  knot_stairs(a, n, 64, 0.0, 1, 0); twist_blocks(a, n, 64, 1); break;        /* disjoint, shuffled, every block descending */
    case TW_BLOCKS_ALT:  knot_stairs(a, n, 64, 0.0, 1, 0); twist_blocks(a, n, 64, 2); break;        /* every other one */
    case TW_STAIRS_ALL:  knot_stairs(a, n, 64, 1.0 / 9.0, 1, 0); twist_blocks(a, n, 64, 1); break;  /* crossings of two twisted strands */
    case TW_STAIRS_ALT:  knot_stairs(a, n, 64, 1.0 / 9.0, 1, 0); twist_blocks(a, n, 64, 2); break;  /* crossings of one twisted and one not */
    case TW_STRADDLE:    knot_stairs(a, n, 7, 0.0, 1, 0); twist_blocks(a, n, 7, 1); break;          /* 7 blocks: every chunk boundary at 2..8 threads cuts a descending run */
    case TW_STRADDLE_X:  knot_stairs(a, n, 7, 0.3, 1, 0); twist_blocks(a, n, 7, 1); break;          /* the same, and the stitched strands cross */
    case TW_TRIPLE:      knot_stairs(a, n, 32, 1.5, 1, 0); twist_blocks(a, n, 32, 3); break;        /* three strands live: twisted pieces staged for merge_all_runs */
    case TW_BITONIC: {                                                                               /* one run up, one run down, over the same range */
        size_t h = (2 * n) / 5;
        for (size_t i = 0; i < h; i++) a[i] = (double)i * 2.5 + 0.3;
        for (size_t i = h; i < n; i++) a[i] = (double)(n - 1 - i) * (5.0 / 3.0) + 1.1;
    } break;
    case TW_DUPS:        knot_stairs(a, n, 16, 0.25, 1, 0); for (size_t i = 0; i < n; i++) a[i] = floor(a[i] / 8.0) * 8.0 + 0.5;
                         twist_blocks(a, n, 16, 1); break;                                           /* a descending run is STRICT: equal keys break it */
    case TW_ZEROS: {
        knot_stairs(a, n, 8, 0.25, 1, 0);
        double mid = 4.0 * (double)((n + 7) / 8) + 0.125 * (double)((n + 7) / 8); int flip = 0;
        for (size_t i = 0; i < n; i++) { a[i] -= mid; if (a[i] > -0.7 && a[i] < 0.7) { a[i] = (flip ^= 1) ? -0.0 : 0.0; } }
        twist_blocks(a, n, 8, 1);
    } break;
    case TW_INF:         knot_stairs(a, n, 16, 0.25, 1, 0); twist_blocks(a, n, 16, 1);              /* a twisted strand's FIRST element is its largest */
                         if (n > 64) { size_t blk = (n + 15) / 16; a[0] = INFINITY; a[n - 1] = -INFINITY; a[blk * 8] = INFINITY; } break;
    case TW_NAN:         knot_stairs(a, n, 64, 1.0 / 9.0, 1, 0); twist_blocks(a, n, 64, 1); if (n > 10) a[n / 2] = NAN; break;
    case TW_KINK_BLOCKS: case TW_KINK_STAIRS: {                                                      /* every other block twisted, then 1% of adjacent pairs swapped: move I, in runs lying both ways */
        knot_stairs(a, n, 64, mode == TW_KINK_BLOCKS ? 0.0 : 1.0 / 9.0, 1, 0); twist_blocks(a, n, 64, 2);
        size_t blk = (n + 63) / 64;
        for (size_t q = 0; q < n / 100; q++) { size_t x = 2 * (nextr() % (n / 2 - 1)); if ((x + 1) % blk == 0 || x % blk == 0) continue; double v = a[x]; a[x] = a[x + 1]; a[x + 1] = v; }
    } break;
    case TW_TINY:        knot_stairs(a, n, n / 64 ? n / 64 : 1, 0.2, 1, 0); twist_blocks(a, n, n / 64 ? n / 64 : 1, 1); break;  /* the run-length bail */
    default: break;
    }
}
enum { TV_SHIPPED, TV_LIFTED, TV_NO_TWIST, TV_NO_KNOT, TV_SCALAR_SCAN, TV_NO_KINK, TV_HOLD8, TV_N };
static const char *TV_NAME[TV_N] = { "shipped", "gate lifted", "LOGOS_NO_TWIST", "LOGOS_NO_KNOT", "LOGOS_NO_FASTSCAN", "LOGOS_NO_KINK", "LOGOS_FASTSCAN_HOLD8" };
/* what logos_twist_last() must say: 1 = the knot route placed twisted strands, 2 = plain relocation did, 0 = nobody did,
   -1 = not asserted (a gate decides, and its verdict is not this section's business). Two verdicts that are the detector's
   and older than the twist: 64 blocks in 70,000 elements are runs too short for it (it bails on the average run length), and
   in a PARALLEL detection a chunk boundary can leave a tail shorter than RELOC_MIN_BODY, which has no gap test and fuses with
   the block after it into one run whose interval hides holes: plain relocation then declines (seen at 1,000,003 elements
   and 8 threads) where the knot route, which prices what crosses, does not. So relocation is named at ONE thread, and the
   parallel rows are counted: at least one of them must have gone through relocation's tile-clipped twisted copy. */
static int twist_expect(int mode, size_t n, int threads, int variant) {
    if (variant == TV_NO_TWIST) return 0;
    int kinked = mode == TW_KINK_BLOCKS || mode == TW_KINK_STAIRS;
    if (variant == TV_NO_KINK) { if (kinked) return -1; variant = TV_SHIPPED; }        /* without the repair a kinked array is the detector's to refuse */
    if (variant == TV_SCALAR_SCAN || variant == TV_HOLD8) variant = TV_SHIPPED;   /* the scalar scan and the old hold: the same verdicts, and (checked below) the same strands and kinks */
    int few = mode == TW_STRADDLE || mode == TW_STRADDLE_X;
    if (n < (few ? 70000u : 300001u)) return -1;
    int disjoint = mode == TW_BLOCKS_ALL || mode == TW_BLOCKS_ALT || mode == TW_STRADDLE || mode == TW_KINK_BLOCKS;
    int stairs = mode == TW_STAIRS_ALL || mode == TW_STAIRS_ALT || mode == TW_STRADDLE_X || mode == TW_KINK_STAIRS;
    if (variant == TV_NO_KNOT && kinked) return -1;      /* a kink on a block's last pair is no kink (its far neighbour disagrees): the pair that is left fuses with the next block, and plain relocation may decline */
    if (variant == TV_NO_KNOT) return disjoint ? (threads == 1 ? 2 : -1) : stairs ? 0 : -1;   /* relocation declines a staircase: one chain covers it */
    if (disjoint || stairs) return 1;
    if (mode == TW_TRIPLE && variant == TV_LIFTED) return 1;
    return -1;
}
static int twist_checks(void) {
    int checks = 0, fails = 0, asserted = 0, reloc_par = 0, have = (int)logos_twist_last(-2);
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 5000, 70000, 300001, 1000003, 3000017 };
    if (!have) printf("  twist: compiled out of this build (-DLOGOS_NO_TWIST or -DLOGOS_NO_RELOCATE): outputs are checked, and that NOTHING is placed twisted\n");
    for (int mode = 0; mode < TW_N; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            if (n > 2000000 && !(mode == TW_STAIRS_ALT || mode == TW_STRADDLE_X || mode == TW_TRIPLE)) continue;
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
            if (!orig || !ref || !a || !bi || !bo) { free(orig); free(ref); free(a); free(bi); free(bo); continue; }
            seed(9100 + (uint64_t)mode * 23 + si);
            twist_gen(orig, n, mode);
            int bits = mode == TW_ZEROS || mode == TW_NAN;
            if (bits) { memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits); }
            else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
            size_t shipped_knot[3] = { 0, 0, 0 }, shipped_kinks[3] = { 0, 0, 0 };
            for (int v = 0; v < TV_N; v++) {
                unsetenv("LOGOS_KNOT_MAX"); unsetenv("LOGOS_NO_TWIST"); unsetenv("LOGOS_NO_KNOT"); unsetenv("LOGOS_NO_FASTSCAN"); unsetenv("LOGOS_NO_KINK"); unsetenv("LOGOS_FASTSCAN_HOLD8");
                if (v == TV_NO_KINK) setenv("LOGOS_NO_KINK", "1", 1);
                if (v == TV_SCALAR_SCAN) setenv("LOGOS_NO_FASTSCAN", "1", 1);
                if (v == TV_HOLD8) setenv("LOGOS_FASTSCAN_HOLD8", "1", 1);
                if (v == TV_LIFTED) setenv("LOGOS_KNOT_MAX", "8", 1);
                if (v == TV_NO_TWIST) setenv("LOGOS_NO_TWIST", "1", 1);
                if (v == TV_NO_KNOT) setenv("LOGOS_NO_KNOT", "1", 1);
                for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                    if (n > 2000000 && threads[ti] != 8) continue;
                    memcpy(a, orig, n * sizeof(double));
                    (void)logos_twist_last(-1); (void)logos_twist_last(4);
                    logos_sort_f64(a, n, threads[ti]);
                    size_t by_knot = logos_twist_last(0), by_reloc = logos_twist_last(1), kinks = logos_twist_last(4);
                    /* move I by name: a kinked shape must have had kinks swapped back, and none with the repair switched off */
                    if (have && (mode == TW_KINK_BLOCKS || mode == TW_KINK_STAIRS) && n >= 300001 && v != TV_NO_TWIST) { checks++; asserted++;
                        if (v == TV_NO_KINK ? kinks != 0 : kinks == 0) { fails++; printf("  FAIL  twist, %s, n=%zu threads=%d, %s: %zu kinks swapped back\n", TW_NAME[mode], n, threads[ti], TV_NAME[v], kinks); } }
                    int ok = 1;
                    if (bits) {
                        if (mode != TW_NAN) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                        if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                    } else ok = memcmp(a, ref, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  twist, %s, n=%zu threads=%d, %s: wrong output\n", TW_NAME[mode], n, threads[ti], TV_NAME[v]); }
                    /* the scan in blocks of eight must find the SAME runs as the scalar scan: same strands, same twists */
                    if (v == TV_SHIPPED) { shipped_knot[ti] = by_knot; shipped_kinks[ti] = kinks; }
                    if (v == TV_SCALAR_SCAN || v == TV_HOLD8) { checks++; if (by_knot != shipped_knot[ti] || kinks != shipped_kinks[ti]) { fails++; printf("  FAIL  twist, %s, n=%zu threads=%d: %zu twisted strands and %zu kinks with %s, %zu and %zu with the shipped scan\n", TW_NAME[mode], n, threads[ti], by_knot, kinks, TV_NAME[v], shipped_knot[ti], shipped_kinks[ti]); } }
                    int want = twist_expect(mode, n, threads[ti], v); if (!have && want >= 0) want = 0;
                    if (v == TV_NO_KNOT && threads[ti] > 1 && by_reloc > 0 && by_knot == 0) reloc_par++;
                    if (want >= 0) {
                        int met = want == 1 ? by_knot > 0 && by_reloc == 0 : want == 2 ? by_reloc > 0 && by_knot == 0 : by_knot == 0 && by_reloc == 0;
                        checks++; asserted++;
                        if (!met) { fails++; printf("  FAIL  twist, %s, n=%zu threads=%d, %s: twisted strands placed by knot=%zu relocation=%zu, expected %s\n",
                                                    TW_NAME[mode], n, threads[ti], TV_NAME[v], by_knot, by_reloc, want == 1 ? "the knot route" : want == 2 ? "relocation" : "none"); }
                    }
                }
            }
            unsetenv("LOGOS_KNOT_MAX"); unsetenv("LOGOS_NO_TWIST"); unsetenv("LOGOS_NO_KNOT"); unsetenv("LOGOS_NO_FASTSCAN"); unsetenv("LOGOS_NO_KINK"); unsetenv("LOGOS_FASTSCAN_HOLD8");
            free(orig); free(ref); free(a); free(bi); free(bo);
        }
    }
    checks++; if (have && !reloc_par) { fails++; printf("  FAIL  twist: no parallel row went through plain relocation's twisted copy\n"); }
    printf("  twist: %d checks (%d of them name the path that must have run; %d parallel rows through relocation), %d failures\n", checks, asserted, reloc_par, fails);
    return fails;
}

/* ---- the galloping two-strand merge: a sparse strand spanning every value, over dense disjoint blocks, so that every block is a
 * two-strand crossing of a few sparse elements and the whole block. What had never run: a gallop in each of the four orientation
 * pairs (sparse and dense, each ascending or descending), a gallop that ends exactly at a strand's end, stretches of exactly the
 * threshold (one sparse element in 9), very long gallops (1 in 4097), TIES at a gallop's boundary (integer-valued blocks with the
 * sparse strand on the same integers), and signed zeros tied across the two strands. Three things are checked on every row: the
 * output against qsort (bitwise multiset where zeros are involved), the output BYTE FOR BYTE against the branch-free merge
 * (LOGOS_NO_KNOT_GALLOP=1: ties must go the same way, which is what keeps -0.0 and +0.0 where they were), and, where the knot route
 * must place, that gallops were made (logos_twist_last(5)) with the gallop on and none with it off. ---- */
enum { GL_UP, GL_SPARSE_TW, GL_BLOCKS_TW, GL_BOTH_TW, GL_TIES, GL_ZEROS, GL_N };
static const char *GL_NAME[GL_N] = { "sparse up, blocks up", "sparse down", "blocks alternately down", "both down", "ties at the boundary", "signed zeros" };
static void gallop_gen(double *a, size_t n, int mode, size_t every) {
    size_t g = n / (every + 1); if (g < 2) g = 2;
    knot_stairs(a + g, n - g, 32, 0.0, 1, 0);                                   /* 32 shuffled disjoint dense blocks */
    if (mode == GL_BLOCKS_TW || mode == GL_BOTH_TW) twist_blocks(a + g, n - g, 32, mode == GL_BOTH_TW ? 1 : 2);
    double span = (double)(n - g);
    for (size_t i = 0; i < g; i++) a[i] = ((double)i + 0.5) * span / (double)g + 0.123;
    if (mode == GL_SPARSE_TW || mode == GL_BOTH_TW) for (size_t l = 0, r = g - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }
    if (mode == GL_TIES) { for (size_t i = g; i < n; i++) a[i] = floor(a[i]); for (size_t i = 0; i < g; i++) a[i] = floor(a[i]); }   /* the sparse values land ON block values */
    if (mode == GL_ZEROS) {                                                         /* shift so that a block and the sparse strand both cross zero, with zeros of both signs */
        double mid = span / 2.0; int flip = 0;
        for (size_t i = 0; i < n; i++) { a[i] = floor(a[i]) - floor(mid); if (a[i] == 0.0) a[i] = (flip ^= 1) ? -0.0 : 0.0; }
    }
}
/* ---- the vector two-strand merge (2026-09-26): the knot route's balanced crossings through the AVX2 bitonic kernel. Every knot
 * shape that merges, at one thread and four, sizes that reach the parallel placement; the output must be ascending under < and
 * the same bit patterns as the input (signed zeros and infinities included), the kernel must have been taken at one thread
 * (logos_twist_last(13)) and never with LOGOS_NO_SIMD_MERGE=1, and both settings must agree as multisets. Skipped, and said so,
 * on a build or CPU without it (logos_twist_last(-4)). ---- */
static int vmerge_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (!logos_twist_last(-4)) { printf("  vector merge: not in this build or CPU, skipped\n"); return 0; }
    const int threads[] = { 1, 4 };
    const size_t sizes[] = { 70000, 300001, 1000003 };
    const int modes[] = { KM_SHUF, KM_ZIG, KM_ZIGSHUF, KM_TIES, KM_ZEROS, KM_INF, KM_TRIPLE, KM_TWO, KM_DUPS };
    for (size_t mi = 0; mi < sizeof modes / sizeof *modes; mi++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        int mode = modes[mi]; size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double)), *b = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !b || !bi || !bo) { free(orig); free(a); free(b); free(bi); free(bo); continue; }
        seed(13100 + (uint64_t)mode * 23 + si);
        knot_gen(orig, n, mode);
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            unsetenv("LOGOS_NO_SIMD_MERGE"); unsetenv("LOGOS_NO_KNOT");
            memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(13);
            logos_sort_f64(a, n, threads[ti]);
            size_t von = logos_twist_last(13);
            setenv("LOGOS_NO_SIMD_MERGE", "1", 1);
            memcpy(b, orig, n * sizeof(double));
            logos_sort_f64(b, n, threads[ti]);
            size_t voff = logos_twist_last(13);
            unsetenv("LOGOS_NO_SIMD_MERGE");
            int ok = 1;
            for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  vector merge, knot mode %d, n=%zu threads=%d: wrong output\n", mode, n, threads[ti]); }
            ok = 1; for (size_t i = 1; i < n; i++) if (b[i] < b[i - 1]) { ok = 0; break; }
            memcpy(bo, b, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  vector merge, knot mode %d, n=%zu threads=%d: wrong output with the kernel off\n", mode, n, threads[ti]); }
            checks++; asserted++; if (voff) { fails++; printf("  FAIL  vector merge, knot mode %d, n=%zu threads=%d: %zu vector merges with LOGOS_NO_SIMD_MERGE=1\n", mode, n, threads[ti], voff); }
            if (threads[ti] == 1) { checks++; asserted++;
                if (!von) { fails++; printf("  FAIL  vector merge, knot mode %d, n=%zu threads=1: the kernel was never taken\n", mode, n); } }
        }
        free(orig); free(a); free(b); free(bi); free(bo);
    }
    printf("  vector merge: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

static int gallop_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 70000, 300001, 1000003 };
    const size_t evs[] = { 8, 9, 64, 4096 };
    for (int mode = 0; mode < GL_N; mode++) for (size_t ei = 0; ei < sizeof evs / sizeof *evs; ei++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si], every = evs[ei];
        if (every == 4096 && n < 300001) continue;
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double)), *b = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !ref || !a || !b || !bi || !bo) { free(orig); free(ref); free(a); free(b); free(bi); free(bo); continue; }
        seed(12000 + (uint64_t)mode * 41 + ei * 7 + si);
        gallop_gen(orig, n, mode, every);
        int bits = mode == GL_ZEROS;
        if (bits) { memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits); }
        else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            /* the byte comparison below is the gallop against the SCALAR branch-free merge, whose tie order it reproduces; the
               vector merge (2026-09-26) orders -0.0 against +0.0 its own way, so it is held off on both sides of this row */
            setenv("LOGOS_NO_SIMD_MERGE", "1", 1);
            unsetenv("LOGOS_NO_KNOT_GALLOP");
            memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(5);
            logos_sort_f64(a, n, threads[ti]);
            size_t gon = logos_twist_last(5);
            setenv("LOGOS_NO_KNOT_GALLOP", "1", 1);
            memcpy(b, orig, n * sizeof(double));
            logos_sort_f64(b, n, threads[ti]);
            size_t goff = logos_twist_last(5);
            unsetenv("LOGOS_NO_KNOT_GALLOP"); unsetenv("LOGOS_NO_SIMD_MERGE");
            int ok = 1;
            if (bits) {
                for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            } else ok = memcmp(a, ref, n * sizeof(double)) == 0;
            checks++; if (!ok) { fails++; printf("  FAIL  gallop, %s, 1 in %zu, n=%zu threads=%d: wrong output\n", GL_NAME[mode], every + 1, n, threads[ti]); }
            checks++; if (memcmp(a, b, n * sizeof(double))) { fails++; printf("  FAIL  gallop, %s, 1 in %zu, n=%zu threads=%d: bytes differ from the branch-free merge\n", GL_NAME[mode], every + 1, n, threads[ti]); }
            checks++; asserted++; if (goff) { fails++; printf("  FAIL  gallop, %s, 1 in %zu, n=%zu threads=%d: %zu gallops with LOGOS_NO_KNOT_GALLOP=1\n", GL_NAME[mode], every + 1, n, threads[ti], goff); }
            if (every >= 64 && n >= 300001) { checks++; asserted++;
                if (!gon) { fails++; printf("  FAIL  gallop, %s, 1 in %zu, n=%zu threads=%d: no gallop was made\n", GL_NAME[mode], every + 1, n, threads[ti]); } }
        }
        free(orig); free(ref); free(a); free(b); free(bi); free(bo);
    }
    printf("  galloping two-strand merge: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the knot route as a LITERAL linked list (LOGOS_KNOT_LIST=1; opt-in, measured slower, kept because it is the structure
 * the route was first described as). An opt-in path is a path nobody runs, so this section runs it on every `make test`: a
 * link per element, twisted runs linked backwards, crossings of two and of three untied by relinking, lone pieces entered in
 * the middle, the stitches, the walk with its guessed next and the plain pointer walk. Rows where the knot route must place
 * assert through logos_twist_last(3) that the placement really went through the list. Oracle: qsort and a byte compare. ---- */
static int list_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 70000, 300001, 1000003 };
    const int tw_modes[] = { TW_BLOCKS_ALL, TW_BLOCKS_ALT, TW_STAIRS_ALL, TW_STAIRS_ALT, TW_STRADDLE_X, TW_TRIPLE, TW_BITONIC, TW_DUPS, TW_INF, TW_TINY };
    for (size_t mi = 0; mi < sizeof tw_modes / sizeof *tw_modes; mi++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        int mode = tw_modes[mi]; size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        if (!orig || !ref || !a) { free(orig); free(ref); free(a); continue; }
        seed(9500 + (uint64_t)mode * 31 + si);
        twist_gen(orig, n, mode);
        memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d);
        for (int plain = 0; plain < 2; plain++) for (int lift = 0; lift < 2; lift++) {
            setenv("LOGOS_KNOT_LIST", "1", 1);
            if (plain) setenv("LOGOS_LIST_PLAIN", "1", 1); else unsetenv("LOGOS_LIST_PLAIN");
            if (lift) setenv("LOGOS_KNOT_MAX", "8", 1); else unsetenv("LOGOS_KNOT_MAX");
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                (void)logos_twist_last(3);
                logos_sort_f64(a, n, threads[ti]);
                size_t went = logos_twist_last(3);
                int ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  list, %s, n=%zu threads=%d, %s walk, gate %s: wrong output\n", TW_NAME[mode], n, threads[ti], plain ? "plain" : "guessed", lift ? "lifted" : "default"); }
                int want = twist_expect(mode, n, threads[ti], lift ? TV_LIFTED : TV_SHIPPED);          /* 1: the knot route must have placed it */
                if (want == 1) { checks++; asserted++; if (went != 1) { fails++; printf("  FAIL  list, %s, n=%zu threads=%d: %zu placements went through the list, expected 1\n", TW_NAME[mode], n, threads[ti], went); } }
            }
        }
        unsetenv("LOGOS_KNOT_LIST"); unsetenv("LOGOS_LIST_PLAIN"); unsetenv("LOGOS_KNOT_MAX");
        free(orig); free(ref); free(a);
    }
    printf("  knot route as a linked list: %d checks (%d of them assert the list was the path), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the whole array's orientation bit: a sequential sort whose sample reads back to front, and whose MIRROR IMAGE passes a
 * directional gate (a descending suffix of half the array, the reach ladder turned round, streams that descend in arrival
 * order), is reversed once and sent through the dispatcher again. What had never run: the re-entry itself (buffers released
 * and taken again, the guard against a second reversal), each of the three gates, and every way of NOT firing: mirrored
 * blocks (relocation takes them first, twisted), a mirror with no cheap route behind it, noise, a parallel sort. Every row
 * names what must have happened through logos_mirror_count(), with the bit on and with LOGOS_NO_MIRROR=1. Oracle: qsort and a
 * byte compare; signed zeros and the NaN row as the bitwise multiset. ---- */
enum { MR_REACH, MR_REACH_STRAY, MR_PREFIX, MR_PREFIX_TIES, MR_STREAMS, MR_NEAR, MR_BLOCKS, MR_TEETH, MR_UNIFORM, MR_ZEROS, MR_INF, MR_NAN, MR_N };
static const char *MR_NAME[MR_N] = { "short reach", "short reach + stray", "prefix_95", "prefix_95, equal keys", "4 streams", "near_sorted 1%", "block_perm",
                                     "64 descending teeth", "uniform", "prefix_95, signed zeros", "short reach, infinities", "short reach, a NaN" };
static void mirror_gen(double *a, size_t n, int mode) {
    switch (mode) {
    case MR_REACH: case MR_REACH_STRAY: case MR_INF: case MR_NAN:
        for (size_t i = 0; i < n; i++) a[i] = (double)i + (frand() * 2.0 - 1.0) * 60.0;
        if (mode == MR_REACH_STRAY) a[n / 3] = 4.0 * (double)n;
        if (mode == MR_INF) { a[0] = -INFINITY; a[n - 1] = INFINITY; a[n / 2] = INFINITY; }
        break;
    case MR_PREFIX: for (size_t i = 0; i < n; i++) a[i] = i < n - n / 20 ? (double)i : frand() * (double)n; break;
    case MR_PREFIX_TIES: for (size_t i = 0; i < n; i++) a[i] = i < n - n / 20 ? floor((double)i / 7.0) : floor(frand() * (double)n / 7.0); break;
    case MR_ZEROS: for (size_t i = 0; i < n; i++) a[i] = i < n - n / 20 ? ((double)i - (double)(n / 2)) : (frand() - 0.5) * (double)n;
                   { for (size_t i = n / 2 - 40; i < n / 2 + 40 && i < n; i++) a[i] = (i & 1) ? -0.0 : 0.0; } break;
    case MR_STREAMS: for (size_t i = 0; i < n; i++) a[i] = (double)(i % 4) * (double)n + (double)(i / 4) + frand() * 0.5; break;
    case MR_NEAR: for (size_t i = 0; i < n; i++) a[i] = (double)i + frand() * 0.5;
                  for (size_t k = 0; k < n / 100; k++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case MR_BLOCKS: knot_stairs(a, n, 64, 0.0, 1, 0); break;
    case MR_TEETH: { size_t tl = (n + 63) / 64; for (size_t i = 0; i < n; i++) a[i] = (double)(i % tl) + (double)(i / tl) * 1e-4; } break;   /* mirrored below: descending teeth */
    default: for (size_t i = 0; i < n; i++) a[i] = frand() * 2e9 - 1e9; break;
    }
    for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }      /* every shape is handed over MIRRORED */
    if (mode == MR_NAN && n > 10) a[n / 2 + 1] = NAN;                     /* off the sample's points as a rule: the engine promises a return and a permutation */
}
/* 1 = mirrored exactly once, 0 = never, -1 = not asserted */
static int mirror_expect(int mode, size_t n, int threads, int off) {
    if (off || (threads > 1 && n >= 200000)) return 0;                     /* switched off, or a parallel sort (one thread per 100k elements): never offered */
    if (n < 65536) return 0;                                               /* below MIRROR_MIN_N */
    switch (mode) {
    case MR_REACH: case MR_REACH_STRAY: case MR_INF: return n >= 262144 ? 1 : -1;
    case MR_PREFIX: case MR_PREFIX_TIES: case MR_ZEROS: return 1;
    case MR_STREAMS: return n == 300001 ? -1 : 1;                          /* at 300,001 the sample shows the stream route no modes, as it lies or mirrored */
    case MR_NEAR: case MR_NAN: return -1;                                  /* near_sorted: the mirrored ladder passes at some sizes, as the plain one does on the plain array */
    default: return 0;                                                     /* near_sorted: no gate behind it. blocks: relocation first. teeth, noise: nothing to gain */
    }
}
static int mirror_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 5000, 70000, 300001, 1000003 };
    for (int mode = 0; mode < MR_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !ref || !a || !bi || !bo) { free(orig); free(ref); free(a); free(bi); free(bo); continue; }
        seed(9900 + (uint64_t)mode * 29 + si);
        mirror_gen(orig, n, mode);
        int bits = mode == MR_ZEROS || mode == MR_NAN;
        if (bits) { memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits); }
        else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
        for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_MIRROR", "1", 1); else unsetenv("LOGOS_NO_MIRROR");
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                (void)logos_mirror_count(1);
                logos_sort_f64(a, n, threads[ti]);
                size_t got = logos_mirror_count(1);
                int ok = 1;
                if (bits) {
                    if (mode != MR_NAN) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                    memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                    if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                } else ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  mirror, %s, n=%zu threads=%d%s: wrong output\n", MR_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_MIRROR" : ""); }
                int want = mirror_expect(mode, n, threads[ti], off);
                if (want >= 0) { checks++; asserted++;
                    if (got != (size_t)want) { fails++; printf("  FAIL  mirror, %s, n=%zu threads=%d%s: mirrored %zu time(s), expected %d\n", MR_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_MIRROR" : "", got, want); } }
            }
        }
        unsetenv("LOGOS_NO_MIRROR");
        free(orig); free(ref); free(a); free(bi); free(bo);
    }
    printf("  whole-array orientation: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the loop-back (R0 on the sphere): an ascending suffix that continues into the ascending prefix across the wrap is one
 * strand; it is rotated to the front once and the dispatcher entered again. What had never run: the rotation (through the pool,
 * through malloc, and the in-place reversals when neither can be had are the resource section's business), the re-entry and its
 * guard, the equality at the wrap, a NaN or an infinity at the seam, signed zeros across it, and every way of NOT firing: the end
 * above the start, a joined strand under half the array, a suffix too short to be worth a rotation when the prefix already is
 * half, noise, a small array. Every row names what must have happened through logos_twist_last(7), with the loop-back on and with
 * LOGOS_NO_LOOPBACK=1, at 1, 4 and 8 threads (the prescan is the same at every thread count).
 * And the relocation detector's floor, which the same probe found: at ONE thread blocks of ~1000 now reach relocation and the knot
 * route (twisted strands placed > 0) and with LOGOS_RELOC_AVG=4096, the old floor, they do not; at P threads the floor is still
 * 4096: blocks of 1000 are not placed there and blocks of 4096 are. Oracle: qsort and a byte compare; signed zeros,
 * infinities and the NaN row as ascending-under-< plus the bitwise multiset. ---- */
enum { LB_RING, LB_RING_TIES, LB_PREFIX, LB_SMALLPREFIX, LB_TWOBLOCKS, LB_ZEROS, LB_INF, LB_NOWRAP, LB_SHORTSUF, LB_TOOSHORT, LB_NAN_END, LB_UNIFORM, LB_N };
static const char *LB_NAME[LB_N] = { "ring buffer (sorted, rotated)", "ring buffer, equal keys, equal at the wrap", "prefix_95 + tail, rotated",
                                     "30% prefix, noise, 40% suffix below it", "two sorted blocks swapped", "ring buffer, signed zeros at the seam",
                                     "ring buffer, infinities at the seam", "prefix_95 + tail as it lies (end above start)", "60% prefix, noise, 1% suffix below it",
                                     "20% prefix, noise, 20% suffix below it", "ring buffer, a NaN at the end", "uniform" };
static void loop_gen(double *a, size_t n, int mode) {
    size_t r = n * 37 / 100;                                           /* the rotation: a[i] = s[(i + r) mod n] */
    double *s = malloc(n * sizeof(double)); if (!s) { for (size_t i = 0; i < n; i++) a[i] = (double)i; return; }
    switch (mode) {
    case LB_RING: case LB_NAN_END: for (size_t i = 0; i < n; i++) s[i] = (double)i + frand() * 0.5; break;
    case LB_RING_TIES: for (size_t i = 0; i < n; i++) s[i] = floor((double)i / 7.0); r = 7 * (n / 20); if (r >= n) r = n / 2; break;   /* the seam lands between two equal keys' groups: a[n-1] == a[0] - 1 or equal */
    case LB_PREFIX: for (size_t i = 0; i < n; i++) s[i] = i < n - n / 20 ? (double)i : frand() * (double)n; break;
    case LB_ZEROS: for (size_t i = 0; i < n; i++) s[i] = (double)i - (double)r; for (size_t i = r - 20; i < r + 20 && i < n; i++) s[i] = (i & 1) ? -0.0 : 0.0; break;
    case LB_INF: for (size_t i = 0; i < n; i++) s[i] = (double)i; s[0] = -INFINITY; s[1] = -INFINITY; s[n - 1] = INFINITY; break;
    default: break;
    }
    switch (mode) {
    case LB_RING: case LB_RING_TIES: case LB_PREFIX: case LB_ZEROS: case LB_INF: case LB_NAN_END:
        for (size_t i = 0; i < n; i++) a[i] = s[(i + r) % n];
        if (mode == LB_NAN_END) a[n - 1] = NAN;
        break;
    case LB_SMALLPREFIX: {                                             /* [0.3 n sorted, high] [0.3 n noise] [0.4 n sorted, below the prefix] */
        size_t p = n * 3 / 10, q = n * 6 / 10;
        for (size_t i = 0; i < n; i++) a[i] = i < p ? (double)n + (double)i : i < q ? frand() * 2.0 * (double)n : (double)(i - q); } break;
    case LB_TWOBLOCKS: { size_t h = n / 3; for (size_t i = 0; i < n; i++) a[i] = i < n - h ? (double)(h + i) + frand() * 0.5 : (double)(i - (n - h)) + frand() * 0.5; } break;
    case LB_NOWRAP: for (size_t i = 0; i < n; i++) a[i] = i < n - n / 20 ? (double)i : (double)n + frand() * (double)n; break;   /* tail above everything: a[n-1] > a[0] */
    case LB_SHORTSUF: {                                                /* d = 0.6 n >= n/2 and the suffix (0.01 n) is under n/16: the tail route's, no rotation */
        size_t p = n * 6 / 10, q = n - n / 100;
        for (size_t i = 0; i < n; i++) a[i] = i < p ? (double)n + (double)i : i < q ? frand() * 3.0 * (double)n : (double)(i - q); } break;
    case LB_TOOSHORT: {                                                /* the joined strand is 0.4 n: under half */
        size_t p = n * 2 / 10, q = n * 8 / 10;
        for (size_t i = 0; i < n; i++) a[i] = i < p ? (double)n + (double)i : i < q ? frand() * 2.0 * (double)n : (double)(i - q); } break;
    default: for (size_t i = 0; i < n; i++) a[i] = frand() * 2e9 - 1e9; break;
    }
    free(s);
}
static int loop_expect(int mode, size_t n, int threads, int off) {     /* 1 = rotated exactly once, 0 = never, -1 = not asserted */
    if (off || n < 4096) return 0;
    int par = threads > 1 && n >= 200000;                               /* one thread per 100k elements: below 200k a sort runs on one */
    switch (mode) {
    case LB_SMALLPREFIX: case LB_TWOBLOCKS: return par ? (mode == LB_TWOBLOCKS ? 1 : 0) : 1;   /* P threads: only when at most n/16 is left outside the strand */
    case LB_PREFIX: return 1;                                          /* 5% left outside: fires at every thread count */
    case LB_RING: case LB_RING_TIES: case LB_ZEROS: case LB_INF: return 1;
    case LB_UNIFORM: return 0;
    default: return 0;
    }
}
static int loop_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (!logos_twist_last(-4)) { printf("  loop-back: not in this build (LOGOS_NO_LOOPBACK)\n"); }
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 3000, 5000, 70000, 300001, 1000003 };
    for (int mode = 0; mode < LB_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !ref || !a || !bi || !bo) { free(orig); free(ref); free(a); free(bi); free(bo); continue; }
        seed(7700 + (uint64_t)mode * 31 + si);
        loop_gen(orig, n, mode);
        int bits = mode == LB_ZEROS || mode == LB_NAN_END || mode == LB_INF;
        if (bits) { memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits); }
        else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
        for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_LOOPBACK", "1", 1); else unsetenv("LOGOS_NO_LOOPBACK");
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                (void)logos_twist_last(7);
                logos_sort_f64(a, n, threads[ti]);
                size_t got = logos_twist_last(7);
                int ok = 1;
                if (bits) {
                    if (mode != LB_NAN_END) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                    memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                    if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                } else ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  loop-back, %s, n=%zu threads=%d%s: wrong output\n", LB_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_LOOPBACK" : ""); }
                int want = logos_twist_last(-4) ? loop_expect(mode, n, threads[ti], off) : 0;
                if (want >= 0) { checks++; asserted++;
                    if (got != (size_t)want) { fails++; printf("  FAIL  loop-back, %s, n=%zu threads=%d%s: rotated %zu time(s), expected %d\n", LB_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_LOOPBACK" : "", got, want); } }
            }
        }
        unsetenv("LOGOS_NO_LOOPBACK");
        free(orig); free(ref); free(a); free(bi); free(bo);
    }
    /* the relocation detector's floor: twisted blocks (every other one descending) are placed by relocation or the knot route
       exactly when the detector keeps going, and every twisted strand placed shows in logos_twist_last(0) + (1) */
    if (logos_twist_last(-2)) {
        struct { size_t n, blk; int threads; const char *env; int want; } R[] = {
            { 1000003, 1000, 1, NULL, 1 }, { 1000003, 1000, 1, "4096", 0 }, { 300001, 300, 1, NULL, 1 }, { 1000003, 3000, 1, NULL, 1 },
            { 2097152, 4096, 8, NULL, 1 },                              /* runs of exactly the parallel floor, chunks aligned with them: placed */
            { 2097152, 1000, 8, NULL, 0 },                              /* the parallel floor is still 4096 */
        };
        for (size_t k = 0; k < sizeof R / sizeof *R; k++) {
            size_t n = R[k].n; double *a = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)); if (!a || !ref) { free(a); free(ref); continue; }
            seed(7900 + k); knot_stairs(a, n, (n + R[k].blk - 1) / R[k].blk, 0.0, 1, 1);
            memcpy(ref, a, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d);
            if (R[k].env) setenv("LOGOS_RELOC_AVG", R[k].env, 1); else unsetenv("LOGOS_RELOC_AVG");
            (void)logos_twist_last(-1);
            logos_sort_f64(a, n, R[k].threads);
            size_t tw = logos_twist_last(0) + logos_twist_last(1);
            unsetenv("LOGOS_RELOC_AVG");
            checks++; if (memcmp(a, ref, n * sizeof(double))) { fails++; printf("  FAIL  reloc floor, blocks of %zu, n=%zu threads=%d%s: wrong output\n", R[k].blk, n, R[k].threads, R[k].env ? ", LOGOS_RELOC_AVG=4096" : ""); }
            checks++; asserted++;
            if ((tw > 0) != R[k].want) { fails++; printf("  FAIL  reloc floor, blocks of %zu, n=%zu threads=%d%s: %zu twisted strands placed, expected %s\n", R[k].blk, n, R[k].threads, R[k].env ? ", LOGOS_RELOC_AVG=4096" : "", tw, R[k].want ? "some" : "none"); }
            free(a); free(ref);
        }
    }
    printf("  loop-back and relocation floor: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- round six: the wave route, the seam, strays inside strands. What had
 * never run: a wave's pieces sorted where they lie and placed by the knot sweep; a rotation found in the reach route's fences before
 * a block is sorted; strays lifted out of runs in four forms, the holes closed in one pass and the strays merged back in place of
 * the copy-back; and every way of NOT firing (parallel
 * sorts, the switches, strays past the cap -- n/16 since 2026-10-21, n/64 before, when this row held 3% --, clean shapes). Every row that can names its path through logos_twist_last:
 * 8 = the wave placed it, 9 = the seam rotated it, 10 = strays lifted. Oracle: qsort and a byte
 * compare; the NaN row as the bitwise multiset. ---- */
enum { R6_WAVE, R6_WAVE2, R6_WAVEWIDE, R6_SINE_CLEAN, R6_SEAM, R6_REACH, R6_STRAY01, R6_STRAY1, R6_STRAY9, R6_STRAYNAN, R6_BLOCKS, R6_UNIFORM, R6_ONEKINK, R6_ONEKINKDESC, R6_ONESTRAY, R6_N };
static const char *R6_NAME[R6_N] = { "sine 32 cycles, noise 50", "sine 8 cycles, noise 50", "sine 32 cycles, swing 2 (the cost gate declines it)", "sine 32 cycles, no noise", "short reach, rotated (a noisy ring buffer)",
    "short reach as it lies", "blocks + 0.1% strays", "blocks + 1% strays", "blocks + 9% strays (past the cap)", "blocks + 0.1% strays + a NaN",
    "block_perm", "uniform", "one strand, 1% adjacent swaps", "one DESCENDING strand, 1% adjacent swaps", "one strand, 0.1% strays" };
static void r6_gen(double *a, size_t n, int mode) {
    switch (mode) {
    case R6_WAVE: case R6_WAVE2: case R6_WAVEWIDE: case R6_SINE_CLEAN: {
        int cyc = mode == R6_WAVE2 ? 8 : 32; double sw = mode == R6_WAVEWIDE ? 2.0 : 0.6, nz = mode == R6_SINE_CLEAN ? 0.0 : 50.0;
        double T = (double)n / cyc, A = sw * T / 2.0;
        for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (nz > 0 ? (frand() * 2.0 - 1.0) * nz : 0.0);
    } break;
    case R6_SEAM: case R6_REACH: {
        size_t r = n * 37 / 100; double *s = malloc(n * sizeof(double));
        if (!s) { for (size_t i = 0; i < n; i++) a[i] = (double)i; break; }
        for (size_t i = 0; i < n; i++) s[i] = (double)i + (frand() * 2.0 - 1.0) * 60.0;
        for (size_t i = 0; i < n; i++) a[i] = mode == R6_SEAM ? s[(i + r) % n] : s[i];
        free(s);
    } break;
    case R6_STRAY01: case R6_STRAY1: case R6_STRAY9: case R6_STRAYNAN: case R6_BLOCKS: {
        knot_stairs(a, n, 64, 0.0, 1, 0);
        double f = mode == R6_STRAY01 || mode == R6_STRAYNAN ? 0.001 : mode == R6_STRAY1 ? 0.01 : mode == R6_STRAY9 ? 0.09 : 0.0;
        for (size_t q = 0; q < (size_t)(f * (double)n); q++) a[nextr() % n] = frand() * (double)n;
        if (mode == R6_STRAYNAN && n > 10) a[n / 3] = NAN;
    } break;
    case R6_ONEKINK: case R6_ONEKINKDESC: case R6_ONESTRAY: {           /* one sorted strand: timestamps a few units apart */
        double c = 1e6; for (size_t i = 0; i < n; i++) { c += 1.0 + frand() * 9.0; a[i] = c; }
        if (mode == R6_ONESTRAY) { for (size_t q = 0; q < n / 1000; q++) a[nextr() % n] = frand() * c; }
        else for (size_t q = 0; q < n / 100; q++) { size_t x = nextr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; }
        if (mode == R6_ONEKINKDESC) for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }
    } break;
    default: for (size_t i = 0; i < n; i++) a[i] = frand() * 2e9 - 1e9; break;
    }
}
/* which witness a mode answers to, and what it must read: 1 = fired (>0), 0 = did not, -1 = not asserted */
static int r6_which(int mode) {
    switch (mode) { case R6_WAVE: case R6_WAVE2: case R6_WAVEWIDE: case R6_SINE_CLEAN: return 8; case R6_SEAM: case R6_REACH: return 9;
                    case R6_UNIFORM: return 8; case R6_ONEKINK: case R6_ONEKINKDESC: return 12; default: return 10; }
}
static int r6_expect(int mode, size_t n, int threads, int off) {
    int par = threads > 1 && n >= 200000;                               /* one thread per 100k elements */
    if (off == 1) return 0;                                             /* off == 2: the three-pass finish, same witnesses */
    switch (mode) {
    case R6_WAVE: case R6_WAVE2: return (!par && n >= (1u << 20)) ? 1 : 0;   /* sequential sorts of a million and up */
    case R6_SEAM: return !par ? 1 : 0;
    case R6_STRAY01: case R6_STRAY1: return 1;                          /* every detector worker lifts strays since 2026-09-26 (it was w[0] at P == 1) */
    case R6_STRAYNAN: return -1;
    case R6_ONEKINK: return 1;                                          /* one run at every P since 2026-09-26: the join repairs a kink across a chunk's
                                                                           end, at either end of the array, and a swapped pair that begins a chunk */
    case R6_ONEKINKDESC: return -1;                                     /* strays are lifted from ASCENDING runs only: a descending strand's unrepaired
                                                                           kinks leave ~100 twisted runs, which the knot route unties (crossing cost 312) */
    case R6_ONESTRAY: return !par ? 1 : -1;                             /* strays lifted (witness 10), whether one run is left or two. At P the reach
                                                                           probe reads FAR strays as sparse but not kink-shaped, so near-sorted takes it first */
    default: return 0;                                                  /* the clean sine (the knot route first), reach as it lies, 9% strays, blocks, noise */
    }
}
static const char *r6_switch(int mode) {
    switch (r6_which(mode)) { case 8: return "LOGOS_NO_WAVE"; case 9: return "LOGOS_NO_SEAM"; case 12: return "LOGOS_NO_ONERUN"; default: return "LOGOS_NO_STRAY"; }
}
static int round6_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const int threads[] = { 1, 4 };
    const size_t sizes[] = { 300001, 1048576, 2000003 };
    for (int mode = 0; mode < R6_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !ref || !a || !bi || !bo) { free(orig); free(ref); free(a); free(bi); free(bo); continue; }
        seed(6600 + (uint64_t)mode * 37 + si);
        r6_gen(orig, n, mode);
        int bits = mode == R6_STRAYNAN, which = r6_which(mode);
        if (bits) { memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits); }
        else { memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d); }
        for (int off = 0; off < 3; off++) {
            if (off == 2 && mode != R6_ONEKINK && mode != R6_ONESTRAY) continue;
            if (off == 1) setenv(r6_switch(mode), "1", 1); else unsetenv(r6_switch(mode));
            if (off == 2) setenv("LOGOS_ONERUN_COPY", "1", 1); else unsetenv("LOGOS_ONERUN_COPY");
            {   int pv = 0;
                for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                    memcpy(a, orig, n * sizeof(double));
                    (void)logos_twist_last(which);
                    logos_sort_f64(a, n, threads[ti]);
                    size_t got = logos_twist_last(which);
                    int ok = 1;
                    if (bits) { memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits); ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0; }
                    else ok = memcmp(a, ref, n * sizeof(double)) == 0;
                    checks++; if (!ok) { fails++; printf("  FAIL  round 6, %s, n=%zu threads=%d%s%s: wrong output\n", R6_NAME[mode], n, threads[ti], off == 2 ? ", three-pass finish" : off ? ", switched off" : "", pv ? ", strided pivot" : ""); }
                    int want = r6_expect(mode, n, threads[ti], off);
                    if (want >= 0) { checks++; asserted++;
                        if ((got > 0) != want) { fails++; printf("  FAIL  round 6, %s, n=%zu threads=%d%s: witness %d read %zu, expected %s\n", R6_NAME[mode], n, threads[ti], off == 2 ? ", three-pass finish" : off ? ", switched off" : "", which, got, want ? "some" : "none"); } }
                }
            }
        }
        unsetenv(r6_switch(mode)); unsetenv("LOGOS_ONERUN_COPY");
        free(orig); free(ref); free(a); free(bi); free(bo);
    }
    printf("  wave, seam, strays: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the prescan in blocks (2026-09-22). From 256 elements up the dispatcher's first loop reads sixteen elements at a time once the
 * direction is known; it must return exactly what the scalar line returns (ascending?, descending?, first descent), and
 * logos_prescan_agree runs both. Rows: sizes around the 256 threshold and the 16-element block, and a few large ones; six bases
 * (ascending, descending, level, ascending and descending with ties, alternating signed zeros); one adjacent swap or one NaN at every
 * position within 40 of either end and at 17 consecutive positions in the middle. Every row is also sorted: ascending under '<' and
 * the same bitwise multiset (a NaN row: the multiset only). ---- */
static int prescan_checks(void) {
    int checks = 0, fails = 0;
    const size_t sizes[] = { 255, 256, 257, 271, 272, 273, 300, 1000, 4099, 70001 };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *b = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!b || !a || !bi || !bo) { free(b); free(a); free(bi); free(bo); continue; }
        for (int base = 0; base < 6; base++) {
            size_t pos[120]; int np = 0;
            for (size_t p = 1; p <= 40 && p < n; p++) pos[np++] = p;
            for (size_t p = n > 40 ? n - 40 : 1; p < n; p++) if (p > 40) pos[np++] = p;
            for (size_t k = 0; k < 17 && n / 2 + k < n; k++) if (n / 2 + k > 40 && n / 2 + k < n - 40) pos[np++] = n / 2 + k;
            for (int pi = -1; pi < np; pi++) for (int kind = 0; kind < 2; kind++) {
                if (pi < 0 && kind) continue;                            /* the unmodified base once */
                for (size_t i = 0; i < n; i++) {
                    switch (base) {
                    case 0: b[i] = (double)i * 0.5 - 7.0; break;
                    case 1: b[i] = (double)(n - i); break;
                    case 2: b[i] = 3.0; break;
                    case 3: b[i] = (double)(i / 3); break;
                    case 4: b[i] = (double)((n - i) / 3); break;
                    default: b[i] = (i & 1) ? -0.0 : 0.0; break;
                    }
                }
                if (pi >= 0) { size_t p = pos[pi];
                    if (kind == 0) { double x = b[p - 1]; b[p - 1] = b[p]; b[p] = x; } else b[p] = NAN; }
                int agree = logos_prescan_agree(b, n);
                checks++; if (!agree) { fails++; printf("  FAIL  prescan in blocks, n=%zu base %d %s at %zu: disagrees with the scalar line\n", n, base, kind ? "NaN" : "swap", pi >= 0 ? pos[pi] : 0); }
                if (n > 4099 && pi >= 0 && (pi & 7)) continue;          /* the large size: every eighth row sorted */
                memcpy(a, b, n * sizeof(double));
                logos_sort_f64(a, n, 1);
                int ok = 1, nan = pi >= 0 && kind == 1;
                if (!nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                memcpy(bi, b, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  prescan in blocks, n=%zu base %d %s at %zu: wrong output\n", n, base, kind ? "NaN" : "swap", pi >= 0 ? pos[pi] : 0); }
            }
        }
        free(b); free(a); free(bi); free(bo);
    }
    printf("  prescan in blocks: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- ties in descending runs (2026-09-22). Timestamps with ~1.25% ties (quarter-microsecond resolution near 1.7e15): sorted blocks,
 * shuffled, every other block written descending, disjoint (plain relocation or the knot route must place them: witnesses 0 and 1)
 * or overlapping their neighbour by a third (a staircase: tied TWISTED strands crossing, which the knot route never met before).
 * 1 and 4 threads, three sizes; ascending under '<' plus the same bitwise multiset. At one thread the disjoint rows must name a
 * run route; LOGOS_DESC_STRICT=1 rows check only the output. ---- */
static int desc_tie_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const size_t sizes[] = { 300001, 1048576, 2000003 };
    const int threads[] = { 1, 4 };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) for (int shape = 0; shape < 2; shape++) {
        size_t n = sizes[si], nb = 64;
        double *t0 = malloc(n * sizeof(double)), *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t)); size_t *perm = malloc(nb * sizeof(size_t));
        if (!t0 || !orig || !a || !bi || !bo || !perm) { free(t0); free(orig); free(a); free(bi); free(bo); free(perm); continue; }
        seed(7100 + si * 13 + (uint64_t)shape);
        double c = 1.7e15; for (size_t i = 0; i < n; i++) { c += -log(1.0 - frand()) * 10.0; t0[i] = c; }
        for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t j = nextr() % (k + 1), x = perm[k]; perm[k] = perm[j]; perm[j] = x; }
        size_t o = 0;
        for (size_t k = 0; k < nb; k++) {
            size_t b = perm[k], s0 = b * n / nb, s1 = (b + 1) * n / nb, len = s1 - s0;
            for (size_t i = 0; i < len; i++) orig[o + i] = t0[s0 + i];
            if (shape == 1 && b + 1 < nb) {             /* every third element taken from the next block's first third: the ranges overlap */
                size_t s2 = (b + 2) * n / nb; for (size_t i = 0; i < len; i += 3) { size_t q = s1 + i / 3; if (q < s2) orig[o + i] = t0[q]; }
                qsort(orig + o, len, sizeof(double), cmp_d); }
            if (k & 1) for (size_t l = o, r = o + len - 1; l < r; l++, r--) { double x = orig[l]; orig[l] = orig[r]; orig[r] = x; }
            o += len;
        }
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (int strict = 0; strict < 2; strict++) {
            if (strict) setenv("LOGOS_DESC_STRICT", "1", 1); else unsetenv("LOGOS_DESC_STRICT");
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double));
                (void)logos_twist_last(0); (void)logos_twist_last(1);
                logos_sort_f64(a, n, threads[ti]);
                size_t placed = logos_twist_last(0) + logos_twist_last(1);
                int ok = 1; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits); if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  descending ties, %s, n=%zu threads=%d%s: wrong output\n", shape ? "staircase" : "disjoint", n, threads[ti], strict ? ", strict" : ""); }
                if (!strict && shape == 0 && threads[ti] == 1) { asserted++; checks++;
                    if (!placed) { fails++; printf("  FAIL  descending ties, disjoint, n=%zu, 1 thread: no run route placed the blocks\n", n); } }
            }
        }
        unsetenv("LOGOS_DESC_STRICT");
        free(t0); free(orig); free(a); free(bi); free(bo); free(perm);
    }
    printf("  descending ties: %d checks (%d of them name the route), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- infinities. +-inf are ordinary IEEE values with a place in the order, but until 2026-09-19 no test put one in front of
 * a PARALLEL sort, and eight of ten shapes crashed: with an infinite sample extreme every value-linear map computed
 * inf * 0 = NaN (the uniformity certificate then measured a distance of 0 and certified the data; the splitter value table
 * took a scale of 0 for finite; flash and coarse buckets cast NaN to an index). Position 0 is always a sample point, so
 * a[0] = -inf was enough. Placements: an infinity AT sample points (Weyl i = 0 and i = 1), a tenth of the array infinite so
 * that the splitters themselves are, and finite extremes whose difference overflows. Checked as ascending under '<' plus an
 * identical bitwise multiset. ---- */
static int infinity_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t n = 300001, nb = 64, blk = (n + nb - 1) / nb;
    static const char *SN[] = { "uniform", "heavy_tail", "quantized", "near_sorted", "block_perm", "int_wide" };
    double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
    uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
    if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); return 1; }
    for (int s = 0; s < 6; s++) for (int place = 0; place < 5; place++) {
        seed(3100 + (uint64_t)s * 11 + (uint64_t)place);
        for (size_t i = 0; i < n; i++) switch (s) {
            case 0: orig[i] = frand() * 2e9 - 1e9; break;
            case 1: orig[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2); break;
            case 2: orig[i] = (double)(nextr() % 4096) / 64.0; break;
            case 3: orig[i] = (double)i + frand() * 0.5; break;
            case 4: orig[i] = (double)(((i / blk) * 37) % nb) * (double)blk + (double)(i % blk) + frand() * 0.5; break;
            default: orig[i] = (double)(int32_t)nextr(); break;
        }
        if (s == 3) for (size_t i = 0; i < n / 100; i++) { size_t x = nextr() % n, y = nextr() % n; double t = orig[x]; orig[x] = orig[y]; orig[y] = t; }
        size_t w1 = (size_t)(0.6180339887498949 * (double)n);
        if (place == 0 || place == 2) orig[0] = -INFINITY;
        if (place == 1 || place == 2) orig[w1] = INFINITY;
        if (place == 3) for (size_t i = 0; i < n; i += 10) orig[i] = (i / 10) & 1 ? INFINITY : -INFINITY;
        if (place == 4) { orig[0] = -1.7e308; orig[w1] = 1.7e308; }
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            int ok = 1;
            for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  infinity shape=%s placement=%d threads=%d\n", SN[s], place, threads[ti]); }
        }
    }
    free(orig); free(a); free(bi); free(bo);
    printf("  infinities: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- buckets no splitter can divide. One value holding most of the array lands in a single bucket, and every parallel leaf
 * loop used to give EACH thread a context as large as that bucket (104 bytes per element at 10M and 14 threads, make memprobe).
 * Contexts now fit a typical bucket and an oversized one borrows room while it is sorted: the shared jumbo block, or a private
 * temporary when two oversized buckets are in hand at once. These shapes force both, in f64, in int32 and through the lazy
 * view's bucket sorter. Oracle: qsort and a byte compare. ---- */
static int dominant_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 4, 8 };
    const size_t sizes[] = { 300001, 1000003 };
    for (int mode = 0; mode < 4; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            double *orig = malloc(n * sizeof(double)), *ref = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
            int32_t *io = malloc(n * sizeof(int32_t)), *ir = malloc(n * sizeof(int32_t)), *ia = malloc(n * sizeof(int32_t));
            if (!orig || !ref || !a || !io || !ir || !ia) { free(orig); free(ref); free(a); free(io); free(ir); free(ia); continue; }
            seed(5150 + (uint64_t)mode * 23 + si);
            for (size_t i = 0; i < n; i++) {
                uint64_t r = nextr(); double u = frand() * 2e9 - 1e9; int32_t w = (int32_t)nextr();
                switch (mode) {
                case 0:  orig[i] = (r % 10) ? 12345.0 : u;                 io[i] = (r % 10) ? 12345 : w; break;                        /* one value, 90% */
                case 1:  orig[i] = (r % 100) < 45 ? -7.5 : (r % 100) < 90 ? 7.5 : u; io[i] = (r % 100) < 45 ? -7 : (r % 100) < 90 ? 7 : w; break;   /* two giants at once */
                case 2:  orig[i] = (r % 100) < 30 ? 1.0 : (r % 100) < 60 ? 2.0 : (r % 100) < 90 ? 3.0 : u;
                         io[i] = (r % 100) < 30 ? 1 : (r % 100) < 60 ? 2 : (r % 100) < 90 ? 3 : w; break;                             /* three */
                default: orig[i] = (r % 10) ? 12345.0 + (double)(r % 3) * 1e-9 : u; io[i] = (r % 10) ? 12345 + (int32_t)(r % 3) : w; break; /* a giant bucket that is NOT one value: its leaf really runs */
                }
            }
            memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d);
            memcpy(ir, io, n * sizeof(int32_t));  qsort(ir, n, sizeof(int32_t), cmp_i);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, threads[ti]);
                int ok = memcmp(a, ref, n * sizeof(double)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  dominant f64 mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
                memcpy(ia, io, n * sizeof(int32_t)); logos_sort_i32(ia, n, threads[ti]);
                ok = memcmp(ia, ir, n * sizeof(int32_t)) == 0;
                checks++; if (!ok) { fails++; printf("  FAIL  dominant i32 mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
                memcpy(a, orig, n * sizeof(double));
                logos_view *v = logos_view_create_f64(a, n, threads[ti]);
                ok = v != NULL;
                if (ok) { logos_view_all(v); ok = memcmp(a, ref, n * sizeof(double)) == 0; logos_view_free(v); }
                checks++; if (!ok) { fails++; printf("  FAIL  dominant view mode=%d n=%zu threads=%d\n", mode, n, threads[ti]); }
            }
            logos_release_buffers();                        /* and what is kept between calls can be given back and built again */
            memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, 4);
            { int ok = memcmp(a, ref, n * sizeof(double)) == 0; checks++; if (!ok) { fails++; printf("  FAIL  dominant after release mode=%d n=%zu\n", mode, n); } }
            free(orig); free(ref); free(a); free(io); free(ir); free(ia);
        }
    }
    printf("  dominant values: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- IEEE special values. An outside review (2026-09-19) listed "NaNs, infinities, subnormals, signalling NaNs and mixed signed
 * zeros" as untested, and it was right: NaN-free data holding SUBNORMALS came back UNSORTED from every parallel sort of 300k
 * elements or more (a bucket whose range is narrower than ~1e-305 makes classes / range overflow to +inf; the minimum then
 * computes 0 * inf = NaN and landed in the LAST coarse bucket), and NaNs with a sign or a payload came back as different bit
 * patterns (the spill sort rewrote them as the canonical NaN). Three contracts, each checked on four shapes:
 *   NaN-free mixes  ascending under '<' and the bitwise multiset preserved: subnormals alone, subnormals inside normal data,
 *                   a finite range whose width overflows, and every special value at once (+-inf, +-0, +-denorm_min, DBL_MAX);
 *   NaN present     logos_sort_f64 promises no order, but the output must still be a permutation of the input BITS;
 *   nan_last        logos_sort_f64_nan_last: every NaN at the tail with its bits intact, the prefix ascending. ---- */
static double ieee_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static void ieee_gen(double *a, size_t n, int shape) {
    size_t nb = 64, blk = (n + nb - 1) / nb;
    for (size_t i = 0; i < n; i++) switch (shape) {
        case 0:  a[i] = frand() * 2e9 - 1e9; break;
        case 1:  a[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2); break;
        case 2:  a[i] = (double)(nextr() % 4096) / 64.0; break;
        default: a[i] = (double)(((i / blk) * 37) % nb) * (double)blk + (double)(i % blk) + frand() * 0.5; break;
    }
}
static void ieee_mix(double *a, size_t n, int mix) {
    size_t w1 = (size_t)(0.6180339887498949 * (double)n);           /* the second Weyl sample point; position 0 is the first */
    switch (mix) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = (frand() - 0.5) * 4e-311; break;                      /* nothing but subnormals */
    case 1: for (size_t i = 0; i < n; i += 3) a[i] = (frand() - 0.5) * 1e-310; break;                   /* a third subnormal, inside normal data: the failing case */
    case 2: a[0] = -1.7e308; a[w1] = 1.7e308; for (size_t i = 5; i < n; i += 11) a[i] = (frand() - 0.5) * 2.0 * 1.7e308; break;   /* finite, but max - min overflows */
    case 3: for (size_t i = 0; i < n; i += 7) { uint64_t k = (i / 7) % 7; a[i] = k == 0 ? INFINITY : k == 1 ? -INFINITY : k == 2 ? -0.0 : k == 3 ? 0.0 : k == 4 ? 4.9e-324 : k == 5 ? -4.9e-324 : 1.7976931348623157e308; } break;
    case 4: for (size_t i = 0; i < n; i += 101) a[i] = NAN; break;                                       /* 1% NaN */
    case 5: a[0] = NAN; a[w1] = NAN; a[n - 1] = NAN; break;                                              /* NaN exactly where the samplers look */
    case 6: for (size_t i = 0; i < n; i += 2) a[i] = NAN; break;                                         /* half NaN */
    default: for (size_t i = 0; i < n; i += 50) { uint64_t k = (i / 50) % 4;                              /* signalling, quiet, negative and payload NaNs */
                 a[i] = ieee_bits(k == 0 ? 0x7FF0000000000001ULL : k == 1 ? 0x7FF8000000000000ULL : k == 2 ? 0xFFF8000000000123ULL : 0xFFF0000000000001ULL); } break;
    }
}
/* ---- the persistent worker pool (2026-10-08): parallel phases run on threads kept between calls. Rows: every route that spawns
 * (partition, lattice, few distinct, sorted input through the parallel prescan, shuffled blocks, near-sorted), at thread counts
 * that make the pool GROW (2, then 8, then 16) and after a release that STOPS it; a fork after the pool exists, whose child must
 * sort in parallel and exit (a child inherits the bookkeeping and none of the threads, so without the atfork reset it hangs); and
 * every row once with LOGOS_NO_POOL=1. Output: ascending under < and the input's bitwise multiset; the pool must have served
 * groups where it is on (logos_twist_last(19)) and none where it is off. ---- */
#include <sys/wait.h>
enum { WP_UNIFORM, WP_QUANT, WP_FEW, WP_SORTED, WP_BLOCKS, WP_NEAR, WP_N };
static const char *WP_NAME[WP_N] = { "uniform", "quantized", "few distinct", "sorted", "shuffled blocks", "near-sorted" };
static void wp_gen(double *a, size_t n, int mode) {
    for (size_t i = 0; i < n; i++) {
        if (mode == WP_UNIFORM) a[i] = frand() * 2e9 - 1e9;
        else if (mode == WP_QUANT) a[i] = (double)(nextr() % 4096) / 64.0;
        else if (mode == WP_FEW) a[i] = (double)(nextr() % 5) * 1e6;
        else a[i] = (double)i + 0.5;
    }
    if (mode == WP_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t i = nextr() % n, j = nextr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; }
    if (mode == WP_BLOCKS) { size_t nb = 64, blk = n / nb; double *t = malloc(n * sizeof(double)); if (!t) return;
        size_t perm[64]; for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t r = nextr() % (k + 1); size_t x = perm[k]; perm[k] = perm[r]; perm[r] = x; }
        size_t o = 0; for (size_t k = 0; k < nb; k++) for (size_t i = perm[k] * blk; i < (perm[k] + 1) * blk; i++) t[o++] = a[i];
        for (; o < n; o++) t[o] = a[o];
        memcpy(a, t, n * sizeof(double)); free(t); }
}
static int wp_check(const double *a, const uint64_t *bi, uint64_t *bo, size_t n) {
    for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0;
    memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
    return memcmp(bi, bo, n * sizeof(uint64_t)) == 0;
}
static int pool_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const size_t n = 1100003;                               /* past PRESCAN_PAR_MIN, so sorted input spawns too */
    const int threads[] = { 2, 8, 16, 4 };
    double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
    uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
    if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); printf("  worker pool: skipped (no memory)\n"); return 0; }
    for (int off = 0; off < 2; off++) for (int mode = 0; mode < WP_N; mode++) {
        seed(19100 + (uint64_t)mode * 43);
        wp_gen(orig, n, mode);
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        if (off) setenv("LOGOS_NO_POOL", "1", 1);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            if (ti == 3) logos_release_buffers();                 /* the pool stopped: the next call must build it again */
            (void)logos_twist_last(19);
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            size_t served = logos_twist_last(19);
            checks++; if (!wp_check(a, bi, bo, n)) { fails++; printf("  FAIL  worker pool, %s, threads=%d%s: wrong output\n", WP_NAME[mode], threads[ti], off ? ", LOGOS_NO_POOL=1" : ""); }
            checks++; asserted++;
            if (off && served) { fails++; printf("  FAIL  worker pool, %s, threads=%d: %zu groups served with LOGOS_NO_POOL=1\n", WP_NAME[mode], threads[ti], served); }
            if (!off && !served) { fails++; printf("  FAIL  worker pool, %s, threads=%d: the pool served no group\n", WP_NAME[mode], threads[ti]); }
        }
        if (off) unsetenv("LOGOS_NO_POOL");
    }
    /* fork with the pool alive: the child sorts in parallel and must finish (a watchdog turns a hang into a failure). Not under
       ThreadSanitizer, whose runtime refuses to start threads in a child forked from a multi-threaded parent ("starting new
       threads after multi-threaded fork is not supported. Dying"): the child exits and the row would read as wrong output. */
#ifdef __SANITIZE_THREAD__
    printf("  worker pool: the fork row is skipped under ThreadSanitizer (its runtime forbids threads after a multi-threaded fork)\n");
    free(orig); free(a); free(bi); free(bo);
    printf("  worker pool: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
#endif
    seed(19900); wp_gen(orig, n, WP_UNIFORM);
    memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
    memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, 8);           /* the pool exists now */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60);
        memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, 8);
        int ok = wp_check(a, bi, bo, n);
        memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, 4);
        ok = ok && wp_check(a, bi, bo, n);
        _exit(ok ? 0 : 3);
    }
    int st = 0;
    checks++; asserted++;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fails++; printf("  FAIL  worker pool: a child forked with the pool alive %s\n", pid < 0 ? "could not be forked" : WIFSIGNALED(st) ? "hung or crashed" : "sorted wrongly");
    }
    free(orig); free(a); free(bi); free(bo);
    printf("  worker pool: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the pool trim (2026-10-08, trim_note / trim_idle): after a large sort, enough small ones hand the large one's pooled pages back
 * (MADV_DONTNEED), and the next large sort reuses those pages, which now read as zeros. Rows: every route whose buffers are pooled
 * (the one-thread buffer and context, the parallel partition's regions, relocation, near-sorted; the counting routes pool nothing),
 * each sorted large, then 12 small
 * sorts, then large again, the output checked every time; the trim must have handed something back where it is on
 * (logos_twist_last(21)) and nothing without LOGOS_TRIM (it is opt-in: LOGOS_TRIM=1 or hard, both run). ---- */
static int trim_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const size_t n = 1100003, n2 = 20011;
    struct { int mode, P; } rows[] = { { WP_UNIFORM, 1 }, { WP_UNIFORM, 8 }, { WP_NEAR, 1 }, { WP_NEAR, 8 }, { WP_BLOCKS, 1 }, { WP_BLOCKS, 8 } };
    double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double)), *s = malloc(n2 * sizeof(double));
    uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
    if (!orig || !a || !s || !bi || !bo) { free(orig); free(a); free(s); free(bi); free(bo); printf("  pool trim: skipped (no memory)\n"); return 0; }
    for (int off = 0; off < 2; off++) for (size_t r = 0; r < sizeof rows / sizeof *rows; r++) {
        int mode = rows[r].mode, P = rows[r].P;
        logos_release_buffers();
        if (off) unsetenv("LOGOS_TRIM"); else setenv("LOGOS_TRIM", (r & 1) ? "hard" : "1", 1);   /* opt-in: both forms */
        seed(19500 + (uint64_t)r * 17);
        wp_gen(orig, n, mode);
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        (void)logos_twist_last(21);
        for (int round = 0; round < 2; round++) {
            memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, P);          /* the second round reuses trimmed pages */
            checks++; if (!wp_check(a, bi, bo, n)) { fails++; printf("  FAIL  pool trim, %s, threads=%d, round %d%s: wrong output\n", WP_NAME[mode], P, round, off ? ", trim off" : ""); }
            for (int k = 0; k < 12; k++) {
                for (size_t i = 0; i < n2; i++) s[i] = frand();
                logos_sort_f64(s, n2, P);
                int ok = 1; for (size_t i = 1; i < n2; i++) if (s[i] < s[i - 1]) ok = 0;
                checks++; if (!ok) { fails++; printf("  FAIL  pool trim, small sort after %s\n", WP_NAME[mode]); }
            }
        }
        size_t back = logos_twist_last(21);
        checks++; asserted++;
        if (off && back) { fails++; printf("  FAIL  pool trim, %s, threads=%d: %zu bytes handed back with the trim off\n", WP_NAME[mode], P, back); }
        if (!off && !back) { fails++; printf("  FAIL  pool trim, %s, threads=%d: nothing handed back after 12 small sorts\n", WP_NAME[mode], P); }
    }
    unsetenv("LOGOS_TRIM");
    free(orig); free(a); free(s); free(bi); free(bo);
    printf("  pool trim: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the mode grid (2026-10-07, mdx_t / md_certify): clustered data with more than three modes, each uniform inside, is scattered
 * through a value grid that names the mode with one cell lookup and at most one compare. Rows aim at what a cell table could get
 * wrong: values in the GULFS that the sample never saw (a monotone bucket order is all that keeps the output sorted), a cluster
 * straddling zero with zeros of both signs, infinities and a NaN, clusters so close that two boundaries share a cell (the grid must
 * decline), and 4 to 48 modes. Every row at 4 and 14 threads, shipped and LOGOS_NO_MDGRID=1: ascending under < (rows without a
 * NaN) and the input's bitwise multiset; where the row says the grid must carry the scatter, logos_twist_last(18) must say it did
 * at 14 threads, and never with the switch set. ---- */
enum { MG_C4, MG_C16, MG_C32, MG_GULF, MG_ZERO, MG_SPECIAL, MG_CLOSE, MG_UNEVEN, MG_BINAN, MG_N };
static const char *MG_NAME[MG_N] = { "4 clusters", "16 clusters", "32 clusters", "16 clusters + values in the gulfs", "16 clusters, one straddling zero",
                                     "16 clusters + infinities and a NaN", "16 clusters, two nearly touching", "12 clusters of uneven weight",
                                     "bimodal + one NaN (the per-mode map's compares: the committed engine segfaulted here)" };
static const int MG_EXPECT[MG_N] = { 0, 1, 1, 1, 1, 0, 0, 1, 0 };   /* 4 clusters: the compares (MD_LIN_MAX) */   /* 1: the grid must carry the scatter at 14 threads */
static void mg_gen(double *a, size_t n, int mode) {
    int k = mode == MG_C4 ? 4 : mode == MG_C32 ? 32 : mode == MG_UNEVEN ? 12 : mode == MG_BINAN ? 2 : 16;
    for (size_t i = 0; i < n; i++) {
        uint64_t r = nextr(); int c = (int)(r % (uint64_t)k);
        if (mode == MG_UNEVEN) { uint64_t u = nextr() % 100; c = u < 40 ? 0 : u < 60 ? 1 : (int)(2 + u % 10); }
        double base = (double)c * 1e8;
        if (mode == MG_ZERO) base -= 5e4 + 3e8;                     /* cluster 3 spans [-5e4, 5e4) */
        if (mode == MG_CLOSE && c == 9) base = 8e8 + 1.02e5;         /* cluster 9 starts just past cluster 8's end */
        a[i] = base + frand() * 1e5;
    }
    if (mode == MG_GULF) for (size_t i = 0; i < n; i += 997) a[i] = (double)(nextr() % 16) * 1e8 + 5e7 + frand() * 1e6;   /* mid-gulf */
    if (mode == MG_ZERO) for (size_t i = 11; i < n; i += 4099) a[i] = (i & 1) ? -0.0 : 0.0;
    if (mode == MG_SPECIAL) { a[n / 3] = INFINITY; a[n / 2] = -INFINITY; a[n - 9] = NAN; }
    if (mode == MG_BINAN) { for (size_t i = 0; i < n; i++) a[i] = (a[i] >= 1e8 ? 1e9 : -1e9) + frand() * 1e6; a[n / 2 + 3] = NAN; }
}
static int mdgrid_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const size_t sizes[] = { 1000003, 3000017 };
    const int threads[] = { 4, 14 };
    for (int mode = 0; mode < MG_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(18100 + (uint64_t)mode * 41 + si);
        mg_gen(orig, n, mode);
        int has_nan = 0; for (size_t i = 0; i < n; i++) if (orig[i] != orig[i]) has_nan = 1;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_MDGRID", "1", 1);
            (void)logos_twist_last(18);
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            size_t used = logos_twist_last(18);
            if (off) unsetenv("LOGOS_NO_MDGRID");
            int ok = 1;
            if (!has_nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  mode grid, %s, n=%zu threads=%d%s: wrong output\n", MG_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_MDGRID=1" : ""); }
            if (off) { checks++; asserted++; if (used) { fails++; printf("  FAIL  mode grid, %s, n=%zu threads=%d: the grid ran with LOGOS_NO_MDGRID=1\n", MG_NAME[mode], n, threads[ti]); } }
            else if (MG_EXPECT[mode] && threads[ti] == 14) { checks++; asserted++; if (!used) { fails++; printf("  FAIL  mode grid, %s, n=%zu threads=14: the scatter did not use the grid\n", MG_NAME[mode], n); } }
        }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  mode grid: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the prescan in parallel (2026-10-07, prescan_par): from 1,048,576 elements a sort with threads scans a 64K head and then
 * the rest in up to eight chunks. Every row: logos_prescan_agree (the scalar line against every block kind AND the parallel scan
 * at 2, 4, 8 and 16 threads, first_desc included), then a 4-thread sort checked ascending (rows without a NaN) and for the
 * bitwise multiset. The swap or NaN goes where a chunked scan could miss it: the first elements, both sides of the head's end,
 * both sides of every chunk boundary for 2 to 8 chunks, the last elements. ---- */
static int parprescan_checks(void) {
    int checks = 0, fails = 0;
    const size_t sizes[] = { 1048579, 2000003 };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *b = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!b || !a || !bi || !bo) { free(b); free(a); free(bi); free(bo); continue; }
        size_t pos[200]; int np = 0;
        for (size_t p = 1; p <= 3; p++) pos[np++] = p;
        for (size_t p = 65534; p <= 65538; p++) pos[np++] = p;
        for (int T = 2; T <= 8; T++) { size_t per = (n - 65536 + (size_t)T - 1) / (size_t)T;
            for (int t = 1; t < T; t++) for (int dlt = -1; dlt <= 1; dlt++) pos[np++] = 65536 + (size_t)t * per + (size_t)(long)dlt; }
        for (size_t p = n - 3; p < n; p++) pos[np++] = p;
        for (int base = 0; base < 4; base++) for (int pi = -1; pi < np; pi++) for (int kind = 0; kind < 2; kind++) {
            if (pi < 0 && kind) continue;
            for (size_t i = 0; i < n; i++) b[i] = base == 0 ? (double)i * 0.5 - 7.0 : base == 1 ? (double)(n - i) : base == 2 ? (double)(i / 3) : 3.0;
            if (pi >= 0) { size_t p = pos[pi];
                if (kind == 0) { double x = b[p - 1]; b[p - 1] = b[p]; b[p] = x; if (base == 3) b[p] = 4.0; } else b[p] = NAN; }
            checks++; if (!logos_prescan_agree(b, n)) { fails++; printf("  FAIL  parallel prescan, n=%zu base %d %s at %zu: disagrees with the scalar line\n", n, base, kind ? "NaN" : "swap", pi >= 0 ? pos[pi] : 0); }
            if (pi >= 0 && (pi % 5)) continue;                         /* every fifth row also sorted, at four threads */
            memcpy(a, b, n * sizeof(double));
            memcpy(bi, b, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
            logos_sort_f64(a, n, 4);
            int ok = 1;
            if (!(pi >= 0 && kind)) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  parallel prescan, n=%zu base %d %s at %zu: wrong output at 4 threads\n", n, base, kind ? "NaN" : "swap", pi >= 0 ? pos[pi] : 0); }
        }
        free(b); free(a); free(bi); free(bo);
    }
    printf("  parallel prescan: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- a handful of values counted in registers (2026-10-07, lc_vec_count): the low-cardinality route compares each vector with the
 * sample's distinct BIT PATTERNS and stops at the first vector holding anything else, where the scalar hash loop takes over. Rows aim at
 * what an integer compare could get wrong or a hand-off could drop: -0.0 and +0.0 as two of the values, a value the sample cannot have
 * seen (three copies, late), a NaN after a long clean stretch, values on a block boundary of eight, and plain 2/5/8 distinct. Every row
 * at 1 and 4 threads, shipped and LOGOS_NO_LCVEC=1: ascending under < (rows without a NaN), the input's bitwise multiset, and the
 * vector count must have settled elements where the row says it can (logos_twist_last(17)), never with the switch set. ---- */
enum { LV_TWO, LV_FIVE, LV_EIGHT, LV_ZEROS, LV_LATE, LV_NAN, LV_INF, LV_N };
static const char *LV_NAME[LV_N] = { "2 values", "5 values", "8 values", "-0.0, +0.0 and 1.5", "5 values + one the sample missed, late", "5 values + a NaN, late", "+-inf and 3 values" };
static void lv_gen(double *a, size_t n, int mode) {
    static const double V5[5] = { -3.25, 0.5, 7.0, 1e6, 2.5e-3 }, V8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (size_t i = 0; i < n; i++) {
        uint64_t r = nextr();
        if (mode == LV_TWO) a[i] = (r & 1) ? 1e6 + 0.5 : -0.25;
        else if (mode == LV_EIGHT) a[i] = V8[r % 8];
        else if (mode == LV_ZEROS) { int k = (int)(r % 3); a[i] = k == 0 ? -0.0 : k == 1 ? 0.0 : 1.5; }
        else if (mode == LV_INF) { int k = (int)(r % 5); a[i] = k == 0 ? INFINITY : k == 1 ? -INFINITY : (double)k; }
        else a[i] = V5[r % 5];
    }
    if (mode == LV_LATE) { a[n - 3] = 42.0; a[n - 100] = 42.0; a[n / 2 + 1] = 42.0; }
    if (mode == LV_NAN) a[n - 7] = NAN;
}
static int lcvec_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (!logos_twist_last(-5)) { printf("  low-cardinality vector count: not in this build or CPU, skipped\n"); return 0; }
    const size_t sizes[] = { 100003, 1000003 };
    const int threads[] = { 1, 4 };
    for (int mode = 0; mode < LV_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(17100 + (uint64_t)mode * 37 + si);
        lv_gen(orig, n, mode);
        int has_nan = 0; for (size_t i = 0; i < n; i++) if (orig[i] != orig[i]) has_nan = 1;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_LCVEC", "1", 1);
            (void)logos_twist_last(17);
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            size_t settled = logos_twist_last(17);
            if (off) unsetenv("LOGOS_NO_LCVEC");
            int ok = 1;
            if (!has_nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  lc vector count, %s, n=%zu threads=%d%s: wrong output\n", LV_NAME[mode], n, threads[ti], off ? ", LOGOS_NO_LCVEC=1" : ""); }
            checks++; asserted++;
            if (off && settled) { fails++; printf("  FAIL  lc vector count, %s, n=%zu threads=%d: %zu elements counted with LOGOS_NO_LCVEC=1\n", LV_NAME[mode], n, threads[ti], settled); }
            if (!off && settled < n / 4) { fails++; printf("  FAIL  lc vector count, %s, n=%zu threads=%d: only %zu elements counted by vector\n", LV_NAME[mode], n, threads[ti], settled); }
        }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  low-cardinality vector count: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the profile pass on the kernel's first partition (2026-10-05, lever C; frontprobe.cpp): a sequential sort that ends in the
 * AVX2 kernel reads the NaN / -0.0 scan off its first partition instead of a pass of its own, so a NaN is found AFTER one
 * partition has moved data and the decline hands a PERMUTED array to the scalar leaf. Rows reach the fused path through sort_fn
 * (uniform) and through leaf_f64's skewed branch (heavy tail); the NaN rows decline there; the zero rows take the signed-zero
 * repair; the dominant rows reach the pivot-is-the-minimum and right-part-all-equal exits. Every row at the three switch
 * settings: shipped (the fused path must have run where the row says so, and a NaN row must have declined), LOGOS_NO_PARTFUSE=1
 * and LOGOS_NO_FRONTFUSE=1 (it must never run). Output: ascending under < (rows without a NaN) and the input's bitwise multiset
 * (every row). Skipped, and said so, on a build or CPU without the AVX2 leaf (logos_twist_last(-5)). ---- */
enum { FU_UNIFORM, FU_HEAVY, FU_NAN_MID, FU_NAN_HEAVY, FU_NAN_FIRST, FU_ZEROS, FU_HEAVY_ZEROS, FU_ALL_ZERO, FU_MIN_DUP, FU_MAX_DUP, FU_INF, FU_N };
static const char *FU_NAME[FU_N] = { "uniform", "heavy tail", "uniform + one NaN", "heavy tail + one NaN", "NaN at a[0]", "uniform + zeros of both signs",
                                     "heavy tail + zeros of both signs", "all zeros, both signs", "90% equal to the minimum", "90% equal to the maximum", "uniform + infinities" };
/* 1 = the fused path must run (shipped); 2 = it must run AND decline on the NaN; 0 = not asserted */
static const int FU_EXPECT[FU_N] = { 1, 1, 2, 2, 0, 1, 1, 0, 0, 0, 1 };
static void fuse_gen(double *a, size_t n, int mode) {
    for (size_t i = 0; i < n; i++) {
        double u = frand();
        if (mode == FU_HEAVY || mode == FU_NAN_HEAVY || mode == FU_HEAVY_ZEROS) { if (u < 1e-12) u = 1e-12; a[i] = pow(u, -1.0 / 1.2); }
        else a[i] = u * 2e9 - 1e9;
    }
    if (mode == FU_NAN_MID || mode == FU_NAN_HEAVY) a[n / 3 + 17] = NAN;
    if (mode == FU_NAN_FIRST) a[0] = NAN;
    if (mode == FU_ZEROS || mode == FU_HEAVY_ZEROS) for (size_t i = 5; i < n; i += 61) a[i] = (i & 1) ? -0.0 : 0.0;
    if (mode == FU_ALL_ZERO) for (size_t i = 0; i < n; i++) a[i] = (nextr() & 1) ? -0.0 : 0.0;
    if (mode == FU_MIN_DUP) for (size_t i = 0; i < n; i++) if (nextr() % 10) a[i] = -2e9;
    if (mode == FU_MAX_DUP) for (size_t i = 0; i < n; i++) if (nextr() % 10) a[i] = 2e9;
    if (mode == FU_INF) for (size_t i = 3; i < n; i += 1001) a[i] = (i & 1) ? INFINITY : -INFINITY;
}
static int fuse_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    if (!logos_twist_last(-5)) { printf("  first-partition scan: not in this build or CPU, skipped\n"); return 0; }
    const size_t sizes[] = { 70001, 300001, 1000003 };
    const int threads[] = { 1, 4 };
    static const char *SW[] = { NULL, "LOGOS_NO_PARTFUSE", "LOGOS_NO_FRONTFUSE" };
    for (int mode = 0; mode < FU_N; mode++) for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(15100 + (uint64_t)mode * 31 + si);
        fuse_gen(orig, n, mode);
        int has_nan = 0; for (size_t i = 0; i < n; i++) if (orig[i] != orig[i]) has_nan = 1;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) for (int sw = 0; sw < 3; sw++) {
            if (SW[sw]) setenv(SW[sw], "1", 1);
            (void)logos_twist_last(15); (void)logos_twist_last(16);
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            size_t fused = logos_twist_last(15), declined = logos_twist_last(16);
            if (SW[sw]) unsetenv(SW[sw]);
            const char *setting = SW[sw] ? SW[sw] : "shipped";
            int ok = 1;
            if (!has_nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            if (memcmp(bi, bo, n * sizeof(uint64_t))) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  first-partition scan, %s, n=%zu threads=%d, %s: wrong output\n", FU_NAME[mode], n, threads[ti], setting); }
            if (threads[ti] != 1) continue;
            if (SW[sw]) { checks++; asserted++; if (fused) { fails++; printf("  FAIL  first-partition scan, %s, n=%zu, %s: the fused path ran %zu times\n", FU_NAME[mode], n, setting, fused); } }
            else if (FU_EXPECT[mode]) {
                checks++; asserted++;
                if (!fused) { fails++; printf("  FAIL  first-partition scan, %s, n=%zu: the fused path never ran\n", FU_NAME[mode], n); }
                checks++; asserted++;
                if ((FU_EXPECT[mode] == 2) != (declined > 0)) { fails++; printf("  FAIL  first-partition scan, %s, n=%zu: %zu NaN declines (expected %s)\n", FU_NAME[mode], n, declined, FU_EXPECT[mode] == 2 ? "one" : "none"); }
            }
        }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  first-partition scan: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the thin front (2026-10-08, thin_sort_f64): a call of at most THIN_MAX_N elements (128, then 2047 from 2026-10-20) is answered before the dispatcher. Sizes run from 2
 * to 160, past the gate on both sides, through all four of its exits (ascending, descending, network, insertion sort, and the AVX2
 * quicksort with its NaN decline). Every input is a permutation of the input BITS on the way out, ascending when NaN-free; and every
 * row says which side of the gate it took (logos_twist_last(20) counts the calls the thin front answered). ---- */
static int thin_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const int threads[] = { 1, 8 };
    const int built = logos_twist_last(-6) != 0;     /* -DLOGOS_NO_THINFRONT: every call must reach the dispatcher instead */
    const size_t tmax = (size_t)logos_twist_last(-13);   /* the thin front's ceiling as compiled (2047 since 2026-10-20; 0 without it) */
    enum { TM = 15 };
    size_t sizes[96]; int nsz = 0;
    for (size_t n = 2; n <= 40; n++) sizes[nsz++] = n;
    for (size_t n = 47; n <= 160; n += 7) sizes[nsz++] = n;
    { const size_t more[] = { 255, 256, 257, 511, 1024, 1500 }; for (int k = 0; k < 6; k++) sizes[nsz++] = more[k]; }
    if (tmax > 2) { sizes[nsz++] = tmax - 1; sizes[nsz++] = tmax; sizes[nsz++] = tmax + 1; sizes[nsz++] = tmax + 40; }
    size_t cap = 0; for (int k = 0; k < nsz; k++) if (sizes[k] > cap) cap = sizes[k];
    double *orig = malloc(cap * sizeof(double)), *a = malloc(cap * sizeof(double));
    uint64_t *bi = malloc(cap * sizeof(uint64_t)), *bo = malloc(cap * sizeof(uint64_t));
    if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); printf("  thin front: no memory, skipped\n"); return 0; }
    for (int si = 0; si < nsz; si++) for (int mix = 0; mix < TM; mix++) for (size_t ti = 0; ti < 2; ti++) {
        size_t n = sizes[si];
        seed(9700 + (uint64_t)n * 37 + (uint64_t)mix);
        for (size_t i = 0; i < n; i++) orig[i] = frand() * 2e9 - 1e9;
        switch (mix) {
        case 0: break;                                                                       /* random */
        case 1: for (size_t i = 0; i < n; i++) orig[i] = (double)i + 0.5; break;              /* sorted */
        case 2: for (size_t i = 0; i < n; i++) orig[i] = (double)(n - i) + 0.5; break;        /* reversed */
        case 3: for (size_t i = 0; i < n; i++) orig[i] = (double)(i % 3); break;              /* three values */
        case 4: for (size_t i = 0; i < n; i++) orig[i] = (double)((n - i) / 4); break;        /* descending with ties */
        case 5: orig[0] = NAN; break;                                                        /* a NaN first */
        case 6: orig[n / 2] = NAN; orig[n - 1] = NAN; break;                                  /* two NaNs, one last */
        case 7: for (size_t i = 0; i < n; i++) orig[i] = (double)i; orig[n / 2] = NAN; break;  /* sorted around a NaN */
        case 8: for (size_t i = 0; i < n; i++) orig[i] = (i & 1) ? 0.0 : -0.0; break;          /* signed zeros only */
        case 9: for (size_t i = 0; i < n; i += 3) orig[i] = (i & 2) ? 0.0 : -0.0; break;       /* signed zeros among values */
        case 10: orig[0] = INFINITY; orig[n - 1] = -INFINITY; if (n > 2) orig[1] = 4.9e-324; break;
        case 11: for (size_t i = 0; i < n; i++) orig[i] = 7.0; if (n > 3) orig[n - 2] = 6.0; break;   /* all equal but one */
        case 12: for (size_t i = 0; i < n; i++) orig[i] = (double)i + 0.5;                    /* KINKS: the thin front's insertion path */
                 for (size_t i = 1; i + 1 < n; i += 17) { double t = orig[i]; orig[i] = orig[i + 1]; orig[i + 1] = t; } break;
        case 13: for (size_t i = 0; i < n; i++) orig[i] = (double)i + 0.5;                    /* one FAR swap: must not look like kinks */
                 if (n > 4) { double t = orig[1]; orig[1] = orig[n - 2]; orig[n - 2] = t; } break;
        case 14: for (size_t i = 0; i < n; i++) orig[i] = (double)(i / 3);                    /* kinks among ties, signed zeros at the front */
                 for (size_t i = 2; i + 1 < n; i += 11) { double t = orig[i]; orig[i] = orig[i + 1]; orig[i + 1] = t; }
                 if (n > 3) { orig[0] = -0.0; orig[1] = 0.0; orig[2] = -0.0; } break;
        }
        size_t nnan = 0; for (size_t i = 0; i < n; i++) nnan += orig[i] != orig[i];
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        memcpy(a, orig, n * sizeof(double));
        (void)logos_twist_last(20);
        logos_sort_f64(a, n, threads[ti]);
        size_t thin = logos_twist_last(20);
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        int ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;
        if (!nnan) for (size_t i = 1; ok && i < n; i++) if (a[i] < a[i - 1]) ok = 0;
        checks++; if (!ok) { fails++; printf("  FAIL  thin front n=%zu mix=%d threads=%d: wrong output\n", n, mix, threads[ti]); }
        checks++; asserted++;
        if ((built && n <= tmax) != (thin == 1)) { fails++; printf("  FAIL  thin front n=%zu mix=%d: %zu thin calls (expected %d)\n", n, mix, thin, built && n <= tmax); }
    }
    free(orig); free(a); free(bi); free(bo);
    printf("  thin front: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the batch entry (2026-10-08, logos_sort_f64_batch): many arrays end to end. Layouts: empty and one-element segments among
 * others, every length on both sides of the thin front (128) and of the leaf's limit (65,536), segments that are sorted, reversed,
 * five-valued, holding NaNs, signed zeros and infinities; at 1, 2 and 8 threads. Every segment must be a permutation of its own
 * input BITS (nothing crosses a boundary) and ascending when it holds no NaN. ---- */
static int batch_checks(void) {
    int checks = 0, fails = 0;
    const size_t cap = 1500000;
    double *orig = malloc(cap * sizeof(double)), *a = malloc(cap * sizeof(double));
    uint64_t *bi = malloc(cap * sizeof(uint64_t)), *bo = malloc(cap * sizeof(uint64_t));
    size_t *off = malloc(20001 * sizeof(size_t));
    if (!orig || !a || !bi || !bo || !off) { free(orig); free(a); free(bi); free(bo); free(off); printf("  batch: skipped (no memory)\n"); return 0; }
    static const size_t edges[] = { 0, 1, 2, 3, 16, 17, 24, 25, 127, 128, 129, 2047, 2048, 65535, 65536, 65537, 300000 };
    const int threads[] = { 1, 2, 8 };
    for (int lay = 0; lay < 4; lay++) {
        seed(19700 + (uint64_t)lay);
        size_t nseg = 0, o = 0; off[0] = 0;
        while (nseg < 20000) {
            size_t L;
            if (lay == 0) L = edges[nseg % (sizeof edges / sizeof *edges)];        /* every edge, in turn */
            else if (lay == 1) L = nextr() % 40;                                    /* many tiny ones, empty ones among them */
            else if (lay == 2) L = (size_t)exp(frand() * log(150000.0));            /* log-uniform to 150,000 */
            else L = 100 + nextr() % 3000;                                         /* the leaf's range */
            if (o + L > cap) break;
            o += L; off[++nseg] = o;
        }
        for (size_t k = 0; k < nseg; k++) {
            double *s = orig + off[k]; size_t L = off[k + 1] - off[k];
            int mix = (int)(nextr() % 8);
            for (size_t i = 0; i < L; i++) {
                switch (mix) {
                case 1: s[i] = (double)i; break;
                case 2: s[i] = (double)(L - i); break;
                case 3: s[i] = (double)(nextr() % 5); break;
                case 4: s[i] = (i & 1) ? 0.0 : -0.0; break;
                default: s[i] = frand() * 2e6 - 1e6; break;
                }
            }
            if (mix == 5 && L) s[nextr() % L] = NAN;
            if (mix == 6 && L > 2) { s[0] = INFINITY; s[L - 1] = -INFINITY; s[L / 2] = 4.9e-324; }
            if (mix == 7 && L > 1) s[nextr() % L] = -0.0;
        }
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            memcpy(a, orig, o * sizeof(double));
            logos_sort_f64_batch(a, off, nseg, threads[ti]);
            int bad = 0;
            for (size_t k = 0; k < nseg && !bad; k++) {
                size_t s0 = off[k], L = off[k + 1] - off[k];
                if (!L) continue;
                memcpy(bi, orig + s0, L * sizeof(double)); qsort(bi, L, sizeof(uint64_t), cmp_bits);
                memcpy(bo, a + s0, L * sizeof(double)); qsort(bo, L, sizeof(uint64_t), cmp_bits);
                if (memcmp(bi, bo, L * sizeof(uint64_t))) bad = 1;
                size_t nn = 0; for (size_t i = 0; i < L; i++) nn += orig[s0 + i] != orig[s0 + i];
                if (!nn) for (size_t i = 1; i < L; i++) if (a[s0 + i] < a[s0 + i - 1]) { bad = 2; break; }
                if (bad) printf("  FAIL  batch, layout %d, threads=%d, segment %zu (%zu elements): %s\n", lay, threads[ti], k, L, bad == 1 ? "not a permutation of its input" : "not ascending");
            }
            checks++; if (bad) fails++;
        }
    }
    /* degenerate calls: no segments, a NULL array with no segments, one segment holding everything */
    logos_sort_f64_batch(NULL, off, 0, 4); checks++;
    seed(19790); for (size_t i = 0; i < 300000; i++) orig[i] = frand();
    memcpy(a, orig, 300000 * sizeof(double)); off[0] = 0; off[1] = 300000;
    logos_sort_f64_batch(a, off, 1, 4);
    { int ok = 1; for (size_t i = 1; i < 300000; i++) if (a[i] < a[i - 1]) ok = 0; checks++; if (!ok) { fails++; printf("  FAIL  batch, one segment of 300000\n"); } }
    free(orig); free(a); free(bi); free(bo); free(off);
    printf("  batch: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the float32 path, the vector lattice map and int32 values through the AVX2 kernel (2026-10-08). Float32: doubles that are all
 * exactly floats (with signed zeros, infinities, float subnormals, ties), and the inputs that must DECLINE it after its pass has
 * begun (a NaN, one double that is not a float, both placed late); logos_twist_last(22) must count exactly the rows that are all
 * floats, at one thread. Lattice: lattices with negative values, a value the sample misses, an off-lattice value and a NaN placed
 * late, at 1 and 8 threads, with the vector map on and off: the same bytes both ways, and logos_twist_last(23) > 0 where the map
 * ran in vectors. Int32: int32-valued doubles at one thread (the radix; the AVX2 kernel for them measured slower and stays behind
 * -DLOGOS_AVX2_INT32, so the LOGOS_NO_AVX2_INT32 setting these rows toggle is now inert: both settings must still be right). Every output a permutation of the
 * input bits and ascending when NaN-free. ---- */
static int f32lat_check(const double *a, const uint64_t *bi, uint64_t *bo, size_t n, int nan_free) {
    memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
    if (memcmp(bi, bo, n * sizeof(uint64_t))) return 0;
    if (nan_free) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0;
    return 1;
}
static int f32lat_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    const size_t NMAX = 300007;
    double *orig = malloc(NMAX * sizeof(double)), *a = malloc(NMAX * sizeof(double)), *b = malloc(NMAX * sizeof(double));
    uint64_t *bi = malloc(NMAX * sizeof(uint64_t)), *bo = malloc(NMAX * sizeof(uint64_t));
    if (!orig || !a || !b || !bi || !bo) { free(orig); free(a); free(b); free(bi); free(bo); printf("  float32 / lattice: skipped (no memory)\n"); return 0; }
    /* float32 */
    static const size_t fns[] = { 65536, 100003, 300007 };
    for (size_t ni = 0; ni < 3; ni++) for (int mode = 0; mode < 9; mode++) {
        size_t n = fns[ni];
        seed(19800 + (uint64_t)mode * 7 + ni);
        for (size_t i = 0; i < n; i++) orig[i] = (double)(float)(frand() * 2e6 - 1e6);
        int expect = 1, nan = 0;
        switch (mode) {
        case 1: for (size_t i = 0; i < n; i += 5) orig[i] = (i & 1) ? 0.0 : -0.0; break;                 /* signed zeros */
        case 2: orig[3] = INFINITY; orig[n - 2] = -INFINITY; orig[n / 2] = (double)1.4e-45f; orig[7] = (double)-1.17549435e-38f; break;
        case 3: for (size_t i = 0; i < n; i++) orig[i] = (double)(float)(nextr() % 3) - 1.0; break;      /* ties: three values */
        case 4: orig[n - 3] = NAN; expect = 0; nan = 1; break;                                           /* declines late, a NaN */
        case 5: orig[n - 3] = 0.1; expect = 0; break;                                                    /* declines late: 0.1 is no float */
        case 6: for (size_t i = 0; i < n; i++) orig[i] = (double)(float)(1.0 + (double)i * 1e-7); break; /* already sorted: the prescan */
        case 7: for (size_t i = 0; i < n; i++) orig[i] = (double)(float)ldexp(frand(), (int)(nextr() % 200) - 100); break;   /* wide exponents */
        case 8: for (size_t i = 0; i < n; i++) orig[i] = 3.5; orig[n / 3] = 3.25; break;                  /* two values */
        }
        if (mode == 3 || mode == 6 || mode == 8) expect = -1;                       /* other routes may take these first: path not asserted */
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (int T = 1; T <= 8; T += 7) {
            memcpy(a, orig, n * sizeof(double));
            (void)logos_twist_last(22);
            logos_sort_f64(a, n, T);
            size_t fired = logos_twist_last(22);
            checks++; if (!f32lat_check(a, bi, bo, n, !nan)) { fails++; printf("  FAIL  float32 mode %d n=%zu threads=%d: wrong output\n", mode, n, T); }
            if (T == 1 && expect >= 0) {
                checks++; asserted++;
                if ((fired > 0) != (expect == 1 && logos_twist_last(-8))) { fails++; printf("  FAIL  float32 mode %d n=%zu: the path fired %zu times, expected %s\n", mode, n, fired, expect ? "once" : "never"); }
            }
        }
    }
    /* the lattice map, vectors on and off */
    for (int mode = 0; mode < 6; mode++) for (int T = 1; T <= 8; T += 7) {
        size_t n = 300007;
        seed(19850 + (uint64_t)mode);
        for (size_t i = 0; i < n; i++) {
            switch (mode) {
            case 0: orig[i] = (double)(nextr() % 4096) / 64.0; break;                    /* quantized */
            case 1: orig[i] = (double)(nextr() % 256) * 1000.0 + 0.5; break;             /* 256 distinct */
            case 2: orig[i] = (double)((int64_t)(nextr() % 4000) - 2000) * 0.25; break;   /* negative values */
            default: orig[i] = (double)(nextr() % 4096) / 64.0; break;
            }
        }
        int nan = 0;
        if (mode == 3) orig[n - 5] = 4097.0 / 64.0;                 /* on the lattice, outside anything the sample saw */
        if (mode == 4) orig[n - 5] = 1.0 / 3.0;                     /* off the lattice, late: the map declines */
        if (mode == 5) { orig[n - 5] = NAN; nan = 1; }              /* a NaN, late */
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_LATVEC", "1", 1); else unsetenv("LOGOS_NO_LATVEC");
            double *out = off ? b : a;
            memcpy(out, orig, n * sizeof(double));
            (void)logos_twist_last(23);
            logos_sort_f64(out, n, T);
            size_t vecs = logos_twist_last(23);
            checks++; if (!f32lat_check(out, bi, bo, n, !nan)) { fails++; printf("  FAIL  lattice map mode %d threads=%d%s: wrong output\n", mode, T, off ? " LOGOS_NO_LATVEC=1" : ""); }
            if (off) { checks++; asserted++; if (vecs) { fails++; printf("  FAIL  lattice map mode %d threads=%d: %zu vectors with LOGOS_NO_LATVEC=1\n", mode, T, vecs); } }
            else if (mode <= 3 && logos_twist_last(-7)) { checks++; asserted++; if (!vecs) { fails++; printf("  FAIL  lattice map mode %d threads=%d: the vector map never ran\n", mode, T); } }
        }
        unsetenv("LOGOS_NO_LATVEC");
        if (!nan) { checks++; if (memcmp(a, b, n * sizeof(double))) { fails++; printf("  FAIL  lattice map mode %d threads=%d: vectors and scalar disagree\n", mode, T); } }
    }
    /* int32-valued doubles at one thread: the AVX2 kernel against the radix */
    static const size_t ins[] = { 4096, 65536, 300007 };
    for (size_t ni = 0; ni < 3; ni++) for (int mode = 0; mode < 3; mode++) {
        size_t n = ins[ni];
        seed(19870 + (uint64_t)mode + ni * 3);
        for (size_t i = 0; i < n; i++) orig[i] = mode == 0 ? (double)(int32_t)(uint32_t)nextr() : mode == 1 ? (double)(int32_t)(nextr() % 16777216) - 8388608.0 : (double)(int32_t)(uint32_t)nextr();
        if (mode == 2) { orig[0] = -2147483648.0; orig[n - 1] = 2147483647.0; orig[n / 2] = -0.0; }
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_AVX2_INT32", "1", 1); else unsetenv("LOGOS_NO_AVX2_INT32");
            double *out = off ? b : a;
            memcpy(out, orig, n * sizeof(double)); logos_sort_f64(out, n, 1);
            checks++; if (!f32lat_check(out, bi, bo, n, 1)) { fails++; printf("  FAIL  int32 kernel mode %d n=%zu%s: wrong output\n", mode, n, off ? " LOGOS_NO_AVX2_INT32=1" : ""); }
        }
        unsetenv("LOGOS_NO_AVX2_INT32");
    }
    free(orig); free(a); free(b); free(bi); free(bo);
    printf("  float32 / lattice / int32 kernel: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the in-place partition (2026-10-08, ipp_partition, opt-in LOGOS_PART_INPLACE=1): shapes that reach the parallel partition
 * (uniform, heavy tail, two clusters, sixteen clusters, integers over a wide range, one heavy duplicate, signed zeros, a NaN, an
 * infinity at each end), sizes that are not a multiple of the 256-element block and a few just past one, at 2, 3, 8 and 14
 * threads. Every output a permutation of the input bits, ascending when NaN-free; logos_twist_last(24) must count the partitions
 * done in place where the switch is on and the shape is one the partition takes (NaN-free shapes), and none where it is off. ---- */
static int ipp_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    static const size_t ns[] = { 300007, 1000003, 2100001, 409600 + 1 };
    static const int ths[] = { 2, 3, 8, 14 };
    const size_t NMAX = 2100001;
    double *orig = malloc(NMAX * sizeof(double)), *a = malloc(NMAX * sizeof(double));
    uint64_t *bi = malloc(NMAX * sizeof(uint64_t)), *bo = malloc(NMAX * sizeof(uint64_t));
    if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); printf("  in-place partition: skipped (no memory)\n"); return 0; }
    enum { IU, IH, IB, IC, II, ID, IZ, IN, IF, IM };
    static const char *nm[IM] = { "uniform", "heavy_tail", "bimodal", "clusters16", "int_wide", "dup20pct", "negzero", "nan", "infinities" };
    for (size_t ni = 0; ni < sizeof ns / sizeof *ns; ni++) for (int mode = 0; mode < IM; mode++) {
        size_t n = ns[ni];
        seed(19950 + (uint64_t)mode * 31 + ni);
        for (size_t i = 0; i < n; i++) {
            double u = frand();
            switch (mode) {
            case IH: { double v = u < 1e-12 ? 1e-12 : u; orig[i] = pow(v, -1.0 / 1.2); } break;
            case IB: orig[i] = ((nextr() & 1) ? 1e9 : -1e9) + frand() * 1e6; break;
            case IC: orig[i] = (double)(nextr() % 16) * 1e8 + frand() * 1e5; break;
            case II: orig[i] = (double)(int32_t)(uint32_t)nextr(); break;
            case ID: orig[i] = (nextr() % 5 == 0) ? 42.0 : u * 2e6 - 1e6; break;
            case IZ: orig[i] = (nextr() % 50 == 0) ? ((i & 1) ? 0.0 : -0.0) : u * 2e6 - 1e6; break;
            default: orig[i] = u * 2e9 - 1e9; break;
            }
        }
        if (mode == IN) orig[n / 3] = NAN;
        if (mode == IF) { orig[n / 5] = INFINITY; orig[n / 2] = -INFINITY; }
        int nan = mode == IN;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof ths / sizeof *ths; ti++) for (int on = 0; on < 2; on++) {
            if (on) setenv("LOGOS_PART_INPLACE", "1", 1); else unsetenv("LOGOS_PART_INPLACE");
            memcpy(a, orig, n * sizeof(double));
            (void)logos_twist_last(24);
            logos_sort_f64(a, n, ths[ti]);
            size_t ip = logos_twist_last(24);
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            int ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;
            if (ok && !nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            checks++; if (!ok) { fails++; printf("  FAIL  in-place partition, %s, n=%zu, threads=%d%s: wrong output\n", nm[mode], n, ths[ti], on ? ", in place" : ""); }
            if (!on) { checks++; asserted++; if (ip) { fails++; printf("  FAIL  in-place partition, %s, n=%zu, threads=%d: ran with the switch off\n", nm[mode], n, ths[ti]); } }
            else if ((mode == IU || mode == IH || mode == IB || mode == IC) && n >= 1000003 && ths[ti] >= 8) {
                checks++; asserted++;
                if (!ip) { fails++; printf("  FAIL  in-place partition, %s, n=%zu, threads=%d: the partition did not run in place\n", nm[mode], n, ths[ti]); }
            }
        }
        unsetenv("LOGOS_PART_INPLACE");
    }
    free(orig); free(a); free(bi); free(bo);
    printf("  in-place partition: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- two halves (2026-10-19, halves_try_f64): at two threads, after every structured route has declined, one split around the
 * sample's median and two sequential leaves. Every output a permutation of the input bits and ascending under < when NaN-free;
 * rows that must take the route say so (logos_twist_last(26)), and rows that must not -- ordered data, a sawtooth whose period
 * aliases the sample's lag, repeats, three threads, the switch set -- say that. Sizes on both sides of the thread policy's
 * two-thread band (n / 100000 decides), threads requested 2, 3 and 14. */
static int halves_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    static const size_t ns[] = { 200003, 299999, 1000003, 2100001 };
    static const int ths[] = { 2, 3, 14 };
    const size_t NMAX = 2100001;
    double *orig = malloc(NMAX * sizeof(double)), *a = malloc(NMAX * sizeof(double));
    uint64_t *bi = malloc(NMAX * sizeof(uint64_t)), *bo = malloc(NMAX * sizeof(uint64_t));
    if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); printf("  two halves: skipped (no memory)\n"); return 0; }
    enum { HU, HH, HB, HC, HI, HZ, HN, HF, HD, HR, HS, HQ, HM };
    static const char *nm[HM] = { "uniform", "heavy_tail", "bimodal", "clusters16", "int_wide", "negzero", "nan", "infinities", "dup20pct",
                                  "noisy ring", "sawtooth at twice the lag", "quantized" };
    for (size_t ni = 0; ni < sizeof ns / sizeof *ns; ni++) for (int mode = 0; mode < HM; mode++) {
        size_t n = ns[ni];
        seed(26260 + (uint64_t)mode * 37 + ni);
        size_t L = (size_t)(0.0062 * (double)n); if (L < 2) L = 2;
        for (size_t i = 0; i < n; i++) {
            double u = frand();
            switch (mode) {
            case HH: { double v = u < 1e-12 ? 1e-12 : u; orig[i] = pow(v, -1.0 / 1.2); } break;
            case HB: orig[i] = ((nextr() & 1) ? 1e9 : -1e9) + frand() * 1e6; break;
            case HC: orig[i] = (double)(nextr() % 16) * 1e8 + frand() * 1e5; break;
            case HI: orig[i] = (double)(int32_t)(uint32_t)nextr(); break;
            case HZ: orig[i] = (nextr() % 50 == 0) ? ((i & 1) ? 0.0 : -0.0) : u * 2e6 - 1e6; break;
            case HD: orig[i] = (nextr() % 5 == 0) ? 42.0 : u * 2e6 - 1e6; break;
            case HR: orig[i] = (double)((i + n / 3) % n) + (u - 0.5) * 40.0; break;
            case HS: orig[i] = (double)(i % L) + (double)(i / L) * 1e-7; break;
            case HQ: orig[i] = (double)(nextr() % 30000) * 0.25; break;
            default: orig[i] = u * 2e9 - 1e9; break;
            }
        }
        if (mode == HN) orig[n / 3] = NAN;
        if (mode == HF) { orig[n / 5] = INFINITY; orig[n / 2] = -INFINITY; orig[n / 7] = INFINITY; }
        int nan = mode == HN;
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        for (size_t ti = 0; ti < sizeof ths / sizeof *ths; ti++) for (int off = 0; off < 2; off++) {
            if (off) setenv("LOGOS_NO_HALVES", "1", 1); else unsetenv("LOGOS_NO_HALVES");
            memcpy(a, orig, n * sizeof(double));
            (void)logos_twist_last(26);
            logos_sort_f64(a, n, ths[ti]);
            size_t hv = logos_twist_last(26);
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            int ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;
            if (ok && !nan) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            checks++; if (!ok) { fails++; printf("  FAIL  two halves, %s, n=%zu, threads=%d%s: wrong output\n", nm[mode], n, ths[ti], off ? ", switch set" : ""); }
            size_t pe = n / 100000; if (pe < 1) pe = 1; int P = (size_t)ths[ti] < pe ? ths[ti] : (int)pe;   /* effective_threads */
            int must = !off && P == 2 && mode <= HI, mustnot = off || P != 2 || mode >= HR;
            if (must || mustnot) {
                checks++; asserted++;
                if (must && !hv) { fails++; printf("  FAIL  two halves, %s, n=%zu, threads=%d: the route did not run\n", nm[mode], n, ths[ti]); }
                if (mustnot && hv) { fails++; printf("  FAIL  two halves, %s, n=%zu, threads=%d%s: the route ran\n", nm[mode], n, ths[ti], off ? " with the switch set" : ""); }
            }
        }
        unsetenv("LOGOS_NO_HALVES");
        if (mode == HU) {                     /* the lazy view's partition at two threads must hand back buckets, never take the route */
            size_t starts[LOGOS_MAX_VIEW_BUCKETS + 1]; double bmn[LOGOS_MAX_VIEW_BUCKETS], bmx[LOGOS_MAX_VIEW_BUCKETS];
            memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(26);
            int nbk = logos_partition_f64(a, n, 2, starts, bmn, bmx);
            size_t hv = logos_twist_last(26);
            checks++; asserted++;
            if (nbk <= 0 || hv) { fails++; printf("  FAIL  two halves, lazy view partition, n=%zu: %d buckets, route ran %zu times\n", n, nbk, hv); }
        }
    }
    free(orig); free(a); free(bi); free(bo);
    printf("  two halves: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the log-line route (2026-10-19, str_ns_try): one thread, near-ordered strings. Each row checks strcmp order and that the
 * output is a permutation of the input pointers, and names the verdict it must reach (logos_twist_last(-9)): lines arriving
 * mostly in order must be TAKEN (1), with late lines, ties in a millisecond, a poison line that is far too large early on (the
 * demotion must recover it), identical lines and empty strings; random lines must be DECLINED at the pre-gate (2); a strand whose
 * windows look ordered but whose whole array carries too many strays must GIVE UP in the pass (3) and still come out sorted;
 * with LOGOS_STR_NO_NS set, or at two threads, it must not run (0). */
static int ns_cmp_str(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }
static int ns_cmp_ptr(const void *x, const void *y) { uintptr_t a = (uintptr_t)*(const char *const *)x, b = (uintptr_t)*(const char *const *)y; return (a > b) - (a < b); }
static int strns_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    static const size_t ns[] = { 65536, 100003, 400009 };
    enum { LG, LP, LI, LE, LR, LX, LC, LM };
    static const char *nm[LM] = { "log lines", "log lines + a poison line", "log lines, many identical", "log lines + empty strings", "random lines", "ordered windows, strays between them", "eight sorted files concatenated" };
    for (size_t ni = 0; ni < sizeof ns / sizeof *ns; ni++) for (int mode = 0; mode < LM; mode++) for (int v = 0; v < 4; v++) {
        size_t n = ns[ni];
        seed(27270 + (uint64_t)mode * 41 + ni);
        char *pool = malloc(n * 40 + 64); const char **a = malloc(n * sizeof *a), **ref = malloc(n * sizeof *ref);
        if (!pool || !a || !ref) { free(pool); free(a); free(ref); printf("  log-line route: skipped (no memory)\n"); return 0; }
        size_t off = 0; double t = 0;
        for (size_t i = 0; i < n; i++) {
            char *p = pool + off; t += frand() * 2.0;
            double ts = t + (frand() < 0.05 ? -frand() * 500.0 : 0.0); if (ts < 0) ts = 0;
            if (mode == LR) ts = frand() * 1e7;
            if (mode == LC) ts = (double)(i % (n / 8 + 1)) * 3.0;
            if (mode == LX) { size_t blk = i / 64; ts = (blk % 2 == 0) ? (double)i : frand() * (double)n; }   /* ordered at the gate's windows only by luck */
            int len = snprintf(p, 40, "%012lld|h%02d|%c", (long long)ts, (int)(nextr() % 32), 'a' + (int)(nextr() % 26));
            if (mode == LI && (i % 3)) len = snprintf(p, 40, "%012lld|same", (long long)ts);
            if (mode == LE && i % 97 == 0) len = 0, p[0] = 0;
            if (mode == LP && i == 10) len = snprintf(p, 40, "zzzzzzzzzzzz|poison");
            a[i] = p; off += (size_t)len + 1;
        }
        memcpy(ref, a, n * sizeof *a);
        qsort(ref, n, sizeof *ref, ns_cmp_str);
        if (v == 3 && mode != LG) { free(pool); free(a); free(ref); continue; }   /* the forced give-up: on plain log lines only */
        if (v == 1) setenv("LOGOS_STR_NO_NS", "1", 1);
        if (v == 3) setenv("LOGOS_STR_NS_CAP", "0.01", 1);
        logos_sort_strings_mt(a, n, v == 2 ? 2 : 1);
        unsetenv("LOGOS_STR_NO_NS"); unsetenv("LOGOS_STR_NS_CAP");
        int verdict = (int)logos_twist_last(-9); (void)logos_twist_last(27);
        int ok = 1;
        for (size_t i = 0; i < n && ok; i++) if (strcmp(a[i], ref[i])) ok = 0;
        if (ok) { qsort(a, n, sizeof *a, ns_cmp_ptr); qsort(ref, n, sizeof *ref, ns_cmp_ptr); ok = memcmp(a, ref, n * sizeof *a) == 0; }
        checks++; if (!ok) { fails++; printf("  FAIL  log-line route, %s, n=%zu%s: wrong output\n", nm[mode], n, v == 1 ? ", switch set" : v == 2 ? ", 2 threads" : v == 3 ? ", forced give-up" : ""); }
        if (v == 3) { checks++; asserted++; if (verdict != 3) { fails++; printf("  FAIL  log-line route, %s, n=%zu: forced give-up read verdict %d\n", nm[mode], n, verdict); } free(pool); free(a); free(ref); continue; }
        int want = v ? 0 : mode == LR ? 2 : (mode == LX || mode == LC) ? -1 : 1;
        if (want >= 0) { checks++; asserted++;
            if (v == 0 && verdict != want) { fails++; printf("  FAIL  log-line route, %s, n=%zu: verdict %d, expected %d\n", nm[mode], n, verdict, want); }
            if (v != 0 && verdict == 1) { fails++; printf("  FAIL  log-line route, %s, n=%zu%s: the route ran\n", nm[mode], n, v == 1 ? " with the switch set" : " at 2 threads"); } }
        if ((mode == LX || mode == LC) && v == 0) { checks++; asserted++; if (verdict == 1) { fails++; printf("  FAIL  log-line route, %s, n=%zu: taken with half the lines strays\n", nm[mode], n); } }
        free(pool); free(a); free(ref);
    }
    printf("  log-line route: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- strings on prefix keys (2026-10-09, logos_sort_strings_mt): every output in strcmp order and a permutation of the input
 * pointers. Inputs: empty strings, bytes of 0x80 and up (strcmp compares them unsigned), identical strings, strings that are
 * prefixes of others ("", "a", "ab", ...), prefixes shared for 8, 9, 16 and 200 bytes (the key skips whole blocks), sorted and
 * reversed input; sizes on both sides of STR_SMALL (32) and of the parallel threshold (65536); 1, 2 and 8 threads. And strings
 * that END on the last byte before a page the process may not read: the 8-byte load must never cross into it (a crash here). ---- */
#include <sys/mman.h>
static int str_ptr_cmp(const void *x, const void *y) { uintptr_t a = (uintptr_t)*(const char *const *)x, b = (uintptr_t)*(const char *const *)y; return (a > b) - (a < b); }
static int str_check(const char **out, const char **in, const char **tmp, size_t n) {
    for (size_t i = 1; i < n; i++) if (strcmp(out[i - 1], out[i]) > 0) return 0;
    memcpy(tmp, in, n * sizeof *tmp); qsort(tmp, n, sizeof *tmp, str_ptr_cmp);
    const char **t2 = malloc(n * sizeof *t2); if (!t2) return 1;
    memcpy(t2, out, n * sizeof *t2); qsort(t2, n, sizeof *t2, str_ptr_cmp);
    int ok = memcmp(tmp, t2, n * sizeof *tmp) == 0;
    free(t2); return ok;
}
static int strings_checks(void) {
    int checks = 0, fails = 0, asserted = 0;
    static const size_t sizes[] = { 2, 3, 31, 32, 33, 100, 4097, 65535, 65536, 65537, 300007 };
    const int threads[] = { 1, 2, 8 };
    size_t NMAX = 300007;
    char *pool = malloc(NMAX * 240);
    const char **in = malloc(NMAX * sizeof *in), **a = malloc(NMAX * sizeof *a), **tmp = malloc(NMAX * sizeof *tmp);
    if (!pool || !in || !a || !tmp) { free(pool); free(in); free(a); free(tmp); printf("  strings: skipped (no memory)\n"); return 0; }
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) for (int mode = 0; mode < 9; mode++) {
        size_t n = sizes[si], o = 0;
        seed(20100 + (uint64_t)mode * 13 + si);
        for (size_t i = 0; i < n; i++) {
            char *s = pool + o; size_t L = 0;
            switch (mode) {
            case 0: L = nextr() % 24; for (size_t j = 0; j < L; j++) s[j] = (char)('a' + nextr() % 26); break;            /* short words, empties */
            case 1: L = 1 + nextr() % 20; for (size_t j = 0; j < L; j++) s[j] = (char)(1 + nextr() % 255); break;        /* every byte value but 0 */
            case 2: L = nextr() % 12; for (size_t j = 0; j < L; j++) s[j] = 'a' + (char)(j % 3); break;                 /* prefixes of one another */
            case 3: memset(s, 'x', 8); L = 8 + nextr() % 3; for (size_t j = 8; j < L; j++) s[j] = (char)('a' + nextr() % 3); break;   /* 8 shared */
            case 4: memset(s, 'q', 9); L = 9 + nextr() % 9; for (size_t j = 9; j < L; j++) s[j] = (char)(0x80 + nextr() % 4); break; /* 9 shared, high bytes */
            case 5: memset(s, 'p', 200); L = 200 + nextr() % 30; for (size_t j = 200; j < L; j++) s[j] = (char)('0' + nextr() % 10); break; /* 200 shared */
            case 6: L = 16; memcpy(s, "same-string-here", 16); if (nextr() % 3 == 0) { s[15] = 'X'; } break;       /* duplicates */
            case 7: L = (size_t)snprintf(s, 32, "k%09zu", i); break;                                                  /* sorted */
            default: L = (size_t)snprintf(s, 32, "k%09zu", n - i); break;                                             /* reversed */
            }
            s[L] = 0; in[i] = s; o += L + 1;
        }
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads + 2; ti++) {   /* the last two: 2 and 8 threads with the opt-in group hand-off */
            int T = ti < 3 ? threads[ti] : (ti == 3 ? 2 : 8), gp = ti >= 3;
            if (gp) setenv("LOGOS_STR_GROUPPAR", "1", 1);
            memcpy(a, in, n * sizeof *a);
            (void)logos_twist_last(25);
            logos_sort_strings_mt(a, n, T);
            size_t ran = logos_twist_last(25);
            if (gp) unsetenv("LOGOS_STR_GROUPPAR");
            checks++; if (!str_check(a, in, tmp, n)) { fails++; printf("  FAIL  strings, mode %d, n=%zu, threads=%d%s: wrong output\n", mode, n, T, gp ? ", LOGOS_STR_GROUPPAR=1" : ""); }
            checks++; asserted++;
            if ((n > 32) != (ran == 1)) { fails++; printf("  FAIL  strings, mode %d, n=%zu: the prefix-key path ran %zu times (expected %d)\n", mode, n, ran, n > 32); }
        }
    }
    /* each string one 8-byte block longer than the last ('x' x 8i, then 'y'): every level splits ONE string off a group of all the
       rest, so a sort that recursed into every group went 3000 levels deep (at 15.7 KB a frame, past an 8 MB stack). The rule that
       loops on the largest group instead keeps it at log2 n. The strings share one buffer: s_i starts 8(k - i) bytes into it. */
    {
        const size_t k = 3000; char *buf = malloc(8 * k + 2);
        const char **e = malloc(k * sizeof *e), **ein = malloc(k * sizeof *ein), **et = malloc(k * sizeof *et);
        if (buf && e && ein && et) {
            memset(buf, 'x', 8 * k); buf[8 * k] = 'y'; buf[8 * k + 1] = 0;
            for (size_t i = 0; i < k; i++) ein[i] = buf + 8 * (k - ((i * 7919) % k));          /* shuffled */
            for (int ti = 0; ti < 2; ti++) {
                memcpy(e, ein, k * sizeof *e);
                logos_sort_strings_mt(e, k, ti ? 8 : 1);
                checks++; if (!str_check(e, ein, et, k)) { fails++; printf("  FAIL  strings, nested blocks, threads=%d: wrong output\n", ti ? 8 : 1); }
            }
        }
        free(buf); free(e); free(ein); free(et);
    }
    /* strings ending on the last readable byte of a page */
    long pg = sysconf(_SC_PAGESIZE);
    unsigned char *m = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != MAP_FAILED && mprotect(m + pg, (size_t)pg, PROT_NONE) == 0) {
        size_t n = 0; const char *e[64];
        for (int len = 0; len < 12; len++) {                           /* each string's NUL is the page's last byte */
            unsigned char *s = m + pg - 1 - len; for (int j = 0; j < len; j++) s[j] = (unsigned char)('a' + (len + j) % 5);
            m[pg - 1] = 0; e[n++] = (const char *)s;                     /* they overlap: each is a suffix of the longer ones */
        }
        for (size_t i = 0; i < 40; i++) e[n++] = (const char *)(m + pg - 1 - (i % 12));
        const char *ein[64]; memcpy(ein, e, n * sizeof *e);
        logos_sort_strings_mt(e, n, 1);
        checks++; if (!str_check(e, ein, tmp, n)) { fails++; printf("  FAIL  strings at a page's end: wrong output\n"); }
        munmap(m, (size_t)pg * 2);
    }
    free(pool); free(in); free(a); free(tmp);
    printf("  strings: %d checks (%d of them name what must have happened), %d failures\n", checks, asserted, fails);
    return fails;
}

/* ---- the small sorting networks (n = 4..16 in the leaf). Nothing else here sorts an array that short WITH a NaN in
 * it: the IEEE section starts at 5000 elements. That matters because a comparator written as a min and a max returns
 * the same operand twice when one side is a NaN and LOSES the other, which is not a permutation; this one is written
 * as one predicate and two selects so that it cannot. Sizes run past the network's range on both sides so that the
 * gate itself is covered, and every input is checked as a permutation of the input BITS. ---- */
static int smallnet_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4 };
    for (size_t n = 2; n <= 24; n++) for (int mix = 0; mix < 8; mix++) for (size_t ti = 0; ti < 2; ti++) {
        double orig[24], a[24]; uint64_t bi[24], bo[24];
        seed(9100 + (uint64_t)n * 31 + (uint64_t)mix);
        for (size_t i = 0; i < n; i++) orig[i] = frand() * 2e9 - 1e9;
        switch (mix) {
        case 0: break;                                                             /* random */
        case 1: for (size_t i = 0; i < n; i++) orig[i] = (double)i; break;          /* sorted */
        case 2: for (size_t i = 0; i < n; i++) orig[i] = (double)(n - i); break;    /* reversed */
        case 3: for (size_t i = 0; i < n; i++) orig[i] = (double)(i % 3); break;    /* three values, many ties */
        case 4: orig[0] = NAN; break;                                              /* one NaN, at a sampled position */
        case 5: orig[n / 2] = NAN; orig[n - 1] = NAN; break;                        /* two NaNs, one at the end */
        case 6: for (size_t i = 0; i < n; i++) orig[i] = (i & 1) ? 0.0 : -0.0; break;   /* signed zeros only */
        case 7: orig[0] = INFINITY; orig[n - 1] = -INFINITY; if (n > 2) orig[1] = 4.9e-324; break;
        }
        size_t nnan = 0; for (size_t i = 0; i < n; i++) nnan += orig[i] != orig[i];
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        memcpy(a, orig, n * sizeof(double));
        logos_sort_f64(a, n, threads[ti]);
        memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
        int ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;                         /* always: a permutation of the bits */
        if (!nnan) for (size_t i = 1; ok && i < n; i++) if (a[i] < a[i - 1]) ok = 0; /* NaN-free: and ascending */
        checks++; if (!ok) { fails++; printf("  FAIL  smallnet n=%zu mix=%d threads=%d\n", n, mix, threads[ti]); }
    }
    printf("  small networks: %d checks, %d failures\n", checks, fails);
    return fails;
}

static int ieee_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 5000, 300001, 1000003 };
    for (int shape = 0; shape < 4; shape++) for (int mix = 0; mix < 8; mix++) for (size_t si = 0; si < 3; si++) {
        size_t n = sizes[si];
        if (n > 500000 && shape != 0) continue;                       /* the million-element size on one shape is enough */
        double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        uint64_t *bi = malloc(n * sizeof(uint64_t)), *bo = malloc(n * sizeof(uint64_t));
        if (!orig || !a || !bi || !bo) { free(orig); free(a); free(bi); free(bo); continue; }
        seed(6600 + (uint64_t)shape * 29 + (uint64_t)mix * 5 + si);
        ieee_gen(orig, n, shape); ieee_mix(orig, n, mix);
        memcpy(bi, orig, n * sizeof(double)); qsort(bi, n, sizeof(uint64_t), cmp_bits);
        size_t nnan = 0; for (size_t i = 0; i < n; i++) nnan += orig[i] != orig[i];
        for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
            /* the default entry */
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64(a, n, threads[ti]);
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            int ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;                                         /* always: a permutation of the bits */
            if (mix < 4) for (size_t i = 1; ok && i < n; i++) if (a[i] < a[i - 1]) ok = 0;              /* NaN-free: and sorted */
            checks++; if (!ok) { fails++; printf("  FAIL  ieee shape=%d mix=%d n=%zu threads=%d\n", shape, mix, n, threads[ti]); }
            /* the entry with a defined NaN order */
            memcpy(a, orig, n * sizeof(double));
            logos_sort_f64_nan_last(a, n, threads[ti]);
            memcpy(bo, a, n * sizeof(double)); qsort(bo, n, sizeof(uint64_t), cmp_bits);
            ok = memcmp(bi, bo, n * sizeof(uint64_t)) == 0;
            for (size_t i = 1; ok && i < n - nnan; i++) if (a[i] < a[i - 1] || a[i] != a[i]) ok = 0;    /* the prefix: NaN-free and ascending */
            for (size_t i = n - nnan; ok && i < n; i++) if (a[i] == a[i]) ok = 0;                       /* the tail: nothing but NaN */
            if (ok && n > nnan && a[0] != a[0]) ok = 0;
            checks++; if (!ok) { fails++; printf("  FAIL  ieee nan_last shape=%d mix=%d n=%zu threads=%d\n", shape, mix, n, threads[ti]); }
        }
        free(orig); free(a); free(bi); free(bo);
    }
    printf("  IEEE special values: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the strict in-place entries. Until 2026-09-20 nothing in this harness called them: they were verified only by the
 * benches that time them. They are also the fallback for arrays above 2^32 - 1 elements, and from that day they are
 * parallel (ranges as tasks after each placement pass), so both sides of the parallel threshold (2^20) are here, at 1, 2, 4
 * and 8 threads. A run passes when the output ascends under '<' and an order-independent hash of the bit patterns equals
 * the input's; with a NaN present only the hash is required (the prescan's compares cannot see a NaN: order unspecified). ---- */
static inline uint64_t ipc_mix(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; }
static uint64_t ipc_hash64(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8); s += ipc_mix(b); } return s; }
static uint64_t ipc_hash32(const int32_t *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) s += ipc_mix((uint64_t)(uint32_t)a[i] + 0x9e3779b97f4a7c15ULL); return s; }
enum { IPS_UNIFORM, IPS_HEAVY, IPS_FEWD, IPS_QUANT, IPS_NEAR, IPS_REVERSE, IPS_EQUAL, IPS_DOM90, IPS_SPECIALS, IPS_BLOCKS, IPS_NAN, IPS_N };
static const char *IPS_NAME[IPS_N] = { "uniform", "heavy_tail", "few_distinct", "quantized", "near_sorted", "reverse", "all_equal", "dominant_90", "specials", "blocks", "with_nan" };
static void ipc_gen64(double *a, size_t n, int s) {
    size_t nb = 64, blk = (n + nb - 1) / nb; if (!blk) blk = 1;
    for (size_t i = 0; i < n; i++) switch (s) {
        case IPS_UNIFORM:  a[i] = frand() * 2e9 - 1e9; break;
        case IPS_HEAVY:    a[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2); break;
        case IPS_FEWD:     a[i] = (double)(nextr() % 5) * 1e6; break;
        case IPS_QUANT:    a[i] = (double)(nextr() % 4096) / 64.0; break;
        case IPS_NEAR:     a[i] = (double)i + frand() * 0.5; break;
        case IPS_REVERSE:  a[i] = (double)(n - i); break;
        case IPS_EQUAL:    a[i] = 42.5; break;
        case IPS_DOM90:    a[i] = (nextr() % 10) ? 12345.0 : frand() * 2e9 - 1e9; break;
        case IPS_SPECIALS: { uint64_t k = nextr() % 16; a[i] = k == 0 ? INFINITY : k == 1 ? -INFINITY : k == 2 ? -0.0 : k == 3 ? 0.0 : k == 4 ? 4.9e-324 : k == 5 ? -4.9e-324
                                        : k == 6 ? 1.7976931348623157e308 : k == 7 ? -1.7976931348623157e308 : k == 8 ? (frand() - 0.5) * 1e-310 : frand() * 2e9 - 1e9; } break;
        case IPS_BLOCKS:   a[i] = (double)((((i / blk) * 37) % nb) * blk) + (double)(i % blk) + frand() * 0.5; break;
        default:           a[i] = (nextr() % 50) ? frand() * 2e9 - 1e9 : NAN; break;
    }
    if (s == IPS_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}
static void ipc_gen32(int32_t *a, size_t n, int s) {
    for (size_t i = 0; i < n; i++) switch (s) {
        case 0:  a[i] = (int32_t)nextr(); break;                                   /* wide */
        case 1:  a[i] = (int32_t)(nextr() % 1000); break;                          /* one and a half bytes vary */
        case 2:  a[i] = (nextr() % 10) ? 12345 : (int32_t)nextr(); break;          /* dominant */
        case 3:  a[i] = (int32_t)i - (int32_t)(n / 2); break;                      /* sorted, crossing zero */
        case 4:  a[i] = (int32_t)(n - i); break;                                   /* reverse */
        default: a[i] = (nextr() % 7) == 0 ? INT32_MIN : (nextr() % 7) == 1 ? INT32_MAX : (int32_t)nextr(); break;   /* the extremes */
    }
}
static int inplace_checks(void) {
    int checks = 0, fails = 0;
    const size_t small[] = { 0, 1, 2, 33, 1000, 65537 }, large[] = { (1u << 20) - 1, 1u << 20, 2500003 };
    const int tsmall[] = { 1, 4 }, tlarge[] = { 1, 2, 4, 8 };
    size_t nmax = 2500003;
    double *o64 = malloc(nmax * sizeof(double)), *a64 = malloc(nmax * sizeof(double));
    int32_t *o32 = malloc(nmax * sizeof(int32_t)), *a32 = malloc(nmax * sizeof(int32_t));
    if (!o64 || !a64 || !o32 || !a32) { free(o64); free(a64); free(o32); free(a32); printf("  in-place: skipped (no memory for the inputs)\n"); return 0; }
    for (int big = 0; big < 2; big++) {
        const size_t *sizes = big ? large : small; size_t ns = big ? sizeof large / sizeof *large : sizeof small / sizeof *small;
        const int *threads = big ? tlarge : tsmall; size_t nt = big ? sizeof tlarge / sizeof *tlarge : sizeof tsmall / sizeof *tsmall;
        for (size_t si = 0; si < ns; si++) {
            size_t n = sizes[si];
            for (int s = 0; s < IPS_N; s++) {
                seed(9090 + (uint64_t)s * 101 + si); ipc_gen64(o64, n, s); uint64_t h = ipc_hash64(o64, n);
                for (size_t ti = 0; ti < nt; ti++) {
                    memcpy(a64, o64, n * sizeof(double)); logos_sort_f64_inplace(a64, n, threads[ti]);
                    int ok = ipc_hash64(a64, n) == h;
                    if (s != IPS_NAN) for (size_t i = 1; i < n; i++) if (a64[i] < a64[i - 1]) { ok = 0; break; }
                    checks++; if (!ok) { fails++; printf("  FAIL  in-place f64 %s n=%zu threads=%d\n", IPS_NAME[s], n, threads[ti]); }
                }
            }
            for (int s = 0; s < 6; s++) {
                seed(7070 + (uint64_t)s * 53 + si); ipc_gen32(o32, n, s); uint64_t h = ipc_hash32(o32, n);
                for (size_t ti = 0; ti < nt; ti++) {
                    memcpy(a32, o32, n * sizeof(int32_t)); logos_sort_i32_inplace(a32, n, threads[ti]);
                    int ok = ipc_hash32(a32, n) == h;
                    for (size_t i = 1; i < n; i++) if (a32[i] < a32[i - 1]) { ok = 0; break; }
                    checks++; if (!ok) { fails++; printf("  FAIL  in-place i32 shape=%d n=%zu threads=%d\n", s, n, threads[ti]); }
                }
            }
        }
    }
    free(o64); free(a64); free(o32); free(a32);
    printf("  in-place: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- the short-reach route: index blocks sorted where they lie, seams repaired, strays shifted back in place. Every branch
 * of it is asked for BY NAME: logos_reach_last() says what the route did, and a row whose expectation is not met fails even
 * when the output is sorted, because a test that cannot tell which path ran tests nothing in particular (the first cut of
 * this section passed with the route never firing at one thread: the gate's sample was 384 points there, not 2048).
 * Checked as ascending under '<' plus an identical bitwise multiset, and against the same sort with the route switched off. ---- */
enum { RM_JITTER, RM_WINDOW, RM_ONE_UP, RM_ONE_DOWN, RM_THREE, RM_EVERYWHERE, RM_HIDDEN, RM_DUPS, RM_SEAMTIES, RM_ZEROS, RM_INF, RM_NAN,
       RM_SUBNORMAL, RM_GAP, RM_PLATEAU, RM_SWAPPED, RM_SPARSE, RM_SPARSEFAR, RM_SPARSEENDS, RM_N };
static const char *RM_NAME[RM_N] = { "jitter", "windows", "one stray up", "one stray down", "three strays", "strays everywhere", "strays the probe cannot see",
    "duplicates", "ties at the seams", "signed zeros", "infinities", "a NaN", "subnormals", "quiet hours (two clusters in a block)", "a plateau in a block",
    "two blocks swapped", "sparse kinks (every 40th pair, adjacent)", "sparse displaced (every 400th element, a third of the array away)",
    "sparse kinks with one at each END of the array" };
static void reach_gen(double *a, size_t n, int mode) {
    double J = (double)n / 4000.0; if (J < 50) J = 50;                  /* the reach grows with n so that the gate's ladder (distances are fractions of n) accepts it */
    const size_t B = 32768;
    for (size_t i = 0; i < n; i++) a[i] = (double)i + (frand() * 2.0 - 1.0) * J;
    switch (mode) {
    case RM_WINDOW:   for (size_t i = 0; i < n; i++) a[i] = (double)i + frand();
                      for (size_t b = 0; b < n; b += 512) { size_t m = n - b < 512 ? n - b : 512; for (size_t q = m - 1; q > 0; q--) { size_t r = nextr() % (q + 1); double t = a[b + q]; a[b + q] = a[b + r]; a[b + r] = t; } } break;
    case RM_ONE_UP:   a[n / 3] = 4.0 * (double)n; break;
    case RM_ONE_DOWN: a[2 * n / 3] = -1e9; break;
    case RM_THREE:    a[5] = 4.0 * (double)n; a[n / 2] = -1e9; a[n - 3] = (double)n / 4.0; break;
    case RM_EVERYWHERE: for (size_t q = 0; q < n / 2000 + 8; q++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case RM_HIDDEN: { size_t K = n / B;                                 /* one stray in every block the eight probe blocks are not */
                      for (size_t k = 0; k < K; k++) { int probe = 0; for (size_t q = 0; q < 8; q++) if (k == (q * 2 + 1) * K / 16) probe = 1; if (!probe) a[k * B + B / 2] = 4.0 * (double)n + (double)k; } } break;
    case RM_DUPS:     for (size_t i = 0; i < n; i++) a[i] = floor(a[i] / (2.0 * J)); break;
    case RM_SEAMTIES: for (size_t i = 0; i < n; i++) a[i] = floor(a[i] / 16.0) * 16.0 + 0.5; break;
    case RM_ZEROS:    { int flip = 0; for (size_t i = 0; i < n; i++) { a[i] -= (double)(n / 2); if (a[i] > -0.4 * J && a[i] < 0.4 * J) a[i] = (flip ^= 1) ? -0.0 : 0.0; } } break;
    case RM_INF:      a[0] = -INFINITY; a[n - 1] = INFINITY; a[n / 2] = INFINITY; break;
    case RM_NAN:      a[n / 2 + 11] = NAN; break;
    case RM_SUBNORMAL: for (size_t i = 0; i < n; i++) a[i] = (a[i] + 2.0 * J) * 4.9406564584124654e-321; break;
    case RM_GAP:      for (size_t i = 0; i < n; i++) a[i] += 1e7 * (double)(i / 50000); break;
    case RM_PLATEAU:  { size_t p0 = n / 2 + 1000, len = n / 4000 < 3000 ? 3000 : n / 4000; for (size_t i = p0; i < p0 + len && i < n; i++) a[i] = (double)p0 + (double)len / 2.0; } break;
    case RM_SWAPPED:  { size_t K = n / B, k = K / 2 - 5; if (K >= 40) for (size_t i = 0; i < B; i++) { double t = a[k * B + i]; a[k * B + i] = a[(k + 1) * B + i]; a[(k + 1) * B + i] = t; } } break;
    case RM_SPARSE:   for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.25; for (size_t i = 0; i + 1 < n; i += 40) { double t = a[i]; a[i] = a[i + 1]; a[i + 1] = t; } break;
    /* the same sparseness with every displaced element a third of the array from home: the descents it makes fail the
       kink shape on one side, so the probe must report the plain gate decline and leave it to near-sorted. Every 400th
       (0.25%), because a far swap inverts its sampled pairs where an adjacent one never does and REACH_TAU is 2%. */
    case RM_SPARSEFAR: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.25;
                       for (size_t i = 0; i < n; i += 400) { size_t j = i + n / 3 + (i * 7919u) % (n / 3 + 1); if (j >= n) break; double t = a[i]; a[i] = a[j]; a[j] = t; } break;
    /* the every-40th kinks starting at 1 (so no interior pair touches an end), plus one kink at EACH end of the array:
       the worker's test reads a[j - 2] and a[j + 1], which do not exist there, and until 2026-09-26 neither end was
       ever repaired -- at P > 1 the head came back as a two-element descending run and the strand was never one run */
    case RM_SPARSEENDS: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.25;
                        for (size_t i = 41; i + 1 < n - 2; i += 40) { double t = a[i]; a[i] = a[i + 1]; a[i + 1] = t; }
                        { double t = a[0]; a[0] = a[1]; a[1] = t; t = a[n - 2]; a[n - 2] = a[n - 1]; a[n - 1] = t; } break;
    default: break;
    }
}
/* what the route must report, or -1 where it depends on the block count (eight blocks are all probe blocks) or on another route.
 * Strays are simply put back when at most four threads sort (the serial shift pass is cheap against what the route saves there);
 * wider sorts ask eight probe blocks first. The engine uses at most one thread per 100k elements. */
static int reach_expect(int mode, size_t n, int threads) {
    int big = n >= 1000000; size_t cap = n / 100000; int eff = (size_t)threads > cap ? (int)(cap ? cap : 1) : threads, tolerant = eff <= 4;
    if (n < 262144) return -1;      /* under the route's floor: the caller asks for an UNCHANGED answer instead */
    switch (mode) {
    case RM_JITTER: case RM_WINDOW: case RM_ZEROS: case RM_SUBNORMAL: case RM_GAP: case RM_PLATEAU: return LOGOS_REACH_SORTED;
    case RM_ONE_UP: case RM_ONE_DOWN: return LOGOS_REACH_SORTED_STRAYS;
    case RM_THREE: case RM_INF: return big || tolerant ? LOGOS_REACH_SORTED_STRAYS : -1;
    case RM_EVERYWHERE: return tolerant ? LOGOS_REACH_SORTED_STRAYS : LOGOS_REACH_DECLINED_PROBE;
    case RM_HIDDEN: return big ? LOGOS_REACH_SORTED_STRAYS : -1;         /* eight blocks are all probe blocks: nothing is hidden there */
    case RM_SWAPPED: return n >= 3000000 ? LOGOS_REACH_DECLINED_PLAN : -1;
    case RM_SPARSE: return LOGOS_REACH_DECLINED_KINKS;     /* was DECLINED_GATE until 2026-09-26: sparse AND kink-shaped now says so */
    case RM_SPARSEFAR: return LOGOS_REACH_DECLINED_GATE;   /* sparse but displaced: the plain decline, near-sorted's kind */
    case RM_SPARSEENDS: return LOGOS_REACH_DECLINED_KINKS; /* kinks, two of them at the ends: the same verdict, and one run */
    default: return -1;
    }
}
static int reach_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 70000, 262144, 262145, 300001, 1000003, 3000017 };
    /* one name per LOGOS_REACH_* value, IN ORDER: when the kink verdict was added without its name here, every failing
       row printed "the route reports '(null)'" from ST[7] one past the end, which is undefined behaviour that happened
       to be legible. A new verdict lands in both places in the same edit. */
    static const char *ST[] = { "not tried", "sorted", "sorted, strays shifted in", "declined at the gate", "declined by the probe", "declined by the plan", "declined: no memory",
                                "declined: sparse kinks (for the detector)" };
    unsetenv("LOGOS_NO_REACH");
    for (int mode = 0; mode < RM_N; mode++) {
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            if (n > 2000000 && !(mode == RM_JITTER || mode == RM_THREE || mode == RM_HIDDEN || mode == RM_SWAPPED || mode == RM_ONE_DOWN)) continue;
            double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double)), *b = malloc(n * sizeof(double));
            if (!orig || !a || !b) { free(orig); free(a); free(b); continue; }
            seed(9100 + (uint64_t)mode * 31 + si);
            reach_gen(orig, n, mode);
            uint64_t h = ipc_hash64(orig, n);
            for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ti++) {
                if (n > 2000000 && threads[ti] != 8) continue;
                memcpy(a, orig, n * sizeof(double));
                int before = logos_reach_last();
                logos_twist_last(12);                                        /* clear the one-strand counter: reading does */
                logos_sort_f64(a, n, threads[ti]);
                /* under the route's floor a sort must leave the answer ALONE (no thread-local access on a small sort's path), so the
                 * expectation there is whatever the previous row left, which is a real state for every mode but the first */
                int got = logos_reach_last(), ok = ipc_hash64(a, n) == h, want = n < 262144 ? before : reach_expect(mode, n, threads[ti]);
                if (mode != RM_NAN) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                checks++; if (!ok) { fails++; printf("  FAIL  short reach, %s, n=%zu threads=%d: wrong output (route: %s)\n", RM_NAME[mode], n, threads[ti], ST[got]); }
                checks++; if (want >= 0 && got != want) { fails++; printf("  FAIL  short reach, %s, n=%zu threads=%d: the route reports '%s', this row is here for '%s'\n", RM_NAME[mode], n, threads[ti], ST[got], ST[want]); }
                /* a kinked strand must come back as ONE run and be finished as such, at every thread count -- the kink verdict
                   alone was not enough: a kink at index 0 left R = 2 at P > 1 and the finish never fired (2026-09-26) */
                if ((mode == RM_SPARSE || mode == RM_SPARSEENDS) && n >= 262144) {
                    checks++; if (!logos_twist_last(12)) { fails++; printf("  FAIL  short reach, %s, n=%zu threads=%d: the strand was not finished as one run\n", RM_NAME[mode], n, threads[ti]); } }
                if (ti == 1 && mode != RM_NAN && mode != RM_ZEROS) {        /* the same input with the route off: two correct sorts of one array are the same bytes */
                    memcpy(b, orig, n * sizeof(double)); setenv("LOGOS_NO_REACH", "1", 1); logos_sort_f64(b, n, threads[ti]); unsetenv("LOGOS_NO_REACH");
                    checks++; if (memcmp(a, b, n * sizeof(double)) || logos_reach_last() != (n < 262144 ? got : LOGOS_REACH_NOT_TRIED)) { fails++; printf("  FAIL  short reach, %s, n=%zu: differs from the sort with the route off\n", RM_NAME[mode], n); }
                }
            }
            free(orig); free(a); free(b);
        }
    }
    printf("  short-reach route: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- a sort must RETURN, whatever it was given. With a NaN present the order is unspecified, but the call still has to come
 * back with a permutation of the input. It did not always: the gap-split partition scanned with a[i] < pivot from the left and
 * a[j] >= pivot from the right, a NaN fails BOTH, and when the two scans met on one neither moved again. Whether they meet on a
 * NaN depends on the order inside a bucket, which depends on thread timing, so it spun about one IEEE-section run in fifteen and
 * had to be caught with a looping hunter (2026-09-20). Heavy-tailed data with 1% NaN at 4 threads hit it about one sort in
 * thirty: 150 of those under a watchdog is a 99% chance of seeing the old code spin. ---- */
#include <signal.h>
#include <unistd.h>
static char nanterm_what[96];
static void nanterm_alarm(int sig) {
    (void)sig; static const char m[] = "\n  FAIL  a sort did not return: ";
    if (write(2, m, sizeof m - 1) < 0 || write(2, nanterm_what, strlen(nanterm_what)) < 0 || write(2, "\n", 1) < 0) { }
    _exit(1);
}
static int nan_termination_checks(void) {
    int checks = 0, fails = 0; size_t n = 300001;
    double *a = malloc(n * sizeof(double));
    if (!a) { printf("  NaN termination: skipped (no memory)\n"); return 0; }
    signal(SIGALRM, nanterm_alarm);
    for (int r = 0; r < 150; r++) {
        seed(1000 + (uint64_t)r);
        for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2);
        for (size_t i = 0; i < n; i += 100) a[i] = NAN;
        uint64_t h = ipc_hash64(a, n);
        snprintf(nanterm_what, sizeof nanterm_what, "heavy_tail with 1%% NaN, n=%zu, 4 threads, repetition %d", n, r);
        alarm(60);
        logos_sort_f64(a, n, 4);
        alarm(0);
        int ok = ipc_hash64(a, n) == h;
        checks++; if (!ok) { fails++; printf("  FAIL  NaN termination: output is not a permutation, repetition %d\n", r); }
    }
    signal(SIGALRM, SIG_DFL);
    free(a);
    printf("  NaN termination: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- boundaries. A dispatcher is a list of thresholds, and an off-by-one lives exactly AT one. Every n from 0 to 300 is run
 * (insertion hands over to run detection at 25, the relocation plan is asked from 25 to 76, the flash classes change with n),
 * then every power of two from 2^9 to 2^21 from below, at and above (MIN_PARALLEL 2^16, the view's 2^12, the in-place sort's
 * 2^20), then the multiples the thread policy and the per-route caps cut at (100k, 350k, 500k), each from below, at and above.
 * Both types, 1 and 4 threads (8 too above 300k), the buffered and the in-place entries. Ascending under '<' plus an
 * order-independent hash of the bit patterns. ---- */
static int boundary_run(double *o64, double *a64, int32_t *o32, int32_t *a32, size_t n, int big, int *checks) {
    static const int s64_small[] = { IPS_UNIFORM, IPS_FEWD, IPS_QUANT, IPS_NEAR, IPS_REVERSE, IPS_BLOCKS, IPS_EQUAL }, s64_big[] = { IPS_UNIFORM, IPS_FEWD, IPS_NEAR, IPS_BLOCKS };
    static const int t_small[] = { 1, 4 }, t_big[] = { 1, 4, 8 };
    const int *shapes = big ? s64_big : s64_small; int nsh = big ? 4 : 7; const int *th = big ? t_big : t_small; int nth = big ? 3 : 2, fails = 0;
    for (int si = 0; si < nsh; si++) {
        seed(4242 + (uint64_t)shapes[si] * 7 + n); ipc_gen64(o64, n, shapes[si]); uint64_t h = ipc_hash64(o64, n);
        for (int ti = 0; ti < nth; ti++) for (int inplace = 0; inplace < 2; inplace++) {
            if (inplace && big && ti != 1) continue;                       /* the in-place entry at large n: 4 threads only, it is slow */
            memcpy(a64, o64, n * sizeof(double));
            if (inplace) logos_sort_f64_inplace(a64, n, th[ti]); else logos_sort_f64(a64, n, th[ti]);
            int ok = ipc_hash64(a64, n) == h; for (size_t i = 1; i < n; i++) if (a64[i] < a64[i - 1]) { ok = 0; break; }
            (*checks)++; if (!ok) { fails++; printf("  FAIL  boundary f64 %s%s n=%zu threads=%d\n", IPS_NAME[shapes[si]], inplace ? " in-place" : "", n, th[ti]); }
        }
    }
    for (int s = 0; s < (big ? 1 : 3); s++) {
        int shape = s == 2 ? 4 : s;                                          /* wide, span 1000, reverse */
        seed(2424 + (uint64_t)shape * 11 + n); ipc_gen32(o32, n, shape); uint64_t h = ipc_hash32(o32, n);
        for (int ti = 0; ti < nth; ti++) {
            memcpy(a32, o32, n * sizeof(int32_t)); logos_sort_i32(a32, n, th[ti]);
            int ok = ipc_hash32(a32, n) == h; for (size_t i = 1; i < n; i++) if (a32[i] < a32[i - 1]) { ok = 0; break; }
            (*checks)++; if (!ok) { fails++; printf("  FAIL  boundary i32 shape=%d n=%zu threads=%d\n", shape, n, th[ti]); }
        }
    }
    return fails;
}
static int boundary_checks(void) {
    int checks = 0, fails = 0; size_t nmax = ((size_t)1 << 21) + 1;
    double *o64 = malloc(nmax * sizeof(double)), *a64 = malloc(nmax * sizeof(double));
    int32_t *o32 = malloc(nmax * sizeof(int32_t)), *a32 = malloc(nmax * sizeof(int32_t));
    if (!o64 || !a64 || !o32 || !a32) { free(o64); free(a64); free(o32); free(a32); printf("  boundaries: skipped (no memory for the inputs)\n"); return 0; }
    for (size_t n = 0; n <= 300; n++) fails += boundary_run(o64, a64, o32, a32, n, 0, &checks);
    for (int k = 9; k <= 21; k++) for (int d = -1; d <= 1; d++) { size_t n = ((size_t)1 << k) + (size_t)d; fails += boundary_run(o64, a64, o32, a32, n, n > 300000, &checks); }
    const size_t cuts[] = { 100000, 350000, 500000 };
    for (size_t c = 0; c < 3; c++) for (size_t j = 1; j <= 3; j++) for (int d = -1; d <= 1; d++) { size_t n = cuts[c] * j + (size_t)d; fails += boundary_run(o64, a64, o32, a32, n, n > 300000, &checks); }
    free(o64); free(a64); free(o32); free(a32);
    printf("  boundaries: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- a chain of MORE runs than merge_all_runs keeps on its stack (64). The run-merge route never takes more than 8 runs, and
 * a knot of this many strands is declined by its cost gate, so the only way in is relocation merging a long chain: three long
 * sorted blocks, shuffled, followed by 70 short ascending runs interleaved over ONE value range below them (every one
 * overlaps every other: one chain). The chain is under half the array, which is what relocation requires. The resource section
 * runs the same shape with every allocation failed in turn, which reaches the fold that needs no list at all. ---- */
static void many_runs_gen(double *a, size_t n) {
    /* the blocks FIRST and the chain at the tail: relocation's progressive gate judges the prefix it has seen, and a prefix
       that is all one chain makes it bail before it reaches the blocks (traced, the first cut of this shape) */
    size_t tail = n / 4, runs = 70, rl = tail / runs; if (rl < 2) rl = 2;
    size_t rest = n - tail, blk = rest / 3; const size_t order[3] = { 2, 0, 1 }; if (!blk) blk = 1;
    for (size_t i = 0; i < rest; i++) { size_t b = i / blk < 3 ? i / blk : 2; a[i] = 2e6 + (double)order[b] * 1e7 + (double)(i - b * blk) + frand() * 0.5; }
    for (size_t i = 0; i < tail; i++) { size_t r = i / rl, j = i % rl; a[rest + i] = (double)j * (1e6 / (double)rl) + (double)r * 1e-3 + frand() * 1e-4; }
}
static int many_runs_checks(void) {
    int checks = 0, fails = 0;
    const size_t sizes[] = { 40000, 400000, 2100000 }; const int threads[] = { 1, 4, 8 };
    for (size_t si = 0; si < 3; si++) {
        size_t n = sizes[si]; double *o = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double));
        if (!o || !a) { free(o); free(a); continue; }
        seed(6161 + si); many_runs_gen(o, n); uint64_t h = ipc_hash64(o, n);
        for (int ti = 0; ti < 3; ti++) {
            memcpy(a, o, n * sizeof(double)); logos_sort_f64(a, n, threads[ti]);
            int ok = ipc_hash64(a, n) == h; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
            checks++; if (!ok) { fails++; printf("  FAIL  many runs n=%zu threads=%d\n", n, threads[ti]); }
        }
        free(o); free(a);
    }
    printf("  many-run chain: %d checks, %d failures\n", checks, fails);
    return fails;
}

/* ---- resource exhaustion: an allocation or a thread creation that FAILS in the middle of a sort ------------------------
 * The in-process twin of `make faultprobe` (which forks a child per run, fails EVERY call in turn, and can therefore
 * report a crash or a hang as a result). Here the same interposers live in the harness, so the fallback paths are part of
 * `make test` instead of code that only a separate probe ever runs: before 2026-09-20 no pthread_create result was
 * looked at, and a failed creation was a hang in the partition and SILENTLY WRONG OUTPUT in the counting routes.
 *   thread creation   every creation of one call fails in turn (`one`), and every creation from the k-th on (`rest`)
 *   allocation        every fourth allocation of one COLD call (the pools are released first), both modes
 * Only calls made from this executable's text are failed, so glibc's own allocations are left alone. glibc 2.34+ only
 * (dlsym in libc proper, __libc_malloc), and not under a sanitizer, which interposes the same functions. ---- */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#    define HARNESS_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define HARNESS_SANITIZED 1
#endif
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && !defined(HARNESS_SANITIZED) && !defined(HARNESS_NO_FAULTS)
#  if __GLIBC_PREREQ(2, 34)
#    define HARNESS_FAULTS 1
#  endif
#endif
/* Under ThreadSanitizer the thread-creation half stays on. TSan exports the interceptor it installs, so a creation that is
 * not failed is forwarded to it and TSan still sees every thread; the allocation half is off, because TSan owns malloc.
 * That puts barrier_drop and the orphan paths, the most race-prone code added on 2026-09-20, under the race detector. */
#if defined(__SANITIZE_THREAD__) && defined(__GLIBC__) && !defined(HARNESS_NO_FAULTS)
#  define HARNESS_FAULTS 1
#  define HARNESS_FAULTS_SPAWN_ONLY 1
#endif
#ifdef HARNESS_FAULTS
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#ifndef HARNESS_FAULTS_SPAWN_ONLY
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
#else
extern int __interceptor_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
#endif
extern char __executable_start[], etext[];
static _Atomic int ft_armed;
static _Atomic long ft_allocs, ft_spawns;
static long ft_target; static int ft_kind, ft_rest;
static int (*ft_real_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int ft_fail(int kind, void *ra) {
    if (!atomic_load_explicit(&ft_armed, memory_order_relaxed) || (char *)ra < __executable_start || (char *)ra >= etext) return 0;
    long k = atomic_fetch_add_explicit(kind ? &ft_spawns : &ft_allocs, 1, memory_order_relaxed) + 1;
    return kind == ft_kind && ft_target > 0 && (ft_rest ? k >= ft_target : k == ft_target);
}
#define FT_RA __builtin_return_address(0)
#ifndef HARNESS_FAULTS_SPAWN_ONLY
void *malloc(size_t n) { if (ft_fail(0, FT_RA)) { errno = ENOMEM; return NULL; } return __libc_malloc(n); }
void *calloc(size_t n, size_t z) { if (ft_fail(0, FT_RA)) { errno = ENOMEM; return NULL; } return __libc_calloc(n, z); }
void *realloc(void *p, size_t n) { if (ft_fail(0, FT_RA)) { errno = ENOMEM; return NULL; } return __libc_realloc(p, n); }
int posix_memalign(void **out, size_t al, size_t n) {
    if (ft_fail(0, FT_RA)) return ENOMEM;
    void *p = __libc_memalign(al, n); if (!p) return ENOMEM;
    *out = p; return 0;
}
#endif
int pthread_create(pthread_t *th, const pthread_attr_t *at, void *(*fn)(void *), void *arg) {
    if (ft_fail(1, FT_RA)) return EAGAIN;
#ifdef HARNESS_FAULTS_SPAWN_ONLY
    (void)ft_real_create;
    return __interceptor_pthread_create(th, at, fn, arg);
#else
    if (!ft_real_create) ft_real_create = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))dlsym(RTLD_NEXT, "pthread_create");
    return ft_real_create ? ft_real_create(th, at, fn, arg) : EAGAIN;
#endif
}

enum { FE_F64, FE_I32, FE_VIEW64, FE_VIEW32, FE_IP64, FE_IP32 };
static const char *FE_NAME[] = { "sort f64", "sort i32", "view f64", "view i32", "in-place f64", "in-place i32" };
typedef struct { const char *shape; int entry; } ft_case_t;
static const ft_case_t FT_CASES[] = {
    { "uniform", FE_F64 }, { "heavy_tail", FE_F64 }, { "few_distinct", FE_F64 }, { "quantized", FE_F64 }, { "mod_card_2k", FE_F64 },
    { "near_sorted", FE_F64 }, { "block_perm", FE_F64 }, { "stairs_x20", FE_F64 }, { "stairs_triple", FE_F64 }, { "dominant_90", FE_F64 },
    { "prefix_95", FE_F64 }, { "many_runs", FE_F64 }, { "short_reach", FE_F64 }, { "short_reach_stray", FE_F64 }, { "int_wide", FE_I32 }, { "int_span_1000", FE_I32 }, { "int_sentinel", FE_I32 },
    { "uniform", FE_VIEW64 }, { "int_wide", FE_VIEW32 },
    { "uniform", FE_IP64 }, { "dominant_90", FE_IP64 }, { "int_wide", FE_IP32 },
};
static void ft_gen64(double *a, size_t n, const char *s) {
    if (!strcmp(s, "many_runs")) { seed(7); many_runs_gen(a, n); return; }      /* 70 runs in one chain: merge_all_runs past its stack lists */
    seed(7); size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nextr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(s, "uniform")) a[i] = frand() * 2e9 - 1e9;
        else if (!strcmp(s, "heavy_tail")) a[i] = 1000.0 * pow(1.0 - frand() * 0.999999, -1.0 / 1.2);
        else if (!strcmp(s, "few_distinct")) a[i] = (double)(nextr() % 5) * 1e6;
        else if (!strcmp(s, "quantized")) a[i] = (double)(nextr() % 4096) / 64.0;
        else if (!strcmp(s, "mod_card_2k")) a[i] = sqrt((double)(nextr() % 2000));
        else if (!strcmp(s, "near_sorted")) a[i] = (double)i + frand() * 0.5;
        else if (!strcmp(s, "block_perm")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + frand() * 0.5;
        else if (!strcmp(s, "stairs_x20")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + 1.0 / 9.0) + frand() * 0.5;
        else if (!strcmp(s, "stairs_triple")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * 2.5 + frand() * 0.5;
        else if (!strcmp(s, "dominant_90")) a[i] = (nextr() % 10) ? 12345.0 : frand() * 2e9 - 1e9;
        else if (!strncmp(s, "short_reach", 11)) a[i] = (double)i + (frand() * 2.0 - 1.0) * 60.0;        /* the short-reach route: blocks in place, seams */
        else a[i] = i < n - n / 20 ? (double)i : frand() * (double)n;                                   /* prefix_95 */
    }
    if (!strcmp(s, "near_sorted")) for (size_t k = 0; k < n / 100; k++) { size_t x = nextr() % n, y = nextr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
    if (!strcmp(s, "short_reach_stray")) a[n / 3] = 4.0 * (double)n;                               /* one far element: the stray lists and the in-place shift */
}
static void ft_gen32(int32_t *a, size_t n, const char *s) {
    seed(7);
    for (size_t i = 0; i < n; i++) a[i] = !strcmp(s, "int_wide") ? (int32_t)nextr() : (int32_t)(nextr() % 1000);
    if (!strcmp(s, "int_sentinel")) a[n / 3] = INT32_MIN;
}
static inline uint64_t ft_mix(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; }
static uint64_t ft_hash64(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8); s += ft_mix(b); } return s; }
static uint64_t ft_hash32(const int32_t *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) s += ft_mix((uint64_t)(uint32_t)a[i] + 0x9e3779b97f4a7c15ULL); return s; }
static size_t ft_starts[LOGOS_MAX_VIEW_BUCKETS + 1]; static unsigned char ft_done[LOGOS_MAX_VIEW_BUCKETS];
static double ft_min64[LOGOS_MAX_VIEW_BUCKETS], ft_max64[LOGOS_MAX_VIEW_BUCKETS]; static int32_t ft_min32[LOGOS_MAX_VIEW_BUCKETS], ft_max32[LOGOS_MAX_VIEW_BUCKETS];
static char ft_what[160];
static void ft_alarm(int sig) {
    (void)sig; static const char m[] = "\n  FAIL  a faulted call HUNG: ";
    if (write(2, m, sizeof m - 1) < 0 || write(2, ft_what, strlen(ft_what)) < 0 || write(2, "\n", 1) < 0) { }
    _exit(1);
}
/* one call with the fault armed; returns 1 when the output is sorted and a permutation of the input's bits. target 0 counts. */
static int ft_run(int entry, double *a, int32_t *ia, const double *orig, const int32_t *io, size_t n, uint64_t h, int T,
                  int kind, int rest, long target, int cold, long *count) {
    int is64 = entry == FE_F64 || entry == FE_VIEW64 || entry == FE_IP64;
    if (is64) memcpy(a, orig, n * sizeof(double)); else memcpy(ia, io, n * sizeof(int32_t));
    if (cold) logos_release_buffers();
    ft_kind = kind; ft_rest = rest; ft_target = target;
    atomic_store(&ft_allocs, 0); atomic_store(&ft_spawns, 0); atomic_store(&ft_armed, 1);
    if (entry == FE_F64) logos_sort_f64(a, n, T);
    else if (entry == FE_IP64) logos_sort_f64_inplace(a, n, T);
    else if (entry == FE_IP32) logos_sort_i32_inplace(ia, n, T);
    else if (entry == FE_I32) logos_sort_i32(ia, n, T);
    else if (entry == FE_VIEW64) { int nbk = logos_partition_f64(a, n, T, ft_starts, ft_min64, ft_max64); if (nbk > 0) { memset(ft_done, 0, sizeof ft_done); logos_sort_buckets_f64(a, ft_starts, nbk, ft_done, T); } }
    else { int nbk = logos_partition_i32(ia, n, T, ft_starts, ft_min32, ft_max32); if (nbk > 0) { memset(ft_done, 0, sizeof ft_done); logos_sort_buckets_i32(ia, ft_starts, nbk, ft_done, T); } }
    atomic_store(&ft_armed, 0);
    if (count) *count = kind ? atomic_load(&ft_spawns) : atomic_load(&ft_allocs);
    if (is64) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return ft_hash64(a, n) == h; }
    for (size_t i = 1; i < n; i++) if (ia[i] < ia[i - 1]) return 0;
    return ft_hash32(ia, n) == h;
}
static int fault_checks(void) {
    int checks = 0, fails = 0; long spawn_runs = 0, alloc_runs = 0;
    const size_t n_spawn = 2100000, n_alloc = 600000;       /* relocation and knot take one thread per 500k: 2.1M gives them four */
    double *orig = malloc(n_spawn * sizeof(double)), *a = malloc(n_spawn * sizeof(double));
    int32_t *io = malloc(n_spawn * sizeof(int32_t)), *ia = malloc(n_spawn * sizeof(int32_t));
    if (!orig || !a || !io || !ia) { free(orig); free(a); free(io); free(ia); printf("  resource exhaustion: skipped (no memory for the inputs)\n"); return 0; }
    signal(SIGALRM, ft_alarm);
    for (size_t ci = 0; ci < sizeof FT_CASES / sizeof *FT_CASES; ci++) {
        int e = FT_CASES[ci].entry, is64 = e == FE_F64 || e == FE_VIEW64 || e == FE_IP64; const char *sh = FT_CASES[ci].shape;
#ifdef HARNESS_FAULTS_SPAWN_ONLY
        const int kind_min = 1;                                          /* ThreadSanitizer build: thread creation only */
#else
        const int kind_min = 0;
#endif
        for (int kind = 1; kind >= kind_min; kind--) {                   /* thread creation first (at 2.1M), then allocation (cold, at 600k) */
            size_t n = kind ? n_spawn : n_alloc; long step = kind ? 1 : (!strcmp(sh, "many_runs") ? 1 : 4);   /* many_runs: every allocation, so both merge lists are failed */
            if (is64) ft_gen64(orig, n, sh); else ft_gen32(io, n, sh);
            uint64_t h = is64 ? ft_hash64(orig, n) : ft_hash32(io, n);
            for (int ti = 0; ti < 2; ti++) {
                int T = ti ? 4 : 1; long K = 0;
                if (kind && T == 1) continue;                            /* one thread creates nothing */
                for (int np = 0; np < (kind ? 2 : 1); np++) {                 /* thread creation: with the worker pool, then without */
                if (np) setenv("LOGOS_NO_POOL", "1", 1);
                snprintf(ft_what, sizeof ft_what, "%s %s threads=%d unfaulted%s", FE_NAME[e], sh, T, np ? " LOGOS_NO_POOL=1" : ""); alarm(120);
                int ok = ft_run(e, a, ia, orig, io, n, h, T, kind, 0, 0, 1, &K);   /* cold: a warm pool would create nothing to fail */
                checks++; if (!ok) { fails++; printf("  FAIL  %s\n", ft_what); }
                for (int rest = 0; rest < 2; rest++) for (long k = 1; k <= K; k += step) {
                    snprintf(ft_what, sizeof ft_what, "%s %s threads=%d %s %s k=%ld of %ld", FE_NAME[e], sh, T, kind ? "thread creation" : "allocation", rest ? "rest" : "one", k, K);
                    alarm(120);
                    ok = ft_run(e, a, ia, orig, io, n, h, T, kind, rest, k, 1, NULL);
                    checks++; if (kind) spawn_runs++; else alloc_runs++;
                    if (!ok) { fails++; printf("  FAIL  %s%s\n", ft_what, np ? " LOGOS_NO_POOL=1" : ""); }
                }
                if (np) unsetenv("LOGOS_NO_POOL");
                }
            }
        }
    }
#ifndef HARNESS_FAULTS_SPAWN_ONLY
    /* two entries that used to fail SILENTLY when a small list could not be allocated: quantiles returned with its output
       unwritten, and find_outliers returned 0, which reads as 'no outliers'. With EVERY allocation failing, on a view whose
       buckets are still unsorted: the quantiles an independent qsort gives, and SIZE_MAX. */
    { size_t n = 300000; double *ref = malloc(n * sizeof(double));
      if (ref) {
          ft_gen64(orig, n, "uniform"); memcpy(a, orig, n * sizeof(double)); memcpy(ref, orig, n * sizeof(double)); qsort(ref, n, sizeof(double), cmp_d);
          const double qs[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; double got[5] = { -1, -1, -1, -1, -1 }; uint32_t outl[16];
          snprintf(ft_what, sizeof ft_what, "view quantiles and find_outliers with every allocation failing"); alarm(120);
          logos_view *v = logos_view_create_f64(a, n, 4);
          if (v) {
              ft_kind = 0; ft_rest = 1; ft_target = 1; atomic_store(&ft_allocs, 0); atomic_store(&ft_armed, 1);
              logos_view_quantiles(v, qs, 5, got);
              size_t oc = logos_find_outliers_f64(orig, n, outl, 16, 0);
              atomic_store(&ft_armed, 0);
              int ok = 1; for (int i = 0; i < 5; i++) if (got[i] != ref[(size_t)(qs[i] * (double)(n - 1) + 0.5)]) ok = 0;
              checks++; if (!ok) { fails++; printf("  FAIL  view quantiles with every allocation failing\n"); }
              checks++; if (oc != SIZE_MAX) { fails++; printf("  FAIL  find_outliers with every allocation failing returned %zu, not SIZE_MAX\n", oc); }
              logos_view_free(v);
          }
          free(ref);
      } }
#endif
    alarm(0); signal(SIGALRM, SIG_DFL);
    logos_release_buffers();
    free(orig); free(a); free(io); free(ia);
    printf("  resource exhaustion: %d checks, %d failures (%ld failed thread creations, %ld failed allocations)\n", checks, fails, spawn_runs, alloc_runs);
    return fails;
}
#else
static int fault_checks(void) { printf("  resource exhaustion: skipped (needs glibc 2.34+; under ASan nothing runs, under TSan the thread-creation half does; `make faultprobe` is the forked version)\n"); return 0; }
#endif

/* LOGOS_HARNESS_ONLY=<part of a section title> runs only the sections whose title contains it, LOGOS_HARNESS_REPEAT=N runs the
   selection N times: the way to chase an intermittent failure without paying for the whole harness on every try. */
typedef struct { const char *title; int (*fn)(void); } section_t;
static int many_and_boundaries(void) { return boundary_checks() + many_runs_checks(); }
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);      /* redirected output is block-buffered: a crash used to eat the last 4 KB and point at the wrong section */
    printf("logos-sort C harness — reference oracle: stdlib qsort\n\n");
    static const section_t S[] = {
        { "correctness: logos vs qsort", correctness }, { "parity extras", extras_checks }, { "sorted prefix + tail", tail_checks }, { "runs of equal values", rle_checks }, { "cuts: windows in order", cuts_checks }, { "other fields", other_fields_checks }, { "dominant-value route", domroute_checks }, { "half the scratch", half_checks }, { "lattice entry", lat1_checks }, { "strand gate", nsstrand_checks }, { "thread boost", boost_checks }, { "hash map cap", mccap_checks }, { "heavy values", heavy_checks }, { "argsort mt", argsort_mt_checks },
        { "interleaved streams", streams_checks }, { "knot route", knot_checks }, { "twist", twist_checks }, { "linked list", list_checks }, { "gallop", gallop_checks }, { "vector merge", vmerge_checks }, { "mirror", mirror_checks }, { "loop-back", loop_checks }, { "round six", round6_checks }, { "prescan in blocks", prescan_checks }, { "descending ties", desc_tie_checks }, { "short-reach route", reach_checks }, { "key grid", kgrid_checks },
        { "small networks", smallnet_checks }, { "thin front", thin_checks }, { "batch", batch_checks }, { "float32 and lattice vectors", f32lat_checks }, { "in-place partition", ipp_checks }, { "two halves", halves_checks }, { "strings on prefix keys", strings_checks }, { "log-line route", strns_checks }, { "first-partition scan", fuse_checks }, { "low-cardinality vector count", lcvec_checks }, { "parallel prescan", parprescan_checks }, { "mode grid", mdgrid_checks }, { "worker pool", pool_checks }, { "pool trim", trim_checks }, { "IEEE special values", ieee_checks }, { "NaN termination", nan_termination_checks }, { "infinities", infinity_checks }, { "dominant values", dominant_checks },
        { "strict in-place entries", inplace_checks }, { "boundaries", many_and_boundaries }, { "signed zeros", signed_zero_checks },
        { "dense integers", dense_checks }, { "sorted view (f64 and int32)", view_checks }, { "resource exhaustion", fault_checks },
    };
    const char *only = getenv("LOGOS_HARNESS_ONLY"); int repeat = getenv("LOGOS_HARNESS_REPEAT") ? atoi(getenv("LOGOS_HARNESS_REPEAT")) : 1;
    if (repeat < 1) repeat = 1;
    int fails = 0;
    for (int r = 0; r < repeat; r++) for (size_t i = 0; i < sizeof S / sizeof *S; i++) {
        if (only && !strstr(S[i].title, only)) continue;
        if (i) printf("== %s ==\n", S[i].title);              /* the first section prints its own two titles */
        fails += S[i].fn();
    }
    if (only) { printf("\n%s (sections matching '%s', %d time(s))\n", fails ? "RESULT: CORRECTNESS FAILURES" : "RESULT: selected checks passed", only, repeat); return fails ? 1 : 0; }
    printf("\n");
    if (!getenv("LOGOS_SKIP_BENCH")) benchmark();      /* =1: correctness only, which is what a sanitizer run wants */
    printf("\n%s\n", fails ? "RESULT: CORRECTNESS FAILURES — do not ship" : "RESULT: all correctness checks passed");
    return fails ? 1 : 0;
}
