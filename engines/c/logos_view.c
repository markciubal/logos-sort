/* logos_view.c — the sorted view. See logos_view.h for the API.
 *
 * WHY THIS WORKS. A partition establishes a total order BETWEEN buckets and no order WITHIN them, and
 * that is exactly what a rank query needs: bucket b holds precisely the elements of rank
 * [start[b], start[b+1]), so "which bucket holds rank k" is a lookup, not a search, and it is exact
 * without one comparison inside any bucket. Buckets are independent, so sorting one costs one bucket
 * and disturbs nothing else. The same partition property, read in value space, says every element of
 * bucket b is <= every element of bucket b+1, so with each bucket's min and max recorded at placement
 * a VALUE query finds its bucket exactly as well.
 *
 * WHY IT IS WORTH DOING, measured rather than argued. Reaching nbk ordered buckets costs about
 * n*log2(nbk) comparisons and finishing costs n*log2(n/nbk). Traced on this engine at 8M uniform on 14
 * threads: the partition takes 20.2 ms and the leaf phase 34.7 ms of a 54.9 ms sort. So 63% of a sort
 * is in the part a view defers, and the prediction from the logarithms matches what the clock says.
 *
 * WHY A COMPARISON SORT CANNOT JUST DO THIS. Quickselect also narrows by partitioning, but it
 * discovers one boundary at a time and its work is not reusable: a second query starts again. Here the
 * splitters come from a single sample taken before any element moves, every boundary is known at once,
 * and one P-way pass serves every query that follows.
 *
 * WHY A BATCH QUERY EXISTS. A loop of single lookups already sorts each bucket at most once, so a
 * batch of m ranks cannot touch fewer buckets than the loop does. What it can do is sort the buckets
 * it touches side by side instead of one after another: the touched set goes to the engine's bucket
 * sorter with everything else masked as done. Ten quantiles at 8M touch ten of 168 buckets, and ten
 * sequential leaves are a few ms against a ~20 ms partition, so the batch is worth about a tenth over
 * the loop, not a multiple (viewbench measures it with the paired harness).
 *
 * WHY THIS FILE IS SO SHORT. The first version hand-rolled a partition and cost two to four times an
 * entire eager sort: it re-derived, badly, a component this engine spent several sessions tuning. The
 * fix was to stop competing with the engine and call it. `logos_partition_f64` is the engine's own
 * partition with its leaf phase skipped, and `logos_sort_buckets_f64` is its leaf phase on its own.
 * Everything else is bookkeeping, and the bookkeeping is the same for int32, so the body lives in
 * logos_view_impl.h and is included once per type below. */
#define _GNU_SOURCE
#include "logos_view.h"
#include "logos_psort.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef VIEW_MIN_N
#define VIEW_MIN_N 4096              /* below this a view is pointless: just sort */
#endif
#ifndef VIEW_PAR_FORCE
#define VIEW_PAR_FORCE 1             /* batch queries sort their touched buckets in parallel; 0 = serially */
#endif

/* ---- double ---- */
#define VT double
#define VIEW logos_view
#define VFN(name) logos_view_##name
#define VCREATE logos_view_create_f64
#define VS(name) name##_f64
#define VPART logos_partition_f64
#define VSORTB logos_sort_buckets_f64
#define VSORT logos_sort_f64
#define VLO (-INFINITY)
#define VHI INFINITY
#define VNAN(x) ((x) != (x))
#include "logos_view_impl.h"

/* ---- int32 ---- */
#define VT int32_t
#define VIEW logos_view_i32
#define VFN(name) logos_view_##name##_i32
#define VCREATE logos_view_create_i32
#define VS(name) name##_i32
#define VPART logos_partition_i32
#define VSORTB logos_sort_buckets_i32
#define VSORT logos_sort_i32
#define VLO INT32_MIN
#define VHI INT32_MAX
#define VNAN(x) 0
#include "logos_view_impl.h"
