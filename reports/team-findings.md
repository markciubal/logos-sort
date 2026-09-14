# Overnight team round — findings

Seven agents, each in an isolated copy of the tree, every benchmark serialised through a
machine-wide lock so they could not corrupt each other's timings. Two claimed wins were handed to
independent skeptics instructed to refute them. Everything below is measured; the raw write-ups are
in `experiments/team/` and the full patches, including for everything that did not land, in
`experiments/team/patches/`.

**Bottom line.** Two wins landed and a third was found while landing them. Three ideas were measured
and rejected, and two of those rejections are more valuable than a win would have been because they
close whole families of future work. One agent produced a map of where order is actually left on the
table, and it points somewhere nobody was working.

The engine passes 1249 correctness checks, up from 835.

---

## What landed

### 1. The int32 sentinel window and the span band

Two defects with a known fix, both now fixed.

The sequential int32 leaf gated its counting sort on raw minimum and maximum, so a single
`INT32_MIN` among a bulk of 0..99 cost about 2.5x. It now takes its table window from the sample's
extremes and spills the tails, the same shape the parallel side already used. Separately, the fixed
262144-slot cap became a 64 MB table budget with a persistent table pool, which repairs the span band
where fourteen threads lost to one core.

| 8M unless noted | builder | skeptic, independently |
|---|---|---|
| one sentinel, 1 thread | 2.4–3.2x | 2.73x at 8M, 3.33x at 1M |
| sentinel row over clean row | not measured | **1.09–1.16 on, 2.4–5.6 off** |
| span 1e6, 14 threads | P/1 0.60 → 2.7 | paired 2.66x |

The skeptic measured the target's literal metric that the builder never did, which is the second row,
and it is the one that matters: with the window on, a sentinel costs almost nothing.

A third thing fell out: the inherited `span <= n` gate was **2x wrong at its top edge** and is now
`n/4`. A count array of n slots is random access into 32+ MB and loses to the radix well before the
old gate gave up.

### 2. Batch queries on the sorted view

`logos_view_quantiles` and `logos_view_ranks` answer m cuts in one pass, and the int32 twin of the
whole view API came with them. Measured at 8M on fourteen threads with both routes in one binary:

| | builder | skeptic |
|---|---|---|
| over sort-then-index | 1.29–1.67x | **1.22–1.63x** |
| over m single lookups | 1.17–1.40x | **1.19–1.56x** |

The engine-core edits are neutral pristine-versus-patched with a copy swap, 0.97–1.06 across nine
rows, so the batch API costs the ordinary sort nothing.

The int32 view is near-neutral at 1.03–1.15x, and the builder explained why rather than burying it:
the int32 leaf is only 7–15% of an int32 sort, so laziness has almost nothing to defer. **The lever
on the integer side is its pre-H0b histogram partition, not the view.**

### 3. The sequential-leaf policy for batch queries

The skeptic caught the batch route silently trading away the vectorized sequential leaf: the bucket
sorter derived its sequential flag from the thread count, so a ten-bucket batch on ten threads took
the scalar parallel leaf instead. The flag is now set when each thread will sort at most one bucket,
which is exactly the batch regime and exactly when a bucket is a lone sequential sort. Full
materialisation is unchanged by construction, so the ledger's measured "vectorized leaf in the
parallel phase loses 5–28%" still governs there.

Measured in one binary, 40 pairs, floor 5.7% that day:

| | old | new |
|---|---|---|
| sawtooth, sort+index / batch | 2.576x | **3.079x** |
| sawtooth, 10x lookup / batch | 1.199x | **1.374x** |
| few_distinct, 10x lookup / batch | 0.877x | **1.033x** |
| uniform, 10x lookup / batch | 1.391x | 1.335x (inside the floor) |

**One caveat, stated because it limits the claim.** The skeptic's specific regression did not
reproduce. They measured the batch at 0.674 on sawtooth, meaning 1.4–1.6x slower than the loop; on
the landed tree with this generator the old policy already read 1.199. Their diagnosis was right and
the fix helps every shape it touches, but the size of what it repairs depends on the generator and is
unresolved.

---

## What was measured and rejected

### Curvature-placed CDF knots — neutral, and it closes a family

A Douglas-Peucker fit on the sample's rank-versus-key curve, placing knots where the CDF bends rather
than by position or by count. It ran twice: the first pass claimed 1.13x on heavy-tailed data and the
second, more careful pass retracted it. Net about 3–4% at fourteen threads, below the floor.

The mechanism is the valuable part. The map's lookup genuinely is cheaper, 0.218 against 0.655 ms per
49k elements on the trial, but it buys a **worse** partition: worst bucket 1.68x the mean against the
existing table's 1.45x. No tolerance setting fixes that, and the reason is structural. **The splitter
table is built from the same sample the approximation is fitted to, so it already has zero in-sample
error, and any cheaper map can only trade balance for lookup cost.**

That is the third failed attempt at a fancier bucket lookup. Taken together they say the lookup is
not where the partition's time is, and the next person should change what is being approximated
rather than how well.

### An atomic chunk queue for the counting routes — neutral, and it redirects the target

Dynamic chunks, a single spawn and parallel merges in the lattice, lowcard and modcard routes.
Inside the 7.7% floor at fourteen threads on both target shapes, 1.03 and 0.97.

It wins about 1.2x in exactly three places and they share a cause: a 30k-slot lattice at fourteen
threads, whose serial slot merge walks 65k slots times 14 tables, and both shapes at seven threads.
**So the static per-thread ranges were never the cost. The serial phase is.**

### Relocation by page table — negative, for the cost of a printf

The census settled it exactly as the plan predicted it might. On the block-permuted shapes **not one
byte can move by page table**: the block size in bytes is not a page multiple, so only the
permutation's fixed points align, 0–1.6%, and those already sit in the right place.

A prototype for the case where aligned runs do exist is correct and wins 1.3–2.0x, but only when the
caller hands over a 2 MB-aligned array whose blocks are 2 MB multiples. On ordinary allocated arrays
it loses, 0.63–0.77 at fourteen threads. One page move costs 10–30 microseconds regardless of size
and 2.5–8x more with thirteen threads live in the address space, and the moves fragment it badly
enough that a second sort of the same buffer faults.

---

## The map: where order is actually left on the table

Forty shapes drawn from real systems, each raced against a sort that was told the structure, all
outputs bit-exact. This agent changed no engine code.

**The gap is overwhelmingly a single-thread dispatcher problem.** At one thread, against an informed
route: shard IDs 13–15x, Gray code 12–13x, hash order 8.6–9.4x, a sorted prefix with an appended tail
6.7–9.3x.

**At fourteen threads the partition is already within 1.0–1.4x of the ceiling on everything except
one route.** The modcard hash-count route is 3–7x off on lattices of 50–65k distinct values, with
workers busy only 14–38 ms of an 86–105 ms wall. On nine shapes the engine already beats the informed
route.

One specific finding worth its own line: on Zipf-distributed IDs at fourteen threads, **30.5 of 68.9
ms goes into a modcard attempt that then aborts.** The gate is wrong before the merge is even
reached.

---

## Not finished

The strided-order probe, for data sorted under a stride such as a column-major read of a sorted array
or a round-robin merge of k shards, has working code and a written prediction in
`experiments/team/strided-probe.md` and its patch, and **zero measurements**. The agent died before
benchmarking. The prediction stands on record, untested, which is the cheapest possible place for
someone to pick it up.

---

## What to do next, ranked

1. **The modcard and lattice serial tail.** The best-evidenced target in the project and it was named
   independently by two agents who never spoke. Two concrete pieces: split the slot merge across
   threads the way the int32 counting route already does, and fix the gate that spends 30 ms deciding
   to abort. Neither was anyone's assignment last night.
2. **The single-thread route selection.** Every large multiple on the map lives here, 4–15x, while
   the parallel path is nearly optimal. Nobody has worked it.
3. **Measure the strided probe.** The code exists; it needs a night on the bench, not a design.
4. **Stop trying to make the bucket lookup cheaper.** Three failures with one shared cause, now
   written into the ledger.
