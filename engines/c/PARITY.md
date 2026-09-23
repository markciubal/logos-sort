# Feature parity: JavaScript vs C vs Rust

Three implementations of logos-sort exist. They are **not** equal. This is the
honest matrix — what each actually has, verified, today.

Legend: ✅ present & tested · 🟡 partial · ❌ absent

| capability | JS (`logos-sort`) | C (`logos-sort-c`) | Rust (`logos-sort-rs`) |
|---|---|---|---|
| **numeric dispatch depth** | full v3 | **full v3** | 🟡 v2 (sample-sort + interpolation leaf) |
| prescan early-exit (sorted/reverse) | ✅ | ✅ | ✅ |
| counting / int32 byte-skip radix | ✅ | ✅ | ✅ (i32 native) |
| run detection + galloping merge (2/4-way) | ✅ | ✅ | ❌ |
| momentum insertion (displacement budget) | ✅ | ✅ | ❌ |
| gap-split (multimodal) | ✅ | ✅ | ❌ |
| flash / coarse bucket | ✅ | ✅ | ✅ (coarse) |
| skew probe → exponent-bit coarse | ✅ | ✅ | ❌ |
| quantized-float radix | ✅ | ✅ | ❌ |
| low-/moderate-cardinality counting, lattice | 🟡 | ✅ | ❌ |
| dual-pivot introsort fallback | ✅ | ✅ | ✅ (`sort_unstable` leaf) |
| **parallel (multi-threaded)** | ❌ | ✅ | ✅ |
| write-combining / prefetch / pooling | ❌ | ✅ | ❌ |
| near-sorted index-partition parallel merge | ❌ | ✅ | ❌ |
| disjoint-run relocation (J5) and the knot route (copy what lies alone, merge only where runs cross) | ❌ | ✅ (f64) | ❌ |
| descending runs consumed as they lie, never reversed (the Moebius twist: orientation bit, backward copy, two-ended merge) | ❌ | ✅ (f64) | ❌ |
| a whole array handed over back to front (mirrored short reach, prefix + tail, streams): one reversal, then the cheap route | ❌ | ✅ (f64) | ❌ |
| a ring buffer read from the wrong slot (an ascending suffix that continues into the prefix across the wrap): one rotation | ❌ | ✅ (f64) | ❌ |
| a noisy ring buffer (one drop in the short-reach route's block medians): one rotation, then that route | ❌ | ✅ (f64, sequential) | ❌ |
| strays inside strands lifted by the run detector, merged back in place of the copy-back | ❌ | ✅ (f64, sequential) | ❌ |
| one strand (a single run after kinks and strays are taken out) finished without a plan | ❌ | ✅ (f64) | ❌ |
| the whole-array prescan in blocks of sixteen | ❌ | ✅ (f64) | ❌ |
| ties continue a descending run (run detector; nothing is stable) | ❌ | ✅ (f64 relocation detector; not the leaf's) | ❌ |
| the prescan's blocks compared with vectors (AVX2, SSE2; the scalar OR elsewhere) | ❌ | ✅ (f64) | ❌ |
| the run detector resumes at a wrong-way step (blocks of eight) | ❌ | ✅ (f64 relocation detector) | ❌ |
| the run detector's block of eight compared with vectors (SSE2, AVX2) | ❌ | ✅ (f64 relocation detector) | ❌ |
| a wave with drift: pieces from the position-ordered sample, sorted where they lie, placed by the knot sweep | ❌ | ✅ (f64, sequential) | ❌ |
| short-reach route (index blocks sorted where they lie, seams repaired in place, strays shifted back; no n-sized buffer) | ❌ | ✅ (f64) | ❌ |
| the nearly-sorted probe tells dense short disorder from near-sortedness before an insertion sort is tried | ✅ from 4096 elements up; the small case also got the budget it never had (45,000 elements with 1% swaps: 47 ms → 2.3), guarded in `test/perf-guard.js` | ✅ | n/a (no such branch) |
| **strict in-place mode (~1n)** | ✅ (`sortInplace`) | ✅ | ✅ |
| **argSort (stable indirect sort)** | ✅ | ✅ | ✅ |
| apply / inverse permutation | ✅ | ✅ | ✅ |
| findOutliers | ✅ | ✅ | ✅ |
| **string sort** | ✅ full (multikey + LCP + packed + run-merge) | 🟡 multikey QS only | 🟡 multikey QS only |
| sortWithProxy (expensive comparator) | ✅ | ❌ | ❌ |
| WebAssembly build | ✅ (native JS) | ✅ (emscripten recipe) | ❌ |
| infinities and subnormals as ordinary values, tested (both broke the C engine until 2026-09-20: a value-linear map whose scale is `classes / range`, a design all three engines share) | 🟡 correct on all 126 edge runs; two quadratic cliffs in flash sort found and fixed (300k subnormals 77 s, a dense cluster 13–21 s), guarded in `test/perf-guard.js` | ✅ 522 checks | ✅ `tests/edge_values.rs`, 252 sorts |
| NaN: a tested floor (no crash, output a permutation of the input's bit patterns) and an entry with a defined order (`logos_sort_f64_nan_last`) | 🟡 one NaN between two clusters hung `sort()` forever (the gap-split partition); fixed and guarded in `test/perf-guard.js`. Order otherwise unspecified | ✅ | ❌ order unspecified, untested |
| a failed allocation or thread creation costs speed, never correctness (`make faultprobe`) | n/a (no threads; an allocation failure throws) | ✅ 7,008 injected failures, 0 bad | ❓ untested |
| measured memory per element (`make memprobe`) | ❌ | ✅ | ❌ |
| strict in-place sort runs in parallel | ❌ | ✅ (2.5–2.85× at 14 threads on random data) | ❌ |

## Reading this

- **C is the flagship.** It has everything JS has on the numeric side and more
  (parallel, in-place, pooling, lattice), plus the argSort family, findOutliers,
  and a correct-but-basic string sort. The gaps are the *advanced* string paths
  and `sortWithProxy`.
- **Rust is a v2 reference** with the same in-place/argSort/outliers/strings
  extras, but its numeric core is the older interpolation-leaf engine — it lacks
  the v3 dispatcher (run-merge, momentum, skew/quant paths, lattice) and the
  parallel machinery beyond the sample-sort layer. Bringing Rust to full v3
  parity is a multi-stage port, not yet done.
- **JS** is single-threaded but has the most complete *string* engine and the
  only `sortWithProxy`.

## Not ported anywhere but JS

- The full string engine's fast paths (per-level LCP skip, packed-prefix
  4-char radix, galloping string run-merge). C and Rust use multikey quicksort,
  which is correct and decent but not those.
- `sortWithProxy` (two-tier sort minimizing expensive-comparator calls).


## Versus the state of the art (single-threaded, AVX-512 CPU, 8M, measured)

`make parbench-simd` (needs Intel x86-simd-sort + an AVX-512 CPU) adds a column
for **x86-simd-sort**, the vectorized-quicksort SIMD state of the art (NumPy's
sort kernel). Result: logos-sort **loses on every shape single-threaded**.

| shape | logos 1T | x86-simd-sort 1T | logos/simd |
|---|---|---|---|
| uniform | 188 ms | 60 ms | 0.32x |
| heavy_tail | 223 ms | 57 ms | 0.26x |
| bimodal | 232 ms | 64 ms | 0.28x |
| int_wide | 134 ms | 58 ms | 0.43x |
| near_sorted | 63 ms | 57 ms | 0.90x |
| quantized | 40 ms | 35 ms | 0.86x |
| few_distinct | 28 ms | 8 ms | 0.30x |

Honest reading: a vectorized quicksort accelerates the *partition* (`vpcompress`),
which SIMD is superb at and logos-sort's scatter cannot match. logos-sort is
closest exactly where it avoids comparison work (near-sorted, quantized) and
worst on comparison-friendly random data. Its edges that survive: **parallel
thread scaling** (x86-simd-sort is single-threaded) and shapes SIMD can't
exploit. It is not competitive with SIMD sorts on raw single-thread numeric
throughput, and this file says so rather than letting the parallel "8-0-0 vs
__gnu_parallel::sort" number stand unqualified.


## Fair benchmarking (single vs single, multi vs multi)

MULTI now includes **IPS4o** (SOTA parallel comparison sort) via `make
parbench-full` -- so the parallel section covers scalar-MIMD (gnu_par, IPS4o),
SIMD+MIMD (x86-simd-sort MT), and logos (+ its SIMD-leaf variant). MIMD note:
logos-sort is already a MIMD sort (threads); the ceiling on one socket is memory
bandwidth, shared by all cores -- adding MIMD cleverness cannot move it. The
lever beyond is NUMA (more memory controllers) or SIMT (GPU, where scatter is
cheap and logos's addressing paradigm finally wins).

parbench compares single-threaded sorts only to each other and multi-threaded
only to each other, never 1T vs PT. SINGLE: std::sort, logos 1T, x86-simd-sort
1T. MULTI (P threads): __gnu_parallel::sort, logos, logos+SIMD-leaf, and
x86-simd-sort's own OpenMP sort. Build all with: make parbench-simd.

Full-wiring result, single-threaded (8M, AVX-512): logos+SIMD reaches
0.72x-0.98x of x86-simd-sort (heavy_tail 0.94x, bimodal 0.98x near parity;
uniform 0.79x, int_wide 0.72x, quantized 0.75x). Parity, never beyond, at 1
thread; the only path to a win is parallel-SIMD on many cores.

## The SIMD-leaf "pivot" (measured)

Rather than beat the SIMD sort, logos-sort can *use* it: `logos_set_simd_sort`
registers a vectorized leaf (`logos_simd.cpp` wires Intel x86-simd-sort), and the
parallel per-bucket sort then runs **parallel-SIMD** (logos partition + AVX-512
quicksort per bucket). Measured single-threaded (8M, AVX-512 box):

- Where wired (uniform coarse path), it closed the gap from **0.39x -> 0.86x** of
  x86-simd-sort — near parity, not beyond: logos's dispatch/probe overhead before
  the hand-off caps it at parity single-threaded.
- Other losing shapes (heavy_tail, bimodal, int_wide, quantized) route through
  *different* fallbacks (bits-coarse, quant/int radix) not yet wired to the leaf,
  so they barely moved. Wiring those is mechanical but wasn't needed to learn the
  lesson.

Conclusion: **the SIMD leaf reaches parity, never exceeds, single-threaded.** The
only way logos beats a single-threaded SIMD sort is thread scaling: build with
`make parbench-simd` and run `LOGOS_SIMD_LEAF=1 ./parbench` on a many-core box —
that pits parallel-SIMD (logos) against single-threaded SIMD (x86-simd-sort), the
one matchup logos can win. Whether it does is for that machine to say; unmeasured
here (1 core).


## Can SIMD accelerate logos's own hot loops? (measured: essentially no)

Two attempts, each A/B'd end-to-end (`logos_simd.cpp` kernels, `-march=native`):
- Vectorizing the coarse **bucket-index** (1.47x in isolation) was **-3% to -5.5%
  end-to-end** and reverted: it fuses with a scalar scatter-count, and the
  SIMD-store->scalar-reload costs more than it saves.
- Vectorizing the **OR/AND byte-skip** (1.22x) integrates cleanly but is ~0.1% of
  a memory-bound sort, so the effect is noise. Kept opt-in, unwired by default.

Confirms the project's thesis quantitatively: logos's hot loops are memory-bound,
so vectorizing their arithmetic doesn't move the needle. SIMD belongs at the leaf
(a vectorized quicksort), and the best way to combine them is a dispatcher that
routes structured data to logos's zero-scatter paths and everything else to a
SIMD sort -- not by vectorizing logos's own passes.