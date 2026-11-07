/* mixprobe (2026-10-24, the second memory round, FINDINGS 3bq): what ONE process keeps resident across a sequence of sorts of
   different shapes and thread counts. rssprobe starts every row in a fresh process, so it cannot see buffers that COEXIST: the
   partition's regions beside the pool, a context bundle per nested sort. The arrays are allocated and touched first; every line
   is resident bytes per element above them, after the sort named, and the last after logos_release_buffers(). ./mixprobe [n] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "logos_psort.h"
static uint64_t RS = 88172645463325252ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r"); if (!f) return -1;
    char line[256]; long v = -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) { v = atol(line + 6); break; }
    fclose(f); return v;
}
enum { UNI, BLOCKS, NEAR, QUANT, DOM50, I32WIDE, I32SPAN, STAIRS };
static void gen(double *a, int32_t *b, size_t n, int s) {
    if (s == I32WIDE) { for (size_t i = 0; i < n; i++) b[i] = (int32_t)(uint32_t)nr(); return; }
    if (s == I32SPAN) { for (size_t i = 0; i < n; i++) b[i] = (int32_t)(nr() % 1000); return; }
    for (size_t i = 0; i < n; i++) switch (s) {
        case UNI: a[i] = fr() * 2e9 - 1e9; break;
        case QUANT: a[i] = (double)(nr() % 30000) * 0.25; break;
        case DOM50: a[i] = (nr() & 1) ? 12345.0 : fr() * 2e9 - 1e9; break;
        case STAIRS: { size_t L = n / 20 + 1, bb = i / L; a[i] = (double)bb * (double)L * 0.9 + (double)(i - bb * L) + 0.25; } break;
        default: a[i] = (double)i + 0.5; break;
    }
    if (s == NEAR) for (size_t k = 0; k < n / 100; k++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; }
    if (s == BLOCKS) { size_t nb = 64, blk = n / nb, perm[64];
        for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), x = perm[k]; perm[k] = perm[r]; perm[r] = x; }
        for (size_t k = 0; k < nb; k++) for (size_t i = 0; i < blk; i++) a[k * blk + i] = (double)(perm[k] * blk + i) + 0.5; }
}
int main(int argc, char **argv) {
    size_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 4194304;
    double *a = malloc(n * sizeof(double)); int32_t *b = malloc(n * sizeof(int32_t));
    memset(a, 0, n * sizeof(double)); memset(b, 0, n * sizeof(int32_t));
    long base = rss_kb();
    struct { const char *name; int shape, P; } seq[] = {
        { "uniform, 14 threads (the partition)", UNI, 14 }, { "64 blocks, 1 thread (the knot route)", BLOCKS, 1 },
        { "1% swaps, 14 threads (near-sorted)", NEAR, 14 }, { "quantized, 1 thread (float32 radix)", QUANT, 1 },
        { "int32 wide, 1 thread (int32 radix)", I32WIDE, 1 }, { "half one value, 14 threads (dominant)", DOM50, 14 },
        { "uniform, 1 thread (AVX2 kernel)", UNI, 1 }, { "int32 span 1000, 14 threads (counting)", I32SPAN, 14 },
        { "staircase, 14 threads (knot, parallel)", STAIRS, 14 }, { "uniform, 14 threads again", UNI, 14 },
    };
    printf("# mixprobe: n = %zu; resident bytes per element above the arrays (%.1f MB), cumulative in ONE process\n", n, base / 1024.0);
    for (size_t k = 0; k < sizeof seq / sizeof seq[0]; k++) {
        RS = 1000 + k; gen(a, b, n, seq[k].shape);
        if (seq[k].shape == I32WIDE || seq[k].shape == I32SPAN) logos_sort_i32(b, n, seq[k].P); else logos_sort_f64(a, n, seq[k].P);
        printf("  %-42s %6.2f\n", seq[k].name, (double)(rss_kb() - base) * 1024.0 / (double)n);
    }
    logos_release_buffers();
    printf("  %-42s %6.2f\n", "after logos_release_buffers()", (double)(rss_kb() - base) * 1024.0 / (double)n);
    return 0;
}
