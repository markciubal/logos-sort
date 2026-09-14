# chaos-explorer: order hidden in data that looks random to the engine's probes

Agent: chaos-explorer, 2026-09-12, overnight team run. **No engine code was changed.** The deliverable is
a bench (`engines/c/chaoshunt.cpp` + two Makefile targets, in the patch), a ranked table of what the engine
leaves on the table on twenty realistic shapes, and three build-ready proposals with numeric predictions.

## What was tried

Twenty shapes that occur in real systems, each generated with the structure a probe could catch, sorted
by the shipped engine at 1 and 14 threads, by a hand-routed "informed" sort that is *told* the structure
(so its time is the ceiling any probe could reach), and by x86-simd-sort's AVX2 quicksort (the real
repo's clone, OpenMP at 14). A `-DLOGOS_PROFILE` copy of the engine (renamed `cp_*`, linked into the
same binary) names the route and the dominant leaf path logos took on every row, and with
`CHAOS_PROF=1` prints the per-phase and per-thread breakdown. Every output is compared bit-exact
against `std::sort` of the same input; all 40 rows passed in every run. Contenders are interleaved per
rep inside one row, so the ratios below are within-row only (CLAUDE.md rules 2 and 3); the box was
loaded by six other agents throughout (memcpy-64MB 5.5-6.7 ms, i.e. the "loaded" band), so absolute
milliseconds are not comparable to anything in FINDINGS.

The informed sorts: **bitmap** (dense permutation: one pass sets a bit per value and proves it is a
permutation, one pass writes min..max), **blocks+3merge** (displacement <= w: sort w-blocks in cache
with x86-simd-sort's AVX2 kernel, then block k's final content is ranks [w, 2w) of the union of blocks
k-1, k, k+1 -- proof: every element of blocks 0..k-2 has final position < kw and every element with
final position < kw lies in blocks 0..k, so exactly w elements of that union rank below block k),
**stable scatter** (two passes into buckets whose input order is already ascending), **k-way merge**
(P-1 splitters, binary search in every run, P heap merges), **tail+merge** (engine sorts the tail, one
backward in-place merge), **reverse+logos**, **narrow radix** (11-bit LSD over the fewest bits, parallel
per pass), **counting** (direct index on the lattice, per-thread tables), **hash count** (one shared
CAS table, distinct values sorted by the engine, runs emitted).

## Prediction (written before measuring, verbatim)

Bench: `chaoshunt` (my copy, `engines/c/chaoshunt.cpp`), n = 2^23 doubles, 20 shapes, best of 5,
logos at 1 and 14 threads against a hand-routed "informed" sort that is told the structure, plus
x86-simd-sort (AVX2 kernels, real-repo clone) where it builds. A -DLOGOS_PROFILE copy of the engine
reports which route and leaf path the engine actually took on each shape.

Gap = logos / informed ceiling, same row, same run.

| shape | hidden order | predicted route (1T / 14T) | predicted gap 1T | gap 14T |
|---|---|---|---|---|
| hash_order (Fibonacci-hash iteration order of 0..n-1) | dense permutation | quant radix / partition+counting leaf | 3-4x | 2-3x |
| gray_code | dense permutation | same | 3-4x | 2-3x |
| event_ts_latency (send time keyed, arrival order, latency <= 4096 events) | displacement <= w | AVX2 leaf / partition | 1.4x | 1.2x |
| window_shuffle (sorted, shuffled inside 2048-windows) | displacement <= w | AVX2 leaf / partition | 1.4x | 1.2x |
| shard_ids_disjoint (8 allocators, disjoint bases, interleaved) | stable bucket by allocator = sorted | AVX2 leaf / modes+partition, leaf prescan sorted | 5x | 1.3x |
| shard_ids_blocks (1024-id blocks from a global counter, 8 consumers) | stable bucket by block = sorted | counting (all-int span n) / partition | 1.0-1.5x | 1.5x |
| sorted_runs_16 (16 concatenated sorted shards, full range each) | 16 runs | leaf (runs > MERGE_MAX_RUNS) / partition | 1.0x | 1.0-1.3x |
| sorted_runs_256 | 256 runs | leaf / partition | 0.8x (merge loses) | 0.8x |
| sorted_plus_tail (95% sorted + 5% random appended) | one run + small tail | AVX2 leaf / near-sorted parallel | 5x | 1.5x |
| reverse_near_sorted (descending with 1% swaps) | descending near-sorted | AVX2 leaf (runs only ascending) / partition | 3x | 2x |
| prices_cents (lognormal cents) | 24-bit ints | quant radix / partition | 1.0-1.2x | 1.2x |
| epoch_ms (one day of ms timestamps, random order) | 27 bits vary | quant radix or bits-coarse / partition | 1.0-1.3x | 1.3x |
| sensor_adc (12-bit ADC sine+noise) | 4096-value lattice | modcard or lattice / same | 1.0-1.3x | 1.0-1.3x |
| sensor_f64 (sine drift + gaussian noise) | smooth index->value map | AVX2 leaf / partition | control, no ceiling | - |
| geo_lat (200 Zipf-weighted clusters + background) | mixture | AVX2 leaf / partition | control | - |
| user_ids_zipf (200k sparse ids, Zipf activity) | 200k distinct > MODCARD_CAP | AVX2 leaf / partition | 2x | 1.3x |
| yyyymmdd (20 years of dates as integers) | 7300-value lattice with holes | modcard / modcard | 1.0-1.2x | 1.0-1.2x |
| mantissa16 ([1,2) with 16 mantissa bits) | 65536-value lattice | lattice or quant radix / same | 1.0-1.3x (2x if it misses) | same |
| small_ratios (a/b, a,b in 1..100) | ~6k distinct | modcard / modcard | 1.0-1.2x | 1.0-1.2x |
| uniform | none | AVX2 leaf / partition | reference row | - |

Overall prediction: the three largest gaps will be shard_ids_disjoint at 1T, sorted_plus_tail at 1T
and the dense permutations (hash_order, gray_code) at both thread counts; everything the counting /
lattice / modcard family already covers (adc, dates, ratios, mantissa16) will be within 1.3x of its
ceiling. x86-simd-sort will beat logos 1T on the continuous unstructured rows by 1.2-1.3x (FINDINGS 1e)
and lose on every row where a counting or lattice route fires.

## What was measured

Two locked best-of-5 runs (run A: all 20 shapes; run B: the top-gap shapes again, 20 minutes later) and
one locked profiling run. Gap = logos / informed, within the row. "route/leaf" is what the profiled copy
reported. logos/xss = logos time over x86-simd-sort time in the same row (>1 means the SIMD sort was
faster).

### Ranked table (run A; run B in parentheses where it was re-measured)

| rank | shape | hidden order | thr | logos ms | informed ms | **gap** | logos/xss | engine route/leaf | informed method |
|---|---|---|---|---|---|---|---|---|---|
| 1 | shard_ids_blocks | ascending inside each 1024-id block | 1 | 403 (343) | 26 (25) | **15.4x (13.7x)** | 3.8x | sequential / int32 radix | stable scatter |
| 2 | gray_code | dense permutation of 0..n-1 | 1 | 451 (393) | 35 (33) | **13.0x (12.0x)** | 3.4x | sequential / quant radix | bitmap |
| 3 | hash_order | dense permutation of 0..n-1 | 1 | 329 (285) | 35 (33) | **9.4x (8.6x)** | 3.1x | sequential / quant radix | bitmap |
| 4 | sorted_plus_tail | 95% sorted + 5% random tail | 1 | 191 (155) | 28 (17) | **6.7x (9.3x)** | 1.4x | sequential / coarse | tail+merge |
| 5 | mantissa16 | 65536-value lattice in [1,2) | 14 | 139 (85) | 20 (13) | **6.9x (6.7x)** | 4.0x | modcard counting | counting |
| 6 | shard_ids_disjoint | 8 ascending streams, disjoint ranges | 1 | 162 (147) | 38 (32) | **4.3x (4.6x)** | 1.2x | sequential / simd leaf | stable scatter |
| 7 | yyyymmdd | 7305-value lattice with holes, span 200k | 1 | 160 (96) | 47 (28) | **3.4x (3.4x)** | 2.0x | modcard counting | counting |
| 8 | mantissa16 | | 1 | 150 (96) | 50 (31) | **3.0x (3.2x)** | 1.3x | sequential / quant radix | counting |
| 9 | prices_cents | 24-bit integers, lognormal, ~50k distinct | 14 | 109 (103*) | 37 (32*) | **2.9x (3.2x*)** | 4.1x | modcard counting | narrow radix |
| 10 | yyyymmdd | | 14 | 46 (19) | 20 (11) | **2.3x (1.7x)** | 1.5x | modcard counting | counting |
| 11 | sorted_plus_tail | | 14 | 32 (27) | 25 (15) | 1.3x (1.7x) | 0.8x | near-sorted merge / run merge | tail+merge |
| 12 | sensor_adc | 4096-value lattice | 1 | 63 | 43 | 1.45x | 0.95x | lattice counting | counting |
| 13 | prices_cents | | 1 | 161 (195*) | 113 (106*) | 1.4x (1.8x*) | 2.2x | sequential / quant radix | narrow radix |
| 14 | shard_ids_blocks | | 14 | 12.7 (15) | 10.3 (11) | 1.2x (1.4x) | 0.35x | partition / counting | stable scatter |
| 15 | sensor_adc | | 14 | 14.5 | 11.1 | 1.3x | 0.6x | lattice counting | counting |
| 16 | gray_code | | 14 | 13.8 (12.5) | 10.9 (10.7) | 1.3x (1.2x) | 0.38x | partition / counting | bitmap |
| 17 | epoch_ms | 27 of 41 bits vary | 1 | 164 | 152 | 1.1x | 1.4x | sequential / simd leaf | narrow radix |
| 18 | hash_order | | 14 | 16.0 (18.4) | 15.4 (17.4) | 1.0x (1.05x) | 0.59x | partition / counting | bitmap |
| 19 | reverse_near_sort | descending + 1% swaps | 1 | 144 | 140 | 1.0x | 1.1x | sequential / flash | reverse+logos |
| - | small_ratios | ~6k distinct rationals | 1 / 14 | 128 / 35 | 133 / 62 | 0.96x / 0.57x | 1.6x / 1.0x | modcard counting | hash count |
| - | user_ids_zipf | 200k distinct sparse ids, Zipf activity | 1 / 14 | 119 / 100 | 174 / 112 | 0.68x / 0.89x | 1.4x / **2.6x** | simd leaf / partition + int32 radix | hash count |
| - | epoch_ms | | 14 | 27 | 57 | 0.47x | 0.70x | partition / flash | narrow radix |
| - | event_ts_latency | displacement <= 4096 | 1 / 14 | 172 / 16 | 285 / 40 | 0.60x / 0.40x | 1.2x / 0.33x | coarse / partition+flash | blocks+3merge |
| - | window_shuffle | displacement <= 2048 | 1 / 14 | 189 / 16 | 263 / 36 | 0.72x / 0.44x | 1.4x / 0.35x | coarse / partition+flash | blocks+3merge |
| - | shard_ids_disjoint | | 14 | 21 (19) | 32 (27) | 0.65x (0.71x) | 0.67x | partition / counting | stable scatter |
| - | reverse_near_sort | | 14 | 16 | 22 | 0.70x | 0.54x | partition / flash | reverse+logos |
| - | sorted_runs_16 | 16 sorted runs | 1 / 14 | 157 / 19 | 258 / 36 | 0.61x / 0.54x | 1.0x / 0.28x | simd leaf / partition+flash | k-way merge |
| - | sorted_runs_256 | 256 sorted runs | 1 / 14 | 135 / 16 | 382 / 52 | 0.35x / 0.30x | 1.3x / 0.47x | simd leaf / partition+flash | k-way merge |
| - | uniform | reference | 1 / 14 | 171 / 33 | - | - | 1.4x / 0.94x | simd leaf / partition+flash | - |
| - | sensor_f64 | smooth drift + noise (control) | 1 / 14 | 169 / 33 | - | - | 1.4x / 0.87x | simd leaf / partition+flash | - |
| - | geo_lat | 200 clusters (control) | 1 / 14 | 188 / 43 | - | - | 1.3x / 1.04x | simd leaf / partition+flash | - |

(*) prices_cents and mantissa16 profile-run values are from the best-of-1 profiling run, not run B.

### The mechanism behind the 14-thread modcard rows (CHAOS_PROF=1, 14 threads, best of 1)

| shape | route | wall ms | phase | per-thread busy ms (min..max) | serial or unattributed share |
|---|---|---|---|---|---|
| prices_cents | modcard counting | 104.6 | modcard probe 104.3 | 14..30 | ~75% |
| mantissa16 | modcard counting | 86.5 | modcard probe 86.4 | 18..38 | ~65% |
| user_ids_zipf | partition | 68.9 | modcard probe 30.5, histogram 13.6, scatter 5.1, leaf 17.7 | 47..60 | the 30.5 ms modcard attempt aborted at MODCARD_CAP and was thrown away (44% of wall) |

## Verdict: probe-only

Nothing shipped, nothing was meant to ship: the target was to quantify. Three of twenty shapes show a
9-15x gap at one thread, four more show 3-7x, and at 14 threads the engine's partition already sits
within 1.0-1.4x of the informed ceiling on every shape except the three the modcard route takes
(2.3-6.9x). Seven shapes are already handled as well as or better than the "obvious" informed route.

**Prediction graded.** Right: sorted_plus_tail 1T (predicted 5x, got 6.7-9.3x), shard_ids_disjoint 1T
(5x, got 4.3-4.6x), sorted_runs (merge loses, and by more than predicted), the controls, and the
14T rows of the dense permutations (predicted 2-3x, got 1.0-1.3x -- wrong in the engine's favour: the
partition's counting leaf is as good as a bitmap). Wrong, with the reason found: (1) the dense
permutations at 1T are 9-13x, not 3-4x, because the radix paths that take them are 3x slower than
x86-simd-sort on the same row on this box, not roughly equal as I assumed; (2) shard_ids_blocks at 1T
is 15x, not 1.0-1.5x, because its span is 1.001n, which fails `COUNTING_SORT_K = 1` and lands on the
int32 LSD radix, the slowest 1T path measured (403 ms); (3) mantissa16 and yyyymmdd are not lattice
routes but modcard, because `LATTICE_MAX_SLOTS 65536` excludes a 200k-slot span and 65536 + margin;
(4) reverse near-sorted is already fine: flash on a descending near-sorted array ties reverse+near-
sorted at 1T and beats it at 14T; (5) the bounded-displacement shapes have no gap at all: coarse's
scatter on locally ordered data is nearly sequential (1T 172 vs uniform 171 in the same run is
coincidence of load; 14T 16 ms against uniform's 33 is not), and the block-sort+merge route loses even
with a SIMD block sort; (6) x86-simd-sort at 14T is *slower* than logos on 16 of 20 shapes here, not
just the structured ones, because a loaded box punishes its task-parallel quicksort more than logos's
chunk phases (CLAUDE.md warns exactly this: treat those ratios as an upper bound on logos).

## Why: the mechanism per gap

1. **Dense integers at one thread go to a radix, never to counting.** `leaf_f64` runs `quant_probe`
   before `sort_fn`, and integers 0..2^23 have four varying key bytes, so hash_order and gray_code take
   the 64-bit quantized radix (329-451 ms) and never reach `sort_fn`'s all-int counting branch (span <=
   n, ~30 ms). shard_ids_blocks reaches `sort_fn` but its span is n + 7*1024, so `COUNTING_SORT_K = 1`
   rejects counting and it takes `lsd_radix_int32` (343-403 ms). The bitmap or a byte-count does the
   whole job in two passes. At 14 threads none of this matters: the partition's buckets are dense
   integer ranges and the leaf's counting branch fires per bucket, within 1.0-1.4x of the bitmap.
2. **A sorted prefix with an appended tail is invisible to the sequential dispatcher.** Runs: one run
   of 0.95n plus ~200k tiny runs, more than MERGE_MAX_RUNS, so the run merge declines; the 40-point
   inversion probe sees ~5% inversions and momentum insertion is not attempted at 8M; the whole array
   goes to coarse (155-191 ms). Sorting the 5% tail and merging backward costs 17-28 ms. The
   `prescan` already stops at the first descent, which *is* the prefix length, so the probe is free.
   At 14 threads the near-sorted route catches it and is within 1.3-1.7x.
3. **Lattices above 65536 slots fall to the modcard hash, which is 3.4x costlier per element at one
   thread and mostly serial at fourteen.** yyyymmdd (span 200k, 3.6% occupied) and mantissa16 (65536 +
   margin) exceed `LATTICE_MAX_SLOTS`. The modcard route's profile at 14T shows 65-75% of its wall
   outside the per-thread map (merge of P tables, sort of the distinct keys, emit) -- 86-139 ms where
   the partition would have taken ~33 and a direct count takes 12-20. prices_cents (24-bit, ~50k
   distinct, not a lattice the table can hold) has the same serial tail: 103-109 ms at 14T against
   32-37 for a three-pass radix and 22-26 for x86-simd-sort.
4. **The modcard gate lets Zipf-distributed ids in and throws 30 ms away.** user_ids_zipf: Chao1 on a
   sample of a Zipf population is a lower bound that badly under-estimates (200k true, the gate passed
   at <= 65536), the map aborts at the cap after 30.5 ms, then the partition runs. That is 44% of the
   row and the whole 2.6x deficit to x86-simd-sort at 14T.
5. **Interleaved allocators at one thread.** shard_ids_disjoint: the sequential path never samples for
   modes (that machinery is in the parallel partition), so the AVX2 leaf sorts 8M elements that a
   two-pass stable scatter puts in order in 32-38 ms (4.3-4.6x). At 14T the mode tables + counting leaf
   already beat my scatter.

## The three proposals (numeric predictions, ready to build)

**P1. Dense-integer route from the sample (sequential path first).** Gate from the existing sodo
sample: every sample value integral, sample distinct == m or nearly, and (max - min + 1) from the
sample <= 4n. Route: bitmap of span bits (1 MB at 8M) with a margin of span/8 on each side, one pass
that sets bits and aborts on an already-set bit (duplicates -> fall back to a byte count with
saturation -> fall back to the current path) or an out-of-range value, one pass that writes the
values. Put it ahead of `quant_probe` in `leaf_f64` and relax `COUNTING_SORT_K` for this route only.
*Prediction:* hash_order, gray_code, shard_ids_blocks at 1T 8-15x (285-451 ms -> 30-40 ms on this
box), 14T neutral to 1.2x (the partition+counting leaf is already there; taking the bitmap at 14T too
is worth 1.2x on gray_code). No effect on any other shape: uniform's sample is not integral, int_wide's
span is 2^32 >> 4n. Harness checks to add: a permutation with one value missing (must fall back), one
duplicated, span exactly n, span 4n+1, negative base, n < 4096 (below the sodo gate).
Cost: half a day. Probe cost: zero, the sample already has min, max, distinct and integrality.

**P2. Sorted-prefix + tail route.** The prescan's first-descent index d is the prefix length for free.
If d >= n/2: sort a[d..n) with the engine (any route, P threads), copy it to scratch, merge backward in
place (one pass of at most n). Generalises by recursion to a log with several appended batches.
*Prediction:* sorted_plus_tail 1T 155-191 -> 17-28 ms (6-9x); 14T 27-32 -> 15-25 ms (1.3-1.7x), by
replacing the near-sorted route's chunk/keep/reject pass with one merge; zero effect elsewhere (on
random data d is ~1, on near-sorted data with 1% swaps d is ~50, both far below n/2). Harness checks:
tail length 0, 1, n-1, tail all smaller than the prefix, tail all larger, tail descending, prefix with
equal keys at the boundary (stability of the merge is not required but ties must not be lost).
Cost: half a day. Probe cost: zero.

**P3. Counts-only lattice tables to 2^18 slots, and a modcard cost gate.** `lattice_try_f64` stores
keys, values and counts per slot (24 B) which is why the cap is 65536. On a lattice the value is implied
by the index (base + i*step), and the per-element check `|q - round(q)| <= 1e-9 (q+1)` already proves
membership, so keys and vals are redundant: a uint32 count array is 1 MB per thread at 262144 slots,
L2-resident. Raise `LATTICE_MAX_SLOTS` to 262144 with that change. Separately, decline modcard when
the sample's Chao1 estimate is above ~16k at P >= 4 (its serial tail then exceeds the partition), and
replace Chao1 with a Good-Turing coverage gate (singletons/m > 0.3 means most of the population is
unseen: decline) to stop the Zipf abort. *Prediction:* yyyymmdd 1T 3.4x (96-160 -> 28-47 ms), 14T
1.7-2.3x; mantissa16 1T 3x, 14T 6.7-6.9x (85-139 -> 13-20 ms); prices_cents 14T 2.9-3.2x by declining
into the partition (103-109 -> ~35 ms); user_ids_zipf 14T 1.7x (69 -> ~40 ms) by not attempting the map;
sensor_adc, small_ratios, zipf, collatz unchanged (their tables already fit / Chao1 already small).
Harness checks: a lattice with 2^17 slots and 1% occupancy, a lattice with values outside the sample's
window on both sides, step 2^-16, a 200k-distinct Zipf sample that must be declined. Cost: one day.
This is the largest total win of the three at 14 threads and the only one that moves 14T rows.

Also worth a line each: **P4** per-mode ascending certificate in the sequential path (for each mode found
by the gulf test, the sample values in index order are ascending -> stable scatter by mode, then the
leaf's prescan finds each bucket sorted): shard_ids_disjoint 1T 4.3-4.6x, nothing else. **P5** the
sequential int32 LSD radix and the quantized radix are 3.1-3.8x slower than x86-simd-sort on the same
row at 8M on this box (uniform: 1.4x); whether that is bandwidth contention from the other agents or the
path itself needs a quiet-box run before anyone acts on it.

## What I would do next

Build P1 and P2 first (each half a day, each a pure sequential-path addition with a free probe), then
P3. Re-run `chaoshunt` after each; it verifies every output and names the route, so a route that fires
on the wrong shape shows up as a changed route name before it shows up as a time. Then a quiet-box
re-run of the 1T radix rows (P5) and, if the 3x holds, `make simdbench` with the int32 AVX2 leaf on.

## Anything surprising

- **Where a merge was the "obvious" informed route, the partition won every time**: k-way merge of 16
  or 256 known runs lost 1.6-3.3x to logos's partition at both thread counts, and block-sort+3-merge on
  bounded-displacement data lost 1.4-2.5x even with a SIMD block sort. The ledger's lesson ("a merge is
  comparison; the scatter is already near bandwidth") holds from the informed side too. The engine's
  coarse/partition on locally ordered data is already close to memcpy: 16 ms at 14T on event_ts_latency
  against 33 for uniform in the same run.
- **The engine's own counting leaf inside the partition equals a hand-written bitmap** on dense
  permutations at 14T (1.0-1.3x). The whole dense-integer problem is a sequential-dispatcher problem.
- **Reversed near-sorted data needs no reverse**: flash ties reverse+near-sorted at 1T and beats it 1.4x
  at 14T.
- **The modcard route at 14T is mostly serial** on 50-65k-distinct data: workers busy 14-38 ms of a
  86-105 ms wall. The route's per-route thread cap (FINDINGS 3k) addressed the merge cost ∝ P by capping
  P; the serial part it did not touch is now the whole story at 8M.
- **x86-simd-sort lost to logos at 14T on 16 of 20 rows** in this loaded run, including uniform (0.94x).
  CLAUDE.md predicts exactly this for a loaded box; the 1T column is the honest one and there the SIMD
  sort wins every continuous row by 1.2-1.6x and the dense-integer rows by 3.1-3.8x.

## Commands (repeatable)

```
# one-time setup of the isolated copy (see the team brief), then in it:
wsl -e bash -lc "cd /tmp/logos-team/chaos-explorer/work/engines/c && make chaoshunt-simd XSS=/mnt/c/Users/markc/Coding/logos-sort-repo/logos-sort-repo/engines/x86-simd-sort/src"
# decisive run (run A): all shapes, best of 5, 1 and 14 threads, under the bench lock
wsl -e bash -lc "cd /tmp/logos-team/chaos-explorer/work/engines/c && flock /tmp/logos-bench.lock ./chaoshunt 5"
# run B: the top-gap shapes again
wsl -e bash -lc "cd /tmp/logos-team/chaos-explorer/work/engines/c && flock /tmp/logos-bench.lock ./chaoshunt 5 0,1,2,5,6,9,17,18"
# per-phase / per-thread breakdown of the three modcard-affected rows
wsl -e bash -lc "cd /tmp/logos-team/chaos-explorer/work/engines/c && flock /tmp/logos-bench.lock env CHAOS_PROF=1 ./chaoshunt 1 11,16,18"
# without the x86-simd-sort clone (informed block sorts then use std::sort, 3x slower per level):
wsl -e bash -lc "cd /tmp/logos-team/chaos-explorer/work/engines/c && make chaoshunt && flock /tmp/logos-bench.lock ./chaoshunt 5"
```
Shape indices: 0 uniform, 1 hash_order, 2 gray_code, 3 event_ts_latency, 4 window_shuffle,
5 shard_ids_disjoint, 6 shard_ids_blocks, 7 sorted_runs_16, 8 sorted_runs_256, 9 sorted_plus_tail,
10 reverse_near_sort, 11 prices_cents, 12 epoch_ms, 13 sensor_adc, 14 sensor_f64, 15 geo_lat,
16 user_ids_zipf, 17 yyyymmdd, 18 mantissa16, 19 small_ratios.

`make test` in the work copy: all 835 checks pass (the engine is untouched; the patch adds
`chaoshunt.cpp` and two Makefile targets, `chaoshunt` and `chaoshunt-simd`). Raw outputs of the three
locked runs are in `/tmp/logos-team/chaos-explorer/run{1_locked_R5,2_profile,3_topgaps_R5}.txt` on WSL.

## Re-verification (07:53, same day, after the work copy was rebuilt from this patch)

The work copy was recreated from pristine and `experiments/team/patches/chaos-explorer.patch` applied
with `patch -p1` from inside `work/` (the integrator's flow from the repo root). It built
(`make chaoshunt-simd XSS=...`), `make test` passed all 835 checks, and the top-gap subset was re-run
under the lock (run C, `flock /tmp/logos-bench.lock ./chaoshunt 5 0,1,2,5,6,9,17,18`, raw output in
`/tmp/logos-team/chaos-explorer/run4_reconfirm_R5.txt`). **Run C's calibration line was memcpy-64MB
16.9 ms / logos-1M 47.5 ms, past the >10 ms line CLAUDE.md rule 2 says to discard** (six agents
benchmarking at once), so its milliseconds and exact ratios are not quoted in the table above. What it
does confirm, because every contender in a row alternates at fine grain, is the *ranking*: gray_code,
shard_ids_blocks, hash_order, sorted_plus_tail and yyyymmdd at 1T were 9-23x over their informed
ceiling, mantissa16 at 14T 8x, shard_ids_disjoint 1T 5x, and every 14T partition row sat at 0.8-1.4x.
The route/leaf names reported by the profiled copy were identical to runs A and B on all 16 rows. The
patch regenerated from the rebuilt copy is identical in content to the one already in the repo (only
diff timestamps differ).
