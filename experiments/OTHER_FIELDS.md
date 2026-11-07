# Other fields: ideas from outside computer science and mathematics (round nine, 2026-10-21)

Asked: "hallucinate more. pull from other branches other than computer science and math." Each idea below names the field it
comes from, the observation there, what it maps to in the engine, what the project already has under another name, and a
prediction written BEFORE the first measurement. Status and measured results are filled in afterwards, in place; a wrong
prediction stays visible next to its grade.

## The ideas

### 1. Kleiber's law (biology): metabolic rate scales as mass^(3/4), not as mass
An elephant does not burn a mouse's energy per gram. The engine's shared sample does: 2048 Weyl points whether the array holds 4096
elements or 8 million, which at 4096 is half the array read at random and sorted (edgeprobe, FINDINGS 3bn: one element across 4096
costs 25-66% more PER ELEMENT on most shapes; IDEAS_LOG 11 (b)). **Mapping:** the gate sample grows sublinearly at the small end,
`m = min(2048, n/8)` or a 3/4 power, while the routes it feeds keep their decisions. **Ceiling first:** the engine against its own bare
AVX2 kernel on unstructured data from 4096 to 65536, one thread, batches of distinct inputs: that difference is everything the
dispatcher costs before the leaf. **Prediction:** the dispatcher is 15-35% of a uniform sort at 4096 and 5-10% at 65536; a smaller sample
recovers 1.10-1.30x on unstructured shapes at 4096-16384, structured shapes at those sizes 0.97-1.03 (their gates keep firing).

### 2. Hub and spoke (postal logistics): sort by region at the hub, by street at the spoke
Mail is not sorted letter against letter: the hub reads the first digits and drops each letter into a regional bag in one motion,
and only the local office orders a bag by street. **Mapping:** at ONE thread a certified-uniform array goes to the AVX2 quicksort
whole, ~23 partition levels, the top ~11 of them streaming 64 MB through DRAM. One equi-width scatter (the KS certificate's grid, no
tables) into L2-sized buckets replaces those top levels with one pass, and the kernel sorts each bucket in cache. Not the ledger's
"partition + SIMD leaf" (measured at 20 threads, before the kernel bridge) and not `coarse_bits_f64` (scalar leaves). **Ceiling
first:** a told hub-and-spoke (min/max, count, scatter, the bare kernel per bucket, copy back) against the engine at 1M-8M.
**Prediction:** 1.10-1.35x at 8M, ~1.0 at 1M; built only if the ceiling clears 1.15.

### 3. Time-division multiplexing (telecommunications), and antiparallel strands (molecular biology)
A TDM line carries k channels in fixed rotation: sample j of the frame belongs to channel j. Round-robin shards writing into one
log, a table with k sorted columns stored row-major, cumulative counters from k sensors polled in turn: each CHANNEL is monotone,
and adjacent elements belong to different channels, so every probe the engine has sees noise (OUTSIDE_THE_BOX C1, "strided order",
raised 2026-09-12 and never measured). DNA adds the other half: its two strands run in OPPOSITE directions, and orderprobe's zipper
(one channel up, one down) sorted 1.17x slower than its own values shuffled. **Mapping:** a probe of a few contiguous windows tests
each stride k in 2..16 for strands that are monotone in either direction; a demux pass lays the k strands out contiguously; the run
routes (the knot sweep with the twist and the vector merge) finish them. **Prediction:** 2-4x at one thread (k = 2 near 4x, k = 16
near 1.5-2x), zipper included; random data pays one probe that fails in a few compares (guards 0.98-1.02).

### 4. Rising sequences (card magic; Bayer and Diaconis, "seven shuffles suffice")
A riffle shuffle of a sorted deck interleaves two rising sequences at random; r riffles leave at most 2^r. Magicians read a deck's
rising sequences to find a card. Data of that shape: k sorted sources interleaved one element at a time in no fixed rotation (a
concurrent scan of k sorted partitions into one buffer). Adjacent elements are from different sources, so no run is longer than a
few elements, and unlike TDM there is no stride. **Mapping:** patience piles: each element goes on the pile whose top is the largest
value not above it (a vector compare against at most 16 tops), the piles are sorted strands, the knot route merges them. Gated on the
SAMPLE in position order needing at most 16 piles (random data needs ~2 sqrt(m), about 90). The JS-era rejection of a "greedy ascending
subsequence" was of a sampled ORDER RATIO used as a probe, not of this route. **Prediction:** 1.5-3x on 1-3 riffles and on 2-8
interleaved sources at one thread; guards 0.98-1.02.

### 5. Moire patterns (optics): two fine gratings make a coarse ghost
Every sampler reads at Weyl positions since the chaos round, and the round before this one still found a strided probe aliasing with a
period of 4 (`quant_probe_f64`). **Test:** sweep the PERIOD of periodic inputs (ramps, triangle waves, two-level square waves) across
~40 periods from 2 to 65536, dense around powers of two and around the samplers' lags, each against its own values shuffled. A smooth
curve is health; a spike at one period is a gate whose decision depends on where it happened to look. **Prediction:** at most two
spikes above 1.15x, both at periods that are multiples of a fixed probe length (8, 32, 64).

### 6. The founder effect (population genetics): a colony's first settlers fix its gene pool
A gate that reads the first elements trusts founders (`sf_prefix` settles int / ascending / descending on 64 elements; the prescan
reads from the front). **Test:** shapes with an unrepresentative FOUNDER at the front (64 or 4096 sorted values, 64 integers, 64 copies of
one value, 4096 values from another distribution) against the same multiset with the founder's elements scattered at random
positions. Position should cost nothing. **Prediction:** every ratio 0.95-1.05 except one: a sorted founder of 4096 on an integer shape.

### 7. Landauer's principle (thermodynamics): erasing a bit costs kT ln 2
A sort resolves a known number of bits: log2 n! for a random permutation, n log2 k for k interleaved sources, log2 B! for B shuffled
blocks, about 0 for a sorted array. **Test:** time per element against bits per element over ~20 shapes whose generators fix their
entropy (ORDER_FROM_CHAOS 4.7, never built). The shapes far above the curve pay for order they never use. **Prediction:** the
interleaved shapes of 3 and 4 above are the worst offenders, at a few bits per element and random's cost; every shape a route takes
sits within 3x of uniform's ns per bit.

### 8. Immune memory (immunology): the second exposure is answered faster
A program that sorts the same kind of array every frame pays the full probe every frame. **Mapping:** remember the last call's route
per size class and fingerprint, and skip gates that declined. **Not built unless** the Kleiber ceiling shows the dispatcher above 20%
of a sort from 65536 up after idea 1: a memory that skips a probe can skip the route that would have won.

### Already in the engine under another name
- Air traffic control's constrained position shifting (an aircraft lands at most k slots from its first-come order): the short-reach
  route. Runway sequencing is bounded displacement.
- Stratigraphy (faults move blocks, overturned folds reverse them, intrusions cut through): relocation, the twist, strays in strands.
- Hump yards (railway cars sorted by gravity onto classification tracks over several humps): radix sorting, with its ledger rows.
- Ecology's species estimators: the shared sample's cardinality IS Chao1, a capture-recapture estimator from ecology.
- Triage (medicine): choosing a leaf per bucket (IDEAS_LOG S8). Split-half reliability (psychometrics): S9.
- Gel electrophoresis (migration by log size): the key grid's log-like total-order key space.
- Okazaki fragments (the lagging strand copied backward in pieces): the twist reads descending runs where they lie.
- Mise en place and just-in-time inventory: lazy contexts and lazy scratch.

### Rejected before measuring, with the reason
- Equal temperament (a GEOMETRIC lattice of values): the common cases (MIDI pitches, decibel steps) have under 3300 distinct values and
  the counting routes take them; a key-space lattice is not linear inside a binade, so there is no one-multiply index.
- Comparative advantage (Ricardo) for performance and efficient cores: needs thread placement, which WSL2 does not give (pinning is in
  the ledger as tested and rejected).
- The marginal value theorem (optimal foraging: leave a patch when its yield falls below the habitat's average): the engine's give-up
  rules are already budgets set by measurement; no mechanism here that a budget does not already express.

## Results

### First pass (`make fieldprobe`, one thread, 2026-10-21): what the ceilings and tests said

**1. Kleiber (`./fieldprobe small`).** The engine against its own bare AVX2 kernel on batches of distinct inputs: uniform **1.38 at
4096**, 1.16 at 8192, 1.08 at 16384, 1.05 at 32768, **1.11 at 65536** (the probes that start there), 1.02 at 131072, 0.98 at 262144;
normal and lognormal the same within 0.04. Predicted 1.15-1.35 at 4096 and 1.05-1.10 at 65536: both a little LOW. At one thread the
gate sample is 384 points, not 2048, so the sample is 9% of a 4096-element array and the rest of the 38% is the gates it feeds.
**int_wide reads 1.72 at 4096 and still 1.11 at 262144**: on distinct inputs the int32 radix loses to the AVX2 kernel at every size
measured, which the ledger's int32 row (0.83-0.94 the other way, frontprobe, one repeated input) did not see.

**2. Hub and spoke (`./fieldprobe hub`): LOST everywhere, 0.58-0.91.** A told one-thread scatter into equi-width buckets of 4096,
16384 or 65536 elements plus the bare kernel per bucket, against the engine, 1M-16M, uniform and normal: 0.75-0.91 with a plain scatter,
0.58-0.87 with a write-combined one. Predicted 1.10-1.35 at 8M: **wrong in sign**. The scatter costs 2.5-4 ns per element at one thread
and the top quicksort levels it replaces cost less: the ledger's "a scatter is per-core latency" holds at one thread too. Not built.

**3. and 4. The weave (`./fieldprobe weave`).** Against a told demux (a stable scatter by channel, then the engine on the strands):
tdm2 1.29, the zipper **1.86**, two random sources **3.0**; but four or more channels and sources read **0.38-0.73**, because the
engine on k sorted strands LAID OUT end to end is slower than on the interleave: four strands 14.3 ns per element, eight 17.0,
sixteen 12.8, against 8.9 for random data. That moved the target from the demux to the run routes (below).

**5. Moire (`./fieldprobe moire`), the round's largest finding.** A triangle wave sorted slower than its own values shuffled at every
period from 127 up: **2.2-2.5 at 1024 and 2049-4097, 7.8 at 4096 (60 ns per element), 2.3-3.1 from 8192 to 65536**; ramps 1.3-1.7 from
period 64 to 4096; square waves flat. Predicted "at most two spikes above 1.15, at multiples of a probe length": wrong twice over --
it is a broad band, not aliasing. The trace (`./fieldprobe_trace once tri 4096`): the leaf's LOW-DISCREPANCY probe (order from
chaos M3) reads two windows of 64 contiguous elements, finds their sorted gaps evenly spaced and calls the data a Weyl sequence; the
AVX2 kernel is refused and coarse recurses on 512-element clusters. Evenly spaced gaps also describe a RAMP. What the probe never asked
is what low discrepancy means: whether the window COVERS the range (a Weyl window spans ~63/64 of it, a ramp's 64 / its length).

**6. Founder (`./fieldprobe founder`).** A 64-element sorted, integer or constant prefix made uniform, normal and quantized data
**11-19% slower** than the same elements scattered; a 4096- or 65536-element sorted founder the same. Predicted all 0.95-1.05 but one:
wrong. Mechanism, read in `sort_fn`: a 64-element prefix that is sorted, integral or constant settles none of int / ascending /
descending, so the whole range takes the scalar profile loop AND the AVX2 kernel's own scan, where a settled prefix lets the kernel's
first partition carry the check. And sixteen distinct values with 4096 huge outliers at the front: **2.16x** (not traced yet).

**7. Landauer (`./fieldprobe landauer`): ns per element against bits per element**, excess = cost / (bits x uniform's ns per bit).
Under 1, where the routes live: few256 0.41, few4096 0.27, coarse windows 1.27, reach4096 1.40. Far above: **sorted data with 1%
random swaps 80x** (10.1 ns per element for 0.29 bits: MORE than random data), the zipper 24x, two random sources 12x, eight sources
10x, four TDM channels 9.7x, runs of 16 equal values 7.5x, two sorted halves 5.9x, 64 sawtooth runs 4.9x. Predicted: the interleaved
shapes the worst offenders (right), every routed shape within 3x (wrong: near_1pct is the worst, and it is not interleaved). Traced:
1% swaps displace 2% of the elements, past the run detector's stray cap of n/64, so the detector bails and the leaf sorts everything.

**And one the probe tripped over:** two random sources (`src2`) cost 5.0-5.3 ns per element on three seeds and 10.3-14.5 on five. The
short-reach probe calls disorder dense at 25% adjacent descents, and two sources interleaved at random show EXACTLY 25% in
expectation: the seed decides. The losing seeds then paid 9.4 ms for the cuts route's block pass before it declined.

### The fixes, each behind a switch, with the prediction written before the A/B
- **Coverage in the low-discrepancy probe** (`LOWDISC_MIN_COVER` 0.5, `LOGOS_LOWDISC_LOCAL=1` restores the gap test alone): triangles at
  1024-16384 1.5-7x, ramps 1.3-1.7x; Weyl and van der Corput 0.95-1.05 (they must keep the probe); everything else 1.00.
- **Founder windows** (three more 64-element windows when the prefix settles nothing, `LOGOS_SF_FRONT_ONLY=1`): the uniform, normal
  and quantized founders 1.10-1.19x; the few16 founder unchanged (another mechanism); everything else 1.00.
- **The k-way merge in vectors** (`merge_runs_vec`: the knot route's k >= 3 crossings and the leaf's run merge, pairwise through the
  two-chain vector kernel, `LOGOS_RUNMERGE_SCALAR=1`): 3-8 laid-out strands 1.5-2.5x.
- **The cuts route declines when nearly every sample point is a candidate** (`LOGOS_CUTS_DENSE=1`): the src2 seeds that reach it lose
  their 9.4 ms; coarse windows unchanged.
- **The short-reach probe's dense/sparse threshold**, laddered (`LOGOS_REACH_DENSE`, 0.25 shipped): src2 ~0.4 at 0.10-0.15.
- **The stray cap**, laddered (`LOGOS_STRAY_DIV`, 64 shipped): near_1pct and near_2pct 0.3-0.5 at 32 or 16.

### The A/B (`FP_SWITCH=<switch> ./fieldprobe ab`, `RB_SWITCH=<switch> ./roundbench`, one thread unless named, 4M; each switch
restores the OLD behaviour, so above 1 means the new default is faster). From 12:42 on 2026-10-21 a headless Chrome from another
session used ~35 cores' worth of CPU time; the runs taken under it read uniform data at 18-33 ns per element (against 9-10) and are
discarded. Everything here was measured before it started or on the quiet box after it ended (19:50-20:05, a uniform calibration
line of 9.8-10.9 ns at the start, middle and end of the pass, and a null run reading 0.96-1.09).

| change | prediction | measured | verdict |
|---|---|---|---|
| **Founder windows** (three more 64-element windows, flags ANDed; `LOGOS_SF_FRONT_ONLY=1`) | founders 1.10-1.19 | founders **1.13-1.16**; ramps, runs16, saw64 1.06-1.14 as a bonus (they never settled either); 23 roundbench guards 0.97-1.08. After it the founder test reads 0.98-1.04 where it read 1.11-1.19 | SHIPPED |
| **The k-way merge in vectors** (`merge_runs_vec`; `LOGOS_RUNMERGE_SCALAR=1`) | 3-8 strands 1.5-2.5 | 3 / 4 / 8 laid-out strands **2.08 / 2.50 / 2.60**, at 14 threads **1.32-1.36**; eight concatenated logs (a roundbench guard) **2.13**; every other guard 0.97-1.04 | SHIPPED |
| **The stray cap 1/16 at one thread** (`LOGOS_STRAY_DIV`, 64 before) | 1% and 2% swaps 2-3x | 1% random swaps **3.07**, 2% **2.09**, roundbench's near_sorted **6.41**; guards 0.95-1.04. **At 14 threads 1/16 LOST** (2% swaps 0.33, 3% 0.84: the near-sorted route takes random swaps there), so P > 1 keeps 1/64. One loss: 3% swaps (6% displaced) **0.885**. The ladder on the quiet box puts 1/8 and 1/12 level with 1/16 on every row (0.96-1.06), so that loss is the stray path's own cost at 6%, not the cap's position | SHIPPED, the 6% loss recorded |
| **The short-reach threshold 0.20** (`LOGOS_REACH_DENSE`, 0.25 before) | src2 ~0.4 | src2 on the five losing seeds **2.0-2.6x** (12.0-14.3 -> 5.5-6.3 ns per element); 46 guard cells at 1 and 14 threads 0.94-1.04 | SHIPPED |
| **The cuts route's dense decline** (`LOGOS_CUTS_DENSE=1`) | src2 seeds lose their 9.4 ms | the 9.4 ms decline is traced; with the reach threshold moved, no bench row reaches it: 0.97-1.04 on 45 rows | KEPT as a guard on a traced cost |
| **Coverage in the low-discrepancy probe** (`LOGOS_LOWDISC_LOCAL=1`) | triangles 1.5-7x, Weyl / vdC 0.95-1.05 | with the cut at 0.5 (the definition): short legs won (tri256 1.09, tri1024 1.15, ramp512 1.19, clustered short legs 1.94) but long clean legs **lost 0.76-0.81** and the saw64 guard 0.85: coarse beats the kernel there (its scatter follows the local order). **As a band** (the kernel for windows covering 5-50% of the range, the old path below 5%), measured on the quiet box: tri256 **1.09**, tri1024 **1.19**, ramp512 **1.22**, clustered short legs **1.97**, and clustered LONG legs **2.23** (the top level keeps coarse, but its sub-buckets now take the kernel instead of recursing); long clean legs, saw64, Weyl and vdC 0.98-1.03; 23 guards 0.95-1.03 | SHIPPED AS THE BAND |
| **The detector's checkpoint minimum** (no stop before 4,096 elements; `LOGOS_RELOC_CHECK_EARLY=1`) | the scattered-founder near_sorted 5x | **0.19 -> 0.94-0.99** (the founder test's paired ratio, under the hog and again on the quiet box). A disordered head of 4,096 or more still reads 0.19: the average-run rule ends the scan there, rightly. Guards 0.97-1.05 at one thread, 0.89-1.05 at 14 (kinks 0.89, inside that column's spread on null-like runs) | SHIPPED |

**Two mistakes of my own, both caught by the A/B before anything shipped.** (1) The founder windows settled triangles, which
DEFERRED the profile pass, so the coverage test compared a window against the four windows' extremes instead of the range, called
the triangle low-discrepancy and refused the kernel: zero partition levels, **2x slower** (0.48-0.56). The kernel's statistics build
named it (`levels`: 0.00 against 17.60). The coverage test now takes the real range when the profile is deferred. (2) A first cut of
the band at 0.05 alone put the comparison the wrong way round -- long legs to the kernel, short ones to coarse -- the worst of both.

**Negatives and ceilings, not built.**
- **Hub and spoke: LOST, 0.58-0.91.** (Above.)
- **The int32 radix against the AVX2 kernel on distinct inputs** (`make twinbatch`, `-DLOGOS_AVX2_INT32`, both orientations): the
  kernel **1.02-1.24 SLOWER** at every size from 4096 to 4M on random 31-bit integers. Predicted it would win below ~1M: wrong. The
  ledger's int32 row (measured on one repeated input) stands on distinct inputs too; int_wide's gap to the bare kernel is the
  dispatcher and the profile loop, not the radix.
- **TDM demultiplexing (C1, strided order)**: OUTSIDE_THE_BOX predicted 3-10x. After the vector k-way merge, the TOLD ceiling (a
  free demux and the engine on the strands) is **1.27-1.90** for 2-4 channels and the zipper, 1.01 at 8, **0.50** at 16: merging k
  strands costs log2(k) vector passes against a quicksort that resolves a bit for ~0.5 ns. Not built; the zipper (1.90) is the case.
- **Rising sequences (patience piles)**: told ceiling **1.14-1.46** for 2-4 sources, ~1.0 at 8 and 16. Not built.
- **Kleiber**: at one thread the gate sample is already 384 points, a sublinear sample; what the 1.38 at 4096 holds is the
  dispatcher's fixed path. Not built. **Immune memory**: not built for the same reason.

**The moire test after the round** (1M, one thread, the quiet box): the period-4096 spike is gone (clustered 7.8x -> 1.18) and
clean triangles read 0.93-1.32; but clustered triangles from period 4095 up still sort **2.3-2.9x** slower than shuffled at 1M (at 4M
the band's sub-bucket effect removed it), and ramps of period ~2048 read 1.3-1.6x.

**Still open** (IDEAS_LOG 12): clustered values on long legs at 1M (above), which want a cluster test the leaf has not got; sixteen
distinct values with 4096 outliers at the front 2.2x (not traced); the zipper at 20x its information share (its demux ceiling 1.90);
src8 reads 8.5 or 14.8 ns per element by seed (not traced); a disordered head of 4096+ before a near-sorted strand; 3% random swaps
at 0.885.

### The hardware question (asked mid-round: "Are there hardware optimizations that you can also perform?")
Measured before: AVX2 kernels (shipped), streaming stores where a pass only writes (the counting emit, shipped 1.21-1.36), a work
queue for the hybrid P/E cores (lattice map, shipped ~1.05); huge pages, streaming stores in the scatter, ping-pong buffers, prefetch
in the kernel, `-march=x86-64-v3`, PGO (all 0.95-1.03); pinning and cgroups (rejected: WSL2's CPUs are virtual). Not available here:
AVX-512, hardware counters (none open under WSL2), NUMA, a GPU. **Never tried before, measured now on the quiet box:**
- **4K aliasing between the array and its scratch: NIL.** Both are mmap-backed malloc blocks behind the same 16-byte header, so they
  share an offset within their pages. A switch handed the scratch out skewed by 64 or by 2112 bytes: **0.93-1.03** on 45 fieldprobe
  shapes and 23 roundbench guards. Predicted nil after reading the passes (a constant skew helps only streams at a FIXED distance,
  and the merges' read and write fronts drift); taken out of the engine again.
- **Code alignment: NIL.** `make twinbatch TW_NEW="-falign-functions=64 -falign-loops=32 -falign-jumps=32"`, both orientations, six
  shapes at 8 to 1M elements on distinct inputs: **0.95-1.07**, centred on 1.00 at every size, the small end included. Not adopted.
- **Streaming stores for the run-length write-back**: designed, not built. Its runs are short and unaligned, so streaming them
  directly would mix streaming and ordinary stores in one cache line; it needs a cache-resident stage streamed out in aligned 32-byte
  stores, the counting emit's 1.21-1.36 being the precedent. Queued (IDEAS_LOG 12).
So of the hardware levers this box allows, everything but that one is now measured, and the answer is the ledger's: the engine is
bound by per-core latency and pass count, not by the memory system, the alignment of its code or its buffers.
