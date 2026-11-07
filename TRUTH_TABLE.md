# logos-sort — truth table and open avenues

Compiled 2026-09-10 from CLAUDE.md, FINDINGS.md, engines/c/{CHANGELOG,PARITY,README}.md,
experiments/paradigm/*.md, and the engine source (logos_psort.c 3.6.0). Nothing here was
re-measured; every status is a reading of the repo's own record.

**Addendum 2026-11-06.** Section S, directly below the legend, re-grades the headline claims against the
2026-11-05 scale report and scoreboard and says where every avenue in H, J and M stands after the two
months of rounds between (FINDINGS.md 3bd-3bw; engines/c/CHANGELOG.md). The body of A-N is left as it
was compiled, except the factual tables F and G, which are brought current. CLAUDE.md's ledger and its
"positive results" list are the live record; this file grades.

Status legend

| status | meaning |
|---|---|
| **MEASURED** | verified output, calibration-checked, and (where two logos variants are compared) an interleaved A/B in one binary |
| **MEASURED, loaded** | same, but the calibration line flagged machine load; margins ±20–30%, winner pattern stable |
| **FALSE** | implemented and measured to lose; in the negative ledger |
| **INFERRED** | consistent with the measurements but never tested directly |
| **UNTESTED** | no measurement exists in the repo |

---

## S. Status on 2026-11-06: what changed since this table was compiled

Fifty-seven days and some thirty engine rounds separate this section from the body of the table. The reference
machine is now the AVX2 box throughout (i9-13900H, 6 P + 8 E cores, GCC 13.3.0, WSL2, 14 benchmark threads); the
AVX-512 box behind sections A and K has not been re-run. Sources: `reports/scale-report.html` (measured 2026-11-05;
13 numeric and 4 string inputs, 1 and 14 threads; x86-simd-sort AVX2, VQSort AVX2, IPS⁴o at 14 threads,
`__gnu_parallel::sort`, LSD radix, timsort; rows parsed with `bizdev/tools/parse_report.py`), `make scoreboard`
(8.4M, 17 shapes, interleaved, minimum of 7; FINDINGS 3bj and 3bt), and each round's paired A/B. Ratios are read
within a row. The scale report was taken with the box in its usual loaded state, and barrier-heavy rivals lose more
to load than logos does, so its 14-thread ratios are upper bounds (A3b still holds).

### S1. The headline claims, re-graded

| # | claim as of 2026-09-11 | status 2026-11-06 | evidence |
|---|---|---|---|
| A1 | logos 1T beats `std::sort` on every shape | MEASURED, holds | 10M: uniform 8.6×, heavy_tail / bimodal / int_wide 7.4–8.4×, near_sorted 5.5×, few_distinct / quantized / block shapes 21–39×; strings 4.7× (log lines) to 12× (uuids) over `std::sort` with strcmp |
| A2 | logos PT beats `__gnu_parallel::sort` on every shape | MEASURED, holds | every 14-thread row of the scale report from 1M up |
| A3, A3b | IPS⁴o at equal threads | MEASURED 2026-11-05 with IPS⁴o GIVEN 14 threads (before 3bt the scale report let it take all 20): logos faster on 13 of 13 numeric inputs at 3.16M and 10M (1.07–7.15×) and 12 of 13 at 1M; **loses on 5–8 of 13 at 100k–316k** | the thread policy runs one thread below 100,000 elements per thread, so at 100k a one-thread logos meets a 14-thread IPS⁴o. Open: bizdev GAPS 6 |
| A4 | x86-simd-sort 1T beats logos 1T on every shape | **FALSE on the AVX2 box: level on unstructured doubles, ahead on structure** | scoreboard 8.4M: uniform / heavy_tail / bimodal / clustered 0.986–1.047 on the in-house kernel. Scale report 10M: uniform 1.02, bimodal 0.95, heavy_tail 0.88, **int_wide 0.86 (a loss: the int32 radix against a vector quicksort)**, sine_slope 0.92 (0.54 at 1M, the worst numeric row); near_sorted 3.65×, quantized 2.42×, few_distinct 1.45×, block shapes 2.6–3.6× over the best rival (timsort there). AVX-512: unmeasured since 2026-09 |
| A5 | x86-simd-sort MT beats logos at PT on unstructured shapes | FALSE | 10M, 14 threads: uniform 1.51×, heavy_tail 1.78×, int_wide 1.33×, bimodal 1.07× over x86-simd-sort's OpenMP sort, the best rival on those rows |
| A5b | the library leaf under the partition | SUPERSEDED by the hybrid (3be–3bi) | the library's quicksort as the SEQUENTIAL leaf and at up to two threads, optional (`-DLOGOS_XSS_LEAF`). Under the 14-thread partition the ledger's verdict stands, except that the sequential leaf's own policy inside buckets past L2 wins 1.05–2.09× (3bm) |
| A6 | few_distinct and quantized beat x86-simd-sort MT | MEASURED, holds | 14T at 10M: 3.04× and 4.42× over the best rival (IPS⁴o); 1T 1.45× and 2.42× over x86-simd-sort |
| A7 | near_sorted a tie at PT | now a WIN | 14T: 1.92× over IPS⁴o at 10M, 3.47× at 3.16M; 1T 3.65× over x86-simd-sort, at 0.4 bytes per element |
| A8 | logos + SIMD leaf reaches parity with x86-simd-sort 1T, never exceeds it | holds, and the parity is now the in-house kernel's too | 3bi: logos with the library leaf is level with the bare library call (0.991–1.010), checks included; 3bh: the in-house kernel within 1.07–1.12× of the library's |
| A9 | the SIMD leaf is worse than plain logos multi-threaded | PARTLY SUPERSEDED (3bm) | see A5b |
| A10 | the 1T unstructured gap cannot be closed within "produce a sorted array" | the gap CLOSED to parity, not by restructuring the scatter (the ledger's three negatives stand) but in the leaf: the AVX2 quicksort (3g), its bridge to the library's kernel (3bh), the profile phase folded into its first partition (3bi) | |
| A11 | behaviour on AVX2-only hardware | now the reference box; every row above is it | |
| A12 | multi-socket / NUMA | UNTESTED, unchanged | |
| A13 | the pattern at other sizes | MEASURED (scale report, 100k–10M): at 14 threads it holds from 1M up; at one thread the unstructured rows are 0.83–0.91 at 100k–1M and 1.02–1.09 at 3.16M–10M, the structured wins hold at every size | the small end (0 to 10k) is FINDINGS 3ad, 3ba, 3bk, 3bn: the thin front answers calls of up to 2047 elements before the dispatcher |
| A14 | FINDINGS §1 numbers are exact | still FALSE; §1–2 are the September baseline and 3bt the current picture | |
| A15 (new) | Google's VQSort (Highway 1.4.0) | MEASURED 2026-11-05, AVX2 only | the slowest of the three vector sorts here: 1.36–1.42× behind x86-simd-sort on unstructured doubles, behind on every scoreboard row, and it reads no order (a sorted array costs it what random data does: 40× logos) |
| A16 (new) | strings | MEASURED (3bl; scale report) | `logos_sort_strings_mt`: 2.4–24× over `std::sort` with strcmp at one thread; against the same 8-byte-prefix scheme on x86-simd-sort's key-value sort 0.84–1.40 (log lines 2.09× at less scratch, urls 1.40×, uuids 0.96, words 0.84), at 24 bytes per string against 8 |
| E2 | addressing vs vectorized comparison on unstructured data | MEASURED: parity, not a win | the strong form stays false (no route here beats a vector quicksort on random doubles); the weak form is new: level at one thread, ahead at fourteen |
| E5 | distribution-awareness as the product beats SOTA | still INFERRED | the lazy view (`logos_view.h`, `make viewbench`) measures median / rank / top-k 1.5–2.9× over sort-then-look, which is the unfair baseline this row asked to replace; no `querybench` against `nth_element`-class baselines exists |

### S2. What section C's ledger has gained

Section C is the ledger as of 2026-09-11; CLAUDE.md's is the live one, 175 rows on 2026-11-06. The entries that would
change a reader's plan here: non-temporal stores and huge pages both fired and measured zero on the scatter (it is
per-core latency bound), the counting routes' serial merge done in parallel measured 1.02–1.12× SLOWER, the knot
route as a literal linked list 0.40–0.57, every outside proposal that was a merge sort in disguise (the edge sort,
the weld, the Moebius handoff, the DLL sort) 2× to 16,000× behind, counting by hashing past L2 a loss, placing X+Y by
its structure 0.86, a format-aware sort of numeric dates 0.24–0.77. Each row names the mechanism.

### S3. The avenues of H, J and M, as they stand

| avenue | status 2026-11-06 |
|---|---|
| H1 partition-then-count (65k–1M distinct) | not built as such. The band narrowed from two sides: the lattice entry takes lattices of 10k–250k values at 1–4 threads (3br, 1.18–4.01×), and the hash route's gate is Chao1 ≤ 3300 at 8+ threads because its serial merge loses above that (3aa). Counting arbitrary doubles by hashing past L2 measured a LOSS (3br): a lookup into a table past L2 is a miss per element, and the vector quicksort is already on the multiset bound |
| H2 exception extraction for near-sorted data | DONE by other mechanisms: the near-sorted route rebuilt (D28), strays lifted by the run detector (3am), kinks swapped back where they stand (3ai), the short-reach route for bounded displacement (D47), the strand gate (3bw). near_sorted 1T 3.65× over x86-simd-sort |
| H3 histogram-only select / quantiles | PARTLY: the lazy view (`logos_view.h`: at, range, topk, rank, ranks, quantiles, count_in_range; 1.5–2.9× over sort-then-look). The fair `querybench` is still owed (open direction 1) |
| H4 equi-depth bucketing from the sample CDF | DONE in the partition in two forms: the key grid for heavy tails (3ab, 1.3–1.8× at 14T) and the per-mode certificate (3s). Equi-depth LEAF classes measured a ceiling of ~3% and were not built (M1); curvature-placed CDF knots measured neutral |
| H5 bucket-relative key narrowing (int) | built, MIXED, opt-in `-DLOGOS_INT_NARROW`; the in-cache radix for int32-valued doubles measured 1.02–1.03 and was reverted (3bm). int_wide at one thread is the engine's one unstructured loss (0.86) |
| H6 branchless network at the leaf | DONE at the small end: Batcher networks for 4–16 elements, 1.03× at n = 4 to 1.58× at n = 16 (3ax); the 64-element AVX2 base case now reads and writes the range directly (3bh). On big buckets the lever measured mixed (line repair, opt-in) |
| H7 one global decision, one fused pass | PARTLY: the top histogram pass no longer EXISTS (the histogram-free partition, 3m, 1.14–1.45×), and the leaf's profile phase rides on the kernel's first partition (3bi, 1.09–1.14× at 1T). A fused two-level scatter was not built; hub-and-spoke at one thread (one L2-sized scatter, then the kernel) measured 0.58–0.91 (3bo) |
| H8 huge pages | built, fired (98 MB AnonHugePages), NEUTRAL 0.97–1.02: the scatter's TLB misses follow the burst size, which write-combining had already fixed |
| H9 structure router | BUILT for one and two threads as the hybrid (`-DLOGOS_XSS_LEAF`, 3be–3bi), off by default (a dependency); at one thread the in-house engine is itself level with the library, so the router is the shipped engine. From four threads up: the partition, where the library's parallel quicksort loses on structured buckets |
| H10 direct key-payload pair sort | NOT built; the largest gap a buyer meets (bizdev GAPS 4). argsort + gather measured 8.5× over `std::sort` on ISO date text (3az) |
| J1 NaN detection in the prescan | DONE (3bi: one AVX2 profile pass whose verdict the kernel trusts; the kernel's first partition carries the NaN / -0.0 flag) |
| J2 per-bucket statistics from the histogram | moot: the histogram is gone (3m). The leaf's own profile is one vector pass over a 64-element prefix plus founder windows (3bi, 3bo) |
| J3 non-temporal stores | the scatter: NEUTRAL. The counting routes' EMIT, which only writes: 1.21–1.36× at one thread, shipped (3bj) |
| J4 reuse the top sample in the buckets | not measured as such; the one shared sample answers every dispatcher gate and the partition reuses it (3n) |
| J5 relocation | DONE; the knot route (3ac), the twist (3ag) and the vector merges (3bc, 3bo) built on it |
| J6 bounded displacement | DONE by the short-reach route (D47) |
| J7 heavy-hitter split | the one-hitter case is DONE: the dominant-value route (3bp: 2.7–9.9× at 14T at shares 0.25–0.9, 2.1× at one thread at 0.9). Many hitters still meet the hash route's gate |
| J8 learned placement with a spill bucket | not built as a leaf; the histogram-free partition IS a single-pass predicted placement at bucket granularity with a spill list (3m) |
| J9 lazy bucket materialisation | DONE: the view (`logos_view.h`) |
| J10 parallel strict in-place | DONE (3ae: 2.5–2.85× over its own one-thread form; still 6–24× behind the buffered sort) |
| J11 f32 / i64 engines | f32 NARROWING inside the f64 path at one thread (3bk: 1.20–1.68× when every value is exactly a float); no f32 or i64 API |
| J12 warm-start across calls | not built; the persistent worker pool and context bundles are the warm part (3bk, 3o) |
| J13 sort-free distinct / group-by | not built |
| J14, J15, J16 | DONE (H0b; the trial-and-commit tables; 12 buckets per thread) |
| M6 quorum trial | not built |
| M8 ping-pong winnow in the AVX2 leaf | superseded by the bridge (3bh): the kernel's base case and pivot were what the gap was made of |
| M9 plan cache | not built (J12) |
| M10 incommensurate class widths | not built; the low-discrepancy probe's coverage band (3bo) took that shape elsewhere |

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
| B57 | **Relocation's parallel win is gone, and nobody had looked.** J5 measured 1.9–2.2× at 20 threads on 2026-09-10. Against today's partition it is 0.95 on block_perm64 and 0.92 on block_overlap at 14 threads, and 4.4–4.5× at one. J5 did not change; the partition got 2–3× faster (D15, D16, D32) and the comparison was never re-run. The knot route ties there for the same reason: scan + place + copy back is the same memory traffic as a partition, and at 14 threads traffic is the cost | MEASURED (two engine copies, this engine vs `-DLOGOS_NO_RELOCATE`, both orientations, layout cancelled, 8.4M, 30 pairs; FINDINGS §3ac) | not acted on: gating the relocation family to sequential sorts needs its own A/B across sizes. Re-measure old parallel wins against the current partition before building on them |
| B58 | **A probing route's decline is not free, so a cautious gate charges for what it refuses.** The knot route's first gate (J5's n/2, from a predicted break-even) made the staircases just past it read 0.92–0.96, because the decline comes after the whole array has been scanned for runs; lifted, the same shapes win 1.2–2.9×. And a parallel route measured only against the shipped engine inherits a sibling's wasted probe in its baseline: 1.2–1.5× that way, a tie against the pure partition | MEASURED (`make knotbench`, default and `lift`; FINDINGS §3ac) | lift the gate, measure the ladder, then set the gate; compare against a copy with the route's whole family compiled out |
| B59 | **`inf * 0 = NaN` fails every compare, so a NaN-blind certificate certifies.** With an infinite sample extreme the uniformity certificate measured a distance of 0 and certified the data, `isfinite` passed a splitter-table scale of `cells / inf = 0`, and flash and coarse buckets cast NaN to a class: uniform doubles with `a[0] = -inf` at 4 threads crashed the committed engine in `hf_scatter` (position 0 is always a sample point), and 8 of 10 shapes crashed or came back unsorted | MEASURED and FIXED (720 sweep checks clean under ASan, UBSan and `float-cast-overflow`, which `undefined` does not include; 90 in the harness; whole change vs the committed engine 0.987–1.021, layout cancelled) | no test had ever put an infinity in front of a parallel sort; found by the knot harness's infinite-endpoint shape |
| B60 | **Below about a hundred elements logos loses to std::sort, and between 25 and 76 it pays for a plan it never uses.** One thread, logos / std::sort: 9× at n = 3–5, 2× at 10–32, ahead for good from n ≥ 100 on the random shapes, 316 on few_distinct and the block shapes, 1000 on near_sorted. A call costs 8 ns at n = 0 and 48 ns at n = 3 (std::sort 0.3 and 5.3). From 25 to 76 elements it detects runs of two or three elements, builds a relocation plan, has it declined and only then sorts: 25–36 ns per element against 18 at n = 24 and 10 at n = 96 | MEASURED (`make scale-full`, batches of distinct inputs below 10k; `bump_probe` for the step, whose two edges were predicted; FINDINGS §3ad) | open: a minimum average run length before run detection's plan, the lever the knot route needed (C30) |
| B61 | **A bench ladder that starts at a million cannot see a cost per call.** Two regressions of the knot route sat in the tree for a day under 2059 green checks and a 40-pair A/B at 1M, 3M and 8.4M: +40 ns on every sort, and 3× at n = 32. The harness checks answers, not times; every A/B bench here starts at 1M | MEASURED (found by the scale report's new sizes, confirmed in one binary against the committed engine; FINDINGS §3ad) | the scale report now runs 0..10M; read its small end after any change to the dispatcher's first page |
| B62 | **Nothing here had ever weighed a sort, and the number in the header was wrong by 1.7–2.6×.** Peak traced heap of one cold call, on top of the input: 15.5 bytes per element at 1 thread and 23 at 14, where the header said "~9n" (int32: 11.5 and 6.5 per 4-byte element, where it said "~5n"). At 14 threads it had been 34.9, because the histogram-free partition allocated its 12.8-byte regions twice: glibc never returns a 64-byte-aligned block from mmap, so the "retry aligned" branch ran every time | MEASURED (`make memprobe`: malloc family interposed, one cold call per forked child; FINDINGS §3ae) | the regions are asked for once, aligned; next: take the 8+1 byte scratch only when a route asks for it (23 → about 14) |
| B63 | **Per-thread scratch sized by the LARGEST bucket is O(P·n) the moment one bucket cannot be divided.** One value holding 90% of 10M elements cost 104.5 bytes per element at 14 threads, a gigabyte for an 80 MB array, and `logos_release_buffers()` gave back 9 of them. The buckets in hand at one moment are disjoint, so they sum to at most n: size contexts for a typical bucket and let the oversized one borrow | MEASURED (`make memprobe`: 104.5 → 17.8, 0 left after release; speed neutral: 0.98–1.06 against the committed engine at 8M in two paired runs, layout cancelled) | `ctx_typical`, `ctx_fit` / `ctx_unfit`, in both partitions and the view's bucket sorter |
| B64 | **Every condition the harness had never produced found a defect.** An infinity (a crash), subnormals (unsorted output), a NaN payload (rewritten), a failed allocation (unsorted output, crashes), a failed thread creation (wrong output, hangs). 2,547 checks were green throughout, because all of them ran the engine on finite, normal numbers with every resource available; and the in-place entries, which are also the fallback above 2³² − 1 elements, had no check at all. A fallback that has never executed is a guess | MEASURED (`make faultprobe`, the harness's IEEE, in-place and resource sections; FINDINGS §3ac, §3ae) | the harness now produces all of them on every `make test` |
| B65 | **On Linux the resource that runs out is threads, not memory.** A pid limit (any container that sets one) makes `pthread_create` return EAGAIN while malloc still succeeds. None of 23 creation sites read the result: the partition hung on a barrier waiting for a participant that did not exist, and the counting, lattice, near-sorted, relocation and sorted-prefix routes returned WRONG OUTPUT with no error, having joined a garbage handle and read results nobody wrote | MEASURED (`make faultprobe`: 154 of 186 injected creation failures ended badly, 45 of them in wrong output; 0 of 186 after) | `par_run` (the orphan runs on the caller), `par_spawn` + `barrier_drop` (the orphan leaves the barrier), tiles covered by the caller in relocation and knot, decline in the int32 counting route |
| B66 | **A forked probe and an in-process one find different bugs.** The sequential path freed a stack context that was never initialised when the scratch buffer failed first: every forked one-thread run passed (a fresh child's deep stack is zeros), and restored in a scratch copy the same code aborts the in-process harness (`munmap_chunk(): invalid pointer`). The forked form is the only one that can report a hang or a crash as a RESULT; the in-process form is the only one whose memory has a history | MEASURED (both directions, 2026-09-20) | both are kept: `make faultprobe` and the harness's resource section |
| B67 | **The scaling exponent has to be measured in pairs here, and at 14 threads it is not one number.** One 50M sort against twenty-five 2M sorts, adjacent, order alternating (null: ±2%; `std::sort` reads p = 1.063–1.066 against the comparison-count floor 1.062). One thread: logos grows 13–19% per element where `std::sort` grows 21–24%, and is exactly linear on the counting and relocation routes. Fourteen threads: uniform +26–34% (2M doubles fit the 24 MB last-level cache, 50M do not) while the structured routes get 31–60% CHEAPER, so at 2M a parallel call on them is mostly fixed cost | MEASURED (`make slopebench`, two full runs; one-thread cells reproduce within ~3 points, 14-thread cells within 10–17; one cell does not reproduce and is excluded; FINDINGS §3ae) | spawning and joining 13 empty workers costs 0.77 ms here, so a two-phase route pays 1.5 ms of a 4.6 ms call at 2M: a persistent worker pool inside `par_run` / `par_spawn` is the lever, not yet built |
| B68 | **The same overflow is a wrong answer in C, a quadratic stall in JavaScript and nothing in Rust.** A subnormal-wide range makes `classes / range` infinite. C cast the resulting NaN to an index: unsorted output. JavaScript's `x * Infinity \| 0` is 0: every element in bucket 0 and an O(n²) insertion pass, 77 s for 300k elements, output correct. Rust's casts saturate and a concentration check bails: fine. What a defect looks like is a property of the language's conversion rules, not of the algorithm | MEASURED (the C harness's IEEE section; `engines/js/test/perf-guard.js`; `engines/rust/tests/edge_values.rs`) | run an edge input found in one engine on the others: it cost an hour and found a second, unrelated JS cliff (a dense cluster in one flash bucket, 13–21 s) |
| B69 | **'Order unspecified' is not a licence to not return.** The gap-split partition scanned with `a[i] < pivot` and `a[j] >= pivot`; a NaN fails both, and when the scans met on one the loop never ended. In C the meeting point depends on the order inside a bucket, hence on thread timing: about one harness run in fifteen, about one sort in thirty on heavy-tailed data with 1% NaN at 4 threads. In JavaScript it is deterministic: `[lows..., NaN, highs...]` with 6,000 elements hangs `sort()` forever, in the published package | MEASURED (a looping hunter that signals the spinning thread for its backtrace; the harness's `NaN termination` section fails at repetition 75 against the old loop; `engines/js/test/perf-guard.js`) | one predicate, `x < pivot`, for both scans. The NaN floor now reads: the call returns, no out-of-bounds access, a permutation of the input's bits |
| B70 | **When every element sits near its final place the scatter is unnecessary.** Index blocks of 32768 sorted where they lie, then only the seams: the elements of L above R[0] and of R below the last of L are merged in place, and what crosses a seam rightward is matched by what crosses leftward. 8.4M: 1.9–2.5× at one thread, 1.3–1.5× at 14 on four bounded-displacement shapes; 30M: 2.1–2.9× / 1.5–1.8×; 1M: 1.8–2.2× / 1.25–1.4×. It weighs 0.10 bytes per element at one thread and 1.33 at 14 (10M), against 15.5 and 23.2 | MEASURED (`make reachbench`, paired, one binary; `make memprobe`) | the first route since the counting ones that asks for no n-sized buffer |
| B71 | **On locally ordered data a flash sort is the in-cache kernel, not the quicksort.** On 32k blocks of bounded-displacement data the flash sort beats the AVX2 quicksort at ONE thread by 1.26–1.81× (a tie on 2048-element shuffled windows): classification is nearly the identity, so count, scatter and insertion pass stay where the cache already is. Disorder of reach 2–3 is an insertion sort's instead (0.81 → 1.10 at one thread) | MEASURED (`RB_OFF=LOGOS_REACH_LEAF=1 RB_ON=LOGOS_REACH_LEAF=2 ./reachbench`, route on both sides) | the leaf policy "AVX2 when sequential" is right for UNSTRUCTURED buckets only |
| B72 | **The 40-pair nearly-sorted probe bounds inversions at one stride, n/40.** Disorder that is dense but shorter than the stride passes it and the momentum insertion sort then spends its whole 8n budget before it gives up: in every partition leaf, in the sequential dispatcher and in the JavaScript engine (where the small case had NO budget: 45,000 elements with 1% swaps took 47 ms against 2.3). 64 strided pairs EIGHT apart separate the cases, because the budget is a mean displacement of 8 | MEASURED (C: old leaf / new, 14 threads, shapes the short-reach route declines: 1.36 / 1.70 / 1.87; regime_switch 1.17 at both thread counts; with the route off, 64-element windows 384 → 160 ms at one thread. JS: 1.6–19× from 20k to 300k, both module load orders) | guards 0.96–1.08 on 24 rows on a day with a 6% floor; 100-pair rows 0.99–1.06 |
| B73 | **How many strays are worth putting back depends on the thread count.** The shift pass that returns strays is serial, ~0.7 ns per element at most; the route saves ~7 ns per element at one thread and ~0.4 at fourteen. Up to four threads strays are simply put back (1.2–2.3× at one thread with strays in every block, 1.15–1.3× at four); wider sorts ask eight probe blocks first and decline when two hold strays (0.96–1.04 at 14 threads) | MEASURED (`make reachbench`: strays_*, zeros_*, sentinel_zeros, late_84, *_strays rows at 1, 4, 7 and 14 threads) | a rule that stopped the block phase when strays kept turning up was dominated and removed: C42 |
| B74 | **A ceiling measures the baseline's weakness too.** Chunk relocation told the segments beat the engine 2.0× at one thread on three value bands, predicted 1.2–1.5×; the engine runs that shape at twice its uniform time at both thread counts (the bands defeat the uniformity certificates and the bits-coarse leaf) | MEASURED (`make reachhunt`, regime_switch) | the margin belongs to the weakness, which is the better thing to fix; the method is not built |
| B88 | **A galloping two-strand merge recovers the whole ceiling on a sparse strand over dense blocks**: eight branch-free steps while the strands alternate, a gallop and one bulk copy when one strand wins eight ahead, only in crossings whose pieces differ fourfold. `LOGOS_NO_KNOT_GALLOP=1` against it, one binary, 8.4M: 1 in 65 **2.45x**, descending **2.56x**, 1 in 513 and 1 in 4097 **2.70x** at one thread, **1.18-1.31x** at 14; 1 in 9 1.03 / 0.99; staircases and guards 0.97-1.03 | MEASURED (`./knotbench 60 8388608 gallop sparse`; ceiling first, `... ceiling`: 2.42-2.75x / 1.03-1.16x) | same output bytes as the branch-free merge (666 harness checks, byte by byte); predictions (1.5-2.3x, 1.00-1.08x) were low |
| B89 | **The dense-integer bitmap beats the leaf far past its old budget**: distinct integers over s x n, one thread, bitmap / leaf (the int32 radix): 5.0x at 6n, 4.3x at 8n, 3.9x at 12n, 3.3x at 16n, 2.9x at 24n, 1.2-1.7x from 32n to 64n, a tie near 96n. The budget went 4n -> 16n: **3.7-5.0x at 8.4M and 6.0-8.7x at 1M** on spans 6n to 16n against the old budget, everything else 0.98-1.05, one planted repeat 0.88 / 0.94 | MEASURED (`./mcbench 40 8388608 span` for the ladder, `... span - 4` for the A/B, one binary) | a wider gate lets in data the bitmap must give back, and each freshly touched page is a fault: at 64n the guards fall to 0.73. The rule: the largest budget where clean spreads win 1.5x and no guard drops under 0.85 |
| B90 | **A ring buffer read from the wrong slot is one strand across the wrap, and the engine now reads it that way** (R0 on the sphere): the prescan's ascending prefix plus an ascending suffix with a[n-1] <= a[0], rotated to the front once. Ring buffer **1.56-1.63x at one thread, 1.90-2.03x at 14**; rotated prefix_95 + tail 1.73-1.75x / 1.46-1.51x; 30% prefix + noise + 40% suffix 2.07-2.14x at one thread; two sorted blocks swapped 1.56-1.58x / 1.78-1.90x; guards 0.97-1.02; no per-call cost at the small end (`make callbench`, after a hinted first fix that cost sorted calls of 2-32 elements 10-16% was replaced) | MEASURED (`./unknotprobe 40 8388608 ab LOOP`, three runs, one binary) | the gate is exact and costs two element reads on anything that ascends overall. Before it the engine paid 2.0x (1T) and 2.6x (14T) over the sorted array, because the tail route merged the second run in element by element |
| B91 | **At one thread the relocation detector's floor was the run merge's, and it declined every array of disjoint blocks averaging 256-4095 elements**: 10.5-12.5 ns per element where blocks of 4096 took 2.0-2.4. Floor 256 at one thread (the knot route's own minimum): blocks of 1000 4.2x, staircase of 2000 3.6x, keyed runs of 3000 5.6x, rotated keyed runs of 4000 7.5x, guards 0.98-1.01; at 14 threads unchanged (every lower floor mixed) | MEASURED (`./unknotprobe 20 8388608 cliff 256` ladder; `... ab FLOOR`, two runs) | runs of EXACTLY the floor pass by nothing: the grouped shape passed as generated and failed rotated by 12 elements. A constant swept for one route is not tuned for the routes that inherit it |
| B92 | **Local Reidemeister moves on a ring of strands stall only where overlapping strands are shuffled apart, and a cut at a gap is what makes them affordable**: one hard state in sixteen shapes (the shuffled staircase); the swing took nodes walked per element 21.5 -> 0.00 on near_sorted, 4.8 -> 1.0 on prefix + tail, 4.0 -> 0 on block_perm; plane against sphere 0.96-1.09 inside the unknotter | MEASURED (`./unknotprobe 4 1048576 count`, `... moves`) | the paper's shape exactly: a hard diagram gets more complex before it gets simpler (a cut adds a strand and removes every false crossing), and the collapse is seen globally (tangles by value), not by local moves |
| B93 | **Strays inside strands are lifted by the run detector and merged back in place of the copy-back**: blocks + 0.1% strays **4.05 / 4.01x**, + 1% **3.26 / 3.00x** at one thread; + 3% (past the n/64 cap) 1.00 / 0.99; guards 0.99-1.01 | MEASURED (`./unknotprobe 40 8388608 ab STRAY`, two runs after the reworks) | five forms, the last at a run's head; the list taken at the first stray; a density exit; a NaN never lifted. Sequential sorts only |
| B94 | **A wave with drift is sorted piece by piece and placed by the knot sweep**: 32 cycles swing 0.6 noise 50 **1.24 / 1.21x**, 8 cycles 1.13 / 1.11x, 16 cycles noise 20 1.12 / 1.12x at one thread; swings of one and two periods declined by the sample's crossing cost (1.00-1.01); guards 0.99-1.01 | MEASURED (`... ab WAVE`, two runs) | the told ceiling was 2.1-3.7x; the route recovers a third to a half of it |
| B95 | **A noisy ring buffer is seen in the reach route's block medians before any block is sorted**: rotated short reach **1.12-1.16x** at one thread over four runs, guards 0.98-1.03 | MEASURED (`... ab SEAM`) | the medians ascend everywhere but at one block, where they drop by most of the range |
| B96 | **The copy-back pass is worth 1.21-1.35x on block shapes** to a route that could return a sorted description: block_perm 1.33 / 1.35, stairs x20 1.21 / 1.33 (one thread / 14) | MEASURED (`./unknotprobe 40 8388608 nocopy`, a ceiling with wrong output by design) | the sorted view's direction; not a sort |
| B97 | **The LIS fraction of the position-ordered sample separates one strand with strays from many strands**: 1.00 sorted and short reach, 0.95-0.98 prefix and near-sorted, 0.21-0.26 blocks, stairs and streams, 0.04 noise; cyclic 1.00 on a ring buffer | MEASURED (`... lis`, 10 of 11 inside predictions) | blind to strays inside blocks; not wired |
| B98 | **A single run left by the detector is finished, not declined**: one sorted strand with 1% adjacent swaps **5.47 / 5.45x** at one thread (it was 0.90 against std::sort, 0.11 against a reverse-and-bypass merge sort); guards 0.99-1.01 | MEASURED (`./tsbench 40 8388608 fixes`, two runs) | sorted (or its mirror) when no strays were lifted; the strays' merge places it otherwise |
| B99 | **At P threads the seam declines the short-reach route before its block phase**: noisy ring buffer **1.34 / 1.30x** at 14 threads, guards 0.99-1.02 | MEASURED (`... fixes`) | one thread still rotates (1.11-1.13x) |
| B100 | **On server timestamps the engine beats std::sort 3.3-6.0x at one thread on every latency shape, 16x on overlapping batches, 6.4x on a late batch**, and the order-reading routes are 1.4-1.9x of that (5x on kinks, batches and late batches) | MEASURED (`./tsbench 20 8388608 all`, three runs) | weak rows: millisecond clocks 1.6x (no counting route); at 14 threads the stray-bearing shapes gain nothing from the order routes |
| B101 | **The dispatcher's prescan reads sixteen at a time once the direction is known**: sorted **2.21-2.22x at one thread, 2.23-2.30x at 14**, sorted then reversed 1.40-1.41x, a 95% prefix + late batch 1.30-1.44x; guards 0.99-1.06 | MEASURED (`./tsbench 40 8388608 prescan`, two runs) | from 256 elements; `logos_prescan_agree` asserts it matches the scalar line on 22,392 harness rows |
| B102 | **A tie continues a descending run** in the relocation detector: 64 shuffled blocks, half descending, with 1.25% ties **6.00-6.10x at one thread, 1.32-1.35x at 14**; guards 0.98-1.03 (two single P-thread dips checked at 120 pairs against a null switch: noise) | MEASURED (`... ties`, `... switch`) | nothing here is stable; the leaf's `detect_runs_limited` still ends descending runs at ties |
| B103 | **One strand's strays go back by one memmove per segment**: kinked strand **2.36-2.37x at one thread**, 0.1% far strays 1.74-1.78x; guards 0.98-1.06 | MEASURED (`... onefix`) | the finish was 20.7 of 34 ms in three full passes |
| B104 | **The dispatcher's frame kept out of the routes it calls**: `nearsorted_parallel_f64` (33 KB) and `streams_try_f64` (13 KB) `noinline`; the frame 60,496 -> 28,256 bytes, a sorted call of 2 elements **84.7 -> 18.6-20.0 ns**, random calls of 2-5 elements level with the committed engine | MEASURED (`make callbench` against the committed engine, two runs; `-fstack-usage`) | GCC had begun inlining the near-sorted route after round six with no stack array added; sorted calls of 2-10 still 1.11-1.30 against the committed engine, not traced |
| B105 | **The dispatcher's prescan compares with vectors** (AVX2 blocks of 32 where the CPU has AVX2 and FMA, SSE2 blocks of 16 otherwise): sorted **1.72-1.81x at one thread and 1.67-1.79x at 14 at 8.4M, 2.06-2.23x at 1M**, a late 5% batch 1.09-1.18x; guards 0.98-1.03; AVX2 over SSE2 1.23-1.27x | MEASURED (`TS_SWITCH=LOGOS_PRESCAN_OR16 ./tsbench 40 8388608 switch`, `... LOGOS_PRESCAN_SSE2`, two runs) | the old block was commented as vectorizable and compiled to sixteen scalar compares at the engine's flags; `logos_prescan_agree` checks every kind |
| B106 | **The run detector jumps to a wrong-way step** instead of walking eight scalar steps after a failed block: one strand with 1% kinks **1.05-1.08x at one thread** (1.06 [1.00 1.13] at 120 pairs, null 1.00); guards 0.97-1.05 | MEASURED (`TS_SWITCH=LOGOS_FASTSCAN_HOLD8 ...`, two runs + 120 pairs) | the run list is identical by construction; the harness compares kinks and twisted strands across the shipped scan, the scalar scan and the old hold |
| B107 | **The run detector's block of eight compares with vectors** (SSE2 two at a time, AVX2 four, advancing over as many blocks as pass): one kinked strand **1.30-1.32x at one thread**, 64 shuffled blocks half descending **1.27-1.30x** at one thread and 1.09-1.16x at 14, 64 batches 1.06-1.12x; guards 0.97-1.02 | MEASURED (`TS_SWITCH=LOGOS_SCANBLOCK_SCALAR ./tsbench 40 8388608 switch`, two runs) | the run list is identical by construction (the scalar line is unchanged); the harness compares kinks and twisted strands across four variants |
| B108 | **Where the parallel speedup stops, per route** (8M, 14 threads): the partition 12.1 of 14 threads busy and e = 0.055-0.09; the counting and lattice routes 6.0-6.9 busy with NONE waiting (a serial merge that grows with P); relocation 6.6 busy and **3.6 waiting at a barrier** (static tiles); kinked data fastest on ONE thread at every P | MEASURED (`make profile`, `make amprobe`) | the diagnosis; the two cheap remedies were rejected (C77) |
| B109 | **Each lattice worker allocates its own count table** instead of the caller allocating all P before the spawn (5.6 MB of serial memset at 8M and 14 threads): quantized and mod_card_30k **0.94-0.99 over 20 of 20 cells** (60 pairs, two runs, 1M-16M) against a null switch at 0.98-1.04; the map phase 8.8-11.3 ms against 10.9-13.8 in the trace | MEASURED (`CAP_SWITCH=LOGOS_LAT_SERIAL_TABLES ./capbench 60`) | ~1.03x, SHIPPED; the prediction (0.83-0.95) was twice the measured effect |
| B110 | **The lattice MAP phase claims chunks from a counter instead of taking equal slices** (`LOGOS_LAT_STATIC_MAP=1` restores the slices): quantized and mod_card_30k **0.90-0.97 on 12 of 12 cells at 4M and up**, two runs of 60 pairs, against a NULL switch on the same rows reading 0.97-1.04. The mechanism was kill-tested FIRST from profile data already on disk: per-thread busy inside the statically chunked phases is bimodal (1.39-2.09x) where the queue-driven leaf reads 1.04 -- 6 performance cores and 8 efficient ones | MEASURED (`CAP_SWITCH=LOGOS_LAT_STATIC_MAP ./capbench 60`) | ~1.05x, SHIPPED; predicted 0.75-0.90, so again about twice the measured effect |
| B111 | **Branchless Batcher networks for 4 to 16 elements in the leaf** (`gen_small_net.py`, off with `LOGOS_NO_SMALLNET=1`): random calls **1.03-1.06x at n = 4 rising monotonically to 1.57-1.58x at n = 16**, two runs, both copy orientations; n = 18 and 24 read 0.99 (the control), sorted rows 0.97-1.04 at every size, and 1000-50000 read 0.98-1.02 because a large sort does not use the scalar quicksort base case. Each network is proved by the 0-1 principle in the generator; the assembly has 424 compare-exchanges and ONE branch | MEASURED (`make callbench`) | SHIPPED, a SMALL-END result (direction 0g), not a result at scale. Predicted 1.5-3x across the range: right at n = 16, wrong at n = 4 |
| B86 | **A kink (one adjacent pair swapped inside a run that otherwise goes on) is swapped back where it stands by the run detector**, when both neighbours agree; the strand stays contiguous, so the knot plan's searches and offsets hold. `LOGOS_NO_KINK=1` against it, one binary, 8.4M: blocks **5.81 / 4.98 / 3.22x at 0.1 / 1 / 4.8% kinks at one thread, 1.52 / 1.28 / 1.08 at 14**; staircases 4.24 / 3.83 / 2.67 and 1.35 / 1.24 / 1.02; the same with every other block twisted; guards 0.98-1.01 over three runs | MEASURED (`./strandprobe 40 8388608 kinkab`; ceiling first, `... kinks`: 4.0-5.6x / 1.15-1.44x) | the cheap subset of strays inside strands (B85): the misplaced element sits next to its hole. The detector writes again, two elements per kink and nothing else |
| B87 | **The AVX2 quicksort is 1.17x slower on a noisy sloped wave than on the same values shuffled; the scalar leaves are not.** With `LOGOS_NO_AVX2=1` the wave reads 1.05 against uniform and its shuffle 1.05; with the AVX2 leaf on, 1.19 and 1.01 | MEASURED (`./sineprobe 30 4194304`, with and without `LOGOS_NO_AVX2=1`; each cell paired against a uniform reference) | not traced. Suspect: the pivot is the median of 63 STRIDED samples, and 32 cycles against 63 strides alternates crest and trough. Replaces the diagnosis in B77 / B78 (C58) |
| B81 | **The run detector scans eight elements at a time, and finds the same runs.** A bound (the largest step times the largest count the block can reach, against the gap limit for the smallest range it can have) proves the scalar test would accept all eight; otherwise the scalar line runs, unchanged. 8.4M, `LOGOS_NO_FASTSCAN=1` against it in one binary: blocks **1.43-1.49 at 1 thread, 1.16-1.22 at 14**; x20 staircases 1.33-1.36 / 1.18-1.19; x80 1.19-1.20 / 1.16-1.17; uniform 1.00 / 0.98 | MEASURED (`./twistbench 60 8388608 scan`; the profile first: the scan was 47-49% of the run routes at one thread) | possible only because the twist (B79) left the detector read-only. The 14-thread prediction (1.05-1.15) was low |
| B82 | **Relocation and the knot route beat the PURE partition at 14 threads again**: block_perm **1.077** (0.95 in §3ac), stairs_x20 **1.165** (a tie of 0.96-1.11); 5.77x and 4.74x at one thread (4.4-4.5) | MEASURED (an engine copy built `-DLOGOS_NO_RELOCATE -DLOGOS_NO_KNOT` against the shipped one, both orientations, sqrt(A/B), 60 pairs, 8.4M; one run) | the margin is B81: one pass of the route no longer costs full price. Open direction 0f's "re-measure before building on it" is done |
| B83 | **One orientation bit for the whole array**: a sequential sort whose MIRROR image passes a directional gate (descending suffix of half the array; the reach ladder turned round; streams descending in arrival order) is reversed once, behind relocation, and re-dispatched. `LOGOS_NO_MIRROR=1` against it, one binary, 8.4M: mirrored short reach **2.07x**, mirrored 95% prefix **2.20x**, mirrored 4 streams **2.76-2.81x**; every guard 0.98-1.02 | MEASURED (`./mirrorprobe 40 8388608 ab`, three cells again at 120 pairs); ceilings B80 | sequential sorts only (at 14 threads the pass eats the gain); mirrored near_sorted has no gate to ask (ceiling 1.2). The prefix prediction (2.3-2.5) was low: the suffix scan is a read the ceiling never paid |
| B84 | **Persistence separates strands from noise where the inversion rate cannot.** From 2,048 strided points, p = the share of consecutive pair directions that agree: noise 0.33, every stranded shape 0.95-1.00 in either direction or both; a triangle wave, a sloped sine and every-other-block-twisted read writhe 0.00-0.05, like noise, and p 0.97-0.98. 16 of 16 shapes classed as predicted, rule fixed in advance | MEASURED (`./strandprobe 0 8388608 writhe`); NOT wired in | it cannot tell mirrored blocks from a mirrored ribbon (same w, p and turns), and a period that divides the stride is a blind spot. It is the gate that ribbons (TWIST_AND_KNOT avenue 1) would need |
| B85 | **Sorted blocks with 1% late records sort 2.0-2.2x faster at one thread when TOLD which positions are strays** (0.1%: 1.6-1.7; 5%: 1.9-2.0), through a told sort whose own overhead is 0.61-0.65 on clean blocks (two serial passes a real route would not make) | MEASURED as a ceiling (`./strandprobe 30 8388608 strays`); NOT BUILT | predicted 3-4x, low by half for that reason; the real ceiling is nearer 3x. At 14 threads the told sort is no ceiling (0.19-0.31). Open direction 0l |
| B80 | **Every cheap route reads order in ONE direction, and the mirror image of its input pays for a full sort.** A shape against its own mirror, 8.4M, paired, two runs: short reach **2.02-2.11 at one thread, 1.41-1.44 at 14**; 95% sorted prefix + tail **2.71-2.74** at one; four interleaved streams **3.10-3.30** at one; near_sorted with 1% swaps only 1.2. One reversal pass in front keeps 1.9-3.1x at one thread and costs 5-10% on a false positive (uniform 0.95-0.99 / 0.90); mirrored block shapes read 0.98-1.02, because the twist (B79) already reads them as they lie | MEASURED as a ceiling (`make mirrorprobe`); NOT BUILT | the bit is in the shared sample already (`inv_all` 0.87-1.00 on these mirrors, 0.50 on noise, but 0.90 on mirrored blocks: so gate on the directional gates run over the MIRRORED sample, behind relocation). Open direction 0k |
| B79 | **The Moebius twist: a run that lies descending is consumed as it lies.** One orientation bit in the run descriptor, a backward copy for a lone piece, a two-ended merge where strands cross, knot cuts in logical coordinates; the detector no longer writes to the array. 8.4M, `LOGOS_NO_TWIST=1` against the twist in ONE binary: every block twisted **1.149 at 1 thread, 1.114 at 14**; blocks of 8k 1.144 / 1.094; staircase x20 1.108 / 1.093; every-other rows 1.056-1.068 / 1.015-1.044; x80 staircases tie at one thread (1.01-1.02); three guards 0.984-1.026 | MEASURED (`./twistbench 120 8388608 ab`; ceiling first, `... ceiling`: 1.07-1.17 / 1.02-1.12, of which it recovers 84-97% at one thread) | the ceiling bench is the reusable part: Z against Z' with the pass already done prices a pass exactly. The leaf's `detect_runs_limited` still reverses; its ceiling is unmeasured |
| B77 | **A sine wave with a slope costs 1.22-1.31x a sort of random data at ONE thread, and the control is the whole finding.** Noise of +-50 elements on a 32-cycle wave; the SAME noise on a plain slope reads 0.59-0.63 (the short-reach route takes it), so the cost belongs to the wave, not the noise. It tracks the REACH rather than the swing (reach 104 → 1.27, 23 → 1.09, 6 → 1.05) and is gone at 14 threads (1.03-1.04). The bench that first claimed it compared absolute times ACROSS ROWS of one run, which rule 2 forbids; rewritten as a paired comparison against a uniform reference with a null, the claim survived, but that was luck | MEASURED (`make sineprobe`, 40 pairs, three runs, null 0.99-1.00) | bounds a composed route (trend + season + residual) at 2.1-3.7x at one thread, nothing at 14; and a route's own A/B ratio is NOT the same quantity as shape-against-random, which is how two of five predictions went wrong |
| B78 | **Partial order can be worse than none: this array is slower than the same numbers SHUFFLED.** The same values with the index order destroyed read 1.01 against uniform where the natural order reads 1.27, so the cost is the order. A profile build says why: the natural order takes the COARSE leaf and the shuffled multiset takes the AVX2 quicksort leaf, because `AVX2_TAKE` requires `!c->nearly` and `sort_fn`'s 40 strided pairs call a sloped wave nearly sorted. The dense-disorder probe added in the same round is the statistic that would overrule it and gates only the insertion sort | MEASURED (`make sineprobe`'s shuffled control; paths from a `-DLOGOS_PROFILE` build, 126 ms coarse against 87 AVX2 on one multiset) | **when a shape is slow, sort its own values in random order before blaming the values**; fix predicted ~1.45x here and neutral on near-sorted shapes, unbuilt |
| B76 | **A chart nobody has dragged is a chart nobody has tested, and a fake of a library must reproduce its awkward behaviour (B38, a second time).** The scale report's zoom and pan drove 216 charts clean under the uPlot stub; in real Chrome a log axis could not be zoomed inside one decade, and a pan killed the tab. uPlot passes EVERY x window through the scale's range function and its log default rounds the far end up to the next decade (100 .. 25,118 came back 100 .. 100,000); and its log ticks walk up from the window's lower end and loop forever when that end is below 1 with a mantissa of 9.5 or more (`Invalid array length`). Its own zoom never meets the second because of the first | MEASURED (`python tools/browser_check.py`, which now drives the first panel with synthetic wheel, key, mouse and touch events in six modes; mutation-checked) | a log axis here brings its own range and its own ticks; the stub rounds the way uPlot does and refuses a log axis without `splits()`; it fails on both earlier cuts |
| B75 | **A sentinel that the SAMPLE happens to contain defeats the partition's certificates.** 840 zeros in 8.4M jittered timestamps: KS 0.99, the key grid rejected ("one bucket would take 168× its share"), a table scatter. The int32 counting route windows its sample against exactly this (B35); the f64 partition does not | MEASURED by trace (`LOGOS_NO_REACH=1 ./reachtrace 5 8388608 once zeros_840`); the cost is not yet measured in pairs | OPEN |
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
| C26 | Knot route, first cut of the sweep: every run endpoint cuts BELOW its value | 98.5% crossing mass on a staircase built with 10% | a strand's last element lands at the head of the next interval and turns the neighbour's whole middle into a two-strand crossing; a first value cuts below itself, a last value above. Caught by the trace build before anything was timed |
| C27 | Knot route gated at J5's n/2, on a predicted 14-thread break-even at 60–100% crossing | 0.92–0.96 on the shapes just past the gate; the predicted break-even does not exist on the two-strand ladder | see B58. Shipped gates are measured: 1.0n parallel (the pure-partition control), 3.0n sequential, 1.0n sequential with at most `MERGE_MAX_RUNS` runs |
| C28 | Knot route's placement under relocation's one-thread-per-500k cap | 0.84–0.99 at 1M and 14 threads on rows that win at 8.4M | the cap was measured on copies, which are bandwidth-bound; merges are compute-bound and ran on 2 threads against the partition's 10. The cap now counts work, n + 3 × cost: 1.26–1.43 |
| C29 | Untying three or more live strands in a PARALLEL sort (knot route, cost 1.57n) | 0.89 against the pure partition at 14 threads, 2.4× at one | staged copy plus an in-place merge tournament; declined early instead, the row rose from 1.16 to 1.24 against the shipped engine. A direct 3- or 4-way merge from the source is the untried angle |
| C30 | Knot route, first cut: runtime switches read with `getenv` on every sort, and a cost gate with no minimum run length | three sorted elements 6 → 42 ns; random n = 32 781 → 2431 ns; block_perm n = 100 2105 → 4969 ns | two `getenv` calls are ~40 ns; and when runs hold two or three elements nearly every element is a run endpoint, so almost nothing "crosses", the gate says go, and the sweep sorts the array the slow way. Switches now read from 512 elements up, route asked only at an average run ≥ 256: 0.93–1.03 of the committed engine from n = 0 to 3162 |
| C31 | Header and README: "`logos_sort_f64` uses ~9n (README: ~8n) bytes of scratch", "`logos_sort_i32` ~5n (README: ~16n)" | f64 15.5 at 1 thread, 23 at 14, 104 on a dominant value before the fix; int32 11.5 and 6.5 per 4-byte element | inferred from the design (one n-sized buffer), never measured; the contexts, the partition's slack regions and the index cache were left out |
| C32 | `logos_release_buffers()` "frees the persistent buffers" | it freed 9 of 23 bytes per element (9 of 104 on a dominant value), and none of the int32 counting route's table pool (up to 64 MB) | it knew about the scratch pool and not about the three caches added after it: the context bundles, the partition's regions, the count tables |
| C33 | TRUTH_TABLE F5, "`qsort` on allocation failure", graded "known" | 2,215 of 8,316 injected allocation failures ended in wrong output or a crash | true of the sites someone had thought about; `merge_all_runs` returned silently, a failed context was still handed to the leaf, the sequential path freed an untouched stack context. "Known" was the wrong grade for a path that had never run |
| C34 | Guarding the value RANGE against infinity (`isfinite(max - min)`), the day before | NaN-free data holding subnormals still came back unsorted at 300k elements and up | a range narrower than about 1e-305 is finite and its reciprocal is not: `classes / range` = +inf, `0 * inf` = NaN, clamped into the last bucket. The thing to guard is the SCALE |
| C35 | Reading where a crashed harness stopped off its redirected log | the log ended in the IEEE section; the crash was four sections later | redirected stdout is block-buffered, and the crash ate the last 4 KB. The harness is line-buffered now |
| C36 | `make sanitize` written with `detect_leaks=0` (the target's first form, the day before; the option was never tried the other way) | the first run with leak checking ON found a real one: 313 KB in 28 allocations, the dispatcher's sorted sample freed only on the fallback label when it is not handed to the partition (a NaN in it, another size) | the pools are reachable from statics, so LeakSanitizer never reported them; the switch hid the one thing it exists to find |
| C37 | Keeping an A/B switch in the tree without building it: `-DLOGOS_NO_SPLITTER_TABLE` (`make stbench`), and `-DLOGOS_NO_RELOCATE` (`make j5bench`) the day before | neither compiled; the first had been broken since the per-mode certificate was added | a flag nobody builds is a fallback nobody runs. A compile-only pass over the Makefile now builds all 49 targets that have a compile step |
| C38 | Cut decomposition as its own route (cut wherever everything before is no larger than everything after; sort each factor alone) | told the cuts: 1.64× at one thread and **0.30× at 14** on 2048-element windows (ranges that small fall under the leaf's flash threshold and a parallel sort sends them to a scalar quicksort); 1.43 / 1.11 on coarse-key groups. Found blind: 1.11 / 0.24, the detection alone 35 ms at one thread against a predicted 8–12 | a cut is a block boundary with an empty seam, so blocks + seams subsume it without the detection pass; a suffix minimum and a running maximum are dependency chains, not loads |
| C39 | Chunk relocation of UNSORTED chunks with disjoint value ranges ("links"), as a route | not built: the ceiling (2.0× / 1.3–1.5×, told the segments) is mostly the baseline's weakness on banded data (B74), and a segment detector was never priced |
| C40 | Putting strays back through a second buffer (two parallel passes: assemble, copy back) | +36 ms at one thread and +12 ms at 14 for ONE stray | replaced by one in-place shift pass: between two strays every element moves by the same amount, so the pass is a list of `memmove`s over exactly the span the strays travel |
| C41 | Sorting the route's blocks with the general leaf, then with the AVX2 quicksort | general leaf: 1.15–1.53× for the route at one thread where the flash kernel gives 1.9–2.5×; the AVX2 quicksort loses to flash by 1.26–1.81× on these blocks at one thread | B71, B72 |
| C42 | A rule that stops the block phase when strays keep turning up (3 stray blocks and an eighth of the blocks done) | **dominated**: strays in 28% of the blocks at 14 threads: 0.78× against the partition when the phase stopped itself, 1.24× when the strays were simply put back | a probing route's decline comes after its probe (C27); here the alternative to declining was cheap |
| C43 | "A small span means integers, send the block to the counting leaf" | every series with UNIT SPACING (index + noise) spans just under its length: 64-element windows took 293 ms at one thread through the general leaf and 76 through the kernel | test eight strided elements for integrality; a type fact may be assumed, a data fact must be certified (B33) |
| C44 | Trusting a statistic's comment: "an inversion rate over ~1900 Fibonacci-lag pairs" | the loop read **7** pairs, a whole array apart: sample indices i and i+144 are 0.31% of n apart but the later index is the EARLIER position (frac(144φ) = 0.99689), so the `p1 <= p0` skip meant for wrapped pairs dropped every adjacent one. It passed gray-code, sawtooth, block-permuted, staircase and grouped-run data as nearly sorted and turned away one near-sorted input in seven | count what a statistic reads. Found because a new gate wanted it at other lags and the counts came back 7 and 3 |
| C45 | A pre-gate that is a SIMPLIFIED copy of the route's scan (the near-sorted pre-gate: the greedy keep/reject scan without its poison recovery) | declined 12 of 40 seeds of sorted data with 1% random swaps and 38 of 40 with 2% (mean rejected 18% and 48%, where the route rejects 2% and 4%): the route was mostly unreachable on the data it was built for | a pre-gate runs the route's OWN scan on a sample of windows, or it measures something else |
| C46 | A harness section for a new route that checks only the output | it passed with the route NEVER FIRING at one thread (the gate's sample is 384 points there, the ladder needs 2048); and the diagnostic added to catch that reported the PREVIOUS sort's answer until it was reset at the top of the dispatcher (from the route's floor of 2^18 elements up: C49) | `logos_reach_last()`: every branch is asserted BY NAME |
| C48 | A harness row that demands identical BYTES from two parallel runs on data holding a NaN (the tail route's parallel NaN row) | it passed for weeks because the broken 7-pair statistic sent its input to the deterministic near-sorted route; read properly the input goes to the partition and two runs of the SAME configuration differ in 21 of 40 tries | with a NaN the engine promises a return and a permutation, and a parallel partition's bucket order depends on timing: check the promise. First seen as one unexplained failure under ThreadSanitizer, whose target cut the FAIL line off with `tail -3` |
| C62 | The proposed "Moebius twist pointer routing" sort (array -> doubly linked list, descending runs read through `.prev`, runs merged pairwise), claimed faster than the engine on sine-wave data | **slower on every shape, in both languages**: C at one thread 0.03-0.51 as written and 0.01-0.23 in its relinking form (clean and noisy sines 0.16-0.29); JavaScript 0.07-0.30 against the JS engine; 1.18-1.33 only against V8's comparator sort on uniform | a natural merge sort with a list in front: a 24-byte node per element before any sorting, then log2(R) full merge passes. Its one idea, reading a descending run backwards, is the engine's twist already. `make dllbench`, `experiments/moebius_dll/bench.js` |
| C63 | The unknotting problem as a sort: a circular doubly linked list, R1/R2/R3 as local rewrites, swings and tangle collapse (Kauffman & Lambropoulou) | **slower than the engine on 15 rows of 16 at one thread**: near_sorted 0.36, block_perm 0.26, staircases 0.20-0.29, sorted 0.14, short reach 0.08-0.11, uniform 0.009; 1.40 only on the rotated keyed runs, which was the engine's floor cliff (B91) | a list pays a node written and walked per element before any move, and the engine's global reading (the knot route's sweep) already is what the paper says local moves lack. `./unknotprobe 20 8388608 moves` |
| C64 | Forgiving each run-detection chunk its partial first run, for runs of exactly the floor at P threads (predicted 1.1-1.4) | **0.974-1.026 on every row**, two runs; reverted | the wrong condition was diagnosed: the per-thread run cap (n / floor / P + 2) binds before the average test. `./unknotprobe 40 8388608 ab` |
| C65 | The loop-back's first cut: rotate, then enter the whole dispatcher again | **0.89 at one thread** on the ring buffer (and on two swapped blocks), 1.12 at 14, against 1.56-1.94 once fixed | the re-entry's prescan re-read everything, and a ring buffer's sort is three or four passes: one redundant pass is the whole margin. Hand the re-entry what the rotation knows |
| C66 | An outside claim (2026-09-21): on an ascending sine wave the speedup came from O(1) reversal of 2-3 element dips plus a boundary check `A_max <= B_min` that lets sorted chunks be concatenated instead of merged, "99 comparisons for 100 tangles" | **not how the engine sorts a sine, and not available there**: a sine with a slope is 65 monotone pieces of ~10,000 elements at 1M (no 2-3 element dips; with noise, 600k runs of 2-3 that ALL overlap their neighbours); adjacent pairs with A_max <= B_min: **0 of 64**; sorted by value, 64 of 64 overlap. The engine runs that shape 1.22-1.31x SLOWER than random data at one thread (B79) and the DLL sort the claim describes measured 0.16-0.29 of the engine on the sine rows (C62) | the interval test IS in the engine, three times over (J5 copies disjoint runs by memcpy and merges only overlapping chains; the knot route merges only where strands cross; the unknotter's R2 walks only overlaps), and it is what makes block_perm cost 2 ns per element. It buys nothing where every piece overlaps its neighbours, which is what a wave IS. Reversing a run costs its length, not O(1); the twist avoids it by carrying a bit |
| C67 | A Weyl-position pivot sample for the AVX2 quicksort (the 63 strided samples suspected of aliasing with a 32-cycle wave) | **0.996-1.009 on every row**, two runs | the second diagnosis of the wave's cost in that quicksort to fail; reverted |
| C68 | Direct three- and four-way merges of the knot route's crossings, one pass from the source | **0.68 on three live strands, 0.52 on four** at one thread | two dependent compares and a chosen load per element against two memcpys and branch-free two-way merges; reverted |
| C69 | The stray list allocated on every sort (1 MB at 8.4M) | **0.94-0.96 on clean block shapes** that found no stray | taken at the first stray now: 0.99-1.01 |
| C70 | The wave route without a crossing-cost gate | **0.76-0.77 on a swing of two periods** | pieces four and five deep make the sweep's k-way merges dearer than the leaf; the sample prices it (limit 1.5 levels per element) |
| C71 | Reading strays to the cap before declining (3% strays, cap n/64) | **0.92-0.93** | a density exit past a thirty-second of the array: 0.99-1.00 |
| C72 | An outside write-up: a merge sort's O(1) boundary check (`a[mid] <= a[mid+1]`: skip the merge) plus reversing small descending runs sorts macroscopically ascending, locally noisy data in O(N), e.g. server logs and sensor telemetry | **the check skipped 0 of 262,143 merges on every shape with network latency**, and the hybrid merged every element 18 times, level with std::sort; the engine beat it 3-6x on those shapes and 12 of 13 overall. It skipped every merge on sorted data and adjacent swaps, where it beats the engine 1.6x | latency reaches farther than the recursion's 32-element blocks, so no two adjacent halves are ever in order; the floor for displacement d is ~log2(d) comparisons per element, not O(1). `./tsbench ... anatomy` |
| C73 | An outside proposal: condense into natural runs, sort the K blocks by their min edge in O(K log K) ("trivial because K is drastically smaller than N"), rearrange them into "an ascending sine wave" and let the boundary-bypass merge sort finish | **K is 41% of N on every noisy shape** (runs of 2.4), the block sort is 15-47% of the pipeline, elements stay 38-6,608 positions from home and the hybrid still merges each 18 times; **1.4-9.9x behind the engine on 13 of 16 shapes, and slower than the hybrid alone on 15 of 16** | a maximal run swallows the next block's first element, so its min edge names the wrong block (1.25M positions off on the proposal's own best case); the engine splits runs at gaps first (J5) and places each element once. Its three wins were engine defects, all fixed (B101-B103). `./tsbench ... edge`, `... edgeanat` |
| C74 | An outside proposal, "the Triple Reversal Algorithm": condense into runs, meta-sort them by min edge, permute them in place by triple reversals ((A^R B^R)^R = BA), weld the seams left to right behind an O(1) boundary check | **FALSE as a sort: 2-4,600x behind the engine on 14 of 16 shapes at one thread, 2.6-368x at 14; the O(1) check bypassed 0.0% of seams on every shape** | a maximal run swallows the next block wherever it starts higher, so units span holes and the prefix's max is near the top at once; the merge behind the O(1) test is O(prefix): quadratic (late batch 390-465x, uniform 4,079-4,639x). Its two wins were engine defects, fixed (B105, B106). `./tsbench 16 1048576 weld`, `weldanat` |
| C75 | A handoff specification of what remains to implement in a Moebius hybrid: bail out to a fallback under 16 elements, swap 2-3 element descending dips inline (Reidemeister I), stitch the runs with a galloping merge behind an O(1) boundary check | **behind the engine on 14 of 16 shapes as written and 13 of 16 repaired**; the micro-kink branch fires 0 times on 1% adjacent swaps; the bail-out as written is a no-op; the O(1) check skips 0.0-1.6% of boundaries | a dip's first element belongs to the preceding ascending run, so the descending run is one element; a run the scan found needs no sorting; and merge ORDER decides: the same runs cost 100,999 merged elements per element left to right and 4.6 bottom-up. Its one win was an engine defect, fixed (B107). `./tsbench 16 1048576 moebius`, `moebanat` |
| C76 | "Treat uniform data as a complete computation": certify the distribution from the sample, sort P chunks with a parallel SIMD quicksort and merge them, if that is fastest | **the chunk sorts alone beat the engine (0.76-0.86 of its time at 8M, 14 threads), and the merge then costs about 3x the engine's whole runtime: 3.3-4.2x on every shape, uniform included** | the merge moves 3.86n on every shape but costs 87 ms where chunks span the whole range and 15-18 ms where they are disjoint: a merge is comparison. The engine's answer on that certificate is one multiply-add into an equi-width grid, no comparisons (§3m). `make unibench` |
| C77 | Give the counting routes a thread ceiling, and gate the near-sorted route off at low thread counts, so the idle threads stop costing | **both rejected by their own stop rules**: no global ceiling passes (the lattice family loses 0.53-0.90 at 8M-16M), a hash-only ceiling reads 1.0 at 4M and 1.1 at 8M, and the near-sorted route is worth 1.80-1.97x at two threads on its own shapes | withholding threads does not convert idle threads into work: the lattice map wants every thread, and the near-sorted route earns its threads. The fix the profile endorses is a PARALLEL merge (0a) and a task queue for relocation's placement. `make capbench`, `NSG_ROUTE=1 ./nsgatebench` |
| C78 | The counting routes' serial merge is what leaves 7-8 of 14 threads idle, so making it parallel is worth ~1.5-2x | **FALSE: the parallel merge is 1.02-1.12x SLOWER**, and the trace says why — the merge is ~2 ms of a route whose map phase is 11.6-14.1 ms, so it is ~10% of the route | a thread-occupancy average says how many threads are busy, not which phase they are busy in. The map phase is ~80% and is where the idle threads still are. Built, correct under every checker, shipped OFF behind `LOGOS_LAT_PAR_MERGE=1` |
| C79 | A 32-bit prefix of the total-order key separates enough of the data to sort on, and the pre-written test for that ("under 90% of elements separated = dead") settles it | **the criterion asked the wrong question**: measured separation is 55.5% uniform, 53.5% heavy_tail, 73.9% int_wide, 0% few_distinct/quantized -- but few_distinct's and quantized's ties are 100% DUPLICATES (free to resolve) and the three random shapes' real tie groups are at most **7 to 10 elements**. Only clustered (10,274) and bimodal (512,897) genuinely collapse | the idea is closed anyway, on a different ground: prefix + a 32-bit index is 8 bytes, exactly what the double already weighs, so nothing is halved. **A kill criterion can be written in advance and still measure the wrong quantity** -- here elements separated, where the cost is the SIZE of the groups that are not |
| C80 | The work queue that pays on the lattice map will also pay on relocation's output tiles, where `make profile` measures 3.6 of 14 threads WAITING at a barrier and per-thread busy is 1.39x bimodal (predicted 0.88-0.97 at 4M+) | **FALSE: 0.96-1.04 on every cell of two runs** at 1M-16M, 60 pairs, on both block_perm and block_overlap. Reverted | the lattice map is COMPUTE into a private per-thread table, where a faster core finishes more elements; relocation's phases 1 and 3 are `memcpy` over disjoint ranges, where one shared memory controller is the bottleneck and extra tiles cannot raise the aggregate. **A work queue buys balance only where the cores are the bottleneck.** It also relocates the target: the waiting threads are at the SECOND barrier, on one indivisible chain merge -- the serial middle, not the tiles |
| C81 | The lattice route's idle threads are its serial merge (and, on a trace build's reading, its 2 ms of table teardown) | **both FALSE, measured with the route split into four profiler phases**: at 14 threads the merge is 1.0-5.8% of the route and the teardown is **9-14 microseconds (0.14%)**. The idle threads are in the EMIT, at concurrency 3.03-3.17 of 14 | the emit hands every thread exactly n/P output positions, so it cannot be an imbalance. A standalone replica splits it: **35% is create+join of 13 workers** and the rest is bandwidth-bound (67 MB of stores at 65 GB/s; 14 threads buy 2.7x over one). The 2 ms teardown was a TRACE-BUILD artifact -- a trace build gives shares, never a price against a normal build |
| C82 | Projecting each element onto a sphere of radius n (x = value, y = a prime, z = the index) and sorting the projection | **FALSE on ORDER, not on cost**: 22.2% of 2.0M pairs at different indices come back order-INVERTED (`invprobe`). proj_x = R*x/sqrt(x^2+p^2+z^2), and with z to 8.4M against values to 1e4 the z^2 term (7e13) dwarfs x^2 (1e8), so the projection orders by value/index, not by value | cost alone would NOT have killed it: the projection is 2.89 ns/element against the engine's 0.60 ns addressing multiply-add, which spread over 14 threads is ~0.21 against a 0.78 ns/element sort. Three of my six timing predictions were wrong IN THE PROPOSAL'S FAVOUR (a constant modulus is strength-reduced to a multiply: 1.09 ns, not 5-15; a 16 MB table with 40k distinct hashes stays cache-resident: 3.33 ns, not 20-80) |
| C83 | Detecting the data FORMAT (these are dates) and sorting on format-derived keys beats detecting only the distribution | **FALSE for numeric timestamps**: a sort TOLD the origin and step is **0.24-0.77 of the engine** (1.3-4.2x slower) on six date shapes, and returns WRONG OUTPUT on day-plus-fraction, which has no exact integer form. **TRUE for date TEXT**: parse + `logos_argsort_f64` + gather is **8.48x** over `std::sort` on `std::string` and **4.87x** over fixed 24-byte `memcmp` records | the engine already certifies the exploitable parts -- the trace shows `day_serial` (3,650 distinct) taking the LATTICE route and the ms/us shapes taking the partition at 1.01-1.04x leaf imbalance, correctly, since 8.4M values over 86.4M slots hold no extra structure. **A format is worth what it saves in COMPARISON WIDTH, not in distribution knowledge** |
| C59 | Running the galloping merge's lookahead on EVERY two-strand crossing (first cut) | the staircase where four elements in five are merged read **0.980, 0.948 and 0.976** over three runs while every other row sat at 1.00 | a balanced crossing can never gallop far, and the call site knows both piece lengths. Only pieces that differ fourfold take the galloping kernel now: 1.004 at 200 pairs |
| C60 | Predicting the dense-integer bitmap's value beyond its budget from the cache ("16 MB is most of the last-level cache, 32 MB is past it") | predicted 1.4-1.8x at 6n and a loss by 32n; measured **5.0x at 6n and a win out to 64n** | the map's cache footprint was never the cost that mattered; the leaf it replaces (the int32 radix, 35 ns per element) was. The same mistake as the dense gate's original 4n: price the ALTERNATIVE, not just the route |
| C61 | A harness row meant to test a gate, built from a SORTED array ("span 8n: must decline", `i * 8`) | it passed for months and **never reached the gate**: the prescan returns a sorted array first | the rows that reach a route must be shuffled, and must say which path ran (`logos_twist_last(6)` for the bitmap now). Under the old budget exactly the eight rows that expect the bitmap fail, which proves they test it |
| C58 | Clearing `c->nearly` when the leaf's dense-disorder probe fires, so that `AVX2_TAKE` admits a noisy sloped wave (open direction 0i (f), predicted 1.45x) | **neutral: 1.02** on the noisy 32-cycle rows, 0.97-1.06 elsewhere (`./sineprobe 40 4194304 ab`, one binary). Reverted | the diagnosis was wrong. It ranked the coarse leaf against the AVX2 leaf by a PROFILE build's times (126 ms against 87); paired in a normal build they cost the same on this shape. The order cost is inside the AVX2 quicksort (B87). **A profile build says which path ran and what share of ITS OWN time each phase took; it does not rank two leaves** |
| C56 | The knot route as a LITERAL linked list (`LOGOS_KNOT_LIST=1`: a 32-bit link per element, twisted runs linked backwards, crossings untied by relinking, the sorted list walked out) | **0.50-0.52 at one thread on blocks, 0.55-0.57 on x20 staircases, 0.73-0.75 on x80, 0.62-0.69 at 14 threads**, and 4 more bytes per element (`./twistbench 60 8388608 list`, one binary; `memprobe` 15.87 -> 19.87) | a link written, a link read and a scalar walk per element, for work a memcpy does eight bytes to the cycle. It gives up least where merges dominate (a relinking merge moves no data), and the twist is free in it. Predictions were too kind everywhere except x80, wrong the other way. Kept opt-in with its own harness section (504 checks, 144 asserting the list was the path) |
| C57 | A value-speculating list walk (guess next = cur + 1, let the load only check) written in plain C | measured **the same as the plain pointer walk** (0.43 against 0.44) | on the path where guess and link compare equal the compiler continued from the LINK, which is legal and restores the chain of dependent loads. Laundering the difference through an empty `asm` hides the equality: 0.43 -> 0.51. **When a speculation trick measures like the code it replaces, read the assembly** |
| C54 | Adding a public symbol (`logos_twist_last`) without adding it to the Makefile's `RENAME` list | **every multi-copy bench stopped linking** (`callbench`, `ledgerbench`, `pfbench`, `tailbench`, ...): duplicate symbol | last round's "all 54 flag variants build" compiled the ENGINE with every flag and never re-linked a bench. Found a round later by `make callbench`. A new public symbol goes into `RENAME` in the same edit; three benches rebuilt as the check |
| C55 | Pre-gating the mirrored STREAM probe on the sample's writhe (`inv_all >= 0.60`), first cut of the whole-array bit | four mirrored streams read **0.88 at 1,048,576 elements and under 0.60 at 1,000,003**; the route never fired there | whether a sampled pair falls inside one stream depends on the lag modulo the stream count. The probe is now asked whenever the ascending one would be, and stops after ~25 reads on data that ascends. Found by a harness row that NAMED what must happen |
| C52 | The orientation bit one level down, in the leaf's `detect_runs_limited` (the galloping run-merge, the leaf's own J5) | **not built: ceiling 1.02-1.06** (6 and 64 descending teeth over one range, 8.4M and 3,000 elements, `make mirrorprobe`) | at one thread the knot route takes few-run data first and already carries the bit; what still reaches the leaf's detector is sorted at ~16 ns per element, where a swap pass is a thirtieth of the work. The twist pays on CHEAP routes only |
| C53 | Predicting that a 95% sorted prefix costs a parallel sort about what its mirror image costs (1.1-1.2x) | **wrong in direction: the mirror is 1.8x FASTER at 14 threads** (0.56, two runs) | not the tail route, which still wins there (1.075 against `LOGOS_NO_TAIL=1`, 3.95 at one thread). A sorted run with a random suffix costs the parallel engine 1.8x a descending run with a random prefix. NOT TRACED: a lead, open direction 0k (c) |
| C50 | One merge loop with a signed stride per strand, for every orientation pair (the twist's first two-ended merge) | the block rows recovered their ceiling and the staircases did not: every other block twisted read **1.000 and 1.002** at one thread against a ceiling of 1.067 | four cursor updates per element where the untwisted merge has two. One loop per orientation pair, the far-end address folded into the base: 1.05-1.06 over the strided loop on x20 staircases, 1.10-1.11 on x80, same binary (`./twistbench 120 8388608 merge`, `LOGOS_TWIST_GENERIC=1`). **A ceiling per SHAPE is what showed it: the rows that fell short were the rows that merge** |
| C51 | Predicting the twist's one-thread ceiling from "a run route costs 2-9 ns per element" | predicted 1.03-1.08, measured **1.07-1.17** | the run routes cost 2.3 ns per element at one thread, so one swap pass is a sixth of the sort. The cheaper a route already is, the more a single avoidable pass is worth: look for passes on the FAST routes |
| C47 | Comparing two runs a few minutes apart, once more (the route's flash against the general leaf on 64-element windows: 86 ms against 76) | the paired rows inside one run said the opposite, 1.12× for flash, and 1.94× on 2048-element windows | methodology 3; the JS twin of the layout bias is MODULE LOAD ORDER: the module loaded second read up to 30% slower at 2000 elements |
| C49 | Judging a round's cost per CALL from benches that start at a million elements; then from one row of one small-call binary | the round as built cost **1.09–1.14 at n = 0, 1, 2** and nothing showed it. Of three causes one was measurable (a helper's 32 KB of arrays inlined into the dispatcher: 8 stack probes became 10), one was not (two `__tls_get_addr` calls: predicted +20–35% on a sorted triple, measured +5% / −14% by orientation), and one was a 17-point "win" at n = 100 from moving a never-executed loop out of line, which the round as built, with the same inline loop, also showed | `make callbench`; rows from 10 to 100 elements read 0.78–1.13 across nine builds with identical logic on their path, reproducibly. **Reproducible is not real.** A small-call row counts at n ≤ 5, in both orientations and two runs, with a mechanism visible in the assembly |
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
| D40 | **The knot route**: runs as strands, a sweep over the 2R run endpoints, pieces that lie alone are copied and only what crosses is merged; staircases of overlapping sorted blocks, which relocation declines because one chain covers everything, no longer pay for a full sort | MEASURED (`make knotbench`, FINDINGS §3ac): 1 thread 2.6–4.4× from 10% to 100% of elements crossing, three live strands 2.6×, eight 1.4×; 14 threads 1.2–1.5× against the shipped engine, a TIE (0.96–1.11) against the pure partition; guards 1.00; 401 harness checks, clean under ASan/UBSan | `knot_plan`, `knot_worker` in `relocate_try_f64` / `runs_relocate_f64`; `LOGOS_NO_KNOT=1`, `-DLOGOS_NO_KNOT` |
| D41 | **Resource exhaustion costs speed, never correctness**: every allocation and every thread creation of a cold call can fail, singly or from some point on, and the array still comes back sorted | MEASURED (`make faultprobe`: on the same 19 cases 7,008 injected failures, 0 bad, where 2,369 of 8,502 had been; the final engine's 23 cases, with the in-place entries and a 70-run chain, 7,894 and 0 bad; with the targeted sweeps all 19 worker functions and the spill path are reached; about 2,150 harness checks repeat a sample in-process, and the thread-creation half also runs under ThreadSanitizer: 186 failed creations, 0 races) | `par_run`, `par_spawn`, `barrier_drop`, `TILE_IS_MINE`, all-or-nothing `ctx_alloc`, `merge_all_runs` on stack lists |
| D42 | **`make memprobe`**: what one cold sort weighs, what it still holds, what is left after release; f64 and int32, 1 thread and P, the in-place entries beside them | MEASURED, and it is the only bench in the tree that does not time | `memprobe.c`; `MEMPROBE_LOG=<bytes>` prints each large allocation with its call site, which is how the double allocation was found |
| D43 | **`make slopebench`**: the scaling exponent measured in adjacent pairs, one large sort against K small ones of the same shape moving the same number of elements, so the drift between two rows of the scale report cancels | MEASURED: null ±2%, `std::sort` p = 1.063–1.066 against a floor of 1.062 | `slopebench.cpp`; `./slopebench 8 50000000 50000000 uniform` is the null test |
| D44 | **NaN with a defined order**, as a separate entry: `logos_sort_f64_nan_last` moves NaNs to the tail with their bit patterns intact, then sorts the prefix. The default entry keeps its unspecified order and gains a tested floor: no crash, no out-of-bounds access, output a permutation of the input's bits | MEASURED (432 harness checks, ASan clean) | `logos_nan_to_end_f64`, `sample_sort` swaps instead of rewriting |
| D45 | **The strict in-place sort runs in parallel**: a placement pass is cycle-following and does not split, but its 256 buckets are disjoint ranges, so ranges are tasks on a fixed 4,096-entry stack (96 KB whatever n is) | MEASURED (`make ipbench`, 14 threads, paired): against its own 1-thread form 2.50× at 8.4M and 2.85× at 50M on uniform, 2.5–2.6× on the other random shapes, 1.1–1.6× on the structured ones, no row below 1 at the 2²⁰ threshold; still 6–24× slower than the buffered sort at the same 14 threads. 408 harness checks, its first | `inplace_parallel`, `ip_worker`; next: the placement pass itself (speculative permutation with repair, PARADIS), the serial 30–35% |
| D46 | **Boundary sweep**: every n from 0 to 300, every power of two from 2⁹ to 2²¹ and the 100k / 350k / 500k multiples from below, at and above; both types, 1/4/8 threads, buffered and in-place | MEASURED: 12,058 checks, passed on the first run | `boundary_checks` in the harness; plus a 70-run chain that drives `merge_all_runs` past its 64-run stack lists (traced) |
| D47 | **The short-reach route** (`reach_try_f64`): index blocks sorted where they lie with a flash kernel, seams repaired in place, strays set aside by median fences and shifted back in place; the gate is a ladder of Fibonacci lags over the shared sample plus four windows. 8.4M: latency_4096 1.91 / 1.31, window_2048 2.22 / 1.38, jitter_1000 2.05 / 1.33, latency_512 2.38 / 1.49 (1 / 14 threads), growing with n; guards 0.98–1.03 against the engine before it, layout cancelled; 0.10 / 1.33 bytes per element | MEASURED (`make reachbench`, `make reachhunt` for the ceilings, `make memprobe`, `make faultprobe`: 492 injected failures, 0 bad; 595 harness checks) | f64 only; declines above ~117M elements (ladder distances are fractions of n, blocks capped at 131072) |
| D48 | **The near-sorted route's gates read what they were meant to** (`inv_all`: every lag-144 pair in position order; the pre-gate runs the route's own scan with poison recovery): sorted runs grouped by a coarse key 3.82× at 14 threads, sorted + 1% swaps 1.30–1.48× on four seeds, + 2% swaps 1.23–1.37× (8.4M); 0.5%, 3%, 5%, 10% swaps and eight guards 0.97–1.04. Then the ROUTE was measured against the partition for the first time across sizes (the old pre-gate had turned 38 of 40 inputs with 2% swaps away): ≤1% swaps win at every size (1.07–2.1×), 2% swaps lose at 1.5M (0.84–0.87) and win only from 6M (1.33–1.41), 3% swaps lose at 1.5–2M (0.75–0.80). So the gate is 2.5% below 6M elements and 5% from there on; with it the mid-size rows read 1.00–1.36 and the grouped runs 2.8–3.8× | MEASURED (`make nsgatebench`; `NSG_ROUTE=1` for route-against-partition; `LOGOS_NS_GATE_OLD=1`) | the route's fixed costs (three spawn phases, one stray sort) are open item 0h's territory |
| D49 | **The general leaf tells dense short disorder from near-sortedness** before it tries an insertion sort (64 strided pairs eight apart): 1.36–1.87× at 14 threads on shapes the short-reach route declines, 1.17× on regime_switch at both thread counts; ported to the JavaScript engine with a budget for its small case (1.6–19×) | MEASURED (`RB_OFF=LOGOS_NO_DENSE_PROBE=1 ./reachbench`; `engines/js/test/perf-guard.js`) | |
| D50 | **`make reachhunt`**: ceilings for disorder of short reach (cuts, blocks + seams, links) beside the zoo of twenty, which it includes unchanged; two new shapes (coarse_key_groups, regime_switch) | MEASURED | its late declines are what exposed D48's 5.8× cliff |
| D51 | **`make callbench`**: what one call costs at 0 to 50,000 elements, random and sorted, a baseline engine (the committed one, or `CB_BASE=`) against the working tree, two binaries with the copies swapped. First results: the short-reach round as built cost 9–14% per call at n ≤ 2 (repaired: 0.98–1.01); entering the dispatcher costs ~5.6 ns (a thread-local read, a call, nine probed pages of a 36 KB frame), so `logos_sort_f64` returns at once when n < 2: **8.5 → 2.9 ns** (0.29, orientations 0.34 and 0.25) | MEASURED (`make callbench CB_BASE=<the engine before the round>`) | every call of 2 to 24 elements still pays the 5.6 ns: open direction 0g; its limits are C49 |
| D52 | **`make twistbench`**: `ceiling` prices the reversal passes (an array with descending blocks against the same array with them already ascending: after the detector's reversals the two are byte-identical), `ab` is `LOGOS_NO_TWIST=1` against the twist, `merge` is the strided two-ended merge against one loop per orientation pair; all in one binary with one engine copy, so there is no layout term. Harness: `twist_checks`, 921 checks, 284 naming the path through `logos_twist_last()`, mutation-tested (a wrong tile-clipped offset fails 25 rows in the knot worker, 3 in relocation) | MEASURED | the named rows found a detector defect older than the twist: a chunk boundary that leaves a body under `RELOC_MIN_BODY` fuses with the next block and plain relocation declines disjoint blocks (1,000,003 elements, 8 threads, knot route off). Not fixed |
| D53 | **`make mirrorprobe`**: every cheap route's shape against its own mirror image (`ceiling`), and against one timed reversal pass plus the sort (`simple`), with a uniform control that prices a false positive and block controls the twist already takes; descending teeth for the leaf's detector; `tail` = the tail route against `LOGOS_NO_TAIL=1`; `make mirrortrace` + `once` prints the route each shape and each mirror took | MEASURED | a probe: it changes nothing in the engine. Its header holds the predictions, the grades and the verdict |
| D54 | **`make strandprobe`**: `strays` = sorted blocks with 0 / 0.1 / 1 / 5% late records against a sort told which positions are strays, with a 0% row that prices the told sort's own overhead; `writhe` = writhe, persistence and turns from 2,048 strided points over sixteen shapes, scored against a rule fixed in advance. `twistbench` gained `scan`, `mirrorprobe` gained `ab` and a row filter | MEASURED | probes: they change nothing in the engine |
| D55 | The scale report carries thirteen shapes: the nine standard ones and `block_twist`, `block_kinked`, `sine_slope`, `reach_mirror`; `strandprobe` gained `kinks` (a ceiling whose teller costs nothing: its 0% rows read 1.00-1.02) and `kinkab`; `sineprobe` gained `ab [SWITCH]`, any per-sort environment switch against the shipped engine on every sine shape | MEASURED (`make scale-full`; both report checkers clean, 312 charts driven, six browser modes) | inside the 10M rows logos leads x86-simd-sort on three of the four new shapes and LOSES sine_slope at one thread, 251 ms against 162 (B87) |
| D56 | `knotbench` gained `ceiling` (a sparse shape against its twin with nothing crossing) and `gallop` (`LOGOS_NO_KNOT_GALLOP=1` against shipped) and five sparse-over-dense shapes; `mcbench` gained `span [lift] [first]` (the dense-integer bitmap's budget laddered, with guards the wider gate lets in) | MEASURED | both changes of this round were decided by these, one binary each |
| D57 | **`make dllbench`** and **`experiments/moebius_dll/`**: the proposed Moebius DLL sort as given (JS) and in two C translations, paired with the engine (C at one thread; JS against logos-sort and V8's sort), seven shapes, every output verified | MEASURED | a probe; no engine change |
| D58 | **`make unknotprobe`** (`ring`, `moves`, `count`, `cliff`, `ab`, `once`) and **`make unknottrace`**: the unknotter on a circular doubly linked list, a shape against itself rotated as a ring buffer, the relocation floor's ladder, and the A/B of the loop-back and the floor | MEASURED | the loop-back and the one-thread floor SHIPPED; the unknotter is a probe |
| D59 | **`./unknotprobe ... ab SEAM|STRAY|WAVE`**, **`... nocopy`**, **`... lis`**, **`... once2`** (for `make unknottrace`): round six's A/Bs, the copy-back ceiling and the LIS classifier | MEASURED | seam, strays and wave SHIPPED; pivot and direct merges reverted (their rows are recorded in the probe's header) |
| D60 | **`make tsbench`** (`all`, `anatomy`, `fixes`, `once` for `make tstrace`): thirteen server-timestamp shapes, the engine against std::sort, the boundary-bypass hybrid and itself with every order-reading route off | MEASURED | two fixes SHIPPED (one strand; the seam at P threads) |
| D61 | **`make tsbench`** round two: `edge`, `edgeanat` (the edge sort and its trimmed form), `prescan`, `ties`, `onefix` (the three fixes against their switches), `switch` (any switch, or a null one, against the shipped engine) | MEASURED | three fixes SHIPPED |
| D62 | **`make sineprobe`** modes `cos` (cos against sin, same noise, routes), `fewgate` (the knot route's few-runs gate against 3.0n on waves of h overlapping runs), `legs` (a cos descending leg and a sin ascending leg, both orders): with a slope a cosine sorts as a sine at one thread; a zero at a[0] costs the 14-thread partition its key grid (7-17%); the few-runs gate forgoes 1.6-2.9x on noise-free waves of 3-8 runs | MEASURED | nothing shipped: two open directions |
| D63 | **`make sineprobe` mode `tilt`**: a linear trend under the scalloped wave and the cosine, from none through a min-to-max line to past the slope at which the wave stops falling: noise-free the cost falls to 0.2 of uniform and the array is sorted past it; with noise the trend buys nothing and three routes misfire (1.4-1.6x uniform at 14 threads) | MEASURED | nothing shipped: three open directions |
| D64 | **`make tsbench`** round three: `weld` (the weld, its gather twin and its P-thread form against the engine at 1 and 14 threads, one shot where a weld takes over 5 s), `weldanat` (K, each step's time, rotations, seams bypassed, elements merged) | MEASURED | the weld: a negative; two fixes SHIPPED |
| D65 | **`make tsbench`** round four: `moebius` (the handoff pipeline, literal and repaired, against the engine, std::sort and the weld at one thread), `moebanat` (runs, twists, kink swaps, fallbacks, merges, boundaries skipped, elements merged) | MEASURED | the pipeline: a negative; one fix SHIPPED |
| D66 | **`make unibench`** (needs the x86-simd-sort clone): P chunks sorted by the AVX2 quicksort and merged by a merge-path tree, against the shipped partition; `anat` prints the chunk and merge milliseconds, the levels and the elements moved | MEASURED | a negative; nothing shipped |
| D67 | **`make amprobe`** (a thread ladder with the Karp-Flatt serial fraction per route, and `ladder` for the counting routes across five sizes) and **`make capbench`** (a counting-route thread ceiling paired against no ceiling) | MEASURED | the ceiling: a negative, shipped off |
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
| F1 | Types: f64 and i32 only (f32 is narrowed INSIDE the f64 path when every value is exactly a float, one thread); no i64/u64/f32/u32 API; no key-payload pair sort (argsort + gather is the route); C strings through `logos_sort_strings(_mt)` (NUL-terminated, strcmp order); many arrays in one call through `logos_sort_f64_batch` | known; bizdev GAPS 4–5 |
| F2 | Not stable (argSort is stable). ±0 kept distinct and bit-exact on counting paths. With a NaN present the ORDER is unspecified, of the other values too (measured: out of order in 246 of 672 probe runs), but the call returns (it did not always: B69), nothing crashes and the output is a permutation of the input's bit patterns; `logos_sort_f64_nan_last` defines the order | MEASURED (432 checks, and 150 sorts under a watchdog) |
| F3 | Output identical for any thread count | MEASURED at 1/2/4/8 |
| F4 | Scratch, two figures. ASKED FOR (`make memprobe`, peak traced heap of one cold call): f64 ~16 bytes per element at one thread on the partition path, 14–17 at fourteen; ~9 on relocation and knot; under 1 on the counting routes; strings ~24 per string; int32 11.5 / 6.5; in-place variants 0 (a fixed 96 KB in parallel). RESIDENT (`make rssprobe`, `make mixprobe`, 4M): one thread 0.1–0.6 on the sorting kernels and at most 4.4 on the routes that place or merge (half a scratch, 3bq); 14 threads ~8.8 on the partition; ~14 kept between calls after ten mixed sorts. `logos_release_buffers()` returns all of it, `malloc_trim` included; `LOGOS_LOWMEM=1` trades 1.25–2.5× of time for a 14-thread peak of 1.5–2.6 | MEASURED |
| F5 | Worst case bounded: coarse depth cap → heapsort; dual-pivot introsort. A failed allocation or thread creation costs speed, never correctness | MEASURED (`make faultprobe`; about 2,150 harness checks) |
| F6 | Every speculative path has an abort budget: momentum (8n displacement), lattice (second distinct value on a slot), modcard (65 536 cap), lowcard (1024 cap), near-sorted (inversion sample) | known |
| F7 | Cardinality coverage: up to 8 values counted in vector registers, ≤256 lowcard (a verified perfect hash), the lattice route for any lattice the sample certifies (to ~250k values at 1–4 threads since 3br, the last n/32 off it spilled), the hash route to Chao1 ≈ 3300 at 8+ threads (its serial merge, 3aa) and by the leaf-fallback test below that, dense integers by bitmap (sequential, spans to 16n), a dominant value at a 20% share (P > 1); above those the partition or the leaf | known; the 65k–1M-distinct band is partly covered now, and hashing past L2 measured a loss (3br) |
| F8 | 54,832 harness checks in 48 sections on 2026-11-06 (22,322 when this table was compiled; the resource section's count moves with thread timing), threads 1/2/4/8, every route's section asserting the path it expects; under ASan + UBSan + float-cast-overflow (4 known reports: the clamped NaN-to-index casts of open item 0h (e)) and under TSan (0 races); `make faultprobe` 7,894 injected failures, 0 bad; three invariant probes (order, isomorphs, edges); every bench verifies every output | MEASURED |
| F9 | Permutation indices are u32 (n < 2³²) | known |
| F10 | Rust: v2 numerics only; JS: its own string engine, single-threaded, the only `sortWithProxy`; C: the fastest string sort of the three (8-byte prefix keys, 3bl); Python: reference | PARITY.md |
| F11 | No streaming sketch anywhere (no KLL, t-digest, HyperLogLog). Top-level splitters come from one strided 2560-sample; per-bucket classes are equi-width over a prescanned min/max; probes are fixed samples or capped passes | known |
| F12 | Boundaries are guessed once and never narrowed: the histogram computes exact per-bucket counts but uses them only for offsets, never to re-balance | known; the regroup avenue (J16) is the fix |
| F13 | `n` is a `size_t`, the buffered engine counts in 32 bits: above 2³² − 1 elements the sort and partition entries hand the array to the strict in-place sort, which counts in `size_t` | read, not run: no machine here holds 34 GB of doubles |
| F14 | In this repository's records the C engine has been built and run only with GCC 13 on x86-64 Linux (WSL2): no other compiler, no 32-bit, no big-endian, no AArch64 | known gap |

## G. Repository state (facts about the tree, not the algorithm)

| # | fact |
|---|---|
| G1 | A second, hand-synchronised copy of CLAUDE.md sits one level up (`logos-sort-repo/CLAUDE.md`, outside git); the nested `logos-sort/` directory is the project. (2026-09-10: the root also held FINDINGS.md and README.md copies and an empty experiments/paradigm/.) |
| G2 | A git repository since 2026-09-14: five commits, one author, the last on 2026-11-06 carrying FINDINGS 3bd–3bw, the probes and `bizdev/`; working tree clean on 2026-11-06. |
| G3 | `LOGOS_PSORT_VERSION` is still 3.6.0 while CHANGELOG carries about thirty-five "(unreleased)" sections of shipped, A/B'd changes. Cutting 3.7.0 is a decision the author has not taken. |
| G4 | Build is POSIX (pthreads, make, g++ + OpenMP for the benches); on this Windows box that means WSL2. The AVX2 kernels are `#pragma GCC target` regions selected at run time (no `-march`); the AVX-512 kernels live only in the optional `logos_simd.cpp`. Built and run only with GCC 13 on x86-64 Linux (F14). |
| G5 | Opponents (x86-simd-sort, IPS⁴o + TBB, Highway / VQSort) are external clones under `engines/`, ignored by git; `make libvqsort.a` builds the VQSort objects without cmake. |

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

### J6. Bounded-displacement sort for jittered monotone data — ADDRESSED 2026-09-20 by another mechanism (D47)
> The d-sized heap was not built. The short-reach route sorts index blocks in place and repairs the seams; on this entry's
> shape (8.4M, local shuffles within 64) it measures **2.46× at one thread and 1.48× at 14** against the engine of the day,
> where this entry predicted about 1.85× at one thread. Detection is a ladder of sample lags, not the inversion distance.
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
