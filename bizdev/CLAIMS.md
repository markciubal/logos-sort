# CLAIMS: what the data supports, graded

Source: `reports/scale-report.html` measured 2026-11-05 (i9-13900H, 6P+8E cores, AVX2, GCC 13.3.0, WSL2; 14
benchmark threads; calibration memcpy-64MB 4.9 ms, std::sort-1M 116 ms = the box's usual loaded state). One
machine only, so the "both machines" half of the green rule cannot be applied; **green here means holds at 1M and
10M on this machine**, and every green row is provisional until a second machine (AVX-512 box, an ARM box) is run.
Amber: holds in one regime (one of the two sizes, or one thread count, or only above a size). Red: not supported.
Differences under 5% are level and are never a win. Memory is the peak traced heap of one cold call in bytes per
element (bytes asked for; resident memory is lower on the one-thread AVX2 path, `make rssprobe`).

Regenerate the rows with `python -I bizdev/tools/parse_report.py reports/scale-report.html rows.md rows.json`.

## Green (holds at 1M and 10M, one machine)

| id | claim | shape | threads | rival (its B/el) | 1M: logos / rival ms | 10M: logos / rival ms | ratio 1M / 10M | logos B/el | worst row 100k-10M |
|---|---|---|---|---|---|---|---|---|---|
| G1 | Nearly sorted doubles (1% random swaps) sort 3.4-3.7x faster than x86-simd-sort at one thread, with 0.4 B/el of scratch | near_sorted | 1 | x86-simd-sort AVX2 (0.0) | 4.06 / 13.94 | 45.0 / 164.4 | 3.43 / 3.65 | 0.4 | 1.17 at 316k |
| G2 | Quantized doubles (values on a lattice) sort 2.3-2.4x faster than x86-simd-sort at one thread with 0.0-0.2 B/el | quantized | 1 | x86-simd-sort AVX2 (0.0) | 2.89 / 6.73 | 27.4 / 66.3 | 2.33 / 2.42 | 0.2 / 0.0 | 2.14 at 100k |
| G3 | Few distinct values sort 1.2-1.5x faster than x86-simd-sort at one thread with 0 B/el | few_distinct | 1 | x86-simd-sort AVX2 (0.0) | 1.19 / 1.38 | 11.0 / 16.0 | 1.17 / 1.45 | 0.0 | 1.17 at 1M (thin) |
| G4 | Shuffled sorted blocks sort 2.1-3.2x faster than timsort (the best rival) at one thread, at 15.6 B/el against timsort's 3.2 | block_perm | 1 | timsort (3.2) | 2.01 / 4.26 | 21.3 / 68.6 | 2.12 / 3.23 | 15.6 | 1.46 at 100k |
| G5 | Overlapping sorted blocks: 2.4-3.1x over timsort, 15.6 vs 3.2 B/el | block_overlap | 1 | timsort (3.2) | 1.79 / 4.23 | 20.1 / 62.4 | 2.37 / 3.11 | 15.6 | 1.06 at 316k (level) |
| G6 | Blocks with every other one descending: 2.3-3.6x over timsort, 15.6 vs 2.6 B/el | block_twist | 1 | timsort (2.6) | 1.95 / 4.39 | 19.8 / 70.5 | 2.25 / 3.56 | 15.6 | 1.51 at 100k |
| G7 | Blocks with adjacent swaps inside: 1.8-2.6x over timsort, 16.1 vs 2.9-3.2 B/el | block_kinked | 1 | timsort (2.9-3.2) | 3.10 / 5.58 | 29.5 / 75.3 | 1.80 / 2.55 | 16.1 | 1.35 at 100k |
| G8 | Log lines (strings, near-ordered) sort 2.1x faster than x86-simd-sort's key-value sort on the same 8-byte prefix scheme, at LESS scratch (6.7 vs 8.0-8.4 B/el) | strings: logs | 1 | x86-simd-sort kv (8.4 / 8.0) | 35.1 / 72.7 | 412 / 863 | 2.07 / 2.09 | 6.7 | 1.90 at 3.16M |
| G9 | URL strings: 1.45x faster than VQSort's key-value sort at equal scratch (24 B/el both) | strings: urls | 1 | VQSort kv (24.0) | 114 / 166 | 1678 / 2464 | 1.45 / 1.47 | 24.0 | 1.25 (vs x86-simd-sort kv, 100k) |
| G10 | Uniform doubles at one thread: 1.3-1.5x faster than Google's VQSort (AVX2), the sort NumPy ships on ARM. (x86-simd-sort beats both; see R1) | uniform | 1 | VQSort AVX2 (0.0) | 15.0 / 19.7 | 144.6 / 212.9 | 1.31 / 1.47 | 16.1 | 1.31 at 1M |
| G11 | 14 threads, 12 numeric shapes: faster than the best parallel rival at both 1M and 10M, 9-17 B/el against IPS4o's 30 (1M) / 3 (10M) and x86-simd-sort's 0 | uniform, near_sorted, few_distinct, heavy_tail, bimodal, quantized, int_wide, block_perm, block_overlap, block_twist, sine_slope, reach_mirror | 14 | IPS4o (30.1 / 3.0) or x86-simd-sort OpenMP (0.0) | see 00-STATE 6.4 | see 00-STATE 6.4 | 1.07 (sine_slope 1M) to 6.13 (block_perm 10M) | 0.0-20.9 | LOSSES below 1M: 0.27-0.93 on 8 of 13 shapes at 100k |
| G12 | 14 threads, 3.16M and 10M: fastest sort in the report on all 13 numeric shapes, 1.07-7.15x | all 13 numeric | 14 | best of IPS4o, x86-simd-sort, gnu_parallel | n/a | 13 of 13 at both sizes | 1.07 (bimodal 10M) to 7.15 (block_overlap 3.16M) | 9.0-17.1 | block_kinked 1.05 at 1M |
| G13 | Log lines, 14 threads: 1.5-1.7x over IPS4o (strcmp), 26 B/el vs 30 / 3 | strings: logs | 14 | IPS4o (30.1 / 3.0) | 13.7 / 23.3 | 172 / 259 | 1.69 / 1.50 | 26.3 | 0.51 at 100k |
| G14 | Against scalar std::sort at one thread: 7.2-8.6x on uniform doubles, 7.4-8.4x on heavy_tail / bimodal / int_wide, 4.0-5.5x on near_sorted, 21-39x on few_distinct / quantized / block shapes, 4.7x on log lines (strcmp), 12x on uuids | all | 1 | std::sort (0.0) | uniform 15.0 / 108.5 | uniform 144.6 / 1242 | uniform 7.2 / 8.6; few_distinct 21.4 / 26.1; block_perm 31.3 / 39.1; near_sorted 4.0 / 5.5 | 0-16 | sine_slope 2.22 at 1M; reach_mirror 2.15 at 100k |
| G15 | Correctness envelope: NaN never hangs or crashes (output a permutation), infinities, subnormals, signed zeros, payload NaNs tested; 7,894 injected allocation and thread-creation failures with 0 wrong outputs; 54,832 property checks in 48 sections (2026-11-06) | n/a | n/a | basis: harness and faultprobe, not the scale report | | | | | |

## Amber (holds in one regime)

| id | claim | where it holds | where it fails | memory |
|---|---|---|---|---|
| A1 | Reversed short-reach disorder (reach_mirror) beats x86-simd-sort at one thread | 1.19 at 10M, 1.20 at 3.16M, 1.08 at 1M | 0.52 at 100k, 0.87 at 316k | 16.1 vs 0.0 |
| A2 | 14 threads beat IPS4o on block_kinked | 1.96 at 10M, 1.97 at 3.16M | 1.05 at 1M (level) | 9.0 vs 3.0 / 30.1 |
| A3 | 14 threads: fastest on every numeric shape from 1M up | 12 of 13 at 1M; 13 of 13 at 3.16M and 10M | block_kinked level at 1M; losses at 100k-316k on 5-8 of 13 | |
| A4 | URL strings beat x86-simd-sort's key-value sort at one thread (1.32 / 1.40) | speed holds at every n | the rival's memory column in the report is defective (1.8e14 B/el), so the memory half of the claim cannot be stated | logos 24.0 |
| A5 | UUID strings, 14 threads, beat x86-simd-sort kv | 1.37 at 10M, 1.66 at 3.16M | 0.93 at 1M, 0.51 at 100k | 26 vs 8 |
| A6 | Bimodal doubles at one thread beat x86-simd-sort | 1.26 at 1M | 0.87 at 3.16M, 0.95 at 10M, 0.84 at 100k: not monotone | 16.1 vs 0.0 |
| A7 | At 14 threads and 1M logos asks for LESS scratch than IPS4o (17 vs 30 B/el) | 1M and below (IPS4o carries a fixed ~30 MB) | 10M: IPS4o 3.0 vs logos 14 | |
| A8 | logos uses "near-zero memory" | few_distinct (0.0), quantized (0.0-0.5), near_sorted at 1 thread (0.4), the counting and lattice routes | the partition asks 14-17 B/el at 14 threads and 16 at one thread (resident under 0.1 on the AVX2 path); strings 24-26; block routes 9 (14T) / 15.6 (1T) | |
| A9 | The one-thread engine is level with x86-simd-sort on uniform doubles | 1.02 at 10M, 1.09 at 3.16M | 0.91 at 1M, 0.90 at 316k, 0.83 at 100k | 16.1 vs 0.0 |

## Red (not supported by the report)

| id | claim | data |
|---|---|---|
| R1 | Faster than x86-simd-sort at one thread on random / unstructured doubles | uniform 0.91 (1M) / 1.02 (10M); heavy_tail 0.95 / 0.88; int_wide 0.81 / 0.86; sine_slope 0.54 / 0.92 |
| R2 | Parallel logos beats IPS4o at every size | 14T, 100k: loses 8 of 13 shapes (0.27-0.72); 316k: loses 5 of 13 |
| R3 | Strings: words beat the key-value rivals | 1T 0.97 / 0.84; 14T 0.86 / 0.86 at 24-26 B/el vs 8 |
| R4 | Strings: uuids at one thread | 1.09 at 1M, 0.96 at 10M, 0.82 at 3.16M |
| R5 | The strict in-place variant is competitive | 2.7-32x slower than the best rival on all 130 numeric rows (ratios 0.03-0.38); 5-30x slower than logos's own buffered sort |
| R6 | Stable sorting | numeric sorts are not stable (PARITY.md:32); only argsort is |
| R7 | Safe to call from several threads at once | outside the contract; wrong output when tried (FINDINGS.md:1968) |
| R8 | Runs on ARM / Apple Silicon / Graviton; builds with clang or MSVC | never built (TRUTH_TABLE.md F14) |
| R9 | Sorts int64 / uint64 / records / (pointer, length) strings | not in the API |

## Do not claim

1. **"2x faster than SIMD sorts on random data."** Refuted: uniform at one thread is 0.91x at 1M and 1.02x at 10M
   against x86-simd-sort (AVX2); heavy_tail 0.88x, int_wide 0.86x at 10M. The 2x figures in the project's history
   are against scalar sorts or at 14 threads against a one-thread-style opponent, never against the vector sort.
2. **"Beats x86-simd-sort 2.5x on quantized data."** That is the AVX-512 box (README.md, FINDINGS §1), which has not
   been re-run. On this box against the AVX2 build it is 2.4x at one thread; say that, with the ISA.
3. **"Near-zero memory" as a property of the engine.** True for three routes; the partition asks 14-17 B/el.
4. **"Faster than IPS4o in parallel"** without a size: below 1M it is slower.
5. **Any absolute millisecond figure from this report** as a performance number; the box was in its loaded state
   (calibration 1.7-1.8x off quiet). Quote ratios inside a row.
6. **"4-14x faster than std::sort on strings"** as the headline. True against strcmp std::sort, but against the same
   prefix-key scheme on x86-simd-sort it is 0.84-1.40x, at 24 B/el against 8.
7. **The 14-thread ratios against IPS4o and gnu_parallel as exact.** Barrier-heavy opponents lose more to a loaded box
   than logos does (CLAUDE.md §1c/1d: IPS4o sped up 3.5x on a quiet box, logos 1.4x), and scalebench runs the OpenMP
   contenders under the default wait policy (scoreboard sets `OMP_WAIT_POLICY=PASSIVE`; scalebench does not, grep
   2026-11-06). Treat them as upper bounds until a quiet re-run.
8. **The paradigm numbers (quantiles 87x, top-k 7x, range-count 42x)** as product claims: they are experiments against
   an AVX-512 build on the old box, not in the scale report, and there is no shipped query API.
9. **"Zero changes that failed their A/B"** as a quality claim to a buyer: it is a process fact, not a result they can test.
10. **Cold-block performance at 100k-1M.** The report's rows at and above 10k sort one LLC-warm array repeatedly; a
    buyer's blocks arrive cold. No row measures that.

## Grading history

| date | change |
|---|---|
| 2026-11-06 | First grading, from the 2026-11-05 report. 15 green, 9 amber, 9 red. |
