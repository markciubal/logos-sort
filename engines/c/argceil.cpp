/* argceil.cpp -- told ceilings for an argsort that goes through the dispatcher (IDEAS_LOG 22 (c), FINDINGS 3ca).
 * logos_argsort_f64 today is a one-thread index-carrying LSD radix (up to 8 passes over 12-byte pairs, three scratch arrays).
 * Against it, per shape and size, min of reps, every permutation checked (a[p[i]] ascending, p a permutation):
 *   kv      (total-order key, index) pairs through x86-simd-sort's key-value quicksort, one thread and P (its OpenMP form). NOT stable.
 *   sort+id the value sort alone (logos_sort_f64 on a copy): the floor any argsort sits above, at one thread and P
 *   ident   one ascending scan (what a prescan costs when the keys are already in order)
 * ./argceil [n] [reps] [P] [shape filter]. Needs the x86-simd-sort clone: make argceil XSS=../x86-simd-sort/src */
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <omp.h>
#include <unistd.h>
#define XSS_USE_OPENMP 1
#include "x86simdsort-static-incl.h"
extern "C" {
#include "logos_psort.h"
}
struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static size_t zipf_rank(Rng &r, size_t V) { double k = std::exp(r.f() * std::log((double)V + 1.0)) - 1.0; size_t z = (size_t)k; return z >= V ? V - 1 : z; }
static void gen(const std::string &sh, std::vector<double> &a, size_t n) {
    Rng r{ 88172645463325252ull }; a.resize(n);
    if (sh == "uniform") for (auto &v : a) v = r.f();
    else if (sh == "stamps") for (auto &v : a) v = 1.7e9 + std::floor(r.f() * 3.15e7);                 /* a year of whole seconds, random order */
    else if (sh == "fk_zipf") { std::vector<double> c(200000); for (auto &v : c) v = std::floor(r.f() * 2147483648.0); for (auto &v : a) v = c[zipf_rank(r, c.size())]; }
    else if (sh == "few5") for (auto &v : a) v = (double)(r.next() % 5);
    else if (sh == "sorted") for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
    else if (sh == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t q = 0; q < n / 100; q++) std::swap(a[r.next() % n], a[r.next() % n]); }
    else for (auto &v : a) v = std::exp(10.0 * r.f());
}
static inline uint64_t tkey(double x) { uint64_t b; memcpy(&b, &x, 8); return (b >> 63) ? ~b : (b | 0x8000000000000000ull); }
static bool check(const std::vector<double> &a, const uint32_t *p, size_t n) {
    std::vector<uint8_t> seen(n, 0);
    for (size_t i = 0; i < n; i++) { if (p[i] >= n || seen[p[i]]) return false; seen[p[i]] = 1; if (i && a[p[i]] < a[p[i - 1]]) return false; }
    return true;
}
int main(int argc, char **argv) {
    size_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 8388608; int reps = argc > 2 ? atoi(argv[2]) : 5, P = argc > 3 ? atoi(argv[3]) : 14;
    const char *filt = argc > 4 ? argv[4] : NULL;
    if (!getenv("OMP_WAIT_POLICY")) { setenv("OMP_WAIT_POLICY", "PASSIVE", 1); execv("/proc/self/exe", argv); }
    const char *SH[] = { "uniform", "heavy", "stamps", "fk_zipf", "few5", "near_sorted", "sorted" };
    printf("# argceil: n = %zu, min of %d, P = %d; ns per element\n", n, reps, P);
    printf("%-12s %9s %9s %9s %9s %9s %9s %9s %9s   %s\n", "shape", "argsort", "mt 1T", "mt PT", "kv 1T", "kv PT", "sort 1T", "sort PT", "ident", "argsort / mt PT");
    std::vector<double> a, w(n); std::vector<uint32_t> p(n), pref(n); std::vector<uint64_t> k(n), v(n);
    for (const char *sh : SH) {
        if (filt && !strstr(sh, filt)) continue;
        gen(sh, a, n);
        double tA = 1e30, tM1 = 1e30, tMP = 1e30, tK1 = 1e30, tKP = 1e30, tS1 = 1e30, tSP = 1e30, tI = 1e30; bool ok = true;
        for (int r = 0; r < reps; r++) {
            double t0 = nowms(); logos_argsort_f64(a.data(), n, p.data()); tA = std::min(tA, nowms() - t0); if (!r) { ok = ok && check(a, p.data(), n); pref = p; }
            t0 = nowms(); logos_argsort_f64_mt(a.data(), n, p.data(), 1); tM1 = std::min(tM1, nowms() - t0); if (!r) ok = ok && p == pref;
            t0 = nowms(); logos_argsort_f64_mt(a.data(), n, p.data(), P); tMP = std::min(tMP, nowms() - t0); if (!r) ok = ok && p == pref;   /* the SAME permutation: stable */
            for (int par = 0; par < 2; par++) {
                omp_set_num_threads(par ? P : 1);
                t0 = nowms();
                for (size_t i = 0; i < n; i++) { k[i] = tkey(a[i]); v[i] = i; }
                x86simdsortStatic::keyvalue_qsort(k.data(), v.data(), n);
                for (size_t i = 0; i < n; i++) p[i] = (uint32_t)v[i];
                double dt = nowms() - t0; if (par) tKP = std::min(tKP, dt); else tK1 = std::min(tK1, dt);
                if (!r) ok = ok && check(a, p.data(), n);
            }
            memcpy(w.data(), a.data(), n * 8); t0 = nowms(); logos_sort_f64(w.data(), n, 1); tS1 = std::min(tS1, nowms() - t0);
            memcpy(w.data(), a.data(), n * 8); t0 = nowms(); logos_sort_f64(w.data(), n, P); tSP = std::min(tSP, nowms() - t0);
            t0 = nowms(); { size_t d = 0; for (size_t i = 1; i < n; i++) d += a[i] < a[i - 1]; for (size_t i = 0; i < n; i++) p[i] = (uint32_t)i; if (d == n) puts(""); } tI = std::min(tI, nowms() - t0);
        }
        double e = 1e6 / (double)n;
        printf("%-12s %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f   %5.2f %s\n", sh, tA * e, tM1 * e, tMP * e, tK1 * e, tKP * e, tS1 * e, tSP * e, tI * e, tA / tMP, ok ? "" : "CHECK FAILED");
        fflush(stdout);
    }
    return 0;
}
