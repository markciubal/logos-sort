/* bench_common.h — the thread count every benchmark defaults to (the engine itself still takes
 * whatever the caller passes; logos_num_cpus() is unchanged).
 *
 * Priority:
 *   1. LOGOS_THREADS in the environment;
 *   2. -DLOGOS_BENCH_THREADS from the Makefile (BENCH_THREADS, default 14 = this box's physical
 *      core count: `make noiseprobe` showed the quietest and lowest minimum at 14 threads on a
 *      20-thread hybrid i9-13900H, because hyperthreading only adds preemption exposure and the
 *      barrier waits for the slowest sibling);
 *   3. the physical core count from /sys when the kernel exposes SMT topology;
 *   4. online CPUs.
 * Fair fights stay fair: every contender runs at the same count. */
#ifndef LOGOS_BENCH_COMMON_H
#define LOGOS_BENCH_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int bench_physical_cores(int online) {
    int ids[4096]; int nid = 0;
    for (int c = 0; c < online && c < 4096; c++) {
        char path[128]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", c);
        FILE *f = fopen(path, "r"); if (!f) return online;
        int id = -1; if (fscanf(f, "%d", &id) != 1) { fclose(f); return online; } fclose(f);
        int seen = 0; for (int k = 0; k < nid; k++) if (ids[k] == id) { seen = 1; break; }
        if (!seen) ids[nid++] = id;
    }
    return nid > 0 ? nid : online;
}
static int bench_threads(int online) {
    const char *e = getenv("LOGOS_THREADS"); if (e && atoi(e) > 0) return atoi(e);
#ifdef LOGOS_BENCH_THREADS
    if (LOGOS_BENCH_THREADS > 0 && LOGOS_BENCH_THREADS <= online) return LOGOS_BENCH_THREADS;
#endif
    return bench_physical_cores(online);
}
/* ---- paired A/B (the standard for anything under 1.25x) ----------------------------------------
 * Best-of-N across separately measured variants cannot resolve small effects on this box: the machine
 * wanders 5-10x on a timescale of tens of seconds (FINDINGS 3t) and separately compiled engine copies
 * differ by up to 1.24x from code layout alone (3r). Both cancel if the two variants are measured
 * ADJACENTLY and you report the distribution of their ratio: within a pair the machine state is the
 * same, and alternating which variant goes first cancels the ordering bias (the first half of a pair
 * measured ~2-4% slow in the null test, from destination-buffer page state).
 *
 * Measured floor on the reference box (make pairprobe, identical work in both halves): a single pair
 * has a p90 deviation of 8-14%, but the MEDIAN of 40 pairs lands within 2-4% of 1.0. So report the
 * median, quote p10/p90, and do not believe an effect smaller than about 3%. */
typedef void (*bench_sort_fn)(double *, size_t, int);
static int bench_cmp_d_(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double bench_now_(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
/* Returns the median of tA/tB over `reps` adjacent pairs (<1 means A is faster); p10/p90 optional. */
static double bench_paired(bench_sort_fn A, bench_sort_fn B, const double *base, double *work,
                           size_t n, int threads, int reps, double *p10, double *p90) {
    double *r = (double *)malloc((size_t)reps * sizeof(double));
    if (!r) return 0.0;
    for (int i = 0; i < reps; i++) {
        double ta, tb, s;
        if (i & 1) {                                   /* alternate which half goes first */
            memcpy(work, base, n * sizeof(double)); s = bench_now_(); B(work, n, threads); tb = bench_now_() - s;
            memcpy(work, base, n * sizeof(double)); s = bench_now_(); A(work, n, threads); ta = bench_now_() - s;
        } else {
            memcpy(work, base, n * sizeof(double)); s = bench_now_(); A(work, n, threads); ta = bench_now_() - s;
            memcpy(work, base, n * sizeof(double)); s = bench_now_(); B(work, n, threads); tb = bench_now_() - s;
        }
        r[i] = tb > 0 ? ta / tb : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), bench_cmp_d_);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    if (p10) *p10 = r[(int)(0.10 * reps)];
    if (p90) *p90 = r[(int)(0.90 * reps)];
    free(r);
    return med;
}
static const char *bench_threads_source(int online) {
    const char *e = getenv("LOGOS_THREADS"); if (e && atoi(e) > 0) return "LOGOS_THREADS";
#ifdef LOGOS_BENCH_THREADS
    if (LOGOS_BENCH_THREADS > 0 && LOGOS_BENCH_THREADS <= online) return "BENCH_THREADS (Makefile, physical cores of the reference box)";
#endif
    return bench_physical_cores(online) < online ? "physical cores from /sys" : "online CPUs";
}
#endif
