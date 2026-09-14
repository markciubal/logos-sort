/* parbench.cpp — FAIR fights: single-threaded sorts compared only to each other,
 * multi-threaded sorts compared only to each other. Never single vs multi.
 *
 *   SINGLE-THREADED:  std::sort  ·  logos (1 thread)  ·  LSD radix  ·  timsort  ·  x86-simd-sort (1T)   [with -DHAVE_XSS]
 *   MULTI-THREADED:   __gnu_parallel::sort  ·  logos (P threads)
 *                     ·  logos+SIMD-leaf (P)  ·  x86-simd-sort OpenMP (P)      [with -DHAVE_XSS]
 *
 * Same data per shape; every output verified against std::sort. Best of k.
 * A calibration line flags a loaded machine (discard if it drifts >15%).
 * Build: make parbench   (scalar)   |   make parbench-simd   (adds x86-simd-sort).
 */
#include "logos_psort.h"
#include "bench_common.h"
#include "bench_sorts.h"
#include <algorithm>
#include <parallel/algorithm>
#include <omp.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#ifdef HAVE_XSS
#define XSS_USE_OPENMP 1
#include "x86simdsort-static-incl.h"
#endif
#ifdef HAVE_IPS4O
#include "ips4o.hpp"
#endif

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
template <class T> static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static std::vector<double> gen(const std::string &shape, size_t n) {
    Rng r{7}; std::vector<double> a(n);
    if (shape == "uniform") for (auto &v : a) v = r.f() * 2e9 - 1e9;
    else if (shape == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i; for (size_t k = 0; k < n / 100; k++) std::swap(a[r.next() % n], a[r.next() % n]); }
    else if (shape == "few_distinct") for (auto &v : a) v = (double)(r.next() % 5) * 1e6;
    else if (shape == "heavy_tail") for (auto &v : a) v = 1000.0 * std::pow(1.0 - r.f() * 0.999999, -1.0 / 1.2);
    else if (shape == "bimodal") for (auto &v : a) v = ((r.next() & 1) ? 1e9 : -1e9) + r.f() * 10.0;
    else if (shape == "quantized") for (auto &v : a) v = (double)(r.next() % 4096) / 64.0;
    else if (shape == "int_wide") for (auto &v : a) v = (double)(int32_t)r.next();
    else if (shape == "block_perm" || shape == "block_overlap") {   /* 64 sorted blocks with disjoint value ranges, shuffled; overlap: every 8th value-block starts 10% inside its predecessor */
        size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[r.next() % (k + 1)]);
        for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
            if (shape == "block_overlap" && (b & 7) == 1) base -= (double)blk * 0.1;
            a[i] = base + (double)(i % blk) + r.f() * 0.5; }
    }
    return a;
}
template <class F> static double best(const std::vector<double> &base, int reps, F f, std::vector<double> &out) {
    double b = 1e300;
    for (int i = 0; i < reps; i++) { std::vector<double> a = base; double t = nowms(); f(a); double ms = nowms() - t; if (ms < b) { b = ms; out = a; } }
    return b;
}

int main() {
    int P = bench_threads(logos_num_cpus());   /* bench_common.h: LOGOS_THREADS, else BENCH_THREADS, else physical cores */
    omp_set_num_threads(P);
    const size_t N = 8000000; const int R = 5; int bad = 0;   /* min of 5: make noiseprobe showed best-of-3 absorbs a 5x outlier */
    const char *shapes[] = { "uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide", "block_perm", "block_overlap" };

    printf("logos-sort FAIR fights, n = 8M, best of %d, P = %d threads\n", R, P);
    { std::vector<double> cal(1000000); Rng cr{3}; for (auto &v : cal) v = cr.f(); double bc = 1e300, bs = 1e300, tmp[1] = {0}; (void)tmp;
      std::vector<double> t2(cal.size());
      for (int i = 0; i < 5; i++) { double t = nowms(); for (int k = 0; k < 8; k++) memcpy(t2.data(), cal.data(), cal.size() * 8); double ms = nowms() - t; if (ms < bc) bc = ms; }
      for (int i = 0; i < 3; i++) { std::vector<double> c2 = cal; double t = nowms(); std::sort(c2.begin(), c2.end()); double ms = nowms() - t; if (ms < bs) bs = ms; }
      printf("calibration (1T): memcpy 64MB %.1f ms, std::sort 1M %.1f ms  <- >15%% drift between runs = loaded\n\n", bc, bs); }

    /* -------- SINGLE-THREADED: 1T vs 1T only -------- */
    omp_set_num_threads(1);                      /* x86-simd-sort's OpenMP sort must really run on one thread here */
    logos_set_simd_sort(nullptr, nullptr);
    printf("== SINGLE-THREADED (each column single-threaded; speedups vs std::sort) ==\n");
#ifdef HAVE_XSS
    printf("  %-12s %11s %11s %7s %11s %7s %11s %7s %11s %7s\n", "shape", "std::sort", "logos 1T", "x", "radix", "x", "timsort", "x", "x86simd 1T", "x");
#else
    printf("  %-12s %11s %11s %7s %11s %7s %11s %7s\n", "shape", "std::sort", "logos 1T", "x", "radix", "x", "timsort", "x");
#endif
    for (const char *sh : shapes) {
        auto base = gen(sh, N); std::vector<double> ref = base, o; std::sort(ref.begin(), ref.end());
        double ts = best(base, R, [](std::vector<double> &a) { std::sort(a.begin(), a.end()); }, o);
        double t1 = best(base, R, [](std::vector<double> &a) { logos_sort_f64(a.data(), a.size(), 1); }, o); bool ok = (o == ref);
        double tr = best(base, R, [](std::vector<double> &a) { radix_sort_f64(a.data(), a.size()); }, o); ok &= (o == ref);
        double tt = best(base, R, [](std::vector<double> &a) { timsort_f64(a.data(), a.size()); }, o); ok &= (o == ref);
#ifdef HAVE_XSS
        double tx = best(base, R, [](std::vector<double> &a) { x86simdsortStatic::qsort(a.data(), a.size(), false, false); }, o); ok &= (o == ref);
        printf("  %-12s %9.1fms %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx%s\n", sh, ts, t1, ts / t1, tr, ts / tr, tt, ts / tt, tx, ts / tx, ok ? "" : "  <-- MISMATCH");
#else
        printf("  %-12s %9.1fms %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx%s\n", sh, ts, t1, ts / t1, tr, ts / tr, tt, ts / tt, ok ? "" : "  <-- MISMATCH");
#endif
        if (!ok) bad++;
    }

    /* -------- MULTI-THREADED: P vs P only -------- */
    omp_set_num_threads(P);
    printf("\n== MULTI-THREADED, P=%d (each column parallel; speedups vs __gnu_parallel::sort) ==\n", P);
#ifdef HAVE_XSS
    #ifdef HAVE_IPS4O
    printf("  %-12s %11s %11s %7s %11s %7s %11s %7s %11s %7s\n", "shape", "gnu_par", "logos", "x", "logos+SIMD", "x", "x86simd MT", "x", "ips4o", "x");
#else
    printf("  %-12s %11s %11s %7s %11s %7s %11s %7s\n", "shape", "gnu_par", "logos", "x", "logos+SIMD", "x", "x86simd MT", "x");
#endif
#else
    printf("  %-12s %11s %11s %7s\n", "shape", "gnu_par", "logos", "x");
#endif
    for (const char *sh : shapes) {
        auto base = gen(sh, N); std::vector<double> ref = base, o; std::sort(ref.begin(), ref.end());
        double tg = best(base, R, [](std::vector<double> &a) { __gnu_parallel::sort(a.begin(), a.end()); }, o); bool ok = (o == ref);
        logos_set_simd_sort(nullptr, nullptr);
        double tl = best(base, R, [P](std::vector<double> &a) { logos_sort_f64(a.data(), a.size(), P); }, o); ok &= (o == ref);
#ifdef HAVE_XSS
        logos_set_simd_sort([](double *a, size_t n) { x86simdsortStatic::qsort(a, n, false, false); }, [](int32_t *a, size_t n) { x86simdsortStatic::qsort(a, n); });
        double tls = best(base, R, [P](std::vector<double> &a) { logos_sort_f64(a.data(), a.size(), P); }, o); ok &= (o == ref);
        logos_set_simd_sort(nullptr, nullptr);
        double tx = best(base, R, [](std::vector<double> &a) { x86simdsortStatic::qsort(a.data(), a.size(), false, false); }, o); ok &= (o == ref);  /* OpenMP-parallel when built with XSS_USE_OPENMP */
#ifdef HAVE_IPS4O
        double ti = best(base, R, [](std::vector<double> &a) { ips4o::parallel::sort(a.begin(), a.end()); }, o); ok &= (o == ref);
        printf("  %-12s %9.1fms %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx%s\n", sh, tg, tl, tg / tl, tls, tg / tls, tx, tg / tx, ti, tg / ti, ok ? "" : "  <-- MISMATCH");
#else
        printf("  %-12s %9.1fms %9.1fms %6.2fx %9.1fms %6.2fx %9.1fms %6.2fx%s\n", sh, tg, tl, tg / tl, tls, tg / tls, tx, tg / tx, ok ? "" : "  <-- MISMATCH");
#endif
#else
        printf("  %-12s %9.1fms %9.1fms %6.2fx%s\n", sh, tg, tl, tg / tl, ok ? "" : "  <-- MISMATCH");
#endif
        if (!ok) bad++;
    }

    printf("\nSINGLE columns are all 1 thread; MULTI columns are all %d threads. No single-vs-multi comparison is made.\n", P);
#ifdef HAVE_XSS
    printf("logos+SIMD = logos partition + x86-simd-sort quicksort per bucket. x86simd MT = x86-simd-sort OpenMP (%s kernels on this build). ips4o = In-place Parallel Super-scalar Samplesort (SOTA parallel comparison sort).\n",
#ifdef __AVX512F__
           "AVX-512");
#else
           "AVX2");
#endif
#endif
    printf("\nRESULT: %s\n", bad ? "OUTPUT MISMATCHES — do not trust timings" : "all outputs verified against std::sort");
    return bad ? 1 : 0;
}
