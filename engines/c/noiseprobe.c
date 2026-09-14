/* noiseprobe.c — measure the measurement. Repeats an identical sort of the same 8M uniform
 * input R times at several thread counts and reports min / median / max and the spread
 * (max/min), plus memcpy and a single-thread sort's CPU-vs-wall ratio (1 - cpu/wall is the
 * share the OS gave to someone else). Tells you which thread count is both fastest and
 * quietest on this machine, and whether the noise is preemption (cpu/wall) or straggler
 * threads (spread grows with thread count). Usage: ./noiseprobe [reps]   LOGOS_THREADS=max */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double cpu_ms(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec * 1e3 + u.ru_utime.tv_usec / 1e3 + u.ru_stime.tv_sec * 1e3 + u.ru_stime.tv_usec / 1e3; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void stats(const char *label, double *v, int R, double extra, const char *extra_name) {
    qsort(v, R, sizeof(double), cmpd);
    double med = R & 1 ? v[R / 2] : 0.5 * (v[R / 2 - 1] + v[R / 2]);
    printf("  %-22s min %8.2f  median %8.2f  max %8.2f  max/min %5.2f  median/min %5.2f", label, v[0], med, v[R - 1], v[R - 1] / v[0], med / v[0]);
    if (extra_name) printf("  %s %.2f", extra_name, extra);
    printf("\n");
}
int main(int argc, char **argv) {
    int R = argc > 1 ? atoi(argv[1]) : 12; if (R < 3) R = 3;
    int Pmax = logos_num_cpus(); const char *e = getenv("LOGOS_THREADS"); if (e && atoi(e) > 0) Pmax = atoi(e);
    size_t N = 8000000; double *base = malloc(N * 8), *a = malloc(N * 8), *b = malloc(N * 8);
    for (size_t i = 0; i < N; i++) base[i] = fr() * 2e9 - 1e9;
    double *v = malloc(R * sizeof(double));
    printf("noise probe: 8M uniform, %d reps per row, up to %d threads\n\n", R, Pmax);
    for (int r = 0; r < R; r++) { double s = now(); memcpy(a, base, N * 8); v[r] = now() - s; }
    stats("memcpy 64MB", v, R, 0, NULL);
    { double cpu_sum = 0, wall_sum = 0;
      for (int r = 0; r < R; r++) { memcpy(a, base, N * 8); double c0 = cpu_ms(), s = now(); logos_sort_f64(a, N, 1); v[r] = now() - s; cpu_sum += cpu_ms() - c0; wall_sum += v[r]; }
      stats("logos 1 thread", v, R, 1.0 - cpu_sum / wall_sum, "preempted share"); }
    int cand[] = { 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32 };
    for (size_t k = 0; k < sizeof cand / sizeof *cand; k++) {
        int P = cand[k]; if (P > Pmax) break;
        double cpu_sum = 0, wall_sum = 0;
        for (int r = 0; r < R; r++) { memcpy(a, base, N * 8); double c0 = cpu_ms(), s = now(); logos_sort_f64(a, N, P); v[r] = now() - s; cpu_sum += cpu_ms() - c0; wall_sum += v[r]; }
        char lab[32]; snprintf(lab, sizeof lab, "logos %d threads", P);
        stats(lab, v, R, cpu_sum / (wall_sum * P), "cpu utilisation");
    }
    /* order effect: the same 20-thread sort, alternating with a memcpy, to see whether back-to-back runs heat or cool */
    { int P = Pmax < 20 ? Pmax : 20; for (int r = 0; r < R; r++) { memcpy(b, base, N * 8); memcpy(a, base, N * 8); double s = now(); logos_sort_f64(a, N, P); v[r] = now() - s; }
      char lab[40]; snprintf(lab, sizeof lab, "logos %dT, memcpy between", P); stats(lab, v, R, 0, NULL); }
    printf("\nread: max/min near 1.0 = quiet; a large median/min with a low preempted share = straggler threads (hybrid cores), not load;\n"
           "a large preempted share = another process. The thread count with the lowest min AND lowest spread is the one to benchmark with.\n");
    return 0;
}
