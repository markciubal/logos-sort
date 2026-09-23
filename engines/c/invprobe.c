/* Does the sphere projection preserve ORDER across different indices?
 * The first collision test compared pairs at the SAME index, where proj_x = R*x/sqrt(x^2+c) is monotone
 * in x and so proves nothing. The index enters the radius, so the interesting pairs are at different z. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
static uint64_t RS = 7717;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
int main(void) {
    size_t n = 8388608;
    double *a = malloc(n * 8);
    if (!a) return 2;
    for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 40000) * 0.25;
    const double R = (double)n, PY = 1000003.0;
    size_t inv = 0, tested = 0, ties = 0;
    for (size_t k = 0; k < 2000000; k++) {
        size_t i = nr() % n, j = nr() % n;
        if (i == j) continue;
        double x1 = a[i], x2 = a[j];
        if (x1 == x2) { ties++; continue; }
        double z1 = (double)i, z2 = (double)j;
        double p1 = x1 * (R / sqrt(x1 * x1 + PY * PY + z1 * z1));
        double p2 = x2 * (R / sqrt(x2 * x2 + PY * PY + z2 * z2));
        int want = (x1 < x2), got = (p1 < p2);
        tested++;
        if (want != got) inv++;
    }
    printf("pairs at DIFFERENT indices: %zu tested, %zu order-INVERTED by the projection (%.1f%%), %zu equal-value skipped\n",
           tested, inv, 100.0 * (double)inv / (double)tested, ties);
    free(a);
    return 0;
}
