# Feature parity: JavaScript vs C vs Rust

Three implementations of logos-sort exist, plus a Python reference. They are **not** equal. This is the matrix of what
each actually has, verified, on 2026-11-06. The C engine is the flagship and the only one that has moved since September
2026; `CHANGELOG.md` here and `../../FINDINGS.md` (3bd-3bw) record what it gained, and what was measured and not kept.

Legend: ✅ present & tested · 🟡 partial · ❌ absent

| capability | JS (`logos-sort`) | C (`logos-sort-c`) | Rust (`logos-sort-rs`) |
|---|---|---|---|
| **numeric dispatch depth** | full v3 | **the v3 dispatcher plus every route below** | 🟡 v2 (sample-sort + interpolation leaf) |
| prescan early-exit (sorted / reverse), in blocks with vector compares, in parallel past a 64K head | ✅ (scalar) | ✅ | ✅ (scalar) |
| the thin front: calls of up to 2047 elements answered before the dispatcher (vector prescan, kink windows, branchless Batcher networks for 4-16 elements) | 🟡 a budgeted small case | ✅ | ❌ |
| counting / int32 byte-skip radix | ✅ | ✅ | ✅ (i32 native) |
| low-cardinality counting: the sample's values in a verified perfect hash, up to 8 of them counted in vector registers, a write-only emit | 🟡 | ✅ | ❌ |
| lattice (direct-index) counting: the map in AVX2 with claimed chunks, and an entry for lattices whose sample shows no repeats (10k-250k values) with off-lattice values spilled and merged in | ❌ | ✅ | ❌ |
| hash counting for moderate cardinality (Chao1-gated) | ❌ | ✅ | ❌ |
| dense integers by presence (a bitmap, sequential) · a dominant value filled in around the rest · runs of equal values (the run-length route) | ❌ | ✅ (f64) | ❌ |
| run detection + galloping merge (2/4-way); the leaf's k-way merge in vectors | ✅ (scalar) | ✅ | ❌ |
| momentum insertion (displacement budget) | ✅ | ✅ | ❌ |
| gap-split (multimodal) | ✅ | ✅ | ❌ |
| flash / coarse bucket | ✅ | ✅ | ✅ (coarse) |
| skew probe → exponent-bit coarse | ✅ | ✅ | ❌ |
| quantized-float radix | ✅ | 🟡 built, OFF by default since 2026-10-20: it lost to the AVX2 kernel everywhere it fired (`LOGOS_QUANT_RADIX=1` restores it) | ❌ |
| float32 narrowing: doubles that are all exactly floats sorted on 4-byte keys, one thread | ❌ | ✅ | ❌ |
| the sequential unstructured leaf: an in-house AVX2 quicksort (x86-simd-sort's design, bridged to within 1.07-1.12x of the library's kernel), its profile pass fused into the kernel's first partition; the library's own kernel as an OPTIONAL leaf (`-DLOGOS_XSS_LEAF`) | ❌ (introsort) | ✅ | ❌ (`sort_unstable`) |
| dual-pivot introsort fallback | ✅ | ✅ | ✅ (`sort_unstable` leaf) |
| **parallel (multi-threaded)** | ❌ | ✅ | ✅ |
| the parallel partition: histogram-free with sample-sized regions and a spill list, a uniformity certificate in value and in key space, per-mode certificates and a mode grid, the key grid for heavy tails, direct-index splitter tables chosen by trial, 12 buckets per thread, two halves at two threads, the sequential leaf inside buckets past L2 | ❌ | ✅ | 🟡 sample sort with a histogram |
| write-combining / prefetch / pooling; a persistent worker pool; lazy contexts and scratch | ❌ | ✅ | ❌ |
| near-sorted index-partition parallel merge, with the kink gate (sparse kinks go to the detector) and the strand gate (in-order overlapping strands go to the run routes) | ❌ | ✅ | ❌ |
| disjoint-run relocation (J5) and the knot route (copy what lies alone, merge only where runs cross; vector two-strand merges, a galloping merge for a sparse strand over dense blocks, k-way merges in vectors) | ❌ | ✅ (f64) | ❌ |
| descending runs consumed as they lie, never reversed (the Moebius twist: orientation bit, backward copy, two-ended merge) | ❌ | ✅ (f64) | ❌ |
| a kink (one adjacent pair swapped inside a run) swapped back where it stands, at every thread count; strays inside strands lifted by the run detector and merged back in place of the copy-back | ❌ | ✅ (f64) | ❌ |
| one strand (a single run after kinks and strays are taken out) finished without a plan | ❌ | ✅ (f64) | ❌ |
| a whole array handed over back to front (mirrored short reach, prefix + tail, streams): one reversal, then the cheap route | ❌ | ✅ (f64, sequential) | ❌ |
| a ring buffer read from the wrong slot: one rotation (the loop-back); a NOISY ring buffer seen in the short-reach route's block medians: one rotation (the seam) | ❌ | ✅ (f64) | ❌ |
| a sorted prefix + unsorted tail; an unsorted head + sorted suffix (vector merge, gallop or the branchy loop, by the tail's size); interleaved ascending streams placed by a stable scatter | ❌ | ✅ (f64) | ❌ |
| disorder of short reach: index blocks sorted where they lie, seams repaired in place, strays shifted back (no n-sized buffer); windows in order wider than that (the cuts route) | ❌ | ✅ (f64) | ❌ |
| a wave with drift: pieces from the position-ordered sample, sorted where they lie, placed by the knot sweep | ❌ | ✅ (f64, sequential) | ❌ |
| ties continue a descending run (run detector; nothing is stable); the detector scans eight at a time with vector compares and resumes at a wrong-way step | ❌ | ✅ (f64 relocation detector; not the leaf's) | ❌ |
| the nearly-sorted probe tells dense short disorder from near-sortedness before an insertion sort is tried | ✅ from 4096 elements up; the small case also got the budget it never had (45,000 elements with 1% swaps: 47 ms → 2.3), guarded in `test/perf-guard.js` | ✅ | n/a (no such branch) |
| every sampler on Weyl positions (no aliasing with a periodic input); one shared sample answers every gate (distinct count, Chao1, inversion rate, KS, the reach ladder) | ❌ (strided) | ✅ | ❌ |
| **strict in-place mode (~1n)** | ✅ (`sortInplace`) | ✅, and in parallel (2.5-2.85x over its own one-thread form at 14 threads; still 6-24x slower than the buffered sort) | ✅ |
| many arrays in one call (`logos_sort_f64_batch`: parallel ACROSS segments, 4.2-9.6x over a loop at 14 threads) | ❌ | ✅ | ❌ |
| a lazy sorted view (`logos_view.h`: partition now, sort a bucket when something reads it; at, range, topk, rank, ranks, quantiles, count_in_range) | ❌ | ✅ (f64 and i32) | ❌ |
| **argSort (stable indirect sort)** | ✅ | ✅ | ✅ |
| apply / inverse permutation | ✅ | ✅ | ✅ |
| findOutliers | ✅ | ✅ | ✅ |
| **string sort** | ✅ the full JS engine (multikey + per-level LCP skip + packed-prefix radix + string run-merge) | ✅ `logos_sort_strings_mt`: 8-byte prefix keys through the numeric machinery (MSD radix with constant-byte skip at one thread, a sample sort at P, equal keys 8 bytes deeper, small groups on keys, a log-line route); 2.4-24x over `std::sort` with strcmp at one thread; against the same prefix scheme on x86-simd-sort's key-value sort 0.96-1.54 at one thread in stringbench (words level), and in the scale report at 10M log lines 2.09, urls 1.40, uuids 0.96, words 0.84. `logos_sort_strings` is the one-thread entry; the old multikey quicksort stays behind `LOGOS_STR_MKQS=1`. About 24 bytes per string of scratch | 🟡 multikey QS only |
| sortWithProxy (expensive comparator) | ✅ | ❌ | ❌ |
| WebAssembly build | ✅ (native JS) | ✅ (emscripten recipe) | ❌ |
| infinities and subnormals as ordinary values, tested (both broke the C engine until 2026-09-20: a value-linear map whose scale is `classes / range`, a design all three engines share) | 🟡 correct on all 126 edge runs; two quadratic cliffs in flash sort found and fixed (300k subnormals 77 s, a dense cluster 13–21 s), guarded in `test/perf-guard.js` | ✅ 522 checks | ✅ `tests/edge_values.rs`, 252 sorts |
| NaN: a tested floor (no crash, output a permutation of the input's bit patterns) and an entry with a defined order (`logos_sort_f64_nan_last`) | 🟡 one NaN between two clusters hung `sort()` forever (the gap-split partition); fixed and guarded in `test/perf-guard.js`. Order otherwise unspecified | ✅ | ❌ order unspecified, untested |
| a failed allocation or thread creation costs speed, never correctness (`make faultprobe`) | n/a (no threads; an allocation failure throws) | ✅ 7,894 injected failures, 0 bad | ❓ untested |
| memory measured both ways: ASKED FOR (`make memprobe`) and RESIDENT (`make rssprobe`, `make mixprobe`); `logos_release_buffers()` returns everything including the heap; `LOGOS_LOWMEM=1`, `LOGOS_TRIM`, `LOGOS_PART_INPLACE` trade speed for memory | `releaseBuffers` only | ✅ | ❌ |
| the test posture: the harness's property checks (54,832 in 48 sections on 2026-11-06, bit-multiset and `<`-order checked, a sample of the fault injection repeated in-process), `make sanitize` (ASan + UBSan + float-cast-overflow, leaks on), `make tsan`, three invariant probes (order must never cost; isomorphs cost the same; one element across a gate must not cost 10% more) | property suite + perf-cliff guards | ✅ | `cargo test` vs `sort_unstable` |
| callable from several threads at once | n/a (single-threaded) | ❌ one call at a time per process: the context pool, the bundle table and the worker pool are globals, and concurrent calls gave WRONG output when tried (FINDINGS 3at) | not examined |
| stability | `argSort` only | `argsort` only; the numeric and string sorts are not stable | `argsort` only |

## Reading this

- **C is the flagship.** It has everything the other two have except `sortWithProxy`, plus the parallel engine, the
  structure-reading routes, strings, the batch entry, the view and the measured memory story. Its gaps are the ones a
  buyer meets first, and `../../bizdev/GAPS.md` ranks them: f64 and i32 only (no i64, no record or key-payload sort),
  not stable, one call at a time per process, built only with GCC 13 on x86-64 Linux.
- **Rust is a v2 reference** with the same in-place / argSort / outliers / strings extras, but its numeric core is the
  older interpolation-leaf engine. It lacks the v3 dispatcher and every route added since; bringing it to parity is a
  multi-stage port, not started (open direction 6).
- **JS** is single-threaded, has the only `sortWithProxy` and its own string fast paths; its numeric engine is the one C
  started from and has not received the C routes.

## Not ported anywhere but JS

- The JS string engine's own fast paths (per-level LCP skip, packed-prefix 4-char radix, galloping string run-merge). C
  took a different route to strings (prefix keys through the numeric machinery): 2.4-24x over `std::sort` with strcmp,
  and against the same scheme on the vector sort ahead on log lines and urls, level or behind on words and uuids. Rust
  uses multikey quicksort.
- `sortWithProxy`.

## Not ported anywhere but C

Everything marked ✅ in the C column alone: the parallel partition and its certificates, the run routes, the counting
routes, the AVX2 leaf, strings on prefix keys, the batch entry, the view, the memory controls, the fault-injection and
invariant tests. That list is open direction 6 in `../../CLAUDE.md`.

## Versus the state of the art (AVX2 box: i9-13900H, 6 P + 8 E cores, GCC 13.3.0, WSL2)

One thread, 8.4M, `make scoreboard` on 2026-11-05 (minimum of 7 interleaved repetitions, every output verified), ns per
element:

| shape | logos | x86-simd-sort (AVX2) | VQSort (Highway 1.4.0, AVX2) |
|---|---|---|---|
| uniform, heavy_tail, bimodal, clustered | 9.0-9.6 | 9.1-9.3 | 12.6-13.3 |
| int_wide | 9.94 | 9.08 | 12.62 |
| 2 / few / 256 / 65,536 distinct values | 0.57 / 0.69 / 1.40 / 2.17 | 0.53 / 1.19 / 3.20 / 6.26 | 1.17 / 1.80 / 3.87 / 7.57 |
| quantized, near_sorted | 1.57, 3.69 | 4.50, 9.38 | 5.12, 12.99 |
| sorted, reversed | 0.30, 0.75 | 10.40, 9.66 | 12.01, 12.49 |
| block_perm, block_overlap | 1.24, 1.28 | 10.09, 10.05 | 12.54, 12.89 |

Reading: on unstructured doubles the one-thread engine is **level** with x86-simd-sort's AVX2 kernels (0.986-1.047 on
the in-house kernel, FINDINGS 3bj), except int_wide at 0.86-0.92 (the int32 radix against a vector quicksort), and it is
1.4-40x ahead wherever the data has structure the dispatcher reads. Google's VQSort is 1.36-1.42x behind x86-simd-sort
on this machine and reads no order (a sorted array costs it what random data does). The 2026-09 AVX-512 numbers (logos
at 0.26-0.43x of x86-simd-sort at one thread, before the AVX2 leaf existed) are FINDINGS §2; that box has not been
re-run, and AVX-512 kernels are 1.5-2x the AVX2 ones, so the one-thread picture there is unmeasured.

Fourteen threads (`reports/scale-report.html`, measured 2026-11-05; IPS4o given 14 threads, x86-simd-sort's OpenMP sort,
`__gnu_parallel::sort`): logos is the fastest sort measured on all 13 numeric inputs at 3.16M and 10M (1.07-7.15x over
the best rival), on 12 of 13 at 1M, and LOSES to IPS4o on 5-8 of 13 at 100k-316k, where the thread policy runs one
thread (one per 100,000 elements). Those 14-thread ratios were taken with the box in its usual loaded state, and
barrier-heavy rivals lose more to load than logos does, so they are upper bounds (CLAUDE.md §1c/1d).

Memory, both figures: the partition path ASKS for ~16 bytes per element at one thread and 14-17 at fourteen (what the
report's circles show) and keeps under 0.1 RESIDENT at one thread on the AVX2 path (`make rssprobe`); the counting
routes under 1, the block routes 9, strings ~24 per string; x86-simd-sort asks 0, IPS4o 3 at 10M and 30 at 1M (a fixed
buffer), timsort 3.

Fair benchmarking is single against single and multi against multi, never 1T against PT, and an interleaved OpenMP
contender needs `OMP_WAIT_POLICY=PASSIVE` or its spinning threads tax the next contender (scoreboard, stringbench and
primeprobe set it; scalebench does not yet).

## The router question, settled as far as one box can

The hybrid (`-DLOGOS_XSS_LEAF`, off by default because it is a C++ header dependency): logos decides, the library's
quicksort sorts what has no structure, as the sequential leaf and at up to two threads. Before the kernel bridge it was
worth 1.18-1.20x at one thread on unstructured shapes; after the bridge (the in-house kernel within 1.07-1.12x of the
library's, FINDINGS 3bh) and the profile phase folded into the kernel's first partition (3bi), logos with the library
leaf is level with the bare library call (0.991-1.010, checks included) and the in-house build is within ~1.05 of it.
So the "router" of open direction 2 is the shipped engine. Still owed: the partition at four threads and up against the
library's parallel quicksort, and `clustered` at every thread count.

## Can SIMD accelerate logos's own hot loops? (the scatter, no; the compute phases, yes)

The ledger's three SIMD negatives (a vectorized scatter, histogram and bucket index; `logos_simd.cpp` keeps the kernels
opt-in) were on memory-bound passes. What did pay was vectorising the COMPUTE phases: the prescan (1.7-2.2x on sorted
input), the run detector's block of eight (1.3x on kinks), the two-strand merge (2x on the kernel), the k-way merge
(2.1-2.6x), the lattice map (2.4-2.6x), the low-cardinality count (1.4-2x), and the leaf's quicksort itself. Vectorise
the pass that is bound by compares and loads; leave the one bound by per-core store latency alone.
