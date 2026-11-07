/* sineprobe.c — a sine wave with a slope: what its structure IS, and whether that structure costs the sort anything.
 *
 * value(i) = i + A sin(2 pi i / T) + noise. A seasonal metric with drift, which is what run lengths that rise and fall
 * like a wave on a slope look like. Three invariants of the index -> value curve, read from an index-ordered sample:
 *   pieces  = monotone pieces of the SMOOTHED sample (direction changes + 1)
 *   depth   = how many pieces cover a value, averaged over the sample (1 = a staircase that never overlaps)
 *   reach   = noise amplitude / median local slope: how far an element sits from its place INSIDE its piece
 *
 * WHY THIS BENCH WAS REWRITTEN (2026-09-20). Its first cut printed one absolute time per shape and invited the reader to
 * compare them: "70 ms with noise against 54 ms uniform, so the shape is slower than random data". That is a comparison
 * ACROSS ROWS OF ONE RUN, which methodology rule 2 forbids, because rows are measured seconds apart on a box that wanders
 * by a multiple. The claim may still be true; the instrument could not establish it. Now every shape is measured against
 * a uniform reference ADJACENTLY, the order alternated across pairs, and the median ratio reported with p10/p90, which is
 * the same discipline as bench_paired() (the two sides sort different DATA rather than different code, so the null below
 * measures instance-to-instance variation as well as the machine).
 *
 * Two controls, both new, and both necessary to answer the question at all:
 *   - the NULL, a second uniform instance against the first. Everything it prints is this box's floor for this bench.
 *   - LINEAR + the same noise, no sine. If that is slow too, the noise is the cost and the wave is innocent.
 *   - the same VALUES with the index order destroyed. If that costs the same, the price is the value distribution and no
 *     route that reads the index order can collect it; if it costs nothing, the price is the order itself.
 * A number here is worth reading only if the null is near 1.000 and the row clears its p10/p90 band.
 *
 * PREDICTIONS, written before the first run of the rewritten bench, and GRADED against three runs:
 *   null (uniform vs uniform)      1.00 +-10%   ->  RIGHT: median 0.99-1.00 (the band itself was +-7 to +-25%, box-dependent)
 *   linear + noise 50, 1 thread    0.25-0.50    ->  WRONG, measured 0.59-0.63. I had read across from the short-reach route's
 *                                                   own A/B (2.05x on jitter, route on against route off) as if that were the
 *                                                   same quantity as shape-against-random. It is not.
 *   sine, no noise, 1 thread       0.25-0.45    ->  RIGHT: 0.32-0.35
 *   sine + noise 50, 1 thread      1.10-1.35    ->  RIGHT: 1.22-1.31, and the discarded across-row reading of 1.30 was right
 *                                                   after all, which was luck and not method
 *   every row at 14 threads        0.85-1.15    ->  WRONG on three rows of six: the structured shapes read 0.60-0.88 there,
 *                                                   faster than random rather than level. Only the noisy wave is level.
 *
 * WHAT THE THIRD CONTROL FOUND. The same VALUES with the index order destroyed read 1.01, level with random, where the natural
 * order reads 1.27: the cost is the ORDER, and this array is slower than the same numbers shuffled. A -DLOGOS_PROFILE build says
 * where it goes: the natural order takes the COARSE leaf, the shuffled multiset and uniform take the AVX2 quicksort leaf (126 ms
 * against 87 and 90 in that build). AVX2_TAKE requires !c->nearly, and sort_fn's 40 strided pairs call a sloped wave nearly
 * sorted because its slope makes a pair n/40 apart ascending almost always. The dense-disorder probe in the same function is
 * exactly the statistic that would overrule that, and today it gates only the insertion sort. Extending it is predicted ~1.45x
 * here and neutral on genuinely near-sorted shapes; unbuilt, and it needs its own paired A/B. See the CHANGELOG.
 *
 * Reads the route the short-reach gate took (logos_reach_last); for the full route per shape use the trace build,
 * `make sinetrace`, which prints routes and must never be timed (a trace build's fprintf changes code generation).
 *
 * MODE `ab [SWITCH]`, 2026-09-21: every shape against ITSELF with one environment switch set around one side (default
 * LOGOS_NO_DENSE_PROBE); the column is t(switch set) / t(shipped). Built to test the fix open direction 0i (f) predicted at 1.45x:
 * when the leaf's dense probe fires, stop calling the bucket nearly sorted, so that AVX2_TAKE admits it. MEASURED: 1.02 on the
 * noisy 32-cycle rows, 0.97-1.06 elsewhere. NEUTRAL, reverted. The prediction was wrong because the diagnosis was: the 126 ms
 * (coarse) against 87 (AVX2) that named the gate came from a PROFILE build, and paired in a normal build the two leaves cost the
 * same on this shape. Where the order cost really is: `LOGOS_NO_AVX2=1 ./sineprobe` reads the wave at 1.05 and its shuffle at
 * 1.05, against 1.19 and 1.01 with the AVX2 leaf on. The AVX2 quicksort is what is slower on this ORDER. Not traced further.
 * usage: ./sineprobe [pairs] [n] [ab [SWITCH]]   (a switch the engine reads once per process, like LOGOS_NO_AVX2, cannot be A/B'd this way)
 *
 * MODE `cos` (2026-09-22): what cos() in place of sin() does. The same waves, the same noise sequence (same seed), cos in place
 * of sin, sorted ADJACENTLY against the sin version, the order alternated: the column is t(cos) / t(sin), above 1 = the cosine
 * costs more. Beside it the route each version took at one thread (read from the witnesses: logos_twist_last, logos_reach_last,
 * logos_mirror_count) and the pieces each has. Shapes: the sloped waves of this bench and of the wave route's A/B, then waves
 * with NO slope (value = A sin or A cos, A = n/2), where a cosine starts at its crest and runs downhill first, and fractional
 * cycles, where the phase changes how many runs the array has. Mode `cosonce`: each version sorted once at 1 and P threads, for
 * `make sinetrace` (routes on stderr, never timed).
 * PREDICTIONS, written before the first run:
 *   With a slope, cos(x) = sin(x + pi/2): the same curve moved a quarter period, so the interior is identical and only the first and
 *   last pieces change length. Same routes, and t(cos)/t(sin) 0.97-1.03 at both thread counts on all five sloped rows.
 *   No slope, 4 whole cycles: eight monotone pieces each spanning the whole range, in both; cos's first piece is DESCENDING (a
 *   twisted strand) where sin's ascends. Same routes, 0.95-1.05.
 *   No slope, 1.5 cycles, no noise: sin has FOUR runs (up a quarter, down a half, up a half, down a quarter), cos has THREE (down,
 *   up, down), each spanning the whole range: cos 0.75-1.0 at one thread (one strand fewer to merge), 0.9-1.1 at P.
 *   No slope, a quarter cycle, no noise: sin is SORTED (the prescan returns), cos is sorted DESCENDING (the prescan, then one
 *   reversal pass): cos 1.5-3.0 at both thread counts.
 * MEASURED (4.2M, 40 pairs, two runs, run 1 / run 2): t(cos)/t(sin)
 *   wave                              route (both)   1 thread        P threads
 *   32 cycles, swing 0.6, no noise    knot           0.996 / 0.998   0.882 / 0.870
 *   32 cycles, swing 0.6, noise 50    wave           0.963 / 0.984   0.879 / 0.852
 *   8 cycles, swing 0.6, noise 50     wave           0.966 / 1.001   0.859 / 0.847
 *   32 cycles, swing 2, noise 50      knot           0.996 / 0.991   1.265 / 1.245
 *   2000 cycles, swing 0.6, noise 50  knot           0.963 / 0.979   1.009 / 1.015
 *   NO slope, 4 cycles, no noise      9 vs 8 runs    1.657 / 1.719   1.047 / 1.066
 *   NO slope, 4 cycles, noise 50      knot           0.951 / 0.988   1.052 / 1.040
 *   NO slope, 1.5 cycles, no noise    4 vs 3 runs    0.896 / 0.971   0.911 / 0.894
 *   NO slope, quarter cycle           sorted/reversed 1.739 / 1.769  1.702 / 1.742
 * GRADED. One thread with a slope RIGHT (0.96-1.00, same routes). P threads with a slope WRONG on four rows of five, and not because
 * of the phase: the partition's lookup. A sloped sine starts at ZERO, so a[0] (always a sample point) and a dozen values near it are
 * tiny or negative; in total-order key space the stretch around zero spans hundreds of binades, the key grid's cells go coarse
 * (shift 50 against 41), one bucket would take 21x its share, the grid is rejected and the scatter takes the value table (mode 1
 * against the cosine's key grid, mode 10): the cosine 0.85-0.88. At swing 2 both cross zero, both key grids fail, and the cosine's
 * uniformity test lands just past its limit (KS 0.0322 against 0.0273) where the sine's passes (0.0120): the cosine 1.25-1.27. No
 * slope, 4 cycles WRONG (predicted a tie, measured 1.66-1.72 at one thread): the cosine has 8 runs and the sine 9, and at <= 8 runs
 * the knot route's cost gate is 1.0n instead of 3.0n: it declined the cosine to the leaf and took the sine (mode `fewgate`). 1.5
 * cycles and the quarter cycle RIGHT.
 *
 * usage: ./sineprobe [pairs] [n] cos | cosonce
 *
 * MODE `fewgate` (2026-09-22, found by `cos`: a pure 4-cycle cosine, 8 runs, sorted 1.66-1.72x slower than the sine, 9 runs, at one
 * thread, because at <= MERGE_MAX_RUNS (8) runs the knot route's cost gate is 1.0n instead of 3.0n and it declined, where at 9 it
 * went). The gate was set from 4 interleaved runs (0.955 with the gate lifted); the knot route's own record has 8 strands at 1.8x
 * where the fallback was the run merge. Here: a pure cosine of h half-cycles (h runs, each spanning the whole range), h = 2..10,
 * with and without noise, the shipped engine against LOGOS_KNOT_FEW=3 (the few-runs gate alone at 3.0n; it was LOGOS_KNOT_MAX=3,
 * every gate, until 2026-09-26), same binary, ONE thread: the column is
 * t(shipped) / t(gate 3.0), above 1 = lifting the few-runs gate is faster. Two runs of 40 pairs.
 * PREDICTIONS, written before the first run: h = 2 and 3 1.00 (cost 1.0n and ~1.6n: two strands go anyway; three read 2.44x in
 * the gate's own record against the shipped engine, so 1.0-1.3); h = 4 0.93-1.00 (the 0.955 the gate was set from); h = 5-8 1.3-1.9
 * (the route took 8 strands 1.8x over the run merge); h = 9 and 10 1.00 (the 3.0n gate already applies). Noise 50 the same within
 * 10%, if the noisy runs are still found whole.
 * MEASURED (4.2M, 40 pairs, two runs): no noise, t(shipped)/t(gate 3.0): h = 2 0.998 / 0.988; 3 2.879 / 2.874; 4 2.173 / 2.229;
 * 5 2.245 / 2.313; 6 2.048 / 2.019; 7 2.060 / 2.051; 8 1.613 / 1.608; 9 1.017 / 1.006; 10 0.988 / 1.015. Noise 50: 0.94-1.02 on every
 * h (noise breaks the runs, so the gate never applies). GRADED: h = 2, 9, 10 and the noisy rows right; h = 3 and 4 WRONG (a tie
 * predicted, 2.2-2.9x measured) and 5-8 at or above the band. The 0.955 the gate was set from was four INTERLEAVED runs, where the
 * run merge really is the fallback; on these arrays the knot route's decline goes to the leaf, a full sort. NOT CHANGED here: the
 * question was what a cosine does. The gate wants its own A/B: 3.0n for every run count, against the four interleaved runs it was
 * set from and against the shapes the run merge takes.
 *
 * usage: ./sineprobe [pairs] [n] fewgate
 *
 * MODE `legs` (2026-09-22): "what if the descending leg is cos and the ascending leg is sin?" Read as a quarter period each: the
 * descending leg is A cos from its crest to zero, the ascending leg A sin from zero back to the crest, so one cycle is down then
 * up and the wave is |cos|: rounded crests, a CUSP at zero, every value in [0, A] (plus the slope) instead of [-A, A]. (Read as half a
 * period each, cos from crest to trough and sin from trough to crest, the legs join into the plain cosine wave of mode `cos`: the
 * same values, the same result.) Two orders: the cos leg first (the array starts at a crest) and the sin leg first (it starts at
 * ZERO). Columns: t(sin leg first) / t(cos leg first), and t(cos leg first) / t(the plain cosine wave of the same cycles), each at 1
 * and P threads, paired and alternated; the routes each order took at one thread.
 * PREDICTIONS, written before the first run:
 *   routes: the same as the sinusoid with the same number of monotone pieces (legs): the sloped waves by the wave route (noise) or
 *     the knot route (none); 4 cycles without a slope is 8 legs, so the few-runs knot gate declines both orders (as it declined the
 *     8-run cosine); 5 cycles is 10 legs and goes to the knot route.
 *   sin leg first / cos leg first, ONE thread: 0.97-1.03 on every row (the same legs; only the ends differ).
 *   sin leg first / cos leg first, P threads, WITH a slope: 1.10-1.20, the sin-cos gap of mode `cos` from the other side: starting
 *     at zero puts a[0] = 0 (always sampled) into the sample, the key grid's cells stretch across the binades around zero, it is
 *     rejected, and the scatter takes the table lookup. WITHOUT a slope every cusp touches zero in both orders: 0.95-1.05.
 *   cos leg first / plain cosine: 0.9-1.1 at both thread counts (the same number of legs and the same depth, half the range).
 * MEASURED (4.2M, 40 pairs, two runs): sin leg first / cos leg first, then cos leg first / plain cosine
 *   wave                              legs  route             1 thread        P threads       | 1 thread        P threads
 *   32 cycles, swing 0.6, no noise      1   prescan: sorted   1.016 / 1.007   0.962 / 0.999   | 0.144 / 0.131   0.220 / 0.189
 *   32 cycles, swing 0.6, noise 50      1   one strand        0.996 / 0.985   1.018 / 0.995   | 0.520 / 0.510   0.734 / 0.701
 *   NO slope, 4 cycles, no noise        8   leaf (declined)   1.058 / 1.002   1.106 / 1.069   | 1.063 / 1.024   0.832 / 0.827
 *   NO slope, 4 cycles, noise 50        8   leaf              0.985 / 1.031   1.102 / 1.105   | 1.015 / 1.009   0.883 / 0.857
 *   NO slope, 5 cycles, no noise       10   knot              1.088 / 1.082   1.173 / 1.171   | 0.998 / 0.980   0.802 / 0.836
 * GRADED. Routes WRONG on the sloped rows: a quarter-period leg stretched over half a cycle falls at most 0.3 pi per element against
 * the slope's 1, so with a slope the wave never descends: noise-free it is SORTED (the prescan returns: 0.13-0.14 of the cosine's
 * time), with noise it is one strand (0.51-0.52). The full sinusoid falls at 0.6 pi and does descend. Leg order at one thread right
 * (0.99-1.09). Leg order at P WRONG both ways: with a slope the waves never reach the partition (1.00-1.02, not 1.10-1.20); without
 * one, only a[0] is sure to be sampled, and the sin leg first puts a[0] = 0.0 there, so its key grid is rejected (one bucket 38x its
 * share) and the scatter takes the table: 1.07-1.17. The same mechanism makes the cos-first wave cheaper than the plain cosine at P
 * (0.80-0.88): the cosine's values straddle zero, the scalloped wave's never go below it.
 *
 * usage: ./sineprobe [pairs] [n] legs
 *
 * MODE `tilt` (2026-09-22): "is the increasing nature taken into account? Start at the min value, get the max value, interpolate the
 * slope and apply it on top of the scalloped wave." The rows above took a trend into account at ONE extreme only: value = i + wave,
 * a rise of 1 per element against a wave amplitude of 0.3 of each cycle's rise, so the trend won everywhere and the scalloped wave
 * came out sorted. The user's line is the other extreme: a trend from the wave's own minimum to its own maximum across the whole
 * array (a total rise of one wave range). Here the whole ladder: value = A wave(i) + r A i / n, A = n/2, wave = the scalloped
 * wave (cos leg down from the crest, sin leg up from zero) or the plain cosine, r stepped as: none; the user's line (r = the
 * wave's range over A: 1 for the scalloped wave, 2 for the cosine); 25%, 50%, 90% and 110% of r* = the trend at which the wave
 * stops falling anywhere (a leg falls at most A pi cycles / n per element, so r* = pi cycles for the scalloped wave and 2 pi cycles
 * for the cosine). Every shape is paired against uniform data over ITS OWN value range (so the ratio is not also a comparison of
 * ranges), the order alternated: the column is t(shape) / t(uniform), below 1 = the structure pays.
 * The engine has no trend model: it sees a trend only as runs that overlap less. The value-linear map a min-to-max interpolation
 * gives is what its flash leaf and its uniform grid already use.
 * PREDICTIONS, written before the first run:
 *   none and the user's line: the same route and cost within 5% (the line rises one wave range over the whole array, 1/8 of a
 *     range per leg at 4 cycles: every leg still falls and the runs overlap almost as deeply).
 *   25% and 50%: every leg still falls somewhere, so the run COUNT does not change, but consecutive runs overlap less and the
 *     knot route's crossing cost falls: the cost falls with it, in a STEP where a gate flips (the few-runs gate at 1.0n for the
 *     4-cycle waves, 3.0n for 32 cycles), not smoothly.
 *   90%: the falls shrink to short stretches near each leg's steepest point: runs of nearly the whole leg, overlapping only near
 *     the joins: 0.1-0.4 of uniform at one thread (the knot route or relocation).
 *   110%: monotone, SORTED (noise-free) or one strand (noise 50): 0.02-0.10 of uniform at one thread, 0.05-0.30 at P.
 *   The cosine behaves like the scalloped wave at the same fraction of its own r*.
 * MEASURED (4.2M, 40 pairs, two runs; t(shape)/t(uniform over its own range), 1 thread | P threads):
 *   trend             scalloped 4 cycles   scalloped 32 cycles        scalloped 32, noise 50     cosine 4 cycles      cosine 32, noise 50
 *   none              1.01 | 1.01-1.05     0.83-0.87 | 1.13-1.17      0.94-0.99 | 1.03-1.05      0.98 | 1.18-1.22     0.98-0.99 | 1.20-1.25
 *   min-to-max line   0.55-0.57 | 0.88-0.96 1.19-1.21 | 1.00-1.02     1.02-1.04 | 0.98           0.56-0.58 | 1.13-1.22 1.03-1.05 | 1.15-1.17
 *   25% of r*         0.35-0.38 | 0.77-0.79 0.38-0.39 | 0.93-0.99     1.21-1.24 | 1.07-1.08      0.36-0.37 | 0.89      1.14-1.25 | 0.84-0.91
 *   50% of r*         0.29-0.31 | 0.75-0.76 0.31-0.32 | 0.76-0.78     1.42-1.43 | 0.91-0.93      0.30-0.31 | 0.65-0.69 1.22-1.23 | 1.40-1.55
 *   90% of r*         0.20 | 0.52-0.54      0.18-0.19 | 0.52-0.59      0.99-1.01 | 1.39-1.54      0.20-0.21 | 0.60-0.61 1.10-1.19 | 1.51-1.58
 *   110% of r*        0.04 | 0.19-0.23      0.04 | 0.20-0.23           0.97 | 1.40-1.48           0.04 | 0.24-0.26      0.87-0.90 | 0.70-1.04
 * GRADED. Noise-free, the trend is worth what was predicted and more: the cost falls from ~1.0 of uniform to 0.2 at 90% of r* and
 * the array is sorted past it. The user's min-to-max line WRONG both ways: on 4 cycles it takes 1.0 to 0.55 (the wave starts at a
 * crest, where the leg is flat, so any rise makes a short first run: 9 runs, past the few-runs knot gate); on 32 noise-free cycles it
 * takes 0.85 to 1.2 (with no trend every cycle repeats the same values, 32 copies of each; the line makes them distinct, and the
 * wave's order then costs more than uniform, the one-thread order cost of open direction 0i (f)). Noise 50 WRONG everywhere: the
 * trend buys nothing at one thread and costs up to 1.5x uniform at P. Traced (make sinetrace, mode `tiltonce`):
 *   one thread, 50% of r*: the detector bails (noise), the WAVE route accepts 65 pieces (crossing 1.31, 1981 of 1983 pairs agree),
 *     the short-reach route declines every piece (near each turning point the curve is flat and the noise reaches past its blocks),
 *     and each piece re-enters the whole dispatcher (sample, gates, a detector scan that bails again, the leaf): 1.42x uniform.
 *   P threads, 90% of r*: the KS certificate passes (0.0111 against 0.0273), the partition takes the equi-width grid (mode 6), and
 *     the regions' 60% slack overflows where the wave bunches values: 631,108 spills, the histogram-free pass is thrown away and the
 *     histogram path runs after it: 1.39-1.54x. A KS distance of 1% of n does not bound one bucket's count within 1.6x at 168 buckets.
 *   P threads, 110% of r*: the near-sorted route accepts and sets aside 878,043 strays (21%), whose nested sort spills the same way
 *     (361,848): 1.40-1.48x.
 * None changed: the question was whether the trend is taken into account. Open directions, with these numbers.
 *
 * usage: ./sineprobe [pairs] [n] tilt
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

/* cycles = 0 and amp = 0 is a plain slope; uniform = 1 is the reference. The reference spans the same values as the
   others (0 .. n) so that the comparison is not also a comparison of value ranges. */
typedef struct { const char *nm; int cycles; double amp, noise; int uniform, shuffle; } shape_t;

static void gen(double *a, size_t n, const shape_t *s, uint64_t seed) {
    RS = seed;
    if (s->uniform) { for (size_t i = 0; i < n; i++) a[i] = fr() * (double)n; return; }
    double T = s->cycles > 0 ? (double)n / (double)s->cycles : 1.0, A = s->cycles > 0 ? s->amp * T / 2.0 : 0.0;
    for (size_t i = 0; i < n; i++) {
        double v = (double)i;
        if (s->cycles > 0) v += A * sin(6.283185307179586 * (double)i / T);
        if (s->noise > 0) v += (fr() * 2.0 - 1.0) * s->noise;
        a[i] = v;
    }
    /* the same multiset, index order destroyed: whatever this costs belongs to the VALUES, not to the order */
    if (s->shuffle) for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); double tmp = a[i]; a[i] = a[j]; a[j] = tmp; }
}

/* the three invariants, from 4096 index-ordered samples smoothed over 9 points */
static void invariants(const double *a, size_t n, int *pieces, double *depth, double *reach, double noise) {
    enum { M = 4096 };
    static double v[M], sm[M], sl[M]; static int start[M + 1];
    size_t st = n / M; if (!st) st = 1;
    for (int i = 0; i < M; i++) v[i] = a[(size_t)i * st < n ? (size_t)i * st : n - 1];
    for (int i = 0; i < M; i++) { double acc = 0; int c = 0;
        for (int d = -4; d <= 4; d++) if (i + d >= 0 && i + d < M) { acc += v[i + d]; c++; }
        sm[i] = acc / c; }
    int p = 1, dir = 0, np = 0; start[np++] = 0;
    for (int i = 1; i < M; i++) { int d2 = sm[i] > sm[i - 1] ? 1 : sm[i] < sm[i - 1] ? -1 : 0;
        if (d2 && dir && d2 != dir) { p++; start[np++] = i; } if (d2) dir = d2; }
    start[np] = M;
    double dp = 0;
    for (int i = 0; i < M; i += 16) { int cover = 0;
        for (int q = 0; q < np; q++) { double lo = 1e300, hi = -1e300;
            for (int k = start[q]; k < start[q + 1]; k++) { if (sm[k] < lo) lo = sm[k]; if (sm[k] > hi) hi = sm[k]; }
            if (sm[i] >= lo && sm[i] <= hi) cover++; }
        dp += cover; }
    for (int i = 1; i < M; i++) sl[i - 1] = fabs(sm[i] - sm[i - 1]) / (double)st;
    qsort(sl, M - 1, sizeof(double), cmpd);
    *pieces = p; *depth = dp / (M / 16);
    *reach = (noise > 0 && sl[(M - 1) / 2] > 0) ? 2 * noise / sl[(M - 1) / 2] : 0;
}

static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

/* mode `cos`: a wave with or without the slope, sin or cos; cycles may be fractional. Without a slope A = amp * n / 2, so the
   values span the same range as the sloped shapes. The noise draws are the same for sin and cos (same seed, one draw per element). */
typedef struct { const char *nm; double cycles, amp, noise; int slope; } wave_t;
static void gen_wave(double *a, size_t n, const wave_t *s, int use_cos, uint64_t seed) {
    RS = seed;
    double T = (double)n / s->cycles, A = s->slope ? s->amp * T / 2.0 : s->amp * (double)n / 2.0;
    for (size_t i = 0; i < n; i++) {
        double th = 6.283185307179586 * (double)i / T, v = s->slope ? (double)i : 0.0;
        v += A * (use_cos ? cos(th) : sin(th));
        if (s->noise > 0) v += (fr() * 2.0 - 1.0) * s->noise;
        a[i] = v;
    }
}
/* mode `legs`: descending leg A cos(pi/2 u/L) from the crest to zero, ascending leg A sin(pi/2 u/L) from zero to the crest, L = T/2
   each; sin_first shifts the cycle by one leg so that the array starts at zero going up. Same noise draws as gen_wave. */
static void gen_legs(double *a, size_t n, const wave_t *s, int sin_first, uint64_t seed) {
    RS = seed;
    double T = (double)n / s->cycles, L = T / 2.0, A = s->slope ? s->amp * T / 2.0 : s->amp * (double)n / 2.0;
    for (size_t i = 0; i < n; i++) {
        double pos = fmod((double)i + (sin_first ? L : 0.0), T);
        double leg = pos < L ? cos(1.5707963267948966 * pos / L) : sin(1.5707963267948966 * (pos - L) / L);
        double v = (s->slope ? (double)i : 0.0) + A * leg;
        if (s->noise > 0) v += (fr() * 2.0 - 1.0) * s->noise;
        a[i] = v;
    }
}
/* mode `tilt`: A wave(i) + r A i / n, A = n/2; wave = the scalloped wave (cos leg first) or the plain cosine */
static void gen_tilt(double *a, size_t n, double cycles, int scallop, double r, double noise, uint64_t seed) {
    RS = seed;
    double T = (double)n / cycles, L = T / 2.0, A = (double)n / 2.0;
    for (size_t i = 0; i < n; i++) {
        double wv;
        if (scallop) { double pos = fmod((double)i, T); wv = pos < L ? cos(1.5707963267948966 * pos / L) : sin(1.5707963267948966 * (pos - L) / L); }
        else wv = cos(6.283185307179586 * (double)i / T);
        double v = A * wv + r * A * ((double)i / (double)n);
        if (noise > 0) v += (fr() * 2.0 - 1.0) * noise;
        a[i] = v;
    }
}
/* the route one 1-thread sort took, from the witnesses (each read clears its count) */
static const char *route_of(const double *src, double *w, size_t n, char *buf) {
    int asc = 1, desc = 1; for (size_t i = 1; i < n && (asc || desc); i++) { if (src[i] < src[i - 1]) asc = 0; if (src[i] > src[i - 1]) desc = 0; }
    if (asc) { strcpy(buf, "prescan: sorted"); return buf; }
    if (desc) { strcpy(buf, "prescan: reversed"); return buf; }
    (void)logos_twist_last(0); (void)logos_twist_last(1); (void)logos_twist_last(7); (void)logos_twist_last(8); (void)logos_twist_last(9);
    (void)logos_twist_last(12); (void)logos_mirror_count(1);
    memcpy(w, src, n * sizeof(double)); logos_sort_f64(w, n, 1);
    int rs = logos_reach_last();
    size_t k0 = logos_twist_last(0), k1 = logos_twist_last(1), lb = logos_twist_last(7), wv = logos_twist_last(8), sm = logos_twist_last(9),
           one = logos_twist_last(12), mr = logos_mirror_count(1);
    buf[0] = 0;
    if (mr) strcat(buf, "mirror+");
    if (lb) strcat(buf, "loop-back+");
    if (sm) strcat(buf, "seam+");
    if (wv) strcat(buf, "wave"); else if (one) strcat(buf, "one strand"); else if (k0) strcat(buf, "knot"); else if (k1) strcat(buf, "relocation");
    else if (rs == 1 || rs == 2) strcat(buf, "short reach"); else strcat(buf, "leaf / other");
    return buf;
}

/* the shape under test against the reference, ADJACENTLY, the order alternated. Returns the median of t_shape/t_ref,
   so above 1 means the shape costs more than random data of the same size. */
static const char *ENV_SHAPE = NULL;      /* mode ab: set around the SHAPE side's sort only */
static double pair(const double *shape, const double *ref, double *w, size_t n, int P, int reps,
                   double *p10, double *p90, int *ok) {
    double *r = (double *)malloc((size_t)reps * sizeof(double));
    if (!r) { *ok = 0; return 0.0; }
    *ok = 1;
    for (int i = 0; i < reps; i++) {
        double ta = 0, tb = 0, s;
        const double *first = (i & 1) ? ref : shape, *second = (i & 1) ? shape : ref;
        double *tf = (i & 1) ? &tb : &ta, *ts = (i & 1) ? &ta : &tb;
        if (ENV_SHAPE && first == shape) setenv(ENV_SHAPE, "1", 1);
        memcpy(w, first, n * sizeof(double));  s = now_ms(); logos_sort_f64(w, n, P); *tf = now_ms() - s;
        if (ENV_SHAPE) unsetenv(ENV_SHAPE);
        if (!sorted_ok(w, n)) *ok = 0;
        if (ENV_SHAPE && second == shape) setenv(ENV_SHAPE, "1", 1);
        memcpy(w, second, n * sizeof(double)); s = now_ms(); logos_sort_f64(w, n, P); *ts = now_ms() - s;
        if (ENV_SHAPE) unsetenv(ENV_SHAPE);
        if (!sorted_ok(w, n)) *ok = 0;
        r[i] = tb > 0 ? ta / tb : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    if (p10) *p10 = r[(int)(0.10 * reps)];
    if (p90) *p90 = r[(int)(0.90 * reps)];
    free(r);
    return med;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40;
    size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : (size_t)1 << 22;
    int P = bench_threads(logos_num_cpus());
    (void)bench_paired;   /* bench_common.h's pairing is for two CODE variants on one array; here the two sides sort
                             different DATA, so pair() below does the same alternation with two bases. */
    if (reps < 4) reps = 4;
    static const char *ST[] = { "not tried", "SORTED by short reach", "sorted, strays shifted", "declined: gate",
                                "declined: probe", "declined: plan", "declined: no memory" };
    const shape_t S[] = {
        { "uniform, a 2nd instance (THE NULL)", 0, 0, 0, 1, 0 },
        { "linear + noise 50, no wave       ", 0, 0, 50, 0, 0 },
        { "32 cycles, swing 0.6, no noise   ", 32, 0.6, 0, 0, 0 },
        { "32 cycles, swing 0.6, noise 50   ", 32, 0.6, 50, 0, 0 },
        { "  the same values, SHUFFLED      ", 32, 0.6, 50, 0, 1 },
        { "32 cycles, swing 2, noise 50     ", 32, 2.0, 50, 0, 0 },
        { "32 cycles, swing 8, noise 50     ", 32, 8.0, 50, 0, 0 },
        { "2000 cycles, swing 0.6, noise 50 ", 2000, 0.6, 50, 0, 0 },
    };
    const int NS = (int)(sizeof S / sizeof S[0]);
    double *ref = (double *)malloc(n * sizeof(double)), *b = (double *)malloc(n * sizeof(double)),
           *w = (double *)malloc(n * sizeof(double));
    if (!ref || !b || !w) { printf("no memory for %zu elements\n", n); return 2; }
    int ab = argc > 3 && !strcmp(argv[3], "ab"); double *ref2 = ab ? (double *)malloc(n * sizeof(double)) : NULL;
    if (ab) { if (!ref2) return 2; ENV_SHAPE = argc > 4 ? argv[4] : "LOGOS_NO_DENSE_PROBE";
        printf("MODE ab: every column below is t(the switch set) / t(shipped) on the SAME array, not shape against uniform. Above 1 = shipped is faster.\n\n"); }
    if (argc > 3 && !strcmp(argv[3], "tiltonce")) {           /* for make sinetrace: the noisy 32-cycle scalloped rows at 50%, 90% and 110% of r*, once each */
        const double F[] = { 0.5, 0.9, 1.1 };
        for (int k = 3; k <= 5; k++) { double r = F[k - 3] * 3.141592653589793 * 32.0; gen_tilt(b, n, 32, 1, r, 50, 6161 + 2 * 7 + (uint64_t)k);
            for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1; fprintf(stderr, "#### scalloped 32 cycles noise 50, %.0f%% of r*, %d thread(s)\n", F[k - 3] * 100.0, T);
                memcpy(w, b, n * sizeof(double)); logos_sort_f64(w, n, T); } }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "tilt")) {
        struct { const char *nm; double cycles; int scallop; double noise; } C[] = {
            { "scalloped, 4 cycles, no noise ", 4, 1, 0 }, { "scalloped, 32 cycles, no noise", 32, 1, 0 }, { "scalloped, 32 cycles, noise 50", 32, 1, 50 },
            { "cosine, 4 cycles, no noise    ", 4, 0, 0 }, { "cosine, 32 cycles, noise 50   ", 32, 0, 50 } };
        const char *LAB[] = { "no trend", "min-to-max line", "25% of r*", "50% of r*", "90% of r*", "110% of r*" };
        double *u2 = (double *)malloc(n * sizeof(double)); if (!u2) return 2;
        printf("A TREND UNDER THE WAVE, n = %zu, %d pairs, P = %d: value = A wave(i) + r A i / n, A = n/2. r* = the trend at which the wave\n"
               "stops falling (pi cycles for the scalloped wave, 2 pi cycles for the cosine). Each shape paired with uniform data over its own\n"
               "value range, order alternated: t(shape) / t(uniform), median [p10 p90]; below 1 = the structure pays.\n\n", n, reps, P);
        printf("  %-31s %-16s %8s %6s | %-16s | %-22s %s\n", "wave", "trend", "r", "pieces", "route (1T)", "1 thread", "P threads");
        for (size_t ci = 0; ci < sizeof C / sizeof C[0]; ci++) for (int k = 0; k < 6; k++) {
            double rs = (C[ci].scallop ? 3.141592653589793 : 6.283185307179586) * C[ci].cycles;
            double r = k == 0 ? 0.0 : k == 1 ? (C[ci].scallop ? 1.0 : 2.0) : k == 2 ? 0.25 * rs : k == 3 ? 0.5 * rs : k == 4 ? 0.9 * rs : 1.1 * rs;
            gen_tilt(b, n, C[ci].cycles, C[ci].scallop, r, C[ci].noise, 6161 + ci * 7 + (uint64_t)k);
            double lo = b[0], hi = b[0]; for (size_t i = 1; i < n; i++) { if (b[i] < lo) lo = b[i]; if (b[i] > hi) hi = b[i]; }
            RS = 999 + ci * 7 + (uint64_t)k; for (size_t i = 0; i < n; i++) u2[i] = lo + fr() * (hi - lo);
            int pc; double dp, rc; char r0[64];
            invariants(b, n, &pc, &dp, &rc, C[ci].noise); route_of(b, w, n, r0);
            double l1, h1, lP, hP; int o1 = 0, oP = 0;
            double m1 = pair(b, u2, w, n, 1, reps, &l1, &h1, &o1), mP = pair(b, u2, w, n, P, reps, &lP, &hP, &oP);
            printf("  %-31s %-16s %8.2f %6d | %-16s | %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s\n", C[ci].nm, LAB[k], r, pc, r0,
                   m1, l1, h1, o1 ? "    " : " BAD", mP, lP, hP, oP ? "" : " BAD");
            fflush(stdout);
        }
        free(u2);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "legs")) {
        const wave_t W[] = {
            { "32 cycles, swing 0.6, no noise   ", 32, 0.6, 0, 1 },
            { "32 cycles, swing 0.6, noise 50   ", 32, 0.6, 50, 1 },
            { "NO slope, 4 cycles, no noise     ", 4, 1.0, 0, 0 },
            { "NO slope, 4 cycles, noise 50     ", 4, 1.0, 50, 0 },
            { "NO slope, 5 cycles, no noise     ", 5, 1.0, 0, 0 },
        };
        const int NW = (int)(sizeof W / sizeof W[0]);
        double *c = (double *)malloc(n * sizeof(double)), *d = (double *)malloc(n * sizeof(double)); if (!c || !d) return 2;
        printf("DESCENDING LEG cos, ASCENDING LEG sin (|cos|: a cusp at zero), n = %zu, %d pairs, P = %d. Paired, order alternated.\n"
               "Columns: t(sin leg first) / t(cos leg first), then t(cos leg first) / t(plain cosine wave); above 1 = the first-named costs more.\n\n", n, reps, P);
        printf("  %-34s %5s | %-18s %-18s | %-22s %-22s | %-22s %s\n", "wave", "legs", "route, cos first", "route, sin first",
               "sin 1st / cos 1st, 1T", "P threads", "cos 1st / cosine, 1T", "P threads");
        for (int s = 0; s < NW; s++) {
            gen_legs(b, n, &W[s], 0, 5151 + (uint64_t)s); gen_legs(c, n, &W[s], 1, 5151 + (uint64_t)s); gen_wave(d, n, &W[s], 1, 5151 + (uint64_t)s);
            int pb, pc; double dp, rc; char r0[64], r1[64];
            invariants(b, n, &pb, &dp, &rc, W[s].noise); invariants(c, n, &pc, &dp, &rc, W[s].noise); (void)pc;
            route_of(b, w, n, r0); route_of(c, w, n, r1);
            double l1, h1, lP, hP, l2, h2, l3, h3; int o1 = 0, oP = 0, o2 = 0, o3 = 0;
            double m1 = pair(c, b, w, n, 1, reps, &l1, &h1, &o1), mP = pair(c, b, w, n, P, reps, &lP, &hP, &oP);
            double m2 = pair(b, d, w, n, 1, reps, &l2, &h2, &o2), m3 = pair(b, d, w, n, P, reps, &l3, &h3, &o3);
            printf("  %-34s %5d | %-18s %-18s | %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s\n",
                   W[s].nm, pb, r0, r1, m1, l1, h1, o1 ? "" : " BAD", mP, lP, hP, oP ? "" : " BAD", m2, l2, h2, o2 ? "" : " BAD", m3, l3, h3, o3 ? "" : " BAD");
            fflush(stdout);
        }
        free(c); free(d);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "legsonce")) {
        const wave_t W1[] = { { "32 cycles, swing 0.6, noise 50   ", 32, 0.6, 50, 1 }, { "NO slope, 4 cycles, no noise     ", 4, 1.0, 0, 0 } };
        for (int s = 0; s < 2; s++) for (int k = 0; k < 2; k++) {
            gen_legs(b, n, &W1[s], k, 5151 + (uint64_t)(s ? 2 : 1));
            for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1; fprintf(stderr, "#### %s %s, %d thread(s)\n", W1[s].nm, k ? "SIN LEG FIRST" : "cos leg first", T);
                memcpy(w, b, n * sizeof(double)); logos_sort_f64(w, n, T); }
        }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "fewgate")) {
        printf("THE KNOT ROUTE'S FEW-RUNS GATE, n = %zu, %d pairs, ONE thread: a pure cosine of h half-cycles (h runs, each spanning the range).\n"
               "Column: t(shipped) / t(LOGOS_KNOT_FEW=3: the few-runs gate ALONE at 3.0n; h > 8 rows are guards), same array; above 1 = lifting it is faster.\n\n", n, reps);
        printf("  %-4s %-8s %-18s %-18s | %s\n", "h", "noise", "route, shipped", "route, gate 3.0", "t(shipped)/t(gate 3.0) [p10 p90]");
        for (int h = 2; h <= 10; h++) for (int nz = 0; nz < 2; nz++) {
            wave_t W = { "few", (double)h / 2.0, 1.0, nz ? 50.0 : 0.0, 0 };
            gen_wave(b, n, &W, 1, 777 + (uint64_t)h * 3 + (uint64_t)nz);
            char r0[64], r1[64]; route_of(b, w, n, r0); setenv("LOGOS_KNOT_FEW", "3", 1); route_of(b, w, n, r1); unsetenv("LOGOS_KNOT_FEW");
            /* the same array both sides, the switch set around one side, the order alternated */
            double p10, p90, med; int ok = 0;
            {   double *r = (double *)malloc((size_t)reps * sizeof(double)); if (!r) return 2;
                for (int i = 0; i < reps; i++) {
                    double ta = 0, tb = 0, s0;
                    for (int side = 0; side < 2; side++) {
                        int lifted = ((i & 1) ? 1 - side : side);            /* alternate which goes first */
                        memcpy(w, b, n * sizeof(double));
                        if (lifted) setenv("LOGOS_KNOT_FEW", "3", 1);
                        s0 = now_ms(); logos_sort_f64(w, n, 1); double dt = now_ms() - s0;
                        if (lifted) { unsetenv("LOGOS_KNOT_FEW"); tb = dt; } else ta = dt;
                        if (!sorted_ok(w, n)) ok = -1;
                    }
                    r[i] = tb > 0 ? ta / tb : 1.0;
                }
                qsort(r, (size_t)reps, sizeof(double), cmpd);
                med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
                p10 = r[(int)(0.10 * reps)]; p90 = r[(int)(0.90 * reps)]; free(r);
            }
            printf("  %-4d %-8s %-18s %-18s | %6.3f [%.2f %.2f]%s\n", h, nz ? "50" : "none", r0, r1, med, p10, p90, ok < 0 ? " BAD" : "");
            fflush(stdout);
        }
        return 0;
    }
    if (argc > 3 && (!strcmp(argv[3], "cos") || !strcmp(argv[3], "cosonce"))) {
        const wave_t W[] = {
            { "32 cycles, swing 0.6, no noise   ", 32, 0.6, 0, 1 },
            { "32 cycles, swing 0.6, noise 50   ", 32, 0.6, 50, 1 },
            { "8 cycles, swing 0.6, noise 50    ", 8, 0.6, 50, 1 },
            { "32 cycles, swing 2, noise 50     ", 32, 2.0, 50, 1 },
            { "2000 cycles, swing 0.6, noise 50 ", 2000, 0.6, 50, 1 },
            { "NO slope, 4 cycles, no noise     ", 4, 1.0, 0, 0 },
            { "NO slope, 4 cycles, noise 50     ", 4, 1.0, 50, 0 },
            { "NO slope, 1.5 cycles, no noise   ", 1.5, 1.0, 0, 0 },
            { "NO slope, quarter cycle, no noise", 0.25, 1.0, 0, 0 },
        };
        const int NW = (int)(sizeof W / sizeof W[0]);
        double *c = (double *)malloc(n * sizeof(double)); if (!c) return 2;
        if (!strcmp(argv[3], "cosonce")) {
            for (int s = 0; s < NW; s++) for (int k = 0; k < 2; k++) {
                gen_wave(b, n, &W[s], k, 4242 + (uint64_t)s);
                for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1; fprintf(stderr, "#### %s %s, %d thread(s)\n", W[s].nm, k ? "COS" : "sin", T);
                    memcpy(w, b, n * sizeof(double)); logos_sort_f64(w, n, T); }
            }
            return 0;
        }
        printf("cos() IN PLACE OF sin(), n = %zu, %d pairs, P = %d. Each wave's cos version sorted ADJACENTLY with its sin version (same\n"
               "noise, same seed), the order alternated: t(cos) / t(sin), median [p10 p90]; above 1 = the cosine costs more.\n\n", n, reps, P);
        printf("  %-34s %6s %6s | %-20s %-20s | %-24s %s\n", "wave", "pc sin", "pc cos", "route, sin (1T)", "route, cos (1T)", "t(cos)/t(sin), 1 thread", "P threads");
        for (int s = 0; s < NW; s++) {
            gen_wave(b, n, &W[s], 0, 4242 + (uint64_t)s); gen_wave(c, n, &W[s], 1, 4242 + (uint64_t)s);
            int ps, pc; double dp, rc; char r0[64], r1[64];
            invariants(b, n, &ps, &dp, &rc, W[s].noise); invariants(c, n, &pc, &dp, &rc, W[s].noise);
            route_of(b, w, n, r0); route_of(c, w, n, r1);
            double p10a, p90a, p10b, p90b; int ok1 = 0, okP = 0;
            double m1 = pair(c, b, w, n, 1, reps, &p10a, &p90a, &ok1), mP = pair(c, b, w, n, P, reps, &p10b, &p90b, &okP);
            printf("  %-34s %6d %6d | %-20s %-20s | %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s\n", W[s].nm, ps, pc, r0, r1,
                   m1, p10a, p90a, ok1 ? "    " : " BAD", mP, p10b, p90b, okP ? "    " : " BAD");
            fflush(stdout);
        }
        free(c);
        return 0;
    }
    const shape_t uni = { "uniform", 0, 0, 0, 1, 0 };
    gen(ref, n, &uni, 12345);

    printf("A sine wave with a slope: value(i) = i + A sin(2 pi i / T) + noise, n = %zu, %d pairs.\n", n, reps);
    printf("Each shape is sorted ADJACENTLY with a uniform reference of the same size and value range, the order\n"
           "alternated; the column is the MEDIAN of t_shape / t_uniform, so above 1 means the shape costs MORE than\n"
           "random data. Read the null row first: it is the same comparison with two uniform instances, so whatever it\n"
           "prints is this box's floor, and no other row means anything inside that band.\n\n");
    printf("  %-34s %6s %6s %7s | %s\n", "shape", "pieces", "depth", "reach", "t_shape / t_uniform, median [p10 p90]");
    printf("  %-34s %6s %6s %7s | %-24s %-24s %s\n", "", "", "", "", "1 thread", "P threads", "short-reach gate (1T)");
    for (int s = 0; s < NS; s++) {
        if (S[s].uniform) gen(b, n, &S[s], 999);              /* the null: a second instance, a different seed */
        else gen(b, n, &S[s], 99 + (uint64_t)s);
        int pieces; double depth, reach;
        invariants(b, n, &pieces, &depth, &reach, S[s].noise);
        memcpy(w, b, n * sizeof(double)); logos_sort_f64(w, n, 1); int st1 = logos_reach_last();
        double p10a, p90a, p10b, p90b; int ok1 = 0, okP = 0;
        /* mode ab: the shape against a COPY of itself (distinct pointers, so the switch finds its side), old policy on the shape side */
        const double *other = ab ? ref2 : ref; if (ab) memcpy(ref2, b, n * sizeof(double));
        double m1 = pair(b, other, w, n, 1, reps, &p10a, &p90a, &ok1);
        double mP = pair(b, other, w, n, P, reps, &p10b, &p90b, &okP);
        printf("  %-34s %6d %6.1f %7.0f | %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s %s\n",
               S[s].nm, pieces, depth, reach,
               m1, p10a, p90a, ok1 ? "    " : " BAD", mP, p10b, p90b, okP ? "    " : " BAD", ST[st1]);
        fflush(stdout);
    }
    printf("\nP = %d threads (%s). The reference is one fixed uniform instance; every row is paired against it.\n", P, bench_threads_source(logos_num_cpus()));
    printf("RESULT: ratios only. Absolute milliseconds from separate rows of one run are not comparable on this box\n"
           "        (methodology rule 2), which is what the first cut of this bench got wrong.\n");
    free(ref); free(b); free(w);
    return 0;
}
