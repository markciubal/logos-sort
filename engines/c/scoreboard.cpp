/* scoreboard.cpp -- WHO IS FASTEST, cell by cell (2026-10-07). "Make the fastest sorting algorithm" needs a definition of
 * fastest before it needs code: this is it. Every contender sorts the same input inside one repetition, the order rotating
 * from repetition to repetition, the minimum kept (FINDINGS 3t: the box drifts, so only interleaved contenders compare);
 * every output is verified (ascending under <, and the input's bit patterns as a multiset). Single-threaded contenders are
 * compared only with single-threaded ones, and P-thread with P-thread.
 *
 *   logos      the engine as it ships (in-house AVX2 kernel)                     copy lg0
 *   hybrid     the engine built -DLOGOS_XSS_LEAF (x86-simd-sort under the router)  copy lg1
 *   xss        x86-simd-sort alone (AVX2 here), OpenMP at P threads
 *   vqsort     Google's VQSort (Highway 1.4.0, dynamic dispatch: AVX2 here), ONE thread only: it has no parallel form, so its
 *              P-thread cells read "-" (2026-11-05)
 *   ips4o      IPS4o, sequential at 1 thread, ips4o::parallel at P
 *   std        std::sort at 1 thread, __gnu_parallel::sort at P
 *
 * The last two columns: the best NON-logos time over logos's, and over the hybrid's (> 1: logos wins the cell). A cell is a
 * LOSS when that ratio is under 0.97 (the two logos copies are separate builds, so a few percent is code layout: the pair
 * bench, not this table, settles anything closer).
 *
 * Build: make scoreboard XSS=../x86-simd-sort/src IPS4O=../ips4o/include HWY=../highway     usage: ./scoreboard [n] [reps] [threads] [shape filter] */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <parallel/algorithm>
#include <omp.h>
#include <unistd.h>
#include "x86simdsort-static-incl.h"
#include "ips4o.hpp"
#include "hwy/contrib/sort/vqsort.h"
extern "C" {
void lg0_sort_f64(double *, size_t, int);
void lg1_sort_f64(double *, size_t, int);
}

static uint64_t RS = 7;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / 9007199254740992.0; }
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void gen(const std::string &s, std::vector<double> &a) {
    size_t n = a.size(); RS = 7;
    if (s == "uniform") for (auto &v : a) v = fr() * 2e9 - 1e9;
    else if (s == "heavy_tail") for (auto &v : a) v = 1000.0 * std::pow(1.0 - fr() * 0.999999, -1.0 / 1.2);
    else if (s == "bimodal") for (auto &v : a) v = ((nr() & 1) ? 1e9 : -1e9) + fr() * 10.0;
    else if (s == "clustered") for (auto &v : a) v = (double)(nr() % 16) * 1e8 + fr() * 1e5;
    else if (s == "clustered_8") for (auto &v : a) v = (double)(nr() % 8) * 1e8 + fr() * 1e5;
    else if (s == "clustered_32") for (auto &v : a) v = (double)(nr() % 32) * 1e8 + fr() * 1e5;
    else if (s == "int_wide") for (auto &v : a) v = (double)(int32_t)nr();
    else if (s == "distinct_2") for (auto &v : a) v = (double)(nr() % 2) * 1e6 + 0.5;
    else if (s == "few_distinct") for (auto &v : a) v = (double)(nr() % 5) * 1e6;
    else if (s == "distinct_256") for (auto &v : a) v = (double)(nr() % 256) * 1000.0 + 0.5;
    else if (s == "distinct_65536") for (auto &v : a) v = (double)(nr() % 65536) * 1000.0 + 0.5;
    else if (s == "quantized") for (auto &v : a) v = (double)(nr() % 4096) / 64.0;
    else if (s == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t k = 0; k < n / 100; k++) std::swap(a[nr() % n], a[nr() % n]); }
    else if (s == "sorted") for (size_t i = 0; i < n; i++) a[i] = (double)i * 1.5;
    else if (s == "reversed") for (size_t i = 0; i < n; i++) a[i] = (double)(n - i) * 1.5;
    else if (s == "block_perm" || s == "block_overlap") {
        size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
        for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
            if (s == "block_overlap" && (b & 7) == 1) base -= (double)blk * 0.1;
            a[i] = base + (double)(i % blk) + fr() * 0.5; }
    }
}
static const char *SHAPES[] = { "uniform", "heavy_tail", "bimodal", "clustered", "clustered_8", "clustered_32", "int_wide", "distinct_2", "few_distinct", "distinct_256",
                                "distinct_65536", "quantized", "near_sorted", "sorted", "reversed", "block_perm", "block_overlap" };

enum { C_LOGOS, C_HYBRID, C_XSS, C_VQ, C_IPS4O, C_STD, C_N };
static const char *CN[C_N] = { "logos", "hybrid", "xss", "vqsort", "ips4o", "std" };
static bool runs_at(int k, int P) { return k != C_VQ || P == 1; }
static void run(int k, double *a, size_t n, int P) {
    switch (k) {
    case C_LOGOS:  lg0_sort_f64(a, n, P); break;
    case C_HYBRID: lg1_sort_f64(a, n, P); break;
    case C_XSS:    omp_set_num_threads(P); x86simdsortStatic::qsort(a, n, false, false); break;
    case C_VQ:     hwy::VQSort(a, n, hwy::SortAscending()); break;
    case C_IPS4O:  if (P == 1) ips4o::sort(a, a + n); else ips4o::parallel::sort(a, a + n, std::less<double>(), P); break;
    case C_STD:    if (P == 1) std::sort(a, a + n); else { omp_set_num_threads(P); __gnu_parallel::sort(a, a + n); } break;
    }
}
static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }

int main(int argc, char **argv) {
    /* OpenMP's threads spin after a parallel region by default, and the contender that runs next pays for it: the first board
       had logos (which follows __gnu_parallel::sort in the rotation) 1.4x slower than the hybrid at 14 threads, an engine
       identical to it there, and IPS4o (which follows x86-simd-sort) behind its own figures. libgomp reads the policy at load,
       so it is set here by re-executing; every contender then starts on a quiet machine. */
    if (!getenv("OMP_WAIT_POLICY")) { setenv("OMP_WAIT_POLICY", "PASSIVE", 1); execv("/proc/self/exe", argv); perror("execv"); return 1; }
    size_t n = argc > 1 ? strtoull(argv[1], 0, 10) : 8388608;
    int reps = argc > 2 ? atoi(argv[2]) : 7; if (reps < 3) reps = 3;
    int Pmax = argc > 3 ? atoi(argv[3]) : 14;
    const char *filt = argc > 4 && argv[4][0] ? argv[4] : NULL;
    std::vector<double> in(n), w(n);
    { std::vector<double> c(1 << 20); RS = 1; for (auto &x : c) x = fr(); double t = now_ms(); std::sort(c.begin(), c.end());
      printf("scoreboard: n = %zu, minimum of %d interleaved repetitions, every output verified; calibration std::sort-1M %.1f ms; OMP_WAIT_POLICY=%s\n", n, reps, now_ms() - t, getenv("OMP_WAIT_POLICY")); }
    printf("ns per element; last columns: best non-logos / logos, / hybrid (> 1 = logos wins; LOSS under 0.97)\n");
    for (int P : { 1, Pmax }) {
        printf("\n%d thread%s\n  %-15s %9s %9s %9s %9s %9s %9s   %-8s %7s %7s\n", P, P > 1 ? "s" : "", "shape", CN[0], CN[1], CN[2], CN[3], CN[4], P > 1 ? "gnu_par" : "std", "best", "/logos", "/hybrid");
        for (const char *s : SHAPES) {
            if (filt && !strstr(s, filt)) continue;
            gen(s, in);
            uint64_t want = bitsum(in.data(), n);
            double best[C_N]; for (auto &b : best) b = 1e300;
            bool ok[C_N]; for (auto &o : ok) o = true;
            for (int r = 0; r < reps; r++) for (int q = 0; q < C_N; q++) {
                int k = (q + r) % C_N;
                if (!runs_at(k, P)) continue;
                memcpy(w.data(), in.data(), n * 8);
                double t = now_ms(); run(k, w.data(), n, P); double ns = (now_ms() - t) * 1e6 / (double)n;
                if (ns < best[k]) best[k] = ns;
                if (r == 0) { for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { ok[k] = false; break; } if (bitsum(w.data(), n) != want) ok[k] = false; }
            }
            int bo = C_XSS; for (int k = C_XSS; k < C_N; k++) if (runs_at(k, P) && best[k] < best[bo]) bo = k;
            double rl = best[bo] / best[C_LOGOS], rh = best[bo] / best[C_HYBRID];
            printf("  %-15s", s); for (int k = 0; k < C_N; k++) { if (!runs_at(k, P)) { printf(" %8s ", "-"); continue; } printf(" %8.2f%s", best[k], ok[k] ? " " : "!"); }
            printf("   %-8s %7.3f %7.3f%s%s\n", CN[bo], rl, rh, rl < 0.97 ? "  LOSS(logos)" : "", rh < 0.97 ? "  LOSS(hybrid)" : "");
            fflush(stdout);
        }
    }
    printf("\n! = WRONG OUTPUT\n");
    return 0;
}
