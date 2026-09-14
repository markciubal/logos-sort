/* htquick.c — one line of heavy_tail timings at P threads, for a build matrix over compile flags.
 * heavy_tail is scalebench's generator: 1000 * (1 - u*0.999999)^(-1/1.2). Prints the minimum of `reps` sorts at
 * 1M and 10M, and uniform 10M as a control, every output checked ascending.   Usage: ./htquick [label] [reps] */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

static uint64_t RS = 7;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

static double best(double *base, double *w, size_t n, int T, int reps, int *asc) {
    double b = 1e30;
    for (int r = 0; r < reps; r++) {
        memcpy(w, base, n * 8);
        double t0 = now_ms(); logos_sort_f64(w, n, T); double dt = now_ms() - t0;
        if (dt < b) b = dt;
        for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { *asc = 0; break; }
    }
    return b;
}

int main(int argc, char **argv) {
    const char *label = argc > 1 ? argv[1] : "default";
    int reps = argc > 2 ? atoi(argv[2]) : 7;
    int P = bench_threads(logos_num_cpus());
    size_t N = 10000000;
    double *base = malloc(N * 8), *w = malloc(N * 8);
    if (!base || !w) return 1;
    int asc = 1;
    RS = 7; for (size_t i = 0; i < 1000000; i++) base[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2);
    double h1 = best(base, w, 1000000, P, reps, &asc);
    RS = 7; for (size_t i = 0; i < N; i++) base[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2);
    double h10 = best(base, w, N, P, reps, &asc);
    RS = 7; for (size_t i = 0; i < N; i++) base[i] = fr() * 2e9 - 1e9;
    double u10 = best(base, w, N, P, reps, &asc);
    printf("  %-26s T=%-2d heavy_tail 1M %7.2f ms   heavy_tail 10M %8.2f ms   uniform 10M %7.2f ms   heavy/uniform %.2f   %s\n",
           label, P, h1, h10, u10, h10 / u10, asc ? "ok" : "NOT ASCENDING");
    free(base); free(w);
    return 0;
}
