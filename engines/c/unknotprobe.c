/* unknotprobe.c — Reidemeister moves on the Moebius loop: the unknotting problem as a sort, after Kauffman & Lambropoulou,
 * "Hard Unknots and Collapsing Tangles" (homepages.math.uic.edu/~kauffman/IntellUnKnot.pdf). A probe: nothing here changes the engine.
 *
 * THE DICTIONARY. The array is closed into a ring: a circular doubly linked list, so that walking forward past the last element
 * arrives at the first. That ring is the knot diagram, and "unknotted" means it reads as ONE ascending strand from some node.
 *   strand        a maximal monotone arc of the ring (a run), with its orientation bit: a strand that descends in ring order is
 *                 read through its prev links. Forward in value is backward in the ring on a twisted strand (the Moebius reading).
 *   crossing      two strands cross where their value intervals overlap: sorting them needs comparisons there and nowhere else.
 *   R0            planar isotopy. On the SPHERE (the paper's diagrams live there: the outer region is a region like any other) a
 *                 rotation of the ring is free, so a sorted array read from the wrong place is already the unknot.
 *   R1 (kink)     an adjacent pair swapped inside a strand that otherwise goes on: swap it back, one crossing gone.
 *   R2 (bigon)    two strands NEXT to each other on the ring whose intervals overlap: pull them apart, i.e. merge them, touching
 *                 only the overlap (walk the lower strand BACK from its top and the upper one forward from its bottom, which is
 *                 what the second link is for). Simplifying: one strand fewer.
 *   R3 (slide)    a strand that crosses neither of its neighbours can slide past one of them, relinked in O(1). It changes no
 *                 crossing count; it makes an R2 available between two strands it separated.
 *   hard          the paper's hard unknot: no simplifying move is available, yet the diagram is not the unknot. Here: no two
 *                 RING-ADJACENT strands overlap, but some overlapping pair exists further apart. Only a move that does not
 *                 simplify (an R3 slide) gets out. The paper's lesson is that local moves stall and a GLOBAL reading (its tangle
 *                 fractions) sees the collapse; the engine's knot route is a global reading (a sweep over all 2R strand ends).
 *
 * MODES
 *   ring   R0, measured on the ENGINE: a shape rotated by 0.37 n (a ring buffer dumped from physical slot 0) against the shape
 *          itself (the ceiling: what a sort that treats the ring as a sphere would save), and against one rotation pass TOLD the
 *          offset plus the sort (simple). Paired, order alternated, 1 thread and P.
 *   moves  the unknotter itself, a literal circular doubly linked list, against logos_sort_f64 at ONE thread (it is sequential),
 *          paired, every output checked. Also prints, per shape, what the moves did: strands, kinks, R2 merges, R3 slides, how
 *          many times the local moves stalled (hard states), and what the loop-back joined.
 *   count  the move counts alone, one run per shape (no timing).
 *
 * PREDICTIONS, written before the first run (n = 8.4M unless noted).
 *   ring, 1 thread: the clean ring buffer (sorted, rotated) 0.95-1.05, because relocation sorts descriptors by value and is
 *     rotation-blind already; blocks and stairs the same (controls). Ring buffer with latency +-60 (short reach, rotated) 1.5-2.2:
 *     the reach ladder's top rungs straddle the seam; near-sorted 1% rotated 1.0-1.3 (it is a leaf shape at one thread already);
 *     4 streams rotated 1.5-3 (the arrival-order probe sees each stream fall once); grouped runs rotated 1.0-1.2. simple: the
 *     ceiling less a pass (~0.9 of it). uniform: ceiling 1.00, simple 0.93-0.97 (the price of a false positive).
 *     14 threads: 1.0-1.4 everywhere, the pass eating most of it, as with the mirror.
 *   moves: the unknotter is SLOWER than the engine on every shape. Closest where the diagram is nearly the unknot already:
 *     the clean ring buffer 0.4-0.9 (build the ring, one scan, one walk out: two passes of pointer work against the engine's
 *     relocation), blocks and stairs 0.3-0.6 (every strand is disjoint or crosses only its neighbour), short reach and
 *     near-sorted 0.05-0.2, uniform 0.01-0.05 (n/2 strands of two: a natural merge sort done by relinking, cache misses on
 *     every level after the first). The literal list route measured 0.50-0.57 on blocks (LOGOS_KNOT_LIST=1), which sets the
 *     scale. Hard states: none on blocks or rotated sorted data (nothing overlaps), some on staircases whose overlapping
 *     neighbours were shuffled apart, most on the shuffled staircase.
 *   STOP RULE: R0 is worth building into the engine only above 1.25 on a realistic shape at one thread, as for the mirror.
 *     The unknotter replaces nothing unless a row clears 1.05.
 *
 * MEASURED (2026-09-21, 8.4M). Mode `ring`, the engine BEFORE this round, 40 pairs, ceiling / simple:
 *   sorted, rotated (clean ring buffer)   1 thread 1.99 / 1.28     14 threads 2.59 / 1.58
 *   short reach +-60, rotated             1 thread 1.29 / 1.17     14 threads 1.95 / 1.13
 *   near_sorted 1%, rotated               1 thread 0.99 / 0.94     14 threads 1.37 / 0.82
 *   4 streams, rotated                    1 thread 1.04 / 0.89     14 threads 1.00 / 0.67
 *   runs by 8 coarse keys, rotated        1 thread 5.81 / 4.17     14 threads 0.86 / 0.56
 *   prefix_95 + tail, rotated             1 thread 1.82 / 1.54     14 threads 1.54 / 1.17
 *   uniform / block_perm / stairs x20     ceilings 0.97-1.01; simple 0.94 / 0.74 / 0.78 at 1 thread, 0.54-0.73 at 14
 *   staircase, blocks in order            1 thread 1.00 / 0.77     14 threads 0.47 / 0.35 (the ROTATED staircase is 2x faster: not traced)
 * GRADED. WRONG on the clean ring buffer (predicted 0.95-1.05, measured 1.99): relocation never saw it; the TAIL route took it
 * and merged the second run in element by element. WRONG on streams (predicted 1.5-3, measured 1.04): the stream route
 * tolerates one descent per stream and sorts the buckets that need it. WRONG on the keyed runs (predicted 1.0-1.2, measured
 * 5.8), and not about rotation at all: the relocation detector bailed at an average run of 4096 and those runs were EXACTLY
 * 4096; rotated, the first run is 984 long and the average falls 12 elements short (mode `cliff`). Low on short reach (1.29
 * against 1.5-2.2). Right on near-sorted, uniform and the block controls at one thread.
 * Mode `moves`, the unknotter against the engine at one thread, 20 pairs (noise rows 5): slower on 15 rows of 16. near_sorted
 * 0.36-0.37, block_perm 0.26, staircases 0.20-0.29, prefix 0.09-0.19, sorted 0.14 (0.28 rotated), short reach 0.08-0.11,
 * streams 0.07, uniform 0.009. The one row above 1 is the rotated keyed runs, 1.40: the engine's floor cliff, not the unknotter's
 * speed. Plane against sphere 0.96-1.09: the loop-back buys nothing INSIDE the unknotter, whose global reading sorts strands by
 * value and is rotation-blind. Moves: exactly ONE hard state in the whole table, on the shuffled staircase (no two ring
 * neighbours cross, yet 63 pairs further apart do), as predicted; every other stall was the size rule. The SWING (a cut at a gap:
 * the Culprit's arc taken out of its strand) is what made local moves affordable: nodes walked per element near_sorted 21.5 ->
 * 0.00, prefix 4.8 -> 1.0, keyed runs 7.0 -> 0, block_perm 4.0 -> 0, stairs x20 3.9 -> 1.5; noise has no gaps and no change.
 * GRADED: every row below 1 as predicted but one; lower than predicted on sorted, blocks and stairs (the list costs a node
 * written and walked per element even when no move is needed), higher on near_sorted (swings plus tangles leave only strays).
 * VERDICT: the unknotter is a measured dead end as a sort, like the literal list before it. R0 cleared the stop rule on the
 * ring buffer and the rotated prefix, and the probe found a cliff in the engine: both are BUILT (mode `ab`). Noisy rotations
 * (short reach, 1.17 simple) are below the stop rule and are not built.
 *
 * usage: ./unknotprobe [pairs] [n] [ring | moves | count | once | once2 | cliff | ab | nocopy | lis] [row filter]   (once: one sort per shape, for make unknottrace;
 *        cliff: the relocation detector's average-run floor, LOGOS_RELOC_AVG unset against =<4th argument, default 256)
 */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

/* ---------------------------------------------------------------------------------------------------------------------------
 * Shapes. ROT = 1 rotates the array by 0.37 n afterwards: a[i] = s[(i + r) mod n], a ring buffer read from physical slot 0.
 * ------------------------------------------------------------------------------------------------------------------------ */
enum { S_RING, S_REACH, S_NEAR, S_STREAMS, S_GROUPED, S_PREFIX, S_UNIFORM, S_BLOCKS, S_STAIRS, S_STAIRSHUF, S_N };
static const char *SN[S_N] = { "sorted (clean ring buffer)", "short reach, +-60", "near_sorted, 1% swaps", "4 ascending streams",
                               "sorted runs by coarse key", "prefix_95 + random tail", "uniform (CONTROL)", "block_perm 64",
                               "staircase, blocks in order", "stairs x20 (shuffled)" };
static const int ROTATES[S_N] = { 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 };       /* a rotated row only where the ORDER of positions is the structure */
static void gen(double *a, size_t n, int s) {
    RS = 99 + (uint64_t)s;
    size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    if (s != S_STAIRS) for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) switch (s) {
        case S_RING:    a[i] = (double)i + fr() * 0.5; break;
        case S_REACH:   a[i] = (double)i + (fr() * 2.0 - 1.0) * 60.0; break;
        case S_NEAR:    a[i] = (double)i + fr() * 0.5; break;
        case S_STREAMS: a[i] = (double)(i % 4) * (double)n + (double)(i / 4) + fr() * 0.5; break;
        case S_GROUPED: a[i] = (double)((i / 4096) % 8) * (double)n + (double)i + fr() * 0.5; break;
        case S_PREFIX:  a[i] = i < n - n / 20 ? (double)i : fr() * (double)n; break;
        case S_UNIFORM: a[i] = fr() * 2e9 - 1e9; break;
        case S_BLOCKS:  a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5; break;
        default:        a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + 1.0 / 9.0) + fr() * 0.5; break;   /* stairs: each block overlaps the next by 11% */
    }
    if (s == S_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}
static void rotate_left(double *a, size_t n, size_t r, double *tmp) {           /* a := a[r..n) ++ a[0..r): the smaller part through tmp */
    if (r == 0 || r >= n) return;
    if (r <= n - r) { memcpy(tmp, a, r * 8); memmove(a, a + r, (n - r) * 8); memcpy(a + n - r, tmp, r * 8); }
    else { size_t k = n - r; memcpy(tmp, a + r, k * 8); memmove(a + k, a, r * 8); memcpy(a, tmp, k * 8); }
}
static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

/* ---------------------------------------------------------------------------------------------------------------------------
 * The unknotter. One circular doubly linked list of n nodes (the Moebius loop); strands are arcs of it.
 * ------------------------------------------------------------------------------------------------------------------------ */
typedef struct { uint32_t head, tail; size_t len; double lo, hi; int rev; uint32_t sn, sp; int live; } strand_t;
typedef struct {
    size_t strands, kinks, swings, joined, r2, r2_nodes, r3, hard, blocked, tangles, forced, final_strands;   /* what the moves did */
} moves_t;
typedef struct {
    double *v; uint32_t *nx, *pv; size_t n;
    strand_t *S; size_t ns, cap;
    moves_t m;
    int plane;                                    /* 1: no loop-back (the diagram in the plane) */
    int noswing;                                  /* 1: no cuts at gaps (the Culprit's arc left in its strand) */
} ring_t;

/* first and last node of a strand in VALUE order, and the step from a node to its value successor inside the strand */
static inline uint32_t s_min(const ring_t *R, const strand_t *s) { (void)R; return s->rev ? s->tail : s->head; }
static inline uint32_t s_max(const ring_t *R, const strand_t *s) { (void)R; return s->rev ? s->head : s->tail; }
static inline uint32_t up(const ring_t *R, const strand_t *s, uint32_t x) { return s->rev ? R->pv[x] : R->nx[x]; }
static inline uint32_t dn(const ring_t *R, const strand_t *s, uint32_t x) { return s->rev ? R->nx[x] : R->pv[x]; }

static int add_strand(ring_t *R, uint32_t h, uint32_t t, size_t len, double lo, double hi, int rev) {
    if (R->ns == R->cap) { size_t c = R->cap ? R->cap * 2 : 1024; strand_t *S = realloc(R->S, c * sizeof *S); if (!S) return 0; R->S = S; R->cap = c; }
    strand_t *s = &R->S[R->ns++]; s->head = h; s->tail = t; s->len = len; s->lo = lo; s->hi = hi; s->rev = rev; s->live = 1; return 1;
}

/* The array's typical step: the median |a[p+1] - a[p]| over 1025 Weyl positions. A step more than UNKNOT_GAP_K times it is a
 * GAP: the arc across it passes over other strands' values without holding any of them (the paper's Culprit: "the arc that
 * passes underneath most of the diagram"). Noise has no gaps (its steps are all of one size), a sorted run with a stray in it
 * has two, a block boundary one. */
#ifndef UNKNOT_GAP_K
#define UNKNOT_GAP_K 1024.0
#endif
static double gap_limit(const double *v, size_t n) {
    enum { M = 1025 }; double d[M]; size_t k = 0; const double phi = 0.6180339887498949;
    if (n < 64) return INFINITY;
    for (size_t i = 0; i < M; i++) { size_t p = (size_t)(fmod((double)(i + 1) * phi, 1.0) * (double)(n - 1)); d[k++] = fabs(v[p + 1] - v[p]); }
    qsort(d, k, sizeof(double), cmpd);
    double med = d[k / 2];
    return med > 0 && isfinite(med) ? med * UNKNOT_GAP_K : INFINITY;   /* a median step of 0 (duplicates): no swings */
}

/* SCAN: build the ring and cut it into strands. R1 (kinks) is repaired where it stands, as the engine's detector does. A SWING
 * cuts a strand at a gap, so the long arc never becomes part of a strand: one more strand, and every false crossing gone
 * (the paper: a hard diagram must get more complex before it gets simpler). The loop-back: when the last strand and the first
 * continue each other across the wrap, they are ONE strand (R0 on the sphere). */
static int ring_scan(ring_t *R) {
    size_t n = R->n; double *v = R->v;
    for (size_t i = 0; i < n; i++) { R->nx[i] = (uint32_t)(i + 1 == n ? 0 : i + 1); R->pv[i] = (uint32_t)(i == 0 ? n - 1 : i - 1); }
    double G = R->noswing ? INFINITY : gap_limit(v, n);
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1; int rev = 0;
        if (j < n && fabs(v[j] - v[i]) > G) { R->m.swings++; }                /* a strand of one: the gap is on its right */
        else if (j < n && v[j] < v[i]) { rev = 1; while (j < n && v[j] <= v[j - 1]) { if (v[j - 1] - v[j] > G) { R->m.swings++; break; } j++; } }
        else {
            while (j < n) {
                if (v[j] >= v[j - 1]) { if (v[j] - v[j - 1] > G) { R->m.swings++; break; } j++; continue; }
                /* a kink: v[j-1] and v[j] swapped inside a run that goes on (both neighbours agree) */
                if (j >= i + 2 && j + 1 < n && v[j] >= v[j - 2] && v[j - 1] <= v[j + 1]) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; R->m.kinks++; j++; continue; }
                break;
            }
        }
        double lo = rev ? v[j - 1] : v[i], hi = rev ? v[i] : v[j - 1];
        if (!add_strand(R, (uint32_t)i, (uint32_t)(j - 1), j - i, lo, hi, rev)) return 0;
        i = j;
    }
    size_t ns = R->ns;
    if (!R->plane && ns >= 2) {                   /* the loop-back */
        strand_t *f = &R->S[0], *l = &R->S[ns - 1];
        int join = f->rev == l->rev && fabs(v[0] - v[n - 1]) <= G && (f->rev ? v[n - 1] >= v[0] : v[n - 1] <= v[0]);
        if (f->len == 1 && l->len == 1) join = 0;  /* two strands of one: their orientation bits say nothing */
        if (join) {                               /* the first strand's arc now starts at the last strand's head, across the wrap */
            f->head = l->head; f->len += l->len; f->lo = f->lo < l->lo ? f->lo : l->lo; f->hi = f->hi > l->hi ? f->hi : l->hi;
            R->ns--; ns--; R->m.joined = 1;
        }
    }
    for (size_t k = 0; k < ns; k++) { R->S[k].sn = (uint32_t)(k + 1 == ns ? 0 : k + 1); R->S[k].sp = (uint32_t)(k == 0 ? ns - 1 : k - 1); }
    R->m.strands = ns;
    return 1;
}

/* unlink strand s's arc from the ring (its internal links stay) and splice it right after strand d's arc */
static void arc_cut(ring_t *R, strand_t *s) { uint32_t a = R->pv[s->head], b = R->nx[s->tail]; R->nx[a] = b; R->pv[b] = a; }
static void arc_after(ring_t *R, strand_t *s, uint32_t after_node) {
    uint32_t b = R->nx[after_node]; R->nx[after_node] = s->head; R->pv[s->head] = after_node; R->nx[s->tail] = b; R->pv[b] = s->tail;
}

/* R2: A and B are ring neighbours (B follows A) and their intervals overlap. Merge them into A, touching only the overlap:
 * the part of the lower strand below the other's bottom, and the part of the upper strand above the other's top, stay linked
 * as they are. The merged strand is ascending in ring order and occupies A's place. */
static void move_r2(ring_t *R, uint32_t ia, uint32_t ib) {
    strand_t *A = &R->S[ia], *B = &R->S[ib]; double *v = R->v;
    uint32_t before = R->pv[A->head], after = R->nx[B->tail];          /* the ring outside the two arcs */
    int single = before == B->tail;                                     /* the two arcs ARE the whole ring */
    strand_t *L = A->lo <= B->lo ? A : B, *U = L == A ? B : A;          /* L holds the overall minimum */
    uint32_t lmin = s_min(R, L), lmax = s_max(R, L), umin = s_min(R, U), umax = s_max(R, U);
    size_t touched = 0;
    /* x: the first node of L at or above U's bottom, found by walking DOWN from L's top, so only L's overlap is touched */
    uint32_t x = lmax; while (x != lmin && v[dn(R, L, x)] >= U->lo) { x = dn(R, L, x); touched++; }
    /* y: the last node of U at or below L's top, found by walking UP from U's bottom */
    uint32_t y = umin; while (y != umax && v[up(R, U, y)] <= L->hi) { y = up(R, U, y); touched++; }
    /* read every link the merge will overwrite BEFORE it runs */
    uint32_t lpre_last = x == lmin ? UINT32_MAX : dn(R, L, x);          /* L's untouched part: lmin .. lpre_last */
    uint32_t usuf = y == umax ? UINT32_MAX : up(R, U, y);               /* U's untouched part: usuf .. umax */
    uint32_t head = UINT32_MAX, last = UINT32_MAX;
#define EMIT(node) do { uint32_t e_ = (node); if (last == UINT32_MAX) head = e_; else { R->nx[last] = e_; R->pv[e_] = last; } last = e_; } while (0)
    /* L's untouched part. Ascending in nx order already: kept as a unit. Twisted: turned round node by node (a flype costs the
       size of the subtangle it turns), each successor read before the node's links are rewritten. */
    if (lpre_last != UINT32_MAX) {
        if (!L->rev) { head = lmin; last = lpre_last; }
        else { uint32_t z = lmin; for (;;) { uint32_t nz = z == lpre_last ? UINT32_MAX : up(R, L, z); EMIT(z); if (nz == UINT32_MAX) break; z = nz; } }
    }
    /* the crossing: L from x to its top against U from its bottom to y, relinked in value order */
    {
        uint32_t p = x, q = umin; int pend = 0, qend = 0;
        while (!pend && !qend) {
            if (v[q] < v[p]) { uint32_t nq = q == y ? UINT32_MAX : up(R, U, q); EMIT(q); if (nq == UINT32_MAX) qend = 1; else q = nq; }
            else             { uint32_t np = p == lmax ? UINT32_MAX : up(R, L, p); EMIT(p); if (np == UINT32_MAX) pend = 1; else p = np; }
        }
        while (!pend) { uint32_t np = p == lmax ? UINT32_MAX : up(R, L, p); EMIT(p); if (np == UINT32_MAX) pend = 1; else p = np; }
        while (!qend) { uint32_t nq = q == y ? UINT32_MAX : up(R, U, q); EMIT(q); if (nq == UINT32_MAX) qend = 1; else q = nq; }
    }
    /* U's untouched part */
    if (usuf != UINT32_MAX) {
        if (!U->rev) { R->nx[last] = usuf; R->pv[usuf] = last; last = umax; }
        else { uint32_t z = usuf; for (;;) { uint32_t nz = z == umax ? UINT32_MAX : up(R, U, z); EMIT(z); if (nz == UINT32_MAX) break; z = nz; } }
    }
#undef EMIT
    R->m.r2++; R->m.r2_nodes += touched;
    if (single) { R->nx[last] = head; R->pv[head] = last; }             /* put the chain back where the two arcs were */
    else { R->nx[before] = head; R->pv[head] = before; R->nx[last] = after; R->pv[after] = last; }
    double lo = A->lo < B->lo ? A->lo : B->lo, hi = A->hi > B->hi ? A->hi : B->hi;
    A->head = head; A->tail = last; A->len += B->len; A->lo = lo; A->hi = hi; A->rev = 0;
    B->live = 0;
    uint32_t bn = B->sn; A->sn = bn; R->S[bn].sp = ia;
}
static inline int overlap(const strand_t *a, const strand_t *b) { return a->lo < b->hi && b->lo < a->hi; }

/* The moves. Rounds of R2 around the ring, each strand merging at most once per round, and only with a neighbour of comparable
 * size (within UNKNOT_BALANCE), so that noise costs n log R and a list never pays a full walk to insert one small strand into a
 * long one. A round in which the size rule blocked every candidate is followed by one forced round. When no candidate is left
 * and the diagram is still not split, it is HARD: the GLOBAL reading takes over. The strands by value fall into TANGLES
 * (maximal chains of overlapping intervals); each tangle's strands are slid together on the ring (R3) and marked, and the
 * rounds collapse each tangle into one strand, disjoint members included (the paper: a tangle collapses as a whole, which no
 * sequence of simplifying local moves sees). With no overlap left the strands are pairwise disjoint and are read out in value
 * order, which on the sphere is the same as reading the unknot from its minimum. */
#ifndef UNKNOT_BALANCE
#define UNKNOT_BALANCE 8
#endif
typedef struct { double lo, hi; uint32_t id; } sref_t;
static int cmp_sref(const void *x, const void *y) { const sref_t *a = x, *b = y; if (a->lo != b->lo) return (a->lo > b->lo) - (a->lo < b->lo); return (a->hi > b->hi) - (a->hi < b->hi); }
static void slide_after(ring_t *R, uint32_t ib, uint32_t ia) {         /* R3: B's arc out of its place, in right after A's */
    strand_t *A = &R->S[ia], *B = &R->S[ib];
    arc_cut(R, B); arc_after(R, B, A->tail);
    uint32_t bp = B->sp, bn = B->sn; R->S[bp].sn = bn; R->S[bn].sp = bp;
    uint32_t an = A->sn; A->sn = ib; B->sp = ia; B->sn = an; R->S[an].sp = ib;
    R->m.r3++;
}
static int ring_unknot(ring_t *R, double *out) {
    size_t live = R->ns, ns = R->ns ? R->ns : 1;
    uint32_t *stamp = calloc(ns, 4), *gid = calloc(ns, 4); if (!stamp || !gid) { free(stamp); free(gid); return 0; }
    uint32_t round = 0, any = 0, ngroups = 0;
    sref_t *o = NULL;
    for (int phase = 0; ; phase++) {
        int forced = 0, stalled_on_size = 0;
        for (;;) {                                                      /* rounds */
            round++; size_t merged = 0, cand = 0, cnt = live, seen = 0;
            while (!R->S[any].live) any++;
            uint32_t s = any;
            while (seen < cnt && live > 1) {
                uint32_t t = R->S[s].sn; strand_t *A = &R->S[s], *B = &R->S[t];
                if (t != s && stamp[s] != round && stamp[t] != round && (overlap(A, B) || (gid[s] && gid[s] == gid[t]))) {
                    cand++;
                    size_t big = A->len > B->len ? A->len : B->len, small = A->len > B->len ? B->len : A->len;
                    if (forced || big <= (size_t)UNKNOT_BALANCE * small) {
                        if (forced && big > (size_t)UNKNOT_BALANCE * small) R->m.forced++;
                        move_r2(R, s, t); live--; merged++; stamp[s] = round; seen += 2;
                    } else seen++;
                } else seen++;
                s = R->S[s].sn;
            }
            if (live <= 1 || !cand) break;
            /* the size rule blocked everything. Inside tangles, force one round; before any tangle is known, a forced round would
               let one long strand swallow its small neighbours one walk at a time, so the global reading goes first. */
            if (!merged && !phase) { stalled_on_size = 1; break; }
            forced = merged == 0;
        }
        if (live <= 1) break;
        /* the global reading: the strands by value */
        free(o); o = malloc(live * sizeof *o); if (!o) { free(stamp); free(gid); return 0; }
        size_t k = 0; for (size_t s = 0; s < R->ns; s++) if (R->S[s].live) { o[k].lo = R->S[s].lo; o[k].hi = R->S[s].hi; o[k].id = (uint32_t)s; k++; }
        qsort(o, live, sizeof *o, cmp_sref);
        int crossing = 0; for (size_t t = 0; t + 1 < live && !crossing; t++) crossing = o[t].hi > o[t + 1].lo;
        if (!crossing) {                                                /* split: pairwise disjoint strands, read by value */
            size_t w = 0;
            for (size_t t = 0; t < live; t++) { strand_t *S = &R->S[o[t].id]; uint32_t z = s_min(R, S); for (size_t c = 0; c < S->len; c++) { out[w++] = R->v[z]; z = up(R, S, z); } }
            R->m.final_strands = live; free(o); free(stamp); free(gid); return w == R->n;
        }
        if (phase > 64) { free(o); free(stamp); free(gid); return 0; }  /* cannot happen: one tangle phase collapses every tangle */
        if (stalled_on_size) R->m.blocked++; else R->m.hard++;      /* hard: NO two ring neighbours cross, the paper's case */
        /* tangles: maximal runs of the value order held together by overlaps (the running top above the next bottom) */
        size_t t0 = 0; double top = o[0].hi;
        for (size_t t = 1; t <= live; t++) {
            if (t < live && o[t].lo < top) { if (o[t].hi > top) top = o[t].hi; continue; }
            if (t - t0 >= 2) {                                          /* a tangle of t - t0 strands: slide them together, in value order */
                ngroups++; R->m.tangles++;
                for (size_t q = t0; q < t; q++) gid[o[q].id] = ngroups;
                for (size_t q = t0 + 1; q < t; q++) if (R->S[o[q - 1].id].sn != o[q].id) slide_after(R, o[q].id, o[q - 1].id);
            }
            if (t < live) { t0 = t; top = o[t].hi; }
        }
    }
    free(o); free(stamp); free(gid);
    /* one strand: the unknot. Read it from its minimum, forward or backward as its bit says (R0: where the ring starts is free). */
    for (size_t s = 0; s < R->ns; s++) if (R->S[s].live) {
        strand_t *S = &R->S[s]; uint32_t z = s_min(R, S); for (size_t c = 0; c < R->n; c++) { out[c] = R->v[z]; z = up(R, S, z); }
        R->m.final_strands = 1; return 1;
    }
    return 0;
}

/* the whole sort: a copy into the node values, the ring, the moves, the unknot read back into a */
static int unknot_sort(double *a, size_t n, int plane, int noswing, moves_t *mv) {
    if (n < 2) return 1;
    if (n >= UINT32_MAX) return 0;
    ring_t R; memset(&R, 0, sizeof R); R.n = n; R.plane = plane; R.noswing = noswing;
    R.v = malloc(n * 8); R.nx = malloc(n * 4); R.pv = malloc(n * 4); double *out = a;
    int ok = R.v && R.nx && R.pv;
    if (ok) { memcpy(R.v, a, n * 8); ok = ring_scan(&R) && ring_unknot(&R, out); }
    if (mv) *mv = R.m;
    free(R.v); free(R.nx); free(R.pv); free(R.S);
    return ok;
}

/* ---------------------------------------------------------------------------------------------------------------------------
 * Paired timing. Side 0 and side 1 are adjacent, order alternated; median of t0 / t1.
 * ------------------------------------------------------------------------------------------------------------------------ */
enum { K_ENGINE, K_ENGINE_TOLD_ROT, K_UNKNOT, K_UNKNOT_PLANE };
static size_t g_rot = 0; static double *g_tmp = NULL;
static int run_kind(int kind, double *w, size_t n, int P) {
    switch (kind) {
        case K_ENGINE: logos_sort_f64(w, n, P); return 1;
        case K_ENGINE_TOLD_ROT: rotate_left(w, n, n - g_rot, g_tmp); logos_sort_f64(w, n, P); return 1;   /* undo a left rotation by r */
        case K_UNKNOT: return unknot_sort(w, n, 0, 0, NULL);
        default: return unknot_sort(w, n, 1, 0, NULL);
    }
}
static double pair(const double *A, int ka, const double *B, int kb, double *w, size_t n, int P, int reps, double *p10, double *p90, int *ok) {
    double *r = malloc((size_t)reps * sizeof(double)); *ok = r != NULL; if (!r) return 0;
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int side = (i & 1) ? 1 - k : k;
            memcpy(w, side ? B : A, n * 8);
            double s = now_ms(); int good = run_kind(side ? kb : ka, w, n, P); t[side] = now_ms() - s;
            if (!good || !sorted_ok(w, n)) *ok = 0;
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; free(r); return med;
}
/* the unknotter must return the same multiset as the engine: compare its output with the engine's, bit for bit */
static int same_bits(const double *x, const double *y, size_t n) { return memcmp(x, y, n * 8) == 0; }

/* ---------------------------------------------------------------------------------------------------------------------------
 * MODE `cliff`: what the rotation row found. The run detector behind relocation and the knot route gives up at 256 runs whose
 * average is under RELOC_MIN_AVG_RUN (4096, the run MERGE's constant). Runs of exactly 4096 pass by nothing; the same runs rotated
 * fail by 12 elements and fall to the leaf. A ladder of run lengths, each paired as LOGOS_RELOC_AVG unset (4096) against a lower
 * setting, same binary, same engine copy.
 * ------------------------------------------------------------------------------------------------------------------------ */
enum { C_BLOCKS, C_STAIRS, C_KEYS, C_RANDOM, C_N };
static const char *CN[C_N] = { "disjoint blocks, shuffled", "staircase (11%), shuffled", "runs by 8 coarse keys", "uniform (GUARD)" };
static void gen_cliff(double *a, size_t n, int s, size_t b) {
    RS = 7 + (uint64_t)s * 131 + b;
    size_t nb = (n + b - 1) / b; size_t *perm = malloc(nb * sizeof *perm); if (!perm) exit(2);
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) switch (s) {
        case C_BLOCKS: a[i] = (double)perm[i / b] * (double)b + (double)(i % b) + fr() * 0.5; break;
        case C_STAIRS: a[i] = (double)perm[i / b] * (double)b + (double)(i % b) * (1.0 + 1.0 / 9.0) + fr() * 0.5; break;
        case C_KEYS:   a[i] = (double)((i / b) % 8) * (double)n + (double)i + fr() * 0.5; break;
        default:       a[i] = fr() * 2e9 - 1e9; break;
    }
    free(perm);
}
static const char *ENV_B = NULL, *ENV_B_VAL = NULL;      /* set around side B's sort only */
static double pair_env(const double *A, double *w, size_t n, int P, int reps, double *p10, double *p90, double *msA, int *ok) {
    double *r = malloc((size_t)reps * sizeof(double)), *ta = malloc((size_t)reps * sizeof(double)); *ok = r && ta; if (!r || !ta) { free(r); free(ta); return 0; }
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int side = (i & 1) ? 1 - k : k;
            memcpy(w, A, n * 8);
            if (side) setenv(ENV_B, ENV_B_VAL, 1);
            double s = now_ms(); logos_sort_f64(w, n, P); t[side] = now_ms() - s;
            if (side) unsetenv(ENV_B);
            if (!sorted_ok(w, n)) *ok = 0;
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0; ta[i] = t[0];
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd); qsort(ta, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; *msA = ta[reps / 2]; free(r); free(ta); return med;
}
static int run_cliff(int reps, size_t n, int P, const char *val, const char *filt, double *s, double *w) {
    static const size_t B[] = { 256, 512, 1024, 2048, 3000, 4000, 4096, 4200, 8192 };
    ENV_B = "LOGOS_RELOC_AVG"; ENV_B_VAL = val;
    printf("THE RELOCATION DETECTOR'S AVERAGE-RUN FLOOR, n = %zu, %d pairs, P = %d: t(LOGOS_RELOC_AVG unset: the shipped floor) / t(LOGOS_RELOC_AVG=%s).\n", n, reps, P, val);
    printf("Above 1 = the lower floor is faster. ns/el: the shipped setting's median time per element.\n\n");
    printf("  %-28s %6s | %-24s %7s | %-24s %7s\n", "shape", "run", "1 thread", "ns/el", "P threads", "ns/el");
    for (int k = 0; k < C_N; k++) for (size_t q = 0; q < sizeof B / sizeof B[0]; q++) {
        if (filt && !strstr(CN[k], filt)) continue;
        if (k == C_RANDOM && q > 0) continue;
        gen_cliff(s, n, k, B[q]);
        double lo1, hi1, ms1, loP, hiP, msP; int ok1, okP;
        double c1 = pair_env(s, w, n, 1, reps, &lo1, &hi1, &ms1, &ok1), cP = pair_env(s, w, n, P, reps, &loP, &hiP, &msP, &okP);
        printf("  %-28s %6zu | %6.3f [%.2f %.2f]%s %7.2f | %6.3f [%.2f %.2f]%s %7.2f\n", CN[k], k == C_RANDOM ? (size_t)0 : B[q],
               c1, lo1, hi1, ok1 ? "  " : " X", ms1 * 1e6 / (double)n, cP, loP, hiP, okP ? "  " : " X", msP * 1e6 / (double)n);
        fflush(stdout);
    }
    printf("\nRESULT: ratios only; every output checked ascending.\n");
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------
 * MODE `ab`: the three engine changes this probe led to, each against its own switch, ONE binary and one engine copy (no layout
 * term). Every cell: median of t(switch set) / t(shipped), [p10 p90]; above 1 = the change is faster.
 *   loop-back   LOGOS_NO_LOOPBACK=1         a suffix that continues into the prefix across the wrap, rotated once
 *   floor       LOGOS_RELOC_AVG=4096        the relocation detector's floor at one thread, 4096 -> 256
 *   slack       LOGOS_RELOC_NO_SLACK=1      one partial run forgiven per detection chunk (matters at P threads)
 * PREDICTIONS, written before the first run (8.4M).
 *   loop-back  ring buffer 1T 1.25-1.35, PT 1.4-1.6 (the told rotation's `simple` column in mode ring); prefix_95 rotated 1T
 *              1.45-1.55, PT 1.1-1.2; 30% prefix + noise + 40% suffix below it 1T 1.5-2.5, PT 1.1-1.5 (without the loop-back no
 *              cheap route sees it); two sorted blocks swapped 1T 1.2-1.4, PT 1.3-1.6; every guard 0.98-1.02.
 *   floor      1T: blocks of 1000 4.0-4.8, staircase of 2000 3.3-3.9, keyed runs of 3000 5.2-6.0, the rotated keyed runs of 4000
 *              4.5-5.8 (at 4096 the slack alone admits them); PT every row 0.98-1.02 (the parallel floor did not change); guards 0.98-1.02.
 *   slack      PT: runs of exactly 4096 1.1-1.4 (the ladder's 4096 rows at P threads); 1T and every guard 0.98-1.02.
 * SHIP RULE: no row under 0.97 over two runs, and the rows each change was built for above 1.1.
 * MEASURED, two runs of 40 pairs each:
 *   floor      1 thread: blocks of 1000 4.21 / 4.16, staircase of 2000 3.58 / 3.60, keyed runs of 3000 5.54 / 5.59, the rotated
 *              keyed runs of 4000 7.47 / 7.59; guards 0.985-1.012. 14 threads: every row 0.975-1.012. SHIPPED.
 *   slack      0.974-1.026 on every row at both thread counts. NOT KEPT: at P threads the per-thread run cap (n / floor / P + 2)
 *              binds before the average does (a chunk of exactly-4096 runs holds 148 counting its two partial pieces).
 *   loop-back  first cut, re-entering the whole dispatcher: ring buffer 0.89 at 1 thread, 1.12-1.13 at 14; two blocks 0.88 /
 *              1.05-1.06; prefix_95 rotated 1.35-1.38 / 1.01-1.03. A ring buffer is three or four passes of traffic, and the
 *              re-entry's prescan re-read all of it. Fixed (the rotation knows [0, L + d) ascends: return at once when that is
 *              the whole array, else call the tail route on the remainder directly; a first fix that HINTED the re-entry's
 *              prescan instead cost every sorted call of 2-32 elements 10-16%, make callbench): ring buffer 1.63 / 1.56 at 1 thread and 1.90 / 1.94
 *              at 14; prefix_95 rotated 1.73 / 1.75 and 1.48 / 1.51; 30% + noise + 40% 2.07 / 2.08 and 1.00 / 1.01 (not offered
 *              at 14 threads: its first cut lost 0.51 there at 1M); two blocks swapped 1.56 / 1.58 and 1.81 / 1.78; every guard
 *              0.98-1.02. The shipped form (tail route called directly, one run): ring buffer 1.63 / 2.03, prefix 1.75 / 1.46,
 *              30% + noise + 40% 2.14 / 1.00, two blocks 1.57 / 1.90, guards 0.97-1.02. SHIPPED.
 * GRADED: the loop-back beat every prediction once it stopped re-reading (ring buffer 1.25-1.35 / 1.4-1.6 predicted); the
 * floor landed inside three predictions of four and above the fourth; the slack was WRONG (predicted 1.1-1.4, measured 1.00),
 * because the wrong condition was diagnosed: it read the average test, and the cap binds first.
 * ------------------------------------------------------------------------------------------------------------------------ */
/* ROUND SIX (2026-09-22): five builds from "where else can the knot view pay?", each against its own switch. PREDICTIONS, written
 * before the 40-pair runs. Honesty note: a 4-pair smoke run had already shown WAVE 1.36 and DIRECT 0.68 on one row each.
 *   PIVOT   (LOGOS_AV_STRIDED=1; the wave route off on BOTH sides, or it takes the sine first): sine 32 cycles noise 50 1.05-1.15
 *           at one thread, swing 2 1.03-1.12; sine 2000 cycles 1.00 (the reach route takes it); uniform, heavy tail, bimodal
 *           0.97-1.03. 14 threads 1.00 everywhere (the parallel leaf never calls the AVX2 quicksort).
 *   SEAM    rotated short reach 1.15-1.25 at one thread (a told rotation read 1.17, and the block phase it no longer wastes is
 *           most of the 1.29 ceiling); 1.00 at 14 (sequential only); rotated near_sorted, short reach as it lies, uniform 0.98-1.02.
 *   STRAY   blocks + 0.1% strays 2.5-4.0 at one thread, + 1% 2.0-3.5 (strandprobe's told ceiling was 1.68 and 2.22 through a told
 *           sort whose own 0% row read 0.61); + 3% (past the n/64 cap) 0.95-1.00, the price of reading to the cap; 1.00 at 14;
 *           block_perm, stairs x20, uniform 0.98-1.02.
 *   WAVE    sine 32 cycles noise 50 1.5-2.5 at one thread, swing 2 1.3-2.0, 8 cycles 1.5-2.5 (the composed route's told ceiling
 *           was 2.1-3.7); 1.00 at 14; guards (the clean sine, which the knot route takes first; 2000 cycles, which the reach
 *           route takes; uniform; near_sorted; streams) 0.98-1.02.
 *   DIRECT  triple and quad 1.05-1.25 at one thread (one pass where staging made three); k8 (staged still) and stairs x20 1.00;
 *           1.00 at 14 (the parallel knot gate declines these shapes there).
 * SHIP RULE, the same as before: no row under 0.97 over two runs, the rows each change was built for above 1.1.
 * MEASURED, 8.4M, 40 pairs, four runs (the first two of the first cuts, the last two after the reworks named below):
 *   PIVOT   0.996-1.009 on every row, sine and guard, both runs. REVERTED. WRONG: the wave's cost in the AVX2 quicksort is not
 *           its pivot. The second diagnosis of that cost to fail (the first was the leaf choice, FINDINGS 3ai).
 *   SEAM    rotated short reach 1.150 / 1.123 / 1.151 / 1.157 at one thread; guards 0.98-1.03; 14 threads 0.98-1.00. SHIPPED.
 *           Right, at the low end. The seam is found in the reach route's FENCES (the block medians, read before a block is sorted):
 *           ascending everywhere but at one block, where they drop by most of the range.
 *   STRAY   first cut: 0.1% 4.18 / 4.14, 1% 3.17 / 3.17, but 3% 0.92 / 0.93 and the clean guards 0.94-0.96 (block_perm, stairs).
 *           The guards paid for a 1 MB stray list allocated on every sort (it is now taken at the first stray), and 3% for a scan
 *           that read to the n/64 cap before declining (it now stops once the density shows the cap will not hold). After:
 *           0.1% 4.05 / 4.01, 1% 3.26 / 3.00, 3% 1.00 / 0.99, every guard 0.99-1.01, 14 threads 0.99-1.02. SHIPPED. Above the
 *           prediction at 0.1%, inside it at 1%; WRONG on the guards and on 3% until the reworks.
 *   WAVE    first cut: 32 cycles swing 0.6 1.35 / 1.35, 8 cycles 1.17 / 1.17, and swing 2 0.76 / 0.77: a swing of several periods
 *           of drift is covered four and five pieces deep and the sweep's k-way merges cost more than the leaf. A crossing-cost gate
 *           from the sample (1.37 levels per element where it won, 2.76 where it lost; limit 1.5) and a median filter for the pieces:
 *           32 cycles 1.24 / 1.21, 8 cycles 1.13 / 1.11, 16 cycles noise 20 1.12 / 1.12, swing 1 and 2 declined (1.00-1.01), every
 *           guard 0.99-1.01, 14 threads 0.97-1.03. SHIPPED. LOW against the prediction (1.5-2.5) and the told ceiling (2.1-3.7).
 *   DIRECT  triple 0.682 / 0.683, quad 0.531 / 0.523, guards 1.00. REVERTED. WRONG in direction: one pass with a chain of two
 *           dependent compares per element loses to two memcpys and branch-free two-way merges.
 * Mode `nocopy` (40 pairs, one run; t(shipped) / t(output left in scratch)): block_perm 1.331 at one thread / 1.345 at 14, stairs
 *   x20 1.209 / 1.329, staircase in order 1.206 / 1.028, runs by coarse key 1.298 / 0.985. Above the prediction at one thread
 *   on block_perm, inside it elsewhere. What a sorted-description API would save; not a sort.
 * Mode `lis`: sorted 1.000; ring buffer 0.630 plain / 1.000 cyclic; blocks + 1% strays 0.233; block_perm 0.235; near_sorted 0.977;
 *   prefix_95 0.952; short reach 1.000; uniform 0.040; sine 0.509; 4 streams 0.258; stairs x20 0.216 (predicted 0.25-0.35, the one
 *   miss). As predicted it separates one strand with strays from many strands, and cannot see strays inside blocks, which the
 *   detector's own scan now lifts. Not wired in: no route this round needed it as a gate. */
typedef struct { const char *name, *env, *val; int gen, rot; size_t b; } abrow_t;
enum { G_MAIN, G_CLIFF, G_SMALLPREFIX, G_TWOBLOCKS, G_SINE, G_STRAYS, G_STAIRS, G_HEAVY, G_BIMODAL };
/* value(i) = i + A sin(2 pi i / T) + noise, T = n / cycles, A = swing * T / 2 (sineprobe's shape); the noisy 32-cycle wave is the one
   the engine sorts 1.22-1.31x SLOWER than random data at one thread */
static void gen_sine(double *a, size_t n, int cycles, double swing, double noise, uint64_t seed) {
    RS = seed; double T = (double)n / (double)cycles, A = swing * T / 2.0;
    for (size_t i = 0; i < n; i++) a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (noise > 0 ? (fr() * 2.0 - 1.0) * noise : 0.0);
}
/* 64 sorted blocks, shuffled, and a fraction f of positions overwritten with random values in [0, n): strays inside strands */
static void gen_strays(double *a, size_t n, double f, uint64_t seed) {
    gen(a, n, S_BLOCKS); RS = seed;
    for (size_t q = 0; q < (size_t)(f * (double)n); q++) a[nr() % n] = fr() * (double)n;
}
/* knotbench's staircase: 64 shuffled blocks, block b covering [b, b + 1 + o) block-widths of value: o = 1.5 keeps three strands live, 2.5 four, 7.0 eight */
static void gen_stairs_o(double *a, size_t n, double o, uint64_t seed) {
    RS = seed; size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), x = perm[k]; perm[k] = perm[r]; perm[r] = x; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5;
}
static void gen_ab(double *a, size_t n, const abrow_t *r, double *tmp) {
    if (r->gen == G_MAIN) gen(a, n, (int)r->b);
    else if (r->gen == G_CLIFF) gen_cliff(a, n, (int)(r->b >> 32), (size_t)(r->b & 0xffffffffu));
    else if (r->gen == G_SMALLPREFIX) { RS = 4711; size_t p = n * 3 / 10, q = n * 6 / 10;
        for (size_t i = 0; i < n; i++) a[i] = i < p ? (double)n + (double)i : i < q ? fr() * 2.0 * (double)n : (double)(i - q); }
    else if (r->gen == G_TWOBLOCKS) { RS = 4712; size_t h = n / 3; for (size_t i = 0; i < n; i++) a[i] = i < n - h ? (double)(h + i) + fr() * 0.5 : (double)(i - (n - h)) + fr() * 0.5; }
    else if (r->gen == G_SINE) gen_sine(a, n, (int)(r->b >> 32), (double)((r->b >> 16) & 0xffff) / 100.0, (double)(r->b & 0xffff), 777);
    else if (r->gen == G_STRAYS) gen_strays(a, n, (double)r->b / 100000.0, 4713);
    else if (r->gen == G_STAIRS) gen_stairs_o(a, n, (double)r->b / 100.0, 4714);
    else if (r->gen == G_HEAVY) { RS = 4715; for (size_t i = 0; i < n; i++) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); }
    else { RS = 4716; for (size_t i = 0; i < n; i++) { double u = fr(); a[i] = (nr() & 1) ? u * 1e6 : 5e6 + u * 1e6; } }
    if (r->rot) rotate_left(a, n, (size_t)(0.37 * (double)n), tmp);
}
#define CL(shape, b) ((size_t)(shape) << 32 | (size_t)(b))
#define SINE(cycles, swing100, noise) ((size_t)(cycles) << 32 | (size_t)(swing100) << 16 | (size_t)(noise))
static int run_ab(int reps, size_t n, int P, const char *filt, double *s, double *w, double *tmp) {
    static const abrow_t R[] = {
        { "LOOP  ring buffer (sorted, rotated)",   "LOGOS_NO_LOOPBACK", "1", G_MAIN, 1, S_RING },
        { "LOOP  prefix_95 + tail, rotated",        "LOGOS_NO_LOOPBACK", "1", G_MAIN, 1, S_PREFIX },
        { "LOOP  30% prefix, noise, 40% suffix",    "LOGOS_NO_LOOPBACK", "1", G_SMALLPREFIX, 0, 0 },
        { "LOOP  two sorted blocks swapped",        "LOGOS_NO_LOOPBACK", "1", G_TWOBLOCKS, 0, 0 },
        { "LOOP  guard: prefix_95 as it lies",      "LOGOS_NO_LOOPBACK", "1", G_MAIN, 0, S_PREFIX },
        { "LOOP  guard: short reach, rotated",      "LOGOS_NO_LOOPBACK", "1", G_MAIN, 1, S_REACH },
        { "LOOP  guard: near_sorted 1%",            "LOGOS_NO_LOOPBACK", "1", G_MAIN, 0, S_NEAR },
        { "LOOP  guard: uniform",                   "LOGOS_NO_LOOPBACK", "1", G_MAIN, 0, S_UNIFORM },
        { "LOOP  guard: block_perm 64",             "LOGOS_NO_LOOPBACK", "1", G_MAIN, 0, S_BLOCKS },
        { "FLOOR disjoint blocks of 1000",          "LOGOS_RELOC_AVG", "4096", G_CLIFF, 0, CL(C_BLOCKS, 1000) },
        { "FLOOR staircase of 2000, shuffled",      "LOGOS_RELOC_AVG", "4096", G_CLIFF, 0, CL(C_STAIRS, 2000) },
        { "FLOOR runs by 8 keys, 3000",             "LOGOS_RELOC_AVG", "4096", G_CLIFF, 0, CL(C_KEYS, 3000) },
        { "FLOOR runs by 8 keys, 4000, rotated",    "LOGOS_RELOC_AVG", "4096", G_CLIFF, 1, CL(C_KEYS, 4000) },
        { "FLOOR guard: blocks of 8192",            "LOGOS_RELOC_AVG", "4096", G_CLIFF, 0, CL(C_BLOCKS, 8192) },
        { "FLOOR guard: uniform",                   "LOGOS_RELOC_AVG", "4096", G_MAIN, 0, S_UNIFORM },
        { "FLOOR guard: stairs x20 (64 blocks)",    "LOGOS_RELOC_AVG", "4096", G_MAIN, 0, S_STAIRSHUF },
        /* round six, 2026-09-21: five builds from the question "where else can the knot view pay?" (predictions in the header below) */
        { "SEAM  short reach +-60, rotated",        "LOGOS_NO_SEAM", "1", G_MAIN, 1, S_REACH },
        { "SEAM  guard: near_sorted 1%, rotated",   "LOGOS_NO_SEAM", "1", G_MAIN, 1, S_NEAR },
        { "SEAM  guard: short reach as it lies",    "LOGOS_NO_SEAM", "1", G_MAIN, 0, S_REACH },
        { "SEAM  guard: uniform",                   "LOGOS_NO_SEAM", "1", G_MAIN, 0, S_UNIFORM },
        { "STRAY blocks + 0.1% strays",             "LOGOS_NO_STRAY", "1", G_STRAYS, 0, 100 },
        { "STRAY blocks + 1% strays",               "LOGOS_NO_STRAY", "1", G_STRAYS, 0, 1000 },
        { "STRAY blocks + 3% strays (past the cap)", "LOGOS_NO_STRAY", "1", G_STRAYS, 0, 3000 },
        { "STRAY guard: block_perm 64",             "LOGOS_NO_STRAY", "1", G_MAIN, 0, S_BLOCKS },
        { "STRAY guard: stairs x20 (shuffled)",     "LOGOS_NO_STRAY", "1", G_MAIN, 0, S_STAIRSHUF },
        { "STRAY guard: uniform",                   "LOGOS_NO_STRAY", "1", G_MAIN, 0, S_UNIFORM },
        { "WAVE  sine 32 cycles, swing 0.6, noise 50", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(32, 60, 50) },
        { "WAVE  sine 32 cycles, swing 2, noise 50", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(32, 200, 50) },
        { "WAVE  sine 8 cycles, swing 0.6, noise 50", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(8, 60, 50) },
        { "WAVE  sine 32 cycles, swing 1, noise 50", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(32, 100, 50) },
        { "WAVE  sine 16 cycles, swing 0.6, noise 20", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(16, 60, 20) },
        { "WAVE  guard: sine 32 cycles, no noise",  "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(32, 60, 0) },
        { "WAVE  guard: sine 2000 cycles, noise 50", "LOGOS_NO_WAVE", "1", G_SINE, 0, SINE(2000, 60, 50) },
        { "WAVE  guard: uniform",                   "LOGOS_NO_WAVE", "1", G_MAIN, 0, S_UNIFORM },
        { "WAVE  guard: near_sorted 1%",            "LOGOS_NO_WAVE", "1", G_MAIN, 0, S_NEAR },
        { "WAVE  guard: 4 ascending streams",       "LOGOS_NO_WAVE", "1", G_MAIN, 0, S_STREAMS },
    };
    printf("A/B OF EVERY CHANGE THIS PROBE LED TO, n = %zu, %d pairs, P = %d: t(switch set) / t(shipped), same array both sides.\n", n, reps, P);
    printf("Above 1 = the change is faster. ONE binary, one engine copy.\n\n  %-38s | %-24s | %-24s\n", "row (switch)", "1 thread", "P threads");
    for (size_t k = 0; k < sizeof R / sizeof *R; k++) {
        if (filt && !strstr(R[k].name, filt)) continue;
        gen_ab(s, n, &R[k], tmp);
        ENV_B = R[k].env; ENV_B_VAL = R[k].val;
        int both = 0;                                                  /* (the pivot's rows ran with the wave route off on BOTH sides; reverted) */
        double lo1, hi1, ms1, loP, hiP, msP; int ok1, okP;
        double c1 = pair_env(s, w, n, 1, reps, &lo1, &hi1, &ms1, &ok1), cP = pair_env(s, w, n, P, reps, &loP, &hiP, &msP, &okP);
        if (both) unsetenv("LOGOS_NO_WAVE");
        /* pair_env returns t(shipped) / t(switch set): turn it round so that above 1 = the change is faster */
        printf("  %-38s | %6.3f [%.2f %.2f]%s | %6.3f [%.2f %.2f]%s\n", R[k].name, 1.0 / c1, 1.0 / hi1, 1.0 / lo1, ok1 ? "  " : " X", 1.0 / cP, 1.0 / hiP, 1.0 / loP, okP ? "  " : " X");
        fflush(stdout);
    }
    printf("\nRESULT: ratios only; every output checked ascending.\n");
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------
 * MODE `nocopy`: the ceiling of a run route that hands back a sorted DESCRIPTION instead of copying the placed array back
 * (open direction 0f (b), the sorted view over run descriptors). LOGOS_RELOC_NOCOPY=1 makes both placements leave their output
 * in scratch, so side B's output is WRONG BY DESIGN and is not checked; the ratio prices the copy-back pass and nothing else.
 * PREDICTION, written first: block_perm 1.15-1.25 at one thread (the copy-back is one of a run route's ~3.5 pass-equivalents),
 * 1.25-1.40 at 14 (traffic is the cost there); stairs x20 1.10-1.20 / 1.15-1.30. It is what a view without materialisation would
 * gain on these shapes; it does not ship as a sort, since a sort must return the array.
 * ------------------------------------------------------------------------------------------------------------------------ */
static double pair_env_nocheck(const double *A, double *w, size_t n, int P, int reps, double *p10, double *p90) {
    double *r = malloc((size_t)reps * sizeof(double)); if (!r) return 0;
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int side = (i & 1) ? 1 - k : k;
            memcpy(w, A, n * 8);
            if (side) setenv(ENV_B, ENV_B_VAL, 1);
            double s = now_ms(); logos_sort_f64(w, n, P); t[side] = now_ms() - s;
            if (side) unsetenv(ENV_B);
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0;
    }
    qsort(r, (size_t)reps, sizeof(double), cmpd);
    double med = (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
    *p10 = r[(int)(0.10 * reps)]; *p90 = r[(int)(0.90 * reps)]; free(r); return med;
}
static int run_nocopy(int reps, size_t n, int P, double *s, double *w, double *tmp) {
    static const abrow_t R[] = {
        { "block_perm 64",           "LOGOS_RELOC_NOCOPY", "1", G_MAIN, 0, S_BLOCKS },
        { "stairs x20 (shuffled)",   "LOGOS_RELOC_NOCOPY", "1", G_MAIN, 0, S_STAIRSHUF },
        { "staircase, blocks in order", "LOGOS_RELOC_NOCOPY", "1", G_MAIN, 0, S_STAIRS },
        { "sorted runs by coarse key", "LOGOS_RELOC_NOCOPY", "1", G_MAIN, 0, S_GROUPED },
    };
    printf("THE COPY-BACK PASS PRICED, n = %zu, %d pairs, P = %d: t(shipped) / t(LOGOS_RELOC_NOCOPY=1, output LEFT IN SCRATCH: wrong by design, unchecked).\n", n, reps, P);
    printf("Above 1 = what a route that returns a sorted DESCRIPTION would save. Not a sort: a ceiling.\n\n  %-30s | %-24s | %-24s\n", "shape", "1 thread", "P threads");
    for (size_t k = 0; k < sizeof R / sizeof *R; k++) {
        gen_ab(s, n, &R[k], tmp); ENV_B = R[k].env; ENV_B_VAL = R[k].val;
        double lo1, hi1, loP, hiP;
        double c1 = pair_env_nocheck(s, w, n, 1, reps, &lo1, &hi1), cP = pair_env_nocheck(s, w, n, P, reps, &loP, &hiP);
        printf("  %-30s | %6.3f [%.2f %.2f]   | %6.3f [%.2f %.2f]\n", R[k].name, c1, lo1, hi1, cP, loP, hiP); fflush(stdout);
    }
    return 0;
}
/* ---------------------------------------------------------------------------------------------------------------------------
 * MODE `lis`: the unknotting number as a classifier. For a strand the unknotting number is n minus its longest increasing
 * subsequence, so the LIS FRACTION of a 2,048-point sample in position order says how much of an array is strays. Plain, and
 * CYCLIC (the sample rotated at its largest drop, so a ring buffer reads as one strand). Patience sorting, O(m log m).
 * PREDICTIONS, written first: sorted 1.00; ring buffer 0.63 plain / 1.00 cyclic; blocks + 1% strays 0.20-0.30 plain (the
 * longest chain of blocks that ascend, about 2 sqrt(64) of 64, LESS 1% strays) - so it does NOT see strays inside blocks, only
 * the block permutation; near_sorted 1% swaps 0.97-0.99; prefix_95 0.95-0.96; short reach 0.99-1.00; uniform 0.04-0.05
 * (2 sqrt(m) / m); sine 32 cycles 0.50-0.60 (per cycle the ascending piece above the last crest); 4 streams 0.25-0.30 (one
 * stream); stairs x20 0.25-0.35. If those land, the statistic separates "one strand with strays" from "many strands", which
 * is the question the stray route's gate asks; it cannot count strays inside blocks, which the detector's own scan does.
 * ------------------------------------------------------------------------------------------------------------------------ */
typedef struct { size_t pos; double v; } ps_t;
static int cmp_ps(const void *x, const void *y) { size_t a = ((const ps_t *)x)->pos, b = ((const ps_t *)y)->pos; return (a > b) - (a < b); }
static size_t lis_len(const double *v, size_t m, double *tails) {           /* patience sorting: tails[k] = smallest tail of a chain of length k+1 */
    size_t L = 0;
    for (size_t i = 0; i < m; i++) { size_t lo = 0, hi = L; while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (tails[mid] < v[i]) lo = mid + 1; else hi = mid; } tails[lo] = v[i]; if (lo == L) L++; }
    return L;
}
static void lis_of(const double *a, size_t n, double *plain, double *cyclic) {
    enum { M = 2048 }; ps_t s[M]; double v[M], t[M];
    for (size_t i = 0; i < M; i++) { double f = (double)i * 0.6180339887498949; f -= (double)(uint64_t)f; s[i].pos = (size_t)(f * (double)n); if (s[i].pos >= n) s[i].pos = n - 1; s[i].v = a[s[i].pos]; }
    qsort(s, M, sizeof *s, cmp_ps);
    for (size_t i = 0; i < M; i++) v[i] = s[i].v;
    *plain = (double)lis_len(v, M, t) / M;
    size_t k = 0; double drop = -1e300; for (size_t i = 0; i + 1 < M; i++) { double d = v[i] - v[i + 1]; if (d > drop) { drop = d; k = i; } }
    double r[M]; for (size_t i = 0; i < M; i++) r[i] = v[(k + 1 + i) % M];
    double c = (double)lis_len(r, M, t) / M; *cyclic = c > *plain ? c : *plain;
}
static int run_lis(size_t n, double *s, double *w, double *tmp) {
    (void)w;
    static const abrow_t R[] = {
        { "sorted", NULL, NULL, G_MAIN, 0, S_RING }, { "ring buffer (sorted, rotated)", NULL, NULL, G_MAIN, 1, S_RING },
        { "blocks + 1% strays", NULL, NULL, G_STRAYS, 0, 1000 }, { "block_perm 64", NULL, NULL, G_MAIN, 0, S_BLOCKS },
        { "near_sorted 1%", NULL, NULL, G_MAIN, 0, S_NEAR }, { "prefix_95 + tail", NULL, NULL, G_MAIN, 0, S_PREFIX },
        { "short reach +-60", NULL, NULL, G_MAIN, 0, S_REACH }, { "uniform", NULL, NULL, G_MAIN, 0, S_UNIFORM },
        { "sine 32 cycles, noise 50", NULL, NULL, G_SINE, 0, SINE(32, 60, 50) }, { "4 ascending streams", NULL, NULL, G_MAIN, 0, S_STREAMS },
        { "stairs x20 (shuffled)", NULL, NULL, G_MAIN, 0, S_STAIRSHUF },
    };
    printf("LIS FRACTION of a 2,048-point sample in position order (the unknotting number, inverted), n = %zu.\n\n  %-34s %8s %8s\n", n, "shape", "plain", "cyclic");
    for (size_t k = 0; k < sizeof R / sizeof *R; k++) { double pl, cy; gen_ab(s, n, &R[k], tmp); lis_of(s, n, &pl, &cy); printf("  %-34s %8.3f %8.3f\n", R[k].name, pl, cy); }
    return 0;
}
/* MODE `once2`: every ab row's shape sorted once at one thread and at P, for the trace build (make unknottrace) */
static int run_once2(size_t n, int P, double *s, double *w, double *tmp) {
    static const abrow_t R[] = {
        { "sine 32 cycles, swing 0.6, noise 50", NULL, NULL, G_SINE, 0, SINE(32, 60, 50) }, { "sine 32 cycles, swing 2, noise 50", NULL, NULL, G_SINE, 0, SINE(32, 200, 50) },
        { "sine 8 cycles, noise 50", NULL, NULL, G_SINE, 0, SINE(8, 60, 50) }, { "sine 2000 cycles, noise 50", NULL, NULL, G_SINE, 0, SINE(2000, 60, 50) },
        { "sine 32 cycles, no noise", NULL, NULL, G_SINE, 0, SINE(32, 60, 0) },
        { "short reach, rotated", NULL, NULL, G_MAIN, 1, S_REACH }, { "near_sorted, rotated", NULL, NULL, G_MAIN, 1, S_NEAR },
        { "blocks + 0.1% strays", NULL, NULL, G_STRAYS, 0, 100 }, { "blocks + 1% strays", NULL, NULL, G_STRAYS, 0, 1000 }, { "blocks + 3% strays", NULL, NULL, G_STRAYS, 0, 3000 },
        { "triple", NULL, NULL, G_STAIRS, 0, 150 }, { "quad", NULL, NULL, G_STAIRS, 0, 250 }, { "k8", NULL, NULL, G_STAIRS, 0, 700 },
        { "uniform", NULL, NULL, G_MAIN, 0, S_UNIFORM }, { "4 streams", NULL, NULL, G_MAIN, 0, S_STREAMS },
    };
    for (size_t k = 0; k < sizeof R / sizeof *R; k++) { gen_ab(s, n, &R[k], tmp);
        for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1;
            fprintf(stderr, "#### %s, %d thread(s)\n", R[k].name, T); memcpy(w, s, n * 8); logos_sort_f64(w, n, T);
            if (!sorted_ok(w, n)) fprintf(stderr, "#### NOT SORTED\n"); } }
    return 0;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 40; size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 8388608;
    const char *mode = argc > 3 ? argv[3] : "ring"; const char *filt = argc > 4 ? argv[4] : NULL;
    int P = bench_threads(logos_num_cpus());
    (void)bench_paired; (void)bench_threads_source; if (reps < 4) reps = 4;
    double *s = malloc(n * 8), *m = malloc(n * 8), *w = malloc(n * 8); g_tmp = malloc(n * 8);
    if (!s || !m || !w || !g_tmp) return 2;
    g_rot = (size_t)(0.37 * (double)n);
    if (!strcmp(mode, "ab")) return run_ab(reps, n, P, filt, s, w, g_tmp);
    if (!strcmp(mode, "nocopy")) return run_nocopy(reps, n, P, s, w, g_tmp);
    if (!strcmp(mode, "lis")) return run_lis(n, s, w, g_tmp);
    if (!strcmp(mode, "once2")) return run_once2(n, P, s, w, g_tmp);
    if (!strcmp(mode, "cliff")) return run_cliff(reps, n, P, filt && atol(filt) >= 16 ? filt : "256", filt && atol(filt) >= 16 ? NULL : filt, s, w);
    if (!strcmp(mode, "once")) {                  /* one sort each, as generated and rotated, 1 thread and P: for a trace build */
        for (int k = 0; k < S_N; k++) for (int rot = 0; rot < 2; rot++) {
            if (filt && !strstr(SN[k], filt)) continue;
            gen(s, n, k); if (rot) rotate_left(s, n, g_rot, w);
            for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1;
                fprintf(stderr, "#### %s%s, %d thread(s)\n", SN[k], rot ? ", ROTATED" : "", T); memcpy(w, s, n * 8); logos_sort_f64(w, n, T);
                if (!sorted_ok(w, n)) fprintf(stderr, "#### NOT SORTED\n"); }
        }
        return 0;
    }
    if (!strcmp(mode, "ring")) {
        printf("R0 ON THE SPHERE: a shape rotated left by %zu (0.37 n; a ring buffer read from physical slot 0) against itself, n = %zu, %d pairs, P = %d.\n", g_rot, n, reps, P);
        printf("ceiling = t(sort rotated) / t(sort as generated); simple = t(sort rotated) / t(rotate back, TOLD the offset, then sort). Above 1 = the seam costs that much.\n\n");
        printf("  %-28s | %-22s %-22s | %-22s %-22s\n", "shape", "1T ceiling", "1T simple", "PT ceiling", "PT simple");
        for (int k = 0; k < S_N; k++) {
            if (filt && !strstr(SN[k], filt)) continue;
            gen(s, n, k); memcpy(m, s, n * 8); rotate_left(m, n, g_rot, w);
            double c[4], lo[4], hi[4]; int ok[4];
            c[0] = pair(m, K_ENGINE, s, K_ENGINE, w, n, 1, reps, &lo[0], &hi[0], &ok[0]);
            c[1] = pair(m, K_ENGINE, m, K_ENGINE_TOLD_ROT, w, n, 1, reps, &lo[1], &hi[1], &ok[1]);
            c[2] = pair(m, K_ENGINE, s, K_ENGINE, w, n, P, reps, &lo[2], &hi[2], &ok[2]);
            c[3] = pair(m, K_ENGINE, m, K_ENGINE_TOLD_ROT, w, n, P, reps, &lo[3], &hi[3], &ok[3]);
            printf("  %-28s | %6.3f [%.2f %.2f]%s  %6.3f [%.2f %.2f]%s  | %6.3f [%.2f %.2f]%s  %6.3f [%.2f %.2f]%s\n", SN[k],
                   c[0], lo[0], hi[0], ok[0] ? "  " : " X", c[1], lo[1], hi[1], ok[1] ? "  " : " X", c[2], lo[2], hi[2], ok[2] ? "  " : " X", c[3], lo[3], hi[3], ok[3] ? "  " : " X");
            fflush(stdout);
        }
        printf("\nRESULT: ratios only; every output checked ascending. Read the uniform row first: its ceiling is this box's floor.\n");
        return 0;
    }
    if (!strcmp(mode, "count") || !strcmp(mode, "moves")) {
        int timed = !strcmp(mode, "moves");
        printf("THE UNKNOTTER: a circular doubly linked list cut into strands, R1 in the scan, R2 between ring neighbours, R3 when stuck. n = %zu%s\n", n,
               timed ? "" : " (counts only)");
        if (timed) printf("each timing cell: median of t(logos_sort_f64, 1 thread) / t(unknotter) [p10 p90]. ABOVE 1 = THE UNKNOTTER IS FASTER. Rotated rows: the shape as a ring buffer.\n");
        printf("\n  %-36s | %8s %6s %6s %6s %8s %7s %6s %8s %7s %6s %6s | %9s | %-22s %-22s\n", "shape", "strands", "kinks", "swings", "loop", "R2", "walk/n", "R3", "hard/blk",
               "tangles", "forced", "final", "no swing", timed ? "vs engine (sphere)" : "", timed ? "plane / sphere" : "");
        for (int k = 0; k < S_N; k++) for (int rot = 0; rot < 2; rot++) {
            if (rot && !ROTATES[k]) continue;
            char nm[80]; snprintf(nm, sizeof nm, "%s%s", SN[k], rot ? ", rotated" : "");
            if (filt && !strstr(nm, filt)) continue;
            gen(s, n, k); if (rot) rotate_left(s, n, g_rot, w);
            memcpy(m, s, n * 8); logos_sort_f64(m, n, 1);                          /* the reference */
            memcpy(w, s, n * 8); moves_t mv; int good = unknot_sort(w, n, 0, 0, &mv) && same_bits(w, m, n);
            moves_t mp; memcpy(w, s, n * 8); int goodp = unknot_sort(w, n, 1, 0, &mp) && same_bits(w, m, n);
            moves_t mq; memcpy(w, s, n * 8); int goodq = unknot_sort(w, n, 0, 1, &mq) && same_bits(w, m, n);
            char hb[16]; snprintf(hb, sizeof hb, "%zu/%zu", mv.hard, mv.blocked);
            printf("  %-36s | %8zu %6zu %6zu %6s %8zu %7.2f %6zu %8s %7zu %6zu %6zu | %9.2f |", nm, mv.strands, mv.kinks, mv.swings, mv.joined ? "joined" : "-", mv.r2,
                   (double)mv.r2_nodes / (double)n, mv.r3, hb, mv.tangles, mv.forced, mv.final_strands, (double)mq.r2_nodes / (double)n);
            if (!good || !goodp || !goodq) printf(" WRONG OUTPUT (sphere %d, plane %d, no swing %d)", good, goodp, goodq);
            if (timed && good && goodp && goodq) {
                int r = (mv.strands > n / 64) ? (reps / 4 > 4 ? reps / 4 : 4) : reps;     /* noise is slow on a list: fewer pairs */
                double lo1, hi1, lo2, hi2; int ok1, ok2;
                double c1 = pair(s, K_ENGINE, s, K_UNKNOT, w, n, 1, r, &lo1, &hi1, &ok1);
                double c2 = pair(s, K_UNKNOT_PLANE, s, K_UNKNOT, w, n, 1, r, &lo2, &hi2, &ok2);
                printf(" %6.3f [%.2f %.2f]%s %6.3f [%.2f %.2f]%s", c1, lo1, hi1, ok1 ? "  " : " X", c2, lo2, hi2, ok2 ? "  " : " X");
            }
            printf("\n"); fflush(stdout);
        }
        printf("\nstrands: after the scan (and the loop-back). swings: strands cut at a gap (the Culprit's arc swung out). loop: whether the last strand and the\n"
               "first were one across the wrap. walk/n: nodes the R2 merges walked, per element (the overlaps). R3: slides. hard/blk: stalls of the local moves with a crossing left,\n"
               "when NO two ring neighbours crossed (hard, the paper's case) / when the size rule blocked every pair that did (blk). tangles: overlapping groups the global reading collapsed. forced: merges past the size rule. final: strands left, pairwise disjoint.\n"
               "no swing: walk/n with the gap cuts off, the Culprit's arc left inside its strand.\n");
        return 0;
    }
    fprintf(stderr, "usage: ./unknotprobe [pairs] [n] [ring | moves | count] [row filter]\n");
    return 2;
}
