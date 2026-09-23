/* datestrprobe: where knowing the date FORMAT actually pays -- when the dates are TEXT.
 *
 * For numeric timestamps the format buys nothing (see dateprobe: a sort told the origin and step is 1.3-4.2x
 * SLOWER than the engine, because the engine already certifies the only exploitable facts -- integrality,
 * narrow span, low cardinality -- from its sample). The most format knowledge can save there is the detection,
 * and detection is the shared sample at 1-4% of a parallel call.
 *
 * Text is the opposite case. A fixed-width ISO-8601 timestamp has a monotone map to an integer that the format
 * hands over for free, and lexicographic order on ISO-8601 IS chronological order, so comparing 24 characters
 * per comparison is pure waste. This prices that: sorting the strings against parsing once into a numeric key,
 * sorting the keys with the engine, and permuting the records.
 *
 * PREDICTION, written before the first run: key-extract + logos + permute beats std::sort on std::string by
 * 8-25x and beats memcmp on fixed 24-byte records by 3-8x, at one thread. If it comes back under 2x the idea
 * is not worth a route and dies here.
 *
 * Usage: ./datestrprobe [n] [reps]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <ctime>
extern "C" {
#include "logos_psort.h"
}

static uint64_t RS = 1;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

struct Rec { char s[24]; };           /* "2026-09-22T14:33:01.123Z", no terminator: fixed width */

static void make_iso(char *o, int y, int mo, int d, int h, int mi, int se, int ms) {
    auto p2 = [&](char *q, int v) { q[0] = char('0' + v / 10); q[1] = char('0' + v % 10); };
    o[0] = char('0' + y / 1000); o[1] = char('0' + y / 100 % 10); o[2] = char('0' + y / 10 % 10); o[3] = char('0' + y % 10);
    o[4] = '-'; p2(o + 5, mo); o[7] = '-'; p2(o + 8, d); o[10] = 'T';
    p2(o + 11, h); o[13] = ':'; p2(o + 14, mi); o[16] = ':'; p2(o + 17, se); o[19] = '.';
    o[20] = char('0' + ms / 100); o[21] = char('0' + ms / 10 % 10); o[22] = char('0' + ms % 10); o[23] = 'Z';
}

/* the format hands this over: fixed offsets, so no scanning and no branches */
static inline double key_of(const char *s) {
    auto d2 = [&](int i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); };
    long long y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    long long v = ((((y * 13 + d2(5)) * 32 + d2(8)) * 25 + d2(11)) * 61 + d2(14)) * 61 + d2(17);
    return (double)(v * 1000 + (s[20] - '0') * 100 + (s[21] - '0') * 10 + (s[22] - '0'));
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 2000000;
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    std::vector<Rec> base(n);
    RS = 4242;
    for (size_t i = 0; i < n; i++)
        make_iso(base[i].s, 2020 + int(nr() % 7), 1 + int(nr() % 12), 1 + int(nr() % 28),
                 int(nr() % 24), int(nr() % 60), int(nr() % 60), int(nr() % 1000));

    printf("datestrprobe: n = %zu fixed-width ISO-8601 timestamps, best of %d\n\n", n, reps);

    /* reference order, by bytes (lexicographic == chronological for ISO-8601) */
    std::vector<Rec> ref = base;
    std::sort(ref.begin(), ref.end(), [](const Rec &a, const Rec &b) { return memcmp(a.s, b.s, 24) < 0; });

    double b_str = 1e18, b_rec = 1e18, b_key = 1e18, t_parse = 0, t_sort = 0, t_perm = 0;

    for (int r = 0; r < reps; r++) {
        std::vector<std::string> v(n);
        for (size_t i = 0; i < n; i++) v[i].assign(base[i].s, 24);
        double t0 = now_ms(); std::sort(v.begin(), v.end()); double d = now_ms() - t0;
        if (memcmp(v[n / 2].data(), ref[n / 2].s, 24) != 0) { printf("  std::string sort WRONG\n"); return 1; }
        if (d < b_str) b_str = d;
    }
    for (int r = 0; r < reps; r++) {
        std::vector<Rec> v = base;
        double t0 = now_ms();
        std::sort(v.begin(), v.end(), [](const Rec &a, const Rec &b) { return memcmp(a.s, b.s, 24) < 0; });
        double d = now_ms() - t0;
        if (memcmp(v[n / 2].s, ref[n / 2].s, 24) != 0) { printf("  record memcmp sort WRONG\n"); return 1; }
        if (d < b_rec) b_rec = d;
    }
    for (int r = 0; r < reps; r++) {
        std::vector<Rec> v = base, out(n);
        std::vector<double> k(n);
        std::vector<uint32_t> idx(n);
        double t0 = now_ms();
        for (size_t i = 0; i < n; i++) k[i] = key_of(v[i].s);              /* parse once, at fixed offsets */
        double t1 = now_ms();
        logos_argsort_f64(k.data(), n, idx.data());     /* the engine's own answer, not std::sort on indices */
        double t2 = now_ms();
        for (size_t i = 0; i < n; i++) out[i] = v[idx[i]];   /* one gather over the records */
        double t3 = now_ms();
        double d = t3 - t0;
        if (memcmp(out[n / 2].s, ref[n / 2].s, 24) != 0) { printf("  key sort WRONG\n"); return 1; }
        if (d < b_key) { b_key = d; t_parse = t1 - t0; t_sort = t2 - t1; t_perm = t3 - t2; }
    }

    printf("  %-44s %9.1f ms\n", "std::sort on std::string", b_str);
    printf("  %-44s %9.1f ms\n", "std::sort on fixed 24-byte records (memcmp)", b_rec);
    printf("  %-44s %9.1f ms   (parse %.1f + logos %.1f + place %.1f)\n",
           "parse to a numeric key, argsort, gather", b_key, t_parse, t_sort, t_perm);
    printf("\n  vs std::string        %6.2fx\n", b_str / b_key);
    printf("  vs fixed records      %6.2fx\n", b_rec / b_key);
    printf("\n  parse = the format's whole contribution; argsort = logos on the keys; place = one gather over 24-byte records.\n");
    printf("  With std::sort on indices instead of logos_argsort_f64 the placement was 618 ms and the win only 2.67x:\n");
    printf("  the format buys the KEY, but it is worth nothing without a fast argsort behind it.\n");
    return 0;
}
