/* xyprobe.cpp -- X+Y: the sort that sits on the quadratic line (2026-11-02, FINDINGS 3bs)
 *
 * THE PREMISE (a counterfactual, as asked): "the ETH boundary is proven to be less than n^2". ETH says 3-SAT needs 2^Omega(n) time
 * and has no n^2 in it, so the premise is read as the nearest real statement: a quadratic barrier falls. Sorting has one famous problem
 * sitting ON that line. X + Y = { x_i + y_j }: n numbers each, N = n^2 sums. Fredman (1976): the sums can fall in at most n^{8n}
 * orders (n^4 hyperplanes x_i + y_j = x_k + y_l cut R^{2n} into at most (n^4)^{2n} cells), so the input carries at most 8 n log2 n
 * bits, 8 log2(n) / n PER ELEMENT -- 0.023 at n = 4096, against log2 N - 1.44 = 22.6 for N distinct random doubles -- and O(n^2)
 * comparisons suffice. No O(n^2)-TIME algorithm is known; the best known is O(n^2 log n).
 *
 * THE TEST (invented): where the barrier is gone, X+Y sorts in O(N). Its fingerprint is a cost per element that stays FLAT from
 * N = 1M to 16M, where anything paying log2 per element grows ~+20%. So: hand the sums to every sort we have, and to one sort that is
 * TOLD the structure (X and Y themselves), and look for flat.
 *   shapes   xy_raw    a[i n + j] = x_i + y_j, X and Y in random order: what a nested loop writes. Every row is a TRANSLATE of
 *                      every other (adding x_i keeps the order of Y), which no route in the engine reads
 *            xy_rows   X and Y sorted first: n ascending rows, each spanning almost the whole range (H_r = log2 n bits)
 *            xy_shuf   xy_raw's values in random order: the same multiset with the structure gone (the control)
 *            uniform   N uniform doubles (the multiset's own effect: the sums are triangular, not uniform)
 *   told     sort X and Y (2 n log n), take B - 1 quantile splitters from a sample of sums, and PLACE: a staircase walk counts each
 *            bucket exactly (for one splitter the prefix of Y below it shrinks as x_i grows: 2n steps per splitter), then each row is
 *            written out as contiguous segments of x_i + Y, already sorted inside. Buckets of S elements are then sorted by the
 *            engine. Placement costs ~N copies + n B boundary steps, NOT N log2 B compares: n B = N^1.5 / S, so for a fixed S the
 *            total is O(N + N^1.5 / S) -- linear until n reaches S, superlinear after; balancing S gives N log n, the known bound.
 *            S in {4096, 32768, 262144}.
 *
 * PREDICTIONS, written before the first run (one thread, n = 1024, 2048, 4096):
 *   logos  xy_raw / xy_shuf   0.95-1.05   blind to the translate structure: the rows look like random triangular data
 *          xy_rows / xy_shuf  1.00-1.20   4096 overlapping runs pass every run gate by; the vector quicksort's order cost (3bn)
 *          xy_shuf / uniform  0.95-1.10
 *   told / logos on xy_raw at 16M (below 1 = told faster):
 *          S = 262144  0.75-0.90  placement ~1 ns per element, then 18 bits of sorting instead of 24
 *          S = 32768   0.70-0.88  segments of ~8 elements at 16M
 *          S = 4096    0.90-1.15  segments of ~1: the placement is a scatter again, and n B = N
 *   growth of the cost per element, one 16M sort against sixteen 1M sorts (each its own X and Y):
 *          logos xy_raw +15..+25%, std::sort +20..+30% (comparison-bound)
 *          told S = 262144 +0..+10%, S = 32768 +3..+15% (flat bucket sorts, the n B term rising as sqrt N), S = 4096 +15..+35%
 *   The counterfactual's fingerprint (growth ~0, at a cost far under log2 n bits): nothing the engine has; the told S = 262144 sort
 *   nearest, and only because its superlinear term N^1.5 / S stays invisible below N = S^2 ~ 7e10.
 *
 * Usage: ./xyprobe [pairs] [n list]     defaults 12, "1024,2048,4096" (N = n^2; the growth section needs the first and last to be
 *        1024 and 4096). Every contender verified (ascending + bit multiset) before it is timed; every ratio from ADJACENT pairs,
 *        order alternating, median [p10 p90]. One thread throughout.
 *        ./xyprobe phases [reps] [n]   defaults 5, 4096: the told sort split into setup / count / place / bucket sorts, and its buckets
 *        as placed against the same buckets shuffled inside (does the order inside a bucket cost the engine?). */
#include "logos_psort.h"
#include "bench_common.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* one instance: X and Y (in the order given) and the N sums in the shape's layout */
struct Inst { size_t n; std::vector<double> x, y, a; };
enum { RAW, ROWS, SHUF, UNI };
static const char *SHN[] = { "xy_raw", "xy_rows", "xy_shuf", "uniform" };
static void make(Inst &I, size_t n, uint64_t seed, int shape) {
    Rng r{seed * 2654435761ULL + 977};
    I.n = n; I.x.resize(n); I.y.resize(n); I.a.resize(n * n);
    for (size_t i = 0; i < n; i++) I.x[i] = r.f() * 1e6;
    for (size_t j = 0; j < n; j++) I.y[j] = r.f() * 1e6;
    if (shape == ROWS) { std::sort(I.x.begin(), I.x.end()); std::sort(I.y.begin(), I.y.end()); }
    for (size_t i = 0; i < n; i++) for (size_t j = 0; j < n; j++) I.a[i * n + j] = I.x[i] + I.y[j];
    if (shape == SHUF) for (size_t k = n * n - 1; k > 0; k--) std::swap(I.a[k], I.a[r.next() % (k + 1)]);
    if (shape == UNI) for (size_t k = 0; k < n * n; k++) I.a[k] = r.f() * 2e6;
}

typedef void (*cont_t)(const Inst &, double *);                  /* w holds a copy of I.a; the contender leaves it sorted */
static void c_logos(const Inst &I, double *w) { logos_sort_f64(w, I.a.size(), 1); }
static void c_std(const Inst &I, double *w) { std::sort(w, w + I.a.size()); }

static size_t g_S = 32768;                                        /* the told sort's bucket size */
static int g_told_bad;                                            /* a cursor that did not land on its bucket's end */
static double *g_ph;                                              /* phases mode: 4 accumulators (setup, count, place, sort), else NULL */
static int g_place_only;                                          /* phases mode: stop after placing, leave g_off */
static std::vector<size_t> g_off;
static void c_told(const Inst &I, double *w) {
    const size_t n = I.n, N = n * n;
    size_t B = N / g_S; if (B < 1) B = 1;
    double t0 = g_ph ? nowms() : 0;
    std::vector<double> xs(I.x), ys(I.y);
    logos_sort_f64(xs.data(), n, 1); logos_sort_f64(ys.data(), n, 1);
    std::vector<double> sp(B > 1 ? B - 1 : 0);                    /* B - 1 splitters: every 32nd of 32 B sampled sums */
    if (B > 1) {
        size_t m = 32 * B; std::vector<double> smp(m); Rng r{0x5EEDULL ^ (uint64_t)N};
        for (size_t k = 0; k < m; k++) { size_t i = r.next() % n, j = r.next() % n; smp[k] = xs[i] + ys[j]; }
        logos_sort_f64(smp.data(), m, 1);
        for (size_t b = 1; b < B; b++) sp[b - 1] = smp[b * 32];
    }
    if (g_ph) { double t = nowms(); g_ph[0] += t - t0; t0 = t; }
    /* bucket b holds [sp[b-1], sp[b]); off[b+1] = #{sums < sp[b]}, by a staircase: as x_i grows, the prefix of Y below sp[b] shrinks */
    std::vector<size_t> off(B + 1); off[0] = 0; off[B] = N;
    for (size_t b = 0; b + 1 < B; b++) {
        size_t j = n, c = 0; const double s = sp[b];
        for (size_t i = 0; i < n; i++) { const double xi = xs[i]; while (j > 0 && !(xi + ys[j - 1] < s)) j--; c += j; }
        off[b + 1] = c;
    }
    if (g_ph) { double t = nowms(); g_ph[1] += t - t0; t0 = t; }
    /* place: each row is a run of segments of x_i + Y, sorted inside; one write pointer, saved when the bucket changes. A row's first
       bucket never moves down (x_i grows), so each row starts from the last row's */
    std::vector<size_t> cur(off.begin(), off.end() - 1);
    const size_t last = B - 1; size_t b0 = 0;
    for (size_t i = 0; i < n; i++) {
        const double xi = xs[i]; size_t b = b0; double *d = w + cur[b];
        double v = xi + ys[0];
        while (b < last && !(v < sp[b])) { cur[b] = (size_t)(d - w); d = w + cur[++b]; }
        b0 = b; *d++ = v;
        for (size_t j = 1; j < n; j++) {
            v = xi + ys[j];
            while (b < last && !(v < sp[b])) { cur[b] = (size_t)(d - w); d = w + cur[++b]; }
            *d++ = v;
        }
        cur[b] = (size_t)(d - w);
    }
    for (size_t b = 0; b < B; b++) if (cur[b] != off[b + 1]) { g_told_bad = 1; return; }
    if (g_ph) { double t = nowms(); g_ph[2] += t - t0; t0 = t; }
    if (g_place_only) { g_off = off; return; }
    for (size_t b = 0; b < B; b++) logos_sort_f64(w + off[b], off[b + 1] - off[b], 1);
    if (g_ph) g_ph[3] += nowms() - t0;
}

static double median(std::vector<double> &v, double *p10, double *p90);
/* ./xyprobe phases [reps] [n]: where the told sort's time goes, per S, and whether its buckets (unions of sorted segments of
   x_i + Y) cost the engine more than the same bucket contents shuffled: the order cost of 3bn, inside the told sort */
static int phases_main(int reps, size_t n) {
    const size_t N = n * n; Inst raw; make(raw, n, 11 + n, RAW);
    std::vector<double> w(N), w2(N);
    double tl = 1e30; for (int r = 0; r < reps; r++) { memcpy(w.data(), raw.a.data(), N * 8); double t0 = nowms(); logos_sort_f64(w.data(), N, 1); tl = std::min(tl, nowms() - t0); }
    printf("# xyprobe phases: n = %zu, N = %zu, min of %d; logos on xy_raw %.2f ns per element\n", n, N, reps, tl * 1e6 / (double)N);
    printf("%-9s %6s %8s | %7s %7s %7s %7s %7s | %-24s\n", "S", "B", "seg len", "setup", "count", "place", "sort", "total", "bucket sorts: placed/shuf");
    for (size_t S : { (size_t)262144, (size_t)32768, (size_t)4096 }) {
        g_S = S; size_t B = N / S;
        double best[4] = { 1e30, 1e30, 1e30, 1e30 }, tot = 1e30;
        for (int r = 0; r < reps; r++) {
            double ph[4] = { 0, 0, 0, 0 }; g_ph = ph; double t0 = nowms(); c_told(raw, w.data()); double t = nowms() - t0; g_ph = NULL;
            for (int k = 0; k < 4; k++) best[k] = std::min(best[k], ph[k]); tot = std::min(tot, t);
        }
        /* the buckets as placed against the same buckets shuffled inside, sorted adjacently, order alternating */
        g_place_only = 1; c_told(raw, w.data()); g_place_only = 0; std::vector<size_t> off = g_off;
        memcpy(w2.data(), w.data(), N * 8); Rng rr{77};
        for (size_t b = 0; b < B; b++) for (size_t k = off[b + 1] - 1; k > off[b]; k--) std::swap(w2[k], w2[off[b] + rr.next() % (k - off[b] + 1)]);
        std::vector<double> placed(N), shuf(N), rat;
        for (int r = 0; r < reps; r++) {
            double t[2];
            for (int side = 0; side < 2; side++) {
                bool p = (side == 0) == ((r & 1) == 0);
                std::vector<double> &dst = p ? placed : shuf; memcpy(dst.data(), p ? w.data() : w2.data(), N * 8);
                double t0 = nowms(); for (size_t b = 0; b < B; b++) logos_sort_f64(dst.data() + off[b], off[b + 1] - off[b], 1); t[p ? 0 : 1] = nowms() - t0;
            }
            rat.push_back(t[0] / t[1]);
        }
        double p10, p90, m = median(rat, &p10, &p90);
        auto ns = [&](double ms) { return ms * 1e6 / (double)N; };
        printf("%-9zu %6zu %8.1f | %7.2f %7.2f %7.2f %7.2f %7.2f | %5.3f [%5.3f %5.3f]\n", S, B, (double)N / ((double)n * (double)B),
               ns(best[0]), ns(best[1]), ns(best[2]), ns(best[3]), ns(tot), m, p10, p90);
        fflush(stdout);
    }
    printf("# phase columns: ns per element, each the minimum over reps; 'seg len' = N / (n B), the mean segment if every row met every bucket\n");
    return 0;
}

static uint64_t mix64(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static uint64_t bag(const double *a, size_t n) { uint64_t h = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); h += mix64(u); } return h; }
static bool verify(cont_t f, const Inst &I, double *w) {
    size_t N = I.a.size(); memcpy(w, I.a.data(), N * 8); g_told_bad = 0; f(I, w);
    if (g_told_bad) return false;
    for (size_t i = 1; i < N; i++) if (w[i] < w[i - 1]) return false;
    return bag(w, N) == bag(I.a.data(), N);
}

struct Side { cont_t f; size_t S; std::vector<const Inst *> v; size_t elems() const { size_t e = 0; for (auto *I : v) e += I->a.size(); return e; } };
static double run_side(const Side &s, double *w) {
    size_t o = 0; for (auto *I : s.v) { memcpy(w + o, I->a.data(), I->a.size() * 8); o += I->a.size(); }
    g_S = s.S; double t0 = nowms(); o = 0;
    for (auto *I : s.v) { s.f(*I, w + o); o += I->a.size(); }
    return nowms() - t0;
}
static double median(std::vector<double> &v, double *p10, double *p90) {
    std::sort(v.begin(), v.end()); size_t m = v.size();
    if (p10) *p10 = v[(size_t)(0.10 * (double)m)];
    if (p90) { size_t k = (size_t)(0.90 * (double)m); *p90 = v[k < m ? k : m - 1]; }
    return (m & 1) ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
}
/* t(A) / t(B) over adjacent pairs, order alternating; both sides' ns per element (medians) */
static double pairAB(const Side &A, const Side &B, double *w, int pairs, double *p10, double *p90, double *nsA, double *nsB) {
    std::vector<double> r, ta, tb; double ea = (double)A.elems(), eb = (double)B.elems();
    for (int p = 0; p < pairs; p++) {
        double t[2];
        for (int side = 0; side < 2; side++) { bool a = (side == 0) == ((p & 1) == 0); t[a ? 0 : 1] = run_side(a ? A : B, w); }
        r.push_back((t[0] / ea) / (t[1] / eb)); ta.push_back(t[0] * 1e6 / ea); tb.push_back(t[1] * 1e6 / eb);
    }
    if (nsA) *nsA = median(ta, NULL, NULL); if (nsB) *nsB = median(tb, NULL, NULL);
    return median(r, p10, p90);
}
static double lg2fact(double m) { return std::lgamma(m + 1.0) / std::log(2.0); }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "phases")) return phases_main(argc > 2 ? atoi(argv[2]) : 5, argc > 3 ? strtoull(argv[3], NULL, 10) : 4096);
    int pairs = argc > 1 ? atoi(argv[1]) : 12; const char *nl = argc > 2 ? argv[2] : "1024,2048,4096";
    std::vector<size_t> ns; for (const char *p = nl; *p;) { ns.push_back(strtoull(p, (char **)&p, 10)); if (*p == ',') p++; }
    size_t nmax = 0; for (size_t v : ns) if (v > nmax) nmax = v;
    std::vector<double> w(nmax * nmax);
    const size_t SS[] = { 262144, 32768, 4096 };
    printf("# xyprobe: the N = n^2 sums X+Y, one thread, %d pairs per cell; cell = t(A) / t(B) per element [p10 p90], below 1 = A faster\n", pairs);
    for (size_t n : ns) {
        size_t N = n * n;
        Inst raw, rows, shuf, uni;
        make(raw, n, 11 + n, RAW); make(rows, n, 11 + n, ROWS); make(shuf, n, 11 + n, SHUF); make(uni, n, 11 + n, UNI);
        /* verify everything first; the bits come from the sorted output */
        if (!verify(c_logos, raw, w.data())) { printf("VERIFY FAILED logos xy_raw n=%zu\n", n); return 1; }
        double hm = lg2fact((double)N); for (size_t i = 0; i < N;) { size_t j = i + 1; while (j < N && w[j] == w[i]) j++; hm -= lg2fact((double)(j - i)); i = j; }
        hm /= (double)N;
        double hr = 0; for (size_t i = 0; i < N;) { size_t j = i + 1; while (j < N && !(rows.a[j] < rows.a[j - 1])) j++; double L = (double)(j - i); hr += L * std::log2((double)N / L); i = j; }
        hr /= (double)N;
        for (const Inst *I : { &rows, &shuf, &uni }) if (!verify(c_logos, *I, w.data())) { printf("VERIFY FAILED logos n=%zu\n", n); return 1; }
        if (!verify(c_std, raw, w.data())) { printf("VERIFY FAILED std n=%zu\n", n); return 1; }
        for (size_t S : SS) { g_S = S; if (!verify(c_told, raw, w.data())) { printf("VERIFY FAILED told S=%zu n=%zu\n", S, n); return 1; } }
        printf("\n## n = %zu, N = %zu: bits per element  random-order bound H_m %.2f | xy_rows' runs H_r %.2f | Fredman's X+Y bound 8 log2(n)/n %.4f\n",
               n, N, hm, hr, 8.0 * std::log2((double)n) / (double)n);
        struct Row { const char *name; Side A, B; } R[] = {
            { "logos xy_raw / xy_shuf",  { c_logos, 0, { &raw } },  { c_logos, 0, { &shuf } } },
            { "logos xy_rows / xy_shuf", { c_logos, 0, { &rows } }, { c_logos, 0, { &shuf } } },
            { "logos xy_shuf / uniform", { c_logos, 0, { &shuf } }, { c_logos, 0, { &uni } } },
            { "std::sort / logos xy_raw", { c_std, 0, { &raw } },   { c_logos, 0, { &raw } } },
            { "told S=262144 / logos",   { c_told, 262144, { &raw } }, { c_logos, 0, { &raw } } },
            { "told S=32768 / logos",    { c_told, 32768, { &raw } },  { c_logos, 0, { &raw } } },
            { "told S=4096 / logos",     { c_told, 4096, { &raw } },   { c_logos, 0, { &raw } } },
        };
        for (auto &row : R) {
            int pp = row.A.f == c_std ? (pairs > 4 ? 4 : pairs) : pairs;
            double p10, p90, na, nb, m = pairAB(row.A, row.B, w.data(), pp, &p10, &p90, &na, &nb);
            printf("%-26s A %6.2f ns  B %6.2f ns  A/B %5.3f [%5.3f %5.3f]\n", row.name, na, nb, m, p10, p90); fflush(stdout);
        }
    }
    /* growth of the cost per element: one sort at the largest n against K sorts at the smallest, each instance its own X and Y */
    size_t nlo = ns.front(), nhi = ns.back();
    if (nhi > nlo && (nhi * nhi) % (nlo * nlo) == 0) {
        size_t K = (nhi * nhi) / (nlo * nlo);
        printf("\n## growth of the cost per element, one N = %zu sort against %zu sorts of N = %zu (log2 %.0f -> %.0f: log2-bound +%.0f%%)\n",
               nhi * nhi, K, nlo * nlo, std::log2((double)(nlo * nlo)), std::log2((double)(nhi * nhi)),
               (std::log2((double)(nhi * nhi)) / std::log2((double)(nlo * nlo)) - 1) * 100);
        for (int shape : { RAW, SHUF }) {
            Inst hi; make(hi, nhi, 501, shape);
            std::vector<Inst> lo(K); std::vector<const Inst *> lop; for (size_t k = 0; k < K; k++) { make(lo[k], nlo, 601 + k, shape); lop.push_back(&lo[k]); }
            struct G { const char *name; cont_t f; size_t S; } GR[] = { { "logos", c_logos, 0 }, { "std::sort", c_std, 0 },
                { "told S=262144", c_told, 262144 }, { "told S=32768", c_told, 32768 }, { "told S=4096", c_told, 4096 } };
            for (auto &g : GR) {
                if (shape == SHUF && g.f == c_told) continue;          /* told reads X and Y; on xy_shuf it is the same sort as on xy_raw */
                double p10, p90, na, nb;
                double m = pairAB(Side{ g.f, g.S, { &hi } }, Side{ g.f, g.S, lop }, w.data(), g.f == c_std ? (pairs > 4 ? 4 : pairs) : pairs, &p10, &p90, &na, &nb);
                printf("%-8s %-14s %6.2f -> %6.2f ns per element  growth %+5.1f%% [%+5.1f %+5.1f]\n", SHN[shape], g.name, nb, na, (m - 1) * 100, (p10 - 1) * 100, (p90 - 1) * 100);
                fflush(stdout);
            }
        }
    }
    return 0;
}
