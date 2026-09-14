/* htprobe.c — why is heavy_tail slow at 14 threads in the scale report?
 *
 * The data is scalebench's heavy_tail exactly: 1000 * (1 - u*0.999999)^(-1/1.2). Two uses:
 *   ./htprobe route [n]     against a -DLOGOS_TRACE engine: one sort at 1 thread and one at P, each preceded by a marker,
 *                           so the dispatcher's decisions (sample, gates, certificates, histfree or histogram) can be read.
 *   ./htprobe time [n] [reps]  against the normal engine: minimum wall time of heavy_tail and uniform at 1 and P threads,
 *                           with each runtime toggle that could touch the parallel path, interleaved rep by rep.
 * Every sorted output is checked ascending. */
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
static void g_heavy(double *a, size_t n)   { for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
static void g_uniform(double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; }

static const char *TOGGLES[] = { "", "LOGOS_MCGATE_OLD", "LOGOS_TAIL_SEQ", "LOGOS_NO_STREAMS", "LOGOS_MC_OFF" };
#define NT ((int)(sizeof TOGGLES / sizeof *TOGGLES))
static void set_only(const char *name) { for (int i = 1; i < NT; i++) unsetenv(TOGGLES[i]); if (name && *name) setenv(name, "1", 1); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "time";
    size_t n = argc > 2 ? (size_t)atol(argv[2]) : 1000000;
    int reps = argc > 3 ? atoi(argv[3]) : 9;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8);
    if (!base || !w) return 1;
    if (strcmp(mode, "route") == 0) {
        RS = 7; g_heavy(base, n);
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            memcpy(w, base, n * 8);
            fprintf(stderr, "=== heavy_tail n=%zu T=%d\n", n, tc[ti]);
            logos_sort_f64(w, n, tc[ti]);
        }
        return 0;
    }
    printf("heavy_tail vs uniform, n=%zu, min of %d interleaved reps, threads 1 and %d; each row one runtime toggle\n\n", n, reps, P);
    printf("  %-11s %-18s %2s %10s %10s  %s\n", "shape", "toggle", "T", "min ms", "median ms", "ascending");
    for (int s = 0; s < 2; s++) {
        RS = 7; if (s == 0) g_heavy(base, n); else g_uniform(base, n);
        int tc[2] = { 1, P };
        for (int ti = 0; ti < 2; ti++) {
            double best[NT], all[NT][64]; int asc[NT];
            for (int k = 0; k < NT; k++) { best[k] = 1e30; asc[k] = 1; }
            for (int r = 0; r < reps && r < 64; r++)
                for (int k0 = 0; k0 < NT; k0++) {
                    int k = (k0 + r) % NT;                        /* rotate which toggle goes first */
                    set_only(TOGGLES[k]);
                    memcpy(w, base, n * 8);
                    double t0 = now_ms(); logos_sort_f64(w, n, tc[ti]); double dt = now_ms() - t0;
                    all[k][r] = dt; if (dt < best[k]) best[k] = dt;
                    for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { asc[k] = 0; break; }
                }
            for (int k = 0; k < NT; k++) {
                double tmp[64]; int m = reps < 64 ? reps : 64; memcpy(tmp, all[k], (size_t)m * sizeof(double));
                for (int i = 1; i < m; i++) { double v = tmp[i]; int j = i; while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; } tmp[j] = v; }
                printf("  %-11s %-18s %2d %10.2f %10.2f  %s\n", s == 0 ? "heavy_tail" : "uniform", *TOGGLES[k] ? TOGGLES[k] : "(default)", tc[ti], best[k], tmp[m / 2], asc[k] ? "yes" : "NO");
            }
            fflush(stdout);
        }
    }
    set_only(NULL);
    free(base); free(w);
    return 0;
}
