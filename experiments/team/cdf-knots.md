# cdf-knots — knots placed by curvature (Douglas-Peucker over the sample CDF), for heavy_tail at 14T

Agent: cdf-knots, overnight 2026-09-12. Target: FINDINGS 1h's one fair-fight loss, heavy_tail 0.94x
against x86-simd-sort at 14 threads. Idea: OUTSIDE_THE_BOX.md B1. Work copy
`/tmp/logos-team/cdf-knots/work`; patch `experiments/team/patches/cdf-knots.patch` (apply with
`patch -p1` from the repo root; everything is opt-in behind `-DLOGOS_CDF_KNOTS`, no version bump).

## What was tried

A new certified bucket map, **mode 10**, for the histogram-free partition (`cd_build`, `cd_fit`,
`cd_lookup` in `logos_psort.c`, right before `do_hist`):

1. **Douglas-Peucker over the sorted sample's rank-vs-KEY curve.** Points are the distinct
   order-preserving keys of the 5376-point partition sample against rank in bucket units; chord
   between the ends, split at the largest vertical deviation while it exceeds `CD_TOL` buckets
   (default 0.5, halved and refitted up to 4 times while the sample imbalance exceeds `CD_BAL_MAX`
   = 1.5x). Key space, not value space: a Pareto is exponential there and a hockey stick in value
   space, and the direct index has to be equi-width in something.
2. **Two lookups.** The first (opt-in `-DCD_SEGMENT_LOOKUP`) indexes a 4096-cell table by key, the
   cell names the segment containing its start plus the key of the first knot inside it (branch-free
   +1), the segment is a multiply-add: two dependent loads. The second, the default, **rasterises the
   fitted function into 4096 flat 16-byte cells** (y0 as float, slope, and the bucket range the fit
   spans there as int16 clamps): one shift, one load, one multiply-add, two clamps — the exponent
   map's shape at 16x its resolution, with the DP deciding what each cell holds. The clamps make
   the map monotone whatever the arithmetic rounds to.
3. **Gated like every other map**: built only when the three certificates (uniform, log-uniform,
   per-mode) declined; vetted by `BAL_OK`; timed by `hf_trial` against the splitter tables and
   picked only if faster. Runtime toggles `LOGOS_KNOTS=0` and `LOGOS_KNOT_TOL=x` so one binary can
   A/B both sides with zero code-layout bias (`make knotbench`, the LOGOS_LPT pattern).
4. **Harness**: four new f64 shapes (`pareto_signed`, `lognormal`, `tail_dups`, `tail_sentinels`)
   covering a segment across the sign boundary of key space, a curve bent both ways, sample runs on
   duplicates, and a bulk that falls into one index cell. 792 double checks (plus 180 int32, 7
   extras) pass both with and without the flag; the new shapes run through the shipped engine when
   the map is compiled out.
5. **A trace bug fixed on the way**: `par: leaf sizes max ...` printed `lsz[h.order[0]]`, which is
   bucket 0 unless `LOGOS_LPT` is on. It now prints the real maximum. Every "imbalance 1.09x" the
   shipped trace ever printed at 14T was bucket 0's size; the true figure for the exact tables is
   1.42-1.49x (below).

## Prediction (written before any A/B was run)

**The premise of B1 is wrong on the mechanism, and the trace already says so.** Before writing any
map I traced the shipped engine (-DLOGOS_TRACE, 8M, 14 threads, under the bench lock). heavy_tail
takes the KEY splitter table (`hf: trial mode 2 style 1`). The splitter tables are EXACT quantile
lookups, so their balance is the sample's balance (Beta(32, m-31), 18% sd per bucket); no map built
from the same sample can be better balanced than that. A curvature map can only win on the LOOKUP:
the key table walks a root cell, a child node, a child cell and a splitter compare on a Pareto head,
where a knot map needs a cell load and one multiply-add. Its balance will be WORSE than the table's,
and the question is whether it stays under the spill slack (1.6x) while being cheaper per element.

| quantity | prediction | measured |
|---|---|---|
| knots on heavy_tail at tolerance 0.5 | 30-80 | 26 at 0.5 (then refit: 78 at 0.25, 249 at 0.125) — **right** |
| sample imbalance of the knot map on heavy_tail | 1.15-1.30x, no spills | in-sample 1.28x at 0.25; **true leaf max 1.68x**, 5215 spills (0.07%) — **balance worse than predicted** |
| hf_trial picks it on heavy_tail / pareto / lognormal / exponential | yes | segment lookup: **never**; flat lookup: heavy_tail, pareto_2.5, exponential yes, lognormal no — **half right** |
| heavy_tail 14T, paired | 1.04-1.10x | **1.035x** (7T 1.094x), inside a ±5% floor — **at the low edge, unresolved** |
| pareto_2.5, lognormal, exponential 14T | 1.02-1.08x | 1.042 / 1.029 / 1.032 — **consistent, unresolved** |
| guards (uniform, int_wide, bimodal, quantized) | 1.00 within noise | 0.972 / 1.011 / 0.980 / 1.009 — **right, and they define tonight's floor** |
| clustered | within noise, maybe picked | rejected by BAL_OK at 12.2x (clusters narrower than a cell); 0.951x, inside the floor — **right on the ratio, wrong on the reason** |
| 1 thread | identical | 0.971 (p10 0.51, p90 2.09: the box) — **identical by construction** |
| fair fight heavy_tail | 0.94x -> ~1.00x | **discarded**: calibration memcpy 18-31 ms, std::sort-1M 316-548 ms (quiet: 2.9 / 65) |

## What was measured

Everything under `flock /tmp/logos-bench.lock`, 8M f64, 14 physical cores (BENCH_THREADS), six other
agents on the box all night. Absolute milliseconds are meaningless (rule 2); only ratios within a
pair or a row are reported.

### Mechanism (`make knotprobe`, one traced sort per shape, 14T)

| shape | map built | trial picked | true leaf max / mean | spills |
|---|---|---|---|---|
| heavy_tail | 78 knots @0.25, in-sample 1.28x | **mode 10 (flat)** | **1.68x** | 5215 |
| heavy_tail, `LOGOS_KNOT_TOL=0.125` | 249 knots, in-sample 1.16x | mode 10 | **1.69x** | 4183 |
| pareto_2.5 | 79 knots @0.25, 1.28x | mode 10 | 1.83x | 10965 |
| pareto_2.5, tol 0.125 | 262 knots, 1.22x | mode 10 | 1.71x | 5050 |
| exponential | 31 knots @0.5, 1.47x | mode 10 | 1.61x | 497 |
| lognormal | 36 knots @0.5, 1.41x | mode 2 (key table) | 1.43x | 0 |
| clustered | 43..733 knots, 12.16x every round | rejected (BAL_OK) -> mode 2 | 1.42x | 0 |
| bimodal | not built (per-mode certificate) | mode 9 | 1.49x | 0 |
| uniform, int_wide | not built (KS certificate) | mode 6 | 1.01x | 0 |

With the segment lookup (first cut, same knots) the trial picked mode 2 on every target shape.

**The balance reading is the finding.** Tightening the tolerance from 0.25 to 0.125 buckets cut the
in-sample imbalance from 1.28x to 1.16x and moved the true imbalance from 1.68x to 1.69x. The true
figure is the sample's own noise (which gives the exact tables their 1.42-1.49x) plus the fit's
in-sample deviation, and the second term cannot go below zero, which is where the exact splitters
already sit. A map fitted to the sample cannot be better balanced than the sample.

### End-to-end, paired, one binary (`make knotbench && flock ... ./knotbench 40 8000000`)

ratio = t(map off) / t(map on), median of 40 adjacent pairs, same machine code both halves.

| shape | 14T | p10..p90 | 7T | p10..p90 | map in play? |
|---|---|---|---|---|---|
| heavy_tail | **1.035** | 0.87..1.39 | **1.094** | 0.89..1.32 | yes |
| pareto_2.5 | 1.042 | 0.89..1.22 | 1.027 | 0.90..1.35 | yes |
| exponential | 1.032 | 0.89..1.17 | 0.969 | 0.77..1.29 | yes |
| lognormal | 1.029 | 0.88..1.20 | 1.017 | 0.84..1.27 | built, not picked |
| gaussian | 1.014 | 0.81..1.20 | 1.044 | 0.84..1.23 | (not traced) |
| clustered | 0.951 | 0.83..1.08 | 1.017 | 0.79..1.37 | rejected |
| uniform | 0.972 | 0.85..1.12 | 0.989 | 0.87..1.20 | never built |
| bimodal | 0.980 | 0.90..1.21 | 0.948 | 0.77..1.16 | never built |
| quantized | 1.009 | 0.84..1.39 | 0.991 | 0.79..1.35 | never built (lattice route) |
| int_wide | 1.011 | 0.88..1.27 | 1.019 | 0.85..1.38 | never built |
| heavy_tail 1T | 0.971 | 0.51..2.09 | | | never partitions |

The four never-built rows span 0.948-1.019 at 14T and 7T: that is tonight's floor at 40 pairs, about
±5% (the quiet-box figure is ±3.7%; the p10/p90 spread of 25-50% per pair says how loaded the box
was). heavy_tail, pareto_2.5 and exponential at 14T all sit at +3.2-4.2%, the right sign three times
out of three and the same size as lognormal's +2.9% where the map was NOT picked. **Not resolved.**

A follow-up at `LOGOS_KNOT_TOL=0.125` (30 pairs, heavy_tail and pareto_2.5 at 14T) was queued; see
the addendum at the end if it landed before the night ended.

### Fair fight (`make parbench-simd XSS=<real repo clone>`, LOGOS_KNOTS=0/1/1/0, best of 5)

Calibration lines: memcpy-64MB 18.4 / 30.4 / 31.2 / 27.0 ms, std::sort-1M 316 / 548 / 386 / 325 ms.
CLAUDE.md rule 2: memcpy above 10 ms is a genuine external hog, discard. For the record the
heavy_tail 14T row read logos/x86simd 0.51x, 1.23x, 1.10x, 0.53x in that order, which is the box,
not the map (the paired harness above puts the map's whole effect at about 3%).

## Verdict

**neutral** — kept opt-in behind `-DLOGOS_CDF_KNOTS`, not shipped as the default (rule 8). The map
is correct (792 checks), it is built and chosen where intended, its lookup is measurably cheaper
than the key splitter table (the trial says so on three shapes), and the end-to-end gain that buys
is about 3-4% on heavy_tail at 14 threads, which tonight's box cannot separate from zero and which
is below the 1.05x the task asked me to beat. The reason is measured, not guessed: the cheaper
lookup is paid for by a worse partition (true leaf max 1.68x vs 1.45x), and no tolerance fixes
that because the exact splitters already have zero in-sample error.

## Why, mechanism

- **B1's premise inverted.** B1 says the DP map "optimises the quantity that actually matters:
  maximum bucket imbalance". It cannot: the shipped tables are exact quantile lookups on the same
  sample, so their imbalance IS the sample's, and any smooth fit adds its residual on top. The only
  thing left to win is the lookup, and the project has already recorded twice that a cheaper
  lookup with a worse partition loses (exponent map: 0.80-0.91x). This map keeps the partition close
  enough (1.68x, 0.07% spills, under the 1.6x slack most of the time) that it does not lose, and
  gains the few per cent the lookup is worth. On heavy_tail the lookup is simply not where the
  time is any more: after H0b and the histogram-free partition, the partition's model costs a few
  per cent of the sort, and the residual heavy_tail deficit against x86-simd-sort is elsewhere
  (leaf or memory traffic, see "next").
- **Why the segment lookup lost the trial**: two dependent loads plus an int64-to-double convert and
  a multiply-add is about what the key table pays on a Pareto head (root cell, child, compare), and
  the table's loads hit L1 (32 KB root) while the segment table's first load is a 64 KB cell array.
- **Why clustered is rejected**: 16 clusters each 1e5 wide over 1.6e9 fall inside single key-space
  cells; a flat cell holds one line, so a cluster narrower than a cell maps to one bucket. The
  segment lookup handles it (43 knots, 15 multi-knot cells) but is slower everywhere else. BAL_OK
  did its job at 12.2x; this is the guard the exponent map taught the project to add.

## What I would do next

1. **Stop attacking heavy_tail's partition model.** Profile the heavy_tail-vs-uniform gap by phase
   (`make profile`) now that the trace prints the true leaf maximum: if the leaves account for the
   gap, the lever is LPT ordering (`LOGOS_LPT`, measured neutral at 1.45x imbalance, may not be at
   1.68x) or a cheaper leaf for the many tiny-range head buckets; if the scatter does, it is the
   spill/WC-buffer path, not the lookup.
2. **Certify the knot map from the sample's second half.** Fit on 2688 points, measure imbalance on
   the other 2688: an honest out-of-sample number instead of the optimistic in-sample 1.16-1.28x,
   which would let `CD_BAL_MAX` be set where it means something.
3. **Use the fit for what it is good at: sizing the regions.** The histogram-free partition gives
   every bucket 1.6x its mean share of slack. A smooth fit's predicted per-bucket mass has LOWER
   variance than the raw quantiles (regression to the curve), so region sizes from the fit could cut
   the slack and the spill rate for the exact tables — the map as a sizing oracle, not a lookup.
4. If someone wants the lookup anyway: 2048 cells (32 KB, L1-resident) and `tol` 0.125 is the
   combination to try, with 160 pairs on a quiet box, since 3-4% is exactly what that resolves.

## Anything surprising

- The shipped trace's "imbalance" line was printing bucket 0 for every run without LOGOS_LPT. The
  1.09x that opened this night's notes was never a balance figure. Fixed in the patch.
- Tolerance did nothing to the true balance (1.68x to 1.69x at 4x the knots). I expected the
  (1 + 2*tol) bound to show up in the leaf sizes; it shows up in-sample only.
- The knot counts are almost the same on pareto_1.2 (78) and pareto_2.5 (79): in key space the two
  differ by a scale factor on the exponent, and Douglas-Peucker with a bucket-unit tolerance is
  invariant to that. The lognormal needs fewer knots (36 at 0.5) than the Paretos need at the
  same tolerance (26/27 at 0.5, but those failed the 1.5x sample check and were refit at 0.25).
- flock is not FIFO: the fair fight and a tolerance sweep both jumped the queue ahead of the main
  A/B. Harmless, but the wall-clock ordering in the task logs is not the submission order.

## Addendum: the tolerance sweep landed

Mechanism at `LOGOS_KNOT_TOL=0.0625` (14T): heavy_tail 704 knots, in-sample 1.16x, **true 1.58x,
0 spills**, still picked; pareto_2.5 711 knots, true 1.59x, 0 spills, but the trial now picked mode
5 (162-197 of 4096 cells hold 2+ knots at this tolerance, so the fit's own resolution is exhausted
by the cell grid); exponential 722 knots, true 1.62x. So the ladder of true leaf maxima is
1.68 / 1.69 / 1.58 at tolerance 0.25 / 0.125 / 0.0625 against 1.45 for the exact table: a
tighter fit buys a little balance at the end, never the table's.

Paired A/B at `LOGOS_KNOT_TOL=0.125`, 30 pairs, 14T: heavy_tail **1.041** (p10 0.935, p90 1.365) —
the same +3-4% as the default tolerance's 1.035; pareto_2.5 **1.049** (p10 0.906, p90 1.374)
against 1.042 at the default. Five target-row readings at 14T now sit between 1.032 and 1.049 with
the guards spanning 0.95-1.02: a real but small effect, at the floor. Verdict unchanged.

## Commands (decisive measurement)

```
# work copy set up as in the team brief, then:
python3 patch_knots.py && python3 patch_knots2.py       # (scratchpad scripts; the patch file is the result)
cd /tmp/logos-team/cdf-knots/work/engines/c
make test                                               # 648 checks, map compiled out
make test CFLAGS='-O3 -std=gnu11 -pthread -Wall -Wextra -fPIC -DLOGOS_CDF_KNOTS'   # 792 checks
make knotprobe && flock /tmp/logos-bench.lock ./knotprobe all 14                   # mechanism
make knotbench && flock /tmp/logos-bench.lock ./knotbench 40 8000000               # paired A/B
LOGOS_KNOT_TOL=0.125 flock /tmp/logos-bench.lock ./knotprobe heavy_tail 14         # tolerance sweep
make parbench-simd XSS=/mnt/c/.../engines/x86-simd-sort/src && LOGOS_KNOTS=1 flock /tmp/logos-bench.lock ./parbench
```
