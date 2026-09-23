# Hard unknots: round five (2026-09-21)

Asked: implement the unknotting problem, using Kauffman and Lambropoulou's "Hard Unknots and Collapsing Tangles"
(homepages.math.uic.edu/~kauffman/IntellUnKnot.pdf) as the guide. Focus on Reidemeister moves acting on the doubly linked
Moebius structure, where walking forward past the end loops back to the beginning.

Numbers: engines/c/CHANGELOG.md (the entry of the same date) and FINDINGS §3al. Probe: `engines/c/unknotprobe.c`.

## Status

| item | verdict |
|---|---|
| the unknotter: a circular doubly linked list, R1/R2/R3 as local rewrites, swings, tangle collapse | **built as a probe, a measured dead end as a sort**: slower than the engine on 15 rows of 16 at one thread (0.009 on noise, 0.20-0.37 on the structured shapes) |
| R0, the loop-back: a ring buffer is one strand across the wrap | **BUILT, won**: 1.56-1.63x at one thread, 1.90-2.03x at 14 on a ring buffer; 1.46-1.75x on a rotated sorted prefix with a random tail; guards 0.97-1.02; no cost per call at the small end |
| the relocation detector's floor, found by the probe's worst row | **BUILT, won at one thread**: blocks of 256-4095 elements 3.6-7.6x; at 14 threads every lower floor was mixed, so it stays 4096 there |
| forgiving a detection chunk its partial first run | **measured, not kept**: 1.00 everywhere; the per-thread run cap binds first |
| a seam finder for NOISY ring buffers (a ring buffer of records with late arrivals) | **measured, not built**: a told rotation recovers 1.17x at one thread, under the stop rule of 1.25 |

## The dictionary

| the paper | the sort |
|---|---|
| a knot diagram | the array closed into a ring: a circular doubly linked list, walking forward past the end arrives at the start |
| the unknot | a ring that reads as ONE ascending strand from some node (a sorted array, or a rotation of one) |
| an arc of the diagram | a strand: a maximal monotone arc of the ring. It carries the twist's orientation bit, so a strand that descends in ring order is read through its prev links (forward in value = backward in the ring on a twisted strand) |
| a crossing | two strands whose value intervals overlap; sorting needs comparisons there and nowhere else |
| crossing number C(K) | nodes a merge must walk: the overlaps (the probe reports it per element) |
| R0, planar isotopy on the sphere | rotating the ring. The paper's diagrams live on the sphere, where the outer region is a region like any other, and some diagrams hard in the plane are easy there. A sorted array read from the wrong slot is two runs in the plane and the unknot on the sphere |
| R1, a kink | an adjacent pair swapped inside a strand that otherwise goes on (the engine's kink repair, round four) |
| R2, a bigon | two ring neighbours that cross, pulled apart by a merge touching only the overlap: the lower strand walked DOWN from its top, the upper one up from its bottom. That is what the second link is for |
| R3, a triangle | a strand that crosses neither neighbour slides past one, relinked in O(1), so two crossing strands become neighbours |
| a hard unknot | no simplifying move, yet not the unknot: no two ring neighbours cross while strands further apart do |
| a swing (the Culprit's long arc swung out) | a cut at a gap: a step far above the array's typical step. One strand more, every false crossing gone: the diagram gets more complex before it gets simpler |
| a collapsing tangle | the global reading: strands by value fall into maximal overlapping groups, each slid together and collapsed as a whole |

## What the unknotter taught

1. **Local moves stall only where crossing strands are far apart on the ring.** In sixteen shapes the paper's hard state appeared
   once, on the shuffled staircase. Every other stall was the size rule (a long strand next to a short one waits, so that a list
   never pays a full walk to insert one small strand).
2. **The Culprit is real and it is the whole cost.** A strand with one far element has an interval spanning most of the range, so
   every local move against it walks its whole partner: rotated short reach walked 72M nodes for a 1M array before swings.
   Cutting at gaps took nodes walked per element from 21.5 to 0.00 on near_sorted and from 4.0 to 0 on block_perm.
3. **The loop-back buys nothing inside the unknotter** (plane against sphere 0.96-1.09), because its global reading, strands
   sorted by value, is rotation-blind already. The engine's cheap routes are not: they read order from index 0.
4. **As a sort it loses for the reason the literal list lost**: a node written and walked per element before any move. The paper
   says local moves lack a global view; the engine's knot route already is one (a sweep over all strand endpoints).

## What the engine got

- **The loop-back.** The prescan has walked the ascending prefix [0, d). If the array also ends in an ascending run [s, n) whose
  last element is no larger than a[0], that run continues into the prefix across the wrap: it is rotated to the front once. When
  the suffix began at the first descent, the array is sorted and the sort returns; otherwise the tail route takes the remainder
  directly. Its first cut re-entered the dispatcher blind and measured 0.89: a ring buffer's sort is three or four passes, and
  re-reading it was the whole margin. The second cut hinted the re-entry's prescan and won the A/B, and `make callbench` then
  charged it 10-16% on every sorted call of 2 to 32 elements: the prescan loop's start had become a variable. Third cut: no hint.
- **The floor.** The probe's worst row (rotated keyed runs, 5.8x) was not about rotation: the run detector gave up at an average
  run under 4096, the run merge's constant, so at one thread every array of disjoint blocks of 256-4095 elements was sorted in
  full. It is now 256 at one thread.

## Still owed

1. **Noisy ring buffers.** Rotated short reach: 1.29x ceiling, 1.17x with a told rotation, at one thread. The cheap seam finder:
   the sample's inverted Fibonacci-lag pairs cluster at the seam, so their position span brackets it for free.
2. **A parallel rotation.** The loop-back rotates on one thread; at 14 threads it still wins 1.9x on a ring buffer, but a
   remainder over n/16 is not offered there (the first cut lost 0.51 on 30% prefix + noise + 40% suffix at 1M).
3. **The parallel tail route splits the PREFIX evenly**, so a tail lying under one prefix piece lands on one thread. Split the
   output by merge path instead.
4. **The sequential tail route moves prefix elements one at a time above the tail's maximum.** Gallop the top: one memmove.
5. **The per-thread run cap at P threads** is a knife edge for runs of exactly the floor (148 runs in a chunk with room for 148).
6. **Leads, not traced**: at 14 threads the in-order staircase sorts 2x SLOWER than the same staircase rotated; at one thread keyed
   runs of 8192 cost 7 ns per element where runs of 4096 cost 2.
7. **The JS and Rust engines** have neither change.

## Round six (2026-09-22): six more ideas, built and paired

Asked where the slowdowns still are and what else the knot view offers. Each idea got a written prediction and an A/B in one binary
against its own switch, two runs of 40 pairs at 8.4M. Numbers: engines/c/CHANGELOG.md and FINDINGS §3am.

| idea | where it came from | verdict |
|---|---|---|
| strays inside strands: lift them in the detector, merge them back in place of the copy-back | the unknotting number; open direction 0l (a) | **SHIPPED**: 4.0x at 0.1%, 3.0-3.3x at 1%, one thread |
| the wave: pieces of the position-ordered sample, sorted where they lie, placed by the knot sweep | back-solving the curve from the sample | **SHIPPED**: 1.11-1.24x at one thread where the crossing cost is low; a swing of two periods lost 0.76 before that gate |
| the seam: a noisy ring buffer read in the reach route's block medians | the footrule's mode, and 0m (a) | **SHIPPED**: 1.12-1.16x at one thread |
| a Weyl pivot for the AVX2 quicksort | the sampler's own anti-aliasing | reverted: 1.00 on every row |
| direct three- and four-way merges for crossings | bridge number | reverted: 0.52-0.68 |
| a sorted DESCRIPTION instead of a copied array | Reidemeister's isotopy class: nobody redraws the diagram | measured as a ceiling: 1.21-1.35x on block shapes |
| the LIS of the sample as the unknotting number | knot theory's unknotting number | measured: separates one strand with strays from many; not wired |

The Culprit turned up again, in the detector this time: a run that STARTS with a stray below it has no body for the gap test, so
its interval reached down to the stray and the chain gate read the whole array as one tangle. The swing at a run's head fixed it.

## The timestamp round (2026-09-22): a write-up's theory against server-shaped data

An outside write-up explained the Moebius hybrid by topology and information theory: the O(1) boundary check proves a partition has
"zero cross-boundary entropy", so on macroscopically ascending, locally noisy data (sensor telemetry, log merging) the merge phase is
skipped and the sort is O(N). `make tsbench` built the hybrid as described and ran it on thirteen timestamp shapes. The check skipped
0% of merges on every shape with network latency, because latency spreads farther than the recursion's 32-element blocks; it skipped
100% on sorted data and adjacent swaps. The entropy framing is right in spirit and wrong in amount: the floor for displacement d is
about log2(d) comparisons per element, and the engine's short-reach route is built on exactly that. The applications are right, and
each already has a route. The round found two gaps in the engine and closed them: one sorted strand with kinks (0.90 -> 4.95x over
std::sort; the hybrid still leads there 1.6x) and the seam at 14 threads (0.74 -> 0.94).

## The edge round (2026-09-22): blocks sorted by their edges

A second proposal from the same line of thought: condense the array into strands, sort the strands by their min edge, lay them out in
that order and let the boundary-bypass merge finish "the sine wave". In knot terms it treats every strand as a rigid arc and sorts the
arcs by one endpoint, which is sound only when the arcs do not cross; where they cross, only a sweep that cuts them at the crossings
(the knot route) places each element once. Measured on sixteen timestamp shapes it lost to the engine 1.4-9.9x on 13 and slowed the
hybrid on 15. The arcs were not rigid either: a maximal run swallows the next block's first element, and a dropout pairs with a real
reading, so an arc's endpoint can belong to another arc. Its three wins were three engine defects (the prescan, a stray finish in
three passes, ties ending a descending run), all fixed and measured: 2.2x, 2.4x, 6.0x on those rows.

## The weld round (2026-09-22): blocks welded in place

A third proposal from the same line: condense the strands, sort them by min edge, move them into that order in place by triple
reversals ((A^R B^R)^R = BA, a rotation in O(1) space), and weld each seam left to right, skipping it when the left max is at most
the right min. In knot terms the rotations are a legal isotopy (they move arcs without cutting them) and the weld is R2 applied
arc by arc against everything already welded, which is where it fails: once one arc reaches the top of the range, every later
crossing is untied against the whole prefix. Measured on sixteen timestamp shapes it lost to the engine by 2x to 4,600x on fourteen,
and its boundary test bypassed no seam at all: the strands it cut were not arcs (a maximal run spans the hole between two blocks).
Its two wins were two engine defects (the prescan's scalar compare chain, the run detector's hold after a wrong-way step), both
fixed: sorted input 1.7-2.2x, kinks 1.05-1.08x.

## The handoff round (2026-09-22): stitching the strands where they lie

A specification of "what remains unimplemented" in a Moebius hybrid: bail out on short runs, swap 2-3 element dips inline, and stitch
the strands with a galloping merge behind an O(1) boundary check. In knot terms it drops round three's meta-sort and keeps the strands
in place order, which is the right instinct: position order is what makes a merge balanced. Measured, it is behind the engine on 14 of
16 shapes, and its specified Reidemeister I move never fires, because a dip's first element belongs to the preceding ascending strand
(the engine's own repair asks both neighbours and swaps the pair back inside the strand). Its one win was an engine defect: the
detector's block of eight was scalar compares, and with vector ones a kinked strand sorts 1.30-1.32x faster at one thread.
