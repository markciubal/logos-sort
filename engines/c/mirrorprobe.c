/* mirrorprobe.c — where else could the Moebius twist go? A probe, not a route: nothing here changes the engine.
 *
 * WHAT THE TWIST ROUND TAUGHT (make twistbench, FINDINGS 3ag), as constraints on where to look next:
 *   1. carrying an orientation bit pays where the consumer COPIES ANYWAY, so the reversal folds into a copy already owed;
 *   2. it pays in proportion to how CHEAP the route already is (a swap pass is a sixth of a 2.3 ns/element route and a
 *      thirtieth of a 16 ns one), and it ties where a merge is the whole sort;
 *   3. it has to be decidable for free, or it taxes every sort that has nothing twisted.
 * The twist as built carries the bit per RUN, inside one route family. Two other places fit those constraints on paper:
 *   (L) the LEAF's sequential detector (detect_runs_limited) still reverses in place before the galloping run-merge and the
 *       leaf's own J5. The same bit, one level down.
 *   (G) the WHOLE ARRAY. Every cheap route the engine owns (near-sorted, short reach, sorted prefix plus tail, interleaved
 *       streams) reads order in ONE direction. Hand any of them its own input mirrored (newest-first event logs, a feed
 *       sorted descending by time with late arrivals) and no gate fires: the prescan's `descending` test is exact, so one
 *       late record turns a 3 ns/element array into a 16 ns one. The orientation bit there is ONE bit for the array, and
 *       the shared sample already holds it: inv_all, the inverted share of adjacent sample pairs, reads ~0.98 on such data.
 *
 * WHAT IS MEASURED. For each shape S and its mirror M (S read back to front), paired, order alternated:
 *   ceiling   t(sort M) / t(sort S)                 what a route that READS the twist would save (no pass at all)
 *   simple    t(sort M) / t(reverse M, then sort)   what ONE reversal pass in front of the dispatcher would save
 *                                                   (the pass is timed; at P threads it runs on P threads)
 * Rows for (L) use the twist bench's method: Z with descending runs against Z' with them ascending, on shapes that reach
 * the leaf detector rather than the top-level one. Controls: uniform (a mirror of noise is noise, so `simple` prices the
 * pass on data it cannot help: that is the cost of a false positive), and block_perm / stairs, which the twist as built
 * already takes (their ceiling should read ~1.0-1.1 where it used to be the whole route's multiple).
 *
 * PREDICTIONS, written before the first run.
 *   (G) 1 thread, ceiling: near_sorted 3-4.5x, short reach 2-3x, prefix_95 3-4x, streams 2-3x; `simple` about 0.9 of that.
 *       14 threads: near_sorted 1.3-2x, short reach 1.2-1.5x, prefix_95 1.1-1.2x, streams 1.0 (that route is sequential).
 *       uniform: ceiling 1.00, simple 0.95-0.98 at 1 thread and 0.90-0.97 at 14. block_perm, stairs: 1.0-1.15.
 *   (L) sawtooth of 64 descending teeth over one range (the leaf detector reverses all of it, then finds 64 runs, too many
 *       to merge, and the general leaf sorts it anyway): 1.01-1.04, by constraint 2. Six descending teeth (run-merge or
 *       knot): 1.00-1.05 at 1 thread because the knot route takes it first and already carries the bit.
 * STOP RULE: a site is worth building only above 1.25 on a realistic shape, since (G) needs a new gate in the dispatcher's
 * first page (make callbench) and a gate is a tax.
 *
 * MEASURED (2026-09-21, 8.4M, runs of 40 and 60 pairs, ceiling / simple):
 *   short reach +-60      1 thread 2.02-2.11 / 1.92-1.99    14 threads 1.41-1.44 / 1.11-1.19
 *   prefix_95 + tail      1 thread 2.71-2.74 / 2.43-2.47    14 threads 0.56 / 0.49-0.52  (the MIRROR is faster: see below)
 *   4 ascending streams   1 thread 3.10-3.30 / 2.92-3.14    14 threads 1.10-1.12 / 0.96
 *   near_sorted 1% swaps  1 thread 1.22-1.23 / 1.20         14 threads 1.17-1.22 / 1.00
 *   uniform (control)     ceiling 0.99-1.02; simple 0.95-0.99 at 1 thread, 0.90 at 14: a false positive costs 5-10%
 *   block_perm, stairs    ceiling 0.97-1.02: the twist as built made a mirrored block shape FREE. simple 0.77-0.90: a pass in
 *                         front of a route that already reads the twist is pure loss, so (G) must sit BEHIND relocation.
 *   (L) teeth             1.02-1.06 everywhere. Below the stop rule, as predicted: not worth building.
 * GRADED. Right: short reach, streams, uniform, the block controls, all of (L). Low: prefix_95 (predicted 3-4x). WRONG:
 * near_sorted (predicted 3-4.5x, measured 1.2x: with 1% of elements swapped across the whole array it is not a cheap shape at
 * one thread either, the leaf sorts it), and prefix_95 at 14 threads, wrong in DIRECTION: the mirror sorts 1.8x FASTER than the
 * shape itself. Mode `tail` checked the obvious suspect and cleared it: the parallel tail route still beats the partition on
 * that shape (1.075 at 14 threads, 3.95 at one). So a sorted run with a random SUFFIX costs the parallel engine 1.8x what the
 * same run with a random PREFIX, descending, costs it. Not traced. That is a lead of its own, and it is not about orientation.
 * VERDICT: (G) clears the stop rule on three realistic shapes at one thread and on short reach at 14. (L) does not.
 *
 * MODE `ab`, after (G) was BUILT in its plain form (mirror_gate in logos_psort.c: sequential sorts, behind relocation, the
 * sample's writhe plus a directional gate run on the mirror image, one reversal, re-entry): LOGOS_NO_MIRROR=1 against the
 * shipped engine, ONE binary and one engine copy, so there is no layout term. Rows: every shape MIRRORED, and the shapes as
 * they lie as guards (the as-is stream shape now pays for a mirrored probe that stops after ~25 reads).
 * PREDICTION, written first. 1 thread: short reach mirrored 1.85-2.00, prefix_95 mirrored 2.30-2.50, streams mirrored
 * 2.80-3.10 (the `simple` column above, less a re-entry of under a millisecond); every other row 0.98-1.02. P threads:
 * every row 0.98-1.02, because a parallel sort is never offered it. SHIP RULE: no row under 0.98, the three rows over 1.5.
 * usage: ./mirrorprobe [pairs] [n] [once | tail | ab]   (once: one sort each, for a trace build; tail: LOGOS_NO_TAIL=1 against the tail route)
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
#include <pthread.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

enum { S_NEAR, S_REACH, S_PREFIX, S_STREAMS, S_GROUPED, S_UNIFORM, S_BLOCKS, S_STAIRS, S_N };
static const char *SN[S_N] = { "near_sorted, 1% swaps", "short reach, +-60", "prefix_95 + random tail", "4 ascending streams", "sorted runs by coarse key",
                               "uniform (CONTROL)", "block_perm (twist has it)", "stairs x20 (twist has it)" };
static void gen(double *a, size_t n, int s) {
    RS = 99 + (uint64_t)s;
    size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) switch (s) {
        case S_NEAR:    a[i] = (double)i + fr() * 0.5; break;
        case S_REACH:   a[i] = (double)i + (fr() * 2.0 - 1.0) * 60.0; break;
        case S_PREFIX:  a[i] = i < n - n / 20 ? (double)i : fr() * (double)n; break;
        case S_STREAMS: a[i] = (double)(i % 4) * (double)n + (double)(i / 4) + fr() * 0.5; break;
        case S_GROUPED: a[i] = 0.0; break;                                  /* filled below */
        case S_UNIFORM: a[i] = fr() * 2e9 - 1e9; break;
        case S_BLOCKS:  a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5; break;
        default:        a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + 1.0 / 9.0) + fr() * 0.5; break;
    }
    if (s == S_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
    if (s == S_GROUPED) {                       /* sorted runs of ~4096, grouped by a coarse key: the near-sorted gate's shape (make nsgatebench) */
        size_t g = 4096; for (size_t i = 0; i < n; i++) a[i] = (double)((i / g) % 8) * (double)n + (double)i + fr() * 0.5;
    }
}
static void mirror(double *a, size_t n) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }

/* the reversal pass of the `simple` fix, on P threads: thread t swaps its slice of the first half with the mirror slice */
typedef struct { double *a; size_t n, lo, hi; } rv_t;
static void *rv_body(void *p) { rv_t *w = p; double *a = w->a; size_t n = w->n; for (size_t l = w->lo; l < w->hi; l++) { double t = a[l]; a[l] = a[n - 1 - l]; a[n - 1 - l] = t; } return NULL; }
static void reverse_par(double *a, size_t n, int P) {
    size_t h = n / 2; if (P < 2 || n < (1u << 20)) { rv_t w = { a, n, 0, h }; rv_body(&w); return; }
    if (P > 64) P = 64;
    pthread_t th[64]; rv_t w[64]; int made = 0;
    for (int t = 0; t < P; t++) { w[t].a = a; w[t].n = n; w[t].lo = h * (size_t)t / (size_t)P; w[t].hi = h * (size_t)(t + 1) / (size_t)P; }
    for (int t = 1; t < P; t++) { if (pthread_create(&th[made], NULL, rv_body, &w[t])) rv_body(&w[t]); else made++; }
    rv_body(&w[0]); for (int t = 0; t < made; t++) pthread_join(th[t], NULL);
}
static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

/* A and B adjacently, order alternated; prepB: reverse B's input first, INSIDE the timed region. Median of tA / tB. */
static const char *ENV_A = NULL;      /* set around side A's sort only (mode `tail`) */
static double pair(const double *A, const double *B, int prepB, double *w, size_t n, int P, int reps, double *p10, double *p90, int *ok) {
    double *r = (double *)malloc((size_t)reps * sizeof(double)); *ok = r != NULL; if (!r) return 0;
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int side = (i & 1) ? 1 - k : k;
            memcpy(w, side ? B : A, n * sizeof(double));
            if (!side && ENV_A) setenv(ENV_A, "1", 1);
            double s = now_ms(); if (side && prepB) reverse_par(w, n, P); logos_sort_f64(w, n, P); t[side] = now_ms() - s;
            if (!side && ENV_A) unsetenv(ENV_A);
            if (!sorted_ok(w, n)) *ok = 0;
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; free(r); return med;
}

/* (L): nt teeth over ONE value range, every tooth descending in Z and ascending in Z' */
static void teeth(double *a, size_t n, size_t nt, int desc) {
    RS = 4242; size_t tl = (n + nt - 1) / nt;
    for (size_t i = 0; i < n; i++) { size_t j = i % tl; if (desc) j = tl - 1 - j; a[i] = (double)j * (1e6 / (double)tl) + (double)(i / tl) * 1e-4 + fr() * 1e-5; }
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40; size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 8388608;
    int once = argc > 3 && !strcmp(argv[3], "once"), P = bench_threads(logos_num_cpus());
    (void)bench_paired; (void)bench_threads_source; if (reps < 4) reps = 4;
    double *s = malloc(n * 8), *m = malloc(n * 8), *w = malloc(n * 8); if (!s || !m || !w) return 2;
    if (once) {
        for (int k = 0; k < S_N; k++) { gen(s, n, k); memcpy(m, s, n * 8); mirror(m, n);
            for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1;
                fprintf(stderr, "#### %s, AS IS, %d thread(s)\n", SN[k], T); memcpy(w, s, n * 8); logos_sort_f64(w, n, T);
                fprintf(stderr, "#### %s, MIRRORED, %d thread(s)\n", SN[k], T); memcpy(w, m, n * 8); logos_sort_f64(w, n, T); } }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "tail")) {     /* a side question the first run raised: does the PARALLEL tail route still beat the partition? */
        gen(s, n, S_PREFIX); ENV_A = "LOGOS_NO_TAIL"; double lo, hi; int ok;
        double c1 = pair(s, s, 0, w, n, 1, reps, &lo, &hi, &ok); printf("prefix_95, t(LOGOS_NO_TAIL=1) / t(tail route):  1 thread %6.3f [%.2f %.2f]%s", c1, lo, hi, ok ? "" : " X");
        double cP = pair(s, s, 0, w, n, P, reps, &lo, &hi, &ok); printf("   %d threads %6.3f [%.2f %.2f]%s\n", P, cP, lo, hi, ok ? "" : " X");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "ab")) {
        printf("THE WHOLE ARRAY'S ORIENTATION BIT, A/B, n = %zu, %d pairs, P = %d: t(LOGOS_NO_MIRROR=1) / t(shipped), same array both sides. Above 1 = the bit is faster.\n\n", n, reps, P);
        printf("  %-28s | %-24s %-24s | %-24s %-24s\n", "shape", "MIRRORED, 1 thread", "MIRRORED, P threads", "as it lies (GUARD), 1T", "as it lies (GUARD), PT");
        ENV_A = "LOGOS_NO_MIRROR";
        for (int k = 0; k < S_N; k++) {
            if (argc > 4 && !strstr(argv[4], SN[k]) && !strstr(SN[k], argv[4])) continue;      /* a 5th argument reruns the rows whose name it contains */
            gen(s, n, k); memcpy(m, s, n * 8); mirror(m, n);
            double c[4], lo[4], hi[4]; int ok[4];
            c[0] = pair(m, m, 0, w, n, 1, reps, &lo[0], &hi[0], &ok[0]); c[1] = pair(m, m, 0, w, n, P, reps, &lo[1], &hi[1], &ok[1]);
            c[2] = pair(s, s, 0, w, n, 1, reps, &lo[2], &hi[2], &ok[2]); c[3] = pair(s, s, 0, w, n, P, reps, &lo[3], &hi[3], &ok[3]);
            printf("  %-28s | %6.3f [%.2f %.2f]%s    %6.3f [%.2f %.2f]%s    | %6.3f [%.2f %.2f]%s    %6.3f [%.2f %.2f]%s\n", SN[k],
                   c[0], lo[0], hi[0], ok[0] ? "  " : " X", c[1], lo[1], hi[1], ok[1] ? "  " : " X", c[2], lo[2], hi[2], ok[2] ? "  " : " X", c[3], lo[3], hi[3], ok[3] ? "  " : " X");
            fflush(stdout);
        }
        printf("\nRESULT: ratios only; every output checked ascending.\n");
        return 0;
    }
    printf("WHERE ELSE COULD THE TWIST GO? n = %zu, %d pairs, P = %d. Every cell is a median of paired ratios [p10 p90]; above 1 = the mirror costs more.\n\n", n, reps, P);
    printf("(G) ONE BIT FOR THE WHOLE ARRAY: a shape against its own mirror image\n");
    printf("  %-28s | %-22s %-22s | %-22s %-22s\n", "shape", "1T ceiling", "1T simple (pass+sort)", "PT ceiling", "PT simple");
    for (int k = 0; k < S_N; k++) {
        gen(s, n, k); memcpy(m, s, n * 8); mirror(m, n);
        double c[4], lo[4], hi[4]; int ok[4];
        c[0] = pair(m, s, 0, w, n, 1, reps, &lo[0], &hi[0], &ok[0]); c[1] = pair(m, m, 1, w, n, 1, reps, &lo[1], &hi[1], &ok[1]);
        c[2] = pair(m, s, 0, w, n, P, reps, &lo[2], &hi[2], &ok[2]); c[3] = pair(m, m, 1, w, n, P, reps, &lo[3], &hi[3], &ok[3]);
        printf("  %-28s | %6.3f [%.2f %.2f]%s  %6.3f [%.2f %.2f]%s  | %6.3f [%.2f %.2f]%s  %6.3f [%.2f %.2f]%s\n", SN[k],
               c[0], lo[0], hi[0], ok[0] ? "  " : " X", c[1], lo[1], hi[1], ok[1] ? "  " : " X", c[2], lo[2], hi[2], ok[2] ? "  " : " X", c[3], lo[3], hi[3], ok[3] ? "  " : " X");
        fflush(stdout);
    }
    printf("\n(L) THE LEAF'S DETECTOR: descending teeth over one value range (Z) against the same teeth ascending (Z')\n");
    printf("  %-28s | %-22s | %-22s\n", "shape", "1T  t(Z)/t(Z')", "PT  t(Z)/t(Z')");
    const size_t NT[] = { 6, 64 }; const size_t NN[] = { n, 3000 };
    for (int q = 0; q < 2; q++) for (int z = 0; z < 2; z++) {
        size_t nn = NN[z], nt = NT[q]; if (nn == 3000 && nt == 64) continue;
        teeth(s, nn, nt, 0); teeth(m, nn, nt, 1);
        double a10, a90, b10, b90; int ok1, okP;
        double c1 = pair(m, s, 0, w, nn, 1, nn < 100000 ? reps * 50 : reps, &a10, &a90, &ok1), cP = nn < 100000 ? 0 : pair(m, s, 0, w, nn, P, reps, &b10, &b90, &okP);
        char nm[64]; snprintf(nm, sizeof nm, "%zu teeth, n = %zu", nt, nn);
        if (nn < 100000) printf("  %-28s | %6.3f [%.2f %.2f]%s | (a parallel sort never sees it)\n", nm, c1, a10, a90, ok1 ? "  " : " X");
        else printf("  %-28s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", nm, c1, a10, a90, ok1 ? "  " : " X", cP, b10, b90, okP ? "  " : " X");
        fflush(stdout);
    }
    printf("\nRESULT: ratios only; every output checked ascending. Read the uniform row first: its ceiling is this box's floor.\n");
    free(s); free(m); free(w); return 0;
}
