/* example.c — minimal use of logos_psort. Build: make example */
#include "logos_psort.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    size_t n = 1000000;
    double *a = malloc(n * sizeof(double));
    uint64_t s = 12345;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a[i] = (double)(s >> 11) / 9007199254740992.0 * 2e9 - 1e9; }

    int threads = logos_num_cpus();
    logos_sort_f64(a, n, threads);          /* ascending, in place */

    int ok = 1;
    for (size_t i = 1; i < n; i++) if (a[i - 1] > a[i]) { ok = 0; break; }
    printf("logos_psort %s: sorted %zu doubles on %d thread(s): %s\n",
           LOGOS_PSORT_VERSION, n, threads, ok ? "OK" : "FAILED");

    int32_t ints[] = { 5, -3, 9, 0, 2 };
    logos_sort_i32(ints, 5, 1);
    printf("int32: %d %d %d %d %d\n", ints[0], ints[1], ints[2], ints[3], ints[4]);
    free(a);
    return ok ? 0 : 1;
}
