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

/* Sort n doubles ascending, in place. NaN ordering is unspecified (avoid NaN).
 * Uses ~9n bytes of scratch (8n buffer + 1n bucket-index cache). */
void logos_sort_f64(double *a, size_t n, int nthreads);

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
 * Uses ~5n bytes of scratch. */
void logos_sort_i32(int32_t *a, size_t n, int nthreads);

/* Strict in-place variants: ~1n memory (no n-sized scratch) instead of the
 * ~2n of the default buffered sorts, at a measured speed cost (random-access
 * swaps vs sequential scatter). Use when memory, not time, is the constraint —
 * e.g. sorting toward 1e9 elements on a machine that cannot hold 2n. Ascending,
 * in place; nthreads is currently ignored (sequential). */
void logos_sort_f64_inplace(double *a, size_t n, int nthreads);
void logos_sort_i32_inplace(int32_t *a, size_t n, int nthreads);

/* Number of online CPUs — a sensible default for nthreads. */
void logos_set_simd_sort(void (*f64)(double *, size_t), void (*i32)(int32_t *, size_t));
void logos_enable_simd(void);          /* registers the SIMD leaf if built with it */
void logos_set_simd_kernels(void (*bkcount)(const double *, long, long, double, double, int, uint16_t *, size_t *), void (*orand)(const double *, size_t, uint64_t *, uint64_t *));
void logos_enable_simd_kernels(void);  /* registers the vectorized bucket-index + OR/AND kernels (logos_simd.cpp) */

int logos_num_cpus(void);

/* Free the process-wide scratch pool (the largest scratch seen is kept between
 * calls to avoid re-faulting ~9n bytes per sort). Safe to call at any time. */
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
size_t logos_find_outliers_f64(const double *a, size_t n, uint32_t *out, size_t cap, size_t budget); /* returns count */
void logos_sort_strings(const char **a, size_t n);                       /* lexicographic, in place */

/* ---- per-phase profiling (only populated when the engine is compiled with
 * -DLOGOS_PROFILE; otherwise logos_profile_get() returns NULL). One record per
 * logos_sort_f64 call: which top-level route ran, wall time per phase, and for
 * every thread its working time, its time waiting at barriers, and the leaf
 * path each bucket took. `make profile` turns this into profile-report.html. ---- */
#define LOGOS_PROF_MAX_THREADS 256
enum { LOGOS_PH_PRESCAN, LOGOS_PH_LOWCARD, LOGOS_PH_MODCARD, LOGOS_PH_NEARSORTED, LOGOS_PH_RELOCATE, LOGOS_PH_SAMPLE,
       LOGOS_PH_HIST, LOGOS_PH_COMBINE, LOGOS_PH_SCATTER, LOGOS_PH_BUCKETS, LOGOS_PH_COPYBACK, LOGOS_PH_OTHER, LOGOS_PH_RELOC_PROBE, LOGOS_PH_N };
enum { LOGOS_PATH_NONE, LOGOS_PATH_SORTED, LOGOS_PATH_COUNTING, LOGOS_PATH_INT32_RADIX, LOGOS_PATH_INSERTION, LOGOS_PATH_RUN_MERGE,
       LOGOS_PATH_RUN_RELOCATE, LOGOS_PATH_MOMENTUM, LOGOS_PATH_GAP_SPLIT, LOGOS_PATH_FLASH, LOGOS_PATH_BITS_COARSE, LOGOS_PATH_COARSE,
       LOGOS_PATH_QUICKSORT, LOGOS_PATH_QUANT_RADIX, LOGOS_PATH_SIMD, LOGOS_PATH_N };
enum { LOGOS_ROUTE_NONE, LOGOS_ROUTE_SORTED, LOGOS_ROUTE_LOWCARD, LOGOS_ROUTE_LATTICE, LOGOS_ROUTE_MODCARD, LOGOS_ROUTE_SEQUENTIAL,
       LOGOS_ROUTE_NEARSORTED, LOGOS_ROUTE_RELOCATE, LOGOS_ROUTE_PARTITION, LOGOS_ROUTE_N };
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
