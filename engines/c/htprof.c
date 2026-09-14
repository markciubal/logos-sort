/* htprof.c — is heavy_tail's slow 14-thread scatter CPU work or lost time? Build against a -DLOGOS_PROFILE engine.
 *
 * heavy_tail (scalebench's generator) and uniform, interleaved in one process with the order alternating, `reps` times
 * each at n and P threads. Per sort: route, total wall, scatter and leaf wall, and for the scatter the per-thread sums of
 * busy (working), cpu (CPU actually received) and wait (at barriers); busy - cpu is time preempted. The verdict line
 * compares CPU nanoseconds per element in the scatter: if heavy_tail's excess is CPU, the lookup costs it; if it is
 * preemption or waiting, the machine does.   Usage: ./htprof [n] [reps] */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t RS = 7;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }

typedef struct { double total, sc_wall, lf_wall, sc_busy, sc_cpu, sc_wait, lf_busy, lf_cpu; int route, ok; } rec_t;
static int cmp_dbl(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

static rec_t one(const double *base, double *w, size_t n, int P) {
    rec_t r; memset(&r, 0, sizeof r);
    memcpy(w, base, n * 8);
    logos_profile_reset();
    logos_sort_f64(w, n, P);
    const logos_profile_t *p = logos_profile_get();
    r.ok = 1; for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) { r.ok = 0; break; }
    if (!p) return r;
    r.route = p->route; r.total = p->total_ms;
    r.sc_wall = p->phase_ms[LOGOS_PH_SCATTER]; r.lf_wall = p->phase_ms[LOGOS_PH_BUCKETS];
    int T = p->nthreads < LOGOS_PROF_MAX_THREADS ? p->nthreads : LOGOS_PROF_MAX_THREADS;
    for (int t = 0; t < T; t++) {
        r.sc_busy += p->busy_ms[t][LOGOS_PH_SCATTER]; r.sc_cpu += p->cpu_ms[t][LOGOS_PH_SCATTER]; r.sc_wait += p->wait_ms[t][LOGOS_PH_SCATTER];
        r.lf_busy += p->busy_ms[t][LOGOS_PH_BUCKETS]; r.lf_cpu += p->cpu_ms[t][LOGOS_PH_BUCKETS];
    }
    return r;
}
static double med(double *v, int k) { qsort(v, (size_t)k, sizeof(double), cmp_dbl); return k % 2 ? v[k / 2] : 0.5 * (v[k / 2 - 1] + v[k / 2]); }

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 10000000;
    int reps = argc > 2 ? atoi(argv[2]) : 12; if (reps > 64) reps = 64;
    int P = bench_threads(logos_num_cpus());
    double *heavy = malloc(n * 8), *unif = malloc(n * 8), *w = malloc(n * 8);
    if (!heavy || !unif || !w) return 1;
    RS = 7; for (size_t i = 0; i < n; i++) heavy[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2);
    RS = 7; for (size_t i = 0; i < n; i++) unif[i] = fr() * 2e9 - 1e9;
    one(heavy, w, n, P); one(unif, w, n, P);                               /* warm the pool and the partition buffers */
    if (!logos_profile_get()) { printf("engine not built with -DLOGOS_PROFILE\n"); return 1; }

    printf("n=%zu, P=%d, %d interleaved reps each (order alternates). ms; busy/cpu/wait are summed over threads.\n\n", n, P, reps);
    printf("  %-10s %3s %-10s %8s | %8s %9s %9s %8s %8s | %8s %9s\n", "shape", "rep", "route", "total", "scat wall", "scat busy", "scat cpu", "preempt", "wait", "leaf wall", "leaf cpu");
    rec_t R[2][64];
    for (int r = 0; r < reps; r++)
        for (int k0 = 0; k0 < 2; k0++) {
            int k = (k0 + r) % 2;
            R[k][r] = one(k == 0 ? heavy : unif, w, n, P);
            rec_t *x = &R[k][r];
            printf("  %-10s %3d %-10s %8.1f | %8.1f %9.1f %9.1f %8.1f %8.1f | %8.1f %9.1f %s\n", k == 0 ? "heavy_tail" : "uniform", r,
                   logos_profile_route_names[x->route], x->total, x->sc_wall, x->sc_busy, x->sc_cpu, x->sc_busy - x->sc_cpu, x->sc_wait,
                   x->lf_wall, x->lf_cpu, x->ok ? "" : "NOT ASCENDING");
        }
    printf("\n  medians          total  scat wall  scat busy  scat cpu  preempt  cpu ns/elem in scatter  leaf wall  leaf cpu\n");
    double ns[2];
    for (int k = 0; k < 2; k++) {
        double a[64], b[64], c[64], d[64], e[64], f[64], g[64];
        for (int r = 0; r < reps; r++) { rec_t *x = &R[k][r]; a[r] = x->total; b[r] = x->sc_wall; c[r] = x->sc_busy; d[r] = x->sc_cpu; e[r] = x->sc_busy - x->sc_cpu; f[r] = x->lf_wall; g[r] = x->lf_cpu; }
        double mc = med(d, reps); ns[k] = mc * 1e6 / (double)n;
        printf("  %-10s %9.1f %10.1f %10.1f %9.1f %8.1f %23.1f %10.1f %9.1f\n", k == 0 ? "heavy_tail" : "uniform", med(a, reps), med(b, reps), med(c, reps), mc, med(e, reps), ns[k], med(f, reps), med(g, reps));
    }
    printf("\n  verdict: the scatter costs heavy_tail %.1f CPU ns per element against uniform's %.1f (%.2fx)\n", ns[0], ns[1], ns[1] > 0 ? ns[0] / ns[1] : 0);
    free(heavy); free(unif); free(w);
    return 0;
}
