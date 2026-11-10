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
(clones; `make parbench-full`, `make profile-full`), and since 2026-11-05 `engines/highway` (Google's VQSort, Highway 1.4.0,
built into `engines/c/libvqsort.a`; one thread only, `make scoreboard` and `make scale-full`). **VQSort on this AVX2 box is the
slowest of the three vector sorts: 1.36-1.42x behind x86-simd-sort on unstructured doubles at 8.4M, behind on every scoreboard
row, and it reads no order (a sorted array costs it what random data does). FINDINGS 3bt.** AVX-512 not measured.

**State on 2026-11-06** (FINDINGS 3bt and 3bw; `reports/scale-report.html` measured 2026-11-05; the README's table):
at 10M on the AVX2 box logos is the fastest sort measured on 10 of 17 inputs at one thread (level on 5; slower on
int_wide 1.05x and strings: words 1.15x, both against x86-simd-sort) and on 17 of 17 at 14 threads; below 1M at 14
threads it loses to IPS4o on 5-8 of 13 numeric shapes, because the thread policy runs one thread under 100k elements
per thread. The latest engine change is the near-sorted route's strand gate (3bw). The version string is still 3.6.0
and every change since is "(unreleased)" in engines/c/CHANGELOG.md. The whole tree is committed (5 commits, the last
2026-11-06). TRUTH_TABLE.md section S re-grades the September claims; bizdev/ reads the same data commercially.
**Run to 316M on 2026-11-08** (FINDINGS 3bx; `reports/scale-report.html` and the one-page `index.html`; quiet box, calibration
3.23 / 66.8 ms; 1 h 27 min, 0 mismatches; strings and the memory pass to 100M; 1e9 needs ~40 GB and WSL2 is capped at 31 GB
here): at 316M logos is the fastest sort measured on 9 of 17 inputs at one thread (level on 3; slower on uniform 0.87 and
reach_mirror 0.62 against x86-simd-sort, words 0.74 and logs 0.92 against its key-value sort at 100M strings) and on 16 of 17
at 14 threads (words 0.94); against IPS4o at 14 threads 13 of 13 at every size from 3.16M up, 11 of 13 at 1M, 5 of 13 at
100k-316k. Three findings at scale, none visible at 10M: the short-reach gate's ceiling in n (reach_mirror 7.3 -> 20.3 ns per
element from 10M to 316M, 1.44 -> 0.62), the log-line string route past 10M (26.7 -> 70.9 ns per string, 2.26 -> 0.92), and
the in-house kernel drifting behind the library's with n (uniform 11.2 -> 14.0 ns per element against 11.1 -> 12.2, 0.99 ->
0.87). IDEAS_LOG 18. The structured routes' cost per element is flat over the same range (quantized 1.70 -> 1.72 ns).
**Thirteen practical shapes and three practical string columns, run to 50M later on 2026-11-08** (FINDINGS 3by; `reports/
scale-report.html` and `index.html` now hold THIS run -- the 316M report was overwritten by it, its numbers are in 3bx and its
console log is `reports/scale-run-316m-2026-11-08.log`; quiet box, 2.85 / 56.0 ms; 1 h 21 min, 0 mismatches): event_times,
prices_cents, ids_dense, fk_zipf, zip_codes, ages, sensor_temps, latencies_us, geo_lat, log_batches, shard_ids, appended_rows,
order_dates; strings paths, emails, dates (`LOGOS_SCALE_SHAPES=a,b,c` runs a few). At 50M logos is the fastest sort measured on
**25 of 33 inputs at one thread** (level on bimodal, int_wide, geo_lat; slower on uniform 0.95, heavy_tail 0.93, fk_zipf 0.78,
uuids 0.87, words 0.70) and **29 of 33 at 14 threads** (level on sensor_temps, words; slower on zip_codes 0.57 and fk_zipf 0.61
against IPS4o). Four engine findings, none visible on the old shapes (IDEAS_LOG 19): a value holding 5-6% of the array declines
the histogram-free partition at 14 threads and lands in ONE bucket 12x the mean (zip_codes, fk_zipf); at 100k-316k on one thread
the counting routes fire where the leaf wins by multiples (zip_codes 0.18 on a hash map sized by a Chao1 of 1,237 for 42,000
distinct; prices_cents 0.45 on a 147k-slot lattice for 100k elements); prices_cents' lattice entry fires or declines by which tail
values the sample touches (1.00 at 10M, 2.40 at 50M); and log_batches at one thread is level with TIMSORT below 10M (the knot
route's flat 1.7 ns per element against a natural merge's 0.7 at 100k). strings: logs reads 2.11 at 50M, so the log-line
route's turn (0.92 at 100M) lies between 50M and 100M.
**The 50M ladder re-measured on 2026-11-10 with 3bz and 3ca in (FINDINGS 3cb; `reports/scale-report.html` and `index.html` hold
THIS run; console log `reports/scale-run-50m-2026-11-10.log`; 1 h 22 min, 0 mismatches, calibration 3.34 / 63.5 ms, slightly
loaded):** the fastest sort measured on **24 of 33 inputs at one thread** (level on uniform, bimodal, geo_lat, log_batches; slower
on heavy_tail 0.94, int_wide 0.92, fk_zipf 0.72, uuids 0.88, words 0.74) and **29 of 33 at 14 threads** (level on uniform,
zip_codes 1.04, words; slower on fk_zipf 0.72 against IPS4o). zip_codes at 14 threads 0.57 -> 1.04, at 100k on one thread 0.18 ->
0.44. Still owed: fk_zipf at both thread counts (0.71-0.85 of x86-simd-sort at one thread at every size), prices_cents and
zip_codes below 1M at one thread, log_batches against timsort below 50M, and against IPS4o at 14 threads 15 of 26 numeric inputs
at 100k and 6 at 1M. The report this run replaced was a 10M run written at 12:24 the same day, NOT the 2026-11-08 one: some
script's `make` target had run the bench and overwritten it (the ledger's row on recipes that run). The index page now shows TOTAL
TIME per sort beside time per element (a switch above the charts; `NOTES` in tools/scale_index.py is the what-changed section,
edit it when the engine changes), and every chart carries dashed GROWTH GUIDES, O(1) / O(log n) / O(n) / O(n log n) / O(n^2), each drawn from logos's
first plotted point (`guides()` in the generator; a checkbox hides them; in the per-element view O(n) is the flat one).
**The engine changes in it, 2026-11-10 (FINDINGS 3ca; three of the 24 ideas four read-only agents returned, IDEAS_LOG 22):** (1) below 8 threads a hash-count map let in on a Chao1 estimate above 128 gives up at 8,192
distinct values (`MC_HASH_SEQ_CAP`, `LOGOS_MC_SEQCAP=0` = off, `logos_twist_last(40)`): zip_codes at one thread 2.03 at 100k, 1.35 at
316k; (2) the histogram-free partition COUNTS heavy values instead of declining on them (`hv_build`, `LOGOS_NO_HFHEAVY=1`,
`logos_twist_last(41)`): zip_codes 1.66 and fk_zipf 1.71 at 10M and 14 threads, 34 guard rows 0.95-1.04; (3) a new entry,
`logos_argsort_f64_mt` (`logos_twist_last(42)`): the one-thread radix's permutation bit for bit, 3.1-9.8x at 14 threads from 500k to
30M, sorted keys 31-58x (the prescan is in `logos_argsort_f64` too), scratch 20 -> 12 bytes per element.
**The change before, 2026-11-09 (FINDINGS 3bz, after the 50M report was measured): the unstructured partition's own thread
floor** (`BOOST_PER` 25000 from 300,000 elements, threads raised and the bucket count kept; `LOGOS_BOOST_PER=1` turns it off;
`logos_twist_last(39)`): 1.62-1.99x at 316k, 1.28-1.42 at 500k, 1.08-1.13 at 1M with 14 threads asked, guards 0.98-1.03. It is
the first of seven invented ideas (IDEAS_LOG 21).

## Repository layout

```
engines/c/          FLAGSHIP. logos_psort.c (engine), logos_extra.c (argSort,
                    permutations, outliers, strings), logos_simd.cpp (optional
                    AVX-512 leaf + kernels), parbench.cpp (the fair fight),
                    scaling.cpp, harness.c (55,809 checks in 52 sections, 2026-11-10), PARITY.md, CHANGELOG.md;
                    stringbench.cpp (C strings: prefix keys against std::sort and x86-simd-sort's key-value sort; bench_strings.h: the
                    string shapes and the prefix scheme, shared with scalebench.cpp), roundbench.c (any per-sort switch against the
                    shipped engine, f64, 23 shapes with twelve structured guards), strsmallbench.cpp (small string calls, paired),
                    orderprobe.c / symprobe.c / edgeprobe.c (the three invariant tests: order must never cost, isomorphs cost the
                    same, one element across a gate must not cost 10% more), rleprobe.c, tailladder.c, headladder.c, thinbench.c,
                    bandbatch.c, ordkern.cpp (the A/B benches of what those tests found, FINDINGS 3bn),
                    fieldprobe.c (the other-fields round, FINDINGS 3bo: ceilings, the moire / founder / Landauer tests and an
                    A/B mode over the round's shapes), twinbatch.c (any compile-time switch paired on DISTINCT inputs),
                    boundprobe.cpp (the comparison bound per shape, FINDINGS 3br: the bits a comparison sort needs on the input,
                    ns per bit, growth 1M -> 16M in pairs; `ab` pairs two engine settings and says whether the lattice entry fired),
                    xyprobe.cpp (X+Y, the sort on the quadratic line, FINDINGS 3bs: the N = n^2 pairwise sums, the engine blind and a
                    sort TOLD X and Y; `phases` splits the told sort and prices the order inside its buckets),
                    primeprobe.cpp (a knot solver that unties only the PRIME FACTORS, FINDINGS 3bv: pinned arcs never moved, the
                    tangles between them untied in place; standalone, paired with the engine at 1 and P threads; PRIMEPROBE_DIAG
                    times the engine alone on a shape's complex factors),
                    strandgate.c (the near-sorted route's STRAND gate, FINDINGS 3bw: 27 shapes, the statistics a gate could read
                    beside a paired timing of any switch against the shipped engine; the table the gate was set from),
                    memprobe.c (what a sort weighs; rssprobe.c: what it keeps RESIDENT; mixprobe.c: what one process keeps across a mixed sequence), faultprobe.c (every allocation and thread creation
                    failed in turn), slopebench.cpp (the scaling exponent in pairs), ipbench.c (in-place, parallel),
                    reachhunt.cpp (ceilings for short-reach disorder), reachbench.c / nsgatebench.c (their paired A/Bs),
                    callbench.c (what ONE CALL costs at the small end, a baseline engine against the working tree),
                    bandbench.c (the leaf's choice of kernel, SIZED: two copies of today's engine differing by one flag,
                    on the shape that actually reaches those paths -- callbench's does not),
                    twistbench.c (the Moebius twist: its ceiling, its A/B, its two merge loops),
                    dllbench.c (the proposed Moebius DLL sort in C, as written and relinked, paired with the engine: slower on every shape),
                    unknotprobe.c (the unknotting problem: a circular doubly linked list unknotted by Reidemeister moves, a ring buffer
                    against itself, the relocation floor's ladder, and the A/Bs of the loop-back, the floor, the seam, strays and the wave;
                    `nocopy` prices the copy-back, `lis` reads the unknotting number from the sample),
                    tsbench.cpp (server timestamps with slight knots: the engine against std::sort, a boundary-bypass merge sort and itself
                    with the order routes off, sixteen log and sensor shapes; round two: the edge sort (blocks sorted by min edge) and
                    the A/Bs of the three fixes it found, plus a null switch for a guard that dips once. The round-three weld and the
                    round-four Moebius handoff were REMOVED 2026-09-23 once they had lost (git history before that commit); the five
                    engine fixes those rounds found are shipped and their A/Bs remain),
amprobe.c / capbench.c  where the parallel speedup stops (the Karp-Flatt serial fraction per route), and the
                    counting-route thread ceiling that did not pay
unibench.cpp        "uniform as a complete computation": P chunks sorted by the AVX2 quicksort and merged by a merge-path tree,
                    against the shipped partition (needs the x86-simd-sort clone; `anat` prices the chunk sorts against the merge)
                    mirrorprobe.c (where else an orientation bit pays: every cheap route's shape against its mirror image; `ab` = the
                    whole-array bit's A/B), strandprobe.c (strays and KINKS inside strands: ceilings, and the kink repair's A/B; writhe and persistence)
engines/rust/       v2 engine + in-place + extras. BEHIND C on numerics (see PARITY.md).
engines/js/         Original engine (npm logos-sort). Most complete STRING engine.
engines/python/     Sequential reference.
experiments/ORDER_FROM_CHAOS.md  next brainstorm: golden-ratio levers, pivots, mechanisms from
                    scripture/philosophy/biology, each with a prediction (TRUTH_TABLE §M)
experiments/OUTSIDE_THE_BOX.md   round two, 2026-09-12: hardware the project has never touched
                    (huge pages, streaming stores), knot placement by curvature, a strided-order
                    probe, relocation by page table, the lazy sorted view. Each with a written
                    prediction. START WITH A1 (huge pages): measured zero AnonHugePages today.
experiments/SHORT_REACH.md      round three, 2026-09-20: what else looks like run detection and unknotting? Six analogies with
                    predictions; one built (the short-reach route), two measured and not built, three unmeasured.
experiments/TWIST_AND_KNOT.md   round four, 2026-09-21: where the Moebius twist meets the unknotting view. Seven avenues with predictions.
                    BUILT: the detector's scan in blocks of eight (3b) and the whole array's orientation bit. MEASURED, not built: the
                    writhe classifier (5) and strays inside strands (2, the next build). Untouched: ribbons, oriented k-way merges,
                    descending output, the int32 twin. Its status table is at the top of the file.
experiments/moebius_dll/  the proposed Moebius DLL sort exactly as given (JavaScript) and its bench against the JS engine and V8's sort.
experiments/HARD_UNKNOTS.md   round five, 2026-09-21: Kauffman & Lambropoulou's hard unknots as a sort. The dictionary (R0-R3,
                    hard states, swings, tangles), the unknotter (a dead end as a sort), and the two engine changes it led to.
experiments/OTHER_FIELDS.md   round nine, 2026-10-21: ideas from outside computer science and mathematics (Kleiber's law,
                    hub and spoke, TDM, riffle shuffles, moire, the founder effect, Landauer, immune memory), each with its
                    prediction, the A/B table and the hardware question's queue.
experiments/IDEAS_LOG.md   THE INTAKE QUEUE: every idea raised, mapped onto the engine, with what is already
                    measured about it and the ONE next measurement that would settle it. Ranked by measured
                    upside; settled entries stay with their verdict. Read it before starting a round, and add
                    to it when an idea arrives (the ledger is for what FAILED, this is for what is queued).
experiments/paradigm/   The "change the deliverable" experiments (the strongest
                    result in the project) + merge/scatter negative results.
experiments/parallel-js-prototype/  worker_threads sample sort; not productionized.
engines/c/gen_small_net.py  generates the leaf's branchless sorting networks for 4..16 elements, and proves
                    each one by the 0-1 principle before writing it into logos_psort.c
tools/sortbench/    Python benchmarking lib (numpy/sorted/SQLite/DuckDB vs logos).
tools/truth_table_html.py   renders TRUTH_TABLE.md -> reports/truth-table.html
tools/embed_uplot.py        bakes engines/c/vendor/uplot.* into engines/c/uplot_embed.h
tools/check_report.js       runs a generated report's script under a stub DOM and a
                    validating uPlot stub; `make scalebench` / `make profile` call it. The stub keeps
                    scale state and DRIVES every chart's zoom and pan; it rounds a log window the way
                    uPlot does and refuses a log axis without its own ticks
tools/ledger_rerun.py       re-tests a ledger verdict against today's noise floor: builds each
                    feature twice with the engine copies SWAPPED, measures both with the
                    paired harness, and reports sqrt(A/B), which cancels the code-layout
                    bias exactly. Use it before trusting any old "neutral" verdict.
tools/browser_check.py      renders a report in real headless Chrome/Edge and fails on any
                    uncaught error, both axis modes (--shot for a screenshot). Use it
                    whenever a chart looks wrong: a stub DOM only produces the inputs
                    you thought of, and it once passed a report whose log panels were
                    entirely blank. It also DRIVES the first panel (wheel, keys, drags, pinch)
                    in six modes: that found a log axis that could not zoom and a pan that
                    killed the tab, both inside uPlot, both passed by the stub.
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
   54,832 property checks in 48 sections (2026-11-06; the resource section's count
   moves with thread timing); keep them green.
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
11. **Run what has never run.** Timing discipline says nothing about a path that is never taken. On 2026-09-19/20 every condition
    the harness had never produced found a defect, under thousands of green checks: an infinity (a crash), subnormals (unsorted
    output), a NaN payload (rewritten), a NaN where two partition scans meet (a sort that NEVER RETURNED, about one run in
    fifteen in C and every time in JavaScript), a failed allocation (unsorted output, crashes: 2,215 of 8,316 injected failures ended
    badly), a failed thread creation (wrong output or a hang: 154 of 186). A fallback that has never executed is a guess. The
    tools: **`make faultprobe`** fails every allocation and every `pthread_create` of a cold call in turn, in forked children, so
    a crash or a hang is a result; the harness repeats a sample IN-PROCESS on every `make test`, which finds what a fresh process
    cannot (a free of uninitialised stack passes in a child whose stack is zeros); **`make sanitize`** and **`make tsan`** run the
    whole harness (`float-cast-overflow` is NOT in `-fsanitize=undefined` and is the check that finds NaN-to-index casts);
    **`make memprobe`** weighs a sort, which no timing bench does. When you add a route, a fallback or a pool, fail it once.
    An intermittent failure is chased with `LOGOS_HARNESS_ONLY=<section> LOGOS_HARNESS_REPEAT=N ./harness`, and on a box with no
    debugger a handler that prints a backtrace plus `tgkill` on the spinning thread stands in for one.
    And a crashed harness's redirected log lies about WHERE: stdout was block-buffered (line-buffered now).
    The same goes for switches: leak checking had been turned OFF in `make sanitize` and hid a real leak; two A/B flags
    (`-DLOGOS_NO_SPLITTER_TABLE`, `-DLOGOS_NO_RELOCATE`) did not compile. Build every flag, run every checker.
    And a test must be able to tell WHICH path ran: the short-reach route's harness section passed with the route never firing at
    one thread, so `logos_reach_last()` now reports the branch taken and every row asserts it by name. The same goes for a
    statistic: the shared sample's inversion rate was documented as ~1900 pairs and read 7 (2026-09-20). Count what it reads.

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
| AVX2 quicksort inside the PARALLEL leaf (`-DLOGOS_AVX2_PARALLEL`, `make simdbench`) | **−5 to −28% at 14T, −10 to −16% at 6T** on every random shape | an L2-resident 33k bucket already sorts at ~9 ns/elem scalar; the vectorized quicksort is 8.5 at 1T and slower under 14 on a hybrid P+E CPU. Sequential it wins 1.2–1.6×, so it ships sequential-only. **SUPERSEDED 2026-10-19 (FINDINGS 3bm): measured again after the kernel bridge (3bh), the sequential leaf's policy in the partition wins where BUCKETS are past L2 -- 3 threads 1.32-2.09x, 6 threads 1.05-1.12, 14 threads at 30M 1.05-1.24, 50k buckets neutral -- and ships gated on bucket size and an unstructured sample (`LOGOS_NO_SEQLEAF=1`)** |
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
| Range gate from raw min/max on integer data | **catastrophic on sentinels**: one INT32_MIN in 8M narrow values cost 11x at 14 threads. FIXED in `count_try_i32` by windowing from the sample and spilling the tail (11.3x -> 1.19x); and at 1 thread by `leaf_i32_windowed` (the same window, `LOGOS_I32_NO_LEAF_WINDOW=1` is its switch; this row said "still has it" until 2026-10-20) | a sample bounds the bulk, never the tail, and a sentinel is a tail of size one. Take the window from the sample, not the extremes (FINDINGS §3w) |
| Bucket-relative digit narrowing for the int32 leaf (H5, first cut) | **mixed** (int_wide +21%/−6%, int_1e6 −16%) | it adds a min/max pass the fused byte histogram never paid. Opt-in `-DLOGOS_INT_NARROW`; redo with the bucket range passed down from the splitters |
| Cutting every run endpoint BELOW its value in the knot route's sweep (first cut) | **98.5% crossing mass** on a staircase built with 10% | a strand's last element landed at the head of the next interval and turned the neighbour's whole middle into a two-strand crossing. A first value cuts below itself, a last value above. Caught by the trace build (`make knottrace`) before anything was timed |
| Gating the knot route at J5's n/2, on a predicted break-even (first cut) | **0.92-0.96** on the staircases just past the gate, and 1.2-2.9x forgone on them | a probing route's decline comes AFTER its probe (a full run scan), so a cautious gate charges for what it refuses. Lift the gate, measure the ladder, then set it (FINDINGS §3ac) |
| Reading a parallel route's win off the shipped engine alone (knot route, 14 threads) | 1.2-1.5x against the shipped engine, **0.96-1.11 against the pure partition**, 0.89 on three strands | the shipped relocation scans a shuffled staircase for 2.8 ms and THEN declines, and that sat in the baseline. Compare against a copy with the route's whole family compiled out (`-DLOGOS_NO_RELOCATE`), both orientations |
| Inheriting relocation's one-thread-per-500k cap for the knot route's merges | **0.84-0.99 at 1M, 14 threads**, on rows that won 1.2-1.5x at 8.4M | the cap was measured on copies, which are bandwidth-bound; a merge is compute-bound, and 2 threads merged what the partition sorted on 10. The cap now counts work (n + 3 x cost): 1.26-1.43 |
| `isfinite(scale)` as the range check of a value-linear map | **SEGV, or unsorted output**, on 8 of 10 shapes with an infinity at a sample point (`a[0]` always is one) | `cells / inf = 0` is finite, and `inf * 0 = NaN` fails every compare, so a KS loop measured a distance of 0 and CERTIFIED the data. Require `isfinite(hi - lo)` and a scale that is finite AND positive; test with `-fsanitize=float-cast-overflow`, which `undefined` does not include |
| Knot route, first cut: switches read with `getenv` on every sort, and a cost gate with no minimum run length | **three sorted elements 6 -> 42 ns; random n = 32 781 -> 2431 ns** (3x), under 2059 green checks and a clean 40-pair A/B | two `getenv` calls are ~40 ns; and with runs of two or three elements nearly every element is a run endpoint, so nothing "crosses", the gate says go and the sweep sorts the slow way. **Every A/B bench here starts at 1M and the harness checks answers, not times: a cost per CALL is invisible to all of them.** Found by `make scale-full`'s new small sizes; read its small end after touching the dispatcher's first page |
| Sizing every thread's context by the LARGEST bucket | **104.5 bytes per element at 14 threads** on one value holding 90% of the array, a gigabyte for an 80 MB array; 17.8 after | that is O(P n) the moment one bucket cannot be divided. The buckets in hand at one moment are disjoint and sum to at most n: size for a typical bucket, let the oversized one borrow (`ctx_fit`). Speed: neutral, 0.98-1.06 against the committed engine at 8M in two paired runs, both copy orientations |
| Asking the allocator for a large block and re-asking aligned if it is not (the histogram-free partition's regions) | **every cold parallel sort held 12.8 bytes per element twice** (34.9 peak against 23.2) | glibc serves a block that large from mmap behind a 16-byte header, so it is NEVER 64-byte aligned and the retry ran every time. Found by logging allocations with their call sites (`MEMPROBE_LOG`), not by reading |
| Guarding the RANGE of a value-linear map (`isfinite(max - min)`) and not its scale, in the leaves | NaN-free data holding **subnormals came back UNSORTED** from every parallel sort of 300k elements and up | a range narrower than ~1e-305 is finite and its reciprocal is not: `classes / range` = +inf, `0 * inf` = NaN, clamped into the LAST bucket. Guard BOTH, everywhere a map is built: the range finite, the scale finite and positive |
| Reading a fallback as working because it is written | **2,215 of 8,316 injected allocation failures and 154 of 186 injected thread-creation failures ended in wrong output, a crash or a hang** (`make faultprobe`) | no `pthread_create` result was read at any of 23 sites; `merge_all_runs` returned with its runs unmerged; a context that failed to allocate was still handed to the leaf by every worker, because only the caller read the flag |
| A forked fault probe as the only fault test | it **passed** a free of an uninitialised stack context on every one-thread run; restored in a scratch copy, the same code aborts the in-process harness (`munmap_chunk(): invalid pointer`) | a fresh child's deep stack is zeros. Fork to be able to report a crash or a hang; run in-process to give memory a history. Keep both |
| Reading the scaling exponent off two rows of the scale report | not attempted as a result: the rows are minutes apart and the box drifts by a multiple. In PAIRS (`make slopebench`) the null reads ±2% and `std::sort` p = 1.063–1.066 against a floor of 1.062 | one 50M sort against twenty-five 2M sorts, adjacent, order alternating. One-thread cells reproduce within ~3 points, 14-thread cells within 10–17, and one cell (block_perm in-place, 18 against 38) does not reproduce at all: quote two runs |
| Predicting the 14-thread slope as "about flat" | **wrong on six shapes of seven, in both directions**: uniform +26–34% per element from 2M to 50M, the structured routes −31% to −60% | 2M doubles sit in the 24 MB last-level cache and 50M do not; and at 2M a parallel call on the counting, lattice, near-sorted and relocation routes is mostly FIXED cost. Spawning and joining 13 empty workers costs 0.77 ms here (about 60 us a thread), so a route with two spawn phases pays 1.5 ms of a 4.6 ms call at 2M. |
| Predicting the parallel in-place sort "3–6× behind the buffered sort" at 14 threads | **9.8× at 8.4M, 6.0× at 50M, 15–24× on the structured shapes** | the first placement pass stays on one thread (a serial share of 30–35%), and the structured shapes' one expensive pass IS that pass. Zero scratch is bought with time |
| Turning leak checking off in the sanitizer run | **a real leak sat behind it**: 313 KB in 28 allocations over the harness, the dispatcher's sorted sample freed only on the fallback label when it is not handed to the partition (a NaN in it) | the pools are reachable from statics, so LeakSanitizer never reported them; `make sanitize` runs with leak checking on now |
| Keeping an A/B switch without building it (`-DLOGOS_NO_SPLITTER_TABLE` / `make stbench`; `-DLOGOS_NO_RELOCATE` / `make j5bench`) | **neither compiled** | the per-mode certificate read the splitter table's modes without asking whether there was one. A flag nobody builds is a fallback nobody runs; all 49 Makefile targets with a compile step build now |
| Assuming a defect found in one engine looks the same in the others | the subnormal scale overflow is **unsorted output in C, a 77 s quadratic stall in JavaScript (output correct) and nothing in Rust** | C casts the NaN to an index; JavaScript's `x * Infinity \| 0` is 0, so every element lands in bucket 0; Rust's casts saturate and a concentration check bails. Run an edge input on every engine: it also found a second JS cliff, a dense cluster in one flash bucket (13-21 s) |
| Partitioning with two different tests (`a[i] < pivot` from the left, `a[j] >= pivot` from the right) on data that may hold a NaN | **the sort never returned**: about one harness run in fifteen in C (the meeting point depends on thread timing), every time in JavaScript on `[lows..., NaN, highs...]` | a NaN fails BOTH tests, so when the scans meet on one neither index moves. Classify with ONE predicate and its negation. 'Order unspecified' still has to return: test it under a watchdog |
| Cut decomposition as its own route (cut wherever everything before is no larger than everything after; `make reachhunt`) | told the cuts: 1.64x at 1 thread, **0.30x at 14** on 2048-element windows; found blind 1.11 / 0.24, the detection alone 35 ms | a cut is a block boundary with an empty seam, so the short-reach route subsumes it without a detection pass. Ranges under 4096 elements go to a scalar quicksort in a parallel sort; a suffix minimum and a running maximum are dependency chains, not loads |
| Chunk relocation of unsorted chunks with disjoint ranges ("links"), told the segments | 2.0x at 1 thread, 1.3-1.5x at 14 on three value bands, **not built** | the margin is the baseline's weakness: the engine runs banded data at twice its uniform time. A ceiling measures the baseline too; fix the weakness, not the symptom |
| Putting strays back through a second buffer (the short-reach route, first cut) | **+36 ms at 1 thread, +12 ms at 14, for ONE stray** | between two strays every element moves by the same amount: one in-place pass of `memmove`s over exactly the span the strays travel (left-movers first from the left, then right-movers from the right) |
| Stopping the short-reach block phase when strays keep turning up | **dominated**: strays in 28% of blocks at 14 threads, 0.78x when the phase stopped itself, 1.24x when they were simply put back | a late stop wastes what was sorted; putting strays back costs one pass at most. Decide before anything moves (eight probe blocks), or not at all |
| Sorting the short-reach route's blocks with the general leaf, or with the AVX2 quicksort at 1 thread | general leaf: the route measured 1.15-1.53x at 1 thread where the flash kernel gives 1.9-2.5x; AVX2 quicksort loses to flash by 1.26-1.81x on these blocks | on locally ordered data classification is nearly the identity, so a flash sort never leaves the cache it is already in. "AVX2 when sequential" is right for UNSTRUCTURED buckets only |
| "A small span means integers: send the block to the counting leaf" | every series with unit spacing (index + noise) spans just under its length: 293 ms at 1 thread on 64-element windows through the general leaf, 76 through the kernel | a type fact may be assumed, a data fact must be certified: test elements for integrality before routing on it |
| Trusting a statistic's comment ("an inversion rate over ~1900 Fibonacci-lag pairs") | the loop read **7 pairs, a whole array apart**; it passed gray-code, sawtooth, block-permuted, staircase and grouped-run data as nearly sorted and turned away one near-sorted input in seven | sample indices i and i+144 are 0.31% of n apart, but the later index is the EARLIER position (frac(144 phi) = 0.99689), so the `p1 <= p0` skip meant for wrapped pairs dropped every adjacent one. Count what a statistic reads |
| A pre-gate that is a simplified copy of its route's scan (the near-sorted pre-gate, without the route's poison recovery) | declined 12 of 40 seeds of sorted data with 1% random swaps and 38 of 40 with 2%; with the 7-pair statistic, sorted runs grouped by a coarse key ran **5.8x off the ceiling at 14 threads** (the route ran, rejected 73%, declined after 57 ms) | a pre-gate runs the route's OWN scan on a sample of windows. Repaired: 3.8x on that shape, 1.3-1.5x on 1% swaps, 1.2-1.4x on 2% (`make nsgatebench`) |
| Calling a bucket nearly sorted from 40 pairs at ONE stride, then trying an insertion sort with an 8n budget | dense disorder shorter than the stride wastes the whole budget: 1.36-1.87x at 14 threads on shapes the short-reach route declines once 64 pairs EIGHT apart decide first; the JS engine's small case had NO budget (45,000 elements with 1% swaps: 47 ms against 2.3) | the budget is a mean displacement of 8, and an element displaced further is usually inverted with its eighth neighbour |
| One inversion gate (5%) for the near-sorted route at every size | the ROUTE against the partition (`NSG_ROUTE=1 ./nsgatebench`), 14 threads: 2% swaps **0.84-0.87 at 1.5M**, 0.94-0.99 at 3-4M, 1.33-1.41 from 6M; 3% swaps **0.75-0.80 at 1.5-2M**; 5% swaps 0.71 at 8.4M with the gate lifted; 1% swaps and fewer win everywhere (1.07-2.1x) | three spawn phases and one stray sort are fixed costs. The gate is now 2.5% below 6M elements, 5% from there on. Nobody had seen that table: the old pre-gate turned 38 of 40 such inputs away at any size |
| A harness row demanding identical BYTES from two parallel runs on data with a NaN | it passed only while the broken gate sent its input down a deterministic route; read properly, two runs of the SAME configuration differ in 21 of 40 tries | with a NaN the engine promises a return and a permutation. A parallel partition's bucket order depends on thread timing: check the promise, not reproducibility. Seen first as an unexplained failure under `make tsan`, whose `tail -3` cut the FAIL line off; loop the plain harness and keep the output |
| Reading a shape's cost without sorting its own values in random order (the sine wave with a slope) | the wave plus noise reads **1.27x a uniform sort at one thread**, and the same VALUES SHUFFLED read **1.01**: the cost is the ORDER, and the array is slower than the same numbers with no order at all | two controls are needed before a shape's time means anything: the same noise without the structure (0.59-0.63 here, the short-reach route takes it) and the same values without the order. A `-DLOGOS_PROFILE` build then named the cause: the natural order takes the COARSE leaf where the shuffled multiset takes the AVX2 quicksort, because `AVX2_TAKE` requires `!c->nearly` and 40 strided pairs call a sloped wave nearly sorted. **THAT NAMED CAUSE WAS WRONG (2026-09-21): see the row on clearing `c->nearly`** |
| Clearing `c->nearly` when the leaf's dense probe fires, so that `AVX2_TAKE` admits a noisy sloped wave (0i (f), predicted 1.45x) | **neutral: 1.02**, one binary (`./sineprobe 40 4194304 ab`). Reverted | the diagnosis ranked the coarse leaf against the AVX2 leaf by a PROFILE build's times (126 ms against 87); paired in a normal build they cost the same on this shape. With `LOGOS_NO_AVX2=1` the wave and its own values shuffled both read 1.05 against uniform; with the AVX2 leaf on, 1.19 and 1.01. **The order cost is inside the AVX2 quicksort.** A profile build says which path ran and what share of its own time each phase took; it does not rank two leaves |
| Judging a round's cost per CALL from benches that start at 1M, then from one row of one small-call binary (`make callbench`, new) | the short-reach round as built cost **1.09-1.14 per call at n = 0, 1, 2** and 1.03-1.09 at 3 and 5, under a full set of clean A/Bs; and a never-executed loop moved out of line "won" 17 points at n = 100 | three causes, three verdicts. MEASURED, fixed: a helper with two 16 KB arrays was inlined into the dispatcher, whose prologue probes every page of its frame on every call (8 probes -> 10); `noinline`, 0.98-1.01; check frames with `-fstack-usage`. NOT RESOLVED, removed because removal is free: a thread-local reset put two `__tls_get_addr` calls (thread-locals are calls under `-fPIC`) on every sort's path; predicted +20-35% on a sorted triple, measured +5% / -14% by orientation. LAYOUT, not shipped: rows from 10 to 100 elements read 0.78-1.13 across nine builds with IDENTICAL logic on their path, reproducibly, in two bench binaries. **Reproducible is not real**: believe a small-call row only at n <= 5, in both orientations and two runs, with a mechanism visible in the assembly |
| One merge loop with a signed stride per strand for every orientation pair (the twist's first two-ended merge, `LOGOS_TWIST_GENERIC=1`) | the block rows recovered their ceiling, the staircases did not: every other block twisted read **1.000 and 1.002** at one thread against a ceiling of 1.067 | four cursor updates per element where the untwisted merge has two. One loop per orientation pair: 1.05-1.06 over it on x20 staircases, 1.10-1.11 on x80, same binary (`./twistbench 120 8388608 merge`). A ceiling per SHAPE showed it: the rows that fell short were the rows that merge |
| Predicting the twist's one-thread ceiling from "a run route costs 2-9 ns per element" | predicted 1.03-1.08, measured **1.07-1.17** | the run routes cost 2.3 ns per element at one thread, so one swap pass is a sixth of the sort. The cheaper a route already is, the more one avoidable pass is worth: look for passes on the FAST routes |
| Naming plain relocation as the path for disjoint blocks at every thread count (the twist's harness section, first cut) | at 1,000,003 elements and 8 threads, knot route off, **nobody placed them**: relocation declined 64 shuffled disjoint blocks | a detection chunk began 7 elements before a block's end; a body under `RELOC_MIN_BODY` (16) has no gap test, so the 7 fused with the next block into one run whose interval hid holes (`merged = 62511` of 93,763 seen). Older than the twist, masked by the knot route, NOT FIXED. Found only because the row named its path |
| The orientation bit one level down, in the leaf's `detect_runs_limited` (`make mirrorprobe`) | **not built: ceiling 1.02-1.06** on 6 and 64 descending teeth, 8.4M and 3,000 elements | at one thread the knot route takes few-run data first and already carries the bit; what still reaches the leaf's detector is sorted at ~16 ns per element, where a swap pass is a thirtieth of the work. The twist pays on CHEAP routes only |
| Adding a public symbol (`logos_twist_last`) without adding it to the Makefile's `RENAME` list | **every multi-copy bench stopped linking** (`callbench`, `ledgerbench`, `pfbench`, `tailbench`, ...), and nobody noticed for a round | "all flag variants build" compiled the ENGINE with every flag and never re-linked a bench. A new public symbol goes into `RENAME` in the same edit, and one multi-copy bench gets rebuilt as the check |
| Pre-gating the mirrored STREAM probe on the sample's writhe (`inv_all >= 0.60`), first cut of the whole-array bit | four mirrored streams read **0.88 at 1,048,576 elements and under 0.60 at 1,000,003**: the route never fired there | whether a sampled pair falls inside one stream depends on the lag modulo the stream count. A statistic over strided pairs aliases with any period in the data; ask the route's own probe instead. Found by a harness row that named what must happen |
| Predicting the stray ceiling (3-4x) from the route's cost alone (`make strandprobe`) | measured **2.0-2.2x** at 1% strays, one thread | the told sort pays two serial passes of its own (0.61 on clean blocks). **Put a 0% row in every told-the-structure ceiling: it prices the teller** |
| The knot route as a LITERAL linked list (`LOGOS_KNOT_LIST=1`, `./twistbench 60 8388608 list`): a 32-bit link per element, twisted runs linked backwards, crossings untied by relinking, the sorted list walked out | **0.50-0.57 at one thread on blocks and x20 staircases, 0.73-0.75 on x80, 0.62-0.69 at 14 threads**, and 4 more bytes per element | the array is NOT converted to a linked list and never was: the shipped route is an array of run descriptors. The list pays a link written, a link read and a scalar walk per element for work a memcpy does eight bytes to the cycle; it gives up least where merges dominate (a relinking merge moves no data) and the twist is free in it. Kept opt-in with its own harness section. **RE-MEASURED 2026-11-05 on today's engine, asked again as "a Moebius strip in a doubly linked list, then knot solving" (FINDINGS 3bu): the list 0.40-0.46 at one thread on all nine structured rows (x80 staircases 0.41, where it had read 0.73-0.75: the vector merges took away the list's one soft spot), 0.61-0.66 at 14; the Reidemeister unknotter 0.007-0.22 on 16 of 16 rows (its one win was the floor cliff, fixed); the plain DLL sort 0.006-0.49. Three implementations, two rounds, no row a list wins: do not build a fourth without a mechanism that removes the per-element link write and walk** |
| A value-speculating list walk (guess next = cur + 1, the load only checks) in plain C | measured **the same as the plain pointer walk** (0.43 against 0.44) | on the path where guess and link compare equal the compiler continued from the LINK and the dependent chain was back. Launder the difference through an empty `asm`: 0.43 -> 0.51. When a speculation trick measures like the code it replaces, read the assembly |
| Running the galloping merge's lookahead on every two-strand crossing (first cut) | the staircase where most elements merge read **0.948-0.980** over three runs | a balanced crossing never gallops far and the call site knows both lengths: only pieces that differ fourfold take the galloping kernel (1.004 after) |
| Predicting a route's value from its OWN cost (the dense bitmap past 4n: "16 MB is most of the last-level cache") | predicted a loss by 32n; measured **5.0x at 6n and a win to 64n** | the alternative decides: the leaf there is the int32 radix at 35 ns per element. Price what the route replaces, not only the route (the same lesson as the 1-thread counting gate) |
| A harness row meant to test a gate, built from a SORTED array (the dense route's "span 8n: must decline") | passed for months without reaching the gate: the prescan returns a sorted array first | rows that reach a route are shuffled and say which path ran (`logos_twist_last(6)`); under the old budget exactly the rows expecting the bitmap fail |
| The proposed "Moebius twist pointer routing" sort, implemented as given (array -> doubly linked list, descending runs read through `.prev`, runs merged pairwise; `make dllbench`, `experiments/moebius_dll/`) | **slower on every shape in both languages**: C one thread 0.03-0.51 as written, 0.01-0.23 relinked, the sines it was written for 0.16-0.29; JS 0.07-0.30 against the JS engine | a natural merge sort with a list in front: a node per element before any sorting, then log2(R) full merge passes. Its one idea, reading a descending run backwards, is already the engine's twist. It beats V8's own sort on uniform (1.18-1.33) only because V8 calls a comparator per compare |
| The unknotting problem as a sort (Kauffman & Lambropoulou): a circular doubly linked list, R1/R2/R3 as local rewrites, swings (cuts at gaps) and tangle collapse (`./unknotprobe 20 8388608 moves`) | **slower on 15 rows of 16 at one thread**: near_sorted 0.36, block_perm 0.26, staircases 0.20-0.29, sorted 0.14, short reach 0.08-0.11, uniform 0.009; the one row above 1 (1.40) was the engine's floor cliff | a list pays a node written and walked per element before any move, and the engine's global reading (the knot route's sweep) is what the paper says local moves lack. The counts are the result: ONE hard state in sixteen shapes (the shuffled staircase), and the swing, a move that complicates first, took walked nodes per element 21.5 -> 0.00 on near_sorted |
| Predicting a rotated sorted array rotation-blind "because relocation sorts descriptors by value" | **1.99x at one thread, 2.59x at 14** over the sorted array | relocation never saw it: the TAIL route took it first (the prescan's prefix is 63%) and merged the second run in element by element. Trace which route takes a shape before predicting from the route you expect |
| The loop-back's first cut: rotate, then enter the whole dispatcher again | **0.89 at one thread** on the ring buffer and on two swapped blocks | the re-entry's prescan re-read an array whose sort is three or four passes long: the redundant pass was the whole margin. Handing it the known ascending stretch (and returning at once when that is everything): 1.56-1.94 |
| Forgiving each run-detection chunk its partial first run, for runs of exactly the floor at 14 threads (predicted 1.1-1.4) | **0.974-1.026 everywhere**, reverted | the per-thread run cap (n / floor / P + 2) binds before the average test does: diagnose the condition that actually fires |
| Hinting the dispatcher's prescan where to start (the loop-back's re-entry, second cut: `for (i = i0; ...)` in place of `for (i = 1; ...)`) | **every SORTED call of 2 to 32 elements 10-16% dearer, in both orientations** (`make callbench`, n = 2: 16.6 -> 21.3 ns), random calls of 2-5 elements 2-9%, the frame unchanged, under a clean 40-pair A/B at 8.4M | a loop whose start is a variable is a different loop, and on a sorted array of 30 elements that loop IS the sort. The re-entry now calls the tail route directly. Third time the first page charged for something a bench at 1M could not see: run callbench after every change to the prescan |
| A Weyl-position pivot sample for the AVX2 quicksort, against the wave's 1.17x order cost there (`./unknotprobe 40 8388608 ab`, rows PIVOT, 2026-09-22) | **0.996-1.009 on every row**, two runs; reverted | the second diagnosis of that cost to fail (the first was the leaf choice). The cost is inside the quicksort and is still not traced |
| Direct three- and four-way merges for the knot route's crossings, one pass from the source instead of staging (rows DIRECT) | **0.68 on three live strands, 0.52 on four** at one thread; reverted | two dependent compares and a chosen load per element lose to two memcpys and branch-free two-way merges. One pass is not cheaper than three when its chain is longer |
| Allocating a route's side list before it has anything to hold (the stray list, 1 MB at 8.4M, on every sort) | **0.94-0.96 on clean block shapes** that never found a stray | take it at the first use: 0.99-1.01 |
| The wave route without a crossing-cost gate | **0.76 on a swing of two periods**, while a swing of 0.6 won 1.35 | pieces covered four and five deep make the sweep's k-way merges dearer than the leaf; the sample prices it before anything is sorted |
| A merge sort's O(1) boundary check (`a[mid] <= a[mid+1]`: skip the merge) plus reversing small descending runs, claimed O(N) on "macroscopically ascending, locally noisy" data such as server logs (an outside write-up; `./tsbench 20 8388608 anatomy`) | **skipped 0 of 262,143 merges on every shape with network latency** (a full merge sort, level with std::sort; the engine 3-6x faster); 100% on sorted data and adjacent swaps, where it beats the engine 1.6x | latency reaches farther than the recursion's 32-element blocks, so no two adjacent halves are ever in order. The floor for displacement d is ~log2(d) comparisons per element: sort windows where they lie and repair the seams (the short-reach route) |
| The edge sort (an outside proposal): condense into natural runs, sort the runs by their MIN EDGE, lay them out, let the boundary-bypass merge finish (`./tsbench 16 8388608 edge`, `edgeanat`) | **1.4-9.9x behind the engine on 13 of 16 shapes, and slower than the hybrid it feeds on 15 of 16**; K is 41% of N on noisy data (runs of 2.4) and the block sort 15-47% of the pipeline | a maximal run swallows the next block's first element, so its min edge names the WRONG block (1.25M positions off on the proposal's own best case); a dropout pairs with a real reading the same way. J5's first lesson again: split runs at gaps before trusting their interval. Its three wins were engine defects, all fixed (positives) |
| Trusting the dispatcher's frame to stay put between rounds when nobody adds a stack array | **every call of 2-10 elements 4-5x the committed engine's** (a sorted pair 17.5 -> 84.7 ns), unseen for a round because this round's own A/B had the same frame on both sides | GCC began inlining `nearsorted_parallel_f64` (a 33 KB frame, called once) into `psort_f64` when another callee changed size: the frame went 35.9 -> 60.5 KB and stack-clash protection probes every page on every call. `noinline` on it and on `streams_try_f64`: 28.3 KB, 18.6-20.0 ns. Run callbench against the COMMITTED engine too, and print the frame every round |
| Sorting on a 32-bit prefix of the total-order key and resolving the collisions, killed on its own pre-written criterion ("under 90% of elements separated") | **the criterion was the wrong measurement**: 55.5% / 53.5% / 73.9% separated on uniform / heavy_tail / int_wide and 0% on few_distinct and quantized -- but the latter two's ties are 100% duplicates, which cost nothing to resolve, and the three random shapes' real tie groups hold at most 7 to 10 elements. clustered (10,274) and bimodal (512,897) are the ones that collapse, because the top 32 bits of a double resolve to about 1e-6 of the magnitude inside one binade | closed on the MECHANISM instead: a 32-bit prefix plus a 32-bit index is 8 bytes, which is what the double already weighs, so nothing is halved and a random-access gather is added. What survives is an MSD radix that stops after four bytes (a LEAF change), logged in experiments/IDEAS_LOG.md as S3b. **Count what the cost is made of, not what is easy to count** |
| A work queue for relocation's output tiles (phases 1 and 3 claim from a counter instead of taking the tile of their own index; 4 per thread, never under 32768 elements, the first overlapping run by binary search; `LOGOS_RELOC_STATIC_TILES=1`, `CAP_SWITCH=... ./capbench 60`) | **0.96-1.04 on every cell of two runs** at 1M-16M on block_perm and block_overlap, against a prediction of 0.88-0.97 from 4M up. Correct throughout (harness on both settings; 1,926 injected allocation failures and 48 thread-creation failures, 0 bad, reaching `reloc_worker` itself with the knot route off) and worth nothing. Reverted | the SAME change on the lattice map won 1.05x, because that phase is COMPUTE into a private per-thread table where a faster core finishes more elements. A memcpy over disjoint ranges is bound by one shared memory controller, so extra tiles cannot raise the aggregate. **A work queue buys balance only where the CORES are the bottleneck** -- which also says the knot worker's identical A1 and C tiles are not worth converting. And it moves the target: if equal tiles change nothing, the 3.6 threads the profile shows waiting are at the SECOND barrier, on one indivisible chain merge (relocation's serial middle) |
| Detecting the data FORMAT (dates/timestamps) and sorting on format-derived keys -- the step and origin known rather than inferred (`make dateprobe`, `./datetrace`) | **0.24-0.77 of the engine on six numeric date shapes**, i.e. a sort TOLD the origin and step is 1.3-4.2x SLOWER; and WRONG OUTPUT on day-plus-fraction (1/86400 has no exact integer form, so the round trip is not a permutation). Predicted 2-4x available on epoch_ms_day; measured 0.77 | the engine already certifies every exploitable part of being a date: the trace shows day_serial (3,650 distinct) on the LATTICE route and the ms/us shapes on the partition at 1.01-1.04x leaf imbalance. 8.4M values over 86.4M slots is dense numeric data and the label adds nothing. **The most a format can save is the DETECTION, which is the shared sample at 1-4% of a parallel call** -- under the floor, the same bound that parked S7. It pays only where it narrows a COMPARISON: date TEXT, below |
| Projecting each element onto a sphere of radius n before sorting (x = value, y = a prime, z = the index), hashed with primes against a table of precomputed values, re-projected on each new maximum while walking a linked list (`make sphereprobe`, `invprobe`) | **22.2% of 2.0M pairs at different indices come back ORDER-INVERTED**, so the projection is not a sort and the original values must be compared anyway | `proj_x = R*x/sqrt(x^2+p^2+z^2)`: the index enters the radius, and with z to 8.4M against values to 1e4 the z^2 term (7e13) dwarfs x^2 (1e8), so it orders by value/index. **Normalising to a sphere divides the value by a radius the index dominates.** COST would not have killed it -- the projection is 2.89 ns/element against the engine's 0.60 ns addressing multiply-add, and ~0.21 spread over 14 threads against a 0.78 ns/element sort -- and three of six timing predictions were wrong in the proposal's favour. Its other parts are already measured here: the linked list 0.50-0.57x, a running maximum is a dependency chain, prime-modulus hashing loses to the multiply-shift perfect hash the counting route already verifies, and (index, value) as a geometry is the zroute profile, which shipped two routes |
| The counting routes' serial merge in PARALLEL (slot ranges reduced independently, an O(P) prefix, ranges written out in parallel, claimed with an atomic so an orphan costs only speed; `LOGOS_LAT_PAR_MERGE=1`, `CAP_SWITCH=... ./capbench`) | **1.02-1.12x SLOWER** at 4M-16M, though correct under the harness, ASan, UBSan, TSan and 546 injected allocation failures | the diagnosis was inferred from a thread-occupancy average (6.0-6.9 of 14 busy) and the TRACE contradicts it: the merge is ~2 ms of a route whose map phase is 11.6-14.1 ms, so it is ~10% of the route and the extra spawn plus a second pass over the slots eat the prize. **Time the phase, do not infer its share from how many threads are busy.** Kept opt-in |
| A thread CEILING for the counting routes, so their serial merge stops growing with P (`LOGOS_COUNT_MAX=<k>`, `make capbench`, two runs of 20 pairs at 1M-16M) | **no global ceiling passes**: k = 3 gives few_distinct 1.50-1.54 at 2M but costs the lattice shapes 0.53-0.87; k = 6 and 8 cost them 0.72-0.90 at 8M and 16M. A ceiling on the HASH route alone then read 1.0 at 4M and 1.1 at 8M, inside the floor | the two families want opposite things: the lattice map phase is real parallel work that uses every thread, the hash route stops scaling near 8. Withholding threads does not convert idle threads into work -- only a PARALLEL merge does (0a). Ships OFF; the knob stays as the switch and the record |
| Gating the near-sorted route off at low thread counts, after a ladder found near-sorted data 2.3x SLOWER at 2 threads than at 1 (`NSG_ROUTE=1 ./nsgatebench`, `TS_SWITCH=LOGOS_NO_NEARSORTED ./tsbench ... kinks`) | **the route is worth 1.80-1.97x at two threads** on 0.5-1% random swaps and 1.56-1.61x on 2%; switching it off helps only KINKS (adjacent swaps: 1.18x at 2 threads, 1.04 at 4) and COSTS 1.10x there at 14 | two different shapes: random swaps displace elements, adjacent swaps are repaired into one strand by the detector. And the gate would not close the gap anyway (13 ms at 2 threads against 6.84 on one). What kinked data wants is the SEQUENTIAL path at every P, a route decision and not a gate on this route |
| "Treat uniform data as a complete computation": certify the distribution, sort P chunks with a parallel SIMD quicksort (x86-simd-sort's AVX2 kernel), merge them with a merge-path tree in which no level runs on one thread (`make unibench`) | **3.3-4.2x behind the engine on every shape at 8M and 14 threads, uniform included**, while the chunk sorts ALONE run at 0.76-0.86 of the engine's whole time; with std::sort per chunk 6.3-9.9x | the merge moves 3.86n on every shape and costs 87 ms where every chunk spans the range against 15-18 ms where chunks are disjoint: a merge is COMPARISON, a scatter is bandwidth (the old parallel-merge-sort entry, now with a number). On the same certificate the engine computes each element's bucket with one multiply-add on an equi-width grid and compares nothing. NOTE, found by the first cut's WRONG output: `logos_sort_f64` may not be called CONCURRENTLY (the context pool and bundle table are globals) |
| The Logos-Moebius hybrid as handed off (scan, twist, Reidemeister I on 2-3 element dips, a fallback under 16, a galloping stitch behind an O(1) boundary check; `./tsbench 16 1048576 moebius`, `moebanat`; implementation REMOVED 2026-09-23, in git history before that commit) | **behind the engine on 14 of 16 shapes as written and 13 of 16 in its strongest form** (block shapes 1.7-2.8x, jitter 5.9-7.0x, late batch 502-543x, uniform 4,759-7,342x, a ring buffer and a newest-first log 6,382-16,213x); the specified micro-kink branch fires **0 times** on 1% adjacent swaps, and the O(1) check skips 0.0-1.6% of boundaries | a dip's first element belongs to the PRECEDING ascending run, so the descending run is one element, never 2 or 3 (the engine's repair asks both neighbours and swaps the pair back inside the run); the bail-out as written sorts a run the scan just proved sorted; and merge ORDER decides: the same runs and kernel cost 100,999 merged elements per element into one growing prefix and 4.6 bottom-up. Its one win was an engine defect, fixed (positives) |
| The triple-reversal weld (an outside proposal): condense into runs, meta-sort them by min edge, permute them IN PLACE by triple reversals ((A^R B^R)^R = BA), weld the seams left to right behind an O(1) boundary check (`./tsbench 16 1048576 weld`, `weldanat`; implementation REMOVED 2026-09-23, in git history before that commit) | **2-4,600x behind the engine on 14 of 16 shapes at one thread** (latency shapes 11-32x, a late 5% batch 390-465x, uniform 4,079-4,639x) and 2.6-368x at 14; the O(1) check bypassed **0.0% of seams on every shape** | a maximal run swallows the next block wherever it starts higher (K = 32 on 64 staircase blocks), so units span holes and the prefix's maximum reaches the top of the range at once; the test then always fails and the merge behind it is O(prefix): quadratic. The edge round's lesson from the other side. The rotations are correct and cost 1.2-3x against a gather on block shapes. Its two wins were engine defects, both fixed (positives) |
| Clearing the AVX2/flash window's TOP: skipping the AVX2 leaf wherever flash is chosen, with no ceiling (first cut of the leaf-order change) | **0.914 at 131072** on the ladder and **1.123 in callbench**, independently, before it was caught | ranges that large still reach `sort_fn` through `leaf_f64`'s skew branch, and flash is chosen there for anything under `COARSE_MIN_N` (500000) -- but above ~65536 the AVX2 kernel is the better of the two. A window needs both ends measured; `AVX2_FLASH_MAX_N` is the ceiling |
| `FLASH_SORT_THRESHOLD = 512` (the obvious floor once the scalar-quicksort cliff below 4096 was found) | **0.795 at n = 512 and 0.939 at 1024** with the AVX2 leaf on, in BOTH orientations | a flash sort needs enough elements to amortise its classification pass; below ~2048 the AVX2 quicksort is better. The crossover is measured (2048), not rounded |
| Narrowing `make callbench`'s SIZES to look for an effect between 4096 and 65536 | **0.989-1.017 on every cell**, on a change that reads 1.24-1.35x on the shape that reaches it | its "random" shape is `(double)(nr() >> 11)`, integer-valued doubles spanning 2^53, which cost 25.7 ns per element at n = 4096 where uniform doubles cost 8.3: it never takes those paths. And its ladder steps 1000 -> 5000 -> 50000, over the whole window. **A bench that does not reach the code cannot clear it** -- check a row's absolute cost against the path you think it took before believing its flatness (`make bandbench` was written for this) |
| OpenMP spin-wait as the explanation for the scale report's small-n 14-thread dip (`__gnu_parallel::sort` runs first in the row and libgomp busy-waits after a region) | **0.87-1.06** measured directly, with `OMP_WAIT_POLICY=passive` as the control; not the effect. The dip itself was also misread -- the report reads 0.69-1.11, not the 0.32 that started the chase | at n <= 31,623 the engine runs ONE thread whatever is requested (`MIN_ELEMS_PER_THREAD` 100000), which a probe confirms (0.97-1.00 at every size, first differing at exactly 100,000: 1.16). Read the report's own numbers before explaining them |
| Building a "one sorted strand with kinks" probe out of `a[i] = i` (`make kinkladder`) | the kink repair reported **0 fixed with 209,142 kinks planted**, on the exact shape it was built for, and the committed engine did the same -- it reads as an engine defect and it is the PROBE | adjacent swaps are a permutation, so `a[i] = i` with kinks is still a PERMUTATION OF 0..n-1 and the dense-integer bitmap claims it before the run detector is ever reached (`once` mode on a `-DLOGOS_TRACE` build named it in one line: `dense-int: 2000000 values, window 5997627`). `a[i] = i + 0.5` declines the dense route and the repair fires (41,745 of 41,746). **A shape built from consecutive integers is a different shape from the one you meant**, and the ledger already had the same trap from the other side (a harness row built from a SORTED array never reached its gate) |
| A benchmark log that changes UNITS mid-column (`make scalebench`'s console: `u = small ? 1e6 : 1.0`, labelled ns below `SMALL_N` and ms from there up) | read as a 300,000x cliff in the SORT. uniform, one thread, the logos column: `n=1000 ns 7908.0`, `n=3162 ns 27330.0`, `n=10000 ms 0.1` -- "it skyrockets at 3162 then drops below n=1", which printed 0.6. Per element the same rows are **7.91, 8.64, 9.01, 8.91 ns**: no cliff, nothing to isolate | the switch sat at exactly the seam, so it looked like a property of that n. Now ONE unit for the whole table -- nanoseconds, two decimals (`fmt_ns`), `%14s` spanning the 0.01 ns floor to ~2e9 -- and the row ends with ns PER ELEMENT, the only column comparable down the page. An intermediate cut gave each value its own auto-scaled suffix (ns/us/ms/s); it was unambiguous but unscannable and unpasteable, so it went. Only the calibration line stays in ms, deliberately: two labelled reference values, not a column, compared against figures recorded in ms everywhere else. No other bench here has a conditional unit label. **Print the comparable quantity, and never let a column change unit** |
| Trusting a stub of the chart library with a ZOOMED window (the scale report's zoom and pan, first cut) | 216 charts driven clean under the stub; in real Chrome a log axis **could not be zoomed inside one decade** and a pan **killed the tab** (`Invalid array length`) | uPlot passes EVERY x window through the scale's range function, explicit ones included, and its log default rounds the far end up to the next decade (100..25k came back 100..100k); and its log ticks walk up from the window's lower end and loop forever when that end is below 1 with a mantissa of 9.5+ (9.6e-6 + 1e-6 rounds to 1.1e-5, a step it has no decimals for, so every later tick is 0). Its own zoom never meets the second BECAUSE of the first. A log axis here brings its own range and its own ticks; the stub rounds like uPlot and refuses a log axis without `splits()`; `tools/browser_check.py` DRIVES the charts in real Chrome. Second time a stub hid a log-axis defect: **a fake of a library must reproduce its awkward behaviour, and a chart nobody has dragged is a chart nobody has tested** |
| Closing a vector kernel's gap by copying the rival's visible tricks (the library's prefetch; its compiler flags; 15 or 63 pivot samples by insertion sort; scalar networks for small base cases) | **prefetch 1.00, flags 1.00, more samples 1.46-1.79x SLOWER at 512, scalar networks 2-9x slower at n <= 16** (`make avkernbench`) | what the library did better was invisible from reading and obvious from a size ladder: the gap was flat in n with half of it in the base case (a staging memcpy per call), and the extra levels needed a CHEAPER sampler, not more samples. Time the kernel alone across sizes before copying anything |
| Setting the hybrid's thread gate from the ladder of the shapes it is FOR (the library's parallel quicksort against the partition, five unstructured shapes) | the ladder said four threads (0.99-2.06); at four threads **eight concatenated logs read 0.66** and a noisy ring buffer 0.90 | a gate is set by what it must not break as much as by what it wins. The partition sorts interleaved sorted runs well because its buckets arrive nearly sorted; a quicksort cannot see that. Gate 2: every guard 0.97-1.08 |
| Reading a bench's completion off `make <target>` when the target's recipe RUNS the bench (`make scalebench` after `./scalebench` had been built by hand) | the binary was newer than its sources, make ran nothing, the tee'd log was EMPTY and the exit code 0; the "182 non-zero memory arrays" that followed were the previous report's | a recipe that runs is skipped with its build; check that the log has the run's own lines (the calibration line, the memory-pass time) before believing it ran, or `make -B` |
| Reading "the memory pass records zeros" off `head -3` of the report's DATA line | the first three arrays ARE zero: they are the sub-10k rows, which have no per-sort peak by design; 182 of 442 arrays carried values and the circles had been drawing all along | a one-line JSON blob needs a count, not a head. The user-visible symptom was the numbers being nowhere on the page, which is what was actually fixed |
| Setting a gate from ONE guard shape with EVERY gate lifted at once (the knot route's few-runs gate at 1.0n, from four interleaved runs at 0.955 under `LOGOS_KNOT_MAX=3`) | with the few-runs gate lifted ALONE (`LOGOS_KNOT_FEW=3`) the same shape reads **1.27**, and 3-8-run shapes had forgone 1.7-3.2x for four rounds | a switch that moves three gates measures none of them, and a verdict taken on an older engine is not a premise. Give each gate its own switch, re-measure the shape it was set from before trusting it |
| A compare-exchange inside one vector with a predicate per LANE (the vector merge's first cut) | **+0.0 came out twice and -0.0 was lost on every tie**, caught by `make mergebench` before any sort ran (planted signed zeros, a bit-pattern multiset check) | the two lanes deciding about ONE pair each computed `s < v` in their own lane, and on a tie both kept the same operand. One mask per pair, permuted to both lanes (also cheaper). The harness's gallop section then failed too: its byte comparison of two switch settings had assumed the scalar merge's tie order on both sides |
| The vector merge as ONE chain (four smallest stored, four largest carried, refill from the smaller head) | **1.14-1.29x** on the kernel against a prediction of 1.5-2.0, correct on every orientation | the network's ~20-cycle latency is serialised through the carried vector, so the kernel is latency-bound exactly like the scalar chain it replaces. A merge has two ends: a second chain from the tails (**1.96-2.28x**). When a vector kernel measures like scalar, count the chain, not the lanes |
| Predicting a merge-kernel win at 14 threads (1.05-1.15 on staircases) | **0.96-1.04 on every row** of knotbench and tsbench at 14 threads, while one thread read 1.11-1.68 | the knot route's parallel form is bound by memory traffic (scan + place + copy back), which 0f already recorded as a tie against the partition; a cheaper compare cannot move a phase that is not comparing |
| The kink gate alone, rerouting kinked strands from near-sorted to relocation at P > 1 (first cut; `TS_SWITCH=LOGOS_NO_KINKGATE ./tsbench 40 8388608 switch kinks`) | **0.91-0.94 at 14 threads** on 1% swaps at random positions, 1.19 at 2, while the same gate reads 2.49-3.30x on kinkladder's even-offset kinks | random positions overlap: ~2% of the swaps are 3-cycles the kink repair cannot undo, and the STRAY LIST that lifts them ran for `w[0]` at P == 1 only, so the detector came back with R = 796 and the knot route placed 796 strands. **A gate that reroutes must reroute to the WHOLE machinery the sequential path had.** Fixed: every worker lifts strays (3.04 at 14) |
| Locating where a P > 1 strand broke by ARITHMETIC on the chunk edges (three hypotheses in a row: the straddle, then the array's ends, then a pair beginning a chunk) | each fixed one case and the harness still read R > 1; the trace's run list named the next in one line every time (`[0 .. 1] descending`; a two-element run at 100001 = 1 mod 40) | print the runs when R is small (`-DLOGOS_TRACE`, R <= 8) before predicting the geometry; the summary "R = 2" says nothing |
| Turning the stray list on for every detector worker without touching the "run of one or two is strays" rule | four harness rows went R = 2 with two strays, twice (a kink whose second element ends a chunk; a swapped pair that begins one) | stray mode lifted EXACTLY the runs the join's boundary repairs were written to see. A run that touches a chunk boundary inside the array stays a run |
| Pricing the route a gate displaces from a `-DLOGOS_TRACE` build's wall time (near-sorted at 14 threads read 12.8 ms there) | predicted 1.5-1.7 at 14 threads, measured **3.04**: low by 1.8x | the ledger already says a trace build answers WHETHER, never how fast; it holds for the baseline of a prediction as much as for the variant |
| Trusting an environment-switch A/B to clear a change that also REWROTE the switched-off path (the profile-phase levers, first version: `sort_fn`'s scalar loop split around a prefix, `&mn, &mx` passed to the new vector helper) | int_wide, which keeps that loop, read **0.966 at one thread** against the engine as it was (`make frontledger`, layout cancelled), while every `LOGOS_NO_FRONTFUSE` A/B read 0.995-1.009 | both sides of a switch run the NEW code, so a switch measures the feature, never what the rewrite did to the path it falls back to. Price a change against the old SOURCE too (`make frontledger FL_BASE=...`); keep the fallback loop byte for byte and return a helper's results by value (1.010 after) |
| Interleaving OpenMP contenders under the default wait policy (`make scoreboard`, first run) | logos read **0.68-0.96 of x86-simd-sort on five 14-thread rows** and 1.4x slower than the hybrid, an engine identical to it at 14 threads | libgomp's threads spin after a region, and the rotation put logos right after `__gnu_parallel::sort` (IPS4o right after the library): the next contender ran on a busy machine. Under `OMP_WAIT_POLICY=PASSIVE` logos won those rows 1.18-1.30x. The scoreboard re-executes itself with it; any bench that interleaves an OpenMP contender needs the same |
| A grid lookup for a FEW modes (the mode grid, predicted 1.0-1.05 at 4 modes) | **4 clusters 0.933, 8 clusters 1.035** through the grid, 16 and 32 clusters 1.31 / 1.24 | a cell multiply, a convert, a byte load and a compare cost what three to seven compares cost; the grid pays only where the compares ran out (past 8 modes). `MD_LIN_MAX` = 8 |
| Trusting the dispatcher's frame to stay put, a THIRD time (the mode grid round) | `psort_f64` **28,320 -> 42,656 bytes**: sorted calls of 2 and 3 elements 1.11-1.13 in `make callbench`, both orientations | GCC began inlining `bt_init` (two 1024-double arrays, 16.6 KB) once the dispatcher's body changed size; then a 64-entry array of mine put it 128 bytes over the seventh page. `noinline` on `bt_init` and no array: 28,288, the regression gone. Print the frame after every round |
| The AVX2 quicksort as the DEFAULT for int32-valued doubles at one thread (`-DLOGOS_AVX2_INT32` turned into a runtime default, 2026-10-08; predicted 1.05-1.12 from the scoreboard's int_wide 0.916) | **0.83 at 8.4M, 0.93-0.94 from 65k to 1M** (`FP_SWITCH=... ./frontprobe_inhouse ... int_wide ab`) | wrong in sign: the narrowed LSD radix does 2-3 passes over 4-byte keys, the quicksort ~23 levels of 8-byte compares. What the library does better on int_wide is not what the in-house kernel does. Reverted to the opt-in flag, byte for byte |
| Lazy one-thread scratch (take the buffer and the context only when a route touches them) | **not built: nothing resident to save** -- uniform, heavy_tail, few distinct and sorted hold 0.04-0.06 bytes per element at one thread (`make rssprobe`) against the 15.5 memprobe reports | memprobe counts what is ASKED for; a large malloc is an mmap and its pages exist once touched, and the AVX2 leaf touches none of them. Measure resident memory before building a memory change |
| Pool trim as a DEFAULT (decaying high-water mark, pages past twice it handed back; MADV_DONTNEED, then MADV_FREE) | residue after one large and 32 small sorts **8-10 -> 0.35-0.83 B/elem**, but the next LARGE call after six small ones **1.3-2.5x slower** (hard form; block_perm 1T 2.48, uniform 14T 2.14), soft form 1.03-1.97 at 1T | predicted 1.03-1.20 from the ~13% page-fault floor; a refault of 60-110 MB costs far more than a steady-state fault rate. Ships OPT-IN (`LOGOS_TRIM=1` / `hard`): a memory policy with a speed price is the caller's choice |
| The in-place partition (IPS4o's block permutation over the same bucket map) as a DEFAULT | peak resident at 14T **8.6 -> 1.4-2.2 B/elem**, and **1.25-1.38x the time** on every shape that reaches the partition, 2M and 8.4M (predicted 1.05-1.25x) | one more scatter-like pass, and the scatter's floor is per-core latency (the ledger), so it pays that again. Ships OPT-IN (`LOGOS_PART_INPLACE=1`); the whole harness passes with it set |
| `-march=x86-64-v3` as the ceiling for per-function `target_clones` (`tools/ledger_rerun.py --only march`) | **0.95-1.00 on every row**, layout cancelled | the hot kernels are already AVX2 intrinsics and the scalar rest gains nothing from AVX2 codegen. target_clones not built (the rule, written first: only if a row clears 1.04) |
| Profile-guided optimisation (`make pgobench`, each orientation trains its own copy) | **0.98-1.03 on every row** | the engine is memory- and kernel-bound; branch layout is not its cost. Not adopted |
| A batch entry whose workers have ONLY the leaf (first cut of `logos_sort_f64_batch`) | five-valued segments of 10,000 at one thread **0.284** against a loop of logos_sort_f64; mixed lengths 1.46 at 14 threads (predicted 3-8) | the dispatcher counts few values in vector registers from 4,096 elements, the leaf's counting is scalar; and segments past 65,536 ran one by one. The workers ask the low-cardinality route first and take everything under 200,000 at P > 1 (the engine runs those on one thread anyway): 1.02 and 7.1 |
| A string sort with no prescan (the first cut of `logos_sort_strings_mt`) | sorted words **0.38 of the old multikey quicksort** and 0.65 of the same prefix scheme on x86-simd-sort | the quicksort's middle pivot is perfect on sorted input, and nothing checked order first. The numeric dispatcher's lesson, one engine over: a one-pass prescan with early exit made it 4.1x AHEAD (24x over std::sort) |
| Sorting each large group of equal string keys in turn, every thread on each (the first cut's tie policy) | paths scaled **1.23x from 1 to 14 threads**; paired against groups handed out concurrently, largest first: **2.58x slower** (`STRING_AB=1`, `LOGOS_STR_SEQBIG=1`) | twenty directories of 100k strings ran one after another on three threads apiece (STR_MIN_PER_THREAD). A group gets threads of its own only past a fair share (1/P of what is left) |
| Recursing into EVERY group of equal string keys (the first cut of the string sort's tie levels) | 3,000 strings, each sharing one 8-byte block more than the last, **SEGFAULT** (exit 139): 3,000 levels at 15.7 KB a frame | no bench or harness shape had a chain of nested groups; printing the frames (`-fstack-usage`) showed a 15.7 KB RECURSIVE frame, and the crash followed from the arithmetic. Loop on the largest group, recurse on the rest (each at most half): depth <= log2 n, frame 208 bytes. Print the frame of every new recursive function |
| Lowering the string sort's parallel thresholds (65536 / 32768 per thread, to 8192 / 4096 and 4096 / 2048), to close the 10k-64k gap against __gnu_parallel::sort that `make scale-full`'s string panels showed | **mixed**: urls at 32k 0.69, uuids / logs at 32k 0.81-0.82, but words at 10k **2.33x slower** and logs at 262k **2.86x slower** (`STRING_AB=1 STRING_AB_ENV=LOGOS_STR_PARMIN=...,LOGOS_STR_PERTHREAD=...`, 14 threads, paired) | a parallel level's fixed cost does not shrink with its range: the sample is nb x 32 keys (the WHOLE range at 8k), and every nested group past the threshold pays four hand-offs. Shipped values kept; the two env switches stay as the A/B |
| Handing groups of equal string keys to the workers below the parallel level (`str_groups_par`, the second attempt at that gap) | every group: urls **1.51-1.88**, logs **0.66-0.85**, words 0.80-0.91; only groups of 256+: urls 1.01-1.38, logs 0.90-0.92, words 0.81-0.95 (10k-60k, 14 threads, paired) | thousands of groups of two or three (lines in one millisecond) each paid a contended claim for nanoseconds of work; and the large groups of words are IDENTICAL strings that finish the moment they are looked at, so waking the workers is pure cost. Opt-in (`LOGOS_STR_GROUPPAR=1`); the lever is a work estimate per group or a cheaper hand-off, and logos still trails gnu_parallel on long-prefix strings at 10k-64k with threads |
| Radix in the cache for int32-valued doubles at one thread (one MSD pass on the top varying byte, then the lower bytes LSD inside L2-sized buckets; FINDINGS 3bm, `RB_SWITCH=...` in `roundbench`, reverted) | predicted 1.15-1.35x on int_wide; **1.034 at 8.4M and 1.023 at 30M over 60 pairs** (a first 30-pair run read 1.065), null 1.004, **0.975 at two threads**; with two 12-bit passes per bucket in place of three 8-bit ones **0.87-0.90** | out-of-cache traffic fell ~400 -> ~265 MB and the time did not: a scatter pass costs per ELEMENT in the cache or out of it (the scatter's own verdict), and 4,096 open write streams thrash L1 even with the bucket in L2. Fewer passes, not cheaper ones, and not wider digits |
| Fanning out the string sort's TOP level only (top threshold 16384, 4096 strings per thread; nested levels keep 65536 / 32768, the earlier attempt's mistake; FINDINGS 3bm) | predicted 1.2-2x at 65k-450k; **100k-200k 1.10-1.88x SLOWER on 8 of 9 shapes**, 16-32k mixed (uuids, urls, paths 1.2-2.2x faster; words 2.09x, ids 1.33x, logs 1.23x slower), 500k+ neutral. A CAP of six top-level threads won 1.05-1.35x at 300-450k and lost 1.18-1.30x on uuids / random from 1M | wrong in sign: a parallel level of a few hundred microseconds on 14 threads waits for its slowest, and its phases hand every thread an equal slice (`str_keys`, the classify pass). Six is this box's P-core count, so the cap is an artefact and is not shipped; the portable form is S1's work queue for those phases |
| Trading the partition for a split and a quicksort WITHOUT asking whether the sample is unstructured (the two-halves route, first cuts) | no gate: noisy ring buffer **0.71**, 64 shuffled blocks **0.44**, 30,000 quantized values **0.80**; inv_all and repeats gated: a sawtooth whose period is twice the sample's lag **0.655** | the partition's scatter keeps local order inside a bucket and its flash / coarse leaves read it; a vector split scrambles it and a quicksort cannot see it. And a strided statistic aliases with a period again (the mirrored streams' lesson): the gate reads 64 windows of ADJACENT pairs too (`sample_unstructured`) |
| Counting the shared sample's repeats against `m` (the unstructured test, first cut) | **every random array declined from six threads up**: distinct 2048 < m 5376 and the saturated Chao1 (2.1M) under n/2 | `distinct`, f1, f2 and Chao1 are taken over the first min(mfull, 2048) GATE points; `m` becomes 5376 when the partition extends the sample. Compare against the gate size. Found by a trace line, after a six-thread A/B read 1.00 where the ungated run had read 1.08-1.13 |
| A branch-free backward merge for the sequential tail route (predicted 1.8-2.4x on two sorted halves; `make tailladder`) | **0.34-0.66 from 10% tails down, 1.12 at a half** | a branch-free merge is a dependency chain (each load waits on the last compare); the branchy loop runs ahead wherever its branch predicts, which is every tail but a half. The single-chain vector merge's lesson, in scalar. What won instead: the TWO-chain vector merge at gaps <= 3 and a gallop from 256 (FINDINGS 3bn) |
| Setting the leaf's 2048-65536 kernel window (AVX2 or flash) by sorting ONE input repeatedly (`make bandbench`, 2026-09-23: flash 1.04-1.30x) | **reversed on batches of DISTINCT inputs** (`make bandbatch`): the AVX2 kernel wins every unordered shape across the window, uniform 1.13-1.25x, normal 1.35-1.57x, bimodal 1.67-1.94x, clustered 2.6-15x | flash's classification and fix-up are data-dependent branches, and a predictor that sees the same input 400 times learns them; the branch-free kernel gains nothing from repetition. **A kernel choice measured on one repeated input flatters the branchy kernel**: measure on distinct instances (the scale report's seam measures the same effect at 2.7x). Flash keeps what is ordered locally (blocks, streams); the window is now decided by a probe of local order |
| Counting every descent in the thin front's kink windows (first cut) | **the thin front 3x SLOWER at 129 random elements** (thinbench 0.73-0.98 where it had read 2.37) | half the pairs mispredicted a branch the sort never needed (~0.6 us of a ~0.4 us sort). The first descent that is not kink-shaped now ends the look: 2.32x at 129. A probe on the hot path must exit early on the common case |
| The quantized float radix in `leaf_f64` (a JS-era path, `radix_f64_full`, from TYPED_MIN_N to RADIX64_MAX_N) | **lost everywhere it fired**: edgeprobe saw near-sorted data 4.2x dearer per element the moment it starts (65536) and 2.8x cheaper the moment it stops (8M); off, quantized 1.40-1.41x, near-sorted 1.93x at 262k, no row it won | it predates the AVX2 leaf and was never re-measured against it. OFF by default, `LOGOS_QUANT_RADIX=1` restores it. Re-measure an inherited path against today's alternative before trusting its gate |
| A STRIDED sampler left behind after the Weyl move (`quant_probe_f64`, every (n/4096)-th element) | four interleaved streams sorted **2.6x slower than their own isomorph**: at 4M the stride 1024 is a multiple of the period 4, so the probe saw one stream | the chaos round moved every sampler to Weyl positions and missed this one. Found only by symprobe (the isomorph's stride landed on a different stream). Grep for `step = n /` before believing every sampler is Weyl |
| The run-length route's first cut (a branch per element in the compress pass, a loop exit per run in the write-back) | **geometric runs of mean 6 gained 1.02** while fixed runs of 8 gained 2.0 | fixed lengths make both branches periodic, real lengths mispredict once per run. Branch-free run starts and an 8-wide blocked write-back: 1.56. A ceiling measured on fixed lengths overstates real data |
| Taking the run-length route before the order routes (first placement, right after the counting routes) | a mirrored 95% prefix built of runs of seven equal keys **stopped reaching the mirror route** (four harness rows) | runs of equal values inside ORDERED data are better served by the route that uses the order; the run-length route is for runs in no order. It now sits last before the leaf. Second time in one round that a new route had to move behind an older one (the cuts route below) |
| Taking the cuts route before the wave route (first placement) | the harness's 8-cycle sine at 2M **stopped reaching the wave route** (witness 8 read 0) | a sloped sine has valid cuts at its troughs. Routes that can both fire need an order set by which is measured on the shape: the wave first |
| Hub and spoke at ONE thread: one equi-width scatter into L2-sized buckets, then the AVX2 kernel per bucket (a told ceiling, `./fieldprobe hub`, predicted 1.10-1.35 at 8M) | **0.58-0.91 at 1M-16M**, uniform and normal, buckets of 4096 to 65536, plain and write-combined | the scatter costs 2.5-4 ns per element at one thread and the top quicksort levels it replaces cost less. The ledger's "a scatter is per-core latency" holds at ONE thread too, which no row had measured |
| Re-testing the int32 row on DISTINCT inputs (the AVX2 kernel for int32-valued doubles, `make twinbatch TW_NEW=-DLOGOS_AVX2_INT32`, both orientations) | the kernel **1.02-1.24 SLOWER** at every size from 4096 to 4M | the row (0.83-0.94, one repeated input) stands. fieldprobe's engine-against-bare-kernel gap on int_wide (1.72 at 4096) is the dispatcher and the profile loop, not the radix: compare the two leaves, not the leaf with the engine |
| A coverage test that reads mn / mx where the profile is DEFERRED (the low-discrepancy band, first cut) | triangle waves **2x slower** (0.48-0.56): the kernel never ran (AV_STATS: 0.00 partition levels against 17.60) | the founder windows settle a triangle, the profile pass is deferred to the kernel's first partition, and until then mn / mx are the WINDOWS' extremes; the test called the triangle low-discrepancy against them. Anything that reads the range ahead of the kernel must run the deferred profile first (it does now, on the rare path) |
| The coverage band's first cut at 0.05 alone | long legs went to the kernel and short legs to coarse: the WORST of both (tri4096 0.77, saw64 0.82) | the condition drops the probe below a threshold, so lowering the threshold moved WHICH legs it drops, not how many. Measured both halves (kernel for short legs, coarse for long clean ones) and shipped the band |
| The stray cap at 1/16 for every thread count | **0.33 on 2% random swaps at 14 threads**, 0.84 on 3%, while one thread won 2.09 and 3.07 | at P > 1 the near-sorted route takes random swaps and the detector's lists cost more than they save. The cap is 1/16 at one thread and 1/64 above |
| Skewing the scratch buffer against 4K aliasing (the hardware question, 2026-10-21: the caller's array and the pooled scratch are both mmap-backed mallocs behind a 16-byte header, so they share an offset in their 4 KB pages; a switch handed the scratch out 64 or 2112 bytes past its base) | **0.93-1.03 on 45 fieldprobe shapes and 23 roundbench guards: nil**. Taken out again | a constant skew helps only streams at a FIXED distance (a plain copy); the merges' read and write fronts drift, so their aliasing, if any, does not depend on the base offset. Predicted nil once the passes were read, before the run |
| Code alignment (`-falign-functions=64 -falign-loops=32 -falign-jumps=32`, `make twinbatch`, both orientations) | **0.95-1.07, centred on 1.00** at every size from 8 to 1M, six shapes, distinct inputs: nil | the small end's 0.78-1.13 layout swings (callbench) were thought to be an alignment lottery an explicit policy could win; on batches of distinct inputs there is no lottery to win. Not adopted |
| Strided order as a route (OUTSIDE_THE_BOX C1, predicted 3-10x) and patience piles for randomly interleaved sorted sources | **not built: told ceilings 1.27-1.90** for 2-4 TDM channels and the zipper, 1.01 at 8, **0.50 at 16**; patience **1.14-1.46** at 2-4 sources | measured after the vector k-way merge: merging k strands costs log2(k) vector passes, against a quicksort that resolves a bit for ~0.5 ns. The demux's gain is bounded by that, not by finding the stride |
| A branch-free compaction that writes every element and advances past the ones it keeps, with the writers' ranges packed end to end (the dominant-value route's first two cuts) | **not a permutation, and a heap abort**, while the timing bench read the route as fast | the last write of each worker lands one slot past its range: on the next worker's first slot (a race) or past the buffer. The timing bench checked ascending order only. One slack slot per writer; and check the BIT MULTISET in a new route's first bench, not only the order |
| Gathering a route's elements into a buffer and sorting them there, then writing back (the dominant-value route's first cut) | memory the SUM of the buffer and the inner sort's scratch: ~17 bytes per element of the rest, above the partition's 9 below a share of one half | copy them to their final place first and free the buffer before the inner sorts take theirs: the peak is then the larger of the two |
| Freeing a large malloc'd buffer in the middle of a sort | **6.25 bytes per element still resident after `logos_release_buffers()`** (against 0.30) | glibc raises its mmap threshold to the size of any mmap-backed block it frees, so every later block up to that size -- the engine's and the CALLER's -- comes from the heap, which keeps freed pages. A buffer that lives inside one call is an mmap of its own; release calls `malloc_trim(0)` |
| Reading a probe's per-element table without checking its unit (the int32 rows of `rssprobe`, first cut) | int32 scratch read at **twice** its size (16 bytes per element) | the probe already reports per element; I had scaled the 4-byte rows by two. Check one cell by hand (bytes resident / n) before deciding on a table |
| Half a scratch with memcpy, memmove and a slide of every upper piece (the half placement's first cut) | **0.86-0.91 against the full scratch** on block shapes at 8.4M, one thread (`halfpair`, roundbench, tsbench) | the full scratch's two passes STREAM (`place_copy`) and a plain store fetches its destination line first: ~48 bytes moved per element against 32. Streaming every copy that does not overlap itself: parity; then copying only the pieces that reach the midpoint into the gaps (`half_fit`): 1.06-1.15 FASTER. Fewer bytes held is not fewer bytes moved; count both |
| Moving the half placement's midpoint to the end of a straddling k-way crossing (first cut) | **strands4 kept 8.09 bytes per element** | one crossing of every strand IS the array. Split the crossing by value (`half_pathk`, a multi-sequence selection): 4.14 |
| Measuring memory only one fresh process at a time (`rssprobe`) | **about 20 bytes per element held between calls** in a process that sorts with 14 threads and then on a one-thread placing route, invisible to every row | the partition's regions and the pool were separate persistent blocks, and every NESTED sort took a fresh context bundle and kept it (the wave route: 10.4 in rssprobe, 3.4 of it live heap). `make mixprobe` holds a mixed sequence in one process; 14.0 after the round |
| Counting arbitrary doubles by HASHING to get under the comparison bound (a told ceiling, scratch `hcount.c`, FINDINGS 3br: one pass into {key, count} slots, the distinct keys sorted, the values written out) | 256 and 1,024 values at 16M **1.70 / 1.20**, the 30,000-value lattice 2.54; **4,096 values 0.73, 65,536 0.81, 262,144 0.71, 1M 0.35**, and at 1M elements a loss nearly everywhere | a lookup into a table past L2 is a miss per element, and the vector quicksort is already ON the multiset bound (0.37-0.57 ns per bit of H_m for every k, flat in n). A lattice wins because its index needs no probe. Not built; the window k <= 2,048 at n >= ~8M is IDEAS_LOG 15 (a) |
| The lattice entry's fill at 8 elements a slot at one thread, and at 16 at four threads (the comparison-bound round) | **0.79** at one thread (131,072 values at 1M); **0.69-0.75** at four threads, predicted 1.2-1.8x | a lattice wider than the data is walked for nothing, and the partition gains more from threads than the route's serial slot merge does. The fill is 16 (P + 1) / 2 per slot: 16, 24, 40 |
| The coarsest step that holds EVERY sample point, among the smallest gap's divisors, with no fill test (the divided step's first cut) | one off-lattice sample value (0.25 k + 0.1 among multiples of 0.25) gave step 0.05, **a lattice five times finer than the data**, mapped at 5.3 ns per element (8 threads, 1M) against 2.4 for uniform data | the counting block had no fill test because its repeats gate had stood in for one; a divided step must leave the fill too. Found by a harness row that asserted the spill count: 0 where it expected spills |
| Predicting spills to pay on off-lattice lattices where the gate's sample is larger (8 and 14 threads) | **0.97-1.05**, predicted 1.5-3x, while one thread read 2.05-3.66 | the 2,048-point sample holds the off-lattice value (the 384-point one does not), the step that keeps it is a 150,000-slot lattice, and the fill or LATTICE_MT_MAX_SLOTS declines it before any map: spills help only a lattice the step search accepted. Trace which gate refused before crediting a route (`boundprobe_trace once [n] [filter] [P]`) |
| Placing X+Y by its structure, told X and Y (`make xyprobe`, FINDINGS 3bs: sort X and Y, a staircase count per quantile splitter, each row written as segments of x_i + Y already sorted inside, buckets of S sorted by the engine), predicted 0.70-0.90 of the engine at 16M | **S = 262144 0.86 and flat (+1.2% per element from 1M to 16M); S = 32768 1.10, S = 4096 1.98**, and the engine itself blind (raw = shuffled 1.00-1.04) | a comparison bit costs the engine only ~0.2-0.4 ns at the margin (buckets of 4k / 262k / 16M at 8.2 / 9.0 / 11.2 ns per element), and a placement pass costs ~1 ns plus a mispredict per segment plus n B staircase steps; only segments of >= ~64 (S / n) make the pass cheaper than the bits it removes. The flat fingerprint is O(N log S + N^1.5 / S), not O(N): **a flatness test over one decade of N cannot tell linear from a superlinear term with a small constant**. Not built |
| Calling an opponent's parallel sort without a thread count (IPS4o in `scalebench`, `ips4o::parallel::sort(begin, end)`) | **IPS4o's P-thread line in every scale report before 2026-11-05 ran on 20 threads** while every other P-thread contender ran on 14, and the memory pass measured IPS4o's SEQUENTIAL sort for the P-thread series | the overload without a count uses `std::thread::hardware_concurrency()`. `scoreboard` always passed P; `scalebench` now does everywhere (timing, seam, memory, numbers and strings). **A fair fight is P against P only if every call is GIVEN P: read each opponent's default** (FINDINGS 3bt) |
| Handing a decomposition's leftover pieces to the engine at P threads, one call per piece (the prime-factor solver's first cut, `PRIMEPROBE_CPLXP=1`) | an in-order staircase with nothing pinned read **0.14 at 14 threads** (0.87 at one) | not the solver: the ENGINE called at 14 threads on 262k-524k elements of two or three overlapping runs takes 12.2 ns per element against 2.2 at one thread (the near-sorted route's claim, FINDINGS 3bv). **A standalone probe that calls the engine inherits the engine's defects at that size and thread count: time the engine alone on the pieces before blaming either** (`PRIMEPROBE_DIAG`) |
| Predicting a gate's win from an exploration table taken on a LOADED box (the strand gate, 2026-11-06: predicted 4-5.5x on the 0.2 staircase from a table where uniform read 3.71 ns per element) | measured **3.46x and 3.87x** in two runs on the quiet box (uniform 1.67 ns per element): four of six 8.4M predictions UNDER; and at 2 and 4 threads predicted 1.3-3, measured **2.1-5.3**, the wrong direction | the near-sorted route has three spawn phases and a nested parallel sort, and lost more to the load than the knot route's two phases did: "a loaded box flatters logos" holds between two of logos's own routes. And at P = 2 the nested stray sort is the partition at two threads, the engine's slowest form (3bm): the gain GROWS as threads fall. Read a paired A/B's ratios on the box it was taken on; take predictions from a quiet run of the same pairs |
| A lower GLOBAL thread floor now that the worker pool made spawning free (`LOGOS_MIN_PER_THREAD=25000` / `50000` against 100000; `roundbench`, 14 threads, 200 pairs, FINDINGS 3bz), predicted 1.3-2x at 100k-316k | unstructured shapes 1.1-1.6x faster at 316k-1M, but **kinks and sparse swaps 3.5-5x SLOWER at 100k, block_perm64 3.5-3.8x, streams 2x, normal 1.5-2.4x at 100k-200k** | the floor was never only the spawn: it is each ROUTE's parallel form at two to four threads (the one-thread path repairs a strand, the two-halves route beats a 24-bucket partition). A thread floor belongs to a route. What shipped is the unstructured partition's own (`BOOST_PER`) |
| Raising the partition's thread count AND its bucket count together (the boost's first cuts) | 64 thin clusters **1.5-2.8x slower** at 500k-700k, a gaussian 1.16 at 316k, while four shapes won 1.1-1.5x | a map that balances at the policy's bucket count need not at twice it: the clusters' certified grid spilled 61,990 of 500k and the histogram path ran after a wasted scatter; the gaussian's key grid was rejected at 72 buckets and kept at 36. Going back cost 10-20% (a map build is ~8% of a sort that size). **Raise the threads and leave the buckets: nothing is rebuilt and no map can change** (all six shapes 1.08-1.99x) |
| Counting strings that repeat (intern by hash, sort the distinct, place pointers by group; a told ceiling, `make strceil`, FINDINGS 3bz), predicted 2-4x on words | **1.19 at 2M, 1.18 at 20M, 0.76 at 200k**; not built | the prediction priced std::sort's fallback (two dereferences per compare). The engine reads each string once for an 8-byte key and most words fit one key, so both sides pay one miss per string, and the told sort adds a pointer scatter into 35,000 groups. Price the fallback the ENGINE has, not the textbook's |
| Packing a small alphabet (16 characters per 64-bit key at <= 15 symbols; told ceiling: the strings re-coded two characters to a byte, `make strceil`), predicted 1.2-1.5x on uuids | dates **1.24**, ids **1.30** with the coding free, **0.88 / 0.84** with one re-coding pass; uuids untouched; not built | 32 bits of hex separate 50M uuids in the first key: depth was never their cost. Where depth is real (dates, ids) the coding eats the gain. Count the levels a column actually needs before shortening them |
| Dropping the 1-thread hash gate's quantized-radix clause because that radix is now off (`LOGOS_MCGATE_V1=1`, `make gateprobe`, FINDINGS 3ca) | zip_codes **2.44 at 100k**, but whole-number columns of 3,000 true values **0.75-0.77 at 316k-1M** | the clause was not the defect, the ESTIMATE was: Chao1 read 1,519 on 384 sample points and the map met 18,923 values. And the sample cannot tell the two cases apart (269 distinct / 1,519 against 344 / 1,777). **When a gate's estimate can be wrong by 12x, bound the route at run time**: the map gives up at 8,192 values |
| Splitting a fat bucket on claim, or sizing regions by sample counts, for a value holding 5-6% at 14 threads (two forms proposed) | **neither built**: the trace put zip_codes' loss in the histogram path's EXTRA PASS (14 ms of counting before a 14 ms scatter), its leaves cost what uniform's do | both forms attack the bucket; the decline was the cost. Counting the heavy values in the scatter removes the decline AND ~45% of the moves. **Trace which phase the time is in before choosing between two fixes for the phase you assumed** |
| (key, index) pairs through a vector key-value quicksort as the ONE-thread argsort (`make argceil`), predicted 2x | **1.22 on uniform, 0.63 on whole seconds** | 16-byte pairs through ~23 partition levels against an LSD radix that skips constant bytes. What the same table showed was elsewhere: sorted keys 54 ns per element, threads 4.4x |
| Partitioning keys on their top VARYING BITS (the threaded argsort's first cut) | log-uniform **16.2 ns** and near-sorted **30.4** at 14 threads where uniform read 5.7 | a column that straddles 1.0 has every exponent bit varying (1022 = 0111..., 1024 = 1000...), so 12 bits were 11 of exponent: ~30 buckets, the top a quarter of the array. Cells of equal width in KEY space from the smallest key: 5.9 and 4.3. A ladder of the width showed it (more bits kept helping) |
| Calling across object files from the engine (`logos_argsort_f64_mt` in logos_psort.c calling `logos_argsort_f64` in logos_extra.c) | **every multi-copy bench stopped linking** | engine copies are built from logos_psort.c alone. The RENAME rule's twin: the engine file links on its own. `make ledgerbench` after any new entry |
| Building every compile-time flag only in rounds that touched it | `-DLOGOS_INT_NARROW` **did not compile from 3bq** and `-DLOGOS_NO_MODE_TABLE` **from 3bj** | a type or helper moved behind another flag's `#if`; the per-round check built only the flags the round named. Build all of them (78 today) every round: grep the `#if` lines for `LOGOS_` and compile each |

## POSITIVE results worth building on

- **Three changes from the agents' round** (FINDINGS 3ca; `make gateprobe`, `make argceil`). (1) **The sequential hash map is capped**
  (`MC_HASH_SEQ_CAP` 8192, `LOGOS_MC_SEQCAP=<k>`, `logos_twist_last(40)`): one thread, uncapped / capped: zip_codes **2.03 at 100k,
  1.61 at 200k, 1.35 at 316k, 1.18 at 1M**; 2,000-3,000-value columns (where the map wins) 0.99-1.02; guards 0.96-1.03. (2) **Heavy
  values are counted, not moved** (`hv_build`, `hv_insert`, `LOGOS_NO_HFHEAVY=1`, `logos_twist_last(41)`): sample runs of at least a
  quarter bucket (up to 64) are counted per thread in the histogram-free scatter and filled in around each bucket's sorted rest;
  14 threads: **zip_codes 1.66 (3.26 -> 1.95 ns per element, uniform's own figure), fk_zipf 1.71 at 10M**, 1.18 at 1M, 34 guard rows
  0.95-1.04; the lazy view takes the same path. (3) **`logos_argsort_f64_mt`** (`LOGOS_ARGSORT_SEQ=1`, `LOGOS_AS_BITS`,
  `logos_twist_last(42)`): a key-order prescan (the identity), then one stable parallel partition into 4,096 equal-width key cells
  and the LSD radix per bucket; the SAME permutation as `logos_argsort_f64`. 14 threads, 500k-30M: **uniform 5.8-7.7x, log-uniform
  5.6-7.2, whole seconds 3.1-4.3, fk_zipf 3.9-6.1, five values 2.8-4.9, near-sorted 6.3-9.8, sorted 31-58x**; scratch 20 -> 12 bytes
  per element. Open: the argsort is still 2.5-3.4x the value sort at 14 threads (the engine's partition on pairs is the lever);
  a skewed key range lands in one cell; 2-7 threads and memory for the heavy values are unmeasured; IDEAS_LOG 22 (d)-(r) queued.
- **The unstructured partition's own thread floor** (FINDINGS 3bz; `boost_threads`, `BOOST_PER` 25000, `BOOST_MIN_N` 300000;
  `LOGOS_BOOST_PER=<k>`, under 1000 = off; `logos_twist_last(39)`). After every structured route has declined and
  `sample_unstructured` says yes, the partition takes one thread per 25,000 elements instead of the policy's one per 100,000, with
  the POLICY's bucket count, sample and maps. t(off) / t(on), 14 threads asked, 200 pairs: uniform, heavy_tail, bimodal, clustered,
  int_wide, normal **1.62-1.99 at 316k, 1.28-1.42 at 500k, 1.08-1.13 at 1M**; seventeen structured guards 0.98-1.03; six threads
  1.56-1.64 / 1.08-1.12. Open: 100k-299k (one or two threads by policy; the boost there won on certified-uniform data and lost on
  heavy tails). A/B any thread-policy idea with `RB_SWITCH=LOGOS_BOOST_PER RB_VALUE=<k> ./roundbench 200 14 316228,500000,1000000`.
- **The near-sorted route's strand gate: strands are not strays** (FINDINGS 3bw; `make strandgate`; `LOGOS_NO_NSSTRAND=1`;
  `logos_twist_last(38)`). At P > 1 the route claimed sorted runs lying IN ORDER and overlapping their neighbours (its inversion
  statistic reads 0.000 on them and its four pre-gate windows lie inside runs), sorted a tenth of the array as strays and ran
  1.6-3.9x slower than the engine with the route off. What tells strands from strays is SCALE: displaced elements show as adjacent
  descents inside the windows (~1.5 per displaced element), strands show only in a sample sorted by position (one descent per
  boundary). Windows <= 4 descents and sample >= 4: decline. t(off) / t(on), 14 threads, 8.4M, two runs: staircases 1.7 / 3.5-3.9 /
  3.1-3.3 / 3.5 (overlap 0.05; 0.2 with 64, 16, 256 runs), swapped neighbours 3.0-3.1, late batches 3.4-3.7; 1M 1.6-3.3, more at 4
  and 2 threads (2.1-5.3); 21 guards 0.95-1.07, roundbench 0.98-1.05, tsbench 0.97-1.05; one cell at or under 1 (16 runs of 65k at
  1M, 0.86-0.98, p90 past 1). The table of 27 shapes was printed BEFORE the rule was written; the stray share and strays per descent could not
  separate the two kinds, the windows could. The harness row that names its path caught an uninitialised counter the A/B would
  have read as 1.00 everywhere.
- **A knot solver that unties only the prime factors, standalone and NOT in the engine yet** (FINDINGS 3bv; `make primeprobe`).
  An element of a run is already in its final place when everything before it is no larger and everything after no smaller, and
  inside a run that is two binary searches against the neighbouring runs' extremes: those PINNED ARCS are never moved, and the
  factors between them are untied in place and independently (two-piece merges through a buffer of the factor's size, a
  descending strand turned where it lies, the rest to the engine). 8.4M, one thread: batches in order overlapping 5% / 20% / 50%
  **2.63 / 1.66 / 1.20**, blocks in order with every other twisted **2.75**, 8 neighbour pairs swapped **2.60**, 4 batches late
  1.23; 14 threads over the knot route's parallel form **3.1 / 2.3 / 1.35 / 2.7 / 2.1**, late batches 0.87; shuffled strands and noise: nothing to
  gain (one factor; the standalone teller's second scan reads 0.77-0.87). Scratch asked for 0.15-3.1% of n. At 1M and 14 threads
  the OpenMP form does not hold. **And an engine defect found by it, FIXED 2026-11-06 by the strand gate (the bullet above, FINDINGS
  3bw): at P > 1 the near-sorted route claimed overlapping strands and the engine was 1.3-1.9x SLOWER at 14 threads than at one on an
  in-order staircase (overlap 0.2), swapped neighbours and late batches (up to 3x at 1M).** The solver's other build, pinned arcs in
  the knot route's plan, is still queued: IDEAS_LOG 17 (b).
- **The comparison bound, shape by shape, and a lattice entry under it** (FINDINGS 3br; `make boundprobe`). Per shape the bits a
  comparison sort needs on the input (repeats, runs, out-of-place: H = the least), ns per bit and the growth from 1M to 16M in pairs.
  The vector quicksort sits ON the multiset bound for random doubles with repeats (0.37-0.57 ns per bit, flat in n) and the ordered
  shapes are far under n log n; the one shape priced like a comparison sort where a direct index places everything was a lattice of
  10k-250k values, because the counting block needs REPEATS in a 384-point-per-thread sample. Now: an entry that asks only that every
  sample point lie on one lattice and that the span leave 16 (P + 1) / 2 elements per slot (`lat1_gate`, `LOGOS_NO_LAT1=1`,
  `logos_twist_last(36)`); the step is the coarsest divisor of the sample's smallest gap holding every point (`LOGOS_LAT_DIV1=1`);
  values off the lattice are SPILLED up to n/32 and merged in after the emit (`LOGOS_LAT_NO_SPILL=1`, `logos_twist_last(37)`).
  One thread **1.18-4.01** wherever it fires (30,000 values 2.27-3.61, 250,000 values 1.18-1.69 at 4M-16M, prices in cents 2.9,
  the last 0.05% off the lattice 2.93-3.66), two threads 1.60-4.19, four 1.23-2.93; guards 0.945-1.039; 16 far sentinels at 8 and 14
  threads 1.55-1.90 (the counting block's lattice, spilled). 30,000 values at 16M: 7.99 -> 2.19 ns per element, and the cost per
  element now FALLS 30% from 1M to 16M. One thread's peak on it 4.14 -> 0.39 bytes per element.
- **The second memory round: half a scratch, keys in place, one block** (FINDINGS 3bq; `make rssprobe`, `make mixprobe`).
  One-thread placements (the knot route, the tail / head vector merge, the wave route) write the lower half of the output into
  half a scratch, copy only the upper half's pieces that reach the midpoint into the gaps the lower half left (else slide
  them all), place the upper half in the array and stream every copy (`half_place`, `LOGOS_PLACE_FULL=1`): 8.1-10.4 -> 4.1-4.4
  bytes per element resident at one thread, and FASTER on block shapes (64 blocks 1.09 at 8.4M, 1.15 at 1M; roundbench
  block_perm64 1.12; every other row 0.97-1.07). The float32 radix keeps its keys in the input (8.1 -> 4.1, `LOGOS_F32_TWOBUF=1`),
  the partition's regions borrow the pool's block (`LOGOS_HF_OWN=1`), nested sorts rewind the context-bundle cursor: ten mixed
  sorts in one process held 23.8 -> 14.0 between calls. `LOGOS_LOWMEM=1`: the in-place partition and the hard trim together.
- **The memory round: one scratch array, a dominant-value route, release returns the heap** (FINDINGS 3bp; `make rssprobe`,
  now 21 shapes). The LSD radixes biased or narrowed their keys into the context's aux array AND ping-ponged through buf: now in place
  in the input, one buffer (`LOGOS_RADIX_TWOBUF=1`): int32 at one thread peak 8.1 -> 4.1 bytes of scratch per element, kept 4.1 ->
  0.15, speed neutral. One value holding 20% or more at P > 1 (85% at one) is filled in around the rest (`dominant_try_f64`,
  `LOGOS_NO_DOMINANT=1`): 14 threads 2.7-9.9x at shares 0.25-0.9 (the partition had given that value ONE bucket and one thread) and
  peak 10.4-11.6 -> 0.9-6.2; one thread 2.1x at 0.9. `logos_release_buffers()` now calls `malloc_trim(0)`.
- **Ideas from other fields: three tests, seven changes** (FINDINGS 3bo, experiments/OTHER_FIELDS.md; `make fieldprobe`, `make twinbatch`).
  Tests: MOIRE (periodic inputs over 46 periods against their own values shuffled: a triangle wave 7.8x slower than shuffled), the
  FOUNDER effect (an unrepresentative prefix against the same elements scattered: 11-19%, and 5x on a near-sorted array with strays in
  its first places), LANDAUER (ns per element against bits per element: sorted data with 1% random swaps cost more than random data).
  Shipped: founder windows when the 64-element prefix settles nothing (founders 1.13-1.16); the k-way merge in vectors for the knot
  route's k >= 3 crossings and the leaf's run merge (2.08-2.60, 1.32-1.36 at 14 threads, eight concatenated logs 2.13); the stray cap
  1/16 at one thread (1% swaps 3.07, 2% 2.09; 1/64 at P > 1); the short-reach threshold 0.20 (two random sources 2.0-2.6x, no longer
  by seed); the cuts route's dense decline; the low-discrepancy probe's COVERAGE band (short legs 1.09-1.22, clustered 1.97-2.23; long
  clean legs keep coarse); the detector's checkpoint minimum (5x on that near-sorted row). The hardware question asked mid-round:
  4K aliasing between the array and its scratch and code alignment both measured NIL (ledger); open leads in IDEAS_LOG 12.
- **Three invariant tests, and eight fixes they found** (FINDINGS 3bn, CHANGELOG; `make orderprobe`, `make symprobe`, `make edgeprobe`).
  The TESTS are the durable part: `orderprobe` times 24 ordered shapes against their own values shuffled (order must never cost),
  `symprobe` times order-isomorphs (negate-and-reverse, exact scalings: they must cost the same), `edgeprobe` crosses every size gate
  with one element (one element must not cost 10% more). Shipped from them: (1) **the run-length route** (`LOGOS_NO_RLE=1`): runs of
  equal values, 2.0x at runs of 8, 3.5x at 16, 7.0x at 64, 1.56x on geometric runs of mean 6; (2) **the cuts route**
  (`LOGOS_NO_CUTS=1`): windows in order wider than the short-reach route's, coarse64k 1.53x; (3) **the tail merge's forms**:
  the two-chain vector merge at a gap <= 3 (1.41x at a half), a gallop from 256 (1.10-1.25); (4) **the head route** (`LOGOS_NO_HEAD=1`):
  a random head before a sorted suffix, 1.39-7.61x (symprobe: 5.02x slower than its isomorph); (5) the quantization probe on Weyl
  positions (streams4 2.17x); (6) **the quantized float radix OFF** (quantized 1.40x, near-sorted 1.93x at 262k); (7) **the thin
  front to 2047** with the vector prescan and kink windows: 1.1-3.7x from 129 to 2047; (8) **the 2048-65536 window by local
  order**: the AVX2 kernel for unordered ranges, uniform 1.13-1.25x, normal 1.35-1.57x, bimodal 1.67-1.94x, clustered 2.6-15x
  (bandbench's flash verdict was an artifact of one repeated input). Diagnosed: the vector quicksort's order cost is in the library
  too, at the same depth.
- **Eight invented ideas, A/B'd: six ship** (FINDINGS 3bm, CHANGELOG; `make roundbench`, `make strsmallbench`). (1) **Two halves at
  two threads** (`halves_try_f64`, `LOGOS_NO_HALVES=1`, `logos_twist_last(26)`): the partition at P = 2 ran 1.01x ONE thread on
  heavy_tail and 0.76x on clustered (768 sample points, under the key grid's 1024; 350k leaves on the parallel leaf); one split at the
  sample's median and two sequential leaves: 8.4M 1.14-2.25x, 250k 1.22-3.04x, twelve guards 0.99-1.02. (2) **The sequential leaf in
  the partition where buckets are past L2** (`LOGOS_NO_SEQLEAF=1`; the ledger's parallel-AVX2-leaf row is superseded): 3 threads
  1.32-2.09x, 4 threads 1.12-1.76, 6 threads 1.05-1.12, 14 threads at 30M 1.05-1.24. Both gated on `sample_unstructured`. (3) **Small
  string groups on keys** (`str_small_keys`): 8-32 strings 1.36-2.77x. (4) **The log-line route** (`str_ns_try`, one thread,
  `LOGOS_STR_NO_NS=1`): one keep pass with demotion, strays sorted alone, merged back in place: 2M log lines 2.11-2.32x (4.7x
  std::sort). (5) **Placements stream** (`place_copy`, `LOGOS_PLACE_PLAIN=1`): block shapes 1.05-1.09 at one thread. (6)
  **`REACH_KINK_MIN` 3**: sparse kinks 1.9-3.4x at 4 and 14 threads. The two largest wins were the SEQUENTIAL leaf put where the
  parallel engine ran a slower one, found by a thread ladder (1, 2, 3, 4) nobody had run.
- **Strings on 8-byte prefix keys** (`logos_sort_strings_mt`, FINDINGS 3bl, `make stringbench`; `logos_sort_strings` calls it with one
  thread, `LOGOS_STR_MKQS=1` keeps the old quicksort): each string's next 8 bytes, big-endian and zero past the NUL, make one 64-bit key in
  strcmp order; (key, pointer) pairs are sorted by an MSD radix with constant-byte skip (one thread) or a sample sort (P threads), equal
  keys go 8 bytes deeper, a range of all-equal keys skips 8 bytes. 2M strings, one thread, against std::sort with strcmp: **uuids 12.4x,
  random bytes 14.5x, ids 8.3x, words 7.1x, urls / paths 4.2-5.0x, near-ordered logs 2.4x, sorted 24x**; the old quicksort 2.4-7.4x
  behind it. Against the same prefix scheme on x86-simd-sort's AVX2 key-value sort: 0.96-1.54 at one thread, 0.90-6.6 at 14 (words at
  14 threads is level, 0.90-1.07 over three runs; the library's OpenMP kernel does not scale on long-prefix data). 14 threads against
  __gnu_parallel::sort: 1.7-6.0x. Predicted 3-6x on high-entropy
  keys: std::sort on strings is MISS-bound (two dereferences per compare), and the key reads each string once per 8 bytes it needs.
- **Every speed and memory trick on the list, each A/B'd** (FINDINGS 3bk, CHANGELOG): five on by default. (1) A **persistent worker
  pool** under `par_run` / `par_spawn` (`LOGOS_NO_POOL=1`, `logos_twist_last(19)`): 1M at 14 threads sorted 2.29, late strays 2.44,
  few distinct 1.47-1.73, near-sorted 1.62, blocks 1.34, partition 1.07-1.13. (2) The **thin front** (<= 128 elements answered, 2047 since 2026-10-20 (3bn),
  before the dispatcher, `-DLOGOS_NO_THINFRONT`): an UNSORTED small call was 160-219 ns (pool lock, a context, a relocation plan
  before the tiny case), now 0.07-0.30 of that; sorted 0.31-0.86. (3) The **lattice map in vectors** (`LOGOS_NO_LATVEC=1`): 256
  values 2.57x and quantized 2.38x at one thread, 1.77 / 1.60 at 14. (4) **Float32 narrowing** (`LOGOS_NO_F32=1`): doubles that are
  all exactly floats, one thread, radix on 4-byte keys: 1.20-1.68. (5) **`logos_sort_f64_batch`**: many arrays, parallel across
  segments: 4.2-9.6x at 14 threads against a loop, 0.97-1.06 at one. Two opt-in, for memory: **`LOGOS_TRIM=1|hard`** (the pools'
  residue 8-10 -> 0.35-0.83 B/elem, a large call after small ones 1.3-2.5x slower) and **`LOGOS_PART_INPLACE=1`** (peak at 14
  threads 8.6 -> 1.4-2.2 B/elem, 1.25-1.38x the time). **And the memory lesson: `make rssprobe` shows resident memory, which is not
  what memprobe counts** -- the one-thread path asks for 15.5 bytes per element and holds 0.04-0.06 on the AVX2 path.
- **The scoreboard, and the four cells it said logos lost** (`make scoreboard`, FINDINGS 3bj; the board itself is the definition of
  "fastest" here: logos, logos with the library leaf, x86-simd-sort, IPS4o, std / gnu_parallel, interleaved, verified, 1 and 14 threads).
  (1) A handful of values counted in vector registers (`lc_vec_count`, `LOGOS_NO_LCVEC=1`): 1T 2 / 5 / 8 values **1.97 / 1.64 /
  1.36**, 14T **1.73 / 2.35 / 1.84**. (2) The counting routes' write-only emit streams from 2M elements (`LOGOS_LC_EMIT_PLAIN=1`):
  **1.21-1.36** at 1T. (3) The prescan in parallel past a 64K head (`prescan_par`, `LOGOS_NO_PARPRESCAN=1`): 14T sorted **1.74**,
  reversed 1.22. (4) The mode grid (`mdx_t`, `LOGOS_NO_MDGRID=1`, up to 64 modes, grid above 8): 14T 16 / 32 clusters **1.31 / 1.24**.
  Guards 0.97-1.03 everywhere. One thread after: every unstructured shape **level with x86-simd-sort on the in-house kernel**
  (0.986-1.047), five distinct values 1.64x AHEAD (was 0.685). And a crash fixed on the way: bimodal data with one NaN the sample
  missed segfaulted every 14-thread sort of the committed engine (the per-mode map cast NaN to a bucket). **Still behind:** 256
  distinct values at 1T (0.845, the lattice map is scalar), int_wide at 1T (0.916), two values (0.93, parity is the ceiling).
- **The profile phase rides on the SIMD sort** (`make frontprobe`, `make frontledger`, FINDINGS 3bi; `LOGOS_NO_FRONTFUSE=1` restores
  all of it, `LOGOS_NO_PARTFUSE=1` only (C); `logos_twist_last(15)` / `(16)` count the fused partitions and their NaN declines):
  priced against x86-simd-sort with one NaN/-0.0 scan in front, logos's own passes before the kernel cost 1.117 on uniform and
  1.045-1.057 on the skewed shapes at one thread. (A) `leaf_f64`'s skewed branch asks the kernel's scan instead of a scalar NaN
  loop; (B) `sort_fn` settles int / ascending / descending on a 64-element prefix (`sf_prefix`) and takes min, max and the
  NaN-or-zero flag in ONE AVX2 pass (`sf_profile_avx2`) whose clean verdict the kernel trusts; (C) where the kernel takes the
  whole range (sequential, from 65,536), the KERNEL'S FIRST PARTITION carries the flag (`av_qsort_fused`, `av_partition_fl`),
  as the library's own partition carries min and max, so no pass precedes the sort at all; a NaN met there declines with the
  array permuted, for callers that said they accept that (`sc_in = -2`). Against the engine as it was, layout cancelled:
  **one thread 1.140 uniform, 1.086-1.089 heavy_tail / bimodal / clustered; 14 threads 1.021-1.055** (B on every bucket);
  int_wide 1.010 / 0.994. With the library leaf, logos now reads **0.991-1.010 against the bare library call**, which checks
  nothing. Harness section `first-partition scan` (306 checks, 108 name the path). **Open (D):** the two-thread hybrid half still
  scans serially in front of the library's parallel quicksort, 7.2-7.9% of that path.
- **The in-house AVX2 quicksort, bridged to x86-simd-sort's** (`make avkernbench`, FINDINGS 3bh; `-DAV_NET_STAGED`, `-DAV_VPIVOT=0`
  restore the old kernel): timed ALONE against the library, the gap was flat in n (1.31-1.41) with half of it in the 64-element base
  case, and compiler flags and prefetch were worth nothing. Two changes found by measurement: the networks read and write the range
  directly (masked loads for the partial vector) instead of staging every call through a buffer with two variable-length memcpys; and
  the pivot is vector-sampled (five vectors, a 9-comparator network across them, the middle of the lane medians) instead of a median
  of 3 below 512 and insertion-sorted samples above. Kernel gap **1.31-1.41 -> 1.07-1.12**; end to end at one thread **1.053-1.068**
  and **1.062-1.087** (compounding to about 1.12-1.16) on uniform, heavy_tail, bimodal, clustered, layout cancelled; 14 threads and
  int_wide 0.98-1.01. **This is the dependency-free half of the hybrid**: the optional library leaf now buys little at one thread.
- **The router's other half, OPTIONAL: x86-simd-sort's quicksort as the sequential unstructured leaf** (`-DLOGOS_XSS_LEAF`,
  `logos_xss.cpp`, `make xssleafbench`, FINDINGS 3be; `LOGOS_NO_XSS=1` keeps the in-house kernel): substituted inside
  `avx2_qsort_f64` behind the same NaN decline and signed-zero repair, sequential gate only. One thread, 8M, layout cancelled:
  **uniform, heavy_tail, bimodal 1.18x, clustered 1.20x**; int_wide 1.01 (int32 radix); 14 threads and fifteen structured shapes
  0.98-1.04. Harness green with the flag. OFF by default: a C++ header-only dependency (BSD 3-Clause) is a project decision.
  **What it settles: the in-house kernel is 1.18x behind the original**, so the component probe against it is worth building,
  and the rest of the one-thread gap sits in front of the kernel. This is NOT the ledger's SIMD leaf under the partition.
  **THE PARALLEL HALF (the hybrid, same flag):** unstructured data at up to `XSS_PAR_MAX` = 2 threads goes to the library's parallel
  quicksort after every structured route has declined (`LOGOS_NO_XSS_PAR=1` keeps the partition). 8M at two threads **1.12-2.70x**;
  at 250k elements, where the thread policy itself picks two, **1.42-2.56x** on four shapes and uniform 0.95; guards 0.97-1.08.
  The gate is 2 and not the 4 the ladder suggested: at four threads eight concatenated logs read 0.66. **Owed by the engine with or
  without the flag:** the partition at two threads, `clustered` at every thread count (1.09 behind even at 14), non-uniform at 250k.
- **Lazy contexts and lazy scratch: a sort weighs what it uses** (`make memprobe`, FINDINGS 3bd; `-DLOGOS_EAGER_CTX`,
  `-DLOGOS_EAGER_SCRATCH` restore the old allocation): a leaf context carried 340 KB of fixed tables from its first allocation (the
  write-combining stage, off below 8 threads; four coarse levels a sequential leaf below ~65k never enters), P of them per parallel
  call, and the parallel dispatcher took its 9 B/elem scratch before choosing a route that mostly never touches it. Now the stage
  comes with staging, the coarse tables on first entry to the coarse path (into the pooled bundle), and the scratch when near-sorted,
  relocation or the histogram path asks. **10k: 49.9 -> 15.6 B/elem; 8.4M at 14 threads: 22.8 -> 13.8** (uniform), 23.1 -> 14.1
  (heavy_tail); one thread at 8.4M 15.6 unchanged (the sequential leaf needs its buffer). Speed at 8M neutral (0.99-1.02 both thread counts, layout cancelled); small calls of 2-10 elements 0.79-0.91 in `make callbench` (a 2 KB per-call memset went with the staging buffer).
- **The knot route's few-runs gate is 3.0n** (`KNOT_COST_MAX_FEW`, `LOGOS_KNOT_FEW=<f>` moves it alone; `./sineprobe 40 4194304
  fewgate`, `./knotbench 40 8388608 few`, FINDINGS 3bc): sequential sorts of at most 8 runs each spanning the range used to decline
  at 1.0n and go to the leaf. One thread: pure cosines of 3-8 runs **1.72-3.22x**, 8 concatenated server logs **1.28x**, and the four
  interleaved runs the 1.0n gate had been set from **1.27x** (the 0.955 of record was measured with every gate lifted at once);
  noisy waves, h = 9 and every R > 8 shape 0.98-1.02, 14 threads untouched (its gate is separate).
- **The vector two-strand merge** (`knot_merge2v_*`, `make mergebench`, `./knotbench 40 8388608 simd`, FINDINGS 3bc; off with
  `LOGOS_NO_SIMD_MERGE=1`, `logos_twist_last(13)` counts it): the knot route's balanced crossings are merged four elements of each
  strand at a time by an AVX2 bitonic network, in TWO independent chains (from the heads and from the tails, meeting in the middle),
  with one predicate and two selects per compare-exchange and one mask per pair inside a vector, so NaNs and signed zeros stay a
  permutation. Kernel **1.96-2.28x** over the branch-free scalar merge on every orientation pair (3.0-4.0 -> 1.5-1.6 ns per element).
  One thread: **staircases 1.11x at 10% crossing to 1.68x at 100%**, blocks overlapping 50% 1.51x, triple 1.22x; k >= 3 crossings,
  the gallop and every guard 0.98-1.03. **14 threads: a tie (0.96-1.04)** -- the knot route is traffic-bound there. A single-chain
  cut measured 1.14-1.29 on the kernel: the network's latency was serialised through the carried vector.
- **The kink gate and the parallel stray list: a kinked strand sorts in parallel** (`make kgverdict`, `make kinkladder one`,
  `TS_SWITCH=LOGOS_NO_KINKGATE ./tsbench 40 8388608 switch kinks`, FINDINGS 3bb; switches `LOGOS_NO_KINKGATE=1`, `LOGOS_STRAY_SEQ=1`):
  the reach probe's four windows now count KINK-SHAPED descents (`a[i+1] >= a[i-1] && a[i] <= a[i+2]`), and sparse disorder that is
  at least 90% kink-shaped (8 descents minimum) declines with `LOGOS_REACH_DECLINED_KINKS`; the P > 1 dispatcher reads that verdict
  and skips near-sorted so relocation's detector repairs the strand. Maximum displacement is the discriminator: a far-displaced
  element fails the shape at both neighbours. The gate ALONE lost 0.91-0.94 at 14 threads (random-position swaps overlap into
  3-cycles the repair cannot undo; at P = 1 the stray list lifts them, and it existed for `w[0]` at P == 1 only), so every detector
  worker now lifts strays and the join concatenates the lists; three boundary geometries are repaired at the join (a straddling
  pair, a pair at either end of the array, a swapped pair that begins a chunk). Kinks row, gate off against shipped: **2.41 at P = 2,
  2.36 at P = 4, 3.04 at P = 14**; guards 0.97-1.03 over two 15-shape sweeps. One strand + 0.5% kinks: 5.52 ms at one thread ->
  2.22 at 14 (**2.49x**, was 0.56-0.88), + 2.5%: 3.30x. **Open:** 0.05% kinks still reads 0.27-0.58 with 0 repaired, because
  `REACH_KINK_MIN` = 8 sits above the ~7.9 descents that density puts in the probe's 16,384 pairs; lower it with its own A/B.
- **The leaf picks its kernel by what the ALTERNATIVE is, not by the kernel** (`make bandbench`, off with
  `LOGOS_AVX2_OVER_FLASH=1`): `sort_fn`'s AVX2 gate sat in front of the flash/coarse decision and took the whole middle of
  the size range. What it replaces differs by size -- the scalar `quicksort` below `FLASH_SORT_THRESHOLD` (AVX2 wins
  1.31-1.47x), `flash_sort` from there to about 65536 (**flash** wins), `coarse_bits_f64` above that (AVX2 wins 1.11-1.38x,
  which is `leaf_f64`'s own gate). The gate now fires only where flash is not the alternative, with a second copy on the
  coarse branch after flash declines, bounded by a new `AVX2_FLASH_MAX_N` = 65536. Shipped configuration: **2048 1.303,
  3000 1.227, 4096 1.185, 6000 1.133**, 8192 1.037; guards 512 0.979, 1024 0.992, 131072 1.008, 2097152 0.988.
  **16384-65536 is NEUTRAL and is not claimed** (two runs straddle one: 1.005-1.040 and 0.964-0.979); the top stays at
  65536 because that is where the structure changes, not because the curve does. And `FLASH_SORT_THRESHOLD` goes
  **4096 -> 2048**: below it `sort_fn` falls through to that scalar quicksort, which has a cliff there (2048 and 3000
  elements cost 27.5 and 36.3 ns each against 6.3 and 6.7 through flash). **2048 1.286, 3000 1.352** with the AVX2 leaf on
  and **4.583 / 5.526 with it off** -- so the cliff is live on any build without AVX2+FMA and on every non-x86 target.
  512 is the floor and not a round number: a threshold of 512 measured 0.795 at n = 512. One thread; both are sequential-leaf
  decisions. Same 4 sanitizer reports as the committed engine (0h (e) not widened), callbench 0.933-1.040, frame unchanged.
- **A galloping two-strand merge for a sparse strand over dense blocks** (`knot_merge2g_*`, `./knotbench 60 8388608 gallop`, FINDINGS
  §3aj; off with `LOGOS_NO_KNOT_GALLOP=1`): one sparse strand spanning every value made every block a crossing of a few sparse elements
  and the whole block, and the branch-free merge touched every element. Eight branch-free steps while the strands alternate; when one
  still wins eight ahead, a gallop and one bulk copy (backward for a twisted strand); only crossings whose pieces differ fourfold take
  this kernel, so balanced ones keep the old one exactly. One binary, 8.4M: **2.45-2.70x at one thread, 1.18-1.31x at 14**, the whole
  ceiling; staircases and guards 0.97-1.03. Same output bytes as the branch-free merge, checked row by row.
- **The dense-integer bitmap takes distinct integers spanning up to 16n** (`DENSE_SPAN_MULT`, `./mcbench 40 8388608 span`, FINDINGS §3aj;
  `LOGOS_DENSE_SPAN=<k>` overrides it): it was 4n, from a guess about the cache. Laddered with the gate lifted, the bitmap beats the leaf
  (the int32 radix) 5.0x at 6n and 3.3x at 16n, and what the wider gate must give back grows with the map (a planted repeat 0.89 at
  16n, 0.73 at 64n). Against the old budget: **3.7-5.0x at 8.4M and 6.0-8.7x at 1M on spans 6n to 16n**, everything else 0.98-1.05.
- **A format is worth what it saves in COMPARISON WIDTH, not in distribution knowledge** (`make datestrprobe`,
  FINDINGS 3az): for NUMERIC dates a sort told the origin and step is 1.3-4.2x slower than the engine, because the
  engine already certifies quantization and low cardinality from its sample (the trace: day_serial takes the lattice
  route, ms/us timestamps take the partition at 1.01-1.04x leaf imbalance). For date TEXT it is the opposite -- a
  fixed-width ISO-8601 stamp has a monotone map to an integer, and lexicographic order on it IS chronological order,
  so 24-byte comparisons are waste. 2M records at one thread: `std::sort` on `std::string` 1724 ms, on fixed 24-byte
  `memcmp` records 989 ms, and parse + `logos_argsort_f64` + one gather **203 ms = 8.48x and 4.87x** (parse 26.3 +
  argsort 129.2 + gather 47.8). **The key is worth nothing without a fast argsort behind it**: placing the records
  with `std::sort` on indices instead read 2.67x, with placement 618 of 702 ms. This is the numeric engine earning
  its keep on record data, and it is the cheap half of direction 5 (the string engine).
- **Where the lattice route's idle threads are** (`make profile` with the route split into four phases, `make
  spawnprobe`, FINDINGS 3ay): the merge is **1.0-5.8%** of the route and the teardown **0.14%** (9-14 microseconds;
  a trace build had timed it at 2 ms, which was the trace build). The idle threads are in the **EMIT, at concurrency
  3.03-3.17 of 14** -- and not from imbalance, since every thread gets exactly n/P output positions. A standalone
  replica splits it: **create+join of 13 workers is 0.564 ms, 35% of the phase**, and the rest is bandwidth-bound
  (67 MB of stores at 65 GB/s; 14 threads buy 2.7x over one). The route pays that spawn TWICE, about **1.13 ms of a
  6.58 ms call**, which is the first per-phase price for the persistent worker pool of 0h (a). The map is
  10.72-11.32 of 14 and its inner loop touches two 400 KB arrays per element at the same random index, where one
  interleaved record would touch one line: untested.
- **Branchless sorting networks for 4 to 16 elements** (`net_sort_small`, generated by `gen_small_net.py`; off with
  `LOGOS_NO_SMALLNET=1`): the leaf finished every short range with insertion sort, which on a dozen unstructured
  elements is ~n^2/4 compares of which about half mispredict. One Batcher network per size, straight-line, taken by the
  dispatcher's tiny case and the quicksort base case. **Measured** (`make callbench`, copies swapped, two runs; random
  calls): **1.03-1.06x at n = 4, 1.22x at 8, 1.33x at 10, 1.39x at 14, 1.57-1.58x at 16**, monotone in n; n = 18 and 24
  read 0.99 (the control), sorted rows 0.97-1.04 everywhere because the prescan returns a sorted array before the leaf,
  and 1000-50000 read 0.98-1.02 because a large sort takes the flash, coarse or AVX2 leaf instead. **A small-end result
  (0g), not one at scale.** The generator proves every network by the 0-1 principle before writing it; the
  compare-exchange is one predicate and two selects, never a min and a max, so a NaN cannot duplicate an element and
  lose its partner. The assembly has 424 compare-exchanges and ONE branch (the size jump table), and `net_sort_small`
  is `noinline` with an 8-byte frame. Harness: 368 checks from n = 2 to 24, including NaNs, signed zeros, infinities
  and heavy ties at sizes nothing else here sorts.
- **The lattice map claims chunks instead of taking equal slices** (off with `LOGOS_LAT_STATIC_MAP=1`): this box is
  an i9-13900H, 6 performance cores and 8 efficient ones, and a phase that hands every thread `n/P` elements ends when
  the SLOWEST core does. Kill-tested first, from profile data already on disk: per-thread busy inside the statically
  chunked phases is bimodal (quantized's map 4.40 against 2.99 ms, 1.47x; mod_card 1.44; the hash map 2.09;
  relocation's tiles 1.39) where the queue-driven leaf reads 1.04. Chunks of `n / 4P`, never under 4096, claimed with
  one atomic; a worker that could not be created never claims and the rest drain it. Quantized and mod_card_30k
  **0.90-0.97 on 12 of 12 cells at 4M and up** over two runs of 60 pairs, against a null switch on the same rows at
  0.97-1.04: about **1.05x**. **The companion negative is the more useful half** (see the ledger): the identical change
  on relocation's memcpy tiles measured 0.96-1.04, because a work queue buys balance only where the CORES are the
  bottleneck.
- **Each lattice worker allocates its own count table** (`lattice_table_init`, off with `LOGOS_LAT_SERIAL_TABLES=1`): the
  caller used to allocate all P tables and memset `slots` keys for each before the spawn, which at 8M and 14 threads is
  5.6 MB of serial writes inside the phase the trace puts at ~80% of the route. Quantized and mod_card_30k **0.94-0.99 over
  20 of 20 cells** (60 pairs, two runs, 1M-16M) against a null switch at 0.98-1.04, so about **1.03x**; the trace confirms
  the mechanism (map phase 8.8-11.3 ms against 10.9-13.8). A failed allocation sets `abort`, which the caller already read:
  faultprobe injects 546 allocation failures and 30 thread-creation failures across the counting shapes, 0 bad.
- **Where the parallel speedup stops, per route** (`make amprobe`, `make profile`, FINDINGS §3au): the partition is healthy
  (13.0 of 14 threads busy in the leaf, 11.3 in the scatter, Karp-Flatt e = 0.055-0.09, perfect packing worth ~1.16x); the
  counting and lattice routes have **6.0-6.9 of 14 busy and NONE waiting** inside the phase that is 98-99% of their wall,
  because a serial merge sits between two parallel phases and grows with P (e rises 0.06 -> 0.23); relocation has **3.6 of
  14 WAITING at a barrier** from static output tiles; the shared sample is 1-4% and strictly serial; and kinked data sorts
  fastest on ONE thread at every P (6.84 ms against 15.6 at two and 7.2 at fourteen). Two cheap remedies were measured and
  rejected (see the ledger), which leaves the two real ones: a parallel merge for the counting routes (0a) and a task queue
  for relocation's placement. **The kinked row's MECHANISM, traced 2026-09-23 (`make kinkladder`): the kink repair fires at
  one thread and NEVER above it** -- `kinks fixed` reads 41,745 at P = 1 and **0** at P = 2, 3, 4, 6, 8, 10 and 14 -- because
  at P > 1 the NEAR-SORTED route claims the array before relocation is reached (`ns: split + in-band sorts done, 4998 strays`)
  and the detector never runs, so ~41,745 runs are sorted instead of one repaired strand: 19.4 ms at one thread against 33.2
  at two and 24.6 at fourteen. **Kinks inside SHUFFLED BLOCKS are unaffected and scale 2.9-3.1x at 14 threads**, because
  relocation takes those at every P. So it is a route decision at P > 1, exactly as the ledger row on gating that route
  concluded, and the gate wants a statistic that tells adjacent swaps from displaced ones: here the shared sample read
  `inv = 0.000, inv_all = 0.000` and the reach probe said `42 descents in 16384 -> sparse`, both of which say "local",
  where 1% random swaps would not. **BUILT 2026-09-26 (the kink gate + the parallel stray list, above): 2.49-3.30x at 14 threads over one on kinkladder's strand, 3.04x on tsbench's kinks row.**
- **The run detector's block of eight compares with vectors** (`reloc_blocks_up` / `reloc_blocks_down`, `LOGOS_SCANBLOCK_SCALAR=1`,
  `-DLOGOS_NO_SIMD_SCAN`; found by the handoff round, `TS_SWITCH=LOGOS_SCANBLOCK_SCALAR ./tsbench 40 8388608 switch`, two runs): the
  loop the trace puts at 89% of a kinked strand's sort was thirteen scalar `subsd` / `maxsd` / `comisd`, read from the disassembly. Two
  steps at a time with SSE2 or four with AVX2, and the helper advances over as many blocks as pass, so the AVX2 form is one call per run
  and not per block; the tests are the scalar ones exactly, so a NaN or inf - inf still falls to the scalar line and the run list is
  identical by construction. Kinks **1.30-1.32x at one thread**, 64 shuffled blocks half descending **1.27-1.30x** at one thread and
  1.09-1.16x at 14, 64 batches 1.06-1.12x, overlapping blocks 1.07x; twelve guards 0.97-1.02 in both runs. Two defects of my own were
  caught by the harness first: the descending bound used the ascending range, and the helper dropped the loop's `j < n` re-check.
- **Two defects found by losing to the triple-reversal weld, both fixed** (`TS_SWITCH=<NAME> ./tsbench 40 8388608 switch`, two runs each):
  (1) the prescan compares with vectors (AVX2 blocks of 32 where the CPU has AVX2 and FMA, SSE2 blocks of 16 otherwise;
  `LOGOS_PRESCAN_OR16=1`, `LOGOS_PRESCAN_SSE2=1`, `-DLOGOS_NO_SIMD_SCAN`): its block loop was commented as one the compiler
  vectorizes and compiled, at the engine's flags, to sixteen scalar compares and a serial OR chain: sorted **1.7-1.8x at 8.4M and
  2.1-2.2x at 1M, at 1 and 14 threads**, a late batch 1.1x, guards 0.98-1.03; (2) the run detector jumps to a wrong-way step
  (`LOGOS_FASTSCAN_HOLD8=1`): a block of eight that fails only on direction proves the steps before it, so the scan resumes there
  instead of walking eight scalar steps: kinks **1.05-1.08x** at one thread (1.06 at 120 pairs, null 1.00), guards 0.97-1.05. After
  both, against the weld: sorted 1.93 (was 0.88-0.92), kinks 0.94 (was 0.87). `make callbench` clean against this round's baseline (every row up to 100 elements 0.94-1.03 with layout cancelled, two runs; sorted calls of 1,000-50,000 elements 2.0-2.7x faster) once the two switch reads moved out of the dispatcher.
- **Three defects found by losing to the edge sort, all fixed** (`./tsbench 40 8388608 prescan | ties | onefix`, two runs each): (1) the
  dispatcher's prescan reads sixteen elements at a time once the direction is known (`prescan_blocks`, `LOGOS_NO_FASTPRESCAN=1`):
  sorted **2.2-2.3x** at 1 and 14 threads, reversed 1.4x, a late batch 1.3-1.4x, guards 0.99-1.06, `make callbench` clean against its own switch (0.98-1.04 at n <= 5);
  (2) a TIE continues a descending run in the relocation detector (`LOGOS_DESC_STRICT=1`): nothing here is stable, and 1.25% ties had
  cut every descending block under the floor: tied descending blocks **6.0-6.1x at one thread, 1.3x at 14**; (3) one strand's strays go
  back by one memmove per segment (`strand_place_strays`, `LOGOS_ONERUN_COPY=1`), the short-reach route's put-back: kinks **2.4x**,
  far strays 1.7-1.8x. After them, against the trimmed edge sort: sorted 0.94-0.95 (was 0.44), kinks 1.39-1.43 (was 0.59-0.60), tied blocks 2.73-2.79 (was 0.40-0.41).
- **One strand is finished, not declined** (`LOGOS_NO_ONERUN=1`, `logos_twist_last(12)`, `./tsbench 40 8388608 fixes`): one sorted strand
  with 1% adjacent swaps lost to std::sort (0.90) because the detector's repairs left ONE run and relocation needs two. Now sorted (or its
  mirror), or placed by the strays' merge: **5.5x**, guards 0.99-1.01. And the seam declines the short-reach route before its block
  phase at 14 threads: noisy ring buffer **1.30-1.34x** there. Both found by `make tsbench` (server timestamps: 3.3-6.0x over std::sort
  at one thread on every latency shape, 16x on overlapping batches).
- **Strays inside strands are lifted by the run detector** (`LOGOS_NO_STRAY=1`, `logos_twist_last(10)`, `./unknotprobe 40 8388608 ab STRAY`,
  FINDINGS §3am): at one thread, five forms (below the run and resumed; a gap above; accepted then fallen back from; two side by side; at a
  run's head, where a run that starts with a stray used to get an interval reaching down to it) and any run of one or two elements.
  The list is taken at the first stray, a density past n/64 ends the scan, the holes close in one pass, and the sorted strays are
  merged back IN PLACE OF the copy-back; a declining plan puts every stray back; a NaN is never lifted. Blocks + 0.1% strays **4.0x**,
  + 1% **3.0-3.3x**, 3% (past the cap) 1.00, guards 0.99-1.01.
- **The wave route** (`LOGOS_NO_WAVE=1`, `logos_twist_last(8)`, `... ab WAVE`): the shared sample in POSITION order, a median filter, the
  monotone pieces; each piece sorted where it lies, the knot sweep across them. Gated on the pieces agreeing with the raw sample and on
  a crossing cost from the sample (at most 1.5 levels per element). Sequential sorts of 1M and up. 32 cycles swing 0.6 noise 50
  **1.21-1.24x**, 8 and 16 cycles 1.11-1.13x, wide swings declined, guards 0.99-1.01.
- **The seam** (`LOGOS_NO_SEAM=1`, `logos_twist_last(9)`, `... ab SEAM`): a NOISY ring buffer shows in the short-reach route's block
  medians, read before any block is sorted, as one drop across the range; rotate once, enter again. **1.12-1.16x** at one thread, guards
  0.98-1.03.
- **The loop-back: a ring buffer is one strand across the wrap** (R0 on the sphere; `loopback_gate`, `./unknotprobe 40 8388608 ab LOOP`,
  FINDINGS §3al; off with `LOGOS_NO_LOOPBACK=1`, `logos_twist_last(7)` counts it): a sorted array read from the wrong slot cost 2.0x the
  sorted array at one thread and 2.6x at 14 (the tail route merged its second run element by element). When the array ends in an
  ascending run that continues into the prescan's ascending prefix (a[n-1] <= a[0]) and the joined strand is at least half of it, the
  run is rotated to the front once; a ring buffer is then sorted, anything else goes to the tail route directly (a hint to the
  re-entry's prescan did the same and cost every sorted call of 2-32 elements 10-16%: `make callbench`). Ring buffer **1.56-1.63x at one thread, 1.90-2.03x at 14**; rotated prefix_95 + tail 1.73-1.75 / 1.46-1.51; 30% prefix +
  noise + 40% suffix 2.07-2.14 at one thread (not offered at 14: over n/16 left outside the strand lost 0.51 there); guards
  0.97-1.02 over three runs; `make callbench` clean once the prescan hint was replaced (see the ledger).
- **The relocation detector's floor at one thread is 256, not the run merge's 4096** (`RELOC_MIN_AVG_RUN_SEQ`, `./unknotprobe 40 8388608
  ab FLOOR`; `LOGOS_RELOC_AVG=<k>`): every array of disjoint blocks averaging 256-4095 elements was declined and sorted in full
  (10.5-12.5 ns per element against 2.0-2.4 for blocks of 4096). Blocks of 1000 **4.2x**, staircase of 2000 3.6x, keyed runs of 3000
  5.6x, rotated keyed runs of 4000 **7.5x**, guards 0.98-1.01. At 14 threads every lower floor was mixed (0.72-1.47), so it stays.
  Found by the unknotting probe's worst row, which was not about rotation: runs of exactly 4096 passed by nothing.
- **A kink is swapped back where it stands** (Reidemeister move I; `./strandprobe 40 8388608 kinkab`, FINDINGS §3ai; off with
  `LOGOS_NO_KINK=1`): one adjacent pair swapped inside a run that otherwise goes on used to end the run, and sorted blocks with a few
  thousand of them paid for a full sort. At a run break the detector asks BOTH neighbours (the element before the pair and the one after
  it) and swaps the pair if they agree, in runs lying either way; never across a detection chunk's end. The strand stays contiguous, so
  the knot plan is untouched. One binary, 8.4M: blocks **5.8x / 5.0x / 3.2x at 0.1 / 1 / 4.8% kinks at one thread, 1.52 / 1.28 / 1.08 at
  14**, staircases 4.2 / 3.8 / 2.7 and 1.35 / 1.24 / 1.02, the same with every other block twisted, guards 0.98-1.01 over three runs.
  Ceiling first: 4.0-5.6x, from a teller that costs nothing (its 0% rows read 1.00). It is the cheap subset of strays inside strands
  (0l (a)). **The detector writes again: two elements per kink and nothing else.**
- **The run detector scans eight elements at a time** (`make twistbench` mode `scan`, FINDINGS §3ah; off with `LOGOS_NO_FASTSCAN=1`): the
  profile put the scan at 47-49% of the run routes at one thread, 1.1 ns per element for a READ. The twist had left it read-only, so eight
  elements are taken at once whenever a bound proves the scalar test would accept each (the largest step times the largest count the block
  can reach, against the gap limit for the smallest range it can have); otherwise the scalar line runs unchanged, so the run list is
  identical by construction and the harness checks that it is. 8.4M, one binary: **blocks 1.43-1.49x at one thread and 1.16-1.22 at 14**,
  x20 staircases 1.33-1.36 / 1.18, x80 1.19 / 1.16, uniform 1.00. **It moved an old verdict: against the PURE partition the run routes at 14
  threads now read 1.077 (block_perm) and 1.165 (stairs_x20), where §3ac had 0.95 and a tie; 5.77x and 4.74x at one thread.**
- **One orientation bit for the whole array** (`mirror_gate`, `./mirrorprobe 40 8388608 ab`, FINDINGS §3ah; off with `LOGOS_NO_MIRROR=1`):
  every cheap route reads order in one direction, so a newest-first log with one late record paid for a full sort. Sequential sorts,
  BEHIND relocation (the twist already sorts mirrored blocks as they lie): when a directional gate fires on the MIRROR image (a descending
  suffix of half the array, the reach ladder turned round, streams descending in arrival order) the array is reversed once and the
  dispatcher entered again. One binary, 8.4M: mirrored short reach **2.07x**, mirrored 95% prefix **2.20x**, mirrored 4 streams
  **2.76-2.81x**, every guard 0.98-1.02. Noise never reaches the gates (the first two need the sample's `inv_all >= 0.90`; the stream probe
  needs a multimodal sample and stops after ~25 reads on data that ascends). Not covered: parallel sorts (the pass eats the gain) and
  mirrored near_sorted (ceiling 1.2, no gate to ask). `logos_mirror_count()` tells a test it fired.
- **The Moebius twist: a run that lies descending is consumed as it lies** (`rdesc_t.rev`, `make twistbench`, FINDINGS §3ag): the run
  routes used to swap every descending run end for end the moment the detector found it. Now the descriptor carries one orientation
  bit, a lone piece is placed by a backward copy, a crossing is merged with the twisted cursor walking down (an up-run beside a
  down-run is a bitonic pair: this is its two-ended merge), the knot plan cuts in logical coordinates, and the detector no longer
  writes to the array (since the kink repair: two elements per kink, and nothing else). `LOGOS_NO_TWIST=1` against the twist in ONE binary, 8.4M, 120 pairs: every block twisted **1.149 at one
  thread, 1.114 at 14**, blocks of 8k 1.144 / 1.094, staircase x20 1.108 / 1.093, every-other rows 1.06-1.07 / 1.02-1.04, a tie where
  four elements in five are merged, guards 0.98-1.03. **Ceiling first**: an array with descending blocks against the same array with
  them already ascending prices a pass exactly (1.07-1.17 / 1.02-1.12); the twist recovers 84-97% of it at one thread. The harness
  section names its path through `logos_twist_last()`. Not converted: the leaf's `detect_runs_limited`, and int32 has no relocation.
- **Disorder of short reach: sort where it lies, repair only the seams** (`reach_try_f64`, `make reachbench`, FINDINGS §3af): when no
  inverted pair is further apart than a few thousand positions (an event log with bounded latency, a series shuffled inside small
  windows) the scatter is unnecessary. Index blocks of 32768 are sorted in place by a flash kernel, the seams are merged in place, and
  median fences set strays aside so that every block provably keeps an untouched middle. 8.4M: **1.9-2.5x at one thread, 1.3-1.5x at
  14**, growing with n (30M: 2.1-2.9x / 1.5-1.8x); guards 0.98-1.03 against the engine before it, layout cancelled. It weighs **0.10
  bytes per element at one thread and 1.33 at 14**, against 15.5 and 23.2. The gate is a ladder of Fibonacci lags over the shared
  sample (positions 0.31% down to 0.028% of n apart), so random data pays nothing. Strays are put back by one in-place shift pass;
  up to four threads always, wider sorts ask eight probe blocks first. f64 only; declines above ~117M elements.
- **The near-sorted gates read what they were meant to, and the general leaf tells dense disorder from near-sortedness**
  (`make nsgatebench`, FINDINGS §3af): sorted runs grouped by a coarse key 2.8-3.8x at 14 threads from 1.5M to 8.4M, 1% random swaps
  1.0-1.5x; the gate is 2.5% below 6M elements and 5% from there on, set from the first table of the ROUTE against the partition
  across sizes. Shapes the short-reach route declines: 1.36-1.87x at 14 threads from the leaf probe. All found by building the
  route above, not by looking for them.
- **Resource exhaustion costs speed, never correctness** (`make faultprobe`, FINDINGS §3ae): every allocation and every thread
  creation of a cold call can fail, singly or from some point on, and the array still comes back sorted: 7,894 injected failures
  across the f64 and int32 sorts, the view's two halves and the in-place entries, 0 bad (it was 2,369 bad of 8,502 on 19 cases). Three kinds of creation site,
  three answers: `par_run` runs an orphaned worker on the calling thread (17 sites with independent workers); `par_spawn` +
  `barrier_drop` drops it from a barrier whose count became atomic (the three queue-driven partitions), which is race-free because
  whoever completes a barrier arrives no earlier than the creating thread; relocation and the knot route, whose threads own static
  output tiles, have the caller cover the orphans' tiles; the int32 counting route declines. `merge_all_runs` keeps its lists on
  the stack; contexts are all or nothing and `ctx_fit` refuses an empty one. About 2,150 harness checks repeat a sample
  in-process (the thread-creation half under ThreadSanitizer too), and **every `pthread_create` in the engine now goes through those two helpers**, which is where a persistent worker
  pool would go.
- **What a sort weighs is measured** (`make memprobe`): f64 15.5 bytes per element at 1 thread, ~23 at 14 on the partition, ~9 on
  relocation and knot, under 1 on the counting routes; int32 11.5 per 4-byte element at 1 thread and ~6.5 at 14; 0 for the
  in-place entries; and `logos_release_buffers()` returns all of it (it returned 9 of 23, and none of the int32 count tables).
  Contexts fit a typical bucket and an oversized one borrows a shared jumbo block, a temporary, or is sorted by the routine that
  needs none. ~~Still open: the 8+1 byte scratch is taken before a route is chosen and the histogram-free partition never touches
  it; taking it lazily is 23 -> about 14.~~ **DONE 2026-09-26: 22.8 -> 13.8 (see the lazy-contexts positive).**
- **NaN has a tested floor and a separate entry with a defined order**: with a NaN present `logos_sort_f64` promises no order (the
  other values came back out of order in 246 of 672 probe runs) but no crash, no out-of-bounds access and an output that is a
  permutation of the input's bit patterns; `logos_sort_f64_nan_last` moves NaNs to the tail with their bits intact and sorts the
  prefix, for one sequential pass the default entry does not pay.
- **The strict in-place sort runs in parallel** (`make ipbench`): a placement pass does not split across threads, but its 256
  buckets are disjoint ranges, so ranges are tasks on a fixed 4,096-entry stack. 14 threads against its own 1-thread form: 2.50x
  at 8.4M and 2.85x at 50M on uniform, 1.1-1.6x on structured shapes, no loss at the 2^20 threshold; still 6-24x slower than the
  buffered sort at the same thread count, for 0 bytes of scratch against 23. It had NO harness check before (408 now).
- **The scaling exponent, in pairs** (`make slopebench`): at one thread logos's cost per element grows 13-19% from 2M to 50M where
  `std::sort`'s grows 21-24%, and the counting and relocation routes are exactly linear (p = 1.00); near_sorted is the one route
  that grows faster than `std::sort` (+29-33%).
- **The knot route: comparisons only where runs cross** (`make knotbench`, FINDINGS §3ac): runs are strands, a crossing is a value
  interval where two or more strands both hold elements, and a sweep over the 2R strand endpoints copies every piece that lies alone and
  merges only what crosses. Staircases (sorted blocks whose ranges each overlap the next), which relocation declines because one chain
  covers everything, no longer pay for a full sort. **One thread: 2.6-4.4x** across the ladder from 10% to 100% of elements crossing,
  three live strands 2.6x, eight 1.4x (1.8x where the fallback would have been the run merge rather than the leaf). **14 threads:
  1.2-1.5x against the shipped engine but a TIE against the pure partition (0.96-1.11)**, because most of that gain is the shipped
  relocation's late decline (a 2.8 ms scan it throws away) turned into useful work; three strands lose 0.89 there, so the parallel gate
  is 1.0n and the sample gate declines them in half a millisecond. Guards 1.00. Gates are measured, not predicted: the predicted n/2
  charged 4-9% for late declines. f64 only.
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
   trim is 5 sample points, so a 5% sentinel still declines. ~~the 1-thread
   `leaf_i32` gate is untouched and still pays~~ (fixed since by `leaf_i32_windowed`; found stale 2026-10-20
   when a round set out to build it). See FINDINGS §3w.
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
0f. ~~Relocation at 14 threads no longer beats the partition~~ **RE-MEASURED 2026-09-21 after the scan in blocks of eight: block_perm 1.077, stairs_x20 1.165 at 14 threads, 5.77x and 4.74x at one (same control). What follows is the record of the tie and what is still open on the knot route.** Relocation at 14 threads no longer beats the partition (`make knotbench`'s pure-partition control, FINDINGS §3ac). J5 measured
   1.9-2.2x at 20 threads on 2026-09-10. Against today's partition (an engine copy built `-DLOGOS_NO_RELOCATE`, both orientations,
   layout cancelled, 8.4M) it is **0.95 on block_perm64 and 0.92 on block_overlap at 14 threads**, and 4.4-4.5x at one. J5 did not
   change; the partition got 2-3x faster (splitter tables, histogram-free scatter, 12 buckets per thread) and the comparison was never
   re-run. The knot route ties there for the same reason: scan + place + copy back is the same memory traffic as a partition, and at
   14 threads traffic is the cost. Two things follow, neither done: (a) A/B gating the whole relocation family to sequential sorts, or
   to a sample-only decision, across 1M-30M; (b) the knot route's parallel form can only become more than a tie by dropping a pass (no
   copy back) or by a direct 3- and 4-way merge from the source. **Re-measure old parallel wins against the current partition before
   building on them.** Also open on the knot route: an int32 twin (that path has no relocation at all), and ~~a sparse strand over dense~~ (DONE 2026-09-21: the galloping two-strand merge, 2.45-2.70x at one thread)
   blocks, which the cost gate prices as whole-block crossings though a galloping merge would touch a few elements.
0g. **The small end** (`make scale-full`, FINDINGS §3ad). **Two parts of this are now settled (FINDINGS 3ba, 2026-09-23).
   (i) There is nothing to parallelise below ~100k**: creating and joining 13 workers costs 0.564 ms where a whole
   10,000-element sort costs 0.108, so spawn alone is 5.2x the work, and `effective_threads` already holds the line exactly
   (a 14-thread request reads 0.97-1.00 against a 1-thread request at every size to 31,623 and first differs at 100,000).
   **(ii) The range from 2048 to 65536 was taking the wrong kernel** and now does not (see the positives). What is left below
   is the n <= 32 regime, which is where the whole remaining loss lives. Below about a hundred elements logos loses to std::sort on every shape (**2026-10-08: the THIN FRONT now answers
   every call of up to 128 elements before the dispatcher -- random 3-100 cost 0.07-0.30 of what they did, sorted 2-100 0.31-0.86;
   **up to 2047 since 2026-10-20, with the vector prescan and kink windows: 1.1-3.7x from 129 to 2047, FINDINGS 3bn**;
   FINDINGS 3bk; the figures that follow are from before it**): 9x at
   n = 3-5, 2x at 10-32 (a call is 8 ns at n = 0 and 48 at n = 3, against 0.3 and 5.3), ahead for good from n >= 100 on the random
   shapes, 316 on the block shapes, 1000 on near_sorted. And **between 25 and 76 elements the dispatcher pays for a plan it never
   uses**: insertion sort hands over to run detection at 25, random data there is runs of two or three, a relocation plan is built
   (two allocations, a libc qsort of the descriptors) and declined, and only then does it sort: 25-36 ns per element against 18 at
   n = 24 and 10 at n = 96, so 72 doubles take 1.9 us and 96 take 1.0. Both edges of that step were predicted from the code and
   landed. Untouched: a minimum average run length before the plan is the obvious lever, and it wants its own A/B.
   **Measured since (`make callbench`, 2026-09-20): entering the dispatcher costs ~5.6 ns before any work**: a thread-local read in
   the public entry, the call, and nine probed stack pages (the dispatcher's own frame is 24 KB, 36.6 KB with what it inlines, and
   stack-clash protection touches every page on every call). `logos_sort_f64` now returns first when n < 2 (8.5 -> 2.9 ns); every
   call of 2 to 24 elements still pays it, 40% of a sorted triple and 3-10% of a random one. The lever is a THIN FRONT: prescan and
   insertion sort in a function with a small frame, the 36 KB body behind a call. Predicted 1.3-1.6x on sorted input of 2 to 10
   elements, 1.03-1.10x on random input of 3 to 10, nothing from 32 up. A second lever is untried: rows from 10 to 100 elements
   move 0.78-1.13 with code layout alone, so an alignment policy (`-falign-functions`, `-falign-loops`) may be worth more there
   than any logic, and would make that end of the scale measurable. **Measured 2026-09-22 (`make callbench`, default baseline):
   against the COMMITTED engine, sorted calls of 3 to 24 elements read 1.08-1.14 in both orientations**, where the weld round's
   own baseline read 0.96-1.01: a drift older than that round. **TRACED 2026-09-22 to the FRAME** (FINDINGS 3ax): at the
   engine’s own flags `psort_f64` is **36,640 bytes committed against 42,576 in the working tree**, and stack-clash
   protection probes every 4096-byte page on every call — 9 probes against 11, two extra on a ~17 ns sorted call, which
   matches the drift’s size. Nothing is newly inlined (no large callee disappeared between the two builds); the dispatcher
   simply carries more locals as this session added routes to it (`nearsorted_parallel_f64` 8,656 -> 33,056,
   `reach_from_sample` 32,848 and new). **The thin front is the fix, and it is now worth more than when it was costed.**
0h. **From the outside review and what it turned up, still owed** (FINDINGS §3ae). (a) **The small-n parallel regime**: at 2M and 14
   threads the structured routes cost 1.5-2.8x per element what they cost at 50M, so a parallel call there is mostly fixed cost.
   Spawning and joining 13 empty workers costs 0.77 ms here (about 60 us a thread), so a route with two spawn phases pays 1.5 ms of a 4.6 ms call at 2M. Every `pthread_create` now goes through `par_run` / `par_spawn`, so a persistent worker pool is a change to two
   functions, not to 23 sites. It wants its own A/B at 1M-4M (`make smallbench`), and near_sorted has no per-route thread cap at
   all. (b) The in-place sort's first placement pass is serial (30-35% of the work): speculative permutation with a repair phase
   (PARADIS) is the known answer; the three read-only passes before it are the cheap third. (c) The tree has only ever met GCC 13
   on x86-64 Linux: no other compiler, no AArch64, no 32-bit, no big-endian; the total-order key and the AVX2 leaf are the places
   to look first. (d) A literature search for the two ideas that might be new here: run permutations planned from non-overlapping
   value intervals (J5, the knot route) and parallel natural-run sorting. (e) With a NaN in the data the flash and coarse leaves
   still cast NaN to an index (clamped, memory-safe, undefined by the letter; per-element loops, so the fix wants an A/B).
   (f) ~~Lazy scratch acquisition, 23 -> about 14 bytes per element at 14 threads.~~ **DONE 2026-09-26: 13.8, and the contexts' 340 KB fixed part is lazy too (10k: 50 -> 15.6).**
   **(a) is DONE 2026-10-08: the persistent worker pool (`wp_*`, FINDINGS 3bk): 1.3-2.4x on the structured routes at 1M and 14 threads.** (g) near_sorted at ONE thread grows 29-33% per
   element from 2M to 50M, more than `std::sort`: the only buffered route that does.
0i. **From the short-reach round, still owed** (FINDINGS §3af, `experiments/SHORT_REACH.md`). (a) **A sentinel the SAMPLE happens to
   contain defeats the f64 partition's certificates**: 840 zeros in 8.4M jittered timestamps give KS 0.99, a rejected key grid and a
   table scatter. The int32 counting route windows its sample against exactly this; do the same here, and measure it in pairs first.
   **A control since (`./sineprobe 40 4194304 cos` and `legs`, 2026-09-22): ONE zero is enough. A sloped sine starts at 0, so a[0]
   (always sampled) is tiny, the key grid in total-order key space goes coarse (the neighbourhood of zero spans hundreds of binades)
   and is rejected: the same wave shifted to start at its crest is 0.85-0.88 of its time at 14 threads, and a wave that starts at
   exactly 0.0 costs 1.07-1.17 against its own mirror order. Window the grid from trimmed quantiles, or build it per sign.**
   (b) Three value bands 2^40 apart run at twice the uniform time at both thread counts (regime_switch in `make reachhunt`).
   (c) The lazy view's bucket sorter sends ranges under 4096 elements to a scalar quicksort when it runs parallel (0.30x on
   2048-element ranges). (d) The short-reach route: an int32 twin; a rung measured inside the probe windows, so that the gate works
   above ~117M elements; a parallel stray shift. **Reached 2026-11-08 (FINDINGS 3bx): reach_mirror at 316M is 0.62 of x86-simd-sort
   at one thread (1.44 at 10M), 20.3 ns per element against 7.3; the rung inside the windows is now a measured lever, IDEAS_LOG 18 (a).** (e) Unmeasured analogies: the transposed diagram (a stable scatter by value band),
   the sorted description (rank and quantiles from run descriptors, which fits direction 1), guessed permutations (strided order).
   (f) **Two scales of order at once** (`experiments/SHORT_REACH.md` §6, `make sineprobe`): a sine wave with a slope plus noise of
   +-50 elements costs **1.22-1.31x a sort of random data at ONE thread** (paired against a uniform reference, order alternated,
   40 pairs, two runs; the null reads 0.99-1.00), and nothing at 14 threads (1.03-1.04). **The control is the finding**: the SAME
   noise on a plain slope reads 0.59-0.63, because the short-reach route takes it, so the cost belongs to the WAVE and not to the
   noise; it tracks the REACH rather than the swing (reach 104 -> 1.27, 23 -> 1.09, 6 -> 1.05). A third control found the CAUSE: the
   same VALUES with the index order destroyed read **1.01**, level with random, so the cost is the ORDER and **the array is slower
   than the same numbers shuffled**. **The cause recorded here was WRONG, and the fix it predicted (1.45x) was built, measured 1.02 and reverted (2026-09-21, FINDINGS
   §3ai).** It ranked the coarse leaf against the AVX2 leaf by a profile build's times; paired in a normal build they cost the same on
   this shape. What IS measured: with `LOGOS_NO_AVX2=1` the wave and its own values shuffled both read 1.05 against uniform, and with the
   AVX2 leaf on they read 1.19 and 1.01, so **the AVX2 quicksort is what is slower on this order, by 1.17x**. **DIAGNOSED 2026-10-20 (`make ordkern`, FINDINGS 3bn): x86-simd-sort pays the same order cost (1.09-1.21) at the SAME partition depth as the in-house kernel, on four ordered shapes, so it is a per-level cost of in-place vector quicksort on locally ordered input -- not our kernel, not the pivot. The lever is routes that keep such orders away from the kernel.** First suspect, then:
   its pivot is the median of 63 STRIDED samples, and 32 cycles against 63 strides alternates crest and trough; try jittered sample
   positions, A/B on `sineprobe` and on the five random shapes. In the scale report's 10M row logos LOSES sine_slope to the SIMD sort
   at one thread, 251 ms against 162. The structure is all still there in a 4096-point sample (65
   monotone pieces, 2.2 covering a value, reach ~100); a composed route (strands from the SMOOTHED sample, short-reach blocks inside
   them, the knot sweep across them) is bounded by measurement at 2.1-3.7x at one thread, nothing at 14.
0j. **From the twist round, still owed** (FINDINGS §3ag, CHANGELOG). (a) The leaf's sequential detector (`detect_runs_limited`) still
   reverses descending runs in place before the galloping run-merge and the leaf's own J5; the same ceiling measurement on a sawtooth
   says what carrying the bit there is worth. (b) The detector's short-tail fusion: a chunk boundary that leaves a body under
   `RELOC_MIN_BODY` fuses with the next block, and plain relocation then declines disjoint blocks; test the gap against the NEXT run's
   slope when the body is short. (c) **A reading of `make profile-full` that ranks what is left per element** (CHANGELOG, same entry):
   the leaf is all of a one-thread sort and 65-72% of a 14-thread one. One thread: mod_card_30k is 3.2x behind the SIMD sort because
   the lattice route declines at one thread (predicted 21 -> 8-10 ns per element); bimodal costs 3x uniform in the SAME leaf for no
   known reason (component probe first); int_wide's int32 radix is 35 ns against ~17 for an AVX2 quicksort (`-DLOGOS_AVX2_INT32`
   exists, read `make simdbench` first). 14 threads: the counting, lattice and relocation routes keep their threads working only
   39-57% of the time (0a, 0h (a)). (d) **The profiler has a defect**: a NESTED sort (the near-sorted route's stray sort) overwrites
   the top-level record, so the near_sorted 14-thread cell reads `thr=1 n=159701`. Fix before trusting that cell.
0k. **One orientation bit for the WHOLE array** (`make mirrorprobe`, FINDINGS §3ag and §3ah). **(a) is BUILT and won: 2.07x / 2.20x / 2.76-2.81x at one thread, guards 0.98-1.02; see the positives. Still open: (b) the 14-thread form, (c) the lead, and mirrored near_sorted (ceiling 1.2, no gate).** Every cheap
   route reads order in one direction, and the prescan's `descending` test is exact, so a newest-first log with one late record pays
   for a full sort. A shape against its own mirror, 8.4M, two runs: short reach **2.0-2.1x at one thread, 1.4x at 14**; 95% sorted
   prefix + tail **2.7x** at one; four interleaved streams **3.1-3.3x** at one; near_sorted with 1% swaps 1.2x. (a) The plain form:
   reverse once and re-enter, BEHIND relocation (mirrored blocks read 0.98-1.02, the twist already has them, and a pass in front of
   them costs 0.77-0.90), fired only when a directional gate (reach ladder, prefix scan, stream probe) says yes on the MIRRORED
   sample; `inv_all` alone is not the gate (0.87-1.00 on these mirrors but 0.90 on mirrored blocks). Predicted 1.9-3.1x at one thread
   on the three shapes, 0.95-1.00 on everything else (a false positive costs 5-10%), and it touches the dispatcher's first page:
   `make callbench`. (b) At 14 threads the pass eats the gain (short reach 1.41 -> 1.15), so there the pass must be folded into
   work already owed (block k sorted out of place into slot B-1-k, at the cost of the route's in-place property). Unmeasured. (c) **A lead from a wrong prediction:** at 14 threads a 95% sorted prefix
   sorts **1.8x slower than its own mirror**, and the tail route is not why (it still wins 1.075 against `LOGOS_NO_TAIL=1`). Not traced.
0l. **From round four, still owed** (`experiments/TWIST_AND_KNOT.md`, FINDINGS §3ah). (a) **Strays inside strands, the next build** (its cheap subset, KINKS, is built: see the positives): sorted
   blocks with 1% late records sort 2.0-2.2x faster at one thread when told which positions are strays, through a told sort that wastes two
   serial passes (its 0% row reads 0.61), so about 3x is there. The shape of the route: lift a stray inside the detector's scan (an element
   that breaks the run while its successor resumes it), sort the strays once, and merge them back IN PLACE OF the copy-back pass. The hard
   part: a strand with holes breaks `knot_cut`'s binary search and every placement offset. (b) **Persistence is a free classifier and is not
   wired in**: 0.33 on noise, 0.95-1.00 on anything stranded, where the inversion rate reads a triangle wave as noise (16 of 16, rule fixed
   in advance). It is the gate ribbons (avenue 1) need; it cannot tell mirrored blocks from a mirrored ribbon. (c) Untouched: ribbons, the
   oriented 3- and 4-way merge from the source, descending output for free, the int32 twin. (d) The fast scan's trick (prove eight steps,
   else fall to the scalar line) has two more homes nobody has measured: the leaf's `detect_runs_limited` and the dispatcher's prescan.
0m. **From the unknotting round, still owed** (`experiments/HARD_UNKNOTS.md`, FINDINGS §3al, §3am). ~~(a) Noisy ring buffers~~ (DONE
   2026-09-22: the seam, 1.12-1.16x). ~~Strays inside strands, 0l (a)~~ (DONE: 3.0-4.2x). (a2) Round six left: the wave route recovers a
   third to a half of its told ceiling (1.21-1.24 against 2.1-3.7), and none at 14 threads; the wave's cost in the AVX2 quicksort is
   still untraced (the pivot is not it); a sorted-DESCRIPTION API would save 1.21-1.35x on block shapes (mode `nocopy`); the
   LIS classifier is measured and unwired. (a3) From `make tsbench`: ~~one kinked strand trails a reverse-and-bypass merge sort 1.6x~~ (fixed: the engine now
   leads it 1.61-1.67); millisecond clocks (84k distinct values on a 1 ms lattice) reach no counting route (1.6x over std::sort);
   at 14 threads the short-reach route declines every stray-bearing shape (retransmits, dropouts). (a4) From the edge round: the
   leaf's `detect_runs_limited` still ends a descending run at a tie; the two routes that reverse or rotate the whole array and
   re-enter (mirror, seam) read 4.3x in one run and 7.1-7.4x in the next against the edge sort, where plain jitter held 7.8x: not traced. (b) The loop-back rotates on ONE thread. (c) The parallel tail route splits
   the PREFIX evenly, so a tail lying under one prefix piece lands on one thread: split the output by merge path. (d) The
   sequential tail route moves prefix elements above the tail's maximum one at a time: gallop the top into one memmove. (e) The
   per-thread run cap at P threads is a knife edge for runs of exactly the floor. (f) Leads: at 14 threads the in-order
   staircase sorts 2x slower than the same staircase rotated (**TRACED 2026-11-05, FINDINGS 3bv: the near-sorted route claims it
   and sorts a tenth of it as strays; with `LOGOS_NO_NEARSORTED=1` it is 3.7-3.9x faster at 8.4M. FIXED 2026-11-06: the strand gate, FINDINGS 3bw, 3.46x there**); at one thread keyed runs of 8192 cost 7 ns per element against 2 for
   4096. (g) Neither change is in the JS or Rust engines. (h) ~~**The knot route's few-runs gate**~~ **DONE 2026-09-26: 3.0n, measured alone (see the positives; four interleaved runs read 1.27, not 0.955).** (`KNOT_COST_MAX_FEW` 1.0n at <= 8
   runs, `./sineprobe 40 4194304 fewgate`): on noise-free waves of 3-8 runs, each spanning the range, lifting it to 3.0n is 1.6-2.9x
   (a pure 4-cycle cosine, 8 runs, cost 1.7x a 9-run sine). It was set from four INTERLEAVED runs (0.955), where the run merge is
   the fallback; here the decline goes to the leaf. A/B 3.0n at every run count against those interleaved runs before changing it.
   (i) **A noisy wave on a trend** (`./sineprobe 40 4194304 tilt`, FINDINGS §3aq): the trend buys nothing with noise +-50, and three
   routes misfire. The WAVE route re-enters the whole dispatcher for each piece the short-reach route declines (1.42x uniform, one
   thread). At 14 threads the KS certificate (0.011) passes an equal-width grid whose buckets overflow the regions' 60% slack: 631,108
   spills, the histogram-free pass thrown away, the histogram path after it (1.39-1.54x): size regions from the sample's counts under
   the CHOSEN map. The near-sorted route accepts 21% strays at 14 threads (1.40-1.48x).
   (a5) **One kinked strand at one thread** (`./tsbench ... weld kinks`, FINDINGS §3ar): a plain insertion sort does 1% adjacent swaps
   1.46-1.64x faster than the engine did, and the weld's plain scan still leads it slightly after the break jump. The relocation
   detector's scan is ~89% of that sort at ~1 ns per element: a cheaper scan for long runs broken by rare kinks is the lever.
   **DONE 2026-09-22** (the detector's block of eight with vector compares, 1.30-1.32x on kinks: see the positives). What is left of
   that row: the pipeline reads 1.04 there now. (a6) **Eight sorted logs concatenated**, each spanning the whole range: the
   handoff pipeline's left-to-right galloping stitch reads 0.80-0.86 over three runs against the engine at one thread, in three runs. The trace names two causes (`./tstrace 1 1048576 once concat`): the knot route declines at its few-runs gate (8 runs, each spanning the range, crossing cost 3,030 = 3.0n against a gate of 1.0n, which is open direction 0m (h), where lifting it to 3.0n measured 1.6-2.9x on shapes of this kind), and then the SEAM route MISFIRES: it reads the eight log boundaries as a noisy ring buffer's single drop, rotates the whole array and re-enters, after which the leaf sorts it anyway (18.2 ms of an 18.3 ms sort). Two levers: that gate, with its own A/B against interleaved runs, and the seam gate's false positive.
1. **`logos_query` — order/analytics API.** Promote `experiments/paradigm/`'s
   `OrderModel` + profiler into `engines/c/` as first-class entry points
   (`rank`, `select`, `quantiles`, `topk`, `count_in_range`, `distinct`) reusing
   the engine's cardinality/lattice detection to pick exact-vs-sampled. Add a
   `querybench` (fair: vs `nth_element`-class baselines, not just "sort first").
   This is where logos genuinely beats the state of the art.
2. **Structure-router**: probe cheaply; route structured data to logos's
   counting/lattice paths (beats x86-simd-sort), everything else to
   x86-simd-sort wholesale. Would win every parbench row. Honest framing: logos
   as router, SIMD as engine. **State 2026-10-05:** built for one thread and two (`-DLOGOS_XSS_LEAF`, FINDINGS 3be/3bg), and the
   router's own profile phase now costs nothing at one thread (3bi: level with the bare library, checks included). Left: the
   two-thread half's serial NaN/-0.0 scan (7.2-7.9% there; fold it into a top partition with the two parts on two threads, or
   scan in parallel), and the partition at four threads and up, where the library loses.
3. **GPU/SIMT port of the radix/counting paths.** The one hardware where scatter
   is cheap and *addressing beats comparing* outright — logos's paradigm finally
   wins on its own terms. Cannot be developed on CPU-only hardware.
4. **NUMA-aware partition** (private per-thread buckets, socket-local memory).
   Only measurable on multi-socket hardware.
5. **Port the JS string engine** (LCP-skip MSD, packed-prefix radix, string
   run-merge) to C/Rust. ~~Currently multikey-quicksort only.~~ **C DONE 2026-10-09 in its own form (8-byte prefix keys through the
   numeric machinery, FINDINGS 3bl): 4-14x over std::sort at one thread, ahead of the same scheme on x86-simd-sort.** Left: Rust, an
   argsort for strings (record data), a near-ordered check finer than the prescan (logs, the weakest row at 2.4x).
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
  costs. Structural levers only: fewer passes, better balance, or skip the scatter. **That is the SCATTER.** A pass that only
  WRITES is a different case: the counting routes' emit read every line before writing it, and non-temporal stores there measured
  1.21-1.36x at one thread (2026-10-07, `LOGOS_LC_EMIT_PLAIN`). Ask whether the pass reads its destination before ruling it out.
- Don't claim SIMD parity: logos+SIMD reaches parity single-threaded, never
  exceeds; parallel-SIMD-leaf is measured worse than plain logos.
- Don't quote a parallel route's win against the shipped engine alone. If a sibling route probes and then declines on the same
  shapes, its wasted probe is in your baseline (the knot route: 1.2-1.5x that way, a tie against the pure partition).
- Don't add a route, a fallback or a pool without failing it once: `make faultprobe`, and `make test`'s resource section. Every
  condition the harness had never produced found a defect on 2026-09-19/20.
- Don't state a memory figure that `make memprobe` did not print. The header said "~9n" until it was measured: 15.5 to 104.
  And say WHICH figure: memprobe counts bytes asked for, `make rssprobe` bytes resident. The one-thread path asks for 15.5 per
  element and holds 0.06 on the AVX2 path; the pools hold 8-10 after any call that touched them, until released.
- Don't call `pthread_create` directly in the engine: `par_run` (independent workers) or `par_spawn` (workers that meet at a
  barrier, with `barrier_drop`). A bare call is a hang or a wrong answer the day a container runs out of pids.
- Don't write a pre-gate as a simplified copy of its route's scan, and don't trust a statistic's comment: run the route's own scan
  on the windows, and count the pairs a statistic reads. The near-sorted route was mostly unreachable for both reasons.
- Don't accept a harness section for a new route that cannot say which branch ran (`logos_reach_last()` is the pattern).
- Don't touch the first page of the dispatcher or of the leaf without `make callbench`: arrays in a helper that gets inlined there
  are stack-probed on EVERY call (`-fstack-usage` prints the frames), and a thread-local access is a function call under `-fPIC`.
  And don't believe a small-call row without a mechanism: 10 to 100 elements read 0.78-1.13 on identical logic. And print the
  dispatcher's frame (`-fstack-usage`) after EVERY round, not only one that touched it — **at the ENGINE's flags, and read
  the RIGHT `.su`**: it is written next to the `-o` target, so `-o /dev/null` writes none at all and `-o /tmp/x.o` leaves a
  stale one in the source directory that a later read will believe (2026-09-22: the stale one was a `-march=native` build,
  which also gives a different frame): an inlining decision moved when another
  callee changed size, the frame grew 35.9 -> 60.5 KB, and small calls cost 4-5x for a round (fixed with `noinline`, 28.3 KB).
- Don't build a harness row for a route out of a SORTED array: the prescan returns it first. The dense route's "must decline" row
  never reached its gate. Shuffle, and assert the path.
- Don't rank two leaves, or two routes, by a profile or trace build's times. Those builds say WHICH path ran and what share of its own
  time each phase took. A fix was designed on 126 ms against 87 from a profile build, predicted 1.45x, and measured 1.02.
- Don't add a public symbol without adding it to `RENAME` in the Makefile, and rebuild one multi-copy bench (`make ledgerbench`) to prove it:
  every such bench stopped linking for a round and no check noticed.
- Don't gate on a statistic over STRIDED pairs when the data may be periodic: it aliases (four mirrored streams read 0.88 or under 0.60 by
  the array's length alone). Ask the route's own probe.
- Don't trust a comment that says a loop vectorizes: read the disassembly at the ENGINE's flags (`-O3 -fPIC`, no `-march`). The
  prescan's block loop was commented as vectorizable and was sixteen scalar compares for a round; vector compares made sorted input
  1.7-2.2x. And an intrinsics header first included inside a `#pragma GCC target` region carries that target: include baseline ones
  at the top of the file. And read a new A/B switch inside the function it switches, not beside the dispatcher's own reads:
  two `getenv` calls there moved random calls of 24 elements to 1.14 in two callbench runs (1.00 once moved).
- Don't test an engine change with `make liblogos_psort.a && ./harness`: the harness links the library STATICALLY, so the old binary
  runs and a bisect reads backwards (three steps of one pointed at innocent code). `make harness`, or `make test`.
  **And never send a build's output to `/dev/null` in a script that then reports the tests passed** (2026-09-22): a new
  harness section failed to compile, `make` left the previous binary in place, and the script printed "all correctness
  checks passed" for both settings of a switch the binary did not contain. Two tells, both cheap: the binary was six
  minutes OLDER than its source, and the section printed no check count. A verification script keeps the build's exit
  status and its errors, and a new section is not believed until it prints how many checks it ran. **Grepping the build for
  `error` and running on is the same mistake** (2026-10-19): an edit cut a function's pivot, the library failed to build, and a
  sequential-leaf A/B ran against the stale library and read 1.00 everywhere. Stop the command on the build's exit status.
- Don't measure memory one fresh process at a time only: `make mixprobe` holds a mixed sequence of sorts in ONE process, where
  buffers that never meet in rssprobe coexist (23.8 bytes per element held between calls before 3bq). And a nested sort is a
  sort: wrap it in `NESTED_SORT`, or its context bundle stays taken until the next top-level call.
- Don't free a large malloc'd block in the middle of a call and then allocate again: glibc's dynamic mmap threshold moves every
  later block of up to that size to the heap, the caller's included (FINDINGS 3bp). A buffer that lives inside one call is an mmap.
- Don't read a pass without a control row's ABSOLUTE time next to it, and don't take WSL's load average as the box's. On
  2026-10-21 a headless Chrome from ANOTHER Claude session (another project) took ~35 cores' worth of CPU time from 12:42; uniform read
  18-33 ns per element against 9-10, a third of a pass's ratios were noise, and `/proc/loadavg` read 0.5 throughout. Windows'
  `Get-Counter '\Process(*)\% Processor Time'` found it; its user-data-dir named the session. Another session's process is the
  user's to stop, not this one's.
- Don't choose between kernels from ONE input sorted repeatedly: a predictor learns the branchy kernel's branches on a repeated input.
  bandbench's flash window (2026-09-23) reversed on distinct inputs (2026-10-20, `make bandbatch`: clustered data 2.6-15x the other way).
  Time batches of distinct instances, as edgeprobe, thinbench and the scale report's small sizes do.
- Don't trust a harness row's premise across rounds: a row built so that one route takes it can be taken by a route added later (the
  run-length row with one swapped pair at n/4 is an unsorted head before a sorted suffix, and the head route took it; the vector merge's
  two-strand row, built 40/60 so the TAIL route would not take it, is a sorted head before a sorted suffix, and the head route took that
  too). When a row fails after a new route lands, ask which route is right for the shape before changing either -- and whether the
  witness counts the KERNEL or one route's use of it (`logos_twist_last(13)` now counts the head and tail routes' vector merges).
- Don't bump the version for a change that failed its A/B.

## Build / verify quick reference (engines/c)

```sh
make test            # 55,809 checks in 52 sections (2026-11-10) — must pass
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
make scale-full      # time vs n (0, 1, 3, 5, 10, then half-decades to LOGOS_MAX_N, default 1e7 since 2026-09-23) for every sort and
                     # TWENTY-SIX numeric shapes (nine standard + block_twist, block_kinked, sine_slope, reach_mirror, and since
                     # 2026-11-08 THIRTEEN PRACTICAL ONES, each a column people sort and generated the way the real thing comes about:
                     # event_times, prices_cents, ids_dense, fk_zipf, zip_codes, ages, sensor_temps, latencies_us, geo_lat, log_batches,
                     # shard_ids, appended_rows, order_dates; SHAPE_DOCS in scalebench.cpp says what each is; LOGOS_SCALE_SHAPES=a,b,c
                     # runs only the named shapes, numeric or string) and since 2026-10-11 STRING shapes (uuids, words, urls, logs, and
                     # since 2026-11-08 paths, emails, dates;
                     # bench_strings.h, LOGOS_SCALE_STRINGS=0 skips them: std::sort / gnu_parallel / IPS4o with strcmp, logos_sort_strings_mt,
                     # x86-simd-sort's key-value sort under the same 8-byte-prefix scheme, the old multikey quicksort; each panel names its
                     # own contenders through the series' `names`, and outputs are compared AS STRINGS) -> scale-report.html: per panel a time chart and an
                     # ns/element chart whose value axis is always linear; below 10k a size is a batch of distinct inputs. LOGOS_MAX_N=5e7 restores the
                     # old ceiling (its 31.6M and 50M rows were most of the run, and the ns/element curves still
                     # rise past 10M, so use it when the top of the curve is the question); LOGOS_MIN_N=10000 = the old ladder;
                     # LOGOS_SCALE_REUSE=<report.html> re-renders measured data through the current template without measuring.
                     # THE SEAM: below 10k a point is a BATCH of distinct instances (~2 MB swept, cache-cold, none repeated),
                     # at 10k and above it is ONE array sorted repeatedly with the MINIMUM kept (78 KB, L2-resident, same
                     # input every rep). Working set, branch history and statistic all flip there, all favouring the right.
                     # A dashed rule marks it on every chart and a seam pass measures the step: n = 10000 timed BOTH ways,
                     # per contender per panel, printed as `batch / single` and shown as a table row. ~33 s;
                     # LOGOS_SCALE_SEAM=0 skips it. MEASURED: 1.03-4.64x across the 1-thread contenders (median 2.69), and
                     # 6-174x for __gnu_parallel::sort at P threads, where a batch opens 26 parallel regions over 10k
                     # elements. Read it before calling a jump at that x an algorithmic cliff -- and note it does NOT
                     # explain every one: sine_slope has the report's BIGGEST cliff (2.34x / 2.64x) and among its
                     # SMALLEST seams (1.34x / 1.54x), so that jump is the DATA (a shape is not the same shape at 3162
                     # as at 10000, and the routing gates are ratios of n). The two causes are separated by this row.
                     # MEMORY: each point carries a circle sized by the peak heap of one COLD call at that size, in BYTES PER
                     # ELEMENT, from a pass that runs after all the timing (releasing the pools is what makes a call cold, so
                     # doing it between timed rows would leave the next row paying for its buffers). The largest reading in the
                     # report is 30 px across and the rest scale linearly by it, so a circle means the same number in every
                     # panel; std::sort and the in-place entry draw none (they allocate nothing), and n < 10k has none (a size
                     # there is a batch). LOGOS_SCALE_MEM=0 skips the pass. The draw hook counts what it strokes in
                     # window.__MEMCIRCLES, which is how a test says the circles reached the canvas rather than assuming it.
                     # THE VALUES ARE VISIBLE TOO (2026-09-26): a "peak heap" column in the tooltip and a third table per panel;
                     # the first three arrays of the DATA line are the sub-10k rows and read 0 BY DESIGN -- do not read them as
                     # "the pass recorded nothing". LOGOS_SCALE_REUSE strips CRLF now (a Windows checkout writes it).
                     # THE SHAPES (2026-10-19): a collapsed accordion above the charts, one entry per shape: what the data is,
                     # a profile, two inline SVG plots and the values, built by shape_demos_json() from the bench's OWN gen() at
                     # n = 2^20 when the page is written (so a REUSE re-render carries it); each panel heading links to its entry.
                     # The descriptions are of the DATA, not the route: read routes from a trace build
                     # AGE: the header reads "measured N hours ago" from META.measuredAt, stored with the data, so a REUSE
                     # re-render keeps the ORIGINAL time. Any timer the page starts must be unref'd (node): a live
                     # setInterval hung tools/check_report.js until the 10-minute timeout.
                     # The charts zoom and pan (Ctrl + scroll, drags, keys, pinch; a plain scroll moves the page). After touching the
                     # template run BOTH checkers from Windows: node tools/check_report.js, python tools/browser_check.py (it drives them)
                     # SINCE 2026-11-05 (FINDINGS 3bt): Google's VQSort is an eighth series (1T only; HWY=../highway), every P-thread opponent
                     # is GIVEN P (IPS4o had used all 20 hardware threads), the series follow the validated dataviz palette order, and the page
                     # OPENS for a reader outside the project: what logos is, "At the largest size" (logos against the fastest other sort per
                     # input, computed from DATA), the contenders, the method, then the charts, then how to reproduce. META carries cpu,
                     # compiler and vqVersion. Shared as a private claude.ai artifact: strip the document wrapper (doctype, html, head, body)
                     # SINCE 2026-11-08: the recipe also renders ../../index.html, THE CLEAN PAGE (tools/scale_index.py: the report's DATA /
                     # META / SHAPES as one short static page, inline SVG charts from 10k up with a hover layer; `make scale-index` alone;
                     # `node tools/check_index.js index.html` drives the hover layer and the per-element / total-time switch under a stub DOM). A LADDER PAST 10M needs three switches:
                     # LOGOS_MAX_SEC=600 (a shape stops once std::sort at one thread passes HALF the per-sort budget, and std::sort at 1e9 is
                     # ~100-160 s), LOGOS_STR_MAX_N=1e8 (1e9 strings would hold ~70 GB) and LOGOS_SCALE_MEM_MAX_N=1e8 (the cold-call memory
                     # pass runs EVERY contender once more per size; per element it is flat from 10M). The bench holds THREE arrays of n at a
                     # time (base, the reference, the working copy; it was four until 2026-11-08), so 316M peaks near 15 GB and 1e9 near 40 GB
                     # with the sort's own scratch: 1e9 does not fit WSL2's default cap of half the host's RAM (31 GB here) without a
                     # .wslconfig. The 2026-11-08 run went to 316M for that reason. THE RECIPE OVERWRITES reports/scale-report.html:
                     # the 316M report was lost to the 50M run that followed it the same day (its console log survives as
                     # reports/scale-run-316m-2026-11-08.log). Copy the report aside before a run whose predecessor is worth keeping.
                     # A 50M run on all 33 inputs takes ~1 h 20 min (numeric ~1 min a shape, strings ~3.5, the memory pass ~35 min)
make noiseprobe      # min/median/max of one sort per thread count: how noisy is this box right now?
make pairprobe       # paired A/B calibration: same engine both sides, so what it prints is the error floor
make mcbench         # counting routes: lattice slot budget, the hash route's tail, the dense-integer route; `./mcbench 40 8388608 span [lift] [first]`:
                     # the bitmap's span budget laddered (lifted to 256n by default) with guards; `... span - 4` = the old 4n budget against shipped
make chaoshunt       # twenty realistic shapes: logos vs a sort TOLD the structure, plus the route logos took
make zroute          # the (index, value) quadtree profile scored against chaoshunt's zoo, rule fixed in advance
make ztrain          # integrity check: the zroute classifier as coded must reproduce its six training shapes
make ztail           # the tightened tail rule vs the original, on fifteen fresh shapes
make prefixbench     # sorted prefix + unsorted tail route: paired A/B, LOGOS_NO_TAIL off, LOGOS_TAIL_SEQ=1 sequential-only
make streamsbench    # interleaved ascending streams route: paired A/B, LOGOS_NO_STREAMS; `./streamsbench 20 8388608 lookup` = table vs compares
make cardgate        # hash-count route gate: ground truth, ladder, skew, gate, heldout, intness, v2 modes (see its header)
make kgbench         # key grid for heavy tails: paired A/B, runtime toggle LOGOS_NO_KGRID
make knotbench       # `... ceiling` = sparse strand over dense blocks against its twin with nothing crossing; `... gallop [row]` = LOGOS_NO_KNOT_GALLOP=1 against shipped.
                     # The knot route: staircases of overlapping runs, paired A/B, runtime toggle LOGOS_NO_KNOT; `./knotbench 20 8388608 lift` lifts the cost gate, a 4th argument filters shapes
make knottrace       # the same bench over a -DLOGOS_TRACE engine: `./knottrace 5 8388608 once` prints which route took each shape
make reachhunt       # ceilings for short-reach disorder: cuts, blocks + seams, links, beside the zoo (included unchanged); paired rows
make reachbench      # the short-reach route: paired A/B, runtime toggle LOGOS_NO_REACH; `./reachbench 40 8388608 - latency` reruns rows by name;
                     # RB_OFF=NAME=VALUE RB_ON=NAME=VALUE compares two settings of a knob with the route on (LOGOS_REACH_LEAF, LOGOS_NO_DENSE_PROBE);
                     # BENCH_PAR_ONLY=1 skips the 1-thread rows. make reachtrace: the same over a -DLOGOS_TRACE engine (`once` prints the gate's decisions)
make sineprobe       # `... cos` = cos against sin (same noise; routes), `... fewgate` = the knot route's few-runs gate against 3.0n,
                     # `... legs` = a cos descending leg and a sin ascending leg, both orders; `cosonce` / `legsonce` for make sinetrace.
                     # `... tilt` = a linear trend under the wave, none to past the slope where it stops falling; `tiltonce` for sinetrace.
                     # `./sineprobe 40 4194304 ab [SWITCH]` = every sine shape against itself with one per-sort switch set. Default mode: a sine wave with a slope: monotone pieces, how many cover a value, reach inside a piece, and whether that structure
                     # COSTS anything, every shape PAIRED against a uniform reference with a null and a no-wave control (1.22-1.31x at one
                     # thread, nothing at 14: open direction 0i (f)). make sinetrace: the routes, from a trace engine, never timed
make twistbench      # the Moebius twist (descending runs consumed as they lie): `./twistbench 40 8388608 ceiling` prices the reversal passes,
                     # `... ab` is LOGOS_NO_TWIST=1 against the twist, `... merge` is the strided two-ended merge (LOGOS_TWIST_GENERIC=1) against
                     # one loop per orientation pair, `... scan` is the detector's scalar scan (LOGOS_NO_FASTSCAN=1) against blocks of eight,
                     # `... list` / `... listplain` is the shipped placement against the LITERAL linked list (LOGOS_KNOT_LIST=1; 0.5-0.75x, opt-in).
                     # One binary, one engine copy: no layout term. Use 120 pairs on a loaded box
make unknotprobe     # `./unknotprobe 40 8388608 ab [LOOP|FLOOR|SEAM|STRAY|WAVE]` = each shipped change against its own switch; `nocopy` = the
                     # copy-back pass priced (wrong output by design); `lis` = the LIS classifier; `once2` = the round-six shapes for unknottrace.
                     # The first two rows: the loop-back (LOGOS_NO_LOOPBACK=1) and the one-thread relocation floor
                     # (LOGOS_RELOC_AVG=4096) against the shipped engine; `ring` = a shape against itself rotated as a ring buffer; `moves` /
                     # `count` = the unknotter (circular doubly linked list, Reidemeister moves) against the engine; `cliff [k]` = the floor's
                     # ladder; make unknottrace + `once` = the routes taken
make tsbench         # server timestamps with slight knots: `./tsbench 20 8388608 all` = the engine against std::sort, the boundary-bypass
                     # hybrid and itself with the order routes off; `anatomy` = what the hybrid's check skips; `fixes` = the one-strand
                     # finish and the seam at P threads against their switches; make tstrace + `once` = the routes taken;
                     # `edge` / `edgeanat` = the edge sort (blocks by min edge) and its claims; `prescan`, `ties`, `onefix` = the three
                     # fixes against their switches; TS_SWITCH=<NAME> ... switch <filter> = one switch, or a NULL one, against shipped;
make unibench XSS=../x86-simd-sort/src   # P chunks by the AVX2 quicksort + a merge-path tree vs the shipped partition; `anat` = chunks vs merge
make amprobe         # where the parallel speedup stops: a thread ladder with the Karp-Flatt serial fraction per route; `ladder` = the
                     # counting routes across five sizes. Read it beside make profile (busy / waiting / idle threads per phase)
make capbench        # any per-sort switch paired against the engine as it ships, across 1M-16M: CAP_SWITCH=<NAME> [CAP_VALUE=<v>]
                     # ./capbench [pairs] [ceiling] [row filter]. Used for the counting-route ceiling (a negative), the lattice
                     # merge in parallel (a negative) and the per-worker tables (shipped, ~1.03x)
make dllbench        # the proposed Moebius DLL sort (as written, and relinked) against the engine at one thread, seven shapes; the JS side:
                     # node --expose-gc experiments/moebius_dll/bench.js [pairs] [n] (against logos-sort and V8's sort). Both slower on every row
make strandprobe     # `... kinks`: adjacent pairs swapped inside runs, against a pass TOLD where they are (the ceiling); `... kinkab [guards]`:
                     # the detector's kink repair against LOGOS_NO_KINK=1. Also `./strandprobe 30 8388608 strays`: sorted blocks with late records against a sort TOLD which positions are strays (a ceiling;
                     # its 0% row prices the told sort itself); `./strandprobe 0 8388608 writhe`: writhe + persistence from 2,048 points, rule fixed first
make mirrorprobe     # `... ab [row]` = the whole-array orientation bit against LOGOS_NO_MIRROR=1 (mirrored rows and as-it-lies guards). Also: where else an orientation bit pays: each cheap route's shape against its MIRROR image (ceiling) and against one timed
                     # reversal pass plus the sort (simple), uniform and block controls, descending teeth for the leaf's detector;
                     # `./mirrorprobe 60 8388608 tail` = the tail route against LOGOS_NO_TAIL=1; make mirrortrace + `once` = the routes taken
make waveprobe       # what share of the wave route is the KNOT SWEEP: LOGOS_WAVE_NOSWEEP=1 runs everything up to the sweep
                     # and stops (output wrong by design, as ./unknotprobe nocopy does), the two alternating inside one
                     # process. MEASURED: the sweep is 51.3% of the route at 8.4M / 32 cycles, and its share is FLAT in
                     # strand count (54.5% at 16 pieces, 46.2% at 128), so it is O(n) movement -- knot_place_seq writes n
                     # and the copy-back reads n -- not O(n log strands). Read its WITNESS line first: the shape must be
                     # unknotprobe's gen_sine or the route declines and the ratio is 1.0 for the wrong reason
make kinkladder      # does kinked data want more cores? The repair is ALREADY per-worker (detect_runs_parallel gives each
                     # thread its own chunk). `./kinkladder 9 8388608` = 64 shuffled blocks + kinks, which scale 2.9-3.1x at
                     # 14 threads; `... one` = ONE sorted strand + kinks, which does NOT: 19.4 ms at 1 thread against 33.2 at
                     # 2 and 24.6 at 14, because at P > 1 the NEAR-SORTED route claims it before relocation and the kink
                     # repair never runs (`kinks fixed` reads 41,745 at one thread and 0 at every other). `once [P]` sorts
                     # once for a -DLOGOS_TRACE build to name the route. Its own shape bug is the lesson: a[i] = i is a
                     # PERMUTATION OF 0..n-1 even after adjacent swaps, so the dense-integer bitmap claimed it and the
                     # repair read 0 fixed, which looked like an engine defect for an hour. + 0.5 fixes it.
                     # SINCE 2026-09-26 (the kink gate): one strand + 0.5% kinks 2.49x at 14 threads, 2.5% 3.30x; 0.05% still 0.27-0.58
make whyfast XSS=../x86-simd-sort/src IPS4O=../ips4o/include   # WHAT MAKES EACH SORT FAST: logos, x86-simd-sort and IPS4o under five
                     # sweeps that change one property each (order, size, duplicates, distribution, threads), interleaved on identical
                     # data, verified. Compare ACROSS a row freely, DOWN a column with care (rows are minutes apart). FINDINGS 3bf
make pmuprobe        # asks the kernel for hardware counters through perf_event_open: NONE open under WSL2 on this box, so
                     # instructions, branch misses and cache misses cannot be counted here. Re-run it on any new machine first
make avkernbench XSS=../x86-simd-sort/src   # the in-house AVX2 quicksort ALONE against x86-simd-sort's (built -march=native and -mavx2),
                     # 64 to 8M, the base-case networks alone at 8-64, partition levels against the ideal; avkernbench_staged = the
                     # old staging buffer. Ladder a kernel knob by building the engine object with -D... and linking avkernbench.cpp
make xssleafbench    # x86-simd-sort's quicksort as the sequential leaf against the in-house copy (and, same flag, the hybrid's parallel half): two engine copies, one built
                     # -DLOGOS_XSS_LEAF, the shim (logos_xss.cpp) linked beside them. Run BOTH orientations and read sqrt(A/B):
                     # make xssleafbench && ./xssleafbench 40 8000000; make -B xssleafbench XL0=-DLOGOS_XSS_LEAF XL1= && ./xssleafbench ...
                     # Needs the clone (XSS=../x86-simd-sort/src). 1.18-1.20x at one thread on unstructured shapes, nothing at 14
make mergebench      # the two-strand merge KERNELS in isolation: the branch-free scalar merge against the AVX2 bitonic merge, paired,
                     # four orientation pairs, 65k and 8.4M, every output checked with signed zeros and ties planted (1.96-2.28x).
                     # End to end: ./knotbench 40 8388608 simd (LOGOS_NO_SIMD_MERGE=1 against shipped), ./knotbench ... few (the
                     # few-runs gate at 3.0n, LOGOS_KNOT_FEW=3), and tsbench's switch mode now takes TS_VALUE=<v> for a valued switch
make scoreboard      # WHO IS FASTEST: logos, logos + library leaf, x86-simd-sort, Google's VQSort (1T only), IPS4o, std / gnu_parallel on 17 shapes, 1 and 14
                     # threads, interleaved, verified, min of N; re-execs itself with OMP_WAIT_POLICY=PASSIVE (see the ledger). LOSS = < 0.97.
                     # ./scoreboard [n] [reps] [threads] [shape filter]. Not paired: rows minutes apart, read a 14-thread half only on a
                     # quiet box (its gnu_parallel column is the tell), and settle anything close with frontprobe's ab mode
make frontprobe      # what logos's PROFILE PHASE costs in front of the SIMD kernel: the hybrid (-DLOGOS_XSS_LEAF) against the same library
                     # call with one NaN/-0.0 scan in front and alone, paired; `./frontprobe [pairs] [n] [threads] [shape] ab` = FP_SWITCH
                     # (default LOGOS_NO_FRONTFUSE; LOGOS_NO_PARTFUSE for lever C) against shipped, guards included; frontprobe_inhouse =
                     # the same over the DEFAULT engine (in-house kernel). Needs XSS=../x86-simd-sort/src
make frontledger FL_BASE=<an older logos_psort.c>   # any engine SOURCE against the working tree at 8.4M, ledgerbench's five shapes at
                     # 1 and 14 threads, two copies built twice with the assignment swapped: ./frontledger_a 80 8388608 &&
                     # ./frontledger_b 80 8388608, effect = sqrt(a / b). The check a switch A/B cannot make (see the ledger)
make kgverdict       # the reach probe's verdict (must read DECLINED_KINKS) and the kinks repaired at P = 1, 4, 8 on three kinked-strand
                     # shapes: the witness for the kink gate. ./kgverdict [n] [shape 0|1|2]
make bandbench       # the leaf's choice between the AVX2 quicksort and flash_sort, SIZED: one binary, two copies of today's
                     # engine differing only in -DLOGOS_AVX2_OVER_FLASH_FORCE, built twice with the copies swapped so the
                     # layout bias cancels. 12 sizes from 512 to 2M on uniform doubles, which is the shape that reaches
                     # these paths (callbench's does not: see its header and the ledger).
                     # make bandbench BB_OLD=-DX=a BB_NEW=-DX=b   pairs ANY two leaf settings at the small end, e.g.
                     # make bandbench BB_OLD= BB_NEW=-DFLASH_SORT_THRESHOLD=2048; add LOGOS_NO_AVX2=1 to ./bandbench_a|_b
                     # to see what a build without AVX2+FMA does (where the scalar quicksort's 2048-3000 cliff is live)
make callbench       # what ONE CALL costs at n = 0..50000, random and sorted: a baseline engine (the committed one, or CB_BASE=<source>) against
                     # the working tree, two binaries with the copies swapped. Run it after touching the first page of the dispatcher or the
                     # leaf. A row counts at n <= 5, in both orientations and two runs, with a mechanism visible in the assembly.
                     # Its SIZES list is coarse (0,1,2,3,5,10,24,...): to resolve an effect that varies with n, narrow it
                     # temporarily (the small networks were mapped at 3,4,5,6,7,8,10,12,14,16,18,24,32) and put it back
python gen_small_net.py   # regenerates the small sorting networks in logos_psort.c (run from engines/c). Every network is
                     # proved by the 0-1 principle before it is written; do not hand-edit the generated block
make strandgate      # the near-sorted route's STRAND gate (FINDINGS 3bw): 27 shapes at P threads, per shape the gate's statistics (inv_all, the
                     # pre-gate's rejects, descents and strays of a 2048-point sample in POSITION order) beside a paired timing of SG_SWITCH (default
                     # LOGOS_NO_NSSTRAND; LOGOS_NO_NEARSORTED = the whole route) against shipped: ./strandgate [pairs] [n] [P] [filter]; > 1 = shipped faster
make nsgatebench     # the near-sorted route's two repaired gates against the gates as they were (LOGOS_NS_GATE_OLD=1); NSG_ROUTE=1: the route
                     # itself against the partition (LOGOS_NO_NEARSORTED), which is what its size-dependent gate was set from
make mixprobe       # what ONE process keeps resident across ten different sorts (3bq): rssprobe cannot see buffers that coexist
make rssprobe        # (since 2026-10-22: 21 shapes, the int32 API and one value holding 25-99% among them; a cell is bytes resident per element)
make memprobe        # what ONE cold sort weighs: peak heap, what is still held, what is left after release; f64 and int32, 1 thread and P, in-place
make rssprobe        # what a sort keeps RESIDENT (fresh process per row): peak during one sort, held after it, after 32 small sorts,
                     # the LazyFree part of that, after release. ./rssprobe [n] [P list] [shape]; `./rssprobe time [n] [P] [pairs]` = the
                     # pool trim's refault cost, paired (RSS_TRIM_FORM=hard for MADV_DONTNEED). memprobe counts what is ASKED for: both true
make batchbench      # logos_sort_f64_batch against a loop of logos_sort_f64 over the same segments, paired: ./batchbench [pairs] [total] [P list] [row]
make roundbench      # any per-sort switch paired against the engine as it ships, f64: RB_SWITCH=<NAME> [RB_VALUE=<v>] ./roundbench [pairs] [P]
                     # [n list] [shape filter]; > 1 = shipped is faster; RB_SWITCH unset = a null run (this box's floor for those rows). 23
                     # shapes: six unstructured, twelve structured guards (rings, concatenated logs, blocks, sawtooths incl. one that aliases
                     # the sample's lag, streams, kinks, sparse swaps). Every output checked before timing
make fieldprobe      # the other-fields round (FINDINGS 3bo): ./fieldprobe small | hub | weave | moire | founder | landauer | ab, and
                     # `once` / `seeds` for one named shape (fieldprobe_trace + once names the route). FP_SWITCH=<NAME> [FP_VALUE=<v>]
                     # ./fieldprobe ab [pairs] [n] [P] [filter]: any per-sort switch on the round's 50 shapes (> 1: shipped is faster);
                     # ./fieldprobe i32ab: the same on the int32 API and int-valued doubles (the memory round's radix A/B)
make twinbatch       # ANY compile-time switch paired on DISTINCT inputs, copies swapped: make twinbatch TW_OLD=<flags> TW_NEW=<flags> &&
                     # ./twinbatch_a 1 [pairs] [sizes] && ./twinbatch_b 0 ...; cell t(NEW) / t(OLD), below 1 = NEW faster
make boundprobe      # THE COMPARISON BOUND PER SHAPE (FINDINGS 3br): H_m / H_r / H_rem / H bits per element, ns per bit, growth
                     # 1M -> 16M in adjacent pairs (+20% = comparison-bound) against std::sort's; ./boundprobe [pairs] [n_lo] [n_hi] [filter].
                     # BP_A=NAME=VALUE BP_B=... [BP_THREADS=P] ./boundprobe ab [pairs] [n list] [filter] = two settings, paired, with a
                     # `fired` column (logos_twist_last(36)); boundprobe_trace + `once [n] [filter] [P]` names the route
make xyprobe         # X+Y (FINDINGS 3bs): ./xyprobe [pairs] [n list] = the engine on the N = n^2 sums (raw, sorted rows, shuffled, uniform)
                     # and a sort TOLD X and Y (staircase placement into buckets of S, then the engine), paired, plus the growth 1M -> 16M;
                     # ./xyprobe phases [reps] [n] = the told sort's setup / count / place / bucket sorts, and its buckets against themselves shuffled
make primeprobe      # THE PRIME-FACTOR KNOT SOLVER (FINDINGS 3bv), standalone: ./primeprobe [pairs] [n] [P] [row filter] = what it found per
                     # shape (runs, factors, pinned share, scratch asked for, phases), then t(engine) / t(solver) at 1 thread (factors by
                     # the engine, and by its own merges) and at P (against the engine and against LOGOS_NO_NEARSORTED=1). Re-execs with
                     # OMP_WAIT_POLICY=PASSIVE. PRIMEPROBE_DIAG="<shape>" = the engine alone on that shape's complex factors at 1 / P / P
                     # with the near-sorted route off; PRIMEPROBE_NOGATE=1, PRIMEPROBE_CPLXP=1 = the first cut
make orderprobe      # ORDER MUST NEVER COST: 24 ordered shapes, each against its own values shuffled, paired: ./orderprobe [pairs] [n] [P list]
                     # [filter]; `... ab` = OP_SWITCH against shipped; orderprobe_trace + `once` names the route. Above 1 = the order costs (a defect)
make symprobe        # ISOMORPHS COST THE SAME: negate-and-reverse and exact scalings on 23 shapes; symprobe_trace + `once`. Found the head route's 5x
make edgeprobe       # ONE ELEMENT ACROSS EACH SIZE GATE: ./edgeprobe [pairs] [P list] [shape] [edge]; above 1.10 = a cliff. Found the thin front's,
                     # the quantized radix's and the flash window's
make rleprobe        # runs of equal values: `kv` = the told ceiling with the route's own pair radix, `ab` = LOGOS_NO_RLE against shipped
make tailladder      # the tail merge's forms across tail fractions; make headladder: the head route against itself off
make thinbench       # the thin front's ceiling moved at run time (a -DLOGOS_THIN_HOOK copy), batches of distinct instances, 14 shapes
make bandbatch       # the 2048-65536 window on DISTINCT inputs, two engine copies (BB_OLD / BB_NEW, as bandbench), both orientations
make ordkern XSS=../x86-simd-sort/src   # the vector quicksorts ALONE (in-house and library) on ordered shapes and their shuffles, with depth
make gateprobe       # two engine settings paired on the PRACTICAL COUNTING SHAPES (zip_codes, ages, fk_zipf, prices_cents, 1k-3k-value
                     # columns; batches of distinct inputs below 2M): GP_A / GP_B = "NAME=VALUE" or empty, GP_THREADS=P
                     # ./gateprobe [pairs] [n list] [filter]; above 1 = B faster. make gateprobe_trace + GP_ONCE=1 names the route. FINDINGS 3ca
make argceil XSS=../x86-simd-sort/src   # logos_argsort_f64 and logos_argsort_f64_mt (1 and P threads) beside told ceilings (a key-value
                     # quicksort on (key, index), the value sort, one scan); the mt permutation must EQUAL the radix's. ./argceil [n] [reps] [P] [shape]
make strceil         # told ceilings for two string ideas (FINDINGS 3bz): interning repeated strings (1.18-1.19 on words, 0.76 at 200k) and
                     # strings re-coded two characters to a byte (1.24-1.30 coding free, 0.84-0.88 with it): ./strceil [n] [reps] [shape]
make strsmallbench   # small string calls (2 to 1,000 strings, batches of distinct inputs), a switch against shipped:
                     # SS_SWITCH=<NAME> [SS_VALUE=<v>] ./strsmallbench [pairs] [sizes] [shape filter]
make stringbench XSS=../x86-simd-sort/src   # C strings, 2M, nine shapes (logs8 = eight log files concatenated): logos_sort_strings_mt against std::sort / __gnu_parallel::sort with
                     # strcmp, the old multikey quicksort, and the same prefix scheme on std::sort and on x86-simd-sort's key-value sort.
                     # ./stringbench [n] [reps] [P list] [shape]; STRING_AB=1 = the tie-group policy A/B (LOGOS_STR_SEQBIG). Re-execs with
                     # OMP_WAIT_POLICY=PASSIVE
make pgobench        # profile-guided build of one copy (trained by pgotrain on 14 shapes, 3 to 8M, 1 and 14 threads) paired with a plain copy,
                     # both orientations, combined by tools/pair_orient.py (sqrt(A/B) per row). Measured 0.98-1.03: not adopted
make faultprobe      # every allocation and every pthread_create of a cold call failed in turn (forked children); exit 1 on any wrong output, crash or hang
                     # ./faultprobe 2100000 4 <shape-or-entry> filters; LOGOS_MCGATE_OLD=1 / LOGOS_NO_KNOT=1 reach mc_map / reloc_worker; -DHF_SLACK=0.2 forces spills
make slopebench      # the scaling exponent in adjacent pairs: one 50M sort against 25 sorts of 2M, p = 1 + ln(c_hi/c_lo)/ln(n_hi/n_lo); null: same size both sides
make ipbench         # the strict in-place sort in parallel: against its own 1-thread form, and against the buffered sort at the same thread count
make sanitize        # the whole harness under ASan + UBSan + float-cast-overflow (LOGOS_SKIP_BENCH=1); make tsan: ThreadSanitizer, needs setarch -R
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
`git clone --branch 1.4.0 google/highway` (then `make libvqsort.a`, no cmake needed), `apt install libtbb-dev`. AVX-512 kernels are `#pragma GCC target`-pinned so they
compile under WSL2 where `-march=native` masks AVX-512.

Naming: snake_case functions/vars, `*_t` types, UPPER_SNAKE macros (C convention;
a camelCase revision was tried and withdrawn).
