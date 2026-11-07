/* faultprobe.c -- what happens when an allocation, or a thread creation, fails in the middle of a sort?
 *
 * An outside review (2026-09-19) listed "no fault-injection testing validates allocation fallback" among the reasons a
 * correctness claim was not yet justified, and nothing here had ever tested it: on Linux malloc all but never returns
 * NULL, so every fallback in the engine was written and never run. On a platform where it does (a WebAssembly heap, a
 * container with a memory limit) the difference between "falls back" and "returns an unsorted array" is the whole
 * contract. Thread creation is the likelier failure on Linux: a container with a pid limit makes pthread_create return
 * EAGAIN while malloc still succeeds.
 *
 * Method. malloc, calloc, realloc, posix_memalign and pthread_create are interposed. Only calls made from this
 * executable's own text are counted and failed (the engine is linked in statically), so a failure injected here is one
 * the ENGINE has to answer for; glibc's internal allocations are left alone. A first run counts the engine's allocations
 * A and thread creations C in one cold call; then the call is repeated, run k failing its k-th call (mode `one`) or every
 * call from the k-th on (mode `rest`, which is what running out of the resource looks like). Every run is a fresh child
 * process forked from a parent that holds the input (so the pools are cold, and a crash or a hang is a result, not the
 * end of the probe). A run passes when the output ascends under '<' and an order-independent hash of the bit patterns
 * equals the input's. The verdict allocates nothing while the fault is armed.
 *
 * Six entries are driven: the f64 sort, the int32 sort, the lazy view's two halves (partition, then sort every
 * bucket) for both types, and the two strict in-place sorts. The summary names the worker functions whose creation was failed and counts the distinct
 * allocation sites that were failed, so what the sweep did NOT reach is visible (needs addr2line and a -g build).
 *
 * Usage: ./faultprobe [n] [threads] [filter]               the sweep; default 2100000 elements, 1 and 4 threads, every case;
 *                                                          filter = part of a shape or entry name (heavy_tail, inplace, view)
 *        ./faultprobe -c <case> <threads> <mode> <k> [n]   one case, printing each failed call site to stderr
 *                                                          mode: one | rest | spawn-one | spawn-rest
 *        ./faultprobe -l                                   list the cases
 * Exit status 1 when any run returned wrong output, crashed or hung. */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern char __executable_start[], etext[];
typedef int (*create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static create_fn real_create;

static _Atomic int fp_armed;
static _Atomic long fp_allocs, fp_spawns;
static _Atomic uintptr_t fp_site;                     /* the first failed call: return address (allocation) or worker function (creation) */
static long fp_target;                                /* which call fails; 0 = none (count only) */
static int fp_kind, fp_rest, fp_trace;                /* kind 0 allocation, 1 thread creation; rest: every call from the target on */

static void fp_say(const char *what, uintptr_t off) { /* no allocation, no stdio: this runs inside malloc */
    char b[64]; int k = 0; while (what[k]) { b[k] = what[k]; k++; }
    b[k++] = ' '; b[k++] = '0'; b[k++] = 'x';
    for (int s = 28; s >= 0; s -= 4) b[k++] = "0123456789abcdef"[(off >> s) & 15];
    b[k++] = '\n'; if (write(2, b, (size_t)k) < 0) { }
}
static int fp_mine(void *ra) { return (char *)ra >= __executable_start && (char *)ra < etext; }
static int fp_fail(int kind, void *ra, void *site) {
    if (!atomic_load_explicit(&fp_armed, memory_order_relaxed) || !fp_mine(ra)) return 0;
    long k = atomic_fetch_add_explicit(kind ? &fp_spawns : &fp_allocs, 1, memory_order_relaxed) + 1;
    int f = kind == fp_kind && fp_target > 0 && (fp_rest ? k >= fp_target : k == fp_target);
    if (f) {
        uintptr_t off = (uintptr_t)((char *)site - __executable_start), zero = 0;
        atomic_compare_exchange_strong(&fp_site, &zero, off);
        if (fp_trace) fp_say(kind ? "fail spawn of worker" : "fail alloc at", off);
    }
    return f;
}
#define RA __builtin_return_address(0)
void *malloc(size_t n) { if (fp_fail(0, RA, RA)) { errno = ENOMEM; return NULL; } return __libc_malloc(n); }
void *calloc(size_t n, size_t z) { if (fp_fail(0, RA, RA)) { errno = ENOMEM; return NULL; } return __libc_calloc(n, z); }
void *realloc(void *p, size_t n) { if (fp_fail(0, RA, RA)) { errno = ENOMEM; return NULL; } return __libc_realloc(p, n); }
int posix_memalign(void **out, size_t al, size_t n) {
    if (fp_fail(0, RA, RA)) return ENOMEM;
    void *p = __libc_memalign(al, n); if (!p) return ENOMEM;
    *out = p; return 0;
}
int pthread_create(pthread_t *th, const pthread_attr_t *at, void *(*fn)(void *), void *arg) {
    if (fp_fail(1, RA, (void *)(uintptr_t)fn)) return EAGAIN;
    return real_create(th, at, fn, arg);
}

/* ---- inputs ---- */
static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr(void) { return (double)(nr() >> 11) / (double)(1ULL << 53); }
enum { E_F64, E_I32, E_VIEW64, E_VIEW32, E_INPLACE64, E_INPLACE32, E_BATCH64, E_IPP64, E_STR };
static const char *EN[] = { "sort f64", "sort i32", "view f64", "view i32", "inplace f64", "inplace i32", "batch f64", "ipart f64", "strings" };
typedef struct { const char *name; int entry; } case_t;
static const case_t CASES[] = {
    { "uniform", E_F64 }, { "heavy_tail", E_F64 }, { "few_distinct", E_F64 }, { "quantized", E_F64 }, { "mod_card_2k", E_F64 },
    { "mod_card_30k", E_F64 }, { "near_sorted", E_F64 }, { "block_perm", E_F64 }, { "stairs_x20", E_F64 }, { "stairs_triple", E_F64 },
    { "block_twist", E_F64 }, { "stairs_twist", E_F64 },
    { "mirror_reach", E_F64 }, { "mirror_prefix", E_F64 }, { "mirror_streams", E_F64 }, { "block_kinked", E_F64 }, { "dense_spread", E_F64 }, { "sparse_over", E_F64 },   /* the 16n bitmap budget; the galloping merge */   /* the whole array's orientation bit: buffers released, one reversal, the dispatcher entered again */        /* the twist: blocks lying descending, consumed as they lie */
    { "ring_buffer", E_F64 }, { "ring_prefix", E_F64 }, { "blocks_1000", E_F64 },
    { "blocks_strays", E_F64 }, { "sine_wave", E_F64 }, { "ring_reach", E_F64 }, { "one_kinked", E_F64 }, { "one_strays", E_F64 },   /* one strand: finished without a plan */   /* round six: strays lifted and the holes closed (a failure after that must put them back), the wave route, the seam */   /* the loop-back: scratch from the pool or malloc, three reversals without; the floor: 2,100 runs at one thread */
    { "dominant_90", E_F64 }, { "few_runs", E_F64 }, { "prefix_95", E_F64 }, { "many_runs", E_F64 }, { "short_reach", E_F64 }, { "short_reach_stray", E_F64 },
    { "stairs_order", E_F64 },                                       /* 2026-11-06: 64 runs IN ORDER, overlapping: the near-sorted strand gate's sample (sample_by_pos) fails here */
    { "int_wide", E_I32 }, { "int_span_1000", E_I32 }, { "int_sentinel", E_I32 },
    { "uniform", E_VIEW64 }, { "dominant_90", E_VIEW64 }, { "int_wide", E_VIEW32 },
    { "uniform", E_INPLACE64 }, { "dominant_90", E_INPLACE64 }, { "heavy_tail", E_INPLACE64 }, { "int_wide", E_INPLACE32 },
    { "uniform", E_BATCH64 }, { "near_sorted", E_BATCH64 },        /* 2026-10-08: the batch entry (segments on both sides of 128 and 65,536) */
    { "uniform", E_IPP64 }, { "heavy_tail", E_IPP64 }, { "dominant_90", E_IPP64 },   /* the in-place partition (LOGOS_PART_INPLACE=1) */
    { "urls", E_STR }, { "uuids", E_STR },                          /* 2026-10-09: strings on prefix keys (logos_sort_strings_mt) */
    { "loglines", E_STR },                                          /* 2026-10-19: near-ordered lines, the log-line route (one thread) */
    { "runs16", E_F64 }, { "head_5", E_F64 }, { "windows_64k", E_F64 }, { "halves", E_F64 },   /* 2026-10-20: the run-length, head, cuts routes; the tail's vector merge */
    { "strands4", E_F64 }, { "tri1024", E_F64 },
    { "lattice_spill", E_F64 },   /* 2026-10-31: the one-thread lattice entry with spills (mod_card_30k reaches it with none) */
    { "dominant_50", E_F64 },   /* 2026-10-22: the dominant-value route (its two inner sorts; its own buffer is an mmap, failed by the harness instead) */   /* 2026-10-21: the k-way merge in vectors (a k >= 3 crossing); a triangle wave (the coverage test, the deferred range) */
};
/* the string cases: a pool of n strings, pointers kept in the f64 array's storage (8 bytes each) */
static char *s_pool; static const char **s_ptr;
static void gen_strings(double *a64, size_t n, const char *shape) {
    free(s_pool); s_pool = (char *)__libc_malloc(n * 48); s_ptr = (const char **)(void *)a64; if (!s_pool) return;
    size_t o = 0; RS = 11;
    for (size_t i = 0; i < n; i++) {
        char *s = s_pool + o; int L;
        if (!strcmp(shape, "urls")) L = snprintf(s, 48, "https://www.site%03u.com/p%u/q%u", (unsigned)(nr() % 300), (unsigned)(nr() % 50), (unsigned)(nr() % 100000));
        else if (!strcmp(shape, "loglines")) { unsigned long long ts = (unsigned long long)i * 2 - ((nr() % 20 == 0) ? nr() % 1000 : 0);   /* 5% late lines */
            if (ts > (unsigned long long)i * 2) ts = 0;
            L = snprintf(s, 48, "%012llu|h%02u|line", ts, (unsigned)(nr() % 32)); }
        else L = snprintf(s, 48, "%016llx%016llx", (unsigned long long)nr(), (unsigned long long)nr());
        s_ptr[i] = s; o += (size_t)L + 1;
    }
}
static uint64_t mix(uint64_t x);
static uint64_t hash_ptrs(const char **p, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) s += mix((uint64_t)(uintptr_t)p[i]); return s; }
/* the batch entry's segments: a fixed cycle of lengths over the array, some past the leaf's limit, some under the thin front's */
static size_t b_off[4097]; static size_t b_nseg;
static void batch_layout(size_t n) {
    static const size_t L[] = { 50000, 3000, 100, 70000, 17, 2, 0, 131072, 9000, 65536 };
    size_t o = 0; b_nseg = 0; b_off[0] = 0;
    while (o < n && b_nseg < 4096) { size_t l = L[b_nseg % (sizeof L / sizeof *L)]; if (o + l > n) l = n - o; o += l; b_off[++b_nseg] = o; }
    if (o < n) b_off[b_nseg] = n;                                   /* the last segment takes what is left */
}
#define NCASES ((int)(sizeof CASES / sizeof CASES[0]))
/* three long sorted blocks, shuffled, then 70 short ascending runs interleaved over one value range (one chain of 70):
   relocation merges the chain with merge_all_runs, past the 64 runs it keeps on its stack */
static void gen_many_runs(double *a, size_t n) {
    size_t tail = n / 4, runs = 70, rl = tail / runs; if (rl < 2) rl = 2;          /* the chain at the TAIL: see harness.c */
    size_t rest = n - tail, blk = rest / 3; const size_t order[3] = { 2, 0, 1 }; if (!blk) blk = 1;
    for (size_t i = 0; i < rest; i++) { size_t b = i / blk < 3 ? i / blk : 2; a[i] = 2e6 + (double)order[b] * 1e7 + (double)(i - b * blk) + fr() * 0.5; }
    for (size_t i = 0; i < tail; i++) { size_t r = i / rl, j = i % rl; a[rest + i] = (double)j * (1e6 / (double)rl) + (double)r * 1e-3 + fr() * 1e-4; }
}
static void gen64(double *a, size_t n, const char *s) {
    if (!strcmp(s, "many_runs")) { RS = 7; gen_many_runs(a, n); return; }
    RS = 7; size_t nb = 64, blk = (n + nb - 1) / nb, perm[64];
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) { size_t r = nr() % (k + 1); size_t t = perm[k]; perm[k] = perm[r]; perm[r] = t; }
    size_t q4 = n / 4 ? n / 4 : 1;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(s, "uniform")) a[i] = fr() * 2e9 - 1e9;
        else if (!strcmp(s, "heavy_tail")) a[i] = 1000.0 * pow(1.0 - fr() * 0.999999, -1.0 / 1.2);
        else if (!strcmp(s, "few_distinct")) a[i] = (double)(nr() % 5) * 1e6;
        else if (!strcmp(s, "quantized")) a[i] = (double)(nr() % 4096) / 64.0;
        else if (!strcmp(s, "mod_card_2k")) a[i] = sqrt((double)(nr() % 2000));
        else if (!strcmp(s, "mod_card_30k")) a[i] = (double)(nr() % 30000) * 1.5;
        else if (!strcmp(s, "lattice_spill")) a[i] = (double)(nr() % 30000) * 1.5 + (i >= n - n / 2000 ? 0.1 : 0.0);   /* the last 0.05% off it */
        else if (!strcmp(s, "near_sorted") || !strcmp(s, "ring_buffer")) a[i] = (double)i + fr() * 0.5;
        else if (!strcmp(s, "block_perm") || !strcmp(s, "block_twist") || !strcmp(s, "block_kinked") || !strcmp(s, "sparse_over") || !strcmp(s, "blocks_strays")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) + fr() * 0.5;
        else if (!strcmp(s, "sine_wave")) { double T = (double)n / 32.0; a[i] = (double)i + 0.3 * T * sin(6.283185307179586 * (double)i / T) + (fr() * 2.0 - 1.0) * 50.0; }
        else if (!strcmp(s, "stairs_x20") || !strcmp(s, "stairs_twist")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * (1.0 + 1.0 / 9.0) + fr() * 0.5;
        else if (!strcmp(s, "stairs_triple")) a[i] = (double)perm[i / blk] * (double)blk + (double)(i % blk) * 2.5 + fr() * 0.5;
        else if (!strcmp(s, "stairs_order")) a[i] = (double)(i / blk) * (double)blk + (double)(i % blk) * 1.2 + fr() * 0.5;           /* in order, each run overlapping the next by 20% */
        else if (!strcmp(s, "dominant_90")) a[i] = (nr() % 10) ? 12345.0 : fr() * 2e9 - 1e9;
        else if (!strcmp(s, "few_runs")) a[i] = (double)(i % q4) * 4.0 + (double)(i / q4) + fr() * 0.5;
        else if (!strncmp(s, "short_reach", 11) || !strcmp(s, "mirror_reach") || !strcmp(s, "ring_reach")) a[i] = (double)i + (fr() * 2.0 - 1.0) * 60.0;           /* the short-reach route: blocks in place, seams */
        else if (!strcmp(s, "mirror_streams")) a[i] = (double)(i % 4) * (double)n + (double)(i / 4) + fr() * 0.5;
        else if (!strcmp(s, "runs16")) a[i] = (double)(((uint64_t)(i / 16) * 2654435761u) % 1000003u) + 0.25;                         /* runs of 16 equal values */
        else if (!strcmp(s, "head_5")) a[i] = i < n / 20 ? fr() * (double)n : (double)i + 0.5;                                          /* a random head, a sorted suffix */
        else if (!strcmp(s, "windows_64k")) a[i] = (double)(i / 65536) * 65536.0 + fr() * 65536.0;                                       /* windows in order */
        else if (!strcmp(s, "dominant_50")) a[i] = (nr() & 1) ? 12345.0 : fr() * 2e9 - 1e9;
        else if (!strcmp(s, "strands4")) a[i] = (double)(i % (n / 4 + 1)) * 4.0 + (double)(i / (n / 4 + 1)) + 0.25;                         /* four sorted strands laid out, crossing everywhere */
        else if (!strcmp(s, "tri1024")) { size_t ph = i % 1024; a[i] = (double)(ph < 512 ? ph : 1024 - ph) + (double)(i / 1024) / (double)(n / 1024 + 2) + (ph < 512 ? 0.0 : 0.25 / (double)(n / 1024 + 2)); }
        else a[i] = i < n - n / 20 ? (double)i : fr() * (double)n;                                   /* prefix_95 */
    }
    if (!strcmp(s, "near_sorted")) for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % n, y = nr() % n; double t = a[x]; a[x] = a[y]; a[y] = t; }
    if (!strcmp(s, "halves")) { size_t h = n / 2; for (size_t i = 0; i < n; i++) a[i] = i < h ? (double)i * 2.0 + 0.25 : (double)(i - h) * 2.0 + 1.25; }   /* two sorted halves, crossing everywhere */
    /* every block of block_twist and every other block of stairs_twist lies descending: backward copies into tiles (an orphaned
       tile is filled by the caller), and crossings of one strand up and one down */
    if (!strcmp(s, "block_twist") || !strcmp(s, "stairs_twist")) { size_t every = !strcmp(s, "block_twist") ? 1 : 2;
        for (size_t b = 0; b * blk < n; b += every) { size_t lo = b * blk, hi = lo + blk > n ? n : lo + blk;
            for (size_t l = lo, r = hi - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; } } }
    if (!strcmp(s, "dense_spread")) { for (size_t i = 0; i < n; i++) a[i] = (double)(i * 12 + nr() % 12); for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); double v = a[i]; a[i] = a[j]; a[j] = v; } }
    if (!strcmp(s, "sparse_over")) { size_t g = n / 65; for (size_t i = 0; i < g; i++) a[i] = ((double)i + 0.5) * (double)(n - g) / (double)g + 0.123; }   /* over the blocks below */
    if (!strcmp(s, "block_kinked")) for (size_t k = 0; k < n / 100; k++) { size_t x = 2 * (nr() % (n / 2 - 1)); double v = a[x]; a[x] = a[x + 1]; a[x + 1] = v; }   /* move I: the detector swaps them back */
    if (!strncmp(s, "mirror_", 7)) for (size_t l = 0, r = n - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }   /* handed over back to front */
    if (!strcmp(s, "short_reach_stray")) a[n / 3] = 4.0 * (double)n;                                /* one far element: the stray lists and the in-place shift */
    if (!strcmp(s, "blocks_strays")) for (size_t q = 0; q < n / 100; q++) a[nr() % n] = fr() * (double)n;
    if (!strcmp(s, "one_kinked") || !strcmp(s, "one_strays")) {        /* one sorted strand: 1% adjacent swaps, or 0.1% strays */
        double c = 1e6; for (size_t i = 0; i < n; i++) { c += 1.0 + fr() * 9.0; a[i] = c; }
        if (!strcmp(s, "one_strays")) for (size_t q = 0; q < n / 1000; q++) a[nr() % n] = fr() * c;
        else for (size_t q = 0; q < n / 100; q++) { size_t x = nr() % (n - 1); double v = a[x]; a[x] = a[x + 1]; a[x + 1] = v; } }
    if (!strncmp(s, "ring_", 5)) {                                     /* a ring buffer read from slot 0: rotated left by 0.37 n, by three reversals */
        size_t r = n * 37 / 100;
        for (size_t l = 0, q = r - 1; l < q; l++, q--) { double x = a[l]; a[l] = a[q]; a[q] = x; }
        for (size_t l = r, q = n - 1; l < q; l++, q--) { double x = a[l]; a[l] = a[q]; a[q] = x; }
        for (size_t l = 0, q = n - 1; l < q; l++, q--) { double x = a[l]; a[l] = a[q]; a[q] = x; } }
    if (!strcmp(s, "blocks_1000")) {                                   /* disjoint blocks of 1000, shuffled, every other one descending */
        size_t b = 1000, nb2 = (n + b - 1) / b; size_t *pm = malloc(nb2 * sizeof *pm);
        if (pm) { for (size_t k = 0; k < nb2; k++) pm[k] = k; for (size_t k = nb2 - 1; k > 0; k--) { size_t r = nr() % (k + 1), x = pm[k]; pm[k] = pm[r]; pm[r] = x; }
                  for (size_t i = 0; i < n; i++) { size_t j = i % b; if ((i / b) & 1) j = b - 1 - j; a[i] = (double)pm[i / b] * (double)b + (double)j + fr() * 0.5; } free(pm); } }
}
static void gen32(int32_t *a, size_t n, const char *s) {
    RS = 7;
    for (size_t i = 0; i < n; i++) a[i] = !strcmp(s, "int_wide") ? (int32_t)nr() : (int32_t)(nr() % 1000);
    if (!strcmp(s, "int_sentinel")) a[n / 3] = INT32_MIN;
}
/* order-independent hash of the bit patterns: a sum of mixed words. Equal multisets give equal sums; unequal ones collide
   with probability about 2^-64. */
static inline uint64_t mix(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; }
static uint64_t hash64(const double *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8); s += mix(b); } return s; }
static uint64_t hash32(const int32_t *a, size_t n) { uint64_t s = 0; for (size_t i = 0; i < n; i++) s += mix((uint64_t)(uint32_t)a[i] + 0x9e3779b97f4a7c15ULL); return s; }

/* the lazy view's two halves, driven the way logos_view.c drives them; static storage, so nothing is allocated here */
static size_t v_starts[LOGOS_MAX_VIEW_BUCKETS + 1]; static unsigned char v_done[LOGOS_MAX_VIEW_BUCKETS];
static double v_min64[LOGOS_MAX_VIEW_BUCKETS], v_max64[LOGOS_MAX_VIEW_BUCKETS]; static int32_t v_min32[LOGOS_MAX_VIEW_BUCKETS], v_max32[LOGOS_MAX_VIEW_BUCKETS];
static void call_entry(int entry, double *a64, int32_t *a32, size_t n, int T) {
    if (entry == E_F64) logos_sort_f64(a64, n, T);
    else if (entry == E_STR) logos_sort_strings_mt((const char **)(void *)a64, n, T);
    else if (entry == E_BATCH64) { batch_layout(n); logos_sort_f64_batch(a64, b_off, b_nseg, T); }
    else if (entry == E_IPP64) { setenv("LOGOS_PART_INPLACE", "1", 1); logos_sort_f64(a64, n, T); unsetenv("LOGOS_PART_INPLACE"); }
    else if (entry == E_INPLACE64) logos_sort_f64_inplace(a64, n, T);
    else if (entry == E_INPLACE32) logos_sort_i32_inplace(a32, n, T);
    else if (entry == E_I32) logos_sort_i32(a32, n, T);
    else if (entry == E_VIEW64) { int nbk = logos_partition_f64(a64, n, T, v_starts, v_min64, v_max64); if (nbk > 0) { memset(v_done, 0, sizeof v_done); logos_sort_buckets_f64(a64, v_starts, nbk, v_done, T); } }
    else { int nbk = logos_partition_i32(a32, n, T, v_starts, v_min32, v_max32); if (nbk > 0) { memset(v_done, 0, sizeof v_done); logos_sort_buckets_i32(a32, v_starts, nbk, v_done, T); } }
}

typedef struct { long out; uintptr_t site; } result_t;
/* One cold call in a child that inherits the input copy-on-write. target 0: count only, out = allocs | spawns << 32.
   Otherwise out = 0 ok, 1 wrong output; -1 crashed, -2 hung. */
static result_t run(int entry, double *a64, int32_t *a32, size_t n, uint64_t hin, int threads, int kind, int rest, long target, int trace) {
    result_t res = { -1, 0 };
    int fd[2]; if (pipe(fd)) return res;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]); alarm(5);                                        /* an unfaulted call takes about 30 ms */
        fp_kind = kind; fp_rest = rest; fp_target = target; fp_trace = trace;
        atomic_store(&fp_allocs, 0); atomic_store(&fp_spawns, 0); atomic_store(&fp_site, 0); atomic_store(&fp_armed, 1);
        call_entry(entry, a64, a32, n, threads);
        atomic_store(&fp_armed, 0);
        result_t r; r.site = atomic_load(&fp_site);
        if (target == 0) r.out = atomic_load(&fp_allocs) | (atomic_load(&fp_spawns) << 32);
        else {
            int ok = 1;
            if (entry == E_STR) { const char **sp = (const char **)(void *)a64; for (size_t i = 1; i < n; i++) if (strcmp(sp[i - 1], sp[i]) > 0) { ok = 0; break; } if (hash_ptrs(sp, n) != hin) ok = 0; }
            else if (entry == E_BATCH64) { for (size_t k = 0; k < b_nseg && ok; k++) for (size_t i = b_off[k] + 1; i < b_off[k + 1]; i++) if (a64[i] < a64[i - 1]) { ok = 0; break; } if (hash64(a64, n) != hin) ok = 0; }
            else if (entry == E_F64 || entry == E_VIEW64 || entry == E_INPLACE64 || entry == E_IPP64) { for (size_t i = 1; i < n; i++) if (a64[i] < a64[i - 1]) { ok = 0; break; } if (hash64(a64, n) != hin) ok = 0; }
            else { for (size_t i = 1; i < n; i++) if (a32[i] < a32[i - 1]) { ok = 0; break; } if (hash32(a32, n) != hin) ok = 0; }
            r.out = ok ? 0 : 1;
        }
        if (write(fd[1], &r, sizeof r) != (ssize_t)sizeof r) _exit(3);
        _exit(0);
    }
    close(fd[1]);
    ssize_t got = read(fd[0], &res, sizeof res); close(fd[0]);
    int st = 0; waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) { res.out = WTERMSIG(st) == SIGALRM ? -2 : -1; return res; }
    if (got != (ssize_t)sizeof res) res.out = -1;
    return res;
}
static const char *verdict(long v) { return v == 0 ? "ok" : v == 1 ? "WRONG OUTPUT" : v == -1 ? "CRASHED" : "HUNG"; }

/* distinct failed sites, per kind */
static uintptr_t SITES[2][2048]; static int NS[2];
static void note_site(int kind, uintptr_t s) { if (!s) return; for (int i = 0; i < NS[kind]; i++) if (SITES[kind][i] == s) return; if (NS[kind] < 2048) SITES[kind][NS[kind]++] = s; }
static void name_sites(const char *self, int kind) {           /* addr2line, if there is one; the counts above do not depend on it */
    if (!NS[kind]) return;
    char cmd[32768]; int k = snprintf(cmd, sizeof cmd, "addr2line -f -s -e '%s'", self);
    for (int i = 0; i < NS[kind] && k < (int)sizeof cmd - 32; i++) k += snprintf(cmd + k, sizeof cmd - (size_t)k, " 0x%lx", (unsigned long)(SITES[kind][i] - (kind ? 0 : 1)));
    k += snprintf(cmd + k, sizeof cmd - (size_t)k, " 2>/dev/null");
    FILE *p = popen(cmd, "r"); if (!p) return;
    char fnm[256], loc[256]; int col = 0;
    while (fgets(fnm, sizeof fnm, p) && fgets(loc, sizeof loc, p)) {
        fnm[strcspn(fnm, "\n")] = 0; loc[strcspn(loc, "\n")] = 0;
        if (kind) col += printf("%s%s", col ? ", " : "    ", fnm); else col += printf("%s%s", col ? ", " : "    ", loc);
        if (col > 100) { printf("\n"); col = 0; }
    }
    if (col) printf("\n");
    pclose(p);
}

int main(int argc, char **argv) {
    real_create = (create_fn)dlsym(RTLD_NEXT, "pthread_create");
    if (!real_create) { fprintf(stderr, "no pthread_create to forward to\n"); return 2; }
    if (argc > 1 && !strcmp(argv[1], "-l")) { for (int i = 0; i < NCASES; i++) printf("%s:%s\n", EN[CASES[i].entry], CASES[i].name); return 0; }
    int single = argc > 1 && !strcmp(argv[1], "-c");
    if (single && argc < 6) { fprintf(stderr, "usage: %s -c <case> <threads> one|rest|spawn-one|spawn-rest <k> [n]     (cases: %s -l; a case is <shape> or <entry>:<shape>)\n", argv[0], argv[0]); return 2; }
    size_t n = single ? (argc > 6 ? (size_t)atof(argv[6]) : 2100000) : (argc > 1 ? (size_t)atof(argv[1]) : 2100000);
    double *a64 = (double *)__libc_malloc(n * 8); int32_t *a32 = (int32_t *)__libc_malloc(n * 4); if (!a64 || !a32) return 2;
    if (single) {
        int ci = -1; char full[96];
        for (int i = 0; i < NCASES && ci < 0; i++) { snprintf(full, sizeof full, "%s:%s", EN[CASES[i].entry], CASES[i].name); if (!strcmp(CASES[i].name, argv[2]) || !strcmp(full, argv[2])) ci = i; }
        if (ci < 0) { fprintf(stderr, "unknown case %s\n", argv[2]); return 2; }
        int e = CASES[ci].entry, is64 = e == E_F64 || e == E_VIEW64 || e == E_INPLACE64 || e == E_BATCH64 || e == E_IPP64, T = atoi(argv[3]), kind = !strncmp(argv[4], "spawn", 5), rest = strstr(argv[4], "rest") != NULL; long k = atol(argv[5]);
        if (CASES[ci].entry == E_STR) gen_strings(a64, n, CASES[ci].name); else if (is64) gen64(a64, n, CASES[ci].name); else gen32(a32, n, CASES[ci].name);
        uint64_t h = CASES[ci].entry == E_STR ? hash_ptrs((const char **)(void *)a64, n) : is64 ? hash64(a64, n) : hash32(a32, n);
        result_t c = run(e, a64, a32, n, h, T, 0, 0, 0, 0);
        printf("%s %s, %d threads, n=%zu: %ld allocations, %ld thread creations in one cold call\n", EN[e], CASES[ci].name, T, n, c.out & 0xffffffffL, c.out >> 32);
        result_t v = run(e, a64, a32, n, h, T, kind, rest, k, 1);
        printf("%s k=%ld: %s\n", argv[4], k, verdict(v.out));
        return v.out != 0;
    }
    int tlist[2] = { 1, argc > 2 ? atoi(argv[2]) : 4 };
    const char *filter = argc > 3 ? argv[3] : NULL;                     /* only the cases whose shape name contains this */
    printf("fault injection: n=%zu, one cold call per run, a fresh child per run. Cells are ok / wrong output / crashed / hung.\n", n);
    printf("  one = only the k-th call fails;  rest = every call from the k-th on fails (the resource has run out)\n\n");
    printf("  %-8s %-14s %2s %6s %6s | %-13s | %-13s | %-11s | %-11s\n", "entry", "shape", "T", "allocs", "spawns", "alloc one", "alloc rest", "spawn one", "spawn rest");
    long bad[2] = { 0, 0 }, runs[2] = { 0, 0 };
    for (int ci = 0; ci < NCASES; ci++) {
        int e = CASES[ci].entry, is64 = e == E_F64 || e == E_VIEW64 || e == E_INPLACE64 || e == E_BATCH64 || e == E_IPP64;
        if (filter && !strstr(CASES[ci].name, filter) && !strstr(EN[CASES[ci].entry], filter)) continue;      /* a shape or an entry */
        if (CASES[ci].entry == E_STR) gen_strings(a64, n, CASES[ci].name); else if (is64) gen64(a64, n, CASES[ci].name); else gen32(a32, n, CASES[ci].name);
        uint64_t h = CASES[ci].entry == E_STR ? hash_ptrs((const char **)(void *)a64, n) : is64 ? hash64(a64, n) : hash32(a32, n);
        for (int ti = 0; ti < 2; ti++) {
            result_t c = run(e, a64, a32, n, h, tlist[ti], 0, 0, 0, 0);
            if (c.out < 0) { printf("  %-8s %-14s %2d   the unfaulted run itself failed (%ld)\n", EN[e], CASES[ci].name, tlist[ti], c.out); bad[0]++; continue; }
            long A = c.out & 0xffffffffL, C = c.out >> 32, r[4][4] = { { 0 } }, first[4] = { 0, 0, 0, 0 };
            for (int m = 0; m < 4; m++) {
                int kind = m >> 1, rest = m & 1; long K = kind ? C : A;
                for (long k = 1; k <= K; k++) {
                    result_t v = run(e, a64, a32, n, h, tlist[ti], kind, rest, k, 0);
                    int slot = v.out == 0 ? 0 : v.out == 1 ? 1 : v.out == -1 ? 2 : 3;
                    r[m][slot]++; runs[kind]++; note_site(kind, v.site);
                    if (slot) { bad[kind]++; if (!first[m]) first[m] = k; }
                }
            }
            printf("  %-8s %-14s %2d %6ld %6ld |", EN[e], CASES[ci].name, tlist[ti], A, C);
            for (int m = 0; m < 4; m++) { char b[48]; snprintf(b, sizeof b, "%ld/%ld/%ld/%ld", r[m][0], r[m][1], r[m][2], r[m][3]); printf(" %-*s%s", m < 2 ? 13 : 11, b, m < 3 ? " |" : ""); }
            if (first[0] || first[1] || first[2] || first[3]) printf("  first bad k: %ld %ld %ld %ld", first[0], first[1], first[2], first[3]);
            printf("\n"); fflush(stdout);
        }
    }
    printf("\nTOTAL allocation faults: %ld runs, %ld bad.   thread-creation faults: %ld runs, %ld bad.\n", runs[0], bad[0], runs[1], bad[1]);
    printf("distinct engine allocation sites failed at least once: %d\n", NS[0]);
    printf("distinct worker functions whose creation was failed: %d\n", NS[1]); name_sites(argv[0], 1);
    if (getenv("FAULTPROBE_SITES")) { printf("allocation sites:\n"); name_sites(argv[0], 0); }
    return (bad[0] || bad[1]) ? 1 : 0;
}
