/* ztrain.cpp — integrity check for zroute.cpp: does the classifier AS CODED reproduce the training set?
 *
 * zroute's rule was fixed from the earlier probe (zprobe.c) by reading its table by hand. If the code
 * disagrees with that hand reading, the held-out zoo result is meaningless, so this regenerates the
 * probe's shapes with the same generator and seeds and asserts the class each should get. No timing:
 * pure computation, so it needs no bench lock.
 *
 * Run: make ztrain && ./ztrain                                                                     */
#define ZROUTE_MAIN zroute_unused_main
#include "zroute.cpp"
#undef main

static void tgen(const char *s, double *a, size_t n) {
    size_t i;
    if (!strcmp(s, "sorted"))        for (i = 0; i < n; i++) a[i] = (double)i;
    else if (!strcmp(s, "reverse"))  for (i = 0; i < n; i++) a[i] = (double)(n - i);
    else if (!strcmp(s, "near_1pct")) { for (i = 0; i < n; i++) a[i] = (double)i; for (i = 0; i < n / 100; i++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; } }
    else if (!strcmp(s, "window_64")) { for (i = 0; i < n; i++) a[i] = (double)i; for (size_t b = 0; b + 64 <= n; b += 64) for (size_t j = 63; j > 0; j--) { size_t k = nr() % (j + 1); double t = a[b + j]; a[b + j] = a[b + k]; a[b + k] = t; } }
    else if (!strcmp(s, "blocks_64")) { size_t nb = 64, bs = n / nb; std::vector<size_t> ord(nb); for (i = 0; i < nb; i++) ord[i] = i; for (i = nb - 1; i > 0; i--) { size_t k = nr() % (i + 1); std::swap(ord[i], ord[k]); } for (i = 0; i < n; i++) a[i] = (double)(ord[(i / bs) % nb] * bs + i % bs); }
    else if (!strcmp(s, "interleave_4")) { for (i = 0; i < n; i++) a[i] = (double)(i / 4) * 4.0 + (double)((i * 7919u) % 4); }
    else if (!strcmp(s, "sawtooth_16")) { size_t p = n / 16; for (i = 0; i < n; i++) a[i] = (double)(i % p); }
    else if (!strcmp(s, "sorted+tail")) { size_t t = n / 20; for (i = 0; i < n - t; i++) a[i] = (double)i; for (; i < n; i++) a[i] = fr() * (double)n; }
    else                              for (i = 0; i < n; i++) a[i] = fr();
}

int main(void) {
    const size_t n = (size_t)1 << 22;
    struct { const char *name; int want; int train; } T[] = {     /* train=1: the six the rule was fitted to */
        { "sorted",       Z_SORTED,   1 }, { "reverse",      Z_SORTED,   0 }, { "near_1pct",    Z_BOUNDED,  1 },
        { "window_64",    Z_SORTED,   0 }, { "blocks_64",    Z_BOUNDED,  1 }, { "interleave_4", Z_SORTED,   0 },
        { "sawtooth_16",  Z_KOVERLAP, 1 }, { "sorted+tail",  Z_TAIL,     1 }, { "uniform",      Z_RANDOM,   1 },
    };
    std::vector<double> a(n); int bad = 0, trained = 0;
    printf("ztrain: the classifier as coded, on the probe's own shapes (n=2^22, same seeds)\n\n");
    for (size_t si = 0; si < sizeof T / sizeof T[0]; si++) {
        RS = 7 + si; tgen(T[si].name, a.data(), n);
        zprof_t p = zprofile(a.data(), n);
        int ok = p.cls == T[si].want;
        if (T[si].train) { trained++; if (!ok) bad++; }
        printf("  %-13s %-10s want %-10s %s%s   o:", T[si].name, ZN[p.cls], ZN[T[si].want], ok ? "ok  " : "MISS", T[si].train ? " (train)" : "        ");
        for (int l = 1; l <= 7; l++) printf(" %.2f", p.o[l]);
        if (p.cls == Z_KOVERLAP) printf("  k=%.1f", p.k);
        if (p.desc) printf("  desc");
        printf("\n");
    }
    printf("\n  training shapes misclassified: %d of %d  -> %s\n", bad, trained, bad ? "THE RULE AS CODED DOES NOT MATCH ITS FITTING; the zoo result is invalid" : "rule as coded matches its fitting");
    return bad ? 1 : 0;
}
