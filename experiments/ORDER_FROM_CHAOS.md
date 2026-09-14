# Order from chaos: experiments to run next

A brainstorm in the style of TRUTH_TABLE section J: nothing here is measured.
Every item names the mechanism, where it plugs into the engine as it stands
(AVX2 sequential leaf, H0b splitter tables, 12 buckets per thread, J5
relocation), a prediction to be graded, and the bench that would grade it.
The ledger is respected: the golden-ratio *pivot* (C10, 6% slower) is not
re-run; every golden-ratio idea below is a different lever.

The name. *Logos* is Heraclitus's word for the order hidden inside flux, and
John's for the Word that was "in the beginning". Genesis 1 is a sorting
algorithm: light divided from darkness, waters above from waters below, land
from sea, each kind after its kind. Separate first, then separate again, then
name. That is a sample sort. The ideas below take the metaphor literally
where it yields a mechanism and drop it where it does not.

## 1. The golden ratio inside the algorithm

**1.1 Weyl sampling stride (low-discrepancy sample positions).** The
splitter sample, the 40-point inversion probe and the 64-point gap probe all
read the input at a fixed stride. A fixed stride aliases with periodic input:
a sawtooth whose period equals the stride puts every sample at the same
phase, the splitters collapse, and the parallel partition degenerates to one
bucket. Positions `floor(frac(i * phi) * n)` (Kronecker/Weyl sequence) are
equidistributed for every period at once; the golden ratio is the "most
irrational" number (continued fraction of all ones), so its sequence has the
lowest discrepancy of any rotation (three-gap theorem). *Where:* the three
samplers. *Prediction:* no change on any current shape; on a constructed
"stride-aliased sawtooth" (n = 8M, period = the sample stride) the current
engine falls to one bucket and takes 3-5x its uniform time, the Weyl sampler
holds it within 10%. *Bench:* new shape in stbench plus a sampler A/B.
Cost: an afternoon. This is the cheapest genuinely new golden-ratio lever.

**1.2 Incommensurate class widths (Pythagoras against resonance).** Flash and
coarse use equi-width classes of width `range / K`. When the data lives on a
lattice of step `s` (timestamps in ms with jitter, prices in cents plus fees)
and `range / K` is near a multiple of `s`, classes alternately hold two
lattice points and one: 2x imbalance, and the insertion fix-up pays for it.
`quant_probe` routes exact lattices to the radix path, but jittered lattices
(90% on-lattice, 10% off) pass through to interpolation. Choosing the class
count so that `range / K = s * (m + 1/phi)` makes the lattice points fall
into classes as evenly as an irrational rotation allows (at most three
distinct class populations, by the three-distance theorem). *Where:* flash
and coarse class count, after a cheap lattice-step estimate from the
existing quantization probe. *Prediction:* jittered-lattice shapes 1.2-1.4x
in the leaf; uniform unchanged. *Bench:* leafbench with two new shapes
(`jittered_ms`, `cents_plus_fee`). Cost: a day.

**1.3 Guided chunk schedule with golden decay.** Histogram and scatter pull
fixed 65536-element chunks; the leaf pulls whole buckets. OpenMP's guided
schedule shrinks chunks geometrically so the tail balances. A schedule where
each chunk is `remaining / (P * phi)` keeps early chunks large (low overhead)
and late chunks small. *Prediction:* the profile's phase imbalance (1.05-1.13
today) drops below 1.03, worth 2-4% of wall at 14T on random shapes, nothing
at 1T. *Bench:* profile-full per-thread busy spread before and after. Cost:
half a day. Low ceiling, but it is the one place a golden ratio plausibly
belongs in the scheduler.

**1.4 Fibonacci search as the table fallback.** When a table cell holds
several splitters and no child, `bucket_of` binary-searches them. Fibonacci
search uses only additions and touches memory in a more predictable pattern.
*Prediction:* unmeasurable (that path is under 1% of lookups after H0b).
Listed to close the question; do not build.

## 2. The golden ratio inside the data

**2.1 The most uniform non-random data.** The sequence `frac(i * phi)` is
equidistributed with the smallest possible deviation: every interval of
width `w` holds `w * n` points to within a few. It is the ideal input for
interpolation bucketing: flash's classes each receive exactly one or two
elements and the fix-up pass does nothing. The AVX2 leaf now takes the whole
sequential unstructured leaf, so this data would run a quicksort it does not
need. *Mechanism:* a local-disorder probe on the sample (Devroye's integral
of f squared, estimated as the collision rate of sample values in equi-width
cells) that routes low-discrepancy data to flash and everything else to the
AVX2 leaf. *Prediction:* on an 8M Weyl or van der Corput sequence flash
finishes in ~40 ms (one count, one scatter, no fix-up) against the AVX2
leaf's ~100 and x86-simd-sort's ~80; the probe costs nothing measurable and
never fires on random shapes. *Bench:* simdbench with `weyl`, `vdc` shapes
and a fifth variant with the probe. Cost: a day. This is the one place the
project's thesis (address, don't compare) beats the vectorized quicksort
single-threaded on continuous data.

**2.2 Phyllotaxis and sunflower data as the parallel-partition best case.**
The same sequence at 14 threads gives perfectly equal buckets and a
histogram that resolves at the root for every element. It is the lower
bound for the partition phase on this box; measuring it tells us what the
scatter and histogram cost when nothing is skewed. *Prediction:* histogram
4.5 ms, scatter 3.8 ms, leaf 8 ms at 14T, a wall of ~17 ms, which would be
the engine's best possible 8M time here. *Bench:* profile-full with the
shape added.

## 3. Pivot sorts revisited

**3.1 Network pivot.** The AVX2 leaf picks the median of 3, 15 or 63 strided
samples with an insertion sort. The 64-element bitonic network already in
the binary sorts 64 strided samples in ~120 ns and yields a better median
(the probe showed 17.4 element-levels against the ideal 17.0). *Prediction:*
2-3% on the sequential sort of random shapes, nothing elsewhere. *Bench:*
the AV_STATS probe (levels) and simdbench. Cost: an hour.

**3.2 The sample is the pivot.** At 14 threads each bucket already owns ~32
sorted sample values (the top-level sample is 32 per splitter). Their
median is a free first pivot and, more usefully, the 32 values are
equi-depth class boundaries for a 33-way in-cache scatter that replaces
flash's equi-width classes. This is the lever the leaf-knobs ledger entry
(C17) named and nobody built: equi-depth classes cost nothing when the
sample is already there. *Prediction:* leaf phase -10 to -20% on heavy_tail
and bimodal at 14T (their equi-width classes are the most unbalanced),
neutral on uniform, which puts bimodal at 14T level with x86-simd-sort.
*Bench:* leafbench variant, then profile-full. Cost: two days. **The
highest-value item in this document.**

**3.3 Interpolation pivot.** For the sequential AVX2 leaf, the value midpoint
`(min + max) / 2` is a free pivot that equals the median on uniform data and
turns the quicksort into a radix-like value split. Skewed data would give
unbalanced splits, but the partition already tracks min and max, so the
split ratio is known after each level and the engine can fall back to a
sampled pivot when the ratio leaves 0.3-0.7. *Prediction:* 3-5% on uniform,
int_wide, negatives at 1T; neutral on heavy_tail after the fallback fires.
Cost: half a day.

**3.4 Three-way (dual-pivot) vectorized partition.** Two pivots per pass give
log3 levels instead of log2, at the cost of two compares and a three-way
permute table (81 entries for 4 lanes) per vector. *Prediction:* the extra
per-vector work (about 1.6x) outweighs the 37% fewer levels on a
memory-bound top level and roughly ties in cache: neutral. Listed so the
next person does not assume it is free money.

**3.5 Golden pivot, the ledger's angle, restated and left alone.** A pivot at
the phi-quantile of the sample gives 38/62 splits. The information argument
says the expected work is minimised at 1/2 (the split entropy H(0.382) is
0.96 bits, so 4% more levels), and C10 measured 6%. No cache argument
rescues it: the small side is in cache at either ratio. Do not re-run.

## 4. Order from chaos: mechanisms from scripture, philosophy and biology

Each entry is a mechanism first and a metaphor second. Where the metaphor
did not yield a mechanism it was dropped.

**4.1 The firmament (Genesis 1:6): separate the modes before drawing the
splitters.** The value table spends its 4096 root cells over the whole range,
including the empty gulf between bimodal's modes, and pays child tables
inside each mode; the key table helps only when modes differ in magnitude.
A first pass over the sorted sample finds gaps larger than ten times the
median gap and declares modes (at most 8); each mode gets its own value
table over its own range; the lookup picks the mode with up to seven
branch-free compares and then indexes. *Prediction:* bimodal's histogram at
14T from 7.3 to ~5 ms, wall 23.7 to ~21, which closes the last parallel row
where x86-simd-sort leads; heavy_tail and uniform unchanged (one mode).
*Bench:* stbench with a fourth engine. Cost: a day.

**4.2 Quorum sensing (bacteria decide together).** Each histogram thread
picks its table and leaf style from its own two or three trial chunks, so
one preempted trial mis-picks that thread for the whole phase (truth table
B17). Bacteria act on the concentration of everyone's signal, not their own.
Threads publish their trial timings through atomics; after a quorum (half of
P) has reported, every thread commits to the group's median winner.
*Prediction:* no change on a quiet box; under load, the per-thread busy
spread of the histogram phase drops from the 1.06-1.10 measured to ~1.03,
and the worst-rep time improves 5-10%. *Bench:* profile-full under the two
hogs, min and max of 12 reps. Cost: half a day.

**4.3 Nothing new under the sun (Ecclesiastes 1:9): a plan cache.** Real
workloads sort the same kind of data repeatedly (each batch of a stream).
FFTW plans once and reuses. Key a small cache on (n bucket, sampled skew
class, cardinality class, sortedness class); on a hit skip the trial chunks
and reuse the table choice and leaf style; on a miss run the trial and
store. *Prediction:* 3-5% on repeated calls at 14T (the trial chunks and
the sample are that share of wall), nothing on the first call. *Bench:* a
new "stream" bench that sorts 50 batches of one shape. Cost: a day.

**4.4 Jethro's hierarchy (Exodus 18): rulers of thousands, hundreds, tens.**
A two-level histogram table (13 x 13 cells for 168 splitters) fits in L1
where the 4096-cell root plus children spills to L2. *Prediction:* neutral
on uniform (the root already resolves 96% of lookups in one load), 5% on
bimodal and clustered, which 4.1 would take anyway. Build only if 4.1 fails.

**4.5 Threshing floor (Ruth 3): winnow in one pass.** Already built: the
streaming small partition in the AVX2 leaf is a winnowing pass. The
extension is to winnow the *top* level the same way when scratch is free:
partition into the ping-pong buffer instead of in place, which removes the
end-choice branch from the large partition too. *Prediction:* 5-8% on the
sequential random shapes (0.48 to ~0.40 ns per element per level over 17
levels), at the price of one n-sized copy per level that is free while the
level is L2-resident and costs 0.1 ns per element when it is not. Cost: a
day, mostly the ping-pong bookkeeping.

**4.6 Immune negative selection: learn what "self" looks like, reject the
rest cheaply.** The dispatcher's probes are hand-tuned thresholds
(FLASH_SAFETY_RATIO, skew > 8, quant <= 4). Occam's razor made formal is
minimum description length: score the 64-sample under four models (uniform
interval, lattice, few-distinct, runs) by the bits needed to describe it, and
dispatch to the shortest description. *Prediction:* identical decisions on
the nine bench shapes (the thresholds were tuned on them), different and
better decisions on the chaos zoo below, where at least two shapes are
currently mis-dispatched. *Bench:* a decision-trace over the zoo, then
timing. Cost: two days. Worth it only after the zoo exists.

**4.7 Maxwell's demon and Landauer: time against entropy.** The
thermodynamic minimum for sorting is n·H bits of work, not n·log n, and the
engine's paths (counting, lattice, relocation) are exactly the ones that
approach n·H. Nobody has plotted it. Generate a family of shapes with
controlled entropy (Zipf with exponent 0.5..3, mixtures of k uniform
plateaus, near-sorted with controlled inversion rate) and plot time per
element against H for logos, x86-simd-sort, IPS4o and the LSD radix.
*Prediction:* the comparison sorts are flat in H except IPS4o's equality
buckets and x86-simd-sort's duplicate handling, which bend at H < 3 bits;
logos bends from H ~ 12 bits down; the LSD radix is a flat line. *Bench:* a
new entropy-scan bench with a line chart in reports. Cost: a day. This is
the chart that would say in one picture what the thesis claims.

**4.8 The chaos zoo.** Deterministic chaos has invariant densities that no
current shape exercises. Add: the logistic map at r = 4 (arcsine density:
infinite density at both edges, the worst case for equi-width classes), the
x-coordinate of the Lorenz attractor (two lobes, bimodal with structure
inside each), Collatz stopping times (integers, heavy tail, moderate
cardinality), Benford / log-uniform magnitudes, a Brownian path (integrated
noise: near-sorted with drift and reversals), prime gaps (small integers,
lattice, skewed), the Weyl sequence (2.1), a jittered millisecond lattice
(1.2), and Zipf. *Predictions:* logistic map is the one shape where the
parallel leaf loses 1.3x to today's numbers (flash's fix-up under arcsine
density); Lorenz-x is dispatched like bimodal; Collatz goes to int32 radix
and is fine; Brownian is mis-dispatched (runs too short for the merge, not
near-sorted enough for momentum) and pays the full partition for data that
is 90% ordered locally; prime gaps go to counting. *Bench:* parbench and
profile-full with the zoo; then 4.6. Cost: a day for the shapes.

**4.9 Differential adhesion (Steinberg): cells sort by local exchange.**
Odd-even transposition is what a sorting network is; done. The biological
point that survives is that local exchange finishes a *nearly* sorted
configuration fastest. The engine's momentum insertion sort is that; its
gate (last_bail_avg_run) is inherited from the JS era and never re-tuned on
C. *Prediction:* re-tuning it on the Brownian shape gives 1.5x there, nothing
elsewhere. Cost: hours, after the zoo exists.

**4.10 Metaphors that yielded no mechanism** (recorded so they are not
re-mined): Noah's pairs, Babel (NUMA, unmeasurable here), Solomon's split
(the median, already the pivot), Daniel's "numbered, weighed, divided"
(count, histogram, scatter: a good caption for the profile treemap, not an
experiment), Zeno (depth limits), Plato's forms (the rank model, section L),
flocking and slime moulds (the chunk scheduler already is local rules),
annealing over knobs (an autotuner: real, but it is 4.3's cache plus a
search, and the knobs left are worth under 5%).

## 5. Grades (2026-09-11, `make chaosbench`; FINDINGS §3h)

| item | predicted | measured | verdict |
|---|---|---|---|
| 1.1 Weyl sampling | 3–5× on the aliased sawtooth, 0 elsewhere | 14× on sawtooth_stride at 14T, 9× on van der Corput (unpredicted), nothing else outside noise; the collatz 1.18–1.25× first credited here was an abandoned lattice attempt failing sooner, fixed properly as FINDINGS §3i | shipped |
| 2.1 low-discrepancy routing | 8M Weyl 1T ~40 ms | 79.6 ms (weyl), 77.9 (van der Corput) vs 100; never fired elsewhere | shipped; the estimate of coarse's cost at 8M was wrong |
| 4.1 firmament (per-mode tables) | bimodal 14T −5 to −8% | −13% (22.8 → 19.9 ms), level with x86-simd-sort there | shipped |
| 3.1 network pivot | 2–3% at 1T | 0.94–1.06×: noise | opt-in `-DAV_PIVOT_NET` |
| 3.2 sample-as-pivot equi-depth leaf | leaf −10 to −20% on skewed shapes | not built: the profile shows the leaf on heavy_tail only 7% above uniform, bimodal below it; ceiling ~3% of wall | dropped, with the reason |
| 4.8 chaos zoo | two mis-dispatches | found: van der Corput aliasing (fixed by 1.1) and sawtooth's 1T run-merge path (545 ms, open) | built |

Not yet built: 1.2, 1.3, 2.2, 3.3, 3.4 (predicted neutral), 4.2, 4.3, 4.4,
4.5, 4.6, 4.7 (the entropy chart), 4.9.

## 5b. The original ranking

| rank | item | prediction | cost | why first |
|---|---|---|---|---|
| 1 | 3.2 sample-as-pivot equi-depth leaf | leaf -10 to -20% on heavy_tail and bimodal at 14T | 2 days | the C17 lever with the sample already paid for; the last parallel row x86-simd-sort leads |
| 2 | 4.1 firmament: per-mode splitter tables | bimodal histogram 7.3 -> 5 ms at 14T | 1 day | same row, different phase; the two together should flip it |
| 3 | 2.1 disorder probe routing Weyl-like data to flash | 8M Weyl 1T ~40 ms vs SIMD ~80 | 1 day | the thesis's one single-threaded win on continuous data |
| 4 | 4.8 chaos zoo + 4.7 entropy scan | two mis-dispatches found; one chart | 2 days | new shapes are how every recent win was found (scale-full found the thread policy) |
| 5 | 1.1 Weyl sampling stride | robustness: 3-5x on aliased input, 0 elsewhere | half a day | cheapest, and the only golden-ratio idea with a theorem behind it |
| 6 | 4.2 quorum trial | worst-rep -5 to -10% under load | half a day | B17 closed |
| 7 | 3.1 network pivot, 3.3 interpolation pivot | 2-5% at 1T | hours | small, cheap, independent |
| 8 | 4.5 ping-pong top-level winnow | 5-8% at 1T random | 1 day | after 3.x, since they share the partition |
| 9 | 4.3 plan cache | 3-5% on repeated calls | 1 day | needs a stream bench first |
| 10 | 1.2 incommensurate classes | 1.2-1.4x on jittered lattices only | 1 day | after the zoo shows the shape matters |
