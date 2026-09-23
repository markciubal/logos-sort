/* strandprobe.c — two probes from experiments/TWIST_AND_KNOT.md. Nothing here changes the engine.
 *
 * MODE `strays` (avenue 2, the unknotting number). Sorted blocks with a share of LATE RECORDS: a fraction f of positions
 * overwritten with values from anywhere. One stray ends a run, runs of a thousand fail the detector's average-run test, and
 * relocation never sees the array. The ceiling is a sort TOLD which positions are strays: lift them out (one pass), sort
 * what is left with the engine (clean blocks: the run routes), sort the strays, merge the two (one pass). Everything is timed.
 * The compaction and the final merge run on ONE thread, so the 14-thread ceiling is a floor on what a route could do.
 *   ratio = t(engine on the array as it is) / t(told)
 * PREDICTION, written first: 1 thread 3-4x at f = 0.1% and 1%, 1.5-2.5x at 5%; 14 threads 0.7-1.1 (the told sort's two serial
 * passes cost what the whole parallel sort costs). STOP RULE: build only if 1% strays clear 2x at one thread.
 *
 * MODE `writhe` (avenue 5). Two numbers from 2,048 strided points, no sort, no second pass:
 *   w = 1 - 2 * (share of adjacent sample pairs that descend)      the writhe: +1 ascending, -1 mirrored, 0 noise OR balanced folds
 *   p = share of consecutive pair-directions that AGREE             persistence: 1/3 for noise, near 1 for strands of either direction
 * RULE, fixed before scoring: p < 0.6 noise; else w >= 0.8 ascending strands, w <= -0.8 mirrored, otherwise folds.
 * PREDICTION: every shape below lands in the class named beside it; inv_all alone (w) cannot tell `noise` from `folds`.
 * Known blind spot, by construction: a period that divides the stride (interleaved streams) reads as one stream.
 *
 * MEASURED, 2026-09-21, 8.4M. STRAYS, one thread: block_perm 0.61 (0% strays: the told sort's own overhead), 1.68 (0.1%), 2.22
 * (1%), 1.97 (5%); stairs x20 0.65, 1.56, 2.04, 1.89. 14 threads 0.19-0.43: the told sort's serial passes make it no ceiling
 * there. The prediction (3-4x) was LOW BY HALF, and the 0% row says why: the teller pays two passes a real route would not, so
 * the real ceiling is nearer 3x. Stop rule met; not built yet (a strand with holes breaks the knot plan's searches and offsets).
 * WRITHE: 16 of 16 as predicted. Noise reads p = 0.33, every stranded shape 0.95-1.00; a triangle wave, a sloped sine and
 * every-other-block-twisted read w = 0.00-0.05 like noise, and p = 0.97-0.98. It cannot tell mirrored blocks from a mirrored
 * ribbon (same w, p and turn count). Not wired into the dispatcher.
 *
 * MODE `kinks` (Reidemeister move I, from an outside suggestion read on 2026-09-21): the cheap subset of `strays`. A KINK is
 * an adjacent pair swapped inside an otherwise sorted run ([.. 4 6 5 7 ..]). One kink ends a run exactly as a stray does,
 * but the misplaced element sits NEXT TO its hole, so the strand stays contiguous: a detector that swaps the pair where it
 * stands keeps every binary search and every offset of the knot plan valid. The ceiling: the engine on the kinked array
 * against the engine on the same array with the kinks undone by a pass that is TOLD where they are (timed; it is a walk
 * over a position list, so unlike `strays` the teller costs almost nothing and the 0% row should read 1.00).
 * PREDICTION, written first: one thread 6-10x at 0.1%, 1% and 5% kinks (a 16 ns/element full sort against a 1.6 ns run
 * route); 14 threads 1.5-1.9x. STOP RULE: build the detector's kink repair only if 1% kinks clear 2x at one thread.
 * KINKS, MEASURED (24 pairs, 8.4M): one thread 5.5-5.6x on blocks at 0.1% and 1%, 4.5 at 4.8%, 4.0-4.3 on staircases; 14
 * threads 1.15-1.44 (1.00 on a staircase at 4.8%); the 0% rows 1.00-1.02. The prediction (6-10x, 1.5-1.9x) was too high
 * at both counts. Stop rule met by a factor of two: BUILT (the detector swaps a kink back where it stands; off with
 * LOGOS_NO_KINK=1).
 *
 * MODE `kinkab`: the A/B of that build. t(LOGOS_NO_KINK=1) / t(shipped) on the same array, one binary, one engine copy.
 * PREDICTION, written first: one thread 3.8-5.3x on the kinked rows (the ceiling less the detector's extra work at every
 * run break), 14 threads 1.10-1.40; the 0% rows, uniform and near_sorted 0.98-1.02 (random data breaks a run every two or
 * three elements and asks the kink question each time, but the detector gives up within a few hundred elements).
 * SHIP RULE: no guard under 0.98, the 1% rows over 2x at one thread.
 * usage: ./strandprobe [pairs] [n] strays | kinks | kinkab | ./strandprobe 0 [n] writhe
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
static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

static void blocks(double *a, size_t n, size_t nb, double o, int twist_every) {
    size_t blk = (n + nb - 1) / nb, *perm = malloc(nb * sizeof *perm);
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5;
    if (twist_every > 0) for (size_t b = 0; b * blk < n; b += (size_t)twist_every) { size_t lo = b * blk, hi = lo + blk > n ? n : lo + blk;
        for (size_t l = lo, r = hi - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }
    free(perm);
}
static void mirror(double *a, size_t n) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }

/* ---------------------------------------------------------------- strays */
static void told_sort(double *w, const unsigned char *mask, size_t n, double *rest, double *st, int P) {
    size_t nr_ = 0, ns = 0;
    for (size_t i = 0; i < n; i++) { if (mask[i]) st[ns++] = w[i]; else rest[nr_++] = w[i]; }
    logos_sort_f64(rest, nr_, P); logos_sort_f64(st, ns, P);
    size_t i = 0, j = 0, o = 0;
    while (i < nr_ && j < ns) { double x = rest[i], y = st[j]; int ty = y < x; w[o++] = ty ? y : x; j += (size_t)ty; i += (size_t)!ty; }
    if (i < nr_) memcpy(w + o, rest + i, (nr_ - i) * 8); else if (j < ns) memcpy(w + o, st + j, (ns - j) * 8);
}
static const char *KENV = NULL;                              /* mode kinkab: side 0 sorts with this switch set, side 1 as shipped */
static size_t *KPOS = NULL; static size_t NKINK = 0;      /* mode kinks: the told positions, ascending */
static void told_unkink(double *w, size_t n, int P) { for (size_t q = 0; q < NKINK; q++) { size_t x = KPOS[q]; double t = w[x]; w[x] = w[x + 1]; w[x + 1] = t; } logos_sort_f64(w, n, P); }
static double pair_strays(const double *A, const unsigned char *mask, double *w, double *rest, double *st, size_t n, int P, int reps, double *p10, double *p90, int *ok) {
    double *r = malloc((size_t)reps * sizeof(double)); *ok = 1;
    for (int i = 0; i < reps; i++) { double t[2];
        for (int k = 0; k < 2; k++) { int side = (i & 1) ? 1 - k : k;
            memcpy(w, A, n * 8); double s = now_ms(); if (KENV) { if (!side) setenv(KENV, "1", 1); logos_sort_f64(w, n, P); if (!side) unsetenv(KENV); }
            else if (side) { if (KPOS) told_unkink(w, n, P); else told_sort(w, mask, n, rest, st, P); } else logos_sort_f64(w, n, P);
            t[side] = now_ms() - s;
            if (!sorted_ok(w, n)) *ok = 0; }
        r[i] = t[0] / t[1]; }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]); *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; free(r); return med;
}

/* ---------------------------------------------------------------- writhe */
static const char *CLS[] = { "noise", "ascending", "mirrored", "folds" };
static int classify(const double *a, size_t n, double *pw, double *pp, size_t *pfolds) {
    enum { S = 2048 }; double s[S]; size_t step = n / S; for (size_t i = 0; i < S; i++) s[i] = a[i * step];
    size_t down = 0, agree = 0, folds = 0; int prev = 0;
    for (size_t i = 1; i < S; i++) { int d = s[i] < s[i - 1] ? -1 : 1; down += d < 0; if (i > 1) { agree += d == prev; folds += d != prev; } prev = d; }
    double w = 1.0 - 2.0 * (double)down / (double)(S - 1), p = (double)agree / (double)(S - 2);
    *pw = w; *pp = p; *pfolds = folds;
    return p < 0.6 ? 0 : w >= 0.8 ? 1 : w <= -0.8 ? 2 : 3;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 30; size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 8388608;
    const char *mode = argc > 3 ? argv[3] : "strays"; int P = bench_threads(logos_num_cpus());
    (void)bench_paired; (void)bench_threads_source;
    double *a = malloc(n * 8), *w = malloc(n * 8), *rest = malloc(n * 8), *st = malloc(n * 8); unsigned char *mask = malloc(n);
    if (!a || !w || !rest || !st || !mask) return 2;
    if (!strcmp(mode, "writhe")) {
        struct { const char *nm; int want; } T[] = {
            { "uniform", 0 }, { "heavy tail", 0 }, { "short reach +-60", 1 }, { "short reach, MIRRORED", 2 }, { "near_sorted 1% swaps", 1 }, { "near_sorted, MIRRORED", 2 },
            { "block_perm 64", 1 }, { "block_perm, MIRRORED", 2 }, { "blocks, every other twisted", 3 }, { "stairs x20, all twisted", 2 },
            { "sine + slope, clean", 3 }, { "sine + slope, noise +-50", 3 }, { "triangle wave, 40 folds", 3 }, { "blocks + 1% strays", 1 }, { "prefix_95", 1 }, { "prefix_95, MIRRORED", 2 } };
        int nt = (int)(sizeof T / sizeof T[0]), hit = 0;
        printf("WRITHE AND PERSISTENCE from 2,048 strided points, n = %zu. Rule fixed in advance: p < 0.6 noise; else w >= 0.8 ascending, w <= -0.8 mirrored, otherwise folds.\n\n", n);
        printf("  %-30s %8s %8s %7s   %-10s %-10s\n", "shape", "writhe", "persist", "turns", "class", "predicted");
        for (int k = 0; k < nt; k++) { RS = 500 + (uint64_t)k;
            switch (k) {
            case 0: for (size_t i = 0; i < n; i++) a[i] = fr(); break;
            case 1: for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
            case 2: case 3: for (size_t i = 0; i < n; i++) a[i] = (double)i + (fr() * 2 - 1) * 60.0; if (k == 3) mirror(a, n); break;
            case 4: case 5: for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; for (size_t q = 0; q < n / 100; q++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } if (k == 5) mirror(a, n); break;
            case 6: case 7: blocks(a, n, 64, 0.0, 0); if (k == 7) mirror(a, n); break;
            case 8: blocks(a, n, 64, 0.0, 2); break;
            case 9: blocks(a, n, 64, 1.0 / 9.0, 1); break;
            case 10: case 11: for (size_t i = 0; i < n; i++) a[i] = (double)i + 4.0 * ((double)n / 65.0) * sin(6.283185307179586 * 32.5 * (double)i / (double)n) + (k == 11 ? (fr() * 2 - 1) * 50.0 : 0.0); break;
            case 12: { size_t h = n / 40; for (size_t i = 0; i < n; i++) { size_t j = i % h; a[i] = ((i / h) & 1) ? (double)(h - j) : (double)j; a[i] += fr() * 0.5; } } break;
            case 13: blocks(a, n, 64, 0.0, 0); for (size_t q = 0; q < n / 100; q++) a[nr() % n] = fr() * (double)n; break;
            default: for (size_t i = 0; i < n; i++) a[i] = i < n - n / 20 ? (double)i : fr() * (double)n; if (k == 15) mirror(a, n); break;
            }
            double wr, pe; size_t folds; int c = classify(a, n, &wr, &pe, &folds); hit += c == T[k].want;
            printf("  %-30s %8.3f %8.3f %7zu   %-10s %-10s %s\n", T[k].nm, wr, pe, folds, CLS[c], CLS[T[k].want], c == T[k].want ? "" : "<-- MISS");
        }
        printf("\n%d of %d as predicted.\n", hit, nt);
        return 0;
    }
    int kab = !strcmp(mode, "kinkab"); if (kab) KENV = "LOGOS_NO_KINK";
    if (kab || !strcmp(mode, "kinks")) {
        printf("KINKS INSIDE STRANDS (adjacent pairs swapped), n = %zu, %d pairs, P = %d: t(engine, array as it is) / t(kinks undone by a told pass, then the engine).\n\n", n, reps, P);
        printf("  %-34s | %-24s | %-24s\n", "shape", "1 thread", "P threads");
        if (kab) printf("MODE kinkab: the columns are t(LOGOS_NO_KINK=1) / t(shipped). Above 1 = the kink repair is faster.\n");
        const double FK[] = { 0.0, 0.001, 0.01, 0.05 }; const double OK_[] = { 0.0, 1.0 / 9.0 };
        KPOS = malloc((n / 2 + 1) * sizeof(size_t)); if (!KPOS) return 2;
        for (int oi = 0; oi < 2; oi++) for (int tw = 0; tw < 2; tw++) for (int fi = 0; fi < 4; fi++) {
            if (tw && (fi == 0 || fi == 3)) continue;
            if (argc > 4 && !strcmp(argv[4], "guards") && fi != 0) continue;          /* `kinkab guards`: only the rows with nothing to repair */
            RS = 1300 + (uint64_t)(oi * 100 + tw * 10 + fi); blocks(a, n, 64, OK_[oi], tw ? 2 : 0); memset(mask, 0, n);
            size_t want = (size_t)(FK[fi] * (double)n), blk = (n + 63) / 64;
            for (size_t q = 0; q < want; q++) { size_t x = 2 * (nr() % (n / 2 - 1)); if ((x + 1) % blk == 0 || x % blk == 0) continue; mask[x] = 1; }
            NKINK = 0; for (size_t x = 0; x + 1 < n; x += 2) if (mask[x]) { double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; KPOS[NKINK++] = x; }
            double l1, h1, lp, hp; int ok1, okp;
            double c1 = pair_strays(a, mask, w, rest, st, n, 1, reps, &l1, &h1, &ok1), cp = pair_strays(a, mask, w, rest, st, n, P, reps, &lp, &hp, &okp);
            char nm[80]; snprintf(nm, sizeof nm, "%s%s, %.1f%% kinks", oi ? "stairs x20" : "block_perm", tw ? " (alt twisted)" : "", 100.0 * (double)NKINK / (double)n);
            printf("  %-34s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", nm, c1, l1, h1, ok1 ? "  " : " X", cp, lp, hp, okp ? "  " : " X"); fflush(stdout);
        }
        if (kab) for (int g = 0; g < 2; g++) {
            if (argc > 4 && !strcmp(argv[4], "guards")) reps = argc > 1 ? atoi(argv[1]) : reps;            /* guards that never had a kink to repair */
            RS = 77 + (uint64_t)g; NKINK = 0;
            if (!g) for (size_t i = 0; i < n; i++) a[i] = fr() * 1e9;
            else { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; for (size_t q = 0; q < n / 100; q++) { size_t x = nr() % n, y = nr() % n; double v = a[x]; a[x] = a[y]; a[y] = v; } }
            double l1, h1, lp, hp; int ok1, okp;
            double c1 = pair_strays(a, mask, w, rest, st, n, 1, reps, &l1, &h1, &ok1), cp = pair_strays(a, mask, w, rest, st, n, P, reps, &lp, &hp, &okp);
            printf("  %-34s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", g ? "near_sorted 1% swaps (GUARD)" : "uniform random (GUARD)", c1, l1, h1, ok1 ? "  " : " X", cp, lp, hp, okp ? "  " : " X"); fflush(stdout);
        }
        return 0;
    }
    printf("STRAYS INSIDE STRANDS, n = %zu, %d pairs, P = %d: t(engine, array as it is) / t(a sort TOLD which positions are strays).\n\n", n, reps, P);
    printf("  %-34s | %-24s | %-24s\n", "shape", "1 thread", "P threads (told sort has 2 serial passes)");
    const double F[] = { 0.0, 0.001, 0.01, 0.05 }; const double O[] = { 0.0, 1.0 / 9.0 };
    for (int oi = 0; oi < 2; oi++) for (int fi = 0; fi < 4; fi++) {
        RS = 900 + (uint64_t)(oi * 10 + fi); blocks(a, n, 64, O[oi], 0); memset(mask, 0, n);
        size_t ns = (size_t)(F[fi] * (double)n);
        for (size_t q = 0; q < ns; q++) { size_t x = nr() % n; mask[x] = 1; a[x] = fr() * (double)n * (1.0 + O[oi]); }
        double l1, h1, lp, hp; int ok1, okp;
        double c1 = pair_strays(a, mask, w, rest, st, n, 1, reps, &l1, &h1, &ok1), cp = pair_strays(a, mask, w, rest, st, n, P, reps, &lp, &hp, &okp);
        char nm[64]; snprintf(nm, sizeof nm, "%s, %.1f%% strays", oi ? "stairs x20" : "block_perm", 100.0 * F[fi]);
        printf("  %-34s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", nm, c1, l1, h1, ok1 ? "  " : " X", cp, lp, hp, okp ? "  " : " X"); fflush(stdout);
    }
    printf("\nRESULT: ratios only; every output checked ascending. The 0.0%% rows are the null for the told sort's own overhead.\n");
    return 0;
}
