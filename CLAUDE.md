# CLAUDE.md — logos-sort project context

Read this first. It encodes the state of the project, the measurement discipline
that produced trustworthy results, and — most importantly — the ledger of things
that were **tried and measured to fail**, so you don't re-run them.

## What this is

**logos-sort** is a distribution-aware adaptive numeric sort engine, implemented
in C (flagship, parallel), Rust, JavaScript, and Python. Its thesis: *addressing
beats comparing* — detect the data's structure (cardinality, sortedness, skew,
lattice/quantization) and dispatch to a zero- or low-comparison path (counting,
radix, run-merge, interpolation bucketing) instead of a comparison sort.

The thesis is **philosophically correct and empirically narrow**. Where it lands
today (all measured, 20-core box, 8M f64, every output verified):

- Beats scalar sorts (`qsort`, `std::sort`) 2.8–12× single-threaded.
- Beats OpenMP `__gnu_parallel::sort` and **IPS⁴o** (SOTA parallel comparison
  sort) on every shape, multi-threaded.
- **Loses to Intel x86-simd-sort (AVX-512 vectorized quicksort)** on all
  unstructured shapes, by ~2–3× multi-threaded and ~3–37× single-threaded.
- **Beats x86-simd-sort only on structured data**: few-distinct (7.2 vs 9.5 ms)
  and quantized (9.2 vs 23.3 ms, a 2.5× win) — the zero-scatter counting/lattice
  paths. This is the algorithm's real, defensible niche.

**The honest one-liner:** logos wins precisely and only where it avoids both
comparing *and* scattering. On unstructured data a vectorized quicksort wins,
and on a single-socket CPU no restructuring changes that (see the ledger).
**Re-measured 2026-09-10 on AVX2 hardware (i9-13900H, no AVX-512), quiet
box, 14 threads, FINDINGS §1d:** logos is at parity with x86-simd-sort's AVX2
kernels on the random shapes (0.86–1.08×) and ahead on structured ones
(few_distinct 1.4×, quantized 1.76×, block shapes 2.2–2.5×). Against IPS⁴o
logos wins 7 of 9 (1.5× random, 2–2.5× structured) and loses near_sorted and
few_distinct by 6–17%. Single-threaded the SIMD sort wins the random shapes
1.23–1.31× (was ~1.7× before the AVX2 sequential leaf, FINDINGS §1e) and
few_distinct 2.6×; logos 1T beats the LSD radix on all nine shapes. **An earlier run on the same box (§1c, loaded
2.5×) showed "7 of 9 over the SIMD sort, every row over IPS⁴o"; that was the
load, not the engine:** between the loaded and the quiet run logos sped up
1.4×, the SIMD sort 1.7×, gnu_par 1.6× and IPS⁴o 3.5×. Barrier-heavy
opponents lose more to preemption than logos's chunk-parallel phases do, so
a loaded box flatters logos. Treat any fair-fight ratio from a run whose
calibration line is off as an upper bound on logos. Re-running at 20 threads
on the quiet box changed nothing (uniform 18.5 vs 18.3 ms), so the thread
count was not the cause. The AVX-512 numbers above are older and that box has
not been re-run.
External opponents live at `engines/x86-simd-sort` and `engines/ips4o`
(clones; `make parbench-full`, `make profile-full`).

## Repository layout

```
engines/c/          FLAGSHIP. logos_psort.c (engine), logos_extra.c (argSort,
                    permutations, outliers, strings), logos_simd.cpp (optional
                    AVX-512 leaf + kernels), parbench.cpp (the fair fight),
                    scaling.cpp, harness.c (1568 checks), PARITY.md, CHANGELOG.md
engines/rust/       v2 engine + in-place + extras. BEHIND C on numerics (see PARITY.md).
engines/js/         Original engine (npm logos-sort). Most complete STRING engine.
engines/python/     Sequential reference.
experiments/ORDER_FROM_CHAOS.md  next brainstorm: golden-ratio levers, pivots, mechanisms from
                    scripture/philosophy/biology, each with a prediction (TRUTH_TABLE §M)
experiments/OUTSIDE_THE_BOX.md   round two, 2026-09-12: hardware the project has never touched
                    (huge pages, streaming stores), knot placement by curvature, a strided-order
                    probe, relocation by page table, the lazy sorted view. Each with a written
                    prediction. START WITH A1 (huge pages): measured zero AnonHugePages today.
experiments/paradigm/   The "change the deliverable" experiments (the strongest
                    result in the project) + merge/scatter negative results.
experiments/parallel-js-prototype/  worker_threads sample sort; not productionized.
tools/sortbench/    Python benchmarking lib (numpy/sorted/SQLite/DuckDB vs logos).
tools/truth_table_html.py   renders TRUTH_TABLE.md -> reports/truth-table.html
tools/embed_uplot.py        bakes engines/c/vendor/uplot.* into engines/c/uplot_embed.h
tools/check_report.js       runs a generated report's script under a stub DOM and a
                    validating uPlot stub; `make scalebench` / `make profile` call it
tools/ledger_rerun.py       re-tests a ledger verdict against today's noise floor: builds each
                    feature twice with the engine copies SWAPPED, measures both with the
                    paired harness, and reports sqrt(A/B), which cancels the code-layout
                    bias exactly. Use it before trusting any old "neutral" verdict.
tools/browser_check.py      renders a report in real headless Chrome/Edge and fails on any
                    uncaught error, both axis modes (--shot for a screenshot). Use it
                    whenever a chart looks wrong: a stub DOM only produces the inputs
                    you thought of, and it once passed a report whose log panels were
                    entirely blank.
reports/            Generated HTML that lives with the project: truth-table.html
                    (from TRUTH_TABLE.md), profile-report.html (`make profile`) and
                    scale-report.html (`make scalebench`, interactive uPlot charts).
                    Every report is ONE self-contained file: no CDN, no sibling assets.
                    That is why uPlot is vendored under engines/c/vendor/ and inlined.
TRUTH_TABLE.md      Every claim graded (measured / inferred / untested / false),
                    ranked avenues with predictions, theory-vs-optimum gaps.
archive/            Single-file JS builds, design docs.
```

Start with `engines/c/`. `make test` must pass before and after any change.

## THE METHODOLOGY (non-negotiable — it is why the results are trustworthy)

1. **Verify before timing.** Every benchmark checks output against a reference
   (`std::sort` / `qsort`). A fast wrong answer is worthless. `harness.c` runs
   1568 property checks; keep them green.
2. **Read the calibration line first — but expect it to say "loaded", and
   know why.** `parbench` prints memcpy-64MB and std::sort-1M times. On the
   reference box these read ~2.9 ms / ~65 ms when it has been idle and 4–7 ms
   / 100–170 ms inside any benchmark that has been running for a minute. That
   is **the machine, not an external process**: measured 2026-09-11, right
   after a 15-minute chain a compute loop and a 64 MB memcpy ran 5.5× and 9.6×
   slower than the same loops 26 seconds later, recovering smoothly, the two
   moving in lockstep. WSL2 exposes no temperature, no live frequency and zero
   steal time, so the mechanism cannot be pinned down from inside. **What
   follows: never compare absolute milliseconds across runs, never compare
   across rows of one run, and do not waste time waiting for a quiet box
   before a long benchmark — the idle reading does not survive the run.**
   Trust only contenders that alternate at fine grain inside one row; that is
   what the speedup columns are, and their baseline is measured in the same
   row at the same moment. A calibration line far outside even the loaded
   range (memcpy >10 ms) still means a genuine external hog: discard that.
3. **Compare within a run, never across runs — and swap which copy holds
   which variant.** To compare *variants of logos*, link them into ONE binary
   and interleave on identical data (`pfbench.c` is the template). Separate
   runs can't resolve a 5% effect when load drifts 25%. **But the copies are
   not interchangeable:** built with identical source and flags, differing
   only in their renamed symbols, four copies still differed by up to 1.24×
   on a quiet box, deterministically, because symbol
   lengths change code alignment (`make tailbench TB0= TB1= TB2= TB3=` is the
   null test; 2026-09-11). **No interleaved A/B here resolves an effect below
   about 1.25× unless you rebuild with the variant-to-copy assignment permuted
   and show the effect follows the variant.** Do that control for every claim
   under 1.25×, or measure the mechanism directly with `-DLOGOS_TRACE` instead
   of inferring it from end-to-end ratios.
   **Better, and the standard from 2026-09-11: `bench_paired()` in
   `bench_common.h`.** It runs the two variants ADJACENTLY on the two halves of
   one array, alternates which half goes first, and reports the median of their
   ratio with p10/p90. Drift cancels inside a pair (both halves see the same
   machine state) and the ordering bias cancels across pairs (the half that
   goes first measures 2-4% slow, from destination-page state). Run
   **`make pairprobe`** first: it is the same harness with the SAME engine on
   both sides, so the true ratio is 1.000 and everything it prints is this
   box's error. Today one pair has a p90 deviation of 8-14%, and the median of
   40 pairs resolves ~3.7% at 8M and ~6.1% at 1M. Error falls as 1/sqrt(pairs),
   so 160 pairs buys ~2%. That moves the resolving floor from ~1.25x to ~1.04x.
   Tested and rejected on this box, do not re-try: **`taskset` pinning** (one
   run looked 1.40 -> 1.13 better, the next gave 1.32 -- WSL2's CPUs are
   virtual and the hypervisor re-places them; a thread pinned to CPUs 12-19 ran
   *faster* than unpinned, which also kills the "those are the E-cores"
   reading), **cgroup cpuset isolation** (needs root, and runs into the same
   virtualisation boundary), and **changing the thread count** (with counts
   rotated inside each rep, 12 and 14 tie at 19.16 vs 19.24 ms; the across-run
   spread favours MORE threads, but that spread is drift, and drift cancels in
   a pair). BENCH_THREADS stays 14.
4. **Single vs single, multi vs multi.** Never compare a 1-thread sort to a
   P-thread sort. `parbench` has two sections for this reason.
5. **Microbenchmark wins do NOT transfer.** Three times a kernel that won in
   isolation LOST when integrated (SIMD bucket-index, winner tree, cache-
   conscious COARSE_K). Always A/B the *full sort* end-to-end. The engine is
   pass-overhead-bound: reducing per-pass cost by adding passes never pays.
6. **Predict before measuring, then grade the prediction.** State the expected
   outcome in writing first. A prediction that's wrong is more informative than
   one that's right — and it keeps you honest.
7. **A measured dead end is a deliverable.** Record negatives in CHANGELOG/
   PARITY/experiments. Don't delete failed code silently; document why it failed.
8. **Don't ship what didn't win.** If the A/B says no, revert. The engine at
   3.6.0 has *zero* changes that failed their A/B.
9. **Profile by phase before choosing what to optimise.** `make profile`
   separates CPU work, preemption, barrier stalls and idle per thread. A/B
   answers "did it help"; only the profile answers "where is the time".
10. **Measure the noise before measuring the engine.** `make noiseprobe`
    repeats one sort at several thread counts and prints min / median / max,
    preempted share and CPU utilisation. On the reference box under its usual
    load (VS Code's software-WebGL GPU process ~44% of the machine, OneDrive
    sync ~20%): a 1-thread sort is not preempted at all yet varies 7% at the
    median and 56% at the worst rep (sibling-hyperthread and cache contention,
    invisible to CPU accounting); parallel runs lose time slices in proportion
    to thread count (utilisation 0.99 at 2 threads, 0.62 at 20; max/min 1.4 →
    2.7); the best minimum is at 14 threads, the physical core count, not 20.
    Consequences: report the minimum of ≥12 reps for anything decisive
    (best-of-3 once absorbed a 5× memcpy outlier), keep variants interleaved,
    and benchmark at the physical core count. **Every bench now defaults to
    `BENCH_THREADS` (Makefile, 14 on this box) via `engines/c/bench_common.h`;**
    the engine's own default (`logos_num_cpus`) is unchanged, and fair fights
    stay P-vs-P at the same count. Override per run with `LOGOS_THREADS=N`.
    Stopping the two hogs is worth more than all of that together.

## NEGATIVE-RESULTS LEDGER — do not re-run these

Each was implemented and measured. Re-trying them without a genuinely new angle
wastes the session.

| idea | result | why |
|---|---|---|
| SIMD-vectorize the scatter | ~0 | data-dependent addresses don't vectorize; `vpscatter` decomposes to serial stores (1.05×) |
| SIMD-vectorize the histogram (`vpconflictd`) | **0.65× slower** | needs gather+scatter per step — the ops it tried to avoid |
| SIMD-vectorize bucket-index in coarse | **−3 to −5.5%** | must fuse with scalar scatter-count → SIMD-store-then-reload costs more |
| SIMD OR/AND byte-skip | ~0 | correct 1.22× kernel, but ~0.1% of a memory-bound sort — noise |
| Winner/loser tree merge (near-sorted) | **1.4–1.9× slower** | over sorted-ish data ~1 live run; heap pops O(1), tree replays full log-path |
| Shared per-thread context pool | **regressed, then segfaulted** | call sites need different sizes; one shared pool thrashes or under-sizes |
| Parallel MERGE sort (chunk-sort + k-way merge) | **merge 15–26× costlier than scatter** | a merge is *comparison*; the top-level scatter is already ~bandwidth (~16 ms). Attacks the cheap op, leaves the expensive one |
| Cache-conscious COARSE_K (smaller, cache-resident) | **0.58–0.90×** | +11% in isolation, but logos's per-level overhead (ping-pong, balance) exceeds the cache gain. 2048 is optimal |
| logos partition + SIMD leaf ("parallel-SIMD") | **worse than plain logos** (measured twice on 20 cores) | pays logos's scatter *plus* SIMD's sort; x86-simd-sort fuses partition+sort |
| Golden-ratio pivot | 6% slower than random | (the name's origin; measured and cut) |
| flips/descent counter, cardinality probe, gap-statistic gallop, wider radix digits, greedy ascending-subsequence | all rejected | see engines/js/CHANGELOG.md |
| Ping-pong buffering (remove copyback) | neutral 1-core, ~12% at 20T (mostly the pool) | removed 37% of traffic; time moved 12% → **floor is per-core latency + fixed cost, NOT bandwidth** |
| Private per-thread buckets (NUMA) | unmeasurable here | real only on multi-socket; single socket top-scatter already ~bandwidth |
| Run-descriptor overlap gate on raw runs (J5 first cut) | **never fired, 0.99×** | ascending block boundaries fuse into runs whose interval hides holes; must gap-split runs first |
| Sequential relocation probe + spawn-then-bail (J5 second cut) | −16% segmented at 20T | one-thread read pass and 19 wasted thread spawns on loaded WSL2; scan chunk 0 on the caller first, sample pre-gate |
| Leaf knobs: flash classes m = n/4 or n/16; coarse instead of flash on ~100k buckets | **mixed, not universal** (`make leafbench`) | fewer classes = more insertion fix-up, which IS the leaf cost; a second coarse level costs more than it saves (B4). Leaf lever is density-adaptive classes + branchless fix-up + fewer probes, not these constants |
| AVX2 quicksort inside the PARALLEL leaf (`-DLOGOS_AVX2_PARALLEL`, `make simdbench`) | **−5 to −28% at 14T, −10 to −16% at 6T** on every random shape | an L2-resident 33k bucket already sorts at ~9 ns/elem scalar; the vectorized quicksort is 8.5 at 1T and slower under 14 on a hybrid P+E CPU. Sequential it wins 1.2–1.6×, so it ships sequential-only |
| Model splitter table (linear bucket-index model per value cell, third trial candidate; `-DLOGOS_MODEL_TABLE`) | **re-tested 2026-09-12 at the ~4% floor with layout cancelled: 0.99–1.02, genuinely neutral** (was "0.93–1.09×, noise") | bimodal's residual histogram cost is not the child-table dereference; a model with error ≥2 buckets loops (−12% heavy_tail as a trial chunk) |
| 128-element bitonic base case | 3.0 vs 1.9 ns/elem for 64 | 32 vectors spill; one more partition level + the 64 network is cheaper |
| Network pivot for the AVX2 leaf (`-DAV_PIVOT_NET`) | **re-tested 2026-09-12 at the ~4% floor with layout cancelled: 0.98–1.03, genuinely neutral** (was "0.94–1.06×, noise") | 15 samples already give 17.4 levels vs ideal 17.0 |
| Equi-depth leaf classes from the sample (M1) | **not built: ceiling ~3%** | profile: leaf on heavy_tail only 7% above uniform, bimodal below it |
| Largest-leaf-first (LPT) dispatch in the parallel partition (`LOGOS_LPT=1`, `make lptbench`) | **neutral**: 0.95-1.05 on seven shapes, all inside the resolving floor | the splitters are sample quantiles, so the buckets are already equal: traced imbalance 1.10x uniform / 1.48x Pareto against the 3x the balance check allows. In the greedy makespan bound the whole straggler cost is 8-11% of ONE phase. Scheduling was solved upstream by the partition design. **The counting/lattice routes' static per-thread ranges (1.3-2x imbalance) are the real target and are untouched** |
| Non-temporal stores in the scatter (`-DLOGOS_STREAM`, `tools/ledger_rerun.py`) | **neutral**: 0.99–1.01 at 14T, mechanism confirmed (99.4% of elements streamed, traced) | removing read-for-ownership traffic changes nothing, which agrees with the ping-pong result: the scatter is per-core-latency bound, not bandwidth bound. With huge pages this is the **second** verified memory-traffic lever to measure zero |
| Huge pages for the large buffers (`-DLOGOS_HUGEPAGE`, `tools/ledger_rerun.py`) | **neutral**: 0.98–1.01 at 8M, 0.97–1.02 at 30M, mechanism confirmed firing (98 MB AnonHugePages vs 0) | `hf_flush` stages 256 doubles per bucket and flushes 2 KB contiguously, so the scatter takes ~1 TLB miss per 256 stores. The miss rate follows the BURST size, not the working set, which is why 8M and 30M measure the same. Write-combining had already solved it |
| Concatenate/sort/fold instead of the hash route's serial global-hash merge (`LOGOS_MC_CONCAT=1`, `make mcbench`) | **0.58-0.90x at 14 threads**, 0.98-1.14x at 1 | the concatenation is `P*d` pairs where the union is `d`, so it sorts P times the data the hash merge touches. Kept opt-in because it is the right shape for a PARALLEL merge: split the key space P ways first and the redundancy disappears |
| Byte-skip radix instead of libc qsort for the merge's pair sort | **neutral**, 0.975-1.149 | a phase trace said 147 ms of a 2268 ms sort, but that was a hot box; cool, the same work is ~10% of the sort. Kept anyway because libc qsort is banned on sampled paths. **Caution: a phase trace taken hot overstates a serial phase** |
| Offering a SEQUENTIAL route to a parallel sort (the dense-integer bitmap, first cut) | **0.32-0.72x at 14 threads** | three single-threaded passes took 71 ms against the parallel partition's 27, and a decline still cost two wasted passes. A gate that passes is not a reason to run; check that the route's parallelism matches the sort's |
| Offering a SEQUENTIAL route to a parallel sort, a second time (the sorted-prefix-plus-tail merge, first cut) | **0.34-0.87x at 14 threads**, while winning 3.6-4.5x at one | its NaN scan and backward merge are single-threaded and displaced a parallel near-sorted route that was already fast. After the dense-integer bitmap this is the same mistake twice: gate every sequential route on the sort actually running sequentially |
| Timing a lookup kernel in a `-DLOGOS_TRACE` build (the stream route's table, `make streamsprobe`) | **inverted**: the trace build timed the table 1.3x FASTER than the compares on 8 perturbed streams; the normal build timed it 0.66-0.71x | the trace build's `fprintf` calls change the whole function's code generation. A trace build answers WHETHER a path runs, never how fast it runs |
| Gating the hash-count route on Chao1 <= 65536 (the old gate) | **1.10-5.30x slower at 14 threads** on 7 of 8 shapes it let in (`make cardgate`) | the serial merge; the route earns at 14 threads only up to a sample Chao1 near 3000. Fixed by the new gate |
| Judging a 1-thread counting route by the DATA (integers, skew, cardinality) | wrong on every held-out miss (`make cardgate` intness mode) | what decides is the FALLBACK it replaces: the hash map beats `leaf_f64`'s multi-pass float radix and loses to its AVX2 quicksort. Ask the leaf's own byte probe |
| Byte-comparing a sort against qsort as the oracle | **flagged two correct sorts** (signed zeros) | qsort calls -0.0 and +0.0 equal. Check ascending under `<` plus the bitwise multiset, as the harness now does |
| Reading a regression off the scale report's rows (heavy_tail 104 ms vs uniform 19.9 at 14T/10M, 2026-09-13) | **not a regression**: the profile's scatter ratio was 2.04x, the same as on 2026-09-11 | rows are measured minutes apart (3 reps at 10M, 1 at 30M) and heavy_tail's table-lookup scatter varied 2.4x run to run where uniform's did not. Profile by phase and compare within a row before calling a regression |
| Curvature-placed CDF knots (Douglas-Peucker on the sample's rank-vs-key curve, `experiments/team/cdf-knots.md`) | **neutral**, ~3-4% on heavy_tail at 14T, below the floor; a first pass claiming 1.13x was retracted by a second | the lookup IS cheaper (0.218 vs 0.655 ms per 49k) but buys a WORSE partition (leaf max 1.68x vs 1.45x). The exact splitters come from the same sample and have zero in-sample error, so approximating them can only add error. **Third failed attempt at a fancier lookup: the lookup is not where the partition's time is** |
| Atomic chunk queue for the f64 counting routes (`experiments/team/count-queue.md`) | **neutral** at 14T on few_distinct and quantized (1.03, 0.97, floor 7.7%) | it wins ~1.2x only where a SERIAL phase dominates (lattice_30k at 14T, both shapes at 7T). The static per-thread ranges were not the cost; **the serial slot merge is** |
| Relocation by page table (`mremap`, `experiments/team/pagetable-reloc.md`) | **negative**: not one byte is movable on the bench shapes | the block size in bytes is not a page multiple, so only the permutation's fixed points align (0-1.6%) and those need no move. Manufactured aligned runs win 1.3-2.0x but only from a caller-aligned array, lose 0.63-0.77 on malloc'd ones, and fragment the address space until a second sort faults |
| Range gate from raw min/max on integer data | **catastrophic on sentinels**: one INT32_MIN in 8M narrow values cost 11x at 14 threads. FIXED in `count_try_i32` by windowing from the sample and spilling the tail (11.3x -> 1.19x); `leaf_i32` at 1 thread still has it | a sample bounds the bulk, never the tail, and a sentinel is a tail of size one. Take the window from the sample, not the extremes (FINDINGS §3w) |
| Bucket-relative digit narrowing for the int32 leaf (H5, first cut) | **mixed** (int_wide +21%/−6%, int_1e6 −16%) | it adds a min/max pass the fused byte histogram never paid. Opt-in `-DLOGOS_INT_NARROW`; redo with the bucket range passed down from the splitters |

## POSITIVE results worth building on

- **Key grid for heavy tails** (`make kgbench`, FINDINGS §3ab): data no certificate accepts used to pay the key splitter
  table in the partition's scatter (57 CPU ns/element on heavy_tail against uniform's 28). Equal-width cells in total-order
  key space with bucket edges snapped to the cells of sample quantiles make the lookup one byte load, gated by the balance
  check. 14 threads: heavy_tail 1.36x at 8.4M and 1.83x at 1M, lognormal 1.40x / 1.67x, exponential 1.32x / 1.54x, Pareto
  0.8 1.33x / 1.75x; bimodal, clustered, int_wide, uniform and every 1-thread row 0.98-1.05. The exponent map it replaces in
  spirit lost because it had one linear piece per binade; the grid has hundreds.
- **Lattice (direct-index) counting**: quantized 109→52 ms seq; the strongest
  structured-data win. `lattice_try_f64`.
- **The type can answer what the data would have to be asked** (`count_try_i32`,
  `make intbench`, FINDINGS §3w): an int32 array is a lattice of step 1 by
  construction, so the parallel counting route needs no step proposal, no tolerance,
  no NaN screen and no collision check, unlike its f64 twin. Span 100 at 8M went
  37.6 → 3.8 ms at 14 threads. The general rule: **a type fact may be assumed, a
  data fact must be certified**, and the test is whether a counterexample is
  representable at all, not whether it seems likely.
- **Near-sorted index-partition + one-pass P-way merge**: 28→22.5 ms at 20T.
- **Per-phase profiler (`make profile`)**: the first breakdown of the engine
  by phase, thread, CPU-vs-preempted, barrier stall and leaf path. Its first
  result corrects the cost model: **at 20T the top-level histogram is the
  dominant phase (49–70% of wall on unstructured shapes) and it is CPU work**
  (2026-09-11: that phase no longer exists, see the histogram-free partition)
  — `bucket_of`'s dependent-load binary search over the splitters, ~100
  ns/element on the reference box vs ~11 for the scatter and ~29 for the
  whole 1T sort. 3.4.0 moved the search out of the scatter, it did not remove
  it. This is why 20 threads give only ~3× over 1. Fix candidate with a written
  prediction: direct-index splitter table (see engines/c/CHANGELOG).
- **Per-mode certificate** (`make tailbench`, FINDINGS §3s): when the value
  range splits into modes at gulfs, each mode is tested for uniformity on its
  own sample slice; if all certify, the partition replaces its per-mode tables
  with branch-free mode selection plus one multiply-add. 1.25–1.38× at 14
  threads on piecewise-uniform data, and it correctly declines gaussian
  mixtures (a hump is not uniform inside). The lesson generalises: splitting at
  gulfs is cheap, assuming the pieces are uniform is not — certify them.
- **Heavy-tail structures** (`make tailbench`, FINDINGS §3q): a uniformity
  certificate in KEY space puts log-uniform data (Benford: prices, file
  sizes) on the one-multiply grid, 1.45× at 14 threads; a balance check
  applies every candidate map to the sample and drops it if a bucket would
  take more than 3× its share, which is the guard the trial always needed
  because it times the lookup and not the partition; and the partition now
  declines up front when one value exceeds a region. An exponent-histogram
  map is built and opt-in, measured slower than the key table it replaces.
- **Counting route's lookup is a verified perfect hash** (`make cardbench`,
  FINDINGS §3p): the gate's sorted sample supplies the distinct values, a
  multiply-shift hash over their keys goes into a byte table, and every
  lookup is confirmed by a key compare so a value the sample missed still
  counts correctly. It removed a 6.5× cliff for 33 to 64 distinct values
  that no bench shape could see (228–318 ms at 1T against 35 for 200
  distinct), and is 15–24× on that window and 2.0–3.1× at 5 distinct.
  Routing such data to the lattice's direct index instead was measured and
  rejected: cheaper per element, 24× the table footprint.
- **Serial fixed costs cut** (`make probebench`, FINDINGS §3o): the sample's
  random reads are prefetched in a batch and sorted by the AVX2 quicksort,
  and per-thread contexts live in a persistent bundle table instead of
  being allocated per call. Per parallel call the serial work before the
  workers went ~2.0 → ~0.9 ms and the scatter-to-leaves step 0.8–1.8 →
  0.09 ms. The probe is not threaded on purpose: a spawn costs what the
  sample costs. SHIPPED as the default: the trace shows the mechanism and the quiet half of the end-to-end A/B confirms 1.07-1.18x at 1M and 1.02-1.10x at 8M on the parallel rows with the 1-thread rows unchanged; the persistent bundles cost about 6 bytes per element of the largest capacity ever requested (48 MB after an 8M single-threaded call), the same order as the existing buffer pool.
- **Sample once, decide once** (`make sodobench`, FINDINGS §3n): one
  two-stage sorted sample answers every dispatcher gate (2048 Weyl points
  for the gates, extended to 5376 only for the partition), with a Chao1
  estimate of the true cardinality so the counting routes are attempted only
  when they can finish, and the partition reuses the sample. The routes keep
  their cheapest-first order, which a cost model from the measured per-route
  costs reproduces. SHIPPED as the default (-DLOGOS_NO_SODO restores the per-gate samples): no loss on any shape once the two-stage draw was in, 1.07-1.39x on zipf and 1.07-1.22x on the 1M rows of the partition shapes, neutral at 8M; every run of the three was on a loaded box, ratios within rows only.
- **Histogram-free partition with a uniformity certificate** (`make hfbench`,
  FINDINGS §3m): the parallel partition no longer counts before it scatters.
  Bucket regions are sized from the sample (a bucket's count has an 18%
  standard deviation, Beta(32, m−31), so 60% slack), threads reserve
  256-element blocks with one atomic each through a write-combining stage,
  spills merge in at the leaf, and above n/16 spills it declines to the
  histogram path. A Kolmogorov-Smirnov distance of the sorted sample from the
  straight line (free) certifies uniform data, which then buckets on an
  equi-width grid with no tables. 1.14–1.45× at 14 threads on every shape
  that reaches the partition, nothing elsewhere; 685 checks pass on both
  paths. This retires the histogram bottleneck the profiler found in H0b's
  era and answers "what does uniform tell us" with a working piece of code.
- **The inherited JS-era constants, swept** (`make knobbench`, TRUTH_TABLE
  §N, FINDINGS §3l): fourteen dispatcher thresholds, four values each, four
  engines interleaved over 19 shapes. Three changed: the run-merge gate
  256 → 4096 (sawtooth 553 → 138 ms at 1T), a new cap of 8 on the runs a
  k-way merge will take (64 runs 283 → 116), the low-cardinality abort cap
  1024 → 256 (zipf 1.44×). Eleven kept, each with a recorded reason; the
  counting span factor stays 1 because a count array above n is random
  access into 64+ MB (2–4× slower than the radix), not for the JS memory
  reason. Every inherited constant is now `#ifndef`-overridable.
- **Per-route thread caps** (`make smallbench`, FINDINGS §3k): the right
  thread count depends on the route. Relocation runs at most one thread per
  500k elements (its parallel form has a serial plan; 6 threads beat 14 by
  1.7× at 3.16M), the counting routes at most one per 350k with a floor of
  3 (they merge P tables, a cost ∝ P; 4 threads beat 10 at 1M, 3 beat 1 at
  316k). 1.2–1.7× on the structured shapes from 1M to 3.16M, nothing at 8M
  where neither cap binds. A global 250k policy was measured and rejected.
- **Thread-count policy** (`effective_threads`, at most one thread per 100k
  elements): the scaling benchmark showed 20 threads slower than 1 below ~5e5
  elements (parallel fixed cost ~5–10 ms). Small-n parallel rows 3–20× faster,
  ≥2M unchanged. Found only because `make scale-full` plots time vs n; parbench
  at 8M could never see it.
- **The structured routes did not scale, and the near-sorted one is
  rebuilt** (`make nsbench`, FINDINGS §3j): the profile's 1T and 14T cells
  showed the partition route scaling 4.6–5.5× but near-sorted 1.8×,
  relocation 2.1× and lattice 3.7–4.4× against 6–7× for IPS⁴o. Two general
  lessons: chunking by index changes the dispatcher's decisions (its gates
  are ratios of n), and the inversion probe bounds inversions, not
  displacement (plain insertion went quadratic). The new near-sorted route
  (thresholds at chunk boundaries, one greedy keep/reject pass per chunk
  with poison recovery, strays sorted once, one two-way merge per band) is
  2.3–2.9× on integer near-sorted data and 1.4–2.1× on f64 at 14T, guards
  unchanged; the momentum insertion now bounds the small nearly-sorted
  case. Still open: static per-thread ranges in lattice/counting (imbalance
  1.3–2×) and relocation's serial middle.
- **Weyl sample positions, low-discrepancy probe, per-mode splitter tables**
  (`make chaosbench`, ORDER_FROM_CHAOS.md M5/M3/M2): every sampler reads at
  `floor(frac(i·φ)·n)`, which cannot alias with a periodic input (a sawtooth
  at the old stride went 546 → 38 ms at 14T, van der Corput 177 → 19); a
  three-gap window probe sends Weyl/van der Corput data to flash instead of
  the AVX2 leaf (1.25–1.29× at 1T); splitter gaps > 10× the median define
  modes with their own value tables (bimodal 14T 22.8 → 19.9 ms, level with
  x86-simd-sort there). The zoo of 15 shapes found the two aliasing failures
  the nine standard shapes never showed, and a third defect behind a gain
  first mis-credited to the sampler: the lattice counter aborted on tail
  values outside its sample-sized window and redid 19–44% of the array
  (Collatz). Widening small tables to 4096 slots: 1.31× / 1.40× (`make
  latbench`). A sample bounds the bulk, never the tail.
- **AVX2 vectorized leaf for sequential sorts** (`avx2_qsort_f64`, generated
  by `gen_avx2_leaf.py`, A/B `make simdbench`): the x86-simd-sort design in
  house, AVX2 only, taking only the unstructured leaf (flash/coarse/bits-coarse
  sites) of a single-threaded sort and stepping aside when the inversion probe
  says nearly sorted. 1T: uniform 1.20×, heavy_tail 1.48×, bimodal 1.59×,
  clustered 1.19×, negatives 1.26×; everything else identical. The first cut
  was 2.3× slower; a component probe (one partition pass, one network) found
  the three defects (end-choice branch per vector, un-unrolled networks,
  reserved-vector overhead on small ranges). Whole-array 8M at 1T is now
  92–104 ms against x86-simd-sort's 79.
- **12 buckets per thread** (`make theorybench`, three runs): 1.08–1.44× on
  9 of 12 partitioning rows by itself; a 33k-element leaf stays in L2 and the
  queue tail shrinks. **Table-leaf branch policy chosen per thread** from the
  trial chunks' root-resolution fraction: flat data keeps the predictable
  root branch, skewed data goes branch-free (a fixed choice lost 5–12% on int
  shapes or up to 35% on heavy_tail). Measured mixed and kept opt-in:
  interleaved counters, line repair, int-leaf narrowing. **All three re-tested
  2026-09-12 with `tools/ledger_rerun.py` at the ~4% floor and with the code-layout
  bias cancelled by a copy swap: every one is neutral (0.95–1.04 across five shapes
  at 1 and 14 threads). The apparent 1.042 for interleaved counters on bimodal did
  not survive 80 pairs (1.016).** Int-leaf narrowing is only partially re-tested,
  since its original mixed verdict was on int32 shapes this f64 bench does not run.
- **Direct-index splitter tables (H0b)**: the histogram's bucket lookup went
  from a 7-step binary search to an O(1) table, with each thread trialling a
  value-space and a key-space table on its first two chunks and committing to
  the faster. 20T: 1.6–3.0× on every shape that reaches the partition, noise on
  the rest (`make stbench`). The single largest parallel win in the project.
- **Disjoint-run relocation (J5)**: block-permuted data (shuffled sorted blocks)
  is memcpy'd into place from sorted run descriptors, merging only overlapping
  chains. 8M: 2.3–3.4× at 1T, 1.9–2.2× at 20T vs the run-merge; guard shapes
  within ±5%. `relocate_try_f64`, `make j5bench`. Gap-splitting the runs is
  what makes it work (see ledger).
- **Persistent scratch pool**: first measurement of the page-fault floor (~13%).
- **Bucket-index cache + destination prefetch**: floor 65→41–49 ms at 20T. The
  scatter is memory-*latency*-bound (hyperthreading helped), not bandwidth.
- **argSort + apply-permutation** (`logos_extra.c`): turns "a fast numeric sort"
  into "sort records by a numeric key" — the feature that makes it useful.
- **The paradigm experiments** (`experiments/paradigm/`): when the deliverable
  is ORDER or ANALYTICS instead of a sorted array, logos's no-scatter model beats
  the SIMD sort: quantiles 87×, top-k 7×, range-count 42×, rank-model 1.57× on
  low-cardinality. **This is the project's most promising direction.**

## OPEN DIRECTIONS (the ones the data actually endorses)

0. ~~Replace `bucket_of`'s binary search with a direct-index splitter table.~~
   **DONE (H0b).** Shipped: value- and key-space tables, each histogram thread
   trials both on its first two chunks and commits (`bt_init`, `do_hist`).
   Measured (`make stbench`, 20T, loaded box): uniform 1.98×, heavy_tail 2.03×,
   bimodal 1.58×, int_wide 2.99×, clustered 1.89×; guards within noise. The
   prediction (1.4–1.9×) was low. Next lever on the same phase: a branchless
   lookup for alternating-cluster data (bimodal is the smallest gain), then
   over-partition-and-regroup for bucket balance (SELF_SAMPLING.md).
0b. ~~Give `psort_i32` a counting route.~~ **DONE.** `count_try_i32`, shipped: the
   parallel int32 path had no counting, lattice or low-card route and lost to a
   SINGLE core for every span below about n/8. It now reaches a parallel counting
   sort with no probing, because an int32 array is a lattice of step 1 by the TYPE.
   Measured (`make intbench`, 8M): span 100 went from 37.6 ms to 3.8 ms at 14
   threads, P/1 from 0.34 to 2.88, and 0.33 to 3.97 at span 1e3. Guards untouched.
   **Two things still missing from that path**: it has no relocation or near-sorted
   route, and it still sorts its own sample with libc `qsort`, which the f64 path
   removed. ~~one far sentinel destroys the span gate~~ **also fixed**: the table
   window now comes from the sample rather than the extremes, and everything outside
   it spills and is sorted as its own run at the ends. A single INT32_MIN went from
   11.3x the baseline to 1.19x. Limits, by design: the spill budget is n/32 and the
   trim is 5 sample points, so a 5% sentinel still declines, and **the 1-thread
   `leaf_i32` gate is untouched and still pays** (59.9 ms against a 24.4 ms
   baseline). See FINDINGS §3w.
0a. ~~The modcard/lattice serial tail~~ **PARTLY DONE, and the diagnosis moved.** The headline win
   was not in the slow route at all: `lattice_try_f64` was DECLINING perfect lattices whose margin
   pushed them past a flat 65536-slot ceiling, so they fell to the hash route. A thread-scaled memory
   budget plus a margin that shrinks before giving up is worth **3.6x at 14 threads** on 60k-distinct
   lattices (`make mcbench`). What remains of the original target: the serial merge is still serial,
   and the obvious fix for it (concatenate/sort/fold) was MEASURED SLOWER because it sorts `P*d` pairs
   for a union of `d`. The real fix is to partition the key space P ways first, then each thread sorts
   `d/P` pairs with no redundancy; the concat path is already in the tree opt-in as the half of that
   which works. Also still open from the chaos map: the modcard gate lets Zipf-like data in and then
   aborts, which a Good-Turing coverage test (singletons/m > 0.3 means most of the population is
   unseen) would catch before the map pass runs. **Measured and superseded (`make cardgate`, CHANGELOG): the abort was the small problem; the hash map lost at 14 threads on 7 of 8 shapes the old gate admitted. The new gate is Chao1 <= 3300 at 8+ threads and, below that, the leaf-fallback test; a singletons ratio could not have separated the shapes.** Original evidence, still valid: The chaos map has the modcard route 3-7x off a
   hand-routed ceiling on 50-65k-distinct lattices with workers busy only 14-38 ms of an 86-105 ms
   wall; the chunk-queue experiment found its only wins exactly where a serial phase dominates and
   none where thread imbalance was supposed to be. Two concrete pieces: the slot merge walks
   `slots x P` on one thread (split it the way `cnti_body` already does in `count_try_i32`), and on
   Zipf IDs 30.5 of 68.9 ms goes into a modcard attempt that then ABORTS, so the gate is wrong before
   the merge is even reached. See `experiments/team/chaos-explorer.md` and `count-queue.md`.
0b1. **Dense integers now sort by presence** (`dense_int_try_f64`, `make mcbench`): integers with no
   repeats spanning not much more than n get a bitmap, one pass to set and one to walk. 8.8-11.4x at
   ONE thread on dense permutations, exactly neutral at 14 (the route is sequential and is only
   offered to sequential sorts). Residual cost 1.26x on dense data with a few sample-invisible
   repeats. That closes three of the chaos map's four largest single-thread gaps.
0b2. **The sequential dispatcher is where the big multiples are.** At ONE thread the chaos map shows
   4-15x against a sort told the structure on dense integers, Gray code, hash order and
   sorted-prefix-plus-tail, while at 14 threads the partition is already within 1.0-1.4x almost
   everywhere. Nobody has worked the 1-thread route selection.
0c. **The sorted view is built** (`logos_view.h`, `make viewbench`): partition now,
   sort a bucket only when something reads it. `at`, `range`, `topk`, `rank`,
   `count_in_range`, `all`. Median, rank and top-k run 1.5-2.9x faster than sorting
   and looking, because the traced leaf phase is 63% of a sort and a view defers all
   of it; per-bucket extremes recorded during placement let value queries find their
   bucket exactly. Break-even at ~16 scattered queries; materialising everything is
   1.10-1.37x an eager sort, the residual being the one extra pass that lands buckets
   in the caller's array. **This is the working core of direction 1 below.** Next:
   `quantiles(q[])` in one pass over the buckets they touch, `distinct` via the
   cardinality routes, and the i32 twin.
0d. **Route on the (index, value) quadtree profile** (`make zroute`, FINDINGS §3x). Held-out test on
   twenty shapes with the rule fixed from six others: blind on 0 of the 3 cases where the engine leaves
   1.2x or more, every miss on a shape the engine already wins. It is a CLASSIFIER, so its value is
   contingent on building the two routes it points at: a sorted-prefix-plus-tail route (9.36x at 1
   thread, 1.79x at 14) and a sequential stable scatter for disjoint streams (5.91x at 1 thread). Its
   tail rule was too loose; tightened and scored on 15 fresh shapes it cuts false tails from 5 of 7 to 1 of 7 at equal recall (`make ztail`). It cannot see order
   finer than the sample spacing, so it complements a linear scan rather than replacing one.
0e. **Both routes the probe pointed at are built, sequential sorts only.** Sorted prefix plus unsorted
   tail (`make prefixbench`): 3.6-4.0x at one thread on a 95% prefix, 4.3-4.5x on 99%, gated for free by
   where the prescan stops. Interleaved ascending streams (`make streamsbench`): a stable scatter by
   stream sorts nothing, 2.6x on sharded IDs and 3.0x on four streams at one thread. **Both next steps are
   built too:** the tail route's parallel form wins at 14 threads (1.17x at 95%, 1.22x at 90%, 1.17x at 80%, no
   loss at 50%, nothing at 99% where the near-sorted route is already as fast), and a 4096-cell exact table
   replaces the stream lookup's compares from 12 streams up (60 streams went from 1.2x to 3.2x over the route
   off), after the table measured SLOWER than the compares at 4 and 8 streams.
1. **`logos_query` — order/analytics API.** Promote `experiments/paradigm/`'s
   `OrderModel` + profiler into `engines/c/` as first-class entry points
   (`rank`, `select`, `quantiles`, `topk`, `count_in_range`, `distinct`) reusing
   the engine's cardinality/lattice detection to pick exact-vs-sampled. Add a
   `querybench` (fair: vs `nth_element`-class baselines, not just "sort first").
   This is where logos genuinely beats the state of the art.
2. **Structure-router**: probe cheaply; route structured data to logos's
   counting/lattice paths (beats x86-simd-sort), everything else to
   x86-simd-sort wholesale. Would win every parbench row. Honest framing: logos
   as router, SIMD as engine.
3. **GPU/SIMT port of the radix/counting paths.** The one hardware where scatter
   is cheap and *addressing beats comparing* outright — logos's paradigm finally
   wins on its own terms. Cannot be developed on CPU-only hardware.
4. **NUMA-aware partition** (private per-thread buckets, socket-local memory).
   Only measurable on multi-socket hardware.
5. **Port the JS string engine** (LCP-skip MSD, packed-prefix radix, string
   run-merge) to C/Rust. Currently multikey-quicksort only. Own test suite.
6. **Rust v3 parity**: port C's dispatcher (run-merge, momentum, skew/quant,
   lattice) to Rust. Multi-stage.
7. Fewer passes in the coarse path (the engine is pass-overhead-bound). Hard;
   most fruit picked. Fuse partition-scatter with first coarse count?

## Do NOT do

- Don't compare 1T to PT. Don't trust a run with a bad calibration line.
- Don't integrate a microbench win without an end-to-end A/B.
- Don't re-run anything in the ledger without a new angle.
- **Don't spend another session making the memory system happier.** Huge pages
  (page-table traffic) and non-temporal stores (read-for-ownership traffic) were both
  built, both verified to actually fire, and both measured exactly zero. With the
  older ping-pong result that removed 37% of traffic for 12% of time, that is three
  independent measurements saying the scatter is bound by per-core latency and fixed
  costs. Structural levers only: fewer passes, better balance, or skip the scatter.
- Don't claim SIMD parity: logos+SIMD reaches parity single-threaded, never
  exceeds; parallel-SIMD-leaf is measured worse than plain logos.
- Don't bump the version for a change that failed its A/B.

## Build / verify quick reference (engines/c)

```sh
make test            # 1568 checks — must pass
make parbench        # fair fight, no external deps
make parbench-simd XSS=../x86-simd-sort/src            # + x86-simd-sort (AVX-512)
make parbench-full XSS=... IPS4O=../ips4o/include      # + IPS4o (needs libtbb-dev)
make scaling         # 10..1e9 ladder + HTML dashboard
make pfbench         # interleaved-variant A/B template
make j5bench         # disjoint-run relocation A/B (same pattern, with calibration line)
make stbench         # splitter-table (histogram lookup) A/B, same pattern
make leafbench       # leaf knobs A/B (measured negative; template)
make theorybench     # bucket count, branch-free leaf, interleave, line repair; six variants
make profile         # per-phase profile -> profile-report.html (treemaps, stalls, GB/s)
make profile-full    # same, with x86-simd-sort (AVX-512 or AVX2) and IPS4o beside every cell
make scale-full      # time vs n (1e4..3e7) for every sort and shape -> scale-report.html line charts
make noiseprobe      # min/median/max of one sort per thread count: how noisy is this box right now?
make pairprobe       # paired A/B calibration: same engine both sides, so what it prints is the error floor
make mcbench         # counting routes: lattice slot budget, the hash route's tail, the dense-integer route
make chaoshunt       # twenty realistic shapes: logos vs a sort TOLD the structure, plus the route logos took
make zroute          # the (index, value) quadtree profile scored against chaoshunt's zoo, rule fixed in advance
make ztrain          # integrity check: the zroute classifier as coded must reproduce its six training shapes
make ztail           # the tightened tail rule vs the original, on fifteen fresh shapes
make prefixbench     # sorted prefix + unsorted tail route: paired A/B, LOGOS_NO_TAIL off, LOGOS_TAIL_SEQ=1 sequential-only
make streamsbench    # interleaved ascending streams route: paired A/B, LOGOS_NO_STREAMS; `./streamsbench 20 8388608 lookup` = table vs compares
make cardgate        # hash-count route gate: ground truth, ladder, skew, gate, heldout, intness, v2 modes (see its header)
make kgbench         # key grid for heavy tails: paired A/B, runtime toggle LOGOS_NO_KGRID
make zerodiag        # -0.0 / +0.0 counts in and out of a sort at sizes 100..300001: a sort must be a permutation
make streamsprobe    # per-sort wall time of the stream lookups; link against a -DLOGOS_TRACE engine only to see phases, never to time them
make viewbench       # the sorted view: rank/quantile/top-k queries vs sort-then-look, f64; viewbench_i32 for int32
make intbench        # int32 span ladder and the sentinel shapes; i32abbench = the paired A/B of the int32 toggles
make ledgerbench LB0=<flags> LB1=<flags>   # two engine copies, paired A/B; python tools/ledger_rerun.py drives
                     # both orientations and cancels layout bias. Re-tests old "noise" verdicts.
make simdbench       # AVX2 leaf A/B: scalar vs sequential-only (shipped) vs also-parallel vs +int32, 1 thread and P
make chaosbench      # the zoo: 15 shapes, baseline vs Weyl sampling / low-discrepancy probe / per-mode tables / network pivot
make latbench        # lattice window size A/B
make nsbench         # near-sorted route A/B: old vs band-first, with and without plain insertion, at P and P/2
make smallbench      # thread policy A/B: P-thread call vs 1 thread, 100k..8M, per-route caps (SM_SET=1: the cap sweep)
make knobbench KB1=-DX=a KB2=-DX=b KB3=-DX=c   # one inherited constant, four values, 19 shapes; ./knobbench base a b c
make hfbench         # histogram-free partition A/B: old histogram path vs shipped vs no certificate vs slack 0.3
make sodobench       # fixed-order gates vs one two-stage sample answering every gate, 1M and 8M, P and P/2
make probebench      # fast sample (prefetch + AVX2 sort) and persistent context bundles, 1 / P/2 / P threads, 1M and 8M
make cardbench       # counting-route lookup across a cardinality ladder (5..200 distinct, on a lattice and off it)
make tailbench       # heavy-tail structures: key-space certificate, exponent map, dominant duplicate
```
`make profile` before guessing where time goes: it found the histogram
bottleneck that three sessions of A/B tuning never saw. `-DLOGOS_TRACE` prints
per-phase timestamps to stderr (it is how the J5 gate bug was found).
External deps (optional): `git clone intel/x86-simd-sort`, `git clone ips4o/ips4o`,
`apt install libtbb-dev`. AVX-512 kernels are `#pragma GCC target`-pinned so they
compile under WSL2 where `-march=native` masks AVX-512.

Naming: snake_case functions/vars, `*_t` types, UPPER_SNAKE macros (C convention;
a camelCase revision was tried and withdrawn).
