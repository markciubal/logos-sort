/* boundprobe.cpp -- where does the engine sit against the comparison lower bound, shape by shape? (2026-10-31)
 *
 * A sort that COMPARES needs log2(n!) ~ n log2 n - 1.44 n comparisons in the worst case, and on a given input it needs at least the
 * bits that input carries: with repeated values log2(n! / prod m_i!) (the multiset entropy H_m, Munro-Spira), with long ascending
 * runs sum L_i log2(n / L_i) (the run entropy H_r, the bound natural merge sorts meet), with a few elements out of place
 * log2 C(n, R) + R log2 n where R = n - LIS (H_rem). Each is an UPPER estimate of what this input needs, so the bound used
 * below is their minimum, H. A sort that does not compare is held to none of them: counting, radix and placement by value.
 *
 * Three columns per shape, every one from ADJACENT PAIRS (slopebench's method: both sides move the same number of elements, order
 * alternating, median with p10 / p90), every contender verified on both sizes first:
 *   growth   the engine's cost per element at 16M over its cost at 1M: one 16M sort against sixteen 1M sorts. A sort that
 *            pays log2 n per element grows log2(16M) / log2(1M) = 24 / 20, +20%, plus whatever the memory hierarchy adds; a
 *            sort whose work per element does not depend on n stays near 0 (the counting routes read -2..+1% in slopebench).
 *   std      the same for std::sort: the comparison calibration (+20% at least, it compares on every shape).
 *   rel      the engine on this shape against the engine on uniform doubles of the same size (t_shape / t_uniform). Uniform
 *            carries log2 n bits per element; a shape that carries H << log2 n and still costs ~1.0 here leaves its
 *            structure on the table.
 * and, once, outside the timing: H_m, H_r, H_rem, H, and the engine's cost per element at 16M divided by H (ns per bit).
 *
 * PREDICTIONS, written before the first run (one thread):
 *   uniform, normal            growth +10..+18% (the AVX2 quicksort; under std::sort's because its top levels stream)   std +20..+25%
 *   cardK, k random doubles    growth within +-5% for every k (a fixed k is a fixed H_m: an entropy-optimal sort is flat in n);
 *                              rel RISING with log2 k from k ~ 256 (the AVX2 quicksort, which stops at equal ranges: ~n log2 k),
 *                              k <= 128 flat and low (the hash count). THE GAP THIS ROUND IS LOOKING FOR: k = 1e3..1e5.
 *   lattice, int, dense, blocks, stairs, concat, halves, sorted   growth within +-5%, rel 0.1-0.5
 *   near_sorted                growth +5..+20% (the one buffered route slopebench saw grow faster than std::sort)
 *   sine, short reach          growth +5..+15% (sorts inside pieces and blocks: log2 of a piece, not of n)
 *
 * Usage: ./boundprobe [pairs] [n_lo] [n_hi] [shape filter]      defaults 6, 1048576, 16777216, every shape. One thread always.
 *        ./boundprobe once [n] [shape filter] [P]                sorts each shape once on P threads (default 1; for boundprobe_trace:
 *                                                                which route took it).
 *        BP_A=NAME=VALUE BP_B=NAME=VALUE [BP_THREADS=P] ./boundprobe ab [pairs] [n list] [shape filter]
 *                                                                two settings of the engine on the same input, adjacent, order
 *                                                                alternating: t(A) / t(B), above 1 = B faster (an empty side sets
 *                                                                nothing). Defaults: A = LOGOS_NO_LAT1=1, B = shipped. A lattice
 *                                                                ladder (latK, intK, cents, spills, sentinels) and guards; `fired`
 *                                                                = logos_twist_last(36) after one B-side sort. FINDINGS 3br. */
#include "logos_psort.h"
#include "bench_common.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static const char *SHAPES[] = { "uniform", "normal", "card16", "card64", "card256", "card1024", "card4096", "card16384", "card65536",
                                "card262144", "card1048576", "zipf65536", "lattice30k", "int_wide", "dense_perm", "sorted", "near_sorted",
                                "block64", "stairs20", "concat8", "halves", "sine", "reach1000" };
static const int NSH = (int)(sizeof SHAPES / sizeof SHAPES[0]);

/* k random doubles drawn ONCE per k (the same set at every n), so a fixed-k shape is the same multiset family at both sizes */
static void card_values(std::vector<double> &v, size_t k) { Rng r{0x9E3779B97F4A7C15ULL ^ k}; v.resize(k); for (size_t i = 0; i < k; i++) v[i] = r.f() * 2e9 - 1e9; }
static void gen(const std::string &s, double *a, size_t n, uint64_t seed) {
    Rng r{seed * 2654435761ULL + 12345};
    if (!s.compare(0, 4, "card")) {
        size_t k = strtoull(s.c_str() + 4, NULL, 10); std::vector<double> v; card_values(v, k);
        for (size_t i = 0; i < n; i++) a[i] = v[r.next() % k];
    } else if (s == "zipf65536") {
        size_t k = 65536; std::vector<double> v; card_values(v, k); std::vector<double> cdf(k); double z = 0;
        for (size_t i = 0; i < k; i++) { z += 1.0 / (double)(i + 1); cdf[i] = z; }
        for (size_t i = 0; i < n; i++) { double u = r.f() * z; size_t j = std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin(); a[i] = v[j < k ? j : k - 1]; }
    } else if (s == "uniform") { for (size_t i = 0; i < n; i++) a[i] = r.f() * 2e9 - 1e9; }
    else if (s == "normal") { for (size_t i = 0; i < n; i += 2) { double u1 = r.f() + 1e-300, u2 = r.f(), m = std::sqrt(-2 * std::log(u1));
                                                                  a[i] = m * std::cos(6.283185307179586 * u2); if (i + 1 < n) a[i + 1] = m * std::sin(6.283185307179586 * u2); } }
    else if (s == "lattice30k") { for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % 30000) * 0.25; }
    else if (s == "int_wide") { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(uint32_t)r.next(); }
    else if (s == "dense_perm") { for (size_t i = 0; i < n; i++) a[i] = (double)i; for (size_t i = n - 1; i > 0; i--) std::swap(a[i], a[r.next() % (i + 1)]); }
    else if (s == "sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; }
    else if (s == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
                                   for (size_t q = 0; q < n / 100; q++) std::swap(a[r.next() % n], a[r.next() % n]); }
    else if (s == "block64") { size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> p(nb); for (size_t k = 0; k < nb; k++) p[k] = k;
                               for (size_t k = nb - 1; k > 0; k--) std::swap(p[k], p[r.next() % (k + 1)]);
                               for (size_t i = 0; i < n; i++) a[i] = (double)p[i / blk] * (double)blk + (double)(i % blk) + 0.5; }
    else if (s == "stairs20") { size_t L = n / 20 + 1; for (size_t i = 0; i < n; i++) { size_t b = i / L; a[i] = (double)b * (double)L * 0.9 + (double)(i - b * L) + 0.25; } }
    else if (s == "concat8") { size_t L = (n + 7) / 8; for (size_t i = 0; i < n; i++) a[i] = r.f() * 1e9;
                               for (size_t lo = 0; lo < n; lo += L) std::sort(a + lo, a + (lo + L < n ? lo + L : n)); }
    else if (s == "halves") { for (size_t i = 0; i < n; i++) a[i] = i < n / 2 ? (double)i * 2.0 + 0.25 : (double)(i - n / 2) * 2.0 + 1.25; }
    else if (s == "sine") { double T = (double)n / 32.0, A = 0.6 * T / 2.0;
                            for (size_t i = 0; i < n; i++) a[i] = (double)i + A * std::sin(6.283185307179586 * (double)i / T) + (r.f() * 2.0 - 1.0) * 50.0; }
    else if (s == "reach1000") { for (size_t i = 0; i < n; i++) a[i] = (double)i + (r.f() * 2.0 - 1.0) * 1000.0; }
    /* the ab mode's ladder: K values on a lattice of step 0.25 (latK), K consecutive integers centred on 0 (intK), cents (price1e5),
       and a lattice whose LAST 0.05% is off it (the worst decline: the sample misses those, the map reads almost all of the array) */
    else if (!s.compare(0, 3, "lat") && s.find('_') == std::string::npos) { size_t k = strtoull(s.c_str() + 3, NULL, 10);
                                   for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % k) * 0.25 + 3.0; }
    else if (!s.compare(0, 3, "int") && s.find('_') == std::string::npos) { size_t k = strtoull(s.c_str() + 3, NULL, 10);
                                   for (size_t i = 0; i < n; i++) a[i] = (double)((long long)(r.next() % k) - (long long)(k / 2)); }
    else if (s == "price1e5") { for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % 100000) / 100.0; }
    else if (s == "lat30k_lateoff") { for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % 30000) * 0.25 + (i >= n - n / 2000 ? 0.1 : 0.0); }
    else if (s == "lat30k_off01") { for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % 30000) * 0.25 + (r.next() % 1000 == 0 ? 0.1 : 0.0); }
    else if (s == "int20000_sent") { for (size_t i = 0; i < n; i++) a[i] = (double)(r.next() % 20000);      /* sixteen far sentinels at the end */
                                     for (size_t q = 0; q < 16 && q < n; q++) a[n - 1 - q] = q & 1 ? 1e9 : -1e9; }
    else { fprintf(stderr, "unknown shape %s\n", s.c_str()); exit(2); }
}

/* bits per element this input carries, three upper estimates */
static double lg2fact(double m) { return std::lgamma(m + 1.0) / std::log(2.0); }
static void entropies(const double *a, size_t n, double *hm, double *hr, double *hrem) {
    std::vector<double> s(a, a + n); std::sort(s.begin(), s.end());
    double bits = lg2fact((double)n);
    for (size_t i = 0; i < n;) { size_t j = i + 1; while (j < n && s[j] == s[i]) j++; bits -= lg2fact((double)(j - i)); i = j; }
    *hm = bits / (double)n;
    double rb = 0;                                                              /* maximal non-descending runs */
    for (size_t i = 0; i < n;) { size_t j = i + 1; while (j < n && !(a[j] < a[j - 1])) j++; double L = (double)(j - i); rb += L * std::log2((double)n / L); i = j; }
    *hr = rb / (double)n;
    std::vector<double> tails; tails.reserve(1 << 16);                          /* longest non-decreasing subsequence */
    for (size_t i = 0; i < n; i++) { auto it = std::upper_bound(tails.begin(), tails.end(), a[i]); if (it == tails.end()) tails.push_back(a[i]); else *it = a[i]; }
    double R = (double)(n - tails.size());
    *hrem = (R > 0 ? (lg2fact((double)n) - lg2fact(R) - lg2fact((double)n - R)) + R * std::log2((double)n) : 0.0) / (double)n;
}

typedef void (*sorter_t)(double *, size_t);
static int g_threads = 1;                                                       /* the ab mode's BP_THREADS; the table is one thread */
static void s_logos(double *a, size_t n) { logos_sort_f64(a, n, g_threads); }
static void s_std(double *a, size_t n) { std::sort(a, a + n); }
static uint64_t mix64(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static uint64_t bag(const double *a, size_t n) { uint64_t h = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); h += mix64(u); } return h; }
static bool verify(sorter_t f, const double *base, double *w, size_t n) {
    memcpy(w, base, n * 8); uint64_t h = bag(base, n); f(w, n);
    for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) return false;
    return bag(w, n) == h;
}
static double median(std::vector<double> &v, double *p10, double *p90) {
    std::sort(v.begin(), v.end()); size_t m = v.size();
    if (p10) *p10 = v[(size_t)(0.10 * (double)m)]; if (p90) *p90 = v[(size_t)(0.90 * (double)m) < m ? (size_t)(0.90 * (double)m) : m - 1];
    return (m & 1) ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
}
/* t(one n_hi sort) / t(K sorts of n_lo), adjacent, order alternating: the growth of the cost per element */
static double growth(sorter_t f, const double *hi, const double *lo, double *w, size_t nlo, size_t nhi, int pairs, double *p10, double *p90) {
    size_t K = nhi / nlo; std::vector<double> r;
    for (int p = 0; p < pairs; p++) {
        double th = 0, tl = 0;
        for (int side = 0; side < 2; side++) {
            bool big = (side == 0) == ((p & 1) == 0);
            if (big) { memcpy(w, hi, nhi * 8); double t0 = nowms(); f(w, nhi); th = nowms() - t0; }
            else { memcpy(w, lo, K * nlo * 8); double t0 = nowms(); for (size_t j = 0; j < K; j++) f(w + j * nlo, nlo); tl = nowms() - t0; }
        }
        r.push_back(th / tl);
    }
    return median(r, p10, p90);
}
/* t(f on x) / t(f on y), adjacent, order alternating; also returns the median ns per element on x */
static double rel(sorter_t f, const double *x, const double *y, double *w, size_t n, int pairs, double *ns_x, double *p10, double *p90) {
    std::vector<double> r, tx;
    for (int p = 0; p < pairs; p++) {
        double a = 0, b = 0;
        for (int side = 0; side < 2; side++) {
            bool first = (side == 0) == ((p & 1) == 0);
            memcpy(w, first ? x : y, n * 8); double t0 = nowms(); f(w, n); double dt = nowms() - t0;
            if (first) a = dt; else b = dt;
        }
        r.push_back(a / b); tx.push_back(a * 1e6 / (double)n);
    }
    if (ns_x) *ns_x = median(tx, NULL, NULL);
    return median(r, p10, p90);
}

/* "NAME=VALUE" (or empty: nothing set) for one side of the ab mode */
static void side_env(const char *spec, const char *other) {
    if (other && *other) { std::string o(other); size_t e = o.find('='); unsetenv(o.substr(0, e).c_str()); }
    if (spec && *spec) { std::string s(spec); size_t e = s.find('='); setenv(s.substr(0, e).c_str(), e == std::string::npos ? "1" : s.substr(e + 1).c_str(), 1); }
}
static const char *AB_SHAPES[] = { "lat8192", "lat16384", "lat30000", "lat65536", "lat131072", "lat250000", "int20000", "int100000", "price1e5",
                                   "lat30k_lateoff", "lat30k_off01", "int20000_sent", "uniform", "normal", "int_wide", "card16384", "near_sorted", "reach1000", "int1000000" };
/* ./boundprobe ab [pairs] [n list, comma-separated] [shape filter], BP_A / BP_B = "NAME=VALUE" or empty: t(A) / t(B) on the same input,
   adjacent, order alternating; above 1 = B is faster. `fired` = logos_twist_last(36) after one B-side sort (the one-thread lattice entry). */
static int ab_main(int argc, char **argv) {
    int pairs = argc > 2 ? atoi(argv[2]) : 20; const char *nl = argc > 3 ? argv[3] : "1048576,4194304,16777216"; const char *filt = argc > 4 ? argv[4] : NULL;
    const char *A = getenv("BP_A"), *B = getenv("BP_B"); if (!A) A = "LOGOS_NO_LAT1=1"; if (!B) B = "";
    if (getenv("BP_THREADS")) g_threads = atoi(getenv("BP_THREADS")); if (g_threads < 1) g_threads = 1;
    std::vector<size_t> ns; for (const char *p = nl; *p;) { ns.push_back(strtoull(p, (char **)&p, 10)); if (*p == ',') p++; }
    size_t nmax = 0; for (size_t v : ns) if (v > nmax) nmax = v;
    std::vector<double> base(nmax), w(nmax);
    printf("# boundprobe ab: %d thread(s), %d pairs; A = \"%s\", B = \"%s\"; cell t(A) / t(B) [p10 p90], above 1 = B faster\n", g_threads, pairs, A, B);
    for (size_t n : ns) {
        gen("uniform", base.data(), n, 99);
        { double best = 1e30; for (int r = 0; r < 3; r++) { memcpy(w.data(), base.data(), n * 8); double t0 = nowms(); logos_sort_f64(w.data(), n, g_threads); double dt = nowms() - t0; if (dt < best) best = dt; }
          printf("# n = %zu, calibration: uniform %.2f ns per element\n", n, best * 1e6 / (double)n); }
        for (const char *sh : AB_SHAPES) {
            if (filt && !strstr(sh, filt)) continue;
            gen(sh, base.data(), n, 7);
            side_env(A, B); bool okA = verify(s_logos, base.data(), w.data(), n);
            side_env(B, A); logos_twist_last(36); bool okB = verify(s_logos, base.data(), w.data(), n); size_t fired = logos_twist_last(36);
            if (!okA || !okB) { printf("%-15s n=%-9zu VERIFY FAILED (A %d, B %d)\n", sh, n, okA, okB); return 1; }
            std::vector<double> r, ta, tb;
            for (int p = 0; p < pairs; p++) {
                double t[2];
                for (int side = 0; side < 2; side++) {
                    int isA = (side == 0) == ((p & 1) == 0);
                    if (isA) side_env(A, B); else side_env(B, A);
                    memcpy(w.data(), base.data(), n * 8); double t0 = nowms(); logos_sort_f64(w.data(), n, g_threads); t[isA ? 0 : 1] = nowms() - t0;
                }
                r.push_back(t[0] / t[1]); ta.push_back(t[0] * 1e6 / (double)n); tb.push_back(t[1] * 1e6 / (double)n);
            }
            double p10, p90, m = median(r, &p10, &p90), na = median(ta, NULL, NULL), nb = median(tb, NULL, NULL);
            printf("%-15s n=%-9zu A %6.2f ns  B %6.2f ns  A/B %5.3f [%5.3f %5.3f]  fired %zu\n", sh, n, na, nb, m, p10, p90, fired);
            fflush(stdout);
        }
    }
    side_env("", A); side_env("", B);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ab")) return ab_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "once")) {
        size_t n = argc > 2 ? strtoull(argv[2], NULL, 10) : 16777216; const char *filt = argc > 3 ? argv[3] : NULL;
        int P = argc > 4 ? atoi(argv[4]) : 1; if (P < 1) P = 1;
        std::vector<double> a(n);
        for (int s = 0; s < NSH; s++) { if (filt && !strstr(SHAPES[s], filt)) continue;
            gen(SHAPES[s], a.data(), n, 7); fprintf(stderr, "=== %s\n", SHAPES[s]); logos_sort_f64(a.data(), n, P); }
        if (filt) for (const char *sh : AB_SHAPES) { if (!strstr(sh, filt)) continue;   /* the ab ladder's shapes, by name only */
            gen(sh, a.data(), n, 7); fprintf(stderr, "=== %s\n", sh); logos_sort_f64(a.data(), n, P); }
        return 0;
    }
    int pairs = argc > 1 ? atoi(argv[1]) : 6;
    size_t nlo = argc > 2 ? strtoull(argv[2], NULL, 10) : 1048576, nhi = argc > 3 ? strtoull(argv[3], NULL, 10) : 16777216;
    const char *filt = argc > 4 ? argv[4] : NULL;
    if (nhi % nlo) { fprintf(stderr, "n_hi must be a multiple of n_lo\n"); return 2; }
    size_t K = nhi / nlo;
    std::vector<double> hi(nhi), lo(nhi), uni(nhi), w(nhi);
    gen("uniform", uni.data(), nhi, 99);
    { double best = 1e30; for (int r = 0; r < 3; r++) { memcpy(w.data(), uni.data(), nhi * 8); double t0 = nowms(); logos_sort_f64(w.data(), nhi, 1); double dt = nowms() - t0; if (dt < best) best = dt; }
      printf("# boundprobe: one thread, %d pairs, n %zu -> %zu (log2 %.1f -> %.1f: a comparison sort grows +%.0f%% per element)\n",
             pairs, nlo, nhi, std::log2((double)nlo), std::log2((double)nhi), (std::log2((double)nhi) / std::log2((double)nlo) - 1) * 100);
      printf("# calibration: logos on uniform doubles at %zu, %.2f ns per element (quiet box ~10-12 at 16M)\n", nhi, best * 1e6 / (double)nhi); }
    printf("# bits per element the input carries: H_m (repeats), H_r (runs), H_rem (out-of-place), H = their minimum; log2 n = %.1f\n", std::log2((double)nhi));
    printf("%-12s %6s %6s %6s %6s | %8s %6s | %-22s | %-22s | %-22s\n", "shape", "H_m", "H_r", "H_rem", "H", "ns/elem", "ns/bit",
           "growth 1M->16M logos", "growth std::sort", "rel to uniform");
    for (int s = 0; s < NSH; s++) {
        if (filt && !strstr(SHAPES[s], filt)) continue;
        gen(SHAPES[s], hi.data(), nhi, 7);
        for (size_t j = 0; j < K; j++) gen(SHAPES[s], lo.data() + j * nlo, nlo, 1000 + j);
        if (!verify(s_logos, hi.data(), w.data(), nhi) || !verify(s_logos, lo.data(), w.data(), nlo) || !verify(s_std, lo.data(), w.data(), nlo)) {
            printf("%-12s VERIFY FAILED\n", SHAPES[s]); return 1; }
        double hm, hr, hrem; entropies(hi.data(), nhi, &hm, &hr, &hrem); double H = std::min(hm, std::min(hr, hrem));
        double g10, g90, g = growth(s_logos, hi.data(), lo.data(), w.data(), nlo, nhi, pairs, &g10, &g90);
        double s10, s90, sg = growth(s_std, hi.data(), lo.data(), w.data(), nlo, nhi, pairs > 3 ? 3 : pairs, &s10, &s90);
        double ns, r10, r90, rr = rel(s_logos, hi.data(), uni.data(), w.data(), nhi, pairs, &ns, &r10, &r90);
        printf("%-12s %6.2f %6.2f %6.2f %6.2f | %8.2f %6.2f | %+5.1f%% [%+5.1f %+5.1f] | %+5.1f%% [%+5.1f %+5.1f] | %5.3f [%5.3f %5.3f]\n",
               SHAPES[s], hm, hr, hrem, H, ns, H > 0.01 ? ns / H : 0.0, (g - 1) * 100, (g10 - 1) * 100, (g90 - 1) * 100,
               (sg - 1) * 100, (s10 - 1) * 100, (s90 - 1) * 100, rr, r10, r90);
        fflush(stdout);
    }
    return 0;
}
