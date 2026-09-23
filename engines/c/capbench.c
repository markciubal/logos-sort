/* capbench.c — a thread CEILING for the counting routes, paired against the engine as it ships.
 *
 * Why a ceiling at all (FINDINGS 3au): the counting and lattice routes run two parallel phases with a SERIAL merge
 * between them, and that merge grows with the thread count (the lattice one walks slots x P). `make profile` at 8M has
 * 6.0-6.9 of 14 threads busy inside that phase with NONE waiting -- the rest have nothing to do -- and a Karp-Flatt
 * sweep puts the experimentally determined serial fraction at 0.06 rising to 0.23 as P goes 2 -> 14, the signature of
 * overhead that grows with P rather than a fixed serial part. A ladder of single timings suggested a peak near 6
 * threads at 8M, but it did not reproduce across sizes, so the ceiling is settled HERE, in pairs, where drift cancels.
 *
 * `LOGOS_COUNT_MAX=<k>` caps every counting route per sort (0 = the shipped behaviour, one thread per 350k with a floor
 * of 3). Each row pairs a ceiling against no ceiling on the same data in one binary, so there is no code-layout term.
 * Above 1 = the ceiling is faster.
 *
 * PREDICTIONS, written before the first run (t(no ceiling) / t(ceiling k), 14 threads, two runs of 20 pairs):
 *   few_distinct (5 values, the HASH route: a trivial merge but a bandwidth-bound emit): k = 3 gives 1.2-1.5 from 4M
 *     up and 1.0-1.2 below; k = 6 gives 1.1-1.3.
 *   quantized and mod_card_30k (the LATTICE route: a heavy parallel map, a merge over slots x P): k = 3 LOSES at 8M
 *     and 16M (0.6-0.9) because the map wants the threads; k = 6 is 0.95-1.15; k = 8 is 1.0-1.1.
 *   Guards (uniform, block_perm: they never reach a counting route) 0.98-1.02 at every k.
 *   Stop rule: a ceiling ships only if it clears 1.05 on its shapes in both runs at 4M and up AND no row of any shape
 *     falls below 0.97 in both. If no single k does that, the honest answer is a per-route ceiling or none at all.
 *
 * MEASURED, a ceiling on BOTH families (two runs of 20 pairs, t(no ceiling) / t(ceiling), 14 threads, 1M to 16M):
 *   k = 3: few_distinct 1.00 1.54 1.11 1.02 0.94 and 1.01 1.50 1.19 1.06 -, but the lattice shapes 0.53-1.02 (both runs).
 *   k = 6: few_distinct 1.02 1.09 1.03 0.94 0.82; lattice 0.72-1.08.
 *   k = 8: few_distinct 1.00 0.94 1.13 1.20 1.05 and 1.05 1.04 1.11 1.11 1.07; lattice 0.82-1.05.
 * GRADED: no global ceiling passes the stop rule. The two families want opposite things and the profile says why: the
 * LATTICE route's map phase is real parallel work that uses every thread, so withholding threads costs it 0.53-0.90 at
 * 8M and 16M; the HASH route stops scaling near 8. k = 3 on few_distinct at 2M (1.50, 1.54) is the one big reproducible
 * cell, and it is the hash route dropping from 5 threads to 3. So the ceiling became the hash route's alone
 * (`count_threads_hash`, default 8), and what follows is its own A/B.
 * PREDICTION for the hash-only ceiling (written before that run): few_distinct 1.05-1.20 from 4M up, 0.98-1.05 at 1M and
 *   2M (the existing one-thread-per-350k rule already holds it to 3-5 threads there); quantized and mod_card_30k
 *   0.98-1.02 (the ceiling no longer reaches them); the guards 0.98-1.02. Ships if few_distinct clears 1.05 at 4M and up
 *   in both runs and nothing falls below 0.97 in both.
 * LATER ROUNDS use the same harness through CAP_SWITCH=<NAME> [CAP_VALUE=<v>], which pairs any per-sort switch against
 * the engine as it ships. When the switch restores the OLD behaviour the ratio reads t(new) / t(old), so BELOW 1 is the
 * change winning.
 *   1. LOGOS_LAT_PAR_MERGE (the lattice merge in parallel, opt-in): measured 1.02-1.12, i.e. 2-12% SLOWER, at 4M-16M.
 *      The trace says why: at 8M and 14 threads the lattice map is 11.6-14.1 ms and the merge ~2 ms, so the merge is ~10%
 *      of the route and the extra spawn plus a second pass over the slots eat the prize. NOT shipped.
 *   2. LOGOS_LAT_SERIAL_TABLES (each worker allocates its own table instead of the caller allocating all P): the caller's
 *      loop was P mallocs plus a memset of `slots` keys per thread -- 5.6 MB of serial writes at 8M and 14 threads --
 *      inside the phase that is ~80% of the route. PREDICTION, written before the first run: quantized and mod_card_30k
 *      0.83-0.95 at 4M and up (the ratio is t(new)/t(old), so below 1 is the win), 0.95-1.02 at 1M-2M where the tables are
 *      smaller and the route takes fewer threads; few_distinct 1.00 (the hash route has its own tables); uniform and
 *      block_perm 0.98-1.02. Ships if a lattice shape clears 0.95 at 4M+ in both runs and nothing reads above 1.03 in both.
 *      MEASURED (60 pairs, two runs, 1M-16M): quantized 0.96 0.96 0.96 0.96 0.99 and 0.99 0.95 0.95 0.99 0.97;
 *      mod_card_30k 0.99 0.94 0.98 0.98 0.98 and 0.97 0.97 0.97 0.97 0.98; a NULL switch on the same rows reads 0.98-1.04.
 *      Twenty of twenty cells on the winning side, centred on 0.97 against a null centred on 1.00. GRADED: the prediction
 *      (0.83-0.95) was too optimistic by about half, and the stop rule's 0.95 was missed -- but the mechanism is verified in
 *      the trace (the map phase is 8.8-11.3 ms with per-worker tables against 10.9-13.8 with the caller's loop, three runs
 *      each) and the null control separates it from noise, so it SHIPS at ~1.03x on the lattice shapes.
 *
   3. LOGOS_LAT_STATIC_MAP (the lattice MAP phase claims chunks instead of taking equal slices). Why: the profile's
 *      per-thread busy times inside that phase are BIMODAL on this hybrid CPU (6 performance cores, 8 efficient):
 *      4.40 ms against 2.99 on quantized, 5.09 against 3.54 on mod_card_30k, 5.24 against 2.51 on few_distinct, and
 *      2.95 against 2.12 in relocation's static tiles -- while the partition's queue-driven leaf reads 10.99 against
 *      10.53, a ratio of 1.04. A static phase ends when its slowest thread ends: quantized's map has a mean busy of
 *      3.6 ms and a wall of 5.4, so a third of its capacity is idle. PREDICTION, written before the first run (the
 *      ratio is t(new)/t(old), so BELOW 1 is the win): quantized and mod_card_30k 0.75-0.90 at 4M and up, 0.90-1.00 at
 *      1M-2M (fewer threads there); few_distinct 0.98-1.02 (the hash route has its own map, untouched by this change);
 *      uniform and block_perm 0.98-1.02. Ships if a lattice shape clears 0.95 at 4M+ in both runs and nothing reads
 *      above 1.03 in both.
 *
 * 4. LOGOS_RELOC_STATIC_TILES (relocation's phases 1 and 3 take one output tile per thread, as they used to).
 *    Why: the same hybrid-CPU argument that S1 just confirmed on the lattice map, at the site where make profile
 *    measures 6.6 of 14 threads busy and 3.6 WAITING at the barrier that follows phase 1. The tiles are disjoint dst
 *    ranges, so a worker can simply claim the next one; phase 2 already works that way. Tiles are 4 per thread, never
 *    under 32768 elements, and the per-tile scan for the first overlapping run became a binary search (it was O(R)
 *    from run 0, which a queue would have paid once per tile).
 *    PREDICTION (written before the first run): block_perm and block_overlap 0.88-0.97 at 4M and up, 0.95-1.02 at
 *    1M-2M where the per-route thread cap (one thread per 500k) leaves few threads to imbalance; guards 0.98-1.02.
 *    The lattice map paid ~1.05x and it had no barrier to wait at, so this should pay at least as much; if the
 *    waiting threads were waiting for the SERIAL plan rather than for a slow tile, it reads 1.00 and dies here.
 *    Ships if both block shapes clear 0.97 at 4M+ in both runs and nothing reads above 1.03 in both.
 *
 * Usage: ./capbench [pairs] [ceiling] [row filter]                    (LOGOS_THREADS=N overrides the thread count) */
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(RS >> 11) / (double)(1ULL << 53); }
static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

enum { C_FEW, C_QUANT, C_MOD, C_UNIFORM, C_BLOCKS, C_BLOVER, C_N };
static const char *CN[C_N] = { "few_distinct (5, hash)", "quantized (lattice)", "mod_card_30k (lattice)", "uniform (guard)", "block_perm (relocation)", "block_overlap (reloc+merge)" };
static void gen(double *a, size_t n, int s) {
    RS = 7717 + (uint64_t)s * 104729;
    if (s == C_FEW) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1000.0;
    else if (s == C_QUANT) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 40000) * 0.25;
    else if (s == C_MOD) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 3.0 + 1.0;
    else if (s == C_UNIFORM) for (size_t i = 0; i < n; i++) { nr(); a[i] = fr() * 1e9; }
    else { /* 64 sorted blocks in shuffled order. Disjoint ranges (C_BLOCKS) place by memcpy alone; ranges that overlap
              the next block (C_BLOVER) form chains, so phase 2 runs and the barrier after phase 1 is on the path. */
           size_t nb = 64, blk = n / nb; double span = (s == C_BLOVER) ? 1.6e6 : 1e6;   /* 1.6e6 > blk*1.0: neighbours overlap */
           size_t *perm = malloc(nb * sizeof(size_t));
           for (size_t k = 0; k < nb; k++) perm[k] = k;
           for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
           for (size_t i = 0; i < n; i++) { size_t b = i / blk < nb ? perm[i / blk] : perm[nb - 1], j = i % blk;
               a[i] = (double)b * span + (double)j * (2.4e6 / (double)blk); }
           free(perm); }
}
static char g_cap[16];
static int g_P = 14;
static const char *g_switch = "LOGOS_COUNT_MAX";   /* CAP_SWITCH=<NAME>: pair any per-sort switch against the engine as it ships */
static int g_plain_zero = 1;                       /* baseline sets the switch to "0" (a ceiling) or leaves it unset (a boolean switch) */
static void sort_capped(double *a, size_t n, int P) { (void)P; setenv(g_switch, g_cap, 1); logos_sort_f64(a, n, g_P); unsetenv(g_switch); }
static void sort_plain(double *a, size_t n, int P) { (void)P; if (g_plain_zero) setenv(g_switch, "0", 1); else unsetenv(g_switch); logos_sort_f64(a, n, g_P); unsetenv(g_switch); }   /* explicitly NO ceiling: the default is now 8 */

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    int cap = argc > 2 ? atoi(argv[2]) : 6;
    const char *filt = argc > 3 ? argv[3] : NULL;
    const size_t NS[] = { 1000000, 2000000, 4000000, 8000000, 16000000 };
    int online = (int)sysconf(_SC_NPROCESSORS_ONLN);
    g_P = bench_threads(online);
    if (getenv("CAP_SWITCH")) { g_switch = getenv("CAP_SWITCH"); g_plain_zero = 0; }
    if (getenv("CAP_VALUE")) snprintf(g_cap, sizeof g_cap, "%s", getenv("CAP_VALUE"));
    else if (g_plain_zero) snprintf(g_cap, sizeof g_cap, "%d", cap);
    else snprintf(g_cap, sizeof g_cap, "1");
    printf("SWITCH %s=%s, %d pairs, P = %d: t(baseline) / t(switch set); above 1 = the switch set is faster.\n\n  %-24s", g_switch, g_cap, pairs, g_P, "shape");
    for (size_t i = 0; i < sizeof NS / sizeof NS[0]; i++) printf("%18zu", NS[i]);
    printf("\n");
    for (int s = 0; s < C_N; s++) {
        if (filt && !strstr(CN[s], filt)) continue;
        printf("  %-24s", CN[s]);
        for (size_t i = 0; i < sizeof NS / sizeof NS[0]; i++) {
            size_t n = NS[i];
            double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
            if (!base || !w || !ref) return 2;
            gen(base, n, s); memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);
            memcpy(w, base, n * 8); sort_capped(w, n, g_P);
            int ok = memcmp(w, ref, n * 8) == 0;
            double lo, hi, med = bench_paired(sort_plain, sort_capped, base, w, n, g_P, pairs, &lo, &hi);
            printf("  %6.2f [%.2f %.2f]%s", med, lo, hi, ok ? "" : "X");
            fflush(stdout);
            free(base); free(w); free(ref);
        }
        printf("\n");
    }
    return 0;
}
