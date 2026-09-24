/* What share of the wave route is the KNOT SWEEP?
 *
 * A trace build put the 1-thread 32-cycle wave at: detection ~40 ms, 65 piece sorts ~347 ms, sweep ~383 ms. Trace
 * builds cannot price a phase (their fprintf calls change the whole function's code generation), so this asks the
 * same question in a NORMAL build: LOGOS_WAVE_NOSWEEP=1 runs everything up to the sweep and stops, leaving sorted
 * pieces -- wrong output by design, exactly as ./unknotprobe nocopy prices the copy-back.
 *
 * The switch is read with getenv inside the route, so the two variants ALTERNATE inside one process on identical
 * data: drift cancels within a pair, and the order is swapped every pair so the ordering bias cancels across them.
 * Reports the median ratio; the full side's output is verified, the skipped side's cannot be.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "logos_psort.h"

static uint64_t RS = 12345;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* unknotprobe.c's gen_sine, copied verbatim (seed 777, as its WAVE rows use). A first cut invented its own sine --
   a value-ranged trend with positional swaps -- and the route DECLINED it, so both sides ran the same fallback and
   the probe reported a meaningless 1.6%. The witness below is what caught that; the shape has to be the real one. */
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static void gen_sine(double *a, size_t n, int cycles, double swing, double noise, uint64_t seed) {
    RS = seed; double T = (double)n / (double)cycles, A = swing * T / 2.0;
    for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (noise > 0 ? (fr() * 2.0 - 1.0) * noise : 0.0);
}

static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return a < b ? -1 : a > b; }

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atof(argv[1]) : 8388608;
    int pairs = argc > 2 ? atoi(argv[2]) : 15;
    int cycles = argc > 3 ? atoi(argv[3]) : 32;
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) { printf("no memory\n"); return 2; }
    gen_sine(base, n, cycles, 0.6, 50.0, 777);
    memcpy(ref, base, n * 8); qsort(ref, n, 8, cmp_d);

    double *rat = malloc((size_t)pairs * sizeof(double));
    double sum_full = 0, sum_skip = 0;
    int bad = 0, skipped_but_sorted = 0;
    for (int p = 0; p < pairs; p++) {
        double tf, ts;
        for (int half = 0; half < 2; half++) {
            int do_full = (p & 1) ? (half == 1) : (half == 0);     /* alternate which side goes first */
            memcpy(w, base, n * 8);
            if (do_full) unsetenv("LOGOS_WAVE_NOSWEEP"); else setenv("LOGOS_WAVE_NOSWEEP", "1", 1);
            logos_release_buffers();
            double t0 = ms(); logos_sort_f64(w, n, 1); double d = ms() - t0;
            /* The route must FIRE, or both sides run the same fallback and the ratio is 1.0 for the wrong reason.
               Two witnesses: logos_twist_last(8) counts wave calls, and with the sweep skipped the output MUST be
               unsorted -- if it comes back sorted, the sweep never ran because the route never ran. */
            if (do_full) { tf = d; if (memcmp(w, ref, n * 8) != 0) bad++; }
            else { ts = d; int srt = 1; for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { srt = 0; break; }
                   if (srt) skipped_but_sorted++; }
        }
        rat[p] = tf / ts; sum_full += tf; sum_skip += ts;
    }
    unsetenv("LOGOS_WAVE_NOSWEEP");
    qsort(rat, (size_t)pairs, sizeof(double), cmp_d);
    double med = rat[pairs / 2];
    printf("n = %zu, %d cycles, %d pairs, one thread%s\n", n, cycles, pairs, bad ? "   <-- FULL SIDE WRONG" : "");
    printf("  WITNESS: wave route fired %zu times; no-sweep side came back SORTED %d of %d times%s\n",
           logos_twist_last(8), skipped_but_sorted, pairs,
           (logos_twist_last(8) == 0 || skipped_but_sorted == pairs)
               ? "   <-- ROUTE NEVER RAN: the ratio below is meaningless" : "   (0 sorted = the sweep really was skipped)");
    printf("  full route          %8.2f ms  (mean)\n", sum_full / pairs);
    printf("  everything but the sweep %8.2f ms  (mean, output wrong by design)\n", sum_skip / pairs);
    printf("  full / no-sweep     %8.3f  (median of pairs; p10 %.3f p90 %.3f)\n", med, rat[pairs / 10], rat[pairs - 1 - pairs / 10]);
    printf("  => the SWEEP is %.1f%% of the route\n", 100.0 * (1.0 - 1.0 / med));
    free(base); free(w); free(ref); free(rat);
    return 0;
}
