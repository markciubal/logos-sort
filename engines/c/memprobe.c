/* memprobe.c — how much memory does one sort take? Peak TRACED HEAP of a single cold call, per shape, size, thread
 * count and entry point, in bytes per element.
 *
 * Why it exists. An outside review of a sibling line of this engine (2026-09-19) measured the buffered sort at 22 bytes
 * per element where the documentation said "~2n", and warned that per-thread scratch "may approach O(Pn) under
 * pessimistic parallel allocation". Neither had ever been measured in this tree: every bench here times, none weighs.
 *
 * Method. malloc, calloc, realloc, posix_memalign and free are interposed (the engine allocates through nothing else)
 * and the live and peak byte counts are kept with malloc_usable_size, so the number is what the allocator actually
 * handed out, not what was asked for. Every measurement runs in a FRESH CHILD PROCESS: the engine keeps a buffer pool
 * and a context pool between calls, so only a cold call shows the footprint, and a fork from a single-threaded parent
 * is safe. The input array itself (8 bytes per element) is allocated before the trace starts and reported separately.
 *
 * PREDICTIONS, written before the first run (14 threads, 10M):
 *   buffered, 1 thread, random shapes: 8 (scratch) + 6 (the sequential context: 4-byte aux + 2-byte idx) = ~14 aux;
 *   buffered, 14 threads, random shapes: 8 (scratch) + 1 (bucket-index cache) + contexts sized by the largest of
 *     168 buckets, 14 * (n/168 * 1.25) * 6 bytes = ~0.6, so ~9.6 aux;
 *   counting routes (few_distinct, quantized): far less, no n-sized scratch at all;
 *   in-place entry: 0 aux;
 *   dominant (90% one value, the rest distinct): THE TEST OF THE O(Pn) WARNING. If every thread's context is sized by the
 *     largest bucket and one bucket holds 0.9n, 14 threads would take 14 * 0.9 * 6 = ~76 bytes per element.
 *
 * Usage: ./memprobe [n ...]          default 1000000 10000000.   LOGOS_THREADS=N overrides the parallel count. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- the tracer ---- */
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void __libc_free(void *);
static _Atomic int mp_active;
static _Atomic size_t mp_live, mp_peak, mp_calls;
static size_t mp_log_min;                       /* MEMPROBE_LOG=<bytes>: print every traced allocation at least this large, with its caller */
static void mp_add_from(void *p, void *caller) {
    if (!p || !atomic_load_explicit(&mp_active, memory_order_relaxed)) return;
    size_t z = malloc_usable_size(p), v = atomic_fetch_add_explicit(&mp_live, z, memory_order_relaxed) + z;
    if (mp_log_min && z >= mp_log_min) { char b[96]; int k = snprintf(b, sizeof b, "  alloc %11zu bytes  caller %p\n", z, caller); if (k > 0 && write(2, b, (size_t)k) < 0) {} }
    atomic_fetch_add_explicit(&mp_calls, 1, memory_order_relaxed);
    size_t old = atomic_load_explicit(&mp_peak, memory_order_relaxed);
    while (v > old && !atomic_compare_exchange_weak_explicit(&mp_peak, &old, v, memory_order_relaxed, memory_order_relaxed)) {}
}
static void mp_sub(void *p) { if (p && atomic_load_explicit(&mp_active, memory_order_relaxed)) atomic_fetch_sub_explicit(&mp_live, malloc_usable_size(p), memory_order_relaxed); }
#define mp_add(p) mp_add_from((p), __builtin_return_address(0))
void *malloc(size_t n) { void *p = __libc_malloc(n); mp_add(p); return p; }
void *calloc(size_t n, size_t z) { void *p = __libc_calloc(n, z); mp_add(p); return p; }
void *realloc(void *p, size_t n) { mp_sub(p); void *q = __libc_realloc(p, n); if (q) mp_add(q); else if (n) mp_add(p); return q; }
int posix_memalign(void **out, size_t al, size_t n) { void *p = __libc_memalign(al, n); if (!p) return 12; mp_add(p); *out = p; return 0; }
void free(void *p) { mp_sub(p); __libc_free(p); }

/* ---- shapes ---- */
static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
enum { S_UNIFORM, S_HEAVY, S_FEWD, S_QUANT, S_NEAR, S_BLOCK, S_STAIRS, S_REACH, S_REACH1, S_DOM90, S_DOM99, S_N };
static const char *SN[S_N] = { "uniform", "heavy_tail", "few_distinct", "quantized", "near_sorted", "block_perm", "stairs_x20", "short_reach", "short_reach+1", "dominant_90", "dominant_99" };
static void gen(double *a, size_t n, int s) {
    RS = 7; size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    for (size_t i = 0; i < n; i++) switch (s) {
        case S_UNIFORM: a[i] = fr() * 2e9 - 1e9; break;
        case S_HEAVY:   a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2); break;
        case S_FEWD:    a[i] = (double)(nr() % 5) * 1e6; break;
        case S_QUANT:   a[i] = (double)(nr() % 4096) / 64.0; break;
        case S_NEAR:    a[i] = (double)i + fr() * 0.5; break;
        case S_BLOCK:   a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5; break;
        case S_STAIRS:  a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + 1.0 / 9.0) + fr() * 0.5; break;
        /* every element within 1000 places of home: index blocks are sorted where they lie, no n-sized buffer is asked for.
           short_reach+1 has ONE far element, which goes back in by an in-place shift (three small lists, no buffer either) */
        case S_REACH: case S_REACH1: a[i] = (double)i + fr() * 2000.0 - 1000.0; break;
        /* one value holds most of the array and the rest are all distinct: too many distinct values for a counting route, and
           one bucket that no splitter can divide */
        case S_DOM90:   a[i] = (nr() % 10) ? 12345.0 : fr() * 2e9 - 1e9; break;
        default:        a[i] = (nr() % 100) ? 12345.0 : fr() * 2e9 - 1e9; break;
    }
    if (s == S_REACH1) a[n / 3] = 4.0 * (double)n;
    if (s == S_NEAR) for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
}

/* one cold sort in a child: peak traced heap, what the engine still holds when the sort returns (its pools), and what is
 * left after logos_release_buffers(). r[0] == (size_t)-1 on failure. */
typedef struct { size_t peak, held, after; int ok; } mp_res_t;
static mp_res_t measure(int shape, size_t n, int threads, int inplace) {
    mp_res_t R = { (size_t)-1, 0, 0, 0 };
    int fd[2]; if (pipe(fd)) return R;
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        double *a = (double *)__libc_malloc(n * sizeof(double));          /* the input: outside the trace */
        size_t out[4] = { (size_t)-1, 0, 0, 0 };
        if (a) {
            gen(a, n, shape);
            { const char *lg = getenv("MEMPROBE_LOG"); mp_log_min = lg ? (size_t)atof(lg) : 0; }
            atomic_store(&mp_live, 0); atomic_store(&mp_peak, 0); atomic_store(&mp_active, 1);
            if (inplace) logos_sort_f64_inplace(a, n, threads); else logos_sort_f64(a, n, threads);
            out[0] = atomic_load(&mp_peak); out[1] = atomic_load(&mp_live);
            logos_release_buffers();
            out[2] = atomic_load(&mp_live);
            atomic_store(&mp_active, 0);
            out[3] = 1;
            for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { out[3] = 0; break; }
        }
        if (write(fd[1], out, sizeof out) != (ssize_t)sizeof out) _exit(2);
        _exit(0);
    }
    close(fd[1]);
    size_t out[4] = { (size_t)-1, 0, 0, 0 };
    if (read(fd[0], out, sizeof out) != (ssize_t)sizeof out) out[0] = (size_t)-1;
    close(fd[0]); int st = 0; waitpid(pid, &st, 0);
    R.peak = out[0]; R.held = out[1]; R.after = out[2]; R.ok = (int)out[3];
    return R;
}

/* ---- the int32 entry: the same three numbers, per 4-byte element ---- */
enum { I_WIDE, I_SPAN1000, I_DOM90, I_N };
static const char *IN[I_N] = { "int_wide", "int_span_1000", "int_dominant_90" };
static void gen32(int32_t *a, size_t n, int s) {
    RS = 7;
    for (size_t i = 0; i < n; i++) a[i] = s == I_WIDE ? (int32_t)nr() : s == I_SPAN1000 ? (int32_t)(nr() % 1000) : ((nr() % 10) ? 12345 : (int32_t)nr());
}
static mp_res_t measure32(int shape, size_t n, int threads, int inplace) {
    mp_res_t R = { (size_t)-1, 0, 0, 0 };
    int fd[2]; if (pipe(fd)) return R;
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        int32_t *a = (int32_t *)__libc_malloc(n * sizeof(int32_t));
        size_t out[4] = { (size_t)-1, 0, 0, 0 };
        if (a) {
            gen32(a, n, shape);
            { const char *lg = getenv("MEMPROBE_LOG"); mp_log_min = lg ? (size_t)atof(lg) : 0; }
            atomic_store(&mp_live, 0); atomic_store(&mp_peak, 0); atomic_store(&mp_active, 1);
            if (inplace) logos_sort_i32_inplace(a, n, threads); else logos_sort_i32(a, n, threads);
            out[0] = atomic_load(&mp_peak); out[1] = atomic_load(&mp_live);
            logos_release_buffers();
            out[2] = atomic_load(&mp_live);
            atomic_store(&mp_active, 0);
            out[3] = 1;
            for (size_t i = 1; i < n; i++) if (a[i] < a[i - 1]) { out[3] = 0; break; }
        }
        if (write(fd[1], out, sizeof out) != (ssize_t)sizeof out) _exit(2);
        _exit(0);
    }
    close(fd[1]);
    size_t out[4] = { (size_t)-1, 0, 0, 0 };
    if (read(fd[0], out, sizeof out) != (ssize_t)sizeof out) out[0] = (size_t)-1;
    close(fd[0]); int st = 0; waitpid(pid, &st, 0);
    R.peak = out[0]; R.held = out[1]; R.after = out[2]; R.ok = (int)out[3];
    return R;
}

int main(int argc, char **argv) {
    int P = bench_threads(logos_num_cpus());
    size_t sizes[8]; int ns = 0;
    for (int i = 1; i < argc && ns < 8; i++) { double v = atof(argv[i]); if (v >= 1) sizes[ns++] = (size_t)v; }
    if (!ns) { sizes[ns++] = 1000000; sizes[ns++] = 10000000; }
    printf("traced heap of ONE cold sort, bytes per element, EXCLUDING the 8-byte input (add 8 for the total)\n");
    printf("peak = the most held at any moment; held = still allocated when the sort returns (the pools);\n");
    printf("after = left once logos_release_buffers() has been called. Fresh child process per cell; threads 1 and %d\n\n", P);
    for (int si = 0; si < ns; si++) {
        size_t n = sizes[si];
        printf("n = %zu\n  %-13s | %8s %8s %8s | %8s %8s %8s | %8s %8s\n", n, "", "1T peak", "held", "after", "PT peak", "held", "after", "inpl 1T", "inpl PT");
        for (int s = 0; s < S_N; s++) {
            mp_res_t a1 = measure(s, n, 1, 0), ap = measure(s, n, P, 0), ai = measure(s, n, 1, 1), aq = measure(s, n, P, 1);
            double d = (double)n;
            printf("  %-13s | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f%s\n", SN[s], (double)a1.peak / d, (double)a1.held / d, (double)a1.after / d,
                   (double)ap.peak / d, (double)ap.held / d, (double)ap.after / d, (double)ai.peak / d, (double)aq.peak / d, (a1.ok && ap.ok && ai.ok && aq.ok) ? "" : "   <-- OUTPUT NOT SORTED");
            fflush(stdout);
        }
        printf("\n  int32, per 4-byte element (add 4 for the total)\n");
        for (int s = 0; s < I_N; s++) {
            mp_res_t a1 = measure32(s, n, 1, 0), ap = measure32(s, n, P, 0), ai = measure32(s, n, 1, 1), aq = measure32(s, n, P, 1);
            double d = (double)n;
            printf("  %-13s | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f%s\n", IN[s], (double)a1.peak / d, (double)a1.held / d, (double)a1.after / d,
                   (double)ap.peak / d, (double)ap.held / d, (double)ap.after / d, (double)ai.peak / d, (double)aq.peak / d, (a1.ok && ap.ok && ai.ok && aq.ok) ? "" : "   <-- OUTPUT NOT SORTED");
            fflush(stdout);
        }
        printf("\n");
    }
    (void)bench_paired; (void)bench_threads_source;
    return 0;
}
