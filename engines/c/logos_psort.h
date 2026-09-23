/* logos_psort.h - parallel (multi-threaded) numeric sort, C + pthreads.
 *
 * Parallel sample sort with the logos-sort JavaScript engine's adaptive numeric
 * dispatcher as the per-bucket leaf: prescan early-exits, counting / byte-skip
 * radix for integers, run detection with galloping merges, momentum insertion
 * with a displacement budget, gap-split, flash, skew probe -> exponent-bit
 * coarse, value coarse, quantized-float radix, dual-pivot introsort. Above it:
 * zero-scatter counting paths for low and moderate cardinality, dynamic
 * chunked phases, spin-then-block barriers, per-thread arenas, runtime-gated
 * write-combining and destination prefetch in every scatter.
 *
 * All functions sort ascending, in place, and give identical results for any
 * thread count. Pass nthreads <= 1 for sequential.
 *
 * Naming follows C convention: snake_case functions/variables, *_t types,
 * UPPER_SNAKE macros.
 */
#ifndef LOGOS_PSORT_H
#define LOGOS_PSORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOGOS_PSORT_VERSION "3.6.0"

/* Sort n doubles ascending, in place. Infinities, subnormals and signed zeros are ordinary values and are tested.
 * With a NaN in the array the ORDER is unspecified, of the other values too (use logos_sort_f64_nan_last); what holds
 * even then, and is tested, is that the call returns, nothing crashes and the output is a permutation of the input's bit patterns.
 *
 * Memory, MEASURED (make memprobe: peak traced heap of one cold call, on top of the 8-byte-per-element input):
 *   1 thread     ~15.5 bytes per element  (8 scratch + 7.5 sequential context)
 *   14 threads   ~23 bytes per element on data that reaches the partition (8 scratch + 1 bucket-index cache + 12.8
 *                slack-sized bucket regions + ~1 contexts); ~9 on the relocation routes; under 1 on the counting routes;
 *                ~1.3 on the short-reach route (index blocks sorted where they lie), which is 0.1 at one thread
 *   a value that holds most of the array does not change that (it was 104 bytes per element at 14 threads until
 *   2026-09-20: every thread's context was sized by the largest bucket).
 * This header used to say "~9n". The buffers are kept between calls; logos_release_buffers() returns all of them.
 * Sizes: n is a size_t, but the buffered engine counts in 32 bits, so arrays above 2^32 - 1 elements are handed to the
 * strict in-place sort (untested at that size: no machine here holds one).
 * Resource exhaustion, TESTED (make faultprobe fails every allocation and every thread creation of a cold call in turn;
 * the harness repeats a sample of that on every make test): a failed malloc or pthread_create costs speed, never
 * correctness. The array still comes back sorted, by a route that needs less, down to an in-place sort that needs
 * nothing. There is no error return because there is no failure left to report. */
void logos_sort_f64(double *a, size_t n, int nthreads);

/* Diagnostic, for tests: what the short-reach route (sort index blocks where they lie, repair only the seams; see
 * logos_psort.c) did on this thread's most recent top-level logos_sort_f64 OF AT LEAST 2^18 ELEMENTS. A smaller sort is never
 * offered to the route and leaves the answer as it was: resetting it there would put a thread-local access, which is a
 * function call in position-independent code, on the path of a three-element sort. A test that cannot tell which path ran
 * tests nothing in particular. NOT_TRIED: the sample's ladder found no short reach (or LOGOS_NO_REACH is set).
 * DECLINED_*: the array went on to the other routes; after PLAN its index blocks have been sorted in place. */
enum { LOGOS_REACH_NOT_TRIED, LOGOS_REACH_SORTED, LOGOS_REACH_SORTED_STRAYS, LOGOS_REACH_DECLINED_GATE, LOGOS_REACH_DECLINED_PROBE,
       LOGOS_REACH_DECLINED_PLAN, LOGOS_REACH_DECLINED_NOMEM };
int logos_reach_last(void);

/* Diagnostic, for tests: how many TWISTED strands (runs that lie descending and were consumed as they lie, never reversed in
 * memory: see rdesc_t in logos_psort.c) the knot route (which = 0) or plain relocation (which = 1) has placed since the counts
 * were last cleared (which = -1 clears both and returns 0; which = -2 returns 1 if the twist is compiled into this build).
 * which = 4: KINKS (an adjacent pair swapped inside a run that otherwise goes on) the run detector swapped back since it was last read.
 * which = 6: arrays the DENSE-INTEGER BITMAP placed (distinct integers spanning at most DENSE_SPAN_MULT x n, sequential sorts) since
 * it was last read.
 * which = 5: GALLOPS made by the knot route's two-strand merges (a stretch where one strand won eight in a row and was moved by one
 * bulk copy) since it was last read; 0 with LOGOS_NO_KNOT_GALLOP=1.
 * which = 3: placements that went through the literal linked list (LOGOS_KNOT_LIST=1) since it was last read.
 * which = 7: sorts the LOOP-BACK rotated first (an ascending suffix that continues into the ascending prefix across the wrap, a
 * ring buffer read from the wrong slot: rotated once, then the dispatcher entered again) since it was last read; which = -4 returns
 * 1 if the loop-back is compiled into this build. Always 0 with LOGOS_NO_LOOPBACK=1.
 * which = 8: arrays the WAVE route placed; 9: sorts the SEAM route rotated first; 10: STRAYS lifted out of strands by routes that
 * completed; 12: sorts finished as ONE STRAND (a single run after the kinks and strays were taken out). Each reading clears it.
 * Always 0 with LOGOS_NO_TWIST=1, which restores the in-place reversal. Not thread-safe across concurrent sorts, like the
 * buffer pool. */
size_t logos_twist_last(int which);

/* Diagnostic, for tests: how many sequential sorts since the last reset were MIRRORED first (the whole array's orientation bit:
 * the sample's writhe said back to front and a directional gate fired on the mirror image, so the array was reversed once and
 * the dispatcher entered again; see mirror_gate in logos_psort.c). Always 0 with LOGOS_NO_MIRROR=1. Not thread-safe across
 * concurrent sorts, like the buffer pool. */
size_t logos_mirror_count(int reset);
/* Test hook: 1 if the dispatcher's prescan in blocks and its scalar line agree on this array (ascending?, descending?, first
 * descent), for EVERY kind of block this CPU runs (the scalar OR of sixteen, SSE2 compares, AVX2 compares). The harness runs
 * it on every position near both ends and every offset of a block. 2026-09-22. */
int logos_prescan_agree(const double *a, size_t n);

/* NaN with a defined order: NaNs are moved to the tail with their bit patterns intact (one sequential pass), then the
 * NaN-free prefix is sorted. logos_nan_to_end_f64 is that pass alone and returns the length of the prefix. */
size_t logos_nan_to_end_f64(double *a, size_t n);
void logos_sort_f64_nan_last(double *a, size_t n, int nthreads);

/* Partition only, for building a lazy sorted view: see logos_view.h. Fills starts[] with nbk+1
 * boundaries and returns nbk, or 0 if the array came out fully sorted. starts[] needs
 * LOGOS_MAX_VIEW_BUCKETS+1 entries. */
#define LOGOS_MAX_VIEW_BUCKETS 256
int logos_partition_f64(double *a, size_t n, int nthreads, size_t *starts, double *bmin, double *bmax);
/* Sort placed buckets in place with the engine's leaf, one context per thread; done[b] != 0 skips b. */
void logos_sort_buckets_f64(double *a, const size_t *starts, int nbk, unsigned char *done, int nthreads);
/* The int32 twins. psort_i32 runs the histogram partition, so the boundaries are exact prefix sums;
 * returns 0 when the array came out sorted (counting route, sequential leaf, prescan, fallback). */
int logos_partition_i32(int32_t *a, size_t n, int nthreads, size_t *starts, int32_t *bmin, int32_t *bmax);
void logos_sort_buckets_i32(int32_t *a, const size_t *starts, int nbk, unsigned char *done, int nthreads);

/* Sort n signed 32-bit ints ascending, in place; 4-byte elements throughout.
 * Memory, MEASURED (make memprobe, per 4-byte element, on top of the input): ~11.5 bytes at 1 thread (4 scratch + 7.5
 * sequential context), ~6.5 at 14 threads on the partition, ~13.6 when one value holds most of the array, under 0.1 on the
 * counting route. This header used to say "~5n". The same resource-exhaustion contract as logos_sort_f64. */
void logos_sort_i32(int32_t *a, size_t n, int nthreads);

/* Strict in-place variants: no n-sized scratch (MEASURED: 0 bytes of heap at 1 thread, a fixed 96 KB task stack in
 * parallel) instead of the 15 to 23 bytes per element of the default buffered sort, at a measured speed cost: random-
 * access swaps against a sequential scatter. Use when memory, not time, is the constraint, e.g. sorting toward 1e9
 * elements on a machine that cannot hold 2n. Ascending, in place. Parallel from 2026-09-20 for n >= 2^20 (placement passes
 * as tasks; the first pass stays on one thread): MEASURED at 14 threads against its own 1-thread form, 2.5x at 8.4M and
 * 2.85x at 50M on uniform doubles, 1.1-1.6x on structured shapes, and still 6-10x slower than the buffered sort at the
 * same 14 threads (make ipbench). With a NaN present the order is unspecified, as for logos_sort_f64. */
void logos_sort_f64_inplace(double *a, size_t n, int nthreads);
void logos_sort_i32_inplace(int32_t *a, size_t n, int nthreads);

/* Number of online CPUs — a sensible default for nthreads. */
void logos_set_simd_sort(void (*f64)(double *, size_t), void (*i32)(int32_t *, size_t));
void logos_enable_simd(void);          /* registers the SIMD leaf if built with it */
void logos_set_simd_kernels(void (*bkcount)(const double *, long, long, double, double, int, uint16_t *, size_t *), void (*orand)(const double *, size_t, uint64_t *, uint64_t *));
void logos_enable_simd_kernels(void);  /* registers the vectorized bucket-index + OR/AND kernels (logos_simd.cpp) */

int logos_num_cpus(void);

/* Free everything the engine keeps between calls: the scratch pool, the partition's bucket regions and the per-thread
 * contexts (measured: nothing is left afterwards; it used to free the 9 bytes per element of the pool and keep the rest).
 * Call it when no sort is running. */
void logos_release_buffers(void);

/* Aliases from the withdrawn 4.0.0 camelCase revision; deprecated. */
#if defined(__GNUC__) || defined(__clang__)
#  define LOGOS_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#  define LOGOS_DEPRECATED(msg)
#endif
LOGOS_DEPRECATED("use logos_sort_f64") static inline void logosSortF64(double *a, size_t n, int t) { logos_sort_f64(a, n, t); }
LOGOS_DEPRECATED("use logos_sort_i32") static inline void logosSortI32(int32_t *a, size_t n, int t) { logos_sort_i32(a, n, t); }
LOGOS_DEPRECATED("use logos_num_cpus") static inline int  logosNumCpus(void) { return logos_num_cpus(); }


/* ---- parity extras (logos_extra.c): indirect sort, permutations, outliers,
 * strings. argSort is stable; perm indices are uint32 (n < 2^32). ---- */
void logos_argsort_f64(const double *a, size_t n, uint32_t *perm);      /* perm sorts a; a unmodified */
void logos_argsort_i32(const int32_t *a, size_t n, uint32_t *perm);
void logos_apply_perm_f64(const double *src, const uint32_t *perm, size_t n, double *out);   /* out[i]=src[perm[i]] */
void logos_apply_perm_i32(const int32_t *src, const uint32_t *perm, size_t n, int32_t *out);
void logos_inverse_perm(const uint32_t *perm, size_t n, uint32_t *inv);
size_t logos_find_outliers_f64(const double *a, size_t n, uint32_t *out, size_t cap, size_t budget); /* returns the count, or SIZE_MAX when its two index arrays could not be allocated */
void logos_sort_strings(const char **a, size_t n);                       /* lexicographic, in place */

/* ---- per-phase profiling (only populated when the engine is compiled with
 * -DLOGOS_PROFILE; otherwise logos_profile_get() returns NULL). One record per
 * logos_sort_f64 call: which top-level route ran, wall time per phase, and for
 * every thread its working time, its time waiting at barriers, and the leaf
 * path each bucket took. `make profile` turns this into profile-report.html. ---- */
#define LOGOS_PROF_MAX_THREADS 256
enum { LOGOS_PH_PRESCAN, LOGOS_PH_LOWCARD, LOGOS_PH_MODCARD, LOGOS_PH_NEARSORTED, LOGOS_PH_RELOCATE, LOGOS_PH_SAMPLE,
       LOGOS_PH_HIST, LOGOS_PH_COMBINE, LOGOS_PH_SCATTER, LOGOS_PH_BUCKETS, LOGOS_PH_COPYBACK, LOGOS_PH_OTHER, LOGOS_PH_RELOC_PROBE,
       /* the lattice route, split: the lumped phase could not say which part the idle threads were in */
       LOGOS_PH_LAT_MAP, LOGOS_PH_LAT_MERGE, LOGOS_PH_LAT_EMIT, LOGOS_PH_LAT_FREE, LOGOS_PH_N };
enum { LOGOS_PATH_NONE, LOGOS_PATH_SORTED, LOGOS_PATH_COUNTING, LOGOS_PATH_INT32_RADIX, LOGOS_PATH_INSERTION, LOGOS_PATH_RUN_MERGE,
       LOGOS_PATH_RUN_RELOCATE, LOGOS_PATH_MOMENTUM, LOGOS_PATH_GAP_SPLIT, LOGOS_PATH_FLASH, LOGOS_PATH_BITS_COARSE, LOGOS_PATH_COARSE,
       LOGOS_PATH_QUICKSORT, LOGOS_PATH_QUANT_RADIX, LOGOS_PATH_SIMD, LOGOS_PATH_N };
enum { LOGOS_ROUTE_NONE, LOGOS_ROUTE_SORTED, LOGOS_ROUTE_LOWCARD, LOGOS_ROUTE_LATTICE, LOGOS_ROUTE_MODCARD, LOGOS_ROUTE_SEQUENTIAL,
       LOGOS_ROUTE_NEARSORTED, LOGOS_ROUTE_RELOCATE, LOGOS_ROUTE_PARTITION, LOGOS_ROUTE_REACH, LOGOS_ROUTE_N };
typedef struct {
    size_t n; int nthreads; int route; double total_ms;
    double phase_ms[LOGOS_PH_N];                                   /* wall time per phase, as the calling thread sees it */
    double busy_ms[LOGOS_PROF_MAX_THREADS][LOGOS_PH_N];            /* per thread: time doing work inside the phase */
    double wait_ms[LOGOS_PROF_MAX_THREADS][LOGOS_PH_N];            /* per thread: time blocked at a barrier inside the phase */
    double cpu_ms[LOGOS_PROF_MAX_THREADS][LOGOS_PH_N];             /* per thread: CPU time actually received inside busy (busy - cpu = preempted) */
    double path_ms[LOGOS_PROF_MAX_THREADS][LOGOS_PATH_N];          /* per thread: leaf time by the path the leaf took */
    size_t path_calls[LOGOS_PROF_MAX_THREADS][LOGOS_PATH_N];
    size_t path_elems[LOGOS_PROF_MAX_THREADS][LOGOS_PATH_N];
} logos_profile_t;
extern const char *const logos_profile_phase_names[LOGOS_PH_N];
extern const char *const logos_profile_path_names[LOGOS_PATH_N];
extern const char *const logos_profile_route_names[LOGOS_ROUTE_N];
void logos_profile_reset(void);
const logos_profile_t *logos_profile_get(void);

#ifdef __cplusplus
}
#endif

#endif /* LOGOS_PSORT_H */
