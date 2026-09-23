/* twistbench.c — the Moebius twist: strands that carry their orientation, so that a descending run is never reversed.
 *
 * WHERE IT COMES FROM. A sibling line of this engine (v3.10, read here as an outside review) links natural RUNS into a ring
 * and calls it a Moebius ring: descending runs are "negative-orientation strands" and the tail-to-head seam "flips logical
 * orientation". But there the twist is a flag. When run ranges overlap it NORMALISES first (reverses every descending run in
 * place) and then merges, and this engine does the same one step earlier: its run detector swaps a descending run end for
 * end the moment it has found it (reloc_detect_worker_body, detect_runs_limited). A strip is only a Moebius strip if you can
 * walk across the twist WITHOUT cutting and re-gluing, and reversing a run in memory is exactly the cut and the glue.
 *
 * WHAT A REAL TWIST WOULD BE. The run descriptor carries one orientation bit. Nothing is reversed. Every consumer reads a
 * twisted strand from its far end: its first value is a[end], a lone piece is placed by a backward copy, a crossing is merged
 * with one cursor walking down (an up-run followed by a down-run is a bitonic pair, and the two-ended merge of a bitonic pair
 * needs no reversal at all), and the knot route's cuts are found by a binary search that knows which way the strand runs.
 * What that buys is the pass that is no longer made: one read and one write of every element that sits in a descending run.
 *
 * CEILING FIRST (mode `ceiling`, the default). Each shape Z has some of its sorted blocks lying descending. Its twin Z' is
 * the SAME array with those blocks already ascending: the same multiset, the same run boundaries, the same route, and after
 * the detector has done its reversals Z IS Z', byte for byte. So t(Z) / t(Z') prices exactly the reversal passes, and that is
 * the most an orientation-carrying implementation could save, reached only if a backward read costs nothing more than a
 * forward one. Paired: the two sorted adjacently, order alternated, median of the ratio with p10/p90.
 *
 * PREDICTION, written before the first run. A swap pass over L2-resident runs is about half a nanosecond per element, the
 * run routes cost 2-9 ns per element at one thread and about 1-2 of wall at fourteen, where they are traffic-bound:
 *   every block descending, 1 thread      1.03-1.08        every block descending, 14 threads   1.05-1.15
 *   every other block, either count       half of that     no block descending (THE NULL)       1.00
 * STOP RULE, also written first: build the twist into the routes only if the ceiling reaches 1.06 on at least two twisted
 * shapes at one thread count. Below that the whole idea is worth less than this box can resolve, whatever it is called.
 *
 * THE CEILING, MEASURED (2026-09-20, 8.4M, 40 pairs, 1 thread / 14 threads): null 1.014 / 1.017; every block twisted 1.174 /
 * 1.118; every other one 1.070 / 1.019; 1024 blocks of 8k 1.164 / 1.051; stairs all twisted 1.115 / 1.075; stairs every other
 * 1.067 / 1.029. The one-thread prediction was WRONG, and low: the run routes cost 2.3 ns per element there, not 2-9, so a swap
 * pass is a sixth of the whole sort. The 14-thread prediction held. The stop rule is met by five rows at one thread: build it.
 *
 * A/B (mode `ab`): the engine with the twist against the same engine with LOGOS_NO_TWIST=1, which restores the in-place
 * reversal. ONE binary and one engine copy, switched by the environment, so there is no code-layout term to cancel. Same
 * pairing. Guards: the two shapes with nothing twisted and a uniform random array, where both sides run the same code.
 * PREDICTION for the A/B, written before its first run. The twist cannot beat the ceiling, and it pays for what replaces the
 * swap pass: a backward copy where there was a memcpy, a cursor walking down in the merges. Both are streams a prefetcher
 * follows, so most of the ceiling should arrive:
 *   1 thread:   every block twisted 1.10-1.16, blocks of 8k 1.09-1.15, stairs all 1.06-1.10, the every-other rows 1.03-1.06
 *   14 threads: every block twisted 1.05-1.11, stairs all 1.03-1.07, blocks of 8k 1.02-1.05, the every-other rows inside the floor
 *   guards:     0.98-1.02 everywhere
 * SHIP RULE: keep it if no guard is below 0.98 and at least three twisted rows at one thread clear 1.04 (this box's floor for
 * 40 pairs at 8M); otherwise restore the reversal and record the negative.
 *
 * MODE `merge`, added after the first A/B: the staircase rows recovered less of their ceiling than the block rows (every other
 * block twisted read 1.00 at one thread against a ceiling of 1.07), and the suspect was the first two-ended merge, ONE loop with
 * a signed stride per strand: four cursor updates per element where the untwisted merge has two. It compares that loop
 * (LOGOS_TWIST_GENERIC=1) with one loop per orientation pair. PREDICTION: 1.03-1.07 at one thread on the x20 staircases,
 * more on x80 where four elements in five are merged (1.08-1.20), nothing at 14 threads, nothing on the block rows.
 *
 * MODE `scan`, 2026-09-21 (experiments/TWIST_AND_KNOT.md, avenue 3b): the twist left the run detector read-only, and the profile
 * puts its scan at 47-49% of the run routes at one thread (1.1 ns per element, for a READ) and 31% at 14. The scan now takes
 * eight elements at a time whenever a bound proves the scalar test would accept them all; LOGOS_NO_FASTSCAN=1 is the scalar
 * scan. Same binary, same engine copy. PREDICTION, written first: the scan falls from 1.1 to 0.35-0.5 ns per element, so
 *   1 thread:   blocks and x20 staircases, twisted or not, 1.30-1.45; x80 staircases 1.10-1.20 (the merge is most of those)
 *   14 threads: 1.05-1.15 (the scan is already spread over 14 cores and reads at the memory's pace)
 *   uniform:    1.00 (chunk 0 gives up within a few dozen elements)
 * SHIP RULE: keep it if no row is below 0.98 and the block rows clear 1.15 at one thread.
 *
 * MODE `list` (and `listplain`), 2026-09-21: the route taken at its word. Asked whether the array is first converted to a linked
 * list: it is not, it never was, and the literal version was then built to be measured (knot_place_list, LOGOS_KNOT_LIST=1):
 * a 32-bit link per element, runs linked forward or BACKWARD (in a list the twist is free), crossings untied by relinking, the
 * units stitched into one sorted list, the list walked out. The ratio is t(array of descriptors, shipped) / t(linked list), so
 * above 1 means the list is faster. The walk guesses next = cur +- 1 and lets the load only check the guess; `listplain` sets
 * LOGOS_LIST_PLAIN=1 and walks by the pointer alone. PREDICTION, written before the first run:
 *   1 thread, guessed walk: blocks 0.60-0.72 (a 4-byte link written and read back per element on top of the same copies), x20
 *     staircases 0.60-0.72, x80 0.50-0.62 (a relinking merge chases a pointer per element, and its output is then walked too)
 *   1 thread, plain walk:   blocks 0.40-0.55 (one dependent load per element, about 5 cycles each, where memcpy moves 8 bytes a cycle)
 *   14 threads: 0.75-0.92 everywhere, plain or guessed (fourteen chains run at once, and the extra link traffic is what is left)
 *   uniform: 1.00 (never reaches the route). Memory: 4 bytes per element more.
 * It is a measurement of a structure, not a candidate: nothing ships unless a row clears 1.04, which I do not expect.
 * usage: ./twistbench [pairs] [n] [ceiling|ab|merge|scan|list|listplain]
 */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

/* nb sorted blocks; block b covers [b, b+1+o) in units of the block length, so consecutive value-blocks overlap by o of a
   block's range (o = 0: disjoint, relocation's shape; o > 0: a staircase, the knot route's). Block order is shuffled. */
static void blocks(double *a, size_t n, size_t nb, double o, uint64_t seed) {
    RS = seed;
    if (nb == 0) { for (size_t i = 0; i < n; i++) a[i] = fr() * 1e9; return; }          /* the guard: uniform random */
    size_t blk = (n + nb - 1) / nb, *perm = (size_t *)malloc(nb * sizeof *perm);
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5;
    free(perm);
}
/* lay every `every`-th block (by position) descending: the multiset and the block boundaries do not change */
static void twist(double *a, size_t n, size_t nb, int every) {
    if (nb == 0) return;
    size_t blk = (n + nb - 1) / nb;
    for (size_t b = 0; b * blk < n; b++) {
        if (every <= 0 || (b % (size_t)every) != 0) continue;
        size_t lo = b * blk, hi = lo + blk > n ? n : lo + blk;
        for (size_t l = lo, r = hi - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }
    }
}
#define XO(x) ((x) / (2.0 - (x)))      /* the overlap that puts a share x of the elements inside two-strand crossings */
typedef struct { const char *nm; size_t nb; double o; int every; const char *what; } shape_t;
static const shape_t SH[] = {
    { "blocks, none twisted (NULL)", 64, 0.0,      0, "Z is Z': whatever this row prints is the floor" },
    { "blocks, every one twisted  ", 64, 0.0,      1, "64 shuffled disjoint blocks, all descending: relocation" },
    { "blocks, every other one    ", 64, 0.0,      2, "half the elements in descending runs" },
    { "blocks of 8k, all twisted  ", 1024, 0.0,    1, "1024 short blocks: more strands, the same passes" },
    { "stairs x20, all twisted    ", 64, XO(0.20), 1, "ranges overlap, 20% of elements cross: the knot route" },
    { "stairs x20, every other    ", 64, XO(0.20), 2, "" },
    { "stairs x80, all twisted    ", 64, XO(0.80), 1, "80% of elements cross: the two-ended merge carries the row" },
    { "stairs x80, every other    ", 64, XO(0.80), 2, "every crossing is one strand up and one down: a bitonic pair" },
    { "stairs x20, none (GUARD)   ", 64, XO(0.20), 0, "nothing twisted: both sides of the A/B run the same code" },
    { "uniform random (GUARD)     ", 0, 0.0,       0, "never reaches the detector" },
};
#define NSH ((int)(sizeof SH / sizeof SH[0]))

static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

/* A and B sorted ADJACENTLY, the order alternated; returns the median of tA/tB. envA/envB: "NAME=1" set around that side's
   sort and cleared after it, or NULL. */
static double pair(const double *A, const char *envA, const double *B, const char *envB, double *w, size_t n, int P, int reps,
                   double *p10, double *p90, int *ok) {
    double *r = (double *)malloc((size_t)reps * sizeof(double)); if (!r) { *ok = 0; return 0; }
    *ok = 1;
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int side = (i & 1) ? 1 - k : k;                       /* 0 = A, 1 = B */
            const double *src = side ? B : A; const char *env = side ? envB : envA;
            memcpy(w, src, n * sizeof(double));
            if (env) setenv(env, "1", 1);
            double s = now_ms(); logos_sort_f64(w, n, P); t[side] = now_ms() - s;
            if (env) unsetenv(env);
            if (!sorted_ok(w, n)) *ok = 0;
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    if (p10) *p10 = r[(int)(0.10 * reps)];
    if (p90) *p90 = r[(int)(0.90 * reps)];
    free(r); return med;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40;
    size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 8388608;
    const char *mode = argc > 3 ? argv[3] : "ceiling";
    int ab = !strcmp(mode, "ab"), mg = !strcmp(mode, "merge"), sc = !strcmp(mode, "scan"), lp = !strcmp(mode, "listplain"), li = lp || !strcmp(mode, "list"), P = bench_threads(logos_num_cpus());
    (void)bench_paired; (void)bench_threads_source;
    if (reps < 4) reps = 4;
    double *z = (double *)malloc(n * sizeof(double)), *zp = (double *)malloc(n * sizeof(double)), *w = (double *)malloc(n * sizeof(double));
    if (!z || !zp || !w) { printf("no memory\n"); return 2; }
    if (lp) setenv("LOGOS_LIST_PLAIN", "1", 1);      /* read only when the list is on */
    if (li)  printf("THE LITERAL LINKED LIST, n = %zu, %d pairs, %s walk: t(array of run descriptors, as shipped) / t(a link per element, relinked, walked out).\n"
                    "Above 1 = the linked list is faster. The last column counts the placements that really went through the list.\n\n", n, reps, lp ? "PLAIN pointer" : "guessed");
    else if (sc)  printf("THE SCAN IN BLOCKS OF EIGHT, n = %zu, %d pairs: t(LOGOS_NO_FASTSCAN=1, one element at a time) / t(eight at a time), the twist\n"
                    "on both sides. Every row that reaches the run detector can differ, twisted or not.\n\n", n, reps);
    else if (mg)  printf("THE TWO-ENDED MERGE, n = %zu, %d pairs: t(LOGOS_TWIST_GENERIC=1, one strided loop) / t(one loop per orientation pair),\n"
                    "the twist on both sides. Only rows with twisted strands that CROSS can differ.\n\n", n, reps);
    else if (!ab) printf("THE CEILING of the Moebius twist, n = %zu, %d pairs: t(Z) / t(Z'), where Z has blocks lying descending and Z' is the\n"
                    "same array with them already ascending. After the detector's in-place reversals Z IS Z', so the ratio prices the\n"
                    "reversal passes and nothing else: the most that strands carrying their orientation could save.\n\n", n, reps);
    else     printf("THE TWIST, A/B, n = %zu, %d pairs: t(LOGOS_NO_TWIST=1, reverse in place as before) / t(twist on), same array both\n"
                    "sides. Above 1 means carrying the orientation is faster than reversing.\n\n", n, reps);
    printf("  %-28s | %-26s %-26s %s\n", "shape", "1 thread  median [p10 p90]", "P threads", "");
    for (int s = 0; s < NSH; s++) {
        blocks(zp, n, SH[s].nb, SH[s].o, 4242 + (uint64_t)s);
        memcpy(z, zp, n * sizeof(double)); twist(z, n, SH[s].nb, SH[s].every);
        double a10, a90, b10, b90; int ok1, okP; double m1, mP;
        if (li)       { (void)logos_twist_last(3); m1 = pair(z, NULL, z, "LOGOS_KNOT_LIST", w, n, 1, reps, &a10, &a90, &ok1); mP = pair(z, NULL, z, "LOGOS_KNOT_LIST", w, n, P, reps, &b10, &b90, &okP); }
        else if (sc)  { m1 = pair(z, "LOGOS_NO_FASTSCAN", z, NULL, w, n, 1, reps, &a10, &a90, &ok1); mP = pair(z, "LOGOS_NO_FASTSCAN", z, NULL, w, n, P, reps, &b10, &b90, &okP); }
        else if (mg)  { m1 = pair(z, "LOGOS_TWIST_GENERIC", z, NULL, w, n, 1, reps, &a10, &a90, &ok1); mP = pair(z, "LOGOS_TWIST_GENERIC", z, NULL, w, n, P, reps, &b10, &b90, &okP); }
        else if (!ab) { m1 = pair(z, NULL, zp, NULL, w, n, 1, reps, &a10, &a90, &ok1); mP = pair(z, NULL, zp, NULL, w, n, P, reps, &b10, &b90, &okP); }
        else     { m1 = pair(z, "LOGOS_NO_TWIST", z, NULL, w, n, 1, reps, &a10, &a90, &ok1); mP = pair(z, "LOGOS_NO_TWIST", z, NULL, w, n, P, reps, &b10, &b90, &okP); }
        if (li) printf("  %-28s | %6.3f [%.2f %.2f]%s      %6.3f [%.2f %.2f]%s      %zu of %d sorts went through the list\n", SH[s].nm, m1, a10, a90, ok1 ? "    " : " BAD",
               mP, b10, b90, okP ? "    " : " BAD", logos_twist_last(3), 2 * reps);
        else printf("  %-28s | %6.3f [%.2f %.2f]%s      %6.3f [%.2f %.2f]%s      %s\n", SH[s].nm, m1, a10, a90, ok1 ? "    " : " BAD",
               mP, b10, b90, okP ? "    " : " BAD", SH[s].what);
        fflush(stdout);
    }
    printf("\nP = %d threads. Read the NULL row first. RESULT: ratios only; every output checked ascending.\n", P);
    free(z); free(zp); free(w);
    return 0;
}
