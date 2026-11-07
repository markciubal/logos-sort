# 00-STATE: what logos-sort is, commercially, on 2026-11-06

Written by the business-development assessor from the repository and the scale report measured 2026-11-05 21:47 UTC
(`reports/scale-report.html`, `META.measuredAt` 1793944050000; 34 panels, 170 rows at 100k and above, every row `ok`).
Every number below was regenerated from that report's DATA array with `bizdev/tools/parse_report.py`, not copied from prose.

## 1. The state in one paragraph

Four things block a sale or an integration today, and none of them is the algorithm. (1) The license is a custom,
non-OSI "Ethical Source" license that requires a paid commercial license for commercial use and carries use
restrictions that no Apache, MIT or BSD project can accept as a dependency; the commercial contact in it is a
placeholder. (2) The engine may not be called from two threads at once (globals: context pool, bundle table), and
every analytics engine sorts many blocks concurrently. (3) It has been built with one compiler on one machine
(GCC 13, x86-64 Linux under WSL2, an i9-13900H): no ARM, no Windows, no clang, no CI. (4) On the data buyers sort
most, random numeric keys at one thread, it is level with or behind Intel's x86-simd-sort while asking for 16 bytes
per element of scratch where the rival asks for none. What it has is real and narrow: at one thread it is 2.3-3.7x
faster than the best rival on nearly sorted, quantized and block-structured inputs and 2.1x on log lines, with
near-zero scratch on three of those; and with 14 threads it is the fastest sort in the report on all 13 numeric
shapes from 3.16M elements up (1.07-6.1x), at 9-17 bytes per element against IPS4o's 3 and x86-simd-sort's 0.

## 2. API surface

| Item | C (flagship, `engines/c/logos_psort.h`, version string 3.6.0) | JavaScript (npm `logos-sort` 0.3.1) | Rust (`engines/rust`, 0.3.0) | Python |
|---|---|---|---|---|
| Sort entry points | `logos_sort_f64`, `logos_sort_i32`, `logos_sort_f64_batch` (many arrays, one call), `logos_sort_f64_nan_last`, `logos_sort_f64_inplace`, `logos_sort_i32_inplace`, `logos_partition_f64/i32` + `logos_sort_buckets_*` (lazy sorted view, `logos_view.h`) | `sort`, `sortInplace`, `sortF64`, `sortI32`, `sortWithProxy` | `sort_f64`, `sort_i32` (via f64), `*_inplace` | `logos_sort`, `logos_sort_inplace` (pure Python, reference only) |
| Element types | f64, i32. No i64, u64, f32 (f32 is narrowed internally only), no generic comparator, no record/struct sort | number[], string[], Float64Array, Int32Array | f64, i32, `&str` | any comparable list |
| Key plus payload | Only through `logos_argsort_f64/i32` (stable, uint32 indices, n < 2^32) + `logos_apply_perm_*`. No sort of (key, rowid) pairs in one pass; argsort speed is not in the scale report | `argSort`, `sortWithIndices`, `applyPermutation` | `argsort_*` + `apply_permutation` | `key=` |
| Strings | `logos_sort_strings(const char **)` / `logos_sort_strings_mt`: NUL-terminated C strings only, strcmp order, ~24 bytes per string of scratch (measured 24-26 in the report). No (pointer, length) form, no collation | full string engine (JS-only) | `sort_strings(&mut [&str])` | n/a |
| Stability | numeric sorts NOT stable ("nothing is stable", PARITY.md:32); argsort stable; equal strings in any order | `argSort` stable only | argsort stable only | with `key=` only |
| Thread model | pthreads, caller passes `nthreads`; persistent worker pool kept between calls; thread policy runs ONE thread below 100,000 elements per thread. **One call at a time per process**: the context pool and bundle table are globals (header; FINDINGS.md:1968) | none | `std::thread::scope` | none |
| NaN contract | default entry: returns, no crash, output is a permutation, order unspecified; `_nan_last` gives NaNs-at-end | partial | none | n/a |
| Memory controls | `logos_release_buffers()`; env switches `LOGOS_LOWMEM=1`, `LOGOS_TRIM=1\|hard`, `LOGOS_PART_INPLACE=1`; no scratch-budget parameter in the API | `releaseBuffers` | n/a | n/a |
| In-place variant | `logos_sort_f64_inplace` / `_i32_inplace`: 0 bytes heap at 1 thread, 96 KB task stack in parallel; parallel from 2^20 | `sortInplace` | yes | yes |
| Diagnostics | `logos_twist_last(which)`, `logos_reach_last()`, `logos_mirror_count()`, profiling under `-DLOGOS_PROFILE` | | | |
| Size limit | buffered engine counts in 32 bits; above 2^32-1 elements the strict in-place sort runs (untested at that size) | | | |

Source for the non-C columns: `engines/js/index.d.ts`, `engines/rust/src/lib.rs`, `engines/python/logos_sort.py`,
`engines/c/PARITY.md`. The JS native addon builds a separate single-file engine (`engines/js/c/sort.c`), not the flagship.

## 3. Build and portability

| Item | Fact | Where |
|---|---|---|
| Compiler | GCC 13.3.0 only; never built with clang, MSVC or any other compiler | Makefile; TRUTH_TABLE.md F14; META.compiler |
| Flags | `-O3 -std=gnu11 -pthread -fPIC`, no `-march`, no `-mavx2` | Makefile:14 |
| ISA | x86-64. AVX2+FMA kernels compiled under `#pragma GCC target` and selected at run time with `__builtin_cpu_supports`; SSE2 fallback on x86-64 without AVX2; scalar elsewhere. AVX-512 kernels exist only in the optional `logos_simd.cpp` | logos_psort.c:44-61, 832 |
| ARM / NEON / SVE | Never built, no code path. "no other compiler, no 32-bit, no big-endian, no AArch64" | TRUTH_TABLE.md:344; CHANGELOG.md:3025 |
| Windows | Needs pthreads and `unistd.h`; does not build under MSVC as written | logos_psort.c:39-40 |
| Dependencies | pthreads, libm; `madvise` on Linux, `malloc_trim` on glibc. x86-simd-sort, IPS4o, Highway are benchmark opponents only; `-DLOGOS_XSS_LEAF` is an opt-in build that links x86-simd-sort as the leaf | Makefile:844-857 |
| CI | none (no `.github`, no CI yaml) | repository |
| Benchmark machine | i9-13900H (6 P-cores + 8 E-cores, 20 hardware threads, AVX2, no AVX-512), 24 MB LLC, WSL2 Ubuntu, GCC 13.3.0, 14 benchmark threads. Calibration on the 2026-11-05 run: memcpy-64MB 4.9 ms, std::sort-1M 116 ms, which CLAUDE.md classes as the box's usual loaded state inside a run (quiet: 2.9 / 65). An older 20-core AVX-512 box produced the numbers in README.md and FINDINGS §1; it has not been re-run | META; CLAUDE.md methodology 2 |

## 4. Repository facts

| Item | Fact |
|---|---|
| License | `engines/c/LICENSE` and `engines/js/LICENSE` (identical): "Logos Sort Ethical Source License, Version 1.1", copyright "(c) 2026" with no holder named. Not OSI-approved; commercial use needs a separate written license; the contact line reads `<commercial-licensing contact to be provided by the author>`; Section 5(c) requires forks to say they derive from "LogosAdaptive Sort" (a name used nowhere else). Prohibited uses include military, surveillance, some human-genetics work and more. `engines/rust` and `engines/python` carry no LICENSE file and `Cargo.toml` has no `license` field. npm lists `SEE LICENSE IN LICENSE`. |
| First public disclosure found | npm package `logos-sort` 0.1.0 published 2026-05-28 (0.1.1 same day, 0.1.2 2026-05-29, 0.2.0/0.2.1 2026-06-16, 0.3.0/0.3.1 2026-06-23). That package is the JavaScript engine with its CHANGELOG, README and the single-file C engine `c/sort.c`. Homepage `codecookbook.org` (resolves). |
| GitHub | `origin` is `github.com/markciubal/logos-sort`; an anonymous fetch of that URL returns 404 on 2026-11-06, so the repository is private, renamed or absent. If private, the C flagship (knot route, lattice entry, string prefix keys, memory rounds) has NOT been publicly disclosed yet. Question 4 in QUESTIONS.md. |
| Git history | 5 commits, one author (Mark Ciubal), first 2026-09-14 "Base code", last 2026-11-06 ("Mobius DLL implementation, other optimizations."), which committed the two months of work that had been sitting uncommitted (the probes under `engines/c/`, FINDINGS 3bd-3bw, this folder). Working tree clean on 2026-11-06. |
| Contributors | One human. `engines/js/package.json` lists the author as "Claude Opus 4.6, 4.7, 4.8, Mark Ciubal", and README.md addresses "a Claude instance continuing this work": the record states that an AI system co-wrote the code. That matters for copyright (and so for the license's enforceability) and for patent inventorship; see IP.md. |
| University resources / coursework | No mention anywhere in the repository (grep for university, coursework, thesis, professor). Treated as "none" until the author answers question 6. |
| Prior public writing | None found in the repository (no blog post, preprint or talk is referenced). The npm README is the only public prose. |

## 5. Test posture

| Item | Fact | Where |
|---|---|---|
| Correctness | `make test`: 48 harness sections, 54,832 property checks on 2026-11-06 (the resource section's count moves with thread timing) against libc qsort plus bitwise-multiset and `<`-order checks (qsort alone misreads signed zeros); every bench verifies its output before timing | harness.c; CLAUDE.md methodology 1 |
| Sanitizers | `make sanitize` (ASan, UBSan, float-cast-overflow, leak checking on), `make tsan` | Makefile:227-232 |
| Fault injection | `make faultprobe`: every malloc/calloc/realloc/posix_memalign and pthread_create of a cold call failed in turn, in forked children; 7,894 injected failures, 0 bad; a sample repeats in-process on every `make test` | FINDINGS §3ae |
| Adversarial inputs for the detector | harness sections for signed zeros, infinities, IEEE specials (subnormals, payload NaNs), "NaN termination" (must return), int32 sentinels, a sawtooth that aliases the sample's lag, short-reach mutations; three invariant probes (`orderprobe`: order must never cost; `symprobe`: isomorphs cost the same; `edgeprobe`: one element across a gate must not cost 10% more) | harness.c; FINDINGS 3bn |
| Randomized / fuzz testing | None. All harness RNGs are fixed-seed xorshift; no seed override; no fuzz target | harness.c:24-27 |
| Worst-case bound | None published. Routes that probe and decline cost a measured overhead on the shapes tried (e.g. a 2.8 ms declined scan at 8.4M), but no adversarial search for the worst input has been run | CLAUDE.md ledger |
| Concurrency safety | `logos_sort_f64` from two threads at once is outside the contract and was found to produce WRONG output when tried (FINDINGS.md:1968) | |

## 6. The measured picture (scale report of 2026-11-05)

How to read the rows. "Rival" is the fastest contender other than logos and logos in-place in that row. Contenders at
1 thread: std::sort, x86-simd-sort (AVX2 build), IPS4o, LSD radix, timsort, VQSort (Highway 1.4.0, AVX2); at 14
threads: `__gnu_parallel::sort`, x86-simd-sort (OpenMP), IPS4o (given 14 threads). Memory is the peak traced heap of
one cold call in bytes per element (what was ASKED for, not what was resident; the engine's one-thread AVX2 path
asks for 16 and touches under 0.1, `make rssprobe`). At 10k elements and above a point is one array sorted
repeatedly with the minimum kept, so a 1M row (8 MB) is LLC-warm; buyers' blocks arrive cold. Ratios inside a row
are the trustworthy quantity; absolute milliseconds are from a loaded box. Differences under 5% are "level".

### 6.1 Seeds from the brief, confirmed or corrected (1 thread, 10M unless stated)

| Seed | Verdict | Data |
|---|---|---|
| Group A: near_sorted 3.65x at 0.4 B/el | confirmed | 45.0 ms vs x86-simd-sort 164.4 ms; 0.4 vs 0.0 B/el. At 1M: 3.43x |
| few_distinct 1.45x at 0 | confirmed | 11.0 vs 16.0 ms; 0.0 vs 0.0. At 1M: 1.17x (the thin end) |
| quantized 2.42x at 0.2 | confirmed on speed; memory is 0.0 at 10M, 0.2 at 1M | 27.4 vs 66.3 ms. At 1M: 2.33x at 0.2 B/el |
| strings: logs 2.09x at 6.7 vs 8 | confirmed | 412 vs 862 ms (x86-simd-sort key-value on 8-byte prefixes); 6.7 vs 8.0 B/el. At 1M: 2.07x, 6.7 vs 8.4 |
| Group B: block shapes 2.6-3.6x at 15.6 vs timsort 3 | confirmed; block_kinked is 2.55x at 16.1 | perm 3.23, overlap 3.11, twist 3.56, kinked 2.55; logos 15.6-16.1 vs timsort 2.6-3.2 B/el. At 1M: 2.12 / 2.37 / 2.25 / 1.80 |
| Group C: uniform 1.02x at 10M, 0.91x at 1M | confirmed | 144.6 vs 147.4 ms (level); 15.0 vs 13.7 ms at 1M; 16.1 vs 0.0 B/el |
| heavy_tail 0.88x, bimodal 0.95x, int_wide 0.86x | confirmed | 160.9/142.0; 165.8/158.1 (level); 181.6/155.4 |
| sine_slope 0.92x at 10M, 0.54x at 1M | confirmed | 173.6/158.9; 22.2/12.0 |
| strings: words 0.84x | confirmed | 535 vs 449 ms; 24 vs 8.1 B/el |
| rivals at ~0 B/el, logos strings at 24 | confirmed; x86-simd-sort's memory column on `strings: urls` is a report DEFECT (reads 1.8e14 B/el) | see 6.6 |
| Parallel 14T: every numeric shape 1.07-6.1x at 9-17 B/el | confirmed at 10M (13 of 13) and at 3.16M (13 of 13); at 1M 12 of 13 (block_kinked 1.05, level); BELOW 1M logos LOSES on 5 of 13 shapes at 316k and 8 of 13 at 100k | 6.4 |
| IPS4o 3 B/el, gnu_parallel 8, LSD radix 16 | IPS4o is 3.0 at 10M but 30.1 at 1M and 300.6 at 100k (a fixed ~30 MB); gnu_parallel 8.0; LSD radix 16.0; timsort 2.6-4.0 | |
| In-place variant 4-12x slower than the best rival | too kind: **2.7x to 32x slower** (ratios 0.03-0.38 on all 130 numeric rows) | 6.5 |

### 6.2 One thread, where logos wins (rival/logos, memory both sides)

| shape | 1M | 10M | logos B/el | rival B/el | rival | worst row 100k-10M |
|---|---|---|---|---|---|---|
| near_sorted | 3.43 | 3.65 | 0.4 | 0.0 | x86-simd-sort | 1.17 at 316k |
| quantized | 2.33 | 2.42 | 0.2 / 0.0 | 0.0 | x86-simd-sort | 2.14 at 100k |
| few_distinct | 1.17 | 1.45 | 0.0 | 0.0 | x86-simd-sort | 1.17 at 1M |
| block_perm | 2.12 | 3.23 | 15.6 | 3.2 | timsort | 1.46 at 100k |
| block_overlap | 2.37 | 3.11 | 15.6 | 3.2 | timsort | 1.06 at 316k |
| block_twist | 2.25 | 3.56 | 15.6 | 2.6 | timsort | 1.51 at 100k |
| block_kinked | 1.80 | 2.55 | 16.1 | 2.9-3.2 | timsort | 1.35 at 100k |
| reach_mirror | 1.08 | 1.19 | 16.1 | 0.0 | x86-simd-sort | 0.52 at 100k (a loss) |
| strings: logs | 2.07 | 2.09 | 6.7 | 8.4 / 8.0 | x86-simd-sort kv | 1.90 at 3.16M |
| strings: urls | 1.32 | 1.40 | 24.0 | defective column (VQSort kv at 24.0 is 1.45-1.47 behind) | x86-simd-sort kv | 1.25 at 100k |

### 6.3 One thread, where logos is level or loses

| shape | 1M | 10M | logos B/el | rival B/el | rival | note |
|---|---|---|---|---|---|---|
| uniform | 0.91 | 1.02 | 16.1 | 0.0 | x86-simd-sort | 0.83 at 100k |
| heavy_tail | 0.95 | 0.88 | 16.1 | 0.0 | x86-simd-sort | 0.75 at 100k |
| bimodal | 1.26 | 0.95 | 16.1 | 0.0 | x86-simd-sort | 0.84 at 100k, 0.87 at 3.16M: not monotone, so no claim |
| int_wide | 0.81 | 0.86 | 16.1 | 0.0 | x86-simd-sort / VQSort | loses at every n |
| sine_slope | 0.54 | 0.92 | 16.1 | 0.0 | x86-simd-sort | the worst numeric row in the report |
| strings: uuids | 1.09 | 0.96 | 24.0 | 8.0 | x86-simd-sort kv | 0.82 at 3.16M |
| strings: words | 0.97 | 0.84 | 24.0 | 8.4 / 8.1 | x86-simd-sort kv | loses from 3.16M |

Per element at 10M, one thread, uniform: logos 14.5 ns, x86-simd-sort 14.7, VQSort 21.3, IPS4o 53, std::sort 124.

### 6.4 Fourteen threads (rival/logos; logos B/el; rival and its B/el)

| shape | 100k | 316k | 1M | 3.16M | 10M | logos B/el at 1M / 10M | rival at 1M / 10M |
|---|---|---|---|---|---|---|---|
| uniform | 0.44 | 0.62 | 2.36 | 1.74 | 1.51 | 16.9 / 13.8 | IPS4o 30.1 / x86-simd-sort 0.0 |
| near_sorted | 0.72 | 0.93 | 1.35 | 3.47 | 1.92 | 16.9 / 17.1 | IPS4o 30.1 / 3.0 |
| few_distinct | 1.29 | 1.29 | 1.19 | 6.40 | 3.04 | 0.1 / 0.0 | IPS4o 30.1 / 3.0 |
| heavy_tail | 0.67 | 1.01 | 1.37 | 2.15 | 1.78 | 17.0 / 14.1 | IPS4o 30.1 / x86-simd-sort 0.0 |
| bimodal | 0.33 | 0.43 | 1.67 | 1.29 | 1.07 | 16.9 / 13.8 | IPS4o 30.1 / x86-simd-sort 0.0 |
| quantized | 0.41 | 0.79 | 1.72 | 6.36 | 4.42 | 0.5 / 0.2 | IPS4o 30.1 / 3.0 |
| int_wide | 0.40 | 1.90 | 1.60 | 1.40 | 1.33 | 16.6 / 13.7 | IPS4o 30.1 / x86-simd-sort 0.0 |
| block_perm | 4.31 | 4.31 | 2.65 | 3.20 | 6.13 | 9.0 / 9.0 | IPS4o 30.1 / 3.0 |
| block_overlap | 1.57 | 1.71 | 2.60 | 7.15 | 4.83 | 9.0 / 9.0 | IPS4o 30.1 / 3.0 |
| block_twist | 2.93 | 1.74 | 1.88 | 5.35 | 4.27 | 9.0 / 9.0 | IPS4o 30.1 / x86-simd-sort 0.0 |
| block_kinked | 1.51 | 1.40 | 1.05 | 1.97 | 1.96 | 9.0 / 9.0 | IPS4o 30.1 / 3.0 |
| sine_slope | 0.27 | 0.58 | 1.07 | 1.28 | 1.39 | 20.9 / 13.8 | IPS4o 30.1 / x86-simd-sort 0.0 |
| reach_mirror | 0.49 | 1.27 | 1.97 | 2.23 | 2.63 | 17.4 / 13.8 | IPS4o 30.1 / x86-simd-sort 0.0 |
| strings: uuids | 0.51 | 1.46 | 0.93 | 1.66 | 1.37 | 26.3 / 26.0 | x86-simd-sort kv 8.0 |
| strings: words | 0.94 | 1.10 | 0.86 | 0.88 | 0.86 | 26.3 / 26.1 | x86-simd-sort kv 8.4 / IPS4o 3.0 |
| strings: urls | 0.76 | 1.13 | 1.02 | 0.98 | 1.03 | 26.3 / 26.0 | IPS4o 30.1 / 3.0 |
| strings: logs | 0.51 | 1.10 | 1.69 | 1.04 | 1.50 | 26.3 / 26.0 | IPS4o 30.1 / 3.0 |

Why the left columns lose: the thread policy runs one thread below 100,000 elements per thread, so a 14-thread
request on 100k-316k elements is a one-thread logos sort against a 14-thread IPS4o (with 30-300 B/el of fixed
buffers). The per-route caps (CLAUDE.md, FINDINGS §3k) were set at 1M-3M. For a buyer whose unit of work is a
100k-1M block this is the regime that matters, and the parallel claim does not hold there. Two caveats on the right
columns: IPS4o and `__gnu_parallel::sort` are barrier-heavy and lose more than logos does to a loaded box
(CLAUDE.md §1c/1d: on a quiet box IPS4o sped up 3.5x against logos's 1.4x), so the 14-thread ratios are upper
bounds until re-run quiet; and the x86-simd-sort parallel column is its OpenMP quicksort under
`OMP_WAIT_POLICY` defaults inside scalebench, which the scoreboard found to need PASSIVE (ledger).

### 6.5 The strict in-place variant

Against the best rival in the same row it reads 0.03-0.38 on every numeric row (2.7x to 32x slower): uniform
0.14-0.22, block shapes 0.06-0.33, few_distinct 0.03-0.11. Against logos's own buffered sort it is 5-30x slower.
It buys 0 bytes of heap. It is not a product; it is the fallback the resource-exhaustion contract needs.

### 6.6 Defects and gaps in the report itself (for the harness backlog)

- `strings: urls`, x86-simd-sort key-value series: peak heap reads 1.8e14 bytes per element at every n. A tracer
  overflow or an unmatched free in the memory pass. Until fixed the urls memory comparison uses VQSort kv (24.0).
- No key-value (payload) numeric row, no int64/uint64 row, no cold-cache measurement, no second machine, no
  fluxsort/crumsort/glidesort/driftsort/ipnsort contenders, no parallel radix sort, no stable sort besides timsort.
- IPS4o's fixed ~30 MB buffer makes its B/el column size-dependent (300 at 100k, 30 at 1M, 3 at 10M); a reader
  comparing memory at 1M must know that.

## 7. What is strong, stated once without adjectives

- The measurement discipline (paired A/B in one binary, copy swap to cancel layout bias, verify before timing,
  negatives recorded) is unusual and is itself a publishable asset (PAPER.md).
- Three invariant tests (order must never cost; isomorphs cost the same; one element across a gate costs under
  10%) and a fault-injection probe that fails every allocation and thread creation: a buyer's due-diligence
  questions are already answered with numbers.
- The structured-data routes (near-sorted, quantized/lattice, block relocation and knot, run-length, string log
  lines) win 2-7x at one thread against the strongest vector sort available on AVX2 hardware, three of them with
  near-zero scratch, and the 14-thread engine is the fastest in the report on every numeric shape from 3.16M.
- The lazy sorted view (`logos_view.h`) and the paradigm experiments (quantiles, top-k, range counts without a
  sort) point at an analytics API where the no-scatter model wins by multiples; it is a direction, not a product.

## 8. Open questions for the author

See `bizdev/QUESTIONS.md`. The three that gate everything else: what outcome is wanted (job, consulting, license
revenue, paper, all); whether the GitHub repository is private and what, beyond the npm package, has ever been
public; and whether the license will be changed to an OSI license for some or all of the tree.
