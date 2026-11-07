/* strandgate: the near-sorted route's STRAND GATE (2026-11-06, FINDINGS 3bw), and the table the gate was set from.
 *
 * Twenty-seven shapes at P threads: in-order staircases (overlap 0.05 / 0.2 / 0.5 / 1.0; 16, 64 and 256 runs), blocks with
 * neighbour pairs swapped, batches arriving late, the near-sorted route's own inputs (sorted + 0.5-5% random swaps, 2% swaps within
 * 2048 places), tsbench's log shapes (latency jitter, retransmits, kinks, a late batch, concatenated logs, clock skew, a ring buffer,
 * telemetry with dropouts), nsgatebench's groups of runs, and shuffled / uniform controls. Per shape, the STATISTICS a gate could read:
 *   inv_all   the shared sample's lag-144 inversion rate (as sodo_sample computes it: the route's own gate reads it, 2.5% / 5%)
 *   win_rej   the four-window pre-gate's reject share (the route's greedy scan on 4 x 4096 elements, 25% declines)
 *   D         descents in a 2048-point Weyl sample sorted by POSITION (the array's scale: one per strand boundary)
 *   S, K, longest   strays that scan finds on the position-ordered sample, their stretches, the longest stretch (exploration only)
 * and then a PAIRED timing, adjacent, order alternating, every output verified on the first pair:
 *   t(switch set) / t(shipped) at P threads, median with p10 / p90, and both sides in ns per element.  ABOVE 1 = SHIPPED IS FASTER.
 * SG_SWITCH names the switch (default LOGOS_NO_NSSTRAND: the strand gate's A/B; LOGOS_NO_NEARSORTED: what the whole route is worth).
 * The greedy scan here is a COPY of ns_split_scan for the statistics columns only; the engine's gate calls the real one.
 *
 * What the table showed, before the gate was written (the exploration, a loaded box): every shape where the route LOST 2-5x reads
 * win_rej 0.000 with D 8-155 (disorder at the array's scale only: strands), every shape where it WINS 1.3-3.5x reads win_rej
 * 0.011-0.067 (disorder at the element's scale too: strays). The gate reads exactly that: adjacent descents inside the windows at
 * most NS_STRAND_WIN_DESC, descents in the position-ordered sample at least NS_STRAND_MIN_DESC -> decline. Usage:
 *   ./strandgate [pairs] [n] [P] [shape filter] */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
static double nowms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static int cmp_d(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static size_t weyl(size_t i, size_t n) { double f = (double)i * 0.6180339887498949; f -= (double)(uint64_t)f; size_t q = (size_t)(f * (double)n); return q < n ? q : n - 1; }

/* ---- shapes ---- */
static void blocks(double *a, size_t n, double o, int shuffled, size_t NB) { size_t blk = (n + NB - 1) / NB; size_t perm[256];
    for (size_t k = 0; k < NB; k++) perm[k] = k;
    if (shuffled) for (size_t k = NB - 1; k > 0; k--) { size_t r = nr() % (k + 1), t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + o) + fr() * 0.5; }
static void rev(double *a, size_t lo, size_t hi) { while (lo + 1 < hi) { double t = a[lo]; a[lo] = a[--hi]; a[lo++] = t; } }
static void rot_left(double *a, size_t lo, size_t mid, size_t hi) { rev(a, lo, mid); rev(a, mid, hi); rev(a, lo, hi); }
static void swap_ranges(double *a, size_t x, size_t y, size_t len) { for (size_t i = 0; i < len; i++) { double t = a[x + i]; a[x + i] = a[y + i]; a[y + i] = t; } }
static void near(double *a, size_t n, double f) { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5;
    size_t k = (size_t)(f * (double)n); for (size_t q = 0; q < k; q++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } }
typedef struct { double arrive, value; } ev_t;
static int cmp_ev(const void *x, const void *y) { double a = ((const ev_t *)x)->arrive, b = ((const ev_t *)y)->arrive; return (a > b) - (a < b); }
static void jitter(double *a, size_t n, double lat_scale, double retx) { ev_t *e = malloc(n * sizeof *e); double c = 1.7e15;
    for (size_t i = 0; i < n; i++) { c += -log(1.0 - fr()) * 10.0; double lat = -log(1.0 - fr()) * lat_scale; if (retx > 0 && fr() < retx) lat += 200000.0; e[i].arrive = c + lat; e[i].value = c; }
    qsort(e, n, sizeof *e, cmp_ev); for (size_t i = 0; i < n; i++) a[i] = e[i].value; free(e); }
static void gen(int s, double *a, size_t n) {
    RS = 7 + (uint64_t)s; size_t blk = (n + 63) / 64;
    switch (s) {
    case 0: blocks(a, n, 0.05, 0, 64); break;
    case 1: blocks(a, n, 0.2, 0, 64); break;
    case 2: blocks(a, n, 0.5, 0, 64); break;
    case 3: blocks(a, n, 1.0, 0, 64); break;
    case 4: blocks(a, n, 0.0, 0, 64); for (size_t b = 0; b * blk < n; b += 2) rev(a, b * blk, (b + 1) * blk < n ? (b + 1) * blk : n); break;
    case 5: blocks(a, n, 0.0, 0, 64); for (size_t b = 4; b + 1 < 64 && (b + 2) * blk <= n; b += 8) swap_ranges(a, b * blk, (b + 1) * blk, blk); break;
    case 6: blocks(a, n, 0.2, 0, 64); for (size_t b = 8; b + 4 <= 64 && (b + 4) * blk <= n; b += 16) rot_left(a, b * blk, (b + 1) * blk, (b + 4) * blk); break;
    case 7: blocks(a, n, 0.2, 1, 64); break;                                   /* shuffled stairs (guard) */
    case 8: { size_t pos = 0; double key = 0; while (pos < n) { size_t len = (size_t)exp(log(2000.0) + fr() * log(100.0)); if (len > n - pos) len = n - pos;
        for (size_t i = 0; i < len; i++) a[pos + i] = key + fr(); key += 1.0; pos += len; }
        for (size_t p = 0; p < n; p += 33421) qsort(a + p, p + 33421 <= n ? 33421 : n - p, 8, cmp_d); } break;   /* groups_of_runs */
    case 9: near(a, n, 0.005); break;
    case 10: near(a, n, 0.01); break;
    case 11: near(a, n, 0.02); break;
    case 12: near(a, n, 0.03); break;
    case 13: near(a, n, 0.05); break;
    case 14: jitter(a, n, 500.0, 0); break;                                   /* latency 0.5 ms */
    case 15: jitter(a, n, 500.0, 0.001); break;                               /* + 0.1% retransmits */
    case 16: { double c = 1.7e15; for (size_t i = 0; i < n; i++) { c += -log(1.0 - fr()) * 10.0; a[i] = c; } for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); double t = a[x]; a[x] = a[x + 1]; a[x + 1] = t; } } break;   /* kinks 1% */
    case 17: { double c = 1.7e15; size_t k = n - n / 20; double *t = malloc(n * 8); for (size_t i = 0; i < n; i++) { c += -log(1.0 - fr()) * 10.0; t[i] = c; }
        memcpy(a, t, k * 8); double t0 = t[0] + 0.9 * (t[k - 1] - t[0]), t1 = t[k - 1]; for (size_t i = k; i < n; i++) a[i] = t0 + fr() * (t1 - t0); free(t); } break;   /* late batch appended 5% */
    case 18: { double c = 1.7e15; double *t = malloc(n * 8); for (size_t i = 0; i < n; i++) { c += -log(1.0 - fr()) * 10.0; t[i] = c; }
        size_t cnt[8] = {0}, off[9] = {0}; unsigned char *w = malloc(n); for (size_t i = 0; i < n; i++) { w[i] = nr() % 8; cnt[w[i]]++; }
        for (int k = 0; k < 8; k++) off[k + 1] = off[k] + cnt[k]; for (size_t i = 0; i < n; i++) a[off[w[i]]++] = t[i]; free(t); free(w); } break;   /* 8 logs concatenated */
    case 19: { ev_t *e = malloc(n * sizeof *e); double c = 1.7e15, skew[8]; for (int k = 0; k < 8; k++) skew[k] = (fr() * 2.0 - 1.0) * 20000.0;
        for (size_t i = 0; i < n; i++) { c += -log(1.0 - fr()) * 10.0; int k = (int)(nr() % 8); e[i].arrive = c + (-log(1.0 - fr()) * 200.0); e[i].value = c + skew[k]; }
        qsort(e, n, sizeof *e, cmp_ev); for (size_t i = 0; i < n; i++) a[i] = e[i].value; free(e); } break;   /* 8 servers, clock skew */
    case 20: blocks(a, n, 0.5, 0, 64); break;                                  /* placeholder: same as 2 */
    case 21: { for (size_t i = 0; i < n; i++) { double x = (double)i / (double)n; a[i] = 50000.0 * pow(x, 1.5) + (fr() * 2.0 - 1.0) * 3.0; if (fr() < 0.0005) a[i] = 0.0; } } break;   /* rocket + dropouts */
    case 22: blocks(a, n, 0.2, 0, 16); break;                                   /* 16 wide stairs */
    case 23: blocks(a, n, 0.2, 0, 256); break;                                  /* 256 narrow stairs */
    case 24: blocks(a, n, 0.0, 0, 64); for (size_t b = 0; b + 1 < 64 && (b + 2) * blk <= n; b += 2) swap_ranges(a, b * blk, (b + 1) * blk, blk); break;   /* every pair swapped */
    case 25: { for (size_t i = 0; i < n; i++) a[i] = (double)i + fr() * 0.5; size_t k = n / 50; for (size_t i = 0; i < k; i++) { size_t x = nr() % n, y = x + (nr() % 2048) + 1; if (y < n) { double t = a[x]; a[x] = a[y]; a[y] = t; } } } break;   /* 2% short-range swaps */
    case 26: { jitter(a, n, 500.0, 0); size_t r = n * 37 / 100; double *c = malloc(n * 8); memcpy(c, a, n * 8); for (size_t i = 0; i < n; i++) a[i] = c[(i + r) % n]; free(c); } break;   /* ring buffer, jitter */
    default: for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    }
}
static const char *NAME[] = { "stairs o=0.05", "stairs o=0.2", "stairs o=0.5", "stairs o=1.0", "blocks alt twisted", "8 pairs swapped", "4 batches late",
    "stairs SHUFFLED 0.2", "groups_of_runs", "near 0.5%", "near 1%", "near 2%", "near 3%", "near 5%", "jitter 0.5ms", "jitter+retx", "kinks 1%",
    "late batch 5%", "8 logs concat", "8 servers skew", "(dup stairs 0.5)", "rocket+dropouts", "16 stairs o=0.2", "256 stairs o=0.2", "every pair swapped",
    "2% short swaps", "ring, jitter", "uniform" };
enum { NSH = 28 };

/* ---- the statistics ---- */
#define NS_POISON 16
static void greedy(const double *a, size_t lo, size_t hi, double *dst, size_t *nin, size_t *nstray, unsigned char *isstray) {
    size_t f = lo, b = hi; double kmax = -INFINITY; size_t streak = 0, streak_start = lo;
    for (size_t i = lo; i < hi; i++) {
        double v = a[i], n1 = i + 1 < hi ? a[i + 1] : INFINITY, n2 = i + 2 < hi ? a[i + 2] : INFINITY;
        int ge = v >= kmax, nb = (v <= n1) | (v <= n2);
        if (ge & nb) { dst[f++] = v; kmax = v; streak = 0; if (isstray) isstray[i] = 0; continue; }
        if (!ge && f > lo) { if (streak == 0) streak_start = i; if (++streak >= NS_POISON) { double poison = dst[--f]; kmax = f > lo ? dst[f - 1] : -INFINITY; b += streak - 1; dst[--b] = poison; if (isstray) isstray[streak_start - 1 < lo ? lo : i] = 1; i = streak_start - 1; streak = 0; continue; } }
        else streak = 0;
        dst[--b] = v; if (isstray) isstray[i] = 1;
    }
    *nin = f - lo; *nstray = hi - b;
}
typedef struct { double inv_all, win_rej; size_t D, S, K, longest, M; } stat_t;
static int cmp_sz(const void *x, const void *y) { size_t a = *(const size_t *)x, b = *(const size_t *)y; return (a > b) - (a < b); }
static stat_t stats(const double *a, size_t n) {
    stat_t s; memset(&s, 0, sizeof s); enum { M = 2048, L = 144 }; s.M = M;
    double raw[M]; for (size_t i = 0; i < M; i++) raw[i] = a[weyl(i, n)];
    size_t inva = 0, compsa = 0;
    for (size_t i = 0; i + L < M; i++) { size_t p0 = weyl(i, n), p1 = weyl(i + L, n); if (p1 == p0) continue; compsa++; if (p1 > p0 ? raw[i] > raw[i + L] : raw[i + L] > raw[i]) inva++; }
    s.inv_all = compsa ? (double)inva / compsa : 1.0;
    size_t rej = 0; double win[4096];
    for (int w = 0; w < 4; w++) { size_t lo = (n / 5) * (size_t)(w + 1) - 2048, nin, ns; greedy(a + lo, 0, 4096, win, &nin, &ns, NULL); rej += ns; }
    s.win_rej = rej / 16384.0;
    /* position order */
    size_t pos[M], idx[M]; for (size_t i = 0; i < M; i++) { pos[i] = weyl(i, n); }
    for (size_t i = 0; i < M; i++) idx[i] = pos[i];
    qsort(idx, M, sizeof(size_t), cmp_sz);
    double v[M], d[M]; unsigned char st[M];
    for (size_t i = 0; i < M; i++) v[i] = a[idx[i]];
    for (size_t i = 1; i < M; i++) s.D += v[i] < v[i - 1];
    size_t nin; memset(st, 0, M); greedy(v, 0, M, d, &nin, &s.S, st);
    size_t cur = 0; for (size_t i = 0; i < M; i++) { if (st[i]) { if (cur == 0) s.K++; cur++; if (cur > s.longest) s.longest = cur; } else cur = 0; }
    return s;
}
static uint64_t bitsum(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, a + i, 8); s += u * 0x9E3779B97F4A7C15ULL + (u >> 29); } return s; }
static int sorted_ok(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }
static int cmp_dd(const void *x, const void *y) { return cmp_d(x, y); }
int main(int argc, char **argv) {
    int pairs = argc > 1 ? atoi(argv[1]) : 8; size_t n = argc > 2 ? (size_t)atoll(argv[2]) : 8388608; int P = argc > 3 ? atoi(argv[3]) : 14;
    const char *filter = argc > 4 ? argv[4] : NULL; setvbuf(stdout, NULL, _IOLBF, 0);
    const char *sw = getenv("SG_SWITCH") ? getenv("SG_SWITCH") : "LOGOS_NO_NSSTRAND";   /* side B sets this switch; shipped is side A */
    if (pairs > 256) pairs = 256;
    double *in = malloc(n * 8), *w = malloc(n * 8); double r[256];
    if (!in || !w) { fprintf(stderr, "no memory\n"); return 1; }
    printf("strandgate: n = %zu, P = %d, %d pairs, switch %s=1. Columns: inv_all | win_rej | D desc | S strays | K stretches | longest | S/D  ||  t(%s) / t(shipped) [p10 p90]  ns/elem shipped / switch.  ABOVE 1 = SHIPPED IS FASTER.\n", n, P, pairs, sw, sw);
    for (int s = 0; s < NSH; s++) {
        if (s == 20) continue;
        if (filter && !strstr(NAME[s], filter)) continue;
        gen(s, in, n); stat_t st = stats(in, n);
        uint64_t bs = bitsum(in, n); int ok = 1; double te = 0, tn = 0; int np = 0;
        for (int p = 0; p < pairs; p++) {
            double t[2];
            for (int side = 0; side < 2; side++) {
                int isA = (side == 0) == ((p & 1) == 0);
                memcpy(w, in, n * 8); if (!isA) setenv(sw, "1", 1);
                double t0 = nowms(); logos_sort_f64(w, n, P); t[isA ? 0 : 1] = nowms() - t0;
                if (!isA) unsetenv(sw);
                if (p == 0 && (!sorted_ok(w, n) || bitsum(w, n) != bs)) ok = 0;
            }
            r[np++] = t[1] / t[0]; te += t[0]; tn += t[1];                 /* t(switch) / t(shipped) */
        }
        qsort(r, np, sizeof(double), cmp_dd);
        printf("%-20s %6.3f  %6.3f  %5zu  %5zu  %5zu  %5zu  %6.2f  ||  %6.3f [%5.3f %5.3f]  %6.2f / %6.2f %s\n", NAME[s], st.inv_all, st.win_rej, st.D, st.S, st.K, st.longest,
               st.D ? (double)st.S / st.D : 0.0, r[np / 2], r[np / 10], r[np - 1 - np / 10], te / np * 1e6 / n, tn / np * 1e6 / n, ok ? "" : "WRONG OUTPUT");
        fflush(stdout);
    }
    free(in); free(w); return 0;
}
