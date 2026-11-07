/* Does kinked data want more cores?
 *
 * The kink repair is ALREADY parallel: detect_runs_parallel gives each of P workers its own [lo, hi) chunk and each
 * worker swaps back the kinks it finds (logos_psort.c, the ascending and descending arms of the block scan). Only a
 * kink straddling a chunk boundary is left, at most P - 1 of them. So "divide the kinks among the cores" is done.
 *
 * And yet kinked data was measured to sort FASTEST ON ONE THREAD at every P (6.84 ms against 15.6 at two and 7.2 at
 * fourteen, amprobe's header). That measurement predates this session's vectorized block scan, which was worth
 * 1.30-1.32x on kinks at one thread -- a change to exactly the phase in question -- so it is re-measured here.
 *
 * Shape: 64 sorted blocks with disjoint value ranges, shuffled in position (block_perm), then a fraction of the
 * adjacent pairs at even offsets swapped (strandprobe's kinks). Min of `reps` per cell, one binary, one array.
 * The witness is logos_twist_last(4): kinks the detector actually swapped back, which reading clears.
 *
 * RESULT (2026-09-23): blocks + kinks scale 2.9-3.1x at 14 threads, so for that shape fewer cores would be worse.
 * ONE STRAND + kinks does not: 19.4 ms at one thread against 33.2 at two and 24.6 at fourteen, because `kinks fixed`
 * reads 41,745 at P = 1 and 0 at every P above it -- at P > 1 the NEAR-SORTED route claims the array before
 * relocation is reached, so the detector never runs. A route decision, not a thread cap.
 * FIXED (2026-09-26, the kink gate + the stray list on every worker): 0.5% kinks 5.52 ms at one thread -> 2.22 at 14 (2.49x),
 * 2.5% 9.20 -> 2.79 (3.30x); 0.05% still 0.27-0.58 with 0 repaired (REACH_KINK_MIN = 8 against ~7.9 expected descents).
 *
 * usage: ./kinkladder [reps] [n] [one | once] [P for once]
 *   (no mode) 64 shuffled sorted blocks + kinks     one  ONE sorted strand + kinks
 *   once [P]  a single sort, for a -DLOGOS_TRACE build to name the route
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "logos_psort.h"

static uint64_t RS = 88172645463325252ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* 64 sorted blocks with disjoint ranges, shuffled in position; then `f` of the even-offset adjacent pairs swapped */
static size_t gen_kinked(double *a, size_t n, double f, uint64_t seed) {
    RS = seed;
    const size_t nb = 64, blk = (n + nb - 1) / nb;
    size_t perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb; k > 1; k--) { size_t j = nr() % k; size_t t = perm[k - 1]; perm[k - 1] = perm[j]; perm[j] = t; }
    for (size_t k = 0; k < nb; k++) {
        size_t src = perm[k], o = k * blk;
        for (size_t i = 0; i < blk && o + i < n; i++) a[o + i] = (double)(src * blk + i);
    }
    size_t nk = 0;
    for (size_t x = 0; x + 1 < n; x += 2)
        if ((double)(nr() >> 11) / 9007199254740992.0 < f) { double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; nk++; }
    return nk;
}

/* ONE sorted strand with the same kinks: the whole array is a single run, so there is nothing for relocation to
   place and the parallel machinery has only the scan to divide. This is the shape behind the recorded claim. */
static size_t gen_kinked_one(double *a, size_t n, double f, uint64_t seed) {
    RS = seed;
    /* + 0.5 so the values are NOT integers. A first cut used a[i] = i exactly, which even after adjacent swaps is a
       PERMUTATION OF 0..n-1: the dense-integer bitmap claimed it before the run detector was ever reached, the kink
       repair reported 0 fixed, and that looked like an engine defect. It was the probe. (`once` mode + a -DLOGOS_TRACE
       build named the route in one line: "dense-int: 2000000 values, window 5997627".) */
    for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5;
    size_t nk = 0;
    for (size_t x = 0; x + 1 < n; x += 2)
        if ((double)(nr() >> 11) / 9007199254740992.0 < f) { double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; nk++; }
    return nk;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 9;
    size_t n = argc > 2 ? strtoull(argv[2], 0, 10) : 8388608;
    int one = argc > 3 && !strcmp(argv[3], "one");
    /* mode `once`: ONE sort of the 0.5% shape at one thread, for a -DLOGOS_TRACE build to name the route.
       The ladder runs 32 sorts per shape, which buries the trace. */
    if (argc > 3 && !strcmp(argv[3], "once")) {
        int tp = argc > 4 ? atoi(argv[4]) : 1;
        double *b = malloc(n * 8), *v = malloc(n * 8);
        if (!b || !v) { printf("no memory\n"); return 2; }
        size_t nk = gen_kinked_one(b, n, 0.005, 4242);
        memcpy(v, b, n * 8);
        /* the kinks that lie within four of a detection-chunk edge (chunk = ceil(n / P), as detect_runs_parallel sizes
           it): the worker cannot repair a pair whose test reads across its edge, and the join repairs only a pair that
           straddles one. A strand that comes back as R > 1 broke at one of these; print them so the trace can be read. */
        { size_t chunk = (n + (size_t)tp - 1) / (size_t)tp;
          for (int t = 1; t < tp; t++) { size_t bd = (size_t)t * chunk;
            for (size_t x = bd >= 6 ? bd - 6 : 0; x + 1 < n && x <= bd + 6; x++)
              if (b[x] > b[x + 1]) fprintf(stderr, "#### kink pair (%zu, %zu) lies at chunk edge %zu (edge - %ld)\n", x, x + 1, bd, (long)bd - (long)x); } }
        fprintf(stderr, "#### ONE sorted strand, %zu kinks (%.2f%%), n = %zu, 1 thread\n", nk, 100.0 * (double)nk / (double)n, n);
        logos_twist_last(4);
        logos_sort_f64(v, n, tp);
        fprintf(stderr, "#### kinks repaired = %zu, one-strand finishes = %zu, reach = %d\n",
                logos_twist_last(4), logos_twist_last(12), logos_reach_last());
        free(b); free(v); return 0;
    }
    static const int PS[] = { 1, 2, 3, 4, 6, 8, 10, 14 };
    static const double FK[] = { 0.0, 0.001, 0.01, 0.05 };
    double *base = malloc(n * 8), *w = malloc(n * 8);
    if (!base || !w) { printf("no memory\n"); return 2; }
    printf("KINKED DATA AGAINST A THREAD LADDER, n = %zu, min of %d reps, shape = %s.\n",
           n, reps, one ? "ONE sorted strand + kinks" : "64 shuffled sorted blocks + kinks");
    printf("The kink repair is already per-worker; this asks whether the SORT wants the threads.\n\n");
    for (size_t fi = 0; fi < sizeof FK / sizeof *FK; fi++) {
        size_t nk = one ? gen_kinked_one(base, n, FK[fi], 4242) : gen_kinked(base, n, FK[fi], 4242);
        printf("  %.2f%% kinks (%zu planted)\n", 100.0 * (double)nk / (double)n, nk);
        printf("    %-4s %10s %10s %9s %12s\n", "P", "ms", "ns/elem", "vs 1T", "kinks fixed");
        double t1 = 0;
        for (size_t pi = 0; pi < sizeof PS / sizeof *PS; pi++) {
            int P = PS[pi]; double best = 1e300; size_t fixed = 0; int bad = 0;
            for (int r = 0; r < reps; r++) {
                memcpy(w, base, n * 8);
                logos_twist_last(4);                                  /* clear the counter */
                double t0 = ms(); logos_sort_f64(w, n, P); double d = ms() - t0;
                if (d < best) { best = d; fixed = logos_twist_last(4); } else logos_twist_last(4);
                for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { bad = 1; break; }
            }
            if (pi == 0) t1 = best;
            printf("    %-4d %10.2f %10.2f %9.2f %12zu%s\n", P, best, best * 1e6 / (double)n, t1 / best, fixed, bad ? "  UNSORTED" : "");
        }
        printf("\n");
    }
    free(base); free(w);
    return 0;
}
