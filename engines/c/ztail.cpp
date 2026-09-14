/* ztail.cpp — tightening the quadtree probe's tail rule, validated on shapes it has never seen.
 *
 * zroute.cpp's held-out test found the tail rule too loose: "low early, peaks late" also fired on ID blocks
 * and on Gray-code order. The fix must NOT be validated on that zoo, or the held-out test becomes a fitted
 * one. So the new rule is written below, and both rules are scored here on fresh shapes: new generators,
 * a different n (5,000,000, not a power of two), and new seeds, none of them in the training set, the zoo,
 * or zprobe.
 *
 * WHY THE NEW RULE. A real tail puts random values across the WHOLE value range at the end of the array, so
 * even the coarsest grid is already off the diagonal (o[1] and o[2] above 1.5), and the late bulge is large
 * (peak at least 3). The two false tails the zoo exposed were slow risers that sit exactly on the diagonal
 * at the coarsest levels (o[1] = o[2] = 1.00) and never exceed about 2.1.
 *
 * PREDICTIONS, written before this ran:
 *   old rule: all 5 true tails caught; about 4 of 7 lookalikes falsely called tail (Gray code, ID blocks,
 *             growing noise, late swaps).
 *   new rule: 3 or 4 of 5 true tails (the 97% prefix and the narrow-range tail are at risk, because a
 *             small or narrow tail may not disturb the coarsest levels); about 1 or 2 of 7 false tails
 *             (Gray code and ID blocks removed; growing noise and late swaps may survive, since both
 *             disturb the coarse levels too).
 *   controls: uniform random, sorted, sawtooth unchanged under both.
 *
 * Run: make ztail && ./ztail                                                                          */
#define ZROUTE_MAIN zroute_unused_main
#include "zroute.cpp"
#undef main

static int zclass_v2(const zprof_t &p) {
    double mx = 0; int am = 1;
    for (int l = 1; l <= 6; l++) if (p.o[l] > mx) { mx = p.o[l]; am = l; }
    auto near = [](double x, double y) { return fabs(x - y) <= 0.15 * std::max(x, y); };
    if (mx <= 1.3)                                                     return Z_SORTED;
    if (p.o[5] >= 16 && p.o[6] >= 24)                                  return Z_RANDOM;
    if (near(p.o[4], p.o[5]) && near(p.o[5], p.o[6]) && p.o[5] >= 3)  return Z_KOVERLAP;
    if (p.o[1] >= 1.5 && p.o[2] > 1.5 && p.o[2] <= 3.0 && am >= 5 && mx >= 3.0) return Z_TAIL;   /* tightened */
    if (am <= 4 && p.o[6] < 0.6 * mx)                                  return Z_BOUNDED;
    return Z_UNKNOWN;
}

static uint32_t rev23(uint32_t x) { uint32_t r = 0; for (int b = 0; b < 23; b++) { r = (r << 1) | (x & 1); x >>= 1; } return r; }

static void fresh(int id, double *a, size_t n) {
    size_t i;
    auto prefix = [&](double f) { size_t d = (size_t)(f * (double)n); for (i = 0; i < n; i++) a[i] = i < d ? -1e9 + 2e9 * (double)i / (double)d : fr() * 2e9 - 1e9; };
    switch (id) {
    case 0: prefix(0.80); break;                                                          /* t1 */
    case 1: prefix(0.90); break;                                                          /* t2 */
    case 2: prefix(0.97); break;                                                          /* t3 */
    case 3: { size_t d = (size_t)(0.90 * n); for (i = 0; i < n; i++) a[i] = i < d ? -1e9 + 2e9 * (double)i / (double)d : -2e8 + fr() * 4e8; break; }  /* t4 narrow tail */
    case 4: { size_t d = (size_t)(0.85 * n); for (i = 0; i < n; i++) a[i] = i < d ? -1e9 + 2e9 * (double)i / (double)d : -1e9 + 2e9 * (double)(i - d) / (double)(n - d); break; }  /* t5 sorted, overlapping tail */
    case 5: for (i = 0; i < n; i++) a[i] = (double)(i ^ (i >> 1)); break;                 /* f1 Gray code */
    case 6: for (i = 0; i < n; i++) a[i] = (double)rev23((uint32_t)i); break;             /* f2 bit reversal */
    case 7: { double x = 0; for (i = 0; i < n; i++) { x += gauss(); a[i] = x; } break; }  /* f3 Brownian walk */
    case 8: for (i = 0; i < n; i++) a[i] = (double)i + gauss() * 0.5 * (double)i; break; /* f4 noise growing with position */
    case 9: { for (i = 0; i < n; i++) a[i] = (double)i;                                   /* f5 shuffled blocks growing along the array */
              size_t b = 0, bs = 16; while (b < n) { size_t m = std::min(bs, n - b); for (size_t j = m - 1; j > 0; j--) { size_t r = nr() % (j + 1); std::swap(a[b + j], a[b + r]); } b += m; if (bs < 65536) bs += bs / 8 + 1; } break; }
    case 10: { for (i = 0; i < n; i++) a[i] = (double)i;                                  /* f6 2% swaps, all within the last third */
               size_t lo = 2 * n / 3; for (i = 0; i < n / 50; i++) { size_t x = lo + nr() % (n - lo), y = lo + nr() % (n - lo); std::swap(a[x], a[y]); } break; }
    case 11: { size_t next = 0, cur[8], left[8]; for (int s = 0; s < 8; s++) { left[s] = 0; cur[s] = 0; }   /* f7 ID blocks, fresh seed */
               for (i = 0; i < n; i++) { int s = 0; double u = fr() * 2.7178, acc = 0; for (s = 0; s < 7; s++) { acc += 1.0 / (s + 1); if (u < acc) break; }
                 if (left[s] == 0) { cur[s] = next; next += 1024; left[s] = 1024; } a[i] = (double)cur[s]++; left[s]--; } break; }
    case 12: for (i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;                      /* c1 uniform */
    case 13: for (i = 0; i < n; i++) a[i] = (double)i; break;                             /* c2 sorted */
    default: { size_t p = n / 8; for (i = 0; i < n; i++) a[i] = (double)(i % p); break; } /* c3 sawtooth, 8 teeth */
    }
}

int main(void) {
    const size_t n = 5000000;
    enum { TRUE_TAIL, FALSE_TAIL, CONTROL };
    struct { const char *name; int group; int want; } F[] = {
        { "prefix_80",        TRUE_TAIL,  Z_TAIL },     { "prefix_90",       TRUE_TAIL,  Z_TAIL },
        { "prefix_97",        TRUE_TAIL,  Z_TAIL },     { "narrow_tail",     TRUE_TAIL,  Z_TAIL },
        { "sorted_tail",      TRUE_TAIL,  Z_TAIL },     { "gray_code",       FALSE_TAIL, -1 },
        { "bit_reversal",     FALSE_TAIL, -1 },         { "brownian",        FALSE_TAIL, -1 },
        { "growing_noise",    FALSE_TAIL, -1 },         { "growing_blocks",  FALSE_TAIL, -1 },
        { "late_swaps",       FALSE_TAIL, -1 },         { "id_blocks",       FALSE_TAIL, -1 },
        { "uniform",          CONTROL,    Z_RANDOM },   { "sorted",          CONTROL,    Z_SORTED },
        { "sawtooth_8",       CONTROL,    Z_KOVERLAP },
    };
    const int NF = (int)(sizeof F / sizeof F[0]);
    std::vector<double> a(n);
    int tp1 = 0, tp2 = 0, fp1 = 0, fp2 = 0, c1 = 0, c2 = 0, nt = 0, nf = 0, nc = 0;
    printf("ztail: old vs tightened tail rule on FRESH shapes (n=%zu, new generators and seeds)\n\n", n);
    printf("  %-15s %-6s %-10s %-10s  o[1..7]\n", "shape", "group", "old rule", "new rule");
    for (int k = 0; k < NF; k++) {
        RS = 1000 + (uint64_t)k * 7919;
        fresh(k, a.data(), n);
        zprof_t p = zprofile(a.data(), n);
        int v1 = p.cls, v2 = zclass_v2(p);
        const char *g = F[k].group == TRUE_TAIL ? "tail" : F[k].group == FALSE_TAIL ? "fake" : "ctrl";
        printf("  %-15s %-6s %-10s %-10s ", F[k].name, g, ZN[v1], ZN[v2]);
        for (int l = 1; l <= 7; l++) printf(" %.2f", p.o[l]);
        printf("\n");
        if (F[k].group == TRUE_TAIL) {                 /* the sorted, overlapping tail is two runs: k-overlap is also a right answer */
            nt++;
            int ok1 = v1 == Z_TAIL || (k == 4 && v1 == Z_KOVERLAP), ok2 = v2 == Z_TAIL || (k == 4 && v2 == Z_KOVERLAP);
            tp1 += ok1; tp2 += ok2;
        } else if (F[k].group == FALSE_TAIL) { nf++; fp1 += v1 == Z_TAIL; fp2 += v2 == Z_TAIL; }
        else { nc++; c1 += v1 == F[k].want; c2 += v2 == F[k].want; }
    }
    printf("\n  %-40s %-10s %-10s %s\n", "", "old rule", "new rule", "predicted new");
    printf("  %-40s %d of %-5d %d of %-5d %s\n", "true tails caught", tp1, nt, tp2, nt, "3-4 of 5");
    printf("  %-40s %d of %-5d %d of %-5d %s\n", "lookalikes falsely called tail", fp1, nf, fp2, nf, "1-2 of 7");
    printf("  %-40s %d of %-5d %d of %-5d %s\n", "controls classified as expected", c1, nc, c2, nc, "3 of 3");
    return 0;
}
