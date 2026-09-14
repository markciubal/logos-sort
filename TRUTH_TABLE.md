# logos-sort — truth table and open avenues

Compiled 2026-09-10 from CLAUDE.md, FINDINGS.md, engines/c/{CHANGELOG,PARITY,README}.md,
experiments/paradigm/*.md, and the engine source (logos_psort.c 3.6.0). Nothing here was
re-measured; every status is a reading of the repo's own record.

Status legend

| status | meaning |
|---|---|
| **MEASURED** | verified output, calibration-checked, and (where two logos variants are compared) an interleaved A/B in one binary |
| **MEASURED, loaded** | same, but the calibration line flagged machine load; margins ±20–30%, winner pattern stable |
| **FALSE** | implemented and measured to lose; in the negative ledger |
| **INFERRED** | consistent with the measurements but never tested directly |
| **UNTESTED** | no measurement exists in the repo |

---

## A. Competitive position (8M f64, 20-thread single-socket box, WSL2)

| # | claim | status | evidence |
|---|---|---|---|
| A1 | logos 1T beats `std::sort` on every shape | MEASURED | 2.8–10.1× (3.6.0 quiet run) |
| A2 | logos PT beats `__gnu_parallel::sort` at equal threads on every shape | MEASURED, quiet 14T (§1d) | 9/0, every row ≥2.7× (near_sorted) up to 7.7× (quantized); the loaded 20T run's ≥3.4× / 8.1× were inflated by load |
| A3 | logos 20T beats IPS⁴o (SOTA parallel comparison sort) on every shape | **FALSE on a quiet box**; was MEASURED loaded | loaded: IPS⁴o 116–140 ms vs logos 7–72 ms. Quiet, 14T (FINDINGS §1d): logos wins 7 of 9 (1.5× random, 2–2.5× structured), IPS⁴o wins near_sorted 1.06× and few_distinct 1.2×. IPS⁴o sped up 3.5× between the loaded and quiet runs, logos 1.4× |
| A4 | Intel x86-simd-sort (AVX-512) 1T beats logos 1T on every shape | MEASURED (AVX-512 box, old) | logos at 0.26–0.90× of it there; on the AVX2 box with the AVX2 sequential leaf logos wins near_sorted, quantized and the block shapes and is at 0.76–0.93× on the random shapes (§1e) |
| A5 | x86-simd-sort MT beats logos 20T on every unstructured shape | **FALSE on AVX2 hardware after H0b + D16** (re-measured 2026-09-10) | AVX-512 box, old engine: uniform 34 vs 60 ms, heavy_tail 24 vs 72, bimodal 24 vs 69, int_wide 23 vs 59. AVX2 box, new engine: logos wins uniform 26.4 vs 30.7, int_wide 25.1 vs 31.0, quantized, near_sorted, few_distinct, both block shapes; ties heavy_tail 36.7 vs 35.3; loses bimodal 41.3 vs 24.4. AVX-512 kernels are ~1.5–2× the AVX2 ones, so on AVX-512 hardware parity is plausible and unmeasured |
| A5b | logos + x86-simd-sort leaf per bucket beats plain logos | MEASURED on AVX2: true on the 3 random shapes (24.2 vs 26.4, 28.1 vs 36.7, 26.4 vs 41.3), false on near_sorted/int_wide | reverses A9's AVX-512-era finding; not the default; needs AVX-512 re-run |
| A6 | logos beats x86-simd-sort MT on few_distinct and quantized | MEASURED, stable across runs | 7.2 vs 9.5 ms; 9.2 vs 23.3 ms (2.5×) |
| A7 | near_sorted is a tie at 20T | MEASURED, loaded | 26.2 vs 24.8 ms |
| A8 | logos + SIMD leaf reaches parity with x86-simd-sort 1T, never exceeds it | MEASURED | 0.72–0.98× |
| A9 | logos + SIMD leaf is *worse* than plain logos multi-threaded | MEASURED twice | worse on 5 of 7 shapes |
| A10 | The 1T gap on unstructured data can be closed within "produce a sorted array" on a single socket | FALSE | three restructurings (merge, cache-K, parallel-SIMD) all lost; see C |
| A11 | Behaviour on AVX2-only hardware (most consumer CPUs) | MEASURED 2026-09-11 (i9-13900H, quiet, 14T; FINDINGS §1e) | 1T the SIMD sort wins random shapes 1.23–1.31× (1.7× before the AVX2 sequential leaf) and few_distinct 2.6×, logos wins near_sorted and blocks, ties quantized/int_wide; 14T parity on random shapes (0.86–1.08×), logos 1.4× few_distinct, 1.76× quantized, 2.2–2.5× blocks. The loaded run (§1c) showed 7 of 9; that was load, and 20T on the quiet box reproduces the 14T numbers |
| A3b | IPS⁴o re-measured on AVX2 box | MEASURED, corrected | loaded 20T: loses every row (1.3–2.6×). Quiet 14T: loses 7 rows, wins near_sorted and few_distinct. Load hurts its barrier-synchronised phases far more than logos's chunk-parallel ones; loaded fair-fight ratios are upper bounds on logos |
| A12 | Behaviour on multi-socket / NUMA hardware | UNTESTED | single socket only; private buckets unmeasurable here |
| A13 | The win/loss pattern holds at sizes other than 8M | MEASURED 2026-09-10 (`make scale-full`, 1e4–1e7, AVX2 box, loaded) | Holds at ≥8M. **Below ~1M it inverts:** x86-simd-sort wins every parallel row, and logos at 20T is slower than logos at 1T from 1e5 to ~5e5 (uniform 1e5: 8.8 vs 2.2 ms) because the engine spawns P threads for any n ≥ 65k. Fixed cost of the parallel route on this box ≈ 5–10 ms. See D18 |
| A14 | Numbers in FINDINGS §1 are exact | FALSE | that run was loaded; only the per-shape winner is trustworthy |

## B. Cost model (what the floor is made of)

| # | claim | status | evidence |
|---|---|---|---|
| B1 | The random-data 20T floor (~40 ms) is per-core scatter *latency* plus fixed cost, not bandwidth | MEASURED | ping-pong removed 37% of traffic, time moved ~12%; write-combining is ~10%; hyperthreading gave 1.3–1.5× |
| B2 | The fine K=2048 scatter dominates; the top-level P-way scatter is near-free | MEASURED (2025; by 2026-09-11 the top-level histogram had become the largest parallel phase and is now gone, D32) | top scatter 17–19 ms 1T (≈2 ns/elem); a direct 2048-way scatter 72 ms |
| B3 | A comparison merge costs 15–26× a P-way scatter | MEASURED | loser-tree merge 286–434 ms vs scatter 17–19 ms |
| B4 | The engine is pass-overhead-bound: a change that adds a pass never pays | MEASURED ×3 | COARSE_K sweep, SIMD bucket-index, winner tree: each won in isolation, lost integrated |
| B5 | Page faults are ~13% of a sequential sort (first call; the pool amortises) | MEASURED | 26 ms per 8M seq; 15 ms at t=4; likely higher under WSL2 |
| B6 | Destination prefetch on/off is 15–25%; distance 16–64 is within 3% | MEASURED, interleaved | pfbench |
| B7 | The scatter and the histogram do not vectorize on a CPU | MEASURED | vpscatter 1.05×, vpconflictd 0.65× |
| B8 | Vectorizing the arithmetic of logos's memory-bound passes is noise | MEASURED | 1.22× byte-skip kernel = 0.1% end-to-end; 1.47× bucket-index = −3 to −5.5% |
| B9 | Sampling cost should scale with the decision's resolution, never with n | MEASURED principle | 3.3.0: 2048-sample proportions, O(cap) probes |
| B10 | The ≤64-element insertion-sort leaf is compute-bound, not memory-bound | INFERRED | it runs in L1; never isolated |
| B11 | Where time goes by phase (probes, histogram, scatter, leaf, copy, stalls) | MEASURED 2026-09-10 | `make profile` → profile-report.html; per thread, CPU vs preempted vs barrier-stalled vs idle |
| B12 | 1T stream bandwidth on the box | MEASURED | memcpy 64 MB ≈ 2.5 ms (calibration line) |
| B13 | **At 20T the top-level histogram was the dominant cost, ~9× the scatter in CPU time, and it was compute (the `bucket_of` binary search), not memory** | MEASURED, then FIXED (H0b) | uniform: histogram 836 thread-ms CPU vs scatter 88, leaf 313; ≤4% preempted; 49–70% of wall. After H0b the whole sort is 1.6–3× faster on those shapes |
| B14 | The 1T sequential route never runs the top-level partition (100% leaf); the 20T route pays a histogram that costs more per element than the whole 1T sort | MEASURED | ~100 vs ~29 ns/element; explains the ~3× 20-thread speedup |
| B15 | The parallel route has a fixed cost of ~5–10 ms on this box (thread creation, barriers, P×12 buckets), so all-threads-for-any-n loses to one thread below ~5e5 elements | MEASURED (scaling benchmark) | fixed by D18; the crossover where 20 threads beat 1 is ~1M elements here |
| B16 | **Noise anatomy of the reference box** (`make noiseprobe`, 12 reps): a 1T sort is never preempted (cpu = wall) yet varies 7% median / 56% worst from sibling-hyperthread and cache contention; parallel runs lose time slices in proportion to thread count (utilisation 0.99 at 2T → 0.62 at 20T, max/min 1.4 → 2.7); best minimum at 14 threads = physical cores | MEASURED | two hogs outside the project (VS Code software-WebGL ~44%, OneDrive ~20%) explain the session's ±12–25% bands; min-of-≥12 and interleaving are the mitigations, `LOGOS_THREADS=14` for quiet numbers |
| B28 | **"Bimodal" says nothing about what is inside a mode.** Splitting at the gulfs is cheap and general; assuming the pieces are then uniform is not. Measured worst-mode distance from uniform: two uniform bands 0.023 (certifies), three uniform bands 0.02, two gaussians 0.30, three uneven clusters 0.32, a wide gaussian mixture does not separate into modes at all | MEASURED (D39) | certify each piece rather than assume it; the same argument killed the exponent map, whose binades are not uniform inside either (C25) |
| B29 | **Adjacency beats quiet.** A machine whose speed wanders 5-10x over tens of seconds cannot be stilled, but two measurements taken back to back share its state, so the wander divides out. Pairing the variants and taking the median of 40 ratios resolves ~3.7% at 8M where best-of-N across separately compiled copies resolved ~1.25x | MEASURED (`make pairprobe`, FINDINGS 3u) | `bench_paired()` in `bench_common.h`; alternate which half goes first, or the destination-page bias adds 2-4% |
| B30 | **You cannot pin a core inside a VM.** `taskset` in WSL2 constrains a virtual CPU number; the Windows hypervisor still places it wherever it likes. The giveaway is that a thread pinned to CPUs 12-19 ran FASTER than unpinned, when on a 6P+8E part those should be the efficiency cores | MEASURED and REJECTED (FINDINGS 3u); cgroup cpuset would need root and hits the same boundary | one run showed spread 1.40 -> 1.13 and the next 1.32: a single non-reproduced improvement is not a result |
| B31 | **A quantile splitter is a load balancer.** Choosing splitters at sample quantiles equalises the buckets by construction, so the leaf phase has almost no scheduling slack left to win: traced largest-leaf-over-mean is 1.10x on uniform and 1.48x on Pareto, and the greedy makespan bound turns that into 8-11% of one phase | MEASURED (`make lptbench`; largest-first dispatch 0.95-1.05 on seven shapes, opt-in `LOGOS_LPT=1`) | before optimising a schedule, measure the imbalance: the bound is W/P + L(1 - 1/P), and with 12 leaves per thread a 1.1x L is worth nothing |
| B32 | **A runtime toggle beats a second compiled copy for an A/B.** Switching a feature with getenv once per call lets one binary measure both variants, which removes the up-to-1.24x code-layout bias (B26/rule 3) instead of controlling for it | MEASURED (used for `lptbench`; cost is one getenv against ~1 ms of serial setup per parallel sort) | only works for features that are a branch, not a different code path; pair it with `bench_paired` |
| B39 | **"Could not resolve" is not "neutral", and the difference is worth paying for.** Five features carried "mixed" or "noise" verdicts reached against a ~1.25x floor. Re-run at the ~4% floor with the code-layout bias cancelled by a copy swap, all five are neutral within 0.95-1.04 across five shapes at two thread counts | MEASURED (`tools/ledger_rerun.py`, 24 pairs x 2 orientations, 8M; the one borderline row re-checked at 80 pairs) | improving the instrument tenfold IS a new angle on a closed question, and it is cheaper than another idea |
| B40 | **Pairing cancels drift; only a copy swap cancels alignment.** Timing two variants back to back removes the machine's thermal wander, but each variant still sits in a fixed compiled copy. Building twice with the assignment swapped gives A = E(L0/L1) and B = (1/E)(L0/L1), so sqrt(A/B) is the true effect and sqrt(A*B) is the layout bias itself | MEASURED (the layout term read 0.96-1.04 over fifty cells, 0.999 on the 80-pair check) | the two corrections are independent; applying only one leaves the other in the number |
| B45 | **A multiscale occupancy profile over (index, value) sees arrangement, and its misses fall where they cannot cost anything.** With the rule fixed from six other shapes, a held-out twenty-shape zoo scored 4 of 8 arrangement shapes classified and 9 of 10 value-set shapes abstained; on the 3 cases where the engine left 1.2x or more against a sort told the structure it was blind on 0, and every miss was on a shape the engine already beats its ceiling on | MEASURED (`make zroute`; `make ztrain` confirms the rule as coded matches its fitting) | thin evidence, 3 opportunities; the profile names structure, it does not supply the route that uses it |
| B46 | **A sample cannot see order finer than its own spacing.** Window-shuffled data with disorder of 2048 and latency-displaced data with disorder of 4096 read sorted or unknown to a 4096-point sample of 8M, whose spacing is 2048 | MEASURED (`make zroute`) | fine-scale order needs a linear scan; do not ask a sample for it |
| B47 | **A classifier's rule can be too loose while its measurement is right.** The tail rule, "low early and peaks late", fired on Gray-code order, which the probe correctly saw as arranged, because consecutive codes differ by one bit | MEASURED (`make zroute`) | fix rules on fresh shapes, never on the test set that exposed them |
| B48 | **Tighten a rule on its failures, validate it on strangers.** The probe's tail rule was redesigned from the two false tails a held-out zoo exposed, then scored on fifteen shapes it had never seen: false tails fell from 5 of 7 to 1 of 7 with recall unchanged at 4 of 5 | MEASURED (`make ztail`) | the survivor, a random walk, shows the limit of occupancy alone: coarse order with fine disorder looks the same whether it came from a tail or a walk |
| B49 | **A sequential route offered to a parallel sort loses, and it took two routes to make that stick.** The dense-integer bitmap and then the sorted-prefix-plus-tail merge each won several times over at one thread and regressed at fourteen, the tail route to 0.34-0.87, because single-threaded passes displaced parallel routes that were already fast | MEASURED (`make mcbench`, `make prefixbench`) | gate a sequential route on the sort actually running sequentially; win at P only with a parallel form of the route |
| B50 | **A stable scatter by stream is already a sort, and the lookup is the tax.** Interleaved ascending streams separated by gulfs sort 2.2-3.0x faster at one thread by scattering stably into the streams and sorting nothing, but the speedup falls from 3.03x at 4 streams to 1.20x at 60, in step with the per-element compare against every boundary; a one-multiply bucket reached 5.9x in the zoo | MEASURED (`make streamsbench`) | make the stream lookup constant-time before widening the route |
| B51 | **A trace build is a different program.** The stream lookup table timed 1.3x faster than the compares on 8 perturbed streams in a `-DLOGOS_TRACE` build and 0.66-0.71x in the normal build, three paired runs | MEASURED (`make streamsprobe`, `make streamsbench`) | use a trace build to see whether a path runs, the normal build to see how fast |
| B52 | **The parallel tail merge wins where its prediction said it would lose, and not where it said it would win.** At 14 threads: 1.17x at a 95% prefix, 1.22x at 90%, 1.17x at 80%, 1.03-1.05x at 50% (predicted 0.5-0.7), 1.01x at 99% (predicted 1.3-1.6) | MEASURED (`make prefixbench`), two runs | the gate built on the wrong half of the prediction was removed; at 99% the near-sorted route is already as fast |
| B53 | **The sort was not a permutation.** Below 65536 elements -0.0 came back as +0.0 (counting sort, int32 radix, dense-integer route) or swapped sign with +0.0 (the AVX2 leaf's min/max networks). Four sites, none new, invisible to a harness that never held a -0.0 | MEASURED (`make zerodiag`, harness signed zeros) | fixed; the check that rides the NaN scan must add no compare or it costs 7% at 1 thread |
| B54 | **A counting route's gate is about the fallback, not the data.** At one thread the hash map beats the float radix (0.42-0.54x) and loses to the AVX2 quicksort (1.06-1.60x), integers or not; at 14 threads its serial merge decides at a sample Chao1 near 3000. The old gate cost up to 5.3x | MEASURED (`make cardgate`, held-out and fresh shapes) | Zipf's Chao1 under-estimate turned out to be the small problem |
| B55 | **A slow-looking row was variance, not a regression.** heavy_tail at 14 threads read 5.2x uniform in the scale report; the per-phase profile gave the same 2.04x scatter ratio as two days earlier, with heavy_tail's scatter CPU varying 2.4x run to run on identical data | MEASURED (`htprof.c`, profile report 2026-09-11) | memory-load-heavy lookups amplify a loaded box; compare within a row, never across rows measured minutes apart |
| B56 | **A byte table in key space is the heavy tail's certificate.** Cells of equal width in total-order key space, edges snapped to sample quantiles, one byte load per element: 1.32-1.40x at 8.4M and 1.54-1.83x at 1M on Pareto, lognormal and exponential data at 14 threads, guards 1.00 | MEASURED (`make kgbench`) | the exponent map failed the same idea with one piece per binade; resolution inside the binade was the whole difference |
| B41 | **You cannot beat an exact table by approximating it.** Three attempts have now failed to improve the partition's bucket lookup, and the reason is structural: the splitter table is built from the same sample the approximation is fitted to, so it already has zero in-sample error and any cheaper map can only trade balance for lookup cost. Curvature-placed knots made the lookup 3x cheaper and the worst bucket worse (1.68x vs 1.45x), netting 3-4% | MEASURED (`experiments/team/cdf-knots.md`; a first pass claiming 1.13x was retracted by a second) | to beat a table, change what is being approximated, not how well |
| B42 | **When a parallel route is slow, measure the serial phase before blaming the threads.** An atomic chunk queue for the counting routes was neutral where imbalance was predicted and won ~1.2x only where a serial phase dominated; the chaos map independently found the same route 3-7x off ceiling with workers idle most of the wall | MEASURED twice the same night, by two agents who did not talk (`count-queue.md`, `chaos-explorer.md`) | Amdahl before load balance. The slot merge walks slots x P on one thread |
| B43 | **A permutation of blocks is not a permutation of pages.** Page-table relocation cannot move a single byte of the bench's block-permuted data, because the block size in bytes is not a page multiple, so only the permutation's fixed points align and those already sit in place | MEASURED for the cost of a printf (`pagetable-reloc.md`); a prototype on manufactured aligned runs wins 1.3-2.0x but fragments the address space until a second sort faults | census the alignment before building the mechanism; the cheap half of an idea can kill the expensive half |
| B44 | **A skeptic's regression may not reproduce, and that is still worth acting on.** An independent re-measurement found the view's batch route 1.4-1.6x slower than a loop on sawtooth and diagnosed the cause exactly (the batch derived its sequential-leaf flag from the thread count). On the landed tree the old policy read 1.199 rather than 0.674, yet the fix improved every shape it touched | MEASURED both ways (CHANGELOG, this round) | act on a confirmed MECHANISM even when the magnitude will not reproduce; record that the magnitude did not |
| B38 | **A stub only generates the inputs you thought of.** The report checker substituted a uPlot stub and called every axis formatter, and still passed a report whose log-log charts were completely blank: the real uPlot hands a log axis a splits array with `null` in the slots it grids but does not label, and the stub only ever passed numbers | MEASURED (the bug shipped; `tools/browser_check.py` in real headless Chrome found it, and the strengthened stub now fails on the old report and passes on the new) | when a fake stands in for a library, make it emit the library's AWKWARD outputs, not its tidy ones; and keep one check that runs the real thing |
| B37 | **Take the window from the sample, not from the extremes.** A range gate keyed on min/max is one far value away from useless; a gate keyed on the sample's own range is not, because the sample describes the bulk. Spill what falls outside and sort it as its own run: the tails are outside the window on both sides, so no merge is needed | MEASURED (`count_try_i32`, `make intbench`, FINDINGS §3w): one INT32_MIN went from 11.3x the baseline to 1.19x | the same reason a sample cannot be trusted for the tail makes it the right thing to trust for the bulk |
| B35 | **One element out of eight million can cost 11x.** A range gate keyed on raw min/max is destroyed by a single far sentinel: a narrow int32 bulk sorts in 3.2 ms at 14 threads and in 36.3 ms with one INT32_MIN added. Frequency does not matter, distance does | MEASURED (`make intbench`, FINDINGS §3w) | the project's own rule restated: a sample bounds the bulk, never the tail, and a sentinel IS a tail of size one. Robust quantiles or gulf-splitting, never min/max |
| B36 | **The sign bit is already free on integers, so assuming negatives changes nothing about comparing them.** Zero-straddling data 3.2 ms, all-negative 3.3 ms, non-negative 3.2 ms. The bias map is affine and the byte-skip radix discovers whether the top bit varies in a pass it was already running | MEASURED (`make intbench`) | what the assumption DOES change is arithmetic: span in 32 bits wraps, and negation of INT32_MIN is undefined |
| B33 | **Type facts may be assumed; data facts must be certified.** "int32 is a lattice of step 1" is true of the type and can never be wrong, so probing for it is pure waste. "This data is uniform" is true of the data and must be tested every time. The exponent map failed precisely by treating a data fact (binades are uniform inside) as if it were a structural one | MEASURED both ways (D39/C25 for the data side; `make intbench` for the type side) | the test is whether a counterexample is *representable*, not whether it is likely |
| B34 | **For integers the key-space and value-space certificates are the same test, provably.** `(uint32_t)x ^ 0x80000000u` equals `x + 2^31` over the signed range: monotone and AFFINE. Kolmogorov-Smirnov is invariant under monotone affine maps, so both certificates return the identical answer. For doubles `total_key` is logarithmic, so they differ, which is the whole reason mode 7 exists | INFERRED by proof, consistent with the f64 measurements (§3q) | one of the two certificates is free to delete on any integer path |
| B27 | **A per-variant trial that times one phase optimises the wrong thing.** The histogram-free partition's trial times the bucket lookup and ignores the balance the lookup produces, so the cheapest lookup can be the worst partition | MEASURED (D37, C25) | fixed for the certified maps by predicting balance from the sample; the same question applies to any future self-tuning choice |
| B26 | **A gate's threshold and its algorithm's cost curve have to be checked against each other.** The counting route's gate admitted 64 distinct values while its lookup degraded from 2.0 to 30.8 ns per element over that range, and no bench shape sat in the window, so the cliff survived every sweep | MEASURED (D35) | the shapes in a suite bound what any A/B can find; the cardinality ladder is now in `cardbench` |
| B25 | **The probe should not be threaded.** Threads are created per phase and a spawn costs about what the whole sample costs; the sample's cost is latency (5376 independent misses), which memory-level parallelism from one thread removes. The 1-thread path does not pay for its unused context because untouched pages cost nothing | MEASURED (D34; the 1T prediction of 8–12% from the context pool was wrong for that reason) | a persistent thread pool remains the precondition for parallelising anything under a millisecond |
| B24 | **The dispatcher's order is cheapest-first and a cost model reproduces it**, so "sample once, decide once" changes how gates are answered, not which route wins; its value is in not drawing five samples and in estimating the true cardinality (Chao1) before committing to a counting route | MEASURED (D33) | the remaining guess is the fixed order itself where two routes both apply; no shape in the zoo exercises that |
| B23 | **A sample sizes buckets to 18%, not 2%.** A bucket's count is n times the spacing of consecutive sample quantiles, Beta(OVERSAMPLE, m−OVERSAMPLE+1), sd 1/√OVERSAMPLE; the DKW bound on the CDF (3.7% of n at 10⁻⁶) is six bucket widths and says nothing useful about one bucket. On certified-uniform data an equi-width grid's counts vary by √(n/K) ≈ 0.5% | DERIVED, then measured (spills ~0 at 60% slack; slack 0.3 within noise) | corrected a claim made in conversation on 2026-09-11 |
| B22 | **The right thread count depends on the route, not just on n.** Partition wants every thread (3 at 316k beat 1 by 1.3×); relocation's parallel form loses below ~500k per thread (6 threads beat 14 by 1.7× at 3.16M); the counting routes merge P tables, a cost ∝ P, so 4 threads beat 10 at 1M yet 3 beat 1 at 316k | MEASURED (`make smallbench`) | shipped as per-route caps (D30); a global 250k policy was measured and rejected |
| B19 | **The structured routes did not scale.** Same sort, same shape, 1T → 14T: partition route 4.6–5.5× (x86-simd-sort 4.0–4.2×), but near-sorted 1.8×, relocation 2.1–2.2×, lattice 3.7–4.4×, against 6–7× for IPS⁴o. The 1T wins come from doing less work; the parallel versions did not keep that (near_sorted: ~200 ms of thread time at 14T vs 36 ms at 1T) | MEASURED (profile report, FINDINGS §3j) | near-sorted fixed (D28); lattice/counting static ranges and relocation's serial middle still open |
| B20 | **Chunking by index changes the dispatcher's decisions.** Its gates are ratios of n (integer span ≤ n, runs per n, quantization); a chunk of near-sorted data has the whole value range, so the chunk leaf took the float radix or bits-coarse at 16–30 ns/elem where the whole array took counting at 6 | MEASURED (nsbench probes) | any parallel route that hands chunks to the dispatcher must first restore the property the whole array had (here: remove the strays), or not use the dispatcher at all |
| B21 | **The inversion probe bounds inversions, not displacement.** 40 strided pairs call a bucket nearly sorted; plain insertion sort on it is quadratic in displacement (333 ms for 8M at 14T once such buckets reached the partition) | MEASURED | momentum insertion with its 8n move budget now takes that case (D29) |
| B17 | The engine's own trial chunks are timed, so a straggler during a trial can mis-pick the table or leaf style | INFERRED | A-B-A-B trial with min per variant would be cheap insurance; unbuilt |
| B18 | Benchmarks default to the physical core count (`BENCH_THREADS` = 14 here, `bench_common.h`), engine default unchanged; WSL2 reports a fictional 10×2 topology, so the value is set, not detected | POLICY, from B16 | all reports from 2026-09-10 evening onward are at 14 threads, every contender at the same count |

## C. Negative ledger (implemented, measured, rejected — do not re-run without a new angle)

| # | idea | result | why |
|---|---|---|---|
| C1 | SIMD scatter (`vpscatter`) | 1.05× | decomposes to serial stores |
| C2 | SIMD histogram (`vpconflictd`) | 0.65× | needs gather+scatter per step |
| C3 | SIMD bucket-index in coarse | −3 to −5.5% | store-then-reload beats nothing |
| C4 | SIMD OR/AND byte-skip | 0.1% | correct kernel, irrelevant share |
| C5 | Winner/loser tree merge, near-sorted | 1.4–1.9× slower | one live run most of the time; heap pops O(1) |
| C6 | Shared per-thread context pool | regressed, then segfaulted | call sites need different sizes |
| C7 | Parallel merge sort (chunk-sort + k-way merge) | merge 15–26× costlier than scatter | replaces the cheap op, leaves the expensive one |
| C8 | Cache-conscious COARSE_K (64–512) | 0.58–0.90× | per-level overhead exceeds cache gain; 2048 optimal |
| C9 | logos partition + SIMD leaf, parallel | worse than plain logos, twice | pays scatter plus SIMD sort |
| C10 | Golden-ratio pivot | −6% vs random | — |
| C11 | Ping-pong (remove copyback) | neutral 1T, ~12% at 20T | kept; taught B1 |
| C12 | Private per-thread buckets (NUMA) | unmeasurable | single socket |
| C13 | JS-era: flips/descent counter, cardinality probe, gap-statistic gallop, wider radix digits (whole-array, 1T), greedy ascending subsequence | rejected | engines/js/CHANGELOG.md |
| C14 | 4.0.0 camelCase revision | withdrawn | convention only |
| C15 | J5 first cut: interval-overlap gate on raw runs | never fired, 0.99× | ascending block boundaries fuse runs around holes; gap-split first |
| C16 | J5 second cut: sequential probe + spawn-then-bail | −16% segmented 20T | scan chunk 0 on the caller, sample pre-gate |
| C17 | Leaf knobs: flash m = n/4, m = n/16, coarse on ~100k buckets (`make leafbench`) | mixed: one shape each up 12–25%, others down 10–45% | the leaf cost is the insertion fix-up (Devroye's n·∫f²) and the per-bucket probes, not the counters or the level count; lever is equi-depth classes gated by an ∫f² estimate, branchless fix-up, fewer probes |
| C19 | AVX2 quicksort in the PARALLEL leaf (33k-element L2-resident buckets, `make simdbench` `-DLOGOS_AVX2_PARALLEL`) | −5 to −28% at 14T, −10 to −16% at 6T on every random shape | the scalar flash/coarse leaf on an L2-resident bucket is already ~9 ns/elem; the vectorized quicksort is 8.5 at 1 thread and slower under 14, on a hybrid P+E CPU (E-cores run 256-bit AVX2 double-pumped). The same code wins 1.2–1.6× sequentially, so it ships sequential-only |
| C20 | Model splitter table (H0c: linear bucket-index model per value cell, third trial candidate; `-DLOGOS_MODEL_TABLE`) | 0.93–1.09× at 14T over two runs, 0.97–1.03× at 7T: noise | the histogram's residual cost on bimodal is branch behaviour on alternating modes, not the child-table dereference the model removes; a model with worst error ≥2 buckets loops and cost −12% on heavy_tail even as a trial chunk |
| C21 | 128-element bitonic base case for the AVX2 leaf | 3.0 vs 1.9 ns/elem for the 64 network | 32 vectors spill; one more partition level (0.4–0.7 ns/elem) plus the 64 network is cheaper |
| C25 | Exponent-histogram bucket map (`-DLOGOS_EXMAP`): flat table indexed by binade, linear interpolation on the mantissa, an approximate inverse CDF in one load and one multiply | lognormal 0.91×, exponential 0.80× against the key splitter table it displaces | a heavy tail decays *inside* each binade too, so its buckets take up to 3× their share and spill where the table's do not; the trial picks it anyway because it times the lookup, not the partition (which is what D37 now catches). First cut was worse still: indexed on the key's top byte, which holds 7 of the 11 exponent bits, so Pareto fell into two entries and 63% spilled |
| C24 | Lattice direct index before the low-cardinality scan (`-DLOGOS_LATTICE_FIRST`) | lattice_5 1T 51.5 ms vs the perfect hash's 20.9; random_50 14T 98.6 vs 64.1 for the shipped scan | cheapest kernel in isolation (0.8 ns/element) but its table spans the value range, ~96 KB against the hash's 4 KB; the fourth microbenchmark win in the ledger that loses on integration |
| C22 | Network pivot for the AVX2 leaf (64 strided samples through the bitonic network) | 0.94–1.06× at 1T over 15 shapes: noise | 15 insertion-sorted samples already give 17.4 levels against the ideal 17.0; opt-in `-DAV_PIVOT_NET` |
| C23 | Equi-depth leaf classes from the top-level sample (M1) | not built: ceiling ~3% | the leaf phase on heavy_tail is 7% above uniform and on bimodal below it (profile); quantile splitters make buckets locally flat |
| C18 | Bucket-relative digit narrowing for the int32 leaf, first cut (H5) | mixed: int_wide +21% / −6%, int_1e6 −16% | adds a min/max pass the fused byte histogram never paid; opt-in `-DLOGOS_INT_NARROW`; redo with the range passed from the splitters |

## D. Positive results (build on these)

| # | result | measured effect | where |
|---|---|---|---|
| D1 | Lattice (direct-index) counting | quantized 109 → 52 ms seq; 30k-distinct 105 → 53 | `lattice_try_f64` |
| D2 | Low-cardinality parallel counting (≤1024 distinct) | few_distinct 130 → 49 ms seq | `lowcard_try_f64` |
| D3 | Moderate-cardinality hash counting (≤65 536 distinct) | 2.5× at 256 distinct, 1.2–1.4× to 32k | `modcard_try_f64` |
| D4 | Near-sorted index-partition + one-pass P-way band merge | 28 → 22.5 ms at 20T | `nearsorted_parallel_f64` |
| D5 | Bucket-index cache + destination prefetch | 20T floor 65 → 41–49 ms | 3.4.0 |
| D6 | Dynamic chunking + spin-then-block barrier | load degradation 2–5× → 1.3–2× | 3.3.x |
| D7 | Native int32 engine (no widening) | int32 loss 0.71× → win 2.5× | 3.2.0 |
| D8 | Persistent scratch pool | measured the page-fault floor; ~13% | 3.6.0 |
| D9 | argSort (stable, index-carrying) + apply / inverse permutation | makes records-by-key possible | logos_extra.c |
| D10 | Order model (rank/select) without materialising | 1.57× at 64 distinct; loses ≥4096 | paradigm #2 |
| D11 | One-pass analytics instead of sort-first | quantiles 87× (approx), top-k 7× (exact), range-count 42× (exact) | paradigm #3 |
| D12 | Strict in-place (~1n) variants | 1.6–6× slower; raises the RAM ceiling, not speed | 3.6.0+ |
| D13 | Disjoint-run relocation (J5): memcpy shuffled sorted blocks into place from sorted run descriptors | 2.3–3.4× 1T, 1.9–2.2× 20T on block-permuted 8M; guards ±5%; parbench 20T 6.1× vs gnu_par | `relocate_try_f64`, `make j5bench` |
| D14 | Per-phase profiler: CPU vs preempted vs stalled vs idle per thread, leaf path per bucket, HTML treemaps | found B13 on its first run | `-DLOGOS_PROFILE`, `make profile` |
| D15 | Direct-index splitter tables with per-thread trial (H0b + J15): O(1) bucket lookup in the histogram | 1.6–3.0× at 20T on every shape that partitions; guards noise | `bt_init`, `do_hist`, `make stbench` |
| D16 | 12 buckets/thread (J16) + table leaf with the root style chosen per thread from the trial's root-resolution fraction | 12/t alone 1.08–1.44× on 9 of 12 partitioning rows across three runs; vs the old engine the adaptive leaf reads 1.13–1.37× at 20T on the partitioning shapes. **Unresolved:** adaptive vs always-branch-free, because that run hit a ±25% load burst; adaptive ships as the better of the two by construction. Interleaved counters and line repair mixed across three runs, opt-in | `BUCKETS_PER_THREAD`, `BT_LEAF`, `do_hist`, `make theorybench` |
| D17 | Profiler and parbench carry x86-simd-sort (AVX-512 or AVX2 per build) and IPS⁴o at equal thread counts; single-thread SIMD column now really single-threaded | first AVX2 fair fight, FINDINGS §1c | `make profile-full`, `make parbench-full` |
| D18 | Thread-count policy: at most one thread per 100k elements (found by the scaling benchmark, A13) | 20T rows at 1e5 3–20× faster (uniform 8.8 → 1.6 ms, block_perm 9.9 → 0.5), 1e6 14.0 → 4.3 with ten threads beating twenty; ≥2M unchanged | `effective_threads`, `make scale-full` |
| D19 | Scaling benchmark: time vs n for every contender and shape, 1T and P, linear axes with O(n) / O(n log n) references and fitted exponents | reports/scale-report.html | `make scale-full`, `scalebench.cpp` |
| D23 | **Weyl sample positions** (`floor(frac(i·φ)·n)` in every sampler; three-gap theorem) | MEASURED (`make chaosbench`): sawtooth whose period equals the old stride, 14T 546 → 38 ms; van der Corput 14T 177 → 19 ms (bit-reversal aliased the stride too, unpredicted); no shape outside its noise band. The collatz 1.18–1.25× first credited here was NOT the sampler (both samples saw ~280 distinct): see D27 | `SAMPLE_POS`, `weyl_pos` |
| D39 | **Per-mode certificate and linear map**: each mode's slice of the sample is tested for uniformity on its own, and if every mode certifies the partition buckets with branch-free mode selection plus one multiply-add instead of a table per mode | MEASURED with the copy-swap control in both directions (`make tailbench`, FINDINGS §3s): bimodal 14T 1.25–1.38×, three uniform bands 1.23–1.33×, gaussian mixture 1.01–1.03× (declines at mode KS 0.164), all other shapes and every 1-thread row inside the band | `hf_lookup_md`, `md_ok`, `-DLOGOS_NO_MODEKS` |
| D36 | **Uniformity certificate in key space** (the order-preserving key is monotone in the logarithm, so log-uniform data certifies there and buckets with one multiply) | MEASURED with the copy-swap control (`make tailbench`, FINDINGS §3q): Benford 14T **1.2–1.3×** (1.32× with the feature in one pair of copies, 1.15× in the other; uncontrolled runs said 1.45–1.50× and were reading the layout bias too). Sample KS 0.018 in key space against 0.746 in value space; correctly declines Pareto (0.692), and fires on no other shape in the suite | `ks_key_ok`, `-DLOGOS_NO_KEY_KS` |
| D37 | **Balance check on every certified bucket map**: apply it to the sorted sample and drop it if any bucket would take more than 3× its share | MEASURED: without it the trial chose a map that sent 63% of the data to the spill list and declined the whole partition after a full pass; with it, Pareto's exponent map is rejected at 4.8× and the sort falls back to the key table with zero spills. ~16k lookups, ~20 µs | `BAL_OK` in `psort_f64` |
| D38 | **Dominant-duplicate guard**: the partition declines from the sample when one value exceeds a region's capacity | MEASURED: a value holding 50% of the data declines in 2.3 ms instead of scattering 8M elements and then failing the spill test | `dup_frac`, `hf_partition` |
| D35 | **Verified perfect hash for the counting route's lookup** (multiply-shift over the sample's distinct keys into a byte table, every lookup confirmed by a key compare so a value the sample missed falls back to the incremental table) | MEASURED (`make cardbench`, FINDINGS §3p): it removes a 6.5× cliff nobody had benchmarked — 33 to 64 distinct values sorted 228–318 ms at 1T against 35 ms for 200 distinct — and is 15–24× on that window, 2.0–3.1× at 5 distinct, guards unchanged | `lc_map_body`, `-DLOGOS_NO_LC_PHASH` |
| D34 | **Serial fixed costs**: batched-prefetch sample reads (`-DLOGOS_NO_SAMPLE_FAST`) and a sample sort with no libc qsort anywhere on a sampled path (NaNs compacted to the tail, AVX2 quicksort 10.9× faster than qsort on 5376 points, the engine's introsort 2.0× where AVX2 is absent), persistent context bundles indexed per top-level sort (`-DLOGOS_NO_CTX_POOL`) | SHIPPED as the default: the trace shows the mechanism and the quiet half of the end-to-end A/B confirms 1.07-1.18x at 1M and 1.02-1.10x at 8M on the parallel rows with the 1-thread rows unchanged; the persistent bundles cost about 6 bytes per element of the largest capacity ever requested (48 MB after an 8M single-threaded call), the same order as the existing buffer pool (`make probebench`, FINDINGS §3o). Trace: serial work before the workers ~2.0 → ~0.9 ms per parallel call, the scatter-to-leaves step 0.8–1.8 → 0.09 ms | `sample_read`, `sample_sort`, `ctx_init` |
| D33 | **Sample once, decide once** (`-DLOGOS_SODO`): one two-stage sorted sample (2048 Weyl points for every gate, extended to 5376 only for the partition) carrying distinct count, Chao1 true-cardinality estimate, NaN, a Fibonacci-lag inversion rate and the KS certificate; counting routes attempted only when Chao1 fits their cap; the partition reuses the sample | SHIPPED as the default (-DLOGOS_NO_SODO restores the per-gate samples): no loss on any shape once the two-stage draw was in, 1.07-1.39x on zipf and 1.07-1.22x on the 1M rows of the partition shapes, neutral at 8M; every run of the three was on a loaded box, ratios within rows only (`make sodobench`, FINDINGS §3n). A full 5376 draw up front lost 5–16% on the cheapest routes at 1M; the two-stage draw removed that | `sodo_sample`, `sodo_extend` |
| D32 | **Histogram-free partition** (sample-sized regions with 60% slack, 256-element block reservation through a per-thread write-combining stage, spill list merged at the leaf, decline above n/16 spills) and the **uniformity certificate** (KS distance of the sorted sample from the straight line ≤ 2/√m ⇒ equi-width grid, no tables) | MEASURED (`make hfbench`, 4 engines, P and P/2): uniform 1.31×, int_wide 1.41×, int_1e6 1.45×, jitter_ms 1.24×, logistic 1.20×, clustered 1.14×, heavy_tail/bimodal 1.01–1.15×; other routes identical. The certificate alone: uniform 22.6 → 20.4, int_wide 22.3 → 17.5 ms | `hf_partition`, `hf_scatter`, `hf_leaves`, `ks_ok`; old path `-DLOGOS_NO_HISTFREE` |
| D31 | **Inherited constants swept** (14 knobs × 4 values, 19 shapes, `make knobbench`): run-merge gate 256 → 4096, new `MERGE_MAX_RUNS` = 8, `LOWCARD_CAP` 1024 → 256; eleven kept | MEASURED: sawtooth 1T 553 → 138 ms, 64 overlapping runs 283 → 116, zipf 1.44× / 1.3–1.5×; every kept value has a recorded reason (section N) | `MIN_AVG_RUN_FOR_MERGE`, `MERGE_MAX_RUNS`, `LOWCARD_CAP` |
| D30 | **Per-route thread caps**: relocation ≤ one thread per 500k, counting routes ≤ one per 350k with a floor of 3; global policy unchanged | MEASURED (four engines interleaved, P-call, 100k–8M): block_perm 1M 1.31×, 3.16M 1.69×; few_distinct 1M 1.68×; quantized 1M 1.28×; mod_card_30k 1M 1.38×, 3.16M 1.27×; nothing else outside its band; 8M identical code. Residual: block shapes at 316k ~0.1 ms behind one thread (entry setup) | `RELOC_MIN_PER_THREAD`, `COUNT_MIN_PER_THREAD`, `count_threads` |
| D28 | **Band-first near-sorted route** (thresholds = window medians at chunk boundaries; one greedy keep/reject pass per chunk with poison recovery leaves a sorted run and strays; strays sorted once, cut by the thresholds; one two-way merge per band) | MEASURED (`make nsbench`, 4 engines, P and P/2): near_sorted_int 2.27–2.85× vs shipped, near_sorted f64 1.42–2.05×, local_shuffle 1.48–1.50×, 0.1%-swapped 1.03–1.21×, guards 0.93–1.07×; near_sorted 14T 20 → 10–14 ms, past IPS⁴o's 17.5 | `nearsorted_parallel_f64`, `ns_split_sort`, `ns_merge2`; old route `-DLOGOS_NS_OLD` |
| D29 | **Bounded insertion for nearly-sorted small ranges** (momentum insertion, 8n move budget, falls through) instead of plain insertion | MEASURED: local_shuffle 14T 93 → 17 ms, near_sorted (chaos) 294 → 14 ms whenever such buckets reach the partition; identical elsewhere | `sort_fn`, `-DLOGOS_PLAIN_INSERTION` |
| D27 | **Lattice window minimum size**: the moderate-cardinality path's direct-index counter sized its window from the sample plus an eighth and aborted on the first out-of-window value, redoing 19–44% of the array in the hash path (Collatz: sample max 444, true max 664; 5–12 ms wasted at 1T). Tables under 4096 slots are now widened to 4096 | MEASURED (`make latbench`): collatz 1.31× at 1T, 1.40× at 14T; 10 other shapes identical inside their bands. A proportional margin was rejected first (quantized 0.87× at 14T: its table tripled). **General:** a sample bounds the bulk, never the tail; a path that aborts on an out-of-sample value must spill or size for the tail cheaply | `LATTICE_MIN_SLOTS`, `lattice_try_f64` |
| D24 | **Low-discrepancy window probe** (two contiguous 64-windows, max/min sorted gap ≤ 4 ⇒ flash/coarse, not the AVX2 leaf) | MEASURED: weyl 1.25×, van der Corput 1.29× at 1T; never fires on the other 13 shapes | `LOWDISC_GAP_RATIO`, `ctx->lowdisc` |
| D25 | **Per-mode splitter tables** (splitter gaps > 10× median define ≤ 8 modes; one value table per mode; fifth candidate in the histogram trial) | MEASURED: bimodal 14T 22.8 → 19.9 ms (1.15×), level with x86-simd-sort on the one parallel row it led; absent on single-mode data | `bt_lookup_d`, `BT_MODE_GAP` |
| D26 | The chaos zoo as a bench (15 shapes: guards, logistic map, Lorenz-x, Collatz, prime gaps, Weyl, van der Corput, Benford, Brownian, Zipf, jittered lattice, stride sawtooth) | BUILT; found two aliasing failures the nine standard shapes never showed, and one open dispatcher gap (sawtooth at 1T runs the k-way merge, 545 ms) | `chaosbench.c` |
| D21 | **AVX2 vectorized leaf for sequential sorts** (in-house x86-simd-sort design: permute-compress partition with plain stores, 8-vector end-choice, streaming small partition, generated 32/64 bitonic networks; unstructured leaf only, sequential only, not when nearly sorted) | MEASURED (`make simdbench`, 4 engines interleaved, 8M): 1T uniform 1.20×, heavy_tail 1.48×, bimodal 1.59×, clustered 1.19×, negatives 1.26×; structured shapes and all 14T rows identical code; whole-array 8M at 1T 92–104 ms vs x86-simd-sort 79 | `avx2_qsort_f64`, `AVX2_TAKE`, `gen_avx2_leaf.py`, FINDINGS §3g |
| D22 | The first AVX2 cut was 2.3× slower than the scalar leaf; three probe-driven fixes made it 1.2–1.6× faster: unroll the end-choice (1.41→0.39 ns/elem per pass), straight-line networks (gcc unrolled 188 of ~670 ops), streaming partition under 512 elements (1.23→0.71 ns/elem) | MEASURED (component probe) | the ledger's "SIMD bolt-ons lose" was about *where* the SIMD ran, not whether it could win; per-component timing found each defect in minutes |
| D20 | Textbook baselines in every bench: LSD radix (11-bit, 6 passes, digit skip) and CPython-style timsort, single-threaded sections only | MEASURED (14T run, quiet box): logos 1T beats LSD radix on 8/9 shapes (1.2× heavy_tail, 1.5× uniform, 11× blocks), loses bimodal 0.87×; timsort 0.55–0.77× std::sort on random data, 12× on blocks; 360 qsort checks | `bench_sorts.h`, FINDINGS §1d |

## E. The thesis itself, graded

| # | claim | status | reading |
|---|---|---|---|
| E1 | "Addressing beats comparing" vs scalar comparison sorts | MEASURED true | A1–A3 |
| E2 | "Addressing beats comparing" vs *vectorized* comparison on unstructured CPU data | MEASURED false | A4–A5, B7: the scatter is the op SIMD avoids and it does not vectorize |
| E3 | "Addressing beats comparing" when it avoids both comparing *and* scattering | MEASURED true | A6: counting / lattice paths, the defensible niche |
| E4 | Structure detection is cheap enough to always run | MEASURED | probes are O(sample) or O(cap) with abort budgets; no shape regressed |
| E5 | "Distribution-awareness as the product" beats state of the art | INFERRED, baselines unfair | paradigm #3 baselines were sort-first and `nth_element`; a fair querybench (radix-select, streaming heap) is still owed. The safe claim is "sort-first is the wrong reflex", not "beats SOTA" |
| E6 | The parallel-SIMD-leaf could win on many cores (PARITY.md, written before it was run) | FALSE, superseded | A9 |

## F. Correctness and feature envelope (C flagship)

| # | fact | status |
|---|---|---|
| F1 | Types: f64 and i32 only; no i64/u64/f32/u32; no key-payload pair sort | known |
| F2 | Not stable (argSort is stable); NaN ordering unspecified; ±0 kept distinct and bit-exact on counting paths | known |
| F3 | Output identical for any thread count | MEASURED at 1/2/4/8 |
| F4 | Scratch ≈9n bytes f64, ≈5n i32; in-place variants ≈1n, sequential only | known |
| F5 | Worst case bounded: coarse depth cap → heapsort; dual-pivot introsort; `qsort` on allocation failure | known |
| F6 | Every speculative path has an abort budget: momentum (8n displacement), lattice (second distinct value on a slot), modcard (65 536 cap), lowcard (1024 cap), near-sorted (inversion sample) | known |
| F7 | Cardinality coverage: ≤1024 lowcard, ≤65 536 modcard/lattice, above that the full unstructured path | known; **65k–~1M distinct is a measured gap in no table** |
| F8 | 685 harness checks, 18 shapes, threads 1/2/4/8; parbench verifies every output vs `std::sort` | MEASURED |
| F9 | Permutation indices are u32 (n < 2³²) | known |
| F10 | Rust: v2 numerics only; JS: full string engine, single-threaded; Python: reference | PARITY.md |
| F11 | No streaming sketch anywhere (no KLL, t-digest, HyperLogLog). Top-level splitters come from one strided 2560-sample; per-bucket classes are equi-width over a prescanned min/max; probes are fixed samples or capped passes | known |
| F12 | Boundaries are guessed once and never narrowed: the histogram computes exact per-bucket counts but uses them only for offsets, never to re-balance | known; the regroup avenue (J16) is the fix |

## G. Repository state (facts about the tree, not the algorithm)

| # | fact |
|---|---|
| G1 | The workspace root holds a second copy: CLAUDE.md, FINDINGS.md, README.md are byte-identical to the nested logos-sort-repo/ versions, and the root experiments/paradigm/ is empty. The nested directory is the real project. |
| G2 | Not a git repository; no history to check claims against. |
| G3 | `LOGOS_PSORT_VERSION` is 3.6.0 while CHANGELOG carries five "(unreleased)" sections with shipped additions (extras, in-place, SIMD hooks, IPS4o). Real features are unversioned. |
| G4 | Build is POSIX (pthreads, make, g++ + OpenMP); on this Windows box that means WSL2. The AVX-512 kernels are pragma-pinned for WSL2. |
| G5 | Opponents (x86-simd-sort, IPS4o, TBB) are external clones, not vendored. |

---

## H. Avenues to explore (ranked; each stated as a prediction to be graded)

Every avenue below is outside the ledger or attacks it from a new angle. Each carries the
prediction written *before* measurement, per the methodology, and the A/B that would settle it.
Rank = expected gain × confidence ÷ cost.

### H0. Per-phase profile of the engine — DONE 2026-09-10 (`make profile`)
**Built:** `-DLOGOS_PROFILE` accounting per phase and per thread (CPU received, preempted,
barrier-stalled, idle, leaf path per bucket) and `profbench` → profile-report.html with treemaps.
**Prediction graded:** wrong in the useful direction. The 1T route never partitions, so the
"scatter vs leaf" split I predicted does not exist there (uniform 1T is 100% coarse leaf). At 20T
the leaf share is 25–34%, but the phase I did not name at all, the **top-level histogram, is
49–70% of wall and ~9× the scatter in CPU** (B13). J14 below, which I had ranked last at "≤2%",
is therefore the top avenue.

### H0b. Direct-index splitter table for `bucket_of` — SHIPPED 2026-09-10
**Measured in the engine** (`make stbench`, interleaved, best of 5, 8M, loaded box), end to end:

| shape | 20 threads | 10 threads |
|---|---|---|
| uniform | 1.98× | 1.72× |
| heavy_tail | 2.03× | 2.63× |
| bimodal | 1.58× | 1.21× |
| int_wide | 2.99× | 1.35× |
| clustered | 1.89× | 2.12× |
| negatives | 2.13× | 1.97× |
| near_sorted, quantized, few_distinct, block_perm (never partition) | 0.88–1.20× | noise at 11–21 ms |

Prediction below (1.4–1.9×) graded: right on direction and on the guards, low on size. 685 harness
checks pass; every A/B output verified. Each thread trials the value- and key-space tables on its
first two chunks and commits to the faster (J15). Next lever on this phase: a branchless lookup
for alternating clusters (bimodal is the smallest gain).
**Consequence for the headline claims:** A2 and A5 were measured before this change. The 20-thread
gap to x86-simd-sort on unstructured shapes was 1.5–2×; this change is 1.6–3× on those rows. The
SIMD comparison must be re-run (`make parbench-simd`) before the "loses on every unstructured shape"
sentence is repeated; the box here has no x86-simd-sort checkout.

(original entry follows)
**Idea:** the histogram computes each element's bucket with a 7-step dependent-load binary search
over 79 splitters. Build once, from the sorted sample, a 4096-cell equi-width table over
[min,max] (or over the high bits of the order-preserving key for skewed data) whose cell holds the
bucket when the cell lies inside one bucket, else a two-entry range to resolve with one compare.
Common case: one multiply, one load.
**Prediction:** histogram CPU ~100 → ~10 ns/element; uniform 20T 78 → ~40 ms on this box; every
unstructured parbench MULTI row 1.5–2×; few_distinct/quantized/block rows unchanged (they never
reach the partition).
**Crux measured (experiments/paradigm/splitter_crux.cpp, 1T, ids verified identical):** engine
search 44–62 ns; value-space table 2.7–5.1 ns on uniform / int_wide / clustered but 25–26 on
heavy_tail / bimodal; key-space table 11.6 / 17.5 on those two; a pass that times both tables on
1/32 of the data and runs the rest on the winner lands within 10% of the best on every shape,
3–10× faster than the search, and never chose wrong. Lookup side of the prediction: confirmed.
Remaining: integrate (A/B), branchless lookup for the alternating-cluster case. See
experiments/paradigm/SELF_SAMPLING.md.
**A/B:** pfbench pattern; then `make profile` again and read the histogram row.
**Cost:** low.

### H1. Partition-then-count: extend the zero-scatter niche to 65k–~1M distinct
**Idea:** the counting paths (D2, D3) are tried only on the whole array and abort at 65 536
distinct. After the top-level quantile partition each of the P×4 buckets holds ~n/80 elements and
~1/80 of the distinct values, so a bucket of 8M/500k-distinct data has ~6k distinct in 100k
elements: squarely inside `modcard`'s winning regime. Run the capped hash-count *per bucket* inside
the leaf, gated by the bucket's own 2048-sample (at 6k distinct that sample shows ~17% repeats;
the whole-array sample showed ~0.2% and correctly declined).
**Why it is new:** the ledger has no result above 65k distinct; F7 names the gap; no code path
exists. It composes two existing pieces, no new pass.
**Prediction:** new shape `card_500k` (8M, 500k distinct): 1T from ~120 ms to 60–70 ms;
20T from ~43 ms to 8–12 ms, which would beat x86-simd-sort MT (~34 ms on such data) by ~3×.
Uniform unchanged within noise (the per-bucket gate declines in O(sample)).
**A/B:** pfbench pattern, engine with and without `-DLOGOS_BUCKET_MODCARD`, shapes uniform,
card_500k, card_2M, quantized. Grade the 1T/20T numbers above.
**Cost:** low. Highest value: it widens the only band where logos beats the SIMD sort.

### H2. Exception extraction for near-sorted data ("sort the displaced set")
**Idea:** near_sorted is n/100 random swaps, so ~2% of elements are displaced and 98% already
form a sorted sequence. One greedy pass keeps a non-decreasing *spine* (append if ≥ last; on a dip,
decide whether the new element or the spine's tail is the intruder by peeking one ahead, with a
short pop-back cascade so the spine is sorted by construction) and compacts the intruders into an
exception list. Sort the exceptions with the engine (tiny), then one galloping two-stream merge of
spine + exceptions back into `a[]`. Every pass is sequential; the only comparison work is over the
2%. Abort budget: if exceptions exceed ~5% of n, fall back to the existing path (same shape as the
momentum displacement budget). Parallel: chunk the extraction, fix chunk boundaries, band-merge as
D4 already does.
**Why it is new:** C5 tuned the merge *kernel*; D4 changed the partition. Neither reduces the
element count that goes through comparison. The current path (runs bail because avg run ≈100 <
256; momentum declines) ends in the coarse scatter, which is only fast here because writes happen
to be near-sequential.
**Prediction:** near_sorted 1T 37 → 12–18 ms; 20T 22.5 → 5–8 ms (a clear win over x86-simd-sort's
24.8, today a tie). segmented / few_runs / prefix_shuffle unchanged (run-merge still catches them).
**A/B:** interleaved, shapes near_sorted at swap rates 0.1%, 1%, 5%, 10% to find the crossover
and confirm the abort budget works.
**Cost:** medium. New algorithm, ~150 lines, needs its own harness shapes at several swap rates.

### H3. Histogram-only exact select / quantiles for the query API
**Idea:** the top-level sample-splitter histogram is one *read* pass with no scatter (B2 says the
scatter is cheap, but a histogram is cheaper still). For `select(k)` or a set of quantiles, that
one pass locates the bucket holding each requested rank; then scatter *only those buckets* (1/80 of
n each) and sort them. For skewed data where a bucket is huge, recurse on it: this is MSD radix
select on the order-preserving key, exact, O(n) with 2–3 read passes.
**Why it is new:** paradigm #3's quantile result is *approximate* (0.4–6.8% error) and its
baseline was a full sort; CLAUDE.md itself asks for a fair `querybench` vs `nth_element`-class
baselines. This gives exact answers from the machinery that already exists in `psort_f64`.
**Prediction:** exact p50/p95/p99 on 8M in 8–12 ms 1T (vs 100 ms sort-first and 69.5 ms
`nth_element` for one rank), ~2 ms at 20T. That is 8–12× 1T exact, versus the current 87×
approximate, and it stays a win against radix-select because the splitters come from a sample
rather than fixed digits.
**A/B:** new `querybench`: `std::nth_element`, a straight MSD radix-select, the sampled profile,
and this; shapes uniform / heavy_tail / few_distinct; report exactness.
**Cost:** low–medium; this is the first concrete entry point of open direction 1.

### H4. Equi-depth coarse bucketing from the sample CDF (one path for value-coarse and bits-coarse)
**Idea:** coarse bucketing is equi-width on the value; heavy tails route to a separate exponent-bit
path. Instead, index a 4096-cell table by the high bits of the order-preserving key (so
log-magnitude resolution is free) and store per cell an equi-depth slot base and scale derived
from the 64-sample the coarse path already takes. Slot = base[cell] + (v − cell_lo)·scale[cell]:
one lookup, one multiply, balanced buckets on any distribution, and the skew probe plus
bits-coarse become one code path.
**Why it is new:** flash and coarse are equi-width; bits-coarse is exponent-keyed; C8 changed K,
not the slot *function*. Equi-depth from a sample is the classic fix for the skewed-bucket
imbalance that makes heavy_tail and bimodal 15–20% slower than uniform at 20T.
**Prediction:** heavy_tail 20T 71.7 → ~60 ms and bimodal 68.7 → ~60 ms (converging on uniform's
60.5 in the same run); uniform within ±3%; quantized/few_distinct untouched (they exit earlier).
**A/B:** interleaved coarse-slot variants; grade on those three rows.
**Cost:** medium. Risk: the table build per recursion level is a new fixed cost (B4); keep it
to the top coarse level only.

### H5. Bucket-relative key narrowing in the integer paths
**Idea:** after the quantile partition each bucket's key range is bounded by its splitters, so
~26 of 32 bits are live and the bucket (~100k × 4–8 B) is L2-resident. Subtract the bucket
minimum and radix-sort the narrowed key in 2 passes of 13 bits (8192-entry histogram, 32 KB)
instead of 4 byte passes, choosing digit width from `log2(range)`. The same key-range fact lets the
f64 path skip the OR/AND probe.
**Why it is new:** C13's "wider radix digits" was rejected whole-array on one core, where a
2048-entry histogram thrashed; per L2-resident bucket the trade inverts. The ledger never tested
digit width *after* partition.
**Prediction:** int32 20T 34 → ~26 ms; int_wide 38.5 → ~30 ms (−20%); 1T similar on the leaf
share; f64 shapes unchanged.
**A/B:** pfbench with `-DRADIX_NARROW`; int32, int_wide, uniform.
**Cost:** low.

### H6. Branchless / SIMD sorting network at the ≤64-element leaf
**Idea:** the one part of logos that is compute-bound (B10) is the insertion sort on ~50-element
final buckets: ~8M/50 = 160k calls, each ~n²/4 predictable moves plus one mispredict per element.
Replace with a bitonic network (8 AVX-512 registers hold 64 doubles; x86-simd-sort's `sort_n`
templates already implement it) or, portably, a cmov-based network for ≤16 and a merge.
**Why it is new:** every SIMD attempt in the ledger (C1–C4, C9) targeted a memory-bound pass or
a whole-bucket hand-off. This targets the in-L1 leaf, where B8's argument does not apply. H0
decides whether the leaf share justifies it.
**Prediction:** uniform 1T −10 to −15%; 20T −5%; nothing on the counting paths. If H0 shows the
leaf under 10% at 1T, skip.
**A/B:** interleaved leaf variants, uniform / heavy_tail / int_wide.
**Cost:** low with x86-simd-sort at hand; medium portable.
**Update 2026-09-10 (after H0b and `make leafbench`, C17):** the leaf is now the largest parallel
phase (40–56% of wall). On a ~100k bucket it is flash sort: equi-width classes with m = n, a
scatter, and one insertion pass that repairs local disorder at a cost ∝ n·∫f² over the bucket
(Devroye, L2). Changing the class count or routing to coarse did not help universally, which
localises the cost in the repair pass and the per-bucket probes. Three levers, in order:
1. *Equi-depth classes gated by an ∫f² estimate.* The bucket's existing 64-sample gives the
   estimate (sum of squared cell occupancies); flat → keep the multiply; skewed → classify by a
   64-segment piecewise-linear CDF. Prediction: heavy_tail and bimodal leaf −20 to −30%, uniform ±2%.
2. *Line-granular branchless repair.* Load eight doubles, one shifted SIMD compare + movemask;
   if the line is in order and its first element ≥ the previous line's last, skip it; else fix
   the line with a small network. Flash output makes the skip the common case. Prediction: leaf
   −10 to −15% on every shape.
3. *Drop the per-bucket probes* (J2): prescan, run detection, inversion and gap samples re-derive
   what the histogram and the splitters already know. Prediction: leaf −10%.
Each is a day with the interleaved A/B; do 1 first because it is the one Devroye's model
predicts most confidently.
**Lever 2 BUILT, measured mixed over three runs, kept opt-in** (`-DLOGOS_LINE_REPAIR`:
branch-free 8-element network per line, then a predictable insertion pass). Run 2: within ±5%
everywhere; run 3: +35% heavy_tail, +40% bimodal, −4% uniform, all at 20T. The mechanism argument
predicted a small universal gain; the data says shape-dependent and noisy. A quiet-box A/B decides
it. Levers 1 and 3 remain open.

### H7. One global decision, one fused two-level pass (open direction 7, concretised)
**Idea:** the engine already sorts a 2560-sample at the top level, which can answer skew,
multimodality and quantisation for the whole array. Decide the per-bucket path *once* from it,
and let the top histogram also count each element's coarse slot (P×4 × 2048 counters per thread,
640 KB, L2-resident), so each bucket's leaf skips its own prescan and histogram and scatters
straight into coarse slots. Removes two of the ~eight memory touches per element without adding
one, which is the only kind of change B4 allows.
**Why it is new:** C8 tested *more* levels; this is *fewer passes at the same levels*. Not a
merge, not SIMD, not a new K.
**Prediction:** unstructured shapes 20T −10 to −15%; 1T −8 to −12%. Risk that the 640 KB counter
array slows the top histogram enough to cancel it; H0 gives the budget.
**A/B:** interleaved; all seven shapes, because the global decision can misroute a bucket whose
local distribution differs from the global one (bimodal is the test).
**Cost:** high; restructures dispatch. Do after H0, H1, H5.

### H8. Huge pages for the scratch pool
**Idea:** `madvise(MADV_HUGEPAGE)` on the persistent pool (and the bucket-index cache).
**Why it is new:** B5 measured the page-fault floor but no one tried to lower it; TLB reach for
2048 write streams was never examined.
**Prediction:** ≤5% on the first call per size, ~0 once the pool is warm; unclear under WSL2.
Low priority; listed because it is a one-line experiment.
**Cost:** trivial.

### H9. Structure router (open direction 2)
**Idea:** probe as today; route counting/lattice/near-sorted shapes to logos, everything else to
x86-simd-sort. Wins every parbench row by construction minus ~1–2 ms of probe.
**Why it is here:** not algorithmic, but it is the only way to a table with no losses, and it
settles the product question (engine vs router). A11 matters: on AVX2-only hardware the router
needs x86-simd-sort's AVX2 kernels or Highway's vqsort, and the gap it routes around may be smaller.
**Prediction:** per row, min(logos, x86simd) + 1–2 ms.
**Cost:** low; mostly build plumbing.

### H10. Direct key-payload pair sort
**Idea:** sorting records today is argSort (8-pass index-carrying radix) plus `apply_perm`, a
random gather that the paradigm experiment measured at 52.8 ms per 8M, the single most expensive
operation recorded in the repo. Scattering (key, payload) as 16-byte elements through the existing
top partition and leaf doubles bandwidth but removes the gather entirely.
**Why it is new:** F1; no pair path exists in any engine.
**Prediction:** sort-records-by-f64-key end-to-end 1.5–2× faster than argSort + apply at 8M.
**Cost:** medium (element-size templating of the scatter).

### Closed — do not pursue (pointer only)
Vectorising scatter/histogram/bucket-index; k-way merge instead of scatter; smaller cache-resident
COARSE_K; SIMD leaf per bucket in parallel; winner-tree merge; shared context pool; golden-ratio
pivot. Details in section C and CLAUDE.md. The only remaining angles on the single-threaded
unstructured gap are H6 (compute-bound leaf) and H7 (fewer passes); everything else in that frame
has been measured closed.

---

## I. Suggested order

0. H0 profile — done; `make profile`. It reordered this list.
1. H0b direct-index splitter table — done; measured 1.6–3× at 20T on every partitioning shape.
   Re-run `make parbench-simd` on a box with x86-simd-sort before repeating A5.
2. H1 partition-then-count (days) → widens the defensible niche; add `card_500k` / `card_2M` shapes.
3. H3 exact select / quantiles + fair `querybench` (days) → first real entry point of the
   order/analytics API, with the honest baseline E5 asks for.
4. H2 exception extraction (days) → turns the near-sorted tie into a win.
5. H5, H4 (days each) → the int and skewed-shape rows.
6. H9 router (day) → the no-loss table.
7. H7, H10, H8 as capacity allows.

Housekeeping first: fold the duplicated root docs into the nested project (G1), put the tree
under git (G2), and cut a 3.7.0 so the shipped extras stop being "(unreleased)" (G3).

---

## J. Speculative avenues (brainstorm; none measured, none in the ledger)

These are guesses, ranked by plausibility, each checked against the cost model (B1–B4: latency-bound
scatter, pass overhead, comparison is the expensive op) so that none repeats a closed result.
Treat every prediction as a bet to be graded, not a claim.

### Tier 1 — cheap, likely real, an afternoon each

### J1. Fold NaN detection into the prescan (remove a whole pass on the slow rows)
**Idea:** `sort_fn` and `leaf_f64` run a separate full `has_nan` scan before bits-coarse, which
is exactly the route heavy_tail and bimodal take. The prescan already touches every element; add
`v != v` there and drop the extra pass.
**Prediction:** heavy_tail and bimodal 1T −3 to −5%; nothing elsewhere. Pure pass removal (B4).

### J2. Piggyback per-bucket statistics on the top histogram
**Idea:** while the histogram pass computes each element's bucket it can also accumulate the
bucket's min, max, all-integer flag and an OR/AND of key bytes at ~zero cost. The per-bucket
leaf then skips its own prescan and the quant probe. A separable, low-risk slice of H7.
**Prediction:** unstructured shapes 20T −4 to −7%.

### J3. Non-temporal streaming stores in the top-level scatter and copyback
**Idea:** the P×4-stream top scatter writes lines that will not be re-read until the bucket sort;
`movnt` stores skip the read-for-ownership and halve that pass's traffic.
**Prediction:** small, ≤3% at 20T, because B1 says bandwidth is not the floor; listed because the
write-combining gate already exists and this is one intrinsic behind it.

### J4. Reuse the top-level sample inside the buckets
**Idea:** the coarse path takes a fresh 64-sample per bucket for gap detection; the top-level
2560-sample is already sorted and contains ~32 points per bucket. Pass the slice down.
**Prediction:** noise on 8M; measurable at n ≈ 100k–1M where fixed costs dominate.

### Tier 2 — real algorithms, a week each

### J5. Disjoint-run relocation for block-permuted data — IMPLEMENTED, MEASURED 2026-09-10
**Idea:** sorted blocks in shuffled order are runs whose value ranges do not overlap. Sort the
*run descriptors* (first value, start, length), group overlapping runs into chains, relocate
whole blocks with `memcpy`, merge only inside multi-run chains.
**Correction to the original note:** the harness's `segmented` and `few_runs` shapes are
*interleaved* runs (every block spans the whole range), which is this path's worst case. The
target shapes are new: `block_perm` and `block_overlap`.
**Prediction (written first):** 8M, 64 blocks: 1T 2.5–4×, 20T 1.5–2.5×; guards within ±3%;
risk row `few_runs`.
**Measured** (`make j5bench`, interleaved, best of 5, calibration ~1.8× off quiet):

| shape | 1T | 20T |
|---|---|---|
| block_perm64 | 2.82× | 2.03× |
| block_perm1k | 3.39× | 2.18× |
| block_overlap (25% of data in 2-run chains) | 2.34× | 1.89× |
| segmented / few_runs / near_sorted / uniform | 0.96–0.99× | 0.95–1.05× |

**What it took:** the first cut never fired (0.99×): ascending block boundaries fuse two blocks
into one run whose interval hides a hole, so a third block reads as an overlap. Runs are now
closed at any jump over 1024× their mean step. The second cut lost 16% on `segmented` at 20T
from a sequential probe and 19 spawn-then-bail threads; the probe is now chunk-parallel with
the caller scanning chunk 0 first and a 1024-sample pre-gate. Both are in the ledger (C15, C16).
**Status:** shipped in engines/c behind `-DLOGOS_NO_RELOCATE` for the A/B; 685 checks pass.

### J6. Bounded-displacement sort for jittered monotone data
**Idea:** timestamps and sensor streams are sorted except that each element sits within some
window d of its final place. A single pass with a d-sized heap (or a 64-wide sorting network
sliding over the array) sorts in O(n log d) with sequential access and no scatter. Detect d from the
inversion sample (distance between the two compared elements); abort if the heap ever exceeds 4d.
**Prediction:** a new `jitter_64` shape (8M, local shuffles within 64) 1T under 20 ms versus the
coarse path's ~37 ms; near_sorted (global random swaps) unaffected, since it aborts.

### J7. Heavy-hitter split at the top level (Zipfian data)
**Idea:** low-cardinality counting gives up above 1024 distinct, but Zipfian data has a few values
that each cover a big share of n plus a long tail of rare ones. From the 2560-sample, any value
appearing ≥ n/(4·nbk) times is a heavy hitter: count those (no scatter, they go straight to their
own output range) and scatter only the tail. A hybrid between D2 and the partition.
**Prediction:** a `zipf_ids` shape (8M, s = 1.1, 1M distinct) 20T −30 to −40%; uniform unchanged.

### J8. Single-pass predicted placement with a spill bucket (Learned-Sort style)
**Idea:** the sorted sample defines a piecewise-linear CDF accurate to about ±n/2560. Place every
element directly at `CDF(v)·n` in the output with a small slack per cell, overflow into a spill
bucket, then one linear touch-up (insertion within the slack) and one merge of the spill. Two
memory passes instead of top scatter + coarse scatter + leaf. Kristo et al. reported 1.3–3× over
radix sort with this shape of algorithm.
**Prediction:** honest odds are even: it removes one scatter (good, B2) but the touch-up is
comparison work over n (bad, B3). If the sample CDF error stays below the cell slack it wins by
10–20% at 1T; if not it loses. Worth a crux test like `merge_crux.cpp` before building it.

### J9. Lazy bucket materialisation (sort on read)
**Idea:** run only the top-level partition (~16 ms at 1T, the cheap op) and return a handle; each
bucket is sorted the first time a reader touches its range. Pagination, top-k, and "show me the
first screen" workloads pay for 1/80 of the sort.
**Prediction:** first-page latency 8M → under 20 ms 1T; full-materialisation cost unchanged.
This is the paradigm result (D11) applied to the array itself.

### J10. Parallel strict-in-place: sort the 256 top-byte buckets across threads
**Idea:** the ~1n in-place variant is sequential; its MSD first level yields 256 independent
buckets. Hand them to the thread pool with the existing atomic work queue (the CHANGELOG names
this as the natural next step; nobody has done it).
**Prediction:** in-place 20T within 2× of the buffered sort, up from 1.6–6× slower; raises the RAM
ceiling for 1e9-element sorts at usable speed.

### Tier 3 — moonshots

### J11. f32 and i64 engines
**Idea:** f32 halves every byte count and the lattice/quantised paths fire far more often on sensor
data; i64 covers timestamps and IDs. New types, same engine, new shapes for the harness.
**Prediction:** f32 uniform ≈ 0.6× of f64 time; the counting-path wins widen because more real
f32 data is quantised.

### J12. Warm-start dispatcher across calls
**Idea:** repeated sorts of the same source (streaming batches) rediscover the same distribution.
Cache the last splitters, lattice base/step and cardinality verdict keyed by (n, min, max, sample
hash); skip the probes when the fingerprint matches.
**Prediction:** invisible at 8M (probes are ~1–2 ms); 10–20% at n ≈ 64k–256k in a tight loop.

### J13. Sort-free distinct / group-by as first-class API
**Idea:** `modcard`'s hash pass already produces (value, count) with zero scatter and abandons it
by regenerating runs. Expose `distinct`, `group_count` and `mode` directly; extend the cap by
partition-then-count (H1).
**Prediction:** group-by on 8M/500k-distinct in ~15 ms 20T versus sort + run-length ≈ 45 ms.

### J14. Interpolation instead of binary search over the splitters — PREDICTION FALSIFIED, PROMOTED
**Idea:** `bucket_of` does a 7-step branchless binary search over 79 splitters per element in the
histogram pass (the scatter reads the cached index). An interpolation step from the sample CDF
reaches the right splitter in 1–2 probes on smooth data.
**Original prediction:** histogram pass −20% in isolation, ≤2% end-to-end; probably noise.
**What the profiler measured:** the histogram is 49–70% of the 20-thread wall and ~9× the scatter
in CPU. The "≤2%" guess assumed the histogram was a cheap streaming pass; it is the bottleneck.
This is now H0b above, with a new prediction of 1.5–2× on every unstructured parallel row.

### J15. The algorithm samples itself: trial kernels on the first chunks, commit for the rest — CRUX MEASURED
**Idea:** a pass that must touch every element is already a sample of the data and of the
machine. Run two candidate kernels on the first chunks (or the exact kernel while accumulating a
cheap statistic), decide once, run the rest on the winner behind a guard. Four uses in this
engine: kernel selection by trial (value- vs key-space bucket table, write-combining, prefetch
distance), over-partition-then-regroup for bucket balance, probes piggybacked on chunk 0 of the
histogram (replacing three probe passes and closing F7), and a measured guard for learned placement.
**Where it does not help:** building the bucket table from first-half hit counts (crux variant c):
the splitters are quantiles, so the table already follows data density; sampling adds pure cost.
**Crux (splitter_crux.cpp):** the self-choosing lookup is within 10% of the best fixed table on
all five shapes and 3–10× faster than the shipped search; details in H0b and
experiments/paradigm/SELF_SAMPLING.md.
**Second use shipped (D16):** the same trial chunks count how many lookups resolve at the table's
root; the committed loop keeps a predictable root branch when that fraction is near 0 or 1 and goes
branch-free otherwise. Two decisions from one trial, no extra pass.
**Predictions for the other three uses (written, unmeasured):** regroup 5–8% at 20T; piggybacked
probes 3–5% on unstructured shapes plus a new counting win at 65k–1M distinct; learned placement
even odds as in J8.

### J16. Over-partition and regroup: narrow the boundaries with counts the engine already paid for
**Idea:** the histogram holds the exact per-bucket counts that the 2560-sample only estimated, and
nothing uses them. Compute 4× more fine buckets in the histogram (the splitter table makes the
extra buckets nearly free), then group adjacent fine buckets into P equal-work ranges *after* the
counts exist. Same scatter, no extra pass, no sketch: exact balance from data already in hand.
**Why:** the profile shows per-thread leaf time ranging 10–18 ms on uniform at 20T and the wall is
the maximum; the 3.6.0 bucket phase was already known to be imbalance-limited.
**Prediction:** 5–8% at 20T on every shape that partitions; nothing elsewhere.
**Cost:** low. This is the right answer to "narrow or guess": narrow with the count, not a second guess.
**SHIPPED 2026-09-10 in its cheap form:** 12 buckets per thread instead of 4 (the queue already
balances dynamically, so only the tail needed shrinking). Measured 1.08–1.44× on 9 of 12
partitioning rows, far above the prediction, because a 33k-element leaf's flash working set stays
in L2 where a 100k one spilled: a cache effect on top of balance. The regroup step proper is not
built; the cap of 256 buckets (one-byte cache) bounds how far the count can go.

### Crux tests before building (the cheap way to kill a bad idea)
(section K, below, is the answer to "where does the theory not meet the optimal algorithm")
| avenue | crux measurement |
|---|---|
| J5 | done: see J5 above (crux was skipped; the full A/B took a day and two wrong turns) |
| J6 | heap-of-64 pass over 8M jittered doubles vs coarse path, one core |
| J7 | count heavy hitters from the sample; measure scatter of the tail only vs full scatter |
| J8 | predicted-placement scatter + touch-up on uniform vs top scatter + coarse scatter, one core |
| J9 | partition-only time vs full sort; page-1 read latency |

---

## K. Where the theory does not meet the optimal algorithm

The thesis is *addressing beats comparing*: compute each element's destination from its value and
place it, instead of comparing. Below, "optimal" means the best algorithm the hardware admits for
the same input, bounded from below by measured floors on the reference box: one streaming pass
over 8M doubles (read + write, 128 MB) is the memcpy calibration, about 2.5 ms quiet; a
four-pass radix on that box would sit near 10–15 ms; x86-simd-sort's vectorised quicksort measures
60 ms at 1T; logos measures 121–145 ms at 1T and 43–60 ms at 20T. Each row names the assumption
the theory makes, what the profiler and benchmarks actually measured, what the optimal algorithm
does instead, and the avenue that closes the gap.

| # | the theory assumes | what is measured | what the optimum does | gap | closes it |
|---|---|---|---|---|---|
| K1 | **The address is free.** Destination = f(value) in O(1). | For a general distribution f is a *search*: the quantile-splitter lookup costs ~100 ns/element of CPU at 20T (B13), 9× the store it feeds. The only O(1) addresses in the engine are the lattice (exact arithmetic) and value-linear coarse slots, which is exactly where it wins. Crux: a table lookup that picks value- or key-space by trial on 1/32 of the data gives the same ids 3–10× faster (J15). | Address by table: one multiply, one load, a short fix-up only where cells straddle a boundary. Learned/sampled CDFs do this. | ~50% of parallel CPU | H0b, H4, J15 |
| K2 | **Counting/radix is O(1) passes; comparison is O(log n).** | The engine touches each element ~8 times (prescan, two probes, histogram, scatter, per-bucket prescan, coarse histogram + scatter, insertion, copyback), and B4 shows every pass carries fixed overhead. A vectorised quicksort's log n levels become cache-resident after the first three, so its DRAM traffic is about four passes, the same as a radix. | Two to four full-memory passes, everything else in cache. Fuse the probes into passes that must happen anyway. | logos 1T is ~10× the radix floor and ~2× the SIMD sort | H7, J2, J4 |
| K3 | **Comparisons are the expensive operation.** | On this CPU the expensive operations are unpredictable branches and dependent loads. A vectorised compare-and-compress does 8 comparisons per instruction with no branch; logos's histogram is a 7-deep dependent-load chain and its fine scatter a random store (B1, latency-bound). The merge crux (B3) was read as "comparison is expensive"; it measured a *latency-bound tree walk*, which is the same hardware fact. | Minimise random access and mispredicts, not comparisons. Comparison-based SIMD partitioning is a *sequential-stream* algorithm; that is why it wins. | the whole unstructured gap (A4, A5) | H6 (branchless leaf), H0b, E2 stays true |
| K4 | **Parallelism = partition value ranges once, then sort independently.** | The parallel route is a *different algorithm* bolted above the fast sequential one: it adds a histogram + scatter + copyback whose histogram alone costs more per element than the entire 1T sort (B14). Result: 20 threads give ~3× over 1 (B14); `__gnu_parallel::sort` gets ~7× from the same cores. | Parallelise the passes the sequential algorithm already makes (per-thread histograms, one prefix scan, per-thread scatter, as the JS prototype's LSD design does), or partition by key bits where the address is O(1). | parallel efficiency ~15% | H0b, then a parallel coarse pass |
| K5 | **The leaf is negligible** (tiny buckets, insertion sort). | Leaf sorts are 25–34% of parallel CPU (~40 ns/element) and 100% of the 1T route, which never partitions. Insertion sort is comparison-based and branchy; the theory's "no comparisons" holds above the leaf, not in it. | A branchless sorting network for ≤64 elements (~4× fewer cycles), or a second in-register counting level. | ~25% of parallel CPU, more at 1T | H6 |
| K6 | **Structure detection generalises**: interpolation bucketing extends counting's zero-movement win to continuous data. | The zero-movement wins (D1–D3) come from *regenerating* output from (value, count): no element moves. Interpolation bucketing still moves every element at least twice; it inherits the address, not the zero movement. The thesis's strong form holds only when entropy is low. | For high-entropy keys every algorithm moves data at least once; the optimum minimises *how many times and in what pattern* (K2, K3), it cannot avoid it. | none to close; this bounds the niche honestly (E3) | H1 widens the low-entropy band |
| K7 | **A cheap probe routes correctly.** | The router has abort budgets but no regret bound and no measurement of misroutes: 65k–1M distinct falls through to the full sort (F7); J5's first gate rejected every valid input (C15); the near-sorted gate declines block-permuted data. The profiler now records the route per call, so misroute rates on mixed shapes are measurable for the first time. | An adaptive sort with bounded regret: detection cost proportional to the win it can unlock, and a fallback whose cost is never worse than the plain path by more than the probe. | unmeasured | route audit via `make profile` on mixed shapes; H1 |
| K8 | **The engine is memory-bound, so vectorising arithmetic cannot help** (B8). | True for the scatter and the byte-skip; false for the histogram and the leaf, which together are ~90% of parallel CPU and are compute-bound (≤4% preempted, far below streaming bandwidth). The ledger's SIMD negatives were on the memory-bound loops. | Vectorise or de-branch the compute-bound phases; leave the memory-bound ones alone. | ~90% of parallel CPU is in compute-bound phases | H0b, H6 (new angle on C1–C4, not a re-run) |
| K9 | **The sorted array is the deliverable.** | Where the deliverable is order or an aggregate, the movement disappears and the model wins 7–87× (D11), with unfair baselines noted (E5). | Never materialise what the query does not need. | the largest measured win in the project | H3, J9, J13 |

**Reading across the rows.** The theory is right about *what* to avoid (data-dependent
comparison trees) and wrong about *why it is expensive*: the cost on this hardware is random access
and dependent loads, not the comparison instruction. logos avoids comparisons by introducing an
address computation that is itself a dependent-load search (K1) plus a random-access scatter (K3),
and it pays for structure detection on every input (K2, K7). The optimal algorithm for unstructured
keys on a CPU is a streaming one: few passes, sequential access, branchless inner loops, whether
it compares (SIMD quicksort) or addresses (radix with O(1) digits). logos already *is* that
algorithm on the lattice, counting and now block-permuted paths, which is why those rows win. The
unstructured rows lose by exactly the two things the profiler put numbers on: the splitter search
and the branchy leaf, together ~90% of parallel CPU. Both have avenues with written predictions
(H0b, H6). Everything else in this table is bounded by the two-to-four-pass floor, which no
restructuring within "produce the array" can beat, and that floor is the honest end of the thesis.

---

## M. Order from chaos: the next brainstorm (experiments/ORDER_FROM_CHAOS.md)

Unmeasured, each with a written prediction. Golden ratio as a *lever other than the pivot* (C10 stays cut), pivot sorts revisited after the AVX2 leaf, and mechanisms drawn from Genesis, Heraclitus and biology where the metaphor yielded one. Ranked:

| rank | item | prediction | cost |
|---|---|---|---|
| M1 | Sample-as-pivot: the ~32 sorted sample values a bucket already owns become equi-depth class boundaries for the parallel leaf (the C17 lever) | leaf −10 to −20% on heavy_tail and bimodal at 14T | **NOT BUILT: profile bounds it at ~3% of wall** (leaf on heavy_tail 13.2 vs uniform 12.3 ms, bimodal below uniform); quantile splitters already make buckets locally flat |
| M2 | Firmament split: per-mode splitter tables after a sample-gap mode detection (Genesis 1:6) | bimodal histogram 7.3 → 5 ms at 14T | **SHIPPED: bimodal 14T 22.8 → 19.9 ms (1.15×, prediction low)**; absent on single-mode shapes; `-DLOGOS_NO_MODE_TABLE` |
| M3 | Disorder probe routing low-discrepancy data (Weyl `frac(i·φ)`, van der Corput) to flash/coarse instead of the AVX2 leaf | 8M Weyl at 1T ~40 ms vs SIMD ~80 | **SHIPPED as a three-gap window probe: weyl 99.5 → 79.6, van der Corput 100 → 78 ms at 1T** (prediction optimistic: coarse at 8M is ~78); never fires elsewhere; `-DLOGOS_NO_LOWDISC` |
| M4 | Chaos zoo (logistic map, Lorenz-x, Collatz, Benford, Brownian, prime gaps, Zipf, jittered lattice) plus an entropy scan chart (time/element against H) | two mis-dispatches found; one chart that states the thesis | **ZOO BUILT (`make chaosbench`, 15 shapes)**: found van der Corput aliasing the stride sampler (9×) and sawtooth's 1T run-merge path (545 ms, open); entropy chart not built |
| M5 | Weyl sampling positions for every sampler (three-gap theorem; the golden ratio's only theorem-backed use here) | 3–5× on stride-aliased input, 0 elsewhere | **SHIPPED: sawtooth_stride 14T 546 → 38 ms (14×), van der Corput 14T 9×, collatz 1.18–1.25×, nothing else outside noise**; `-DLOGOS_STRIDE_SAMPLE` |
| M6 | Quorum trial: threads commit to the group's median trial winner (B17) | worst-rep −5 to −10% under load | ½ day |
| M7 | Network pivot (64-sample median via the bitonic network); interpolation pivot with split-ratio fallback | 2–5% at 1T | **network pivot MEASURED NEUTRAL** (0.94–1.06× across the zoo), opt-in `-DAV_PIVOT_NET`; interpolation pivot unbuilt |
| M8 | Ping-pong winnow at the top level of the AVX2 leaf (no end-choice branch) | 5–8% at 1T random | 1 day |
| M9 | Plan cache keyed on a data signature (FFTW-style) for repeated sorts | 3–5% on repeated calls | 1 day |
| M10 | Incommensurate class widths `s·(m + 1/φ)` for jittered lattices | 1.2–1.4× on those shapes only | 1 day |

Dropped after inspection: dual-pivot vectorized partition (per-vector work outweighs fewer levels), Fibonacci search fallback (<1% of lookups), golden pivot (C10, information argument), and the metaphors listed in ORDER_FROM_CHAOS.md §4.10.

## N2. The interleaved A/B has a per-copy layout bias (2026-09-11)

| claim | grade | evidence |
|---|---|---|
| Four engine copies built from identical source and identical flags, differing only in renamed symbols, time differently | **MEASURED: up to 1.24×**, deterministic, one copy systematically fastest across unrelated shapes | `make tailbench TB0= TB1= TB2= TB3=`; code and data alignment shifts with symbol length |
| Therefore an interleaved A/B cannot resolve an effect below ~1.25× on its own | FOLLOWS | best-of-N does not remove a deterministic bias |
| The control that does work: permute the variant-to-copy assignment and require the effect to follow the variant | MEASURED on the key-space certificate: 1.32× with it in copies 1 and 3, 1.15× with it in copies 0 and 2 — follows the feature, so real, and about 1.2–1.3× rather than the 1.45–1.50× the uncontrolled runs gave | now required for any claim under 1.25× (methodology rule 3) |
| Results above the band, unaffected | perfect hash 15–24× (D35), Weyl sampling 9–14× (D23), near-sorted route 2.3–2.9× (D28), cardinality cliff 6.5×, lattice window 1.31–1.40× (D27), thread caps 1.3–1.8× on the rows that carry them (D30) | each far outside 1.25× |
| Results below the band, now **unconfirmed as end-to-end ratios** | serial fixed costs 1.02–1.18× (D34), most sample-once rows 1.07–1.19× (D33) | their *mechanisms* were measured directly by trace (serial work before the workers 2.0 → 0.9 ms; the scatter-to-leaves step 1.15 → 0.09 ms; five samples reduced to one) and those stand |

## N. The inherited constants, swept (2026-09-11, `make knobbench`, FINDINGS §3l)

Fourteen JS-era dispatcher thresholds, four values each, four engines interleaved over 19 shapes at 1 thread and 14. Three changed, eleven kept with evidence.

| constant (JS-era value) | values swept | verdict | evidence |
|---|---|---|---|
| MIN_AVG_RUN_FOR_MERGE (256) | 64, 1024, 4096; then 16384, 65536, 262144 | **4096 shipped** | sawtooth_stride 1T 553 → 138 ms (its 5376 runs go to the AVX2 leaf); 262144 broke relocation on 64-block data (17 → 52 ms) because the same cap feeds run detection |
| MERGE_MAX_RUNS (new) | ∞, 32, 8, 3 | **8 shipped** | 64 overlapping runs 1T 283 → 116 ms; a 4-way merge still beats the leaf (89 vs 120), so 3 lost that row |
| LOWCARD_CAP (1024) | 512, 256, 128 | **256 shipped** | zipf 1T 47 → 33 ms, 14T 15 → 11: it passes the 64-sample gate yet has thousands of distinct values and aborted late; 256 aborts early. Data with 256–1024 distinct values ran 4.4× slower on this path than on the hash anyway (collatz at gate 1024) |
| COUNTING_SORT_K (1) | 2, 4, 8 | keep 1 | K=2 made int_span2n 1T 2.2× slower, K=8 int_span8n 4×: a count array above n is random access into 64+ MB; the JS reason was memory, the C reason is cache. Counting is cheap only when the increments are sequential (near-sorted integers) |
| LOWCARD_SAMPLE_MAX (64) | 16, 256, 1024 | keep 64 | 1024 accepts collatz and runs it 4.4× slower; 16 gains only what LOWCARD_CAP now gains |
| NEARLY_SORTED_INV_RATIO (0.05) | 0.02, 0.1, 0.2 | keep | 0.02 lost 23% on near_sorted_f64; 0.1 and 0.2 gained nothing |
| QUANT_PROBE_MAX (4) | 2, 5, 6 | keep 4 | 5 and 6 send integer data to the 8-pass float radix (near_sorted_int 1T 40 → 147 ms); 2 within noise of 4 |
| FLASH_SORT_THRESHOLD (4096) | 512, 16384, 65536 | keep | 65536 sends 33k buckets to the scalar introsort, 3× slower at 14T; 512 and 16384 noise |
| FLASH_SAFETY_RATIO (0.05) | 0.01, 0.2, 0.5 | keep | 0.5 sends buckets to coarse instead of flash, uniform 14T 22 → 30 ms; 0.01 and 0.2 no consistent gain |
| MOMENTUM_THRESHOLD (50000) | 5000, 500000, ∞ | keep | noise; ∞ burns the 8n budget on near_sorted_f64 (0.91×) |
| SKEW_PROBE_MAX (8) | 4, 16, 32 | keep | noise at every value |
| TYPED_MIN_N (65536) | 16384, 262144, 1M | keep | noise |
| INSERTION_SORT_THRESHOLD (24) | 16, 48, 96 | keep | within the bench's ±5% first-column bias; nothing separable |
| COARSE_SMALL (64) | 32, 128, 256 | keep | same |

What it says about the guesswork question: the inherited third of the engine's constants was mostly right, and the three that were wrong were all gates on paths whose cost model differs between JS and C (a k-way merge, a late-aborting counting attempt). The remaining risk is in the probe *structure* (B20, B21), not in the constants.

## L. What the literature offers, and where each piece plugs in

"We know a lot about uniform distributions and nothing about the structure" is half right. The
literature says a sample *does* know the structure, with bounds, and that the cost of the
transform it implies can be computed before paying it.

| # | literature | what it gives this engine | status |
|---|---|---|---|
| L1 | Probability integral transform + Dvoretzky–Kiefer–Wolfowitz (1956; Massart 1990) | any distribution becomes uniform under its own CDF; m samples estimate that CDF within ε with probability 1 − 2e^(−2mε²). 2560 samples: splitters within ~3% of true quantiles at 99%; 65k samples: ~0.6% | implicit in OVERSAMPLE = 32; never derived or tuned |
| L2 | Devroye, *Lecture Notes on Bucket Algorithms* (1986) | bucket sort is expected-linear iff the density is square-integrable, and the fix-up work is ∝ n·∫f². The sum of squared cell occupancies of a sample estimates ∫f², so the leaf's cost is predictable before the pass | missing; the leafbench negative (C17) is this model showing up |
| L3 | Learned Sort (Kristo, Vaidya, Çetintemel, Misra, Kraska, SIGMOD 2020; duplicates redesign 2021) | CDF-model placement into slack buckets with local fix-up; 1.5–3× over radix/IPS⁴o on real data | J8, unmeasured; the H0b tables are a crude CDF model |
| L4 | Adaptive sorting: Estivill-Castro & Wood survey (1992); Munro & Spira multisets (1976); Munro & Wild Powersort (2018) | named measures of presortedness (Runs, Inv, Rem, Block, Osc) each with an n·log(1+M/n) bound and an optimal algorithm; multiset bound n·H for counting; run-entropy-optimal merge order | partial and unnamed: run-merge ≈ Runs, near-sorted ≈ Inv, J5 ≈ Block, counting/lattice hit the multiset bound; H2 is the Rem-optimal path; merge schedule is fixed 2/4-way, not Powersort |
| L5 | Sketches: HyperLogLog (Flajolet et al. 2007), KLL (Karnin, Lang, Liberty 2016), t-digest (Dunning) | one-pass, mergeable cardinality and ε-approximate quantiles; KLL rank error ≤ εn with a bound, t-digest better at the tails | **not implemented** (F11). Not needed for the top-level splitters, where random access makes a strided sample equivalent; needed for chunk-0 piggybacked probes (merge per-thread sketches), for closing the 65k–1M-distinct gap (HLL, F7), and for the query API's quantiles (D11's 16k-sample has no bound) |
| L6 | I/O-model distribution sort (Aggarwal & Vitter 1988); Wassenberg & Sanders multicore radix (2011); Satish et al. bandwidth-oblivious sort (2010) | the pass-count floor (two passes at this fan-out) and the multicore design that reaches it: per-thread histograms, one prefix scan, per-thread scatter, software write-combining | B4 is this model on a CPU; K2/K4 open; the JS prototype in experiments/parallel-js-prototype is this design |
| L7 | Super Scalar Sample Sort (Sanders & Winkel 2004); IPS⁴o (Axtmann et al. 2017) | branchless decision tree for bucket lookup; equal-key buckets; in-place parallel partition | `bucket_of` was the decision tree and is now the H0b table; equal-key buckets = the counting paths |
| L8 | Algorithm selection (Rice 1976); FFTW's planner (Frigo & Johnson 2005) | estimate to shortlist, measure to commit; measured selection is robust, estimated is cheap | J15 shipped in one place (value vs key table); Devroye's estimate (L2) is the "estimate" half |
| L9 | Interpolation search (Gonnet, Rogers, George 1980); Flashsort (Neubert 1998) | O(log log n) on uniform, degrading with the same density quantity as L2 | the leaf's flash sort is Neubert's with m = n classes |

**How boundaries are decided today (guess vs narrow):**

| level | boundary comes from | narrowed afterwards? |
|---|---|---|
| top-level buckets | one strided 2560-sample, every 32nd sorted point | no; frozen before the histogram |
| histogram | exact count per bucket for every element | used for offsets only, never for balance |
| leaf classes | equi-width over a prescanned min/max of the bucket | no; a 64-sample only gates flash vs coarse |
| skewed data | sampled skew probe → exponent-bit keys | no |
| counting, lattice, relocation | exact, with abort budgets | exact by construction |

The engine guesses once from a sound sample and never narrows. The cheapest genuine narrowing is
J16 (regroup on the exact counts), then equi-depth leaf classes gated by the L2 estimate; sketches
belong in the query API and the streaming probe, not in the partition.

**Physical properties that make neighbour checks cheap** (relevant to the leaf fix-up and the
piggybacked probes): a cache line is eight doubles, so comparing an element with its line-mates
costs no memory traffic; the hardware prefetcher runs a neighbour-wise scan at bandwidth; one SIMD
compare of a register against its one-lane shift yields "a[i] ≤ a[i+1]" for eight pairs, a second
gives equality, an OR of key bytes gives the byte-skip mask; AVX-512 conflict detection finds equal
values within sixteen lanes (useless as a histogram, C2, but exactly a similarity check on data
already in the register). Nothing is content-addressed: caches and TLBs hash on address, so "values
near x" can only be found after placing by value, which is what the partition is for.
