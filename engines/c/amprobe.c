/* amprobe.c — where the parallel speedup stops, and why (2026-09-22).
 *
 * Amdahl's law bounds a parallel sort by its serial fraction: S(P) <= 1 / (s + (1 - s)/P). This probe measures s
 * instead of assuming it, with the Karp-Flatt metric (below), and prints the thread count at which each route peaks.
 * Read it beside `make profile`, which gives the other half: how many threads are BUSY, WAITING at a barrier, or idle
 * inside each phase. Together they separate the three ways a route stops scaling -- a fixed serial part, overhead that
 * grows with P, and imbalance -- which want different fixes.
 *
 * What they found on this box at 8M (FINDINGS 3au): the partition is healthy (12.1 of 14 threads busy, e ~ 0.07); the
 * counting and lattice routes have 6.0-6.9 of 14 busy with NONE waiting, because a serial merge sits between two
 * parallel phases and the lattice one walks slots x P, so its serial part GROWS with P; relocation has 3.6 of 14
 * WAITING at a barrier (static output tiles); and kinked data sorts fastest on ONE thread at every P.
 *
 * Modes: default = the thread ladder with Karp-Flatt per shape. `ladder` = the counting routes across five sizes, which
 * is what a thread ceiling would have to be set from (it was measured and rejected: see capbench.c).
 *
 * Usage: ./amprobe [reps] [n] [ladder]                                        (min of `reps` per cell, one binary)
 *
 * Amdahl probe: one shape sorted at a ladder of thread counts, min of R reps each, with the Karp-Flatt metric
 *     e(P) = (1/S(P) - 1/P) / (1 - 1/P),   S(P) = T(1)/T(P)
 * which is the experimentally determined serial fraction. e roughly CONSTANT across P = a genuine Amdahl serial part;
 * e RISING with P = per-thread overhead (a merge whose cost grows with P, spawn cost, contention). Each row also prints
 * the thread count with the lowest time, which is where that route's speedup peaks. The route can change with P (the
 * dispatcher's gates are ratios of n and P), so the row prints logos_twist_last()'s route witnesses too. */
#include "logos_psort.h"
#include "bench_common.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t rs = 88172645463325252ULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double fr(void) { return (double)(nr() >> 11) * (1.0 / 9007199254740992.0); }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
enum { S_UNIFORM, S_FEWDIST, S_QUANT, S_MODCARD, S_BLOCKS, S_NEARSORTED, S_N };
static const char *SN[S_N] = { "uniform", "few_distinct (5)", "quantized (lattice)", "mod_card_30k", "block_perm (64)", "near_sorted (1%)" };
static void gen(double *a, size_t n, int s) {
    rs = 12345 + (uint64_t)s * 7919;
    if (s == S_UNIFORM) for (size_t i = 0; i < n; i++) a[i] = fr() * 1e9;
    else if (s == S_FEWDIST) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1000.0;
    else if (s == S_QUANT) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 40000) * 0.25;
    else if (s == S_MODCARD) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 3.0 + 1.0;
    else if (s == S_BLOCKS) {
        size_t nb = 64, blk = n / nb; size_t *perm = malloc(nb * sizeof(size_t));
        for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
        for (size_t i = 0; i < n; i++) { size_t b = i / blk < nb ? perm[i / blk] : perm[nb - 1], j = i % blk; a[i] = (double)b * 1e6 + (double)j; }
        free(perm);
    } else { for (size_t i = 0; i < n; i++) a[i] = (double)i * 0.5;
             for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } }
}
/* mode `ladder`: the counting routes' best thread count across sizes, which is what a cap must be set from */
static int ladder(int reps) {
    const size_t NS[] = { 1000000, 2000000, 4000000, 8000000, 16000000 };
    const int PS[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 14 }, NP = 10;
    const int SH[] = { S_FEWDIST, S_QUANT, S_MODCARD }, NSH = 3;
    printf("THE COUNTING ROUTES' THREAD LADDER: min of %d reps, ms. The cap must come from this table, not from one size.\n\n", reps);
    for (int si = 0; si < NSH; si++) {
        printf("  %-22s", SN[SH[si]]);
        for (int k = 0; k < NP; k++) printf("%8d", PS[k]);
        printf("   threads\n");
        for (size_t ni = 0; ni < sizeof NS / sizeof NS[0]; ni++) {
            size_t n = NS[ni];
            double *a = malloc(n * 8), *w = malloc(n * 8), *r = malloc(n * 8);
            if (!a || !w || !r) return 2;
            gen(a, n, SH[si]); memcpy(r, a, n * 8); qsort(r, n, 8, cmpd);
            double best = 1e300; int bestP = 1; double T[16];
            printf("    n = %-18zu", n);
            for (int k = 0; k < NP; k++) {
                double bt = 1e300;
                for (int q = 0; q < reps; q++) {
                    memcpy(w, a, n * 8);
                    double t0 = bench_now_(); logos_sort_f64(w, n, PS[k]); double el = bench_now_() - t0;
                    if (memcmp(w, r, n * 8)) { printf(" WRONG\n"); return 1; }
                    if (el < bt) bt = el;
                }
                T[k] = bt; if (bt < best) { best = bt; bestP = PS[k]; }
                printf("%8.2f", bt);
            }
            printf("   best %d (%.2fx over 14)\n", bestP, T[NP - 1] / best);
            fflush(stdout);
            free(a); free(w); free(r);
        }
        printf("\n");
    }
    return 0;
}
int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 9;
    size_t n = argc > 2 ? strtoull(argv[2], 0, 10) : 8000000;
    const int PS[] = { 1, 2, 3, 4, 6, 8, 10, 12, 14 };
    const int NP = (int)(sizeof PS / sizeof PS[0]);
    if (argc > 3 && !strcmp(argv[3], "ladder")) return ladder(reps);
    double *s = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!s || !w || !ref) return 2;
    printf("AMDAHL PROBE, n = %zu, min of %d reps per cell, one binary. T(P) in ms, S = T(1)/T(P),\n"
           "e = Karp-Flatt serial fraction (constant across P = a real serial part; rising = overhead that grows with P).\n\n", n, reps);
    for (int sh = 0; sh < S_N; sh++) {
        gen(s, n, sh); memcpy(ref, s, n * 8); qsort(ref, n, 8, cmpd);
        double t1 = 0, best = 1e300; int bestP = 1;
        printf("  %-22s", SN[sh]);
        for (int k = 0; k < NP; k++) printf("%8d", PS[k]);
        printf("   threads\n    %-20s", "T(P) ms");
        double T[16];
        for (int k = 0; k < NP; k++) {
            double bestrep = 1e300;
            for (int r = 0; r < reps; r++) {
                memcpy(w, s, n * 8);
                double t0 = bench_now_(); logos_sort_f64(w, n, PS[k]); double el = bench_now_() - t0;
                if (memcmp(w, ref, n * 8)) { printf(" WRONG"); return 1; }
                if (el < bestrep) bestrep = el;
            }
            T[k] = bestrep; if (k == 0) t1 = bestrep;
            if (bestrep < best) { best = bestrep; bestP = PS[k]; }
            printf("%8.2f", bestrep);
        }
        printf("\n    %-20s", "speedup S(P)");
        for (int k = 0; k < NP; k++) printf("%8.2f", t1 / T[k]);
        printf("\n    %-20s", "Karp-Flatt e(P)");
        for (int k = 0; k < NP; k++) {
            if (PS[k] == 1) { printf("%8s", "-"); continue; }
            double S = t1 / T[k], P = PS[k];
            printf("%8.3f", (1.0 / S - 1.0 / P) / (1.0 - 1.0 / P));
        }
        printf("\n    best: %d threads, %.2f ms (%.2fx over one thread)\n\n", bestP, best, t1 / best);
        fflush(stdout);
    }
    free(s); free(w); free(ref);
    return 0;
}
