/* logos_view.h — a sorted VIEW: pay for order only where you look.
 *
 * `logos_sort_f64` returns a sorted array. Most callers then take a slice, a median, a top-k or a
 * range count, and throw the rest away. This is the same engine with the last step made lazy.
 *
 * Creating a view partitions the array into buckets that are ordered with respect to each other and
 * unordered inside. That is enough to answer "which bucket holds rank k" exactly, without sorting
 * anything, because bucket b holds precisely the elements of rank [start[b], start[b+1]). Indexing a
 * position sorts the one bucket that contains it and nothing else. Materialising every bucket gives
 * byte-identical output to `logos_sort_f64`.
 *
 * The view also records each bucket's minimum and maximum while placing it, so a VALUE query (rank of
 * x, count in [lo, hi)) finds its bucket exactly too, whatever bucket map the partition chose.
 *
 * Measured at 8M on 14 threads (make viewbench): a median or a top-k through a view is about 2-2.7x
 * faster than sorting and looking, because the traced leaf phase is 63% of a sort and a view defers all
 * of it. Break-even is around sixteen scattered queries. Materialising everything costs what an eager
 * sort costs, since it runs the engine's own leaf.
 *
 * The array is permuted in place by logos_view_create and must outlive the view.
 *
 *   logos_view *v = logos_view_create_f64(a, n, 0);
 *   double med    = logos_view_at(v, n / 2);            // sorts ~n/nbk elements, not n
 *   size_t below  = logos_view_rank(v, 100.0);          // how many elements are < 100.0
 *   const double *top = logos_view_topk(v, 100);        // the 100 largest, ascending, in place
 *   double qs[3]; logos_view_quantiles(v, (double[]){0.25, 0.5, 0.75}, 3, qs);   // one pass, <= 3 buckets
 *   logos_view_all(v);                                  // finish; a[] now equals a full sort
 *   logos_view_free(v);
 */
#ifndef LOGOS_VIEW_H
#define LOGOS_VIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct logos_view logos_view;

/* Partition a[0..n) in place into ordered buckets. threads <= 0 means the engine's default.
 * Returns NULL only on allocation failure. a[] must stay alive and unmodified until free. */
logos_view *logos_view_create_f64(double *a, size_t n, int threads);

/* ---- rank space ---- */

/* The k-th smallest element, 0-based. Sorts the single bucket containing rank k if needed. */
double logos_view_at(logos_view *v, size_t k);

/* Guarantee a[lo..hi) holds the correctly ordered elements of those ranks. */
void logos_view_range(logos_view *v, size_t lo, size_t hi);

/* The k largest elements, ascending, as a pointer into a[]: a + n - k. Sorts the buckets they span. */
const double *logos_view_topk(logos_view *v, size_t k);

/* ---- batch queries: m answers in one pass ---- */

/* out[i] = the element of rank ks[i] (clamped to n-1), any order, duplicates allowed. The cuts are
 * sorted, the buckets they land in are walked once, and every touched bucket is sorted once, the
 * touched set in parallel. m ranks cost the partition plus at most m buckets, usually fewer. */
void logos_view_ranks(logos_view *v, const size_t *ks, size_t m, double *out);

/* out[i] = quantile q[i] in [0,1]: the element of rank round(q * (n - 1)), so q=0 is the minimum,
 * q=1 the maximum, q=0.5 the median (the upper one for even n). Same cost model as logos_view_ranks. */
void logos_view_quantiles(logos_view *v, const double *q, size_t m, double *out);

/* ---- value space ---- */

/* Number of elements strictly less than x. Sorts at most one bucket. NaN x returns n. */
size_t logos_view_rank(logos_view *v, double x);

/* Number of elements in [lo, hi). Sorts at most two buckets. */
size_t logos_view_count_in_range(logos_view *v, double lo, double hi);

/* ---- finishing ---- */

/* Materialise everything with the engine's leaf: afterwards a[] is exactly what logos_sort_f64
 * would have produced, at about the cost logos_sort_f64 would have charged. */
void logos_view_all(logos_view *v);

/* Introspection, for benchmarks and for deciding whether laziness paid. */
size_t logos_view_nbuckets(const logos_view *v);
size_t logos_view_materialised(const logos_view *v);   /* buckets sorted so far */
size_t logos_view_n(const logos_view *v);

void logos_view_free(logos_view *v);

/* ---- the int32 twin: the same API over logos_sort_i32's partition ------------------------------
 * The int32 engine's counting route sorts small-span data outright (span <= 2^18), in which case the
 * view holds one finished bucket and every query is a lookup; there is nothing to defer and nothing
 * to gain. Full-range and wide-span data reach the histogram partition and defer the leaf as f64 does.
 * Rank-space quantiles use the same round(q * (n - 1)) rule. */
typedef struct logos_view_i32 logos_view_i32;
logos_view_i32 *logos_view_create_i32(int32_t *a, size_t n, int threads);
int32_t logos_view_at_i32(logos_view_i32 *v, size_t k);
void logos_view_range_i32(logos_view_i32 *v, size_t lo, size_t hi);
const int32_t *logos_view_topk_i32(logos_view_i32 *v, size_t k);
void logos_view_ranks_i32(logos_view_i32 *v, const size_t *ks, size_t m, int32_t *out);
void logos_view_quantiles_i32(logos_view_i32 *v, const double *q, size_t m, int32_t *out);
size_t logos_view_rank_i32(logos_view_i32 *v, int32_t x);
size_t logos_view_count_in_range_i32(logos_view_i32 *v, int32_t lo, int32_t hi);
void logos_view_all_i32(logos_view_i32 *v);
size_t logos_view_nbuckets_i32(const logos_view_i32 *v);
size_t logos_view_materialised_i32(const logos_view_i32 *v);
size_t logos_view_n_i32(const logos_view_i32 *v);
void logos_view_free_i32(logos_view_i32 *v);

#ifdef __cplusplus
}
#endif
#endif
