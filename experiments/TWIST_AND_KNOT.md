# Twist and knot: round four (2026-09-21)

Asked: what other avenues open up where the Moebius twist (a strand carries its orientation and is never reversed) meets the
unknotting view (runs are strands, comparisons happen only where strands cross)? Nothing here is built. Every item has a
mechanism, a place in the code, a prediction written before any measurement, and the ceiling to measure first.

## Status, 2026-09-21 (after the round that acted on this file; numbers in engines/c/CHANGELOG.md and FINDINGS §3ah)

| avenue | verdict |
|---|---|
| 3b, vectorise the scan | **BUILT, won**: 1.43-1.49x at one thread on blocks, 1.16-1.22 at 14. The run routes beat the pure partition at 14 threads again (1.08, 1.17) |
| the whole array's bit (open direction 0k, from the probe before this file) | **BUILT, won**: mirrored short reach 2.07x, mirrored prefix 2.20x, mirrored streams 2.76-2.81x at one thread; guards 0.98-1.02 |
| 5, writhe | **measured, not wired in**: persistence classes 16 of 16 shapes as predicted; the inversion rate reads folds as noise |
| 2, strays inside strands | **ceiling measured, not built**: 2.0-2.2x at 1% strays through a wasteful told sort, about 3x real. The next build |
| kinks (Reidemeister move I, the cheap subset of 2; from an outside write-up) | **BUILT, won**: 5.8x / 5.0x / 3.2x at 0.1 / 1 / 4.8% kinks at one thread, 1.3-1.5x at 14 up to 1%; guards 0.98-1.01 |
| 3a, plan from the sample and verify while placing | not started; 3b took the cheaper half of the same prize |
| 1 ribbons, 4 oriented k-way merges, 6 descending output, 7 int32 twin | not started |

## What the two rounds established (the constraints any new idea has to meet)

1. An orientation bit pays where the consumer COPIES ANYWAY, in proportion to how cheap the route already is (a swap pass is
   a sixth of a 2.3 ns/element route, a thirtieth of a 16 ns one), and it ties where a merge is the whole sort.
2. At 14 threads the run routes TIE the partition (0.92-1.11): scan + place + copy back is the same traffic as a scatter.
   Only a dropped pass changes that (open direction 0f).
3. The twist made the run detector READ-ONLY. A declined or failed attempt now leaves the input intact. Nothing has used that yet.
4. Every cheap route reads order in one direction, and real disorder is never clean: runs carry noise, strays, late records.
   The run detector bails on the first of those, and the array pays for a full sort.

## The avenues, ranked by the evidence already in hand

### 1. Ribbons: a strand with WIDTH (unifies short reach, the twist and the knot sweep)
A Moebius strip is a ribbon, not a line. A ribbon here is a monotone trend, ascending or descending, with disorder of short
reach around it. Today a strand must be exactly monotone (one noisy element ends it) and the short-reach route accepts exactly
one ascending ribbon spanning the whole array. Generalise: find ribbons from the SMOOTHED sample, sort each where it lies with
the short-reach block kernel (a descending ribbon classifies mirrored), then hand ribbon descriptors, orientation included, to
the knot sweep. One descending ribbon is `mirrorprobe`'s case (G); sixty-five alternating ribbons is the sloped sine wave (0i (f)).
- Ceilings already measured: 2.0-2.1x (mirrored short reach), 2.7x (mirrored prefix), 2.1-3.7x (the sine wave), all at one thread.
- Prediction: 1.8-3x at one thread on those shapes, 1.1-1.4x at 14, guards 0.98-1.02 if the gate is avenue 5.
- Where: `reach_try_f64`'s gate and block phase, `runs_plan` / `knot_plan` taking descriptors that are not exact runs.

### 2. The unknotting number: strays inside strands
A knot's unknotting number is how few crossing changes untie it. The sorting analogue is how few elements must be LIFTED OUT so
that what remains is a handful of clean strands. The near-sorted route does this for ONE strand (keep/reject with poison
recovery). The run detector does not: block-permuted data with 1% late records, or a staircase with outliers, breaks into runs
of fifty, the average-run bail fires, and relocation never sees it. Put the keep/reject inside the detector: an element that
breaks the run while its successor resumes it is a stray, set aside, sorted once, merged back as one sparse strand (the
galloping merge 0f already asks for).
- Ceiling: NOT measured. `make chaoshunt`'s told-the-structure method on block_perm and stairs with 0.1%, 1%, 5% strays.
- Prediction: 3-4x at one thread at 1% strays (16 ns/element falls to 4-5), 1.0-1.3x at 14, fading to nothing near 5%.
- Where: `reloc_detect_worker_body`; the stray list is the near-sorted route's.

### 3. Spend the read-only detector: drop a pass
Constraint 3 is an unspent asset. Two ways to spend it.
(a) **Plan from the sample, verify while placing.** Draw the diagram (strands, orientations, cuts) from 2,048 points, find each
boundary by bisecting on VALUE between the two sample points that straddle it, then copy every piece into `dst` while checking
monotonicity in the same loop. A violation abandons the attempt, and because nothing wrote to `a[]` the fallback costs time only.
That removes the full detection scan, which is the pass constraint 2 says must go.
(b) **Vectorise the scan.** With no interleaved writes the detector is a compare of neighbours: eight doubles a step, a mask,
scalar work only at candidate breaks.
- Ceiling first: a `-DLOGOS_PROFILE` build's RELOC_PROBE share of the run routes. A trace build suggested about half at one
  thread, and a trace build times nothing reliably, so that number is a reason to measure, not a result.
- Prediction if the share is 35-50%: (a) 1.15-1.3x at 14 threads on block shapes, turning the tie into a win, 1.2-1.4x at one;
  (b) about half of that. If the share is under 20%, stop.

### 4. Reidemeister III: reorder the merges, and merge crossings of three or more straight from the source
The third Reidemeister move slides a strand across a crossing without changing the knot. In the plan that is freedom of merge
ORDER. Today a crossing of three or more strands is staged (every piece copied into `dst`) and then folded by `merge_all_runs`:
one copy pass plus log k merge levels. With orientation carried per strand, a direct oriented 3- and 4-way merge reads the
pieces where they lie, in either direction, and writes once.
- Evidence: three live strands read 0.89 against the pure partition at 14 threads, which is why the parallel gate stops at two.
- Prediction: 0.89 moves to 1.0-1.1 at 14 threads and the gate can open to three; 2.6x at one thread moves to about 3x.
- Where: `knot_worker` phase B, `knot_place_seq`; a `knot_merge3x` / `knot_merge4x` in the mould of `merge_4way_hybrid`.

### 5. Writhe: a signed invariant the sample can compute for free (the gate for 1, 2 and the whole-array bit)
A knot's writhe is its SIGNED crossing count. `inv_all` is an unsigned, global version: 0 ascending, 1 mirrored, 0.5 for noise
AND for a balanced pattern of folds, which is why it cannot gate avenue 1 (a triangle wave reads 0.5) and misreads mirrored
blocks (0.90). Compute the inverted share per WINDOW of the sorted-by-position sample and look at two numbers: the mean of
|2 inv - 1| (orientability: near 1 for ribbons of either direction, near 0 for noise) and the number of sign changes (how many
folds). Both cost nothing beyond the sample.
- Ceiling: none needed, it is a classifier. Score it on `chaoshunt`'s zoo plus the mirrors, rule fixed in advance, as `zroute` was.
- Prediction: separates noise from folds on every zoo shape; mirrored blocks and mirrored short reach land in different cells.

### 6. The non-orientable output: descending order for free
The bit can be carried out of the sort as well as into it. ORDER BY ... DESC is sort-then-reverse today. Every route ends in a
placement or a copy back, so a descending entry can write bucket b where bucket B-1-b would go and have the leaf emit mirrored,
at no pass at all. The sorted view gets it trivially (`at(n-1-i)`), and `argSort` gets a descending permutation.
- Ceiling: the twist bench's own numbers. A reversal pass is 7-17% of a cheap route and about 3% of a random sort.
- Prediction: 1.03 on random data, 1.07-1.17 on structured data, against sort-then-reverse. Small, but it is an API users ask for.

### 7. The int32 twin, born twisted
The int32 path has no relocation, no knot route and no short reach (0b, 0f, 0i). Its twin can carry the orientation bit from its
first line, and a type fact helps: integer strands tie often, and ties break a strictly descending run, so the descending test
wants to be non-strict with stability waived, which integers allow and doubles with signed zeros do not.
- Prediction: what the f64 routes measured, 2.3-4.4x at one thread on block shapes, a tie at 14.

## Considered and set aside
- **The leaf's detector.** Measured: ceiling 1.02-1.06 (`make mirrorprobe`). Closed.
- **Alternating-direction merge trees** (write every other merged run descending so that the next two-ended merge starts from
  adjacent ends). It changes locality, not work. Three measurements say the memory system is not the bound. Predicted neutral.
- **The ring's seam** (a rotated sorted array as one strand on a ring). Relocation already places two disjoint runs by copy,
  twisted or not. Nothing left to collect.

## Suggested order
5 first (free, and it gates the rest), then the ceiling for 2 and the profile for 3, since both are one afternoon and either
could be a stop. 1 is the largest prize and the largest build. 4 and 6 are small and independent. 7 last.
