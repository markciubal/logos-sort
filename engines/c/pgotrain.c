/* pgotrain: the training run for a profile-guided build of ONE renamed engine copy (make pgobench). It calls <PGO_COPY>_sort_f64,
 * built with -fprofile-generate, on a broad mix -- fourteen shapes, sizes from 3 to 8M, one thread and fourteen -- so the profile is
 * not the five shapes ledgerbench then times. Every output is checked ascending: a profile taken over a broken sort is worthless.
 * Build: see make pgobench. usage: pgotrain [scale] (1 = full) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifndef PGO_COPY
#define PGO_COPY lg1
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
void CAT(PGO_COPY, _sort_f64)(double *, size_t, int);
#define SORT CAT(PGO_COPY, _sort_f64)

static uint64_t RS = 0x2545F4914F6CDD1DULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static void gen(double *a, size_t n, int s) {
    for (size_t i = 0; i < n; i++) {
        double u = fr();
        switch (s) {
        case 0: a[i] = u * 2e9 - 1e9; break;
        case 1: { double v = u < 1e-12 ? 1e-12 : u; a[i] = pow(v, -1.0 / 1.2); } break;
        case 2: a[i] = ((nr() & 1) ? 1e9 : -1e9) + u * 1e6; break;
        case 3: a[i] = (double)(nr() % 16) * 1e8 + u * 1e5; break;
        case 4: a[i] = (double)(int32_t)(uint32_t)nr(); break;
        case 5: a[i] = (double)(nr() % 5); break;
        case 6: a[i] = (double)(nr() % 4096) / 64.0; break;
        case 7: a[i] = (double)i + 0.5; break;
        case 8: a[i] = (double)(n - i); break;
        case 9: a[i] = (double)i + 0.5; break;                          /* near-sorted, swaps below */
        case 10: a[i] = (double)((i * 2654435761u) % n); break;          /* a permutation */
        case 11: a[i] = sin((double)i * 1e-4) * 1e3 + (double)i * 1e-3 + u; break;
        case 12: a[i] = (double)(float)(u * 2e6 - 1e6); break;
        default: a[i] = (double)(i / 4096 * 7919 % 64) * 4096.0 + (double)(i % 4096); break;   /* shuffled blocks */
        }
    }
    if (s == 9) for (size_t k = 0; k < n / 100 + 1; k++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; }
}
int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1; if (scale < 1) scale = 1;
    static const size_t sizes[] = { 3, 7, 16, 30, 100, 1000, 5000, 40000, 300000, 2000000, 8388608 };
    size_t nmax = 8388608;
    double *a = malloc(nmax * sizeof(double));
    if (!a) return 1;
    long sorts = 0;
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si], reps = n < 1000 ? 2000 / scale : n < 100000 ? 40 / scale : 2;
        if (reps < 1) reps = 1;
        for (int s = 0; s < 14; s++) for (int P = 1; P <= 14; P += 13) for (size_t r = 0; r < reps; r++) {
            gen(a, n, s); SORT(a, n, P); sorts++;
            for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { printf("pgotrain: UNSORTED, shape %d n=%zu P=%d\n", s, n, P); return 2; }
        }
    }
    printf("pgotrain: %ld sorts, all ascending\n", sorts);
    free(a);
    return 0;
}
