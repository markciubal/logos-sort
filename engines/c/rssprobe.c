/* rssprobe: what a sort keeps RESIDENT, not what it asks for.
 *
 * make memprobe counts the bytes a sort ALLOCATES (peak heap, from a malloc hook). That is the right number for "can this sort run
 * in a container with N MB", and the wrong one for "how much memory does this process hold": a large malloc is an mmap, and a page
 * nobody touches costs nothing until it is touched. The one-thread path asks for 15.5 bytes per element (an 8-byte buffer and a
 * context of 6 bytes per element of 1.25x capacity) whatever route sorts the array, and the AVX2 quicksort, which sorts unstructured
 * data in place, touches none of it. The pools then keep what WAS touched for the next call, forever, unless the caller releases.
 *
 * Every row runs in a forked child, so each starts cold:
 *   peak      the resident high-water mark during ONE sort, above the input array (VmHWM after /proc/self/clear_refs "5")
 *   held      resident after that sort returns (what the pools kept)
 *   small     resident after 32 further sorts of n/64 elements (the same thread count): what a long-running process keeps after one
 *             big call, which is what a pool trim policy is for
 *   of_lazy   how much of `small` is LazyFree (pages handed back with MADV_FREE: still mapped, reclaimed only under memory pressure)
 *   released  resident after logos_release_buffers()
 * All in bytes per element of the big sort, against the array's own 8. Every sort is checked ascending.
 *
 * usage: rssprobe [n] [threads list, comma separated] [shape filter]      e.g. rssprobe 8388608 1,14 uniform */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include "logos_psort.h"

static uint64_t RS = 88172645463325252ULL;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }

enum { S_UNIFORM, S_HEAVY, S_QUANT, S_FEW, S_NEAR, S_BLOCKS, S_SORTED, S_INTWIDE,
       S_RUNS16, S_HALVES, S_HEAD5, S_STRANDS4, S_COARSE, S_STAIRS, S_TRI,             /* 2026-10-22: the routes of rounds 3bn and 3bo */
       S_DOM90, S_DOM99, S_I32WIDE, S_I32SPAN, S_DOM25, S_DOM50, S_SINE, S_N };    /* one value holding 90% / 99%; the int32 API (bytes per 4-byte element) */
static const char *SN[S_N] = { "uniform", "heavy_tail", "quantized", "few_distinct", "near_sorted", "block_perm", "sorted", "int_wide",
                               "runs16", "two_halves", "head_5", "strands4", "coarse64k", "stairs_x20", "triangle1024",
                               "dominant_90", "dominant_99", "i32_wide", "i32_span1000", "dominant_25", "dominant_50", "sine_wave" };

static void gen(double *a, size_t n, int s) {
    for (size_t i = 0; i < n; i++) {
        switch (s) {
        case S_UNIFORM: a[i] = fr() * 2e9 - 1e9; break;
        case S_HEAVY:   a[i] = exp(fr() * 40.0 - 20.0) * (nr() & 1 ? 1 : -1); break;
        case S_QUANT:   a[i] = (double)(nr() % 30000) * 0.25; break;
        case S_FEW:     a[i] = (double)(nr() % 5) * 1e6; break;
        case S_INTWIDE: a[i] = (double)(int32_t)(uint32_t)nr(); break;
        case S_DOM90:   a[i] = (nr() % 10) ? 12345.0 : fr() * 2e9 - 1e9; break;
        case S_DOM99:   a[i] = (nr() % 100) ? 12345.0 : fr() * 2e9 - 1e9; break;
        case S_DOM25:   a[i] = (nr() % 4) == 0 ? 12345.0 : fr() * 2e9 - 1e9; break;
        case S_DOM50:   a[i] = (nr() & 1) ? 12345.0 : fr() * 2e9 - 1e9; break;
        case S_RUNS16:  a[i] = (double)(((uint64_t)(i / 16) * 2654435761u) % 1000003u) + 0.25; break;          /* the run-length route */
        case S_HALVES:  a[i] = i < n / 2 ? (double)i * 2.0 + 0.25 : (double)(i - n / 2) * 2.0 + 1.25; break;    /* the tail route's vector merge */
        case S_HEAD5:   a[i] = i < n / 20 ? fr() * (double)n : (double)i + 0.5; break;                          /* the head route */
        case S_STRANDS4: { size_t L = n / 4 + 1; a[i] = (double)(i % L) * 4.0 + (double)(i / L) + 0.25; } break; /* the k-way merge in vectors */
        case S_COARSE:  a[i] = (double)(i / 65536) * 65536.0 + fr() * 65536.0; break;                           /* the cuts route */
        case S_STAIRS:  { size_t L = n / 20 + 1, b = i / L; a[i] = (double)b * (double)L * 0.9 + (double)(i - b * L) + 0.25; } break;   /* the knot route */
        case S_TRI:     { size_t ph = i % 1024; a[i] = (double)(ph < 512 ? ph : 1024 - ph) + (double)(i / 1024) / (double)(n / 1024 + 2) + (ph < 512 ? 0.0 : 0.25 / (double)(n / 1024 + 2)); } break;
        case S_SINE:    { double T = (double)n / 32.0, A = 0.6 * T / 2.0; a[i] = (double)i + A * sin(6.283185307179586 * (double)i / T) + (fr() * 2.0 - 1.0) * 50.0; } break;   /* the wave route (2026-10-24) */
        default:        a[i] = (double)i + 0.5; break;
        }
    }
    if (s == S_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t i = nr() % n, j = nr() % n; double t = a[i]; a[i] = a[j]; a[j] = t; }
    if (s == S_BLOCKS) {   /* 64 sorted blocks in shuffled order, swapped in place block by block (no second array: it would be resident) */
        size_t nb = 64, blk = n / nb, perm[64];
        for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1), x = perm[k]; perm[k] = perm[r]; perm[r] = x; }
        for (size_t k = 0; k < nb; k++) for (size_t i = 0; i < blk; i++) a[k * blk + i] = (double)(perm[k] * blk + i) + 0.5;
    }
}
static long kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r"); if (!f) return -1;
    char line[256]; long v = -1; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl)) { v = atol(line + kl); break; }
    fclose(f); return v;
}
static long lazy_kb(void) {   /* MADV_FREE'd pages still mapped: counted in VmRSS, reclaimed by the kernel only under pressure */
    FILE *f = fopen("/proc/self/smaps_rollup", "r"); if (!f) return 0;
    char line[256]; long v = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "LazyFree:", 9)) { v = atol(line + 9); break; }
    fclose(f); return v;
}
static int reset_hwm(void) { FILE *f = fopen("/proc/self/clear_refs", "w"); if (!f) return 0; int ok = fputs("5", f) >= 0; fclose(f); return ok; }
static int ascending(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) return 0; return 1; }

#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }
/* rssprobe time [n] [P] [pairs] [shape]: what the pool trim COSTS. A large sort, then K sorts of n/64, then the large sort again,
 * timed; LOGOS_TRIM=1 (or RSS_TRIM_FORM=hard) against the trim off, alternating which goes first, median of the ratio. The switch is read by the
 * first large call of each side, so the small calls between run with it. K = 2 must read 1.00 (the trim cannot fire in two calls). */
static const char *g_trim_form = "1";
static int time_mode(int argc, char **argv) {
    if (getenv("RSS_TRIM_FORM")) g_trim_form = getenv("RSS_TRIM_FORM");      /* "1" (MADV_FREE) or "hard" (MADV_DONTNEED) */
    size_t n = argc > 2 ? strtoull(argv[2], NULL, 10) : 8388608;
    int P = argc > 3 ? atoi(argv[3]) : 1, pairs = argc > 4 ? atoi(argv[4]) : 20;
    const char *filt = argc > 5 ? argv[5] : "";
    size_t n2 = n / 64;
    double *orig = malloc(n * sizeof(double)), *a = malloc(n * sizeof(double)), *b = malloc(n2 * sizeof(double)), *r = malloc(pairs * sizeof(double));
    if (!orig || !a || !b || !r) return 2;
    printf("# rssprobe time: n = %zu, %d thread(s), %d pairs; ratio = LOGOS_TRIM=%s / off for the large sort after K small ones (> 1: the trim costs)\n", n, P, pairs, g_trim_form);
    const int Ks[] = { 2, 8, 32 };
    for (int s = 0; s < S_N; s++) {
        if (*filt ? !strstr(SN[s], filt) : !(s == S_UNIFORM || s == S_QUANT || s == S_NEAR || s == S_BLOCKS || s == S_INTWIDE)) continue;
        RS = 88172645463325252ULL + (uint64_t)s * 7919; gen(orig, n, s);
        printf("  %-13s", SN[s]);
        for (int ki = 0; ki < 3; ki++) {
            int K = Ks[ki]; size_t trimmed = 0;
            for (int p = 0; p < pairs; p++) {
                double t[2];
                for (int side = 0; side < 2; side++) {
                    int on = (side ^ (p & 1)) == 0;
                    if (on) setenv("LOGOS_TRIM", g_trim_form, 1); else unsetenv("LOGOS_TRIM");
                    memcpy(a, orig, n * sizeof(double)); logos_sort_f64(a, n, P);
                    (void)logos_twist_last(21);
                    for (int k = 0; k < K; k++) { RS += 13; gen(b, n2, S_UNIFORM); logos_sort_f64(b, n2, P); }
                    if (on) trimmed += logos_twist_last(21);
                    memcpy(a, orig, n * sizeof(double));
                    double t0 = now(); logos_sort_f64(a, n, P); t[on] = now() - t0;
                    if (!ascending(a, n)) { printf(" WRONG OUTPUT\n"); return 3; }
                }
                r[p] = t[1] / t[0];
            }
            qsort(r, pairs, sizeof(double), cmpd);
            printf("   K=%-2d %.3f [%.3f, %.3f] %6.1f MB/pair", K, r[pairs / 2], r[pairs / 10], r[pairs - 1 - pairs / 10], trimmed / 1048576.0 / pairs);
        }
        printf("\n");
    }
    unsetenv("LOGOS_TRIM");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "time")) return time_mode(argc, argv);
    size_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 8388608;
    const char *tl = argc > 2 ? argv[2] : "1,14";
    const char *filt = argc > 3 ? argv[3] : "";
    int th[16], nt = 0;
    for (const char *p = tl; *p && nt < 16; ) { th[nt++] = atoi(p); const char *c = strchr(p, ','); if (!c) break; p = c + 1; }
    printf("# rssprobe: n = %zu; resident BYTES PER ELEMENT above the input array (which is 8); every row a fresh process\n", n);
    printf("# %-13s %3s  %8s %8s %8s %8s %9s\n", "shape", "P", "peak", "held", "small", "of_lazy", "released");
    for (int s = 0; s < S_N; s++) {
        if (*filt && !strstr(SN[s], filt)) continue;
        for (int t = 0; t < nt; t++) {
            int pfd[2]; if (pipe(pfd)) return 1;
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                RS = 88172645463325252ULL + (uint64_t)s * 7919;
                int i32 = s == S_I32WIDE || s == S_I32SPAN;
                double *a = i32 ? NULL : malloc(n * sizeof(double)); size_t n2 = n / 64; double *b = malloc(n2 * sizeof(double));
                int32_t *ai = i32 ? malloc(n * sizeof(int32_t)) : NULL;
                if ((!a && !ai) || !b) _exit(2);
                if (i32) for (size_t i = 0; i < n; i++) ai[i] = s == S_I32WIDE ? (int32_t)(uint32_t)nr() : (int32_t)(nr() % 1000);
                else gen(a, n, s);
                gen(b, n2, S_UNIFORM);
                long r0 = kb("VmRSS:");
                int hw = reset_hwm();
                if (i32) logos_sort_i32(ai, n, th[t]); else logos_sort_f64(a, n, th[t]);
                long hwm = kb("VmHWM:"), r1 = kb("VmRSS:");
                int ok = 1;
                if (i32) { for (size_t i = 1; i < n; i++) if (ai[i] < ai[i - 1]) { ok = 0; break; } } else ok = ascending(a, n);
                for (int k = 0; k < 32; k++) { RS += 13; gen(b, n2, S_UNIFORM); logos_sort_f64(b, n2, th[t]); ok = ok && ascending(b, n2); }
                long r2 = kb("VmRSS:"), lz = lazy_kb();
                logos_release_buffers();
                long r3 = kb("VmRSS:");
                char msg[256];
                double f = 1024.0 / (double)n;                             /* bytes per element: the array is 8 bytes each, 4 on the int32 rows */
                snprintf(msg, sizeof msg, "  %-13s %3d  %8.2f %8.2f %8.2f %8.2f %9.2f%s%s\n", SN[s], th[t], hw ? (hwm - r0) * f : NAN, (r1 - r0) * f,
                         (r2 - r0) * f, lz * f, (r3 - r0) * f, ok ? "" : "  WRONG OUTPUT", hw ? "" : "  (no clear_refs: peak unknown)");
                if (write(pfd[1], msg, strlen(msg)) < 0) _exit(3);
                _exit(ok ? 0 : 4);
            }
            close(pfd[1]);
            char buf[512]; ssize_t k = read(pfd[0], buf, sizeof buf - 1); close(pfd[0]);
            int st = 0; waitpid(pid, &st, 0);
            if (k > 0) { buf[k] = 0; fputs(buf, stdout); } else printf("  %-13s %3d  child failed (status %d)\n", SN[s], th[t], st);
        }
    }
    return 0;
}
