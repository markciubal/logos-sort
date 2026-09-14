/* test.c — correctness and basic timing checks for logos-sort */

#define _POSIX_C_SOURCE 199309L  /* for clock_gettime */

#include "sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <math.h>

static int is_sorted(const double *a, size_t n) {
    for (size_t i = 1; i < n; i++) if (a[i - 1] > a[i]) return 0;
    return 1;
}

static int double_cmp(const void *x, const void *y) {
    double dx = *(const double *)x, dy = *(const double *)y;
    return (dx > dy) - (dx < dy);
}

static int multiset_equal(const double *a, const double *b, size_t n) {
    double *ca = (double *)malloc(n * sizeof(double));
    double *cb = (double *)malloc(n * sizeof(double));
    memcpy(ca, a, n * sizeof(double));
    memcpy(cb, b, n * sizeof(double));
    qsort(ca, n, sizeof(double), double_cmp);
    qsort(cb, n, sizeof(double), double_cmp);
    int eq = memcmp(ca, cb, n * sizeof(double)) == 0;
    free(ca); free(cb);
    return eq;
}

/* ---------------- generators ---------------- */

typedef void (*gen_t)(double *a, size_t n, unsigned *seed);

static double frand(unsigned *seed) {
    *seed = (*seed * 1664525u + 1013904223u);
    return (double)(*seed & 0xFFFFFFu) / (double)0x1000000;
}

static void gen_random(double *a, size_t n, unsigned *seed) {
    for (size_t i = 0; i < n; i++) a[i] = frand(seed) * 1e6;
}
static void gen_sorted(double *a, size_t n, unsigned *seed) {
    (void)seed; for (size_t i = 0; i < n; i++) a[i] = (double)i;
}
static void gen_reverse(double *a, size_t n, unsigned *seed) {
    (void)seed; for (size_t i = 0; i < n; i++) a[i] = (double)(n - i);
}
static void gen_equal(double *a, size_t n, unsigned *seed) {
    double v = frand(seed); for (size_t i = 0; i < n; i++) a[i] = v;
}
static void gen_skewed(double *a, size_t n, unsigned *seed) {
    /* power-law tail */
    for (size_t i = 0; i < n; i++) {
        double u = frand(seed);
        if (u < 1e-6) u = 1e-6;
        a[i] = pow(u, -1.5);
    }
}
static void gen_few_unique(double *a, size_t n, unsigned *seed) {
    for (size_t i = 0; i < n; i++) a[i] = (double)((unsigned)(frand(seed) * 8.0));
}

/* ---------------- correctness suite ---------------- */

static int correctness(void) {
    struct { const char *name; gen_t gen; } cases[] = {
        {"random",      gen_random},
        {"sorted",      gen_sorted},
        {"reverse",     gen_reverse},
        {"all-equal",   gen_equal},
        {"skewed-pow",  gen_skewed},
        {"few-unique",  gen_few_unique},
    };
    size_t sizes[] = {0, 1, 2, 24, 25, 100, 1000, 10000, 100000};
    int failed = 0;

    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            size_t n = sizes[si];
            unsigned seed = (unsigned)(ci * 1009 + si * 17 + 1);
            double *a = (double *)malloc((n + 1) * sizeof(double));
            double *orig = (double *)malloc((n + 1) * sizeof(double));
            cases[ci].gen(a, n, &seed);
            memcpy(orig, a, n * sizeof(double));

            int rc = ls_sort_double(a, n);
            int ok = (rc == 0) && is_sorted(a, n) && (n == 0 || multiset_equal(a, orig, n));

            printf("  %-12s n=%-7zu  %s\n", cases[ci].name, n, ok ? "✓" : "✗");
            if (!ok) failed++;
            free(a); free(orig);
        }
    }
    return failed;
}

/* ---------------- timing ---------------- */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench(void) {
    size_t sizes[] = {1000, 10000, 100000, 1000000};
    printf("\nbench: ls_sort_double vs qsort (median of 5 trials, uniform random)\n");
    printf("  n            ls_sort           qsort         speedup\n");
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        size_t n = sizes[si];
        unsigned seed = 42;
        double *a = (double *)malloc(n * sizeof(double));
        double *orig = (double *)malloc(n * sizeof(double));
        gen_random(orig, n, &seed);

        double tls = 1e9, tqs = 1e9;
        for (int t = 0; t < 5; t++) {
            memcpy(a, orig, n * sizeof(double));
            double t0 = now_s();
            ls_sort_double(a, n);
            double dt = now_s() - t0;
            if (dt < tls) tls = dt;
        }
        for (int t = 0; t < 5; t++) {
            memcpy(a, orig, n * sizeof(double));
            double t0 = now_s();
            qsort(a, n, sizeof(double), double_cmp);
            double dt = now_s() - t0;
            if (dt < tqs) tqs = dt;
        }
        printf("  %-10zu  %8.2f ms      %8.2f ms      %.2fx\n",
               n, tls * 1000.0, tqs * 1000.0, tqs / tls);
        free(a); free(orig);
    }
}

int main(void) {
    printf("logos-sort C library v%s\n\n", ls_sort_version());
    printf("Correctness:\n");
    int failed = correctness();
    if (failed > 0) {
        printf("\n✗ %d failures\n", failed);
        return 1;
    }
    bench();
    printf("\n✓ all correctness checks pass\n");
    return 0;
}
