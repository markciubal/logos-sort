# Short reach — what else looks like run detection and unknotting?

Round three, 2026-09-20. Companion to `ORDER_FROM_CHAOS.md` and `OUTSIDE_THE_BOX.md`, same rules: every idea
carries a written prediction before anything is measured, and a measured dead end is a deliverable.

The question was abstract: the knot route worked, so what other views of an array work the same way? Six
candidates were written down with predictions. The first step was a ceiling bench, not a route. One candidate
was then built and ships; two were measured and are not built; three were not measured. Defects in code that
was already shipping turned up on the way and are listed at the end.

Words used below. The **reach** of an array is the largest index distance between two elements that are out of
order with each other. A **seam** is the boundary between two index blocks that have each been sorted where
they lie. A **stray** is an element far outside the value range of its neighbourhood: a late event, a zero in a
column of timestamps.

## 1. The pattern that transfers

The knot route has four parts, and each candidate below was judged by whether it has all four.

- **A view.** See the array as a geometric object (strands, crossings).
- **An invariant.** A cheap quantity that bounds the comparison work (elements inside crossings).
- **Hardware-friendly moves.** Contiguous copies, short sequential merges, counting.
- **A measured gate.** Set from measurement, with a decline that costs almost nothing.

One ledger constraint applies to every candidate. At 14 threads a route that scans, places and copies back
only ties the partition (relocation and the knot route both do). A new path must skip the scatter, or win at
one thread.

## 2. The six candidates, as written before anything was measured

| # | Analogy | View | What replaces comparing | Prediction | Verdict |
|---|---|---|---|---|---|
| 1 | Prime factors of a composite knot | cut wherever everything before is no larger than everything after | each factor sorts alone, in place | 2-3x at 1 thread, 1.5-2.5x at 14 | **measured, not built**: §3 |
| 2 | A braid whose crossings are all short | sort index blocks in place, look only at the seams | merge the few elements near each seam | 1.5-2.5x at both | **built and shipped**: §4. The prediction landed, for a reason it did not name |
| 3 | Links instead of knots | min and max per unsorted chunk, then the knot route's interval sweep | disjoint chunks move by one copy | near zero cost as a detector | **measured as a ceiling, not built**: §3 |
| 4 | Transposing the diagram | swap index and value | a stable scatter by value band unties strands | 2-3x at 1 thread, a tie at 14 | not measured; §3's links row bounds it |
| 5 | Sorting the description | the run plan is already a sorted view | rank and quantiles from run descriptors | 5-10x for a handful of queries | not measured |
| 6 | Greedy simplification moves | guess a structured permutation, apply, verify | any permutation is a safe move | 3-10x (strided order, `OUTSIDE_THE_BOX.md` C1) | not measured |

What routes between them was also a prediction: the dispatcher's sample can price how far the disorder
reaches, as the knot route priced how many strands cross. That part is built (§4, the ladder).

## 3. Ceilings first (`make reachhunt`)

`engines/c/reachhunt.cpp` includes the zoo of twenty unchanged (the quadtree classifier's held-out test
depends on it) and adds shapes and informed sorts of its own. Every method is told the structure, so its time
bounds what a route could reach. Paired rows, 12 pairs, n = 2^23, every output checked bit-exact against
`std::sort`. Ratio = t(logos) / t(method).

| shape | method | 1 thread | 14 threads |
|---|---|---|---|
| event_ts_latency (displacement <= 4096) | blocks in place + seam repair | 1.28-1.31 | 1.18-1.22 |
| window_shuffle (2048-element windows) | the same | 1.34-1.37 | 1.22-1.24 |
| window_shuffle | cuts, told where they are | 1.64 | 0.30 |
| window_shuffle | cuts, found blind | 1.11 | 0.24 |
| coarse_key_groups (groups of 2k-200k, random inside) | cuts, told | 1.43 | 1.11 |
| regime_switch (3 value bands, reach 512 inside a dwell) | links, told the segments, then blocks | 2.0-2.1 | 1.3-1.5 |
| latency + 0.01% far strays | blocks + seam repair, which must decline | 0.60 | 0.41 |
| uniform (guard) | blind cut detection that finds nothing | 0.98 | 0.89 |

Predictions, graded:

- **P1, blocks on event_ts_latency, 1.2-1.5x / 1.2-1.6x: right** at one thread, low end at 14. The table
  given in conversation had said 1.5-2.5x; a cost model written just before measuring corrected it, because
  the one-thread route on this shape is already a coarse pass plus an in-cache leaf.
- **P2, cuts told on window_shuffle, 2-3x / 1.3-1.8x: wrong both ways.** 1.64x at one thread. 0.30x at 14:
  ranges of 2048 elements fall under the leaf's flash threshold and go to a scalar quicksort when the sort
  is parallel. The same data cut into 33k blocks is 1.24x, so cuts that small are worth less than blocks.
- **P3, blind cut detection costs 8-12 ms / 1.5-2.5 ms: wrong, 35 ms / 9 ms.** A suffix-min and a running
  max are dependency chains of one compare per element; they do not run at the speed of the loads.
- **P4, coarse_key_groups, 1.5-2x / 1.2-1.5x: a little low**, 1.43 / 1.11. The bench's own blind gate
  declined this shape (it asked for cuts in an eighth of the chunks; groups of 43k elements put one in a
  fourteenth), so there is no blind number.
- **P5, links on regime_switch, 1.2-1.5x / 0.8-1.1x: wrong, 2.0 / 1.3-1.5**, and for a reason that is not
  the method's merit: the engine runs this shape at twice its uniform time (three bands 2^40 apart defeat
  the uniformity certificates and the bits-coarse leaf). A ceiling measures the baseline's weakness too.
- **P6, strays make boundary repair decline, 0.5-0.7x: right at one thread, worse at 14 (0.41).** One
  stray at the end of a sorted block makes the whole next block its merge zone.
- **P7, a blind detector costs the guards <= 3% / 5-8%: right / wrong (11% at 14 threads).**

Why cuts and links are not built. Cuts are a special case of blocks: a block boundary that happens to be a
cut has an empty seam. Finding cuts at arbitrary positions costs a pass that blocks do not need, and the only
shape where cuts beat blocks (tiny factors at one thread) is one the block route already wins. Links need
segment detection the ceiling did not price, and most of their margin belongs to the banded-data weakness
above, which is the better thing to fix (§7).

The stop rule was written down in advance: if blocks did not beat the 14-thread time on both zoo shapes, no
parallel route. They did, by 18-24%, so the route was built.

## 4. The route: sort where it lies, repair only the seams (`reach_try_f64`, `make reachbench`)

**The algorithm.** Cut the array into index blocks of B = 32768 (larger when the reach needs it, never above
131072). Sort every block in place, in parallel. For the seam between sorted blocks L and R, y = the elements
of L greater than R[0] and x = the elements of R less than the last of L, by binary search; merging those two
pieces in place sorts L and R together, and what crosses a seam rightward is matched by what crosses
leftward, so nothing else moves. No n-sized buffer is asked for.

**Why the seams cannot fail.** With m(k) the median of sorted block k, an element of block k below m(k-1) or
above m(k+1) is a stray and is set aside first. What is left of block k lies in [m(k-1), m(k+1)], so x(k)
counts elements below m(k), y(k+1) counts elements above it, and m(k) is in neither: every block keeps an
untouched middle, which is the condition under which the seams are independent and the result is sorted.
The ceiling bench could only check that condition and decline; the route guarantees it.

**Strays decide the cost, and a sample cannot see them.**

- None: block sorts and seams, in place.
- A few, found by the block sorts: they are sorted apart and put back by one in-place shift pass. Between
  two strays every element moves by the same amount, so the pass is a handful of `memmove`s over exactly the
  span the strays travel. Left-movers go first, left to right; right-movers second, right to left. The first
  cut merged them through a second buffer: +36 ms at one thread and +12 ms at 14 for a single stray.
- In many blocks: it depends on the thread count. The shift pass is serial, about 0.7 ns per element at most, and the
  route saves ~7 ns per element at one thread and ~0.4 at fourteen. Up to four threads strays are simply put back
  (n/32 at most). Wider sorts hold eight probe blocks against their fences first (the estimated medians of their
  neighbours, nine strided elements each) and decline when two break one: about 0.05 ms at 8M, nothing moved. A rule
  that stopped the block phase when strays kept turning up was measured dominated and removed (strays in 28% of the
  blocks at 14 threads: 1.24x against the partition when put back, 0.78x when the phase stopped itself).
- A block that holds a stray is split three ways before it is sorted. One zero in a block of timestamps
  stretches the leaf's value-linear map until every real element lands in one class (block sorts of
  latency + 0.01% strays: 15.7 ms at 14 threads against 9.2 without them).

**The gate reads the sample and four windows, never the array.** Sample indices i and i+L sit
|frac(L phi)| n positions apart; for the Fibonacci lags 144, 233, 377, 610, 987 and 1597 that is 0.31%,
0.19%, 0.12%, 0.073%, 0.045% and 0.028% of n, a ladder of distances. The reach estimate is the smallest rung
from which no distance shows more than 2% inverted pairs. Random data fails the first rung, which the shared
sample has already read, so it pays nothing. Then four 4096-element windows must show dense adjacent
descents (25%); sparse disorder on sorted data is the near-sorted route's kind and is left to it.

**The block sort was the surprise.** The first cut sorted blocks with the engine's general leaf and measured
1.15-1.53x at one thread. Two findings followed, both by paired A/B inside the route:

- the general leaf's 40-pair probe calls a block nearly sorted when the reach is under its stride of n/40,
  tries an insertion sort that gives up after 8n moves, and keeps the quicksort away. Going straight to a
  kernel: 1.10-1.13x at one thread, **1.97x at 14 threads on latency < 512**;
- on these blocks a **flash sort beats the AVX2 quicksort at one thread by 1.26-1.81x** (a tie on
  window_2048). Classification is nearly the identity on locally ordered data, so counts, scatter and
  insertion pass all stay where the cache already is. The flash here counts its classes before scattering
  and hands a block with a crowded class (above 48) to the general leaf: a block that straddles a quiet hour
  holds two clusters, which the leaf's quantile-spread check would pass.

- the first cut's "a small span means integers" test sent every series with unit spacing (index + noise) to the
  general leaf: 293 ms at one thread on 64-element windows, 76 through the kernel. It now tests eight elements for
  integrality first;
- disorder that reaches two or three places is an insertion sort's: noise of +-2 at one thread measured 0.81 with flash
  alone and 1.10 with an insertion kernel chosen by 64 strided pairs four apart.

Block size: 16384 against 32768 is neutral (0.96-1.05), 65536 is 0.85-1.00. 32768 stays.

**Measured** (`make reachbench`, route off / route on, one binary; 8.4M with 30 pairs on a day whose null test read 6.0%,
30M with 12 pairs, 1M with 120):

| shape | 8.4M, 1 thread | 8.4M, 14 threads | 30M: 1 / 14 | 1M: 1 / 14 | 8.4M at 4 / 7 threads |
|---|---|---|---|---|---|
| latency_4096 (event log, latency < 4096 ms) | **1.91** | **1.31** | 2.53 / 1.51 | 1.00 / 1.00 (*) | 1.61 / 1.63 |
| window_2048 (shuffled inside windows) | **2.22** | **1.38** | 2.72 / 1.79 | 1.94 / 1.38 | 1.79 / 1.82 |
| jitter_1000 (index + noise of +-1000) | **2.05** | **1.33** | 2.13 / 1.52 | 1.81 / 1.25 | 1.50 / 1.43 |
| latency_512 | **2.38** | **1.49** | 2.86 / 1.70 | 2.21 / 1.38 | 1.82 / 1.68 |
| window_64 (`TRUTH_TABLE` J6's shape) | 2.46 | 1.48 | 2.86 / 1.83 | 2.45 / 1.34 | |
| jitter_2, jitter_8, jitter_30 | 1.10, 2.54, 2.17 | 1.80, 2.35, 1.31 | | | |
| ticks_dup (jittered one-second ticks) | 2.80 | 1.02 (a counting route takes it first) | | | |
| one_stray (latency_4096 + one far element) | 1.81 | 1.14 | 2.44 / 1.31 | | 1.49 / 1.39 |
| strays in 0.001%, 0.01%, 0.1%, 1% of elements | 1.77, 1.57, 1.49, 1.23 | 1.26, 1.00, 1.01, 1.04 | | | 1.28 / 1.01 (0.01%) |
| 0.05% zeros; 8, 84, 840 zeros; 84 late events | 1.58; 2.28, 2.35, 1.54; 1.77 | 0.99; 0.96, 0.97, 0.99; 1.10 | | | 1.15 / 1.01 (0.05%) |
| reach_40000 (needs blocks above the cap) | 1.01 | 0.96 | | | |
| uniform, near_1pct, coarse_groups, regime_switch, stairs_x20, block_perm64, heavy_tail, few_distinct | 0.99-1.03 | 0.94-1.02 | 1.02 / 0.98 | 1.00 / 1.01 | |

(*) a reach of 4096 is longer than the first rung of a 1M ladder, so the gate declines from the sample.

Against the engine as it was before this round (two copies, both orientations, layout cancelled): 0.98-1.03 on nine
guard shapes at 8M and 0.98-1.02 at 1M.

Predictions in `reachbench.c`, graded: the four target rows were predicted 1.10-1.45 at one thread and 1.15-1.40 at 14
from the ceilings. Right at 14 threads, **low at one thread everywhere**, because the ceilings ran the general leaf.
ticks_dup 2-3x at one thread was right; 1.3-1.8x at 14 was wrong (another route wins the dispatch). one_stray was
predicted 0.95-1.10 / 0.70-0.90 for the buffered merge and measured 1.81 / 1.14 after the shift pass replaced it.
Strays "0.97-1.00 everywhere" was right at 14 threads and wrong at one, where they are now put back. Guards: right.

**What it weighs, and what happens when resources run out.** `make memprobe`: 0.10 bytes per element at one thread and
1.33 at fourteen (10M), against 15.5 and 23.2 on the partition. See also
`make faultprobe` (480 failed allocations and 12 failed thread creations over the two short-reach cases, none
ending badly). Every branch of the route is asserted BY NAME in the harness through `logos_reach_last()`:
the first version of that section passed with the route never firing at one thread, because the gate's
sample is 384 points there and the ladder needs 2048.

## 5. Limits, and what is still open

- f64 only. The int32 path has no relocation, knot or short-reach route.
- The ladder's distances are fractions of n and blocks are capped at 131072, so above about 117M elements the
  gate declines even when the reach is short. A rung measured inside the probe windows would lift that.
- The stray shift pass is serial. One stray costs little; a few hundred spread over a 30M array cost one
  pass at `memmove` speed.
- Candidates 4, 5 and 6 of §2 are unmeasured. Candidate 5 (rank and quantiles from run descriptors) is the
  one that fits the project's strongest direction.

## 6. A sine wave with a slope: two scales of order that no single route sees (measured, not built)

Asked while this round was being verified: runs that rise and fall like a sine wave on a slope (a seasonal metric with drift), what
does that say about finding other patterns? And, asked afterwards, does that shape actually COST anything? It does, at one thread.
value(i) = i + A sin(2 pi i / T) + noise, n = 4M, `make sineprobe`.

**How it is measured, after the first cut got it wrong.** The first cut printed one absolute time per shape and invited the reader
to compare them ("70 ms with noise against 54 ms uniform"). That is a comparison across rows of one run, which methodology rule 2
forbids: rows are measured seconds apart on a box that wanders by a multiple. Every shape is now sorted ADJACENTLY with a uniform
reference of the same size and value range, the order alternated across 40 pairs, and the median of t_shape / t_uniform reported.
Two controls decide whether a row means anything: a NULL (a second uniform instance against the first) and LINEAR + the same noise
and no wave. Two runs, both quoted.

| shape | pieces | depth | reach | 1 thread | 14 threads |
|---|---|---|---|---|---|
| uniform, a second instance (THE NULL) | | | | 0.99 / 1.00 / 1.00 | 1.01 / 1.02 / 0.99 |
| linear + noise 50, no wave (the control) | 1 | 1.0 | 100 | **0.63 / 0.59** | 0.60 / 0.72 |
| 32 cycles, swing 0.6 of a cycle's climb, no noise | 65 | 2.2 | 0 | 0.35 / 0.33 | 0.82 / 0.88 |
| the same + noise of +-50 elements | 65 | 2.2 | 104 | **1.31 / 1.22 / 1.27** | 1.04 / 1.03 / 1.00 |
| the same VALUES, index order destroyed (the second control) | 1971 | 229.9 | 1 | **1.01** | 1.04 |
| swing = 2 climbs + noise | 65 | 4.7 | 23 | 1.10 / 1.09 | 0.85 / 0.80 |
| swing = 8 climbs + noise | 65 | 15.2 | 6 | 1.07 / 1.03 | 1.04 / 1.03 |
| 2000 cycles + noise (the wave is inside the reach: the short-reach route takes it) | 1 | 1.0 | 100 | 0.76 / 0.98 | 0.81 / 0.79 |

- **The wave with noise costs 1.22-1.31x a sort of random data at ONE thread**, in three runs, p10 above 1.13 in all of them. Clean,
  the same wave is 0.32-0.35, so fifty elements of noise are worth a factor of 3.7. **Two controls say what that cost belongs to.**
  The SAME noise on a plain slope is 0.59-0.63, faster than random, because the short-reach route takes it: so it is not the noise.
  The SAME VALUES with the index order destroyed read 1.01, level with random: so it is not the value distribution either. What
  costs 27% is the ORDER, and this array is **slower than the same numbers shuffled**.
- **Where it goes** (`-DLOGOS_PROFILE`, paths and element counts only): the natural order takes the **coarse** leaf, 4.19M elements
  in one call; shuffled, the same multiset takes the **AVX2 quicksort** leaf; uniform takes the AVX2 leaf too. In a profile build
  that is 126 ms against 87 and 90. The cause is one clause: `AVX2_TAKE` requires `!c->nearly`, and `sort_fn`'s 40 strided pairs
  call the wave nearly sorted, because its overall slope makes a pair 1/40 of the array apart ascending almost always. The rule is
  right for data that IS nearly sorted (coarse's near-sequential scatter beats a quicksort by 1.23x there) and wrong here.
- **The fix is already half-built and unmeasured.** This round added a dense-disorder probe to the same function, 64 strided pairs
  eight apart, precisely to tell "nearly sorted" from "densely disordered but shorter than the stride" (the noisy wave scores about
  27 of 64, well over its threshold of 16). It gates the insertion sort only. Letting it also overrule `!c->nearly` for the AVX2
  leaf is a one-line change to a macro plus carrying `dense` in the context. **Predicted: ~1.45x on this shape** (1.27 -> about
  0.88, i.e. faster than random), nothing on genuinely near-sorted shapes, where the dense probe reads false. It needs its own
  paired A/B against the near-sorted guards before anyone believes it.
- **The cost tracks the REACH, not the swing**: reach 104 -> 1.22-1.31, reach 23 -> 1.09-1.10, reach 6 -> 1.03-1.07. A deeper swing
  steepens the local slope, so the same noise displaces fewer positions. And a wave short enough to sit inside the reach (2000
  cycles) is taken by the short-reach route at 0.76-0.98.
- **At 14 threads the cost is gone** (1.03-1.04, inside the null's band), while the other structured shapes are 1.1-1.7x faster than
  random there. The whole finding is a one-thread finding, which is where the chaos map already said the multiples are (0b2).
- Why: the noise breaks every run, so the knot route sees no strands, and the descending half-cycles put long-range inversions in the
  sample, so the short-reach ladder fails its first rung and never fires ("not tried" in the gate column). Each route sees one scale
  and is blinded by the other. All the structure is still there and costs microseconds to read from a 4096-point index-ordered
  sample: 65 monotone pieces, 2.2 of them covering a value, a reach of ~100 inside each.
- What it indicates about finding patterns: every route so far is a statement about ONE property of the index -> value curve. The
  next ones are statements about the SMOOTHED curve plus a residual, which is the trend + season + residual of time-series
  analysis read as a sorting plan: the trend makes cycles climb (a staircase, the knot route's shape), the season makes strands
  (one per half-cycle, alternately descending), the residual is short reach inside a strand (this round's route). Three numbers
  choose the plan: pieces, how many pieces cover a value (the merge depth; past ~3 the ledger says stop), reach inside a piece.
- A period is also an invariant. If run lengths repeat with period T and the slope is positive, elements at equal phase form
  ascending streams at stride T, which turns the "guessed permutation" candidate (section 2, row 6) from a guess into a test.
- **The ceiling for a composed route** (strand boundaries from the smoothed sample, short-reach blocks inside strands, the knot sweep
  across them) is now bounded by measurement rather than guessed: from 1.27 to where the same noise lands on a plain slope (0.61) is
  **2.1x**, and to where the clean wave lands (0.34) is **3.7x**, at one thread. Take the leaf-gate fix above first, though: it is one
  line, it collects about 1.45x of that, and what the composed route would still be worth afterwards is 2.6x, not 3.7x; nothing at 14, and nothing once three or more cycles
  overlap (swing 8, depth 15.2, already reads 1.03-1.07). Where the local slope vanishes (crests and troughs) the reach blows up, so
  those blocks will hold the strays. Unbuilt.
- **Predictions graded** (they were written into `sineprobe.c` before the rewritten bench was run): the null, the clean wave and the
  noisy wave landed inside their ranges, and the discarded across-row reading of 1.30 turned out to be right. Two were WRONG.
  Linear + noise was predicted 0.25-0.50 and measured 0.59-0.63, because I had read across from the short-reach route's own A/B
  (2.05x on jitter, route on against route off) as if it were the same quantity as shape against random: it is not. And "every row
  at 14 threads within 0.85-1.15" was wrong on three rows of six, all of them faster than that.

## 7. Defects found on the way

1. **The shared sample's inversion rate read 7 pairs, not ~1900.** Sample indices i and i+144 are 0.31% of n
   apart, and the later index is the EARLIER position (frac(144 phi) = 0.99689). The `p1 <= p0` skip meant
   to drop the few pairs that wrap around the array dropped all 1,897 adjacent pairs and kept the 7 wrapped
   ones. It passed block-permuted, sawtooth and grouped-run data as nearly sorted and turned away one
   genuinely near-sorted input in seven. Found because the short-reach gate wanted the same statistic at
   other lags and the pair counts came back 7 and 3.
2. **The near-sorted pre-gate had no poison recovery**, and the route it guards has. On sorted data with
   random swaps it declined 12 of 40 seeds at 1% swaps and 38 of 40 at 2%.
3. **Sorted runs grouped by a coarse key ran 5.8x off the ceiling at 14 threads** (70 ms against 12): the
   7-pair statistic let the near-sorted route run, it rejected 73% of the array and declined after 57 ms.
   Found only because the ceiling bench's late decline handed the engine an array of sorted blocks.
4. **The general leaf's nearly-sorted probe bounds inversions at one stride**, so it misjudges any bucket
   whose disorder is dense but shorter than n/40. Inside this route that is avoided; inside the partition's
   leaves it is still there.
5. **The route itself, as first built, cost every SMALL call 9-14%**, which no bench of the round could see
   because they all start at a million elements. Its gate helper's two 16 KB arrays were inlined into the
   dispatcher, whose prologue probes every page of its frame on every call, and its diagnostic's reset put
   two thread-local accesses (function calls under `-fPIC`) on the path of every sort. `make callbench` was
   written to find it; see the CHANGELOG for what that bench can and cannot resolve.

Defects 1 to 3 are repaired and measured in `make nsgatebench`, defect 4 by a probe of 64 pairs eight apart (C and JavaScript),
and defect 5 by `noinline` on the helper and a reset that starts at the route's floor (`make callbench`: 0.98-1.01 at n <= 5);
see the CHANGELOG entry of the same date. Repairing the gates made it possible, for the first time, to measure the near-sorted
ROUTE against the partition across sizes. It loses at 1.5-4M elements once 2% of the data is swapped, so its gate became
size-dependent. It also exposed a harness row that had demanded identical bytes from two parallel runs on NaN data and passed only
because the broken gate made the path deterministic.
