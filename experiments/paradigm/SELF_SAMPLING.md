# The algorithm sampling itself: compare halfway, commit, optimise the rest

The question: what would it look like for a pass to observe its own first half and
run its second half differently? Measured on the one place the profiler says matters
most (the top-level bucket lookup, `splitter_crux.cpp`), then generalised.

## The pattern

A pass over n elements is already a sample of the data, and its first chunks are
a sample of the *machine* running this code on this data. So:

```
for a pass that must touch every element anyway:
    chunks [0, k):         run variant A on some, variant B on others, or run the
                           exact/general variant while accumulating cheap side
                           statistics (a counter per cell, a timer per chunk)
    one decision (<1 ms):  pick the winner / build the specialised structure /
                           re-balance / abort the route
    chunks [k, n):         run the chosen variant, guarded by a branchless check
                           or a budget that reverts if the assumption fails
```

In SIMD terms this is the same as "compare halfway": the first half of the work is
the trial, the comparison is one serial step, the second half is the optimised
kernel. The guard is what keeps it correct rather than merely fast.

## Where it does NOT help: building the address table

The bucket ids are defined by 79 splitters that are quantiles of a 2560-sample.
A direct-index table over the splitters can be built from the splitters alone
(0.1 ms), and its refinement follows splitter density, which *is* data density
because the splitters are quantiles. Counting cell hits during the first half
(variant c below) buys nothing: same lookup cost, same table. The data already
sampled itself when the splitters were chosen.

## Where it DOES help: choosing between two tables at runtime

Two tables give the same ids by different routes: cells over the *value* range
(one multiply, one load; flat data resolves at depth 1) and cells over the
*order-preserving key* (log-magnitude resolution; skewed data resolves shallow,
flat data goes deeper). Neither wins everywhere, and which one wins depends on
the data *and* the branch predictor's behaviour on it, which no probe predicts
well. So build both (0.3 ms), time each on 1/32 of the elements, run the rest
on the winner.

`splitter_crux.cpp`, 8M doubles, 80 buckets, one thread, nanoseconds per element
for the bucket id, every variant's ids checked identical to the engine's search
(machine loaded, so (a) reads ~2× its quiet value; ratios are what matter):

| shape | (a) engine search | (b) value table | (c) half-sampled build | (d) key table | (e) self-chosen | chose |
|---|---|---|---|---|---|---|
| uniform | 43.6 | **4.7** | 25.9 | 19.0 | 5.3 | value |
| int_wide | 53.9 | **2.7** | 35.4 | 11.4 | 5.3 | value |
| heavy_tail | 55.3 | 24.8 | 39.7 | **11.6** | 13.3 | key |
| bimodal | 53.2 | 26.2 | 47.5 | **17.5** | 16.6 | key |
| clustered | 61.7 | **5.1** | 21.6 | 18.0 | 5.3 | value |

Readings:
- The self-choosing pass (e) is within 10% of the best table on every shape and
  3–10× faster than the shipped search everywhere. It never chose wrong.
- (c) confirms the argument above: sampling the first half to build the table
  is pure overhead.
- Predictions, written first: search 25–35 ns quiet (measured 44–62 loaded:
  consistent); value table 3–4 ns on flat shapes (4.7 / 2.7 / 5.1: right);
  heavy_tail 6–10 (value table 24.8: wrong, three levels of equi-width cells
  cannot resolve a Pareto head; the key table's 11.6 is the right structure);
  bimodal 4–6 (17.5: wrong, the two clusters alternate randomly so every
  data-dependent branch in the lookup mispredicts; a branchless lookup is the
  next step there).

What this means for the engine (H0b in the truth table): at 20 threads the
histogram was ~100 ns/element of CPU, 63% of the uniform sort's wall. A 5 ns
lookup takes that phase from ~49 ms to ~5 ms wall on this box, i.e. the whole
20-thread uniform sort from ~78 to ~35 ms. The prediction stands at 1.5–2× on
every unstructured parallel row; the crux says the lookup side of it is real.

## The four places the pattern applies in this engine

1. **Kernel selection by trial** (this crux). Any phase with two plausible
   kernels: value vs key table; write-combining on/off (today gated on thread
   count, measured to lose 8–35% sequentially and win 5–12% at 20T, a runtime
   trial on two chunks would pick per machine); prefetch distance (pfbench's
   five variants become a 5-chunk trial). The cost is one extra kernel on 1/32
   of the data.

2. **Over-partition and regroup**. The histogram already produces exact counts;
   sampled quantile splitters give buckets of unequal size, and the profile shows
   the bucket phase's per-thread busy time ranging 10–18 ms on uniform (the wall
   is the max). Compute 4× more fine buckets in the histogram, then group
   adjacent fine buckets into P equal-work ranges *after* seeing the counts.
   Same scatter, no extra pass, imbalance gone. Predicted 5–8% at 20T.

3. **Probes piggybacked on the first chunk**. The lowcard / modcard / lattice /
   relocation gates each cost a separate sample or partial pass. The first
   chunk of the histogram can carry a hash sketch (distinct count), an OR/AND of
   key bytes (quantisation), min/max, and a run count per thread. Route after
   chunk 0, on n/P real elements instead of 2048 samples: closes the
   65k–1M-distinct gap (F7) and drops three probe passes. Predicted 3–5% on
   unstructured shapes and a new counting win on mid-cardinality data.

4. **Learned placement with a measured guard** (J8). Place the first half by a
   sampled CDF into slack cells, count overflows; if overflow is under a budget,
   continue, else fall back to the exact path for the rest. The first half's
   exact positions are also a far better CDF than the sample for the second half.
   This is the one case where the first half genuinely improves the model,
   because positions (not buckets) are the deliverable.

## The SIMD-like framing

A vectorised kernel is a straight-line loop with no data-dependent branch. The
self-sampling pass keeps that property: the trial runs two straight-line kernels
on disjoint chunks, the decision is scalar and happens once, and the committed
kernel is again straight-line. The bimodal row shows why this matters: the
value-table lookup has a data-dependent branch per level and mispredicts on
alternating clusters; the next version of the lookup should compute both
candidate results and select, so the committed kernel is branchless and the
trial measures only memory behaviour.

## Not measured here

Multi-threaded numbers (the crux is one thread; at 20T the search costs more
per element, 70–105 ns in the profile, so the ratio should widen), the integrated
A/B in the engine (H0b), and the branchless lookup for the bimodal case.
