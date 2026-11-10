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
**SETTLED 2026-09-26: BUILT AND SHIPPED, and it was not "run it sequentially".** The reach probe now counts kink-shaped descents and
declines with `LOGOS_REACH_DECLINED_KINKS`; the P > 1 dispatcher skips near-sorted on that verdict; and every detector worker lifts
strays (the gate alone lost 0.91 at 14 threads because the list ran at P = 1 only). Kinks row 2.41 / 2.36 / 3.04 at P = 2 / 4 / 14,
guards 0.97-1.03; one strand + 0.5% kinks 2.49x at 14 threads OVER one thread, so the shape now wants the cores. Left: the 0.05%
cell (`REACH_KINK_MIN` = 8 against ~7.9 expected descents). CHANGELOG, FINDINGS 3bb.

### 6. Eight concatenated logs: the engine is 1.25x behind a plain merge
**From:** the handoff round. **Known:** traced to two causes — the knot route declines at its few-runs gate (8 runs each
spanning the range, crossing cost 3,030 = 3.0n against a gate of 1.0n) and then the SEAM route misfires, reading the
eight log boundaries as a ring buffer's single drop and rotating the whole array for nothing (18.2 ms of an 18.3 ms
sort). **Next:** the gate value wants its own A/B against interleaved runs (open direction 0m (h)); the seam gate wants
a guard that the medians show ONE drop, not eight.

### 7. The two-thread hybrid's serial NaN/-0.0 scan (raised 2026-10-05, FINDINGS 3bi)
**From:** pricing the profile phase against x86-simd-sort (`make frontprobe`). **Known:** at one thread the scan now rides
on the kernel's first partition and costs nothing; on the optional build's two-thread half (`-DLOGOS_XSS_LEAF`, P <=
`XSS_PAR_MAX`) it still runs serially in front of the library's parallel quicksort, measured at **1.072-1.079** of that path
(the scan-plus-library against the library alone, 40 pairs, 8.4M). The default engine is unaffected (its two-thread path is
the partition). **Two forms:** (a) a top partition here with the flag on it (`av_partition_fl`) and the two parts sorted on
two threads by the one-thread library call -- the whole prize, but a static split replaces the library's task balancing, so
the pivot wants a large sample (a 1023-point median keeps the split within ~1.6%); (b) the scan itself split over the P
threads, about half the prize and no change to who sorts what. **Next:** build (a) behind a switch and pair it with
`./frontprobe 40 8388608 2 "" ab` (FP_SWITCH = that switch), plus the eight-concatenated-logs and noisy-ring guards the
two-thread gate was set from.

### 8. The scoreboard's remaining one-thread losses (raised 2026-10-07, FINDINGS 3bj, `make scoreboard`)
**SETTLED 2026-10-08 (FINDINGS 3bk): (a) BUILT, 2.57x on the row (predicted 1.5); (b) MEASURED SLOWER (0.83), stays opt-in.**
**(a) 256 distinct values, 0.845 of x86-simd-sort.** The lattice route's map is a scalar round, tolerance check, bounds check and
key compare per element: 40 of 46 ms in a trace. Vectorise the index (`_mm256_round_pd`, a vector tolerance and bounds test), gather
or compare the slot keys, keep only the increments scalar; predicted 2-3x on the map, about 1.5x on the row, and the quantized row
with it. **(b) int_wide, 0.916.** The int32 radix (11.07 ns) against a vector quicksort (10.14): `-DLOGOS_AVX2_INT32` routes such
buckets to the in-house kernel and has not been measured since the kernel bridge (FINDINGS 3bh); with the library leaf it would be
the library. A/B it first. **(c) Two values, 0.93.** Count + streamed emit now moves the same bytes as the library's one partition
pass; parity is the ceiling for this design, so leave it unless a one-pass two-value partition is cheap to try.
**Also seen, not chased:** a `-DLOGOS_TRACE` build puts 0.6-4.9 ms between entry and "prescan done" on random data at 14 threads,
with or without the parallel prescan: probably the trace build itself, but price the dispatcher's entry in a normal build once.

### 9. Leads from the eleven-trick round (raised 2026-10-08, FINDINGS 3bk)
**(a) Sorted calls of 1,000 elements cost 1.09-1.12 against the COMMITTED engine** (`make callbench`, both orientations, two runs),
bisected to the uncommitted changes that predate 2026-10-05 (HEAD -> that working tree: 1.094; everything since: 0.98-1.03). About
30-50 ns on a ~550 ns call; sorted 5,000 reads 1.02, so it is not a fixed cost. **Next:** narrow callbench's sizes to 300-3000 and
bisect inside that diff. **(b) A lattice of 30,000 negative values at 300,000 elements never takes the lattice route** (the harness
row had to narrow to 4,000 values to reach it): read which route takes it before assuming the gate is right. **(c) The in-place
partition's 25-38%** is one block-permutation pass; IPS4o hides it by recursing in place, the engine's leaves are one level, so
the pass is not amortised. A two-level variant (permute only once the buckets are cache-sized) is the angle, if memory at 14 threads
ever matters more than the opt-in already gives. **(d) The trim's refault** costs 1.3-2.5x on the next large call: a trim that
hands back only what has stayed untouched for N calls, by tracking the high-water of each call rather than a decaying mark, would
pay less often; or the soft form (MADV_FREE) on a box that is not WSL2, where reclaim of lazy-free pages may be lazier.

### 10. Leads from the invented round (raised 2026-10-19, FINDINGS 3bm, `make roundbench`, `make strsmallbench`)
**Settled in the round:** two halves at P = 2 and the sequential leaf in the partition past L2 (both shipped, 1.05-3.0x); small
string groups on keys (shipped); the log-line route (shipped, 2.1-2.3x on log lines); placements stream (shipped, 1.05-1.09);
`REACH_KINK_MIN` 3 (shipped); radix in the cache for int32 (neutral, reverted); the string sort's top level fanned out (slower,
reverted). Leads, each with the measurement that would settle it:
**(a) A work queue for the string sort's static-sliced phases** (`str_keys_worker` and `str_part_worker` phases 0 and 1 each take
n/P): capping the TOP level at six threads read 1.05-1.35x at 300-450k strings and lost from 1M, and six is this box's P-core count,
which is S1's signature (a static slice ends when the slowest core does). Chunks of n/4P claimed with one atomic, per-CHUNK counts for
the scatter. Predicted 1.05-1.3x at 300k-1M with 14 threads, 1.00 at one. Kill test first: per-thread busy time of those phases in a
profile build (bimodal across P- and E-cores, as the lattice map's was, or not).
**(b) The log-line route at P threads.** It is one thread (the keep pass is a dependency chain); 2M log lines at 14 threads take ~29 ms
on the radix. Chunk-local keep passes, all strays sorted by the parallel radix, then a merge-path split of P + 1 sorted sequences.
Predicted 1.3-1.8x at 14 threads; the merge with strcmp is the cost to price first (time one P + 1 way merge of the kept chunks).
**(c) Quarters at three and four threads.** The halves route's split generalises (k - 1 pivots, sequential leaves), but the
sequential leaf in the partition already took most of P = 3 and 4 (1.12-2.09x). What is left is the partition's scatter against a
split; price it with a profile build at P = 4 before building anything. Also not traced: normal at P = 4 read 0.98 with the
sequential leaf where P = 3 read 1.37.
**(d) The kink probe's blind spot.** 16,384 adjacent pairs see ~3 descents at 0.02% kinks and under one at 0.005%: below that density
no minimum helps. When four windows show one or two kink-shaped descents, read four more before declining. Measure first how often a
strand of 0.005-0.02% kinks reaches near-sorted at P > 1 (kgverdict at those densities).
**(e) At P = 2 the key grid still needs 1,024 sample points and gets 768.** Moot for unstructured data (the halves route takes it), but
an ordered or repeating array that the halves gate declines still pays the table scatter there. Oversample to 1,024 at P = 2, paired on
the guards that the gate declines (noisy ring, concatenated logs, quantized at 250k).

### 11. Leads from the three invariant tests (raised 2026-10-20, FINDINGS 3bn, `make orderprobe`, `make symprobe`, `make edgeprobe`)
**Settled in the round:** the run-length route, the cuts route, the tail merge's forms, the head route, the quantization probe on
Weyl positions, the quantized float radix off, the thin front to 2047, the 2048-65536 window by local order (all shipped); the
vector quicksort's order cost diagnosed (the library pays it too, same depth). Leads, each with the measurement that would settle it:
**(a) The one-to-two-thread edge at 14 threads (edgeprobe, 200,000).** Blocks 2.46x, kinks 3.11x, runs of 16 1.76x and a 95% prefix
1.49x dearer per element at two threads than at one, four separate mechanisms: relocation's floor is 4096 at P > 1 against 256 at one
thread (blocks of 3125 declined); the kink verdict needs the reach probe, which starts at 2^18; the tail route's parallel form loses to
its sequential one at P = 2; the run-length route is offered to sequential sorts only. Near-sorted, sine, coarse windows and streams
EARN their second thread (0.32-0.81), so no rule of the form "structured at P = 2 runs sequentially" -- four A/Bs at 200k-400k, P = 2
and 3: the floor at P = 2, the kink verdict below 2^18, the tail's sequential form at P = 2, the run-length route at P <= 2.
**(b) The shared sample at 4096 (+25-66% per element on most shapes for one element more).** 2048 points of 4096 elements is half the
array. The structured routes earn there (prefix 0.34, halves 0.61, few distinct 0.52), so the lever is a sample proportional to n at
the small end (min(2048, n/16)?), not a higher threshold. Ladder the sample size at 4096-32768 on the edge shapes.
**(c) The probes at 65536 (+11% on uniform for one element more).** skew_probe reads 8192 random positions, the run-length probe 16k,
plus the dense and stream gates. Time each probe alone at 65536 (a -DLOGOS_TRACE build says which run; a normal build times them).
**(d) coarse at 500,000 (sine 1.44x, coarse64 1.58x at one thread).** The leaf's flash -> coarse switch for ranges the probe calls
nearly sorted and dense; the cuts route now takes coarse windows from 2^20, so what is left is 500k-1M and the sine. Pair coarse
against the AVX2 kernel on dense nearly ranges between 500k and 1M.
**(e) momentum at 50,000 (near-sorted 0.72 above it, i.e. the momentum insertion at <= 50,000 costs 1.38x what the path above it
costs).** Ladder MOMENTUM_THRESHOLD on near-sorted and kinked ranges.
**(f) A 30,000-value lattice at 32768 elements loses 0.74 to the new window gate** (flash's classes match the lattice). A lattice test
in the window probe would cost compares on every range; worth it only if lattices of ~1 value per element are common at that size.
**(g) The 14-thread halves of orderprobe and symprobe were unreadable today** (p10-p90 0.5-2.1). Re-run them with 40+ pairs on a quiet
box: they are the only invariant data the round has at P > 1.
**(h) The AVX2 order cost's hardware mechanism.** Same depth, same cost in the library: per level, on locally ordered input. Without
counters here (`make pmuprobe`: none open under WSL2) the candidates (store-to-load forwarding near the write fronts, 4K aliasing, the
compress-store's partial lines) cannot be told apart; re-run ordkern on a machine with counters.

### 12. Leads from the other-fields round (raised 2026-10-21, FINDINGS 3bo, experiments/OTHER_FIELDS.md, `make fieldprobe`)
**Settled in the round:** founder windows, the k-way merge in vectors, the stray cap at one thread, the short-reach threshold, the
cuts route's dense decline, the low-discrepancy coverage band, the detector's checkpoint minimum (all shipped); hub and spoke (lost),
the AVX2 kernel for int32 on distinct inputs (lost), TDM demux and patience piles (ceilings, not built), 4K aliasing between the
array and its scratch and code alignment (both nil). Leads, each with the measurement that would settle it:
**(a) Clustered values on long legs at 1M** still sort 2.3-2.9x slower than shuffled (fieldprobe moire, tri_clu from period 4095; at
4M the band's sub-bucket effect removed it). The top level keeps coarse on long legs because coarse wins on CLEAN ones (1.25); what
is missing is a cluster test the leaf can afford (skew_probe's 256 bins see 2048 evenly filled bands as flat). Candidate: the share
of a sorted sample's gaps under 1% of the mean gap. Then route long legs by clustering, not by leg length alone.
**(b) A staged streaming write-back for the run-length route** (the hardware queue's last item): stage the runs in a cache-resident
block, stream it out in aligned 32-byte stores. Predicted 1.05-1.15 on runs of 8-16 at 4M (one write-only pass of ~3.4 ns per
element); the counting emit's 1.21-1.36 is the precedent.
**(c) 3% random swaps at 0.885** (6% displaced): the stray path costs ~13 ns per element there, the leaf ~11.9. A cap that declines
earlier (1/20) is untested; time the stray path's phases at 2-6% first.
**(d) Sixteen distinct values with 4096 huge outliers at the FRONT: 2.2x** against the same outliers scattered (founder test). Trace
it (`./fieldprobe_trace once founder 5 4`).
**(e) The zipper at 20x its information share** (10.3 ns per element for 1 bit): its demux ceiling is 1.90, the one TDM shape worth
a route; a stride-2 probe of contiguous windows is free on random data.
**(f) src8 reads 8.5 or 14.8 ns per element by seed** (weave against landauer, same generator): another gate on a knife edge; the
`seeds` mode first.
**(g) A disordered head of 4096+ elements before a near-sorted strand** (founder rows 0.19): the average-run rule ends the scan in
the head. A head route for "unsorted head + near-sorted suffix" would need the suffix's own scan.
**(h) The dispatcher at 4096-8192 elements**: 1.16-1.38x the bare kernel on unstructured data (`./fieldprobe small`). The sample at
one thread is already 384 points; time the gates one by one before touching it.

### 13. Leads from the memory round (raised 2026-10-22, FINDINGS 3bp, `make rssprobe`)
**Settled in the round:** one scratch array for the LSD radixes; the dominant-value route; release returns the heap (all shipped).
What still holds a full copy, and the measurement that would decide each:
**(a) The route's guard rows are owed** (`FP_SWITCH=LOGOS_NO_DOMINANT ./fieldprobe ab`, `RB_SWITCH=... ./roundbench` at 1 and 14
threads): taken under another session's CPU load, they read 0.64-1.16 on shapes the route cannot touch.
**(b) Dominance below 20% at 14 threads** still costs 1.5-1.8x uniform (share 0.05-0.1: 2.4-3.3 ns per element against 1.6-1.9), and
the route has nothing to gain there because the rest is most of the array. The cause is upstream: the histogram-free partition declines
when one value exceeds a region and the histogram path gives that value one bucket. An EQUALITY bucket in the partition (IPS4o's
answer) would take every share; measure it against the route on 5-90%.
**(c) The structured routes' one full copy at one thread** (8.1 bytes per element: relocation, the knot route, the tail and head
merges, strays). A half-size buffer exists for two-run merges (copy the smaller run out, merge one-chain into place) at the one-chain
vector kernel's speed (1.14-1.29x over scalar against 1.96-2.28x): price it as an opt-in, `LOGOS_LOWMEM`.
**(d) The float32 path** (8.1 at one thread) keeps two arrays because a decline must leave the input untouched. A read-only
verification pass first would allow in-place narrowing for one more pass over the input: measure the pass against the 4 bytes saved.
**(e) Release after two inner sorts** still leaves 0.6-0.9 bytes per element (shares 0.25 and 0.5) where one sort leaves 0.0-0.4: what
malloc_trim does not reach (thread arenas, the pool's bookkeeping). `MEMPROBE_LOG`-style allocation logging names it.
**(f) A single memory switch** for callers who want the low-memory forms together (the in-place partition, the pool trim, (c)):
today they are three environment variables and their speed prices are in three ledger rows.

### 14. Leads from the second memory round (raised 2026-10-24, FINDINGS 3bq, `make rssprobe`, `make mixprobe`)
**Settled in the round:** half a scratch for one-thread placements (13 (c)); the float32 keys in place (13 (d)); the regions in the
pool's block; nested sorts rewind the bundle cursor; `LOGOS_LOWMEM=1` (13 (f)); the dominant route's guard rows (13 (a): neutral).
What still holds a full copy, and the measurement that would decide each:
**(a) The parallel placing routes** (8.0-8.6 at 14 threads: near-sorted, relocation and the knot route, the tail merge). Half a
scratch needs step 2 of half_place across threads: the gap copies are independent once planned (their destinations exclude every
source), so they parallelise; the SLIDE does not (a dependency chain in address order). Price the gap plan's success rate on the
parallel shapes first, then the copies at 14 threads against the placement they sit inside (bound by memory traffic there, 0f).
**(b) The 14-thread partition** (8.8): structural (the scatter reads everything before anything is written back). The in-place form
is opt-in at 1.25-1.38x; a partition with HALF its regions (scatter the first half of the input, then the second into the freed
half, leaves gathering two parts per bucket) would need exact counts for the second half: one more read pass, and a dependency
order between leaves. Unmeasured; its prediction would have to beat the in-place form's price.
**(c) What logos_release_buffers() leaves** (0.5-2 bytes per element in one process, growing less than linearly in n: 2.3 MB at 1M,
5.8 at 4M, 8.5 at 16M). Heap pages malloc_trim does not return. An allocation log over a mixed sequence names it.
**(d) The head route still slides**: its suffix's upper part fits neither gap the lower half leaves. A gap plan that may SPLIT a
copy job's source across gaps (a copy has no reason to stay whole) would cover blocks of unequal size; a merge job's pieces must
stay whole. Measure how often half_fit falls back on the bench shapes before building it.

### 15. Leads from the comparison-bound round (raised 2026-10-31, FINDINGS 3br, `make boundprobe`)
**Settled in the round:** the lattice entry for a lattice the sample does not see repeat, the divided step, spills (all shipped);
hashing arbitrary doubles past L2 (a told ceiling, a negative). What `boundprobe` still shows above the bound it could reach:
**(a) The hash window.** Counting k distinct arbitrary doubles through a one-multiply hash table beat the vector quicksort at 16M and
one thread for k = 256 (1.70) and 1,024 (1.20), and lost from 4,096 (0.73) and at 1M nearly everywhere. A route gated on the
sample's Chao1 in [256, 2,048] and n >= ~8M, sequential only: the told ceiling is the prize (1.2-1.7x), so ladder k and n with the gate
lifted before setting it, and price the decline (one wasted map pass) on 4,096-16,384 values.
**(b) A step estimate that tolerates a few off-lattice sample points.** Today the step must hold EVERY sample point, so one
off-lattice value in the 2,048-point sample (a late batch, 0.1% noise) gives a lattice five times finer, which is declined, and the
spills never run: the off-lattice rows at 8 and 14 threads read 0.97-1.05 where one thread reads 2.05-3.66. Take the step from the
points that agree (the gap's divisor that holds all but at most m/64), and let the map spill the rest: predicted 1.5-3x on those rows
at 8 and 14 threads, nothing elsewhere; the decline cost on data that is NOT a lattice is what to measure first.
**(c) The fill by thread count is a formula** (16 (P + 1) / 2), set from one and four threads. Ladder it at 2, 3, 6 and 8 on lattices
of 30k-250k values.
**(d) Held-out shapes.** The entry was set on lattices of step 0.25, integers and cents. A lattice of step 1/3 (not exact in binary),
timestamps on a 1 ms lattice with an offset, prices with a few unit misprints: does the step search find them, and do the spills keep
the route?
**(e) The bound itself for the rows still ON it:** random doubles with repeats sit at 0.37-0.57 ns per bit for every k; nothing
compares cheaper. The only way under is not to compare, which is (a) for k <= 2,048 and nothing known beyond it.

### 16. Leads from the X+Y test (raised 2026-11-02, FINDINGS 3bs, `make xyprobe`)
**Settled:** the engine is blind to X+Y (raw = shuffled, 1.00-1.04); a sort TOLD X and Y places the top log2 B bits by staircase
segments and wins only with long segments (S = 262144: 0.86 at 16M, flat growth +1.2%), loses at S = 32768 (1.10) and S = 4096 (1.98).
**(a) n = 16,384 (N = 268M).** Does the told S = 262144 lead widen to ~1.3x as predicted (engine +18% per 16x, told flat, the n B
term 0.11 -> ~0.4 ns per element)? The only measurement that separates "linear in range" from the asymptote.
**(b) Many overlapping ascending runs cost the vector quicksort 4-14%** (xy_rows / xy_shuf, 1,024-4,096 runs of n). Add the shape to
orderprobe; 3bn says the lever is a route that keeps such orders away from the kernel, and none of today's run routes takes R ~ sqrt(N).
**(c) The marginal price of a comparison bit is ~0.2-0.4 ns** in the engine (buckets of 4k / 262k / 16M at 8.2 / 9.0 / 11.2 ns per
element), against the 0.37-0.57 AVERAGE boundprobe reports. Any route that removes bits by an extra pass must beat that margin, not
the average: price it per removed bit before building.

### 17. Leads from the prime-factor knot solver (raised 2026-11-05, FINDINGS 3bv, `make primeprobe`)
**Settled as a standalone solver:** pinned arcs (elements already in their final place, two binary searches per run) left alone and
the factors between them untied in place: one thread 1.2-2.8x at 8.4M where strands lie mostly in order, 2.1-3.1x over the knot
route's parallel form at 14 where half or more is settled; nothing where strands are shuffled (one factor); the 14-thread form does not hold at 1M in OpenMP.
**(a) DONE 2026-11-06 (the strand gate, FINDINGS 3bw, `make strandgate`): the near-sorted route's claim on overlapping strands at P > 1.** An
engine defect, measured: an in-order staircase at overlap 0.2, swapped neighbours and late batches cost 1.3-1.9x MORE at 14 threads than
at one (8.4M), up to 3x at 1M, and 262k-element regions of two or three overlapping runs 12.2 ns per element against 2.2 at one thread;
with `LOGOS_NO_NEARSORTED=1` the same inputs read 0.77-0.88 ns, i.e. 1.6-3.9x at 8.4M and up to 5.2x at 1M by a route decision alone. The
route set about a tenth aside as strays (sample inv = 0.000: its statistics cannot see block-scale overlap). THE FIX: the windows its
pre-gate already reads count adjacent descents (the element's scale); when they hold at most 4 and a 2048-point sample in position
order holds 4 or more descents (the array's scale), the route declines. 14 threads: 1.67-3.87x at 8.4M on six strand shapes (two runs),
1.6-3.3x at 1M, 2.1-5.3x at 2 and 4 threads; guards 0.95-1.07. One cell at or under 1: 16 runs of 65k at 1M (0.86-0.98, p90 past 1).
**(b) NEXT: pinned arcs in the knot route's plan.** The detector's run list is already in hand, so the decomposition is microseconds; lone
pieces whose destination is their source are skipped and each factor is placed in its own span. Predicted from the standalone solver:
one thread 1.2-2.8x on in-order shapes, guards 1.00 (no second scan). At P threads factors as tasks on the engine's pool: unmeasured,
and the 1M rows say the fixed cost decides there. Needs: harness rows that name the path, faultprobe on the factor buffer, callbench.
**(c) The gate at a quarter settled is a guess** between 1.6% (bails) and 34% (wins 1.17-1.40): ladder the overlap from 0.5 to 1.0.
**(d) Complex factors** (a late batch drags several strands into one factor) took one thread each, one after another: the late-batches
row is level with the knot route at 14 threads (0.87). Integrated, they would be queued with the simple ones.

---

### 18. Leads from the ladder to 316M (raised 2026-11-08, FINDINGS 3bx, `LOGOS_MAX_N=3.16e8 make scale-full`)
**From:** the first scale run past 10M on this box (316M numeric, 100M strings, quiet: calibration 3.23 / 66.8 ms). Three
inputs turned between 10M and the top, none of them visible at 10M.
- **(a) The short-reach gate's ceiling in n.** **Maps to:** `reach_from_sample`'s Fibonacci-lag ladder over the shared
  sample (lags 0.31% down to 0.028% of n) and `mirror_gate`, which asks the same ladder on the mirrored sample.
  **Known:** reach_mirror 7.30 ns per element at 10M and **20.28 at 316M** (x86-simd-sort 10.6 → 12.6): the finest lag is
  ~88,000 positions at 316M against a displacement of ±60, so the route declines, the mirror behind it never fires, and the
  leaf pays the vector quicksort's order cost (3bn) on top. Open direction 0i (d) predicted ~117M before any run reached it.
  **Next:** one rung measured INSIDE the probe windows (adjacent pairs at a fixed small lag, independent of n), gated so
  that random data still pays nothing; `make reachbench` at 100M and 316M before and after, `make orderprobe` as the guard.
  Status: **OPEN**; the upside is the whole 3.3× (20.3 → ~6 ns per element) on back-to-front logs at scale.
- **(b) The log-line string route past 10M.** **Maps to:** `str_ns_try` (one thread; `LOGOS_STR_NO_NS=1`). **Known:** logs
  26.7 ns per string at 10M and **70.9 at 100M** (the key-value sort 60.1 → 65.5): 2.26× ahead → 0.92 behind. Untraced: it
  may decline at 100M (and the prefix sort then pays for a near order it cannot use), or run and lose (its stray sort or its
  merge growing faster than n). **Next:** a trace build on 100M log lines, then `make stringbench` at 10M, 30M and 100M with
  the route on and off. Status: **OPEN**.
- **(c) The in-house kernel drifts behind the library's with n.** **Maps to:** `avx2_qsort_f64` (3bh) against
  x86-simd-sort's, one thread, whole array. **Known:** uniform 11.16 → 13.97 ns per element from 10M to 316M (+25%) against
  11.09 → 12.18 (+10%), the ratio 0.99 → 0.87; bimodal 1.00 → 0.95; heavy_tail and int_wide hold (1.03, 1.04). The kernel
  gap was 1.07–1.12 at 8M. **Next:** `make avkernbench`'s size ladder extended to 32M–256M, kernels alone, both built the
  same way: a gap that grows there is the partition's cache behaviour; one that does not is what runs before the kernel.
  Status: **OPEN**; the upside is ~1.15× on random doubles at one thread past 100M, nothing below.
  **Narrowed 2026-11-08 (3by), for (b):** strings: logs reads 2.11 at 50M and 0.92 at 100M, so the turn lies between those two sizes.

### 19. Leads from the practical shapes (raised 2026-11-08, FINDINGS 3by, `LOGOS_MAX_N=5e7 make scale-full`, `LOGOS_SCALE_SHAPES=...`)
**From:** thirteen practical numeric shapes and three string columns added to the scale bench and run to 50M (quiet box, 2.85 /
56.0 ms). Four engine findings, none visible on the thirteen older shapes. Ranked by measured upside.
- **(a) A value holding 5-6% of the array defeats the parallel partition.** **Maps to:** `hf` (the histogram-free partition declines
  up front when one value would overflow a region), the histogram path it falls to (buckets by value, so that value's copies fill
  ONE bucket), and `dominant_try_f64` (3bp), which takes a value only from 20% at P > 1. **Known:** zip_codes **0.57** and fk_zipf
  **0.61** of IPS⁴o at 50M and 14 threads, 0.48-0.67 at every size from 1M; the trace at 10M: `hf: declined up front, one value is
  6.2% of the data`, then a bucket of 693,266 (12x the mean of 59,523) sorted by one thread on the skewed leaf branch. A Zipf key
  column (the most common grouping column in analytics) lives exactly in the gap: top value 6-8%, top hundred 40%, none over 20%.
  One thread is fine on both (2.47 via the lattice; 0.78 via the leaf). **Next:** the dominant route extended to the heavy hitters:
  every sample mode that would exceed a bucket's share gets a counted slot and is placed by arithmetic (its count from the
  histogram pass, or from a per-thread count in the scatter), the rest goes to the partition as now. Gate from the sample's mode
  shares; A/B at 1M-50M with `roundbench`'s guards and the two shapes; faultprobe for the new allocation. Predicted 1.5-2.2x on
  both at 14 threads (the buckets would then be 59k each and the leaf phase balanced), 0.98-1.02 on every guard. Status: **OPEN**.
- **(b) The one-thread counting gates at 100k-316k.** **Maps to:** the hash-count route's 1-thread gate (the leaf-fallback test,
  3p / `make cardgate`), the lattice entry's fill rule (3br: 16 (P + 1) / 2 elements per slot), and the shared sample's Chao1.
  **Known:** zip_codes 0.18 at 100k and 0.30 at 316k (the hash route on a Chao1 of 1,237 for 42,000 distinct values: 19,048 map
  entries, 26 ns per element against the vector quicksort's 4.7; the lattice takes it from 1M and reads 0.98 there); prices_cents
  0.45 / 0.83 (a lattice of 146,942 slots for 100,000 elements, 0.68 per slot, with spills: 14 ns per element against 6.7);
  latencies_us 0.73 at 100k (39,002 slots for 100k); ages 0.55 at 50k in a one-off (the counting route at 3.6 ns per element
  against 2.0). The fill rule should have refused 0.68 per slot, so either a different entry fired or the rule reads something
  other than n / slots: that is the first thing to read (`boundprobe_trace once 100000 prices`). **Next:** name the entry, then
  one rule for all three: a counting route at one thread must beat the leaf's cost model on n, slots and distinct (the
  cardgate lesson: price the FALLBACK, not the data), A/B'd at 50k-1M on the three shapes with `roundbench`'s guards and
  `edgeprobe` across the gate. Status: **OPEN**; the upside is 2-5x at 100k-316k on counting-shaped columns, which is the
  buyer's block size (bizdev GAPS 6).
- **(c) The lattice entry's span is the sample's span.** **Maps to:** `lat1_gate` (3br). **Known:** prices_cents' lattice fires
  at 1M, 3.16M and 50M (2.4-5.5 ns per element) and declines at 10M and 31.6M (the leaf, 5.9 ns per element, 1.00 against the
  vector quicksort): on a log-normal the slot count follows the sample's largest value, and whether the grid fits the budget
  depends on which tail prices the 384 Weyl points touch. The int32 counting route windows its sample and spills the tail (3w);
  the lattice entry spills values OFF the grid but takes the grid's span from the whole sample. **Next:** window the lattice from
  trimmed quantiles of the sample (the tail into the existing spill list, bounded by n/32), gated so a window that would spill
  more than the budget still declines; A/B on prices_cents, latencies_us (which also pays a wide window) and the lattice guards
  at 1M-50M. Predicted 2.4x on prices_cents at 10M and 31.6M (where it now reads 1.00), neutral elsewhere. Status: **OPEN**.
- **(d) The knot route's one-thread floor against a natural merge.** **Maps to:** the knot route's four passes (scan, plan,
  placement, copy-back; the half-scratch placement of 3bq). **Known:** log_batches at one thread reads 0.33 / 0.42 / 0.90 / 0.50 /
  1.02 / 1.11 against TIMSORT at 100k / 316k / 1M / 3.16M / 10M / 50M: a natural merge sort with galloping merges reads the 64
  in-order batches as runs and merges only the 20% that overlap, 0.7 ns per element at 100k, where the knot route costs a flat
  1.7-1.8 at every size. At 14 threads the knot route is 3.9x over IPS⁴o and timsort has no parallel form. **Next:** this is the
  prime-factor solver's case (17 (b): pinned arcs never moved, the tangles between them untied in place): build it in the knot
  route's plan and A/B on log_batches and `make primeprobe`'s shapes at 100k-50M, one thread. Predicted 1.5-2.5x at one thread
  below 10M, nothing at 14 threads. Status: **OPEN**.

### 20. "The Moebius strip in an array" (asked 2026-11-09; the array form is the shipped twist, FINDINGS §3ag; the list forms lost, 3bu)
**From:** "What about trying the moebius strip in an array?", after the doubly-linked-list form had lost three times (3bu: 0.40-0.46,
0.007-0.22, 0.006-0.49 at one thread). **Maps to:** what the engine has run since 2026-09-21: the orientation bit on the run
descriptors (`rdesc_t.rev`), a descending run consumed where it lies (backward copy alone, the two-ended merge in a crossing, the
knot plan in logical coordinates), the detector read-only. Measured then (`make twistbench`, `LOGOS_NO_TWIST=1` against the twist,
one binary, 8.4M): every block twisted **1.149 at one thread / 1.114 at 14**, blocks of 8k 1.144 / 1.094, x20 staircase 1.108 /
1.093, 84-97% of the reversal-pass ceiling; block_twist at 50M (3by) 1.42 ns per element at one thread (4.0x timsort) and 0.74 at
14 (4.6x IPS⁴o). The DLL proposal's one idea, reading a descending run backwards, IS this. Nothing new was built; the user chose to
leave the three corners below queued. Each in TWIST_AND_KNOT.md's terms:
- **(a) The one route that still un-twists: the wave route reverses every descending piece in place** (`wave_try_f64`,
  logos_psort.c ~line 8258, a scalar swap pass over about half the array) before the short-reach kernel sorts it and the sweep
  places it. The Moebius form sorts a descending piece AS IT LIES and hands it to the sweep with `rev` set, which the sweep already
  understands; the catch is that `reach_try_f64` (`reach_blocks` / `reach_seams`, ~220 lines) sorts ascending only, so it needs a
  mirrored kernel. **Known:** by arithmetic the pass is ~2-3 ms of an ~80 ms route at 8.4M, and constraint 1 of the twist round
  (the bit pays in proportion to how cheap the route is) puts this one, at ~10 ns per element, at the bottom. **Prediction, written
  before any measurement: 1.02-1.04 at one thread on the sine shapes, under the 40-pair floor.** **Next:** a `LOGOS_WAVE_NOFLIP`
  switch that skips the reversal (wrong output by design, the `LOGOS_WAVE_NOSWEEP` device) prices the pass end to end in one
  binary (`./sineprobe 40 4194304 ab`, 15 minutes); build the mirrored kernel only if that reads above ~1.05. Status: **OPEN**.
- **(b) The non-orientable OUTPUT: descending order for free** (avenue 6). A `logos_sort_f64_desc` that carries the bit OUT:
  the counting / lattice / RLE / dense routes emit in reverse slot order, the placing routes (knot, relocation, wave, tail, head)
  turn their copy-back into a reversed copy, the parallel partition mirrors its bucket ranges and reverses each bucket while it is
  L2-resident, everything else sorts and reverses. **Known:** the prediction of 2026-09-21 stands (1.03 on random data, 1.07-1.17 on
  structured, against sort-then-reverse); the counting routes may read higher (a reversal pass is about the cost of their emit).
  **Next:** the entry, a harness section asserting bytes equal to the reversed ascending output on every route by name, a paired
  bench against sort-then-reverse on roundbench's 23 shapes at 1 and 14 threads. Status: **OPEN** (a feature; ORDER BY ... DESC).
- **(c) The int32 twin, born twisted** (avenue 7): the int32 path has no relocation, knot or short-reach route. **Known:** 2.3-4.4x
  predicted on int32 block shapes from the f64 routes, a tie at 14; a multi-session port. Status: **OPEN**.
- Settled and not to be re-asked: the orientation bit one level down in the leaf's `detect_runs_limited` (ceiling 1.02-1.06, ledger),
  the direct oriented 3- and 4-way merges from the source (0.68 / 0.52, ledger), any list form (3bu), and ribbons (avenue 1), whose
  first cut is the wave route itself (1.21-1.24 of a 2.1-3.7 told ceiling; what is left of it is (a) above and 0m (a2) / (i)).

### 21. Seven invented ideas (asked 2026-11-09: "hallucinate more ways to sort faster"; NOTHING MEASURED, predictions written first)
Each is anchored to a number already in the record. Ranked by predicted upside over effort.
- **(a) Re-sweep the thread policy now that spawning is free.** **Maps to:** `effective_threads` (`MIN_ELEMS_PER_THREAD` 100000).
  The floor was set, and re-confirmed on 2026-09-23 (3ba), from the cost of creating 13 threads (0.56 ms); the persistent pool
  (3bk, 2026-10-08) removed that cost, and the record shows no sweep since. Meanwhile logos loses to IPS⁴o on 5-8 of 13 shapes at
  100k-316k at 14 threads for exactly this reason. **Next:** `make smallbench SM_SET=1` at 30k-1M with 25k / 50k per thread.
  Predicted 1.3-2x at 100k-316k on partition and counting shapes, nothing from 1M. Check first that the sweep was not already run.
  **SETTLED 2026-11-09 (FINDINGS 3bz): a split verdict. CLOSED-LOST as a global floor** (kinks, swaps, blocks 3.5-5x slower at 100k:
  the fixed cost is each route's parallel form at 2-4 threads, not the spawn). **CLOSED-WON for the unstructured partition alone,
  threads raised and the bucket count kept** (`BOOST_PER` 25000 from 300,000 elements): 1.62-1.99 at 316k, 1.28-1.42 at 500k,
  1.08-1.13 at 1M at 14 threads, guards 0.98-1.03. **Still open:** 100k-299k, where the policy runs one or two threads. With the
  boost in front of the two-halves route, uniform and int_wide gained 1.4-2.0x at 200k-290k and heavy_tail / normal lost 1.25-2.4x
  (24 buckets and a 768-point sample have no key grid): next is the boost there for a KS-certified sample only, or a partition whose
  sample is sized for the key grid; and the sequential path at 100k-199k has no boost point at all.
- **(b) Hedge a doubtful gate on the idle cores.** At 100k-316k one to three threads work and eleven idle, and that is where the
  counting gates choose wrong by 2-5x (19 (b)). When a gate's margin is thin, run the counting route and the leaf concurrently on
  two copies (a 100k copy is ~20 us), take the first finisher, cancel the other by flag. Predicted 2-5x on the wrong-gate shapes
  (zip_codes, prices_cents at 100k-316k), 0.95-1.00 where the gate was right. Costs one scratch of n; needs the pool's wake cost.
- **(c) Count strings that repeat.** words is the worst cell in the report (0.70 at one thread, 50M): a 50,000-word vocabulary
  sorted as 50M strings. Intern by hash in one pass (key + length + full compare), count, sort the DISTINCT strings, emit pointers
  per group. Hash counting lost for doubles past 4,096 values (3br) because the fallback there costs 0.4 ns per bit; here the
  fallback pays two dereferences per compare, so price the fallback. Gate on the sample's exact repeats. Predicted 2-4x on words,
  1.2-2x on emails' domains-first variant only if keyed, nothing on uuids / logs.
  **CLOSED 2026-11-09 at the ceiling, not built (`make strceil`, FINDINGS 3bz): 1.19 at 2M, 1.18 at 20M, 0.76 at 200k** on words
  (33,574-35,803 distinct), told the answer. The prediction priced the wrong fallback: the engine does not compare strings, it reads
  each once for an 8-byte key, and nearly every word fits one key, so both sides pay the same one miss per string; the told sort
  then scatters pointers into 35,000 groups, a miss of its own. Every other column has over n/8 distinct and declines.
- **(d) Pack the alphabet.** uuids (0.87 at one thread) carry 4 bits per byte and the 8-byte prefix key holds 8 characters. Read the
  alphabet from the sample; at <= 16 symbols pack 16 characters per key, at <= 64 pack 10 (an order-preserving code table per
  byte, 256 entries). Fewer tie levels, fewer dereferences. Predicted 1.2-1.5x on uuids and hex ids, 1.05-1.2 on urls / paths /
  dates, 1.00 elsewhere (the gate declines on a wide alphabet).
  **CLOSED 2026-11-09 at the ceiling, not built (`make strceil`): strings already re-coded two characters to a byte sort 1.24x
  faster (dates) and 1.30x (ids) at 2M, and with one re-coding pass counted 0.88 and 0.84.** An in-engine form pays a table lookup
  per character at every level instead, so it lands between the two. And the cell it was aimed at was misread: eight hex characters
  are 32 bits, which separates 2M uuids (and 50M) in the FIRST key, so depth is not what uuids cost (their 0.87 is the key sort).
- **(e) Heavy hitters at one thread.** fk_zipf reads 0.78 at one thread: the top handful of keys hold ~20% of the array and the
  quicksort partitions them like anything else. `lc_vec_count`'s compare-and-compact for the sample's top <= 8 modes, the rest to
  the kernel, the modes filled in by arithmetic (the dominant route, k values). Predicted 1.15-1.3x on Zipf keys; the same
  machinery is 19 (a)'s slot filler at P threads.
- **(f) `logos_sort_f64_into(src, dst)`.** Callers that want a sorted COPY pay a memcpy and then the sort's own scratch. With a
  destination given, the partition scatters straight into dst and the leaves finish in place (no scratch at all: 8.6 -> ~0 bytes
  per element at 14 threads, one pass fewer than copy-then-sort), and the placing routes drop their copy-back, which `nocopy`
  already priced at 1.21-1.35x on block shapes. Predicted 1.2-1.35x on block shapes and 1.1-1.2x on the partition against
  memcpy + sort. Different from 4 (a description) and S5 (a stream): the deliverable is still an array.
- **(g) Gates that learn from the clock.** Every 19 (b)-class defect is a route chosen where its cost per element exceeds the
  leaf's. Keep a small table of measured ns per element by (route, log2 n) across calls; a route whose running cost exceeds the
  leaf's entry at that size is skipped until re-tried one call in 64. Self-corrects a gate on any machine, at the price of global
  state and non-determinism in route choice (output unchanged). Predicted: 19 (b)'s losses to ~1.0 after a few calls, 0.99-1.00
  elsewhere. Park behind reentrancy (bizdev GAPS 1), which the table would complicate.

### 22. Twenty-four invented ideas from four read-only agents (asked 2026-11-10: "run a few agents and hallucinate ways to speed up and decrease memory usage"; NOTHING MEASURED)
Four agents, one lens each (one-thread speed, parallel speed, memory, the outsider), each told to read the ledger and this log first
and to give a prediction, a kill criterion and ONE settling measurement per idea. Nothing was built or run. Function names are the
agents' (they report having checked them); only (a)'s gate was re-read by the main session. Merged and ranked here by predicted
upside over the cost of the settling measurement. T = time, M = memory.
**Tier 1: cheap to settle, anchored to a number in the record.**
- **(a) T. The hash gate prices a fallback that is switched off** (sharpens 19 (b)). `mc_hash_allowed` admits the hash route at one
  thread where `sample_quant_passes <= QUANT_PROBE_MAX`, i.e. where the leaf WOULD have taken the quantized float radix; that radix
  is off by default since 3bn (`g_quant_radix`), so the real fallback on zip_codes is `sort_fn`'s counting sort. Verified in the
  source 2026-11-10. **Next, no build:** `LOGOS_MCGATE_V1=1` with `LOGOS_SCALE_SHAPES=zip_codes,ages`, then `make cardgate` heldout.
  Predicted zip_codes at 100k-316k 26 -> under 4 ns per element (0.18 -> above 1.5); kill: still under 1.0. Risk: 129-3300 distinct
  non-integers at 8M and up (3br's hash window), so the final gate is probably n >= 8M and Chao1 <= 2048.
  **MEASURED 2026-11-10 (FINDINGS 3ca): half right.** V1 reads 2.44 on zip_codes at 100k but 0.75-0.77 on 3,000-value whole-number
  columns: the clause is not the defect, the ESTIMATE is (Chao1 1,519, 18,923 met). **BUILT instead: the map gives up at 8,192
  distinct values (`MC_HASH_SEQ_CAP`): zip_codes 2.03 / 1.61 / 1.35 / 1.18 at 100k / 200k / 316k / 1M, guards 0.96-1.03.** Status: **DONE**.
- **(b) T. The 5-6% value at P threads, two forms to measure AGAINST EACH OTHER** (19 (a)). (b1) Split on claim: a worker that claims
  a bucket over ~3x the mean does one three-way vector split, marks the equal part done and requeues the rest (`hf_leaves`, a task
  stack as `ip_push` / `ip_run`); needs no gate and catches values the sample missed. (b2) Regions sized from the sample's counts
  under the chosen map, a repeating value given an equality bucket that needs no leaf (`hf_partition`); also covers 0m (i)'s
  631k-spill case. Predicted zip_codes / fk_zipf at 14 threads 1.4-1.8x (b1), 1.5-2.0x (b2); kill under 1.15 / 1.2 or a roundbench
  guard under 0.97. Risk: (b1) scrambles locally ordered buckets (gate on `sample_unstructured`); (b2) adds a load per flush.
  **BUILT 2026-11-10 in a third form (FINDINGS 3ca): heavy values are COUNTED in the scatter and filled in** (the trace showed the
  loss on zip_codes is the histogram path's extra pass, not the fat bucket). 14 threads: **zip_codes 1.66, fk_zipf 1.71 at 10M**,
  34 guard rows 0.95-1.04. (b1) and (b2) not built: nothing is left for them on these shapes. Status: **DONE**; closes 19 (a).
- **(c) T + M. Argsort through the dispatcher.** `logos_argsort_f64` is a one-thread 8-pass LSD radix with three scratch arrays
  (20 bytes per element) that never meets the prescan, the counting routes or threads; it was 129 of 203 ms in 3az (~65 ns per
  element against ~10 for the value sort). `logos_argsort_f64_mt`: identity from the prescan, indices by slot from the counting
  routes, else (key, idx) pairs through the partition. **Next (told ceiling):** in `datestrprobe`, x86-simd-sort's key-value sort on
  (key, idx). Predicted 2x at one thread, 5-8x at 14, scratch 20 -> 12; kill under 1.5x at one thread on uniform.
  **MEASURED AND BUILT 2026-11-10 (FINDINGS 3ca).** The ceiling killed the one-thread half (1.22 on uniform, 0.63 on whole seconds)
  and pointed elsewhere: sorted keys 54 ns per element, threads 4.4x. `logos_argsort_f64_mt`: a prescan, then one stable parallel
  partition on equal-width key cells and the LSD radix per bucket: **3.1-9.8x at 14 threads from 500k to 30M, sorted 31-58x, scratch
  20 -> 12, the identical permutation.** Left: the engine's partition on pairs (the argsort is still 2.5-3.4x the value sort at 14
  threads), counting routes for few values, `logos_argsort_i32`, a skewed key range (one cell). Status: **DONE** (first form).
- **(d) M + T. The near-sorted route in place at P > 1.** `nearsorted_parallel_f64` copies every chunk out and merges back (n moved
  twice, 8 bytes per element held). Compact the kept elements inside each chunk, lift ~1% strays, shift whole segments between
  insertion points (`strand_place_strays`' pass). Predicted 8.0-8.6 -> 0.3-0.5 bytes per element and 1.10-1.30x on 1% swaps at 8.4M;
  kill under 1.00 at 2% swaps. Differs from 14 (a) (half a scratch): no placement buffer at all.
- **(e) M. What a process holds between calls: one arena, an idle reaper, a parallel pre-fault.** (e1) Pool, regions, context
  bundles and count tables each sit at their own high-water mark; one arena with a bump cursor holds the largest single call's need.
  Predicted mixprobe 14.0 -> 9-10 at time 1.00. FIRST confirm the 14.0's split with allocation logging (not confirmed). (e2) A
  parked worker MADV_FREEs the arena after ~250 ms with no sort: under 1 held after idle, back-to-back calls 1.00 (the ledger's trim
  decayed by CALL COUNT and lost 1.3-2.5x; this keys on the clock). (e3) The workers `MADV_POPULATE_WRITE` the pool in P slices
  before the route starts: refault penalty to <= 1.10x, which would let trim be a default; kill if still over 1.25x (WSL2 may
  serialise the faults). (e2) and (e3) attack the same price from two sides.
- **(f) T. A split tree at 100k-299k** (21 (a)'s open range): the two-halves route to 4-8 ways on sample quantiles, no scatter, no
  map, sequential AVX2 leaves (`halves_try_f64`). Predicted heavy_tail / normal / clustered 1.3-1.7x at 150k-290k with 14 asked;
  kill under 1.10 on heavy_tail at 200k. Guards: the aliasing sawtooth, concatenated logs, the noisy ring.
**Tier 2: a new kernel or entry point, a told ceiling first.**
- **(g) T. Eight-lane int32 vector quicksort on narrowed keys** (int_wide 0.95, fk_zipf 0.78 if it is on that radix: trace first).
  NOT the ledger's `-DLOGOS_AVX2_INT32`, which is the four-lane f64 kernel on 8-byte compares. Ceiling in `avkernbench`: narrow +
  x86-simd-sort's `avx2_qsort<int32_t>` + widen. Predicted 1.25-1.5x on int_wide; kill under 1.10. Stepping stone with today's
  kernel: gate the int32 radix on sample REPEATS (a quicksort finishes a repeated value when it becomes pivot), predicted 1.2-1.3x
  on fk_zipf at 50M, kill under 1.08.
- **(h) T. Affine integers as 4-byte keys.** `all_int32` tests the ABSOLUTE range, so epoch-ms integers or integers x 2^20 miss the
  int path (symprobe: 1.19), and a lattice the fill rule declines is still integers. Map (v - mn) or round(x q) to uint32 with a
  per-element round-trip check, spill non-conformers. Ceiling: convert, `logos_sort_i32`, convert back, bit multiset checked.
  Predicted prices_cents at 100k 0.45 -> above 1.3; kill under 1.15. The date-format negative replaced routes that fired; this fires
  only on declines.
- **(i) T. Equals-elided three-way partition in the kernel** (`av_qsort_rec`): when the lane medians tie, one pass with two masks,
  equals counted and filled by arithmetic. Predicted 4k-262k distinct doubles at 16M 1.10-1.25, unique data 1.00. `make boundprobe`.
- **(j) T. Relocation's indivisible chain cut by merge path** (`reloc_worker` phase 2, `half_pathk`): the target the ledger's tile
  queue row itself names. Predicted block_overlap at 8.4M and 14 threads 1.15-1.30x; kill under 1.05 (`capbench`, `make profile`).
- **(k) M. Narrow regions**: 4-byte keys in the partition's regions when the sample is all int32-valued or float32-exact, the rest
  spilled as doubles. Predicted 8.6 -> 4.5 bytes per element at 14 threads, time 0.95-1.10x faster; kill under 0.97.
- **(l) T. Told runs**, `logos_merge_runs_f64(a, off, nruns, P)`: callers concatenating sorted shards know the boundaries; skip the
  detector and the gates that misfire (0m (a6)). Predicted log_batches at 100k-1M 1.5-2.5x; `primeprobe`'s scan share is the ceiling.
- **(m) T. Strings at 10k-64k: parallelise only the gather** (`str_keys_worker` on 2-4 workers, or a 16-ahead prefetch as the
  zero-thread control), `str_msd` left sequential. The three ledger rows all moved the whole level. Predicted urls / uuids / logs
  1.25-1.6x at 16k-64k; kill under 1.10 at 32k or words at 10k under 0.95.
- **(n) T / M. Byte-key and offset-layout entries** for record data: `logos_argsort_bytes` (fixed-width memcmp keys read at a
  stride, no pointer; predicted 5-7x over std::sort + memcmp on 24-byte stamps) and `logos_argsort_strings_off` (Arrow-style offsets,
  32-bit indices; 32 -> 24 bytes per string). One byte-key core could serve both. And typed front doors (`logos_sort_i64`, `_f32`)
  converted in place: f32 8 bytes per element total against 24 by widening; int64 past 2^53 cannot be sorted losslessly today.
**Tier 3: small or speculative.**
- **(o) T.** Tree wake and call-long warm workers (`wp_post` signals 13 workers serially): predicted 1.05-1.15x on structured routes
  at 1M; first time 13 empty `par_run` groups back to back.
- **(p) T.** A zero should not cost a scalar pass (`av_qsort_fused`, `av_scan_nan_negzero`): predicted 1.03-1.05 on data holding a
  zero; needs 160 pairs.
- **(q) M.** Strings: an in-place top MSD pass, scratch sized by the largest bucket: predicted 24 -> 9 bytes per string on uuids at
  up to 1.08x the time; words falls back and saves nothing.
- **(r) M.** Leaf-fused block eviction for the in-place partition: predicted 8.6 -> 1.5-2.5 at 1.05-1.15x the time. Check first
  with `make profile` under `LOGOS_PART_INPLACE=1`: if the permutation phase is under ~20% of the wall, the prize is not there.
**A control worth running before any kernel work on 18 (c):** uniform and heavy_tail reach the SAME kernel, and a branch-free
comparison kernel cannot tell two continuous iid distributions apart, yet heavy_tail read 1.03 in the 316M run where uniform read
0.87. The drift may be the single rep at 316M. Kernels alone at 100M and up, three interleaved reps, heavy_tail as the control.

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

### S9. Split the sample in half and ask whether the halves agree — raised 2026-09-26
**From:** the statistics brainstorm. **Maps to:** the shared sample (`sodo`): Chao1, KS and the inversion rate on the first 1024
positions against the second 1024, free of extra reads. Disagreement says the array is not ONE population along the index.
**Known:** eight concatenated logs read 0.80-0.86 against the engine (0m (a6)) and the seam gate misfires on them; three value
bands run at 2x uniform (0i (b)). Nothing measured about the split itself.
**Next:** compute both halves' statistics on the existing sample over the chaoshunt zoo and score the separation before wiring
anything. Predicted: catches concat and regime_switch, nothing on the nine standard shapes.

### S10. Trimmed quantiles for the key grid (a transplant of `count_try_i32`'s window) — raised 2026-09-26
**Maps to:** 0i (a): one zero at a[0] defeats the f64 partition's certificates. The int32 counting route already windows its
sample and spills the tail (11.3x -> 1.19x). **Next:** 0.1% / 99.9% quantiles for the grid, paired on `./sineprobe ... cos`.
Predicted 1.07-1.17x on waves that start at zero, the 0.85-0.88 at 14 threads recovered, guards neutral.

### S11. Order the route cascade by probe cost / P(accept) (the Smith ratio) — raised 2026-09-26
**Known:** the wasted probes measured so far are ~2.8 ms of a ~20 ms sort (the knot route's late decline), and reordering
cannot remove the probe that eventually accepts. **Next:** count, over the zoo, the time in probes that decline; drop it if
under 10% everywhere. Honest prediction: 1.05-1.15x on shapes with two late declines, nothing elsewhere.

### S12. Entropy of the sign string over CONTIGUOUS windows — raised 2026-09-26
**Maps to:** the reach probe's windows (not strided pairs, which alias). Sign of successive differences as a binary string:
0 bits per step sorted, 1 for noise, intermediate for a wave; its run-length distribution separates waves, kinks and blocks.
Detection-only, so under the 1-4% bound unless it changes a route. No prediction defended; a unification usually loses to the
specific probes it would replace. **Next:** none until a route needs a gate the existing probes cannot give.

### S13. Merge alternatives, asked 2026-09-26 ("faster and more CPU-efficient, even at the gate level")
**Built and shipped:** the vector two-strand merge (FINDINGS 3bc): a 4-wide AVX2 bitonic network, two chains from both ends,
1.96-2.28x on the kernel, 1.11-1.68x on staircases at one thread, a tie at 14 (the knot route is traffic-bound there).
**Not built, on measured grounds:** multiway merges (winner tree 1.4-1.9x slower; direct 3/4-way 0.68/0.52); wider galloping
(shipped where it pays, gated by a fourfold ratio where it loses 0.95-0.98); in-place block merges (SymMerge, GrailSort:
scratch bought with time, and the routes have scratch); merging on the total-order int64 key (only worth it as a NaN-free
variant of the vector kernel, which needed no such thing: one predicate and two selects keeps NaNs and signed zeros a
permutation). **Open:** the k >= 3 crossings go through `merge_all_runs` (a k-way merge) and never see the vector kernel;
a tree of 2-way vector merges is log2(k) passes of 1.5 ns per element against one k-way pass whose cost per element is not
measured here -- measure that pass first (`make mergebench` extended to k = 3, 4, 8) before building anything.

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
