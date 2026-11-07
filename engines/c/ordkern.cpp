/* ordkern: WHERE is the vector quicksort's order cost? (2026-10-20, the second invented round)
 *
 * orderprobe found four shapes that sort SLOWER as they lie than as the same values shuffled at one thread, all of them through the
 * AVX2 quicksort: sine_slope (the open 0i (f): 1.17x inside the kernel, two diagnoses failed -- the leaf choice, the pivot sample),
 * zipper (an ascending and a descending sequence interleaved, 1.17), groups16 (runs of 16 equal values, 1.20; now the run-length
 * route's) and coarse64k (sorted by 64k windows, random inside each; 1.10 once the AVX2 kernel takes it). Here the KERNELS ALONE: the
 * in-house quicksort (logos_av_qsort_hook) and x86-simd-sort's (built -mavx2 -mfma, the engine's own target), each on a shape and on
 * its own values shuffled, paired and alternated. Printed per kernel: t(shape) / t(shuffled), and the in-house kernel's elements
 * partitioned per element (AV_STATS) for both, against log2(n / 64): a pivot that goes wrong on ordered input shows as MORE levels,
 * a per-level cost as the same levels at a higher price.
 * PREDICTION, written first: the library pays the same order cost (it is the same design), and the levels are the same on both
 * inputs, so the cost is per level: a memory effect of partitioning ordered data in place, not the pivot.
 *   ./ordkern [pairs] [n] */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>
extern "C" int logos_av_qsort_hook(double *a, size_t n);
extern "C" size_t logos_av_stat_elems(void);
extern "C" void xss_base_qsort(double *a, size_t n);

static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double u01() { return (double)(nr() >> 11) / 9007199254740992.0; }
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void gen(int s, double *a, size_t n) {
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; break;                                            /* uniform: the null */
    case 1: { double T = (double)n / 32.0, A = 0.6 * T / 2.0;                                                       /* sine_slope */
        for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (u01() * 2.0 - 1.0) * 50.0; break; }
    case 2: for (size_t i = 0; i < n; i++) a[i] = (i & 1) ? (double)n - (double)(i / 2) - 0.25 : (double)(i / 2) + 0.25; break;   /* zipper */
    case 3: { double v = 0; for (size_t i = 0; i < n; i++) { if (i % 16 == 0) v = u01() * 1e9; a[i] = v; } break; }                /* groups16 */
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 65536) * 65536.0 + u01() * 65536.0; break;        /* coarse64k */
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)i + (u01() - 0.5) * 2000.0; break;                     /* noise +-1000 on a slope */
    case 6: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = 0; i + 1 < n; i += 2) std::swap(a[i], a[i + 1]); break;   /* pairs swapped */
    }
}
static const char *SN[] = { "uniform (null)", "sine_slope", "zipper", "groups16", "coarse64k", "slope + noise 1000", "adjacent pairs swapped" };
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    std::vector<double> a(n), s(n), w(n), r(n);
    double lv = log2((double)n / 64.0);
    printf("# ordkern: the kernels alone, one thread, n = %zu, %d pairs. Columns: t(shape) / t(its values shuffled), p10-p90 (above 1: the\n"
           "# order costs); then the in-house kernel's elements partitioned per element, shape and shuffled (ideal log2(n/64) = %.1f).\n", n, pairs, lv);
    printf("%-24s %-24s %-24s %s\n", "shape", "in-house", "x86-simd-sort", "levels shape / shuffled");
    for (int sh = 0; sh < 7; sh++) {
        RS = 0x9E3779B97F4A7C15ULL + (uint64_t)sh * 7919;
        gen(sh, a.data(), n); s = a; for (size_t i = n - 1; i > 0; i--) std::swap(s[i], s[nr() % (i + 1)]);
        /* verify both kernels on both inputs against std::sort */
        r = a; std::sort(r.begin(), r.end());
        int bad = 0;
        for (int k = 0; k < 2; k++) for (int x = 0; x < 2; x++) { w = x ? s : a; if (k) xss_base_qsort(w.data(), n); else logos_av_qsort_hook(w.data(), n); if (w != r) bad = 1; }
        if (bad) { printf("%-24s MISMATCH\n", SN[sh]); continue; }
        (void)logos_av_stat_elems(); w = a; logos_av_qsort_hook(w.data(), n); double lvs = (double)logos_av_stat_elems() / (double)n;
        w = s; logos_av_qsort_hook(w.data(), n); double lvr = (double)logos_av_stat_elems() / (double)n;
        printf("%-24s", SN[sh]);
        for (int k = 0; k < 2; k++) {
            std::vector<double> rt(pairs);
            for (int i = 0; i < pairs; i++) {
                double ta = 0, tb = 0;
                for (int q = 0; q < 2; q++) { int afirst = (i & 1) == 0, doa = (q == 0) == afirst;
                    w = doa ? a : s; double t0 = now_ms(); if (k) xss_base_qsort(w.data(), n); else logos_av_qsort_hook(w.data(), n); double dt = now_ms() - t0;
                    if (doa) ta = dt; else tb = dt; }
                rt[i] = ta / tb;
            }
            std::sort(rt.begin(), rt.end());
            char cell[64]; snprintf(cell, sizeof cell, "%.3f (%.2f-%.2f)", rt[pairs / 2], rt[pairs / 10], rt[pairs * 9 / 10]);
            printf(" %-24s", cell);
        }
        printf(" %.2f / %.2f\n", lvs, lvr); fflush(stdout);
    }
    return 0;
}
