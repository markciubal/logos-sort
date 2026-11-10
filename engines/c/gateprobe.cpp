/* gateprobe.cpp -- two engine settings paired on the practical counting shapes (the scale bench's own generators), one thread or P.
 * GP_A / GP_B = "NAME=VALUE" or empty; cell t(A) / t(B), above 1 = B faster. ./gateprobe [pairs] [n list] [filter]; GP_THREADS=P.
 * Every output is checked (ascending, same multiset as std::sort) under both settings before it is timed. */
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
extern "C" {
#include "logos_psort.h"
}
struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double gauss(Rng &r) { double u = r.f(), v = r.f(); if (u < 1e-300) u = 1e-300; return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v); }
static size_t zipf_rank(Rng &r, size_t V) { double k = std::exp(r.f() * std::log((double)V + 1.0)) - 1.0; size_t z = (size_t)k; return z >= V ? V - 1 : z; }
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void gen(const std::string &shape, std::vector<double> &a, size_t n, uint64_t seed) {
    Rng r{ seed * 0x9E3779B97F4A7C15ull + 88172645463325252ull }; a.resize(n);
    if (shape == "zip_codes") { const size_t Z = 42000; std::vector<uint32_t> all(99450); for (size_t k = 0; k < all.size(); k++) all[k] = (uint32_t)(501 + k);
        for (size_t k = 0; k < Z; k++) std::swap(all[k], all[k + r.next() % (all.size() - k)]); for (auto &v : a) v = (double)all[zipf_rank(r, Z)]; }
    else if (shape == "ages") { double cum[101]; double acc = 0; for (int k = 0; k <= 100; k++) { acc += k < 50 ? 1.0 : (101.0 - k) / 51.0; cum[k] = acc; }
        for (auto &v : a) { double u = r.f() * acc; v = (double)(std::upper_bound(cum, cum + 101, u) - cum); if (v > 100) v = 100; } }
    else if (shape == "fk_zipf") { std::vector<double> cust(200000); for (auto &v : cust) v = std::floor(r.f() * 2147483648.0); for (auto &v : a) v = cust[zipf_rank(r, cust.size())]; }
    else if (shape == "prices_cents") for (auto &v : a) { double c = std::floor(std::exp(3.5 + 1.3 * gauss(r)) * 100.0 + 0.5); if (c < 1) c = 1; if (c > 1e7) c = 1e7; v = c / 100.0; }
    else if (shape == "int_zipf_3k") { for (auto &v : a) v = (double)(zipf_rank(r, 3000) * 37 % 65536); }            /* integers, ~3k distinct, skewed */
    else if (shape == "int_eq_2k") { std::vector<double> t(2000); for (auto &v : t) v = std::floor(r.f() * 65536.0); for (auto &v : a) v = t[r.next() % 2000]; }
    else if (shape == "int_eq_1k_wide") { std::vector<double> t(1000); for (auto &v : t) v = std::floor(r.f() * 2147483648.0); for (auto &v : a) v = t[r.next() % 1000]; }
    else if (shape == "real_eq_1k") { std::vector<double> t(1000); for (auto &v : t) v = r.f(); for (auto &v : a) v = t[r.next() % 1000]; }              /* arbitrary doubles: the leaf is the AVX2 quicksort */
    else if (shape == "real_zipf_2k") { std::vector<double> t(2000); for (auto &v : t) v = r.f() * 1e6; for (auto &v : a) v = t[zipf_rank(r, 2000)]; }
    else if (shape == "halves_3k") { for (auto &v : a) v = (double)(zipf_rank(r, 3000) * 37 % 32768) * 0.5; }
    else if (shape == "secs_exp") { for (auto &v : a) v = std::floor(-900.0 * std::log(1.0 - r.f() * 0.999999)); }                                   /* exponential seconds, whole */
    else if (shape == "cents_2dp") { for (auto &v : a) v = std::round(std::exp(3.0 + gauss(r)) * 100.0) / 100.0; }
    else for (auto &v : a) v = r.f();
}
static void side_env(const char *spec, const char *other) {
    if (other && *other) { std::string o(other); size_t e = o.find('='); unsetenv(o.substr(0, e).c_str()); }
    if (spec && *spec) { std::string s(spec); size_t e = s.find('='); setenv(s.substr(0, e).c_str(), e == std::string::npos ? "1" : s.substr(e + 1).c_str(), 1); }
}
static double median(std::vector<double> v, double *p10, double *p90) { std::sort(v.begin(), v.end()); if (p10) *p10 = v[v.size() / 10]; if (p90) *p90 = v[v.size() * 9 / 10]; return v[v.size() / 2]; }
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 40; const char *nl = argc > 2 ? argv[2] : "100000,316228,1000000"; const char *filt = argc > 3 ? argv[3] : NULL;
    const char *A = getenv("GP_A"), *B = getenv("GP_B"); if (!A) A = ""; if (!B) B = "LOGOS_MCGATE_V1=1";
    int T = getenv("GP_THREADS") ? atoi(getenv("GP_THREADS")) : 1; if (T < 1) T = 1;
    const char *SH[] = { "zip_codes", "ages", "fk_zipf", "prices_cents", "int_zipf_3k", "int_eq_2k", "int_eq_1k_wide", "real_eq_1k", "real_zipf_2k", "halves_3k", "secs_exp", "cents_2dp", "uniform" };
    printf("# gateprobe: %d thread(s), %d pairs; A = \"%s\", B = \"%s\"; t(A) / t(B), above 1 = B faster\n", T, pairs, A, B);
    std::vector<double> base, w, ref;
    for (const char *p = nl; *p;) {
        size_t n = strtoull(p, (char **)&p, 10); if (*p == ',') p++;
        int reps = n < 2000000 ? (int)(2000000 / n) : 1;                 /* small n: a batch of distinct instances per timing */
        for (const char *sh : SH) {
            if (filt && !strstr(sh, filt)) continue;
            if (getenv("GP_ONCE")) { std::vector<double> x; gen(sh, x, n, 7); for (int side = 0; side < 2; side++) { side_env(side ? B : A, side ? A : B);
                w = x; fprintf(stderr, "==== %s n=%zu side %s\n", sh, n, side ? "B" : "A"); logos_sort_f64(w.data(), n, T); } continue; }
            std::vector<std::vector<double>> inst(reps); for (int k = 0; k < reps; k++) gen(sh, inst[k], n, 7 + k);
            w.resize(n); int ok = 1;
            for (int side = 0; side < 2; side++) {
                side_env(side ? B : A, side ? A : B);
                w = inst[0]; logos_sort_f64(w.data(), n, T); ref = inst[0]; std::sort(ref.begin(), ref.end());
                if (memcmp(w.data(), ref.data(), n * 8)) ok = 0;
            }
            if (!ok) { printf("%-15s n=%-9zu VERIFY FAILED\n", sh, n); return 1; }
            std::vector<std::vector<double>> work(reps, std::vector<double>(n));
            std::vector<double> r, ta, tb;
            for (int pr = 0; pr < pairs; pr++) {
                double t[2];
                for (int side = 0; side < 2; side++) {
                    int isA = (side == 0) == ((pr & 1) == 0);
                    if (isA) side_env(A, B); else side_env(B, A);
                    for (int k = 0; k < reps; k++) memcpy(work[k].data(), inst[k].data(), n * 8);
                    double t0 = nowms(); for (int k = 0; k < reps; k++) logos_sort_f64(work[k].data(), n, T); t[isA ? 0 : 1] = nowms() - t0;
                }
                r.push_back(t[0] / t[1]); ta.push_back(t[0] * 1e6 / ((double)n * reps)); tb.push_back(t[1] * 1e6 / ((double)n * reps));
            }
            double p10, p90, m = median(r, &p10, &p90);
            printf("%-15s n=%-9zu A %6.2f ns  B %6.2f ns  A/B %5.3f [%5.3f %5.3f]\n", sh, n, median(ta, 0, 0), median(tb, 0, 0), m, p10, p90); fflush(stdout);
        }
    }
    return 0;
}
