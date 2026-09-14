/* streamsprobe.c — where does the stream route's time go, table lookup against compare lookup?
 *
 * The 4096-cell table won 2.74x at 60 streams and LOST at 4 streams (0.81) and on 8 streams with 1% perturbed
 * values (0.68), while 8 shards at unequal rates gained 1.10. Build against a -DLOGOS_TRACE engine: every sort
 * prints a marker, then the route's phase stamps (lookup pass, scatter, bucket check, final copy), so the
 * difference can be read per phase instead of inferred from an end-to-end ratio. Each sort's wall time also goes
 * to stdout, so the same probe can run against the normal build. One thread, 8.4M, alternating.
 *
 * Usage: ./streamsprobe [reps] 2> trace.txt > wall.txt */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

static void streams(double *a, size_t n, size_t S, double perturb) {
    size_t ctr[64] = { 0 };
    for (size_t i = 0; i < n; i++) { size_t s = (size_t)(nr() % S); a[i] = (double)s * 1073741824.0 + (double)ctr[s]++; }
    if (perturb > 0) for (size_t k = 0; k < (size_t)(perturb * (double)n); k++) { size_t i = (size_t)(nr() % n); if (a[i] < 1073741824.0) a[i] = fr() * (double)ctr[0]; }
}
static void g_shard(double *a, size_t n) {
    size_t ctr[8] = { 0 };
    for (size_t i = 0; i < n; i++) { int s = 0; double u = fr() * 2.7178, acc = 0; for (s = 0; s < 7; s++) { acc += 1.0 / (s + 1); if (u < acc) break; } a[i] = (double)s * 1073741824.0 + (double)ctr[s]++; }
}
static void g_s4(double *a, size_t n)    { streams(a, n, 4, 0); }
static void g_s8(double *a, size_t n)    { streams(a, n, 8, 0); }
static void g_dirty(double *a, size_t n) { streams(a, n, 8, 0.01); }

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 5;
    size_t n = 8388608;
    double *base = malloc(n * 8), *w = malloc(n * 8);
    if (!base || !w) return 1;
    const struct { const char *name; void (*g)(double *, size_t); } SH[] = {
        { "streams_4", g_s4 }, { "streams_8", g_s8 }, { "streams_dirty", g_dirty }, { "shard_ids", g_shard } };
    for (int k = 0; k < 4; k++) {
        RS = 11 + (uint64_t)k; SH[k].g(base, n);
        for (int r = 0; r < reps; r++)
            for (int v = 0; v < 2; v++) {
                int cmp = (r + v) % 2;                      /* alternate which variant goes first */
                if (cmp) setenv("LOGOS_STREAMS_CMP", "1", 1); else unsetenv("LOGOS_STREAMS_CMP");
                memcpy(w, base, n * 8);
                fprintf(stderr, "=== %s %s %d\n", SH[k].name, cmp ? "compare" : "table", r);
                double t0 = now_ms();
                logos_sort_f64(w, n, 1);
                printf("%s %s %.2f\n", SH[k].name, cmp ? "compare" : "table", now_ms() - t0);
            }
    }
    unsetenv("LOGOS_STREAMS_CMP");
    free(base); free(w);
    return 0;
}
