/* j5bench.c — disjoint-run relocation (J5) A/B. Two copies of the engine
 * (-DLOGOS_NO_RELOCATE = pre-J5, and default = J5) live in this ONE binary and
 * run interleaved on identical data, so background load affects both equally
 * and cannot bias the result (pfbench pattern). Every output is verified sorted
 * and permutation-exact (XOR of raw bits).
 * Usage: ./j5bench            (all CPUs)      LOGOS_THREADS=N ./j5bench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
void rl0_sort_f64(double *, size_t, int); int rl0_num_cpus(void);
void rl1_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
static sf F[2] = { rl0_sort_f64, rl1_sort_f64 };
static const char *VN[2] = { "no-reloc", "reloc" };
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }

/* shuffled sorted blocks; overlap_every > 0 makes every value-block with index 1 mod overlap_every start 10% inside its predecessor */
static void gen_blocks(double *a, size_t n, size_t nb, int overlap_every) {
    size_t blk = (n + nb - 1) / nb; size_t *perm = malloc(nb * sizeof(size_t));
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
        if (overlap_every && (b % overlap_every) == 1) base -= (double)blk * 0.1;
        a[i] = base + (double)(i % blk) + fr() * 0.5; }
    free(perm);
}
enum { S_BP64, S_BP1024, S_BOVER, S_SEGMENTED, S_FEWRUNS, S_NEARSORT, S_UNIFORM, S_N };
static const char *NM[S_N] = { "block_perm64", "block_perm1k", "block_overlap", "segmented", "few_runs", "near_sorted", "uniform" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_BP64:   gen_blocks(a, n, 64, 0); break;
    case S_BP1024: gen_blocks(a, n, 1024, 0); break;
    case S_BOVER:  gen_blocks(a, n, 64, 8); break;
    case S_SEGMENTED: { size_t blk = n / 64; for (i = 0; i < n; i++) a[i] = (double)(i % blk) * 64.0 + (double)(i / blk) + fr() * 0.5; } break;
    case S_FEWRUNS:   { size_t blk = n / 4;  for (i = 0; i < n; i++) a[i] = (double)(i % blk) * 4.0 + (double)(i / blk) + fr() * 0.5; } break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
                     for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    default:         for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    }
}
int main(void) {
    int P = bench_threads(rl0_num_cpus());
    size_t N = 8000000; const int R = 5; double *base = malloc(N * 8), *a = malloc(N * 8);
    printf("J5 disjoint-run relocation A/B — interleaved in one process, n=8M, best of %d, %d threads\n", R, P);
    printf("(load affects both variants equally here; lowest time per row wins)\n");
    {   /* calibration (methodology rule 2): memcpy 64MB and a 1M single-thread sort of the
           no-reloc engine on uniform data; if these drift >15% from a quiet baseline, the machine
           is loaded and the table below is noise. Quiet 20-core WSL2 reference: ~2.5 ms / ~30 ms. */
        double *c1 = malloc(N * 8), *c2 = malloc(N * 8); RS = 3; for (size_t i = 0; i < N; i++) c1[i] = fr();
        double bm = 1e300, bs = 1e300;
        for (int r = 0; r < 5; r++) { double s = now(); memcpy(c2, c1, N * 8); double x = now() - s; if (x < bm) bm = x; }
        for (int r = 0; r < 5; r++) { memcpy(c2, c1, 1000000 * 8); double s = now(); rl0_sort_f64(c2, 1000000, 1); double x = now() - s; if (x < bs) bs = x; }
        printf("calibration: memcpy 64MB %.2f ms, logos(no-reloc) 1M uniform 1T %.1f ms\n\n", bm, bs);
        free(c1); free(c2);
    }
    printf("  %-14s %3s  %10s %10s   speedup\n", "shape", "thr", VN[0], VN[1]);
    for (int d = 0; d < S_N; d++) {
        RS = 7; gen(base, N, d); uint64_t before = xsum(base, N);
        for (int pass = 0; pass < 2; pass++) {
            int t = pass ? P : 1; double b[2] = { 1e300, 1e300 };
            for (int r = 0; r < R; r++) for (int v = 0; v < 2; v++) {
                memcpy(a, base, N * 8); double s = now(); F[v](a, N, t); double x = now() - s; if (x < b[v]) b[v] = x;
                if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; }
                for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; }
            }
            printf("  %-14s %3d  %8.1fms %8.1fms   %5.2fx\n", NM[d], t, b[0], b[1], b[0] / b[1]);
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact. Read the block_* rows for the effect and the others for regressions.\n");
    return 0;
}
