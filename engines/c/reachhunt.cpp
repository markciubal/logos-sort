/* reachhunt.cpp -- ceilings for disorder of SHORT REACH (FINDINGS 3af, experiments/SHORT_REACH.md).
 *
 * The knot route priced how many strands cross. This prices how FAR the disorder reaches. Four views of an array
 * whose elements all sit near their final place, each with the sort a route could run if it knew:
 *   cuts     (prime factors of a composite knot): cut wherever everything before is no larger than everything after;
 *            each factor sorts alone, in place, in cache. No scatter, no merge, and the test is exact.
 *   blocks   (a braid whose crossings are all short): sort index blocks in place with the engine's own leaf, then merge
 *            only the few elements near each boundary. Whatever crosses a boundary rightward is matched by an equal
 *            count crossing leftward, so nothing else moves.
 *   links    (unsorted chunks whose value ranges do not overlap): one copy puts every chunk where it belongs, then the
 *            blocks form finishes it.
 * Every method is a CEILING: it is told the structure (the cut positions, the displacement bound, the segments), or it
 * finds it blind where that is cheap, so its time bounds what an engine route could reach. The zoo of twenty in
 * chaoshunt.cpp is included unchanged (zroute's held-out test depends on it); the shapes added here live in this file.
 *
 * Measurement: every output is checked bit-exact against std::sort first. Then logos and one method run ADJACENTLY on
 * the same input, order alternating, and the row reports the median of t(logos)/t(method) with p10/p90 (the paired
 * protocol of bench_common.h) plus the minimum of each side. Ratios within a row only.
 *
 * PREDICTIONS, written 2026-09-20 before this file existed (baseline that day: event_ts_latency 119 ms / 11.8 ms,
 * window_shuffle 122 / 11.1, uniform 115 / 15.0 at 1 / 14 threads, n = 2^23):
 *   P1 blocks + repair, event_ts_latency   1.2-1.5x at 1 thread, 1.2-1.6x at 14. (The table given to the user said 1.5-2.5x.)
 *   P2 cuts told, window_shuffle           2-3x at 1 thread, 1.3-1.8x at 14.
 *   P3 cuts found blind                    +8-12 ms at 1 thread, +1.5-2.5 ms at 14 over the told form.
 *   P4 coarse_key_groups, cuts             1.5-2x at 1 thread, 1.2-1.5x at 14 (components are larger than a window).
 *   P5 regime_switch, links                1.2-1.5x at 1 thread, 0.8-1.1x at 14 (the tie every copy-based route has there).
 *   P6 latency + 0.01% far strays          boundary repair declines every time, the block sort is wasted: 0.5-0.7x.
 *   P7 uniform (guard), blind cuts         <= 3% at 1 thread, 5-8% at 14: one read pass that finds nothing.
 * GRADED (12 pairs, n = 2^23; full table in FINDINGS 3af):
 *   P1 right at one thread (1.28-1.31), low end at 14 (1.18-1.22).   P2 WRONG both ways: 1.64 and 0.30 (ranges of 2048 fall
 *   under the leaf's flash threshold and a parallel sort sends them to a scalar quicksort; as 33k blocks the same data is 1.24).
 *   P3 WRONG: 35 ms and 9 ms (a suffix minimum and a running maximum are dependency chains, not loads).   P4 a little low:
 *   1.43 / 1.11, and the blind form was declined by this file's own gate (cuts in an eighth of the chunks; groups of 43k put
 *   one in a fourteenth).   P5 WRONG: 2.0-2.1 / 1.3-1.5, because the engine runs three value bands at twice its uniform time:
 *   a ceiling measures the baseline's weakness too.   P6 right at one thread (0.60), worse at 14 (0.41).   P7 right at one
 *   thread (0.98), wrong at 14 (0.89).
 *   Blocks + seams became reach_try_f64 (make reachbench). Cuts are a block boundary with an empty seam: not built. Links: not
 *   built. groups_of_runs was added when a late decline of blocks + seams handed the engine sorted blocks and it took 70 ms
 *   at 14 threads against 12 for the cuts: the near-sorted gates (make nsgatebench).
 *
 * Run: make reachhunt && ./reachhunt [pairs=12] [shape,shape,...] [n=8388608]
 * No engine code is changed by this file. */
#define main chaoshunt_unused_main
#include "chaoshunt.cpp"
#undef main
#include <immintrin.h>
#include <limits>

/* ---- what the generator tells the informed sorts, and what the last call did ---- */
static std::vector<size_t> g_segs;      /* told: component or segment starts, with n at the end */
static size_t g_B = 0;                  /* block size for blocks + repair */
static struct outcome_t { size_t comps, maxcomp, zone; int declined; double t_detect, t_sort, t_repair, t_move; } g_out;

/* ---- min and max of a range: AVX2 when the CPU has it (a bench is allowed to ask) ---- */
__attribute__((target("avx2"))) static void minmax_avx2(const double *p, size_t len, double *mn, double *mx) {
    __m256d vmn = _mm256_set1_pd(p[0]), vmx = vmn; size_t i = 0;
    for (; i + 4 <= len; i += 4) { __m256d v = _mm256_loadu_pd(p + i); vmn = _mm256_min_pd(vmn, v); vmx = _mm256_max_pd(vmx, v); }
    double a4[4], b4[4]; _mm256_storeu_pd(a4, vmn); _mm256_storeu_pd(b4, vmx);
    double lo = a4[0], hi = b4[0]; for (int k = 1; k < 4; k++) { if (a4[k] < lo) lo = a4[k]; if (b4[k] > hi) hi = b4[k]; }
    for (; i < len; i++) { if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; }
    *mn = lo; *mx = hi;
}
static void minmax_scalar(const double *p, size_t len, double *mn, double *mx) {
    double lo = p[0], hi = p[0]; for (size_t i = 1; i < len; i++) { if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; } *mn = lo; *mx = hi;
}
static int g_avx2 = -1;
static void minmax(const double *p, size_t len, double *mn, double *mx) {
    if (g_avx2 < 0) g_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    if (g_avx2) minmax_avx2(p, len, mn, mx); else minmax_scalar(p, len, mn, mx);
}
/* P threads pulling indices 0..count-1 from one atomic: the ranges here are uneven */
template <class F> static void par_each(int P, size_t count, F f) {
    std::atomic<size_t> next{0};
    par_for(P, [&](int, int) { size_t k; while ((k = next.fetch_add(1, std::memory_order_relaxed)) < count) f(k); });
}

/* ---- sort index ranges in place with the engine's own leaf. A range too large for one core at P > 1 waits and gets
 * the whole parallel engine afterwards, which is what a route would do with a factor it cannot split. ---- */
static void sort_ranges(double *a, const std::vector<size_t> &st, int P) {
    int nb = (int)st.size() - 1; if (nb < 1) return;
    std::vector<unsigned char> done((size_t)nb, 0);
    size_t total = st.back() - st[0], big = P > 1 ? total / (2 * (size_t)P) : (size_t)-1, mc = 0;
    if (P > 1 && big < 262144) big = 262144;
    for (int b = 0; b < nb; b++) { size_t len = st[b + 1] - st[b]; if (len > mc) mc = len; if (len > big) done[b] = 2; }
    g_out.comps = (size_t)nb; g_out.maxcomp = mc;
    logos_sort_buckets_f64(a, st.data(), nb, done.data(), P);
    for (int b = 0; b < nb; b++) if (done[b] == 2) logos_sort_f64(a + st[b], st[b + 1] - st[b], P);
}

/* ---- cuts, told ---- */
static void m_cuts_told(double *a, size_t n, int P) { (void)n; g_out = outcome_t{}; double s = now(); sort_ranges(a, g_segs, P); g_out.t_sort = now() - s; }

/* ---- cuts, blind. Level 1: min and max per chunk (one vectorised read pass, parallel). Level 2: prefix max and suffix
 * min over the chunk summaries say exactly which chunk STARTS are cuts and which chunks can hold a cut INSIDE (nothing
 * before the chunk exceeds anything after it); only those are scanned. The chunk is 3000 elements so that no window in
 * the zoo divides it: alignment must not do the work. Cuts closer than MINC to the previous one are dropped. ---- */
static const size_t CH = 3000, MINC = 256;
static int find_cuts(const double *a, size_t n, int P, std::vector<size_t> &st) {
    size_t K = (n + CH - 1) / CH; std::vector<double> mn(K), mx(K);
    par_for(P, [&](int t, int PP) { size_t c0 = K * t / PP, c1 = K * (t + 1) / PP;
        for (size_t c = c0; c < c1; c++) { size_t lo = c * CH; minmax(a + lo, std::min(CH, n - lo), &mn[c], &mx[c]); } });
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> PM(K + 1), SM(K + 1);            /* PM[c] = max of the chunks before c, SM[c] = min of the chunks from c on */
    PM[0] = -INF; for (size_t c = 0; c < K; c++) PM[c + 1] = std::max(PM[c], mx[c]);
    SM[K] = INF; for (size_t c = K; c-- > 0;) SM[c] = std::min(SM[c + 1], mn[c]);
    size_t open = 0; for (size_t c = 0; c < K; c++) if (PM[c] <= SM[c + 1]) open++;
    if (open * 8 < K) { st.clear(); st.push_back(0); st.push_back(n); return 0; }     /* under an eighth of the chunks could hold a cut: not this structure */
    std::vector<std::vector<size_t>> cuts((size_t)P);
    par_for(P, [&](int t, int PP) { size_t c0 = K * t / PP, c1 = K * (t + 1) / PP; std::vector<double> sm(CH + 1); auto &out = cuts[(size_t)t];
        size_t last = 0;
        for (size_t c = c0; c < c1; c++) {
            size_t lo = c * CH, len = std::min(CH, n - lo);
            if (c > 0 && PM[c] <= SM[c] && lo - last >= MINC) { out.push_back(lo); last = lo; }
            if (!(PM[c] <= SM[c + 1])) continue;
            sm[len] = SM[c + 1]; for (size_t j = len; j-- > 0;) sm[j] = std::min(sm[j + 1], a[lo + j]);
            double run = PM[c];
            for (size_t j = 1; j < len; j++) { if (a[lo + j - 1] > run) run = a[lo + j - 1]; if (run <= sm[j] && lo + j - last >= MINC) { out.push_back(lo + j); last = lo + j; } }
        } });
    st.clear(); st.push_back(0);
    for (auto &v : cuts) for (size_t p : v) if (p - st.back() >= MINC) st.push_back(p);
    if (st.size() > 1 && n - st.back() < MINC) st.pop_back();
    st.push_back(n);
    return 1;
}
static void m_cuts_blind(double *a, size_t n, int P) {
    g_out = outcome_t{}; std::vector<size_t> st; double s = now(); int ok = find_cuts(a, n, P, st); g_out.t_detect = now() - s;
    if (!ok) { g_out.declined = 1; s = now(); logos_sort_f64(a, n, P); g_out.t_sort = now() - s; return; }
    s = now(); sort_ranges(a, st, P); g_out.t_sort = now() - s;
}

/* ---- boundary repair over sorted blocks. For the boundary between L and R: y = the elements of L greater than R[0],
 * x = the elements of R less than the last of L, both by binary search. Merging those two pieces in place sorts L u R.
 * All boundaries are independent, and the result is globally sorted, when every block keeps a NON-EMPTY untouched
 * middle: x(k) + y(k+1) < len(k). (With an empty middle the zone of boundary k can end above the start of the zone of
 * boundary k+1: a stray at the end of block k-1 larger than all of block k does exactly that.) Otherwise: decline. ---- */
static int repair_boundaries(double *a, const std::vector<size_t> &st, int P) {
    int nb = (int)st.size() - 1; if (nb < 2) return 1;
    std::vector<size_t> x((size_t)nb + 1, 0), y((size_t)nb + 1, 0);
    for (int k = 1; k < nb; k++) {                        /* a few hundred binary searches: serial, it is microseconds */
        const double *L = a + st[k - 1], *Le = a + st[k], *R = Le, *Re = a + st[k + 1];
        if (!(R[0] < Le[-1])) continue;
        y[k] = (size_t)(Le - std::upper_bound(L, Le, R[0]));
        x[k] = (size_t)(std::lower_bound(R, Re, Le[-1]) - R);
    }
    size_t ymax = 0, zone = 0;
    for (int k = 0; k < nb; k++) { if (x[k] + y[k + 1] >= st[k + 1] - st[k]) return 0; if (y[k] > ymax) ymax = y[k]; zone += x[k] + y[k]; }
    g_out.zone = zone;
    if (!zone) return 1;
    std::vector<int> live; for (int k = 1; k < nb; k++) if (y[k]) live.push_back(k);
    par_for(std::min(P, (int)live.size()), [&](int t, int PP) { std::vector<double> tmp(ymax);
        for (size_t q = (size_t)t; q < live.size(); q += (size_t)PP) { int k = live[q];
            double *zl = a + st[k] - y[k]; memcpy(tmp.data(), zl, y[k] * 8);
            const double *p = tmp.data(), *pe = p + y[k], *r = a + st[k], *re = r + x[k]; double *o = zl;
            while (p < pe && r < re) { if (*r < *p) *o++ = *r++; else *o++ = *p++; }
            while (p < pe) *o++ = *p++;                   /* what is left of R is already in place: o has caught up with r */
        } });
    return 1;
}
static void blocks_of(size_t lo, size_t hi, size_t B, std::vector<size_t> &st) { for (size_t p = lo; p < hi; p += B) st.push_back(p); }
static void m_blocks(double *a, size_t n, int P) {
    g_out = outcome_t{}; std::vector<size_t> st; blocks_of(0, n, g_B, st);
    if (st.size() > 1 && n - st.back() < g_B / 2) st.pop_back();          /* no runt at the end: its middle would be the first to vanish */
    st.push_back(n);
    double s = now(); sort_ranges(a, st, P); g_out.t_sort = now() - s;
    s = now(); int ok = repair_boundaries(a, st, P); g_out.t_repair = now() - s;
    if (!ok) { g_out.declined = 1; s = now(); logos_sort_f64(a, n, P); g_out.t_repair += now() - s; }
}

/* ---- links, told the segments: min and max of each unsorted segment; if the ranges are disjoint one copy puts each
 * segment at its final offset, and the blocks form finishes it there (the segments are locally disordered inside). ---- */
static void m_links_told(double *a, size_t n, int P) {
    g_out = outcome_t{}; const std::vector<size_t> &sg = g_segs; size_t ns = sg.size() - 1; double *buf = g_buf;
    std::vector<double> mn(ns), mx(ns); double s = now();
    par_each(P, ns, [&](size_t k) { minmax(a + sg[k], sg[k + 1] - sg[k], &mn[k], &mx[k]); });
    std::vector<size_t> ord(ns); for (size_t k = 0; k < ns; k++) ord[k] = k;
    std::sort(ord.begin(), ord.end(), [&](size_t p, size_t q) { return mn[p] < mn[q]; });
    int ok = 1; for (size_t k = 0; k + 1 < ns; k++) if (mx[ord[k]] > mn[ord[k + 1]]) { ok = 0; break; }
    g_out.t_detect = now() - s;
    if (!ok) { g_out.declined = 1; s = now(); logos_sort_f64(a, n, P); g_out.t_sort = now() - s; return; }
    std::vector<size_t> dst(ns), st; size_t off = 0;
    for (size_t k = 0; k < ns; k++) { size_t g = ord[k], len = sg[g + 1] - sg[g]; dst[g] = off; size_t before = st.size(); blocks_of(off, off + len, g_B, st);
        if (st.size() - before > 1 && off + len - st.back() < g_B / 2) st.pop_back(); off += len; }
    st.push_back(n);
    s = now(); par_each(P, ns, [&](size_t g) { memcpy(buf + dst[g], a + sg[g], (sg[g + 1] - sg[g]) * 8); }); g_out.t_move = now() - s;
    s = now(); sort_ranges(buf, st, P); g_out.t_sort = now() - s;
    s = now(); ok = repair_boundaries(buf, st, P); if (!ok) { g_out.declined = 1; logos_sort_f64(buf, n, P); } g_out.t_repair = now() - s;
    s = now(); par_copy(a, buf, n, P); g_out.t_move += now() - s;
}

/* ---- the shapes ---- */
enum { R_UNIFORM, R_LATENCY, R_WSHUF, R_GROUPS, R_REGIME, R_STRAYS, R_RUNS16, R_GRUNS, R_N };
static const char *RNM[R_N] = { "uniform", "event_ts_latency", "window_shuffle", "coarse_key_groups", "regime_switch", "latency+strays", "sorted_runs_16", "groups_of_runs" };
static const char *RHID[R_N] = { "none (guard)", "displacement <= 4096 (zoo shape 3)", "2048-element windows shuffled inside (zoo shape 4)",
    "sorted by a coarse key, random inside; groups of 2k..200k elements", "3 value bands, dwell 20k..400k, reach 512 inside a dwell",
    "event_ts_latency with 0.01% of elements swapped to anywhere", "16 sorted runs (guard: long reach)",
    "coarse_key_groups with every 33421-element block sorted: what a late decline of blocks+repair hands back to the engine" };
static void rgen(double *a, size_t n, int d) {
    size_t i; g_segs.clear();
    switch (d) {
    case R_UNIFORM: gen(a, n, S_UNIFORM); break;
    case R_LATENCY: gen(a, n, S_LATENCY); break;
    case R_WSHUF:   gen(a, n, S_WSHUF); for (size_t p = 0; p < n; p += 2048) g_segs.push_back(p); g_segs.push_back(n); break;
    case R_GROUPS:  { size_t pos = 0; double key = 0;                       /* think: events filed per minute, the minutes in order, volume uneven */
                      while (pos < n) { size_t len = (size_t)exp(log(2000.0) + fr() * log(100.0)); if (len > n - pos) len = n - pos;
                        g_segs.push_back(pos); for (i = 0; i < len; i++) a[pos + i] = key + fr(); key += 1.0; pos += len; }
                      g_segs.push_back(n); break; }
    case R_REGIME:  { size_t pos = 0; int r = 0;                            /* a meter that reports from one of three sources; each source has its own offset */
                      while (pos < n) { size_t len = (size_t)exp(log(20000.0) + fr() * log(20.0)); if (len > n - pos) len = n - pos;
                        g_segs.push_back(pos); double base = (double)r * 1099511627776.0;
                        for (i = 0; i < len; i++) a[pos + i] = base + (double)(pos + i) + fr() * 512.0;
                        pos += len; r = (r + 1 + (int)(nr() % 2)) % 3; }
                      g_segs.push_back(n); break; }
    case R_STRAYS:  gen(a, n, S_LATENCY); for (i = 0; i < n / 10000; i++) { size_t x = nr() % n, y = nr() % n; std::swap(a[x], a[y]); } break;
    case R_RUNS16:  gen(a, n, S_RUNS16); break;
    case R_GRUNS:   rgen(a, n, R_GROUPS); for (size_t p = 0; p < n; p += 33421) std::sort(a + p, a + std::min(n, p + 33421)); break;
    }
}
/* which methods run on which shape, and with what block size */
enum { M_CUTS_TOLD, M_CUTS_BLIND, M_BLOCKS, M_BLOCKS_AUTO, M_LINKS, M_N };
static const char *MNM[M_N] = { "cuts told", "cuts blind", "blocks+repair (B from w)", "blocks+repair (B = n/251)", "links told + blocks" };
static bench_sort_fn MFN[M_N] = { m_cuts_told, m_cuts_blind, m_blocks, m_blocks, m_links_told };
static size_t block_for(int d, int m, size_t n) {
    size_t w = d == R_LATENCY || d == R_STRAYS ? 4096 : d == R_WSHUF ? 2048 : d == R_REGIME ? 512 : d == R_GROUPS ? 4096 : 4096;
    if (m == M_BLOCKS_AUTO) return (n + 250) / 251;
    return 8 * w + w / 3;
}
static int runs_on(int d, int m) {
    switch (d) {
    case R_UNIFORM: return m == M_CUTS_BLIND || m == M_BLOCKS_AUTO;
    case R_LATENCY: return m == M_CUTS_BLIND || m == M_BLOCKS || m == M_BLOCKS_AUTO;
    case R_WSHUF:   return m == M_CUTS_TOLD || m == M_CUTS_BLIND || m == M_BLOCKS || m == M_BLOCKS_AUTO;
    case R_GROUPS:  return m == M_CUTS_TOLD || m == M_CUTS_BLIND || m == M_BLOCKS_AUTO;
    case R_REGIME:  return m == M_LINKS || m == M_CUTS_BLIND || m == M_BLOCKS_AUTO;
    case R_STRAYS:  return m == M_BLOCKS || m == M_CUTS_BLIND;
    case R_RUNS16:  return m == M_CUTS_BLIND || m == M_BLOCKS_AUTO;
    case R_GRUNS:   return m == M_CUTS_TOLD;
    }
    return 0;
}
static int cmp_d(const void *p, const void *q) { double x = *(const double *)p, y = *(const double *)q; return x < y ? -1 : x > y; }
int main(int argc, char **argv) {
    int P = bench_threads(logos_num_cpus()); int pairs = argc > 1 ? atoi(argv[1]) : 12; if (pairs < 3) pairs = 3;
    const size_t N = argc > 3 ? (size_t)atol(argv[3]) : (size_t)1 << 23;
    int want[R_N]; for (int d = 0; d < R_N; d++) want[d] = argc <= 2 || !argv[2][0] || argv[2][0] == '-';
    if (argc > 2 && argv[2][0] && argv[2][0] != '-') for (const char *s = argv[2]; *s; ) { int v = atoi(s); if (v >= 0 && v < R_N) want[v] = 1; while (*s && *s != ',') s++; if (*s) s++; }
    g_dump = getenv("CHAOS_PROF") != NULL;
    std::vector<double> base(N), a(N), ref(N), bufv(N); g_buf = bufv.data();
    printf("reachhunt: ceilings for disorder of short reach. n=%zu, %d pairs per cell, threads 1 and %d; ratio = t(logos)/t(method), >1 means the method wins\n", N, pairs, P);
    { double bm = 1e300, bs = 1e300; for (size_t i = 0; i < N; i++) base[i] = fr();
      for (int r = 0; r < 5; r++) { double s = now(); memcpy(a.data(), base.data(), N * 8); double x = now() - s; if (x < bm) bm = x; }
      size_t m1 = std::min(N, (size_t)1000000);
      for (int r = 0; r < 5; r++) { memcpy(a.data(), base.data(), m1 * 8); double s = now(); logos_sort_f64(a.data(), m1, 1); double x = now() - s; if (x < bs) bs = x; }
      printf("calibration: memcpy %zu MB %.2f ms, logos 1M uniform 1T %.1f ms (idle reference ~2.9 / ~13 at 64 MB)\n\n", N * 8 >> 20, bm, bs); }
    printf("  %-18s %3s  %-26s %8s %8s  %-22s %s\n", "shape", "thr", "method", "logos", "method", "median ratio [p10,p90]", "what the method did");
    std::vector<double> rat((size_t)pairs);
    for (int d = 0; d < R_N; d++) {
        if (!want[d]) continue;
        RS = 1007 + (uint64_t)d; rgen(base.data(), N, d); std::vector<size_t> segs = g_segs; ref = base; std::sort(ref.begin(), ref.end());
        int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; char rt[96]; route_of(a.data(), base.data(), N, t, rt, sizeof rt);
            memcpy(a.data(), base.data(), N * 8); logos_sort_f64(a.data(), N, t); int okl = !memcmp(a.data(), ref.data(), N * 8);
            printf("  %-18s %3d  engine route: %s%s\n", RNM[d], t, rt, okl ? "" : "   LOGOS MISMATCH");
            for (int m = 0; m < M_N; m++) {
                if (!runs_on(d, m)) continue;
                g_segs = segs; g_B = block_for(d, m, N); bench_sort_fn F = MFN[m];
                memcpy(a.data(), base.data(), N * 8); F(a.data(), N, t); int ok = !memcmp(a.data(), ref.data(), N * 8); outcome_t first = g_out;
                double tl = 1e300, tm = 1e300;
                for (int r = 0; r < pairs; r++) { double s, x, y;
                    if (r & 1) { memcpy(a.data(), base.data(), N * 8); s = now(); F(a.data(), N, t); y = now() - s; memcpy(a.data(), base.data(), N * 8); s = now(); logos_sort_f64(a.data(), N, t); x = now() - s; }
                    else       { memcpy(a.data(), base.data(), N * 8); s = now(); logos_sort_f64(a.data(), N, t); x = now() - s; memcpy(a.data(), base.data(), N * 8); s = now(); F(a.data(), N, t); y = now() - s; }
                    if (x < tl) tl = x; if (y < tm) tm = y; rat[(size_t)r] = y > 0 ? x / y : 1.0; }
                qsort(rat.data(), (size_t)pairs, sizeof(double), cmp_d);
                double med = (pairs & 1) ? rat[(size_t)pairs / 2] : 0.5 * (rat[(size_t)pairs / 2 - 1] + rat[(size_t)pairs / 2]);
                char note[200]; outcome_t o = g_out; (void)first;
                snprintf(note, sizeof note, "%s%zu ranges, max %zu, zone %.2f%%; detect %.1f move %.1f sort %.1f repair %.1f ms", o.declined ? "DECLINED; " : "", o.comps, o.maxcomp,
                         100.0 * (double)o.zone / (double)N, o.t_detect, o.t_move, o.t_sort, o.t_repair);
                printf("  %-18s %3d  %-26s %6.1fms %6.1fms  %5.2fx [%4.2f,%4.2f]      %s%s\n", "", t, MNM[m], tl, tm, med, rat[(size_t)(0.10 * pairs)], rat[(size_t)(0.90 * pairs)], note, ok ? "" : "   METHOD MISMATCH");
                fflush(stdout);
            }
        }
    }
    printf("\nhidden order per shape:\n"); for (int d = 0; d < R_N; d++) printf("  %-18s %s\n", RNM[d], RHID[d]);
    return 0;
}
