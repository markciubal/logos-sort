# Outside the box — round two

Companion to `ORDER_FROM_CHAOS.md`. Same rules: **every idea carries a written
prediction before anything is measured** (methodology rule 6), and a measured dead
end is a deliverable (rule 7). Nothing here is implemented. Ordered by
(expected value) / (effort), not by how clever it sounds.

The resolving floor is now ~4% with `bench_paired` plus a copy swap
(`tools/ledger_rerun.py`), so effects that were invisible when most of this engine
was built are now measurable. Several of these are only worth trying because of that.

---

## A. The machine underneath — three things this project has never touched

> **Section verdict, 2026-09-12: this section is closed.** A1 and A2 were both built,
> both verified to fire, and both measured exactly neutral. Two independent levers on
> two different kinds of memory traffic, plus the older ping-pong result (37% less
> traffic bought 12% of time), say the scatter is bound by per-core latency and fixed
> cost rather than by the memory system. A3 is untouched and is about thread policy
> rather than memory, so it survives. **Sections B and C are where the value is.**

### A1. Huge pages for the scratch pool — **BUILT AND MEASURED 2026-09-12: NEUTRAL**

> **Result.** Mechanism confirmed (98 MB of AnonHugePages instead of 0), effect nil:
> 0.98–1.01 at 8M and 0.97–1.02 at 30M. Prediction below was 1.10–1.30x and it was
> **wrong**, because `hf_flush` already stages 256 doubles per bucket and flushes 2 KB
> contiguously, so the TLB miss rate follows the burst size rather than the working
> set. Quadrupling n changed nothing, which is the proof. Kept opt-in as
> `-DLOGOS_HUGEPAGE`. Original text preserved below.

**The gap.** `grep -rn madvise engines/c` returns nothing. On the reference box
`/sys/kernel/mm/transparent_hugepage/enabled` reads `always [madvise] never`, which
means huge pages are handed out **only on explicit request**, and `AnonHugePages`
reads `0 kB` while the engine runs. So every buffer is 4 KB pages, always.

**Why that should hurt badly.** Raptor Lake's L2 TLB is ~2048 entries. At 4 KB that
covers 8 MB. An 8M-element sort touches 64 MB of source plus 64 MB of destination,
so the working set is ~16x the TLB's reach — and the top-level scatter writes to
those 64 MB in *bucket* order, which is close to random with respect to pages. That
is the worst possible access pattern for a TLB: roughly one miss per store, each
costing a page walk. With 2 MB pages the same 2048 entries cover 4 GB and the misses
vanish entirely.

**Implementation.** One `madvise(ptr, len, MADV_HUGEPAGE)` in the persistent pool
allocator, plus 2 MB alignment on the allocation so the kernel can actually promote
it. Perhaps ten lines. `-DLOGOS_NO_HUGEPAGE` to A/B it.

**Prediction.** 1.10–1.30x at 14 threads on every shape that reaches the partition
(the scatter is the phase that thrashes), 1.05–1.12x at 1 thread, and **nothing** on
the counting and lattice routes, whose tables are small enough to be TLB-resident
already. If it comes back flat everywhere, the honest conclusion is that the
hardware page-walker is prefetching well enough to hide it, which is worth knowing.

**Test with.** `make hfbench`-style paired A/B; confirm the mechanism by watching
`AnonHugePages` in `/proc/meminfo` move off zero while it runs.

### A2. Non-temporal stores for the top-level scatter — **BUILT AND MEASURED 2026-09-12: NEUTRAL**

> **Result.** Mechanism confirmed by a traced counter (99.4% of elements streamed),
> effect nil: 0.99–1.01 at 14 threads, with the 1-thread rows at 0.993–1.003 as a clean
> null control. Prediction below was 1.05–1.15x and it was **wrong**. Together with A1
> this is the second verified memory-traffic lever to measure zero, so the honest
> conclusion is that the scatter is not memory-bound and the rest of section A is not
> worth building. Kept opt-in as `-DLOGOS_STREAM`. Original text preserved below.

**Mechanism.** The scatter's destination is written once and not read again until the
leaf phase, which happens after a barrier. So every scattered line is pure pollution
in L2/L3 between now and then, evicting the bucket-index cache and the splitter
tables that the *same* loop is reading. `_mm256_stream_pd` bypasses the cache
hierarchy and writes straight to memory through the write-combining buffers. The
write-combining staging buffer (`HF_BLOCK` 256) already batches stores per bucket, so
the lines are full and streaming is legal and efficient.

**Prediction.** 1.05–1.15x at 14 threads where DRAM pressure and cache contention are
highest; roughly neutral at 1 thread; possibly a small *loss* on small n where the
destination would have stayed in L3 and the leaf could have read it hot. If so, gate
on n.

**Risk.** Streaming stores need the fence discipline right (`_mm_sfence` before the
barrier) or the leaf reads stale data. That is a correctness bug the harness will
catch loudly, which is the good kind.

### A3. Thread count chosen from the machine's *current* speed

**Mechanism.** FINDINGS §3t established that this box runs 5–10x slower when hot and
recovers over ~26 s. The optimal thread count almost certainly moves with that: a
thermally limited package gains less from more threads, because they contend for a
power budget rather than for idle cores. The engine picks threads from n alone.

**Prediction.** On a cold box the current policy is right. On a hot one, 8–10 threads
beat 14 by 1.05–1.15x. A 1 ms calibration loop at sort start is cheap enough to pay
for on any sort above ~1M elements.

**Why it is interesting beyond speed.** It makes the engine adaptive to something
every long benchmark in this project has been fighting.

---

## B. The model — better maps, not more tables

### B1. L-infinity optimal piecewise-linear CDF  ★ aims at the one known loss

**The gap.** The engine has an equi-width grid (certified uniform), a log grid
(certified log-uniform), per-mode linear maps, and fallback tables. All of them place
knots by *position* (equi-width) or by *count* (equi-depth quantiles). Neither places
knots where the CDF actually **bends**.

**Mechanism.** Take the sorted sample as a curve of value against rank. Run
Douglas–Peucker on it with a tolerance of one bucket. The result is the
piecewise-linear approximation with the fewest segments whose worst-case bucket error
is bounded — knots dense where the distribution curves, sparse where it is straight.
Bucket lookup becomes: binary search over ~32 knots (or a 256-entry direct index over
the knot range), then one multiply-add. That is strictly a better use of the same
budget than equi-width or equi-depth, because it optimises the quantity that actually
matters: maximum bucket imbalance.

**Prediction.** heavy_tail at 14 threads goes from 0.94x against x86-simd-sort to
1.02–1.12x — it is the shape whose CDF bends hardest and the only fair-fight loss
left. Uniform and quantized unchanged (their certificates already fire first and are
cheaper). Bimodal within noise of the per-mode map it would replace.

**Why it might fail.** The project has measured twice that adding a level to the
lookup costs more than a better bucket distribution saves. This is the third attempt
at that class of idea, so the prediction should be treated with suspicion. But the
first two attempts optimised the *lookup*; this one optimises *balance*, which is the
thing `BAL_OK` exists to protect.

### B2. An order budget instead of a pile of gates

**Mechanism.** Sorting removes `log2(n!)` bits of disorder. Every probe the
dispatcher already runs is really an estimate of how many of those bits are *already
gone*: sortedness, cardinality, lattice spacing, mode structure. Right now each gate
is an independent threshold, tuned separately, and the thresholds disagree at their
boundaries — which is exactly where the engine loses (the int32 span band between the
counting cap and n/8, where 14 threads still lose to one core).

Replace the gate chain with one number. Estimate the remaining entropy `H`, give each
route a measured "bits removed per nanosecond per element" rate, and pick the route
with the best rate for this `H`. The rates are already measured; they are scattered
across the CHANGELOG.

**Prediction.** Neutral on every shape the gates already get right, which is most of
them, and that is fine. The win is at the boundaries: 1.2–2x on the bands where two
gates disagree today, and, more importantly, new shapes stop needing a new threshold.

**Honest risk.** This is a refactor wearing an optimisation's clothes. It should be
judged on whether it removes tuning constants, not on its speedup.

---

## C. Orders we currently cannot see

### C1. Strided order — the probe nothing has

**The gap.** The dispatcher detects ascending runs, bounded displacement, block
permutations, and low cardinality. It cannot see data that is sorted *under a stride*:
a sorted array read column-major, a k-way interleave of sorted streams, a
round-robin merge from k shards. Every one of those is common in analytics and looks
completely random to every probe we have.

**Mechanism.** For s in {2, 3, 4, 8, 16, 32}, check whether `a[i]` is ascending along
each residue class mod s, using the same Weyl sample positions. Cost is a few
thousand reads, the sample is already in cache. If it fires, the array is a perfect
k-way merge of sorted runs, which the existing run-merge machinery can consume with
no scatter at all.

**Prediction.** 3–10x on any strided shape, because it converts a full partition into
a k-way merge of already-sorted sequences. Exactly neutral elsewhere: the probe is
~0.1% of a sort and it either fires or it does not. **This is the highest
value-per-line idea in the document** if such data ever shows up, and the cheapest
way to find out is to add the shape to `chaosbench` and see how badly the engine
currently does on it.

### C2. Bail out mid-flight when the partition goes bad

**Mechanism.** `BAL_OK` checks bucket balance against the *sample* before scattering.
Nothing checks it against *reality* during the scatter. A sample can be unlucky, and
an adversary can make it wrong on purpose. Threads already count into per-bucket
region counters; watching the largest one cross 3x its share mid-scatter costs one
compare per block flush, not per element.

**Prediction.** Zero effect on every benchmark shape, because the sample is not
unlucky on any of them. The value is a bounded worst case rather than a faster
average, and that is the kind of thing you want in a library other people call.

---

## D. Moving bytes without moving bytes

### D1. Relocation by page table  ★ the genuinely strange one

**Mechanism.** The J5 relocation route already recognises block-permuted data and
`memcpy`s sorted runs into place. But a `memcpy` of an aligned, page-sized, sorted run
is *pure waste*: the bytes are already correct, only their address is wrong. If the
buffers are `mmap`ed and runs are 2 MB aligned, `mremap(MREMAP_FIXED)` moves the
*mapping* instead of the data. Megabytes relocate by editing a page table entry.

**Prediction.** Nothing for runs under ~64 KB, because a TLB shootdown across 14
threads costs more than copying 64 KB. Above ~2 MB of aligned run, near-infinite
speedup on that run, because zero bytes move. Whole-array effect on block-permuted
shapes: 1.5–4x, entirely dependent on how much of the array lands in aligned
multi-megabyte runs, which the existing run descriptors already tell us.

**Cheapest first step, and do this before writing any code.** Instrument
`relocate_try_f64` to report the distribution of run lengths and their alignment on
the bench shapes. If nothing is 2 MB aligned, the idea is dead for the cost of a
`printf`, and that is a result worth recording.

---

## E. Changing the deliverable — where this engine actually beats the field

The paradigm experiments remain the strongest measured result in the project
(quantiles 87x, top-k 7x, range-count 42x). Two ideas that follow from taking them
seriously.

### E1. The lazy sorted view — **BUILT AND MEASURED 2026-09-12: WORKS, 1.9-2.7x**

> **Result.** Median, rank, top-k and range-count through a view run 1.44-2.89x faster
> than sorting and looking across four shapes. Building the view costs 35-55% of a
> sort, matching the traced 37% partition share. Break-even at about 16 scattered
> queries, and 1.10-1.37x slower if you materialise everything (one structural extra
> pass; see the CHANGELOG). The
> prediction below (identical when fully materialised, 5-50x for a slice) was right
> about direction and too optimistic about both ends. Shipped as `logos_view.h`.
> **The first attempt hand-rolled its own partition and cost 2-4x an eager sort**: a
> lazy API is only worth building on a partition that is already the best available.
> Original text preserved below.

**Mechanism.** `logos_sort` returns a sorted array. Most callers then take a slice, a
top-k, a median, or a filtered subset. Return instead a handle holding the splitters,
the bucket offsets, and the unsorted buckets, and sort a bucket only when something
indexes into it. `view[k]` sorts one bucket. `median(view)` sorts one. A full scan
sorts all of them and costs exactly what the eager sort costs today.

**Prediction.** Identical performance when fully materialised (the work is the same,
just deferred), and 5–50x when the caller wanted a slice, which is most of the time
in analytics. The risk is entirely API design, not performance.

**Why it fits this engine specifically.** The partition is already the expensive part
and the leaves are independent. Laziness is nearly free to implement and impossible
for a comparison sort to imitate, because a quicksort's partitions are not independent
of the order in which you resolve them.

### E2. Splitters that outlive the call

**Mechanism.** Cache the splitters, the certificate, and the chosen route keyed by a
cheap signature of the data (n, min, max, a few sample quantiles). A second sort of
similar data skips the sample and the decision entirely. Streaming and time-series
workloads re-sort near-identical distributions thousands of times.

**Prediction.** 1.02–1.05x at 8M, where the sample is ~0.9 ms of a ~20 ms sort, but
1.15–1.40x at 100k–1M where the fixed costs dominate — the same region where
`effective_threads` and the per-route thread caps already found their wins. Must be
opt-in: silently reusing a stale model on shifted data is a correctness-adjacent
footgun even though the output stays correct.

---

## F. Long shots, recorded so they are not re-invented

- **Speculative placement.** Use the CDF model to write each element at its *predicted
  final index*, then repair collisions. Prediction: fails. Repair cost grows with the
  square of the model error and the model is never that good. Stated because it is the
  logical endpoint of "addressing beats comparing" and someone will think of it.
- **Sort by rank model, not value** (Learned Sort proper). Truth table row L. Still
  untested here; B1 above is the cheap 80% of it.
- **Quorum-sensing thread pool**, where workers measure their own throughput and
  collectively retire. Prediction: loses to the static policy plus A3, and costs a
  synchronisation point per chunk.
- **Branch predictor as a classifier.** Cute, not actionable.
