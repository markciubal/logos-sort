/* pairprobe.c — what can this machine resolve today? Runs the paired A/B harness with the SAME engine
 * in both halves, so the true ratio is exactly 1.0 and everything reported is measurement error.
 * Run it before trusting any A/B result under about 1.25x, the way you would read a calibration line.
 * Usage: ./pairprobe [pairs] [n]                                        LOGOS_THREADS=N overrides. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40; if (reps < 8) reps = 8;
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 8000000;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *work = malloc(n * 8);
    if (!base || !work) return 1;
    for (size_t i = 0; i < n; i++) base[i] = fr() * 2e9 - 1e9;
    printf("paired null test: identical engine in both halves, n=%zu, %d pairs, %d threads\n", n, reps, P);
    printf("the true ratio is 1.000; everything below is this machine's measurement error\n\n");
    printf("  %-10s %9s %9s %9s %11s\n", "size", "median", "p10", "p90", "resolves");
    size_t sizes[2] = { n / 8 ? n / 8 : 1000, n };
    for (int k = 0; k < 2; k++) {
        double p10 = 0, p90 = 0;
        double med = bench_paired(logos_sort_f64, logos_sort_f64, base, work, sizes[k], P, reps, &p10, &p90);
        double dev = fabs(p90 - 1.0) > fabs(p10 - 1.0) ? fabs(p90 - 1.0) : fabs(p10 - 1.0);
        printf("  %-10zu %9.3f %9.3f %9.3f %10.1f%%\n", sizes[k], med, p10, p90, (dev / sqrt((double)reps)) * 100.0 * 1.25);
    }
    printf("\n  median is what to report; p10/p90 bound a single pair. \"resolves\" is the rough\n"
           "  standard error of the median over these pairs: effects smaller than that are noise.\n");
    free(base); free(work);
    return 0;
}
