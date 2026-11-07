/* What makes each sort fast? logos, x86-simd-sort and IPS4o, by controlled experiment (2026-09-28).
 *
 * This machine exposes NO hardware counters (make pmuprobe: every PERF_TYPE_HARDWARE event fails to open under WSL2), so
 * instructions, branch misses and cache misses cannot be counted. What can be done with a clock is to change ONE
 * property of the input and watch how each sort's time per element responds; the response names the mechanism.
 *
 *   ORDER    the same multiset in five orders. A sort that reads order (runs, blocks) collapses on the ordered rows;
 *            one that only partitions by value does not care; one whose branches are predicted speeds up when sorted.
 *   SIZE     uniform data from 4k to 16M. Cost per element against log2(n): a comparison sort grows with the depth of
 *            its recursion, an addressing sort with how many of its passes leave the cache.
 *   DUPES    2 to 65536 distinct values, and all distinct. Who turns repeats into less work.
 *   SHAPE    uniform, exponential tail, two clusters, a lattice of step 0.01. Who depends on the value distribution.
 *   THREADS  1, 2, 4, 8, 14 on uniform and on shuffled sorted blocks: speedup and the Karp-Flatt serial fraction
 *            e = (1/S - 1/P) / (1 - 1/P), the share of the work that did not get faster.
 *
 * The three sorts alternate on identical data inside every cell, the order rotating by repetition; the minimum of the
 * repetitions is kept. Every output is checked: ascending, and the same sum of bit patterns as the input. Rows of one
 * table are minutes apart on a box that drifts, so compare ACROSS a row freely and DOWN a column with care.
 *
 * Build: make whyfast XSS=../x86-simd-sort/src IPS4O=../ips4o/include     usage: ./whyfast [n] [reps] */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>
#include <functional>
#include <omp.h>
#include "x86simdsort-static-incl.h"
#include "ips4o.hpp"
extern "C" {
#include "logos_psort.h"
}

static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / 9007199254740992.0; }
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

enum { K_LOGOS, K_XSS, K_IPS4O, K_N };
static const char *KN[K_N] = { "logos", "x86-simd-sort", "IPS4o" };
static void run(int k, double *a, size_t n, int P) {
    if (k == K_LOGOS) logos_sort_f64(a, n, P);
    else if (k == K_XSS) { omp_set_num_threads(P); x86simdsortStatic::qsort(a, n, false, false); }
    else if (P == 1) ips4o::sort(a, a + n);
    else ips4o::parallel::sort(a, a + n, std::less<double>(), P);
}
static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }

/* one cell: `copies` arrays of n (the same input repeated), each sort timed over all of them; ns per element, minimum of reps */
static bool cell(const std::vector<double> &in, size_t n, int copies, int P, int reps, double out[K_N]) {
    std::vector<double> w(n * (size_t)copies);
    uint64_t want = bitsum(in.data(), n); bool ok = true;
    for (int k = 0; k < K_N; k++) out[k] = 1e300;
    for (int r = 0; r < reps; r++) for (int s = 0; s < K_N; s++) {
        int k = (s + r) % K_N;
        for (int c = 0; c < copies; c++) memcpy(w.data() + (size_t)c * n, in.data(), n * 8);
        double t0 = now_ms();
        for (int c = 0; c < copies; c++) run(k, w.data() + (size_t)c * n, n, P);
        double ns = (now_ms() - t0) * 1e6 / ((double)n * copies);
        if (ns < out[k]) out[k] = ns;
        for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { ok = false; break; }
        if (bitsum(w.data(), n) != want) ok = false;
    }
    return ok;
}
static void head(const char *title, const char *first) {
    printf("\n%s\n  %-34s %14s %14s %14s\n", title, first, KN[0], KN[1], KN[2]);
}
static void line(const char *name, const double v[K_N], bool ok, const double *base) {
    printf("  %-34s", name);
    for (int k = 0; k < K_N; k++) { if (base) printf(" %7.2f (%4.2f)", v[k], v[k] / base[k]); else printf(" %14.2f", v[k]); }
    printf("%s\n", ok ? "" : "   WRONG OUTPUT"); fflush(stdout);
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? strtoull(argv[1], 0, 10) : 8388608;
    int reps = argc > 2 ? atoi(argv[2]) : 5; if (reps < 3) reps = 3;
    printf("WHAT MAKES EACH SORT FAST: ns per element, minimum of %d repetitions, n = %zu unless the row says otherwise.\n"
           "In brackets: the cell over the FIRST row of its table, per sort. Every output verified.\n", reps, n);
    std::vector<double> u(n), a(n); double v[K_N], base[K_N];
    RS = 12345; for (auto &x : u) x = fr() * 1e9;
    std::vector<double> sorted = u; std::sort(sorted.begin(), sorted.end());

    /* ---- ORDER: one multiset, five orders, one thread ---- */
    head("ORDER (one thread): the same values, arranged differently", "arrangement");
    bool ok = cell(u, n, 1, 1, reps, base); line("shuffled", base, ok, base);
    ok = cell(sorted, n, 1, 1, reps, v); line("sorted", v, ok, base);
    a = sorted; std::reverse(a.begin(), a.end()); ok = cell(a, n, 1, 1, reps, v); line("reversed", v, ok, base);
    a = sorted; RS = 7; for (size_t k = 0; k < n / 100; k++) { size_t i = nr() % n, j = nr() % n; std::swap(a[i], a[j]); }
    ok = cell(a, n, 1, 1, reps, v); line("sorted, 1% of elements swapped far", v, ok, base);
    { size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
      RS = 9; for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
      size_t o = 0; for (size_t k = 0; k < nb; k++) { size_t s = perm[k] * blk, e = std::min(n, s + blk); for (size_t i = s; i < e; i++) a[o++] = sorted[i]; } }
    ok = cell(a, n, 1, 1, reps, v); line("64 sorted blocks, shuffled", v, ok, base);

    /* ---- SIZE: uniform, one thread ---- */
    printf("\nSIZE (one thread, uniform): ns per element, and in brackets ns per element per log2(n)\n  %-34s %14s %14s %14s\n", "n", KN[0], KN[1], KN[2]);
    for (size_t m : { (size_t)4096, (size_t)65536, (size_t)1048576, (size_t)16777216 }) {
        if (m > n * 2) continue;
        std::vector<double> x(m); RS = 100 + m; for (auto &q : x) q = fr() * 1e9;
        int copies = m < 1048576 ? (int)(2097152 / m) : 1;
        ok = cell(x, m, copies, 1, reps, v);
        char nm[48]; snprintf(nm, sizeof nm, "%zu (log2 = %.0f)", m, std::log2((double)m));
        printf("  %-34s", nm); for (int k = 0; k < K_N; k++) printf(" %7.2f (%4.2f)", v[k], v[k] / std::log2((double)m)); printf("%s\n", ok ? "" : "   WRONG OUTPUT"); fflush(stdout);
    }

    /* ---- DUPES ---- */
    head("DUPLICATES (one thread): how many distinct values", "distinct values");
    ok = cell(u, n, 1, 1, reps, base); line("all distinct", base, ok, base);
    for (size_t d : { (size_t)65536, (size_t)256, (size_t)16, (size_t)2 }) {
        RS = 500 + d; for (auto &x : a) x = (double)(nr() % d) * 1000.0 + 0.5;
        ok = cell(a, n, 1, 1, reps, v); char nm[32]; snprintf(nm, sizeof nm, "%zu", d); line(nm, v, ok, base);
    }

    /* ---- SHAPE ---- */
    head("DISTRIBUTION (one thread): how the values are spread", "distribution");
    ok = cell(u, n, 1, 1, reps, base); line("uniform", base, ok, base);
    RS = 31; for (auto &x : a) x = -std::log(1.0 - fr() * 0.999999) * 1e6;                   ok = cell(a, n, 1, 1, reps, v); line("exponential tail", v, ok, base);
    RS = 32; for (auto &x : a) x = (nr() & 1) ? fr() * 1e3 : 1e12 + fr() * 1e3;               ok = cell(a, n, 1, 1, reps, v); line("two narrow clusters, far apart", v, ok, base);
    RS = 33; for (auto &x : a) x = std::floor(fr() * 1e6) * 0.01;                             ok = cell(a, n, 1, 1, reps, v); line("lattice, step 0.01 (1e6 levels)", v, ok, base);

    /* ---- THREADS ---- */
    for (int shape = 0; shape < 2; shape++) {
        std::vector<double> x = u;
        if (shape) { size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
                     RS = 9; for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
                     size_t o = 0; for (size_t k = 0; k < nb; k++) { size_t s = perm[k] * blk, e = std::min(n, s + blk); for (size_t i = s; i < e; i++) x[o++] = sorted[i]; } }
        printf("\nTHREADS, %s: ns per element; in brackets speedup over one thread and the Karp-Flatt serial fraction\n  %-10s %22s %22s %22s\n",
               shape ? "64 sorted blocks shuffled" : "uniform", "P", KN[0], KN[1], KN[2]);
        double one[K_N] = { 0, 0, 0 };
        for (int P : { 1, 2, 4, 8, 14 }) {
            ok = cell(x, n, 1, P, reps, v);
            if (P == 1) for (int k = 0; k < K_N; k++) one[k] = v[k];
            printf("  %-10d", P);
            for (int k = 0; k < K_N; k++) { double S = one[k] / v[k], e = P > 1 ? (1.0 / S - 1.0 / P) / (1.0 - 1.0 / P) : 0.0;
                if (P == 1) printf(" %22.2f", v[k]); else printf(" %8.2f (%4.2fx e=%4.2f)", v[k], S, e); }
            printf("%s\n", ok ? "" : "   WRONG OUTPUT"); fflush(stdout);
        }
    }
    return 0;
}
