/* dllbench.c — the proposed "Moebius twist pointer routing" sort (experiments/moebius_dll/moebius_dll.js) in C, twice, against the
 * engine. Nothing here changes the engine.
 *
 *   moebius_faithful: the JavaScript as written. The array becomes a doubly linked list (one node array, which is kinder than a
 *     node per allocation); runs are found by walking it; a descending run is read back through .prev; every run is copied out
 *     contiguously; runs are merged pairwise, round after round (ping-pong between two buffers where the JS allocates new
 *     arrays), with the proposal's merge loop (`left[i] <= right[j]`).
 *   moebius_relink: the proposal's own pure-C form. Descending runs are turned round by swapping every node's next and prev
 *     (moebiusTwist), runs are merged by rewriting links (mergeInPlace, no scratch), and the final list is walked into the array.
 *
 * Both are single-threaded, so they are paired against logos_sort_f64 at ONE thread only (methodology rule 4). Every cell is the
 * median over adjacent pairs of t(logos) / t(moebius), order alternated: ABOVE 1 MEANS THE MOEBIUS SORT IS FASTER. Every output
 * is compared byte for byte with qsort's.
 *
 * PREDICTION, written before the first run (8.4M, one thread). Against the engine, every row below 1:
 *   faithful: clean sine 0.3-0.7 (65 runs, 7 merge rounds, plus a 200 MB node array built and walked), noisy sine 0.2-0.35 (runs of
 *     about two: ~22 rounds), uniform 0.15-0.3, near-sorted 0.1-0.3, shuffled blocks 0.1-0.25 (the engine places them by copies at
 *     2.3 ns per element), descending teeth 0.3-0.7 (the engine's twist already reads them as they lie), few distinct 0.05-0.2.
 *   relink: 0.3-0.7 of `faithful` on each row: after the first merge round the list's order no longer matches memory, so every
 *     later round and the final walk chase pointers through 200 MB.
 * STOP RULE: nothing replaces anything unless a row clears 1.05 against the engine.
 *
 * MEASURED, 2026-09-21 (20 pairs at 8.4M, 40 at 1M; t(logos) / t(moebius), so every number below 1 is how much slower it is):
 *                           faithful 8.4M / 1M     relink 8.4M / 1M
 *   sine + slope, clean       0.18 / 0.16            0.18 / 0.23
 *   sine + slope, noise 50    0.29 / 0.29            0.20 / 0.23
 *   descending teeth x64      0.51 / 0.46            0.13 / 0.17
 *   uniform                   0.10 / 0.11            0.02 / 0.04
 *   near_sorted 1%            0.31 / 0.37            0.17 / 0.27
 *   block_perm 64             0.09 / 0.09            0.09 / 0.13
 *   few_distinct              0.03 / 0.03            0.01 / 0.02
 * No row clears the stop rule; the best is the descending teeth at 0.46-0.51, where the proposal's one real idea (read a
 * descending run backwards) is the whole job and the engine's leaf still sorts it twice as fast. GRADED: noisy sine and teeth
 * inside the prediction; clean sine (0.3-0.7 predicted), uniform, blocks and few-distinct LOWER, because the engine is faster
 * on those than the prediction allowed (the clean wave is 0.29x a random sort for the engine; blocks are copies; few-distinct is
 * counted); near-sorted a little higher. relink against faithful: 0.2-0.7 as predicted on uniform, teeth, noise and few-distinct,
 * but EQUAL or BETTER on the clean sine and the blocks (1.0-1.45): with a few long runs the relinking merge reads nodes nearly in
 * memory order and saves the copy out, and at 1M the 24 MB of nodes sit in the last-level cache.
 *
 * usage: ./dllbench [pairs] [n]
 */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct dll_node { double val; struct dll_node *next, *prev; } dll_node_t;

static dll_node_t *array_to_dll(const double *a, size_t n) {
    dll_node_t *nd = malloc(n * sizeof *nd); if (!nd) return NULL;
    for (size_t i = 0; i < n; i++) { nd[i].val = a[i]; nd[i].next = i + 1 < n ? &nd[i + 1] : NULL; nd[i].prev = i ? &nd[i - 1] : NULL; }
    return nd;
}

/* ---------------------------------------------------------------- the JavaScript, as written */
static void merge_js(const double *l, size_t nl, const double *r, size_t nr, double *o) {
    size_t i = 0, j = 0, k = 0;
    while (i < nl && j < nr) { if (l[i] <= r[j]) o[k++] = l[i++]; else o[k++] = r[j++]; }
    while (i < nl) o[k++] = l[i++];
    while (j < nr) o[k++] = r[j++];
}
static void moebius_faithful(double *a, size_t n, int threads) {
    (void)threads;
    if (n <= 1) return;
    dll_node_t *head = array_to_dll(a, n);
    double *buf = malloc(n * sizeof(double)), *alt = malloc(n * sizeof(double));
    size_t *rs = malloc((n + 1) * sizeof(size_t)), *rs2 = malloc((n + 1) * sizeof(size_t));   /* run starts, and a sentinel */
    if (!head || !buf || !alt || !rs || !rs2) { free(head); free(buf); free(alt); free(rs); free(rs2); qsort(a, n, sizeof(double), bench_cmp_d_); return; }
    /* Pass 1: Tangle Condensation via Moebius Twist */
    size_t R = 0, w = 0;
    dll_node_t *curr = head;
    while (curr) {
        dll_node_t *start = curr; int desc = curr->next && curr->next->val < curr->val;
        while (curr->next) {
            if (desc && curr->next->val >= curr->val) break;
            if (!desc && curr->next->val < curr->val) break;
            curr = curr->next;
        }
        dll_node_t *end = curr, *nexts = curr->next;
        end->next = NULL; if (nexts) nexts->prev = NULL;
        rs[R++] = w;
        if (desc) { for (dll_node_t *t = end; t && t != start->prev; t = t->prev) buf[w++] = t->val; }   /* MOEBIUS TWIST: read .prev */
        else      { for (dll_node_t *t = start; t; t = t->next) buf[w++] = t->val; }
        curr = nexts;
    }
    rs[R] = w;
    /* Pass 2: Standard Merge of the pre-sorted runs, pairwise, round after round */
    double *src = buf, *dst = alt;
    while (R > 1) {
        size_t R2 = 0;
        for (size_t i = 0; i < R; i += 2) {
            rs2[R2++] = rs[i];
            if (i + 1 < R) merge_js(src + rs[i], rs[i + 1] - rs[i], src + rs[i + 1], rs[i + 2] - rs[i + 1], dst + rs[i]);
            else memcpy(dst + rs[i], src + rs[i], (rs[i + 1] - rs[i]) * sizeof(double));
        }
        rs2[R2] = rs[R];
        size_t *tr = rs; rs = rs2; rs2 = tr; R = R2;
        double *td = src; src = dst; dst = td;
    }
    memcpy(a, src, n * sizeof(double));
    free(head); free(buf); free(alt); free(rs); free(rs2);
}

/* ---------------------------------------------------------------- the proposal's pure-C form: twist and merge by relinking */
static dll_node_t *moebius_twist(dll_node_t *start, dll_node_t *end) {   /* as proposed: swap next and prev along start..end */
    dll_node_t *curr = start, *tmp = NULL, *new_head = end;
    while (curr) { tmp = curr->next; curr->next = curr->prev; curr->prev = tmp; if (curr == end) break; curr = tmp; }
    new_head->prev = NULL; start->next = NULL;
    return new_head;
}
static dll_node_t *merge_in_place(dll_node_t *l, dll_node_t *r) {        /* as proposed */
    if (!l) return r;
    if (!r) return l;
    dll_node_t *head, *tail;
    if (l->val <= r->val) { head = l; l = l->next; } else { head = r; r = r->next; }
    tail = head;
    while (l && r) {
        if (l->val <= r->val) { tail->next = l; l->prev = tail; tail = l; l = l->next; }
        else                  { tail->next = r; r->prev = tail; tail = r; r = r->next; }
    }
    if (l) { tail->next = l; l->prev = tail; }
    if (r) { tail->next = r; r->prev = tail; }
    return head;
}
static void moebius_relink(double *a, size_t n, int threads) {
    (void)threads;
    if (n <= 1) return;
    dll_node_t *nodes = array_to_dll(a, n);
    dll_node_t **heads = malloc(n * sizeof *heads);
    if (!nodes || !heads) { free(nodes); free(heads); qsort(a, n, sizeof(double), bench_cmp_d_); return; }
    size_t R = 0; dll_node_t *curr = nodes;
    while (curr) {
        dll_node_t *start = curr; int desc = curr->next && curr->next->val < curr->val;
        while (curr->next) {
            if (desc && curr->next->val >= curr->val) break;
            if (!desc && curr->next->val < curr->val) break;
            curr = curr->next;
        }
        dll_node_t *end = curr, *nexts = curr->next;
        end->next = NULL; if (nexts) nexts->prev = NULL;
        heads[R++] = desc ? moebius_twist(start, end) : start;
        curr = nexts;
    }
    while (R > 1) {
        size_t R2 = 0;
        for (size_t i = 0; i < R; i += 2) heads[R2++] = i + 1 < R ? merge_in_place(heads[i], heads[i + 1]) : heads[i];
        R = R2;
    }
    size_t w = 0; for (dll_node_t *t = heads[0]; t; t = t->next) a[w++] = t->val;
    free(nodes); free(heads);
}

static void logos_1(double *a, size_t n, int threads) { logos_sort_f64(a, n, threads); }

/* ---------------------------------------------------------------- the shapes of experiments/moebius_dll/bench.js */
static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static void gen(double *a, size_t n, int s) {
    RS = 2463534242u + (uint64_t)s;
    double T = (double)n / 32.0, A = 0.6 * T / 2.0;
    switch (s) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T); break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (fr() * 2 - 1) * 50.0; break;
    case 2: { size_t tl = (n + 63) / 64; for (size_t i = 0; i < n; i++) a[i] = (double)(tl - 1 - i % tl) + (double)(i / tl) * 1e-4 + fr() * 1e-5; } break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
            for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 5: { size_t nb = 64, blk = (n + nb - 1) / nb, perm[64]; for (size_t k = 0; k < nb; k++) perm[k] = k;
              for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
              for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5; } break;
    default: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 5) * 1e6; break;
    }
}
static const char *SN[] = { "sine + slope, clean", "sine + slope, noise 50", "descending teeth x64", "uniform", "near_sorted 1%", "block_perm 64", "few_distinct" };

int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 20; if (pairs < 4) pairs = 4;
    size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 8388608;
    (void)bench_threads_source; (void)bench_threads;
    double *base = malloc(n * 8), *w = malloc(n * 8), *ref = malloc(n * 8);
    if (!base || !w || !ref) return 2;
    printf("MOEBIUS DLL SORT (as proposed, in C) vs logos_sort_f64, ONE thread, n = %zu, %d pairs.\n", n, pairs);
    printf("each cell: median of t(logos) / t(moebius) [p10 p90]. ABOVE 1 = THE MOEBIUS SORT IS FASTER.\n\n");
    printf("  %-24s | %-26s | %-26s\n", "shape", "faithful (JS, translated)", "relink (proposal's C form)");
    for (int s = 0; s < 7; s++) {
        gen(base, n, s); memcpy(ref, base, n * 8); qsort(ref, n, 8, bench_cmp_d_);
        int okf = 1, okr = 1;
        memcpy(w, base, n * 8); moebius_faithful(w, n, 1); okf = !memcmp(w, ref, n * 8);
        memcpy(w, base, n * 8); moebius_relink(w, n, 1);   okr = !memcmp(w, ref, n * 8);
        double f10 = 0, f90 = 0, r10 = 0, r90 = 0;
        double f = bench_paired(logos_1, moebius_faithful, base, w, n, 1, pairs, &f10, &f90);
        double r = bench_paired(logos_1, moebius_relink, base, w, n, 1, pairs, &r10, &r90);
        printf("  %-24s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", SN[s], f, f10, f90, okf ? "      " : " WRONG", r, r10, r90, okr ? "      " : " WRONG");
        fflush(stdout);
    }
    printf("\nEvery output compared byte for byte with qsort's. Single-threaded code, so one thread only.\n");
    free(base); free(w); free(ref);
    return 0;
}
