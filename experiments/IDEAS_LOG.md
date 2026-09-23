# Ideas log

An intake queue for ideas worth keeping, with what is known about each one and the next step that would settle it.
It is deliberately different from the three records it sits beside:

- **CLAUDE.md's ledger** holds ideas that were built and MEASURED TO FAIL, so nobody re-runs them.
- **TRUTH_TABLE.md** grades CLAIMS (measured / inferred / untested / false).
- **FINDINGS.md** is the narrative of what each round measured.
- **This file** is the queue: raised, mapped onto the engine, and either waiting for a measurement or closed with a
  pointer to where the measurement lives.

Rules that keep it useful: every entry says where the idea came from, what it maps onto in this engine, what is already
measured about it, and the ONE next measurement that would settle it. When an entry is settled it stays, with its
verdict and a pointer — an idea that was tried and lost is worth as much here as one that won.

Status: **OPEN** (worth doing, not started) · **BUILDING** · **CLOSED-WON** (shipped) · **CLOSED-LOST** (measured,
did not pay) · **PARKED** (needs hardware, a dependency, or a decision this repo cannot make).

---

## OPEN — ranked by measured upside

### 1. Where the lattice MAP phase's idle threads are (was: a parallel merge)
**From:** the Amdahl round (2026-09-22). **Maps to:** `count_try_f64` / `lattice_try_f64`, the serial merge between
`par_run(lc_map)` and `par_run(lc_emit)`.
**Known:** `make profile` at 8M has **6.0-6.9 of 14 threads busy and NONE waiting** inside the phase that is 98-99% of
those routes' wall; Karp-Flatt rises 0.06 → 0.23 as P goes 2 → 14, so the serial part GROWS with P (the lattice merge
walks `slots x P`). Two cheap remedies were measured and rejected: a global thread ceiling (the lattice family loses
0.53-0.90 at 8M-16M, `make capbench`) and a hash-only ceiling (1.0 at 4M, 1.1 at 8M, inside the floor). The
concatenate/sort/fold variant of the merge is already in the tree opt-in and measured SLOWER (`LOGOS_MC_CONCAT=1`,
0.58-0.90x at 14 threads) because it sorts `P*d` pairs for a union of `d`.
**SETTLED 2026-09-22, and the diagnosis moved.** The parallel merge was built (slot ranges reduced independently, an O(P)
prefix, ranges claimed with an atomic) and measured **1.02-1.12x SLOWER**: the TRACE puts the merge at ~2 ms of a route
whose MAP phase is 11.6-14.1 ms, so it is ~10% of the route, not the ~55% the occupancy average implied. It is correct
under every checker and ships OFF behind `LOGOS_LAT_PAR_MERGE=1`. What DID pay on the same route: each worker now
allocates its own count table instead of the caller allocating all P before the spawn (5.6 MB of serial memset at 8M and
14 threads) — **~1.03x, shipped**, 20 of 20 cells on the winning side against a measured null.
**What is still open:** the map phase is ~80% of the route and `make profile` still shows only 6.0-6.9 of 14 threads busy
there. The next measurement is finer profiler phases inside that route (map / merge / emit as separate phases), because
the current one lumps them together and an occupancy average cannot say which phase the idle time is in. Suspects: the
per-thread tables are 1.2 MB each at 50k slots (cache pressure) and the slot access is random.

### 2. A task queue for relocation's placement — SETTLED 2026-09-22: BUILT, MEASURED NEUTRAL, REVERTED (see S1)
The tiles were not the imbalance. 0.96-1.04 on every cell of two runs at 1M-16M. What remains of this entry is the
SECOND barrier: phase 2 merges overlapping chains from a queue already, but one long chain is one indivisible task, so
a single thread merges it while the rest wait. The next measurement is a merge-path split of one chain, not tiling.

### 2a. The original entry, kept for its evidence
**From:** the Amdahl round. **Maps to:** `reloc_worker` phases 1 and 3 (`TILE_IS_MINE`, static output tiles).
**Known:** the profile shows 6.6 busy, **3.6 WAITING at a barrier**, 3.8 idle; phase 2 (the chain merges) already uses an
atomic counter and does not stall this way. Perfect packing of that row is worth ~2.4x; realistically 1.1-1.3x.
**Next:** replace the static tile loop with the same atomic-counter pattern phase 2 uses, keeping the orphan-covering
contract (`par_spawn` + `barrier_drop`) intact. A/B on block_perm and block_overlap at 1M-30M, guards elsewhere, and
`make faultprobe` on the relocation cases because the orphan path changes.

### 3. Prime-implicant minimisation of the dispatcher's route choice
**From:** "kmap minimization with implicants" (2026-09-22). **Maps to:** the gate cascade in `psort_f64`.
**Known, and why only one of three mappings is open:** the BIT-level version is already shipped (a byte that is constant
across the keys is a don't-care, and the radix path skips that pass; the SIMD form of the skip measured ~0 in the
ledger). The bucket-lookup version is already in its minimal practical form (direct-index splitter tables, H0b, replaced
a 7-step binary search). What is NOT done is the ROUTE-SELECTION version: the dispatcher is a hand-ordered cascade of
~20 gates over a small feature set (integral values? `inv_all` ~ 0? short reach? Chao1 small? spans the range? P > 1?
n above a threshold?), which is a boolean function whose output is "the route that wins".
**Why it is worth doing:** the repo already holds the truth table. `make chaoshunt` runs 20 realistic shapes against a
sort TOLD the structure and prints the route the engine took; `make zroute` scores a rule fixed in advance on held-out
shapes; `make ztail` re-scores a tightened rule on 15 fresh ones. Minimising that table into prime implicants gives a
compact rule set that can be scored the same way, against the cascade, on shapes it has never seen.
**Next (one measurement):** build the truth table from chaoshunt's shapes (features from the shared sample, output = the
route that actually wins, from the told-the-structure ceiling), minimise with Quine-McCluskey, then score the minimised
rule against today's cascade on HELD-OUT shapes: count the cases where it picks a better route and the cases where it
picks worse. Only if it wins on held-out data does any code change follow. Expect the value to be in the shapes the
cascade currently mis-routes, not in the microseconds the gates cost (the whole gate phase is 1-4% of a parallel call).

### 4. A sorted DESCRIPTION instead of a sorted array
**From:** round six (`./unknotprobe 40 8388608 nocopy`). **Known:** dropping the copy-back is worth **1.21-1.35x** on the
block shapes; the run descriptors plus their order already describe the sorted sequence. **Next:** an API that returns
the description (`logos_sorted_runs`), with `rank`, `at` and `range` answered from it — this is direction 1's shape and
the only lever in the knot family that does not move bytes.

### 5. Kinked data sorts fastest on ONE thread
**From:** the Amdahl round. **Known:** 1% adjacent swaps at 8.4M: **6.84 ms at 1 thread**, 15.6 at 2, 9.4-9.7 at 4, 7.2 at
14 — more threads never pay. The near-sorted route is not the cause (switching it off helps at 2 threads by 1.18x and
COSTS 1.10x at 14). At one thread the detector repairs the kinks and the one-strand finish takes it.
**Next:** decide whether the dispatcher can recognise "one strand with sparse descents" from what it already computes
(the reach probe prints `178 descents in 16384 -> sparse, decline`; `inv_all` is 0.000 because adjacent swaps are
invisible to strided sampling) and run that shape sequentially at any P. The risk is a false positive on data that
parallelises well, so the gate has to be measured on the near-sorted family, not just on kinks.

### 6. Eight concatenated logs: the engine is 1.25x behind a plain merge
**From:** the handoff round. **Known:** traced to two causes — the knot route declines at its few-runs gate (8 runs each
spanning the range, crossing cost 3,030 = 3.0n against a gate of 1.0n) and then the SEAM route misfires, reading the
eight log boundaries as a ring buffer's single drop and rotating the whole array for nothing (18.2 ms of an 18.3 ms
sort). **Next:** the gate value wants its own A/B against interleaved runs (open direction 0m (h)); the seam gate wants
a guard that the medians show ONE drop, not eight.

---

## SPECULATIVE — raised 2026-09-22, nothing measured yet

Free-association, kept because each one is anchored to a number this repo already has. None of these has been built;
every one carries a prediction so it can be graded, and the cheapest test that would kill it.

### S1. The threads are not equal: work-queue the phases that chunk statically  — SETTLED 2026-09-22, SPLIT VERDICT
**The observation it explains.** This box is an i9-13900H: 6 performance cores and 8 efficient ones. The engine hands
every thread an equal slice (`lo = t*n/P`), so on a hybrid CPU the fast cores finish and EXIT while the slow ones run on
— and a phase's wall is set by the slowest. That is exactly the profile's unexplained shape: 6.0-6.9 of 14 threads busy
inside the lattice map with **none waiting** (a finished thread is neither busy nor waiting), and 6.6 busy with 3.6
waiting in relocation's static tiles. Roughly 6 of 14 busy is what "6 fast cores carry it" looks like.
**The change.** Chunk the map, the counting emit and relocation's placement into ~4P blocks claimed from an atomic
counter, the way relocation's chain merges and the histogram-free partition already do.
**Prediction:** quantized and mod_card_30k 1.15-1.40x at 14 threads and 1.00 at one; relocation's block shapes 1.10-1.25x;
guards 0.98-1.02. If the hybrid explanation is wrong the gain will be under 1.05 and this dies cheaply.
**Cheapest test before building:** time the map phase per thread (the profiler already stores per-thread busy) and look
for a bimodal distribution — 6 threads at one speed, 8 at another. One trace run answers it.

**THE KILL TEST PASSED, from profile data already on disk** — no new run needed: per-thread busy inside the statically
chunked phases is bimodal (quantized lattice map 4.40 vs 2.99 ms, 1.47x; mod_card 1.44; few_distinct hash map 2.09;
relocation tiles 1.39) against the queue-driven leaf at 1.04. So both sites were built.

**Lattice MAP phase: SHIPPED, ~1.05x.** Quantized and mod_card_30k 0.90-0.97 on 12 of 12 cells at 4M and up, two runs
of 60 pairs, against a null switch at 0.97-1.04. `LOGOS_LAT_STATIC_MAP=1` restores the slices. Predicted 0.75-0.90 —
too optimistic by about half.

**Relocation output tiles: REVERTED, 0.96-1.04 on every cell of two runs.** Predicted 0.88-0.97 at 4M+. Correct
throughout (harness both settings; 1,926 injected allocation failures and 48 thread-creation failures, 0 bad, reaching
`reloc_worker` once the knot route is off) and worth nothing.

**The contrast is the finding.** The lattice map is COMPUTE into a private per-thread table, so a faster core really
finishes more elements. Relocation phases 1 and 3 are `memcpy` over disjoint ranges, bound by one shared memory
controller, so giving a fast core extra tiles cannot raise the aggregate. **A work queue buys balance only where the
CORES are the bottleneck.** Corollary, without another round: the knot worker A1 and C phases — the same static tiles
over the same kind of copy — are not worth converting.

**Where it leaves the real target:** if equal tiles change nothing, the 3.6 threads `make profile` shows waiting are
not waiting for a slow tile. They are at the SECOND barrier, where phase 2 merges overlapping chains — already
queue-driven, but one long chain is a single indivisible task. That is relocation's serial middle, and it needs a
merge-path split of ONE chain.

### S2. Sorting networks at the small end — BUILT AND SHIPPED 2026-09-22
Batcher networks for 4 to 16, generated and proved by the 0-1 principle, taken by the dispatcher's tiny case and the
quicksort base case. **Measured** (`make callbench`, copies swapped, two runs): 1.03-1.06x at n = 4, 1.22x at 8, 1.33x
at 10, 1.39x at 14, **1.57-1.58x at 16**, monotone in n; n = 18 and 24 read 0.99 (control), sorted rows 0.97-1.04,
1000-50000 read 0.98-1.02. A SMALL-END result (direction 0g), not one at scale.
**Graded:** predicted 1.5-3x across the whole range — right at 16, wrong at 4, because the effect is the ratio of
comparators to mispredicted branches and grows with n. The stop rule inherited the same flaw and is met only from
n = 8; shipped anyway since nothing regresses and every in-range size improves in both runs and both orientations.
**Not done:** a merge of two networks to cover 17-32, and the int32 twin.

### S3. Sort 32-bit key prefixes, resolve the collisions — KILL-TESTED 2026-09-22, and the test itself was wrong
The idea: sort on the top 32 bits of the total-order key, then fix only the groups whose prefixes tie. The pre-written
kill criterion was "if under 90% of elements are separated, it is dead before any code".

**Measured** (2M elements per shape, the real total-order key, every prefix group examined): separated fractions are
uniform 55.5%, heavy_tail 53.5%, int_wide 73.9%, bimodal 36.8%, clustered 0%, few_distinct 0%, quantized 0%. By the
criterion as written, dead on every shape.

**But the criterion was the wrong question, and the second reading says so.** A tie between EQUAL values costs nothing
to resolve. Splitting the ties that way:

| shape | unique | max REAL tie | in real ties | free (duplicates) |
|---|---|---|---|---|
| uniform | 55.5% | 9 | 44.5% | 0% |
| heavy_tail | 53.5% | 10 | 46.5% | 0% |
| int_wide | 73.9% | 7 | 26.1% | 0.03% |
| bimodal | 36.8% | **512,897** | 63.2% | 0% |
| clustered | 0% | **10,274** | 100% | 0% |
| few_distinct | 0% | 1 | 0% | **100%** |
| quantized | 0% | 1 | 0% | **100%** |

So `few_distinct` and `quantized` read 0% separated only because their ties are duplicates — nothing to resolve — and
on `uniform`, `heavy_tail` and `int_wide` the real tie groups are at most **7 to 10 elements**, which an insertion pass
closes. A 32-bit prefix separates those three well. It collapses only where values are clustered inside one binade
(`clustered`: 2M distinct values over ~105 prefixes; `bimodal`: one group of 512,897), because the top 32 bits of a
double are sign + exponent + 20 mantissa bits, so inside one binade they resolve to about 1e-6 of the magnitude.

**Verdict: closed anyway, on the mechanism rather than on separation.** To sort by a 32-bit prefix you must carry the
element: prefix + 32-bit index is 8 bytes, exactly what the double already weighs, and it adds a random-access gather
at the end; prefix + the double is 12, which is worse. Nothing is halved. The idea survives only as an MSD radix that
stops after four bytes, which is a leaf change, not a key change — logged as S3b below.
**The lesson worth keeping:** the kill criterion counted separated ELEMENTS when the thing that costs is the size of
the groups that are not separated. A test can be written before the measurement and still ask the wrong question.

### S3b. Stop the leaf's radix after the top four bytes (what survives of S3)
The measurement above says the unstructured shapes' real tie groups are 7-10 elements. An MSD radix over the top four
key bytes would leave groups that size and finish them with insertion, instead of running byte passes over all eight.
**Prediction:** 1.1-1.25x on the f64 radix leaf where it runs, 1.00 where the AVX2 quicksort takes the leaf instead
(which is the sequential unstructured case, so the prize is the PARALLEL leaf), 0.9-1.0 on clustered and bimodal, which
need the gate. **Test before building:** trace which leaf actually runs on each shape at 14 threads — if the radix leaf
is rare for f64, this is worth nothing and dies there.

### S4. Give the counting routes a run-length deliverable
On few_distinct and quantized the sorted output is a tiny RLE, and the route already computes it: the emit phase exists
only to materialise the array (~3 ms of a ~17 ms sort). `logos_sort_rle` would return (value, count) pairs and stop.
**Prediction:** 5-50x for a consumer that accepts RLE on low-cardinality data; irrelevant to array consumers. It is the
project's strongest thesis ("change the deliverable") applied to the routes that already win.
**Test:** none needed to estimate — the emit's share is already traced; the question is whether a caller wants it.

### S5. Stream sorted runs to the consumer instead of returning an array
Most sorts feed a scan, a join or an aggregation. An entry that yields sorted RUNS as they finish would skip the
copy-back (worth 1.21-1.35x on block shapes, already measured via `nocopy`) and let the consumer overlap with the sort.
**Prediction:** 1.2-1.4x for streaming consumers, 1.00 for array consumers. **Test:** a bench that sums the sorted array
through both paths.

### S6. Speculate the partition while the probe runs — CLOSED 2026-09-22, on a dependency, not a measurement
It cannot be built as written. The partition's scatter needs its splitters; the splitters are quantiles of the SORTED
sample; the sorted sample IS the probe. There is no partition work that can start before the probe finishes, so there
is nothing to speculate on. The only work that is genuinely independent is touching the input, and that is already
bandwidth the sort will spend anyway.
**What survives:** the probe's own serial cost is ~0.9 ms of a parallel call at 8M (FINDINGS §3o) and it is not
threaded, on purpose, because a spawn costs what the sample costs. That is the real target, and it is S7's target too.
**The lesson:** "run A while B runs" needs A and B to be independent. Check the data dependency before predicting a
speedup from overlap.

### S7. Stop sampling when the decision is settled — PARKED 2026-09-22, the prize is under the instrument
The sample is 1-4% of a parallel call and an early stop can recover at most part of that, so the effect is 0.5-2% at 8M.
The paired harness resolves ~3.7% at 8M over 40 pairs and error falls as 1/sqrt(pairs), so telling 1% from nothing needs
roughly 550 pairs per cell — about fifteen times the usual run, per shape, and that is before the layout term. **An
effect that cannot be measured must not be shipped**, so this waits for either a cheaper instrument or a reason to
believe it is much larger than 2%. Its one real lead is the same as S6's: the probe is serial and deliberately
unthreaded.
**The lesson worth keeping:** decide whether the bench can SEE an effect before building it. That is a cheaper kill test
than any code.

### S8. Choose the leaf per BUCKET, not per thread — REWRITTEN 2026-09-22: its premise was stale
As written it said "the partition could hand each bucket the byte histogram it already touched". **There is no such
histogram any more.** The partition has been histogram-free since 2026-09-11 (FINDINGS §3m): it sizes regions from the
sample and scatters without counting. So the idea as stated proposes reusing a phase that was deleted, and would have to
ADD a per-bucket pass that the leaf currently does not pay.
**What survives, and is still worth a measurement:** the leaf already chooses its branch policy per THREAD from the
first trial chunks. Choosing per BUCKET costs nothing extra if the decision comes from the bucket's own first chunk
rather than from a new pass. **Prediction:** 1.03-1.10x on mixed-skew shapes (heavy_tail, bimodal), 1.00 on uniform;
at the low end of that it is inside the floor and dies. **Test first:** trace how often one thread's committed policy
disagrees with what a bucket would have chosen -- if the disagreement is under ~20% of buckets, the ceiling is too
small to chase.
**The lesson:** an idea written against the engine of six weeks ago can name a phase that no longer exists. Re-read the
code before predicting from it.

---

## PARKED

- **Machine-learning filters** (raised 2026-09-22, the message was cut off after the heading). Three classifiers already
  exist here, all measured, none wired in: the (index, value) quadtree profile (`make zroute`, blind on 0 of the 3 cases
  where the engine leaves 1.2x or more), writhe + persistence (`./strandprobe 0 8388608 writhe`: 0.33 on noise against
  0.95-1.00 on anything stranded, 16 of 16 with the rule fixed in advance), and the LIS classifier (`./unknotprobe ...
  lis`). Parked until the filter's JOB is stated: route selection, gate admission, or something else. Note that idea 3
  above is the non-statistical version of the same question, and it comes with a ready-made scoring harness.
- **GPU / SIMT port of the counting and radix paths** — the one hardware where scatter is cheap. Cannot be developed on
  this box.
- **NUMA-aware partition** — only measurable on multi-socket hardware.

---

## CLOSED — settled this session, kept so they are not re-run

| idea | verdict | where |
|---|---|---|
| The triple-reversal weld (condense, meta-sort by min edge, in-place rotations, seam weld behind an O(1) check) | **LOST**: 2-4,600x behind the engine; the O(1) check bypassed 0.0% of seams on every shape | CHANGELOG, FINDINGS §3ar, ledger |
| The handed-off Moebius hybrid (bail out under 16, swap 2-3 element dips, galloping stitch) | **LOST**: behind on 14 of 16 shapes; the micro-kink branch fires 0 times on the shape it targets | FINDINGS §3as, ledger |
| "Uniform data as a complete computation": SIMD chunk sorts + a merge-path tree | **LOST**: 3.3-4.2x behind, though the chunk sorts ALONE beat the engine (0.76-0.86); the merge costs 5.8x what moving the same bytes costs | FINDINGS §3at, `make unibench` |
| A thread ceiling for the counting routes | **LOST**: no global value passes; a hash-only ceiling reads 1.0-1.1 | FINDINGS §3au, `make capbench` |
| Gating the near-sorted route off at low thread counts | **LOST**: the route is worth 1.80-1.97x at two threads on its own shapes | FINDINGS §3au |
| KMT-style geometric reduction before the topological check | **LOST as such**: the analogue of KMT's expensive step (the plan) is already **0.2-1.1%** of the sort, and the element-level reduction is already shipped (the sweep merges only what crosses: 825,678 of 8,388,608 elements on 64 overlapping blocks) | this file, traced 2026-09-22 |
| The counting routes' serial merge in parallel | **LOST**: 1.02-1.12x slower; the merge is ~10% of the route (the trace), not the ~55% a thread-occupancy average implied | FINDINGS §3av, ledger |
| Each lattice worker allocates its own count table | **WON**, ~1.03x on the lattice shapes, 20 of 20 cells against a measured null | FINDINGS §3av, B109 |
| S2, branchless sorting networks for 4-16 elements | **WON**, 1.03x at n = 4 rising to 1.58x at n = 16, two runs, both orientations, controls flat outside the range | FINDINGS 3ax, B111 |
| S1, the hybrid-CPU work queue, on the lattice MAP phase | **WON**, ~1.05x: 0.90-0.97 on 12 of 12 cells at 4M+, two runs, against a measured null | FINDINGS 3aw, B110 |
| S1, the same work queue on relocation's output tiles | **LOST**: 0.96-1.04 on every cell of two runs, against a prediction of 0.88-0.97. A queue buys balance only where the CORES are the bottleneck, and a memcpy phase is bound by the memory controller | FINDINGS 3aw, C80, ledger |
| A 32-bit key prefix (S3) | **CLOSED on the mechanism**, after its own pre-written kill criterion fired for the wrong reason: prefix + index is 8 bytes, which is what a double already weighs | this file, C79, ledger |
| Speculating the partition during the probe (S6) | **CLOSED on a dependency**: the splitters need the sorted sample, and the sorted sample IS the probe | this file |
| Three defects found by losing to the edge sort; two by losing to the weld; one by losing to the handoff | **WON**, all shipped | CLAUDE.md positives |
