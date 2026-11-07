/* twinbatch: ANY compile-time switch, paired on batches of DISTINCT inputs (2026-10-21, experiments/OTHER_FIELDS.md).
 *
 * bandbatch's method with the question left open: two copies of the engine, one built TW_OLD and one TW_NEW, each measurement a
 * batch of distinct instances (max(1, 2^22 / n) of them), adjacent and alternated, every output checked against qsort first. Built
 * twice with the copies swapped (twinbatch_a holds NEW in copy tw1, twinbatch_b in tw0): read the geometric mean of the two cells.
 *   make twinbatch TW_OLD=<flags> TW_NEW=<flags> && ./twinbatch_a 1 [pairs] [sizes] [filter] && ./twinbatch_b 0 [pairs] [sizes] [filter]
 * Cell: t(NEW) / t(OLD); BELOW 1 = the new build is faster. First use: -DLOGOS_AVX2_INT32 (the AVX2 kernel for int32-valued doubles),
 * which the ledger measured 0.83-0.94 the other way on ONE repeated input while fieldprobe small, on distinct inputs, put the bare
 * kernel 1.13-1.25 ahead of the int32 radix from 4096 to 262144. PREDICTION, written first: the kernel wins int_wide below ~1M
 * (0.80-0.90) and loses above it (1.05-1.20), so the answer is a size gate; the non-integer guard reads 1.00. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
void tw0_sort_f64(double *a, size_t n, int threads);
void tw1_sort_f64(double *a, size_t n, int threads);
static uint64_t rs;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *SH[] = { "int_wide", "int_1e6", "int_signed", "int_blocks", "int_near", "uniform" };
#define NSH ((int)(sizeof SH / sizeof SH[0]))
static void gen(int s, double *a, size_t n, uint64_t seed) {
    rs = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL + (uint64_t)s;
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() >> 33); break;                                  /* [0, 2^31) */
    case 1: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 1000000); break;
    case 2: for (size_t i = 0; i < n; i++) a[i] = (double)(int64_t)((nr() >> 33) - (1ULL << 30)) * 1.0; break;  /* [-2^30, 2^30) */
    case 3: { size_t B = 64, L = n / B ? n / B : 1; size_t perm[64]; for (size_t b = 0; b < B; b++) perm[b] = b;
              for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
              for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L + (i - b * L)) * 3.0; } break; }
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(i * 3); for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e9; break;
    }
}
int main(int argc, char **argv) {
    int new_slot = argc > 1 ? atoi(argv[1]) : 1, pairs = argc > 2 ? atoi(argv[2]) : 15;
    const char *sl = argc > 3 ? argv[3] : "4096,16384,65536,262144,1048576,4194304", *filt = argc > 4 ? argv[4] : "";
    size_t SZ[32]; int NSZ = 0;
    for (const char *p = sl; *p && NSZ < 32; ) { SZ[NSZ++] = (size_t)atof(p); while (*p && *p != ',') p++; if (*p == ',') p++; }
    printf("# twinbatch (NEW in copy tw%d): cell = t(NEW) / t(OLD), batches of distinct inputs, %d pairs; below 1: NEW is faster\n%-12s", new_slot, pairs, "shape");
    for (int k = 0; k < NSZ; k++) printf(" %8zu", SZ[k]);
    printf("\n");
    for (int s = 0; s < NSH; s++) {
        if (*filt && !strstr(SH[s], filt)) continue;
        printf("%-12s", SH[s]);
        for (int k = 0; k < NSZ; k++) {
            size_t n = SZ[k], R = n >= ((size_t)1 << 22) ? 1 : ((size_t)1 << 22) / n;
            double *base = malloc(R * n * 8), *w = malloc(R * n * 8), *rt = malloc((size_t)pairs * 8), *ref = malloc(n * 8);
            if (!base || !w || !rt || !ref) { printf(" %8s", "NOMEM"); free(base); free(w); free(rt); free(ref); continue; }
            for (size_t r = 0; r < R; r++) gen(s, base + r * n, n, r + 1);
            int bad = 0;
            memcpy(ref, base, n * 8); qsort(ref, n, 8, cmpd);
            for (int v = 0; v < 2 && !bad; v++) { memcpy(w, base, n * 8); if (v) tw1_sort_f64(w, n, 1); else tw0_sort_f64(w, n, 1); if (memcmp(w, ref, n * 8)) bad = 1; }
            if (bad) { printf(" %8s", "MISMATCH"); free(base); free(w); free(rt); free(ref); continue; }
            for (int i = 0; i < pairs; i++) {
                double t[2] = { 0, 0 };
                for (int q = 0; q < 2; q++) { int v = ((i & 1) == 0) == (q == 0) ? 0 : 1;
                    memcpy(w, base, R * n * 8);
                    double t0 = now_ms(); for (size_t r = 0; r < R; r++) { if (v) tw1_sort_f64(w + r * n, n, 1); else tw0_sort_f64(w + r * n, n, 1); } t[v] = now_ms() - t0; }
                rt[i] = t[new_slot] / t[1 - new_slot];
            }
            qsort(rt, (size_t)pairs, 8, cmpd);
            printf(" %8.3f", rt[pairs / 2]); fflush(stdout);
            free(base); free(w); free(rt); free(ref);
        }
        printf("\n");
    }
    return 0;
}
