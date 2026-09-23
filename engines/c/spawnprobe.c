/* spawnprobe: how much of a short parallel phase is thread creation and joining?
 *
 * The lattice route's EMIT phase measures 1.84 ms of wall at 14 threads with an average concurrency of
 * 3.17 and NOTHING waiting at a barrier -- while every thread is handed exactly n/P output positions, so
 * the work is equal by construction. Equal work cannot produce that. This prices the alternative: the
 * phase spends most of its wall creating and joining the threads, and a worker that does not exist yet is
 * neither busy nor waiting, which is exactly what the profiler shows.
 *
 * PREDICTION, written first: create+join of 13 workers costs ~0.6-0.9 ms here (the ledger has 0.77 ms for
 * 13 empty workers), and the emit's actual fill work is ~0.4-0.6 ms, so 55-70% of that phase is spawn and
 * join. If instead the spawn is cheap (<0.1 ms), this explanation is wrong and the imbalance is elsewhere.
 *
 * Usage: ./spawnprobe [n] [threads] [reps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

typedef struct { double *a; size_t lo, hi; double val; int work; } arg_t;
static void *body(void *p) {
    arg_t *w = p;
    if (w->work) for (size_t i = w->lo; i < w->hi; i++) w->a[i] = w->val;
    return NULL;
}

static double run(double *a, size_t n, int P, int work, int reps) {
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        pthread_t th[64]; arg_t w[64];
        size_t chunk = (n + (size_t)P - 1) / (size_t)P;
        double t0 = now_ms();
        for (int t = 0; t < P; t++) {
            w[t].a = a; w[t].lo = (size_t)t * chunk; w[t].hi = w[t].lo + chunk > n ? n : w[t].lo + chunk;
            w[t].val = (double)t; w[t].work = work;
        }
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, body, &w[t]);   /* the caller runs slot 0, as par_run does */
        body(&w[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        double d = now_ms() - t0;
        if (d < best) best = d;
    }
    return best;
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 8388608;
    int P = argc > 2 ? atoi(argv[2]) : 14;
    int reps = argc > 3 ? atoi(argv[3]) : 15;
    double *a = malloc(n * sizeof(double));
    if (!a) return 2;
    for (size_t i = 0; i < n; i++) a[i] = 0.0;      /* pre-fault, so the fill measures stores and not page faults */

    double empty = run(a, n, P, 0, reps);
    double full = run(a, n, P, 1, reps);
    double seq = 0;
    { double best = 1e18;
      for (int r = 0; r < reps; r++) { double t0 = now_ms(); for (size_t i = 0; i < n; i++) a[i] = 1.0; double d = now_ms() - t0; if (d < best) best = d; }
      seq = best; }

    printf("spawnprobe: n = %zu, %d threads, best of %d\n\n", n, P, reps);
    printf("  create + join %d workers, NO work      %8.3f ms\n", P - 1, empty);
    printf("  the same, each filling n/%d doubles    %8.3f ms\n", P, full);
    printf("  the fill alone, one thread             %8.3f ms\n", seq);
    printf("\n  work in the parallel run   ~ %8.3f ms  (full - empty)\n", full - empty);
    printf("  spawn+join share of the phase %7.1f %%\n", 100.0 * empty / full);
    printf("  implied average concurrency   %7.2f    (work_total / wall, work_total = %.3f ms)\n",
           (full - empty > 0 ? (seq / full) : 0.0), seq);
    free(a);
    return 0;
}
