# view-analytics: the sorted view becomes an analytics API (batch quantiles + the int32 twin)

Agent: view-analytics. Work copy: /tmp/logos-team/view-analytics/work (WSL). Nothing under the real
repo's engines/ or tools/ was modified; the patch is experiments/team/patches/view-analytics.patch
(10 text files, applies with `patch -p1` from the repo root, dry-run verified against a fresh copy).

## What was tried

(a) `logos_view_quantiles(v, q, m, out)` and its rank-space primitive `logos_view_ranks(v, ks, m, out)`:
    m cut points in one pass. The ranks are sorted, the bucket walk is monotone (O(nbk + m), no
    per-cut binary search), each touched bucket is forced ONCE however many cuts share it, and when
    more than one bucket is touched they are sorted IN PARALLEL through `logos_sort_buckets_f64` with a
    temporary done-mask (every bucket outside the touched set marked done). `logos_view_range` and
    `topk` go through the same set-force. `-DVIEW_PAR_FORCE=0` keeps the forcing serial.
(b) The int32 twin: `logos_partition_i32` (a lazy flag on the old histogram path's `do_buckets_i32`:
    place the bucket, record its min/max, skip `leaf_i32`), `logos_sort_buckets_i32`, and the whole
    `logos_view_*_i32` family generated from the same source as the f64 one (`logos_view_impl.h`,
    included twice by `logos_view.c`). When `count_try_i32` fires (span <= 2^18) the array is already
    sorted and the partition returns 0 buckets, exactly like the f64 one; the sequential leaf, the
    prescan and the qsort fallback do the same.
(c) Found on the way and fixed: `logos_sort_buckets_f64`'s workers each called `ctx_init` from inside
    the thread, and `ctx_init` advances the context-pool cursor `g_ctx_next` with a plain `++`, so two
    workers could be handed the same pooled scratch buffers. `hf_partition` avoids exactly this by
    initialising its P contexts on the calling thread before spawning; both bucket sorters now do that.

Files: engines/c/logos_view_impl.h (new, the body), logos_view.c (two includes), logos_view.h (API),
logos_psort.c (`psort_i32_ex` with lazy mode, `logos_partition_i32`, `logos_sort_buckets_i32`, the
context fix in `logos_sort_buckets_f64`, one TRACE line), logos_psort.h, harness.c (+288 checks),
viewbench.c (quantile rows, three paired lines, failure diagnostics), viewbench_i32.c (new), Makefile
(`viewbench_i32` target, `logos_view_impl.h` dependency), CHANGELOG.md (entry).

## Prediction (written before any measurement, reproduced verbatim)

8M elements, 14 threads, ratios are sort-then-look / view-route, min of reps within one row:

| row | uniform | heavy_tail | bimodal | clustered |
|---|---|---|---|---|
| f64 quantiles(10) vs sort-then-index | 2.2-2.6x | 1.4-1.7x | 1.4-1.7x | 1.4-1.7x |
| f64 quantiles(10) vs naive 10x logos_view_at | 1.03-1.12x on every shape (see below) |

The prediction the brief gave (2.0-2.8x over sort-then-index) I expect to hold only on uniform;
the other shapes' view+median rows sit at 1.6x and ten cuts cost nine more buckets than a median.

The naive loop is NOT ten times more work: `force()` is already idempotent per bucket, so the
only thing the batch API can buy over m calls to `logos_view_at` is (1) sorting the touched
buckets in parallel instead of serially (10 x ~48k elements x ~7 ns = ~3.3 ms serial vs ~0.5 ms
parallel, against a ~20 ms partition, so ~1.10x) and (2) ten binary searches over 168 entries,
which is nothing. So 1.03-1.12x, below the unpaired resolving floor: it will be measured with the
paired loop (bench_paired, both routes in ONE binary so there is no layout bias between them).

int32, 8M, 14 threads:

| row | span 1e8 | full range | span 2e5 (counting fires) | clustered |
|---|---|---|---|---|
| i32 view + median | 1.4-1.9x | 1.4-1.9x | 0.95-1.0x (no gain, array is already sorted) | 1.2-1.6x |
| i32 quantiles(10) | 1.3-1.7x | 1.3-1.7x | ~1.0x | 1.1-1.5x |

Lower than the f64 ratios because the int32 leaf is a 3-4 pass LSD radix (~2-3 ns/element) and the
int32 partition is the OLD histogram path, whose bucket lookup is a binary search over 167
splitters on both the histogram and the scatter pass, so the leaf share of an i32 sort should be
well under the 63% traced for f64. If the i32 ratios come out ABOVE the f64 ones the leaf is
costlier than I think and the profile numbers for int32 are worth a look.

## What was measured

Box: the reference i9-13900H under WSL2, 14 threads, 8M elements, every run under the bench lock.
`make pairprobe` on the day: the same engine on both sides resolves 2.8% at 8M and 9.7% at 1M
(median of 40 pairs; a single pair's p10/p90 is 0.88-1.14 at 8M). Every timing below is verified
against qsort first (all `verify: ok` in the runs reported; the one exception is discussed under
"surprising"). The box drifted 2-4x in absolute ms between runs (f64 uniform eager sort: 33.6, 19.4,
66.4 ms across the three runs); only the paired ratios are comparable, and they are.

### f64, paired (median of tA/tB over adjacent pairs, >1 = the view route is faster), three runs
Run 1: 20 pairs; run 2: 30 pairs; run 3: 30 pairs (run 3 on the most loaded box of the three).

| shape | sort-then-index / view+quantiles(10) | 10x view_at / view+quantiles(10) | eager sort / partition only (run 3) | deferred leaf share |
|---|---|---|---|---|
| uniform | 1.45, 1.67, 1.53 | 1.39, 1.40, 1.25 | 1.70 [p10 1.49, p90 2.04] | 41% |
| heavy_tail | 1.29, 1.53, 1.41 | 1.35, 1.22, 1.19 | 1.70 [1.26, 1.88] | 41% |
| bimodal | 1.63, 1.63, 1.56 | 1.35, 1.34, 1.28 | 1.79 [1.60, 2.07] | 44% |
| clustered | 1.34, 1.37, 1.32 | 1.17, 1.26, 1.17 | 1.47 [1.29, 1.59] | 32% |

p10/p90 of the first two columns in the final run: uniform 1.33/1.95 and 1.04/1.40; heavy_tail
1.25/1.76 and 0.96/1.47; bimodal 1.34/1.80 and 1.08/1.48; clustered 1.18/1.73 and 0.99/1.61.

### f64, the min-of-7 table (final run, for the shape of the cost model; not for ratios under 1.25x)

| 8M, 14T, ms | uniform | heavy_tail | bimodal | clustered |
|---|---|---|---|---|
| logos_sort_f64 | 66.4 | 92.7 | 81.4 | 128.4 |
| build a view, look at nothing | 31.9 (2.08x) | 48.4 (1.91x) | 41.2 (1.98x) | 79.5 (1.62x) |
| view + median | 34.7 (1.91x) | 51.7 (1.79x) | 45.0 (1.81x) | 84.9 (1.51x) |
| view + 10 quantiles (batch), 10 buckets | 37.3 (1.78x) | 71.9 (1.29x) | 52.3 (1.56x) | 86.7 (1.48x) |
| view + 10 quantiles (10x at), 10 buckets | 60.4 (1.10x) | 76.8 (1.21x) | 64.4 (1.26x) | 104.1 (1.23x) |
| view + 16 random ranks, 14-15 buckets | 66.4 (1.00x) | 90.4 (1.03x) | 69.7 (1.17x) | 110.0 (1.17x) |
| view + materialise everything | 92.3 (0.72x) | 113.6 (0.82x) | 104.3 (0.78x) | 153.6 (0.84x) |

### int32, paired, four runs (20, 20, 20, 30 pairs; run 1 is before the context fix)

| shape | buckets | sort-then-index / view+quantiles(10) | 10x view_at / view+quantiles(10) | eager sort / partition only (run 4) | leaf share |
|---|---|---|---|---|---|
| span_1e8 | 168 | 1.12, 1.12, 1.09, 1.04 | 1.06, 1.04, 1.03, 1.04 | 1.11 [0.93, 1.23] | 10% |
| full_range | 168 | 1.12, 1.11, 1.11, 1.04 | 1.08, 1.06, 1.07, 1.04 | 1.18 [1.05, 1.41] | 15% |
| span_2e5 (counting route) | 1 | 1.02, 1.01, 0.99, 0.97 | 1.01, 1.01, 0.98, 0.93 | 1.01 [0.76, 1.68] | 1% |
| clustered | 168 | 1.07, 1.10, 1.05, 1.03 | 1.08, 1.05, 1.03, 1.03 | 1.08 [0.98, 1.20] | 7% |

The min-of-7 "view + median" row for int32 was 1.17/1.09/1.19/1.16 (span_1e8) and 0.85/1.08/1.20/1.14
(full_range) across the runs: it wanders around 1.1x, which is what the paired columns say too.

### The mechanism, measured directly (va_probe, 1 thread, 47,619-element bucket of 8M uniform)

| what | ms | ns/element |
|---|---|---|
| force one bucket through the view | 1.44-2.10 | 30-44 |
| logos_sort_f64 on a HOT copy of 47,619 elements | 1.36-1.50 | 28-31 |
| logos_sort_f64 on a COLD copy | 1.30-1.65 | 27-35 |
| ten serial forces (the naive loop's extra work) | 13.3-14.8 | 28-31 |
| the same ten as one batch (10 threads) | 5.1-5.4 | |

Compiled with -DLOGOS_TRACE, the int32 lazy path prints `i32: lazy partition, 168 buckets placed
unsorted, leaf_i32 skipped` on every create, and the benches' "buckets sorted" column (from
`logos_view_materialised`) shows the batch touching exactly 10 buckets for 10 cuts.

## Verdict

**Shipped, with an honest split.** The f64 batch quantiles are a real win: 1.29-1.67x over
sort-then-index and 1.17-1.40x over ten single lookups, paired, on all four shapes, every ratio above
the 2.8% floor and the p10 of the sort-then-index comparison above 1.18 on every shape. The int32 view
works (288 harness checks, byte-identical materialisation) but is **near-neutral as a speedup**:
1.02-1.12x on the partitioned shapes and 0.97-1.02x on small-span data, so it ships as an API whose
measured gain is documented as small, not as a performance feature. The context-pool race fix is a
correctness change that applies to the already-shipped f64 bucket sorter.

Grading the predictions:
- f64 quantiles(10) vs sort-then-index, uniform 2.2-2.6x predicted: **wrong, measured 1.45-1.67x.**
  heavy_tail/bimodal/clustered 1.4-1.7x predicted: right on bimodal (1.56-1.63) and on the upper
  runs of heavy_tail (1.29-1.53), slightly high on clustered (1.32-1.37). The brief's 2.0-2.8x did
  not happen anywhere.
- f64 batch vs naive loop, 1.03-1.12x predicted: **wrong in the other direction, measured 1.17-1.40x.**
- int32 view 1.4-1.9x predicted on span-1e8 / full-range: **wrong, measured 1.04-1.12x.** Small-span
  "no gain" predicted: right (0.97-1.02x). The direction ("lower than f64 because the leaf share is
  small") was right; the magnitude of the leaf share was not: it is 7-15%, not "well under 63%".

## Why, mechanism

- **One forced bucket costs ~1.4 ms at one thread, 30 ns/element**, and that is the engine's own price
  for sorting 47k elements (a plain `logos_sort_f64` on a hot copy costs the same), not view overhead.
  Ten of them serially are ~14 ms against a partition of ~15-20 ms on a quiet box, which is why the
  naive loop loses 1.17-1.40x to the batch (I had assumed ~0.3 ms per bucket and predicted 1.1x) and
  why the batch cannot reach 2x over sort-then-index: even in parallel the ten buckets cost ~5 ms
  (memory-bound leaves contend; 10 threads give ~2.7x over serial, not 10x).
- **The deferred share is the whole story.** Paired eager-sort / partition-only is 1.47-1.79x for
  f64 (leaf share 32-44% on the loaded box; the project's trace said 63% on a quiet one) and only
  1.01-1.18x for int32 (leaf share 1-15%). The int32 engine still runs the pre-H0b histogram path:
  `bucket_of_i32` is a binary search over 167 splitters on the histogram pass and the scatter reuses
  the byte cache, and the leaf is a 3-4 pass LSD radix on 4-byte L2-resident data. A view can only
  save the leaf, so on int32 there is little to save. The barrier-heavy partition also suffers more
  from load than the leaves do (CLAUDE.md, the loaded-box note), which is why the f64 leaf share
  reads 32-44% here instead of 63%; on a quiet box the f64 ratios should be higher, not lower.
- The batch's mechanism is confirmed by the materialised count (10 buckets for 10 cuts) and by the
  serial-vs-batch timing above; the int32 lazy path by the TRACE line.

## What I would do next

1. **Port H0b and the histogram-free partition to int32.** The paired leaf-share numbers say the int32
   partition is 85-93% of an int32 sort, and the f64 engine's history says the direct-index splitter
   table alone was 1.6-3.0x on that phase. That is the int32 lever; the view is not, until it is done.
   It would also lift the int32 view to f64-like ratios for free.
2. **Cheaper single forces.** 30 ns/element for a 47k leaf at one thread is 2.5x the AVX2 leaf's
   whole-array rate (12 ns/element at 8M); a `-DLOGOS_TRACE` pass over `leaf_f64` on a 47k bucket
   would show whether the dispatcher's probes (sample sort, inversion probe, window probe) are a
   fixed cost worth skipping when the caller already knows the bucket is unstructured.
3. **Keep the regions alive** (the design choice CHANGELOG already names): the lazy copy-back is one
   extra pass the eager path never makes and is why materialise-all sits at 0.72-0.84x.
4. `bench_paired` for the view's other rows (median, top-k) so the CHANGELOG's 2.85x-style numbers get
   the same treatment; the min-of-7 table moved 1.29x to 0.92x on heavy_tail between runs.
5. `TRACE_START()` is only called by `psort_f64`, so `psort_i32`'s trace timestamps are absolute;
   cosmetic, left alone because the spill recursion re-enters `psort_i32`.

## Anything surprising

- **One verify failure, not reproduced.** The first `viewbench_i32` run reported `clustered ... verify:
  FAILED` (the bench then only recorded a boolean). Every check on the identical data passed in a
  standalone probe; a 120-repetition stress of `logos_view_all` / `range(0, n)` at 14 threads found
  no corruption; after moving `ctx_init` out of the workers (the race described above) three further
  full runs and the 288 harness checks were clean. The benches now print the source line and rep of
  the first failed check, so a recurrence will be diagnosable. The race is the likely cause and the
  fix is correct on its own terms, but "likely" is the honest word.
- The scratchpad directory is shared between the agents: two of my patch scripts were overwritten by
  another agent's after they had run (harmless, but every later file got a `va_` prefix).
- A batch API over an idempotent per-bucket force cannot reduce the buckets touched; its entire value
  is parallelism among the touched set, and that turned out to be worth more (1.17-1.40x) than the
  arithmetic predicted because the per-bucket cost was 4x what I assumed. Measure the unit cost first.

## Commands (decisive measurement)

```
# set up (once)
wsl -e bash -lc "cd /tmp/logos-team/view-analytics/work/engines/c && make -s clean; make test 2>&1 | tail -6"
# calibration, then the two benches, all under the bench lock (8M, 14 threads, min of 7, 30 pairs)
wsl -e bash -lc "cd /tmp/logos-team/view-analytics/work/engines/c && make -s pairprobe viewbench viewbench_i32 && flock /tmp/logos-bench.lock sh -c './pairprobe; ./viewbench 7 8000000 30; ./viewbench_i32 7 8000000 30'"
# the mechanism: per-bucket force cost, serial vs batch, and the TRACE proof of the int32 lazy path
wsl -e bash -lc "cd /tmp/logos-team/view-analytics/work/engines/c && cc -O3 -std=gnu11 -pthread -DLOGOS_TRACE -c logos_psort.c -o /tmp/pt.o && cc -O3 -std=gnu11 -pthread -DLOGOS_BENCH_THREADS=14 -I. /tmp/logos-team/view-analytics/va_probe.c /tmp/pt.o logos_view.o logos_extra.o -o /tmp/va_probe_trace -lpthread -lm && flock /tmp/logos-bench.lock /tmp/va_probe_trace 8000000"
```
Logs of every run quoted above: /tmp/logos-team/view-analytics/logs/ (viewbench1, vb_2, final_f64;
viewbench_i32_1, vi32_2, vi32_3, final_i32; pairprobe). The probes va_probe.c / va_probe2.c live in
/tmp/logos-team/view-analytics/ and are not part of the patch.
