// routes.cpp -- ROUTE EQUIVALENCE: does the Rust port (src/port, PORTING.md) take the same path as the C engine?
//
// For every shape, size and thread count the same input is sorted by the C engine and by the Rust port, several times
// each (twice at one thread, ROUTES_REPS = 6 times above it). Each engine keeps the C's route witnesses
// (logos_twist_last(k), logos_reach_last(), logos_mirror_count()). At one thread they must be EQUAL. With threads some
// are counts that depend on timing (which worker claimed which chunk or bucket), so a witness is different only when
// the range Rust shows does not meet the range C shows; one that varies inside either engine is listed as such.
//
//   routes [n list] [P list] [shape filter]        e.g.  routes 100000,1000003,8388608 1,4,14 block
//   ROUTES_VERBOSE=1 prints every cell's witnesses, not only the differing ones.
#define main parity_main_unused
#include "parity.cpp"
#undef main

extern "C" {
size_t logos_twist_last(int which);
int logos_reach_last(void);
size_t logos_mirror_count(int reset);
void logos_rsport_sort_f64(double *a, size_t n, int nthreads);
size_t logos_rsport_twist_last(int which);
int logos_rsport_reach_last(void);
size_t logos_rsport_mirror_count(int reset);
}

// the f64 sort's witnesses (see logos_twist_last in logos_psort.c); 100 = reach_last, 101 = mirror_count
static const int KEYS[] = { 0, 1, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 16, 17, 18, 20, 22, 23, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, -11, -12, 100, 101 };
static const int NKEYS = (int)(sizeof KEYS / sizeof KEYS[0]);
static const char *key_name(int k) {
    switch (k) {
    case 0: return "twist(knot)"; case 1: return "twist(reloc)"; case 4: return "kinks"; case 5: return "gallops"; case 6: return "dense";
    case 7: return "loop-back"; case 8: return "wave"; case 9: return "seam"; case 10: return "strays"; case 12: return "one-strand";
    case 13: return "vec-merges"; case 15: return "fused-parts"; case 16: return "fuse-declines"; case 17: return "lcvec-elems";
    case 18: return "mode-grid"; case 20: return "thin"; case 22: return "f32"; case 23: return "latvec"; case 26: return "halves";
    case 28: return "rle"; case 29: return "cuts"; case 30: return "lowdisc"; case 31: return "sf-windows"; case 32: return "runmerge-vec";
    case 33: return "dominant"; case 34: return "half-place"; case 35: return "hf-pooled"; case 36: return "lat1"; case 37: return "lat-spilled";
    case 38: return "ns-strand"; case 39: return "boost"; case 40: return "mc-capped"; case 41: return "hf-heavy"; case -11: return "tail-kind";
    case -12: return "head-kind"; case 100: return "reach-last"; case 101: return "mirror";
    }
    return "?";
}
static void read_c(size_t *w) {
    for (int i = 0; i < NKEYS; i++) {
        int k = KEYS[i];
        if (k == 100) w[i] = (size_t)logos_reach_last();
        else if (k == 101) w[i] = logos_mirror_count(1);
        else w[i] = logos_twist_last(k);
    }
    logos_twist_last(-1);
}
static void read_r(size_t *w) {
    for (int i = 0; i < NKEYS; i++) {
        int k = KEYS[i];
        if (k == 100) w[i] = (size_t)logos_rsport_reach_last();
        else if (k == 101) w[i] = logos_rsport_mirror_count(1);
        else w[i] = logos_rsport_twist_last(k);
    }
    logos_rsport_twist_last(-1);
}

// shapes beyond parity.cpp's, each built for one route of the C engine
static const char *MORE[] = { "dense_perm", "dense_16n", "rle8", "streams4", "streams60", "ring", "ring_noisy", "mirror_reach", "mirror_prefix", "head_5",
    "wave32", "cuts64k", "dominant_half", "kink_strand", "strays_blocks", "stairs_shuffled", "lattice_30k", "lattice_250k", "hash_2000", "f32_vals",
    "sorted_swaps1", "blocks_1000", "two_values", "van_der_corput", "reach_far", "lattice_offs", "cuts16k", "stream_rand16", "zip_like", "late_batches",
    "desc_blocks_ties", "pm_inf", "all_equal", "organ_pipe", "sorted_one_swap", "half_sorted_halves", "sparse_kinks", "int_seconds", "edge_kinks" };
static void gen_more(const std::string &s, std::vector<double> &a) {
    size_t n = a.size();
    auto shuffle = [&](size_t lo, size_t hi) { for (size_t i = hi - 1; i > lo; i--) { size_t j = lo + (size_t)(nr() % (i - lo + 1)); std::swap(a[i], a[j]); } };
    if (s == "dense_perm") { for (size_t i = 0; i < n; i++) a[i] = (double)i; shuffle(0, n); }
    else if (s == "dense_16n") { for (size_t i = 0; i < n; i++) a[i] = (double)(i * 12 + nr() % 12); shuffle(0, n); }
    else if (s == "rle8") { for (size_t i = 0; i < n; i += 8) { double v = fr() * 1e6; for (size_t k = i; k < i + 8 && k < n; k++) a[k] = v; } }
    else if (s == "streams4" || s == "streams60") { size_t k = s == "streams4" ? 4 : 60; std::vector<double> cur(k); for (size_t t = 0; t < k; t++) cur[t] = (double)t * 1e9;
        for (size_t i = 0; i < n; i++) { size_t t = i % k; cur[t] += fr(); a[i] = cur[t]; } }
    else if (s == "ring") { size_t r = n / 3; for (size_t i = 0; i < n; i++) a[i] = (double)((i + r) % n) + 0.5; }
    else if (s == "ring_noisy") { size_t r = n / 3; for (size_t i = 0; i < n; i++) a[i] = (double)((i + r) % n) + (fr() - 0.5) * 100.0; }
    else if (s == "mirror_reach") { for (size_t i = 0; i < n; i++) a[i] = (double)(n - i) + (fr() - 0.5) * 100.0; }
    else if (s == "mirror_prefix") { size_t t = n / 20; for (size_t i = 0; i < n - t; i++) a[n - 1 - i] = (double)i + 0.5; for (size_t i = 0; i < t; i++) a[i] = fr() * (double)n; }
    else if (s == "head_5") { size_t h = n / 20; for (size_t i = 0; i < h; i++) a[i] = fr() * (double)n; for (size_t i = h; i < n; i++) a[i] = (double)i + 0.5; }
    else if (s == "wave32") { for (size_t i = 0; i < n; i++) { double x = (double)i / (double)n; a[i] = x * (double)n + 0.6 * ((double)n / 32.0) * sin(x * 32.0 * 6.283185307179586) + (fr() - 0.5) * 100.0; } }
    else if (s == "cuts64k") { size_t w = 65536; for (size_t i = 0; i < n; i++) a[i] = (double)(i / w) * 1e7 + fr() * 1e6; }
    else if (s == "dominant_half") { for (size_t i = 0; i < n; i++) a[i] = (nr() & 1) ? 42.0 : fr() * 1e6; }
    else if (s == "kink_strand") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = 40; i + 1 < n; i += 200) std::swap(a[i], a[i + 1]); }
    else if (s == "strays_blocks") { size_t B = 64, L = n / B; for (size_t b = 0; b < B; b++) { size_t v = (b * 37) % B; for (size_t i = 0; i < L; i++) a[b * L + i] = (double)(v * L + i) + 0.5; }
        for (size_t i = B * L; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = 0; i < n / 100; i++) a[nr() % n] = fr() * (double)n; }
    else if (s == "stairs_shuffled") { size_t B = 64, L = n / B; std::vector<size_t> ord(B); for (size_t b = 0; b < B; b++) ord[b] = (b * 29) % B;
        for (size_t b = 0; b < B; b++) { double lo = (double)ord[b] * (double)L * 0.8; for (size_t i = 0; i < L; i++) a[b * L + i] = lo + (double)i + fr() * 0.5; }
        for (size_t i = B * L; i < n; i++) a[i] = (double)B * (double)L + (double)i; }
    else if (s == "lattice_30k") { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; }
    else if (s == "lattice_250k") { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 250000) * 0.01; }
    else if (s == "hash_2000") { std::vector<double> v(2000); for (auto &x : v) x = fr() * 1e9; for (size_t i = 0; i < n; i++) a[i] = v[nr() % 2000]; }
    else if (s == "f32_vals") { for (size_t i = 0; i < n; i++) a[i] = (double)(float)(fr() * 1e6); }
    else if (s == "sorted_swaps1") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = 0; i < n / 100; i++) std::swap(a[nr() % n], a[nr() % n]); }
    else if (s == "blocks_1000") { size_t L = 1000, B = n / L; for (size_t b = 0; b < B; b++) { size_t v = (b * 7919) % B; for (size_t i = 0; i < L; i++) a[b * L + i] = (double)(v * L + i) + 0.5; }
        for (size_t i = B * L; i < n; i++) a[i] = (double)i + 0.5; }
    else if (s == "two_values") { for (size_t i = 0; i < n; i++) a[i] = (nr() & 1) ? 1.5 : -2.5; }
    else if (s == "lattice_offs") { for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 0.25; for (size_t j = 0; j < 200 && j * 10007 + 11 < n; j++) a[j * 10007 + 11] += 0.1;
        if (n > 16) { a[n - 5] = -1e12; a[n / 2] = 1e15; a[7] = -0.0; } }
    else if (s == "cuts16k") { size_t w = 16384; for (size_t i = 0; i < n; i++) a[i] = (double)(i / w) * 1e7 + fr() * 1e6; }
    else if (s == "stream_rand16") { std::vector<double> cur(16); for (size_t t = 0; t < 16; t++) cur[t] = (double)t * 1e9; for (size_t i = 0; i < n; i++) { size_t t = nr() % 16; cur[t] += fr(); a[i] = cur[t]; } }
    else if (s == "zip_like") { for (size_t i = 0; i < n; i++) { double u = fr(); a[i] = u < 0.055 ? 10001.0 : (double)(501 + (size_t)(pow(fr(), 3.0) * 42000.0)); } }
    else if (s == "late_batches") { size_t B = 64, L = n / B; for (size_t b = 0; b < B; b++) for (size_t i = 0; i < L; i++) a[b * L + i] = ((double)b + 1.2 * (double)i / (double)L) * (double)L;
        for (size_t i = B * L; i < n; i++) a[i] = 1e18 + (double)i; for (size_t b = 5; b + 9 < B; b += 16) for (size_t i = 0; i < L; i++) std::swap(a[b * L + i], a[(b + 7) * L + i]); }
    else if (s == "desc_blocks_ties") { size_t B = 64, L = n / B; for (size_t b = 0; b < B; b++) { size_t v = (b * 37) % B; for (size_t i = 0; i < L; i++) a[b * L + i] = (double)(v * L + (L - 1 - i) / 2 * 2); }
        for (size_t i = B * L; i < n; i++) a[i] = (double)(B * L + i); }
    else if (s == "pm_inf") { for (size_t i = 0; i < n; i++) { uint64_t r = nr() % 100; a[i] = r == 0 ? INFINITY : r == 1 ? -INFINITY : fr(); } if (n) a[0] = -INFINITY; }
    else if (s == "all_equal") { for (size_t i = 0; i < n; i++) a[i] = 3.25; }
    else if (s == "organ_pipe") { for (size_t i = 0; i < n; i++) a[i] = i < n / 2 ? (double)i + 0.5 : (double)(n - i) + 0.25; }
    else if (s == "sorted_one_swap") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; if (n > 8) std::swap(a[n / 4], a[3 * n / 4]); }
    else if (s == "half_sorted_halves") { size_t h = n * 2 / 5; for (size_t i = 0; i < h; i++) a[i] = (double)i * 2.5 + 0.5; for (size_t i = h; i < n; i++) a[i] = (double)(i - h) * 1.6 + 0.25; }
    else if (s == "sparse_kinks") { for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t i = 40; i + 1 < n; i += 2000) std::swap(a[i], a[i + 1]); }
    else if (s == "edge_kinks") {   // one strand with kinks ON the detection chunks' edges for every thread count, and at both ends
        for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
        if (n > 64) { std::swap(a[0], a[1]); std::swap(a[n - 2], a[n - 1]);
            for (size_t P : { 2, 3, 4, 5, 6, 8, 14 }) { size_t chunk = (n + P - 1) / P; for (size_t t = 1; t < P; t++) { size_t c = t * chunk; if (c + 4 >= n || c < 4) continue;
                if (a[c - 1] > a[c] || a[c] > a[c + 1] || a[c - 2] > a[c - 1] || a[c + 1] > a[c + 2]) continue;      // keep kinks apart
                if (t % 2 == 1) std::swap(a[c - 1], a[c]); else std::swap(a[c], a[c + 1]); } } } }
    else if (s == "int_seconds") { for (size_t i = 0; i < n; i++) a[i] = (double)(1700000000 + nr() % 86400); }
    else if (s == "van_der_corput") { for (size_t i = 0; i < n; i++) { uint64_t x = i, r = 0; for (int b = 0; b < 40; b++) { r = (r << 1) | (x & 1); x >>= 1; } a[i] = (double)r / 1099511627776.0; } }
    else if (s == "reach_far") { for (size_t i = 0; i < n; i++) a[i] = (double)i + (fr() - 0.5) * 20000.0; }
    else gen(s, a);
}

int main(int argc, char **argv) {
    std::vector<size_t> ns; std::vector<int> Ps;
    auto list = [](const char *s, std::vector<double> &out) { std::string t(s); size_t p = 0; while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size(); out.push_back(atof(t.substr(p, q - p).c_str())); p = q + 1; } };
    { std::vector<double> v; list(argc > 1 ? argv[1] : "3000,100000,1000003,8388608", v); for (double x : v) ns.push_back((size_t)x); }
    { std::vector<double> v; list(argc > 2 ? argv[2] : "1,2,4,14", v); for (double x : v) Ps.push_back((int)x); }
    const char *filt = argc > 3 ? argv[3] : "";
    bool verbose = getenv("ROUTES_VERBOSE") != NULL;
    int REPS = getenv("ROUTES_REPS") ? atoi(getenv("ROUTES_REPS")) : 6;
    std::vector<std::string> shapes;
    for (const char *s : BOARD) shapes.push_back(s);
    for (const char *s : EXTRA) shapes.push_back(s);
    for (const char *s : MORE) shapes.push_back(s);
    int cells = 0, same = 0, wrong = 0, differ = 0;
    for (const std::string &s : shapes) {
        if (*filt && s.find(filt) == std::string::npos) continue;
        for (size_t n : ns) {
            std::vector<double> base(n), c1(n), c2(n), r(n);
            RS = 7 + n; gen_more(s, base);
            bool has_nan = false; for (size_t i = 0; i < n; i++) if (base[i] != base[i]) { has_nan = true; break; }
            uint64_t want = bitsum(base.data(), n);
            for (int P : Ps) {
                // C several times, Rust several times: a witness counts as different only when the two engines' RANGES do not meet
                int reps = P == 1 ? 2 : REPS;
                size_t clo[64], chi[64], rlo[64], rhi[64], w[64];
                read_c(w); read_r(w);                            // clear
                bool okc = true, okr = true;
                for (int rep = 0; rep < reps; rep++) {
                    c1 = base; logos_sort_f64(c1.data(), n, P); read_c(w);
                    okc = okc && check(c1.data(), n, want, !has_nan);
                    for (int i = 0; i < NKEYS; i++) { if (!rep || w[i] < clo[i]) clo[i] = w[i]; if (!rep || w[i] > chi[i]) chi[i] = w[i]; }
                    r = base; logos_rsport_sort_f64(r.data(), n, P); read_r(w);
                    okr = okr && check(r.data(), n, want, !has_nan);
                    for (int i = 0; i < NKEYS; i++) { if (!rep || w[i] < rlo[i]) rlo[i] = w[i]; if (!rep || w[i] > rhi[i]) rhi[i] = w[i]; }
                }
                std::string diff, unstable, sig;
                for (int i = 0; i < NKEYS; i++) {
                    char b[128];
                    if (rhi[i] < clo[i] || rlo[i] > chi[i]) { snprintf(b, sizeof b, " %s C=%zu..%zu R=%zu..%zu;", key_name(KEYS[i]), clo[i], chi[i], rlo[i], rhi[i]); diff += b; }
                    else if (clo[i] != chi[i] || rlo[i] != rhi[i]) { snprintf(b, sizeof b, " %s", key_name(KEYS[i])); unstable += b; }
                    if (chi[i] && KEYS[i] != 100) { snprintf(b, sizeof b, " %s=%zu", key_name(KEYS[i]), chi[i]); sig += b; }
                }
                cells++;
                const char *verdict = !okr ? "WRONG OUTPUT (Rust)" : !okc ? "WRONG OUTPUT (C)" : diff.empty() ? "same" : "DIFFERENT";
                if (!okr || !okc) wrong++; else if (diff.empty()) same++; else differ++;
                if (verbose || !okr || !okc || !diff.empty())
                    printf("%-16s n=%-9zu P=%-2d %-10s%s%s%s%s%s\n", s.c_str(), n, P, verdict, diff.c_str(), unstable.empty() ? "" : "  [varies run to run:", unstable.c_str(), unstable.empty() ? "" : "]",
                           verbose ? (std::string("  |") + sig).c_str() : "");
                fflush(stdout);
            }
        }
    }
    printf("\n%d cells: %d same route, %d different, %d wrong output\n", cells, same, differ, wrong);
    return wrong || differ ? 1 : 0;
}
