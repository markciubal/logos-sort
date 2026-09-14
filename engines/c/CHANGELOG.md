# Changelog

## (unreleased) — "heavy_tail is benchmarking very slow": no regression, and a key grid that makes it 1.3-1.8x faster (`make kgbench`)

### The investigation: nothing had regressed
- The scale report (2026-09-13 02:01) had heavy_tail at 14 threads and 10M at 104 ms against uniform's 19.9 and the best
  opponent's 64.5. Runtime toggles for every change of the previous day moved it by nothing; a build matrix over the
  partition's compile flags found no culprit (and a box drifting 3.4x within the run).
- The per-phase profiler settled it (`htprof.c`, 12 interleaved sorts each): the scatter cost heavy_tail 57.3 CPU ns per
  element against uniform's 28.1, **2.04x, exactly the ratio of the 2026-09-11 profile** (11.0 vs 5.4 ms). Uniform passes a
  certificate and buckets with one multiply; heavy_tail passes none and pays the key splitter table.
- What made it LOOK like a regression: heavy_tail's scatter CPU varied 2.4x across reps on identical data where uniform's
  held within 20% (dependent memory loads stall under contention, a multiply does not); the whole box ran about 4x slower
  than on 09-11; and the report compares rows measured minutes apart, at 3 reps for 10M and 1 for 30M.
- There is no "no structure, abort" rule: each route declines on its own, mostly from the shared sample, so a heavy tail runs
  every certificate, fails them all, and takes the most memory-hungry lookup.

### The fix: a key grid (mode 10 of the histogram-free partition), shipped
- Equal-width cells in total-order KEY space, a roughly logarithmic scale, with each bucket edge snapped to the cell that
  holds a sample quantile. A lookup is one subtract, one shift and one byte load; the table never decreases, so the partition
  is correct whatever the balance. It is taken after the certificates and before any splitter-table trial, only when the
  balance check passes on the sample. `LOGOS_NO_KGRID=1` disables it at runtime; `-DLOGOS_NO_KGRID` removes it.
- It is the exponent map (mode 8) done right. That map used one linear piece per binade and lost 0.80-0.91x, because a heavy
  tail decays inside each binade too; the grid puts hundreds of cells in a binade (11429 cells on heavy_tail at 10M).

| grid off / grid on, 14 threads | 8.4M | 1M | predicted |
|---|---|---|---|
| heavy_tail (Pareto 1.2) | **1.36** | **1.83** | 1.2-1.35 |
| lognormal (sigma 2) | **1.40** | **1.67** | 1.1-1.3 |
| pareto_0.8 | **1.33** | **1.75** | 1.1-1.3 |
| exponential | **1.32** | **1.54** | 1.1-1.25 |
| bimodal, mixed-sign heavy tails, uniform | 1.00-1.01 | 0.99-1.00 | 1.00 |
| clustered, int_wide | 0.99-1.00 | 1.01-1.03 | 0.95-1.2 |
| every shape at 1 thread | 0.98-1.05 | 0.98-1.01 | 1.00 |

- **Prediction and grade.** Direction right on every row; magnitudes under-predicted, slightly at 8.4M and by far at 1M, where
  the splitter table's per-element fixed cost is a larger share of the sort. The guards and the one-thread rows read 1.00 as
  predicted. Floor 9.8% before the run (a noisy start) and 3.4% after, at 8M.
- Inferred, not measured head to head: against the scale report's 1.62x deficit to the best opponent on heavy_tail at 10M,
  a 1.36x gain leaves roughly 1.2x.
- Harness: 32 key-grid checks (Pareto 0.5, 1.2 and 3, lognormal, exponential, a negative heavy tail, heavy tails on both signs,
  and a range the grid cannot resolve), at 4 and 8 threads, ascending plus the bitwise multiset. 1568 checks.
- Found along the way, not fixed: `-DLOGOS_NO_SPLITTER_TABLE` no longer compiles (later code assumes the tables exist).

## (unreleased) — the hash-count route's gate, a signed-zero correctness defect, and a broken bench build (`make cardgate`, `make zerodiag`)

### The hash-count gate: measured first, then rebuilt from the measurement
- **The assignment was the Zipf abort** (30.5 of 68.9 ms at 14 threads spent on a hash map that aborts). The ground truth
  said that was the SMALL problem. With the route switched off at runtime (`LOGOS_MC_OFF=1`), at 14 threads the hash map
  LOST to the fallback on 7 of 8 shapes that reached it: 5.30x on 60k equal-odds doubles, 3.81x on lognormal cents,
  2.45x on 30k, 1.90x on Zipf user ids, 1.62x on the 65536-value lattice. My written predictions were mostly wrong and
  all in one direction; the route earned only on a Pareto of small integers (0.28x).
- **Where the lines are** (`ladder` and `skew` modes). At 14 threads the serial merge decides: a sample Chao1 of 2734 or
  less earned or broke even, 3979 or more lost, skewed or not. At ONE thread the hash map lost to the fallback on every
  equal-odds shape from 300 distinct values up and on Zipf activity over random doubles from 1k up.
- **Why one thread is different** (`intness` mode). It is not the data but the FALLBACK. `leaf_f64` sends data whose key
  bytes vary in at most 4 of 8 positions to a multi-pass float radix, and everything else to the AVX2 quicksort; the hash
  map beats the first (0.42-0.54x) and loses to the second (1.06-1.60x). Two competing predictions were written before
  the run; this one was right on all three rows where they disagreed (integers below 2^30 lost like their real twins,
  the same exponential data shifted by 0.5 won like the integer version), integrality was wrong on all three.
- **Shipped.** Hash map only when sample Chao1 <= 3300 at 8+ threads; below 8 threads when Chao1 <= 128, or when the leaf's
  own byte probe says the fallback would be the float radix (Chao1 <= 3300, n in the radix window). At 8+ threads no
  lattice table above 65536 slots. `LOGOS_MCGATE_OLD=1` restores the old gate, `LOGOS_MCGATE_V1=1` the version without
  the fallback test. Thread counts 2-7 were not measured and take the 1-thread rule.

| shape | old gate / new gate, 14 threads | 1 thread |
|---|---|---|
| hash_60k (60k equal-odds doubles) | **4.46** | 0.95 (gated out before too) |
| hash_30k | **2.45** | 1.02 |
| prices_cents (lognormal cents) | **2.70** | 1.20 |
| user_ids_zipf (200k sparse ids, Zipf) | **1.88** | 1.07 |
| mantissa16 (65536-value lattice) | **1.40** | 0.99 |
| hash_5k | 1.23 | 1.19 |
| yyyymmdd | 1.10 | 1.02 |
| zipf_pareto (where the route wins) | 1.01 | 1.07 |
| guards: uniform, few_5 | 1.01-1.02 | 1.00-1.01 |

- **Held-out test of version 1**, ten shapes written before the rule existed: 4 cells gained 1.2x or more (latency 1.39x,
  a 100k vocabulary 2.10x at 14 threads, 2k equal-odds 1.59x, half-lattice data 1.66x) and what the gate still let in
  cost at most 10% in 18 of 20 cells, both as predicted. The no-regression prediction FAILED on one cell: exponential
  whole seconds at one thread, 0.505x, which is the miss that led to the fallback finding.
- **Version 2 on seven fresh shapes, one thread:**

| fresh shape | sample Chao1 | v1 / v2 | v2 / route off |
|---|---|---|---|
| Zipf over 2000 integers below 2^16 | 567 | **1.76** | 0.57 |
| Zipf over 20000 integers below 2^20 | 857 | **1.84** | 0.60 |
| Zipf over 3000 halves below 2^14 | 499 | **2.02** | 0.52 |
| whole numbers, exponential, mean 20 | 103 | 1.02 | 1.00 |
| equal odds over 8000 integers (Chao1 above 3300) | 5343 | 0.99 | 0.97 |
| prices to the cent (AVX2 fallback) | 4857 | 0.98 | 1.01 |
| ratings 1.0-5.0 by halves (low-cardinality route) | 9 | 0.99 | 0.98 |

  All seven inside their written predictions (30 pairs, 8M floor 2.7-3.3%): version 2 wins 1.76-2.02x where the fallback would be the float radix, and every shape it declines reads 0.97-1.01 against the route off.

### Signed zeros: the sort was not a permutation below 65536 elements
- A byte compare against qsort flagged two held-out shapes. Every variant was ascending and bit-for-bit the same multiset,
  every difference a -0.0 / +0.0 swap, so that flag was an ORACLE artifact (qsort calls the two equal). But a harness
  check written to test signed zeros properly then found a REAL defect (`make zerodiag`):

| shape, 1 thread | n | -0.0 in | -0.0 out | +0.0 in | +0.0 out |
|---|---|---|---|---|---|
| integers near zero | 65535 | 763 | **0** | 742 | 1505 |
| few distinct {-0, 0, 1, 2} | 4095 | 992 | **0** | 1001 | 1993 |
| integers holding only -0.0 | 32768 | 744 | **0** | 0 | 744 |
| random doubles + 1% zeros | 2000 | 15 | **23** | 16 | **8** |
| tenths near zero | 1000 | 5 | 4 | 4 | 5 |
| any shape | 65536 and 300001 | | unchanged | | unchanged |

- **Four sites.** `sort_fn`'s counting sort and int32 radix and the dense-integer route rebuild values from integers, and
  -0.0 passes `v == floor(v)`, so it came back as +0.0. The AVX2 leaf's networks are `_mm256_min_pd` / `_mm256_max_pd`,
  which return the same operand when -0.0 meets +0.0, so one sign was written twice and the other lost. The optional
  x86-simd-sort leaf has the same hazard. None was new this session; the harness had never held a -0.0.
- **Fixed.** A -0.0 is not an integer for the rebuilding paths. The vectorized leaves make every zero +0.0, sort, and write
  the -0.0 back at the start of the zero run, where total order puts them. The first version of the AVX2 check cost
  **7% of a 1-thread sort** on uniform and heavy_tail (layout cancelled); the second flags NaN and zeros with the one
  compare the NaN scan already made: uniform 1.001 and heavy_tail 0.991 at one thread, every 14-thread row within 4% (24 pairs, layout cancelled). Predicted 0.97-1.03: right. One row, clustered at one thread, read 1.080 in the check's favour, which a check cannot earn; one of its two orientations read 1.13, so it is noise.
- Harness: 36 signed-zero checks (tenths, integers, few distinct, random with zeros, a dense permutation whose zero is
  -0.0, wide integers through the parallel partition's leaves). 1536 checks.

### Every two-copy bench failed to link
- The shared `RENAME` macro lacked the four sorted-view symbols, so `ledgerbench` (and every other rule that links two
  renamed engine copies) failed with multiple definitions; only the `cp.o` rule had them. They are in `RENAME` now.

## (unreleased) — the next step on both routes: a parallel tail merge, and a table for the stream lookup
Both measured with the paired harness at 8.4M, one binary switched at runtime, every output verified against qsort.
Noise floor on the final run: 3.2% at 1M, 4.7% at 8M. `make test`: 1500 checks.

### Sorted prefix plus unsorted tail, parallel form: landed, parallel sorts take the route again
- Every thread scans its chunk for NaN and copies it to scratch, so the array is untouched until the route commits;
  the tail is sorted inside scratch; the prefix is cut at P-1 evenly spaced positions and each cut's value is
  binary-searched in the sorted tail; every thread merges its piece of prefix with its piece of tail straight into
  the array. The pieces tile the output and read only scratch, so nothing is copied back. `LOGOS_TAIL_SEQ=1`
  restores sequential-only.

| shape | 14 threads, sequential form (first cut) | parallel form, run 1 | parallel form, run 2 |
|---|---|---|---|
| 99% sorted prefix | 0.866 | 1.014 | 1.006 |
| 95% prefix | 0.811 | 1.166 | **1.172** |
| 90% prefix | not run | 1.227 | **1.215** |
| 80% prefix | 0.578 | 1.157 | **1.170** |
| 50% prefix | 0.336 | 1.034 | 1.047 |
| guards: 49% prefix, uniform, near-sorted, sorted, reverse | 0.96-1.01 | 0.99-1.03 | 0.98-1.02 |

  Run 1 had an n/8 tail gate lifted by `LOGOS_TAIL_PAR_ANY=1`; run 2 is the shipped build with no gate. The
  1-thread rows are unchanged (4.2-4.4x at 99%, 3.4-3.6x at 95%).
- **Prediction and grade.** Predicted at 14 threads: 99% 1.3-1.6, 95% 1.2-1.5, 90% 1.05-1.3, and with the gate
  lifted 80% 0.9-1.1 and 50% 0.5-0.7. **Wrong at 99%** (1.01): the near-sorted route it displaces is already as
  fast when the strays are 1%. Just under at 95%, inside at 90%. **Wrong, in the good direction, on large tails:**
  80% gained 1.16x and 50% did not lose. The n/8 gate existed only because of that prediction, so it was removed.
- Harness: 116 tail checks (was 57): a 1M size, each shape also run with `LOGOS_TAIL_SEQ=1`, and a NaN inside the
  PREFIX, which the prescan walks straight past, for the parallel form.

### Stream route lookup table: landed from 12 streams up
- A 4096-cell byte table over the sample's range gives each value's stream with one load, where the compares cost
  one per boundary. It is exact, not approximate: the cell index is a monotone function of the value, so a cell
  that holds no boundary has every lower boundary below all its values and every higher boundary above them, and
  its precomputed count IS the compare-sum's answer. Cells holding a boundary fall back to the compares. A NaN
  boundary or a non-finite range skips the table.
- Table against compares, route on both sides, 1 thread:

| streams | run 1 | run 2 | run 3 (table forced) |
|---|---|---|---|
| 4 equal | 0.810 | 0.802 | 0.794 |
| 8, 1% perturbed | 0.678 | 0.663 | 0.705 |
| 8 shards at rates 1/(s+1) | 1.096 | 1.042 | 1.112 |
| 12 equal | | | 1.013 |
| 16 equal | 1.208 | 1.234 | 1.197 |
| 60 equal | **2.744** | **2.715** | **2.788** |

- **Prediction and grade.** Predicted 4 streams 1.00-1.10, 8 shards and perturbed 1.05-1.20, 16 streams
  1.15-1.35, 60 streams 1.5-2.2. Right at 16, beaten at 60, right on shards, **wrong at 4 streams and on the
  perturbed shape, both losses.** So the table runs only from `STREAMS_TABLE_MIN_MODES` (12) streams, where it
  measured neutral; `LOGOS_STREAMS_TABLE=1` forces it, `LOGOS_STREAMS_CMP=1` forbids it.
- The route off against on, as shipped:

| shape, lookup used | 20 pairs | 30 pairs | before the table (earlier run) |
|---|---|---|---|
| 4 equal streams, compares | 2.686 | **3.209** | 3.033 |
| 8 shards at rates 1/(s+1), compares | 1.889 (p10-p90 1.29-2.86) | **2.709** | 2.634 |
| 8 streams, 1% perturbed, compares | 2.351 | **2.357** | 2.353 |
| 12 equal streams, table | 1.221 (p90 2.69) | **2.636** | not run |
| 16 equal streams, table | 2.374 | **2.646** | 2.155 |
| 60 equal streams, table | **3.152** | 2.866 | 1.197 |
| every shape at 14 threads | 0.94-1.04 | 0.95-1.03 | 0.97-1.03 |
| guards at 1 thread | 0.98-1.02 | 0.98-1.02 | 0.97-1.02 |

  Predicted: 4 streams 2.8-3.1, shard IDs 2.5-2.9, perturbed 2.1-2.4, 16 streams 2.5-2.9, 60 streams 3.0-3.4. The perturbed shape and 60 streams landed inside; 4 streams, shard IDs and 16 streams landed inside on the 30-pair run and below on the 20-pair run, whose shard-ID and 12-stream rows spread from 1.3 to 2.9 inside a single row, so those low readings are noise, not an effect. The clean result is the 60-stream row: 1.20x before the table, 2.9-3.2x after. The "before" column comes from a different run and is a within-row ratio, so read it as direction, not as a paired comparison.
- **A trace build inverted the answer.** Looking for the loss, a `-DLOGOS_TRACE` build of a probe (`make
  streamsprobe`) put phase stamps inside the route and showed the table FASTER on the perturbed shape: lookup
  pass 14 against 22 ms, whole sort 129 against 170. The same probe against the normal build showed it slower,
  109 against 101. The trace build's `fprintf` calls change the function's code generation enough to flip a
  0.7x into a 1.3x. **Never time a code-generation-sensitive kernel in a trace build**; use a trace build to see
  WHETHER a path runs, and the normal build to see how fast.
- Harness: 63 stream checks (was 51): two boundaries sharing the table's first cell, and a stream ending in +inf.

## (unreleased) — the two routes the quadtree probe pointed at, built and measured (`make prefixbench`, `make streamsbench`)
Both are one binary switched at runtime, measured with the paired harness at 8.4M and verified against qsort.
Two runs of the tail route bracket its ranges, on days whose noise floors measured 2.6% and 10.2%.

### Sorted prefix plus an unsorted tail: landed, sequential sorts only
- Append-mostly data. The prescan already walks an ascending prefix to its first descent, so the prefix
  length is free: if it is at least half the array, sort only the tail and merge the two pieces from the
  back, with scratch the size of the tail and one pass. NaN is refused, because a NaN can hide inside a
  prefix that only LOOKS ascending. The probe pointed at this structure, but the prescan's own stopping
  point turned out to be a cheaper and exact gate, so the route does not consult the probe at all.

| shape | 1 thread | 14 threads, first cut | 14 threads, shipped |
|---|---|---|---|
| 99% sorted prefix | **4.29-4.55x** | 0.866 | 1.000 |
| 95% prefix, the zoo's shape | **3.57-3.95x** | 0.811 | 0.994 |
| 80% prefix | 2.09-2.20x | 0.578 | 1.019 |
| 50% prefix, the gate's edge | 1.12-1.21x | 0.336 | 1.020 |
| guards: 49% prefix, uniform, near-sorted, sorted, reverse | 0.97-1.01 | 0.96-1.01 | 1.00-1.01 |

- **The first cut regressed at 14 threads**, 0.34 to 0.87. Its NaN scan and backward merge are
  single-threaded passes, and the parallel near-sorted route it displaced was already fast. This is the
  second time for that lesson, after the dense-integer bitmap: **a gate that passes is not a reason to run
  a sequential algorithm inside a parallel sort.** It is now offered only to sequential sorts and every
  14-thread row sits at 1.00. A parallel merge, splitting both runs at P-1 value splitters, is how to win
  at 14 threads as well.
- **Prediction and grade.** Predicted 6-9x at one thread and 1.3-1.8x at fourteen on the 95% prefix.
  Measured 3.6-4.0x and a regression. Right in direction at one thread and over-predicted by about half;
  simply wrong at fourteen, because the zoo's 1.79x gap against its hand-routed ceiling did not survive a
  paired A/B against the engine's own near-sorted route.
- Harness: 57 checks, including the gate's edge, a one-element tail, tails wholly above or below the
  prefix, a descending tail, equal keys across the merge, and a NaN case that must leave the output
  bit-identical to the route switched off.

### Interleaved ascending streams: landed, sequential sorts only
- Sharded ID allocators, tenant-prefixed sequences, per-sensor keys: several streams, each ascending in
  arrival order, in value ranges separated by gulfs, interleaved element by element. **A stable scatter
  preserves arrival order inside each bucket, so if the buckets are the streams, every bucket is already
  sorted** and no comparison sort runs.
- The streams are the gulfs in the dispatcher's own sorted sample, gaps wider than ten times the median,
  cut at each gulf's midpoint. Ascent is checked on a jittered 1024-point sample taken in position order,
  which the dispatcher's sample cannot answer because sorting it discarded the positions. After the
  scatter every bucket is checked and any that is not in order goes to the ordinary leaf, so correctness
  never rests on the gate.

| shape | 1 thread | 14 threads |
|---|---|---|
| 4 equal streams | **3.03x** | 1.01 |
| shard IDs, 8 shards at rates 1/(s+1), the zoo's shape | **2.63x** | 1.00 |
| 8 streams with 1% of values perturbed | 2.35x | 1.00 |
| 16 equal streams | 2.16x | 1.00 |
| 60 streams, near the 64-mode cap | 1.20x | 1.03 |
| guards: uniform, bimodal, clustered, heavy_tail, few_distinct, sorted_runs | 0.97-1.02 | 0.97-1.03 |

- **Prediction and grade.** Predicted 4-6x on shard IDs, measured 2.63x. The ORDERING was predicted
  correctly, fewer streams winning more, sixty least, perturbed streams a little below clean ones, and the
  magnitude was over-predicted. The ordering is also the explanation: the speedup falls exactly as the
  stream count rises, which points at the per-element mode lookup, a compare against every boundary. The
  zoo's hand-routed ceiling bucketed with one multiply and reached 5.9x. A direct-index table over value
  space would make the lookup constant in the number of streams, and is the obvious next step.
- A second, smaller run (1M elements, 5 pairs, no floor taken) kept the one-thread ordering: 4 streams 2.75x,
  perturbed 2.31x, shard IDs 2.24x, 16 streams 1.94x, 60 streams 1.03x. Its 14-thread rows scatter from 0.89 to
  1.14 with p10 as low as 0.52, which is what 5 pairs resolves, not an effect.
- The guards are clean, including bimodal and clustered random data, which DO have gulf-separated modes
  and are refused by the ascent check, as designed.
- Harness: 51 checks, covering 2, 8 and 60 streams, 70 streams past the cap, a stream only mostly
  ascending, ties, negative ranges, bimodal random data the gate must refuse, and a NaN case.

## (unreleased) — the quadtree probe's tail rule, tightened and validated on strangers (`make ztail`)
- zroute's held-out test found the tail rule too loose: "low early, peaks late" also fired on ID blocks and
  on Gray-code order. The fix was designed FROM those failures, so it could not honestly be validated on
  that zoo. It was validated instead on fifteen fresh shapes: new generators, n = 5,000,000 (not a power of
  two), new seeds, none of them in the rule's training set, the zoo, or the first probe.
- **The new rule.** A real tail puts random values across the WHOLE value range at the end of the array, so
  even the coarsest grid is already off the diagonal, o[1] >= 1.5 and o[2] > 1.5, and the late bulge is
  large, a peak of at least 3. The zoo's two false tails both sat on the diagonal at the coarsest levels or
  never climbed past about 2.1.

| on 15 fresh shapes | old rule | new rule | predicted for the new rule |
|---|---|---|---|
| true tails caught | 4 of 5 | 4 of 5 | 3-4 of 5 |
| lookalikes falsely called tail | 5 of 7 | **1 of 7** | 1-2 of 7 |
| controls classified as expected | 3 of 3 | 3 of 3 | 3 of 3 |

- **False tails fell five-fold with recall unchanged.** Removed: Gray code and growing shuffled blocks, whose
  peak stays under 3 through level 6, and late swaps and ID blocks, which read exactly 1.00 at the coarsest
  level.
- **The survivor is instructive.** A Brownian random walk climbs steadily, 2.00 through 7.73 by level 6,
  exactly as a tail does. From occupancy alone a random walk and a sorted prefix with a random tail are
  hard to separate: both are ordered at coarse scale and disordered at fine scale.
- **The miss under both rules:** a sorted prefix followed by a sorted, overlapping tail. That is two runs,
  and its profile plateaus near 2.9, just under the k-overlap threshold of 3, so no label fires.
- Prediction and grade: the new rule's numbers were right on both counts. The old rule did slightly worse
  than predicted, 4 true tails rather than 5 and 5 false ones rather than 4. The predicted SURVIVORS were
  wrong: growing noise read random under both rules and was never a false tail, and the one false tail
  that did survive, Brownian motion, was not predicted.
- zroute.cpp keeps its original rule, because it is the record of a held-out test and rewriting it would
  falsify that record. The tightened rule lives in ztail.cpp.

## (unreleased) — a quadtree over (index, value) as a routing probe: a held-out test (`make zroute`)
- **The idea.** Treat every element as the point (position, value). Take a sample, convert both axes
  to ranks, and count how many cells of a 2^l x 2^l grid it occupies at each level; o[l] = occupied /
  2^l. A sorted array sits on the diagonal and reads 1.00 at every scale, and the SHAPE of the profile
  over l pictures the arrangement: overlapping sorted runs plateau at their count, disjoint blocks settle
  near 2, near-sorted data peaks then decays, a sorted prefix with a random tail stays low and bulges
  late. One profile standing in for a chain of separately tuned gates, which is the "order budget" idea
  from OUTSIDE_THE_BOX.md made concrete. **No engine code changed**: this measures whether routing on
  the profile would pick right where today's gates do not.
- **The discipline, because a classifier tested on the shapes it was built from proves nothing.** The
  rule was fixed in advance from the six training shapes of an earlier eight-shape probe. `make ztrain`
  confirms the rule AS CODED reproduces all six (0 of 6 misclassified), so the code matches its fitting.
  It was then applied unchanged to the chaos explorer's twenty-shape zoo, which it had never seen, and
  every prediction was written into zroute.cpp before it ran.

| | predicted in advance | measured |
|---|---|---|
| the probe's own output matched the prediction | | 17 of 20 shapes |
| arrangement shapes classified correctly | about 4 of 8 | **4 of 8** |
| value-set shapes where it rightly abstained | 10 of 10 | 9 of 10 |

- **The number that matters: blind 0, at both thread counts.** On every arrangement shape where the
  engine leaves 1.2x or more on the table against a sort told the structure, the profile classified the
  structure correctly. All four of its misses fall on shapes where the engine already BEATS that
  ceiling. The failures land exactly where they cannot cost anything.

| arrangement shape | gap at 1 thread | gap at 14 threads | profile read | correct |
|---|---|---|---|---|
| sorted_plus_tail | **9.36x OPPORTUNITY** | **1.79x OPPORTUNITY** | tail | yes |
| shard_ids_disjoint | **5.91x OPPORTUNITY** | 0.76x redundant | k-overlap, k=9 (8 streams) | yes |
| sorted_runs_16 | 0.65x redundant | 0.55x redundant | k-overlap, k=16.5 | yes |
| reverse_near_sort | 0.99x redundant | 0.65x redundant | bounded, descending | yes |
| event_ts_latency | 0.28x moot | 0.20x moot | unknown | no: disorder of 4096 is two sample spacings |
| window_shuffle | 0.33x moot | 0.18x moot | sorted | no: disorder of 2048 is one sample spacing |
| sorted_runs_256 | 0.40x moot | 0.30x moot | random | no: 256 pieces saturate the sample ceiling |
| shard_ids_blocks | 0.86x moot | 1.19x moot | tail | no: a false tail |

- **Four limits, stated because each one bounds the claim.**
  1. **Thin evidence.** Eight scored shapes, two opportunities at one thread and one at fourteen. Blind
     on 0 of 3 is encouraging, not conclusive.
  2. **An opportunity is a classification, not a speedup.** No route exists yet to take either one. The
     tail needs the sorted-prefix-plus-tail route (the explorer's P2); disjoint streams at one thread
     need a stable scatter the sequential path lacks. The profile names the prize; a route claims it.
  3. **The tail rule is too loose.** "Low early, peaks late" also matched shard_ids_blocks and
     gray_code, the second a value-set shape where the probe should have abstained, which is the one
     abstention it missed. On gray_code the profile is not wrong that arrangement structure exists:
     consecutive Gray codes differ by one bit, so neighbours stay close in value. The LABEL is wrong,
     and the prediction ignored that structure. **Not retuned on this zoo**: that would turn a held-out
     test into a fitted one, so any fix has to be validated on fresh shapes.
  4. **The blind spot is structural and confirmed.** The window and latency shapes read sorted or
     unknown because their disorder spans at most two sample spacings (n/m = 2048 here). Order finer
     than a sample's own spacing needs a linear scan. The engine already handles both at 0.18-0.33x of
     the ceiling, so nothing is lost.
- **Value-set gaps the probe rightly ignores**, recorded because they are real and belong to the counting
  routes: mantissa16 at 3.39x at ONE thread through the quantized radix (the lattice budget fixed only
  14 threads, where it reads 1.57x), prices_cents at 1.86x at 14 threads through modcard, and yyyymmdd
  at 1.62-1.66x through lattice counting.
- New: `make chaoshunt` (the explorer's zoo, landed from its patch), `make zroute` (this test),
  `make ztrain` (the integrity check), and a `cp.o` rule for the profiled, renamed engine copy that also
  renames the four partition and bucket symbols RENAME misses, without which the zoo benches fail to
  link. Raw output: `experiments/zroute-results.txt`.

## (unreleased) — dense integers sort by presence, not by comparing: 8.8-11.4x at one thread (`make mcbench`)
- The chaos map's other headline was that the SEQUENTIAL dispatcher leaves 4-15x on the table, with
  dense permutations of consecutive integers among the worst: a Gray-code order and a hash-iteration
  order reach the quantized radix and pay four passes over 64 MB.
- If the values are integers with no repeats spanning not much more than n, the sorted output carries
  no information beyond WHICH integers are present. One bit per possible value, one pass to set the
  bits, one pass to walk them in order. At 8M the map is 1 MB, which is L2-resident, so the middle
  pass is a cached bit-set per element.
- Measured, paired, one binary switched by `LOGOS_NO_DENSE`, 8.4M, 20 pairs:

| shape | 1 thread | 14 threads |
|---|---|---|
| gray_code (permutation of 0..n-1) | **11.352** [p10 10.214] | 1.019 |
| shard_ids (1024-id blocks, 8 consumers) | **10.034** [p10 9.304] | 1.016 |
| hash_order (permutation of 0..n-1) | **8.777** [p10 8.149] | 1.017 |
| span_3n (guard: dense but spread) | 0.998 | 0.999 |
| uniform (guard: not integral, never gated in) | 0.984 | 0.990 |
| one_repeat (guard: must decline) | 0.796 | 1.004 |

- Prediction and grade: I predicted 6-12x at one thread and neutral at fourteen. Right on both, at the
  top of the range.
- **Two mistakes on the way, both worth keeping.** First, the sample gate bounded the span by `m*m`,
  which quietly turned the route OFF at one thread, where the sample is small and the route is the
  whole point; the bound is against n now. Second, the route was offered to parallel sorts too, where
  three single-threaded passes took 71 ms against the parallel partition's 27, and a DECLINE there
  still cost two wasted passes (0.32 on a shape with one repeat). **A sequential algorithm must not be
  offered to a parallel sort just because its gate happens to pass**; it is now attempted only when
  the sort is going to run sequentially, which is why every 14-thread row above sits at 1.00.
- Then the extremes pass was removed, which is where 7.3x became 11.4x. The window now comes from the
  sample with a margin of n on each side and the single pass aborts on anything outside it, the same
  bargain the lattice route makes. A decline costs one PARTIAL pass instead of two full ones: the
  one-repeat guard went from 0.603 to 0.796.
- **The residual cost, stated plainly**: dense integer data that contains a few repeats, too few for
  the sample to see, pays about 1.26x. Data with enough repeats to show up in the sample is rejected
  at the gate for free, and data with many repeats belongs to the counting routes anyway. That band is
  narrow but it is real.
- Harness: 72 new checks covering the permutation, descending, span exactly 4n, span 8n (must
  decline), a negative base, one repeat, one far outlier, one non-integer, and many repeats, at 1 and
  4 threads, either side of the route's minimum n. Total 1321.

## (unreleased) — the counting routes: a slot budget worth 3.6x, and two measured negatives behind it (`make mcbench`)
Chasing the overnight chaos map's clearest finding: the moderate-cardinality route sitting 3-7x off a
hand-routed ceiling with its workers idle for most of the wall. The profile said something different
from what everyone assumed, including me.

### The bug was upstream of the slow route: a perfect lattice could not get in
- `lattice_try_f64` had a flat 65536-slot ceiling, and a table that did not fit was **declined
  outright**. A lattice of 60000 evenly spaced values therefore fell all the way to the hash route,
  because its eighth-of-span margin pushed 60000 slots to 75128. The margin exists to catch values
  the sample missed; spending it to reject the whole route is backwards, since a too-small margin
  costs at most an abort and an abort is recoverable.
- Both halves are fixed, and the shape of the fix is the one `count_try_i32` already used when the
  int32 route's fixed cap was replaced two entries ago: **the real constraint is memory, and memory
  scales with the thread count**. The ceiling is now `LATTICE_MAX_BYTES` (64 MB) divided among the
  threads, capped at `LATTICE_MAX_SLOTS` (262144) so the serial slot merge, which walks slots x P,
  cannot grow without limit. An oversized table now shrinks its margin before giving up.
- Measured, paired, ONE binary switched by `LOGOS_LATTICE_OLD` so there is no code-layout bias,
  8.4M elements, 24 pairs:

| shape | 1 thread | 14 threads |
|---|---|---|
| lattice_60k (60000 slots, margin pushed it to 75128) | 1.003 | **3.575** [p10 2.900, p90 4.167] |
| mantissa_64k (65536 slots, over by the margin alone) | 0.999 | **3.603** [p10 2.308, p90 4.928] |
| lattice_30k (fit the old ceiling too) | 1.017 | 1.033 |
| lattice_120k | 1.013 | 1.047 |
| guards: zipf, few_5, quantized, uniform, heavy_tail | 0.987-1.075 | 0.834-1.148 (wide, untouched code) |

- `mantissa_64k` is the row the chaos map measured at 6.9x off its ceiling. `lattice_120k` does NOT
  move, and the reason is worth recording: at 120000 distinct the sample is all-unique, so the
  modcard GATE rejects it before the slot cap is ever consulted. The cap was never its problem.
- The 1-thread rows are all flat. The route fires there too, but the sequential alternative is close
  enough that the change does not show; the win is the parallel table.

### NOT LANDED: replacing the serial merge with concatenate/sort/fold
- The hash route merges its per-thread tables by re-inserting every entry into one global hash,
  single-threaded, rehashing as it grows. The obvious replacement is what the low-cardinality route
  already does: concatenate the tables, sort by key, fold equal keys in a linear pass. No hash at all.
- **It is slower above one thread and the arithmetic says why.** The concatenation is `P*d` pairs
  where the union is only `d`, so at 14 threads it sorts fourteen times the data the hash merge
  touches. Paired, one binary, non-lattice shapes chosen so both halves reach the hash route:

| shape | 1 thread | 14 threads |
|---|---|---|
| hash_5k | 0.984 | 0.900 |
| hash_30k | 1.049 | **0.724** |
| hash_60k | 1.140 | **0.582** |
| zipf | 0.985 | 0.804 |

- At one thread, where `P*d == d` and there is no redundancy, it is neutral-to-better, which is
  exactly the prediction the arithmetic makes. Kept opt-in as `LOGOS_MC_CONCAT=1` rather than deleted,
  because it is the right SHAPE for the fix the serial merge still needs: partition the key space P
  ways first and it becomes P independent sorts of `d/P` pairs, with no redundancy and no hash.

### Kept but neutral: a byte-skip radix instead of libc qsort for the merge's pair sort
- The merge sorted its (key, value, count) triples with `qsort`, which this project bans on sampled
  paths, and a phase trace made it look urgent: 147 ms of a 2268 ms sort on a hot box. Replaced with
  a byte-skip LSD radix on the key (constant key bytes cost one OR/AND pass to detect and then no
  pass at all).
- **Measured against what it replaced: 0.975-1.149, inside the floor.** It is kept because the rule
  says so, not because it won, and the 147 ms reading is a caution about trusting a phase trace taken
  on a hot box: the same work is ~10% of the sort when the machine is cool. `LOGOS_MC_QSORT=1`
  restores qsort for anyone who wants to re-measure.

## (unreleased) — overnight team round: two wins landed, three measured negatives, one map
Seven agents worked in isolated copies with benchmarks serialised through a machine-wide lock; two
claimed wins were re-measured by independent skeptics told to refute them. Findings and the full
patches, including the code for everything that did NOT land, are under `experiments/team/`.

### LANDED: the int32 sentinel window and the span band (`make intbench`, `make i32abbench`)
- **A sample bounds the bulk, never the tail**, now applied to the SEQUENTIAL int32 leaf as well.
  `leaf_i32` gated its counting sort on raw min/max, so one INT32_MIN among a 0..99 bulk cost about
  2.5x. It now takes the table window from the sample's K smallest and largest and spills the tails,
  the same shape `count_try_i32` already used on the parallel side.
- **The fixed 262144-slot cap became a 64 MB table budget plus a persistent table pool**, which
  fixes the band where 14 threads lost to one core.
- **An inherited constant was found 2x wrong at its top edge**: the sequential `span <= n` gate is
  now `span <= n/4`, because a count array of n slots is random access into 32+ MB and loses to the
  radix well before the old gate gave up.

| measurement, 8M unless noted | builder | skeptic, independently |
|---|---|---|
| one INT32_MIN sentinel, 1 thread | 2.4-3.2x | 2.73x at 8M, 3.33x at 1M |
| sentinel row over clean row | not measured | **1.09-1.16 on, 2.4-5.6 off** |
| span 1e6, 14 threads | P/1 0.60 -> 2.7, paired 3.2x | paired 2.66x |

- Harness int32 coverage 180 -> 306 checks.

### LANDED: batch queries on the sorted view (`logos_view_quantiles`, `logos_view_ranks`, `make viewbench`)
- m ranks or quantiles in one pass: the cuts are sorted, the buckets they land in are walked once,
  and the touched set is sorted side by side instead of one after another. The int32 twin of the
  whole view API came with it (`logos_view_*_i32`).
- Measured, 8M, 14 threads, paired, both routes in one binary: **1.22-1.63x over sort-then-index**
  and **1.19-1.56x over m single lookups** (skeptic's own numbers; the builder got 1.29-1.67 and
  1.17-1.40). The engine-core edits are neutral pristine-versus-patched with a copy swap, 0.97-1.06
  across nine rows.
- **The int32 view is deliberately near-neutral (1.03-1.15x) and the reason is worth keeping**: the
  int32 leaf is only 7-15% of an int32 sort, so there is almost nothing to defer. The lever on that
  side is its pre-H0b histogram partition, not laziness.
- Harness gains 288 view checks; total 1249.

### LANDED: the sequential-leaf policy for batch queries
- The skeptic caught the batch route silently trading away the AVX2 sequential leaf:
  `logos_sort_buckets_f64` derived `seq` from the thread count, so a ten-bucket batch on ten threads
  took the scalar parallel leaf. `seq` is now set when each thread will sort AT MOST ONE bucket,
  which is exactly the batch regime and exactly when a bucket is a lone sequential sort. Full
  materialisation is unchanged by construction (168 buckets over 14 threads never satisfies it), so
  the ledger's measured "AVX2 in the parallel leaf loses 5-28%" still governs there.
- Measured, one binary, `LOGOS_VIEW_SEQ0=1` restores the old policy, 40 pairs, floor 5.7% that day:

| sawtooth, 8M, 14T | old | new |
|---|---|---|
| sort+index / batch | 2.576x | **3.079x** |
| 10x view_at / batch | 1.199x | **1.374x** |
| few_distinct, 10x at / batch | 0.877x | **1.033x** |
| uniform, 10x at / batch | 1.391x | 1.335x (inside the floor) |

- **Honest caveat: the skeptic's specific regression did not reproduce here.** They measured the
  batch at 0.674 on sawtooth, i.e. 1.4-1.6x slower than the loop; on this tree with this generator
  the old policy already read 1.199. Their diagnosis was still right and the fix still helps on
  every shape it touches, but the size of what it repairs is generator-dependent and unresolved.

### NOT LANDED, measured neutral: curvature-placed CDF knots
- A Douglas-Peucker fit on the sample's rank-versus-key curve, knots dense where the CDF bends.
  **Neutral, about 3-4% on heavy_tail at 14 threads, below the floor.** Ran twice; the first pass
  claimed 1.13x and the second, more careful one retracted it.
- The mechanism is the useful part and it closes a family. The map's lookup IS cheaper (0.218 vs
  0.655 ms per 49k elements on the trial), but it is paid for by a WORSE partition: true leaf max
  1.68x against the key table's 1.45x. No tolerance setting fixes that, because the exact splitters
  are built from the same sample and already have zero in-sample error, so an approximation of them
  can only add error. **Three attempts at a fancier bucket lookup have now failed for this reason;
  the lookup is not where the partition's time is.** Code and numbers in `experiments/team/`.

### NOT LANDED, measured neutral: an atomic chunk queue for the f64 counting routes
- Dynamic chunks plus a single spawn plus parallel merges in the lattice, lowcard and modcard routes.
  **Inside the 7.7% floor at 14 threads on both target shapes (1.03, 0.97).**
- It wins about 1.2x in exactly three places, and they share a cause: `lattice_30k` at 14 threads
  (whose serial slot merge walks 65k slots times 14 tables) and both shapes at 7 threads. **So the
  static per-thread ranges were not the cost; the SERIAL phase is.** That redirects the target.

### NOT LANDED, measured negative: relocation by page table
- The census settled it for the cost of a printf, as predicted. On the bench's block-permuted shapes
  **not one byte can move by page table**: the block size in bytes is not a page multiple, so only
  the permutation's fixed points align, 0-1.6%, and those need no move at all.
- The prototype built for the case where aligned runs do exist is correct and wins 1.3-2.0x, but only
  when the caller hands over a 2 MB-aligned array whose blocks are 2 MB multiples; on ordinary
  malloc'd arrays it loses (0.63-0.77 at 14 threads), one `mremap` costs 10-30 us regardless of size
  and 2.5-8x more with 13 threads live in the address space, and the moves fragment it badly enough
  that a second sort of the same buffer faults.

### THE MAP: where order is actually left on the table (`experiments/team/chaos-explorer.md`)
- Forty shapes drawn from real systems, each raced against a hand-routed ceiling, all bit-exact.
- **The gap is overwhelmingly a SEQUENTIAL dispatcher problem**: at 1 thread, shard IDs 13-15x,
  Gray code 12-13x, hash order 8.6-9.4x, sorted-prefix-plus-tail 6.7-9.3x off the informed route.
- **At 14 threads the partition is already within 1.0-1.4x of the ceiling on everything except the
  modcard route**, which is 3-7x off on 50-65k-distinct lattices with workers busy only 14-38 ms of
  an 86-105 ms wall. On nine shapes the engine already beats the informed route.
- Zipf-distributed IDs spend 30.5 of 68.9 ms at 14 threads inside a modcard attempt that then ABORTS.

### Not finished
- The strided-order probe (k interleaved sorted runs) has code and a written prediction in
  `experiments/team/strided-probe.md` and its patch, and **zero measurements**: the agent died
  before benchmarking. The prediction stands on record, untested.

## (unreleased) — int32: the sequential leaf windows its table too, the parallel count route fits threads to a table budget, and the counting gate drops to n/4 (`make i32abbench`, experiments/team/i32-sentinel-seq.md)
- Three int32 changes, each a RUNTIME toggle so one binary measures both sides with no
  code-layout bias (methodology rule 3), each paired-adjacent at 8M on a loaded box under the
  team bench lock, every output verified against qsort first. Ratios are old/new medians.
- **(a) `leaf_i32_windowed`**: the sequential leaf gated its counting sort on raw min/max, so
  one INT32_MIN among a 0..99 bulk sent 8M elements to the radix (2.5x, intbench). It now does
  what `count_try_i32` did the entry before: a 2048-point sample gives a full and a K=5 trimmed
  window; the window is counted, the tail spills into the scratch buffer (low from the front,
  high from the back), is sorted by a recursive call and placed before / after the bulk. 1T:
  one INT32_MIN **3.19x**, both extremes **4.91x**, a 0.1% far tail 1e6 wide **4.86x**; the
  guards (clean bulk, span 101, 5% sentinel, span 1e8, full range) 0.985-1.024. Measured
  against the clean row adjacently the sentinel row is now within noise of it (section G).
  `-DLOGOS_NO_I32_LEAF_WINDOW` compiles it out, `LOGOS_I32_NO_LEAF_WINDOW=1` turns it off.
- **(b) table budget**: the parallel count route had a fixed 262144-slot cap and lost to one
  core for spans between that and ~n/8 (intbench 1e6 row, P/1 0.60-0.87). The cap is now a
  MEMORY budget (`COUNT_I32_TABLE_BUDGET_MB`, 64): Pc = min(count_threads, budget/span),
  declining below 3 threads or above span n. 14T old/new: span 5e5 **4.10x**, 1e6 **3.20x**
  (P/1 0.60 -> 2.68), 2e6 **2.34x** (8 threads), 4e6 **1.52x** (4 threads); 8e6 and full
  range unchanged. `LOGOS_I32_FIXED_CAP=1` restores the old rule.
- **(b, mechanism) persistent count tables**: traced at span 1e6, the 53 MB of per-thread
  tables cost 8-28 ms just to FREE, and 8 threads beat 14 by 1.3x for footprint reasons alone.
  The tables now live in a static pool (grown, never freed, bounded by the budget), each
  thread zeroing its own. **1.14-1.38x** across the band (section F); the free step is gone
  from the trace. A 256 MB budget would buy another 1.32x at span 4e6 and 1.17x at 8e6, at
  224-256 MB persistent; 64 MB stays the default. `-DLOGOS_NO_CNTI_POOL` / `LOGOS_I32_NO_TABLE_POOL=1`.
- **(c) `LEAF_I32_COUNT_DIV` 1 -> 4**: found in the baseline: at 1T the "n = 8e6" row (span =
  n, counting) was slower than the "1e8" row (radix). Swept DIV 1/2/4/8 paired: counting still
  wins at span 2e6 (radix 0.83x), loses at 4e6 (1.27-1.36x) and at span n (**2.0x**); the
  crossover sits between n/4 and n/2 (the table stops being cache resident), so the gate is
  span <= n/4. Parallel leaves (buckets of n/168) measure neutral under it (0.99-1.03).
- Harness int32 coverage 180 -> 306 checks (954 total): span n/2 (the budget band), a rare
  far low tail needing its own recursive sort, tails on both sides with a 2^32 span, values on
  the exact window edges, a 2.5% sentinel that must decline at the sample, and n = 8192 (above
  the window's minimum, below MIN_PARALLEL).
- Predictions and grades: (a) predicted <= 1.15x the clean row and a paired 2.0-2.5x over the
  radix: measured 3.19x (the radix on this loaded box is dearer than the sample suggested).
  (b) predicted P/1 2.0-3.0 and paired 3-4x: measured 2.68 and 3.20x. (c) predicted the
  crossover at ~n/4 and ~1.6x at span n: measured between n/4 and n/2, 2.0x.

## (unreleased) — batch quantiles on the view, and the int32 view (experiments/team/view-analytics.md)
- `logos_view_quantiles(v, q, m, out)` and its rank-space primitive `logos_view_ranks(v, ks, m, out)`:
  m cuts in one pass. The cuts are sorted, the bucket walk is monotone (no search per cut), a bucket
  several cuts share is marked once, and the touched set is sorted TOGETHER through
  `logos_sort_buckets_f64` with everything else masked as done, so the buckets sort in parallel.
  `logos_view_range` / `topk` go through the same set-force, so a range spanning buckets is parallel too.
- **What a batch can and cannot buy.** `force()` was already idempotent per bucket, so a loop of m
  `logos_view_at` calls touches exactly the buckets the batch touches; the batch only sorts them side by
  side. Measured with the paired harness (both routes in one binary, 8M, 14 threads, three runs):
  ten quantiles through the batch are 1.17-1.40x faster than the same ten through the loop, and
  1.29-1.67x faster than sort-then-index (uniform 1.45/1.67/1.53, heavy_tail 1.29/1.53/1.41, bimodal
  1.63/1.63/1.56, clustered 1.34/1.37/1.32). The predicted 2.0-2.8x over sort-then-index did not
  happen because one forced 47k-element bucket costs ~1.4 ms at one thread (30 ns/element, the same as
  a plain `logos_sort_f64` on that many elements, cold or hot), so ten of them are most of a partition.
- **The int32 view** (`logos_view_create_i32` and the same API with an `_i32` suffix; the body is
  `logos_view_impl.h`, included once per type). `logos_partition_i32` is a lazy flag on
  `do_buckets_i32` (place the bucket, record min/max, skip `leaf_i32`) and `logos_sort_buckets_i32`
  is the leaf on its own; `count_try_i32` firing returns 0 buckets like the f64 route does.
  **Measured near-neutral, kept as an API, not a speedup:** paired sort-then-index / view+quantiles
  is 1.02-1.12x on span-1e8, full-range and clustered ints and 0.97-1.02x on span 2e5 (counting sorts
  it outright, nothing to defer, as predicted). The mechanism is the leaf share: the paired
  eager-sort / partition-only ratio says the leaf `leaf_i32` is 7-17% of an int32 sort against 32-44%
  (63% traced on a quiet box) for f64, because the int32 engine still runs the OLD histogram path
  whose bucket lookup is a binary search over 167 splitters on both passes. The int32 lever is that
  partition (H0b's direct-index table and the histogram-free partition, never ported to int32), not
  the view.
- **A latent race fixed.** `logos_sort_buckets_f64`'s workers each called `ctx_init`, which advances
  the pool cursor `g_ctx_next` with a plain increment, so two threads could draw the same pooled
  buffers; `hf_partition` avoids this by initialising its contexts on the calling thread. Both bucket
  sorters now do the same. One int32 verify failure was seen before the fix (viewbench_i32 clustered,
  one run) and none in three runs and 120 stress repetitions after it; the race was not reproduced
  on demand, so this is the likely cause, not a proven one.
- `make test` grows by 288 view checks (f64 and int32, 8 shapes x 6 sizes x 3 thread counts: ranks,
  a 13-cut quantile list with duplicates and out-of-range cuts, rank(x) on and between values,
  count-in-range, top-k, materialise-all byte-identical to qsort). `make viewbench_i32` is the int32
  table; `make viewbench` gained the quantile rows and three paired lines, the last of which is the
  deferred leaf share.

## (unreleased) — the view grows value-space queries and finishes with the engine's own leaf
- `logos_view_rank(x)`, `logos_view_count_in_range(lo, hi)` and `logos_view_topk(k)` join
  `logos_view_at` and `logos_view_range`. Every answer is exact and verified against a
  qsort reference in `make viewbench`.
- **How a value query finds its bucket without sorting anything.** The partition
  property says every element of bucket b is <= every element of bucket b+1. So if the
  view knows each bucket's minimum and maximum, the first bucket whose maximum reaches x
  is the only one that can straddle x, and everything before it lies wholly below. The
  extremes are computed INSIDE the lazy copy in `hf_leaves`, where the data is streaming
  through anyway, so they cost almost nothing and save the view a whole pass. Empty
  buckets get their neighbours' values so the binary search stays monotone.
- **Materialising now runs the engine's leaf.** `logos_sort_buckets_f64` is `hf_leaves`
  without the region copy: one pre-sized context per thread, buckets pulled from an
  atomic queue, `leaf_f64` on each. The first version re-entered `logos_sort_f64` per
  bucket and paid the whole dispatcher 168 times.

| 8M, 14 threads | uniform | heavy_tail | bimodal | clustered |
|---|---|---|---|---|
| eager `logos_sort_f64` | 34.7 ms | 28.4 | 20.5 | 39.3 |
| view + median | **2.85x** | 1.64x | 1.66x | 1.61x |
| view + top 100 | 2.36x | 1.33x | 1.46x | 1.50x |
| view + rank(x) | **2.89x** | 1.53x | 1.53x | 1.56x |
| view + count in [lo,hi) | 2.04x | 1.45x | 1.51x | 1.44x |
| view + 16 scattered ranks | 1.25x | 0.90x | 0.83x | 1.00x |
| view + materialise all | 0.91x | 0.77x | 0.73x | 0.82x |

- Read the last two rows honestly. Sixteen scattered lookups touch 14 of 168 buckets and
  break even; a view is for a few queries. Materialising everything still costs
  1.10-1.37x of an eager sort, and the residual is now structural rather than
  accidental: the lazy copy that lands the buckets in the caller's array is one extra
  pass over the data that the eager path never makes, because its leaf sorts straight
  from the region buffer into place. Keeping the regions alive inside the view would
  remove it at the cost of holding a second copy of the array; that is a design choice
  left open, not a bug.
- Prediction and grade for the whole feature: create at 40-55% of a sort (measured
  35-55%, right), single query = create + one bucket (right), materialise at 1.0-1.3x of
  eager (measured 1.10-1.37x, slightly worse, for the reason above).

## (unreleased) — the sorted VIEW: pay for order only where you look (`logos_view.h`, `make viewbench`)
- `logos_sort_f64` returns a sorted array; most callers then take a median, a top-k or
  a slice and discard the rest. A view partitions into ordered buckets and sorts a
  bucket only when something indexes into it.
- **Why it works.** A partition gives a total order BETWEEN buckets and none WITHIN
  them, which is exactly what a rank query needs: bucket b holds precisely the elements
  of rank [start[b], start[b+1]), so "which bucket holds rank k" is a lookup, exact,
  with no comparison inside any bucket. Buckets are independent, so sorting one costs
  one bucket.
- **Why it pays, measured not argued.** Reaching nbk buckets costs about n*log2(nbk)
  and finishing costs n*log2(n/nbk). Traced at 8M uniform on 14 threads, the partition
  is 20.2 ms and the leaf phase 34.7 ms of a 54.9 ms sort, so **63% of a sort lives in
  the part a view defers** and the clock agrees with the logarithms.

| shape | eager sort | view + median | speedup | materialise everything |
|---|---|---|---|---|
| uniform | 39.4 ms | **14.4** | **2.74x** | 46.6 (0.84x) |
| bimodal | 25.7 | **12.3** | **2.09x** | 37.7 (0.68x) |
| heavy_tail | 31.6 | **16.5** | **1.92x** | 34.4 (0.92x) |
| clustered | 32.1 | 25.9 | 1.24x | 47.6 (0.68x) |

- Building a view and looking at nothing costs 35-50% of a sort, which is the traced
  partition share. A single rank query adds one bucket, about 0.6 ms out of 8M.
- **The honest limits.** Sixteen scattered rank queries touch 14 of 168 buckets and
  land at 0.79-1.13x, so a view is for a few lookups, not for many. Materialising
  everything costs 1.09-1.47x of an eager sort: the view sorts each bucket through the
  full public entry point instead of the engine's pre-sized leaf path, which is fixable
  overhead rather than anything fundamental. Clustered data gains least because its
  partition is the most expensive part of its sort.
- **A negative worth keeping.** The first implementation hand-rolled its own partition
  and cost **two to four times an entire eager sort** — it re-derived, badly, what this
  engine spent several sessions tuning. A branchy binary search for the bucket lookup
  alone cost more than the whole sort it was deferring; making it branchless recovered
  2.5x and was still not close. Sweeping the bucket count from 16 to 244 changed
  nothing, which ruled out the obvious knob. **A lazy API is only worth building on top
  of a partition that is already the best one available.**
- Engine change is four lines plus a flag: `hf_leaves` gains a `lazy` branch that
  copies a bucket's region and spills into place without sorting, and
  `logos_partition_f64` is the public entry point that returns the boundaries (0 means
  a route sorted the array outright, so there is nothing to defer). The default path is
  untouched; all 835 checks pass.

## (unreleased) — non-temporal stores: mechanism confirmed, effect nil. Two memory levers in a row say the scatter is not memory-bound (opt-in `-DLOGOS_STREAM`)
- A normal store to a line that is not resident must first FETCH that line, so writing
  N bytes costs N read plus N written. The scatter overwrites every byte of its
  destination and nothing reads it until after the barrier, so that read-for-ownership
  traffic is pure waste. `hf_flush` already arrives with 2 KB of contiguous data, which
  is exactly the shape a streaming store wants.
- Implemented: `hf_stream_copy` (`_mm256_stream_pd`, scalar prologue to alignment,
  scalar tail), used by `hf_flush`, with one `_mm_sfence` per thread at the end of
  `hf_scatter` before the barrier hands the regions to the leaf phase. The region
  buffer is forced 64-byte aligned so the prologue is empty for every full block.
- **The mechanism fires**: traced, 99.4% of all elements leave through non-temporal
  stores. **The effect is nil**: 0.99-1.01 at 14 threads, and the 1-thread rows sit at
  0.993-1.003, which is a clean null control since the sequential path does not use
  this code at all.
- Prediction and grade: I predicted 1.03-1.10x at 14 threads. **Wrong**, and the
  project's own prior was right: the ping-pong entry already recorded that removing
  37% of memory traffic moved time only 12%, so the scatter is bound by per-core
  latency and fixed costs, not by bandwidth.
- **The two results together are worth more than either.** Huge pages removed
  page-table traffic. Streaming stores removed read-for-ownership traffic. Both were
  verified to actually happen, and both changed nothing. Two independent levers on two
  different kinds of memory traffic, both null, is strong convergent evidence that the
  scatter is not memory-bound in any ordinary sense — and it retires the whole family
  of "make the memory system happier" ideas. The remaining levers are structural:
  fewer passes, better bucket balance, and routes that avoid the scatter entirely.
- **A GCC trap worth writing down, because it cost real time and will recur.** This
  file includes `immintrin.h` INSIDE `#pragma GCC target("avx2,fma,popcnt")`. GCC
  stamps every intrinsic `always_inline` plus the target active where the header was
  read, and an always_inline callee cannot be inlined into a caller whose target is a
  SUBSET. So a helper marked `target("avx")` fails with "target specific option
  mismatch" — and it fails that way even for plain SSE2 intrinsics like
  `_mm_stream_pd`, which is a baffling error for a baseline x86-64 instruction. Any
  new intrinsic helper in this file must repeat the same target string verbatim.
- Kept opt-in per rule 8, with a `LOGOS_TRACE` counter that reports how many elements
  took the path, so the next person can check the mechanism before trusting a number.

## (unreleased) — huge pages: mechanism confirmed, effect nil, and the reason is a compliment to `hf_flush` (opt-in `-DLOGOS_HUGEPAGE`)
- The engine called `madvise` nowhere. On this box transparent huge pages are in
  `madvise` mode, so they are handed out only on request, and `AnonHugePages` read
  **0 kB** for the whole benchmark. An 8M-element sort therefore touched ~128 MB
  entirely through 4 KB pages, against a second-level TLB that covers 8 MB.
- Implemented: `big_alloc`, which rounds allocations at or above 4 MB up to 2 MB,
  aligns them, and advises `MADV_HUGEPAGE`. Applied to the scratch pool, the
  bucket-index cache, the histogram-free region buffer, and the private fallbacks.
  Falls back to plain `malloc` on any failure, so it can never make a sort fail.
- **The mechanism fires.** With it on the process holds ~98 MB of `AnonHugePages`
  instead of none.
- **The effect is nil.** Paired A/B with the copy swap, five shapes at 1 and 14
  threads: **0.98-1.01 at 8M, 0.97-1.02 at 30M**. Every row inside the ~4% floor.
- Prediction and grade: I predicted 1.10-1.30x at 14 threads and 1.05-1.12x at 1.
  **Wrong, and wrong for a findable reason.** I reasoned about the access pattern at
  ELEMENT granularity when the code operates at 2 KB granularity.
- **Why it cannot help here.** `hf_flush` stages HF_BLOCK (256) doubles per bucket and
  flushes them as one 2 KB `memcpy`. The write stream is 2 KB contiguous bursts at
  bucket-random offsets, so it touches about one page per 256 stores. The TLB miss
  rate is set by the BURST size, not the working set — which is precisely why 8M and
  30M measure identically, and that equality is the discriminating evidence. The
  write-combining buffer, added to batch atomics and fill cache lines, had already
  made the scatter TLB-friendly as a side effect.
- Kept opt-in per rule 8. It may still be worth something on a machine whose pool is
  cold, or for a future path that scatters without staging.
- The general lesson: **before optimising a memory access pattern, check the
  granularity the code actually operates at, not the one the algorithm suggests.**

## (unreleased) — re-tested five ledger verdicts against the new noise floor: all five are genuinely neutral (`tools/ledger_rerun.py`)
- Every "mixed" and "noise" verdict in the ledger was reached when an interleaved
  A/B here could not resolve better than about 1.25x. `bench_paired` now resolves
  about 4%, so anything once dismissed in the 1.05-1.25 band had never actually
  been tested. A tenfold improvement in resolution is the new angle the ledger's
  own "do not re-run" rule asks for, so these were re-run.
- **Pairing alone is not enough**, and this is the part worth keeping. Pairing
  cancels the machine's 5-10x thermal drift, because the two variants are timed
  back to back. It does NOT cancel code alignment, because each variant still
  lives in a fixed compiled copy. So every feature is built TWICE with the copies
  swapped. With layout factors L0 and L1 and a true effect E, orientation A reads
  `E*(L0/L1)` and orientation B reads `(1/E)*(L0/L1)`, so `sqrt(A/B)` is E with
  the layout term cancelled exactly, and `sqrt(A*B)` recovers the layout bias.
- Measured, 5 shapes x {1, 14} threads x 24 pairs x 2 orientations, 8M, every
  output verified against qsort:

| feature | previous verdict | re-tested, layout cancelled |
|---|---|---|
| interleaved histogram counters | "mixed", kept opt-in | 0.97-1.04, largest 1.042 did not survive 80 pairs (1.016) |
| cache-line repair in the leaf | "mixed", kept opt-in | 0.95-1.02 |
| bucket-relative int32 digit narrowing | "mixed", +21% / -16% | 0.99-1.02 **on f64 shapes only, see the caveat** |
| linear model splitter table | "0.93-1.09x, noise" | 0.99-1.02 |
| sorting-network pivot, AVX2 leaf | "0.94-1.06x, noise" | 0.98-1.03 |

- **Nothing is promoted.** Rule 8 stands: none of these won, so none of them ship.
  What changed is what we know. Five rows move from "could not resolve" to
  "measured neutral, bounded at about 4% on these shapes", which is a real result
  rather than an absence of one.
- **The layout bias was small here.** `sqrt(A*B)` stayed within 0.96-1.04 across
  all fifty cells and read 0.999 on the 80-pair confirmation. That does not
  contradict the 1.24x from the four-copy `tailbench` null test; it says the bias
  depends on which copies you get, and a two-copy build can be nearly free of it.
  Treat the estimate with the same error bars as everything else here.
- **Caveat, stated because it limits the claim.** `ledgerbench` drives f64 shapes.
  The int-narrowing row's original verdict was on int32 shapes including int_1e6,
  which is not reproduced here, and which is exactly the span band where the new
  parallel counting route now fires. That row is only partially re-tested.
- New: `make ledgerbench LB0=<flags> LB1=<flags>` (two engine copies, paired) and
  `python tools/ledger_rerun.py` (drives both orientations and does the
  arithmetic). `./ledgerbench <pairs> <n> <shape> <threads>` narrows to one cell,
  which is how the one borderline row got 80 pairs cheaply.

## (unreleased) — BUG, shipped and fixed: the log-log charts rendered blank
- The uPlot upgrade in the entry below shipped with every log-log panel empty.
  Linear axes were fine, so the report looked correct until the toggle was pressed.
- Cause: **on a log axis uPlot passes the `values` callback a splits array with
  `null` in every slot it wants gridded but NOT labelled.** Both axis formatters
  assumed a number and called `.toFixed` on it, which threw once per chart, 18
  times, after uPlot had already built the legend. Hence a legend with no plot.
  Linear splits contain no nulls, which is why only one mode broke.
- Fixed by making both axis formatters hand `null` straight back, and by making
  `fmtN` and `fmtMs` null-safe in their own right. `fmtMs` also gained three
  decimals below 0.1 ms, because a log axis puts splits down there and they were
  all rendering as "0 ms".
- **The checker passed the broken report, and that is the more useful finding.**
  `check_report.js` substitutes a uPlot stub and exercises every formatter, but it
  only ever fed the axis callbacks well-formed numbers, so it tested a shape the
  real library never produces. A stub only generates the inputs you thought of.
  It now passes a splits array with nulls interleaved, in both directions, and
  asserts the returned label array matches the splits length. Verified the way a
  regression test should be: it fails on the old report and passes on the new one.
- **New: `tools/browser_check.py`.** Renders a report in real headless Chrome or
  Edge with an error trap installed ahead of the report's own script, and fails on
  any uncaught error. It has no blind spot, because it is the actual browser. Runs
  both axis modes, takes `--shot out.png` for a screenshot, and skips cleanly with
  exit 0 when no browser is installed, so `make scalebench` and `make scale-full`
  both call it. This is what found the bug; the stub could not have.
- Both reports now read clean in every mode.

## (unreleased) — the scale report is interactive: uPlot charts, zoom, scrub, series toggles
- `scale-report.html` drew its own SVG. It now draws with uPlot (1.6.31, MIT), which
  brings drag-to-zoom on either axis, double-click to reset, a live legend that reads
  every series at the cursor's n, and click-to-hide per series. There is also a reset
  button beside the linear/log toggle.
- **uPlot is vendored and INLINED, not fetched.** These reports get opened from disk
  and passed around, so a CDN dependency would break them. `engines/c/vendor/` holds
  the 50 KB minified build, `tools/embed_uplot.py` turns it into `uplot_embed.h`, and
  `scalebench.cpp` splices it at two placeholders. The header is checked in, so the
  build needs no Python; regenerate it only when bumping the version.
- Everything that made the report worth reading is unchanged: the same measured data,
  the O(n) and O(n log n) reference curves anchored at logos's largest point, the
  empirical exponents from the least-squares slope of log t against log n, the tables,
  the calibration banner, and the verify-before-timing discipline.
- Theming survived the move to canvas. uPlot cannot read CSS variables, so the stroke
  colours are resolved from the same tokens in JS at build time and every chart is
  rebuilt when the colour scheme changes.
- **`tools/check_report.js` rewritten, and it is now a real test.** It could only count
  SVG elements, which a canvas chart does not have. It now substitutes a uPlot stub
  that asserts the contract each chart must satisfy: one data array per series, all the
  same length, x strictly ascending and positive, no NaN, nothing non-positive on a log
  axis, every series carrying a stroke and a label, and every formatter callable
  including on a null. Then it flips to log-log through the real button handler and
  revalidates all eighteen panels. `make scalebench` and `make scale-full` run it.
- Honest note on what this buys. Each series has eight to twelve points, so zooming is
  of limited use; the wins are the cursor readout, which compares every contender at one
  n without hunting for tooltips, and hiding series to decluttered a busy panel.

## (unreleased) — one sentinel no longer destroys the int32 range gate (`make intbench`)
- Measured the turn before: a narrow int32 bulk sorts in 3.2 ms at 14 threads, and
  adding a SINGLE INT32_MIN to eight million elements made it 36.3 ms. The gate read
  raw min/max, so one far value blew the span past the table and the whole array fell
  back to a partition. Frequency did not matter, distance did.
- `count_try_i32` now takes its table window from the SAMPLE rather than from the
  extremes. The pre-gate pass already tracks the five smallest and five largest sample
  points, which yields both the plain min/max gate and a trimmed window in one pass.
  When the true span overflows the table the route does not give up: it counts the
  window, spills everything outside it to per-thread lists, and sorts those as their
  own runs. Slot order is value order and the tails are outside the window on both
  sides, so the output is low-tail, bulk, high-tail with no merge at all.
- Two windows, chosen in order. The sample's own min/max is preferred because it
  covers the most, and it is the right one when the sample never saw the sentinel.
  The trimmed window is the fallback for when it did.

| shape, 14 threads | before, x baseline | after, x baseline |
|---|---|---|
| bulk + one INT32_MIN | **11.3x** | **1.19x** |
| bulk + both extremes | 11.7x | 1.25x |
| bulk + one -1 (near the bulk) | 1.06x | 1.04x |
| bulk + 5% INT32_MIN | 10.8x | declines, by design |
| zero-straddling, all-negative | 1.00x | 1.00x |

- Ratios are against the baseline row of the SAME run, because the box drifts 5-10x
  between runs (rule 2). Absolute: the sentinel row went from 36.3 ms to 5.0 ms while
  its baseline moved 3.2 to 5.5.
- **The limit, stated plainly.** The spill budget is n/32, and the trim is five sample
  points out of 2048. So this absorbs sentinels rarer than about 0.25%, and a 5%
  sentinel still declines to the partition. That is the intended answer: a tail worth
  more than a thirty-second of the array is not a tail, and the general fix for a
  frequent-but-distant value is gulf-splitting with a second table, which the f64 path
  has and this one does not.
- **The 1-thread path is unchanged and still slow on these shapes** (59.9 ms against a
  24.4 ms baseline). `leaf_i32` gates its counting sort on raw min/max. Same fix
  applies; not done.
- Declining is free: nothing is written to `a[]` until after the spill budget is known,
  and every thread reaches the same barriers on the abort path.

## (unreleased) — the parallel int32 path gets a counting route: up to 10x, and it needed no probe at all (`make intbench`)
- Fixes the defect recorded in the entry below: `psort_i32` had no counting,
  lattice or low-cardinality route, so it sampled, histogrammed and scattered 8M
  elements whatever the span, and lost to a SINGLE core for every span under
  about n/8.
- The fix asks nothing. An int32 array is a lattice of step 1 by the TYPE, so the
  only question is whether the span fits a table, which is a min/max pass. The f64
  lattice route has to propose a step from the sample, verify every element with a
  relative tolerance, screen NaNs and keep an abort path for two doubles sharing a
  slot; none of that is representable for int32, because int32 -> uint32 is a
  bijection.
- `count_try_i32`: parallel min/max, then per-thread count tables filled from an
  ATOMIC chunk queue rather than static slices (so efficiency cores pull fewer
  chunks instead of stragglering behind the join, FINDINGS 3v), then each thread
  folds the P tables over a contiguous slot range it owns alone, then emits. Slot
  order is value order, so each thread's output is one contiguous run and no merge
  is needed. `-DLOGOS_NO_COUNT_I32` disables it.

| span | P/1 before | P/1 after | 14T after |
|---|---|---|---|
| 100 (categories) | 0.34 | **2.88** | 3.8 ms |
| 1e3 (scores) | 0.33 | **3.97** | 3.3 |
| 1e4 | 0.34 | **4.66** | 2.8 |
| 1e5 (zip-ish) | 0.46 | **3.33** | 5.1 |
| 1e6 | 0.88 | 0.87 | declines at the pre-gate |
| n = 8e6 | 2.37 | 2.64 | untouched |
| 1e8 | 1.78 | 1.73 | untouched |
| full 2^32 | 1.79 | 1.81 | untouched |

- Prediction and grade: I predicted 14T would fall from ~36 ms to 6-12 ms, P/1 of
  1.0 to 2.0. Measured 2.8-5.1 ms and 2.9-4.7. **Too conservative in both size and
  ratio**, because counting sort's write pass is sequential per thread and scales
  better than the scatter it replaced.
- **The span must be computed in 64 bits.** INT32_MAX - INT32_MIN is -1 in 32-bit
  arithmetic, so a `span <= budget` gate written in 32 bits passes on the widest
  possible input and then sizes a table from -1. `make intbench` prints this before
  it benchmarks anything. The sequential leaf was already correct here; the trap was
  latent for new code, and mode 9 of the harness now pins it.
- **A sample pre-gate, on the calling thread, before anything is spawned.** The
  first cut ran the full parallel min/max and only then declined, which cost the
  span-1e6 row (0.88 -> 0.55). A sample's span is a LOWER bound on the true span, so
  a sample that already overflows the table proves the array does, with no false
  negatives. This is the J5 lesson from the ledger, re-learned: probe on the caller
  before paying for threads.
- Harness int32 coverage 30 -> 180 checks (835 total): span 1 and span 2, the table
  cap and one past it, all-negative, zero-straddling, the INT32_MIN/INT32_MAX pair,
  narrow bulk with far sentinels, and every element the minimum, each at 1, 4 and 16
  threads.
- **Still open, and now the biggest int32 loss:** one far sentinel in an otherwise
  narrow array destroys the gate. Bulk of 100 distinct values takes 3.2 ms at 14
  threads; add a single INT32_MIN and it takes 36.3 ms, **11x**, for one element out
  of eight million. The window has to come from robust sample quantiles or from the
  gulf-splitting the f64 path already does, not from min/max. A sample bounds the
  bulk, never the tail.

## (unreleased) — MEASURED DEFECT: the parallel int32 path loses to a single core on small-span data, by up to 3.6x (`make intbench`)
- An int32 array is a lattice by construction: step exactly 1, base at the minimum.
  That is a fact of the TYPE, not of the data, so it is true without probing, without
  a tolerance, with no NaN case and with no chance of two values sharing a slot. The
  sequential leaf uses it (`leaf_i32`: counting when span <= n, byte-skip LSD radix
  otherwise). **`psort_i32` does not.** It goes prescan -> sample -> splitters ->
  histogram -> full scatter for every input, and has no counting, lattice, low-card,
  relocation or near-sorted route at all.

| span (max - min) | 1 thread | 14 threads | P/1 |
|---|---|---|---|
| 100 (categories) | 37.8 ms | 134.2 ms | **0.28** |
| 1e3 (scores) | 31.6 | 95.8 | **0.33** |
| 1e4 | 30.3 | 91.5 | **0.33** |
| 1e5 (zip-ish) | 44.9 | 91.7 | **0.49** |
| 1e6 | 76.0 | 77.4 | 0.98 |
| n = 8e6 | 168.3 | 64.3 | 2.62 |
| 1e8 (> n) | 105.2 | 62.5 | 1.68 |
| full 2^32 | 132.0 | 80.1 | 1.65 |

- The crossover is at span ~ n/8. Below it the parallel path pays a sample, a
  histogram and a scatter of all 8M elements to do a job that is two passes over a
  count array, and 14 threads finish after one core would have.
- This is the f64/i32 divergence catching up with the project. Everything built over
  the last sessions landed on the f64 dispatcher: the certificates, the histogram-free
  partition, the sample-once gates, the fast sample, the context pool, the AVX2 leaf,
  the removal of libc qsort from sampled paths. The i32 parallel path predates all of
  it and still sorts its own sample with `qsort`.
- Two things the int path can ASSUME rather than measure, both provable:
  1. **The lattice.** Step is 1 and base is the minimum. The gate is just
     `span <= budget`, and the prescan already walks the array, so min/max is free
     there.
  2. **Key space and value space are the same test.** For int32 the order-preserving
     key is `(uint32_t)x ^ 0x80000000u`, which over the signed range is exactly
     `x + 2^31`: monotone AND affine. A Kolmogorov-Smirnov statistic is invariant
     under a monotone affine map, so the value-space and key-space certificates are
     provably identical for integers and only one is worth computing. For doubles
     `total_key` is logarithmic, which is why the two genuinely differ there.
- Not fixed in this entry, only measured and recorded. `make intbench` is the bench.

## (unreleased) — largest-leaf-first scheduling: measured NEUTRAL, and the reason is the interesting part (`make lptbench`)
- The idea: leaves are handed to threads by one atomic increment each, so whichever
  leaf is picked up LAST sets the finish time. Greedy self-scheduling in an
  arbitrary order has a makespan of at most (2 - 1/P) times optimal; taking the
  longest job first tightens that to (4/3 - 1/(3P)). The leaf sizes are already
  known by then (they were just computed for the output offsets), so ordering them
  costs one heapsort over at most 256 indices and nothing else.
- Prediction, written first: uniform ~1.00, skewed shapes 1.02-1.10, nothing above
  1.10 because the balance check already caps a bucket at 3x its share.
- Measured, paired harness, 24 pairs, 8M, 14 threads: uniform 1.046, Pareto 0.951,
  exponential 1.005, lognormal 0.999, bimodal 0.980, clustered 1.011, one fat
  region 0.987. Every row inside what this box resolves. The prediction was right
  about the size and wrong to expect any of it to be visible.
- **Then the mechanism was traced instead of guessed, and it says the target is not
  there.** The splitters are SAMPLE QUANTILES, and equalising the buckets is exactly
  what a quantile splitter does. Traced largest leaf over the mean: **1.10x on
  uniform, 1.48x on Pareto**, against the 3x the balance check would permit. Put
  those in the greedy bound W/P + L(1 - 1/P) with 12 leaves per thread and the
  entire straggler cost is 8% of the leaf phase on uniform and 11% on Pareto;
  largest-first recovers only part of that, so the ceiling is 1-3% of one phase.
  The scheduling problem had already been solved upstream by the partition design.
- Kept as opt-in (`LOGOS_LPT=1`), default off, per rule 8. The toggle is read with
  getenv per partition call rather than cached, which is what let ONE binary measure
  both orders in a paired A/B with no second copy of the code and therefore no
  code-layout bias. That pattern is worth reusing: a runtime toggle sidesteps rule
  3's whole problem.
- **Where the idea should actually pay, still open:** the counting and lattice
  routes carve STATIC per-thread ranges and have a measured 1.3-2x imbalance. Same
  reasoning, different code, a real target.

## (unreleased) — paired A/B is now the standard for small effects (`bench_paired`, `make pairprobe`)
- Tested three ways of making this box measure better, and only one survived.
- **CPU pinning: rejected, measured.** `taskset` improved the spread in one
  run (1-thread max/min 1.40 -> 1.13) and did not reproduce in the next (the
  same pinned configuration gave 1.13 then 1.32, while unpinned gave 1.12).
  The reason is structural: WSL2's CPUs are virtual and the Windows hypervisor
  places them on physical cores as it likes, so taskset cannot hold a core. A
  single thread pinned to CPUs 12-19 ran FASTER than unpinned, which rules out
  the "those are the efficiency cores" reading entirely.
- **cgroup cpuset isolation: not reachable.** The controller is enabled but the
  cgroup tree needs root, and it would hit the same virtualisation boundary:
  isolating guest CPUs does not isolate physical cores.
- **Thread count: no change.** With counts rotated inside each rep so they share
  thermal state, 12 and 14 tie on the minimum (19.16 vs 19.24 ms) and the
  across-run spread favours MORE threads (20 threads 1.30, 14 threads 2.22),
  the opposite of what the original un-interleaved probe concluded. But that
  spread is mostly drift, which cancels in a paired test, so it is the wrong
  thing to optimise. BENCH_THREADS stays 14.
- **Paired A/B: adopted.** `bench_paired()` in `bench_common.h` measures two
  variants ADJACENTLY, alternating which goes first, and returns the median of
  their ratio with p10/p90. Within a pair the machine state is shared, so the
  5-10x drift cancels; alternating cancels the ordering bias (the first half of
  a pair measured 2-4% slow, from destination-buffer page state).
- **`make pairprobe`** runs the same harness with the SAME engine in both
  halves, so the true ratio is 1.000 and everything it prints is this machine's
  error. Read it like a calibration line. Today: a single pair has a p90
  deviation of 8-14%, and the median of 40 pairs resolves about 3.7% at 8M and
  6.1% at 1M. Error falls as 1/sqrt(pairs), so 160 pairs buys about 2%.
- Net: the resolving floor goes from about 1.25x (best-of-N across separately
  compiled copies, 3r) to about 1.04x. That is the standard for anything small
  from here on.

## (unreleased) — MACHINE CHARACTERISED: the reference box runs 5-10x slower hot than cold, and a long benchmark is always hot
- All day the calibration line inside every benchmark read "loaded" no matter
  how quiet the machine measured beforehand. The cause is not an external
  process. Immediately after the 15-minute benchmark chain, a compute-bound
  loop took 202 ms and a 64 MB memcpy 35.1 ms; after 26 seconds of idling the
  same two took 37.0 ms and 3.66 ms. That is 5.5x and 9.6x, recovering
  smoothly, and the two metrics move in lockstep (their ratios track within
  0.1 of each other), so it is not core clock alone.

| seconds idle after the run | compute (xorshift chain) | memcpy 64 MB |
|---|---|---|
| 1 | 202.0 ms | 35.1 ms |
| 5 | 130.4 | 13.9 |
| 12 | 101.9 | 10.2 |
| 20 | 48.1 | 4.1 |
| 26 | **37.0** | **3.66** |
| 26–50 (plateau, bursty) | 37–62 | 3.7–6.0 |

- The mechanism is not identifiable from inside WSL2: /proc/cpuinfo reports a
  static nominal 2995 MHz, no thermal zone exposes a temperature, no cpufreq
  directory exists, and steal time reads zero. Package power/thermal limiting
  and host scheduling of the VM are both consistent with the evidence.
- **Consequences, and they are the important part.** (1) "Wait for the box to
  go quiet, then run" is futile for anything that takes minutes: the idle probe
  measures the cold machine and the benchmark heats it within seconds. Hours
  were spent on this today. (2) Absolute milliseconds are not comparable across
  runs at all, and a benchmark's own calibration line will always say loaded.
  (3) Comparisons are trustworthy only where contenders alternate at fine grain
  inside one row, which parbench, and every A/B bench here, already do -- the
  speedup columns are self-normalising because their baseline is measured in
  the same row at the same moment. (4) Comparing across ROWS of one run is
  unsafe: the machine's speed wanders by 5x on a scale of tens of seconds.
- Methodology rules 2 and 10 amended. This also retires a long-standing
  misreading: several entries across the project's history blame "the two hogs"
  (VS Code's GPU process, OneDrive) for drift that this measurement says is
  mostly the machine's own thermal state under the benchmark's load.

## (unreleased) — per-mode certificate: a linear map where every mode is uniform inside itself (`make tailbench`)
- The engine already splits the value range at gulfs (gaps between consecutive
  splitters more than BT_MODE_GAP times the median) and gives each mode its own
  value table. The profile said the modes were not the problem: bimodal at 14
  threads spent 10.2 ms in the scatter against uniform's 3.5 while their leaf
  phases matched (11.9 against 11.1). The cost is the address calculation, not
  the sorting.
- So: certify each mode separately. Take the sample slice that falls inside a
  mode and run the same Kolmogorov-Smirnov test the global certificate uses. If
  EVERY mode certifies, drop the tables entirely -- pick the mode with up to
  seven branch-free compares, then interpolate linearly into the stretch of
  buckets that mode owns. Modes partition the splitters contiguously, so one
  mode's top bucket and the next mode's bottom bucket meet exactly and the map
  is monotone, which is all a partition needs. `-DLOGOS_NO_MODEKS` turns it off.
- Measured (FINDINGS 3s), with the copy-swap control in both directions:

| shape | 14T, map off | 14T, map on | layout A | layout B |
|---|---|---|---|---|
| bimodal (two uniform bands) | 21.7 | **17.3** | 1.38× | **1.25×** |
| three uniform bands | 22.2 | **18.1** | 1.33× | **1.23×** |
| two gaussians | 23.4 | 22.7 | 1.01× | 1.03× (declines: mode KS 0.164) |
| heavy_tail, uniform, quantized, near_sorted, int_wide | | | 1.00–1.07× | 1.00–1.07× (untouched) |
| every shape at 1 thread | | | 1.00–1.06× | 1.00–1.06× (the route is parallel-only: identical code) |

Layout A put the feature in copies 1 and 3, layout B in copies 0 and 2; the gain follows the feature in
both, which is the control required for any claim under 1.25× (§3r). Layout B was the quieter run
(calibration 3.2 and 3.4 ms) and is the number to quote.

- It fires only on genuinely piecewise-flat data. A mixture of gaussians does
  not certify (first mode KS 0.164) because a hump is not uniform inside, and
  a wide mixture does not even separate into modes; both fall back to the
  per-mode tables exactly as before. Worth saying plainly: the shape that
  gains most is the synthetic bimodal one in our own suite, so a piecewise-
  uniform shape (`three_uniform_bands`) and a realistic mixture
  (`two_gaussians`) were both added to `tailbench` before the claim was made.
- One bug of mine is worth recording because it would recur: when slicing the
  sample per mode, the boundary has to sit INSIDE the gulf. Cutting at the
  mode's last splitter leaves a stray point on the wrong side, and cutting at
  the next mode's first splitter drags points forward; either way one slice
  stretches across the whole gap and its KS reads 0.99 instead of 0.02. The
  midpoint of the gulf is correct.

## (unreleased) — MEASUREMENT DEFECT FOUND: the interleaved A/B has a per-copy layout bias of up to 1.24x
- Every interleaved A/B in this project compiles N copies of the engine with
  different macros, renames their public symbols (the RENAME pattern) and runs
  them against each other inside one process. That controls for machine load,
  which was the point. It does NOT control for the copies themselves.
- Measured directly: `make tailbench TB0= TB1= TB2= TB3=` builds four copies
  with IDENTICAL flags and identical source, differing only in symbol names.
  On a quiet box the four columns still differed by up to 1.16x, and one copy
  was systematically the fastest across unrelated shapes (uniform 1.09x,
  exponential 1.16x, pareto 1.13x, int_wide 1.24x in a second layout). The
  cause is code and data layout: different symbol lengths move functions to
  different alignments. It is deterministic, so it does not average out over
  reps, and best-of-N does not remove it.
- **Consequence: no interleaved A/B in this project can resolve an effect
  below about 1.25x** unless the variant-to-copy assignment is swapped and the
  effect is shown to follow the variant. That control is cheap -- rebuild with
  the flags permuted and re-run -- and is now the required practice for any
  claim under 1.25x. Rule 3 of the methodology has been amended.
- Re-graded with it (see TRUTH_TABLE N2 and the rows named there): the results
  above the band stand (perfect hash 15-24x, Weyl sampling 9-14x, near-sorted
  route 2.3-2.9x, cardinality cliff 6.5x, lattice window 1.31-1.40x, thread
  caps 1.3-1.8x on the rows that carry them). The results below it are now
  marked unconfirmed as end-to-end ratios: the serial fixed costs (1.02-1.18x)
  and most of the sample-once rows (1.07-1.19x). Both of those were also
  measured directly by trace -- serial work before the workers 2.0 -> 0.9 ms,
  the scatter-to-leaves step 1.15 -> 0.09 ms, five samples reduced to one --
  and those direct measurements are unaffected, so the mechanisms stand while
  their end-to-end value does not.
- The key-space certificate was re-measured with the control: with the feature
  in copies 1 and 3 it gave Benford 1.32x, with it in copies 0 and 2, 1.15x.
  The gain follows the feature, so it is real; its size is about 1.2-1.3x, not
  the 1.45-1.50x the uncontrolled runs suggested.

## (unreleased) — heavy-tail structures: a certificate in key space, a balance check on every map, a dominant-duplicate guard (`make tailbench`)
- **Certificate in key space (shipped, `-DLOGOS_NO_KEY_KS`).** The
  order-preserving key is monotone in the logarithm, so log-uniform data is
  uniform there and earns the same one-multiply grid the value certificate
  gives uniform data. Measured on the sample: Benford 0.018 in key space
  against 0.746 in value space, and it certifies. A true Pareto does NOT --
  the log of a Pareto is exponential, 0.692 -- so the test correctly declines
  it. Benford at 14 threads, with the copy-swap control: about 1.2-1.3x
  (1.32x with the feature in one pair of copies, 1.15x in the other; the
  uncontrolled runs said 1.45-1.50x and were reading the layout bias as well).
- **Balance check on every certified map (shipped).** The per-thread trial
  times the LOOKUP, not the partition the lookup produces, and it cheerfully
  picked a map that sent 63% of the data to the spill list. Each map is now
  applied to the sorted sample and dropped if any bucket would take more than
  HF_BAL_MAX = 3x its share; the cost is about 16k lookups, ~20 us. On Pareto
  this fires at 4.8x and the sort falls back to the key table with no spills.
  It protects the two older certified modes as well as the new one.
- **Dominant-duplicate guard (shipped).** A single value larger than a bucket
  region overflows it however the splitters fall, and the spill test only
  noticed after a whole pass over the data. The sample already knows the
  largest share any one value holds, so the partition declines up front:
  measured 2.3 ms instead of scattering 8M elements first.
- **Exponent-histogram map (OPT-IN, `-DLOGOS_EXMAP`, measured negative).**
  Index a flat table by the binade -- the integer part of the logarithm --
  and interpolate on the mantissa, allocating resolution in proportion to the
  sample's density: an approximate inverse CDF in one load and one multiply.
  The first cut indexed on the key's top byte, which holds only the top 7 of
  the 11 exponent bits, so Pareto data landed in TWO entries and 63% of it
  spilled; indexing on the relative binade gives 13 to 33. It is then
  correct, and still slower: the trial selects it on lognormal and
  exponential because its lookup is cheapest, and those sorts run 0.91x and
  0.80x of the key splitter table it displaced, because a heavy tail decays
  inside each binade too and its buckets spill where the table's do not.
  Kept opt-in: the machinery, and the balance check it forced, are what a
  better inverse-CDF map would need.
- Measurement note: the reference box was between 2x and 6x off its quiet
  calibration all day (memcpy 2.9 ms quiet, 17.6 during the decisive run and
  6.4 immediately after). Identical-code columns spread 12-18%, so only the
  Benford result and the exponent map's losses clear the band; the rest of
  the table is inside it and is not claimed.
- New `tailbench`: tails of several weights, the log-uniform case, a discrete
  tail, a dominant duplicate, and the usual guards.

## (unreleased) — a cardinality cliff, and a verified perfect hash for the counting route (`make cardbench`)
- **The defect.** The low-cardinality counting route's per-element lookup used
  a branchless linear scan up to 32 distinct values and a binary search above
  it, and its gate admits up to 64 distinct values in the sample. Per element
  at 8M: 2.0 ns at 5 distinct, 30.8 ns at 64. Nothing in any bench had a
  cardinality in that window (the shapes are 5, 1000, 4096 and 30000), so an
  8M array with 33 to 64 distinct values sorted 6.5x slower than the same
  array with 200: 228-318 ms against 35 at one thread, 32-40 against 7.2 at
  fourteen. Found by a kernel microbenchmark while answering a question about
  the few-distinct loss to x86-simd-sort, then reproduced end to end.
- **The fix (variant A, shipped, `-DLOGOS_NO_LC_PHASH` restores the scan).**
  The gate has already sorted a sample and found the distinct values, so a
  multiply-shift perfect hash over their keys into a byte table turns the
  lookup into a multiply, a shift, a byte load and one compare: 1.1 ns per
  element at every cardinality. The compare is what makes it exact -- a value
  the sample never saw fails it and falls into the old incremental table, so
  the counts are right whatever the sample missed. The table is cleared on
  every attempt and each attempt is verified by reading it back, so a
  colliding multiplier merely fails and no slot can index past the key array.
- **Variant B (lattice's direct index before the scan): rejected, measured.**
  The direct index is the cheapest kernel in isolation (0.8 ns/element against
  2.0) but its table spans the value range -- 4096 slots of key, value and
  count, ~96 KB -- while the perfect hash's is 4 KB and stays in L1. On
  5-distinct lattice data it ran 51.5 ms against the hash's 20.9, and on
  random_50 at 14 threads it lost to the shipped scan (98.6 vs 64.1 ms). A
  fourth microbenchmark win that does not survive integration (ledger C2-C4).
- **Variant C (how wide should the gate be now the lookup is O(1)):** see the
  table; the shipped gate of 64 is kept.

Interleaved, best of 5, rotated order, n = 8M (calibration memcpy 4.0 ms; the run was still ~2× off
quiet, so the guard rows carry a ±10% band and only the large ratios are decisive):

| shape | thr | scan (shipped before) | perfect hash | ratio |
|---|---|---|---|---|
| 33 distinct, lattice | 1 / 14 | 255.8 / 30.4 | **14.7 / 7.4** | 17.4× / 4.1× |
| 50 distinct, lattice | 1 / 14 | 276.1 / 36.5 | **18.0 / 5.8** | 15.3× / 6.3× |
| 64 distinct, lattice | 1 / 14 | 309.7 / 36.9 | **16.3 / 5.6** | 19.0× / 6.6× |
| 33 distinct, arbitrary | 1 / 14 | 253.3 / 31.5 | **15.9 / 7.7** | 15.9× / 4.1× |
| 50 distinct, arbitrary | 1 / 14 | 286.4 / 34.4 | **16.5 / 6.2** | 17.4× / 5.6× |
| 64 distinct, arbitrary | 1 / 14 | 317.4 / 74.9 | **15.6 / 13.1** | 20.3× / 5.7× |
| 5 distinct, lattice | 1 / 14 | 29.2 / 9.1 | **15.0 / 7.8** | 1.95× / 1.17× |
| 5 distinct, arbitrary | 1 / 14 | 30.2 / 8.4 | **16.4 / 6.8** | 1.84× / 1.23× |
| 200 distinct (gate declines, other route) | 1 / 14 | 34.5 / 10.3 | 36.0 / 8.7 | 0.96× / 1.18× |
| quantized, mod_card_30k, uniform, near_sorted | 1 / 14 | | | 0.91–1.12×, inside the run's band |


Variant C, how wide the gate should be now that the lookup is O(1), is **unresolved**. Its two builds
widened the gate and the hash table together, which confounds them, and the machine degraded during the
run (memcpy 2.9 → 38 ms in bursts), so its column is not reported. What is established: the multiplier
search finds a collision-free table on the first or second attempt for every cardinality the shipped
gate admits (verified for 5, 33, 50 and 64 keys at 12 bits, on a lattice and off it), and fails at 200
keys in a 4096-entry table, so any future widening must grow the table with the gate.


- New `cardbench` covers the ladder no other bench reaches: 5, 33, 50, 64 and
  200 distinct values, each on a lattice and off it, with quantized,
  mod_card_30k, uniform and near_sorted as guards.

## (unreleased) — the serial fixed costs: batched-prefetch sample, vectorized sample sort, persistent context bundles (`make probebench`) — SHIPPED as the default: the trace shows the mechanism and the quiet half of the end-to-end A/B confirms 1.07-1.18x at 1M and 1.02-1.10x at 8M on the parallel rows with the 1-thread rows unchanged; the persistent bundles cost about 6 bytes per element of the largest capacity ever requested (48 MB after an 8M single-threaded call), the same order as the existing buffer pool
- The question was whether the probe is parallelised. It is not, and threading
  it would be the wrong fix: the engine creates its threads per phase, and a
  pthread_create costs about what the whole stage-1 sample costs. The trace
  (uniform, 14 threads) showed the serial work before the workers at ~2 ms
  per call regardless of n (5376 random reads at ~100 ns each and two qsorts),
  plus 0.8-1.8 ms between scatter and leaves for fourteen fresh context
  allocations sized to the largest bucket: 9% of an 8M sort, 21% of a 1M one.
- **Batched prefetch** (`sample_read`): every prefetch for a stage is issued
  before the first read, so the line-fill buffers overlap the misses instead
  of taking them one at a time. **Vectorized sample sort** (`sample_sort`):
  the AVX2 quicksort sorts the sample when it has no NaN, qsort otherwise.
  `-DLOGOS_NO_SAMPLE_FAST` restores the plain reads. Stage-1 sample 0.5 ->
  0.15 ms. Follow-up the same day: the NaN case no longer falls back to libc
  qsort either; `sample_sort` compacts NaNs to the tail (every gate declines
  on any NaN, so their order is irrelevant) and sorts the rest with the AVX2
  quicksort, or with the engine's own scalar introsort where AVX2 is absent.
  Every sample the engine draws (the per-gate samples of the non-default
  paths, the near-sorted strays, the histogram-free spills, the mode
  detection's gaps) goes through it; libc qsort remains only on the three
  out-of-memory fallbacks. Measured on a 5376-point sample: libc qsort
  0.64 ms, AVX2 0.058 ms (10.9x), scalar introsort 0.30 ms (2.0x); with 1%
  NaN 0.70 / 0.070 / 0.29 ms.
- **Persistent context bundles** (`ctx_init` / `ctx_free`, `-DLOGOS_NO_CTX_POOL`):
  a context's buffers (~6 bytes per element of capacity plus ~340 KB fixed)
  now live in a static table indexed in order of ctx_init calls within one
  top-level sort, grown when a call needs more, never freed. Every call site
  is unchanged. The bundle cursor restarts at each public entry and keeps
  counting through the near-sorted route's nested call, so an outer sort's
  contexts are never re-used underneath it. One sort at a time, the same
  contract as the buffer pool. The serial step between scatter and leaves
  0.8-1.8 -> 0.09 ms. The 1-thread path gains nothing from it, because the
  AVX2 leaf never touches the context buffers and untouched pages cost
  nothing (the prediction of 8-12% there was wrong for that reason).
- Both configurations pass the 685-check harness.
- Measured end to end (four engines interleaved, 1, P/2 and P threads, 1M and 8M):

| shape | n | thr | neither | fast sample | ctx pool | both | both / neither |
|---|---|---|---|---|---|---|---|
| uniform | 1M | 14 / 7 | 3.45 / 3.44 | 3.16 / 3.21 | 3.32 / 3.21 | **3.05 / 3.12** | 1.13 / 1.10× |
| near_sorted | 1M | 14 / 7 | 2.27 / 2.02 | 2.18 / 1.83 | 2.27 / 1.91 | **2.05** / 2.06 | 1.11 / 0.98× |
| few_distinct | 1M | 14 / 7 | 1.77 / 1.60 | 1.52 / 1.45 | 1.57 / 1.50 | **1.50** / 1.47 | 1.18 / 1.09× |
| quantized | 1M | 14 / 7 | 2.76 / 2.48 | 2.65 / 2.29 | 2.41 / 2.50 | 2.99 / **2.21** | 0.92 / 1.12× |
| uniform | 8M | 14 / 7 | 18.17 / 25.55 | **17.46** / 24.35 | 18.64 / 24.84 | 17.73 / 25.44 | 1.02 / 1.00× (fast sample alone 1.04 / 1.05×) |
| near_sorted | 8M | 14 / 7 | 10.78 / 10.14 | 10.49 / 10.02 | **9.89** / 9.95 | 10.25 / 10.55 | 1.05 / 0.96× |
| quantized | 8M | 14 / 7 | 10.38 / 10.58 | **9.41** / 10.11 | 10.20 / 10.79 | 10.34 / 11.31 | 1.00 / 0.94× (fast sample alone 1.10 / 1.05×) |
| every shape | 1M and 8M | 1 | | | | | 0.98 to 1.06×: identical work, as the trace predicts |
| block_perm, zipf, heavy_tail, int_wide | 8M | | | | | | the box reloaded during these rows (heavy_tail 14T at 62 ms against 21 quiet); not read |


## (unreleased) — sample once, decide once: one sample answers every gate (`make sodobench`) — SHIPPED as the default (-DLOGOS_NO_SODO restores the per-gate samples): no loss on any shape once the two-stage draw was in, 1.07-1.39x on zipf and 1.07-1.22x on the 1M rows of the partition shapes, neutral at 8M; every run of the three was on a loaded box, ratios within rows only
- The fixed-order dispatcher drew five samples (2048 for the low-cardinality
  gate, 2048 for the moderate-cardinality gate, 40 strided pairs for the
  near-sorted gate, four windows for its pre-gate, 5376 for the splitters and
  the KS certificate), and each gate was a threshold on one statistic with no
  estimate of what the route would cost. `-DLOGOS_SODO` replaces that with
  one sample in two stages: the first 2048 points of the Weyl sequence (a
  prefix of a low-discrepancy sequence is itself equidistributed) are sorted
  once and carry the distinct count, the singletons and doubletons, NaN, and
  an inversion rate over Fibonacci-lag pairs (indices i and i+144 sit 0.37%
  of n apart in position, so ~1900 strided pairs replace 40); only if the
  partition is reached are the remaining 3328 points read, sorted and merged
  in, with the KS distance computed on the full 5376, and the partition
  reuses that sample instead of drawing its own.
- The new statistic is Chao1, an estimate of the TRUE number of distinct
  values from the sample's singletons and doubletons (d + f1(f1-1)/(2(f2+1))).
  The counting routes are attempted only when it fits their cap, which is
  what the "abandoned attempt" defects lacked: a sample with 60 distinct
  values and many singletons means thousands, and zipf no longer starts a
  low-cardinality map it will abort. The routes keep their cheapest-first
  order: a cost model from the measured per-route costs (counting 0.8,
  lattice 0.9, relocation 1.1, near-sorted 1.3, partition 2.3-2.7 ns/elem at
  14T) reproduces that order, so the change is in how the gates are answered,
  not in which route wins when several apply.
- A first cut drew the full 5376 up front and lost 5-16% on few_distinct and
  10% on block_perm at 1M: the cheapest routes were paying for the
  partition's sample. The two-stage draw removed those losses.
- Measured (two engines interleaved, P and P/2, 1M and 8M):

| shape | n | thr | per-gate samples | sample once | ratio |
|---|---|---|---|---|---|
| zipf | 8M | 14 / 7 | 27.4 / 31.1 | **23.2 / 27.1** | 1.18 / 1.15× (loaded run; 1.39× and 1.12–1.19× in two earlier runs) |
| zipf | 1M | 14 / 7 | 12.8 / 11.1 | **12.0 / 10.4** | 1.07× (1.47–1.52× in the first run) |
| near_sorted | 1M | 14 / 7 | 7.7 / 6.5 | **6.4 / 5.5** | 1.22 / 1.17× |
| jitter_ms | 1M | 14 / 7 | 3.3 / 2.9 | **2.7 / 2.6** | 1.20 / 1.11× |
| uniform | 1M | 14 / 7 | 3.5 / 4.5 | **3.2 / 3.8** | 1.08 / 1.17× |
| int_wide | 1M | 14 / 7 | 2.9 / 3.0 | **2.5 / 2.7** | 1.12 / 1.09× |
| heavy_tail, collatz, sawtooth, quantized, mod_card | 1M | | | | 1.03 to 1.31× at 14 threads |
| all shapes | 8M | | | | 0.92 to 1.18×, inside the loaded-run band |
| few_distinct | 8M | 14 | 7.35 / 6.42 min, 9.04 / 8.05 median (30 alternating reps) | 6.74 / 7.03 min, 8.73 / 7.93 median | noise; the 0.78–0.87 in the 5-rep tables was load |


## (unreleased) — histogram-free partition with a uniformity certificate (`make hfbench`); the histogram path is `-DLOGOS_NO_HISTFREE`
- The idea: the histogram pass exists to learn bucket sizes before the
  scatter so every thread can write to exact offsets. The sample already
  predicts them, so the scatter now writes into sample-sized regions with
  slack and a spill path, and the histogram pass, its per-element
  bucket-index cache and its second read of the data are gone. One read of
  the input instead of two.
- The arithmetic that shaped the design (and corrected a claim made in
  conversation): a DKW bound on a 5376-point sample is 3.7% of n at the
  one-in-a-million level, which is six bucket widths, not 2% of a bucket. The
  right statistic is the spacing between consecutive sample quantiles: a
  bucket's count is n times a Beta(32, m-31) variable, standard deviation
  about 1/sqrt(32) = 18% of the bucket. So regions carry `HF_SLACK` = 0.6
  (0.3 measured within noise), threads reserve `HF_BLOCK` = 256-element blocks
  with one atomic per block through a per-thread write-combining stage (no
  reservation is ever left partly filled, so no compaction), the rare
  overflow goes to a per-thread spill list that is sorted and merged in at
  the leaf, and if spills exceed n/16 (a bucket the sample could not size:
  heavy duplicates) the routine declines and the histogram path runs. The
  region buffer is persistent (1.6n doubles, 102 MB at 8M, page-faulted once).
- **The certificate.** The Kolmogorov-Smirnov distance between the sorted
  sample and the straight line through its extremes costs m subtractions on
  a sample already sorted. Uniform data gives D ~ 1/sqrt(m); a tail, a mode or
  a lattice gives 0.1-0.5. Below `HF_KS_MAX` / sqrt(m) = 2/sqrt(m) the
  partition drops the splitter tables and the per-thread trial and buckets
  by one multiply on an equi-width grid, whose counts on certified-uniform
  data vary by sqrt(n/K), about 0.5%. Attributed separately in the A/B: the
  certificate alone is worth uniform 22.6 -> 20.4 ms, int_wide 22.3 -> 17.5.
  Without it the table/style trial runs once, serially, on three 16k windows
  (which also closes the per-thread mis-pick, B17).
- Measured (four engines interleaved, P and P/2, loaded box, memcpy 4.9 ms;
  FINDINGS 3m): uniform 1.31x / 1.42x, int_wide 1.41x, int_1e6 1.45x / 1.29x,
  jitter_ms 1.24x / 1.53x, logistic 1.20x, clustered 1.14x, heavy_tail and
  bimodal 1.01-1.15x; the shapes that take other routes or decline on spills
  identical within noise. Both paths pass the 685-check harness. Prediction
  (4-6 ms off 19 at 14T on uniform) graded right in size, 6.3 ms off 26.7 on
  a loaded box.

## (unreleased) — the inherited JS-era constants, swept (`make knobbench`)
- The question: the dispatcher's thresholds came from the JS engine and were
  never re-tuned in C. Were they set for a bounded language? Every inherited
  constant is now overridable (`#ifndef`), the two inline probe thresholds
  are named (`SKEW_PROBE_MAX`, `QUANT_PROBE_MAX`), and `knobbench` runs four
  engines with four values of one constant, interleaved, over 19 shapes
  (the standard nine plus integers with span 2n and 8n, collatz, zipf,
  sawtooth, brownian, weyl, jittered lattice, and 2/4/8/64 overlapping
  sorted runs) at 1 thread and 14, best of 3. Fourteen knobs, ~60 engine
  builds, every output verified.
- Three were wrong for C and are changed: the run-merge gate
  `MIN_AVG_RUN_FOR_MERGE` 256 → 4096 (a k-way merge is comparison work with
  a mispredicting branch per element; the AVX2 leaf is cheaper unless the
  runs are very few: sawtooth 553 → 138 ms), a new `MERGE_MAX_RUNS` = 8
  (64 overlapping runs 283 → 116 ms; a 4-way merge still wins, so not 3),
  and the low-cardinality abort cap `LOWCARD_CAP` 1024 → 256 (zipf 1.44×:
  the same "abandoned attempt" defect as the lattice window). The counting
  sort now allocates its own count array when the span exceeds n, which the
  context buffer could not hold (found by a segfault in the sweep).
- Eleven were right, or indistinguishable from right: the counting span
  factor stays 1 for a C reason (a count array above n is random access into
  64+ MB, 2–4× slower than the radix), the quantization probe at 4 (5 and 6
  route integers to the float radix, 3.7× slower), the flash threshold and
  safety ratio, the nearly-sorted ratio, the momentum threshold, the skew
  probe, the typed-leaf minimum, the insertion threshold and the coarse
  cutoff. The JS values were not a bounded-language compromise; where they
  were wrong it was the merge gate, and the merge is expensive in C for a
  reason the ledger already held.
- Bench lessons: the first knobbench run re-used a stale binary for the
  second and third knobs (the make target rebuilt only on source changes;
  fixed with a forced rebuild), and the first column runs first in every
  rep and reads ~3–5% slow; the variant order now rotates per rep.

## (unreleased) — per-route thread caps: relocation and the counting routes use fewer threads (`make smallbench`)
- Question asked: is a single thread ever faster than the parallel call?
  From the scaling report, yes, in a band around 100k-1M elements on the
  structured shapes: block_perm at 316k 1.0 vs 1.4 ms, at 1M 2.1 vs 2.5-3.0;
  and the counting shapes at 1M ran 10 threads where 4 were 1.5-1.8x faster.
  Below 32k the bench resolution (0.1 ms) says nothing. At 100k every
  column runs identical code (the global policy already gives one thread).
- Why: the routes have different parallel cost shapes. The partition route
  wants every thread it can get (3 threads at 316k beat one by 1.3x). The
  relocation route has a serial plan and a spawn, and its parallel form
  lost to the sequential one below ~500k elements per thread (at 3.16M, 6
  threads beat 14 by 1.7x). The counting routes (lowcard, modcard, lattice)
  merge P tables, a cost proportional to P and not n, so at 1M four threads
  beat ten; but at 316k three threads still beat one (the tables are small).
  A single global policy cannot express that, and a global 250k policy was
  measured and rejected (it cost uniform and near_sorted their 3-thread wins
  at 316k).
- Shipped: `RELOC_MIN_PER_THREAD` = 500k (250k measured close at 1M, worse
  at 3.16M) and `COUNT_MIN_PER_THREAD` = 350k with a floor of
  `COUNT_MIN_THREADS` = 3 (swept 150k/250k/350k: 350k best at 3.16M by 1.2x,
  within 10% of 250k at 1M; without the floor quantized lost 22% at 316k).
  The global `MIN_ELEMS_PER_THREAD` = 100k stays. Neither cap binds at 8M
  (8M/500k and 8M/350k both exceed 14 threads), so parbench and the profile
  report are unchanged; the scaling report is regenerated.
- Measured (four engines interleaved, P-thread call, best of 5-40 reps by
  size, FINDINGS 3k): block_perm 1M 1.31x and 3.16M 1.69x, block_overlap 1M
  1.21x, few_distinct 1M 1.68x, quantized 1M 1.28x, mod_card_30k 1M 1.38x and
  3.16M 1.27x; the unaffected routes and every 8M row inside their bands.
  Residual: the block shapes at 316k are still ~0.1 ms slower than one
  thread (parallel-entry setup with 3 threads); not worth a third cap.

## (unreleased) — the structured routes did not scale: near-sorted route redesigned, bounded insertion (`make nsbench`)
- **The finding.** From the profile report (1 thread against 14, same sort,
  same shape): the partition route speeds up 4.6-5.5x, like x86-simd-sort's
  4.0-4.2x, but the structured routes that give logos its single-thread wins
  scaled 1.8x (near_sorted), 2.1-2.2x (block relocation) and 3.7-4.4x
  (lattice), against 6-7x for IPS4o. The one-thread wins come from doing
  LESS WORK (one merge pass, one memcpy, one count); the parallel versions of
  those routes did not keep that. On near_sorted the 14 threads spent ~200 ms
  of thread time against 36 ms for the one-thread algorithm.
- **Two mechanisms, both general.** (1) Chunking by index changes the
  dispatcher's decisions: its gates are ratios of n (integer span <= n,
  runs per n, quantization), and a chunk of near-sorted data has the WHOLE
  value range (its strays come from anywhere), so the chunk leaf took the
  8-pass float radix or bits-coarse at 16-30 ns/elem where the one-thread
  route took counting at 6. (2) The 40-pair inversion probe bounds
  inversions, not displacement: a nearly-sorted 48k bucket whose 1% displaced
  elements travelled ~n/2 made the dispatcher's plain insertion sort
  quadratic (333 ms for 8M at 14 threads) once anything routed such buckets
  to the partition. Fixed by using the momentum insertion, which has an 8n
  move budget and falls through (-DLOGOS_PLAIN_INSERTION restores).
- **New near-sorted route** (band-first; old one under -DLOGOS_NS_OLD): band
  thresholds are window medians AT THE CHUNK BOUNDARIES (sample quantiles
  landed ~n/448 positions off and turned 589k elements into strays); each
  thread makes ONE greedy pass over its chunk keeping an element iff it is
  in band, not below the last kept value, and not above both of its next
  two neighbours (the kept sequence is sorted by construction, no leaf runs
  at all), with poison recovery when a displaced-high element slips in
  (after 16 in-band rejections for "below the maximum" alone, the maximum is
  popped into the strays and the streak rescanned; without it 88% of 8M was
  rejected); the strays (the displaced elements, ~2% of n; decline above
  n/4) are sorted once, through the parallel engine when large, and cut by
  the same thresholds; each band then does one two-way merge into a[]. A
  pre-gate runs the scan on four 4096-windows and declines before spawning.
- Measured (four engines interleaved, 9 shapes, P and P/2), new / shipped:
  near_sorted_int 2.27-2.85x, near_sorted f64 1.42-2.05x, local_shuffle
  1.48-1.50x, near_sorted_f64 (0.1%) 1.03-1.21x, guards 0.93-1.07x. The
  near_sorted row at 14T that IPS4o led (17.5 vs 20.4) is now ~10-14 ms.
- Predictions graded: "8-10 ms at 14T" was right for the integer shape
  (10.3), optimistic for the f64 one (14.2 loaded, 9.6 quiet). Three
  intermediate designs measured and replaced on the way (chunk leaf via
  leaf_f64, via sort_fn, greedy without poison recovery): 0.53-0.76x, then
  0.07x from the insertion hazard. Every one of them was found by the
  per-phase trace, not by reasoning.
- Still open on the same finding: lattice/counting/relocation use static
  per-thread ranges (imbalance 1.3-2x) and relocation has a serial middle.

## (unreleased) — lattice window minimum size (`LATTICE_MIN_SLOTS`, `make latbench`)
- Diagnosing the chaos zoo's collatz gain (profile + a probe on the lattice
  window): the moderate-cardinality path first tries a direct-index lattice
  counter whose window is the 2048-sample's range plus an eighth. Collatz
  stopping times (0..664, 576 distinct) have a tail the sample never sees
  (sample max 413-444), so the map pass met a value outside the window after
  scanning 19-44% of the array, gave up, and the hash path redid the work:
  5-12 ms wasted at 1T, 13-25% of that sort. The Weyl sampler's "win" was a
  narrower window that failed 6.5 ms sooner.
- Fix: tables smaller than LATTICE_MIN_SLOTS (4096) are widened to it, which
  covers tails up to ~4x the sampled range at no cost; larger tables keep the
  eighth. Measured (two engines interleaved, 11 shapes, 1T and 14T): collatz
  1.31x at 1T (42.9 -> 32.8 ms) and 1.40x at 14T (10.1 -> 7.2); every other
  row identical code inside its band. A proportional margin (a full span
  each side) was measured first and rejected: quantized 0.87x at 14T, its
  4096-slot table tripled, zeroed and scanned per thread.
- The general lesson is recorded in the truth table: a sample bounds the
  bulk, never the tail; any path that aborts on an out-of-sample value
  should either spill or size for the tail cheaply.

## (unreleased) — order from chaos: Weyl sampling, low-discrepancy probe, per-mode splitter tables (`make chaosbench`)
Three of the ORDER_FROM_CHAOS.md items built, A/B'd as five engines in one
binary over fifteen shapes (the four standard guards, deterministic chaos,
number theory, low-discrepancy sequences, Benford, Brownian, Zipf, a jittered
lattice, and a sawtooth whose period equals the splitter sample stride), at
1 thread and 14. Shipped:
- **Weyl sample positions** (M5, `SAMPLE_POS`, `weyl_pos`): every sampler
  reads the input at `floor(frac(i*phi)*n)` instead of a fixed stride.
  Measured: sawtooth_stride 14T 546 -> 38 ms (the stride put every sample at
  one phase, the splitters collapsed, one thread sorted 8M); van_der_corput
  14T 177 -> 19 ms (bit-reversal is periodic in powers of two and aliased the
  same way: unpredicted); nothing else moved outside its noise band. (The
  1.18-1.25x first reported on collatz was NOT the sampler seeing the data
  better: both samples saw ~280 distinct values. It was where an abandoned
  lattice attempt happened to give up, see the lattice-window entry above.)
  `-DLOGOS_STRIDE_SAMPLE` restores the stride.
- **Low-discrepancy probe** (M3, `LOWDISC_GAP_RATIO`, `ctx->lowdisc`): before
  the AVX2 leaf, two contiguous 64-element windows are sorted; if every
  window's max/min gap ratio is below 4 (the three-gap theorem gives at most
  ~2.6 for a phi rotation; random data is in the hundreds) the data is a
  permutation of an equi-width grid and goes to flash/coarse, whose fix-up
  is then free. Measured 1T: weyl 99.5 -> 79.6 ms, van_der_corput 100 -> 78
  (predicted 45-60: coarse at 8M costs more than estimated); never fired on
  any other shape. `-DLOGOS_NO_LOWDISC`.
- **Per-mode splitter tables** (M2, "firmament", `bt_lookup_d`, mode 5 in the
  histogram trial): gaps between consecutive splitters larger than ten times
  the median gap are gulfs; each mode gets its own 2048-cell value table over
  its own splitter range; the lookup picks the mode with up to seven
  branch-free compares. Built only when >= 2 modes exist, so single-mode
  shapes are untouched. Measured 14T: bimodal 22.8 -> 19.9 ms (1.15x,
  predicted 1.05-1.08x), level with x86-simd-sort's 19.0 on the one parallel
  row it led; heavy_tail 21.3 -> 22.2 is inside that row's band (20.3-22.2).
  `-DLOGOS_NO_MODE_TABLE`.
Not shipped:
- **Network pivot** (M7, `-DAV_PIVOT_NET`): 64 strided samples sorted by the
  bitonic network as the AVX2 leaf's pivot. 0.94-1.06x at 1T across the zoo:
  noise. Predicted 2-3%. Opt-in.
- **Sample-as-pivot equi-depth leaf** (M1): not built. The profile already
  bounds it: the leaf phase on heavy_tail is 7% above uniform (13.2 vs 12.3
  ms at 14T) and on bimodal below it, so equi-depth classes have a ceiling
  of ~3% of wall. The quantile splitters already make buckets locally flat.
- The zoo found one dispatcher gap worth a later look: sawtooth_stride at 1
  thread is 545 ms on every variant (5376 overlapping runs go to the k-way
  run merge); the parallel path handles it in 38.

## (unreleased) — AVX2 vectorized leaf for sequential sorts (`gen_avx2_leaf.py`, `make simdbench`)
- In-house port of the x86-simd-sort design, AVX2 only (4 doubles per vector,
  this box has no AVX-512): an in-place quicksort whose partition splits each
  vector with one compare, one table-driven permute and two plain stores (the
  permute packs left-going lanes to the front and right-going lanes to the
  back; the same vector is stored at the left cursor and at the right cursor
  minus four, the wrong lanes land in already-consumed space), eight vectors
  loaded per end-choice so the data-dependent "which end next" branch is paid
  once per 32 elements; ranges under 512 use a streaming out-of-place
  partition into a stack buffer with no end-choice at all; base case 32- or
  64-element bitonic networks emitted as straight-line code by
  `gen_avx2_leaf.py` (gcc left the loop form 2.6x slower). Pivot: median of 3
  below 512, of 15 below 64k, of 63 above; equal runs split off with a
  strict-compare partition; depth limit falls back to the scalar introsort;
  declines on NaN. Runtime cpuid gate, `LOGOS_NO_AVX2=1` env, `-DLOGOS_NO_AVX2_LEAF`.
- Where it runs (the difference from the x86-simd-sort hook, which replaces
  every path): only the UNSTRUCTURED leaf, i.e. where sort_fn would run flash,
  coarse or bits-coarse and leaf_f64's skewed branch. Counting, int32 radix,
  quantized radix, run relocation/merge, momentum, gap split are untouched.
- Policy (measured, four variants interleaved in one binary, 1 thread and 14):
  SEQUENTIAL sorts only, and not when sort_fn's inversion probe says nearly
  sorted (that flag now rides in the context so coarse's bucket hook obeys it
  too). 1T: uniform 1.20x, heavy_tail 1.48x, bimodal 1.59x, clustered 1.19x,
  negatives 1.26x; int_wide, quantized, few_distinct, blocks, near_sorted
  identical code within noise. In the PARALLEL leaf (33k-element L2-resident
  buckets, 14 threads on a hybrid P+E CPU) the same code lost 5-28% on every
  random shape and 10-16% at 6 threads, so `-DLOGOS_AVX2_PARALLEL` is opt-in
  and measured negative here. Before the nearly-sorted gate, near_sorted 1T
  lost 1.23x: coarse's scatter on nearly-sorted data is near-sequential.
- Engineering record (probe): the first cut was 2.3x SLOWER than the scalar
  leaf (268 vs 117 ms at 8M): one vector per end-choice mispredicted every
  four elements (1.41 ns/elem per partition pass) and the networks did not
  unroll. Unrolling by 8 and plain stores took a pass to 0.39 ns/elem, the
  straight-line networks 1.9 ns/elem, the streaming small partition 0.71 vs
  1.23 ns/elem on 256-element ranges. A 128-element network measured 3.0
  ns/elem: one more partition level plus the 64 network is cheaper. Whole
  8M at 1T: 92-104 ms against x86-simd-sort's 79 and the scalar leaf's 117-126.
- Prediction graded: "1.15-1.2x at 14T on random shapes, 85-95 ms at 1T" was
  wrong on the first (lost) and right on the second.

## (unreleased) — model splitter table (H0c, Learned-Sort style): measured neutral, OPT-IN (`-DLOGOS_MODEL_TABLE`)
- One level of 4096 value cells; a cell crossed by several splitters stores a
  linear model of bucket index against value through its first and last
  splitter; the lookup predicts with one multiply and corrects against the
  real splitters, two branch-free compares when the build-time worst error
  is one bucket, a loop otherwise. Enters the histogram's per-thread trial as
  a third candidate beside the value and key tables.
- Measured twice at 14 threads (`make stbench`, three engines interleaved):
  model vs value+key 0.93-1.09x on the random shapes across the two runs,
  7-thread rows 0.97-1.03x: noise. First run heavy_tail -12% because a model
  that can miss by 2+ buckets loops in its correction even as a trial chunk;
  gated to merr <= 1, the loss went away and nothing else appeared.
  Prediction ("bimodal histogram 7.3 -> 5.5 ms") wrong: the remaining
  histogram cost on bimodal is not the child-table dereference.

## (unreleased) — textbook baselines in every bench: LSD radix sort and timsort (`bench_sorts.h`)
- `radix_sort_f64`: LSD radix over the IEEE-754 order-preserving key, 11-bit
  digits, six passes, all six histograms taken in one pass, a pass skipped when
  every key shares that digit. `timsort_f64`: Tim Peters' listsort as in
  CPython (minrun 32-64, natural runs with strictly-descending reversal,
  binary insertion to extend short runs, the corrected run-stack invariant,
  galloping merges with adaptive min_gallop). Header-only C, included by the
  C++ benches. 360 checks against qsort (12 shapes x 15 sizes) before wiring.
- They appear in the SINGLE-THREADED section of parbench, the 1-thread cells
  of profbench (bars, summary columns, card headers) and the 1-thread panels
  of scalebench (two more series and table columns). They are sequential
  algorithms, so by the fair-fight rule they never appear beside the parallel
  sorts.
- Measured (parbench-full, 14 threads, quiet calibration: memcpy 4.1 ms,
  std::sort-1M 64 ms; FINDINGS 1d): 1T logos beats the LSD radix on 8 of 9
  shapes (1.2x heavy_tail ... 11x block shapes) and loses bimodal 0.87x;
  timsort is 0.55-0.77x of std::sort on random data and 12x on the block
  shapes, where J5's relocation is still 1.3-1.6x faster than its merges. The
  same quiet run puts logos at parity with x86-simd-sort AVX2 on random shapes
  at 14T (0.86-1.08x) and 1.4-2.5x ahead on structured ones; IPS4o takes
  near_sorted and few_distinct by 6-17% at this count.

## (unreleased) — benchmarks default to the physical core count (`BENCH_THREADS`, 14 here)
- `bench_common.h`: `bench_threads(online)` = LOGOS_THREADS env, else
  `-DLOGOS_BENCH_THREADS` (Makefile `BENCH_THREADS ?= 14`), else the physical
  core count from /sys topology, else online CPUs. parbench, profbench,
  scalebench, pfbench, j5bench, stbench, leafbench and theorybench all use it.
  WSL2 shows a fictional 10x2 topology for this 6P+8E CPU, which is why the
  Makefile value exists. The engine's default (`logos_num_cpus`) is unchanged.
- Reason: the noise probe put the lowest and quietest minimum at 14 threads;
  the goal from here is clean margins, not peak numbers. Reports regenerated
  at 14 threads; opponents run at the same count, so fights stay fair.

## (unreleased) — noise probe (`make noiseprobe`): the measurement, measured
- `noiseprobe.c` repeats one 8M uniform sort at 1, 2, 4 ... 20 threads (12 reps
  by default) and prints min / median / max, max/min, median/min, the
  single-thread preempted share (1 - cpu/wall) and the parallel CPU
  utilisation (cpu / (wall * P)).
- On the reference box under its session load (VS Code's software-WebGL GPU
  process ~44%, OneDrive sync ~20%): memcpy 64 MB median/min 1.14 with one
  110 ms outlier (24x); logos 1T min 252 / median 271 / max 393 ms with a
  preempted share of 0.0 -- the variance is sibling-hyperthread and cache
  contention, which CPU accounting cannot see; parallel utilisation falls
  from 0.99 at 2 threads to 0.62 at 20, and max/min grows from 1.4 to 2.7;
  the best minimum is at 14 threads (35.0 ms) versus 37.7 at 20.
- Consequences applied: parbench and profbench now take the minimum of 5
  instead of 3 (the interleaved A/Bs already used 5); CLAUDE.md methodology
  rule 10 records the probe and the thread-count advice. Not applied (the
  user's call): stopping the two hogs, which is worth more than everything
  else combined.
- Engine note for later: the histogram's trial chunks are timed, so a
  straggler during a trial can pick the wrong table or leaf style; on a noisy
  box an A-B-A-B trial taking the min per variant would be cheap insurance.

## (unreleased) — thread-count policy: at most one thread per 100k elements
The scaling benchmark's first run showed the parallel route's fixed cost
(thread creation, barriers, P x 12 tiny buckets; ~5-10 ms on this loaded box):
with 20 threads for any n >= 65536, logos was SLOWER than its own single-thread
route from 1e5 to ~5e5 elements (uniform 1e5: 8.8 ms parallel vs 2.2 ms single;
few_distinct 1e4: 4.3 vs 0.1; block_perm 1e5: 9.9 vs 0.4). `effective_threads`
now caps P at n / MIN_ELEMS_PER_THREAD (100000) in both entry points; results
never depend on the thread count, so this is pure policy.
- Measured (`LOGOS_MAX_N=3.2e6 make scale-full`, 20-thread column, before ->
  after): uniform 1e5 8.8 -> 1.6 ms, 3.16e5 12.9 -> 3.0, 1e6 14.0 -> 4.3 (ten
  threads now beat twenty), 3.16e6 24.9 -> 14.1; heavy_tail 1e5 4.1 -> 2.5,
  1e6 10.3 -> 4.7; bimodal 1e6 14.1 -> 5.1; int_wide 1e6 5.8 -> 4.6;
  block_perm 1e5 9.9 -> 0.5, 1e6 10.1 -> 2.1. Sizes >= 2M take all threads as
  before. Cross-run comparison on a loaded box, but the effect is 3-20x on the
  small rows and the 1T column in the same run is the floor it now matches.
- Prediction graded: "1e5 rows to their 1T time, 3e5 halves, 1e6 within noise"
  -- right on the first two, low on the third (fewer threads beat more at 1e6).
- `-DLOGOS_NO_THREAD_POLICY` restores the old behaviour; `MIN_ELEMS_PER_THREAD`
  is overridable. 685 checks, 0 failures.

## (unreleased) — scaling benchmark with every contender: `make scale-full` -> scale-report.html
- `scalebench.cpp`: sizes 1e4, 3e4, ... 3e7 (LOGOS_MAX_N), nine shapes, two sections that never
  mix thread counts: std::sort, logos, x86-simd-sort, IPS4o sequential at 1 thread; __gnu_parallel::sort,
  logos, x86-simd-sort OpenMP, IPS4o parallel at P. std::sort's output is the reference and every
  other output is checked equal to it. Reps 5 / 3 / 1 by size, minimum reported, per-sort budget
  (LOGOS_MAX_SEC) stops a shape early. The report is one log-log line chart per shape and section
  (n on x, ms on y, one line per sort, direct end labels, tooltips, a table view with logos vs the
  best other), plus the calibration line; copied to reports/. `make scalebench` builds it without
  the opponents. `tools/check_report.js` now checks this report too. Closes the truth table's A13
  ("does the win/loss pattern hold at sizes other than 8M?").
- Charts use linear axes by default (a toggle gives log-log), draw dashed O(n) and O(n log n)
  reference curves anchored at logos's largest measured point, and print each series' empirical
  exponent (least-squares slope of log t vs log n over n >= 1e5, where fixed costs no longer
  dominate) in the label and the table. Single-threaded, the four sorts fit n^1.03-1.19 on the
  random shapes, as n log n should over this range; the parallel exponents are lower because the
  parallel fixed cost is a large share of the measured range.

## (unreleased) — table-leaf branch policy chosen per thread from the trial (supersedes the package note below)
Three interleaved A/B runs (`make theorybench`) split the branch-free leaf by
shape with a mechanism: flat data resolves nearly every element at the table's
root, so the old predictable branch there is free and the branch-free form pays
an extra load (int shapes -5 to -12%); skewed data resolves a mix at the root,
so that branch mispredicts and branch-free wins (heavy_tail up to +50%,
bimodal +20%). A fixed hybrid (branchy root, branch-free children) lost on
heavy_tail. Shipped: the histogram's trial chunks, which already time the value
and key tables, now also count how many lookups resolve at the root; the
committed loop keeps the predictable root branch only when that fraction is
near 0 or 1 and goes branch-free otherwise (`do_hist`, `bt_lookup_*`,
`style`). Two decisions from one trial, no extra pass. `-DLOGOS_LOOKUP_BRANCHY`
(the original two-branch form) and `-DLOGOS_LOOKUP_BRANCHLESS` remain for A/Bs.
- Measured vs the pre-package engine (4 buckets/thread, two-branch leaf), 20T /
  10T: uniform 1.37x / 1.30x, heavy_tail 1.13 / 1.32, bimodal 1.01 / 1.19,
  int_wide 1.31 / 1.17, clustered 1.02 / 1.28; never-partition rows in noise.
- Honest limit: the run that compared adaptive against always-branch-free hit a
  load burst (identical-code rows spread +-25%), so that comparison is
  unresolved here; adaptive is chosen because it is the better of the two by
  construction and never measured worse than the old engine. Interleaved
  counters and line repair were mixed across all three runs and stay opt-in.
- Bucket count: 12 per thread confirmed in all three runs (1.08-1.44x on 9 of
  12 partitioning rows on its own).

## (unreleased) — SOTA opponents in the profiler; first fair fight on AVX2 hardware
- `profbench` runs x86-simd-sort (`-DHAVE_XSS`) and IPS4o (`-DHAVE_IPS4O`)
  beside every cell at the cell's thread count (`make profile-full`, like
  `parbench-full`); the report's fair-fight bars, summary and card headers
  carry both, labelled with the SIMD kernel ISA of the build (AVX-512 or AVX2).
- Fairness fix in both benches: the single-threaded x86-simd-sort column ran
  with OpenMP threads set to P (its OpenMP build parallelises regardless of
  the "1T" label). Both benches now set OpenMP threads to 1 for the
  single-threaded section and to P for the parallel one.
- The opponents are cloned at `engines/x86-simd-sort` and `engines/ips4o`
  (the Makefile's default XSS / IPS4O paths); TBB from `libtbb-dev`.
- First measurement on AVX2-only hardware (i9-13900H, loaded ~2.5x): at 20
  threads logos beats x86-simd-sort's AVX2 kernels on 7 of 9 shapes (uniform
  1.16x, int_wide 1.24x, quantized 1.5x, blocks 2.3-3x), ties heavy_tail,
  loses bimodal 0.59x; IPS4o loses to logos on every row. **Corrected by the
  quiet 14-thread run (FINDINGS 1d): those margins were the load. Quiet, it
  is parity on random shapes (0.86-1.08x), 1.4-2.5x on structured ones, and
  IPS4o takes near_sorted and few_distinct.** Single-threaded the
  SIMD sort wins the three random shapes ~2x and few_distinct 3.5x. logos +
  AVX2 leaf per bucket now beats plain logos on the random shapes (24.2 vs
  26.4 uniform, 28.1 vs 36.7 heavy_tail, 26.4 vs 41.3 bimodal), reversing the
  AVX-512-era "worse on 5 of 7"; not made default, AVX-512 unmeasured. Full
  table: FINDINGS.md section 1c.

## (unreleased) — the rest of the theory, A/B'd as a package (`make theorybench`)
Four changes on the parallel route, each behind its own flag, measured as six
interleaved variants on 8M at 20 and 10 threads, every output verified:
- **12 buckets per thread** (was 4; `BUCKETS_PER_THREAD`, capped so P x BPT <=
  256 to keep the one-byte bucket cache). The bucket queue already balances
  dynamically, so more buckets shrink the tail; and a ~33k-element leaf's flash
  working set stays in L2 where a 100k one spilled. The largest single effect.
- **Branch-free table leaf** (`-DLOGOS_NO_LOOKUP_BRANCHLESS` restores the
  two-branch form): cells with zero or one splitter share one compare-and-add
  path; only the rare child descent branches. Children are now sized to the
  splitters they separate (4 cells per splitter, 64-256), which is what
  removed 12/t's loss on bimodal in the first run (240 splitters put 120 in a
  cluster cell; a 64-cell child went two levels deeper).
- **Interleaved histogram counters** (`-DLOGOS_NO_HIST_INTERLEAVE`): two count
  rows, even/odd elements, merged at chunk end.
- **Line repair after flash** (`-DLOGOS_NO_LINE_REPAIR`): a branch-free 19-
  compare-exchange network sorts every aligned line of eight, then an insertion
  pass whose "already in place" test is now true for ~98% of elements, so it
  predicts instead of mispredicting on ~18%.
- **Measured** (second run, calibration ~2x off quiet; the identical-code guard
  rows spread +-12% at 30 ms and +-25% at 15 ms, which is the noise band):
  the package vs the previous defaults, 20T / 10T -- uniform **1.28x / 1.15x**,
  heavy_tail **1.24 / 1.24**, bimodal **1.47 / 1.24**, int_wide 0.98 / 1.08,
  int_1e6 **1.16 / 1.42**, clustered 1.02 / 1.28; guards near_sorted 1.17 /
  1.11, quantized 0.98 / 1.09, block_perm 0.99 / 1.04. Never below 0.98x.
  Bucket count alone reproduces most of it (1.08-1.44x on 9 of 12 rows).
- **What this run could not do:** attribute the remainder among the three
  smaller changes; their single-feature columns move inside the noise band on
  this loaded box. They ship as part of the measured package; the flags exist
  so the attribution can be done on a quiet machine (`make theorybench`).
- **Not shipped:** bucket-relative digit narrowing for the int32 leaf (H5).
  Mixed in its first A/B (int_wide +21% at 20T, -6% at 10T, int_1e6 -16%)
  because it adds a min/max pass the fused byte histogram never paid. Kept
  opt-in as `-DLOGOS_INT_NARROW`; the fix is to pass the bucket's range down
  from the splitters instead of rescanning.
- Predictions graded: bucket count "3-6%" was far too low (it is a cache
  effect, not only balance); branch-free leaf "+20% bimodal" right in
  direction, unresolved in size; interleave "0-4%" unresolved; line repair
  "5-10% of leaf" unresolved. 685 checks, 0 failures throughout.

## (unreleased) — leaf-phase knobs measured and REJECTED (`make leafbench`)
After H0b the leaf sorts are the largest parallel phase (40-56% of wall on
unstructured shapes). On a ~100k-element bucket the leaf is flash sort: one
equi-width classification with m = n classes, a scatter, one insertion pass
over the buffer (Devroye's bucket model: fix-up cost ~ n * integral of f^2 over
the bucket). Two one-constant variants, four engines interleaved in one binary,
8M, best of 5, 20 and 10 threads:
- **m = n/4** (`-DFLASH_M_DIV=4`, counters L2-resident): int_wide +25%, uniform
  +4% at 20T, but clustered / negatives / heavy_tail 10-20% SLOWER. Predicted
  +3-6% everywhere: wrong. Fewer classes means more fix-up, and the fix-up is
  the cost, not the counters.
- **coarse instead of flash for buckets** (`-DCOARSE_MIN_N=16384`): bimodal
  +12% at 20T, int_wide +21% at 10T, everything else 10-45% slower (B4 again:
  a second cache-resident level costs more than it saves).
- m = n/16 lost everywhere, as predicted.
Nothing ships; both constants stay `#ifndef`-guarded for later A/Bs and
`leafbench.c` is the template. What the negative says: the leaf's cost is the
comparison fix-up plus its own probe passes (prescan, run detection, gap
sample) on data the histogram already characterised. The next levers are
density-adaptive (equi-depth) classes gated by Devroye's integral-of-f^2
estimate from the bucket sample, a branchless fix-up, and dropping the
per-bucket probes (J2). See TRUTH_TABLE.md H6/K5.

## (unreleased) — direct-index splitter tables for the histogram (H0b): the parallel path's bottleneck, removed
The profiler's first finding was that `bucket_of`'s binary search over the
sample splitters cost ~100 ns/element of CPU at 20 threads, 49-70% of every
unstructured sort's wall. It is replaced by two O(1) tables that give the same
bucket id: cells over the VALUE range (flat data resolves in one level) and
cells over the order-preserving KEY (log-magnitude cells; skewed data resolves
shallow). A cell crossed by one splitter resolves with one compare; a cell
crossed by several gets a child table over the span of those splitters. Cell
bounds are widened by a relative 1e-13 before consulting the splitters, so
rounding can only make an entry more conservative, never wrong: every
"resolved" answer is exact and every other case ends in a compare against a
real splitter. Which table is faster depends on the data and on the branch
predictor, so **each histogram thread times both on its first two chunks and
commits to the winner** -- the "algorithm samples itself" pattern from
experiments/paradigm/SELF_SAMPLING.md. Build cost ~0.3 ms; falls back to the
search if a table cannot be built; off with `-DLOGOS_NO_SPLITTER_TABLE`.
- **Prediction (written first):** uniform / heavy_tail / bimodal / int_wide /
  clustered 1.4-1.9x end to end at 20T; near_sorted / few_distinct /
  quantized / block_perm within +-3% (they never reach the partition); 1T
  identical.
- **Measured** (`make stbench`, interleaved in one binary, best of 5, 8M,
  calibration ~2x off quiet): at 20 threads uniform **1.98x**, heavy_tail
  **2.03x**, bimodal **1.58x**, int_wide **2.99x**, clustered **1.89x**,
  negatives **2.13x**; at 10 threads 1.21-2.63x. Guard rows (identical code)
  moved 0.88-1.20x at 11-21 ms, which is this loaded box's noise band at those
  times. Prediction graded: right on direction and on the guards, low on size
  -- the histogram was a larger share than its wall suggested because the
  threads it stalled were also idle.
- Correctness: 685 harness checks at threads 1/2/4/8, every A/B and parbench
  output verified sorted and permutation-exact. The tables are exact by
  construction (conservative bounds + real-splitter compares), not by testing.
- `make stbench` is the A/B; `make profile` shows the histogram row after.

## (unreleased) — per-phase profiler (`make profile`) and block shapes in parbench
- `-DLOGOS_PROFILE` builds record, per `logos_sort_f64` call: the top-level
  route, wall time per phase, and per thread its working time, the CPU time it
  actually received (so preemption by other processes is separated from engine
  cost), time blocked at barriers, and the leaf path each bucket took
  (`logos_profile_get`, `logos_psort.h`). Zero cost when not compiled in; the
  normal library object is untouched.
- `make profile` runs `profbench` (the parbench shapes plus mod_card_30k,
  block_perm and block_overlap, at 1 thread and at P, every output verified
  against std::sort, best of 3) and writes `profile-report.html`: per cell a
  treemap of thread-time (phases, leaf sorts split by path, preempted, stalled
  at barriers, idle), per-thread stacked bars, and a phase table with CPU
  utilisation and streaming rate against the memcpy calibration. The
  calibration line sits at the top of the page.
- parbench: `block_perm` and `block_overlap` rows (J5's target shapes) in both
  sections. This run (loaded box, calibration ~2x off quiet): block_perm 1T
  19.2x vs std::sort, 20T 6.1x vs __gnu_parallel::sort; block_overlap 18.5x /
  4.6x.
- **First finding, and it corrects the cost model.** At 20 threads the
  top-level histogram is the largest phase on every unstructured shape (63% of
  uniform's wall, 49-70% across heavy_tail / bimodal / int_wide), and it is CPU
  work, not memory and not preemption: 836 thread-ms of CPU for 8M elements
  (~100 ns/element on this loaded box) against 88 for the scatter and 313 for
  all leaf sorts together. The cause is `bucket_of`'s dependent-load binary
  search over the 79 sample splitters; 3.4.0's bucket-index cache moved that
  search out of the scatter into the histogram, it did not remove it. Per
  element the histogram now costs more than the whole 1-thread coarse sort
  (~29 ns/element), which is why 20 threads buy only ~3x over one. Prediction
  for the fix, written before building it: a direct-index table over the
  splitters (one multiply and one load in the common case, a short search only
  in cells that straddle a splitter) takes histogram CPU from ~100 to ~10
  ns/element, uniform 20T from 78 to ~40 ms on this box, and every unstructured
  MULTI row of parbench up 1.5-2x. Not implemented yet; A/B it with the
  pfbench pattern.
- Fix: the report's script referenced a `const` helper before its declaration
  (temporal dead zone), so the page showed headings and no data. The helper is
  now declared first; `tools/check_report.js` executes the page's script under
  a stub DOM and fails unless every cell renders, and `make profile` runs it
  when node is available.
- The report also carries the fair fight on the same data: std::sort beside
  every 1-thread cell, __gnu_parallel::sort beside every P-thread cell (and
  x86-simd-sort's OpenMP sort with `make profile-simd XSS=...`), all verified,
  as a grouped bar chart and as columns in the summary. parbench remains the
  place for the full opponent set; the profile shows *why* each row wins or
  loses.
- Crux for the fix (`experiments/paradigm/splitter_crux.cpp`, one thread, ids
  verified identical): value-space table 2.7–5.1 ns/element on flat shapes,
  key-space table 11.6–17.5 on skewed ones, and a pass that times both on 1/32
  of the data then commits is within 10% of the best on every shape, 3–10×
  over the search. Building the table from first-half hit counts gains nothing.
  Design note on the pattern: `experiments/paradigm/SELF_SAMPLING.md`.
- Methodology: this is the profile the truth table called H0. Three sessions of
  A/B-only tuning never saw the histogram because nothing broke the sort down
  by phase; the trace macro found the J5 gate bug the same day. Profile before
  guessing.

## (unreleased) — disjoint-run relocation (J5): block-permuted data without a merge
Sorted blocks concatenated out of order (merged log partitions, time-series
shards) are runs whose VALUE ranges mostly do not overlap. Merging them is
comparison work over every element for nothing. New path: sort the run
descriptors by first value, group runs whose ranges overlap into chains, memcpy
every run to its chain-ordered place, merge only inside multi-run chains. Fully
disjoint runs cost one copy pass and zero comparisons.
- Where: a chunk-parallel top-level probe in `psort_f64` (`relocate_try_f64`,
  after the near-sorted gate, both the sequential and the parallel branch) and a
  hook in `sort_fn` between run detection and `merge_all_runs`. Off with
  `-DLOGOS_NO_RELOCATE`. Gate: relocate only when the elements inside multi-run
  chains are <= n/2; interleaved runs stay on the galloping run-merge.
- **Prediction (written first):** 8M, 64 disjoint blocks: 1T 2.5-4x, 20T 1.5-2.5x;
  interleaved / near-sorted / uniform within +-3%; risk row `few_runs`.
  **Measured** (`make j5bench`, interleaved in one binary, best of 5, calibration
  ~1.8x off quiet so margins are loose): block_perm64 **2.82x / 2.03x**,
  block_perm1k **3.39x / 2.18x**, block_overlap (a quarter of the data in 2-run
  chains) **2.34x / 1.89x**; segmented 0.98/0.95, few_runs 0.99/1.03,
  near_sorted 0.97/1.05, uniform 0.96/0.98 (1T/20T). Prediction graded: right
  on the effect and on the risk row.
- Two wrong turns, kept as measured negatives:
  1. **Interval-overlap gate on raw runs rejects every block-permuted input.**
     Two shuffled blocks that happen to meet in ascending order fuse into one
     run whose value interval hides a hole; a third block belonging in the hole
     then reads as an overlap. Half of all block boundaries are ascending, so
     the first cut never fired (A/B: 0.99x, i.e. the old path plus a wasted
     probe). Fix: close a run at any jump > 1024x its mean step so far
     (`reloc_gap_split`), in the probe and when stitching chunks.
  2. **A sequential probe plus spawn-then-bail costs the parallel path.** The
     first probe read the whole array on one thread (~7 ms of a 19 ms sort at
     20T), and spawning 19 threads that bail at once cost ~8 ms on this loaded
     WSL2 box (segmented 20T 0.84x). Fixes: chunk-parallel detection with a
     per-thread prefix gate, the caller's thread scanning chunk 0 before any
     spawn, and a 1024-sample pre-gate that decides "few long interleaved runs"
     (few_runs went from 0.87x to 0.99x) without a read pass.
- Harness: shapes `block_perm` and `block_overlap` (fractional values so the
  quantized-radix probe does not claim them); 685 checks, 0 failures at
  threads 1/2/4/8. `j5bench.c` is the A/B (pfbench pattern, with a calibration
  line). Makefile: `RENAME` macro renames every public symbol for multi-variant
  binaries (fixes `make pfbench`, which broke when the engine gained
  `logos_set_simd_*` and the in-place entry points).
- `-DLOGOS_TRACE` prints per-phase timestamps to stderr (prescan, gates,
  histogram, scatter, buckets, relocation) — the seed of the per-phase profile
  the engine never had; it is how wrong turn 1 was found.

## (unreleased) — IPS4o added to the fair fight
- `make parbench-full` adds **IPS4o** (In-place Parallel Super-scalar Samplesort,
  the state-of-the-art parallel *comparison* sort) to parbench's MULTI-THREADED
  section, alongside gnu_par, logos, logos+SIMD, and x86-simd-sort MT -- all
  parallel, compared like to like. Needs x86-simd-sort + IPS4o headers + TBB;
  guarded by -DHAVE_IPS4O so plain `make parbench` is unaffected. Verified
  correct; MIMD-scalar SOTA opponent now covered. (IPS4o carries thread-pool
  startup cost, so it is not meant for single-threaded runs -- read it in the
  MULTI section on real cores.)

## (unreleased) — SIMD in logos's OWN hot loops: measured, mostly rejected
Two experiments (vectorize the arithmetic in logos's winning paths, and the
scatter's index computation), each A/B'd end-to-end, not just in a microbench:
- **#2 bucket-index (value->slot) in coarse bucketing** vectorizes 1.47x in
  isolation but is **net-negative end-to-end (-3% to -5.5%)** and was reverted:
  it must fuse with a scalar scatter-count, so the vectorized-store-then-scalar-
  reload costs more than the SIMD saves. A textbook memory-wall result.
- **#1 OR/AND byte-skip reduction** vectorizes 1.22x and integrates cleanly
  (standalone pass), but is **~0.1% of a memory-bound sort**, so the end-to-end
  effect is lost in noise. Kept as an opt-in kernel (logos_enable_simd_kernels,
  logos_simd.cpp), unwired by default; the core stays scalar and portable.
- Net finding: **SIMD does not help logos's hot loops** because they are memory-
  bound, not compute-bound. The kernels (logos_simd.cpp) are shipped as a
  documented, reproducible negative result. SIMD's real place remains the leaf
  (x86-simd-sort), and the strongest integration is a dispatcher, not a kernel.

## (unreleased) — SIMD-leaf hook (the "state-of-the-art pivot")
- `logos_set_simd_sort()` registers an optional vectorized leaf; `logos_simd.cpp`
  wires Intel x86-simd-sort (AVX-512, `-march=native -DHAVE_XSS`). The large-n
  fallback and the parallel per-bucket sort then use it -> parallel-SIMD (logos
  dispatch + AVX-512 quicksort per bucket). Core stays dependency-free (NULL by
  default). Verified correct sequentially and at threads=4.
- Measured single-thread: the leaf closes uniform from 0.39x to 0.86x of
  x86-simd-sort (parity, not beyond). See PARITY.md. `make parbench-simd` +
  `LOGOS_SIMD_LEAF=1` runs the parallel-SIMD vs single-threaded-SIMD fight on a
  many-core box.

## (unreleased) — measured against the SIMD state of the art
- Added an **x86-simd-sort (AVX-512)** column to parbench (`make parbench-simd`,
  needs the header-only lib + an AVX-512 CPU; guarded by -DHAVE_XSS so plain
  parbench is unchanged). Result, single-threaded 8M: logos-sort **loses on
  every shape, 1.1x-3.8x** — worst on comparison-friendly random data, closest
  on the structured shapes it targets (near-sorted 0.90x, quantized 0.86x).
- README and PARITY.md updated to state this plainly: logos-sort beats scalar
  and OpenMP parallel sorts but is beaten by vectorized SIMD sorts on one core;
  its surviving edges are thread scaling and SIMD-unfriendly data shapes.

## (unreleased) — parity extras (argSort family, findOutliers, strings)
- New `logos_extra.c`: **argSort** (`logos_argsort_f64/i32`, stable index-carrying
  radix, input unmodified), **apply/inverse permutation**, **findOutliers**
  (`logos_find_outliers_f64`), and a **multikey-quicksort string sort**
  (`logos_sort_strings`). These bring C to near-full API parity with JS — the
  argSort + apply-permutation pair is what turns "a fast numeric sort" into
  "sort records by a numeric key." Verified in the harness (7 checks) and a
  standalone suite (22 checks incl. stability, permutation validity, inverse
  round-trip, planted outliers, empty/prefix/duplicate strings).
- Added **PARITY.md**: an honest JS/C/Rust feature matrix. Not ported to C:
  the JS string engine's fast paths (LCP skip, packed-prefix radix, string
  run-merge) and `sortWithProxy` — marked as gaps, not shipped unverified.
- Rust (v0.3.x) gained the same extras (argsort/apply/inverse/outliers/strings);
  its numeric core remains v2 (see PARITY.md).


## (unreleased) — strict in-place mode + richer scaling test
- **Strict in-place variants** `logos_sort_f64_inplace` / `logos_sort_i32_inplace`:
  cycle-following (American-flag) MSD radix with a one-pass OR/AND byte-skip and
  an order-preserving key (int32 sign-bias; double IEEE total-order). Auxiliary
  memory O(256*bytes) of stack — no n-sized buffer — so peak footprint ~1n vs
  the buffered paths' ~2n, roughly halving the RAM ceiling. Measured cost
  (8M, 1 core, vs buffered): f64 2.9-3.4x, int32 1.6x full-range / 6x byte-range
  slower — random-access swaps vs sequential scatter. Sequential for now
  (parallel-in-place = sort the 256 top buckets across threads, still stack-only,
  is the natural next step). Verified vs qsort across shapes/sizes/types.
- **Scaling test** (`make scaling`) extended: sizes now 10,30,100,300,1000,3000
  ... up to 1e9; microbench timing (each size sorted repeatedly to >=120ms
  accumulated, MINIMUM reported) so tiny sizes get real resolution; THREE
  contenders on identical data (buffered ~2n, in-place ~1n, gnu_par); the
  in-place contender keeps climbing after the ~3n buffered/gnu_par ones are
  RAM-capped. Dashboard shows all three throughput curves.

## (superseded) — scaling test + dashboard
- `make scaling` (now C++/OpenMP): sorts powers of ten from 1e4 toward 1e9 with
  a same-data `__gnu_parallel::sort` baseline on every size (gnu_par on a copy),
  and writes an HTML dashboard `scaling-report.html` (throughput chart, speedup
  table, summary cards) in addition to the terminal table. Both sorts are
  verified sorted, permutation-exact, and equal to each other before timing is
  trusted. `scaling-report-sample.html` is an example run.
- (superseded) earlier `make scaling`: sorts powers of ten from 1e4 up to 1e9 elements, stopping at
  whichever comes first — the 1e9 ceiling, a 30s/sort wall-time budget (a size
  runs only if the previous one finished under ~60% of budget), or available
  RAM (skips any size whose input+scratch+reference would exceed 85% of free
  memory). Every size that runs is verified sorted AND permutation-exact
  (XOR-of-bits checksum). Overrides: LOGOS_MAX_N, LOGOS_MAX_SEC, LOGOS_THREADS.
- Engine unchanged (still 3.6.0).


## (unreleased) — two experiments tried and REJECTED by their A/B
Kept here as measured negative results; the 3.6.0 engine is unchanged.
- **Pooling the per-thread contexts** (not just the scatter buffer): rejected.
  One shared, resizable context pool cannot serve call sites with different
  size needs (distribution leaf ~maxb, near-sorted chunk-sort ~chunk) without
  thrashing; interleaved A/B showed near-sorted 167 -> 237 ms, and sizing the
  near-sorted contexts down to fit the pool segfaulted (a skewed chunk's bucket
  can exceed chunk/BUCKETS, and the coarse idx/land arrays are indexed by
  position within the bucket). Not worth a per-call-site pool for the gain.
- **Winner (tournament) tree band-merge** replacing the binary heap in the
  near-sorted path: rejected. Measured 1.4-1.9x SLOWER. Reason: over sorted-ish
  data each value band draws from ~1 chunk, so the merge usually has ONE live
  run and the heap pops in O(1); the winner tree replays a full log2(K)-deep
  path every element regardless. The heap's best case is the common case here.
- Lesson reinforced: the merge kernel is not the near-sorted bottleneck, and a
  shared context pool needs per-site sizing. Both would have shipped on
  reasoning alone; the interleaved A/B (each engine measured with its own pool,
  no cross-contamination) caught both.


## 3.6.0 — four floor experiments (each gated by an interleaved A/B)
- **A. Ping-pong buffering.** coarse, bits-coarse and flash return WHICH buffer
  holds the result and recurse with buffers swapped; consecutive levels cancel
  instead of copying back, and a level copies only a minority when children
  land in different buffers. The top level no longer copies back unless the
  result did not land in a[]. Removes the ~37% of memory traffic that was pure
  copying. One-core A/B: neutral (copies are sequential streams there); the
  payoff is the bandwidth-bound many-core regime, like write-combining and
  prefetch before it. Never worse by construction.
- **B. Lattice (direct-index) counting.** If the sample shows every value is an
  integer multiple of one step, slot = round((v-base)/step): counting with no
  hashing and no scatter, bit-exact (each slot keeps the exact key; a second
  distinct double on one lattice point aborts to the hash path). Sequential
  8M: quantized 109 -> 52 ms, 30k-distinct 105 -> 53 ms — on the two-pass floor
  next to few_distinct.
- **C. Persistent scratch pool** (+ `logos_release_buffers()`): the largest
  scratch seen is kept between calls. This is also the first MEASUREMENT of
  the page-fault floor: 26 ms per sort sequentially (12.8%) and 15 ms at t=4
  on the Linux container; likely more under WSL2.
- **D. Near-sorted index-partition + one-pass P-way merge.** Split by index
  (order kept), sort chunks in parallel into dst, cut the sorted runs into P
  value bands by binary search (free), P-way heap-merge each band straight
  into a[] — no scatter, no copyback. Gated by the dispatcher's inversion
  sample. Bit-exact on adversarial shapes (duplicates across bands, run-
  structured, far outliers). One-core t=4: near_sorted 261 -> 163 ms.
- 606 checks, 0 failures throughout.

## 3.5.0 — snake_case throughout (4.0.0 camelCase revision withdrawn)
- Naming convention settled on C's own: snake_case functions and variables,
  `*_t` types, UPPER_SNAKE macros. The engine had been split between the
  camelCase functions ported from the JavaScript engine (`coarseBucketSort`,
  `mergeGallop`, `skewProbe`...) and snake_case parallel machinery; now one
  convention end to end — engine, header, tools, wasm recipe, docs. 114
  identifiers renamed mechanically with word-boundary matching, then audited.
- Public API unchanged from 3.x (`logos_sort_f64`, `logos_sort_i32`,
  `logos_num_cpus`), so this is a minor bump, not a major one. The 4.0.0
  camelCase revision (delivered and withdrawn within minutes, used by nobody)
  is superseded; its names survive as deprecated inline aliases.
- No algorithmic changes; 606 checks, 0 failures; all outputs match std::sort.

## 3.4.1 — pfbench: interleaved A/B for the prefetch distance
- Two separate `parbench` runs at PF_DIST 64 and 128 were both flagged by the
  calibration line as loaded (+25-50% on unchanged code) and could not answer
  the question. Separate runs cannot resolve a ~5% knob when load drifts 25%
  between them.
- `make pfbench`: five engine variants (prefetch off, PF_DIST 16/32/64/128)
  linked into ONE binary and interleaved on identical data, so load affects
  every variant equally and cannot bias the comparison. `PF_DIST` is now
  overridable (`-DPF_DIST=N`).
- One-core result: prefetch on vs off is the real effect (15-25%); 16/32/64
  are within ~3%; 128 slightly worse. Default stays 32.

## 3.4.0 — memory-level parallelism in the scatter
- Evidence from the 20-CPU fair fight: both sorts sped up 1.3-1.5x going from
  10 to 20 threads, so the scatter is memory-LATENCY-bound (hyperthreads hide
  stalls), not bandwidth-saturated. The limiter is the serial cursor chain
  `to[c[b]++] = x` first seen in the project's very first profile.
- **Bucket-index cache**: the partition's histogram pass now stores each
  element's bucket (1 byte) so the scatter reads it instead of recomputing
  `bucket_of` — 15-20% off the parallel path by itself (f64 and int32).
- **Destination prefetch** (32 elements ahead) in every scatter: partition,
  coarse, bits-coarse, int32 radix, float radix. Another 6-13%, sequential and
  parallel, on scatter-heavy shapes; shows even on one core (uniform -13%),
  confirming stalls were the cost. Neutral on the zero-scatter counting paths.
- Interleaved three-way A/B (3.3.2 | +cache | +cache+prefetch), 8M: uniform
  t=4 284 -> 227 -> 212 ms; heavy_tail 228 -> 192 -> 185; int_wide 186 -> 162
  -> 146; int32 122 -> 104. 606 checks, 0 failures.

## 3.3.2 — spin-then-block barrier
- A second, moderately loaded 20-core run (std::sort 12-45% off quiet on
  unchanged code; calibration line now establishes the baseline) showed the
  load asymmetry persisted after chunking: on losing rows logos-parallel ran
  1.7-1.8x slower than its own quiet numbers while __gnu_parallel::sort moved
  0-20%. Chunking fixes work imbalance; the remaining cost is barrier WAKE-UP
  latency — the old barrier slept on a condvar immediately, so every phase
  paid scheduler wake-ups for 19 threads (milliseconds each on loaded WSL2).
- Barrier now spins on an atomic generation counter (~0.5-1 ms budget) before
  sleeping — libgomp's approach. Threads arriving within microseconds of each
  other never sleep; only true stragglers pay a wake-up.
- `parbench` honors `LOGOS_THREADS=N` for both sorts (try physical cores on a
  hyperthreaded box: memory-bound work gains little from HT).
- Rows independent of scheduling luck were steady across all runs: quantized
  24.2-24.6 ms (2.5x), few_distinct 2.9-6.3x. 606 checks, 0 failures.

## 3.3.1 — straggler tolerance
- A loaded 20-core run (unchanged opponents 1.5-2x slower; logos on 20 threads
  slower than on 1) exposed an architectural weakness: histogram and scatter
  used one fixed slice per thread under barriers, so every phase waited for the
  slowest of 20 threads — under background load logos degraded 2-5x while
  `__gnu_parallel::sort` (dynamically scheduled) degraded 1.3-2x. The int32
  path, byte-identical between versions, doubled while its opponent did not
  move: same load, same code, twice the damage.
- Fix: every phase now pulls fixed-size CHUNKS from an atomic counter (the
  bucket phase already did). Histograms and scatter cursors are per chunk, so
  any thread may process any chunk; a descheduled thread does fewer chunks
  instead of stalling the barrier. Applies to f64 and int32.
- `parbench` prints a machine-calibration line (memcpy 64MB, std::sort 1M,
  single-threaded): if it drifts >15% between runs, the machine is loaded and
  the table is not comparable. Discard such runs.
- 606 checks, 0 failures.

## 3.3.0 — moderate cardinality, runtime write-combining
- **Moderate-cardinality parallel counting** (hash map, zero scatter): the
  low-cardinality idea extended to thousands of distinct values. A fixed
  2048-sample answers only the proportion question ("enough repeats?"); the
  real probe is a capped per-thread hash-map pass that aborts at 65536 distinct
  (cost O(cap), never O(n)) and, when it succeeds, is already pass one — the
  output is regenerated as runs from (value, count), no element ever moved.
  Measured sequential crossover (interleaved A/B, 8M): 2.5x at 256 distinct,
  1.4x at 4096 (quantized), 1.2-1.3x to 32k; gate declines above with no
  regression. Gate is thread-aware (looser when >=8 threads, where the map
  pass parallelizes). Targets the last fair-fight loss (quantized, 0.72x).
- **Write-combining is runtime-gated** (on when nthreads >= 8), no compile
  flag: the 20-core run showed it loses 8-35% sequentially and wins 5-12% at
  20 threads, moving the floor ~75 -> ~68 ms — i.e. bandwidth is ~10% of the
  floor; the rest is fixed cost and contention.
- Harness: `mod_card_30k` shape; 606 checks, 0 failures.
- Documented the sampling principle: sample size scales with the decision's
  resolution (buckets, cap), never with n.

## 3.2.0 — leverage parallelism by moving fewer bytes
Driven by the 20-core fair fight (parity with __gnu_parallel::sort, 4 wins /
4 losses, both converging on the same ~70 ms memory-bandwidth floor).
- **Native int32 engine** (`logos_sort_i32` no longer widens to double): 4-byte
  elements through every phase, halving bytes moved. Leaf = JS `sortI32`
  (asc/desc prescan, counting when span<=n, byte-skip LSD radix). Targets the
  0.71x int32 loss.
- **Low-cardinality parallel counting sort**: when a strided sample shows few
  distinct values, per-thread maps (keyed on the IEEE total-order bits, so
  -0.0/+0.0 stay distinct and output is bit-exact) are merged and every thread
  fills its own output range — two balanced parallel passes instead of a
  partition that cannot split equal keys. Also used sequentially: few_distinct
  8M 130 -> 49 ms. Branchless small-map lookup (the binary search's mispredicts
  had made the first version slower). Targets the 0.71x few_distinct loss.
- **Write-combining scatters, OPT-IN (`make WC=1`)**: interleaved A/B on one
  core measured 8-23% slower (destination lines are L2-resident; staging is
  overhead), so it ships off by default. It can only pay where the scatter is
  DRAM-bound — many cores — which is where to test it.
- Harness: 570 checks, 0 failures throughout.

## 3.1.0 — fair-fight benchmark
- `parbench.cpp` (optional, g++ + OpenMP): logos vs `std::sort` and
  `__gnu_parallel::sort` at equal thread counts, outputs verified against
  `std::sort`. Sequential: v3 beats `std::sort` on all 8 shapes (1.2–3.2×).
- Makefile `parbench` target; included in `dist`.

## 3.0.0 — JS-faithful dispatcher as the leaf
- Ported the JavaScript engine's numeric dispatcher (`lib/sort.js`) as the
  per-bucket leaf and sequential path: prescan early-exits; counting sort;
  byte-skip int32 LSD radix; run detection with prefix bail; galloping 2-way
  and 4-way merges; momentum insertion with displacement budget; gap detection
  and gap-split; flash sort; skew probe → exponent-bit coarse; value coarse
  bucket sort; quantized-float 8-pass radix; dual-pivot introsort.
- Module-level JS state (`lastBailAvgRun`, pooled scratch) became per-thread
  context for concurrency safety.
- Deliberate improvement over JS: bits-coarse same-high-word ranges fall back
  to value coarse instead of quicksort (bimodal 8M: 895 → 366 ms).
- Harness: 15 shapes, 570 checks vs qsort, six shapes targeting one ported
  path each. Fixed sequential heavy-tail loss (0.75× → 5.5×); near-sorted
  3.9× → 11×.
- Self-contained package: Makefile (static/shared lib, install, dist),
  example, license, changelog.

## 2.0.0 — multi-threaded design
- Coarse interpolation leaf replaced the 8-pass radix (3× sequential).
- One thread spawn per sort with barrier-synchronized phases; per-thread
  arenas; 4×P buckets with an atomic work queue; parallel copyback;
  branchless bucket lookup (found via interleaved A/B: binary-search
  mispredicts were the partition cost).

## 1.0.0 — initial parallel sample sort
- Sample → quantile splitters → parallel histogram/scatter → per-bucket LSD
  radix on order-preserving keys; pthreads spawned per phase.
