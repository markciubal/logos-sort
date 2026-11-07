/* scalebench.cpp — time against n for every contender, every shape, at 1 thread and at P.
 *
 * Sizes 0, 1, 3, 5, 10, then half-decade steps 32, 100, ... up to LOGOS_MAX_N (default 1e7, and the ceiling is itself
 * a point), thirteen shapes (nine standard, four from the twist-and-knot rounds), and since 2026-10-11 four STRING shapes
 * (uuids, words, urls, logs: pointers to NUL-terminated strings in strcmp order, bench_strings.h; LOGOS_SCALE_STRINGS=0
 * skips them), each panel naming its own contenders: std::sort / __gnu_parallel::sort and IPS4o with strcmp, logos
 * (logos_sort_strings_mt), x86-simd-sort's key-value sort under the same 8-byte-prefix scheme, and the multikey quicksort
 * logos used before. Single-threaded section: std::sort, logos, logos in-place (no scratch at all), x86-simd-sort
 * (-DHAVE_XSS, OpenMP threads set to 1), Google's VQSort from the Highway library (-DHAVE_VQSORT, since 2026-11-05: one thread
 * only, it has no parallel form; on strings, its key-value sort under the same 8-byte-prefix scheme), IPS4o sequential
 * (-DHAVE_IPS4O), LSD radix, timsort.
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
#include <sys/stat.h>
#include <ctime>
#include <atomic>
#include <malloc.h>
#ifdef HAVE_XSS
#define XSS_USE_OPENMP 1
#include "x86simdsort-static-incl.h"
#endif
#ifdef HAVE_IPS4O
#include "ips4o.hpp"
#endif
#ifdef HAVE_XSS
#define BENCH_STRINGS_XSS 1
#endif
#ifdef HAVE_VQSORT
#include "hwy/contrib/sort/vqsort.h"
#include "hwy/targets.h"
#define BENCH_STRINGS_VQ 1
#endif
#include "bench_strings.h"            /* the string shapes and the prefix scheme, shared with stringbench */

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
/* ---- "the shapes": every shape described, and shown from its OWN generator ----
   Built when the report is written, so a LOGOS_SCALE_REUSE re-render carries it too, from the same gen() / gen_strings()
   the timing used (seed 7) at DEMO_N elements: the whole array at DEMO_OV evenly spaced positions, DEMO_W consecutive
   elements around the feature that defines the shape, DEMO_T of their values as text, and a profile counted over all
   DEMO_N elements. A string has no numeric value, so strings are plotted by RANK, their position in sorted order.
   The descriptions are of the DATA, not of the route logos takes: routes move from round to round, and a trace build
   (`make knottrace`, `make tstrace`, ... with `once`) is where to read them. */
static const size_t DEMO_N = (size_t)1 << 20, DEMO_OV = 512, DEMO_W = 48, DEMO_T = 12, DEMO_TS = 10;
struct ShapeDoc { const char *shape, *brief, *desc; };
static const ShapeDoc SHAPE_DOCS[] = {
    { "uniform", "random doubles: no order, no repeats, no skew",
      "Doubles drawn uniformly from −1e9 to 1e9 and laid out in random order. There is nothing to exploit: no runs, no repeated "
      "values, no skew. It is the case general-purpose sorts are built for, and the baseline every other shape is read against." },
    { "near_sorted", "sorted, then 1% of positions swapped at random",
      "The values 0, 1, 2, … n−1 in order, then n/100 random pairs of positions swapped, so about 2% of the elements sit far "
      "from where they belong and the rest are already in place. Models an index or a log that was sorted once and then lightly "
      "disturbed. Every value is an integer, and the set of values is exactly 0 … n−1." },
    { "few_distinct", "five values, repeated in random order",
      "Every element is one of five values (0, 1e6, 2e6, 3e6, 4e6), chosen at random, so each fills about a fifth of the "
      "array. Models status codes, categories or any low-cardinality column: a sort that counts instead of comparing can "
      "finish in one pass." },
    { "heavy_tail", "Pareto: mostly small, a few enormous",
      "A Pareto distribution with shape 1.2 and minimum 1000: half of the values are under about 1,800, and the largest are "
      "100,000 times the smallest. Models file sizes, prices, incomes and latencies. Dividing the value range evenly puts "
      "nearly everything into the first bucket, which is why the plots use a log value axis." },
    { "bimodal", "two tight clusters, 2e9 apart",
      "Each value is about −1e9 or about +1e9 (a coin flip), plus a random offset under 10, in random order. Two dense "
      "clusters with an enormous empty gap between them: a sort that divides the value range evenly puts almost everything "
      "into two buckets and still has to sort inside each cluster." },
    { "quantized", "4,096 grid values, multiples of 1/64",
      "Multiples of 1/64 from 0 to 63.984375, chosen at random: 4,096 distinct values on an exact grid, each repeated about "
      "n/4096 times. Models readings or prices with a fixed resolution. Too many values to call few, but a sort that "
      "recognizes the grid can count instead of comparing." },
    { "int_wide", "random 32-bit integers, stored as doubles",
      "Random signed 32-bit integers over their whole range (about ±2.1e9), stored as doubles. Every value is an integer, "
      "but there are far too many distinct values to count. Models wide IDs and hash values; it asks whether a sort notices "
      "that 4-byte keys would do." },
    { "block_perm", "64 sorted blocks, shuffled",
      "The array is cut into 64 equal blocks of length L = n/64. Block b holds b·L, b·L + 1, … plus a jitter under 0.5, so "
      "every block is sorted and no two blocks' value ranges overlap. Then the blocks are put in random order. Models sorted "
      "chunks or shards concatenated out of order: sorting it only moves 64 pieces, with nothing to compare inside them." },
    { "block_overlap", "64 shuffled blocks, 1 in 8 overlapping its neighbour",
      "block_perm, except that one block in eight is shifted down by a tenth of a block length, so its value range overlaps "
      "the block below it by 10%. Those eight pairs have to be merged, not just moved; the other blocks still only move." },
    { "block_twist", "64 shuffled blocks, every other one descending",
      "block_perm with the 1st, 3rd, 5th … blocks (by position) reversed, so half of the sorted runs lie in descending "
      "order. Models data in which some segments were written newest-first. A sort that only recognizes ascending runs sees "
      "half of the structure." },
    { "block_kinked", "64 shuffled blocks with 1% of adjacent pairs swapped",
      "block_perm, then n/100 adjacent pairs at even positions swapped. Each swap is a tiny local defect, a kink, that "
      "breaks a long sorted run in two, so a sort that only accepts clean runs sees thousands of short ones. Models sorted "
      "data with occasional neighbours out of order, such as two log lines in the same millisecond written the wrong way round." },
    { "sine_slope", "a rising trend, a sine wave on it, and noise",
      "a[i] = i + A·sin(2πi/T) + noise, with 32 full cycles (T = n/32), amplitude A = 0.3·T and noise uniform in ±50. "
      "The wave is steep enough that the array falls for part of every cycle, and the noise disorders every neighbourhood. "
      "Order at two scales: long rising and falling stretches, short-reach disorder inside them. Models a periodic signal on "
      "a trend, such as the daily load of a growing service." },
    { "reach_mirror", "short-reach disorder, delivered back to front",
      "a[i] = i + noise uniform in ±60, then the whole array reversed: every element is within about 120 positions of its "
      "place, but the array runs from high to low. Models a newest-first log whose records arrived slightly out of order; a "
      "sort that reads order in one direction only sees chaos." },
    { "strings: uuids", "random UUIDs: all distinct, all 36 characters",
      "UUID-formatted strings, 32 random hex digits in groups of 8-4-4-4-12. Every string is distinct, all have the same "
      "length and the first bytes are already random, so the first 8 bytes nearly settle the order: the easy case for a "
      "sort on prefixes." },
    { "strings: words", "pseudo-words, heavily repeated",
      "Pronounceable pseudo-words of 2 to 14 letters from a vocabulary of 60,000, drawn with a heavy head: a few words make "
      "up a large share of the array and most appear rarely. Models tokens from text. Many strings are identical, and a sort "
      "can only find that out by reading them to the end." },
    { "strings: urls", "URLs sharing a long common prefix",
      "https://www.<site><tld>/<1 to 4 path segments>, half of them with ?id=N. Every string begins with the same 12 bytes, so "
      "the first 8 bytes say nothing and a sort on prefixes must look deeper; sites and paths repeat with a heavy head. "
      "Models crawl lists and access logs keyed by URL." },
    { "strings: logs", "timestamped log lines, nearly in time order",
      "Lines of the form 2026-10-09T14:03:27.512Z host-07 [INFO] word word. Time advances about 1 ms per line, and 5% of the "
      "lines carry a timestamp up to 500 ms older than their neighbours' (late arrivals), so the array is nearly sorted "
      "behind a long shared prefix, the date. Models a server log written from a buffer." },
};
static std::string jstr(const char *s) {          /* a JSON string that is also safe inside a <script> element */
    std::string o = "\"";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += (char)*p; }
        else if (*p < 0x20 || *p == '<' || *p == '>' || *p == '&') { char t[8]; snprintf(t, sizeof t, "\\u%04x", *p); o += t; }
        else o += (char)*p;
    }
    return o + "\"";
}
static std::string jnums(const std::vector<double> &v, const char *fmt) {
    std::string o = "["; char t[40];
    for (size_t i = 0; i < v.size(); i++) { snprintf(t, sizeof t, fmt, v[i]); if (i) o += ','; o += t; }
    return o + "]";
}
static std::string shape_demos_json() {
    const size_t N = DEMO_N, NONE = (size_t)-1, blk = (N + 63) / 64;
    std::string o = "[";
    for (const ShapeDoc &d : SHAPE_DOCS) {
        const bool is_str = !strncmp(d.shape, "strings: ", 9);
        std::vector<double> ov(DEMO_OV), win; std::vector<size_t> hl; size_t c = NONE; char t[400];
        std::string stats, text;
        if (!is_str) {
            const std::string sh = d.shape;
            std::vector<double> a = gen(sh, N);
            size_t inord = 0; bool ints = true;
            for (size_t i = 0; i + 1 < N; i++) inord += a[i] <= a[i + 1];
            for (double v : a) if (v != std::floor(v)) { ints = false; break; }
            std::vector<double> s = a; std::sort(s.begin(), s.end());
            size_t distinct = 1; for (size_t i = 1; i < N; i++) distinct += s[i] != s[i - 1];
            snprintf(t, sizeof t, "{\"distinct\":%zu,\"min\":%.17g,\"max\":%.17g,\"inorder\":%.8f,\"runs\":%zu,\"ints\":%s}",
                     distinct, s[0], s[N - 1], (double)inord / (double)(N - 1), N - inord, ints ? "true" : "false");
            stats = t;
            /* the feature: where the shape is most itself, so the window shows it */
            if (sh == "near_sorted") { for (size_t i = 0; i < N; i++) if (a[i] != (double)i) { c = i; break; } if (c != NONE) hl = { c }; }
            else if (sh == "block_perm" || sh == "block_overlap" || sh == "block_twist") { c = blk; hl = { blk - 1, blk }; }
            else if (sh == "block_kinked") { for (size_t i = 0; i + 1 < N; i++) if (a[i] > a[i + 1] && (i + 1) % blk) { c = i; hl = { i, i + 1 }; break; } }
            else if (sh == "sine_slope") c = N / 64;          /* half a cycle in: the wave's steepest fall */
            else if (sh == "reach_mirror") c = N / 2;
            for (size_t k = 0; k < DEMO_OV; k++) ov[k] = a[k * N / DEMO_OV];
            size_t w0 = c == NONE ? 0 : c >= DEMO_W / 2 ? c - DEMO_W / 2 : 0; if (w0 + DEMO_W > N) w0 = N - DEMO_W;
            win.assign(a.begin() + (std::ptrdiff_t)w0, a.begin() + (std::ptrdiff_t)(w0 + DEMO_W));
            size_t t0 = c == NONE ? w0 : std::max(w0, c >= DEMO_T / 2 ? c - DEMO_T / 2 : 0); if (t0 + DEMO_T > w0 + DEMO_W) t0 = w0 + DEMO_W - DEMO_T;
            snprintf(t, sizeof t, ",\"w0\":%zu,\"t0\":%zu,\"tn\":%zu", w0, t0, DEMO_T); text = t;
        } else {
            StrSet S = gen_strings(d.shape + 9, N);
            const std::vector<const char *> &p = S.p;
            size_t inord = 0, lmin = (size_t)-1, lmax = 0; double lsum = 0;
            for (size_t i = 0; i + 1 < N; i++) inord += strcmp(p[i], p[i + 1]) <= 0;
            for (const char *x : p) { size_t L = strlen(x); lmin = std::min(lmin, L); lmax = std::max(lmax, L); lsum += (double)L; }
            std::vector<const char *> s = p; std::sort(s.begin(), s.end(), str_less);
            size_t distinct = 1; for (size_t i = 1; i < N; i++) distinct += strcmp(s[i], s[i - 1]) != 0;
            snprintf(t, sizeof t, "{\"distinct\":%zu,\"minLen\":%zu,\"maxLen\":%zu,\"meanLen\":%.2f,\"inorder\":%.8f,\"runs\":%zu}",
                     distinct, lmin, lmax, lsum / (double)N, (double)inord / (double)(N - 1), N - inord);
            stats = t;
            if (!strcmp(d.shape + 9, "logs")) for (size_t i = 0; i + 1 < N; i++) if (strcmp(p[i], p[i + 1]) > 0) { c = i + 1; hl = { c }; break; }
            auto rank = [&](const char *x) { return (double)(std::lower_bound(s.begin(), s.end(), x, str_less) - s.begin()); };
            for (size_t k = 0; k < DEMO_OV; k++) ov[k] = rank(p[k * N / DEMO_OV]);
            size_t w0 = c == NONE ? 0 : c >= DEMO_W / 2 ? c - DEMO_W / 2 : 0; if (w0 + DEMO_W > N) w0 = N - DEMO_W;
            for (size_t i = w0; i < w0 + DEMO_W; i++) win.push_back(rank(p[i]));
            size_t t0 = c == NONE ? w0 : std::max(w0, c >= DEMO_TS / 2 ? c - DEMO_TS / 2 : 0); if (t0 + DEMO_TS > w0 + DEMO_W) t0 = w0 + DEMO_W - DEMO_TS;
            snprintf(t, sizeof t, ",\"w0\":%zu,\"t0\":%zu,\"tn\":%zu,\"lines\":[", w0, t0, DEMO_TS); text = t;
            for (size_t i = t0; i < t0 + DEMO_TS; i++) { if (i > t0) text += ','; text += jstr(p[i]); }
            text += "]";
        }
        if (o.size() > 1) o += ",";
        o += "{\"shape\":" + jstr(d.shape) + ",\"kind\":\"" + (is_str ? "str" : "num") + "\",\"brief\":" + jstr(d.brief) + ",\"desc\":" + jstr(d.desc);
        snprintf(t, sizeof t, ",\"n\":%zu,\"step\":%zu,\"ylog\":%s,\"stats\":", N, N / DEMO_OV, !strcmp(d.shape, "heavy_tail") ? "true" : "false");
        o += t; o += stats; o += text;
        o += ",\"ov\":" + jnums(ov, is_str ? "%.0f" : "%.6g") + ",\"win\":" + jnums(win, is_str ? "%.0f" : "%.17g") + ",\"hl\":[";
        for (size_t k = 0; k < hl.size(); k++) { snprintf(t, sizeof t, "%s%zu", k ? "," : "", hl[k]); o += t; }
        o += "]}";
    }
    return o + "]";
}
/* every contender is a callable on (pointer, length), so one list serves a single large array and a batch of small ones */
/* equal outputs: doubles bit for bit as before; strings as strings (identical strings may come back in any pointer order) */
static bool same(const std::vector<double> &a, const std::vector<double> &b) { return a == b; }
static bool same(const std::vector<const char *> &a, const std::vector<const char *> &b) { return str_same(a, b); }
template <class T, class F> static double best_of(const std::vector<T> &base, int reps, F f, std::vector<T> &out, double budget_ms) {
    double b = 1e300;
    for (int i = 0; i < reps; i++) { std::vector<T> a = base; double t = nowms(); f(a.data(), a.size()); double ms = nowms() - t; if (ms < b) { b = ms; out.swap(a); } if (ms > budget_ms) break; }
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
template <class T> struct SmallT { size_t n = 0, ni = 0; std::vector<T> pristine, work, ref; std::vector<StrSet> pools; };   /* pools: the strings' storage */
typedef SmallT<double> Small;
static Small make_small(const std::string &shape, size_t n) {
    Small s; s.n = n; s.ni = n == 0 ? 4096 : std::min<size_t>(4096, std::max<size_t>(16, 262144 / n));
    s.pristine.assign(s.ni * n + 1, 0.0);                                   /* + 1: data() is never null, even for n = 0 */
    for (size_t k = 0; k < s.ni; k++) { std::vector<double> a = gen(shape, n, 7 + k); std::copy(a.begin(), a.end(), s.pristine.begin() + (std::ptrdiff_t)(k * n)); }
    s.ref = s.pristine;
    for (size_t k = 0; k < s.ni; k++) std::sort(s.ref.begin() + (std::ptrdiff_t)(k * n), s.ref.begin() + (std::ptrdiff_t)((k + 1) * n));
    s.work = s.pristine;
    return s;
}
/* the string twin of make_small: ni instances of n strings, each with its own pool, pointers end to end */
static SmallT<const char *> make_small_str(const char *shape, size_t n) {
    SmallT<const char *> s; s.n = n; s.ni = n == 0 ? 4096 : std::min<size_t>(4096, std::max<size_t>(16, 262144 / n));
    s.pristine.assign(s.ni * n + 1, nullptr); s.pools.reserve(s.ni);
    for (size_t k = 0; k < s.ni; k++) { s.pools.push_back(gen_strings(shape, n, 7 + k)); std::copy(s.pools.back().p.begin(), s.pools.back().p.end(), s.pristine.begin() + (std::ptrdiff_t)(k * n)); }
    s.ref = s.pristine;
    for (size_t k = 0; k < s.ni; k++) std::sort(s.ref.begin() + (std::ptrdiff_t)(k * n), s.ref.begin() + (std::ptrdiff_t)((k + 1) * n), str_less);
    s.work = s.pristine;
    return s;
}
template <class T, class F> static double best_small(SmallT<T> &s, F f, bool &ok, double budget_ms) {
    double best = 1e300, acc = 0;
    for (int batch = 0; batch < 25; batch++) {
        std::copy(s.pristine.begin(), s.pristine.end(), s.work.begin());
        T *w = s.work.data(); const size_t n = s.n, ni = s.ni;
        double t = nowms();
        for (size_t k = 0; k < ni; k++) f(w + k * n, n);
        double ms = nowms() - t;
        if (ms < best) best = ms;
        acc += ms;
        if ((batch >= 4 && acc > 120.0) || ms > budget_ms) break;
    }
    if (s.ni * s.n) { std::vector<T> a(s.work.begin(), s.work.begin() + (std::ptrdiff_t)(s.ni * s.n)), b(s.ref.begin(), s.ref.begin() + (std::ptrdiff_t)(s.ni * s.n)); ok &= same(a, b); }
    double per = best / (double)s.ni;
    return per > 1e-8 ? per : 1e-8;
}
static std::string jnum(double v) { char b[32]; snprintf(b, sizeof b, "%.4g", v); return b; }

/* ONE UNIT FOR THE WHOLE TABLE: NANOSECONDS, every value, every row. This console used to print nanoseconds below
   SMALL_N and milliseconds from SMALL_N up -- switching mid-column, at exactly the seam -- so the logos column for
   uniform read 27330.0 at n = 3162 and then 0.1 at n = 10000. That is a 300,000x collapse in the printed figure and
   none of it is real: per element the same three rows are 7.91, 8.64 and 9.01 ns. It was read, reasonably, as the
   sort falling off a cliff at 3162 and then beating its own n = 1 row. An auto-scaled suffix per value fixed the
   ambiguity but left a column that cannot be scanned or pasted into anything; one fixed unit can. Two decimals
   because best_small's floor is 0.01 ns (what it reports when a sort is inlined away on an empty range), and the
   widest value is a full sort of the largest n, about 2e9 ns, so %14s holds both ends with room.
   Rotating buffers because a single printf takes twelve of these. The ONE deliberate exception is the calibration
   line above the table: it is two labelled reference values rather than a column, and its whole job is comparison
   against figures recorded elsewhere in milliseconds (CLAUDE.md's quiet ~2.9 ms / ~65 ms), which nanoseconds would
   break. Everything that is a per-sort measurement is ns. */
static const char *fmt_ns(double ms) {
    static char buf[16][32];
    static int slot = 0;
    char *b = buf[slot = (slot + 1) & 15];
    snprintf(b, 32, "%.2f", ms * 1e6);
    return b;
}

/* ---- peak-heap tracer (the method of memprobe.c). Armed only inside the memory pass below, so the timed
   sorts never pay more than one relaxed load per allocation, and they make very few. */
extern "C" {
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void __libc_free(void *);
}
static std::atomic<int> sm_active{0};
static std::atomic<size_t> sm_live{0}, sm_peak{0};
static void sm_add(void *p) {
    if (!p || !sm_active.load(std::memory_order_relaxed)) return;
    size_t z = malloc_usable_size(p), v = sm_live.fetch_add(z, std::memory_order_relaxed) + z;
    size_t old = sm_peak.load(std::memory_order_relaxed);
    while (v > old && !sm_peak.compare_exchange_weak(old, v, std::memory_order_relaxed)) {}
}
static void sm_sub(void *p) { if (p && sm_active.load(std::memory_order_relaxed)) sm_live.fetch_sub(malloc_usable_size(p), std::memory_order_relaxed); }
extern "C" {
void *malloc(size_t n) { void *p = __libc_malloc(n); sm_add(p); return p; }
void *calloc(size_t n, size_t z) { void *p = __libc_calloc(n, z); sm_add(p); return p; }
void *realloc(void *p, size_t n) { sm_sub(p); void *q = __libc_realloc(p, n); if (q) sm_add(q); else if (n) sm_add(p); return q; }
int posix_memalign(void **out, size_t al, size_t n) { void *p = __libc_memalign(al, n); if (!p) return 12; sm_add(p); *out = p; return 0; }
void free(void *p) { sm_sub(p); __libc_free(p); }
}
/* one cold call, traced: bytes per element. The work array is filled BEFORE arming so it is not counted, and
   the engine's pools are released first, since a warm call allocates nothing and would read zero. */
template <class T, class F>
static double mem_of(std::vector<T> &work, const std::vector<T> &base, F f, bool is_logos) {
    std::copy(base.begin(), base.end(), work.begin());
    if (is_logos) logos_release_buffers();
    sm_live.store(0); sm_peak.store(0); sm_active.store(1);
    f(work.data(), work.size());
    sm_active.store(0);
    return (double)sm_peak.load() / (double)work.size();
}

#define NC 8                                                        /* contenders per row */
struct Row { size_t n; double t[NC]; double m[NC]; bool ok; };     /* t: [0]=std::sort or gnu_par, [1]=logos, [2]=xss, [3]=ips4o, [4]=LSD radix, [5]=timsort, [6]=logos in-place, [7]=VQSort (4, 5, 7: 1T only); 0 = absent */
/* seam[k]: n = seam_n (SMALL_N) measured the SMALL way -- a batch of distinct instances -- beside the row that
   measured the same n the big way. seam[k] / row.t[k] is the measurement-mode step, at the one size where BOTH
   modes resolve. It is not a universal correction: where it comes out near 1 and the chart still has a cliff, the
   cliff belongs to the data and not to the harness, which is exactly what it is there to let a reader tell apart. */
struct Series { std::string shape; int threads; std::vector<Row> rows; double seam[NC] = {}; size_t seam_n = 0;
                std::vector<std::string> names; std::string note; const char *str = nullptr; };   /* str: a string shape (names/note: its own) */

int main() {
    int P = bench_threads(logos_num_cpus());   /* bench_common.h: LOGOS_THREADS, else BENCH_THREADS, else physical cores */
    /* 1e7 since 2026-09-23, for turnaround: it was 5e7, and the two rows above 1e7 (31.6M and 50M) are the
       most expensive in the run by a wide margin -- every contender, every shape, at both thread counts, plus
       the cold-call memory pass. LOGOS_MAX_N=5e7 restores the old ceiling when the top of the curve is the point. */
    double max_n = 1e7; if (const char *e = getenv("LOGOS_MAX_N")) { double v = atof(e); if (v >= 1e4) max_n = v; }
    double max_sec = 20; if (const char *e = getenv("LOGOS_MAX_SEC")) { double v = atof(e); if (v > 0) max_sec = v; }
    const char *shapes[] = { "uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide", "block_perm", "block_overlap", "block_twist", "block_kinked", "sine_slope", "reach_mirror" };
    double min_n = 0; if (const char *e = getenv("LOGOS_MIN_N")) { double v = atof(e); if (v >= 0) min_n = v; }
    /* 0, 1, 3, 5 and 10 were asked for by hand: the empty call, the single element, and the sizes at which a sort is all
       fixed cost. 32 .. 3162 continue in the ladder's own half-decade steps so that the ns/element curves have no
       three-decade hole between 10 and 10k. LOGOS_MIN_N=10000 restores the old ladder. */
    std::vector<size_t> sizes;
    for (size_t n : { (size_t)0, (size_t)1, (size_t)3, (size_t)5, (size_t)10, (size_t)32, (size_t)100, (size_t)316, (size_t)1000, (size_t)3162 }) if ((double)n >= min_n) sizes.push_back(n);
    for (double n = 1e4; n <= max_n + 1; n *= 3.1622776601683795) { size_t v = (size_t)std::llround(n / 1e3) * 1000; if ((double)v >= min_n) sizes.push_back(v); }
    /* the ceiling is itself a point when the half-decade ladder stops well short of it. At the 1e7 default the ladder
       lands exactly on it (.., 3.16M, 10M) so nothing is appended; at LOGOS_MAX_N=5e7 it adds 50M after 31.6M. */
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
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   /* a checkout on Windows may have written CRLF; the block ends in a semicolon and a bare newline */
        size_t a = s.find("const DATA="), b = a == std::string::npos ? a : s.find(",META=", a), c = b == std::string::npos ? b : s.find(";\n", b);
        if (c == std::string::npos) { fprintf(stderr, "%s: no DATA/META block found\n", reuse); return 1; }
        j = s.substr(a + 11, b - (a + 11)); meta_s = s.substr(b + 6, c - (b + 6));
        /* WHEN it was measured travels with the data. A report older than the field gets the modification time of the
           file it is read from, which is when that run wrote it (or when it was last copied: the best there is). */
        if (meta_s.find("\"measuredAt\"") == std::string::npos) {
            struct stat sb; double at = stat(reuse, &sb) == 0 ? (double)sb.st_mtime * 1000.0 : 0.0;
            size_t e = meta_s.rfind('}');
            if (e != std::string::npos && at > 0) { char t[64]; snprintf(t, sizeof t, ",\"measuredAt\":%.0f", at); meta_s.insert(e, t); }
        }
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
                if (is_ref) ref.swap(o); else ok &= same(o, ref);
                return t;
            };
            /* ---- single-threaded ---- */
            omp_set_num_threads(1);
            Row r1{n, {}, {}, true};
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
#ifdef HAVE_VQSORT
            r1.t[7] = T([](double *p, size_t m) { hwy::VQSort(p, m, hwy::SortAscending()); }, r1.ok, false);
#endif
            s1.rows.push_back(r1);
            /* ---- P threads ---- */
            omp_set_num_threads(P);
            Row rp{n, {}, {}, true};
            rp.t[0] = T([](double *p, size_t m) { __gnu_parallel::sort(p, p + m); }, rp.ok, false);
            rp.t[1] = T([P](double *p, size_t m) { logos_sort_f64(p, m, P); }, rp.ok, false);
            rp.t[6] = T([P](double *p, size_t m) { logos_sort_f64_inplace(p, m, P); }, rp.ok, false);   /* parallel from 2026-09-20: placement passes as tasks */
#ifdef HAVE_XSS
            rp.t[2] = T([](double *p, size_t m) { x86simdsortStatic::qsort(p, m, false, false); }, rp.ok, false);
#endif
#ifdef HAVE_IPS4O
            rp.t[3] = T([P](double *p, size_t m) { ips4o::parallel::sort(p, p + m, std::less<double>(), P); }, rp.ok, false);   /* P threads, like every P-thread contender (the default is all hardware threads) */
#endif
            sp.rows.push_back(rp);
            if (!r1.ok || !rp.ok) bad++;
            /* Nanoseconds per sort throughout; then nanoseconds PER ELEMENT for logos, the only column that may be
               compared down the page -- and the one that shows there is no cliff where the units used to imply one. */
            printf("  %-13s n=%-9zu 1T ns: std %14s logos %14s inplace %14s xss %14s vqsort %14s ips4o %14s radix %14s tim %14s | %dT ns: gnu_par %14s logos %14s inplace %14s xss %14s ips4o %14s | logos ns/elem 1T %8.2f %dT %8.2f%s\n",
                   sh, n,
                   fmt_ns(r1.t[0]), fmt_ns(r1.t[1]), fmt_ns(r1.t[6]), fmt_ns(r1.t[2]), fmt_ns(r1.t[7]), fmt_ns(r1.t[3]), fmt_ns(r1.t[4]), fmt_ns(r1.t[5]),
                   P, fmt_ns(rp.t[0]), fmt_ns(rp.t[1]), fmt_ns(rp.t[6]), fmt_ns(rp.t[2]), fmt_ns(rp.t[3]),
                   n ? r1.t[1] * 1e6 / (double)n : 0.0, P, n ? rp.t[1] * 1e6 / (double)n : 0.0,
                   (r1.ok && rp.ok) ? "" : "  <-- MISMATCH");
            fflush(stdout);
            if (r1.t[0] > budget * 0.5) over = true;        /* the slowest contender is at half the budget: stop this shape here */
        }
        all.push_back(s1); all.push_back(sp);
    }

    /* ---- strings (2026-10-11): pointers to NUL-terminated strings in strcmp order, the four shapes that span the cases --
       high-entropy keys (uuids), short keys with heavy duplicates (words), long shared prefixes (urls), nearly in order (logs).
       Same ladder, same batching below SMALL_N, same verification (std::sort's output, compared as STRINGS). The columns keep
       their colours and take their names from the series: x86-simd-sort cannot sort strings, so its column is its key-value
       sort under the same 8-byte-prefix scheme logos uses (bench_strings.h); the radix column is the multikey quicksort
       logos_sort_strings was until 2026-10-09; timsort and the in-place entry have no string form. LOGOS_SCALE_STRINGS=0 skips. */
    static const char *sshapes[] = { "uuids", "words", "urls", "logs" };
    const bool do_str = !getenv("LOGOS_SCALE_STRINGS") || atoi(getenv("LOGOS_SCALE_STRINGS")) != 0;
    for (const char *ssh : sshapes) {
        if (!do_str) break;
        Series s1{std::string("strings: ") + ssh, 1, {}}, sp{std::string("strings: ") + ssh, P, {}};
        s1.str = sp.str = ssh;
        s1.names = { "std::sort (strcmp)", "logos", std::string("x86-simd-sort ") + xss_isa + " key-value, on 8-byte prefixes", "IPS\\u2074o (strcmp)", "multikey quicksort (logos before 2026-10-09)", "", "", "VQSort (Google) key-value, on 8-byte prefixes" };
        sp.names = { "__gnu_parallel::sort (strcmp)", "logos", std::string("x86-simd-sort ") + xss_isa + " key-value, on 8-byte prefixes (OpenMP)", "IPS\\u2074o (strcmp)", "", "", "", "" };
        s1.note = sp.note = "Strings: an array of pointers to NUL-terminated strings, sorted in strcmp order (unsigned bytes, a string before its extensions). "
                            "logos is logos_sort_strings_mt: each string's next 8 bytes become one 64-bit key, the keys are sorted as numbers, and only groups of equal keys go 8 bytes deeper. "
                            "x86-simd-sort and Google's VQSort cannot sort strings; their lines are their key-value sorts driven by the SAME prefix scheme, so they compare the kernels under one method "
                            "(VQSort takes 16-byte records, so its line also pays one pack and one unpack per call; it has no parallel form and appears at one thread only). "
                            "std::sort, __gnu_parallel::sort and IPS\\u2074o compare with strcmp. Every output is checked against std::sort's as strings. FINDINGS 3bl.";
        bool over = false;
        for (size_t n : sizes) {
            if (over) break;
            const bool small = n < SMALL_N;
            SmallT<const char *> sm; StrSet S; std::vector<const char *> base;
            if (small) sm = make_small_str(ssh, n); else { S = gen_strings(ssh, n); base = S.p; }
            int reps = n <= 1000000 ? 5 : n <= 10000000 ? 3 : 1; double budget = max_sec * 1e3;
            std::vector<const char *> ref, o;
            auto T = [&](auto f, bool &ok, bool is_ref) -> double {
                if (small) return best_small(sm, f, ok, budget);
                double t = best_of(base, reps, f, o, budget);
                if (is_ref) ref.swap(o); else ok &= same(o, ref);
                return t;
            };
            omp_set_num_threads(1);
            Row r1{n, {}, {}, true};
            r1.t[0] = T([](const char **p, size_t m) { std::sort(p, p + m, str_less); }, r1.ok, true);
            r1.t[1] = T([](const char **p, size_t m) { logos_sort_strings_mt(p, m, 1); }, r1.ok, false);
#ifdef HAVE_XSS
            r1.t[2] = T([](const char **p, size_t m) { prefix_xss_sort(p, m, false); }, r1.ok, false);
#endif
#ifdef HAVE_IPS4O
            r1.t[3] = T([](const char **p, size_t m) { ips4o::sort(p, p + m, str_less); }, r1.ok, false);
#endif
            r1.t[4] = T([](const char **p, size_t m) { if (m > 1) bs_mkqs(p, 0, (long)m - 1, 0); }, r1.ok, false);
#ifdef HAVE_VQSORT
            r1.t[7] = T([](const char **p, size_t m) { prefix_vq_sort(p, m); }, r1.ok, false);
#endif
            s1.rows.push_back(r1);
            omp_set_num_threads(P);
            Row rp{n, {}, {}, true};
            rp.t[0] = T([](const char **p, size_t m) { __gnu_parallel::sort(p, p + m, str_less); }, rp.ok, false);
            rp.t[1] = T([P](const char **p, size_t m) { logos_sort_strings_mt(p, m, P); }, rp.ok, false);
#ifdef HAVE_XSS
            rp.t[2] = T([](const char **p, size_t m) { prefix_xss_sort(p, m, true); }, rp.ok, false);
#endif
#ifdef HAVE_IPS4O
            rp.t[3] = T([P](const char **p, size_t m) { ips4o::parallel::sort(p, p + m, str_less, P); }, rp.ok, false);
#endif
            sp.rows.push_back(rp);
            if (!r1.ok || !rp.ok) bad++;
            printf("  %-15s n=%-9zu 1T ns: std %14s logos %14s xss-prefix %14s vq-prefix %14s ips4o %14s mkqs %14s | %dT ns: gnu_par %14s logos %14s xss-prefix %14s ips4o %14s | logos ns/string 1T %8.2f %dT %8.2f%s\n",
                   s1.shape.c_str(), n,
                   fmt_ns(r1.t[0]), fmt_ns(r1.t[1]), fmt_ns(r1.t[2]), fmt_ns(r1.t[7]), fmt_ns(r1.t[3]), fmt_ns(r1.t[4]),
                   P, fmt_ns(rp.t[0]), fmt_ns(rp.t[1]), fmt_ns(rp.t[2]), fmt_ns(rp.t[3]),
                   n ? r1.t[1] * 1e6 / (double)n : 0.0, P, n ? rp.t[1] * 1e6 / (double)n : 0.0,
                   (r1.ok && rp.ok) ? "" : "  <-- MISMATCH");
            fflush(stdout);
            if (r1.t[0] > budget * 0.5) over = true;
        }
        all.push_back(s1); all.push_back(sp);
    }

    /* ---- the seam pass: how big is the measurement-mode step at SMALL_N? -------------------------------------
       Below SMALL_N a size is a BATCH of distinct instances (ni = 262144/n of them, ~2 MB swept, every instance
       cache-cold and none repeated); at SMALL_N and above it is ONE array copied from a pristine base and sorted
       again and again, keeping the MINIMUM (78 KB at n = 10000, L2-resident, the same input every rep). Three
       advantages flip at that x at once -- working set, branch history, and mean-of-batch against min-of-reps --
       and all three favour the right-hand side. Eighteen series in the 2026-09-22 report peak at 3162 and fall at
       10000 BECAUSE OF THIS, across every contender, which is the tell that it is the harness and not a sort.
       So measure SMALL_N BOTH ways and publish the ratio: the curve is then splice-able instead of merely wrong,
       and a reader can see the step rather than read it as an algorithmic cliff. Costs one extra batch per
       contender per panel. LOGOS_SCALE_SEAM=0 skips it. */
    if (!getenv("LOGOS_SCALE_SEAM") || atoi(getenv("LOGOS_SCALE_SEAM")) != 0) {
        double t_seam = nowms();
        for (size_t si = 0; si < all.size(); si++) {
            Series &s = all[si];
            /* AT SMALL_N, where both modes resolve. Measuring instead at the last BATCHED size (3162) was tried and
               is worthless: one sort there is ~20 us and the big path runs 5 reps, which is precisely the resolution
               problem batching exists to solve -- gnu_par scattered from 0.01x to 2.41x across the shapes. */
            const Row *at = nullptr;
            for (const Row &r : s.rows) if (r.n == (size_t)SMALL_N) { at = &r; break; }
            if (!at) continue;
            s.seam_n = at->n;
            omp_set_num_threads(s.threads);
            bool ok = true; const double budget = max_sec * 1e3;
            const int P2 = s.threads;
            if (s.str) {                                     /* a string series: its own contenders, batched the same way */
                SmallT<const char *> ss = make_small_str(s.str, (size_t)SMALL_N);
                if (at->t[0] > 0) s.seam[0] = P2 == 1
                    ? best_small(ss, [](const char **p, size_t m) { std::sort(p, p + m, str_less); }, ok, budget)
                    : best_small(ss, [](const char **p, size_t m) { __gnu_parallel::sort(p, p + m, str_less); }, ok, budget);
                if (at->t[1] > 0) s.seam[1] = best_small(ss, [P2](const char **p, size_t m) { logos_sort_strings_mt(p, m, P2); }, ok, budget);
#ifdef HAVE_XSS
                if (at->t[2] > 0) s.seam[2] = best_small(ss, [P2](const char **p, size_t m) { prefix_xss_sort(p, m, P2 > 1); }, ok, budget);
#endif
#ifdef HAVE_IPS4O
                if (at->t[3] > 0) s.seam[3] = best_small(ss, [P2](const char **p, size_t m) { if (P2 == 1) ips4o::sort(p, p + m, str_less); else ips4o::parallel::sort(p, p + m, str_less, P2); }, ok, budget);
#endif
                if (at->t[4] > 0) s.seam[4] = best_small(ss, [](const char **p, size_t m) { if (m > 1) bs_mkqs(p, 0, (long)m - 1, 0); }, ok, budget);
#ifdef HAVE_VQSORT
                if (at->t[7] > 0) s.seam[7] = best_small(ss, [](const char **p, size_t m) { prefix_vq_sort(p, m); }, ok, budget);
#endif
                if (!ok) { printf("  seam %s %dT: MISMATCH\n", s.shape.c_str(), s.threads); bad++; }
                printf("  seam %-15s %2dT n=%-6zu batch/single  std %.2fx logos %.2fx xss-prefix %.2fx vq-prefix %.2fx ips4o %.2fx mkqs %.2fx\n",
                       s.shape.c_str(), s.threads, at->n,
                       at->t[0] > 0 ? s.seam[0] / at->t[0] : 0.0, at->t[1] > 0 ? s.seam[1] / at->t[1] : 0.0,
                       at->t[2] > 0 ? s.seam[2] / at->t[2] : 0.0, at->t[7] > 0 ? s.seam[7] / at->t[7] : 0.0,
                       at->t[3] > 0 ? s.seam[3] / at->t[3] : 0.0, at->t[4] > 0 ? s.seam[4] / at->t[4] : 0.0);
                fflush(stdout);
                continue;
            }
            Small sm = make_small(s.shape, (size_t)SMALL_N);
            if (at->t[0] > 0) s.seam[0] = P2 == 1
                ? best_small(sm, [](double *p, size_t m) { std::sort(p, p + m); }, ok, budget)
                : best_small(sm, [](double *p, size_t m) { __gnu_parallel::sort(p, p + m); }, ok, budget);
            if (at->t[1] > 0) s.seam[1] = best_small(sm, [P2](double *p, size_t m) { logos_sort_f64(p, m, P2); }, ok, budget);
#ifdef HAVE_XSS
            if (at->t[2] > 0) s.seam[2] = best_small(sm, [](double *p, size_t m) { x86simdsortStatic::qsort(p, m, false, false); }, ok, budget);
#endif
#ifdef HAVE_IPS4O
            if (at->t[3] > 0) s.seam[3] = best_small(sm, [P2](double *p, size_t m) { if (P2 == 1) ips4o::sort(p, p + m); else ips4o::parallel::sort(p, p + m, std::less<double>(), P2); }, ok, budget);
#endif
            if (at->t[4] > 0) s.seam[4] = best_small(sm, [](double *p, size_t m) { radix_sort_f64(p, m); }, ok, budget);
            if (at->t[5] > 0) s.seam[5] = best_small(sm, [](double *p, size_t m) { timsort_f64(p, m); }, ok, budget);
            if (at->t[6] > 0) s.seam[6] = best_small(sm, [P2](double *p, size_t m) { logos_sort_f64_inplace(p, m, P2); }, ok, budget);
#ifdef HAVE_VQSORT
            if (at->t[7] > 0) s.seam[7] = best_small(sm, [](double *p, size_t m) { hwy::VQSort(p, m, hwy::SortAscending()); }, ok, budget);
#endif
            if (!ok) { printf("  seam %s %dT: MISMATCH\n", s.shape.c_str(), s.threads); bad++; }
            printf("  seam %-13s %2dT n=%-6zu batch/single  std %.2fx logos %.2fx xss %.2fx vqsort %.2fx ips4o %.2fx radix %.2fx tim %.2fx inplace %.2fx\n",
                   s.shape.c_str(), s.threads, at->n,
                   at->t[0] > 0 ? s.seam[0] / at->t[0] : 0.0, at->t[1] > 0 ? s.seam[1] / at->t[1] : 0.0,
                   at->t[2] > 0 ? s.seam[2] / at->t[2] : 0.0, at->t[7] > 0 ? s.seam[7] / at->t[7] : 0.0, at->t[3] > 0 ? s.seam[3] / at->t[3] : 0.0,
                   at->t[4] > 0 ? s.seam[4] / at->t[4] : 0.0, at->t[5] > 0 ? s.seam[5] / at->t[5] : 0.0,
                   at->t[6] > 0 ? s.seam[6] / at->t[6] : 0.0);
            fflush(stdout);
        }
        printf("seam pass: %.1f s\n\n", (nowms() - t_seam) / 1e3);
    }

    /* ---- the memory pass: peak heap of one COLD call, bytes per element, for the circles on the charts.
       It runs after ALL timing on purpose. Releasing the engine's pools is what makes a call cold, and doing
       that between timed rows would have left the next row's first call paying for its buffers. Skipped below
       SMALL_N, where a size is a batch of distinct inputs and a peak would be the batch's, not one sort's.
       LOGOS_SCALE_MEM=0 skips the pass; the charts then simply draw no circles. */
    if (!getenv("LOGOS_SCALE_MEM") || atoi(getenv("LOGOS_SCALE_MEM")) != 0) {
        double t_mem = nowms();
        for (size_t si = 0; si < all.size(); si++) {
            Series &s = all[si];
            omp_set_num_threads(s.threads);
            for (size_t ri = 0; ri < s.rows.size(); ri++) {
                Row &r = s.rows[ri];
                if (r.n < (size_t)SMALL_N) continue;
                const int P2 = s.threads;
                if (s.str) {                                 /* bytes per STRING: the pointer array is filled before arming, the pool too */
                    StrSet S = gen_strings(s.str, r.n); std::vector<const char *> sw(r.n);
                    if (r.t[0] > 0) r.m[0] = s.threads == 1
                        ? mem_of(sw, S.p, [](const char **q, size_t m) { std::sort(q, q + m, str_less); }, false)
                        : mem_of(sw, S.p, [](const char **q, size_t m) { __gnu_parallel::sort(q, q + m, str_less); }, false);
                    if (r.t[1] > 0) r.m[1] = mem_of(sw, S.p, [P2](const char **q, size_t m) { logos_sort_strings_mt(q, m, P2); }, true);
#ifdef HAVE_XSS
                    if (r.t[2] > 0) r.m[2] = mem_of(sw, S.p, [P2](const char **q, size_t m) { prefix_xss_sort(q, m, P2 > 1); }, false);
#endif
#ifdef HAVE_IPS4O
                    if (r.t[3] > 0) r.m[3] = mem_of(sw, S.p, [P2](const char **q, size_t m) { if (P2 == 1) ips4o::sort(q, q + m, str_less); else ips4o::parallel::sort(q, q + m, str_less, P2); }, false);
#endif
                    if (r.t[4] > 0) r.m[4] = mem_of(sw, S.p, [](const char **q, size_t m) { if (m > 1) bs_mkqs(q, 0, (long)m - 1, 0); }, false);
#ifdef HAVE_VQSORT
                    if (r.t[7] > 0) r.m[7] = mem_of(sw, S.p, [](const char **q, size_t m) { prefix_vq_sort(q, m); }, false);
#endif
                    continue;
                }
                std::vector<double> base = gen(s.shape, r.n), work(r.n);
                if (r.t[0] > 0) r.m[0] = s.threads == 1
                    ? mem_of(work, base, [](double *q, size_t m) { std::sort(q, q + m); }, false)
                    : mem_of(work, base, [](double *q, size_t m) { __gnu_parallel::sort(q, q + m); }, false);
                if (r.t[1] > 0) r.m[1] = mem_of(work, base, [P2](double *q, size_t m) { logos_sort_f64(q, m, P2); }, true);
#ifdef HAVE_XSS
                if (r.t[2] > 0) r.m[2] = mem_of(work, base, [](double *q, size_t m) { x86simdsortStatic::qsort(q, m, false, false); }, false);
#endif
#ifdef HAVE_IPS4O
                if (r.t[3] > 0) r.m[3] = mem_of(work, base, [P2](double *q, size_t m) { if (P2 == 1) ips4o::sort(q, q + m); else ips4o::parallel::sort(q, q + m, std::less<double>(), P2); }, false);
#endif
                if (r.t[4] > 0) r.m[4] = mem_of(work, base, [](double *q, size_t m) { radix_sort_f64(q, m); }, false);
                if (r.t[5] > 0) r.m[5] = mem_of(work, base, [](double *q, size_t m) { timsort_f64(q, m); }, false);
                if (r.t[6] > 0) r.m[6] = mem_of(work, base, [P2](double *q, size_t m) { logos_sort_f64_inplace(q, m, P2); }, true);
#ifdef HAVE_VQSORT
                if (r.t[7] > 0) r.m[7] = mem_of(work, base, [](double *q, size_t m) { hwy::VQSort(q, m, hwy::SortAscending()); }, false);
#endif
            }
        }
        omp_set_num_threads(P);
        printf("memory pass: peak heap of one cold call per point, bytes/element, %.1f s\n", (nowms() - t_mem) / 1e3);
    }

    /* ---- JSON ---- */
    auto arr = [](const double *v) { std::string o = "["; for (int k = 0; k < NC; k++) { if (k) o += ","; o += jnum(v[k]); } return o + "]"; };
    j = "[";
    for (size_t si = 0; si < all.size(); si++) {
        const Series &s = all[si]; j += (si ? ",{" : "{"); j += "\"shape\":\"" + s.shape + "\",\"threads\":" + std::to_string(s.threads);
        j += ",\"seamN\":" + std::to_string(s.seam_n);
        if (!s.names.empty()) { j += ",\"names\":["; for (size_t k = 0; k < s.names.size(); k++) j += std::string(k ? "," : "") + "\"" + s.names[k] + "\""; j += "]"; }
        if (!s.note.empty()) j += ",\"note\":\"" + s.note + "\"";
        j += ",\"seam\":" + arr(s.seam);
        j += ",\"rows\":[";
        for (size_t ri = 0; ri < s.rows.size(); ri++) { const Row &r = s.rows[ri]; j += (ri ? ",{" : "{"); j += "\"n\":" + std::to_string(r.n) + ",\"t\":" + arr(r.t);
            j += ",\"m\":" + arr(r.m);
            j += std::string(",\"ok\":") + (r.ok ? "true" : "false") + "}"; }
        j += "]}";
    }
    j += "]";
    const char *vq_isa =
#ifdef HAVE_VQSORT
        hwy::TargetName(hwy::SupportedTargets() & -hwy::SupportedTargets());   /* the instruction set its dispatch picked on this machine */
#else
        "";
#endif
    char meta[600]; snprintf(meta, sizeof meta, "{\"threads\":%d,\"memcpyMs\":%.3f,\"std1mMs\":%.2f,\"maxN\":%.0f,\"xss\":%s,\"xssIsa\":\"%s\",\"vqsort\":%s,\"vqIsa\":\"%s\",\"ips4o\":%s,\"ncpus\":%d,\"smallBelow\":%d,\"measuredAt\":%.0f}", P, memcpy_ms, std1m_ms, max_n,
#ifdef HAVE_XSS
             "true",
#else
             "false",
#endif
             xss_isa,
#ifdef HAVE_VQSORT
             "true",
#else
             "false",
#endif
             vq_isa,
#ifdef HAVE_IPS4O
             "true",
#else
             "false",
#endif
             logos_num_cpus(), (int)SMALL_N, (double)time(NULL) * 1000.0);
    meta_s = meta;
    /* WHAT it was measured on, for a reader outside the project: the CPU's model name, the compiler, and the version of each
       opponent that carries one in its headers. Read at measuring time, so a LOGOS_SCALE_REUSE re-render keeps the original's. */
    {
        std::string cpu;
        if (FILE *ci = fopen("/proc/cpuinfo", "r")) { char ln[512];
            while (fgets(ln, sizeof ln, ci)) if (!strncmp(ln, "model name", 10)) { const char *c = strchr(ln, ':'); if (c) { cpu = c + 1; while (!cpu.empty() && (cpu[0] == ' ' || cpu[0] == '\t')) cpu.erase(0, 1); while (!cpu.empty() && (cpu.back() == '\n' || cpu.back() == ' ')) cpu.pop_back(); } break; }
            fclose(ci); }
        std::string add = ",\"cpu\":" + jstr(cpu.c_str()) + ",\"compiler\":" + jstr("GCC " __VERSION__);
#ifdef HAVE_VQSORT
        char v[64]; snprintf(v, sizeof v, "%d.%d.%d", HWY_MAJOR, HWY_MINOR, HWY_PATCH); add += ",\"vqVersion\":" + jstr(v);
#endif
        meta_s.insert(meta_s.size() - 1, add);
    }
    }

    static const char *TMPL = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>logos-sort Benchmarks</title><meta name="claude:viewport-fit-sides" content="cover">
<style>
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s-logos:#2a78d6;--s-base:#898781;--s-xss:#eb6834;--s-vq:#1baf7a;--s-ips:#eda100;--s-radix:#e87ba4;--s-tim:#008300;--s-inpl:#6250d6;
--pos:#2a78d6;--neg:#e34948;--mid:#c3c2b7}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-vq:#199e70;--s-ips:#c98500;--s-radix:#d55181;--s-tim:#008300;--s-inpl:#9085e9;
--pos:#3987e5;--neg:#e66767;--mid:#383835}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-vq:#199e70;--s-ips:#c98500;--s-radix:#d55181;--s-tim:#008300;--s-inpl:#9085e9;
--pos:#3987e5;--neg:#e66767;--mid:#383835}
*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding-block:0 3rem;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
body::before{content:"";position:fixed;inset:0 0 auto;height:env(safe-area-inset-top,0px);z-index:41;background:var(--page)}
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
.bar{position:sticky;top:env(safe-area-inset-top,0px);z-index:40;background:var(--page);padding:6px 0 2px;margin:0 0 8px;border-bottom:1px solid var(--grid)}.bar .row{margin:2px 0 4px}
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
.shapes{margin:12px 0 16px;scroll-margin-top:64px}.shapes>summary{font-size:15px;font-weight:600;color:var(--ink);padding:4px 0}
.shapes>.sub{margin:8px 0 10px}
.shp{border:1px solid var(--grid);border-radius:8px;background:var(--surface);margin:6px 0;padding:0 12px;scroll-margin-top:64px}
.shp>summary{padding:8px 0;color:var(--ink);font-size:13px}.shp>summary b{font-weight:600;margin-right:10px}.shp>summary span{color:var(--ink2)}
.shp[open]>summary{border-bottom:1px solid var(--grid)}
.shp .body{padding:4px 0 12px;font-size:13px;color:var(--ink2)}.shp .body p{margin:8px 0}
.shp .prof{font-variant-numeric:tabular-nums;color:var(--muted);font-size:12px}.shp .cap{margin:10px 0 2px;font-size:12px;color:var(--muted)}
.plots{display:grid;grid-template-columns:1fr 1fr;gap:12px 20px}@media(max-width:640px){.plots{grid-template-columns:1fr}}
.plots figure{margin:0;min-width:0}.plots figcaption{font-size:12px;color:var(--muted);margin:0 0 2px}
.plots svg{display:block;width:100%;height:auto;max-width:440px}
.plots .ax{stroke:var(--axis);stroke-width:1;fill:none}.plots .lbl{fill:var(--muted);font-size:10px;font-family:system-ui,-apple-system,"Segoe UI",sans-serif}
.plots .d{fill:var(--s-logos)}.plots .ln{stroke:var(--axis);stroke-width:1;fill:none}.plots .mk{fill:var(--s-xss)}
.vals{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:1px 16px;font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--ink);font-variant-numeric:tabular-nums}
.vals span{color:var(--muted)}.vals .h,.lines .h{color:var(--s-xss);font-weight:600}
.lines{margin:0;padding:0;list-style:none;font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--ink);overflow-x:auto}
.lines li{white-space:pre}.lines li span{color:var(--muted);display:inline-block;min-width:8ch}
p.sub a{color:var(--s-logos)}
.whint{position:absolute;z-index:31;left:50%;top:6px;transform:translateX(-50%);pointer-events:none;opacity:0;transition:opacity .25s;background:var(--ink);color:var(--page);font-size:12px;padding:3px 10px;border-radius:999px;white-space:nowrap}
/* the front matter: what logos-sort is, the summary chart (computed from DATA), the contenders, the method */
.top{padding-block:30px 4px}.eyebrow{margin:0;color:var(--muted);font-size:12px;letter-spacing:.07em;text-transform:uppercase}
.top h1{font-size:30px;line-height:1.15;margin:6px 0 12px;letter-spacing:-.01em;text-wrap:balance}
.lede{font-size:16px;line-height:1.55;max-width:66ch;margin:0 0 14px}
.front{margin:34px 0 0}.front>h2{font-size:19px;margin:0 0 8px;text-wrap:balance}
.prose{max-width:72ch}.prose p{margin:0 0 10px}
.score{font-size:15px;margin:0 0 14px;max-width:72ch}
.gl{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:860px){.gl{grid-template-columns:1fr}}
.glc{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:12px 14px 10px;min-width:0}
.glc h3{margin:0 0 6px;color:var(--ink)}
.glr{display:grid;grid-template-columns:7.4rem minmax(0,1fr) 7.6rem;gap:0 10px;align-items:start;padding:4px 0}
@media(max-width:480px){.glr{grid-template-columns:5.8rem minmax(0,1fr) 6.6rem}}
.glr .nm{font-size:12px;line-height:16px;color:var(--ink2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.glr .trk{position:relative;height:16px}
.glr .mid{position:absolute;top:-4px;bottom:-17px;width:1px;background:var(--mid)}
.glr .b{position:absolute;top:3px;height:10px;border-radius:2px}
.b.pos{background:var(--pos)}.b.neg{background:var(--neg)}.b.eq{background:var(--muted)}
.glr .v{font-size:12px;line-height:16px;color:var(--ink2);font-variant-numeric:tabular-nums}
.glr .v b{color:var(--ink);font-weight:600}.glr .v small{display:block;color:var(--muted);font-size:11px;line-height:13px}
.glr.ax{padding-top:2px}.glr.ax .trk{height:30px}
.tk{position:absolute;top:2px;font-style:normal;font-size:11px;color:var(--muted);transform:translateX(-50%);font-variant-numeric:tabular-nums;white-space:nowrap}
.tk.l{transform:none}.tk.r{transform:translateX(-100%)}.tk.cap{top:16px}
.who{display:grid;grid-template-columns:minmax(10rem,max-content) minmax(0,1fr);gap:9px 20px;max-width:84ch;margin:0}
@media(max-width:600px){.who{grid-template-columns:minmax(0,1fr);gap:2px}.who dd{margin-bottom:10px}}
.who dt{font-weight:600;color:var(--ink)}.who dd{margin:0;color:var(--ink2)}.who a{color:var(--s-logos)}.who .sw{height:10px;width:10px;border-radius:50%}
.method{margin:0;padding-left:20px;max-width:76ch;color:var(--ink2)}.method li{margin:0 0 7px}.method b{color:var(--ink)}
pre.cmd{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:10px 14px;overflow-x:auto;font:12.5px/1.6 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--ink);margin:6px 0 10px;max-width:100%}
.glance-t td:nth-child(5),.glance-t th:nth-child(5){text-align:left}
</style></head><body><div class="wrap">
<header class="top">
<p class="eyebrow">Benchmark report</p>
<h1>logos-sort benchmarks</h1>
<p class="lede">logos-sort is a sorting engine that reads the shape of its input before it sorts. When the data has structure, such as stretches already in order, a handful of distinct values, numbers on a fixed grid, or sorted blocks in the wrong order, it places elements by address instead of comparing them. This report times it against the fastest open-source sorts available, on the same inputs, from an empty array to <span id="nmaxw">ten million</span> elements, on one thread and on many.</p>
<p class="sub" id="sub"></p><p class="sub" id="age"></p>
</header>

<section class="front" id="glance-sec" aria-labelledby="h-glance">
<h2 id="h-glance">At the largest size</h2>
<p class="score" id="score"></p>
<div class="gl" id="glance"></div>
<p class="hint">Each bar compares logos with the fastest other sort on that input, at the largest size measured. To the right of the line logos is faster by the factor shown; to the left, the other sort is. Hover a row for both times. Differences under 5% are shown as level: that is within this machine's run-to-run noise.</p>
<details><summary>the same comparison as a table</summary><div class="tw" id="glance-table"></div></details>
</section>

<section class="front prose" aria-labelledby="h-why">
<h2 id="h-why">Where the speed comes from</h2>
<p>Most of the inputs below have structure that real data often has: logs that arrive nearly in time order, columns with a few distinct values, prices on a grid of cents, shards concatenated out of order. logos detects that from a small sample and takes a route that does less work than any comparison sort can: it counts, places whole runs, or merges only where runs overlap. On those inputs the margin is generally larger at ten million elements than at one million.</p>
<p>Uniformly random numbers have no structure to use. On one thread logos then runs its own vectorized quicksort, the same design as x86-simd-sort and VQSort, so those rows are a race between kernels; on many threads it splits the values into ranges across all cores and sorts each range. The uniform, heavy_tail and bimodal inputs are that case.</p>
<p>The trade is memory. logos often allocates scratch space, where std::sort and the vector quicksorts sort in place; the circles on the detailed charts show the peak heap per element for every sort.</p>
</section>

<section class="front" aria-labelledby="h-who">
<h2 id="h-who">The contenders</h2>
<dl class="who" id="who"></dl>
</section>

<section class="front" aria-labelledby="h-how">
<h2 id="h-how">How it was measured</h2>
<ul class="method" id="method"></ul>
<div id="cal"></div>
</section>

<section class="front" id="detail" aria-labelledby="h-detail">
<h2 id="h-detail">Every size, every sort</h2>
<p class="sub">One section per input. In each, the left panel compares sorts that each use one thread and the right panel sorts that each use the same number of threads, never one against many. Each panel has two charts of the same measurements: the time per sort, and below it the time per element, which shows how the cost grows with n. The tables under each panel hold every number.</p>
</section>
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
<details class="how"><summary>reading the charts in detail: the reference curves, exponents, the seam at 10k and the memory circles</summary>
<p class="sub">The dashed grey curves are O(n) and O(n log n) references anchored at logos's largest measured point, so a series that bends above the O(n log n) curve is growing faster than a comparison sort should. The label after each series name is its empirical exponent, the least-squares slope of log t against log n over n &ge; 100k (below that fixed costs dominate). The single-threaded panel compares sorts that each use one thread; the parallel panel compares sorts that each use the same P threads (VQSort, the LSD radix and timsort have no parallel form and appear in the first only). Every output was checked equal to std::sort's on the same data; the minimum of the repetitions is plotted. A line that stops early hit the per-sort time budget.</p>
<p class="sub">Each panel holds two charts of the same measurements: time per sort, and below it nanoseconds per element, whose value axis is always linear (only its n axis follows the axis toggle). n = 0 is the cost of one call: it has no per-element value, and it cannot sit on a log axis, so the log view leaves it to the tables. The size buttons cut every chart down to the small end, because a linear n axis puts everything below a million into its first pixels. The last row of the ns-per-element table is the scaling slope read off two points: with c the cost per element, p = 1 + ln(c_hi / c_lo) / ln(n_hi / n_lo), so p = 1 is linear scaling and the percentage beside it is how much dearer one element became from 1M to the largest size. Those two rows are measured minutes apart on a machine whose speed drifts, so read that row as a sketch: make slopebench measures the same number in adjacent pairs, where the drift cancels. Sizes below 10k are timed as batches of distinct inputs sorted back to back, since one such sort is shorter than the clock can resolve and repeating one input would train the branch predictor; the parallel sorts are called with all threads there too, so what they show at small n is what such a call costs. <b>That change of method is a SEAM, and the dashed line marks it.</b> Three things flip there at once, all favouring the right-hand side: the working set (a batch is about 2 MB and exceeds L2, one array at n = 10k is 78 KB and does not), the branch history (every batch instance is a different input; the single array is the same input every rep), and the statistic (a batch's total divided by its count, against the minimum of several reps). So a curve may be read along either side of the line, but a step ACROSS it is partly the method. The <b>batch &divide; single</b> row in the ns-per-element table measures that step for this panel: the same n timed both ways, at the one size where both modes resolve. Use it to tell the two apart. Where it is well above 1 a jump at the line is the harness; the sequential contenders sit around 2-4x. Where it is near 1 and the chart still jumps, the jump is real and belongs to the data - a shape is not the same shape at 3162 as at 10000, since the number of runs, cycles or distinct values in it moves with n, and the engine's routing gates are ratios of n. Both happen in this report. The extreme values, tens to hundreds, are all __gnu_parallel::sort at P threads, and they are not a measurement fault but the point restated: a batch calls the sort 26 times back to back, so 26 parallel regions are opened and joined on 10k elements, where the single measurement opens one and keeps the best of five. Everything that panel plots LEFT of the line for a parallel contender is dominated by that per-call cost rather than by sorting, which is what "the parallel sorts are called with all threads there too" means in practice. <b>The circle around a point is memory</b>: the peak heap of one COLD call at that size, in bytes per element, measured in a pass of its own after all the timing (releasing the pools is what makes a call cold, and doing that between timed rows would have left the next row paying for its buffers). The value is the circle's RADIUS: the largest reading anywhere in the report is 15 px and every other scales linearly by it, so a circle means the same number in every panel; with a linear radius, twice the memory draws four times the ink, so read them as ranks rather than areas. The numbers themselves are in the tooltip (the peak heap column, bytes per element and the total it means at that n) and in each panel's third table. No circle means nothing was allocated - std::sort and the strict in-place entry draw none at any size - and sizes below 10k have none because a size there is a BATCH of distinct inputs, where a peak would be the batch's and not one sort's. Per element, not absolute: absolute bytes would simply grow with n and say nothing, and this way the small-n rows show the fixed pool costs before they amortize.</p>
</details>
<details class="shapes" id="shapes"><summary>The <span id="shapecount">17</span> shapes: what each one is, with demo data from its own generator</summary>
<p class="sub">Every shape below is drawn by the bench's own generator, seed 7, at n = 1,048,576: the data the charts sorted at that size, not a picture of it. Left, the whole array at 512 evenly spaced positions; right, 48 consecutive elements around the feature that defines the shape (in orange where there is one), and below them twelve of those values in the order the sort receives them. The line under each description counts all n elements. Strings have no numeric value, so they are plotted by rank, their position in sorted order. Shapes are defined partly in proportion (64 blocks, 32 cycles, n/100 swaps) and partly in absolute terms (noise of &plusmn;50 or &plusmn;60), so at small n the second kind is relatively noisier, one reason a curve can bend between sizes. The batches below 10k use seeds 7, 8, 9, &hellip; for their instances. Each panel's heading links back to its entry here.</p>
<div id="shapelist"></div>
</details>
<div id="charts"></div>
<section class="front prose" aria-labelledby="h-repro">
<h2 id="h-repro">Reproducing it</h2>
<p>Everything here is one command on Linux with GCC and an x86-64 CPU with AVX2. From a checkout of logos-sort (github.com/markciubal/logos-sort), fetch the opponents beside it, then build and run:</p>
<pre class="cmd">cd engines
git clone --depth 1 https://github.com/intel/x86-simd-sort
git clone --depth 1 https://github.com/ips4o/ips4o
git clone --depth 1 --branch 1.4.0 https://github.com/google/highway
sudo apt install libtbb-dev          # the IPS4o build links TBB
cd c && make scale-full              # about ten minutes; writes scale-report.html</pre>
<p>The run prints every time as it goes and stops with an error if any sort's output differs from std::sort's. Absolute times depend on the machine; the comparisons within each chart are the result.</p>
</section>
</div>
<script>__UPLOT_JS__</script>
<script>
const DATA=__DATA__,META=__META__;
const SHAPES=__SHAPES__;
const NAMES1=['std::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','LSD radix','timsort','logos in-place','VQSort (Google) '+(META.vqIsa||'')];
const NAMESP=['__gnu_parallel::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','','','logos in-place',''];
const CVAR=['--s-base','--s-logos','--s-xss','--s-ips','--s-radix','--s-tim','--s-inpl','--s-vq'];
/* legend, table and tooltip order = the palette's slot order, so colours that sit side by side are the validated adjacent pairs */
const ORDER=[1,2,7,3,4,5,6,0];
const cssv=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim()||'#888888';
const fmtN=n=>n==null?'':n>=1e9?(+(n/1e9).toFixed(2))+'B':n>=1e6?(+(n/1e6).toFixed(2))+'M':n>=1e3?(+(n/1e3).toFixed(1))+'k':String(+n.toFixed(2));
/* one formatter per quantity, each walking down the units: five elements sort in nanoseconds, ten million in seconds */
const fmtMs=v=>v==null?'':v===0?'0':v>=1000?(+(v/1000).toFixed(1))+' s':v>=1?(+v.toFixed(v>=10?0:1))+' ms':v>=1e-3?(+(v*1e3).toFixed(v>=1e-2?0:1))+' µs':(+(v*1e6).toFixed(v>=1e-5?0:v>=1e-6?1:2))+' ns';
const fmtNs=v=>v==null?'':v===0?'0':v>=1e6?(+(v/1e6).toFixed(1))+' ms':v>=1e3?(+(v/1e3).toFixed(1))+' µs':(+v.toFixed(v>=100?0:v>=10?1:2))+' ns';
let MODE='lin';
let RANGE='all';
let SHOWMEM=true;
const RANGES={all:Infinity,k10:1e4,t10:10};
/* MEMORY. r.m[k] is the peak heap of one COLD call in bytes per element, from scalebench's memory pass (its
   header says why it runs after all timing and why the unit is per element). The maximum is taken across EVERY
   shape, size and sort in the report, not per panel, so the same circle means the same number everywhere.
   MEMPX is the diameter of that largest reading; the rest scale linearly by it, as asked. Note the perceptual
   caveat: with a linear diameter, twice the memory draws four times the ink. */
const MEMPX=30;
const MEMMAX=(function(){let m=0;DATA.forEach(function(s){s.rows.forEach(function(r){if(r.m)r.m.forEach(function(v){if(v>0&&v>m)m=v;});});});return m;})();
const NS=DATA.length?DATA[0].rows.map(r=>r.n):[];
/* HOW OLD these measurements are, from the moment stored with them (META.measuredAt, ms since the epoch), against the
   reader's clock, refreshed while the page stays open. Past a week it says so: a scaling curve describes the engine
   that was measured, not the one in the tree. */
function agoText(ms){const s=Math.max(0,Math.round(ms/1000));const u=[[31536000,'year'],[2592000,'month'],[86400,'day'],[3600,'hour'],[60,'minute']];
 for(const [k,nm] of u){if(s>=k){const v=Math.floor(s/k);return v+' '+nm+(v==1?'':'s')+' ago';}}return s<10?'just now':s+' seconds ago';}
function paintAge(){const el=document.getElementById('age');if(!el)return;const at=META&&META.measuredAt;
 if(!(at>0)){el.textContent='measured: time not recorded';return;}
 const d=new Date(at),age=Date.now()-at,p=n=>String(n).padStart(2,'0');
 const abs=d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes());
 el.innerHTML='<b>measured '+agoText(age)+'</b> ('+abs+', your local time)'+(age>7*86400000?' <b>&mdash; older than a week: re-run make scale-full before trusting it</b>':'');}
paintAge();
/* refreshed every 30 s. Under node (tools/check_report.js) a live interval keeps the process from ever exiting -- the first cut
   hung the checker for ten minutes -- so the timer is unref'd where that exists; a browser's timer id has no such method */
if(typeof setInterval==='function'){const h=setInterval(paintAge,30000);if(h&&typeof h.unref==='function')h.unref();}
document.getElementById('sub').textContent=[META.cpu||'',`${META.threads} threads for the parallel runs (${META.ncpus} logical CPUs)`,META.compiler||'',`${NS.length} sizes from ${NS.length?fmtN(NS[0]):'?'} to ${NS.length?fmtN(NS[NS.length-1]):'?'}`,`${DATA.length/2} inputs`].filter(Boolean).join(' · ');
document.getElementById('cal').innerHTML=`<div class="warn">How busy the machine was, measured before the run on one thread: copying 64 MB took ${META.memcpyMs.toFixed(2)} ms and std::sort of a million doubles ${META.std1mMs.toFixed(1)} ms. At rest this machine reads about 3 ms and 60 ms. Well above that, every absolute time in the report is inflated, while the comparisons between sorts in one chart still hold.</div>`;
/* the contenders, in the charts' colours; the versions and instruction sets come from META where the run recorded them */
document.getElementById('who').innerHTML=[
 ['--s-logos','logos-sort','The engine being measured: C, with AVX2 kernels and its own parallel partition. Also shown, in violet: its in-place entry, which allocates no scratch memory at all.',''],
 ['--s-xss','x86-simd-sort','Intel\u2019s vectorized quicksort library, here built for '+(META.xssIsa||'this CPU')+'; at P threads its OpenMP version.','https://github.com/intel/x86-simd-sort'],
 ['--s-vq','VQSort','Google\u2019s vectorized quicksort from the Highway library'+(META.vqVersion?' (version '+META.vqVersion+')':'')+'. It picks its instruction set when it runs'+(META.vqIsa?' ('+META.vqIsa+' on this machine)':'')+' and has no parallel form, so it appears at one thread only.','https://github.com/google/highway/tree/master/hwy/contrib/sort'],
 ['--s-ips','IPS\u2074o','In-place Parallel Super Scalar Samplesort from KIT, a state-of-the-art comparison sort, sequential at one thread and parallel at P.','https://github.com/ips4o/ips4o'],
 ['--s-base','std::sort','GCC\u2019s standard library sort (introsort); at P threads __gnu_parallel::sort, its OpenMP mode. Every other output is checked against it.',''],
 ['--s-radix','LSD radix, timsort','Two reference implementations written for this benchmark (timsort in green), one thread only.',''],
].filter(w=>w[1]!=='VQSort'||META.vqsort).map(w=>`<dt><i class="sw" style="background:var(${w[0]})"></i>${w[1]}</dt><dd>${w[2]}${w[3]?` <a href="${w[3]}" rel="noopener" target="_blank">Source</a>`:''}</dd>`).join('')+
 `<dt>On strings</dt><dd>The vector sorts sort numbers, not text. On the four string inputs they sort the same 8-byte prefix keys logos uses, so those lines compare the kernels under one method. VQSort sorts 16-byte records, so its line also pays to pack and unpack them.</dd>`;
document.getElementById('method').innerHTML=[
 '<b>Every output is checked</b> against std::sort\u2019s on the same input before its time counts. A mismatch would mark the row with \u2717; there are none unless one is shown.',
 '<b>Same input, best of several runs.</b> Each size is sorted by every contender in turn, from an identical copy. The fastest of 5 runs is kept up to a million elements, of 3 up to ten million.',
 '<b>One thread against one thread, '+META.threads+' against '+META.threads+'.</b> '+META.threads+' is the physical core count; every parallel sort is given that many threads.',
 '<b>Small sizes are timed in batches.</b> Below '+fmtN(META.smallBelow||10000)+' elements one sort is shorter than the clock can resolve, so each size is a batch of different inputs sorted back to back. The detailed notes under the charts explain the step this makes in some curves.',
 '<b>The machine is a laptop, not a lab.</b> It ran under WSL2 with ordinary background programs, so absolute times move by tens of percent from run to run. The sorts at one size run one after another, seconds apart, so the drift between them is small but not zero; that is why differences under 5% count as level here.',
].map(t=>'<li>'+t+'</li>').join('');
document.getElementById('legend').innerHTML=ORDER.filter(k=>k<2||(k==2&&META.xss)||(k==3&&META.ips4o)||(k==7&&META.vqsort)||(k>=4&&k<=6)).map(k=>`<span><i class="sw" style="background:${cssv(CVAR[k])}"></i>${k==0?'std::sort (1T) / __gnu_parallel::sort (P)':NAMES1[k]+((k==4||k==5||k==7)?' (1 thread only)':'')}</span>`).join('')+`<span><i class="sw ref"></i>O(n) and O(n log n), anchored at logos's largest n</span>`+(MEMMAX>0?`<span><i class="sw" style="background:transparent;border:1.5px solid var(--muted);border-radius:50%"></i>circle radius = peak heap of one cold call in bytes per element (the report's largest, ${+MEMMAX.toFixed(1)} B/elem, is ${MEMPX/2} px); the values are in the tooltip and in each panel's tables</span>`:'');

/* empirical exponent: least-squares slope of log10 t vs log10 n over n >= 1e5 (fixed costs dominate below) */
function exponent(rows,k){const pts=rows.filter(r=>r.t[k]>0&&r.n>=1e5).map(r=>[Math.log10(r.n),Math.log10(r.t[k])]);if(pts.length<3)return null;
 const mx=pts.reduce((a,p)=>a+p[0],0)/pts.length,my=pts.reduce((a,p)=>a+p[1],0)/pts.length;let sxx=0,sxy=0;pts.forEach(p=>{sxx+=(p[0]-mx)**2;sxy+=(p[0]-mx)*(p[1]-my);});return sxx>0?sxy/sxx:null;}

/* memory as text: bytes per element with the total it means at that n (the circle is the same number as a radius) */
function fmtBytes(b){return b>=1073741824?(b/1073741824).toFixed(2)+' GB':b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?(b/1024).toFixed(0)+' KB':b.toFixed(0)+' B';}
function fmtMem(v,n){return (+v.toFixed(v>=10?1:2))+' B/elem ('+fmtBytes(v*n)+')';}
function table(s){const names=s.names||(s.threads==1?NAMES1:NAMESP);const cols=ORDER.filter(k=>s.rows.some(r=>r.t[k]>0));
 const exps=cols.map(k=>{const e=exponent(s.rows,k);return e===null?'–':'n^'+e.toFixed(2);});
 const head=last=>`<thead><tr><th>n</th>${cols.map(k=>`<th>${names[k]}</th>`).join('')}${last?`<th>${last}</th>`:''}</tr></thead>`;
 const tRows=s.rows.map(r=>{const others=cols.filter(k=>k!=1&&k!=6&&r.t[k]>0).map(k=>r.t[k]);const best=others.length?Math.min(...others):0;
  return `<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.t[k]>0?fmtMs(r.t[k]):'–'}</td>`).join('')}<td>${best&&r.t[1]>0?(best/r.t[1]).toFixed(2)+'×':'–'}${r.ok?'':' ✗'}</td></tr>`;}).join('');
 /* the scaling slope read off two points, through ns per element: with c = t/n, t ~ n^p gives p = 1 + ln(c_hi/c_lo) / ln(n_hi/n_lo).
    p = 1 is linear; what a reader feels is the first number, how much dearer one element got. From 1M up, below that fixed costs bend it. */
 const slope=k=>{const rs=s.rows.filter(r=>r.t[k]>0&&r.n>=1e6);if(rs.length<2)return '-';const a=rs[0],b=rs[rs.length-1],g=(b.t[k]/b.n)/(a.t[k]/a.n);
  return (g>=1?'+':'')+((g-1)*100).toFixed(1)+'% (p = '+(1+Math.log(g)/Math.log(b.n/a.n)).toFixed(3)+')';};
 const pRows=s.rows.map(r=>`<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.t[k]>0&&r.n>0?fmtNs(r.t[k]*1e6/r.n):'–'}</td>`).join('')}</tr>`).join('');
 /* MEMORY, as numbers: the peak heap of one cold call per point, the value each circle's radius stands for. Rows below
    the seam have none (a size there is a batch); a dash on a measured row is a sort that allocated nothing. */
 const mRows=s.rows.filter(r=>r.m&&r.m.some(v=>v>0)).map(r=>`<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.m[k]>0?fmtMem(r.m[k],r.n):(r.t[k]>0?'0':'–')}</td>`).join('')}</tr>`).join('');
 const mTable=mRows?`<h3 class="cap2">peak heap of one cold call: bytes per element (total at that n). The circle drawn at each point has this value as its radius, ${MEMPX/2} px for the report's largest (${+MEMMAX.toFixed(1)} B/elem); 0 = the sort allocated nothing; sizes below ${fmtN(META.smallBelow||10000)} are batches and have no per-sort peak</h3><div class="tw"><table>${head('')}<tbody>${mRows}</tbody></table></div>`:'';
 /* THE SEAM, measured: n = smallBelow timed the SMALL way (a batch of distinct instances) over the same n timed the
    BIG way (one array, min of reps). Everything left of that n is on the batch side, so DIVIDE those points by this
    to bring them onto the right-hand scale -- or multiply the right-hand points by it to go the other way. Without
    it the curve steps by 2.3-2.6x at that x for reasons that have nothing to do with any sort. */
 const seamRow=(()=>{const sm=s.seam;if(!sm||!s.seamN)return '';const at=s.rows.find(r=>r.n==s.seamN);if(!at)return '';
  if(!cols.some(k=>sm[k]>0&&at.t[k]>0))return '';
  return `<tr><td>batch ÷ single at n = ${fmtN(s.seamN)}</td>${cols.map(k=>`<td>${sm[k]>0&&at.t[k]>0?(sm[k]/at.t[k]).toFixed(2)+'×':'–'}</td>`).join('')}</tr>`;})();
 return `<details><summary>tables: time per sort, ns per element, empirical exponents, peak heap per element</summary><h3>time per sort</h3><div class="tw"><table>${head('logos vs best other')}<tbody>${tRows}<tr><td>t ∝ (n ≥ 100k)</td>${exps.map(e=>`<td>${e}</td>`).join('')}<td></td></tr></tbody></table></div><h3 class="cap2">nanoseconds per element (n = 0 has no elements: its row above is the cost of one call)</h3><div class="tw"><table>${head('')}<tbody>${pRows}${seamRow}<tr><td>per element, 1M to the largest n</td>${cols.map(k=>`<td>${slope(k)}</td>`).join('')}</tr></tbody></table></div>${mTable}</details>`;}

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
 const names=s.names||(s.threads==1?NAMES1:NAMESP),lg=MODE==='log',per=kind==='nsel',ylog=lg&&!per;
 const rows=visible(s);if(!rows.length)return null;
 const xs=rows.map(r=>r.n),fv=per?fmtNs:fmtMs;
 const yv=(r,k)=>{const t=r.t[k];if(!(t>0))return null;if(!per)return t;return r.n>0?t*1e6/r.n:null;};
 const data=[xs],series=[{label:'n',value:(u,v)=>v==null?'–':fmtN(v)}],meta=[null],mems=[null];
 ORDER.slice().reverse().forEach(k=>{
  if(!rows.some(r=>yv(r,k)!==null))return;
  data.push(rows.map(r=>yv(r,k)));
  const nm=names[k]||('series '+k),ex=exponent(s.rows,k),bad=s.rows.some(r=>r.t[k]>0&&!r.ok);
  meta.push({name:nm,ref:false,logos:k===1,color:cssv(CVAR[k])});
  series.push({label:nm+(ex!==null?' ~n^'+ex.toFixed(2):'')+(bad?' ✗':''),
   stroke:cssv(CVAR[k]),width:k===1?2.5:1.75,points:{show:true,size:6},
   value:(u,v)=>v==null?'–':fv(v)});
  mems.push(rows.map(r=>(r.m&&r.m[k]>0)?r.m[k]:null));
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
   series.push({label:nm,stroke:cssv('--muted'),width:1.25,dash:[5,4],points:{show:false},value:(u,v)=>v==null?'–':fv(v)});
   mems.push(null);});}
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
         setSeries:[(u,si,o)=>{if(o&&o.show!=null)IX.refit(u);}],
         /* MEMORY CIRCLES. Drawn after the series, clipped to the plotting rect, closing over `mems` so the
            hook needs nothing from the uPlot instance but ctx, bbox and valToPos. Every access is guarded:
            the stub in tools/check_report.js implements only the parts of uPlot the report is known to use. */
         draw:[u=>{
          /* THE SEAM. Left of META.smallBelow a point is a BATCH of distinct instances (~2 MB swept, every one
             cache-cold, none repeated); at and above it a point is ONE array sorted again and again with the
             MINIMUM kept (78 KB, L2-resident, same input every rep). Measured both ways, the step is 2.3-2.6x,
             so the curve may be read on either side of this line but NOT across it. Drawn before the circles and
             independently of them. Counted, because a stub DOM proves only that the code ran. */
          (()=>{
           const c=u&&u.ctx,sb=META&&META.smallBelow;
           if(!c||!(sb>0)||!u.valToPos||!u.bbox||!c.moveTo)return;
           const xs=data&&data[0];if(!xs||!xs.length)return;
           if(!(xs[0]<sb&&xs[xs.length-1]>=sb))return;              /* the seam is off-screen: nothing to mark */
           const px=u.valToPos(sb,'x',true);if(!isFinite(px))return;
           const dpr=u.pxRatio||(window.devicePixelRatio||1);
           c.save();
           if(c.beginPath&&c.rect&&c.clip){c.beginPath();c.rect(u.bbox.left,u.bbox.top,u.bbox.width,u.bbox.height);c.clip();}
           c.strokeStyle=cssv('--muted');c.globalAlpha=0.85;c.lineWidth=1*dpr;
           if(c.setLineDash)c.setLineDash([4*dpr,4*dpr]);
           c.beginPath();c.moveTo(px,u.bbox.top);c.lineTo(px,u.bbox.top+u.bbox.height);c.stroke();
           if(c.setLineDash)c.setLineDash([]);
           if(c.fillText){c.globalAlpha=1;c.fillStyle=cssv('--muted');
            c.font=(10*dpr)+'px system-ui,sans-serif';
            if(c.textAlign!==undefined)c.textAlign='left';
            c.fillText('batch | single',px+4*dpr,u.bbox.top+12*dpr);}
           c.restore();
           window.__SEAMRULES=(window.__SEAMRULES||0)+1;
          })();
          if(!SHOWMEM||!(MEMMAX>0))return;
          const c=u&&u.ctx;if(!c||!c.arc||!u.valToPos||!u.bbox)return;
          const dpr=u.pxRatio||(window.devicePixelRatio||1);
          c.save();
          if(c.beginPath&&c.rect&&c.clip){c.beginPath();c.rect(u.bbox.left,u.bbox.top,u.bbox.width,u.bbox.height);c.clip();}
          for(let i=1;i<mems.length&&i<data.length;i++){
           const col=mems[i];if(!col)continue;
           const se=u.series&&u.series[i];if(se&&se.show===false)continue;
           c.strokeStyle=(se&&se.stroke)||cssv('--muted');c.globalAlpha=0.5;c.lineWidth=1.5*dpr;
           for(let jx=0;jx<col.length;jx++){
            const v=col[jx];if(!(v>0))continue;
            const y=data[i][jx];if(y==null)continue;
            const xv=data[0][jx];
            const px=u.valToPos(xv,'x',true),py=u.valToPos(y,'y',true);
            if(!isFinite(px)||!isFinite(py))continue;
            const rr=(MEMPX*(v/MEMMAX)/2)*dpr;if(!(rr>=0.5))continue;
            c.beginPath();c.arc(px,py,rr,0,6.283185307179586);c.stroke();window.__MEMCIRCLES=(window.__MEMCIRCLES||0)+1;
           }}
          c.restore();
         }]},
 };
 const u=new uPlot(opts,data,holder);
 u._ix={per,ylog,fv,meta,mems,capEl,capBase,capHome:capEl?capEl.textContent:''};
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
  const nn=u.data[0][i];
  for(let si=1;si<u.data.length;si++){const m=ix.meta[si],v=u.data[si][i];if(m.ref||!u.series[si].show||v==null)continue;const mv=ix.mems&&ix.mems[si]?ix.mems[si][i]:null;rows.push([v,m,mv]);if(m.logos)L=v;}
  if(!rows.length){t.style.display='none';return;}
  rows.sort((a,b)=>a[0]-b[0]);
  const anyM=rows.some(r=>r[2]>0);
  t.innerHTML='<b>n = '+Number(nn).toLocaleString('en-US')+'</b><table><tr><th></th><th>'+(ix.per?'per element':'per sort')+'</th><th>'+(L?'× logos':'')+'</th>'+(anyM?'<th>peak heap</th>':'')+'</tr>'+
   rows.map(([v,m,mv])=>'<tr><td><i class="sw" style="background:'+m.color+'"></i>'+esc(m.name)+'</td><td>'+ix.fv(v)+'</td><td>'+(L?(v/L).toFixed(2):'')+'</td>'+(anyM?'<td>'+(mv>0?fmtMem(mv,nn):'–')+'</td>':'')+'</tr>').join('')+'</table>';
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

/* THE SHAPES. SHAPES is written by scalebench when it writes the page (shape_demos_json), from the bench's own generators,
   so each demo is the data that was sorted. Only the shapes this report measured are listed, in its order; a panel's
   heading links to its entry. Plots are inline SVG coloured by the page's tokens, so a theme change needs no redraw. */
const escH=t=>String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const slug=s=>'about-'+String(s).toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-|-$/g,'');
const SHAPEDOC={};SHAPES.forEach(d=>{SHAPEDOC[d.shape]=d;});
/* a pair of labels (an axis's ends, a range's ends) takes as many significant digits as it needs to differ: 295,070 and
   295,081 both read 295,100 at four */
const cfmt=(v,p)=>{p=p||4;const a=Math.abs(v);return a>=1e6?(+v.toExponential(p-1)).toExponential().replace('e+','e'):(+v.toPrecision(p)).toLocaleString('en-US',{maximumFractionDigits:12});};
const cpair=(lo,hi)=>{for(let p=3;p<12;p++){const a=cfmt(lo,p),b=cfmt(hi,p);if(a!==b)return [a,b];}return [cfmt(lo,12),cfmt(hi,12)];};
const efmt=v=>{const s=String(v);return s.length>14?String(+v.toPrecision(10)):s;};
const pfmt=p=>{const q=p*100;return (q>=99||q<=1?+q.toFixed(3):+q.toFixed(1))+'%';};
function openShape(name){const o=document.getElementById('shapes'),d=document.getElementById(slug(name));if(!d)return;o.open=true;d.open=true;if(d.scrollIntoView)d.scrollIntoView({behavior:'smooth',block:'start'});}
function demoPlot(vals,x0,step,ylog,hl,join,label){
 const W=330,H=132,L=56,R=10,T=8,B=22,pw=W-L-R,ph=H-T-B,n=vals.length,f=ylog?(v=>Math.log10(v>0?v:1e-300)):(v=>v);
 let lo=Infinity,hi=-Infinity,ilo=0,ihi=0;
 vals.forEach((v,i)=>{const y=f(v);if(y<lo){lo=y;ilo=i;}if(y>hi){hi=y;ihi=i;}});
 const span=hi>lo?hi-lo:1,b0=hi>lo?lo:lo-0.5,lab=cpair(vals[ilo],vals[ihi]);
 const X=i=>(L+(n>1?i*pw/(n-1):pw/2)).toFixed(1),Y=v=>(T+ph-(f(v)-b0)/span*ph).toFixed(1);
 let s='<svg viewBox="0 0 '+W+' '+H+'" role="img" aria-label="'+escH(label)+'">';
 s+='<path class="ax" d="M'+L+' '+T+'V'+(T+ph)+'H'+(L+pw)+'"/>';
 s+='<text class="lbl" x="'+(L-5)+'" y="'+(T+7)+'" text-anchor="end">'+escH(lab[1])+'</text>';
 if(hi>lo)s+='<text class="lbl" x="'+(L-5)+'" y="'+(T+ph)+'" text-anchor="end">'+escH(lab[0])+'</text>';
 s+='<text class="lbl" x="'+L+'" y="'+(H-6)+'">'+(x0).toLocaleString('en-US')+'</text>';
 s+='<text class="lbl" x="'+(L+pw)+'" y="'+(H-6)+'" text-anchor="end">'+(x0+(n-1)*step).toLocaleString('en-US')+'</text>';
 if(join)s+='<polyline class="ln" points="'+vals.map((v,i)=>X(i)+','+Y(v)).join(' ')+'"/>';
 const mk=i=>!!hl&&hl.indexOf(x0+i*step)>=0;
 vals.forEach((v,i)=>{if(!mk(i))s+='<circle class="d" cx="'+X(i)+'" cy="'+Y(v)+'" r="'+(join?2.2:1.3)+'"/>';});
 vals.forEach((v,i)=>{if(mk(i))s+='<circle class="mk" cx="'+X(i)+'" cy="'+Y(v)+'" r="3.4"/>';});
 return s+'</svg>';
}
function shapeEntry(d){
 const st=d.stats,str=d.kind==='str',n=d.n,wl=d.win.length;
 const prof=['n = '+n.toLocaleString('en-US'),st.distinct.toLocaleString('en-US')+(str?' distinct strings':' distinct values')];
 if(str)prof.push(st.minLen+' to '+st.maxLen+' bytes, mean '+st.meanLen);
 else{const r=cpair(st.min,st.max);prof.push('from '+r[0]+' to '+r[1]);}
 prof.push(pfmt(st.inorder)+' of adjacent pairs in order',st.runs.toLocaleString('en-US')+' ascending runs');
 if(!str&&st.ints)prof.push('every value an integer');
 const what=str?'rank in sorted order':'value';
 let h='<p>'+escH(d.desc)+'</p><p class="prof">'+escH(prof.join(' · '))+'</p><div class="plots">';
 h+='<figure><figcaption>the whole array, every '+d.step.toLocaleString('en-US')+'th element ('+what+(d.ylog?', log axis':'')+' against position)</figcaption>'+demoPlot(d.ov,0,d.step,d.ylog,null,false,d.shape+': the whole array')+'</figure>';
 h+='<figure><figcaption>elements '+d.w0.toLocaleString('en-US')+' to '+(d.w0+wl-1).toLocaleString('en-US')+(d.hl.length?', the feature in orange':'')+'</figcaption>'+demoPlot(d.win,d.w0,1,d.ylog,d.hl,true,d.shape+': '+wl+' consecutive elements')+'</figure></div>';
 h+='<p class="cap">'+(str?'Those strings, as the sort receives them:':'The values, as the sort receives them:')+'</p>';
 if(str)h+='<ol class="lines">'+d.lines.map((t,k)=>{const i=d.t0+k;return '<li'+(d.hl.indexOf(i)>=0?' class="h"':'')+'><span>'+i+'</span>'+escH(t)+'</li>';}).join('')+'</ol>';
 else{h+='<div class="vals">';for(let i=d.t0;i<d.t0+d.tn;i++)h+='<div'+(d.hl.indexOf(i)>=0?' class="h"':'')+'><span>a['+i+']</span> '+escH(efmt(d.win[i-d.w0]))+'</div>';h+='</div>';}
 return '<details class="shp" id="'+slug(d.shape)+'"><summary><b>'+escH(d.shape)+'</b><span>'+escH(d.brief)+'</span></summary><div class="body">'+h+'</div></details>';
}
(function(){
 const host=document.getElementById('shapelist');if(!host)return;
 const seen=DATA.filter((s,i)=>i%2===0).map(s=>SHAPEDOC[s.shape]).filter(Boolean);
 host.innerHTML=seen.map(shapeEntry).join('');
 const c=document.getElementById('shapecount');if(c)c.textContent=String(seen.length);
 if(!seen.length){const o=document.getElementById('shapes');if(o)o.style.display='none';}
 if(typeof location!=='undefined'&&/^#about-/.test(location.hash||'')){const t=document.getElementById(location.hash.slice(1)),o=document.getElementById('shapes');if(t&&o){o.open=true;t.open=true;if(t.scrollIntoView)t.scrollIntoView({block:'start'});}}
})();

/* AT THE LARGEST SIZE. For each input and thread count: logos against the fastest OTHER sort at the largest n logos measured.
   "Other" is every contender whose name does not contain "logos" (its in-place entry and its pre-2026-10 string sort are logos).
   A contender that ran out of time budget before that n has no time there and drops out of that row: it was the slowest.
   Bars run from parity on a log scale, so 2x faster and 2x slower are the same length; within 5% is "level". */
const LEVEL=0.05;
const SHORT=nm=>String(nm||'').replace(/ \(.*$/,'').replace(/ key-value.*$/,'').replace(/ (AVX2|AVX-512|AVX3\w*|AVX10\w*|SSE\w*|NEON|SVE\w*)$/,'');
function glance(which){const out=[];
 for(let i=0;i<DATA.length;i+=2){const s=DATA[i+which];if(!s)continue;
  const names=s.names||(s.threads==1?NAMES1:NAMESP),rs=s.rows.filter(r=>r.n>0&&r.t[1]>0);if(!rs.length)continue;
  const r=rs[rs.length-1];let bk=-1;
  r.t.forEach((v,k)=>{if(k===1||!(v>0)||/logos/.test(names[k]||''))return;if(bk<0||v<r.t[bk])bk=k;});
  if(bk<0)continue;
  out.push({shape:s.shape,threads:s.threads,n:r.n,lg:r.t[1],ot:r.t[bk],who:SHORT(names[bk]),x:r.t[bk]/r.t[1],ok:r.ok});}
 return out;}
(function(){
 const host=document.getElementById('glance');if(!host)return;
 const G=[glance(0),glance(1)],all=G[0].concat(G[1]);
 if(!all.length){const sec=document.getElementById('glance-sec');if(sec)sec.hidden=true;return;}
 const TK=[0.02,0.05,0.1,0.2,0.5,1,2,5,10,20,50,100,200,500];
 let lo=Math.min(1,...all.map(g=>g.x))/1.08,hi=Math.max(1,...all.map(g=>g.x))*1.08;
 lo=TK.filter(t=>t<=lo).pop()||TK[0];hi=TK.find(t=>t>=hi)||TK[TK.length-1];
 const X=v=>Math.max(0,Math.min(100,(Math.log(v)-Math.log(lo))/(Math.log(hi)-Math.log(lo))*100));
 const vd=g=>g.x>1+LEVEL?'faster':g.x<1/(1+LEVEL)?'slower':'level';
 const fx=f=>(f>=10?f.toFixed(1):f.toFixed(2))+'\u00d7';
 const thr=t=>t==1?'one thread':t+' threads';
 const tip=g=>g.shape+', '+Number(g.n).toLocaleString('en-US')+' elements, '+thr(g.threads)+': logos '+fmtMs(g.lg)+', '+g.who+' '+fmtMs(g.ot)+(g.ok?'':' (an output differed from std::sort\u2019s)');
 const row=g=>{const v=vd(g),a=X(1),b=X(g.x),l=Math.min(a,b),w=Math.max(Math.abs(b-a),0.6);
  const val=v==='faster'?'<b>'+fx(g.x)+'</b> faster':v==='slower'?'<b>'+fx(1/g.x)+'</b> slower':'<b>level</b> ('+g.x.toFixed(2)+')';
  return '<div class="glr" title="'+escH(tip(g))+'"><span class="nm">'+escH(g.shape)+'</span><span class="trk"><i class="mid" style="left:'+a.toFixed(2)+'%"></i><i class="b '+(v==='faster'?'pos':v==='slower'?'neg':'eq')+'" style="left:'+l.toFixed(2)+'%;width:'+w.toFixed(2)+'%"></i></span><span class="v">'+val+'<small>vs '+escH(g.who)+(g.ok?'':' \u2717')+'</small></span></div>';};
 const ticks=TK.filter(t=>t>=lo&&t<=hi);
 const axis='<div class="glr ax" aria-hidden="true"><span></span><span class="trk">'+ticks.map((t,i)=>{const x=X(t),c=i===0?' l':i===ticks.length-1?' r':'';return '<i class="tk'+c+'" style="left:'+x.toFixed(2)+'%">'+(t>=1?t:+(1/t).toFixed(0))+'\u00d7</i>';}).join('')+
  '<i class="tk cap l" style="left:0%">\u2190 other faster</i><i class="tk cap r" style="left:100%">logos faster \u2192</i></span><span></span></div>';
 const panel=(rows,title)=>rows.length?'<div class="glc" role="group" aria-label="'+escH(title)+'"><h3>'+escH(title)+'</h3>'+rows.map(row).join('')+axis+'</div>':'';
 host.innerHTML=panel(G[0],'1 thread each')+panel(G[1],META.threads+' threads each');
 {const nw=document.getElementById('nmaxw');if(nw&&NS.length)nw.textContent=Number(NS[NS.length-1]).toLocaleString('en-US');}
 const cnt=a=>{const c={faster:0,level:0,slower:0};a.forEach(g=>c[vd(g)]++);return c;},c1=cnt(G[0]),cp=cnt(G[1]);
 const nmax=Math.max(...all.map(g=>g.n)),same=all.every(g=>g.n===nmax);
 const sc=document.getElementById('score');
 const part=(c,n,t)=>'<b>'+c.faster+' of '+n+'</b> inputs '+t+(c.level||c.slower?' (level on '+c.level+', slower on '+c.slower+')':'');
 if(sc)sc.innerHTML='At '+(same?'':'up to ')+'<b>'+Number(nmax).toLocaleString('en-US')+' elements</b>, logos is the fastest sort measured on '+
  [G[0].length?part(c1,G[0].length,'at one thread'):'',G[1].length?part(cp,G[1].length,'at '+META.threads+' threads'):''].filter(Boolean).join(' and on ')+'.';
 const tb=document.getElementById('glance-table');
 if(tb)tb.innerHTML='<table class="glance-t"><thead><tr><th>input</th><th>threads</th><th>n</th><th>logos</th><th>fastest other</th><th>its time</th><th>other \u00f7 logos</th></tr></thead><tbody>'+
  all.map(g=>'<tr><td>'+escH(g.shape)+'</td><td>'+g.threads+'</td><td>'+fmtN(g.n)+'</td><td>'+fmtMs(g.lg)+'</td><td>'+escH(g.who)+'</td><td>'+fmtMs(g.ot)+'</td><td>'+g.x.toFixed(2)+'\u00d7'+(g.ok?'':' \u2717')+'</td></tr>').join('')+'</tbody></table>';
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
  const doc=SHAPEDOC[s1.shape];
  if(doc){const br=document.createElement('p');br.className='sub';br.textContent=doc.brief.charAt(0).toUpperCase()+doc.brief.slice(1)+'. ';
   const a=document.createElement('a');a.href='#'+slug(s1.shape);a.textContent='What this shape is, with demo data';
   a.onclick=e=>{if(e&&e.preventDefault)e.preventDefault();openShape(s1.shape);};br.appendChild(a);host.appendChild(br);}
  if(s1.note){const nt=document.createElement('p');nt.className='sub';nt.textContent=s1.note;host.appendChild(nt);}   /* a series that names its own contenders says what they are */
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
/* the memory toggle: only offered when the report actually carries a memory pass (older ones do not) */
if(MEMMAX>0){const b=document.createElement('button');b.className='f';b.textContent='memory circles';
 b.title='circle radius = peak heap of one cold call, bytes per element; the largest reading in this report ('+(+MEMMAX.toFixed(1))+' B/elem) is '+(MEMPX/2)+' px; the values are in the tooltip and each panel\'s tables';
 b.setAttribute('aria-pressed',SHOWMEM);b.dataset.mem='1';
 b.onclick=()=>{SHOWMEM=!SHOWMEM;b.setAttribute('aria-pressed',SHOWMEM);render();};fl.appendChild(b);}
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
/* a viewer that switches theme by attribute (data-theme on the root) changes the tokens without changing the media query:
   the canvases resolve their colours when drawn, so redraw then too */
if(typeof MutationObserver==='function')new MutationObserver(()=>render()).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
/* for tools/check_report.js and tools/browser_check.py, which drive the charts through these */
globalThis.__SCALE={CH,IX};
</script></body></html>
)HTML";
    std::string html(TMPL);
    size_t p = html.find("__DATA__"); if (p != std::string::npos) html.replace(p, 8, j);
    p = html.find("__META__"); if (p != std::string::npos) html.replace(p, 8, meta_s);
    { double t_demo = nowms(); std::string sd = shape_demos_json();       /* the shapes accordion: generated, never measured */
      p = html.find("__SHAPES__"); if (p != std::string::npos) html.replace(p, 10, sd);
      printf("shape demos: %zu shapes at n = %zu, %.1f s\n", sizeof SHAPE_DOCS / sizeof SHAPE_DOCS[0], DEMO_N, (nowms() - t_demo) / 1e3); }
    /* uPlot is inlined, not fetched: these reports are opened from disk and passed around, so the
       page must not depend on a CDN. See engines/c/vendor/README.md and tools/embed_uplot.py. */
    p = html.find("__UPLOT_CSS__"); if (p != std::string::npos) html.replace(p, 13, UPLOT_CSS);
    p = html.find("__UPLOT_JS__");  if (p != std::string::npos) html.replace(p, 12, UPLOT_JS);
    FILE *f = fopen("scale-report.html", "w");
    if (f) { fwrite(html.data(), 1, html.size(), f); fclose(f); printf("\nwrote scale-report.html\n"); }
    printf("RESULT: %s\n", reuse ? "re-rendered; the measurements are those of the report it was read from" : bad ? "OUTPUT MISMATCHES — do not trust timings" : "all outputs verified against std::sort");
    return bad ? 1 : 0;
}
