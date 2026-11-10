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
#ifdef __GLIBC__
#include <malloc.h>            /* malloc_trim: logos_release_buffers */
#endif
#endif
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#if defined(__x86_64__) && defined(__SSE2__) && !defined(LOGOS_NO_SIMD_SCAN)   /* -DLOGOS_NO_SIMD_SCAN: the scalar OR of sixteen, as on other targets */
/* Here, before the AVX2 region below includes immintrin.h: a header first included inside a target pragma defines its intrinsics
   for that target, and a baseline function calling them fails to compile (see prescan_blocks). */
#include <emmintrin.h>
#define LOGOS_HAVE_SSE2 1
#endif
/* What this CPU runs, cached once: 2 = AVX2 with FMA (as the AVX2 leaf asks), 1 = SSE2 (baseline x86-64), 0 = neither. Read by the
   prescan's blocks and by the run detector's, which sits far earlier in the file. */
static int g_simd_kind = -1;
static inline int logos_simd_kind(void) {
    int c = g_simd_kind;
    if (c < 0) {
#ifdef LOGOS_HAVE_SSE2
        c = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") ? 2 : 1;
#else
        c = 0;
#endif
        g_simd_kind = c;
    }
    return c;
}

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
                                                            "histogram", "combine", "scatter", "leaf sorts", "copyback", "other", "relocate probe",
                                                            "lattice map", "lattice merge", "lattice emit", "lattice teardown" };
const char *const logos_profile_path_names[LOGOS_PATH_N] = { "none", "already sorted", "counting", "int32 radix", "insertion", "run merge",
                                                             "run relocate", "momentum", "gap split", "flash", "bits coarse", "coarse",
                                                             "quicksort", "quant radix", "simd leaf" };
const char *const logos_profile_route_names[LOGOS_ROUTE_N] = { "none", "already sorted", "lowcard counting", "lattice counting", "modcard counting",
                                                               "sequential leaf", "near-sorted merge", "run relocation", "sample-sort partition", "short-reach blocks" };
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
/* sort_fn's profile pass goes vector (sf_profile_avx2) from SF_VEC_MIN_N elements, after a scalar prefix of SF_PREFIX */
#ifndef SF_VEC_MIN_N
#define SF_VEC_MIN_N 1024
#endif
#ifndef SF_PREFIX
#define SF_PREFIX 64
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
/* 4096 until 2026-09-23. Below this sort_fn has no flash sort and falls through to the scalar quicksort,
 * which has a cliff there: with the AVX2 leaf off, 2048 and 3000 elements cost 27.5 and 36.3 ns each
 * against 6.3 and 6.7 through flash. Paired A/B, both orientations, layout cancelled (make bandbench
 * BB_OLD= BB_NEW=-DFLASH_SORT_THRESHOLD=2048): 2048 reads 1.286 and 3000 reads 1.352 with the AVX2 leaf
 * on, 4.583 and 5.526 with it off (a non-AVX2 build, and every non-x86 one), and 512 / 1024 / 4096 and up
 * are 0.986-1.016 in both. 512 is the floor, not a round number: a threshold of 512 measured 0.795 at
 * n = 512 and 0.939 at 1024 with the AVX2 leaf on, in both orientations -- flash needs the elements. */
#define FLASH_SORT_THRESHOLD     2048
#endif
#ifndef AVX2_FLASH_MAX_N
/* The top of the window where sort_fn prefers flash_sort to the AVX2 quicksort. Laddered at one thread
 * (2026-09-23): flash wins 1.27x at 4096 and 1.24x at 8192, 1.05-1.06x at 16384-32768, 1.03 at 65536, and
 * LOSES 1.09x at 131072 -- which a first cut with no ceiling shipped as a regression, caught by callbench
 * and the ladder together. Ranges this large reach sort_fn through leaf_f64's skew branch, where flash is
 * still chosen below COARSE_MIN_N; above this bound the AVX2 kernel is the better of the two. */
#define AVX2_FLASH_MAX_N         65536
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
#ifndef LOWDISC_MIN_COVER
#define LOWDISC_MIN_COVER        0.5      /* ... and the share of the range that window must span (a ramp's window spans 64 / its leg) */
#endif
#ifndef LOWDISC_LONG_COVER
#define LOWDISC_LONG_COVER       0.05     /* ... except a window spanning under this share: a LONG leg, which coarse's scatter follows (measured) */
#endif
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
/* round six's routes and what they stand on: the wave sorts pieces and places them with relocation's planner and the knot sweep;
   the seam is found in the reach route's fences (and in the position-ordered sample, which the wave block also reads) */
#if defined(LOGOS_NO_RELOCATE) && !defined(LOGOS_NO_WAVE)
#define LOGOS_NO_WAVE
#endif
#if (defined(LOGOS_NO_REACH) || defined(LOGOS_NO_SODO)) && !defined(LOGOS_NO_SEAM)
#define LOGOS_NO_SEAM
#endif
#if defined(LOGOS_NO_SODO) && !defined(LOGOS_NO_WAVE)
#define LOGOS_NO_WAVE
#endif
static __attribute__((unused)) int g_reach_relax = 0;   /* the wave route's pieces: the short-reach route below its usual size and block count */
static __attribute__((unused)) int g_seamed = 0;        /* the seam route's re-entry guard */
static __attribute__((unused)) size_t g_seam_count = 0, g_reach_seam = 0;   /* g_reach_seam: a rotation the reach route found in its fences, for the dispatcher to make */
/* Thread-count policy (make scale-full, 2026-09-10): the parallel route has a fixed cost of several
 * ms (thread creation, barriers, P*12 tiny buckets), so on this box 20 threads were SLOWER than one
 * from 1e5 to ~5e5 elements. Use at most one thread per MIN_ELEMS_PER_THREAD elements; results never
 * depend on the thread count. -DLOGOS_NO_THREAD_POLICY restores "all threads for any n >= 65536". */
#ifndef MIN_ELEMS_PER_THREAD
#define MIN_ELEMS_PER_THREAD     100000
#endif
#ifndef BOOST_PER
#define BOOST_PER 25000                   /* the unstructured partition: one thread per 25k elements (0 = off). 50k measured 1.07-1.59x, 25k 1.08-1.92x */
#endif
#ifndef BOOST_MIN_N
#define BOOST_MIN_N 300000
#endif
/* LOGOS_MIN_PER_THREAD=<k>: the A/B knob for the floor (read only for parallel requests of 32768 elements and up, out of line). */
static __attribute__((noinline, unused)) size_t min_per_thread_env(void) {
    const char *e = getenv("LOGOS_MIN_PER_THREAD"); size_t v = e ? (size_t)strtoull(e, NULL, 10) : 0;
    return v >= 1000 ? v : (size_t)MIN_ELEMS_PER_THREAD;
}
/* LOGOS_BOOST_PER=<k> [LOGOS_BOOST_MIN=<n>]: the thread count the UNSTRUCTURED partition may take, one per k elements from n. */
static __attribute__((noinline, unused)) int boost_threads(size_t n, int P, int Preq) {
    const char *e = getenv("LOGOS_BOOST_PER"), *m = getenv("LOGOS_BOOST_MIN");
    size_t per = e ? (size_t)strtoull(e, NULL, 10) : (size_t)BOOST_PER, mn = m ? (size_t)strtoull(m, NULL, 10) : (size_t)BOOST_MIN_N;
    if (per < 1000 || n < mn) return P;
    size_t pb = n / per; if (pb > (size_t)Preq) pb = (size_t)Preq;
    return pb > (size_t)P ? (int)pb : P;
}
static inline int effective_threads(size_t n, int P) {
#ifdef LOGOS_NO_THREAD_POLICY
    (void)n; return P;
#else
    size_t per = MIN_ELEMS_PER_THREAD; if (P > 1 && n >= 32768) per = min_per_thread_env();
    size_t pmax = n / per; if (pmax < 1) pmax = 1;
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
    int pool_ix;                                    /* that bundle's index, or -1: tables taken lazily (ctx_coarse_ready) are stored there */
} ctx_t;

/* POOL TRIM (2026-10-08, make rssprobe). The buffer pool, the partition's regions and the context bundles keep what a call used
 * for the next one, which is what makes repeated sorts cheap (the page-fault floor, ~13%). But they keep it FOREVER: a process that
 * sorted 8M doubles once holds 8-10 bytes per element of them RESIDENT through any number of small sorts afterwards (rssprobe's
 * "small" column), until it calls logos_release_buffers(). Each pooled region now carries a decaying high-water mark of what it
 * was asked for (an eighth off per acquire, never below the current request); when the part that may be resident reaches past
 * twice that mark by more than TRIM_MIN_BYTES, the pages above the mark are handed back with MADV_DONTNEED. The block itself
 * stays, so a later large call faults them in again (contents are scratch, written before they are read; dropped pages read as
 * zeros). Same-size calls never trim; it takes about six consecutive calls at under half the size.
 * OPT-IN: LOGOS_TRIM=1 hands pages back with MADV_FREE (still mapped; the kernel takes them only under memory pressure), LOGOS_TRIM=hard
 * with MADV_DONTNEED (resident drops at once). Read with the other switches, at the top of a sort of 65536 or more. MEASURED
 * (make rssprobe, 8.4M): after one large sort and 32 small ones, 8-10 bytes per element stay resident without it and 0.35-0.83
 * with the hard form; the soft form leaves 7.6-9.6 of them LazyFree. The price is the next LARGE call after six or more small
 * ones, which faults its pages back: the hard form 1.3-2.5x on that one call (block_perm at one thread 2.48, uniform at 14 2.14),
 * the soft form 1.03-1.97 on this box, where the measurement was noisy. Same-size and alternating calls never pay. That is a loss
 * on speed, so it is not the default: a long-running process that wants the memory back without calling
 * logos_release_buffers() turns it on. */
#ifndef TRIM_MIN_BYTES
#define TRIM_MIN_BYTES ((size_t)1 << 20)
#endif
typedef struct { size_t hw, hi; unsigned ep; } trim_t;   /* bytes: the decaying mark; the top of what may be resident; the call that last noted it */
static int g_no_trim = 1, g_trim_hard = 0;   /* off unless LOGOS_TRIM is set; LOGOS_TRIM=hard: MADV_DONTNEED (resident drops at once) */
static unsigned g_trim_ep;                     /* one per top-level logos_sort_f64: a region nobody noted in a call decays at its end (trim_idle) */
static size_t g_trim_bytes;                    /* logos_twist_last(21): bytes handed back since it was last read */
static __attribute__((unused)) void trim_note(trim_t *t, void *base, size_t cap, size_t req) {
    size_t dec = t->hw - t->hw / 8;
    t->hw = req > dec ? req : dec; t->ep = g_trim_ep;
    if (req > t->hi) t->hi = req;
    if (t->hi > cap) t->hi = cap;
    if (g_no_trim || !base || t->hi <= 2 * t->hw + TRIM_MIN_BYTES) return;
    uintptr_t s = ((uintptr_t)base + t->hw + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)base + t->hi) & ~(uintptr_t)4095;
    if (e > s) {
        int r = -1;
#ifdef MADV_FREE
        if (!g_trim_hard) r = madvise((void *)s, e - s, MADV_FREE);
#endif
        if (r != 0) r = madvise((void *)s, e - s, MADV_DONTNEED);
        if (r == 0) g_trim_bytes += e - s;
    }
    t->hi = t->hw;
}
static __attribute__((unused)) void trim_reset(trim_t *t) { t->hi = 0; }   /* a fresh block: nothing in it is resident yet */

#define WC_MIN_THREADS 8   /* measured: staging loses ~8-35% sequentially, wins ~5-12% at 20 threads */
static void ctx_release(ctx_t *c) {
    free(c->aux); free(c->idx); free(c->wc); free(c->wcn);
    for (int d = 0; d < COARSE_MAXD; d++) { free(c->C[d]); free(c->S[d]); free(c->H[d]); free(c->land[d]); }
}
/* All or nothing. A context that could not be had is ALL ZEROS when this returns 0: nothing held, nothing to free, and
 * ctx_fit refuses it, so whoever holds it sorts with a routine that needs no context. Before 2026-09-20 a failure left
 * the parts that had been allocated in place (leaked when the context was a pooled bundle) and, worse, left a context
 * that looked usable: see ctx_fit. Found by failing every allocation in turn, `make faultprobe`. */
/* the fixed part of a context, taken only when it can be used (2026-09-26; make memprobe): the four coarse levels' tables
 * (counts, starts, heads, landing bytes: 197 KB) on first entry to the coarse path, and the write-combining stage (131 KB) only
 * when staging is on (WC_MIN_THREADS). Before, every context carried all 340 KB from its first allocation, P of them per parallel
 * call, and a 10,000-element sort held 50 bytes per element of which 33 were tables it never touched: below ~65k a sequential
 * leaf takes flash or the AVX2 quicksort, never coarse. -DLOGOS_EAGER_CTX restores the old allocation, for the A/B. */
static int ctx_coarse_alloc(ctx_t *c) {
    int ok = 1;
    for (int d = 0; ok && d < COARSE_MAXD; d++) {
        c->land[d] = malloc(COARSE_K);
        c->C[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        c->S[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        c->H[d] = malloc((COARSE_K + 1) * sizeof(size_t));
        if (!c->land[d] || !c->C[d] || !c->S[d] || !c->H[d]) ok = 0;
    }
    if (!ok) for (int d = 0; d < COARSE_MAXD; d++) { free(c->C[d]); free(c->S[d]); free(c->H[d]); free(c->land[d]); c->C[d] = c->S[d] = c->H[d] = NULL; c->land[d] = NULL; }
    return ok;
}
static int ctx_wc_alloc(ctx_t *c) {
    c->wc = malloc((size_t)COARSE_K * 8 * sizeof(double)); c->wcn = calloc(COARSE_K, 1);
    if (c->wc && c->wcn) return 1;
    free(c->wc); free(c->wcn); c->wc = NULL; c->wcn = NULL; return 0;
}
static int ctx_alloc(ctx_t *c, size_t cap, int wc_on) {
    memset(c, 0, sizeof *c); c->cap = cap; c->wc_on = wc_on; c->pool_ix = -1;
    int ok = 1;
    c->aux = malloc((cap + 1) * sizeof(uint32_t));
    c->idx = malloc((cap ? cap : 1) * sizeof(uint16_t));
    if (!c->aux || !c->idx) ok = 0;
#ifdef LOGOS_EAGER_CTX
    if (ok && !ctx_wc_alloc(c)) ok = 0;
    if (ok && !ctx_coarse_alloc(c)) ok = 0;
#else
    if (ok && wc_on && !ctx_wc_alloc(c)) ok = 0;
#endif
    if (!ok) { ctx_release(c); memset(c, 0, sizeof *c); c->pool_ix = -1; }
    return ok;
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
static trim_t g_ctx_trim[CTX_POOL_MAX][2];      /* aux, idx */
static int ctx_init(ctx_t *c, size_t cap, int wc_on) {
    if (g_ctx_next >= CTX_POOL_MAX) { int r = ctx_alloc(c, cap, wc_on); c->pooled = 0; return r; }
    int i = g_ctx_next++; ctx_t *b = &g_ctx_pool[i];
    if (!g_ctx_pool_used[i] || b->cap < cap) {
        if (g_ctx_pool_used[i]) ctx_release(b);
        size_t want = cap + cap / 4 + 64;
        if (!ctx_alloc(b, want, wc_on)) { g_ctx_pool_used[i] = 0; memset(c, 0, sizeof *c); return 0; }   /* the bundle is empty again and the caller's context is all zeros */
        g_ctx_pool_used[i] = 1; trim_reset(&g_ctx_trim[i][0]); trim_reset(&g_ctx_trim[i][1]);
    }
    trim_note(&g_ctx_trim[i][0], b->aux, (b->cap + 1) * sizeof(uint32_t), (cap + 1) * sizeof(uint32_t));
    trim_note(&g_ctx_trim[i][1], b->idx, b->cap * sizeof(uint16_t), cap * sizeof(uint16_t));
    if (wc_on && !b->wc && !ctx_wc_alloc(b)) { memset(c, 0, sizeof *c); c->pool_ix = -1; return 0; }   /* the bundle keeps what it has; this call gets no context */
    size_t bcap = b->cap;
    memset(c, 0, sizeof *c);
    c->aux = b->aux; c->idx = b->idx; c->wc = b->wc; c->wcn = b->wcn;
    for (int d = 0; d < COARSE_MAXD; d++) { c->land[d] = b->land[d]; c->C[d] = b->C[d]; c->S[d] = b->S[d]; c->H[d] = b->H[d]; }
    if (c->wcn) memset(c->wcn, 0, COARSE_K);
    c->cap = bcap; c->wc_on = wc_on; c->pooled = 1; c->pool_ix = i;
    return 1;
}
/* the coarse tables, on first entry to the coarse path. Into the bundle when the context is pooled, so the next sort has them;
   0 = no memory, and the caller sorts that range with a routine that needs no tables (faultprobe fails this site) */
static int ctx_coarse_ready(ctx_t *c) {
    if (c->C[0]) return 1;
    ctx_t *b = c->pool_ix >= 0 ? &g_ctx_pool[c->pool_ix] : c;
    if (b != c && b->C[0]) { for (int d = 0; d < COARSE_MAXD; d++) { c->land[d] = b->land[d]; c->C[d] = b->C[d]; c->S[d] = b->S[d]; c->H[d] = b->H[d]; } return 1; }
    if (!ctx_coarse_alloc(b)) return 0;
    if (b != c) for (int d = 0; d < COARSE_MAXD; d++) { c->land[d] = b->land[d]; c->C[d] = b->C[d]; c->S[d] = b->S[d]; c->H[d] = b->H[d]; }
    return 1;
}
static void ctx_free(ctx_t *c) { if (!c->pooled) ctx_release(c); }
#else
static int ctx_init(ctx_t *c, size_t cap, int wc_on) { int r = ctx_alloc(c, cap, wc_on); c->pooled = 0; return r; }
static void ctx_free(ctx_t *c) { ctx_release(c); }
static int ctx_coarse_ready(ctx_t *c) { return c->C[0] ? 1 : ctx_coarse_alloc(c); }   /* no pool: into the context itself, freed with it */
#endif
/* ---- a context that fits the bucket at hand (2026-09-20; measured by make memprobe) -------------------------
 * Every parallel leaf loop used to size ALL P contexts by the LARGEST bucket. With balanced buckets that is nothing
 * (P * n/nbk * 7.5 bytes, under a byte per element). With one bucket no splitter can divide, a value that holds most
 * of the array, it is P * n: 104 bytes per element at 10M and 14 threads on 90% one value, a gigabyte of scratch for
 * an 80 MB array. That is the O(Pn) an outside review warned about, and nothing here had ever weighed a sort.
 * But a thread only needs room for the bucket in its hands, and the buckets in hand at one moment are disjoint, so they
 * sum to at most n. Contexts are therefore sized for a TYPICAL bucket (twice the mean), and a bucket above that BORROWS
 * room for as long as it is being sorted: the shared jumbo block if it is free (persistent, like the pools, so a
 * dominant value does not page-fault tens of megabytes on every call), otherwise a private temporary. If neither can be
 * had, ctx_fit says so and the caller sorts that one bucket with a routine that needs no context.
 * Only aux and idx scale with the bucket; everything else in a context is fixed size. */
static pthread_mutex_t g_jumbo_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *g_jumbo_aux = NULL; static uint16_t *g_jumbo_idx = NULL; static size_t g_jumbo_cap = 0;
typedef struct { uint32_t *aux; uint16_t *idx; size_t cap; int kind; } ctx_fit_t;   /* kind: 0 nothing borrowed, 1 the jumbo, 2 a temporary */
static inline size_t ctx_typical(size_t maxb, size_t n, int nbk) { size_t t = 2 * (n / (size_t)(nbk > 0 ? nbk : 1)) + 4096; return maxb < t ? maxb : t; }
static int ctx_fit(ctx_t *c, size_t nb, ctx_fit_t *f) {
    f->kind = 0;
    if (!c->aux) return 0;                          /* a context that could not be allocated is all zeros: no tables to lend room to */
    if (nb <= c->cap) return 1;
    f->aux = c->aux; f->idx = c->idx; f->cap = c->cap;
    if (pthread_mutex_trylock(&g_jumbo_m) == 0) {
        if (g_jumbo_cap < nb) {
            size_t want = nb + nb / 4 + 64;
            uint32_t *na = malloc((want + 1) * sizeof(uint32_t)); uint16_t *ni = malloc(want * sizeof(uint16_t));
            if (na && ni) { free(g_jumbo_aux); free(g_jumbo_idx); g_jumbo_aux = na; g_jumbo_idx = ni; g_jumbo_cap = want; }
            else { free(na); free(ni); }
        }
        if (g_jumbo_cap >= nb) { c->aux = g_jumbo_aux; c->idx = g_jumbo_idx; c->cap = g_jumbo_cap; f->kind = 1; return 1; }
        pthread_mutex_unlock(&g_jumbo_m);
    }
    uint32_t *ta = malloc((nb + 1) * sizeof(uint32_t)); uint16_t *ti = malloc((nb ? nb : 1) * sizeof(uint16_t));
    if (!ta || !ti) { free(ta); free(ti); return 0; }
    c->aux = ta; c->idx = ti; c->cap = nb; f->kind = 2;
    return 1;
}
static void ctx_unfit(ctx_t *c, ctx_fit_t *f) {
    if (f->kind == 0) return;
    if (f->kind == 2) { free(c->aux); free(c->idx); }
    c->aux = f->aux; c->idx = f->idx; c->cap = f->cap;
    if (f->kind == 1) pthread_mutex_unlock(&g_jumbo_m);
}
static void cnti_release_all(void);      /* the int32 counting route's table pool; defined with it, far below */
/* everything the context side keeps between calls; logos_release_buffers calls it */
static void ctx_release_all(void) {
#ifndef LOGOS_NO_CTX_POOL
    for (int i = 0; i < CTX_POOL_MAX; i++) if (g_ctx_pool_used[i]) { ctx_release(&g_ctx_pool[i]); memset(&g_ctx_pool[i], 0, sizeof g_ctx_pool[i]); g_ctx_pool_used[i] = 0; }
    g_ctx_next = 0;
#endif
    pthread_mutex_lock(&g_jumbo_m);
    free(g_jumbo_aux); free(g_jumbo_idx); g_jumbo_aux = NULL; g_jumbo_idx = NULL; g_jumbo_cap = 0;
    pthread_mutex_unlock(&g_jumbo_m);
}


static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int cmp_i32(const void *a, const void *b) { int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }
static int g_no_dense_probe = 0;      /* read once per sort on the calling thread (leaf_env), like the knot route's switches */
static int g_no_fastpre = 0;          /* LOGOS_NO_FASTPRESCAN=1: the dispatcher's prescan runs its scalar line; read from 65,536 up */
static int g_no_frontfuse = 0;        /* LOGOS_NO_FRONTFUSE=1: the leaf's profile passes as they were (see sf_profile_avx2); read from 65,536 up */
static int g_no_partfuse = 0;         /* LOGOS_NO_PARTFUSE=1: the kernel scans before it partitions (see av_qsort_fused); read from 65,536 up */
static int log2i(size_t n) { int l = 0; while (n >>= 1) l++; return l; }
static size_t sample_sort(double *s, size_t m);   /* defined with the samplers; used by the histogram-free leaf's spills above it */

/* ---- SMALLNET BEGIN (generated by gen_small_net.py -- do not edit by hand) ----
 * Branchless sorting networks for 4 to 16 elements, Batcher odd-even mergesort, straight-line.
 * NET_CE moves elements (one predicate, two selects) instead of taking a min and a max, so a NaN
 * leaves its pair alone and the output stays a permutation of the input's bit patterns.
 * Each network is proved by the 0-1 principle in the generator before it is written here. */
#define NET_CE(i, j) do { double x_ = a[i], y_ = a[j]; int c_ = (y_ < x_); \
                          a[i] = c_ ? y_ : x_; a[j] = c_ ? x_ : y_; } while (0)
/* noinline on purpose: inlined into the dispatcher this would add spill slots to a frame that is
 * stack-probed on EVERY call, which has cost this project a round before (see CLAUDE.md). */
__attribute__((noinline)) static void net_sort_small(double *a, long n) {
    switch (n) {
    case 4: NET_CE(0,1); NET_CE(2,3); NET_CE(0,2); NET_CE(1,3); NET_CE(1,2);
            break;
    case 5: NET_CE(0,1); NET_CE(2,3); NET_CE(0,2); NET_CE(1,3); NET_CE(1,2); NET_CE(0,4); NET_CE(2,4); NET_CE(1,2);
            NET_CE(3,4);
            break;
    case 6: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(0,2); NET_CE(1,3); NET_CE(1,2); NET_CE(0,4); NET_CE(1,5);
            NET_CE(2,4); NET_CE(3,5); NET_CE(1,2); NET_CE(3,4);
            break;
    case 7: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(0,2); NET_CE(1,3); NET_CE(4,6); NET_CE(1,2); NET_CE(5,6);
            NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(2,4); NET_CE(3,5); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6);
            break;
    case 8: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(0,2); NET_CE(1,3); NET_CE(4,6); NET_CE(5,7);
            NET_CE(1,2); NET_CE(5,6); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(2,4); NET_CE(3,5);
            NET_CE(1,2); NET_CE(3,4); NET_CE(5,6);
            break;
    case 9: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(0,2); NET_CE(1,3); NET_CE(4,6); NET_CE(5,7);
            NET_CE(1,2); NET_CE(5,6); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(2,4); NET_CE(3,5);
            NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(0,8); NET_CE(4,8); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8);
            NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8);
            break;
    case 10: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(0,2); NET_CE(1,3); NET_CE(4,6);
            NET_CE(5,7); NET_CE(1,2); NET_CE(5,6); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(2,4);
            NET_CE(3,5); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(0,8); NET_CE(1,9); NET_CE(4,8); NET_CE(5,9);
            NET_CE(2,4); NET_CE(3,5); NET_CE(6,8); NET_CE(7,9); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8);
            break;
    case 11: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(0,2); NET_CE(1,3); NET_CE(4,6);
            NET_CE(5,7); NET_CE(8,10); NET_CE(1,2); NET_CE(5,6); NET_CE(9,10); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6);
            NET_CE(3,7); NET_CE(2,4); NET_CE(3,5); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(9,10); NET_CE(0,8);
            NET_CE(1,9); NET_CE(2,10); NET_CE(4,8); NET_CE(5,9); NET_CE(6,10); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8);
            NET_CE(7,9); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8); NET_CE(9,10);
            break;
    case 12: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(10,11); NET_CE(0,2); NET_CE(1,3);
            NET_CE(4,6); NET_CE(5,7); NET_CE(8,10); NET_CE(9,11); NET_CE(1,2); NET_CE(5,6); NET_CE(9,10); NET_CE(0,4);
            NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(2,4); NET_CE(3,5); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6);
            NET_CE(9,10); NET_CE(0,8); NET_CE(1,9); NET_CE(2,10); NET_CE(3,11); NET_CE(4,8); NET_CE(5,9); NET_CE(6,10);
            NET_CE(7,11); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8); NET_CE(7,9); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6);
            NET_CE(7,8); NET_CE(9,10);
            break;
    case 13: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(10,11); NET_CE(0,2); NET_CE(1,3);
            NET_CE(4,6); NET_CE(5,7); NET_CE(8,10); NET_CE(9,11); NET_CE(1,2); NET_CE(5,6); NET_CE(9,10); NET_CE(0,4);
            NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(8,12); NET_CE(2,4); NET_CE(3,5); NET_CE(10,12); NET_CE(1,2);
            NET_CE(3,4); NET_CE(5,6); NET_CE(9,10); NET_CE(11,12); NET_CE(0,8); NET_CE(1,9); NET_CE(2,10); NET_CE(3,11);
            NET_CE(4,12); NET_CE(4,8); NET_CE(5,9); NET_CE(6,10); NET_CE(7,11); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8);
            NET_CE(7,9); NET_CE(10,12); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8); NET_CE(9,10); NET_CE(11,12);
            break;
    case 14: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(10,11); NET_CE(12,13); NET_CE(0,2);
            NET_CE(1,3); NET_CE(4,6); NET_CE(5,7); NET_CE(8,10); NET_CE(9,11); NET_CE(1,2); NET_CE(5,6); NET_CE(9,10);
            NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(8,12); NET_CE(9,13); NET_CE(2,4); NET_CE(3,5);
            NET_CE(10,12); NET_CE(11,13); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(9,10); NET_CE(11,12); NET_CE(0,8);
            NET_CE(1,9); NET_CE(2,10); NET_CE(3,11); NET_CE(4,12); NET_CE(5,13); NET_CE(4,8); NET_CE(5,9); NET_CE(6,10);
            NET_CE(7,11); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8); NET_CE(7,9); NET_CE(10,12); NET_CE(11,13); NET_CE(1,2);
            NET_CE(3,4); NET_CE(5,6); NET_CE(7,8); NET_CE(9,10); NET_CE(11,12);
            break;
    case 15: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(10,11); NET_CE(12,13); NET_CE(0,2);
            NET_CE(1,3); NET_CE(4,6); NET_CE(5,7); NET_CE(8,10); NET_CE(9,11); NET_CE(12,14); NET_CE(1,2); NET_CE(5,6);
            NET_CE(9,10); NET_CE(13,14); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7); NET_CE(8,12); NET_CE(9,13);
            NET_CE(10,14); NET_CE(2,4); NET_CE(3,5); NET_CE(10,12); NET_CE(11,13); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6);
            NET_CE(9,10); NET_CE(11,12); NET_CE(13,14); NET_CE(0,8); NET_CE(1,9); NET_CE(2,10); NET_CE(3,11); NET_CE(4,12);
            NET_CE(5,13); NET_CE(6,14); NET_CE(4,8); NET_CE(5,9); NET_CE(6,10); NET_CE(7,11); NET_CE(2,4); NET_CE(3,5);
            NET_CE(6,8); NET_CE(7,9); NET_CE(10,12); NET_CE(11,13); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8);
            NET_CE(9,10); NET_CE(11,12); NET_CE(13,14);
            break;
    case 16: NET_CE(0,1); NET_CE(2,3); NET_CE(4,5); NET_CE(6,7); NET_CE(8,9); NET_CE(10,11); NET_CE(12,13); NET_CE(14,15);
            NET_CE(0,2); NET_CE(1,3); NET_CE(4,6); NET_CE(5,7); NET_CE(8,10); NET_CE(9,11); NET_CE(12,14); NET_CE(13,15);
            NET_CE(1,2); NET_CE(5,6); NET_CE(9,10); NET_CE(13,14); NET_CE(0,4); NET_CE(1,5); NET_CE(2,6); NET_CE(3,7);
            NET_CE(8,12); NET_CE(9,13); NET_CE(10,14); NET_CE(11,15); NET_CE(2,4); NET_CE(3,5); NET_CE(10,12);
            NET_CE(11,13); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(9,10); NET_CE(11,12); NET_CE(13,14); NET_CE(0,8);
            NET_CE(1,9); NET_CE(2,10); NET_CE(3,11); NET_CE(4,12); NET_CE(5,13); NET_CE(6,14); NET_CE(7,15); NET_CE(4,8);
            NET_CE(5,9); NET_CE(6,10); NET_CE(7,11); NET_CE(2,4); NET_CE(3,5); NET_CE(6,8); NET_CE(7,9); NET_CE(10,12);
            NET_CE(11,13); NET_CE(1,2); NET_CE(3,4); NET_CE(5,6); NET_CE(7,8); NET_CE(9,10); NET_CE(11,12); NET_CE(13,14);
            break;
    default: break;   /* the caller checks the range; n outside it is a no-op, not a wrong sort */
    }
}
/* comparators per size: 4:5, 5:9, 6:12, 7:16, 8:19, 9:28, 10:32, 11:38, 12:42, 13:48, 14:53, 15:59, 16:63 */
/* ---- SMALLNET END ---- */
static int g_no_smallnet = 0;   /* LOGOS_NO_SMALLNET=1: short ranges go back to insertion sort, for the A/B */
/* Read once at load, never on the sort path: this switch is live at n = 4, where two getenv calls would be
 * several times the whole sort (the knot route's first cut cost a sorted triple 6 -> 42 ns exactly that way). */
__attribute__((constructor)) static void smallnet_env(void) { g_no_smallnet = getenv("LOGOS_NO_SMALLNET") != NULL; }

/* LOGOS_AVX2_OVER_FLASH=1: put the AVX2 leaf back IN FRONT of the flash/coarse decision in sort_fn, which is
 * where it sat until 2026-09-23. See the comment on that gate for what the order is worth and why. */
/* -DLOGOS_AVX2_OVER_FLASH_FORCE bakes it in, which is what lets ONE binary hold both orders: the two engine
 * copies of a paired A/B have their own statics but read the same environment. */
#ifdef LOGOS_AVX2_OVER_FLASH_FORCE
static int g_avx2_over_flash = 1;
#else
static int g_avx2_over_flash = 0;
#endif
/* THE WINDOW BY LOCAL ORDER (2026-10-20, make bandbatch). Between FLASH_SORT_THRESHOLD and AVX2_FLASH_MAX_N flash used to take every
 * range, on bandbench's verdict -- measured by sorting ONE input 120-400 times, which lets a predictor learn flash's data-dependent
 * branches. On batches of DISTINCT inputs (both copy orientations) the AVX2 quicksort wins every unordered shape across the window:
 * uniform 1.15-1.32x, normal 1.33-1.53x, bimodal 1.7-1.9x, clustered 2.6-15x (dense clusters fall into a few flash classes and the
 * insertion pass goes quadratic inside them), heavy_tail 1.01-1.16x; flash keeps what is ordered LOCALLY: 64 shuffled blocks
 * 1.33-1.50x, four interleaved streams 1.47-1.70x; near-sorted, prefix and kinked ranges tie. So the window is decided by a probe of
 * 128 adjacent pairs: fewer than a third descending (blocks show one in 64, four streams one in four, random data one in two) keeps
 * flash; otherwise the AVX2 gate takes the range. -DLOGOS_BAND_FLASH / LOGOS_BAND_FLASH=1: flash for the whole window, as before. */
#ifdef LOGOS_BAND_FLASH
static int g_band_flash = 1;
#else
static int g_band_flash = 0;
#endif
static atomic_size_t g_lowdisc_fires;     /* logos_twist_last(30): leaf ranges the low-discrepancy probe sent past the AVX2 kernel */
static atomic_size_t g_sf_windows;        /* logos_twist_last(31): profiles the founder windows settled where the prefix could not */
__attribute__((constructor)) static void avx2flash_env(void) { if (getenv("LOGOS_AVX2_OVER_FLASH")) { g_avx2_over_flash = 1; } if (getenv("LOGOS_BAND_FLASH")) g_band_flash = 1; }
static inline int band_local_order(const double *a, size_t n) {   /* n >= FLASH_SORT_THRESHOLD: four windows of 32 adjacent pairs */
    size_t d = 0;
    for (int w = 0; w < 4; w++) { size_t lo = 1 + (n - 34) * (size_t)w / 4; for (size_t i = lo; i < lo + 32; i++) d += (size_t)(a[i] < a[i - 1]); }
    return d * 3 < 128;
}

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
    /* the base case: a branchless network for 4 to 16 elements, insertion sort otherwise. Insertion sort's
     * compare here mispredicts about half the time on unstructured data, which is what the leaf sorts. */
    { long len = hi - lo + 1;
      if (len >= 4 && len <= 16 && !g_no_smallnet) net_sort_small(a + lo, len); else insertion_sort(a, lo, hi); }
}

/* ---- integer paths ------------------------------------------------------- */
/* LSD radix for int32-valued doubles: bias to unsigned, fused histogram +
 * OR/AND constant-byte detection, skip constant bytes, last pass un-biases
 * straight into a[]. src = ctx aux, dst = buf reinterpreted as uint32. */
/* views of the input's storage as 4-byte keys (3bp) and of doubles under them; outside the LOGOS_INT_NARROW branch, because the
   float32 radix uses them whichever radix is built (-DLOGOS_INT_NARROW did not build from 3bq to 2026-10-31) */
typedef uint32_t __attribute__((may_alias)) u32a_t;
typedef double __attribute__((may_alias)) f64a_t;
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
/* ONE SCRATCH ARRAY (2026-10-22, the memory round; LOGOS_RADIX_TWOBUF=1 from 4,096 elements up: the context's aux as the key array,
   as before). The keys are narrowed IN PLACE into a's own storage, read as uint32: writing key i overlaps the double a[i / 2], which
   was read already, so a forward pass is safe (the views are may_alias, so strict aliasing cannot reorder them). The passes then
   ping-pong between a's storage and half of buf, and the LAST pass must read the buffer that is not a, because it writes doubles
   into a: with an odd number of earlier passes that holds as it stands; with an even number the keys are copied to buf first.
   rssprobe: int_wide at one thread touched aux (4 bytes per element) AND buf; now buf's first half only. (u32a_t and f64a_t are
   declared above the LOGOS_INT_NARROW branch.) */
static void lsd_radix_int32(double *a, size_t n, double *buf, ctx_t *c) {
    int twobuf = n >= 4096 && getenv("LOGOS_RADIX_TWOBUF");
    u32a_t *src = twobuf ? (u32a_t *)c->aux : (u32a_t *)a, *dst = (u32a_t *)buf;
    const f64a_t *av = (const f64a_t *)a;
    uint32_t *c0 = c->hist[0], *c1 = c->hist[1], *c2 = c->hist[2], *c3 = c->hist[3];
    memset(c0, 0, 257 * 4); memset(c1, 0, 257 * 4); memset(c2, 0, 257 * 4); memset(c3, 0, 257 * 4);
    uint32_t or_a = 0, and_a = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(int32_t)av[i] ^ 0x80000000u; src[i] = v; or_a |= v; and_a &= v;
        c0[(v & 0xFF) + 1]++; c1[((v >> 8) & 0xFF) + 1]++; c2[((v >> 16) & 0xFF) + 1]++; c3[((v >> 24) & 0xFF) + 1]++;
    }
    uint32_t diff = or_a ^ and_a;
    int a0 = (diff & 0xFF) != 0, a1 = (diff & 0xFF00) != 0, a2 = (diff & 0xFF0000) != 0, a3 = (diff & 0xFF000000u) != 0;
    int last = a3 ? 3 : a2 ? 2 : a1 ? 1 : a0 ? 0 : -1;
    if (last < 0) {                                                    /* all equal: widen back, from the END (the keys are in a's storage) */
        for (size_t i = n; i-- > 0;) { uint32_t v = src[i]; a[i] = (double)(int32_t)(v ^ 0x80000000u); }
        return;
    }
    if (!twobuf && ((a0 + a1 + a2 + a3 - 1) & 1) == 0) { memcpy(dst, src, n * sizeof(uint32_t)); u32a_t *t = src; src = dst; dst = t; }
    u32a_t *from = src, *to = dst, *tmp;
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
 * int32-valued buckets here instead of the LSD radix. Measured again 2026-10-08
 * as a runtime default at one thread (frontprobe ab): int_wide 0.83 at 8M and
 * 0.93-0.94 from 65k to 1M -- the narrowed radix is the faster of the two, so
 * it stays opt-in. Declines (returns 0)
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
/* THE NETWORKS READ AND WRITE THE RANGE DIRECTLY (2026-09-29, make avkernbench): a full vector is a plain load, the one
 * partial vector a masked load blended with +inf, the vectors past the end are +inf; stores mirror it. It used to stage
 * every call through a buffer: a variable-length memcpy in, a scalar loop padding +inf, a variable-length memcpy out --
 * two libc calls per base case, which the kernel probe put at the head of a 1.3x gap to x86-simd-sort (whose sort_n
 * loads the same way as this). -DAV_NET_STAGED restores the buffer, for the A/B. Every index is a constant after
 * unrolling, so each test below folds to one compare against n. */
#ifdef AV_NET_STAGED
#define AV_LD(k) _mm256_load_pd(tmp + (k))
#define AV_ST(k, v) _mm256_store_pd(tmp + (k), (v))
#else
static inline __attribute__((always_inline)) __m256d av_ld(const double *a, size_t n, size_t k) {
    if (k + 4 <= n) return _mm256_loadu_pd(a + k);
    if (k >= n) return _mm256_set1_pd(INFINITY);
    __m256i m = _mm256_load_si256((const __m256i *)av_kmask[n - k]);
    return _mm256_blendv_pd(_mm256_set1_pd(INFINITY), _mm256_maskload_pd(a + k, m), _mm256_castsi256_pd(m));
}
static inline __attribute__((always_inline)) void av_st(double *a, size_t n, size_t k, __m256d v) {
    if (k + 4 <= n) _mm256_storeu_pd(a + k, v);
    else if (k < n) _mm256_maskstore_pd(a + k, _mm256_load_si256((const __m256i *)av_kmask[n - k]), v);
}
#define AV_LD(k) av_ld(a, n, (k))
#define AV_ST(k, v) av_st(a, n, (k), (v))
#endif
static void av_sortnet32(double *a, size_t n) {          /* n <= 32, no NaN; padded with +inf */
    __m256d v0, v1, v2, v3, v4, v5, v6, v7, t;
#ifdef AV_NET_STAGED
    double tmp[32] __attribute__((aligned(32)));
    memcpy(tmp, a, n * sizeof(double)); for (size_t i = n; i < 32; i++) tmp[i] = INFINITY;
#endif
    v0 = AV_LD(0); v1 = AV_LD(4); v2 = AV_LD(8); v3 = AV_LD(12); v4 = AV_LD(16); v5 = AV_LD(20); v6 = AV_LD(24); v7 = AV_LD(28);
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
    AV_ST(0, v0); AV_ST(4, v1); AV_ST(8, v2); AV_ST(12, v3); AV_ST(16, v4); AV_ST(20, v5); AV_ST(24, v6); AV_ST(28, v7);
#ifdef AV_NET_STAGED
    memcpy(a, tmp, n * sizeof(double));
#endif
}
static void av_sortnet64(double *a, size_t n) {          /* n <= 64, no NaN; padded with +inf */
    __m256d v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, t;
#ifdef AV_NET_STAGED
    double tmp[64] __attribute__((aligned(32)));
    memcpy(tmp, a, n * sizeof(double)); for (size_t i = n; i < 64; i++) tmp[i] = INFINITY;
#endif
    v0 = AV_LD(0); v1 = AV_LD(4); v2 = AV_LD(8); v3 = AV_LD(12); v4 = AV_LD(16); v5 = AV_LD(20); v6 = AV_LD(24); v7 = AV_LD(28); v8 = AV_LD(32); v9 = AV_LD(36); v10 = AV_LD(40); v11 = AV_LD(44); v12 = AV_LD(48); v13 = AV_LD(52); v14 = AV_LD(56); v15 = AV_LD(60);
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
    AV_ST(0, v0); AV_ST(4, v1); AV_ST(8, v2); AV_ST(12, v3); AV_ST(16, v4); AV_ST(20, v5); AV_ST(24, v6); AV_ST(28, v7); AV_ST(32, v8); AV_ST(36, v9); AV_ST(40, v10); AV_ST(44, v11); AV_ST(48, v12); AV_ST(52, v13); AV_ST(56, v14); AV_ST(60, v15);
#ifdef AV_NET_STAGED
    memcpy(a, tmp, n * sizeof(double));
#endif
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
#ifdef AV_PREFETCH   /* the library's one-block-ahead prefetch in the unrolled loop, for the kernel A/B (make avkernbench) */
#define AV_PF(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#else
#define AV_PF(p) ((void)0)
#endif
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
#define AV_PART_UNROLLED(U) do {         __m256d vl[U], vr[U];         for (int i = 0; i < (U); i++) { vl[i] = _mm256_loadu_pd(a + lo + 4 * i); vr[i] = _mm256_loadu_pd(a + hi - 4 * (U) + 4 * i); }         size_t left = lo + 4 * (U), right = hi - 4 * (U);         while (right - left >= 4 * (U)) {             __m256d v[U];             if (r - right < left - l) { right -= 4 * (U); for (int i = 0; i < (U); i++) { v[i] = _mm256_loadu_pd(a + right + 4 * i); AV_PF(a + right + 4 * i - 4 * (U)); } }             else                      { for (int i = 0; i < (U); i++) { v[i] = _mm256_loadu_pd(a + left + 4 * i); AV_PF(a + left + 4 * i + 4 * (U)); } left += 4 * (U); }             for (int i = 0; i < (U); i++) av_part_vec(a, &l, &r, v[i], piv, gt, &mn, &mx);         }         while (right > left) {                                /* remainder, one vector at a time (windows stay >= 4) */             __m256d v;             if (r - right < left - l) { right -= 4; v = _mm256_loadu_pd(a + right); }             else                      { v = _mm256_loadu_pd(a + left); left += 4; }             av_part_vec(a, &l, &r, v, piv, gt, &mn, &mx);         }         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vl[i], piv, gt, &mn, &mx);         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vr[i], piv, gt, &mn, &mx); } while (0)
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
/* THE LIBRARY'S PIVOT, VECTOR-SAMPLED (2026-09-29, make avkernbench): five vectors from spread positions, a 9-comparator
 * network ACROSS them leaves each lane's median of 5 in the middle vector, and one in-register sort of that vector gives
 * the middle of the four lane medians. 20 samples for about a dozen vector instructions. Below 512 elements the pivot was a
 * median of 3, which cost ~1.2-1.7 partition levels over ideal; 15 samples by insertion sort bought the levels back and lost
 * 1.2-1.5x in cost (the ladder). AV_VPIVOT: 0 = off, 1 = below AV_PIV_SMALL only, 2 = every range. */
#ifndef AV_VPIVOT
#define AV_VPIVOT 2    /* kernel vs the library 1.07-1.12, was 1.18-1.27; -DAV_VPIVOT=0 restores the old pivots */
#endif
#define AV_MM(x, y) do { __m256d lo_ = _mm256_min_pd((x), (y)); (y) = _mm256_max_pd((x), (y)); (x) = lo_; } while (0)
static inline __attribute__((always_inline)) double av_pivot_vec(const double *a, size_t n) {
    size_t d = (n - 4) / 5;
    __m256d v0 = _mm256_loadu_pd(a), v1 = _mm256_loadu_pd(a + d), v2 = _mm256_loadu_pd(a + 2 * d),
            v3 = _mm256_loadu_pd(a + 3 * d), v4 = _mm256_loadu_pd(a + 4 * d);
    AV_MM(v0, v3); AV_MM(v1, v4); AV_MM(v0, v2); AV_MM(v1, v3); AV_MM(v0, v1);
    AV_MM(v2, v4); AV_MM(v1, v2); AV_MM(v3, v4); AV_MM(v2, v3);
    double t[4]; _mm256_storeu_pd(t, av_sort1(v2)); return t[2];
}
#undef AV_MM
static double av_pivot(const double *a, size_t n) {
    if (AV_VPIVOT == 2 || (AV_VPIVOT == 1 && n < AV_PIV_SMALL)) return av_pivot_vec(a, n);   /* n > 64 always here */
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
    /* Strided on purpose. Weyl positions were tried here (2026-09-22, the suspicion that 63 strides alias with a 32-cycle wave):
       1.00 on every row, sine and guards, two runs of 40 pairs. The wave's cost in this quicksort is not its pivot. */
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
/* the base case. AV_BASE_SCALAR (0 = off) sends ranges of at most that many elements to the branchless scalar networks
   (net_sort_small, 4..16) instead of padding them into the 8-vector network: the kernel probe put our network at n = 8
   and 16 at 3.5x and 2.1x the library's, which halves its network to fit. For the ladder in make avkernbench. */
#ifndef AV_BASE_SCALAR
#define AV_BASE_SCALAR 0
#endif
static inline __attribute__((always_inline)) void av_base(double *a, size_t n) {
    if (AV_BASE_SCALAR && n <= AV_BASE_SCALAR) { if (n >= 4) net_sort_small(a, (long)n); else if (n > 1) insertion_sort(a, 0, (long)n - 1); return; }
    if (n > 32) av_sortnet64(a, n); else if (n > 1) av_sortnet32(a, n);
}
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
    av_base(a, n);   /* a 128 network measured 3.0 vs 1.9 ns/elem: one more level + 64 is cheaper */
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
/* THE LEAF'S PROFILE PASS, VECTORIZED, AND ITS VERDICT HANDED TO THE KERNEL (2026-10-05, make frontprobe). sort_fn read every
 * element once in a scalar loop for min / max / all-integer / ascending / descending, and the AVX2 kernel then read every
 * element AGAIN for NaN and -0.0. Priced against x86-simd-sort with one scan in front, logos's profile phase cost 1.12-1.14x
 * on uniform data at one thread. The integer and order flags are settled by a scalar prefix on almost any unsorted,
 * non-integer range (one non-integer clears all_int, one descent and one ascent clear asc and desc); this pass then takes
 * min and max four lanes at a time and, with the one compare per vector av_scan_nan_negzero uses, whether any NaN or zero is
 * present. sc = 0: neither (the kernel may skip its scan: a property of the multiset, so any permutation in between keeps
 * it true); -1: something flagged (the kernel scans and tells them apart). min_pd(v, m) returns m when v is a NaN, as the
 * scalar `v < mn` ignores it; with a NaN in mn0 every lane stays NaN, as the scalar loop's did. Results come back BY VALUE:
 * passing &mn, &mx made sort_fn's own scalar loop work on address-taken locals, and integer data, which keeps that loop,
 * read 0.966 against the engine before this change (make frontledger, layout cancelled). */
typedef struct { double mn, mx; int sc; } sf_prof_t;
static __attribute__((noinline)) sf_prof_t sf_profile_avx2(const double *a, size_t n, double mn0, double mx0) {
    __m256d m0 = _mm256_set1_pd(mn0), m1 = m0, m2 = m0, m3 = m0, x0 = _mm256_set1_pd(mx0), x1 = x0, x2 = x0, x3 = x0;
    __m256d acc = _mm256_setzero_pd(), zero = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256d v0 = _mm256_loadu_pd(a + i), v1 = _mm256_loadu_pd(a + i + 4), v2 = _mm256_loadu_pd(a + i + 8), v3 = _mm256_loadu_pd(a + i + 12);
        m0 = _mm256_min_pd(v0, m0); m1 = _mm256_min_pd(v1, m1); m2 = _mm256_min_pd(v2, m2); m3 = _mm256_min_pd(v3, m3);
        x0 = _mm256_max_pd(v0, x0); x1 = _mm256_max_pd(v1, x1); x2 = _mm256_max_pd(v2, x2); x3 = _mm256_max_pd(v3, x3);
        acc = _mm256_or_pd(acc, _mm256_or_pd(_mm256_or_pd(_mm256_cmp_pd(v0, zero, _CMP_EQ_UQ), _mm256_cmp_pd(v1, zero, _CMP_EQ_UQ)),
                                             _mm256_or_pd(_mm256_cmp_pd(v2, zero, _CMP_EQ_UQ), _mm256_cmp_pd(v3, zero, _CMP_EQ_UQ))));
    }
    m0 = _mm256_min_pd(_mm256_min_pd(m0, m1), _mm256_min_pd(m2, m3)); x0 = _mm256_max_pd(_mm256_max_pd(x0, x1), _mm256_max_pd(x2, x3));
    int flagged = _mm256_movemask_pd(acc) != 0;
    double mn = mn0, mx = mx0, t[4];
    _mm256_storeu_pd(t, m0); for (int q = 0; q < 4; q++) if (t[q] < mn) mn = t[q];
    _mm256_storeu_pd(t, x0); for (int q = 0; q < 4; q++) if (t[q] > mx) mx = t[q];
    for (; i < n; i++) { double v = a[i]; if (v < mn) mn = v; else if (v > mx) mx = v; flagged |= (v != v) | (v == 0.0); }
    sf_prof_t r = { mn, mx, flagged ? -1 : 0 };
    return r;
}
/* 1 = sorted in place; 0 = declined (NaN present), caller keeps its scalar path.
 * The networks are min/max, and _mm256_min_pd / _mm256_max_pd return the SAME operand when -0.0 meets +0.0, so a
 * compare-exchange of the two wrote one sign twice and lost the other: the output was not a permutation (harness signed
 * zeros, zerodiag). When a -0.0 is present every zero is made +0.0, the sort runs, and the -0.0 are written back at the
 * start of the zero run, which is contiguous in sorted output and is where -0.0 belongs in total order. A decline happens
 * before anything is written, EXCEPT on the fused path (sc_in = -2, av_qsort_fused), where the scan rides on the first
 * partition and a NaN is met after it: that caller gets the same multiset back, permuted, and says in advance that it
 * accepts it. -DLOGOS_ZERO_SIGN_UNSAFE restores the old kernel entry for the A/B. */
#ifdef LOGOS_XSS_LEAF
/* THE ROUTER'S OTHER HALF (logos_xss.cpp): x86-simd-sort's own quicksort in place of the in-house recursion, behind the same
   NaN decline and signed-zero repair. From XSS_LEAF_MIN_N up, where one getenv is nothing against the sort; LOGOS_NO_XSS=1
   keeps the in-house kernel, for a one-binary A/B. */
void logos_xss_qsort_f64(double *a, size_t n);
void logos_xss_qsort_par_f64(double *a, size_t n, int threads);
/* THE HYBRID'S PARALLEL HALF: unstructured data at up to XSS_PAR_MAX threads goes to the library whole (make whyfast:
   at 2 and 4 threads its quicksort leads the engine's partition, from 8 the partition leads). Set from a ladder with
   the gate lifted, not predicted. LOGOS_NO_XSS_PAR=1 keeps the partition, for the one-binary A/B. */
/* 2, not 4: laddered with the gate lifted (8M, layout cancelled) the library leads the partition 1.12-2.70x at two threads and
   0.99-2.06x at four, and loses from eight (uniform 0.80, 0.63 at fourteen). But at FOUR threads the guards break: eight
   concatenated logs 0.66 and a noisy ring buffer 0.90, against 1.02-1.16 on the random shapes. At two, every guard is 0.97-1.08. */
#ifndef XSS_PAR_MAX
#define XSS_PAR_MAX 2
#endif
#ifndef XSS_LEAF_MIN_N
#define XSS_LEAF_MIN_N 4096
#endif
static atomic_size_t g_xss_calls;       /* logos_twist_last(14): ranges handed to the library; reading clears it */
#endif
#ifndef LOGOS_ZERO_SIGN_UNSAFE
/* the kernel proper, NaN-free and every zero +0.0: the library where the build has it, else the in-house recursion */
static void av_sort_core(double *a, size_t n, int P) {
    (void)P;
#ifdef LOGOS_XSS_LEAF
    if (P > 1) { logos_xss_qsort_par_f64(a, n, P); atomic_fetch_add_explicit(&g_xss_calls, 1, memory_order_relaxed); return; }
    if (n >= XSS_LEAF_MIN_N && !getenv("LOGOS_NO_XSS")) { logos_xss_qsort_f64(a, n); atomic_fetch_add_explicit(&g_xss_calls, 1, memory_order_relaxed); return; }
#endif
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
}
/* the -0.0 made +0.0 before the sort go back at the start of the zero run, contiguous in sorted output, where total order puts them */
static void av_negz_restore(double *a, size_t n, size_t nneg) {
    size_t lo = 0, hi = n;                                             /* first element >= 0.0: the zero run starts here */
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (a[mid] < 0.0) lo = mid + 1; else hi = mid; }
    for (size_t k = 0; k < nneg; k++) a[lo + k] = -0.0;
}
static atomic_size_t g_fuse_parts, g_fuse_declines;   /* logos_twist_last(15) / (16): fused first partitions, and NaN declines after one */
#endif
#ifndef AV_FUSE_MIN_N
#define AV_FUSE_MIN_N 65536
#endif
#ifndef LOGOS_ZERO_SIGN_UNSAFE
/* av_partition's loop for a whole range of at least AV_FUSE_MIN_N, gt = 0, with ONE compare and one OR per vector added: the
   NaN-or-zero flag of av_scan_nan_negzero, read off the vectors the partition loads anyway. A separate copy, so the recursion's
   own av_partition is the same code it was. *flag = 1 when a NaN or a zero went by. */
static size_t av_partition_fl(double *a, size_t n, double pivot, double *smallest, double *biggest, int *flag) {
    enum { U = AV_UNROLL };
    size_t lo = 0, hi = n; double sm = *smallest, bg = *biggest; int fs = 0;
    while ((hi - lo) & 3) {                                   /* scalar until the length is a multiple of 4 */
        double x = a[lo]; if (x < sm) sm = x; if (x > bg) bg = x; fs |= (x != x) | (x == 0.0);
        if (x >= pivot) { hi--; a[lo] = a[hi]; a[hi] = x; } else lo++;
    }
    __m256d piv = _mm256_set1_pd(pivot), mn = _mm256_set1_pd(sm), mx = _mm256_set1_pd(bg), nz = _mm256_setzero_pd(), zero = _mm256_setzero_pd();
    size_t l = lo, r = hi;
    __m256d vl[U], vr[U];
    for (int i = 0; i < U; i++) { vl[i] = _mm256_loadu_pd(a + lo + 4 * i); vr[i] = _mm256_loadu_pd(a + hi - 4 * U + 4 * i); }
    size_t left = lo + 4 * U, right = hi - 4 * U;
    while (right - left >= 4 * U) {
        __m256d v[U];
        if (r - right < left - l) { right -= 4 * U; for (int i = 0; i < U; i++) v[i] = _mm256_loadu_pd(a + right + 4 * i); }
        else                      { for (int i = 0; i < U; i++) v[i] = _mm256_loadu_pd(a + left + 4 * i); left += 4 * U; }
        for (int i = 0; i < U; i++) { nz = _mm256_or_pd(nz, _mm256_cmp_pd(v[i], zero, _CMP_EQ_UQ)); av_part_vec(a, &l, &r, v[i], piv, 0, &mn, &mx); }
    }
    while (right > left) {
        __m256d v;
        if (r - right < left - l) { right -= 4; v = _mm256_loadu_pd(a + right); }
        else                      { v = _mm256_loadu_pd(a + left); left += 4; }
        nz = _mm256_or_pd(nz, _mm256_cmp_pd(v, zero, _CMP_EQ_UQ)); av_part_vec(a, &l, &r, v, piv, 0, &mn, &mx);
    }
    for (int i = 0; i < U; i++) { nz = _mm256_or_pd(nz, _mm256_cmp_pd(vl[i], zero, _CMP_EQ_UQ)); av_part_vec_m(a, &l, &r, vl[i], piv, 0, &mn, &mx); }
    for (int i = 0; i < U; i++) { nz = _mm256_or_pd(nz, _mm256_cmp_pd(vr[i], zero, _CMP_EQ_UQ)); av_part_vec_m(a, &l, &r, vr[i], piv, 0, &mn, &mx); }
    double t[4]; _mm256_storeu_pd(t, mn); for (int i = 0; i < 4; i++) if (t[i] < sm) sm = t[i];
    _mm256_storeu_pd(t, mx); for (int i = 0; i < 4; i++) if (t[i] > bg) bg = t[i];
    *smallest = sm; *biggest = bg; *flag = fs | (_mm256_movemask_pd(nz) != 0);
    return l;
}
/* LEVER C (2026-10-05, make frontprobe): THE KERNEL'S FIRST PARTITION IS THE PROFILE PASS. x86-simd-sort already reads min
   and max off its partition's vectors (partition_unrolled(..., &smallest, &biggest)); this adds the NaN-or-zero flag to the
   same loads, so the scan that used to precede the kernel is gone. The top level is partitioned here, by the in-house
   partition, and the two parts go to the kernel proper (the library where the build has it). The cost of riding along:
   a NaN is found AFTER one partition has moved data, so a decline returns a PERMUTATION of the input rather than the input,
   and only callers that accept that pass sc_in = -2 (sort_fn, leaf_f64's skewed branch: both continue on the same multiset).
   A flagged zero costs what it cost before, one scalar pass, then the same signed-zero repair. LOGOS_NO_PARTFUSE=1: scan first. */
static int av_qsort_fused(double *a, size_t n) {
    double sm = INFINITY, bg = -INFINITY, p = av_pivot(a, n); int fl = 0;
    atomic_fetch_add_explicit(&g_fuse_parts, 1, memory_order_relaxed);
    size_t m = av_partition_fl(a, n, p, &sm, &bg, &fl), nneg = 0;
    if (fl) {                                                  /* a NaN or a zero went by: tell them apart, as the scan did */
        int nan = 0, negz = 0;
        for (size_t k = 0; k < n; k++) { nan |= (a[k] != a[k]); negz |= (a[k] == 0.0 && signbit(a[k])); }
        if (nan) { atomic_fetch_add_explicit(&g_fuse_declines, 1, memory_order_relaxed); return 0; }
        if (negz) for (size_t k = 0; k < n; k++) if (a[k] == 0.0 && signbit(a[k])) { a[k] = 0.0; nneg++; }
    }
    if (!(sm == bg)) {                                         /* all equal: sorted. Else av_qsort_rec's top level, then the parts */
        if (m == 0) av_sort_core(a, n, 1);                     /* the pivot was the minimum: the ordinary recursion splits the equals off */
        else if (p == bg) av_sort_core(a, m, 1);               /* the right part is all == pivot */
        else { av_sort_core(a, m, 1); av_sort_core(a + m, n - m, 1); }
    }
    if (nneg) av_negz_restore(a, n, nneg);
    return 1;
}
#endif
/* sc_in: the caller's verdict on this range's multiset. -1 = none: scan here, and decline (on a NaN) before anything moves.
   -2 = none, and the caller accepts a permuted array on a decline: from AV_FUSE_MIN_N the scan rides on the first partition
   (av_qsort_fused). >= 0: what av_scan_nan_negzero would return (sf_profile_avx2 proves 0; leaf_f64's skewed branch scans
   itself). Only where the scan happens moves; the decline and the signed-zero repair are the same code. */
static int avx2_qsort_f64_ps(double *a, size_t n, int P, int sc_in) {
    if (n < 2) return 1;
    (void)P; (void)sc_in;
#ifdef LOGOS_ZERO_SIGN_UNSAFE
    if (av_has_nan(a, n)) return 0;
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
#else
    if (sc_in == -2 && P == 1 && n >= AV_FUSE_MIN_N && !g_no_partfuse) return av_qsort_fused(a, n);
    int sc = sc_in >= 0 ? sc_in : av_scan_nan_negzero(a, n);
    if (sc & 1) return 0;
    size_t nneg = 0;
    if (sc & 2) for (size_t i = 0; i < n; i++) if (a[i] == 0.0 && signbit(a[i])) { a[i] = 0.0; nneg++; }
    av_sort_core(a, n, P);
    if (nneg) av_negz_restore(a, n, nneg);
#endif
    return 1;
}
#ifdef LOGOS_XSS_LEAF
static int avx2_qsort_f64_p(double *a, size_t n, int P) { return avx2_qsort_f64_ps(a, n, P, -1); }   /* the hybrid's parallel half */
#endif
static int avx2_qsort_f64(double *a, size_t n) { return avx2_qsort_f64_ps(a, n, 1, -1); }
static int avx2_qsort_f64_sc(double *a, size_t n, int sc) { return avx2_qsort_f64_ps(a, n, 1, sc); }
#ifdef LOGOS_AV_HOOK
/* make avkernbench: the in-house kernel alone, and its partition count (elements partitioned, with AV_STATS) */
int logos_av_qsort_hook(double *a, size_t n) { return avx2_qsort_f64(a, n); }
void logos_av_net_hook(double *a, size_t n) { av_base(a, n); }   /* the base case alone, no NaN scan */
#ifdef AV_STATS
size_t logos_av_stat_elems(void) { size_t r = av_stat_elems; av_stat_elems = 0; av_stat_calls = 0; return r; }
#else
size_t logos_av_stat_elems(void) { return 0; }
#endif
#endif
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
    /* The SCALE must be finite, not only the range. A range narrower than about 1e-305 (any bucket of subnormals) makes
       classes / range overflow to +inf; then every element computes x * inf = inf and the minimum computes 0 * inf = NaN,
       and the casts below are undefined. Here that happened to put everything in class 0 and the insertion pass sorted
       the bucket quadratically (43 ms of leaf time for 300k elements); in coarse_bucket_sort it put the MINIMUM in the
       LAST bucket and the sort came back wrong. A comparison sort has no scale. Found 2026-09-20 by an IEEE probe. */
    if (!isfinite(scale)) { quicksort(a, 0, (long)n - 1, 2 * log2i(n)); return a; }
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
    /* a subnormal-wide range: the scale overflows, the minimum computes 0 * inf = NaN, its cast is garbage and the clamp
       below sent it to the LAST bucket. NaN-free data with subnormals came back UNSORTED from every parallel sort of 300k
       elements or more, on the committed engine too (see flash_sort). */
    if (!isfinite(scale)) { quicksort(src, lo, hi, 2 * log2i((size_t)len)); return src; }
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
    if (!isfinite(smx - smn)) return (double)KC;   /* every finite value falls in one value-linear bin: maximal skew, so bits keying (the cast below was NaN -> long, undefined) */
    double sc = KC / (smx - smn); int Hh[KC]; memset(Hh, 0, sizeof Hh);
    if (!isfinite(sc)) return (double)KC;          /* a subnormal-wide sample range: the same overflow, the same answer as an infinite one */
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

#define MERGE_STACK_RUNS 64
static void merge_all_runs(double *a, double *buf, const size_t *runs, size_t n_runs) {
    /* The two work lists used to be malloc'd, and a failed malloc RETURNED with the runs unmerged: a sort that came back
       unsorted and said nothing (make faultprobe, 2026-09-20; the run-merge route, relocation and the knot route all come
       through here). Nearly every caller has a handful of runs (the run-merge route at most MERGE_MAX_RUNS, a crossing a
       few strands), so the lists live on the stack up to MERGE_STACK_RUNS, and past that a failed allocation folds the
       runs in left to right, which needs no list at all: slower, never wrong. The merge kernels allocate nothing. */
    size_t stackA[2 * (MERGE_STACK_RUNS + 4)], stackB[2 * (MERGE_STACK_RUNS + 4)], capw = 2 * (n_runs + 4);
    size_t *A = stackA, *B = stackB, *heapA = NULL, *heapB = NULL;
    if (n_runs > MERGE_STACK_RUNS) {
        heapA = malloc(capw * sizeof(size_t)); heapB = malloc(capw * sizeof(size_t));
        if (!heapA || !heapB) {
            free(heapA); free(heapB);
            for (size_t k = 1; k < n_runs; k++) merge_gallop(a, buf, runs[0], runs[2 * k - 1], runs[2 * k], runs[2 * k + 1]);
            return;
        }
        A = heapA; B = heapB;
    }
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
    free(heapA); free(heapB);
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
/* A CEILING FOR THE HASH ROUTE ONLY (2026-09-22, `make capbench`). Both counting routes run two parallel phases with a
 * serial merge between them, and `make profile` at 8M has 6.0-6.9 of 14 threads busy inside that phase with NONE waiting.
 * But the two families want opposite things, measured in pairs at 1M-16M: a ceiling of 3, 6 or 8 applied to BOTH costs the
 * LATTICE shapes 0.53-0.90 at 8M and 16M (its map phase is real parallel work and wants every thread), while the HASH
 * route stops scaling around 8. So a ceiling was tried on the hash route ALONE (`count_threads_hash`) -- and it did not
 * clear its own stop rule either: few_distinct read 1.06 0.94 1.01 1.12 1.04 and 1.06 0.97 1.01 1.07 1.06 over 1M-16M in
 * two runs of 20 pairs, so 1.0 at 4M and about 1.1 at 8M, inside what 20 pairs resolve. It ships OFF. The knob stays as
 * the A/B switch and as the record of the measurement: LOGOS_COUNT_MAX=<k> caps the hash route per sort, 0 = no ceiling
 * (the default). What the profile actually asks for is a PARALLEL merge, not fewer threads: open direction 0a. */
#ifndef COUNT_MAX_THREADS
#define COUNT_MAX_THREADS 0
#endif
static int g_count_max = COUNT_MAX_THREADS;
static inline int count_threads(size_t n, int P) { size_t pm = n / COUNT_MIN_PER_THREAD; if (pm < COUNT_MIN_THREADS) pm = COUNT_MIN_THREADS; return (size_t)P > pm ? (int)pm : P; }
static inline int count_threads_hash(size_t n, int P) { int r = count_threads(n, P); if (g_count_max > 0 && r > g_count_max) r = g_count_max; return r; }
/* (outside the relocation #ifndef: the counting routes need this whether or not relocation is compiled in;
   inside it, -DLOGOS_NO_RELOCATE -- the pre-J5 engine that make j5bench builds -- did not compile) */
#if defined(LOGOS_NO_RELOCATE) && !defined(LOGOS_NO_KNOT)
#define LOGOS_NO_KNOT          /* the knot route lives inside the relocation route */
#endif
#if defined(LOGOS_NO_SPLITTER_TABLE) && !defined(LOGOS_NO_MODEKS)
#define LOGOS_NO_MODEKS        /* the per-mode certificate reads the modes the splitter table found. Without this the pre-H0b engine
                                  that make stbench builds had not compiled since that certificate was added (found 2026-09-20) */
#endif
#ifndef LOGOS_NO_RELOCATE
#define RELOC_MIN_N 4096
/* The run detector behind relocation and the knot route gives up once it holds 256 runs whose average is under a floor, and
 * keeps room for n / floor runs. The floor used to be the run MERGE's constant (MIN_AVG_RUN_FOR_MERGE = 4096, swept for the
 * merge by make knobbench), inherited by two routes that COPY runs. So at one thread every array of disjoint sorted blocks
 * averaging 256 to 4095 elements was declined and sorted in full: 10.5-12.5 ns per element where blocks of 4096 took 2.0-2.4,
 * and runs of EXACTLY 4096 passed by nothing, so the same runs rotated by 0.37 n failed by 12 elements and cost 5.8x (found by
 * unknotprobe's ring buffer rows; the ladder is its `cliff` mode, 8.4M, 20 pairs). One thread: the knot route's own measured
 * minimum, 256; disjoint blocks 2.6-5.5x, shuffled staircases 1.9-4.2x, runs by coarse key 3.2-5.7x, 4096 and up 0.98-1.02.
 * P threads: the partition is already near the copy's cost there, and every lower floor was MIXED (256: 0.72-0.96 on small
 * blocks and staircases, 1.15-1.41 on keyed runs; 1024: 0.91-0.98 against 1.25-1.47), so it stays at 4096. Measured and NOT kept:
 * forgiving each detection chunk its partial first run, meant for runs of exactly 4096 at P threads (1.12-1.43 when a lower floor
 * admitted them). It read 0.98-1.01 on every row: at P threads the per-thread run cap (n / floor / P + 2) binds first, since a
 * chunk of exactly-4096 runs holds 148 of them counting the partial pieces at both ends. LOGOS_RELOC_AVG=<k> sets the floor per
 * sort (read where relocation starts), for the ladder. */
#ifndef RELOC_MIN_AVG_RUN_SEQ
#define RELOC_MIN_AVG_RUN_SEQ 256
#endif
#ifndef RELOC_MIN_AVG_RUN_PAR
#define RELOC_MIN_AVG_RUN_PAR MIN_AVG_RUN_FOR_MERGE
#endif
static size_t g_reloc_avg = RELOC_MIN_AVG_RUN_SEQ;   /* set on the calling thread where relocation starts; the detector's threads read it */
static int g_reloc_nocopy = 0;                       /* LOGOS_RELOC_NOCOPY=1: the placements leave their output in scratch. WRONG OUTPUT BY DESIGN: the
                                                        ceiling of a route that hands back a sorted DESCRIPTION instead of copying (./unknotprobe ... nocopy) */
static int g_stray_on = 1;                           /* LOGOS_NO_STRAY=1 clears it */
static int g_stray_par = 1;                          /* LOGOS_STRAY_SEQ=1 clears it: the stray list at ONE thread only, as it shipped before 2026-09-26 */
#define STRAY_DIV_PAR 64
#ifndef RELOC_CHECK_MIN_SEEN
#define RELOC_CHECK_MIN_SEEN 4096                     /* the run detector's early checkpoints may stop the scan only past this many elements */
#endif
static size_t g_stray_div = 16;                      /* the stray list holds at most a 1/g_stray_div share of a chunk: 64 until 2026-10-21, LOGOS_STRAY_DIV=<k> */
static int g_onerun_off = 0;                         /* LOGOS_NO_ONERUN=1 sets it: a single run declines, as before */
static int g_onerun_copy = 0;                        /* LOGOS_ONERUN_COPY=1: one strand's strays merged back through scratch, as before */
static size_t g_onerun_count = 0;                    /* logos_twist_last(12): sorts finished as one strand */
#ifndef RELOC_MIN_PER_THREAD
#define RELOC_MIN_PER_THREAD 500000
#endif
/* THE MOEBIUS TWIST (2026-09-20; ceiling and A/B: make twistbench). A run that lies DESCENDING in memory used to be swapped end
 * for end the moment the detector had found it: a read and a write of every such element, before anything useful happened to
 * it. Now the run keeps lying as it lies and its descriptor carries one orientation bit. first/last are its smallest and
 * largest value whichever way it lies, every consumer reads a twisted strand from its far end (a backward copy where a lone
 * piece is placed, a cursor walking down where two strands cross, a search in logical coordinates where the knot route cuts),
 * and the detector no longer writes to the array at all, so a declined probe leaves the input exactly as it was.
 * In a flat [start,end,...] run list the orientation rides in the top bit of the END index (RUN_TWIST); lists from the
 * sequential leaf detector and lists in output coordinates never carry it. LOGOS_NO_TWIST=1 / -DLOGOS_NO_TWIST: reverse in
 * place as before. */
typedef struct { double first, last; size_t start, len; int rev; } rdesc_t;
#define RUN_TWIST ((size_t)1 << (sizeof(size_t) * 8 - 1))
static size_t g_twist_knot, g_twist_reloc;      /* the witness: see logos_twist_last() */
#define RUN_END(e) ((e) & ~RUN_TWIST)
#define RUN_REV(e) (((e) & RUN_TWIST) != 0)
/* o[i] = x[cnt-1-i]: a twisted strand's piece, turned the right way round by the copy that was going to be made anyway */
static void copy_rev(double *restrict o, const double *restrict x, size_t cnt) { for (size_t i = 0; i < cnt; i++) o[i] = x[cnt - 1 - i]; }
static inline void strand_copy(double *o, const double *x, size_t cnt, int rev) { if (rev) copy_rev(o, x, cnt); else memcpy(o, x, cnt * sizeof(double)); }
static int cmp_rdesc(const void *x, const void *y) { double a = ((const rdesc_t *)x)->first, b = ((const rdesc_t *)y)->first; return (a > b) - (a < b); }

/* Sorted descriptors + chains for a flat [start,end,...] run list. chain[k] is
 * the index of chain k's first descriptor (nchain+1 entries, sentinel R);
 * *merged = elements inside chains with more than one run. 0 on alloc failure. */
static int runs_plan(const double *a, const size_t *runs, size_t R, rdesc_t **pd, size_t **pchain, size_t *pnchain, size_t *pmerged) {
    rdesc_t *d = malloc(R * sizeof *d); size_t *ch = malloc((R + 1) * sizeof *ch);
    if (!d || !ch) { free(d); free(ch); return 0; }
    for (size_t k = 0; k < R; k++) { size_t s = runs[2 * k], e = RUN_END(runs[2 * k + 1]); int rv = RUN_REV(runs[2 * k + 1]);
        d[k].first = rv ? a[e] : a[s]; d[k].last = rv ? a[s] : a[e]; d[k].start = s; d[k].len = e - s + 1; d[k].rev = rv; }
    qsort(d, R, sizeof *d, cmp_rdesc);
    size_t nc = 0, merged = 0, celems = d[0].len, cruns = 1; double cmax = d[0].last; ch[nc++] = 0;
    for (size_t k = 1; k < R; k++) {
        if (d[k].first < cmax) { if (d[k].last > cmax) cmax = d[k].last; celems += d[k].len; cruns++; }   /* overlaps the chain so far */
        else { if (cruns > 1) merged += celems; ch[nc++] = k; cmax = d[k].last; celems = d[k].len; cruns = 1; }
    }
    if (cruns > 1) merged += celems;
    ch[nc] = R; *pd = d; *pchain = ch; *pnchain = nc; *pmerged = merged; return 1;
}

#ifndef LOGOS_NO_KNOT
/* ---- the knot route: comparisons only where strands cross (2026-09-19; A/B: make knotbench) -------------
 * J5 treats a chain of overlapping runs as one unit: it merges ALL of the chain, and it declines the whole
 * array once more than half of it sits in chains. But two runs that overlap in a tenth of their range are
 * tangled only in that tenth. Read each run as a STRAND and the run descriptors, sorted by first value, as a
 * linked list that the sorted output walks along. Where one strand is alone in its value interval the walk is
 * a memcpy. A CROSSING is a value interval in which two or more strands both hold elements; a knot is a group
 * of strands joined by crossings (J5's chain). Untying a knot takes comparisons only inside its crossings.
 *
 * The plan is a sweep over the 2R strand endpoints in value order (a first value cuts below itself, a last
 * value above itself). Between two consecutive cuts the set of live strands is constant, and the piece of an
 * ascending strand between two cuts is contiguous, found by a galloping search from where its previous piece
 * ended. An interval with one non-empty piece is a copy; one with k >= 2 is a k-way merge of exactly those
 * pieces. Pieces are counted by the ELEMENTS they hold, so a fused run whose interval hides a hole (the defect
 * that made J5's first cut never fire, and the reason it gap-splits) reads correctly here: over the hole its
 * piece is empty and nothing crosses.
 * Work: O(R log R) plan, O(n) copies, O(crossing mass * log k) comparisons.
 *
 * It is NOT the ledger's parallel merge sort: that put every element through a P-way merge, this merges only
 * what crosses, and the gate bounds that. Gate: cost = sum over crossings of mass * ceil(log2 k) must be at
 * most KNOT_COST_MAX_* times n (measured, see below); whatever J5's own gate accepts is accepted too, since
 * there the finer plan can only do less work. Every early gate J5 has (sample, per-thread prefix) asks the
 * knot question only when J5's own answer would be no, so shapes J5 takes, or declines for short runs, cost
 * nothing extra. The placement's thread count follows its WORK (n + KNOT_MERGE_WEIGHT * cost), not n.
 * Parallel placement: tiles of the output are filled by copy; two-strand crossings are merged straight from
 * a[] into place off an atomic queue (a large one is first cut into grains at values taken from its largest
 * piece, the same operation as one more sweep boundary); crossings of three or more strands are staged side
 * by side and merged after the barrier with a[] as scratch.
 * -DLOGOS_NO_KNOT removes it; LOGOS_NO_KNOT=1 disables it at runtime (J5 exactly as before);
 * LOGOS_KNOT_MAX=<x> overrides all three cost gates (the bench's ladder uses it to find the break-even). */
/* The cost gates, as multiples of n, set from measurement (make knotbench at 8.4M; FINDINGS 3ac). The first cut used
 * J5's n/2 on a written prediction that the route would break even with 60-100% of the array inside crossings at 14
 * threads. Wrong in kind: with the gate lifted, two strands with EVERY element crossing (cost 1.0n) measured 2.62x at
 * one thread and 1.52x at 14 against the shipped engine, three live strands (1.57n) 2.44x and 1.12x, and it loses only
 * from 3-4 live strands (1.96n: 2.48x, 0.82x) through 8 (2.95n: 1.29x, 1.05x) to 64 fully interleaved (6n: 0.79x, 0.77x).
 * And n/2 was not merely cautious: a decline comes after the whole array has been scanned for runs, so the shapes
 * just past it paid 4-9% for nothing (0.92-0.96 where 1.00 was predicted).
 *
 * But "against the shipped engine" flatters the parallel rows: on a shuffled staircase the shipped relocation scans
 * the array and THEN declines, and that wasted scan is in the baseline. Against the PURE PARTITION (an engine copy built
 * with -DLOGOS_NO_RELOCATE, both copy orientations, layout cancelled) the route at 14 threads is a tie on two-strand
 * crossings (10% crossing 0.96, 20% 0.97, 60% 1.03, 100% 1.11, zigzag 1.01, 14 shards 1.08) and LOSES on three strands
 * (0.89), while at one thread it wins 1.8-4.1x on every one of them. So the parallel gate stops at two-strand cost:
 * three strands are then declined by the sample gate in half a millisecond and sort at the partition's speed.
 *   parallel sorts:                      1.0n
 *   sequential, more than MERGE_MAX_RUNS: 3.0n  (the fallback is the sequential leaf, 4-5x a merge level)
 *   sequential, at most MERGE_MAX_RUNS:   3.0n  (was 1.0n until 2026-09-26, set from 4 interleaved runs at 0.955 with the gate
 *                                               lifted, where the fallback is the run merge. Re-measured with the few-runs gate
 *                                               ALONE lifted (LOGOS_KNOT_FEW=3, ./sineprobe ... fewgate, ./knotbench ... few,
 *                                               tsbench): pure cosines of 3-8 runs 1.72-3.22x, 8 concatenated logs 1.28x, and
 *                                               those 4 interleaved runs now 1.27x; h = 9 and every R > 8 shape 0.98-1.02) */
#ifndef KNOT_COST_MAX_SEQ
#define KNOT_COST_MAX_SEQ 3.0
#endif
#ifndef KNOT_COST_MAX_FEW
#define KNOT_COST_MAX_FEW 3.0
#endif
#ifndef KNOT_COST_MAX_PAR
#define KNOT_COST_MAX_PAR 1.0
#endif
#ifndef KNOT_GRAIN_MIN
#define KNOT_GRAIN_MIN 32768
#endif
/* The plan is only worth making over LONG strands. The cost gate cannot see that on its own: when runs hold two or
 * three elements nearly every element is a run endpoint, so almost nothing "crosses", the gate says go, and the sweep
 * (five allocations, a sort of the 2R endpoints, a galloping search per live strand per cut) ends up sorting the array
 * the slow way. Found by make scale-full once it grew sizes below 10k: random data at n = 32 is 13 runs, and the first
 * cut took 2431 ns where the committed engine took 781; block_perm at n = 100, 4969 against 2105. J5's chain gate
 * never had the problem, since short random runs all overlap. So: an average run of at least this many elements, in
 * every place the route is asked. At 256 the plan is under a tenth of any fallback (about 30 ns * R * log2 R). */
#ifndef KNOT_MIN_AVG_RUN
#define KNOT_MIN_AVG_RUN 256
#endif
#ifndef KNOT_MERGE_WEIGHT
#define KNOT_MERGE_WEIGHT 3       /* a merged element, per level, in units of a copied one (the placement's thread cap) */
#endif
static int g_knot_on = 1;
static int g_knot_gallop = 1;          /* LOGOS_NO_KNOT_GALLOP=1 clears it; read once per sort in knot_env, on the calling thread */
static double g_knot_frac_seq = KNOT_COST_MAX_SEQ, g_knot_frac_few = KNOT_COST_MAX_FEW, g_knot_frac_par = KNOT_COST_MAX_PAR;
/* read once per sort on the calling thread (one sort at a time, the buffer pool's contract); leaves read the globals */
static void knot_env(void) {
    g_knot_on = !getenv("LOGOS_NO_KNOT");
    g_knot_gallop = !getenv("LOGOS_NO_KNOT_GALLOP");
    const char *e = getenv("LOGOS_KNOT_MAX"); double f = e ? atof(e) : 0.0;
    const char *ef = getenv("LOGOS_KNOT_FEW"); double ff = ef ? atof(ef) : 0.0;      /* the few-runs gate alone (<= MERGE_MAX_RUNS runs), for its A/B */
    g_knot_frac_seq = f > 0 ? f : KNOT_COST_MAX_SEQ; g_knot_frac_few = ff > 0 ? ff : f > 0 ? f : KNOT_COST_MAX_FEW; g_knot_frac_par = f > 0 ? f : KNOT_COST_MAX_PAR;
}
typedef struct { size_t src, len; int rev; } kpt_t;                          /* src: the piece's lowest address, whichever way it lies */
typedef struct { size_t src, len, dst; int copy, rev; } kpiece_t;   /* copy: placed by a copy (alone in its interval, or staged for a k >= 3 merge); rev: twisted */
typedef struct { size_t p0, k, dst, len; } kseg_t;             /* a crossing: pieces [p0, p0+k) land at dst and must be merged */
typedef struct { kpiece_t *pc; size_t npc, cap_pc; kseg_t *sg; size_t nsg, cap_sg, n3, crossing, cost, kmax, total; int last_single; } knot_plan_t;
static void knot_plan_free(knot_plan_t *K) { free(K->pc); free(K->sg); K->pc = NULL; K->sg = NULL; }
/* Element c, counted in ASCENDING value order, of the strand that occupies a[st, st+ln). A TWISTED strand lies descending in
 * memory and is read from its far end; KNOT_SRC is the lowest address of its logical range [c0, c1). */
#define KNOT_AT(a, st, ln, rv, c) ((rv) ? (a)[(st) + (ln) - 1 - (c)] : (a)[(st) + (c)])
#define KNOT_SRC(st, ln, rv, c0, c1) ((rv) ? (st) + (ln) - (c1) : (st) + (c0))
/* Where a cut falls inside a strand, in those logical coordinates: the first position in [lo,hi) that is past the cut.
 * above = 0 cuts BELOW v (elements < v stay before it), above = 1 cuts ABOVE v (elements <= v stay before it).
 * Galloping from lo: a strand's next cut is either a few elements on or a whole piece away. */
#define KNOT_BEFORE(x) (above ? (x) <= v : (x) < v)
static inline size_t knot_cut(const double *a, size_t st, size_t ln, int rv, size_t lo, size_t hi, double v, int above) {
    if (lo >= hi || !KNOT_BEFORE(KNOT_AT(a, st, ln, rv, lo))) return lo;
    size_t j = lo, s = 1;
    while (j + s < hi && KNOT_BEFORE(KNOT_AT(a, st, ln, rv, j + s))) { j += s; s <<= 1; }
    size_t L = j + 1, R = j + s < hi ? j + s : hi;
    while (L < R) { size_t m = L + ((R - L) >> 1); if (KNOT_BEFORE(KNOT_AT(a, st, ln, rv, m))) L = m + 1; else R = m; }
    return L;
}
#undef KNOT_BEFORE
static inline size_t knot_levels(size_t k) { size_t l = 0; while (((size_t)1 << l) < k) l++; return l; }
static int knot_push_piece(knot_plan_t *K, size_t src, size_t len, size_t dst, int copy, int rev) {
    if (K->npc == K->cap_pc) { size_t nc = K->cap_pc ? 2 * K->cap_pc : 256; kpiece_t *p = realloc(K->pc, nc * sizeof *p); if (!p) return 0; K->pc = p; K->cap_pc = nc; }
    kpiece_t q = { src, len, dst, copy, rev }; K->pc[K->npc++] = q; return 1;
}
static int knot_push_seg(knot_plan_t *K, size_t p0, size_t k, size_t dst, size_t len) {
    if (K->nsg == K->cap_sg) { size_t nc = K->cap_sg ? 2 * K->cap_sg : 64; kseg_t *p = realloc(K->sg, nc * sizeof *p); if (!p) return 0; K->sg = p; K->cap_sg = nc; }
    kseg_t g = { p0, k, dst, len }; K->sg[K->nsg++] = g; return 1;
}
/* one value interval, as it will be placed: k non-empty pieces landing at K->total */
static int knot_emit_one(knot_plan_t *K, const kpt_t *tp, size_t k, size_t seglen) {
    if (k == 1) {
        if (K->last_single && K->npc) {                       /* the same strand, still alone: extend its copy */
            kpiece_t *p = &K->pc[K->npc - 1];
            /* an untwisted strand's next piece lies after the last one in memory, a twisted strand's BEFORE it */
            if (p->rev == tp[0].rev && (p->rev ? tp[0].src + tp[0].len == p->src : p->src + p->len == tp[0].src)) {
                if (p->rev) p->src = tp[0].src;
                p->len += tp[0].len; K->total += seglen; return 1; }
        }
        if (!knot_push_piece(K, tp[0].src, tp[0].len, K->total, 1, tp[0].rev)) return 0;
        K->last_single = 1; K->total += seglen; return 1;
    }
    size_t p0 = K->npc, o = K->total;
    /* Crossings of three or more are STAGED (copied side by side, then merged in scratch). A direct three- and four-way merge from
       the source was built and measured on 2026-09-22 (./unknotprobe 40 8388608 ab, rows DIRECT): 0.68 on three live strands and
       0.52 on four at one thread, two runs. One pass with a longer chain loses to three passes with a short one: every step of
       the direct merge waits on two dependent compares and the load they choose, where the staged path is two memcpys and
       branch-free two-way merges. */
    for (size_t q = 0; q < k; q++) { if (!knot_push_piece(K, tp[q].src, tp[q].len, o, k >= 3, tp[q].rev)) return 0; o += tp[q].len; }
    if (!knot_push_seg(K, p0, k, K->total, seglen)) return 0;
    if (k >= 3) K->n3++;
    K->last_single = 0; K->total += seglen; return 1;
}
/* A crossing larger than the grain is cut at values read from its largest piece, every piece at the same values:
 * the sub-intervals are independent merges (or plain copies, where only one piece has elements). */
static int knot_emit(knot_plan_t *K, const double *a, const kpt_t *tp, kpt_t *sub, size_t *cq, size_t k, size_t seglen, size_t grain, int emit) {
    if (k >= 2) { K->crossing += seglen; K->cost += seglen * knot_levels(k); if (k > K->kmax) K->kmax = k; }
    if (!emit) { K->total += seglen; return 1; }
    if (k < 2 || seglen <= grain) return knot_emit_one(K, tp, k, seglen);
    size_t m = (seglen + grain - 1) / grain, big = 0;
    for (size_t q = 1; q < k; q++) if (tp[q].len > tp[big].len) big = q;
    for (size_t q = 0; q < k; q++) cq[q] = 0;                /* cursors in each piece's logical coordinates */
    for (size_t c = 1; c <= m; c++) {
        size_t ks = 0, sl = 0;
        double cv = 0; if (c < m) cv = KNOT_AT(a, tp[big].src, tp[big].len, tp[big].rev, (tp[big].len / m) * c + ((tp[big].len % m) * c) / m);
        for (size_t q = 0; q < k; q++) {
            size_t e = tp[q].len, c1 = c < m ? knot_cut(a, tp[q].src, tp[q].len, tp[q].rev, cq[q], e, cv, 0) : e;
            if (c1 > cq[q]) { sub[ks].src = KNOT_SRC(tp[q].src, tp[q].len, tp[q].rev, cq[q], c1); sub[ks].len = c1 - cq[q]; sub[ks].rev = tp[q].rev; ks++; sl += c1 - cq[q]; }
            cq[q] = c1;
        }
        if (ks && !knot_emit_one(K, sub, ks, sl)) return 0;
    }
    return 1;
}
/* d: run descriptors sorted by first value. emit = 0 measures only (crossing, cost, kmax). Returns 0 when there is no
 * plan: a NaN endpoint (a NaN is always a run of its own, since every compare with it fails, and has no place in the
 * order), more live-strand visits than a plan is worth, or no memory. */
static int knot_plan(const double *a, const rdesc_t *d, size_t R, size_t grain, int emit, knot_plan_t *K) {
    memset(K, 0, sizeof *K);
    if (R < 1) return 0;
    for (size_t i = 0; i < R; i++) if (d[i].first != d[i].first || d[i].last != d[i].last) return 0;
    double *ls = malloc(R * sizeof(double)); size_t *cur = malloc(R * sizeof(size_t)), *act = malloc(R * sizeof(size_t)), *cq = malloc(R * sizeof(size_t));
    kpt_t *tp = malloc(2 * R * sizeof *tp);
    if (!ls || !cur || !act || !cq || !tp) { free(ls); free(cur); free(act); free(cq); free(tp); return 0; }
    for (size_t i = 0; i < R; i++) { ls[i] = d[i].last; cur[i] = 0; }     /* cur: how much of strand i is consumed, in its logical coordinates */
    sample_sort(ls, R);
    size_t is = 0, il = 0, nact = 0, visits = 0, visit_cap = 64 * R + 65536; int ok = 1;
    /* The sweep line visits 2R cuts in value order. A strand's FIRST value cuts below itself and its LAST value cuts
     * above itself, so a strand's elements lie exactly between its own two cuts; at equal values the first-cut goes
     * first, and what lies between the two is the elements equal to that value. (Cutting every endpoint below its
     * value, the first cut of this code, put each strand's last element at the head of the NEXT interval, where it
     * turned the whole middle of the neighbouring strand into a two-strand crossing: 98.5% crossing mass measured on
     * a staircase built with 10%.) Firsts arrive sorted in d[], lasts in ls[]: a two-pointer merge yields the cuts. */
    while (il < R) {
        int above = !(is < R && d[is].first <= ls[il]); double v = above ? ls[il] : d[is].first;
        size_t k = 0, seglen = 0;                             /* the interval that ENDS at this cut */
        for (size_t q = 0; q < nact; q++) {
            size_t r = act[q], c0 = cur[r], c1 = knot_cut(a, d[r].start, d[r].len, d[r].rev, c0, d[r].len, v, above);
            cur[r] = c1;
            if (c1 > c0) { tp[k].src = KNOT_SRC(d[r].start, d[r].len, d[r].rev, c0, c1); tp[k].len = c1 - c0; tp[k].rev = d[r].rev; k++; seglen += c1 - c0; }
        }
        visits += nact; if (visits > visit_cap) { ok = 0; break; }
        if (k && !knot_emit(K, a, tp, tp + R, cq, k, seglen, grain, emit)) { ok = 0; break; }
        if (!above) act[nact++] = is++;                       /* the strand whose first value this is goes live */
        else {                                                /* strands ending here are fully consumed: drop them */
            size_t w = 0; for (size_t q = 0; q < nact; q++) if (d[act[q]].last > v) act[w++] = act[q];
            nact = w; il++;
        }
    }
    free(ls); free(cur); free(act); free(cq); free(tp);
    if (!ok) { knot_plan_free(K); return 0; }
    return 1;
}
/* both pieces span the same value interval, so they interleave: a branch-free merge, no galloping (what a gallop would
 * have skipped, the sweep already cut away as copies) */
static void knot_merge2(const double *x, size_t nx, const double *y, size_t ny, double *o) {
    size_t i = 0, j = 0;
    while (i < nx && j < ny) { double xv = x[i], yv = y[j]; int ty = yv < xv; *o++ = ty ? yv : xv; j += (size_t)ty; i += (size_t)!ty; }
    if (i < nx) memcpy(o, x + i, (nx - i) * sizeof(double));
    else if (j < ny) memcpy(o, y + j, (ny - j) * sizeof(double));
}
/* The same merge with either strand TWISTED: its cursor walks down instead of up. An up-run and the down-run after it are a
 * bitonic pair, and this is that pair's two-ended merge; the reversal a bitonic merge is usually preceded by never happens. */
/* One loop per orientation pair, so that a twisted cursor costs what an untwisted one does: two updates per element, and the
 * far-end address folded into the base. The first cut was ONE loop with a signed stride per strand (four updates per element);
 * it is kept behind LOGOS_TWIST_GENERIC=1 because it is what the specialised loops were measured against (make twistbench,
 * mode `merge`). */
#define KNOT_MERGE2_DEF(name, XAT, YAT) \
static void name(const double *x, size_t nx, const double *y, size_t ny, double *o) { \
    size_t i = 0, j = 0; \
    while (i < nx && j < ny) { double xv = XAT, yv = YAT; int ty = yv < xv; *o++ = ty ? yv : xv; j += (size_t)ty; i += (size_t)!ty; } \
    for (; i < nx; i++) *o++ = XAT; \
    for (; j < ny; j++) *o++ = YAT; }
KNOT_MERGE2_DEF(knot_merge2_du, x[nx - 1 - i], y[j])
KNOT_MERGE2_DEF(knot_merge2_ud, x[i], y[ny - 1 - j])
KNOT_MERGE2_DEF(knot_merge2_dd, x[nx - 1 - i], y[ny - 1 - j])
#undef KNOT_MERGE2_DEF
static int g_twist_generic = 0;        /* LOGOS_TWIST_GENERIC=1: the strided loop. Read once per relocation attempt, on the calling thread */
/* THE GALLOPING TWO-STRAND MERGE (2026-09-21; ceiling and A/B: make knotbench, modes `ceiling` and `gallop`). A crossing is two
 * strands that both hold elements of one value interval, and on a staircase they alternate element by element, which is what the
 * branch-free merge above is for. But ONE sparse strand spanning every block (a late column, a sparse index over dense pages)
 * makes every block a crossing of a few sparse elements and the whole block, and the branch-free merge then touches every element
 * one compare at a time: 2.4-2.7x the same array with nothing crossing, at one thread. So: while the strands alternate, eight
 * branch-free steps; when the element KNOT_GALLOP_RUN places ahead in one strand still wins, gallop (steps of 8, 16, 32, then a
 * bisection) and move the whole stretch with one bulk copy. The lookahead costs two predictable compares per eight elements on
 * data that alternates. Ties go to x exactly as in the branch-free merge, so the output is the same bytes (signed zeros too).
 * One function per orientation pair, as above; XAT is element k in ascending value order, XCPY copies cnt of them from k on.
 * Returns the number of gallops, for the witness. LOGOS_NO_KNOT_GALLOP=1: the branch-free merge alone. */
#ifndef KNOT_GALLOP_RUN
#define KNOT_GALLOP_RUN 8
#endif
/* Only a crossing whose pieces are UNBALANCED can gallop far, so only those take this kernel: one piece at least KNOT_GALLOP_RATIO
   times the other. A balanced crossing (every staircase) keeps the branch-free kernel exactly. The first cut ran the lookahead on
   every crossing and read 0.948-0.980 on the staircase where four elements in five are merged (three runs, 40 to 200 pairs). */
#ifndef KNOT_GALLOP_RATIO
#define KNOT_GALLOP_RATIO 4
#endif
#define KNOT_MERGE2G_DEF(name, XAT, YAT, XCPY, YCPY) \
static size_t name(const double *x, size_t nx, const double *y, size_t ny, double *o) { \
    size_t i = 0, j = 0, gallops = 0; const size_t G = KNOT_GALLOP_RUN; \
    while (i < nx && j < ny) { \
        double xv = XAT(i), yv = YAT(j); \
        if (i + G <= nx && XAT(i + G - 1) <= yv) {                 /* x wins the next G, ties included: gallop x */ \
            size_t lo = i + G - 1, s = G; \
            while (lo + s < nx && XAT(lo + s) <= yv) { lo += s; s <<= 1; } \
            size_t hi = lo + s < nx ? lo + s : nx; \
            while (lo + 1 < hi) { size_t m = lo + ((hi - lo) >> 1); if (XAT(m) <= yv) lo = m; else hi = m; } \
            XCPY(o, i, hi - i); o += hi - i; i = hi; gallops++; continue; } \
        if (j + G <= ny && YAT(j + G - 1) < xv) {                  /* y wins the next G, strictly: gallop y */ \
            size_t lo = j + G - 1, s = G; \
            while (lo + s < ny && YAT(lo + s) < xv) { lo += s; s <<= 1; } \
            size_t hi = lo + s < ny ? lo + s : ny; \
            while (lo + 1 < hi) { size_t m = lo + ((hi - lo) >> 1); if (YAT(m) < xv) lo = m; else hi = m; } \
            YCPY(o, j, hi - j); o += hi - j; j = hi; gallops++; continue; } \
        for (size_t st = 0; st < G && i < nx && j < ny; st++) { double a_ = XAT(i), b_ = YAT(j); int ty = b_ < a_; *o++ = ty ? b_ : a_; j += (size_t)ty; i += (size_t)!ty; } \
    } \
    if (i < nx) XCPY(o, i, nx - i); else if (j < ny) YCPY(o, j, ny - j); \
    return gallops; }
#define KG_UP(v, k) (v)[k]
#define KG_DN(v, nv, k) (v)[(nv) - 1 - (k)]
#define KG_CPY_UP(v, oo, k, cnt) memcpy((oo), (v) + (k), (cnt) * sizeof(double))
#define KG_CPY_DN(v, nv, oo, k, cnt) copy_rev((oo), (v) + (nv) - (k) - (cnt), (cnt))
#define KXU(k) KG_UP(x, k)
#define KXD(k) KG_DN(x, nx, k)
#define KYU(k) KG_UP(y, k)
#define KYD(k) KG_DN(y, ny, k)
#define KXCU(oo, k, c) KG_CPY_UP(x, oo, k, c)
#define KXCD(oo, k, c) KG_CPY_DN(x, nx, oo, k, c)
#define KYCU(oo, k, c) KG_CPY_UP(y, oo, k, c)
#define KYCD(oo, k, c) KG_CPY_DN(y, ny, oo, k, c)
KNOT_MERGE2G_DEF(knot_merge2g_uu, KXU, KYU, KXCU, KYCU)
KNOT_MERGE2G_DEF(knot_merge2g_du, KXD, KYU, KXCD, KYCU)
KNOT_MERGE2G_DEF(knot_merge2g_ud, KXU, KYD, KXCU, KYCD)
KNOT_MERGE2G_DEF(knot_merge2g_dd, KXD, KYD, KXCD, KYCD)
#undef KNOT_MERGE2G_DEF
#undef KG_UP
#undef KG_DN
#undef KG_CPY_UP
#undef KG_CPY_DN
#undef KXU
#undef KXD
#undef KYU
#undef KYD
#undef KXCU
#undef KXCD
#undef KYCU
#undef KYCD
/* THE VECTOR TWO-STRAND MERGE (2026-09-26; kernel: make mergebench; A/B: ./knotbench 40 8388608 simd, TS_SWITCH=LOGOS_NO_SIMD_MERGE
 * ./tsbench 40 8388608 switch). The branch-free merge above is a dependency chain: the next pair of loads waits on the compare
 * that moved a cursor, about five cycles per element. Here four elements of each strand are merged in registers by a bitonic
 * network (one compare-exchange across the two vectors, then two inside each), the four smallest are stored, the four largest
 * are carried, and the next four are loaded from whichever strand's head is smaller: one scalar compare per FOUR elements, and
 * both candidate loads are made so that the choice is a blend, not a branch. A compare-exchange is one predicate and two selects
 * (lo takes b where b < a, ties keep a), so a NaN or a signed zero comes out a permutation, where min/max would return the same
 * operand twice for -0.0 against +0.0. A twisted strand is loaded from its far end and reversed in the register, one permute
 * more. Only balanced crossings arrive here (knot_merge2x sends the unbalanced ones to the gallop). The tail, once a strand has
 * fewer than four left, folds the carry into the shorter remainder and the branch-free loop finishes. AVX2 + FMA at run time,
 * read with the per-sort switches; LOGOS_NO_SIMD_MERGE=1 keeps the scalar merge. logos_twist_last(13) counts the merges taken. */
#if defined(LOGOS_HAVE_SSE2) && !defined(LOGOS_NO_SIMD_MERGE)
#define LOGOS_SIMD_MERGE 1
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#include <immintrin.h>
#define KM_CEX(a, b, lo, hi) do { __m256d m_ = _mm256_cmp_pd((b), (a), _CMP_LT_OQ); (lo) = _mm256_blendv_pd((a), (b), m_); (hi) = _mm256_blendv_pd((b), (a), m_); } while (0)
/* a bitonic 4-vector sorted ascending: (0,2)(1,3), then (0,1)(2,3). ONE mask per pair, permuted to both of its lanes: the lane
   that takes the smaller and the lane that takes the larger must agree on a tie, or +0.0 against -0.0 (or two equal values)
   comes out as the same operand twice. The first cut computed a predicate per lane and lost an element on every tie. */
static inline __m256d km_bitonic4(__m256d v) {
    __m256d s = _mm256_permute4x64_pd(v, 0x4E), m = _mm256_cmp_pd(s, v, _CMP_LT_OQ);   /* s = lanes 2 3 0 1; m0 = v2 < v0, m1 = v3 < v1 */
    v = _mm256_blendv_pd(v, s, _mm256_permute4x64_pd(m, 0x44));                         /* mask lanes 0 1 0 1 */
    s = _mm256_permute4x64_pd(v, 0xB1); m = _mm256_cmp_pd(s, v, _CMP_LT_OQ);           /* s = lanes 1 0 3 2; m0 = v1 < v0, m2 = v3 < v2 */
    return _mm256_blendv_pd(v, s, _mm256_permute4x64_pd(m, 0xA0));                      /* mask lanes 0 0 2 2 */
}
static inline void km_merge8(__m256d a, __m256d b, __m256d *lo, __m256d *hi) {   /* a, b ascending: the four smallest ascending, the rest ascending */
    __m256d l, h; b = _mm256_permute4x64_pd(b, 0x1B); KM_CEX(a, b, l, h); *lo = km_bitonic4(l); *hi = km_bitonic4(h);
}
#define KM_LD_UP(v, k) _mm256_loadu_pd((v) + (k))
#define KM_LD_DN(v, nv, k) _mm256_permute4x64_pd(_mm256_loadu_pd((v) + (nv) - 4 - (k)), 0x1B)   /* elements k..k+3, ascending, of a strand stored descending */
/* Two chains at once (the second cut): the first cut carried one vector between iterations and was latency-bound on it, 1.14-1.29x
   over the scalar merge against a prediction of 1.5-2.0. A merge has two ends: the front chain stores the four smallest and
   carries the four largest, loading from the strand whose HEAD is smaller; the back chain, independent of it, stores the four
   largest at the far end of the output and carries the four smallest, loading from the strand whose TAIL is larger. They run in
   lockstep until a strand has fewer than eight unconsumed elements; the two carries and the remainders (one of them under eight)
   fill the gap between the two output cursors with a small scalar merge. */
#define KNOT_MERGE2V_DEF(name, XAT, YAT, XLD, YLD, XCPY, YCPY) \
static void name(const double *x, size_t nx, const double *y, size_t ny, double *o) { \
    size_t i = 0, j = 0, ib = 0, jb = 0, nc = 0; double c[8], *of = o, *ob = o + nx + ny; \
    if (nx >= 8 && ny >= 8) { \
        __m256d va = XLD(0), vb = YLD(0), wa = XLD(nx - 4), wb = YLD(ny - 4), lo, hi, lo2, hi2; i = j = ib = jb = 4; \
        for (;;) { \
            km_merge8(va, vb, &lo, &hi); _mm256_storeu_pd(of, lo); of += 4; \
            km_merge8(wa, wb, &lo2, &hi2); ob -= 4; _mm256_storeu_pd(ob, hi2); \
            if (i + ib + 8 > nx || j + jb + 8 > ny) break; \
            long long tf = XAT(i) <= YAT(j), tb = XAT(nx - 1 - ib) >= YAT(ny - 1 - jb); \
            __m256d cx = XLD(i), cy = YLD(j), dx = XLD(nx - 4 - ib), dy = YLD(ny - 4 - jb); \
            va = _mm256_blendv_pd(cy, cx, _mm256_castsi256_pd(_mm256_set1_epi64x(-tf))); vb = hi; i += 4 * (size_t)tf; j += 4 * (size_t)!tf; \
            wa = _mm256_blendv_pd(dy, dx, _mm256_castsi256_pd(_mm256_set1_epi64x(-tb))); wb = lo2; ib += 4 * (size_t)tb; jb += 4 * (size_t)!tb; \
        } \
        { double f[4], b[4]; _mm256_storeu_pd(f, hi); _mm256_storeu_pd(b, lo2); size_t p = 0, q = 0;   /* the two carries, one sorted list of eight */ \
          while (p < 4 && q < 4) { int tq = b[q] < f[p]; c[nc++] = tq ? b[q] : f[p]; q += (size_t)tq; p += (size_t)!tq; } \
          while (p < 4) { c[nc++] = f[p++]; } while (q < 4) { c[nc++] = b[q++]; } } \
    } \
    size_t xe = nx - ib, ye = ny - jb; double t[15]; size_t nt = 0, p = 0;   /* the gap [of, ob) holds the carries and both remainders */ \
    if (xe - i <= ye - j) { size_t q = i; while (p < nc && q < xe) { double cv = c[p], xv = XAT(q); int ty = xv < cv; t[nt++] = ty ? xv : cv; q += (size_t)ty; p += (size_t)!ty; } \
                            while (p < nc) { t[nt++] = c[p++]; } while (q < xe) { t[nt++] = XAT(q++); } \
                            p = 0; while (p < nt && j < ye) { double tv = t[p], yv = YAT(j); int ty = yv < tv; *of++ = ty ? yv : tv; j += (size_t)ty; p += (size_t)!ty; } \
                            while (p < nt) { *of++ = t[p++]; } if (j < ye) { YCPY(of, j, ye - j); } } \
    else                  { size_t q = j; while (p < nc && q < ye) { double cv = c[p], yv = YAT(q); int ty = yv < cv; t[nt++] = ty ? yv : cv; q += (size_t)ty; p += (size_t)!ty; } \
                            while (p < nc) { t[nt++] = c[p++]; } while (q < ye) { t[nt++] = YAT(q++); } \
                            p = 0; while (p < nt && i < xe) { double tv = t[p], xv = XAT(i); int tk = tv < xv; *of++ = tk ? tv : xv; i += (size_t)!tk; p += (size_t)tk; } \
                            while (p < nt) { *of++ = t[p++]; } if (i < xe) { XCPY(of, i, xe - i); } } \
    (void)ob; }
#define KMXU(k) x[k]
#define KMXD(k) x[nx - 1 - (k)]
#define KMYU(k) y[k]
#define KMYD(k) y[ny - 1 - (k)]
#define KMLXU(k) KM_LD_UP(x, k)
#define KMLXD(k) KM_LD_DN(x, nx, k)
#define KMLYU(k) KM_LD_UP(y, k)
#define KMLYD(k) KM_LD_DN(y, ny, k)
#define KMCXU(oo, k, c) memcpy((oo), x + (k), (c) * sizeof(double))
#define KMCXD(oo, k, c) copy_rev((oo), x + nx - (k) - (c), (c))
#define KMCYU(oo, k, c) memcpy((oo), y + (k), (c) * sizeof(double))
#define KMCYD(oo, k, c) copy_rev((oo), y + ny - (k) - (c), (c))
KNOT_MERGE2V_DEF(knot_merge2v_uu, KMXU, KMYU, KMLXU, KMLYU, KMCXU, KMCYU)
KNOT_MERGE2V_DEF(knot_merge2v_du, KMXD, KMYU, KMLXD, KMLYU, KMCXD, KMCYU)
KNOT_MERGE2V_DEF(knot_merge2v_ud, KMXU, KMYD, KMLXU, KMLYD, KMCXU, KMCYD)
KNOT_MERGE2V_DEF(knot_merge2v_dd, KMXD, KMYD, KMLXD, KMLYD, KMCXD, KMCYD)
#undef KNOT_MERGE2V_DEF
#undef KMXU
#undef KMXD
#undef KMYU
#undef KMYD
#undef KMLXU
#undef KMLXD
#undef KMLYU
#undef KMLYD
#undef KMCXU
#undef KMCXD
#undef KMCYU
#undef KMCYD
#pragma GCC pop_options
#endif
static int g_simd_merge = 0;              /* the vector kernel for this sort: AVX2 + FMA at run time and LOGOS_NO_SIMD_MERGE unset; read per sort from MIN_PARALLEL up */
static atomic_size_t g_simd_merges;       /* logos_twist_last(13): two-strand merges taken by the vector kernel; reading clears it */
static atomic_size_t g_knot_gallops;   /* logos_twist_last(5): gallops made since it was last read (one add per placement worker) */
static size_t knot_merge2x(const double *x, size_t nx, int xr, const double *y, size_t ny, int yr, double *o) {
    if (g_knot_gallop && !g_twist_generic && (nx >= KNOT_GALLOP_RATIO * ny || ny >= KNOT_GALLOP_RATIO * nx)) {
        if (!xr && !yr) return knot_merge2g_uu(x, nx, y, ny, o);
        if (xr && yr) return knot_merge2g_dd(x, nx, y, ny, o);
        return xr ? knot_merge2g_du(x, nx, y, ny, o) : knot_merge2g_ud(x, nx, y, ny, o);
    }
#ifdef LOGOS_SIMD_MERGE
    if (g_simd_merge && !g_twist_generic) {
        atomic_fetch_add_explicit(&g_simd_merges, 1, memory_order_relaxed);
        if (!xr && !yr) knot_merge2v_uu(x, nx, y, ny, o); else if (xr && yr) knot_merge2v_dd(x, nx, y, ny, o);
        else if (xr) knot_merge2v_du(x, nx, y, ny, o); else knot_merge2v_ud(x, nx, y, ny, o);
        return 0;
    }
#endif
    if (!xr && !yr) { knot_merge2(x, nx, y, ny, o); return 0; }
    if (!g_twist_generic) { if (xr && yr) knot_merge2_dd(x, nx, y, ny, o); else if (xr) knot_merge2_du(x, nx, y, ny, o); else knot_merge2_ud(x, nx, y, ny, o); return 0; }
    ptrdiff_t ix = xr ? (ptrdiff_t)nx - 1 : 0, iy = yr ? (ptrdiff_t)ny - 1 : 0, sx = xr ? -1 : 1, sy = yr ? -1 : 1; size_t i = 0, j = 0;
    while (i < nx && j < ny) { double xv = x[ix], yv = y[iy]; int ty = yv < xv; *o++ = ty ? yv : xv;
        iy += sy & -(ptrdiff_t)ty; ix += sx & -(ptrdiff_t)!ty; j += (size_t)ty; i += (size_t)!ty; }
    for (; i < nx; i++, ix += sx) *o++ = x[ix];
    for (; j < ny; j++, iy += sy) *o++ = y[iy];
    return 0;
}
#ifdef LOGOS_MERGE_HOOK
/* make mergebench: the two-strand merge kernels in isolation, the gallop held off so that the choice is scalar against vector */
void logos_merge2_hook(const double *x, size_t nx, int xr, const double *y, size_t ny, int yr, double *o, int simd) {
    int s = g_simd_merge, g = g_knot_gallop; g_simd_merge = simd; g_knot_gallop = 0;
    knot_merge2x(x, nx, xr, y, ny, yr, o);
    g_simd_merge = s; g_knot_gallop = g;
}
#endif
/* ---- the k-way merge in vectors (2026-10-21, experiments/OTHER_FIELDS.md; fieldprobe weave and landauer) ----
 * merge_all_runs merges with branchy scalar kernels (a four-way hybrid, a galloping two-way merge) and copies every level back.
 * On strands that interleave element by element -- a crossing of three or more live strands in the knot route, the leaf's run
 * merge on runs that each span the range -- each of those branches is a coin flip: four sorted strands laid end to end cost
 * 14.3 ns per element at one thread, MORE than random data (8.9). Here adjacent pairs are merged level by level by knot_merge2x
 * (the two-chain vector kernel; the galloping one where a piece is four times the other), ping-ponging between the array and
 * the scratch, with one copy back if the result ends in the scratch. Runs must be contiguous and in order. Relocation's chain
 * merges keep merge_all_runs: blocks that overlap only in part are what galloping is for. Returns 0, touching nothing, where
 * the vector kernel is off: LOGOS_RUNMERGE_SCALAR=1 (the A/B), LOGOS_NO_SIMD_MERGE=1, or no AVX2. */
static int g_runmerge_vec = 1;
static atomic_size_t g_runmerge_vecs;    /* logos_twist_last(32): k-way merges done in vectors; reading clears it */
static __attribute__((noinline)) int merge_runs_vec(double *a, double *buf, const size_t *runs, size_t k) {
#ifdef LOGOS_SIMD_MERGE
    if (k < 2 || !g_simd_merge || !g_runmerge_vec || g_twist_generic) return 0;
    for (size_t i = 1; i < k; i++) if (runs[2 * i] != runs[2 * i - 1] + 1) return 0;
    size_t stk[MERGE_STACK_RUNS + 1], *bd = k <= MERGE_STACK_RUNS ? stk : malloc((k + 1) * sizeof(size_t));
    if (!bd) return 0;
    for (size_t i = 0; i < k; i++) bd[i] = runs[2 * i];
    bd[k] = runs[2 * k - 1] + 1;                                       /* run i is [bd[i], bd[i + 1]) */
    double *src = a, *dst = buf; size_t m = k, gl = 0;
    while (m > 1) {
        size_t w = 0;
        for (size_t i = 0; i < m; i += 2) {                            /* writes go to bd[w], w <= i / 2: nothing still to be read */
            if (i + 1 < m) gl += knot_merge2x(src + bd[i], bd[i + 1] - bd[i], 0, src + bd[i + 1], bd[i + 2] - bd[i + 1], 0, dst + bd[i]);
            else memcpy(dst + bd[i], src + bd[i], (bd[i + 1] - bd[i]) * sizeof(double));
            bd[w++] = bd[i];
        }
        bd[w] = bd[m]; m = w;
        double *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a + bd[0], src + bd[0], (bd[1] - bd[0]) * sizeof(double));
    if (gl) atomic_fetch_add_explicit(&g_knot_gallops, gl, memory_order_relaxed);
    if (bd != stk) free(bd);
    atomic_fetch_add_explicit(&g_runmerge_vecs, 1, memory_order_relaxed);
    return 1;
#else
    (void)a; (void)buf; (void)runs; (void)k; return 0;
#endif
}
/* [start,end] pairs of every piece in output coordinates, the form merge_all_runs takes (used by k >= 3 crossings only) */
static size_t *knot_rl(const knot_plan_t *K) {
    size_t *rl = malloc(2 * (K->npc ? K->npc : 1) * sizeof(size_t)); if (!rl) return NULL;
    for (size_t q = 0; q < K->npc; q++) { rl[2 * q] = K->pc[q].dst; rl[2 * q + 1] = K->pc[q].dst + K->pc[q].len - 1; }
    return rl;
}
/* sequential placement into buf. Order matters: every read of a[] (copies, two-strand merges) comes before the
 * k >= 3 merges, which use a[] as scratch. */
static void knot_place_seq(double *a, double *buf, const knot_plan_t *K, const size_t *rl) {
    for (size_t q = 0; q < K->npc; q++) if (K->pc[q].copy) strand_copy(buf + K->pc[q].dst, a + K->pc[q].src, K->pc[q].len, K->pc[q].rev);
    size_t gl = 0;
    for (size_t s = 0; s < K->nsg; s++) if (K->sg[s].k == 2) { const kpiece_t *x = &K->pc[K->sg[s].p0], *y = x + 1; gl += knot_merge2x(a + x->src, x->len, x->rev, a + y->src, y->len, y->rev, buf + K->sg[s].dst); }
    if (gl) atomic_fetch_add_explicit(&g_knot_gallops, gl, memory_order_relaxed);
    if (K->n3) for (size_t s = 0; s < K->nsg; s++) if (K->sg[s].k >= 3) {
        const size_t *r = rl + 2 * K->sg[s].p0;
        if (!merge_runs_vec(buf, a, r, K->sg[s].k)) merge_all_runs(buf, a, r, K->sg[s].k);
    }
}
/* ===================== HALF THE SCRATCH (2026-10-24, the second memory round, FINDINGS 3bq) =====================
 * A sequential placement that writes every piece into an n-element scratch and then copies the scratch back holds TWO copies of
 * the data at its peak: 8 bytes per element above the array on every block shape at one thread (make rssprobe). Half of the
 * scratch does the same work in the same number of moves:
 *   1. every job (a piece copied, two pieces merged, k >= 3 pieces staged for a merge) whose output lies in the LOWER half [0, H)
 *      is written into buf[0, H), reading a[] as it lies;
 *   2. the pieces the upper half reads must lie clear of [H, N). Those already below H stay, and those reaching H or above are
 *      copied into the gaps the lower half's sources left (half_fit, planned before anything moves); failing that, every piece
 *      slides LEFT, in address order, into [0, N - H): each lands at or below its own address and past every piece already
 *      moved, so a memmove never covers anything unread, and N - H <= H keeps them clear of [H, N);
 *   3. the lower k >= 3 merges run in buf with the part of a[] nothing lives in now as their scratch ([H, N), or [N - H, N));
 *   4. every upper job writes a[H, N) from the slid pieces, and the upper k >= 3 merges then take a[0, N - H) as their scratch;
 *   5. buf[0, H) is copied to a[0, H) -- or, with strays, merged with them BACKWARDS into a[0, N + S), which needs no room at all.
 * Moves: H + (what slides, at most N - H) + (N - H) + H, against N + N; the scratch touched is H. H = ceil(N / 2), and a job that
 * straddles it is split: a copy at H, a two-way merge at its merge path, a k >= 3 merge by a selection on values (half_pathk), so
 * one crossing of every strand (four interleaved strands, eight concatenated logs) is halved too; only past HALF_KMAX strands
 * does H move to the job's end (and when that is N the placement declines having touched nothing). Sequential only: step 2
 * is one pass in address order. LOGOS_PLACE_FULL=1: the full scratch, for the A/B;
 * logos_twist_last(34) counts the placements made this way. */
#ifndef HALF_MIN_N
#define HALF_MIN_N 65536
#endif
/* the placements stream (place_copy, defined with relocation below): every copy here that does not overlap itself streams too.
   The first cut used memcpy and memmove, which fetch each destination line before writing it, and read 0.85-0.91 against the
   full scratch on block shapes at one thread (roundbench, tsbench): the full scratch's two passes stream, so it moved 32 bytes
   per element where the half one moved about 48. */
static void half_copy(double *dst, const double *src, size_t k, int nt); static void half_fence(int nt); static int half_nt_ok(size_t n);
static atomic_size_t g_half_places;      /* logos_twist_last(34): placements made with half a scratch; reading clears it */
typedef struct { size_t src, len, dst; int rev; } hpc_t;   /* a piece: src its lowest address; dst where it lands (a copy) or is staged (k >= 3) */
typedef struct { size_t dst, len, p0, k; } hjob_t;          /* output [dst, dst + len): k == 1 a copy of piece p0, k == 2 a merge of p0 and p0 + 1, k >= 3 staged, then merged */
typedef struct { size_t src, i; } hord_t;
static int half_cmp(const void *p, const void *q) { size_t x = ((const hord_t *)p)->src, y = ((const hord_t *)q)->src; return (x > y) - (x < y); }
static inline hpc_t half_sub(const hpc_t *x, size_t c0, size_t c1, size_t dst) {   /* the logical part [c0, c1) of a piece, a piece of its own */
    hpc_t r = { KNOT_SRC(x->src, x->len, x->rev, c0, c1), c1 - c0, dst, x->rev }; return r;
}
/* how many of the t smallest elements of a two-piece merge come from x: the merge path, both pieces read in value order */
static size_t half_path(const double *a, const hpc_t *x, const hpc_t *y, size_t t) {
    size_t lo = t > y->len ? t - y->len : 0, hi = t < x->len ? t : x->len;
    while (lo < hi) {
        size_t m = lo + ((hi - lo) >> 1);
        if (KNOT_AT(a, x->src, x->len, x->rev, m) < KNOT_AT(a, y->src, y->len, y->rev, t - m - 1)) lo = m + 1; else hi = m;
    }
    return lo;
}
/* the same for k >= 3 pieces: c[q] of the t smallest come from piece q, every element taken no larger than any left. A binary search on
 * VALUES: a value from the piece with the widest open range splits every piece by two binary searches (elements below it, and up to
 * it); t below the first count closes every range from above, t past the second from below, and between the two the cut is the
 * elements below plus as many equal ones as t needs. The widest range shrinks by half or more each round. */
#ifndef HALF_KMAX
#define HALF_KMAX 64
#endif
static size_t half_rank(const double *a, const hpc_t *x, double v, int le) {      /* elements of x below v (le: up to v) */
    size_t lo = 0, hi = x->len;
    while (lo < hi) { size_t m = lo + ((hi - lo) >> 1); double e = KNOT_AT(a, x->src, x->len, x->rev, m); if (le ? !(v < e) : e < v) lo = m + 1; else hi = m; }
    return lo;
}
static void half_pathk(const double *a, const hpc_t *x, size_t k, size_t t, size_t *c) {
    size_t lo[HALF_KMAX], hi[HALF_KMAX], Lq[HALF_KMAX], Uq[HALF_KMAX];
    for (size_t q = 0; q < k; q++) { lo[q] = 0; hi[q] = x[q].len; }
    for (;;) {
        size_t big = 0, w = 0, L = 0, U = 0;
        for (size_t q = 0; q < k; q++) if (hi[q] - lo[q] > w) { w = hi[q] - lo[q]; big = q; }
        if (w == 0) break;
        double v = KNOT_AT(a, x[big].src, x[big].len, x[big].rev, lo[big] + w / 2);
        for (size_t q = 0; q < k; q++) { Lq[q] = half_rank(a, &x[q], v, 0); Uq[q] = half_rank(a, &x[q], v, 1); L += Lq[q]; U += Uq[q]; }
        if (t < L) { for (size_t q = 0; q < k; q++) if (Lq[q] < hi[q]) hi[q] = Lq[q]; }
        else if (t > U) { for (size_t q = 0; q < k; q++) if (Uq[q] > lo[q]) lo[q] = Uq[q]; }
        else { size_t need = t - L; for (size_t q = 0; q < k; q++) { size_t e = Uq[q] - Lq[q], tk = need < e ? need : e; c[q] = Lq[q] + tk; need -= tk; } return; }
    }
    for (size_t q = 0; q < k; q++) c[q] = lo[q];
}
/* one job's reads, written at out + its output offsets: out is buf for the lower half and a[] itself for the upper */
static inline void half_piece(double *o, const double *x, size_t cnt, int rev, int nt) { if (rev) copy_rev(o, x, cnt); else half_copy(o, x, cnt, nt); }
static size_t half_job(double *out, const double *a, const hpc_t *pc, const hjob_t *j, int nt) {
    const hpc_t *x = &pc[j->p0];
    if (j->k == 1) { half_piece(out + j->dst, a + x->src, x->len, x->rev, nt); return 0; }
    if (j->k == 2) return knot_merge2x(a + x->src, x->len, x->rev, a + x[1].src, x[1].len, x[1].rev, out + j->dst);
    for (size_t q = 0; q < j->k; q++) half_piece(out + x[q].dst, a + x[q].src, x[q].len, x[q].rev, nt);
    return 0;
}
/* a staged k >= 3 job, merged where it lies with scr[0, len) as its scratch */
static void half_mergek(double *out, double *scr, const hpc_t *pc, const hjob_t *j, size_t *rl) {
    for (size_t q = 0; q < j->k; q++) { rl[2 * q] = pc[j->p0 + q].dst - j->dst; rl[2 * q + 1] = rl[2 * q] + pc[j->p0 + q].len - 1; }
    if (!merge_runs_vec(out + j->dst, scr, rl, j->k)) merge_all_runs(out + j->dst, scr, rl, j->k);
}
/* step 5 with strays: the placed elements are buf[0, H) then a[H, N), st[0, S) is sorted, and the n = N + S elements are merged
 * from the top down. Everything above a stray moves right by the strays still to come, so the writes land at or above every
 * unread position; each stray finds its place by a gallop down from where the last one went. */
#define HALF_AT(m) ((m) < H ? buf[m] : a[m])
static void half_strays(double *a, const double *buf, size_t H, size_t N, const double *st, size_t S, int nt) {
    size_t i = N;
    for (size_t q = S; q-- > 0;) {
        double v = st[q]; size_t p;
        if (i == 0 || !(v < HALF_AT(i - 1))) p = i;
        else {
            size_t top = i - 1, step = 1, lo;                             /* HALF_AT(top) > v: find the first index whose element is > v */
            for (;;) { if (step > top) { lo = 0; break; } if (!(v < HALF_AT(top - step))) { lo = top - step + 1; break; } top -= step; step <<= 1; }
            size_t hi = top;
            while (lo < hi) { size_t m = lo + ((hi - lo) >> 1); if (v < HALF_AT(m)) hi = m; else lo = m + 1; }
            p = lo;
        }
        if (i > H && p < i) { size_t s0 = p > H ? p : H; memmove(a + s0 + q + 1, a + s0, (i - s0) * sizeof(double)); }   /* the part in a[] first: */
        if (p < H && p < i) { size_t e0 = i < H ? i : H; half_copy(a + p + q + 1, buf + p, e0 - p, nt); }                 /* it moves right of this */
        a[p + q] = v; i = p;
    }
    half_copy(a, buf, i < H ? i : H, nt);                                 /* what is left below every stray; a[H, i) is in place */
    half_fence(nt);
}
#undef HALF_AT
/* step 2, the cheap way: pieces already below H stay where they lie, and only those that reach H or above are copied into the gaps
 * the lower half's sources left below H, largest first, first fit. Planned before anything moves: returns 0 having moved nothing
 * when a piece finds no gap, when there are too many pieces to plan, or when a lower k >= 3 merge is longer than [H, N), which is
 * its scratch on this path. A gap excludes every upper piece's range, so no copy covers anything unread, in any order. */
#ifndef HALF_FIT_MAX
#define HALF_FIT_MAX 1024
#endif
static int half_fit(double *a, hpc_t *pc, const hord_t *ord, size_t m, size_t H, size_t N, const hjob_t *jb, size_t nj, int nt) {
    if (m > HALF_FIT_MAX) return 0;
    for (size_t q = 0; q < nj; q++) if (jb[q].dst < H && jb[q].k >= 3 && jb[q].len > N - H) return 0;
    size_t *gs = malloc(4 * (m + 1) * sizeof(size_t)); if (!gs) return 0;
    size_t *ge = gs + (m + 1), *mv = ge + (m + 1), *to = mv + (m + 1), ng = 0, nm = 0, prev = 0;
    for (size_t r = 0; r < m; r++) {                                   /* gaps: [0, H) less every upper piece (ord is by address) */
        const hpc_t *p = &pc[ord[r].i];
        if (p->src + p->len > H) mv[nm++] = ord[r].i;
        if (p->src >= H) continue;
        if (p->src > prev) { gs[ng] = prev; ge[ng] = p->src; ng++; }
        prev = p->src + p->len < H ? p->src + p->len : H;
    }
    if (prev < H) { gs[ng] = prev; ge[ng] = H; ng++; }
    for (size_t x = 1; x < nm; x++) { size_t v = mv[x], y = x; while (y > 0 && pc[mv[y - 1]].len < pc[v].len) { mv[y] = mv[y - 1]; y--; } mv[y] = v; }
    for (size_t x = 0; x < nm; x++) {
        size_t L = pc[mv[x]].len, g = 0;
        while (g < ng && ge[g] - gs[g] < L) g++;
        if (g == ng) { free(gs); return 0; }                             /* nothing has moved */
        to[x] = gs[g]; gs[g] += L;
    }
    for (size_t x = 0; x < nm; x++) { hpc_t *p = &pc[mv[x]]; half_copy(a + to[x], a + p->src, p->len, nt); p->src = to[x]; }
    free(gs); return 1;
}
/* pc[] has room for 2 * HALF_KMAX + 4 more pieces and jb[] for one more job (a split adds them). st is sorted. Returns 0 having touched
 * nothing in a[] or buf when no midpoint below N can be had or memory runs out. */
static int half_place(double *a, size_t N, double *buf, hpc_t *pc, size_t npc, hjob_t *jb, size_t nj, const double *st, size_t S) {
    size_t H = (N + 1) / 2, js = SIZE_MAX;
    for (size_t q = 0; q < nj; q++) if (jb[q].dst < H && jb[q].dst + jb[q].len > H) { js = q; break; }
    if (js != SIZE_MAX) {
        hjob_t *j = &jb[js]; size_t t = H - j->dst;
        if (j->k > HALF_KMAX) { H = j->dst + j->len; if (H >= N) return 0; }      /* too many strands to split: H moves to its end */
        else if (j->k >= 3) {                                                    /* split by value: two staged jobs (or fewer pieces) */
            size_t c[HALF_KMAX], k = j->k, p0 = j->p0, d0 = j->dst, len = j->len, nl = 0, nh = 0, lb = npc, hb, o = d0;
            half_pathk(a, pc + p0, k, t, c);
            for (size_t q = 0; q < k; q++) if (c[q]) { hpc_t x = pc[p0 + q]; pc[npc++] = half_sub(&x, 0, c[q], o); o += c[q]; nl++; }
            hb = npc; o = H;
            for (size_t q = 0; q < k; q++) if (c[q] < pc[p0 + q].len) { hpc_t x = pc[p0 + q]; pc[npc++] = half_sub(&x, c[q], x.len, o); o += x.len - c[q]; nh++; }
            hjob_t lo = { d0, t, lb, nl }, hi = { H, len - t, hb, nh };
            jb[js] = lo; jb[nj++] = hi;
        }
        else if (j->k == 1) {
            const hpc_t x = pc[j->p0];
            pc[npc] = half_sub(&x, 0, t, j->dst); pc[npc + 1] = half_sub(&x, t, x.len, H);
            hjob_t lo = { j->dst, t, npc, 1 }, hi = { H, x.len - t, npc + 1, 1 };
            jb[js] = lo; jb[nj++] = hi; npc += 2;
        } else {
            const hpc_t x = pc[j->p0], y = pc[j->p0 + 1];
            size_t i = half_path(a, &x, &y, t), k = t - i, base = npc, len = j->len, d0 = j->dst;
            hjob_t lo, hi;
            if (i && k) { pc[npc++] = half_sub(&x, 0, i, 0); pc[npc++] = half_sub(&y, 0, k, 0); lo = (hjob_t){ d0, t, base, 2 }; }
            else { pc[npc++] = i ? half_sub(&x, 0, i, d0) : half_sub(&y, 0, k, d0); lo = (hjob_t){ d0, t, base, 1 }; }
            base = npc;
            if (i < x.len && k < y.len) { pc[npc++] = half_sub(&x, i, x.len, 0); pc[npc++] = half_sub(&y, k, y.len, 0); hi = (hjob_t){ H, len - t, base, 2 }; }
            else { pc[npc++] = i < x.len ? half_sub(&x, i, x.len, H) : half_sub(&y, k, y.len, H); hi = (hjob_t){ H, len - t, base, 1 }; }
            jb[js] = lo; jb[nj++] = hi;
        }
    }
    size_t nu = 0, kmax = 2;
    for (size_t q = 0; q < nj; q++) { if (jb[q].k > kmax) kmax = jb[q].k; if (jb[q].dst >= H) nu += jb[q].k; }
    hord_t *ord = malloc((nu ? nu : 1) * sizeof *ord); size_t *rl = malloc(2 * kmax * sizeof(size_t));
    if (!ord || !rl) { free(ord); free(rl); return 0; }
    size_t gl = 0, m = 0, w = 0; int nt = half_nt_ok(N);
    for (size_t q = 0; q < nj; q++) if (jb[q].dst < H) gl += half_job(buf, a, pc, &jb[q], nt);                /* 1 */
    for (size_t q = 0; q < nj; q++) if (jb[q].dst >= H) for (size_t r = 0; r < jb[q].k; r++) { ord[m].src = pc[jb[q].p0 + r].src; ord[m].i = jb[q].p0 + r; m++; }
    qsort(ord, m, sizeof *ord, half_cmp);
    int fitted = half_fit(a, pc, ord, m, H, N, jb, nj, nt);                                                     /* 2: gaps first */
    if (!fitted) for (size_t r = 0; r < m; r++) {                                                               /* 2: else the slide */
        hpc_t *p = &pc[ord[r].i];
        if (p->src != w) { if (w + p->len <= p->src) half_copy(a + w, a + p->src, p->len, nt); else memmove(a + w, a + p->src, p->len * sizeof(double)); }
        p->src = w; w += p->len;
    }
    half_fence(nt);                                                                                             /* what streamed is read below */
    for (size_t q = 0; q < nj; q++) if (jb[q].dst < H && jb[q].k >= 3) half_mergek(buf, fitted ? a + H : a + (N - H), pc, &jb[q], rl);   /* 3 */
    for (size_t q = 0; q < nj; q++) if (jb[q].dst >= H) gl += half_job(a, a, pc, &jb[q], nt);                  /* 4 */
    half_fence(nt);
    for (size_t q = 0; q < nj; q++) if (jb[q].dst >= H && jb[q].k >= 3) half_mergek(a, a, pc, &jb[q], rl);
    if (S) half_strays(a, buf, H, N, st, S, nt); else { half_copy(a, buf, H, nt); half_fence(nt); }            /* 5 */
    free(ord); free(rl);
    if (gl) atomic_fetch_add_explicit(&g_knot_gallops, gl, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_half_places, 1, memory_order_relaxed);
    return 1;
}
/* the knot plan as jobs (in output order), then half_place */
static int knot_place_half(double *a, double *buf, size_t N, const knot_plan_t *K, const double *st, size_t S) {
    hpc_t *pc = malloc((K->npc + 2 * HALF_KMAX + 4) * sizeof *pc); hjob_t *jb = malloc((K->npc + 1) * sizeof *jb);   /* at most one job per piece */
    if (!pc || !jb) { free(pc); free(jb); return 0; }
    for (size_t q = 0; q < K->npc; q++) { hpc_t p = { K->pc[q].src, K->pc[q].len, K->pc[q].dst, K->pc[q].rev }; pc[q] = p; }
    size_t nj = 0, s = 0;
    for (size_t q = 0; q < K->npc; ) {
        if (s < K->nsg && K->sg[s].p0 == q) { hjob_t j = { K->sg[s].dst, K->sg[s].len, q, K->sg[s].k }; jb[nj++] = j; q += K->sg[s].k; s++; }
        else { hjob_t j = { K->pc[q].dst, K->pc[q].len, q, 1 }; jb[nj++] = j; q++; }
    }
    int ok = half_place(a, N, buf, pc, K->npc, jb, nj, st, S);
    free(pc); free(jb); return ok;
}
/* two sorted pieces lying side by side, [0, d) and [d, n): the tail and head routes' vector merge with half its scratch */
static __attribute__((unused)) int half_two(double *a, size_t n, size_t d, double *buf) {
    hpc_t pc[6] = { { 0, d, 0, 0 }, { d, n - d, 0, 0 } }; hjob_t jb[2] = { { 0, n, 0, 2 } };
    return half_place(a, n, buf, pc, 2, jb, 1, NULL, 0);
}
static int half_on(size_t n) { return n >= HALF_MIN_N && !getenv("LOGOS_PLACE_FULL"); }
#endif

/* Sequential relocation for the leaf dispatcher: copies every run into buf in
 * chain order, then merges inside multi-run chains with a[] as scratch (all
 * runs have already been copied out, so a[] is free). Returns buf, or NULL when
 * the gate declines (caller merges in place as before). */
static double *runs_relocate_f64(double *a, size_t n, double *buf, const size_t *runs, size_t n_runs) {
    rdesc_t *d; size_t *ch, nc, merged;
    if (!runs_plan(a, runs, n_runs, &d, &ch, &nc, &merged)) return NULL;
#ifndef LOGOS_NO_KNOT
    if (g_knot_on && n >= (size_t)KNOT_MIN_AVG_RUN * n_runs) {   /* finer than chains: copy what lies alone, merge only what crosses */
        knot_plan_t K;
        if (knot_plan(a, d, n_runs, n + 1, 1, &K)) {
            double kf = n_runs > MERGE_MAX_RUNS ? g_knot_frac_seq : g_knot_frac_few;   /* few runs: the caller's run merge is the fallback */
            int go = K.total == n && ((double)K.cost <= kf * (double)n || merged <= n / 2);
            size_t *krl = NULL; if (go && K.n3) { krl = knot_rl(&K); if (!krl) go = 0; }
            if (go) { knot_place_seq(a, buf, &K, krl); free(krl); knot_plan_free(&K); free(d); free(ch); return buf; }
            free(krl); knot_plan_free(&K);
        }
    }
#endif
    if (merged > n / 2) { free(d); free(ch); return NULL; }
    size_t *rl = malloc(2 * n_runs * sizeof(size_t)); if (!rl) { free(d); free(ch); return NULL; }
    for (size_t q = 0, o = 0; q < n_runs; q++) { rl[2 * q] = o; rl[2 * q + 1] = o + d[q].len - 1; strand_copy(buf + o, a + d[q].start, d[q].len, d[q].rev); o += d[q].len; }
    for (size_t k = 0; k < nc; k++) if (ch[k + 1] - ch[k] > 1) merge_all_runs(buf, a, rl + 2 * ch[k], ch[k + 1] - ch[k]);
    free(rl); free(d); free(ch); return buf;
}
#endif

/* ---- quantized-float radix (typed path) --------------------------------- */
static inline void key_parts(double v, uint32_t *tl, uint32_t *th) {
    uint64_t u; memcpy(&u, &v, 8); uint32_t lo = (uint32_t)u, hi = (uint32_t)(u >> 32);
    if (hi >> 31) { *tl = ~lo; *th = ~hi; } else { *tl = lo; *th = hi | 0x80000000u; }
}
/* The probe's positions are the engine's Weyl positions (SAMPLE_POS), like every other sampler since the chaos round. It read every
   (n/4096)-th element until 2026-10-20, and a STRIDE aliases with a period: four interleaved streams at 4M (period 4, stride 1024)
   showed it stream 0 alone, small integers with constant low bytes, so it reported 4 radix passes, and the full float radix then
   ran 6-8 passes over data the leaf sorts 2.6x faster (symprobe: the same array negated and reversed was not "quantized", because
   its stride landed on stream 3). LOGOS_QUANT_STRIDED=1: the stride, for the A/B (read once per sort, with the leaf's switches). */
static int g_quant_strided = 0;
/* THE QUANTIZED FLOAT RADIX IS OFF (2026-10-20). leaf_f64 used to send every range of TYPED_MIN_N..RADIX64_MAX_N whose sample varied in
   at most QUANT_PROBE_MAX key bytes to radix_f64_full, a JS-era path (port of sort_fn.f64) that predates the AVX2 leaf. edgeprobe found
   the cliff where it starts: one element across 65536 cost near-sorted data 4.2x per element, zipf integers 2.2x, quantized 1.75x; and
   past RADIX64_MAX_N, where it stops, near-sorted data got 2.8x cheaper. Paired with it off (roundbench, 15 pairs): quantized 1.40x at
   262k and 1.41x at 4M, near_sorted 1.93x at 262k, nothing the radix won at 1 or 14 threads. LOGOS_QUANT_RADIX=1 turns it back on. */
static int g_quant_radix = 0;
static int quant_probe_f64(const double *a, size_t n) {
    size_t m = n < 4096 ? n : 4096, step = n / m;
    uint32_t or_h = 0, and_h = 0xFFFFFFFFu, or_l = 0, and_l = 0xFFFFFFFFu;
    if (g_quant_strided) { for (size_t i = 0, p = 0; i < m; i++, p += step) { uint32_t tl, th; key_parts(a[p], &tl, &th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; } }
    else for (size_t i = 0; i < m; i++) { size_t p = SAMPLE_POS(i, m, n); uint32_t tl, th; key_parts(a[p < n ? p : n - 1], &tl, &th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; }
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

#ifdef LOGOS_AVX2_LEAF
/* sort_fn's scalar loop on the first k elements: settled = it cleared all-integer, ascending AND descending (then mn < mx are
   the prefix's extremes). By value, like sf_profile_avx2, so sort_fn's own loop keeps its locals in registers. */
typedef struct { double mn, mx; int settled, flags; } sf_pre_t;     /* flags: 1 all-integer, 2 ascending, 4 descending, in the window */
static __attribute__((noinline)) sf_pre_t sf_prefix(const double *a, size_t k) {
    double mn = a[0], mx = a[0];
    int all_int = isfinite(a[0]) && a[0] == floor(a[0]) && !(a[0] == 0.0 && signbit(a[0])), asc = 1, desc = 1;
    for (size_t i = 1; i < k; i++) {
        double v = a[i];
        if (v < mn) mn = v; else if (v > mx) mx = v;
        if (all_int && !(isfinite(v) && v == floor(v) && !(v == 0.0 && signbit(v)))) all_int = 0;
        if (asc && v < a[i - 1]) asc = 0;
        if (desc && v > a[i - 1]) desc = 0;
    }
    sf_pre_t r = { mn, mx, !all_int && !asc && !desc, all_int | asc << 1 | desc << 2 };
    return r;
}
#endif
/* no knot route (and so no vector kernel): the leaf's run merge keeps merge_all_runs. Out here because -DLOGOS_NO_RELOCATE
   implies LOGOS_NO_KNOT and removes the whole relocation region, a stub inside it with the knot code. */
#ifdef LOGOS_NO_KNOT
static int merge_runs_vec(double *a, double *buf, const size_t *runs, size_t k) { (void)a; (void)buf; (void)runs; (void)k; return 0; }
#endif
/* ---- the dispatcher (port of sort_fn) ------------------------------------ */
/* Returns the buffer holding the sorted result: a (most paths) or buf (ping-pong paths). */
static double *sort_fn(double *a, size_t n, double *buf, ctx_t *c) {
    if (n < 2) return a;
    double mn = a[0], mx = a[0];
    /* -0.0 == floor(-0.0), but the counting and int32 radix paths below REBUILD values from integers and wrote it back
     * as +0.0, so the output was not a permutation of the input (harness signed zeros, zerodiag). A -0.0 is therefore not
     * an integer here, and data holding one takes a path that moves elements instead. */
    int all_int = isfinite(a[0]) && a[0] == floor(a[0]) && !(a[0] == 0.0 && signbit(a[0]));
    int asc = 1, desc = 1, sc = -1, deferred = 0, settled = 0;   /* sc: the AVX2 kernel's NaN / -0.0 verdict, -1 = unknown */
#ifdef LOGOS_AVX2_LEAF
    /* THE PROFILE PASS, VECTORIZED (sf_profile_avx2), OR LEFT TO THE KERNEL (lever C, av_qsort_fused). A 64-element prefix settles
       all-integer, ascending and descending on almost anything unsorted and non-integer; then min, max and the NaN-or-zero flag
       come from one AVX2 pass, or, where the AVX2 gate below will take the whole range (a sequential sort above the flash
       window), from the kernel's first partition, and this pass runs late only if the kernel is not taken or declines. An
       ascent and a descent in the prefix put two different values in it, so the PREFIX's mn < mx already and every `mx > mn`
       before that gate reads true. Anything the prefix does not settle (integers, sorted stretches) takes the loop below,
       unchanged: its locals are never address-taken (see sf_prof_t). */
    if (n >= SF_VEC_MIN_N && !g_no_frontfuse && avx2_leaf_on()) {
        sf_pre_t pre = sf_prefix(a, SF_PREFIX);
        /* THE FOUNDER EFFECT (fieldprobe founder, 2026-10-21): the prefix is only the first place to look for a counterexample.
           A sorted, integer or constant run at the front (a header block, the first rows of a table) settles nothing, and the
           whole range then paid the scalar profile loop AND the kernel's own scan: 11-19% on uniform, normal and quantized data
           against the same elements scattered. Each property is disproved by a counterexample ANYWHERE, so three more windows at
           n/4, n/2 and 3n/4 are asked and their flags ANDed: an ascent and a descent somewhere put two different values in the
           windows (mn < mx), as the prefix's did. LOGOS_SF_FRONT_ONLY=1: the prefix alone, for the A/B. */
        if (!pre.settled && n >= 4 * SF_PREFIX) {
            int fl = pre.flags; double wmn = pre.mn, wmx = pre.mx;
            for (int w = 1; w <= 3 && fl; w++) {
                sf_pre_t q = sf_prefix(a + (n / 4) * (size_t)w - SF_PREFIX / 2, SF_PREFIX);
                fl &= q.flags; wmn = q.mn < wmn ? q.mn : wmn; wmx = q.mx > wmx ? q.mx : wmx;
            }
            if (!fl && !getenv("LOGOS_SF_FRONT_ONLY")) { pre.settled = 1; pre.mn = wmn; pre.mx = wmx; atomic_fetch_add_explicit(&g_sf_windows, 1, memory_order_relaxed); }
        }
        if (pre.settled) {
            settled = 1; all_int = 0; asc = 0; desc = 0; mn = pre.mn; mx = pre.mx;
            if (n > AVX2_FLASH_MAX_N && n >= AV_FUSE_MIN_N && c->seq && !g_no_partfuse && mx > mn) deferred = 1;
            else { sf_prof_t pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; }
        }
    }
    if (!settled)
#endif
    for (size_t i = 1; i < n; i++) {
        double v = a[i];
        if (v < mn) mn = v; else if (v > mx) mx = v;
        if (all_int && !(isfinite(v) && v == floor(v) && !(v == 0.0 && signbit(v)))) all_int = 0;
        if (asc && v < a[i - 1]) asc = 0;
        if (desc && v > a[i - 1]) desc = 0;
    }
    (void)sc; (void)deferred; (void)settled;
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
    if (n <= INSERTION_SORT_THRESHOLD) { PATH_TAG(c, LOGOS_PATH_INSERTION);
        if (n >= 4 && n <= 16 && !g_no_smallnet) net_sort_small(a, (long)n); else insertion_sort(a, 0, (long)n - 1);
        return a; }

    size_t run_cap = n / MIN_AVG_RUN_FOR_MERGE; if (run_cap < MAX_RUNS_FOR_MERGE) run_cap = MAX_RUNS_FOR_MERGE;
    size_t n_runs = 0; size_t *runs = detect_runs_limited(a, n, run_cap, MIN_AVG_RUN_FOR_MERGE, &n_runs, c);
    if (runs) {
        if (n_runs > 1) {
#ifndef LOGOS_NO_RELOCATE
            double *r = runs_relocate_f64(a, n, buf, runs, n_runs);   /* J5: disjoint runs -> memcpy into buf, no merge */
            if (r) { PATH_TAG(c, LOGOS_PATH_RUN_RELOCATE); free(runs); return r; }
#endif
            if (n_runs <= MERGE_MAX_RUNS) { PATH_TAG(c, LOGOS_PATH_RUN_MERGE); if (!merge_runs_vec(a, buf, runs, n_runs)) merge_all_runs(a, buf, runs, n_runs); free(runs); return a; }
        }
        free(runs);   /* too many overlapping runs for a k-way merge: the leaf below is cheaper (make knobbench, MERGE_MAX_RUNS) */
    }

    size_t sample_size = n < 40 ? n : 40, sample_step = n / sample_size; if (sample_step < 1) sample_step = 1;
    size_t inv = 0, comps = 0;
    for (size_t i = 0; i + sample_step < n; i += sample_step) { if (a[i] > a[i + sample_step]) inv++; comps++; }
    int nearly = comps > 0 && (double)inv / comps <= NEARLY_SORTED_INV_RATIO; c->nearly = nearly;
    /* The 40 pairs bound inversions at ONE stride, n/40. Disorder that is dense but shorter than the stride (an event log
       with bounded latency, a series jittered around its own order) passes them, and the insertion sort below then
       spends its whole budget of 8n moves before it gives up. 64 strided pairs EIGHT apart tell the two apart: the
       budget is a mean displacement of 8, and an element displaced further than that is usually inverted with its
       eighth neighbour. Sparse disorder shows a few percent of such pairs; disorder of reach 2 or 3, which the insertion
       sort handles best, shows none; noise of +-20 shows a third. LOGOS_NO_DENSE_PROBE=1: the A/B. */
    int dense = 0;
    if (nearly && n >= 1024 && !g_no_dense_probe) { size_t st = n / 64, d = 0; for (size_t i = 0; i + 8 < n; i += st) d += a[i] > a[i + 8]; dense = d >= 16; }
    /* TRIED 2026-09-21 and reverted: clearing c->nearly when `dense` fires, so that AVX2_TAKE lets a sequential sort use the AVX2
       quicksort on a sloped wave with noise. Paired, one binary (./sineprobe 40 4194304 ab): 1.02, neutral. The shape's 1.19x over
       its own values shuffled is NOT the leaf choice: with LOGOS_NO_AVX2=1 the wave and its shuffle cost the same (1.05, 1.05),
       so the cost of the ORDER is inside the AVX2 quicksort. The 126-against-87 ms that named this gate came from a profile build. */
    if (n >= 65536) TRACE("leaf probe: n=%zu inv=%zu/%zu nearly=%d dense=%d -> policy nearly=%d", n, inv, comps, nearly, dense, c->nearly);
    if (nearly && !dense) {
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
     * pays its full log n levels.
     * A window must also COVER the range (LOWDISC_MIN_COVER), which is what low discrepancy means: 64 consecutive points of a Weyl
     * or van der Corput sequence span ~63/64 of it. Evenly spaced gaps alone also describe a RAMP, 64 consecutive elements of a
     * sorted stretch, which spans 64 / (its length) of the range. The moire test (fieldprobe moire, 2026-10-21: periodic inputs
     * across 46 periods, each against its own values shuffled) found what that cost: a triangle wave sorted 2-3x slower than
     * shuffled from period 1024 up and 7.8x at period 4096 (60 ns per element, the AVX2 kernel refused and coarse recursing on
     * 512-element clusters); ramps 1.3-1.7x from period 64 to 4096. LOGOS_LOWDISC_LOCAL=1: the gap test alone, as before.
     * The coverage test is a BAND, and it was MEASURED (fieldprobe ab, roundbench, 4M, one thread). Coverage of half the range or
     * more is low discrepancy, as the definition says. Under it the kernel beats what the misfire chose on SHORT legs, 512 or less
     * (1.09-1.19 clean, 1.94 clustered) -- but on LONG clean legs coarse beats the kernel (2048: 1.25, 8192: 1.3, the saw64 guard:
     * 1.18), because its scatter follows the local order. So a window spanning under LOWDISC_LONG_COVER (5%: legs from ~1280) keeps
     * the probe and coarse. What that leaves: long legs whose values sit in tight CLUSTERS still take coarse, which recurses badly
     * on them (a clustered triangle of period 4096: 2.3x at 4M, 7.8x at 1M). Telling the two apart needs a cluster test the leaf does
     * not have (skew_probe's 256 bins see 2048 evenly filled bands as flat). Open: experiments/OTHER_FIELDS.md. A first cut at 0.05
     * alone had the comparison the wrong way round (long legs to the kernel, short ones to coarse): the worst of both. */
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
            else {
#ifdef LOGOS_AVX2_LEAF
                /* the coverage test needs the RANGE, and a deferred profile has only the windows' extremes: a triangle settled by the
                   founder windows read its 64-window as covering them, was called low-discrepancy and lost the kernel (0 partition
                   levels, 2x). Even gaps are rare, so the pass the kernel was going to carry runs here instead, only then. */
                if (deferred) { sf_prof_t pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; deferred = 0; }
#endif
                double span = win[63] - win[0], rng = mx - mn;
                if (span < LOWDISC_MIN_COVER * rng && span >= LOWDISC_LONG_COVER * rng && !getenv("LOGOS_LOWDISC_LOCAL")) ld = 0;   /* a SHORT ramp: the kernel */
            }
        }
        c->lowdisc = ld;
        if (ld) atomic_fetch_add_explicit(&g_lowdisc_fires, 1, memory_order_relaxed);
        if (ld) { PATH_TAG(c, LOGOS_PATH_FLASH); }
    }
#endif
#ifdef LOGOS_AVX2_LEAF
    /* AVX2 leaf, taken here only where FLASH SORT IS NOT AVAILABLE (n < FLASH_SORT_THRESHOLD). What the
     * alternative is decides, not the kernel: laddered at one thread with LOGOS_NO_AVX2 (2026-09-23), the
     * AVX2 quicksort beats the scalar quicksort below 4096 (1.31-1.47x, and 3.5-4.3x at 2048-3000 where
     * that quicksort has a cliff) and beats coarse_bits above TYPED_MIN_N (1.11-1.38x, which is leaf_f64's
     * own gate) -- but it LOSES to flash_sort in between, 1.24-1.34x at 4096-8192, because on data that
     * buckets evenly classification places every element within one class of home while a quicksort pays
     * its full log n levels. This gate used to run before the flash decision and took that whole window.
     * The coarse branch below has the other half of this gate. Nearly-sorted keeps coarse either way
     * (its scatter is near-sequential, measured 1.23x better). LOGOS_AVX2_OVER_FLASH=1 restores the old order. */
    if (n >= 32 && mx > mn && (n < FLASH_SORT_THRESHOLD || n > AVX2_FLASH_MAX_N || g_avx2_over_flash || (!g_band_flash && !band_local_order(a, n)))
        && AVX2_TAKE(c, n) && avx2_qsort_f64_sc(a, n, deferred ? -2 : sc)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
    if (deferred) { sf_prof_t pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; deferred = 0; }   /* not taken, or a NaN declined it: the pass, late */
#endif
    if (n >= FLASH_SORT_THRESHOLD && mx > mn && !isfinite(mx - mn)) {
        /* An infinite range (a +-inf in the data, or two finite values whose difference overflows). Every value-linear
         * map below (flash classes, coarse buckets, the gap-split pivot) computes (x - mn) * scale with scale = 0, and
         * inf * 0 is NaN: the cast to a bucket index is undefined and the bucket that held the infinity came back
         * UNSORTED (uniform data with a[0] = -inf at 4 threads; found 2026-09-19 by the knot harness, which is the
         * first test to put an infinity in front of a parallel sort). The exponent-bit coarse path keys on the bit
         * pattern, where an infinity is just the largest exponent; it needs a NaN-free array. */
        int inf_nan = 0; for (size_t i = 0; i < n; i++) if (a[i] != a[i]) { inf_nan = 1; break; }
        if (!inf_nan && n >= TYPED_MIN_N && ctx_coarse_ready(c)) { PATH_TAG(c, LOGOS_PATH_BITS_COARSE); return coarse_bits_f64(a, buf, 0, (long)n - 1, 0, 0, 0, 0, c); }
        PATH_TAG(c, LOGOS_PATH_QUICKSORT);
        quicksort(a, 0, (long)n - 1, 2 * log2i(n));
        return a;
    }
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
            /* ONE predicate, x < pivot, for both scans. The pair it replaces, a[i] < pivot and a[j] >= pivot, BOTH fail on a NaN:
               when the two scans met on one, neither index moved and the loop never ended. A sort that does not return, on the
               data the header calls 'order unspecified'. Inherited from the JS engine; found 2026-09-20 because the harness hung
               about one run in fifteen: the order inside a bucket depends on thread timing, so whether the scans meet on a NaN
               does too. Heavy-tailed data with 1% NaN at 4 threads spun about one sort in thirty. A NaN now goes right. */
            while (i <= j) { while (i <= j && a[i] < pivot) i++; while (i <= j && !(a[j] < pivot)) j--; if (i < j) { double t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
            if (i > 1) { double *r = sort_fn(a, (size_t)i, buf, c); if (r != a) memcpy(a, buf, (size_t)i * sizeof(double)); }
            if ((size_t)i < n) { double *r = sort_fn(a + i, n - (size_t)i, buf, c); if (r != a + i) memcpy(a + i, buf, (n - (size_t)i) * sizeof(double)); }
            return a;
        }
        if (n < COARSE_MIN_N && (samples[47] - samples[16]) / range >= FLASH_SAFETY_RATIO) { PATH_TAG(c, LOGOS_PATH_FLASH); return flash_sort(a, n, mn, mx, buf, c); }
#ifdef LOGOS_AVX2_LEAF
        /* Flash declined this range, so the alternative is coarse -- and against coarse the AVX2 quicksort wins.
         * This is the other half of the gate above; between them the leaf takes the SIMD kernel everywhere it
         * is the best of the three and nowhere it is not. */
        if (!g_avx2_over_flash && AVX2_TAKE(c, n) && avx2_qsort_f64_sc(a, n, sc)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
#endif
        if (n >= TYPED_MIN_N && skew_probe(a, n) > SKEW_PROBE_MAX) {
            int has_nan = 0; for (size_t i = 0; i < n; i++) if (a[i] != a[i]) { has_nan = 1; break; }
            if (!has_nan && ctx_coarse_ready(c)) { PATH_TAG(c, LOGOS_PATH_BITS_COARSE); return coarse_bits_f64(a, buf, 0, (long)n - 1, 0, 0, 0, 0, c); }
        }
        if (ctx_coarse_ready(c)) { PATH_TAG(c, LOGOS_PATH_COARSE); return coarse_bucket_sort(a, buf, 0, (long)n - 1, 0, mn, mx, c); }
    }
    PATH_TAG(c, LOGOS_PATH_QUICKSORT);
    quicksort(a, 0, (long)n - 1, 2 * log2i(n));
    return a;
}

/* typed-double entry (port of sort_fn.f64): quantized -> float radix; skewed
 * -> bits coarse; else the dispatcher. */
/* typed-double entry (port of sort_fn.f64); returns the buffer holding the result */
/* FLOAT32 NARROWING (2026-10-08, make f32bench). Doubles that are all exactly floats (data that was float before it was stored as
 * double: sensor readings, a float32 column widened by a reader) carry 32 bits of order in 64. One pass converts each to float and
 * back, which proves it exact and writes a 32-bit total-order key (NaN fails the test, so a NaN declines); the keys are radix
 * sorted relative to their minimum in 1 to 3 passes of at most 11 bits, the last pass writing the doubles straight back. Moves 4
 * bytes per element per pass where the double kernels move 8. The first double that is not a float ends the pass, so ordinary
 * doubles pay one element. One thread (the context must be sequential), F32_MIN_N and up. LOGOS_NO_F32=1 or -DLOGOS_NO_F32. */
#ifndef F32_MIN_N
#define F32_MIN_N 65536
#endif
static int g_no_f32 = 0;
static atomic_size_t g_f32_sorts;     /* logos_twist_last(22): sorts the float32 path finished (atomic: batch workers are sequential contexts too) */
static inline uint32_t f32_key(float f) { uint32_t u; memcpy(&u, &f, 4); return (u >> 31) ? ~u : (u | 0x80000000u); }
static inline double f32_unkey(uint32_t k) { uint32_t u = (k >> 31) ? (k & 0x7FFFFFFFu) : ~k; float f; memcpy(&f, &u, 4); return (double)f; }
/* ONE SCRATCH ARRAY (2026-10-24, the second memory round, FINDINGS 3bq): the keys went into the context's aux array and the passes
 * ping-ponged between aux and buf, two 4-byte arrays where one does (8 bytes per element above the array at one thread, make
 * rssprobe). Now the keys are narrowed IN PLACE, key i written over the bytes of a[i / 2], which was read already (the int32 radixes'
 * trick, 3bp), and the passes ping-pong between the input's own storage and buf. The LAST pass must read the array that is not a[],
 * so with an odd number of passes the biased keys go to buf from the start. A decline must still hand back a[] as it came: every key
 * written before the first value that is not a float came from a value that IS one, so widening them back from the top down (key k
 * is read before double k covers keys 2k and 2k + 1) restores the input bit for bit, -0.0 included. LOGOS_F32_TWOBUF=1: the keys
 * in aux, for the A/B. */
static __attribute__((unused)) int f32_radix_try(double *a, size_t n, double *buf, ctx_t *c) {
    int twobuf = getenv("LOGOS_F32_TWOBUF") != NULL;
    u32a_t *src = twobuf ? (u32a_t *)c->aux : (u32a_t *)a, *dst = (u32a_t *)buf;
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (size_t i = 0; i < n; i++) {
        double x = a[i]; float f = (float)x;
        if ((double)f != x) {                                          /* not a float, or a NaN: decline with a[] as it came */
            if (!twobuf) for (size_t k = i; k-- > 0;) a[k] = f32_unkey(src[k]);
            return 0;
        }
        uint32_t k = f32_key(f); src[i] = k; if (k < mn) mn = k; if (k > mx) mx = k;
    }
    uint32_t range = mx - mn;
    if (range == 0) {                                                  /* one value (both zeros would differ in key): sorted */
        if (!twobuf) for (size_t k = n; k-- > 0;) a[k] = f32_unkey(src[k]);
        atomic_fetch_add_explicit(&g_f32_sorts, 1, memory_order_relaxed); return 1;
    }
    int bits = 32 - __builtin_clz(range);
    int np = bits <= 11 ? 1 : bits <= 22 ? 2 : 3, w = (bits + np - 1) / np; uint32_t mask = (1u << w) - 1; size_t nc = (size_t)1 << w;
    uint32_t *cnt = calloc((size_t)np * (nc + 1), sizeof(uint32_t));
    if (!cnt) { if (!twobuf) for (size_t k = n; k-- > 0;) a[k] = f32_unkey(src[k]); return 0; }   /* a[] as it came: the caller's leaf sorts it */
    uint32_t *c0 = cnt, *c1 = cnt + (nc + 1), *c2 = cnt + 2 * (nc + 1);
    u32a_t *h = (!twobuf && (np & 1)) ? dst : src;                     /* where the biased keys go: the last pass must not read a[] */
    if (np == 1)      for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; h[i] = k; c0[k + 1]++; }
    else if (np == 2) for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; h[i] = k; c0[(k & mask) + 1]++; c1[(k >> w) + 1]++; }
    else              for (size_t i = 0; i < n; i++) { uint32_t k = src[i] - mn; h[i] = k; c0[(k & mask) + 1]++; c1[((k >> w) & mask) + 1]++; c2[(k >> (2 * w)) + 1]++; }
    for (int p = 0; p < np; p++) { uint32_t *cc = cnt + (size_t)p * (nc + 1); for (size_t b = 1; b <= nc; b++) cc[b] += cc[b - 1]; }
    u32a_t *from = h, *to = h == dst ? src : dst;
    for (int p = 0; p < np; p++) {
        uint32_t *cc = cnt + (size_t)p * (nc + 1); int sh = p * w;
        if (p == np - 1) { for (size_t i = 0; i < n; i++) { uint32_t k = from[i]; a[cc[(k >> sh) & mask]++] = f32_unkey(k + mn); } }
        else { for (size_t i = 0; i < n; i++) { if (i + PF_DIST < n) PREFETCH_W(to + cc[(from[i + PF_DIST] >> sh) & mask]); uint32_t k = from[i]; to[cc[(k >> sh) & mask]++] = k; }
               u32a_t *tmp = from; from = to; to = tmp; }
    }
    free(cnt);
    atomic_fetch_add_explicit(&g_f32_sorts, 1, memory_order_relaxed);
    return 1;
}
static double *leaf_f64(double *a, size_t n, double *buf, ctx_t *c) {
    if (n < TYPED_MIN_N) return sort_fn(a, n, buf, c);
    if (g_quant_radix && n <= RADIX64_MAX_N && quant_probe_f64(a, n) <= QUANT_PROBE_MAX) { TRACE("leaf_f64: n=%zu quantized -> float radix", n); PATH_TAG(c, LOGOS_PATH_QUANT_RADIX); radix_f64_full(a, n, buf, c); return a; }
#ifndef LOGOS_NO_F32
    if (c->seq && n >= F32_MIN_N && !g_no_f32 && f32_radix_try(a, n, buf, c)) return a;     /* every value a float: 4-byte keys */
#endif
    if (skew_probe(a, n) <= SKEW_PROBE_MAX) return sort_fn(a, n, buf, c);
    TRACE("leaf_f64: n=%zu skewed (probe %.1f) -> the skewed branch", n, skew_probe(a, n));
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE)
    /* The skewed branch asked "is there a NaN?" with a scalar loop and then the kernel asked again with its own scan: two full
       passes for one answer (make frontprobe: 1.05-1.06x against the library with one scan in front, heavy_tail, bimodal,
       clustered). Where the kernel is going to run, its scan answers this branch's question too. */
    if (!g_no_frontfuse && AVX2_TAKE(c, n)) {
        if (!g_no_partfuse) {                /* lever C: the kernel's first partition asks it (av_qsort_fused) */
            if (avx2_qsort_f64_sc(a, n, -2)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
            return sort_fn(a, n, buf, c);    /* declined: a NaN, met in that partition; the same multiset, permuted */
        }
        int sc = av_scan_nan_negzero(a, n);
        if (sc & 1) return sort_fn(a, n, buf, c);
        if (avx2_qsort_f64_sc(a, n, sc)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
    }
#endif
    int has_nan = 0; for (size_t i = 0; i < n; i++) if (a[i] != a[i]) { has_nan = 1; break; }
    if (has_nan) return sort_fn(a, n, buf, c);
#ifdef LOGOS_AVX2_LEAF
    if (AVX2_TAKE(c, n) && avx2_qsort_f64(a, n)) { PATH_TAG(c, LOGOS_PATH_SIMD); return a; }
#endif
    if (ctx_coarse_ready(c)) { PATH_TAG(c, LOGOS_PATH_BITS_COARSE); return coarse_bits_f64(a, buf, 0, (long)n - 1, 0, 0, 0, 0, c); }
    PATH_TAG(c, LOGOS_PATH_QUICKSORT); quicksort(a, 0, (long)n - 1, 2 * log2i(n)); return a;   /* no memory for the tables */
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
typedef struct { pthread_mutex_t m; pthread_cond_t cv; atomic_int count; atomic_int arrived; atomic_int gen; } barrier_t;
static void barrier_init(barrier_t *b, int n) { pthread_mutex_init(&b->m, NULL); pthread_cond_init(&b->cv, NULL); atomic_init(&b->count, n); atomic_init(&b->arrived, 0); atomic_init(&b->gen, 0); }
/* One participant fewer: its thread could not be created. Only the thread that runs the create loop calls this, and only
 * before its own first arrival. Whoever completes a barrier arrives no earlier than that thread does (it is one of the
 * participants), so the completing thread always reads the final count and no arrival is ever missed. */
static void barrier_drop(barrier_t *b) { atomic_fetch_sub_explicit(&b->count, 1, memory_order_release); }
static void barrier_wait(barrier_t *b) {
    int g = atomic_load_explicit(&b->gen, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->arrived, 1, memory_order_acq_rel) + 1 == atomic_load_explicit(&b->count, memory_order_acquire)) {
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

/* ---- the persistent worker pool (2026-10-08) -----------------------------------------------------------------------------
 * Every parallel phase used to create its P - 1 threads and join them: ~60 us a thread here, 0.56-0.77 ms for 13, paid once per
 * phase (FINDINGS 3ay: the lattice route spent ~1.1 ms of a 6.6 ms call doing it; at 1-4M elements that is most of a call). The
 * pool keeps the threads. Each has its OWN mailbox (function, argument, state), so a thread that sat out one group can never read
 * the next group's job half-written, and only the threads given a task are woken. A thread runs at most one task per group, so
 * the workers of a barrier group are always concurrent, as created threads were. Idle threads spin WP_SPIN pauses (the gap
 * between two phases of one sort) and then sleep on their own condition variable, so a process that has stopped sorting burns
 * nothing.
 *   - One group at a time: a nested or concurrent group (a nested sort, another caller) finds the pool busy and creates threads,
 *     exactly the old code, which also stays the fallback when the pool cannot grow (creation failed: those tasks are orphans
 *     with the same answers as before).
 *   - fork: a child inherits the bookkeeping and none of the threads; pthread_atfork resets it there.
 *   - logos_release_buffers() stops and joins the threads, so a released engine holds nothing, and the harness's thread-creation
 *     faults start from a cold pool.
 * LOGOS_NO_POOL=1: create per phase, as before (the A/B). */
#ifndef WP_MAX
#define WP_MAX 256
#endif
#ifndef WP_SPIN
#define WP_SPIN 4000
#endif
typedef struct {
    pthread_mutex_t m; pthread_cond_t cv;
    atomic_int state;                        /* 0 idle, 1 a task is posted, 2 quit */
    void *(*fn)(void *); void *arg;          /* written by the owner before state = 1 (release) */
    pthread_t th;
} __attribute__((aligned(64))) wp_slot_t;
static wp_slot_t *g_wp;                      /* WP_MAX mailboxes, allocated once */
static int g_wp_n;                           /* threads running; touched only by the holder of g_wp_busy */
static atomic_int g_wp_busy;                 /* a group holds the pool */
static atomic_int g_wp_left;                 /* the holder's tasks not yet finished */
static atomic_size_t g_wp_groups;            /* logos_twist_last(19): groups the pool served; reading clears it */
static _Atomic(const void *) g_wp_owner;     /* the par_spawn group's handle array, which par_join recognises */
static pthread_mutex_t g_wp_dm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wp_dcv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_wp_once = PTHREAD_ONCE_INIT;
static void wp_atfork_child(void) {
    g_wp_n = 0; atomic_store(&g_wp_busy, 0); atomic_store(&g_wp_left, 0); atomic_store(&g_wp_owner, NULL);
    pthread_mutex_init(&g_wp_dm, NULL); pthread_cond_init(&g_wp_dcv, NULL);
    if (g_wp) for (int i = 0; i < WP_MAX; i++) { pthread_mutex_init(&g_wp[i].m, NULL); pthread_cond_init(&g_wp[i].cv, NULL); atomic_store(&g_wp[i].state, 0); }
}
static void wp_init(void) {
    wp_slot_t *s = NULL;
    if (posix_memalign((void **)&s, 64, (size_t)WP_MAX * sizeof(wp_slot_t)) != 0) return;
    for (int i = 0; i < WP_MAX; i++) { pthread_mutex_init(&s[i].m, NULL); pthread_cond_init(&s[i].cv, NULL); atomic_init(&s[i].state, 0); s[i].fn = NULL; s[i].arg = NULL; }
    g_wp = s;
    pthread_atfork(NULL, NULL, wp_atfork_child);
}
static void *wp_main(void *p) {
    wp_slot_t *s = p;
    for (;;) {
        int st, spins = 0;
        while ((st = atomic_load_explicit(&s->state, memory_order_acquire)) == 0) {
            if (++spins < WP_SPIN) { CPU_RELAX(); continue; }
            pthread_mutex_lock(&s->m);
            while ((st = atomic_load_explicit(&s->state, memory_order_acquire)) == 0) pthread_cond_wait(&s->cv, &s->m);
            pthread_mutex_unlock(&s->m);
            break;
        }
        if (st == 2) return NULL;
        s->fn(s->arg);
        atomic_store_explicit(&s->state, 0, memory_order_release);
        if (atomic_fetch_sub_explicit(&g_wp_left, 1, memory_order_acq_rel) == 1) {
            pthread_mutex_lock(&g_wp_dm); pthread_cond_signal(&g_wp_dcv); pthread_mutex_unlock(&g_wp_dm);
        }
    }
}
/* Take the pool for a group of `need` tasks. Returns how many pool threads it may use, 0 if it must create its own. */
static int wp_begin(int need) {
    if (need <= 0) return 0;
    if (atomic_exchange_explicit(&g_wp_busy, 1, memory_order_acquire)) return 0;      /* nested or concurrent: the old path */
    if (getenv("LOGOS_NO_POOL")) { atomic_store_explicit(&g_wp_busy, 0, memory_order_release); return 0; }
    pthread_once(&g_wp_once, wp_init);
    if (!g_wp) { atomic_store_explicit(&g_wp_busy, 0, memory_order_release); return 0; }
    while (g_wp_n < need && g_wp_n < WP_MAX) {                                        /* grow; a failed creation stops growth */
        wp_slot_t *s = &g_wp[g_wp_n];
        atomic_store_explicit(&s->state, 0, memory_order_relaxed);
        if (pthread_create(&s->th, NULL, wp_main, s) != 0) break;
        g_wp_n++;
    }
    int avail = g_wp_n < need ? g_wp_n : need;
    if (avail == 0) { atomic_store_explicit(&g_wp_busy, 0, memory_order_release); return 0; }
    atomic_store_explicit(&g_wp_left, avail, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_wp_groups, 1, memory_order_relaxed);
    return avail;
}
static void wp_post(int i, void *(*fn)(void *), void *arg) {
    wp_slot_t *s = &g_wp[i];
    s->fn = fn; s->arg = arg;
    atomic_store_explicit(&s->state, 1, memory_order_release);
    pthread_mutex_lock(&s->m); pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->m);
}
static void wp_end(void) {                                                            /* wait for the group, then free the pool */
    for (int i = 0; i < WP_SPIN && atomic_load_explicit(&g_wp_left, memory_order_acquire) != 0; i++) CPU_RELAX();
    if (atomic_load_explicit(&g_wp_left, memory_order_acquire) != 0) {
        pthread_mutex_lock(&g_wp_dm);
        while (atomic_load_explicit(&g_wp_left, memory_order_acquire) != 0) pthread_cond_wait(&g_wp_dcv, &g_wp_dm);
        pthread_mutex_unlock(&g_wp_dm);
    }
    atomic_store_explicit(&g_wp_owner, NULL, memory_order_relaxed);
    atomic_store_explicit(&g_wp_busy, 0, memory_order_release);
}
/* Stop and join every pool thread (logos_release_buffers). Not while a group holds the pool. */
static void wp_shutdown(void) {
    if (atomic_exchange_explicit(&g_wp_busy, 1, memory_order_acquire)) return;
    for (int i = 0; i < g_wp_n; i++) {
        wp_slot_t *s = &g_wp[i];
        atomic_store_explicit(&s->state, 2, memory_order_release);
        pthread_mutex_lock(&s->m); pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->m);
    }
    for (int i = 0; i < g_wp_n; i++) { pthread_join(g_wp[i].th, NULL); atomic_store_explicit(&g_wp[i].state, 0, memory_order_relaxed); }
    g_wp_n = 0;
    atomic_store_explicit(&g_wp_busy, 0, memory_order_release);
}

/* ---- thread creation can fail ---------------------------------------------------------------------------------------
 * pthread_create returns EAGAIN under a process or pid limit (any container with one) and ENOMEM when memory is short.
 * Until 2026-09-20 none of the 23 call sites here looked at the result: a failed creation was a join on a garbage handle,
 * and at the six sites that meet at a barrier also a barrier that never fills. `make faultprobe` fails every creation of a
 * cold sort in turn; before this, every such run hung or crashed.
 *
 * par_run: workers 1..P-1 each on a new thread, worker 0 on this one (run0 = 0: the caller has done worker 0 already),
 * then join. A worker whose thread cannot be created runs HERE instead, after the others are under way, so the phase gets
 * slower and never wrong. Valid wherever the workers do not wait for one another: every site without a barrier. */
static void par_run(pthread_t *th, int P, void *(*fn)(void *), void *args, size_t stride, int run0) {
    int avail = P > 1 ? wp_begin(P - 1) : 0;
    if (avail > 0) {                                            /* the pool: workers 1..avail on its threads, the rest here */
        for (int t = 1; t <= avail; t++) wp_post(t - 1, fn, (char *)args + (size_t)t * stride);
        if (run0) fn(args);
        for (int t = avail + 1; t < P; t++) fn((char *)args + (size_t)t * stride);
        wp_end();
        return;
    }
    int made = 0, no = 0, orph[16];
    for (int t = 1; t < P; t++) {
        void *w = (char *)args + (size_t)t * stride;
        if (pthread_create(&th[made], NULL, fn, w) == 0) made++;
        else if (no < 16) orph[no++] = t;                       /* once the others are under way */
        else fn(w);                                             /* creation has stopped working altogether: no list needed */
    }
    if (run0) fn(args);
    for (int i = 0; i < no; i++) fn((char *)args + (size_t)orph[i] * stride);
    for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
}
/* par_spawn: the create loop for workers that DO meet at a barrier, where an orphan cannot simply be run here because its
 * phases interleave with everyone else's. It is dropped from the barrier instead. A queue-driven phase needs nothing
 * more, the others drain the queue; a site with static per-thread shares passes orphan[] and covers those shares on the
 * calling thread, or declines. Returns the number of handles to join: th[0 .. made). */
static int par_spawn(pthread_t *th, int P, void *(*fn)(void *), void *args, size_t stride, barrier_t *bar, unsigned char *orphan) {
    int avail = P > 1 ? wp_begin(P - 1) : 0;
    if (avail > 0) {                                            /* the pool: a task with no thread is an orphan, as a failed creation is */
        for (int t = 1; t < P; t++) {
            if (t <= avail) wp_post(t - 1, fn, (char *)args + (size_t)t * stride);
            else { if (bar) barrier_drop(bar); if (orphan) orphan[t] = 1; }
        }
        atomic_store_explicit(&g_wp_owner, (const void *)th, memory_order_relaxed);
        return avail;
    }
    int made = 0;
    for (int t = 1; t < P; t++) {
        if (pthread_create(&th[made], NULL, fn, (char *)args + (size_t)t * stride) == 0) made++;
        else { if (bar) barrier_drop(bar); if (orphan) orphan[t] = 1; }
    }
    return made;
}
/* the join of a par_spawn group: the pool's group, or `made` created threads */
static void par_join(pthread_t *th, int made) {
    if (th && atomic_load_explicit(&g_wp_owner, memory_order_relaxed) == (const void *)th) { wp_end(); return; }
    for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
}

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
/* outside the mode table's guard: the mode grid (mdx_t) sorts its cuts with it too, and -DLOGOS_NO_MODE_TABLE did not build (2026-10-31) */
static __attribute__((unused)) int cmp_int(const void *x, const void *y) { int a = *(const int *)x, b = *(const int *)y; return (a > b) - (a < b); }
static int bt_cnt_le_v(const bt_t *t, double x)   { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->sp[m] <= x) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_lt_v(const bt_t *t, double x)   { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->sp[m] <  x) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_le_k(const bt_t *t, uint64_t k) { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->ksp[m] <= k) lo = m + 1; else hi = m; } return lo; }
static int bt_cnt_lt_k(const bt_t *t, uint64_t k) { int lo = 0, hi = t->nsp; while (lo < hi) { int m = (lo + hi) >> 1; if (t->ksp[m] <  k) lo = m + 1; else hi = m; } return lo; }
static int bt_build_v(bt_t *t, double vlo, double vhi, int cells, int depth) {
    if (t->nvn >= BT_MAX_NODES || t->nve + cells > BT_MAX_ENTS || !(vhi > vlo)) return -1;
    int id = t->nvn++; bt_vnode_t *nd = &t->vn[id]; nd->lo = vlo; nd->scale = cells / (vhi - vlo); nd->cells = cells; nd->base = t->nve; t->nve += cells;
    /* a scale of ZERO is not a scale: an infinite range gives cells / inf = 0, which is finite, and then every cell edge is
       -inf + c / 0 = NaN and every lookup (x - lo) * 0 = NaN lands in cell 0, whose entry was built from NaN compares. The
       partition came back with its buckets out of order (uniform data, a[0] = -inf, 4 threads; 2026-09-19). With no value
       table the trial falls to the key-space table, where an infinity is only the largest key. */
    if (!isfinite(nd->scale) || !(nd->scale > 0)) return -1;
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
    t->mscale = BT_ROOT_CELLS / (vhi - vlo); if (!isfinite(t->mscale) || !(t->mscale > 0)) return 0;   /* zero = infinite range: see bt_build_v */
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
/* noinline: its two 1024-double gap arrays are 16 KB, and when GCC inlined it into psort_f64 (2026-10-07, after the mode grid
   changed the dispatcher's size) the dispatcher's frame went 28,320 -> 42,656 bytes, probed page by page on EVERY call: sorted
   calls of 2 and 3 elements read 1.11-1.13 in make callbench. It runs once per partition. */
static __attribute__((noinline)) int bt_init(bt_t *t, const double *sp, int nsp, double vlo, double vhi) {
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
    struct mdx_s *mdx;                    /* the same map past MD_LIN_MAX modes, with an O(1) mode grid (heap: this struct is on the dispatcher's stack) */
    struct hv_s *hv;                      /* heavy values the histogram-free scatter counts instead of moving (heap, for the same reason) */
#ifndef LOGOS_NO_KGRID
    int kg_ok, kg_shift; uint64_t kg_lo; uint32_t kg_cells; uint8_t kg_tab[1u << KG_BITS];   /* key grid: bucket per key-space cell */
#endif
    int seq_leaf;                         /* the leaves may take the sequential leaf's AVX2 quicksort (see SEQLEAF_MIN_BUCKET) */
} job_t;
/* THE MODE GRID (2026-10-07, make scoreboard): the per-mode map chose its mode with one compare per mode boundary, every element,
   and reloaded the boundaries each time (the scatter's stores are doubles, so they may alias them): 8 clusters scattered in 7.5 ms
   at 14 threads against bimodal's 4.7 on the same map, and past 8 modes the map was not offered at all (the splitter table keeps
   at most 8), so 16 clusters fell to the table lookup, 13.6 ms. Here the value range is cut into MD_GRID_CELLS equal cells and
   each cell stores how many mode boundaries lie wholly below it; floating-point subtract and multiply are monotone, so that count
   is exact for every value in the cell, and the one boundary a cell may hold is settled by one compare (a cell holding two is
   not built). Up to MD_MAX modes, found from the splitters with the table's own gulf rule. LOGOS_NO_MDGRID=1: the old map. */
#ifndef MD_MAX
#define MD_MAX 64
#endif
#ifndef MD_LIN_MAX
#define MD_LIN_MAX 8                      /* up to this many modes the compares are cheaper: 4 clusters read 0.933 through the grid, 8 read 1.035 (noise), 16 1.311 */
#endif
#define MD_GRID_CELLS 4096
static atomic_size_t g_mdgrid_scatters;   /* logos_twist_last(18): partitions whose scatter used the mode grid; reading clears it */
typedef struct mdx_s {
    int nm, cells; double glo, gscale;
    int first[MD_MAX], last[MD_MAX]; double lo[MD_MAX + 1], hi[MD_MAX], scale[MD_MAX];   /* lo[nm] = NaN */
    uint8_t grid[MD_GRID_CELLS];
} mdx_t;
static inline __attribute__((always_inline)) int md_cell(const mdx_t *g, double x) {
    double t = (x - g->glo) * g->gscale;                    /* a NaN lands in cell 0 */
    return t >= 0 ? (t < (double)g->cells ? (int)t : g->cells - 1) : 0;
}
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
static inline uint64_t total_key(double v);   /* again, outside the splitter-table block: -DLOGOS_NO_SPLITTER_TABLE compiled that one out, and the engine did not build */
static inline __attribute__((always_inline)) int hf_lookup_kg(const job_t *j, double x) {
    uint64_t k = total_key(x);
    uint64_t d = k > j->kg_lo ? (k - j->kg_lo) >> j->kg_shift : 0;
    return j->kg_tab[d < j->kg_cells ? d : j->kg_cells - 1];
}
#endif
static inline __attribute__((always_inline)) int hf_lookup_md(const job_t *j, double x) {
    const mdx_t *g = j->mdx;
    if (g) {                                                  /* the mode grid: one cell, at most one compare */
        int k = g->grid[md_cell(g, x)]; k += (x >= g->lo[k + 1]);   /* lo[nm] is a NaN: never passed, not even by +inf */
        if (!(x >= g->lo[k])) return g->first[k];            /* below the mode, or a NaN: a real bucket either way */
        if (x >= g->hi[k]) return g->last[k] + 1;
        int b = g->first[k] + 1 + (int)((x - g->lo[k]) * g->scale[k]);
        return b > g->last[k] ? g->last[k] : b;
    }
    int k = 0;
    for (int q = 1; q < j->md_nm; q++) k += (x >= j->md_lo[q]);
    /* inside the gulf below this mode -- or a NaN, which used to fall through to (int)(NaN * scale), INT_MIN on x86, a NEGATIVE
       bucket: bimodal data holding one NaN the sample missed crashed every 14-thread sort (found 2026-10-07 by the mode grid's
       harness rows; the committed engine segfaults too). */
    if (!(x >= j->md_lo[k])) return j->md_first[k];
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
        else { double *r = j->dst + s;
               if (nb > 1) { ctx_fit_t cf;
                   if (ctx_fit(ctx, nb, &cf)) { r = leaf_f64(j->dst + s, nb, j->src + s, ctx); ctx_unfit(ctx, &cf); }
                   else quicksort(j->dst + s, 0, (long)nb - 1, 2 * log2i(nb)); }      /* no memory for a context this large: a sort that needs none */
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
static int pool_acquire(size_t buf_bytes, size_t bidx_bytes, void **buf, uint8_t **bidx); static void pool_release(void);
static double *hf_buf = NULL; static size_t hf_buf_cap = 0;          /* persistent, like the pool: page faults once */
static atomic_size_t g_hf_pooled;      /* logos_twist_last(35): partitions whose regions were the pool's block; reading clears it */
static trim_t hf_trim;                                                  /* and trimmed like it (trim_note) */
/* HEAVY VALUES ARE COUNTED, NOT MOVED (2026-11-10, FINDINGS 3ca). A value holding more than a region (one bucket of n / ~170 plus
 * slack) made the partition decline up front: the histogram path then cost a counting pass and a scatter where this one costs
 * a scatter, and gave every copy of the value to ONE bucket and one thread. A Zipf key column lives there (US ZIP codes by
 * population, a foreign key by customer: the top value 6%, the top sixty-four 45%, none near the dominant route's 20%). The sorted
 * sample names the values: every run of bit-equal points at least a quarter of a bucket's share, the HV_MAX longest. The scatter
 * looks a value up as before and, in a bucket that holds a heavy value, compares bits: a match is counted per thread and not
 * staged, not written, not sorted. A bucket's output is then its other elements, sorted by the leaf, with each heavy run filled
 * in by arithmetic where it belongs. What the decline test reads afterwards is the largest share among the values NOT taken.
 * Bits decide, as in the dominant route: -0.0 beside a heavy +0.0 stays an ordinary element and lands next to the run, equal
 * under <. LOGOS_NO_HFHEAVY=1: off (the old decline). logos_twist_last(41): partitions that counted heavy values. */
#ifndef HV_MAX
#define HV_MAX 64
#endif
typedef struct hv_s { int n; double rest; double v[HV_MAX]; uint64_t b[HV_MAX]; size_t tot[HV_MAX]; uint8_t lo[MAX_BUCKETS], hi[MAX_BUCKETS]; } hv_t;
static atomic_size_t g_hf_heavy;
static inline size_t hv_runlen(const double *samp, size_t m, size_t i) {
    uint64_t x, y; memcpy(&x, samp + i, 8); size_t e = i + 1;
    while (e < m) { memcpy(&y, samp + e, 8); if (y != x) break; e++; }
    return e - i;
}
/* the heavy values of a sorted sample, in value order; *maskp marks their sample points (for the balance check). NULL: none. */
static __attribute__((noinline, unused)) hv_t *hv_build(const double *samp, size_t m, int nbk, uint8_t **maskp) {
    *maskp = NULL;
    if (m < 256 || nbk < 2 || nbk > MAX_BUCKETS || getenv("LOGOS_NO_HFHEAVY")) return NULL;
    size_t cut = m / (4 * (size_t)nbk); if (cut < 6) cut = 6;
    for (;;) {                                                        /* at most HV_MAX values: raise the cut until they fit */
        size_t nr = 0;
        for (size_t i = 0; i < m;) { size_t L = hv_runlen(samp, m, i); if (L >= cut && samp[i] == samp[i]) nr++; i += L; }
        if (!nr) return NULL;
        if (nr <= HV_MAX) break;
        cut += 1 + cut / 8;
    }
    hv_t *hv = calloc(1, sizeof *hv); uint8_t *mask = calloc(m, 1);
    if (!hv || !mask) { free(hv); free(mask); return NULL; }
    size_t rest = 1;
    for (size_t i = 0; i < m;) {
        size_t L = hv_runlen(samp, m, i);
        if (L >= cut && samp[i] == samp[i] && hv->n < HV_MAX) { hv->v[hv->n] = samp[i]; memcpy(&hv->b[hv->n], samp + i, 8); hv->n++; memset(mask + i, 1, L); }
        else if (L > rest) rest = L;
        i += L;
    }
    hv->rest = (double)rest / (double)m;
    *maskp = mask;
    return hv;
}
/* out[0 .. no) holds a bucket's other elements, sorted; its heavy values q in [lo, hi) go in where they belong, from the top down */
static void hv_insert(double *out, size_t no, const hv_t *hv, int lo, int hi) {
    size_t w = no; for (int q = lo; q < hi; q++) w += hv->tot[q];
    size_t e = no;
    for (int q = hi - 1; q >= lo; q--) {
        double v = hv->v[q]; size_t c = hv->tot[q];
        size_t a = 0, b = e; while (a < b) { size_t mid = (a + b) >> 1; if (out[mid] <= v) a = mid + 1; else b = mid; }   /* first element above v */
        size_t up = e - a;
        if (up && w != e) memmove(out + w - up, out + a, up * sizeof(double));
        w -= up; e = a;
        for (size_t i = w - c; i < w; i++) out[i] = v;
        w -= c;
    }
}
typedef struct { double v; uint16_t b; } hf_spill_t;
typedef struct {
    job_t *j; int P; size_t cap, region; double *rb;                             /* region = cap rounded so buckets start 64-byte aligned */
    atomic_size_t *cur; atomic_size_t *ovf;                          /* per bucket: reservation cursor, first overflowing position (SIZE_MAX = none) */
    int mode, style;
    size_t *cnt, *spo, *spc, *bstart; hf_spill_t *spv; size_t S;     /* after the scatter */
    barrier_t bar; atomic_size_t next, bnext; int declined;
    int *order;                  /* leaf dispatch order; identity, or largest-first when LPT is on */
    int lazy;                    /* 1: place the buckets but do not sort them (logos_partition_f64) */
    double *vmin, *vmax;         /* lazy: per-bucket extremes, computed while the bucket is copied */
    hv_t *hv;                    /* heavy values counted in the scatter (NULL: none) */
} hf_t;
typedef struct { hf_t *h; int t; ctx_t *ctx; hf_spill_t *sp; size_t spn, spcap; size_t hvc[HV_MAX]; } hf_arg_t;

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
    if (p + k <= h->cap) { hf_copy(h->rb + (size_t)b * h->region + p, blk, k); return; }
    size_t want = SIZE_MAX; atomic_compare_exchange_strong(&h->ovf[b], &want, p);   /* first overflow marks the filled end */
    for (size_t i = 0; i < k; i++) hf_spill_push(w, blk[i], b);
}
static void hf_scatter(hf_arg_t *w) {
    hf_t *h = w->h; job_t *j = h->j; int nb = j->nbk, mode = h->mode, style = h->style; const hv_t *hv = h->hv;
    double *st = malloc((size_t)nb * HF_BLOCK * sizeof(double)); uint16_t *sc = calloc(nb, sizeof(uint16_t));
    if (!st || !sc) { free(st); free(sc); h->declined = 1; return; }
    size_t ch;
    while ((ch = atomic_fetch_add(&h->next, 1)) < j->nchunk && !h->declined) {
        size_t lo = ch * j->csize, hi = lo + j->csize; if (hi > j->n) hi = j->n; const double *src = j->src;
        if (hv) for (size_t i = lo; i < hi; i++) {                     /* a bucket that holds a heavy value: its copies are counted here and never staged */
            double x = src[i]; int b = hf_lookup(j, x, mode, style);
            unsigned q = hv->lo[b], qe = hv->hi[b];
            if (q != qe) { uint64_t xb; memcpy(&xb, &x, 8); for (; q < qe; q++) if (hv->b[q] == xb) break; if (q < qe) { w->hvc[q]++; continue; } }
            st[(size_t)b * HF_BLOCK + sc[b]] = x;
            if (++sc[b] == HF_BLOCK) { hf_flush(w, b, st + (size_t)b * HF_BLOCK, HF_BLOCK); sc[b] = 0; }
        }
        else for (size_t i = lo; i < hi; i++) {
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
        double *reg = h->rb + b * h->region, *out = j->src + h->bstart[b]; size_t nb = h->cnt[b], ns = h->spc[b];
        const hv_t *hv = h->hv; int hq0 = hv ? hv->lo[b] : 0, hq1 = hv ? hv->hi[b] : 0;
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
                { size_t o = nb + ns; for (int q = hq0; q < hq1; q++) { double v = hv->v[q]; size_t c = hv->tot[q]; for (size_t i = 0; i < c; i++) out[o + i] = v; o += c; if (c) { mn = v < mn ? v : mn; mx = v > mx ? v : mx; } } }
                h->vmin[b] = mn; h->vmax[b] = mx;
            } else {
                memcpy(out, reg, nb * sizeof(double));
                for (size_t i = 0; i < ns; i++) out[nb + i] = h->spv[h->spo[b] + i].v;
                { size_t o = nb + ns; for (int q = hq0; q < hq1; q++) { double v = hv->v[q]; size_t c = hv->tot[q]; for (size_t i = 0; i < c; i++) out[o + i] = v; o += c; } }
            }
            PROF(double mz = prof_busy_since(w->ctx->tid, LOGOS_PH_BUCKETS, s0); prof_path(w->ctx->tid, w->ctx->path, nb + ns, mz);)
            continue;
        }
        if (ns == 0) {
            double *r = nb > 1 ? leaf_f64(reg, nb, out, w->ctx) : reg;
            if (r != out) memcpy(out, r, nb * sizeof(double));
            if (hq0 != hq1) hv_insert(out, nb, hv, hq0, hq1);
        } else {
            double *sv = malloc((ns ? ns : 1) * sizeof(double));
            if (!sv) {            /* unchecked until 2026-09-20. No room for the spill list: lay both parts down and sort the bucket where it lies */
                memcpy(out, reg, nb * sizeof(double)); for (size_t i = 0; i < ns; i++) out[nb + i] = h->spv[h->spo[b] + i].v;
                quicksort(out, 0, (long)(nb + ns) - 1, 2 * log2i(nb + ns));
                if (hq0 != hq1) hv_insert(out, nb + ns, hv, hq0, hq1);
                PROF(double mq = prof_busy_since(w->ctx->tid, LOGOS_PH_BUCKETS, s0); prof_path(w->ctx->tid, w->ctx->path, nb + ns, mq);)
                continue;
            }
            for (size_t i = 0; i < ns; i++) sv[i] = h->spv[h->spo[b] + i].v;
            sample_sort(sv, ns);
            double *r = nb > 1 ? leaf_f64(reg, nb, out, w->ctx) : reg;
            if (r != reg) memcpy(reg, r, nb * sizeof(double));                    /* sorted region back in the buffer, out is free */
            const double *x = reg, *xe = reg + nb, *y = sv, *ye = sv + ns; double *o = out;
            while (x < xe && y < ye) { if (*y < *x) *o++ = *y++; else *o++ = *x++; }
            if (x < xe) { memcpy(o, x, (size_t)(xe - x) * sizeof(double)); o += xe - x; }
            if (y < ye) memcpy(o, y, (size_t)(ye - y) * sizeof(double));
            free(sv);
            if (hq0 != hq1) hv_insert(out, nb + ns, hv, hq0, hq1);
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
/* THE IN-PLACE PARTITION (2026-10-08, LOGOS_PART_INPLACE=1, make ipartbench). The histogram-free partition scatters into a region
 * buffer of 1.6n doubles (12.8 bytes per element asked for, 8.6 resident: make rssprobe). This one keeps the data in the array,
 * IPS4o's block permutation over the same bucket map (hf_trial picks it, hf_lookup classifies):
 *   1. CLASSIFY. The array is cut into P stripes of whole blocks (IP_B elements). Each stripe has a buffer of one block per bucket;
 *      a full buffer goes back into the stripe, behind the read position (a block fills only after the stripe has supplied that
 *      many elements, so it never overtakes the read). A stripe ends as full blocks, each of one bucket, then a gap.
 *   2. COMPACT. Bucket k's final range [b_k, b_k+1) rounded up to blocks is its AREA, [d_k, d_k+1) in block slots; whatever full
 *      blocks lie in an area move to its front (the gaps are the stripes' tails, at most P * buckets * IP_B elements in all).
 *   3. PERMUTE. Per bucket a write slot w and a read end r, [w, r) holding blocks not yet placed, under a spin lock. A thread pops a
 *      block from some bucket's r into a swap buffer, classifies it by its first element, takes slot w of that block's bucket: if
 *      the slot holds an unplaced block the two swap, else (an empty slot) it waits until no read of that bucket is in flight and
 *      writes. Each bucket is visited once by each thread; [w, r) only shrinks. A slot past the array's last whole block is the
 *      OVERFLOW block, a buffer of IP_B.
 *   4. CLEAN UP. Each bucket's block data that lies past its range (into the next bucket's head, or in the overflow) is copied
 *      aside; a barrier; then each bucket fills its holes (its head, its tail) from that and from every stripe's partial buffer.
 *   5. LEAVES. Each bucket is sorted where it lies, buckets claimed from a counter.
 * Every phase claims its work (stripes, buckets) from a counter, so a worker whose thread could not be created costs speed, never
 * correctness (par_spawn drops it from the barrier). Allocation failure declines before the array is touched. */
#ifndef IP_B
#define IP_B 256
#endif
static int g_part_inplace = 0;          /* LOGOS_PART_INPLACE=1: the in-place partition in place of the region buffer */
static atomic_size_t g_ip_parts;         /* logos_twist_last(24): partitions done in place */
typedef struct {
    job_t *j; int P, nbk, mode, style; size_t n, nslot, stripe, nstripe;
    double *a, *sbuf;                    /* sbuf: nstripe x nbk x IP_B partial buffers */
    uint16_t *scnt;                      /* nstripe x nbk fill counts */
    size_t *hist;                        /* nstripe x nbk element counts */
    size_t *wfull;                       /* nstripe: end of each stripe's full blocks, in elements */
    size_t *b, *d, *w, *r;               /* nbk + 1: bucket starts (elements), area starts (slots); w, r in slots */
    atomic_int *lock, *reading;
    double *ovf, *swp, *tmp; size_t *tmpn;   /* overflow block; 2 swap blocks per thread; 2 blocks per bucket of data copied aside */
    ctx_t *ctxs; size_t maxb;
    atomic_size_t next1, next2, next3, next4, next5;
    atomic_int fail;
    barrier_t bar;
} ipp_t;
typedef struct { ipp_t *p; int t; } ipp_arg_t;
static inline double *ipp_slot(ipp_t *p, size_t s) { return s < p->nslot ? p->a + s * IP_B : p->ovf; }
static inline void ipp_lock(atomic_int *l) { int z; for (;;) { z = 0; if (atomic_compare_exchange_weak_explicit(l, &z, 1, memory_order_acquire, memory_order_relaxed)) return; CPU_RELAX(); } }
static inline void ipp_unlock(atomic_int *l) { atomic_store_explicit(l, 0, memory_order_release); }
static inline int ipp_full(const ipp_t *p, size_t s) {               /* after CLASSIFY: does slot s hold a full block? */
    if (s >= p->nslot) return 0;
    size_t st = s * IP_B / p->stripe; if (st >= p->nstripe) st = p->nstripe - 1;   /* the last stripe runs on to n */
    return s * IP_B < p->wfull[st];
}
static void ipp_classify(ipp_t *p) {
    job_t *j = p->j; int nb = p->nbk, mode = p->mode, style = p->style; size_t st;
    while ((st = atomic_fetch_add(&p->next1, 1)) < p->nstripe) {
        size_t lo = st * p->stripe, hi = lo + p->stripe; if (st == p->nstripe - 1 || hi > p->n) hi = p->n;
        double *buf = p->sbuf + st * (size_t)nb * IP_B; uint16_t *sc = p->scnt + st * (size_t)nb; size_t *hs = p->hist + st * (size_t)nb;
        size_t w = lo; double *a = p->a;
        for (size_t i = lo; i < hi; i++) {
            double x = a[i]; int k = hf_lookup(j, x, mode, style);
            buf[(size_t)k * IP_B + sc[k]] = x;
            if (++sc[k] == IP_B) { memcpy(a + w, buf + (size_t)k * IP_B, IP_B * sizeof(double)); w += IP_B; sc[k] = 0; hs[k] += IP_B; }
        }
        for (int k = 0; k < nb; k++) hs[k] += sc[k];
        p->wfull[st] = w;
    }
}
static void ipp_compact(ipp_t *p) {
    size_t k;
    while ((k = atomic_fetch_add(&p->next2, 1)) < (size_t)p->nbk) {
        size_t lo = p->d[k], hi = p->d[k + 1], full = 0;
        if (hi > lo) {
            size_t x = lo, y = hi;                                 /* [x, y) not yet looked at; fullness is read as CLASSIFY left it,
                                                                      which stays true of every slot the two pointers have not passed */
            for (;;) {
                while (x < y && ipp_full(p, x)) x++;
                while (x < y && !ipp_full(p, y - 1)) y--;
                if (x >= y) break;                                 /* else x is empty and y - 1 full, so x < y - 1 */
                memcpy(p->a + x * IP_B, p->a + (y - 1) * IP_B, IP_B * sizeof(double));
                x++; y--;
            }
            for (size_t s = lo; s < hi; s++) full += ipp_full(p, s);    /* how many there were: the moves do not change the count */
        }
        p->w[k] = lo; p->r[k] = lo + full;
    }
}
static void ipp_permute(ipp_t *p, int t) {
    double *cur = p->swp + (size_t)t * 2 * IP_B, *oth = cur + IP_B;
    int nb = p->nbk; job_t *j = p->j;
    for (int q = 0; q < nb; q++) {
        int k = (int)(((size_t)t * (size_t)nb / (size_t)p->P + (size_t)q) % (size_t)nb);
        for (;;) {
            ipp_lock(&p->lock[k]);
            if (p->r[k] <= p->w[k]) { ipp_unlock(&p->lock[k]); break; }
            size_t s = --p->r[k]; atomic_fetch_add_explicit(&p->reading[k], 1, memory_order_relaxed);
            ipp_unlock(&p->lock[k]);
            memcpy(cur, ipp_slot(p, s), IP_B * sizeof(double));
            atomic_fetch_sub_explicit(&p->reading[k], 1, memory_order_release);
            for (;;) {                                               /* carry it home, swapping with whatever is there */
                int g = hf_lookup(j, cur[0], p->mode, p->style);
                ipp_lock(&p->lock[g]);
                size_t s2 = p->w[g]++; int full = s2 < p->r[g];
                ipp_unlock(&p->lock[g]);
                if (full) { double *x = ipp_slot(p, s2); memcpy(oth, x, IP_B * sizeof(double)); memcpy(x, cur, IP_B * sizeof(double)); double *tt = cur; cur = oth; oth = tt; continue; }
                while (atomic_load_explicit(&p->reading[g], memory_order_acquire)) CPU_RELAX();   /* a read of this slot may be in flight */
                memcpy(ipp_slot(p, s2), cur, IP_B * sizeof(double));
                break;
            }
        }
    }
}
static void ipp_aside(ipp_t *p) {                                      /* CLEAN UP, first half: block data past each range, copied aside */
    size_t k;
    while ((k = atomic_fetch_add(&p->next3, 1)) < (size_t)p->nbk) {
        size_t f = p->w[k] - p->d[k], m = 0;
        if (f) {
            size_t lim = p->b[k + 1] < p->nslot * IP_B ? p->b[k + 1] : p->nslot * IP_B, end = p->w[k] * IP_B;
            size_t ge = end < lim ? end : lim;
            if (ge < p->d[k] * IP_B) ge = p->d[k] * IP_B;
            for (size_t q = ge; q < end; q++) p->tmp[k * 2 * IP_B + m++] = ipp_slot(p, q / IP_B)[q % IP_B];
        }
        p->tmpn[k] = m;
    }
}
static void ipp_fill(ipp_t *p) {                                       /* CLEAN UP, second half: every bucket's holes */
    size_t k;
    while ((k = atomic_fetch_add(&p->next4, 1)) < (size_t)p->nbk) {
        size_t bk = p->b[k], be = p->b[k + 1], f = p->w[k] - p->d[k], gs, ge;
        if (f) {
            gs = p->d[k] * IP_B; size_t lim = be < p->nslot * IP_B ? be : p->nslot * IP_B, end = p->w[k] * IP_B;
            ge = end < lim ? end : lim; if (ge < gs) ge = gs;
        } else gs = ge = be;
        double *a = p->a;
        const double *src = p->tmp + k * 2 * IP_B; size_t ns = p->tmpn[k], si = 0, st = 0, sj = 0;
        for (int part = 0; part < 2; part++) {
            size_t o = part ? ge : bk, oe = part ? be : gs;
            for (; o < oe; o++) {
                if (si < ns) { a[o] = src[si++]; continue; }
                while (st < p->nstripe && sj >= p->scnt[st * (size_t)p->nbk + k]) { st++; sj = 0; }
                if (st >= p->nstripe) { atomic_store(&p->fail, 2); return; }   /* the counts disagree: cannot happen; never write past */
                a[o] = p->sbuf[(st * (size_t)p->nbk + k) * IP_B + sj++];
            }
        }
    }
}
static void ipp_leaves(ipp_t *p, int t) {
    size_t k; double *lb = NULL; ctx_t *c = &p->ctxs[t];
    while ((k = atomic_fetch_add(&p->next5, 1)) < (size_t)p->nbk) {
        size_t s = p->b[k], nb = p->b[k + 1] - s;
        if (nb < 2) continue;
        if (!lb) lb = malloc(p->maxb * sizeof(double));            /* none: this bucket and the next take quicksort, below (the array is
                                                                       permuted by now, so this must NOT set p->fail, whose recovery assumes CLASSIFY's layout) */
        ctx_fit_t cf;
        if (lb && ctx_fit(c, nb, &cf)) { double *r = leaf_f64(p->a + s, nb, lb, c); if (r != p->a + s) memcpy(p->a + s, r, nb * sizeof(double)); ctx_unfit(c, &cf); }
        else quicksort(p->a + s, 0, (long)nb - 1, 2 * log2i(nb));   /* no memory: a sort that needs none */
    }
    free(lb);
}
static void ipp_serial(ipp_t *p) {                                     /* between CLASSIFY and COMPACT, on the caller: offsets and areas */
    int nb = p->nbk;
    p->b[0] = 0;
    for (int k = 0; k < nb; k++) { size_t c = 0; for (size_t st = 0; st < p->nstripe; st++) c += p->hist[st * (size_t)nb + k]; p->b[k + 1] = p->b[k] + c; }
    if (p->b[nb] != p->n) { atomic_store(&p->fail, 1); return; }
    size_t maxb = 1;
    for (int k = 0; k <= nb; k++) { p->d[k] = (p->b[k] + IP_B - 1) / IP_B; if (k < nb && p->b[k + 1] - p->b[k] > maxb) maxb = p->b[k + 1] - p->b[k]; }
    p->maxb = maxb;
    for (int t = 0; t < p->P; t++) if (!ctx_init(&p->ctxs[t], maxb, 0)) { atomic_store(&p->fail, 1); return; }
    PROF(for (int t = 0; t < p->P; t++) p->ctxs[t].tid = t;)
}
static void *ipp_worker(void *v) {
    ipp_arg_t *w = v; ipp_t *p = w->p; int t = w->t;
    ipp_classify(p);
    barrier_wait(&p->bar); if (t == 0) ipp_serial(p); barrier_wait(&p->bar);
    if (atomic_load(&p->fail)) return NULL;
    ipp_compact(p);   barrier_wait(&p->bar);
    ipp_permute(p, t); barrier_wait(&p->bar);
    ipp_aside(p);     barrier_wait(&p->bar);
    ipp_fill(p);      barrier_wait(&p->bar);
    if (atomic_load(&p->fail)) return NULL;
    ipp_leaves(p, t);
    return NULL;
}
/* returns 1 sorted; 0 declined BEFORE the array was touched (the histogram path follows). */
static int ipp_partition(job_t *j, int P, ctx_t *ctxs, pthread_t *th, int mode, int style) {
    size_t n = j->n; int nb = j->nbk;
    if (P < 2 || n < (size_t)P * IP_B * 4) return 0;
    ipp_t p; memset(&p, 0, sizeof p);
    p.j = j; p.P = P; p.nbk = nb; p.mode = mode; p.style = style; p.n = n; p.a = j->src; p.ctxs = ctxs;
    p.nslot = n / IP_B; p.nstripe = (size_t)P;
    p.stripe = ((n / (size_t)P) / IP_B) * IP_B; if (p.stripe < IP_B) return 0;
    p.sbuf = malloc(p.nstripe * (size_t)nb * IP_B * sizeof(double));
    p.scnt = calloc(p.nstripe * (size_t)nb, sizeof(uint16_t)); p.hist = calloc(p.nstripe * (size_t)nb, sizeof(size_t));
    p.wfull = calloc(p.nstripe, sizeof(size_t));
    p.b = calloc((size_t)nb + 1, sizeof(size_t)); p.d = calloc((size_t)nb + 1, sizeof(size_t));
    p.w = calloc((size_t)nb + 1, sizeof(size_t)); p.r = calloc((size_t)nb + 1, sizeof(size_t));
    p.lock = calloc((size_t)nb, sizeof(atomic_int)); p.reading = calloc((size_t)nb, sizeof(atomic_int));
    p.ovf = malloc(IP_B * sizeof(double)); p.swp = malloc((size_t)P * 2 * IP_B * sizeof(double));
    p.tmp = malloc((size_t)nb * 2 * IP_B * sizeof(double)); p.tmpn = calloc((size_t)nb, sizeof(size_t));
    ipp_arg_t *wa = calloc((size_t)P, sizeof(ipp_arg_t));
    int sorted = 0;
    if (p.sbuf && p.scnt && p.hist && p.wfull && p.b && p.d && p.w && p.r && p.lock && p.reading && p.ovf && p.swp && p.tmp && p.tmpn && wa) {
        for (int k = 0; k < nb; k++) { atomic_init(&p.lock[k], 0); atomic_init(&p.reading[k], 0); }
        atomic_init(&p.next1, 0); atomic_init(&p.next2, 0); atomic_init(&p.next3, 0); atomic_init(&p.next4, 0); atomic_init(&p.next5, 0);
        atomic_init(&p.fail, 0);
        barrier_init(&p.bar, P);
        for (int t = 0; t < P; t++) { wa[t].p = &p; wa[t].t = t; }
        int made = par_spawn(th, P, ipp_worker, wa, sizeof wa[0], &p.bar, NULL);
        ipp_worker(&wa[0]);
        par_join(th, made);
        barrier_destroy(&p.bar);
        /* a failure in the serial step (ctx_init) leaves the array CLASSIFIED but not permuted: blocks plus gaps, the partial
           buffers holding the rest. Put it back together (every element is in a block or a buffer) and sort it the plain way. */
        if (atomic_load(&p.fail) == 2) { quicksort(p.a, 0, (long)n - 1, 2 * log2i(n)); sorted = 1; }   /* defensive: never seen */
        else if (atomic_load(&p.fail)) {
            size_t o = 0;
            double *a = p.a;
            for (size_t st = 0; st < p.nstripe; st++) {               /* full blocks first, moved left over the gaps */
                size_t lo = st * p.stripe, e = p.wfull[st];
                if (lo != o) memmove(a + o, a + lo, (e - lo) * sizeof(double));
                o += e - lo;
            }
            for (size_t st = 0; st < p.nstripe; st++) for (int k = 0; k < nb; k++) {
                size_t c = p.scnt[st * (size_t)nb + k]; memcpy(a + o, p.sbuf + (st * (size_t)nb + k) * IP_B, c * sizeof(double)); o += c;
            }
            quicksort(a, 0, (long)n - 1, 2 * log2i(n));
            for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
            sorted = 1;
        } else {
            sorted = 1;
            atomic_fetch_add_explicit(&g_ip_parts, 1, memory_order_relaxed);
        }
    }
    free(p.sbuf); free(p.scnt); free(p.hist); free(p.wfull); free(p.b); free(p.d); free(p.w); free(p.r); free(p.lock); free(p.reading);
    free(p.ovf); free(p.swp); free(p.tmp); free(p.tmpn); free(wa);
    return sorted;
}
/* returns 1 if the array was sorted, 0 if declined (a[] untouched; the histogram path follows) */
static int hf_partition(job_t *j, int P, ctx_t *ctxs, pthread_t *th, int lazy, size_t *vstart, int *vnbk, double *vmin, double *vmax) {
    size_t n = j->n; int nbk = j->nbk;
    /* One value holding more than a region can hold overflows it however the splitters fall, and the
     * spill test would then fail after a whole pass over the data. The sample already knows the largest
     * share any single value has, so decline here instead. */
    hv_t *hv = (!lazy && g_part_inplace) ? NULL : j->hv;                /* the in-place partition moves every element */
    double dupf = hv ? hv->rest : j->dup_frac;
    if (dupf * (double)nbk > 1.0 + HF_SLACK) { TRACE("hf: declined up front, one value is %.1f%% of the data", dupf * 100.0); return 0; }
    hf_t h; memset(&h, 0, sizeof h); h.j = j; h.P = P;
    if (!lazy && g_part_inplace) {                                     /* no region buffer: the block permutation (ipp_partition) */
        hf_trial(&h);
        TRACE("ip: trial mode %d style %d", h.mode, h.style);
        if (ipp_partition(j, P, ctxs, th, h.mode, h.style)) return 1;
        memset(&h, 0, sizeof h); h.j = j; h.P = P;
    }
    h.cap = (size_t)((double)(n / (size_t)nbk + 1) * (1.0 + HF_SLACK)) + HF_BLOCK;
    h.region = (h.cap + 7) & ~(size_t)7;
    size_t need = h.region * (size_t)nbk;
    /* THE POOL'S BLOCK FIRST (2026-10-24, the second memory round, FINDINGS 3bq). The regions had a buffer of their own, kept
       between calls like the pool, so a process that sorted with 14 threads and then took any route that places into the pool
       (relocation, the knot route, near-sorted, the tail merge) held BOTH: 17-20 bytes per element resident between calls in a
       mixed sequence (mixprobe) against 8.7 for either alone. The pool is free here (the dispatcher takes it lazily, for the
       routes that place), so the regions borrow its block, 64-byte aligned inside it, and give it back before returning; the
       buffer of their own remains for when the pool is busy. LOGOS_HF_OWN=1: always their own, for the A/B. */
    void *hpb = NULL; uint8_t *hpbi = NULL;
    int hpooled = !getenv("LOGOS_HF_OWN") && pool_acquire(need * sizeof(double) + 64, 0, &hpb, &hpbi);
    if (hpooled) { h.rb = (double *)(((uintptr_t)hpb + 63) & ~(uintptr_t)63); atomic_fetch_add_explicit(&g_hf_pooled, 1, memory_order_relaxed); }
    else {
        /* grown, never preserved: the regions are overwritten before they are read, so this frees and
           reallocates rather than realloc()ing, which lets it come back 2 MB aligned (see big_alloc). */
        if (need > hf_buf_cap) {
            /* 64-byte aligned even when big_alloc falls through to malloc: `region` is a multiple of 8
               doubles, so an aligned base keeps every region start aligned and the streaming prologue
               empty for all the full-block flushes. */
            /* It used to ask big_alloc first and re-ask posix_memalign if the block was not 64-byte aligned, freeing the first
               afterwards. glibc serves a block this large from mmap with a 16-byte header, so it is NEVER aligned: every cold
               parallel sort held the 12.8 bytes per element twice at its peak (make memprobe: 34.9 -> 22.1 aux at 10M and 14
               threads). Aligned first, and the old block goes before the new one is asked for. */
            free(hf_buf); hf_buf = NULL; hf_buf_cap = 0;
            double *nbuf = NULL;
    #if defined(LOGOS_HUGEPAGE) && defined(MADV_HUGEPAGE)
            nbuf = (double *)big_alloc(need * sizeof(double));                   /* 2 MB aligned when it works, which is 64-byte aligned */
            if (nbuf && (((uintptr_t)nbuf) & 63u)) { free(nbuf); nbuf = NULL; }
    #endif
            if (!nbuf && posix_memalign((void **)&nbuf, 64, need * sizeof(double)) != 0) nbuf = NULL;
            if (!nbuf) nbuf = (double *)big_alloc(need * sizeof(double));        /* unaligned beats none: only the streaming prologue cares */
            if (!nbuf) return 0;
            hf_buf = nbuf; hf_buf_cap = need; trim_reset(&hf_trim);
        }
        trim_note(&hf_trim, hf_buf, hf_buf_cap * sizeof(double), need * sizeof(double));
    h.rb = hf_buf;
    }
    TRACE("hf: regions in %s", hpooled ? "the pool's block" : "their own buffer");
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
        if (hv) {                                                     /* each heavy value's bucket under the map just chosen; the maps are monotone, so value order is bucket order */
            memset(hv->lo, 0, sizeof hv->lo); memset(hv->hi, 0, sizeof hv->hi); int pb = -1, okm = 1;
            for (int q = 0; q < hv->n; q++) { int b = hf_lookup(j, hv->v[q], h.mode, h.style); if (b < pb || b < 0 || b >= nbk) { okm = 0; break; } if (b != pb) hv->lo[b] = (uint8_t)q; hv->hi[b] = (uint8_t)(q + 1); pb = b; hv->tot[q] = 0; }
            if (okm) { h.hv = hv; atomic_fetch_add_explicit(&g_hf_heavy, 1, memory_order_relaxed); TRACE("hf: %d heavy values counted in the scatter, the largest left is %.2f%% of the sample", hv->n, hv->rest * 100.0); }
            else if (j->dup_frac * (double)nbk > 1.0 + HF_SLACK) h.declined = 1;   /* a map that is not monotone: cannot happen; never count on it */
        }
        if (h.mode == 9 && j->mdx) atomic_fetch_add_explicit(&g_mdgrid_scatters, 1, memory_order_relaxed);   /* logos_twist_last(18) */
#if defined(LOGOS_TRACE) && defined(LOGOS_STREAM)
        atomic_store(&g_streamed, 0);
#endif
        barrier_init(&h.bar, P); atomic_init(&h.next, 0); atomic_init(&h.bnext, 0);
        for (int t = 0; t < P; t++) { wa[t].h = &h; wa[t].t = t; wa[t].ctx = &ctxs[t]; }
        int hf_made = par_spawn(th, P, hf_worker, wa, sizeof wa[0], &h.bar, NULL);   /* queue-driven throughout: an orphan is only dropped from the barrier */
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
                { size_t *fill = calloc(nbk, sizeof(size_t));
                  if (!fill) h.declined = 1;                  /* unchecked until 2026-09-20; a[] is untouched here, so declining is free */
                  else { for (int t = 0; t < P; t++) for (size_t i = 0; i < wa[t].spn; i++) { int b = wa[t].sp[i].b; h.spv[h.spo[b] + fill[b]++] = wa[t].sp[i]; } free(fill); } }
                size_t maxb = 1, lsz[MAX_BUCKETS];
                if (h.hv) for (int q = 0; q < h.hv->n; q++) { size_t c = 0; for (int t = 0; t < P; t++) c += wa[t].hvc[q]; h.hv->tot[q] = c; }
                for (int b = 0; b < nbk; b++) { size_t z = h.cnt[b] + h.spc[b], hz = 0; if (h.hv) for (int q = h.hv->lo[b]; q < h.hv->hi[b]; q++) hz += h.hv->tot[q];
                    lsz[b] = z; h.bstart[b + 1] = h.bstart[b] + z + hz; if (z > maxb) maxb = z; }   /* a leaf sorts the others; the heavy runs are filled in */
                hf_order(h.order, lsz, nbk);                          /* biggest leaf first: the last one picked up sets the finish time */
                TRACE("par: leaf sizes max %zu mean %zu -> imbalance %.2fx", lsz[h.order[0]], n / (size_t)nbk, (double)lsz[h.order[0]] * (double)nbk / (double)n);
                if (h.bstart[nbk] != n) h.declined = 1;
                for (int t = 0; !h.declined && t < P; t++) if (!ctx_init(&ctxs[t], maxb, 0)) h.declined = 1;
                for (int t = 0; !h.declined && t < P; t++) ctxs[t].seq = j->seq_leaf;   /* the sequential leaf's policy, AVX2 quicksort included */
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
        par_join(th, hf_made);
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
    if (hpooled) pool_release();
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
                 uint64_t hmul; const uint64_t *hkey; const uint8_t *slot; size_t sd, *hcnt;   /* hmul != 0: verified perfect hash from the sample */
                 const double *hvals; int vec; } lcmap_t;   /* vec: count the sample's sd values with vector compares first (lc_vec_count) */
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_NO_LCVEC)
#define LC_VEC_MAX 8
static atomic_size_t g_lcvec_elems;   /* logos_twist_last(17): elements the vector count settled; reading clears it */
/* A HANDFUL OF VALUES, COUNTED IN REGISTERS (2026-10-07, make scoreboard): with 2 to 5 distinct values the hash loop below runs
   at ~1.4 ns per element, and the cost is not the hash: `hc[sl]++` on one of two counters chains every increment through
   store-to-load forwarding, while x86-simd-sort finishes two values in one partition pass (0.59 ns). Here each 4-lane vector is
   compared with every value's BIT PATTERN (an integer compare: -0.0 and +0.0 stay apart, a NaN matches nothing) and each lane
   counts in its own 64-bit register, so no counter is ever in memory. The first vector that holds anything else stops it, and
   the scalar loop resumes there with the counts so far: a value the sample missed, a NaN, everything after them, as before.
   Returns where it stopped; adds into cnt[0..sd). */
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")   /* the string the intrinsics header was first read under: see hf_stream_copy */
static size_t lc_vec_count(const double *a, size_t lo, size_t hi, const double *vals, int sd, size_t *cnt) {
    __m256i b[LC_VEC_MAX], c[LC_VEC_MAX];   /* a broadcast copies a double's bits exactly: the patterns compared below */
    for (int j = 0; j < sd; j++) { b[j] = _mm256_castpd_si256(_mm256_set1_pd(vals[j])); c[j] = _mm256_setzero_si256(); }
    size_t i = lo;
    for (; i + 8 <= hi; i += 8) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(a + i)), v1 = _mm256_loadu_si256((const __m256i *)(a + i + 4));
        __m256i m0 = _mm256_setzero_si256(), m1 = _mm256_setzero_si256();
        for (int j = 0; j < sd; j++) {
            __m256i e0 = _mm256_cmpeq_epi64(v0, b[j]), e1 = _mm256_cmpeq_epi64(v1, b[j]);
            c[j] = _mm256_sub_epi64(c[j], _mm256_add_epi64(e0, e1));      /* each match is -1 in its lane */
            m0 = _mm256_or_si256(m0, e0); m1 = _mm256_or_si256(m1, e1);
        }
        if (_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_and_si256(m0, m1))) != 0xF) {   /* something else in these eight: undo them */
            for (int j = 0; j < sd; j++) {
                __m256i e0 = _mm256_cmpeq_epi64(v0, b[j]), e1 = _mm256_cmpeq_epi64(v1, b[j]);
                c[j] = _mm256_add_epi64(c[j], _mm256_add_epi64(e0, e1));
            }
            break;
        }
    }
    for (int j = 0; j < sd; j++) { uint64_t t[4]; _mm256_storeu_si256((__m256i *)t, c[j]); cnt[j] += (size_t)(t[0] + t[1] + t[2] + t[3]); }
    return i;
}
#pragma GCC pop_options
#endif
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
        size_t i0 = m->lo;
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_NO_LCVEC)
        if (m->vec) { i0 = lc_vec_count(m->a, m->lo, m->hi, m->hvals, (int)m->sd, hc); atomic_fetch_add_explicit(&g_lcvec_elems, i0 - m->lo, memory_order_relaxed); }
#endif
        for (size_t i = i0; i < m->hi; i++) {
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
typedef struct { double *a; size_t lo, hi; const double *gv; const size_t *gstart; size_t gd; int tid; int nt; } lcemit_t;
/* THE EMIT STREAMS (2026-10-07, make scoreboard): the counting routes' second pass only WRITES, but an ordinary store reads its
   line first, and after the count pass an array larger than the cache is no longer in it, so the emit moved every line twice
   (two values at one thread: 1.00 ns per element against x86-simd-sort's 0.66, whose in-place partition writes lines it has just
   read). Runs of LC_NT_MIN_RUN and more, in arrays of LC_NT_MIN_N and more, go out with non-temporal stores; smaller arrays keep
   the cache-friendly stores, since a caller who reads the result next would find it evicted. LOGOS_LC_EMIT_PLAIN=1: the A/B. */
#ifndef LC_NT_MIN_N
#define LC_NT_MIN_N 2097152
#endif
#ifndef LC_NT_MIN_RUN
#define LC_NT_MIN_RUN 512
#endif
#ifdef LOGOS_HAVE_SSE2
static void lc_fill_nt(double *p, size_t k, double val) {
    size_t i = 0;
    while (i < k && ((uintptr_t)(p + i) & 15)) p[i++] = val;
    __m128d v = _mm_set1_pd(val);
    for (; i + 8 <= k; i += 8) { _mm_stream_pd(p + i, v); _mm_stream_pd(p + i + 2, v); _mm_stream_pd(p + i + 4, v); _mm_stream_pd(p + i + 6, v); }
    for (; i + 2 <= k; i += 2) _mm_stream_pd(p + i, v);
    for (; i < k; i++) p[i] = val;
}
#endif
static void *lc_emit_body(void *p);
static void *lc_emit(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = lc_emit_body(p); PROF(prof_busy_since(((lcemit_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *lc_emit_body(void *p) {
    lcemit_t *e = p;
    size_t v; { size_t lo = 0, hi = e->gd; while (lo < hi) { size_t mid = (lo + hi) >> 1; if (e->gstart[mid + 1] <= e->lo) lo = mid + 1; else hi = mid; } v = lo; }
    size_t pos = e->lo;
#ifdef LOGOS_HAVE_SSE2
    if (e->nt) {
        while (pos < e->hi && v < e->gd) { size_t end = e->gstart[v + 1]; if (end > e->hi) end = e->hi; double val = e->gv[v];
            if (end - pos >= LC_NT_MIN_RUN) { lc_fill_nt(e->a + pos, end - pos, val); pos = end; } else for (; pos < end; pos++) e->a[pos] = val;
            v++; }
        _mm_sfence();                                         /* the streamed lines are visible before the join */
        return NULL;
    }
#endif
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
    /* NaNs to the tail by SWAPPING, so every bit pattern survives. It used to compact the numbers and then write the canonical
       NAN over the tail. Harmless on a sample; but the histogram-free leaf sorts its spills with this, and those are the
       caller's data: a signalling NaN, a negative NaN or a payload came back as another bit pattern, so the output was not a
       permutation of the input (16 of 672 runs of an IEEE probe, 2026-09-20). */
    size_t k = m;
    for (size_t i = 0; i < k; ) { if (s[i] != s[i]) { double v = s[i]; s[i] = s[--k]; s[k] = v; } else i++; }
#ifdef LOGOS_AVX2_LEAF
    if (k >= 2 && avx2_leaf_on() && avx2_qsort_f64(s, k)) return m - k;
#endif
    if (k >= 2) quicksort(s, 0, (long)k - 1, 2 * log2i(k));
    return m - k;
}
/* returns 1 if it sorted a[], 0 if it declined (a[] untouched) */
static int lowcard_try_f64(double *a, size_t n, int P, const double *ext, size_t ext_m) {
    P = count_threads_hash(n, P);   /* the ceiling is this route's alone: see count_threads_hash */
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
    /* Numerically equal values with different bits are the two zeros (the sample holds no NaN): they compare equal, so the sort can
       interleave them, and both must become entries or the one left out misses every lookup (and stops the vector count at once). */
    for (size_t i = 0; i < sm; i++) {
        int add = sd == 0 || S[i] != sv[sd - 1];
        if (!add) { uint64_t k = total_key(S[i]); add = 1; for (size_t q = sd; q-- > 0; ) if (sk[q] == k) { add = 0; break; } }   /* from the back: a duplicate is almost always the last entry */
        if (add) { if (sd == LOWCARD_SAMPLE_MAX || sd == 255) { sd = 0; break; } sv[sd] = S[i]; sk[sd] = total_key(S[i]); sd++; }
    }
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
    int vec = 0;
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_NO_LCVEC)
    vec = hmul && sd >= 1 && sd <= LC_VEC_MAX && avx2_leaf_on() && !getenv("LOGOS_NO_LCVEC");
#endif
    TRACE("lowcard: sample %zu values, %zu distinct, perfect hash %s, vector count %s", sm, sd, hmul ? "built" : "none", vec ? "on" : "off");
    (void)S; (void)sm; (void)sv; (void)sk;

    size_t chunk = (n + P - 1) / P;
    lcmap_t *mp = calloc((size_t)P, sizeof(lcmap_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!mp || !th) { free(mp); free(th); return 0; }
    int ok = 1;
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        mp[t].keys = malloc(LOWCARD_CAP * sizeof(uint64_t)); mp[t].vals = malloc(LOWCARD_CAP * sizeof(double)); mp[t].cnts = malloc(LOWCARD_CAP * sizeof(size_t));
        if (!mp[t].keys || !mp[t].vals || !mp[t].cnts) ok = 0;
        if (hmul) { mp[t].hmul = hmul; mp[t].hkey = sk; mp[t].slot = slot; mp[t].sd = sd; mp[t].hcnt = calloc(sd, sizeof(size_t)); if (!mp[t].hcnt) ok = 0;
                    mp[t].hvals = sv; mp[t].vec = vec; }
    }
    int sorted = 0;
    if (ok) {
        par_run(th, P, lc_map, mp, sizeof mp[0], 1);
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
                    for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * chunk; em[t].hi = em[t].lo + chunk; if (em[t].hi > n) em[t].hi = n; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; em[t].nt = n >= LC_NT_MIN_N && !getenv("LOGOS_LC_EMIT_PLAIN"); }
                    par_run(th, P, lc_emit, em, sizeof em[0], 1);
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
/* What the sequential map may HOLD when it was let in on an estimate above MC_HASH_CHAO1_SEQ (2026-11-10, FINDINGS 3ca). Chao1 is a
   lower bound and a long tail hides from 384 sample points: US ZIP codes read 1,519 and the map then met 18,923 distinct values in
   100,000 elements, a table past L2 and 2.4x the leaf's time. The gate admits an estimate of at most 3,300, so a map past a small
   multiple of that was let in on a wrong estimate and gives up there. LOGOS_MC_SEQCAP=<k> moves it (0 = no cap, the old engine). */
#ifndef MC_HASH_SEQ_CAP
#define MC_HASH_SEQ_CAP 8192
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
typedef struct { const double *a; size_t lo, hi; hmap_t h; int abort; int tid; size_t cap; } mcmap_t;
static atomic_size_t g_mc_capped;                              /* logos_twist_last(40): sequential hash maps that gave up at MC_HASH_SEQ_CAP */
static void *mc_map_body(void *p);
static void *mc_map(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = mc_map_body(p); PROF(prof_busy_since(((mcmap_t *)p)->tid, g_prof_phase, s0);) return r; }
static void *mc_map_body(void *p) {
    mcmap_t *m = p;
    for (size_t i = m->lo; i < m->hi; i++) {
        double x = m->a[i];
        if (x != x || !hmap_add(&m->h, total_key(x), x, 1, m->cap)) {
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
/* THE MAP PHASE CLAIMS CHUNKS (2026-09-22). Static slices (`lo = t*n/P`) assume the threads are equal; this box is a
 * hybrid CPU (6 performance cores, 8 efficient ones) and the profile's per-thread busy times inside this phase are
 * BIMODAL: 4.40 ms against 2.99 on quantized, 5.09 against 3.54 on mod_card_30k, 5.24 against 2.51 on few_distinct --
 * while the one phase that already claims work from a queue, the partition's leaf sorts, reads 10.99 against 10.53
 * (a ratio of 1.04). A statically chunked phase ends when its SLOWEST thread ends, so a third of this phase's capacity
 * was idle. Chunks of n/(4P) are claimed with one atomic each, and an abort stops the others at their next claim
 * instead of after their whole slice. LOGOS_LAT_STATIC_MAP=1 restores the slices: the A/B. */
typedef struct { const double *a; size_t lo, hi; double base, step; long margin, slots; lslots_t t; int abort; int tid;
                 atomic_size_t *next; atomic_int *abort_all; size_t n, cstep;
                 double *sp; size_t spn, spalloc, spcap; atomic_size_t *sptot; } lattice_arg_t;   /* this thread's spill list; sptot NULL = abort instead */
/* A VALUE OFF THE LATTICE IS SET ASIDE, NOT A REASON TO DECLINE (2026-10-31, the boundary round, FINDINGS 3br). The map used to give
 * up at the first element off the lattice, outside the window or sharing a lattice point with a different double, which a sample
 * cannot rule out: a lattice whose last 0.05% lies off it was mapped almost to its end and then sorted the slow way (0.80-0.85 of
 * the route off at one thread, 0.70-0.81 at two). Such an element now goes to its thread's side list, taken at its first use; the
 * threads share one budget of LAT_SPILL_DIV of the array (spcap, counted in *sptot). Before the emit the lists are gathered into
 * one; after it, that list is sorted on its own and merged in from the top. A NaN still declines. LOGOS_LAT_NO_SPILL=1. */
#ifndef LAT_SPILL_DIV
#define LAT_SPILL_DIV 32
#endif
static atomic_size_t g_lat_spilled;    /* logos_twist_last(37): elements the lattice route set aside and merged back in */
static int lat_spill(lattice_arg_t *m, double x) {
    if (!m->sptot || atomic_fetch_add_explicit(m->sptot, 1, memory_order_relaxed) >= m->spcap) return 0;
    if (m->spn == m->spalloc) {
        size_t na = m->spalloc ? 2 * m->spalloc : 4096; if (na > m->spcap) na = m->spcap; if (na <= m->spn) na = m->spn + 1;
        double *p = realloc(m->sp, na * sizeof(double)); if (!p) return 0;
        m->sp = p; m->spalloc = na;
    }
    m->sp[m->spn++] = x; return 1;
}
static void *lattice_map_body(void *p);
static void *lattice_map(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = lattice_map_body(p); PROF(prof_busy_since(((lattice_arg_t *)p)->tid, g_prof_phase, s0);) return r; }
static int g_lat_serial_tables = 0;   /* LOGOS_LAT_SERIAL_TABLES=1: the caller allocates every thread's table, as it used to */
static int g_lat_static_map = 0;      /* LOGOS_LAT_STATIC_MAP=1: the map takes equal slices again, as it used to */
/* Each worker allocates and initialises ITS OWN table. It used to be the caller's loop: P mallocs of three arrays plus a
 * memset of `slots` keys per thread, all before the spawn, which at 8M and 14 threads is 5.6 MB of serial writes inside the
 * phase the trace puts at ~80% of this route. A failed allocation sets `abort`, which the caller already reads. */
static int lattice_table_init(lattice_arg_t *m) {
    if (m->t.keys) return 1;
    m->t.keys = malloc((size_t)m->slots * sizeof(uint64_t));
    m->t.vals = malloc((size_t)m->slots * sizeof(double));
    m->t.cnts = calloc((size_t)m->slots, sizeof(size_t));
    if (!m->t.keys || !m->t.vals || !m->t.cnts) return 0;
    memset(m->t.keys, 0xFF, (size_t)m->slots * sizeof(uint64_t));
    return 1;
}
static int lattice_map_range(lattice_arg_t *m, size_t lo, size_t hi);
static void *lattice_map_body(void *p) {
    lattice_arg_t *m = p;
    if (!lattice_table_init(m)) { m->abort = 1; if (m->abort_all) atomic_store(m->abort_all, 1); return NULL; }
    if (!m->next) { if (!lattice_map_range(m, m->lo, m->hi)) m->abort = 1; return NULL; }   /* LOGOS_LAT_STATIC_MAP=1 */
    for (;;) {
        if (atomic_load_explicit(m->abort_all, memory_order_relaxed)) return NULL;
        size_t c = atomic_fetch_add(m->next, 1), lo = c * m->cstep;
        if (lo >= m->n) return NULL;
        size_t hi = lo + m->cstep; if (hi > m->n) hi = m->n;
        if (!lattice_map_range(m, lo, hi)) { m->abort = 1; atomic_store(m->abort_all, 1); return NULL; }
    }
}
/* THE LATTICE MAP IN VECTORS (2026-10-08, make scoreboard's 256-distinct cell at one thread: 0.845 of x86-simd-sort). Per element
 * the map scales, rounds, tests the residue, indexes a slot and compares a key: all of it data-parallel, then one count. Four at a
 * time here: an FMA, a round, the residue and NaN test as one unordered compare, the slot range as two compares, the keys of the
 * four slots by one gather against the four elements' total-order keys. A vector that passes all of it adds its four counts; any
 * other vector (a slot not yet seen, an off-lattice value, a NaN, two doubles on one point) takes the scalar step below for its
 * four elements, which is the shipped loop's body, so the verdicts and the tables are the same. Rounding differs from the scalar
 * cast only at a residue of exactly one half, which both reject. LOGOS_NO_LATVEC=1 runs the scalar loop. */
static int g_no_latvec = 0;
static __attribute__((unused)) atomic_size_t g_latvec_vecs;     /* logos_twist_last(23): vectors of four the vector map counted without the scalar step */
static inline int lattice_step(lattice_arg_t *m, double x, double inv) {
    if (x != x) return 0;
    double q = (x - m->base) * inv; double qi = (double)(long)(q + (q >= 0 ? 0.5 : -0.5));
    double err = q - qi; if (err < 0) err = -err;
    double tol = 1e-9 * (qi >= 0 ? qi : -qi) + 1e-9;
    if (err > tol) return lat_spill(m, x);
    long idx = (long)qi + m->margin; if (idx < 0 || idx >= m->slots) return lat_spill(m, x);
    uint64_t k = total_key(x), kk = m->t.keys[idx];
    if (kk == k) { m->t.cnts[idx]++; return 1; }
    if (kk != UINT64_MAX) return lat_spill(m, x);
    m->t.keys[idx] = k; m->t.vals[idx] = x; m->t.cnts[idx] = 1;
    return 1;
}
#ifdef LOGOS_AVX2_LEAF
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
static int lattice_map_vec(lattice_arg_t *m, size_t lo, size_t hi, double inv) {
    const __m256d vbase = _mm256_set1_pd(m->base), vinv = _mm256_set1_pd(inv), e9 = _mm256_set1_pd(1e-9);
    const __m256d absm = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7fffffffffffffffLL));
    const __m256d vmarg = _mm256_set1_pd((double)m->margin), vslots = _mm256_set1_pd((double)m->slots), zero = _mm256_setzero_pd();
    const __m256i sbit = _mm256_set1_epi64x((long long)0x8000000000000000ULL), z = _mm256_setzero_si256();
    const long long *keys = (const long long *)m->t.keys; size_t *cnts = m->t.cnts;
    size_t i = lo, fast = 0;
    for (; i + 4 <= hi; i += 4) {
        __m256d x = _mm256_loadu_pd(m->a + i);
        __m256d q = _mm256_mul_pd(_mm256_sub_pd(x, vbase), vinv);
        __m256d qi = _mm256_round_pd(q, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
        __m256d err = _mm256_and_pd(_mm256_sub_pd(q, qi), absm);
        __m256d tol = _mm256_fmadd_pd(_mm256_and_pd(qi, absm), e9, e9);
        __m256d qm = _mm256_add_pd(qi, vmarg);
        __m256d ok = _mm256_and_pd(_mm256_cmp_pd(err, tol, _CMP_LE_OQ),                 /* false on a NaN */
                     _mm256_and_pd(_mm256_cmp_pd(qm, zero, _CMP_GE_OQ), _mm256_cmp_pd(qm, vslots, _CMP_LT_OQ)));
        if (_mm256_movemask_pd(ok) == 15) {
            __m128i idx = _mm256_cvttpd_epi32(qm);
            __m256i kk = _mm256_i32gather_epi64(keys, idx, 8);
            __m256i u = _mm256_castpd_si256(x);
            __m256i k = _mm256_xor_si256(u, _mm256_or_si256(_mm256_cmpgt_epi64(z, u), sbit));   /* total_key, four at once */
            if (_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(kk, k))) == 15) {
                cnts[_mm_cvtsi128_si32(idx)]++; cnts[_mm_extract_epi32(idx, 1)]++;
                cnts[_mm_extract_epi32(idx, 2)]++; cnts[_mm_extract_epi32(idx, 3)]++;
                fast++;
                continue;
            }
        }
        for (size_t j = i; j < i + 4; j++) if (!lattice_step(m, m->a[j], inv)) return 0;
    }
    atomic_fetch_add_explicit(&g_latvec_vecs, fast, memory_order_relaxed);
    for (; i < hi; i++) if (!lattice_step(m, m->a[i], inv)) return 0;
    return 1;
}
#pragma GCC pop_options
#endif
/* one chunk into this thread's own table; 0 = this data is not a lattice (the caller declines) */
static int lattice_map_range(lattice_arg_t *m, size_t lo, size_t hi) {
    const double inv = 1.0 / m->step;
#ifdef LOGOS_AVX2_LEAF
    if (!g_no_latvec && avx2_leaf_on()) return lattice_map_vec(m, lo, hi, inv);
#endif
    for (size_t i = lo; i < hi; i++) {
        double x = m->a[i]; if (x != x) return 0;
        double q = (x - m->base) * inv; double qi = (double)(long)(q + (q >= 0 ? 0.5 : -0.5));
        double err = q - qi; if (err < 0) err = -err;
        double tol = 1e-9 * (qi >= 0 ? qi : -qi) + 1e-9;
        if (err > tol) { if (lat_spill(m, x)) continue; return 0; }                   /* off-lattice */
        long idx = (long)qi + m->margin; if (idx < 0 || idx >= m->slots) { if (lat_spill(m, x)) continue; return 0; }
        uint64_t k = total_key(x), kk = m->t.keys[idx];
        if (kk == k) { m->t.cnts[idx]++; continue; }
        if (kk != UINT64_MAX) { if (lat_spill(m, x)) continue; return 0; }            /* two doubles on one lattice point */
        m->t.keys[idx] = k; m->t.vals[idx] = x; m->t.cnts[idx] = 1;
    }
    return 1;
}
/* THE LATTICE MERGE, IN PARALLEL (2026-09-22, found by `make amprobe` + `make profile`: the counting and lattice routes
 * spend 98-99% of their wall in one phase where only 6.0-6.9 of 14 threads are busy and NONE are waiting -- the rest have
 * nothing to do, because this merge sat between the two parallel phases and walked `slots x P` on one thread. Withholding
 * threads instead was measured and rejected, `make capbench`.)
 * Slots are value-ordered, so a slot RANGE is an output range: each range is reduced across the P tables independently,
 * then one O(P) prefix over the per-range totals gives every range its offset in gv/gstart, and the ranges are written
 * out in parallel. Ranges are CLAIMED with an atomic, so a worker whose thread could not be created costs speed and never
 * correctness (par_spawn drops it from the barrier; whoever is free takes its range).
 * MEASURED AND NOT SHIPPED (2026-09-22): 1.02-1.12 SLOWER than the serial loop at 4M-16M (`CAP_SWITCH=LOGOS_LAT_PAR_MERGE
 * ./capbench`). The diagnosis that sent me here was wrong, and the trace says why: at 8M and 14 threads the MAP phase is
 * 11-15 ms and this merge is ~2 ms, so it is ~10% of the route and Amdahl caps the prize there; the extra spawn (~60 us a
 * thread), the temp arrays and the second pass over the slots eat it. The idle threads `make profile` sees are NOT here.
 * Kept opt-in behind LOGOS_LAT_PAR_MERGE=1 as the A/B and the record. */
#ifndef LATTICE_PMERGE_MIN_SLOTS
#define LATTICE_PMERGE_MIN_SLOTS 8192
#endif
#ifndef LATTICE_PMERGE_MIN_P
#define LATTICE_PMERGE_MIN_P 4
#endif
typedef struct { lattice_arg_t *mp; int P, t; long slots, nr; double *tv; size_t *tc;
                 size_t *ne, *tot; unsigned char *cf; atomic_int *claim1, *claim2; barrier_t *bar;
                 double *gv; size_t *gstart; } lmerge_t;
static inline long lmerge_lo(long r, long slots, long nr) { return (long)(((size_t)r * (size_t)slots) / (size_t)nr); }
static void *lattice_merge_worker(void *p) {
    lmerge_t *w = p; int P = w->P; long nr = w->nr;
    for (long r = 0; r < nr; r++) {                      /* pass 1: reduce this range across the P tables */
        if (atomic_exchange(&w->claim1[r], 1)) continue;
        long lo = lmerge_lo(r, w->slots, nr), hi = lmerge_lo(r + 1, w->slots, nr);
        size_t ne = 0, tot = 0; unsigned char cf = 0;
        for (long i = lo; i < hi; i++) {
            uint64_t key = UINT64_MAX; size_t cnt = 0; double val = 0;
            for (int q = 0; q < P; q++) { uint64_t kk = w->mp[q].t.keys[i]; if (kk == UINT64_MAX) continue;
                if (key == UINT64_MAX) { key = kk; val = w->mp[q].t.vals[i]; } else if (kk != key) { cf = 1; break; }
                cnt += w->mp[q].t.cnts[i]; }
            if (cf) break;
            w->tv[i] = val; w->tc[i] = cnt;
            if (cnt) { ne++; tot += cnt; }
        }
        w->ne[r] = ne; w->tot[r] = tot; w->cf[r] = cf;
    }
    barrier_wait(w->bar);
    for (long r = 0; r < nr; r++) if (w->cf[r]) return NULL;    /* a slot two threads disagree on: the caller falls back */
    for (long r = 0; r < nr; r++) {                      /* pass 2: write this range's entries at its own offset */
        if (atomic_exchange(&w->claim2[r], 1)) continue;
        size_t ofs = 0, base = 0;
        for (long q = 0; q < r; q++) { ofs += w->ne[q]; base += w->tot[q]; }
        long lo = lmerge_lo(r, w->slots, nr), hi = lmerge_lo(r + 1, w->slots, nr);
        for (long i = lo; i < hi; i++) if (w->tc[i]) { w->gv[ofs] = w->tv[i]; w->gstart[ofs] = base; base += w->tc[i]; ofs++; }
    }
    return NULL;
}
static int g_lat_par_merge = 0;            /* LOGOS_LAT_PAR_MERGE=1 (opt-in: measured slower), read with the other per-sort switches */
/* returns 1 when the parallel merge ran and filled gd/acc; 0 when the caller should run the serial loop */
static int lattice_merge_par(lattice_arg_t *mp, int P, long slots, double *gv, size_t *gstart, pthread_t *th,
                             size_t *pgd, size_t *pacc, int *pconflict) {
    if (P < LATTICE_PMERGE_MIN_P || slots < LATTICE_PMERGE_MIN_SLOTS || !g_lat_par_merge) return 0;
    double *tv = malloc((size_t)slots * sizeof(double)); size_t *tc = malloc((size_t)slots * sizeof(size_t));
    size_t *ne = calloc((size_t)P, sizeof(size_t)), *tot = calloc((size_t)P, sizeof(size_t));
    unsigned char *cf = calloc((size_t)P, 1);
    atomic_int *c1 = calloc((size_t)P, sizeof(atomic_int)), *c2 = calloc((size_t)P, sizeof(atomic_int));
    lmerge_t *wa = calloc((size_t)P, sizeof(lmerge_t));
    if (!tv || !tc || !ne || !tot || !cf || !c1 || !c2 || !wa) { free(tv); free(tc); free(ne); free(tot); free(cf); free(c1); free(c2); free(wa); return 0; }
    for (int q = 0; q < P; q++) { atomic_init(&c1[q], 0); atomic_init(&c2[q], 0); }
    barrier_t bar; barrier_init(&bar, P);
    for (int q = 0; q < P; q++) { lmerge_t w = { mp, P, q, slots, P, tv, tc, ne, tot, cf, c1, c2, &bar, gv, gstart }; wa[q] = w; }
    int made = par_spawn(th, P, lattice_merge_worker, wa, sizeof wa[0], &bar, NULL);
    lattice_merge_worker(&wa[0]);
    par_join(th, made);
    barrier_destroy(&bar);
    int conflict = 0; size_t gd = 0, acc = 0;
    for (int q = 0; q < P; q++) { if (cf[q]) conflict = 1; gd += ne[q]; acc += tot[q]; }
    TRACE("lattice: merge in parallel over %d ranges of %ld slots, %zu distinct%s", P, slots, gd, conflict ? ", CONFLICT -> serial" : "");
    *pconflict = conflict; *pgd = gd; *pacc = acc;
    free(tv); free(tc); free(ne); free(tot); free(cf); free(c1); free(c2); free(wa);
    return !conflict;                                    /* a conflict: nothing was written, the caller runs the serial loop */
}
/* The lattice's step from a sorted sample: the coarsest step that puts every sample point on a lattice through the smallest one, or
 * 0. The smallest gap proposes it, but a sample holding no two ADJACENT lattice points sees a multiple of the step: 384 points (one
 * thread) over 65,536 values miss every adjacent pair about one time in ten, over 262,144 more often than not, and the route then
 * fired by chance (the boundary round, FINDINGS 3br). So the gap's divisors up to LATTICE_STEP_DIV are tried, coarsest first; a
 * coarser lattice than the data's that still holds all m points is vanishingly rare, and the map checks every element anyway.
 * But a FINER one is the danger: one value off the lattice in the sample (0.25 k + 0.1 among multiples of 0.25) makes the
 * coarsest step holding everything 0.05, a lattice five times wider than the data, and the counting block, which has no fill
 * test of its own, mapped it at 5.3 ns per element (8 threads, 1M) against 2.4 for uniform data. So a step that needed a divisor
 * must leave lat_fill_min(P) elements per slot (lattice_try_f64), as the entry below the counting block demands of every step.
 * LOGOS_LAT_DIV1=1 tries the gap alone, as before. *distinct = the sample's distinct values, *div = the divisor used. */
#ifndef LATTICE_STEP_DIV
#define LATTICE_STEP_DIV 16
#endif
#ifndef LAT1_MIN_FILL
#define LAT1_MIN_FILL 16.0             /* elements per lattice slot at one thread; see lat1_gate */
#endif
static inline double lat_fill_min(int P) { return LAT1_MIN_FILL * (double)(P + 1) / 2.0; }
static double lattice_sample_step(const double *samp, size_t m, size_t *distinct, int *div) {
    double smin = samp[0], g = INFINITY; size_t d0 = 1;
    for (size_t i = 1; i < m; i++) if (samp[i] != samp[i - 1]) { double gi = samp[i] - samp[i - 1]; if (gi < g) g = gi; d0++; }
    *distinct = d0; *div = 0;
    if (!(g > 0) || !isfinite(g)) return 0;
    int dmax = getenv("LOGOS_LAT_DIV1") ? 1 : LATTICE_STEP_DIV;
    for (int d = 1; d <= dmax; d++) {
        double step = g / (double)d; size_t i = 0;
        for (; i < m; i++) { double q = (samp[i] - smin) / step; if (!(q < 9e15)) return 0;   /* past 2^53 an index is not exact, and an overflowed quotient cast to long is undefined */
                             double qi = (double)(long)(q + 0.5); double e = q - qi; if (e < 0) e = -e; if (e > 1e-9 * (qi + 1)) break; }
        if (i == m) { *div = d; return step; }
    }
    return 0;
}
/* returns 1 if sorted, 0 if declined (a[] untouched). samp: sorted sample of m values. */
static void lattice_spill_finish(double *a, size_t nl, double *sp, size_t spn);   /* the spills sorted and merged in: with the nested sorts, below */
static int lattice_try_f64(double *a, size_t n, int P, const double *samp, size_t m) {
    /* distinct sample values -> candidate step */
    double smin = samp[0], smax = samp[m - 1]; size_t distinct; int div;
    if (!isfinite(smin) || !isfinite(smax)) return 0;   /* an infinity has no lattice index: (inf - smin) / step cast to long below was undefined */
    double step = lattice_sample_step(samp, m, &distinct, &div);
    if (distinct < 3 || !(step > 0)) return 0;
    if (div > 1 && ((smax - smin) / step + 1.0) * lat_fill_min(P) > (double)n) return 0;   /* a divided step may be a finer lattice than the data's */
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
    PROF(int ph_save_outer = g_prof_phase;)
    /* the map claims chunks unless LOGOS_LAT_STATIC_MAP=1 asks for the old equal slices: see lattice_arg_t */
    atomic_size_t map_next; atomic_init(&map_next, 0);
    atomic_int map_abort; atomic_init(&map_abort, 0);
    size_t cstep = (n + (size_t)(4 * P) - 1) / (size_t)(4 * P); if (cstep < 4096) cstep = 4096;
    atomic_size_t sptot; atomic_init(&sptot, 0);                                       /* see lat_spill */
    int spill = !getenv("LOGOS_LAT_NO_SPILL");
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        mp[t].base = smin; mp[t].step = step; mp[t].margin = margin; mp[t].slots = slots;
        mp[t].spcap = n / LAT_SPILL_DIV; mp[t].sptot = spill ? &sptot : NULL;
        mp[t].n = n; mp[t].cstep = cstep; mp[t].next = g_lat_static_map ? NULL : &map_next; mp[t].abort_all = &map_abort;
        if (g_lat_serial_tables && !lattice_table_init(&mp[t])) ok = 0;   /* otherwise each worker takes its own, in parallel */
    }
    int sorted = 0;
    if (ok) {
        PROF(g_prof_phase = LOGOS_PH_LAT_MAP; prof_phase(LOGOS_PH_MODCARD);)
        par_run(th, P, lattice_map, mp, sizeof mp[0], 1);
        PROF(prof_phase(LOGOS_PH_LAT_MAP);)
        TRACE("lattice: map done (%d threads, %ld slots)", P, slots);
        int aborted = 0; for (int t = 0; t < P; t++) if (mp[t].abort) aborted = 1;
        if (!aborted) {
            /* merge by index: keys must agree across threads; counts sum. Index order == value order. */
            double *gv = malloc((size_t)slots * sizeof(double)); size_t *gstart = malloc(((size_t)slots + 1) * sizeof(size_t));
            lcemit_t *em = malloc((size_t)P * sizeof(lcemit_t));
            if (gv && gstart && em) {
                size_t gd = 0, acc = 0; int conflict = 0;
                if (!lattice_merge_par(mp, P, slots, gv, gstart, th, &gd, &acc, &conflict)) {
                    gd = 0; acc = 0; conflict = 0;
                    for (long i = 0; i < slots && !conflict; i++) {
                        uint64_t key = UINT64_MAX; size_t cnt = 0; double val = 0;
                        for (int t = 0; t < P; t++) { uint64_t kk = mp[t].t.keys[i]; if (kk == UINT64_MAX) continue;
                            if (key == UINT64_MAX) { key = kk; val = mp[t].t.vals[i]; } else if (kk != key) { conflict = 1; break; }
                            cnt += mp[t].t.cnts[i]; }
                        if (cnt) { gv[gd] = val; gstart[gd] = acc; acc += cnt; gd++; }
                    }
                }
                PROF(prof_phase(LOGOS_PH_LAT_MERGE); g_prof_phase = LOGOS_PH_LAT_EMIT;)
                TRACE("lattice: merge done (serial, walked %ld slots x %d threads = %ld reads)", slots, P, (long)slots * P);
                size_t spn = 0; for (int t = 0; t < P; t++) spn += mp[t].spn;
                size_t nl = n - spn, ech = (nl + P - 1) / P;           /* the lattice's own elements; the spills follow them */
                int spok = 1;                                           /* the threads' lists gathered into thread 0's, before anything is written */
                if (spn > mp[0].spn) {
                    if (mp[0].spalloc < spn) { double *p = realloc(mp[0].sp, spn * sizeof(double)); if (p) { mp[0].sp = p; mp[0].spalloc = spn; } else spok = 0; }
                    if (spok) { size_t o = mp[0].spn; for (int t = 1; t < P; t++) if (mp[t].spn) { memcpy(mp[0].sp + o, mp[t].sp, mp[t].spn * sizeof(double)); o += mp[t].spn; } }
                }
                if (!conflict && acc == nl && spok) {
                    gstart[gd] = acc;
                    for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * ech; em[t].hi = em[t].lo + ech; if (em[t].hi > nl) em[t].hi = nl; if (em[t].lo > nl) em[t].lo = nl; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; em[t].nt = n >= LC_NT_MIN_N && !getenv("LOGOS_LC_EMIT_PLAIN"); }
                    par_run(th, P, lc_emit, em, sizeof em[0], 1);
                    PROF(prof_phase(LOGOS_PH_LAT_EMIT);)
                    TRACE("lattice: emit done (%zu distinct, %zu spilled)", gd, spn);
                    if (spn) { lattice_spill_finish(a, nl, mp[0].sp, spn); atomic_fetch_add_explicit(&g_lat_spilled, spn, memory_order_relaxed); }   /* see lat_spill */
                    sorted = 1;
                }
            }
            free(gv); free(gstart); free(em);
        }
    }
    TRACE("lattice: freeing %d tables of %ld slots (%.1f MB total)", P, slots, (double)P * (double)slots * 24.0 / 1048576.0);
    for (int t = 0; t < P; t++) { free(mp[t].t.keys); free(mp[t].t.vals); free(mp[t].t.cnts); free(mp[t].sp); }
    free(mp); free(th);
    PROF(prof_phase(LOGOS_PH_LAT_FREE); g_prof_phase = ph_save_outer;)
    TRACE("lattice: tables freed");
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
    return n >= TYPED_MIN_N && n <= RADIX64_MAX_N && chao1 <= (double)MC_HASH_CHAO1_RADIX && samp && m && sample_quant_passes(samp, m) <= QUANT_PROBE_MAX ? 2 : 0;   /* 2: capped */
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

    size_t chunk = (n + P - 1) / P, mc_cap = MODCARD_CAP;
    if (allow_hash == 2) { const char *e = getenv("LOGOS_MC_SEQCAP"); size_t v = e ? (size_t)strtoull(e, NULL, 10) : (size_t)MC_HASH_SEQ_CAP; if (v && v < mc_cap) mc_cap = v; }
    mcmap_t *mp = calloc((size_t)P, sizeof(mcmap_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    if (!mp || !th) { free(mp); free(th); return 0; }
    int ok = 1;
    for (int t = 0; t < P; t++) {
        mp[t].a = a; PROF(mp[t].tid = t;) mp[t].lo = (size_t)t * chunk; mp[t].hi = mp[t].lo + chunk; if (mp[t].hi > n) mp[t].hi = n;
        mp[t].cap = mc_cap;
        if (!hmap_init(&mp[t].h, 4096)) ok = 0;                 /* small start; grows with the data, stays cache-resident */
    }
    int sorted = 0;
    if (ok) {
        par_run(th, P, mc_map, mp, sizeof mp[0], 1);
        int aborted = 0; size_t tot = 0; for (int t = 0; t < P; t++) { if (mp[t].abort) aborted = 1; tot += mp[t].h.used; }
        if (aborted && mc_cap < MODCARD_CAP) atomic_fetch_add(&g_mc_capped, 1);
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
                        for (int t = 0; t < P; t++) { oe[t].a = a; PROF(oe[t].tid = t;) oe[t].lo = (size_t)t * chunk; oe[t].hi = oe[t].lo + chunk; if (oe[t].hi > n) oe[t].hi = n; oe[t].gv = ov; oe[t].gstart = os_; oe[t].gd = w; oe[t].nt = n >= LC_NT_MIN_N && !getenv("LOGOS_LC_EMIT_PLAIN"); }
                        par_run(th, P, lc_emit, oe, sizeof oe[0], 1);
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
                for (int t = 0; t < P; t++) { em[t].a = a; PROF(em[t].tid = t;) em[t].lo = (size_t)t * chunk; em[t].hi = em[t].lo + chunk; if (em[t].hi > n) em[t].hi = n; em[t].gv = gv; em[t].gstart = gstart; em[t].gd = gd; em[t].nt = n >= LC_NT_MIN_N && !getenv("LOGOS_LC_EMIT_PLAIN"); }
                par_run(th, P, lc_emit, em, sizeof em[0], 1);
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

/* the histogram-free partition's region buffer exists only when that partition is compiled in (make hfbench builds without it) */
#ifndef LOGOS_NO_HISTFREE
#define HF_RELEASE() do { free(hf_buf); hf_buf = NULL; hf_buf_cap = 0; } while (0)
#else
#define HF_RELEASE() ((void)0)
#endif
#ifndef LOGOS_NO_POOL
static pthread_mutex_t g_pool_m = PTHREAD_MUTEX_INITIALIZER;
static int g_pool_busy = 0;
static void *g_pool_buf = NULL; static size_t g_pool_cap = 0;     /* bytes */
static uint8_t *g_pool_bidx = NULL; static size_t g_pool_bidx_cap = 0;
static trim_t g_pool_trim[2];                                     /* buf, bidx (see trim_note) */
/* acquire: returns 1 if the pooled buffers were handed out (caller must release), 0 if the caller must allocate */
/* TRIED 2026-10-21 and taken out (FINDINGS 3bo): handing the scratch out SKEWED from its base by 64 or 2112 bytes, against 4K
   aliasing (a large malloc is an mmap behind a 16-byte header, so the caller's array and this buffer share an offset in their 4 KB
   pages). 0.93-1.03 on 45 fieldprobe shapes and 23 roundbench guards: nil. A constant skew helps only streams at a FIXED distance,
   and the merges' read and write fronts drift. */
static int pool_acquire(size_t buf_bytes, size_t bidx_bytes, void **buf, uint8_t **bidx) {
    pthread_mutex_lock(&g_pool_m);
    if (g_pool_busy) { pthread_mutex_unlock(&g_pool_m); return 0; }
    if (g_pool_cap < buf_bytes) { free(g_pool_buf); g_pool_buf = big_alloc(buf_bytes); g_pool_cap = g_pool_buf ? buf_bytes : 0; trim_reset(&g_pool_trim[0]); }
    if (bidx_bytes && g_pool_bidx_cap < bidx_bytes) { free(g_pool_bidx); g_pool_bidx = big_alloc(bidx_bytes); g_pool_bidx_cap = g_pool_bidx ? bidx_bytes : 0; trim_reset(&g_pool_trim[1]); }
    if (!g_pool_buf || (bidx_bytes && !g_pool_bidx)) { pthread_mutex_unlock(&g_pool_m); return 0; }
    trim_note(&g_pool_trim[0], g_pool_buf, g_pool_cap, buf_bytes);
    if (bidx_bytes) trim_note(&g_pool_trim[1], g_pool_bidx, g_pool_bidx_cap, bidx_bytes);
    g_pool_busy = 1; *buf = g_pool_buf; *bidx = bidx_bytes ? g_pool_bidx : NULL;
    pthread_mutex_unlock(&g_pool_m);
    return 1;
}
static void pool_release(void) { pthread_mutex_lock(&g_pool_m); g_pool_busy = 0; pthread_mutex_unlock(&g_pool_m); }
/* the end of a top-level sort: the large regions this call did not ask for decay as if asked for nothing. Without it the parallel
   partition's regions stayed resident through any number of one-thread calls, which never acquire them (rssprobe, 14 threads). */
static void trim_idle(void) {
    if (g_pool_trim[0].hi <= TRIM_MIN_BYTES && g_pool_trim[1].hi <= TRIM_MIN_BYTES
#ifndef LOGOS_NO_HISTFREE
        && hf_trim.hi <= TRIM_MIN_BYTES
#endif
        ) return;                                               /* nothing large enough to trim: no lock on small calls */
    pthread_mutex_lock(&g_pool_m);
    if (!g_pool_busy) {
        if (g_pool_trim[0].ep != g_trim_ep) trim_note(&g_pool_trim[0], g_pool_buf, g_pool_cap, 0);
        if (g_pool_trim[1].ep != g_trim_ep) trim_note(&g_pool_trim[1], g_pool_bidx, g_pool_bidx_cap, 0);
#ifndef LOGOS_NO_HISTFREE
        if (hf_trim.ep != g_trim_ep) trim_note(&hf_trim, hf_buf, hf_buf_cap * sizeof(double), 0);
#endif
    }
    pthread_mutex_unlock(&g_pool_m);
}
void logos_release_buffers(void) {
    pthread_mutex_lock(&g_pool_m);
    if (!g_pool_busy) { free(g_pool_buf); g_pool_buf = NULL; g_pool_cap = 0; free(g_pool_bidx); g_pool_bidx = NULL; g_pool_bidx_cap = 0;
                        /* and the rest of what is kept between calls: it used to free only the 9 bytes per element above and leave the
                           partition's 12.8 and the contexts behind, which make memprobe's "after release" column showed */
                        HF_RELEASE(); ctx_release_all(); cnti_release_all(); }
    pthread_mutex_unlock(&g_pool_m);
    wp_shutdown();                                              /* and the worker threads */
#ifdef __GLIBC__
    /* and the HEAP's free pages (2026-10-22, the memory round). glibc raises its mmap threshold to the size of any mmap-backed block
       it frees, so after one large sort the next call's blocks of up to that size come from the heap, and a freed heap page stays
       resident until trimmed: two sorts in a row (the dominant-value route's two ends, or a caller's two calls) left 1.0-1.7
       bytes per element resident after this call. Trimming is what "returns all of it" means there. */
    malloc_trim(0);
#endif
}
#else
static int pool_acquire(size_t a, size_t b, void **c, uint8_t **d) { (void)a; (void)b; (void)c; (void)d; return 0; }
static void pool_release(void) {}
static void trim_idle(void) {
#ifndef LOGOS_NO_HISTFREE
    if (hf_trim.ep != g_trim_ep) trim_note(&hf_trim, hf_buf, hf_buf_cap * sizeof(double), 0);
#endif
}
void logos_release_buffers(void) { HF_RELEASE(); ctx_release_all(); cnti_release_all(); wp_shutdown(); }
#endif
/* THE SCRATCH, TAKEN WHEN A ROUTE NEEDS IT (2026-09-26, make memprobe). The parallel dispatcher used to take its 8-byte-per-element
 * scratch and the 1-byte bucket index before any route was chosen, and the route that takes most parallel sorts, the histogram-free
 * partition, never touches either: 9 of the 23 bytes per element a 14-thread sort held were never written. Now the near-sorted
 * route takes the scratch after its window gate, relocation after its plan says something will be placed, and the histogram path
 * when the histogram-free one has declined; a sort that ends on the histogram-free partition or a counting route never takes it.
 * One acquisition per sort at most, released once (scratch_drop); a failure is remembered so nobody asks twice. -DLOGOS_EAGER_SCRATCH
 * takes it up front as before, for the A/B. */
typedef struct { size_t n, bidx_n; void *pb; uint8_t *pbi; double *dst; uint8_t *bidx; int pooled, have, failed; } scratch_t;
static double *scratch_get(scratch_t *s) {
    if (s->have) return s->dst;
    if (s->failed) return NULL;
    s->pooled = pool_acquire(s->n * sizeof(double), s->bidx_n, &s->pb, &s->pbi);
    s->dst = s->pooled ? (double *)s->pb : (double *)big_alloc(s->n * sizeof(double));
    s->bidx = s->pooled ? s->pbi : (s->bidx_n ? (uint8_t *)big_alloc(s->bidx_n) : NULL);   /* optional: the histogram path recomputes bucket_of without it */
    if (!s->dst) { if (s->pooled) pool_release(); else free(s->bidx); s->pooled = 0; s->bidx = NULL; s->failed = 1; return NULL; }
    s->have = 1; return s->dst;
}
static void scratch_drop(scratch_t *s) {
    if (!s->have) return;
    if (s->pooled) pool_release(); else { free(s->bidx); free(s->dst); }
    s->have = 0; s->dst = NULL; s->bidx = NULL;
}


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
static void ns_split_scan(ns_chunk_arg_t *w);
static void *ns_split_sort(void *p) {
    ns_chunk_arg_t *w = p;
    PROF(prof_stamp_t s0 = prof_stamp(); w->ctx->path = LOGOS_PATH_RUN_MERGE;)
    ns_split_scan(w);
    PROF(double ms = prof_busy_since(w->ctx->tid, LOGOS_PH_NEARSORTED, s0); prof_path(w->ctx->tid, w->ctx->path, w->hi - w->lo, ms);)
    return NULL;
}
static void ns_split_scan(ns_chunk_arg_t *w) {
    size_t lo = w->lo, hi = w->hi;
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
#ifndef NS_WIDE_MIN_N
#define NS_WIDE_MIN_N      6000000   /* from here on the near-sorted gate admits up to NEARLY_SORTED_INV_RATIO (5%); below, NS_INV_RATIO_SMALL */
#endif
#ifndef NS_INV_RATIO_SMALL
#define NS_INV_RATIO_SMALL 0.025
#endif
static _Thread_local int ns_depth = 0;
/* A NESTED sort's context bundles are free again when it returns (2026-10-24, the second memory round, FINDINGS 3bq). The bundle
   cursor restarts only at a top-level sort, so every nested sort of one call (the wave route sorts each of its ~65 pieces with
   one; the stray sorts; the near-sorted route's strays) took a FRESH bundle and kept it: the sine wave held 2.4 bytes per element
   of piece contexts after the call (sinetrace). The short-reach route already rewound the cursor after its own sorts. */
#ifndef LOGOS_NO_CTX_POOL
#define NESTED_SORT(call) do { int cm_ = g_ctx_next; ns_depth++; call; ns_depth--; g_ctx_next = cm_; } while (0)
#else
#define NESTED_SORT(call) do { ns_depth++; call; ns_depth--; } while (0)
#endif
/* The lattice route's spills (see lat_spill), after the emit has written the lattice's own nl elements to a[0, nl): sorted on their
 * own, then merged in from the top. Every lattice element above a spill moves up by the spills still to come, so the writes land at
 * or above every unread position; each spill finds its place by a gallop down from where the last one went, and everything between
 * two spills moves in one memmove (half_strays, with no buffer). */
static void psort_f64(double *a, size_t n, int P, size_t *vstart, int *vnbk, double *vmin, double *vmax);
static void lattice_spill_finish(double *a, size_t nl, double *sp, size_t spn) {
    NESTED_SORT(psort_f64(sp, spn, 1, NULL, NULL, NULL, NULL));
    size_t i = nl;
    for (size_t q = spn; q-- > 0;) {
        double v = sp[q]; size_t p;
        if (i == 0 || !(v < a[i - 1])) p = i;
        else {
            size_t top = i - 1, step = 1, lo;                             /* a[top] > v: find the first index whose element is > v */
            for (;;) { if (step > top) { lo = 0; break; } if (!(v < a[top - step])) { lo = top - step + 1; break; } top -= step; step <<= 1; }
            size_t hi = top;
            while (lo < hi) { size_t mid = lo + ((hi - lo) >> 1); if (v < a[mid]) hi = mid; else lo = mid + 1; }
            p = lo;
        }
        if (p < i) memmove(a + p + q + 1, a + p, (i - p) * sizeof(double));
        a[p + q] = v; i = p;
    }
}
/* THE STRAND GATE (2026-11-06, FINDINGS 3bw; found by the prime-factor knot solver, 3bv). At P > 1 this route claimed sorted runs that
 * lie IN ORDER and overlap their neighbours (batches of a log appended as they close, a few neighbours swapped or late): the sample's
 * lag-144 inversion rate reads 0.000 on them (pairs 0.31% of n apart sit inside one run), the four pre-gate windows lie inside runs
 * and reject nothing, so the route ran, called the whole overlap of every run strays (825,721 of 8.4M on a staircase at overlap 0.2),
 * sorted them through a nested parallel sort and merged every band: 1.3-1.9x SLOWER at 14 threads than the same array at one, and
 * 1.6-3.9x slower than the engine with this route off, where the knot route places the strands. What tells strands from strays is
 * SCALE: disorder of a few displaced elements shows at the element's scale (adjacent descents inside the windows, ~1.5 per displaced
 * element: 180 in 16,384 at 0.5% swaps) and at the array's scale alike; disorder between long sorted stretches shows ONLY at the
 * array's scale (a 2048-point sample in position order reads one descent per strand boundary, 63 on 64 strands; the windows read 0,
 * or 1 where a window straddles a boundary). So: when the windows hold at most NS_STRAND_WIN_DESC adjacent descents in all and the
 * position-ordered sample holds NS_STRAND_MIN_DESC or more, decline: the run routes behind this one are the right ones. Measured on
 * 27 shapes before the rule was written (nsx, the exploration): every shape where the route lost 2-5x reads windows 0 / sample 8-155;
 * every shape where it wins 1.3-3.5x reads windows 180-1100. The sample is read only when the windows are clean (a sorted array with
 * a handful of displaced elements pays it: ~1% of that route). LOGOS_NO_NSSTRAND=1 is the switch; logos_twist_last(38) counts the
 * declines; the harness names the path on both kinds of input. */
#ifndef NS_STRAND_WIN_DESC
#define NS_STRAND_WIN_DESC 4
#endif
#ifndef NS_STRAND_MIN_DESC
#define NS_STRAND_MIN_DESC 4
#endif
#define NS_STRAND_M 2048
typedef struct { size_t pos; double v; } psamp_t;
static psamp_t *sample_by_pos(const double *a, size_t n, size_t m);
static _Atomic size_t g_ns_strand = 0;
/* noinline (2026-09-22): nothing with a large frame may land in the dispatcher's. Between two rounds, with no stack array added,
 * GCC began inlining this route (a 33 KB frame, called once) into psort_f64, whose frame went from 35,888 to 60,496 bytes: stack-clash
 * protection probes every page of it on EVERY call, and a sort of 2 to 10 elements went from 17.5 to 85 ns (make callbench against
 * the committed engine). An inlining decision moves when any other callee grows or shrinks; -fstack-usage prints the frame. */
static __attribute__((noinline)) int nearsorted_parallel_f64(double *a, size_t n, int P, double *dst, scratch_t *lz, double inv_rate) {
    if (P < 2 || P > 256 || ns_depth > 0) return 0;
    int ns_gate_old = getenv("LOGOS_NS_GATE_OLD") != NULL;               /* the A/B: both gates as they were before 2026-09-20 */
    (void)ns_gate_old;
    if (getenv("LOGOS_NO_NEARSORTED")) return 0;                        /* the route's own A/B: what it is worth against the partition (make nsgatebench, NSG_ROUTE=1) */
    if (inv_rate < 0) {                                        /* gate: the dispatcher's 40-point strided inversion sample */
        size_t ss = n < 40 ? n : 40, st = n / ss; if (st < 1) st = 1; size_t inv = 0, comps = 0;
        for (size_t i = 0; i + st < n; i += st) { if (a[i] > a[i + st]) inv++; comps++; }
        if (comps == 0) return 0;
        inv_rate = (double)inv / comps;
    }
    /* The gate, measured again on the repaired statistic (2026-09-20), as the ROUTE against the partition (LOGOS_NO_NEARSORTED; make
       nsgatebench with NSG_ROUTE=1), 14 threads, sorted data with random swaps:
         0.5% and 1% swaps (the statistic reads 1-2.5%): the route wins at every size, 1.07-1.48x from 1M to 2M, 1.36-2.1x from 3M up;
         2% swaps (reads ~3.5-5%): 0.84-0.87 at 1.5M, 0.93-1.15 at 2M, 0.94-0.99 at 3-4M, and 1.33-1.41 at 6M, 1.12 at 8.4M;
         3% swaps (reads ~5-6.5%): 0.75 at 1.5M, 0.80 at 2M, a tie from 3M; 5% swaps (reads ~8%), gate lifted: 0.71 at 8.4M.
       The route has three spawn phases and sorts its strays once, which a few million elements do not pay back unless the strays are
       few. So: 2.5% below NS_WIDE_MIN_N elements, 5% from there on. The old pre-gate hid all of this: without poison recovery it
       turned away 38 of 40 inputs with 2% swaps whatever their size. */
    if (inv_rate > ((ns_gate_old || n >= NS_WIDE_MIN_N) ? NEARLY_SORTED_INV_RATIO : NS_INV_RATIO_SMALL)) return 0;

#ifndef LOGOS_NS_OLD
    /* pre-gate: the greedy scan on four 4096-element windows; if it would reject more than a quarter the
       full route would decline after a whole pass and a thread spawn (measured 0.83-0.89x on locally
       shuffled data, where every element sits within +-1000 of home and the partition route is the right one) */
    if (n >= 4 * 4096) {
        size_t rej = 0, tot = 0, wdesc = SIZE_MAX;                         /* wdesc: counted by the repaired pre-gate only; the old one leaves the strand gate closed */
        if (ns_gate_old) {
            for (int w = 0; w < 4; w++) {
                size_t lo = (n / 5) * (size_t)(w + 1) - 2048, hi = lo + 4096; double kmax = -INFINITY;
                for (size_t i = lo; i < hi; i++) { double v = a[i], n1 = a[i + 1], n2 = a[i + 2]; if ((v >= kmax) & ((v <= n1) | (v <= n2))) kmax = v; else rej++; tot++; }
            }
        } else {
            /* The route's OWN scan on the four windows, poison recovery included. The scan above has none: one displaced-high
               element followed within two places by a larger one becomes the kept maximum and every legitimate element after
               it in the window counts as rejected. Measured on sorted data with random swaps, 40 seeds each: this gate
               declined 12 of 40 inputs at 1% swaps and 38 of 40 at 2% (mean rejected 18% and 48%, where the route itself
               rejects 2% and 4%), so the route it guards was mostly unreachable on the data it was built for. */
            double win[4096]; ns_chunk_arg_t pw; memset(&pw, 0, sizeof pw); wdesc = 0;
            for (int w = 0; w < 4; w++) {
                size_t lo = (n / 5) * (size_t)(w + 1) - 2048;
                pw.a = a + lo; pw.dst = win; pw.lo = 0; pw.hi = 4096; pw.tlo = -INFINITY; pw.thi = INFINITY; pw.ctx = NULL;
                ns_split_scan(&pw); rej += pw.nstray; tot += 4096;
                for (size_t i = lo; i < lo + 4096; i++) wdesc += a[i + 1] < a[i];          /* the element's scale: adjacent descents (a[lo + 4096] exists: lo + 4096 <= 4n/5 + 2048 < n) */
            }
        }
        if (rej * 4 > tot) return 0;
#ifndef LOGOS_NO_NSSTRAND
        if (wdesc <= NS_STRAND_WIN_DESC && !getenv("LOGOS_NO_NSSTRAND")) {               /* the windows see one sorted strand: does the array hold several? */
            psamp_t *ps = sample_by_pos(a, n, NS_STRAND_M);                             /* NULL (no memory, a NaN): no verdict, the route goes on as before */
            if (ps) {
                size_t sdesc = 0; for (size_t i = 1; i < NS_STRAND_M; i++) sdesc += ps[i].v < ps[i - 1].v;
                free(ps);
                if (sdesc >= NS_STRAND_MIN_DESC) {
                    TRACE("ns: strand gate: %zu descents in the windows, %zu in the position-ordered sample -> strands, declined", wdesc, sdesc);
                    atomic_fetch_add(&g_ns_strand, 1); return 0;
                }
                TRACE("ns: strand gate: %zu descents in the windows, %zu in the sample -> one strand, the route goes on", wdesc, sdesc);
            }
        }
#else
        (void)wdesc;
#endif
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
        if (ok && !dst && !(dst = scratch_get(lz))) ok = 0;   /* the scratch, taken only now: nothing above needed it */
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
        par_run(th, P, ns_sort_chunk, ca, sizeof ca[0], 1);
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
            par_run(th, P, ns_merge_band, ma, sizeof ma[0], 1);
            sorted = 1;
        }
        if (!sorted) { memcpy(a, dst, n * sizeof(double)); sorted = -1; }
        free(rlo); free(rhi); free(slo); free(shi); free(boff); free(ma);
    }
#else
    if (ok) {
        /* 1. split + sort the in-band part of every chunk, in parallel; a[] is untouched */
        TRACE("ns: thresholds + contexts ready");
        par_run(th, P, ns_split_sort, ca, sizeof ca[0], 1);
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
                    if (S >= NS_STRAYS_PARALLEL && P >= 2) { NESTED_SORT(logos_sort_f64(strays, S, P)); }
                    else { ctx_t sc; if (ctx_init(&sc, S, 0)) { double *r = sort_fn(strays, S, sscr, &sc); if (r != strays) memcpy(strays, r, S * sizeof(double)); ctx_free(&sc); }
                           else sample_sort(strays, S); }
                }
                sb[0] = 0; for (int b = 1; b < P; b++) sb[b] = lower_bound_d(strays, 0, S, thr[b]); sb[P] = S;
                TRACE("ns: strays sorted and cut");
                /* 3. one two-way merge per band, straight into a[] */
                size_t off = 0;
                for (int t = 0; t < P; t++) { mg[t].run = dst + ca[t].lo; mg[t].nrun = ca[t].nin; mg[t].strays = strays; mg[t].s0 = sb[t]; mg[t].s1 = sb[t + 1]; mg[t].out = a + off; mg[t].tid = t; off += ca[t].nin + (sb[t + 1] - sb[t]); }
                if (off == n) {
                    par_run(th, P, ns_merge2, mg, sizeof mg[0], 1);
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
static int reloc_sample_gate(const double *a, size_t n, double kfrac, double kfew) {
    /* MAXR: what J5 judges from the sample. KMAXR: what the knot route judges (kfrac > 0). Sixteen runs was the limit
     * because a chain verdict needs long sample runs; a crossing COST is a sum and tolerates short ones. It matters
     * because the per-thread prefix gate below cannot see a shuffled staircase (the few blocks one thread holds
     * rarely neighbour each other in value), so a shape beyond the cost gate was scanned in full and declined
     * late: 3-4 live strands cost 0.91-0.95 that way, and 0.82 if taken. */
    enum { S = 1024, MAXR = 16, KMAXR = 128 };
    size_t maxr = kfrac > 0 ? KMAXR : MAXR;
    if (n < 4 * S) return 1;
    double s[S]; size_t step = n / S; for (size_t i = 0; i < S; i++) s[i] = a[i * step];
    size_t runs[2 * (KMAXR + 1)], R = 0, i = 0;
    while (i < S) {
        size_t j = i + 1;
        if (j < S) {
            if (s[j] >= s[i]) { while (j < S && s[j] >= s[j - 1] && !reloc_gap_split_f(s, i, j - 1, j, 4.0, 4)) j++; }
            else { while (j < S && s[j] < s[j - 1] && !(j - i >= 4 && (s[j - 1] - s[j]) * (double)(j - i) > 4.0 * (s[i] - s[j - 1]))) j++;
                   for (size_t l = i, r = j - 1; l < r; l++, r--) { double t = s[l]; s[l] = s[r]; s[r] = t; } }
        }
        if (R == maxr) return 1;
        runs[2 * R] = i; runs[2 * R + 1] = j - 1; R++; i = j;
    }
    if (R < 2) return 1;
    rdesc_t *d; size_t *ch, nc, merged;
    if (!runs_plan(s, runs, R, &d, &ch, &nc, &merged)) return 1;
    int go = merged <= S / 2;
#ifndef LOGOS_NO_KNOT
    /* Most of the sample sits in chains: tangled for J5, but how much of it actually CROSSES? Up to MAXR runs the
     * sample's verdict is final, as J5's is. Beyond that the runs are short (8-60 points), so the sample only DECLINES,
     * and only when its estimate clears the gate by a tenth; anything nearer goes to the full scan. */
    if (R > MAXR) go = 1;
    if (kfrac > 0 && merged > S / 2) {                        /* chains already say go: nothing to ask */
        knot_plan_t K;
        if (knot_plan(s, d, R, (size_t)S + 1, 0, &K)) {
            double kf = R > MERGE_MAX_RUNS ? kfrac : kfew;
            if (R <= MAXR) go = (double)K.cost <= kf * (double)S;
            else if ((double)K.cost > 1.1 * kf * (double)S) go = 0;
            TRACE("knot: sample gate R=%zu crossing=%zu cost=%zu kmax=%zu of %d (gate %.2f) -> %s", R, K.crossing, K.cost, K.kmax, S, kf, go ? "go" : "decline");
        }
    }
#else
    (void)kfrac; (void)kfew; (void)maxr;
#endif
    free(d); free(ch);
    TRACE("reloc: sample gate R=%zu merged=%zu/%d", R, merged, S);
    return go;
}
/* TIES IN A DESCENDING RUN (2026-09-22, tsbench's edge round). An ascending run has always taken equal neighbours; a descending one
 * ended at the first tie, as a stable sort's must (Timsort reverses strictly descending runs only). Nothing here is stable and
 * nothing needs to be: equal doubles read backwards are still in order, and the twist reads a descending strand from its far end.
 * Timestamps with ~1.25% ties (two events in one quarter microsecond) cut every descending block into ~80-element pieces, under the
 * detector's floor, and 64 shuffled blocks with every other one descending paid for a full sort: 2.4x behind a trimmed merge of the
 * same blocks at one thread. A run still STARTS descending only on a strict fall (a tie at a run's head goes the ascending way, as
 * before). LOGOS_DESC_STRICT=1 restores the strict test (read with the other per-sort switches, from MIN_PARALLEL up): the A/B. */
static __attribute__((unused)) int g_desc_ties = 1;
typedef struct { double *a; size_t lo, hi, cap, R; size_t *runs; atomic_int *abort; int tid; double kfrac; int twist, fast, kink, ties, simd; size_t kinks;
                 size_t *stray, nstray, stray_cap, sdiv; int last; } rdet_arg_t;   /* last: this chunk ends at the array's end */
/* the stray list is taken at the FIRST stray: allocating it on every sort cost clean block shapes 4-6% (1 MB, taken and given
   back through the heap top each call, measured 0.94-0.96 on block_perm and stairs x20 in two runs). 0 = no room: strays end runs. */
static int stray_room(rdet_arg_t *w, size_t k) {
    if (!w->stray_cap || w->nstray + k > w->stray_cap) return 0;
    if (!w->stray) { w->stray = malloc(w->stray_cap * sizeof(size_t)); if (!w->stray) { w->stray_cap = 0; return 0; } }
    return 1;
}   /* stray: positions lifted out of ascending runs (NULL: strays end runs, as before) */   /* kfrac > 0: the knot route's cost gate; twist: leave descending runs as they lie; fast: scan in blocks of eight */
/* THE BLOCK SCAN, VECTORIZED (2026-09-22, found by the handoff pipeline's kinks row: its plain scan beat the engine 1.20-1.27x on one
 * strand with 1% adjacent swaps, where the trace puts this scan at 89% of the sort; the loop below compiled to thirteen scalar
 * subsd / maxsd / comisd, the prescan's defect one level down). The eight steps are computed two at a time (SSE2, baseline x86-64) or
 * four at a time (AVX2 where the CPU has AVX2 and FMA), and the helper advances over AS MANY blocks as pass, so the AVX2 form is one
 * call per run and not one per block. The tests are the scalar ones exactly: `!(d >= 0)` ascending and `!(d > 0)` descending (`!(d >= 0)`
 * where a tie continues the run), so a NaN or inf - inf fails the block and lands on the scalar line as before; and max keeps the
 * accumulator when a step is NaN, as `d > dmax ? d : dmax` does. The run list is therefore identical to the scalar line's by
 * construction, and the harness checks it against both the scalar scan and this scan's own scalar form.
 * LOGOS_SCANBLOCK_SCALAR=1 (read with the other per-sort switches, from MIN_PARALLEL up): the A/B. */
enum { RELOC_BLK_BREAK = 0, RELOC_BLK_BOUND = 1, RELOC_BLK_SHORT = 2 };   /* why the advance stopped */
#define RELOC_BLOCKS_FN(NAME, STEP, ACCEPT, RANGE)                                                                        \
static size_t NAME(const double *a, size_t i, size_t j, size_t n, int ties, int *pstate) {                         \
    (void)ties;                                                                                                    \
    for (;;) {                                                                                                     \
        if (j + 8 > n) { *pstate = RELOC_BLK_SHORT; return j; }                                                     \
        double dmax = 0.0; int ok = 1;                                                                              \
        for (int k = 0; k < 8; k++) { double d = STEP; ok &= ACCEPT; dmax = d > dmax ? d : dmax; }                  \
        int fits = !(dmax * (double)(j - i + 8) > RELOC_GAP_FACTOR * (RANGE));                                    \
        if (ok && fits) { j += 8; continue; }                                                                      \
        *pstate = !ok && fits ? RELOC_BLK_BREAK : RELOC_BLK_BOUND;                                                  \
        return j;                                                                                                   \
    }                                                                                                               \
}
RELOC_BLOCKS_FN(reloc_blocks_up_c, a[j + k] - a[j + k - 1], d >= 0.0, a[j - 1] - a[i])
RELOC_BLOCKS_FN(reloc_blocks_down_c, a[j + k - 1] - a[j + k], (d > 0.0) | (ties & (d == 0.0)), a[i] - a[j - 1])
#ifdef LOGOS_HAVE_SSE2
#define RELOC_BLOCKS_SIMD(NAME, VEC, LOAD, SUB, MAXV, OR, CMP_BAD, MOVEMASK, ZERO, WIDE, HMAX, CUR, PREV, RANGE)          \
static size_t NAME(const double *a, size_t i, size_t j, size_t n, int ties, int *pstate) {                         \
    (void)ties;                                                                                                    \
    for (;;) {                                                                                                     \
        if (j + 8 > n) { *pstate = RELOC_BLK_SHORT; return j; }                                                     \
        VEC acc = ZERO, bad = ZERO; const double *p = a + j;                                                        \
        for (int k = 0; k < 8; k += WIDE) { VEC d = SUB(LOAD(CUR), LOAD(PREV));                                      \
            bad = OR(bad, CMP_BAD); acc = MAXV(d, acc); }                                                            \
        int ok = MOVEMASK(bad) == 0; double dmax = HMAX(acc);                                                        \
        int fits = !(dmax * (double)(j - i + 8) > RELOC_GAP_FACTOR * (RANGE));                                       \
        if (ok && fits) { j += 8; continue; }                                                                        \
        *pstate = !ok && fits ? RELOC_BLK_BREAK : RELOC_BLK_BOUND;                                                   \
        return j;                                                                                                    \
    }                                                                                                                \
}
static inline double hmax2(__m128d v) { __m128d h = _mm_unpackhi_pd(v, v); return _mm_cvtsd_f64(_mm_max_pd(v, h)); }
RELOC_BLOCKS_SIMD(reloc_blocks_up_sse2, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, _mm_cmpnge_pd(d, _mm_setzero_pd()),
                  _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, p + k, p + k - 1, a[j - 1] - a[i])
RELOC_BLOCKS_SIMD(reloc_blocks_down_sse2_strict, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, _mm_cmpngt_pd(d, _mm_setzero_pd()),
                  _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, p + k - 1, p + k, a[i] - a[j - 1])
RELOC_BLOCKS_SIMD(reloc_blocks_down_sse2_ties, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, _mm_cmpnge_pd(d, _mm_setzero_pd()),
                  _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, p + k - 1, p + k, a[i] - a[j - 1])
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#include <immintrin.h>
static inline double hmax4(__m256d v) { __m128d lo = _mm256_castpd256_pd128(v), hi = _mm256_extractf128_pd(v, 1); lo = _mm_max_pd(lo, hi);
                                        __m128d h = _mm_unpackhi_pd(lo, lo); return _mm_cvtsd_f64(_mm_max_pd(lo, h)); }
RELOC_BLOCKS_SIMD(reloc_blocks_up_avx2, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                  _mm256_cmp_pd(d, _mm256_setzero_pd(), _CMP_NGE_UQ), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4, p + k, p + k - 1, a[j - 1] - a[i])
RELOC_BLOCKS_SIMD(reloc_blocks_down_avx2_strict, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                  _mm256_cmp_pd(d, _mm256_setzero_pd(), _CMP_NGT_UQ), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4, p + k - 1, p + k, a[i] - a[j - 1])
RELOC_BLOCKS_SIMD(reloc_blocks_down_avx2_ties, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                  _mm256_cmp_pd(d, _mm256_setzero_pd(), _CMP_NGE_UQ), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4, p + k - 1, p + k, a[i] - a[j - 1])
#pragma GCC pop_options
#endif
static int g_scan_scalar = 0;            /* LOGOS_SCANBLOCK_SCALAR=1: the scalar block test, read per sort from MIN_PARALLEL up */
static inline size_t reloc_blocks_up(const double *a, size_t i, size_t j, size_t n, int simd, int *pstate) {
#ifdef LOGOS_HAVE_SSE2
    if (simd == 2) return reloc_blocks_up_avx2(a, i, j, n, 0, pstate);
    if (simd == 1) return reloc_blocks_up_sse2(a, i, j, n, 0, pstate);
#else
    (void)simd;
#endif
    return reloc_blocks_up_c(a, i, j, n, 0, pstate);
}
static inline size_t reloc_blocks_down(const double *a, size_t i, size_t j, size_t n, int simd, int ties, int *pstate) {
#ifdef LOGOS_HAVE_SSE2
    if (simd == 2) return ties ? reloc_blocks_down_avx2_ties(a, i, j, n, ties, pstate) : reloc_blocks_down_avx2_strict(a, i, j, n, ties, pstate);
    if (simd == 1) return ties ? reloc_blocks_down_sse2_ties(a, i, j, n, ties, pstate) : reloc_blocks_down_sse2_strict(a, i, j, n, ties, pstate);
#else
    (void)simd;
#endif
    return reloc_blocks_down_c(a, i, j, n, ties, pstate);
}
static void *reloc_detect_worker_body(void *p);
static void *reloc_detect_worker(void *p) { PROF(prof_stamp_t s0 = prof_stamp();) void *r = reloc_detect_worker_body(p); PROF(prof_busy_since(((rdet_arg_t *)p)->tid, LOGOS_PH_RELOC_PROBE, s0);) return r; }
static void *reloc_detect_worker_body(void *p) {
    rdet_arg_t *w = p; double *a = w->a; size_t n = w->hi, i = w->lo, next_check = 2; const int ties = w->ties;
    while (i < n) {
        /* THE SWING AT A RUN'S HEAD (stray mode): a run that starts with a stray below it, [stray, x, x+1, ...], has no body for the
           gap test to read, and its interval reached down to the stray (blocks with 1% strays read to the end and then declined on
           crossing cost). If the first step dwarfs the next two, the head is a stray and the run starts after it. */
        if (w->stray_cap && i + 4 < n) {
            double r1 = a[i + 3] - a[i + 1], r2 = a[i + 4] - a[i + 2];
            if (a[i + 1] <= a[i + 2] && a[i + 2] <= a[i + 3] && r1 > 0 && fabs(a[i + 1] - a[i]) * 2.0 > RELOC_GAP_FACTOR * r1 && stray_room(w, 1)) { w->stray[w->nstray++] = i; i++; continue; }
            if (a[i + 2] <= a[i + 3] && a[i + 3] <= a[i + 4] && r2 > 0 && fabs(a[i + 2] - a[i + 1]) * 2.0 > RELOC_GAP_FACTOR * r2 && fabs(a[i + 2] - a[i]) * 2.0 > RELOC_GAP_FACTOR * r2 && stray_room(w, 2)) {
                w->stray[w->nstray++] = i; w->stray[w->nstray++] = i + 1; i += 2; continue; }   /* two strays at the head */
        }
        size_t j = i + 1, tw = 0;
        if (j < n) {
            /* THE SCAN IN BLOCKS OF EIGHT (2026-09-21, make twistbench mode `scan`). The profile put this loop at 47-49% of the run
             * routes at one thread (1.1 ns per element for a read): one dependent compare, one multiply and one call per element.
             * Since the twist it writes nothing, so eight elements can be taken at once whenever a bound PROVES the scalar test
             * accepts each of them: all eight steps go the run's way, and the LARGEST step times the LARGEST count the block can
             * reach stays under the gap limit for the SMALLEST range it can have (the range only grows inside a run). Rounding is
             * monotone in every operand, so the bound holds in floating point as it does on paper; a NaN or inf - inf fails the
             * sign test and lands on the scalar line, which is unchanged. The run list is therefore identical by construction.
             * After a block fails, eight scalar steps pass before the next attempt, so data that hugs the limit cannot thrash.
             * THE BREAK (2026-09-22, found by the triple-reversal weld, tsbench mode `weld`: on one strand with 1% adjacent swaps the
             * weld's plain scan beat the engine, and the trace put this scan at 89% of the sort). A block that fails only because one
             * step goes the WRONG WAY still proves the steps before that step: a prefix of the block has a largest step no larger, a
             * count no larger and a range no smaller. So the scan jumps to the break, the scalar line takes the break and one step more,
             * and a block is tried again; a block that fails the gap BOUND keeps the eight. The run list is identical by construction,
             * as before, and a fast advance now clears the one-step `nolook` a lifted stray sets, which a block of eight could otherwise
             * carry eight elements past the step it was meant for. LOGOS_FASTSCAN_HOLD8=1 = the old hold (fast == 1): the A/B. */
            if (a[j] >= a[i]) {
                size_t hold = 0; int nolook = 0;   /* nolook: a[j-2] is a lifted stray this step, so no test may read it */
                while (j < n) {
                    if (w->fast && !hold && j - i >= RELOC_MIN_BODY && j + 8 <= n) {
                        int st; size_t j2 = reloc_blocks_up(a, i, j, n, w->simd, &st);
                        if (j2 != j) { j = j2; nolook = 0; }
                        if (st == RELOC_BLK_BREAK && w->fast == 2) {       /* the break: the steps before it are proven, jump to it */
                            size_t kf = 0; while (a[j + kf] - a[j + kf - 1] >= 0.0) kf++;   /* the block's own test: inf - inf stops it here too */
                            if (kf) { j += kf; nolook = 0; }
                            hold = 2;
                        } else if (st != RELOC_BLK_SHORT) hold = 8;
                        if (j >= n) break;        /* the advance ended exactly at the chunk's end: the scalar line must not read a[n] */
                    }
                    int look2 = !nolook; nolook = 0;
                    if (a[j] >= a[j - 1] && !reloc_gap_split(a, i, j - 1, j)) { j++; if (hold) hold--; }
                    /* A KINK (Reidemeister move I; ceiling and A/B: make strandprobe): one adjacent pair swapped inside a run that
                     * otherwise goes on, [.. 4 6 5 7 ..]. It used to end the run; a few thousand of them pushed the average run
                     * under the detector's floor and an array of sorted blocks paid for a full sort (4.0-5.6x at one thread).
                     * The misplaced element sits next to its hole, so the pair is swapped back where it stands and the strand
                     * stays contiguous: every search and offset of the plan still holds. Both neighbours must agree (the element
                     * before the pair and the one after it), so noise is not bubbled and a turn into a descending run is not
                     * mistaken for one. Never across a chunk's end: the next chunk's thread may be writing there. */
                    else if (look2 && w->kink && j + 1 < n && j >= i + 2 && a[j] < a[j - 1] && a[j] >= a[j - 2] && a[j - 1] <= a[j + 1]) {
                        double x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; w->kinks++; }
                    /* A STRAY (see stray_compact), in three forms. Its position is recorded, the run reads on past it, and the hole is closed
                       before the plan is made. Never the last element of a chunk. The first cut lifted only the first form: a stray ABOVE
                       the run was accepted into it (a gap split ended the run before it, or a[j+1] ended it after it), so the runs stayed
                       ~100 long, their intervals spanned the range, and the chain gate stopped the scan at eight runs. */
                    else if (w->stray_cap && j + 1 < n && j >= i + 4 && a[j] < a[j - 1] && a[j + 1] >= a[j - 1] && !reloc_gap_split(a, i, j - 1, j + 1) && stray_room(w, 1)) {
                        w->stray[w->nstray++] = j; j += 2; nolook = 1; if (hold) hold = hold > 2 ? hold - 2 : 0; }    /* below the run: a[j] breaks it, a[j+1] resumes it */
                    else if (w->stray_cap && j + 1 < n && j >= i + 4 && a[j] >= a[j - 1] && a[j + 1] >= a[j - 1] && !reloc_gap_split(a, i, j - 1, j + 1) && stray_room(w, 1)) {
                        w->stray[w->nstray++] = j; j += 2; nolook = 1; if (hold) hold = hold > 2 ? hold - 2 : 0; }    /* a gap up: a[j] far above, a[j+1] resumes */
                    else if (look2 && w->stray_cap && j + 1 < n && j >= i + 4 && a[j] < a[j - 1] && a[j] >= a[j - 2] && a[j + 1] >= a[j] && !reloc_gap_split(a, i, j - 2, j) && stray_room(w, 1)) {
                        w->stray[w->nstray++] = j - 1; j += 1; nolook = 1; if (hold) hold--; }                         /* above the run: a[j-1] was the stray, a[j] continues from a[j-2] and the run goes on past it */
                    else if (w->stray_cap && j + 2 < n && j >= i + 4 && a[j] == a[j] && a[j + 1] == a[j + 1] && a[j + 2] >= a[j - 1] && !reloc_gap_split(a, i, j - 1, j + 2) && stray_room(w, 2)) {
                        w->stray[w->nstray++] = j; w->stray[w->nstray++] = j + 1; j += 3; nolook = 1; if (hold) hold = hold > 3 ? hold - 3 : 0; }   /* two strays side by side, a[j+2] resumes */
                    else break;
                }
            }
            else { size_t hold = 0;
                   while (j < n) {
                       if (w->fast && !hold && j - i >= RELOC_MIN_BODY && j + 8 <= n) {
                           int st; size_t j2 = reloc_blocks_down(a, i, j, n, w->simd, ties, &st);
                           j = j2;
                           if (st == RELOC_BLK_BREAK && w->fast == 2) {    /* the break, as in an ascending run */
                               size_t kf = 0; for (;;) { double d = a[j + kf - 1] - a[j + kf]; if (!((d > 0.0) | (ties & (d == 0.0)))) break; kf++; }
                               j += kf; hold = 2;
                           } else if (st != RELOC_BLK_SHORT) hold = 8;
                           if (j >= n) break;     /* as above: the loop condition the old `continue` re-checked */
                       }
                       if ((a[j] < a[j - 1] || (ties && a[j] == a[j - 1])) && !(j - i >= RELOC_MIN_BODY && (a[j - 1] - a[j]) * (double)(j - i) > RELOC_GAP_FACTOR * (a[i] - a[j - 1]))) { j++; if (hold) hold--; }
                       else if (w->kink && j + 1 < n && j >= i + 2 && a[j] > a[j - 1] && a[j] < a[j - 2] && a[j - 1] > a[j + 1]) {   /* the same kink in a run that lies descending */
                           double x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; w->kinks++; }
                       else break;
                   }
                   if (w->twist) { if (j - i >= 2) tw = RUN_TWIST; }          /* the twist: the run stays as it lies (a NaN's run of one has no direction) */
                   else for (size_t l = i, r = j - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } }
        }
        /* A RUN OF ONE OR TWO, in stray mode, is strays: a run that STARTS with a stray is two elements whose interval spans the
           range, and the chain gate below read it as one tangle and stopped the scan (blocks with 1% strays bailed at R = 4). */
        /* ... unless it touches a chunk boundary inside the array: the join's boundary repairs ask for that run (a kink whose second
           element is the chunk's last leaves a run of one; a kink that begins a chunk is a two-element run lying the other way; lifted
           here, each left R = 2 and strays where the join would have stitched one strand). */
        if (w->stray_cap && j - i <= 2 && !(j == n && !w->last) && !(i == w->lo && w->lo > 0) && a[i] == a[i] && a[j - 1] == a[j - 1] && stray_room(w, j - i)) {   /* never a NaN: it keeps the route it had */
            for (size_t q = i; q < j; q++) w->stray[w->nstray++] = q;
            i = j; continue; }
        w->runs[2 * w->R] = i; w->runs[2 * w->R + 1] = (j - 1) | tw; w->R++;
        size_t seen = j - w->lo;
        /* strays arriving faster than the cap allows: the scan would read on until the cap and decline there (3% strays paid 7%
           for a scan it threw away). Past a thirty-second of the array the density is known well enough to stop now. */
        if (w->stray_cap && seen > (w->hi - w->lo) / 32 && w->nstray * w->sdiv > seen) { atomic_store(w->abort, 1); return NULL; }
        if (w->R >= w->cap || (w->R >= 256 && seen < w->R * g_reloc_avg) || atomic_load(w->abort)) { atomic_store(w->abort, 1); return NULL; }
        if (w->R == next_check) {
            rdesc_t *d; size_t *ch, nc, merged;
            if (runs_plan(a, w->runs, w->R, &d, &ch, &nc, &merged)) {
                TRACE("gate [%zu..%zu) R=%zu seen=%zu chains=%zu merged=%zu", w->lo, w->hi, w->R, seen, nc, merged);
                /* Not before RELOC_CHECK_MIN_SEEN elements (2026-10-21, fieldprobe founder): a near-sorted array whose first 64 places
                   held strays from anywhere stopped HERE at R = 2 after 7 elements, and 4M elements took the leaf at 22.9 ns per element
                   where the stray list finishes them at 4.3. Random data never needed the early stop: the average-run rule above ends
                   it by R = 256, a few hundred elements in. LOGOS_RELOC_CHECK_EARLY=1: stop at any checkpoint, as before. */
                int stop = merged > seen / 2 && (seen >= RELOC_CHECK_MIN_SEEN || getenv("LOGOS_RELOC_CHECK_EARLY"));
#ifndef LOGOS_NO_KNOT
                /* chains cover most of this prefix. J5 stops here; the knot route stops only if what CROSSES is too much */
                if (stop && w->kfrac > 0 && seen >= (size_t)KNOT_MIN_AVG_RUN * w->R) { knot_plan_t K; if (knot_plan(a, d, w->R, seen + 1, 0, &K) && (double)K.cost <= w->kfrac * (double)seen) stop = 0; }
#endif
                free(d); free(ch);
                if (stop) { atomic_store(w->abort, 1); return NULL; }
            }
            next_check <<= 1;
        }
        i = j;
    }
    return NULL;
}
/* Flat [start,end,...] list of the whole array's runs (malloc'd), or NULL when a
 * thread's gate said "not block-permuted". */
static size_t g_kink_count = 0;         /* logos_twist_last(4): kinks the detector swapped back, summed over the scans since it was last read */
static size_t *detect_runs_parallel(double *a, size_t n, int P, size_t *n_runs, double kfrac, int twist, int fast, int kink, size_t **pstray, size_t *pnstray) {
    size_t run_cap = n / g_reloc_avg; if (run_cap < MAX_RUNS_FOR_MERGE) run_cap = MAX_RUNS_FOR_MERGE;
    if (n < MIN_PARALLEL) P = 1;
    size_t chunk = (n + (size_t)P - 1) / (size_t)P, cap = run_cap / (size_t)P + 2;
    rdet_arg_t *w = calloc((size_t)P, sizeof *w); pthread_t *th = malloc((size_t)P * sizeof *th); atomic_int abort_flag; atomic_init(&abort_flag, 0);
    *n_runs = 0; if (!w || !th) { free(w); free(th); return NULL; }
    int ok = 1;
    for (int t = 0; t < P; t++) { w[t].a = a; PROF(w[t].tid = t;) w[t].lo = (size_t)t * chunk; w[t].hi = w[t].lo + chunk > n ? n : w[t].lo + chunk; w[t].cap = cap; w[t].abort = &abort_flag; w[t].kfrac = kfrac; w[t].twist = twist; w[t].fast = fast; w[t].kink = kink; w[t].ties = g_desc_ties; w[t].simd = g_scan_scalar ? 0 : logos_simd_kind(); w[t].kinks = 0; w[t].last = (t == P - 1);
                                  w[t].runs = malloc(2 * cap * sizeof(size_t)); if (!w[t].runs) ok = 0;
                                  w[t].stray = NULL; w[t].nstray = 0; w[t].stray_cap = 0; }
    if (pstray) { *pstray = NULL; *pnstray = 0; }
    /* every worker may lift strays (2026-09-26; it was w[0] at P == 1 only): each chunk gets its share of the n / 64 budget, and
       the join concatenates the lists. The list itself is taken at the first stray (stray_room); no memory: strays end runs, as before */
    /* the cap is a share of each chunk: 1/16 at one thread since 2026-10-21 (1% random swaps 3.0x, 2% 2.1x), 1/64 at P > 1, where 2% swaps
       read 0.33 at 14 threads with 1/16: the near-sorted route is what takes random swaps there */
    if (ok && pstray) { size_t sdiv = P > 1 ? STRAY_DIV_PAR : g_stray_div; for (int t = 0; t < P; t++) { w[t].sdiv = sdiv; w[t].stray_cap = (w[t].hi - w[t].lo) / sdiv + 1; } }
    if (ok) {
        /* chunk 0 first, on the caller's thread: interleaved data fails its gate within a few runs,
           so the common decline never pays for spawning P-1 threads (milliseconds on a loaded box) */
        reloc_detect_worker(&w[0]);
        if (!atomic_load(&abort_flag)) {
            par_run(th, P, reloc_detect_worker, w, sizeof w[0], 0);
        }
    }
    size_t *runs = NULL, R = 0, bkinks = 0;                              /* bkinks: kinks repaired ON chunk boundaries, at the join */
    if (ok && !atomic_load(&abort_flag)) {
        size_t total = 0; for (int t = 0; t < P; t++) total += w[t].R;
        runs = malloc(2 * (total + 1) * sizeof(size_t));
        if (runs) for (int t = 0; t < P; t++) for (size_t k = 0; k < w[t].R; k++) {
            size_t s = w[t].runs[2 * k], ev = w[t].runs[2 * k + 1]; int rv = RUN_REV(ev), stitched = 0;
            /* A KINK THAT BEGINS A CHUNK (2026-09-26, the RM_SPARSEENDS row at n = 300001, P = 3: a pair at 100001 with the
               chunk edge at 100001): the worker cannot test its first pair, so it emits the swapped pair as a TWO-ELEMENT run
               lying the other way, and the stitch below wants the same way. If that pair, swapped back, fits between what
               precedes and what follows, it is a kink: swap it, flip the run, and let the stitch have it. Both orientations. */
            if (kink && R > 0 && RUN_END(runs[2 * R - 1]) + 1 == s && RUN_END(ev) == s + 1 && rv != RUN_REV(runs[2 * R - 1]) && s >= 1 && s + 2 < n) {
                int kk = !rv ? (a[s] < a[s + 1] && a[s] >= a[s + 2] && a[s + 1] <= a[s - 1])          /* an ascending pair inside a descending strand */
                             : (a[s] > a[s + 1] && a[s] <= a[s + 2] && a[s + 1] >= a[s - 1]);         /* a descending pair inside an ascending strand */
                if (kk) { double x = a[s]; a[s] = a[s + 1]; a[s + 1] = x; bkinks++; rv = !rv; ev = rv ? (ev | RUN_TWIST) : RUN_END(ev); }
            }
            /* stitch across the chunk boundary: two pieces of one run, lying the same way. A descending run that straddled a
               boundary used to become two disjoint runs, because each chunk reversed its own part; untouched, it is one strand. */
            if (R > 0 && RUN_END(runs[2 * R - 1]) + 1 == s && RUN_REV(runs[2 * R - 1]) == rv) {
                size_t p0 = runs[2 * R - 2];
                /* A KINK ON THE BOUNDARY (2026-09-26, make kinkladder): the worker's repair asks the element two back, which
                   is the other chunk, so a swapped pair straddling a boundary was left and split one strand in two -- and
                   with up to P - 1 of them, R was never 1 at P > 1 and the one-strand finish never fired there (0.94 at 14
                   threads against near-sorted, where one thread finishes the same strand 1.3x faster). This is the one
                   place that sees both sides. The detector's own test, both orientations; at most P - 1 positions, serial. */
                if (kink && s >= 2 && s + 1 < n) {
                    int kk = !rv ? (a[s] < a[s - 1] && a[s] >= a[s - 2] && a[s - 1] <= a[s + 1])
                                 : (a[s] > a[s - 1] && a[s] < a[s - 2] && a[s - 1] > a[s + 1]);   /* strict, as the worker's descending arm is */
                    if (kk) { double x = a[s]; a[s] = a[s - 1]; a[s - 1] = x; bkinks++; }
                }
                if (!rv) stitched = a[s] >= a[s - 1] && !reloc_gap_split(a, p0, s - 1, s);
                else stitched = (a[s] < a[s - 1] || (g_desc_ties && a[s] == a[s - 1])) && !(s - p0 >= RELOC_MIN_BODY && (a[s - 1] - a[s]) * (double)(s - p0) > RELOC_GAP_FACTOR * (a[p0] - a[s - 1]));
            }
            if (stitched) runs[2 * R - 1] = ev;
            else { runs[2 * R] = s; runs[2 * R + 1] = ev; R++; }
        }
    }
    /* A KINK AT EITHER END OF THE ARRAY (2026-09-26): the worker's ascending test reads a[j - 2] and a[j + 1], so the pair
       (0, 1) has no left neighbour to ask and the pair (n - 2, n - 1) no right one, and neither is ever repaired. At the
       head that reads as a two-element DESCENDING run before an ascending one; at the tail as a one-element run after it.
       At P = 1 the stray list lifts them; at P > 1 there is no list, R came back 2 and the one-strand finish never fired
       (the trace: run 0 = [0 .. 1] descending, run 1 = [2 .. n)). Ask only the neighbour that exists. */
    if (runs && kink && R >= 2 && n >= 4) {
        if (RUN_REV(runs[1]) && runs[0] == 0 && RUN_END(runs[1]) == 1 && !RUN_REV(runs[3]) && runs[2] == 2 && a[0] > a[1] && a[0] <= a[2]) {
            double x = a[0]; a[0] = a[1]; a[1] = x; bkinks++;
            runs[2] = 0; memmove(runs, runs + 2, 2 * (R - 1) * sizeof(size_t)); R--;                        /* run 1 now starts at 0 */
        }
        if (R >= 2) { size_t L = runs[2 * R - 2], E = RUN_END(runs[2 * R - 1]), pe = RUN_END(runs[2 * R - 3]);
            if (!RUN_REV(runs[2 * R - 3]) && L == n - 1 && E == n - 1 && pe == n - 2 && a[n - 1] < a[n - 2] && a[n - 1] >= a[n - 3]) {
                double x = a[n - 1]; a[n - 1] = a[n - 2]; a[n - 2] = x; bkinks++;
                runs[2 * R - 3] = n - 1; R--;                          /* the ascending run now ends at n - 1: no twist bit */
            } }
    }
    if (runs && pstray) {
        size_t S = 0; for (int t = 0; t < P; t++) S += w[t].nstray;
        if (S && P == 1) { *pstray = w[0].stray; *pnstray = S; w[0].stray = NULL; }
        else if (S) {                  /* P lists, each in position order and every chunk's below the next's: the concatenation IS the sorted list */
            size_t *sp = malloc(S * sizeof *sp);
            if (sp) { size_t q = 0; for (int t = 0; t < P; t++) { if (w[t].nstray) memcpy(sp + q, w[t].stray, w[t].nstray * sizeof *sp); q += w[t].nstray; } *pstray = sp; *pnstray = S; }
            else { free(runs); runs = NULL; R = 0; }   /* no room for the list: the runs read past holes nobody will close, so decline (nothing but kinks moved) */
        }
    }
    for (int t = 0; t < P; t++) { g_kink_count += w[t].kinks; free(w[t].runs); free(w[t].stray); }
    g_kink_count += bkinks;
    free(w); free(th); *n_runs = R; return runs;
}

/* PLACEMENTS STREAM (2026-10-19, roundbench). Relocation and the knot route copy every run piece into a scratch buffer and then
 * copy the buffer back: two passes that WRITE every byte of a destination nothing has read since it was last written. A plain
 * store to a line that is not resident fetches it first, so each such pass moved its bytes three times (read source, fetch
 * destination, write). The scatter's streaming stores measured zero (its floor is per-core latency, the ledger), but the
 * counting routes' write-only emit measured 1.21-1.36x, and the ledger's own advice is to ask whether a pass reads its
 * destination before ruling a store type out: these do not. From PLACE_NT_MIN_N elements (the array past the last-level cache)
 * and on pieces of PLACE_NT_MIN_K doubles or more: AVX2 streaming stores after a scalar prologue to a 32-byte boundary, and an
 * sfence before every barrier that hands the bytes to another thread. LOGOS_PLACE_PLAIN=1: memcpy, for the A/B. */
#ifndef PLACE_NT_MIN_N
#define PLACE_NT_MIN_N (1u << 22)          /* 32 MB of doubles, past the 24 MB last-level cache: at 2.1M it measured 1.016 */
#endif
#ifndef PLACE_NT_MIN_K
#define PLACE_NT_MIN_K 512
#endif
#if defined(__x86_64__) && defined(__GNUC__) && !defined(LOGOS_NO_PLACE_NT)
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
static void place_stream(double *dst, const double *src, size_t k) {
    size_t i = 0;
    while (i < k && (((uintptr_t)(dst + i)) & 31u)) { dst[i] = src[i]; i++; }
    for (; i + 16 <= k; i += 16) {
        _mm256_stream_pd(dst + i,      _mm256_loadu_pd(src + i));
        _mm256_stream_pd(dst + i + 4,  _mm256_loadu_pd(src + i + 4));
        _mm256_stream_pd(dst + i + 8,  _mm256_loadu_pd(src + i + 8));
        _mm256_stream_pd(dst + i + 12, _mm256_loadu_pd(src + i + 12));
    }
    for (; i + 4 <= k; i += 4) _mm256_stream_pd(dst + i, _mm256_loadu_pd(src + i));
    for (; i < k; i++) dst[i] = src[i];
}
static void place_fence(void) { _mm_sfence(); }
#pragma GCC pop_options
static int place_nt_ok(size_t n) { return n >= PLACE_NT_MIN_N && __builtin_cpu_supports("avx2") && !getenv("LOGOS_PLACE_PLAIN"); }
#else
static void place_stream(double *dst, const double *src, size_t k) { memcpy(dst, src, k * sizeof(double)); }
static void place_fence(void) { }
static int place_nt_ok(size_t n) { (void)n; return 0; }
#endif
static inline void place_copy(double *dst, const double *src, size_t k, int nt) {
    if (nt && k >= PLACE_NT_MIN_K) place_stream(dst, src, k); else memcpy(dst, src, k * sizeof(double));
}
/* for half_place (above the relocation code, so declared there) */
static __attribute__((unused)) void half_copy(double *dst, const double *src, size_t k, int nt) { place_copy(dst, src, k, nt); }
static __attribute__((unused)) void half_fence(int nt) { if (nt) place_fence(); }
static __attribute__((unused)) int half_nt_ok(size_t n) { return place_nt_ok(n); }
typedef struct { double *a, *dst; size_t n, R, nc; int P, t; const rdesc_t *d; const size_t *off, *ch, *rl; atomic_size_t *next; barrier_t *bar;
                 const unsigned char *orphan; int copyback; int nt; } reloc_arg_t;   /* orphan: the caller only. Tiles of workers whose thread could not be created; nt: placements stream (place_copy) */
#define TILE_IS_MINE(w, q) ((q) == (w)->t || ((w)->orphan && (w)->orphan[q]))
static void *reloc_worker(void *p) {
    reloc_arg_t *w = p; size_t n = w->n; int P = w->P;
    PROF(prof_stamp_t s0 = prof_stamp();)
    /* phase 1: fill dst[lo,hi) from the pieces of runs that land there (balanced, sequential streams) */
    for (int tq = 0; tq < P; tq++) { if (!TILE_IS_MINE(w, tq)) continue;
      size_t lo = (size_t)tq * n / P, hi = (size_t)(tq + 1) * n / P;
      size_t q = 0; while (q < w->R && w->off[q] + w->d[q].len <= lo) q++;
      for (; q < w->R && w->off[q] < hi; q++) {
          size_t ds = w->off[q] > lo ? w->off[q] : lo, de = w->off[q] + w->d[q].len < hi ? w->off[q] + w->d[q].len : hi;
          size_t o = ds - w->off[q], cnt = de - ds;                     /* the run's logical range [o, o+cnt): a twisted run holds it at its far end */
          if (!w->d[q].rev) place_copy(w->dst + ds, w->a + w->d[q].start + o, cnt, w->nt);
          else copy_rev(w->dst + ds, w->a + w->d[q].start + (w->d[q].len - o - cnt), cnt); } }
    if (w->nt) place_fence();                                          /* the streamed bytes before the barrier that hands them on */
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
    if (w->copyback) for (int tq = 0; tq < P; tq++) { if (!TILE_IS_MINE(w, tq)) continue;
      size_t lo = (size_t)tq * n / P, hi = (size_t)(tq + 1) * n / P; place_copy(w->a + lo, w->dst + lo, hi - lo, w->nt); }
    if (w->nt) place_fence();
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s4);)
    return NULL;
}

#ifndef LOGOS_NO_KNOT
/* ---- the knot route's parallel placement (plan and rationale: see knot_plan) ---- */
typedef struct { double *a, *dst; size_t n; int P, t; const knot_plan_t *K; const size_t *rl; atomic_size_t *nx2, *nx3; barrier_t *bar;
                 const unsigned char *orphan; int copyback; int nt; } knot_arg_t;    /* orphan: as in reloc_arg_t; copyback: 0 when the caller merges the output back itself; nt: as in reloc_arg_t */
static void *knot_worker(void *p) {
    knot_arg_t *w = p; const knot_plan_t *K = w->K; size_t n = w->n; int P = w->P;
    PROF(prof_stamp_t s0 = prof_stamp();)
    /* A1: this thread's tile of the output, from the copy pieces that land in it (balanced, sequential streams) */
    for (int tq = 0; tq < P; tq++) { if (!TILE_IS_MINE(w, tq)) continue;
      size_t lo = (size_t)tq * n / P, hi = (size_t)(tq + 1) * n / P, L = 0, H = K->npc;
      while (L < H) { size_t m = L + ((H - L) >> 1); if (K->pc[m].dst + K->pc[m].len <= lo) L = m + 1; else H = m; }
      for (size_t q = L; q < K->npc && K->pc[q].dst < hi; q++) {
          const kpiece_t *pc = &K->pc[q]; if (!pc->copy) continue;
          size_t ds = pc->dst > lo ? pc->dst : lo, de = pc->dst + pc->len < hi ? pc->dst + pc->len : hi;
          size_t o = ds - pc->dst, cnt = de - ds;
          if (!pc->rev) place_copy(w->dst + ds, w->a + pc->src + o, cnt, w->nt);
          else copy_rev(w->dst + ds, w->a + pc->src + (pc->len - o - cnt), cnt); } }
    /* A2: two-strand crossings, merged straight from a[] into place; a[] is only read until the barrier */
    size_t s, gl = 0;
    while ((s = atomic_fetch_add(w->nx2, 1)) < K->nsg) {
        const kseg_t *g = &K->sg[s]; if (g->k != 2) continue;
        const kpiece_t *x = &K->pc[g->p0], *y = x + 1;
        gl += knot_merge2x(w->a + x->src, x->len, x->rev, w->a + y->src, y->len, y->rev, w->dst + g->dst);
    }
    if (gl) atomic_fetch_add_explicit(&g_knot_gallops, gl, memory_order_relaxed);
    if (w->nt) place_fence();
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s0); double t1 = prof_now();)
    if (P > 1) barrier_wait(w->bar);
    PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t1); prof_stamp_t s2 = prof_stamp();)
    /* B: crossings of three or more strands, staged side by side in dst; a[] is free scratch now */
    if (K->n3) {
        while ((s = atomic_fetch_add(w->nx3, 1)) < K->nsg) { const kseg_t *g = &K->sg[s]; if (g->k >= 3 && !merge_runs_vec(w->dst, w->a, w->rl + 2 * g->p0, g->k)) merge_all_runs(w->dst, w->a, w->rl + 2 * g->p0, g->k); }
        PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s2); double t3 = prof_now();)
        if (P > 1) barrier_wait(w->bar);
        PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t3); s2 = prof_stamp();)
    }
    /* C: copy back */
    if (w->copyback) for (int tq = 0; tq < P; tq++) { if (!TILE_IS_MINE(w, tq)) continue;
      size_t lo = (size_t)tq * n / P, hi = (size_t)(tq + 1) * n / P; place_copy(w->a + lo, w->dst + lo, hi - lo, w->nt); }
    if (w->nt) place_fence();
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s2);)
    return NULL;
}
#endif

#ifndef LOGOS_NO_KNOT
/* ===================== the knot route, LITERALLY: the array as a linked list (opt-in, LOGOS_KNOT_LIST=1) =====================
 * The route was first described as "convert the array to a linked list, find the runs, untie the knots". What shipped keeps
 * the idea and not the structure: an array of run descriptors, pieces placed by copies and merges. This is the structure taken
 * at its word, built to be measured against that (./twistbench 60 8388608 list):
 *   1. every element gets a LINK, a 32-bit index: inside an ascending piece next[i] = i + 1, inside a TWISTED piece
 *      next[i] = i - 1. In a list the twist costs nothing at all: a descending run is a run whose links point the other way;
 *   2. a crossing is untied by RELINKING: the pieces' sublists are merged by rewriting links, no element moves;
 *   3. the units are stitched tail to head in output order. The array is now one sorted linked list;
 *   4. the list is walked out into dst (one thread: from the head, all n elements, through every stitch; P threads: each
 *      unit from its own head, since a list cannot be entered in the middle) and dst is copied back.
 * The walk is given its best known form: next is GUESSED (cur + 1, or cur - 1 inside a twisted run) and the load only checks
 * the guess, so inside a run the chain of dependent loads that makes a linked list slow is broken by the branch predictor.
 * LOGOS_LIST_PLAIN=1 walks by the pointer alone, to show what the guess is worth.
 * It reuses the knot plan for WHERE the strands cross; what it replaces is the placement. It costs 4 more bytes per element.
 * Returns 0 (nothing touched) when the links cannot be allocated or n does not fit 32 bits: the array placement runs instead. */
#define LIST_END 0xFFFFFFFFu
typedef struct { uint32_t head, tail; size_t len, dst, lo, hi, seg; int rev; } lunit_t;       /* seg == SIZE_MAX: part of a lone piece, a[lo, hi) */
static size_t g_list_count = 0;                         /* logos_twist_last(3): how many placements went through the list */
static inline void list_link_range(uint32_t *nx, size_t lo, size_t hi, int rev) {
    if (rev) for (size_t i = lo; i < hi; i++) nx[i] = (uint32_t)i - 1u; else for (size_t i = lo; i < hi; i++) nx[i] = (uint32_t)i + 1u;
}
/* merge two sublists by relinking. x is the earlier piece and wins ties, as in knot_merge2. Returns the head, sets *ptail. */
static uint32_t list_merge(const double *a, uint32_t *nx, uint32_t x, size_t cx, uint32_t xt, uint32_t y, size_t cy, uint32_t yt, uint32_t *ptail) {
    uint32_t head, tail;
    if (a[y] < a[x]) { head = y; y = nx[y]; cy--; } else { head = x; x = nx[x]; cx--; }
    tail = head;
    while (cx && cy) {
        if (a[y] < a[x]) { nx[tail] = y; tail = y; y = nx[y]; cy--; }
        else             { nx[tail] = x; tail = x; x = nx[x]; cx--; }
    }
    if (cx) { nx[tail] = x; *ptail = xt; } else if (cy) { nx[tail] = y; *ptail = yt; } else *ptail = tail;
    return head;
}
/* write cnt elements, in list order from cur, to o[]. The last element's link is never read (it may be a stitch in progress). */
static void list_walk(const double *a, const uint32_t *nx, uint32_t cur, size_t cnt, double *o, int plain) {
    if (!cnt) return;
    if (plain) { for (size_t k = 0; k + 1 < cnt; k++) { o[k] = a[cur]; cur = nx[cur]; } o[cnt - 1] = a[cur]; return; }
    uint32_t step = 1u;                                   /* +1 in an ascending run, -1 (as 0xFFFFFFFF) in a twisted one */
    for (size_t k = 0; k + 1 < cnt; k++) {
        o[k] = a[cur];
        uint32_t real = nx[cur], guess = cur + step, ne = real ^ guess;
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
        /* Without this the compiler notes that guess == real on the path where they compare equal, continues from `real`, and the
           chain of dependent loads is back (read in the assembly: the first cut measured exactly what the plain walk does).
           Laundering the difference keeps the equality to ourselves: on the common path cur comes from cur + step alone. */
        __asm__ volatile("" : "+r"(ne));
#endif
        if (__builtin_expect(ne != 0, 0)) {                /* the link leaves the run: follow it, and learn which way the next run lies */
            guess = real;
            if (k + 2 < cnt) step = nx[real] == real + 1u ? 1u : (uint32_t)0 - 1u;
        }
        cur = guess;
    }
    o[cnt - 1] = a[cur];
}
typedef struct { double *a, *dst; uint32_t *nx; size_t n; const knot_plan_t *K; lunit_t *u; size_t nu; atomic_size_t *q1, *q2, *q3; barrier_t *bar; int P, t, plain; } list_arg_t;
#define LIST_COPY_CHUNK ((size_t)1 << 18)
static void *list_worker(void *p) {
    list_arg_t *w = p; const knot_plan_t *K = w->K; uint32_t *nx = w->nx; size_t k;
    PROF(prof_stamp_t s0 = prof_stamp();)
    /* 1: links, and every crossing untied by relinking. Units are disjoint in a[] and in next[], so this needs no order. */
    while ((k = atomic_fetch_add(w->q1, 1)) < w->nu) {
        lunit_t *u = &w->u[k];
        if (u->seg == SIZE_MAX) { list_link_range(nx, u->lo, u->hi, u->rev); continue; }
        const kseg_t *g = &K->sg[u->seg]; uint32_t head = 0, tail = 0; size_t cnt = 0;
        for (size_t q = 0; q < g->k; q++) {
            const kpiece_t *pc = &K->pc[g->p0 + q];
            list_link_range(nx, pc->src, pc->src + pc->len, pc->rev);
            uint32_t h = (uint32_t)(pc->rev ? pc->src + pc->len - 1 : pc->src), tl = (uint32_t)(pc->rev ? pc->src : pc->src + pc->len - 1);
            if (!q) { head = h; tail = tl; cnt = pc->len; }
            else { head = list_merge(w->a, nx, head, cnt, tail, h, pc->len, tl, &tail); cnt += pc->len; }
        }
        u->head = head; u->tail = tail;
    }
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s0); double t1 = prof_now();)
    if (w->P > 1) barrier_wait(w->bar);
    PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t1); prof_stamp_t s2 = prof_stamp();)
    /* 2: the stitch (one thread; it writes unit TAILS only, which no walker reads), and the walk */
    if (w->t == 0) { for (size_t q = 0; q + 1 < w->nu; q++) nx[w->u[q].tail] = w->u[q + 1].head; nx[w->u[w->nu - 1].tail] = LIST_END; }
    if (w->P == 1) list_walk(w->a, nx, w->u[0].head, w->n, w->dst, w->plain);            /* the whole list, from its head */
    else while ((k = atomic_fetch_add(w->q2, 1)) < w->nu) list_walk(w->a, nx, w->u[k].head, w->u[k].len, w->dst + w->u[k].dst, w->plain);
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s2); double t3 = prof_now();)
    if (w->P > 1) barrier_wait(w->bar);
    PROF(prof_wait(w->t, LOGOS_PH_RELOCATE, prof_now() - t3); prof_stamp_t s4 = prof_stamp();)
    /* 3: copy back */
    size_t nch = (w->n + LIST_COPY_CHUNK - 1) / LIST_COPY_CHUNK;
    while ((k = atomic_fetch_add(w->q3, 1)) < nch) { size_t lo = k * LIST_COPY_CHUNK, hi = lo + LIST_COPY_CHUNK > w->n ? w->n : lo + LIST_COPY_CHUNK; memcpy(w->a + lo, w->dst + lo, (hi - lo) * sizeof(double)); }
    PROF(prof_busy_since(w->t, LOGOS_PH_RELOCATE, s4);)
    return NULL;
}
static int knot_place_list(double *a, double *dst, size_t n, const knot_plan_t *K, int P, int plain) {
    if (n >= (size_t)LIST_END - 1 || !K->npc) return 0;
    size_t grain = P > 1 ? n / ((size_t)P * 8) : n; if (grain < 65536) grain = 65536;
    size_t nu = K->nsg; for (size_t q = 0; q < K->npc; q++) if (K->pc[q].copy) nu += (K->pc[q].len + grain - 1) / grain;   /* an upper bound: staged k >= 3 pieces are counted too */
    uint32_t *nx = malloc(n * sizeof(uint32_t)); lunit_t *u = malloc((nu ? nu : 1) * sizeof *u);
    list_arg_t *w = malloc((size_t)P * sizeof *w); pthread_t *th = malloc((size_t)P * sizeof *th);
    if (!nx || !u || !w || !th) { free(nx); free(u); free(w); free(th); return 0; }
    size_t m = 0, s = 0;
    for (size_t q = 0; q < K->npc; ) {
        if (s < K->nsg && K->sg[s].p0 == q) { lunit_t x = { 0, 0, K->sg[s].len, K->sg[s].dst, 0, 0, s, 0 }; u[m++] = x; q += K->sg[s].k; s++; continue; }
        const kpiece_t *pc = &K->pc[q++];
        for (size_t c0 = 0; c0 < pc->len; c0 += grain) {                       /* a lone piece's address is known at any offset, so it can be entered there */
            size_t c1 = c0 + grain > pc->len ? pc->len : c0 + grain, lo = pc->rev ? pc->src + pc->len - c1 : pc->src + c0, hi = lo + (c1 - c0);
            lunit_t x = { (uint32_t)(pc->rev ? hi - 1 : lo), (uint32_t)(pc->rev ? lo : hi - 1), c1 - c0, pc->dst + c0, lo, hi, SIZE_MAX, pc->rev }; u[m++] = x;
        }
    }
    barrier_t bar; atomic_size_t q1, q2, q3; atomic_init(&q1, 0); atomic_init(&q2, 0); atomic_init(&q3, 0);
    if (P > 1) barrier_init(&bar, P);
    for (int t = 0; t < P; t++) { list_arg_t x = { a, dst, nx, n, K, u, m, &q1, &q2, &q3, &bar, P, t, plain }; w[t] = x; }
    int made = par_spawn(th, P, list_worker, w, sizeof w[0], P > 1 ? &bar : NULL, NULL);   /* every phase is queue-driven: an orphan is dropped from the barrier and the others drain its share */
    list_worker(&w[0]);
    par_join(th, made);
    if (P > 1) barrier_destroy(&bar);
    TRACE("knot, as a linked list: %zu units, %zu links of 4 bytes, P=%d%s", m, n, P, plain ? ", plain pointer walk" : "");
    g_list_count++;
    free(nx); free(u); free(w); free(th);
    return 1;
}
#endif

/* Returns 1 with a[] sorted, or 0 (a[] still a permutation of the input; the
 * caller continues on the normal path). dst: n-element scratch. */
/* STRAYS INSIDE STRANDS (2026-09-21; ceiling: ./strandprobe 30 8388608 strays, A/B: ./unknotprobe 40 8388608 ab STRAY; off with
 * LOGOS_NO_STRAY=1). Sorted blocks with a few late records in them used to pay for a full sort: every stray ended a run, and a few
 * thousand of them pushed the average run under the detector's floor. The detector now LIFTS a stray where it finds one (an element
 * below the run whose successor resumes it; one a gap above it; one accepted into the run whose successor falls back to the run;
 * and any run of one or two elements, which is what a run that starts with a stray looks like), records its position, and reads
 * on. Before the plan is made the holes are closed in one pass over the array, so every run stays contiguous and shorter and every
 * search and offset of the plan holds. The strays are sorted apart (a small sort)
 * and merged back IN PLACE OF the copy-back pass: the placement writes n - S elements into scratch, and one merge writes all n
 * back. Sequential sorts only, at most n / 64 strays, ascending runs only (a stray inside a twisted run still ends it). If the plan
 * declines after the holes were closed, the strays are written into the holes and the array is a permutation again. */
static size_t g_stray_count = 0;            /* logos_twist_last(10): strays lifted by routes that completed; calling thread */
static void stray_compact(double *a, size_t *runs, size_t R, const size_t *sp, size_t S, double *st) {
    size_t w = 0, q = 0;                     /* one pass, left to right: every write lands below every unread element */
    for (size_t k = 0; k < R; k++) {
        size_t s = runs[2 * k], e = RUN_END(runs[2 * k + 1]), tw = runs[2 * k + 1] & RUN_TWIST, ns = w, p = s;
        while (q < S && sp[q] < s) { st[q] = a[sp[q]]; q++; }                 /* strays between runs */
        while (q < S && sp[q] <= e) { size_t len = sp[q] - p; memmove(a + w, a + p, len * sizeof(double)); w += len; st[q] = a[sp[q]]; p = sp[q] + 1; q++; }
        memmove(a + w, a + p, (e + 1 - p) * sizeof(double)); w += e + 1 - p;
        runs[2 * k] = ns; runs[2 * k + 1] = (w - 1) | tw;
    }
    while (q < S) { st[q] = a[sp[q]]; q++; }
}
/* the inverse, when the plan declines after the holes were closed: expand from the right, strays back at their positions */
static void stray_restore(double *a, size_t n, const size_t *sp, size_t S, const double *st) {
    size_t w = n - S, q = S;
    for (size_t p = n; p-- > 0;) { if (q > 0 && sp[q - 1] == p) a[p] = st[--q]; else a[p] = a[--w]; }
}
static void psort_f64(double *a, size_t n, int P, size_t *vstart, int *vnbk, double *vmin, double *vmax);
/* the strays sorted, then merged with the placed n - S elements of dst into a: this pass replaces the copy-back */
static void stray_finish(double *a, const double *dst, size_t m, double *st, size_t S) {
    NESTED_SORT(psort_f64(st, S, 1, NULL, NULL, NULL, NULL));
    size_t i = 0, o = 0;
    for (size_t q = 0; q < S; q++) {
        double v = st[q]; size_t lo = i, hi = m;
        while (lo < hi) { size_t mid = lo + ((hi - lo) >> 1); if (dst[mid] < v) lo = mid + 1; else hi = mid; }
        if (lo > i) { memcpy(a + o, dst + i, (lo - i) * sizeof(double)); o += lo - i; i = lo; }
        a[o++] = v;
    }
    if (i < m) memcpy(a + o, dst + i, (m - i) * sizeof(double));
}

/* ONE STRAND, STRAYS PUT BACK IN PLACE (2026-09-22, tsbench's edge round). When the detector leaves one run and lifted strays, the
 * strand is one sorted sequence stored with holes where the strays were. It used to be finished in three full passes: holes closed,
 * the strand copied to scratch, the sorted strays merged back (20.7 of 34 ms on a strand with 1% adjacent swaps, whose 820 strays
 * each sit one or two positions from home). The short-reach route's put-back applies unchanged: a strand element's final place is its
 * rank in the strand plus the strays below it, so between two events (a hole, a stray's insertion point) every element moves by the
 * SAME amount, and the array is a list of segments, each one memmove. Segments that move left go first, left to right; then those
 * that move right, right to left; then each stray is written to its slot (reach_place_strays argues why nothing unread is
 * overwritten). One stray costs one memmove of the span it travels, so strays near home cost almost nothing. The positions are a
 * permutation whatever the binary searches answer, so a NaN in the strand costs order, never elements. LOGOS_ONERUN_COPY=1: the
 * three passes, for the A/B. */
typedef struct { size_t src, len, dst; } sseg_t;
static void strand_place_strays(double *a, size_t n, const size_t *sp, size_t S, const double *T, sseg_t *sg, size_t *tpos) {
    size_t ns = 0, ti = 0, cb = 0;
    for (size_t k = 0; k <= S; k++) {                                 /* piece k: between hole k-1 and hole k */
        size_t lo = k ? sp[k - 1] + 1 : 0, hi = k < S ? sp[k] : n, p = lo;
        if (hi <= lo) continue;
        while (ti < S && p < hi && T[ti] <= a[hi - 1]) {               /* this stray sorts before some element of this piece */
            size_t L = p, R = hi; double v = T[ti];
            while (L < R) { size_t m = L + ((R - L) >> 1); if (a[m] < v) L = m + 1; else R = m; }
            if (L > p) { sg[ns].src = p; sg[ns].len = L - p; sg[ns].dst = cb + (p - lo) + ti; ns++; }
            tpos[ti] = cb + (L - lo) + ti; ti++; p = L;
        }
        if (hi > p) { sg[ns].src = p; sg[ns].len = hi - p; sg[ns].dst = cb + (p - lo) + ti; ns++; }
        cb += hi - lo;
    }
    for (; ti < S; ti++) tpos[ti] = cb + ti;                           /* above every strand element */
    for (size_t i = 0; i < ns; i++) if (sg[i].dst < sg[i].src) memmove(a + sg[i].dst, a + sg[i].src, sg[i].len * sizeof(double));
    for (size_t i = ns; i-- > 0;) if (sg[i].dst > sg[i].src) memmove(a + sg[i].dst, a + sg[i].src, sg[i].len * sizeof(double));
    for (size_t i = 0; i < S; i++) a[tpos[i]] = T[i];
}

static int relocate_try_f64(double *a, size_t n, int P, double *dst, scratch_t *lz) {   /* dst, or lz to take it when something will be placed */
    { const char *e = getenv("LOGOS_RELOC_AVG"); long v = e ? atol(e) : 0;
      g_reloc_avg = v >= 16 ? (size_t)v : (P > 1 && n >= MIN_PARALLEL) ? RELOC_MIN_AVG_RUN_PAR : RELOC_MIN_AVG_RUN_SEQ; }
#ifndef LOGOS_NO_KNOT
    int kpar = P > 1 && n >= MIN_PARALLEL;
    double kfrac = g_knot_on ? (kpar ? g_knot_frac_par : g_knot_frac_seq) : 0.0, kfew = kpar ? kfrac : g_knot_frac_few;
#else
    double kfrac = 0.0, kfew = 0.0;
#endif
    if (!reloc_sample_gate(a, n, kfrac, kfew)) { TRACE("reloc: sample gate declined"); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
#ifdef LOGOS_NO_TWIST
    int twist = 0;
#else
    int twist = !getenv("LOGOS_NO_TWIST");      /* read where the route starts, never on a small sort's path: this is reached from RELOC_MIN_N up */
#endif
#ifndef LOGOS_NO_KNOT
    g_twist_generic = twist && getenv("LOGOS_TWIST_GENERIC") != NULL;
#endif
#ifdef LOGOS_NO_FASTSCAN
    int fast = 0;
#else
    int fast = getenv("LOGOS_NO_FASTSCAN") ? 0 : getenv("LOGOS_FASTSCAN_HOLD8") ? 1 : 2;   /* 1: eight scalar steps after any failed block */
#endif
#ifdef LOGOS_NO_KINK
    int kink = 0;
#else
    int kink = !getenv("LOGOS_NO_KINK");
#endif
    size_t R = 0, *sp = NULL, S = 0; int stray_on = g_stray_on && (P == 1 || n < MIN_PARALLEL || g_stray_par) && ns_depth == 0;
    size_t *runs = detect_runs_parallel(a, n, P, &R, kfrac, twist, fast, kink, stray_on ? &sp : NULL, &S);
    TRACE("reloc: detect_runs_parallel -> %s, R=%zu, strays=%zu", runs ? "runs" : "bail", R, S);
#ifdef LOGOS_TRACE
    /* a few runs: print them, so a strand that should be one run and is not says WHERE it broke (the kink-at-a-boundary
       hunt of 2026-09-26 needed exactly this: R=2 said nothing, [0..499998] [499999..n) said everything) */
    if (runs && R <= 8) for (size_t k = 0; k < R; k++) TRACE("reloc:   run %zu: [%zu .. %zu] %s", k, runs[2 * k], RUN_END(runs[2 * k + 1]), RUN_REV(runs[2 * k + 1]) ? "descending" : "ascending");
#endif
    if (!runs) { free(sp); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
    /* ONE STRAND (2026-09-22, found by tsbench's kinks row): one sorted strand with 1% adjacent swaps. The detector swapped the kinks
       back and lifted the tangled ones as strays, which left ONE run, and a plan needs two: the route declined and the leaf re-sorted an
       array that was sorted but for 820 elements (0.11 against a merge sort that only reverses and bypasses; 0.90 against std::sort).
       One run and no strays is a sorted array (or its mirror); one run with strays is placed by the strays' own merge. LOGOS_NO_ONERUN=1:
       off, for the A/B (./tsbench ... onerun). */
    if (R == 1 && !g_onerun_off) {
        size_t s0 = runs[0], e0 = RUN_END(runs[1]); int rv0 = RUN_REV(runs[1]);
        if (!S && s0 == 0 && e0 == n - 1) {
            if (rv0) for (size_t l = 0, r = n - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }
            TRACE("reloc: one run, %s: done", rv0 ? "descending" : "ascending"); free(runs); g_onerun_count++; PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 1;
        }
        if (S && S < n) {
            double *st1 = malloc(S * sizeof(double));
            if (st1 && !rv0 && !g_onerun_copy) {                      /* in place: one memmove per segment (strand_place_strays) */
                sseg_t *sg = malloc((2 * S + 2) * sizeof *sg); size_t *tpos = malloc(S * sizeof *tpos);
                if (sg && tpos) {
                    for (size_t q = 0; q < S; q++) st1[q] = a[sp[q]];
                    NESTED_SORT(psort_f64(st1, S, 1, NULL, NULL, NULL, NULL));
                    strand_place_strays(a, n, sp, S, st1, sg, tpos);
                    TRACE("reloc: one run and %zu strays: put back in place", S); g_stray_count += S; g_onerun_count++;
                    free(sg); free(tpos); free(st1); free(sp); free(runs); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 1;
                }
                free(sg); free(tpos);                                  /* no room for the plan: the three passes below */
            }
            if (st1 && (dst || (dst = scratch_get(lz)))) {
                stray_compact(a, runs, 1, sp, S, st1);            /* the strand now fills [0, n - S) */
                strand_copy(dst, a, n - S, rv0);                   /* the merge reads it from scratch and writes the array */
                stray_finish(a, dst, n - S, st1, S);
                TRACE("reloc: one run and %zu strays: merged", S); g_stray_count += S; g_onerun_count++;
                free(st1); free(sp); free(runs); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 1;
            }
            free(st1);                                             /* no scratch for the three passes: declined below */
        }
    }
    if (R < 2) { free(runs); free(sp); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }
    /* strays: close the holes before the plan reads the runs; a decline below puts them back into the holes */
    double *st = NULL; size_t n_eff = n;
    if (S) {
        st = malloc(S * sizeof(double));
        if (!st) { free(sp); free(runs); PROF(prof_phase(LOGOS_PH_RELOC_PROBE);) return 0; }   /* nothing moved yet: the runs hold strays, not a plan */
        stray_compact(a, runs, R, sp, S, st); n_eff = n - S;
    }
#define STRAY_BAIL() do { if (S) stray_restore(a, n, sp, S, st); free(st); free(sp); } while (0)
    int cb = !g_reloc_nocopy && !S;                    /* the placements copy back themselves unless the strays' merge does it, or the ceiling switch is set */
    rdesc_t *d; size_t *ch, nc, merged;
    int ok = runs_plan(a, runs, R, &d, &ch, &nc, &merged); free(runs);
    PROF(prof_phase(LOGOS_PH_RELOC_PROBE);)
    if (!ok) { STRAY_BAIL(); return 0; }
    size_t ntw = 0; for (size_t q = 0; q < R; q++) ntw += (size_t)d[q].rev;
    TRACE("reloc: plan chains=%zu merged=%zu (gate n/2=%zu)", nc, merged, n / 2);
#ifndef LOGOS_NO_KNOT
    if (kfrac > 0 && n >= (size_t)KNOT_MIN_AVG_RUN * R) {
        int Pk = n < MIN_PARALLEL ? 1 : P;
        size_t grain = n + 1;
        if (Pk > 1) { grain = n / ((size_t)Pk * 8); if (grain < KNOT_GRAIN_MIN) grain = KNOT_GRAIN_MIN; }
        knot_plan_t K;
        if (knot_plan(a, d, R, grain, 1, &K)) {
            /* Thread policy: relocation's one thread per RELOC_MIN_PER_THREAD elements was measured on pure copies, which
             * are bandwidth-bound. A merge is compute-bound, about three copies' worth per element per level, and the first
             * cut of this route inherited the copy cap: at 1M it placed on 2 threads what the partition it displaced sorted
             * on 10, and the heavily crossed shapes LOST there (80% crossing 0.84, three strands 0.94) while winning 1.2-1.5x
             * at 8.4M where the cap does not bind. So the cap counts work, not elements. */
            { size_t work = n + KNOT_MERGE_WEIGHT * K.cost, pm = work / RELOC_MIN_PER_THREAD; if (pm < 1) pm = 1; if ((size_t)Pk > pm) Pk = (int)pm; }
            double kf = R > MERGE_MAX_RUNS ? kfrac : kfew;
            int go = K.total == n_eff && ((double)K.cost <= kf * (double)n || merged <= n / 2);
            if (go && !dst && !(dst = scratch_get(lz))) go = 0;    /* the scratch, now that something will be placed */
            TRACE("knot: R=%zu chains=%zu in-chains=%zu | crossing=%zu cost=%zu kmax=%zu pieces=%zu merges=%zu (k>=3: %zu) gate %.2f -> %s",
                  R, nc, merged, K.crossing, K.cost, K.kmax, K.npc, K.nsg, K.n3, kf, go ? "untie" : "decline");
            size_t *krl = NULL; knot_arg_t *kw = NULL; pthread_t *kth = NULL; unsigned char *korph = NULL;
            if (go && K.n3) { krl = knot_rl(&K); if (!krl) go = 0; }
            /* the literal linked list, opt-in (see knot_place_list); it declines without touching anything and the array placement follows */
            if (go && !S && cb && getenv("LOGOS_KNOT_LIST") && knot_place_list(a, dst, n, &K, Pk, getenv("LOGOS_LIST_PLAIN") != NULL)) {
                g_twist_knot += ntw;                                  /* twisted strands, consumed as links that point the other way */
                free(krl); knot_plan_free(&K); free(d); free(ch); return 1; }
            /* one thread: half the scratch (half_place). The strays are sorted first; a decline leaves them sorted, which the full
               placement below and a bail (stray_restore puts them back into the holes in any order) both accept */
            if (go && Pk == 1 && !g_reloc_nocopy && half_on(n_eff)) {
                if (S) NESTED_SORT(psort_f64(st, S, 1, NULL, NULL, NULL, NULL));
                if (knot_place_half(a, dst, n_eff, &K, st, S)) {
                    TRACE("knot: placed with half a scratch (P=1), %zu of %zu strands twisted, %zu strays", ntw, R, S);
                    g_twist_knot += ntw; g_stray_count += S;
                    free(st); free(sp); free(krl); knot_plan_free(&K); free(d); free(ch);
                    return 1;
                }
            }
            if (go) { kw = malloc((size_t)Pk * sizeof *kw); kth = malloc((size_t)Pk * sizeof *kth); korph = calloc((size_t)Pk, 1); if (!kw || !kth || !korph) go = 0; }
            if (go) {
                barrier_t kbar; atomic_size_t nx2, nx3; atomic_init(&nx2, 0); atomic_init(&nx3, 0);
                if (Pk > 1) barrier_init(&kbar, Pk);
                int knt = place_nt_ok(n);
                for (int t = 0; t < Pk; t++) { knot_arg_t w = { a, dst, n_eff, Pk, t, &K, krl, &nx2, &nx3, &kbar, NULL, cb, knt }; kw[t] = w; }
                int kmade = par_spawn(kth, Pk, knot_worker, kw, sizeof kw[0], Pk > 1 ? &kbar : NULL, korph);   /* as in relocation */
                kw[0].orphan = korph;
                knot_worker(&kw[0]);
                par_join(kth, kmade);
                if (Pk > 1) barrier_destroy(&kbar);
                TRACE("knot: copies + crossing merges + copyback done (P=%d), %zu of %zu strands twisted, %zu strays", Pk, ntw, R, S);
                g_twist_knot += ntw;
                if (S) { stray_finish(a, dst, n_eff, st, S); g_stray_count += S; }
                free(st); free(sp);
                free(kw); free(kth); free(korph); free(krl); knot_plan_free(&K); free(d); free(ch);
                return 1;
            }
            free(kw); free(kth); free(korph); free(krl); knot_plan_free(&K);
        }
    }
#endif
    if (merged > n / 2) { free(d); free(ch); STRAY_BAIL(); return 0; }
    size_t *off = malloc(R * sizeof(size_t)), *rl = malloc(2 * R * sizeof(size_t));
    if (!off || !rl) { free(d); free(ch); free(off); free(rl); STRAY_BAIL(); return 0; }
    for (size_t q = 0, o = 0; q < R; q++) { off[q] = o; rl[2 * q] = o; rl[2 * q + 1] = o + d[q].len - 1; o += d[q].len; }
    /* Relocation's parallel form has a serial middle (plan) and a spawn; below RELOC_MIN_PER_THREAD elements
       per thread it lost to the sequential form (block_perm 316k: 1.4 vs 1.0 ms with 3 threads; 1M: 2.5 vs 2.7
       with 10, scale-full). Use at most one thread per that many elements. A/B: make smallbench. */
    if (n < MIN_PARALLEL) P = 1;
    { size_t pm = n / RELOC_MIN_PER_THREAD; if (pm < 1) pm = 1; if ((size_t)P > pm) P = (int)pm; }
    barrier_t bar; atomic_size_t next; atomic_init(&next, 0);
    if (!dst && !(dst = scratch_get(lz))) { free(off); free(rl); free(d); free(ch); STRAY_BAIL(); return 0; }   /* no scratch: nothing placed */
    reloc_arg_t *wa = malloc((size_t)P * sizeof *wa); pthread_t *th = malloc((size_t)P * sizeof *th); unsigned char *orphan = calloc((size_t)P, 1);
    if (!wa || !th || !orphan) { free(wa); free(th); free(orphan); free(off); free(rl); free(d); free(ch); STRAY_BAIL(); return 0; }
    if (P > 1) barrier_init(&bar, P);
    int rnt = place_nt_ok(n);
    for (int t = 0; t < P; t++) { reloc_arg_t w = { a, dst, n_eff, R, nc, P, t, d, off, ch, rl, &next, &bar, NULL, cb, rnt }; wa[t] = w; }
    /* static output tiles and a barrier: an orphan is dropped from the barrier and the caller fills and copies back its tile */
    int rel_made = par_spawn(th, P, reloc_worker, wa, sizeof wa[0], P > 1 ? &bar : NULL, orphan);
    wa[0].orphan = orphan;
    reloc_worker(&wa[0]);
    par_join(th, rel_made);
    TRACE("reloc: copy+merge+copyback done (P=%d), %zu of %zu runs twisted, %zu strays", P, ntw, R, S);
    g_twist_reloc += ntw;
    if (S) { stray_finish(a, dst, n_eff, st, S); g_stray_count += S; }
    free(st); free(sp);
#undef STRAY_BAIL
    if (P > 1) barrier_destroy(&bar);
    free(wa); free(th); free(orphan); free(off); free(rl); free(d); free(ch);
    return 1;
}
#endif


/* ===================== short reach: sort where it lies, repair only the seams =====================
 * The knot route priced how many strands cross. This route prices how FAR the disorder reaches. In an event log whose
 * records arrive with bounded latency, in a series shuffled inside small windows, in anything jittered around its own
 * order, every element already sits near its final place: no inverted pair is further apart than some reach rho, and
 * rho is a few thousand positions in an array of millions. The partition does not know that. It samples, scatters all
 * n elements into a second buffer (the writes happen to land nearly in order, so it is fast, but it is still a pass out
 * and a pass back) and sorts 33k-element buckets there.
 *
 * If rho is small the scatter is unnecessary: cut the array into index blocks of B >= 4 rho, sort every block WHERE IT
 * LIES, in parallel (a flash sort of the route's own: see reach_sort_direct), and then look only at the seams. For the seam between sorted blocks L
 * and R: y = the elements of L greater than R[0], x = the elements of R less than the last of L, both by binary search.
 * Merging those two pieces in place sorts L u R, and what crosses a seam rightward is matched by what crosses leftward,
 * so nothing else moves. The seams are independent, and the result is globally sorted, as long as every block keeps an
 * untouched middle. That is GUARANTEED here rather than hoped for: with m(k) the median of sorted block k, an element
 * of block k below m(k-1) or above m(k+1) is a STRAY and is set aside first; what is left of block k lies in
 * [m(k-1), m(k+1)], so x(k) counts elements below m(k), y(k+1) counts elements above it, and m(k) itself is in neither.
 *
 * Strays (one late event, a zero in a column of timestamps) are what a sample cannot see, and they decide the cost:
 *   none: block sort + seams, in place. No n-sized buffer is ever asked for.
 *   a few, found by the block sorts: they are sorted apart and put back by ONE in-place shift pass. Between two strays
 *     every element moves by the same amount, so the pass is a handful of memmoves over exactly the span the strays
 *     travel, which is the movement any algorithm owes them. (The first cut merged them through a second buffer: two
 *     parallel passes, +36 ms at one thread and +12 ms at 14 for a single stray.)
 *   in many blocks, in a sort wider than four threads: the serial shift pass costs what the route saves there. Eight probe
 *     blocks are held against their fences before a thread is started or an element moved (0.05-0.1 ms); two that break
 *     one decline. Up to four threads strays are simply put back (1.5-1.8x at one thread with strays in every block).
 *   more than n/32, or medians that do not ascend: not this structure. Decline; the rest of the dispatcher gets an
 *     array of sorted blocks, which its run routes like.
 *
 * The gate reads the dispatcher's sample and four small windows, never the array. Sample indices i and i+L sit
 * |frac(L phi)| n positions apart; for the Fibonacci lags 144, 233, 377, 610, 987, 1597 that is 0.31%, 0.19%, 0.12%,
 * 0.073%, 0.045% and 0.028% of n, a ladder of distances. The reach estimate is the smallest rung from which no distance
 * shows more than REACH_TAU inverted pairs (a stray inverts its pair at every distance, so the tolerance is what lets a
 * few strays through). Random data fails the first rung, which the shared sample has already read, so it pays nothing.
 * Then four 4096-element windows must show DENSE adjacent descents: sparse disorder on sorted data is the near-sorted
 * route's kind and is left to it.
 *
 * Ceilings first (make reachhunt, FINDINGS 3af): told the structure, blocks + seams beat the shipped engine 1.28-1.37x
 * at one thread and 1.18-1.24x at 14 on the two bounded-displacement shapes of the zoo; cut decomposition and chunk
 * relocation were measured beside it and are not built. MEASURED as built (make reachbench, 8.4M, route off / on in one
 * binary): event log with latency < 4096 ms 1.91 / 1.31 (1 / 14 threads), 2048-element shuffled windows 2.22 / 1.38, noise
 * of +-1000 2.05 / 1.33, latency < 512 ms 2.38 / 1.49; more at 30M (2.1-2.9 / 1.5-1.8); guards 0.98-1.03 against the engine
 * before it, layout cancelled. It weighs 0.10 bytes per element at one thread and 1.33 at 14 (make memprobe), against
 * 15.5 and 23.2. Off at runtime with LOGOS_NO_REACH=1, at compile time with -DLOGOS_NO_REACH. f64 only; above ~117M
 * elements the ladder's finest rung is coarser than a block may be and the gate declines. */
#if (defined(LOGOS_STRIDE_SAMPLE) || defined(LOGOS_NO_SODO)) && !defined(LOGOS_NO_REACH)
#define LOGOS_NO_REACH                /* the gate is a ladder of Weyl lags over the shared sample: no Weyl sample, no route */
#endif
#ifndef LOGOS_NO_REACH
#ifndef REACH_MIN_N
#define REACH_MIN_N      (1u << 18)
#endif
#ifndef REACH_TAU
#define REACH_TAU        0.02         /* inverted share of the sampled pairs a rung may show and still count as past the reach */
#endif
#ifndef REACH_BLOCK
#define REACH_BLOCK      32768        /* a block, its scratch and its context stay in L2: the size of a partition leaf */
#endif
#ifndef REACH_BLOCK_MAX
#define REACH_BLOCK_MAX  131072       /* past this the block sort is no longer an in-cache sort and the route has no edge */
#endif
#define REACH_MIN_BLOCKS 8
#ifndef REACH_DENSE
#define REACH_DENSE      0.20         /* adjacent descents in the probe windows; below this the disorder is sparse (0.25 until 2026-10-21: see below) */
/* 0.25 sat exactly on two sorted sources interleaved at random, which show 25% adjacent descents in expectation: the seed decided
   whether this route ran (5.0-5.3 ns per element at one thread, 4M) or the leaf did (10.3-14.5; fieldprobe seeds src 2, 2026-10-21).
   At 0.20 all eight seeds run it (5.5-6.3). LOGOS_REACH_DENSE=<f> moves it, for the A/B. */
/* SPARSE disorder has two kinds, and until 2026-09-26 this probe told the dispatcher only that it was sparse. Random swaps
   DISPLACE elements, and the near-sorted route wins on them (1.80-1.97x at two threads); adjacent swaps are KINKS, which the
   run detector repairs in place (5.5x at one thread) -- but at P > 1 near-sorted claimed those too, so the detector never ran
   and one kinked strand cost 33.2 ms at two threads against 19.4 at one (make kinkladder). The two kinds look different in
   these same windows: a kink is a descent whose pair, swapped, fits between its neighbours (the detector's own test); a
   displaced element fails that on one side. When at least REACH_KINK_MIN descents are seen and REACH_KINK_FRAC of them are
   kink-shaped, the decline is reported as LOGOS_REACH_DECLINED_KINKS and the dispatcher hands the array past near-sorted.
   LOGOS_NO_KINKGATE=1 restores the plain sparse decline, for the A/B. */
#ifndef REACH_KINK_FRAC
#define REACH_KINK_FRAC  0.90
#endif
#ifndef REACH_KINK_MIN
#define REACH_KINK_MIN   3            /* was 8, which sat above the ~3-8 descents that 0.02-0.05% kinks put in 16,384 pairs (2026-10-19) */
#endif
#endif
#define REACH_STRAY_DIV  32           /* more strays than n/32: decline */
#define REACH_PROBES     8
#ifndef REACH_TOLERANT_P
#define REACH_TOLERANT_P  4
#endif
static _Thread_local int g_reach_last = LOGOS_REACH_NOT_TRIED;      /* logos_reach_last(): what this route did on the last top-level f64 sort */
/* The ladder. Returns the reach estimate in elements, 0 when even the longest rung is inverted too often. raw[] is the
 * dispatcher's gate sample in Weyl order; it is 2048 points from six threads up and 384 at one, which has room for two
 * rungs only, so below 2048 the ladder reads its own points (the first m are the same ones; ~20 us, and only for data
 * that has already passed the first rung). Positions are computed once; every rung after that is compares. */
#define REACH_LADDER_M 2048
/* noinline: its two 16 KB arrays would otherwise land in the dispatcher's frame, whose prologue probes every page of it on EVERY
   call, the three-element ones included (measured: +2 probes, +0.8 ns at n = 0 and 1, before this attribute). */
static __attribute__((noinline)) size_t reach_from_sample(const double *a, size_t n, const double *raw, size_t m, int mirror) {   /* mirror: the ladder of the array read back to front */
    static const unsigned short LAG[6] = { 144, 233, 377, 610, 987, 1597 };
    double own[REACH_LADDER_M]; size_t pos[REACH_LADDER_M];
    if (n < REACH_LADDER_M) return 0;
    if (m < REACH_LADDER_M) { int nan = 0; sample_read(a, n, 0, REACH_LADDER_M, REACH_LADDER_M, own, &nan); if (nan) return 0; raw = own; }
    m = REACH_LADDER_M;
    for (size_t i = 0; i < m; i++) pos[i] = SAMPLE_POS(i, m, n);
    size_t best = 0;
    for (int k = 0; k < 6; k++) {
        size_t L = LAG[k], inv = 0, comps = 0, dist = 0;
        for (size_t i = 0; i + L < m; i++) {
            size_t p0 = pos[i], p1 = pos[i + L]; if (p0 == p1) continue;
            size_t d = p1 > p0 ? p1 - p0 : p0 - p1; if (d > n / 2) continue;              /* a wrapped pair: a whole array apart, not this rung */
            comps++; dist = d; { double lo_ = p1 > p0 ? raw[i] : raw[i + L], hi_ = p1 > p0 ? raw[i + L] : raw[i]; if (mirror ? lo_ < hi_ : lo_ > hi_) inv++; }   /* lo_: the EARLIER position */
        }
        if (comps < 64 || (double)inv > REACH_TAU * (double)comps) break;
        best = dist;
    }
    return best;
}
static size_t reach_upper_bound(const double *a, size_t lo, size_t hi, double v) { while (lo < hi) { size_t m = lo + ((hi - lo) >> 1); if (a[m] <= v) lo = m + 1; else hi = m; } return lo; }
typedef struct { size_t src, len, dst; } reach_seg_t;
typedef struct {
    double *a, *T, *fence; size_t n, B, S; int K, go, leafmode, tolerant;   /* go: 0 decline, 1 seams only, 2 seams, then the strays are put back */
    size_t *lo, *hi, *x, *y;                                           /* per block: core [lo,hi) and the seam counts */
    atomic_size_t nblk, nseam, nstrayblk;
    barrier_t *bar;
} reach_job_t;
typedef struct { reach_job_t *j; ctx_t *ctx; double *scratch; int tid; } reach_arg_t;
static inline size_t reach_s(const reach_job_t *j, int k) { return (size_t)k * j->B; }
static inline size_t reach_e(const reach_job_t *j, int k) { return k + 1 == j->K ? j->n : (size_t)(k + 1) * j->B; }
/* A block is sorted by the leaf, and the leaf's flash and coarse sorts map values linearly onto classes: one stray (a
 * zero in a column of timestamps) stretches that map until every real element lands in one class. Measured on the first
 * cut: the block sorts of latency + 0.01% strays took 15.7 ms at 14 threads against 9.2 without the strays, a sentinel
 * shape 27 against 10. So a block is first held against two FENCES, the estimated medians of its neighbours (nine
 * strided elements each, read before any thread starts): min and max in one pass over a block the sort is about to load
 * anyway, and only a block that breaks a fence is split three ways, its few strays sorted apart from its core. The
 * block comes out fully sorted either way, so a fence that is wrong costs time and nothing else. */
static inline double reach_est_median(const double *p, size_t len) {
    double w[9]; size_t st = len / 9; for (int i = 0; i < 9; i++) w[i] = p[(size_t)i * st + st / 2];
    insertion_sort(w, 0, 8); return w[4];
}
/* four independent chains each way: the compiler keeps them in registers and the pass runs at the speed of the loads */
static void reach_minmax(const double *p, size_t nb, double *mn, double *mx) {
    double l0 = p[0], l1 = l0, l2 = l0, l3 = l0, h0 = l0, h1 = l0, h2 = l0, h3 = l0; size_t i = 0;
    for (; i + 4 <= nb; i += 4) { double v0 = p[i], v1 = p[i + 1], v2 = p[i + 2], v3 = p[i + 3];
        l0 = v0 < l0 ? v0 : l0; l1 = v1 < l1 ? v1 : l1; l2 = v2 < l2 ? v2 : l2; l3 = v3 < l3 ? v3 : l3;
        h0 = v0 > h0 ? v0 : h0; h1 = v1 > h1 ? v1 : h1; h2 = v2 > h2 ? v2 : h2; h3 = v3 > h3 ? v3 : h3; }
    for (; i < nb; i++) { l0 = p[i] < l0 ? p[i] : l0; h0 = p[i] > h0 ? p[i] : h0; }
    l0 = l1 < l0 ? l1 : l0; l2 = l3 < l2 ? l3 : l2; h0 = h1 > h0 ? h1 : h0; h2 = h3 > h2 ? h3 : h2;
    *mn = l2 < l0 ? l2 : l0; *mx = h2 > h0 ? h2 : h0;
}
static int reach_breaks_fence(const reach_job_t *j, int k) {
    double mn, mx; reach_minmax(j->a + reach_s(j, k), reach_e(j, k) - reach_s(j, k), &mn, &mx);
    return (k > 0 && mn < j->fence[k - 1]) || (k + 1 < j->K && mx > j->fence[k + 1]);
}
/* The block sort proper. A block of this route is known to be densely disordered and, strays aside, spread over its own
 * range, so sort_fn's prescan, run detection and probes (about 2 of its ~12 ns per element) are skipped and the block
 * goes straight to a kernel: the AVX2 quicksort in a sequential sort, a flash sort otherwise. sort_fn's 40-pair probe
 * also MISJUDGES these blocks when the reach is under its stride of n/40: it calls a block with latency < 512 nearly
 * sorted, tries an insertion sort that gives up after 8n moves, and keeps the quicksort away (latency_512 at 14
 * threads: 1.97x between the two block sorts, make reachbench with RB_OFF/RB_ON).
 * The flash here is the engine's with two differences. Its classes are counted before anything is scattered, and a class
 * above REACH_FLASH_MAXPOP sends the block to the general leaf instead: a block that straddles a quiet hour holds two
 * clusters, the spread check between sample quantiles passes it, and the insertion pass would be quadratic inside a
 * class. And a NaN indexes class 0 by a compare, not by the cast of a NaN. Returns 0 when the block is not its kind. */
#define REACH_FLASH_MAXPOP 48
static int reach_sort_direct(reach_arg_t *w, double *p, size_t nb, double mn, double mx, int mode) {
    (void)mode;                                                        /* read by the AVX2 leaf only */
    if (nb < FLASH_SORT_THRESHOLD || !(mx > mn) || !isfinite(mx - mn)) return 0;
    if ((mx - mn) < (double)COUNTING_SORT_K * (double)nb) {           /* integers over a small span: the general leaf counts them, which no sort beats */
        int ints = 1; size_t st8 = nb / 8; for (int q = 0; q < 8 && ints; q++) { double v = p[(size_t)q * st8]; ints = v == floor(v); }
        if (ints) return 0;       /* (the first cut sent EVERY small span there: a series with unit spacing, i + noise, spans just under its length,
                                     and its blocks took the general leaf: 293 ms at one thread on 64-element windows, 76 through this kernel) */
    }
#ifdef LOGOS_AVX2_LEAF
    if (mode == 1 && w->ctx->seq && avx2_leaf_on() && avx2_qsort_f64(p, nb)) { PATH_TAG(w->ctx, LOGOS_PATH_SIMD); return 1; }
#endif
    { size_t st = nb / 32, desc = 0; for (size_t i = 0; i + 1 < nb && !desc; i += st) desc = p[i] > p[i + 1];
      if (!desc) return 0; }                                         /* no descent in 32 looks: perhaps sorted already, which the general leaf settles in one pass */
    /* Disorder that reaches two or three places is an insertion sort's: under two moves per element, where the flash below
       makes three passes (noise of +-2 at one thread: 0.81 against the shipped engine with flash alone, which sorts that
       shape by insertion). 64 strided pairs FOUR apart tell: reach 4 shows none inverted, noise of +-8 shows a quarter.
       The momentum sort gives up past 8n moves and leaves a permutation behind, so a wrong guess costs time only. */
    { size_t st = nb / 64, d4 = 0; for (size_t i = 0; i + 4 < nb; i += st) d4 += p[i] > p[i + 4];
      if (d4 <= 3 && insertion_sort_momentum(p, 0, (long)nb - 1)) { PATH_TAG(w->ctx, LOGOS_PATH_INSERTION); return 1; } }
    size_t m = nb; double scale = (double)(m - 1) / (mx - mn);
    if (!isfinite(scale) || !(scale > 0)) return 0;
    ctx_fit_t cf; if (!ctx_fit(w->ctx, nb, &cf)) return 0;
    uint32_t *cnt = w->ctx->aux; double *buf = w->scratch; int ok = 1;
    memset(cnt, 0, (m + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < nb; i++) { double d = (p[i] - mn) * scale; size_t idx = d >= 0 ? (size_t)d : 0; cnt[idx + 1]++; }
    uint32_t big = 0; for (size_t i = 1; i <= m; i++) { if (cnt[i] > big) big = cnt[i]; cnt[i] += cnt[i - 1]; }
    if (big > REACH_FLASH_MAXPOP) ok = 0;
    else {
        for (size_t i = 0; i < nb; i++) { double v = p[i], d = (v - mn) * scale; size_t idx = d >= 0 ? (size_t)d : 0; buf[cnt[idx]++] = v; }
        insertion_sort(buf, 0, (long)nb - 1);
        memcpy(p, buf, nb * sizeof(double));
        PATH_TAG(w->ctx, LOGOS_PATH_FLASH);
    }
    ctx_unfit(w->ctx, &cf);
    return ok;
}
static void reach_sort_range(reach_arg_t *w, double *p, size_t nb) {
    ctx_fit_t cf; if (nb < 2) return;
    if (ctx_fit(w->ctx, nb, &cf)) { double *r = leaf_f64(p, nb, w->scratch, w->ctx); if (r != p) memcpy(p, r, nb * sizeof(double)); ctx_unfit(w->ctx, &cf); }
    else quicksort(p, 0, (long)nb - 1, 2 * log2i(nb));
}
static void reach_blocks(reach_arg_t *w) {
    reach_job_t *j = w->j; size_t k;
    while ((k = atomic_fetch_add(&j->nblk, 1)) < (size_t)j->K) {
        size_t s = reach_s(j, (int)k), nb = reach_e(j, (int)k) - s; double *p = j->a + s;
        PROF(prof_stamp_t s0 = prof_stamp(); w->ctx->path = LOGOS_PATH_NONE;)
        double bmn, bmx; reach_minmax(p, nb, &bmn, &bmx);
        double flo = k > 0 ? j->fence[k - 1] : -INFINITY, fhi = k + 1 < (size_t)j->K ? j->fence[k + 1] : INFINITY;
        if (bmn < flo || bmx > fhi) {                                   /* lows | core | highs, then each part on its own */
            size_t lt = 0, gt = nb, i = 0;
            while (i < gt) { double v = p[i]; if (v < flo) { p[i] = p[lt]; p[lt] = v; lt++; i++; } else if (v > fhi) { gt--; p[i] = p[gt]; p[gt] = v; } else i++; }
            reach_sort_range(w, p, lt); reach_sort_range(w, p + gt, nb - gt);
            /* the core is what the block would have been without its strays: the same kernel, on its own range (the first cut gave it
               to the general leaf: 242 of 256 blocks of latency + 0.01% strays took 206 ms at one thread, against ~55 for clean blocks) */
            if (gt - lt > 1) { double cmn, cmx; reach_minmax(p + lt, gt - lt, &cmn, &cmx);
                if (!(j->leafmode && reach_sort_direct(w, p + lt, gt - lt, cmn, cmx, j->leafmode))) reach_sort_range(w, p + lt, gt - lt); }
            atomic_fetch_add_explicit(&j->nstrayblk, 1, memory_order_relaxed);
        } else if (!(j->leafmode && reach_sort_direct(w, p, nb, bmn, bmx, j->leafmode))) reach_sort_range(w, p, nb);
        PROF(double ms = prof_busy_since(w->tid, LOGOS_PH_BUCKETS, s0); prof_path(w->tid, w->ctx->path, nb, ms);)
    }
}
/* One seam: the y elements at the end of core k-1 and the x at the start of core k, merged in place. The output runs
 * through the end of core k-1 and then JUMPS the stray slots between the two cores (no strays: the jump is zero). It
 * can never overtake the read pointer in R: after i elements from tmp and q from R it stands q - (y - i) <= q slots
 * into R. When tmp is empty the output stands exactly on the read pointer and the rest of R is already in place. */
static void reach_seams(reach_arg_t *w) {
    reach_job_t *j = w->j; size_t k; double *tmp = w->scratch;
    while ((k = atomic_fetch_add(&j->nseam, 1)) < (size_t)j->K) {
        if (k == 0 || !j->y[k]) continue;
        PROF(prof_stamp_t s0 = prof_stamp();)
        size_t y = j->y[k], x = j->x[k]; double *o = j->a + j->hi[k - 1] - y, *oe = j->a + j->hi[k - 1];
        memcpy(tmp, o, y * sizeof(double));
        const double *p = tmp, *pe = tmp + y, *r = j->a + j->lo[k], *re = r + x;
        while (p < pe && r < re) { double v; if (*r < *p) v = *r++; else v = *p++; *o++ = v; if (o == oe) o = j->a + j->lo[k]; }
        while (p < pe) { *o++ = *p++; if (o == oe) o = j->a + j->lo[k]; }
        PROF(prof_busy_since(w->tid, LOGOS_PH_NEARSORTED, s0);)
    }
}
/* Between the block sorts and the seams, on the calling thread while the others wait at the barrier: medians, fences,
 * strays, seam counts. Everything is checked, so data this route should never have met (a NaN the sample missed makes
 * every binary search meaningless) ends in a decline or in a permutation, never in a write out of bounds. */
static void reach_plan(reach_job_t *j) {
    double *a = j->a; int K = j->K; size_t S = 0;
    j->go = 0;
    for (int k = 0; k < K; k++) {
        size_t s = reach_s(j, k), e = reach_e(j, k), mid = s + (e - s) / 2;
        double mlo = k > 0 ? a[reach_s(j, k - 1) + (reach_e(j, k - 1) - reach_s(j, k - 1)) / 2] : a[s];
        double mhi = k + 1 < K ? a[reach_s(j, k + 1) + (reach_e(j, k + 1) - reach_s(j, k + 1)) / 2] : a[e - 1];
        if (!(mlo <= a[mid]) || !(a[mid] <= mhi)) { TRACE("reach: medians do not ascend at block %d -> decline", k); return; }
        size_t lo = k > 0 ? lower_bound_d(a, s, e, mlo) : s, hi = k + 1 < K ? reach_upper_bound(a, s, e, mhi) : e;
        if (!(lo <= mid && mid < hi)) { TRACE("reach: core of block %d lost its median -> decline", k); return; }
        j->lo[k] = lo; j->hi[k] = hi; S += (lo - s) + (e - hi);
    }
    if (S > j->n / REACH_STRAY_DIV) { TRACE("reach: %zu strays of %zu -> decline", S, j->n); return; }
    j->x[0] = j->y[0] = 0; j->y[K] = 0;
    for (int k = 1; k < K; k++) {
        double lfirst = a[j->lo[k]], llast = a[j->hi[k - 1] - 1]; j->x[k] = j->y[k] = 0;
        if (!(lfirst < llast)) continue;
        j->y[k] = j->hi[k - 1] - reach_upper_bound(a, j->lo[k - 1], j->hi[k - 1], lfirst);
        j->x[k] = lower_bound_d(a, j->lo[k], j->hi[k], llast) - j->lo[k];
    }
    for (int k = 0; k < K; k++) if (j->x[k] + j->y[k + 1] >= j->hi[k] - j->lo[k]) { TRACE("reach: block %d keeps no middle -> decline", k); return; }
    j->S = S; j->go = S ? 2 : 1;
}
/* The strays go back in, in place. The cores are one sorted sequence now, stored with holes where the strays were; the
 * strays are sorted in T. A core element's final place is its rank among the cores plus the strays below it, so between
 * two consecutive events (a hole, a stray's insertion point) every element moves by the SAME amount: the array is a list
 * of segments, each one memmove. Order: segments that move left, left to right; then segments that move right, right to
 * left. A left-mover lands on holes or on where an earlier left-mover used to be, never on a right-mover's source (its
 * target starts past that source's end), and a right-mover's target ends where its right neighbour's final place
 * begins, which by then holds nothing unread. Then every stray is written to its slot. One stray costs one memmove of
 * the span it travels. sg[] and tpos[] were sized before anything moved: from here on nothing can fail. */
static void reach_place_strays(reach_job_t *j, reach_seg_t *sg, size_t *tpos) {
    double *a = j->a; const double *T = j->T; size_t S = j->S, ns = 0, ti = 0, cb = 0;
    for (int k = 0; k < j->K; k++) {
        size_t lo = j->lo[k], hi = j->hi[k], p = lo;
        while (ti < S && p < hi && T[ti] <= a[hi - 1]) {                /* this stray sorts before some element of this core */
            size_t q = lower_bound_d(a, p, hi, T[ti]);
            if (q > p) { sg[ns].src = p; sg[ns].len = q - p; sg[ns].dst = cb + (p - lo) + ti; ns++; }
            tpos[ti] = cb + (q - lo) + ti; ti++; p = q;
        }
        if (hi > p) { sg[ns].src = p; sg[ns].len = hi - p; sg[ns].dst = cb + (p - lo) + ti; ns++; }
        cb += hi - lo;
    }
    for (; ti < S; ti++) tpos[ti] = cb + ti;                            /* above every core element (and any NaN among them) */
    for (size_t i = 0; i < ns; i++) if (sg[i].dst < sg[i].src) memmove(a + sg[i].dst, a + sg[i].src, sg[i].len * sizeof(double));
    for (size_t i = ns; i-- > 0;) if (sg[i].dst > sg[i].src) memmove(a + sg[i].dst, a + sg[i].src, sg[i].len * sizeof(double));
    for (size_t i = 0; i < S; i++) a[tpos[i]] = T[i];
}
static void *reach_worker(void *p) {
    reach_arg_t *w = p; reach_job_t *j = w->j;
    reach_blocks(w);
    if (j->bar) { barrier_wait(j->bar); barrier_wait(j->bar); }       /* all blocks sorted; then the caller has planned */
    if (j->go) reach_seams(w);
    return NULL;
}
/* returns 1 if a[] is sorted; 0 if it declined, and then a[] is a permutation of what it was (possibly with blocks sorted) */
static int reach_try_f64(double *a, size_t n, int P, size_t reach) {
    if (!reach || ns_depth > 0 || (n < REACH_MIN_N && !g_reach_relax) || getenv("LOGOS_NO_REACH")) return 0;
    g_reach_last = LOGOS_REACH_DECLINED_GATE;
    size_t B = REACH_BLOCK; if (4 * reach > B) B = (4 * reach + 4095) & ~(size_t)4095;
    if (B > REACH_BLOCK_MAX || n / B < (g_reach_relax ? 2u : (unsigned)REACH_MIN_BLOCKS)) { TRACE("reach: %zu needs blocks of %zu -> decline", reach, B); return 0; }
    { size_t desc = 0, tot = 0, kinkish = 0;                           /* dense or sparse? four windows, the near-sorted pre-gate's */
      for (int w = 0; w < 4; w++) { size_t lo = (n / 5) * (size_t)(w + 1) - 2048, hi = lo + 4096;
        for (size_t i = lo; i < hi; i++) { int d = a[i] > a[i + 1]; desc += (size_t)d; tot++;
          /* the detector's kink test (its j is this i + 1): the inverted pair, swapped back, fits between its neighbours */
          kinkish += (size_t)(d && i >= 1 && i + 2 < n && a[i + 1] >= a[i - 1] && a[i] <= a[i + 2]); } }
      double rdense = REACH_DENSE; { const char *e = getenv("LOGOS_REACH_DENSE"); if (e) rdense = atof(e); }   /* the ladder's switch */
      if ((double)desc < rdense * (double)tot) {
          size_t kmin = REACH_KINK_MIN; { const char *e = getenv("LOGOS_REACH_KINK_MIN"); if (e && atol(e) > 0) kmin = (size_t)atol(e); }   /* the A/B, sparse branch only */
          if (desc >= kmin && (double)kinkish >= REACH_KINK_FRAC * (double)desc && !getenv("LOGOS_NO_KINKGATE")) {
              TRACE("reach: %zu descents in %zu, %zu kink-shaped -> sparse KINKS, decline for the detector", desc, tot, kinkish);
              g_reach_last = LOGOS_REACH_DECLINED_KINKS; return 0; }
          TRACE("reach: %zu descents in %zu (%zu kink-shaped) -> sparse, decline", desc, tot, kinkish); return 0; } }
    int K = (int)(n / B); if (P > K) P = K; if (P < 1) P = 1;
    size_t maxlen = n - (size_t)(K - 1) * B;                           /* the last block takes the remainder, under 2B */
    reach_job_t j; memset(&j, 0, sizeof j); j.a = a; j.n = n; j.B = B; j.K = K;
    { const char *lm = getenv("LOGOS_REACH_LEAF"); j.leafmode = lm ? atoi(lm) : 2; }   /* the A/B: 0 = every block through the general leaf, 1 = the AVX2 quicksort in a sequential sort */
    atomic_init(&j.nblk, 0); atomic_init(&j.nseam, 0); atomic_init(&j.nstrayblk, 0);
    j.fence = malloc((size_t)K * sizeof(double));
    if (!j.fence) { g_reach_last = LOGOS_REACH_DECLINED_NOMEM; return 0; }
    /* Strays show before a thread is started or an element moved: eight blocks spread over the array, min and max against
       their fences. Two that break a fence end it, so data with strays everywhere pays for two blocks and six medians
       (~0.05 ms at 8M); clean data reads all eight, which the block sorts were about to load anyway. The first cut found
       strays by sorting: a late decline of ~3 ms at 14 threads. */
    /* How many strays are worth putting back depends on the thread count. The shift pass that returns them is serial, at most
       ~0.7 ns per element; what the route saves is ~7 ns per element at one thread and ~0.4 at fourteen. Up to four threads
       strays are simply put back (n/32 of them at most): 1.5-1.8x at one thread and 1.07-1.56x at four on every stray shape of
       the bench. Wider sorts ask eight probe blocks first and decline when two hold strays. Strays the probe did not see are
       put back as well: a rule that stopped the block phase when strays kept turning up was measured DOMINATED (strays in 28%
       of the blocks at 14 threads: 1.24x against the partition when put back, 0.78x when the phase stopped itself). */
    j.tolerant = P <= REACH_TOLERANT_P;
    int probe_bad = 0;
    for (int q = 0; q < REACH_PROBES && probe_bad < 2 && !j.tolerant; q++) { int k = (int)(((size_t)q * 2 + 1) * (size_t)K / (2 * REACH_PROBES));
        for (int d = -1; d <= 1; d += 2) if (k + d >= 0 && k + d < K) j.fence[k + d] = reach_est_median(a + reach_s(&j, k + d), reach_e(&j, k + d) - reach_s(&j, k + d));
        probe_bad += reach_breaks_fence(&j, k); }
    if (probe_bad >= 2) { TRACE("reach: reach %zu, but probe blocks hold strays -> decline", reach); free(j.fence); g_reach_last = LOGOS_REACH_DECLINED_PROBE; return 0; }
    for (int k = 0; k < K; k++) j.fence[k] = reach_est_median(a + reach_s(&j, k), reach_e(&j, k) - reach_s(&j, k));
#ifndef LOGOS_NO_SEAM
    /* THE SEAM (2026-09-21): a noisy ring buffer read from the wrong slot passes the ladder (one pair per rung straddles the seam),
       and the first cut sorted all 256 blocks before its plan saw that the medians do not ascend (100 ms at one thread, then the
       knot route). The fences are those medians, read before anything is sorted: ascending everywhere but at ONE block, where they
       drop by most of the range, means a rotation. The seam is the largest step down inside those two blocks; the dispatcher
       rotates once and enters again. Sequential sorts only (the pass eats the gain at 14 threads). LOGOS_NO_SEAM=1: off. */
    if (ns_depth == 0 && !g_seamed && !g_reach_relax && K >= 4 && !getenv("LOGOS_NO_SEAM")) {
        int drops = 0, kd = -1; double fmin = j.fence[0], fmax = j.fence[0];
        for (int k = 0; k < K; k++) { if (j.fence[k] < fmin) fmin = j.fence[k]; if (j.fence[k] > fmax) fmax = j.fence[k]; if (k + 1 < K && j.fence[k + 1] < j.fence[k]) { drops++; kd = k; } }
        if (drops == 1 && kd >= 1 && kd + 2 < K && j.fence[kd] - j.fence[kd + 1] >= 0.5 * (fmax - fmin)) {
            size_t lo = reach_s(&j, kd), hi = reach_e(&j, kd + 1) - 1, best = lo; double bd = -INFINITY;
            for (size_t q = lo; q < hi; q++) { double d = a[q] - a[q + 1]; if (d > bd) { bd = d; best = q; } }
            /* at P threads the rotation is one thread's pass and a parallel partition is cheap: decline BEFORE the block phase instead,
               which the route used to run in full and then throw away (0.75 at 14 threads on a noisy ring buffer, tsbench) */
            if (bd > 0) { if (P == 1) g_reach_seam = best + 1; TRACE("reach: the fences drop once, at block %d: a rotation, seam %zu (%s)", kd, best + 1, P == 1 ? "rotate" : "decline"); free(j.fence); g_reach_last = LOGOS_REACH_DECLINED_GATE; return 0; }
        }
    }
#endif
    size_t *meta = malloc(4 * ((size_t)K + 2) * sizeof(size_t)); reach_arg_t *wa = calloc((size_t)P, sizeof *wa);
    ctx_t *ctxs = calloc((size_t)P, sizeof *ctxs); pthread_t *th = malloc((size_t)P * sizeof *th);
    void *pb = NULL; uint8_t *pbi = NULL; int pooled = 0; double *scr = NULL;
    if (meta && wa && ctxs && th) { pooled = pool_acquire((size_t)P * maxlen * sizeof(double), 0, &pb, &pbi); scr = pooled ? (double *)pb : malloc((size_t)P * maxlen * sizeof(double)); }
#ifndef LOGOS_NO_CTX_POOL
    int ctx_mark = g_ctx_next;
#endif
    int nctx = 0, sorted = 0;
    g_reach_last = LOGOS_REACH_DECLINED_NOMEM;
    if (scr) for (; nctx < P; nctx++) if (!ctx_init(&ctxs[nctx], maxlen, 0)) break;
    if (scr && nctx == P) {
        j.lo = meta; j.hi = meta + (K + 2); j.x = meta + 2 * (K + 2); j.y = meta + 3 * (K + 2);
        barrier_t bar; if (P > 1) { barrier_init(&bar, P); j.bar = &bar; }
        for (int t = 0; t < P; t++) { wa[t].j = &j; wa[t].ctx = &ctxs[t]; wa[t].scratch = scr + (size_t)t * maxlen; wa[t].tid = t; ctxs[t].seq = (P == 1); PROF(ctxs[t].tid = t;) }
        TRACE("reach: reach %zu -> %d blocks of %zu, %d threads", reach, K, B, P);
        /* queue-driven phases that meet at a barrier: a thread that cannot be created is dropped from it, the rest drain the queues */
        int made = par_spawn(th, P, reach_worker, wa, sizeof wa[0], j.bar, NULL);
        reach_blocks(&wa[0]);
        if (j.bar) barrier_wait(j.bar);
        TRACE("reach: %d blocks sorted, %zu held strays", K, (size_t)atomic_load(&j.nstrayblk));
        reach_plan(&j);
        g_reach_last = j.go ? (j.go == 2 ? LOGOS_REACH_SORTED_STRAYS : LOGOS_REACH_SORTED) : LOGOS_REACH_DECLINED_PLAN;
        reach_seg_t *sg = NULL; size_t *tpos = NULL;
        if (j.go == 2) {                                               /* set the strays aside and sort them; everything their return needs is taken NOW */
            j.T = malloc(j.S * sizeof(double)); tpos = malloc(j.S * sizeof(size_t)); sg = malloc(((size_t)K + j.S + 1) * sizeof *sg);
            if (!j.T || !tpos || !sg) { TRACE("reach: no memory for %zu strays -> decline", j.S); j.go = 0; g_reach_last = LOGOS_REACH_DECLINED_NOMEM; }
            else {
                size_t w = 0;
                for (int k = 0; k < K; k++) { size_t s = reach_s(&j, k), e = reach_e(&j, k);
                    memcpy(j.T + w, a + s, (j.lo[k] - s) * sizeof(double)); w += j.lo[k] - s; memcpy(j.T + w, a + j.hi[k], (e - j.hi[k]) * sizeof(double)); w += e - j.hi[k]; }
                ns_depth++; logos_sort_f64(j.T, j.S, j.S >= NS_STRAYS_PARALLEL ? P : 1); ns_depth--;
                TRACE("reach: %zu strays set aside and sorted", j.S);
            }
        }
        if (j.bar) barrier_wait(j.bar);                                 /* release the workers into the seams, or home */
        if (j.go) reach_seams(&wa[0]);
        par_join(th, made);
        if (j.bar) barrier_destroy(&bar);
        if (j.go == 2) reach_place_strays(&j, sg, tpos);
        sorted = j.go != 0;
        TRACE("reach: %s (%zu strays)", sorted ? "done" : "declined after the block phase", j.S);
        free(j.T); free(tpos); free(sg);
    }
    for (int t = 0; t < nctx; t++) ctx_free(&ctxs[t]);
#ifndef LOGOS_NO_CTX_POOL
    g_ctx_next = ctx_mark;                                             /* the bundles are free again: whoever sorts next starts where this route started */
#endif
    if (pooled) pool_release(); else free(scr);
    free(meta); free(wa); free(ctxs); free(th); free(j.fence);
    return sorted;
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
typedef struct { size_t m, mfull, distinct, f1, f2, reach; double chao1, inv_rate, inv_all, ks; int has_nan; double *sorted; } sodo_t;
/* stage 1: the first SODO_GATE_M points of the Weyl sequence (a prefix of a low-discrepancy sequence is
   itself equidistributed) answer every gate; the cheapest routes never pay for the partition's sample */
#define SODO_GATE_M 2048
static int sodo_sample(const double *a, size_t n, size_t mfull, sodo_t *S) {
    memset(S, 0, sizeof *S); if (mfull > n) mfull = n; size_t m = mfull < SODO_GATE_M ? mfull : SODO_GATE_M; S->m = m; S->mfull = mfull;
    double *raw = malloc(m * sizeof(double)); S->sorted = malloc(mfull * sizeof(double)); if (!raw || !S->sorted) { free(raw); free(S->sorted); S->sorted = NULL; return 0; }
    sample_read(a, n, 0, m, mfull, raw, &S->has_nan);
    /* inversion rate over Fibonacci-lag pairs, wrapped pairs skipped */
    /* inv_all reads EVERY pair of the lag, in position order. The later sample index is the EARLIER position here
     * (frac(144 * 0.618...) = 0.99689, a step of -0.31% of n), so the `p1 <= p0` test above it, meant to drop the few pairs
     * that wrap around the array, drops all but those: inv_rate has been answered by 7 pairs, a whole array apart, where the
     * note above promises ~1900 (found 2026-09-20, when the short-reach gate wanted the same statistic at other lags and the
     * counts came back 7 and 3). inv_rate is left as it was until its own A/B; the short-reach gate reads inv_all. */
    { const size_t L = 144; size_t inv = 0, comps = 0, inva = 0, compsa = 0;
      for (size_t i = 0; i + L < m; i++) { size_t p0 = SAMPLE_POS(i, mfull, n), p1 = SAMPLE_POS(i + L, mfull, n); if (p1 == p0) continue;
          compsa++; if (p1 > p0 ? raw[i] > raw[i + L] : raw[i + L] > raw[i]) inva++;
          if (p1 > p0) { comps++; if (raw[i] > raw[i + L]) inv++; } }
      S->inv_rate = comps ? (double)inv / (double)comps : 1.0; S->inv_all = compsa ? (double)inva / (double)compsa : 1.0; }
#ifndef LOGOS_NO_REACH
    if (n >= REACH_MIN_N && !S->has_nan && S->inv_all <= REACH_TAU) S->reach = reach_from_sample(a, n, raw, m, 0);   /* random data stops at the first rung, already read */
#endif
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
static size_t g_dense_count = 0;       /* logos_twist_last(6): arrays the dense-integer bitmap placed; sequential sorts, calling thread */
#ifndef LOGOS_NO_DENSEINT
#ifndef DENSE_SPAN_MULT
/* 16, from a ladder (2026-09-21, ./mcbench 40 8388608 span): n distinct integers over s x n at ONE thread, the bitmap against the leaf
   (an int32 radix there): 5.0x at 6n, 4.3x at 8n, 3.9x at 12n, 3.3x at 16n, 2.9x at 24n, 1.2x at 32n, a tie near 96n. What the gate
   must give back costs more as the map grows, because every fresh page is a fault: random integers with repeats 0.99 at 16n and 0.73
   at 64n, one planted repeat 0.89 and 0.73. The budget is the largest where the clean spreads win 1.5x and no guard drops under
   0.85. It was 4, from a guess about the cache: the map fits nowhere near it and still wins, because the leaf it replaces is slow. */
#define DENSE_SPAN_MULT 16
#endif
#ifndef DENSE_MIN_N
#define DENSE_MIN_N 65536
#endif
#define DENSE_INT_MAX 4503599627370496.0   /* 2^52: above this a double cannot be an exact odd integer */

/* Does the sample say this is worth a pass? Every sampled value integral, essentially no repeats, and
 * a sampled span within the budget. All three are already computed or one subtraction away. */
static int dense_gate(const double *samp, size_t m, size_t n, double mult) {
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
    return span > 0 && span <= mult * (double)n;
}

static int dense_int_try_f64(double *a, size_t n, const double *samp, size_t m) {
    if (n < DENSE_MIN_N || getenv("LOGOS_NO_DENSE")) return 0;
    double mult = DENSE_SPAN_MULT;                                   /* LOGOS_DENSE_SPAN=<k>: the span budget, k x n (the ladder of 2026-09-21) */
    { const char *e = getenv("LOGOS_DENSE_SPAN"); if (e) { double v = atof(e); if (v >= 1 && v <= 256) mult = v; } }
    if (!samp || !dense_gate(samp, m, n, mult)) return 0;

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
    if ((double)span > (mult + 2.0) * (double)n) return 0;

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
    g_dense_count += out == n;
    return out == n;
}
#endif

/* ===================== runs of equal values: sort the runs, not the elements =====================
 * Re-sorting a table by a column that is constant inside the groups of its previous order, a sensor that holds its reading
 * for a while, run-length data expanded: the array is RUNS of equal values, the runs in no particular order. The counting
 * routes ignore order and stop near 3300 distinct values; past that nothing used the runs, and orderprobe (2026-10-20) found
 * runs of 16 sorting 1.20x SLOWER than the same values shuffled at one thread (the AVX2 quicksort pays for the order).
 * Here: one pass collapses each run to (total-order key, length), the pairs are sorted by an MSD byte radix that skips
 * constant bytes (the string engine's str_msd, carrying a length instead of a pointer), and one pass writes each run back.
 * Equal means equal BITS, so -0.0 and +0.0 stay distinct runs, a NaN (found or not by the sample) keeps its payload and lands
 * where its key puts it, and the output is a permutation of the input's bit patterns either way.
 * CEILING first (make rleprobe, told the runs, one thread, paired against logos_sort_f64): runs of 4 1.79x, 8 2.73x, 16 3.72x,
 * 64 6.65x; 3 1.04, 2 0.74, no runs 0.40. So the gate asks for a mean run of at least four: in eight windows of 2048 adjacent
 * pairs, RLE_EQ_MIN of the pairs must be bit-equal. Sequential sorts only (the passes are serial; a parallel sort must not be
 * offered a sequential route, the dense bitmap's lesson). LOGOS_NO_RLE=1: off. logos_twist_last(28): sorts it finished. */
#ifndef LOGOS_NO_RLE
#ifndef RLE_MIN_N
#define RLE_MIN_N 65536
#endif
#ifndef RLE_EQ_MIN
#define RLE_EQ_MIN 0.75
#endif
static atomic_size_t g_rle_sorts;
static inline uint64_t rle_key(double d) { uint64_t u; memcpy(&u, &d, 8); return (u >> 63) ? ~u : (u | 0x8000000000000000ULL); }
static inline double rle_unkey(uint64_t k) { uint64_t u = (k >> 63) ? (k & 0x7FFFFFFFFFFFFFFFULL) : ~k; double d; memcpy(&d, &u, 8); return d; }
/* (key, length) pairs in (k, v); the result lands in (k, v) when out2 == 0 and in (k2, v2) when out2 == 1. Depth <= 8 (one per byte). */
static void rle_msd(uint64_t *k, uint32_t *v, uint64_t *k2, uint32_t *v2, size_t n, int out2) {
    if (n <= 48) {
        for (size_t i = 1; i < n; i++) { uint64_t x = k[i]; uint32_t p = v[i]; size_t j = i; while (j > 0 && k[j - 1] > x) { k[j] = k[j - 1]; v[j] = v[j - 1]; j--; } k[j] = x; v[j] = p; }
        if (out2) { memcpy(k2, k, n * sizeof *k); memcpy(v2, v, n * sizeof *v); }
        return;
    }
    uint64_t o = 0, an = ~0ULL;
    for (size_t i = 0; i < n; i++) { o |= k[i]; an &= k[i]; }
    uint64_t diff = o ^ an;
    if (!diff) { if (out2) { memcpy(k2, k, n * sizeof *k); memcpy(v2, v, n * sizeof *v); } return; }
    int sh = (63 - __builtin_clzll(diff)) & ~7;
    size_t c[257]; memset(c, 0, sizeof c);
    for (size_t i = 0; i < n; i++) c[((k[i] >> sh) & 0xff) + 1]++;
    for (int b = 0; b < 256; b++) c[b + 1] += c[b];
    size_t st[257]; memcpy(st, c, sizeof st);
    for (size_t i = 0; i < n; i++) { size_t pos = c[(k[i] >> sh) & 0xff]++; k2[pos] = k[i]; v2[pos] = v[i]; }
    for (int b = 0; b < 256; b++) { size_t s0 = st[b], m = st[b + 1] - s0; if (m) rle_msd(k2 + s0, v2 + s0, k + s0, v + s0, m, !out2); }
}
static __attribute__((noinline)) int rle_try_f64(double *a, size_t n) {
    if (n < RLE_MIN_N || n > UINT32_MAX || getenv("LOGOS_NO_RLE")) return 0;
    size_t eq = 0, tot = 0;
    for (int w = 0; w < 8; w++) {
        size_t lo = (n / 9) * (size_t)(w + 1) - 1024;
        for (size_t i = lo; i < lo + 2048; i++) { uint64_t x, y; memcpy(&x, a + i, 8); memcpy(&y, a + i + 1, 8); eq += x == y; tot++; }
    }
    if ((double)eq < RLE_EQ_MIN * (double)tot) return 0;
    /* Run starts first, BRANCH-FREE: every position is stored into the next slot and the slot advances only where the bits change.
       A branch per element (the first cut) mispredicted once per run on geometric run lengths -- the same mean run as fixed runs of
       six read 1.02 against 2.0 -- because fixed lengths make the branch periodic and real lengths do not. */
    size_t cap = n / 3;                                               /* the gate asked for a mean run of four; more runs than n/3: decline */
    uint32_t *st = malloc((cap + 1) * sizeof *st), *v2 = malloc(cap * sizeof *v2);
    uint64_t *k = malloc(cap * sizeof *k), *k2 = malloc(cap * sizeof *k2);
    int ok = st && v2 && k && k2;
    size_t m = 0;
    if (ok) {
        uint64_t prev; memcpy(&prev, a, 8); st[0] = 0; m = 1;
        for (size_t i = 1; i < n; i++) {
            uint64_t x; memcpy(&x, a + i, 8);
            st[m] = (uint32_t)i; m += (size_t)(x != prev); prev = x;
            if (m >= cap) { ok = 0; break; }
        }
    }
    if (ok) {
        st[m] = (uint32_t)n;
        uint32_t *v = st;                                             /* lengths overwrite the starts in place: st[r + 1] is read first */
        for (size_t r = 0; r < m; r++) { k[r] = rle_key(a[st[r]]); v[r] = st[r + 1] - st[r]; }
        int asc = 1; for (size_t i = 1; i < m && asc; i++) asc = k[i - 1] <= k[i];   /* runs already in order: nothing to sort */
        if (!asc) rle_msd(k, v, k2, v2, m, 0);
        /* Write-back in blocks of eight: a run of up to eight is one predictable iteration, and its overshoot is overwritten by the
           next run. Near the end of the array, where an overshoot would leave it, element by element. */
        size_t w = 0;
        for (size_t r = 0; r < m; r++) {
            double x = rle_unkey(k[r]); size_t len = v[r];
            if (w + len + 8 <= n) { for (size_t q = 0; q < len; q += 8) { double *d = a + w + q; d[0] = x; d[1] = x; d[2] = x; d[3] = x; d[4] = x; d[5] = x; d[6] = x; d[7] = x; } }
            else for (size_t q = 0; q < len; q++) a[w + q] = x;
            w += len;
        }
        atomic_fetch_add_explicit(&g_rle_sorts, 1, memory_order_relaxed);
        TRACE("rle: %zu runs (mean %.1f), %s", m, (double)n / (double)m, asc ? "already in order" : "sorted");
    } else TRACE("rle: gate passed (%zu of %zu pairs equal) but %s -> decline", eq, tot, st && v2 && k && k2 ? "too many runs" : "no memory");
    free(st); free(v2); free(k); free(k2);
    return ok;
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
static int g_tail_kind = 0;      /* logos_twist_last(-11): the last sequential tail merge's form, 1 loop, 2 gallop, 3 vector */
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
        par_run(th, P, tail_scan, sc, sizeof sc[0], 1);
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
        par_run(th, P, tail_merge_seg, sg, sizeof sg[0], 1);
    }
    if (pooled) pool_release(); else free(buf);
    free(sc); free(sg); free(th);
    return ok;
}
/* THE SEQUENTIAL MERGE, three forms by the gap g = d / tl (prefix elements per tail element), set from a ladder (make tailladder,
 * 2026-10-20: a sorted prefix of random values and a random tail spanning it, one thread, 30 pairs, against the old loop):
 *   g <= TAIL_VEC_RATIO   both pieces are sorted where they lie (the tail in place), and the knot route's TWO-CHAIN vector merge writes
 *                         them into a pooled buffer that is copied back. AVX2 only (g_simd_merge); without it the old loop.
 *   g >= TAIL_GALLOP_RATIO each tail element, from the top, finds how many prefix elements are > it (exponential search down from i,
 *                         binary search inside the last step) and they shift right in one memmove: 1.10 at g = 332, 1.17-1.33 at
 *                         g >= 1000, a tie at 99, a LOSS below (0.80-0.91 at g = 9-49: a memmove per ~19 elements costs more
 *                         than it saves). The vector form: 1.46 at g = 1 and 1.25 at g = 3, 0.95 at 9, 0.52-0.78 beyond (it
 *                         moves the whole array twice). Shipped edges, same ladder: 1.41 / 1.26 at the top, 1.21-1.25 at the
 *                         bottom, 0.96-1.06 between.
 *   otherwise             the old backward loop, one branch per element. Its branch is predictable whenever the tail is small, and
 *                         it then runs ahead speculatively: a BRANCH-FREE version of it (one compare, two selects) measured 0.34-0.66
 *                         from 10% tails down and only 1.12 at a half, because a branch-free merge is a dependency chain (each load
 *                         waits on the last compare) -- the ledger's single-chain vector merge, in scalar. Removed.
 * LOGOS_TAIL_BRANCHY=1: the old loop at every gap (the A/B); LOGOS_TAIL_GALLOP=<g>, LOGOS_TAIL_VEC=<g> move the two edges (the
 * ladder). All read per call of this route only. logos_twist_last(-11): the last sequential merge's form (1 loop, 2 gallop, 3 vector). */
#ifndef TAIL_GALLOP_RATIO
#define TAIL_GALLOP_RATIO 256
#endif
#ifndef TAIL_VEC_RATIO
#define TAIL_VEC_RATIO 3
#endif
static int tail_merge_try_f64(double *a, size_t n, size_t d, int P) {
    if (getenv("LOGOS_NO_TAIL")) return 0;
    if (P > 1 && n >= MIN_PARALLEL) return tail_merge_par_f64(a, n, d, P);
    int bad = 0;
    for (size_t i = 0; i < n; i++) bad |= (a[i] != a[i]);
    if (bad) return 0;
    int branchy = getenv("LOGOS_TAIL_BRANCHY") != NULL;
    double ggal = TAIL_GALLOP_RATIO, gvec = TAIL_VEC_RATIO;
    { const char *e = getenv("LOGOS_TAIL_GALLOP"); if (e) ggal = atof(e); e = getenv("LOGOS_TAIL_VEC"); if (e) gvec = atof(e); }
    (void)gvec;                                                 /* read only where the vector kernel is compiled */
    size_t tl = n - d;
    psort_f64(a + d, tl, P, NULL, NULL, NULL, NULL);            /* the tail may be anything: full dispatcher, in place */
#ifdef LOGOS_SIMD_MERGE
    if (!branchy && g_simd_merge && (double)d <= gvec * (double)tl) {
        void *pb = NULL; uint8_t *pbi = NULL; int pooled = pool_acquire(n * sizeof(double), 0, &pb, &pbi);
        double *o = pooled ? (double *)pb : (double *)malloc(n * sizeof(double));
        if (o) {
            if (!(half_on(n) && half_two(a, n, d, o))) {              /* half the scratch (half_place); the full one below HALF_MIN_N */
                atomic_fetch_add_explicit(&g_simd_merges, 1, memory_order_relaxed);   /* counted with the knot route's: the same kernel */
                knot_merge2v_uu(a, d, a + d, tl, o);
                memcpy(a, o, n * sizeof(double));
            }
            if (pooled) pool_release(); else free(o);
            g_tail_kind = 3;
            return 1;
        }
    }
#endif
    double *buf = (double *)malloc(tl * sizeof(double));
    if (!buf) return 0;                                         /* the tail is sorted, the prefix untouched: still a permutation */
    memcpy(buf, a + d, tl * sizeof(double));
    /* Backward merge. w == i + j throughout, so a write at w-1 never lands on an unread prefix element. */
    size_t i = d, j = tl, w = n;
    if (!branchy && (double)d >= ggal * (double)tl) {
        while (j > 0 && i > 0) {
            double y = buf[j - 1];
            if (!(a[i - 1] > y)) { a[--w] = y; j--; continue; }
            size_t hi = i - 1, step = 1, lo;                          /* a[hi] > y; find the first index p with a[p] > y */
            for (;;) { if (step > hi) { lo = 0; break; } if (!(a[hi - step] > y)) { lo = hi - step + 1; break; } hi -= step; step <<= 1; }
            while (lo < hi) { size_t mid = lo + ((hi - lo) >> 1); if (a[mid] > y) hi = mid; else lo = mid + 1; }
            size_t p = lo;                                            /* a[p .. i) > y, a[p - 1] <= y (or p == 0); ties stay prefix-first */
            memmove(a + p + j, a + p, (i - p) * sizeof(double));
            i = p; w = i + j; a[--w] = y; j--;
        }
        if (j > 0) memcpy(a, buf, j * sizeof(double));
        g_tail_kind = 2;
    } else {
        while (j > 0) {
            if (i > 0 && a[i - 1] > buf[j - 1]) a[--w] = a[--i];
            else                                a[--w] = buf[--j];
        }
        g_tail_kind = 1;
    }
    free(buf);
    return 1;
}
/* ===================== an unsorted head before a sorted suffix: the tail route's mirror =====================
 * symprobe (2026-10-20) maps each array to its order-isomorph y[i] = -x[n-1-i] -- the same permutation pattern, the distribution
 * mirrored through zero -- and asks that the two cost the same. A 95% sorted prefix with a random tail read 5.02x SLOWER that way
 * round (4.29-6.97 over 20 pairs): its isomorph is a random HEAD before a sorted SUFFIX (a batch prepended to a sorted array), and
 * only the tail form had a route; the prescan stopped at the head's first descent and the leaf sorted all of it. The gate is as
 * free as the tail's: walk the ascending suffix back from the end, which stops within a couple of elements on random data. At
 * least half the array sorted at the end: sort the head where it lies (the full dispatcher), then merge FORWARD, by the tail's
 * measured forms -- the two-chain vector merge into a pooled buffer when the head is a quarter or more, a gallop when the suffix
 * is TAIL_GALLOP_RATIO times the head or more, the plain loop between. Sequential sorts, like the tail's merge; NaN declines (the
 * scan, then a[] untouched). LOGOS_NO_HEAD=1: off. logos_twist_last(-12): the last head merge's form (1 loop, 2 gallop, 3 vector). */
static int g_head_kind = 0;
static __attribute__((noinline)) int head_merge_try_f64(double *a, size_t n, int P) {
    size_t s = n - 1;
    while (s > 0 && a[s - 1] <= a[s]) s--;                          /* [s, n) ascending; random data stops at once */
    if (n - s < n / 2 || s == 0 || getenv("LOGOS_NO_HEAD")) return 0;
    int bad = 0; for (size_t i = 0; i < n; i++) bad |= (a[i] != a[i]);
    if (bad) return 0;
    size_t hs = s, sl = n - s;
    double gvec = TAIL_VEC_RATIO, ggal = TAIL_GALLOP_RATIO;
    { const char *e = getenv("LOGOS_TAIL_GALLOP"); if (e) ggal = atof(e); e = getenv("LOGOS_TAIL_VEC"); if (e) gvec = atof(e); }
    (void)gvec;                                                 /* read only where the vector kernel is compiled */
    psort_f64(a, hs, P, NULL, NULL, NULL, NULL);                  /* the head may be anything: full dispatcher, in place */
#ifdef LOGOS_SIMD_MERGE
    if (g_simd_merge && (double)sl <= gvec * (double)hs) {
        void *pb = NULL; uint8_t *pbi = NULL; int pooled = pool_acquire(n * sizeof(double), 0, &pb, &pbi);
        double *o = pooled ? (double *)pb : (double *)malloc(n * sizeof(double));
        if (o) {
            if (!(half_on(n) && half_two(a, n, hs, o))) {             /* half the scratch, as the tail route */
                atomic_fetch_add_explicit(&g_simd_merges, 1, memory_order_relaxed);
                knot_merge2v_uu(a, hs, a + hs, sl, o);
                memcpy(a, o, n * sizeof(double));
            }
            if (pooled) pool_release(); else free(o);
            g_head_kind = 3;
            return 1;
        }
    }
#endif
    double *buf = (double *)malloc(hs * sizeof(double));
    if (!buf) return 0;                                         /* the head is sorted, the suffix untouched: still a permutation */
    memcpy(buf, a, hs * sizeof(double));
    /* Forward merge. w == i + (j - hs) throughout, so a write at w never lands on an unread suffix element. */
    size_t i = 0, j = hs, w = 0;
    if ((double)sl >= ggal * (double)hs) {
        while (i < hs && j < n) {
            double y = buf[i];
            if (!(a[j] < y)) { a[w++] = y; i++; continue; }
            size_t lo = j + 1, step = 1, hi;                           /* a[j] < y; find the first index p >= j with a[p] >= y */
            for (;;) { if (lo + step > n) { hi = n; break; } if (!(a[lo + step - 1] < y)) { hi = lo + step - 1; break; } lo += step; step <<= 1; }
            while (lo < hi) { size_t mid = lo + ((hi - lo) >> 1); if (a[mid] < y) lo = mid + 1; else hi = mid; }
            size_t p = lo;                                            /* a[j .. p) < y <= a[p] */
            memmove(a + w, a + j, (p - j) * sizeof(double));
            w += p - j; j = p; a[w++] = y; i++;
        }
        if (i < hs) memcpy(a + w, buf + i, (hs - i) * sizeof(double));
        g_head_kind = 2;
    } else {
        while (i < hs) {
            if (j < n && a[j] < buf[i]) a[w++] = a[j++];
            else                        a[w++] = buf[i++];
        }
        g_head_kind = 1;
    }
    free(buf);
    return 1;
}
#endif

#if !defined(LOGOS_NO_DOMINANT) && defined(LOGOS_SODO)            /* the gate reads the shared sample: without it the route has no caller */
#define LOGOS_HAVE_DOMINANT 1
/* ===================== one value holding a large share: sort the rest, fill the middle (2026-10-22, the memory round) =====================
 * A column that is mostly zero, a default, a sentinel. At 14 threads the histogram-free partition declines up front when one value
 * fills more than a region (one bucket of n / ~170), the histogram path then puts every copy of that value into ONE bucket, and one
 * thread sorts it: share 0.25 / 0.5 / 0.75 / 0.9 cost 4.47 / 6.45 / 8.76 / 9.65 ns per element against 1.58 for uniform data -- from
 * a half up, slower than ONE thread -- and 0.9 held 11.5 bytes per element where uniform holds 8.8 (rssprobe, domtime, 4M).
 * Here the value is read off the shared sample (the longest run of bit-equal points in the sorted sample); the other elements are
 * counted below it and above it, gathered in parallel into a buffer of their own size, copied straight to the two ends of the array
 * around a fill of the value -- [below] [the value x V] [above] -- and, the buffer freed, the two ends are sorted where they lie by the
 * whole engine. Memory: the LARGER of the buffer (8 (1 - share) bytes per element) and the inner sorts' scratch, never their sum. Bits, not ==, decide what the value is: -0.0 beside +0.0 goes with the rest and lands after the fill, equal under <. A NaN
 * the sample missed goes with the rest too (the engine promises a permutation there, no order). Gates: DOM_SHARE_PAR at P > 1, where
 * the buffer is never larger than the partition it replaces; DOM_SHARE_SEQ at one thread, where the AVX2 kernel already splits off
 * runs of equal values and only a large share leaves it something to gain. LOGOS_NO_DOMINANT=1: off; logos_twist_last(33): sorts done. */
#ifndef DOM_SHARE_PAR
#define DOM_SHARE_PAR 0.2       /* measured: 14 threads, share 0.25 / 0.5 / 0.75 / 0.9: 2.7 / 3.0 / 5.6 / 9.9x; 0.05-0.1 nothing to gain */
#endif
#ifndef DOM_SHARE_SEQ
#define DOM_SHARE_SEQ 0.85      /* measured: one thread, 0.25-0.75 LOSE 1.35-1.44x (the kernel splits off equal runs), 0.9 wins 2.1x */
#endif
#ifndef DOM_MIN_N
#define DOM_MIN_N 65536
#endif
static atomic_size_t g_dom_sorts;
/* The buffer comes from mmap, not malloc: glibc RAISES its mmap threshold to the size of any mmap-backed block it frees, so a
   24 MB buffer freed mid-sort sent the inner sorts' (and the caller's later) blocks to the heap, which keeps freed pages: rssprobe
   read 6.25 bytes per element still resident AFTER logos_release_buffers() at share 0.25, against 0.30 without the route.
   LOGOS_DOM_FAILALLOC=1 fails it, for the harness (the fault probe intercepts malloc only). */
static double *dom_buf_get(size_t bytes) {
    if (getenv("LOGOS_DOM_FAILALLOC")) return NULL;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : (double *)p;
}
typedef struct { double *a; double *dst; size_t lo, hi, ne, lt, off, ob, oa, flo, fhi, n, A; uint64_t vb; double v; } domw_t;
static void *dom_count(void *p) {                                    /* two plain reductions, so that they vectorize: not the value, below it */
    domw_t *w = p; size_t ne = 0, lt = 0; const double *a = w->a; double v = w->v; uint64_t vb = w->vb;
    for (size_t i = w->lo; i < w->hi; i++) { double x = a[i]; uint64_t b; memcpy(&b, &x, 8); ne += b != vb; lt += x < v; }
    w->ne = ne; w->lt = lt; return NULL;
}
static void *dom_gather(void *p) {                                   /* every element that is not the value, in order, branch-free */
    domw_t *w = p; double *d = w->dst + w->off; size_t c = 0;
    for (size_t i = w->lo; i < w->hi; i++) { double x = w->a[i]; uint64_t b; memcpy(&b, &x, 8); d[c] = x; c += b != w->vb; }
    return NULL;
}
static void *dom_place(void *p) {                                    /* this worker's gathered elements to the two ends, then its share of the fill */
    domw_t *w = p; const double *s = w->dst + w->off; double *pb = w->a + w->ob, *pa = w->a + (w->n - w->A) + w->oa; double v = w->v;
    for (size_t i = 0; i < w->ne; i++) { double x = s[i]; if (x < v) *pb++ = x; else *pa++ = x; }
    for (size_t i = w->flo; i < w->fhi; i++) w->a[i] = v;
    return NULL;
}
static __attribute__((noinline)) int dominant_try_f64(double *a, size_t n, int P, const double *samp, size_t m) {
    if (n < DOM_MIN_N || m < 64 || getenv("LOGOS_NO_DOMINANT")) return 0;
    size_t best = 1, bi = 0, run = 1;
    for (size_t i = 1; i < m; i++) {
        uint64_t x, y; memcpy(&x, samp + i, 8); memcpy(&y, samp + i - 1, 8);
        run = x == y ? run + 1 : 1; if (run > best) { best = run; bi = i; }
    }
    double share = (double)best / (double)m, need = P > 1 ? DOM_SHARE_PAR : DOM_SHARE_SEQ;
    { const char *e = getenv(P > 1 ? "LOGOS_DOM_SHARE_PAR" : "LOGOS_DOM_SHARE_SEQ"); if (e) need = atof(e); }   /* the ladder's switch */
    if (share < need || samp[bi] != samp[bi]) return 0;
    double v = samp[bi]; uint64_t vb; memcpy(&vb, &v, 8);
    int T = P > 1 ? P : 1; size_t chunk = (n + (size_t)T - 1) / (size_t)T;
    domw_t *w = calloc((size_t)T, sizeof *w); pthread_t *th = malloc((size_t)T * sizeof *th);
    if (!w || !th) { free(w); free(th); return 0; }
    for (int t = 0; t < T; t++) { w[t].a = a; w[t].vb = vb; w[t].v = v; w[t].n = n; w[t].lo = (size_t)t * chunk < n ? (size_t)t * chunk : n; w[t].hi = w[t].lo + chunk < n ? w[t].lo + chunk : n; }
    par_run(th, T, dom_count, w, sizeof w[0], 1);
    size_t k = 0, B = 0, A = 0;
    size_t slots = 0;                                                 /* one slack slot per worker: the branch-free gather writes every element, the value included, one past what it keeps (without it the last write raced the next worker and ran off the buffer: not a permutation, a heap abort) */
    for (int t = 0; t < T; t++) { w[t].off = slots; w[t].ob = B; w[t].oa = A; k += w[t].ne; B += w[t].lt; A += w[t].ne - w[t].lt; slots += w[t].ne + 1; }
    TRACE("dominant: share %.3f in the sample; %zu below the value, %zu above, %zu copies of it", share, B, A, n - k);
    if (k == 0) { free(w); free(th); atomic_fetch_add_explicit(&g_dom_sorts, 1, memory_order_relaxed); return 1; }   /* every element is the value */
    double *buf = dom_buf_get(slots * sizeof(double));
    if (!buf) { free(w); free(th); return 0; }                       /* nothing has moved: decline */
    size_t V = n - k, fc = (V + (size_t)T - 1) / (size_t)T;
    for (int t = 0; t < T; t++) { w[t].dst = buf; w[t].A = A; w[t].flo = B + ((size_t)t * fc < V ? (size_t)t * fc : V); w[t].fhi = B + ((size_t)t * fc + fc < V ? (size_t)t * fc + fc : V); }
    par_run(th, T, dom_gather, w, sizeof w[0], 1);
    par_run(th, T, dom_place, w, sizeof w[0], 1);                     /* a's own contents are all in buf or are the value: safe to overwrite */
    munmap(buf, slots * sizeof(double)); free(w); free(th);         /* the buffer goes before the two sorts take their own scratch */
    if (B > 1) psort_f64(a, B, P, NULL, NULL, NULL, NULL);            /* each end where it lies, by the whole engine */
    if (A > 1) psort_f64(a + (n - A), A, P, NULL, NULL, NULL, NULL);
    atomic_fetch_add_explicit(&g_dom_sorts, 1, memory_order_relaxed);
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

/* The stream route's gate, as a function so that it can be asked about the MIRROR image too (mirror = 1: do the modes DESCEND
 * in arrival order?). 1024 jittered, strictly increasing positions; more than 2% out of order, or a NaN, says no. */
static __attribute__((noinline)) int streams_arrival_ok(const double *a, size_t n, const double *bnd, int nb, int nm, int mirror) {
    enum { SM = 1024 };
    size_t stride = n / SM;
    double last[STREAMS_MAX_MODES]; for (int q = 0; q < nm; q++) last[q] = mirror ? INFINITY : -INFINITY;
    size_t wrong = 0;
    for (size_t i = 0; i < SM; i++) {
        uint64_t h = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL; h ^= h >> 29;
        size_t p = i * stride + (stride ? (size_t)(h % stride) : 0);
        if (p >= n) p = n - 1;
        double x = a[p];
        if (x != x) return 0;
        int md = streams_mode(x, bnd, nb);
        if (mirror ? x > last[md] : x < last[md]) { if (++wrong > SM / 50) return 0; }   /* more than 2% out of order: stop reading */
        last[md] = x;
    }
    return wrong <= SM / 50;
}
/* noinline: a 13 KB frame; see nearsorted_parallel_f64. */
static __attribute__((noinline)) int streams_try_f64(double *a, size_t n, double *buf, ctx_t *c, const double *bnd, int nm, double lo, double hi) {
    if (nm < 2 || getenv("LOGOS_NO_STREAMS")) return 0;
    int nb = nm - 1;

    /* Gate: do the modes ascend in ARRIVAL order? A jittered, strictly increasing set of positions, so the probe
     * cannot alias with a periodic layout and needs no sort of its own. */
    if (!streams_arrival_ok(a, n, bnd, nb, nm, 0)) return 0;

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
    TRACE("streams: %d modes, arrival probe passed, %d buckets needed a sort, table %d, %zu ambiguous lookups", nm, dirty, use_tab, amb);
    return 1;
}
#endif

/* vstart/vnbk non-NULL: partition only, leaving the buckets unsorted, and report their boundaries.
 * *vnbk is set to 0 whenever the array came out FULLY sorted instead -- every other route in this
 * dispatcher produces sorted output, so 0 means "there is nothing left to do". */
/* ===================== one orientation bit for the WHOLE array =====================
 * The twist carries a bit per run. This is the same bit for the array. Every cheap route reads order in ONE direction, and the
 * prescan's `descending` test is exact, so a newest-first log with one late record fired no gate and paid for a full sort:
 * a shape against its own mirror image cost 2.0x (short reach), 2.7x (sorted prefix plus tail) and 3.1x (interleaved streams)
 * at one thread (make mirrorprobe). The plain answer: reverse once, then let the dispatcher see what it knows how to read.
 * Where and when, both from measurements: BEHIND relocation, because the twist already sorts mirrored blocks as they lie and
 * a pass in front of them costs 10-23%; only when the sample's writhe says mirrored (inv_all >= MIRROR_INV_MIN, which noise
 * at 0.50 never reaches) AND a directional gate fires ON THE MIRROR IMAGE, because a pass that buys nothing costs 5-14%; and
 * on sequential sorts only, because at 14 threads the pass eats the gain (1.41 -> 1.15 on short reach, nothing elsewhere).
 * LOGOS_NO_MIRROR=1 / -DLOGOS_NO_MIRROR: off. logos_mirror_count(): how often it fired. A/B: ./mirrorprobe 40 8388608 ab. */
#ifndef MIRROR_INV_MIN
#define MIRROR_INV_MIN 0.90
#endif
#define MIRROR_MIN_N (1u << 16)
static size_t g_mirror_count = 0;
size_t logos_mirror_count(int reset) { size_t r = g_mirror_count; if (reset) g_mirror_count = 0; return r; }
#if defined(LOGOS_SODO) && !defined(LOGOS_NO_MIRROR)
static int g_mirrored = 0;             /* re-entry guard; written on the calling thread only, one top-level sort at a time */
/* noinline: nothing of this may land in the dispatcher's frame (make callbench). Returns which gate fired, 0 for none. */
static __attribute__((noinline)) int mirror_gate(const double *a, size_t n, double inv_all, const double *st_bnd, int st_nm) {
    if (getenv("LOGOS_NO_MIRROR")) return 0;
    /* (1) the mirror image of a sorted prefix: a DESCENDING SUFFIX of at least half the array. The scan stops at the first
     *     ascent from the end, so it is long only when it is about to pay. */
#ifndef LOGOS_NO_TAILROUTE
    if (inv_all >= MIRROR_INV_MIN && !getenv("LOGOS_NO_TAIL")) { size_t i = n - 1; while (i > 0 && a[i - 1] >= a[i]) i--; if (n - i >= n / 2 && n >= TAIL_MIN_N) return 1; }
#endif
    (void)inv_all; (void)a; (void)n;          /* a build with every directional route compiled out has nothing to ask */
#ifndef LOGOS_NO_REACH
    /* (2) the mirror image of short reach: the same ladder, every comparison turned round */
    if (inv_all >= MIRROR_INV_MIN && n >= REACH_MIN_N && !getenv("LOGOS_NO_REACH") && reach_from_sample(a, n, NULL, 0, 1) > 0) return 2;
#endif
#ifndef LOGOS_NO_STREAMS
    /* (3) the mirror image of interleaved ascending streams: the modes DESCEND in arrival order */
    if (st_nm >= 2 && n >= STREAMS_MIN_N && !getenv("LOGOS_NO_STREAMS") && streams_arrival_ok(a, n, st_bnd, st_nm - 1, st_nm, 1)) return 3;
#else
    (void)st_bnd; (void)st_nm;
#endif
    return 0;
}
#endif

/* THE LOOP-BACK: R0 on the sphere (Kauffman & Lambropoulou, "Hard Unknots and Collapsing Tangles": a knot diagram lives on the
 * sphere, where the outer region is a region like any other, and some diagrams that are hard in the plane are easy there).
 * Close the array into a ring, so that walking forward past its end arrives at its start, and a sorted array read from the
 * wrong place (a ring buffer dumped from physical slot 0, a log that wrapped) is ONE ascending strand across the wrap. Read
 * in the plane it is two runs, and the engine paid for them: the tail route merged the second run in element by element,
 * 2.0x the sort of the same array unrotated at one thread and 2.6x at 14 (make unknotprobe, mode `ring`).
 * The gate is exact and nearly free. The prescan has just walked the ascending prefix [0, d) to its first descent. If the
 * array also ENDS in an ascending run [s, n) whose last element is no larger than a[0], that run continues into the prefix
 * across the wrap: rotate it to the front once and enter the dispatcher again, which then sees one sorted prefix of
 * n - s + d elements (a ring buffer: the whole array, and the prescan returns; a wrapped log with a random stretch: the tail
 * route's shape). On other data the first comparison fails (a[n-1] > a[0] on anything that ascends overall) or the backward
 * walk stops within an element or two. It fires only when the joined strand is at least half the array, and, when the prefix
 * alone already is, only when the suffix it saves sorting is at least n / 16 (a rotation costs about 1.4 passes; sorting
 * that much of a tail costs more). At P threads only when the strand leaves at most n / 16 outside it: the rotation is one
 * thread's pass, and a large remainder handed to the parallel tail route lost to the partition (0.51 at 1M on 30% prefix +
 * noise + 40% suffix). LOGOS_NO_LOOPBACK=1 / -DLOGOS_NO_LOOPBACK: off. logos_twist_last(7): how often it fired. */
#ifndef LOOPBACK_MIN_N
#define LOOPBACK_MIN_N 4096
#endif
static __attribute__((unused)) size_t g_loop_count = 0;   /* logos_twist_last(7); calling thread, one top-level sort at a time (the pool's contract) */
static __attribute__((unused)) int g_looped = 0;          /* re-entry guard */
/* a := a[s..n) ++ a[0..s): the shorter part through scratch (the pool's buffer when it is free), the longer by one memmove;
   three reversals in place when no scratch can be had, so a failed allocation costs time and never the answer */
static void rotate_left_f64(double *a, size_t n, size_t s) {
    size_t k = n - s, m = k < s ? k : s;
    void *pb = NULL; uint8_t *pbi = NULL; int pooled = pool_acquire(m * sizeof(double), 0, &pb, &pbi);
    double *t = pooled ? (double *)pb : (double *)malloc(m * sizeof(double));
    if (t) {
        if (k <= s) { memcpy(t, a + s, k * sizeof(double)); memmove(a + k, a, s * sizeof(double)); memcpy(a, t, k * sizeof(double)); }
        else        { memcpy(t, a, s * sizeof(double)); memmove(a, a + s, k * sizeof(double)); memcpy(a + k, t, s * sizeof(double)); }
        if (pooled) pool_release(); else free(t);
        return;
    }
    for (size_t l = 0, r = s - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }
    for (size_t l = s, r = n - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }
    for (size_t l = 0, r = n - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }
}
#ifndef LOGOS_NO_LOOPBACK
/* noinline: nothing of this may land in the dispatcher's frame (make callbench). d: the prescan's first descent, 0 < d < n.
   Returns the rotation to make (s, the joined strand's start), or 0. */
static __attribute__((noinline)) size_t loopback_gate(const double *a, size_t n, size_t d, int par) {
    if (!(a[n - 1] <= a[0])) return 0;                                  /* the end does not continue into the start (or a NaN is there) */
    size_t s = n - 1; while (s > d && a[s - 1] <= a[s]) s--;            /* [s, n) ascends */
    size_t L = n - s;
    if (L + d < n / 2) return 0;
    if (d >= n / 2 && L < n / 16) return 0;                             /* the tail route has this already; the rotation would save too little */
    if (par && n - (L + d) > n / 16) return 0;                          /* P threads: the rotation is one thread's pass, and a big tail left for the
                                                                           parallel tail route lost to the partition (0.51 on 30% + noise + 40%) */
    if (getenv("LOGOS_NO_LOOPBACK")) return 0;                          /* read only when about to fire */
    return s;
}
#endif

#if !defined(LOGOS_NO_SEAM) || !defined(LOGOS_NO_WAVE) || !defined(LOGOS_NO_CUTS) || !defined(LOGOS_NO_NSSTRAND)   /* the near-sorted strand gate reads sample_by_pos too */
/* ===================== the sample in POSITION order: the seam route and the wave route =====================
 * Every dispatcher gate reads the shared sample sorted by VALUE, which throws the positions away. Put the same 2048 points back in
 * position order and two more shapes become readable at the cost of one small sort (about 0.1 ms):
 *   SEAM   a noisy ring buffer read from the wrong slot: the largest drop between consecutive points marks the seam, and each side
 *          is nearly sorted (few inverted adjacent pairs). The loop-back covers the EXACT case in the prescan; this one needs the
 *          sample because noise stops the prescan within two elements. Rotate once, enter the dispatcher again: the short-reach
 *          route takes it from there (ceiling 1.29, a told rotation 1.17 at one thread: make unknotprobe, mode ring).
 *   WAVE   a seasonal series with drift: monotone at the sample's resolution (a few dozen long pieces of the SMOOTHED sample, each
 *          direction held for many points) but broken by noise at the element's, so the run detector sees runs of two. Each piece
 *          is a strand whose disorder is short reach; the pieces overlap in value (a crest and the trough after it share a range).
 *          So: cut the array at the pieces' extrema, sort each piece where it lies (a descending piece reversed first, then the
 *          short-reach route, relaxed to a few blocks; the full dispatcher where that declines), and place the sorted pieces with
 *          the knot sweep, which copies what lies alone and merges only where pieces cross. The composed route's ceiling was
 *          measured at 2.1-3.7x at one thread (experiments/SHORT_REACH.md). Sequential sorts only.
 * Both are behind relocation, the mirror and the streams; both re-read the sample (no lifetime games with the gates' copy) and only
 * for sequential sorts of a million elements and up whose lag-144 inversion rate is under WAVE_INV_MAX (noise reads 0.50). Switches:
 * LOGOS_NO_SEAM=1, LOGOS_NO_WAVE=1; witnesses logos_twist_last(9) and (8). */
#ifndef WAVE_MIN_N
#define WAVE_MIN_N (1u << 20)
#endif
#ifndef WAVE_INV_MAX
#define WAVE_INV_MAX 0.45
#endif
#ifndef WAVE_COST_MAX
#define WAVE_COST_MAX 1.5
#endif
/* psamp_t and this function's declaration sit above nearsorted_parallel_f64, whose strand gate reads it too */
static psamp_t *sample_by_pos(const double *a, size_t n, size_t m) {
    psamp_t *s = malloc(m * sizeof *s), *t = malloc(m * sizeof *s); size_t *cnt = calloc(m + 1, sizeof(size_t)); double *raw = malloc(m * sizeof(double));
    if (!s || !t || !cnt || !raw) { free(s); free(t); free(cnt); free(raw); return NULL; }
    int nan = 0; sample_read(a, n, 0, m, m, raw, &nan);
    if (nan) { free(s); free(t); free(cnt); free(raw); return NULL; }
    for (size_t i = 0; i < m; i++) { t[i].pos = SAMPLE_POS(i, m, n); t[i].v = raw[i]; size_t key = (size_t)((double)t[i].pos / (double)n * (double)m); if (key >= m) key = m - 1; cnt[key + 1]++; }
    for (size_t i = 0; i < m; i++) cnt[i + 1] += cnt[i];
    for (size_t i = 0; i < m; i++) { size_t key = (size_t)((double)t[i].pos / (double)n * (double)m); if (key >= m) key = m - 1; s[cnt[key]++] = t[i]; }
    for (size_t i = 1; i < m; i++) { psamp_t x = s[i]; size_t j = i; while (j > 0 && s[j - 1].pos > x.pos) { s[j] = s[j - 1]; j--; } s[j] = x; }   /* bucketed by position already: a few swaps */
    free(t); free(cnt); free(raw);
    return s;
}
static size_t g_wave_count = 0;
/* ===================== cuts: data ordered by windows, unordered inside them =====================
 * A table written in time-partitioned batches, a log flushed per minute and shuffled inside the minute: the array is a sequence of
 * WINDOWS, every value of a window above every value of the windows before it, the window itself in no order. A cut is a position
 * with everything before it no larger than everything after it; between two cuts lies a segment that can be sorted where it lies,
 * by itself, at the size of a cache. The short-reach route covers windows up to about 0.3% of n (its ladder's longest rung); above
 * that nothing used the order, and orderprobe (2026-10-20) measured windows of 64k in 4M sorting 1.38x SLOWER than the same values
 * shuffled at one thread: the leaf's probe calls such a range nearly sorted and densely disordered at once, which keeps the AVX2
 * quicksort away, and the quicksort itself pays ~1.1x for ordered input anyway (make ordkern: per level, the library too).
 * The ledger's cut decomposition (2026-09-20) found cuts element by element with a running maximum and a suffix minimum, dependency
 * chains, 35 ms at 8M. Here: (1) the shared sample in POSITION order names candidate cuts for free (its prefix maximum at most its
 * suffix minimum), and fewer than CUTS_MIN_CAND of them declines at no cost -- random data shows none; (2) one pass takes each block
 * of CUTS_BLK elements' minimum and maximum, independent reductions that vectorise, and a cut at a block boundary is exact when the
 * blocks' prefix maximum is at most their suffix minimum; (3) a block that is the ONLY obstacle between its neighbours straddles a
 * window's edge: its own running maximum and minimum find the cut inside it (4096 elements, a few such blocks). Then every segment
 * goes to the sequential leaf where it lies. Sequential sorts; LOGOS_NO_CUTS=1: off; logos_twist_last(29): sorts it finished. */
#if !defined(LOGOS_NO_CUTS) && defined(LOGOS_SODO)   /* its candidates come from the shared sample, so without one it has no caller */
#define LOGOS_HAVE_CUTS 1                 /* the route is compiled: the dispatcher's call and logos_twist_last(29) ask this, not the switch */
#ifndef CUTS_MIN_N
#define CUTS_MIN_N (1u << 20)
#endif
#ifndef CUTS_BLK
#define CUTS_BLK 4096
#endif
#ifndef CUTS_MIN_CAND
#define CUTS_MIN_CAND 8
#endif
static atomic_size_t g_cuts_sorts;
static void cuts_minmax(const double *p, size_t nb, double *mn, double *mx) {   /* four chains each way; a NaN is never selected */
    double l0 = p[0], l1 = l0, l2 = l0, l3 = l0, h0 = l0, h1 = l0, h2 = l0, h3 = l0; size_t i = 0;
    for (; i + 4 <= nb; i += 4) { double v0 = p[i], v1 = p[i + 1], v2 = p[i + 2], v3 = p[i + 3];
        l0 = v0 < l0 ? v0 : l0; l1 = v1 < l1 ? v1 : l1; l2 = v2 < l2 ? v2 : l2; l3 = v3 < l3 ? v3 : l3;
        h0 = v0 > h0 ? v0 : h0; h1 = v1 > h1 ? v1 : h1; h2 = v2 > h2 ? v2 : h2; h3 = v3 > h3 ? v3 : h3; }
    for (; i < nb; i++) { l0 = p[i] < l0 ? p[i] : l0; h0 = p[i] > h0 ? p[i] : h0; }
    l0 = l1 < l0 ? l1 : l0; l2 = l3 < l2 ? l3 : l2; h0 = h1 > h0 ? h1 : h0; h2 = h3 > h2 ? h3 : h2;
    *mn = l2 < l0 ? l2 : l0; *mx = h2 > h0 ? h2 : h0;
}
static __attribute__((noinline)) int cuts_try_f64(double *a, size_t n, double *buf, ctx_t *c, const psamp_t *ps, size_t m) {
    if (n < CUTS_MIN_N || getenv("LOGOS_NO_CUTS")) return 0;
    /* (1) the sample's candidate cuts */
    double *sm = malloc(m * sizeof *sm); if (!sm) return 0;
    sm[m - 1] = ps[m - 1].v; for (size_t i = m - 1; i-- > 0; ) sm[i] = ps[i].v < sm[i + 1] ? ps[i].v : sm[i + 1];
    size_t cand = 0; double pm = ps[0].v;
    for (size_t i = 0; i + 1 < m; i++) { if (ps[i].v > pm) pm = ps[i].v; cand += pm <= sm[i + 1]; }
    free(sm);
    if (cand < CUTS_MIN_CAND) return 0;
    /* Windows of width W leave about n / W candidates. Nearly EVERY sample point a candidate means disorder finer than the sample's
       spacing, which is the short-reach route's data or near-sortedness, and exact cuts at 4096-element blocks then mostly fail:
       two sources interleaved at random (fieldprobe weave, 2026-10-21) showed 1723 candidates of 2048, paid 9.4 ms of block
       extremes (2.2 ns per element) and declined with 44 segments. LOGOS_CUTS_DENSE=1: try anyway, for the A/B. */
    if (cand > m / 2 && !getenv("LOGOS_CUTS_DENSE")) { TRACE("cuts: %zu sample candidates of %zu -> finer than the sample, decline", cand, m); return 0; }
    /* (2) block extremes, and the exact cuts at block boundaries */
    size_t nb = (n + CUTS_BLK - 1) / CUTS_BLK;
    double *bmn = malloc(nb * sizeof *bmn), *bmx = malloc(nb * sizeof *bmx), *smn = malloc((nb + 1) * sizeof *smn);
    size_t *cut = malloc((nb + 2) * sizeof *cut);
    if (!bmn || !bmx || !smn || !cut) { free(bmn); free(bmx); free(smn); free(cut); return 0; }
    for (size_t b = 0; b < nb; b++) { size_t lo = b * CUTS_BLK, hi = lo + CUTS_BLK < n ? lo + CUTS_BLK : n; cuts_minmax(a + lo, hi - lo, &bmn[b], &bmx[b]); }
    smn[nb] = INFINITY; for (size_t b = nb; b-- > 0; ) smn[b] = bmn[b] < smn[b + 1] ? bmn[b] : smn[b + 1];
    size_t nc = 0; cut[nc++] = 0; double pmx = -INFINITY;
    for (size_t b = 0; b < nb; b++) {
        /* block b sits between prefix max pmx (blocks < b) and suffix min smn[b + 1] (blocks > b) */
        if (b > 0 && pmx <= smn[b]) { if (cut[nc - 1] != b * CUTS_BLK) cut[nc++] = b * CUTS_BLK; }
        else if (b > 0 && pmx <= smn[b + 1] && bmx[b] > smn[b + 1] && bmn[b] < pmx) {
            /* (3) the window's edge is INSIDE block b: the first position p in it with max(a[.. p)) <= min(a[p ..)) */
            size_t lo = b * CUTS_BLK, hi = lo + CUTS_BLK < n ? lo + CUTS_BLK : n, len = hi - lo;
            double *rmin = buf;                                       /* the leaf's scratch, free until the segments are sorted */
            double run = smn[b + 1]; for (size_t q = len; q-- > 0; ) { if (a[lo + q] < run) run = a[lo + q]; rmin[q] = run; }
            double rmax = pmx;
            for (size_t q = 1; q < len; q++) { if (a[lo + q - 1] > rmax) rmax = a[lo + q - 1]; if (rmax <= rmin[q]) { if (cut[nc - 1] != lo + q) cut[nc++] = lo + q; break; } }
        }
        if (bmx[b] > pmx) pmx = bmx[b];
    }
    cut[nc++] = n;
    free(bmn); free(bmx); free(smn);
    size_t big = 0; for (size_t k = 0; k + 1 < nc; k++) if (cut[k + 1] - cut[k] > big) big = cut[k + 1] - cut[k];
    if (nc - 1 < CUTS_MIN_CAND || big > n / 4) { TRACE("cuts: %zu sample candidates, %zu segments, the largest %zu -> decline", cand, nc - 1, big); free(cut); return 0; }
    /* (4) every segment where it lies */
    for (size_t k = 0; k + 1 < nc; k++) {
        size_t lo = cut[k], len = cut[k + 1] - lo;
        if (len < 2) continue;
        double *r = leaf_f64(a + lo, len, buf, c);
        if (r != a + lo) memcpy(a + lo, r, len * sizeof(double));
    }
    TRACE("cuts: %zu sample candidates, %zu segments, the largest %zu: sorted where they lie", cand, nc - 1, big);
    free(cut);
    atomic_fetch_add_explicit(&g_cuts_sorts, 1, memory_order_relaxed);
    return 1;
}
#endif
#ifndef LOGOS_NO_SEAM
/* noinline: nothing of this may land in the dispatcher's frame (make callbench) */
static __attribute__((noinline)) size_t seam_gate(const double *a, size_t n, const psamp_t *s, size_t m) {
    if (getenv("LOGOS_NO_SEAM")) return 0;
    (void)n;
    size_t k = 0; double drop = -INFINITY, vmin = s[0].v, vmax = s[0].v;
    for (size_t i = 0; i < m; i++) { if (s[i].v < vmin) vmin = s[i].v; if (s[i].v > vmax) vmax = s[i].v; }
    for (size_t i = 0; i + 1 < m; i++) { double d = s[i].v - s[i + 1].v; if (d > drop) { drop = d; k = i; } }
    if (!(drop >= 0.5 * (vmax - vmin)) || !isfinite(drop)) return 0;
    if (k + 1 < m / 16 || m - (k + 1) < m / 16) return 0;                  /* a rotation by under a sixteenth is not worth a pass */
    size_t inv = 0;                                                        /* adjacent pairs on the same side of the seam */
    for (size_t i = 0; i + 1 < m; i++) if (i != k && s[i].v > s[i + 1].v) inv++;
    if (inv > m / 100) return 0;
    size_t lo = s[k].pos, hi = s[k + 1].pos, best = lo; double bd = -INFINITY;
    for (size_t p = lo; p < hi; p++) { double d = a[p] - a[p + 1]; if (d > bd) { bd = d; best = p; } }
    return bd > 0 ? best + 1 : 0;
}
#endif
#ifndef LOGOS_NO_WAVE
/* pieces of the smoothed sample: sm = a median of five (a mean of sixteen turned the single step of a ROTATION into a descending
   piece, and the wave route then sorted a rotated near-sorted array in three pieces for nothing), dir[i] = sign(sm[i+1] - sm[i]), a
   piece is a stretch of one direction at least WAVE_MIN_PIECE points long (shorter flips are absorbed). Returns the piece count. */
#define WAVE_MIN_PIECE 8
static size_t wave_pieces(const psamp_t *s, size_t m, size_t *starts, int *dirs, size_t maxp) {
    double *sm = malloc(m * sizeof(double)); if (!sm) return 0;
    for (size_t i = 0; i < m; i++) { double w5[5]; size_t lo = i >= 2 ? i - 2 : 0, hi = i + 3 <= m ? i + 3 : m, c = 0;
        for (size_t j = lo; j < hi; j++) w5[c++] = s[j].v;
        for (size_t x = 1; x < c; x++) { double v = w5[x]; size_t y = x; while (y > 0 && w5[y - 1] > v) { w5[y] = w5[y - 1]; y--; } w5[y] = v; }
        sm[i] = w5[c / 2]; }
    size_t np = 0; int cur = 0; size_t run = 0, st = 0;
    for (size_t i = 0; i + 1 < m; i++) {
        int d = sm[i + 1] > sm[i] ? 1 : sm[i + 1] < sm[i] ? -1 : 0; if (!d) d = cur;
        if (d == cur) { run++; continue; }
        if (cur == 0) { cur = d; run = 1; st = 0; continue; }
        /* a change of direction: count how long it holds */
        size_t hold = 0; for (size_t j = i; j + 1 < m && hold < WAVE_MIN_PIECE; j++) { int dj = sm[j + 1] > sm[j] ? 1 : sm[j + 1] < sm[j] ? -1 : 0; if (dj == d || dj == 0) hold++; else break; }
        if (hold < WAVE_MIN_PIECE) continue;                               /* a wobble: the piece goes on */
        if (np == maxp) { free(sm); return 0; }
        starts[np] = st; dirs[np] = cur; np++; st = i; cur = d; run = 1;
    }
    if (np == maxp) { free(sm); return 0; }
    starts[np] = st; dirs[np] = cur; np++; starts[np] = m;
    free(sm); return np;
}
#ifndef LOGOS_NO_REACH
static int reach_try_f64(double *a, size_t n, int P, size_t reach);
#endif
static __attribute__((noinline)) int wave_try_f64(double *a, size_t n, double *buf, const psamp_t *s, size_t m) {
    if (getenv("LOGOS_NO_WAVE")) return 0;
    enum { MAXP = 512 };
    size_t starts[MAXP + 1]; int dirs[MAXP];
    size_t np = wave_pieces(s, m, starts, dirs, MAXP);
    if (np < 3 || np > m / WAVE_MIN_PIECE) { TRACE("wave: %zu pieces -> decline", np); return 0; }
    /* the pieces must be real at the element's scale too: raw adjacent sample pairs inside a piece mostly go the piece's way */
    size_t agree = 0, tot = 0;
    for (size_t p = 0; p < np; p++) for (size_t i = starts[p]; i + 1 < starts[p + 1]; i++) { tot++; agree += dirs[p] > 0 ? s[i + 1].v >= s[i].v : s[i + 1].v <= s[i].v; }
    if (tot == 0 || (double)agree < 0.85 * (double)tot) { TRACE("wave: %zu pieces but only %zu of %zu pairs agree -> decline", np, agree, tot); return 0; }
    /* THE CROSSING COST, from the sample: for every point, how many pieces' [min, max] cover it, priced ceil(log2 k) merge levels. A
       wave whose swing is several periods of drift is covered four and five deep, and the sweep's k-way merges then cost more than
       the leaf (swing 2: 0.76 in two runs, where swing 0.6 won 1.35). */
    { double *pl = malloc(np * sizeof(double)), *ph = malloc(np * sizeof(double)); if (!pl || !ph) { free(pl); free(ph); return 0; }
      for (size_t p = 0; p < np; p++) { pl[p] = INFINITY; ph[p] = -INFINITY; for (size_t i = starts[p]; i < starts[p + 1]; i++) { if (s[i].v < pl[p]) pl[p] = s[i].v; if (s[i].v > ph[p]) ph[p] = s[i].v; } }
      size_t lv = 0;
      for (size_t i = 0; i < m; i += 4) { size_t k = 0; for (size_t p = 0; p < np; p++) k += s[i].v >= pl[p] && s[i].v <= ph[p]; size_t l = 0; while (((size_t)1 << l) < k) l++; lv += l; }
      free(pl); free(ph);
      double cost = (double)lv / (double)((m + 3) / 4);
      if (cost > WAVE_COST_MAX) { TRACE("wave: %zu pieces, crossing cost %.2f levels per element -> decline", np, cost); return 0; }
      TRACE("wave: crossing cost %.2f levels per element", cost); }
    /* boundaries at the element's scale: the extremum between the last point of one piece and the first of the next */
    size_t *B = malloc((np + 1) * sizeof(size_t)); if (!B) return 0;
    B[0] = 0; B[np] = n;
    for (size_t p = 1; p < np; p++) {
        size_t lo = s[starts[p] - 1].pos, hi = s[starts[p]].pos, best = lo; double bv = a[lo];
        if (dirs[p - 1] > 0) { for (size_t q = lo; q <= hi; q++) if (a[q] > bv) { bv = a[q]; best = q; } }
        else                 { for (size_t q = lo; q <= hi; q++) if (a[q] < bv) { bv = a[q]; best = q; } }
        B[p] = best + 1;
        if (B[p] <= B[p - 1]) B[p] = B[p - 1] + 1;
    }
    TRACE("wave: %zu pieces, %zu of %zu pairs agree -> sort each where it lies", np, agree, tot);
    size_t *runs = malloc(2 * np * sizeof(size_t)); if (!runs) { free(B); return 0; }
    for (size_t p = 0; p < np; p++) {
        double *x = a + B[p]; size_t len = B[p + 1] - B[p];
        if (dirs[p] < 0) for (size_t l = 0, r = len - 1; l < r; l++, r--) { double t = x[l]; x[l] = x[r]; x[r] = t; }
#ifndef LOGOS_NO_REACH
        g_reach_relax = 1; int ok = reach_try_f64(x, len, 1, 2048); g_reach_relax = 0;
#else
        int ok = 0;
#endif
        if (!ok) NESTED_SORT(psort_f64(x, len, 1, NULL, NULL, NULL, NULL));
        runs[2 * p] = B[p]; runs[2 * p + 1] = B[p + 1] - 1;
    }
    /* LOGOS_WAVE_NOSWEEP=1 prices the sweep: everything up to here (the sample, the pieces, the gates and the 65 piece
       sorts) and nothing after it. The array comes back as sorted pieces, so the OUTPUT IS WRONG BY DESIGN -- the same
       device as `./unknotprobe nocopy`, and for the same reason: a trace build's timestamps cannot price a phase, and
       this is a normal build. Read per sort (the route runs once per call, at n >= 1M) so one binary can alternate. */
    if (getenv("LOGOS_WAVE_NOSWEEP")) {
        /* Every other switch in this engine returns a CORRECT answer; this one does not, so it says so on stderr the
           first time it fires. Without that it is a footgun: set it in a shell and every later sort of a wave-shaped
           array comes back quietly unsorted, `make test` included. Once per process, not per sort. */
        /* write(2), not fprintf: <stdio.h> is included only under -DLOGOS_TRACE, on purpose -- a sort library should
           not drag stdio into every binary that links it, and the first cut of this warning broke the normal build. */
        static int warned = 0;
        if (!warned) {
            static const char msg[] = "logos: LOGOS_WAVE_NOSWEEP is set -- the wave route returns UNSORTED output on purpose (it prices the sweep). Unset it for any real sort.\n";
            warned = 1; ssize_t wr = write(2, msg, sizeof msg - 1); (void)wr;
        }
        TRACE("wave: sweep SKIPPED (wrong output by design)");
        free(runs); free(B); g_wave_count++; return 1;
    }
    /* the sweep: the pieces are strands now; copy what lies alone, merge where they cross */
    rdesc_t *d; size_t *ch, nc, merged; int placed = 0;
    if (runs_plan(a, runs, np, &d, &ch, &nc, &merged)) {
#ifndef LOGOS_NO_KNOT
        knot_plan_t K;
        if (knot_plan(a, d, np, n + 1, 1, &K)) {
            size_t *krl = K.n3 ? knot_rl(&K) : NULL;
            if (K.total == n && (!K.n3 || krl)) {
                if (!(half_on(n) && knot_place_half(a, buf, n, &K, NULL, 0))) { knot_place_seq(a, buf, &K, krl); memcpy(a, buf, n * sizeof(double)); }
                placed = 1;                                                     /* half the scratch where it can (half_place) */
            }
            free(krl); knot_plan_free(&K);
        }
#endif
        free(d); free(ch);
    }
    if (!placed) { merge_all_runs(a, buf, runs, np); }                      /* the k-way merge: slower, still right */
    TRACE("wave: placed (%s)", placed ? "knot sweep" : "k-way merge");
    free(runs); free(B); g_wave_count++;
    return 1;
}
#endif
#endif

/* THE PRESCAN IN BLOCKS (2026-09-22, found by tsbench's edge round: on a sorted array of 8.4M the engine took 2.2x ONE plain scan,
 * because the whole-array prescan below is the entire sort there and it ran a scalar line with two tests and two live flags per
 * element). The scalar line decides three things: no fall anywhere (ascending), no rise anywhere (descending), and the first fall.
 * Here the first two elements that differ say which way the array starts, and from then on only ONE test can still change the answer,
 * so the rest is read sixteen at a time with the test's results OR-ed (no branch inside a block, and a loop the compiler can
 * vectorize), and the scalar line takes over inside the block that fires. Comparisons are the same ordered ones, so a NaN answers
 * both tests false exactly as before. From PRESCAN_FAST_MIN elements up only: a small sort keeps its loop untouched (a loop whose start
 * became a variable once cost every sorted call of 2-32 elements 10-16%). noinline: nothing of this lands in the dispatcher's frame.
 * logos_prescan_agree() runs both on one array: the harness asserts they agree. LOGOS_NO_FASTPRESCAN=1 (read from 65,536 up) and
 * -DLOGOS_NO_FASTPRESCAN: the A/B. */
/* VECTOR COMPARES (2026-09-22, found by the triple-reversal weld, tsbench mode `weld`: its one plain scan beat the engine on sorted
 * data). The paragraph above calls the block loop one the compiler can vectorize; at the engine's flags (no -march) it never was:
 * sixteen scalar comisd / seta / or steps with a serial OR chain, about 0.6 ns per element, and a copy of it measured 1.46-1.69x
 * behind SSE2 compares and 1.70-2.07x behind AVX2 ones. Now a block compares two doubles at a time with SSE2 (baseline x86-64, no
 * dispatch) or four with AVX2 (blocks of 32, where the CPU has AVX2 and FMA, as the AVX2 leaf asks), ORs the masks and tests once;
 * the scalar OR of sixteen remains for every other target. The ordered compares (cmplt / cmpgt, _CMP_LT_OQ / _CMP_GT_OQ) answer
 * false on a NaN exactly as the scalar line does. LOGOS_PRESCAN_OR16=1 = the old block, LOGOS_PRESCAN_SSE2=1 = SSE2 on an AVX2
 * CPU (both read per sort from 65,536 up): the A/B, tsbench mode `switch`. logos_prescan_agree() checks every kind this CPU runs. */
#ifndef PRESCAN_FAST_MIN
#define PRESCAN_FAST_MIN 256
#endif
typedef struct { size_t first_desc; int asc, desc; } pscan_t;
/* Each returns the start of the first block in which its test fires, or where whole blocks run out; the scalar line takes over there. */
static inline size_t prescan_rise_or16(const double *a, size_t n, size_t i) {
    for (; i + 16 <= n; i += 16) { int f = 0; for (int k = 0; k < 16; k++) f |= a[i + k] < a[i + k - 1]; if (f) break; }
    return i;
}
static inline size_t prescan_fall_or16(const double *a, size_t n, size_t i) {
    for (; i + 16 <= n; i += 16) { int f = 0; for (int k = 0; k < 16; k++) f |= a[i + k] > a[i + k - 1]; if (f) break; }
    return i;
}
#ifdef LOGOS_HAVE_SSE2
static inline size_t prescan_rise_sse2(const double *a, size_t n, size_t i) {
    for (; i + 16 <= n; i += 16) { __m128d m = _mm_setzero_pd();
        for (int k = 0; k < 16; k += 2) m = _mm_or_pd(m, _mm_cmplt_pd(_mm_loadu_pd(a + i + k), _mm_loadu_pd(a + i + k - 1)));
        if (_mm_movemask_pd(m)) break; }
    return i;
}
static inline size_t prescan_fall_sse2(const double *a, size_t n, size_t i) {
    for (; i + 16 <= n; i += 16) { __m128d m = _mm_setzero_pd();
        for (int k = 0; k < 16; k += 2) m = _mm_or_pd(m, _mm_cmpgt_pd(_mm_loadu_pd(a + i + k), _mm_loadu_pd(a + i + k - 1)));
        if (_mm_movemask_pd(m)) break; }
    return i;
}
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#include <immintrin.h>              /* a no-op when the AVX2 leaf above included it; -DLOGOS_NO_AVX2_LEAF leaves it to here */
static __attribute__((noinline)) size_t prescan_rise_avx2(const double *a, size_t n, size_t i) {
    for (; i + 32 <= n; i += 32) { __m256d m = _mm256_setzero_pd();
        for (int k = 0; k < 32; k += 4) m = _mm256_or_pd(m, _mm256_cmp_pd(_mm256_loadu_pd(a + i + k), _mm256_loadu_pd(a + i + k - 1), _CMP_LT_OQ));
        if (_mm256_movemask_pd(m)) break; }
    return i;
}
static __attribute__((noinline)) size_t prescan_fall_avx2(const double *a, size_t n, size_t i) {
    for (; i + 32 <= n; i += 32) { __m256d m = _mm256_setzero_pd();
        for (int k = 0; k < 32; k += 4) m = _mm256_or_pd(m, _mm256_cmp_pd(_mm256_loadu_pd(a + i + k), _mm256_loadu_pd(a + i + k - 1), _CMP_GT_OQ));
        if (_mm256_movemask_pd(m)) break; }
    return i;
}
#pragma GCC pop_options
#endif
static int g_pre_or16 = 0, g_pre_sse2 = 0;  /* LOGOS_PRESCAN_OR16 / LOGOS_PRESCAN_SSE2, read per sort from 65,536 up */
static inline int prescan_cpu(void) { return logos_simd_kind(); }
static inline pscan_t prescan_kind(const double *a, size_t n, int kind) {
    pscan_t r = { n, 1, 1 };
    size_t i = 1;
    for (; i < n; i++) {                                   /* until one test fires: a couple of elements on random data */
        if (a[i] < a[i - 1]) { r.asc = 0; r.first_desc = i; i++; break; }
        if (a[i] > a[i - 1]) { r.desc = 0; i++; break; }
    }
    if (r.asc) {                                           /* rising or level so far: only a fall can change the answer */
#ifdef LOGOS_HAVE_SSE2
        i = kind == 2 ? prescan_rise_avx2(a, n, i) : kind == 1 ? prescan_rise_sse2(a, n, i) : prescan_rise_or16(a, n, i);
#else
        (void)kind; i = prescan_rise_or16(a, n, i);
#endif
        for (; i < n; i++) if (a[i] < a[i - 1]) { r.asc = 0; r.first_desc = i; break; }
    } else {                                               /* fell first: only a rise can change the answer */
#ifdef LOGOS_HAVE_SSE2
        i = kind == 2 ? prescan_fall_avx2(a, n, i) : kind == 1 ? prescan_fall_sse2(a, n, i) : prescan_fall_or16(a, n, i);
#else
        i = prescan_fall_or16(a, n, i);
#endif
        for (; i < n; i++) if (a[i] > a[i - 1]) { r.desc = 0; break; }
    }
    return r;
}
static __attribute__((noinline, unused)) pscan_t prescan_blocks(const double *a, size_t n) {   /* unused under -DLOGOS_NO_FASTPRESCAN */
    int kind = prescan_cpu();
    if (n >= 65536) {     /* the A/B switches are read HERE, not beside the dispatcher's own: two reads there moved the layout of its small-call path */
        if (ns_depth == 0) { g_pre_or16 = getenv("LOGOS_PRESCAN_OR16") != NULL; g_pre_sse2 = getenv("LOGOS_PRESCAN_SSE2") != NULL; }
        if (g_pre_or16) kind = 0; else if (g_pre_sse2 && kind > 1) kind = 1;
    }
    return prescan_kind(a, n, kind);
}
/* THE PRESCAN IN PARALLEL (2026-10-07, make scoreboard): at 14 threads a sorted array cost 0.35 ns per element, all of it this
   scan on ONE thread, against IPS4o's 0.19. A 64K head is scanned first, exactly as before, so random data still leaves after a
   couple of elements; only when the head is still in order (rising with no fall, or falling with no rise) and the sort has
   threads does the rest go out in PRESCAN_PAR_T chunks, each looking for its first wrong-way step with the same vector helpers,
   boundary pair included. The earliest chunk that found one gives first_desc: the same answer the single scan gives, NaN
   semantics included (a NaN is never a fall). Fewer threads than the sort's: the scan is bandwidth, and a spawn is ~60 us. */
#ifndef PRESCAN_PAR_MIN
#define PRESCAN_PAR_MIN 1048576
#endif
#ifndef PRESCAN_PAR_HEAD
#define PRESCAN_PAR_HEAD 65536
#endif
#ifndef PRESCAN_PAR_T
#define PRESCAN_PAR_T 8
#endif
typedef struct { const double *a; size_t lo, hi, found; int fall, kind; } pspar_t;
static void *prescan_par_worker(void *p) {
    pspar_t *w = p; const double *a = w->a; size_t i = w->lo, hi = w->hi; int kind = w->kind; w->found = hi;
    if (w->fall) {                                         /* rising so far: the first fall */
#ifdef LOGOS_HAVE_SSE2
        i = kind == 2 ? prescan_rise_avx2(a, hi, i) : kind == 1 ? prescan_rise_sse2(a, hi, i) : prescan_rise_or16(a, hi, i);
#else
        (void)kind; i = prescan_rise_or16(a, hi, i);
#endif
        for (; i < hi; i++) if (a[i] < a[i - 1]) { w->found = i; break; }
    } else {                                               /* falling so far: the first rise */
#ifdef LOGOS_HAVE_SSE2
        i = kind == 2 ? prescan_fall_avx2(a, hi, i) : kind == 1 ? prescan_fall_sse2(a, hi, i) : prescan_fall_or16(a, hi, i);
#else
        (void)kind; i = prescan_fall_or16(a, hi, i);
#endif
        for (; i < hi; i++) if (a[i] > a[i - 1]) { w->found = i; break; }
    }
    return NULL;
}
static pscan_t prescan_par_run(const double *a, size_t n, int P) {
    pscan_t h = prescan_blocks(a, PRESCAN_PAR_HEAD);       /* also reads the kind switches, as the single scan does */
    int fall;
    if (h.asc && !h.desc) fall = 1;
    else if (!h.asc && h.desc) fall = 0;
    else if (!h.asc && !h.desc) { pscan_t r = { h.first_desc, 0, 0 }; return r; }   /* broken both ways inside the head: final */
    else return prescan_blocks(a, n);                      /* level so far: the plain scan */
    int kind = prescan_cpu(); if (g_pre_or16) kind = 0; else if (g_pre_sse2 && kind > 1) kind = 1;
    int T = P < PRESCAN_PAR_T ? P : PRESCAN_PAR_T;
    pspar_t w[PRESCAN_PAR_T]; pthread_t th[PRESCAN_PAR_T];
    size_t lo = PRESCAN_PAR_HEAD, per = (n - lo + (size_t)T - 1) / (size_t)T;
    for (int t = 0; t < T; t++) { size_t s = lo + (size_t)t * per, e = s + per; if (s > n) s = n; if (e > n) e = n;
        pspar_t x = { a, s, e, e, fall, kind }; w[t] = x; }
    par_run(th, T, prescan_par_worker, w, sizeof w[0], 1);
    size_t f = n; for (int t = 0; t < T; t++) if (w[t].found < w[t].hi) { f = w[t].found; break; }
    pscan_t r;
    if (fall) { r.first_desc = f; r.asc = f == n; r.desc = 0; }                 /* a rise was seen in the head */
    else      { r.first_desc = h.first_desc; r.asc = 0; r.desc = f == n; }      /* the first fall was in the head */
    return r;
}
static __attribute__((noinline, unused)) pscan_t prescan_par(const double *a, size_t n, int P) {   /* unused under -DLOGOS_NO_FASTPRESCAN */
    if (getenv("LOGOS_NO_PARPRESCAN")) return prescan_blocks(a, n);   /* read here, on this path only: P > 1 and n >= PRESCAN_PAR_MIN */
    return prescan_par_run(a, n, P);
}
int logos_prescan_agree(const double *a, size_t n) {       /* test hook: the scalar line, exactly as the dispatcher runs it, against every kind of block */
    int asc = 1, desc = 1; size_t first_desc = n;
    for (size_t i = 1; i < n && (asc || desc); i++) { if (asc && a[i] < a[i - 1]) { asc = 0; first_desc = i; } if (a[i] > a[i - 1]) desc = 0; }
    if (n < 2) return 1;
    for (int kind = 0; kind <= prescan_cpu(); kind++) {
        pscan_t r = prescan_kind(a, n, kind);
        if (!(r.asc == asc && r.desc == desc && r.first_desc == first_desc)) return 0;
    }
    if (n >= PRESCAN_PAR_MIN) for (int P = 2; P <= 16; P *= 2) {          /* and the parallel scan, at four thread counts */
        pscan_t r = prescan_par_run(a, n, P);
        if (!(r.asc == asc && r.desc == desc && r.first_desc == first_desc)) return 0;
    }
    return 1;
}

/* the vector two-strand merge's switch, read with the others; the kernel and its flag live in the knot route's region */
#ifndef LOGOS_NO_KNOT
static inline void simd_merge_env(void) { g_simd_merge = logos_simd_kind() == 2 && !getenv("LOGOS_NO_SIMD_MERGE"); g_runmerge_vec = !getenv("LOGOS_RUNMERGE_SCALAR"); }
#else
static inline void simd_merge_env(void) {}
#endif
#ifndef LOGOS_NO_MODEKS
/* The per-mode certificate from the SPLITTERS (see mdx_t): modes at the gulfs, each tested for uniformity on its own sample
   slice exactly as the 8-mode version below tests it; up to MD_LIN_MAX modes go to the compare lookup in the job, more to the
   grid. Returns 1 if every mode certified. noinline: its arrays stay out of the dispatcher's frame. */
static __attribute__((noinline)) int md_certify(job_t *j, const double *sp, int nsp, const double *samp, size_t m) {
    if (nsp < 8 || nsp > MAX_BUCKETS) return 0;
    double gaps[MAX_BUCKETS], srt[MAX_BUCKETS]; int ng = nsp - 1;
    for (int i = 0; i < ng; i++) gaps[i] = sp[i + 1] - sp[i];
    memcpy(srt, gaps, (size_t)ng * sizeof(double)); sample_sort(srt, (size_t)ng);
    double med = srt[ng / 2];
    if (!(med > 0)) return 0;
    int cut[MD_MAX], nc = 0;                                  /* the widest MD_MAX - 1 gulfs */
    for (int i = 0; i < ng; i++) if (gaps[i] > BT_MODE_GAP * med) {
        if (nc < MD_MAX - 1) cut[nc++] = i;
        else { int wmin = 0; for (int q = 1; q < nc; q++) if (gaps[cut[q]] < gaps[cut[wmin]]) wmin = q; if (gaps[i] > gaps[cut[wmin]]) cut[wmin] = i; }
    }
    if (nc < 1) return 0;
    qsort(cut, (size_t)nc, sizeof(int), cmp_int);
    int nm = nc + 1, fi[MD_MAX], la[MD_MAX]; double lo[MD_MAX + 1], hi[MD_MAX], sc[MD_MAX];
    for (int k = 0, first = 0; k < nm; k++) { int last = k < nc ? cut[k] : nsp - 1; fi[k] = first; la[k] = last; lo[k] = sp[first]; hi[k] = sp[last]; first = last + 1; }
    size_t at = 0;
    for (int k = 0; k < nm; k++) {                            /* uniform inside each mode: the test of the block below, unchanged */
        if (la[k] <= fi[k] || !(hi[k] > lo[k]) || !isfinite(hi[k] - lo[k]) || !isfinite(1.0 / (hi[k] - lo[k]))) return 0;
        while (at < m && samp[at] < lo[k]) at++;
        size_t from = at;
        while (at < m && samp[at] < hi[k]) at++;
        size_t cnt = at - from;
        if (cnt < 64) return 0;
        double D = 0, inv = 1.0 / (hi[k] - lo[k]);
        for (size_t i = 0; i < cnt; i++) {
            double F = (samp[from + i] - lo[k]) * inv, e1 = F - (double)i / (double)cnt, e2 = (double)(i + 1) / (double)cnt - F;
            if (e1 < 0) e1 = -e1;
            if (e2 < 0) e2 = -e2;
            if (e1 > D) D = e1;
            if (e2 > D) D = e2;
        }
        if (D > HF_KS_MAX / sqrt((double)cnt)) { TRACE("par: mode %d of %d is not uniform inside (KS %.3f)", k, nm, D); return 0; }
        sc[k] = (double)(la[k] - fi[k]) / (hi[k] - lo[k]);
    }
    if (nm <= MD_LIN_MAX) {
        for (int k = 0; k < nm; k++) { j->md_first[k] = fi[k]; j->md_last[k] = la[k]; j->md_lo[k] = lo[k]; j->md_hi[k] = hi[k]; j->md_scale[k] = sc[k]; }
        j->md_nm = nm; j->md_ok = 1;
        TRACE("par: all %d modes certify uniform inside themselves (compares)", nm);
        return 1;
    }
    mdx_t *g = malloc(sizeof *g);
    if (!g) return 0;
    g->nm = nm; g->cells = MD_GRID_CELLS; g->glo = lo[0];
    double span = hi[nm - 1] - lo[0];
    if (!(span > 0) || !isfinite(span)) { free(g); return 0; }
    g->gscale = (double)MD_GRID_CELLS / span;
    if (!isfinite(g->gscale) || !(g->gscale > 0)) { free(g); return 0; }
    for (int k = 0; k < nm; k++) { g->first[k] = fi[k]; g->last[k] = la[k]; g->lo[k] = lo[k]; g->hi[k] = hi[k]; g->scale[k] = sc[k]; }
    g->lo[nm] = NAN;                                          /* the compare past the last boundary is false for every x, +inf included */
    /* grid[c] = boundaries lo[1..nm-1] whose own cell lies BELOW c; two boundaries in one cell: no grid */
    int q = 1;
    for (int c = 0; c < MD_GRID_CELLS; c++) {
        while (q < nm && md_cell(g, g->lo[q]) < c) q++;
        g->grid[c] = (uint8_t)(q - 1);
    }
    for (int b = 2; b < nm; b++) if (md_cell(g, g->lo[b]) == md_cell(g, g->lo[b - 1])) { TRACE("par: mode grid declined, two boundaries in one cell"); free(g); return 0; }
    j->mdx = g; j->md_nm = nm; j->md_ok = 1;
    TRACE("par: all %d modes certify uniform inside themselves (mode grid, %d cells)", nm, MD_GRID_CELLS);
    return 1;
}
#endif
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE) && defined(LOGOS_SODO)
/* IS THE SAMPLE UNSTRUCTURED? The question two changes of 2026-10-19 ask before they trade the partition's order-keeping scatter
 * for a quicksort that cannot see order (the halves route at two threads; the AVX2 quicksort as the partition's leaf). Its scatter
 * keeps local order inside a bucket, and flash and coarse read it; a quicksort does not. Three readings, each a decline:
 * ORDER at the sample's lag (inv_all under HALVES_INV_MIN: a noisy ring buffer read 0.008, eight concatenated logs 0.021, 64
 * shuffled blocks 0.112, every random shape 0.46-0.53), REPEATS (the sample holds duplicates and Chao1 says under two copies per
 * value would be too few: 30,000 quantized values at 250k lost 0.80), and ADJACENT order, which a strided statistic cannot see
 * (a sawtooth whose period is twice the lag reads inv_all 0.5 and lost 0.655): 64 windows of 32 adjacent pairs at Weyl
 * positions, random data ~50% falling. */
#ifndef HALVES_INV_MIN
#define HALVES_INV_MIN 0.25          /* the sample's inversion rate: random data reads ~0.5 */
#endif
#ifndef HALVES_FALL_MIN
#define HALVES_FALL_MIN 0.30         /* adjacent pairs that fall, in 64 windows of 32: random data ~0.5 */
#endif
#ifndef SEQLEAF_MIN_BUCKET
#define SEQLEAF_MIN_BUCKET 96000     /* the partition's mean bucket, elements, from which its leaves take the sequential policy */
#endif
static int sample_unstructured(const double *a, size_t n, const sodo_t *S) {
    if (n < 4096 || S->m < 64) return 0;
    if (S->inv_all < HALVES_INV_MIN) { TRACE("unstructured? the sample is ordered (inv_all %.3f): no", S->inv_all); return 0; }
    size_t mg = S->mfull < SODO_GATE_M ? S->mfull : SODO_GATE_M;      /* distinct and Chao1 count the gate points only, before the extension */
    if (S->distinct < mg && S->chao1 < 0.5 * (double)n) { TRACE("unstructured? the sample repeats (Chao1 %.0f): no", S->chao1); return 0; }
    size_t falls = 0, pairs = 0;
    for (int w = 0; w < 64; w++) {
        size_t at = (size_t)((double)(n - 34) * fmod((double)(w + 1) * 0.6180339887498949, 1.0));
        for (size_t i = at; i < at + 32; i++) { falls += a[i + 1] < a[i]; pairs++; }
    }
    if ((double)falls < HALVES_FALL_MIN * (double)pairs) { TRACE("unstructured? adjacent order (%zu of %zu pairs fall): no", falls, pairs); return 0; }
    return 1;
}
#endif
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE) && !defined(LOGOS_NO_HALVES) && defined(LOGOS_SODO)
/* TWO HALVES, TWO CORES (2026-10-19; a thread ladder found it). The partition at TWO threads ran 1.01x one thread on heavy_tail
 * and bimodal, 1.28x on uniform and 0.76x on clustered, where three threads gave 1.6-2.0x. A trace named two causes. (1) The
 * partition samples 32 points per bucket, 12 buckets per thread: 768 at P = 2, under the 1024 the key grid needs, so a heavy
 * tail paid the table scatter (51 ms against 19 ms at P = 3). (2) Its leaves were 24 buckets of ~350k, past L2, sorted by the
 * PARALLEL leaf, which never takes the AVX2 quicksort (that measured worse at 6 and 14 threads, with L2-resident buckets).
 * Two threads do not need a partition. Split ONCE around the sample's median: each thread partitions its half in place with the
 * kernel's own partition (one predicate, x >= pivot, so a NaN goes left and the result is a permutation whatever happens), the
 * two misplaced blocks trade places (min of their sizes, from both ends), and each part goes to the SEQUENTIAL leaf on its own
 * thread with its own context -- what a one-thread sort runs, twice, at half the size (logos_sort_buckets_f64 already runs
 * sequential leaves side by side). It sits where the hybrid's library call sits: after every structured route has declined.
 * Declines before anything moves when the median does not split the sample within HALVES_BAL of even (heavy duplicates), the
 * sample saw a NaN, or the scratch or a context cannot be had; a decline AFTER the split hands the caller a permutation of its
 * input, which the partition sorts as well as the original. LOGOS_NO_HALVES=1: the partition, for the one-binary A/B. */
#ifndef HALVES_MIN_N
#define HALVES_MIN_N 131072          /* each half must hold the kernel partition's reserved vectors many times over */
#endif
#ifndef HALVES_BAL
#define HALVES_BAL 0.125             /* the sample's left share must lie in 0.5 +- this */
#endif
static size_t g_halves;              /* logos_twist_last(26): sorts the two-halves route finished */
typedef struct { double *a; size_t lo, hi, m, s0, s1; double piv; double *buf; ctx_t *c; int phase; } hv_arg_t;
static void *hv_worker(void *p) {
    hv_arg_t *q = p; double *a = q->a;
    if (q->phase == 0) {                                     /* this thread's half, partitioned in place around the pivot */
        double sm = INFINITY, bg = -INFINITY; int fl = 0;
        q->m = q->lo + av_partition_fl(a + q->lo, q->hi - q->lo, q->piv, &sm, &bg, &fl);
    } else if (q->phase == 1) {                              /* its share of the block trade: pair i is (lo + i, hi - 1 - i) */
        for (size_t i = q->s0; i < q->s1; i++) { size_t x = q->lo + i, y = q->hi - 1 - i; double t = a[x]; a[x] = a[y]; a[y] = t; }
    } else {                                                 /* its part, by the sequential leaf */
        size_t len = q->hi - q->lo;
        if (len > 1) { double *r = leaf_f64(a + q->lo, len, q->buf + q->lo, q->c); if (r != a + q->lo) memcpy(a + q->lo, r, len * sizeof(double)); }
    }
    return NULL;
}
__attribute__((noinline)) static int halves_try_f64(double *a, size_t n, int P, scratch_t *sc, const sodo_t *S) {
    const double *samp = S->sorted; size_t m = S->m;
    if (P != 2 || n < HALVES_MIN_N || !samp || m < 64 || !avx2_leaf_on() || getenv("LOGOS_NO_HALVES")) return 0;
    if (!sample_unstructured(a, n, S)) return 0;                      /* order and repeats stay with the partition */
    double piv = samp[m / 2];
    size_t lb = 0, hb = m;                                   /* sample points below the pivot */
    while (lb < hb) { size_t mid = lb + (hb - lb) / 2; if (samp[mid] < piv) lb = mid + 1; else hb = mid; }
    double share = (double)lb / (double)m;
    if (!(share >= 0.5 - HALVES_BAL && share <= 0.5 + HALVES_BAL)) { TRACE("halves: the median splits the sample %.3f / %.3f, declined", share, 1 - share); return 0; }
    double *buf = scratch_get(sc);
    if (!buf) return 0;
    hv_arg_t q[2]; pthread_t th[2]; size_t h = n / 2;
    memset(q, 0, sizeof q);
    q[0].a = q[1].a = a; q[0].piv = q[1].piv = piv; q[0].buf = q[1].buf = buf;
    q[0].lo = 0; q[0].hi = h; q[1].lo = h; q[1].hi = n;
    par_run(th, 2, hv_worker, q, sizeof q[0], 1);
    size_t m0 = q[0].m, m1 = q[1].m - h;                     /* left counts: [0, m0) and [h, h + m1) hold x < pivot (or NaN) */
    size_t A = h - m0, B = m1, k = A < B ? A : B, L = m0 + m1;
    for (int t = 0; t < 2; t++) { q[t].phase = 1; q[t].lo = m0; q[t].hi = h + m1; q[t].s0 = k * (size_t)t / 2; q[t].s1 = k * (size_t)(t + 1) / 2; }
    if (k) par_run(th, 2, hv_worker, q, sizeof q[0], 1);
    TRACE("halves: pivot %.6g, parts %zu / %zu (sample said %.3f), %zu pairs traded", piv, L, n - L, share, k);
    ctx_t c[2];
    size_t cap = L > n - L ? L : n - L;
    if (!ctx_init(&c[0], cap, 0)) return 0;                  /* the array is a permutation of what came in: the caller sorts that */
    if (!ctx_init(&c[1], cap, 0)) { ctx_free(&c[0]); return 0; }
    c[0].seq = c[1].seq = 1;
    for (int t = 0; t < 2; t++) { q[t].phase = 2; q[t].c = &c[t]; }
    q[0].lo = 0; q[0].hi = L; q[1].lo = L; q[1].hi = n;
    par_run(th, 2, hv_worker, q, sizeof q[0], 1);
    ctx_free(&c[0]); ctx_free(&c[1]);
    g_halves++;
    return 1;
}
#endif
/* A LATTICE THE SAMPLE CANNOT SEE REPEAT (2026-10-31, the boundary round, FINDINGS 3br; `make boundprobe`). The counting block
 * above is entered only when the sample shows repeats (distinct <= 0.98 m), and the sample is 384 points per thread below eight
 * threads: at one thread 30,000 values on a lattice give it two or three repeats, so the lattice route never ran there and the data
 * went to the float32 radix or the AVX2 quicksort -- an entropy-bounded COMPARISON sort at ~0.45 ns per bit of the input, 7.65 ns
 * per element on 30,000 values at 16M, where counting by direct index owes a constant per element. A lattice does not need repeats
 * to be seen: every sample point must lie on one (lattice_sample_step; lattice_try_f64 asks the same and the map checks every
 * element). What the repeats gate also guarded is the FILL: a lattice wider than the data is walked for nothing. With the gate
 * lifted (`./boundprobe ab`, A/B on the same input), one thread: 8 elements per slot LOST 0.79 (131,072 values at 1M), 10.5 read
 * 1.00-1.23, 16 and up won 1.22-4.56. Four threads: 16-17 a slot lost 0.69-0.75, 32-42 won 1.24-1.55 -- the partition gains more
 * from threads than the route's serial slot merge does. So the span must leave LAT1_MIN_FILL * (P + 1) / 2 elements per slot: 16
 * at one thread, 24 at two, 40 at four. At eight threads and up the 2,048-point gate sees the repeats itself.
 * LOGOS_NO_LAT1=1: off; LOGOS_LAT1_FILL=<f> moves the one-thread fill; LOGOS_LAT1_PMAX=<p>: no entry above p threads. */
#ifndef LAT1_PMAX
#define LAT1_PMAX 64
#endif
static __attribute__((noinline, unused)) int lat1_gate(const double *s, size_t m, size_t n, int P) {
    if (m < 64 || getenv("LOGOS_NO_LAT1")) return 0;
    int pmax = LAT1_PMAX; { const char *e = getenv("LOGOS_LAT1_PMAX"); if (e) pmax = atoi(e); } if (P > pmax) return 0;
    double fill = lat_fill_min(P); { const char *e = getenv("LOGOS_LAT1_FILL"); if (e && atof(e) > 0) fill = atof(e) * (double)(P + 1) / 2.0; }
    double smin = s[0], smax = s[m - 1];
    if (!isfinite(smin) || !isfinite(smax)) return 0;
    size_t distinct; int div; double step = lattice_sample_step(s, m, &distinct, &div);
    if (distinct < 3 || !(step > 0)) return 0;
    double span = (smax - smin) / step;
    return span >= 0 && (span + 1.0) * fill <= (double)n;
}
static __attribute__((unused)) atomic_size_t g_lat1_sorts;     /* logos_twist_last(36): sorts the lattice entry (lat1_gate) finished */
static __attribute__((unused)) atomic_size_t g_boosts;         /* logos_twist_last(39): partitions that took the unstructured thread boost */
static void psort_f64(double *a, size_t n, int P, size_t *vstart, int *vnbk, double *vmin, double *vmax) {
    if (vnbk) *vnbk = 0;
    if (P < 1) P = 1;
    if (n < 2) return;
    if ((size_t)P > n) P = (int)n;
    int Preq = P;
    P = effective_threads(n, P);
    TRACE_START(); TRACE("psort_f64 n=%zu P=%d", n, P);
#ifndef LOGOS_NO_REACH
    /* Reset here, not at the route: a sort that ends in an earlier route must not report the last one's answer. And only where the
     * route can run: under -fPIC a thread-local access is a call to __tls_get_addr, and reading ns_depth plus writing this put TWO of
     * them on the path of a three-element sort, where there had been none. A cost per CALL shows only at make scale-full's small
     * end; no bench that starts at a million can see it. A smaller sort leaves the answer alone, and logos_psort.h says so. */
    if (n >= REACH_MIN_N && ns_depth == 0) g_reach_last = LOGOS_REACH_NOT_TRIED;
#endif
#ifndef LOGOS_NO_RELOCATE
    if (n >= MIN_PARALLEL) { g_stray_on = !getenv("LOGOS_NO_STRAY") && !getenv("LOGOS_KNOT_LIST"); g_stray_par = !getenv("LOGOS_STRAY_SEQ"); { const char *e = getenv("LOGOS_STRAY_DIV"); g_stray_div = e && atol(e) >= 4 ? (size_t)atol(e) : 16; } simd_merge_env(); g_reloc_nocopy = getenv("LOGOS_RELOC_NOCOPY") != NULL; g_onerun_off = getenv("LOGOS_NO_ONERUN") != NULL; g_desc_ties = !getenv("LOGOS_DESC_STRICT"); g_onerun_copy = getenv("LOGOS_ONERUN_COPY") != NULL; g_scan_scalar = getenv("LOGOS_SCANBLOCK_SCALAR") != NULL;
                                  { const char *cm = getenv("LOGOS_COUNT_MAX"); g_count_max = cm ? atoi(cm) : COUNT_MAX_THREADS; }
                                  g_lat_par_merge = getenv("LOGOS_LAT_PAR_MERGE") != NULL;
                                  g_lat_serial_tables = getenv("LOGOS_LAT_SERIAL_TABLES") != NULL;
                                  g_lat_static_map = getenv("LOGOS_LAT_STATIC_MAP") != NULL; }
#endif
#ifndef LOGOS_NO_KNOT
    /* Two getenv calls are about 40 ns. Read on every call, as the first cut did, that was 7x the whole sort of three
     * sorted elements (6 ns -> 42 ns; make scale-full's small sizes). Below two strands of KNOT_MIN_AVG_RUN the route
     * cannot fire, so the switches are not read there. */
    if (n >= 2 * (size_t)KNOT_MIN_AVG_RUN) knot_env();
#endif
    if (n >= 65536 && ns_depth == 0) { int lowmem = getenv("LOGOS_LOWMEM") != NULL;   /* LOGOS_LOWMEM=1: every form that trades speed for memory (FINDINGS 3bq) */
                                       g_no_dense_probe = getenv("LOGOS_NO_DENSE_PROBE") != NULL; g_quant_strided = getenv("LOGOS_QUANT_STRIDED") != NULL; g_quant_radix = getenv("LOGOS_QUANT_RADIX") != NULL; g_no_fastpre = getenv("LOGOS_NO_FASTPRESCAN") != NULL;
                                       g_no_frontfuse = getenv("LOGOS_NO_FRONTFUSE") != NULL; g_no_partfuse = getenv("LOGOS_NO_PARTFUSE") != NULL;
                                       { const char *tr = getenv("LOGOS_TRIM"); g_no_trim = !tr && !lowmem; g_trim_hard = tr ? !strcmp(tr, "hard") : lowmem; }
                                       g_no_latvec = getenv("LOGOS_NO_LATVEC") != NULL;
                                       g_no_f32 = getenv("LOGOS_NO_F32") != NULL;
#ifndef LOGOS_NO_HISTFREE
                                       g_part_inplace = lowmem || getenv("LOGOS_PART_INPLACE") != NULL;
#endif
                                     }
    PROF(g_prof.n = n; g_prof.nthreads = P; g_prof.route = LOGOS_ROUTE_NONE; double t_start = prof_now(); g_prof_tp = t_start;)

    /* whole-array early exits (the JS prescan's O(n) wins; short-circuits fast on random data) */
    { int asc = 1, desc = 1; size_t first_desc = n;
#ifndef LOGOS_NO_FASTPRESCAN
      if (n >= PRESCAN_FAST_MIN && !(g_no_fastpre && n >= 65536)) { pscan_t ps = P > 1 && n >= PRESCAN_PAR_MIN ? prescan_par(a, n, P) : prescan_blocks(a, n); asc = ps.asc; desc = ps.desc; first_desc = ps.first_desc; }
      else
#endif
      for (size_t i = 1; i < n && (asc || desc); i++) { if (asc && a[i] < a[i - 1]) { asc = 0; first_desc = i; } if (a[i] > a[i - 1]) desc = 0; }
      if (asc) { PROF(prof_phase(LOGOS_PH_PRESCAN); prof_finish(LOGOS_ROUTE_SORTED, t_start);) return; }
      if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } PROF(prof_phase(LOGOS_PH_PRESCAN); prof_finish(LOGOS_ROUTE_SORTED, t_start);) return; }
      (void)first_desc;
#ifndef LOGOS_NO_LOOPBACK
      /* the loop-back: an ascending suffix that continues into the ascending prefix across the wrap is one strand; see loopback_gate */
      if (n >= LOOPBACK_MIN_N && !g_looped && !vstart) {
          size_t s = loopback_gate(a, n, first_desc, P > 1 && n >= MIN_PARALLEL);
          if (s) {
              size_t known = n - s + first_desc;                    /* after the rotation [0, known) ascends: suffix, then prefix */
              TRACE("loop-back: prefix %zu, suffix %zu continues into it across the wrap: rotated (%zu known ascending)", first_desc, n - s, known);
              rotate_left_f64(a, n, s); g_loop_count++;
              /* One pass saved each way, and it is most of the difference: a ring buffer costs the rotation plus two read passes,
                 so re-entering the dispatcher, whose prescan re-read all of it, measured 0.89 against the tail route it replaces at
                 one thread. A hint to the prescan (start past the known stretch) fixed that and cost every SORTED call of 2 to 32
                 elements 10-16% (make callbench: the loop's start became a variable). So: the whole ring one strand, return; else the
                 tail route directly, since [0, known) is the sorted prefix it wants and the re-entry chose it anyway; the dispatcher
                 only if that declines (a NaN in the remainder, no scratch). */
              if (known >= n) { PROF(prof_phase(LOGOS_PH_PRESCAN); prof_finish(LOGOS_ROUTE_SORTED, t_start);) return; }   /* the whole ring was one strand */
              g_looped = 1;
#ifndef LOGOS_NO_TAILROUTE
              if (tail_merge_try_f64(a, n, known, P)) {
                  TRACE("loop-back: tail route took the remainder (%zu)", n - known);
                  PROF(prof_phase(LOGOS_PH_NEARSORTED); prof_finish(LOGOS_ROUTE_NEARSORTED, t_start);)
                  g_looped = 0; return;
              }
#endif
              psort_f64(a, n, P, NULL, NULL, NULL, NULL);        /* re-enter plainly */
              g_looped = 0;
              return;
          }
      }
#endif
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
      if (first_desc < n / 2 && n >= TAIL_MIN_N && (P == 1 || n < MIN_PARALLEL) && head_merge_try_f64(a, n, P)) {
          TRACE("head: an unsorted head before a sorted suffix, sorted and merged forward");
          PROF(prof_phase(LOGOS_PH_NEARSORTED); prof_finish(LOGOS_ROUTE_NEARSORTED, t_start);)
          return;
      }
#endif
    }
    PROF(prof_phase(LOGOS_PH_PRESCAN);)
    /* few distinct values: map+emit counting sort (parallel when P>1) instead of
       a partition that cannot split equal keys, or a span-sized count array */
    TRACE("prescan done");
    int Pb = P; (void)Pb; (void)Preq;                 /* Pb: the thread count the unstructured partition may take (boost_threads) */
#ifdef LOGOS_SODO
    sodo_t SO; int sodo_ok = 0;
    Pb = P > 1 && Preq > P ? boost_threads(n, P, Preq) : P;          /* the unstructured partition's own floor (IDEAS_LOG 21 (a)) */
    { int nbk0 = P * BUCKETS_PER_THREAD; if (nbk0 > MAX_BUCKETS) nbk0 = P * (MAX_BUCKETS / P > 0 ? MAX_BUCKETS / P : 1);
      if (n >= 4096) sodo_ok = sodo_sample(a, n, (size_t)nbk0 * OVERSAMPLE, &SO); }
    if (sodo_ok) TRACE("sodo: m=%zu distinct=%zu chao1=%.0f inv=%.3f inv_all=%.3f reach=%zu ks=%.4f nan=%d", SO.m, SO.distinct, SO.chao1, SO.inv_rate, SO.inv_all, SO.reach, SO.ks, SO.has_nan);
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
#ifndef LOGOS_NO_LATTICE
    /* a lattice the sample cannot see repeat (lat1_gate); only where the block above was not entered, so no lattice is tried twice */
    if (sodo_ok && !SO.has_nan && n >= MODCARD_MIN_N && (double)SO.distinct > (P >= WC_MIN_THREADS ? MODCARD_GATE_MT : MODCARD_GATE) * (double)SO.m
        && lat1_gate(SO.sorted, SO.m, n, n < MIN_PARALLEL ? 1 : P) && lattice_try_f64(a, n, n < MIN_PARALLEL ? 1 : count_threads(n, P), SO.sorted, SO.m)) {
        atomic_fetch_add_explicit(&g_lat1_sorts, 1, memory_order_relaxed);
        free(SO.sorted); PROF(prof_phase(LOGOS_PH_MODCARD); g_prof.route = LOGOS_ROUTE_LATTICE; prof_finish(LOGOS_ROUTE_LATTICE, t_start);) return;
    }
#endif
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

#ifdef LOGOS_HAVE_DOMINANT
    /* one value holding a large share of the array: the rest sorted on its own, the value filled in (see dominant_try_f64) */
    if (sodo_ok && !SO.has_nan && dominant_try_f64(a, n, P, SO.sorted, SO.m)) {
        free(SO.sorted); PROF(prof_phase(LOGOS_PH_LOWCARD); prof_finish(LOGOS_ROUTE_LOWCARD, t_start);) return;
    }
#endif
#ifndef LOGOS_NO_REACH
    /* short reach: every element near its final place. Sort index blocks where they lie and repair only the seams; tried
       before any n-sized buffer is asked for, because without strays it needs none. */
    if (sodo_ok && SO.reach && reach_try_f64(a, n, P, SO.reach)) {
        free(SO.sorted);
        PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_REACH, t_start);)
        return;
    }
#ifndef LOGOS_NO_SEAM
    if (g_reach_seam) {                                                /* the reach route found a rotation in its fences: rotate once, enter again */
        size_t sm = g_reach_seam; g_reach_seam = 0;
        if (sodo_ok) { free(SO.sorted); SO.sorted = NULL; }
        TRACE("seam: rotated by %zu", sm);
        rotate_left_f64(a, n, sm); g_seam_count++; g_seamed = 1;
        psort_f64(a, n, P, NULL, NULL, NULL, NULL);
        g_seamed = 0;
        return;
    }
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
        /* no ctx_free here: when buf failed, ctx_init never ran and c is whatever the stack held (it used to be freed: a crash
           in-process, luck in a fresh one); when ctx_init failed, c is all zeros and holds nothing */
        if (!buf || !ctx_init(&c, n, 0)) { if (pooled) pool_release(); else free(buf); qsort(a, n, sizeof(double), cmp_d); return; }
        c.seq = 1;
        TRACE("seq: buffers ready (pooled=%d)", pooled);
        PROF(prof_phase(LOGOS_PH_OTHER);)
#ifndef LOGOS_NO_RELOCATE
        if (n >= RELOC_MIN_N && relocate_try_f64(a, n, 1, buf, NULL)) { TRACE("seq: relocated, done"); PROF(prof_phase(LOGOS_PH_RELOCATE); prof_finish(LOGOS_ROUTE_RELOCATE, t_start);) if (pooled) pool_release(); else free(buf); ctx_free(&c); return; }   /* J5 */
        PROF(prof_phase(LOGOS_PH_RELOCATE);)
        TRACE("seq: relocation declined");
#endif
#if defined(LOGOS_SODO) && !defined(LOGOS_NO_MIRROR)
        /* the whole array's orientation bit: see mirror_gate. Noise never gets past the first test, which reads a number already in hand. */
        /* The writhe cannot pre-gate STREAMS: whether a sampled pair falls inside one stream depends on the lag modulo the stream
           count (four mirrored streams read 0.88 at 1,048,576 elements and under 0.6 at 1,000,003). A multimodal sample is what
           gets the ascending probe a few lines down, so it gets the mirrored one too; it stops after ~25 reads on data that ascends. */
#ifndef LOGOS_NO_STREAMS
        int mirror_ask = sodo_ok && (SO.inv_all >= MIRROR_INV_MIN || st_nm >= 2);
#else
        int mirror_ask = sodo_ok && SO.inv_all >= MIRROR_INV_MIN;
#endif
        if (sodo_ok && !SO.has_nan && mirror_ask && n >= MIRROR_MIN_N && !g_mirrored && !vstart) {
#ifndef LOGOS_NO_STREAMS
            int mg = mirror_gate(a, n, SO.inv_all, st_bnd, st_nm);
#else
            int mg = mirror_gate(a, n, SO.inv_all, NULL, 0);
#endif
            TRACE("mirror: inv_all=%.3f, gate %d (1 descending suffix, 2 reach, 3 streams)", SO.inv_all, mg);
            if (mg) {
                if (pooled) pool_release(); else free(buf);
                ctx_free(&c);
                for (size_t l = 0, r = n - 1; l < r; l++, r--) { double x = a[l]; a[l] = a[r]; a[r] = x; }
                g_mirror_count++; g_mirrored = 1;
                psort_f64(a, n, P, NULL, NULL, NULL, NULL);          /* re-enter: the prescan, the ladder and the stream probe now read it their way */
                g_mirrored = 0;
                return;
            }
        }
#endif
#if defined(LOGOS_SODO) && !defined(LOGOS_NO_STREAMS)
        if (st_nm >= 2 && streams_try_f64(a, n, buf, &c, st_bnd, st_nm, st_lo, st_hi)) {
            PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
            if (pooled) pool_release(); else free(buf);
            ctx_free(&c);
            return;
        }
#endif
#if defined(LOGOS_SODO) && (!defined(LOGOS_NO_SEAM) || !defined(LOGOS_NO_WAVE) || !defined(LOGOS_NO_CUTS))
        /* the sample in position order: the seam route (a noisy ring buffer) and the wave route (monotone pieces at the sample's
           scale, noise at the element's). Read the header of sample_by_pos. */
        if (sodo_ok && !SO.has_nan && n >= WAVE_MIN_N && !vstart && !g_seamed) {
            psamp_t *ps = sample_by_pos(a, n, SODO_GATE_M);
            int wavey = SO.inv_all <= WAVE_INV_MAX;                   /* the seam and the wave need a mostly ascending sample; the cuts route asks its own */
            if (ps) {
#ifndef LOGOS_NO_SEAM
                size_t seam = wavey ? seam_gate(a, n, ps, SODO_GATE_M) : 0;
                if (seam) {
                    TRACE("seam: a noisy ring buffer, seam at %zu: rotated", seam);
                    free(ps); if (pooled) pool_release(); else free(buf); ctx_free(&c);
                    rotate_left_f64(a, n, seam); g_seam_count++; g_seamed = 1;
                    psort_f64(a, n, P, NULL, NULL, NULL, NULL);
                    g_seamed = 0; return;
                }
#endif
#ifndef LOGOS_NO_WAVE
                if (wavey && wave_try_f64(a, n, buf, ps, SODO_GATE_M)) {
                    free(ps); PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
                    if (pooled) pool_release(); else free(buf); ctx_free(&c); return;
                }
#endif
#ifdef LOGOS_HAVE_CUTS
                /* after the wave route: a sloped sine has cuts at its troughs, and the wave route is the measured win there */
                if (cuts_try_f64(a, n, buf, &c, ps, SODO_GATE_M)) {
                    free(ps); PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
                    if (pooled) pool_release(); else free(buf); ctx_free(&c); return;
                }
#endif
                free(ps);
            }
        }
#endif
#ifndef LOGOS_NO_RLE
        /* Runs of equal values, LAST before the leaf: every order route has declined by now. Placed first (2026-10-20), it took a
           mirrored 95% prefix of runs of seven equal keys from the mirror and tail routes that use its order (harness, mirror rows). */
        if (rle_try_f64(a, n)) {
            PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_SEQUENTIAL, t_start);)
            if (pooled) pool_release(); else free(buf); ctx_free(&c); return;
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
    scratch_t sc = { n, j.nbk <= 256 ? n : 0, NULL, NULL, NULL, NULL, 0, 0, 0 };   /* j.dst and j.bidx stay NULL until a route asks (scratch_get) */
#ifdef LOGOS_EAGER_SCRATCH
    (void)scratch_get(&sc);
#endif
    PROF(prof_phase(LOGOS_PH_OTHER);)
    /* KINKS go past near-sorted to the detector (2026-09-26, make kinkladder). The reach probe above declined as sparse and
       saw that the sparse descents were kink-shaped; near-sorted would take that array (its gate reads inv_all, which is
       0.000 on kinks: sampled pairs sit far apart and adjacent swaps never invert one) and sort ~41,745 strays instead of
       letting the detector swap ~41,745 pairs back where they stand. One thread-local read, on the P > 1 path only. */
    int kinks_seen = 0;
#ifndef LOGOS_NO_REACH
    kinks_seen = (g_reach_last == LOGOS_REACH_DECLINED_KINKS);
    if (kinks_seen) TRACE("par: nearsorted skipped, the reach probe saw sparse KINKS -> relocation");
#endif
#ifndef LOGOS_NO_NEARSORTED
#ifdef LOGOS_SODO
    /* inv_all, not inv_rate: the latter is 7 pairs a whole array apart (see sodo_sample). It passed block-permuted, sawtooth
       and grouped-run data as nearly sorted, and turned away one genuinely near-sorted input in seven. */
    if (!kinks_seen && nearsorted_parallel_f64(a, n, P, NULL, &sc, sodo_ok ? (getenv("LOGOS_NS_GATE_OLD") ? SO.inv_rate : SO.inv_all) : -1.0)) {
#else
    if (!kinks_seen && nearsorted_parallel_f64(a, n, P, NULL, &sc, -1.0)) {
#endif         /* keeps order instead of scattering it */
        PROF(prof_phase(LOGOS_PH_NEARSORTED); prof_finish(LOGOS_ROUTE_NEARSORTED, t_start);)
        scratch_drop(&sc);
#ifdef LOGOS_SODO
        if (sodo_ok) free(SO.sorted);
#endif
        return;
    }
#endif
    PROF(prof_phase(LOGOS_PH_NEARSORTED);)
    TRACE("par: nearsorted declined");
#ifndef LOGOS_NO_RELOCATE
    if (n >= RELOC_MIN_N && relocate_try_f64(a, n, P, NULL, &sc)) {   /* J5: block-permuted data -> memcpy runs into place, no scatter */
        TRACE("par: relocated, done");
        PROF(prof_phase(LOGOS_PH_RELOCATE); prof_finish(LOGOS_ROUTE_RELOCATE, t_start);)
        scratch_drop(&sc);
#ifdef LOGOS_SODO
        if (sodo_ok) free(SO.sorted);
#endif
        return;
    }
    TRACE("par: relocation declined");
    PROF(prof_phase(LOGOS_PH_RELOCATE);)
#endif
#ifdef LOGOS_SODO
    /* THE UNSTRUCTURED PARTITION'S OWN THREAD FLOOR (2026-11-09, IDEAS_LOG 21 (a), FINDINGS 3bz). Every structured route has
       declined; if the sample is unstructured the partition takes one thread per BOOST_PER elements instead of the global one per
       MIN_ELEMS_PER_THREAD, which was set from the cost of creating threads before the worker pool existed and which the
       structured routes still need (a 25k or 50k floor for everything lost 3-5x on kinks and blocks at 100k).
       THREADS, NOT BUCKETS: the bucket count stays the policy's, so the sample, the splitters and every map are exactly what the
       unboosted sort builds. With the buckets doubled too, the first cuts lost wherever a map that balanced at the policy's count
       did not at twice it (64 thin clusters' certified grid: spills, then the histogram path, 1.5-2.8x slower; a gaussian's key
       grid rejected at 72 buckets and kept at 36: 1.16), and rebuilding the maps to go back cost 10-20% by itself.
       LOGOS_BOOST_PER=<k> moves the floor (a value under 1000 turns the boost off), LOGOS_BOOST_MIN=<n> its smallest array. */
    if (Pb > P && sodo_ok && !SO.has_nan && !vnbk && sample_unstructured(a, n, &SO)) {
        TRACE("par: unstructured -> partition threads %d -> %d (buckets stay %d)", P, Pb, j.nbk);
        P = Pb; j.P = P; atomic_fetch_add(&g_boosts, 1);
        j.wc_on = (P >= WC_MIN_THREADS);
        j.csize = (n + (size_t)P * 4 - 1) / ((size_t)P * 4); if (j.csize > CHUNK_MAX) j.csize = CHUNK_MAX; if (j.csize < 1) j.csize = 1;
        j.nchunk = (n + j.csize - 1) / j.csize;
    }
#endif
#if defined(LOGOS_XSS_LEAF) && defined(LOGOS_AVX2_LEAF)
    if (P <= XSS_PAR_MAX && avx2_leaf_on() && !getenv("LOGOS_NO_XSS_PAR") && avx2_qsort_f64_p(a, n, P)) {
        TRACE("par: no structure found at P=%d -> x86-simd-sort, parallel", P);
        scratch_drop(&sc);
#ifdef LOGOS_SODO
        if (sodo_ok) free(SO.sorted);
#endif
        PROF(prof_phase(LOGOS_PH_OTHER); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
        return;
    }
#endif
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE) && !defined(LOGOS_NO_HALVES) && defined(LOGOS_SODO)
    if (!vnbk && sodo_ok && !SO.has_nan && halves_try_f64(a, n, P, &sc, &SO)) {    /* two threads: one split, two sequential leaves; never for the lazy view, which wants its buckets unsorted */
        TRACE("par: no structure found at P=2 -> two halves, sequential leaves");
        scratch_drop(&sc);
        free(SO.sorted);
        PROF(prof_phase(LOGOS_PH_BUCKETS); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
        return;
    }
#endif
    double *sp = malloc((size_t)j.nsp * sizeof(double));
    j.counts = calloc(j.nchunk * j.nbk, sizeof(size_t)); j.offs = malloc(j.nchunk * j.nbk * sizeof(size_t));
    j.bsize = calloc(j.nbk, sizeof(size_t)); j.bstart = calloc(j.nbk, sizeof(size_t));
    warg_t *wa = malloc((size_t)P * sizeof(warg_t)); pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    if (!sp || !j.counts || !j.offs || !j.bsize || !j.bstart || !wa || !th || !ctxs) goto fallback;

    size_t m = (size_t)j.nbk * OVERSAMPLE; if (m > n) m = n;
    double *samp = NULL;
#ifdef LOGOS_SODO
    if (sodo_ok && SO.mfull == m && sodo_extend(a, n, &SO) && !SO.has_nan) { samp = SO.sorted; SO.sorted = NULL; }   /* the one sample, extended and sorted */
    /* Not handed over (a NaN in it, another size, or it could not be extended): it is not needed again, and it used to LEAK
       here, 11 KB per sort, because only the fallback label freed it. Found the first time the harness ran with leak checking
       on (make sanitize, 2026-09-20); it had been switched off. */
    else if (sodo_ok) { free(SO.sorted); SO.sorted = NULL; }
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
    /* The range must be FINITE. With an infinite sample minimum every F below is inf * 0 = NaN, every compare with it is
     * false, D stays 0 and the data was certified uniform; the grid lookup then cast NaN to a bucket index and the scatter
     * wrote through it (a[0] = -inf in uniform data at 4 threads: SEGV in hf_scatter; position 0 is always a sample point.
     * Found 2026-09-19 by the knot harness's infinite-endpoint shape). A finite pair whose difference overflows is the same. */
    if (m >= 256 && j.ks_hi > j.ks_lo && isfinite(j.ks_hi - j.ks_lo) && isfinite(1.0 / (j.ks_hi - j.ks_lo))) {   /* finite range AND finite scale (subnormal-wide ranges overflow it) */
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
    uint8_t *hvmask = NULL;                                           /* sample points that are heavy values: the balance check leaves them out */
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
#ifdef LOGOS_HISTFREE
        j.hv = hv_build(samp, m, j.nbk, &hvmask);
#endif
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
    if (!getenv("LOGOS_NO_MDGRID")) (void)md_certify(&j, sp, j.nsp, samp, m);    /* up to MD_MAX modes, from the splitters */
    else if (j.bt_ok && j.bt.nm >= 2 && j.bt.nm <= 8) {
        int nm = j.bt.nm, ok = 1;
        size_t at = 0;
        for (int k = 0; k < nm && ok; k++) {
            int fi = j.bt.mfirst[k], la = j.bt.mlast[k];
            double lo = j.bt.mlo[k], hi = j.bt.mhi[k];
            if (la <= fi || !(hi > lo) || !isfinite(hi - lo) || !isfinite(1.0 / (hi - lo))) { ok = 0; break; }   /* an infinite mode: F below is NaN, D stays 0 and it would certify */
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
            for (size_t i = 0; i < m; i++) { if (hvmask && hvmask[i]) continue; int b_ = (EXPR); if (b_ >= 0 && b_ < j.nbk) load[b_]++; } \
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
    free(hvmask);
    free(samp); j.sp = sp;
    PROF(prof_phase(LOGOS_PH_SAMPLE);)
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE) && defined(LOGOS_SODO)
    /* THE AVX2 QUICKSORT AS THE PARTITION'S LEAF, where buckets are past L2 (2026-10-19, roundbench). The ledger had it 5-28%
       WORSE at 14 threads and 10-16% at 6, measured before the kernel was bridged to x86-simd-sort's (FINDINGS 3bh: its gap
       1.31-1.41 -> 1.07-1.12) and with 33k-element buckets that fit L2. Re-measured with the sequential leaf's whole policy on
       the partition's contexts: 8.4M at three threads (233k buckets) uniform 1.30, heavy_tail 1.34, bimodal 1.29, normal 1.36,
       clustered 2.17; four threads 1.23-1.81; six 1.08-1.13; fourteen (50k buckets) 0.98-1.02, and at 30M (178k buckets)
       heavy_tail 1.19, clustered 1.23. So the bucket's size decides, not the thread count, and only on a sample that looks
       unstructured: a noisy ring buffer's buckets lost 0.79, because block-ordered buckets are what flash and coarse are for.
       LOGOS_NO_SEQLEAF=1: the parallel leaf's policy, for the A/B. */
    j.seq_leaf = sodo_ok && !SO.has_nan && n / (size_t)j.nbk >= SEQLEAF_MIN_BUCKET && avx2_leaf_on() && !getenv("LOGOS_NO_SEQLEAF")
                 && sample_unstructured(a, n, &SO);
    TRACE("par: leaves %s (mean bucket %zu)", j.seq_leaf ? "take the sequential leaf" : "keep the parallel leaf", n / (size_t)j.nbk);
#endif
#ifdef LOGOS_HISTFREE
    if (hf_partition(&j, P, ctxs, th, vnbk != NULL, vstart, vnbk, vmin, vmax)) {
        TRACE("par: histfree done");
        for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
        scratch_drop(&sc);
#ifndef LOGOS_NO_SPLITTER_TABLE
        bt_free(&j.bt);
#endif
        free(j.mdx); j.mdx = NULL;                        /* the mode grid (mdx_t), outside the table's #ifndef */
    free(j.hv); j.hv = NULL;
        free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
        PROF(prof_phase(LOGOS_PH_OTHER); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
        return;
    }
    TRACE("par: histfree declined, histogram path");
#endif
    if (!scratch_get(&sc)) goto fallback;                     /* the histogram path scatters into the scratch: taken now, not before */
    j.dst = sc.dst; j.bidx = sc.bidx;

    barrier_init(&j.bar, P); atomic_init(&j.hnext, 0); atomic_init(&j.snext, 0); atomic_init(&j.next, 0);
    for (int t = 0; t < P; t++) { wa[t].j = &j; wa[t].t = t; wa[t].ctx = &ctxs[t]; }
    int hist_made = par_spawn(th, P, worker, wa, sizeof wa[0], &j.bar, NULL);       /* queue-driven throughout */
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
    /* A typical bucket; a larger one borrows (ctx_fit). A context that cannot be had stays all zeros and ctx_fit refuses it,
       so that thread sorts its buckets with the routine that needs no context. There used to be a flag here that only the
       CALLER looked at: it sat the buckets out while every worker ran the leaf on a context with no tables. */
    for (int t = 0; t < P; t++) (void)ctx_init(&ctxs[t], ctx_typical(maxb, n, j.nbk), j.wc_on);
    PROF(for (int t = 0; t < P; t++) ctxs[t].tid = t; g_prof.busy_ms[0][LOGOS_PH_COMBINE] += prof_now() - g_prof_tp; g_prof.cpu_ms[0][LOGOS_PH_COMBINE] += prof_now() - g_prof_tp; prof_phase(LOGOS_PH_COMBINE);)
    barrier_wait(&j.bar);
    PROF(prof_stamp_t ss0 = prof_stamp();)
    do_scatter(&j);
    PROF(prof_busy_since(0, LOGOS_PH_SCATTER, ss0); double ts1 = prof_now();)
    barrier_wait(&j.bar);
    PROF(prof_wait(0, LOGOS_PH_SCATTER, prof_now() - ts1); prof_phase(LOGOS_PH_SCATTER);)
    TRACE("par: scatter done");
    do_buckets(&j, &ctxs[0]);
    par_join(th, hist_made);
    PROF(prof_phase(LOGOS_PH_BUCKETS);)
    TRACE("par: buckets done");
    barrier_destroy(&j.bar);
    for (int t = 0; t < P; t++) ctx_free(&ctxs[t]);
    scratch_drop(&sc);
#ifndef LOGOS_NO_SPLITTER_TABLE
    bt_free(&j.bt);
#endif
    free(j.mdx); j.mdx = NULL;                        /* the mode grid (mdx_t), outside the table's #ifndef */
    free(j.hv); j.hv = NULL;
    free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    PROF(prof_phase(LOGOS_PH_OTHER); prof_finish(LOGOS_ROUTE_PARTITION, t_start);)
    return;
fallback:
#ifdef LOGOS_SODO
    if (sodo_ok) free(SO.sorted);
#endif
    scratch_drop(&sc);
#ifndef LOGOS_NO_SPLITTER_TABLE
    bt_free(&j.bt);
#endif
    free(j.mdx); j.mdx = NULL;                        /* the mode grid (mdx_t), outside the table's #ifndef */
    free(j.hv); j.hv = NULL;
    free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    qsort(a, n, sizeof(double), cmp_d);
}


/* ===================== native int32 engine (no widening) ===================== */
/* Leaf: port of JS sort_fn.sort_i32 — prescan asc/desc; counting when span<=n;
 * otherwise byte-skip LSD radix on the biased uint32 key. 4-byte elements
 * throughout: half the bytes of the old widen-to-double path. */
/* ONE SCRATCH ARRAY (2026-10-22, the memory round): the bias is applied IN PLACE and the passes ping-pong between a and dst; the last
   pass reads the array that is not a (an even number of earlier passes copies the keys to dst first). src (the context's aux) is
   used only with LOGOS_RADIX_TWOBUF=1 from 4,096 elements up, the form before. rssprobe: 8 bytes of scratch per 4-byte element at
   one thread -> 4. */
static void lsd_radix_i32(int32_t *a, size_t n, uint32_t *src, uint32_t *dst, ctx_t *c) {
    if (!(n >= 4096 && getenv("LOGOS_RADIX_TWOBUF"))) src = (uint32_t *)a;
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
    if (src == (uint32_t *)a && ((a0 + a1 + a2 + a3 - 1) & 1) == 0) { memcpy(dst, src, n * sizeof(uint32_t)); uint32_t *t = src; src = dst; dst = t; }
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
        else if (nb > 1) { ctx_fit_t cf;
            if (ctx_fit(ctx, nb, &cf)) { leaf_i32(j->dst + s, nb, (uint32_t *)(j->src + s), ctx); ctx_unfit(ctx, &cf); }
            else qsort(j->dst + s, nb, sizeof(int32_t), cmp_i32); }
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
    par_run(th, P, cnti_minmax, wa, sizeof wa[0], 1);
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
        /* Static slot ranges, and each table is zeroed by the thread that owns it, so an orphan cannot be covered by a
           queue. Nothing is written to a[] before the first barrier: drop the orphan and let the whole route decline. */
        int cnt_made = par_spawn(th, Pc, cnti_body, wa, sizeof wa[0], &c.bar, NULL);
        if (cnt_made != Pc - 1) atomic_store(&c.fail, 1);
        cnti_body(&wa[0]);
        par_join(th, cnt_made);
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
/* logos_release_buffers gives the count tables back too. It left them until 2026-09-20 (make memprobe's int32 rows): up to
   the 64 MB table budget stayed held after the call whose name says otherwise. */
static void cnti_release_all(void) {
#if !defined(LOGOS_NO_COUNT_I32) && !defined(LOGOS_NO_CNTI_POOL)
    for (int t = 0; t < CNTI_POOL_MAX; t++) { free(g_cnti_tab[t]); g_cnti_tab[t] = NULL; g_cnti_cap[t] = 0; }
#endif
}
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
        if (!buf || !ctx_init(&c, n, 0)) { free(buf); qsort(a, n, sizeof(int32_t), cmp_i32); return; }   /* no ctx_free: see psort_f64 */
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
    int i32_made = par_spawn(th, P, worker_i32, wa, sizeof wa[0], &j.bar, NULL);    /* queue-driven throughout */
    do_hist_i32(&j);
    barrier_wait(&j.bar);
    for (size_t ch = 0; ch < j.nchunk; ch++) for (int b = 0; b < j.nbk; b++) j.bsize[b] += j.counts[ch * j.nbk + b];
    for (int b = 1; b < j.nbk; b++) j.bstart[b] = j.bstart[b - 1] + j.bsize[b - 1];
    for (int b = 0; b < j.nbk; b++) { size_t acc = j.bstart[b]; for (size_t ch = 0; ch < j.nchunk; ch++) { j.offs[ch * j.nbk + b] = acc; acc += j.counts[ch * j.nbk + b]; } }
    { size_t maxb = 1; for (int b = 0; b < j.nbk; b++) if (j.bsize[b] > maxb) maxb = j.bsize[b];
      for (int t = 0; t < P; t++) (void)ctx_init(&ctxs[t], ctx_typical(maxb, j.n, j.nbk), 0);      /* a failed one stays zeroed and ctx_fit refuses it: see psort_f64 */
      barrier_wait(&j.bar);
      do_scatter_i32(&j);
      barrier_wait(&j.bar);
      do_buckets_i32(&j, &ctxs[0]);
      par_join(th, i32_made);
      barrier_destroy(&j.bar);
      for (int t = 0; t < P; t++) ctx_free(&ctxs[t]); }
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
 * that cannot hold 2n). Parallel from 2026-09-20 (inplace_parallel, below): the
 * first placement pass stays on one thread, everything under it is tasks.
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
    if (n > (size_t)UINT32_MAX) { logos_sort_f64_inplace(a, n, nthreads); return 0; }   /* 32-bit counters: see logos_sort_f64. 0 = came out sorted */
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
                 size_t maxb; int seq; size_t typ; } sb_arg_t;
typedef struct { sb_arg_t *w; ctx_t *ctx; } sb_targ_t;
static void *sb_worker(void *p) {
    sb_targ_t *ta = p; sb_arg_t *w = ta->w; ctx_t *c = ta->ctx; size_t b;
    double *scratch = malloc((w->typ ? w->typ : 1) * sizeof(double));      /* a typical bucket; a larger one gets its own for as long as it is sorted */
    if (!scratch) return NULL;
    c->seq = w->seq;
    while ((b = atomic_fetch_add(w->next, 1)) < (size_t)w->nbk) {
        if (w->done[b]) continue;
        size_t s0 = w->starts[b], nb = w->starts[b + 1] - s0;
        if (nb > 1) { ctx_fit_t cf; double *big = nb > w->typ ? malloc(nb * sizeof(double)) : NULL, *sc = nb > w->typ ? big : scratch;
            if (sc && ctx_fit(c, nb, &cf)) { double *r = leaf_f64(w->a + s0, nb, sc, c); if (r != w->a + s0) memcpy(w->a + s0, r, nb * sizeof(double)); ctx_unfit(c, &cf); }
            else quicksort(w->a + s0, 0, (long)nb - 1, 2 * log2i(nb));
            free(big); }
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
    sb_arg_t w = { a, starts, nbk, done, &next, maxb, (P == 1) || one_each, ctx_typical(maxb, starts[nbk] - starts[0], nbk) };
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    sb_targ_t *ta = calloc((size_t)P, sizeof(sb_targ_t));
    int made = 0;
    if (th && ctxs && ta) { for (int t = 0; t < P; t++) { if (!ctx_init(&ctxs[t], w.typ, 0)) break; made++; } }
    for (int t = 0; t < made; t++) { ta[t].w = &w; ta[t].ctx = &ctxs[t]; }
    if (made > 0) par_run(th, made, sb_worker, ta, sizeof ta[0], 1);
    for (int t = 0; t < made; t++) ctx_free(&ctxs[t]);
    free(th); free(ctxs); free(ta);
    /* a worker that could not get a context leaves its buckets undone; finish them here, serially */
    for (int b = 0; b < nbk; b++) if (!done[b]) { size_t s0 = starts[b], nb = starts[b + 1] - s0; if (nb > 1) logos_sort_f64(a + s0, nb, 1); done[b] = 1; }
}
/* logos_twist_last(): twisted strands consumed by placements since the counts were cleared, and by which route. Plain statics: written
   on the calling thread inside relocate_try_f64 only (never on a small sort's path), one sort at a time like the pool. */
static size_t g_twist_knot = 0, g_twist_reloc = 0;
static size_t g_str_sorts;    /* logos_twist_last(25): defined with the string sort, below */
static atomic_size_t g_argsort_par;  /* logos_twist_last(42): defined with logos_argsort_f64_mt, at the end of the file */
static size_t g_str_ns, g_str_ns_gate; static int g_str_ns_last;   /* logos_twist_last(27), (-10), (-9): the log-line route, below */
static size_t g_thin_calls;   /* logos_twist_last(20): calls the thin front answered (plain: logos_sort_f64 is not reentrant anyway) */
static size_t thin_ceiling(void);     /* THIN_MAX_N, defined with the thin front below; 0 without it */
size_t logos_twist_last(int which) {
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE)
    if (which == -5) return (size_t)avx2_leaf_on();                             /* is lever C (the scan on the first partition) live? */
    if (which == 15) return atomic_exchange(&g_fuse_parts, 0);                  /* kernel calls whose first partition carried the scan */
#ifndef LOGOS_NO_LCVEC
    if (which == 17) return atomic_exchange(&g_lcvec_elems, 0);                 /* elements the low-cardinality vector count settled */
#endif
    if (which == 16) return atomic_exchange(&g_fuse_declines, 0);               /* ... that met a NaN there and declined */
#else
    if (which == -5) return 0;
#endif
    if (which == 19) return atomic_exchange(&g_wp_groups, 0);                      /* parallel groups the worker pool served */
    if (which == 20) { size_t v = g_thin_calls; g_thin_calls = 0; return v; }      /* calls the thin front answered */
    if (which == 21) { size_t v = g_trim_bytes; g_trim_bytes = 0; return v; }      /* bytes the pool trim handed back */
    if (which == 22) return atomic_exchange(&g_f32_sorts, 0);
#if defined(LOGOS_AVX2_LEAF) && !defined(LOGOS_ZERO_SIGN_UNSAFE) && !defined(LOGOS_NO_HALVES) && defined(LOGOS_SODO)
    if (which == 26) { size_t v = g_halves; g_halves = 0; return v; }           /* sorts the two-halves route finished (P = 2) */
#endif
    if (which == 27) { size_t v = g_str_ns; g_str_ns = 0; return v; }                /* string sorts the log-line route finished */
#ifdef LOGOS_HAVE_CUTS
    if (which == 29) return atomic_exchange(&g_cuts_sorts, 0);                        /* f64 sorts the cuts route finished (windows in order) */
#endif
    if (which == 30) return atomic_exchange(&g_lowdisc_fires, 0);                     /* leaf ranges the low-discrepancy probe sent past the AVX2 kernel */
    if (which == 31) return atomic_exchange(&g_sf_windows, 0);
#ifdef LOGOS_HAVE_DOMINANT
    if (which == 33) return atomic_exchange(&g_dom_sorts, 0);                         /* sorts the dominant-value route finished */
#endif                        /* profiles the founder windows settled where the 64-element prefix could not */
#ifndef LOGOS_NO_KNOT
    if (which == 32) return atomic_exchange(&g_runmerge_vecs, 0);                     /* k-way merges done in vectors (merge_runs_vec) */
    if (which == 34) return atomic_exchange(&g_half_places, 0);                       /* placements made with half a scratch (half_place) */
#endif
#ifndef LOGOS_NO_HISTFREE
    if (which == 35) return atomic_exchange(&g_hf_pooled, 0);                         /* partitions whose regions were the pool's block */
#endif
#if !defined(LOGOS_NO_LATTICE) && !defined(LOGOS_NO_MODCARD)
    if (which == 36) return atomic_exchange(&g_lat1_sorts, 0);                        /* lattice sorts whose sample showed no repeats (lat1_gate) */
#endif
    if (which == 37) return atomic_exchange(&g_lat_spilled, 0);                       /* elements the lattice route spilled and merged back (lat_spill) */
#ifndef LOGOS_NO_HISTFREE
    if (which == 41) return atomic_exchange(&g_hf_heavy, 0);                          /* histogram-free partitions that counted heavy values instead of moving them */
#endif
    if (which == 42) return atomic_exchange(&g_argsort_par, 0);                       /* argsorts that took the parallel partition (logos_argsort_f64_mt) */
    if (which == 40) return atomic_exchange(&g_mc_capped, 0);                         /* sequential hash maps that gave up at their cap (MC_HASH_SEQ_CAP) */
    if (which == 39) return atomic_exchange(&g_boosts, 0);                            /* partitions that took the unstructured thread boost (BOOST_PER) */
    if (which == 38) return atomic_exchange(&g_ns_strand, 0);                        /* parallel sorts the near-sorted route's STRAND gate turned away (strands, not strays) */
#ifndef LOGOS_NO_RLE
    if (which == 28) return atomic_exchange(&g_rle_sorts, 0);                         /* f64 sorts the runs-of-equal-values route finished */
#endif
    if (which == -9) return (size_t)g_str_ns_last;                                     /* the last string sort's log-line verdict (see str_ns_try) */
    if (which == -10) return g_str_ns_gate;                                            /* its pre-gate's strays, out of 4,096 */
#ifndef LOGOS_NO_TAILROUTE
    if (which == -11) { int v = g_tail_kind; g_tail_kind = 0; return (size_t)v; }      /* the last sequential tail merge: 1 loop, 2 gallop, 3 vector; reading clears it */
    if (which == -12) { int v = g_head_kind; g_head_kind = 0; return (size_t)v; }      /* the last head merge (an unsorted head before a sorted suffix); reading clears it */
#endif
    if (which == 25) { size_t v = g_str_sorts; g_str_sorts = 0; return v; }        /* string sorts the prefix-key path finished */
    if (which == 24) {
#ifndef LOGOS_NO_HISTFREE
        return atomic_exchange(&g_ip_parts, 0);                                    /* partitions done in place */
#else
        return 0;
#endif
    }
    if (which == 23) {
#ifdef LOGOS_AVX2_LEAF
        return atomic_exchange(&g_latvec_vecs, 0);                                  /* vectors the lattice map counted in AVX2 */
#else
        return 0;
#endif
    }                           /* sorts the float32 path finished */
    if (which == -7) {                      /* do the AVX2 kernels run here (built in, and the CPU has AVX2 and FMA)? */
#ifdef LOGOS_AVX2_LEAF
        return (size_t)avx2_leaf_on();
#else
        return 0;
#endif
    }
    if (which == -8) {                      /* is the float32 path in this build? */
#ifdef LOGOS_NO_F32
        return 0;
#else
        return 1;
#endif
    }
    if (which == -6) {                      /* is the thin front in this build? */
#ifdef LOGOS_NO_THINFRONT
        return 0;
#else
        return 1;
#endif
    }
    if (which == -13) {                     /* the thin front's ceiling as compiled (0: no thin front) */
        return thin_ceiling();
    }
    if (which == 18) return atomic_exchange(&g_mdgrid_scatters, 0);               /* partitions that scattered through the mode grid */
    if (which == -2) {                      /* is the twist in this build at all? */
#if defined(LOGOS_NO_TWIST) || defined(LOGOS_NO_RELOCATE)
        return 0;
#else
        return 1;
#endif
    }
#ifndef LOGOS_NO_KNOT
    if (which == 3) { size_t r = g_list_count; g_list_count = 0; return r; }
#ifdef LOGOS_XSS_LEAF
    if (which == 14) return atomic_exchange(&g_xss_calls, 0);                  /* ranges sorted by x86-simd-sort; reading clears it */
#endif
    if (which == -4) {                      /* is the vector two-strand merge in this build, on this CPU? */
#ifdef LOGOS_SIMD_MERGE
        return logos_simd_kind() == 2 && !getenv("LOGOS_NO_SIMD_MERGE");
#else
        return 0;
#endif
    }
    if (which == 13) return atomic_exchange(&g_simd_merges, 0);               /* two-strand merges taken by the vector kernel; reading clears it */
    if (which == 5) return atomic_exchange(&g_knot_gallops, 0);                 /* gallops made by two-strand merges; reading clears it */      /* placements through the literal linked list; reading clears it */
#endif
#ifndef LOGOS_NO_RELOCATE
    if (which == 4) { size_t r = g_kink_count; g_kink_count = 0; return r; }      /* kinks swapped back by the run detector; reading clears it */
#endif
    if (which == -3) {                      /* is the dense-integer bitmap in this build? */
#if defined(LOGOS_NO_DENSEINT) || !defined(LOGOS_SODO)
        return 0;
#else
        return 1;
#endif
    }
    if (which == 6) { size_t r = g_dense_count; g_dense_count = 0; return r; }   /* arrays placed by the dense-integer bitmap; reading clears it */
    if (which == -4) {                      /* is the loop-back in this build? */
#ifdef LOGOS_NO_LOOPBACK
        return 0;
#else
        return 1;
#endif
    }
#ifndef LOGOS_NO_LOOPBACK
    if (which == 7) { size_t r = g_loop_count; g_loop_count = 0; return r; }     /* sorts the loop-back rotated first; reading clears it */
#endif
#if !defined(LOGOS_NO_SEAM) || !defined(LOGOS_NO_WAVE)
    if (which == 8) { size_t r = g_wave_count; g_wave_count = 0; return r; }     /* arrays the wave route placed */
    if (which == 9) { size_t r = g_seam_count; g_seam_count = 0; return r; }     /* sorts the seam route rotated first */
#endif
#ifndef LOGOS_NO_RELOCATE
    if (which == 10) { size_t r = g_stray_count; g_stray_count = 0; return r; }  /* strays lifted out of strands by routes that completed */
    if (which == 12) { size_t r = g_onerun_count; g_onerun_count = 0; return r; } /* sorts finished as a single strand */
#endif
    if (which < 0) { g_twist_knot = 0; g_twist_reloc = 0; return 0; }
    return which ? g_twist_reloc : g_twist_knot;
}
int logos_reach_last(void) {
#ifndef LOGOS_NO_REACH
    return g_reach_last;
#else
    return LOGOS_REACH_NOT_TRIED;
#endif
}
/* THE THIN FRONT (2026-10-08, open direction 0g, make callbench). A small call used to enter the whole dispatcher: the prologue of
 * a 28 KB frame probed page by page, the buffer pool's lock, a context allocated, and from 25 elements a relocation plan built and
 * declined, before an insertion sort or a network did the work (FINDINGS 3ad: 72 doubles took 1.9 us, 96 took 1.0). Up to
 * THIN_MAX_N elements the call is answered here, in a function with a small frame: the prescan's two exits (ascending: done;
 * descending: one reversal), the proven networks for 4 to 16, and from 17 the AVX2 quicksort, whose vector networks beat insertion
 * sort 1.8-3.6x from 17 to 24 elements (make callbench); without AVX2, or when the kernel declines on a NaN, insertion sort to
 * INSERTION_SORT_THRESHOLD and the scalar introsort above it. Each is a sort the leaf already uses on ranges this short, so a
 * NaN-free input comes back sorted and any input as a permutation of its bits (harness section "thin front"). Random 3-100
 * elements 3.5-15x, sorted 2-100 1.2-3.2x against the dispatcher (callbench.c). -DLOGOS_NO_THINFRONT restores the old entry.
 * 2026-10-20: the ceiling 128 -> 2047, with two additions measured for it (thinbench): the dispatcher's vector prescan from
 * PRESCAN_FAST_MIN (the scalar loop lost sorted and reversed input 0.35-0.8 from 256), and the kink windows below. */
#ifndef THIN_MAX_N
#define THIN_MAX_N 2047                 /* was 128. edgeprobe (2026-10-20): ONE element past 128 cost 1.2-4.3x more per element on all sixteen
                                           shapes, at one thread and at 14 (the dispatcher's frame, pool and context, then the same kernels).
                                           thinbench, batches of distinct instances, the ceiling moved at run time: with the vector prescan
                                           and the kink windows below, the thin front beats the dispatcher at every size from 129 to 2047 on
                                           fourteen shapes -- uniform 2.32x at 129 to 1.11x at 2047, kinks 1.6-2.3x, near-sorted to 3.0x,
                                           a 95% prefix to 3.7x, zipf integers 2.0-3.1x, few distinct values 0.94-1.33. It stops below
                                           FLASH_SORT_THRESHOLD, where the leaf's own choice of kernel begins. */
#endif
#ifndef THIN_KINK_MIN_N
#define THIN_KINK_MIN_N 64              /* the kink windows need room for eight windows of 32 pairs and their neighbours */
#endif
static size_t thin_ceiling(void) {
#ifdef LOGOS_NO_THINFRONT
    return 0;
#else
    return THIN_MAX_N;
#endif
}
#ifdef LOGOS_THIN_HOOK                    /* make thinbench: the ceiling moved at run time, in a bench copy of the engine only */
static size_t g_thin_max = THIN_MAX_N;
void logos_thin_max_hook(size_t m) { g_thin_max = m; }
#define THIN_LIMIT g_thin_max
#else
#define THIN_LIMIT THIN_MAX_N
#endif
static __attribute__((noinline)) void thin_core_f64(double *a, size_t n) {   /* no global state: the batch entry's workers share it */
    int asc = 1, desc = 1;
    if (n >= PRESCAN_FAST_MIN) { pscan_t ps = prescan_kind(a, n, prescan_cpu()); asc = ps.asc; desc = ps.desc; }   /* the dispatcher's vector scan */
    else for (size_t i = 1; i < n && (asc || desc); i++) { if (a[i] < a[i - 1]) asc = 0; if (a[i] > a[i - 1]) desc = 0; }
    if (asc) return;
    if (desc) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; } return; }
    if (n >= 4 && n <= 16 && !g_no_smallnet) { net_sort_small(a, (long)n); return; }
    if (n >= THIN_KINK_MIN_N) {
        /* KINKS: a sorted run with a few adjacent pairs swapped is the one shape the dispatcher beat the thin front on (thinbench:
           0.52-0.87 from 384 to 2047, the leaf's insertion against a quicksort). Eight windows of 32 adjacent pairs: when every
           descent they see is kink-shaped (swapped back, the pair fits between its neighbours -- the run detector's own test), a
           budgeted insertion sort finishes it, and gives up to the quicksort past 8n moves. Random swaps displace elements far and
           fail the shape, so near-sorted data keeps the quicksort (the dispatcher's insertion there was 2.9x SLOWER, thinbench). */
        /* The first descent that is NOT kink-shaped ends the look: random data shows one within a few pairs. Counting every descent
           cost the thin front 3x at 129 random elements (thinbench), half the pairs mispredicting a branch the sort never needed. */
        size_t d = 0; int clean = 1;
        for (int w = 0; w < 8 && clean; w++) {
            size_t lo = 2 + (n - 36) * (size_t)w / 8;
            for (size_t i = lo; i < lo + 32; i++) if (a[i] < a[i - 1]) { if (!(a[i] >= a[i - 2] && a[i - 1] <= a[i + 1])) { clean = 0; break; } d++; }
        }
        if (clean && d > 0 && insertion_sort_momentum(a, 0, (long)n - 1)) return;
    }
#ifdef LOGOS_AVX2_LEAF
    if (n > 16 && avx2_leaf_on() && avx2_qsort_f64(a, n)) return;   /* from 17 its vector networks beat insertion sort 4x */
#endif
    if (n <= INSERTION_SORT_THRESHOLD) { insertion_sort(a, 0, (long)n - 1); return; }
    quicksort(a, 0, (long)n - 1, 2 * log2i(n));
}
#ifndef LOGOS_NO_THINFRONT
static inline void thin_sort_f64(double *a, size_t n) { g_thin_calls++; thin_core_f64(a, n); }   /* one call deep, as before the split */
#endif
void logos_sort_f64(double *a, size_t n, int nthreads) {
    /* Nothing to sort: return before the thread-local read below, the call into the dispatcher and the nine pages its prologue
       probes (its frame is 36 KB, and stack-clash protection touches every page of a frame on every call). A call with 0 or 1
       elements went from 8.5 to 2.9 ns (make callbench, both copy orientations: 0.34 and 0.25). Up to THIN_MAX_N elements the
       thin front answers next (thin_sort_f64). */
    if (n < 2) return;
#ifndef LOGOS_NO_THINFRONT
    if (n <= THIN_LIMIT) { thin_sort_f64(a, n); return; }    /* the thin front: no dispatcher frame, no pool, no context */
#endif
    /* The buffered engine counts in 32 bits (the context's aux array, the byte histograms), so above 2^32 - 1 elements a
       count can wrap. The API takes a size_t and an outside review rightly asked where the bound was. No machine here can
       hold such an array (34 GB of doubles), so this line is correct by inspection, not by test: the strict in-place
       radix counts in size_t throughout and takes those arrays. */
    if (n > (size_t)UINT32_MAX) { logos_sort_f64_inplace(a, n, nthreads); return; }
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;              /* one top-level sort at a time: the bundle cursor restarts */
#endif
    int top = ns_depth == 0;
    if (top) g_trim_ep++;                           /* the pool trim's call count (trim_note, trim_idle) */
    psort_f64(a, n, nthreads, NULL, NULL, NULL, NULL);
    if (top) trim_idle();
}
/* THE BATCH ENTRY (2026-10-08, make batchbench). Many independent arrays laid end to end, segment k = a[off[k] .. off[k+1]).
 * A loop of logos_sort_f64 calls pays the dispatcher once per segment (from 129 elements: the pool's lock, a context, the shared
 * sample, a relocation plan) and runs every segment under 100,000 elements on ONE thread whatever nthreads says, since a parallel
 * call there is mostly fixed cost. Here: segments up to THIN_MAX_N take the thin front's kernels; longer ones the prescan's two
 * exits, the low-cardinality route from 4096 elements (as the dispatcher has it: without it five-valued segments of 10,000 ran
 * 3.5x SLOWER than the loop, make batchbench) and then the leaf (leaf_f64), each worker with a PRIVATE context and buffer, so
 * nothing pooled or global is shared; workers claim groups of segments from one counter. How long a segment the workers take
 * depends on whether there ARE workers: with several, everything under 2 x MIN_ELEMS_PER_THREAD (the engine would sort those on
 * one thread anyway, so nothing is given up for the parallelism); with one, only up to BATCH_SEQ_MAX, because at one thread the
 * gain is only the dispatcher's setup and the whole engine has routes the leaf lacks. Longer segments keep the whole engine (its
 * routes, its own threads) and are sorted after the parallel phase, on the calling thread, one at a time. A worker whose context cannot be
 * allocated sorts its segments with the AVX2 kernel or the scalar introsort, which need none; a thread that cannot be created is
 * run on the caller (par_run). Output: every segment sorted as logos_sort_f64 would sort it (with a NaN: a permutation). */
#ifndef BATCH_LEAF_MAX
#define BATCH_LEAF_MAX (2 * MIN_ELEMS_PER_THREAD - 1)   /* with several workers */
#endif
#ifndef BATCH_SEQ_MAX
#define BATCH_SEQ_MAX 4096                               /* with one */
#endif
#define BATCH_MIN_PER_THREAD 65536     /* elements in pure segments per extra worker: below it a worker costs more than it sorts */
static void thin_core_f64(double *a, size_t n);
typedef struct { double *a; const size_t *off; size_t nseg, grain, ctx_cap, lmax; atomic_size_t next; } batch_t;
typedef struct { batch_t *b; } batch_arg_t;
static void batch_one(double *s, size_t L, ctx_t *c, double *buf, int have) {
    if (L < 2) return;
    if (L <= THIN_MAX_N) { thin_core_f64(s, L); return; }
    int asc = 1, desc = 1;
    for (size_t i = 1; i < L && (asc || desc); i++) { if (s[i] < s[i - 1]) asc = 0; if (s[i] > s[i - 1]) desc = 0; }
    if (asc) return;
    if (desc) { for (size_t l = 0, r = L - 1; l < r; l++, r--) { double t = s[l]; s[l] = s[r]; s[r] = t; } return; }
    if (L >= 4096 && lowcard_try_f64(s, L, 1, NULL, 0)) return;   /* one thread: malloc and atomics only, no pool, no context */
    if (have) { double *r = leaf_f64(s, L, buf, c); if (r != s) memcpy(s, r, L * sizeof(double)); return; }
#ifdef LOGOS_AVX2_LEAF
    if (avx2_leaf_on() && avx2_qsort_f64(s, L)) return;          /* no context: the kernel needs none */
#endif
    quicksort(s, 0, (long)L - 1, 2 * log2i(L));
}
static void *batch_worker(void *p) {
    batch_t *b = ((batch_arg_t *)p)->b;
    ctx_t c; double *buf = NULL; int have = 0, tried = 0;
    for (;;) {
        size_t k0 = atomic_fetch_add_explicit(&b->next, b->grain, memory_order_relaxed);
        if (k0 >= b->nseg) break;
        size_t k1 = k0 + b->grain < b->nseg ? k0 + b->grain : b->nseg;
        for (size_t k = k0; k < k1; k++) {
            size_t L = b->off[k + 1] - b->off[k];
            if (L > b->lmax) continue;                            /* the caller's, after this phase */
            if (L > THIN_MAX_N && !tried) {                       /* the context, at the first segment that wants one */
                tried = 1;
                buf = malloc(b->ctx_cap * sizeof(double));
                have = buf && ctx_alloc(&c, b->ctx_cap, 0);
                if (have) c.seq = 1; else { free(buf); buf = NULL; }
            }
            batch_one(b->a + b->off[k], L, &c, buf, have);
        }
    }
    if (have) ctx_release(&c);
    free(buf);
    return NULL;
}
void logos_sort_f64_batch(double *a, const size_t *off, size_t nseg, int nthreads) {
    if (!a || !off || nseg == 0) return;
    int P = nthreads > 1 ? nthreads : 1; if (P > 256) P = 256;
    /* one pass over the offsets sizes both limits: with a million short segments each pass is a few percent of the call */
    size_t pure_hi = 0, cap_hi = 0, big_hi = 0, pure_lo = 0, cap_lo = 0, big_lo = 0;
    for (size_t k = 0; k < nseg; k++) {
        size_t L = off[k + 1] - off[k];
        if (L > BATCH_LEAF_MAX) { big_hi++; big_lo++; continue; }
        pure_hi += L; if (L > THIN_MAX_N && L > cap_hi) cap_hi = L;
        if (L > BATCH_SEQ_MAX) { big_lo++; continue; }
        pure_lo += L; if (L > THIN_MAX_N && L > cap_lo) cap_lo = L;
    }
    if ((size_t)P > pure_hi / BATCH_MIN_PER_THREAD) P = (int)(pure_hi / BATCH_MIN_PER_THREAD);
    if (P < 1) P = 1;
    /* one worker: only what the dispatcher would sort the same way */
    size_t lmax = P > 1 ? BATCH_LEAF_MAX : BATCH_SEQ_MAX, pure = P > 1 ? pure_hi : pure_lo, cap = P > 1 ? cap_hi : cap_lo, big = P > 1 ? big_hi : big_lo;
    if (pure) {
        batch_t b; b.a = a; b.off = off; b.nseg = nseg; b.ctx_cap = cap; b.lmax = lmax; atomic_init(&b.next, 0);
        size_t avg = pure / nseg + 1;
        b.grain = 16384 / avg; if (b.grain < 1) b.grain = 1;     /* about 16k elements per claim */
#ifdef LOGOS_AVX2_LEAF
        (void)avx2_leaf_on();                                   /* its cached flag is set here, before any worker reads it */
#endif
        if (P == 1) { batch_arg_t w = { &b }; batch_worker(&w); }
        else {
            pthread_t th[256]; batch_arg_t wa[256];
            for (int t = 0; t < P; t++) wa[t].b = &b;
            par_run(th, P, batch_worker, wa, sizeof wa[0], 1);
        }
    }
    if (big) for (size_t k = 0; k < nseg; k++) {
        size_t L = off[k + 1] - off[k];
        if (L > lmax) logos_sort_f64(a + off[k], L, nthreads);
    }
}

/* ==== STRINGS: 8-byte prefix keys through the engine's sample-sort machinery (2026-10-09, make stringbench) ====
 * logos_sort_strings (logos_extra.c) is a plain multikey quicksort on one thread: every comparison chases two pointers. Here the
 * first 8 bytes of each string become one 64-bit key, big-endian and zero-padded past the NUL, so comparing keys IS comparing the
 * strings' first 8 bytes the way strcmp does (unsigned bytes, a shorter string first). (key, pointer) pairs are sorted with no
 * pointer chased: an MSD radix that skips constant bytes on one thread, a sample sort into P x 16 buckets in parallel (splitters
 * are sample quantiles, as the numeric partition's are). A run of EQUAL keys whose last byte is not 0 is a group of strings sharing
 * 8 more bytes: it is sorted the same way 8 bytes deeper; equal keys ending in 0 are identical strings and are done. A range whose
 * keys are ALL equal skips straight to the next 8 bytes (URLs, paths, timestamps: the shared prefix costs one key pass per 8 bytes).
 * Every scratch array is indexed by position, so disjoint groups are sorted by different threads with no allocation and no
 * sharing. Ranges of STR_SMALL or fewer take the multikey quicksort from their depth. A failed allocation sorts the range with
 * the multikey quicksort (slower, never wrong); a thread that cannot be created runs on the caller (par_run). */
#ifndef STR_SMALL
#define STR_SMALL 32
#endif
#ifndef STR_PAR_MIN
#define STR_PAR_MIN 65536                 /* below this a range is sorted on one thread */
#endif
#ifndef STR_MIN_PER_THREAD
#define STR_MIN_PER_THREAD 32768
#endif
typedef uint64_t __attribute__((may_alias, aligned(1))) str_u64u;
/* the 8 bytes from s, big-endian, every byte from the first NUL on zero. One load when the 8 bytes cannot leave s's page: the
   bytes past the NUL are read and discarded (strlen's own trick), which is why the sanitizer is told to look away here. */
__attribute__((no_sanitize_address)) static inline uint64_t str_key8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (((uintptr_t)p & 4095u) <= 4088u) {
        uint64_t v = *(const str_u64u *)p;
        uint64_t z = (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;   /* the lowest set bit marks the first NUL exactly */
        if (z) { unsigned sh = (unsigned)__builtin_ctzll(z) & ~7u; v &= sh ? ((1ULL << sh) - 1) : 0; }
        return __builtin_bswap64(v);
    }
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) { uint64_t c = p[i]; k |= c << (56 - 8 * i); if (!c) break; }
    return k;
}
static void str_mkqs(const char **a, long lo, long hi, long d) {           /* Bentley-Sedgewick from depth d (logos_extra.c's, with a depth) */
    while (hi - lo > 16) {
        int pv = (unsigned char)a[(lo + hi) / 2][d];
        long lt = lo, gt = hi, i = lo;
        while (i <= gt) {
            int c = (unsigned char)a[i][d];
            if (c < pv) { const char *t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
            else if (c > pv) { const char *t = a[gt]; a[gt] = a[i]; a[i] = t; gt--; }
            else i++;
        }
        str_mkqs(a, lo, lt - 1, d);
        str_mkqs(a, gt + 1, hi, d);
        if (pv == 0) return;
        lo = lt; hi = gt; d++;
    }
    for (long i = lo + 1; i <= hi; i++) { const char *t = a[i]; long j = i; while (j > lo && strcmp(a[j - 1] + d, t + d) > 0) { a[j] = a[j - 1]; j--; } a[j] = t; }
}
/* KEYS ALL THE WAY DOWN (2026-10-19). A range of STR_SMALL strings or fewer went to the multikey quicksort above, one BYTE per
   level, so 32 log lines walked their shared "2026-10-09T" one partition pass at a time (make scale-full: 3.4 us against IPS4o's
   1.1). Here such a range takes the big path's own currency on the stack: 8-byte keys, a shared block skipped in one step, an
   insertion sort on the keys, and each group of equal keys 8 bytes deeper (each strictly smaller than its range, so the depth is
   under STR_SMALL). Laddered against a plain insertion sort with strcmp at 2 to 8 strings (31 pairs, five shapes): keys tie or win
   at every size, so keys from two strings up. MEASURED (strsmallbench, 21-31 pairs; stringbench STRING_AB, 9 reps): small calls of
   8-32 strings 1.36-2.77x on uuids, words, urls, logs and paths; inside 2M-string sorts urls 1.10 at 1 and 14 threads, everything
   else 0.98-1.06. LOGOS_STR_SMALLMKQS=1: the multikey quicksort for the groups inside a sort of more than STR_SMALL (read once per
   such sort; a call of STR_SMALL or fewer always takes the keys, so it reads no environment). */
static int g_str_smallmkqs = 0;
static void str_small_keys(const char **a, size_t n, size_t d) {
    uint64_t k[STR_SMALL];
    for (;;) {
        uint64_t mn = ~0ULL, mx = 0;
        for (size_t i = 0; i < n; i++) { uint64_t x = str_key8(a[i] + d); k[i] = x; if (x < mn) mn = x; if (x > mx) mx = x; }
        if (mn == mx) { if ((mn & 0xff) == 0) return; d += 8; continue; }    /* one shared block, or identical strings */
        for (size_t i = 1; i < n; i++) { uint64_t x = k[i]; const char *p = a[i]; size_t j = i; while (j > 0 && k[j - 1] > x) { k[j] = k[j - 1]; a[j] = a[j - 1]; j--; } k[j] = x; a[j] = p; }
        for (size_t i = 0; i < n; ) {
            size_t j = i + 1; while (j < n && k[j] == k[i]) j++;
            if (j - i > 1 && (k[i] & 0xff)) str_small_keys(a + i, j - i, d + 8);
            i = j;
        }
        return;
    }
}
/* (key, pointer) pairs: data in (k, v); the result must land in (k, v) when out2 == 0, in (k2, v2) when out2 == 1 */
static void str_msd(uint64_t *k, const char **v, uint64_t *k2, const char **v2, size_t n, int out2) {
    if (n <= 48) {
        for (size_t i = 1; i < n; i++) { uint64_t x = k[i]; const char *p = v[i]; size_t j = i; while (j > 0 && k[j - 1] > x) { k[j] = k[j - 1]; v[j] = v[j - 1]; j--; } k[j] = x; v[j] = p; }
        if (out2) { memcpy(k2, k, n * sizeof *k); memcpy(v2, v, n * sizeof *v); }
        return;
    }
    uint64_t o = 0, an = ~0ULL;
    for (size_t i = 0; i < n; i++) { o |= k[i]; an &= k[i]; }
    uint64_t diff = o ^ an;
    if (!diff) { if (out2) { memcpy(k2, k, n * sizeof *k); memcpy(v2, v, n * sizeof *v); } return; }   /* all equal */
    int sh = (63 - __builtin_clzll(diff)) & ~7;                          /* the most significant byte that varies */
    size_t c[257]; memset(c, 0, sizeof c);
    for (size_t i = 0; i < n; i++) c[((k[i] >> sh) & 0xff) + 1]++;
    for (int b = 0; b < 256; b++) c[b + 1] += c[b];
    size_t st[257]; memcpy(st, c, sizeof st);
    for (size_t i = 0; i < n; i++) { size_t pos = c[(k[i] >> sh) & 0xff]++; k2[pos] = k[i]; v2[pos] = v[i]; }
    for (int b = 0; b < 256; b++) { size_t s = st[b], m = st[b + 1] - s; if (m) str_msd(k2 + s, v2 + s, k + s, v + s, m, !out2); }
}
typedef struct { const char **a; uint64_t *k, *k2; const char **v2; uint16_t *bid; } str_ws_t;
typedef struct { str_ws_t *w; size_t lo, hi, d; uint64_t mn, mx; } str_keys_arg_t;
static void *str_keys_worker(void *p) {
    str_keys_arg_t *q = p; const char **a = q->w->a; uint64_t *k = q->w->k, mn = ~0ULL, mx = 0; size_t d = q->d;
    for (size_t i = q->lo; i < q->hi; i++) { if (i + 8 < q->hi) __builtin_prefetch(a[i + 8] + d); uint64_t x = str_key8(a[i] + d); k[i] = x; if (x < mn) mn = x; if (x > mx) mx = x; }
    q->mn = mn; q->mx = mx; return NULL;
}
__attribute__((noinline)) static void str_keys(str_ws_t *w, size_t lo, size_t hi, size_t d, int P, uint64_t *mn, uint64_t *mx) {
    str_keys_arg_t one = { w, lo, hi, d, 0, 0 };
    if (P <= 1) { str_keys_worker(&one); *mn = one.mn; *mx = one.mx; return; }
    str_keys_arg_t qa[256]; pthread_t th[256]; size_t n = hi - lo;
    for (int t = 0; t < P; t++) { qa[t] = one; qa[t].lo = lo + n * (size_t)t / (size_t)P; qa[t].hi = lo + n * (size_t)(t + 1) / (size_t)P; }
    par_run(th, P, str_keys_worker, qa, sizeof qa[0], 1);
    *mn = ~0ULL; *mx = 0;
    for (int t = 0; t < P; t++) { if (qa[t].mn < *mn) *mn = qa[t].mn; if (qa[t].mx > *mx) *mx = qa[t].mx; }
}
/* the parallel sample sort of (key, pointer) pairs over [lo, hi): classify, count, scatter to (k2, v2), buckets sorted back */
typedef struct { str_ws_t *w; size_t lo, hi; int nb; const uint64_t *spl; size_t *cnt; size_t *ofs; const size_t *bst; atomic_size_t *next; int phase; } str_part_arg_t;
static inline int str_bucket(const uint64_t *spl, int nb, uint64_t key) {   /* spl: nb - 1 sorted splitters; nb a power of two */
    int i = 0;
    for (int h = nb >> 1; h; h >>= 1) i += (spl[i + h - 1] <= key) ? h : 0;
    return i;
}
static void *str_part_worker(void *p) {
    str_part_arg_t *q = p; str_ws_t *w = q->w;
    if (q->phase == 0) {                                                 /* classify and count this thread's slice */
        for (size_t i = q->lo; i < q->hi; i++) { int b = str_bucket(q->spl, q->nb, w->k[i]); w->bid[i] = (uint16_t)b; q->cnt[b]++; }
    } else if (q->phase == 1) {                                          /* scatter it */
        for (size_t i = q->lo; i < q->hi; i++) { size_t pos = q->ofs[w->bid[i]]++; w->k2[pos] = w->k[i]; w->v2[pos] = w->a[i]; }
    } else {                                                             /* buckets, claimed: sorted from (k2, v2) back into (k, a) */
        size_t b;
        while ((b = atomic_fetch_add_explicit(q->next, 1, memory_order_relaxed)) < (size_t)q->nb) {
            size_t s = q->bst[b], m = q->bst[b + 1] - s;
            if (m) str_msd(w->k2 + s, w->v2 + s, w->k + s, w->a + s, m, 1);
        }
    }
    return NULL;
}
static int str_kv_par(str_ws_t *w, size_t lo, size_t hi, int P) {     /* 0: no memory, the caller sorts on one thread */
    size_t n = hi - lo;
    int nb = 64; while (nb < P * 16 && nb < 1024) nb <<= 1;
    size_t m = (size_t)nb * 32; if (m > n) m = n;
    uint64_t *sk = malloc(m * sizeof *sk), *sk2 = malloc(m * sizeof *sk2), *spl = malloc((size_t)nb * sizeof *spl);
    const char **sv = malloc(m * sizeof *sv), **sv2 = malloc(m * sizeof *sv2);
    size_t *cnt = calloc((size_t)P * (size_t)nb, sizeof *cnt), *bst = malloc(((size_t)nb + 1) * sizeof *bst);
    str_part_arg_t *qa = malloc((size_t)P * sizeof *qa);
    int ok = sk && sk2 && spl && sv && sv2 && cnt && bst && qa;
    if (ok) {
        for (size_t j = 0; j < m; j++) { size_t i = lo + (size_t)((double)n * fmod((double)j * 0.6180339887498949, 1.0)); if (i >= hi) i = hi - 1; sk[j] = w->k[i]; sv[j] = NULL; }
        str_msd(sk, sv, sk2, sv2, m, 0);                                 /* the sample, sorted by the same radix */
        for (int j = 0; j < nb - 1; j++) spl[j] = sk[(size_t)(j + 1) * m / (size_t)nb];
        pthread_t th[256]; atomic_size_t next; atomic_init(&next, 0);
        for (int t = 0; t < P; t++) { str_part_arg_t a0 = { w, lo + n * (size_t)t / (size_t)P, lo + n * (size_t)(t + 1) / (size_t)P, nb, spl, cnt + (size_t)t * (size_t)nb, NULL, bst, &next, 0 }; qa[t] = a0; }
        par_run(th, P, str_part_worker, qa, sizeof qa[0], 1);
        size_t pos = lo;                                                 /* bucket-major, thread-minor: each thread's offsets */
        for (int b = 0; b < nb; b++) { bst[b] = pos; for (int t = 0; t < P; t++) { size_t c = cnt[(size_t)t * (size_t)nb + b]; cnt[(size_t)t * (size_t)nb + b] = pos; pos += c; } }
        bst[nb] = pos;
        for (int t = 0; t < P; t++) { qa[t].ofs = cnt + (size_t)t * (size_t)nb; qa[t].phase = 1; }
        par_run(th, P, str_part_worker, qa, sizeof qa[0], 1);
        for (int t = 0; t < P; t++) qa[t].phase = 2;
        par_run(th, P, str_part_worker, qa, sizeof qa[0], 1);
    }
    free(sk); free(sk2); free(spl); free(sv); free(sv2); free(cnt); free(bst); free(qa);
    return ok;
}
static void str_level(str_ws_t *w, size_t lo, size_t hi, size_t d, int P);
static int g_str_seqbig = 0;   /* LOGOS_STR_SEQBIG=1: every group of STR_PAR_MIN or more in turn with every thread, as first written (the A/B) */
static size_t g_str_par_min = STR_PAR_MIN, g_str_per_thread = STR_MIN_PER_THREAD;   /* LOGOS_STR_PARMIN / LOGOS_STR_PERTHREAD: the ladder's A/B */
typedef struct { str_ws_t *w; const size_t *rs; size_t nr; size_t d; atomic_size_t *next; } str_runs_arg_t;
static void *str_runs_worker(void *p) {
    str_runs_arg_t *q = p; size_t r;
    while ((r = atomic_fetch_add_explicit(q->next, 1, memory_order_relaxed)) < q->nr) str_level(q->w, q->rs[2 * r], q->rs[2 * r + 1], q->d, 1);
    return NULL;
}
/* One range at depth d, until it is sorted. RECURSION IS BOUNDED BY log2 n: every group of equal keys but the LARGEST is sorted by
   a recursive call (each such group is at most half its range), and the largest continues in this loop 8 bytes deeper -- the
   multikey quicksort's rule for its equal partition. Without it, strings that each share one 8-byte block more than the last
   recursed once per string, with a 15.7 KB frame; the parallel branch's arrays now live in str_level_par, which is not inlined. */
static void str_level_par(str_ws_t *w, size_t lo, size_t hi, size_t *pd, int P, int Pn, size_t *nlo, size_t *nhi);
/* GROUPS IN PARALLEL BELOW THE PARALLEL LEVEL (2026-10-11, make scale-full's string panels). Under g_str_par_min (65536) a range
   was sorted on one thread, and at 10k-64k long-prefix strings lost to __gnu_parallel::sort (urls at 10k: 1.59 ms against 0.74).
   Lowering the thresholds measured MIXED (logs at 262k 2.86x slower: a parallel level's sample and phases do not shrink with
   the range). The work on long-prefix data is in the groups below the first level, and those need no parallel sample sort: the
   level is sorted on one thread, its LARGE groups (STR_GROUP_MIN_LEN or more) are handed to the workers and the small ones stay on
   this thread. Only with at least two large groups holding STR_GROUP_PAR_MIN / 2 strings between them; LOGOS_STR_GROUPPAR=0 turns
   it off. Handing EVERY group out (the first cut) made logs 0.66-0.85 and words 0.80-0.91 at 10k-60k: thousands of groups of two
   or three (lines in one millisecond, identical long words) each paid a contended claim and a call for nanoseconds of work.
   MEASURED MIXED AND SHIPPED OFF (LOGOS_STR_GROUPPAR=1 turns it on), paired at 14 threads, 21 reps (STRING_AB=1): with the size gate,
   urls 1.01 / 1.22 / 1.38 at 10k / 32k / 60k, but logs 0.90-0.92 and words 0.81-0.95 -- waking the workers costs more than the
   large groups hold when they are identical strings (one frequent long word), which finish the moment they are looked at. Without
   the gate urls read 1.51-1.88 and logs 0.66-0.85. Logos still trails __gnu_parallel::sort on long-prefix strings at 10k-64k with
   threads; the lever is a cheaper hand-off (or a work estimate per group), not a lower threshold (that ladder: logs at 262k 2.86x
   slower). Returns 1 when it sorted the groups, 0 to leave them all to the caller. */
#ifndef STR_GROUP_PAR_MIN
#define STR_GROUP_PAR_MIN 8192
#endif
#ifndef STR_GROUP_MIN_LEN
#define STR_GROUP_MIN_LEN 256
#endif
static int g_str_grouppar = 0;
__attribute__((noinline)) static int str_groups_par(str_ws_t *w, size_t lo, size_t hi, size_t d, int P) {
    size_t cap = 256, nr = 0, tot = 0, *rs = malloc(cap * 2 * sizeof *rs);
    if (!rs) return 0;
    for (size_t i = lo; i < hi; ) {
        size_t j = i + 1; uint64_t x = w->k[i];
        while (j < hi && w->k[j] == x) j++;
        if (j - i >= STR_GROUP_MIN_LEN && (x & 0xff)) {                 /* the large groups only: the small ones stay on this thread */
            if (nr == cap) { size_t *t = realloc(rs, cap * 4 * sizeof *rs); if (!t) { free(rs); return 0; } rs = t; cap *= 2; }   /* nothing sorted yet: the caller does it */
            rs[2 * nr] = i; rs[2 * nr + 1] = j; nr++; tot += j - i;
        }
        i = j;
    }
    if (nr < 2 || tot < STR_GROUP_PAR_MIN / 2) { free(rs); return 0; }
    /* the small groups first, on this thread, while every key is still the one this level sorted on: once the workers start,
       the large groups' keys are overwritten 8 bytes deeper, and a scan after them would find false groups inside them */
    for (size_t i = lo; i < hi; ) {
        size_t j = i + 1; uint64_t x = w->k[i];
        while (j < hi && w->k[j] == x) j++;
        if (j - i > 1 && j - i < STR_GROUP_MIN_LEN && (x & 0xff)) str_level(w, i, j, d + 8, 1);
        i = j;
    }
    size_t m = 0;                                                        /* the groups of 1024 and more to the front: claimed first */
    for (size_t r = 0; r < nr; r++) if (rs[2 * r + 1] - rs[2 * r] >= 1024) {
        size_t a0 = rs[2 * r], a1 = rs[2 * r + 1]; rs[2 * r] = rs[2 * m]; rs[2 * r + 1] = rs[2 * m + 1]; rs[2 * m] = a0; rs[2 * m + 1] = a1; m++;
    }
    int Pr = P; if ((size_t)Pr > nr) Pr = (int)nr;
    pthread_t th[256]; str_runs_arg_t ra[256]; atomic_size_t next; atomic_init(&next, 0);
    for (int t = 0; t < Pr; t++) { str_runs_arg_t a0 = { w, rs, nr, d + 8, &next }; ra[t] = a0; }
    par_run(th, Pr, str_runs_worker, ra, sizeof ra[0], 1);
    free(rs);
    return 1;
}
static void str_level(str_ws_t *w, size_t lo, size_t hi, size_t d, int P) {
    for (;;) {
        size_t n = hi - lo;
        if (n <= STR_SMALL) { if (n > 1) { if (g_str_smallmkqs) str_mkqs(w->a, (long)lo, (long)hi - 1, (long)d); else str_small_keys(w->a + lo, n, d); } return; }
        int Pn = P;
        if (n < g_str_par_min) Pn = 1;
        else if ((size_t)Pn > n / g_str_per_thread) Pn = (int)(n / g_str_per_thread);
        if (Pn < 1) Pn = 1;
        if (Pn > 1) {                                                    /* the parallel level hands back the group to go on with */
            size_t nlo = 0, nhi = 0;
            str_level_par(w, lo, hi, &d, P, Pn, &nlo, &nhi);
            if (nhi - nlo < 2) return;
            lo = nlo; hi = nhi; d += 8; continue;
        }
        str_keys_arg_t one = { w, lo, hi, d, 0, 0 };
        str_keys_worker(&one);
        if (one.mn == one.mx) { if ((one.mn & 0xff) == 0) return; d += 8; continue; }   /* one shared 8-byte block (or identical strings) */
        str_msd(w->k + lo, w->a + lo, w->k2 + lo, w->v2 + lo, n, 0);
        if (P > 1 && n >= STR_GROUP_PAR_MIN && g_str_grouppar && str_groups_par(w, lo, hi, d, P)) return;   /* the groups, on the workers */
        size_t blo = 0, bhi = 0;                                         /* the largest group: this loop's next range */
        for (size_t i = lo; i < hi; ) {
            size_t j = i + 1; uint64_t x = w->k[i];
            while (j < hi && w->k[j] == x) j++;
            if (j - i > 1 && (x & 0xff)) {
                if (j - i > bhi - blo) { size_t ol = blo, oh = bhi; blo = i; bhi = j; if (oh - ol > 1) str_level(w, ol, oh, d + 8, 1); }
                else str_level(w, i, j, d + 8, 1);
            }
            i = j;
        }
        if (bhi - blo < 2) return;
        lo = blo; hi = bhi; d += 8;
    }
}
/* the parallel half of a level: keys (skipping shared blocks: *pd comes back as the depth the range was sorted at), the sample
   sort, the groups handed out. Not inlined: its arrays (threads, arguments) are kilobytes and str_level is recursive. *nlo, *nhi:
   the one group the caller goes on with, 8 bytes deeper (none: equal bounds). */
__attribute__((noinline)) static void str_level_par(str_ws_t *w, size_t lo, size_t hi, size_t *pd, int P, int Pn, size_t *nlo, size_t *nhi) {
    size_t d = *pd;
    *nlo = *nhi = 0;
    for (;;) {
        uint64_t mn, mx; str_keys(w, lo, hi, d, Pn, &mn, &mx);
        if (mn != mx) break;
        if ((mn & 0xff) == 0) return;                                    /* identical strings */
        d += 8;                                                          /* one shared block: the same range, one block deeper */
    }
    *pd = d;
    if (!str_kv_par(w, lo, hi, Pn)) str_msd(w->k + lo, w->a + lo, w->k2 + lo, w->v2 + lo, hi - lo, 0);
    /* Groups of equal keys, collected first (the deeper levels overwrite the keys), then handed to the workers CONCURRENTLY,
       the large ones first; a group gets threads of its own only when it alone is more than a fair share of what is left
       (1/P of it). Processing each large group in turn with every thread scaled paths 1.23x from 1 to 14 threads: twenty
       user directories of 100k strings each ran one after another on three threads apiece (make stringbench). */
    size_t cap = 1024, nr = 0, tot = 0, *rs = malloc(cap * 2 * sizeof *rs);
    if (!rs) {                                                           /* no list: the groups in turn, on this thread */
        for (size_t i = lo; i < hi; ) {
            size_t j = i + 1; uint64_t x = w->k[i];
            while (j < hi && w->k[j] == x) j++;
            if (j - i > 1 && (x & 0xff)) str_level(w, i, j, d + 8, 1);
            i = j;
        }
        return;
    }
    for (size_t i = lo; i < hi; ) {
        size_t j = i + 1; uint64_t x = w->k[i];
        while (j < hi && w->k[j] == x) j++;
        if (j - i > 1 && (x & 0xff)) {
            if (nr == cap) { size_t *t = realloc(rs, cap * 4 * sizeof *rs); if (!t) { str_level(w, i, j, d + 8, 1); i = j; continue; } rs = t; cap *= 2; }
            rs[2 * nr] = i; rs[2 * nr + 1] = j; nr++; tot += j - i;
        }
        i = j;
    }
    size_t big_lo[64], big_hi[64]; int nbig = 0;
    for (size_t r = 0; r < nr; ) {                                       /* past a fair share: out of the list, threads of its own */
        size_t len = rs[2 * r + 1] - rs[2 * r];
        if (len >= g_str_par_min && (g_str_seqbig || len * (size_t)P > tot) && nbig < 64) {
            big_lo[nbig] = rs[2 * r]; big_hi[nbig] = rs[2 * r + 1]; nbig++;
            rs[2 * r] = rs[2 * (nr - 1)]; rs[2 * r + 1] = rs[2 * (nr - 1) + 1]; nr--;
        } else r++;
    }
    size_t m = 0;                                                        /* the groups of 4096 and more to the front: claimed first */
    for (size_t r = 0; r < nr; r++) if (rs[2 * r + 1] - rs[2 * r] >= 4096) {
        size_t a0 = rs[2 * r], a1 = rs[2 * r + 1]; rs[2 * r] = rs[2 * m]; rs[2 * r + 1] = rs[2 * m + 1]; rs[2 * m] = a0; rs[2 * m + 1] = a1; m++;
    }
    int gi = -1;                                                         /* the largest big group goes back to the caller's loop */
    for (int g = 0; g < nbig; g++) if (gi < 0 || big_hi[g] - big_lo[g] > big_hi[gi] - big_lo[gi]) gi = g;
    for (int g = 0; g < nbig; g++) if (g != gi) str_level(w, big_lo[g], big_hi[g], d + 8, P);
    if (nr) {
        int Pr = P; if ((size_t)Pr > nr) Pr = (int)nr;
        pthread_t th[256]; str_runs_arg_t ra[256]; atomic_size_t next; atomic_init(&next, 0);
        for (int t = 0; t < Pr; t++) { str_runs_arg_t a0 = { w, rs, nr, d + 8, &next }; ra[t] = a0; }
        par_run(th, Pr, str_runs_worker, ra, sizeof ra[0], 1);
    }
    free(rs);
    if (gi >= 0) { *nlo = big_lo[gi]; *nhi = big_hi[gi]; }
}
/* THE LOG-LINE ROUTE (2026-10-19): the numeric near-sorted route, one engine over. Log lines arrive almost in order -- a few
 * late, and lines stamped in the same millisecond in no order -- and the radix sorts them as if they were random: 2M lines at
 * one thread 117 ms, 2.4x std::sort, the weakest string row. Here ONE pass keeps the strand: a line no smaller than the last
 * kept line is kept (compacted in place), else it is a stray; and when it undercuts the last kept line but not the one before,
 * the LAST kept line is the stray (it was too large: the numeric route's poison recovery, here as one extra compare). The
 * strays alone are sorted (by this same entry, which may decline them to the radix), then merged with the kept strand from
 * the back, in place. A pre-gate runs the SAME pass on 64 windows of 64 lines first (the ledger: never a simplified copy of the
 * route's scan), and the pass itself gives up past STR_NS_STRAY_MAX strays, handing the caller a permutation of its input.
 * One thread only: the pass is a dependency chain, and the sort with threads is already 4x faster than this route could be.
 * LOGOS_STR_NO_NS=1: the radix, for the A/B (read only on calls this large). */
#ifndef STR_NS_MIN
#define STR_NS_MIN 65536
#endif
#ifndef STR_NS_STRAY_MAX
#define STR_NS_STRAY_MAX 0.35           /* strays per line past which the pass gives up */
#endif
#ifndef STR_NS_GATE
#define STR_NS_GATE 0.30                /* the pre-gate's windows must show fewer: logs read 0.16, every random shape 0.85-0.90 */
#endif
#ifndef STR_NS_GLOBAL_MAX
#define STR_NS_GLOBAL_MAX 0.10          /* the global probe's inversions, at every lag */
#endif
static size_t g_str_ns;                 /* logos_twist_last(27): string sorts the log-line route finished */
static int g_str_ns_last;               /* logos_twist_last(-9): 0 not tried, 1 done, 2 declined at the pre-gate, 3 gave up in the pass, 4 no memory */
static size_t g_str_ns_gate;           /* logos_twist_last(-10): the pre-gate's strays, out of 4,096 window lines */
/* the keep pass over a[lo, hi): kept lines compacted to a[lo ...), strays appended to s; returns the kept count, or SIZE_MAX
   when the strays pass cap (then a[lo, hi) is a permutation of what it held: the strays went back where the kept ones left room) */
static size_t str_ns_pass(const char **a, size_t lo, size_t hi, const char **s, size_t *ns, size_t cap) {
    size_t w = lo, S = *ns;
    for (size_t i = lo; i < hi; i++) {
        const char *x = a[i];
        if (w == lo || strcmp(a[w - 1], x) <= 0) a[w++] = x;
        else if (w - lo >= 2 && strcmp(a[w - 2], x) <= 0) { s[S++] = a[w - 1]; a[w - 1] = x; }   /* the last kept line was too large */
        else s[S++] = x;
        if (S - *ns > cap || ((i & 4095) == 4095 && (double)(S - *ns) > STR_NS_GATE * (double)(i - lo) + 2048.0)) {
            /* give up (at the cap, or early: strays outrunning the gate's rate, as a concatenation's second file does): the
               strays fill the room between the kept lines and the unread ones */
            size_t k = *ns; for (size_t j = w; j <= i; j++) a[j] = s[k++];
            *ns = S; return SIZE_MAX;
        }
    }
    *ns = S; return w - lo;
}
__attribute__((noinline)) static int str_ns_try(const char **a, size_t n) {
    g_str_ns_last = 0;
    if (n < STR_NS_MIN || getenv("LOGOS_STR_NO_NS")) return 0;
    {   /* the pre-gate, part one: GLOBAL order. 256 lines in position order, pairs 1, 4, 16 and 64 apart. Log lines read ~0 (a late
           line is a few hundred lines late, far less than a lag), noise 0.5, and eight sorted files concatenated read their resets:
           every window inside a file is perfectly ordered, which is why the windows alone would admit them */
        const char *g[256];
        for (int k = 0; k < 256; k++) g[k] = a[(size_t)((double)k * (double)(n - 1) / 255.0)];
        static const int lag[4] = { 1, 4, 16, 64 };
        for (int q = 0; q < 4; q++) {
            int inv = 0, L = lag[q];
            for (int k = 0; k + L < 256; k++) inv += strcmp(g[k], g[k + L]) > 0;
            if ((double)inv > STR_NS_GLOBAL_MAX * (double)(256 - L)) { g_str_ns_last = 2; return 0; }
        }
    }
    {   /* the pre-gate, part two: the route's own pass on 64 windows of 64 lines */
        const char *wb[64], *ws[64]; size_t strays = 0;
        for (int k = 0; k < 64; k++) {
            size_t at = (size_t)((double)(n - 64) * fmod((double)(k + 1) * 0.6180339887498949, 1.0)), ns = 0;
            memcpy(wb, a + at, sizeof wb);
            (void)str_ns_pass(wb, 0, 64, ws, &ns, 64);
            strays += ns;
        }
        g_str_ns_gate = strays;
        if ((double)strays > STR_NS_GATE * 4096.0) { g_str_ns_last = 2; return 0; }
    }
    double capf = STR_NS_STRAY_MAX;
    { const char *e = getenv("LOGOS_STR_NS_CAP"); if (e && atof(e) > 0) capf = atof(e); }   /* the harness forces the give-up with it */
    size_t cap = (size_t)(capf * (double)n), S = 0;
    const char **s = malloc((cap + 2) * sizeof *s);
    if (!s) { g_str_ns_last = 4; return 0; }
    size_t K = str_ns_pass(a, 0, n, s, &S, cap);
    if (K == SIZE_MAX) { free(s); g_str_ns_last = 3; return 0; }
    logos_sort_strings_mt(s, S, 1);                                    /* the strays: the radix, or this route again on fewer */
    size_t i = K, j = S, w = n;                                        /* merge from the back: w never passes i */
    while (j > 0) {
        if (i > 0 && strcmp(a[i - 1], s[j - 1]) > 0) a[--w] = a[--i];
        else a[--w] = s[--j];
    }
    free(s);
    g_str_ns++; g_str_ns_last = 1;
    return 1;
}
void logos_sort_strings_mt(const char **a, size_t n, int nthreads) {
    if (!a || n < 2) return;
    if (n <= STR_SMALL) { str_small_keys(a, n, 0); return; }          /* no getenv on a small call */
    g_str_smallmkqs = getenv("LOGOS_STR_SMALLMKQS") != NULL;            /* the A/B for the groups inside a large sort */
    {   /* the prescan, as the numeric dispatcher's: already in order costs one pass. Without it sorted words ran 2.6x slower
           than the multikey quicksort, whose middle pivot is perfect on sorted input (make stringbench) */
        size_t i = 1;
        while (i < n && strcmp(a[i - 1], a[i]) <= 0) i++;
        if (i == n) { g_str_sorts++; return; }
        if (i == 1) {                                                    /* the first step went down: strictly descending throughout? */
            while (i < n && strcmp(a[i - 1], a[i]) > 0) i++;
            if (i == n) { for (size_t l = 0, r = n - 1; l < r; l++, r--) { const char *t = a[l]; a[l] = a[r]; a[r] = t; } g_str_sorts++; return; }
        }
    }
    int P = nthreads > 1 ? nthreads : 1; if (P > 256) P = 256;
    g_str_seqbig = P > 1 && getenv("LOGOS_STR_SEQBIG") != NULL;      /* the A/B switches matter only with threads: no getenv on a small call */
    g_str_par_min = STR_PAR_MIN; g_str_per_thread = STR_MIN_PER_THREAD;
    { const char *eg = P > 1 ? getenv("LOGOS_STR_GROUPPAR") : NULL; g_str_grouppar = eg && atoi(eg) != 0; }   /* opt-in: measured mixed */
    if (P > 1) { const char *e1 = getenv("LOGOS_STR_PARMIN"), *e2 = getenv("LOGOS_STR_PERTHREAD");
                 if (e1 && atol(e1) >= 64) g_str_par_min = (size_t)atol(e1);
                 if (e2 && atol(e2) >= 64) g_str_per_thread = (size_t)atol(e2); }
    g_str_ns_last = 0;
    if (P == 1 && n >= STR_NS_MIN && str_ns_try(a, n)) { g_str_sorts++; return; }   /* log lines: keep the strand, sort the strays */
    str_ws_t w; w.a = a;
    w.k = malloc(n * sizeof *w.k); w.k2 = malloc(n * sizeof *w.k2); w.v2 = malloc(n * sizeof *w.v2);
    w.bid = P > 1 ? malloc(n * sizeof *w.bid) : NULL;
    if (!w.k || !w.k2 || !w.v2 || (P > 1 && !w.bid)) {                  /* no memory: the multikey quicksort needs none */
        free(w.k); free(w.k2); free(w.v2); free(w.bid);
        str_mkqs(a, 0, (long)n - 1, 0); return;
    }
    str_level(&w, 0, n, 0, P);
    free(w.k); free(w.k2); free(w.v2); free(w.bid);
    g_str_sorts++;
}
void logos_sort_i32(int32_t *a, size_t n, int nthreads) {   /* native: 4-byte elements throughout */
    if (n > (size_t)UINT32_MAX) { logos_sort_i32_inplace(a, n, nthreads); return; }     /* 32-bit counters: see logos_sort_f64 */
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
    if (n > (size_t)UINT32_MAX) { logos_sort_i32_inplace(a, n, nthreads); return 0; }   /* 32-bit counters: see logos_sort_f64. 0 = came out sorted */
    int nbk = 0;
#ifndef LOGOS_NO_CTX_POOL
    if (ns_depth == 0) g_ctx_next = 0;
#endif
    psort_i32_ex(a, n, nthreads, starts, &nbk, bmin, bmax);
    return nbk;
}
typedef struct { int32_t *a; const size_t *starts; int nbk; unsigned char *done; atomic_size_t *next; size_t maxb, typ; } sbi_arg_t;
typedef struct { sbi_arg_t *w; ctx_t *ctx; } sbi_targ_t;
static void *sbi_worker(void *p) {
    sbi_targ_t *ta = p; sbi_arg_t *w = ta->w; ctx_t *c = ta->ctx; size_t b;
    uint32_t *buf = malloc((w->typ ? w->typ : 1) * sizeof(uint32_t));
    if (!buf) return NULL;
    while ((b = atomic_fetch_add(w->next, 1)) < (size_t)w->nbk) {
        if (w->done[b]) continue;
        size_t s0 = w->starts[b], nb = w->starts[b + 1] - s0;
        if (nb > 1) { if (g_simd_i32) g_simd_i32(w->a + s0, nb); else { ctx_fit_t cf; uint32_t *big = nb > w->typ ? malloc(nb * sizeof(uint32_t)) : NULL, *sc = nb > w->typ ? big : buf;
            if (sc && ctx_fit(c, nb, &cf)) { leaf_i32(w->a + s0, nb, sc, c); ctx_unfit(c, &cf); } else qsort(w->a + s0, nb, sizeof(int32_t), cmp_i32);
            free(big); } }
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
    sbi_arg_t w = { a, starts, nbk, done, &next, maxb, ctx_typical(maxb, starts[nbk] - starts[0], nbk) };
    pthread_t *th = malloc((size_t)P * sizeof(pthread_t));
    ctx_t *ctxs = calloc((size_t)P, sizeof(ctx_t));
    sbi_targ_t *ta = calloc((size_t)P, sizeof(sbi_targ_t));
    int made = 0;
    if (th && ctxs && ta) { for (int t = 0; t < P; t++) { if (!ctx_init(&ctxs[t], w.typ, 0)) break; made++; } }
    for (int t = 0; t < made; t++) { ta[t].w = &w; ta[t].ctx = &ctxs[t]; }
    if (made > 0) par_run(th, made, sbi_worker, ta, sizeof ta[0], 1);
    for (int t = 0; t < made; t++) ctx_free(&ctxs[t]);
    free(th); free(ctxs); free(ta);
    for (int b = 0; b < nbk; b++) if (!done[b]) { size_t s0 = starts[b], nb = starts[b + 1] - s0; if (nb > 1) logos_sort_i32(a + s0, nb, 1); done[b] = 1; }
}
int logos_num_cpus(void) { long c = sysconf(_SC_NPROCESSORS_ONLN); return c < 1 ? 1 : (int)c; }

/* ---- NaN, with a defined order, for callers who cannot promise NaN-free data ------------------------------------
 * logos_sort_f64 leaves the ORDER unspecified when the array holds a NaN: a NaN fails every compare, so a prescan can
 * take [1, NaN, 0.5] for ascending, and measured on an IEEE probe the non-NaN values came back out of order in most
 * runs. What it does promise, and the harness checks, is the floor: no crash, no out-of-bounds access, and an output
 * that is a permutation of the input's bit patterns. Making the order defined costs one more pass over the array, which
 * the default entry does not pay (neither does std::sort, whose contract a NaN also breaks). This entry pays it: NaNs
 * are swapped to the tail with their bits intact, and the NaN-free prefix is sorted. The pass is sequential. */
size_t logos_nan_to_end_f64(double *a, size_t n) {
    size_t m = n;
    for (size_t i = 0; i < m; ) { if (a[i] != a[i]) { double v = a[i]; a[i] = a[--m]; a[m] = v; } else i++; }
    return m;
}
void logos_sort_f64_nan_last(double *a, size_t n, int nthreads) { logos_sort_f64(a, logos_nan_to_end_f64(a, n), nthreads); }

/* ---- the strict in-place sort, in parallel (2026-09-20) ------------------------------------------------------------
 * The in-place entries took an `nthreads` and ignored it. `make slopebench` priced that: at 50M the sequential in-place
 * sort costs 3.6 to 21 times what the buffered sort costs per element at ONE thread (uniform 100.7 against 25.3 ns), and the
 * buffered sort then goes parallel while this one could not. An outside review's headline comparison was exactly this
 * pair, in-place against buffered, and the sibling line it reviewed had a parallel in-place sort.
 *
 * The placement pass of an American-flag sort is cycle-following and does not split across threads without block
 * machinery (IPS4o's); what splits for free is everything AFTER a pass, because its 256 buckets are disjoint ranges of the
 * array. So: ranges are tasks. A worker pops a range; a small one (<= IP_GRAIN) it sorts outright with the sequential
 * kernel; a large one gets ONE placement pass, after which its large children go back on the stack and its small children
 * are sorted where they lie. The first pass over the whole array therefore still runs on one thread, which bounds the
 * speed-up. MEASURED (make ipbench, 14 threads, against its own 1-thread form): 2.5x at 8.4M and 2.85x at 50M on uniform
 * doubles, a serial share of about 30%; 1.1-1.6x on the structured shapes, whose one expensive pass IS the serial one; no
 * loss at the 2^20 threshold. It is still 6-10x slower than the buffered sort at the same 14 threads: zero scratch is
 * bought with time. A value that owns a whole bucket is split again by whoever pops it. NEXT: the placement pass itself,
 * by speculative permutation with a repair phase (PARADIS); the three read-only passes before it are the cheap third.
 *
 * Still strictly in place: the task stack is a fixed IP_TASKS entries (96 KB) whatever n is, and a full stack means the
 * child is sorted inline instead of queued. If the stack or the thread handles cannot be allocated the caller sorts
 * sequentially (nothing has been touched yet); a thread that cannot be created is par_run's business, and the workers
 * finish on "no task queued and none in hand", which does not depend on how many of them exist. */
#ifndef IP_PAR_MIN
#define IP_PAR_MIN (1u << 20)        /* below this the sequential kernel is done before a spawn has paid for itself */
#endif
#ifndef IP_GRAIN
#define IP_GRAIN   (1u << 15)        /* a range this small is sorted by whoever holds it, not split into tasks */
#endif
#define IP_TASKS   4096
#define DEFINE_INPLACE_PASS(NAME, ETYPE, KEY) \
static void NAME(ETYPE *a, size_t lo, size_t hi, int shift, size_t *bstart) {          /* bstart: 257 entries out */ \
    size_t cnt[256]; memset(cnt, 0, sizeof cnt); \
    for (size_t i = lo; i < hi; i++) cnt[(KEY(a[i]) >> shift) & 0xffu]++; \
    bstart[0] = lo; for (int b = 0; b < 256; b++) bstart[b + 1] = bstart[b] + cnt[b]; \
    size_t next[256]; for (int b = 0; b < 256; b++) next[b] = bstart[b]; \
    for (int b = 0; b < 256; b++) { \
        while (next[b] < bstart[b + 1]) {                          /* the same cycle placement as the sequential kernel */ \
            ETYPE v = a[next[b]]; unsigned tb = (unsigned)((KEY(v) >> shift) & 0xffu); \
            if (tb == (unsigned)b) { next[b]++; } \
            else { a[next[b]] = a[next[tb]]; a[next[tb]] = v; next[tb]++; } \
        } \
    } \
}
DEFINE_INPLACE_PASS(inplace_pass_i32, int32_t, IN_KEY_I32)
DEFINE_INPLACE_PASS(inplace_pass_f64, double,  IN_KEY_F64)

typedef struct { size_t lo, hi; int si; } ip_task_t;
typedef struct {
    void *a; int f64; const int *shifts; int ns;
    ip_task_t *q; size_t top;
    size_t inflight;                                  /* tasks queued or in hand; guarded by m, like the stack */
    pthread_mutex_t m; pthread_cond_t cv;
} ip_pool_t;
static int ip_push(ip_pool_t *pl, size_t lo, size_t hi, int si) {
    pthread_mutex_lock(&pl->m);
    if (pl->top == IP_TASKS) { pthread_mutex_unlock(&pl->m); return 0; }
    ip_task_t t = { lo, hi, si }; pl->q[pl->top++] = t; pl->inflight++;
    pthread_cond_signal(&pl->cv);
    pthread_mutex_unlock(&pl->m);
    return 1;
}
static void ip_sort_range(ip_pool_t *pl, size_t lo, size_t hi, int si) {
    if (pl->f64) inplace_msd_f64((double *)pl->a, lo, hi, pl->shifts, si, pl->ns);
    else inplace_msd_i32((int32_t *)pl->a, lo, hi, pl->shifts, si, pl->ns);
}
static void ip_run(ip_pool_t *pl, ip_task_t t) {
    if (t.hi - t.lo <= IP_GRAIN || t.si + 1 >= pl->ns) { ip_sort_range(pl, t.lo, t.hi, t.si); return; }
    size_t bs[257];
    if (pl->f64) inplace_pass_f64((double *)pl->a, t.lo, t.hi, pl->shifts[t.si], bs);
    else inplace_pass_i32((int32_t *)pl->a, t.lo, t.hi, pl->shifts[t.si], bs);
    for (int b = 0; b < 256; b++) {
        size_t clo = bs[b], chi = bs[b + 1];
        if (chi - clo < 2) continue;
        if (chi - clo > IP_GRAIN && ip_push(pl, clo, chi, t.si + 1)) continue;
        ip_sort_range(pl, clo, chi, t.si + 1);                     /* small, or the stack is full: here and now */
    }
}
static void *ip_worker(void *p) {
    ip_pool_t *pl = p;
    pthread_mutex_lock(&pl->m);
    for (;;) {
        while (pl->top == 0 && pl->inflight > 0) pthread_cond_wait(&pl->cv, &pl->m);
        if (pl->top == 0) break;                                   /* nothing queued and nothing in hand anywhere: done */
        ip_task_t t = pl->q[--pl->top];
        pthread_mutex_unlock(&pl->m);
        ip_run(pl, t);
        pthread_mutex_lock(&pl->m);
        if (--pl->inflight == 0) pthread_cond_broadcast(&pl->cv);
    }
    pthread_mutex_unlock(&pl->m);
    return NULL;
}
/* returns 1 when the array has been sorted, 0 when nothing was touched and the caller should sort sequentially */
static int inplace_parallel(void *a, int f64, size_t n, const int *shifts, int ns, int P) {
    ip_pool_t pl; memset(&pl, 0, sizeof pl);
    pl.a = a; pl.f64 = f64; pl.shifts = shifts; pl.ns = ns;
    pl.q = malloc(IP_TASKS * sizeof(ip_task_t));
    pthread_t *th = malloc((size_t)P * sizeof *th);
    if (!pl.q || !th) { free(pl.q); free(th); return 0; }
    pthread_mutex_init(&pl.m, NULL); pthread_cond_init(&pl.cv, NULL);
    ip_task_t root = { 0, n, 0 }; pl.q[pl.top++] = root; pl.inflight = 1;
    par_run(th, P, ip_worker, &pl, 0, 1);                          /* stride 0: every worker shares the one pool */
    pthread_mutex_destroy(&pl.m); pthread_cond_destroy(&pl.cv);
    free(pl.q); free(th);
    return 1;
}

/* ---- strict in-place entries (~1n memory; slower; for RAM-bound large n) ---- */
void logos_sort_i32_inplace(int32_t *a, size_t n, int nthreads) {
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
    { int P = effective_threads(n, nthreads < 1 ? 1 : nthreads);           /* the prescan and the OR/AND pass above stay on one thread */
      if (P > 1 && n >= IP_PAR_MIN && inplace_parallel(a, 0, n, shifts, ns, P)) return; }
    inplace_msd_i32(a, 0, n, shifts, 0, ns);
}
void logos_sort_f64_inplace(double *a, size_t n, int nthreads) {
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
    { int P = effective_threads(n, nthreads < 1 ? 1 : nthreads);
      if (P > 1 && n >= IP_PAR_MIN && inplace_parallel(a, 1, n, shifts, ns, P)) return; }
    inplace_msd_f64(a, 0, n, shifts, 0, ns);
}

/* ===================== argsort with threads (2026-11-10, FINDINGS 3ca; make argceil, make argbench) =====================
 * logos_argsort_f64 is an index-carrying LSD radix on one thread: up to eight passes over 12-byte pairs and 20 bytes of scratch per
 * element, whatever the keys look like. Measured against told ceilings at 8.4M: sorted keys cost it 54 ns per element where one
 * scan answers (0.6); a vector key-value quicksort beat it by only 1.2x at one thread (so the one-thread kernel stays), but 14
 * threads were worth 4.4x. This entry:
 *   1. keys already in order (by the total-order key, which is the order the radix defines): the identity, one pass;
 *   2. one thread by the thread policy: the same one-thread radix;
 *   3. else one STABLE parallel partition into up to 2^AS_BITS cells of equal width in key space (per-thread histograms, offsets in thread order),
 *      then every bucket finished where it lies by a byte-skipping LSD radix on the bits below, buckets claimed from a counter.
 * The pairs are written once by the partition, so the scratch is 12 bytes per element plus one bucket per worker (was 20).
 * Stable, like logos_argsort_f64, and the same permutation bit for bit. A failed allocation falls back to it (a is never
 * written, perm is rewritten whole). LOGOS_ARGSORT_SEQ=1: always the one-thread radix; logos_twist_last(42): parallel argsorts done. */
#ifndef AS_BITS
#define AS_BITS 12
#endif
typedef struct {
    const double *a; size_t n, lo, hi; int sh, nb; uint64_t mask;      /* mask: the smallest key (the cells' origin) */
    uint64_t orv, andv;                                               /* phase 0: this chunk's largest and smallest key */ size_t *cnt, *off; uint64_t *k1; uint32_t *i1, *perm;
    atomic_size_t *next; const size_t *bstart; atomic_int *fail; int phase;
} asw_t;
static void as_bucket(asw_t *w, size_t s, size_t e, uint64_t **ksp, uint32_t **isp, size_t *cap) {
    uint64_t *k = w->k1 + s; uint32_t *ix = w->i1 + s, *out = w->perm + s; size_t len = e - s;
    if (len == 1) { out[0] = ix[0]; return; }
    uint64_t orv = 0, andv = ~0ULL; for (size_t i = 0; i < len; i++) { orv |= k[i]; andv &= k[i]; }
    uint64_t diff = orv ^ andv;
    if (!diff) { memcpy(out, ix, len * sizeof(uint32_t)); return; }      /* one value: the partition already left its indices in order */
    if (len <= 48) {                                                     /* stable insertion on the pairs */
        for (size_t i = 1; i < len; i++) { uint64_t kk = k[i]; uint32_t ii = ix[i]; size_t q = i; while (q && k[q - 1] > kk) { k[q] = k[q - 1]; ix[q] = ix[q - 1]; q--; } k[q] = kk; ix[q] = ii; }
        memcpy(out, ix, len * sizeof(uint32_t)); return;
    }
    if (*cap < len) {
        free(*ksp); free(*isp); *cap = 0;
        *ksp = malloc(len * sizeof(uint64_t)); *isp = malloc(len * sizeof(uint32_t));
        if (!*ksp || !*isp) { free(*ksp); free(*isp); *ksp = NULL; *isp = NULL; atomic_store(w->fail, 1); return; }
        *cap = len;
    }
    uint64_t *ks = k, *kd = *ksp; uint32_t *is = ix, *id = *isp;
    for (int p = 0; p < 8; p++) {
        int sh = p * 8; if (((diff >> sh) & 0xff) == 0) continue;
        size_t c[257]; memset(c, 0, sizeof c);
        for (size_t i = 0; i < len; i++) c[((ks[i] >> sh) & 0xff) + 1]++;
        for (int b = 0; b < 256; b++) c[b + 1] += c[b];
        for (size_t i = 0; i < len; i++) { size_t b = (ks[i] >> sh) & 0xff, pos = c[b]++; kd[pos] = ks[i]; id[pos] = is[i]; }
        { uint64_t *t = ks; ks = kd; kd = t; } { uint32_t *t = is; is = id; id = t; }
    }
    memcpy(out, is, len * sizeof(uint32_t));
}
static void *as_worker(void *p) {
    asw_t *w = p; const double *a = w->a;
    if (w->phase == 0) {                                                 /* which key bits vary */
        uint64_t mn = ~0ULL, mx = 0;
        for (size_t i = w->lo; i < w->hi; i++) { uint64_t k = total_key(a[i]); mn = k < mn ? k : mn; mx = k > mx ? k : mx; }
        w->orv = mx; w->andv = mn;
    } else if (w->phase == 1) {                                          /* this chunk's histogram */
        size_t *c = w->cnt; int sh = w->sh; uint64_t k0 = w->mask;
        for (size_t i = w->lo; i < w->hi; i++) c[(total_key(a[i]) - k0) >> sh]++;
    } else if (w->phase == 2) {                                          /* this chunk's pairs to their buckets, in order */
        size_t *o = w->off; int sh = w->sh; uint64_t k0 = w->mask; uint64_t *k1 = w->k1; uint32_t *i1 = w->i1;
        for (size_t i = w->lo; i < w->hi; i++) { uint64_t k = total_key(a[i]); size_t pos = o[(k - k0) >> sh]++; k1[pos] = k; i1[pos] = (uint32_t)i; }
    } else {                                                             /* buckets, claimed 16 at a time */
        uint64_t *ks = NULL; uint32_t *is = NULL; size_t cap = 0, b0;
        while ((b0 = atomic_fetch_add(w->next, 16)) < (size_t)w->nb && !atomic_load_explicit(w->fail, memory_order_relaxed))
            for (size_t b = b0; b < b0 + 16 && b < (size_t)w->nb; b++) { size_t s = w->bstart[b], e = w->bstart[b + 1]; if (e > s) as_bucket(w, s, e, &ks, &is, &cap); }
        free(ks); free(is);
    }
    return NULL;
}
/* the one-thread radix, as logos_argsort_f64 (logos_extra.c) has it: kept here so that this file links on its own (the paired
   benches link engine copies without logos_extra.c, and a call across broke every one of them the day this entry was added) */
static const double *g_as_arr;
static int as_cmp(const void *x, const void *y) {
    uint32_t i = *(const uint32_t *)x, j = *(const uint32_t *)y; uint64_t a = total_key(g_as_arr[i]), b = total_key(g_as_arr[j]);
    return a < b ? -1 : a > b ? 1 : i < j ? -1 : i > j;
}
static void as_seq(const double *a, size_t n, uint32_t *perm) {
    for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
    if (n < 2) return;
    uint64_t *k0 = malloc(n * 8), *k1 = malloc(n * 8); uint32_t *i1 = malloc(n * 4);
    if (!k0 || !k1 || !i1) { free(k0); free(k1); free(i1); g_as_arr = a; qsort(perm, n, 4, as_cmp); return; }   /* no memory: slow, and not reentrant, as the original */
    uint64_t orv = 0, andv = ~0ULL;
    for (size_t i = 0; i < n; i++) { uint64_t k = total_key(a[i]); k0[i] = k; orv |= k; andv &= k; }
    uint64_t diff = orv ^ andv, *ks = k0, *kd = k1; uint32_t *is = perm, *id = i1;
    for (int p = 0; p < 8; p++) {
        int sh = p * 8; if (((diff >> sh) & 0xff) == 0) continue;
        size_t c[257]; memset(c, 0, sizeof c);
        for (size_t i = 0; i < n; i++) c[((ks[i] >> sh) & 0xff) + 1]++;
        for (int b = 0; b < 256; b++) c[b + 1] += c[b];
        for (size_t i = 0; i < n; i++) { size_t b = (ks[i] >> sh) & 0xff, pos = c[b]++; kd[pos] = ks[i]; id[pos] = is[i]; }
        { uint64_t *t = ks; ks = kd; kd = t; } { uint32_t *t = is; is = id; id = t; }
    }
    if (is != perm) memcpy(perm, is, n * 4);
    free(k0); free(k1); free(i1);
}
void logos_argsort_f64_mt(const double *a, size_t n, uint32_t *perm, int nthreads) {
    if (!a || !perm || n == 0) return;
    if (n < 2) { perm[0] = 0; return; }
    {   size_t i = 1; uint64_t pk = total_key(a[0]);                     /* the prescan: in key order already -> the identity */
        for (; i < n; i++) { uint64_t k = total_key(a[i]); if (k < pk) break; pk = k; }
        if (i == n) { for (size_t q = 0; q < n; q++) perm[q] = (uint32_t)q; return; }
    }
    int P = effective_threads(n, nthreads < 1 ? 1 : nthreads);
    if (P < 2 || n < 200000 || n > 0xFFFFFFFFull || getenv("LOGOS_ARGSORT_SEQ")) { as_seq(a, n, perm); return; }
    asw_t *w = calloc((size_t)P, sizeof *w); pthread_t *th = malloc((size_t)P * sizeof *th);
    uint64_t *k1 = malloc(n * sizeof(uint64_t)); uint32_t *i1 = malloc(n * sizeof(uint32_t));
    size_t *cnt = NULL, *bstart = NULL; int done = 0;
    if (w && th && k1 && i1) {
        size_t chunk = (n + (size_t)P - 1) / (size_t)P; atomic_int fail; atomic_init(&fail, 0); atomic_size_t next; atomic_init(&next, 0);
        for (int t = 0; t < P; t++) { w[t].a = a; w[t].n = n; w[t].lo = (size_t)t * chunk < n ? (size_t)t * chunk : n; w[t].hi = w[t].lo + chunk < n ? w[t].lo + chunk : n;
            w[t].k1 = k1; w[t].i1 = i1; w[t].perm = perm; w[t].fail = &fail; w[t].next = &next; w[t].phase = 0; }
        par_run(th, P, as_worker, w, sizeof w[0], 1);
        uint64_t kmx = 0, kmn = ~0ULL; for (int t = 0; t < P; t++) { if (w[t].lo < w[t].hi) { kmx = w[t].orv > kmx ? w[t].orv : kmx; kmn = w[t].andv < kmn ? w[t].andv : kmn; } }
        uint64_t diff = kmx - kmn;                                        /* cells of equal width in KEY space from the smallest key: raw key bits
                                                                             put a column that straddles 1.0 (every exponent bit varies) into ~30 buckets */
        if (!diff) { for (size_t q = 0; q < n; q++) perm[q] = (uint32_t)q; done = 1; }   /* cannot be reached past the prescan; kept for the arithmetic below */
        else {
            int asb = AS_BITS; { const char *e = getenv("LOGOS_AS_BITS"); if (e) { int v = atoi(e); if (v >= 4 && v <= 16) asb = v; } }
            int h = 63 - __builtin_clzll(diff), bits = h + 1 < asb ? h + 1 : asb, sh = h + 1 - bits, nb = (int)(diff >> sh) + 1;
            cnt = calloc((size_t)P * (size_t)nb, sizeof(size_t)); bstart = malloc(((size_t)nb + 1) * sizeof(size_t));
            if (cnt && bstart) {
                for (int t = 0; t < P; t++) { w[t].sh = sh; w[t].nb = nb; w[t].mask = kmn; w[t].cnt = cnt + (size_t)t * (size_t)nb; w[t].off = w[t].cnt; w[t].bstart = bstart; w[t].phase = 1; }
                par_run(th, P, as_worker, w, sizeof w[0], 1);
                size_t acc = 0;                                           /* bucket starts; each thread's counts become its write offsets, in thread order (stable) */
                for (int b = 0; b < nb; b++) { bstart[b] = acc; for (int t = 0; t < P; t++) { size_t c = cnt[(size_t)t * (size_t)nb + (size_t)b]; cnt[(size_t)t * (size_t)nb + (size_t)b] = acc; acc += c; } }
                bstart[nb] = acc;
                if (acc == n) {
                    for (int t = 0; t < P; t++) w[t].phase = 2;
                    par_run(th, P, as_worker, w, sizeof w[0], 1);
                    for (int t = 0; t < P; t++) w[t].phase = 3;
                    par_run(th, P, as_worker, w, sizeof w[0], 1);
                    if (!atomic_load(&fail)) { done = 1; atomic_fetch_add_explicit(&g_argsort_par, 1, memory_order_relaxed); }

                }
            }
        }
    }
    free(w); free(th); free(k1); free(i1); free(cnt); free(bstart);
    if (!done) as_seq(a, n, perm);                            /* no memory somewhere: the one-thread radix, which has its own fallback */
}
