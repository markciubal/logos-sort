/* splitter_crux.cpp — can the top-level histogram's bucket lookup be O(1)?
 *
 * The profiler (make profile) showed bucket_of's binary search over the 79
 * sample splitters is the dominant 20-thread phase (~100 ns/element on the
 * reference box, 9x the scatter). Three ways to compute the same bucket id:
 *   (a) the shipped branchless binary search;
 *   (b) a static table built from the splitters alone: 4096 equi-width cells
 *       over [min,max]; a cell crossed by one splitter resolves with one
 *       compare, a cell crossed by several gets a child table over the SPAN OF
 *       THOSE SPLITTERS (not the whole cell), recursively, so clustered data
 *       (bimodal: 40 splitters in 10 units) resolves in depth 2, not 6;
 *   (c) "sample itself": run (a) on the first half while counting cell hits,
 *       then build (b) only for cells the data actually touched, and run the
 *       second half on it. Same lookup as (b); measures what the first half
 *       buys (build cost) and what it cannot (the lookup is fixed by the
 *       splitters, which are already quantiles of the data).
 * Every variant's bucket ids are checked equal to (a) for all n elements.
 * Build: g++ -O3 -std=gnu++17 splitter_crux.cpp -o splitter_crux
 */
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static std::vector<double> gen(const std::string &shape, size_t n) {
    Rng r{7}; std::vector<double> a(n);
    if (shape == "uniform") for (auto &v : a) v = r.f() * 2e9 - 1e9;
    else if (shape == "heavy_tail") for (auto &v : a) v = 1000.0 * std::pow(1.0 - r.f() * 0.999999, -1.0 / 1.2);
    else if (shape == "bimodal") for (auto &v : a) v = ((r.next() & 1) ? 1e9 : -1e9) + r.f() * 10.0;
    else if (shape == "int_wide") for (auto &v : a) v = (double)(int32_t)r.next();
    else if (shape == "clustered") for (auto &v : a) { double s = 0; for (int k = 0; k < 4; k++) s += r.f(); v = (s - 2.0) * 1e6; }
    return a;
}

/* ---- (a) the engine's bucket_of ---- */
static inline int bucket_of(double x, const double *sp, int nsp) {
    const double *p = sp; int len = nsp;
    while (len > 0) { int half = len >> 1; int go = (p[half] <= x); p += go ? half + 1 : 0; len = go ? len - half - 1 : half; }
    return (int)(p - sp);
}

/* ---- (b) static recursive splitter table ---- */
struct Ent { int16_t lo, hi; int32_t child; };            /* buckets possible in this cell: lo..hi; child table if hi-lo > 1 */
struct Node { double lo, scale; int cells; int base; };   /* entries live in one flat array at [base, base+cells) */
struct Table {
    std::vector<Node> nodes; std::vector<Ent> ents; const double *sp; int nsp; int maxdepth = 8, built = 0;
    /* bucket_of semantics: bucket(x) = number of splitters <= x */
    int cnt_le(double x) const { return (int)(std::upper_bound(sp, sp + nsp, x) - sp); }
    int cnt_lt(double x) const { return (int)(std::lower_bound(sp, sp + nsp, x) - sp); }
    /* build a node covering [vlo, vhi) with `cells` cells; returns node index */
    int build(double vlo, double vhi, int cells, int depth, const uint32_t *hits /* optional: skip untouched cells */) {
        if (nodes.size() > 20000) { fprintf(stderr, "Table runaway: depth %d span %g cells %d nodes %zu\n", depth, vhi - vlo, cells, nodes.size()); exit(3); }
        int id = (int)nodes.size(); Node nd; nd.lo = vlo; nd.scale = cells / (vhi - vlo); nd.cells = cells; nd.base = (int)ents.size();
        nodes.push_back(nd); ents.resize(ents.size() + cells); built++;
        for (int c = 0; c < cells; c++) {
            double clo = vlo + c / nd.scale, chi = (c + 1 == cells) ? vhi : vlo + (c + 1) / nd.scale;
            Ent e; e.lo = (int16_t)cnt_le(clo); e.hi = (int16_t)cnt_lt(chi); e.child = -1;
            /* x in [clo, chi): bucket in [e.lo, e.hi]; splitters inside the cell are sp[e.lo .. e.hi-1] */
            if (e.hi - e.lo > 1 && depth < maxdepth && (!hits || hits[c])) {
                double slo = sp[e.lo], shi = sp[e.hi - 1];
                if (shi > slo) { int ch = build(slo, shi, 64, depth + 1, nullptr); e.child = ch; }
            }
            ents[nodes[id].base + c] = e;
        }
        return id;
    }
    inline int lookup(double x) const {
        const Node *nd = &nodes[0];
        for (;;) {
            int c = (int)((x - nd->lo) * nd->scale); if (c < 0) c = 0; else if (c >= nd->cells) c = nd->cells - 1;
            const Ent &e = ents[nd->base + c];
            if (e.lo == e.hi) return e.lo;
            if (e.hi - e.lo == 1) return e.lo + (sp[e.lo] <= x);                 /* one splitter: one compare */
            if (e.child >= 0) {                                                 /* several: child over the splitters' span */
                const Node *ch = &nodes[e.child];
                if (x < ch->lo) return e.lo;                                    /* below the first splitter in the cell */
                if (x >= sp[e.hi - 1]) return e.hi;                             /* at or above the last */
                nd = ch; continue;
            }
            return e.lo + bucket_of(x, sp + e.lo, e.hi - e.lo);                 /* depth cap: range-limited search */
        }
    }
};

/* ---- (d) the same table in KEY space: cells over the order-preserving 64-bit key, so a cell is a
 * slice of one binade and resolution follows log-magnitude — skewed data spreads evenly ---- */
static inline uint64_t total_key(double v) { uint64_t u; memcpy(&u, &v, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
struct KNode { uint64_t klo; int shift; int cells; int base; };
struct KTable {
    std::vector<KNode> nodes; std::vector<Ent> ents; const double *sp; int nsp; std::vector<uint64_t> ksp; int maxdepth = 8;
    int cnt_le(uint64_t k) const { return (int)(std::upper_bound(ksp.begin(), ksp.end(), k) - ksp.begin()); }
    int cnt_lt(uint64_t k) const { return (int)(std::lower_bound(ksp.begin(), ksp.end(), k) - ksp.begin()); }
    static int shift_for(uint64_t span, int cells) { int s = 0; while ((span >> s) >= (uint64_t)cells) s++; return s; }
    int build(uint64_t klo, uint64_t khi, int cells, int depth) {           /* covers [klo, khi] */
        if (nodes.size() > 20000) { fprintf(stderr, "KTable runaway: depth %d span %llu cells %d nodes %zu\n", depth, (unsigned long long)(khi - klo), cells, nodes.size()); exit(3); }
        int id = (int)nodes.size(); KNode nd; nd.klo = klo; nd.shift = shift_for(khi - klo, cells); nd.cells = cells; nd.base = (int)ents.size();
        nodes.push_back(nd); ents.resize(ents.size() + cells);
        for (int c = 0; c < cells; c++) {
            /* cell [clo, chi) in 128-bit so cells past the top of the key range cannot wrap around */
            unsigned __int128 clo128 = (unsigned __int128)klo + ((unsigned __int128)c << nd.shift), chi128 = (unsigned __int128)klo + ((unsigned __int128)(c + 1) << nd.shift);
            Ent e; e.child = -1;
            if (clo128 > khi) { e.lo = e.hi = (int16_t)nsp; ents[nodes[id].base + c] = e; continue; }   /* beyond every value: top bucket */
            uint64_t clo = (uint64_t)clo128; e.lo = (int16_t)cnt_le(clo);
            e.hi = (int16_t)(chi128 > (unsigned __int128)UINT64_MAX ? nsp : cnt_lt((uint64_t)chi128));
            if (e.hi - e.lo > 1 && depth < maxdepth) { uint64_t slo = ksp[e.lo], shi = ksp[e.hi - 1]; if (shi > slo) e.child = build(slo, shi, 64, depth + 1); }
            ents[nodes[id].base + c] = e;
        }
        return id;
    }
    void init(const double *s, int n, double mn, double mx) { sp = s; nsp = n; ksp.resize(n); for (int i = 0; i < n; i++) ksp[i] = total_key(s[i]); nodes.clear(); ents.clear(); build(total_key(mn), total_key(mx), 4096, 0); }
    inline int lookup(double x) const {
        uint64_t k = total_key(x); const KNode *nd = &nodes[0];
        for (;;) {
            uint64_t d = k - nd->klo; int c = (int)(d >> nd->shift); if (k < nd->klo) c = 0; else if (c >= nd->cells) c = nd->cells - 1;
            const Ent &e = ents[nd->base + c];
            if (e.lo == e.hi) return e.lo;
            if (e.hi - e.lo == 1) return e.lo + (ksp[e.lo] <= k);
            if (e.child >= 0) { const KNode *ch = &nodes[e.child]; if (k < ch->klo) return e.lo; if (k >= ksp[e.hi - 1]) return e.hi; nd = ch; continue; }
            return e.lo + bucket_of(x, sp + e.lo, e.hi - e.lo);
        }
    }
};

int main() {
    const size_t N = 8000000; const int NBK = 80, NSP = NBK - 1, OVERSAMPLE = 32, R = 5;
    const char *shapes[] = { "uniform", "int_wide", "heavy_tail", "bimodal", "clustered" };
    printf("splitter lookup crux — n=8M, %d buckets, 1 thread, best of %d; ns per element for the bucket id\n", NBK, R);
    printf("  %-11s %9s %9s %9s %9s %9s   %8s %6s %6s  %-6s %s\n", "shape", "(a)search", "(b)value", "(c)half", "(d)key", "(e)self", "build(b)", "nodes", "depth", "chose", "check");
    for (const char *sh : shapes) {
        auto a = gen(sh, N);
        /* engine's splitters: strided sample, sorted, quantiles */
        size_t m = (size_t)NBK * OVERSAMPLE; std::vector<double> samp(m); size_t step = N / m;
        for (size_t i = 0, q = 0; i < m; i++, q += step) samp[i] = a[q < N ? q : N - 1];
        std::sort(samp.begin(), samp.end());
        std::vector<double> sp(NSP); for (int b = 1; b < NBK; b++) sp[b - 1] = samp[(size_t)b * m / NBK];
        double mn = *std::min_element(a.begin(), a.end()), mx = *std::max_element(a.begin(), a.end());
        std::vector<uint8_t> ref(N), out(N); std::vector<size_t> cnt(NBK);
        auto time = [&](auto f) { double best = 1e300; for (int r = 0; r < R; r++) { std::fill(cnt.begin(), cnt.end(), 0); double t = nowms(); f(); double ms = nowms() - t; if (ms < best) best = ms; } return best * 1e6 / N; };
        /* (a) */
        double ta = time([&] { for (size_t i = 0; i < N; i++) { int b = bucket_of(a[i], sp.data(), NSP); ref[i] = (uint8_t)b; cnt[b]++; } });
        /* (b): static table from splitters */
        Table tb; tb.sp = sp.data(); tb.nsp = NSP; double t0 = nowms(); tb.build(mn, std::nextafter(mx, INFINITY), 4096, 0, nullptr); double buildb = nowms() - t0;
        double tbms = time([&] { for (size_t i = 0; i < N; i++) { int b = tb.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } });
        bool okb = std::equal(ref.begin(), ref.end(), out.begin());
        /* (c): first half with (a) while counting L1 cell hits, then build the table only where hit, second half on it */
        std::vector<uint32_t> hits(4096, 0); double s1 = 4096.0 / (std::nextafter(mx, INFINITY) - mn);
        Table tc; tc.sp = sp.data(); tc.nsp = NSP; double buildc = 0;
        double tcms = time([&] {
            size_t half = N / 2; std::fill(hits.begin(), hits.end(), 0);
            for (size_t i = 0; i < half; i++) { double x = a[i]; int c = (int)((x - mn) * s1); if (c > 4095) c = 4095; hits[c]++; int b = bucket_of(x, sp.data(), NSP); out[i] = (uint8_t)b; cnt[b]++; }
            double t1 = nowms(); tc.nodes.clear(); tc.ents.clear(); tc.built = 0; tc.build(mn, std::nextafter(mx, INFINITY), 4096, 0, hits.data()); buildc = nowms() - t1;
            for (size_t i = half; i < N; i++) { int b = tc.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; }
        });
        bool okc = std::equal(ref.begin(), ref.end(), out.begin());
        /* (d): key-space table */
        KTable td; td.init(sp.data(), NSP, mn, mx);
        double tdms = time([&] { for (size_t i = 0; i < N; i++) { int b = td.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } });
        bool okd = std::equal(ref.begin(), ref.end(), out.begin());
        /* (e): the algorithm samples itself — time (b) and (d) on the first 1/32 each, run the rest on the winner */
        const char *chose = "?";
        double tems = time([&] {
            size_t probe = N / 32, i = 0; double tv, tk;
            { double t = nowms(); for (; i < probe; i++) { int b = tb.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } tv = nowms() - t; }
            { double t = nowms(); for (; i < 2 * probe; i++) { int b = td.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } tk = nowms() - t; }
            if (tv <= tk) { chose = "value"; for (; i < N; i++) { int b = tb.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } }
            else          { chose = "key";   for (; i < N; i++) { int b = td.lookup(a[i]); out[i] = (uint8_t)b; cnt[b]++; } }
        });
        bool oke = std::equal(ref.begin(), ref.end(), out.begin());
        /* depth of (b): longest chain */
        int maxd = 0; { std::vector<int> depth(tb.nodes.size(), 0); for (size_t i = 0; i < tb.nodes.size(); i++) for (int c = 0; c < tb.nodes[i].cells; c++) { int ch = tb.ents[tb.nodes[i].base + c].child; if (ch >= 0) { depth[ch] = depth[i] + 1; if (depth[ch] > maxd) maxd = depth[ch]; } } }
        printf("  %-11s %7.1fns %7.1fns %7.1fns %7.1fns %7.1fns   %6.2fms %6zu %6d  %-6s %s\n", sh, ta, tbms, tcms, tdms, tems, buildb, tb.nodes.size(), maxd + 1, chose, okb && okc && okd && oke ? "ids identical" : "MISMATCH");
        (void)buildc;
    }
    printf("\n(a) is the engine's bucket_of. (c) = first half at (a) speed collecting cell hits, second half on a table built only where hit.\n"
           "(d) = table over the order-preserving key (log-magnitude cells). (e) = time (b) and (d) on 1/32 each, rest on the winner.\n");
    return 0;
}
