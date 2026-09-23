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
                                                                             sorted-prefix-plus-tail route (prefix >= n/2) does not take it first */
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
            unsetenv("LOGOS_NO_KNOT_GALLOP");
            memcpy(a, orig, n * sizeof(double)); (void)logos_twist_last(5);
            logos_sort_f64(a, n, threads[ti]);
            size_t gon = logos_twist_last(5);
            setenv("LOGOS_NO_KNOT_GALLOP", "1", 1);
            memcpy(b, orig, n * sizeof(double));
            logos_sort_f64(b, n, threads[ti]);
            size_t goff = logos_twist_last(5);
            unsetenv("LOGOS_NO_KNOT_GALLOP");
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
 * sorts, the switches, strays past the n/64 cap, clean shapes). Every row that can names its path through logos_twist_last:
 * 8 = the wave placed it, 9 = the seam rotated it, 10 = strays lifted. Oracle: qsort and a byte
 * compare; the NaN row as the bitwise multiset. ---- */
enum { R6_WAVE, R6_WAVE2, R6_WAVEWIDE, R6_SINE_CLEAN, R6_SEAM, R6_REACH, R6_STRAY01, R6_STRAY1, R6_STRAY3, R6_STRAYNAN, R6_BLOCKS, R6_UNIFORM, R6_ONEKINK, R6_ONEKINKDESC, R6_ONESTRAY, R6_N };
static const char *R6_NAME[R6_N] = { "sine 32 cycles, noise 50", "sine 8 cycles, noise 50", "sine 32 cycles, swing 2 (the cost gate declines it)", "sine 32 cycles, no noise", "short reach, rotated (a noisy ring buffer)",
    "short reach as it lies", "blocks + 0.1% strays", "blocks + 1% strays", "blocks + 3% strays (past the cap)", "blocks + 0.1% strays + a NaN",
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
    case R6_STRAY01: case R6_STRAY1: case R6_STRAY3: case R6_STRAYNAN: case R6_BLOCKS: {
        knot_stairs(a, n, 64, 0.0, 1, 0);
        double f = mode == R6_STRAY01 || mode == R6_STRAYNAN ? 0.001 : mode == R6_STRAY1 ? 0.01 : mode == R6_STRAY3 ? 0.03 : 0.0;
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
    case R6_STRAY01: case R6_STRAY1: return !par ? 1 : 0;
    case R6_STRAYNAN: return -1;
    case R6_ONEKINK: return !par ? 1 : -1;                             /* one thread: one run. P threads: a kink across a detection chunk's end is
                                                                           never repaired, so the strand may come as two runs and take the plan */
    case R6_ONEKINKDESC: return -1;                                     /* strays are lifted from ASCENDING runs only: a descending strand's unrepaired
                                                                           kinks leave ~100 twisted runs, which the knot route unties (crossing cost 312) */
    case R6_ONESTRAY: return !par ? 1 : -1;                             /* strays lifted (witness 10), whether one run is left or two */
    default: return 0;                                                  /* the clean sine (the knot route first), reach as it lies, 3% strays, blocks, noise */
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
       RM_SUBNORMAL, RM_GAP, RM_PLATEAU, RM_SWAPPED, RM_SPARSE, RM_N };
static const char *RM_NAME[RM_N] = { "jitter", "windows", "one stray up", "one stray down", "three strays", "strays everywhere", "strays the probe cannot see",
    "duplicates", "ties at the seams", "signed zeros", "infinities", "a NaN", "subnormals", "quiet hours (two clusters in a block)", "a plateau in a block",
    "two blocks swapped", "sparse disorder" };
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
    case RM_SPARSE: return LOGOS_REACH_DECLINED_GATE;
    default: return -1;
    }
}
static int reach_checks(void) {
    int checks = 0, fails = 0;
    const int threads[] = { 1, 4, 8 };
    const size_t sizes[] = { 70000, 262144, 262145, 300001, 1000003, 3000017 };
    static const char *ST[] = { "not tried", "sorted", "sorted, strays shifted in", "declined at the gate", "declined by the probe", "declined by the plan", "declined: no memory" };
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
                logos_sort_f64(a, n, threads[ti]);
                /* under the route's floor a sort must leave the answer ALONE (no thread-local access on a small sort's path), so the
                 * expectation there is whatever the previous row left, which is a real state for every mode but the first */
                int got = logos_reach_last(), ok = ipc_hash64(a, n) == h, want = n < 262144 ? before : reach_expect(mode, n, threads[ti]);
                if (mode != RM_NAN) for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { ok = 0; break; }
                checks++; if (!ok) { fails++; printf("  FAIL  short reach, %s, n=%zu threads=%d: wrong output (route: %s)\n", RM_NAME[mode], n, threads[ti], ST[got]); }
                checks++; if (want >= 0 && got != want) { fails++; printf("  FAIL  short reach, %s, n=%zu threads=%d: the route reports '%s', this row is here for '%s'\n", RM_NAME[mode], n, threads[ti], ST[got], ST[want]); }
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
                snprintf(ft_what, sizeof ft_what, "%s %s threads=%d unfaulted", FE_NAME[e], sh, T); alarm(120);
                int ok = ft_run(e, a, ia, orig, io, n, h, T, kind, 0, 0, !kind, &K);
                checks++; if (!ok) { fails++; printf("  FAIL  %s\n", ft_what); }
                for (int rest = 0; rest < 2; rest++) for (long k = 1; k <= K; k += step) {
                    snprintf(ft_what, sizeof ft_what, "%s %s threads=%d %s %s k=%ld of %ld", FE_NAME[e], sh, T, kind ? "thread creation" : "allocation", rest ? "rest" : "one", k, K);
                    alarm(120);
                    ok = ft_run(e, a, ia, orig, io, n, h, T, kind, rest, k, !kind, NULL);
                    checks++; if (kind) spawn_runs++; else alloc_runs++;
                    if (!ok) { fails++; printf("  FAIL  %s\n", ft_what); }
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
        { "correctness: logos vs qsort", correctness }, { "parity extras", extras_checks }, { "sorted prefix + tail", tail_checks },
        { "interleaved streams", streams_checks }, { "knot route", knot_checks }, { "twist", twist_checks }, { "linked list", list_checks }, { "gallop", gallop_checks }, { "mirror", mirror_checks }, { "loop-back", loop_checks }, { "round six", round6_checks }, { "prescan in blocks", prescan_checks }, { "descending ties", desc_tie_checks }, { "short-reach route", reach_checks }, { "key grid", kgrid_checks },
        { "small networks", smallnet_checks }, { "IEEE special values", ieee_checks }, { "NaN termination", nan_termination_checks }, { "infinities", infinity_checks }, { "dominant values", dominant_checks },
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
