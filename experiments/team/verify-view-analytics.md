# verify-view-analytics: skeptic's re-measurement of the view-analytics claim

Agent: verify-view-analytics (the skeptic). Work copy /tmp/logos-team/verify-view-analytics (WSL);
pristine copy alongside; the teammate's patch applied to `work/` with `patch -p1`. Nothing under the
real repo's engines/ or tools/ was modified. Logs of every run quoted here are in
/tmp/logos-team/verify-view-analytics/logs/ (pairprobe, viewbench_f64, vguard, i32pairA, i32pairB,
viewbench_i32, vsaw, vsaw_avxpar). Every timing ran under `flock /tmp/logos-bench.lock`.

## The claim under test

view-analytics: "Batch quantiles on the f64 view win 1.29-1.67x over sort-then-index and 1.17-1.40x
over ten single lookups (paired, 8M, 14T) because a forced 47k bucket costs 1.4 ms, not 0.3; the int32
view works but is near-neutral (1.02-1.12x)". Verdict claimed: shipped. Touches engine core.

## What was tried

1. Applied the patch to a fresh copy; `make test` (1123 checks, 288 new for the view f64/i32).
2. Read the findings for discipline: prediction-first, paired harness, verification, lock, mechanism.
3. Re-measured the decisive f64 and i32 numbers with the teammate's own `viewbench` / `viewbench_i32`
   (both routes in ONE binary, so route-vs-route has no copy-layout bias), 30 and 20 pairs.
4. Engine-core regression hunt: the patch turns `psort_i32` into a wrapper and adds a per-bucket
   `if (j->lazy)` to `do_buckets_i32`, so I built the PRISTINE engine and the PATCHED engine into one
   binary (`i32pair.c`, the ledgerbench pattern with four extra renames the Makefile's RENAME macro
   lacks: `logos_partition_f64/i32`, `logos_sort_buckets_f64/i32`), in BOTH copy orientations, paired,
   int32 at 14T and 1T on four shapes plus an f64 uniform null row. Effect = sqrt(A/B).
5. Feature-level guard shapes the teammate did not test (`vguard.c`, both routes in one binary):
   near_sorted, few_distinct, quantized (parbench's, 4096 levels), quant_1e5 (100k levels), int_wide,
   sawtooth (`(i % 4096) * 1e3 + fr()`), uniform as the anchor. Each shape's three routes and a full
   materialise are verified against qsort before the pairs.
6. Follow-up on the one guard that failed (sawtooth): `vsaw.c` prints the partition's bucket sizes and
   pairs 10x `logos_view_at` against `logos_view_quantiles`, linked three ways: shipped view
   (parallel force), view built with `-DVIEW_PAR_FORCE=0` (serial force), and shipped view over an
   engine built with `-DLOGOS_AVX2_PARALLEL` (AVX2 leaf allowed inside the parallel bucket sorter).

## Prediction (written before the lock chain ran)

- Decisive f64 ratios reproduce within the day's floor: 1.3-1.6x over sort-then-index, 1.15-1.4x
  over ten single lookups, all four shapes. int32 1.0-1.15x.
- Engine-core pristine-vs-patched: within the floor everywhere (the eager paths are barely touched).
- Guards: near_sorted / few_distinct / quantized go to structured routes (0 buckets) and read ~1.0;
  int_wide behaves like uniform. I had no prediction for sawtooth beyond "probably like int_wide".

Graded at the end: first three right; the sawtooth guess was wrong, and that is the finding.

## Discipline check on the teammate's work

- Patch applies cleanly (10 files, no fuzz); `make test` passes, 1123 checks, after a clean rebuild.
- Prediction-first: the findings file says "reproduced verbatim". Unverifiable from timestamps: the
  first bench logs are 04:04, the findings file's mtime is 04:44 (rewritten after the runs). Taken on
  trust; the predictions are specific and two were graded wrong, which is the usual signature of an
  honest one.
- Paired harness: yes, `bench_paired`, 20-30 pairs, three or four runs. Copy swap: not needed for the
  headline, which compares two API routes inside one binary (a runtime choice, no layout term). The
  engine-core edits were NOT A/B'd pristine-vs-patched by the teammate; done here (step 4).
- Verification: every row in viewbench checks against qsort; the paired loop verifies whichever route
  ran last (`G_out`), which with an even pair count is the sort route, so the batch's output inside
  the paired loop is covered only by the min-of-7 row on the same data. Adequate.
- Bench lock: their commands show `flock`; cannot be verified from logs. My runs all used it.
- Mechanism: the TRACE line and the materialised count show the paths fire. Their per-bucket cost
  probe (va_probe) is not in the patch; not re-run.
- `pairprobe` on my run: 8M resolves 4.7% (median 1.014, p10 0.760, p90 1.180, 40 pairs); 1M 15.6%.
  Noisier than the teammate's 2.8% day. All 8M ratios below are judged against ~5-7%.

## What was measured

All 8M elements, 14 threads, i9-13900H under WSL2, every run under the lock, every output verified.

### Decisive f64 (viewbench 7 8000000 30; median of 30 pairs, >1 = view route faster)

| shape | claim: sort+index / view+quantiles | mine | claim: 10x view_at / batch | mine | eager / partition-only (mine) |
|---|---|---|---|---|---|
| uniform | 1.45-1.67 | **1.429** [p10 0.99, p90 2.07] | 1.25-1.40 | **1.555** [0.88, 2.57] | 1.822 (leaf share 45%) |
| heavy_tail | 1.29-1.53 | **1.627** [1.03, 1.94] | 1.19-1.35 | **1.408** [1.01, 1.80] | 1.702 (41%) |
| bimodal | 1.56-1.63 | **1.402** [1.10, 1.88] | 1.28-1.35 | **1.310** [0.90, 2.04] | 1.686 (41%) |
| clustered | 1.32-1.37 | **1.221** [1.07, 1.57] | 1.17-1.26 | **1.194** [0.94, 1.64] | 1.426 (30%) |

Reproduces. My range 1.22-1.63 against their 1.29-1.67; clustered lands a little under their band
but its p10 is 1.07 and the floor is ~5%. The batch-over-naive medians 1.19-1.56 all clear the floor;
single pairs do not (p10 below 1 on three shapes), which is why 30 pairs are needed.

### Decisive int32 (viewbench_i32 5 8000000 20)

| shape | buckets | sort+index / view+quantiles | 10x at / batch | eager / partition-only (leaf share) |
|---|---|---|---|---|
| span_1e8 | 168 | 1.154 [0.98, 1.30] | 1.049 | 1.094 (9%) |
| full_range | 168 | 1.135 [0.89, 1.27] | 1.091 | 1.217 (18%) |
| span_2e5 | 1 | 1.031 [0.68, 1.37] | 1.113 | 1.023 (2%) |
| clustered | 168 | 1.057 [0.98, 1.23] | 1.009 | 0.997 (0%) |

"Near-neutral, 1.02-1.12x" confirmed (1.03-1.15 here, 20 pairs, floor ~7%). The leaf share of an
int32 sort is 0-18%: the teammate's explanation (int32 partition is 85-93% of the sort) holds.

### Engine core: pristine vs patched, copy-swapped (i32pair, 20 pairs each orientation)

A = t(pristine)/t(patched) with pristine in lg0; B = t(patched)/t(pristine) with the copies swapped;
effect = sqrt(A/B) (>1 = patched faster); layout term sqrt(A*B).

| row | A | B | effect sqrt(A/B) | layout sqrt(A*B) |
|---|---|---|---|---|
| i32_full 14T | 0.996 | 1.065 | 0.967 | 1.030 |
| i32_full 1T | 1.069 | 1.045 | 1.011 | 1.057 |
| i32_1e8 14T | 1.086 | 1.011 | 1.036 | 1.048 |
| i32_1e8 1T | 1.014 | 1.037 | 0.988 | 1.025 |
| i32_2e5 14T (counting route) | 1.125 | 1.005 | 1.058 | 1.063 |
| i32_2e5 1T | 1.050 | 1.038 | 1.006 | 1.044 |
| i32_clus 14T | 1.051 | 1.032 | 1.009 | 1.041 |
| i32_clus 1T | 0.989 | 1.017 | 0.986 | 1.003 |
| f64_uniform 14T (null: f64 eager path untouched) | 1.045 | 1.000 | 1.022 | 1.022 |

Every effect is within 0.97-1.06 with 20 pairs per orientation (floor ~6.6% at that count). No
engine-core regression resolved; the wrapper and the lazy flag cost nothing measurable.

### Guard shapes (vguard 24 8000000)

| shape | buckets / touched | sort+index / view+quantiles | 10x at / batch |
|---|---|---|---|
| near_sorted | 168 / 10 | 1.271 [1.00, 1.56] | 1.120 [0.99, 1.46] |
| few_distinct | 1 / 1 (route sorted outright) | 1.082 [0.72, 1.38] | 0.986 [0.76, 1.38] |
| quantized (4096 levels) | 1 / 1 (lattice sorted outright) | 1.042 [0.83, 1.26] | 1.008 [0.75, 1.40] |
| quant_1e5 | 168 / 10 | 1.302 [1.01, 1.65] | 1.334 [0.93, 1.75] |
| int_wide | 168 / 10 | 1.728 [1.43, 1.95] | 1.254 [0.99, 1.46] |
| **sawtooth** | 168 / 10 | 5.489 [4.27, 6.33] (eager sort takes a slow route here; pre-existing) | **0.724 [0.48, 1.09]** |
| uniform | 168 / 10 | 1.509 [1.21, 1.76] | 1.780 [1.27, 2.39] |

Six of seven are at or above 1. Sawtooth: the batch API is SLOWER than ten single lookups on the
same view, outside the floor in the wrong direction.

### Sawtooth follow-up (vsaw, 8M, 14T; same data, same binary layout for the view code)

Bucket sizes are balanced (min 46,872, max 48,850, mean 47,619, none empty), so it is not imbalance.

| build | 10x view_at / quantiles(10) | pairs |
|---|---|---|
| shipped (parallel force, scalar parallel leaf) | **0.674** [0.52, 0.94] | 40 |
| shipped, second session | **0.624** [0.48, 1.14] | 30 |
| view built `-DVIEW_PAR_FORCE=0` (serial force: identical work both routes) | 0.974 [0.79, 1.37] | 40 |
| engine built `-DLOGOS_AVX2_PARALLEL` (AVX2 leaf inside the parallel bucket sorter) | **1.209** [0.99, 1.69] | 30 |
| uniform, shipped | 1.392 [1.11, 2.01] | 40 |
| uniform, serial force | 1.066 [0.76, 1.33] | 40 |
| uniform, AVX2-parallel engine | 1.416 [1.06, 1.88] | 30 |

## Verdict

**Confirmed, with one regression the integrator must know about.**

- The headline reproduces: f64 batch quantiles 1.22-1.63x over sort-then-index and 1.19-1.56x over
  ten single lookups on the four shapes, 30 pairs, all clear of a 4.7% floor; int32 near-neutral
  (1.03-1.15x) confirmed; the engine-core edits are neutral pristine-vs-patched with the copy swap.
  The parallel force is the mechanism (serial-force build reads 1.07 on uniform, parallel 1.39).
- **Regression on a guard shape: sawtooth.** The batch is 1.4-1.6x SLOWER than ten `logos_view_at`
  calls (0.62-0.72, two sessions, p90 below 1 in one). Cause, confirmed by the two control builds:
  `logos_sort_buckets_f64` sets `seq = (P == 1)`, so a single lookup (P=1) sorts its bucket with the
  AVX2 sequential leaf while the batch (P=10) sends the same buckets through the scalar parallel
  leaf; on sawtooth-shaped buckets (24 integer levels with fractional jitter, period-24 order after
  the scatter) the scalar leaf loses more than 10-way parallelism recovers. With the AVX2 leaf allowed
  in the parallel sorter the same batch is 1.21x faster than the naive loop, and uniform is unchanged
  (1.42 vs 1.39). The batch's gain is therefore "parallel force" on shapes where the two leaves are
  close and "parallel force minus a leaf downgrade" everywhere else.

## Why, mechanism

The view's buckets are independent 47k-element sorts, each L2-resident. Inside the eager engine the
ledger says the AVX2 leaf loses under 14 threads on random shapes (-5 to -28%), which is why it ships
sequential-only; but that verdict was for the 14T full sort where all 168 leaves run at once. In the
batch, 10 threads each sort one bucket and on uniform the scalar and AVX2 leaves tie (1.39 vs 1.42),
while on sawtooth the AVX2 quicksort is far ahead. `seq` is a proxy for "how many leaves are running",
and the view's bucket sorter uses it as a proxy for "which leaf is better", which is a different
question.

## What I would do next

1. In `logos_sort_buckets_f64` (and the i32 twin), pass `seq = 1` for the view's forced set, or
   expose it as a parameter, and re-run vguard + viewbench: predicted sawtooth 0.65 -> ~1.2, uniform
   unchanged, the other shapes within noise. This is a one-line change; do not ship the batch route
   without measuring it. (Building the whole engine with `-DLOGOS_AVX2_PARALLEL` is NOT the fix: the
   ledger says that costs the eager 14T sort 5-28%.)
2. Add sawtooth (and quant_1e5) to viewbench's shape list so the guard stays.
3. The eager `logos_sort_f64` is 5.5x slower than partition + 10 forced buckets on this sawtooth: some
   dispatcher route (run-merge, from 1953 ascending runs of 4096) is being chosen over the partition.
   Pre-existing, not this patch, but it is a free 5x on that shape for whoever picks it up.
4. Add `logos_partition_f64/i32` and `logos_sort_buckets_f64/i32` to the Makefile's RENAME macro:
   without them `make ledgerbench` cannot link two copies of the current engine.

## Anything surprising

- The teammate's box resolved 2.8%; mine 4.7% at 8M, with single-pair p10/p90 of 0.76/1.18. Their
  numbers reproduced anyway because the effects are 1.2x and up.
- The regression hides behind the shapes everyone uses: uniform, heavy_tail, bimodal, clustered all
  have buckets where the scalar and AVX2 leaves are within ~10% of each other. It took a shape with
  structure INSIDE a bucket to expose the leaf switch. Structure at the top level (few_distinct,
  quantized) goes to the counting/lattice routes and never reaches the view at all.
- Sawtooth also shows the eager sort at 5.5x the view route, the opposite sign of everything else.

## Commands (decisive measurements, all under the lock)

```
# setup + patch + tests
wsl -e bash -lc "cd /tmp/logos-team/verify-view-analytics && patch -p1 -d work < .../experiments/team/patches/view-analytics.patch && cd work/engines/c && make -s clean; make test 2>&1 | tail -3"
# floor of the day, then the teammate's benches re-run
wsl -e bash -lc "cd /tmp/logos-team/verify-view-analytics/work/engines/c && make -s pairprobe viewbench viewbench_i32 && flock /tmp/logos-bench.lock sh -c './pairprobe; ./viewbench 7 8000000 30; ./viewbench_i32 5 8000000 20'"
# guards (both routes, one binary) and the pristine-vs-patched copy-swapped int32 pair
#   build: scratchpad/build.sh (compiles pristine and work logos_psort.c with lg0/lg1 renames in both orientations)
wsl -e bash -lc "cd /tmp/logos-team/verify-view-analytics/work/engines/c && flock /tmp/logos-bench.lock sh -c './vguard 24 8000000; ./i32pairA 20 8000000; ./i32pairB 20 8000000'"
# sawtooth mechanism: parallel force vs serial force vs AVX2-parallel engine
wsl -e bash -lc "cd /tmp/logos-team/verify-view-analytics/work/engines/c && cc -O3 -std=gnu11 -pthread -fPIC -DVIEW_PAR_FORCE=0 -c logos_view.c -o logos_view_serial.o && cc -O3 -std=gnu11 -pthread -fPIC -DLOGOS_AVX2_PARALLEL -c logos_psort.c -o psort_avxpar.o && cc -O3 -std=gnu11 -pthread -DLOGOS_BENCH_THREADS=14 vsaw.c liblogos_psort.a -o vsaw_par -lpthread -lm && cc -O3 -std=gnu11 -pthread -DLOGOS_BENCH_THREADS=14 vsaw.c logos_view_serial.o logos_psort.o logos_extra.o -o vsaw_ser -lpthread -lm && cc -O3 -std=gnu11 -pthread -DLOGOS_BENCH_THREADS=14 vsaw.c psort_avxpar.o logos_view.o logos_extra.o -o vsaw_avxpar -lpthread -lm && flock /tmp/logos-bench.lock sh -c './vsaw_par 40 8000000 saw; ./vsaw_ser 40 8000000 saw; ./vsaw_avxpar 30 8000000 saw; ./vsaw_par 30 8000000 uniform'"
```
Sources of vguard.c, i32pair.c, vsaw.c and build.sh: /tmp/logos-team/verify-view-analytics/work/engines/c/
(copies in this session's scratchpad). They are verification tools, not part of any patch.
