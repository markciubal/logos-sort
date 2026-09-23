/* lattrace: where the lattice route's wall actually goes, phase by phase.
 *
 * Open direction 1 asks it directly: `make profile` shows 6.0-6.9 of 14 threads busy and NONE waiting inside the
 * phase that is 98-99% of this route's wall, but the profiler lumps map, merge, emit and teardown into ONE phase, and
 * an occupancy average cannot say which of them the idle time is in. This prints the trace timestamps that bracket
 * each of them, so the shares are read rather than inferred.
 *
 * Link against a -DLOGOS_TRACE engine (make lattrace). A trace build says WHICH path ran and what share of its own
 * time each phase took; it never ranks two implementations (see the ledger).
 *
 * Usage: ./lattrace [n] [threads] [shape]      shapes: quantized (default), modcard, lattice60k
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logos_psort.h"

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 8388608;
    int P = argc > 2 ? atoi(argv[2]) : 14;
    const char *shape = argc > 3 ? argv[3] : "quantized";
    double *a = malloc(n * sizeof(double));
    if (!a) return 2;
    RS = 7717;
    if (!strcmp(shape, "modcard")) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 30000) * 3.0 + 1.0;
    else if (!strcmp(shape, "lattice60k")) for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 60000) * 0.5;
    else for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 40000) * 0.25;

    fprintf(stderr, "== %s, n = %zu, %d threads ==\n", shape, n, P);
    logos_sort_f64(a, n, P);
    for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { fprintf(stderr, "UNSORTED at %zu\n", i); free(a); return 1; }
    fprintf(stderr, "(output verified sorted)\n");
    free(a);
    return 0;
}
