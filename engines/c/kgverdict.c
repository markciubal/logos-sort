/* The witness for the kink gate (2026-09-26): what verdict does the reach probe give three kinked-strand shapes, and how many
 * kinks does the detector repair, at P = 1, 4, 8? Shape 0 = the harness's RM_SPARSE (a[i] = i + 0.25, every 40th adjacent pair
 * swapped), 1 = kinkladder's one strand (random 0.5% of even pairs), 2 = the harness's RM_SPARSEENDS (a kink at each end too).
 * Every row must read DECLINED_KINKS; anything else means the near-sorted route took the array at P > 1. Written when the
 * harness passed RM_SPARSE expecting DECLINED_GATE, which said the gate had not fired: numbers instead of arithmetic. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logos_psort.h"
static const char *ST[] = { "not tried", "sorted", "sorted+strays", "DECLINED_GATE", "declined probe", "declined plan", "no memory", "DECLINED_KINKS" };
static uint64_t RS = 4242;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
int main(int argc, char **argv) {
    size_t n = argc > 1 ? strtoull(argv[1], 0, 10) : 300001;
    double *o = malloc(n * 8), *a = malloc(n * 8);
    int only = argc > 2 ? atoi(argv[2]) : -1;                                 /* a second argument runs one shape: 0, 1 or 2 */
    for (int shape = 0; shape < 3; shape++) {
        if (only >= 0 && shape != only) continue;
        size_t nk = 0;
        for (size_t i = 0; i < n; i++) o[i] = (double)i + (shape == 1 ? 0.5 : 0.25);
        if (shape == 0) { for (size_t i = 0; i + 1 < n; i += 40) { double t = o[i]; o[i] = o[i + 1]; o[i + 1] = t; nk++; } }
        else if (shape == 1) { RS = 4242; for (size_t x = 0; x + 1 < n; x += 2) if ((double)(nr() >> 11) / 9007199254740992.0 < 0.005) { double t = o[x]; o[x] = o[x + 1]; o[x + 1] = t; nk++; } }
        else { /* the harness RM_SPARSEENDS shape, verbatim: interior kinks from 41, plus one at EACH end */
            for (size_t i = 41; i + 1 < n - 2; i += 40) { double t = o[i]; o[i] = o[i + 1]; o[i + 1] = t; nk++; }
            { double t = o[0]; o[0] = o[1]; o[1] = t; t = o[n - 2]; o[n - 2] = o[n - 1]; o[n - 1] = t; nk += 2; } }
        printf("%s: n=%zu, %zu kinks (%.2f%%)\n", shape == 0 ? "harness RM_SPARSE" : shape == 1 ? "kinkladder one-strand" : "harness RM_SPARSEENDS", n, nk, 100.0 * nk / n);
        static const int PS[] = { 1, 4, 8 };
        for (int pi = 0; pi < 3; pi++) {
            memcpy(a, o, n * 8); logos_twist_last(4);
            logos_sort_f64(a, n, PS[pi]);
            int v = logos_reach_last(); size_t fixed = logos_twist_last(4);
            int srt = 1; for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { srt = 0; break; }
            printf("   P=%d  reach verdict = %-14s  kinks repaired = %-7zu %s\n", PS[pi], v >= 0 && v < 8 ? ST[v] : "?", fixed, srt ? "" : "UNSORTED");
        }
    }
    return 0;
}
