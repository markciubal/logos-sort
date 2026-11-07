/* orderprobe: "order must never cost" (2026-10-20, the second invented round). Every shape is timed against THE SAME VALUES
 * SHUFFLED, adjacently, alternating which goes first, at one thread and at P. The multiset is identical, so the only thing the
 * ratio measures is what the engine does with the ORDER:
 *
 *   ratio = median over pairs of t(shape as it lies) / t(its own values in random order)
 *
 * Below 1: the engine found and used the order. About 1: it ignored the order, which is a missed opportunity only where the
 * order is exploitable. ABOVE 1: the order made the sort SLOWER than having no order at all, which is a defect: the sine wave
 * with a slope (0i (f)) was the first of these, at 1.17-1.27. Every row's two outputs are compared bit for bit before anything
 * is timed (the shapes hold no NaN and no signed zero, so a correct sort of the same multiset is one byte string).
 *
 *   ./orderprobe [pairs] [n] [P list] [shape filter]        e.g. ./orderprobe 20 4194304 1,14
 *   ./orderprobe 0 4194304 14 <filter> once                 one sort per shape: link against a -DLOGOS_TRACE engine (orderprobe_trace)
 *   OP_SWITCH=<NAME> [OP_VALUE=<v>] ./orderprobe ... ab      the shape as it lies with the switch set against shipped (> 1: shipped faster)
 *
 * The shapes are all new to the project's benches (none of them had been run before this file), chosen as data a sort meets:
 * logs, walks, groupings, interleavings, reversals with ties, FFT output, tiles. PREDICTION, written before the first run:
 * every ratio at or below 1.00 within the floor (~4% at 20 pairs), with the strongly ordered shapes (rot1, min_at_end,
 * sorted_ties, desc_ties, two_halves, blocks512, coarse1k) at 0.05-0.5; the walks, groups and tiles near 1 (a counting route
 * ignores order by design). Any row above 1.05 in both thread counts is a finding. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"


static uint64_t rs = 0x2545F4914F6CDD1DULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static double gauss(void) { double u = u01() + 1e-300, v = u01(); return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

static const char *SHAPES[] = {
    "uniform", "sorted", "organ_pipe", "organ_pipe_noise", "rot1", "min_at_end", "zipper", "price_walk", "float_walk",
    "groups16", "groups4096", "coarse64k", "coarse1k", "two_halves", "bitrev", "desc_ties", "sorted_ties", "tile1000",
    "prefix_desc_tail", "blocks512", "multi_sine", "exp_noise", "stairs_desc", "desc_ties_noise" };
#define NSH ((int)(sizeof SHAPES / sizeof SHAPES[0]))

static void shuffle(double *a, size_t n) { for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); double t = a[i]; a[i] = a[j]; a[j] = t; } }

static void gen(int sh, double *a, size_t n) {
    switch (sh) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; break;
    case 2: case 3:                                          /* up through the even values, down through the odd ones */
        for (size_t i = 0; i < n; i++) a[i] = (i < n / 2 ? 2.0 * (double)i : 2.0 * (double)(n - i) - 1.0) + 0.5 + (sh == 3 ? (u01() - 0.5) * 100.0 : 0.0);
        break;
    case 4: a[0] = (double)n + 0.5; for (size_t i = 1; i < n; i++) a[i] = (double)i - 0.5; break;           /* the maximum pushed to the front */
    case 5: for (size_t i = 0; i + 1 < n; i++) a[i] = (double)i + 1.5; a[n - 1] = 0.5; break;              /* the minimum appended at the end */
    case 6: for (size_t i = 0; i < n; i++) a[i] = (i & 1) ? (double)n - (double)(i / 2) - 0.25 : (double)(i / 2) + 0.25; break;  /* up and down, interleaved */
    case 7: { double p = 100000.0; for (size_t i = 0; i < n; i++) { p += (nr() & 1) ? 1.0 : -1.0; a[i] = p / 100.0; } break; }    /* cents, a +-1 cent walk */
    case 8: { double p = 0; for (size_t i = 0; i < n; i++) { p += gauss(); a[i] = p; } break; }
    case 9: case 10: { size_t L = sh == 9 ? 16 : 4096; double v = 0;                              /* runs of equal keys, runs in random order */
        for (size_t i = 0; i < n; i++) { if (i % L == 0) v = u01() * 1e9; a[i] = v; } break; }
    case 11: case 12: { size_t W = sh == 11 ? 65536 : 1024;                                       /* sorted by window, random inside one */
        for (size_t i = 0; i < n; i++) a[i] = (double)(i / W) * (double)W + u01() * (double)W; break; }
    case 13: { for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6;                                  /* two sorted halves, each spanning the range */
        qsort(a, n / 2, sizeof(double), cmpd); qsort(a + n / 2, n - n / 2, sizeof(double), cmpd); break; }
    case 14: { int b = 0; while (((size_t)1 << b) < n) b++;                                         /* bit-reversal order (an FFT's output) */
        for (size_t i = 0; i < n; i++) { size_t r = 0, x = i; for (int k = 0; k < b; k++) { r = (r << 1) | (x & 1); x >>= 1; } a[i] = (double)r + 0.5; } break; }
    case 15: for (size_t i = 0; i < n; i++) a[i] = (double)((n - 1 - i) / 4) + 0.5; break;          /* descending, every value four times */
    case 16: for (size_t i = 0; i < n; i++) a[i] = (double)(i / 4) + 0.5; break;                     /* ascending, every value four times */
    case 17: for (size_t i = 0; i < n; i++) a[i] = (double)(i % 1000) + 0.5; break;                  /* one sorted tile of 1000 values, repeated */
    case 18: { size_t p = n - n / 10; for (size_t i = 0; i < p; i++) a[i] = (double)i + 0.5;        /* 90% ascending, then a DESCENDING tail over the range */
        for (size_t i = p; i < n; i++) a[i] = (double)(n - i) * ((double)p / (double)(n - p)) + 0.25; break; }
    case 19: { size_t L = 512, B = n / L; size_t *perm = malloc(B * sizeof *perm); for (size_t b = 0; b < B; b++) perm[b] = b;   /* shuffled sorted blocks of 512 */
        for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
        for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) + 0.25; } free(perm); break; }
    case 20: for (size_t i = 0; i < n; i++) { double x = (double)i;                                   /* a trend and three waves of very different periods */
        a[i] = x + 0.08 * (double)n * sin(6.283185307179586 * x / ((double)n / 7.0)) + 0.004 * (double)n * sin(6.283185307179586 * x / ((double)n / 61.0))
             + 0.0003 * (double)n * sin(6.283185307179586 * x / ((double)n / 499.0)) + (u01() - 0.5) * 20.0; } break;
    case 21: for (size_t i = 0; i < n; i++) a[i] = exp((double)i * 20.0 / (double)n) * (1.0 + (u01() - 0.5) * 0.002); break;   /* growth with relative noise */
    case 22: { size_t B = 20, L = n / B;                                                              /* a staircase read backwards: descending blocks, */
        for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1, k = i - b * L; a[i] = (double)(B - 1 - b) * (double)L * 0.9 + (double)(L - k) + 0.125; } break; }   /* each overlapping the next */
    case 23: for (size_t i = 0; i < n; i++) a[i] = (double)((n - 1 - i) / 4) + 0.5 + (u01() < 0.001 ? 7.0 : 0.0); break;   /* desc_ties with 0.1% bumped */
    }
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *g_sw, *g_val;

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20;
    size_t n = argc > 2 ? (size_t)atof(argv[2]) : (size_t)1 << 22;
    const char *pl = argc > 3 ? argv[3] : "1,14";
    const char *filt = argc > 4 ? argv[4] : "";
    int once = argc > 5 && !strcmp(argv[5], "once"), ab = argc > 5 && !strcmp(argv[5], "ab");
    g_sw = getenv("OP_SWITCH"); g_val = getenv("OP_VALUE");
    double *a = malloc(n * sizeof(double)), *s = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double)), *r1 = malloc(n * sizeof(double));
    double *ra = malloc((size_t)(pairs > 0 ? pairs : 1) * sizeof(double));
    if (!a || !s || !w || !r1 || !ra) { fprintf(stderr, "out of memory\n"); return 1; }
    if (ab) printf("# orderprobe ab: switch %s%s%s, n = %zu, %d pairs. Column: median t(switch) / t(shipped), p10-p90. > 1: shipped is faster.\n",
                   g_sw ? g_sw : "(none: null run)", g_val ? "=" : "", g_val ? g_val : "", n, pairs);
    else if (!once) printf("# orderprobe: n = %zu, %d pairs. Column: median t(as it lies) / t(same values shuffled), p10-p90; ns/elem as it lies.\n"
                           "# BELOW 1: the order was used. ABOVE 1: the order made the sort SLOWER than no order (a defect).\n", n, pairs);
    for (const char *pp = pl; *pp; ) {
        int P = atoi(pp); while (*pp && *pp != ',') pp++; if (*pp == ',') pp++;
        if (!once) printf("\n## P = %d\n%-18s %8s  %15s  %9s\n", P, "shape", "ratio", "p10-p90", "ns/elem");
        for (int sh = 0; sh < NSH; sh++) {
            if (*filt && !strstr(SHAPES[sh], filt)) continue;
            rs = 0x2545F4914F6CDD1DULL + (uint64_t)sh * 0x9E3779B97F4A7C15ULL;
            gen(sh, a, n); memcpy(s, a, n * sizeof(double)); shuffle(s, n);
            if (once) { fprintf(stderr, "\n===== %s, P = %d =====\n", SHAPES[sh], P); memcpy(w, a, n * sizeof(double)); logos_sort_f64(w, n, P); continue; }
            /* verify: the shape and its shuffle must sort to the same bytes (and the result must ascend) */
            memcpy(w, a, n * sizeof(double)); logos_sort_f64(w, n, P);
            memcpy(r1, s, n * sizeof(double)); logos_sort_f64(r1, n, P);
            int bad = memcmp(w, r1, n * sizeof(double)) != 0;
            for (size_t i = 1; i < n && !bad; i++) if (w[i] < w[i - 1]) bad = 1;
            if (ab && !bad) { if (g_sw) setenv(g_sw, g_val ? g_val : "1", 1); memcpy(r1, a, n * sizeof(double)); logos_sort_f64(r1, n, P); if (g_sw) unsetenv(g_sw);
                              bad = memcmp(w, r1, n * sizeof(double)) != 0; }
            if (bad) { printf("%-18s MISMATCH -- not timed\n", SHAPES[sh]); fflush(stdout); continue; }
            double tsum = 0;
            for (int i = 0; i < pairs; i++) {
                double tx, ty, t0;
                const double *X = a, *Y = ab ? a : s;
                for (int k = 0; k < 2; k++) {
                    int first_x = (i & 1) == 0;
                    int do_x = (k == 0) == first_x;
                    memcpy(w, do_x ? X : Y, n * sizeof(double));
                    if (ab && do_x && g_sw) setenv(g_sw, g_val ? g_val : "1", 1);
                    t0 = now_ms(); logos_sort_f64(w, n, P); double dt = now_ms() - t0;
                    if (ab && do_x && g_sw) unsetenv(g_sw);
                    if (do_x) tx = dt; else ty = dt;
                }
                ra[i] = ty > 0 ? tx / ty : 1.0;
                if (!ab) tsum += tx; else tsum += ty;
            }
            qsort(ra, (size_t)pairs, sizeof(double), cmpd);
            double med = (pairs & 1) ? ra[pairs / 2] : 0.5 * (ra[pairs / 2 - 1] + ra[pairs / 2]);
            printf("%-18s %8.3f  %6.3f - %6.3f  %9.2f\n", SHAPES[sh], med, ra[(int)(0.1 * pairs)], ra[(int)(0.9 * pairs)], tsum / pairs * 1e6 / (double)n);
            fflush(stdout);
        }
    }
    free(a); free(s); free(w); free(r1); free(ra);
    return 0;
}
