// qpubench.cpp -- does a file of quantum-sourced bytes sort in a different TIME than pseudo-random bytes of the same size?
//
//   qpubench <file> [K prng sets = 400] [rounds = 400]
//
// The file's bytes are read four ways (the same transform is applied to every PRNG set, so the pipelines are identical):
//   u64_unit : 8 bytes -> one double in [0, 1)            (n = bytes / 8)
//   u32      : 4 bytes -> one double, the integer value    (n = bytes / 4)
//   u16      : 2 bytes -> one double                       (n = bytes / 2)
//   bytes    : 1 byte  -> one double, 0..255               (n = bytes)
// Contenders: the file (in TWO slots, A and B, the same data: their difference is the measurement's repeatability),
// K sets from splitmix64 / xorshift and from std::mt19937_64 (half each).
// Method: one sort of one fresh copy per (round, set), the sets visited in a new random order every round, so no
// set is sorted twice in a row and a branch predictor cannot learn one input; per set the MEDIAN over the rounds.
// The file's median is then placed in the distribution of the K PRNG medians: z-score and rank.
// Noise-free companions, per set: the number of comparisons std::sort makes, ascending runs, and inversions.
// Every output is verified (ascending, and the same multiset as the input) before anything is timed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
extern "C" { void logos_sort_f64(double *a, size_t n, int nthreads); }

static double now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static uint64_t SM = 0x9E3779B97F4A7C15ULL;
static uint64_t splitmix() { uint64_t z = (SM += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }

static std::vector<double> view(const std::vector<uint8_t> &b, int w) {
    size_t n = b.size() / w; std::vector<double> a(n);
    for (size_t i = 0; i < n; i++) {
        uint64_t v = 0; memcpy(&v, &b[i * w], w);                       // little-endian
        a[i] = w == 8 ? (double)(v >> 11) / 9007199254740992.0 : (double)v;
    }
    return a;
}
static size_t g_cmp;
static bool counted(double x, double y) { g_cmp++; return x < y; }
static size_t runs(const std::vector<double> &a) { size_t r = 1; for (size_t i = 1; i < a.size(); i++) r += a[i] < a[i - 1]; return r; }
static double inversions(std::vector<double> a) {                          // merge count, O(n log n)
    size_t n = a.size(); std::vector<double> t(n); double inv = 0;
    for (size_t w = 1; w < n; w *= 2) for (size_t lo = 0; lo < n; lo += 2 * w) {
        size_t mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n), i = lo, j = mid, k = lo;
        while (i < mid && j < hi) { if (a[j] < a[i]) { inv += (double)(mid - i); t[k++] = a[j++]; } else t[k++] = a[i++]; }
        while (i < mid) t[k++] = a[i++];
        while (j < hi) t[k++] = a[j++];
        for (size_t q = lo; q < hi; q++) a[q] = t[q];
    }
    return inv;
}
static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); size_t m = v.size(); return m & 1 ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]); }
struct stat_t { double mean, sd, lo, hi; };
static stat_t stats(const std::vector<double> &v) {
    double s = 0, q = 0, lo = v[0], hi = v[0]; for (double x : v) { s += x; lo = std::min(lo, x); hi = std::max(hi, x); }
    double m = s / v.size(); for (double x : v) q += (x - m) * (x - m);
    return { m, sqrt(q / (v.size() - 1)), lo, hi };
}
static void place(const char *what, double qa, double qb, const std::vector<double> &prng, const char *unit) {
    stat_t s = stats(prng); double q = 0.5 * (qa + qb);
    size_t below = 0; for (double x : prng) below += x < q;
    printf("    %-22s file %10.2f / %10.2f %s | PRNG mean %10.2f sd %8.2f [%10.2f .. %10.2f] | z = %+6.2f, rank %zu of %zu (%.1f%%)\n",
           what, qa, qb, unit, s.mean, s.sd, s.lo, s.hi, s.sd > 0 ? (q - s.mean) / s.sd : 0.0, below + 1, prng.size() + 1, 100.0 * (double)below / (double)prng.size());
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: qpubench <file> [K] [rounds]\n"); return 2; }
    int K = argc > 2 ? atoi(argv[2]) : 400, R = argc > 3 ? atoi(argv[3]) : 400;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    std::vector<uint8_t> file; { uint8_t buf[65536]; size_t g; while ((g = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + g); } fclose(f);
    size_t B = file.size() & ~(size_t)7;
    file.resize(B);
    // the byte sets: 0 and 1 the file, 2 .. K+1 PRNG
    std::vector<std::vector<uint8_t>> sets(K + 2, std::vector<uint8_t>(B));
    sets[0] = file; sets[1] = file;
    std::mt19937_64 mt(20261113);
    for (int k = 0; k < K; k++) for (size_t i = 0; i < B; i += 8) { uint64_t v = (k & 1) ? mt() : splitmix(); memcpy(&sets[k + 2][i], &v, 8); }
    { size_t ones = 0; for (uint8_t x : file) ones += __builtin_popcount(x);
      std::vector<double> po; for (int k = 0; k < K; k++) { size_t o = 0; for (uint8_t x : sets[k + 2]) o += __builtin_popcount(x); po.push_back((double)o / (8.0 * B)); }
      printf("%s: %zu bytes; %d PRNG sets of the same size (splitmix64 and mt19937_64), %d rounds, one sort per (round, set), order reshuffled each round\n", argv[1], B, K, R);
      place("share of 1 bits", (double)ones / (8.0 * B), (double)ones / (8.0 * B), po, " "); }
    { double t0 = now_ns(); std::vector<double> c(1 << 20); for (auto &x : c) x = (double)(splitmix() >> 11); std::sort(c.begin(), c.end()); printf("calibration: std::sort of 1M doubles %.1f ms\n\n", (now_ns() - t0) / 1e6 - 0); }

    const int W[4] = { 8, 4, 2, 1 }; const char *WN[4] = { "u64_unit", "u32", "u16", "bytes" };
    for (int wi = 0; wi < 4; wi++) {
        std::vector<std::vector<double>> a(K + 2);
        for (int k = 0; k < K + 2; k++) a[k] = view(sets[k], W[wi]);
        size_t n = a[0].size();
        printf("%s: n = %zu\n", WN[wi], n);
        // verify both sorts on every set
        std::vector<double> w(n), ref(n);
        for (int k = 0; k < K + 2; k++) {
            ref = a[k]; std::sort(ref.begin(), ref.end());
            w = a[k]; logos_sort_f64(w.data(), n, 1);
            if (memcmp(w.data(), ref.data(), n * 8) != 0) { printf("  WRONG OUTPUT from logos on set %d\n", k); return 1; }
        }
        // noise-free statistics
        std::vector<double> cmp(K + 2), rn(K + 2), inv(K + 2);
        for (int k = 0; k < K + 2; k++) { w = a[k]; g_cmp = 0; std::sort(w.begin(), w.end(), counted); cmp[k] = (double)g_cmp; rn[k] = (double)runs(a[k]); inv[k] = inversions(a[k]); }
        place("std::sort comparisons", cmp[0], cmp[1], std::vector<double>(cmp.begin() + 2, cmp.end()), " ");
        place("ascending runs", rn[0], rn[1], std::vector<double>(rn.begin() + 2, rn.end()), " ");
        place("inversions / (n^2/4)", inv[0] / (0.25 * n * n), inv[1] / (0.25 * n * n), [&] { std::vector<double> v; for (int k = 2; k < K + 2; k++) v.push_back(inv[k] / (0.25 * n * n)); return v; }(), " ");
        // timing
        for (int sorter = 0; sorter < 2; sorter++) {
            std::vector<std::vector<double>> t(K + 2); for (auto &v : t) v.reserve(R);
            std::vector<int> ord(K + 2); for (int k = 0; k < K + 2; k++) ord[k] = k;
            for (int r = -3; r < R; r++) {                                 // three warm-up rounds, not recorded
                for (int k = K + 1; k > 0; k--) std::swap(ord[k], ord[splitmix() % (k + 1)]);
                for (int o = 0; o < K + 2; o++) {
                    int k = ord[o]; memcpy(w.data(), a[k].data(), n * 8);
                    double t0 = now_ns();
                    if (sorter == 0) logos_sort_f64(w.data(), n, 1); else std::sort(w.begin(), w.end());
                    double dt = now_ns() - t0;
                    if (r >= 0) t[k].push_back(dt);
                }
            }
            std::vector<double> med(K + 2); for (int k = 0; k < K + 2; k++) med[k] = median(t[k]) / (double)n;
            std::vector<double> pm(med.begin() + 2, med.end()), half1, half2;
            place(sorter == 0 ? "logos, ns per element" : "std::sort, ns per elem", med[0], med[1], pm, "ns");
            // the same test with the two PRNG families against each other: a null that should read |z| small
            for (int k = 0; k < K; k++) (k & 1 ? half2 : half1).push_back(pm[k]);
            stat_t s1 = stats(half1), s2 = stats(half2); double se = sqrt(s1.sd * s1.sd / half1.size() + s2.sd * s2.sd / half2.size());
            printf("    %-22s splitmix sets %.3f, mt19937 sets %.3f ns per element: difference of means %+.2f standard errors\n", "  (null: PRNG vs PRNG)", s1.mean, s2.mean, (s1.mean - s2.mean) / se);
        }
        printf("\n");
    }
    return 0;
}
