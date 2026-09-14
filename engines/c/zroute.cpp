/* zroute.cpp — does a quadtree over (index, value) know which route a shape wants?
 *
 * THE IDEA. Treat every element as the point (position, value) and count how many cells of a
 * 2^l x 2^l grid its sample occupies at each level l, with both axes converted to RANKS. A sorted
 * array puts every point on the diagonal and occupies exactly 2^l cells, so the profile o[l] =
 * occupied / 2^l reads 1.00 at every scale. Disorder raises it. The shape of the profile over l is a
 * picture of the ARRANGEMENT: overlapping sorted runs plateau at the run count, disjoint blocks settle
 * near 2, near-sorted data peaks then decays, a sorted prefix with a random tail stays low and bulges
 * late. (experiments/team and the conversation that produced this: an 8-shape probe, zprobe.c.)
 *
 * THE TEST. The explorer's twenty-shape zoo (chaoshunt.cpp, included below unchanged) knows the
 * hidden structure of every shape, times the engine, and times a hand-routed sort that is TOLD the
 * structure. For each shape this adds the profile and a classification, then scores two things:
 *   1. does the profile classify arrangement structure correctly, and abstain on value-set structure;
 *   2. where the engine leaves more than 1.20x on the table, did the profile see the right structure?
 * Case 2 is the whole point: it is the routing opportunity the profile would open.
 *
 * NO FITTING TO THIS ZOO. The classification rule below was fixed BEFORE this file was run, from the
 * six training shapes of the earlier probe (sorted, uniform, sawtooth_16, blocks_64, near_1pct,
 * sorted+tail), all six of which it classifies correctly. It is applied here unchanged, so the
 * twenty shapes are a held-out test. Rule 6: the predictions in EX[] were also written first.
 *
 * PREDICTIONS, written before running:
 *   - value-set shapes (dense permutations, integers, lattices, cardinality): the probe reads
 *     arrangement only, so it should ABSTAIN (report random) on all 10.
 *   - arrangement shapes: about 4 of 8 correct (shard_ids_disjoint, sorted_runs_16, sorted_plus_tail,
 *     reverse_near_sort). Misses: event_ts_latency, window_shuffle and shard_ids_blocks because their
 *     disorder sits at or below the sample spacing (n/m = 2048 positions); sorted_runs_256 because 256
 *     overlapping runs saturate the sample-size ceiling and read as random.
 *   - opportunities concentrate at ONE thread, where the engine's sequential dispatcher is weakest.
 *
 * Ties in value are broken by a hash of the sample index, not by position, so duplicate values cannot
 * manufacture diagonal structure the data does not have.
 *
 * Run: make zroute && flock /tmp/logos-bench.lock ./zroute [reps]                                   */
/* ZROUTE_MAIN lets another file include this one for zprofile() without taking its main(). */
#ifndef ZROUTE_MAIN
#define ZROUTE_MAIN main
#endif
#define main chaoshunt_unused_main
#include "chaoshunt.cpp"
#undef main
#define main ZROUTE_MAIN

enum { Z_SORTED, Z_RANDOM, Z_KOVERLAP, Z_TAIL, Z_BOUNDED, Z_UNKNOWN, Z_N };
static const char *ZN[Z_N] = { "sorted", "random", "k-overlap", "tail", "bounded", "unknown" };
struct zprof_t { double o[13]; int cls; double k; int desc; };

static zprof_t zprofile(const double *a, size_t n) {
    const size_t m = 4096; const int B = 12;                       /* 2^B == m */
    std::vector<size_t> pos(m); std::vector<double> sv(m); std::vector<uint32_t> idx(m), vr(m);
    std::vector<uint64_t> z(m), hk(m);
    double phi = 0.6180339887498949, u = 0.0;
    for (size_t i = 0; i < m; i++) { u += phi; if (u >= 1) u -= 1; pos[i] = (size_t)(u * (double)n); if (pos[i] >= n) pos[i] = n - 1; }
    std::sort(pos.begin(), pos.end());                             /* position rank == sample index */
    for (size_t i = 0; i < m; i++) { sv[i] = a[pos[i]]; idx[i] = (uint32_t)i; hk[i] = mix((uint64_t)i + 0x9e3779b97f4a7c15ULL); }
    std::sort(idx.begin(), idx.end(), [&](uint32_t x, uint32_t y) { return sv[x] < sv[y] || (sv[x] == sv[y] && hk[x] < hk[y]); });
    for (size_t r = 0; r < m; r++) vr[idx[r]] = (uint32_t)r;
    for (size_t i = 0; i < m; i++) {
        uint64_t k = 0; uint32_t x = (uint32_t)i, y = vr[i];
        for (int b = 0; b < B; b++) k |= ((uint64_t)(x >> b & 1) << (2 * b + 1)) | ((uint64_t)(y >> b & 1) << (2 * b));
        z[i] = k;
    }
    std::sort(z.begin(), z.end());
    zprof_t p{};
    for (int l = 1; l <= B; l++) {
        int sh = 2 * (B - l); size_t occ = 1;
        for (size_t i = 1; i < m; i++) if ((z[i] >> sh) != (z[i - 1] >> sh)) occ++;
        p.o[l] = (double)occ / (double)(1u << l);
    }
    double lo = 0, hi = 0;                                         /* direction: first vs last quarter */
    for (size_t i = 0; i < m / 4; i++) lo += vr[i];
    for (size_t i = m - m / 4; i < m; i++) hi += vr[i];
    p.desc = hi < lo;

    /* THE RULE, fixed from the six training shapes before this zoo was run. Order matters. */
    double mx = 0; int am = 1;
    for (int l = 1; l <= 6; l++) if (p.o[l] > mx) { mx = p.o[l]; am = l; }
    auto near = [](double x, double y) { return fabs(x - y) <= 0.15 * std::max(x, y); };
    if (mx <= 1.3)                                                    p.cls = Z_SORTED;
    else if (p.o[5] >= 16 && p.o[6] >= 24)                            p.cls = Z_RANDOM;
    else if (near(p.o[4], p.o[5]) && near(p.o[5], p.o[6]) && p.o[5] >= 3) { p.cls = Z_KOVERLAP; p.k = p.o[5]; }
    else if (p.o[2] <= 3.0 && am >= 5)                                p.cls = Z_TAIL;
    else if (am <= 4 && p.o[6] < 0.6 * mx)                            p.cls = Z_BOUNDED;
    else                                                              p.cls = Z_UNKNOWN;
    return p;
}

/* Pre-registered. arr: is the hidden structure a property of the ARRANGEMENT (scored), of the VALUE
 * SET (the probe should abstain), or a non-monotone curve no route exploits (reported, not scored).
 * ideal: what a perfect arrangement probe would report. predicted: what we expect THIS one to report. */
enum { A_VALUESET = 0, A_ARRANGE = 1, A_CURVE = 2 };
struct expect_t { int arr, ideal, predicted; };
static const expect_t EX[S_N] = {
    /* uniform            */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* hash_order         */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* gray_code          */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* event_ts_latency   */ { A_ARRANGE,  Z_BOUNDED,  Z_SORTED   },
    /* window_shuffle     */ { A_ARRANGE,  Z_BOUNDED,  Z_SORTED   },
    /* shard_ids_disjoint */ { A_ARRANGE,  Z_KOVERLAP, Z_KOVERLAP },
    /* shard_ids_blocks   */ { A_ARRANGE,  Z_BOUNDED,  Z_SORTED   },
    /* sorted_runs_16     */ { A_ARRANGE,  Z_KOVERLAP, Z_KOVERLAP },
    /* sorted_runs_256    */ { A_ARRANGE,  Z_KOVERLAP, Z_RANDOM   },
    /* sorted_plus_tail   */ { A_ARRANGE,  Z_TAIL,     Z_TAIL     },
    /* reverse_near_sort  */ { A_ARRANGE,  Z_BOUNDED,  Z_BOUNDED  },
    /* prices_cents       */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* epoch_ms           */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* sensor_adc         */ { A_CURVE,    Z_UNKNOWN,  Z_UNKNOWN  },
    /* sensor_f64         */ { A_CURVE,    Z_UNKNOWN,  Z_UNKNOWN  },
    /* geo_lat            */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* user_ids_zipf      */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* yyyymmdd           */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* mantissa16         */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
    /* small_ratios       */ { A_VALUESET, Z_RANDOM,   Z_RANDOM   },
};
static int zhit(int d, const zprof_t &p) { return p.cls == EX[d].ideal && (d != S_REVNS || p.desc); }

struct row_t { int d, t; double tl, tc, gap; int okl, okc; char rt[96]; };

int main(int argc, char **argv) {
    int P = bench_threads(logos_num_cpus()); const size_t N = (size_t)1 << 23; int R = argc > 1 ? atoi(argv[1]) : 3;
    std::vector<double> base(N), a(N), ref(N), bufv(N); std::vector<uint64_t> k0(N), k1(N);
    g_buf = bufv.data(); g_k0 = k0.data(); g_k1 = k1.data();
    std::vector<row_t> rows; std::vector<zprof_t> zps(S_N);

    printf("zroute: does a quadtree over (index, value) know the route? n=2^23, best of %d, threads 1 and %d\n", R, P);
    printf("gap = logos / informed ceiling within a row. The rule and predictions were fixed before this ran.\n\n");
    printf("  %-18s %3s %9s %9s %6s  %-28s %-16s %-10s %s\n", "shape", "thr", "logos", "informed", "gap",
           "engine route/leaf", "profile", "ideal", "o[1..7]");
    for (int d = 0; d < S_N; d++) {
        RS = 7 + (uint64_t)d; gen(base.data(), N, d); params_t sp = CP; ref = base; std::sort(ref.begin(), ref.end());
        zprof_t zp = zprofile(base.data(), N); zps[d] = zp;
        int tset[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            int t = tset[ti]; row_t rw{}; rw.d = d; rw.t = t; rw.tl = 1e300; rw.tc = 1e300; rw.okl = 1; rw.okc = 1;
            route_of(a.data(), base.data(), N, t, rw.rt, sizeof rw.rt);
            for (int r = 0; r < R; r++) {
                memcpy(a.data(), base.data(), N * 8); double s = now(); logos_sort_f64(a.data(), N, t); double x = now() - s;
                if (x < rw.tl) rw.tl = x;
                if (memcmp(a.data(), ref.data(), N * 8)) rw.okl = 0;
                if (sp.kind != C_NONE) {
                    CP = sp; memcpy(a.data(), base.data(), N * 8); s = now(); informed(a.data(), N, t); x = now() - s;
                    if (x < rw.tc) rw.tc = x;
                    if (memcmp(a.data(), ref.data(), N * 8)) rw.okc = 0;
                }
            }
            rw.gap = sp.kind != C_NONE ? rw.tl / rw.tc : 1.0;
            char pc[40];
            if (zp.cls == Z_KOVERLAP) snprintf(pc, sizeof pc, "%s k=%.1f%s", ZN[zp.cls], zp.k, zp.desc ? " dsc" : "");
            else snprintf(pc, sizeof pc, "%s%s", ZN[zp.cls], zp.desc ? " dsc" : "");
            char cs[24], gs[16];
            if (sp.kind != C_NONE) { snprintf(cs, sizeof cs, "%7.1fms", rw.tc); snprintf(gs, sizeof gs, "%5.2fx", rw.gap); }
            else { snprintf(cs, sizeof cs, "%9s", "-"); snprintf(gs, sizeof gs, "%6s", "-"); }
            printf("  %-18s %3d %7.1fms %9s %6s  %-28s %-16s %-10s", NM[d], t, rw.tl, cs, gs, rw.rt, pc, ZN[EX[d].ideal]);
            if (ti == 0) for (int l = 1; l <= 7; l++) printf(" %.2f", zp.o[l]);
            printf("%s%s\n", rw.okl ? "" : "  LOGOS MISMATCH", rw.okc ? "" : "  INFORMED MISMATCH");
            fflush(stdout);
            rows.push_back(rw);
        }
    }

    printf("\n== scoring (rule and predictions fixed before this ran) ==\n");
    int asp = 0; for (int d = 0; d < S_N; d++) asp += zps[d].cls == EX[d].predicted;
    printf("  the probe's own output matched the prediction on %d of %d shapes\n", asp, S_N);
    int an = 0, ah = 0, vn = 0, va = 0;
    for (int d = 0; d < S_N; d++) {
        if (EX[d].arr == A_ARRANGE) { an++; ah += zhit(d, zps[d]); }
        else if (EX[d].arr == A_VALUESET) { vn++; va += zps[d].cls == Z_RANDOM; }
    }
    printf("  arrangement shapes classified correctly:           %d of %d   (predicted about 4)\n", ah, an);
    printf("  value-set shapes where the probe rightly abstained: %d of %d   (predicted %d)\n", va, vn, vn);
    for (int ti = 0; ti < 2; ti++) {
        int opp = 0, blind = 0, red = 0, moot = 0;
        printf("\n  %d thread%s, arrangement shapes (engine ok = within 1.20x of the informed ceiling):\n", ti ? P : 1, ti ? "s" : "");
        for (int d = 0; d < S_N; d++) {
            if (EX[d].arr != A_ARRANGE) continue;
            const row_t &r = rows[(size_t)d * 2 + (size_t)ti];
            int eok = r.gap <= 1.20, pok = zhit(d, zps[d]);
            const char *v = (!eok && pok) ? "OPPORTUNITY" : (!eok && !pok) ? "blind: neither sees it" : (eok && pok) ? "redundant" : "moot";
            if (!eok && pok) opp++; else if (!eok) blind++; else if (pok) red++; else moot++;
            printf("    %-18s gap %5.2fx   engine %-2s   profile %-2s   -> %s\n", NM[d], r.gap, eok ? "ok" : "no", pok ? "ok" : "no", v);
        }
        printf("    opportunity %d   blind %d   redundant %d   moot %d\n", opp, blind, red, moot);
    }
    printf("\n  curves (a non-monotone trend no route exploits; reported, not scored):");
    for (int d = 0; d < S_N; d++) if (EX[d].arr == A_CURVE) printf("  %s=%s", NM[d], ZN[zps[d].cls]);
    printf("\n");
    return 0;
}
