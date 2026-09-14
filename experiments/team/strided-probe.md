# strided-probe — order the engine cannot see: k interleaved sorted runs

Agent: strided-probe. Target: OUTSIDE_THE_BOX.md C1. Work copy: `/tmp/logos-team/strided-probe/work`
(WSL). Patch: `experiments/team/patches/strided-probe.patch` (`patch -p1` from the repo root; paths
`work/engines/c/...`). A previous session wrote the design and the prediction below and died before
producing a patch; this session built it from scratch, keeping that prediction as the one on record.

## What was tried

Data that is sorted UNDER A STRIDE — a sorted matrix read column-major, a k-way round-robin interleave
of k sorted streams, a merge of k shards taken one element each in turn — is a permutation of k sorted
runs whose elements sit at positions `i ≡ r (mod k)`. Every probe the dispatcher has (ascending runs,
inversion rate, block permutation, cardinality, KS uniformity) sees it as random and sends it to the
partition.

1. **The zoo.** `ileave_k` (k ∈ {2, 3, 4, 7, 8, 16, 64}: k sorted runs of one distribution, interleaved)
   and `transp_k` (k ∈ {4, 7, 64}: a sorted array's k-row matrix read column-major, i.e. the runs are
   value-disjoint in class order) added to `chaosbench`, with `ch0`/`ch4` built `-DLOGOS_NO_STRIDED` as
   the engine before this route.
2. **The probe** (`strided_probe_f64`). 1024 windows of 65 elements at `p_j = j·step + floor(frac(j·φ)·step)`:
   ascending in j by construction, Weyl-jittered so that `p_j mod s` is equidistributed for every s (the
   ORDER_FROM_CHAOS M5 aliasing lesson, in reverse: a plain stride puts every sample into one residue
   class for any s dividing the stride and passes vacuously). Every s in 1..64 is tested in one pass over
   the samples with a compact alive list, and s survives only under BOTH of
   - *window*: `a[p] <= a[p+s]` — the lag-s neighbour must not descend;
   - *class*: the samples ascend inside each residue class mod s.
   s = 1 surviving means nearly sorted (not ours: decline). The smallest survivor is k. The probe also
   reports whether every sampled row of the k-column view is ascending (runs look value-disjoint).
3. **The route** (`strided_try_f64`), filter-then-verify. A parallel gather copies the k runs contiguously
   into scratch while checking `a[i] >= a[i-k]` for every element (NaN fails it); any violation bails
   with `a` untouched, so the probe never has to be a proof. After one barrier:
   - runs value-disjoint in class order (transposed matrix): scratch IS the sorted array, parallel copy
     back. Two passes, zero compares, any k ≤ 64.
   - otherwise, k ≤ `STRIDED_MERGE_MAX_K` (8): log2(k) passes of a branch-free 2-way merge ping-ponging
     between scratch and `a`. Each thread owns the output slice between two splitters taken from the
     dispatcher's already-sorted sample; lower-bounding its two splitters in every run once gives, by
     prefix sums, its segment of every merged group at every level. A barrier separates levels. When the
     pass count is even the result lands in scratch and one copy pass (after a barrier) follows.
   - k > 8 with overlapping runs: declines up front (see "Why").
   `MERGE_MAX_RUNS` (8) is not applied: it caps the LEAF's galloping 4-way ladder, a different kernel.
   Opt-out `-DLOGOS_NO_STRIDED` (the A/B baseline). Opt-in `-DLOGOS_STRIDED_LOSER_TREE`: the first,
   measured-negative merge (below). `-DLOGOS_TRACE` prints `strided: probe k=.. rows_ascending=..
   (N compares over 1024 windows)`, `gather done|bailed`, and the final disposition.
4. **Harness**: nine new f64 shapes (`ileave_2`, `ileave_7` — n not a multiple of k —, `ileave_64`,
   `transp_64`, `ileave_break` — one violation at 90%, the gather must bail —, `ileave_inf` — ±inf inside
   runs —, `ileave_ties`, `ileave_div` — disjoint runs in the wrong class order —, `transp_break` — last
   row breaks disjointness without breaking a column, so the windows guess wrong and the route must bail
   after the gather) × 9 sizes × 4 thread counts = 324 checks. 1159 total, all green.
5. **A/B**: `ledgerbench` gained the strided shapes plus `near_sorted` and `segmented` guards;
   `tools/ledger_rerun.py` gained the feature (flag-disables) and wraps the run in the bench lock.

## Prediction (written before any measurement, by the previous session; kept as the one on record)

Reference box, 8M f64, 14 threads (parallel) and 1 thread (sequential), ratios pristine/route.

| shape | 1T | 14T | reasoning |
|---|---|---|---|
| ileave_2 | 3.0x | 3.0x | gather ~10 ms + 2-way merge ~2.5 ns/elem (20 ms) vs ~95 ms; at 14T 2+2 ms vs 19 |
| ileave_3, 4 | 2.5x | 2.5x | 4-leaf tree, 2 levels |
| ileave_7, 8 | 2.0x | 2.5x | 3 levels |
| ileave_16 | 1.5x | 2.0x | 4 levels |
| ileave_64 | 1.0-1.2x | 1.5-2.0x | 6 dependent compares/elem ~10 ns: at 1T that IS the AVX2 quicksort's cost; at 14T the partition's fixed costs still lose to one merge pass |
| transp_4, transp_64 | 5x | 4-5x | gather + memcpy back, zero compares, ~2 passes |
| every other shape | 1.00x | 1.00x | probe fires or it does not; ~0.1% of a 1T sort, 0.5% of a 14T one, under the paired floor |

What would make it wrong: (a) the gather's k write streams; (b) the loser tree mispredicting worse than
modelled; (c) the probe firing on a zoo shape it should not.

The C1 text predicted 3–10x; the prediction above says LESS at 1T for large k because a merge is
comparison work (the merge-vs-scatter ledger row).

## What was measured

MEASURED-TABLES-PLACEHOLDER

## Verdict

VERDICT-PLACEHOLDER

## Why, mechanism

- **The probe's first version was wrong, and the verifying gather is what saved it.** Testing "samples
  ascend inside each residue class" on samples thousands of positions apart returned k = 5 on an
  8-interleave, k = 14 on a 64-interleave, k = 6 on a 17-interleave (trace). k interleaved runs of ONE
  distribution are coarsely sorted: `a[i]` is about the i/n quantile with noise of order sqrt(n), so
  two samples 39k positions apart are ordered with 7-sigma confidence whatever residue class they share.
  The test has to compare adjacent members of a class, `a[p] <= a[p+s]`, which for a wrong s is a coin
  flip. The class test stays as a second condition because the window test alone lets a long-period
  sawtooth through (s = 2 survives 1024 windows with 68% probability when the period is 5376). Every
  wrong k cost only a bailed gather; `a` was never touched.
- **A k-way loser tree loses to the partition for k ≥ 8.** Same run, 1T, loaded box (only the ratio
  counts): 24 ns/elem for k = 2, 230 for k = 8, 410 for k = 64, against 115 ns/elem for the partition
  on uniform. log2(k) dependent, half-mispredicted steps per element; this is the ledger's "a merge is
  comparison work" (parallel merge sort row) and the knobbench measurement behind `MERGE_MAX_RUNS`
  (64 contiguous runs: merge 283 ms vs leaf 116) in a new coat. The branch-free 2-way merge runs at
  ~2.4 ns/elem per pass (measured: the k = 2 row), so log2(k) passes of it beat one loser-tree pass up
  to k ≈ 8 and lose to the partition beyond: the merge route is capped at k = 8, the disjoint route is
  not (zero compares).
- **The transposed matrix is the shape where addressing beats comparing outright:** gather + copy, no
  comparison at all.
- **Two races the harness found** (both at k ≥ 3, P > 1, never at k = 2 or P = 1): a thread's writes at
  level ℓ, in the coarser group layout, overlap what a slower thread still reads at level ℓ−1 from the
  same buffer; and the final copy back from scratch is a level too. A barrier between levels and one
  before the copy; log2(k) barriers in all, P = 14, negligible.

## What I would do next

- Read the trace's phase split on a quiet box: the route is two to four streaming passes; at 14T those
  are bandwidth-bound (14 × 2 × 8 B / 2.4 ns ≈ 93 GB/s asked of a laptop's ~50), so the 14T ceiling is
  the copy-pass cost, not the compare. Fusing the gather with level 0 of the merge for k = 2 (read two
  columns, write one merged stream) would make ileave_2 a ONE-pass sort.
- Unequal shards: a round-robin merge of shards of unequal length changes stride near the end; the
  probe's tail samples kill every s and the route declines. A "strided prefix" variant (route the
  prefix, sort the tail, one 2-way merge) is the next shape.
- k = 16..64 with overlapping runs is left to the partition on purpose. The only thing that could win
  there is a SIMD bitonic merge network (the x86-simd-sort design), which is the "parallel-SIMD leaf"
  ledger row's territory.

## Anything surprising

- Interleaves of one distribution are sorted at coarse scale. That is a structural fact worth
  remembering beyond this route: any probe on far-apart samples (the Fibonacci-lag inversion rate
  included) sees interleaved runs as nearly sorted. The near-sorted route declined them only because
  its gate is on a 0.37%-of-n lag where the sqrt(n) noise still dominates at 8M; at larger n it might
  not.
- The engine's own merge machinery, handed the same k contiguous runs, is the measured ceiling the task
  asked for, and it says the same thing as my loser tree: for k = 64 the leaf beats the merge 2.4x.
  A k-way merge of sorted sequences is not a cheap deliverable; only k ≤ 8, or disjoint runs, are.

## Commands (decisive measurements)

```
# work copy: /tmp/logos-team/strided-probe/work/engines/c   (built from the patch on a fresh copy)
make test                                                    # 1159 checks
cc -O3 -std=gnu11 -pthread -DLOGOS_TRACE logos_psort.c logos_extra.c stridetrace.c -o stridetrace -lpthread -lm
flock /tmp/logos-bench.lock ./stridetrace 8000000 14         # mechanism: which route, phase stamps
flock /tmp/logos-bench.lock ./stridetrace 8000000 1
make chaosbench && flock /tmp/logos-bench.lock ./chaosbench   # blindness table, ch0/ch4 = -DLOGOS_NO_STRIDED
cd ../.. && python3 tools/ledger_rerun.py --only LOGOS_NO_STRIDED --pairs 24   # paired, both orientations, lock inside
```
