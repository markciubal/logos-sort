/* dateprobe: what would knowing the DATA FORMAT (these are dates/timestamps) buy over what the engine
 * already certifies from the data alone?
 *
 * Why this shape of question. The engine gets a bare `double *`, so "these are dates" is a DATA fact and must
 * be certified, not assumed (the count_try_i32 rule). But a format would hand over the STEP and the ORIGIN for
 * free -- 1 ms since an epoch, 1 day since an epoch -- which is exactly what the int32 type handed count_try_i32
 * when it went 37.6 -> 3.8 ms. CLAUDE.md 0m (a3) already records the symptom: "millisecond clocks (84k distinct
 * values on a 1 ms lattice) reach no counting route (1.6x over std::sort)".
 *
 * Read from the dispatcher, the gate that decides a timestamp's fate is unit-dependent. For integer-valued
 * doubles it tries, in order: a counting sort when span <= COUNTING_SORT_K * n, then the int32 radix when the
 * values fit +-2.1e9, then it falls through to the general float leaf. So epoch SECONDS over a day (span 86,400)
 * gets a counting sort, and epoch MILLISECONDS over the same day (span 86.4M = 10.3n, and 1.76e12 is outside
 * int32) gets neither.
 *
 * The ceiling here is a sort TOLD the format: subtract the origin, divide by the step, and LSD-radix only the
 * bytes that actually vary. That is what the format buys, priced. A 0-gap row means the engine already has it.
 *
 * PREDICTION, written before the first run (ceiling / engine, one thread):
 *   epoch_sec_day    1.0-1.1   counting fires (span 86,400 << n): nothing to win
 *   day_serial       1.0-1.1   counting fires (3,650 distinct)
 *   packed_ymd       1.0-1.2   fits int32 and the span is ~100k: counting fires
 *   epoch_ms_hour    1.0-1.2   span 3.6M < n: counting fires
 *   epoch_ms_day     2.0-4.0   NO route: span 10.3n and outside int32 -> general float leaf
 *   epoch_us_min     2.0-4.0   same hole (span 7.1n)
 *   excel_serial     1.5-3.0   not integer at all (fractional day), so every integer gate is skipped
 * If epoch_ms_day comes back near 1.0 the hole is not real and this dies here.
 *
 * Usage: ./dateprobe [n] [reps]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <ctime>
extern "C" {
#include "logos_psort.h"
}

static uint64_t RS = 1;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / (double)(1ULL << 53); }
static double now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

struct Shape { const char *name; double origin, step; int integral; };

/* epoch origin: 2026-09-22 ~ 1.7789e9 seconds */
static const double EP = 1778900000.0;

enum { S_SEC_DAY, S_MS_DAY, S_MS_HOUR, S_US_MIN, S_DAY_SER, S_PACKED, S_EXCEL, S_N };
static Shape SH[S_N] = {
    { "epoch_sec_day  (1 s, 1 day)",      EP,            1.0,       1 },
    { "epoch_ms_day   (1 ms, 1 day)",     EP * 1000.0,   1.0,       1 },
    { "epoch_ms_hour  (1 ms, 1 hour)",    EP * 1000.0,   1.0,       1 },
    { "epoch_us_min   (1 us, 1 min)",     EP * 1000000.0, 1.0,      1 },
    { "day_serial     (1 day, 10 yr)",    20000.0,       1.0,       1 },
    { "packed_ymd     (YYYYMMDD, 10 yr)", 20200101.0,    1.0,       1 },
    { "excel_serial   (day + fraction)",  46000.0,       1.0 / 86400.0, 0 },
};

static void gen(double *a, size_t n, int s) {
    RS = 99173 + (uint64_t)s * 7919;
    for (size_t i = 0; i < n; i++) {
        switch (s) {
        case S_SEC_DAY:  a[i] = EP + (double)(nr() % 86400); break;
        case S_MS_DAY:   a[i] = EP * 1000.0 + (double)(nr() % 86400000ULL); break;
        case S_MS_HOUR:  a[i] = EP * 1000.0 + (double)(nr() % 3600000ULL); break;
        case S_US_MIN:   a[i] = EP * 1000000.0 + (double)(nr() % 60000000ULL); break;
        case S_DAY_SER:  a[i] = 20000.0 + (double)(nr() % 3650); break;
        case S_PACKED: { uint64_t y = 2020 + nr() % 10, m = 1 + nr() % 12, d = 1 + nr() % 28;
                         a[i] = (double)(y * 10000 + m * 100 + d); break; }
        default:         a[i] = 46000.0 + (double)(nr() % 3650) + (double)(nr() % 86400) / 86400.0; break;
        }
    }
}

/* the ceiling: TOLD the origin and the step, so the values are int64 keys with few varying bytes.
   LSD radix over exactly the bytes that vary, then back to doubles. */
static void told_sort(double *a, size_t n, double origin, double step) {
    std::vector<uint64_t> k(n), tmp(n);
    const double inv = 1.0 / step;
    uint64_t mn = ~0ULL, mx = 0;
    for (size_t i = 0; i < n; i++) { uint64_t v = (uint64_t)(long long)((a[i] - origin) * inv + 0.5); k[i] = v; if (v < mn) mn = v; if (v > mx) mx = v; }
    int nb = 0; { uint64_t r = mx - mn; while (r) { nb++; r >>= 8; } if (!nb) nb = 1; }
    for (size_t i = 0; i < n; i++) k[i] -= mn;
    for (int b = 0; b < nb; b++) {                       /* only the bytes that actually vary */
        size_t cnt[256] = {0};
        for (size_t i = 0; i < n; i++) cnt[(k[i] >> (8 * b)) & 0xFF]++;
        size_t acc = 0; for (int v = 0; v < 256; v++) { size_t c = cnt[v]; cnt[v] = acc; acc += c; }
        for (size_t i = 0; i < n; i++) tmp[cnt[(k[i] >> (8 * b)) & 0xFF]++] = k[i];
        k.swap(tmp);
    }
    for (size_t i = 0; i < n; i++) a[i] = origin + (double)(k[i] + mn) * step;
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 8388608;
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    int P = 14;
    std::vector<double> base(n), w(n), ref(n);

    printf("dateprobe: n = %zu, best of %d. ms; ceiling = a sort TOLD the origin and step.\n\n", n, reps);
    printf("  %-32s %9s %9s %9s %9s   %8s\n", "shape", "logos 1T", "logos 14T", "std::sort", "told 1T", "told/1T");
    for (int s = 0; s < S_N; s++) {
        gen(base.data(), n, s);
        ref = base; std::sort(ref.begin(), ref.end());
        double b1 = 1e18, bP = 1e18, bs = 1e18, bt = 1e18;
        for (int r = 0; r < reps; r++) {
            w = base; double t0 = now_ms(); logos_sort_f64(w.data(), n, 1); double d = now_ms() - t0;
            if (w != ref) { printf("  %-32s LOGOS 1T WRONG\n", SH[s].name); goto next; }
            if (d < b1) b1 = d;

            w = base; t0 = now_ms(); logos_sort_f64(w.data(), n, P); d = now_ms() - t0;
            if (w != ref) { printf("  %-32s LOGOS %dT WRONG\n", SH[s].name, P); goto next; }
            if (d < bP) bP = d;

            w = base; t0 = now_ms(); std::sort(w.begin(), w.end()); d = now_ms() - t0;
            if (d < bs) bs = d;

            w = base; t0 = now_ms(); told_sort(w.data(), n, SH[s].origin, SH[s].step); d = now_ms() - t0;
            if (w != ref) { printf("  %-32s TOLD WRONG (the shape is not on that lattice)\n", SH[s].name); goto next; }
            if (d < bt) bt = d;
        }
        printf("  %-32s %9.1f %9.1f %9.1f %9.1f   %8.2f\n", SH[s].name, b1, bP, bs, bt, b1 / bt);
    next:;
    }
    printf("\n  told/1T above 1 = what the format would buy at one thread, if the engine could be told.\n");
    return 0;
}
