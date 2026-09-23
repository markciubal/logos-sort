# logos-sort (C, multi-threaded) — v3

The JavaScript engine's numeric dispatcher, ported to C, running as the
per-bucket leaf (and the sequential path) underneath a parallel sample-sort top
level. Numbers only (f64, i32).

## Package

```
logos_psort.h     public API (3 functions) + LOGOS_PSORT_VERSION
logos_psort.c     the engine (single translation unit, no dependencies beyond libc/pthread/libm)
harness.c         test: 570 correctness checks vs qsort + benchmark
example.c         minimal usage
parbench.cpp      optional fair-fight bench vs std::sort / __gnu_parallel::sort (g++ + OpenMP)
Makefile          build, test, example, install, uninstall, dist, clean
IMPLEMENTING.md   from-scratch implementation guide
wasm/             Emscripten build recipe (threaded WebAssembly)
CHANGELOG.md · LICENSE · .gitignore
```

### Build and test

```sh
make            # liblogos_psort.a, liblogos_psort.so, ./harness, ./example-bin
make test       # run the harness: correctness (exit code != 0 on any failure) + benchmark
make example    # build and run the usage example
make dist       # source tarball logos-sort-c-3.0.0.tar.gz (builds standalone)
make clean
```

Any C11 compiler with POSIX threads. Override as needed:
`make CC=clang CFLAGS="-O2 -pthread"`.

### Install and link

```sh
make install                       # header -> /usr/local/include, libs -> /usr/local/lib
make install PREFIX=$HOME/.local   # or anywhere
make uninstall
```

```c
#include <logos_psort.h>
logos_sort_f64(a, n, logos_num_cpus());   /* ascending, in place */
```
```sh
cc myprog.c -llogos_psort -lpthread -lm   # shared; or link liblogos_psort.a directly
```

### API

```c
void   logos_sort_f64(double  *a, size_t n, int nthreads); /* ascending, in place */
void   logos_sort_i32(int32_t *a, size_t n, int nthreads);
void   logos_sort_f64_nan_last(double *a, size_t n, int nthreads); /* NaNs to the tail, bits intact, then the prefix sorted */
size_t logos_nan_to_end_f64(double *a, size_t n);          /* that pass alone; returns the length of the NaN-free prefix */
void   logos_sort_f64_inplace(double  *a, size_t n, int nthreads); /* no scratch at all; slower; sequential */
void   logos_sort_i32_inplace(int32_t *a, size_t n, int nthreads);
void   logos_release_buffers(void);                        /* returns everything kept between calls */
int    logos_num_cpus(void);                               /* sensible default for nthreads */
int    logos_reach_last(void);                             /* diagnostic, for tests: what the short-reach route did on the last f64 sort of 2^18+ elements */
```
Results never depend on `nthreads` (tested at 1/2/4/8); `nthreads <= 1` is fully sequential. Not a stable sort.

**Memory, measured** (`make memprobe`: peak traced heap of one cold call, on top of the input). `logos_sort_f64`: 15.5 bytes per
element at one thread, about 23 at fourteen on data that reaches the partition, about 9 on the relocation and knot routes, under 1
on the counting routes, and 0.10 / 1.33 (one thread / fourteen) on the short-reach route, which asks for no n-sized buffer. `logos_sort_i32`: 11.5 bytes per 4-byte element at one thread, about 6.5 at fourteen on the partition, under 0.1 on the counting route. The in-place entries: 0. The buffers are kept between calls;
`logos_release_buffers()` returns all of them. (This section said "~8n" until the day it was measured.)

**Special values.** Infinities, subnormals and signed zeros are ordinary values, and tested. With a NaN in the array the ORDER is
unspecified, of the other values too; what holds even then, and is tested, is that the call returns, nothing crashes and the output
is a permutation of the input's bit patterns. Use `logos_sort_f64_nan_last` when the data may hold NaNs: it costs one sequential pass.

**When a resource runs out.** A failed `malloc` or `pthread_create` costs speed, never correctness: the array still comes back
sorted, by a route that needs less, down to a sort that needs nothing. `make faultprobe` fails every allocation and every thread
creation of a cold call in turn; `make test` repeats a sample of that in-process. There is no error return because there is no
failure left to report.

**Sizes.** `n` is a `size_t`. The buffered engine counts in 32 bits, so arrays above 2^32 - 1 elements go to the in-place sort
(read, not run: no machine here holds one). Built and run only with GCC 13 on x86-64 Linux.

## What was ported (from `lib/sort.js`)

Per range, the dispatcher does what the JS `sort_fn` / `sort_fn.f64` do, with the
same thresholds:

- **one-pass prescan** — min, max, all-integer?, ascending?, descending? →
  already sorted returns in O(n); reversed is reversed in place
- **integers** — span ≤ n → counting sort; int32 range → LSD radix with
  fused histogram and **constant-byte skip** (a byte identical everywhere
  contributes no ordering, so its pass is skipped)
- **natural runs** — `detect_runs_limited` (descending runs reversed in place,
  prefix-based bail) → bottom-up run-merge: **galloping 2-way** (`merge_gallop`,
  adaptive min-gallop with eager start) and **4-way tournament with galloping**
  (`merge_4way_hybrid`) when ≥16 runs remain
- **nearly sorted** (40-point inversion sample) → insertion, or **momentum
  insertion** with the **displacement budget** that aborts to the distribution
  paths when the verdict was wrong (the fix for the front-loaded cliff)
- **large data** → 64-sample **gap detection** → multimodal → **gap-split**
  Hoare partition + recurse; flash-safe & <500k → **flash sort**; sampled
  **skew probe** >8 → **exponent-bit ("bits") coarse** bucketing; else
  **value coarse bucket sort** (K=2048, bucket-index cache, depth cap)
- **otherwise** → **dual-pivot introsort** (`sort5` pivots, 3-region partition,
  Dutch-flag fallback for duplicates, heapsort at depth 0)
- **typed entry** (≥65536): quantized data (≤4 active key bytes, n≤8M) →
  **8-pass float radix over the IEEE total-order key**, constant bytes skipped;
  skewed → bits coarse

## Deliberate deviations from the JS engine

1. **Per-thread context instead of module-level state.** JS keeps a pooled
   scratch buffer and the `last_bail_avg_run` side channel at module scope.
   Buckets sort concurrently here, so every such thing lives in a per-thread
   `ctx_t` (scratch, bucket-index cache, per-depth counters, radix histograms,
   `last_bail_avg_run`). Semantics identical; thread-safe.
2. **Bits-coarse same-high-word fallback → value coarse (not quicksort).** The
   harness's bimodal generator exposed a latent JS inefficiency: tight clusters
   at large magnitude trip the skew probe, route to bits-coarse, and every
   value in a cluster shares one high word — where JS falls to a full quicksort
   on the whole cluster. Values sharing a high word are close together, which is
   exactly where value-linear bucketing works. Measured: 895 → 366 ms on 8M.
   **Worth back-porting to the JS engine.**
3. Not ported: strings (a separate engine), `arg_sort`, `find_outliers`,
   `sort_with_proxy`.

## Parallel top level (v2, unchanged)

Sample → quantile splitters → parallel histogram → parallel scatter into 4×P
value-balanced buckets → per-bucket leaf via an atomic work queue with parallel
copyback. One thread spawn; barrier-synchronized phases; per-thread arenas;
branchless bucket lookup; no locks outside the barrier. See `IMPLEMENTING.md`.

## Build / run

```sh
make run     # builds ./harness: 570 correctness checks vs qsort, then benchmark
```

## Testing a knob: use `pfbench`, not repeated `parbench` runs

The interleaved A/B benches: `pfbench` (prefetch distance), `j5bench`
(relocation), `stbench` (splitter tables, model table), `leafbench` (leaf
knobs), `theorybench` (bucket count, leaf branch policy, interleave, line
repair), `simdbench` (AVX2 leaf, four policies, 1 thread and P), `chaosbench` (the
zoo: Weyl sampling, low-discrepancy probe, per-mode tables, network pivot),
`latbench` (lattice window size), `nsbench` (near-sorted route, four engines),
`smallbench` (thread policy at 100k–8M, per-route caps), `knobbench` (one inherited constant, four values, 19 shapes), `hfbench` (histogram-free partition, KS certificate, slack), `sodobench` (fixed-order gates vs one sample), `probebench` (fast sample and context bundles), `cardbench` (the counting route's lookup across a cardinality ladder), `tailbench` (heavy-tail structures), `knotbench` (the knot route: staircases of overlapping runs; one binary, `LOGOS_NO_KNOT`).

`parbench` compares *sorts* within one run. To compare *variants of logos*
(e.g. the prefetch distance), separate runs are fragile: background load
drifts 25% between them and swamps a 5% knob — the calibration line will tell
you when that has happened. `make pfbench && ./pfbench` builds five engine
variants into one binary and interleaves them on identical data; load then
costs precision but cannot bias the result. Pick the column that wins the
parallel rows and build with `-DPF_DIST=N`.

## Twelve buckets per thread, and a table leaf whose branch policy is chosen per run

Two further changes on the parallel route, each A/B'd over three interleaved
runs (`make theorybench`, six variants, 8M, 20 and 10 threads). The bucket
count went from 4 to 12 per thread (capped at 256 buckets so the one-byte
bucket cache survives): the bucket queue already balances dynamically, so
more buckets shrink its tail, and a ~33k-element leaf's flash working set
stays in L2 where a 100k one spilled; 1.08–1.44× on 9 of 12 partitioning
rows by itself. The splitter-table leaf's children are sized to the splitters
they separate and resolve with one compare-and-add; at the root the choice
between a predictable branch (flat data: nearly every element resolves there,
the branch is free and saves a load) and a branch-free path (skewed data: the
root resolves a mix and the branch mispredicts) is made per thread from the
fraction of root resolutions counted during the trial chunks, because a fixed
choice lost 5–12% on the int shapes or up to 35% on heavy_tail. Measured
mixed across runs and therefore opt-in, not shipped: interleaved histogram
counters (`-DLOGOS_HIST_INTERLEAVE`), line repair after flash
(`-DLOGOS_LINE_REPAIR`), int-leaf digit narrowing (`-DLOGOS_INT_NARROW`).

## The histogram lookup: direct-index splitter tables (H0b)

The profiler found the parallel path's real bottleneck: the binary search that
assigns each element to one of the P×4 buckets cost ~100 ns/element of CPU at
20 threads, more per element than the whole single-thread sort. It is now an
O(1) table lookup with two tables (value-space cells for flat data, key-space
cells for skewed data); each histogram thread times both on its first two
chunks and commits to the faster one. Exact by construction; falls back to the
search if a table cannot be built. Interleaved A/B (`make stbench`, 8M, loaded
box):

| shape (20 threads) | search | table | |
|---|---|---|---|
| uniform | 76.0 ms | 38.4 ms | 1.98× |
| heavy_tail | 71.4 | 35.1 | 2.03× |
| bimodal | 42.4 | 26.9 | 1.58× |
| int_wide | 63.6 | 21.3 | 2.99× |
| clustered | 72.8 | 38.6 | 1.89× |
| near_sorted / quantized / few_distinct / block_perm (never partition) | — | — | noise |

Build with `-DLOGOS_NO_SPLITTER_TABLE` for the previous behaviour.

## The sorted view: pay for order only where you look (`make viewbench`)

`logos_view.h`. Partition the array into ordered buckets and sort a bucket only when
something reads it. A median, a rank, a top-k or a range count then costs the partition
plus one or two buckets instead of a whole sort: 1.5–2.9× at 8M on 14 threads, every
answer verified exact. Materialising everything gives byte-identical output to
`logos_sort_f64` at 1.10–1.37× its cost. Why it works: a partition is a total order
*between* buckets and none *within*, which is exactly what a rank query needs, and the
traced leaf phase is 63% of a sort. CHANGELOG has the numbers and the two lessons.

## Multimodal data: a certificate per mode (`make tailbench`)

When the sample splits into modes at large gaps, each mode gets tested for
uniformity on its own slice of the sample. If they all certify, the partition
drops its per-mode tables and buckets with a few branch-free compares plus one
multiply-add. Measured at 14 threads with the copy-swap control: bimodal
1.25–1.38×, three uniform bands 1.23–1.33×, a gaussian mixture 1.01–1.03×
because it correctly declines (a hump is not uniform inside), everything else
untouched. `-DLOGOS_NO_MODEKS`; FINDINGS §3s.

## Heavy tails: a certificate in key space, and a balance check (`make tailbench`)

The partition's bucket map is chosen from the sample. Three things were added
for skewed data (FINDINGS §3q):

| data | uniformity in value space | in key space | map the partition now uses |
|---|---|---|---|
| uniform | 0.013 (passes) | — | equi-width grid on the value |
| Benford / log-uniform | 0.746 | **0.018 (passes)** | equi-width grid on the **key** |
| Pareto α = 1.2 | 0.985 | 0.692 | key splitter table (exponent map rejected by the balance check at 4.8×) |
| lognormal, exponential | 0.9+ | 0.7+ | key splitter table |
| one value holding 50% | — | — | declines before scattering |

(a sample of 5376 certifies below 0.027)

The key-space certificate is the one that pays: log-uniform data (prices,
file sizes, measured magnitudes) sorts 1.45× faster at 14 threads because it
buckets with one multiply instead of walking a table. The balance check is
the guard the trial always needed — it times the lookup, not the partition,
and without the check it selected a map that spilled 63% of the data. The
duplicate guard declines the partition from the sample instead of after a
wasted pass. An exponent-histogram map is built but opt-in: correct, and
slower than the table it would replace.

## The counting route's lookup: a verified perfect hash (`make cardbench`)

The low-cardinality route counts each element into a small table. Its lookup
used to be a linear scan up to 32 distinct values and a binary search above,
which cost 2.0 ns per element at 5 distinct values and 30.8 ns at 64 — an
unbenchmarked cliff, since no shape in the suite had a cardinality between 33
and 64. Now the gate's own sorted sample supplies the distinct values, a
multiply-shift perfect hash over their keys goes into a byte table, and the
lookup is a multiply, a shift, a byte load and one compare, 1.1 ns at every
cardinality. The compare keeps it exact: a value the sample missed fails it
and falls into the old incremental table. `-DLOGOS_NO_LC_PHASH` restores the
scan; `make cardbench` is the A/B; FINDINGS §3p.

Rejected on measurement: routing low-cardinality data to the lattice's direct
index first. The index is cheaper per element in isolation but its table spans
the value range, about 96 KB against the hash's 4 KB, and it lost.

## The serial fixed costs: prefetched sample, vectorized sample sort, context bundles (`make probebench`)

The probe is not parallelised and should not be: a thread spawn costs about
what the sample costs. Instead the sample's random reads are issued as
prefetches first so the misses overlap, the sample is sorted by the AVX2
quicksort (NaNs compacted to the tail; the engine's own introsort where AVX2
is absent; libc qsort on no sampled path, it is 11× slower than the AVX2 sort
and 2× slower than the introsort on 5376 points), and the per-thread contexts (buffers ~6 bytes per element of
capacity plus ~340 KB) live in a persistent table instead of being allocated
on every call. Per parallel call the serial work before the workers went
from ~2 ms to ~0.9 ms and the serial step between scatter and leaves from
0.8–1.8 ms to 0.09 ms (FINDINGS §3o). `-DLOGOS_NO_SAMPLE_FAST`,
`-DLOGOS_NO_CTX_POOL`. SHIPPED as the default: the trace shows the mechanism and the quiet half of the end-to-end A/B confirms 1.07-1.18x at 1M and 1.02-1.10x at 8M on the parallel rows with the 1-thread rows unchanged; the persistent bundles cost about 6 bytes per element of the largest capacity ever requested (48 MB after an 8M single-threaded call), the same order as the existing buffer pool.

## Sample once, decide once (`make sodobench`)

The parallel entry's gates are answered from one sorted sample instead of
five: the first 2048 Weyl points carry the distinct count, a Chao1 estimate
of the true cardinality (singletons and doubletons), NaN, and an inversion
rate over ~1900 Fibonacci-lag pairs (until 2026-09-20 the loop read SEVEN of
them, a whole array apart: see the short-reach section below); the partition, if reached, extends the
same sample to 5376, computes the KS certificate on it, and uses it for the
splitters. The routes keep their cheapest-first order, because a cost model
from the measured per-route costs reproduces it. What changes is that the
counting routes are attempted only when the estimated true cardinality fits,
and no probe is drawn twice. `-DLOGOS_SODO`; FINDINGS §3n. SHIPPED as the default (-DLOGOS_NO_SODO restores the per-gate samples): no loss on any shape once the two-stage draw was in, 1.07-1.39x on zipf and 1.07-1.22x on the 1M rows of the partition shapes, neutral at 8M; every run of the three was on a loaded box, ratios within rows only.

## Histogram-free partition and the uniformity certificate (`make hfbench`)

The parallel partition no longer counts before it scatters. The sample sizes
the buckets: each bucket's true count is n times the spacing between two
consecutive sample quantiles, a Beta(32, m−31) variable with an 18% standard
deviation, so every bucket gets a region of (n/K)·1.6 in a persistent buffer,
threads reserve 256-element blocks in it with one atomic per block through a
per-thread write-combining stage, and the rare overflow spills to a list
that is merged in at the leaf. If spills exceed n/16 the routine declines and
the histogram path runs (`-DLOGOS_NO_HISTFREE` forces it). One read of the
data instead of two, no bucket-index cache, no exact-layout requirement.

The certificate: a Kolmogorov-Smirnov distance between the sorted sample and
the line through its extremes, free since the sample is sorted. Below
2/√m the partition uses an equi-width grid, one multiply per element and no
tables. Measured at 14 threads: uniform 1.31×, int_wide 1.41×, int_1e6 1.45×,
jitter_ms 1.24×, logistic 1.20×, clustered 1.14×, heavy_tail and bimodal
within noise to 1.15×; other routes untouched (FINDINGS §3m).

## The inherited constants, swept (`make knobbench`)

The dispatcher's thresholds came from the JS engine. `knobbench` builds four
engines with four values of one constant (`make knobbench KB1=-DX=a KB2=-DX=b
KB3=-DX=c && ./knobbench base a b c`) and runs them interleaved over 19
shapes at 1 thread and 14. Fourteen constants were swept (FINDINGS §3l):
three changed (the run-merge gate to 4096, a new cap of 8 on the runs a
k-way merge will take, the low-cardinality abort cap to 256), eleven kept
with the evidence recorded. The JS values were not a bounded-language
compromise: where one was wrong, the merge was the reason, and merges are
expensive in C.

## Why the structured routes did not scale, and the new near-sorted route (`make nsbench`)

The profile report's 1-thread and 14-thread cells give each sort's own
speedup on each shape. The partition route scales like the vectorized
quicksort (4.6–5.5× against 4.0–4.2×), but the routes that win single-threaded
did not: near-sorted 1.8×, block relocation 2.1×, lattice 3.7–4.4×, against
6–7× for IPS⁴o (FINDINGS §3j). Two general causes. Chunking by index changes
the dispatcher's decisions, because its gates are ratios of n and a chunk of
near-sorted data spans the whole value range, so the chunk leaf ran a radix
at 16–30 ns per element where the whole array ran counting at 6. And the
inversion probe bounds inversions, not displacement, so a nearly-sorted 48k
bucket with far-travelled elements made plain insertion quadratic; the
momentum insertion (8n move budget, falls through) now takes that case.

The near-sorted route was rebuilt band-first: thresholds at the chunk
boundaries, one greedy keep/reject pass per chunk that leaves a sorted run
and a stray list (with poison recovery), the strays sorted once, and one
two-way merge per band. near_sorted at 14 threads: 20 → 10–14 ms; locally
shuffled data 1.5×; guards unchanged. `-DLOGOS_NS_OLD` keeps the old route.

## Order from chaos: Weyl sampling, low-discrepancy probe, per-mode tables (`make chaosbench`)

Three items from `experiments/ORDER_FROM_CHAOS.md`, each A/B'd on its own
against the same baseline over fifteen shapes (FINDINGS §3h):

- **Weyl sample positions.** Every sampler reads `floor(frac(i·φ)·n)` instead
  of a fixed stride. A stride aliases with periodic input: a sawtooth whose
  period equals the stride collapsed the splitters and one thread sorted all
  8M (546 ms); the Weyl sequence is equidistributed against every period
  (three-gap theorem) and holds it at 38. Van der Corput data aliased the same
  way (177 → 19 ms at 14T). `-DLOGOS_STRIDE_SAMPLE` restores the stride.
- **Low-discrepancy probe.** Two contiguous 64-element windows; if their
  sorted gaps never differ by more than 4× the data is a permutation of an
  equi-width grid and goes to flash/coarse instead of the AVX2 leaf: weyl and
  van der Corput 1.25–1.29× at 1 thread. `-DLOGOS_NO_LOWDISC`.
- **Per-mode splitter tables.** Splitter gaps over ten times the median gap
  divide the range into modes (up to 8); each mode gets its own value table
  and the histogram trial can pick it. bimodal 14T 22.8 → 19.9 ms, level with
  x86-simd-sort on that row. Absent on single-mode data. `-DLOGOS_NO_MODE_TABLE`.

Measured neutral and left opt-in: the network pivot (`-DAV_PIVOT_NET`). Not
built: equi-depth leaf classes from the sample, because the profile bounds
the gain at ~3% (the leaf on heavy_tail is only 7% above uniform).

## AVX2 leaf for sequential sorts (`gen_avx2_leaf.py`, `make simdbench`)

An in-house AVX2 port of the x86-simd-sort design takes the unstructured leaf
of a single-threaded sort: an in-place quicksort whose partition splits each
4-double vector with one compare, one table permute and two plain stores,
loads eight vectors per end-choice, streams ranges under 512 through a stack
buffer, and finishes with 32/64-element bitonic networks emitted as
straight-line code. It runs only where the dispatcher would otherwise run
flash, coarse or bits-coarse; counting, radix, relocation, merge and momentum
keep their paths, and it steps aside when the inversion probe says nearly
sorted (coarse's near-sequential scatter is 1.23× better there).

Measured (four engines in one binary, 8M): single-threaded uniform 1.20×,
heavy_tail 1.48×, bimodal 1.59×, clustered 1.19×, negatives 1.26×; every
structured shape identical. Inside the parallel leaf the same code lost
5–28% at 14 threads and 10–16% at 6 on this hybrid P+E CPU, so it is gated to
sequential sorts; `-DLOGOS_AVX2_PARALLEL` lifts the gate for another box.
`-DLOGOS_NO_AVX2_LEAF` compiles it out, `LOGOS_NO_AVX2=1` disables it at run
time. The networks are generated: edit `gen_avx2_leaf.py`, run it, `make test`.

## Model splitter table (opt-in, measured neutral)

`-DLOGOS_MODEL_TABLE` adds a third candidate to the histogram's per-thread
trial: one level of value cells with a linear bucket-index model per
multi-splitter cell, corrected against the real splitters. Two 14-thread
runs of `make stbench` put it at 0.93–1.09× of the value+key tables, the
7-thread rows within 3%: noise. Kept as a measured negative.

## Textbook baselines: LSD radix and timsort

`bench_sorts.h` carries two untuned single-threaded reference sorts so the
charts show where a plain radix sort and a plain adaptive merge sort sit:
`radix_sort_f64` (LSD over the order-preserving key, 11-bit digits, six
passes, constant digits skipped) and `timsort_f64` (CPython's listsort:
minrun, natural runs, binary insertion, the corrected stack invariant,
galloping merges). Both are in the single-threaded section of parbench, the
1-thread cells of the profile report and the 1-thread panels of the scaling
report; never beside the parallel sorts.

Where they land (8M, 1 thread, quiet box, FINDINGS §1d): logos beats the
radix on 8 of 9 shapes (uniform 128 vs 188 ms, few_distinct 27 vs 96,
block_perm 17 vs 184) and loses bimodal (158 vs 137), the one shape where a
distribution-blind sort pays nothing for being blind. Timsort is slower than
std::sort on random data (854 vs 660 ms uniform) and 12× faster on the block
shapes (27 ms), which logos's relocation still beats (17 ms).

## Benchmark thread count: `BENCH_THREADS`

Every benchmark (parbench, profbench, scalebench and the A/B benches) defaults
to `BENCH_THREADS` from the Makefile, 14 on the reference box, through
`bench_common.h`: `LOGOS_THREADS=N` in the environment wins, then the Makefile
value, then the physical core count from `/sys` if the kernel exposes it, then
all online CPUs. The engine's own default, `logos_num_cpus()`, is unchanged;
the policy is about clean measurement, and every contender in a fair fight
runs at the same count.

## Measuring the noise: `make noiseprobe`

Repeats one 8M sort at 1, 2, 4 … 20 threads and prints min / median / max,
spread, the single-thread preempted share and the parallel CPU utilisation.
Run it before a benchmarking session: if median/min at your thread count is
above ~1.3, expect ±30% per rep and rely on the minimum of 12 or more
interleaved reps. On the reference box the best minimum was at 14 threads
(its physical core count), and the noise came from two other processes, not
from the engine.

## Thread count follows n, and the route

Three thread policies, all measured (`make scale-full`, `make smallbench`):

- **Global:** at most one thread per 100k elements (`MIN_ELEMS_PER_THREAD`).
  Below ~65k the parallel entry is not used at all.
- **Relocation:** at most one thread per 500k (`RELOC_MIN_PER_THREAD`). Its
  parallel form has a serial plan and a spawn; at 3.16M six threads beat
  fourteen by 1.7×, at 1M four beat ten.
- **Counting routes** (lowcard, modcard, lattice): at most one thread per
  350k, never fewer than 3 (`COUNT_MIN_PER_THREAD`, `COUNT_MIN_THREADS`).
  They merge P tables, a cost proportional to P and not n: at 1M four
  threads beat ten by 1.5–1.8×; at 316k three still beat one.

A single global threshold cannot express this (a global 250k policy was
measured and rejected). Neither per-route cap binds at 8M, so the fair fight
at that size is unchanged. `smallbench` runs the P-thread call against the
single-thread call from 100k to 8M; the only rows where one thread is still
faster are the block shapes at 316k, by ~0.1 ms of parallel-entry setup.


`logos_sort_f64(a, n, nthreads)` treats `nthreads` as a ceiling: the engine uses
at most one thread per 100k elements (`MIN_ELEMS_PER_THREAD`), because the
parallel route's fixed cost made 20 threads slower than one below ~5e5
elements on the reference box. Results never depend on the thread count.
`-DLOGOS_NO_THREAD_POLICY` restores the old behaviour.

## Scaling: `make scale-full` → scale-report.html

Nineteen sizes from 0 to 5e7 (0, 1, 3, 5, 10, then half-decades from 32, and the
5e7 ceiling itself) for every shape: one panel per shape for the single-threaded
sorts (std::sort, logos, logos in-place, x86-simd-sort, IPS⁴o, LSD radix, timsort)
and one for the parallel sorts (`__gnu_parallel::sort`, logos, logos in-place,
x86-simd-sort OpenMP, IPS⁴o parallel), never mixing thread counts. Under each
ns-per-element table a slope row gives p = 1 + ln(c_hi/c_lo) / ln(n_hi/n_lo) from 1M
to the largest size; its two rows are minutes apart on a drifting machine, so it is a
sketch and `make slopebench` is the measurement. Each panel holds two charts of the same measurements:
time per sort (linear axes, with a log-log toggle) and **nanoseconds per
element, whose value axis is always linear**. Size buttons cut both to
`n ≤ 10k` or `n ≤ 10`, since a linear n axis puts everything below a million
into its first pixels. Below 10k a size is timed as a batch of distinct inputs
sorted back to back (one such sort is shorter than the clock can resolve); n = 0
is the cost of one call. std::sort's output is the reference for every other
sort. `make scalebench` runs it without the two external opponents. Overrides:
`LOGOS_MAX_N`, `LOGOS_MIN_N` (10000 restores the old ladder), `LOGOS_THREADS`,
`LOGOS_MAX_SEC`; `LOGOS_SCALE_REUSE=<report.html>` re-renders a report's
measurements through the current template without measuring.

## Profiling: `make profile` → profile-report.html

The engine compiled with `-DLOGOS_PROFILE` records, for every call, the
top-level route it took, wall time per phase, and per thread its working time,
the CPU time it actually received (preemption by other processes shows up
separately, not as engine cost), time blocked at barriers, and the leaf path
each bucket took. `make profile` runs the parbench shapes plus the block shapes
at 1 thread and at P and writes an HTML report: per cell a treemap of
thread-time (phase, leaf path, preempted, stalled, idle), per-thread stacked
bars, and a phase table with CPU utilisation and streaming rate against the
memcpy calibration. The fair-fight opponent at the same thread count runs on
the same data (std::sort at 1 thread, `__gnu_parallel::sort` at P; add
x86-simd-sort with `make profile-simd XSS=...`) and appears as a bar chart and
in the summary, so each cell shows both *how it compares* and *why*. Every
output is verified against std::sort; the calibration line is at the top of
the page.

**What it found first** (20 threads, 8M, this box): the top-level histogram is
the largest phase on every unstructured shape and it is compute, not memory —
`bucket_of`'s binary search over the splitters costs ~100 ns per element of
CPU here, against ~11 ns for the scatter it feeds and ~29 ns for the entire
1-thread sort. The 3.4.0 bucket-index cache moved that search out of the
scatter; it did not remove it. That is the parallel path's real bottleneck and
the next thing to fix (see CHANGELOG for the prediction).

## What had never run: memory, exhausted resources, special values, the in-place sort

An outside review of a sibling line of this engine (2026-09-19) listed what had not been tested: NaNs, infinities, allocation
failures, extreme sizes, memory that "may approach O(Pn)". None of it was assumed to hold here; each became a measurement in this
tree, and **every condition the harness had never produced found a defect**. Full account: FINDINGS §3ae, CHANGELOG.

| tool | what it answers | what it found |
|---|---|---|
| `make memprobe` | what ONE cold sort weighs (malloc family interposed, a forked child per cell): peak, still held, left after release | 23 bytes per element at 14 threads where the header said "~9n"; **104** when one value holds most of the array (every thread's context was sized by the largest bucket); the partition's regions allocated twice; `logos_release_buffers()` returning 9 of 23. Now 23 / 17.8 / 0 |
| `make faultprobe` | what happens when the k-th allocation, or the k-th `pthread_create`, of a cold call fails (forked children, so a crash or a hang is a result) | 2,215 of 8,316 injected allocation failures and 154 of 186 thread-creation failures ended in **wrong output**, a crash or a hang. Now 0 of 7,008. `make test` repeats a sample in-process |
| `make sanitize`, `make tsan` | the whole harness under ASan + UBSan + `float-cast-overflow`, and under ThreadSanitizer | the casts behind the infinity and subnormal defects (`float-cast-overflow` is not part of `-fsanitize=undefined`); 0 data races |
| `make slopebench` | the scaling exponent p = 1 + ln(c_hi/c_lo) / ln(n_hi/n_lo), 2M against 50M, measured in adjacent pairs so machine drift cancels | at one thread buffered logos grows 13-19% per element from 2M to 50M where `std::sort` grows 21-24%, and is exactly linear on the counting and relocation routes; at 14 threads uniform grows 26-34% (2M fits the cache, 50M does not) while the structured routes get 31-60% cheaper, so at 2M a parallel call there is mostly fixed cost |
| `make ipbench` | the strict in-place sort in parallel: against its own 1-thread form, and against the buffered sort at the same thread count | 2.5x at 8.4M and 2.85x at 50M on uniform; still 6-24x slower than buffered: zero scratch is bought with time |

How a failed thread creation is survived, since no `pthread_create` result was read at any of 23 sites: workers that do not wait
for one another go through `par_run`, which runs an orphaned worker on the calling thread; workers that meet at a barrier go
through `par_spawn`, which drops the orphan from the barrier (its count became atomic; whoever completes a barrier arrives no
earlier than the creating thread, so the count it reads is final). Relocation and the knot route give each thread a static tile
of the output, so there the caller covers the orphans' tiles; the int32 counting route declines.

## Disorder of short reach: sort where it lies, repair only the seams (`make reachbench`)

An event log whose records arrive with bounded latency, a series shuffled inside small windows, anything jittered around its own
order: every element already sits near its final place. The partition does not know that, and scatters all n elements into a
second buffer and back. `reach_try_f64` does not scatter. It cuts the array into index blocks of 32768, sorts every block where it
lies (a flash sort: on locally ordered data classification is nearly the identity, which makes it 1.26-1.81x faster than the AVX2
quicksort on these blocks even at one thread; an insertion sort when the disorder reaches only two or three places), and then
repairs the seams: for sorted blocks L and R, the elements of L above R[0] and the elements of R below the last of L are merged in
place. What crosses a seam rightward is matched by what crosses leftward, so nothing else moves, and no n-sized buffer is asked
for (`make memprobe`: 0.10 bytes per element at one thread, 1.33 at fourteen, against 15.5 and 23.2).

The seams are independent only while every block keeps an untouched middle, and the route guarantees that instead of hoping for
it: with m(k) the median of sorted block k, an element of block k below m(k-1) or above m(k+1) is a *stray* and is set aside
first. Strays (one late event, a zero in a column of timestamps) are what a sample cannot see. A few are sorted apart and put back
by one in-place shift pass: between two strays every element moves by the same amount, so the pass is a list of `memmove`s over
exactly the span the strays travel. Up to four threads they are simply put back; wider sorts, where the serial shift costs what
the route saves, hold eight probe blocks against their neighbours' medians first and decline when two hold strays.

The gate reads the dispatcher's sample, never the array. Sample indices i and i+L are |frac(L phi)| n positions apart, and the
Fibonacci lags 144 ... 1597 give a ladder of distances from 0.31% down to 0.028% of n. The reach is the smallest rung from which
no distance shows more than 2% inverted pairs. Random data fails the first rung, which the shared sample has already read.

Route off / route on in one binary (`LOGOS_NO_REACH`), 8.4M, 30 pairs:

| shape | 1 thread | 14 threads |
|---|---|---|
| latency_4096 (event log, latency under 4096 ms) | 1.91 | 1.31 |
| window_2048 (shuffled inside 2048-element windows) | 2.22 | 1.38 |
| jitter_1000 (index + noise of +-1000) | 2.05 | 1.33 |
| latency_512 | 2.38 | 1.49 |
| one far stray / strays in 0.01% of elements / 0.05% zeros | 1.81 / 1.57 / 1.58 | 1.14 / 1.00 / 0.99 |
| uniform, near-sorted, staircases, block-permuted, heavy tail, few distinct | 0.99-1.03 | 0.94-1.02 |

The margins grow with n (30M: 2.1-2.9x and 1.5-1.8x) and the route declines above about 117M elements, where even the ladder's
finest rung is coarser than a block may be. f64 only. `make reachhunt` holds the ceilings that were measured before anything was
built, including the two analogies that were NOT built (cut decomposition, chunk relocation); `experiments/SHORT_REACH.md` has the
whole list. Building the gate found two defects in the near-sorted route's gates (`make nsgatebench`: 3.8x on sorted runs grouped
by a coarse key, 1.2-1.5x on 1-2% random swaps) and one in the general leaf's nearly-sorted probe. `make callbench` (what ONE CALL
costs at 0 to 50,000 elements, the engine before a change against the engine after it) was written to check that none of this cost
a small sort anything. It had: the gate helper's 32 KB of arrays sat in the dispatcher's frame, which is probed page by page on every
call (+9-14% at n <= 2, repaired). The same bench priced entering the dispatcher at ~5.6 ns, so a call with nothing to sort now
returns first: 8.5 -> 2.9 ns.

## The knot route: comparisons only where runs cross

Relocation (next section) treats a chain of overlapping runs as one unit: it merges every element of the chain, and
declines the whole array once chains hold more than half of it. But two runs that overlap in a tenth of their range
are tangled only in that tenth. The knot route reads each run as a *strand*, the run descriptors sorted by first
value as a linked list the sorted output walks along, and a *crossing* as a value interval in which two or more
strands both hold elements. It sweeps the strand endpoints in value order, cuts every strand where the set of live
strands changes, **copies** each piece that lies alone in its interval and **merges** only the pieces that share one.
Plan O(R log R), copies O(n), comparisons O(crossing mass x log k).

The target is a *staircase*: sorted blocks whose value ranges each overlap the next (time-series shards with clock
skew, rotated logs, sorted files with overlapping boundaries). One chain covers everything, so relocation declines
and the array used to pay for a full sort. Paired A/B in one binary (`make knotbench`, `LOGOS_NO_KNOT` = the engine
as it shipped before), route off / route on:

| shape (8.4M) | share of elements crossing | 1 thread | 14 threads |
|---|---|---|---|
| 64 shuffled blocks, each overlapping the next | 10% | 4.44× | 1.20× |
| | 20% | 4.05× | 1.32× |
| | 60% | 2.93× | 1.31× |
| | 100% (every element in a two-strand crossing) | 2.64× | 1.50× |
| three strands live everywhere | 100% | 2.58× | 1.24× (declined early) |
| eight strands live everywhere | 100% | 1.40× | 1.15× (declined early) |
| the same blocks in value order | 20% | 4.02× | 1.02× (the near-sorted route takes it first) |
| 14 shuffled strands | 20% | 1.85× | 1.09× |
| block_perm64, uniform, few_runs, segmented, near_sorted (guards) | | 1.00–1.02× | 0.99–1.06× |

**Read the 14-thread column with care.** It is against the engine as it shipped, whose relocation scans a shuffled
staircase for runs and *then* declines; that wasted scan is in the baseline. Against the pure partition (an engine
copy built `-DLOGOS_NO_RELOCATE`, both copy orientations, layout bias cancelled) the route is worth 1.8–4.1× at one
thread and is a **tie at 14** (0.96–1.11 on two-strand crossings, 0.89 on three, which is why parallel sorts stop at
two). The same control shows relocation itself at 0.95 (block_perm64) and 0.92 (block_overlap) at 14 threads: its
1.9–2.2× was measured before the partition got two to three times faster.

It is gated on a measured cost (the sum over crossings of mass x ceil(log2 of the strands live there)): at most 1.6n
for parallel sorts and 3n for sequential ones, because with the gate lifted it stops paying only from three to four
live strands up. `-DLOGOS_NO_KNOT` removes it, `LOGOS_NO_KNOT=1` disables it at runtime, `make knottrace` prints
which route took each bench shape. f64 only. Full account, including three wrong turns of its own: CHANGELOG.

**Descending runs are consumed as they lie** (the Moebius twist, `make twistbench`). The detector used to swap a descending
run end for end the moment it found one. Now the run's descriptor carries one orientation bit and nothing is reversed: a lone
piece is placed by a backward copy, a crossing is merged with the twisted strand's cursor walking down, the plan cuts in
logical coordinates, and the detector never writes to the array. Against `LOGOS_NO_TWIST=1` in one binary at 8.4M: 1.15x at
one thread and 1.11x at 14 when every block lies descending, 1.06-1.07x when every other one does, a tie when four elements in
five are merged, guards 0.98-1.03. `logos_twist_last()` tells a test how many twisted strands a placement consumed.

**The detector scans eight elements at a time.** Because it no longer writes, a run is extended in blocks of eight whenever a
bound proves the scalar test would have accepted each element; otherwise the scalar line runs, so the runs found are the same.
1.43-1.49x at one thread on block shapes and 1.16-1.22x at 14 (`./twistbench 60 8388608 scan`, `LOGOS_NO_FASTSCAN=1` = the scalar
scan). With it the run routes beat the pure partition at 14 threads again: 1.08 on block_perm, 1.17 on a staircase.

**One orientation bit for the whole array.** A sequential sort whose mirror image would take a cheap route (a descending suffix
of half the array, short reach read back to front, streams that descend in arrival order) is reversed once, behind relocation,
and dispatched again: 2.07x, 2.20x and 2.76-2.81x on those three at 8.4M, every guard 0.98-1.02 (`./mirrorprobe 40 8388608 ab`,
`LOGOS_NO_MIRROR=1` = off, `logos_mirror_count()` for tests).

**A sparse strand over dense blocks gallops.** A crossing whose two pieces differ fourfold in length is merged in steps of eight
while the strands alternate and by a gallop plus one bulk copy when one keeps winning: a late column over dense pages sorts
2.45-2.70x faster at one thread and 1.18-1.31x at 14 (`./knotbench 60 8388608 gallop sparse`, `LOGOS_NO_KNOT_GALLOP=1` = off).

**Distinct integers spanning up to 16n sort by presence bits** (it was 4n): 3.7-5.0x at 8.4M and 6.0-8.7x at 1M on spans 6n to 16n
at one thread (`./mcbench 40 8388608 span - 4`; `LOGOS_DENSE_SPAN=<k>` sets the budget).

**A kink is swapped back where it stands.** One adjacent pair swapped inside a run that otherwise goes on (`.. 4 6 5 7 ..`) used
to end the run; sorted blocks with a few thousand of them paid for a full sort. The detector asks both neighbours and swaps the
pair if they agree: 5.8x at 0.1% kinks, 5.0x at 1%, 3.2x at 4.8% at one thread, 1.3-1.5x at 14 up to 1%, guards 0.98-1.01
(`./strandprobe 40 8388608 kinkab`, `LOGOS_NO_KINK=1` = off).

**A ring buffer is one strand across the wrap.** A sorted array read from the wrong slot (a ring buffer dumped from slot 0, a
log that wrapped) is two runs in the plane and one on a ring. When the array ends in an ascending run that continues into its
ascending prefix (last element no larger than the first), the engine rotates it to the front once: 1.56-1.63x at one thread and
1.90-2.03x at 14 on a ring buffer, 1.73-1.75x on a rotated sorted prefix with a random tail, guards 0.97-1.02
(`./unknotprobe 40 8388608 ab LOOP`, `LOGOS_NO_LOOPBACK=1` = off, `logos_twist_last(7)` for tests).

**Blocks of a few thousand elements reach relocation at one thread.** The run detector used to give up on runs averaging under
4096 (the run merge's constant): disjoint blocks of 1000 now sort 4.2x faster, keyed runs of 3000 5.6x (`./unknotprobe 40 8388608
ab FLOOR`; `LOGOS_RELOC_AVG=<k>` sets the floor).

**Strays inside strands are lifted, not sorted around.** Sorted blocks with late records used to pay for a full sort. At one
thread the run detector lifts strays out of the runs, the runs are placed as before, and the sorted strays are merged back in
place of the copy-back pass: 4.0x at 0.1% strays, 3.0-3.3x at 1% (`./unknotprobe 40 8388608 ab STRAY`, `LOGOS_NO_STRAY=1` = off).

**A wave with drift is sorted piece by piece.** A seasonal series is monotone at the sample's scale and noisy at the element's;
its pieces are sorted where they lie and placed by the knot sweep: 1.11-1.24x at one thread where the sample prices the
crossings low (`... ab WAVE`, `LOGOS_NO_WAVE=1`). **A noisy ring buffer** shows as one drop in the short-reach route's block
medians: one rotation, then that route, 1.12-1.16x (`... ab SEAM`, `LOGOS_NO_SEAM=1`).

**Server timestamps.** `make tsbench` sorts thirteen log and sensor shapes (network latency, retransmits, clock skew, overlapping
batches, ring buffers, newest-first logs, telemetry with dropouts): 3.3-6.0x over std::sort at one thread on the latency shapes and
16x on overlapping batches, and 3-6x over a merge sort that skips merges whose halves are already in order (its check never fires
on latency). One sorted strand with kinks is finished as one run (5.5x, `LOGOS_NO_ONERUN=1` = off).

**Uniform data as a complete computation.** `make unibench XSS=../x86-simd-sort/src` sorts P chunks with the AVX2
quicksort and merges them with a merge-path tree, against the shipped partition: the chunk sorts alone beat the engine
(0.76-0.86 of its time), and the merge then costs about three times the engine's whole runtime, so the pipeline lands at
3.3-4.2x. The same 3.86n of movement costs 15 ms when chunks are disjoint and 87 ms when they overlap: a merge is
comparison, and on a uniformity certificate the engine computes each bucket with one multiply-add instead.

**Stitching runs in place order.** (implementation REMOVED 2026-09-23; in git history before that commit) `./tsbench 16 1048576 moebius` built a handed-off pipeline (scan, twist, inline swap for 2-3
element dips, a fallback under 16, a galloping stitch behind an O(1) boundary check): behind the engine on 14 of 16 shapes, and
quadratic where the runs stitch into one growing prefix (a ring buffer 58-68 s against 8 ms). Its one win was an engine defect, now
fixed: the run detector's block of eight compares with vectors (kinks 1.3x, shuffled blocks 1.3x at one thread).

**Welding blocks in place.** (implementation REMOVED 2026-09-23; in git history before that commit) `./tsbench 16 1048576 weld` built "condense into runs, meta-sort them by min edge, permute them in
place by triple reversals, weld the seams behind an O(1) boundary check": 2-4,600x behind the engine on 14 of 16 shapes, because
the boundary check never fires (maximal runs span holes) and the merge behind it is quadratic. Its two wins were engine defects, now
fixed: the prescan compares with vectors (sorted input 1.7-2.2x) and the run detector resumes at a wrong-way step (kinks 1.05-1.08x).

**Sorting blocks by their edges.** `./tsbench 16 8388608 edge` builds the proposal "condense into runs, sort the runs by their min edge,
lay them out, merge": 1.4-9.9x behind the engine on 13 of 16 shapes, because on noisy data the runs are 2.4 elements long and a
maximal run's min edge can name the wrong block. Its three wins were engine defects, now fixed: the prescan reads sixteen elements at a
time (sorted input 2.2x), a tie no longer ends a descending run (tied descending blocks 6x), and one strand's strays go back in place
(2.4x).

**Is the array converted to a linked list? No.** The route was first described that way; it is an array of run descriptors.
The literal version exists, opt-in (`LOGOS_KNOT_LIST=1`): a 32-bit link per element, twisted runs linked backwards, crossings
untied by relinking, the sorted list walked out. It measures 0.50-0.75 of the shipped route at one thread and 0.62-0.69 at 14,
for 4 more bytes per element (`./twistbench 60 8388608 list`), and is kept because the measurement needs the code.

## Disjoint-run relocation (J5): block-permuted data without a merge

Sorted blocks concatenated out of order (merged log partitions, time-series
shards) are runs whose value ranges mostly do not overlap, so merging them is
comparison work for nothing. The engine now sorts the run *descriptors*, groups
overlapping runs into chains, memcpys every run to its chain-ordered place and
merges only inside multi-run chains. Chunk-parallel probe with a sampled
pre-gate and per-thread early bail; interleaved runs stay on the run-merge.
Interleaved A/B (`make j5bench`, 8M, loaded box so margins are loose):

| shape | 1T | 20T |
|---|---|---|
| 64 shuffled disjoint blocks | 2.82× | 2.03× |
| 1024 shuffled disjoint blocks | 3.39× | 2.18× |
| 64 blocks, a quarter in 2-run chains | 2.34× | 1.89× |
| segmented / few_runs / near_sorted / uniform (guards) | 0.96–0.99× | 0.95–1.05× |

Note that the harness's `segmented` and `few_runs` shapes are *interleaved*
runs (every block spans the whole range), which is this path's worst case, not
its target; `block_perm` and `block_overlap` are the target shapes. Build with
`-DLOGOS_NO_RELOCATE` for the pre-J5 engine, `-DLOGOS_TRACE` for per-phase
timestamps on stderr. Full account, including the two wrong turns: CHANGELOG.

## 3.4.0: memory-level parallelism (index cache + prefetch)

The 10-vs-20-thread experiment showed the scatter is latency-bound, not
bandwidth-saturated. Two changes attack that: the histogram pass caches each
element's bucket index (the scatter no longer recomputes it), and every scatter
prefetches the destination line 32 elements ahead so a core keeps many misses
in flight. Interleaved A/B on one core: parallel path -19..-25%, sequential
-7..-13% on scatter-heavy shapes, neutral on the counting paths. See CHANGELOG.

> **Where this stands against the state of the art (important, measured).**
> Against Intel's `x86-simd-sort` (AVX-512 vectorized quicksort — the SIMD state
> of the art, and NumPy's sort kernel), logos-sort **loses single-threaded on
> every shape, by 1.1x-3.8x** (8M, this CPU): worst on comparison-friendly random
> data (uniform/heavy-tail/bimodal ~3x), closest on the structured shapes it is
> built for (near-sorted 0.90x, quantized 0.86x). A vectorized quicksort
> partitions with `vpcompress`, which SIMD accelerates and logos-sort's scatter
> cannot match. So the honest claim is: **logos-sort beats scalar and OpenMP
> parallel sorts (qsort, std::sort, __gnu_parallel::sort) but is beaten by
> AVX-512 vectorized sorts on a single core.** Its remaining edge over the SIMD
> sorts is *thread scaling* (they are single-threaded; logos is parallel) and
> data shapes SIMD cannot exploit — a genuine but narrow niche. Run
> `make parbench-simd` to see it. Full matrix: PARITY.md.

## Fair-fight results after J5 + H0b (2026-09-10, same 20-CPU machine, loaded — read ratios, not times)

| shape | std::sort 1T | logos 1T | | gnu_par 20T | logos 20T | |
|---|---|---|---|---|---|---|
| uniform | 1093 ms | 256 ms | 4.3× | 197.5 ms | 24.3 ms | **8.14×** |
| near_sorted | 231 | 82 | 2.8× | 115.8 | 33.4 | **3.47×** |
| few_distinct | 189 | 50 | 3.8× | 30.7 | 8.3 | **3.71×** |
| heavy_tail | 1103 | 278 | 4.0× | 167.4 | 43.9 | **3.82×** |
| bimodal | 1062 | 314 | 3.4× | 184.6 | 54.4 | **3.39×** |
| quantized | 778 | 37 | 20.8× | 110.3 | 16.8 | **6.55×** |
| int_wide | 1032 | 108 | 9.6× | 156.8 | 31.5 | **4.97×** |
| block_perm | 581 | 33 | 17.8× | 122.5 | 17.2 | **7.13×** |
| block_overlap | 550 | 40 | 13.9× | 58.1 | 13.1 | **4.45×** |

**9 wins, 0 losses**, every parallel row ≥3.4× (was ≥1.74× at 3.6.0). The box
was loaded (calibration ~2× off quiet), so absolute times are inflated and the
within-run ratios are the result. SIMD columns not re-measured here.

## Fair-fight results (3.6.0, quiet 20-CPU machine — verified by calibration)

| shape | std::sort 1T | logos 1T | | gnu_par 20T | logos 20T | |
|---|---|---|---|---|---|---|
| uniform | 573 ms | 121 ms | 4.7× | 81.9 ms | 42.8 ms | **1.91×** |
| near_sorted | 103 ms | 37 ms | 2.8× | 41.1 ms | 22.5 ms | **1.82×** |
| few_distinct | 150 ms | 26 ms | 5.8× | 25.1 ms | 8.3 ms | **3.00×** |
| heavy_tail | 560 ms | 149 ms | 3.8× | 71.6 ms | 41.1 ms | **1.74×** |
| bimodal | 568 ms | 162 ms | 3.5× | 80.5 ms | 38.7 ms | **2.08×** |
| quantized | 364 ms | 36 ms | 10.1× | 59.2 ms | 8.8 ms | **6.71×** |
| int_wide | 562 ms | 84 ms | 6.7× | 76.5 ms | 38.5 ms | **1.98×** |
| int32 | 510 ms | 86 ms | 5.9× | 86.0 ms | 34.0 ms | **2.53×** |

**8 wins, 0 ties, 0 losses** vs GCC's OpenMP `__gnu_parallel::sort` at equal
thread counts; every row ≥ 1.74×. History: 3.1.0 4-4 · 3.2.0 6-1-1 · 3.3.2
7-1-0 · 3.4.0 8-0-0 · 3.6.0 8-0-0 with wider margins on six rows.

What 3.6.0's four experiments taught, graded against predictions made before
the run: the lattice path landed where predicted (quantized 14.8 -> 8.8 ms);
the near-sorted index-partition merge helped (28 -> 22.5) but less than
predicted — the P-way heap merge kernel is the next thing to tune; and the
random-data floor moved only ~12% despite ping-pong removing ~37% of memory
traffic, most of that gain traceable to the scratch pool. So at ~40 ms on 20
threads the random-data floor is per-core scatter latency plus fixed cost,
**not** bandwidth — a correction to the earlier traffic-based model. All
outputs verified against `std::sort`; read the calibration line before the table.

## 3.4.0: memory-level parallelism (index cache + prefetch)

The 10-vs-20-thread experiment showed the scatter is latency-bound, not
bandwidth-saturated. Two changes attack that: the histogram pass caches each
element's bucket index (the scatter no longer recomputes it), and every scatter
prefetches the destination line 32 elements ahead so a core keeps many misses
in flight. Interleaved A/B on one core: parallel path -19..-25%, sequential
-7..-13% on scatter-heavy shapes, neutral on the counting paths. See CHANGELOG.

## Fair-fight results (3.4.0, quiet 20-CPU machine — verified by calibration)

| shape | std::sort 1T | logos 1T | | gnu_par 20T | logos 20T | |
|---|---|---|---|---|---|---|
| uniform | 578 ms | 145 ms | 4.0× | 84.9 ms | 48.6 ms | **1.75×** |
| near_sorted | 102 ms | 37 ms | 2.8× | 44.2 ms | 28.2 ms | **1.57×** |
| few_distinct | 155 ms | 30 ms | 5.3× | 26.0 ms | 6.5 ms | **4.01×** |
| heavy_tail | 569 ms | 166 ms | 3.4× | 69.2 ms | 47.1 ms | **1.47×** |
| bimodal | 557 ms | 171 ms | 3.3× | 70.2 ms | 44.5 ms | **1.58×** |
| quantized | 367 ms | 83 ms | 4.5× | 46.5 ms | 14.8 ms | **3.13×** |
| int_wide | 559 ms | 92 ms | 6.1× | 68.2 ms | 40.6 ms | **1.68×** |
| int32 | 503 ms | 81 ms | 6.2× | 85.6 ms | 32.7 ms | **2.62×** |

**8 wins, 0 ties, 0 losses** against GCC's OpenMP `__gnu_parallel::sort` at
equal thread counts. History against the same opponent: 3.1.0 4-4, 3.2.0
6-1-1, 3.3.2 7-1-0, 3.4.0 8-0-0. The 3.4.0 step (index cache + destination
prefetch) moved the pure-scatter floor from ~65 ms to ~41-49 ms — a larger
gain on 20 threads than on one core, as predicted, because the 10-vs-20-thread
experiment had shown the scatter to be memory-latency-bound. All outputs
verified against `std::sort`. Read the calibration line before the table.

## 3.3.2: spin-then-block barrier

Chunking (3.3.1) fixed work imbalance but not barrier wake-up latency: a
barrier that sleeps immediately makes every phase pay scheduler wake-ups for
all threads, which on a loaded WSL2 box costs milliseconds each. The barrier
now spins briefly on an atomic generation counter before sleeping (as libgomp
does), so threads that arrive together never sleep. `parbench` takes
`LOGOS_THREADS=N` — on a hyperthreaded box try the physical core count.

## 3.3.1: straggler tolerance — and how to recognize a bad run

A run on a loaded machine (unchanged `std::sort` and `__gnu_parallel::sort`
1.5-2x slower than in quiet runs; logos on 20 threads slower than on 1, which
cannot happen on an idle box) exposed a real weakness: with one fixed slice per
thread, every barrier-synchronized phase waited for the slowest thread, so
background load hurt logos 2-5x but the dynamically-scheduled opponent only
1.3-2x. All phases now pull small chunks from an atomic counter; a slow thread
does fewer chunks instead of stalling everyone. `parbench` now prints a
calibration line — if memcpy/std::sort drift >15% between runs, discard the run.

## 3.3.0: moderate cardinality, and why samples are fixed-size

Extends the zero-scatter counting idea to thousands of distinct values via a
capped per-thread hash map (see `modcard_try_f64`). Measured crossover on 8M,
sequential: 2.5x at 256 distinct, 1.4x at 4096, ~1.2x to 32k, then the gate
declines with no penalty. Write-combining is now runtime-gated (on at >=8
threads) after the 20-core run showed it loses sequentially and wins in
parallel.

**Fixed sample vs. a percentage of n.** It depends on what the sample
estimates. Proportions and quantiles (splitters, skew, "fraction of repeats")
have error that depends on the *absolute* sample size, not the sampling
fraction — 2048 samples pin a proportion to ~±2% at any n — so a fixed sample
is right and a percentage wastes work at large n. What a sample *should* scale
with is the decision's resolution: the splitter sample is 32 per bucket. For
cardinality a fixed sample saturates (2048 samples can't tell 4k distinct from
8M) and a percentage doesn't fix it; the right instrument is a capped first
pass with early abort, whose cost is bounded by the cap, not n.

## 3.2.0: what the 20-core fair fight changed

On 20 threads logos and `__gnu_parallel::sort` **converged on the same ~70 ms**
(logos scaling 2.1x from a 3.8x-better start; GCC scaling 7.7x from a slower
one) — the memory-bandwidth wall made visible. The sequential advantage does
not survive parallelism because the parallel regime is bound by bytes moved and
fixed costs, not by the algorithm. So 3.2.0 leverages parallelism by moving
fewer bytes and balancing better, not by parallelizing more:

- **native int32** (no widening; half the bytes) — targets the int32 loss
- **low-cardinality parallel counting** (balanced; equal keys defeat quantile
  splitters) — targets the few_distinct loss; also 130 -> 49 ms sequential
- **write-combining scatters, opt-in** (`make WC=1`) — loses 8-23% on one core
  (L2-resident destinations), may win where the scatter is DRAM-bound. Measure
  it on the many-core box with `make clean && make parbench WC=1 && ./parbench`.

Still on the list: index-partition + parallel merge for near-sorted data (its
20-thread scaling was ~1.0x because value-partitioning destroys the order the
run-merge exploits), eliminating the copyback pass, and a persistent buffer
pool to remove per-call page-fault cost.

## The fair fight (optional): vs std::sort and OpenMP __gnu_parallel::sort

`qsort` is single-threaded and slow (a function call per comparison), so the
harness's speedups overstate the gap against real opponents. `parbench.cpp`
races logos against **`std::sort`** (inlined introsort, single-threaded) and
**`__gnu_parallel::sort`** (GCC's OpenMP parallel sort) at the *same thread
count*, verifying every output against `std::sort`.

```sh
make parbench && ./parbench      # needs g++ with OpenMP
```

Single-core sandbox, 8M, sequential column only (the parallel column needs
real cores):

| shape | std::sort | logos v3 (1T) | |
|---|---|---|---|
| uniform | 657 ms | 242 ms | 2.7× |
| near_sorted | 127 ms | 51 ms | 2.5× |
| heavy_tail | 678 ms | 270 ms | 2.5× |
| quantized | 452 ms | 154 ms | 2.9× |
| int_wide | 620 ms | 195 ms | 3.2× |
| bimodal | 620 ms | 319 ms | 2.0× |
| few_distinct | 157 ms | 132 ms | 1.2× |
| int32 uniform | 541 ms | 228 ms | 2.4× |

Honest cross-read: Rust's `sort_unstable` (ipnsort) ran ~2× faster than
`std::sort` on this same machine, so against an ipnsort-class sort these
margins shrink to roughly 1.1–1.5× on uniform data, stay large on structured
shapes (near-sorted, heavy tail, int-wide), and **`few_distinct` becomes a
loss** — pdqsort-family sorts handle few distinct values in near-O(n), while
the JS counting gate here allocates a 4M-entry count array. Retuning
`COUNTING_SORT_K` for C is the fix.

## Correctness

`harness.c` checks every (shape × size × thread count) against `qsort` element
-for-element plus an XOR-of-bits permutation checksum. Fifteen shapes, several
targeting one ported path each: `segmented` (4-way merge), `few_runs` (gallop),
`prefix_shuffle` (momentum abort), `bimodal` (gap-split), `quantized` (float
radix), `int_wide` (int32 radix), `heavy_tail` (bits coarse). **570 checks,
0 failures.**

## Results vs qsort (single-core sandbox, 8M, sequential)

| shape | v2 (interp leaf) | **v3 (JS-faithful leaf)** |
|---|---|---|
| near_sorted | 3.9× | **11.0×** — run-merge + gallop now exploit order |
| heavy_tail | 0.75× (loss) | **5.5×** — skew probe → bits coarse |
| prefix_shuffle | — | **8.0×** — no cliff (momentum budget) |
| quantized | — | **8.7×** (12.8× at 1M) — float radix |
| int_wide | — | **7.0×** (11.5× at 1M) — int32 radix |
| bimodal | — | **4.5×** after deviation #2 (1.9× before) |
| uniform / clustered | 6.1× / 6.0× | 6.1× / 6.2× (same coarse design) |
| few_distinct | 7.8× | 5.3× — see residuals |

## Honest residuals

- **`few_distinct` regressed** (70 → 146 ms). Those values are integer-valued
  with span ≈ 0.5n, so the JS counting gate fires with a 4M-entry count array
  and cache-unfriendly increments. Faithful to JS — whose gate is tuned against
  a JS radix — but the old value-coarse leaf handled this shape better in C.
  The gate constant is *language-relative* (a finding from the Python port);
  retuning `COUNTING_SORT_K` for C is the fix.
- **Partitioning forfeits order.** On the parallel path, near-sorted and
  segmented shapes lose most of their sequential advantage (the partition
  scatters the structure the run-merge exploits; each bucket must rediscover
  it). A top-level near-sortedness check routing to a parallel merge instead of
  a partition is the right follow-up.
- `logos_sort_i32` still widens to double (exact); the dispatcher then takes
  the counting / int32-radix paths, but the two conversion passes remain.
- `t>1` numbers on a single core measure coordination overhead only.
