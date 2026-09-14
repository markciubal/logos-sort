/* logos_psort.c — v3: the JavaScript engine's numeric dispatcher, ported to C,
 * used as the per-bucket leaf (and the sequential path) under the v2 parallel
 * sample-sort top level.
 *
 * Per-range dispatch (port of lib/sort.js sort_fn / sort_fn.f64):
 *   one-pass prescan (min, max, all-integer?, ascending?, descending?)
 *   -> already ascending: return; descending: reverse
 *   -> all integers: span<=n counting sort; int32 range: byte-skip LSD radix
 *   -> tiny: insertion
 *   -> natural runs (few, or many-but-long): galloping run-merge (2-way + 4-way)
 *   -> nearly sorted by inversion sample: insertion / momentum insertion
 *      (momentum carries a displacement budget and aborts to the paths below)
 *   -> large: sampled gap detection -> gap-split partition + recurse;
 *      flash-safe & <500k -> flash sort; sampled skew >8 -> exponent-bit
 *      ("bits") coarse bucketing; else value coarse bucket sort
 *   -> otherwise dual-pivot introsort (heapsort guard)
 *   Typed entry (>=65536): quantized data (<=4 active key bytes, n<=8M) ->
 *   8-pass float radix with constant-byte skip; skewed -> bits coarse.
 *
 * Deviation from JS: module-level side channels (last_bail_avg_run, pooled
 * scratch) become per-thread context, since buckets sort concurrently.
 *
 * Parallel top level (v2, unchanged): sample -> quantile splitters -> parallel
 * histogram -> parallel scatter -> per-bucket leaf via atomic work queue with
 * parallel copyback; one thread spawn; barrier-synchronized phases; no locks
 * outside the barrier; branchless bucket lookup.
 */
#define _GNU_SOURCE
#include "logos_psort.h"
#include <stdlib.h>
#ifdef __linux__
#include <sys/mman.h>          /* madvise(MADV_HUGEPAGE): see big_alloc */
#endif
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

/* ---- optional phase trace (-DLOGOS_TRACE): stderr timestamps per phase ---- */
#ifdef LOGOS_TRACE
#include <stdio.h>
#include <time.h>
static double g_trace_t0;
static double trace_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#define TRACE_START() (g_trace_t0 = trace_now())
#define TRACE(...) do { fprintf(stderr, "  [trace %8.2f ms] ", trace_now() - g_trace_t0); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define TRACE_START() ((void)0)
#define TRACE(...) ((void)0)
#endif

/* ---- optional per-phase profiling (-DLOGOS_PROFILE); see logos_psort.h ---- */
const char *const logos_profile_phase_names[LOGOS_PH_N] = { "prescan", "lowcard probe", "modcard probe", "near-sorted", "relocate", "sample",
                                                            "histogram", "combine", "scatter", "leaf sorts", "copyback", "other", "relocate probe" };
const char *const logos_profile_path_names[LOGOS_PATH_N] = { "none", "already sorted", "counting", "int32 radix", "insertion", "run merge",
                                                             "run relocate", "momentum", "gap split", "flash", "bits coarse", "coarse",
                                                             "quicksort", "quant radix", "simd leaf" };
const char *const logos_profile_route_names[LOGOS_ROUTE_N] = { "none", "already sorted", "lowcard counting", "lattice counting", "modcard counting",
                                                               "sequential leaf", "near-sorted merge", "run relocation", "sample-sort partition" };
#ifdef LOGOS_PROFILE
#include <time.h>
static logos_profile_t g_prof;
static int g_prof_phase;                        /* phase the counting-probe workers attribute their time to */
static double g_prof_tp;                        /* phase clock: start of the phase being timed on the calling thread */
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double prof_cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
typedef struct { double w, c; } prof_stamp_t;   /* wall clock + this thread's CPU clock */
static inline prof_stamp_t prof_stamp(void) { prof_stamp_t s; s.w = prof_now(); s.c = prof_cpu(); return s; }
static void prof_phase(int ph) { double now = prof_now(); g_prof.phase_ms[ph] += now - g_prof_tp; g_prof_tp = now; }
static void prof_finish(int route, double t_start) { g_prof.route = route; g_prof.total_ms = prof_now() - t_start; }
/* busy = wall time inside a work section; cpu = the CPU time the thread actually got in it (the gap is preemption) */
static inline double prof_busy_since(int tid, int ph, prof_stamp_t s) {
    double w = prof_now() - s.w, c = prof_cpu() - s.c; if (c > w) c = w;
    if (tid >= 0 && tid < LOGOS_PROF_MAX_THREADS) { g_prof.busy_ms[tid][ph] += w; g_prof.cpu_ms[tid][ph] += c; } return w; }
static inline void prof_wait(int tid, int ph, double ms) { if (tid >= 0 && tid < LOGOS_PROF_MAX_THREADS) g_prof.wait_ms[tid][ph] += ms; }
static inline void prof_path(int tid, int path, size_t n, double ms) {
    if (tid >= 0 && tid < LOGOS_PROF_MAX_THREADS) { g_prof.path_ms[tid][path] += ms; g_prof.path_calls[tid][path]++; g_prof.path_elems[tid][path] += n; } }
void logos_profile_reset(void) { memset(&g_prof, 0, sizeof g_prof); }
const logos_profile_t *logos_profile_get(void) { return &g_prof; }
#define PROF(...) __VA_ARGS__
#else
void logos_profile_reset(void) {}
const logos_profile_t *logos_profile_get(void) { return NULL; }
#define PROF(...)
#endif
#define PATH_TAG(c, X) PROF(if ((c)->path == LOGOS_PATH_NONE) (c)->path = (X);)

/* ---- constants ported from lib/sort.js ---------------------------------- */
/* Memory-level parallelism for scatters: the destination line for the element
 * PF_DIST ahead is prefetched, so a core keeps many misses in flight instead of
 * serializing on the cursor chain `to[c[b]++] = x`. Evidence: on a 20-CPU box
 * both sorts sped up 1.3-1.5x going from 10 to 20 threads, i.e. the scatter is
 * latency-bound (hyperthreads hide stalls), not bandwidth-saturated. */
#if defined(__GNUC__) && !defined(LOGOS_NO_PREFETCH)
#  define PREFETCH_W(p) __builtin_prefetch((p), 1, 0)
#else
#  define PREFETCH_W(p) ((void)0)
#endif
#ifndef PF_DIST
#define PF_DIST 32          /* override: -DPF_DIST=N; measure with `make pfbench` */
#endif

#ifndef INSERTION_SORT_THRESHOLD
#define INSERTION_SORT_THRESHOLD 24
#endif
/* JS-era dispatcher thresholds, inherited untuned; every one overridable for the sweep (make knobbench) */
#ifndef MERGE_MAX_RUNS
#define MERGE_MAX_RUNS 8   /* k-way merge only when the overlapping, non-relocatable runs are this few (make knobbench): 64 runs at 1T 283 -> 116 ms via the leaf; a 4-way merge still beats the leaf (89 vs 120), and a cap of 3 lost that */
#endif
#ifndef SKEW_PROBE_MAX
#define SKEW_PROBE_MAX 8
#endif
#ifndef QUANT_PROBE_MAX
#define QUANT_PROBE_MAX 4
#endif
#ifndef COUNTING_SORT_K
#define COUNTING_SORT_K          1
#endif
#ifndef NEARLY_SORTED_INV_RATIO
#define NEARLY_SORTED_INV_RATIO  0.05
#endif
#ifndef MOMENTUM_THRESHOLD
#define MOMENTUM_THRESHOLD       50000
#endif
#ifndef MAX_RUNS_FOR_MERGE
#define MAX_RUNS_FOR_MERGE       32
#endif
#ifndef MIN_AVG_RUN_FOR_MERGE
#define MIN_AVG_RUN_FOR_MERGE    4096     /* was 256 (JS era). Swept (make knobbench): 4096 sends sawtooth_stride's 5376 runs to the AVX2 leaf, 553 -> 138 ms at 1T; 262144 broke relocation on 64-block data (17 -> 52 ms), so the cap stays here and the merge has its own MERGE_MAX_RUNS */
#endif
#ifndef MOMENTUM_MAX_AVG_RUN
#define MOMENTUM_MAX_AVG_RUN     8
#endif
#ifndef FLASH_SORT_THRESHOLD
#define FLASH_SORT_THRESHOLD     4096
#endif
#ifndef FLASH_SAFETY_RATIO
#define FLASH_SAFETY_RATIO       0.05
#endif
#ifndef COARSE_MIN_N
#define COARSE_MIN_N             500000   /* below this a bucket goes to flash, above to coarse; measure with make leafbench */
#endif
#ifndef FLASH_M_DIV
#define FLASH_M_DIV              1        /* flash classes = n / FLASH_M_DIV (Devroye: fix-up cost is insensitive to a few elements per class) */
#endif
#ifndef MIN_GALLOP
#define MIN_GALLOP               7
#endif
#ifndef COARSE_K
#define COARSE_K                 2048
#endif
#define COARSE_MAXD              4
#ifndef COARSE_SMALL
#define COARSE_SMALL             64
#endif
#ifndef QUADWAY_THRESHOLD
#define QUADWAY_THRESHOLD        16
#endif
#ifndef TYPED_MIN_N
#define TYPED_MIN_N              65536
#endif
#ifndef RADIX64_MAX_N
#define RADIX64_MAX_N            8388608
#endif
/* parallel top level */
#ifndef OVERSAMPLE
#define OVERSAMPLE               32
#endif
#define LOWDISC_GAP_RATIO        4.0      /* max/min sorted-gap ratio in a 64-window below which data is low-discrepancy */
/* Sample positions. Default: the Kronecker/Weyl sequence floor(frac(i*phi)*n), equidistributed
 * against every period at once (three-gap theorem; phi's continued fraction is all ones, so its
 * rotation has the lowest discrepancy). A fixed stride aliases with periodic input: a sawtooth
 * whose period equals the stride puts every sample at one phase and the splitters collapse.
 * -DLOGOS_STRIDE_SAMPLE restores the stride (A/B: make chaosbench, shape sawtooth_stride). */
#ifdef LOGOS_STRIDE_SAMPLE
#define SAMPLE_POS(i, m, n) ((size_t)(i) * ((n) / (m) > 0 ? (n) / (m) : 1))
#else
static inline size_t weyl_pos(size_t i, size_t n) { double f = (double)i * 0.6180339887498949; f -= (double)(uint64_t)f; size_t q = (size_t)(f * (double)n); return q < n ? q : n - 1; }
#define SAMPLE_POS(i, m, n) weyl_pos((i), (n))
#endif
#define MIN_PARALLEL             (1u<<16)
/* Thread-count policy (make scale-full, 2026-09-10): the parallel route has a fixed cost of several
 * ms (thread creation, barriers, P*12 tiny buckets), so on this box 20 threads were SLOWER than one
 * from 1e5 to ~5e5 elements. Use at most one thread per MIN_ELEMS_PER_THREAD elements; results never
 * depend on the thread count. -DLOGOS_NO_THREAD_POLICY restores "all threads for any n >= 65536". */
#ifndef MIN_ELEMS_PER_THREAD
#define MIN_ELEMS_PER_THREAD     100000
#endif
static inline int effective_threads(size_t n, int P) {
#ifdef LOGOS_NO_THREAD_POLICY
    (void)n; return P;
#else
    size_t pmax = n / MIN_ELEMS_PER_THREAD; if (pmax < 1) pmax = 1;
    return (size_t)P > pmax ? (int)pmax : P;
#endif
}
#ifndef BUCKETS_PER_THREAD
#define BUCKETS_PER_THREAD       12       /* was 4. Measured (make theorybench): 12 shrinks the queue tail and keeps ~33k-element leaves L2-resident; capped so P*BPT <= 256 (byte bucket cache) */
#endif
#define MAX_BUCKETS              256      /* the per-element bucket-index cache is one byte */

/* ---- per-thread context (replaces JS module-level pool + side channels) --- */
typedef struct {
    size_t cap;
    uint32_t *aux;                                  /* cap+1: counts / radix src   */
    uint16_t *idx;                                  /* cap: coarse bucket index    */
    size_t *C[COARSE_MAXD], *S[COARSE_MAXD], *H[COARSE_MAXD];
    uint32_t hist[8][257];
    double last_bail_avg_run;
    double *wc; uint8_t *wcn;                       /* write-combining stage: K x 8 doubles, K counters */
    int wc_on; int seq;        /* 1 when this context sorts on the calling thread alone (AVX2 leaf policy) */ int nearly;   /* sort_fn's inversion probe said nearly sorted: coarse keeps its buckets too */ int lowdisc;   /* sort_fn's window probe found low-discrepancy data: flash/coarse, not the AVX2 leaf */                                      /* runtime: staged scatters only pay when the scatter is DRAM-bound (many threads) */
    uint8_t *land[COARSE_MAXD];                     /* ping-pong: which buffer each child landed in */
    int tid, path;                                  /* -DLOGOS_PROFILE: thread index, first leaf path taken */
    int pooled;                                     /* buffers belong to the persistent bundle table */
} ctx_t;

#define WC_MIN_THREADS 8   /* measured: staging loses ~8-35% sequentially, wins ~5-12% at 20 threads */
static int ctx_alloc(ctx_t *c, size_t cap, int wc_on) {
    memset(c, 0, sizeof *c); c->cap = cap; c->wc_on = wc_on;
    c->aux = malloc((cap + 1) * sizeof(uint32_t));
    c->idx = malloc((cap ? cap : 1) * sizeof(uint16_t));
    if (!c->aux || !c->idx) return 0;
    c->wc = malloc((size_t)COARSE_K * 8 * sizeof(double)); c->wcn = calloc(COARSE_K, 1);
    if (!c->wc || !c->wcn) return 0;
    for (int d = 0; d < COARSE_MAXD; d++) {
        c->land[d] = malloc(COARSE_K); if (!c->land[d]) return 0;
        c->C[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        c->S[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        c->H[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        if (!c->C[d] || !c->S[d] || !c->H[d]) return 0;
    }
    return 1;
}
static void ctx_release(ctx_t *c) {
    free(c->aux); free(c->idx); free(c->wc); free(c->wcn);
    for (int d = 0; d < COARSE_MAXD; d++) { free(c->C[d]); free(c->S[d]); free(c->H[d]); free(c->land[d]); }
}
/* Persistent context bundles (default; -DLOGOS_NO_CTX_POOL allocates per call). A context's buffers are
 * ~6 bytes per element of capacity plus ~340 KB fixed; the partition made P of them fresh on every call
 * (9 MB of malloc and first-touch at 8M, the 1.15 ms serial step between scatter and leaves) and the
 * sequential path one of size n (48 MB at 8M, page-faulted every call). Buffers now live in a static
 * table indexed in order of ctx_init calls within one top-level sort, grown when a call needs more,
 * never freed; ctx_free returns nothing. One sort at a time, the same contract as the buffer pool. */
#ifndef LOGOS_NO_CTX_POOL
#define CTX_POOL_MAX 320
static ctx_t g_ctx_pool[CTX_POOL_MAX]; static int g_ctx_pool_used[CTX_POOL_MAX]; static int g_ctx_next = 0;
static int ctx_init(ctx_t *c, size_t cap, int wc_on) {
    if (g_ctx_next >= CTX_POOL_MAX) { int r = ctx_alloc(c, cap, wc_on); c->pooled = 0; return r; }
    int i = g_ctx_next++; ctx_t *b = &g_ctx_pool[i];
    if (!g_ctx_pool_used[i] || b->cap < cap) {
        if (g_ctx_pool_used[i]) ctx_release(b);
        size_t want = cap + cap / 4 + 64;
        if (!ctx_alloc(b, want, wc_on)) { g_ctx_pool_used[i] = 0; return 0; }
        g_ctx_pool_used[i] = 1;
    }
    size_t bcap = b->cap;
    memset(c, 0, sizeof *c);
    c->aux = b->aux; c->idx = b->idx; c->wc = b->wc; c->wcn = b->wcn;
    for (int d = 0; d < COARSE_MAXD; d++) { c->land[d] = b->land[d]; c->C[d] = b->C[d]; c->S[d] = b->S[d]; c->H[d] = b->H[d]; }
    memset(c->wcn, 0, COARSE_K);
    c->cap = bcap; c->wc_on = wc_on; c->pooled = 1;
    return 1;
}
static void ctx_free(ctx_t *c) { if (!c->pooled) ctx_release(c); }
#else
static int ctx_init(ctx_t *c, size_t cap, int wc_on) { int r = ctx_alloc(c, cap, wc_on); c->pooled = 0; return r; }
static void ctx_free(ctx_t *c) { ctx_release(c); }
#endif

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int cmp_i32(const void *a, const void *b) { int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }
static int log2i(size_t n) { int l = 0; while (n >>= 1) l++; return l; }
static size_t sample_sort(double *s, size_t m);   /* defined with the samplers; used by the histogram-free leaf's spills above it */

/* ---- leaf primitives (indices are signed so hi < lo is a legal empty range) */
static void insertion_sort(double *a, long lo, long hi) {
    for (long i = lo + 1; i <= hi; i++) { double k = a[i]; long j = i - 1; while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; }
}
/* returns 1 if completed, 0 if the displacement budget (~8n) was exceeded */
static int insertion_sort_momentum(double *a, long lo, long hi) {
    long budget = 8 * (hi - lo + 1), work = 0, momentum = 1;
    for (long i = lo + 1; i <= hi; i++) {
        double k = a[i];
        if (a[i - 1] <= k) { momentum = momentum > 1 ? momentum >> 1 : 1; continue; }
        long j = i - 1, step = momentum;
        while (step > 1 && (j - step < lo || a[j - step] <= k)) step >>= 1;
        while (j - step >= lo && a[j - step] > k) { j -= step; step <<= 1; }
        long left = j - step < lo ? lo : j - step, right = j;
        while (left < right) { long mid = (left + right) >> 1; if (a[mid] > k) right = mid; else left = mid + 1; }
        long dist = i - left;
        work += dist;
        if (work > budget) return 0;
        for (long p = i; p > left; p--) a[p] = a[p - 1];
        a[left] = k; momentum = dist;
    }
    return 1;
}
static void sift_down(double *a, long base, long root, long end) {
    for (;;) {
        long big = root, l = 2 * root + 1, r = l + 1;
        if (l < end && a[base + l] > a[base + big]) big = l;
        if (r < end && a[base + r] > a[base + big]) big = r;
        if (big == root) return;
        double t = a[base + root]; a[base + root] = a[base + big]; a[base + big] = t;
        root = big;
    }
}
static void heap_sort(double *a, long lo, long hi) {
    long len = hi - lo + 1;
    for (long i = (len >> 1) - 1; i >= 0; i--) sift_down(a, lo, i, len);
    for (long i = len - 1; i > 0; i--) { double t = a[lo]; a[lo] = a[lo + i]; a[lo + i] = t; sift_down(a, lo, 0, i); }
}
#define SWP(x, y) do { double _t = a[x]; a[x] = a[y]; a[y] = _t; } while (0)
static void sort5(double *a, long i1, long i2, long i3, long i4, long i5) {
    if (a[i2] < a[i1]) SWP(i2, i1);
    if (a[i3] < a[i2]) { SWP(i3, i2); if (a[i2] < a[i1]) SWP(i2, i1); }
    if (a[i4] < a[i3]) { SWP(i4, i3); if (a[i3] < a[i2]) { SWP(i3, i2); if (a[i2] < a[i1]) SWP(i2, i1); } }
    if (a[i5] < a[i4]) { SWP(i5, i4); if (a[i4] < a[i3]) { SWP(i4, i3); if (a[i3] < a[i2]) { SWP(i3, i2); if (a[i2] < a[i1]) SWP(i2, i1); } } }
}
/* dual-pivot introsort; d = remaining depth budget, heapsort at 0 */
static void quicksort(double *a, long lo, long hi, int d) {
    while (hi - lo >= INSERTION_SORT_THRESHOLD) {
        if (d == 0) { heap_sort(a, lo, hi); return; }
        long len = hi - lo + 1;
        long seventh = (len >> 3) + (len >> 6) + 1;
        long e3 = (lo + hi) >> 1, e2 = e3 - seventh, e4 = e3 + seventh, e1 = e2 - seventh, e5 = e4 + seventh;
        sort5(a, e1, e2, e3, e4, e5);
        if (a[e1] != a[e2] && a[e2] != a[e3] && a[e3] != a[e4] && a[e4] != a[e5]) {
            double p1 = a[e2], p2 = a[e4];
            a[e2] = a[lo]; a[e4] = a[hi];
            long less = lo + 1, great = hi - 1, k = less;
            while (k <= great) {
                double ak = a[k];
                if (ak < p1) { a[k] = a[less]; a[less++] = ak; }
                else if (ak > p2) {
                    while (k < great && a[great] > p2) great--;
                    double ag = a[great];
                    a[k] = ag; a[great--] = ak;
                    if (ag < p1) { a[k] = a[less]; a[less++] = ag; }
                }
                k++;
            }
            a[lo] = a[less - 1]; a[less - 1] = p1;
            a[hi] = a[great + 1]; a[great + 1] = p2;
            d--;
            quicksort(a, lo, less - 2, d);
            quicksort(a, great + 2, hi, d);
            if (p1 == p2) return;
            long m_lo = less, m_hi = great;
            while (m_lo <= m_hi && a[m_lo] == p1) m_lo++;
            while (m_lo <= m_hi && a[m_hi] == p2) m_hi--;
            lo = m_lo; hi = m_hi;
        } else {
            double pv = a[e3];
            long lt = lo, gt = hi, k = lo;
            while (k <= gt) {
                double v = a[k];
                if (v < pv)      { double t = a[lt]; a[lt] = v; a[k] = t; lt++; k++; }
                else if (v > pv) { double t = a[gt]; a[gt] = v; a[k] = t; gt--; }
                else k++;
            }
            d--;
            if (lt - lo < hi - gt) { quicksort(a, lo, lt - 1, d); lo = gt + 1; }
            else                   { quicksort(a, gt + 1, hi, d); hi = lt - 1; }
        }
    }
    insertion_sort(a, lo, hi);
}

/* ---- integer paths ------------------------------------------------------- */
/* LSD radix for int32-valued doubles: bias to unsigned, fused histogram +
 * OR/AND constant-byte detection, skip constant bytes, last pass un-biases
 * straight into a[]. src = ctx aux, dst = buf reinterpreted as uint32. */
#ifdef LOGOS_INT_NARROW
/* Bucket-relative digit narrowing (H5). OPT-IN: the A/B (make theorybench) was mixed because the
 * min/max pass it needs is one the fused byte histogram never paid; revisit with the bucket range
 * passed down from the splitters instead of rescanned. After the quantile partition a bucket's int32 keys span far
 * fewer than 32 bits (8M int_wide / 80 buckets: ~26). Subtract the minimum, size the digits to the
 * live bits (<= 13 bits each, <= 8192 counters, cache-resident), and do 1-3 passes instead of 4.
 * Exact. -DLOGOS_NO_INT_NARROW restores the fixed 4-byte-pass radix with byte skipping. */
static void lsd_radix_int32(double *a, size_t n, double *buf, ctx_t *c) {
    uint32_t *src = c->aux, *dst = (uint32_t *)buf;
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (size_t i = 0; i < n; i++) { uint32_t v = (uint32_t)(int32_t)a[i] ^ 0x80000000u; src[i] = v; if (v < mn) mn = v; if (v > mx) mx = v; }
    uint32_t range = mx - mn;
    if (range == 0) return;                                            /* all equal: already sorted */
    int bits = 32 - __builtin_clz(range);
    int np = bits <= 13 ? 1 : bits <= 26 ? 2 : 3, w = (bits + np - 1) / np; uint32_t mask = (1u << w) - 1; size_t nc = (size_t)1 << w;
    uint32_t *cnt = calloc((size_t)np * (nc + 1), sizeof(uint32_t));
    if (!cnt) { quicksort(a, 0, (long)n - 1, 2 * log2i(n)); return; }
    uint32_t *c0 = cnt, *c1 = cnt + (nc + 1), *c2 = cnt + 2 * (nc + 1);
    if (np == 1)      for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; src[i] = k; c0[k + 1]++; }
    else if (np == 2) for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; src[i] = k; c0[(k & mask) + 1]++; c1[(k >> w) + 1]++; }
    else              for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; src[i] = k; c0[(k & mask) + 1]++; c1[((k >> w) & mask) + 1]++; c2[(k >> (2 * w)) + 1]++; }
    for (int p = 0; p < np; p++) { uint32_t *cc = cnt + (size_t)p * (nc + 1); for (size_t b = 1; b <= nc; b++) cc[b] += cc[b - 1]; }
    uint32_t *from = src, *to = dst;
    for (int p = 0; p < np; p++) {
        uint32_t *cc = cnt + (size_t)p * (nc + 1); int sh = p * w;
        if (p == np - 1) { for (size_t i = 0; i < n; i++) { uint32_t k = from[i]; a[cc[(k >> sh) & mask]++] = (double)(int32_t)((k + mn) ^ 0x80000000u); } }
        else { for (size_t i = 0; i < n; i++) { if (i + PF_DIST < n) PREFETCH_W(to + cc[(from[i + PF_DIST] >> sh) & mask]); uint32_t k = from[i]; to[cc[(k >> sh) & mask]++] = k; }
               uint32_t *tmp = from; from = to; to = tmp; }
    }
    free(cnt);
}
#else
static void lsd_radix_int32(double *a, size_t n, double *buf, ctx_t *c) {
    uint32_t *src = c->aux, *dst = (uint32_t *)buf;
    uint32_t *c0 = c->hist[0], *c1 = c->hist[1], *c2 = c->hist[2], *c3 = c->hist[3];
    memset(c0, 0, 257 * 4); memset(c1, 0, 257 * 4); memset(c2, 0, 257 * 4); memset(c3, 0, 257 * 4);
    uint32_t or_a = 0, and_a = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(int32_t)a[i] ^ 0x80000000u; src[i] = v; or_a |= v; and_a &= v;
        c0[(v & 0xFF) + 1]++; c1[((v >> 8) & 0xFF) + 1]++; c2[((v >> 16) & 0xFF) + 1]++; c3[((v >> 24) & 0xFF) + 1]++;
    }
    uint32_t diff = or_a ^ and_a;
    int a0 = (diff & 0xFF) != 0, a1 = (diff & 0xFF00) != 0, a2 = (diff & 0xFF0000) != 0, a3 = (diff & 0xFF000000u) != 0;
    int last = a3 ? 3 : a2 ? 2 : a1 ? 1 : a0 ? 0 : -1;
    if (last < 0) { for (size_t i = 0; i < n; i++) a[i] = (double)(int32_t)(src[i] ^ 0x80000000u); return; }
    uint32_t *from = src, *to = dst, *tmp;
#define RPASS(cc, sh, act, idx) \
    if (act) { for (int b = 1; b < 257; b++) cc[b] += cc[b - 1]; \
        if (last == idx) { for (size_t i = 0; i < n; i++) { uint32_t v = from[i]; a[cc[(v >> sh) & 0xFF]++] = (double)(int32_t)(v ^ 0x80000000u); } return; } \
        for (size_t i = 0; i < n; i++) { if (i + PF_DIST < n) PREFETCH_W(to + cc[(from[i + PF_DIST] >> sh) & 0xFF]); uint32_t v = from[i]; to[cc[(v >> sh) & 0xFF]++] = v; } tmp = from; from = to; to = tmp; }
    RPASS(c0, 0, a0, 0)
    RPASS(c1, 8, a1, 1)
    RPASS(c2, 16, a2, 2)
    for (int b = 1; b < 257; b++) c3[b] += c3[b - 1];
    for (size_t i = 0; i < n; i++) { uint32_t v = from[i]; a[c3[(v >> 24) & 0xFF]++] = (double)(int32_t)(v ^ 0x80000000u); }
#undef RPASS
}

#endif

/* ---- float distribution paths ------------------------------------------- */

/* ---- AVX2 vectorized leaf: compress-store partition + bitonic networks ------
 * The x86-simd-sort design, in-house and AVX2-only (4 doubles per vector): an
 * in-place quicksort whose partition splits each vector with one compare, one
 * table-driven permute and two plain stores (the permute packs left-going
 * lanes to the front and right-going lanes to the back, so the same vector is
 * stored at the left cursor and at the right cursor minus four; the lanes that
 * land wrong are garbage in already-consumed space and are overwritten later).
 * AV_UNROLL vectors are loaded per end-choice so the data-dependent "which end
 * next" branch is paid once per 32 elements, not per four (two vectors per
 * end below 512 elements). Base case: 32- or 64-element bitonic
 * networks as straight-line code (generated: gen_avx2_leaf.py).
 * It takes only the UNSTRUCTURED leaf: the places sort_fn would run flash /
 * coarse / bits-coarse (and leaf_f64's skewed branch). Counting, int32 radix,
 * quantized radix, run relocation / merge, momentum and the gap split keep
 * their paths, which is the difference from the x86-simd-sort hook
 * (g_simd_f64), which replaces them all. Runtime-gated on cpuid (avx2+fma)
 * and LOGOS_NO_AVX2=1 in the environment; -DLOGOS_NO_AVX2_LEAF compiles it
 * out; -DAVX2_LEAF_MAX_N=N caps the size it takes whole (above it coarse
 * partitions first and hands its buckets down); -DLOGOS_AVX2_INT32 also routes
 * int32-valued buckets here instead of the LSD radix. Declines (returns 0)
 * when the range holds a NaN. A/B: make simdbench. */
#if !defined(LOGOS_NO_AVX2_LEAF) && defined(__GNUC__) && defined(__x86_64__)
#define LOGOS_AVX2_LEAF 1
#ifndef AVX2_LEAF_MAX_N
#define AVX2_LEAF_MAX_N SIZE_MAX
#endif
/* Policy (measured, make simdbench): the AVX2 leaf takes a range only in a SEQUENTIAL sort and
 * only when sort_fn's inversion probe did not call the data nearly sorted. In the parallel leaf
 * (33k-element L2-resident buckets, 14 threads on a hybrid P+E CPU) it lost 5-28% on the random
 * shapes; single-threaded it won 1.2-1.6x. On nearly-sorted data coarse's near-sequential
 * scatter beats a quicksort by 1.23x. -DLOGOS_AVX2_PARALLEL lifts the thread gate. */
#ifdef LOGOS_AVX2_PARALLEL
#define AVX2_TAKE(c, n) (!(c)->nearly && !(c)->lowdisc && avx2_leaf_on() && (size_t)(n) <= AVX2_LEAF_MAX_N)
#else
#define AVX2_TAKE(c, n) ((c)->seq && !(c)->nearly && !(c)->lowdisc && avx2_leaf_on() && (size_t)(n) <= AVX2_LEAF_MAX_N)
#endif
#ifndef AV_UNROLL
#define AV_UNROLL 8                /* measured: 4 and 16 both ~10% slower (probe) */
#endif
static int g_avx2 = -1;
static inline int avx2_leaf_on(void) { int v = g_avx2; if (v < 0) { v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && !getenv("LOGOS_NO_AVX2"); g_avx2 = v; } return v; }
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#include <immintrin.h>
static const uint32_t av_lut[16][8] __attribute__((aligned(32))) = {
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 2, 3, 4, 5, 6, 7, 0, 1 },
    { 0, 1, 4, 5, 6, 7, 2, 3 },
    { 4, 5, 6, 7, 0, 1, 2, 3 },
    { 0, 1, 2, 3, 6, 7, 4, 5 },
    { 2, 3, 6, 7, 0, 1, 4, 5 },
    { 0, 1, 6, 7, 2, 3, 4, 5 },
    { 6, 7, 0, 1, 2, 3, 4, 5 },
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 2, 3, 4, 5, 0, 1, 6, 7 },
    { 0, 1, 4, 5, 2, 3, 6, 7 },
    { 4, 5, 0, 1, 2, 3, 6, 7 },
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 2, 3, 0, 1, 4, 5, 6, 7 },
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 0, 1, 2, 3, 4, 5, 6, 7 }
};
static const int64_t av_kmask[5][4] __attribute__((aligned(32))) = { { 0, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, -1, 0, 0 }, { -1, -1, -1, 0 }, { -1, -1, -1, -1 } };
static const int64_t av_kmask_hi[5][4] __attribute__((aligned(32))) = { { 0, 0, 0, 0 }, { 0, 0, 0, -1 }, { 0, 0, -1, -1 }, { 0, -1, -1, -1 }, { -1, -1, -1, -1 } };
#define AV_CE(v, t, m) ((v) = _mm256_blend_pd(_mm256_min_pd((v), (t)), _mm256_max_pd((v), (t)), (m)))
static inline __attribute__((always_inline)) __m256d av_rev(__m256d v) { return _mm256_permute4x64_pd(v, 0x1B); }
static inline __attribute__((always_inline)) __m256d av_sort1(__m256d v) {      /* 4 lanes -> ascending */
    __m256d t;
    t = _mm256_permute_pd(v, 0x5); AV_CE(v, t, 0xA);
    t = av_rev(v);                 AV_CE(v, t, 0xC);
    t = _mm256_permute_pd(v, 0x5); AV_CE(v, t, 0xA);
    return v;
}
static inline __attribute__((always_inline)) __m256d av_merge1(__m256d v) {     /* 4-lane bitonic -> ascending */
    __m256d t;
    t = _mm256_permute4x64_pd(v, 0x4E); AV_CE(v, t, 0xC);
    t = _mm256_permute_pd(v, 0x5);      AV_CE(v, t, 0xA);
    return v;
}
static void av_sortnet32(double *a, size_t n) {          /* n <= 32, no NaN; padded with +inf */
    __m256d v0, v1, v2, v3, v4, v5, v6, v7, t; double tmp[32] __attribute__((aligned(32)));
    memcpy(tmp, a, n * sizeof(double)); for (size_t i = n; i < 32; i++) tmp[i] = INFINITY;
    v0 = _mm256_load_pd(tmp + 0); v1 = _mm256_load_pd(tmp + 4); v2 = _mm256_load_pd(tmp + 8); v3 = _mm256_load_pd(tmp + 12); v4 = _mm256_load_pd(tmp + 16); v5 = _mm256_load_pd(tmp + 20); v6 = _mm256_load_pd(tmp + 24); v7 = _mm256_load_pd(tmp + 28);
    v0 = av_sort1(v0);
    v1 = av_sort1(v1);
    v2 = av_sort1(v2);
    v3 = av_sort1(v3);
    v4 = av_sort1(v4);
    v5 = av_sort1(v5);
    v6 = av_sort1(v6);
    v7 = av_sort1(v7);
    v1 = av_rev(v1);
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v3 = av_rev(v3);
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    v5 = av_rev(v5);
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    t = v2; v2 = v3; v3 = t;
    v2 = av_rev(v2);
    v3 = av_rev(v3);
    t = _mm256_min_pd(v0, v2); v2 = _mm256_max_pd(v0, v2); v0 = t;
    t = _mm256_min_pd(v1, v3); v3 = _mm256_max_pd(v1, v3); v1 = t;
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    t = v6; v6 = v7; v7 = t;
    v6 = av_rev(v6);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v4, v6); v6 = _mm256_max_pd(v4, v6); v4 = t;
    t = _mm256_min_pd(v5, v7); v7 = _mm256_max_pd(v5, v7); v5 = t;
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    t = v4; v4 = v7; v7 = t;
    t = v5; v5 = v6; v6 = t;
    v4 = av_rev(v4);
    v5 = av_rev(v5);
    v6 = av_rev(v6);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v0, v4); v4 = _mm256_max_pd(v0, v4); v0 = t;
    t = _mm256_min_pd(v1, v5); v5 = _mm256_max_pd(v1, v5); v1 = t;
    t = _mm256_min_pd(v2, v6); v6 = _mm256_max_pd(v2, v6); v2 = t;
    t = _mm256_min_pd(v3, v7); v7 = _mm256_max_pd(v3, v7); v3 = t;
    t = _mm256_min_pd(v0, v2); v2 = _mm256_max_pd(v0, v2); v0 = t;
    t = _mm256_min_pd(v1, v3); v3 = _mm256_max_pd(v1, v3); v1 = t;
    t = _mm256_min_pd(v4, v6); v6 = _mm256_max_pd(v4, v6); v4 = t;
    t = _mm256_min_pd(v5, v7); v7 = _mm256_max_pd(v5, v7); v5 = t;
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    _mm256_store_pd(tmp + 0, v0); _mm256_store_pd(tmp + 4, v1); _mm256_store_pd(tmp + 8, v2); _mm256_store_pd(tmp + 12, v3); _mm256_store_pd(tmp + 16, v4); _mm256_store_pd(tmp + 20, v5); _mm256_store_pd(tmp + 24, v6); _mm256_store_pd(tmp + 28, v7);
    memcpy(a, tmp, n * sizeof(double));
}
static void av_sortnet64(double *a, size_t n) {          /* n <= 64, no NaN; padded with +inf */
    __m256d v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, t; double tmp[64] __attribute__((aligned(32)));
    memcpy(tmp, a, n * sizeof(double)); for (size_t i = n; i < 64; i++) tmp[i] = INFINITY;
    v0 = _mm256_load_pd(tmp + 0); v1 = _mm256_load_pd(tmp + 4); v2 = _mm256_load_pd(tmp + 8); v3 = _mm256_load_pd(tmp + 12); v4 = _mm256_load_pd(tmp + 16); v5 = _mm256_load_pd(tmp + 20); v6 = _mm256_load_pd(tmp + 24); v7 = _mm256_load_pd(tmp + 28); v8 = _mm256_load_pd(tmp + 32); v9 = _mm256_load_pd(tmp + 36); v10 = _mm256_load_pd(tmp + 40); v11 = _mm256_load_pd(tmp + 44); v12 = _mm256_load_pd(tmp + 48); v13 = _mm256_load_pd(tmp + 52); v14 = _mm256_load_pd(tmp + 56); v15 = _mm256_load_pd(tmp + 60);
    v0 = av_sort1(v0);
    v1 = av_sort1(v1);
    v2 = av_sort1(v2);
    v3 = av_sort1(v3);
    v4 = av_sort1(v4);
    v5 = av_sort1(v5);
    v6 = av_sort1(v6);
    v7 = av_sort1(v7);
    v8 = av_sort1(v8);
    v9 = av_sort1(v9);
    v10 = av_sort1(v10);
    v11 = av_sort1(v11);
    v12 = av_sort1(v12);
    v13 = av_sort1(v13);
    v14 = av_sort1(v14);
    v15 = av_sort1(v15);
    v1 = av_rev(v1);
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v3 = av_rev(v3);
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    v5 = av_rev(v5);
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    v9 = av_rev(v9);
    t = _mm256_min_pd(v8, v9); v9 = _mm256_max_pd(v8, v9); v8 = t;
    v8 = av_merge1(v8);
    v9 = av_merge1(v9);
    v11 = av_rev(v11);
    t = _mm256_min_pd(v10, v11); v11 = _mm256_max_pd(v10, v11); v10 = t;
    v10 = av_merge1(v10);
    v11 = av_merge1(v11);
    v13 = av_rev(v13);
    t = _mm256_min_pd(v12, v13); v13 = _mm256_max_pd(v12, v13); v12 = t;
    v12 = av_merge1(v12);
    v13 = av_merge1(v13);
    v15 = av_rev(v15);
    t = _mm256_min_pd(v14, v15); v15 = _mm256_max_pd(v14, v15); v14 = t;
    v14 = av_merge1(v14);
    v15 = av_merge1(v15);
    t = v2; v2 = v3; v3 = t;
    v2 = av_rev(v2);
    v3 = av_rev(v3);
    t = _mm256_min_pd(v0, v2); v2 = _mm256_max_pd(v0, v2); v0 = t;
    t = _mm256_min_pd(v1, v3); v3 = _mm256_max_pd(v1, v3); v1 = t;
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    t = v6; v6 = v7; v7 = t;
    v6 = av_rev(v6);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v4, v6); v6 = _mm256_max_pd(v4, v6); v4 = t;
    t = _mm256_min_pd(v5, v7); v7 = _mm256_max_pd(v5, v7); v5 = t;
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    t = v10; v10 = v11; v11 = t;
    v10 = av_rev(v10);
    v11 = av_rev(v11);
    t = _mm256_min_pd(v8, v10); v10 = _mm256_max_pd(v8, v10); v8 = t;
    t = _mm256_min_pd(v9, v11); v11 = _mm256_max_pd(v9, v11); v9 = t;
    t = _mm256_min_pd(v8, v9); v9 = _mm256_max_pd(v8, v9); v8 = t;
    t = _mm256_min_pd(v10, v11); v11 = _mm256_max_pd(v10, v11); v10 = t;
    v8 = av_merge1(v8);
    v9 = av_merge1(v9);
    v10 = av_merge1(v10);
    v11 = av_merge1(v11);
    t = v14; v14 = v15; v15 = t;
    v14 = av_rev(v14);
    v15 = av_rev(v15);
    t = _mm256_min_pd(v12, v14); v14 = _mm256_max_pd(v12, v14); v12 = t;
    t = _mm256_min_pd(v13, v15); v15 = _mm256_max_pd(v13, v15); v13 = t;
    t = _mm256_min_pd(v12, v13); v13 = _mm256_max_pd(v12, v13); v12 = t;
    t = _mm256_min_pd(v14, v15); v15 = _mm256_max_pd(v14, v15); v14 = t;
    v12 = av_merge1(v12);
    v13 = av_merge1(v13);
    v14 = av_merge1(v14);
    v15 = av_merge1(v15);
    t = v4; v4 = v7; v7 = t;
    t = v5; v5 = v6; v6 = t;
    v4 = av_rev(v4);
    v5 = av_rev(v5);
    v6 = av_rev(v6);
    v7 = av_rev(v7);
    t = _mm256_min_pd(v0, v4); v4 = _mm256_max_pd(v0, v4); v0 = t;
    t = _mm256_min_pd(v1, v5); v5 = _mm256_max_pd(v1, v5); v1 = t;
    t = _mm256_min_pd(v2, v6); v6 = _mm256_max_pd(v2, v6); v2 = t;
    t = _mm256_min_pd(v3, v7); v7 = _mm256_max_pd(v3, v7); v3 = t;
    t = _mm256_min_pd(v0, v2); v2 = _mm256_max_pd(v0, v2); v0 = t;
    t = _mm256_min_pd(v1, v3); v3 = _mm256_max_pd(v1, v3); v1 = t;
    t = _mm256_min_pd(v4, v6); v6 = _mm256_max_pd(v4, v6); v4 = t;
    t = _mm256_min_pd(v5, v7); v7 = _mm256_max_pd(v5, v7); v5 = t;
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    t = v12; v12 = v15; v15 = t;
    t = v13; v13 = v14; v14 = t;
    v12 = av_rev(v12);
    v13 = av_rev(v13);
    v14 = av_rev(v14);
    v15 = av_rev(v15);
    t = _mm256_min_pd(v8, v12); v12 = _mm256_max_pd(v8, v12); v8 = t;
    t = _mm256_min_pd(v9, v13); v13 = _mm256_max_pd(v9, v13); v9 = t;
    t = _mm256_min_pd(v10, v14); v14 = _mm256_max_pd(v10, v14); v10 = t;
    t = _mm256_min_pd(v11, v15); v15 = _mm256_max_pd(v11, v15); v11 = t;
    t = _mm256_min_pd(v8, v10); v10 = _mm256_max_pd(v8, v10); v8 = t;
    t = _mm256_min_pd(v9, v11); v11 = _mm256_max_pd(v9, v11); v9 = t;
    t = _mm256_min_pd(v12, v14); v14 = _mm256_max_pd(v12, v14); v12 = t;
    t = _mm256_min_pd(v13, v15); v15 = _mm256_max_pd(v13, v15); v13 = t;
    t = _mm256_min_pd(v8, v9); v9 = _mm256_max_pd(v8, v9); v8 = t;
    t = _mm256_min_pd(v10, v11); v11 = _mm256_max_pd(v10, v11); v10 = t;
    t = _mm256_min_pd(v12, v13); v13 = _mm256_max_pd(v12, v13); v12 = t;
    t = _mm256_min_pd(v14, v15); v15 = _mm256_max_pd(v14, v15); v14 = t;
    v8 = av_merge1(v8);
    v9 = av_merge1(v9);
    v10 = av_merge1(v10);
    v11 = av_merge1(v11);
    v12 = av_merge1(v12);
    v13 = av_merge1(v13);
    v14 = av_merge1(v14);
    v15 = av_merge1(v15);
    t = v8; v8 = v15; v15 = t;
    t = v9; v9 = v14; v14 = t;
    t = v10; v10 = v13; v13 = t;
    t = v11; v11 = v12; v12 = t;
    v8 = av_rev(v8);
    v9 = av_rev(v9);
    v10 = av_rev(v10);
    v11 = av_rev(v11);
    v12 = av_rev(v12);
    v13 = av_rev(v13);
    v14 = av_rev(v14);
    v15 = av_rev(v15);
    t = _mm256_min_pd(v0, v8); v8 = _mm256_max_pd(v0, v8); v0 = t;
    t = _mm256_min_pd(v1, v9); v9 = _mm256_max_pd(v1, v9); v1 = t;
    t = _mm256_min_pd(v2, v10); v10 = _mm256_max_pd(v2, v10); v2 = t;
    t = _mm256_min_pd(v3, v11); v11 = _mm256_max_pd(v3, v11); v3 = t;
    t = _mm256_min_pd(v4, v12); v12 = _mm256_max_pd(v4, v12); v4 = t;
    t = _mm256_min_pd(v5, v13); v13 = _mm256_max_pd(v5, v13); v5 = t;
    t = _mm256_min_pd(v6, v14); v14 = _mm256_max_pd(v6, v14); v6 = t;
    t = _mm256_min_pd(v7, v15); v15 = _mm256_max_pd(v7, v15); v7 = t;
    t = _mm256_min_pd(v0, v4); v4 = _mm256_max_pd(v0, v4); v0 = t;
    t = _mm256_min_pd(v1, v5); v5 = _mm256_max_pd(v1, v5); v1 = t;
    t = _mm256_min_pd(v2, v6); v6 = _mm256_max_pd(v2, v6); v2 = t;
    t = _mm256_min_pd(v3, v7); v7 = _mm256_max_pd(v3, v7); v3 = t;
    t = _mm256_min_pd(v8, v12); v12 = _mm256_max_pd(v8, v12); v8 = t;
    t = _mm256_min_pd(v9, v13); v13 = _mm256_max_pd(v9, v13); v9 = t;
    t = _mm256_min_pd(v10, v14); v14 = _mm256_max_pd(v10, v14); v10 = t;
    t = _mm256_min_pd(v11, v15); v15 = _mm256_max_pd(v11, v15); v11 = t;
    t = _mm256_min_pd(v0, v2); v2 = _mm256_max_pd(v0, v2); v0 = t;
    t = _mm256_min_pd(v1, v3); v3 = _mm256_max_pd(v1, v3); v1 = t;
    t = _mm256_min_pd(v4, v6); v6 = _mm256_max_pd(v4, v6); v4 = t;
    t = _mm256_min_pd(v5, v7); v7 = _mm256_max_pd(v5, v7); v5 = t;
    t = _mm256_min_pd(v8, v10); v10 = _mm256_max_pd(v8, v10); v8 = t;
    t = _mm256_min_pd(v9, v11); v11 = _mm256_max_pd(v9, v11); v9 = t;
    t = _mm256_min_pd(v12, v14); v14 = _mm256_max_pd(v12, v14); v12 = t;
    t = _mm256_min_pd(v13, v15); v15 = _mm256_max_pd(v13, v15); v13 = t;
    t = _mm256_min_pd(v0, v1); v1 = _mm256_max_pd(v0, v1); v0 = t;
    t = _mm256_min_pd(v2, v3); v3 = _mm256_max_pd(v2, v3); v2 = t;
    t = _mm256_min_pd(v4, v5); v5 = _mm256_max_pd(v4, v5); v4 = t;
    t = _mm256_min_pd(v6, v7); v7 = _mm256_max_pd(v6, v7); v6 = t;
    t = _mm256_min_pd(v8, v9); v9 = _mm256_max_pd(v8, v9); v8 = t;
    t = _mm256_min_pd(v10, v11); v11 = _mm256_max_pd(v10, v11); v10 = t;
    t = _mm256_min_pd(v12, v13); v13 = _mm256_max_pd(v12, v13); v12 = t;
    t = _mm256_min_pd(v14, v15); v15 = _mm256_max_pd(v14, v15); v14 = t;
    v0 = av_merge1(v0);
    v1 = av_merge1(v1);
    v2 = av_merge1(v2);
    v3 = av_merge1(v3);
    v4 = av_merge1(v4);
    v5 = av_merge1(v5);
    v6 = av_merge1(v6);
    v7 = av_merge1(v7);
    v8 = av_merge1(v8);
    v9 = av_merge1(v9);
    v10 = av_merge1(v10);
    v11 = av_merge1(v11);
    v12 = av_merge1(v12);
    v13 = av_merge1(v13);
    v14 = av_merge1(v14);
    v15 = av_merge1(v15);
    _mm256_store_pd(tmp + 0, v0); _mm256_store_pd(tmp + 4, v1); _mm256_store_pd(tmp + 8, v2); _mm256_store_pd(tmp + 12, v3); _mm256_store_pd(tmp + 16, v4); _mm256_store_pd(tmp + 20, v5); _mm256_store_pd(tmp + 24, v6); _mm256_store_pd(tmp + 28, v7); _mm256_store_pd(tmp + 32, v8); _mm256_store_pd(tmp + 36, v9); _mm256_store_pd(tmp + 40, v10); _mm256_store_pd(tmp + 44, v11); _mm256_store_pd(tmp + 48, v12); _mm256_store_pd(tmp + 52, v13); _mm256_store_pd(tmp + 56, v14); _mm256_store_pd(tmp + 60, v15);
    memcpy(a, tmp, n * sizeof(double));
}
/* one vector, plain stores: valid only while both free windows hold >= 4 slots */
static inline __attribute__((always_inline)) void av_part_vec(double *a, size_t *l, size_t *r, __m256d v, __m256d piv, int gt, __m256d *mn, __m256d *mx) {
    __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
    int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
    __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
    _mm256_storeu_pd(a + *l, p);
    _mm256_storeu_pd(a + *r - 4, p);
    *l += (size_t)(4 - k); *r -= (size_t)k;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}
/* one vector, masked stores: exact, for the reserved vectors at the end */
static inline __attribute__((always_inline)) void av_part_vec_m(double *a, size_t *l, size_t *r, __m256d v, __m256d piv, int gt, __m256d *mn, __m256d *mx) {
    __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
    int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
    __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
    _mm256_maskstore_pd(a + *l, _mm256_load_si256((const __m256i *)av_kmask[4 - k]), p);       /* first 4-k lanes */
    _mm256_maskstore_pd(a + *r - 4, _mm256_load_si256((const __m256i *)av_kmask_hi[k]), p);    /* last k lanes land at r-k..r */
    *l += (size_t)(4 - k); *r -= (size_t)k;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}
#ifndef AV_SMALL_N
#define AV_SMALL_N 512
#endif
/* small ranges (< AV_SMALL_N, L1-resident): stream through the input once, writing every permuted
 * vector to both ends of a stack buffer, then copy back. No end-choice branch, no reserved vectors;
 * the last two vectors use masked stores because their two stores can overlap. */
static size_t av_partition_small(double *a, size_t n, double pivot, int gt, double *smallest, double *biggest) {
    double tmp[AV_SMALL_N] __attribute__((aligned(32)));
    __m256d piv = _mm256_set1_pd(pivot), mn = _mm256_set1_pd(*smallest), mx = _mm256_set1_pd(*biggest);
    size_t l = 0, r = n, i = 0, nv = n >> 2;
    for (; i + 2 < nv; i++) {
        __m256d v = _mm256_loadu_pd(a + 4 * i);
        __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
        int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
        __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
        _mm256_storeu_pd(tmp + l, p); _mm256_storeu_pd(tmp + r - 4, p);
        l += (size_t)(4 - k); r -= (size_t)k;
        mn = _mm256_min_pd(mn, v); mx = _mm256_max_pd(mx, v);
    }
    for (; i < nv; i++) { __m256d v = _mm256_loadu_pd(a + 4 * i); av_part_vec_m(tmp, &l, &r, v, piv, gt, &mn, &mx); }
    double sm = *smallest, bg = *biggest;
    for (i = nv << 2; i < n; i++) {                            /* scalar tail into the middle gap */
        double x = a[i]; if (x < sm) sm = x; if (x > bg) bg = x;
        int right = gt ? (x > pivot) : (x >= pivot);
        if (right) tmp[--r] = x; else tmp[l++] = x;
    }
    memcpy(a, tmp, n * sizeof(double));
    double t[4]; _mm256_storeu_pd(t, mn); for (int q = 0; q < 4; q++) if (t[q] < sm) sm = t[q];
    _mm256_storeu_pd(t, mx); for (int q = 0; q < 4; q++) if (t[q] > bg) bg = t[q];
    *smallest = sm; *biggest = bg; return l;
}
/* partitions a[lo..hi): returns the first index of the right part; tracks min and max of the range */
static size_t av_partition(double *a, size_t lo, size_t hi, double pivot, int gt, double *smallest, double *biggest) {
    if (hi - lo < AV_SMALL_N) return lo + av_partition_small(a + lo, hi - lo, pivot, gt, smallest, biggest);
    double sm = *smallest, bg = *biggest;
    while ((hi - lo) & 3) {                                   /* scalar until the length is a multiple of 4 */
        double x = a[lo]; if (x < sm) sm = x; if (x > bg) bg = x;
        int right = gt ? (x > pivot) : (x >= pivot);
        if (right) { hi--; a[lo] = a[hi]; a[hi] = x; } else lo++;
    }
    if (hi == lo) { *smallest = sm; *biggest = bg; return lo; }
    __m256d piv = _mm256_set1_pd(pivot), mn = _mm256_set1_pd(sm), mx = _mm256_set1_pd(bg);
    size_t l = lo, r = hi;
    /* U reserved vectors per end (so stores never overrun unread data), U loads per end-choice */
#define AV_PART_UNROLLED(U) do {         __m256d vl[U], vr[U];         for (int i = 0; i < (U); i++) { vl[i] = _mm256_loadu_pd(a + lo + 4 * i); vr[i] = _mm256_loadu_pd(a + hi - 4 * (U) + 4 * i); }         size_t left = lo + 4 * (U), right = hi - 4 * (U);         while (right - left >= 4 * (U)) {             __m256d v[U];             if (r - right < left - l) { right -= 4 * (U); for (int i = 0; i < (U); i++) v[i] = _mm256_loadu_pd(a + right + 4 * i); }             else                      { for (int i = 0; i < (U); i++) v[i] = _mm256_loadu_pd(a + left + 4 * i); left += 4 * (U); }             for (int i = 0; i < (U); i++) av_part_vec(a, &l, &r, v[i], piv, gt, &mn, &mx);         }         while (right > left) {                                /* remainder, one vector at a time (windows stay >= 4) */             __m256d v;             if (r - right < left - l) { right -= 4; v = _mm256_loadu_pd(a + right); }             else                      { v = _mm256_loadu_pd(a + left); left += 4; }             av_part_vec(a, &l, &r, v, piv, gt, &mn, &mx);         }         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vl[i], piv, gt, &mn, &mx);         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vr[i], piv, gt, &mn, &mx); } while (0)
    if (hi - lo == 4)      { __m256d v = _mm256_loadu_pd(a + lo); av_part_vec_m(a, &l, &r, v, piv, gt, &mn, &mx); }
    else if (hi - lo < 16) AV_PART_UNROLLED(1);
    else if (hi - lo < 512) AV_PART_UNROLLED(2);
    else AV_PART_UNROLLED(AV_UNROLL);
#undef AV_PART_UNROLLED
    double t[4]; _mm256_storeu_pd(t, mn); for (int i = 0; i < 4; i++) if (t[i] < sm) sm = t[i];
    _mm256_storeu_pd(t, mx); for (int i = 0; i < 4; i++) if (t[i] > bg) bg = t[i];
    *smallest = sm; *biggest = bg; return l;
}
#ifndef AV_PIV_SMALL
#define AV_PIV_SMALL 512
#endif
static double av_pivot(const double *a, size_t n) {
#ifdef AV_PIVOT_NET
    /* OPT-IN, measured neutral (make chaosbench: 0.94-1.06x at 1T, noise): 64 strided samples sorted by
       the bitonic network (~120 ns) as the pivot instead of 15 insertion-sorted samples. */
    if (n >= 4096) {
        double s[64] __attribute__((aligned(32)));
        for (int i = 0; i < 64; i++) s[i] = a[(size_t)((double)(n - 1) * (i + 0.5) / 64)];
        av_sortnet64(s, 64);
        return s[32];
    }
#endif
    if (n < AV_PIV_SMALL) { double x = a[0], y = a[n / 2], z = a[n - 1]; return x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y)); }
    int k = n < 65536 ? 15 : 63; double s[63];
    for (int i = 0; i < k; i++) s[i] = a[(size_t)((double)(n - 1) * (i + 0.5) / k)];
    for (int i = 1; i < k; i++) { double v = s[i]; int j = i; while (j > 0 && s[j - 1] > v) { s[j] = s[j - 1]; j--; } s[j] = v; }
    return s[k / 2];
}
#ifdef AV_STATS
static size_t av_stat_calls, av_stat_elems;
#define AV_STAT(n) (av_stat_calls++, av_stat_elems += (n))
#else
#define AV_STAT(n) ((void)0)
#endif
static void av_qsort_rec(double *a, size_t n, int depth) {
    while (n > 64) {
        if (depth-- == 0) { quicksort(a, 0, (long)n - 1, 2 * log2i(n)); return; }
        AV_STAT(n);
        double sm = INFINITY, bg = -INFINITY, p = av_pivot(a, n);
        size_t m = av_partition(a, 0, n, p, 0, &sm, &bg);
        if (sm == bg) return;                                  /* all equal */
        if (m == 0) {                                          /* pivot is the minimum: split off the run of equals */
            m = av_partition(a, 0, n, p, 1, &sm, &bg);
            a += m; n -= m; continue;
        }
        if (p == bg) { n = m; continue; }                      /* right part is all == pivot */
        if (m < n - m) { av_qsort_rec(a, m, depth); a += m; n -= m; }
        else           { av_qsort_rec(a + m, n - m, depth); n = m; }
    }
    if (n > 32) av_sortnet64(a, n); else if (n > 1) av_sortnet32(a, n);   /* a 128 network measured 3.0 vs 1.9 ns/elem: one more level + 64 is cheaper */
}
static inline int av_has_nan(const double *a, size_t n) {
    __m256d acc = _mm256_setzero_pd(); size_t i = 0;
    for (; i + 4 <= n; i += 4) { __m256d v = _mm256_loadu_pd(a + i); acc = _mm256_or_pd(acc, _mm256_cmp_pd(v, v, _CMP_UNORD_Q)); }
    int any = _mm256_movemask_pd(acc);
    for (; i < n; i++) any |= (a[i] != a[i]);
    return any != 0;
}
#ifndef LOGOS_ZERO_SIGN_UNSAFE
/* NaN (bit 0) and -0.0 (bit 1), at the price of the old NaN-only scan. The first version added a second compare, an AND
 * and an OR per block and cost 7% of a 1-thread sort on uniform and heavy_tail (tools/ledger_rerun.py, layout cancelled).
 * Now ONE compare per block, "equal to zero or unordered", flags NaN and every zero together, and only when something was
 * flagged does a scalar pass tell them apart. Data holding no zero and no NaN pays exactly what it paid before the fix. */
static int av_scan_nan_negzero(const double *a, size_t n) {
    __m256d acc = _mm256_setzero_pd(), zero = _mm256_setzero_pd(); size_t i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm256_or_pd(acc, _mm256_cmp_pd(_mm256_loadu_pd(a + i), zero, _CMP_EQ_UQ));
    int flagged = _mm256_movemask_pd(acc) != 0;
    for (; i < n; i++) flagged |= (a[i] != a[i]) | (a[i] == 0.0);
    if (!flagged) return 0;
    int nan = 0, negz = 0;
    for (size_t k = 0; k < n; k++) { nan |= (a[k] != a[k]); negz |= (a[k] == 0.0 && signbit(a[k])); }
    return nan | (negz << 1);
}
#endif
/* 1 = sorted in place; 0 = declined (NaN present), caller keeps its scalar path.
 * The networks are min/max, and _mm256_min_pd / _mm256_max_pd return the SAME operand when -0.0 meets +0.0, so a
 * compare-exchange of the two wrote one sign twice and lost the other: the output was not a permutation (harness signed
 * zeros, zerodiag). When a -0.0 is present every zero is made +0.0, the sort runs, and the -0.0 are written back at the
 * start of the zero run, which is contiguous in sorted output and is where -0.0 belongs in total order. The only decline
 * happens before anything is written. -DLOGOS_ZERO_SIGN_UNSAFE restores the old kernel entry for the A/B. */
static int avx2_qsort_f64(double *a, size_t n) {
    if (n < 2) return 1;
#ifdef LOGOS_ZERO_SIGN_UNSAFE
    if (av_has_nan(a, n)) return 0;
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
#else
    int sc = av_scan_nan_negzero(a, n);
    if (sc & 1) return 0;
    size_t nneg = 0;
    if (sc & 2) for (size_t i = 0; i < n; i++) if (a[i] == 0.0 && signbit(a[i])) { a[i] = 0.0; nneg++; }
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
    if (nneg) {
        size_t lo = 0, hi = n;                                         /* first element >= 0.0: the zero run starts here */
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (a[mid] < 0.0) lo = mid + 1; else hi = mid; }
        for (size_t k = 0; k < nneg; k++) a[lo + k] = -0.0;
    }
#endif
    return 1;
}
#pragma GCC pop_options
#endif

/* Flash sort, ping-pong: scatters a -> buf and finishes IN buf. Returns buf. */
/* OPT-IN (measured neutral within +-5%, make theorybench; kept for a quiet-box A/B).
 * Line-granular repair of flash output (H6 lever 2). Flash leaves only intra-class disorder, so
 * nearly every element is within a few positions of home. Pass 1 sorts every aligned line of eight
 * with a branch-free min/max network (19 compare-exchanges, depth 6); pass 2 is an insertion sort
 * whose "already in place" test is now true for ~98% of elements, so its branch predicts instead
 * of mispredicting on ~18% of them. Exact for any input. -DLOGOS_NO_LINE_REPAIR = plain insertion. */
#ifdef LOGOS_LINE_REPAIR
#define CE_(i, j) do { double x_ = a[i], y_ = a[j]; a[i] = x_ < y_ ? x_ : y_; a[j] = x_ < y_ ? y_ : x_; } while (0)
static inline void sort8_net(double *a) {
    CE_(0, 1); CE_(2, 3); CE_(4, 5); CE_(6, 7);
    CE_(0, 2); CE_(1, 3); CE_(4, 6); CE_(5, 7);
    CE_(1, 2); CE_(5, 6); CE_(0, 4); CE_(3, 7);
    CE_(1, 5); CE_(2, 6);
    CE_(1, 4); CE_(3, 6);
    CE_(2, 4); CE_(3, 5);
    CE_(3, 4);
}
#undef CE_
static void line_repair(double *a, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) sort8_net(a + i);
    for (size_t k = 1; k < n; k++) {
        double v = a[k]; if (v >= a[k - 1]) continue;
        size_t j = k; while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; } a[j] = v;
    }
}
#endif
static double *flash_sort(double *a, size_t n, double mn, double mx, double *buf, ctx_t *c) {
    size_t m = n / FLASH_M_DIV; if (m < 2) m = 2; double scale = (double)(m - 1) / (mx - mn);
    uint32_t *counts = c->aux;
    memset(counts, 0, (m + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) { size_t idx = (size_t)((a[i] - mn) * scale); counts[idx + 1]++; }
    for (size_t i = 1; i <= m; i++) counts[i] += counts[i - 1];
    for (size_t i = 0; i < n; i++) { double v = a[i]; size_t idx = (size_t)((v - mn) * scale); buf[counts[idx]++] = v; }
#ifdef LOGOS_LINE_REPAIR
    line_repair(buf, n);
#else
    insertion_sort(buf, 0, (long)n - 1);
#endif
    return buf;
}


/* Write-combining scatter. A plain scatter does one 8-byte write per element to
 * one of K destinations — on a cache-line-based memory system each such write
 * can cost a full 64-byte line read-modify-write. Staging 8 doubles per slot
 * and flushing whole lines turns scattered writes into streaming ones, which is
 * what memory bandwidth (the wall at high thread counts) rewards. Stable: a
 * slot's elements are flushed in arrival order. `H` advances by flushed count.
 * RUNTIME-GATED (ctx->wc_on): measured 8-35% slower sequentially, where the
 * destination lines are L2-resident and the staging is pure overhead, and
 * 5-12% faster at 20 threads where the scatter is DRAM-bound. On when
 * nthreads >= WC_MIN_THREADS. */
#define WC_SCATTER(LO, HI, IDX, SRC) do { \
    if (c->wc_on) { \
        double *wc_ = c->wc; uint8_t *wcn_ = c->wcn; \
        for (long i = (LO); i <= (HI); i++) { size_t b_ = (IDX)[i]; \
            if (i + PF_DIST <= (HI)) PREFETCH_W(buf + H[(IDX)[i + PF_DIST]]); \
            wc_[b_ * 8 + wcn_[b_]] = (SRC)[i]; \
            if (++wcn_[b_] == 8) { memcpy(buf + H[b_], wc_ + b_ * 8, 64); H[b_] += 8; wcn_[b_] = 0; } } \
        for (size_t b_ = 0; b_ < k; b_++) if (wcn_[b_]) { memcpy(buf + H[b_], wc_ + b_ * 8, wcn_[b_] * sizeof(double)); H[b_] += wcn_[b_]; wcn_[b_] = 0; } \
    } else { \
        for (long i = (LO); i <= (HI); i++) { \
            if (i + PF_DIST <= (HI)) PREFETCH_W(buf + H[(IDX)[i + PF_DIST]]); \
            buf[H[(IDX)[i]]++] = (SRC)[i]; } \
    } \
} while (0)
/* Coarse value-bucket sort, PING-PONG. Sorts src[lo..hi] using dst[lo..hi] as
 * scratch and returns the buffer (src or dst) holding the sorted range. Each
 * level scatters src -> dst and recurses with the buffers swapped, so
 * consecutive levels cancel instead of copying back; a level copies only when
 * its children landed in different buffers, and then only the minority. On
 * uniform data (all children the same depth) no copy is made at all — the
 * memcpy-back that was ~37% of all memory traffic is gone. */
static void (*g_simd_f64)(double *, size_t) = NULL;
static void (*g_simd_i32)(int32_t *, size_t) = NULL;
/* A registered SIMD sort is a vectorized network too, with the same -0.0 / +0.0 min/max hazard as the AVX2 leaf, so every
 * call goes through this wrapper and gets the same fix-up. SIMD builds only; the scan is scalar, the price of correctness. */
static void (*g_simd_user_f64)(double *, size_t) = NULL;
static void simd_f64_signed(double *a, size_t n) {
    size_t nneg = 0;
    for (size_t i = 0; i < n; i++) if (a[i] == 0.0 && signbit(a[i])) { a[i] = 0.0; nneg++; }
    g_simd_user_f64(a, n);
    if (nneg) {
        size_t lo = 0, hi = n;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (a[mid] < 0.0) lo = mid + 1; else hi = mid; }
        for (size_t k = 0; k < nneg; k++) a[lo + k] = -0.0;
    }
}
void logos_set_simd_sort(void (*f64)(double *, size_t), void (*i32)(int32_t *, size_t)) { g_simd_user_f64 = f64; g_simd_f64 = f64 ? simd_f64_signed : NULL; g_simd_i32 = i32; }
static void (*g_simd_bkcount)(const double *, long, long, double, double, int, uint16_t *, size_t *) = NULL;
static void (*g_simd_orand)(const double *, size_t, uint64_t *, uint64_t *) = NULL;
void logos_set_simd_kernels(void (*bkc)(const double *, long, long, double, double, int, uint16_t *, size_t *), void (*oa)(const double *, size_t, uint64_t *, uint64_t *)) { g_simd_bkcount = bkc; g_simd_orand = oa; (void)g_simd_bkcount; }  /* bkc measured net-negative in coarse (store+reload vs scatter), left unwired */

static double *coarse_bucket_sort(double *src, double *dst, long lo, long hi, int depth, double mn, double mx, ctx_t *c) {
    long len = hi - lo + 1;
    if (len <= COARSE_SMALL) { insertion_sort(src, lo, hi); return src; }
    if (depth >= COARSE_MAXD) { quicksort(src, lo, hi, 2 * log2i((size_t)len)); return src; }
    if (mn != mn) { mn = src[lo]; mx = src[lo]; for (long i = lo + 1; i <= hi; i++) { double v = src[i]; if (v < mn) mn = v; else if (v > mx) mx = v; } }
    if (mn == mx) return src;
    size_t k = (size_t)len < COARSE_K ? ((size_t)(len >> 1) > 2 ? (size_t)(len >> 1) : 2) : COARSE_K;
    size_t *C = c->C[depth], *S = c->S[depth], *H = c->H[depth];
    double scale = (double)(k - 1) / (mx - mn);
    uint16_t *idx = c->idx; double *a = src, *buf = dst;
    memset(C, 0, k * sizeof(size_t));
    for (long i = lo; i <= hi; i++) { size_t b = (size_t)((a[i] - mn) * scale); if (b >= k) b = k - 1; idx[i] = (uint16_t)b; C[b]++; }
    S[0] = (size_t)lo;
    for (size_t i = 0; i < k; i++) S[i + 1] = S[i] + C[i];
    memcpy(H, S, k * sizeof(size_t));
    WC_SCATTER(lo, hi, idx, a);                                     /* src -> dst */
    uint8_t *land = c->land[depth]; long in_dst = 0, in_src = 0;
    for (size_t b = 0; b < k; b++) {
        long blo = (long)S[b], bhi = (long)S[b + 1] - 1, clen = bhi - blo + 1;
        if (clen <= 1) { land[b] = 1; in_dst += clen; continue; }
        if (clen <= COARSE_SMALL) { insertion_sort(dst, blo, bhi); land[b] = 1; in_dst += clen; continue; }
#ifdef LOGOS_AVX2_LEAF
        if (AVX2_TAKE(c, clen) && avx2_qsort_f64(dst + blo, (size_t)clen)) { land[b] = 1; in_dst += clen; continue; }
#endif
        double *r = coarse_bucket_sort(dst, src, blo, bhi, depth + 1, NAN, NAN, c);   /* swapped */
        if (r == dst) { land[b] = 1; in_dst += clen; } else { land[b] = 0; in_src += clen; }
    }
    if (in_dst >= in_src) {
        for (size_t b = 0; b < k; b++) if (!land[b]) memcpy(dst + S[b], src + S[b], (S[b + 1] - S[b]) * sizeof(double));
        return dst;
    }
    for (size_t b = 0; b < k; b++) if (land[b]) memcpy(src + S[b], dst + S[b], (S[b + 1] - S[b]) * sizeof(double));
    return src;
}

/* IEEE-754 total-order key of the high word: monotonic in value and ~linear in
 * log-magnitude, so heavy tails bucket evenly. */
static inline uint32_t hi_key(double v) { uint64_t u; memcpy(&u, &v, 8); uint32_t h = (uint32_t)(u >> 32); return (h & 0x80000000u) ? ~h : (h | 0x80000000u); }

/* Exponent-bit coarse (heavy tails), same ping-pong contract as coarse_bucket_sort. */

static double *coarse_bits_f64(double *src, double *dst, long lo, long hi, int depth, int have_key, uint32_t mn_key, uint32_t mx_key, ctx_t *c) {
    long len = hi - lo + 1;
    if (g_simd_f64 && depth == 0) { g_simd_f64(src + lo, (size_t)len); return src; }   /* SIMD leaf (heavy tails) */
    if (len <= COARSE_SMALL) { insertion_sort(src, lo, hi); return src; }
    if (depth >= COARSE_MAXD) { quicksort(src, lo, hi, 2 * log2i((size_t)len)); return src; }
    if (!have_key) { mn_key = mx_key = hi_key(src[lo]); for (long i = lo + 1; i <= hi; i++) { uint32_t kk = hi_key(src[i]); if (kk < mn_key) mn_key = kk; else if (kk > mx_key) mx_key = kk; } }
    if (mn_key == mx_key) return coarse_bucket_sort(src, dst, lo, hi, depth, NAN, NAN, c);   /* same high word: values are close; value bucketing */
    size_t k = (size_t)len < COARSE_K ? ((size_t)(len >> 1) > 2 ? (size_t)(len >> 1) : 2) : COARSE_K;
    size_t *C = c->C[depth], *S = c->S[depth], *H = c->H[depth];
    double scale = (double)(k - 1) / ((double)mx_key - (double)mn_key);
    uint16_t *idx = c->idx; double *a = src, *buf = dst;
    memset(C, 0, k * sizeof(size_t));
    for (long i = lo; i <= hi; i++) { size_t b = (size_t)(((double)hi_key(a[i]) - (double)mn_key) * scale); if (b >= k) b = k - 1; idx[i] = (uint16_t)b; C[b]++; }
    S[0] = (size_t)lo;
    for (size_t i = 0; i < k; i++) S[i + 1] = S[i] + C[i];
    memcpy(H, S, k * sizeof(size_t));
    WC_SCATTER(lo, hi, idx, a);
    uint8_t *land = c->land[depth]; long in_dst = 0, in_src = 0;
    for (size_t b = 0; b < k; b++) {
        long blo = (long)S[b], bhi = (long)S[b + 1] - 1, clen = bhi - blo + 1;
        if (clen <= 1) { land[b] = 1; in_dst += clen; continue; }
        if (clen <= COARSE_SMALL) { insertion_sort(dst, blo, bhi); land[b] = 1; in_dst += clen; continue; }
#ifdef LOGOS_AVX2_LEAF
        if (AVX2_TAKE(c, clen) && avx2_qsort_f64(dst + blo, (size_t)clen)) { land[b] = 1; in_dst += clen; continue; }
#endif
        double *r = coarse_bits_f64(dst, src, blo, bhi, depth + 1, 0, 0, 0, c);
        if (r == dst) { land[b] = 1; in_dst += clen; } else { land[b] = 0; in_src += clen; }
    }
    if (in_dst >= in_src) {
        for (size_t b = 0; b < k; b++) if (!land[b]) memcpy(dst + S[b], src + S[b], (S[b + 1] - S[b]) * sizeof(double));
        return dst;
    }
    for (size_t b = 0; b < k; b++) if (land[b]) memcpy(src + S[b], dst + S[b], (S[b + 1] - S[b]) * sizeof(double));
    return src;
}

/* sampled skew probe: max value-linear bin / ideal; >8 => bits keying */
static double skew_probe(const double *a, size_t n) {
    enum { M = 8192, KC = 256 };
    static _Thread_local double samp[M];
    uint32_t seed = (uint32_t)(0x9e3779b9u ^ (uint32_t)n);
    double smn = INFINITY, smx = -INFINITY;
    for (int i = 0; i < M; i++) { seed = seed * 1664525u + 1013904223u; double v = a[(size_t)((double)seed / 4294967296.0 * (double)n)]; samp[i] = v; if (v < smn) smn = v; if (v > smx) smx = v; }
    if (!(smx > smn)) return 1;
    double sc = KC / (smx - smn); int Hh[KC]; memset(Hh, 0, sizeof Hh);
    for (int i = 0; i < M; i++) { long b = (long)((samp[i] - smn) * sc); if (b >= KC) b = KC - 1; else if (b < 0) b = 0; Hh[b]++; }
    int mxb = 0; for (int b = 0; b < KC; b++) if (Hh[b] > mxb) mxb = Hh[b];
    return (double)mxb / ((double)M / KC);
}

/* ---- run detection + galloping merges ----------------------------------- */
/* Flat [start,end,...] run list (malloc'd) or NULL on bail; records the
 * observed average run length in ctx->last_bail_avg_run. Prefix-based bail. */
static size_t *detect_runs_limited(double *a, size_t n, size_t max_runs, size_t min_avg, size_t *n_runs, ctx_t *c) {
    size_t *res = malloc(2 * (max_runs + 2) * sizeof(size_t));
    if (!res) { *n_runs = 0; return NULL; }
    size_t R = 0, i = 0;
    while (i < n) {
        size_t j = i + 1;
        if (j < n) {
            if (a[j] >= a[i]) { while (j < n && a[j] >= a[j - 1]) j++; }
            else { while (j < n && a[j] < a[j - 1]) j++;
                   for (size_t l = i, r = j - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }
        }
        res[2 * R] = i; res[2 * R + 1] = j - 1; R++;
        if (R > max_runs) { c->last_bail_avg_run = (double)j / R; free(res); *n_runs = 0; return NULL; }
        if (min_avg && R >= 256 && j < R * min_avg) { c->last_bail_avg_run = (double)j / R; free(res); *n_runs = 0; return NULL; }
        i = j;
    }
    *n_runs = R; return res;
}

static void merge_gallop(double *a, double *buf, size_t a1, size_t b1, size_t a2, size_t b2) {
    if (a[b1] <= a[a2]) return;
    const int MG0 = 2; size_t p = a1, q = a2, w = a1; int p_wins = 0, q_wins = 0, mg = MG0;
    while (p <= b1 && q <= b2) {
        if (a[p] <= a[q]) { buf[w++] = a[p++]; p_wins++; q_wins = 0; } else { buf[w++] = a[q++]; q_wins++; p_wins = 0; }
        if (p_wins >= mg) { double t = a[q]; size_t p0 = p, s = 1, j = p; while (j + s <= b1 && a[j + s - 1] <= t) { j += s; s <<= 1; }
            size_t L = j, R = (b1 + 1 < j + s) ? b1 + 1 : j + s; while (L < R) { size_t m = (L + R) >> 1; if (a[m] <= t) L = m + 1; else R = m; }
            while (p < L) buf[w++] = a[p++];
            p_wins = 0; if (p - p0 < MIN_GALLOP) mg++; else if (mg > MG0) mg--; }
        else if (q_wins >= mg) { double t = a[p]; size_t q0 = q, s = 1, j = q; while (j + s <= b2 && a[j + s - 1] < t) { j += s; s <<= 1; }
            size_t L = j, R = (b2 + 1 < j + s) ? b2 + 1 : j + s; while (L < R) { size_t m = (L + R) >> 1; if (a[m] < t) L = m + 1; else R = m; }
            while (q < L) buf[w++] = a[q++];
            q_wins = 0; if (q - q0 < MIN_GALLOP) mg++; else if (mg > MG0) mg--; }
    }
    while (p <= b1) buf[w++] = a[p++];
    while (q <= b2) buf[w++] = a[q++];
    memcpy(a + a1, buf + a1, (b2 - a1 + 1) * sizeof(double));
}

/* 4-way tournament merge with galloping on the run that keeps winning */
static void merge_4way_hybrid(double *a, double *buf, size_t a1, size_t b1, size_t a2, size_t b2, size_t a3, size_t b3, size_t a4, size_t b4) {
    if (a[b1] <= a[a2] && a[b2] <= a[a3] && a[b3] <= a[a4]) return;
    const double INF = INFINITY;
    size_t p[4] = { a1, a2, a3, a4 }; const size_t e[4] = { b1, b2, b3, b4 };
    double v[4] = { a[a1], a[a2], a[a3], a[a4] };
    size_t w = a1; int wc[4] = { 0, 0, 0, 0 };
    while (p[0] <= e[0] || p[1] <= e[1] || p[2] <= e[2] || p[3] <= e[3]) {
        double lv = v[0] <= v[1] ? v[0] : v[1]; int lw = v[0] <= v[1] ? 0 : 1;
        double rv = v[2] <= v[3] ? v[2] : v[3]; int rw = v[2] <= v[3] ? 2 : 3;
        int win = lv <= rv ? lw : rw; double wv = lv <= rv ? lv : rv;
        buf[w++] = wv;
        p[win]++; v[win] = p[win] <= e[win] ? a[p[win]] : INF;
        for (int x = 0; x < 4; x++) wc[x] = (x == win) ? wc[x] + 1 : 0;
        if (wc[win] >= MIN_GALLOP) {
            double t = INF; for (int x = 0; x < 4; x++) if (x != win && v[x] < t) t = v[x];
            int strict = (win != 0);                     /* run 0 uses <=, others < (stability of ties) */
            if (t == INF) { while (p[win] <= e[win]) buf[w++] = a[p[win]++]; v[win] = INF; }
            else {
                size_t s = 1, j = p[win], ee = e[win];
                if (strict) { while (j + s <= ee && a[j + s - 1] < t) { j += s; s <<= 1; } }
                else        { while (j + s <= ee && a[j + s - 1] <= t) { j += s; s <<= 1; } }
                size_t L = j, R = (ee + 1 < j + s) ? ee + 1 : j + s;
                if (strict) { while (L < R) { size_t m = (L + R) >> 1; if (a[m] < t) L = m + 1; else R = m; } }
                else        { while (L < R) { size_t m = (L + R) >> 1; if (a[m] <= t) L = m + 1; else R = m; } }
                while (p[win] < L) buf[w++] = a[p[win]++];
                v[win] = p[win] <= e[win] ? a[p[win]] : INF;
            }
            wc[win] = 0;
        }
    }
    memcpy(a + a1, buf + a1, (b4 - a1 + 1) * sizeof(double));
}

static void merge_all_runs(double *a, double *buf, const size_t *runs, size_t n_runs) {
    size_t capw = 2 * (n_runs + 4);
    size_t *A = malloc(capw * sizeof(size_t)), *B = malloc(capw * sizeof(size_t));
    if (!A || !B) { free(A); free(B); return; }
    memcpy(A, runs, 2 * n_runs * sizeof(size_t));
    size_t *cur = A, *next = B, cur_n = n_runs;
    while (cur_n > 1) {
        size_t next_n = 0;
        if (cur_n >= QUADWAY_THRESHOLD) {
            for (size_t k = 0; k < cur_n; k += 4) {
                size_t num = cur_n - k < 4 ? cur_n - k : 4; const size_t *r = cur + 2 * k;
                if (num == 1) { next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[1]; next_n++; continue; }
                if (num == 2) { merge_gallop(a, buf, r[0], r[1], r[2], r[3]); next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[3]; next_n++; continue; }
                if (num == 3) { merge_gallop(a, buf, r[0], r[1], r[2], r[3]); merge_gallop(a, buf, r[0], r[3], r[4], r[5]);
                                next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[5]; next_n++; continue; }
                merge_4way_hybrid(a, buf, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
                next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[7]; next_n++;
            }
        } else {
            for (size_t k = 0; k < cur_n; k += 2) {
                const size_t *r = cur + 2 * k;
                if (k + 1 >= cur_n) { next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[1]; next_n++; continue; }
                merge_gallop(a, buf, r[0], r[1], r[2], r[3]);
                next[2 * next_n] = r[0]; next[2 * next_n + 1] = r[3]; next_n++;
            }
        }
        size_t *t = cur; cur = next; next = t; cur_n = next_n;
    }
    free(A); free(B);
}

/* ---- disjoint-run relocation (J5) --------------------------------------- */
/* Block-permuted data (sorted blocks concatenated out of order: merged log
 * partitions, time-series shards) is a set of runs whose VALUE ranges mostly do
 * not overlap. Merging such runs is comparison work over every element for
 * nothing: sort the run DESCRIPTORS by first value instead, group runs whose
 * ranges overlap into chains, memcpy every run to its chain-ordered place, and
 * merge only inside multi-run chains. Fully disjoint runs cost one copy pass and
 * zero comparisons. Gate: relocate only when the elements inside multi-run
 * chains are at most n/2 (interleaved runs, where every range overlaps every
 * other, stay on the galloping run-merge). Build with -DLOGOS_NO_RELOCATE to
 * get the pre-J5 engine for an interleaved A/B (make j5bench). */
#ifndef LOGOS_NO_RELOCATE
#define RELOC_MIN_N 4096
#ifndef RELOC_MIN_PER_THREAD
#define RELOC_MIN_PER_THREAD 500000
#endif
/* The counting routes (lowcard, modcard, lattice) carve static per-thread ranges and merge P tables; at
   1M elements 4 threads beat 10 by 1.5-1.8x and at 3.16M 9 beat 14 by 1.1-1.4x (make smallbench). At most one
   thread per this many elements. */
#ifndef COUNT_MIN_PER_THREAD
#define COUNT_MIN_PER_THREAD 350000   /* swept 150k/250k/350k (make smallbench, SM_SET=1): 350k best at 3.16M by 1.2x, within 10% of 250k at 1M */
#endif
/* floor of COUNT_MIN_THREADS: at 316k three threads beat one by 1.4x on quantized and few_distinct (the table
   merge is cheap there); the cap is against the P-proportional table cost at 1M and above */
#ifndef COUNT_MIN_THREADS
#define COUNT_MIN_THREADS 3
#endif
static inline int count_threads(size_t n, int P) { size_t pm = n / COUNT_MIN_PER_THREAD; if (pm < COUNT_MIN_THREADS) pm = COUNT_MIN_THREADS; return (size_t)P > pm ? (int)pm : P; }
typedef struct { double first, last; size_t start, len; } rdesc_t;
static int cmp_rdesc(const void *x, const void *y) { double a = ((const rdesc_t *)x)->first, b = ((const rdesc_t *)y)->first; return (a > b) - (a < b); }

/* Sorted descriptors + chains for a flat [start,end,...] run list. chain[k] is
 * the index of chain k's first descriptor (nchain+1 entries, sentinel R);
 * *merged = elements inside chains with more than one run. 0 on alloc failure. */
static int runs_plan(const double *a, const size_t *runs, size_t R, rdesc_t **pd, size_t **pchain, size_t *pnchain, size_t *pmerged) {
    rdesc_t *d = malloc(R * sizeof *d); size_t *ch = malloc((R + 1) * sizeof *ch);
    if (!d || !ch) { free(d); free(ch); return 0; }
    for (size_t k = 0; k < R; k++) { size_t s = runs[2 * k], e = runs[2 * k + 1]; d[k].first = a[s]; d[k].last = a[e]; d[k].start = s; d[k].len = e - s + 1; }
    qsort(d, R, sizeof *d, cmp_rdesc);
    size_t nc = 0, merged = 0, celems = d[0].len, cruns = 1; double cmax = d[0].last; ch[nc++] = 0;
    for (size_t k = 1; k < R; k++) {
        if (d[k].first < cmax) { if (d[k].last > cmax) cmax = d[k].last; celems += d[k].len; cruns++; }   /* overlaps the chain so far */
        else { if (cruns > 1) merged += celems; ch[nc++] = k; cmax = d[k].last; celems = d[k].len; cruns = 1; }
    }
    if (cruns > 1) merged += celems;
    ch[nc] = R; *pd = d; *pchain = ch; *pnchain = nc; *pmerged = merged; return 1;
}

/* Sequential relocation for the leaf dispatcher: copies every run into buf in
 * chain order, then merges inside multi-run chains with a[] as scratch (all
 * runs have already been copied out, so a[] is free). Returns buf, or NULL when
 * the gate declines (caller merges in place as before). */
static double *runs_relocate_f64(double *a, size_t n, double *buf, const size_t *runs, size_t n_runs) {
    rdesc_t *d; size_t *ch, nc, merged;
    if (!runs_plan(a, runs, n_runs, &d, &ch, &nc, &merged)) return NULL;
    if (merged > n / 2) { free(d); free(ch); return NULL; }
    size_t *rl = malloc(2 * n_runs * sizeof(size_t)); if (!rl) { free(d); free(ch); return NULL; }
    for (size_t q = 0, o = 0; q < n_runs; q++) { rl[2 * q] = o; rl[2 * q + 1] = o + d[q].len - 1; memcpy(buf + o, a + d[q].start, d[q].len * sizeof(double)); o += d[q].len; }
    for (size_t k = 0; k < nc; k++) if (ch[k + 1] - ch[k] > 1) merge_all_runs(buf, a, rl + 2 * ch[k], ch[k + 1] - ch[k]);
    free(rl); free(d); free(ch); return buf;
}
#endif

/* ---- quantized-float radix (typed path) --------------------------------- */
static inline void key_parts(double v, uint32_t *tl, uint32_t *th) {
    uint64_t u; memcpy(&u, &v, 8); uint32_t lo = (uint32_t)u, hi = (uint32_t)(u >> 32);
    if (hi >> 31) { *tl = ~lo; *th = ~hi; } else { *tl = lo; *th = hi | 0x80000000u; }
}
static int quant_probe_f64(const double *a, size_t n) {
    size_t m = n < 4096 ? n : 4096, step = n / m;
    uint32_t or_h = 0, and_h = 0xFFFFFFFFu, or_l = 0, and_l = 0xFFFFFFFFu;
    for (size_t i = 0, p = 0; i < m; i++, p += step) { uint32_t tl, th; key_parts(a[p], &tl, &th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; }
    int passes = 0;
    for (int p = 0; p < 8; p++) { int hi_word = p >= 4, sh = (p & 3) << 3; uint32_t o = hi_word ? or_h : or_l, an = hi_word ? and_h : and_l; if (((o >> sh) & 0xFF) != ((an >> sh) & 0xFF)) passes++; }
    return passes;
}
static void radix_f64_full(double *a, size_t n, double *buf, ctx_t *c) {
    if (g_simd_f64) { g_simd_f64(a, n); return; }                                        /* SIMD leaf (quantized) */
    for (int p = 0; p < 8; p++) memset(c->hist[p], 0, 257 * 4);
    uint32_t or_h = 0, and_h = 0xFFFFFFFFu, or_l = 0, and_l = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t tl, th; key_parts(a[i], &tl, &th);
        or_h |= th; and_h &= th; or_l |= tl; and_l &= tl;
        c->hist[0][(tl & 0xFF) + 1]++; c->hist[1][((tl >> 8) & 0xFF) + 1]++; c->hist[2][((tl >> 16) & 0xFF) + 1]++; c->hist[3][((tl >> 24) & 0xFF) + 1]++;
        c->hist[4][(th & 0xFF) + 1]++; c->hist[5][((th >> 8) & 0xFF) + 1]++; c->hist[6][((th >> 16) & 0xFF) + 1]++; c->hist[7][((th >> 24) & 0xFF) + 1]++;
    }
    double *from = a, *to = buf;
    for (int p = 0; p < 8; p++) {
        int hi_word = p >= 4, sh = (p & 3) << 3;
        uint32_t o = hi_word ? or_h : or_l, an = hi_word ? and_h : and_l;
        if (((o >> sh) & 0xFF) == ((an >> sh) & 0xFF)) continue;
        uint32_t *h = c->hist[p];
        for (int b = 1; b < 257; b++) h[b] += h[b - 1];
        for (size_t i = 0; i < n; i++) {
            if (i + PF_DIST < n) { uint32_t tl2, th2; key_parts(from[i + PF_DIST], &tl2, &th2); PREFETCH_W(to + h[((hi_word ? th2 : tl2) >> sh) & 0xFF]); }
            uint32_t tl, th; key_parts(from[i], &tl, &th); uint32_t key = hi_word ? th : tl; uint32_t b = (key >> sh) & 0xFF; to[h[b]++] = from[i]; }
        double *t = from; from = to; to = t;
    }
    if (from != a) memcpy(a, from, n * sizeof(double));
}

/* ---- the dispatcher (port of sort_fn) ------------------------------------ */
/* Returns the buffer holding the sorted result: a (most paths) or buf (ping-pong paths). */
static double *sort_fn(double *a, size_t n, double *buf, ctx_t *c) {
    if (n < 2) return a;
    double mn = a[0], mx = a[0];
    /* -0.0 == floor(-0.0), but the counting and int32 radix paths below REBUILD values from integers and wrote it back
     * as +0.0, so the output was not a permutation of the input (harness signed zeros, zerodiag). A -0.0 is therefore not
     * an integer here, and data holding one takes a path that moves elements instead. */
    int all_int = isfinite(a[0]) && a[0] == floor(a[0]) && !(a[0] == 0.0 && signbit(a[0]));
    int asc = 1, desc = 1;
    for (size_t i = 1; i < n; i++) {
        double v = a[i];
        if (v < mn) mn = v; else if (v > mx) mx = v;
        if (all_int && !(isfinite(v) && v == floor(v) && !(v == 0.0 && signbit(v)))) all_int = 0;
        if (asc && v < a[i - 1]) asc = 0;
        if (desc && v > a[i - 1]) desc = 0;
    }
    int all_int32 = all_int && mn >= -2147483648.0 && mx <= 2147483647.0;
    if (asc) { PATH_TAG(c, LOGOS_PATH_SORTED); return a; }
    if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } PATH_TAG(c, LOGOS_PATH_SORTED); return a; }
    if (all_int) {
        double span = mx - mn + 1;
        if (span <= (double)COUNTING_SORT_K * (double)n) {
            /* the context's aux holds n counters; a span above n (COUNTING_SORT_K > 1) needs its own array */
            size_t sp = (size_t)span; uint32_t *B = sp <= n ? c->aux : malloc(sp * sizeof(uint32_t));
            if (B) {
                memset(B, 0, sp * sizeof(uint32_t));
                for (size_t i = 0; i < n; i++) B[(size_t)(a[i] - mn)]++;
                size_t w = 0;
                for (size_t v = 0; v < sp; v++) { uint32_t cnt = B[v]; double val = (double)v + mn; while (cnt-- > 0) a[w++] = val; }
                if (B != c->aux) free(B);
                PATH_TAG(c, LOGOS_PATH_COUNTING); return a;
            }
        }
        if (all_int32 && n >= 64) { if (g_simd_f64) { PATH_TAG(c, LOGOS_PATH_SIMD); g_simd_f64(a, n); return a; }
#if defined(LOGOS_AVX2_LEAF) && defined(LOGOS_AVX2_INT32)
            if (AVX2_TAKE(c, n) && avx2_qsort_f64(a, n)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
#endif
            PATH_TAG(c, LOGOS_PATH_INT32_RADIX); lsd_radix_int32(a, n, buf, c); return a; }
    }
    if (n <= INSERTION_SORT_THRESHOLD) { PATH_TAG(c, LOGOS_PATH_INSERTION); insertion_sort(a, 0, (long)n - 1); return a; }

    size_t run_cap = n / MIN_AVG_RUN_FOR_MERGE; if (run_cap < MAX_RUNS_FOR_MERGE) run_cap = MAX_RUNS_FOR_MERGE;
    size_t n_runs = 0; size_t *runs = detect_runs_limited(a, n, run_cap, MIN_AVG_RUN_FOR_MERGE, &n_runs, c);
    if (runs) {
        if (n_runs > 1) {
#ifndef LOGOS_NO_RELOCATE
            double *r = runs_relocate_f64(a, n, buf, runs, n_runs);   /* J5: disjoint runs -> memcpy into buf, no merge */
            if (r) { PATH_TAG(c, LOGOS_PATH_RUN_RELOCATE); free(runs); return r; }
#endif
            if (n_runs <= MERGE_MAX_RUNS) { PATH_TAG(c, LOGOS_PATH_RUN_MERGE); merge_all_runs(a, buf, runs, n_runs); free(runs); return a; }
        }
        free(runs);   /* too many overlapping runs for a k-way merge: the leaf below is cheaper (make knobbench, MERGE_MAX_RUNS) */
    }

    size_t sample_size = n < 40 ? n : 40, sample_step = n / sample_size; if (sample_step < 1) sample_step = 1;
    size_t inv = 0, comps = 0;
    for (size_t i = 0; i + sample_step < n; i += sample_step) { if (a[i] > a[i + sample_step]) inv++; comps++; }
    int nearly = comps > 0 && (double)inv / comps <= NEARLY_SORTED_INV_RATIO; c->nearly = nearly;
    if (nearly) {
#ifdef LOGOS_PLAIN_INSERTION
        if (n <= MOMENTUM_THRESHOLD) { PATH_TAG(c, LOGOS_PATH_INSERTION); insertion_sort(a, 0, (long)n - 1); return a; }
#else
        /* The 40-pair inversion probe bounds inversions, not displacement: a 48k bucket of near-sorted data
           whose 1% displaced elements travelled ~n/2 made plain insertion sort quadratic (333 ms for 8M at
           14T, nsbench). The momentum variant has an 8n move budget and falls through when it is exceeded. */
        if (n <= MOMENTUM_THRESHOLD) { if (insertion_sort_momentum(a, 0, (long)n - 1)) { PATH_TAG(c, LOGOS_PATH_INSERTION); return a; } }
        else
#endif
        if (c->last_bail_avg_run > 0 && c->last_bail_avg_run <= MOMENTUM_MAX_AVG_RUN) {
            if (insertion_sort_momentum(a, 0, (long)n - 1)) { PATH_TAG(c, LOGOS_PATH_MOMENTUM); return a; }
            /* aborted: not actually near-sorted; fall through */
        }
    }
#ifndef LOGOS_NO_LOWDISC
    /* Low-discrepancy probe (order from chaos, M3): two contiguous windows of 64. A Weyl / van der
     * Corput / Halton sequence has at most three distinct gap sizes in any window (three-gap
     * theorem), ratio <= ~2.6 for phi; random data's sorted gaps are exponential and the max/min
     * ratio is in the hundreds. Such data is a permutation of an equi-width grid: flash and coarse
     * place every element within one class of home and the fix-up is free, while a quicksort
     * pays its full log n levels. */
    c->lowdisc = 0;
    if (n >= FLASH_SORT_THRESHOLD && mx > mn) {
        int ld = 1;
        for (int w = 0; w < 2 && ld; w++) {
            double win[64]; size_t base = (n / 3) * (size_t)(w + 1) - 32;
            for (int i = 0; i < 64; i++) win[i] = a[base + (size_t)i];
            insertion_sort(win, 0, 63);
            double gmin = INFINITY, gmax = 0;
            for (int i = 1; i < 64; i++) { double g = win[i] - win[i - 1]; if (g < gmin) gmin = g; if (g > gmax) gmax = g; }
            if (!(gmin > 0) || gmax > LOWDISC_GAP_RATIO * gmin) ld = 0;
        }
        c->lowdisc = ld;
        if (ld) { PATH_TAG(c, LOGOS_PATH_FLASH); }
    }
#endif
#ifdef LOGOS_AVX2_LEAF
    if (n >= 32 && mx > mn && AVX2_TAKE(c, n) && avx2_qsort_f64(a, n)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }   /* AVX2 leaf: unstructured data; nearly-sorted keeps coarse (its scatter is near-sequential, measured 1.23x better) */
#endif
    if (n >= FLASH_SORT_THRESHOLD && mx > mn) {
        if (g_simd_f64) { PATH_TAG(c, LOGOS_PATH_SIMD); g_simd_f64(a, n); return a; }        /* SIMD leaf for large unstructured data */
        enum { SS = 64 };
        double samples[SS]; size_t s_stride = n / SS;
        for (int i = 0; i < SS; i++) samples[i] = a[i * s_stride];
        insertion_sort(samples, 0, SS - 1);
        double max_gap = 0; int max_gap_idx = -1;
        for (int i = 1; i < SS; i++) { double g = samples[i] - samples[i - 1]; if (g > max_gap) { max_gap = g; max_gap_idx = i; } }
        double gaps[SS - 1]; for (int i = 0; i < SS - 1; i++) gaps[i] = samples[i + 1] - samples[i];
        insertion_sort(gaps, 0, SS - 2);
        double median_gap = gaps[(SS - 1) >> 1], range = mx - mn, bucket_width = range / (double)n;
        int has_tight = 0; for (int i = 0; i + 6 < SS; i++) if (samples[i + 6] - samples[i] < bucket_width) { has_tight = 1; break; }
        int is_multimodal = median_gap > 0 && max_gap >= 10 * median_gap && max_gap >= 0.2 * range && has_tight && max_gap_idx >= 4 && max_gap_idx <= SS - 4;
        if (is_multimodal) {
            PATH_TAG(c, LOGOS_PATH_GAP_SPLIT);
            double pivot = (samples[max_gap_idx - 1] + samples[max_gap_idx]) / 2;
            long i = 0, j = (long)n - 1;
            while (i <= j) { while (i <= j && a[i] < pivot) i++; while (i <= j && a[j] >= pivot) j--; if (i < j) { double t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
            if (i > 1) { double *r = sort_fn(a, (size_t)i, buf, c); if (r != a) memcpy(a, buf, (size_t)i * sizeof(double)); }
            if ((size_t)i < n) { double *r = sort_fn(a + i, n - (size_t)i, buf, c); if (r != a + i) memcpy(a + i, buf, (n - (size_t)i) * sizeof(double)); }
            return a;
        }
        if (n < COARSE_MIN_N && (samples[47] - samples[16]) / range >= FLASH_SAFETY_RATIO) { PATH_TAG(c, LOGOS_PATH_FLASH); return flash_sort(a, n, mn, mx, buf, c); }
        if (n >= TYPED_MIN_N && skew_probe(a, n) > SKEW_PROBE_MAX) {
            int has_nan = 0; for (size_t i = 0; i < n; i++) if (a[i] != a[i]) { has_nan = 1; break; }
            if (!has_nan) { PATH_TAG(c, LOGOS_PATH_BITS_COARSE); return coarse_bits_f64(a, buf, 0, (long)n - 1, 0, 0, 0, 0, c); }
        }
        PATH_TAG(c, LOGOS_PATH_COARSE);
        return coarse_bucket_sort(a, buf, 0, (long)n - 1, 0, mn, mx, c);
    }
    PATH_TAG(c, LOGOS_PATH_QUICKSORT);
    quicksort(a, 0, (long)n - 1, 2 * log2i(n));
    return a;
}

/* typed-double entry (port of sort_fn.f64): quantized -> float radix; skewed
 * -> bits coarse; else the dispatcher. */
/* typed-double entry (port of sort_fn.f64); returns the buffer holding the result */
static double *leaf_f64(double *a, size_t n, double *buf, ctx_t *c) {
    if (n < TYPED_MIN_N) return sort_fn(a, n, buf, c);
    if (n <= RADIX64_MAX_N && quant_probe_f64(a, n) <= QUANT_PROBE_MAX) { PATH_TAG(c, LOGOS_PATH_QUANT_RADIX); radix_f64_full(a, n, buf, c); return a; }
    if (skew_probe(a, n) <= SKEW_PROBE_MAX) return sort_fn(a, n, buf, c);
    int has_nan = 0; for (size_t i = 0; i < n; i++) if (a[i] != a[i]) { has_nan = 1; break; }
    if (has_nan) return sort_fn(a, n, buf, c);
#ifdef LOGOS_AVX2_LEAF
    if (AVX2_TAKE(c, n) && avx2_qsort_f64(a, n)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
#endif
    PATH_TAG(c, LOGOS_PATH_BITS_COARSE);
    return coarse_bits_f64(a, buf, 0, (long)n - 1, 0, 0, 0, 0, c);
}

/* ---- parallel top level (v2) -------------------------------------------- */
/* Spin-then-block barrier. Threads that arrive early SPIN on the generation
 * counter for a short while before sleeping on the condvar. In the common case
 * (threads arriving within microseconds of each other) nobody sleeps and the
 * release costs nothing; only genuine stragglers pay a scheduler wake-up. A
 * sleep-immediately barrier made every phase pay ~ms of wake-up latency per
 * thread on a loaded WSL2 box — the sensitivity a loaded 20-core run exposed.
 * (This is what OpenMP's libgomp does: spin, then futex.) */
#if defined(__x86_64__) || defined(__i386__)
#  define CPU_RELAX() __asm__ __volatile__("pause" ::: "memory")
#else
#  define CPU_RELAX() __asm__ __volatile__("" ::: "memory")
#endif
#define BARRIER_SPIN 20000
typedef struct { pthread_mutex_t m; pthread_cond_t cv; int count; atomic_int arrived; atomic_int gen; } barrier_t;
static void barrier_init(barrier_t *b, int n) { pthread_mutex_init(&b->m, NULL); pthread_cond_init(&b->cv, NULL); b->count = n; atomic_init(&b->arrived, 0); atomic_init(&b->gen, 0); }
static void barrier_wait(barrier_t *b) {
    int g = atomic_load_explicit(&b->gen, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->arrived, 1, memory_order_acq_rel) + 1 == b->count) {
        atomic_store_explicit(&b->arrived, 0, memory_order_relaxed);
        pthread_mutex_lock(&b->m);
        atomic_store_explicit(&b->gen, g + 1, memory_order_release);   /* release spinners */
        pthread_cond_broadcast(&b->cv);                                  /* and sleepers */
        pthread_mutex_unlock(&b->m);
        return;
    }
    for (int i = 0; i < BARRIER_SPIN; i++) { if (atomic_load_explicit(&b->gen, memory_order_acquire) != g) return; CPU_RELAX(); }
    pthread_mutex_lock(&b->m);
    while (atomic_load_explicit(&b->gen, memory_order_acquire) == g) pthread_cond_wait(&b->cv, &b->m);
    pthread_mutex_unlock(&b->m);
}
static void barrier_destroy(barrier_t *b) { pthread_mutex_destroy(&b->m); pthread_cond_destroy(&b->cv); }

static inline int bucket_of(double x, const double *sp, int nsp) {
    if (nsp <= 32) { int b = 0; for (int i = 0; i < nsp; i++) b += (sp[i] <= x); return b; }
    const double *p = sp; int len = nsp;
    while (len > 0) { int half = len >> 1; int go = (p[half] <= x); p += go ? half + 1 : 0; len = go ? len - half - 1 : half; }
    return (int)(p - sp);
}

/* Work is split into fixed-size CHUNKS that threads pull from atomic counters,
 * for every phase. A thread that gets descheduled (background load, WSL2, a
 * busy core) simply completes fewer chunks instead of stalling every barrier on
 * itself — the straggler weakness a loaded 20-core run exposed. Histograms and
 * scatter offsets are per CHUNK, so any thread may process any chunk in either
 * phase; within a bucket, order is by chunk index (stable per chunk). */
/* ---- direct-index splitter tables (H0b) ----------------------------------
 * bucket_of's binary search over the splitters was the dominant 20-thread
 * phase (make profile: ~100 ns/element of CPU, 9x the scatter it feeds). Two
 * tables give the SAME bucket id in O(1) for most elements: cells over the
 * VALUE range (flat data resolves in one level) and cells over the order-
 * preserving KEY (log-magnitude cells; skewed data resolves shallow). A cell
 * crossed by one splitter resolves with one compare against that splitter; a
 * cell crossed by several gets a child table over the span of those splitters.
 * Cell bounds are widened by a relative 1e-13 before consulting the splitters,
 * so rounding can only make an entry more conservative, never wrong: every
 * "resolved" answer is exact and every other case ends in a compare against a
 * real splitter. Which table is faster depends on the data and on the branch
 * predictor, so each histogram thread times both on its first two chunks and
 * commits to the winner (experiments/paradigm/splitter_crux.cpp and
 * SELF_SAMPLING.md). Off with -DLOGOS_NO_SPLITTER_TABLE (A/B: make stbench). */
#ifndef LOGOS_NO_SPLITTER_TABLE
static inline uint64_t total_key(double v);   /* defined with the counting paths below */
#define BT_ROOT_CELLS  4096
#define BT_CHILD_CELLS 64          /* minimum; a child gets 4 cells per splitter it must separate, up to BT_MAX_CHILD */
#define BT_MAX_CHILD   256
#define BT_MAX_DEPTH   8
#define BT_MAX_NODES   512
#define BT_MODE_GAP    10.0      /* a splitter gap this many times the median gap is a gulf between modes */
#define BT_MODE_CELLS  2048
#define BT_MODE_MAXSP  1024
#define BT_MAX_ENTS    (BT_ROOT_CELLS + BT_MAX_NODES * BT_MAX_CHILD)
static inline int bt_child_cells(int nsplit) { int c = 4 * nsplit; return c < BT_CHILD_CELLS ? BT_CHILD_CELLS : c > BT_MAX_CHILD ? BT_MAX_CHILD : c; }
typedef struct { int16_t lo, hi; int32_t child; } bt_ent_t;
typedef struct { double lo, scale; int cells, base; } bt_vnode_t;
typedef struct { uint64_t klo; int shift, cells, base; } bt_knode_t;
typedef struct { double slo; float slope; int16_t lo, hi; } bt_ment_t;   /* model cell: bucket ~ lo + 1 + (x - slo) * slope */
typedef struct {
    const double *sp; int nsp; uint64_t *ksp; double vlo, vhi; uint64_t klo, khi;
    bt_vnode_t *vn; int nvn; bt_ent_t *ve; int nve;      /* value-space table */
    bt_knode_t *kn; int nkn; bt_ent_t *ke; int nke;      /* key-space table */
    bt_ment_t *me; double mscale; int merr;              /* model table (H0c): one level of value cells, a linear CDF model per cell */
    int nm, mroot[8], mfirst[8], mlast[8]; double mlo[8], mhi[8];   /* mode table (M2, "firmament"): a value table per mode, modes = big gaps between splitters */
    int vok, kok, mok, dok;
} bt_t;
#ifndef LOGOS_NO_MODE_TABLE
static int cmp_int(const void *x, const void *y) { int a = *(const int *)x, b = *(const int *)y; return (a > b) - (a < b); }
#endif
static int bt_cnt_le_v(const bt_t *t, double x)   { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->sp[m] <= x) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_lt_v(const bt_t *t, double x)   { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->sp[m] <  x) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_le_k(const bt_t *t, uint64_t k) { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->ksp[m] <= k) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_lt_k(const bt_t *t, uint64_t k) { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->ksp[m] <  k) lo = m + 1; else hi = m; } return lo; }
static int bt_build_v(bt_t *t, double vlo, double vhi, int cells, int depth) {
    if (t->nvn >= BT_MAX_NODES || t->nve + cells > BT_MAX_ENTS || !(vhi > vlo)) return -1;
    int id = t->nvn++; bt_vnode_t *nd = &t->vn[id]; nd->lo = vlo; nd->scale = cells / (vhi - vlo); nd->cells = cells; nd->base = t->nve; t->nve += cells;
    if (!isfinite(nd->scale)) return -1;
    for (int c = 0; c < cells; c++) {
        double clo = vlo + c / nd->scale, chi = (c + 1 == cells) ? vhi : vlo + (c + 1) / nd->scale;
        double w = fabs(clo) * 1e-13 + fabs(chi) * 1e-13 + 1e-300;            /* conservative widening (see above) */
        bt_ent_t e; e.lo = (int16_t)bt_cnt_le_v(t, clo - w); e.hi = (int16_t)bt_cnt_lt_v(t, chi + w); e.child = -1;
        if (e.hi < e.lo) e.hi = e.lo;
        if (e.hi - e.lo > 1 && depth < BT_MAX_DEPTH) {
            double slo = t->sp[e.lo], shi = t->sp[e.hi - 1];
            if (shi > slo) { int ch = bt_build_v(t, slo, shi, bt_child_cells(e.hi - e.lo), depth + 1); if (ch < 0) return -1; e.child = ch; }
        }
        t->ve[nd->base + c] = e;
    }
    return id;
}
#if defined(__SIZEOF_INT128__)
static int bt_build_k(bt_t *t, uint64_t klo, uint64_t khi, int cells, int depth) {
    if (t->nkn >= BT_MAX_NODES || t->nke + cells > BT_MAX_ENTS || !(khi > klo)) return -1;
    int id = t->nkn++; bt_knode_t *nd = &t->kn[id]; nd->klo = klo; nd->cells = cells; nd->base = t->nke; t->nke += cells;
    { int s = 0; uint64_t span = khi - klo; while (s < 63 && (span >> s) >= (uint64_t)cells) s++; nd->shift = s; }
    for (int c = 0; c < cells; c++) {
        unsigned __int128 clo = (unsigned __int128)klo + ((unsigned __int128)c << nd->shift), chi = (unsigned __int128)klo + ((unsigned __int128)(c + 1) << nd->shift);
        bt_ent_t e; e.child = -1;
        if (clo > khi) { e.lo = e.hi = (int16_t)t->nsp; t->ke[nd->base + c] = e; continue; }   /* past the top key: top bucket */
        e.lo = (int16_t)bt_cnt_le_k(t, (uint64_t)clo);
        e.hi = (int16_t)(chi > (unsigned __int128)UINT64_MAX ? t->nsp : bt_cnt_lt_k(t, (uint64_t)chi));
        if (e.hi < e.lo) e.hi = e.lo;
        if (e.hi - e.lo > 1 && depth < BT_MAX_DEPTH) {
            uint64_t slo = t->ksp[e.lo], shi = t->ksp[e.hi - 1];
            if (shi > slo) { int ch = bt_build_k(t, slo, shi, bt_child_cells(e.hi - e.lo), depth + 1); if (ch < 0) return -1; e.child = ch; }
        }
        t->ke[nd->base + c] = e;
    }
    return id;
}
#endif
/* ---- model table (H0c, Learned-Sort style with the splitters as ground truth) ----
 * One level of BT_ROOT_CELLS value cells. A cell crossed by 0 or 1 splitters
 * resolves as in the value table. A cell crossed by several stores a LINEAR
 * model of bucket-index against value fitted through its first and last
 * splitter (a piecewise-linear inverse CDF), and the lookup predicts the bucket
 * with one multiply and corrects it against the real splitters: when the
 * build-time worst-case error is <= 1 bucket (merr) the correction is two
 * branch-free compares, otherwise a short loop. Where the value table would
 * descend into a child table (a second dependent load and a branch on
 * "resolved"), the model does an FMA and two L1 loads. Same trial as the
 * other two: each histogram thread times it on a chunk and keeps the fastest.
 * OPT-IN (-DLOGOS_MODEL_TABLE): measured twice at 14 threads (make stbench),
 * within noise on every shape (0.93-1.09x, 7-thread rows 0.97-1.03x); the
 * histogram's remaining cost on bimodal is not the child-table dereference the
 * model removes. Kept as a measured negative and a template. */
#ifdef LOGOS_MODEL_TABLE
static inline __attribute__((always_inline)) int bt_m_pred(const bt_ment_t *e, double x) {
    float g = (float)(x - e->slo) * e->slope; if (g > 32767.f) g = 32767.f; if (g < -32768.f) g = -32768.f;
    int b = e->lo + 1 + (int)g; if (b < e->lo) b = e->lo; if (b > e->hi) b = e->hi; return b;
}
static int bt_build_m(bt_t *t, double vlo, double vhi) {
    if (!(vhi > vlo)) return 0;
    t->mscale = BT_ROOT_CELLS / (vhi - vlo); if (!isfinite(t->mscale)) return 0;
    int maxerr = 0;
    for (int c = 0; c < BT_ROOT_CELLS; c++) {
        double clo = vlo + c / t->mscale, chi = (c + 1 == BT_ROOT_CELLS) ? vhi : vlo + (c + 1) / t->mscale;
        double w = fabs(clo) * 1e-13 + fabs(chi) * 1e-13 + 1e-300;
        bt_ment_t e; e.lo = (int16_t)bt_cnt_le_v(t, clo - w); e.hi = (int16_t)bt_cnt_lt_v(t, chi + w); if (e.hi < e.lo) e.hi = e.lo;
        e.slo = 0; e.slope = 0;
        if (e.hi - e.lo >= 2) {
            double s0 = t->sp[e.lo], s1 = t->sp[e.hi - 1];
            e.slo = s0; e.slope = s1 > s0 ? (float)((double)(e.hi - 1 - e.lo) / (s1 - s0)) : 0.0f;
            /* worst error over the cell: the model is monotone, so its extremes against the step
               function are at the splitters (from above and from just below) and at the cell edges */
            for (int i = e.lo; i < e.hi; i++) {
                double x = t->sp[i]; int d = bt_m_pred(&e, x) - (i + 1); if (d < 0) d = -d; if (d > maxerr) maxerr = d;
                x = nextafter(x, -INFINITY); d = bt_m_pred(&e, x) - i; if (d < 0) d = -d; if (d > maxerr) maxerr = d;
            }
            { int d = bt_m_pred(&e, clo) - e.lo; if (d < 0) d = -d; if (d > maxerr) maxerr = d; }
            { int d = bt_m_pred(&e, nextafter(chi, -INFINITY)) - e.hi; if (d < 0) d = -d; if (d > maxerr) maxerr = d; }
        }
        t->me[c] = e;
    }
    t->merr = maxerr; return 1;
}
#endif
static void bt_free(bt_t *t) { free(t->ksp); free(t->vn); free(t->ve); free(t->kn); free(t->ke); free(t->me); memset(t, 0, sizeof *t); }
/* vlo/vhi: the sample's min and max (every splitter lies inside). Returns 1 if at least one table built. */
static int bt_init(bt_t *t, const double *sp, int nsp, double vlo, double vhi) {
    memset(t, 0, sizeof *t); t->sp = sp; t->nsp = nsp; t->vlo = vlo; t->vhi = vhi;
    if (nsp < 2 || !(vhi > vlo)) return 0;
    t->vn = malloc(BT_MAX_NODES * sizeof *t->vn); t->ve = malloc(BT_MAX_ENTS * sizeof *t->ve);
    t->kn = malloc(BT_MAX_NODES * sizeof *t->kn); t->ke = malloc(BT_MAX_ENTS * sizeof *t->ke); t->ksp = malloc((size_t)nsp * sizeof(uint64_t));
    if (!t->vn || !t->ve || !t->kn || !t->ke || !t->ksp) { bt_free(t); return 0; }
    t->vok = bt_build_v(t, vlo, nextafter(vhi, INFINITY), BT_ROOT_CELLS, 0) == 0;
#if defined(__SIZEOF_INT128__)
    for (int i = 0; i < nsp; i++) t->ksp[i] = total_key(sp[i]);
    t->klo = total_key(vlo); t->khi = total_key(vhi); t->nkn = 0; t->nke = 0;
    t->kok = (t->khi > t->klo) && bt_build_k(t, t->klo, t->khi, BT_ROOT_CELLS, 0) == 0;
#endif
#ifdef LOGOS_MODEL_TABLE
    t->me = malloc(BT_ROOT_CELLS * sizeof *t->me);
    t->mok = t->me && bt_build_m(t, vlo, nextafter(vhi, INFINITY));
#endif
#ifndef LOGOS_NO_MODE_TABLE
    /* Firmament split (Genesis 1:6, ORDER_FROM_CHAOS 4.1): the global value table spends its cells on
     * the empty gulf between modes and pays child tables inside each. Find gaps between consecutive
     * splitters larger than BT_MODE_GAP times the median gap (at most 8 modes, the largest gaps win),
     * build one value table per mode over the mode's own splitter range, and let the lookup pick the
     * mode with up to seven branch-free compares. Built only when there are >= 2 modes. */
    t->nm = 0; t->dok = 0;
    if (t->vok && nsp >= 8) {
        double gaps[BT_MODE_MAXSP]; int ng = nsp - 1 < BT_MODE_MAXSP ? nsp - 1 : BT_MODE_MAXSP;
        for (int i = 0; i < ng; i++) gaps[i] = sp[i + 1] - sp[i];
        double sorted[BT_MODE_MAXSP]; memcpy(sorted, gaps, ng * sizeof(double)); sample_sort(sorted, ng);
        double med = sorted[ng / 2];
        int cut[8]; int nc = 0;                                   /* indices i where the gap sp[i]..sp[i+1] is a gulf */
        if (med > 0) for (int i = 0; i < ng; i++) if (gaps[i] > BT_MODE_GAP * med) {
            if (nc < 7) cut[nc++] = i;
            else { int wmin = 0; for (int q = 1; q < nc; q++) if (gaps[cut[q]] < gaps[cut[wmin]]) wmin = q; if (gaps[i] > gaps[cut[wmin]]) cut[wmin] = i; }
        }
        if (nc >= 1) {
            qsort(cut, nc, sizeof(int), cmp_int);
            int first = 0, ok = 1;
            for (int k = 0; k <= nc && ok; k++) {
                int last = k < nc ? cut[k] : nsp - 1;             /* splitters first..last belong to mode k */
                t->mlo[k] = sp[first]; t->mhi[k] = sp[last]; t->mfirst[k] = first; t->mlast[k] = last; t->mroot[k] = -1;
                if (last > first && sp[last] > sp[first]) { t->mroot[k] = bt_build_v(t, sp[first], nextafter(sp[last], INFINITY), BT_MODE_CELLS, 0); if (t->mroot[k] < 0) ok = 0; }
                first = last + 1;
            }
            if (ok) { t->nm = nc + 1; t->dok = 1; }
        }
    }
#endif
    if (!t->vok && !t->kok && !t->mok && !t->dok) { bt_free(t); return 0; }
    return 1;
}
/* Leaf of a table lookup. Inside a child (a cluster of splitters, 4 cells per splitter) resolved and
 * one-splitter cells mix ~3:1, so a branch on "resolved" mispredicts; the two cases share one
 * compare-and-add whose compare is harmless when there is no splitter. At the ROOT the right choice
 * depends on the data: flat data resolves nearly every element there (a predictable branch is free
 * and saves a load), skewed data resolves a mix (the branch mispredicts). `style` picks: 0 = branchy
 * root, 1 = branch-free root. The histogram's trial chunks count root resolutions (`resolved`) and
 * choose the style per thread from that fraction (make theorybench: all-branch-free lost 5-12% on
 * int shapes, all-branchy lost up to 35% on heavy_tail). -DLOGOS_LOOKUP_BRANCHY forces two
 * branches everywhere (the original form); -DLOGOS_LOOKUP_BRANCHLESS forces branch-free everywhere. */
#if defined(LOGOS_LOOKUP_BRANCHY)
#define BT_LEAF(e, CMP, depth) do { (void)style; if ((e)->lo == (e)->hi) { if (resolved && (depth) == 0) (*resolved)++; return (e)->lo; } if ((e)->hi - (e)->lo == 1) return (e)->lo + (CMP((e)->lo)); } while (0)
#else
#define BT_LEAF(e, CMP, depth) do { \
    if ((!style || resolved) && (depth) == 0 && (e)->lo == (e)->hi) { if (resolved) (*resolved)++; return (e)->lo; } \
    int span_ = (e)->hi - (e)->lo; if (span_ <= 1) { int li_ = (e)->lo < t->nsp ? (e)->lo : t->nsp - 1; return (e)->lo + ((span_ > 0) & (CMP(li_))); } } while (0)
#endif
static inline __attribute__((always_inline)) int bt_lookup_v(const bt_t *t, double x, int style, size_t *resolved) {
    if (!(x >= t->vlo)) return 0;                            /* below every splitter, or NaN (bucket_of gives 0 too) */
    if (x >= t->vhi) return t->nsp;
    const bt_vnode_t *nd = t->vn; int depth = 0;
#define VCMP_(i) (t->sp[i] <= x)
    for (;;) {
        double f = (x - nd->lo) * nd->scale; int c = f >= (double)nd->cells ? nd->cells - 1 : (f > 0 ? (int)f : 0);
        const bt_ent_t *e = &t->ve[nd->base + c];
        BT_LEAF(e, VCMP_, depth);
        if (e->child >= 0) { const bt_vnode_t *ch = &t->vn[e->child]; if (x < ch->lo) return e->lo; if (x >= t->sp[e->hi - 1]) return e->hi; nd = ch; depth++; continue; }
        return e->lo + bucket_of(x, t->sp + e->lo, e->hi - e->lo);
    }
#undef VCMP_
}
static inline __attribute__((always_inline)) int bt_lookup_k(const bt_t *t, double x, int style, size_t *resolved) {
    uint64_t k = total_key(x);
    if (k < t->klo) return 0;
    if (k >= t->khi) return t->nsp;
    const bt_knode_t *nd = t->kn; int depth = 0;
#define KCMP_(i) (t->ksp[i] <= k)
    for (;;) {
        uint64_t d = k - nd->klo; int c = (int)(d >> nd->shift); if (c >= nd->cells) c = nd->cells - 1;
        const bt_ent_t *e = &t->ke[nd->base + c];
        BT_LEAF(e, KCMP_, depth);
        if (e->child >= 0) { const bt_knode_t *ch = &t->kn[e->child]; if (k < ch->klo) return e->lo; if (k >= t->ksp[e->hi - 1]) return e->hi; nd = ch; depth++; continue; }
        return e->lo + bucket_of(x, t->sp + e->lo, e->hi - e->lo);
    }
#undef KCMP_
}
#ifdef LOGOS_MODEL_TABLE
static inline __attribute__((always_inline)) int bt_lookup_m(const bt_t *t, double x, int style, size_t *resolved) {
    if (!(x >= t->vlo)) return 0;
    if (x >= t->vhi) return t->nsp;
    double f = (x - t->vlo) * t->mscale; int c = f >= (double)BT_ROOT_CELLS ? BT_ROOT_CELLS - 1 : (f > 0 ? (int)f : 0);
    const bt_ment_t *e = &t->me[c];
    if ((!style || resolved) && e->lo == e->hi) { if (resolved) (*resolved)++; return e->lo; }
    int span = e->hi - e->lo;
    if (span <= 1) { int li = e->lo < t->nsp ? e->lo : t->nsp - 1; return e->lo + ((span > 0) & (t->sp[li] <= x)); }
    int b = bt_m_pred(e, x);
    if (t->merr <= 1) {                                       /* prediction within one bucket: two branch-free compares */
        int bi = b < t->nsp ? b : t->nsp - 1; b += (b < e->hi) & (t->sp[bi] <= x);
        int bj = b > 0 ? b - 1 : 0;           b -= (b > e->lo) & (t->sp[bj] > x);
        return b;
    }
    while (b < e->hi && t->sp[b] <= x) b++;
    while (b > e->lo && t->sp[b - 1] > x) b--;
    return b;
}
#else
#define bt_lookup_m bt_lookup_v
#endif
#ifndef LOGOS_NO_MODE_TABLE
static inline __attribute__((always_inline)) int bt_lookup_d(const bt_t *t, double x, int style, size_t *resolved) {
    if (!(x >= t->vlo)) return 0;
    if (x >= t->vhi) return t->nsp;
    int k = 0; for (int q = 1; q < t->nm; q++) k += (x >= t->mlo[q]);          /* branch-free mode pick, nm <= 8 */
    if (x >= t->mhi[k]) return t->mlast[k] + 1;                                /* in the gulf after mode k (or on its last splitter) */
    if (x < t->mlo[k]) return 0;                                               /* below the first splitter (k == 0 only) */
    const bt_vnode_t *nd = &t->vn[t->mroot[k]]; int depth = 0;
#define VCMP_(i) (t->sp[i] <= x)
    for (;;) {
        double f = (x - nd->lo) * nd->scale; int c = f >= (double)nd->cells ? nd->cells - 1 : (f > 0 ? (int)f : 0);
        const bt_ent_t *e = &t->ve[nd->base + c];
        BT_LEAF(e, VCMP_, depth);
        if (e->child >= 0) { const bt_vnode_t *ch = &t->vn[e->child]; if (x < ch->lo) return e->lo; if (x >= t->sp[e->hi - 1]) return e->hi; nd = ch; depth++; continue; }
        return e->lo + bucket_of(x, t->sp + e->lo, e->hi - e->lo);
    }
#undef VCMP_
}
#else
#define bt_lookup_d bt_lookup_v
#endif
static double bt_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
#endif

/* Exponent-histogram bucket map (heavy tails). The key table spreads its cells evenly over the key
 * range, so on a Pareto the head -- where almost all the data is -- falls in few cells, each crossed by
 * many splitters, and every lookup there pays a second dependent load. This map indexes a flat 256-entry
 * table by the top byte of the order-preserving key, which is the sign and exponent, i.e. the integer
 * part of the logarithm, and interpolates linearly on the remaining 56 bits inside it. Resolution is
 * therefore allocated in proportion to the sample's density, which makes it an approximate inverse CDF:
 * one byte extract, one load, one multiply, one add. `span` is the number of buckets the exponent owns
 * and the offset is clamped below it, so the map is monotone by construction whatever the arithmetic
 * rounds to -- monotone is all a partition needs to be correct. Measured (make tailbench). */
typedef struct { uint32_t base, span; double slope; } ex_ent_t;
#define EX_SHIFT 52                      /* sign + the 11 exponent bits: one table entry per binade */
#define EX_MASK  0x000FFFFFFFFFFFFFULL   /* the 52 mantissa bits below it */
#define EX_SCALE 4503599627370496.0      /* 2^52 */
#ifndef HF_BAL_MAX
#define HF_BAL_MAX 3.0                   /* reject a bucket map that gives any bucket this many times its share of the sample */
#ifndef KG_BITS
#define KG_BITS 14                       /* key grid: at most 2^KG_BITS cells, one byte each (16 KB) */
#endif
#endif
#define CHUNK_MAX 65536
typedef struct {
    int P, nbk, nsp; double *src, *dst; size_t n; const double *sp;
    size_t nchunk, csize;                 /* chunk grid */
    size_t *counts, *offs;                /* nchunk x nbk */
    size_t *bstart, *bsize;               /* nbk */
    barrier_t bar; atomic_size_t hnext, snext, next; int wc_on;
    uint8_t *bidx;                        /* per-element bucket index, filled by the histogram pass (nbk <= 256) */
#ifndef LOGOS_NO_SPLITTER_TABLE
    bt_t bt; int bt_ok;                   /* direct-index splitter tables for the histogram */
#endif
    int ks_ok; double ks_d, ks_lo, ks_hi, ks_scale;   /* uniformity certificate from the sorted sample (histogram-free partition) */
    int ks_key_ok; uint64_t kk_lo, kk_hi; double kk_scale;   /* the same test in KEY space: log-uniform data (Benford) certifies here */
    int ex_ok; uint64_t ex_e0; double dup_frac; ex_ent_t ex[256];   /* exponent map (ex_e0 = the lowest binade sampled) and the largest single-value share */
    int md_ok, md_nm, md_first[8], md_last[8]; double md_lo[8], md_hi[8], md_scale[8];   /* per-mode linear map: certified uniform INSIDE each mode */
#ifndef LOGOS_NO_KGRID
    int kg_ok, kg_shift; uint64_t kg_lo; uint32_t kg_cells; uint8_t kg_tab[1u << KG_BITS];   /* key grid: bucket per key-space cell */
#endif
} job_t;
typedef struct { job_t *j; int t; ctx_t *ctx; } warg_t;

/* one chunk of the histogram with a given bucket function; fills the bucket-index cache when present */
#ifdef LOGOS_HIST_INTERLEAVE
/* OPT-IN (measured neutral, make theorybench): two count rows (even/odd elements) merged at the
 * end, so back-to-back increments of one counter do not serialise on store-to-load forwarding. */
#define HIST_CHUNK(BUCKET) do { \
    size_t c2_[j->nbk]; memset(c2_, 0, sizeof c2_); size_t i = lo; \
    if (bx) { for (; i + 1 < hi; i += 2) { int b0 = (BUCKET); i++; int b1 = (BUCKET); i--; bx[i] = (uint8_t)b0; bx[i + 1] = (uint8_t)b1; c[b0]++; c2_[b1]++; } \
              for (; i < hi; i++) { int b = (BUCKET); bx[i] = (uint8_t)b; c[b]++; } } \
    else    { for (; i + 1 < hi; i += 2) { int b0 = (BUCKET); i++; int b1 = (BUCKET); i--; c[b0]++; c2_[b1]++; } \
              for (; i < hi; i++) { int b = (BUCKET); c[b]++; } } \
    for (int b_ = 0; b_ < j->nbk; b_++) c[b_] += c2_[b_]; } while (0)
#else
#define HIST_CHUNK(BUCKET) do { \
    if (bx) { for (size_t i = lo; i < hi; i++) { int b = (BUCKET); bx[i] = (uint8_t)b; c[b]++; } } \
    else    { for (size_t i = lo; i < hi; i++) { int b = (BUCKET); c[b]++; } } } while (0)
#endif
/* Per-mode linear map. The engine already splits the value range at gulfs -- gaps between consecutive
 * splitters more than BT_MODE_GAP times the median gap -- and gives each mode its own value table. When
 * every mode is ALSO uniform inside itself, which the sample can certify one slice at a time, the table
 * is unnecessary: pick the mode with up to seven branch-free compares and interpolate linearly into the
 * stretch of buckets that mode owns. Modes partition the splitters contiguously, so mode k's top bucket
 * and mode k+1's bottom bucket meet exactly and the map is monotone. Measured (make tailbench):
 * -DLOGOS_NO_MODEKS turns it off. Note that a mixture of gaussians does NOT certify -- a hump is not
 * uniform inside -- so this fires on genuinely piecewise-flat data only. */
#ifndef LOGOS_NO_KGRID
/* The key grid (mode 10). A double's total-order key is a monotone, roughly logarithmic scale, so equal-width cells in KEY
 * space follow a heavy tail far better than cells in value space. Each cell holds the bucket it belongs to, with bucket
 * edges snapped to the cells holding the sample's quantiles, so a lookup is one subtract, one shift and one byte load:
 * no splitter compare, no child table. The exponent map (mode 8) tried the same idea with one linear piece per binade and
 * lost, because a heavy tail decays inside each binade too (buckets up to 3x their share); here a binade holds hundreds of
 * cells, and the balance check rejects any grid whose cells are too coarse for the data. The table never decreases, so
 * the partition stays correct whatever the balance. */
static inline __attribute__((always_inline)) int hf_lookup_kg(const job_t *j, double x) {
    uint64_t k = total_key(x);
    uint64_t d = k > j->kg_lo ? (k - j->kg_lo) >> j->kg_shift : 0;
    return j->kg_tab[d < j->kg_cells ? d : j->kg_cells - 1];
}
#endif
static inline __attribute__((always_inline)) int hf_lookup_md(const job_t *j, double x) {
    int k = 0;
    for (int q = 1; q < j->md_nm; q++) k += (x >= j->md_lo[q]);
    if (x < j->md_lo[k]) return j->md_first[k];              /* inside the gulf below this mode */
    if (x >= j->md_hi[k]) return j->md_last[k] + 1;
    int b = j->md_first[k] + 1 + (int)((x - j->md_lo[k]) * j->md_scale[k]);
    return b > j->md_last[k] ? j->md_last[k] : b;
}
/* the exponent map as a standalone inline, so the histogram path can trial it too */
static inline __attribute__((always_inline)) int hf_lookup_ex(const job_t *j, double x) {
    uint64_t k = total_key(x), q = k >> EX_SHIFT;
    unsigned e = q <= j->ex_e0 ? 0u : (unsigned)(q - j->ex_e0); if (e > 255u) e = 255u;
    const ex_ent_t *en = &j->ex[e];
    unsigned off = 0;
    if (en->span) { double f = (double)(k & EX_MASK) * en->slope; off = f > 0 ? (unsigned)f : 0; if (off >= en->span) off = en->span - 1; }
    return (int)(en->base + off);
}
static void do_hist(job_t *j) {
    size_t ch;
#ifndef LOGOS_NO_SPLITTER_TABLE
    /* mode: 0 = trial (one chunk per candidate table: 1 = value, 2 = key, 4 = model), then the fastest;
       3 = search (no tables). The trial also counts lookups resolved at the root; the committed leaf
       style is branchy-root only when that fraction is near 0 or 1 (predictable), branch-free otherwise. */
    int cand[5], ncand = 0;
    if (j->bt_ok && j->bt.vok) cand[ncand++] = 1;
    if (j->bt_ok && j->bt.kok) cand[ncand++] = 2;
    if (j->bt_ok && j->bt.mok && j->bt.merr <= 1) cand[ncand++] = 4;
    if (j->bt_ok && j->bt.dok) cand[ncand++] = 5;   /* per-mode value tables, only when >= 2 modes were found */
    /* NOT the exponent map: this path's scatter may recompute the bucket with bucket_of, and only the
       exact splitter lookups agree with it. Approximate maps belong to the histogram-free partition,
       which uses one map for counting and placing alike. */   /* a model that can miss by 2+ buckets loops in its correction: measured -12% on heavy_tail even as a trial chunk */
    int mode = ncand == 0 ? 3 : (ncand == 1 ? cand[0] : 0), trial = 0, style = 0;
    double tt[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 }; size_t res[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, len[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
#if defined(LOGOS_LOOKUP_BRANCHLESS)
    style = 1;
#endif
#endif
    while ((ch = atomic_fetch_add(&j->hnext, 1)) < j->nchunk) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n;
        size_t *c = j->counts + ch * j->nbk; for (int b = 0; b < j->nbk; b++) c[b] = 0;
        uint8_t *bx = j->bidx; const double *src = j->src;
#ifndef LOGOS_NO_SPLITTER_TABLE
        int use = mode == 0 ? cand[trial] : mode; double t0 = mode == 0 ? bt_now() : 0;
        if (mode == 0) {                                  /* trial chunks: probe one candidate, count root resolutions */
            if (use == 1)      HIST_CHUNK(bt_lookup_v(&j->bt, src[i], 0, &res[1]));
            else if (use == 2) HIST_CHUNK(bt_lookup_k(&j->bt, src[i], 0, &res[2]));
            else if (use == 5) HIST_CHUNK(bt_lookup_d(&j->bt, src[i], 0, &res[5]));
            else               HIST_CHUNK(bt_lookup_m(&j->bt, src[i], 0, &res[4]));
            len[use] = hi - lo; tt[use] = bt_now() - t0; trial++;
            if (trial == ncand) {
                mode = cand[0]; for (int q = 1; q < ncand; q++) if (tt[cand[q]] < tt[mode]) mode = cand[q];
                double p = (double)res[mode] / (double)len[mode];
#if defined(LOGOS_LOOKUP_BRANCHLESS)
                style = 1; (void)p;
#elif defined(LOGOS_LOOKUP_BRANCHY)
                style = 0; (void)p;
#else
                style = (p > 0.85 || p < 0.15) ? 0 : 1;
#endif
                TRACE("hist trial: value %.3f key %.3f model %.3f modes %.3f ms -> mode %d, root-resolved %.2f, style %d", tt[1], tt[2], tt[4], tt[5], mode, p, style);
            }
        }
        else if (use == 1) { if (style) HIST_CHUNK(bt_lookup_v(&j->bt, src[i], 1, NULL)); else HIST_CHUNK(bt_lookup_v(&j->bt, src[i], 0, NULL)); }
        else if (use == 2) { if (style) HIST_CHUNK(bt_lookup_k(&j->bt, src[i], 1, NULL)); else HIST_CHUNK(bt_lookup_k(&j->bt, src[i], 0, NULL)); }
        else if (use == 4) { if (style) HIST_CHUNK(bt_lookup_m(&j->bt, src[i], 1, NULL)); else HIST_CHUNK(bt_lookup_m(&j->bt, src[i], 0, NULL)); }
        else if (use == 5) { if (style) HIST_CHUNK(bt_lookup_d(&j->bt, src[i], 1, NULL)); else HIST_CHUNK(bt_lookup_d(&j->bt, src[i], 0, NULL)); }
        else               HIST_CHUNK(bucket_of(src[i], j->sp, j->nsp));
#else
        HIST_CHUNK(bucket_of(src[i], j->sp, j->nsp));
#endif
    }
}
static void do_scatter(job_t *j) {
    int nb = j->nbk; double *st = NULL; uint8_t *sc = NULL;
    if (j->wc_on) { st = malloc((size_t)nb * 8 * sizeof(double)); sc = calloc(nb, 1); if (!st || !sc) { free(st); free(sc); st = NULL; sc = NULL; } }
    size_t ch;
    while ((ch = atomic_fetch_add(&j->snext, 1)) < j->nchunk) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n;
        size_t *o = j->offs + ch * j->nbk; const uint8_t *bx = j->bidx;
        if (st) {
            for (size_t i = lo; i < hi; i++) { double x = j->src[i]; int b = bx ? bx[i] : bucket_of(x, j->sp, j->nsp);
                if (bx && i + PF_DIST < hi) PREFETCH_W(j->dst + o[bx[i + PF_DIST]]);
                st[b * 8 + sc[b]] = x; if (++sc[b] == 8) { memcpy(j->dst + o[b], st + b * 8, 64); o[b] += 8; sc[b] = 0; } }
            for (int b = 0; b < nb; b++) if (sc[b]) { memcpy(j->dst + o[b], st + b * 8, sc[b] * sizeof(double)); o[b] += sc[b]; sc[b] = 0; }
        } else if (bx) {
            for (size_t i = lo; i < hi; i++) { if (i + PF_DIST < hi) PREFETCH_W(j->dst + o[bx[i + PF_DIST]]); j->dst[o[bx[i]]++] = j->src[i]; }
        } else {
            for (size_t i = lo; i < hi; i++) { double x = j->src[i]; j->dst[o[bucket_of(x, j->sp, j->nsp)]++] = x; }
        }
    }
    free(st); free(sc);
}
static void do_buckets(job_t *j, ctx_t *ctx) {
    size_t b;
    while ((b = atomic_fetch_add(&j->next, 1)) < (size_t)j->nbk) {
        size_t s = j->bstart[b], nb = j->bsize[b];
        PROF(prof_stamp_t s0 = prof_stamp(); ctx->path = LOGOS_PATH_NONE;)
        if (g_simd_f64 && nb > 1) { g_simd_f64(j->dst + s, nb); memcpy(j->src + s, j->dst + s, nb * sizeof(double)); PROF(ctx->path = LOGOS_PATH_SIMD;) }  /* parallel-SIMD leaf */
        else { double *r = nb > 1 ? leaf_f64(j->dst + s, nb, j->src + s, ctx) : j->dst + s;
               if (r != j->src + s) memcpy(j->src + s, j->dst + s, nb * sizeof(double)); }
        PROF(double ms = prof_busy_since(ctx->tid, LOGOS_PH_BUCKETS, s0); prof_path(ctx->tid, ctx->path, nb, ms);)
    }
}

/* ---- histogram-free partition ("trust the sample"; default since 2026-09-11, -DLOGOS_NO_HISTFREE restores the histogram path; A/B: make hfbench) --------
 * The histogram pass exists to learn the bucket sizes before the scatter so every thread can write to
 * exact offsets. The sample already predicts them: a bucket's true count is n times the spacing between
 * two consecutive sample quantiles, a Beta(OVERSAMPLE, m-OVERSAMPLE+1) variable, so its standard deviation
 * is about 1/sqrt(OVERSAMPLE) = 18% of the bucket (NOT the 2% a DKW bound on the CDF suggests: that bound
 * is 3.7% of n, six bucket widths). So: give every bucket a region of (n / nbk) * (1 + HF_SLACK) in a
 * persistent buffer, let threads reserve HF_BLOCK-element blocks in it with one atomic per block (staged in
 * a per-thread write-combining buffer so no reservation is ever left partly filled), and spill the rare
 * overflow to a per-thread list that is sorted and merged in at the leaf. One read of the data instead
 * of two, no per-element bucket-index cache, and the exact splitter layout is no longer needed. If the
 * spills exceed n / HF_SPILL_DIV (a bucket the sample could not size: heavy duplicates), decline and let
 * the histogram path run. The table/style trial happens once, serially, on three 16k windows. */
#ifndef HF_KS_MAX
#define HF_KS_MAX 2.0        /* D <= HF_KS_MAX / sqrt(m) certifies uniform (about the 0.001 level) */
#endif
#if !defined(LOGOS_NO_HISTFREE) && !defined(LOGOS_HISTFREE)
#define LOGOS_HISTFREE 1
#endif
#ifdef LOGOS_HISTFREE
#ifndef HF_SLACK
#define HF_SLACK 0.6
#endif
#define HF_BLOCK 256
#define HF_SPILL_DIV 16
static void *big_alloc(size_t bytes);        /* huge-page-backed when the OS allows; defined with the pool */
static double *hf_buf = NULL; static size_t hf_buf_cap = 0;          /* persistent, like the pool: page faults once */
typedef struct { double v; uint16_t b; } hf_spill_t;
typedef struct {
    job_t *j; int P; size_t cap, region;                             /* region = cap rounded so buckets start 64-byte aligned */
    atomic_size_t *cur; atomic_size_t *ovf;                          /* per bucket: reservation cursor, first overflowing position (SIZE_MAX = none) */
    int mode, style;
    size_t *cnt, *spo, *spc, *bstart; hf_spill_t *spv; size_t S;     /* after the scatter */
    barrier_t bar; atomic_size_t next, bnext; int declined;
    int *order;                  /* leaf dispatch order; identity, or largest-first when LPT is on */
    int lazy;                    /* 1: place the buckets but do not sort them (logos_partition_f64) */
    double *vmin, *vmax;         /* lazy: per-bucket extremes, computed while the bucket is copied */
} hf_t;
typedef struct { hf_t *h; int t; ctx_t *ctx; hf_spill_t *sp; size_t spn, spcap; } hf_arg_t;

static inline int hf_lpt_on(void) { return getenv("LOGOS_LPT") != NULL; }   /* OFF by default: measured neutral, see hf_order. Uncached so one binary can A/B both orders. */
static inline __attribute__((always_inline)) int hf_lookup(const job_t *j, double x, int mode, int style) {
    if (mode == 6) {                                                  /* certified uniform: equi-width grid, one multiply, no table */
        if (!(x >= j->ks_lo)) return 0;
        if (x >= j->ks_hi) return j->nbk - 1;
        int b = (int)((x - j->ks_lo) * j->ks_scale); return b >= j->nbk ? j->nbk - 1 : b;
    }
#ifndef LOGOS_NO_KGRID
    if (mode == 10) return hf_lookup_kg(j, x);                        /* key grid: one byte load, no compares */
#endif
    if (mode == 7) {                                                  /* certified uniform IN KEY SPACE (log-uniform data) */
        uint64_t k = total_key(x);
        if (k <= j->kk_lo) return 0;
        if (k >= j->kk_hi) return j->nbk - 1;
        int b = (int)((double)(k - j->kk_lo) * j->kk_scale); return b >= j->nbk ? j->nbk - 1 : b;
    }
    if (mode == 9) return hf_lookup_md(j, x);                         /* every mode certified uniform inside itself */
    if (mode == 8) {                                                  /* exponent-histogram map: approximate inverse CDF */
        uint64_t k = total_key(x), q = k >> EX_SHIFT;
        unsigned e = q <= j->ex_e0 ? 0u : (unsigned)(q - j->ex_e0); if (e > 255u) e = 255u;
        const ex_ent_t *en = &j->ex[e];
        unsigned off = 0;
        if (en->span) { double f = (double)(k & EX_MASK) * en->slope; off = f > 0 ? (unsigned)f : 0; if (off >= en->span) off = en->span - 1; }
        return (int)(en->base + off);
    }
#ifndef LOGOS_NO_SPLITTER_TABLE
    if (mode == 1) return bt_lookup_v(&j->bt, x, style, NULL);
    if (mode == 2) return bt_lookup_k(&j->bt, x, style, NULL);
    if (mode == 5) return bt_lookup_d(&j->bt, x, style, NULL);
    if (mode == 4) return bt_lookup_m(&j->bt, x, style, NULL);
#else
    (void)mode; (void)style;
#endif
    return bucket_of(x, j->sp, j->nsp);
}
/* serial trial: time each candidate table on three 16k windows as a histogram, keep the fastest */
static void hf_trial(hf_t *h) {
    job_t *j = h->j; h->mode = 3; h->style = 0;
#ifndef HF_NO_KS
    if (j->ks_ok) { h->mode = 6; return; }                            /* the Kolmogorov-Smirnov certificate replaces the trial */
    if (j->ks_key_ok) { h->mode = 7; return; }                        /* uniform in key space: log-uniform data, same one multiply */
    if (j->md_ok) { h->mode = 9; return; }                            /* uniform inside every mode: compares then one multiply */
#ifndef LOGOS_NO_KGRID
    if (j->kg_ok && !getenv("LOGOS_NO_KGRID")) { h->mode = 10; return; }   /* balanced on the sample: the key grid, before any table trial */
#endif
#endif
#ifndef LOGOS_NO_SPLITTER_TABLE
    int cand[5], nc = 0;
    if (j->bt_ok && j->bt.vok) cand[nc++] = 1;
    if (j->bt_ok && j->bt.kok) cand[nc++] = 2;
    if (j->bt_ok && j->bt.mok && j->bt.merr <= 1) cand[nc++] = 4;
    if (j->bt_ok && j->bt.dok) cand[nc++] = 5;
    if (j->ex_ok) cand[nc++] = 8;
    if (nc == 0) return;
    size_t *c = calloc(j->nbk, sizeof(size_t)); if (!c) { h->mode = cand[0]; return; }
    double best = 1e300; size_t W = j->n < 3 * 16384 ? j->n / 3 : 16384;
    for (int q = 0; q < nc; q++) {
        size_t res = 0; double t0 = bt_now();
        for (int w = 0; w < 3; w++) { size_t lo = (j->n / 4) * (size_t)(w + 1) - W / 2, hi = lo + W;
            for (size_t i = lo; i < hi; i++) { int b = cand[q] == 1 ? bt_lookup_v(&j->bt, j->src[i], 0, &res) : cand[q] == 2 ? bt_lookup_k(&j->bt, j->src[i], 0, &res) : cand[q] == 5 ? bt_lookup_d(&j->bt, j->src[i], 0, &res) : cand[q] == 8 ? hf_lookup_ex(j, j->src[i]) : bt_lookup_m(&j->bt, j->src[i], 0, &res); c[b]++; } }
        double dt = bt_now() - t0;
        if (dt < best) { best = dt; h->mode = cand[q]; double p = (double)res / (double)(3 * W); h->style = (p > 0.85 || p < 0.15) ? 0 : 1; }
    }
    free(c);
#endif
}
static void hf_spill_push(hf_arg_t *w, double v, int b) {
    if (w->spn == w->spcap) { size_t nc = w->spcap ? w->spcap * 2 : 4096; hf_spill_t *p = realloc(w->sp, nc * sizeof *p); if (!p) { w->h->declined = 1; return; } w->sp = p; w->spcap = nc; }
    w->sp[w->spn].v = v; w->sp[w->spn].b = (uint16_t)b; w->spn++;
}
/* flush k staged elements of bucket b: reserve exactly k slots, write, or spill the block */
/* ---------------- non-temporal (streaming) stores for the scatter ----------------
 * A normal store to a cache line that is not resident must first FETCH that line from memory, so
 * writing N bytes costs N read plus N written. That read-for-ownership traffic is pure waste here: the
 * scatter overwrites every byte of its destination and nothing reads it until after the barrier.
 * `_mm256_stream_pd` writes straight through the write-combining buffers and skips the fetch.
 *
 * Two things make it legal and efficient in this spot specifically. The destination region is about
 * 100 MB at 8M elements, far past the 24 MB L3, so it was going to DRAM regardless and there is no
 * cached copy to lose. And `hf_flush` already arrives with 2 KB of contiguous data, so the write
 * combiners fill completely; streaming partial lines would be worse than a normal store.
 *
 * Streaming stores are weakly ordered, so `hf_scatter` fences once per thread before the barrier that
 * hands the regions to the leaf phase. Alignment is handled rather than assumed: `p` comes from an
 * atomic add of flush sizes and the end-of-chunk flushes are partial, so a region offset can be any
 * value. Scalar prologue, streamed middle, scalar tail. -DLOGOS_STREAM turns it on. */
#if defined(LOGOS_STREAM) && (defined(__x86_64__) || defined(__i386__))
#define HF_STREAM_MIN 64        /* doubles; below this the prologue costs more than the fetch saves */
static int g_stream = -1;
static inline int stream_on(void) { int v = g_stream; if (v < 0) { v = __builtin_cpu_supports("avx2") && !getenv("LOGOS_NO_STREAM"); g_stream = v; } return v; }
/* The target string below is not a choice, it has to MATCH the one immintrin.h was included under at
 * the top of this file. GCC stamps every intrinsic `always_inline` plus the target active where the
 * header was read, and an always_inline callee cannot be inlined into a caller whose target is a
 * SUBSET of it. So `target("avx")` here fails with "target specific option mismatch" even for plain
 * SSE2 intrinsics, which is a confusing error for an obviously-baseline instruction. Keep the two
 * strings identical; `stream_on` does the matching runtime check. */
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#ifdef LOGOS_TRACE
static atomic_size_t g_streamed = 0;      /* doubles written non-temporally; proves the path is live */
#endif
static void hf_stream_copy(double *dst, const double *src, size_t k) {
    size_t i = 0;
#ifdef LOGOS_TRACE
    atomic_fetch_add(&g_streamed, k);
#endif
    while (i < k && (((uintptr_t)(dst + i)) & 31u)) { dst[i] = src[i]; i++; }   /* reach a 32-byte boundary */
    for (; i + 16 <= k; i += 16) {                                             /* two 64-byte lines per step */
        _mm256_stream_pd(dst + i,      _mm256_loadu_pd(src + i));
        _mm256_stream_pd(dst + i + 4,  _mm256_loadu_pd(src + i + 4));
        _mm256_stream_pd(dst + i + 8,  _mm256_loadu_pd(src + i + 8));
        _mm256_stream_pd(dst + i + 12, _mm256_loadu_pd(src + i + 12));
    }
    for (; i + 4 <= k; i += 4) _mm256_stream_pd(dst + i, _mm256_loadu_pd(src + i));
    for (; i < k; i++) dst[i] = src[i];
}
static inline void hf_stream_fence(void) { _mm_sfence(); }
#pragma GCC pop_options
static inline void hf_copy(double *dst, const double *src, size_t k) {
    if (k >= HF_STREAM_MIN && stream_on()) hf_stream_copy(dst, src, k);
    else memcpy(dst, src, k * sizeof(double));
}
#else
static inline void hf_stream_fence(void) { }
static inline void hf_copy(double *dst, const double *src, size_t k) { memcpy(dst, src, k * sizeof(double)); }
#endif

static void hf_flush(hf_arg_t *w, int b, const double *blk, size_t k) {
    hf_t *h = w->h; size_t p = atomic_fetch_add(&h->cur[b], k);
    if (p + k <= h->cap) { hf_copy(hf_buf + (size_t)b * h->region + p, blk, k); return; }
    size_t want = SIZE_MAX; atomic_compare_exchange_strong(&h->ovf[b], &want, p);   /* first overflow marks the filled end */
    for (size_t i = 0; i < k; i++) hf_spill_push(w, blk[i], b);
}
static void hf_scatter(hf_arg_t *w) {
    hf_t *h = w->h; job_t *j = h->j; int nb = j->nbk, mode = h->mode, style = h->style;
    double *st = malloc((size_t)nb * HF_BLOCK * sizeof(double)); uint16_t *sc = calloc(nb, sizeof(uint16_t));
    if (!st || !sc) { free(st); free(sc); h->declined = 1; return; }
    size_t ch;
    while ((ch = atomic_fetch_add(&h->next, 1)) < j->nchunk && !h->declined) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n; const double *src = j->src;
        for (size_t i = lo; i < hi; i++) {
            double x = src[i]; int b = hf_lookup(j, x, mode, style);
            st[(size_t)b * HF_BLOCK + sc[b]] = x;
            if (++sc[b] == HF_BLOCK) { hf_flush(w, b, st + (size_t)b * HF_BLOCK, HF_BLOCK); sc[b] = 0; }
        }
    }
    for (int b = 0; b < nb; b++) if (sc[b]) { hf_flush(w, b, st + (size_t)b * HF_BLOCK, sc[b]); sc[b] = 0; }
    hf_stream_fence();        /* weakly ordered stores must land before the barrier hands off the regions */
    free(st); free(sc);
}
/* Longest-processing-time-first leaf dispatch.
 *
 * The leaves are handed out by one atomic increment each, so whichever leaf a thread picks up LAST
 * decides when everybody finishes. Greedy self-scheduling in an arbitrary order has a makespan of at
 * most (2 - 1/P) times optimal; taking the longest job first tightens that to (4/3 - 1/(3P)), and both
 * bounds are tight. Here the job lengths are not estimated, they are known exactly -- the leaf sizes
 * were just computed for the output offsets -- so the ordering costs one heapsort over at most 256
 * indices and nothing else.
 *
 * Concretely at 14 threads: 168 buckets, 12 per thread, and the balance check already caps any one
 * bucket at 3x its share. A 3x leaf picked up last leaves the other threads idle for two leaf-times,
 * about 20% of the phase. Picking it first costs nothing.
 *
 * The toggle is read with getenv on each partition call, not cached, which is what lets one binary
 * measure both orders in a paired A/B with no second copy of the code and therefore no code-layout
 * bias (methodology rule 3). It is one getenv per parallel sort against ~1 ms of serial setup.
 *
 * MEASURED NEUTRAL, and it ships OFF (`LOGOS_LPT=1` turns it on; `make lptbench`). The reasoning above
 * is sound and the target is not there, because the splitters are SAMPLE QUANTILES: equalising the
 * buckets is what a quantile splitter does, so the scheduling problem was already solved upstream by
 * the partition design. Traced leaf imbalance (largest leaf over the mean) is 1.10x on uniform data and
 * 1.48x on Pareto, against the 3x the balance check would allow. Put those in the greedy makespan bound
 * W/P + L(1 - 1/P), with 12 leaves per thread: the WHOLE straggler cost is 8% of the leaf phase on
 * uniform and 11% on Pareto, and largest-first recovers only part of that, so the ceiling is 1-3% of
 * the leaf phase and less of the sort. That is under what this box resolves (`make pairprobe`: 3.7% at
 * 40 pairs), and the paired A/B duly read 0.95-1.05 on seven shapes. Where the idea SHOULD pay is the
 * counting and lattice routes, which carve static per-thread ranges and have a measured 1.3-2x
 * imbalance; that is a different piece of code and still open. */
static void hf_order(int *ord, const size_t *sz, int k) {
    for (int i = 0; i < k; i++) ord[i] = i;
    if (!hf_lpt_on()) return;
    for (int st = k / 2 - 1; st >= 0; st--) {                       /* heapify: max-heap keyed on size */
        int r = st;
        for (;;) { int c = 2 * r + 1; if (c >= k) break;
            if (c + 1 < k && sz[ord[c + 1]] > sz[ord[c]]) c++;
            if (sz[ord[c]] <= sz[ord[r]]) break;
            int t = ord[r]; ord[r] = ord[c]; ord[c] = t; r = c; }
    }
    for (int e = k - 1; e > 0; e--) {                               /* pop: leaves ord[] ascending */
        int t = ord[0]; ord[0] = ord[e]; ord[e] = t;
        int r = 0;
        for (;;) { int c = 2 * r + 1; if (c >= e) break;
            if (c + 1 < e && sz[ord[c + 1]] > sz[ord[c]]) c++;
            if (sz[ord[c]] <= sz[ord[r]]) break;
            int t2 = ord[r]; ord[r] = ord[c]; ord[c] = t2; r = c; }
    }
    for (int i = 0, q = k - 1; i < q; i++, q--) { int t = ord[i]; ord[i] = ord[q]; ord[q] = t; }
}
static void hf_leaves(hf_arg_t *w) {
    hf_t *h = w->h; job_t *j = h->j; size_t bi;
    while ((bi = atomic_fetch_add(&h->bnext, 1)) < (size_t)j->nbk) {
        size_t b = (size_t)h->order[bi];
        double *reg = hf_buf + b * h->region, *out = j->src + h->bstart[b]; size_t nb = h->cnt[b], ns = h->spc[b];
        PROF(prof_stamp_t s0 = prof_stamp(); w->ctx->path = LOGOS_PATH_NONE;)
        if (h->lazy) {
            /* A view only needs the buckets PLACED, not ordered inside. Copy the region and append the
             * spills; both belong to this bucket, so any order within it is still a correct partition
             * and the caller sorts one bucket when it looks at one. This is where the ~63% of the sort
             * that lives in the leaves gets deferred. */
            if (h->vmin) {
                /* The data is streaming through anyway, so the bucket's extremes come almost free here
                 * and save the view a whole extra pass. rank(x) needs them to find x's bucket exactly,
                 * whatever bucket map the partition used. Empty buckets read (+inf, -inf). */
                double mn = INFINITY, mx = -INFINITY;
                for (size_t i = 0; i < nb; i++) { double x = reg[i]; out[i] = x; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
                for (size_t i = 0; i < ns; i++) { double x = h->spv[h->spo[b] + i].v; out[nb + i] = x; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
                h->vmin[b] = mn; h->vmax[b] = mx;
            } else {
                memcpy(out, reg, nb * sizeof(double));
                for (size_t i = 0; i < ns; i++) out[nb + i] = h->spv[h->spo[b] + i].v;
            }
            PROF(double mz = prof_busy_since(w->ctx->tid, LOGOS_PH_BUCKETS, s0); prof_path(w->ctx->tid, w->ctx->path, nb + ns, mz);)
            continue;
        }
        if (ns == 0) {
            double *r = nb > 1 ? leaf_f64(reg, nb, out, w->ctx) : reg;
            if (r != out) memcpy(out, r, nb * sizeof(double));
        } else {
            double *sv = malloc((ns ? ns : 1) * sizeof(double)); for (size_t i = 0; i < ns; i++) sv[i] = h->spv[h->spo[b] + i].v;
            sample_sort(sv, ns);
            double *r = nb > 1 ? leaf_f64(reg, nb, out, w->ctx) : reg;
            if (r != reg) memcpy(reg, r, nb * sizeof(double));                    /* sorted region back in the buffer, out is free */
            const double *x = reg, *xe = reg + nb, *y = sv, *ye = sv + ns; double *o = out;
            while (x < xe && y < ye) { if (*y < *x) *o++ = *y++; else *o++ = *x++; }
            if (x < xe) { memcpy(o, x, (size_t)(xe - x) * sizeof(double)); o += xe - x; }
            if (y < ye) memcpy(o, y, (size_t)(ye - y) * sizeof(double));
            free(sv);
        }
        PROF(double ms = prof_busy_since(w->ctx->tid, LOGOS_PH_BUCKETS, s0); prof_path(w->ctx->tid, w->ctx->path, nb + ns, ms);)
    }
}
static void *hf_worker(void *p) {
    hf_arg_t *w = p; hf_t *h = w->h;
    PROF(prof_stamp_t s0 = prof_stamp();)
    hf_scatter(w);
    PROF(prof_busy_since(w->t, LOGOS_PH_SCATTER, s0); double t1 = prof_now();)
    barrier_wait(&h->bar); barrier_wait(&h->bar);                    /* main: sizes, spills, prefix, contexts */
    PROF(prof_wait(w->t, LOGOS_PH_SCATTER, prof_now() - t1);)
    if (!h->declined) hf_leaves(w);
    return NULL;
}
/* returns 1 if the array was sorted, 0 if declined (a[] untouched; the histogram path follows) */
static int hf_partition(job_t *j, int P, ctx_t *ctxs, pthread_t *th, int lazy, size_t *vstart, int *vnbk, double *vmin, double *vmax) {
    size_t n = j->n; int nbk = j->nbk;
    /* One value holding more than a region can hold overflows it however the splitters fall, and the
     * spill test would then fail after a whole pass over the data. The sample already knows the largest
     * share any single value has, so decline here instead. */
    if (j->dup_frac * (double)nbk > 1.0 + HF_SLACK) { TRACE("hf: declined up front, one value is %.1f%% of the data", j->dup_frac * 100.0); return 0; }
    hf_t h; memset(&h, 0, sizeof h); h.j = j; h.P = P;
    h.cap = (size_t)((double)(n / (size_t)nbk + 1) * (1.0 + HF_SLACK)) + HF_BLOCK;
    h.region = (h.cap + 7) & ~(size_t)7;
    size_t need = h.region * (size_t)nbk;
    /* grown, never preserved: the regions are overwritten before they are read, so this frees and
       reallocates rather than realloc()ing, which lets it come back 2 MB aligned (see big_alloc). */
    if (need > hf_buf_cap) {
        /* 64-byte aligned even when big_alloc falls through to malloc: `region` is a multiple of 8
           doubles, so an aligned base keeps every region start aligned and the streaming prologue
           empty for all the full-block flushes. */
        double *nbuf = (double *)big_alloc(need * sizeof(double));
        if (!nbuf) return 0;
        if (((uintptr_t)nbuf) & 63u) {
            double *al = NULL;
            if (posix_memalign((void **)&al, 64, need * sizeof(double)) == 0 && al) { free(nbuf); nbuf = al; }
        }
        free(hf_buf); hf_buf = nbuf; hf_buf_cap = need;
    }
    h.cur = malloc((size_t)nbk * sizeof(atomic_size_t)); h.ovf = malloc((size_t)nbk * sizeof(atomic_size_t));
    h.cnt = calloc(nbk, sizeof(size_t)); h.spo = calloc(nbk + 1, sizeof(size_t)); h.spc = calloc(nbk, sizeof(size_t)); h.bstart = calloc(nbk + 1, sizeof(size_t));
    h.order = malloc((size_t)nbk * sizeof(int));
    hf_arg_t *wa = calloc((size_t)P, sizeof(hf_arg_t));
    int ok = h.cur && h.ovf && h.cnt && h.spo && h.spc && h.bstart && h.order && wa, sorted = 0;
    if (ok) {
        for (int b = 0; b < nbk; b++) { atomic_init(&h.cur[b], 0); atomic_init(&h.ovf[b], SIZE_MAX); }
        h.lazy = lazy; h.vmin = lazy ? vmin : NULL; h.vmax = lazy ? vmax : NULL;
        hf_trial(&h);
        TRACE("hf: trial mode %d style %d, cap %zu per bucket", h.mode, h.style, h.cap);
#if defined(LOGOS_TRACE) && defined(LOGOS_STREAM)
        atomic_store(&g_streamed, 0);
#endif
        barrier_init(&h.bar, P); atomic_init(&h.next, 0); atomic_init(&h.bnext, 0);
        for (int t = 0; t < P; t++) { wa[t].h = &h; wa[t].t = t; wa[t].ctx = &ctxs[t]; }
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, hf_worker, &wa[t]);
        PROF(prof_stamp_t s0 = prof_stamp();)
        hf_scatter(&wa[0]);
        PROF(prof_busy_since(0, LOGOS_PH_SCATTER, s0); double t1 = prof_now();)
        barrier_wait(&h.bar);
        PROF(prof_wait(0, LOGOS_PH_SCATTER, prof_now() - t1); prof_phase(LOGOS_PH_SCATTER);)
        TRACE("par: histfree scatter done");
        /* sizes, spills by bucket, output offsets */
        size_t S = 0; for (int t = 0; t < P; t++) S += wa[t].spn;
        if (!h.declined && S <= n / HF_SPILL_DIV) {
            h.spv = malloc((S ? S : 1) * sizeof(hf_spill_t));
            if (h.spv) {
                for (int b = 0; b < nbk; b++) { size_t o = atomic_load(&h.ovf[b]), c = atomic_load(&h.cur[b]); h.cnt[b] = o == SIZE_MAX ? c : o; }
                for (int t = 0; t < P; t++) for (size_t i = 0; i < wa[t].spn; i++) h.spc[wa[t].sp[i].b]++;
                for (int b = 0; b < nbk; b++) h.spo[b + 1] = h.spo[b] + h.spc[b];
                { size_t *fill = calloc(nbk, sizeof(size_t)); for (int t = 0; t < P; t++) for (size_t i = 0; i < wa[t].spn; i++) { int b = wa[t].sp[i].b; h.spv[h.spo[b] + fill[b]++] = wa[t].sp[i]; } free(fill); }
                size_t maxb = 1, lsz[MAX_BUCKETS];
                for (int b = 0; b < nbk; b++) { size_t z = h.cnt[b] + h.spc[b]; lsz[b] = z; h.bstart[b + 1] = h.bstart[b] + z; if (z > maxb) maxb = z; }
                hf_order(h.order, lsz, nbk);                          /* biggest leaf first: the last one picked up sets the finish time */
                TRACE("par: leaf sizes max %zu mean %zu -> imbalance %.2fx", lsz[h.order[0]], n / (size_t)nbk, (double)lsz[h.order[0]] * (double)nbk / (double)n);
                if (h.bstart[nbk] != n) h.declined = 1;
                for (int t = 0; !h.declined && t < P; t++) if (!ctx_init(&ctxs[t], maxb, 0)) h.declined = 1;
                PROF(for (int t = 0; t < P; t++) ctxs[t].tid = t;)
                h.S = S;
            } else h.declined = 1;
        } else h.declined = 1;
        TRACE("par: histfree spills %zu (%s)", S, h.declined ? "declined" : "ok");
#if defined(LOGOS_TRACE) && defined(LOGOS_STREAM)
        TRACE("par: %zu of %zu elements went out through non-temporal stores", (size_t)atomic_load(&g_streamed), n);
#endif
        PROF(prof_phase(LOGOS_PH_COMBINE);)
        barrier_wait(&h.bar);
        if (!h.declined) hf_leaves(&wa[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        PROF(prof_phase(LOGOS_PH_BUCKETS);)
        barrier_destroy(&h.bar);
        sorted = !h.declined;
        if (sorted && lazy && vstart && vnbk) {                        /* hand the boundaries to the view */
            for (int b = 0; b <= nbk; b++) vstart[b] = h.bstart[b];
            *vnbk = nbk;
        }
        if (!sorted) for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);   /* the histogram path re-inits them */
    }
    for (int t = 0; wa && t < P; t++) free(wa[t].sp);
    free(wa); free(h.cur); free(h.ovf); free(h.cnt); free(h.spo); free(h.spc); free(h.bstart); free(h.spv); free(h.order);
    return sorted;
}
#endif
static void *worker(void *p) {
    warg_t *w = p; job_t *j = w->j;
    PROF(prof_stamp_t s0 = prof_stamp();)
    do_hist(j);
    PROF(prof_busy_since(w->t, LOGOS_PH_HIST, s0); double t1 = prof_now();)
    barrier_wait(&j->bar); barrier_wait(&j->bar);          /* second barrier: main's serial combine */
    PROF(prof_wait(w->t, LOGOS_PH_HIST, prof_now() - t1); prof_stamp_t s2 = prof_stamp();)
    do_scatter(j);
    PROF(prof_busy_since(w->t, LOGOS_PH_SCATTER, s2); double t3 = prof_now();)
    barrier_wait(&j->bar);
    PROF(prof_wait(w->t, LOGOS_PH_SCATTER, prof_now() - t3);)
    do_buckets(j, w->ctx);
    return NULL;
}


/* ============ low-cardinality parallel counting sort (f64) ============
 * Quantile splitters cannot separate EQUAL keys, so few-distinct data
 * collapses into a handful of giant buckets (load imbalance) and then pays
 * per-bucket passes for nothing. If a strided sample shows few distinct
 * values, each thread instead builds a tiny sorted map (key -> count) over its
 * slice — keyed on the IEEE total-order bit pattern so -0.0/+0.0 stay distinct
 * and output is bit-exact — the maps are merged, and every thread fills its own
 * output range from the global (value, count) list. Two parallel passes,
 * perfectly balanced. Any NaN, or more than LOWCARD_CAP distinct values in one
 * slice, aborts before anything is written and the normal path runs. */
#ifndef LOWCARD_SAMPLE_MAX
#define LOWCARD_SAMPLE_MAX 64
#endif
#ifndef LOWCARD_MAX_SCAN
#define LOWCARD_MAX_SCAN   LOWCARD_SAMPLE_MAX   /* above this many distinct sample values the counting route declines and
                                                   the partition absorbs the duplicates instead (variant C: make cardbench) */
#endif
#ifndef LC_PHASH_BITS
#define LC_PHASH_BITS 12                        /* 4096-entry byte table: collision-free for <= 64 keys in a few tries */
#endif
#ifndef LOWCARD_CAP
#define LOWCARD_CAP        256    /* was 1024 (JS era). The map aborts at this many distinct values; zipf passes the 64-sample gate yet has thousands and aborted late: 256 gives zipf 1.44x at 1T, 1.3-1.5x at 14T (make knobbench). Data with 256-1024 distinct values ran 4.4x slower on this path than on the hash anyway (collatz) */
#endif
static inline uint64_t total_key(double v) { uint64_t u; memcpy(&u, &v, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
typedef struct { const double *a; size_t lo, hi; uint64_t *keys; double *vals; size_t *cnts; size_t d; int abort; int tid;
                 uint64_t hmul; const uint64_t *hkey; const uint8_t *slot; size_t sd, *hcnt; } lcmap_t;   /* hmul != 0: verified perfect hash from the sample */
static void *lc_map_body(void *p);
static void *lc_map(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = lc_map_body(p); PROF(prof_busy_since(((lcmap_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *lc_map_body(void *p) {
    lcmap_t *m = p; size_t d = 0;
    if (m->hmul) {
        /* The gate already sorted a sample and found <= LOWCARD_SAMPLE_MAX distinct values; a multiply-shift
         * perfect hash over those keys turns the per-element lookup into a multiply, a shift, a byte load and
         * one compare. The compare is what makes it exact: a value the sample missed fails it and falls into
         * the incremental table below, so the counts are right whatever the sample saw. Measured per element
         * (make cardbench): scan 2.0 ns at 5 distinct and 30.8 ns at 64 (its binary search above 32), hash
         * 1.1 ns at both. */
        const uint64_t mul = m->hmul, *hk = m->hkey; const uint8_t *slot = m->slot; size_t *hc = m->hcnt;
        for (size_t i = m->lo; i < m->hi; i++) {
            double x = m->a[i]; uint64_t k = total_key(x);
            unsigned h = (unsigned)((k * mul) >> (64 - LC_PHASH_BITS)); unsigned sl = slot[h];
            if (hk[sl] == k) { hc[sl]++; continue; }
            if (x != x) { m->abort = 1; return NULL; }
            size_t lo = 0; for (size_t q = 0; q < d; q++) lo += (m->keys[q] < k);
            if (lo < d && m->keys[lo] == k) { m->cnts[lo]++; continue; }
            if (d == LOWCARD_CAP) { m->abort = 1; return NULL; }
            memmove(m->keys + lo + 1, m->keys + lo, (d - lo) * sizeof(uint64_t));
            memmove(m->vals + lo + 1, m->vals + lo, (d - lo) * sizeof(double));
            memmove(m->cnts + lo + 1, m->cnts + lo, (d - lo) * sizeof(size_t));
            m->keys[lo] = k; m->vals[lo] = x; m->cnts[lo] = 1; d++;
        }
        m->d = d; return NULL;
    }
    for (size_t i = m->lo; i < m->hi; i++) {
        double x = m->a[i]; if (x != x) { m->abort = 1; return NULL; }
        uint64_t k = total_key(x);
        size_t lo;
        if (d <= 32) { lo = 0; for (size_t q = 0; q < d; q++) lo += (m->keys[q] < k); }   /* branchless: no mispredicts */
        else { size_t hi = d; lo = 0; while (lo < hi) { size_t mid = (lo + hi) >> 1; if (m->keys[mid] < k) lo = mid + 1; else hi = mid; } }
        if (lo < d && m->keys[lo] == k) { m->cnts[lo]++; continue; }
        if (d == LOWCARD_CAP) { m->abort = 1; return NULL; }
        memmove(m->keys + lo + 1, m->keys + lo, (d - lo) * sizeof(uint64_t));
        memmove(m->vals + lo + 1, m->vals + lo, (d - lo) * sizeof(double));
        memmove(m->cnts + lo + 1, m->cnts + lo, (d - lo) * sizeof(size_t));
        m->keys[lo] = k; m->vals[lo] = x; m->cnts[lo] = 1; d++;
    }
    m->d = d; return NULL;
}
typedef struct { double *a; size_t lo, hi; const double *gv; const size_t *gstart; size_t gd; int tid; } lcemit_t;
static void *lc_emit_body(void *p);
static void *lc_emit(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = lc_emit_body(p); PROF(prof_busy_since(((lcemit_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *lc_emit_body(void *p) {
    lcemit_t *e = p;
    size_t v; { size_t lo = 0, hi = e->gd; while (lo < hi) { size_t mid = (lo + hi) >> 1; if (e->gstart[mid + 1] <= e->lo) lo = mid + 1; else hi = mid; } v = lo; }
    size_t pos = e->lo;
    while (pos < e->hi && v < e->gd) { size_t end = e->gstart[v + 1]; if (end > e->hi) end = e->hi; double val = e->gv[v]; for (; pos < end; pos++) e->a[pos] = val; v++; }
    return NULL;
}
typedef struct { uint64_t k; double v; size_t c; } lcpair_t;
static int cmp_lc_pair(const void *x, const void *y) { uint64_t a = ((const lcpair_t *)x)->k, b = ((const lcpair_t *)y)->k; return (a > b) - (a < b); }
/* Sort (key, value, count) triples by key. The key is total_key(double) and monotone in the value,
 * so this sorts by value too.
 *
 * This replaced libc qsort, which the project bans on every sampled path for the usual reason and
 * which was measured here doing real damage: traced on a 60k-distinct lattice, qsort of 60k pairs
 * took 147 ms of a 2268 ms sort, because every one of the ~1M comparisons is an indirect call that
 * cannot inline. A byte-skip LSD radix is a handful of linear passes over 24-byte records.
 *
 * The byte skip is what makes it cheap rather than merely faster: cardinality-limited data varies in
 * only two to four of the eight key bytes, and a constant byte costs one OR/AND pass to detect and
 * then no pass at all. `tmp` must hold n pairs. */
static void lc_sort_pairs(lcpair_t *p, size_t n, lcpair_t *tmp) {
    if (n < 2) return;
    if (getenv("LOGOS_MC_QSORT")) { qsort(p, n, sizeof(lcpair_t), cmp_lc_pair); return; }   /* for the A/B */
    if (n <= 48) {
        for (size_t i = 1; i < n; i++) { lcpair_t k = p[i]; size_t j = i; while (j && p[j - 1].k > k.k) { p[j] = p[j - 1]; j--; } p[j] = k; }
        return;
    }
    uint64_t or_a = 0, and_a = ~(uint64_t)0;
    for (size_t i = 0; i < n; i++) { or_a |= p[i].k; and_a &= p[i].k; }
    uint64_t diff = or_a ^ and_a;
    lcpair_t *src = p, *dst = tmp;
    for (int b = 0; b < 8; b++) {
        if (!((diff >> (b * 8)) & 0xFFu)) continue;          /* constant byte: no pass needed */
        int sh = b * 8;
        size_t cnt[257]; memset(cnt, 0, sizeof cnt);
        for (size_t i = 0; i < n; i++) cnt[((src[i].k >> sh) & 0xFFu) + 1]++;
        for (int c = 0; c < 256; c++) cnt[c + 1] += cnt[c];
        for (size_t i = 0; i < n; i++) dst[cnt[(src[i].k >> sh) & 0xFFu]++] = src[i];
        lcpair_t *t = src; src = dst; dst = t;
    }
    if (src != p) memcpy(p, src, n * sizeof(lcpair_t));
}
/* The sample's reads are independent cache misses; issuing every prefetch before the first read lets the
 * core overlap them (the line-fill buffers hold ~16). Measured: the two stages together 1.05 -> ~0.15 ms
 * (make probebench, -DLOGOS_NO_SAMPLE_FAST). */
static void sample_read(const double *a, size_t n, size_t from, size_t to, size_t mfull, double *out, int *has_nan) {
    (void)mfull;
#ifndef LOGOS_NO_SAMPLE_FAST
    for (size_t i = from; i < to; i++) { size_t q = SAMPLE_POS(i, mfull, n); __builtin_prefetch(a + (q < n ? q : n - 1), 0, 0); }
#endif
    for (size_t i = from; i < to; i++) { size_t q = SAMPLE_POS(i, mfull, n); double v = a[q < n ? q : n - 1]; out[i - from] = v; if (v != v) *has_nan = 1; }
}
/* Sorts a sample in place. NaNs are compacted to the tail first (the count is returned; every gate declines
 * on any NaN, so their order never matters) and the rest goes to the AVX2 quicksort, or to the engine's own
 * scalar introsort where AVX2 is absent. No libc qsort: its callback compare is 5-10x the cost of either. */
static size_t sample_sort(double *s, size_t m) {
    size_t k = 0;
    for (size_t i = 0; i < m; i++) { double v = s[i]; if (v == v) s[k++] = v; }
    for (size_t i = k; i < m; i++) s[i] = NAN;
#ifdef LOGOS_AVX2_LEAF
    if (k >= 2 && avx2_leaf_on() && avx2_qsort_f64(s, k)) return m - k;
#endif
    if (k >= 2) quicksort(s, 0, (long)k - 1, 2 * log2i(k));
    return m - k;
}
/* returns 1 if it sorted a[], 0 if it declined (a[] untouched) */
static int lowcard_try_f64(double *a, size_t n, int P, const double *ext, size_t ext_m) {
    P = count_threads(n, P);
    double *own = NULL; const double *S = ext; size_t sm = ext_m;
    if (!ext) {                                                /* gate: strided sample distinct count */
        size_t m = n < 2048 ? n : 2048;
        own = malloc(m * sizeof(double)); if (!own) return 0;
        for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); own[i] = a[q < n ? q : n - 1]; }
        size_t nan_n = sample_sort(own, m);
        size_t distinct = 1; for (size_t i = 1; i < m - nan_n; i++) if (own[i] != own[i - 1]) distinct++;
        if (nan_n || distinct > LOWCARD_MAX_SCAN) { free(own); return 0; }
        S = own; sm = m - nan_n;
    }
    /* verified perfect hash over the sample's distinct values (see lc_map_body) */
    uint64_t hmul = 0; size_t sd = 0; double sv[LOWCARD_SAMPLE_MAX]; uint64_t sk[LOWCARD_SAMPLE_MAX];
    uint8_t slot[1u << LC_PHASH_BITS];
#ifndef LOGOS_NO_LC_PHASH
    /* Distinct values of the sorted sample, then a multiply-shift perfect hash over their keys. The table is
     * cleared every attempt and the attempt is checked by reading it back, so a colliding multiplier simply
     * fails and no entry can ever index past the key array: every slot holds a real key index and every
     * lookup is verified by a compare. */
    for (size_t i = 0; i < sm; i++)
        if (sd == 0 || S[i] != sv[sd - 1]) { if (sd == LOWCARD_SAMPLE_MAX || sd == 255) { sd = 0; break; } sv[sd] = S[i]; sk[sd] = total_key(S[i]); sd++; }
    if (sd >= 1) {
        uint64_t seed = 0x9E3779B97F4A7C15ULL ^ (uint64_t)sd;
        for (int attempt = 0; attempt < 24 && !hmul; attempt++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; uint64_t mul = seed | 1; int ok = 1;
            memset(slot, 0, sizeof slot);
            for (size_t q = 0; q < sd; q++) slot[(unsigned)((sk[q] * mul) >> (64 - LC_PHASH_BITS))] = (uint8_t)q;
            for (size_t q = 0; q < sd && ok; q++) if (slot[(unsigned)((sk[q] * mul) >> (64 - LC_PHASH_BITS))] != (uint8_t)q) ok = 0;
            if (ok) hmul = mul;
        }
    }
#endif
    if (own) free(own);
    TRACE("lowcard: sample %zu values, %zu distinct, perfect hash %s", sm, sd, hmul ? "built" : "none");
    (void)S; (void)sm; (void)sv; (void)sk;

    size_t chunk = (n + P - 1) / P;
    lcmap_t *mp = calloc((size_t)P, sizeof(lcmap_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!mp || !th) { free(mp); free(th); return 0; }
    int ok = 1;
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        mp[t].keys = malloc(LOWCARD_CAP * sizeof(uint64_t)); mp[t].vals = malloc(LOWCARD_CAP * sizeof(double)); mp[t].cnts = malloc(LOWCARD_CAP * sizeof(size_t));
        if (!mp[t].keys || !mp[t].vals || !mp[t].cnts) ok = 0;
        if (hmul) { mp[t].hmul = hmul; mp[t].hkey = sk; mp[t].slot = slot; mp[t].sd = sd; mp[t].hcnt = calloc(sd, sizeof(size_t)); if (!mp[t].hcnt) ok = 0; }
    }
    int sorted = 0;
    if (ok) {
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lc_map, &mp[t]);
        lc_map(&mp[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        int aborted = 0; size_t total = 0, over = 0;
        for (int t = 0; t < P; t++) { if (mp[t].abort) aborted = 1; total += mp[t].d + (hmul ? sd : 0); over += mp[t].d; }
        TRACE("lowcard: map done, %zu entries the hash did not cover%s", over, aborted ? ", ABORTED" : "");
        if (!aborted) {
            lcpair_t *pairs = malloc((total ? total : 1) * sizeof(lcpair_t));
            lcpair_t *ptmp = malloc((total ? total : 1) * sizeof(lcpair_t));
            double *gv = malloc((total ? total : 1) * sizeof(double));
            size_t *gcnt = malloc((total ? total : 1) * sizeof(size_t));
            size_t *gstart = malloc((total + 2) * sizeof(size_t));
            if (pairs && ptmp && gv && gcnt && gstart) {
                size_t w = 0;
                for (int t = 0; t < P; t++) for (size_t i = 0; i < mp[t].d; i++) { pairs[w].k = mp[t].keys[i]; pairs[w].v = mp[t].vals[i]; pairs[w].c = mp[t].cnts[i]; w++; }
                if (hmul) for (int t = 0; t < P; t++) for (size_t q = 0; q < sd; q++) if (mp[t].hcnt[q]) { pairs[w].k = sk[q]; pairs[w].v = sv[q]; pairs[w].c = mp[t].hcnt[q]; w++; }
                lc_sort_pairs(pairs, w, ptmp);
                /* fold equal keys across threads into (value, total count) */
                size_t gd = 0;
                for (size_t i = 0; i < w; i++) {
                    if (gd > 0 && pairs[i].k == pairs[i - 1].k) { gcnt[gd - 1] += pairs[i].c; continue; }
                    gv[gd] = pairs[i].v; gcnt[gd] = pairs[i].c; gd++;
                }
                /* prefix sums -> each value's output start; gstart[gd] == n */
                gstart[0] = 0; for (size_t i = 0; i < gd; i++) gstart[i + 1] = gstart[i] + gcnt[i];
                lcemit_t *em = malloc((size_t)P * sizeof(lcemit_t));
                if (em) {
                    for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * chunk; em[t].hi = em[t].lo + chunk; if (em[t].hi > n) em[t].hi = n; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; }
                    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lc_emit, &em[t]);
                    lc_emit(&em[0]);
                    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
                    free(em); sorted = 1;
                }
            }
            free(ptmp); free(pairs); free(gv); free(gcnt); free(gstart);
        }
    }
    for (int t = 0; t < P; t++) { free(mp[t].keys); free(mp[t].vals); free(mp[t].cnts); free(mp[t].hcnt); }
    free(mp); free(th);
    return sorted;
}


/* ======== moderate-cardinality parallel counting (hash map, zero scatter) ========
 * Generalizes the low-cardinality path to thousands of distinct values. The
 * fixed 2048-sample only answers a PROPORTION question ("enough repeats to be
 * worth trying?" — a fixed sample is statistically right for that); the real
 * probe is a CAPPED first pass: every thread builds a hash map key->count over
 * its slice and aborts the instant it exceeds MODCARD_CAP distinct values, so a
 * failed attempt costs O(cap) per thread, never O(n). When it succeeds it is
 * already pass one: the output of a cardinality-limited sort is fully
 * determined by (distinct values, counts), so pass two just regenerates the
 * output as runs — no element is ever moved, no scattered writes at all.
 * Keys are the IEEE total-order bits (bit-exact output, -0/+0 distinct). */
#define MODCARD_CAP     65536
#define MODCARD_MIN_N   65536
#define MODCARD_GATE    0.98        /* sequential: sample distinct <= 0.98*m, i.e. >=2% repeats (birthday bound d <~ 50k) */
#ifndef MC_HASH_CHAO1_SEQ
#define MC_HASH_CHAO1_SEQ 128
#endif
#ifndef MC_HASH_CHAO1_MT
#define MC_HASH_CHAO1_MT 3300
#endif
#ifndef MC_HASH_CHAO1_RADIX
#define MC_HASH_CHAO1_RADIX 3300
#endif
#ifndef LATTICE_MT_MAX_SLOTS
#define LATTICE_MT_MAX_SLOTS 65536
#endif
#define MODCARD_GATE_MT 0.995       /* many threads: the map pass parallelizes, so attempt up to the cap (>=0.5% repeats) */
typedef struct { uint64_t *keys; double *vals; size_t *cnts; size_t cap, used; } hmap_t;
static inline uint64_t mix64(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static int hmap_init(hmap_t *h, size_t slots) {
    h->cap = slots; h->used = 0;
    h->keys = malloc(slots * sizeof(uint64_t)); h->vals = malloc(slots * sizeof(double)); h->cnts = malloc(slots * sizeof(size_t));
    if (!h->keys || !h->vals || !h->cnts) return 0;
    memset(h->keys, 0xFF, slots * sizeof(uint64_t));   /* empty = UINT64_MAX (the key of +NaN, which never enters) */
    return 1;
}
static void hmap_free(hmap_t *h) { free(h->keys); free(h->vals); free(h->cnts); h->keys = NULL; h->vals = NULL; h->cnts = NULL; }
static int hmap_grow(hmap_t *h) {
    hmap_t g; if (!hmap_init(&g, h->cap * 2)) { hmap_free(&g); return 0; }
    for (size_t i = 0; i < h->cap; i++) if (h->keys[i] != UINT64_MAX) {
        size_t q = mix64(h->keys[i]) & (g.cap - 1); while (g.keys[q] != UINT64_MAX) q = (q + 1) & (g.cap - 1);
        g.keys[q] = h->keys[i]; g.vals[q] = h->vals[i]; g.cnts[q] = h->cnts[i]; }
    g.used = h->used; hmap_free(h); *h = g; return 1;
}
/* add `c` occurrences of value v (key k); returns 0 when aborted (cap exceeded / alloc failure) */
static int hmap_add(hmap_t *h, uint64_t k, double v, size_t c, size_t maxd) {
    for (;;) {
        size_t q = mix64(k) & (h->cap - 1);
        for (;;) {
            uint64_t kk = h->keys[q];
            if (kk == k) { h->cnts[q] += c; return 1; }
            if (kk == UINT64_MAX) {
                if (h->used >= maxd) return 0;
                if (h->used * 2 >= h->cap) break;                 /* grow, then re-probe */
                h->keys[q] = k; h->vals[q] = v; h->cnts[q] = c; h->used++; return 1;
            }
            q = (q + 1) & (h->cap - 1);
        }
        if (!hmap_grow(h)) return 0;
    }
}
typedef struct { const double *a; size_t lo, hi; hmap_t h; int abort; int tid; } mcmap_t;
static void *mc_map_body(void *p);
static void *mc_map(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = mc_map_body(p); PROF(prof_busy_since(((mcmap_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *mc_map_body(void *p) {
    mcmap_t *m = p;
    for (size_t i = m->lo; i < m->hi; i++) {
        double x = m->a[i];
        if (x != x || !hmap_add(&m->h, total_key(x), x, 1, MODCARD_CAP)) {
            m->abort = 1;
            TRACE("modcard: thread gave up %zu of %zu elements in (%.0f%% scanned), %zu distinct so far",
                  i - m->lo, m->hi - m->lo, 100.0 * (double)(i - m->lo) / (double)(m->hi - m->lo ? m->hi - m->lo : 1), m->h.used);
            return NULL;
        }
    }
    return NULL;
}

/* ============ lattice (direct-index) counting ============
 * Most quantized data is a LATTICE: every value an integer multiple of one step
 * from a base (sensor ticks, cents, k/64). Then the slot index is arithmetic,
 * round((v - base) / step), and no hashing is needed. The sample proposes the
 * step (smallest positive gap between distinct sample values) and the pass
 * verifies it per element; anything off-lattice, out of range, or two distinct
 * doubles on one lattice point (0.3 vs 0.1*3) aborts to the hash path. Each
 * slot keeps the exact key, so the output is bit-exact. */
/* The slot ceiling was a flat 65536 and a table that did not fit was DECLINED outright. Both halves
 * were wrong, in exactly the way the int32 counting route's fixed cap was wrong before
 * count_try_i32 replaced it with a budget: the real constraint is MEMORY, which scales with the
 * thread count, and a table that does not fit should first try a SMALLER MARGIN rather than give up.
 * A perfect 60000-value lattice was falling all the way to the hash route because its eighth-of-span
 * margin pushed 60000 slots to 75128. Shrinking the margin risks an abort, which is recoverable;
 * declining is guaranteed to be slower. Set LATTICE_MAX_BYTES to bound the per-sort table memory
 * (P tables of 24 bytes a slot); LATTICE_MAX_SLOTS is the absolute per-table ceiling that keeps the
 * serial slot merge, which walks slots x P, from growing without limit. */
#ifndef LATTICE_MAX_BYTES
#define LATTICE_MAX_BYTES (64u << 20)
#endif
#ifndef LATTICE_MAX_SLOTS
#define LATTICE_MAX_SLOTS 262144
#endif
#define LATTICE_SLOT_BYTES (sizeof(uint64_t) + sizeof(double) + sizeof(size_t))
#define LATTICE_MIN_SLOTS 4096
typedef struct { uint64_t *keys; double *vals; size_t *cnts; } lslots_t;
typedef struct { const double *a; size_t lo, hi; double base, step; long margin, slots; lslots_t t; int abort; int tid; } lattice_arg_t;
static void *lattice_map_body(void *p);
static void *lattice_map(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = lattice_map_body(p); PROF(prof_busy_since(((lattice_arg_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *lattice_map_body(void *p) {
    lattice_arg_t *m = p; const double inv = 1.0 / m->step;
    for (size_t i = m->lo; i < m->hi; i++) {
        double x = m->a[i]; if (x != x) { m->abort = 1; return NULL; }
        double q = (x - m->base) * inv; double qi = (double)(long)(q + (q >= 0 ? 0.5 : -0.5));
        double err = q - qi; if (err < 0) err = -err;
        double tol = 1e-9 * (qi >= 0 ? qi : -qi) + 1e-9;
        if (err > tol) { m->abort = 1; return NULL; }                        /* off-lattice */
        long idx = (long)qi + m->margin; if (idx < 0 || idx >= m->slots) { m->abort = 1; return NULL; }
        uint64_t k = total_key(x), kk = m->t.keys[idx];
        if (kk == k) { m->t.cnts[idx]++; continue; }
        if (kk != UINT64_MAX) { m->abort = 1; return NULL; }                 /* two doubles on one lattice point */
        m->t.keys[idx] = k; m->t.vals[idx] = x; m->t.cnts[idx] = 1;
    }
    return NULL;
}
/* returns 1 if sorted, 0 if declined (a[] untouched). samp: sorted sample of m values. */
static int lattice_try_f64(double *a, size_t n, int P, const double *samp, size_t m) {
    /* distinct sample values -> candidate step */
    double smin = samp[0], smax = samp[m - 1]; double step = INFINITY; size_t distinct = 1;
    for (size_t i = 1; i < m; i++) if (samp[i] != samp[i - 1]) { double g = samp[i] - samp[i - 1]; if (g < step) step = g; distinct++; }
    if (distinct < 3 || !(step > 0) || !isfinite(step)) return 0;
    for (size_t i = 0; i < m; i++) { double q = (samp[i] - smin) / step, qi = (double)(long)(q + 0.5); double e = q - qi; if (e < 0) e = -e; if (e > 1e-9 * (qi + 1)) return 0; }
    long cap_slots = (long)(LATTICE_MAX_BYTES / LATTICE_SLOT_BYTES / (size_t)(P < 1 ? 1 : P));
    if (cap_slots > LATTICE_MAX_SLOTS) cap_slots = LATTICE_MAX_SLOTS;
    if (cap_slots < LATTICE_MIN_SLOTS) cap_slots = LATTICE_MIN_SLOTS;
#ifdef LATTICE_CAP_OLD
    cap_slots = 65536;
#endif
    if (getenv("LOGOS_LATTICE_OLD")) cap_slots = 65536;   /* one binary can A/B the budget; see latbench */
    double spanD = (smax - smin) / step; if (!(spanD >= 0) || spanD > (double)cap_slots) return 0;
    long span = (long)(spanD + 0.5) + 1;
#ifdef LATTICE_MARGIN_OLD
    long margin = span / 8 + 64;
#else
    /* A sample of 2048 misses the tail of integer data (Collatz stopping times: sample max 444, true max
       664), and the map pass gave up on the first value outside the window after scanning 19-44% of the
       array (5-12 ms wasted at 1T, chaos zoo). Small tables are widened to LATTICE_MIN_SLOTS for free
       (Collatz: 1.23x at 1T, 1.55x at 14T); a proportional margin was measured to cost quantized 13% at
       14T (its 4096-slot table tripled, zeroed and scanned per thread), so large tables keep the eighth. */
    long margin = span / 8 + 64; if (span + 2 * margin < LATTICE_MIN_SLOTS) margin = (LATTICE_MIN_SLOTS - span) / 2;
#endif
    /* Too wide for the budget: take the widest margin that still fits before giving up. */
    if (span + 2 * margin > cap_slots && !getenv("LOGOS_LATTICE_OLD")) { margin = (cap_slots - span) / 2; if (margin < 0) margin = 0; }
    long slots = span + 2 * margin;
    if (slots > cap_slots) return 0;
    /* Many threads: the slot merge walks slots x P on one thread. Measured (make cardgate): a 40128-slot table broke
     * even at 14 threads, an 82048-slot one cost 1.6-1.9x against the partition. LOGOS_MCGATE_OLD=1 lifts this. */
    if (P >= WC_MIN_THREADS && slots > LATTICE_MT_MAX_SLOTS && !getenv("LOGOS_MCGATE_OLD")) return 0;

    size_t chunk = (n + P - 1) / P;
    lattice_arg_t *mp = calloc((size_t)P, sizeof(lattice_arg_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!mp || !th) { free(mp); free(th); return 0; }
    int ok = 1;
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        mp[t].base = smin; mp[t].step = step; mp[t].margin = margin; mp[t].slots = slots;
        mp[t].t.keys = malloc((size_t)slots * sizeof(uint64_t)); mp[t].t.vals = malloc((size_t)slots * sizeof(double)); mp[t].t.cnts = calloc((size_t)slots, sizeof(size_t));
        if (!mp[t].t.keys || !mp[t].t.vals || !mp[t].t.cnts) ok = 0; else memset(mp[t].t.keys, 0xFF, (size_t)slots * sizeof(uint64_t));
    }
    int sorted = 0;
    if (ok) {
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lattice_map, &mp[t]);
        lattice_map(&mp[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        int aborted = 0; for (int t = 0; t < P; t++) if (mp[t].abort) aborted = 1;
        if (!aborted) {
            /* merge by index: keys must agree across threads; counts sum. Index order == value order. */
            double *gv = malloc((size_t)slots * sizeof(double)); size_t *gstart = malloc(((size_t)slots + 1) * sizeof(size_t));
            lcemit_t *em = malloc((size_t)P * sizeof(lcemit_t));
            if (gv && gstart && em) {
                size_t gd = 0, acc = 0; int conflict = 0;
                for (long i = 0; i < slots && !conflict; i++) {
                    uint64_t key = UINT64_MAX; size_t cnt = 0; double val = 0;
                    for (int t = 0; t < P; t++) { uint64_t kk = mp[t].t.keys[i]; if (kk == UINT64_MAX) continue;
                        if (key == UINT64_MAX) { key = kk; val = mp[t].t.vals[i]; } else if (kk != key) { conflict = 1; break; }
                        cnt += mp[t].t.cnts[i]; }
                    if (cnt) { gv[gd] = val; gstart[gd] = acc; acc += cnt; gd++; }
                }
                if (!conflict && acc == n) {
                    gstart[gd] = acc;
                    for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * chunk; em[t].hi = em[t].lo + chunk; if (em[t].hi > n) em[t].hi = n; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; }
                    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lc_emit, &em[t]);
                    lc_emit(&em[0]);
                    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
                    sorted = 1;
                }
            }
            free(gv); free(gstart); free(em);
        }
    }
    for (int t = 0; t < P; t++) { free(mp[t].t.keys); free(mp[t].t.vals); free(mp[t].t.cnts); }
    free(mp); free(th);
    return sorted;
}

/* Whether the HASH MAP may run, from the sample's Chao1 estimate. Measured (make cardgate; ladder and skew modes,
 * 8.4M, paired): below 8 threads the hash map lost to the fallback on every equal-odds shape from 300 distinct values
 * up (1.09-1.35x) and on Zipf activity from 1k up (1.03-1.24x); it won only when a handful of values carry the data
 * (floor of a Pareto, sample Chao1 31-50: 3x). At 14 threads its serial merge decides: it won up to a sample Chao1 of
 * 2734 (0.22-0.99x) and lost from 3979 up (1.12-5.30x), skewed or not. The old gate let everything to Chao1 65536
 * through. Thread counts 2-7 were not measured and take the sequential threshold. LOGOS_MCGATE_OLD=1 restores it. */
/* Version 2, below 8 threads. What decides a 1-thread hash map is not the data but the FALLBACK it would replace
 * (make cardgate, intness mode): leaf_f64 sends data whose key bytes vary in at most QUANT_PROBE_MAX of 8 positions to
 * radix_f64_full, a multi-pass float radix, and the hash map beats that (0.42-0.54x on exponential seconds, integer or
 * half-integer alike); everything else goes to the AVX2 quicksort, which the hash map loses to (1.17-1.60x, integers
 * or not). So the gate asks the leaf's own question of the sample. Version 1 (Chao1 <= 128 only) missed exactly that
 * case on a held-out shape. LOGOS_MCGATE_V1=1 restores version 1. The sample probe is 384-2048 points where the leaf
 * reads 4096, so a rare value with an extra varying byte can make the two disagree near the threshold. */
static int sample_quant_passes(const double *s, size_t m) {
    uint32_t or_h = 0, and_h = 0xFFFFFFFFu, or_l = 0, and_l = 0xFFFFFFFFu;
    for (size_t i = 0; i < m; i++) { uint32_t tl, th; key_parts(s[i], &tl, &th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; }
    int passes = 0;
    for (int p = 0; p < 8; p++) { int hi_word = p >= 4, sh = (p & 3) << 3; uint32_t o = hi_word ? or_h : or_l, an = hi_word ? and_h : and_l; if (((o >> sh) & 0xFF) != ((an >> sh) & 0xFF)) passes++; }
    return passes;
}
static inline int mc_hash_allowed(double chao1, int P, size_t n, const double *samp, size_t m) {
    if (getenv("LOGOS_MCGATE_OLD")) return 1;
    if (P >= WC_MIN_THREADS) return chao1 <= (double)MC_HASH_CHAO1_MT;
    if (chao1 <= (double)MC_HASH_CHAO1_SEQ) return 1;
    if (getenv("LOGOS_MCGATE_V1")) return 0;
    return n >= TYPED_MIN_N && n <= RADIX64_MAX_N && chao1 <= (double)MC_HASH_CHAO1_RADIX && samp && m && sample_quant_passes(samp, m) <= QUANT_PROBE_MAX;
}
static int modcard_try_f64(double *a, size_t n, int P, const double *ext, size_t ext_m, int allow_hash) {
    P = count_threads(n, P);
    size_t m = ext ? ext_m : (n < 2048 ? n : 2048);
    double *samp = ext ? (double *)ext : malloc(m * sizeof(double)); if (!samp) return 0;
    if (!ext) {
        for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); samp[i] = a[q < n ? q : n - 1]; }
        sample_sort(samp, m);
        size_t distinct = 1; int has_nan = 0;
        for (size_t i = 1; i < m; i++) { if (samp[i] != samp[i - 1]) distinct++; if (samp[i] != samp[i]) has_nan = 1; }
        double gate = P >= WC_MIN_THREADS ? MODCARD_GATE_MT : MODCARD_GATE;
        if (has_nan || (double)distinct > gate * (double)m) { free(samp); return 0; }   /* too few repeats: not cardinality-limited */
    }
#ifndef LOGOS_NO_LATTICE
    if (lattice_try_f64(a, n, P, samp, m)) { PROF(g_prof.route = LOGOS_ROUTE_LATTICE;) if (!ext) free(samp); return 1; }    /* direct-index when the data is a lattice */
#endif
    if (!ext) free(samp);
    if (!allow_hash || getenv("LOGOS_MC_HASH_OFF")) return 0;  /* the Chao1 gate (mc_hash_allowed); the env var is a measurement toggle */

    size_t chunk = (n + P - 1) / P;
    mcmap_t *mp = calloc((size_t)P, sizeof(mcmap_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!mp || !th) { free(mp); free(th); return 0; }
    int ok = 1;
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        if (!hmap_init(&mp[t].h, 4096)) ok = 0;                 /* small start; grows with the data, stays cache-resident */
    }
    int sorted = 0;
    if (ok) {
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, mc_map, &mp[t]);
        mc_map(&mp[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        int aborted = 0; size_t tot = 0; for (int t = 0; t < P; t++) { if (mp[t].abort) aborted = 1; tot += mp[t].h.used; }
        TRACE("modcard: map phase done, %d threads, aborted=%d, %zu entries across the tables", P, aborted, tot);
        if (!aborted) {
            /* The merge used to build ONE global hash by re-inserting every entry of every thread's
             * table, single-threaded, growing and rehashing as it went: traced at 221 ms of a 2268 ms
             * sort on a 60k-distinct lattice, on top of a 147 ms qsort after it. Both are gone. This
             * is the shape the low-cardinality route already used: concatenate the tables, sort the
             * concatenation by key once, and fold equal keys in a single linear pass. No hash, no
             * rehashing, and the sort is the byte-skip radix above. */
            /* MERGING THE PER-THREAD TABLES. Two strategies, and the obvious-looking one lost.
             *
             * Default (below): one global hash, every thread's entries re-inserted into it. Serial,
             * and it rehashes as it grows, which is genuinely ugly.
             *
             * Opt-in `LOGOS_MC_CONCAT=1`: concatenate the tables, sort the concatenation by key, fold
             * equal keys in one linear pass. No hash at all, and it is what the low-cardinality route
             * does. It is SLOWER above one thread and the reason is arithmetic: the concatenation is
             * P*d pairs where the union is only d, so at 14 threads it sorts fourteen times the data
             * the hash merge touches. Measured (make mcbench, paired, one binary): 0.90 on 5k
             * distinct, 0.72 on 30k, 0.58 on 60k and 0.80 on zipf at 14 threads, against 0.98-1.14 at
             * ONE thread where P*d == d and there is no redundancy. Kept because it is the right
             * shape for a future parallel merge: partition the key space P ways first and this
             * becomes P independent sorts of d/P pairs, which is the fix the serial merge still needs.
             *
             * What DID win, in both strategies, is the sort: libc qsort is gone (see lc_sort_pairs). */
            if (getenv("LOGOS_MC_CONCAT")) goto mc_concat;
            {
                hmap_t g; int gok = hmap_init(&g, 8192);
                for (int t = 0; gok && t < P; t++) for (size_t i = 0; i < mp[t].h.cap; i++)
                    if (mp[t].h.keys[i] != UINT64_MAX && !hmap_add(&g, mp[t].h.keys[i], mp[t].h.vals[i], mp[t].h.cnts[i], (size_t)-1)) gok = 0;
                if (gok) {
                    size_t gd = g.used;
                    lcpair_t *op = malloc((gd ? gd : 1) * sizeof(lcpair_t)), *ot = malloc((gd ? gd : 1) * sizeof(lcpair_t));
                    double *ov = malloc((gd ? gd : 1) * sizeof(double)); size_t *os_ = malloc((gd + 2) * sizeof(size_t));
                    lcemit_t *oe = malloc((size_t)P * sizeof(lcemit_t));
                    if (op && ot && ov && os_ && oe) {
                        size_t w = 0; for (size_t i = 0; i < g.cap; i++) if (g.keys[i] != UINT64_MAX) { op[w].k = g.keys[i]; op[w].v = g.vals[i]; op[w].c = g.cnts[i]; w++; }
                        lc_sort_pairs(op, w, ot);        /* the OLD merge; the qsort itself is gone for good */
                        os_[0] = 0; for (size_t i = 0; i < w; i++) { ov[i] = op[i].v; os_[i + 1] = os_[i] + op[i].c; }
                        for (int t = 0; t < P; t++) { oe[t].a = a; PROF(oe[t].tid = t;) oe[t].lo = (size_t)t * chunk; oe[t].hi = oe[t].lo + chunk; if (oe[t].hi > n) oe[t].hi = n; oe[t].gv = ov; oe[t].gstart = os_; oe[t].gd = w; }
                        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lc_emit, &oe[t]);
                        lc_emit(&oe[0]);
                        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
                        sorted = 1;
                    }
                    free(op); free(ot); free(ov); free(os_); free(oe);
                }
                hmap_free(&g);
                goto mc_done;
            }
mc_concat: ;
            lcpair_t *pairs = malloc((tot ? tot : 1) * sizeof(lcpair_t));
            lcpair_t *ptmp = malloc((tot ? tot : 1) * sizeof(lcpair_t));
            double *gv = malloc((tot ? tot : 1) * sizeof(double)); size_t *gstart = malloc((tot + 2) * sizeof(size_t));
            lcemit_t *em = malloc((size_t)P * sizeof(lcemit_t));
            if (pairs && ptmp && gv && gstart && em) {
                size_t w = 0;
                for (int t = 0; t < P; t++) for (size_t i = 0; i < mp[t].h.cap; i++)
                    if (mp[t].h.keys[i] != UINT64_MAX) { pairs[w].k = mp[t].h.keys[i]; pairs[w].v = mp[t].h.vals[i]; pairs[w].c = mp[t].h.cnts[i]; w++; }
                TRACE("modcard: %zu pairs concatenated, sorting them", w);
                lc_sort_pairs(pairs, w, ptmp);
                TRACE("modcard: pairs sorted");
                size_t gd = 0; gstart[0] = 0;
                for (size_t i = 0; i < w; i++) {
                    if (gd > 0 && pairs[i].k == pairs[i - 1].k) { gstart[gd] += pairs[i].c; continue; }   /* same value, another thread */
                    gv[gd] = pairs[i].v; gstart[gd + 1] = gstart[gd] + pairs[i].c; gd++;
                }
                TRACE("modcard: folded to %zu distinct", gd);
                for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * chunk; em[t].hi = em[t].lo + chunk; if (em[t].hi > n) em[t].hi = n; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; }
                for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, lc_emit, &em[t]);
                lc_emit(&em[0]);
                for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
                TRACE("modcard: emit done");
                sorted = 1;
            }
            free(pairs); free(ptmp); free(gv); free(gstart); free(em);
        }
mc_done: ;
    }
    for (int t = 0; t < P; t++) hmap_free(&mp[t].h);
    free(mp); free(th);
    return sorted;
}


/* ============ persistent buffer pool ============
 * Every sort used to malloc and first-touch ~9n bytes of scratch (a 64 MB
 * scatter buffer + bucket-index cache for 8M doubles): tens of thousands of
 * page faults per call, paid to the kernel (or, under WSL2, to Windows). A
 * process-wide pool keeps the largest scratch seen and hands it back on the
 * next call. Guarded by a mutex; if a sort is already holding the pool
 * (concurrent sorts from different threads), the caller allocates privately.
 * logos_release_buffers() frees it. */
/* ---------------- large allocations, on huge pages where the OS allows ----------------
 * The top-level scatter writes across the whole destination in BUCKET order, which is close to random
 * with respect to PAGES. On the reference box the second-level TLB holds about 2048 entries, so at the
 * default 4 KB page it covers 8 MB, against a working set near 128 MB for an 8M-element sort. That is
 * roughly one translation miss per store, each costing a page walk. At 2 MB pages the same 2048 entries
 * cover 4 GB and the misses disappear.
 *
 * Linux commonly ships transparent huge pages in `madvise` mode (it is set that way here), which means
 * they are handed out ONLY when asked for, and the engine measured AnonHugePages at 0 kB before this
 * existed. Asking is not enough on its own either: the kernel can only promote a region whose start and
 * length line up with the huge page, so the alignment matters as much as the advice.
 *
 * MEASURED NEUTRAL, and therefore OPT-IN (-DLOGOS_HUGEPAGE). The mechanism demonstrably fires -- with
 * it on, the process holds ~98 MB of AnonHugePages instead of none -- and the end-to-end effect is
 * nil: 0.98-1.01 at 8M and 0.97-1.02 at 30M, five shapes at 1 and 14 threads, layout bias cancelled
 * (tools/ledger_rerun.py). Quadrupling the working set changed nothing, which is the clue.
 *
 * The reason is `hf_flush`. The scatter does NOT write one element at a time to a random page: it
 * stages HF_BLOCK (256) doubles per bucket and flushes them as a single 2 KB memcpy. So the write
 * stream is 2 KB contiguous bursts at bucket-random offsets, which touches about one page per 256
 * stores. The TLB miss rate is set by the BURST size, not by the working set, which is exactly why
 * 8M and 30M measure the same. The write-combining buffer, added to batch atomics and fill cache
 * lines, had already made the scatter TLB-friendly as a side effect.
 *
 * Everything below BIG_MIN keeps plain malloc regardless, because rounding a small buffer up to 2 MB
 * costs more than the page walks it would save. Freed with plain free() on both paths. */
#ifndef BIG_ALIGN
#define BIG_ALIGN (2u << 20)
#endif
#ifndef BIG_MIN
#define BIG_MIN   (4u << 20)
#endif
static void *big_alloc(size_t bytes) {
#if !defined(LOGOS_HUGEPAGE) || !defined(MADV_HUGEPAGE)
    return malloc(bytes);                      /* OPT-IN: measured neutral, see the note above */
#else
    if (bytes < BIG_MIN) return malloc(bytes);
    size_t rounded = (bytes + (BIG_ALIGN - 1)) & ~(size_t)(BIG_ALIGN - 1);
    void *p = NULL;
    if (posix_memalign(&p, BIG_ALIGN, rounded) != 0 || !p) return malloc(bytes);   /* fall back, never fail */
    (void)madvise(p, rounded, MADV_HUGEPAGE);
    return p;
#endif
}

#ifndef LOGOS_NO_POOL
static pthread_mutex_t g_pool_m = PTHREAD_MUTEX_INITIALIZER;
static int g_pool_busy = 0;
static void *g_pool_buf = NULL; static size_t g_pool_cap = 0;     /* bytes */
static uint8_t *g_pool_bidx = NULL; static size_t g_pool_bidx_cap = 0;
/* acquire: returns 1 if the pooled buffers were handed out (caller must release), 0 if the caller must allocate */
static int pool_acquire(size_t buf_bytes, size_t bidx_bytes, void **buf, uint8_t **bidx) {
    pthread_mutex_lock(&g_pool_m);
    if (g_pool_busy) { pthread_mutex_unlock(&g_pool_m); return 0; }
    if (g_pool_cap < buf_bytes) { free(g_pool_buf); g_pool_buf = big_alloc(buf_bytes); g_pool_cap = g_pool_buf ? buf_bytes : 0; }
    if (bidx_bytes && g_pool_bidx_cap < bidx_bytes) { free(g_pool_bidx); g_pool_bidx = big_alloc(bidx_bytes); g_pool_bidx_cap = g_pool_bidx ? bidx_bytes : 0; }
    if (!g_pool_buf || (bidx_bytes && !g_pool_bidx)) { pthread_mutex_unlock(&g_pool_m); return 0; }
    g_pool_busy = 1; *buf = g_pool_buf; *bidx = bidx_bytes ? g_pool_bidx : NULL;
    pthread_mutex_unlock(&g_pool_m);
    return 1;
}
static void pool_release(void) { pthread_mutex_lock(&g_pool_m); g_pool_busy = 0; pthread_mutex_unlock(&g_pool_m); }
void logos_release_buffers(void) {
    pthread_mutex_lock(&g_pool_m);
    if (!g_pool_busy) { free(g_pool_buf); g_pool_buf = NULL; g_pool_cap = 0; free(g_pool_bidx); g_pool_bidx = NULL; g_pool_bidx_cap = 0; }
    pthread_mutex_unlock(&g_pool_m);
}
#else
static int pool_acquire(size_t a, size_t b, void **c, uint8_t **d) { (void)a; (void)b; (void)c; (void)d; return 0; }
static void pool_release(void) {}
void logos_release_buffers(void) {}
#endif


/* ============ near-sorted parallel path: index-partition + P-way merge ============
 * Value-partitioning scatters the order that the run-merge exploits, so
 * near-sorted data barely sped up in parallel (37 ms -> 28 ms on 20 threads).
 * This path keeps order: split by INDEX into P chunks, sort each chunk in
 * parallel with the full dispatcher (cheap on near-sorted input: run detection
 * and galloping merges), land every sorted chunk in dst, then merge the P
 * sorted runs in ONE pass with no scatter: quantile thresholds from a sample
 * cut every run by binary search into P value bands (free — the runs are
 * sorted), and each thread P-way-merges its band straight into a[]. */
/* Band-first near-sorted route (2026-09-11; the old chunk-sort + P-way heap merge is kept under
 * -DLOGOS_NS_OLD for the A/B, make nsbench). Why: the old route sped up only 1.8x from 1 to 14 threads
 * while IPS4o and x86-simd-sort got 4-7x. Two causes, both structural. (1) Chunking by index made every
 * chunk's value span the WHOLE range (its 1% strays come from anywhere), so the per-chunk dispatcher
 * could no longer take the path the one-thread route took (counting on the integer shape: 36 ms for
 * 8M) and fell to bits-coarse at 5x the cost per element (151 ms of thread time). (2) The 14-way heap
 * merge is comparison work with a mispredicting branch per element. Now: quantile thresholds first;
 * each thread splits its chunk into in-band values (thr[t] <= v < thr[t+1]) and strays, sorts the
 * in-band part, whose span is 1/P of the range, with the full dispatcher; the strays (about 1% of n on
 * near-sorted data, decline above n/8) are gathered, sorted once, and cut by the same thresholds; each
 * band then does ONE two-way merge of its in-band run with its strays straight into a[]. */
typedef struct { double *a, *dst; size_t lo, hi; ctx_t *ctx; double tlo, thi; size_t nin, nstray; } ns_chunk_arg_t;
#ifdef LOGOS_NS_OLD
static void *ns_sort_chunk(void *p) {
    ns_chunk_arg_t *w = p; size_t n = w->hi - w->lo;
    if (n < 2) { if (n == 1) w->dst[w->lo] = w->a[w->lo]; return NULL; }
    PROF(prof_stamp_t s0 = prof_stamp(); w->ctx->path = LOGOS_PATH_NONE;)
    double *r = leaf_f64(w->a + w->lo, n, w->dst + w->lo, w->ctx);
    if (r != w->dst + w->lo) memcpy(w->dst + w->lo, w->a + w->lo, n * sizeof(double));
    PROF(double ms = prof_busy_since(w->ctx->tid, LOGOS_PH_NEARSORTED, s0); prof_path(w->ctx->tid, w->ctx->path, n, ms);)
    return NULL;
}
typedef struct { double *dst, *out; int P; const size_t *rlo, *rhi; size_t *slo, *shi; size_t out_off; int tid; } ns_merge_arg_t;
static void *ns_merge_band_body(void *p);
static void *ns_merge_band(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = ns_merge_band_body(p); PROF(prof_busy_since(((ns_merge_arg_t *)p)->tid, LOGOS_PH_NEARSORTED, s0);) return r; }
static void *ns_merge_band_body(void *p) {
    ns_merge_arg_t *w = p; int P = w->P;
    double hv[256]; int hr[256]; int hn = 0; size_t pos[256];
    for (int r = 0; r < P; r++) {
        pos[r] = w->slo[r];
        if (pos[r] >= w->shi[r]) continue;
        double v = w->dst[pos[r]]; int i = hn++;
        while (i > 0) { int q = (i - 1) >> 1; if (hv[q] <= v) break; hv[i] = hv[q]; hr[i] = hr[q]; i = q; }
        hv[i] = v; hr[i] = r;
    }
    size_t o = w->out_off;
    while (hn > 0) {
        double v = hv[0]; int r = hr[0]; w->out[o++] = v;
        pos[r]++;
        double nv; int nr2;
        if (pos[r] < w->shi[r]) { nv = w->dst[pos[r]]; nr2 = r; } else { hn--; if (hn == 0) break; nv = hv[hn]; nr2 = hr[hn]; }
        int i = 0;
        for (;;) {
            int l = 2 * i + 1; if (l >= hn) break;
            int c = l; if (l + 1 < hn && hv[l + 1] < hv[l]) c = l + 1;
            if (hv[c] >= nv) break;
            hv[i] = hv[c]; hr[i] = hr[c]; i = c;
        }
        hv[i] = nv; hr[i] = nr2;
    }
    return NULL;
}
#endif
static size_t lower_bound_d(const double *a, size_t lo, size_t hi, double v) { while (lo < hi) { size_t m = lo + ((hi - lo) >> 1); if (a[m] < v) lo = m + 1; else hi = m; } return lo; }
#define NS_POISON 16
#ifndef LOGOS_NS_OLD
/* phase 1: one greedy scan of the chunk. An element is KEPT when it lies in the chunk's band, is not below
 * the last kept value, and is not above both of its next two neighbours; everything else is a stray. The
 * kept sequence is sorted by construction (no leaf runs at all; measured: every leaf the dispatcher chose
 * for a chunk, run merge, bits-coarse, int32 radix, cost 16-30 ns/elem, this scan ~1). A displaced element
 * from later in the array is above its neighbours, one from earlier is below the kept maximum; a run of
 * adjacent displaced-high elements can slip through and cost a cascade of rejections until the sequence
 * catches up, which only adds strays, never breaks the order. Kept to the front of the chunk's region in
 * dst, strays to the back. */
static void *ns_split_sort(void *p) {
    ns_chunk_arg_t *w = p; size_t lo = w->lo, hi = w->hi;
    PROF(prof_stamp_t s0 = prof_stamp(); w->ctx->path = LOGOS_PATH_RUN_MERGE;)
    size_t f = lo, b = hi; const double tlo = w->tlo, thi = w->thi; const double *a = w->a; double *dst = w->dst;
    /* Poison recovery: a displaced-high element followed within two positions by a larger displaced-high
       element passes the neighbour test and becomes the kept maximum; everything legitimate after it is
       then "below kmax" until the sequence catches up, which may be the rest of the chunk (measured: 88%
       of 8M rejected). After NS_POISON consecutive in-band rejections for that reason alone, the kept
       maximum is the poison: pop it into the strays, pull the streak back out of the strays, rescan. */
    double kmax = -INFINITY; size_t streak = 0, streak_start = lo;
    for (size_t i = lo; i < hi; i++) {
        double v = a[i];
        double n1 = i + 1 < hi ? a[i + 1] : INFINITY, n2 = i + 2 < hi ? a[i + 2] : INFINITY;
        int inb = (v >= tlo) & (v < thi), ge = v >= kmax, nb = (v <= n1) | (v <= n2);
        if (inb & ge & nb) { dst[f++] = v; kmax = v; streak = 0; continue; }
        if (inb && !ge && f > lo) {
            if (streak == 0) streak_start = i;
            if (++streak >= NS_POISON) {
                double poison = dst[--f]; kmax = f > lo ? dst[f - 1] : -INFINITY;
                b += streak - 1;                                   /* un-push the streak's strays (all but this one, not pushed yet) */
                dst[--b] = poison;
                i = streak_start - 1; streak = 0; continue;        /* re-evaluate the streak under the corrected maximum */
            }
        } else streak = 0;
        dst[--b] = v;
    }
    w->nin = f - lo; w->nstray = hi - b;
    PROF(double ms = prof_busy_since(w->ctx->tid, LOGOS_PH_NEARSORTED, s0); prof_path(w->ctx->tid, w->ctx->path, hi - lo, ms);)
    return NULL;
}
/* phase 2: one band = its sorted in-band run merged with its slice of the sorted strays, into a[] */
typedef struct { const double *run, *strays; size_t nrun, s0, s1; double *out; int tid; } ns_merge2_t;
static void *ns_merge2(void *p) {
    ns_merge2_t *w = p; PROF(prof_stamp_t st = prof_stamp();)
    const double *x = w->run, *xe = x + w->nrun, *y = w->strays + w->s0, *ye = w->strays + w->s1; double *o = w->out;
    while (x < xe && y < ye) { if (*y < *x) *o++ = *y++; else *o++ = *x++; }
    if (x < xe) { memcpy(o, x, (size_t)(xe - x) * sizeof(double)); o += xe - x; }
    if (y < ye) memcpy(o, y, (size_t)(ye - y) * sizeof(double));
    PROF(prof_busy_since(w->tid, LOGOS_PH_NEARSORTED, st);)
    return NULL;
}
#endif
/* returns 1 if sorted, 0 if declined (a[] untouched) */
#define NS_STRAYS_PARALLEL 65536
static _Thread_local int ns_depth = 0;
static int nearsorted_parallel_f64(double *a, size_t n, int P, double *dst, double inv_rate) {
    if (P < 2 || P > 256 || ns_depth > 0) return 0;
    if (inv_rate < 0) {                                        /* gate: the dispatcher's 40-point strided inversion sample */
        size_t ss = n < 40 ? n : 40, st = n / ss; if (st < 1) st = 1; size_t inv = 0, comps = 0;
        for (size_t i = 0; i + st < n; i += st) { if (a[i] > a[i + st]) inv++; comps++; }
        if (comps == 0) return 0;
        inv_rate = (double)inv / comps;
    }
    if (inv_rate > NEARLY_SORTED_INV_RATIO) return 0;

#ifndef LOGOS_NS_OLD
    /* pre-gate: the greedy scan on four 4096-element windows; if it would reject more than a quarter the
       full route would decline after a whole pass and a thread spawn (measured 0.83-0.89x on locally
       shuffled data, where every element sits within +-1000 of home and the partition route is the right one) */
    if (n >= 4 * 4096) {
        size_t rej = 0, tot = 0;
        for (int w = 0; w < 4; w++) {
            size_t lo = (n / 5) * (size_t)(w + 1) - 2048, hi = lo + 4096; double kmax = -INFINITY;
            for (size_t i = lo; i < hi; i++) { double v = a[i], n1 = a[i + 1], n2 = a[i + 2]; if ((v >= kmax) & ((v <= n1) | (v <= n2))) kmax = v; else rej++; tot++; }
        }
        if (rej * 4 > tot) return 0;
    }
#endif
    size_t chunk = (n + P - 1) / P;
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t)); ns_chunk_arg_t *ca = malloc((size_t)P * sizeof(ns_chunk_arg_t));
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    size_t m = (size_t)P * 32; if (m > n) m = n;
    double *samp = malloc(m * sizeof(double)), *thr = malloc((size_t)(P + 1) * sizeof(double));
    if (!ctxs || !ca || !th || !samp || !thr) { free(ctxs); free(ca); free(th); free(samp); free(thr); return 0; }
    int ok = 1, sorted = 0;
    for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); samp[i] = a[q < n ? q : n - 1]; if (samp[i] != samp[i]) ok = 0; }
    if (ok) {
        /* Band thresholds at the CHUNK BOUNDARIES, not sample quantiles: a quantile of a 448-sample lands
           ~n/448 positions off the boundary, and on near-sorted data every element between the two counts
           as a stray (measured: 589k strays of 8M instead of the ~160k displaced ones, and a 16 ms serial
           strays sort). The median of a 33-element window around a[t*chunk] is the boundary value and a
           displaced element there cannot be picked. */
        thr[0] = -INFINITY; thr[P] = INFINITY;
        for (int b = 1; b < P; b++) {
            size_t c0 = (size_t)b * chunk, w0 = c0 >= 16 ? c0 - 16 : 0, w1 = c0 + 17 <= n ? c0 + 17 : n; size_t k = w1 - w0;
            double win[33]; memcpy(win, a + w0, k * sizeof(double)); insertion_sort(win, 0, (long)k - 1); thr[b] = win[k / 2];
            if (thr[b] != thr[b]) ok = 0;
        }
        for (int b = 1; b + 1 < P; b++) if (!(thr[b] <= thr[b + 1])) ok = 0;   /* boundaries must ascend, else this is not near-sorted */
        (void)samp;
        for (int t = 0; ok && t < P; t++) {
            ca[t].a = a; ca[t].dst = dst; ca[t].lo = (size_t)t * chunk; ca[t].hi = ca[t].lo + chunk; if (ca[t].hi > n) ca[t].hi = n; if (ca[t].lo > n) ca[t].lo = n;
            ca[t].ctx = &ctxs[t]; ca[t].tlo = thr[t]; ca[t].thi = thr[t + 1]; ca[t].nin = ca[t].nstray = 0;
            if (!ctx_init(&ctxs[t], chunk, 0)) ok = 0;
            PROF(ctxs[t].tid = t;)
        }
    }
#ifdef LOGOS_NS_OLD
    if (ok) {
        size_t *rlo = malloc((size_t)P * sizeof(size_t)), *rhi = malloc((size_t)P * sizeof(size_t));
        for (int t = 0; t < P; t++) { rlo[t] = ca[t].lo; rhi[t] = ca[t].hi; }
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, ns_sort_chunk, &ca[t]);
        ns_sort_chunk(&ca[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); samp[i] = dst[q < n ? q : n - 1]; }
        sample_sort(samp, m);
        size_t *slo = malloc((size_t)P * P * sizeof(size_t)), *shi = malloc((size_t)P * P * sizeof(size_t));
        size_t *boff = malloc(((size_t)P + 1) * sizeof(size_t)); ns_merge_arg_t *ma = malloc((size_t)P * sizeof(ns_merge_arg_t));
        for (int b = 0; b < P; b++) for (int r = 0; r < P; r++) {
            slo[b * P + r] = (b == 0) ? rlo[r] : lower_bound_d(dst, rlo[r], rhi[r], samp[(size_t)b * m / P]);
            shi[b * P + r] = (b == P - 1) ? rhi[r] : lower_bound_d(dst, rlo[r], rhi[r], samp[(size_t)(b + 1) * m / P]); }
        boff[0] = 0;
        for (int b = 0; b < P; b++) { size_t sz = 0; for (int r = 0; r < P; r++) sz += shi[b * P + r] - slo[b * P + r]; boff[b + 1] = boff[b] + sz; }
        if (boff[P] == n) {
            for (int b = 0; b < P; b++) { ma[b].dst = dst; PROF(ma[b].tid = b;) ma[b].out = a; ma[b].P = P; ma[b].rlo = rlo; ma[b].rhi = rhi; ma[b].slo = slo + b * P; ma[b].shi = shi + b * P; ma[b].out_off = boff[b]; }
            for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, ns_merge_band, &ma[t]);
            ns_merge_band(&ma[0]);
            for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
            sorted = 1;
        }
        if (!sorted) { memcpy(a, dst, n * sizeof(double)); sorted = -1; }
        free(rlo); free(rhi); free(slo); free(shi); free(boff); free(ma);
    }
#else
    if (ok) {
        /* 1. split + sort the in-band part of every chunk, in parallel; a[] is untouched */
        TRACE("ns: thresholds + contexts ready");
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, ns_split_sort, &ca[t]);
        ns_split_sort(&ca[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        size_t S = 0; for (int t = 0; t < P; t++) S += ca[t].nstray;
        TRACE("ns: split + in-band sorts done, %zu strays", S);
        if (S <= n / 4) {                                        /* more strays than that: not near-sorted, decline */
            /* 2. gather and sort the strays once (serial: ~1% of n), cut them by the same thresholds */
            double *strays = malloc((S ? S : 1) * sizeof(double)), *sscr = malloc((S ? S : 1) * sizeof(double));
            size_t *sb = malloc((size_t)(P + 1) * sizeof(size_t)); ns_merge2_t *mg = malloc((size_t)P * sizeof(ns_merge2_t));
            if (strays && sscr && sb && mg) {
                size_t w = 0; for (int t = 0; t < P; t++) { memcpy(strays + w, dst + ca[t].hi - ca[t].nstray, ca[t].nstray * sizeof(double)); w += ca[t].nstray; }
                /* The strays are themselves near-sorted (they keep their original order), and the sequential
                   dispatcher on them took the run merge or momentum insertion: 90-280 ms when they were 20% of
                   n (measured). Large stray sets go back through the parallel engine, which declines this route
                   one level down (ns_depth) and partitions them; small ones take the dispatcher. */
                if (S >= 2) {
                    if (S >= NS_STRAYS_PARALLEL && P >= 2) { ns_depth++; logos_sort_f64(strays, S, P); ns_depth--; }
                    else { ctx_t sc; if (ctx_init(&sc, S, 0)) { double *r = sort_fn(strays, S, sscr, &sc); if (r != strays) memcpy(strays, r, S * sizeof(double)); ctx_free(&sc); }
                           else sample_sort(strays, S); }
                }
                sb[0] = 0; for (int b = 1; b < P; b++) sb[b] = lower_bound_d(strays, 0, S, thr[b]); sb[P] = S;
                TRACE("ns: strays sorted and cut");
                /* 3. one two-way merge per band, straight into a[] */
                size_t off = 0;
                for (int t = 0; t < P; t++) { mg[t].run = dst + ca[t].lo; mg[t].nrun = ca[t].nin; mg[t].strays = strays; mg[t].s0 = sb[t]; mg[t].s1 = sb[t + 1]; mg[t].out = a + off; mg[t].tid = t; off += ca[t].nin + (sb[t + 1] - sb[t]); }
                if (off == n) {
                    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, ns_merge2, &mg[t]);
                    ns_merge2(&mg[0]);
                    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
                    sorted = 1;
                    TRACE("ns: band merges done");
                }
            }
            free(strays); free(sscr); free(sb); free(mg);
        }
    }
#endif
    for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
    free(ctxs); free(ca); free(th); free(samp); free(thr);
    return sorted == 1;
}

#ifndef LOGOS_NO_RELOCATE
/* ---- top-level disjoint-run relocation (J5, parallel) ------------------- */
/* Chunk-parallel run detection with two additions over detect_runs_limited:
 *  - a run is CLOSED at a jump larger than RELOC_GAP_FACTOR times its mean step
 *    so far. Two shuffled blocks that happen to meet in ascending order would
 *    otherwise fuse into one run whose value interval hides a hole, and a third
 *    block belonging in that hole would then read as an overlap;
 *  - an overlap gate on each thread's prefix at R = 2, 4, 8, ...: when most of
 *    what the thread has seen sits in multi-run chains the data is interleaved,
 *    not block-permuted, and every thread stops (random data stops after a few
 *    dozen elements, interleaved runs after ~1/8 of a chunk).
 * Runs are stitched across chunk boundaries afterwards. Descending runs are
 * reversed in place per chunk (a straddling one becomes two disjoint runs). */
#define RELOC_GAP_FACTOR 1024.0
#define RELOC_MIN_BODY   16
static inline int reloc_gap_split_f(const double *a, size_t start, size_t last, size_t next, double factor, size_t min_body) {
    size_t cnt = last - start + 1; if (cnt < min_body) return 0;
    return (a[next] - a[last]) * (double)cnt > factor * (a[last] - a[start]);
}
static inline int reloc_gap_split(const double *a, size_t start, size_t last, size_t next) {   /* would a[next] break the run [start..last]? */
    return reloc_gap_split_f(a, start, last, next, RELOC_GAP_FACTOR, RELOC_MIN_BODY);
}
/* Sampled pre-gate (sample size scales with the decision, not n): 1024 strided
 * elements. If they form few (<= 16) runs -- i.e. the array has few, very long
 * runs -- the per-thread prefix gate below cannot judge (each chunk lies inside
 * one run) and the full probe would cost a read pass to learn what the sample
 * already shows: whether those long runs are interleaved (decline) or disjoint
 * blocks (go). Many sample runs: undecidable here, run the probe. Sample-level
 * gap splitting uses a small factor because a sample step is coarse; a false
 * split only yields two disjoint pieces and never a false overlap. */
static int reloc_sample_gate(const double *a, size_t n) {
    enum { S = 1024, MAXR = 16 };
    if (n < 4 * S) return 1;
    double s[S]; size_t step = n / S; for (size_t i = 0; i < S; i++) s[i] = a[i * step];
    size_t runs[2 * (MAXR + 1)], R = 0, i = 0;
    while (i < S) {
        size_t j = i + 1;
        if (j < S) {
            if (s[j] >= s[i]) { while (j < S && s[j] >= s[j - 1] && !reloc_gap_split_f(s, i, j - 1, j, 4.0, 4)) j++; }
            else { while (j < S && s[j] < s[j - 1] && !(j - i >= 4 && (s[j - 1] - s[j]) * (double)(j - i) > 4.0 * (s[i] - s[j - 1]))) j++;
                   for (size_t l = i, r = j - 1; l < r; l++, r--) { double t = s[l]; s[l] = s[r]; s[r] = t; } }
        }
        if (R == MAXR) return 1;
        runs[2 * R] = i; runs[2 * R + 1] = j - 1; R++; i = j;
    }
    if (R < 2) return 1;
    rdesc_t *d; size_t *ch, nc, merged;
    if (!runs_plan(s, runs, R, &d, &ch, &nc, &merged)) return 1;
    free(d); free(ch);
    TRACE("reloc: sample gate R=%zu merged=%zu/%d", R, merged, S);
    return merged <= S / 2;
}
typedef struct { double *a; size_t lo, hi, cap, R; size_t *runs; atomic_int *abort; int tid; } rdet_arg_t;
static void *reloc_detect_worker_body(void *p);
static void *reloc_detect_worker(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = reloc_detect_worker_body(p); PROF(prof_busy_since(((rdet_arg_t *)p)->tid, LOGOS_PH_RELOC_PROBE, s0);) return r; }
static void *reloc_detect_worker_body(void *p) {
    rdet_arg_t *w = p; double *a = w->a; size_t n = w->hi, i = w->lo, next_check = 2;
    while (i < n) {
        size_t j = i + 1;
        if (j < n) {
            if (a[j] >= a[i]) { while (j < n && a[j] >= a[j - 1] && !reloc_gap_split(a, i, j - 1, j)) j++; }
            else { while (j < n && a[j] < a[j - 1] && !(j - i >= RELOC_MIN_BODY && (a[j - 1] - a[j]) * (double)(j - i) > RELOC_GAP_FACTOR * (a[i] - a[j - 1]))) j++;
                   for (size_t l = i, r = j - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }
        }
        w->runs[2 * w->R] = i; w->runs[2 * w->R + 1] = j - 1; w->R++;
        size_t seen = j - w->lo;
        if (w->R >= w->cap || (w->R >= 256 && seen < w->R * MIN_AVG_RUN_FOR_MERGE) || atomic_load(w->abort)) { atomic_store(w->abort, 1); return NULL; }
        if (w->R == next_check) {
            rdesc_t *d; size_t *ch, nc, merged;
            if (runs_plan(a, w->runs, w->R, &d, &ch, &nc, &merged)) {
                TRACE("gate [%zu..%zu) R=%zu seen=%zu chains=%zu merged=%zu", w->lo, w->hi, w->R, seen, nc, merged);
                free(d); free(ch);
                if (merged > seen / 2) { atomic_store(w->abort, 1); return NULL; }
            }
            next_check <<= 1;
        }
        i = j;
    }
    return NULL;
}
/* Flat [start,end,...] list of the whole array's runs (malloc'd), or NULL when a
 * thread's gate said "not block-permuted". */
static size_t *detect_runs_parallel(double *a, size_t n, int P, size_t *n_runs) {
    size_t run_cap = n / MIN_AVG_RUN_FOR_MERGE; if (run_cap < MAX_RUNS_FOR_MERGE) run_cap = MAX_RUNS_FOR_MERGE;
    if (n < MIN_PARALLEL) P = 1;
    size_t chunk = (n + (size_t)P - 1) / (size_t)P, cap = run_cap / (size_t)P + 2;
    rdet_arg_t *w = calloc((size_t)P, sizeof *w); pthread_t *th = malloc((size_t)P * sizeof *th); atomic_int abort_flag; atomic_init(&abort_flag, 0);
    *n_runs = 0; if (!w || !th) { free(w); free(th); return NULL; }
    int ok = 1;
    for (int t = 0; t < P; t++) { w[t].a = a; PROF(w[t].tid = t;) w[t].lo = (size_t)t * chunk; w[t].hi = w[t].lo + chunk > n ? n : w[t].lo + chunk; w[t].cap = cap; w[t].abort = &abort_flag;
                                  w[t].runs = malloc(2 * cap * sizeof(size_t)); if (!w[t].runs) ok = 0; }
    if (ok) {
        /* chunk 0 first, on the caller's thread: interleaved data fails its gate within a few runs,
           so the common decline never pays for spawning P-1 threads (milliseconds on a loaded box) */
        reloc_detect_worker(&w[0]);
        if (!atomic_load(&abort_flag)) {
            for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, reloc_detect_worker, &w[t]);
            for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        }
    }
    size_t *runs = NULL, R = 0;
    if (ok && !atomic_load(&abort_flag)) {
        size_t total = 0; for (int t = 0; t < P; t++) total += w[t].R;
        runs = malloc(2 * (total + 1) * sizeof(size_t));
        if (runs) for (int t = 0; t < P; t++) for (size_t k = 0; k < w[t].R; k++) {
            size_t s = w[t].runs[2 * k], e = w[t].runs[2 * k + 1];
            if (R > 0 && runs[2 * R - 1] + 1 == s && a[s] >= a[s - 1] && !reloc_gap_split(a, runs[2 * R - 2], s - 1, s)) runs[2 * R - 1] = e;   /* stitch across the chunk boundary */
            else { runs[2 * R] = s; runs[2 * R + 1] = e; R++; }
        }
    }
    for (int t = 0; t < P; t++) free(w[t].runs);
    free(w); free(th); *n_runs = R; return runs;
}

typedef struct { double *a, *dst; size_t n, R, nc; int P, t; const rdesc_t *d; const size_t *off, *ch, *rl; atomic_size_t *next; barrier_t *bar; } reloc_arg_t;
static void *reloc_worker(void *p) {
    reloc_arg_t *w = p; size_t n = w->n; int P = w->P;
    size_t lo = (size_t)w->t * n / P, hi = (size_t)(w->t + 1) * n / P;
    PROF(prof_stamp_t s0 = prof_stamp();)
    /* phase 1: fill dst[lo,hi) from the pieces of runs that land there (balanced, sequential streams) */
    { size_t q = 0; while (q < w->R && w->off[q] + w->d[q].len <= lo) q++;
      for (; q < w->R && w->off[q] < hi; q++) {
          size_t ds = w->off[q] > lo ? w->off[q] : lo, de = w->off[q] + w->d[q].len < hi ? w->off[q] + w->d[q].len : hi;
          memcpy(w->dst + ds, w->a + w->d[q].start + (ds - w->off[q]), (de - ds) * sizeof(double)); } }
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s0); double t1 = prof_now();)
    if (P > 1) barrier_wait(w->bar);
    PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t1); prof_stamp_t s2 = prof_stamp();)
    /* phase 2: merge inside multi-run chains; chains are disjoint dst ranges and a[] is free scratch now */
    size_t k;
    while ((k = atomic_fetch_add(w->next, 1)) < w->nc) {
        size_t k0 = w->ch[k], k1 = w->ch[k + 1];
        if (k1 - k0 > 1) merge_all_runs(w->dst, w->a, w->rl + 2 * k0, k1 - k0);
    }
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s2); double t3 = prof_now();)
    if (P > 1) barrier_wait(w->bar);
    PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t3); prof_stamp_t s4 = prof_stamp();)
    /* phase 3: copy back */
    memcpy(w->a + lo, w->dst + lo, (hi - lo) * sizeof(double));
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s4);)
    return NULL;
}

/* Returns 1 with a[] sorted, or 0 (a[] still a permutation of the input; the
 * caller continues on the normal path). dst: n-element scratch. */
static int relocate_try_f64(double *a, size_t n, int P, double *dst) {
    if (!reloc_sample_gate(a, n)) { TRACE("reloc: sample gate declined"); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
    size_t R = 0; size_t *runs = detect_runs_parallel(a, n, P, &R);
    TRACE("reloc: detect_runs_parallel -> %s, R=%zu", runs ? "runs" : "bail", R);
    if (!runs) { PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
    if (R < 2) { free(runs); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
    rdesc_t *d; size_t *ch, nc, merged;
    int ok = runs_plan(a, runs, R, &d, &ch, &nc, &merged); free(runs);
    PROF(prof_phase(LOGOS_PH_RELOC_PROBE);)
    if (!ok) return 0;
    TRACE("reloc: plan chains=%zu merged=%zu (gate n/2=%zu)", nc, merged, n / 2);
    if (merged > n / 2) { free(d); free(ch); return 0; }
    size_t *off = malloc(R * sizeof(size_t)), *rl = malloc(2 * R * sizeof(size_t));
    if (!off || !rl) { free(d); free(ch); free(off); free(rl); return 0; }
    for (size_t q = 0, o = 0; q < R; q++) { off[q] = o; rl[2 * q] = o; rl[2 * q + 1] = o + d[q].len - 1; o += d[q].len; }
    /* Relocation's parallel form has a serial middle (plan) and a spawn; below RELOC_MIN_PER_THREAD elements
       per thread it lost to the sequential form (block_perm 316k: 1.4 vs 1.0 ms with 3 threads; 1M: 2.5 vs 2.7
       with 10, scale-full). Use at most one thread per that many elements. A/B: make smallbench. */
    if (n < MIN_PARALLEL) P = 1;
    { size_t pm = n / RELOC_MIN_PER_THREAD; if (pm < 1) pm = 1; if ((size_t)P > pm) P = (int)pm; }
    barrier_t bar; atomic_size_t next; atomic_init(&next, 0);
    reloc_arg_t *wa = malloc((size_t)P * sizeof *wa); pthread_t *th = malloc((size_t)P * sizeof *th);
    if (!wa || !th) { free(wa); free(th); free(off); free(rl); free(d); free(ch); return 0; }
    if (P > 1) barrier_init(&bar, P);
    for (int t = 0; t < P; t++) { reloc_arg_t w = { a, dst, n, R, nc, P, t, d, off, ch, rl, &next, &bar }; wa[t] = w; }
    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, reloc_worker, &wa[t]);   /* same assumption as psort_f64 */
    reloc_worker(&wa[0]);
    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
    TRACE("reloc: copy+merge+copyback done (P=%d)", P);
    if (P > 1) barrier_destroy(&bar);
    free(wa); free(th); free(off); free(rl); free(d); free(ch);
    return 1;
}
#endif


/* ---- sample once, decide once (default since 2026-09-11; -DLOGOS_NO_SODO restores the per-gate samples; A/B: make sodobench) ----
 * The fixed-order dispatcher draws five samples (2048 for the low-cardinality gate, 2048 for the
 * moderate-cardinality gate, 40 strided pairs for the near-sorted gate, four windows for its pre-gate,
 * 5376 for the splitters and the KS certificate) and each gate is a threshold on one statistic. Here one
 * sorted sample of nbk*OVERSAMPLE carries every statistic: distinct count, singletons and doubletons
 * (Chao1 estimate of the TRUE number of distinct values, which is what the "abandoned attempt" defects
 * lacked: a sample of 60 distinct values with many singletons means thousands), NaN, the KS distance,
 * and an inversion rate over Fibonacci-lag pairs (sample indices i and i+144 sit 0.37% of n apart in
 * position, a strided pair like the 40-point probe's but with ~5000 of them). The routes then run in the
 * same cheapest-first order, because a cost model built from the measured per-route costs reproduces
 * that order (counting 0.8, lattice 0.9, near-sorted 1.3, relocation 1.1, partition 2.3-2.7 ns/elem at
 * 14T), but every gate is answered from the one sample, the counting routes are attempted only when the
 * estimated true cardinality fits, and the partition reuses the sample and its certificate. */
#if !defined(LOGOS_NO_SODO) && !defined(LOGOS_SODO)
#define LOGOS_SODO 1
#endif
#ifdef LOGOS_SODO
typedef struct { size_t m, mfull, distinct, f1, f2; double chao1, inv_rate, ks; int has_nan; double *sorted; } sodo_t;
/* stage 1: the first SODO_GATE_M points of the Weyl sequence (a prefix of a low-discrepancy sequence is
   itself equidistributed) answer every gate; the cheapest routes never pay for the partition's sample */
#define SODO_GATE_M 2048
static int sodo_sample(const double *a, size_t n, size_t mfull, sodo_t *S) {
    memset(S, 0, sizeof *S); if (mfull > n) mfull = n; size_t m = mfull < SODO_GATE_M ? mfull : SODO_GATE_M; S->m = m; S->mfull = mfull;
    double *raw = malloc(m * sizeof(double)); S->sorted = malloc(mfull * sizeof(double)); if (!raw || !S->sorted) { free(raw); free(S->sorted); S->sorted = NULL; return 0; }
    sample_read(a, n, 0, m, mfull, raw, &S->has_nan);
    /* inversion rate over Fibonacci-lag pairs, wrapped pairs skipped */
    { const size_t L = 144; size_t inv = 0, comps = 0;
      for (size_t i = 0; i + L < m; i++) { size_t p0 = SAMPLE_POS(i, mfull, n), p1 = SAMPLE_POS(i + L, mfull, n); if (p1 <= p0) continue; comps++; if (raw[i] > raw[i + L]) inv++; }
      S->inv_rate = comps ? (double)inv / (double)comps : 1.0; }
    memcpy(S->sorted, raw, m * sizeof(double)); free(raw);
    if (sample_sort(S->sorted, m)) S->has_nan = 1;
    double *s = S->sorted; size_t d = 0, f1 = 0, f2 = 0;
    for (size_t i = 0; i < m;) { size_t j = i + 1; while (j < m && s[j] == s[i]) j++; d++; if (j - i == 1) f1++; else if (j - i == 2) f2++; i = j; }
    S->distinct = d; S->f1 = f1; S->f2 = f2; S->chao1 = (double)d + (double)f1 * (double)(f1 > 0 ? f1 - 1 : 0) / (2.0 * (double)(f2 + 1));
    S->ks = 1.0;
    return 1;
}
/* stage 2 (partition only): read the remaining points, sort them, merge with stage 1; the KS distance comes with them */
static int sodo_extend(const double *a, size_t n, sodo_t *S) {
    size_t m = S->m, mfull = S->mfull; if (mfull <= m) return 1;
    size_t k = mfull - m; double *ext = malloc(k * sizeof(double)); if (!ext) return 0;
    int nan2 = 0; sample_read(a, n, m, mfull, mfull, ext, &nan2); if (nan2) S->has_nan = 1;
    if (sample_sort(ext, k)) S->has_nan = 1;
    double *s = S->sorted; size_t i = m, j = k, w = mfull;              /* merge from the back, in place */
    while (j > 0) { if (i > 0 && s[i - 1] > ext[j - 1]) s[--w] = s[--i]; else s[--w] = ext[--j]; }
    free(ext); S->m = mfull;
    if (mfull >= 256 && s[mfull - 1] > s[0]) {
        double D = 0, inv = 1.0 / (s[mfull - 1] - s[0]);
        for (size_t q = 0; q < mfull; q++) { double F = (s[q] - s[0]) * inv, e1 = F - (double)q / (double)mfull, e2 = (double)(q + 1) / (double)mfull - F; if (e1 < 0) e1 = -e1; if (e2 < 0) e2 = -e2; if (e1 > D) D = e1; if (e2 > D) D = e2; }
        S->ks = D;
    }
    return 1;
}
#endif
/* ===================== dense integers: sort by presence, not by comparing =====================
 * A dense permutation of consecutive integers is the cheapest thing a sort can be handed and this
 * engine was among the worst at it: the overnight chaos map measured a Gray-code order and a
 * hash-iteration order 9-13x off a sort that knew the structure, at ONE thread, because they reach
 * the quantized radix and pay four passes over 64 MB.
 *
 * If the values are integers with no repeats spanning not much more than n, the sorted output carries
 * no information beyond WHICH integers are present. So: one bit per possible value, one pass to set
 * the bits, one pass to walk them in order. At 8M elements the bitmap is 1 MB, which is L2-resident,
 * so the middle pass is a cached bit-set per element and the cost collapses to reading the input once
 * and writing the output once.
 *
 * The gate is free: the dispatcher's sample already knows whether the values are integral, how many
 * are distinct, and roughly how wide they are. But a sample bounds the bulk and never the tail (this
 * project's most expensive recurring lesson), so the true extremes are established by a real pass
 * before anything is allocated, and that pass doubles as the integrality check.
 *
 * Declines, all cheap and all leaving a[] untouched: a non-integral value, a span wider than
 * DENSE_SPAN_MULT * n, or ANY repeat. Repeats are refused rather than handled because a value that
 * repeats is by definition cardinality-limited, and the counting routes above already own that case
 * and do it better. -DLOGOS_NO_DENSEINT removes the route; LOGOS_NO_DENSE=1 disables it at runtime. */
#ifndef LOGOS_NO_DENSEINT
#ifndef DENSE_SPAN_MULT
#define DENSE_SPAN_MULT 4            /* a span of 4n is a 4n-bit map: 4 MB at 8M, still cache-friendly */
#endif
#ifndef DENSE_MIN_N
#define DENSE_MIN_N 65536
#endif
#define DENSE_INT_MAX 4503599627370496.0   /* 2^52: above this a double cannot be an exact odd integer */

/* Does the sample say this is worth a pass? Every sampled value integral, essentially no repeats, and
 * a sampled span within the budget. All three are already computed or one subtraction away. */
static int dense_gate(const double *samp, size_t m, size_t n) {
    if (m < 256) return 0;
    size_t rep_ = 0;
    for (size_t i = 0; i < m; i++) {
        double v = samp[i];
        if (!(v >= -DENSE_INT_MAX && v <= DENSE_INT_MAX)) return 0;
        if (v != (double)(int64_t)v) return 0;                       /* not an integer: not this route */
        if (i && v == samp[i - 1]) rep_++;
    }
    if (rep_ > m / 64) return 0;                                     /* repeats: the counting routes own that */
    /* The sample's span UNDERSTATES the true one, so this only rejects the hopeless cases and pass 1
     * makes the real decision. An earlier version scaled this bound with m*m, which quietly turned the
     * route off at one thread (where the sample is small) -- the one place it was meant to fire. */
    double span = samp[m - 1] - samp[0] + 1.0;
    return span > 0 && span <= (double)DENSE_SPAN_MULT * (double)n;
}

static int dense_int_try_f64(double *a, size_t n, const double *samp, size_t m) {
    if (n < DENSE_MIN_N || getenv("LOGOS_NO_DENSE")) return 0;
    if (!samp || !dense_gate(samp, m, n)) return 0;

    /* ONE pass, not two. The first version computed the true extremes in a pass of its own and then
     * set bits in a second, which made a DECLINE cost two passes: measured 0.60 on dense integers
     * containing a single repeat, i.e. the route made that shape 1.7x slower before handing it back.
     *
     * Instead the window comes from the sample with a margin of n on each side, and the single pass
     * aborts on anything outside it. That is the same bargain the lattice route makes, and the same
     * lesson behind it: a sample bounds the bulk and never the tail, so budget for the tail rather
     * than trust the sample. A value outside the window costs one partial pass, exactly like a repeat.
     * The emit does not need the true minimum either, since it walks the map in order and skips the
     * absent slots, so nothing was lost by dropping the extremes pass. */
    int64_t base = (int64_t)samp[0] - (int64_t)n;
    double topd = samp[m - 1] + (double)n;
    if (!(topd < DENSE_INT_MAX)) return 0;
    uint64_t span = (uint64_t)((int64_t)topd - base) + 1;
    if (span > (uint64_t)(DENSE_SPAN_MULT + 2) * (uint64_t)n) return 0;

    size_t words = (size_t)((span + 63) / 64);
    uint64_t *bits = calloc(words, sizeof(uint64_t));
    if (!bits) return 0;

    /* Presence. Aborts on a non-integer, a value outside the window, or ANY repeat: a value that
     * repeats is cardinality-limited, and the counting routes above own that case and do it better.
     * a[] is untouched until the emit, so every abort leaves the array exactly as it was found. */
    for (size_t i = 0; i < n; i++) {
        double v = a[i];
        int64_t k = (int64_t)v;
        if ((double)k != v || (k == 0 && signbit(v))) { free(bits); return 0; }   /* a -0.0 would come back as +0.0 */
        uint64_t d = (uint64_t)(k - base);
        if (k < base || d >= span) { free(bits); return 0; }
        uint64_t w = d >> 6, b = (uint64_t)1 << (d & 63);
        if (bits[w] & b) { free(bits); return 0; }
        bits[w] |= b;
    }

    /* Walk the map in order. A whole empty word is one test, so the margin costs almost nothing. */
    size_t out = 0;
    for (size_t w = 0; w < words; w++) {
        uint64_t x = bits[w];
        while (x) {
            int t = __builtin_ctzll(x);
            a[out++] = (double)(base + (int64_t)(w * 64 + (size_t)t));
            x &= x - 1;
        }
    }
    free(bits);
    TRACE("dense-int: %zu values, window %llu, emitted %zu", n, (unsigned long long)span, out);
    return out == n;
}
#endif

/* ===================== sorted prefix + unsorted tail =====================
 * Append-mostly data: a sorted log, a sorted table, an index that has had a batch of rows added at the
 * end. The overnight chaos map measured this shape 9.4x off a sort told the structure at ONE thread and
 * 1.8x at fourteen, and the (index, value) quadtree probe named it correctly on a held-out zoo. This is the
 * route both pointed at.
 *
 * The gate costs nothing. The prescan in psort_f64 already walks an ascending prefix to its first descent,
 * so when it stops at d the array is known ascending on [0, d). If that prefix is at least half the array,
 * sort only the tail [d, n) and merge the two sorted pieces from the back: scratch the size of the tail and
 * one pass. On random data the prescan stops within a couple of elements, so d is tiny and the route is
 * never considered; on near-sorted data with scattered swaps the first descent comes early for the same
 * reason. Notice that the route does not need the quadtree probe at all: the probe pointed at the
 * structure, but the prescan's own stopping point is a cheaper and EXACT gate for it.
 *
 * NaN is refused because the prescan's comparisons with NaN are false, so a NaN can hide inside a prefix
 * that only LOOKS ascending, and the merge needs a total order. The scan vectorises and runs only on arrays
 * that already passed the gate.
 *
 * The merge is exact and keeps every tie. Profiled under LOGOS_ROUTE_NEARSORTED, the closest existing route
 * tag. -DLOGOS_NO_TAILROUTE removes it; LOGOS_NO_TAIL=1 disables it at runtime. */
#ifndef LOGOS_NO_TAILROUTE
#ifndef TAIL_MIN_N
#define TAIL_MIN_N 4096
#endif
static void psort_f64(double *a, size_t n, int P, size_t *vstart, int *vnbk, double *vmin, double *vmax);
/* The parallel form. Every thread scans its chunk for NaN and copies it to scratch, so a[] is untouched until
 * the route has committed. The tail is sorted inside scratch by the full dispatcher. Then the prefix is cut at
 * P-1 evenly spaced positions, each cut's value is binary-searched in the sorted tail, and every thread merges
 * its piece of prefix with its piece of tail straight into a[]: the pieces tile the output and read only
 * scratch, so no thread writes where another reads and nothing is copied back. Ties keep prefix before tail. */
typedef struct { const double *a; double *buf; size_t lo, hi; int bad; } tail_scan_t;
static void *tail_scan(void *p) {
    tail_scan_t *w = p; int bad = 0;
    for (size_t i = w->lo; i < w->hi; i++) { double x = w->a[i]; bad |= (x != x); w->buf[i] = x; }
    w->bad = bad;
    return NULL;
}
typedef struct { const double *pre, *tail; size_t i0, i1, j0, j1; double *out; } tail_seg_t;
static void *tail_merge_seg(void *p) {
    tail_seg_t *w = p; size_t i = w->i0, j = w->j0; double *o = w->out;
    while (i < w->i1 && j < w->j1) { double x = w->pre[i], y = w->tail[j]; int tk = y < x; *o++ = tk ? y : x; j += (size_t)tk; i += (size_t)!tk; }
    if (i < w->i1) { memcpy(o, w->pre + i, (w->i1 - i) * sizeof(double)); o += w->i1 - i; }
    if (j < w->j1) memcpy(o, w->tail + j, (w->j1 - j) * sizeof(double));
    return NULL;
}
static inline size_t tail_lb(const double *v, size_t len, double key) {
    size_t lo = 0, hi = len;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (v[mid] < key) lo = mid + 1; else hi = mid; }
    return lo;
}
static int tail_merge_par_f64(double *a, size_t n, size_t d, int P) {
    size_t tl = n - d;
    void *pb = NULL; uint8_t *pbi = NULL; int pooled = pool_acquire(n * sizeof(double), 0, &pb, &pbi);
    double *buf = pooled ? (double *)pb : (double *)big_alloc(n * sizeof(double));
    tail_scan_t *sc = malloc((size_t)P * sizeof *sc); tail_seg_t *sg = malloc((size_t)P * sizeof *sg);
    pthread_t *th = malloc((size_t)P * sizeof *th);
    int ok = buf && sc && sg && th;
    if (ok) {
        for (int t = 0; t < P; t++) { sc[t].a = a; sc[t].buf = buf; sc[t].lo = n * (size_t)t / (size_t)P; sc[t].hi = n * (size_t)(t + 1) / (size_t)P; sc[t].bad = 0; }
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, tail_scan, &sc[t]);
        tail_scan(&sc[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
        for (int t = 0; t < P; t++) if (sc[t].bad) ok = 0;
    }
    if (ok) {
        psort_f64(buf + d, tl, P, NULL, NULL, NULL, NULL);        /* the tail may be anything: full dispatcher */
        const double *pre = buf, *tail = buf + d;
        size_t j0 = 0;
        for (int t = 0; t < P; t++) {
            size_t i0 = d * (size_t)t / (size_t)P, i1 = d * (size_t)(t + 1) / (size_t)P;
            size_t j1 = (t == P - 1) ? tl : tail_lb(tail, tl, pre[i1]);
            if (j1 < j0) j1 = j0;
            sg[t].pre = pre; sg[t].tail = tail; sg[t].i0 = i0; sg[t].i1 = i1; sg[t].j0 = j0; sg[t].j1 = j1; sg[t].out = a + i0 + j0;
            j0 = j1;
        }
        for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, tail_merge_seg, &sg[t]);
        tail_merge_seg(&sg[0]);
        for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
    }
    if (pooled) pool_release(); else free(buf);
    free(sc); free(sg); free(th);
    return ok;
}
static int tail_merge_try_f64(double *a, size_t n, size_t d, int P) {
    if (getenv("LOGOS_NO_TAIL")) return 0;
    if (P > 1 && n >= MIN_PARALLEL) return tail_merge_par_f64(a, n, d, P);
    int bad = 0;
    for (size_t i = 0; i < n; i++) bad |= (a[i] != a[i]);
    if (bad) return 0;
    size_t tl = n - d;
    double *buf = (double *)malloc(tl * sizeof(double));
    if (!buf) return 0;
    psort_f64(a + d, tl, P, NULL, NULL, NULL, NULL);            /* the tail may be anything: full dispatcher */
    memcpy(buf, a + d, tl * sizeof(double));
    /* Backward merge. w == i + j throughout, so a write at w-1 never lands on an unread prefix element. */
    size_t i = d, j = tl, w = n;
    while (j > 0) {
        if (i > 0 && a[i - 1] > buf[j - 1]) a[--w] = a[--i];
        else                                a[--w] = buf[--j];
    }
    free(buf);
    return 1;
}
#endif

/* ===================== interleaved ascending streams: a stable scatter is already a sort =====================
 * Sharded ID allocators, multi-tenant sequences under a tenant prefix, per-sensor keys merged into one table:
 * several streams, each ascending in arrival order, in value ranges separated by wide gulfs, interleaved element
 * by element. At ONE thread the chaos map measured this 5.9x off a sort told the structure (the engine sent it
 * to the AVX2 quicksort), and the (index, value) quadtree probe named it on a held-out zoo, reading nine
 * overlapping pieces against eight real streams.
 *
 * The observation: a STABLE scatter preserves arrival order inside each bucket. So if the buckets ARE the
 * streams, every bucket comes out already sorted and no comparison sort runs: two passes and a copy.
 *
 * The buckets come from the sample the dispatcher already drew. Its sorted values show the gulfs directly, as
 * gaps far wider than the median gap (the same test the per-mode splitter tables use), and each boundary goes
 * at the MIDPOINT of its gulf, which is where the per-mode certificate learned a cut has to go. Whether each
 * stream really ascends is then checked on a small sample taken in POSITION order, which the dispatcher's own
 * sample cannot answer because sorting it threw the positions away. Random data inside a mode descends about
 * half the time, so that check declines immediately.
 *
 * Correctness never rests on the gate. After the scatter every bucket is checked in one linear pass and any
 * bucket not in order goes to the ordinary leaf, so a stream that is only mostly ascending costs a little and a
 * wrong guess costs a partition, never a wrong answer. Sequential only: at fourteen threads the engine already
 * runs this shape at 0.76x of the ceiling. -DLOGOS_NO_STREAMS removes it; LOGOS_NO_STREAMS=1 disables it at
 * runtime. */
#ifndef LOGOS_NO_STREAMS
#ifndef STREAMS_MAX_MODES
#define STREAMS_MAX_MODES 64
#endif
#ifndef STREAMS_GAP
#define STREAMS_GAP 10.0
#endif
#ifndef STREAMS_MIN_N
#define STREAMS_MIN_N 65536
#endif
/* Mode boundaries from a sorted sample: a gulf is a gap wider than STREAMS_GAP times the median gap, cut at its
 * midpoint. Returns the number of modes, or 0 when there is nothing to use. */
static int streams_modes(const double *samp, size_t m, double *bnd) {
    if (!samp || m < 64) return 0;
    double *g = (double *)malloc((m - 1) * sizeof(double));
    if (!g) return 0;
    for (size_t i = 0; i + 1 < m; i++) g[i] = samp[i + 1] - samp[i];
    sample_sort(g, m - 1);                                   /* the engine's own sort: no libc qsort here */
    double med = g[(m - 1) / 2];
    free(g);
    if (!(med > 0)) return 0;                                /* heavy duplicates: the counting routes own that */
    double thr = STREAMS_GAP * med;
    int nb = 0;
    for (size_t i = 0; i + 1 < m; i++) {
        double gap = samp[i + 1] - samp[i];
        if (gap > thr) { if (nb + 1 >= STREAMS_MAX_MODES) return 0; bnd[nb++] = samp[i] + 0.5 * gap; }
    }
    return nb >= 1 ? nb + 1 : 0;
}
/* branch-free, and a loop the compiler vectorises: a compare per boundary, summed */
static inline int streams_mode(double x, const double *bnd, int nb) { int md = 0; for (int q = 0; q < nb; q++) md += (x >= bnd[q]); return md; }
#ifndef STREAMS_TABLE_BITS
#define STREAMS_TABLE_BITS 12
#endif
#ifndef STREAMS_TABLE_MIN_MODES
#define STREAMS_TABLE_MIN_MODES 12   /* below this many streams the compares are faster (make streamsbench, lookup) */
#endif
/* Cell of x in an equal-width grid over the sample's range, clamped at both ends (NaN goes to cell 0). The map is
 * monotone in x, which is what makes the table exact: if x's cell holds no boundary, then every boundary in a
 * lower cell is below x and every boundary in a higher cell is above it, so the cell's precomputed count of
 * boundaries below it IS the compare-sum's answer. A cell that holds a boundary falls back to the compares. */
static inline size_t streams_cell(double x, double lo, double inv) {
    double t = (x - lo) * inv;
    t = t > 0 ? t : 0;
    double top = (double)((1u << STREAMS_TABLE_BITS) - 1);
    t = t < top ? t : top;
    return (size_t)t;
}

static int streams_try_f64(double *a, size_t n, double *buf, ctx_t *c, const double *bnd, int nm, double lo, double hi) {
    if (nm < 2 || getenv("LOGOS_NO_STREAMS")) return 0;
    int nb = nm - 1;

    /* Gate: do the modes ascend in ARRIVAL order? A jittered, strictly increasing set of positions, so the probe
     * cannot alias with a periodic layout and needs no sort of its own. */
    enum { SM = 1024 };
    size_t stride = n / SM;
    double last[STREAMS_MAX_MODES]; for (int q = 0; q < nm; q++) last[q] = -INFINITY;
    size_t descents = 0;
    for (size_t i = 0; i < SM; i++) {
        uint64_t h = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL; h ^= h >> 29;
        size_t p = i * stride + (stride ? (size_t)(h % stride) : 0);
        if (p >= n) p = n - 1;
        double x = a[p];
        if (x != x) return 0;
        int md = streams_mode(x, bnd, nb);
        if (x < last[md]) descents++;
        last[md] = x;
    }
    if (descents > SM / 50) return 0;                        /* more than 2% out of order: not ascending streams */

    /* Pass 1: mode per element, cached, and counted. a[] is not touched until the final copy. */
    uint8_t *mc = (uint8_t *)malloc(n);
    if (!mc) return 0;
    size_t cnt[STREAMS_MAX_MODES + 1]; memset(cnt, 0, sizeof cnt);
    int bad = 0;
    /* The mode lookup: a table of 2^STREAMS_TABLE_BITS cells over the sample's range, one load per element, where the
     * compares cost one per boundary (3.0x at 4 streams fell to 1.2x at 60). LOGOS_STREAMS_CMP=1 uses the compares. */
    enum { ST = 1u << STREAMS_TABLE_BITS };
    uint8_t tab[ST]; double span = hi - lo, inv = 0; size_t amb = 0;
    /* Measured in the normal build: the compares win at 4 streams (0.80x) and on 8 perturbed streams (0.66x), the table
     * wins at 16 (1.23x) and 60 (2.72x). So the table runs from STREAMS_TABLE_MIN_MODES streams up. LOGOS_STREAMS_TABLE=1
     * forces it at any count and LOGOS_STREAMS_CMP=1 forbids it. A -DLOGOS_TRACE build showed the OPPOSITE at 8 streams
     * (the table faster), because its fprintf calls change this function's codegen: never time a lookup in a trace build. */
    int use_tab = !getenv("LOGOS_STREAMS_CMP") && (nm >= STREAMS_TABLE_MIN_MODES || getenv("LOGOS_STREAMS_TABLE"))
                  && span > 0 && span < INFINITY;
    for (int q = 0; q < nb && use_tab; q++) if (bnd[q] != bnd[q]) use_tab = 0;
    if (use_tab) { inv = (double)ST / span; if (!(inv < INFINITY)) use_tab = 0; }
    if (use_tab) {
        uint16_t nbc[ST]; memset(nbc, 0, sizeof nbc);
        for (int q = 0; q < nb; q++) nbc[streams_cell(bnd[q], lo, inv)]++;
        unsigned below = 0;
        for (size_t k = 0; k < ST; k++) { tab[k] = nbc[k] ? 255 : (uint8_t)below; below += nbc[k]; }
        for (size_t i = 0; i < n; i++) {
            double x = a[i]; bad |= (x != x);
            int md = tab[streams_cell(x, lo, inv)];
            if (md == 255) { md = streams_mode(x, bnd, nb); amb++; }
            mc[i] = (uint8_t)md; cnt[md]++;
        }
    } else
    for (size_t i = 0; i < n; i++) { double x = a[i]; bad |= (x != x); int md = streams_mode(x, bnd, nb); mc[i] = (uint8_t)md; cnt[md]++; }
    if (bad) { free(mc); return 0; }
    TRACE("streams: lookup pass done (table %d)", use_tab);
    size_t off[STREAMS_MAX_MODES + 1], cur[STREAMS_MAX_MODES];
    off[0] = 0;
    for (int q = 0; q < nm; q++) { off[q + 1] = off[q] + cnt[q]; cur[q] = off[q]; }

    /* Pass 2: the stable scatter. Inside each bucket, elements keep the order they arrived in. */
    for (size_t i = 0; i < n; i++) buf[cur[mc[i]]++] = a[i];
    free(mc);
    TRACE("streams: scatter done");

    /* Each bucket is checked, and sorted only if it is not already in order. */
    int dirty = 0;
    for (int q = 0; q < nm; q++) {
        size_t s0 = off[q], len = off[q + 1] - s0, j;
        for (j = 1; j < len; j++) if (buf[s0 + j] < buf[s0 + j - 1]) break;
        if (j < len) {
            dirty++;
            double *r = leaf_f64(buf + s0, len, a + s0, c);  /* a[] is scratch until the final copy */
            if (r != buf + s0) memcpy(buf + s0, r, len * sizeof(double));
        }
    }
    TRACE("streams: buckets checked");
    memcpy(a, buf, n * sizeof(double));
    TRACE("streams: %d modes, %zu of %d sampled descents, %d buckets needed a sort, table %d, %zu ambiguous lookups", nm, descents, (int)SM, dirty, use_tab, amb);
    return 1;
}
#endif

/* vstart/vnbk non-NULL: partition only, leaving the buckets unsorted, and report their boundaries.
 * *vnbk is set to 0 whenever the array came out FULLY sorted instead -- every other route in this
 * dispatcher produces sorted output, so 0 means "there is nothing left to do". */
static void psort_f64(double *a, size_t n, int P, size_t *vstart, int *vnbk, double *vmin, double *vmax) {
    if (vnbk) *vnbk = 0;
    if (P < 1) P = 1;
    if (n < 2) return;
    if ((size_t)P > n) P = (int)n;
    P = effective_threads(n, P);
    TRACE_START(); TRACE("psort_f64 n=%zu P=%d", n, P);
    PROF(g_prof.n = n; g_prof.nthreads = P; g_prof.route = LOGOS_ROUTE_NONE; double t_start = prof_now(); g_prof_tp = t_start;)

    /* whole-array early exits (the JS prescan's O(n) wins; short-circuits fast on random data) */
    { int asc = 1, desc = 1; size_t first_desc = n;
      for (size_t i = 1; i < n && (asc || desc); i++) { if (asc && a[i] < a[i - 1]) { asc = 0; first_desc = i; } if (a[i] > a[i - 1]) desc = 0; }
      if (asc) { PROF(prof_phase(LOGOS_PH_PRESCAN); prof_finish(LOGOS_ROUTE_SORTED, t_start);) return; }
      if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } PROF(prof_phase(LOGOS_PH_PRESCAN); prof_finish(LOGOS_ROUTE_SORTED, t_start);) return; }
      (void)first_desc;
#ifndef LOGOS_NO_TAILROUTE
      /* A sorted prefix with an unsorted tail. The loop above just walked the prefix to its first descent, so
       * the prefix length costs nothing: sort only the tail and merge it in. See tail_merge_try_f64. */
      /* The sequential form, offered to parallel sorts, LOST at 14 threads: 0.81x on a 95% prefix and 0.34x on a
       * 50% prefix (make prefixbench), because its NaN scan and backward merge are single-threaded and the
       * parallel near-sorted route it displaced was already fast. Parallel sorts now get the parallel form
       * (tail_merge_par_f64). It was first gated to tails of at most n/8, on a prediction that a large tail loses
       * (0.5-0.7x at 50%); measured with the gate lifted it did not: 1.16x at an 80% prefix, 1.03x at 50%. So
       * there is no tail gate. LOGOS_TAIL_SEQ=1 restores sequential-only. */
      if (first_desc >= n / 2 && n >= TAIL_MIN_N
          && ((P == 1 || n < MIN_PARALLEL) || !getenv("LOGOS_TAIL_SEQ"))
          && tail_merge_try_f64(a, n, first_desc, P)) {
          TRACE("tail: sorted prefix %zu, tail %zu sorted and merged", first_desc, n - first_desc);
          PROF(prof_phase(LOGOS_PH_NEARSORTED); prof_finish(LOGOS_ROUTE_NEARSORTED, t_start);)
          return;
      }
#endif
    }
    PROF(prof_phase(LOGOS_PH_PRESCAN);)
    /* few distinct values: map+emit counting sort (parallel when P>1) instead of
       a partition that cannot split equal keys, or a span-sized count array */
    TRACE("prescan done");
#ifdef LOGOS_SODO
    sodo_t SO; int sodo_ok = 0;
    { int nbk0 = P * BUCKETS_PER_THREAD; if (nbk0 > MAX_BUCKETS) nbk0 = P * (MAX_BUCKETS / P > 0 ? MAX_BUCKETS / P : 1);
      if (n >= 4096) sodo_ok = sodo_sample(a, n, (size_t)nbk0 * OVERSAMPLE, &SO); }
    if (sodo_ok) TRACE("sodo: m=%zu distinct=%zu chao1=%.0f inv=%.3f ks=%.4f nan=%d", SO.m, SO.distinct, SO.chao1, SO.inv_rate, SO.ks, SO.has_nan);
    PROF(prof_phase(LOGOS_PH_SAMPLE);)
#ifndef LOGOS_NO_DENSEINT
    /* dense consecutive integers with no repeats: presence bits, no comparisons at all */
    /* Sequential sorts only. The route is three single-threaded passes; measured at 14 threads it
     * took 71 ms against the parallel partition's 27, and a DECLINE there still costs two wasted
     * passes (0.32 on a shape with one repeat). A sequential algorithm must not be offered to a
     * parallel sort just because its gate happens to pass. */
    if ((P == 1 || n < MIN_PARALLEL) && sodo_ok && !SO.has_nan && dense_int_try_f64(a, n, SO.sorted, SO.m)) {
        free(SO.sorted);
        PROF(prof_phase(LOGOS_PH_LOWCARD); prof_finish(LOGOS_ROUTE_LOWCARD, t_start);)
        return;
    }
#endif
    PROF(g_prof_phase = LOGOS_PH_LOWCARD;)
#if defined(LOGOS_LATTICE_FIRST) && !defined(LOGOS_NO_LATTICE)
    /* Low-cardinality data is usually a lattice, and the lattice's direct index is the cheapest lookup the
     * engine has (0.8 ns/element against 2.0 for the scan at 5 distinct); the shipped order reaches it only
     * after the scan declines. Variant B: make cardbench. */
    if (sodo_ok && !SO.has_nan && SO.distinct <= LOWCARD_SAMPLE_MAX && lattice_try_f64(a, n, count_threads(n, P), SO.sorted, SO.m)) {
        free(SO.sorted); PROF(prof_phase(LOGOS_PH_LOWCARD); g_prof.route = LOGOS_ROUTE_LATTICE; prof_finish(LOGOS_ROUTE_LATTICE, t_start);) return;
    }
#endif
    if (sodo_ok && !SO.has_nan && SO.distinct <= LOWCARD_MAX_SCAN && SO.chao1 <= LOWCARD_CAP / 2 && lowcard_try_f64(a, n, P, SO.sorted, SO.m)) { free(SO.sorted); PROF(prof_phase(LOGOS_PH_LOWCARD); prof_finish(LOGOS_ROUTE_LOWCARD, t_start);) return; }
    PROF(prof_phase(LOGOS_PH_LOWCARD);)
#ifndef LOGOS_NO_MODCARD
    PROF(g_prof_phase = LOGOS_PH_MODCARD;)
    /* LOGOS_MC_OFF=1 skips this route at runtime: the ceiling of any better gate (make cardgate) */
    if (sodo_ok && !SO.has_nan && n >= MODCARD_MIN_N && (double)SO.distinct <= (P >= WC_MIN_THREADS ? MODCARD_GATE_MT : MODCARD_GATE) * (double)SO.m && SO.chao1 <= (double)MODCARD_CAP
        && !getenv("LOGOS_MC_OFF") && modcard_try_f64(a, n, P, SO.sorted, SO.m, mc_hash_allowed(SO.chao1, P, n, SO.sorted, SO.m))) { free(SO.sorted); PROF(prof_phase(LOGOS_PH_MODCARD); prof_finish(g_prof.route == LOGOS_ROUTE_LATTICE ? LOGOS_ROUTE_LATTICE : LOGOS_ROUTE_MODCARD, t_start);) return; }
    PROF(prof_phase(LOGOS_PH_MODCARD);)
#endif
#else
    PROF(g_prof_phase = LOGOS_PH_LOWCARD;)
    if (n >= 4096 && lowcard_try_f64(a, n, P, NULL, 0)) { PROF(prof_phase(LOGOS_PH_LOWCARD); prof_finish(LOGOS_ROUTE_LOWCARD, t_start);) return; }
    PROF(prof_phase(LOGOS_PH_LOWCARD);)
    TRACE("lowcard declined");
#ifndef LOGOS_NO_MODCARD
    /* thousands of distinct values: hash-map counting, still zero scatter */
    PROF(g_prof_phase = LOGOS_PH_MODCARD;)
    if (n >= MODCARD_MIN_N && modcard_try_f64(a, n, P, NULL, 0, 1)) { PROF(prof_phase(LOGOS_PH_MODCARD); prof_finish(g_prof.route == LOGOS_ROUTE_LATTICE ? LOGOS_ROUTE_LATTICE : LOGOS_ROUTE_MODCARD, t_start);) return; }
    PROF(prof_phase(LOGOS_PH_MODCARD);)
    TRACE("modcard declined");
#endif
#endif

    if (P == 1 || n < MIN_PARALLEL) {
#if defined(LOGOS_SODO) && !defined(LOGOS_NO_STREAMS)
        double st_bnd[STREAMS_MAX_MODES], st_lo = 0, st_hi = 0; int st_nm = 0;   /* read the modes before the sample is freed */
        if (sodo_ok && !SO.has_nan && n >= STREAMS_MIN_N) {
            st_nm = streams_modes(SO.sorted, SO.m, st_bnd);
            if (st_nm >= 2) { st_lo = SO.sorted[0]; st_hi = SO.sorted[SO.m - 1]; }
        }
#endif
#ifdef LOGOS_SODO
        if (sodo_ok) { free(SO.sorted); SO.sorted = NULL; }
#endif
        void *pb = NULL; uint8_t *pbi = NULL; int pooled = pool_acquire(n * sizeof(double), 0, &pb, &pbi);
        double *buf = pooled ? (double *)pb : (double *)big_alloc(n * sizeof(double)); ctx_t c;
        if (!buf || !ctx_init(&c, n, 0)) { if (pooled) pool_release(); else free(buf); ctx_free(&c); qsort(a, n, sizeof(double), cmp_d); return; }
        c.seq = 1;
        TRACE("seq: buffers ready (pooled=%d)", pooled);
        PROF(prof_phase(LOGOS_PH_OTHER);)
#ifndef LOGOS_NO_RELOCATE
        if (n >= RELOC_MIN_N && relocate_try_f64(a, n, 1, buf)) { TRACE("seq: relocated, done"); PROF(prof_phase(LOGOS_PH_RELOCATE); prof_finish(LOGOS_ROUTE_RELOCATE, t_start);) if (pooled) pool_release(); else free(buf); ctx_free(&c); return; }   /* J5 */
        PROF(prof_phase(LOGOS_PH_RELOCATE);)
        TRACE("seq: relocation declined");
#endif
#if defined(LOGOS_SODO) && !defined(LOGOS_NO_STREAMS)
        if (st_nm >= 2 && streams_try_f64(a, n, buf, &c, st_bnd, st_nm, st_lo, st_hi)) {
            PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
            if (pooled) pool_release(); else free(buf);
            ctx_free(&c);
            return;
        }
#endif
        PROF(c.path = LOGOS_PATH_NONE; prof_stamp_t sl = prof_stamp();)
        double *r = leaf_f64(a, n, buf, &c);
        PROF(double lms = prof_busy_since(0, LOGOS_PH_BUCKETS, sl); prof_path(0, c.path, n, lms); prof_phase(LOGOS_PH_BUCKETS);)
        TRACE("seq: leaf done (result in %s)", r == a ? "a" : "buf");
        if (r != a) memcpy(a, buf, n * sizeof(double));   /* only if parity left it in scratch */
        TRACE("seq: copyback done");
        PROF(prof_phase(LOGOS_PH_COPYBACK); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
        if (pooled) pool_release(); else free(buf);
        ctx_free(&c);
        return;
    }

    job_t j; memset(&j, 0, sizeof j);
    j.P = P; j.nbk = P * BUCKETS_PER_THREAD; if (j.nbk > MAX_BUCKETS) j.nbk = P * (MAX_BUCKETS / P > 0 ? MAX_BUCKETS / P : 1); j.nsp = j.nbk - 1; j.src = a; j.n = n;
    j.wc_on = (P >= WC_MIN_THREADS);
    /* chunk grid: ~4 chunks per thread at minimum, never larger than CHUNK_MAX elements */
    j.csize = (n + (size_t)P * 4 - 1) / ((size_t)P * 4); if (j.csize > CHUNK_MAX) j.csize = CHUNK_MAX; if (j.csize < 1) j.csize = 1;
    j.nchunk = (n + j.csize - 1) / j.csize;
    void *pb = NULL; uint8_t *pbi = NULL;
    int pooled = pool_acquire(n * sizeof(double), j.nbk <= 256 ? n : 0, &pb, &pbi);
    j.dst = pooled ? (double *)pb : (double *)big_alloc(n * sizeof(double));
    j.bidx = pooled ? pbi : (j.nbk <= 256 ? (uint8_t *)big_alloc(n) : NULL);   /* optional: falls back to recomputing bucket_of */
    PROF(prof_phase(LOGOS_PH_OTHER);)
#ifndef LOGOS_NO_NEARSORTED
#ifdef LOGOS_SODO
    if (j.dst && nearsorted_parallel_f64(a, n, P, j.dst, sodo_ok ? SO.inv_rate : -1.0)) {
#else
    if (j.dst && nearsorted_parallel_f64(a, n, P, j.dst, -1.0)) {
#endif         /* keeps order instead of scattering it */
        PROF(prof_phase(LOGOS_PH_NEARSORTED); prof_finish(LOGOS_ROUTE_NEARSORTED, t_start);)
        if (pooled) pool_release(); else { free(j.bidx); free(j.dst); }
#ifdef LOGOS_SODO
        if (sodo_ok) free(SO.sorted);
#endif
        return;
    }
#endif
    PROF(prof_phase(LOGOS_PH_NEARSORTED);)
    TRACE("par: nearsorted declined");
#ifndef LOGOS_NO_RELOCATE
    if (j.dst && n >= RELOC_MIN_N && relocate_try_f64(a, n, P, j.dst)) {   /* J5: block-permuted data -> memcpy runs into place, no scatter */
        TRACE("par: relocated, done");
        PROF(prof_phase(LOGOS_PH_RELOCATE); prof_finish(LOGOS_ROUTE_RELOCATE, t_start);)
        if (pooled) pool_release(); else { free(j.bidx); free(j.dst); }
#ifdef LOGOS_SODO
        if (sodo_ok) free(SO.sorted);
#endif
        return;
    }
    TRACE("par: relocation declined");
    PROF(prof_phase(LOGOS_PH_RELOCATE);)
#endif
    double *sp = malloc((size_t)j.nsp * sizeof(double));
    j.counts = calloc(j.nchunk * j.nbk, sizeof(size_t)); j.offs = malloc(j.nchunk * j.nbk * sizeof(size_t));
    j.bsize = calloc(j.nbk, sizeof(size_t)); j.bstart = calloc(j.nbk, sizeof(size_t));
    warg_t *wa = malloc((size_t)P * sizeof(warg_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    if (!j.dst || !sp || !j.counts || !j.offs || !j.bsize || !j.bstart || !wa || !th || !ctxs) goto fallback;

    size_t m = (size_t)j.nbk * OVERSAMPLE; if (m > n) m = n;
    double *samp = NULL;
#ifdef LOGOS_SODO
    if (sodo_ok && SO.mfull == m && sodo_extend(a, n, &SO) && !SO.has_nan) { samp = SO.sorted; SO.sorted = NULL; }   /* the one sample, extended and sorted */
#endif
    if (!samp) {
        samp = malloc(m * sizeof(double)); if (!samp) goto fallback;
        for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); samp[i] = a[q < n ? q : n - 1]; }
        sample_sort(samp, m);
    }
    for (int b = 1; b < j.nbk; b++) sp[b - 1] = samp[(size_t)b * m / j.nbk];
#ifndef LOGOS_NO_SPLITTER_TABLE
    j.bt_ok = bt_init(&j.bt, sp, j.nsp, samp[0], samp[m - 1]);   /* O(1) bucket lookup for the histogram; falls back to the search if it cannot build */
#endif
    /* Uniformity certificate: the Kolmogorov-Smirnov distance between the sorted sample's empirical CDF
       and the straight line through its extremes. Uniform data gives D ~ 1/sqrt(m); anything with a
       tail, a mode or a lattice gives 0.1-0.5. Costs m subtractions on a sample already sorted. */
    j.ks_ok = 0; j.ks_d = 1.0; j.ks_lo = samp[0]; j.ks_hi = samp[m - 1]; j.ks_scale = 0;
    if (m >= 256 && j.ks_hi > j.ks_lo) {
        double D = 0, inv = 1.0 / (j.ks_hi - j.ks_lo);
        for (size_t i = 0; i < m; i++) { double F = (samp[i] - j.ks_lo) * inv, e1 = F - (double)i / (double)m, e2 = (double)(i + 1) / (double)m - F; if (e1 < 0) e1 = -e1; if (e2 < 0) e2 = -e2; if (e1 > D) D = e1; if (e2 > D) D = e2; }
        j.ks_d = D; j.ks_ok = D <= HF_KS_MAX / sqrt((double)m); j.ks_scale = (double)j.nbk * inv;
        TRACE("par: KS distance %.4f (limit %.4f) -> %s", D, HF_KS_MAX / sqrt((double)m), j.ks_ok ? "uniform" : "not uniform");
    }
    /* The same test in KEY space. The order-preserving key is monotone in the logarithm, so log-uniform
     * data (Benford's law: prices, file sizes, measured magnitudes) is uniform here and earns the same
     * one-multiply grid. A true Pareto is NOT: the log of a Pareto is exponential, and it fails this test
     * as well (measured 0.69 against a 0.027 limit), which is what the exponent map below is for. */
    j.ks_key_ok = 0; j.kk_lo = total_key(samp[0]); j.kk_hi = total_key(samp[m - 1]); j.kk_scale = 0;
#ifndef LOGOS_NO_KEY_KS
    if (!j.ks_ok && m >= 256 && j.kk_hi > j.kk_lo) {
        double span = (double)(j.kk_hi - j.kk_lo), D = 0;
        for (size_t i = 0; i < m; i++) {
            double F = (double)(total_key(samp[i]) - j.kk_lo) / span;
            double e1 = F - (double)i / (double)m, e2 = (double)(i + 1) / (double)m - F;
            if (e1 < 0) e1 = -e1;
            if (e2 < 0) e2 = -e2;
            if (e1 > D) D = e1;
            if (e2 > D) D = e2;
        }
        j.ks_key_ok = D <= HF_KS_MAX / sqrt((double)m); j.kk_scale = (double)j.nbk / span;
        TRACE("par: KS distance in key space %.4f -> %s", D, j.ks_key_ok ? "log-uniform" : "not log-uniform");
    }
#endif
    /* exponent-histogram map, and the largest share a single value holds in the sample */
    {   size_t cnt[256]; memset(cnt, 0, sizeof cnt); size_t run = 1, maxrun = 1;
        j.ex_e0 = total_key(samp[0]) >> EX_SHIFT;                       /* entries are binades above the lowest sampled */
        for (size_t i = 0; i < m; i++) {
            uint64_t q = total_key(samp[i]) >> EX_SHIFT;
            unsigned e = q <= j.ex_e0 ? 0u : (unsigned)(q - j.ex_e0); if (e > 255u) e = 255u;
            cnt[e]++;
            if (i && samp[i] == samp[i - 1]) { run++; if (run > maxrun) maxrun = run; } else run = 1;
        }
        j.dup_frac = (double)maxrun / (double)m;
        size_t cum = 0; uint32_t prev = 0; int nonempty = 0;
        for (int e = 0; e < 256; e++) {
            uint32_t base = (uint32_t)((double)j.nbk * (double)cum / (double)m);
            if (base > (uint32_t)(j.nbk - 1)) base = (uint32_t)(j.nbk - 1);
            if (base < prev) base = prev;
            cum += cnt[e];
            uint32_t next = (uint32_t)((double)j.nbk * (double)cum / (double)m);
            if (next > (uint32_t)j.nbk) next = (uint32_t)j.nbk;
            if (next < base) next = base;
            j.ex[e].base = base; j.ex[e].span = next - base;
            j.ex[e].slope = j.ex[e].span ? (double)j.ex[e].span / EX_SCALE : 0.0;
            if (cnt[e]) nonempty++;
            prev = base;
        }
        j.ex_ok = nonempty >= 2;
#ifndef LOGOS_EXMAP
        /* OPT-IN, measured negative (make tailbench): the trial selects it on lognormal and exponential
         * data because its lookup is the cheapest, and the sort then runs 0.91x and 0.80x of the key
         * splitter table it displaced. The map balances by binade, but a heavy tail decays inside each
         * binade too, so its buckets carry up to 3x their share and spill where the table spills none.
         * Kept because the machinery -- and the balance check it forced -- are what a better inverse-CDF
         * map would need. */
        j.ex_ok = 0;
#endif
        TRACE("par: exponent map over %d binades, largest single value %.2f%% of the sample", nonempty, j.dup_frac * 100.0);
    }
#ifndef LOGOS_NO_KGRID
    /* The key grid: cells of equal width in key space, each bucket edge snapped to the cell of a sample quantile. Built for
     * every sampled partition; the balance check below keeps it only if no bucket would take more than HF_BAL_MAX times its
     * share, and hf_trial takes it before timing any splitter table. LOGOS_NO_KGRID=1 skips it at runtime. */
    j.kg_ok = 0;
    if (m >= 1024 && isfinite(samp[0]) && isfinite(samp[m - 1]) && j.nbk >= 2 && j.nbk <= 256) {
        uint64_t klo = total_key(samp[0]), khi = total_key(samp[m - 1]);
        if (khi > klo) {
            uint64_t span = khi - klo; int sh = 0;
            while ((span >> sh) >= (1ull << KG_BITS)) sh++;
            uint32_t cells = (uint32_t)(span >> sh) + 1; if (cells > (1u << KG_BITS)) cells = 1u << KG_BITS;
            j.kg_lo = klo; j.kg_shift = sh; j.kg_cells = cells;
            uint32_t c = 0; int b = 0;
            for (int q = 1; q < j.nbk; q++) {                               /* edge q: the cell of the q/nbk sample quantile */
                size_t iq = (size_t)q * m / (size_t)j.nbk;
                uint64_t dq = (total_key(samp[iq]) - klo) >> sh; uint32_t cq = dq < cells ? (uint32_t)dq : cells - 1;
                for (; c < cq; c++) j.kg_tab[c] = (uint8_t)b;
                b = q;
            }
            for (; c < cells; c++) j.kg_tab[c] = (uint8_t)b;
            j.kg_ok = 1;
            TRACE("par: key grid %u cells, shift %d", cells, sh);
        }
    }
#endif
    /* Per-mode certificate: every mode uniform inside itself, tested on the sample slice that falls in it.
     * The boundary has to sit INSIDE the gulf -- a boundary at either bounding splitter leaves a stray
     * point on the wrong side and stretches the next slice across the whole gap. */
    j.md_ok = 0; j.md_nm = 0;
#ifndef LOGOS_NO_MODEKS
    if (j.bt_ok && j.bt.nm >= 2 && j.bt.nm <= 8) {
        int nm = j.bt.nm, ok = 1;
        size_t at = 0;
        for (int k = 0; k < nm && ok; k++) {
            int fi = j.bt.mfirst[k], la = j.bt.mlast[k];
            double lo = j.bt.mlo[k], hi = j.bt.mhi[k];
            if (la <= fi || !(hi > lo)) { ok = 0; break; }
            while (at < m && samp[at] < lo) at++;                       /* skip the gulf below this mode */
            size_t from = at;
            while (at < m && samp[at] < hi) at++;                       /* the mode's own points */
            size_t cnt = at - from;
            if (cnt < 64) { ok = 0; break; }
            double D = 0, inv = 1.0 / (hi - lo);
            for (size_t i = 0; i < cnt; i++) {
                double F = (samp[from + i] - lo) * inv;
                double e1 = F - (double)i / (double)cnt, e2 = (double)(i + 1) / (double)cnt - F;
                if (e1 < 0) e1 = -e1;
                if (e2 < 0) e2 = -e2;
                if (e1 > D) D = e1;
                if (e2 > D) D = e2;
            }
            if (D > HF_KS_MAX / sqrt((double)cnt)) { ok = 0; TRACE("par: mode %d is not uniform inside (KS %.3f)", k, D); break; }
            j.md_first[k] = fi; j.md_last[k] = la; j.md_lo[k] = lo; j.md_hi[k] = hi;
            j.md_scale[k] = (double)(la - fi) / (hi - lo);
        }
        if (ok) { j.md_ok = 1; j.md_nm = nm; TRACE("par: all %d modes certify uniform inside themselves", nm); }
    }
#endif
    /* A map is only usable if the partition it produces is balanced, and the per-thread trial times the
     * LOOKUP, not the partition: it once picked a map that sent 63% of the data to the spill list. Apply
     * each certified map to the sample here and drop it if any bucket would take more than HF_BAL_MAX
     * times its share. The sample's own per-bucket counts are noisy (about OVERSAMPLE each), so this
     * threshold is far looser than the region slack it protects. */
    {   uint32_t load[MAX_BUCKETS]; double lim = HF_BAL_MAX * (double)m / (double)j.nbk;
#define BAL_OK(EXPR, FLAG) do { if (FLAG) { \
            memset(load, 0, (size_t)j.nbk * sizeof(uint32_t)); \
            for (size_t i = 0; i < m; i++) { int b_ = (EXPR); if (b_ >= 0 && b_ < j.nbk) load[b_]++; } \
            uint32_t mx_ = 0; for (int b_ = 0; b_ < j.nbk; b_++) if (load[b_] > mx_) mx_ = load[b_]; \
            if ((double)mx_ > lim) { FLAG = 0; TRACE("par: a map was rejected, one bucket would take %.1fx its share", (double)mx_ * (double)j.nbk / (double)m); } } } while (0)
#ifdef LOGOS_HISTFREE
        BAL_OK(hf_lookup(&j, samp[i], 6, 0), j.ks_ok);       /* the certified grids exist only there */
        BAL_OK(hf_lookup(&j, samp[i], 7, 0), j.ks_key_ok);
#endif
        BAL_OK(hf_lookup_md(&j, samp[i]), j.md_ok);
        BAL_OK(hf_lookup_ex(&j, samp[i]), j.ex_ok);
#ifndef LOGOS_NO_KGRID
        BAL_OK(hf_lookup_kg(&j, samp[i]), j.kg_ok);
#endif
#undef BAL_OK
    }
    free(samp); j.sp = sp;
    PROF(prof_phase(LOGOS_PH_SAMPLE);)
#ifdef LOGOS_HISTFREE
    if (hf_partition(&j, P, ctxs, th, vnbk != NULL, vstart, vnbk, vmin, vmax)) {
        TRACE("par: histfree done");
        for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
        if (pooled) pool_release(); else { free(j.bidx); free(j.dst); }
#ifndef LOGOS_NO_SPLITTER_TABLE
        bt_free(&j.bt);
#endif
        free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
        PROF(prof_phase(LOGOS_PH_OTHER); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
        return;
    }
    TRACE("par: histfree declined, histogram path");
#endif

    barrier_init(&j.bar, P); atomic_init(&j.hnext, 0); atomic_init(&j.snext, 0); atomic_init(&j.next, 0);
    for (int t = 0; t < P; t++) { wa[t].j = &j; wa[t].t = t; wa[t].ctx = &ctxs[t]; }
    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, worker, &wa[t]);
    PROF(prof_stamp_t sh0 = prof_stamp();)
    do_hist(&j);                                   /* main = worker 0 */
    PROF(prof_busy_since(0, LOGOS_PH_HIST, sh0); double th1 = prof_now();)
    barrier_wait(&j.bar);
    PROF(prof_wait(0, LOGOS_PH_HIST, prof_now() - th1); prof_phase(LOGOS_PH_HIST);)
    TRACE("par: histogram done");
    /* combine: bucket sizes, starts, per-chunk write cursors (serial, O(nchunk*nbk)) */
    for (size_t ch = 0; ch < j.nchunk; ch++) for (int b = 0; b < j.nbk; b++) j.bsize[b] += j.counts[ch * j.nbk + b];
    for (int b = 1; b < j.nbk; b++) j.bstart[b] = j.bstart[b - 1] + j.bsize[b - 1];
    for (int b = 0; b < j.nbk; b++) { size_t acc = j.bstart[b]; for (size_t ch = 0; ch < j.nchunk; ch++) { j.offs[ch * j.nbk + b] = acc; acc += j.counts[ch * j.nbk + b]; } }
    size_t maxb = 1; for (int b = 0; b < j.nbk; b++) if (j.bsize[b] > maxb) maxb = j.bsize[b];
    int ok = 1; for (int t = 0; t < P; t++) if (!ctx_init(&ctxs[t], maxb, j.wc_on)) ok = 0;
    PROF(for (int t = 0; t < P; t++) ctxs[t].tid = t; g_prof.busy_ms[0][LOGOS_PH_COMBINE] += prof_now() - g_prof_tp; g_prof.cpu_ms[0][LOGOS_PH_COMBINE] += prof_now() - g_prof_tp; prof_phase(LOGOS_PH_COMBINE);)
    barrier_wait(&j.bar);
    PROF(prof_stamp_t ss0 = prof_stamp();)
    do_scatter(&j);
    PROF(prof_busy_since(0, LOGOS_PH_SCATTER, ss0); double ts1 = prof_now();)
    barrier_wait(&j.bar);
    PROF(prof_wait(0, LOGOS_PH_SCATTER, prof_now() - ts1); prof_phase(LOGOS_PH_SCATTER);)
    TRACE("par: scatter done");
    if (ok) do_buckets(&j, &ctxs[0]);
    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
    PROF(prof_phase(LOGOS_PH_BUCKETS);)
    TRACE("par: buckets done");
    barrier_destroy(&j.bar);
    for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
    if (!ok) { if (pooled) pool_release(); else { free(j.bidx); free(j.dst); } free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs); qsort(a, n, sizeof(double), cmp_d); return; }
    if (pooled) pool_release(); else { free(j.bidx); free(j.dst); }
#ifndef LOGOS_NO_SPLITTER_TABLE
    bt_free(&j.bt);
#endif
    free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    PROF(prof_phase(LOGOS_PH_OTHER); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
    return;
fallback:
#ifdef LOGOS_SODO
    if (sodo_ok) free(SO.sorted);
#endif
    if (pooled) pool_release(); else { free(j.bidx); free(j.dst); }
#ifndef LOGOS_NO_SPLITTER_TABLE
    bt_free(&j.bt);
#endif
    free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    qsort(a, n, sizeof(double), cmp_d);
}


/* ===================== native int32 engine (no widening) ===================== */
/* Leaf: port of JS sort_fn.sort_i32 — prescan asc/desc; counting when span<=n;
 * otherwise byte-skip LSD radix on the biased uint32 key. 4-byte elements
 * throughout: half the bytes of the old widen-to-double path. */
static void lsd_radix_i32(int32_t *a, size_t n, uint32_t *src, uint32_t *dst, ctx_t *c) {
    uint32_t *c0 = c->hist[0], *c1 = c->hist[1], *c2 = c->hist[2], *c3 = c->hist[3];
    memset(c0, 0, 257 * 4); memset(c1, 0, 257 * 4); memset(c2, 0, 257 * 4); memset(c3, 0, 257 * 4);
    uint32_t or_a = 0, and_a = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)a[i] ^ 0x80000000u; src[i] = v; or_a |= v; and_a &= v;
        c0[(v & 0xFF) + 1]++; c1[((v >> 8) & 0xFF) + 1]++; c2[((v >> 16) & 0xFF) + 1]++; c3[((v >> 24) & 0xFF) + 1]++;
    }
    uint32_t diff = or_a ^ and_a;
    int a0 = (diff & 0xFF) != 0, a1 = (diff & 0xFF00) != 0, a2 = (diff & 0xFF0000) != 0, a3 = (diff & 0xFF000000u) != 0;
    int last = a3 ? 3 : a2 ? 2 : a1 ? 1 : a0 ? 0 : -1;
    if (last < 0) { for (size_t i = 0; i < n; i++) a[i] = (int32_t)(src[i] ^ 0x80000000u); return; }
    uint32_t *from = src, *to = dst, *tmp;
#define RPI(cc, sh, act, idx) \
    if (act) { for (int b = 1; b < 257; b++) cc[b] += cc[b - 1]; \
        if (last == idx) { for (size_t i = 0; i < n; i++) { uint32_t v = from[i]; a[cc[(v >> sh) & 0xFF]++] = (int32_t)(v ^ 0x80000000u); } return; } \
        for (size_t i = 0; i < n; i++) { if (i + PF_DIST < n) PREFETCH_W(to + cc[(from[i + PF_DIST] >> sh) & 0xFF]); uint32_t v = from[i]; to[cc[(v >> sh) & 0xFF]++] = v; } tmp = from; from = to; to = tmp; }
    RPI(c0, 0, a0, 0)
    RPI(c1, 8, a1, 1)
    RPI(c2, 16, a2, 2)
    for (int b = 1; b < 257; b++) c3[b] += c3[b - 1];
    for (size_t i = 0; i < n; i++) { uint32_t v = from[i]; a[c3[(v >> 24) & 0xFF]++] = (int32_t)(v ^ 0x80000000u); }
#undef RPI
}
/* Sequential counting gate: the leaf counts when span <= n / LEAF_I32_COUNT_DIV. DIV 1 is the inherited
 * "span <= n". LOGOS_I32_COUNT_DIV in the environment overrides it, uncached, so ONE binary can sweep the
 * gate with no code-layout bias (make i32abbench, experiments/team/i32-sentinel-seq.md). */
#ifndef LEAF_I32_COUNT_DIV
#define LEAF_I32_COUNT_DIV 4            /* was 1 ("span <= n"). Measured at 8M, 1 thread, paired (make i32abbench C):
                                           counting still beats the 3-pass radix at span 2e6 (radix 0.83x) and loses
                                           at 4e6 (radix 1.27-1.36x) and at span n (2.0x): the table stops being cache
                                           resident between n/4 and n/2. n/4 takes every win and no loss; 1e6 rows are
                                           unchanged, and the parallel leaves (buckets of n/168) measure neutral. */
#endif
static inline int64_t leaf_i32_count_max(size_t n) {
    const char *e = getenv("LOGOS_I32_COUNT_DIV"); long d = e ? atol(e) : LEAF_I32_COUNT_DIV; if (d < 1) d = 1;
    return (int64_t)(n / (size_t)d);
}
/* Windowed counting for the SEQUENTIAL leaf (experiments/team/i32-sentinel-seq.md). One far value pushes
 * the span past the table -- INT32_MIN among a 0..99 bulk cost ~2.5x at one thread, make intbench -- and
 * the parallel route already answered that in count_try_i32: take the window from the SAMPLE, count the
 * window, spill the tail. Same idea here, on one thread, with the tails going into the scratch buffer
 * (low tail from the front, high tail from the back), sorted by a recursive call and placed before /
 * after the bulk. Slot order is value order and the tails lie outside the window on both sides, so there
 * is no merge. Returns 1 if a[] was sorted, 0 if it declined; nothing is written to a[] until every
 * element has been counted or spilled, so declining costs at most one read pass. The tail budget is
 * n/32 like the parallel route, and the trim is K = 5 of a 2048-point sample, so a sentinel rarer than
 * about 0.25% is absorbed and a frequent one declines at the sample before touching the array.
 * -DLOGOS_NO_I32_LEAF_WINDOW compiles it out; LOGOS_I32_NO_LEAF_WINDOW in the environment turns it off
 * at run time (uncached, so one binary can A/B both, make i32abbench). */
#ifndef LEAF_I32_WINDOW_MIN_N
#define LEAF_I32_WINDOW_MIN_N 4096      /* below this the sample is the array and the radix is cheap anyway */
#endif
#ifndef LEAF_I32_SPILL_DIV
#define LEAF_I32_SPILL_DIV 32           /* tail budget n/32, the same as count_try_i32 */
#endif
static inline int leaf_i32_window_off(void) { return getenv("LOGOS_I32_NO_LEAF_WINDOW") != NULL; }
static void leaf_i32(int32_t *a, size_t n, uint32_t *buf, ctx_t *c);
static int leaf_i32_windowed(int32_t *a, size_t n, uint32_t *buf, ctx_t *c, int64_t cmax) {
    size_t m = 2048; if (m > n / 4) m = n / 4;
    enum { K = 5 };
    int32_t lo5[K], hi5[K]; int nl = 0, nh = 0;
    for (size_t i = 0; i < m; i++) {
        size_t q = SAMPLE_POS(i, m, n); int32_t v = a[q < n ? q : n - 1];
        if (nl < K) { int j = nl++; while (j > 0 && lo5[j - 1] > v) { lo5[j] = lo5[j - 1]; j--; } lo5[j] = v; }
        else if (v < lo5[K - 1]) { int j = K - 1; while (j > 0 && lo5[j - 1] > v) { lo5[j] = lo5[j - 1]; j--; } lo5[j] = v; }
        if (nh < K) { int j = nh++; while (j > 0 && hi5[j - 1] < v) { hi5[j] = hi5[j - 1]; j--; } hi5[j] = v; }
        else if (v > hi5[K - 1]) { int j = K - 1; while (j > 0 && hi5[j - 1] < v) { hi5[j] = hi5[j - 1]; j--; } hi5[j] = v; }
    }
    if (nl == 0) return 0;
    int32_t sfl = lo5[0], sfh = hi5[0];                                  /* the sample's own range: covers the most */
    int32_t stl = nl >= K ? lo5[K - 1] : lo5[0], sth = nh >= K ? hi5[K - 1] : hi5[0];   /* trimmed: survives a seen sentinel */
    int32_t wlo, whi;
    if ((int64_t)sfh - (int64_t)sfl + 1 <= cmax) { wlo = sfl; whi = sfh; }
    else if ((int64_t)sth - (int64_t)stl + 1 <= cmax) { wlo = stl; whi = sth; }
    else return 0;                                                       /* even the trimmed window overflows: not a tail */
    int64_t span = (int64_t)whi - (int64_t)wlo + 1;
    uint32_t *C = c->aux; memset(C, 0, (size_t)span * sizeof(uint32_t));
    size_t nlo = 0, hat = n, nsp = 0, spcap = n / LEAF_I32_SPILL_DIV + 64;
    for (size_t i = 0; i < n; i++) {
        int32_t v = a[i];
        if (v >= wlo && v <= whi) C[(size_t)((int64_t)v - wlo)]++;
        else { if (++nsp > spcap) return 0; if (v < wlo) buf[nlo++] = (uint32_t)v; else buf[--hat] = (uint32_t)v; }
    }
    size_t nhi = n - hat, w = nlo;
    for (int64_t v = 0; v < span; v++) { uint32_t k = C[v]; int32_t val = (int32_t)(v + wlo); while (k-- > 0) a[w++] = val; }
    TRACE("i32 leaf: windowed count n=%zu span=%lld window [%d,%d] spill lo=%zu hi=%zu", n, (long long)span, wlo, whi, nlo, nhi);
    /* both tails out of the scratch before either recursion reuses it (a tail is at most n/32 + 64) */
    for (size_t i = 0; i < nlo; i++) a[i] = (int32_t)buf[i];
    for (size_t i = 0; i < nhi; i++) a[hat + i] = (int32_t)buf[hat + i];
    if (nlo > 1) leaf_i32(a, nlo, buf, c);
    if (nhi > 1) leaf_i32(a + hat, nhi, buf, c);
    return 1;
}
static void leaf_i32(int32_t *a, size_t n, uint32_t *buf, ctx_t *c) {   /* buf: n uint32 scratch */
    if (n < 2) return;
    int32_t mn = a[0], mx = a[0]; int asc = 1, desc = 1;
    for (size_t i = 1; i < n; i++) { int32_t v = a[i]; if (v < mn) mn = v; else if (v > mx) mx = v; if (asc && v < a[i - 1]) asc = 0; if (desc && v > a[i - 1]) desc = 0; }
    if (asc) return;
    if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { int32_t t = a[l]; a[l] = a[r]; a[r] = t; } return; }
    int64_t span = (int64_t)mx - (int64_t)mn + 1;
    int64_t cmax = leaf_i32_count_max(n);
    if (span <= cmax) {
        uint32_t *C = c->aux; memset(C, 0, (size_t)span * sizeof(uint32_t));
        for (size_t i = 0; i < n; i++) C[(size_t)((int64_t)a[i] - mn)]++;
        size_t w = 0;
        for (int64_t v = 0; v < span; v++) { uint32_t k = C[v]; int32_t val = (int32_t)(v + mn); while (k-- > 0) a[w++] = val; }
        return;
    }
#ifndef LOGOS_NO_I32_LEAF_WINDOW
    /* the raw span overflowed the table; a sample-derived window may still fit (the sentinel case) */
    if (n >= LEAF_I32_WINDOW_MIN_N && !leaf_i32_window_off() && leaf_i32_windowed(a, n, buf, c, cmax)) return;
#endif
    if (g_simd_i32) { g_simd_i32(a, n); return; }               /* SIMD leaf for full-range ints */
    lsd_radix_i32(a, n, c->aux, buf, c);
}
static inline int bucket_of_i32(int32_t x, const int32_t *sp, int nsp) {
    if (nsp <= 32) { int b = 0; for (int i = 0; i < nsp; i++) b += (sp[i] <= x); return b; }
    const int32_t *p = sp; int len = nsp;
    while (len > 0) { int half = len >> 1; int go = (p[half] <= x); p += go ? half + 1 : 0; len = go ? len - half - 1 : half; }
    return (int)(p - sp);
}
typedef struct {
    int P, nbk, nsp; int32_t *src, *dst; size_t n; const int32_t *sp;
    size_t nchunk, csize; size_t *counts, *offs, *bstart, *bsize;
    barrier_t bar; atomic_size_t hnext, snext, next; uint8_t *bidx;
    int lazy;                    /* 1: place the buckets but do not sort them (logos_partition_i32) */
    int32_t *vmin, *vmax;        /* lazy: per-bucket extremes, computed while the bucket is copied back */
} jobi_t;
typedef struct { jobi_t *j; int t; ctx_t *ctx; } wargi_t;
static void do_hist_i32(jobi_t *j) {
    size_t ch;
    while ((ch = atomic_fetch_add(&j->hnext, 1)) < j->nchunk) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n;
        size_t *c = j->counts + ch * j->nbk; for (int b = 0; b < j->nbk; b++) c[b] = 0;
        if (j->bidx) { uint8_t *bx = j->bidx; for (size_t i = lo; i < hi; i++) { int b = bucket_of_i32(j->src[i], j->sp, j->nsp); bx[i] = (uint8_t)b; c[b]++; } }
        else for (size_t i = lo; i < hi; i++) c[bucket_of_i32(j->src[i], j->sp, j->nsp)]++;
    }
}
static void do_scatter_i32(jobi_t *j) {
    size_t ch;
    while ((ch = atomic_fetch_add(&j->snext, 1)) < j->nchunk) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n;
        size_t *o = j->offs + ch * j->nbk; const uint8_t *bx = j->bidx;
        if (bx) { for (size_t i = lo; i < hi; i++) { if (i + PF_DIST < hi) PREFETCH_W(j->dst + o[bx[i + PF_DIST]]); j->dst[o[bx[i]]++] = j->src[i]; } }
        else for (size_t i = lo; i < hi; i++) { int32_t x = j->src[i]; j->dst[o[bucket_of_i32(x, j->sp, j->nsp)]++] = x; }
    }
}
static void do_buckets_i32(jobi_t *j, ctx_t *ctx) {
    size_t b;
    while ((b = atomic_fetch_add(&j->next, 1)) < (size_t)j->nbk) {
        size_t s = j->bstart[b], nb = j->bsize[b];
        if (j->lazy) {
            /* A view only needs the bucket PLACED (see the f64 twin in hf_leaves): copy it back and
             * record its extremes on the way, which a value query needs to find x's bucket exactly. An
             * empty bucket reads (INT32_MAX, INT32_MIN); the view repairs that to keep its searches
             * monotone. The leaf, the part a view defers, is skipped. */
            int32_t mn = INT32_MAX, mx = INT32_MIN;
            for (size_t i = 0; i < nb; i++) { int32_t x = j->dst[s + i]; j->src[s + i] = x; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
            if (j->vmin) { j->vmin[b] = mn; j->vmax[b] = mx; }
            continue;
        }
        if (g_simd_i32 && nb > 1) g_simd_i32(j->dst + s, nb);
        else if (nb > 1) leaf_i32(j->dst + s, nb, (uint32_t *)(j->src + s), ctx);
        if (nb) memcpy(j->src + s, j->dst + s, nb * sizeof(int32_t));
    }
}
static void *worker_i32(void *p) {
    wargi_t *w = p; jobi_t *j = w->j;
    do_hist_i32(j); barrier_wait(&j->bar); barrier_wait(&j->bar);
    do_scatter_i32(j); barrier_wait(&j->bar);
    do_buckets_i32(j, w->ctx);
    return NULL;
}
#ifndef LOGOS_NO_COUNT_I32
static void psort_i32(int32_t *a, size_t n, int P);
/* ===================== parallel counting sort for int32 =====================
 * No probing. An int32 array is a lattice of step 1 by the TYPE, so the only question is whether the
 * span fits a table, and that is a min/max pass. Contrast the f64 lattice route, which has to propose
 * a step from the sample, verify every element against it with a relative tolerance, screen NaNs, and
 * keep an abort path for two distinct doubles landing on one slot. None of that can happen here,
 * because int32 -> uint32 is a bijection. The type answers what the float path must ask.
 *
 * The span MUST be computed in 64 bits. INT32_MAX - INT32_MIN is -1 in 32-bit arithmetic, so a
 * `span <= budget` gate written in 32 bits passes on the widest possible input and then sizes a table
 * from -1 (`make intbench` prints this before it benchmarks anything).
 *
 * Chunks are handed out by an atomic counter rather than sliced statically, so on a hybrid CPU the
 * efficiency cores pull fewer chunks instead of becoming stragglers behind a join (FINDINGS 3v).
 *
 * Measured before this existed (`make intbench`, 8M): the parallel path lost to a SINGLE core for
 * every span below about n/8, by up to 3x, because it had no counting route to reach. */
#ifndef COUNT_I32_MAX_SLOTS
#define COUNT_I32_MAX_SLOTS (1 << 18)   /* the OLD fixed cap: per-thread table 1 MB, 14 MB at 14 threads.
                                           Kept as the A/B baseline (LOGOS_I32_FIXED_CAP in the environment
                                           restores it). It left a band, 262144 < span <= ~n/8, where the
                                           partition LOST to one core (intbench span 1e6, P/1 0.60-0.87). */
#endif
/* Table budget (experiments/team/i32-sentinel-seq.md). The per-thread tables cost 4 B x span x threads, so
 * instead of a fixed span cap the route fits the THREAD COUNT to a memory budget: Pc = min(count_threads,
 * budget / span), and declines below COUNT_MIN_THREADS threads or above span n. At 8M and 64 MB that is
 * 14 threads to span 1.2M, 8 at 2M, 4 at 4M, and a decline at 8M where the partition already wins.
 * LOGOS_I32_TABLE_MB in the environment overrides the budget, uncached, so one binary can sweep it. */
#ifndef COUNT_I32_TABLE_BUDGET_MB
#define COUNT_I32_TABLE_BUDGET_MB 64
#endif
static inline size_t cnti_budget_slots(void) {
    const char *e = getenv("LOGOS_I32_TABLE_MB"); long mb = e ? atol(e) : COUNT_I32_TABLE_BUDGET_MB; if (mb < 1) mb = 1;
    return (size_t)mb << 18;                                      /* MB -> 4-byte slots */
}
static inline int cnti_fixed_cap(void) { return getenv("LOGOS_I32_FIXED_CAP") != NULL; }
/* Persistent count tables (experiments/team/i32-sentinel-seq.md). Traced at span 1e6, 14 threads, 8M: the
 * 53 MB of per-thread tables cost 8-28 ms just to FREE (page teardown) on top of the faults taken inside the
 * count pass, and a 32 MB budget (8 threads) beat the 64 MB one (14 threads) by 1.3x for that reason alone.
 * So the tables live in a static pool, grown when a call needs more and never freed, each thread zeroing its
 * own table before it counts (a parallel memset, ~0.3 ms for 4 MB); the same one-sort-at-a-time contract as
 * the ctx bundles. Footprint is bounded by the budget. -DLOGOS_NO_CNTI_POOL restores calloc/free per call;
 * LOGOS_I32_NO_TABLE_POOL in the environment does the same at run time (uncached, for the A/B). */
#ifndef LOGOS_NO_CNTI_POOL
#define CNTI_POOL_MAX 64
static uint32_t *g_cnti_tab[CNTI_POOL_MAX]; static size_t g_cnti_cap[CNTI_POOL_MAX];
static inline int cnti_pool_off(void) { return getenv("LOGOS_I32_NO_TABLE_POOL") != NULL; }
#endif
typedef struct {
    int32_t *a; size_t n; int P;
    int64_t mn, span;
    uint32_t **tab; size_t *tot;
    size_t csize, nchunk; atomic_size_t next;
    barrier_t bar;
    /* windowed mode: the table covers [wlo, whi] and everything else spills (see cnti_window) */
    int32_t wlo, whi; int32_t **spill; size_t *spn, spcap;
    int32_t *sbuf, *sbuf2; size_t n_below, n_spill; atomic_int fail;
    int pooled;                                                   /* tables come from the persistent pool: zero them here */
} cnti_t;
typedef struct { cnti_t *c; int t; int32_t lo, hi; } cnti_arg_t;

static void *cnti_minmax(void *p) {
    cnti_arg_t *w = p; cnti_t *c = w->c;
    size_t chunk = (c->n + (size_t)c->P - 1) / (size_t)c->P;
    size_t lo = (size_t)w->t * chunk, hi = lo + chunk; if (hi > c->n) hi = c->n;
    int32_t mn = INT32_MAX, mx = INT32_MIN;                       /* neutral when the slice is empty */
    for (size_t i = lo; i < hi; i++) { int32_t v = c->a[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    w->lo = mn; w->hi = mx; return NULL;
}
static void *cnti_body(void *p) {
    cnti_arg_t *w = p; cnti_t *c = w->c; int t = w->t, P = c->P;
    uint32_t *T = c->tab[t]; size_t ch, sn = 0;
    if (c->pooled) memset(T, 0, (size_t)c->span * sizeof(uint32_t));   /* own table only; others are read after the barrier */
    int32_t *sp = c->spill ? c->spill[t] : NULL;
    const int32_t wlo = c->wlo, whi = c->whi;
    while ((ch = atomic_fetch_add(&c->next, 1)) < c->nchunk) {
        size_t lo = ch * c->csize, hi = lo + c->csize; if (hi > c->n) hi = c->n;
        if (!sp) { for (size_t i = lo; i < hi; i++) T[(size_t)((int64_t)c->a[i] - c->mn)]++; }
        else for (size_t i = lo; i < hi; i++) {          /* windowed: the tail goes to the spill list */
            int32_t v = c->a[i];
            if (v >= wlo && v <= whi) T[(size_t)((int64_t)v - c->mn)]++;
            else if (sn < c->spcap) sp[sn++] = v;
            else atomic_store(&c->fail, 1);              /* more tail than budgeted: decline after the join */
        }
    }
    if (sp) c->spn[t] = sn;
    barrier_wait(&c->bar);
    /* Nothing has been written to a[] yet, so declining here is free. Every thread must still reach
     * the same number of barriers, hence the two waits rather than an early return. */
    if (atomic_load(&c->fail)) { barrier_wait(&c->bar); barrier_wait(&c->bar); return NULL; }
    /* Each thread owns a contiguous slot range and folds the P tables into tab[0] over it. The ranges
     * are disjoint, so every slot is written by exactly one thread and read only by that thread. */
    int64_t s0 = c->span * t / P, s1 = c->span * (t + 1) / P; size_t tot = 0;
    for (int64_t v = s0; v < s1; v++) {
        uint32_t sum = 0; for (int q = 0; q < P; q++) sum += c->tab[q][v];
        c->tab[0][v] = sum; tot += sum;
    }
    c->tot[t] = tot;
    barrier_wait(&c->bar);
    /* One thread gathers the spilled tail, splits it at the window (everything below the window sorts
     * before the bulk, everything above sorts after it, with no merge), and sorts each side. The
     * recursion is bounded: a spill list is at most n/32, so the depth is under five. */
    if (t == 0 && sp) {
        size_t tot_sp = 0; for (int q = 0; q < P; q++) tot_sp += c->spn[q];
        size_t at2 = 0;
        for (int q = 0; q < P; q++) { if (c->spn[q]) memcpy(c->sbuf + at2, c->spill[q], c->spn[q] * sizeof(int32_t)); at2 += c->spn[q]; }
        size_t nb = 0; for (size_t i = 0; i < tot_sp; i++) if (c->sbuf[i] < wlo) nb++;
        size_t li = 0, hi2 = nb;
        for (size_t i = 0; i < tot_sp; i++) { int32_t v = c->sbuf[i]; if (v < wlo) c->sbuf2[li++] = v; else c->sbuf2[hi2++] = v; }
        if (nb > 1) psort_i32(c->sbuf2, nb, 1);
        if (tot_sp - nb > 1) psort_i32(c->sbuf2 + nb, tot_sp - nb, 1);
        c->n_below = nb; c->n_spill = tot_sp;
    }
    barrier_wait(&c->bar);
    /* Slot order is value order, so this thread's output is one contiguous run of the array. */
    size_t at = c->n_below; for (int q = 0; q < t; q++) at += c->tot[q];
    for (int64_t v = s0; v < s1; v++) {
        uint32_t k = c->tab[0][v]; int32_t val = (int32_t)(v + c->mn);
        while (k-- > 0) c->a[at++] = val;
    }
    if (t == 0 && sp && c->n_spill) {                     /* the two tails, into regions nobody else touches */
        if (c->n_below) memcpy(c->a, c->sbuf2, c->n_below * sizeof(int32_t));
        size_t na = c->n_spill - c->n_below;
        if (na) memcpy(c->a + c->n - na, c->sbuf2 + c->n_below, na * sizeof(int32_t));
    }
    return NULL;
}
/* returns 1 if the array was sorted, 0 if declined with a[] untouched */
static int count_try_i32(int32_t *a, size_t n, int P) {
    if (n > UINT32_MAX) return 0;                                 /* counts are uint32 */
    /* Sample pre-gate, on the calling thread, before anything is allocated or spawned. A sample's
     * span is a LOWER bound on the true span, so a sample that already overflows the table proves
     * the array does, and we decline for free with no false negatives. Without this, wide data pays
     * a full parallel min/max pass only to be turned away -- the J5 mistake, see the ledger. */
    int windowed = 0; int32_t wlo = 0, whi = 0;
    int32_t sfull_lo = 0, sfull_hi = 0, strim_lo = 0, strim_hi = 0;
    int Pc0 = count_threads(n, P); if (Pc0 < 1) Pc0 = 1;         /* merging P tables costs O(span*P) */
    int64_t max_slots; size_t budget_slots = 0;
    if (cnti_fixed_cap()) max_slots = COUNT_I32_MAX_SLOTS;
    else {
        budget_slots = cnti_budget_slots();
        int floor_t = Pc0 < COUNT_MIN_THREADS ? Pc0 : COUNT_MIN_THREADS;
        max_slots = (int64_t)(budget_slots / (size_t)floor_t);    /* the widest span that still gets floor_t threads */
        if (max_slots > (int64_t)n) max_slots = (int64_t)n;       /* the leaf's rule: a table wider than n is sparse */
    }
    {   size_t m = 2048; if (m > n) m = n;
        /* Track the K smallest and K largest of the sample, which gives both the plain min/max gate
         * and a TRIMMED window in one pass. The trimmed window is what survives a sentinel: a value
         * that appears in fewer than K of the sample's m points cannot move it. */
        enum { K = 5 };
        int32_t lo5[K], hi5[K]; int nl = 0, nh = 0;
        for (size_t i = 0; i < m; i++) {
            size_t q = SAMPLE_POS(i, m, n); int32_t v = a[q < n ? q : n - 1];
            if (nl < K) { int j = nl++; while (j > 0 && lo5[j - 1] > v) { lo5[j] = lo5[j - 1]; j--; } lo5[j] = v; }
            else if (v < lo5[K - 1]) { int j = K - 1; while (j > 0 && lo5[j - 1] > v) { lo5[j] = lo5[j - 1]; j--; } lo5[j] = v; }
            if (nh < K) { int j = nh++; while (j > 0 && hi5[j - 1] < v) { hi5[j] = hi5[j - 1]; j--; } hi5[j] = v; }
            else if (v > hi5[K - 1]) { int j = K - 1; while (j > 0 && hi5[j - 1] < v) { hi5[j] = hi5[j - 1]; j--; } hi5[j] = v; }
        }
        if (nl == 0) return 0;
        sfull_lo = lo5[0]; sfull_hi = hi5[0];                            /* sample min/max: bounds the bulk */
        strim_lo = nl >= K ? lo5[K - 1] : lo5[0];                        /* trimmed: survives a rare sentinel */
        strim_hi = nh >= K ? hi5[K - 1] : hi5[0];
        /* The trimmed window is the narrowest candidate, so if even that overflows the table nothing
         * will fit and we decline here, before spawning anything (the J5 lesson). */
        if ((int64_t)strim_hi - (int64_t)strim_lo + 1 > max_slots) return 0;
    }
    cnti_t c; memset(&c, 0, sizeof c);
    atomic_init(&c.fail, 0);
    c.a = a; c.n = n; c.P = P;
    cnti_arg_t *wa = calloc((size_t)P, sizeof(cnti_arg_t));
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!wa || !th) { free(wa); free(th); return 0; }
    for (int t = 0; t < P; t++) { wa[t].c = &c; wa[t].t = t; }
    TRACE("i32: count route pre-gate passed, max_slots %lld", (long long)max_slots);
    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, cnti_minmax, &wa[t]);
    cnti_minmax(&wa[0]);
    for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
    TRACE("i32: parallel min/max done");
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    for (int t = 0; t < P; t++) { if (wa[t].lo < mn) mn = wa[t].lo; if (wa[t].hi > mx) mx = wa[t].hi; }
    int64_t span = (int64_t)mx - (int64_t)mn + 1;                 /* 64-bit on purpose: see the header */
    if (span > max_slots) {
        /* The true range does not fit but a sample-derived window might, which is the sentinel case:
         * the tail is too far away to count and too rare for the sample to have seen. Prefer the
         * sample's own min/max, which covers the most; fall back to the trimmed window when the
         * sample did see the sentinel. Either way the tail spills and is sorted as its own run. */
        if ((int64_t)sfull_hi - (int64_t)sfull_lo + 1 <= max_slots) { wlo = sfull_lo; whi = sfull_hi; }
        else { wlo = strim_lo; whi = strim_hi; }
        windowed = 1; mn = wlo; span = (int64_t)whi - (int64_t)wlo + 1;
    }
    if (span < 2 || span > max_slots) { free(wa); free(th); return 0; }
    int Pc = Pc0;
    if (budget_slots) { size_t pm = budget_slots / (size_t)span; if (pm < (size_t)Pc) Pc = (int)pm; }   /* fit the threads to the budget */
    if (Pc < 1) Pc = 1;
    int ok = 1;
    c.P = Pc; c.mn = mn; c.span = span; c.wlo = wlo; c.whi = whi;
    c.tab = calloc((size_t)Pc, sizeof(uint32_t *)); c.tot = calloc((size_t)Pc, sizeof(size_t));
    if (!c.tab || !c.tot) ok = 0;
    if (ok && windowed) {
        /* Budget for the tail. Beyond this the route declines and the partition handles it, which is
         * the right answer: a "tail" worth more than a thirty-second of the array is not a tail. */
        c.spcap = n / 32 / (size_t)Pc + 1024;
        c.spill = calloc((size_t)Pc, sizeof(int32_t *)); c.spn = calloc((size_t)Pc, sizeof(size_t));
        c.sbuf = malloc(c.spcap * (size_t)Pc * sizeof(int32_t)); c.sbuf2 = malloc(c.spcap * (size_t)Pc * sizeof(int32_t));
        if (!c.spill || !c.spn || !c.sbuf || !c.sbuf2) ok = 0;
        for (int t = 0; ok && t < Pc; t++) { c.spill[t] = malloc(c.spcap * sizeof(int32_t)); if (!c.spill[t]) ok = 0; }
    }
#ifndef LOGOS_NO_CNTI_POOL
    if (ok && Pc <= CNTI_POOL_MAX && !cnti_pool_off()) {
        c.pooled = 1;
        for (int t = 0; ok && t < Pc; t++) {
            if (g_cnti_cap[t] < (size_t)span) { free(g_cnti_tab[t]); g_cnti_tab[t] = malloc((size_t)span * sizeof(uint32_t)); g_cnti_cap[t] = g_cnti_tab[t] ? (size_t)span : 0; }
            c.tab[t] = g_cnti_tab[t]; if (!c.tab[t]) ok = 0;
        }
    } else
#endif
    for (int t = 0; ok && t < Pc; t++) { c.tab[t] = calloc((size_t)span, sizeof(uint32_t)); if (!c.tab[t]) ok = 0; }
    TRACE("i32: %d tables of %lld slots allocated (%.1f MB)", Pc, (long long)span, (double)Pc * (double)span * 4.0 / 1048576.0);
    if (ok) {
        c.csize = (n + (size_t)Pc * 4 - 1) / ((size_t)Pc * 4); if (c.csize < 4096) c.csize = 4096;
        c.nchunk = (n + c.csize - 1) / c.csize;
        atomic_init(&c.next, 0); barrier_init(&c.bar, Pc);
        for (int t = 1; t < Pc; t++) pthread_create(&th[t], NULL, cnti_body, &wa[t]);
        cnti_body(&wa[0]);
        for (int t = 1; t < Pc; t++) pthread_join(th[t], NULL);
        barrier_destroy(&c.bar);
        if (atomic_load(&c.fail)) ok = 0;                         /* more tail than budgeted; a[] untouched */
        TRACE("i32: counting route, span %lld, %d threads, %s, spill %zu", (long long)span, Pc,
              windowed ? "windowed" : "full range", c.n_spill);
    }
    if (!c.pooled) { for (int t = 0; c.tab && t < Pc; t++) free(c.tab[t]); }
    if (c.tab) for (int t = 0; t < Pc; t++) c.tab[t] = NULL;
    TRACE("i32: tables %s", c.pooled ? "returned to the pool" : "freed");
    for (int t = 0; c.tab && t < Pc; t++) free(c.tab[t]);
    for (int t = 0; c.spill && t < Pc; t++) free(c.spill[t]);
    free(c.spill); free(c.spn); free(c.sbuf); free(c.sbuf2);
    free(c.tab); free(c.tot); free(wa); free(th);
    return ok;
}
#endif  /* LOGOS_NO_COUNT_I32 */
/* vstart/vnbk non-NULL: partition only (the int32 twin of psort_f64's lazy mode). *vnbk is 0 whenever
 * the array came out FULLY sorted instead: the sequential leaf, the prescan and the counting route all
 * produce sorted output, and so does the qsort fallback, so 0 means "nothing left to do". */
static void psort_i32_ex(int32_t *a, size_t n, int P, size_t *vstart, int *vnbk, int32_t *vmin, int32_t *vmax);
static void psort_i32(int32_t *a, size_t n, int P) { psort_i32_ex(a, n, P, NULL, NULL, NULL, NULL); }
static void psort_i32_ex(int32_t *a, size_t n, int P, size_t *vstart, int *vnbk, int32_t *vmin, int32_t *vmax) {
    if (vnbk) *vnbk = 0;
    if (P < 1) P = 1;
    if (n < 2) return;
    if ((size_t)P > n) P = (int)n;
    P = effective_threads(n, P);
    TRACE_START();
    if (P == 1 || n < MIN_PARALLEL) {
        uint32_t *buf = malloc(n * sizeof(uint32_t)); ctx_t c;
        if (!buf || !ctx_init(&c, n, 0)) { free(buf); ctx_free(&c); qsort(a, n, sizeof(int32_t), cmp_i32); return; }
        leaf_i32(a, n, buf, &c);
        free(buf); ctx_free(&c);
        return;
    }
    { int asc = 1, desc = 1;
      for (size_t i = 1; i < n && (asc || desc); i++) { if (a[i] < a[i - 1]) asc = 0; if (a[i] > a[i - 1]) desc = 0; }
      if (asc) return;
      if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { int32_t t = a[l]; a[l] = a[r]; a[r] = t; } return; } }
#ifndef LOGOS_NO_COUNT_I32
    /* the type already says this is a lattice of step 1; the only question is whether it fits a table */
    if (count_try_i32(a, n, P)) return;
#endif
    jobi_t j; memset(&j, 0, sizeof j);
    j.lazy = vnbk != NULL; j.vmin = j.lazy ? vmin : NULL; j.vmax = j.lazy ? vmax : NULL;
    j.P = P; j.nbk = P * BUCKETS_PER_THREAD; if (j.nbk > MAX_BUCKETS) j.nbk = P * (MAX_BUCKETS / P > 0 ? MAX_BUCKETS / P : 1); j.nsp = j.nbk - 1; j.src = a; j.n = n;
    j.csize = (n + (size_t)P * 4 - 1) / ((size_t)P * 4); if (j.csize > CHUNK_MAX) j.csize = CHUNK_MAX; if (j.csize < 1) j.csize = 1;
    j.nchunk = (n + j.csize - 1) / j.csize;
    j.dst = malloc(n * sizeof(int32_t));
    j.bidx = j.nbk <= 256 ? malloc(n) : NULL;
    int32_t *sp = malloc((size_t)j.nsp * sizeof(int32_t));
    j.counts = calloc(j.nchunk * j.nbk, sizeof(size_t)); j.offs = malloc(j.nchunk * j.nbk * sizeof(size_t));
    j.bsize = calloc(j.nbk, sizeof(size_t)); j.bstart = calloc(j.nbk, sizeof(size_t));
    wargi_t *wa = malloc((size_t)P * sizeof(wargi_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    int32_t *samp = NULL;
    if (!j.dst || !sp || !j.counts || !j.offs || !j.bsize || !j.bstart || !wa || !th || !ctxs) goto fallback;
    { size_t m = (size_t)j.nbk * OVERSAMPLE; if (m > n) m = n;
      samp = malloc(m * sizeof(int32_t)); if (!samp) goto fallback;
      size_t step = n / m; if (step == 0) step = 1; (void)step;
      for (size_t i = 0; i < m; i++) { size_t q = SAMPLE_POS(i, m, n); samp[i] = a[q < n ? q : n - 1]; }
      qsort(samp, m, sizeof(int32_t), cmp_i32);
      for (int b = 1; b < j.nbk; b++) sp[b - 1] = samp[(size_t)b * m / j.nbk];
      free(samp); samp = NULL; j.sp = sp; }
    barrier_init(&j.bar, P); atomic_init(&j.hnext, 0); atomic_init(&j.snext, 0); atomic_init(&j.next, 0);
    for (int t = 0; t < P; t++) { wa[t].j = &j; wa[t].t = t; wa[t].ctx = &ctxs[t]; }
    for (int t = 1; t < P; t++) pthread_create(&th[t], NULL, worker_i32, &wa[t]);
    do_hist_i32(&j);
    barrier_wait(&j.bar);
    for (size_t ch = 0; ch < j.nchunk; ch++) for (int b = 0; b < j.nbk; b++) j.bsize[b] += j.counts[ch * j.nbk + b];
    for (int b = 1; b < j.nbk; b++) j.bstart[b] = j.bstart[b - 1] + j.bsize[b - 1];
    for (int b = 0; b < j.nbk; b++) { size_t acc = j.bstart[b]; for (size_t ch = 0; ch < j.nchunk; ch++) { j.offs[ch * j.nbk + b] = acc; acc += j.counts[ch * j.nbk + b]; } }
    { size_t maxb = 1; for (int b = 0; b < j.nbk; b++) if (j.bsize[b] > maxb) maxb = j.bsize[b];
      int ok = 1; for (int t = 0; t < P; t++) if (!ctx_init(&ctxs[t], maxb, 0)) ok = 0;
      barrier_wait(&j.bar);
      do_scatter_i32(&j);
      barrier_wait(&j.bar);
      if (ok) do_buckets_i32(&j, &ctxs[0]);
      for (int t = 1; t < P; t++) pthread_join(th[t], NULL);
      barrier_destroy(&j.bar);
      for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
      if (!ok) goto fallback; }
    if (j.lazy && vstart) {                                        /* hand the boundaries to the view */
        for (int b = 0; b < j.nbk; b++) vstart[b] = j.bstart[b];
        vstart[j.nbk] = n; *vnbk = j.nbk;
        TRACE("i32: lazy partition, %d buckets placed unsorted, leaf_i32 skipped", j.nbk);
    }
    free(j.bidx); free(j.dst); free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    return;
fallback:
    free(samp); free(j.bidx); free(j.dst); free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    qsort(a, n, sizeof(int32_t), cmp_i32);
}


/* ============ strict in-place sort (cycle-following MSD radix) ============
 * Auxiliary memory is O(256 * bytes) of stack — no n-sized buffer — so peak
 * footprint is ~1n instead of the ~2n of the buffered fast paths. This roughly
 * halves the RAM ceiling (the limit on the largest sortable n) at a measured
 * cost in speed: the buffered scatter is sequential, cycle-following is random-
 * access swaps, which the memory-latency wall punishes. Use it only when RAM,
 * not time, is the binding constraint (e.g. sorting toward 1e9 on a machine
 * that cannot hold 2n). Sequential for now; parallel in-place (sort the 256
 * top buckets across threads) is a natural, still-stack-only extension.
 *
 * Numbers are placed by an order-preserving unsigned key (int32: bias the sign
 * bit; double: the IEEE total-order transform, total_key). A one-pass OR/AND
 * over the keys skips bytes that never vary (byte-range ints collapse to 1-2
 * levels). Small buckets finish with insertion by value. */
#define IN_KEY_I32(v) ((uint32_t)(v) ^ 0x80000000u)
#define IN_KEY_F64(v) total_key(v)
#define DEFINE_INPLACE_MSD(NAME, ETYPE, KEY) \
static void NAME(ETYPE *a, size_t lo, size_t hi, const int *shifts, int si, int ns) { \
    size_t n = hi - lo; \
    if (n <= 32 || si >= ns) {                                    /* leaf: insertion by value */ \
        for (size_t i = lo + 1; i < hi; i++) { ETYPE k = a[i]; size_t j = i; while (j > lo && a[j - 1] > k) { a[j] = a[j - 1]; j--; } a[j] = k; } \
        return; \
    } \
    int shift = shifts[si]; \
    size_t cnt[256]; memset(cnt, 0, sizeof cnt); \
    for (size_t i = lo; i < hi; i++) cnt[(KEY(a[i]) >> shift) & 0xffu]++; \
    size_t bstart[257]; bstart[0] = lo; for (int b = 0; b < 256; b++) bstart[b + 1] = bstart[b] + cnt[b]; \
    size_t next[256]; for (int b = 0; b < 256; b++) next[b] = bstart[b]; \
    for (int b = 0; b < 256; b++) { \
        while (next[b] < bstart[b + 1]) {                          /* American-flag cycle placement */ \
            ETYPE v = a[next[b]]; unsigned tb = (unsigned)((KEY(v) >> shift) & 0xffu); \
            if (tb == (unsigned)b) { next[b]++; } \
            else { a[next[b]] = a[next[tb]]; a[next[tb]] = v; next[tb]++; } \
        } \
    } \
    for (int b = 0; b < 256; b++) if (bstart[b + 1] - bstart[b] > 1) NAME(a, bstart[b], bstart[b + 1], shifts, si + 1, ns); \
}
DEFINE_INPLACE_MSD(inplace_msd_i32, int32_t, IN_KEY_I32)
DEFINE_INPLACE_MSD(inplace_msd_f64, double,  IN_KEY_F64)

/* ---- public wrappers ---------------------------------------------------- */
/* Partition only: place the elements into ordered buckets and leave each bucket unsorted, then report
 * the boundaries. This is the engine's own partition with its leaf phase skipped, which is the whole
 * point -- measured at 8M uniform on 14 threads, the partition is 20.2 ms of a 54.9 ms sort and the
 * leaves are the other 34.7, so a caller who only needs a few ranks can skip about 63% of the work.
 *
 * starts[] must hold LOGOS_MAX_VIEW_BUCKETS+1 entries. Returns the bucket count, with
 * starts[0]==0 and starts[nbk]==n, so bucket b owns exactly the elements of rank
 * [starts[b], starts[b+1]). Returns 0 when the array came out fully sorted instead, which every other
 * route in the dispatcher does; the caller then has nothing left to do. a[] is permuted either way and
 * is always a valid permutation of the input. See logos_view.h for the API built on this. */
int logos_partition_f64(double *a, size_t n, int nthreads, size_t *starts, double *bmin, double *bmax) {
    if (!a || !starts || n < 2) return 0;
    int nbk = 0;
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    psort_f64(a, n, nthreads, starts, &nbk, bmin, bmax);
    return nbk;
}

/* Sort a set of already-placed buckets in place, using the engine's own leaf and one pre-sized
 * context per thread -- the same machinery hf_leaves uses, minus the region copy. This is what makes
 * materialising a view cost what an eager sort costs: the first version of the view re-entered
 * logos_sort_f64 per bucket, paying the whole dispatcher 168 times, and ran 1.09-1.47x slower than the
 * sort it was deferring. done[b] != 0 skips a bucket; each bucket is written by exactly one thread. */
typedef struct { double *a; const size_t *starts; int nbk; unsigned char *done; atomic_size_t *next;
                 size_t maxb; int seq; } sb_arg_t;
typedef struct { sb_arg_t *w; ctx_t *ctx; } sb_targ_t;
static void *sb_worker(void *p) {
    sb_targ_t *ta = p; sb_arg_t *w = ta->w; ctx_t *c = ta->ctx; size_t b;
    double *scratch = malloc((w->maxb ? w->maxb : 1) * sizeof(double));
    if (!scratch) return NULL;
    c->seq = w->seq;
    while ((b = atomic_fetch_add(w->next, 1)) < (size_t)w->nbk) {
        if (w->done[b]) continue;
        size_t s0 = w->starts[b], nb = w->starts[b + 1] - s0;
        if (nb > 1) { double *r = leaf_f64(w->a + s0, nb, scratch, c); if (r != w->a + s0) memcpy(w->a + s0, r, nb * sizeof(double)); }
        w->done[b] = 1;
    }
    free(scratch);
    return NULL;
}
/* Contexts are initialised HERE, on the calling thread, before any worker starts: ctx_init advances
 * the pool cursor g_ctx_next with a plain increment, so a worker that calls it races every other
 * worker for the same pooled buffers (hf_partition initialises its P contexts up front for the same
 * reason). A worker whose context could not be made leaves its buckets undone; the serial tail below
 * finishes them. */
void logos_sort_buckets_f64(double *a, const size_t *starts, int nbk, unsigned char *done, int nthreads) {
    if (!a || !starts || nbk <= 0 || !done) return;
    int P = nthreads > 0 ? nthreads : logos_num_cpus();
    if (P > nbk) P = nbk;
    if (P < 1) P = 1;
    size_t maxb = 1; for (int b = 0; b < nbk; b++) { size_t c = starts[b + 1] - starts[b]; if (c > maxb) maxb = c; }
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    /* Sequential-leaf policy. `seq` decides whether a bucket may use the AVX2 quicksort, which the
     * ledger measured 5-28% WORSE inside the engine's parallel leaf phase, where each of 14 threads
     * sorts a dozen buckets back to back and the vector units contend. A batch query is the other
     * regime: it forces a handful of buckets, at most one per thread, so each is genuinely a lone
     * sequential sort and should get the sequential leaf. Without this a ten-bucket batch on sawtooth
     * ran 1.4-1.6x SLOWER than ten single lookups, because it silently traded the AVX2 leaf away. */
    int todo = 0; for (int b = 0; b < nbk; b++) if (!done[b]) todo++;
    int one_each = (todo <= P) && !getenv("LOGOS_VIEW_SEQ0");   /* =1 restores the old policy, for the A/B */
    atomic_size_t next; atomic_init(&next, 0);
    sb_arg_t w = { a, starts, nbk, done, &next, maxb, (P == 1) || one_each };
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    sb_targ_t *ta = calloc((size_t)P, sizeof(sb_targ_t));
    int made = 0;
    if (th && ctxs && ta) { for (int t = 0; t < P; t++) { if (!ctx_init(&ctxs[t], maxb, 0)) break; made++; } }
    for (int t = 0; t < made; t++) { ta[t].w = &w; ta[t].ctx = &ctxs[t]; }
    for (int t = 1; t < made; t++) pthread_create(&th[t], NULL, sb_worker, &ta[t]);
    if (made > 0) sb_worker(&ta[0]);
    for (int t = 1; t < made; t++) pthread_join(th[t], NULL);
    for (int t = 0; t < made; t++) ctx_free(&ctxs[t]);
    free(th); free(ctxs); free(ta);
    /* a worker that could not get a context leaves its buckets undone; finish them here, serially */
    for (int b = 0; b < nbk; b++) if (!done[b]) { size_t s0 = starts[b], nb = starts[b + 1] - s0; if (nb > 1) logos_sort_f64(a + s0, nb, 1); done[b] = 1; }
}
void logos_sort_f64(double *a, size_t n, int nthreads) {
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;              /* one top-level sort at a time: the bundle cursor restarts */
#endif
    psort_f64(a, n, nthreads, NULL, NULL, NULL, NULL);
}
void logos_sort_i32(int32_t *a, size_t n, int nthreads) {   /* native: 4-byte elements throughout */
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    psort_i32(a, n, nthreads);
}
/* The int32 twins of logos_partition_f64 / logos_sort_buckets_f64 (contract in logos_psort.h). The
 * int32 engine still runs the OLD histogram path (do_hist_i32 / do_scatter_i32 / do_buckets_i32), so
 * its lazy mode is a flag on do_buckets_i32 rather than on hf_leaves; the bucket boundaries it reports
 * come from the histogram's exact prefix sums. */
int logos_partition_i32(int32_t *a, size_t n, int nthreads, size_t *starts, int32_t *bmin, int32_t *bmax) {
    if (!a || !starts || n < 2) return 0;
    int nbk = 0;
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    psort_i32_ex(a, n, nthreads, starts, &nbk, bmin, bmax);
    return nbk;
}
typedef struct { int32_t *a; const size_t *starts; int nbk; unsigned char *done; atomic_size_t *next; size_t maxb; } sbi_arg_t;
typedef struct { sbi_arg_t *w; ctx_t *ctx; } sbi_targ_t;
static void *sbi_worker(void *p) {
    sbi_targ_t *ta = p; sbi_arg_t *w = ta->w; ctx_t *c = ta->ctx; size_t b;
    uint32_t *buf = malloc((w->maxb ? w->maxb : 1) * sizeof(uint32_t));
    if (!buf) return NULL;
    while ((b = atomic_fetch_add(w->next, 1)) < (size_t)w->nbk) {
        if (w->done[b]) continue;
        size_t s0 = w->starts[b], nb = w->starts[b + 1] - s0;
        if (nb > 1) { if (g_simd_i32) g_simd_i32(w->a + s0, nb); else leaf_i32(w->a + s0, nb, buf, c); }
        w->done[b] = 1;
    }
    free(buf);
    return NULL;
}
/* Same shape as the f64 one, contexts made on the calling thread (see logos_sort_buckets_f64). */
void logos_sort_buckets_i32(int32_t *a, const size_t *starts, int nbk, unsigned char *done, int nthreads) {
    if (!a || !starts || nbk <= 0 || !done) return;
    int P = nthreads > 0 ? nthreads : logos_num_cpus();
    if (P > nbk) P = nbk;
    if (P < 1) P = 1;
    size_t maxb = 1; for (int b = 0; b < nbk; b++) { size_t c = starts[b + 1] - starts[b]; if (c > maxb) maxb = c; }
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    atomic_size_t next; atomic_init(&next, 0);
    sbi_arg_t w = { a, starts, nbk, done, &next, maxb };
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    sbi_targ_t *ta = calloc((size_t)P, sizeof(sbi_targ_t));
    int made = 0;
    if (th && ctxs && ta) { for (int t = 0; t < P; t++) { if (!ctx_init(&ctxs[t], maxb, 0)) break; made++; } }
    for (int t = 0; t < made; t++) { ta[t].w = &w; ta[t].ctx = &ctxs[t]; }
    for (int t = 1; t < made; t++) pthread_create(&th[t], NULL, sbi_worker, &ta[t]);
    if (made > 0) sbi_worker(&ta[0]);
    for (int t = 1; t < made; t++) pthread_join(th[t], NULL);
    for (int t = 0; t < made; t++) ctx_free(&ctxs[t]);
    free(th); free(ctxs); free(ta);
    for (int b = 0; b < nbk; b++) if (!done[b]) { size_t s0 = starts[b], nb = starts[b + 1] - s0; if (nb > 1) logos_sort_i32(a + s0, nb, 1); done[b] = 1; }
}
int logos_num_cpus(void) { long c = sysconf(_SC_NPROCESSORS_ONLN); return c < 1 ? 1 : (int)c; }

/* ---- strict in-place entries (~1n memory; slower; for RAM-bound large n) ---- */
void logos_sort_i32_inplace(int32_t *a, size_t n, int nthreads) {
    (void)nthreads;                                               /* sequential for now */
    if (n < 2) return;
    int asc = 1, desc = 1;
    for (size_t i = 1; i < n && (asc || desc); i++) { if (a[i] < a[i - 1]) asc = 0; if (a[i] > a[i - 1]) desc = 0; }
    if (asc) return;
    if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { int32_t t = a[l]; a[l] = a[r]; a[r] = t; } return; }
    uint32_t orv = 0, andv = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { uint32_t k = IN_KEY_I32(a[i]); orv |= k; andv &= k; }
    uint32_t diff = orv ^ andv; int shifts[4], ns = 0;
    for (int b = 3; b >= 0; b--) if ((diff >> (b * 8)) & 0xffu) shifts[ns++] = b * 8;
    if (ns == 0) return;                                          /* all equal */
    inplace_msd_i32(a, 0, n, shifts, 0, ns);
}
void logos_sort_f64_inplace(double *a, size_t n, int nthreads) {
    (void)nthreads;
    if (n < 2) return;
    int asc = 1, desc = 1;
    for (size_t i = 1; i < n && (asc || desc); i++) { if (a[i] < a[i - 1]) asc = 0; if (a[i] > a[i - 1]) desc = 0; }
    if (asc) return;
    if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } return; }
    uint64_t orv, andv;
    if (g_simd_orand) g_simd_orand(a, n, &orv, &andv);
    else { orv = 0; andv = ~0ULL; for (size_t i = 0; i < n; i++) { uint64_t k = IN_KEY_F64(a[i]); orv |= k; andv &= k; } }
    uint64_t diff = orv ^ andv; int shifts[8], ns = 0;
    for (int b = 7; b >= 0; b--) if ((diff >> (b * 8)) & 0xffu) shifts[ns++] = b * 8;
    if (ns == 0) return;
    inplace_msd_f64(a, 0, n, shifts, 0, ns);
}
