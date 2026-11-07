/* primeprobe.cpp -- a knot solver that unties only the PRIME FACTORS of the diagram (2026-11-05, FINDINGS 3bv)
 *
 * Asked: "is there a faster knot solving algorithm you can hallucinate and benchmark against?" The engine's knot route reads
 * the array as strands (sorted runs) and unties it GLOBALLY: one sweep over the strand endpoints, every piece lifted into
 * scratch and laid down again (half a scratch since 3bq), crossings merged on the way. The trace of an in-order staircase
 * (sorted batches arriving in order, each overlapping the next) shows what that wastes: 1,651,455 of 8,388,608 elements cross,
 * and all 8.4M are moved. At 14 threads the knot route does not even run there: the near-sorted route takes it and sorts
 * 825,721 "strays".
 *
 * THE INVENTED SOLVER. A knot is a connected sum of prime knots (Schubert), and each factor can be untied alone. For strands:
 *   pinned arc   an element x of ascending run i is ALREADY IN ITS FINAL PLACE when every element before it is <= x and every
 *                element after it is >= x. Inside a run that needs only the runs' extremes: with pmax = the largest value in
 *                the runs before i and smin = the smallest in the runs after i, the pinned arc of run i is its elements in
 *                [pmax, smin], found by two binary searches. O(R log n) from the run list, no pass over the elements.
 *   prime factor the stretch between two consecutive pinned arcs. It is closed under sorting, so it is untied alone, in place:
 *                two ascending pieces are merged through a buffer of the factor's own size (two chains, from the heads and
 *                from the tails), a descending strand is reversed where it lies (the twist), anything else goes to the engine.
 *   The pinned arcs are never read again after the scan. Scratch is the largest simple factor, not n / 2. Factors share
 *   nothing, so at P threads they are a work queue with no barrier and no serial plan.
 * A run cut in two (at a chunk boundary of the parallel scan) is two runs whose pieces pin each other: nothing to join.
 *
 * Contenders, every output checked (ascending + the bit multiset) before timing, every cell from adjacent pairs, order alternating:
 *   engine      logos_sort_f64 as it ships
 *   prime+eng   scan, pinned arcs, every factor handed to logos_sort_f64 (one thread): the decomposition alone, the engine's kernels
 *   prime+mrg   scan, pinned arcs, the solver's own in-place merges and reversals; at P threads the scan in P chunks and the
 *               factors from an OpenMP queue (the engine may not be called concurrently, so complex factors wait and take it at P)
 *   eng, no ns  at P threads only: the engine with LOGOS_NO_NEARSORTED=1, so that what the prime solver beats there is told
 *               apart: the knot route's parallel form, or the near-sorted route's claim on a shape that is not its own
 * Shuffled shapes have no pinned arcs: one factor, the engine does everything, and the standalone solver pays its scan on top.
 * That row is the price of the teller; integrated, the decomposition would reuse the detector's run list and cost microseconds.
 *
 * PREDICTIONS, written before the first run (8.4M, 64 blocks; cells are t(engine) / t(solver), above 1 = the solver is faster):
 *   one thread, prime+mrg     stairs in order o = 0.05  2.0-2.8    o = 0.2  1.4-1.9    o = 0.5  1.0-1.3    o = 1.0 (nothing pinned) 0.75-0.90
 *                             blocks in order, every other twisted 2.0-2.6    8 neighbour pairs swapped 1.7-2.2
 *                             shuffled blocks / shuffled stairs 0.75-0.90 (the scan twice)   sorted 0.9-1.1   uniform 0.97-1.00 (bails)
 *               prime+eng     behind prime+mrg by 10-30% on the staircases (a dispatcher entered 63 times), level elsewhere
 *   14 threads, prime+mrg     stairs in order o = 0.05  3-6    o = 0.2  2.5-4    o = 0.5  1.5-2.5    twisted 2-4    swapped 2-3
 *                             shuffled 0.85-0.95    against "eng, no ns" the staircases 1.5-3 (the rest is the route's misfire)
 *   The solver's scan: 0.2-0.3 ns per element at one thread, about 0.1 at 14.
 *
 * AFTER THE SMOKE RUN (6 pairs), three changes, each recorded: (1) at 14 threads the first cut sent complex factors to the
 *   engine at P threads and "o = 1.0" read 0.14: the ENGINE, called at 14 threads on 262k elements of two or three overlapping
 *   runs, takes 12.8-15.1 ns per element against 1.7-2.4 at one thread and 1.3-1.5 with LOGOS_NO_NEARSORTED=1
 *   (PRIMEPROBE_DIAG="stairs in order, o=1.0" prints it). An engine defect, not the solver's; complex factors now take one
 *   thread each. (2) The gate: at least a quarter of the array settled, else the engine. (3) A two-piece factor that does not
 *   interleave is a block swap and is rotated through the buffer (the swapped-pairs row: prime+mrg 1.54 -> 2.78 at one thread).
 *   PREDICTIONS for what was added: "o = 1.0" now bails, 0.80-0.90 at one thread and 0.75-0.85 at 14; "4 batches late"
 *   (complex factors about a quarter of the array) one thread 1.10-1.35, 14 threads 1.5-3.5 against the engine and 0.85-1.05
 *   against the engine with the near-sorted route off.
 *
 * Usage: ./primeprobe [pairs] [n] [P] [row filter]     defaults 30, 8388608, BENCH_THREADS. Re-executes itself with
 *        OMP_WAIT_POLICY=PASSIVE (libgomp's threads spin after a region and the engine's sort would run beside them). */
#include "logos_psort.h"
#include "bench_common.h"
#include <immintrin.h>
#include <omp.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run { size_t lo, hi; double mn, mx; int desc; };
enum { K_ENGINE, K_MERGE, K_REVERSE };
struct Factor { size_t p, q, mid; int kind; };
struct Stats { size_t runs = 0, factors = 0, simple = 0, pinned = 0, turned = 0, largest = 0, buf = 0; int bailed = 0; double t_scan = 0, t_plan = 0, t_solve = 0; };

/* first j' >= j with a[j'] < a[j' - 1] (an ascending run's end), or n */
static inline size_t asc_end(const double *a, size_t j, size_t n) {
    while (j + 16 <= n) {
        __m256d m0 = _mm256_cmp_pd(_mm256_loadu_pd(a + j), _mm256_loadu_pd(a + j - 1), _CMP_LT_OQ);
        __m256d m1 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 4), _mm256_loadu_pd(a + j + 3), _CMP_LT_OQ);
        __m256d m2 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 8), _mm256_loadu_pd(a + j + 7), _CMP_LT_OQ);
        __m256d m3 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 12), _mm256_loadu_pd(a + j + 11), _CMP_LT_OQ);
        if (_mm256_movemask_pd(_mm256_or_pd(_mm256_or_pd(m0, m1), _mm256_or_pd(m2, m3)))) break;
        j += 16;
    }
    while (j < n && !(a[j] < a[j - 1])) j++;
    return j;
}
/* first j' >= j with !(a[j'] < a[j' - 1]) (a strictly descending run's end), or n */
static inline size_t desc_end(const double *a, size_t j, size_t n) {
    while (j + 16 <= n) {
        __m256d m0 = _mm256_cmp_pd(_mm256_loadu_pd(a + j), _mm256_loadu_pd(a + j - 1), _CMP_NLT_UQ);
        __m256d m1 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 4), _mm256_loadu_pd(a + j + 3), _CMP_NLT_UQ);
        __m256d m2 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 8), _mm256_loadu_pd(a + j + 7), _CMP_NLT_UQ);
        __m256d m3 = _mm256_cmp_pd(_mm256_loadu_pd(a + j + 12), _mm256_loadu_pd(a + j + 11), _CMP_NLT_UQ);
        if (_mm256_movemask_pd(_mm256_or_pd(_mm256_or_pd(m0, m1), _mm256_or_pd(m2, m3)))) break;
        j += 16;
    }
    while (j < n && a[j] < a[j - 1]) j++;
    return j;
}
/* natural runs of [lo, hi): ascending (ties continue) or strictly descending. false = more than cap runs (noise: not this solver's data) */
static bool scan_runs(const double *a, size_t lo, size_t hi, std::vector<Run> &out, size_t cap) {
    size_t i = lo;
    while (i < hi) {
        if (out.size() >= cap) return false;
        Run r; r.lo = i; size_t j = i + 1;
        if (j < hi && a[j] < a[i]) { j = desc_end(a, j + 1 < hi ? j + 1 : hi, hi); r.desc = 1; r.mx = a[i]; r.mn = a[j - 1]; }
        else {
            j = asc_end(a, j, hi);
            /* an ascending run swallows the first element of a twisted block that starts higher: when the descent goes on,
               give that element back to the descending run it begins */
            if (j + 1 < hi && a[j + 1] < a[j] && j - 1 > i) j--;
            r.desc = 0; r.mn = a[i]; r.mx = a[j - 1];
        }
        r.hi = j; out.push_back(r); i = j;
    }
    return true;
}

/* pinned arcs and the factors between them */
static void decompose(const double *a, size_t n, const std::vector<Run> &R, std::vector<Factor> &F, Stats &st) {
    const double INF = std::numeric_limits<double>::infinity();
    size_t m = R.size(); std::vector<double> sufmin(m + 1); sufmin[m] = INF;
    for (size_t i = m; i-- > 0;) sufmin[i] = std::min(sufmin[i + 1], R[i].mn);
    auto emit = [&](size_t p, size_t q, size_t r0, size_t r1) {
        Factor f; f.p = p; f.q = q; f.mid = 0; f.kind = K_ENGINE;
        bool all_desc = true, chain = true;
        for (size_t k = r0; k <= r1; k++) { if (!R[k].desc) all_desc = false; if (k > r0 && !(R[k - 1].mn >= R[k].mx)) chain = false; }
        if (all_desc && chain) f.kind = K_REVERSE;                       /* one descending strand (in pieces or not) */
        else if (r1 == r0) return;                                        /* a piece of one ascending run: already sorted */
        else if (r1 == r0 + 1 && !R[r0].desc && !R[r1].desc) { f.kind = K_MERGE; f.mid = R[r1].lo; }
        F.push_back(f); if (q - p > st.largest) st.largest = q - p;
        if (f.kind != K_ENGINE) { st.simple++; if (f.kind == K_MERGE && q - p > st.buf) st.buf = q - p; if (f.kind == K_REVERSE) st.turned += q - p; }
    };
    double pmax = -INF; size_t open = 0, frun = 0;
    for (size_t i = 0; i < m; i++) {
        const Run &r = R[i];
        if (!r.desc) {
            const double *b = a + r.lo, *e = a + r.hi;
            const double *plo = std::lower_bound(b, e, pmax), *phi = std::upper_bound(b, e, sufmin[i + 1]);
            if (plo < phi) {
                size_t P0 = (size_t)(plo - a), P1 = (size_t)(phi - a);
                if (P0 > open) emit(open, P0, frun, P0 > r.lo ? i : i - 1);
                st.pinned += P1 - P0; open = P1; frun = P1 < r.hi ? i : i + 1;
            }
        }
        pmax = std::max(pmax, r.mx);
    }
    if (open < n) emit(open, n, frun, m - 1);
    st.runs = m; st.factors = F.size();
}

/* two ascending pieces x[0, na) and x[na, na + nb), merged in place through buf: two chains, the smallest from the heads and
   the largest from the tails, until fewer than two elements of a side are left between them; then one plain merge of the rest */
static void merge2(double *x, size_t na, size_t nb, double *buf) {
    if (x[na + nb - 1] < x[0]) {                                           /* every right element below every left one: two blocks swapped */
        if (na <= nb) { memcpy(buf, x, na * sizeof(double)); memmove(x, x + na, nb * sizeof(double)); memcpy(x + nb, buf, na * sizeof(double)); }
        else { memcpy(buf, x + na, nb * sizeof(double)); memmove(x + nb, x, na * sizeof(double)); memcpy(x, buf, nb * sizeof(double)); }
        return;
    }
    memcpy(buf, x, (na + nb) * sizeof(double));
    const double *l = buf, *r = buf + na;
    ptrdiff_t i = 0, j = 0, i2 = (ptrdiff_t)na - 1, j2 = (ptrdiff_t)nb - 1;
    double *o = x, *o2 = x + na + nb - 1;
    while (i < i2 && j < j2) {
        double u = l[i], v = r[j]; int t = v < u; *o++ = t ? v : u; j += t; i += !t;
        double u2 = l[i2], v2 = r[j2]; int t2 = u2 > v2; *o2-- = t2 ? u2 : v2; i2 -= t2; j2 -= !t2;
    }
    while (i <= i2 && j <= j2) { double u = l[i], v = r[j]; int t = v < u; *o++ = t ? v : u; j += t; i += !t; }
    while (i <= i2) *o++ = l[i++];
    while (j <= j2) *o++ = r[j++];
}

/* THE GATE (added after the smoke run): untie by factors only when at least a quarter of the array is settled without a
   compare -- pinned, or a descending strand turned where it lies. Below that the tangles are most of the array and the engine's
   global sweep is the better solver; the whole array goes to it. PRIMEPROBE_NOGATE=1 lifts the gate. */
static size_t PRIME_SETTLED_DIV = 4;
/* mode 0: every factor to the engine (the decomposition alone); mode 1: the solver's own merges and reversals */
static void prime_sort(double *a, size_t n, int mode, Stats *sto) {
    Stats st; double t0 = nowms();
    std::vector<Run> R; R.reserve(256);
    if (!scan_runs(a, 0, n, R, n / 256 + 16)) { st.bailed = 1; logos_sort_f64(a, n, 1); if (sto) *sto = st; return; }
    double t1 = nowms(); st.t_scan = t1 - t0;
    std::vector<Factor> F; decompose(a, n, R, F, st);
    if (st.pinned + st.turned < n / PRIME_SETTLED_DIV) { st.bailed = 2; logos_sort_f64(a, n, 1); if (sto) *sto = st; return; }
    double t2 = nowms(); st.t_plan = t2 - t1;
    double *buf = (mode == 1 && st.buf) ? (double *)malloc(st.buf * sizeof(double)) : NULL;
    for (const Factor &f : F) {
        if (f.kind == K_REVERSE) std::reverse(a + f.p, a + f.q);
        else if (f.kind == K_MERGE && mode == 1 && buf) merge2(a + f.p, f.mid - f.p, f.q - f.mid, buf);
        else logos_sort_f64(a + f.p, f.q - f.p, 1);
    }
    free(buf);
    st.t_solve = nowms() - t2;
    if (sto) *sto = st;
}
static void prime_sort_par(double *a, size_t n, int P, Stats *sto) {
    Stats st; double t0 = nowms();
    std::vector<std::vector<Run>> part((size_t)P); std::vector<char> ok((size_t)P, 1);
    const size_t capc = (n / 256 + 16) / (size_t)P + 16;
    #pragma omp parallel for num_threads(P) schedule(static, 1)
    for (int t = 0; t < P; t++) ok[(size_t)t] = scan_runs(a, n * (size_t)t / (size_t)P, n * (size_t)(t + 1) / (size_t)P, part[(size_t)t], capc);
    bool fine = true; for (char c : ok) fine = fine && c;
    if (!fine) { st.bailed = 1; logos_sort_f64(a, n, P); if (sto) *sto = st; return; }
    std::vector<Run> R; for (auto &v : part) R.insert(R.end(), v.begin(), v.end());
    double t1 = nowms(); st.t_scan = t1 - t0;
    std::vector<Factor> F; decompose(a, n, R, F, st);
    if (st.pinned + st.turned < n / PRIME_SETTLED_DIV) { st.bailed = 2; logos_sort_f64(a, n, P); if (sto) *sto = st; return; }
    double t2 = nowms(); st.t_plan = t2 - t1;
    const long nf = (long)F.size();
    #pragma omp parallel num_threads(P)
    {
        double *buf = st.buf ? (double *)malloc(st.buf * sizeof(double)) : NULL;
        #pragma omp for schedule(dynamic, 1)
        for (long k = 0; k < nf; k++) {
            const Factor &f = F[(size_t)k];
            if (f.kind == K_REVERSE) std::reverse(a + f.p, a + f.q);
            else if (f.kind == K_MERGE && buf) merge2(a + f.p, f.mid - f.p, f.q - f.mid, buf);
        }
        free(buf);
    }
    /* a complex factor is untied by ONE thread, like every factor: the engine at one thread, one factor after another. That is
       conservative (integrated, they would be queued with the rest). PRIMEPROBE_CPLXP=1 restores the first cut, the engine at P
       threads per factor, which is where the engine's own defect on overlapping strands showed (see PRIMEPROBE_DIAG). */
    const int cp = getenv("PRIMEPROBE_CPLXP") ? P : 1;
    for (const Factor &f : F) if (f.kind == K_ENGINE || (f.kind == K_MERGE && !st.buf)) logos_sort_f64(a + f.p, f.q - f.p, cp);
    st.t_solve = nowms() - t2;
    if (sto) *sto = st;
}

/* ---- shapes ---- */
static uint64_t RS = 7;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / 9007199254740992.0; }
static const size_t NB = 64;
/* 64 sorted blocks; block b holds b * blk + j * (1 + o) + jitter, so its range overlaps the next block's by o block lengths */
static void gen_blocks(std::vector<double> &a, double o, bool shuffled) {
    size_t n = a.size(), blk = (n + NB - 1) / NB; std::vector<size_t> perm(NB);
    for (size_t k = 0; k < NB; k++) perm[k] = k;
    if (shuffled) for (size_t k = NB - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5;
}
static void gen(const std::string &s, std::vector<double> &a) {
    size_t n = a.size(), blk = (n + NB - 1) / NB; RS = 7;
    if (s == "sorted") for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
    else if (s == "stairs in order, o=0.05") gen_blocks(a, 0.05, false);
    else if (s == "stairs in order, o=0.2") gen_blocks(a, 0.2, false);
    else if (s == "stairs in order, o=0.5") gen_blocks(a, 0.5, false);
    else if (s == "stairs in order, o=1.0") gen_blocks(a, 1.0, false);
    else if (s == "blocks in order, alt twisted") { gen_blocks(a, 0.0, false); for (size_t b = 0; b * blk < n; b += 2) std::reverse(a.begin() + (std::ptrdiff_t)(b * blk), a.begin() + (std::ptrdiff_t)std::min(n, (b + 1) * blk)); }
    else if (s == "blocks, 8 neighbour pairs swapped") { gen_blocks(a, 0.0, false);
        for (size_t b = 4; b + 1 < NB && (b + 2) * blk <= n; b += 8) std::swap_ranges(a.begin() + (std::ptrdiff_t)(b * blk), a.begin() + (std::ptrdiff_t)((b + 1) * blk), a.begin() + (std::ptrdiff_t)((b + 1) * blk)); }
    else if (s == "stairs o=0.2, 4 batches late") { gen_blocks(a, 0.2, false);       /* four batches each arrive three places late */
        for (size_t b = 8; b + 4 <= NB && (b + 4) * blk <= n; b += 16) std::rotate(a.begin() + (std::ptrdiff_t)(b * blk), a.begin() + (std::ptrdiff_t)((b + 1) * blk), a.begin() + (std::ptrdiff_t)((b + 4) * blk)); }
    else if (s == "stairs SHUFFLED, o=0.2 (guard)") gen_blocks(a, 0.2, true);
    else if (s == "blocks SHUFFLED (guard)") gen_blocks(a, 0.0, true);
    else if (s == "uniform (guard)") for (auto &v : a) v = fr() * 2e9 - 1e9;
    else { fprintf(stderr, "unknown shape %s\n", s.c_str()); exit(2); }
}
static const char *SHAPES[] = { "sorted", "stairs in order, o=0.05", "stairs in order, o=0.2", "stairs in order, o=0.5", "stairs in order, o=1.0",
                                "blocks in order, alt twisted", "blocks, 8 neighbour pairs swapped", "stairs o=0.2, 4 batches late", "stairs SHUFFLED, o=0.2 (guard)",
                                "blocks SHUFFLED (guard)", "uniform (guard)" };

static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }
static int g_P = 1;
typedef void (*cont_t)(double *, size_t);
static void c_eng1(double *a, size_t n) { logos_sort_f64(a, n, 1); }
static void c_pe(double *a, size_t n) { prime_sort(a, n, 0, NULL); }
static void c_pm(double *a, size_t n) { prime_sort(a, n, 1, NULL); }
static void c_engP(double *a, size_t n) { logos_sort_f64(a, n, g_P); }
static void c_engP_nons(double *a, size_t n) { setenv("LOGOS_NO_NEARSORTED", "1", 1); logos_sort_f64(a, n, g_P); unsetenv("LOGOS_NO_NEARSORTED"); }
static void c_pmP(double *a, size_t n) { prime_sort_par(a, n, g_P, NULL); }
static bool verify(cont_t f, const std::vector<double> &in, std::vector<double> &w) {
    size_t n = in.size(); memcpy(w.data(), in.data(), n * 8); f(w.data(), n);
    for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) return false;
    return bitsum(w.data(), n) == bitsum(in.data(), n);
}
static double med(std::vector<double> &v, double *p10, double *p90) {
    std::sort(v.begin(), v.end()); size_t m = v.size();
    if (p10) *p10 = v[(size_t)(0.10 * (double)m)];
    if (p90) { size_t k = (size_t)(0.90 * (double)m); *p90 = v[k < m ? k : m - 1]; }
    return (m & 1) ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
}
/* t(A) / t(B), adjacent, order alternating; also both medians in ns per element */
static double pairAB(cont_t A, cont_t B, const std::vector<double> &in, std::vector<double> &w, int pairs, double *p10, double *p90, double *nsA, double *nsB) {
    size_t n = in.size(); std::vector<double> r, ta, tb;
    for (int p = 0; p < pairs; p++) {
        double t[2];
        for (int side = 0; side < 2; side++) {
            bool isA = (side == 0) == ((p & 1) == 0);
            memcpy(w.data(), in.data(), n * 8); double t0 = nowms(); (isA ? A : B)(w.data(), n); t[isA ? 0 : 1] = nowms() - t0;
        }
        r.push_back(t[0] / t[1]); ta.push_back(t[0] * 1e6 / (double)n); tb.push_back(t[1] * 1e6 / (double)n);
    }
    *nsA = med(ta, NULL, NULL); *nsB = med(tb, NULL, NULL);
    return med(r, p10, p90);
}

int main(int argc, char **argv) {
    if (!getenv("OMP_WAIT_POLICY")) { setenv("OMP_WAIT_POLICY", "PASSIVE", 1); execv("/proc/self/exe", argv); perror("execv"); return 1; }
    int pairs = argc > 1 ? atoi(argv[1]) : 30; if (pairs < 4) pairs = 4;
    size_t n = argc > 2 ? strtoull(argv[2], NULL, 10) : 8388608;
    g_P = argc > 3 ? atoi(argv[3]) : bench_threads(logos_num_cpus()); if (g_P < 1) g_P = 1;
    const char *filt = argc > 4 ? argv[4] : NULL;
    if (getenv("PRIMEPROBE_NOGATE")) PRIME_SETTLED_DIV = (size_t)-1;
    std::vector<double> in(n), w(n);
    /* PRIMEPROBE_DIAG=<shape>: the engine alone on that shape's complex factors, each region sorted at 1 thread, at P, and at P with
       the near-sorted route off; fastest of 5. No OpenMP region has run in this process when these are timed. */
    if (const char *dg = getenv("PRIMEPROBE_DIAG")) {
        gen(dg, in); std::vector<Run> R; Stats st; std::vector<Factor> F;
        if (!scan_runs(in.data(), 0, n, R, n / 256 + 16)) { printf("bailed\n"); return 0; }
        decompose(in.data(), n, R, F, st);
        printf("%s: the engine on each complex factor, ns per element (fastest of 5)\n  %10s %9s %9s %12s\n", dg, "elements", "1 thread", "P threads", "P, no ns");
        double tot[3] = { 0, 0, 0 }; size_t tn = 0;
        for (const Factor &f : F) { if (f.kind != K_ENGINE) continue; size_t m = f.q - f.p; double b3[3];
            for (int c = 0; c < 3; c++) { double best = 1e300;
                for (int k = 0; k < 5; k++) { memcpy(w.data(), in.data() + f.p, m * 8); double t0 = nowms();
                    if (c == 0) logos_sort_f64(w.data(), m, 1); else if (c == 1) logos_sort_f64(w.data(), m, g_P); else c_engP_nons(w.data(), m);
                    best = std::min(best, nowms() - t0); }
                b3[c] = best; tot[c] += best; }
            tn += m; printf("  %10zu %9.2f %9.2f %12.2f\n", m, b3[0] * 1e6 / (double)m, b3[1] * 1e6 / (double)m, b3[2] * 1e6 / (double)m); }
        if (tn) printf("  %10zu %9.2f %9.2f %12.2f   (all of them)\n", tn, tot[0] * 1e6 / (double)tn, tot[1] * 1e6 / (double)tn, tot[2] * 1e6 / (double)tn);
        return 0;
    }
    { std::vector<double> c(1 << 20); RS = 1; for (auto &x : c) x = fr(); double t = nowms(); std::sort(c.begin(), c.end());
      printf("primeprobe: n = %zu, %d pairs per cell, P = %d; calibration std::sort-1M %.1f ms; OMP_WAIT_POLICY=%s\n", n, pairs, g_P, nowms() - t, getenv("OMP_WAIT_POLICY")); }
    printf("cells: t(engine) / t(solver), median [p10 p90]; ABOVE 1 = THE PRIME SOLVER IS FASTER. ns = median nanoseconds per element.\n\n");
    printf("WHAT THE SOLVER FOUND (one thread, own merges; phases are the fastest of 5, in ns per element)\n");
    printf("  %-34s %6s %7s %7s %8s %9s %8s | %6s %6s %6s\n", "shape", "runs", "factors", "simple", "pinned", "largest", "buffer", "scan", "plan", "solve");
    for (const char *s : SHAPES) {
        if (filt && !strstr(s, filt)) continue;
        gen(s, in); Stats best; double bt = 1e300;
        for (int k = 0; k < 5; k++) { memcpy(w.data(), in.data(), n * 8); Stats st; double t0 = nowms(); prime_sort(w.data(), n, 1, &st); double t = nowms() - t0; if (t < bt) { bt = t; best = st; } }
        if (best.bailed == 1) { printf("  %-34s bailed: more than n / 256 runs, the engine sorted it\n", s); continue; }
        if (best.bailed == 2) { printf("  %-34s %6zu %7zu %7zu %7.1f%%  gate: under a quarter settled, the engine sorted it\n", s, best.runs, best.factors, best.simple, 100.0 * (double)best.pinned / (double)n); continue; }
        printf("  %-34s %6zu %7zu %7zu %7.1f%% %8.1f%% %7.2f%% | %6.2f %6.3f %6.2f\n", s, best.runs, best.factors, best.simple, 100.0 * (double)best.pinned / (double)n,
               100.0 * (double)best.largest / (double)n, 100.0 * (double)best.buf / (double)n, best.t_scan * 1e6 / (double)n, best.t_plan * 1e6 / (double)n, best.t_solve * 1e6 / (double)n);
    }
    printf("  pinned: elements never moved. largest: the largest factor. buffer: the scratch the solver asked for, as a share of n elements.\n");
    for (int par = 0; par < 2; par++) {
        if (par && g_P < 2) break;
        if (!par) printf("\n1 THREAD\n  %-34s %7s %9s %9s | %-24s %-24s\n", "shape", "engine", "prime+eng", "prime+mrg", "engine / prime+eng", "engine / prime+mrg");
        else printf("\n%d THREADS\n  %-34s %7s %9s %9s | %-24s %-24s\n", g_P, "shape", "engine", "eng no ns", "prime+mrg", "engine / prime+mrg", "eng, no ns / prime+mrg");
        for (const char *s : SHAPES) {
            if (filt && !strstr(s, filt)) continue;
            gen(s, in);
            cont_t E = par ? c_engP : c_eng1, X = par ? c_engP_nons : c_pe, Y = par ? c_pmP : c_pm;
            if (!verify(E, in, w) || !verify(X, in, w) || !verify(Y, in, w)) { printf("  %-34s VERIFY FAILED\n", s); return 1; }
            double a10, a90, b10, b90, nsE, nsX, nsY, nsE2;
            if (!par) {
                double ra = pairAB(E, X, in, w, pairs, &a10, &a90, &nsE, &nsX), rb = pairAB(E, Y, in, w, pairs, &b10, &b90, &nsE2, &nsY);
                printf("  %-34s %7.2f %9.2f %9.2f | %5.2f [%5.2f %5.2f]      %5.2f [%5.2f %5.2f]\n", s, 0.5 * (nsE + nsE2), nsX, nsY, ra, a10, a90, rb, b10, b90);
            } else {
                double ra = pairAB(E, Y, in, w, pairs, &a10, &a90, &nsE, &nsY), rb = pairAB(X, Y, in, w, pairs, &b10, &b90, &nsX, &nsE2);
                printf("  %-34s %7.2f %9.2f %9.2f | %5.2f [%5.2f %5.2f]      %5.2f [%5.2f %5.2f]\n", s, nsE, nsX, 0.5 * (nsY + nsE2), ra, a10, a90, rb, b10, b90);
            }
            fflush(stdout);
        }
    }
    printf("\nEvery contender's output was checked on every shape (ascending, and the input's bit patterns as a multiset) before it was timed.\n");
    return 0;
}
