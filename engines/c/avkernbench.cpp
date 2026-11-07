/* Where is the in-house AVX2 quicksort's 1.18x gap to x86-simd-sort? (2026-09-29)
 *
 * The kernels alone, no dispatcher: the engine's avx2_qsort_f64 (through -DLOGOS_AV_HOOK) against the library's
 * quicksort compiled TWO ways -- with -march=native, as every bench here builds it, and with plain -mavx2 -mfma, the
 * engine's own target -- so compiler tuning is separated from algorithm. Uniform doubles, one thread, sizes from one
 * base-case network (64) to 8M: a gap that is flat in n lives in the network or a per-call cost, one that grows with
 * n lives in the partition. Paired and alternated inside each cell, median of the ratios; every output verified
 * (ascending, same bit patterns). The engine side is built twice by the Makefile, with and without -DAV_PREFETCH and
 * with and without -DAV_STATS, and the partition count per element is printed against the ideal log2(n / 64).
 *
 * usage: ./avkernbench [pairs]   (run avkernbench_pf for the prefetch build) */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>
#include "x86simdsort-static-incl.h"
extern "C" int logos_av_qsort_hook(double *a, size_t n);
extern "C" void logos_av_net_hook(double *a, size_t n);
extern "C" size_t logos_av_stat_elems(void);
extern "C" void xss_base_qsort(double *a, size_t n);   /* the library, -mavx2 -mfma */

static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }

enum { K_OURS, K_XSS_NATIVE, K_XSS_BASE, K_N };
static void run(int k, double *a, size_t n) {
    if (k == K_OURS) logos_av_qsort_hook(a, n);
    else if (k == K_XSS_NATIVE) x86simdsortStatic::qsort(a, n, false, false);
    else xss_base_qsort(a, n);
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 30; if (pairs < 6) pairs = 6;
    printf("THE KERNELS ALONE, one thread, uniform doubles, %d alternated repetitions per cell, every output verified.\n"
           "ns per element (minimum), and t(ours) / t(library): above 1 = the library is faster.\n\n", pairs);
    /* ---- the base case alone: ranges of 8..64 (what the recursion hands the networks), our network without the NaN scan
       against the library's qsort, which at these sizes goes straight to its own network ---- */
    printf("BASE CASE: our network alone against the library at the same n; t(ours) / t(library), above 1 = library faster\n");
    for (size_t n : { (size_t)8, (size_t)16, (size_t)24, (size_t)33, (size_t)40, (size_t)48, (size_t)56, (size_t)64 }) {
        size_t copies = 4194304 / n, tot = n * copies; std::vector<double> in(tot), w(tot);
        RS = 77 + n; for (auto &x : in) x = (double)(nr() >> 11) / 9007199254740992.0 * 1e9;
        std::vector<double> r; double b0 = 1e300, b1 = 1e300; bool ok = true;
        for (int p = 0; p < pairs; p++) { double t[2];
            for (int s2 = 0; s2 < 2; s2++) { int k = (s2 + p) & 1; memcpy(w.data(), in.data(), tot * 8);
                double t0 = now_ms();
                if (k == 0) for (size_t c = 0; c < copies; c++) logos_av_net_hook(w.data() + c * n, n);
                else for (size_t c = 0; c < copies; c++) x86simdsortStatic::qsort(w.data() + c * n, n, false, false);
                t[k] = (now_ms() - t0) * 1e6 / (double)tot;
                for (size_t c = 0; c < copies && ok; c++) for (size_t i = 1; i < n; i++) if (w[c * n + i] < w[c * n + i - 1]) { ok = false; break; } }
            r.push_back(t[0] / t[1]); if (t[0] < b0) b0 = t[0]; if (t[1] < b1) b1 = t[1]; }
        std::sort(r.begin(), r.end());
        printf("  n = %-4zu ours %6.3f ns/el   library %6.3f   ratio %.3f [%.2f %.2f]%s\n", n, b0, b1, r[r.size() / 2], r[r.size() / 10], r[r.size() * 9 / 10], ok ? "" : "   WRONG OUTPUT");
        fflush(stdout);
    }
    printf("\nWHOLE KERNEL:\n");
    printf("  %-9s %8s %10s %10s   %-18s %-18s  %s\n", "n", "copies", "ours", "lib native", "ours / lib native", "ours / lib -mavx2", "partitioned per element (ideal log2(n/64))");
    for (size_t n : { (size_t)64, (size_t)512, (size_t)4096, (size_t)65536, (size_t)1048576, (size_t)8388608 }) {
        int copies = n < 1048576 ? (int)(8388608 / n) : 1;
        size_t tot = n * (size_t)copies;
        std::vector<double> in(tot), w(tot);
        RS = 1000 + n; for (auto &x : in) x = (double)(nr() >> 11) / 9007199254740992.0 * 1e9;
        std::vector<uint64_t> want(copies); for (int c = 0; c < copies; c++) want[c] = bitsum(in.data() + (size_t)c * n, n);
        double best[K_N]; for (int k = 0; k < K_N; k++) best[k] = 1e300;
        std::vector<double> rn, rb; bool ok = true; double part = 0;
        for (int p = 0; p < pairs; p++) {
            double t[K_N];
            for (int s = 0; s < K_N; s++) {
                int k = (s + p) % K_N;
                memcpy(w.data(), in.data(), tot * 8);
                (void)logos_av_stat_elems();
                double t0 = now_ms();
                for (int c = 0; c < copies; c++) run(k, w.data() + (size_t)c * n, n);
                t[k] = (now_ms() - t0) * 1e6 / (double)tot;
                if (k == K_OURS) part = (double)logos_av_stat_elems() / (double)tot;
                if (t[k] < best[k]) best[k] = t[k];
                for (int c = 0; c < copies && ok; c++) { const double *q = w.data() + (size_t)c * n;
                    for (size_t i = 1; i < n; i++) if (q[i] < q[i - 1]) { ok = false; break; }
                    if (bitsum(q, n) != want[c]) ok = false; }
            }
            rn.push_back(t[K_OURS] / t[K_XSS_NATIVE]); rb.push_back(t[K_OURS] / t[K_XSS_BASE]);
        }
        std::sort(rn.begin(), rn.end()); std::sort(rb.begin(), rb.end());
        auto med = [](std::vector<double> &v) { return v[v.size() / 2]; };
        auto pc = [](std::vector<double> &v, double q) { return v[(size_t)(q * (double)(v.size() - 1))]; };
        char a1[40], a2[40]; snprintf(a1, sizeof a1, "%.3f [%.2f %.2f]", med(rn), pc(rn, .1), pc(rn, .9)); snprintf(a2, sizeof a2, "%.3f [%.2f %.2f]", med(rb), pc(rb, .1), pc(rb, .9));
        double ideal = n > 64 ? std::log2((double)n / 64.0) : 0.0;
        printf("  %-9zu %8d %10.3f %10.3f   %-18s %-18s  %.2f (%.2f)%s\n", n, copies, best[K_OURS], best[K_XSS_NATIVE], a1, a2, part, ideal, ok ? "" : "   WRONG OUTPUT");
        fflush(stdout);
    }
    return 0;
}
