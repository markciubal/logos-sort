/* scalebench.cpp — time against n for every contender, every shape, at 1 thread and at P.
 *
 * Sizes 0, 1, 3, 5, 10, then half-decade steps 32, 100, ... up to LOGOS_MAX_N (default 5e7, and the ceiling is itself
 * a point), thirteen shapes (nine standard, four from the twist-and-knot rounds). Single-threaded section: std::sort, logos, logos in-place (no scratch at all), x86-simd-sort
 * (-DHAVE_XSS, OpenMP threads set to 1), IPS4o sequential (-DHAVE_IPS4O), LSD radix, timsort.
 * Parallel section at P threads: __gnu_parallel::sort, logos, logos in-place (parallel from 2026-09-20),
 * x86-simd-sort OpenMP, IPS4o parallel. Never one thread against many. std::sort's output is the reference;
 * every other output is checked equal to it. From 10k up, reps shrink with n (5 / 3 / 1) and the minimum is
 * reported. Below 10k one sort is shorter than the clock can resolve, so a size is timed as a BATCH of distinct
 * instances sorted back to back (see Small); n = 0 is a real row, the cost of one call.
 * Writes scale-report.html: per shape and thread count TWO line charts of the same measurements, time per sort
 * and nanoseconds per element. The time chart has linear axes by default with a log-log toggle; the
 * ns/element chart's value axis is ALWAYS linear and only its n axis follows the toggle. Size buttons cut
 * both down to n <= 10k or n <= 10, because a linear n axis crushes the small sizes into its first pixel.
 * Every chart zooms and pans (wheel with Ctrl or after a click, pinch, drag selections, Shift + drag, keys, per-panel
 * buttons), the two charts of a panel share their n window, a box by the pointer lists every sort at that n, and
 * the control bar stays in view with a jump to each shape: the script's IX block, driven in a real browser by
 * tools/browser_check.py and under a stub by tools/check_report.js.
 * Also: dashed O(n) and O(n log n) reference curves anchored at logos's largest visible point, each series'
 * empirical exponent (least-squares slope of log t vs log n over n >= 1e5), tables of both quantities, and
 * the calibration line. Build: make scalebench (no deps) or make scale-full (both opponents).
 * Overrides: LOGOS_MAX_N, LOGOS_MIN_N (10000 restores the old ladder), LOGOS_THREADS, LOGOS_MAX_SEC.
 * LOGOS_SCALE_REUSE=<report.html> re-renders that report's measurements through the current template, measuring nothing. */
#include "logos_psort.h"
#include "uplot_embed.h"
#include "bench_common.h"
#include "bench_sorts.h"
#include <algorithm>
#include <parallel/algorithm>
#include <omp.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#ifdef HAVE_XSS
#define XSS_USE_OPENMP 1
#include "x86simdsort-static-incl.h"
#endif
#ifdef HAVE_IPS4O
#include "ips4o.hpp"
#endif

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void gen_blocks(std::vector<double> &a, Rng &r, size_t nb, int overlap_every) {
    size_t n = a.size(); if (nb > n) nb = n ? n : 1; size_t blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb);
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[r.next() % (k + 1)]);
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
        if (overlap_every && (b % overlap_every) == 1) base -= (double)blk * 0.1;
        a[i] = base + (double)(i % blk) + r.f() * 0.5; }
}
static std::vector<double> gen(const std::string &shape, size_t n, uint64_t seed = 7) {
    Rng r{seed}; std::vector<double> a(n);
    if (shape == "uniform") for (auto &v : a) v = r.f() * 2e9 - 1e9;
    else if (shape == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i; for (size_t k = 0; k < n / 100; k++) std::swap(a[r.next() % n], a[r.next() % n]); }
    else if (shape == "few_distinct") for (auto &v : a) v = (double)(r.next() % 5) * 1e6;
    else if (shape == "heavy_tail") for (auto &v : a) v = 1000.0 * std::pow(1.0 - r.f() * 0.999999, -1.0 / 1.2);
    else if (shape == "bimodal") for (auto &v : a) v = ((r.next() & 1) ? 1e9 : -1e9) + r.f() * 10.0;
    else if (shape == "quantized") for (auto &v : a) v = (double)(r.next() % 4096) / 64.0;
    else if (shape == "int_wide") for (auto &v : a) v = (double)(int32_t)r.next();
    else if (shape == "block_perm") gen_blocks(a, r, 64, 0);
    else if (shape == "block_overlap") gen_blocks(a, r, 64, 8);
    /* 2026-09-21, the shapes the twist-and-knot rounds were about. block_twist: every other sorted block lies DESCENDING (the twist).
       block_kinked: block_perm with 1% of adjacent pairs swapped (Reidemeister move I, the detector's kink repair). sine_slope: a
       sloped sine, 32 cycles, with noise of +-50 elements (still 1.2x a random sort at one thread: open). reach_mirror: disorder
       of short reach handed over BACK TO FRONT, a newest-first log with late records (the whole array's orientation bit). */
    else if (shape == "block_twist") { gen_blocks(a, r, 64, 0); size_t nb = 64 > n ? (n ? n : 1) : 64, blk = (n + nb - 1) / nb;
        for (size_t b = 0; b * blk < n; b += 2) std::reverse(a.begin() + (long)(b * blk), a.begin() + (long)std::min(n, (b + 1) * blk)); }
    else if (shape == "block_kinked") { gen_blocks(a, r, 64, 0); if (n >= 4) for (size_t k = 0; k < n / 100; k++) { size_t x = 2 * (r.next() % (n / 2)); if (x + 1 < n) std::swap(a[x], a[x + 1]); } }
    else if (shape == "sine_slope") { double T = (double)n / 32.0, A = 0.6 * T / 2.0; for (size_t i = 0; i < n; i++) a[i] = (double)i + A * std::sin(6.283185307179586 * (double)i / T) + (r.f() * 2.0 - 1.0) * 50.0; }
    else if (shape == "reach_mirror") { for (size_t i = 0; i < n; i++) a[i] = (double)i + (r.f() * 2.0 - 1.0) * 60.0; std::reverse(a.begin(), a.end()); }
    return a;
}
/* every contender is a callable on (pointer, length), so one list serves a single large array and a batch of small ones */
template <class F> static double best_of(const std::vector<double> &base, int reps, F f, std::vector<double> &out, double budget_ms) {
    double b = 1e300;
    for (int i = 0; i < reps; i++) { std::vector<double> a = base; double t = nowms(); f(a.data(), a.size()); double ms = nowms() - t; if (ms < b) { b = ms; out.swap(a); } if (ms > budget_ms) break; }
    return b;
}
/* ---- small sizes ----------------------------------------------------------------------------------------------
 * Below SMALL_N one sort is shorter than the clock can resolve (five elements sort in about 20 ns, and reading
 * steady_clock costs about that), and sorting the SAME few values again and again would let the branch predictor
 * learn them, which flatters every comparison sort. So a small size is measured as a BATCH: `ni` distinct instances
 * of the shape (seeds 7, 8, ...), laid end to end, copied fresh OUTSIDE the timed region and sorted back to back
 * inside it. The per-sort time is the fastest batch divided by ni, and every instance of the last batch is checked
 * against std::sort of that same instance. n = 0 is a real row: it is what the call costs. A sort the compiler
 * inlines to nothing on an empty range measures zero; it is floored at 0.01 ns so that "measured" and "absent"
 * (0 in the report's data) stay distinguishable. */
#define SMALL_N 10000
struct Small { size_t n = 0, ni = 0; std::vector<double> pristine, work, ref; };
static Small make_small(const std::string &shape, size_t n) {
    Small s; s.n = n; s.ni = n == 0 ? 4096 : std::min<size_t>(4096, std::max<size_t>(16, 262144 / n));
    s.pristine.assign(s.ni * n + 1, 0.0);                                   /* + 1: data() is never null, even for n = 0 */
    for (size_t k = 0; k < s.ni; k++) { std::vector<double> a = gen(shape, n, 7 + k); std::copy(a.begin(), a.end(), s.pristine.begin() + (std::ptrdiff_t)(k * n)); }
    s.ref = s.pristine;
    for (size_t k = 0; k < s.ni; k++) std::sort(s.ref.begin() + (std::ptrdiff_t)(k * n), s.ref.begin() + (std::ptrdiff_t)((k + 1) * n));
    s.work = s.pristine;
    return s;
}
template <class F> static double best_small(Small &s, F f, bool &ok, double budget_ms) {
    double best = 1e300, acc = 0;
    for (int batch = 0; batch < 25; batch++) {
        std::copy(s.pristine.begin(), s.pristine.end(), s.work.begin());
        double *w = s.work.data(); const size_t n = s.n, ni = s.ni;
        double t = nowms();
        for (size_t k = 0; k < ni; k++) f(w + k * n, n);
        double ms = nowms() - t;
        if (ms < best) best = ms;
        acc += ms;
        if ((batch >= 4 && acc > 120.0) || ms > budget_ms) break;
    }
    ok &= (s.work == s.ref);
    double per = best / (double)s.ni;
    return per > 1e-8 ? per : 1e-8;
}
static std::string jnum(double v) { char b[32]; snprintf(b, sizeof b, "%.4g", v); return b; }

struct Row { size_t n; double t[7]; bool ok; };       /* t: [0]=std::sort or gnu_par, [1]=logos, [2]=xss, [3]=ips4o, [4]=LSD radix, [5]=timsort, [6]=logos in-place (4-6: 1T only); 0 = absent */
struct Series { std::string shape; int threads; std::vector<Row> rows; };

int main() {
    int P = bench_threads(logos_num_cpus());   /* bench_common.h: LOGOS_THREADS, else BENCH_THREADS, else physical cores */
    double max_n = 5e7; if (const char *e = getenv("LOGOS_MAX_N")) { double v = atof(e); if (v >= 1e4) max_n = v; }
    double max_sec = 20; if (const char *e = getenv("LOGOS_MAX_SEC")) { double v = atof(e); if (v > 0) max_sec = v; }
    const char *shapes[] = { "uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide", "block_perm", "block_overlap", "block_twist", "block_kinked", "sine_slope", "reach_mirror" };
    double min_n = 0; if (const char *e = getenv("LOGOS_MIN_N")) { double v = atof(e); if (v >= 0) min_n = v; }
    /* 0, 1, 3, 5 and 10 were asked for by hand: the empty call, the single element, and the sizes at which a sort is all
       fixed cost. 32 .. 3162 continue in the ladder's own half-decade steps so that the ns/element curves have no
       three-decade hole between 10 and 10k. LOGOS_MIN_N=10000 restores the old ladder. */
    std::vector<size_t> sizes;
    for (size_t n : { (size_t)0, (size_t)1, (size_t)3, (size_t)5, (size_t)10, (size_t)32, (size_t)100, (size_t)316, (size_t)1000, (size_t)3162 }) if ((double)n >= min_n) sizes.push_back(n);
    for (double n = 1e4; n <= max_n + 1; n *= 3.1622776601683795) { size_t v = (size_t)std::llround(n / 1e3) * 1000; if ((double)v >= min_n) sizes.push_back(v); }
    /* the ceiling is itself a point when the half-decade ladder stops well short of it (50M by default: 10M, 31.6M, 50M) */
    if (!sizes.empty() && (double)sizes.back() < max_n * 0.9 && max_n >= min_n) sizes.push_back((size_t)max_n);
    if (sizes.empty()) { fprintf(stderr, "no sizes between LOGOS_MIN_N and LOGOS_MAX_N\n"); return 1; }
    const char *xss_isa =
#if defined(HAVE_XSS) && defined(__AVX512F__)
        "AVX-512";
#elif defined(HAVE_XSS)
        "AVX2";
#else
        "";
#endif
    /* LOGOS_SCALE_REUSE=<report.html>: measure nothing, lift DATA and META out of an existing report and write them through
       the CURRENT template. The run takes minutes and the presentation is what gets iterated on; measurements must not
       change because a caption did. */
    std::string j, meta_s; int bad = 0;
    const char *reuse = getenv("LOGOS_SCALE_REUSE");
    if (reuse) {
        FILE *rf = fopen(reuse, "rb"); if (!rf) { fprintf(stderr, "cannot open %s\n", reuse); return 1; }
        std::string s; char rb[65536]; size_t got; while ((got = fread(rb, 1, sizeof rb, rf)) > 0) s.append(rb, got); fclose(rf);
        size_t a = s.find("const DATA="), b = a == std::string::npos ? a : s.find(",META=", a), c = b == std::string::npos ? b : s.find(";\n", b);
        if (c == std::string::npos) { fprintf(stderr, "%s: no DATA/META block found\n", reuse); return 1; }
        j = s.substr(a + 11, b - (a + 11)); meta_s = s.substr(b + 6, c - (b + 6));
        printf("re-rendering the measurements in %s through the current template; nothing is measured\n", reuse);
    } else {
    /* calibration */
    double memcpy_ms = 1e300, std1m_ms = 1e300;
    { std::vector<double> c1(8000000), c2(8000000); Rng cr{3}; for (auto &v : c1) v = cr.f();
      for (int i = 0; i < 5; i++) { double t = nowms(); memcpy(c2.data(), c1.data(), c1.size() * 8); double ms = nowms() - t; if (ms < memcpy_ms) memcpy_ms = ms; }
      for (int i = 0; i < 3; i++) { std::copy(c1.begin(), c1.begin() + 1000000, c2.begin()); double t = nowms(); std::sort(c2.begin(), c2.begin() + 1000000); double ms = nowms() - t; if (ms < std1m_ms) std1m_ms = ms; } }
    printf("logos-sort scaling: %zu sizes (%zu .. %zu), P = %d, budget %.0f s/sort; below %d elements a size is timed as a batch of distinct inputs\n", sizes.size(), sizes.front(), sizes.back(), P, max_sec, (int)SMALL_N);
    printf("calibration (1T): memcpy 64MB %.2f ms, std::sort 1M %.1f ms  <- >15%% drift from quiet = loaded\n\n", memcpy_ms, std1m_ms);

    std::vector<Series> all;
    for (const char *sh : shapes) {
        Series s1{sh, 1, {}}, sp{sh, P, {}}; bool over = false;
        for (size_t n : sizes) {
            if (over) break;
            const bool small = n < SMALL_N;
            Small sm; std::vector<double> base;
            if (small) sm = make_small(sh, n); else base = gen(sh, n);
            int reps = n <= 1000000 ? 5 : n <= 10000000 ? 3 : 1; double budget = max_sec * 1e3;
            std::vector<double> ref, o;
            /* one measuring function for both regimes. Large n: std::sort's output becomes the reference (is_ref) and every
               other output is compared with it. Small n: the batch carries its own per-instance reference. */
            auto T = [&](auto f, bool &ok, bool is_ref) -> double {
                if (small) return best_small(sm, f, ok, budget);
                double t = best_of(base, reps, f, o, budget);
                if (is_ref) ref.swap(o); else ok &= (o == ref);
                return t;
            };
            /* ---- single-threaded ---- */
            omp_set_num_threads(1);
            Row r1{n, {0, 0, 0, 0, 0, 0, 0}, true};
            r1.t[0] = T([](double *p, size_t m) { std::sort(p, p + m); }, r1.ok, true);
            r1.t[1] = T([](double *p, size_t m) { logos_sort_f64(p, m, 1); }, r1.ok, false);
#ifdef HAVE_XSS
            r1.t[2] = T([](double *p, size_t m) { x86simdsortStatic::qsort(p, m, false, false); }, r1.ok, false);
#endif
#ifdef HAVE_IPS4O
            r1.t[3] = T([](double *p, size_t m) { ips4o::sort(p, p + m); }, r1.ok, false);
#endif
            r1.t[4] = T([](double *p, size_t m) { radix_sort_f64(p, m); }, r1.ok, false);
            r1.t[5] = T([](double *p, size_t m) { timsort_f64(p, m); }, r1.ok, false);
            /* the strict in-place entry: no scratch at all (make memprobe: 0 bytes of heap) against 15-23 bytes per element
               for the buffered sort. An outside review's headline comparison; here it is one more line on the same axes. */
            r1.t[6] = T([](double *p, size_t m) { logos_sort_f64_inplace(p, m, 1); }, r1.ok, false);
            s1.rows.push_back(r1);
            /* ---- P threads ---- */
            omp_set_num_threads(P);
            Row rp{n, {0, 0, 0, 0, 0, 0, 0}, true};
            rp.t[0] = T([](double *p, size_t m) { __gnu_parallel::sort(p, p + m); }, rp.ok, false);
            rp.t[1] = T([P](double *p, size_t m) { logos_sort_f64(p, m, P); }, rp.ok, false);
            rp.t[6] = T([P](double *p, size_t m) { logos_sort_f64_inplace(p, m, P); }, rp.ok, false);   /* parallel from 2026-09-20: placement passes as tasks */
#ifdef HAVE_XSS
            rp.t[2] = T([](double *p, size_t m) { x86simdsortStatic::qsort(p, m, false, false); }, rp.ok, false);
#endif
#ifdef HAVE_IPS4O
            rp.t[3] = T([](double *p, size_t m) { ips4o::parallel::sort(p, p + m); }, rp.ok, false);
#endif
            sp.rows.push_back(rp);
            if (!r1.ok || !rp.ok) bad++;
            const double u = small ? 1e6 : 1.0;                 /* small rows print nanoseconds per sort, the rest milliseconds */
            printf("  %-13s n=%-9zu 1T %s: std %9.1f logos %9.1f inplace %9.1f xss %9.1f ips4o %9.1f radix %9.1f tim %9.1f | %dT: gnu_par %9.1f logos %9.1f inplace %9.1f xss %9.1f ips4o %9.1f%s\n", sh, n, small ? "ns" : "ms",
                   r1.t[0] * u, r1.t[1] * u, r1.t[6] * u, r1.t[2] * u, r1.t[3] * u, r1.t[4] * u, r1.t[5] * u, P, rp.t[0] * u, rp.t[1] * u, rp.t[6] * u, rp.t[2] * u, rp.t[3] * u, (r1.ok && rp.ok) ? "" : "  <-- MISMATCH");
            fflush(stdout);
            if (r1.t[0] > budget * 0.5) over = true;        /* the slowest contender is at half the budget: stop this shape here */
        }
        all.push_back(s1); all.push_back(sp);
    }

    /* ---- JSON ---- */
    j = "[";
    for (size_t si = 0; si < all.size(); si++) {
        const Series &s = all[si]; j += (si ? ",{" : "{"); j += "\"shape\":\"" + s.shape + "\",\"threads\":" + std::to_string(s.threads) + ",\"rows\":[";
        for (size_t ri = 0; ri < s.rows.size(); ri++) { const Row &r = s.rows[ri]; j += (ri ? ",{" : "{"); j += "\"n\":" + std::to_string(r.n) + ",\"t\":[" + jnum(r.t[0]) + "," + jnum(r.t[1]) + "," + jnum(r.t[2]) + "," + jnum(r.t[3]) + "," + jnum(r.t[4]) + "," + jnum(r.t[5]) + "," + jnum(r.t[6]) + "],\"ok\":" + (r.ok ? "true" : "false") + "}"; }
        j += "]}";
    }
    j += "]";
    char meta[400]; snprintf(meta, sizeof meta, "{\"threads\":%d,\"memcpyMs\":%.3f,\"std1mMs\":%.2f,\"maxN\":%.0f,\"xss\":%s,\"xssIsa\":\"%s\",\"ips4o\":%s,\"ncpus\":%d,\"smallBelow\":%d}", P, memcpy_ms, std1m_ms, max_n,
#ifdef HAVE_XSS
             "true",
#else
             "false",
#endif
             xss_isa,
#ifdef HAVE_IPS4O
             "true",
#else
             "false",
#endif
             logos_num_cpus(), (int)SMALL_N);
    meta_s = meta;
    }

    static const char *TMPL = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>logos-sort scaling</title>
<style>
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s-logos:#2a78d6;--s-base:#898781;--s-xss:#eb6834;--s-ips:#1baf7a;--s-radix:#eda100;--s-tim:#e87ba4;--s-inpl:#7a5cd6}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-ips:#199e70;--s-radix:#c98500;--s-tim:#d55181;--s-inpl:#9a82ee}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-ips:#199e70;--s-radix:#c98500;--s-tim:#d55181;--s-inpl:#9a82ee}
*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding-block:0 3rem;padding-inline:clamp(16px,3vw,32px)}
.wrap{max-width:1100px;margin:0 auto}h1{font-size:22px;margin:28px 0 4px}h2{font-size:16px;margin:26px 0 6px}h3{font-size:13px;margin:0 0 4px;color:var(--ink2);font-weight:600}
.sub{color:var(--ink2);margin:0 0 12px;font-size:13px}.warn{background:var(--surface);border-left:3px solid #fab219;padding:8px 12px;margin:10px 0}
.legend{display:flex;gap:6px 16px;flex-wrap:wrap;font-size:12px;color:var(--ink2);margin:6px 0 10px}.sw{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:6px;border-radius:2px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:820px){.grid2{grid-template-columns:1fr}}
.card{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:12px 14px;margin:0 0 16px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0 6px}
button.f{font:inherit;font-size:13px;padding:4px 12px;border:1px solid var(--axis);background:var(--surface);color:var(--ink);border-radius:999px;cursor:pointer}
button.f[aria-pressed="true"]{background:var(--ink);color:var(--page);border-color:var(--ink)}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px;background:var(--surface)}th,td{padding:4px 8px;text-align:right;border-bottom:1px solid var(--grid)}th{color:var(--muted);font-weight:500;font-size:12px}td:first-child,th:first-child{text-align:left}
.tw{overflow-x:auto;border:1px solid var(--grid);border-radius:6px}details{margin-top:8px}summary{cursor:pointer;color:var(--ink2);font-size:13px}
__UPLOT_CSS__
/* uPlot is themed from the same tokens as the rest of the page. It draws the plot to a canvas, so
   the stroke colours are resolved from these variables in JS at build time and the charts are
   rebuilt when the colour scheme changes. */
.uplot{width:100%}.u-wrap{margin:0 auto}
.uplot,.u-legend{font:12px system-ui,-apple-system,"Segoe UI",sans-serif;color:var(--ink2)}
.u-legend{margin:8px 0 0;text-align:left}
.u-legend th,.u-legend td{border:0;padding:1px 10px 1px 0;font-size:12px}
.u-legend tr{cursor:pointer}
.u-legend .u-series.u-off *{opacity:.35}
.u-legend .u-value{color:var(--ink);font-variant-numeric:tabular-nums;text-align:right}
.u-select{background:color-mix(in srgb,var(--s-logos) 16%,transparent)}
.u-cursor-x,.u-cursor-y{border-color:var(--axis)}
.u-cursor-pt{border-color:var(--surface)}
.hint{color:var(--muted);font-size:12px;margin:2px 0 0}.cap2{margin-top:14px}
.sep{display:inline-block;width:1px;height:18px;background:var(--axis);margin:0 4px}
.bar{position:sticky;top:0;z-index:40;background:var(--page);padding:6px 0 2px;margin:0 0 8px;border-bottom:1px solid var(--grid)}.bar .row{margin:2px 0 4px}
select.f{font:inherit;font-size:13px;padding:4px 10px;border:1px solid var(--axis);background:var(--surface);color:var(--ink);border-radius:999px;cursor:pointer;max-width:220px}
h2{scroll-margin-top:64px}.card.wide{grid-column:1/-1}
.hd{display:flex;align-items:center;gap:6px;margin:0 0 4px}.hd h3{margin:0;flex:1}
button.t{font:inherit;font-size:12px;line-height:1.2;min-width:26px;padding:2px 8px;border:1px solid var(--axis);background:var(--surface);color:var(--ink2);border-radius:6px;cursor:pointer}
button.t:hover{color:var(--ink);border-color:var(--ink2)}button.t[aria-pressed="true"]{background:var(--ink);color:var(--page);border-color:var(--ink)}
/* the plot itself: a crosshair to select with, a hand once 'drag pans' is on, a ring while it is the chart the wheel talks to */
.u-over{cursor:crosshair;outline:none}#charts.pan .u-over{cursor:grab}.u-over.grabbing,#charts.pan .u-over.grabbing{cursor:grabbing}
.u-over.act{box-shadow:inset 0 0 0 2px color-mix(in srgb,var(--s-logos) 60%,transparent)}
.tip{position:absolute;z-index:30;display:none;pointer-events:none;background:var(--surface);color:var(--ink);border:1px solid var(--axis);border-radius:6px;padding:6px 9px;font-size:12px;line-height:1.35;white-space:nowrap;box-shadow:0 3px 12px rgba(0,0,0,.18)}
.tip table{width:auto;margin-top:2px;font-size:12px;background:none}.tip th,.tip td{border:0;padding:1px 0 1px 12px}.tip th:first-child,.tip td:first-child{padding-left:0}.tip th{font-size:11px}.tip .sw{margin-right:5px}
.how{margin:0 0 10px;font-size:13px;color:var(--ink2)}.how ul{margin:6px 0 4px;padding-left:20px}.how li{margin:2px 0}.how b{color:var(--ink);font-weight:600}
.whint{position:absolute;z-index:31;left:50%;top:6px;transform:translateX(-50%);pointer-events:none;opacity:0;transition:opacity .25s;background:var(--ink);color:var(--page);font-size:12px;padding:3px 10px;border-radius:999px;white-space:nowrap}
</style></head><body><div class="wrap">
<h1>logos-sort — time against n</h1>
<p class="sub" id="sub"></p><div id="cal"></div>
<div class="legend" id="legend"></div>
<div class="bar"><div class="row" id="filters" role="group" aria-label="view controls"></div></div>
<p class="sub"><b>Every chart zooms and pans.</b> Ctrl + scroll, or a pinch, zooms around the pointer; a sideways drag picks a range of n; a double-click goes back. The box by the pointer lists every sort at that n, fastest first, with its time relative to logos.</p>
<details class="how"><summary>every gesture, key and button</summary><ul>
<li><b>Zoom.</b> Ctrl + scroll or pinch, around the pointer. Drag sideways for a range of n, up or down for a range of values, a box for both. Over the value axis, Ctrl + scroll zooms that axis alone. The &minus; and + buttons of a panel zoom around its middle.</li>
<li><b>Plain scroll moves the page</b>, because the page is mostly charts. Click a chart and plain scroll zooms that chart until the pointer leaves it.</li>
<li><b>Move along n.</b> Shift + scroll, a sideways scroll, Shift + drag, the middle button; or a plain drag once &ldquo;drag pans&rdquo; is on. On a touch screen: one finger sideways, once zoomed.</li>
<li><b>Go back.</b> Double-click, the 0 key or the panel&rsquo;s reset button. &ldquo;reset all&rdquo; also brings back every hidden series.</li>
<li><b>Keyboard.</b> Tab to a chart; the arrows move along n (Shift for bigger steps), + and &minus; zoom, 0 resets, Esc lets go.</li>
<li><b>Series.</b> Click a legend row to hide one, Ctrl + click to show it alone. The value axis refits to what is in view.</li>
<li><b>Panels.</b> The two charts of a panel share their n window and their pointer. &ldquo;expand&rdquo; gives a panel the full width. The bar above stays in view and jumps to any shape.</li>
</ul></details>
<p class="sub">The dashed grey curves are O(n) and O(n log n) references anchored at logos's largest measured point, so a series that bends above the O(n log n) curve is growing faster than a comparison sort should. The label after each series name is its empirical exponent, the least-squares slope of log t against log n over n &ge; 100k (below that fixed costs dominate). The single-threaded panel compares sorts that each use one thread; the parallel panel compares sorts that each use all threads. Every output was checked equal to std::sort's on the same data; the minimum of the repetitions is plotted. A line that stops early hit the per-sort time budget.</p>
<p class="sub">Each panel holds two charts of the same measurements: time per sort, and below it nanoseconds per element, whose value axis is always linear (only its n axis follows the axis toggle). n = 0 is the cost of one call: it has no per-element value, and it cannot sit on a log axis, so the log view leaves it to the tables. The size buttons cut every chart down to the small end, because a linear n axis puts everything below a million into its first pixels. The last row of the ns-per-element table is the scaling slope read off two points: with c the cost per element, p = 1 + ln(c_hi / c_lo) / ln(n_hi / n_lo), so p = 1 is linear scaling and the percentage beside it is how much dearer one element became from 1M to the largest size. Those two rows are measured minutes apart on a machine whose speed drifts, so read that row as a sketch: make slopebench measures the same number in adjacent pairs, where the drift cancels. Sizes below 10k are timed as batches of distinct inputs sorted back to back, since one such sort is shorter than the clock can resolve and repeating one input would train the branch predictor; the parallel sorts are called with all threads there too, so what they show at small n is what such a call costs.</p>
<div id="charts"></div>
</div>
<script>__UPLOT_JS__</script>
<script>
const DATA=__DATA__,META=__META__;
const NAMES1=['std::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','LSD radix','timsort','logos in-place'];
const NAMESP=['__gnu_parallel::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','','','logos in-place'];
const CVAR=['--s-base','--s-logos','--s-xss','--s-ips','--s-radix','--s-tim','--s-inpl'];
const cssv=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim()||'#888888';
const fmtN=n=>n==null?'':n>=1e9?(+(n/1e9).toFixed(2))+'B':n>=1e6?(+(n/1e6).toFixed(2))+'M':n>=1e3?(+(n/1e3).toFixed(1))+'k':String(+n.toFixed(2));
/* one formatter per quantity, each walking down the units: five elements sort in nanoseconds, ten million in seconds */
const fmtMs=v=>v==null?'':v===0?'0':v>=1000?(+(v/1000).toFixed(1))+' s':v>=1?(+v.toFixed(v>=10?0:1))+' ms':v>=1e-3?(+(v*1e3).toFixed(v>=1e-2?0:1))+' µs':(+(v*1e6).toFixed(v>=1e-5?0:v>=1e-6?1:2))+' ns';
const fmtNs=v=>v==null?'':v===0?'0':v>=1e6?(+(v/1e6).toFixed(1))+' ms':v>=1e3?(+(v/1e3).toFixed(1))+' µs':(+v.toFixed(v>=100?0:v>=10?1:2))+' ns';
let MODE='lin';
let RANGE='all';
const RANGES={all:Infinity,k10:1e4,t10:10};
const NS=DATA.length?DATA[0].rows.map(r=>r.n):[];
document.getElementById('sub').textContent=`${META.threads} threads (${META.ncpus} online) · ${NS.length} sizes, ${NS.length?fmtN(NS[0]):'?'} .. ${NS.length?fmtN(NS[NS.length-1]):'?'} · ${DATA.length/2} shapes`;
document.getElementById('cal').innerHTML=`<div class="warn">Calibration (1T): memcpy 64 MB ${META.memcpyMs.toFixed(2)} ms, std::sort 1M ${META.std1mMs.toFixed(1)} ms. Quiet 20-core reference \u2248 2.5 ms / \u2248 50 ms; beyond 15% the machine was loaded and ratios within one chart are more trustworthy than absolute times.</div>`;
document.getElementById('legend').innerHTML=[1,6,0,2,3,4,5].filter(k=>k<2||(k==2&&META.xss)||(k==3&&META.ips4o)||k>=4).map(k=>`<span><i class="sw" style="background:${cssv(CVAR[k])}"></i>${k==0?'std::sort (1T) / __gnu_parallel::sort (P)':NAMES1[k]+((k==4||k==5)?' (1 thread only)':'')}</span>`).join('')+`<span><i class="sw ref"></i>O(n) and O(n log n), anchored at logos's largest n</span>`;

/* empirical exponent: least-squares slope of log10 t vs log10 n over n >= 1e5 (fixed costs dominate below) */
function exponent(rows,k){const pts=rows.filter(r=>r.t[k]>0&&r.n>=1e5).map(r=>[Math.log10(r.n),Math.log10(r.t[k])]);if(pts.length<3)return null;
 const mx=pts.reduce((a,p)=>a+p[0],0)/pts.length,my=pts.reduce((a,p)=>a+p[1],0)/pts.length;let sxx=0,sxy=0;pts.forEach(p=>{sxx+=(p[0]-mx)**2;sxy+=(p[0]-mx)*(p[1]-my);});return sxx>0?sxy/sxx:null;}

function table(s){const names=s.threads==1?NAMES1:NAMESP;const cols=[1,6,0,2,3,4,5].filter(k=>s.rows.some(r=>r.t[k]>0));
 const exps=cols.map(k=>{const e=exponent(s.rows,k);return e===null?'–':'n^'+e.toFixed(2);});
 const head=last=>`<thead><tr><th>n</th>${cols.map(k=>`<th>${names[k]}</th>`).join('')}${last?`<th>${last}</th>`:''}</tr></thead>`;
 const tRows=s.rows.map(r=>{const others=cols.filter(k=>k!=1&&k!=6&&r.t[k]>0).map(k=>r.t[k]);const best=others.length?Math.min(...others):0;
  return `<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.t[k]>0?fmtMs(r.t[k]):'–'}</td>`).join('')}<td>${best&&r.t[1]>0?(best/r.t[1]).toFixed(2)+'×':'–'}${r.ok?'':' ✗'}</td></tr>`;}).join('');
 /* the scaling slope read off two points, through ns per element: with c = t/n, t ~ n^p gives p = 1 + ln(c_hi/c_lo) / ln(n_hi/n_lo).
    p = 1 is linear; what a reader feels is the first number, how much dearer one element got. From 1M up, below that fixed costs bend it. */
 const slope=k=>{const rs=s.rows.filter(r=>r.t[k]>0&&r.n>=1e6);if(rs.length<2)return '-';const a=rs[0],b=rs[rs.length-1],g=(b.t[k]/b.n)/(a.t[k]/a.n);
  return (g>=1?'+':'')+((g-1)*100).toFixed(1)+'% (p = '+(1+Math.log(g)/Math.log(b.n/a.n)).toFixed(3)+')';};
 const pRows=s.rows.map(r=>`<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.t[k]>0&&r.n>0?fmtNs(r.t[k]*1e6/r.n):'–'}</td>`).join('')}</tr>`).join('');
 return `<details><summary>tables: time per sort, ns per element, empirical exponents</summary><h3>time per sort</h3><div class="tw"><table>${head('logos vs best other')}<tbody>${tRows}<tr><td>t ∝ (n ≥ 100k)</td>${exps.map(e=>`<td>${e}</td>`).join('')}<td></td></tr></tbody></table></div><h3 class="cap2">nanoseconds per element (n = 0 has no elements: its row above is the cost of one call)</h3><div class="tw"><table>${head('')}<tbody>${pRows}<tr><td>per element, 1M to the largest n</td>${cols.map(k=>`<td>${slope(k)}</td>`).join('')}</tr></tbody></table></div></details>`;}

/* Two uPlots per panel: time per sort against n, and the same measurements as nanoseconds per element. Series are
   built in draw order with logos LAST so it sits on top, and the two big-O references are appended as dashed series
   so the live legend reads them out like any other.
   The ns/element chart's VALUE axis is always linear; only its n axis follows the toggle. n = 0 has no per-element
   cost (a gap there) and cannot sit on a log axis at all, so the log view drops that row from both charts. */
function visible(s){const lim=RANGES[RANGE],lg=MODE==='log';return s.rows.filter(r=>r.n<=lim&&(!lg||r.n>0));}

/* The two run-off-the-top rules, as ONE function of the values about to be drawn, so that a zoomed window is treated
   the way a size preset is. cols holds one array of values per real series (the dashed references are not asked).
   A linear ns/element axis has one enemy: a FIXED cost divided by a tiny n. The LSD radix baseline pays about 7 us per
   call whatever n is, which at n = 3 is ~2400 ns per element against 2-100 for every other sort, and that one spike
   would press every other line into the floor. So the axis opens at three times the 85th percentile of what is drawn.
   The linear TIME chart needs a narrower rule. There the largest n towers over the rest by right, so a percentile
   would clip honest points; the only thing to guard against is ONE series standing more than four times above every
   other (the radix baseline's 7 us in the n <= 10 view, against 0.2 us for the rest). Then the axis opens just above
   the others' peak. Either way the caption names what runs off the top, its true values stay in the box by the
   pointer, the live legend and the table, and the value axis can be scrolled open. Nothing is dropped. */
function capRule(per,ylog,cols,names){
 const tail=' True values: the box by the pointer, the live legend and the table. Hide a series to rescale, or scroll on the value axis.';
 if(per){
  const all=[];cols.forEach(c=>c.forEach(v=>all.push(v)));all.sort((a,b)=>a-b);
  if(all.length>=8){const c=3*all[Math.floor(0.85*(all.length-1))],off=[];
   cols.forEach((col,i)=>{if(col.length){const m=Math.max(...col);if(m>c)off.push(names[i]+' (up to '+fmtNs(m)+')');}});
   if(off.length)return{cap:c,text:'nanoseconds per element (value axis always linear) · axis opens at '+fmtNs(c)+'; runs off the top: '+off.join(', ')+'.'+tail};}
  return{cap:Infinity,text:null};}
 if(!ylog){
  const pk=[];cols.forEach((col,i)=>{if(col.length)pk.push([Math.max(...col),names[i]]);});pk.sort((a,b)=>b[0]-a[0]);
  if(pk.length>=2&&pk[0][0]>4*pk[1][0]){const cap=1.3*pk[1][0];
   return{cap,text:'time per sort · axis opens at '+fmtMs(cap)+'; runs off the top: '+pk[0][1]+' (up to '+fmtMs(pk[0][0])+').'+tail};}}
 return{cap:Infinity,text:null};}

/* A log axis brings its own ticks. uPlot's walk up from the scale's lower end in steps of that end's decade, rounding each
   to the step's decimals, and they LOOP FOREVER (an array that grows until the tab dies: 'Invalid array length') when the
   lower end lies below 1 with a mantissa of 9.5 or more: 9.6e-6 + 1e-6 rounds to 1.1e-5, that becomes the next step, it is
   not one of uPlot's round increments, so the rounding falls back to whole numbers and every later tick is 0. uPlot's own
   zoom never meets this, because it rounds a log window's lower end down first, and that same rounding is what made a log
   axis unzoomable here (see the x scale below). Found by panning in a real browser (tools/browser_check.py).
   So: 1..9 of every decade, or round linear steps once less than about a decade is in view; and the labels thinned to what
   fits, where uPlot's filter would have gone by the leading digit of ticks it no longer chooses. */
function logSplits(mn,mx){
 if(!(mn>0)||!(mx>mn)||!isFinite(mx))return[];
 const a=Math.log10(mn),b=Math.log10(mx),out=[],lo=mn*(1-1e-12),hi=mx*(1+1e-12);
 if(b-a<1.2){
  const raw=(mx-mn)/6,p=Math.pow(10,Math.floor(Math.log10(raw))),m=raw/p,st=(m<1.5?1:m<3.5?2:m<7.5?5:10)*p;
  for(let k=Math.ceil(mn/st);k*st<=hi&&out.length<64;k++){const v=+(k*st).toPrecision(12);if(v>=lo)out.push(v);}
  return out;}
 for(let e=Math.floor(a);e<=Math.ceil(b);e++)for(let m=1;m<=9;m++){const v=+(m*Math.pow(10,e)).toPrecision(12);if(v>=lo&&v<=hi)out.push(v);}
 return out;}
function logLabels(u,sp,isX){
 const sc=u.scales[isX?'x':'y'],d=Math.log10(sc.max/sc.min);
 if(!(d>=1.2))return sp;                                          /* linear steps: every one is labelled */
 const px=((isX?u.over.clientWidth:u.over.clientHeight)||300)/d,need=isX?46:20;    /* pixels per decade; what one label wants */
 let ms,every=1;
 if(px>=19.6*need)ms=[1,2,3,4,5,6,7,8,9];else if(px>=3.3*need)ms=[1,2,5];else if(px>=2.1*need)ms=[1,3];else{ms=[1];every=Math.max(1,Math.ceil(need/px));}
 return sp.map(v=>{if(v==null)return null;const e=Math.floor(Math.log10(v)+1e-9),m=Math.round(v/Math.pow(10,e));
  return ms.indexOf(m)>=0&&(every===1||((e%every)+every)%every===0)?v:null;});}
const logAxis=isX=>({splits:(u,ai,mn,mx)=>logSplits(mn,mx),filter:(u,sp)=>logLabels(u,sp,isX)});

function build(s,holder,kind,capEl,syncKey,height){
 const names=s.threads==1?NAMES1:NAMESP,lg=MODE==='log',per=kind==='nsel',ylog=lg&&!per;
 const rows=visible(s);if(!rows.length)return null;
 const xs=rows.map(r=>r.n),fv=per?fmtNs:fmtMs;
 const yv=(r,k)=>{const t=r.t[k];if(!(t>0))return null;if(!per)return t;return r.n>0?t*1e6/r.n:null;};
 const data=[xs],series=[{label:'n',value:(u,v)=>v==null?'–':fmtN(v)}],meta=[null];
 [0,2,3,4,5,6,1].forEach(k=>{
  if(!rows.some(r=>yv(r,k)!==null))return;
  data.push(rows.map(r=>yv(r,k)));
  const nm=names[k]||('series '+k),ex=exponent(s.rows,k),bad=s.rows.some(r=>r.t[k]>0&&!r.ok);
  meta.push({name:nm,ref:false,logos:k===1,color:cssv(CVAR[k])});
  series.push({label:nm+(ex!==null?' ~n^'+ex.toFixed(2):'')+(bad?' ✗':''),
   stroke:cssv(CVAR[k]),width:k===1?2.5:1.75,points:{show:true,size:6},
   value:(u,v)=>v==null?'–':fv(v)});
 });
 if(data.length<2)return null;
 const capBase=capEl?capEl.textContent:'';
 const cr=capRule(per,ylog,data.slice(1).map(c=>c.filter(v=>v!==null)),meta.slice(1).map(m=>m.name));
 const cap=cr.cap;if(cr.text&&capEl)capEl.textContent=cr.text;
 /* references, anchored at logos's largest VISIBLE point; below 100 elements a growth curve says nothing */
 const lr=rows.filter(r=>r.t[1]>0&&r.n>=100);
 if(lr.length){const L=lr[lr.length-1];
  [['O(n)',n=>n],['O(n log n)',n=>n*Math.log2(n)]].forEach(([nm,f])=>{
   const a=L.t[1]/f(L.n);
   data.push(xs.map(n=>{if(n<2)return null;const t=a*f(n),y=per?t*1e6/n:t;return y>0&&isFinite(y)?y:null;}));
   meta.push({name:nm,ref:true,logos:false,color:cssv('--muted')});
   series.push({label:nm,stroke:cssv('--muted'),width:1.25,dash:[5,4],points:{show:false},value:(u,v)=>v==null?'–':fv(v)});});}
 const ax={stroke:cssv('--muted'),grid:{stroke:cssv('--grid'),width:1},ticks:{stroke:cssv('--axis'),width:1},font:'11px system-ui',labelFont:'12px system-ui'};
 const yscale=ylog?{distr:3}:{distr:1,range:(u,mn,mx)=>[0,Math.min((mx||1)*1.05,cap)]};
 const opts={
  class:per?'ch-nsel':'ch-time',
  width:Math.max(260,holder.clientWidth||520),height:height||(per?240:300),padding:[10,10,0,0],
  /* uPlot passes EVERY n window through the x scale's range function, explicit ones included, and its default for a log
     scale rounds the far end up to the next decade: a zoom to 100 .. 25k came back as 100 .. 100k, and nothing inside
     one decade (the n <= 10 view) could be zoomed at all. Found by driving the real thing (tools/browser_check.py);
     the stub had taken an explicit range at its word, and models the rounding now. So: the window that was asked for. */
  scales:{x:{time:false,distr:lg?3:1,range:(u,mn,mx)=>mn==null||mx>mn?[mn,mx]:lg?[mn/2,mx*2]:[mn-1,mx+1]},y:yscale},
  /* On a log axis uPlot passes null for every split it wants gridded but NOT labelled, so both
     formatters have to hand null straight back or they throw on the first minor tick. */
  axes:[Object.assign({},ax,{values:(u,v)=>v.map(x=>x==null?null:fmtN(x)),label:'n (elements)',labelSize:28},lg?logAxis(true):{}),
        Object.assign({},ax,{values:(u,v)=>v.map(x=>x==null?null:fv(x)),size:66},ylog?logAxis(false):{})],
  series,
  /* uPlot draws the selection and keeps the two charts of a panel on one cursor; what a selection MEANS is decided in
     IX (drag.setScale is off). A drag that stays within 30 px of a line zooms one axis, a diagonal one zooms both. */
  cursor:{drag:{x:true,y:true,uni:30,dist:6,setScale:false},focus:{prox:24},points:{size:9},
   sync:{key:syncKey,setSeries:true,scales:['x',null]},
   bind:{mousedown:(u,targ,handler)=>e=>IX.down(u,targ,handler,e),
         dblclick:(u,targ)=>e=>{if(e.target===targ)IX.reset(u);}}},
  legend:{live:true},
  hooks:{ready:[u=>{IX.lim(u);}],setSelect:[u=>{IX.selected(u);}],setCursor:[u=>{IX.tip(u);}],
         setSeries:[(u,si,o)=>{if(o&&o.show!=null)IX.refit(u);}]},
 };
 const u=new uPlot(opts,data,holder);
 u._ix={per,ylog,fv,meta,capEl,capBase,capHome:capEl?capEl.textContent:''};
 IX.wire(u,(per?'nanoseconds per element':'time per sort')+', '+s.shape+', '+(s.threads==1?'1 thread':s.threads+' threads'));
 return u;
}

/* ---------------------------------------------------------------- interaction
   Every change of view goes through setView(), in SCALE space (log10 of the value on a log axis), so the wheel, the
   keys, the buttons, a drag selection, a pan and a pinch all obey the same limits: never outside the measured sizes,
   never narrower than the smallest gap between two of them. uPlot's own zoom is off, because its automatic value
   range reads only the points INSIDE the window, and between two sizes there are none: the axis it then returns is
   garbage. fitY() reads the neighbours just outside the window too, so every visible segment fits, and it applies the
   run-off-the-top rules to the window the way build() applies them to a size preset. The two charts of a panel share
   their n window (and, through uPlot's sync, their cursor, their hidden series and the selection as it is dragged).
   A plain wheel over a chart scrolls the PAGE: there are dozens of charts on it. It zooms with Ctrl (which is also
   what a trackpad pinch sends), or after the chart has been clicked, until the pointer leaves it. */
let DRAG='zoom';
const IX=(()=>{
 const TR=sc=>sc.distr===3?{f:Math.log10,i:t=>Math.pow(10,t)}:{f:v=>v,i:t=>t};
 const clamp=(v,a,b)=>v<a?a:v>b?b:v,ZERO={left:0,top:0,width:0,height:0};
 const esc=t=>String(t).replace(/[&<>]/g,c=>c==='&'?'&amp;':c==='<'?'&lt;':'&gt;');
 let SRC=null,hints=2;

 /* a chart's limits in scale space, read once, while it still shows its home view (the ready hook asks first) */
 function lim(u){
  if(u._lim)return u._lim;
  const sx=u.scales.x,sy=u.scales.y;if(sx.min==null||sx.max==null||sy.min==null||sy.max==null)return null;
  const X=TR(sx),Y=TR(sy),xs=u.data[0];let gap=Infinity,lo=Infinity,hi=-Infinity;
  for(let i=1;i<xs.length;i++){const g=X.f(xs[i])-X.f(xs[i-1]);if(g>0&&g<gap)gap=g;}
  for(let s=1;s<u.data.length;s++)for(const v of u.data[s])if(v!=null){if(v<lo)lo=v;if(v>hi)hi=v;}
  const x=[X.f(sx.min),X.f(sx.max)];
  const y=sy.distr===3?[Math.min(Y.f(sy.min),Math.log10(lo)-0.3),Math.max(Y.f(sy.max),Math.log10(hi)+0.3)]
                      :[Math.min(0,sy.min),Math.max(sy.max,hi*1.05)];
  return u._lim={x,y,hx:[sx.min,sx.max],gx:isFinite(gap)?gap:x[1]-x[0]};
 }
 /* the window [a,b] brought inside [lo,hi]: no narrower than minSpan, moved rather than cut where it sticks out */
 function win(a,b,lo,hi,minSpan){
  if(a>b){const t=a;a=b;b=t;}
  let w=b-a;if(!(w<hi-lo))return[lo,hi];
  if(w<minSpan){const c=(a+b)/2;w=Math.min(minSpan,hi-lo);a=c-w/2;b=c+w/2;}
  if(a<lo){a=lo;b=lo+w;}if(b>hi){b=hi;a=hi-w;}
  return[a,b];
 }
 const atHome=(L,xw)=>{const e=1e-9*Math.max(1,Math.abs(L.x[1]-L.x[0]));return xw[0]<=L.x[0]+e&&xw[1]>=L.x[1]-e;};
 function zoomed(u){const L=lim(u);if(!L)return false;const X=TR(u.scales.x);return!atHome(L,[X.f(u.scales.x.min),X.f(u.scales.x.max)]);}

 /* the value range (scale space) of everything visible over the n window [x0,x1], the neighbours just outside included */
 function fitY(u,x0,x1){
  const ix=u._ix,xs=u.data[0],n=xs.length;let i0=0,i1=n-1;
  while(i0<n-1&&xs[i0+1]<=x0)i0++;
  while(i1>0&&xs[i1-1]>=x1)i1--;
  const cols=[],names=[];let mn=Infinity,mx=-Infinity;
  for(let si=1;si<u.data.length;si++){
   if(!u.series[si].show)continue;
   const d=u.data[si],col=[];let a=i0,b=i1;
   while(a>0&&d[a]==null)a--;while(b<n-1&&d[b]==null)b++;
   for(let i=a;i<=b;i++){const v=d[i];if(v==null)continue;col.push(v);if(v<mn)mn=v;if(v>mx)mx=v;}
   if(!ix.meta[si].ref){cols.push(col);names.push(ix.meta[si].name);}
  }
  if(!(mx>-Infinity))return null;
  const cr=capRule(ix.per,ix.ylog,cols,names);
  if(ix.capEl)ix.capEl.textContent=cr.text||ix.capBase;
  if(ix.ylog){const a=Math.log10(mn),b=Math.log10(mx),pad=Math.max(0.05*(b-a),0.04);return[a-pad,b+pad];}
  const top=Math.min(mx*1.05,cr.cap);return[0,top>0?top:1];
 }

 /* xr, yr: windows in scale space, or null. No yr = fit the value axis to the n window; the home window gets uPlot's
    own automatic range back, so a reset looks exactly like the first draw. */
 function setView(u,xr,yr,mirror){
  const L=lim(u);if(!L||!u._ix)return false;
  const sx=u.scales.x,sy=u.scales.y,X=TR(sx),Y=TR(sy);
  const xw=xr?win(xr[0],xr[1],L.x[0],L.x[1],L.gx):null,home=xw?atHome(L,xw):!zoomed(u);
  let x0=sx.min,x1=sx.max;
  if(xw){x0=xw[0]<=L.x[0]?L.hx[0]:X.i(xw[0]);x1=xw[1]>=L.x[1]?L.hx[1]:X.i(xw[1]);}
  let yw=null,yauto=false;
  if(yr)yw=win(yr[0],yr[1],L.y[0],L.y[1],(L.y[1]-L.y[0])*1e-7);
  else if(home){yauto=true;if(u._ix.capEl)u._ix.capEl.textContent=u._ix.capHome;}
  else yw=fitY(u,x0,x1);
  u.batch(()=>{
   if(xw)u.setScale('x',{min:x0,max:x1});
   if(yauto)u.setScale('y',{min:null,max:null});
   else if(yw)u.setScale('y',{min:Y.i(yw[0]),max:Y.i(yw[1])});
  });
  if(mirror&&xw)(u._sibs||[]).forEach(v=>setView(v,[X.f(x0),X.f(x1)],null,false));
  return true;
 }
 /* f < 1 zooms in, around the value xv (or the middle of the window) */
 function zoomX(u,xv,f){const L=lim(u);if(!L)return false;const sx=u.scales.x,X=TR(sx),a=X.f(sx.min),b=X.f(sx.max);
  if(f<1){const fm=L.gx/(b-a);if(!(fm<1))return false;if(f<fm)f=fm;}       /* at the smallest window: stay, do not creep toward the pointer */
  let c=xv==null?NaN:X.f(xv);if(!isFinite(c))c=(a+b)/2;c=clamp(c,a,b);
  return setView(u,[c-(c-a)*f,c+(b-c)*f],null,true);}
 function zoomY(u,yv,f){const L=lim(u);if(!L)return false;const sy=u.scales.y,Y=TR(sy),a=Y.f(sy.min),b=Y.f(sy.max);
  let c=yv==null?NaN:Y.f(yv);if(!isFinite(c))c=(a+b)/2;c=clamp(c,a,b);
  return setView(u,null,[c-(c-a)*f,c+(b-c)*f],false);}
 function panX(u,frac){const L=lim(u);if(!L)return false;const sx=u.scales.x,X=TR(sx),a=X.f(sx.min),b=X.f(sx.max),d=frac*(b-a);
  return setView(u,[a+d,b+d],null,true);}
 function reset(u){const L=lim(u);return L?setView(u,[L.x[0],L.x[1]],null,true):false;}
 function refit(u){if(zoomed(u)){const X=TR(u.scales.x);setView(u,[X.f(u.scales.x.min),X.f(u.scales.x.max)],null,false);}}

 /* uPlot has finished drawing a selection. Its panel twin mirrors the box and fires this hook as well: only the chart
    the mouse went down on acts. A box as tall as the plot is a range of n, one as wide as the plot a range of values. */
 function selected(u){
  const s=u.select;if(!(s.width>0||s.height>0))return;
  if(SRC!==u||!u._ix){u.setSelect(ZERO,false);return;}
  const W=u.over.clientWidth||s.width,H=u.over.clientHeight||s.height;
  const xOnly=s.top<=0.5&&s.height>=H-1,yOnly=s.left<=0.5&&s.width>=W-1,X=TR(u.scales.x),Y=TR(u.scales.y);
  const xr=yOnly&&!xOnly?null:[X.f(u.posToVal(s.left,'x')),X.f(u.posToVal(s.left+s.width,'x'))];
  const yr=xOnly?null:[Y.f(u.posToVal(s.top+s.height,'y')),Y.f(u.posToVal(s.top,'y'))];
  u.setSelect(ZERO,false);
  if(xOnly&&yOnly)return;
  setView(u,xr,yr,true);
 }
 /* mousedown on the plot: Shift, the middle button or the 'drag pans' switch grab the view, anything else is uPlot's */
 function down(u,targ,handler,e){
  if(e.target!==targ)return;
  SRC=u;u._mf=true;
  if(targ.focus&&document.activeElement!==targ)targ.focus({preventScroll:true});
  if(e.button===1||(e.button===0&&(e.shiftKey||DRAG==='pan'))){pan(u,e);return;}
  if(e.button===0)handler(e);
 }
 function pan(u,e){
  const L=lim(u);if(!L)return;e.preventDefault();
  const r=u.over.getBoundingClientRect(),sx=u.scales.x,sy=u.scales.y,X=TR(sx),Y=TR(sy);
  const ax=X.f(sx.min),bx=X.f(sx.max),ay=Y.f(sy.min),by=Y.f(sy.max),x0=e.clientX,y0=e.clientY;
  u.over.classList.add('grabbing');
  const mv=ev=>{const dx=(ev.clientX-x0)/(r.width||1)*(bx-ax),dy=(ev.clientY-y0)/(r.height||1)*(by-ay);
   setView(u,[ax-dx,bx-dx],[ay+dy,by+dy],true);};
  const up=()=>{document.removeEventListener('mousemove',mv);document.removeEventListener('mouseup',up);u.over.classList.remove('grabbing');};
  document.addEventListener('mousemove',mv);document.addEventListener('mouseup',up);
 }
 function hint(u){
  if(hints<=0||!u._hint)return;hints--;
  const h=u._hint;h.style.opacity='1';clearTimeout(h._t);h._t=setTimeout(()=>{h.style.opacity='0';},1800);
 }
 function wheel(u,e){
  if(!u._ix)return;
  const r=u.over.getBoundingClientRect(),k=e.deltaMode===1?33:e.deltaMode===2?300:1,dy=e.deltaY*k,dx=e.deltaX*k;
  const zoomKey=e.ctrlKey||e.metaKey||e.altKey,active=document.activeElement===u.over,side=Math.abs(dx)>Math.abs(dy);
  if(e.shiftKey||(side&&!zoomKey)){                 /* along n: Shift + wheel (which some browsers turn into a sideways delta) or a sideways scroll */
   const d=side?dx:dy;if(!d||!zoomed(u))return;
   e.preventDefault();panX(u,clamp(d,-300,300)/600);return;}
  if(!zoomKey&&!active){hint(u);return;}            /* the page scrolls */
  if(!dy)return;
  const onAxis=e.clientX<r.left;
  if(!zoomKey&&dy>0&&!onAxis&&!zoomed(u))return;    /* an active chart at its home view has nothing to zoom out of: the page scrolls */
  e.preventDefault();
  const f=Math.exp(clamp(dy,-300,300)*0.0025);
  if(onAxis)zoomY(u,u.posToVal(clamp(e.clientY-r.top,0,r.height),'y'),f);
  else zoomX(u,u.posToVal(clamp(e.clientX-r.left,0,r.width),'x'),f);
 }
 function key(u,e){
  if(e.ctrlKey||e.metaKey||e.altKey)return;
  const k=e.key,c=u._in&&u.cursor.left>=0?u.posToVal(u.cursor.left,'x'):null;let done=true;
  if(k==='+'||k==='=')zoomX(u,c,0.7);else if(k==='-'||k==='_')zoomX(u,c,1/0.7);
  else if(k==='ArrowLeft')panX(u,e.shiftKey?-0.5:-0.15);else if(k==='ArrowRight')panX(u,e.shiftKey?0.5:0.15);
  else if(k==='0'||k==='Home')reset(u);else if(k==='Escape')u.over.blur();else done=false;
  if(done)e.preventDefault();
 }
 /* every sort at the n under the pointer, fastest first, with its time relative to logos */
 function tip(u){
  const ix=u._ix,t=u._tip;if(!ix||!t)return;
  const c=u.cursor,i=c.idx;
  if(!u._in||i==null||!(c.left>=0)){t.style.display='none';return;}
  const rows=[];let L=null;
  for(let si=1;si<u.data.length;si++){const m=ix.meta[si],v=u.data[si][i];if(m.ref||!u.series[si].show||v==null)continue;rows.push([v,m]);if(m.logos)L=v;}
  if(!rows.length){t.style.display='none';return;}
  rows.sort((a,b)=>a[0]-b[0]);
  t.innerHTML='<b>n = '+Number(u.data[0][i]).toLocaleString('en-US')+'</b><table><tr><th></th><th>'+(ix.per?'per element':'per sort')+'</th><th>'+(L?'× logos':'')+'</th></tr>'+
   rows.map(([v,m])=>'<tr><td><i class="sw" style="background:'+m.color+'"></i>'+esc(m.name)+'</td><td>'+ix.fv(v)+'</td><td>'+(L?(v/L).toFixed(2):'')+'</td></tr>').join('')+'</table>';
  t.style.display='block';
  const W=u.over.clientWidth||0,H=u.over.clientHeight||0,tw=t.offsetWidth||0,th=t.offsetHeight||0;
  let x=c.left+16;if(x+tw>W)x=c.left-16-tw;if(x<0)x=Math.max(0,W-tw);
  t.style.left=x+'px';t.style.top=clamp(c.top-th/2,0,Math.max(0,H-th))+'px';
 }
 /* touch: two fingers pinch and carry the n window, one finger carries it sideways once zoomed; up and down stays the page's */
 function touch(u,o){
  let g=null;
  const two=t=>({x:(t[0].clientX+t[1].clientX)/2,d:Math.hypot(t[0].clientX-t[1].clientX,t[0].clientY-t[1].clientY)||1});
  o.addEventListener('touchstart',e=>{
   const L=lim(u),t=e.touches;if(!L||!t.length)return;
   const X=TR(u.scales.x);g={a:X.f(u.scales.x.min),b:X.f(u.scales.x.max),r:o.getBoundingClientRect(),n:t.length,gx:L.gx};
   if(t.length>=2){const m=two(t);g.x=m.x;g.d=m.d;}else g.x=t[0].clientX;},{passive:true});
  o.addEventListener('touchmove',e=>{
   if(!g)return;const t=e.touches,w=g.b-g.a,pw=g.r.width||1;
   if(g.n>=2&&t.length>=2){const m=two(t),f=Math.max(g.d/m.d,Math.min(1,g.gx/w)),c=g.a+(g.x-g.r.left)/pw*w,sh=(m.x-g.x)/pw*w*f;
    e.preventDefault();setView(u,[c-(c-g.a)*f-sh,c+(g.b-c)*f-sh],null,true);}
   else if(g.n===1&&t.length===1&&zoomed(u)){e.preventDefault();const sh=(t[0].clientX-g.x)/pw*w;setView(u,[g.a-sh,g.b-sh],null,true);}},{passive:false});
  const end=()=>{g=null;};o.addEventListener('touchend',end);o.addEventListener('touchcancel',end);
 }
 function wire(u,label){
  const o=u.over,w=(u.root&&u.root.querySelector&&u.root.querySelector('.u-wrap'))||u.root||o;
  o.setAttribute('tabindex','0');o.setAttribute('role','application');
  o.setAttribute('aria-label',label+'. Interactive chart: arrow keys move along n, plus and minus zoom, 0 resets.');
  o.style.touchAction='pan-y';
  const t=document.createElement('div');t.className='tip';o.appendChild(t);u._tip=t;
  const h=document.createElement('div');h.className='whint';h.textContent='Ctrl + scroll zooms · or click the chart, then scroll';o.appendChild(h);u._hint=h;
  w.addEventListener('wheel',e=>wheel(u,e),{passive:false});
  o.addEventListener('keydown',e=>key(u,e));
  o.addEventListener('mouseenter',()=>{u._in=true;});
  o.addEventListener('mouseleave',()=>{u._in=false;t.style.display='none';if(u._mf){u._mf=false;if(o.blur)o.blur();}});
  o.addEventListener('focus',()=>o.classList.add('act'));
  o.addEventListener('blur',()=>o.classList.remove('act'));
  touch(u,o);
 }
 return{lim,win,fitY,setView,zoomX,zoomY,panX,reset,refit,zoomed,selected,down,wheel,key,tip,wire};
})();

const CH=[],WIDE=new Set();
const hgt=(kind,wide)=>kind==='nsel'?(wide?320:240):(wide?440:300);
function render(){
 CH.forEach(c=>c.u.destroy());CH.length=0;
 const host=document.getElementById('charts');host.innerHTML='';
 const pending=[];
 for(let i=0;i<DATA.length;i+=2){
  const s1=DATA[i],sp=DATA[i+1];
  const h2=document.createElement('h2');h2.id='shape-'+(i/2);h2.textContent=s1.shape;host.appendChild(h2);
  const g=document.createElement('div');g.className='grid2';host.appendChild(g);
  [s1,sp].forEach(s=>{
   const key=s.shape+'|'+s.threads,wide=WIDE.has(key);
   const card=document.createElement('div');card.className='card'+(wide?' wide':'');
   const mine=()=>CH.filter(c=>c.card===card);
   const hd=document.createElement('div');hd.className='hd';
   const h3=document.createElement('h3');h3.textContent=s.threads==1?'1 thread each':s.threads+' threads each';hd.appendChild(h3);
   /* one set of buttons per panel: its two charts share their n window, so the first chart stands for both */
   [['−','zoom out',u=>IX.zoomX(u,null,1/0.6)],['+','zoom in',u=>IX.zoomX(u,null,0.6)],['reset','back to every size',u=>IX.reset(u)]].forEach(([txt,title,fn])=>{
    const b=document.createElement('button');b.className='t';b.textContent=txt;b.title=title;b.setAttribute('aria-label',title);
    b.onclick=()=>{const m=mine();if(m.length)fn(m[0].u);};hd.appendChild(b);});
   {const b=document.createElement('button');b.className='t';b.dataset.wide='1';b.textContent=wide?'shrink':'expand';b.title='give this panel the full width';b.setAttribute('aria-pressed',wide);
    b.onclick=()=>{const on=!WIDE.has(key);if(on)WIDE.add(key);else WIDE.delete(key);
     card.className='card'+(on?' wide':'');b.textContent=on?'shrink':'expand';b.setAttribute('aria-pressed',on);
     mine().forEach(c=>{c.h=hgt(c.kind,on);c.u.setSize({width:Math.max(260,c.holder.clientWidth||520),height:c.h});});
     CH.forEach(c=>{if(c.u.syncRect)c.u.syncRect(true);});};       /* every chart below has moved: uPlot caches where its plot is */
    hd.appendChild(b);}
   card.appendChild(hd);
   const c1=document.createElement('p');c1.className='hint';c1.textContent='time per sort';card.appendChild(c1);
   const holder=document.createElement('div');card.appendChild(holder);
   const c2=document.createElement('p');c2.className='hint cap2';c2.textContent='nanoseconds per element (value axis always linear)';card.appendChild(c2);
   const holder2=document.createElement('div');card.appendChild(holder2);
   const det=document.createElement('div');det.innerHTML=table(s);card.appendChild(det);
   g.appendChild(card);
   if(s.rows.length){pending.push([s,holder,'time',c1,card,key,wide]);pending.push([s,holder2,'nsel',c2,card,key,wide]);}});}
 /* instantiate only after the DOM is laid out, so clientWidth is real */
 pending.forEach(([s,holder,kind,capEl,card,key,wide])=>{const h=hgt(kind,wide),u=build(s,holder,kind,capEl,key,h);if(u)CH.push({u,holder,h,card,kind});});
 CH.forEach(c=>{c.u._sibs=CH.filter(d=>d.card===c.card&&d!==c).map(d=>d.u);});
}

const fl=document.getElementById('filters');
const sep=()=>{const sp=document.createElement('span');sp.className='sep';fl.appendChild(sp);};
[['lin','linear axes'],['log','log-log axes']].forEach(([v,l])=>{const b=document.createElement('button');b.className='f';b.textContent=l;b.setAttribute('aria-pressed',v===MODE);
 b.onclick=()=>{MODE=v;fl.querySelectorAll('button[data-ax]').forEach(x=>x.setAttribute('aria-pressed',x===b));render();};b.dataset.ax=v;fl.appendChild(b);});
sep();
/* which sizes are drawn: a linear n axis puts everything below a million into its first pixels, so the small end gets its own views */
[['all','all sizes'],['k10','n ≤ 10k'],['t10','n ≤ 10']].forEach(([v,l])=>{const b=document.createElement('button');b.className='f';b.textContent=l;b.setAttribute('aria-pressed',v===RANGE);
 b.onclick=()=>{RANGE=v;fl.querySelectorAll('button[data-rg]').forEach(x=>x.setAttribute('aria-pressed',x===b));render();};b.dataset.rg=v;fl.appendChild(b);});
sep();
/* what a plain drag does. Shift + drag and the middle button always pan; this switch is for a mouse hand that wants to pan all the time */
[['zoom','drag zooms'],['pan','drag pans']].forEach(([v,l])=>{const b=document.createElement('button');b.className='f';b.textContent=l;b.setAttribute('aria-pressed',v===DRAG);
 b.onclick=()=>{DRAG=v;fl.querySelectorAll('button[data-dm]').forEach(x=>x.setAttribute('aria-pressed',x===b));document.getElementById('charts').className=v==='pan'?'pan':'';};b.dataset.dm=v;fl.appendChild(b);});
sep();
{const b=document.createElement('button');b.className='f';b.textContent='reset all';b.title='every chart back to every size, every series shown';b.onclick=()=>render();fl.appendChild(b);}
{const j=document.createElement('select');j.className='f';j.setAttribute('aria-label','jump to a shape');
 j.innerHTML='<option value="">jump to a shape…</option>'+DATA.filter((s,i)=>i%2===0).map((s,i)=>'<option value="'+i+'">'+s.shape+'</option>').join('');
 j.onchange=()=>{const t=document.getElementById('shape-'+j.value);if(t&&t.scrollIntoView)t.scrollIntoView({behavior:'smooth',block:'start'});j.value='';};fl.appendChild(j);}

render();
let rt;addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(()=>CH.forEach(c=>c.u.setSize({width:Math.max(260,c.holder.clientWidth||520),height:c.h})),120);});
const mq=matchMedia('(prefers-color-scheme: dark)');
(mq.addEventListener?mq.addEventListener.bind(mq,'change'):mq.addListener.bind(mq))(()=>render());
/* for tools/check_report.js and tools/browser_check.py, which drive the charts through these */
globalThis.__SCALE={CH,IX};
</script></body></html>
)HTML";
    std::string html(TMPL);
    size_t p = html.find("__DATA__"); if (p != std::string::npos) html.replace(p, 8, j);
    p = html.find("__META__"); if (p != std::string::npos) html.replace(p, 8, meta_s);
    /* uPlot is inlined, not fetched: these reports are opened from disk and passed around, so the
       page must not depend on a CDN. See engines/c/vendor/README.md and tools/embed_uplot.py. */
    p = html.find("__UPLOT_CSS__"); if (p != std::string::npos) html.replace(p, 13, UPLOT_CSS);
    p = html.find("__UPLOT_JS__");  if (p != std::string::npos) html.replace(p, 12, UPLOT_JS);
    FILE *f = fopen("scale-report.html", "w");
    if (f) { fwrite(html.data(), 1, html.size(), f); fclose(f); printf("\nwrote scale-report.html\n"); }
    printf("RESULT: %s\n", reuse ? "re-rendered; the measurements are those of the report it was read from" : bad ? "OUTPUT MISMATCHES — do not trust timings" : "all outputs verified against std::sort");
    return bad ? 1 : 0;
}
