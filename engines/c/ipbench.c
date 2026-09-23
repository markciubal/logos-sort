/* ipbench.c -- the strict in-place sort, in parallel: what the threads buy, and what zero scratch still costs.
 *
 * Until 2026-09-20 logos_sort_f64_inplace took an nthreads and ignored it. It now runs its placement passes as tasks
 * (logos_psort.c, inplace_parallel): the first pass over the whole array stays on one thread, everything under it is
 * spread over the threads. Two questions, both paired (bench_paired: the two sides adjacent in time, order alternating,
 * median of the ratios with p10 and p90), every output verified first:
 *
 *   P/1        the in-place sort at P threads against ITSELF at one thread. A scaling number, not a fair fight.
 *   vs buffered   the in-place sort at P threads against logos_sort_f64 at P threads: multi against multi. This is
 *              what zero bytes of scratch costs in time, where make memprobe says what the scratch costs in memory
 *              (0 against about 23 bytes per element at 14 threads).
 *
 * PREDICTIONS, written before the first run (2026-09-20):
 *   P/1 at 14 threads, uniform, 8M and 50M          2x to 3x: the first placement pass stays serial
 *   few_distinct (one varying byte)                 1.0x: its only pass IS the serial one
 *   quantized (two varying bytes)                   1.0x to 1.3x
 *   near_sorted, block_perm                         1.5x to 2.5x
 *   against the buffered sort at 14 threads         still 3x to 6x slower on uniform
 * GRADES (8.4M and 50M, 14 threads, 12 and 8 pairs):
 *   uniform P/1                 2.50x at 8.4M, 2.85x at 50M           RIGHT
 *   few_distinct                1.34x                                 WRONG premise: 0, 1e6 .. 4e6 differ in FOUR bytes (4-7), not one. But five
 *                                                                     values make a task tree two serial passes deep and at most four wide
 *   quantized                   1.45x                                 a little above the range
 *   near_sorted / block_perm    1.65x / 1.29x                         half right: block_perm spends nearly everything in the first pass
 *   against buffered at P       9.8x at 8.4M, 6.0x at 50M (uniform); 15-24x on the structured shapes      WRONG at 8.4M: I priced the
 *                               buffered sort at its hot-box speed. dominant_90 3.3x, the closest row.
 *   at the 2^20 threshold       1.02-2.26x, no row below 1            (not predicted; checked because a threshold is a claim)
 *
 * Usage: ./ipbench [pairs] [n] [shape]      default 12 pairs, 8388608 elements, every shape. LOGOS_THREADS=N. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static const char *SN[] = { "uniform", "heavy_tail", "bimodal", "near_sorted", "few_distinct", "quantized", "block_perm", "dominant_90" };
#define NSH 8
static void gen(double *a, size_t n, int s) {
    size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) { uint64_t r = nr(); double u = fr() * 2e9 - 1e9;
        switch (s) {
        case 0: a[i] = u; break;
        case 1: a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
        case 2: a[i] = ((r & 1) ? 1e9 : -1e9) + fr() * 10.0; break;
        case 3: a[i] = (double)i + fr() * 0.5; break;
        case 4: a[i] = (double)(r % 5) * 1e6; break;
        case 5: a[i] = (double)(r % 4096) / 64.0; break;
        case 6: a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5; break;
        default: a[i] = (r % 10) ? 12345.0 : u; break;
        } }
    if (s == 3) for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}
static inline uint64_t mix(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; }
static uint64_t mhash(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8); s += mix(b); } return s; }
static int sorted_perm(const double *w, size_t n, uint64_t h) { for (size_t i = 1; i < n; i++) if (w[i] < w[i - 1]) return 0; return mhash(w, n) == h; }

static void ip_one(double *a, size_t n, int t) { (void)t; logos_sort_f64_inplace(a, n, 1); }
static void ip_par(double *a, size_t n, int t) { logos_sort_f64_inplace(a, n, t); }
static void buf_par(double *a, size_t n, int t) { logos_sort_f64(a, n, t); }

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 12; size_t n = argc > 2 ? (size_t)atof(argv[2]) : 8388608;
    const char *only = argc > 3 ? argv[3] : NULL;
    int P = bench_threads(logos_num_cpus());
    double *base = malloc(n * 8), *w = malloc(n * 8);
    if (!base || !w) return 2;
    printf("strict in-place sort, n=%zu, %d threads, %d pairs; ratios are medians with p10..p90, above 1 = the left side is slower\n", n, P, pairs);
    printf("  %-13s | %-32s | %-32s | %s\n", "shape", "in-place: 1 thread / P threads", "in-place P / buffered P", "verified");
    int bad = 0;
    for (int k = 0; k < NSH; k++) {
        if (only && strcmp(only, SN[k])) continue;
        RS = 11 + (uint64_t)k; gen(base, n, k);
        uint64_t h = mhash(base, n); int ok = 1;
        memcpy(w, base, n * 8); ip_one(w, n, P); ok &= sorted_perm(w, n, h);
        memcpy(w, base, n * 8); ip_par(w, n, P); ok &= sorted_perm(w, n, h);
        memcpy(w, base, n * 8); buf_par(w, n, P); ok &= sorted_perm(w, n, h);
        double a10 = 0, a90 = 0, b10 = 0, b90 = 0;
        double sp = bench_paired(ip_one, ip_par, base, w, n, P, pairs, &a10, &a90);
        double vb = bench_paired(ip_par, buf_par, base, w, n, P, pairs, &b10, &b90);
        char c1[48], c2[48]; snprintf(c1, sizeof c1, "%5.2fx (%5.2f .. %5.2f)", sp, a10, a90); snprintf(c2, sizeof c2, "%5.2fx (%5.2f .. %5.2f)", vb, b10, b90);
        printf("  %-13s | %-32s | %-32s | %s\n", SN[k], c1, c2, ok ? "yes" : "NO");
        fflush(stdout); if (!ok) bad++;
    }
    (void)bench_threads_source;
    return bad ? 1 : 0;
}
