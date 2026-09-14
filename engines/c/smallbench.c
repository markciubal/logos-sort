/* smallbench.c — small-n policy A/B. Four engines in ONE binary, interleaved on identical data; every
 * row is the P-THREAD CALL (what a caller gets) with the 1-thread call of the shipped engine as the
 * reference column, at n = 32k .. 3.16M, best of many reps (small sorts are noisy):
 *   sm0  before: global policy one thread per 100k, no per-route caps
 *   sm1  relocation at most one thread per 500k elements   (RELOC_MIN_PER_THREAD; 250k measured close at 1M, worse at 3.16M)
 *   sm2  the counting routes (lowcard/modcard/lattice) at most one thread per 350k, never below 3 (COUNT_MIN_PER_THREAD, COUNT_MIN_THREADS)
 *   sm3  both = the shipped engine
 * Rejected on the way: a global 250k policy (cost uniform/near_sorted their 3-thread wins at 316k); a
 * counting cap without the floor (cost quantized 22% at 316k). SM_SET=1 builds the cap sweep instead.
 * Every output verified sorted and permutation-exact. Run: make smallbench && ./smallbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define N_LT(x) ((x) < (size_t)atol(getenv("SMALL_MIN_N")))
void sm0_sort_f64(double *, size_t, int); int sm0_num_cpus(void);
void sm1_sort_f64(double *, size_t, int); void sm2_sort_f64(double *, size_t, int); void sm3_sort_f64(double *, size_t, int);
typedef void (*sf)(double *, size_t, int);
#define NV 4
static sf F[NV] = { sm0_sort_f64, sm1_sort_f64, sm2_sort_f64, sm3_sort_f64 };
#if defined(SM_SET) && SM_SET == 1
static const char *VN[NV] = { "count100k", "count150k", "count250k", "count350k" };   /* all with the relocation cap at 500k and the floor of 3 */
#else
static const char *VN[NV] = { "before", "reloc500k", "count350k", "both=now" };
#endif
static uint64_t RS = 7; static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t xsum(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
static void gen_blocks(double *a, size_t n, size_t nb, int overlap) {
    size_t blk = (n + nb - 1) / nb; size_t *perm = malloc(nb * sizeof(size_t));
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk; if (overlap && (b & 7) == 1) base -= (double)blk * 0.1; a[i] = base + (double)(i % blk) + fr() * 0.5; }
    free(perm);
}
enum { S_UNIFORM, S_NEARSORT, S_HEAVY, S_QUANT, S_FEWDIST, S_INTWIDE, S_BLOCK, S_BLOCKOV, S_MODCARD, S_N };
static const char *NM[S_N] = { "uniform", "near_sorted", "heavy_tail", "quantized", "few_distinct", "int_wide", "block_perm", "block_overlap", "mod_card_30k" };
static void gen(double *a, size_t n, int d) {
    size_t i;
    switch (d) {
    case S_UNIFORM:  for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case S_NEARSORT: for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case S_HEAVY:    for (i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
    case S_QUANT:    for (i = 0; i < n; i++) a[i] = (double)(nr() % 4096) / 64.0; break;
    case S_FEWDIST:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    case S_INTWIDE:  for (i = 0; i < n; i++) a[i] = (double)(int32_t)nr(); break;
    case S_BLOCK:    gen_blocks(a, n, 64, 0); break;
    case S_MODCARD:  for (i = 0; i < n; i++) a[i] = (double)(nr() % 30000); break;
    default:         gen_blocks(a, n, 64, 1); break;
    }
}
int main(void) {
    int P = bench_threads(sm0_num_cpus());
    const size_t sizes[] = { 100000, 316000, 1000000, 3162000, 8000000 }; const int ns = 5;
    size_t NMAX = sizes[ns - 1]; double *base = malloc(NMAX * 8), *a = malloc(NMAX * 8);
    printf("small-n policy A/B — interleaved in one process, P-thread call, %d threads; reference = shipped engine at 1 thread\n\n", P);
    printf("  %-14s %9s %8s | %9s %9s %9s %9s   best P-call (x vs shipped P-call)\n", "shape", "n", "1T ref", VN[0], VN[1], VN[2], VN[3]);
    const char *only = getenv("SMALL_SHAPES");   /* comma-separated shape names to run, default all */
    for (int d = 0; d < S_N; d++) {
        if (only && !strstr(only, NM[d])) continue;
        for (int si = 0; si < ns; si++) {
            if (getenv("SMALL_MIN_N") && N_LT(sizes[si])) continue;
            size_t N = sizes[si]; int R = N <= 100000 ? 40 : N <= 1000000 ? 15 : N <= 3162000 ? 7 : 5;
            RS = 7 + (uint64_t)si; gen(base, N, d); uint64_t before = xsum(base, N);
            double ref = 1e300, b[NV] = { 1e300, 1e300, 1e300, 1e300 };
            for (int r = 0; r < R; r++) {
                memcpy(a, base, N * 8); double s = now(); sm0_sort_f64(a, N, 1); double x = now() - s; if (x < ref) ref = x;
                for (int v = 0; v < NV; v++) {
                    memcpy(a, base, N * 8); s = now(); F[v](a, N, P); x = now() - s; if (x < b[v]) b[v] = x;
                    if (r == 0) { if (xsum(a, N) != before) { printf("NOT A PERMUTATION (%s, %s)\n", NM[d], VN[v]); return 1; } for (size_t i = 1; i < N; i++) if (a[i - 1] > a[i]) { printf("UNSORTED (%s, %s)\n", NM[d], VN[v]); return 1; } }
                }
            }
            int best = 0; for (int v = 1; v < NV; v++) if (b[v] < b[best]) best = v;
            printf("  %-14s %9zu %7.2fms | %7.2fms %7.2fms %7.2fms %7.2fms   %s (%.2fx)%s\n", NM[d], N, ref, b[0], b[1], b[2], b[3], VN[best], b[0] / b[best], ref < b[best] ? "   <- 1T still faster" : "");
        }
    }
    printf("\nAll outputs verified sorted and permutation-exact.\n");
    return 0;
}
