# "Parallelize the probe, merge at the end" — measured, and why it doesn't help

The idea (a good one): replace logos's sample-sort — which does a random-access
value **scatter** into buckets — with a **parallel merge sort**: sort P chunks
locally (each with logos's own dispatcher, so structured chunks skip the scatter),
then k-way **merge** the sorted runs. The appeal: a merge is *sequential* access,
which hardware loves, so it should beat the random scatter.

## The crux test (single-threaded, decisive, no parallelism confound)

Both operations move all n elements once. Which is cheaper? Measured directly on
8M doubles, `merge_crux.cpp`:

| runs P | scatter (random write) | loser-tree merge | merge / scatter |
|---|---|---|---|
| 16 | 18.6 ms | 285.6 ms | **15.3×** |
| 32 | 16.9 ms | 352.1 ms | **20.8×** |
| 64 | 16.6 ms | 433.7 ms | **26.2×** |

The merge is a proper **loser tree** (the fastest scalar k-way merger — log₂P
predictable compares, no branchy heap), so this is not a strawman. It still loses
by 15–26×.

## Why — and this is the real finding

Two things, both of which reinforce the project's core thesis:

1. **The scatter was never the villain at this granularity.** A partition into
   P = 16–64 buckets is *not* random — it's P sequential write streams, and P
   cache lines fit in cache, so it runs at ~memory bandwidth (~2 ns/element).
   The "scatter is 64% of the time" finding from early profiling was about the
   *fine-grained* scatter (K = 2048 sub-buckets inside each bucket sort), not the
   cheap top-level partition. Merge sort replaces the **cheap** scatter.

2. **A merge is comparison, and comparison is the expensive operation.** The
   loser tree pays log₂P compares per element *plus* reads from P run-heads
   scattered in memory (P read streams, latency-bound) *plus* a tree traversal —
   ~35 ns/element. That is exactly the comparison cost logos's addressing
   paradigm exists to avoid. **Replacing a near-free scatter with a
   comparison-heavy merge is going backwards.**

And critically, merge sort **doesn't even touch the real cost**: each chunk still
sorts itself with the same fine-grained coarse scatter + multiple passes. So the
restructuring (a) replaces the cheap operation with an expensive one and (b)
leaves the expensive one unchanged. Strictly worse.

## The honest conclusion

The instinct — "avoid the scatter" — was reasonable, but the measurement shows
the scatter isn't the problem; **comparison is, and a merge is comparison.** Even
a perfectly *vectorized* merge (bitonic networks) could at best tie the scatter
(both become bandwidth-bound at ~16 ms), never beat it, and still wouldn't touch
the per-chunk cost. So parallel merge sort is not the lever.

This is why the earlier answer routed to x86-simd-sort instead: on *unstructured*
data there is no structure for logos to exploit and no restructuring within the
"produce a sorted array" frame beats a vectorized quicksort — which loops back to
the paradigm result. logos wins by **not comparing and not scattering** (its
counting/lattice paths), and it can only keep that edge by keeping the deliverable
as *order/structure*, not a merged array. Building the full parallel merge sort
would optimize a measured dead end; the crux settles it here.
