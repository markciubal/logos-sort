/* bandbench: the leaf's choice between the AVX2 quicksort and flash_sort, sized.
 *
 * WHY THIS EXISTS AND callbench DOES NOT DO IT. The effect lives between FLASH_SORT_THRESHOLD (4096)
 * and about 65536, on data that buckets evenly. callbench steps straight over that window
 * (1000 -> 5000 -> 50000) and its "random" shape is (double)(nr() >> 11) -- integer-valued doubles
 * spanning 2^53, which costs 25.7 ns per element at n = 4096 where this shape costs 8.3, i.e. it never
 * reaches these paths at all. Narrowing callbench's SIZES alone read 0.989-1.017 across the whole band
 * and would have buried the result. A bench has to reach the code before its flatness means anything.
 *
 * WHAT IT MEASURES. One binary holds two engine copies, bb0 and bb1, one compiled
 * -DLOGOS_AVX2_OVER_FLASH_FORCE (the order as it shipped before 2026-09-23: the AVX2 leaf takes the range
 * before the flash/coarse decision is made) and one without (the order now: flash first inside the
 * window). Blocks alternate between the copies on identical data, so drift cancels; the Makefile builds
 * it twice with the copies SWAPPED and the second run reports the geometric mean, which cancels the code
 * layout bias exactly -- the same construction as callbench, whose header explains why that is needed.
 *
 *   make bandbench                 both orientations, cancelled
 *   ./bandbench <0|1> [other.txt]  one orientation; the argument is which copy is the NEW order
 *
 * Shape: uniform doubles in [0, 1e9). Guards below the window (512-3000, where flash does not exist and
 * nothing may change) and above it (131072-2097152, where the AVX2 kernel must keep the range).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

void bb0_sort_f64(double *a, size_t n, int threads);
void bb1_sort_f64(double *a, size_t n, int threads);

static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

enum { N_SIZES = 12, BLOCKS = 80 };
static const size_t SIZES[N_SIZES] = { 512, 1024, 2048, 3000, 4096, 6000, 8192, 16384, 32768, 65536, 131072, 2097152 };

static double lower_quartile(double *v, int m) {
    for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return v[m / 4];
}

int main(int argc, char **argv) {
    int new_slot = argc > 1 ? atoi(argv[1]) : 0;          /* which copy holds the NEW order in this build */
    double other[N_SIZES]; int have_other = 0;
    for (int i = 0; i < N_SIZES; i++) { other[i] = 0; }
    if (argc > 2) {
        FILE *f = fopen(argv[2], "r"); char line[512];
        if (!f) { fprintf(stderr, "bandbench: cannot read %s\n", argv[2]); return 2; }
        while (fgets(line, sizeof line, f)) {
            size_t n; double a, b, r;
            if (sscanf(line, "row %zu %lf %lf %lf", &n, &a, &b, &r) == 4) {
                for (int i = 0; i < N_SIZES; i++) { if (SIZES[i] == n) { other[i] = r; have_other = 1; } }
            }
        }
        fclose(f);
    }
    printf("# bandbench: ns per element, 1 thread, lower quartile of %d alternating blocks per engine.\n", BLOCKS / 2);
    printf("# old = the AVX2 leaf ahead of the flash decision; new = flash first for %d <= n <= %d.\n", 4096, 65536);
    printf("# old/new above 1 means the NEW order is faster.%s\n", have_other ? " Last column: both orientations, layout cancelled." : "");
    printf("#\n# %8s %12s %12s %10s%s\n", "n", "old", "new", "old/new", have_other ? "   other run   cancelled" : "");
    for (int si = 0; si < N_SIZES; si++) {
        size_t n = SIZES[si];
        size_t reps = n <= 4096 ? 400 : n <= 65536 ? 120 : n <= 131072 ? 60 : 8;
        double *base = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double));
        if (!base || !w) { fprintf(stderr, "bandbench: no memory\n"); return 2; }
        RS = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(si * 131 + 7);
        for (size_t i = 0; i < n; i++) { base[i] = (double)(nr() >> 11) / 9007199254740992.0 * 1e9; }
        static double tt[2][BLOCKS]; int cnt[2] = { 0, 0 };
        for (int blk = 0; blk < BLOCKS; blk++) {
            int slot = blk & 1; double t0 = now_ns();
            for (size_t r = 0; r < reps; r++) {
                memcpy(w, base, n * sizeof(double));
                if (slot) { bb1_sort_f64(w, n, 1); } else { bb0_sort_f64(w, n, 1); }
            }
            tt[slot][cnt[slot]++] = (now_ns() - t0) / (double)reps / (double)n;
            for (size_t i = 1; i < n; i++) { if (w[i] < w[i - 1]) { printf("UNSORTED: n=%zu copy bb%d\n", n, slot); return 1; } }
        }
        double q[2] = { lower_quartile(tt[0], cnt[0]), lower_quartile(tt[1], cnt[1]) };
        double nw = q[new_slot], old = q[1 - new_slot], ratio = old / nw;
        printf("row %8zu %12.3f %12.3f %10.3f", n, old, nw, ratio);
        if (have_other && other[si] > 0) {
            double c = ratio * other[si], g = 1.0, lo = 0.0, hi = c > 1.0 ? c : 1.0;   /* sqrt by bisection: no libm needed */
            for (int it = 0; it < 60; it++) { g = 0.5 * (lo + hi); if (g * g > c) { hi = g; } else { lo = g; } }
            printf("   %9.3f   %9.3f", other[si], g);
        }
        printf("\n"); fflush(stdout);
        free(base); free(w);
    }
    return 0;
}
