// strsmallbench: what ONE small string sort costs, and an A/B switch paired against the shipped engine (2026-10-19).
//
//   SS_SWITCH=<NAME> [SS_VALUE=<v>] ./strsmallbench [pairs] [sizes] [shape filter]
//
// For each shape and size: a batch of distinct inputs (2 MB of pointers' worth, cold, as scalebench's small rows), sorted
// once each with the switch set and once without, the two batches adjacent and alternating which goes first. Printed: ns per
// call for both, and the median over pairs of t(switch) / t(shipped): ABOVE 1 MEANS THE SHIPPED ENGINE IS FASTER. Also
// IPS4o-free context: std::sort with strcmp on the same batch. Every output is checked against std::sort's.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
extern "C" {
#include "logos_psort.h"
}
#include "bench_strings.h"
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 21;
    const char *sl = argc > 2 ? argv[2] : "4,8,16,24,32,48,64,128,1000";
    const char *filt = argc > 3 ? argv[3] : "";
    const char *sw = getenv("SS_SWITCH"), *sv = getenv("SS_VALUE");
    std::vector<size_t> sizes; for (const char *p = sl; *p; ) { sizes.push_back((size_t)atol(p)); const char *c = strchr(p, ','); if (!c) break; p = c + 1; }
    static const char *shapes[] = { "uuids", "words", "urls", "logs", "paths" };
    printf("# strsmallbench: switch %s%s%s, %d pairs; ns per call; ratio = t(switch)/t(shipped), > 1: shipped is faster\n", sw ? sw : "(none: null)", sv ? "=" : "", sv ? sv : "", pairs);
    for (const char *sh : shapes) {
        if (*filt && !strstr(sh, filt)) continue;
        StrSet S = gen_strings(sh, 400000);
        for (size_t n : sizes) {
            size_t calls = 2000000 / (n * 8 + 64); if (calls < 4) calls = 4; if (calls * n > 400000) calls = 400000 / n;
            std::vector<const char *> base(calls * n), work(calls * n), ref(calls * n);
            for (size_t i = 0; i < calls * n; i++) base[i] = S.p[(i * 2654435761ULL) % 400000];   // distinct inputs, scattered in the pool
            ref = base; for (size_t c = 0; c < calls; c++) std::sort(ref.begin() + c * n, ref.begin() + (c + 1) * n, str_less);
            auto run = [&](bool on) { if (on && sw) setenv(sw, sv ? sv : "1", 1); work = base; double t0 = now();
                for (size_t c = 0; c < calls; c++) logos_sort_strings_mt(work.data() + c * n, n, 1);
                double dt = now() - t0; if (on && sw) unsetenv(sw); return dt / (double)calls; };
            bool bad = false;
            for (int v = 0; v < 2; v++) { run(v); for (size_t i = 0; i < calls * n; i++) if (strcmp(work[i], ref[i])) { bad = true; break; } }
            std::vector<double> r, ta, tb;
            for (int p = 0; p < pairs; p++) { double a, b; if (p & 1) { b = run(false); a = run(true); } else { a = run(true); b = run(false); } r.push_back(a / b); ta.push_back(a); tb.push_back(b); }
            work = base; double t0 = now(); for (size_t c = 0; c < calls; c++) std::sort(work.begin() + c * n, work.begin() + (c + 1) * n, str_less); double tstd = (now() - t0) / (double)calls;
            std::sort(r.begin(), r.end()); std::sort(ta.begin(), ta.end()); std::sort(tb.begin(), tb.end());
            printf("%-6s n=%-5zu switch %9.1f  shipped %9.1f  std::sort %9.1f ns   ratio %.3f (%.3f-%.3f)%s\n", sh, n, ta[pairs / 2], tb[pairs / 2], tstd,
                   r[pairs / 2], r[pairs / 10], r[pairs * 9 / 10], bad ? "  WRONG OUTPUT" : "");
            fflush(stdout);
        }
    }
    return 0;
}
