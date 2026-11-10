// bench_strings.h -- the string side of the benches, shared by stringbench.cpp and scalebench.cpp so that both measure the
// same inputs the same way: generators for eight string shapes, and the 8-byte-prefix scheme driven by any sorter of
// (key, value) pairs (std::sort, or x86-simd-sort's key-value sort when the includer defines BENCH_STRINGS_XSS after
// including x86simdsort-static-incl.h). Header-only; needs OpenMP (-fopenmp) for the parallel prefix driver.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

// ---- data: one pool of NUL-terminated strings, pointers in generation order (as if read from a file) ----
struct StrSet {
    std::vector<char> buf; std::vector<const char *> p;
    void add(const std::string &s, std::vector<size_t> &off) { off.push_back(buf.size()); buf.insert(buf.end(), s.begin(), s.end()); buf.push_back(0); }
};
struct StrRng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / (double)(1ULL << 53); } };
static std::string str_word(uint64_t id) {                 // a pronounceable pseudo-word for each id, 2 to 14 letters
    static const char *syl[] = { "ka", "ri", "to", "mel", "an", "so", "ver", "li", "qu", "ex", "or", "pa", "tes", "ni", "do", "ur", "ba", "ce", "fi", "go" };
    uint64_t x = id * 0x9E3779B97F4A7C15ULL + 7; std::string s; int k = 1 + (int)(x % 6); x /= 6;
    for (int i = 0; i < k; i++) { s += syl[x % 20]; x /= 20; x ^= x >> 7; x *= 0xBF58476D1CE4E5B9ULL; }
    return s;
}
static uint64_t str_zipf(StrRng &r, uint64_t V) { double u = r.f(); return (uint64_t)std::pow((double)V, u * u) - 1; }   // a heavy head, a long tail
// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm), for the `dates` shape
static void str_civil(long z, int &y, unsigned &m, unsigned &d) {
    z += 719468; const long era = (z >= 0 ? z : z - 146096) / 146097; const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; const long yy = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1; m = mp < 10 ? mp + 3 : mp - 9; y = (int)(yy + (m <= 2));
}
// shapes: uuids, random, words, words_sorted, ids, urls, paths, logs, logs8, emails, dates. Deterministic in (shape, n, seed).
// emails and dates (2026-11-08): two columns people sort every day. emails: local part from a skewed vocabulary, sometimes
// dotted or numbered, at a domain drawn with a heavy head (seven webmail providers, then a long tail of company domains).
// dates: ISO-8601 timestamps with milliseconds ("2021-07-14T09:26:53.589Z"), random instants over the ten years from 2016, in
// random order: fixed width, lexicographic order IS chronological order, and the first 8 bytes hold only year and month.
static StrSet gen_strings(const char *shape, size_t n, uint64_t seed = 7) {
    StrSet S; std::vector<size_t> off; off.reserve(n); S.buf.reserve(n * 40 + 1);
    StrRng R{ 0x853C49E6748FEA9BULL ^ (seed * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)strlen(shape) * 0x1234567ULL };
    char tmp[256];
    if (!strcmp(shape, "words") || !strcmp(shape, "words_sorted")) {
        std::vector<std::string> v; v.reserve(n);
        for (size_t i = 0; i < n; i++) v.push_back(str_word(str_zipf(R, 60000)));
        if (!strcmp(shape, "words_sorted")) std::sort(v.begin(), v.end());
        for (auto &s : v) S.add(s, off);
    } else if (!strcmp(shape, "urls")) {
        static const char *tld[] = { ".com", ".org", ".net", ".io" };
        for (size_t i = 0; i < n; i++) {
            std::string s = "https://www." + str_word(str_zipf(R, 5000)) + tld[R.next() % 4] + "/";
            int segs = 1 + (int)(R.next() % 4);
            for (int g = 0; g < segs; g++) { s += str_word(str_zipf(R, 3000)); if (g + 1 < segs) s += "/"; }
            if (R.next() % 2) { snprintf(tmp, sizeof tmp, "?id=%llu", (unsigned long long)(R.next() % 1000000)); s += tmp; }
            S.add(s, off);
        }
    } else if (!strcmp(shape, "uuids")) {
        for (size_t i = 0; i < n; i++) { uint64_t a = R.next(), b = R.next();
            snprintf(tmp, sizeof tmp, "%08llx-%04llx-%04llx-%04llx-%012llx", (unsigned long long)(a >> 32), (unsigned long long)((a >> 16) & 0xffff),
                     (unsigned long long)(a & 0xffff), (unsigned long long)(b >> 48), (unsigned long long)(b & 0xffffffffffffULL)); S.add(tmp, off); }
    } else if (!strcmp(shape, "paths")) {
        for (size_t i = 0; i < n; i++) {
            snprintf(tmp, sizeof tmp, "/home/user%02llu/projects/proj%03llu/src/%s/%s.c", (unsigned long long)(R.next() % 20), (unsigned long long)(str_zipf(R, 100)),
                     str_word(str_zipf(R, 400)).c_str(), str_word(R.next() % 20000).c_str());
            S.add(tmp, off);
        }
    } else if (!strcmp(shape, "logs") || !strcmp(shape, "logs8")) {   // timestamps that mostly increase, a little out of order;
        double t = 0;                                               // logs8: eight such files concatenated, each over the same hours
        size_t file = n / 8 + 1;
        for (size_t i = 0; i < n; i++) {
            if (!strcmp(shape, "logs8") && i % file == 0) t = 0;
            t += R.f() * 2.0; double ts = t + (R.f() < 0.05 ? -R.f() * 500.0 : 0.0); if (ts < 0) ts = 0;
            long ms = (long)ts; int H = (int)(ms / 3600000) % 24, M = (int)(ms / 60000) % 60, Sx = (int)(ms / 1000) % 60, F = (int)(ms % 1000);
            snprintf(tmp, sizeof tmp, "2026-10-09T%02d:%02d:%02d.%03dZ host-%02llu [%s] %s %s", H, M, Sx, F, (unsigned long long)(R.next() % 32),
                     (R.next() % 10) ? "INFO" : "WARN", str_word(str_zipf(R, 2000)).c_str(), str_word(str_zipf(R, 2000)).c_str());
            S.add(tmp, off);
        }
    } else if (!strcmp(shape, "ids")) {
        for (size_t i = 0; i < n; i++) { snprintf(tmp, sizeof tmp, "user_%010llu", (unsigned long long)(R.next() % 10000000000ULL)); S.add(tmp, off); }
    } else if (!strcmp(shape, "emails")) {
        static const char *web[] = { "gmail.com", "yahoo.com", "outlook.com", "hotmail.com", "icloud.com", "proton.me", "aol.com" };
        static const char *tld[] = { ".com", ".com", ".org", ".net", ".co.uk", ".de", ".io" };
        for (size_t i = 0; i < n; i++) {
            std::string s = str_word(str_zipf(R, 50000));
            uint64_t k = R.next() % 10;
            if (k < 4) s += "." + str_word(str_zipf(R, 50000));                                             // first.last
            if (k >= 7) { snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)(R.next() % 2000)); s += tmp; }   // a number
            uint64_t d = str_zipf(R, 2000);
            s += "@"; if (d < 7) s += web[d]; else s += str_word(d) + tld[R.next() % 7];
            S.add(s, off);
        }
    } else if (!strcmp(shape, "dates")) {
        for (size_t i = 0; i < n; i++) {
            uint64_t ms = 16801ULL * 86400000ULL + (uint64_t)(R.f() * 3653.0 * 86400000.0);   // 2016-01-01 .. 2025-12-31, to the ms
            long days = (long)(ms / 86400000ULL); uint64_t rem = ms % 86400000ULL; int y; unsigned m, d; str_civil(days, y, m, d);
            snprintf(tmp, sizeof tmp, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", y, m, d, (unsigned)(rem / 3600000), (unsigned)(rem / 60000 % 60),
                     (unsigned)(rem / 1000 % 60), (unsigned)(rem % 1000));
            S.add(tmp, off);
        }
    } else {                                                // random printable bytes, 5 to 40 of them
        for (size_t i = 0; i < n; i++) { int L = 5 + (int)(R.next() % 36); for (int j = 0; j < L; j++) tmp[j] = (char)(33 + R.next() % 94); tmp[L] = 0; S.add(tmp, off); }
    }
    if (S.buf.empty()) S.buf.push_back(0);
    S.p.resize(n);
    for (size_t i = 0; i < n; i++) S.p[i] = S.buf.data() + off[i];    // after the pool stops growing
    return S;
}
static inline bool str_less(const char *x, const char *y) { return strcmp(x, y) < 0; }
// equal as strings at every position: identical strings may come back in any pointer order
static bool str_same(const std::vector<const char *> &a, const std::vector<const char *> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (strcmp(a[i], b[i])) return false;
    return true;
}

// ---- the prefix scheme with any (keys, values) sorter: keys of 8 bytes, groups of equal keys 8 bytes deeper ----
static inline uint64_t bs_key8(const char *s) { uint64_t k = 0; for (int i = 0; i < 8; i++) { uint64_t c = (unsigned char)s[i]; k |= c << (56 - 8 * i); if (!c) break; } return k; }
static void bs_mkqs(const char **a, long lo, long hi, long d) {
    while (hi - lo > 16) {
        int pv = (unsigned char)a[(lo + hi) / 2][d]; long lt = lo, gt = hi, i = lo;
        while (i <= gt) { int c = (unsigned char)a[i][d]; if (c < pv) std::swap(a[lt++], a[i++]); else if (c > pv) std::swap(a[gt--], a[i]); else i++; }
        bs_mkqs(a, lo, lt - 1, d); bs_mkqs(a, gt + 1, hi, d); if (pv == 0) return; lo = lt; hi = gt; d++;
    }
    for (long i = lo + 1; i <= hi; i++) { const char *t = a[i]; long j = i; while (j > lo && strcmp(a[j - 1] + d, t + d) > 0) { a[j] = a[j - 1]; j--; } a[j] = t; }
}
template <class KV> static void prefix_level(const char **a, uint64_t *k, size_t lo, size_t hi, size_t d, KV &kv, bool par) {
    for (;;) {
        size_t n = hi - lo;
        if (n <= 32) { if (n > 1) bs_mkqs(a, (long)lo, (long)hi - 1, (long)d); return; }
        uint64_t mn = ~0ULL, mx = 0;
        bool p = par && n >= 65536;
        #pragma omp parallel for if(p) reduction(min:mn) reduction(max:mx) schedule(static)
        for (size_t i = lo; i < hi; i++) { uint64_t x = bs_key8(a[i] + d); k[i] = x; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
        if (mn == mx) { if ((mn & 0xff) == 0) return; d += 8; continue; }
        kv(k + lo, (uint64_t *)(a + lo), n, p);
        std::vector<std::pair<size_t, size_t>> runs, big;
        for (size_t i = lo; i < hi; ) { size_t j = i + 1; while (j < hi && k[j] == k[i]) j++; if (j - i > 1 && (k[i] & 0xff)) ((p && j - i >= 65536) ? big : runs).push_back({ i, j }); i = j; }
        if (p) {
            #pragma omp parallel for schedule(dynamic, 64)
            for (size_t r = 0; r < runs.size(); r++) prefix_level(a, k, runs[r].first, runs[r].second, d + 8, kv, false);
        } else for (auto &r : runs) prefix_level(a, k, r.first, r.second, d + 8, kv, false);
        for (auto &r : big) prefix_level(a, k, r.first, r.second, d + 8, kv, true);
        return;
    }
}
struct KVStd { std::vector<std::pair<uint64_t, uint64_t>> tmp;
    void operator()(uint64_t *k, uint64_t *v, size_t n, bool) { tmp.resize(n); for (size_t i = 0; i < n; i++) tmp[i] = { k[i], v[i] };
        std::sort(tmp.begin(), tmp.end(), [](const std::pair<uint64_t, uint64_t> &x, const std::pair<uint64_t, uint64_t> &y) { return x.first < y.first; });
        for (size_t i = 0; i < n; i++) { k[i] = tmp[i].first; v[i] = tmp[i].second; } } };
static void prefix_std_sort(const char **a, size_t n) { std::vector<uint64_t> k(n ? n : 1); KVStd kv; prefix_level(a, k.data(), 0, n, 0, kv, false); }
#ifdef BENCH_STRINGS_VQ
// the same scheme on Google's VQSort (Highway): its key-value form sorts 16-byte {value, key} records rather than two parallel
// arrays, so each call packs the pairs and unpacks them after -- one pass each way, the price of that layout. VQSort is
// single-threaded, so there is no parallel driver.
struct KVVq { std::vector<hwy::K64V64> tmp;
    void operator()(uint64_t *k, uint64_t *v, size_t n, bool) { tmp.resize(n); for (size_t i = 0; i < n; i++) { tmp[i].key = k[i]; tmp[i].value = v[i]; }
        hwy::VQSort(tmp.data(), n, hwy::SortAscending());
        for (size_t i = 0; i < n; i++) { k[i] = tmp[i].key; v[i] = tmp[i].value; } } };
static void prefix_vq_sort(const char **a, size_t n) { std::vector<uint64_t> k(n ? n : 1); KVVq kv; prefix_level(a, k.data(), 0, n, 0, kv, false); }
#endif
#ifdef BENCH_STRINGS_XSS
struct KVXss { void operator()(uint64_t *k, uint64_t *v, size_t n, bool) { x86simdsortStatic::keyvalue_qsort(k, v, n); } };
// the 8-byte-prefix scheme on x86-simd-sort's key-value sort; par: OpenMP over the large sorts and the groups (threads as set)
static void prefix_xss_sort(const char **a, size_t n, bool par) { std::vector<uint64_t> k(n ? n : 1); KVXss kv; prefix_level(a, k.data(), 0, n, 0, kv, par); }
#endif
