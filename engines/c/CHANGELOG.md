# Changelog

## (unreleased) — the queued ideas, A/B'd one at a time: a work queue that pays on a COMPUTE phase and measures nothing on a MEMCPY phase, and branchless sorting networks at the small end (1.03x at n = 4 to 1.58x at n = 16)

Asked: run the A/B tests on the queued ideas and merge what succeeds. Four were built and measured, three were closed
without building because the measurement or the mechanism already settled them.

### 1. The threads on this box are not equal — SPLIT VERDICT (FINDINGS 3aw)
This is an i9-13900H: 6 performance cores, 8 efficient ones. Any phase that hands every thread `n/P` elements ends when
the SLOWEST core does. **The kill test came first and needed no new run**: per-thread busy inside the statically chunked
phases is bimodal (quantized lattice map 4.40 vs 2.99 ms, 1.47x; mod_card 1.44; few_distinct hash map 2.09; relocation
tiles 1.39) where the queue-driven leaf reads 1.04.

- **The lattice MAP phase claims chunks: SHIPPED, ~1.05x.** Quantized and mod_card_30k 0.90-0.97 on 12 of 12 cells at
  4M and up, two runs of 60 pairs, against a NULL switch on the same rows at 0.97-1.04. `LOGOS_LAT_STATIC_MAP=1`
  restores the slices. Predicted 0.75-0.90: too optimistic by about half.
- **Relocation's output tiles: REVERTED, 0.96-1.04 on every cell of two runs.** Predicted 0.88-0.97 at 4M+. Correct
  throughout (harness both settings; faultprobe 1,926 allocation and 48 thread-creation failures, 0 bad, reaching
  `reloc_worker` itself once the knot route is switched off) and worth nothing.
- **The contrast is the result.** The lattice map is COMPUTE into a private per-thread table, so a faster core really
  does finish more elements. Relocation's phases 1 and 3 are `memcpy` over disjoint ranges, bound by one shared memory
  controller. **A work queue buys balance only where the CORES are the bottleneck** — which also says, without another
  round, that the knot worker's identical A1 and C tiles are not worth converting. And it moves the target: if equal
  tiles change nothing, the 3.6 threads the profile shows waiting are at the SECOND barrier, on one indivisible chain
  merge. That is relocation's serial middle, and it wants a merge-path split of ONE chain.

### 2. Branchless sorting networks for 4 to 16 elements — SHIPPED (FINDINGS 3ax)
The leaf finished short ranges with insertion sort: ~n^2/4 compares on unstructured data, about half mispredicting.
`gen_small_net.py` emits one straight-line Batcher network per size (424 comparators) and **proves each by the 0-1
principle before writing it**. The compare-exchange is one predicate and two selects, never a min and a max, because
min/max return the same operand on a NaN and would lose its partner — the output must stay a permutation. The assembly
confirms the mechanism: 424 compare-exchanges, ONE branch (the size jump table), an 8-byte `noinline` frame.
- **Measured** (`make callbench`, copies swapped, two runs): random calls 1.03-1.06x at n = 4, 1.22x at 8, 1.33x at 10,
  1.39x at 14, **1.57-1.58x at 16**, monotone; n = 18 and 24 read 0.99 (the control); sorted rows 0.97-1.04 at every
  size; 1000-50000 read 0.98-1.02. **A small-end result (direction 0g), not a result at scale** — a large sort takes
  the flash, coarse or AVX2 leaf, not the scalar quicksort base case.
- **Graded**: predicted 1.5-3x across 4-16. Right at 16, wrong at 4; the effect is the ratio of comparators to
  mispredicted branches and grows with n. The stop rule had the same flaw (one bar across a scaling range), so it is met
  from n = 8 and missed at 4-7. Shipped because nothing regresses and every size in range improves in both runs and
  both orientations; the miss is recorded rather than rounded away.

### 3. Closed without building
- **A 32-bit key prefix**, killed on its own pre-written criterion — which asked the wrong question. Measured
  separation is 55.5% / 53.5% / 73.9% on uniform / heavy_tail / int_wide and 0% on few_distinct and quantized, but the
  latter two's ties are 100% DUPLICATES (free) and the random shapes' real tie groups hold 7-10 elements. It dies on the
  mechanism instead: prefix + a 32-bit index is 8 bytes, exactly what a double already weighs.
- **Speculating the partition during the probe**: incoherent. The splitters need the sorted sample, and the sorted
  sample IS the probe. Nothing is independent to speculate on.
- **Choosing the leaf per bucket**: its premise named the partition's byte histogram, which has not existed since the
  partition went histogram-free. Rewritten around what survives.
- **Stopping the sample early**: parked. At 0.5-2% it would need ~550 pairs per cell to separate from zero.

### A process failure, recorded because it nearly shipped a false result
The first correctness run reported "all correctness checks passed" for both settings of the new switch **from a stale
binary**. The verification script sent `make` output to `/dev/null`; the new harness section failed to compile (a
heredoc turned `\n` inside two `printf` strings into real newlines); `make` left the previous binary. Two cheap tells:
the binary was six minutes OLDER than its source, and the new section printed no check count. Built properly it is
**368 checks, 0 failures** over n = 2 to 24, with NaNs, signed zeros, infinities and heavy ties at sizes nothing else
here sorts. **A script that reports tests passing must keep the build's errors and exit status.**

### Checked
`make test` on the final state (networks in, tile queue out); the new section runs with the networks on and off;
`make callbench` in both copy orientations, twice, plus a narrowed size ladder to locate the crossover; faultprobe
1,926 + 48 injected failures, 0 bad, on both tile settings; the A/B flag builds; the dispatcher frame printed against
the committed engine.

## (unreleased) — the lattice route, after the Amdahl round pointed at it: the serial merge in PARALLEL is 1.02-1.12x SLOWER and is not shipped (the trace says it is ~10% of the route, not the ~55% the idle-thread count implied); moving each worker's table allocation off the caller's critical path IS worth ~1.03x on the lattice shapes and ships (`make capbench` with CAP_SWITCH)

Asked: run the A/B tests on the queued ideas and merge what succeeds. This entry is the top of that queue.

### 1. The lattice merge in parallel — BUILT, MEASURED, NOT SHIPPED
`lattice_merge_par`: slots are value-ordered, so a slot RANGE is an output range; each range is reduced across the P
tables independently, an O(P) prefix over the per-range totals gives every range its offset in gv/gstart, and the ranges
are written out in parallel. Ranges are claimed with an atomic, so a worker whose thread could not be created costs speed
and never correctness. It is correct (the harness, ASan/UBSan and TSan all pass with it forced on, and faultprobe injects
546 allocation failures and 30 thread-creation failures across three counting shapes with 0 bad) — and it is **1.02-1.12x
slower** at 4M-16M, so it ships OFF behind `LOGOS_LAT_PAR_MERGE=1`.
**Why the diagnosis was wrong.** `make profile` shows 6.0-6.9 of 14 threads busy with none waiting inside the phase that
is 98-99% of this route's wall, and I read that as the serial merge. The TRACE says otherwise: at 8M and 14 threads the
MAP phase is 11.6-14.1 ms and the merge about 2 ms, so the merge is ~10% of the route. Amdahl caps the prize there, and
the extra spawn (~60 us a thread), the temp arrays and a second pass over the slots eat it. **Inferring a phase's share
from a thread-occupancy average is not the same as timing the phase.**

### 2. Each worker allocates its own table — SHIPPED, ~1.03x
The caller's setup loop was P mallocs of three arrays plus a memset of `slots` keys per thread: at 8M and 14 threads that
is 5.6 MB of serial writes inside the phase the trace puts at ~80% of the route. Each worker now takes its own table at
the top of `lattice_map` (a failed allocation sets `abort`, which the caller already reads); `LOGOS_LAT_SERIAL_TABLES=1`
restores the old order for the A/B.
- **Measured** (60 pairs, two runs, 1M-16M, t(new)/t(old) so below 1 is the win): quantized 0.96 0.96 0.96 0.96 0.99 and
  0.99 0.95 0.95 0.99 0.97; mod_card_30k 0.99 0.94 0.98 0.98 0.98 and 0.97 0.97 0.97 0.97 0.98. **Twenty of twenty cells
  on the winning side, centred on 0.97**, against a NULL switch on the same rows reading 0.98-1.04. Guards (uniform,
  block_perm, few_distinct) 0.95-1.08 across both runs at 20 pairs.
- **Mechanism, from the trace**: the map phase is 8.82, 8.94, 11.33 ms with per-worker tables against 10.87, 10.92,
  13.75 with the caller's loop.
- **Graded**: the prediction (0.83-0.95) was too optimistic by about half, and the stop rule's 0.95 bar was missed. It
  ships anyway because the mechanism is verified and the null control separates ~3% from noise — recorded here so the
  judgement is visible rather than implied.

### Checked
`make test` (all sections, 0 failures) with the shipped default; 90 flag variants build; ASan + UBSan +
float-cast-overflow and TSan clean on the counting sections with the parallel merge forced on as well as off; faultprobe
546 + 30 injected failures, 0 bad, on both paths; `make memprobe` unchanged (quantized holds 0.21 bytes per element at
one thread and 0.49 at fourteen).

### Still open on this route
The map phase is ~80% of it and `make profile` still shows only 6.0-6.9 of 14 threads busy there. That is now the
question: WHERE in the map phase the threads are idle (per-thread tables are 1.2 MB each at 50k slots, so cache pressure
and the random slot access are the suspects). The profiler lumps the whole route into one phase; finer phases would
answer it.

## (unreleased) — where the parallel speedup stops, measured two ways, and two remedies that did NOT pay: a thread ceiling for the counting routes (the lattice family loses 0.53-0.90 at 8M-16M) and a gate against the near-sorted route at low thread counts (it is worth 1.8-1.97x at 2 threads on the shapes it was built for). The diagnosis stands and names the one fix left: a PARALLEL merge (`make amprobe`, `make capbench`, `make profile`)

Asked: inspect the engine for Amdahl's-law opportunities, then make the changes.

### The measurement (two instruments, 8M doubles, 14 threads)
`make profile` gives per-phase wall, busy, waiting and idle threads; `make amprobe` gives a thread ladder with the
Karp-Flatt metric e(P) = (1/S - 1/P)/(1 - 1/P), the experimentally determined serial fraction: e roughly constant across
P is a fixed serial part, e rising with P is overhead that grows with the thread count.
- **The partition is healthy**: 13.0 of 14 threads busy in the leaf, 11.3 in the scatter, e = 0.055-0.09 from 3 threads
  up, measured 6.5-7.3x at 14 against an Amdahl bound of 8.2x. Perfect packing would buy ~1.16x. Nothing to take here,
  which agrees with the three memory-traffic experiments that measured zero.
- **The counting and lattice routes**: one phase is 98-99% of the wall and inside it 6.0-6.9 of 14 threads are busy with
  NONE waiting -- the others have nothing to do. That is the serial merge between the two parallel phases (the hash
  route gathers, sorts and folds P x d pairs; the lattice walks slots x P), and it GROWS with P, which is why e rises
  0.06 -> 0.23 as P goes 2 -> 14.
- **Relocation**: 6.6 busy, **3.6 WAITING at a barrier**, 3.8 idle, e rising 0.02 -> 0.27. The placement gives every
  thread a static output tile (phase 2, the chain merges, is already an atomic queue).
- **The sample** is 1-4% of every parallel call and strictly single-threaded: a ceiling of ~1.03x.
- **Kinked data sorts fastest on ONE thread**: 6.84 ms at 1 thread, 15.6 at 2, 9.4-9.7 at 4, 7.2 at 14. More threads
  never pay on that shape.

### Remedy 1, measured and REJECTED: a thread ceiling for the counting routes
`LOGOS_COUNT_MAX=<k>` caps them per sort; `make capbench` pairs a ceiling against no ceiling at 1M-16M, two runs of 20
pairs. No global ceiling passes: k = 3 gives few_distinct 1.50-1.54 at 2M but costs the lattice shapes **0.53-0.87**;
k = 6 and k = 8 cost them 0.72-0.90 at 8M and 16M. The two families want opposite things and the profile says why: the
lattice route's map phase is real parallel work that wants every thread, while the hash route stops scaling near 8. A
ceiling on the HASH route alone (`count_threads_hash`) was then measured on its own: few_distinct 1.06 0.94 1.01 1.12
1.04 and 1.06 0.97 1.01 1.07 1.06 over 1M-16M -- about 1.0 at 4M and 1.1 at 8M, inside what 20 pairs resolve, so it
fails its own stop rule too. **Shipped OFF**; the knob stays as the A/B switch and the record. Guards (uniform,
block_perm) 0.95-1.05 throughout, so the knob is correctly scoped.

### Remedy 2, measured and REJECTED: prefer the sequential path at low thread counts on near-sorted data
The ladder's worst row was near-sorted data at 2 threads, so the near-sorted route looked like the culprit. It is not.
Against the route switched off (`NSG_ROUTE=1 ./nsgatebench`, 2 threads) the route is worth **1.80-1.97x** on the shapes
it was built for (1% and 0.5% random swaps) and 1.56-1.61x on 2% swaps. On KINKS (adjacent swaps, a different shape:
the detector repairs them into one strand) turning it off is worth 1.18x at 2 threads and 1.04x at 4 -- but it COSTS
1.10x at 14 threads (`TS_SWITCH=LOGOS_NO_NEARSORTED ./tsbench ... kinks`). A gate would trade a win at 14 threads for a
win at 2, and it would not close the real gap anyway: without the route, 2 threads still take ~13 ms against 6.84 on one.

### What is left, and it is the only thing the profile actually asks for
Make the counting routes' merge PARALLEL (open direction 0a: split the key space P ways, then each thread folds its own
slice) so those 7-8 idle threads have work, and give relocation's placement a task queue instead of static tiles. Both
are real changes, not knobs; neither is built.

### Tools added
`amprobe.c` (the thread ladder with Karp-Flatt, and `ladder` for the counting routes across five sizes) and
`capbench.c` (a ceiling paired against no ceiling). Nothing in the engine changed except the opt-in knob.

## (unreleased) — "treat uniform data as a complete computation": certify the distribution, sort P chunks with a parallel SIMD quicksort, merge them. The chunk sorts alone DO beat the engine (0.76-0.86 of its time); the merge then costs about three times the engine's whole runtime, so the pipeline lands at 3.3-4.2x on every shape including uniform (`make unibench`)

Asked: once the sample says the data is somewhat uniform, stop looking for a pattern — sort it as a complete computation:
a parallel SIMD sort per chunk, then a merge, if that is fastest. A/B it.

### What the engine already does on that certificate, and what was new
The histogram-free partition runs a Kolmogorov-Smirnov test on the shared sample, and where it certifies, every element's
bucket is ONE multiply-add on an equi-width grid with no tables and no comparisons (FINDINGS §3m). That is the proposal's
"complete computation" half, shipped since then. What is new is the second half: replacing the scatter with a merge. The
ledger's "parallel merge sort (chunk-sort + k-way merge): merge 15-26x costlier than the scatter" predates the AVX2 leaf
and used a k-way merge, so `unibench.cpp` builds the strongest form available today: P chunks sorted by x86-simd-sort's
AVX2 quicksort, then a bottom-up tree in which EVERY merge is split across the threads by merge path, so no level
collapses onto one thread.

### Measured (8M, 14 threads, two runs of 16 pairs; the full table is in unibench.cpp's header)
- **The premise holds.** The chunk sorts alone, with no merge, run at **0.76-0.86 of the shipped engine's time** on the
  random shapes: fourteen SIMD quicksorts of 571k elements in parallel are faster than the whole engine.
- **The merge is the verdict.** Adding it puts the pipeline at **3.43-3.96x (run 1) and 3.32-4.19x (run 2)** on every
  shape, uniform included. With std::sort per chunk instead, 6.28-9.89x.
- **Why, with a number.** The merge moves 3.86n on every shape, but it costs 87 ms on uniform and 15-18 ms on
  near_sorted and block_perm. Where neighbouring chunks hold disjoint value ranges the merge path degenerates into a
  copy; where every chunk spans the whole range every output element comes from a comparison. A merge on uniform data
  costs 5.8x what moving the same bytes costs, which is the old ledger entry with a measurement attached.
- **The structured guards** are worse still: on near_sorted and block_perm even the chunk sorts alone are 1.69-1.89x
  slower than the engine, because its routes read the order that chunking throws away.

### A defect in the first cut, caught by the check and not by the clock
The first version sorted each chunk with `logos_sort_f64(a + lo, len, 1)` from 14 threads. **The engine's sequential entry
may not be called concurrently**: its context pool and bundle table (`g_ctx_pool`, `g_ctx_next`) are plain globals. The
anatomy printed WRONG on int_wide, and every number from that cut was discarded; the kernel became x86-simd-sort's, which
is header-only and has no shared state. Worth stating in the header where the memory contract is: one top-level sort at a
time per process, whatever the thread count inside it.

### Not shipped
Nothing changed in the engine. The uniform path stays as it is: certify, then compute the bucket.

## (unreleased) — the Logos-Moebius hybrid as handed off (scan, twist, Reidemeister I on 2-3 element dips, a fallback under 16, a galloping stitch behind an O(1) boundary check): behind the engine on 14 of 16 timestamp shapes as written and 13 of 16 in its strongest form, and quadratic where the runs are stitched into one growing prefix (a ring buffer 58-68 s against 8 ms); the row it won was an engine defect, fixed: the run detector's block of eight compiled to scalar compares, and with vector ones kinks run 1.30-1.32x and shuffled blocks 1.27-1.30x faster at one thread (`make tsbench` modes `moebius`, `moebanat`)

> The implementation was REMOVED on 2026-09-23 and is in git history before that commit. Everything measured below stands; the engine fixes this round found are shipped and keep their own A/B switches.

Asked: to inform the weld with a handoff specification of "what remains unimplemented" in a Logos-Moebius hybrid: (A) bail out to a
fallback sort when a run is shorter than 16; (B) swap a descending run of 2 or 3 inline (Reidemeister move I) instead of reversing it;
(C) stitch the runs with a galloping merge that skips a boundary outright when the left maximum is at most the right minimum.

### What the engine already had, and what was actually new
The kink repair (B, shipped, and stronger: it swaps the pair back INSIDE the run instead of ending it, 5.8x on kinked blocks), the
twist (a descending run is consumed as it lies, so 2 and 3 element runs need no swap at all), the run-length floors (A), and the O(1)
boundary check (C), measured twice before this round: 0 of 262,143 merges skipped on every latency shape (round one) and 0.0% of seams
on every shape (round three). What was new: the runs stay in POSITION order and are stitched, where round three sorted them by min edge
first. So both were built: the LITERAL pipeline, exactly as specified, and a REPAIRED form (a tie continues a descending run; a short run
is EXTENDED to 32 by insertion, Timsort's minrun, rather than re-sorted; balanced bottom-up merges instead of one growing prefix).

### Measured (1M, two runs; the full table and grades are in tsbench.cpp's header)
- **The literal pipeline is behind the engine on 14 of 16 shapes**, by 1.7-2.8x on the block shapes, 5.9-7.0x on jitter, 22-26x on
  telemetry and clock skew, **502-543x on a late 5% batch, 4,759-7,342x on uniform data, and 6,382-16,213x on a ring buffer and a
  newest-first log** (58-143 s for one sort). The repaired form is behind on 13 of 16, by 1.5-7.4x.
- **Component B never fires on the shape it was written for.** With 1% adjacent swaps the literal form counts 0 kink swaps: a dip's
  first element belongs to the PRECEDING ascending run (the scan stops there), so the descending run is one element and never 2 or 3.
  The dip becomes two runs and is paid for at merge time. The engine's repair asks both neighbours and swaps the pair inside the run.
- **Component A as written is a no-op**: `fallback_partition_sort(a, run_start, i)` sorts the run the scan has just proved sorted, so it
  costs a pass and changes nothing. As Timsort's minrun (extend to 32, then insertion-sort) it is worth about 2x on the noisy shapes:
  433k runs of 2.4 elements become 32,768 of 32, and merged elements per element fall from 20.3 to 2.9.
- **The O(1) check skipped 1.6% of merges (literal) and 0.0% (repaired) on the noisy shapes**, a third independent measurement.
- **Merge ORDER decides everything else.** The same runs, the same kernel: stitched left to right into one growing prefix a ring buffer
  merges each element 100,999 times (58-68 s); merged bottom-up in balanced pairs, 4.6 times (24 ms). A ring buffer is a sorted tail
  followed by a sorted head, so once the prefix holds the high values every later run merges through all of it.

### The defect it found, fixed (A/B in one binary against its switch, two runs of 40 pairs at 8.4M)
- **The run detector's block of eight compares with vectors** (`reloc_blocks_up` / `reloc_blocks_down`, `LOGOS_SCANBLOCK_SCALAR=1` =
  the scalar block test; `-DLOGOS_NO_SIMD_SCAN` = the scalar form everywhere, as on other targets). The pipeline's plain scan beat the
  engine 1.20-1.27x on one kinked strand, where the trace puts this scan at 89% of the sort; read from the disassembly, the loop was
  thirteen scalar `subsd` / `maxsd` / `comisd` with a serial AND chain, the prescan's defect one level down. The eight steps are now
  computed two at a time with SSE2 (baseline x86-64) or four at a time with AVX2, and the helper advances over as many blocks as pass,
  so the AVX2 form costs one call per run and not one per block. The tests are the scalar ones exactly (`!(d >= 0)` ascending,
  `!(d > 0)` or `!(d >= 0)` with ties descending), so a NaN or inf - inf fails the block and lands on the scalar line as before, and
  the max keeps its accumulator on a NaN as `d > dmax ? d : dmax` does: the run list is identical by construction. Kinks **1.30-1.32x
  at one thread** and 1.04-1.09x at 14; 64 blocks half descending **1.27-1.30x** and 1.09-1.16x; 64 batches 1.06-1.12x; 64 blocks
  overlapping 1.07x; the twelve other shapes 0.97-1.02 in both runs. After it the pipeline's kinks row reads 1.04 (was
  0.79-0.83).
- **Two defects of my own on the way, both caught by the harness before anything was timed**: the block's gap bound used the ascending
  range for descending runs too (wrong output on twisted blocks at 8 threads), and the helper returning where the old code had
  `continue` dropped the loop's `j < n` re-check, so the scalar line could read one element past a chunk's end. Both fixed; the
  harness's twist section (2,208 checks) is what failed.

### Still ahead of the engine, and open
Eight sorted logs concatenated, each spanning the whole range: the pipeline reads 0.80-0.86 over three runs at one thread. The trace names two causes (`./tstrace 1 1048576 once concat`): the knot route declines at its few-runs gate (8 runs, each spanning the range, crossing cost 3,030 = 3.0n against a gate of 1.0n, which is open direction 0m (h), where lifting it to 3.0n measured 1.6-2.9x on shapes of this kind), and then the SEAM route MISFIRES: it reads the eight log boundaries as a noisy ring buffer's single drop, rotates the whole array and re-enters, after which the leaf sorts it anyway (18.2 ms of an 18.3 ms sort). Two levers: that gate, with its own A/B against interleaved runs, and the seam gate's false positive.

### Checked
`make test` 45,899 checks, 0 failures (the total wobbles by a few between runs: the resource-exhaustion section injects as many allocation
failures as a timing-dependent run makes). 90 flag variants build, 0 failures, warnings only where earlier rounds had them, and
`-DLOGOS_NO_SIMD_SCAN` (the scalar block test and the scalar prescan everywhere, as on any other target) passes the prescan and twist
sections. ASan + UBSan + float-cast-overflow on nine touched sections: nothing new, only the known NaN-to-index casts in the two leaves and
in `skew_probe` (open direction 0h (e)), further down the file for the hoisted helpers. The edge-input probe now runs four variants (shipped,
the old hold, the scalar scan, the scalar block test) over stretches of equal +inf and -inf, descending blocks from +inf, NaNs, signed zeros
and 1% kinks, 70k to 1M elements, 1 and 4 threads: 120 rows, 0 failures, 0 sanitizer reports, and every row went through the detector.
TSan on the twist, knot route and descending ties: 0 races. faultprobe on eight touched cases: 2,396 injected allocation failures and 72
thread-creation failures, 0 bad. Frames unchanged (`psort_f64` 28,256 bytes, the detector 304, `prescan_blocks` 48). `make callbench`
against the same tree built `-DLOGOS_NO_SIMD_SCAN`, two runs: every row at n <= 5 reads 0.87-1.01 with the copies' layout cancelled, and
sorted calls of 1,000 to 50,000 elements 2.1-2.7x faster; sorted n = 24 reads 1.16-1.18 in both runs while random n = 24 reads 0.94, on
paths whose logic did not change (the prescan starts at 256, the detector's switch at 1M): the ledger's layout band, recorded, not traced.

## (unreleased) — the triple-reversal weld as proposed (condense into runs, extract edges, meta-sort by min edge, move the blocks into place IN PLACE by triple reversals, weld the seams left to right behind an O(1) boundary check): 2-4,600x behind the engine on 14 of 16 timestamp shapes at one thread and 2.6-368x at 14; the O(1) check fired on none of its seams, and the weld behind it is quadratic; its two leads were two engine defects, both fixed: the prescan's scalar compare chain (sorted 1.7-2.2x) and the run detector's hold after a wrong-way step (kinks 1.05-1.08x) (`make tsbench` modes `weld`, `weldanat`, `switch`)

> The implementation was REMOVED on 2026-09-23 and is in git history before that commit. Everything measured below stands; the engine fixes this round found are shipped and keep their own A/B switches.

Asked: the pipeline pasted as "the Triple Reversal Algorithm", with "the critical ordering of steps": (1) tangle condensation, every
descending run reversed, K sorted blocks; (2) edge extraction, a min edge, a max edge and a length per block; (3) a meta-sort of the
descriptors by min edge; (4) the physical permutation in O(1) space by triple reversals, (A^R B^R)^R = BA; (5) the seam weld: skip a
seam when the left max is at most the right min, otherwise gallop and merge only the overlap. And a rule: a strict A/B with the
engine ("normal") and with these steps; if the steps are faster, catalog what was removed and keep the newer algorithm; then a
second turn with threads.

### What was built (tsbench.cpp round three)
- `weld`: the five steps in that order. Step 1 reverses STRICTLY descending runs (stable; a tie continues an ascending run). Step 4:
  sliding each block into place by its own rotation moves n x K elements, so the rotations are scheduled by SymMerge (Kim and
  Kutzner's rotation merge) applied to whole blocks, bottom up, with the O(1) "already in order" test before every merge: O(n log^2 K)
  moves at worst, none where the blocks already lie in order. Step 5 gallops back into the sorted prefix for the elements above the
  next block's minimum and forward into the block for those below the prefix's maximum, and merges only those, the smaller side
  through a buffer, ties to the left.
- `weld, gather`: step 4 as one gather into scratch, to price the in-place constraint alone. `weld, P threads`, the second turn:
  condensation on P chunks, each SymMerge level's merges spread over P threads, the weld on P groups of blocks in parallel and then
  across the P group seams.
- `weldanat`: K, each step's time, rotations, elements moved, seams, seams bypassed and elements merged, one run per shape.
- Run at 1,048,576 elements, decided after a first anatomy at 262,144 and before any timing: the weld is quadratic on uniform data
  (24,489 merged elements per element there, 3.5 s), so 8.4M would take hours. Where one weld takes over 5 s a row is one shot of
  each kind. Every output checked.

### Measured (1M, two runs of up to 16 pairs; the full table and grades are in tsbench.cpp's header)
- **Against the engine at one thread** the weld is faster on two rows, a sorted array (0.88 / 0.92) and one strand with 1% adjacent
  swaps (0.87 / 0.87), and slower on fourteen: 2.0x on blocks overlapping by half, 3.2-3.3x on overlapping batches, 5.7-6.0x on 64
  blocks half descending, 1.05-1.13x on eight concatenated logs, 3.0x on millisecond clocks, 11-22x on the latency shapes, 32x on
  clock skew, **390-465x on a late 5% batch and 4,079-4,639x on uniform data and on jitter shuffled** (58-63 s for one sort).
- **The O(1) test never fired: 0.0% of seams bypassed, on every shape.** A maximal run swallows the next block wherever that block
  happens to start above it (K = 32 on 64 staircase blocks), so a unit spans a hole, and after the meta-sort the prefix's maximum is
  near the top of the range within a few units. The test is O(1); the merge behind it is O(prefix): 1,720 elements merged per element
  on the late batch, 98,000 on uniform (57 of 58.7 s). The edge round found this from the other side (a run's interval hides holes
  until the runs are split at gaps: J5's first lesson).
- **The triple reversals are correct and not the bottleneck, but not free.** On noisy data K is 41% of n and the SymMerge levels move
  18.5-72 n elements (172 n on the controls). Against the gather twin the in-place constraint costs 1.2-3.0x on the block shapes and
  nothing where the weld is the cost; on kinks the gather is the slower one (0.43-0.45), since nothing moves in place.
- **The second turn, 14 threads**: 2.6-3.3x behind the engine on sorted, 4.8-4.9x on kinks, 4.2-8.0x on the block shapes, 24-41x on the
  latency shapes, 46-47x on the late batch, 303-368x on the controls. Threads help the weld most where it is quadratic (uniform
  4,254x at one thread, 303x at 14: P groups divide the prefix it merges into) and least where it is cheap.

### The rule's answer
The steps are not faster, so nothing is catalogued as removed and the engine keeps its routes. The two rows the weld won were traced,
as the stop rule written before the first run required, and each was an engine defect.

### Two defects it found, both fixed (each A/B'd in one binary against its switch, two runs of 40 pairs, `TS_SWITCH=<NAME> ./tsbench 40 8388608 switch`)
- **The prescan compares with vectors** (`prescan_rise_avx2` / `_sse2` and their falling twins; `LOGOS_PRESCAN_OR16=1` = the old
  block, `LOGOS_PRESCAN_SSE2=1` = SSE2 on an AVX2 CPU, both read from 65,536 elements; `-DLOGOS_NO_SIMD_SCAN` = the scalar OR
  everywhere). On a sorted array the engine's whole sort is its prescan, and a copy of `prescan_blocks` measured level with the engine.
  Its block loop, commented "a loop the compiler can vectorize", compiled at the engine's flags (no -march) to sixteen scalar
  comisd / seta / or steps with a serial OR chain, about 0.6 ns per element; a branchy scalar scan was 1.03-1.07x faster, SSE2 compares
  1.46-1.69x, AVX2 compares 1.70-2.07x. A block now compares four doubles at a time with AVX2 (blocks of 32, where the CPU has AVX2 and
  FMA, as the AVX2 leaf asks) or two with SSE2 (baseline x86-64, no dispatch), ORs the masks and tests once; the ordered compares answer
  false on a NaN exactly as the scalar line does. `emmintrin.h` is included at the top of the file: included first inside the AVX2
  region, its intrinsics would carry that target and a baseline caller would not compile. Sorted **1.72-1.81x at one thread and
  1.67-1.79x at 14 at 8.4M, 2.17-2.22x and 2.06-2.23x at 1M**; a late 5% batch 1.09-1.13x / 1.13-1.18x; the fourteen other shapes
  0.98-1.03 in both runs. AVX2 over SSE2 1.23-1.27x on sorted. `logos_prescan_agree` now checks every kind the CPU runs against the
  scalar line (22,392 harness rows).
- **The run detector jumps to a wrong-way step** (`reloc_detect_worker_body`, `LOGOS_FASTSCAN_HOLD8=1` = the old hold). On one strand
  with 1% adjacent swaps the trace build put the detector's scan at 89% of the sort; after a failed block of eight it walked eight
  scalar steps, a gap-test call each, before trying a block again, and a kink every ~100 elements paid that every time. A block that
  fails only because one step goes the wrong way still proves the steps before it (a prefix of the block has a largest step no larger,
  a count no larger and a range no smaller), so the scan jumps to that step, the scalar line takes it and one step more, and a block is
  tried again; a block that fails the gap bound keeps the eight. The break is found with the block's own difference test, so a
  stretch of equal infinities (inf - inf = NaN) stops it exactly where the block failed. The run list is identical by construction; a
  fast advance also clears the one-step `nolook` a lifted stray sets, which a block of eight could otherwise carry eight elements past
  its step. Kinks **1.08 / 1.05 at 8.4M and 1.08 / 1.06 at 1M at one thread, 1.06 [1.00 1.13] at 120 pairs beside a null switch at 1.00
  [0.93 1.07]**; nothing at 14 threads (0.97-1.02, where the near-sorted route takes kinks); the fifteen other shapes 0.97-1.05. The
  harness twist section now requires the same kink count and twisted strands from the shipped scan, the scalar scan and the old hold.
- **After both**, the weld against the engine at 1M: sorted 1.93 (was 0.88-0.92), kinks 0.94 (was 0.87). A plain
  insertion sort still does that kinked strand 1.46-1.64x faster than the engine did before the fix: open.

### Predictions, graded
K RIGHT; rotation moves on sorted and kinks RIGHT (none), on the latency shapes WRONG (1-5 n predicted, 18.5-72 n: each SymMerge level
moves about n, and 433k units make 19 levels); "seams bypassed on disjoint blocks" WRONG (none anywhere). Weld against the engine:
sorted right at the edge, kinks WRONG (0.87, predicted 1.0-2.0), latency shapes mostly right, block shapes RIGHT, concatenated logs
WRONG (1.05-1.13, predicted 2-6), the controls WRONG by tenfold (predicted 50-500). The second turn: 10-1000 right on the noisy and
quadratic rows, below it on sorted, kinks and blocks; "worse than at one thread everywhere" WRONG on the three quadratic rows. Fix A
RIGHT at 8.4M, above at 1M; fix B at the bottom edge of 1.08-1.20.

### Checked
`make test` 45,533 checks, 0 failures; the twist section's new variant asks the shipped scan, the scalar scan and the old hold for the
same kinks and twisted strands (436 rows). 88 flag variants build, warnings only where round eleven had them; the first pass found
two defects of this change, both fixed: `-DLOGOS_NO_AVX2_LEAF` did not compile (`immintrin.h` came only from the AVX2 leaf's region)
and `-DLOGOS_NO_FASTPRESCAN` left `prescan_blocks` unused. `-DLOGOS_NO_SIMD_SCAN` (the scalar OR everywhere) passes the prescan
and twist sections. ASan + UBSan + float-cast-overflow on nine touched sections: nothing new (the three known NaN-to-index casts in
the flash and coarse leaves and the one in `skew_probe`, open direction 0h (e), seven lines further down). A new edge-input test
(sorted blocks ending in +inf ties, starting in -inf ties, descending from +inf, with NaNs, with signed zeros; 1% kinks; 70k-1M
elements; 1 and 4 threads; shipped, old hold, scalar scan): 90 rows, 0 failures, 0 sanitizer reports, and every row went through
the detector (66-10,135 kinks swapped back). TSan on three sections: 0 races. faultprobe on eight touched cases: 2,396 injected
allocation failures and 72 thread-creation failures, 0 bad. Frames: `psort_f64` 28,256 bytes (unchanged), `prescan_blocks` 48.
`make callbench` against this round's baseline (the same tree with the scalar OR): every row up to 100 elements 0.94-1.03 with
layout cancelled, two runs; sorted calls of 1,000-50,000 elements 2.0-2.7x faster. The first pass read the two A/B switches beside
the dispatcher's own and random calls of 24 elements read 1.14 in both runs (both orientations agreeing at 1.06-1.07 and
1.22-1.23) on a path whose logic had not changed; read inside `prescan_blocks` instead, 1.00 and 1.01. Against the COMMITTED engine
sorted calls of 3 to 24 elements read 1.08-1.14 (both orientations) where this round's baseline reads 0.96-1.01: older than this
round, not traced (open direction 0g).

## (unreleased) — a trend under the wave, from none through a min-to-max line to past the point where the wave stops falling: noise-free the engine collects it (to 0.2 of uniform, sorted past it); with noise it collects nothing and three routes misfire (`make sineprobe` mode `tilt`)

Asked: whether the increasing trend is taken into account, e.g. a line from the wave's minimum to its maximum applied on top of the
scalloped wave. The earlier sloped rows had a trend only at one extreme (a rise of 1 per element against a wave of 0.3 of each
cycle's rise, which comes out sorted); the user's line is the other extreme. Mode `tilt` runs the ladder: value = A wave(i) + r A i/n,
r = 0, the min-to-max line, and 25/50/90/110% of r*, the trend at which the wave stops falling (pi x cycles for the scalloped wave,
2 pi x cycles for the cosine), on the scalloped wave and the cosine, 4 and 32 cycles, noise 0 and 50, each paired against uniform
data over its own range at 1 and 14 threads, two runs of 40 pairs at 4.2M. The engine has no trend model: it sees a trend only as
runs that overlap less, and "interpolate from min to max" is its uniform grid.

### Measured (the full table is in sineprobe.c's header)
- **Noise-free, the trend is collected.** The cost falls from about 1.0 of uniform to 0.35-0.39 at 25% of r*, 0.29-0.32 at 50%, 0.18-0.21
  at 90% at one thread (0.52-0.78 at 14), and past r* the array is sorted (0.04 / 0.19-0.26). The cosine follows the scalloped wave
  at the same fraction of its own r*.
- **The min-to-max line moves the route, not the structure.** On 4 cycles it takes the cost from 1.0 to 0.55-0.58: the wave starts
  at a crest, where the leg is flat, so any rise makes a short first run, 9 runs instead of 8, past the few-runs knot gate. On 32
  noise-free cycles it takes 0.83-0.87 to 1.19-1.21: with no trend every cycle repeats the same values, and the line makes them
  distinct, after which the wave's order costs more than uniform (open direction 0i (f)).
- **With noise 50 the trend buys nothing**, 0.97-1.43 of uniform at one thread, and costs 1.39-1.58x at 14 threads in the upper half
  of the ladder. Three misfires, traced: the WAVE route accepts the noisy tilted wave, the short-reach route declines every one of its
  65 pieces, and each piece re-enters the whole dispatcher (1.42x at 50% of r*, one thread); the partition's KS certificate passes
  (0.011) and its equal-width buckets overflow the regions' 60% slack where the wave bunches values: 631,108 spills, the
  histogram-free pass thrown away, the histogram path after it (1.39-1.54x at 90%, 14 threads); the near-sorted route accepts 21%
  strays at 110% and their nested sort spills the same way (1.40-1.48x).

### Predictions, graded
Noise-free right past the line (a step where a gate flips, then a smooth fall to sorted). The line itself WRONG both ways (0.55 and
1.2 where a tie was predicted). Every noisy row WRONG (a strand with short reach was predicted; the routes misfire).

### Not changed, and why
The question was whether the trend is taken into account. The three misfires are recorded as open directions: the wave route's
per-piece fallback (sort a piece the short-reach route declines with the leaf directly, or decline the wave when its pieces'
reach exceeds their blocks); regions sized from the sample's per-bucket counts under the CHOSEN map (the KS certificate does not
bound a bucket within the 60% slack); the near-sorted route's stray budget at 14 threads.

## (unreleased) — cos() in place of sin(), and a wave whose descending leg is cos and ascending leg is sin: the phase itself costs nothing at one thread; what moved the times were two engine behaviours, both measured, neither changed (`make sineprobe` modes `cos`, `fewgate`, `legs`)

Asked: what a cosine wave does in place of the sine wave of `sineprobe`, then what a wave does whose descending leg is a cosine
(crest to zero) and whose ascending leg is a sine (zero to crest). Both built as probe modes, paired against the sine version (the
same noise draws) at 1 and 14 threads, two runs of 40 pairs at 4.2M; routes read from the witnesses and a trace build.

### Measured
- **With a slope, at one thread, a cosine is a sine**: 0.96-1.00 on five waves, the same routes. cos(x) = sin(x + pi/2), so only
  the first and last pieces change.
- **At 14 threads the phase moved the partition's lookup, not the route.** A sloped sine starts at ZERO: a[0], always a sample
  point, and a dozen values near it are tiny or negative, and in total-order key space the stretch around zero spans hundreds of
  binades. The key grid's cells go coarse, one bucket would take 21x its share, the grid is rejected and the scatter takes the value
  table; the cosine starts at its crest and keeps the key grid: **the cosine 0.85-0.88 at 14 threads** on three waves. With a
  larger swing both cross zero, both key grids fail, and the cosine's uniformity test lands just past its limit (KS 0.0322 against
  0.0273): **the cosine 1.25-1.27**. The legs wave repeats it without a slope: the sin-leg-first order starts at exactly 0.0 and
  reads 1.07-1.17 at 14 threads against the cos-leg-first order, which never goes below zero and is 0.80-0.88 of the plain
  cosine there. This is open direction 0i (a) (a sentinel the sample contains defeats the partition's certificates) with a control:
  ONE value, a[0] = 0, is enough.
- **Without a slope, the ends decide the run count.** A 4-cycle cosine has 8 runs, the sine 9, and at <= 8 runs the knot route's
  cost gate is 1.0n instead of 3.0n: it declined the cosine to the leaf and took the sine, **the cosine 1.66-1.72 at one thread**.
  Mode `fewgate` measured that gate on noise-free waves of h runs, each spanning the range, shipped against every gate at 3.0n:
  **h = 3 2.87-2.88x, 4 2.17-2.23x, 5 2.25-2.31x, 6 2.02-2.05x, 7 2.05-2.06x, 8 1.61x**, h = 2, 9, 10 and every noisy row 0.94-1.02.
  The gate was set from four INTERLEAVED runs (0.955), where the run merge really is the fallback; here the fallback is the leaf.
  A quarter cycle is sorted as a sine and reversed as a cosine (1.70-1.77, the reversal pass); 1.5 cycles gives the cosine one run
  fewer (0.90-0.97).
- **The cos-down, sin-up wave is |cos|** (a quarter period per leg: rounded crests, a cusp at zero). Each leg falls at most 0.3 pi
  per element where the full sinusoid falls 0.6 pi, so with a slope it NEVER descends: noise-free it is sorted (0.13-0.14 of the
  cosine's time at one thread), with noise one strand (0.51-0.52). Which leg comes first changes nothing at one thread (0.99-1.09).
  Read as half a period per leg, the legs join into the plain cosine: the same values as the rows above.

### Predictions, graded (in sineprobe.c's header)
One thread with a slope right. P threads wrong on four sloped rows of five, and the legs wave's P columns wrong in both directions:
the prediction had the phase acting through the routes; it acts through a[0] in the partition's sample. The 8-run cosine wrong
(a tie predicted, 1.7x measured), and the few-runs gate wrong at 3 and 4 runs (a tie predicted, 2.2-2.9x).

### Not changed, and why
The question was what a cosine does; the two behaviours it exposed are recorded as open directions with their numbers, each wanting
its own A/B before anything ships: the few-runs knot gate (3.0n at every run count, against the four interleaved runs it was set
from and the shapes the run merge takes), and a key grid windowed from the sample's trimmed quantiles, or built per sign, so that one
zero in the sample cannot coarsen it.

## (unreleased) — the edge sort as proposed (condense into blocks, sort the blocks by their min edge, rearrange, then the boundary-bypass merge sort): 1.4-9.9x behind the engine on 13 of 16 timestamp shapes and slower than the hybrid it was meant to feed; its three wins were three engine defects, all fixed: the prescan in blocks (sorted 2.2x), ties inside descending runs (tied descending blocks 6.0x at one thread), one strand's strays put back in place (kinks 2.4x) (`make tsbench`)

Asked: an outside proposal. (1) Condense the array into natural runs, reversing the descending ones (the Moebius twist). (2) Treat each
block as a black box with a min edge and a max edge and sort the K blocks by min edge, O(K log K), "computationally trivial because K
is drastically smaller than N". (3) Rearrange the blocks physically: "an ascending sine wave" whose only inversions sit where one
block's range overlaps the next, the shape the boundary-bypass hybrid of the previous entry "thrives on". Built exactly so and A/B'd.

### What was built
- `tsbench.cpp` round two: `edge` (steps 1-3, then the hybrid as described; a generous reading skips the rearrangement when the min
  edges already ascend) and `edge+trim`, the strongest form of the idea this bench can write (the same blocks merged bottom-up with
  their neighbours, a merge skipped when the edges say disjoint and otherwise cut to its overlap by two binary searches, Timsort's
  trimming). Modes `edge` (against the engine, the hybrid and std::sort at one thread; the proposal is sequential) and `edgeanat`
  (each claim measured: K, the edge sort's share, overlaps and displacement after the rearrangement, what the finish then does). Three
  shapes join the table: 64 sorted blocks shuffled with every other one descending (the proposal's own best case), 64 blocks
  overlapping by half, 8 server logs concatenated. A first smoke run took descending runs strictly, as the hybrid does, and ~1.25%
  ties cut every descending block into ~80-element pieces; the condense step now lets a tie continue a descending run.

### Measured (8.4M, one thread, two runs of 16 pairs; the full table and grades are in tsbench.cpp's header)
- **The claims.** K is 41% of N on every shape with noise in it, not "drastically smaller": blocks average 2.4 elements, and sorting
  3.46 million of them is 15-34% of the pipeline (47.5% on a newest-first log). After the rearrangement neighbouring blocks overlap on
  78-100% of boundaries on every shape but sorted, elements still sit 38 positions from home on jitter, 649 on clock skew and 6,608 on
  telemetry, and the hybrid still merges every element 18 times on every latency shape. On the proposal's own best case the min edge
  LIES: a maximal run swallows the first element of the next block, its min edge then names the wrong block, and the whole block is
  placed wrong (1.25M positions from home on average). A sensor dropout of 0.0 does the same to one real reading. The ledger has
  this failure since J5's first cut: a run's interval hides holes until the runs are split at gaps.
- **The timings.** The edge sort as written is 1.4-9.9x behind the engine on 13 of 16 shapes (jitter 7.8x, uniform 8.5-8.8x, batches
  4.2-4.3x, concatenated logs 1.44x) and it makes the hybrid it feeds SLOWER on 15 of 16 (0.59-0.90); only on the disjoint blocks does
  it help it (1.23-1.26). The trimmed form beats std::sort 8-22x on structured shapes and loses 0.68-0.82x on every noisy one.
- **Three rows the engine lost**, each a defect, each traced and fixed (below): a sorted array (edge+trim 2.2x ahead: the engine's
  prescan), one kinked strand (1.7x ahead: three full passes to put back 820 strays), 64 blocks half descending with ties (2.5x
  ahead: a tie ended a descending run).

### The three fixes (each A/B'd in one binary against its switch, two runs of 40 pairs at 8.4M)
- **The prescan in blocks** (`prescan_blocks`, off with `LOGOS_NO_FASTPRESCAN=1` from 65,536 elements or `-DLOGOS_NO_FASTPRESCAN`): the
  dispatcher's first loop, which IS the whole sort of a sorted array, ran two tests and two live flags per element. Once the first
  two unequal neighbours say which way the array starts only one test can change the answer, so the rest is read sixteen at a time
  with the results OR-ed and the scalar line takes over inside the block that fires. From 256 elements up; smaller sorts keep their
  loop untouched. Sorted **2.21-2.22x at one thread, 2.23-2.30x at 14**; sorted then reversed 1.40-1.41x; a 95% prefix plus a late
  batch 1.30-1.31x / 1.43-1.44x; guards 0.99-1.06. `logos_prescan_agree()` (a test hook, in `RENAME`) runs both lines; the harness
  asserts agreement on 22,392 rows (every position within 40 of either end and 17 in the middle, six bases, a swap or a NaN).
- **A tie continues a descending run** (the relocation detector's scalar and eight-at-a-time lines and the chunk stitch; off with
  `LOGOS_DESC_STRICT=1`): an ascending run always took equal neighbours; a descending one ended at the first tie, as a STABLE sort's
  must, and nothing here is stable (equal doubles read backwards are still in order; the twist reads a descending strand from its far
  end, and `knot_cut` sees it in ascending order with ties, as it always saw ascending strands). The tied blocks **6.00-6.10x at one
  thread, 1.32-1.35x at 14**; guards 0.98-1.03, and the two P-thread guard dips under the stop rule (0.91, 0.95, one row per run)
  were checked at 120 pairs against a switch nobody reads: 0.99-1.01 both. The leaf's own detector (`detect_runs_limited`) still ends
  descending runs at ties: not measured.
- **One strand's strays put back in place** (`strand_place_strays`, off with `LOGOS_ONERUN_COPY=1`): timed by phase, the kinked
  strand's finish was 20.7 of 34 ms (holes closed 4.9, the strand copied to scratch 7.4, the strays merged back 8.4) for 820 strays
  one or two positions from home. The short-reach route's put-back applies unchanged: between two events every element moves by the
  same amount, so the finish is one memmove per segment over only the span each stray travels. Kinks **2.36-2.37x at one thread**,
  one strand with 0.1% strays scattered across the range 1.74-1.78x, guards 0.98-1.06.

### After the fixes
The edge rows the engine lost, re-run in the same modes: logos against edge+trim on sorted 0.94-0.95 (was 0.44), kinks 1.39-1.43 (was 0.59-0.60), the tied
blocks 2.73-2.79 (was 0.40-0.41); kinks against the hybrid 1.61-1.67 (it was 0.61); against the edge sort as written: sorted 3.75-3.86 (was 1.55), kinks 2.26-2.34 (was 0.89), the tied blocks 4.96-5.06 (was 0.76); kinks against std::sort 11.1-11.2 (was 4.95). On a sorted array the trimmed form keeps a
lead of 5-6%: its scan makes one comparison per element, the prescan two until the direction is known.

### A regression older than this round, found by its callbench
Run against the committed engine as well as against its own switch, `make callbench` showed every call of 2 to 10 elements 4-5x
dearer than the committed engine (a sorted pair 17.5 against 84.7 ns; random pairs 23 against 67), in BOTH copies of this round's own
A/B, so no switch of this round was the cause. `-fstack-usage` named it: the dispatcher's frame was 60,496 bytes, 15 pages, where
round six measured 35,888 and the committed engine has 36,640. Nobody had added a stack array: GCC had begun inlining
`nearsorted_parallel_f64` (a 33 KB frame, called once) into `psort_f64`, and stack-clash protection probes every page of the frame on
every call. An inlining decision moves whenever another callee grows or shrinks. `noinline` on it and on `streams_try_f64` (13 KB):
the frame is 28,256 bytes (7 pages), a sorted pair 18.6-20.0 ns, random calls of 2-5 elements level with the committed engine
(0.99-1.02). Sorted calls of 2 to 10 elements still read 1.11-1.30 against the committed engine with layout cancelled, one
orientation of the second run reading 0.99-1.02: older than this round (the prescan against its own switch read 0.98-1.04 there),
not traced.

### Predictions, graded
Edge sort: the anatomy right on what the steps cost (block lengths, the edge sort's share, the hybrid's full merge), WRONG on what the
proposal claims for blocks; timings inside their bands on the latency shapes and the controls, WRONG on the three defect rows. The
fixes: the prescan right on sorted and reversed and above on the late batch; the ties right at one thread and low at 14; the
one-strand finish right on both rows.

### Verification
Every compile-time switch builds (81 variants, 0 failed; the warnings some print are older than this round: a route's function left unused when the route is compiled out, the committed engine prints the same). `make test`: **45,099 checks**, 0 failures (22,392 in the new "prescan in blocks" section, 27 in "descending ties", and round six's one-strand rows now run the three-pass finish too), and again after the `noinline` change. ASan + UBSan + float-cast-overflow on the prescan, descending-ties, round-six, knot-route, twist and short-reach sections: every check passed, and the only reports are the known NaN-to-index casts in the flash and coarse leaves (open direction 0h (e)). TSan on descending ties, round six and the knot route: 0 races. `make faultprobe` on nine touched cases (one_kinked, one_strays, blocks_strays, blocks_1000, block_twist, stairs_twist, ring_buffer, prefix_95, mirror_prefix): 2,876 injected allocation failures and 78 thread-creation failures, 0 bad; a failed plan allocation in the one-strand finish falls back to the three passes. `make memprobe` clean. A multi-copy bench (`make ledgerbench`) relinks with the new public symbol. `make callbench` against the engine with `-DLOGOS_NO_FASTPRESCAN`: 0.98-1.04 at n <= 5 and 0.98-1.08 up to 100 elements with layout cancelled, sorted calls of 1,000 and up 0.50-0.53; against the committed engine it found the frame regression recorded above.

## (unreleased) — server timestamps with slight knots: the engine against std::sort and against the boundary-bypass "Moebius hybrid" an outside write-up described; the hybrid's O(1) check skips 0% of merges on any shape with latency in it; two losses found and fixed (one kinked strand 0.90 -> 4.95x over std::sort; a noisy ring buffer at 14 threads 0.74 -> 0.94) (`make tsbench`)

Asked: evaluate a write-up that credits the speed of a "Moebius hybrid" to an O(1) boundary bypass (`if (a[mid] <= a[mid+1])` skip the
merge) plus a Reidemeister-I reversal of small descending "tangles", claims O(N) on data "macroscopically ascending but locally
noisy", and names sensor telemetry and distributed log merging as its applications; then A/B the algorithm on timestamp data a
server with slight knots would produce.

### What was built
- `engines/c/tsbench.cpp` (`make tsbench`, `make tstrace`): thirteen shapes at 8.4M, timestamps in microseconds near 1.7e15. A log
  written in ARRIVAL order recording SEND time (latency exponential, mean 0.5 ms); the same with 0.1% retransmits 200 ms late; one
  strand with 1% adjacent swaps; eight servers with clock skew up to +-20 ms; 64 agents' overlapping sorted batches; a ring buffer
  dump; a newest-first log; rocket telemetry with sensor noise and 0.05% dropouts reading 0.0; millisecond clocks; a 5% late batch
  appended; and two controls. Contenders: the engine, `std::sort`, and the hybrid exactly as described (reverse every descending run,
  then a top-down merge sort that skips a merge when `a[mid-1] <= a[mid]`), plus the engine against itself with every route that
  reads ORDER switched off. Every output compared bit for bit with a sorted reference. Mode `anatomy` counts what the check skips.
- **One strand** (`LOGOS_NO_ONERUN=1`, `logos_twist_last(12)`): the tsbench kinks row found it. The run detector swapped the kinks back
  and lifted the tangled ones as strays, which left ONE run, and relocation needs two: it declined and the leaf re-sorted an array that
  was sorted but for 820 elements. A single run with no strays is now a sorted array (or its mirror); a single run with strays is
  placed by the strays' own merge. Kinks **5.47 / 5.45x** at one thread, 1.01-1.03 at 14, guards 0.99-1.01.
- **The seam at P threads**: at 14 threads the short-reach route sorted every block of a noisy ring buffer before its plan saw the seam,
  then declined. The fences' single drop now declines it BEFORE the block phase at P threads (one thread still rotates): ring buffer
  **1.34 / 1.30x** at 14 threads, guards 0.99-1.02.

### Measured (three runs of the full table, the last after both fixes; details in tsbench.cpp's header)
- **The hybrid's boundary check skips 0 of 262,143 merges on every shape with latency in it**, and merges every element 18 times: a
  full merge sort. It skips 100% on sorted data and on adjacent swaps (the reversal repairs them), 99.7% on overlapping batches (which
  still merge 7.8 elements per element), 95% on a late batch.
- **Against the hybrid, the engine wins 12 of 13 shapes**: 3.0-5.9x on every latency shape, 3.5-3.8x on batches, 7.6-8.6x on the
  controls, 1.12-1.19x on sorted data. It LOSES the kinked strand: 0.11 before the one-strand fix, **0.61 after**. There the hybrid's
  two sequential passes beat the engine's five (sample, probes, detection, strays, merge).
- **Against std::sort** at one thread: 3.3-6.0x on the latency shapes, 15.7-16.2x on batches, 6.4-6.6x on the late batch, 9.6-9.7x on
  sorted, 6.3-6.9x on the controls, 4.95x on kinks after the fix, and only **1.6x on millisecond clocks** (no counting route takes 84k
  distinct values).
- **What the order-reading routes buy** (the knot view): 1.4-1.9x at one thread on every latency shape, 5.1-5.6x on kinks, batches and
  the late batch; 1.3-1.7x at 14 threads on jitter, clock skew, kinks and batches; 0.96-1.02 at 14 threads on retransmits, telemetry
  and newest first (the short-reach route's probe blocks see strays and decline there); 1.00 on the controls.

### The write-up, graded against the measurements
- "The O(1) boundary bypass skips the merge": true only where no merge block straddles disorder. Network latency always straddles, so
  on the write-up's own application (log merging) the check never fires and the hybrid is a plain merge sort, level with std::sort.
- "O(N) on macroscopically ascending, locally noisy data": the information-theoretic floor for displacement up to d is about log2(d)
  comparisons per element, not O(1); the engine's short-reach route sorts 32k-element windows where they lie and repairs the seams,
  which is O(n log B) with a small B, and that is what the latency rows measure.
- "Reverse the descending valleys (Reidemeister I)": exact for adjacent swaps (the hybrid's one win) and the engine does the same
  (kink repair); it does nothing for latency, where the disorder is not a descending run but interleaving.
- The applications are right, and the project already has a route for each: short reach for latency and sensor noise, strays for
  retransmits and dropouts, the knot route for overlapping batches, the tail route for late batches, the seam and the loop-back for ring
  buffers, the mirror for newest-first logs.

### Predictions, graded
Anatomy exactly right. Against std::sort low on most structured shapes (introsort is fast on nearly sorted input) and high on the
controls; WRONG on kinks and millisecond clocks. Against the hybrid low on the structured shapes and WRONG on kinks. The knot view low on
latency at one thread, high on batches and the late batch, WRONG at 14 threads on the stray-bearing shapes. Both fixes inside their
predictions (5-8x; 1.25-1.40x).

### Verification
Every compile-time switch builds (77 variants, 0 failed). `make test`: **22,664 checks**, 0 failures (the round-six section gained
the one-strand rows, which name their path through witness 12). ASan + UBSan + float-cast-overflow on the round-six, knot-route,
short-reach and twist sections: every check passed, and the only reports are the known NaN-to-index casts in the flash and coarse
leaves (open direction 0h (e)). TSan on the round-six, knot-route and short-reach sections: 0 races. `make faultprobe` on the new and
touched cases (one_kinked, one_strays, ring_reach, blocks_strays, sine_wave, short_reach, ring_buffer): 6,812 injected allocation
failures and 54 thread-creation failures, 0 bad. The one-strand finish allocates one stray buffer; a failure there declines to the
old path.

## (unreleased) — round six: three more places the knot view pays, SHIPPED (strays inside strands 3.0-4.2x, the wave route 1.11-1.24x, the seam 1.12-1.16x, all at one thread); two tried and reverted (a Weyl pivot for the AVX2 leaf, direct 3/4-way merges); two measured and not wired (the copy-back's price, the LIS classifier) (`make unknotprobe`)

Asked: where do the slowdowns still occur, and are there other ways of thinking about the knots (math, statistics, knotting,
unknotting, cutting and rejoining) to back-solve the sort; then "implement all features and do a/b testing to determine which
to keep". Six ideas were built, each with a written prediction, a paired A/B in one binary against its own switch, two runs of 40
pairs at 8.4M, and every output verified.

### Shipped
- **Strays inside strands** (`LOGOS_NO_STRAY=1`, `logos_twist_last(10)`): sorted blocks with late records used to pay for a full
  sort, because every stray ended a run. At one thread the run detector now LIFTS strays in five forms (below the run with its
  successor resuming it; a gap above it; one accepted into the run whose successor falls back; two side by side; and at a run's
  head, where the first step dwarfs the next two), and any run of one or two elements is strays. The list is taken at the first
  stray; past a thirty-second of the array, a density that will not fit the n/64 cap ends the scan. The holes are closed in one
  pass, the strays are sorted apart and merged back IN PLACE OF the copy-back pass; a plan that declines after the holes are closed
  puts every stray back at its position. A NaN is never lifted. Blocks + 0.1% strays **4.05 / 4.01x**, + 1% **3.26 / 3.00x**,
  + 3% (past the cap) 1.00 / 0.99, guards 0.99-1.01.
- **The wave route** (`LOGOS_NO_WAVE=1`, `logos_twist_last(8)`): a seasonal series with drift, monotone at the sample's scale and
  noisy at the element's, sorted at 1.22-1.31x the cost of random data because the run detector sees runs of two. The shared
  sample is put back in position order; a median filter finds its monotone pieces; each piece's extremum is found at the element's
  scale; each piece is sorted where it lies (reversed first if it descends; the short-reach route, relaxed; the full dispatcher if
  that declines) and the knot sweep places the pieces, merging only where they cross. Gated on the pieces agreeing with the raw
  sample (85%) and on a crossing cost from the sample (at most 1.5 merge levels per element). Sequential sorts of a million and
  up. 32 cycles swing 0.6 noise 50 **1.24 / 1.21x**, 8 cycles 1.13 / 1.11x, 16 cycles noise 20 1.12 / 1.12x; swings of 1 and 2
  periods declined (1.00-1.01); guards 0.99-1.01.
- **The seam** (`LOGOS_NO_SEAM=1`, `logos_twist_last(9)`): a NOISY ring buffer read from the wrong slot, which the loop-back (exact
  only) cannot see. The reach route's fences (block medians, read before any block is sorted) ascend everywhere but at one block,
  where they drop by most of the range: that is a rotation. The seam is the largest step down inside those two blocks; the
  dispatcher rotates once and enters again, and the short-reach route takes the rest. A second detector reads the position-ordered
  sample for arrays whose ladder failed. Sequential sorts. Rotated short reach **1.15 / 1.12 / 1.15 / 1.16x** over four runs,
  guards 0.98-1.03.

### Reverted
- **A Weyl pivot sample for the AVX2 quicksort**, on the suspicion that 63 strided samples alias with a 32-cycle wave: **0.996-1.009
  on every row**, two runs. The second diagnosis of the wave's cost in that quicksort to fail.
- **Direct three- and four-way merges** for the knot route's crossings, one pass from the source instead of stage-then-merge:
  **0.68 on three live strands, 0.52 on four**. Every step waits on two dependent compares and the load they choose; staging is two
  memcpys and branch-free two-way merges.

### Measured, not wired
- **The copy-back pass, priced** (mode `nocopy`, `LOGOS_RELOC_NOCOPY=1` leaves the output in scratch, wrong by design): block_perm
  1.33x at one thread and 1.35x at 14, stairs x20 1.21 / 1.33x. What a route that returns a sorted DESCRIPTION instead of an array
  would save: the sorted view's direction, not a sort.
- **The unknotting number as a classifier** (mode `lis`: longest increasing subsequence of the position-ordered sample, plain and
  cyclic): 10 of 11 shapes inside their predictions. It separates one strand with strays from many strands and cannot see strays
  inside blocks; no route this round needed it.

### First cuts that failed on the way (each measured, each fixed before the verdict above)
- The stray lift at first read only one form; a stray ABOVE a run was accepted into it, runs stayed ~100 long, and blocks with 1%
  strays declined at eight runs. Then a run STARTING with a stray, [stray, x, x+1, ...], had no body for the gap test and its
  interval reached down to the stray: the unknotter's Culprit, again. Then two strays side by side ended runs every ~8,000 elements.
- The first global form of the lift also lifted a NaN (the two-strays form tested neither element): the harness's knot-route NaN
  rows caught it.
- The stray list, 1 MB, was allocated on every sort: 0.94-0.96 on clean block shapes that never found a stray.
- The wave route without its cost gate: 0.76 on a swing of two periods.

### Predictions, graded
Seam right at the low end (1.15-1.25). Strays above at 0.1% (2.5-4.0), inside at 1% (2.0-3.5), WRONG on the guards and on 3% until
the reworks. Wave LOW (1.5-2.5 predicted) and WRONG on swing 2 until the gate. Pivot WRONG (1.05-1.15 predicted, 1.00). Direct
WRONG in direction (1.05-1.25 predicted, 0.52-0.68). No-copy above at one thread on block_perm, inside elsewhere.

### Verification
Every compile-time switch builds (77 variants; three failed with `-DLOGOS_NO_RELOCATE` until the wave route and the seam's sample test
got guards of their own, and the new switches' unused-symbol warnings were cleared). `make test`: 22,602 checks, 0 failures (the
round-six section 282 at the time, 138 naming their path). `make sanitize`: the 4 known NaN-to-index casts and nothing else; `make
tsan`: 0 races. `make faultprobe`: 17,414 injected allocation failures and 354 thread-creation failures, 0 bad, blocks_strays (a
failure after the holes are closed must put every stray back), sine_wave and ring_reach included. The round-six section built with the
seam and the wave compiled out: 0 wrong outputs. `make callbench` against the same source with this round's dispatcher code removed:
no row where both copy orientations agree (sorted n = 2-5 read 1.16-1.33 one way and 0.75-0.85 the other; cancelled 0.97-1.00); the
dispatcher's frame 35,856 -> 35,888 bytes, still nine pages. `make memprobe` runs clean.

## (unreleased) — the unknotting problem as a sort (Kauffman & Lambropoulou, "Hard Unknots and Collapsing Tangles"): a circular doubly linked list unknotted by Reidemeister moves is SLOWER than the engine on 15 rows of 16; what it found in the engine is SHIPPED: the loop-back (a ring buffer is one strand across the wrap, 1.56-2.03x) and the relocation floor at one thread (blocks of 256-4095 elements, 3.6-7.6x) (`make unknotprobe`)

Asked: implement the unknotting problem, guided by the paper, with Reidemeister moves acting on the doubly linked Moebius
structure, where walking forward past the end loops back to the beginning.

### The dictionary (the paper to the sort)
- The array closed into a ring (a circular doubly linked list) is the diagram; "unknotted" = it reads as ONE ascending strand
  from some node. A strand is a maximal monotone arc, with the twist's orientation bit (a descending strand is read through its
  prev links). Two strands CROSS where their value intervals overlap.
- R0, planar isotopy: on the SPHERE the outer region is a region like any other, so rotating the ring is free and a sorted array
  read from the wrong slot is already the unknot (the paper: some diagrams hard in the plane are easy on the sphere).
- R1 = the kink (already the engine's). R2 = two ring neighbours whose intervals overlap, pulled apart by a merge that touches only
  the overlap (the lower strand walked DOWN from its top, the upper one up from its bottom: what the second link is for). R3 = a
  strand slid past a neighbour it does not cross, relinked in O(1), to put two crossing strands side by side.
- HARD (the paper's hard unknot): no simplifying move, yet not the unknot. Here: no two ring neighbours cross while strands further
  apart do. The paper's escape is a move that makes the diagram MORE complex first; here the SWING, a cut at a gap (the Culprit's
  "arc that passes underneath most of the diagram" taken out of its strand), and the paper's global reading is the TANGLE: the
  strands by value fall into maximal overlapping groups, each slid together and collapsed as a whole.

### What was built
- `engines/c/unknotprobe.c` (`make unknotprobe`, `make unknottrace`), a probe. Modes: `ring` (a shape rotated as a ring buffer
  against itself, through the engine), `moves` / `count` (the unknotter: build the ring, scan with R1 and swings, balanced rounds of
  R2 between ring neighbours of comparable size, R3 slides and tangle collapse when stuck, strands read out by value; paired with
  the engine at one thread, every output compared bit for bit), `cliff` (the relocation floor's ladder), `ab` (the engine changes
  below), `once` (for the trace build).
- **The loop-back** (`loopback_gate`, `rotate_left_f64` in logos_psort.c; `LOGOS_NO_LOOPBACK=1` / `-DLOGOS_NO_LOOPBACK`,
  `logos_twist_last(7)` counts it, `(-4)` says it is compiled in). The prescan has walked the ascending prefix [0, d). If the array
  also ends in an ascending run [s, n) with a[n-1] <= a[0], that run continues into the prefix across the wrap: rotate it to the
  front once (the shorter part through the pool's scratch or malloc, the longer by one memmove; three reversals in place when no
  scratch can be had). When the suffix began at the first descent the array is now sorted and the sort returns; otherwise the
  tail route is called directly on the remainder (the joined strand is its sorted prefix), and the dispatcher is entered again
  only if that declines. Fires when the joined strand is at least
  half the array (and, when the prefix alone is, only for a suffix of n/16 or more); at P threads only when at most n/16 is left
  outside the strand.
- **The relocation detector's floor** (`RELOC_MIN_AVG_RUN_SEQ` 256, `RELOC_MIN_AVG_RUN_PAR` 4096; `LOGOS_RELOC_AVG=<k>` per sort).
  The detector behind relocation and the knot route gave up at 256 runs averaging under 4096 elements, the run MERGE's constant,
  inherited by two routes that COPY runs. At one thread it is now the knot route's own measured minimum.
- `engines/c/harness.c`: section "loop-back", 732 checks, 366 of which name what must have happened (rotated exactly once or
  never, at 1, 4 and 8 threads, on and off; equal keys at the wrap, signed zeros, infinities, a NaN at the end; five ways of not
  firing), and six rows asserting which blocks relocation placed at each thread count. `faultprobe.c`: cases ring_buffer,
  ring_prefix, blocks_1000.

### Measured (8.4M; paired, order alternated; one binary for every A/B)

The engine before this round, a shape rotated by 0.37 n against itself (`./unknotprobe 40 8388608 ring`), ceiling / told rotation:

| shape | 1 thread | 14 threads |
|---|---|---|
| sorted (a clean ring buffer) | 1.99 / 1.28 | 2.59 / 1.58 |
| short reach +-60 | 1.29 / 1.17 | 1.95 / 1.13 |
| prefix_95 + tail | 1.82 / 1.54 | 1.54 / 1.17 |
| runs by 8 coarse keys | **5.81** / 4.17 | 0.86 / 0.56 |
| near_sorted 1%, 4 streams | 0.99-1.04 | 1.00-1.37 |
| uniform, block_perm, stairs x20 (controls) | 0.97-1.01; told pass 0.74-0.94 | 0.96-1.01; told pass 0.54-0.73 |

The unknotter against the engine at one thread (`./unknotprobe 20 8388608 moves`), t(engine) / t(unknotter): near_sorted 0.36-0.37,
block_perm 0.26, staircases 0.20-0.29, prefix 0.09-0.19, sorted 0.14 (0.28 rotated), short reach 0.08-0.11, streams 0.07,
uniform 0.009; rotated keyed runs **1.40**, the engine's floor cliff below. Plane against sphere 0.96-1.09. One hard state in the
whole table (the shuffled staircase); the swing cut nodes walked per element from 21.5 to 0.00 on near_sorted, 4.8 to 1.0 on the
prefix, 7.0 and 4.0 to 0 on keyed runs and blocks, 3.9 to 1.5 on stairs x20.

The engine changes, t(switch set) / t(shipped), two runs of 40 pairs (`./unknotprobe 40 8388608 ab`):

| row | 1 thread | 14 threads |
|---|---|---|
| loop-back: ring buffer (sorted, rotated) | **1.63 / 1.56 / 1.63** | **1.90 / 1.94 / 2.03** |
| loop-back: prefix_95 + tail, rotated | 1.73 / 1.75 / 1.75 | 1.48 / 1.51 / 1.46 |
| loop-back: 30% prefix, noise, 40% suffix below it | 2.07 / 2.08 / 2.14 | 1.00 / 1.01 / 1.00 (not offered) |
| loop-back: two sorted blocks swapped | 1.56 / 1.58 / 1.57 | 1.81 / 1.78 / 1.90 |
| loop-back guards: prefix_95 as it lies, rotated short reach, near_sorted, uniform, block_perm | 0.99-1.02 | 0.97-1.02 |

(loop-back rows: two runs of the hinted form, then one of the shipped form, which calls the tail route directly)
| floor: disjoint blocks of 1000 | **4.21 / 4.16** | 1.00 / 1.00 |
| floor: staircase of 2000, shuffled | 3.58 / 3.60 | 0.99 / 0.99 |
| floor: runs by 8 keys of 3000 | 5.54 / 5.59 | 0.99 / 0.99 |
| floor: runs by 8 keys of 4000, rotated | **7.47 / 7.59** | 0.98 / 1.01 |
| floor guards: blocks of 8192, uniform, stairs x20 | 0.985-1.012 | 0.975-1.012 |

The floor's ladder (`./unknotprobe 20 8388608 cliff 256`), floor 4096 against 256: at one thread 2.6-5.5x on disjoint blocks,
1.9-4.2x on staircases, 3.2-5.7x on keyed runs from 256 to 4000 elements, 0.97-1.02 from 4096 up; at 14 threads MIXED (0.72-0.96
on small blocks and staircases, 1.15-1.41 on keyed runs); floor 1024 at 14 threads mixed too (0.91-0.98 against 1.25-1.47).

### Predictions, graded
- WRONG on the clean ring buffer, the probe's first row: predicted rotation-blind (0.95-1.05) because relocation sorts descriptors
  by value; the TAIL route took it and merged the second run element by element: 1.99. WRONG on rotated streams (predicted 1.5-3,
  1.04: the stream route tolerates one descent per stream). WRONG on rotated keyed runs (predicted 1.0-1.2, 5.8) and not about
  rotation: runs of exactly 4096 passed the detector's floor by nothing, and rotated they failed by 12 elements.
- The unknotter: every row below 1 as predicted but one (the cliff's row); lower than predicted where no move is needed (the list
  costs a node written and walked per element anyway), higher on near_sorted.
- The loop-back's FIRST CUT re-entered the whole dispatcher and measured 0.89 at one thread on the ring buffer (predicted
  1.25-1.35): the re-entry's prescan re-read an array whose sort is three or four passes long. Returning at once when the whole
  ring was one strand, and otherwise handing the prescan the known stretch, moved it to 1.56-1.63, above the prediction; every
  loop-back row then beat its prediction. **But the hint touched the dispatcher's first page** (`make callbench`, baseline = the
  working tree with `-DLOGOS_NO_LOOPBACK`): every SORTED call of 2 to 32 elements cost 10-16% more, in both orientations
  (n = 2: 16.6 -> 21.3 ns), random calls of 2 to 5 elements 2-9%, the frame unchanged at 35,856 bytes. The prescan loop's start
  had become a variable. The hint is gone: the remainder goes to the tail route directly, which is the route the re-entry chose.
  Measured again without it: sorted rows of 2 to 32 elements read 0.82-0.94 in one orientation and 1.06-1.21 in the other
  (cancelled 0.92-1.04), random rows 0.98-1.06, which by the rule (both orientations, n <= 5) is layout, not a cost.
  The A/B run once more in the shipped form (tail route called directly, 40 pairs): ring buffer 1.63 / 2.03, rotated prefix
  1.75 / 1.46, 30% + noise + 40% 2.14 / 1.00, two blocks swapped 1.57 / 1.90, guards 0.97-1.02.
- The floor landed inside three predictions of four and above the fourth.

### Not kept: forgiving each detection chunk its partial first run (predicted 1.1-1.4 at 14 threads on runs of exactly 4096)
0.974-1.026 on every row at both thread counts, two runs. The diagnosis was of the wrong condition: at P threads the per-thread run
cap (n / floor / P + 2) binds before the average test, since a chunk of exactly-4096 runs holds 148 counting its two partial pieces.
Reverted.

### Not built: a seam finder for NOISY ring buffers
Rotated short reach costs 1.29x at one thread and a told rotation recovers 1.17x, below the stop rule of 1.25 written before the
first run. The cheap design, for when it is wanted: the sample's inverted Fibonacci-lag pairs cluster at the seam.

### Verification
Every compile-time switch in the source builds (67 variants, the pairs touching this round's code included). `make test`: 22,322
checks, 0 failures (the loop-back section is 732 of them, 366 naming the path taken). `make sanitize`: the 4 known NaN-to-index
casts and nothing else; `make tsan`: 0 races. `make faultprobe`: 11,532 injected allocation failures and 324 thread-creation
failures, 0 bad, the three new cases included. The harness built `-DLOGOS_NO_LOOPBACK` passes its section with the witness
saying off. `make memprobe` unchanged. After the last engine edit (the hint removed) the flags, `make test`, the loop-back section
under ASan/UBSan/float-cast-overflow (0 reports) and TSan (0 races), and the touched fault cases (652 failures, 0 bad) ran again
on the final source; `make callbench` against the working tree built `-DLOGOS_NO_LOOPBACK`: no row where both orientations agree.

## (unreleased) — the proposed "Moebius twist pointer routing" sort, implemented as given and measured: 2-80x SLOWER than the engine on every shape (`make dllbench`, `experiments/moebius_dll/`; no engine change)

Asked: implement the JavaScript from an outside conversation (array -> doubly linked list; runs found by walking it; a descending
run read back through `.prev` pointers; runs extracted and merged pairwise), which it said "should be faster than the current",
translate it to a lower-level language, and A/B it.

### What was built
- `experiments/moebius_dll/moebius_dll.js`: the JavaScript exactly as given, in a module. `bench.js` pairs it with the JS engine
  (npm logos-sort) and with V8's `Array.prototype.sort`, on a fresh copy of the same `Array<number>` each time, every output checked.
- `engines/c/dllbench.c`, two translations: `faithful` (the JavaScript as written; one node array, which is kinder than a node per
  allocation) and `relink` (the conversation's own pure-C form: descending runs turned round by swapping next and prev, runs merged
  by rewriting links, the final list walked out). Paired with `logos_sort_f64` at ONE thread, since both are single-threaded.
- What it is: a natural merge sort (run detection, descending runs taken backwards, bottom-up pairwise merges) with a linked list in
  front. O(n log R) for R runs, so O(n log n) as soon as noise breaks the runs up.

### Measured (t(engine) / t(Moebius); below 1 = how much slower the Moebius sort is)

| shape | C faithful, 8.4M / 1M | C relink, 8.4M / 1M | JS vs logos-sort, 1M / 100k | JS vs V8 sort, 1M / 100k |
|---|---|---|---|---|
| sine + slope, clean | 0.18 / 0.16 | 0.18 / 0.23 | 0.13 / 0.24 | 0.20 / 0.38 |
| sine + slope, noise 50 | 0.29 / 0.29 | 0.20 / 0.23 | 0.11 / 0.12 | 0.71 / 0.81 |
| descending teeth x64 | 0.51 / 0.46 | 0.13 / 0.17 | 0.15 / 0.30 | 0.76 / 0.86 |
| uniform | 0.10 / 0.11 | 0.02 / 0.04 | 0.08 / 0.07 | **1.18 / 1.33** |
| near_sorted 1% | 0.31 / 0.37 | 0.17 / 0.27 | 0.12 / 0.12 | 0.55 / 0.54 |
| block_perm 64 | 0.09 / 0.09 | 0.09 / 0.13 | 0.09 / 0.19 | 0.19 / 0.38 |
| few_distinct | 0.03 / 0.03 | 0.01 / 0.02 | 0.07 / 0.10 | 0.49 / 0.79 |

- **No row clears the stop rule (1.05 against the engine), in either language, at any size.** The two sine rows it was written for
  are 3.4 to 9x slower. The closest row, the descending teeth at 0.46-0.51 in C, is the one where its real idea (read a descending run
  backwards) is the whole job, and the engine already does that (the twist, `rdesc_t.rev`).
- **Why.** The list costs a 24-byte node written per element and a pointer walk before any sorting happens; the pairwise merge makes
  log2(R) full passes where the engine copies disjoint runs in one (blocks), counts (few distinct), sorts noise where it lies (short
  reach) or runs a vectorised quicksort (uniform, noisy sine). The one win, 1.18-1.33x against V8's own sort on uniform data, is V8
  calling a comparator function per compare while this merge compares numbers inline; the JS engine is 12x faster again on that row.
- **Predictions, graded.** Every row below 1, as predicted. Lower than predicted on the clean sine (C 0.3-0.7, JS 0.3-0.8), uniform,
  blocks and few-distinct, because the engine is faster there than I allowed; inside on the noisy sine and the teeth. The relink form
  was predicted at 0.3-0.7 of the faithful one: right on uniform, teeth, noise and few-distinct, WRONG on the clean sine and blocks
  (1.0-1.45 of it): with a few long runs a relinking merge reads nodes nearly in memory order and saves the copy out. Against V8's
  sort the prediction (0.2-0.7 everywhere) was wrong on uniform (1.18-1.33) and low on noise and teeth (0.71-0.86).
- **Not integrated; kept as a probe.** The engine is unchanged by this entry, so `make test` and the checkers were not re-run for it.

## (unreleased) — a galloping two-strand merge for a sparse strand over dense blocks (2.4-2.7x at one thread); the dense-integer bitmap's budget 4n -> 16n (3.7-8.7x on spans 6n to 16n) (`make knotbench`, `make mcbench`)

The two changes the last answers recommended, each with a ceiling or a ladder first and a paired A/B in one binary.

### 1. The galloping two-strand merge (`knot_merge2g_*`; off with `LOGOS_NO_KNOT_GALLOP=1`)
- **The shape.** One sparse strand spanning every value over dense disjoint blocks (a late column, a sparse index over dense pages).
  The knot route already took it at both thread counts (a trace: `crossing = n`, `kmax = 2`, cost exactly at the 14-thread gate of
  1.0n), and its branch-free merge then touched every element one compare at a time: every block was a crossing of a few sparse
  elements and the whole block. The gate was never the problem; the kernel was.
- **Ceiling first** (`./knotbench 30 8388608 ceiling`): each sparse shape against its twin with the sparse values moved below every
  block, so the same runs lie in the same places and nothing crosses. One thread **2.42-2.75x**, 14 threads 1.03-1.16.
- **What was built.** Eight branch-free steps while the strands alternate; when the element eight places ahead in one strand still
  wins, a gallop (steps of 8, 16, 32 and a bisection) and ONE bulk copy of the stretch, forward or backward for a twisted strand.
  Ties go to the first strand exactly as in the branch-free merge, so the output is the same bytes, signed zeros included. One
  function per orientation pair, generated by one macro.
- **A/B, one binary, one engine copy** (`./knotbench 60 8388608 gallop sparse`), t(`LOGOS_NO_KNOT_GALLOP=1`) / t(shipped):

| shape, 8.4M | 1 thread | 14 threads |
|---|---|---|
| sparse strand 1 in 65 over 32 blocks | **2.45** | **1.18** |
| the same, sparse strand descending | **2.56** | **1.31** |
| 1 in 513 | **2.70** | **1.23** |
| 1 in 4097 (a few thousand late records) | **2.70** | **1.22** |
| 1 in 9 (stretches of eight: the threshold) | 1.03 | 0.99 |
| staircases x20 / x80 (200 and 120 pairs) | 1.002 / 1.004 | 1.026 / 1.008 |
| every other knot bench row: blocks, x10-x100, triple, quad, k8, ordered, zigzag, shards, guards | 0.97-1.03 at 120 pairs | 0.97-1.03 |

- **Prediction, graded.** 1 in 4097 2.0-2.3x, 1 in 513 1.9-2.2x, 1 in 65 1.5-1.9x at one thread: all LOW, the gallop recovers its
  whole ceiling (the bulk copy is as fast as the copy it stands in for). 14 threads 1.00-1.08: LOW, 1.18-1.31. 1 in 9 0.95-1.05 and
  the staircases 0.98-1.02: held, after the wrong turn below.
- **One wrong turn.** The first cut ran the lookahead on EVERY two-strand crossing. The staircase where four elements in five are
  merged read 0.980, 0.948 and 0.976 over three runs (40, 120 and 200 pairs), where every other row sat at 1.00. The call site knows
  both piece lengths and a gallop can only go far when one piece is much longer, so the kernel now runs only when one piece is at
  least `KNOT_GALLOP_RATIO` (4) times the other; a balanced crossing keeps the branch-free kernel instruction for instruction. After:
  1.004 at 200 pairs.
- **Harness:** a section of its own (`gallop_checks`, **666 checks, 270 naming what must have happened**): the four orientation
  pairs, stretches at the threshold (1 in 9, 1 in 10), gallops of four thousand, ties exactly at a gallop's boundary (integer-valued
  blocks with the sparse strand on the same integers), signed zeros tied across the two strands; every row against qsort, BYTE FOR
  BYTE against the branch-free merge, and with the witness `logos_twist_last(5)`: gallops where there must be, none with the switch
  off. **Mutation-tested:** a twisted stretch copied from a base one element off fails 198 checks; letting the second strand win ties
  inside a gallop fails 9, all in the signed-zero rows, through the byte check. A third mutant (stopping a gallop one element early
  on a tie) passes, and reading it, it must: the next branch-free step emits the same element, so the output cannot change.

### 2. The dense-integer bitmap's span budget: 4n -> 16n (`DENSE_SPAN_MULT`; `LOGOS_DENSE_SPAN=<k>` overrides it per sort)
- **My recommendation rested on a misreading.** I said the route accepted integers "spanning about n". It accepted up to 4n already
  (a 6n window). The question that remained was whether a wider budget pays.
- **The ladder first** (`./mcbench 40 8388608 span`): n DISTINCT integers spread over s x n, shuffled, ONE thread (the route is
  sequential-only), the shipped budget against the gate lifted to 256n. Up to 4n both sides take the bitmap (the null rows, 1.00-1.02);
  above it each row is the bitmap against the leaf, which for these values is the int32 radix.

| span | 6n | 8n | 12n | 16n | 24n | 32n | 48n | 64n | 96n | 128n |
|---|---|---|---|---|---|---|---|---|---|---|
| 8.4M, bitmap / leaf | **4.97** | **4.27** | **3.91** | **3.33** | **2.87** | 1.21 | 1.41 | 1.74 | 0.97 | 1.12 |
| 1M (first run, lifted to 64n) | 4.01 | 6.86 | 5.28 | 5.64 | 3.66 | 5.55 | | | | |

  Guards the lifted gate lets in and the bitmap must give back, 8.4M: random integers with repeats 0.99 at 16n and **0.73 at 64n**,
  one repeat planted mid-array **0.89 at 16n** and 0.73 at 64n; wide int32 and uniform doubles 0.98-0.99 (never admitted). The waste
  grows with the map because every freshly touched page is a fault.
- **Prediction, graded.** 6n 1.4-1.8x, 8n 1.3-1.7x, 16n 1.0-1.3x, a loss by 32n: all far too LOW. I priced the leaf as if it were
  fast on integers; it is the int32 radix (35 ns per element on int_wide, in the profile two rounds ago).
- **Ship rule, written before the confirming run:** the largest budget where the clean spreads win at least 1.5x and no guard drops
  under 0.85. That is 16n. For scale, the 4n budget already accepts 0.79 on the planted-repeat shape at span n.
- **A/B of the new default against the old** (`./mcbench 40 8388608 span - 4`, one binary, `LOGOS_DENSE_SPAN=4` on one side):

| span | 2n | 4n | 6n | 8n | 12n | 16n | 24n-128n |
|---|---|---|---|---|---|---|---|
| 8.4M | 1.05 | 1.02 | **5.05** | **4.24** | **3.80** | **3.66** | 0.99-1.01 |
| 1M | 1.01 | 1.01 | **7.52** | **8.72** | **6.04** | **6.36** | 0.98-1.02 |

  Guards: repeats 16n 0.99 / 0.97, repeats 64n 0.99 / 0.97, **planted 16n 0.883 / 0.943** (the one row that pays, inside the rule),
  planted 64n 1.02 / 0.99 (declined by the gate, as before), int_wide 1.02 / 1.01, uniform 1.00 / 1.01 (8.4M / 1M). Ship rule met.
- **The harness never reached this gate.** Its rows "span 4n: at the budget" and "span 8n: must decline" were SORTED arrays
  (`i * 4`, `i * 8`), so the prescan returned before the route was asked, and nothing said which path ran. New shuffled rows name it
  through a witness, `logos_twist_last(6)`: taken at 6n, 12n, 15n and with a negative base; declined by the gate at 24n and 40n;
  given back after one planted repeat, one far value, one -0.0; never offered to a parallel sort. Under `LOGOS_DENSE_SPAN=4` exactly
  the eight rows that expect the bitmap fail, which is the proof that they test it.

### Verified
- `make test` **21,554 checks**, 0 failures (695 new: the gallop section's 666 and the dense route's rows by name); all 61 compile-time switch variants build; `make sanitize` clean but for the four known NaN-to-index casts in the leaves; `make tsan` 0 data races (the gallop witness is one relaxed atomic add per placement worker); `make faultprobe` over 34 cases, `dense_spread` and `sparse_over` among them: **10,884 allocation and 306 thread-creation failures, 0 bad** (a failed bitmap allocation declines to the leaf). The dispatcher's frame is unchanged (35,856 bytes); the only per-call additions are a `getenv` in `knot_env`, reached from 512 elements up, and one in the dense route, reached by sequential sorts from 65,536 up.

## (unreleased) — three changes taken from an outside write-up: kinks repaired in the run detector (won, 3.8-5.8x); the noisy sloped sine's gate fix (built, NEUTRAL, reverted, and the diagnosis behind it was wrong); four shapes added to the scale report (`make strandprobe`, `make sineprobe`, `make scale-full`)

An outside conversation about a "Moebius unknot sort" was read against this engine. Most of it was already here (runs as
strands, the twist, the early-out merge, measured bail-out constants) or already measured false here (a literal linked list,
the golden-ratio cut, cubic unknotting costs); no sort was timed in it. Three things were worth taking, and all three were done.

### 1. Reidemeister move I: a KINK is swapped back where it stands (`LOGOS_NO_KINK=1` or `-DLOGOS_NO_KINK` = off)
- **What a kink is.** One adjacent pair swapped inside a run that otherwise goes on: `[.. 4 6 5 7 ..]`. It ended the run exactly
  as a stray does, and a few thousand of them pushed the average run under the detector's floor of 4,096, so an array of sorted
  blocks paid for a full sort. It is the cheap subset of "strays inside strands" (open direction 0l (a)): the misplaced element
  sits NEXT TO its hole, so the strand stays contiguous and every binary search and offset of the knot plan still holds.
- **What was built.** At a run break the detector asks both neighbours: in an ascending run, `a[j] < a[j-1]`, `a[j] >= a[j-2]`
  and `a[j-1] <= a[j+1]`; mirrored, strictly, in a run that lies descending. If they agree the pair is swapped and the run goes
  on. Both neighbours must agree, so noise is not bubbled and a turn into a descending run is not mistaken for a kink. Never
  across a detection chunk's end, where the next chunk's thread may be writing. **The detector therefore writes again, two
  elements per kink and nothing else**; a declined probe still leaves a permutation.
- **Ceiling first** (`./strandprobe 24 8388608 kinks`): the engine on the kinked array against the engine after a pass TOLD where
  the kinks are. One thread 5.5-5.6x on blocks at 0.1% and 1% kinks, 4.5 at 4.8%, 4.0-4.3 on staircases; 14 threads 1.15-1.44;
  the 0% rows 1.00-1.02 (this teller costs nothing, unlike the strays one). Predicted 6-10x and 1.5-1.9x: too high at both counts.
- **A/B, one binary, one engine copy** (`./strandprobe 40 8388608 kinkab`), t(`LOGOS_NO_KINK=1`) / t(shipped):

| shape, 8.4M, 40 pairs | 1 thread | 14 threads |
|---|---|---|
| block_perm, 0.1% / 1% / 4.8% kinks | **5.81 / 4.98 / 3.22** | **1.52 / 1.28** / 1.08 |
| block_perm, every other block twisted, 0.1% / 1% | **5.68 / 4.50** | **1.44 / 1.30** |
| stairs x20, 0.1% / 1% / 4.8% kinks | **4.24 / 3.83 / 2.67** | **1.35 / 1.24** / 1.02 |
| stairs x20, every other block twisted, 0.1% / 1% | **4.26 / 3.77** | **1.36 / 1.35** |
| guards: blocks and stairs with no kinks, uniform, near_sorted 1% | 0.98-1.02 | 0.95-1.05 at 40 pairs; 0.98-1.01 at 120 and 160 |

- **Prediction, graded.** One thread 3.8-5.3x (3.8-5.8 up to 1% kinks: held, the 0.1% rows just over; the 4.8% rows read 2.7-3.2,
  65-70% of their ceiling, because every kink costs a scalar step and eight held elements of the fast scan). 14 threads
  1.10-1.40 (1.24-1.52 up to 1%: the top just over). Guards 0.98-1.02: held after a second and third run (the uniform 14-thread
  cell read 1.03, 0.95 and 1.01). Ship rule (no guard under 0.98, the 1% rows over 2x at one thread): met.
- **Harness:** two kinked shapes (blocks and staircases, every other block twisted, 1% kinks) and a `LOGOS_NO_KINK` variant in
  the twist section, 491 more checks; the rows NAME it through `logos_twist_last(4)`: kinks swapped back where they must be,
  none with the repair off. One expectation had to go: with the knot route off, a kink on a block's LAST pair is no kink (its
  far neighbour disagrees), the pair that is left fuses with the next block, and plain relocation may decline (the short-tail
  fusion of open direction 0j (b), a third sighting).

### 2. The noisy sloped sine: the fix was built, measured NEUTRAL and reverted, and the diagnosis was wrong
- Open direction 0i (f) said: a sloped wave with noise costs 1.2-1.3x a sort of random data at one thread because the 40 strided
  pairs call it nearly sorted, `AVX2_TAKE` then keeps it off the AVX2 quicksort and on the coarse leaf, and the dense-disorder
  probe should overrule that: predicted 1.45x.
- **Built** (`c->nearly = 0` when `dense` fires) **and A/B'd in one binary** (`./sineprobe 40 4194304 ab`): **1.02** on the noisy
  32-cycle rows, 0.97-1.06 elsewhere. Neutral. Reverted; the code keeps a comment saying so.
- **Why the prediction failed: the diagnosis came from a PROFILE build.** That build timed the coarse leaf at 126 ms and the AVX2
  leaf at 87 on this shape. Paired in a normal build the two leaves cost the same on it. A trace confirms the switch does what
  it says (policy `nearly=1` takes coarse, `nearly=0` takes the AVX2 leaf); the time does not move.
- **Where the order cost really is.** `LOGOS_NO_AVX2=1 ./sineprobe`: the wave reads 1.05 against uniform and ITS OWN VALUES
  SHUFFLED read 1.05. With the AVX2 leaf on: 1.19 and 1.01. So the scalar leaves do not care about this order and **the AVX2
  quicksort does**: it is 1.17x slower on the wave than on the same numbers shuffled. Not traced further. First suspect, and
  only a suspect: its pivot is the median of 63 STRIDED samples, and 32 cycles against 63 strides is a phase step of half a
  cycle, so the sample alternates crest and trough.
- The lesson is already in the ledger (a trace build times nothing; a phase trace taken hot overstates a phase) and it was
  broken anyway: **a profile build's leaf times were used to rank two leaves.** Only a paired A/B in a normal build ranks them.

### 3. The scale report carries the shapes these rounds were about
- `scalebench.cpp` gained `block_twist` (every other block descending), `block_kinked` (1% adjacent swaps), `sine_slope` (32
  cycles, noise of +-50) and `reach_mirror` (short reach handed over back to front): thirteen shapes, every sort, 0 to 3e7
  elements, time and ns per element, with the zoom and pan from the round before. That is the chart the outside write-up drew
  from formulas, measured.
- Both report checkers pass on the new file: `node tools/check_report.js` drives 312 charts in six views, `python tools/browser_check.py` drives real Chrome in all six modes. Inside the 10M rows, one thread, logos against x86-simd-sort's AVX2 kernels: block_twist 25.5 ms against 176, block_kinked 39.4 against 181, reach_mirror 127 against 156, and **sine_slope 251 against 162: logos loses that row**, which is item 2's open question seen from outside. At 14 threads logos leads all four (13.8 / 16.7 / 23.7 / 35.2 ms against 43.0 / 47.2 / 60.6 / 40.1).

### Verified
- `make test` **20,859 checks**, 0 failures (491 new in the twist section for the kinked shapes and the no-kink variant); all 61 compile-time switch variants build; `make sanitize` clean but for the four known NaN-to-index casts in the leaves; `make tsan` 0 data races (the kink swap never crosses a detection chunk's end); `make faultprobe` over 32 cases, a kinked one among them: **10,306 allocation and 288 thread-creation failures, 0 bad**. The reverted gate fix left the engine's logic as it was; its trace line stays.

## (unreleased) — the knot route as a LITERAL linked list: built, measured, 1.5-2x slower, kept opt-in (`LOGOS_KNOT_LIST=1`, `./twistbench 60 8388608 list`)

Asked: is the array first converted to a linked list? It is not and never was. The route was first DESCRIBED that way; what
shipped keeps the idea (runs as strands, comparisons only where they cross) and not the structure: an array of run descriptors
sorted by first value, pieces placed by copies and merges. Asked next: build the literal one. Built, to be measured.

### What it is (`knot_place_list`)
- **A 32-bit link per element.** Inside an ascending piece `next[i] = i + 1`, inside a TWISTED piece `next[i] = i - 1`. In a list
  the twist is free: a descending run is a run whose links point the other way, and the twisted rows measure the same as the
  untwisted ones (0.513 against 0.515).
- **A crossing is untied by RELINKING.** The pieces' sublists are merged by rewriting links; no element moves. Crossings of
  three or more fold left.
- **The units are stitched tail to head**, and the array is then one sorted linked list. One thread walks it from its head,
  all n elements, through every stitch. P threads walk each unit from its own head, because a list cannot be entered in the
  middle (a lone piece can: its address is known at any offset, so it is cut into grains). Then dst is copied back.
- It reuses the knot plan for WHERE strands cross and replaces the placement. Phases are queue-driven behind `par_spawn` and a
  barrier, so a worker that cannot be created is dropped and the others drain its share. If the links cannot be allocated, or
  n does not fit 32 bits, it declines without touching anything and the array placement runs.

### A/B, one binary, one engine copy: t(array of descriptors, shipped) / t(linked list). Above 1 would mean the list is faster.

| shape, 8.4M, 60 pairs | 1 thread | 14 threads |
|---|---|---|
| 64 blocks, none / all / every other twisted | **0.52 / 0.51 / 0.50** | 0.69 / 0.63 / 0.67 |
| 1024 blocks of 8k, all twisted | **0.51** | 0.68 |
| staircase x20, none / all / every other twisted | **0.55 / 0.57 / 0.56** | 0.63 / 0.63 / 0.62 |
| staircase x80, all / every other twisted | **0.75 / 0.73** | 0.65 / 0.64 |
| uniform (never reaches the route) | 1.01 | 1.01 |

Every placement went through the list (the bench prints the witness: 120 of 120 sorts per row, 0 on uniform).
**It weighs 4 bytes per element more** (`LOGOS_KNOT_LIST=1 ./memprobe`: 15.87 -> 19.87 at one thread, 9.06 -> 13.06 at 14).

- **Prediction, graded.** One thread: blocks 0.60-0.72 (**0.50-0.52: too kind**), x20 0.60-0.72 (0.55-0.57: too kind), x80 0.50-0.62
  (**0.73-0.75: WRONG the other way**). 14 threads 0.75-0.92 (**0.62-0.69: too kind**). Plain pointer walk 0.40-0.55 (0.43-0.44:
  held). Uniform 1.00: held. The x80 miss is the one thing the list is good at: a relinking merge moves no data, so where
  four elements in five are merged the list gives up least. Everywhere else it pays a link written, a link read and a scalar
  walk per element for work a memcpy does eight bytes to the cycle.
- **The walk's best form, and the wrong turn inside it.** A plain list walk is one DEPENDENT load per element. The known
  answer is to GUESS next = cur + 1 (or cur - 1 in a twisted run) and let the load only check the guess, so that inside a
  run the branch predictor breaks the chain. The first cut measured exactly what the plain walk measures (0.43 against
  0.44), and the assembly said why: on the path where guess and link compare equal the compiler continued from the LINK,
  which is legal and puts the dependency straight back. Laundering the difference through an empty `asm` keeps the equality
  from it: blocks 0.43 -> 0.51, x20 0.49 -> 0.56 at one thread; nothing at 14 threads, where fourteen chains already run
  side by side. **If a value-speculation trick measures the same as the code it replaces, read the assembly.**
- **Verdict.** Not a candidate: no row is near 1. Kept opt-in, as `LOGOS_MC_CONCAT` is, because it is the structure the route
  was named after and the measurement is only reproducible while the code exists.

### An opt-in path is a path nobody runs, so it was run
- A harness section of its own (`list_checks`, **504 checks, 144 of them asserting through `logos_twist_last(3)` that the
  placement went through the list**): twisted and untwisted blocks and staircases, a run straddling chunk boundaries, three live
  strands, a bitonic pair, equal keys, infinities, tiny strands; 1, 4 and 8 threads; both walks; the cost gate as shipped and lifted.
- The knot and twist sections with `LOGOS_KNOT_LIST=1` set from outside: 1,684 checks, both walks. The same under ASan + UBSan +
  float-cast-overflow (only the known NaN casts in the leaves) and under ThreadSanitizer (0 races: the stitch writes unit tails
  only, and no walker reads its unit's last link).
- `LOGOS_KNOT_LIST=1 ./faultprobe` on five run-route shapes: 2,356 allocation and 54 thread-creation failures, `list_worker`
  among the creations failed, 0 bad. `make test` 20,370 checks, all 60 switch variants build.

## (unreleased) — round four, twist and knot: the run detector scans in blocks of eight; one orientation bit for the whole array; relocation beats the partition at 14 threads again; two probes (writhe, strays) (`make twistbench`, `make mirrorprobe`, `make strandprobe`, `experiments/TWIST_AND_KNOT.md`)

Asked: do what `experiments/TWIST_AND_KNOT.md` suggested, and A/B it. Its order was the free classifier first, then the two
cheap stop-or-go measurements, then builds. Two things were built and both won; two were measured and not built; one defect
from the round before turned up on the way.

### The profile said go: the run detector's scan, eight elements at a time (avenue 3b; off with `LOGOS_NO_FASTSCAN=1` or `-DLOGOS_NO_FASTSCAN`)
- **The check that had to pass first.** `make profile-full`'s block rows: the detector's scan is **47-49% of the run routes at one
  thread** (1.1 ns per element, for a READ) and 31% at 14. The stop was 20%.
- **What it is.** Since the twist the detector writes nothing, so a run can be extended eight elements at once whenever a bound
  PROVES the scalar test would have accepted each of them: all eight steps go the run's way, and the largest step times the
  largest count the block can reach stays under the gap limit for the smallest range it can have (the range only grows inside a
  run). Rounding is monotone in every operand, so the bound holds in floating point as on paper; a NaN or inf - inf fails the
  sign test and lands on the scalar line, which is unchanged. **The run list is identical by construction**, and the harness
  checks it: a scalar-scan variant of every twist row must place the same number of twisted strands (362 new checks).
  After a block fails, eight scalar steps pass before the next attempt, so data hugging the limit cannot thrash.
- **A/B, one binary, one engine copy** (`./twistbench 60 8388608 scan`), t(scalar) / t(blocks of eight):

| shape, 8.4M, 60 pairs | 1 thread | 14 threads |
|---|---|---|
| 64 blocks, none / all / every other twisted | **1.46 / 1.43 / 1.46** | **1.22 / 1.16 / 1.18** |
| 1024 blocks of 8k, all twisted | **1.49** | **1.18** |
| staircase x20, none / all / every other twisted | **1.36 / 1.33 / 1.34** | **1.19 / 1.18 / 1.18** |
| staircase x80, all / every other twisted | **1.19 / 1.20** | **1.16 / 1.17** |
| uniform (guard) | 1.003 | 0.981 |

- **Prediction, graded.** One thread 1.30-1.45 on blocks and x20 staircases (1.33-1.49: the block rows just over), x80 1.10-1.20
  (held), uniform 1.00 (held). 14 threads 1.05-1.15: LOW, measured 1.16-1.22; I assumed the parallel scan already read at the
  memory's pace, and it did not. Ship rule (no row under 0.98, block rows over 1.15 at one thread): met.

### Which changed an old verdict: relocation against the PURE partition at 14 threads
Open direction 0f recorded that the run routes no longer beat the partition at 14 threads (0.95 on block_perm, a tie of
0.96-1.11 on staircases) and told the next reader to re-measure before building on them. Re-measured, the same way: an engine
copy built `-DLOGOS_NO_RELOCATE -DLOGOS_NO_KNOT` against the shipped one, both copy orientations, sqrt(A/B), 60 pairs, 8.4M.

| shape | 14 threads | 1 thread |
|---|---|---|
| block_perm | **1.077** (was 0.95) | **5.77x** (was 4.4-4.5) |
| stairs_x20 | **1.165** (was 0.96-1.11) | **4.74x** |

Predicted 1.08-1.16 and 1.10-1.25 from the scan's own A/B: block_perm at the edge, staircase inside. The parallel run routes
are a win again, by the margin of one pass they no longer pay at full price. One run; 1.077 is two floors above 1.

### One orientation bit for the WHOLE array (open direction 0k, plain form; off with `LOGOS_NO_MIRROR=1` or `-DLOGOS_NO_MIRROR`)
- **What it is.** On a sequential sort, BEHIND relocation, `mirror_gate` asks three directional gates about the array's MIRROR
  image: a descending suffix of at least half the array (the mirror of a sorted prefix), the reach ladder with every comparison
  turned round, and the stream probe asking whether the modes DESCEND in arrival order. If one says yes the buffers are
  released, the array is reversed once, and the dispatcher is entered again, where the prescan, the ladder and the stream
  route now read it their way. Noise never gets that far: the first two gates run only when the sample's writhe says mirrored
  (`inv_all >= 0.90`, noise reads 0.50), the third only on a multimodal sample and it stops after ~25 reads on data that ascends.
- **A/B, one binary** (`./mirrorprobe 40 8388608 ab`, the three strayed rows again at 120 pairs), t(off) / t(on):

| shape, 8.4M | MIRRORED, 1 thread | MIRRORED, 14 threads | as it lies (guard), 1 / 14 |
|---|---|---|---|
| short reach, noise of +-60 | **2.07** (2.075, 2.067) | 0.99 | 1.00 / 0.98 |
| 95% sorted prefix + random tail | **2.20** | 1.01 | 1.00 / 1.00 |
| 4 interleaved streams | **2.76-2.81** | 1.02 | 1.01 / 0.98 |
| near_sorted 1%, grouped runs, uniform, block_perm, stairs | 0.99-1.01 | 0.99-1.01 | 0.99-1.02 |

- **Prediction, graded.** Short reach 1.85-2.00 (2.07: just over), prefix 2.30-2.50 (**2.20: LOW**, the descending-suffix scan
  is a read of 95% of the array that the `simple` ceiling never paid), streams 2.80-3.10 (2.76-2.81: at the edge). Guards
  0.98-1.02: held once the three cells that strayed at 40 pairs (0.95-0.97) were re-run at 120 (0.98-1.02). Ship rule met.
- **One wrong turn.** The first cut pre-gated the stream probe on the writhe too (`inv_all >= 0.60`). The harness rows that NAME
  what must happen failed at 1,000,003 elements: whether a sampled pair falls inside one stream depends on the lag modulo the
  stream count, so four mirrored streams read 0.88 at 1,048,576 elements and under 0.60 at 1,000,003. The stream probe is now
  asked whenever the ascending one would be.
- **Not covered, on purpose:** parallel sorts (the pass eats the gain: 1.41 -> 1.15 on short reach, nothing elsewhere), and
  mirrored near_sorted data, whose ceiling is 1.2x and which has no directional gate to ask.
- **Frames and calls.** `mirror_gate` and the stream probe are `noinline`; the dispatcher's frame went 36,656 -> 35,856 bytes
  (the stream probe's array left it). `make callbench` against the engine before this round: n <= 5 reads 0.97-1.05 with the
  two orientations disagreeing (1.26 against 0.88 on sorted pairs), which the bench's own rule calls noise; nothing was added
  to a small sort's path.

### Measured and NOT built
- **Writhe and persistence (avenue 5): the classifier works, 16 of 16, rule fixed in advance** (`./strandprobe 0 8388608 writhe`).
  From 2,048 strided points: w = 1 - 2 x (share of adjacent pairs that descend), p = share of consecutive pair directions that
  agree. Noise reads p = 0.33 (uniform 0.328, heavy tail 0.336); every stranded shape reads 0.95-1.00, in either direction or
  both. So p separates what the inversion rate cannot: **a triangle wave, a sloped sine and every-other-block-twisted all read
  w = 0.00-0.05, exactly like noise, and p = 0.97-0.98.** Not wired into the dispatcher: nothing built this round needed it
  (the mirror gates ask the routes' own probes), and it cannot tell mirrored blocks from a mirrored ribbon (both w = -0.97,
  p = 0.97, ~64 turns). It is the gate avenue 1 (ribbons) would need.
- **Strays inside strands (avenue 2): the ceiling clears the stop rule, barely, and the build is large**
  (`./strandprobe 30 8388608 strays`). Sorted blocks with late records against a sort TOLD which positions are strays:

| shape, 1 thread | 0% (the told sort's own overhead) | 0.1% | 1% | 5% |
|---|---|---|---|---|
| block_perm | 0.61 | 1.68 | **2.22** | 1.97 |
| stairs x20 | 0.65 | 1.56 | **2.04** | 1.89 |

  Predicted 3-4x: LOW by half, because the told sort pays two serial passes of its own (its null row is 0.61). A route that
  lifts strays inside the detector's scan and merges them back in place of the copy-back pays neither, so the real ceiling is
  nearer 3x. At 14 threads the told sort is no ceiling at all (0.19-0.31: its two passes are serial). Stop rule (2x at 1%): met.
  Not built: a strand with holes breaks the knot plan's binary searches and every placement offset. It is the next build.

### Found on the way
- **Every multi-copy bench had stopped linking.** The Makefile's `RENAME` list did not carry `logos_twist_last` (added last
  round) nor `logos_mirror_count`, so `callbench`, `ledgerbench`, `pfbench`, `tailbench` and the rest died at the link with a
  duplicate symbol. Last round's "all flag variants build" compiled the ENGINE with every flag and never re-linked the benches.
  Fixed; three of them rebuilt as a check. **A new public symbol goes into `RENAME` in the same edit.**
- A `-DLOGOS_TRACE` build broke when the stream probe moved into its own function (a trace line named its locals). Caught
  because the trace bench ran an old binary and showed no `mirror:` line. The flag check now builds five more pairs.

### Verified
- `make test` **19,866 checks**, 0 failures (916 new: 556 for the whole array's bit, 268 of them naming what must have happened, and 362 scalar-scan rows that must place the same twisted strands as the fast scan); all 60 compile-time switch variants build, including `-DLOGOS_NO_TAILROUTE`, which the first cut of the suffix gate broke and the flag check caught; `make sanitize` clean but for the four known NaN-to-index casts in the leaves (open direction 0h (e)); `make tsan` 0 data races; `make faultprobe` over 31 cases, now with two twisted and three MIRRORED shapes (buffers released, one reversal, the dispatcher entered again): **10,082 allocation and 276 thread-creation failures, 0 bad**; `make callbench` against the engine before the round: nothing at n <= 5.

## (unreleased) — the Moebius twist: a run that lies descending is consumed as it lies, never reversed (`make twistbench`); and a profile reading of where the nanoseconds per element go today (`make profile-full`)

### Where it came from
- A sibling line of this engine links natural runs into a ring and calls it a Moebius ring: descending runs are "negative-orientation
  strands" and the seam "flips logical orientation". But there the twist is a label. When run ranges overlap it NORMALISES (reverses
  every descending run in place) and then merges, and this engine did the same one step earlier: `reloc_detect_worker_body` swapped a
  descending run end for end the moment it had found it. A strip is a Moebius strip only if you can walk across the twist without
  cutting and re-gluing, and reversing a run in memory is exactly the cut and the glue. The question: what is the twist if it is
  real, and is it worth anything?

### What it is (`rdesc_t.rev`, `RUN_TWIST`; off with `LOGOS_NO_TWIST=1` or `-DLOGOS_NO_TWIST`)
- **One orientation bit per run, and nothing is reversed.** In a flat `[start, end, ...]` run list the bit rides in the top bit of
  the END index; `runs_plan` turns it into `rdesc_t.rev`, with `first`/`last` the run's smallest and largest value whichever way it
  lies. Every consumer reads a twisted strand from its far end:
  - a lone piece, a piece staged for a crossing of three or more, and relocation's phase 1 are placed by a BACKWARD copy
    (`copy_rev`, `strand_copy`). Clipped to a placement tile, the logical range `[o, o + cnt)` of a twisted run lives at
    `start + len - o - cnt`;
  - the knot plan works in LOGICAL coordinates (`KNOT_AT`, `KNOT_SRC`): cursors count consumed elements from 0, `knot_cut` gallops
    and bisects through an accessor that knows which way the strand runs, and a twisted strand's next piece lies BEFORE the last one
    in memory, so the extend rule moves `src` down instead of growing `len` up;
  - a two-strand crossing is merged with the twisted cursor walking down (`knot_merge2x`). An up-run beside a down-run is a
    bitonic pair, and this is that pair's two-ended merge: the reversal that a bitonic merge is usually preceded by never happens.
- **The detector no longer writes to the array.** A declined probe leaves the input exactly as it was. And a descending run that
  straddles a detection chunk's boundary is now ONE strand: each chunk used to reverse its own part, which left two ascending runs
  with the HIGHER one first, and those cannot be stitched. The stitch is orientation-aware (same orientation only, the descending
  gap-split rule for twisted pairs).
- Not converted, on purpose: the leaf's sequential detector (`detect_runs_limited`, which feeds the galloping run-merge and the
  leaf's own J5) still reverses in place, so lists from it never carry the bit. The int32 path has no relocation at all.
- **A witness, because a harness section must be able to say which path ran:** `logos_twist_last(0 | 1)` counts the twisted
  strands placed by the knot route or by plain relocation since `logos_twist_last(-1)` cleared it (`-2`: is the twist in this
  build). Two plain statics written inside `relocate_try_f64` only; the dispatcher's frame is unchanged (36,656 bytes before and
  after, `-fstack-usage`), so nothing was added to a small sort's path.

### Ceiling first (`./twistbench 40 8388608 ceiling`)
Each shape Z has sorted blocks lying descending; its twin Z' is the same array with them already ascending. After the detector's
reversals Z IS Z', so t(Z) / t(Z') prices exactly the reversal passes: the most an orientation-carrying engine could save.

| shape, 8.4M, 40 pairs | 1 thread | 14 threads |
|---|---|---|
| blocks, none twisted (the null) | 1.014 | 1.017 |
| 64 disjoint blocks, every one twisted | **1.174** | **1.118** |
| every other one | 1.070 | 1.019 |
| 1024 blocks of 8k, all twisted | **1.164** | 1.051 |
| staircase, 20% crossing, all twisted | **1.115** | 1.075 |
| staircase, every other | 1.067 | 1.029 |

- **Prediction, graded.** One thread: predicted 1.03-1.08, measured 1.07-1.17. WRONG, and low: I priced the run routes at 2-9 ns
  per element and they cost 2.3, so a swap pass is a sixth of the whole sort. 14 threads: predicted 1.05-1.15, measured 1.05-1.12
  on the all-twisted rows. Held. Stop rule (1.06 on two shapes at one thread count) met by five rows: build it.

### The A/B (`./twistbench 120 8388608 ab`: `LOGOS_NO_TWIST=1` against the twist, ONE binary, one engine copy)
No code-layout term to cancel: both sides are the same object code, switched by the environment.

| shape, 8.4M, 120 pairs | 1 thread | 14 threads | share of the ceiling, 1 thread |
|---|---|---|---|
| blocks, none twisted (null) | 0.998 | 0.994 | |
| every block twisted | **1.149** | **1.114** | 0.86 |
| every other one | **1.068** | 1.015 | 0.97 |
| blocks of 8k, all twisted | **1.144** | **1.094** | 0.88 |
| staircase x20, all twisted | **1.108** | **1.093** | 0.94 |
| staircase x20, every other | **1.056** | 1.044 | 0.84 |
| staircase x80, all twisted | 1.014 | 1.093 | not measured |
| staircase x80, every other | 1.020 | 1.045 | not measured |
| staircase, none twisted (GUARD) | 0.984 | 0.987 | |
| uniform random (GUARD) | 1.026 | 1.006 | |

- **Prediction, graded.** One thread: every block 1.10-1.16 (1.149, held), blocks of 8k 1.09-1.15 (1.144, held), staircase all
  1.06-1.10 (1.108, at the edge), every-other rows 1.03-1.06 (1.068 and 1.056: one just over). 14 threads: every block 1.05-1.11
  (1.114), staircase all 1.03-1.07 (1.093, LOW), blocks of 8k 1.02-1.05 (1.094, LOW), every-other inside the floor (1.015, 1.044:
  held). Guards 0.98-1.02: 0.984-1.026. The 14-thread cells have p10-p90 of 0.81-1.33 on this box tonight, so their second
  decimal is not information; the three guards say the floor of a median of 120 pairs is about 2.5%.
- Two earlier runs of the same A/B with the first merge (40 and 80 pairs) read the block rows the same way: 1.193 / 1.159 and
  1.141 / 1.127 at one thread, 1.095 / 1.066 and 1.092 / 1.085 at 14.
- **Ship rule, written before the run** (no guard below 0.98, three twisted rows at one thread over 1.04): met. It ships.
- **Against the engine before the twist, two sources in one binary, both copy orientations, sqrt(A/B)** (the nine guard shapes,
  8M, none of them twisted): uniform 1.006 / 1.026 (14 threads / 1), block_perm 1.022 / 0.988, stairs_x20 0.992 / 0.988,
  near_sorted 1.004 / 1.008, bimodal 1.023 / 0.978 at 30 pairs. Four cells strayed at 30 pairs, in both directions (heavy_tail
  1 thread 1.106, few_distinct 14 threads 0.934, quantized 1 thread 0.931, dominant_90 14 threads 0.962), and came back toward
  1 at 80 pairs: heavy_tail 1.015 / 1.064, dominant_90 1.012 / 0.989, few_distinct 0.979 / 0.987, quantized 0.970 / 0.976.
  Nothing the twist added is on those shapes' paths (its first instruction sits behind relocation's sample gate, which random
  data does not pass) and the dispatcher's frame is unchanged, so what is left of quantized's 2-3% is layout or noise. Recheck
  it the next time the engine is touched.
- **Where it does nothing:** with four elements in five inside crossings (x80) the one-thread rows tie (1.01-1.02). There the merge
  is the sort, and a merge that reads one strand backwards saves a swap pass worth about what the backward cursor costs. No loss.

### One wrong turn inside it: the first two-ended merge (`./twistbench 120 8388608 merge`, `LOGOS_TWIST_GENERIC=1`)
- The first `knot_merge2x` was ONE loop with a signed stride per strand: four cursor updates per element where the untwisted
  merge has two. The first A/B showed it: the block rows recovered their ceiling and the staircases did not (every other block
  twisted: **1.000 and 1.002** at one thread in two runs, against a ceiling of 1.067).
- Now one loop per orientation pair (`knot_merge2_du`, `_ud`, `_dd`, generated by one macro), the far-end address folded into the
  base. The strided loop is kept behind `LOGOS_TWIST_GENERIC=1` because it is what the new ones were measured against. Strided /
  specialised, one thread: x20 all twisted **1.054**, x20 every other **1.062**, x80 all **1.111**, x80 every other **1.099**
  (a 40-pair run before it: 1.024, 1.060, 1.112, 1.143); block rows 0.99-1.03; 14 threads 0.98-1.08, not resolved. Prediction
  (1.03-1.07 on x20, 1.08-1.20 on x80, nothing on blocks or at 14): held everywhere.

### Verified
- `twist_checks` in the harness, **921 checks, 284 of them naming the path that must have run**: thirteen shapes (blocks and
  staircases all and every-other twisted, 7 blocks so that every chunk boundary cuts a descending run, the same with crossings,
  three live strands, one run up and one down over the same range, equal keys, signed zeros, infinities at a twisted strand's
  ends, a NaN, runs of 64) x five sizes x 1, 4, 8 threads x four configurations (shipped, gate lifted, `LOGOS_NO_TWIST=1`,
  `LOGOS_NO_KNOT=1`, the only way plain relocation's twisted copy is reached). Both merge forms pass it; so does a build with the
  twist compiled out, where the section asserts instead that NOTHING is placed twisted.
- **Mutation-tested:** the untwisted offset in the knot worker's tile-clipped copy fails 25 rows, the same in relocation's
  phase 1 fails 3. Both only show at 4 and 8 threads, where a tile cuts a piece.
- `make test` **18,950 checks**, 0 failures; all 54 compile-time flag variants build, the twist's own with no warnings; `make sanitize` clean but for the four known NaN-to-index casts in the flash and coarse leaves (open direction 0h (e)); `make tsan` 0 data races; `make faultprobe` on the 26 cases as before 8,136 allocation and 234 thread-creation failures, 0 bad, and on two new twisted cases (`block_twist`, `stairs_twist`) 526 and 24, 0 bad: a failed `knot_worker` leaves the caller to fill the orphaned tile by backward copies. With `LOGOS_NO_KNOT=1` at 2,000,000 elements the same for `reloc_worker`: 186 and 12, 0 bad.

### Found on the way, not fixed
- **A chunk boundary can leave a run too short for the gap test, and plain relocation then declines disjoint blocks.** At
  1,000,003 elements and 8 threads, with the knot route off, a detection chunk began 7 elements before a block's end. A body
  shorter than `RELOC_MIN_BODY` (16) has no gap test, so those 7 fused with the next block into one run whose interval covered
  other blocks (`merged = 62511` of 93,763 seen) and the gate said stop. Older than the twist (the run boundaries are the same
  either way) and masked by the knot route, which prices what crosses instead of what chains. Found because the harness row
  NAMED relocation and got nobody. Seen again in the fault probe: at 2,100,000 elements and 4 threads a chunk starts 8 elements
  before a block's end and plain relocation declines `block_twist`; at 2,000,000, where chunk and block boundaries coincide,
  it takes it. Whether a given array fuses is a coin toss per boundary (is the next block higher or lower).
- The twist stops at the top-level detector. The leaf's `detect_runs_limited` still reverses, so a sawtooth that goes to the
  galloping run-merge pays the swap pass as before. Its ceiling is the same measurement on a different shape; not measured.

### Probed afterwards: where else could the twist go? (`make mirrorprobe`, nothing built)
Asked: is there another point in the code where an orientation bit pays, given what this round found? The round's constraints:
the bit pays where the consumer copies anyway, in proportion to how CHEAP the route already is, and only if it is decided for
free. Two candidate sites, both measured by ceiling before anything was written.

- **(L) The leaf's detector (`detect_runs_limited`): NO.** Descending teeth over one value range against the same teeth
  ascending: 6 teeth 1.05 at one thread and 1.00 at 14, 6 teeth in 3,000 elements 1.04, 64 teeth 1.02-1.04. Below the stop
  rule (1.25), as predicted. At one thread the knot route takes few-run data first and already carries the bit; what is left
  reaches a 16 ns/element leaf, where a swap pass is a thirtieth of the sort.
- **(G) ONE bit for the whole array: YES.** Every cheap route reads order in one direction. Hand it its own input mirrored (a
  newest-first event log, a feed sorted descending by time with late arrivals) and no gate fires, because the prescan's
  `descending` test is exact: one late record and the array pays for a full sort. The shared sample already holds the bit
  (`inv_all` reads 0.87-1.00 on these mirrors, 0.50 on noise). Each shape against its own mirror, 8.4M, two runs (40 and 60
  pairs); `ceiling` = t(mirror) / t(shape), `simple` = t(mirror) / t(one reversal pass, then sort), the pass timed:

| shape | 1 thread ceiling | 1 thread simple | 14 threads ceiling | 14 threads simple |
|---|---|---|---|---|
| short reach, noise of +-60 | **2.02-2.11** | **1.92-1.99** | **1.41-1.44** | 1.11-1.19 |
| 95% sorted prefix + random tail | **2.71-2.74** | **2.43-2.47** | 0.56 | 0.49-0.52 |
| 4 interleaved ascending streams | **3.10-3.30** | **2.92-3.14** | 1.10-1.12 | 0.96 |
| near_sorted, 1% swaps | 1.22-1.23 | 1.20 | 1.17-1.22 | 1.00 |
| uniform (control) | 0.99-1.02 | 0.95-0.99 | 1.00-1.01 | 0.90 |
| block_perm, stairs (the twist has them) | 0.98-1.01 | 0.86-0.90 | 0.97-1.02 | 0.77-0.80 |

- **What the table says about a design.** (1) A false positive costs 5-10% (the uniform row's `simple`), so the gate cannot be
  `inv_all` alone: mirrored blocks read 0.90 and the twist already sorts them at a ceiling of 1.00. The reversal has to sit
  BEHIND relocation and fire only when a directional gate (the reach ladder, the prefix scan, the stream probe) says yes ON THE
  MIRRORED SAMPLE, which is 2,048 points and costs nothing. (2) At one thread the plain version (reverse once, re-enter) keeps
  90-95% of the ceiling. (3) At 14 threads only short reach clears the bar, and the pass eats most of it (1.41 -> 1.15), so
  there the pass would have to be FOLDED into work already owed: block k sorted out of place into block slot B-1-k, which
  costs the short-reach route its in-place property (0.1 bytes per element today). Unmeasured; do the one-thread form first.
- **Predictions, graded.** Right: short reach, streams, uniform, the block controls, all of (L). Low: prefix_95 (3-4x predicted).
  WRONG: near_sorted (3-4.5x predicted, 1.2 measured: with 1% of elements swapped across the whole array the shape is not
  cheap at one thread in either direction), and prefix_95 at 14 threads, wrong in DIRECTION.
- **A lead that is not about orientation.** At 14 threads prefix_95 sorts 1.8x SLOWER than its own mirror. The obvious suspect
  is cleared: the parallel tail route still beats the partition there (`./mirrorprobe 60 8388608 tail`: 1.075 at 14 threads,
  3.95 at one). So a sorted run with a random SUFFIX costs the parallel engine 1.8x what a descending run with a random PREFIX
  costs it. Not traced. One guess to test first, and it is only a guess: the mirror's buckets are handed to the leaf as
  descending runs, which its prescan reverses for almost nothing.
- **A confirmation worth having:** the block rows' ceiling of 0.98-1.02 is the twist seen from outside. Before it, a mirrored
  block shape paid a swap pass (1.07-1.17); now an array and its mirror image cost the same.

### Where the nanoseconds per element go today (`make profile-full`, 8M, loaded box: memcpy 3.77 ms, std::sort 1M 80.2 ms)
Asked: what takes the longest, and what is left to trim? Rows are minutes apart, so only comparisons INSIDE a row mean anything.

| 1 thread, ns per element | logos | x86-simd-sort (AVX2) | what logos ran | logos behind by |
|---|---|---|---|---|
| uniform | 16.1 | 11.8 | partition + AVX2 quicksort leaf | 1.36x |
| heavy_tail | 15.2 | 12.0 | the same | 1.27x |
| bimodal | 49.5 | 22.1 | the same leaf, 3x uniform's cost | 2.24x |
| int_wide | 35.1 | 18.5 | int32 radix | 1.9x |
| mod_card_30k | 21.0 | 6.6 | quantised radix (the lattice route declines at one thread) | 3.2x |
| quantized | 10.3 | 10.2 | lattice | level |
| few_distinct | 2.1 | 1.5 | counting | 1.4x |
| near_sorted | 3.6 | 12.8 | near-sorted route | ahead 3.6x |
| block shapes | 2.3 | 12 | relocation / knot | ahead 5x |

| 14 threads, ns per element | logos | x86-simd-sort | IPS4o | where logos's wall time goes |
|---|---|---|---|---|
| uniform | 2.0 | 3.0 | 4.7 | leaf 72%, scatter 24% |
| heavy_tail | 4.5 | 11.9 | | leaf 68%, scatter 27% |
| bimodal | 4.2 | 7.2 | | leaf and scatter, both 2.1x uniform's |
| int_wide | 4.3 | 8.3 | | int32 radix leaf 2.9 against flash's 1.4 |
| few_distinct | 1.1 | 1.4 | 0.9 | threads working 39-57% of the time |
| quantized | 2.0 | 6.9 | | the same |
| mod_card_30k | 1.3 | 2.4 | | the same |
| block shapes | 1.1-1.2 | 2.9 | | the same |

- **The leaf is where the time is, at both thread counts**: all of a one-thread sort, 65-72% of a 14-thread one, with the scatter
  at 24-30%. Thread-time on the partition route is 81-85% working, 13-15% idle, 1-5% at barriers: there is no scheduling slack
  left to collect. On the counting, lattice and relocation routes threads WORK only 39-57% of the time: serial phases and spawns
  (open directions 0a and 0h (a)).
- **Ranked by what the rows say, each a prediction to be graded, none built:**
  1. `mod_card_30k` at one thread, 3.2x behind: the lattice route declines at one thread and a multi-pass float radix takes
     30,000 distinct values. A one-thread counting route there predicts 21 -> 8-10 ns per element.
  2. `bimodal` at one thread costs 3x what uniform costs IN THE SAME LEAF and nobody knows why. It wants a component probe
     (the way the AVX2 leaf's first cut was debugged), not a guess.
  3. `int_wide`, 1.9x behind at one thread and the slowest leaf at 14: an int32 radix at 35 ns where an AVX2 quicksort is about
     17. `-DLOGOS_AVX2_INT32` exists and is opt-in; read `make simdbench`'s verdict on it before anything else.
  4. The `AVX2_TAKE` gate: a sloped wave with noise reads "nearly sorted" to 40 strided pairs and is denied the AVX2 leaf; the
     dense-disorder probe from the short-reach round is the statistic that would overrule it. Predicted 1.45x on that shape,
     neutral on genuinely near-sorted data (open direction 0i (f)).
  5. 14 threads: every non-uniform shape READS 2.1-2.2x uniform in both the leaf and the scatter. That is a comparison across
     rows, which this report cannot certify (the ledger has a row about exactly that mistake), so it is a question and not a
     finding: pair heavy_tail against uniform the way `sineprobe` pairs its shapes. If it holds, the ledger's verdict on
     equi-depth leaf classes ("heavy_tail's leaf only 7% above uniform, ceiling 3%") predates the key grid and the
     histogram-free scatter and wants re-measuring; the ceiling would be about 1.6x.
  6. The structured routes' idle threads: a persistent worker pool behind `par_run` / `par_spawn`, and the serial slot merge.
  7. The small end: the thin front (open direction 0g).
- **A profiler defect, found reading the table, not fixed:** the near_sorted 14-thread cell records `thr=1 n=159701`. A NESTED
  sort (the near-sorted route's stray sort) overwrites the top-level profile record. Until it is fixed that cell's phase split is
  the stray sort's, not the route's.

## (unreleased) — the scale report zooms and pans; and two defects in the chart library's log axes that only a driven browser could show (`make scale-full`, `tools/browser_check.py`, `tools/check_report.js`)

### Which report, and how to make it again
- `make parbench-full && ./parbench` is the fair fight at 8M: two tables on the terminal, no file. The HTML with the same opponents
  beside every row is `make scale-full` (about three minutes, writes `scale-report.html` and copies it to `reports/`). To put
  measurements that already exist through a changed template, measuring nothing: `LOGOS_SCALE_REUSE=<report.html> ./scalebench`.
  WSL has no node and no browser, so the two checkers run from Windows on the written file (`node tools/check_report.js`,
  `python tools/browser_check.py`).

### What the charts do now (the template's `IX` block in `scalebench.cpp`)
- **Zoom**: Ctrl + wheel or a pinch, around the pointer; a sideways drag for a range of n, a vertical one for a range of values, a box
  for both (uPlot draws the selection, the report decides what it means); over the value axis the wheel zooms that axis alone; a minus
  and a plus button per panel. **A plain wheel scrolls the page**, because the page is 36 charts: it zooms only a chart that has been clicked,
  until the pointer leaves, and even then a wheel the chart has no use for (zooming out of the home view) goes back to the page.
- **Pan**: Shift + wheel, a sideways scroll, Shift + drag, the middle button, a "drag pans" switch; one finger sideways on a touch
  screen once zoomed. **Keys**: Tab to a chart, arrows, + and -, 0, Esc. **Back**: double-click, 0, the panel's reset, "reset all".
- **The two charts of a panel share their n window**, and through uPlot's cursor sync their pointer, their hidden series and the
  selection as it is dragged. The value axis refits to the window, reading the neighbours just outside it too, so a window between
  two sizes still shows its segments; and the two run-off-the-top rules are now one function applied to whatever window is in view,
  so a zoom into the small end is treated like the "n <= 10" preset.
- **A box by the pointer** lists every sort at that n, fastest first, with its time relative to logos. "expand" gives a panel the
  full width and more height. The control bar stays in view and carries a jump to each shape. Every limit is in one place
  (`setView`): never outside the measured sizes, never narrower than the smallest gap between two of them, a zoom that has reached
  that limit stays where it is rather than creeping toward the pointer.

### Found by driving it, under 216 charts that passed the stub
- **uPlot passes EVERY n window through the x scale's range function, explicit ones included, and its default for a log scale rounds
  the far end up to the next decade.** A zoom to 100 .. 25,118 came back as 100 .. 100,000, and the "n <= 10" view, which is one
  decade, could not be zoomed at all. The stub had taken an explicit range at its word. The x scale now has a range function that
  returns what it is given.
- **uPlot's log ticks loop forever on a panned window.** They walk up from the scale's lower end in steps of that end's decade,
  rounding each to the step's decimals. Below 1 with a mantissa of 9.5 or more (9.6e-6 + 1e-6 rounds to 1.1e-5) the next step is
  1.1e-5, which is not one of its round increments, the rounding falls back to whole numbers, every later tick is 0, and the tab
  dies of `Invalid array length`. uPlot's own zoom never meets this BECAUSE of the rounding above. A log axis here brings its own
  ticks (1..9 of every decade, round linear steps once less than about a decade is in view) and its own label thinning.
- **The stub reproduces both now**: it rounds a log x window the way uPlot does unless the scale brings a range function, it refuses
  a log axis without its own `splits()`, and it feeds that function the windows that killed the tab. It fails on both earlier
  cuts of the report. It also keeps real scale state, asserts on every range it is handed (finite, not empty, positive on a log
  axis, inside the measured sizes, not below zero on a linear value axis) and drives every chart through the layer's own entry
  points and listeners in all six views.
- **`tools/browser_check.py` drives the first panel in real Chrome, in all six modes**: a plain wheel (must be left to the page),
  Ctrl + wheel, Shift + wheel, a sideways scroll, a wheel over the value axis, the keys, the wheel on the chart that has the focus,
  the panel's buttons and "expand", a drag selection, a double-click, both ways of panning, the box by the pointer, a series hidden
  while zoomed, a zoom pushed until it stops, a pinch and a one-finger drag. Three things a driver of uPlot has to know are in its
  header (scales are read back only after a microtask; a mousemove with no `movementX` is IGNORED during a drag; `mouseenter` does
  not bubble). Mutation check: a wheel handler that swallows the page's scroll and a twin that does not follow are both caught;
  removing the `syncRect` call after "expand" is NOT, because uPlot re-reads its plot rectangle on every `mouseenter` and scroll,
  which makes that call belt and braces rather than a fix.

## (unreleased) — disorder of short reach: index blocks sorted where they lie; and what the hunt for it found in the near-sorted gates and in the general leaf (`make reachhunt`, `make reachbench`, `make nsgatebench`); and what ONE CALL costs at the small end (`make callbench`)

### Where it came from
- A question, not a bug report: the knot route worked, so what other views of an array work the same way? Six candidates were
  written down with predictions (`experiments/SHORT_REACH.md`): cuts, short-range braids, links, the transposed diagram, the sorted
  description, guessed permutations. The first step was a ceiling bench, not a route, with a stop rule written in advance: no
  parallel route unless the ceiling beats the 14-thread time on both bounded-displacement shapes of the zoo.
- One candidate is built and ships (below). Two were measured and are not built. Three are unmeasured. The hunt also found defects
  in code that was already shipping: two in the near-sorted route's gates, a third that the repaired gates exposed (one gate for
  every size), one in the general leaf's nearly-sorted probe (and in the JavaScript engine, whose small case had no budget at
  all), and a harness row that passed only because of the broken gate. All are repaired here, each with its own A/B. Three
  weaknesses are recorded as open.

### Ceilings first (`make reachhunt`)
- `reachhunt.cpp` includes the zoo of twenty unchanged (the quadtree classifier's held-out test depends on it) and adds two shapes
  (coarse_key_groups, regime_switch) and informed sorts that are TOLD the structure. Paired rows, n = 2^23, every output checked
  bit-exact against `std::sort`, ratio = t(logos) / t(method):

| shape | method | 1 thread | 14 threads |
|---|---|---|---|
| event_ts_latency (displacement <= 4096) | index blocks sorted in place + seam repair | 1.28-1.31 | 1.18-1.22 |
| window_shuffle (2048-element windows) | the same | 1.34-1.37 | 1.22-1.24 |
| window_shuffle | cuts, told where they are | 1.64 | 0.30 |
| window_shuffle | cuts, found blind | 1.11 | 0.24 |
| coarse_key_groups (groups of 2k-200k, random inside) | cuts, told | 1.43 | 1.11 |
| regime_switch (3 value bands, reach 512 inside a dwell) | chunk relocation, told the segments, then blocks | 2.0-2.1 | 1.3-1.5 |
| latency + 0.01% far strays | blocks + seam repair, which must decline | 0.60 | 0.41 |
| uniform (guard) | blind cut detection that finds nothing | 0.98 | 0.89 |

- Seven predictions, two right. Cuts told at 14 threads were predicted 1.3-1.8x and measured 0.30x: ranges of 2048 elements fall
  under the leaf's flash threshold and a parallel sort sends them to a scalar quicksort. Blind cut detection was predicted at
  8-12 ms and costs 35: a suffix minimum and a running maximum are dependency chains. Chunk relocation was predicted 1.2-1.5x and
  measured 2.0x for a reason that is not its merit: the engine runs three-band data at twice its uniform time. **A ceiling
  measures the baseline's weakness too.**
- Cuts are a special case of blocks (a block boundary that happens to be a cut has an empty seam), and finding them costs a pass
  that blocks do not need: not built. Chunk relocation needs a segment detector the ceiling did not price: not built.

### The short-reach route (`reach_try_f64`, `make reachbench`; off with `LOGOS_NO_REACH=1` or `-DLOGOS_NO_REACH`)
- **What it does.** Index blocks of 32768 (more when the reach needs it, 131072 at most) are sorted in place, in parallel. Then
  only the seams: for sorted blocks L and R, the elements of L above R[0] and the elements of R below the last of L are merged in
  place; what crosses a seam rightward is matched by what crosses leftward, so nothing else moves. No n-sized buffer is asked
  for: `make memprobe` weighs **0.10 bytes per element at one thread and 1.33 at fourteen** (10M), against 15.5 and 23.2.
- **Why the seams cannot fail.** With m(k) the median of sorted block k, an element of block k below m(k-1) or above m(k+1) is a
  STRAY and is set aside first. What is left lies in [m(k-1), m(k+1)], so the seam on the left takes elements below m(k), the
  seam on the right takes elements above it, and m(k) is in neither: every block keeps an untouched middle, which is the
  condition under which the seams are independent. The ceiling bench could only check that condition and decline.
- **Strays decide the cost, and a sample cannot see them.** A few are sorted apart and put back by ONE in-place shift pass:
  between two strays every element moves by the same amount, so the pass is a list of `memmove`s, left-movers first from the
  left, right-movers second from the right, and one stray costs one `memmove` of the span it travels. The first cut merged them
  through a second buffer: +36 ms at one thread and +12 ms at 14 for a single stray. A block that holds a stray is split three
  ways before it is sorted, because one zero in a block of timestamps stretches the flash sort's map until every real element
  lands in one class (15.7 ms of block sorts at 14 threads against 9.2).
- **How many strays are worth it depends on the thread count.** The shift is serial, ~0.7 ns per element at most; the route saves
  ~7 ns per element at one thread and ~0.4 at fourteen. Up to four threads strays are simply put back (n/32 at most). Wider sorts
  hold eight probe blocks against their fences first (the estimated medians of their neighbours) and decline when two break one:
  0.05-0.1 ms, nothing moved. A rule that stopped the block phase when strays kept turning up was measured DOMINATED and removed
  (strays in 28% of the blocks at 14 threads: 1.24x against the partition when put back, 0.78x when the phase stopped itself).
- **The gate reads the sample and four windows, never the array.** Sample indices i and i+L sit |frac(L phi)| n positions apart; for
  the Fibonacci lags 144, 233, 377, 610, 987, 1597 that is 0.31%, 0.19%, 0.12%, 0.073%, 0.045% and 0.028% of n, a ladder of
  distances. The reach estimate is the smallest rung from which no distance shows more than 2% inverted pairs. Random data fails
  the first rung, which the shared sample has already read, so it pays nothing. Four 4096-element windows must then show dense
  adjacent descents; sparse disorder on sorted data is left to the near-sorted route.
- **The block sort was the surprise.** The first cut used the general leaf: 1.15-1.53x at one thread. Then, each by paired A/B
  inside the route: going straight to a kernel is 1.10-1.13x at one thread and **1.97x at 14 threads on latency < 512** (the
  general leaf's 40-pair probe calls such a block nearly sorted and wastes an insertion attempt); a **flash sort beats the AVX2
  quicksort at one thread by 1.26-1.81x** on these blocks, because classification is nearly the identity on locally ordered data;
  the first cut's "small span means integers" test sent every series with unit spacing to the general leaf (64-element windows:
  293 ms at one thread, 76 through the kernel); and disorder that reaches two or three places is an insertion sort's (noise of
  +-2 at one thread: 0.81 with flash alone, 1.10 with the insertion kernel, told apart by pairs four apart). The route's flash
  counts its classes before it scatters and hands a block with a crowded class to the general leaf: a block that straddles a
  quiet hour holds two clusters, which the leaf's quantile-spread check would pass. Block size: 16384 neutral, 65536 0.85-1.00.

Route off / route on in one binary (`LOGOS_NO_REACH`). 8.4M with 30 pairs on a day whose null test read 6.0% at 8M; 30M with 12
pairs; 1M with 120 pairs:

| shape | 8.4M, 1 thread | 8.4M, 14 threads | 30M: 1 / 14 | 1M: 1 / 14 | 8.4M at 4 / 7 threads |
|---|---|---|---|---|---|
| latency_4096 (event log, latency < 4096 ms) | **1.91** | **1.31** | 2.53 / 1.51 | 1.00 / 1.00 (*) | 1.61 / 1.63 |
| window_2048 (shuffled inside 2048-element windows) | **2.22** | **1.38** | 2.72 / 1.79 | 1.94 / 1.38 | 1.79 / 1.82 |
| jitter_1000 (index + noise of +-1000) | **2.05** | **1.33** | 2.13 / 1.52 | 1.81 / 1.25 | 1.50 / 1.43 |
| latency_512 | **2.38** | **1.49** | 2.86 / 1.70 | 2.21 / 1.38 | 1.82 / 1.68 |
| window_64 (`TRUTH_TABLE` J6's shape) | 2.46 | 1.48 | 2.86 / 1.83 | 2.45 / 1.34 | |
| jitter_2, jitter_8, jitter_30 | 1.10, 2.54, 2.17 | 1.80, 2.35, 1.31 | 1.09, 2.56 / 1.67, 1.55 | 1.12, 2.60 / 1.44, 2.08 | |
| ticks_dup (jittered one-second ticks: heavy duplicates) | 2.80 | 1.02 (a counting route wins the dispatch) | | | |
| one_stray (latency_4096 + one far element) | 1.81 | 1.14 | 2.44 / 1.31 | | 1.49 / 1.39 |
| strays in 0.001%, 0.01%, 0.1%, 1% of elements | 1.77, 1.57, 1.49, 1.23 | 1.26, 1.00, 1.01, 1.04 | 2.21 / 1.14 (0.01%) | | 1.28 / 1.01 (0.01%) |
| 0.05% zeros; 8, 84, 840 zeros; 84 late events | 1.58; 2.28, 2.35, 1.54; 1.77 | 0.99; 0.96, 0.97, 0.99; 1.10 | 1.41 / 1.00 (0.05%) | | 1.15 / 1.01 (0.05%) |
| latency_512, noise of +-100, +-20, +-8, +-2, each + 0.01% strays | 1.84, 1.81, 2.33, 2.32, 1.56 | 0.99-1.02 | | | |
| reach_40000 (needs blocks above the cap: declined from the sample) | 1.01 | 0.96 | | | |
| uniform, near_1pct, coarse_groups, regime_switch, stairs_x20, block_perm64, heavy_tail, few_distinct | 0.99-1.03 | 0.94-1.02 (few_distinct read 1.18 with p10/p90 of 0.68/1.46) | 1.02 / 0.98 (uniform) | 1.00 / 1.01 (uniform) | |

(*) a reach of 4096 is longer than the first rung of a 1M ladder (0.31% of n is 3,100 positions), so the gate declines from the sample.

- **Predictions in `reachbench.c`, graded.** The four target rows were predicted 1.10-1.45 at one thread and 1.15-1.40 at 14 from
  the ceilings: right for the 14-thread rows, **low at one thread everywhere**, because the ceilings ran the general leaf.
  ticks_dup 2-3x at one thread: right; 1.3-1.8x at 14: wrong, a counting route wins the dispatch first. one_stray was predicted
  0.95-1.10 / 0.70-0.90 for the buffered merge and measured 1.81 / 1.14 after the shift pass replaced it. Strays "0.97-1.00
  everywhere": right at 14 threads, wrong at one, where they are now put back. Guards 1.00: right.
- **Guards against the engine before this round, layout cancelled (two copies, both orientations):** 0.98-1.03 on nine shapes at
  8M at both thread counts; 0.98-1.02 on the same nine shapes at 1M (240 pairs).
- **Tests.** A harness section of 595 checks asks for every branch BY NAME through `logos_reach_last()` (sorted; sorted with
  strays shifted in; declined at the gate, by the probe, by the plan): jitter, windows, one stray up, one down, three, strays
  everywhere, strays the probe cannot see, duplicates, ties at the seams, signed zeros, infinities, a NaN, subnormals, quiet
  hours, a plateau, two blocks swapped, sparse disorder. The first version passed with the route never firing at one thread (the
  gate's sample is 384 points there and the ladder needs 2048), and the diagnostic reported the PREVIOUS sort's answer until it
  was reset at the top of the dispatcher rather than at the route (from the route's floor of 2^18 elements up: see the small end,
  below). `make faultprobe`: two new cases, 480 failed allocations and 12
  failed thread creations, none ending badly (20 allocation sites; `reach_worker` dropped from its barrier in every position).
  The whole sweep on the final engine, twice: **8,170 and 8,166 failed allocations (the count moves by a few with thread timing), 234 failed thread creations each, 0 bad** (141 allocation sites, 18
  worker functions; 15 minutes on a quiet box). An earlier sweep, run beside a ThreadSanitizer harness and a JavaScript benchmark,
  took 52 minutes and reported ONE hang (int32, one thread, the fourth allocation): its watchdog is 5 seconds. Alone, that case
  passed 120 times of 120. Run the fault probe with nothing beside it.

### The near-sorted route's two gates (`make nsgatebench`; `LOGOS_NS_GATE_OLD=1` restores both)
- **The shared sample's inversion rate read SEVEN pairs, not ~1900.** Sample indices i and i+144 are 0.31% of n apart, and the
  later index is the EARLIER position (frac(144 phi) = 0.99689). The `p1 <= p0` test meant to drop the few pairs that wrap around
  the array dropped all 1,897 adjacent pairs and kept the 7 wrapped ones, a whole array apart. With 7 pairs it passed gray-code,
  sawtooth, block-permuted, staircase and grouped-run data as nearly sorted (true rates 8-79%) and turned away one genuinely
  near-sorted input in seven. Found because the short-reach gate wanted the same statistic at other lags and the pair counts came
  back 7 and 3. The gate now reads every pair in position order (`inv_all`).
- **The pre-gate had no poison recovery, and the route it guards has.** A displaced-high element followed within two places by a
  larger one becomes the kept maximum and everything after it in the window counts as rejected. On sorted data with random swaps
  the pre-gate declined 12 of 40 seeds at 1% swaps and 38 of 40 at 2% (mean rejected 18% and 48%, where the route rejects 2% and
  4%). The pre-gate now runs the route's own scan on its four windows.
- **What the two cost together:** sorted runs grouped by a coarse key (several sorted shards per day, the days in order) ran 5.8x
  off the ceiling at 14 threads. The 7-pair statistic let the near-sorted route run, it rejected 73% of the array and declined
  after 57 ms. Found only because the ceiling bench's late decline handed the engine an array of sorted blocks.

| shape (8.4M, gates as they were / repaired) | 1 thread | 14 threads |
|---|---|---|
| groups_of_runs (sorted 33k blocks inside coarse-key groups) | 1.00 | **3.82** |
| sorted + 1% random swaps, four seeds (the 7-pair statistic turned two away; the pre-gate turned the other two away) | 0.95-1.01 | **1.36, 1.39, 1.30, 1.48** |
| sorted + 2% swaps, two seeds (newly admitted: true rate ~3.5%) | 1.00-1.01 | **1.37, 1.23** |
| sorted + 0.5%, 3%, 5%, 10% swaps (100 pairs) | 0.99-1.02 | 1.01, 1.01, 0.98, 1.00 |
| block_perm64, stairs_x20, sawtooth, gray_code, sorted_runs_256, uniform, heavy_tail, few_distinct | 0.99-1.00 | 0.97-1.04 (30 pairs); 0.98-1.01 (100 pairs) |

**What the ROUTE is worth, not the gates** (`NSG_ROUTE=1 ./nsgatebench`: the route switched off by `LOGOS_NO_NEARSORTED` against the repaired
gates, 14 threads, 100-300 pairs). The old pre-gate had turned 38 of 40 inputs with 2% swaps away whatever their size, so nobody had
seen this table:

| swaps | 1M | 1.5M | 2M | 3M | 4M | 6M | 8.4M |
|---|---|---|---|---|---|---|---|
| 0.5% | 1.36 | 1.19 | 1.48 | 1.66 | 1.69 | 1.75 | 2.12 |
| 1%, two seeds | 1.21, 1.28 | 1.07, 1.12 | 1.25, 1.25 | 1.42, 1.37 | 1.38, 1.43 | 1.36, 1.73 | 1.38, 1.41 |
| 2%, two seeds | 0.98, 1.04 | **0.84, 0.87** | 0.93, 1.15 | 0.94, 0.94 | 0.99, 0.98 | 1.33, 1.41 | 1.12, 1.13 |
| 3% | 1.00 | **0.75** | **0.80** | 1.00 | 1.01 | 1.02 | 1.09 |

The route has three spawn phases and sorts its strays once, which a few million elements do not pay back unless the strays are few. So
the gate is **2.5% below 6M elements and 5% from there on** (`NS_INV_RATIO_SMALL`, `NS_WIDE_MIN_N`); with the gate lifted, 5% swaps lose
0.71 at 8.4M. With it, gates as they were / repaired at 1.5M, 2M and 4M (300 pairs): 1% swaps 1.00-1.36, 0.5% swaps 0.98-1.61, 2% and 3%
swaps 1.00-1.02, groups_of_runs 2.76-3.81.

- **A harness row was passing because of the broken gate.** The tail route's parallel NaN row demanded that two runs (route on, route
  off) return the same BYTES, on the premise that a declined tail route leaves both on one deterministic path. That held only while
  the 7-pair statistic sent its input (a tenth of it a random tail, which reads 5.1%) to the near-sorted route. Read properly it goes
  to the partition, where the order inside a bucket depends on thread timing, and with a NaN present two runs of the SAME
  configuration differ: 21 of 40 tries. Seen first as one unexplained 'CORRECTNESS FAILURES' under ThreadSanitizer, whose target
  pipes through `tail -3` and cut the FAIL line off; caught by looping the plain harness (first pass). The row now checks what the
  engine promises with a NaN, a return and a permutation of the input's bits; twelve full passes in a row are clean.

### Asked after the fact: were the last patches A/B'd, and does the sine wave actually cost anything?
- **The guard A/B, re-run on the engine as it ships.** Each of the three per-call repairs above was measured where it acts (`make
  callbench`, both copy orientations), but the 8.4M guards had not been re-run since the last of them. Shipped engine against the
  engine from before this round, two copies, both orientations, layout bias cancelled by sqrt(A/B), 30 pairs each: **0.977-1.022
  across nine shapes at 1 and 14 threads**, every row inside the ~4% floor, every output verified. The layout term itself (sqrt(A*B))
  ran 0.98-1.04, which is the usual size. So the round is neutral at 8.4M and 3.4x cheaper on a call with nothing to sort.
- The report work of the entry above needed no A/B: every line of it is inside `scalebench.cpp`'s HTML template literal (lines
  244-727) or its file header comment, and the measurement code is lines 1-243. A presentation change cannot move a timing, and
  saying so is cheaper than proving it with a bench that could not see it anyway.
- **The sine wave with a slope: 1.22-1.31x at ONE thread, and the control is what makes that mean anything** (`make sineprobe`,
  rewritten; `experiments/SHORT_REACH.md` section 6). The first cut of that bench printed one absolute time per shape and invited
  the reader to compare rows of one run, which rule 2 forbids; the claim it was used for ("slower than random data") survived a
  proper paired measurement, but that was luck, not method. Now every shape is sorted adjacently with a uniform reference, the order
  alternated over 40 pairs, with a null (a second uniform instance: 0.99-1.00) and a no-wave control. The wave plus noise of +-50
  elements reads 1.31 and 1.22 in two runs; the SAME noise on a plain slope reads 0.59-0.63, because the short-reach route takes it.
  So the cost belongs to the wave, not to the noise, and it tracks the REACH rather than the swing (reach 104 -> 1.27, 23 -> 1.09,
  6 -> 1.05). At 14 threads it is inside the null's band. Two of five written predictions were wrong: a route's own A/B ratio is not
  the same quantity as shape-against-random, and the structured shapes at 14 threads are faster than random, not level.
- **A third control found the cause, and it is a one-line gate.** The same VALUES with the index order destroyed read **1.01**, level
  with random: so the 27% belongs to the ORDER, and the array is slower than the same numbers shuffled. A `-DLOGOS_PROFILE` build
  says where it goes: the natural order takes the **coarse** leaf, the shuffled multiset and uniform both take the **AVX2 quicksort**
  leaf (126 ms against 87 and 90 in that build). `AVX2_TAKE` requires `!c->nearly`, and `sort_fn`'s 40 strided pairs call the wave
  nearly sorted because its slope makes a pair n/40 apart ascending almost always. That rule is right for data that is nearly sorted
  and wrong here, and **the statistic that would overrule it was added in this very round**: the dense-disorder probe, 64 pairs eight
  apart, which the noisy wave fails at about 27 of 64 against a threshold of 16, and which today gates only the insertion sort.
  Carrying `dense` in the context and letting it overrule `!c->nearly` is predicted at **~1.45x on this shape** (1.27 -> about 0.88,
  faster than random) and neutral on genuinely near-sorted shapes. NOT BUILT: it wants its own paired A/B against the near-sorted
  guards, and the question that produced it was a question, not a request for a change.
- `make sinetrace` is its trace sibling, for which route each shape takes. Routes only: a trace build must never be timed (ledger).

### The general leaf's nearly-sorted probe (`LOGOS_NO_DENSE_PROBE=1` restores the old leaf)
- `sort_fn` calls a bucket nearly sorted when at most 5% of 40 strided pairs are inverted, then tries the momentum insertion
  sort, which gives up after 8n moves. Disorder that is DENSE but shorter than the stride of n/40 passes the probe and wastes
  those 8n moves, in every partition leaf and in the sequential dispatcher. Data the short-reach route declines still meets it.
  Now, when the probe says nearly, 64 strided pairs EIGHT apart decide whether the insertion sort is tried at all: its budget is
  a mean displacement of 8, and an element displaced further is usually inverted with its eighth neighbour.

| shape (8.4M, old leaf / new leaf, the short-reach route on both sides, 30 pairs) | 1 thread | 14 threads |
|---|---|---|
| latency_512 + 0.01% strays (the route declines it at 14 threads: the partition's leaves meet it) | the route takes it | **1.36** |
| noise of +-100 + strays | the route takes it | **1.70** |
| noise of +-20 + strays | the route takes it | **1.87** |
| noise of +-8 and +-2 + strays (inside the insertion sort's budget: it must still run) | | 1.00, 1.01 |
| regime_switch | 1.17 | 1.16 |
| coarse_groups, reach_40000 | 1.10, 1.07 | 0.99, 1.00 |
| the other 24 rows | 0.98-1.01 | 0.96-1.08 (a 6% day); 0.99-1.06 on six guards with 100 pairs |
| the three stray rows at ONE thread, measured before the route took strays there (24 pairs) | 1.32, 1.31, 1.28 | |
| with the route OFF on both sides (four configurations interleaved, minima): 64-element windows | 384 -> 160 ms | 39.7 -> 21.4 ms |
| the same: noise of +-30 | 497 -> 221 ms | 46.5 -> 23.9 ms |

### What a call costs at the small end (`make callbench`, new)
- **Why it was looked at.** Every A/B of this round starts at a million elements, and the ledger already holds a route whose first
  cut made every tiny call 7x dearer (two `getenv`s). So the engine before the round and the engine after it were linked into one
  binary and called on 0 to 50,000 elements, random and already sorted, alternating in 80 blocks per size, with the copies swapped
  in a second binary and the geometric mean taken. That is now `make callbench` (baseline: the committed engine, or `CB_BASE=<source>`).
- **As built, the round cost every small call.** work / before, both orientations combined:

| n | 0 | 1 | 2 | 3 | 5 |
|---|---|---|---|---|---|
| random, the round as built | 1.11 | 1.12 | 1.11 | 1.06 | 1.00 |
| sorted, the round as built | 1.14 | 1.09 | 1.11 | 1.03 | 1.09 |
| random, as repaired (a cool box) | 1.01 | 0.99 | 0.99 | 0.98 | 1.01 |
| sorted, as repaired | 1.00 | 1.00 | 1.00 | 0.98 | 1.01 |
| either shape, with the early return below | **0.29** | **0.29** | | | |

- **Measured and fixed: the dispatcher's frame.** `reach_from_sample` holds two 16 KB arrays and was inlined into `psort_f64`, whose
  frame went from 36.6 to 44.8 KB. With stack-clash protection (this toolchain's default) a prologue touches EVERY page of its frame
  on EVERY call: 8 probes became 10. Alone against the final engine that is 1.09 / 1.09 at n = 0 and 1 on random input and 1.11 /
  1.14 on sorted, all eight orientation readings between 1.07 and 1.16. `noinline` on the helper. `-fstack-usage` prints every frame.
- **Not resolved, removed anyway: thread-local accesses.** `logos_reach_last()`'s state was reset at the top of every sort. Under
  `-fPIC` a thread-local access is a call to `__tls_get_addr`; the dispatcher had none and got two, on the path of every sort of two
  or more elements. Predicted +20-35% on a sorted triple. Measured alone: +5% in one orientation and -14% in the other, which is
  nothing (the call's fast path is about ten instructions, and the public entry already made one). It went because removing it is
  free: the reset now starts at the route's floor of 2^18 elements, **a smaller sort leaves the answer alone**, the header says so,
  and the harness rows under the floor assert exactly that (the answer UNCHANGED; they used to assert NOT_TRIED).
- **A win that is code layout, not shipped.** With the general leaf's new probe loop inline, a sort of 100 random doubles, WHICH
  NEVER RUNS IT, read 5% slower than before the round (1.045, 1.060, 1.053: three rounds, both orientations). With the loop deleted:
  0.98, 0.97. Out of line: 0.88, 0.88, 0.89, and again in a second bench binary (1.00-1.03 inline, 0.88-0.89 out of line). Predicted
  "back to parity", so wrong before it was understood. Then the round AS BUILT, with the same inline loop and two unrelated
  differences in ANOTHER function, read 0.90. No mechanism, so the engine keeps the inline probe that every A/B of the round measured.
- **What such a bench can resolve.** Rows from 10 to 100 elements read anywhere from 0.78 to 1.13 across nine builds whose logic on
  that path is identical, and they do so REPRODUCIBLY: two runs of one pair of binaries agree within 1 to 5 points, and two different
  bench binaries agree with each other. The copy swap cancels the bias between the two copies; it does not cancel what a change does
  to the addresses of everything after it. **Reproducible is not real**: it is a property of one build, and the next edit anywhere
  earlier in the file draws again (an alignment policy, `-falign-functions` / `-falign-loops`, is the untried lever). And on a box
  that has just run something long, single rows at n <= 5 scatter to 0.90-1.14 with the two orientations disagreeing (0.79 against
  1.02). A small-call row counts when it is at n <= 5, agrees in both orientations and in two runs, and has a mechanism that can be
  pointed at in the assembly. The final record run had the clearest trap: with the shipped engine as copy cb0 and the engine before
  the round as cb1, sorted input of 2 to 5 elements read 1.02-1.30 in that ONE orientation over five runs and either link order
  (1.00-1.05 in the other), worst when the box was slow; the same engine code in the same slot beside a DIFFERENT partner object
  read 0.95-1.00. One orientation of one pairing is layout, not cost.
- **The ceiling it exposed, and the one line taken from it.** Entering the dispatcher costs about 5.6 ns before any work: a
  thread-local read in the public entry, the call, and nine probed pages (the dispatcher's own frame is 24 KB, 36.6 KB with what it
  inlines). `logos_sort_f64` now returns at once when n < 2: **a call with 0 or 1 elements went from 8.5 to 2.9 ns** (0.29;
  orientations 0.34 and 0.25; predicted 0.35-0.6), nothing else moved. Every call of 2 to 24 elements still pays those 5.6 ns: 40%
  of a sorted triple, 3-10% of a random one. A thin front for them (prescan and insertion sort in a function with a small frame,
  the 36 KB body behind a call) is open direction 0g: predicted 1.3-1.6x on sorted input of 2 to 10 elements, 1.03-1.10x on random
  input of 3 to 10, nothing from 32 up.

### Found and NOT fixed
- **A sentinel the SAMPLE happens to contain defeats the partition's certificates.** With 840 zeros in 8.4M jittered timestamps
  the trace shows KS 0.99, a rejected key grid ("one bucket would take 168x its share") and a table scatter. The int32 counting
  route windows its sample against exactly this; the f64 partition does not.
- Three value bands 2^40 apart run at twice the uniform time at both thread counts (regime_switch).
- The lazy view's bucket sorter sends ranges under 4096 elements to a scalar quicksort when it runs parallel.
- The route is f64 only; its ladder is fractions of n and blocks are capped at 131072, so above ~117M elements the gate declines
  even when the reach is short; its stray shift is serial.
- The JavaScript engine's repair of the same probe starts at 4096 elements. Below that its nearly-sorted case is still a plain
  insertion sort with no budget: both changes measured 10-30% SLOWER on 2000-3000 elements, and the size bounds the worst case.
- Every f64 call of 2 to 24 elements pays about 5.6 ns to enter the dispatcher (nine probed stack pages); see the small end, above.

## (unreleased) — an outside review of a sibling line, re-tested in this tree: memory that grew with the thread count, a failed thread creation that returned wrong output, subnormals that came back unsorted, an in-place sort that ignored its threads (`make memprobe`, `make faultprobe`, `make slopebench`, `make ipbench`, `make sanitize`, `make tsan`)

### Where it came from, and how it was used
- A publication-readiness review ("publish after revision") of a separately developed line of this engine: v3.10, about 1,900 lines, with a
  linked-run fallback and a hash-trie reducer this tree does not have, measured on a different host. Its three headline issues were
  incomplete edge-domain validation (NaNs, infinities, allocation failures, extreme sizes), buffered memory that is understated and
  "may approach O(Pn) under pessimistic parallel allocation", and a novelty claim that needs a literature review.
- None of it was assumed to hold here. Each item was turned into a measurement in THIS tree, and what the measurement found was fixed.
  Nothing in the review said "a failed pthread_create returns the wrong answer" or "the partition allocates its regions twice"; it
  said what had never been tested, and that was enough.

### Memory, weighed for the first time (`make memprobe`)
- Every bench here times; none had ever weighed. `memprobe` interposes the malloc family and reports the peak traced heap of ONE COLD
  sort in a fresh child process (the pools make every later call look cheap), what is still held when the sort returns, and what is
  left after `logos_release_buffers()`. Bytes per element on top of the 8-byte input, 10M elements:

| | 1 thread | 14 threads, before | 14 threads, after | predicted (14 threads) |
|---|---|---|---|---|
| uniform, heavy_tail, near_sorted | 15.5 | 34.9 | **23.2** | 9.6 |
| block_perm (relocation) | 15.5 | 9.0 | 9.0 | |
| staircases (the knot route; the partition before it existed) | 15.5 | 34.9 | 9.0 | |
| few_distinct, quantized (counting routes) | 0.0 | 0.0-0.2 | 0.0-0.2 | under 1 |
| **dominant_90** (one value holds 90%, the rest distinct) | 15.5 | **104.5** | **17.8** | 76 if the warning was right |
| the in-place entry | 0 | | | 0 |

| still held after `logos_release_buffers()` | before | after |
|---|---|---|
| any partition shape, 1 thread | 7.5 | **0** |
| uniform, 14 threads | 14.0 | **0** |
| dominant_90, 14 threads | **95.2** | **0** |

- **The O(Pn) warning was right.** Every parallel leaf loop sized ALL P contexts by the LARGEST bucket. Balanced, that is under a byte
  per element; with a bucket no splitter can divide it is P times n: a gigabyte of scratch for an 80 MB array. But a thread only needs
  room for the bucket in its hands, and the buckets in hand at one moment are disjoint, so they sum to at most n. Contexts now fit a
  typical bucket (twice the mean) and an oversized one BORROWS room while it is sorted: a shared jumbo block if free (persistent, so a
  dominant value does not page-fault tens of megabytes per call), else a private temporary, else the bucket is sorted by a routine
  that needs no context. Same in the int32 path and in the lazy view's bucket sorter, whose per-thread scratch had the same shape.
- **A wasted double allocation.** The histogram-free partition asked for its slack-sized regions (12.8 bytes per element), found the
  block not 64-byte aligned, asked again with `posix_memalign` and freed the first. glibc serves a block that large from mmap behind
  a 16-byte header, so it is NEVER aligned: every cold parallel sort held the regions twice at its peak. Found by logging allocations
  with their call sites (`MEMPROBE_LOG`). Aligned first now.
- **`logos_release_buffers()` released 9 of the 23 bytes per element it was holding**, and on the dominant shape 9 of 104: it freed the
  scratch pool and left the partition's regions and every context behind, 950 MB still held for an 80 MB array after the call whose
  name says otherwise. It releases everything now, and the probe's last column is how that is known.
  It also left the int32 counting route's table pool, which is bounded only by that route's 64 MB budget; found when the probe
  grew int32 rows and one of them read 0.01 after release instead of 0.
- **The header said "~9n bytes of scratch".** Measured: 15.5 at one thread, 23 at fourteen. Corrected, with the breakdown.
  The int32 entry said "~5n" and the README "~16n". Measured per 4-byte element: 11.5 at one thread (4 scratch + 7.5
  sequential context), 6.5 at fourteen on the partition, 13.6 with a dominant value, under 0.1 on the counting route.
- **The bound, checked against the probe.** A context costs 7.5 bytes per element of capacity (4 + 2, grown with 25% headroom). With
  B = 12P buckets and m the largest bucket the old engine peaked near 9n + 25.6n + 7.5 P m: 35.6n on uniform (measured 34.9) and
  9n + 7.5 x 14 x 0.9n = 103.5n on the dominant shape, where the partition declines (measured 104.5). Now: 9n + 12.8n + about 1.3n
  of contexts = 23.1n (measured 23.2), and 9n + 1.3n + 7.5 m = 17.0n when it declines (measured 17.8). O(n) with a constant under
  24 whatever P is, where it had been O(P m).
- Predictions, graded. One thread: predicted 14, measured 15.5 (the context pool's 25% growth headroom). Fourteen threads: predicted
  9.6, measured 34.9, wrong by 3.6x: I forgot the partition's regions exist, and nobody could have predicted the double allocation.
  Dominant: predicted 76 if the warning held, measured 104. The warning held.
- Speed: neutral. The final engine against the committed one, paired, both copy orientations so that the code-layout term cancels,
  8M, 30 pairs each, run twice: 0.99 to 1.04 on nine rows (uniform, heavy_tail, dominant_90, block_perm, near_sorted,
  few_distinct and quantized at 14 threads, uniform and block_perm at one), and after the last fix of the round 0.98 to 1.06 on
  eleven, with bimodal at both thread counts (0.998 and 1.035: its leaves take the gap-split path that fix touched). At or inside
  the ~4% floor; the staircase row reads 1.40 and 1.43, which is the knot route. An earlier check of the memory fixes alone, on a hot box, read 0.96 to 1.07 with the same spread on the one-thread rows
  that change cannot touch.
- Still open: the 8-byte scratch and the 1-byte index cache are taken before any route is chosen, and the histogram-free partition
  never touches them. Acquiring them only when a route asks would take the common parallel path from 23 to about 14.

### Resource exhaustion: a failed allocation, a failed thread creation (`make faultprobe`, and about 2,150 checks in `make test`)
- Nothing here had ever run a fallback. On Linux malloc all but never returns NULL, so every `if (!p)` in the engine was written and
  never executed; and no `pthread_create` result was looked at, at any of 23 sites. The second is the likelier failure in practice:
  a container with a pid limit makes `pthread_create` return EAGAIN while malloc still succeeds.
- `faultprobe` interposes malloc, calloc, realloc, posix_memalign and pthread_create, counts the calls the ENGINE makes in one cold
  call (return address inside the executable; glibc's own allocations are left alone), then repeats the call failing the k-th one
  (`one`), or every one from the k-th on (`rest`: the resource has run out), each run in a fresh forked child, so a crash or a hang
  is a result. Four entries (f64 sort, int32 sort, the view's partition-then-sort-buckets for both), 19 shape cases, 1 and 4
  threads, 2.1M elements. Runs that ended badly (wrong output, crash or hang) out of runs made:

| one cold call, one fault injected | runs | came back sorted | wrong output | crashed | hung |
|---|---|---|---|---|---|
| an allocation, 1 thread, before | 1,500 | 1,239 | **261** | 0 | 0 |
| an allocation, 4 threads, before | 6,816 | 4,862 | **1,880** | **74** | 0 |
| a thread creation, 4 threads, before | 186 | 32 | **45** | 2 | **107** |
| all of them, after | 7,008 | **7,008** | 0 | 0 | 0 |

(The allocation runs fell from 8,316 to 6,822 because the k-way merge no longer allocates. Three targeted sweeps reach what the
main one does not, and are clean too, 1,226 allocation and 48 creation faults: the hash-count route forced with
`LOGOS_MCGATE_OLD=1`, relocation without the knot route with `LOGOS_NO_KNOT=1`, and the partition's spill path forced with
`-DHF_SLACK=0.2`. The final engine's own sweep, with the in-place entries and a 70-run chain added (23 cases): 7,894 injected
failures, 0 bad. Across them the creation of every one of the 19 worker functions in the default build has been failed at least
once, and 132 distinct allocation sites.)

- **A failed thread creation returned WRONG OUTPUT, silently, on the counting, lattice, near-sorted, relocation and sorted-prefix
  routes** (few_distinct 6 of 6, prefix_95 6 of 6), and hung the partition every time. The creating thread joined a garbage
  handle and read results a worker never wrote; at the six sites that meet at a barrier the barrier never filled. Three kinds of
  site, three answers. The 17 sites whose workers do not wait for one another go through `par_run`: the orphaned worker runs on
  the calling thread after the others are under way, so the phase is slower and never wrong. The three barrier sites that are
  queue-driven throughout (the two f64 partitions and the int32 one) go through `par_spawn`, which DROPS the orphan from the barrier: the
  barrier's count became atomic, and lowering it is safe because whoever completes a barrier arrives no earlier than the creating
  thread, which is itself a participant and has finished creating by then. Relocation and the knot route give each thread a static
  tile of the output, so there the caller also fills and copies back the orphans' tiles. The int32 counting route has static slot
  ranges and tables zeroed by their owners, and nothing written to a[] before its first barrier: it declines.
- **`merge_all_runs` returned with the runs unmerged when its two work lists could not be allocated**: a sort that came back unsorted
  and said nothing. The run-merge route, relocation and the knot route all come through it; the three-strand staircase failed 124
  of 200 single-allocation faults at ONE thread. The lists live on the stack up to 64 runs now, and beyond that a failed allocation
  folds the runs in left to right, which needs no list. The merge kernels never allocated.
- **A context that could not be allocated was still handed to the leaf.** The partition set a flag that only the CALLER looked at; it
  sat the buckets out while every worker ran the leaf on a context with no tables (a crash, dominant_90 25 of 112). And the
  sequential path called `ctx_free` on a stack context `ctx_init` had never touched when the scratch buffer failed first. That one
  was found by reading, while chasing the crash above; restored in a scratch copy it aborts the in-process harness
  (`munmap_chunk(): invalid pointer`) and passes every forked one-thread run, because a fresh child's stack is zeros. Contexts are all
  or nothing now (a failed one is all zeros and holds nothing, a half-built pooled bundle is no longer leaked), `ctx_fit` refuses an
  empty one, and whoever holds it sorts its buckets with the routine that needs no context.
- Smaller ones: two unchecked allocations on the histogram-free partition's spill path (reached only when a bucket overflows its
  region; tested by building the probe with `-DHF_SLACK=0.2`, which makes 21,000 spills on heavy_tail: 170 of 432 allocation faults crashed the committed engine there, 0 of 862 after); the
  view's `quantiles` returned with its output unwritten, and `logos_find_outliers_f64` returned 0, which reads as "no outliers"
  (it returns SIZE_MAX now).
- **The same probe lives in the harness**, in-process: every thread creation of a call failed in turn and every fourth allocation of
  a cold call, both modes, 20 cases, about 2,150 checks (the count moves by a few with thread timing). Only a process that has
  already sorted has a stack worth freeing, which is what the forked probe cannot offer. It also showed that the harness's own
  output lied: redirected stdout is block-buffered, so a crash ate the last 4 KB and the log ended four sections early. Line-buffered now.
- Predictions, graded. Allocation faults: predicted 5-15% of runs bad at four threads and under 5% at one; measured 27% overall,
  because one defect sat behind the hundreds of allocations a k-way merge makes. Thread creation: predicted every run bad; measured
  154 of 186, and the 32 that passed did so on a garbage join handle that happened not to fault.
- The contract this buys, and the header now states it: a failed malloc or pthread_create costs speed, never correctness. There is
  no error return because there is no failure left to report.

### The strict in-place sort goes parallel, and gets its first checks (`make ipbench`)
- The review's headline comparison was in-place against buffered, and the sibling line it reviewed sorts in place in parallel.
  Here `logos_sort_f64_inplace` took an `nthreads` and ignored it, and `make slopebench` priced the gap: at 50M and ONE thread
  the in-place sort costs 3.6 to 21 times what the buffered sort costs per element (uniform 100.7 against 25.3 ns), after which the
  buffered sort goes parallel and this one could not.
- The placement pass of an American-flag sort is cycle-following and does not split across threads without block machinery. What
  splits for free is everything AFTER a pass: its 256 buckets are disjoint ranges of the array. So ranges are tasks. A worker pops
  a range; a small one (up to 32k elements) it sorts outright with the sequential kernel; a large one gets ONE placement pass,
  after which its large children go back on the stack and its small ones are sorted where they lie. Still strictly in place: the
  task stack is a fixed 4,096 entries (96 KB) whatever n is, and a full stack means a child is sorted inline. From n = 2^20.
- Measured, 14 threads, paired, every output verified (the left column is a scaling number, the sort against ITSELF at one
  thread; the right one is multi against multi):

| 8.4M | in-place, 1 thread / 14 threads | in-place 14 / buffered 14 |
|---|---|---|
| uniform | **2.50x** (2.38 .. 2.65) | 9.8x slower |
| heavy_tail | 2.46x | 9.2x |
| bimodal | 2.64x | 12.3x |
| near_sorted | 1.65x | 15.0x |
| few_distinct | 1.34x | 23.9x |
| quantized | 1.45x | 24.3x |
| block_perm | 1.29x | 23.0x |
| dominant_90 | 1.08x | 3.3x |
| uniform, 50M | **2.85x** (2.71 .. 2.90) | 6.0x |
| at the 2^20 threshold, all eight shapes | 1.02 .. 2.26x, no row below 1 | |

- The first pass over the whole array still runs on one thread, and that is the whole story of the table: a serial share of
  30 to 35% caps random data near 3x, and the structured shapes, whose one expensive pass IS that first pass, gain least. Zero
  scratch is bought with time: 6 to 24 times slower than the buffered sort at the same thread count, against 0 bytes per element
  instead of 23. NEXT, if it matters: the placement pass itself, by speculative permutation with a repair phase (PARADIS); the
  three read-only passes before it (prescan, OR/AND, histogram) are the cheap third of the serial share.
- Predictions, graded: uniform 2x to 3x, RIGHT (2.50, 2.85). few_distinct 1.0x, WRONG on its premise: 0, 1e6 .. 4e6 differ in
  four bytes, not one; but five values make a task tree two serial passes deep and at most four wide (1.34x). near_sorted and
  block_perm 1.5x to 2.5x: half right (1.65x, 1.29x). Against buffered "still 3x to 6x slower": WRONG at 8.4M (9.8x), right at
  50M (6.0x); I priced the buffered sort at its hot-box speed.
- **Nothing in the harness had ever called the in-place entries.** They were verified only by the benches that time them, and
  they are the fallback for arrays above 2^32 - 1 elements. 408 checks now: both types, both sides of the parallel threshold, 1 to
  8 threads, special values, and NaN (bit-pattern permutation only). They passed on the first run. `make faultprobe` drives them
  too: 40 injected failures, 0 bad.

### IEEE special values (432 checks)
- **NaN-free data holding SUBNORMALS came back UNSORTED** from every parallel sort of 300k elements or more, on the committed engine
  too. A bucket whose value range is narrower than about 1e-305 makes `classes / range` overflow to +inf; every element then computes
  `x * inf`, the minimum computes `0 * inf = NaN`, and in the coarse bucket leaf that NaN's garbage cast was clamped into the LAST
  bucket. Yesterday's infinity fix guarded the RANGE (`isfinite(max - min)`); the thing that must be finite is the SCALE. Guarded in
  flash, coarse, the skew probe and both certificates. The same leaves had been running quadratically there as a bonus: 43 ms of
  leaf time for 300k elements, 7 after.
- **NaN.** The order with a NaN present stays unspecified, as documented, and the probe shows how unspecified: the other values came
  back out of order in 246 of 672 runs, because a NaN fails every compare and a prescan takes `[1, NaN, 0.5]` for ascending. What is
  now promised AND tested is the floor: no crash, no out-of-bounds access (ASan clean), and an output that is a permutation of the
  input's BIT PATTERNS. That floor was broken in 16 of the 672: the spill sort compacted the numbers and wrote the canonical NaN over
  the tail, so signalling, negative and payload NaNs came back as different bits. It swaps now.
- **With a NaN in the data a parallel sort could fail to RETURN.** The gap-split partition (two modes far apart: split at the gulf,
  sort each side) scanned with `a[i] < pivot` from the left and `a[j] >= pivot` from the right. A NaN fails BOTH, so when the two
  scans met on one neither index moved again. Whether they meet on a NaN depends on the order inside a bucket, which depends on
  thread timing: the harness spun about one run in fifteen, with one worker at 100% and the caller asleep in a join, and it took a
  looping hunter that signals the spinning thread to get a backtrace (no debugger on this box). Heavy-tailed data with 1% NaN at 4
  threads hit it about one sort in thirty. Inherited from the JS engine, where it is deterministic (below). One predicate now,
  `x < pivot`, for both scans, so a NaN goes right. The floor this entry promises therefore reads: the call RETURNS, nothing is
  read or written out of bounds, and the output is a permutation of the input's bit patterns. 150 such sorts under a watchdog are
  in the harness (`NaN termination`); against the old loop that section fails at repetition 75.
- **`logos_sort_f64_nan_last`**, for callers who cannot promise NaN-free data: NaNs to the tail with their bits intact (one
  sequential pass, `logos_nan_to_end_f64`), then the prefix is sorted. The default entry does not pay that pass, and neither does
  `std::sort`, whose contract a NaN also breaks.
- Left alone: with a NaN in the data the flash and coarse leaves still cast NaN to an index. Clamped, memory-safe on x86-64 and
  AArch64, undefined by the letter; both are per-element loops that would need an A/B.

### Sanitizers, whole harness, for the first time
- `make sanitize`: AddressSanitizer, UBSan, `float-cast-overflow` and (from this round) LeakSanitizer over every check: no memory
  error and, after the fix below, no leak; the only reports are the NaN casts above. `make tsan`: ThreadSanitizer, **0 data races** (it needs address-space randomisation off on this kernel,
  `setarch -R`, or it dies at start with "unexpected memory mapping"). `float-cast-overflow` is NOT part of `-fsanitize=undefined`
  in GCC, and it is the one that found the infinity and subnormal defects. `LOGOS_SKIP_BENCH=1 ./harness` runs the checks without
  the timing half, the only form a sanitizer run can afford. The fault section steps aside under a sanitizer: both interpose the
  same functions.
- Sizes: `n` is a `size_t` but the buffered engine counts in 32 bits. Above 2^32 - 1 elements the public entries (sort and
  partition, both types) hand the array to the strict in-place sort, which counts in `size_t` throughout (read, not run: no machine
  here holds 34 GB of doubles).

### The scaling exponent, measured in pairs (`make slopebench`), and the scale report after the review's method
- The review read a scaling exponent off two sizes through the cost per element: with c = t/n, t ~ n^p gives
  p = 1 + ln(c_50M / c_2M) / ln 25. The scale report holds those two numbers, but on this box its rows are measured minutes
  apart and the machine's speed drifts by a multiple in that time (methodology 2), so a slope read off two rows carries the
  drift. `slopebench` measures it the way this project measures everything else: ONE 50M sort adjacent to TWENTY-FIVE 2M sorts
  of the same shape (the same generator, the same number of elements through the same work buffer), order alternating, median
  of 8 pairs. The ratio of the two times IS c_50M / c_2M.
- Two calibrations, because a new instrument is a claim. The null (50M on both sides, true growth 0) read +2.0%, +1.7%, +0.4%
  and -0.8%: the floor is about 2%. And `std::sort` on uniform must show at least the comparison-count slope, log2(50M) /
  log2(2M) = 1.222, p = 1.062: measured +23.5% and +22.3% in two runs, p = 1.066 and 1.063.
- Growth of the cost per element from 2M to 50M, two full runs (first / second):

| | std::sort, 1 thread | logos, 1 thread | logos in-place, 1 thread | logos, 14 threads | in-place, 14 threads |
|---|---|---|---|---|---|
| uniform | +23.5 / +22.3% | +13.4 / +16.3% | +21.2 / +21.6% | +34.3 / +26.4% | -20.6% |
| heavy_tail | +21.4 / +21.0 | +19.3 / +17.3 | +34.3 / +38.2 | +7.4 / +19.9 | -15.9 |
| bimodal | +24.0 / +21.7 | +14.0 / +17.6 | +22.7 / +21.3 | -14.4 / -14.7 | +0.4 |
| near_sorted | +27.1 / +29.8 | **+29.3 / +33.4** | +1.5 / +2.8 | **-59.5 / -53.8** | -22.5 |
| few_distinct | +13.0 / +15.2 | **+0.5 / -1.7** | +0.8 / +0.3 | -30.7 / -39.4 | -1.5 |
| quantized | +1.8 / +3.0 | **-0.1 / -0.8** | +4.6 / +3.9 | -40.2 / -56.7 | -15.6 |
| block_perm | +37.5 / +31.3 | **-4.4 / +0.2** | +18.1 / +37.8 | -50.0 / -37.8 | -29.1 |

- How far to trust it. The one-thread cells reproduce within about 3 points, with one exception that does not (block_perm
  in-place, 18 against 38, with p10-p90 ranges that do not overlap: unexplained, so that cell is not evidence). The 14-thread
  cells reproduce within 10 to 17 points. What follows is only what is larger than that.
- One thread. logos grows LESS than `std::sort` on every random shape (13-19% against 21-24%, p about 1.04-1.05 against 1.06),
  and the counting and relocation routes are exactly linear (p = 1.00 on few_distinct, quantized and block_perm, where
  `std::sort` pays 13-15%, 2-3% and 31-38%). near_sorted is the one buffered route that grows faster than `std::sort` (+29-33%).
  The in-place sort grows like a comparison sort (+21% on uniform; the reviewed sibling measured +9.0% for its own) and at 50M
  costs 3.6 to 21 times the buffered sort per element.
- Fourteen threads: two opposite effects, and the prediction ("about flat") caught neither. Uniform gets 26-34% DEARER per
  element: 2M doubles are 16 MB and sit in this CPU's 24 MB last-level cache, 50M do not. The structured routes get 31-60%
  CHEAPER: at 2M near_sorted costs 2.2-2.8 times per element what it costs at 50M, quantized 1.7-2.3, block_perm 1.7-2.1,
  few_distinct 1.5-1.6. At 2M a parallel call on those routes is mostly FIXED cost, of the order of 1 to 4 ms per call. That is
  a lead, not a result. What a spawn costs here: creating and joining 13 empty workers takes 0.77 ms at the median (0.56 to 1.02), about 60 us a
  thread, so a route with two spawn phases pays 1.5 ms of a 4.6 ms near_sorted call at 2M: about a third of that fixed cost,
  and nothing at 50M. Every `pthread_create` now goes through `par_run` / `par_spawn`, which is where a persistent worker pool
  would go. It has not been built: it changes the library's threading model, and it wants its own A/B at 1M to 4M.
- Predictions, graded. `std::sort` +22-30%: RIGHT. Buffered logos at one thread +5-15%: RIGHT on uniform (13, 16), low on
  heavy_tail (17-19). In-place +10-25%: RIGHT on uniform. Counting shapes flat: RIGHT. The null within 3%: RIGHT. Fourteen
  threads "about flat, -5 to +10%": WRONG on six shapes of seven, in both directions.

- The report gained what the review's tables had. The strict in-place sort is a series in both panels, one thread and fourteen;
  the ladder's ceiling is now itself a size, so 19 sizes from 0 to 50M; and under each ns-per-element table sits a slope row,
  p = 1 + ln(c_hi / c_lo) / ln(n_hi / n_lo) from 1M to the largest size, labelled as a sketch because its two rows are minutes
  apart: `make slopebench` is the measurement. Both checkers pass, the stub one and the headless browser in six axis and range
  variants.
- At 50M and 14 threads (one repetition per cell, contenders adjacent in time, every output verified against `std::sort`): logos
  leads on bimodal (169 ms against 210 for x86-simd-sort and 262 for IPS4o), quantized (59 / 167 / 121), int_wide (187 / 224 /
  369), near_sorted (145 / 220 / 193) and the block shapes (68 / 332 / 247 and 118 / 263 / 354); it trails x86-simd-sort on
  uniform (279 against 209, IPS4o 289) and heavy_tail (455 against 326, IPS4o 660), and trails IPS4o on few_distinct (63 against
  50). The same picture as at 8M. The in-place sort at 14 threads takes 1.2 to 2.1 s there, 2.8 times under its one-thread time on
  uniform, which agrees with ipbench's 2.85.

### Boundaries, a long chain, and what leak checking found once it was switched on
- **The harness can run one section, in a loop.** `LOGOS_HARNESS_ONLY=<part of a section title>` and `LOGOS_HARNESS_REPEAT=N`: how
  the intermittent spin above was chased without paying for the whole harness on every try.
- **A boundary sweep, 12,058 checks.** A dispatcher is a list of thresholds, and an off-by-one lives exactly at one: every n from 0 to
  300, every power of two from 2^9 to 2^21 from below, at and above, and the multiples the thread policy and the per-route caps cut at
  (100k, 350k, 500k), both types, 1, 4 and 8 threads, the buffered and the in-place entries. It passed on its first run; it is
  there so that the next threshold does too.
- **A chain of more runs than `merge_all_runs` keeps on its stack.** Three long sorted blocks, shuffled, then 70 short runs interleaved
  over one value range: at one thread the knot route takes it with 70-71 live strands (traced), past the 64-run stack lists, and the
  resource section fails every allocation of that call in turn, which reaches the fold that needs no list. The first cut of the
  shape put the chain at the HEAD and never reached the merge: relocation's progressive gate judges the prefix it has seen, and a
  prefix that is all one chain makes it bail. Found by tracing the route before trusting the test.
- **Leak checking had been off in `make sanitize`, and the first run with it on found a leak**: 313 KB in 28 allocations over the
  harness, all in the IEEE section. When the dispatcher's sorted sample is not handed to the partition (a NaN in it, another size,
  or it could not be extended) the partition draws its own, and the first one was freed only on the fallback label, never on the
  normal returns: 11 KB per such sort, in the committed engine too. One `else` fixes it. LeakSanitizer works under WSL2; the
  pools are reachable from statics, so only a real leak is reported.
- **`-DLOGOS_NO_SPLITTER_TABLE` did not compile, so `make stbench` had not built since the per-mode certificate was added**: that
  block reads the modes the splitter table found without asking whether there is one. The flag now implies `-DLOGOS_NO_MODEKS`, the
  pre-H0b engine passes the whole harness again, and a compile-only pass over the Makefile (a scratch copy, every target's compile and link lines) now builds all 49 targets that have a compile step. With `-DLOGOS_NO_RELOCATE` the day before, that is
  the second A/B switch found rotten: a flag nobody builds is a fallback nobody runs.
- ThreadSanitizer now covers the thread-creation faults too: under TSan the harness keeps that half of its resource section and
  forwards the creations it does not fail to TSan's own interceptor (`__interceptor_pthread_create`), so `barrier_drop` and the
  orphan paths run under the race detector. Measured: 186 failed thread creations under ThreadSanitizer, 0 failures, 0 data races.

### The other engines, on the same inputs
- All three engines share the design behind the infinity and subnormal defects, a value-linear bucket map whose scale is
  `classes / range`, so the C probe's edge inputs were run on each.
- **JavaScript: `sort()` never returned on one NaN between two clusters.** The same gap-split loop, and JavaScript is single
  threaded, so it is deterministic: `[lows..., NaN, highs...]` with 6,000 elements hangs forever, plain Array or Float64Array. In
  the published package. Fixed the same way, and `test/perf-guard.js` holds the case.
- **JavaScript: correct on all 126 runs of the C probe's edge inputs, and two quadratic cliffs.** A subnormal-wide range makes the scale overflow to Infinity; in
  JavaScript `x * Infinity | 0` and `0 * Infinity | 0` are both 0, so nothing goes out of bounds and nothing comes back unsorted, but
  every element lands in bucket 0 and flash sort's closing insertion pass is O(n^2): **300,001 subnormals took 77 s** (42 ms
  after). The second has nothing to do with subnormals: flash sort is one scatter and ONE insertion pass, so a dense cluster of
  distinct values inside a wide range sits in one of its n buckets, and the dispatcher's safety probe compares two quartiles,
  between which such a cluster hides: **13-21 s for 300k elements** (about 30 ms after). Fixes: a scale that is not a positive
  finite number goes to the introsort, and the closing pass carries a displacement budget (16 shifted slots per element) past
  which the array goes to the coarse sort. Cost on healthy flash-path data about 3% (interleaved old against new, 40 pairs, 12
  rows; the null of two identical modules reads 1.000 with rows from 0.97 to 1.06); scanning the counts for a large bucket
  instead was measured first and rejected at 5-11%. `test/perf-guard.js` guards both. See engines/js/CHANGELOG.md.
- **Rust: passes** (`engines/rust/tests/edge_values.rs`, 252 sorts in 2.7 s). It guards the range and not the scale; reading its
  leaf, what saves it is that Rust's float-to-integer casts saturate and a concentration check then bails to the standard sort.
  That is a test now, where it had been luck.

### From the review's list, not done
- Cross-compiler, cross-architecture, 32-bit and big-endian builds: this tree has only ever met GCC 13 on x86-64.
- A literature review, beyond a first pass. FINDINGS 3ae lists what was checked (titles, venues, years; the papers not re-read):
  relocation is adaptivity to the known measure Block (Carlsson, Levcopoulos, Petersson 1993), the knot route's crossing cost is
  close kin to Osc (Levcopoulos, Petersson 1989/1993), run-adaptive merge policies (Powersort) are a different axis, and parallel
  in-place sorting is well trodden (IPS4o, PARADIS, Regions Sort). Nothing in this tree should be called a new measure or a
  complexity result; the engineering claims stand as they are. The full review is still owed.
- `pthread_mutex_init`, `pthread_cond_init` and `pthread_join` results are still unread. None can fail for the reasons a container
  imposes, and none has a fallback that means anything.

## (unreleased) — the scale report grows a small end: n = 0, 1, 3, 5, 10, and ns per element on a linear axis; and what those sizes found (`make scale-full`)

### The report
- **Sizes.** 0, 1, 3, 5 and 10 were asked for; 32, 100, 316, 1000 and 3162 continue in the ladder's own half-decade steps so the
  ns/element curves have no three-decade hole before the old start at 10k. Seventeen sizes, 0 to 10M. `LOGOS_MIN_N=10000` restores
  the old ladder.
- **Small sizes are timed as batches.** Five elements sort in about 20 ns and reading the clock costs about that, so below 10k a
  size is measured as up to 4096 DISTINCT instances of the shape (seeds 7, 8, ...), copied fresh outside the timed region and sorted
  back to back inside it; the fastest batch divided by the instance count is the per-sort time. Distinct, because sorting the same
  five values again and again trains the branch predictor and flatters every comparison sort. Every instance of the last batch is
  checked against std::sort of that instance. n = 0 is a real row, the cost of one call; a sort the compiler inlines to nothing
  there is floored at 0.01 ns, because 0 means "absent" in the report's data.
- **A second chart in every panel: nanoseconds per element, value axis ALWAYS linear**, for all nine shapes at 1 and 14 threads.
  Only its n axis follows the linear / log-log toggle. n = 0 has no per-element value (a gap) and cannot sit on a log axis, so the
  log view drops that row from the charts and leaves it to the tables. Size buttons (`all sizes`, `n <= 10k`, `n <= 10`) cut every
  chart down to the small end, because a linear n axis puts everything below a million into its first pixels.
- **A linear axis has one enemy, a fixed cost divided by a tiny n.** The LSD radix baseline pays about 7 us per call whatever n
  is: 2300 ns per element at n = 3 against 2-100 for everything else, and in the first rendering that one spike pressed every
  other line into the floor (seen in a real screenshot, not guessed). The ns/element axis now opens at three times the 85th
  percentile of what is drawn. The linear TIME chart gets a narrower rule, since there the largest n towers over the rest by right
  and a percentile would clip honest points: it is capped only when ONE series peaks more than four times above every other. In
  both cases the caption names the series and its true peak, the live legend and the tables keep the true values, and hiding that
  series rescales the axis. Nothing is dropped.
- Tables: time per sort with units that walk from ns to s (they used to print 0.00 ms), and a second table of ns per element.
- `LOGOS_SCALE_REUSE=<report.html>` re-renders a report's measurements through the current template and measures nothing: the run
  takes minutes, the presentation is what gets iterated on, and measurements must not change because a caption did.
- Checkers. `tools/check_report.js`: n = 0 is legal on a linear n axis and fatal on a log one, the ns/element chart may never be on a
  log value axis, every panel must have one, n = 0 must actually be drawn in the linear view, and every size preset is clicked in
  both axis modes; the previous report still passes. `tools/browser_check.py` renders six variants in the real browser (two axis
  modes times three size views). Both clean on the shipped report.

### What the new sizes found
**1. Two regressions of my own, a day old, in the knot route.** Every A/B bench in this tree starts at 1M and the harness checks
answers, not times, so a cost per CALL was invisible to all 2059 checks and to `knotbench`.
- The route's two runtime switches were read with `getenv` on every sort: about 40 ns. Three sorted elements went from 6 ns to 42.
- The cost gate let the plan run on runs of two or three elements. Random data at n = 32 is 13 runs; when nearly every element is
  a run endpoint almost nothing "crosses", the gate says go, and the sweep (five allocations, a sort of the 2R endpoints, a gallop
  per live strand per cut) sorts the array the slow way: 2431 ns against 781 on the committed engine, and block_perm at n = 100
  4969 against 2105. J5's chain gate never had the problem, since short random runs all overlap.
- Fixed: the switches are read only from 512 elements up (below two strands of `KNOT_MIN_AVG_RUN` the route cannot fire), and the
  route is asked only where the average run is at least `KNOT_MIN_AVG_RUN` = 256 elements, in all three places it is asked.
  Confirmed in ONE binary (committed engine and working tree alternating batch by batch, since two separate runs had std::sort itself
  drifting 20%): working tree / committed = 0.93-1.03 at every size from 0 to 3162 on uniform and block_perm. 2059 checks pass.

**2. Below about a hundred elements logos loses to std::sort**, on every shape (one thread, logos / std::sort):

| n | 0 | 1 | 3 | 5 | 10 | 32 | 100 | 316 | 1000 | 10k | 10M |
|---|---|---|---|---|---|---|---|---|---|---|---|
| uniform | 25 | 4.0 | 9.0 | 9.0 | 2.0 | 2.0 | 0.50 | 0.38 | 0.24 | 0.18 | 0.17 |
| near_sorted | 26 | 4.0 | 2.4 | 2.0 | 1.7 | 1.2 | 1.9 | 1.06 | 0.95 | 0.69 | 0.22 |
| block_perm | 26 | 4.1 | 9.3 | 5.4 | 2.2 | 1.7 | 1.27 | 0.69 | 0.65 | 0.26 | 0.07 |

  Per call on uniform data, ns: n = 0: std::sort 0.3, logos 8.2 (a call the compiler cannot inline); n = 3: 5.3 against 48; n = 5:
  9.5 against 85; n = 10: 67 against 136 (x86-simd-sort 28). Logos stays ahead from n >= 100 on uniform, heavy_tail, bimodal,
  quantized and int_wide, from 316 on few_distinct and the block shapes, from 1000 on near_sorted. None of this is the knot route
  (item 1's A/B is against the committed engine); it is what a dispatcher costs when there is nothing to dispatch.

**3. A step in the dispatcher between 25 and 76 elements** (`bump_probe`, fine ladder, uniform random, committed engine identical):

| n | 24 | 25 | 32 | 48 | 64 | 72 | 80 | 88 | 96 | 128 |
|---|---|---|---|---|---|---|---|---|---|---|
| logos ns per element | 17.7 | 36.3 | 24.3 | 25.7 | 25.3 | 26.7 | 15.0 | 10.8 | 10.7 | 9.8 |
| average runs in the input | 10 | 11 | 14 | 20 | 27 | 30 | 33 | 37 | 40 | 53 |

  Predicted before it ran: the bump starts at 25, where insertion sort hands over to run detection, and ends near 80, where random
  data has more than the 32 runs `detect_runs_limited` allows and it bails at once. Both edges landed exactly. In between the engine
  detects runs of two or three elements, builds a relocation plan (two allocations and a libc `qsort` of the descriptors), has it
  declined, and only then sorts: 72 random doubles take 1.9 us and 96 take 1.0. NOT fixed here: it is a dispatcher change with
  its own A/B, and this entry is a report. The obvious lever is the same one item 1 needed, a minimum average run length.

**At the large end** (10M, ns per element, one thread | 14 threads): uniform logos 13.4, x86-simd-sort 10.3, IPS4o 28.8, std::sort
77.2 | logos 1.94, x86-simd-sort 2.73, IPS4o 3.80, gnu_parallel 11.0; block_perm logos 2.8 against 12.7 and 20.3 | 1.13 against 2.73
and 2.85. Calibration memcpy 3.17 ms, std::sort 1M 65.3 ms; rows are measured minutes apart, so read within a row.

## (unreleased) — the knot route: comparisons only where runs cross; infinities, which crashed the parallel sort; and relocation no longer beats the partition at 14 threads (`make knotbench`)

### The idea, and what it maps to
- The request: when the array shows "knotting", read it as a linked list, find ascending and descending runs chunk-parallel with one
  chunk per core, and untie the knots the way the unknotting problem is attacked. The mapping that made it buildable: a **strand** is a
  maximal run; the run descriptors sorted by first value are the **linked list** the sorted output walks along; a **crossing** is a value
  interval in which two or more strands both hold elements; a **knot** is a group of strands joined by crossings, which is J5's chain.
  One honest note on the analogy: the topological unknotting problem is in NP and co-NP and not known to be polynomial, while sorting
  is O(n log n) whatever is done. What the framing buys is a cost that follows the **crossing number** instead of n log n.
- Relocation (J5) treats a knot as one unit. It merges every element of a chain, and it declines the whole array once chains hold more
  than half of it. But two runs that overlap in a tenth of their range are tangled only in that tenth. The knot route sweeps the 2R strand
  endpoints in value order, cuts each strand where the set of live strands changes (a galloping search from where its last piece
  ended), **copies** every piece that lies alone in its interval and **merges** only the pieces that share one.
  Work: O(R log R) plan, O(n) copies, O(crossing mass x log k) comparisons.
- Pieces are counted by the ELEMENTS they hold, so a fused run whose interval hides a hole reads correctly: over the hole its piece
  is empty and nothing crosses. That is the defect that made J5's first cut never fire and the reason J5 gap-splits; here it cannot occur.
- Parallel form, because a sequential route must not be offered to a parallel sort (ledger, twice): chunk-parallel run detection
  (J5's, with a crossing-cost gate beside its chain gate); a serial O(R log R) plan; placement in which every thread fills its tile of
  the output by copy, two-strand crossings are merged straight from `a[]` into place off an atomic queue (a large one is first cut into
  grains at values read from its largest piece, the same operation as one more sweep boundary), and crossings of three or more strands
  are staged side by side and merged after the barrier with `a[]` as scratch.
- Where: `knot_plan`, `knot_worker`, called from `relocate_try_f64` and from the leaf's `runs_relocate_f64`. Whatever J5's own gate
  accepts is accepted too, since there the finer plan can only do less work, and every early gate asks the knot question only when J5's
  answer would be no. `LOGOS_NO_KNOT=1` gives J5 exactly as before at runtime, `-DLOGOS_NO_KNOT` removes the route,
  `LOGOS_KNOT_MAX=<x>` overrides the cost gates. f64 only; the int32 path still has no relocation at all.

### Measured against the shipped engine (`make knotbench`: one binary, `LOGOS_NO_KNOT` off vs on, paired, every row verified first)
The target shape is a STAIRCASE: sorted blocks whose value ranges each overlap the next (time-series shards with clock skew, rotated
logs, sorted files with overlapping boundaries). One chain covers everything, so J5 declines and the array pays for a full sort.
`stairs_xNN` has NN% of its elements inside a crossing. Cost is the gate's measure, the sum over crossings of mass x ceil(log2 k).
Floor (`make pairprobe`, same session): 3.2% at 8M, 4.5% at 1M; a 14-thread row of a 10 ms sort is noisier, see p10/p90 in the bench.

| route off / route on | cost | 8.4M, 1 thr | 8.4M, 14 thr | 3.1M, 14 thr | 1M, 14 thr | predicted at 8.4M (1 thr; 14 thr) |
|---|---|---|---|---|---|---|
| block_perm64 (J5's shape, no crossings) | 0 | 1.000 | 0.986 | 1.019 | 1.002 | 1.00; 1.00 |
| block_overlap (J5's: 25% in chains, 2.5% crosses) | 0.03n | **1.099** | **1.131** | 1.095 | 1.027 | 1.10-1.25; 1.03-1.15 |
| stairs_x10 | 0.10n | **4.44** | **1.20** | 1.23 | 1.38 | 2.2-3.5; 1.5-2.2 |
| stairs_x20 | 0.20n | **4.05** | **1.32** | 1.20 | 1.33 | 2.0-3.2; 1.4-2.0 |
| stairs_x40 | 0.39n | **3.42** | **1.35** | 1.21 | 1.40 | 1.8-2.8; 1.1-1.6 |
| stairs_x60 | 0.59n | **2.93** | **1.31** | 1.27 | 1.44 | 1.6-2.5; 0.95-1.3 |
| stairs_x80 | 0.79n | **2.62** | **1.39** | 1.18 | 1.29 | 1.4-2.2; 0.85-1.15 |
| stairs_x100 (every element crosses) | 0.98n | **2.64** | **1.50** | 1.30 | 1.44 | 1.3-2.0; 0.75-1.05 |
| triple (three strands live at once) | 1.57n | **2.58** | 1.24 (declined) | 1.29 | 1.30 | 0.9-1.5; 0.6-0.95 |
| quad (3-4 live) | 1.96n | **2.68** | 1.09 (declined) | 1.04 | 1.02 | added after the first run |
| k8 (8 live) | 2.95n | **1.40** | 1.15 (declined) | 1.21 | 1.28 | added after the first run |
| stairs_ord_x20 (blocks in value order) | 0.20n | **4.02** | 1.02 | 1.00 | 1.01 | 2.0-3.2; 1.00 |
| zigzag_x20 (in order, every other block descends) | 0.20n | **3.73** | 1.05 | 0.99 | 0.98 | 2.0-3.2; 1.4-2.0 |
| shards14_x20 (14 shuffled strands) | 0.19n | **1.85** | 1.09 | 0.98 | 0.98 | 2.0-3.2; 1.3-1.9 |
| uniform, few_runs, segmented, near_1pct (guards) | | 1.000-1.015 | 0.988-1.055 | 0.988-1.027 | 0.971-1.030 | 1.00 |

One-thread rows at 3.1M and 1M repeat the 8.4M column within a tenth (4.3-4.5 on x10 down to 2.4-2.8 on x80 to triple, guards
0.98-1.02). The last three shaped rows at 3.1M and 1M are from 80-pair re-runs, taken because 40 pairs read 0.95-0.97 there; the
in-order staircase never reaches this route at 14 threads, so its 0.95-0.97 at 40 pairs was the noise, and it re-read 1.00-1.01.

- **"(declined)" rows still win.** triple, quad and k8 at 14 threads are DECLINED by the knot route and read 1.09-1.30, because its
  sample gate declines in half a millisecond where J5 scans the whole array for runs before its chain gate says no.
- shards14 at one thread read 3.5-3.7 in the first two runs and 1.85 in the confirmation run. Not noise: adding two shapes to the
  table changed its seed and so the shuffle. The knot route takes 23 ms for every shuffle of the 14 strands; the shipped fallback takes
  83 ms for one and 41-43 ms for three others (minimum of 7 each), because ascending junctions fuse runs and the fused count lands on
  9 (past `MERGE_MAX_RUNS`: the sequential leaf) or on 8 (the run merge). So the one-thread gain is about 1.8x where the fallback
  would have been the run merge and 2.6-4.4x where it would have been the leaf.

### Measured against the PURE PARTITION, which is the number that says what the route itself is worth at 14 threads
"Against the shipped engine" flatters the parallel rows. On a shuffled staircase the shipped relocation scans the array and THEN
declines (traced: bail at 3.0 ms on 64 strands; on 14 shards the sample gate declines at 1.2 ms), and that wasted scan sits in the
baseline. So: two engine copies in one binary, this engine against one built with `-DLOGOS_NO_RELOCATE`, both copy orientations,
sqrt(A/B) so the layout bias cancels (the `tools/ledger_rerun.py` arithmetic), 8.4M, 30 pairs:

| engine / pure partition | 1 thread | 14 threads |
|---|---|---|
| block_perm64 (**J5's own target, no knot work at all**) | 4.47 | **0.95** |
| block_overlap (J5's own) | 4.37 | **0.92** |
| stairs_x10 | 4.14 | 0.96 |
| stairs_x20 | 3.80 | 0.97 |
| stairs_x60 | 2.78 | 1.03 |
| stairs_x100 | 2.55 | 1.11 |
| triple (accepted in that build) | 2.43 | **0.89** |
| zigzag_x20 | 3.54 | 1.01 |
| shards14_x20 | 1.77 | 1.08 |

- **At one thread the route is worth 1.8-4.1x. At 14 threads it TIES the partition** on two-strand crossings and lost 11% on three
  strands, which is why the parallel gate ended at 1.0n. With three strands declined early the triple row against the shipped engine
  went UP, 1.16 -> 1.24 at 8.4M, 1.09 -> 1.29 at 3.1M, 1.20 -> 1.30 at 1M: the partition was the better sort and the only thing
  wrong with it had been the scan in front of it.
- So the honest reading of the 14-thread column of the first table: the 1.2-1.5x on shuffled staircases is the shipped relocation's
  late decline turned into useful work (or, for the declined rows, simply removed), not a placement that outruns the partition.
- **The finding that is not about this route: relocation's own parallel win is gone.** J5 measured 1.9-2.2x at 20 threads on
  block_perm on 2026-09-10. Against today's partition it is 0.95 on block_perm64 and 0.92 on block_overlap at 14 threads. Nothing in
  J5 changed; the partition did (direct-index splitter tables 1.6-3.0x, histogram-free scatter 1.14-1.45x, 12 buckets per thread), and
  the comparison was never re-run. At one thread J5 is 4.4-4.5x and untouched by any of that. NOT acted on here: gating relocation to
  sequential sorts is a change to shipped behaviour that deserves its own A/B across sizes. Recorded as an open direction.

### Predictions, graded
- **Right:** identical plans read 1.00 (block_perm64); block_overlap inside both ranges; the in-order staircase never reaches the route
  at 14 threads (the near-sorted route is asked first, and its strays turn out to be 63 disjoint runs that J5 relocates); every guard.
- **One thread, too low everywhere:** predicted 2.0-3.5, measured 2.6-4.4. I priced the run scan and the two copies at their worst.
- **Fourteen threads, too high on the light staircases and on zigzag and shards14:** predicted 1.3-2.2, measured 1.05-1.35. I took
  the partition at its old 19-21 ms. Interleaved in one process at 8.4M (minimum of 11) it is 15.4 ms on uniform random data and
  11.6 ms on the staircase with the route off, of which about 2.8 ms is relocation's scan before it bails (traced; the data is then
  certified uniform and takes the one-multiply grid). The route on is 9.0 ms. So the partition proper is about 8.8: a tie.
- **The break-even, wrong in kind.** I predicted the route would stop paying at 14 threads somewhere between 60% and 100% of the array
  inside crossings, and set the gate to J5's n/2 on that. With the gate lifted the two-strand ladder never breaks even against the
  shipped engine: every element crossing is 2.62x at one thread and 1.52x at 14, three live strands 2.44x and 1.12x; it loses from
  3-4 live strands up (1.96n: 2.48x and 0.82x; 4 fully interleaved runs 0.96x and 0.82x; 64 fully interleaved 0.79x and 0.77x). The
  ledger's verdict on merging was about putting every element through a P-way merge; a two-way merge of what crosses, straight from
  the source, is a different operation.
- **"Exactly 1.00 past the gate", wrong, and the costly kind of wrong:** a decline comes after the whole array has been scanned for
  runs, so under the n/2 gate the 60-100% staircases read 0.92-0.96 at one thread and 0.91-1.00 at 14. The cautious gate did not
  merely forgo wins, it charged for them.

### Three defects of my own, each found by measuring rather than by reading
(Two more turned up a day later, both at sizes no bench here reached: the entry above this one. The route read its
switches with `getenv` on every call, and its cost gate let the plan run on runs of two or three elements.)
1. **The sweep cut every endpoint below its value.** A strand's last element then lands at the head of the NEXT interval, where it
   turns the whole middle of the neighbouring strand into a two-strand crossing. The `-DLOGOS_TRACE` build (`make knottrace`) showed
   98.5% crossing mass on a staircase built with 10% before anything was timed. A first value cuts below itself, a last value above.
2. **The n/2 gate** (above). Gates now come from measurement: 1.0n for parallel sorts (the pure-partition table), 3.0n for sequential
   sorts of more than `MERGE_MAX_RUNS` runs (the fallback is the sequential leaf; 8 live strands at 2.95n still 1.3-1.4x), 1.0n for
   sequential sorts of at most that many (the fallback is the run merge, which does the same merges without the staging copy). And
   the sample gate now judges up to 128 sample runs, not 16, but only to DECLINE and only when its estimate clears the gate by a
   tenth: a per-thread prefix cannot see a shuffled staircase, because the few blocks one thread holds rarely neighbour each other
   in value.
3. **The thread cap was inherited from copying.** Relocation runs one thread per 500k elements, measured on pure copies, which are
   bandwidth-bound. A merge is compute-bound. At 1M the first cut placed on 2 threads what the partition it displaced sorted on 10, and
   the heavily crossed rows lost there (x60 0.99, x80 0.84, x100 0.97, triple 0.94) while winning 1.2-1.5x at 8.4M where the cap does
   not bind. The cap now counts work, n + 3 x cost. Predicted before the re-run 1.05-1.3; measured x60 1.42, x80 1.26, x100 1.43.

### Found on the way and fixed: an infinity crashed the parallel sort
- The knot harness's infinite-endpoint shape segfaulted with the route OFF. Reduced: uniform random doubles, `a[0] = -INFINITY`, 4
  threads, **SEGV in `hf_scatter` on the committed engine**. Position 0 is always a sample point (Weyl i = 0), so it is deterministic.
  A sweep of ten shapes with an infinity at a sample point crashed eight of them on the committed engine; no test had ever put an
  infinity in front of a parallel sort.
- One cause in four places: a value-linear map over an infinite range computes `inf * 0 = NaN`, and NaN fails every compare.
  (a) The uniformity certificate: every F is NaN, the distance stays 0, the data is CERTIFIED uniform, and the grid lookup casts NaN
  to a bucket index (the wild write). (b) `bt_build_v` and `bt_build_m` checked the scale with `isfinite`, and `cells / inf = 0` is
  finite: the partition came back with its buckets out of order. (c) Flash and coarse buckets in the leaf cast NaN to a class: the bucket
  holding the infinity came back unsorted. (d) The per-mode certificate has the same blind loop; the lattice gate cast an infinite
  quotient to `long`; the skew probe cast NaN to `long` (harmless on x86, undefined anyway).
- Fixes, all off the per-element paths: the certificates require `isfinite(hi - lo)` (which also covers two finite values whose
  difference overflows); a scale must be finite AND positive; the dispatcher sends a non-finite range to the exponent-bit coarse path,
  which keys on the bit pattern, or to quicksort; the lattice gate declines on a non-finite extreme or a quotient past 2^53; the skew
  probe reports maximal skew.
- Verified under `-fsanitize=address,undefined,float-cast-overflow` (the last is not part of `undefined` in GCC, and it is what found
  (b)-(d)): 10 shapes x 8 placements x 3 sizes x 3 thread counts, 720 checks, no wrong output and no report. 90 of them are now in the
  harness (`infinity_checks`).
- Left alone, on purpose: with a NaN in the data flash and coarse still cast NaN to an index (clamped, memory-safe; NaN ordering is
  documented as unspecified), and the lattice worker's per-element cast is still undefined for an infinity the sample missed (x86
  yields the indefinite value, the tolerance test then aborts the route, which is correct). Both are in hot loops that would need an A/B.

### Also found and fixed: `-DLOGOS_NO_RELOCATE` did not compile
- The counting routes' thread-cap helper sat inside the relocation `#ifndef`, so the pre-J5 engine that `make j5bench` builds failed
  with `COUNT_MIN_THREADS undeclared`. Moved above it. `-DLOGOS_NO_RELOCATE` now implies `-DLOGOS_NO_KNOT`.

### Whole change against the committed engine
- The guards and the new gates sit in setup code, but a claim of "free" is still a claim. `git show HEAD:engines/c/logos_psort.c` as
  one copy, the working tree as the other, `ledgerbench`'s five shapes, both orientations, 8M, 24 pairs: **0.987-1.021** at 1 and 14
  threads on uniform, heavy_tail, bimodal, clustered and int_wide, inside the floor. Layout factors 0.99-1.03.

### Harness
- 401 knot checks (`knot_checks`): staircases in order and shuffled, few wide crossings, three strands live, descending blocks in order
  and shuffled, equal keys ACROSS strands, runs of equal keys whose cut values repeat, a crossing that sits on zero and holds -0.0 and
  +0.0 in both strands, infinite endpoints with +inf tied across strands, a sparse strand over dense blocks, strands of 64 (the
  run-length bail), two strands with every element crossing (40/60, so the prefix-plus-tail route does not take it first), and a NaN
  (no plan: the answer must be bit-identical to a run with the route off). Sizes 1000 to 3M, 1, 4 and 8 threads, each under the
  default gate and with the gate lifted so the placements the default gate declines are exercised too. Clean under ASan and UBSan.
  90 infinity checks. **2059 checks.**

### Not done
- Crossings of three or more strands still take the staged copy plus J5's in-place merge tournament; a direct three- or four-way
  merge from the source is the obvious way to move the break-even, and the only thing likely to make the parallel form MORE than a tie.
- The copy back. The route reads `a[]` twice and writes twice (scan, place, copy back), as J5 does. At 14 threads that traffic is
  the whole cost, which is why a route that sorts nothing only ties a partition that sorts everything.
- A sparse strand lying over dense blocks makes every block it touches a two-strand crossing, so the cost gate prices the whole
  block although a galloping merge would touch a few elements. Cutting at the sparse strand's own elements is the same sweep again.
- At 14 threads the near-sorted route is asked before relocation, so an in-order staircase never reaches this route there.

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
