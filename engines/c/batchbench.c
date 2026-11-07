/* batchbench: logos_sort_f64_batch against the loop a caller would write (logos_sort_f64 on every segment), paired.
 *
 * PREDICTION, written before the first run (2026-10-08). One thread: segments of 8 to 100 elements 1.00-1.05 (both take the
 * thin front; the loop pays one call more per segment), 1000 elements 1.05-1.25 (the batch skips the dispatcher's setup: the
 * pool's lock, a context, the shared sample, a relocation plan, against ~20 us of sorting), 10,000 and 50,000 1.00-1.10,
 * mixed lengths 1.0-1.1. Fourteen threads: every length up to 50,000 6-10x, because the loop sorts each segment under
 * 100,000 elements on ONE thread whatever it is asked for, and the batch spreads segments over the threads; mixed lengths
 * (some past 65,536, which keep the whole engine) 3-8x. RISK: structured segments (five distinct values, near-sorted) of
 * 10,000 elements at one thread, where the dispatcher has routes the leaf lacks: 0.7-1.0.
 *
 * usage: batchbench [pairs] [total elements] [threads list] [row filter]       ratio = loop / batch (> 1: the batch is faster) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "logos_psort.h"
#include "bench_common.h"

static uint64_t RS = 0x9E3779B97F4A7C15ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

static size_t *g_off; static size_t g_nseg;
static void loop_sort(double *a, size_t n, int P) { (void)n; for (size_t k = 0; k < g_nseg; k++) logos_sort_f64(a + g_off[k], g_off[k + 1] - g_off[k], P); }
static void batch_sort(double *a, size_t n, int P) { (void)n; logos_sort_f64_batch(a, g_off, g_nseg, P); }

enum { C_UNIFORM, C_FEW, C_NEAR, C_QUANT };
static const char *CN[] = { "uniform", "few5", "near", "quant" };
static size_t layout(size_t *off, size_t cap, size_t total, size_t len) {   /* len 0 = log-uniform in [2, 200000] */
    size_t k = 0, o = 0; off[0] = 0;
    while (o < total && k + 1 < cap) {
        size_t L = len ? len : (size_t)exp(log(2.0) + fr() * (log(200000.0) - log(2.0)));
        if (o + L > total) L = total - o;
        o += L; off[++k] = o;
    }
    return k;
}
static void fill(double *a, const size_t *off, size_t nseg, int content) {
    for (size_t k = 0; k < nseg; k++) {
        double *s = a + off[k]; size_t L = off[k + 1] - off[k];
        for (size_t i = 0; i < L; i++) s[i] = content == C_FEW ? (double)(nr() % 5) : content == C_NEAR ? (double)i + 0.5 : content == C_QUANT ? (double)(nr() % 4096) / 64.0 : fr() * 2e9 - 1e9;
        if (content == C_NEAR) for (size_t j = 0; j < L / 100 + 1 && L > 1; j++) { size_t x = nr() % L, y = nr() % L; double t = s[x]; s[x] = s[y]; s[y] = t; }
    }
}
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 30;
    size_t total = argc > 2 ? strtoull(argv[2], NULL, 10) : 8388608;
    const char *tl = argc > 3 ? argv[3] : "1,14";
    const char *filt = argc > 4 ? argv[4] : "";
    int th[8], nt = 0;
    for (const char *p = tl; *p && nt < 8; ) { th[nt++] = atoi(p); const char *c = strchr(p, ','); if (!c) break; p = c + 1; }
    double *base = malloc(total * sizeof(double)), *work = malloc(total * sizeof(double)), *ref = malloc(total * sizeof(double));
    g_off = malloc((total + 1) * sizeof(size_t));
    if (!base || !work || !ref || !g_off) return 1;
    struct { size_t len; int content; } rows[] = { { 8, C_UNIFORM }, { 32, C_UNIFORM }, { 100, C_UNIFORM }, { 1000, C_UNIFORM }, { 10000, C_UNIFORM },
                                                  { 50000, C_UNIFORM }, { 0, C_UNIFORM }, { 1000, C_FEW }, { 10000, C_FEW }, { 10000, C_NEAR }, { 0, C_NEAR },
                                                  { 10000, C_QUANT }, { 150000, C_UNIFORM }, { 150000, C_FEW }, { 150000, C_NEAR } };
    printf("# batchbench: %zu elements in all, %d pairs; ratio = loop of logos_sort_f64 / logos_sort_f64_batch (> 1: the batch is faster)\n", total, pairs);
    for (size_t r = 0; r < sizeof rows / sizeof *rows; r++) {
        char name[64]; snprintf(name, sizeof name, "%s_%s", CN[rows[r].content], rows[r].len ? "" : "mixed");
        if (rows[r].len) snprintf(name, sizeof name, "%s_%zu", CN[rows[r].content], rows[r].len);
        if (*filt && !strstr(name, filt)) continue;
        RS = 0x9E3779B97F4A7C15ULL + r * 101;
        g_nseg = layout(g_off, total + 1, total, rows[r].len);
        size_t n = g_off[g_nseg];
        fill(base, g_off, g_nseg, rows[r].content);
        printf("  %-16s %8zu segs", name, g_nseg);
        for (int t = 0; t < nt; t++) {
            memcpy(ref, base, n * sizeof(double)); loop_sort(ref, n, th[t]);
            memcpy(work, base, n * sizeof(double)); batch_sort(work, n, th[t]);
            if (memcmp(ref, work, n * sizeof(double))) { printf("  P=%d: OUTPUT DIFFERS\n", th[t]); return 2; }
            for (size_t k = 0; k < g_nseg; k++) for (size_t i = g_off[k] + 1; i < g_off[k + 1]; i++) if (work[i] < work[i - 1]) { printf("  P=%d: UNSORTED\n", th[t]); return 3; }
            double p10 = 0, p90 = 0, m = bench_paired(loop_sort, batch_sort, base, work, n, th[t], pairs, &p10, &p90);
            printf("   P=%-2d %6.3f [%.3f, %.3f]", th[t], m, p10, p90);
        }
        printf("\n");
    }
    return 0;
}
