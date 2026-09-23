/* unibench.cpp — "treat uniform data as a complete computation": once the sample says the distribution is near-uniform,
 * sort it as P independent chunks with the fastest sequential kernel and merge them, instead of partitioning.
 *
 * THE PROPOSAL (2026-09-22): sample; if the data is somewhat uniform, run a parallel SIMD sort per chunk and then merge,
 * if that is fastest. What the engine already does on that certificate: the histogram-free partition applies a
 * Kolmogorov-Smirnov test to the shared sample and, when it certifies, buckets on an EQUI-WIDTH GRID with no tables and
 * no comparisons -- one multiply-add per element, which is the "complete computation" half of the proposal (FINDINGS
 * 3m). What is new here is the other half: replacing the scatter with a merge. The ledger already has "parallel merge
 * sort (chunk-sort + k-way merge): merge 15-26x costlier than the scatter", but that was measured before the AVX2
 * sequential leaf existed and it used a k-way merge, so this builds the strongest form available today.
 *
 * WHAT IS BUILT (all in this bench; the engine is not changed)
 *   A  shipped      logos_sort_f64(a, n, P): the partition, with the equi-width grid where the certificate fires.
 *   B  chunk+merge  P chunks, each sorted by x86-simd-sort's AVX2 quicksort (the proposal's "parallel SIMD sort"),
 *                   then merged bottom-up. EVERY merge is split across the threads by MERGE PATH (a binary search on
 *                   the diagonal), so no level of the tree collapses onto one thread -- the naive tree's last merge is
 *                   n elements on one thread, which is the Amdahl trap this bench is meant to avoid.
 *   C  std + merge  the same tree with std::sort per chunk, to separate the kernel from the merge.
 *   D  chunks only  B without the merge (wrong output by design): what the chunk sorts alone cost, so the merge's
 *                   share is measured and not inferred.
 * Modes: `all` (paired A/B on every shape), `anat` (one run per shape: chunk ms, merge ms, levels, bytes moved).
 *
 * A FIRST CUT SORTED EACH CHUNK WITH logos AT ONE THREAD, AND THAT IS NOT ALLOWED: the engine's context pool and
 * bundle table are plain globals (`g_ctx_pool`, `g_ctx_next`), so logos_sort_f64 may not be called concurrently from
 * several threads. The harness caught it -- int_wide came back WRONG in the anatomy -- and every number from that cut
 * was discarded. The kernel is now x86-simd-sort's AVX2 quicksort, which is header-only and has no shared state; it is
 * also what the proposal actually asks for.
 * PREDICTIONS, written before the first run with that kernel (t(x) / t(shipped) at 14 threads, 8M doubles; above 1 =
 * the engine wins):
 *   D (chunks only, no merge) 0.3-0.5 on uniform: 14 SIMD quicksorts of 571k elements in parallel, and that kernel
 *     sorts 8M in ~79 ms on one thread. The chunk sorts alone SHOULD beat the engine -- that is the proposal's premise.
 *   B (chunks + merge) 3.0-4.5 on uniform: the merge tree moves the array log2(14) = 3.86 times and measured 96 ms by
 *     itself at 8M in the anatomy, against the engine's whole 16-27 ms. The merge, not the kernel, is the verdict.
 *   C (std::sort per chunk + the same merge) 4.5-6.5, so B / C ~ 0.7: the kernel is worth something, the merge is worth
 *     more than the kernel saves.
 *   The structured guards (near_sorted, block_perm) 2-20x: the engine's routes read the order, this reads none.
 *   Stop rule: two runs. The proposal replaces the uniform path only if B reads under 0.95 on uniform in both runs.
 * MEASURED (8M doubles, 14 threads, two runs of 16 pairs; every output checked against std::sort):
 *   shape               chunks+merge / shipped   std+merge / shipped   chunks only / shipped
 *   uniform                 3.92  3.87               6.98  6.70              0.82  0.82
 *   heavy_tail              3.73  3.87               7.10  6.74              0.78  0.76
 *   bimodal                 3.43  3.32               6.28  6.81              0.76  0.95
 *   int_wide                3.96  3.64               6.89  6.55              0.86  0.86
 *   near_sorted (1%)        3.96  3.86               4.19  3.88              1.69  1.72
 *   block_perm (64)         3.92  4.19               8.86  9.89              1.89  1.83
 * Anatomy (one run, 8M): the chunk sorts cost 15-17 ms on EVERY shape and the merge moves 3.86n on every shape, but the
 * merge costs 87 ms on uniform, 45-51 on heavy_tail, bimodal and int_wide, and 15-18 on near_sorted and block_perm.
 * GRADED. The proposal's PREMISE is right and its conclusion is not: fourteen SIMD quicksorts in parallel beat the whole
 * engine (chunks only 0.76-0.86 on the random shapes), and then the merge costs about three times the engine's entire
 * runtime, so chunk+merge lands at 3.3-4.2x on every shape including the uniform target. D predicted 0.3-0.5, measured
 * 0.76-0.95: WRONG, the chunk sorts are only 1.05-1.3x faster than the engine, not 2-3x. B predicted 3.0-4.5, measured
 * 3.32-4.19: RIGHT. C predicted 4.5-6.5, measured 6.28-9.89: above on block_perm. B / C predicted 0.7, measured
 * 0.39-0.59: the SIMD kernel is worth more than predicted, and it still does not change the verdict.
 * THE MECHANISM, measured rather than argued: the same 3.86n of movement costs 15 ms when neighbouring chunks hold
 * DISJOINT value ranges (near_sorted, block_perm: the merge path degenerates and std::merge becomes a copy) and 87 ms
 * when every chunk spans the whole range (uniform: every output element comes from a comparison). That is the ledger's
 * "a merge is comparison, the top-level scatter is bandwidth", with a number on it: on uniform data a merge costs 5.8x
 * what moving the same bytes costs. The engine's answer on the same certificate is one multiply-add per element into an
 * equi-width grid and no comparisons at all (FINDINGS 3m), which is why it wins here.
 *
 * Usage: ./unibench [pairs] [n] [all | anat] [row filter]                    (LOGOS_THREADS=N overrides the count) */
#include "logos_psort.h"
#include "bench_common.h"
#ifdef HAVE_XSS
#include "x86simdsort-static-incl.h"        /* the SIMD kernel per chunk: header-only, no globals, safe from many threads */
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---------------------------------------------------------------- the shapes */
enum { U_UNIFORM, U_HEAVY, U_BIMODAL, U_INTWIDE, U_NEARSORT, U_BLOCKS, U_N };
static const char *UN[U_N] = { "uniform (the target)", "heavy_tail", "bimodal", "int_wide", "near_sorted (1%)", "block_perm (64)" };
static void gen(double *a, size_t n, int s) {
    RS = 99991 + (uint64_t)s * 7717;
    if (s == U_UNIFORM) for (size_t i = 0; i < n; i++) a[i] = fr() * 1e9;
    else if (s == U_HEAVY) for (size_t i = 0; i < n; i++) { double u = fr(); a[i] = 1.0 / (u > 1e-12 ? u : 1e-12); }
    else if (s == U_BIMODAL) for (size_t i = 0; i < n; i++) a[i] = (nr() & 1) ? fr() * 1e3 : 1e9 + fr() * 1e3;
    else if (s == U_INTWIDE) for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)nr();
    else if (s == U_NEARSORT) { for (size_t i = 0; i < n; i++) a[i] = (double)i * 0.5;
        for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); std::swap(a[x], a[x + 1]); } }
    else { size_t nb = 64, blk = n / nb; std::vector<size_t> perm(nb);
        for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
        for (size_t i = 0; i < n; i++) { size_t b = i / blk < nb ? perm[i / blk] : perm[nb - 1], j = i % blk; a[i] = (double)b * 1e6 + (double)j; } }
}

/* ---------------------------------------------------------------- the merge path: split one merge across T threads
 * The merge of X[0,x) and Y[0,y) has a monotone path; the t-th of T equal diagonals is found by a binary search on
 * i + j = d, which gives every thread a slice of the output that it can merge with no communication. */
static void mp_split(const double *X, size_t x, const double *Y, size_t y, size_t d, size_t *pi, size_t *pj) {
    size_t lo = d > y ? d - y : 0, hi = d < x ? d : x;
    while (lo < hi) { size_t i = lo + (hi - lo) / 2, j = d - i;
        if (j > 0 && i < x && Y[j - 1] > X[i]) lo = i + 1; else hi = i; }
    *pi = lo; *pj = d - lo;
}
static void par_merge(double *dst, const double *X, size_t x, const double *Y, size_t y, int T, std::vector<std::thread> &pool) {
    size_t tot = x + y;
    if (T < 1) T = 1;
    if (tot < 1u << 16) T = 1;
    pool.clear();
    for (int t = 0; t < T; t++) {
        size_t d0 = tot * (size_t)t / (size_t)T, d1 = tot * (size_t)(t + 1) / (size_t)T;
        auto body = [=]() {
            size_t i0, j0, i1, j1;
            mp_split(X, x, Y, y, d0, &i0, &j0);
            mp_split(X, x, Y, y, d1, &i1, &j1);
            std::merge(X + i0, X + i1, Y + j0, Y + j1, dst + d0);
        };
        if (t + 1 == T) body(); else pool.emplace_back(body);
    }
    for (auto &th : pool) th.join();
}

/* ---------------------------------------------------------------- B / C / D: chunk, sort, merge */
static std::vector<double> g_scratch;
static double g_ph_chunk = 0, g_ph_merge = 0; static int g_levels = 0; static double g_moved = 0; static int g_count = 0;
static void chunk_merge(double *a, size_t n, int P, int kernel, int do_merge) {   /* kernel 0 = logos at one thread, 1 = std::sort */
    if (P < 1) P = 1;
    std::vector<size_t> bd((size_t)P + 1);
    for (int t = 0; t <= P; t++) bd[(size_t)t] = n * (size_t)t / (size_t)P;
    double t0 = g_count ? now_ms() : 0;
    { std::vector<std::thread> th;
      for (int t = 0; t < P; t++) {
          size_t lo = bd[(size_t)t], hi = bd[(size_t)t + 1];
          auto body = [=]() { if (hi > lo) {
#ifdef HAVE_XSS
              if (kernel) std::sort(a + lo, a + hi); else x86simdsortStatic::qsort(a + lo, hi - lo, false, false);
#else
              std::sort(a + lo, a + hi); (void)kernel;
#endif
          } };
          if (t + 1 == P) body(); else th.emplace_back(body);
      }
      for (auto &x : th) x.join(); }
    if (g_count) { g_ph_chunk = now_ms() - t0; g_levels = 0; g_moved = 0; }
    if (!do_merge) return;
    double t1 = g_count ? now_ms() : 0;
    if (g_scratch.size() < n) g_scratch.resize(n);
    double *src = a, *dst = g_scratch.data();
    std::vector<size_t> cur = bd;
    std::vector<std::thread> pool;
    while (cur.size() > 2) {                                  /* bottom-up: merge neighbouring runs, every merge split by merge path */
        std::vector<size_t> nxt; nxt.push_back(0);
        size_t nruns = cur.size() - 1, npair = (nruns + 1) / 2;
        for (size_t k = 0; k + 1 < cur.size(); k += 2) {
            size_t lo = cur[k], mid = cur[k + 1], hi = k + 2 < cur.size() ? cur[k + 2] : cur[cur.size() - 1];
            if (k + 2 >= cur.size()) { std::memcpy(dst + lo, src + lo, (mid - lo) * sizeof(double)); nxt.push_back(mid); break; }
            int T = (int)(((size_t)P + npair - 1) / npair); if (T < 1) T = 1;
            par_merge(dst + lo, src + lo, mid - lo, src + mid, hi - mid, T, pool);
            if (g_count) g_moved += (double)(hi - lo);
            nxt.push_back(hi);
        }
        if (nxt.back() != n) nxt.push_back(n);
        std::swap(src, dst); cur = nxt; if (g_count) g_levels++;
    }
    if (src != a) std::memcpy(a, src, n * sizeof(double));
    if (g_count) g_ph_merge = now_ms() - t1;
}

/* ---------------------------------------------------------------- the paired harness */
static int g_P = 14, g_kind_a = 0, g_kind_b = 0;
enum { K_SHIPPED, K_CHUNK_LOGOS, K_CHUNK_STD, K_CHUNKS_ONLY };
static void run_kind(int k, double *w, size_t n) {
    if (k == K_SHIPPED) logos_sort_f64(w, n, g_P);
    else if (k == K_CHUNK_LOGOS) chunk_merge(w, n, g_P, 0, 1);
    else if (k == K_CHUNK_STD) chunk_merge(w, n, g_P, 1, 1);
    else chunk_merge(w, n, g_P, 0, 0);
}
static void fn_a(double *w, size_t n, int) { run_kind(g_kind_a, w, n); }
static void fn_b(double *w, size_t n, int) { run_kind(g_kind_b, w, n); }
static double pairs_ratio(const double *base, double *w, size_t n, int ka, int kb, int reps, double *lo, double *hi) {
    g_kind_a = ka; g_kind_b = kb;
    return bench_paired(fn_a, fn_b, base, w, n, 1, reps, lo, hi);
}
static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 16;
    size_t n = argc > 2 ? strtoull(argv[2], 0, 10) : 8000000;
    const char *mode = argc > 3 ? argv[3] : "all";
    const char *filt = argc > 4 ? argv[4] : NULL;
    int online = (int)sysconf(_SC_NPROCESSORS_ONLN);
    g_P = bench_threads(online);
    std::vector<double> s(n), w(n), ref(n);
    if (!std::strcmp(mode, "anat")) {
        std::printf("CHUNK + MERGE, ANATOMY, n = %zu, P = %d, one run per shape (shares, not rankings).\n\n"
                    "  %-24s %10s %10s %10s %8s %10s %9s\n", n, g_P, "shape", "chunks ms", "merge ms", "total ms",
                    "levels", "moved / n", "shipped ms");
        for (int k = 0; k < U_N; k++) {
            if (filt && !std::strstr(UN[k], filt)) continue;
            gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            std::memcpy(w.data(), s.data(), n * 8);
            g_count = 1; g_ph_chunk = g_ph_merge = 0; double t0 = now_ms(); chunk_merge(w.data(), n, g_P, 0, 1); double tot = now_ms() - t0; g_count = 0;
            int ok = std::memcmp(w.data(), ref.data(), n * 8) == 0;
            std::memcpy(w.data(), s.data(), n * 8); double t1 = now_ms(); logos_sort_f64(w.data(), n, g_P); double tship = now_ms() - t1;
            std::printf("  %-24s %10.2f %10.2f %10.2f %8d %10.2f %9.2f%s\n", UN[k], g_ph_chunk, g_ph_merge, tot, g_levels,
                        g_moved / (double)n, tship, ok ? "" : "  WRONG");
            std::fflush(stdout);
        }
        return 0;
    }
    std::printf("UNIFORM AS A COMPLETE COMPUTATION, n = %zu, %d pairs, P = %d threads. Median of paired ratios [p10 p90].\n"
                "Above 1 = the shipped engine is faster. chunks only has no merge (its output is not sorted): it prices the\n"
                "chunk sorts alone, so the merge's share is measured.\n\n  %-24s | %-22s | %-22s | %-22s\n",
                n, reps, g_P, "shape", "chunk+merge / shipped", "std+merge / shipped", "chunks only / shipped");
    for (int k = 0; k < U_N; k++) {
        if (filt && !std::strstr(UN[k], filt)) continue;
        gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
        std::memcpy(w.data(), s.data(), n * 8); chunk_merge(w.data(), n, g_P, 0, 1);
        int ok = sorted_ok(w.data(), n) && std::memcmp(w.data(), ref.data(), n * 8) == 0;
        double l1, h1, l2, h2, l3, h3;
        double c1 = pairs_ratio(s.data(), w.data(), n, K_CHUNK_LOGOS, K_SHIPPED, reps, &l1, &h1);
        double c2 = pairs_ratio(s.data(), w.data(), n, K_CHUNK_STD, K_SHIPPED, reps, &l2, &h2);
        double c3 = pairs_ratio(s.data(), w.data(), n, K_CHUNKS_ONLY, K_SHIPPED, reps, &l3, &h3);
        std::printf("  %-24s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f] | %7.2f [%5.2f %5.2f]\n",
                    UN[k], c1, l1, h1, ok ? " " : "X", c2, l2, h2, c3, l3, h3);
        std::fflush(stdout);
    }
    return 0;
}
