// strceil: told ceilings for IDEAS_LOG 21 (c) and (d), one thread.
//   (c) count strings that repeat: intern by hash, sort the distinct strings, place the pointers by group -- against logos_sort_strings_mt
//   (d) pack the alphabet: the same strings re-coded two characters to a byte (alphabets of <= 15 symbols) sorted by the engine -- against the originals
// ./strceil [n] [reps]. Every told output is checked AS STRINGS against std::sort before timing.
#include "bench_strings.h"
#include <chrono>
extern "C" {
#include "logos_psort.h"
}
static double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- (c) intern, count, sort the distinct, place ----
static inline uint64_t hstr(const char *s, size_t *len) {
    uint64_t h = 0x9E3779B97F4A7C15ULL; const unsigned char *p = (const unsigned char *)s; size_t l = 0;
    for (;;) { uint64_t w = 0; size_t k = 0; while (k < 8 && p[l + k]) { w |= (uint64_t)p[l + k] << (8 * k); k++; }
               h = (h ^ w) * 0xFF51AFD7ED558CCDULL; h ^= h >> 32; l += k; if (k < 8) break; }
    *len = l; return h;
}
struct Slot { const char *rep; uint32_t gid; uint32_t len; };
static bool intern_sort(const char **a, size_t n, std::vector<const char *> &out, size_t *distinct) {
    size_t cap = 1 << 16; std::vector<Slot> tab(cap, Slot{nullptr, 0, 0}); std::vector<uint32_t> gid(n); std::vector<const char *> reps; std::vector<uint32_t> cnt;
    for (size_t i = 0; i < n; i++) {
        size_t len; uint64_t h = hstr(a[i], &len); size_t q = h & (cap - 1);
        for (;;) { Slot &s = tab[q];
            if (!s.rep) { s.rep = a[i]; s.gid = (uint32_t)reps.size(); s.len = (uint32_t)len; reps.push_back(a[i]); cnt.push_back(0); break; }
            if (s.len == len && !memcmp(s.rep, a[i], len)) break;
            q = (q + 1) & (cap - 1); }
        gid[i] = tab[q].gid; cnt[gid[i]]++;
        if (reps.size() * 2 > cap) {                                   // too many distinct for this table: grow (rehash the representatives)
            if (reps.size() > n / 8) return false;                    // not a repeating column: decline
            cap <<= 2; std::vector<Slot> t2(cap, Slot{nullptr, 0, 0});
            for (size_t g = 0; g < reps.size(); g++) { size_t l2; uint64_t h2 = hstr(reps[g], &l2); size_t q2 = h2 & (cap - 1); while (t2[q2].rep) q2 = (q2 + 1) & (cap - 1); t2[q2] = Slot{reps[g], (uint32_t)g, (uint32_t)l2}; }
            tab.swap(t2); }
    }
    size_t d = reps.size(); *distinct = d;
    std::vector<const char *> sr(reps); logos_sort_strings_mt(sr.data(), d, 1);
    std::vector<size_t> start(d);                                      // start of each group in the output, by gid
    { size_t at = 0; for (size_t r = 0; r < d; r++) { size_t len; uint64_t h = hstr(sr[r], &len); size_t q = h & (cap - 1); while (!(tab[q].len == len && !memcmp(tab[q].rep, sr[r], len))) q = (q + 1) & (cap - 1); uint32_t g = tab[q].gid; start[g] = at; at += cnt[g]; } }
    out.resize(n); for (size_t i = 0; i < n; i++) out[start[gid[i]]++] = a[i];
    return true;
}
// ---- (d) two characters to a byte: an order-preserving 4-bit code per symbol, 0 kept for the terminator ----
static bool pack_set(const std::vector<const char *> &p, std::vector<char> &buf, std::vector<const char *> &q) {
    bool seen[256] = {false}; for (const char *s : p) for (const unsigned char *c = (const unsigned char *)s; *c; c++) seen[*c] = true;
    int code[256], k = 0; for (int c = 1; c < 256; c++) if (seen[c]) code[c] = ++k; if (k > 15) return false;
    size_t tot = 0; for (const char *s : p) tot += strlen(s) / 2 + 2; buf.resize(tot); q.resize(p.size()); size_t at = 0;
    for (size_t i = 0; i < p.size(); i++) { q[i] = buf.data() + at; const unsigned char *c = (const unsigned char *)p[i];
        while (*c) { int hi = code[*c++], lo = *c ? code[*c++] : 0; buf[at++] = (char)(hi << 4 | lo); } buf[at++] = 0; }
    return true;
}
int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atof(argv[1]) : 2000000; int reps = argc > 2 ? atoi(argv[2]) : 7;
    printf("# strceil: n = %zu, one thread, min of %d alternating reps, ms. told = the idea's ceiling; ratio = t(logos) / t(told), above 1 = the idea is faster\n", n, reps);
    const char *shapes[] = { "words", "emails", "urls", "paths", "logs", "uuids", "dates", "ids" };
    for (const char *sh : shapes) {
        if (argc > 3 && strcmp(argv[3], sh)) continue;
        StrSet S = gen_strings(sh, n); std::vector<const char *> base(S.p), ref(S.p), w(n), out;
        std::sort(ref.begin(), ref.end(), str_less);
        // (c)
        size_t d = 0; bool ok = intern_sort(base.data(), n, out, &d); double tl = 1e300, tt = 1e300;
        if (ok && !str_same(out, ref)) { printf("%-7s intern: WRONG OUTPUT\n", sh); return 1; }
        for (int r = 0; r < reps; r++) {
            w = base; double t0 = now(); logos_sort_strings_mt(w.data(), n, 1); double t1 = now() - t0; if (t1 < tl) tl = t1;
            if (ok) { t0 = now(); intern_sort(base.data(), n, out, &d); t1 = now() - t0; if (t1 < tt) tt = t1; }
        }
        if (ok) printf("%-7s (c) intern: %8zu distinct   logos %8.1f   told %8.1f   ratio %.2f\n", sh, d, tl, tt, tl / tt);
        else    printf("%-7s (c) intern: declined (over n/8 distinct)   logos %8.1f\n", sh, tl);
        // (d)
        std::vector<char> pb; std::vector<const char *> pq;
        if (pack_set(base, pb, pq)) {
            double tp = 1e300, ts = 1e300;
            for (int r = 0; r < reps; r++) { std::vector<char> b2; std::vector<const char *> q2; double t0 = now(); pack_set(base, b2, q2); double t1 = now() - t0; if (t1 < tp) tp = t1;
                                             w = pq; t0 = now(); logos_sort_strings_mt(w.data(), n, 1); t1 = now() - t0; if (t1 < ts) ts = t1; }
            printf("%-7s (d) packed alphabet: sort of the packed strings %8.1f (ratio %.2f), with a full re-coding pass %8.1f (ratio %.2f)\n", sh, ts, tl / ts, ts + tp, tl / (ts + tp));
        } else printf("%-7s (d) packed alphabet: over 15 symbols, not packed two to a byte\n", sh);
        fflush(stdout);
    }
    return 0;
}
