/* callbench: what ONE CALL costs at the small end, a baseline engine against the working tree, in one binary.
 *
 * Every A/B bench in this directory starts at a million elements and the harness checks answers, not times, so a cost per CALL is
 * invisible to all of them (the ledger's knot-route entry: two getenv calls took three sorted elements from 6 to 42 ns). make
 * scale-full shows the small end, but against OTHER sorts and minutes apart. This bench asks the narrower question that a change to
 * the dispatcher's or the leaf's first page needs answered: did a call get more expensive than it was?
 *
 * S2, the small sorting networks (2026-09-22). PREDICTION, written before the first run: the leaf sorts 4 to 16 elements
 * with a branchless Batcher network instead of insertion sort, so RANDOM rows of 4-16 elements 1.5-3x (insertion there is
 * ~n^2/4 compares of which about half mispredict; the network is 5 to 63 compare-exchanges with no branches at all), 17-32
 * elements 1.0-1.1 (only the quicksort base case sees it), nothing from 64 up, and SORTED rows unchanged at every n because
 * the prescan returns a sorted array before the leaf is reached. Ships if the random rows from 4 to 16 clear 1.15 in both
 * orientations and two runs, with no sorted row under 0.97 and nothing at n <= 5 under 0.97.
 *
 *   make callbench                        baseline = the committed engine (git show HEAD:engines/c/logos_psort.c)
 *   make callbench CB_BASE=/path/old.c    baseline = any engine source that builds against today's logos_psort.h
 *   make callbench CB_BASE=a.c SRC=b.c    any two sources (a = baseline)
 *
 * Two binaries, because the copies are not interchangeable (methodology 3): in callbench_a the baseline is copy cb0 and the working
 * tree cb1, in callbench_b they are swapped, and the geometric mean of the two work/base ratios cancels the copies' layout bias.
 * Inside a binary the two engines alternate in 80 blocks per size and the lower quartile of each engine's block times is kept.
 * K distinct inputs per size are copied in before every call; the copy and the loop are inside both timings (about 3 ns, which
 * dilutes a ratio at n <= 3 and not above). Two shapes: random, and already sorted, where a call is the dispatcher's first page and
 * nothing else, so a fixed cost shows at full size.
 *
 * usage: callbench <slot of the working tree: 0|1> [the other orientation's output]
 *
 * WHAT IT FOUND THE DAY IT WAS WRITTEN (2026-09-20: the short-reach round against the engine before it; CHANGELOG has the table).
 *   - MEASURED, FIXED. A gate helper with two 16 KB arrays was inlined into the dispatcher, whose prologue probes every page of its
 *     frame on EVERY call (stack-clash protection): 8 probes became 10, and a call with 0 or 1 elements cost 9-14% more, in all
 *     eight readings (two sizes, two shapes, two orientations: 1.07-1.16). noinline on the helper: 0.98-1.01. -fstack-usage prints
 *     every function's frame.
 *   - NOT RESOLVED, REMOVED ANYWAY. A thread-local diagnostic was reset at the top of every sort. Under -fPIC a thread-local access
 *     is a call to __tls_get_addr, and the dispatcher had none: two of them on the path of every sort of two or more elements.
 *     Predicted +20-35% on a sorted triple. Measured alone: +5% in one orientation and -14% in the other, which is nothing. The
 *     call's fast path is about ten instructions (and the public entry already made one). It went because removing it is free: the
 *     reset now starts at the route's floor of 2^18 elements, and logos_psort.h says so.
 *   - A WIN THAT IS CODE LAYOUT, NOT SHIPPED. With a probe loop inline in the general leaf, a sort of 100 random doubles, WHICH
 *     NEVER RUNS IT, read 5% slower than the baseline; with the loop deleted 2.5% faster; with the loop out of line 12% faster
 *     (three rounds, both orientations, in this bench's scratch predecessor; and again here: 1.00-1.03 inline, 0.88-0.89 out of
 *     line). But the round AS BUILT, with the same inline loop and two unrelated differences in ANOTHER function, read 0.90.
 *     No mechanism, so the engine keeps the inline probe that every A/B of the round measured.
 *   - THE CEILING. Entering the dispatcher costs about 5.6 ns before any work (a thread-local read, the call, nine probed pages:
 *     the frame is 36 KB). The public entry now returns at once when n < 2: 8.5 -> 2.9 ns, 0.29 (orientations 0.34 and 0.25).
 *     Every call of 2 to 24 elements still pays it; a thin front for them is open direction 0g.
 * WHAT IT CANNOT SAY, which the third finding shows. The swap cancels the bias between the two COPIES. It does not cancel what a
 * code change does to the addresses of everything after it: rows from 10 to 100 elements read anywhere from 0.78 to 1.13 across
 * nine builds whose logic on that path is identical, REPRODUCIBLY (two runs of one pair of binaries agree within 1 to 5 points,
 * and two different bench binaries agree with each other). Reproducible is not real: it is a property of one build, and the next
 * edit anywhere earlier in the file draws again. An alignment policy (-falign-functions, -falign-loops) is the untried lever.
 * And on a box that has just run something long, single rows at n <= 5 scatter to 0.90-1.14 with the two orientations
 * disagreeing (0.79 against 1.02): when the two ratio columns of a row disagree, that row is noise or layout. The final record run
 * had the clearest case: shipped engine as cb0, the engine before the round as cb1, sorted input of 2 to 5 elements read 1.02-1.30
 * in that ONE orientation over five runs and either link order (1.00-1.05 in the other), worst on a slow box; the same engine
 * code in the same slot beside a DIFFERENT partner object read 0.95-1.00. One orientation of one pairing is not a cost.
 * Believe a row when it is at n <= 5, agrees in both orientations and in two runs, and has a mechanism you can point at in the
 * assembly (cc -S for the path, -fstack-usage for the frame). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

void cb0_sort_f64(double *, size_t, int);
void cb1_sort_f64(double *, size_t, int);

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }
static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }

enum { N_SIZES = 13, N_SHAPES = 2, BLOCKS = 80 };
static const size_t SIZES[N_SIZES] = { 0, 1, 2, 3, 5, 10, 24, 32, 72, 100, 1000, 5000, 50000 };
static const char *SHAPES[N_SHAPES] = { "random", "sorted" };

static double lower_quartile(double *v, int m) {
    for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return v[m / 4];
}

int main(int argc, char **argv) {
    int work_slot = argc > 1 ? atoi(argv[1]) : 1;
    double other[N_SHAPES][N_SIZES]; int have_other = 0;
    for (int s = 0; s < N_SHAPES; s++) { for (int i = 0; i < N_SIZES; i++) { other[s][i] = 0; } }
    if (argc > 2) {
        FILE *f = fopen(argv[2], "r"); char line[512];
        if (!f) { fprintf(stderr, "callbench: cannot read %s\n", argv[2]); return 2; }
        while (fgets(line, sizeof line, f)) {
            char shape[32]; unsigned long n; double b, w, r;
            if (sscanf(line, "row %31s %lu %lf %lf %lf", shape, &n, &b, &w, &r) != 5) { continue; }
            for (int s = 0; s < N_SHAPES; s++) { for (int i = 0; i < N_SIZES; i++) { if (!strcmp(shape, SHAPES[s]) && SIZES[i] == (size_t)n) { other[s][i] = r; have_other = 1; } } }
        }
        fclose(f);
    }
    printf("# callbench: ns per call, 1 thread, lower quartile of %d alternating blocks per engine; the working tree is copy cb%d here.\n", BLOCKS / 2, work_slot);
    printf("# work/base above 1 means a call costs MORE than it did.%s\n", have_other ? " Last column: both orientations, the copies' layout bias cancelled." : "");
    if (have_other) { printf("# A row whose two ratio columns disagree is noise; a row from 10 to 100 elements moves with code layout alone (see the header).\n"); }
    for (int s = 0; s < N_SHAPES; s++) {
        printf("#\n# %-8s %8s %12s %12s %10s%s\n", "shape", "n", "base", "work", "work/base", have_other ? "   other run   cancelled" : "");
        for (int si = 0; si < N_SIZES; si++) {
            size_t n = SIZES[si], w1 = n ? n : 1, K = 4096 / w1; if (K < 4) { K = 4; }
            size_t reps = n <= 100 ? 50000 : n <= 5000 ? 1000 : 75;
            double *pool = malloc(K * w1 * sizeof(double)), *w = malloc(w1 * sizeof(double));
            if (!pool || !w) { fprintf(stderr, "callbench: no memory\n"); return 2; }
            RS = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(si * 131 + s);
            for (size_t k = 0; k < K; k++) {
                double acc = 0;
                for (size_t i = 0; i < w1; i++) { double v = (double)(nr() >> 11); if (s == 1) { acc += v; v = acc; } pool[k * w1 + i] = v; }
            }
            static double tt[2][BLOCKS]; int cnt[2] = { 0, 0 };
            for (int blk = 0; blk < BLOCKS; blk++) {
                int slot = blk & 1; double t0 = now_ns();
                for (size_t r = 0; r < reps; r++) {
                    memcpy(w, pool + (r % K) * w1, n * sizeof(double));
                    if (slot) { cb1_sort_f64(w, n, 1); } else { cb0_sort_f64(w, n, 1); }
                }
                tt[slot][cnt[slot]++] = (now_ns() - t0) / (double)reps;
                for (size_t i = 1; i < n; i++) { if (w[i] < w[i - 1]) { printf("UNSORTED: %s n=%zu copy cb%d\n", SHAPES[s], n, slot); return 1; } }
            }
            double q[2] = { lower_quartile(tt[0], cnt[0]), lower_quartile(tt[1], cnt[1]) };
            double base = q[1 - work_slot], work = q[work_slot], ratio = work / base;
            printf("row %-8s %8zu %12.1f %12.1f %10.3f", SHAPES[s], n, base, work, ratio);
            if (have_other && other[s][si] > 0) {
                double c = ratio * other[s][si], g = 1.0, lo = 0.0, hi = c > 1.0 ? c : 1.0;       /* sqrt by bisection: no libm needed */
                for (int it = 0; it < 60; it++) { g = 0.5 * (lo + hi); if (g * g > c) { hi = g; } else { lo = g; } }
                printf("   %9.3f   %9.3f", other[s][si], g);
            }
            printf("\n"); fflush(stdout);
            free(pool); free(w);
        }
    }
    return 0;
}
