/* fieldprobe: ceilings and tests for the "other fields" round (experiments/OTHER_FIELDS.md, 2026-10-21). One binary, one mode per question.
 *
 *   ./fieldprobe small    [pairs] [P]          Kleiber: what the dispatcher costs over its own bare AVX2 kernel, 4096..262144, batches of
 *                                              distinct inputs. Column: t(engine) / t(bare kernel). PREDICTION: 1.15-1.35 at 4096, 1.05-1.10 at 65536.
 *   ./fieldprobe hub      [pairs] [n list]     Hub and spoke: a TOLD one-thread scatter into equi-width buckets + the bare kernel per bucket, against
 *                                              the engine. Column: t(engine) / t(hub), above 1 = the hub would win. PREDICTION: 1.10-1.35 at 8M.
 *   ./fieldprobe weave    [pairs] [n] [P]      TDM channels and randomly interleaved sources against a TOLD demux (a stable scatter by channel, then
 *                                              the engine on the k strands). PREDICTION: 2-4x (tdm2 and the zipper ~4x, 16 channels 1.5-2x).
 *   ./fieldprobe moire    [pairs] [n] [P]      Periodic inputs across ~46 periods, each against its own values shuffled. PREDICTION: at most two
 *                                              spikes above 1.15, at multiples of a fixed probe length.
 *   ./fieldprobe founder  [pairs] [n] [P]      An unrepresentative founder at the front against the same elements scattered. PREDICTION: all
 *                                              0.95-1.05 but one (a sorted founder of 4096 on integers).
 *   ./fieldprobe landauer [reps]  [n] [P]      ns per element against bits per element (the generator's entropy). PREDICTION: the interleaved
 *                                              shapes are the outliers, every routed shape within 3x of uniform's ns per bit.
 * A trailing argument filters shapes by substring. Every output is checked against qsort before it is timed. Links an engine built with
 * -DLOGOS_AV_HOOK, which exports the bare kernel (logos_av_qsort_hook) and changes nothing else. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "logos_psort.h"

int logos_av_qsort_hook(double *a, size_t n);

static uint64_t rs = 0x2545F4914F6CDD1DULL;
static uint64_t nr(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double u01(void) { return (double)(nr() >> 11) / 9007199254740992.0; }
static double gauss(void) { double u = u01() + 1e-300, v = u01(); return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void shuffle(double *a, size_t n) { for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); double t = a[i]; a[i] = a[j]; a[j] = t; } }
static const char *g_filt = "";
static int skip(const char *name) { return *g_filt && !strstr(name, g_filt); }

/* m sorted uniform values in [lo, lo + span): normalised cumulative exponential spacings, O(m), no sort */
static void sorted_uniform(double *v, size_t m, double lo, double span) {
    double s = 0; for (size_t i = 0; i < m; i++) { s += -log(u01() + 1e-300); v[i] = s; }
    double tot = s - log(u01() + 1e-300);
    for (size_t i = 0; i < m; i++) v[i] = lo + v[i] / tot * span;
}

/* ---- runners: each sorts ONE instance of n elements in place ---- */
typedef void (*run_fn)(double *w, size_t n, void *ctx);
static void run_logos(double *w, size_t n, void *c) { logos_sort_f64(w, n, *(int *)c); }
static void run_bare(double *w, size_t n, void *c) { (void)c; if (!logos_av_qsort_hook(w, n)) qsort(w, n, sizeof(double), cmpd); }

typedef struct { size_t T; int wc; double *buf; size_t *cnt, *pos; double *stage; uint8_t *fill; } hub_t;
static void run_hub(double *a, size_t n, void *vc) {
    hub_t *h = vc;
    size_t B = n / h->T;
    if (B < 2) { run_bare(a, n, NULL); return; }
    double mn = a[0], mx = a[0];
    for (size_t i = 1; i < n; i++) { double x = a[i]; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
    double sc = (double)B / (mx - mn);
    memset(h->cnt, 0, B * sizeof(size_t));
    for (size_t i = 0; i < n; i++) { size_t b = (size_t)((a[i] - mn) * sc); b = b < B ? b : B - 1; h->cnt[b]++; }
    size_t s = 0; for (size_t b = 0; b < B; b++) { h->pos[b] = s; s += h->cnt[b]; }
    if (!h->wc) {
        for (size_t i = 0; i < n; i++) { double x = a[i]; size_t b = (size_t)((x - mn) * sc); b = b < B ? b : B - 1; h->buf[h->pos[b]++] = x; }
    } else {                                                          /* write-combining: a cache line staged per bucket */
        memset(h->fill, 0, B);
        for (size_t i = 0; i < n; i++) {
            double x = a[i]; size_t b = (size_t)((x - mn) * sc); b = b < B ? b : B - 1;
            double *st = h->stage + b * 8; st[h->fill[b]] = x;
            if (++h->fill[b] == 8) { memcpy(h->buf + h->pos[b], st, 64); h->pos[b] += 8; h->fill[b] = 0; }
        }
        for (size_t b = 0; b < B; b++) { memcpy(h->buf + h->pos[b], h->stage + b * 8, h->fill[b] * sizeof(double)); h->pos[b] += h->fill[b]; }
    }
    s = 0; for (size_t b = 0; b < B; b++) { size_t len = h->cnt[b]; if (len > 1) run_bare(h->buf + s, len, NULL); s += len; }
    memcpy(a, h->buf, n * sizeof(double));
}

typedef struct { const uint8_t *lab; int k; double *buf; int P; } demux_t;
static void run_demux(double *a, size_t n, void *vc) {             /* told: a stable scatter by channel, then the engine on k strands */
    demux_t *d = vc; size_t cnt[256] = { 0 }, pos[256];
    for (size_t i = 0; i < n; i++) cnt[d->lab[i]]++;
    size_t s = 0; for (int j = 0; j < d->k; j++) { pos[j] = s; s += cnt[j]; }
    for (size_t i = 0; i < n; i++) d->buf[pos[d->lab[i]]++] = a[i];
    logos_sort_f64(d->buf, n, d->P);
    memcpy(a, d->buf, n * sizeof(double));
}

/* ---- checking and pairing ---- */
static double *g_ref, *g_chk;
static int check(run_fn f, void *c, const double *x, size_t n) {   /* the runner's output must equal qsort's, byte for byte */
    memcpy(g_ref, x, n * sizeof(double)); qsort(g_ref, n, sizeof(double), cmpd);
    memcpy(g_chk, x, n * sizeof(double)); f(g_chk, n, c);
    return memcmp(g_ref, g_chk, n * sizeof(double)) == 0;
}
/* A on copies of xa, B on copies of xb, adjacent, order alternating; inst instances of n each. Median tA / tB, p10, p90; mean ms. */
static double paired(run_fn fa, void *ca, const double *xa, run_fn fb, void *cb, const double *xb, double *w, size_t n, size_t inst,
                     int pairs, double *p10, double *p90, double *ma, double *mb) {
    double *r = malloc((size_t)pairs * sizeof(double)), sa = 0, sb = 0;
    for (int i = 0; i < pairs; i++) {
        double t[2];
        for (int k = 0; k < 2; k++) {
            int do_a = (k == 0) == ((i & 1) == 0);
            memcpy(w, do_a ? xa : xb, n * inst * sizeof(double));
            double t0 = now_ms();
            for (size_t b = 0; b < inst; b++) (do_a ? fa : fb)(w + b * n, n, do_a ? ca : cb);
            t[do_a ? 0 : 1] = now_ms() - t0;
        }
        r[i] = t[0] / t[1]; sa += t[0]; sb += t[1];
    }
    qsort(r, (size_t)pairs, sizeof(double), cmpd);
    double med = (pairs & 1) ? r[pairs / 2] : 0.5 * (r[pairs / 2 - 1] + r[pairs / 2]);
    *p10 = r[(int)(0.1 * pairs)]; *p90 = r[(int)(0.9 * pairs)]; *ma = sa / pairs; *mb = sb / pairs;
    free(r); return med;
}

/* ================= small: the dispatcher's cost over its bare kernel ================= */
static void gen_unstr(int sh, double *a, size_t n) {
    for (size_t i = 0; i < n; i++)
        a[i] = sh == 0 ? u01() * 1e6 : sh == 1 ? gauss() * 1e3 : sh == 2 ? exp(gauss() * 2.0) : (double)(nr() >> 33);
}
static int mode_small(int pairs, int P) {
    const char *nm[] = { "uniform", "normal", "lognormal", "int_wide" };
    const size_t total = (size_t)1 << 22;
    double *x = malloc(total * sizeof(double)), *w = malloc(total * sizeof(double));
    printf("# small: t(engine, P=%d) / t(bare AVX2 kernel), batches of distinct inputs (%zu elements per batch), %d pairs\n", P, total, pairs);
    printf("%-10s %8s %8s  %15s  %9s %9s\n", "shape", "n", "ratio", "p10-p90", "ns eng", "ns bare");
    for (int sh = 0; sh < 4; sh++) {
        if (skip(nm[sh])) continue;
        for (size_t n = 4096; n <= 262144; n *= 2) {
            size_t inst = total / n;
            rs = 0x1234567ULL + (uint64_t)sh * 977 + n;
            gen_unstr(sh, x, n * inst);
            if (!check(run_logos, &P, x, n) || !check(run_bare, NULL, x, n)) { printf("%-10s %8zu MISMATCH\n", nm[sh], n); continue; }
            double p10, p90, ma, mb;
            double r = paired(run_logos, &P, x, run_bare, NULL, x, w, n, inst, pairs, &p10, &p90, &ma, &mb);
            printf("%-10s %8zu %8.3f  %6.3f - %6.3f  %9.2f %9.2f\n", nm[sh], n, r, p10, p90, ma * 1e6 / (double)total, mb * 1e6 / (double)total);
            fflush(stdout);
        }
    }
    free(x); free(w); return 0;
}

/* ================= hub: one equi-width scatter + the kernel per bucket ================= */
static int mode_hub(int pairs, const char *nlist) {
    int one = 1;
    printf("# hub: t(engine, 1 thread) / t(told hub and spoke), %d pairs. ABOVE 1: the hub would win. T = elements per bucket; wc = write-combined scatter\n", pairs);
    printf("%-8s %9s %6s %3s %8s  %15s  %9s %9s\n", "shape", "n", "T", "wc", "ratio", "p10-p90", "ns eng", "ns hub");
    for (const char *p = nlist; *p; ) {
        size_t n = (size_t)atof(p); while (*p && *p != ',') p++; if (*p == ',') p++;
        double *x = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double)), *buf = malloc(n * sizeof(double));
        for (int sh = 0; sh < 2; sh++) {
            const char *nm = sh ? "normal" : "uniform";
            if (skip(nm)) continue;
            rs = 0xABCDEFULL + n + (uint64_t)sh; gen_unstr(sh, x, n);
            const size_t Ts[] = { 4096, 16384, 65536 };
            for (int ti = 0; ti < 3; ti++) for (int wc = 0; wc < 2; wc++) {
                size_t T = Ts[ti], B = n / T + 1;
                hub_t h = { T, wc, buf, malloc(B * sizeof(size_t)), malloc(B * sizeof(size_t)), aligned_alloc(64, ((B * 64 + 63) / 64) * 64), malloc(B) };
                if (!check(run_hub, &h, x, n) || !check(run_logos, &one, x, n)) { printf("%-8s %9zu MISMATCH\n", nm, n); continue; }
                double p10, p90, ma, mb;
                double r = paired(run_logos, &one, x, run_hub, &h, x, w, n, 1, pairs, &p10, &p90, &ma, &mb);
                printf("%-8s %9zu %6zu %3d %8.3f  %6.3f - %6.3f  %9.2f %9.2f\n", nm, n, T, wc, r, p10, p90, ma * 1e6 / (double)n, mb * 1e6 / (double)n);
                fflush(stdout);
                free(h.cnt); free(h.pos); free(h.stage); free(h.fill);
            }
        }
        free(x); free(w); free(buf);
    }
    return 0;
}

/* ================= weave: TDM channels and interleaved sources ================= */
/* kind 0: TDM, channel i % k; dirs bit j = channel j descending; sep = channels in disjoint value ranges.
   kind 1: k sorted sources interleaved at random, each next element from a source chosen in proportion to what it has left (a riffle). */
static void gen_weave(double *a, uint8_t *lab, size_t n, int kind, int k, unsigned dirs, int sep) {
    double *ch[64]; size_t m[64], at[64];
    for (int j = 0; j < k; j++) {
        m[j] = kind == 0 ? (n - (size_t)j + (size_t)k - 1) / (size_t)k : n / (size_t)k + ((size_t)j < n % (size_t)k);
        ch[j] = malloc(m[j] * sizeof(double)); at[j] = 0;
        sorted_uniform(ch[j], m[j], sep ? (double)j * 2e6 : 0.0, 1e6);
        if (dirs >> j & 1) for (size_t l = 0, r = m[j] - 1; l < r; l++, r--) { double t = ch[j][l]; ch[j][l] = ch[j][r]; ch[j][r] = t; }
    }
    if (kind == 0) for (size_t i = 0; i < n; i++) { int j = (int)(i % (size_t)k); a[i] = ch[j][i / (size_t)k]; lab[i] = (uint8_t)j; }
    else {
        size_t left = n;
        for (size_t i = 0; i < n; i++) {
            size_t r = nr() % left; int j = 0;
            while (r >= m[j] - at[j]) { r -= m[j] - at[j]; j++; }
            a[i] = ch[j][at[j]++]; lab[i] = (uint8_t)j; left--;
        }
    }
    for (int j = 0; j < k; j++) free(ch[j]);
}
static int mode_weave(int pairs, size_t n, int P) {
    struct { const char *nm; int kind, k; unsigned dirs; int sep; } S[] = {
        { "tdm2", 0, 2, 0, 0 }, { "zipper", 0, 2, 2, 0 }, { "tdm4", 0, 4, 0, 0 }, { "tdm4_alt", 0, 4, 0xA, 0 }, { "tdm8", 0, 8, 0, 0 },
        { "tdm16", 0, 16, 0, 0 }, { "tdm8_sep", 0, 8, 0, 1 }, { "src2", 1, 2, 0, 0 }, { "src4", 1, 4, 0, 0 }, { "src8", 1, 8, 0, 0 },
        { "src16", 1, 16, 0, 0 }, { "src4_desc2", 1, 4, 0x3, 0 } };
    double *x = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double)), *dm = malloc(n * sizeof(double)), *buf = malloc(n * sizeof(double));
    uint8_t *lab = malloc(n);
    printf("# weave: n = %zu, P = %d, %d pairs. ceiling = t(engine) / t(told demux + engine on the strands); runs = t(engine) / t(engine on the\n"
           "# strands already laid out). ABOVE 1: the interleave is costing the engine that much.\n", n, P, pairs);
    printf("%-11s %8s  %15s %8s  %9s %9s %9s\n", "shape", "ceiling", "p10-p90", "runs", "ns eng", "ns told", "ns runs");
    for (size_t s = 0; s < sizeof S / sizeof *S; s++) {
        if (skip(S[s].nm)) continue;
        rs = 0x5151ULL + s * 7919;
        gen_weave(x, lab, n, S[s].kind, S[s].k, S[s].dirs, S[s].sep);
        demux_t d = { lab, S[s].k, buf, P };
        { size_t cnt[256] = { 0 }, pos[256], q = 0; for (size_t i = 0; i < n; i++) cnt[lab[i]]++;            /* the strands, laid out */
          for (int j = 0; j < S[s].k; j++) { pos[j] = q; q += cnt[j]; } for (size_t i = 0; i < n; i++) dm[pos[lab[i]]++] = x[i]; }
        if (!check(run_logos, &P, x, n) || !check(run_demux, &d, x, n)) { printf("%-11s MISMATCH\n", S[s].nm); continue; }
        double p10, p90, ma, mb, q10, q90, qa, qb;
        double r = paired(run_logos, &P, x, run_demux, &d, x, w, n, 1, pairs, &p10, &p90, &ma, &mb);
        double r2 = paired(run_logos, &P, x, run_logos, &P, dm, w, n, 1, pairs, &q10, &q90, &qa, &qb);
        printf("%-11s %8.3f  %6.3f - %6.3f %8.3f  %9.2f %9.2f %9.2f\n", S[s].nm, r, p10, p90, r2, ma * 1e6 / (double)n, mb * 1e6 / (double)n, qb * 1e6 / (double)n);
        fflush(stdout);
    }
    free(x); free(w); free(dm); free(buf); free(lab); return 0;
}

/* ================= moire: periodic inputs across periods ================= */
static void gen_periodic(int fam, double *a, size_t n, size_t p) {
    double sh = 1.0 / (double)(n / p + 2);
    for (size_t i = 0; i < n; i++) {
        size_t ph = i % p, h = p / 2;
        if (fam == 0) a[i] = (double)ph + (double)(i / p) * sh;                                         /* ramps, each one shifted a hair */
        else if (fam == 1) a[i] = (double)(ph < h ? ph : p - ph) + u01() * 1e-3;                     /* triangle, CLUSTERED: n / (p/2) per band, within 1e-3 */
        else if (fam == 3) a[i] = (double)(ph < h ? ph : p - ph) + (double)(i / p) * sh + (ph < h ? 0.0 : 0.5 * sh);   /* triangle, every band filled evenly */
        else a[i] = (ph < (p + 1) / 2 ? 0.0 : 1e6) + u01() * 1e5;                                      /* square: two levels, noise inside */
    }
}
static int mode_moire(int pairs, size_t n, int P) {
    const size_t per[] = { 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 48, 63, 64, 65, 96, 127, 128, 129, 192, 255, 256, 257, 384, 511, 512,
                           513, 1000, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 8192, 16384, 32768, 65536 };
    const char *fam[] = { "ramp", "tri_clu", "square", "triangle" };
    double *x = malloc(n * sizeof(double)), *y = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double));
    printf("# moire: n = %zu, P = %d, %d pairs. t(periodic as it lies) / t(the same values shuffled); a spike against its neighbours is a finding\n", n, P, pairs);
    printf("%7s", "period"); for (int f = 0; f < 4; f++) if (!skip(fam[f])) printf("  %8s %6s", fam[f], "ns"); printf("\n");
    for (size_t pi = 0; pi < sizeof per / sizeof *per; pi++) {
        printf("%7zu", per[pi]);
        for (int f = 0; f < 4; f++) {
            if (skip(fam[f])) continue;
            rs = 0x77ULL + per[pi] * 31 + (uint64_t)f;
            gen_periodic(f, x, n, per[pi]); memcpy(y, x, n * sizeof(double)); shuffle(y, n);
            if (!check(run_logos, &P, x, n)) { printf("  MISMATCH       "); continue; }
            double p10, p90, ma, mb;
            double r = paired(run_logos, &P, x, run_logos, &P, y, w, n, 1, pairs, &p10, &p90, &ma, &mb);
            printf("  %8.3f %6.2f", r, ma * 1e6 / (double)n);
        }
        printf("\n"); fflush(stdout);
    }
    free(x); free(y); free(w); return 0;
}

/* ================= founder: an unrepresentative prefix ================= */
static void gen_base(int sh, double *a, size_t n) {
    switch (sh) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; break;
    case 1: for (size_t i = 0; i < n; i++) a[i] = gauss() * 1e3; break;
    case 2: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() >> 33); break;
    case 3: for (size_t i = 0; i < n; i++) a[i] = (double)i + 0.5; for (size_t s = 0; s < n / 100; s++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; } break;
    case 4: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 100000) * 0.01; break;
    case 5: for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 16) * 3.25; break;
    }
}
static int mode_founder(int pairs, size_t n, int P) {
    const char *shn[] = { "uniform", "normal", "int_wide", "near_sorted", "quantized", "few16" };
    const char *fdn[] = { "64 sorted 0..63", "64 integers", "64 copies", "4096 own sorted", "4096 huge", "65536 own sorted" };
    const size_t fsz[] = { 64, 64, 64, 4096, 4096, 65536 };
    double *x = malloc(n * sizeof(double)), *y = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double));
    printf("# founder: n = %zu, P = %d, %d pairs. t(founder at the front) / t(the same elements, the founder scattered at random positions)\n", n, P, pairs);
    printf("%-12s %-17s %8s  %15s  %9s\n", "shape", "founder", "ratio", "p10-p90", "ns front");
    for (int sh = 0; sh < 6; sh++) {
        if (skip(shn[sh])) continue;
        for (int fd = 0; fd < 6; fd++) {
            size_t f = fsz[fd];
            rs = 0x99ULL + (uint64_t)sh * 101 + (uint64_t)fd;
            gen_base(sh, x, n);
            if (fd == 0) for (size_t i = 0; i < f; i++) x[i] = (double)i;
            else if (fd == 1) for (size_t i = 0; i < f; i++) x[i] = floor(x[i]);
            else if (fd == 2) for (size_t i = 0; i < f; i++) x[i] = 123.0;
            else if (fd == 3 || fd == 5) qsort(x, f, sizeof(double), cmpd);
            else for (size_t i = 0; i < f; i++) x[i] = 1e15 * (1.0 + u01());
            memcpy(y, x, n * sizeof(double));
            for (size_t i = 0; i < f; i++) { size_t j = f + nr() % (n - f); double t = y[i]; y[i] = y[j]; y[j] = t; }
            if (!check(run_logos, &P, x, n) || !check(run_logos, &P, y, n)) { printf("%-12s %-17s MISMATCH\n", shn[sh], fdn[fd]); continue; }
            double p10, p90, ma, mb;
            double r = paired(run_logos, &P, x, run_logos, &P, y, w, n, 1, pairs, &p10, &p90, &ma, &mb);
            printf("%-12s %-17s %8.3f  %6.3f - %6.3f  %9.2f\n", shn[sh], fdn[fd], r, p10, p90, ma * 1e6 / (double)n);
            fflush(stdout);
        }
    }
    free(x); free(y); free(w); return 0;
}

/* ================= landauer: ns per element against bits per element ================= */
static double lg2fact(double m) { return lgamma(m + 1.0) / log(2.0); }
static double gen_entropy(int sh, double *a, size_t n, const char **name) {   /* fills a, returns bits per element of the generator */
    static const char *nm[] = { "uniform", "sorted", "reversed", "few2", "few16", "few256", "few4096", "blocks64", "blocks1024", "blocks16384",
                                "src2", "src4", "src8", "src16", "tdm4", "zipper", "near_1pct", "reach64", "reach4096", "coarse64k", "runs16",
                                "two_halves", "saw64", "rotated" };
    if (sh >= (int)(sizeof nm / sizeof *nm)) return -1;
    *name = nm[sh];
    double N = (double)n;
    uint8_t *lab = malloc(n);
    double H = 0;
    switch (sh) {
    case 0: for (size_t i = 0; i < n; i++) a[i] = u01() * 1e6; H = lg2fact(N) / N; break;
    case 1: sorted_uniform(a, n, 0, 1e6); H = 0; break;
    case 2: sorted_uniform(a, n, 0, 1e6); for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } H = 0; break;
    case 3: case 4: case 5: case 6: { size_t k = sh == 3 ? 2 : sh == 4 ? 16 : sh == 5 ? 256 : 4096;
        for (size_t i = 0; i < n; i++) a[i] = (double)(i % k) * 7.5 + 1.25;
        shuffle(a, n);
        H = (lg2fact(N) - (double)k * lg2fact(N / (double)k)) / N; break; }
    case 7: case 8: case 9: { size_t B = sh == 7 ? 64 : sh == 8 ? 1024 : 16384, L = n / B; size_t *perm = malloc(B * sizeof *perm);
        for (size_t b = 0; b < B; b++) perm[b] = b;
        for (size_t b = B - 1; b > 0; b--) { size_t j = nr() % (b + 1), t = perm[b]; perm[b] = perm[j]; perm[j] = t; }
        for (size_t i = 0; i < n; i++) { size_t b = i / L < B ? i / L : B - 1; a[i] = (double)(perm[b] * L) + (double)(i - b * L) + 0.25; }
        free(perm); H = lg2fact((double)B) / N; break; }
    case 10: case 11: case 12: case 13: { int k = sh == 10 ? 2 : sh == 11 ? 4 : sh == 12 ? 8 : 16;
        gen_weave(a, lab, n, 1, k, 0, 0); H = (lg2fact(N) - (double)k * lg2fact(N / (double)k)) / N; break; }
    case 14: gen_weave(a, lab, n, 0, 4, 0, 0); H = (lg2fact(N) - 4.0 * lg2fact(N / 4.0)) / N; break;
    case 15: gen_weave(a, lab, n, 0, 2, 2, 0); H = (lg2fact(N) - 2.0 * lg2fact(N / 2.0)) / N; break;
    case 16: { sorted_uniform(a, n, 0, 1e6); size_t s = n / 100; for (size_t q = 0; q < s; q++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; }
        H = ((double)s * 2.0 * log2(N) - lg2fact((double)s) - (double)s) / N; break; }
    case 17: case 18: case 19: { size_t W = sh == 17 ? 64 : sh == 18 ? 4096 : 65536; sorted_uniform(a, n, 0, 1e6);
        for (size_t b = 0; b < n; b += W) shuffle(a + b, b + W <= n ? W : n - b);
        H = lg2fact((double)W) / (double)W; break; }
    case 20: { size_t R = n / 16; for (size_t i = 0; i < n; i++) a[i] = 0; double v = 0;
        for (size_t i = 0; i < n; i++) { if (i % 16 == 0) v = u01() * 1e9; a[i] = v; } H = lg2fact((double)R) / N; break; }
    case 21: sorted_uniform(a, n / 2, 0, 1e6); sorted_uniform(a + n / 2, n - n / 2, 0, 1e6); H = (lg2fact(N) - 2.0 * lg2fact(N / 2.0)) / N; break;
    case 22: { size_t R = 64, L = n / R; for (size_t r = 0; r < R; r++) sorted_uniform(a + r * L, r + 1 < R ? L : n - r * L, 0, 1e6);
        H = (lg2fact(N) - (double)R * lg2fact(N / (double)R)) / N; break; }
    case 23: { sorted_uniform(a, n, 0, 1e6); size_t k = n / 3 + 17; double *t = malloc(n * sizeof(double));
        memcpy(t, a + k, (n - k) * sizeof(double)); memcpy(t + (n - k), a, k * sizeof(double)); memcpy(a, t, n * sizeof(double)); free(t); H = log2(N) / N; break; }
    }
    free(lab); return H;
}
static int mode_landauer(int reps, size_t n, int P) {
    double *x = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double));
    double ns_u = 0, h_u = 0;
    printf("# landauer: n = %zu, P = %d, min of %d reps. bits = log2 of the number of inputs the generator can make, per element.\n"
           "# excess = (ns/elem) / (bits x uniform's ns per bit): 1 = the shape costs exactly its share of information; above 1, order paid for and not used.\n", n, P, reps);
    printf("%-12s %7s %9s %9s %8s\n", "shape", "bits", "ns/elem", "ns/bit", "excess");
    for (int sh = 0; ; sh++) {
        const char *nm; rs = 0x4242ULL + (uint64_t)sh * 13;
        double H = gen_entropy(sh, x, n, &nm);
        if (H < 0) break;
        if (skip(nm) && sh != 0) continue;
        if (!check(run_logos, &P, x, n)) { printf("%-12s MISMATCH\n", nm); continue; }
        double best = 1e30;
        for (int r = 0; r < reps; r++) { memcpy(w, x, n * sizeof(double)); double t0 = now_ms(); logos_sort_f64(w, n, P); double dt = now_ms() - t0; if (dt < best) best = dt; }
        double ns = best * 1e6 / (double)n;
        if (sh == 0) { ns_u = ns; h_u = H; }
        if (H >= 0.05) printf("%-12s %7.3f %9.2f %9.3f %8.2f\n", nm, H, ns, ns / H, ns / (H * ns_u / h_u));
        else printf("%-12s %7.3f %9.2f %9s %8s\n", nm, H, ns, "-", "-");
        fflush(stdout);
    }
    free(x); free(w); return 0;
}

/* ================= once / seeds: one shape, by name, for a -DLOGOS_TRACE build (once) or across seeds (seeds) =================
 *   once  tri|ramp|square <period> [n] [P]      once tdm|src|runs <k> [n] [P] [dirs]      once ent <name> [n] [P]
 *   once  founder <shape 0-5> <founder 0-5> [n] [P]                                       seeds <same arguments as once>
 * runs = k sorted strands, each spanning the range, laid out one after another (what a demux hands the run routes). */
static int gen_named(int argc, char **argv, double *x, size_t *pn, int *pP) {
    const char *f = argv[2]; size_t n = *pn; int P = 1;
    if (!strcmp(f, "founder")) {
        int sh = atoi(argv[3]), fd = atoi(argv[4]);
        if (argc > 5) n = (size_t)atof(argv[5]);
        if (argc > 6) P = atoi(argv[6]);
        const size_t fsz[] = { 64, 64, 64, 4096, 4096, 65536 }; size_t fn = fsz[fd];
        gen_base(sh, x, n);
        if (fd == 0) for (size_t i = 0; i < fn; i++) x[i] = (double)i;
        else if (fd == 1) for (size_t i = 0; i < fn; i++) x[i] = floor(x[i]);
        else if (fd == 2) for (size_t i = 0; i < fn; i++) x[i] = 123.0;
        else if (fd == 3 || fd == 5) qsort(x, fn, sizeof(double), cmpd);
        else for (size_t i = 0; i < fn; i++) x[i] = 1e15 * (1.0 + u01());
        if (argc > 7) for (size_t i = 0; i < fn; i++) { size_t j = fn + nr() % (n - fn); double t = x[i]; x[i] = x[j]; x[j] = t; }   /* scattered */
    } else {
        if (argc > 4) n = (size_t)atof(argv[4]);
        if (argc > 5) P = atoi(argv[5]);
        if (!strcmp(f, "tri") || !strcmp(f, "triclu") || !strcmp(f, "ramp") || !strcmp(f, "square"))
            gen_periodic(f[0] == 'r' ? 0 : !strcmp(f, "triclu") ? 1 : f[0] == 't' ? 3 : 2, x, n, (size_t)atof(argv[3]));
        else if (!strcmp(f, "tdm") || !strcmp(f, "src")) {
            uint8_t *lab = malloc(n); gen_weave(x, lab, n, f[0] == 's', atoi(argv[3]), argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0, 0); free(lab);
        } else if (!strcmp(f, "runs")) {
            size_t k = (size_t)atoi(argv[3]), L = n / k;
            for (size_t r = 0; r < k; r++) sorted_uniform(x + r * L, r + 1 < k ? L : n - r * L, 0, 1e6);
        } else if (!strcmp(f, "ent")) {
            const char *nm; int sh = 0; double H;
            while ((H = gen_entropy(sh, x, n, &nm)) >= 0 && strcmp(nm, argv[3])) sh++;
            if (H < 0) { fprintf(stderr, "no entropy shape %s\n", argv[3]); return 0; }
        } else { fprintf(stderr, "unknown family %s\n", f); return 0; }
    }
    *pn = n; *pP = P; return 1;
}
static int mode_once(int argc, char **argv, int seeds) {
    size_t n = (size_t)1 << 20; int P = 1;
    double *x = malloc(((size_t)1 << 25) * sizeof(double)), *w = malloc(((size_t)1 << 25) * sizeof(double));
    if (argc < 4) { fprintf(stderr, "once <family> <param> ...\n"); return 1; }
    for (int s = 0; s < (seeds ? 8 : 1); s++) {
        rs = 0x2545F4914F6CDD1DULL + (uint64_t)(seeds || !getenv("FP_SEED") ? s : atoi(getenv("FP_SEED"))) * 0x9E3779B97F4A7C15ULL;
        if (!gen_named(argc, argv, x, &n, &P)) return 1;
        if (getenv("FP_SHUFFLE")) shuffle(x, n);                       /* the same values, the order destroyed */
        if (!check(run_logos, &P, x, n)) { printf("MISMATCH\n"); return 1; }
        double best = 1e30;
        for (int r = 0; r < (seeds ? 5 : 1); r++) {
            memcpy(w, x, n * sizeof(double));
            if (!seeds) fprintf(stderr, "\n===== %s %s, n = %zu, P = %d =====\n", argv[2], argv[3], n, P);
            double t0 = now_ms(); logos_sort_f64(w, n, P); double dt = now_ms() - t0; if (dt < best) best = dt;
        }
        printf("%s %s seed %d: %.2f ns/elem\n", argv[2], argv[3], s, best * 1e6 / (double)n); fflush(stdout);
    }
    free(x); free(w); return 0;
}

/* ================= ab: one per-sort switch against the engine as it ships, on this round's shapes =================
 *   FP_SWITCH=<NAME> [FP_VALUE=<v>] ./fieldprobe ab [pairs] [n] [P] [filter]     > 1: shipped is faster. FP_SWITCH unset: a null run.
 * Shapes: the moire rows (triangles and ramps at the periods that misfired, a square wave), Weyl and van der Corput sequences (what
 * the low-discrepancy probe is FOR: they must keep it), founders, interleaved sources and TDM channels, k strands laid out, 1% and 2%
 * random swaps, and plain controls. */
static const struct { const char *nm; char fam; int p1, p2; } ABS[] = {
    { "uniform", 'E', 0, 0 }, { "normal", 'F', 1, -1 }, { "tri256", 'T', 256, 0 }, { "tri1024", 'T', 1024, 0 }, { "tri4096", 'T', 4096, 0 },
    { "tri16384", 'T', 16384, 0 }, { "triclu1024", 'C', 1024, 0 }, { "triclu4096", 'C', 4096, 0 }, { "ramp64", 'R', 64, 0 }, { "ramp512", 'R', 512, 0 }, { "ramp2048", 'R', 2048, 0 }, { "square1024", 'S', 1024, 0 },
    { "weyl", 'Y', 0, 0 }, { "vdcorput", 'V', 0, 0 }, { "f_uni_64sorted", 'F', 0, 0 }, { "f_uni_4096sorted", 'F', 0, 3 }, { "f_norm_64int", 'F', 1, 1 },
    { "f_quant_64same", 'F', 4, 2 }, { "f_few16_huge", 'F', 5, 4 }, { "src2", 'W', 2, 0 }, { "src4", 'W', 4, 0 }, { "src8", 'W', 8, 0 },
    { "tdm4", 'D', 4, 0 }, { "zipper", 'D', 2, 2 }, { "runs3", 'U', 3, 0 }, { "runs4", 'U', 4, 0 }, { "runs8", 'U', 8, 0 }, { "runs16", 'U', 16, 0 },
    { "near_1pct", 'N', 100, 0 }, { "near_2pct", 'N', 50, 0 }, { "near_0.2pct", 'N', 500, 0 }, { "near_3pct", 'N', 33, 0 }, { "near_5pct", 'N', 20, 0 },
    { "kinks20", 'K', 40, 0 }, { "kinks4", 'K', 8, 0 }, { "near_head64", 'H', 64, 0 }, { "dom10", 'O', 10, 0 }, { "dom25", 'O', 25, 0 }, { "dom50", 'O', 50, 0 },
    { "dom75", 'O', 75, 0 }, { "dom90", 'O', 90, 0 }, { "saw64", 'E', 22, 0 }, { "two_halves", 'E', 21, 0 },
    { "coarse64k", 'E', 19, 0 }, { "blocks1024", 'E', 8, 0 }, { "runs16eq", 'E', 20, 0 } };
static void gen_ab(int s, double *x, size_t n) {
    const char *nm; uint8_t *lab;
    switch (ABS[s].fam) {
    case 'E': gen_entropy(ABS[s].p1, x, n, &nm); break;
    case 'T': gen_periodic(3, x, n, (size_t)ABS[s].p1); break;
    case 'C': gen_periodic(1, x, n, (size_t)ABS[s].p1); break;
    case 'R': gen_periodic(0, x, n, (size_t)ABS[s].p1); break;
    case 'S': gen_periodic(2, x, n, (size_t)ABS[s].p1); break;
    case 'Y': for (size_t i = 0; i < n; i++) { double v = (double)i * 0.6180339887498949; x[i] = (v - floor(v)) * 1e6; } break;
    case 'V': for (size_t i = 0; i < n; i++) { double r = 0, f = 0.5; for (size_t k = i + 1; k; k >>= 1, f *= 0.5) if (k & 1) r += f; x[i] = r * 1e6; } break;
    case 'F': { int sh = ABS[s].p1, fd = ABS[s].p2; gen_base(sh, x, n); if (fd < 0) break;
        const size_t fsz[] = { 64, 64, 64, 4096, 4096, 65536 }; size_t f = fsz[fd];
        if (fd == 0) for (size_t i = 0; i < f; i++) x[i] = (double)i;
        else if (fd == 1) for (size_t i = 0; i < f; i++) x[i] = floor(x[i]);
        else if (fd == 2) for (size_t i = 0; i < f; i++) x[i] = 123.0;
        else if (fd == 3 || fd == 5) qsort(x, f, sizeof(double), cmpd);
        else for (size_t i = 0; i < f; i++) x[i] = 1e15 * (1.0 + u01());
        break; }
    case 'W': lab = malloc(n); gen_weave(x, lab, n, 1, ABS[s].p1, 0, 0); free(lab); break;
    case 'D': lab = malloc(n); gen_weave(x, lab, n, 0, ABS[s].p1, (unsigned)ABS[s].p2, 0); free(lab); break;
    case 'U': { size_t k = (size_t)ABS[s].p1, L = n / k; for (size_t r = 0; r < k; r++) sorted_uniform(x + r * L, r + 1 < k ? L : n - r * L, 0, 1e6); break; }
    case 'K': sorted_uniform(x, n, 0, 1e6);                         /* adjacent pairs swapped with probability p1 %: p1 / 2 % descents */
        for (size_t i = 0; i + 1 < n; i += 2) if (nr() % 100 < (uint64_t)ABS[s].p1) { double t = x[i]; x[i] = x[i + 1]; x[i + 1] = t; }
        break;
    case 'O': for (size_t i = 0; i < n; i++) x[i] = (int)(nr() % 100) < ABS[s].p1 ? 12345.0 : u01() * 2e9 - 1e9; break;   /* one value holding p1 % */
    case 'H': sorted_uniform(x, n, 0, 1e6);                         /* 1% random swaps, and the first p1 places hold elements from anywhere */
        for (size_t q = 0; q < n / 100; q++) { size_t i = nr() % n, j = nr() % n; double t = x[i]; x[i] = x[j]; x[j] = t; }
        for (size_t i = 0; i < (size_t)ABS[s].p1; i++) { size_t j = (size_t)ABS[s].p1 + nr() % (n - (size_t)ABS[s].p1); double t = x[i]; x[i] = x[j]; x[j] = t; }
        break;
    case 'N': sorted_uniform(x, n, 0, 1e6); for (size_t q = 0; q < n / (size_t)ABS[s].p1; q++) { size_t i = nr() % n, j = nr() % n; double t = x[i]; x[i] = x[j]; x[j] = t; } break;
    }
}
static const char *g_sw, *g_swv;
static void run_switch(double *w, size_t n, void *c) { setenv(g_sw, g_swv ? g_swv : "1", 1); logos_sort_f64(w, n, *(int *)c); unsetenv(g_sw); }
static int mode_ab(int pairs, size_t n, int P) {
    g_sw = getenv("FP_SWITCH"); g_swv = getenv("FP_VALUE");
    double *x = malloc(n * sizeof(double)), *w = malloc(n * sizeof(double));
    printf("# fieldprobe ab: switch %s%s%s, n = %zu, P = %d, %d pairs. Column: median t(switch) / t(shipped), p10-p90. > 1: shipped is faster.\n",
           g_sw ? g_sw : "(none: null run)", g_swv ? "=" : "", g_swv ? g_swv : "", n, P, pairs);
    printf("%-18s %8s  %15s  %9s %9s\n", "shape", "ratio", "p10-p90", "ns switch", "ns ship");
    for (size_t s = 0; s < sizeof ABS / sizeof *ABS; s++) {
        if (skip(ABS[s].nm)) continue;
        rs = 0x6060ULL + s * 104729; gen_ab((int)s, x, n);
        run_fn fa = g_sw ? run_switch : run_logos;
        if (!check(run_logos, &P, x, n) || !check(fa, &P, x, n)) { printf("%-18s MISMATCH\n", ABS[s].nm); fflush(stdout); continue; }
        double p10, p90, ma, mb;
        double r = paired(fa, &P, x, run_logos, &P, x, w, n, 1, pairs, &p10, &p90, &ma, &mb);
        printf("%-18s %8.3f  %6.3f - %6.3f  %9.2f %9.2f\n", ABS[s].nm, r, p10, p90, ma * 1e6 / (double)n, mb * 1e6 / (double)n);
        fflush(stdout);
    }
    free(x); free(w); return 0;
}

/* ================= i32ab: one switch on the INT32 API and on int-valued doubles (the memory round, 2026-10-22) =================
 *   FP_SWITCH=<NAME> [FP_VALUE=<v>] ./fieldprobe i32ab [pairs] [n] [P]     > 1: shipped is faster. Output checked against qsort first. */
static int cmpi(const void *x, const void *y) { int32_t a = *(const int32_t *)x, b = *(const int32_t *)y; return (a > b) - (a < b); }
static int mode_i32ab(int pairs, size_t n, int P) {
    const char *sw = getenv("FP_SWITCH"), *val = getenv("FP_VALUE");
    const char *nm[] = { "i32_wide", "i32_24bit", "i32_16bit", "f64_intwide", "f64_int24bit" };
    int32_t *x = malloc(n * 4), *w = malloc(n * 4), *r = malloc(n * 4);
    double *xd = malloc(n * 8), *wd = malloc(n * 8), *rd = malloc(n * 8);
    double *ra = malloc((size_t)pairs * sizeof(double));
    printf("# fieldprobe i32ab: switch %s%s%s, n = %zu, P = %d, %d pairs. median t(switch) / t(shipped); > 1: shipped is faster\n",
           sw ? sw : "(none)", val ? "=" : "", val ? val : "", n, P, pairs);
    for (int s = 0; s < 5; s++) {
        if (skip(nm[s])) continue;
        rs = 0x1E32ULL + (uint64_t)s * 977;
        int dbl = s >= 3;
        for (size_t i = 0; i < n; i++) {
            uint64_t v = nr(); int32_t k = s == 0 || s == 3 ? (int32_t)(uint32_t)v : s == 1 || s == 4 ? (int32_t)(v % (1u << 24)) - (1 << 23) : (int32_t)(v % 65536u);
            if (dbl) xd[i] = (double)k; else x[i] = k;
        }
        int bad = 0;
        for (int side = 0; side < 2 && !bad; side++) {
            if (side && sw) setenv(sw, val ? val : "1", 1);
            if (dbl) { memcpy(wd, xd, n * 8); memcpy(rd, xd, n * 8); qsort(rd, n, 8, cmpd); logos_sort_f64(wd, n, P); bad = memcmp(wd, rd, n * 8) != 0; }
            else { memcpy(w, x, n * 4); memcpy(r, x, n * 4); qsort(r, n, 4, cmpi); logos_sort_i32(w, n, P); bad = memcmp(w, r, n * 4) != 0; }
            if (sw) unsetenv(sw);
        }
        if (bad) { printf("%-14s MISMATCH\n", nm[s]); continue; }
        double ts = 0, tb = 0;
        for (int i = 0; i < pairs; i++) {
            double t[2];
            for (int k = 0; k < 2; k++) {
                int do_sw = (k == 0) == ((i & 1) == 0);
                if (dbl) memcpy(wd, xd, n * 8); else memcpy(w, x, n * 4);
                if (do_sw && sw) setenv(sw, val ? val : "1", 1);
                double t0 = now_ms(); if (dbl) logos_sort_f64(wd, n, P); else logos_sort_i32(w, n, P); double dt = now_ms() - t0;
                if (do_sw && sw) unsetenv(sw);
                t[do_sw ? 0 : 1] = dt;
            }
            ra[i] = t[0] / t[1]; ts += t[0]; tb += t[1];
        }
        qsort(ra, (size_t)pairs, sizeof(double), cmpd);
        double med = (pairs & 1) ? ra[pairs / 2] : 0.5 * (ra[pairs / 2 - 1] + ra[pairs / 2]);
        printf("%-14s %8.3f  %6.3f - %6.3f  %9.2f %9.2f\n", nm[s], med, ra[(int)(0.1 * pairs)], ra[(int)(0.9 * pairs)], ts / pairs * 1e6 / (double)n, tb / pairs * 1e6 / (double)n);
        fflush(stdout);
    }
    free(x); free(w); free(r); free(xd); free(wd); free(rd); free(ra); return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "small";
    if (!strcmp(mode, "once") || !strcmp(mode, "seeds")) {
        g_ref = malloc(((size_t)1 << 25) * sizeof(double)); g_chk = malloc(((size_t)1 << 25) * sizeof(double));
        return mode_once(argc, argv, mode[0] == 's');
    }
    int pairs = argc > 2 ? atoi(argv[2]) : 15;
    if (argc > 5) g_filt = argv[5];
    const size_t cap = (size_t)1 << 25;
    g_ref = malloc(cap * sizeof(double)); g_chk = malloc(cap * sizeof(double));
    if (!strcmp(mode, "small")) { if (argc > 3) g_filt = argc > 4 ? argv[4] : ""; return mode_small(pairs, argc > 3 ? atoi(argv[3]) : 1); }
    if (!strcmp(mode, "hub")) { if (argc > 4) g_filt = argv[4]; return mode_hub(pairs, argc > 3 ? argv[3] : "1048576,2097152,4194304,8388608,16777216"); }
    size_t n = argc > 3 ? (size_t)atof(argv[3]) : (size_t)1 << 22;
    int P = argc > 4 ? atoi(argv[4]) : 1;
    if (n > cap) { fprintf(stderr, "n above %zu\n", cap); return 1; }
    if (!strcmp(mode, "weave")) return mode_weave(pairs, n, P);
    if (!strcmp(mode, "moire")) return mode_moire(pairs, n, P);
    if (!strcmp(mode, "founder")) return mode_founder(pairs, n, P);
    if (!strcmp(mode, "landauer")) return mode_landauer(pairs, n, P);
    if (!strcmp(mode, "ab")) return mode_ab(pairs, n, P);
    if (!strcmp(mode, "i32ab")) return mode_i32ab(pairs, n, P);
    fprintf(stderr, "modes: small hub weave moire founder landauer\n"); return 1;
}
