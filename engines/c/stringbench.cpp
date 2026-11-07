// stringbench: sorting C strings -- logos against std::sort, __gnu_parallel::sort, the multikey quicksort logos had, and the same
// 8-byte-prefix scheme driven by std::sort and by x86-simd-sort's AVX2 key-value sort (the library cannot sort strings itself: a
// key-value sort over (prefix, pointer) pairs is the fair way to put its kernel under a string sort).
//
// PREDICTION, written before the first run (2026-10-09), 2M strings, 1 thread, against std::sort with strcmp:
//   high-entropy prefixes (uuids, random bytes): logos 3-6x (std::sort chases two pointers per compare, ~21 compares per string;
//   logos reads each string once per 8 bytes it needs and sorts 16-byte pairs in cache);
//   words (Zipf, short, duplicates): 2-4x; ids ("user_" + digits, one tie level): 2-4x;
//   long shared prefixes with structure (urls, paths, logs): 1.5-3x (each extra 8 bytes is a pass of random pointer reads);
//   sorted words: 0.5-1.0x (std::sort's introsort is cheap on sorted input; nothing here detects runs).
//   The multikey quicksort logos had: 1.2-2x over std::sort. Prefix + std::sort: between the two. Prefix + x86-simd-sort:
//   within 0.8-1.25 of logos (the key-value kernel against an MSD radix on 16-byte pairs; the key reads are the same).
// 14 threads, against __gnu_parallel::sort with strcmp: logos 3-6x; against prefix + x86-simd-sort (OpenMP): 1.0-1.5x.
//
// Every output is checked against std::sort's (strcmp equal at every position). Contenders rotate inside each rep on the same
// input; ratios are taken within a rep and their median reported. Re-executes itself with OMP_WAIT_POLICY=PASSIVE (the
// scoreboard's lesson: OpenMP threads spin after a region and slow whatever runs next).
//
// usage: stringbench [n] [reps] [threads list] [shape filter]        build: make stringbench XSS=../x86-simd-sort/src
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include <parallel/algorithm>
#include <unistd.h>
#include <omp.h>
#include "x86simdsort-static-incl.h"
extern "C" {
#include "logos_psort.h"
}

#define BENCH_STRINGS_XSS 1
#include "bench_strings.h"                                   // the shapes and the prefix scheme, shared with scalebench
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void prefix_std(const char **a, size_t n, int) { prefix_std_sort(a, n); }
static void prefix_xss(const char **a, size_t n, int P) { omp_set_num_threads(P); prefix_xss_sort(a, n, P > 1); }
static void std_strcmp(const char **a, size_t n, int) { std::sort(a, a + n, [](const char *x, const char *y) { return strcmp(x, y) < 0; }); }
static void gnu_par(const char **a, size_t n, int P) { omp_set_num_threads(P); __gnu_parallel::sort(a, a + n, [](const char *x, const char *y) { return strcmp(x, y) < 0; }); }
static void logos_mkqs(const char **a, size_t n, int) { setenv("LOGOS_STR_MKQS", "1", 1); logos_sort_strings(a, n); unsetenv("LOGOS_STR_MKQS"); }   /* the quicksort logos_sort_strings was */
static void logos_new(const char **a, size_t n, int P) { logos_sort_strings_mt(a, n, P); }
/* the A/B contender: logos with the settings in STRING_AB_ENV ("NAME=VALUE,NAME=VALUE"; default LOGOS_STR_SEQBIG=1) set for its calls */
static void ab_env(bool on) {
    const char *spec = getenv("STRING_AB_ENV"); std::string s = spec ? spec : "LOGOS_STR_SEQBIG=1";
    size_t i = 0;
    while (i < s.size()) { size_t c = s.find(',', i); if (c == std::string::npos) c = s.size(); std::string kv = s.substr(i, c - i); size_t e = kv.find('=');
        if (e != std::string::npos) { std::string k = kv.substr(0, e); if (on) setenv(k.c_str(), kv.substr(e + 1).c_str(), 1); else unsetenv(k.c_str()); }
        i = c + 1; }
}
static void logos_seqbig(const char **a, size_t n, int P) { ab_env(true); logos_sort_strings_mt(a, n, P); ab_env(false); }

typedef void (*sort_fn)(const char **, size_t, int);
struct Contender { const char *name; sort_fn f; bool one, many; };
static const Contender C[] = {
    { "logos", logos_new, true, true }, { "std::sort", std_strcmp, true, false }, { "gnu_par", gnu_par, false, true },
    { "logos_mkqs", logos_mkqs, true, false }, { "prefix+std", prefix_std, true, false }, { "prefix+xss", prefix_xss, true, true },
    { "logos_ab", logos_seqbig, false, true },       /* A/B: logos with STRING_AB_ENV set (STRING_AB=1 only; default: the first cut's group policy) */
};
static const int NC = (int)(sizeof C / sizeof C[0]);

int main(int argc, char **argv) {
    if (!getenv("OMP_WAIT_POLICY")) { setenv("OMP_WAIT_POLICY", "PASSIVE", 1); execv("/proc/self/exe", argv); }
    size_t n = argc > 1 ? (size_t)atof(argv[1]) : 2000000;
    int reps = argc > 2 ? atoi(argv[2]) : 7;
    const char *tl = argc > 3 ? argv[3] : "1,14";
    const char *filt = argc > 4 ? argv[4] : "";
    std::vector<int> ths; for (const char *p = tl; *p; ) { ths.push_back(atoi(p)); const char *c = strchr(p, ','); if (!c) break; p = c + 1; }
    static const char *shapes[] = { "uuids", "random", "words", "ids", "urls", "paths", "logs", "logs8", "words_sorted" };
    printf("# stringbench: n = %zu strings, %d reps, OMP_WAIT_POLICY=%s. Per contender: median ms, and the median over reps of\n", n, reps, getenv("OMP_WAIT_POLICY"));
    printf("# (its time / logos's time) in the same rep: > 1 means logos is faster. Every output checked against std::sort's.\n");
    for (const char *sh : shapes) {
        if (*filt && !strstr(sh, filt)) continue;
        StrSet S = gen_strings(sh, n);
        std::vector<const char *> base = S.p, work(n), ref(n);
        ref = base; std_strcmp(ref.data(), n, 1);
        double avg = 0; for (size_t i = 0; i < n; i++) avg += strlen(base[i]); avg /= (double)n;
        for (int P : ths) {
            printf("%-13s P=%-2d (mean length %.1f)\n", sh, P, avg);
            std::vector<int> idx; for (int c = 0; c < NC; c++) if (P == 1 ? C[c].one : C[c].many) idx.push_back(c);
            if (getenv("STRING_AB")) { idx.clear(); idx.push_back(0); for (int c = 0; c < NC; c++) if (!strcmp(C[c].name, "logos_ab")) idx.push_back(c); }
            else idx.erase(std::remove_if(idx.begin(), idx.end(), [](int c) { return !strcmp(C[c].name, "logos_ab"); }), idx.end());
            std::vector<std::vector<double>> t(NC);
            bool bad[16] = { false };
            for (int r = 0; r < reps; r++) {
                for (size_t q = 0; q < idx.size(); q++) {
                    int c = idx[(q + (size_t)r) % idx.size()];
                    work = base; double t0 = now(); C[c].f(work.data(), n, P); double dt = now() - t0;
                    t[c].push_back(dt);
                    if (r == 0) for (size_t i = 0; i < n; i++) if (strcmp(work[i], ref[i])) { bad[c] = true; break; }
                }
            }
            for (int c : idx) {
                std::vector<double> v = t[c], ratio; std::sort(v.begin(), v.end());
                for (int r = 0; r < reps; r++) ratio.push_back(t[c][r] / t[0][r]);
                std::sort(ratio.begin(), ratio.end());
                printf("  %-12s %9.1f ms  %7.1f ns/string   x%-6.2f%s\n", C[c].name, v[v.size() / 2], v[v.size() / 2] * 1e6 / (double)n,
                       ratio[ratio.size() / 2], bad[c] ? "   WRONG OUTPUT" : "");
            }
            fflush(stdout);
        }
    }
    return 0;
}
