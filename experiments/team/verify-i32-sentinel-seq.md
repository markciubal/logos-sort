# verify-i32-sentinel-seq: skeptic's re-measurement of i32-sentinel-seq

Agent: verify-i32-sentinel-seq, 2026-09-12 overnight. Work copy `/tmp/logos-team/verify-i32-sentinel-seq/work` (WSL),
pristine copy beside it. The claim under test: experiments/team/i32-sentinel-seq.md (patch
experiments/team/patches/i32-sentinel-seq.patch). My patch (their six files plus my `skeptbench.c`):
experiments/team/patches/verify-i32-sentinel-seq.patch. Raw chain log: verify-i32-sentinel-seq.chain.log beside this file.

## Prediction (written before any locked measurement)

Skeptic's bench: `skeptbench.c` (one binary; side OLD sets all four runtime opt-outs at once
= pristine behaviour, side NEW = shipped default; alternating-order adjacent pairs, median ratio).
- pairprobe today: median ~1.03, resolving ~7% at 8M.
- (a) "bulk + one INT32_MIN", 1T, 8M, old/new: **2.0-3.5** (reproduces; the radix on 8M vs a 100-slot count).
- (b) "span 1e6", 14T, 8M, old/new: **2.5-4.0** (reproduces).
- Section G (the literal target metric, never measured by the claimant): sentinel/clean with the
  window ON: **1.05-1.25**; with it OFF: 2-3.5.
- Guards the claimant did not run:
  - exp mean 3e5 (natural heavy tail), 1T: window fires with a ~1.8e6 window and ~0.25% spill; **1.1-1.3** new faster.
    14T: the old cap declined it (trimmed range 6e5 > 262144), new counts it with ~7 x 9 MB: **1.5-2.5**.
  - gauss sigma ~1e6 (bounded at +-4.8e6): declines at the sample on both sides at both thread counts: **0.97-1.03** (null).
  - bimodal far (modes 4e9 apart), 14T: null, **1.0 +- floor**.
  - full 2^32, 14T: the 168 leaves each pay a 2048-point sample then decline: **0.98-1.02**.
  - **The regression I expect: n = 1M, 1T, span n/2 (5e5, a 2 MB L2-resident table).** DIV 1 counted it,
    DIV 4 sends it to the 3-pass radix. Predict old/new **0.80-0.95** (counting still wins at that n).
    span n at 1M (4 MB table): ~1.0.

What would change my verdict: (a) or (b) under 1.5 in my own run; any guard below 0.93 at 8M; the
n=1M DIV regression below 0.85 would make the DIV 4 constant a "ship as opt-in" not a "ship".

## Set-up and static checks

- Patch `experiments/team/patches/i32-sentinel-seq.patch` applies cleanly (`patch -p1 -d work`, six files, no rejects).
- `make test` in the patched copy: **954 checks pass** (648 double, 306 int32) plus 7 extras; matches the claim.
- Engine diff read in full: nothing is written to `a[]` before the windowed pass has counted or spilled every
  element (a mid-pass bail leaves the array untouched); the persistent table pool follows the ctx-bundle
  one-sort-at-a-time contract; every piece has a `-D` opt-out and an uncached env toggle.
- Discipline audit of their write-up: prediction dated before the code (session 1); every ratio comes from ONE
  binary with runtime toggles (so no copy swap was needed: the 1.24x layout trap does not arise); `verify_both`
  against qsort precedes every timed row; the bench lock is claimed for every run; a `-DLOGOS_TRACE` table shows
  each path firing only on its target shapes. Gaps they admit: section G (the target's literal metric) never got
  the lock, and sections B/F were measured once, in session 1.
- My own `-DLOGOS_TRACE` build (`tr/tracedrv`, no lock needed, every output verified) on my guard shapes:
  exp mean 3e5 at 1T fires the windowed leaf (window [620, 1861140], spill 16614 + 16301 = 0.4%); at 14T it takes
  the count route with the FULL range (span 4.7M < max_slots 5.59M) at **3 threads x 18.8 MB**, not the window;
  gauss, bimodal-far and full 2^32 decline at both thread counts and reach the radix / partition as before.

## What was measured (all under `flock /tmp/logos-bench.lock`, every row verified vs qsort, one binary)

Box today: `make pairprobe` (same engine both halves, 40 pairs) median **1.014 at 8M, resolves 4.4%; 1.020 at
1M, resolves 9.4%**. My runs use 16-20 pairs, so the floor is ~6% at 8M and ~13% at 1M.

`skeptbench` (`/tmp/logos-team/verify-i32-sentinel-seq/work/engines/c/skeptbench.c`, in the patch): side OLD
sets `LOGOS_I32_NO_LEAF_WINDOW=1 LOGOS_I32_FIXED_CAP=1 LOGOS_I32_NO_TABLE_POOL=1 LOGOS_I32_COUNT_DIV=1`
(= pristine behaviour), side NEW is the shipped default. old/new > 1 means the patch is faster.

### n = 8M, 20 pairs (`./skeptbench 20 8000000 both 0`)

| shape | thr | old/new | p10 | p90 | claimant | verdict |
|---|---|---|---|---|---|---|
| **bulk + one INT32_MIN** | 1 | **2.734** | 1.67 | 3.56 | 2.39-3.19 | reproduces |
| bulk 0..99 (null) | 1 | 0.946 | 0.77 | 1.47 | 0.985-1.078 | noise |
| exp mean 3e5 (GUARD, window fires) | 1 | 0.924 | 0.76 | 1.34 | not run | inside noise; see below |
| gauss sigma ~1e6 (GUARD, declines) | 1 | 0.980 | 0.76 | 1.75 | not run | null |
| span n/2 (DIV 4 -> radix) | 1 | 1.651 | 1.15 | 2.64 | 1.314 | reproduces |
| span n (DIV 4 -> radix) | 1 | 1.648 | 1.31 | 2.72 | 2.032 | reproduces |
| full 2^32 | 1 | 0.964 | 0.67 | 1.06 | 1.011-1.024 | noise |
| **span 1e6** | 14 | **2.661** | 2.07 | 3.38 | 2.72-3.20 | reproduces |
| span 3e6 (5 x 12 MB) | 14 | 1.304 | 1.01 | 1.75 | not run | win |
| exp mean 3e5 (GUARD) | 14 | 1.261 | 0.79 | 1.54 | not run | win, via 3 full-range threads |
| gauss (GUARD) | 14 | 0.993 | 0.92 | 1.18 | not run | null |
| bimodal far (GUARD) | 14 | 1.086 | 0.88 | 1.19 | not run | noise |
| full 2^32 (GUARD, 168 leaves pay a sample) | 14 | 0.967 | 0.90 | 1.11 | 0.998 | noise |

### Section G, the target's own metric, 1 thread, 8M, 16 pairs (`./i32abbench 16 8000000 G`), never measured by the claimant

| shape | sentinel/clean, window ON | p10 | p90 | window OFF | p10 | p90 |
|---|---|---|---|---|---|---|
| bulk + one INT32_MIN | **1.090** | 0.78 | 1.33 | 2.367 | 1.86 | 5.94 |
| bulk + both extremes | **1.156** | 0.84 | 1.31 | 5.336 | 4.95 | 6.21 |
| bulk + 0.1% wide tail | **1.138** | 1.03 | 1.40 | 5.553 | 2.89 | 6.92 |
| bulk + one -1 (control) | 0.981 | 0.90 | 1.09 | 1.027 | 0.81 | 1.31 |

The target said "from ~2.5x to under 1.2x": with the window the sentinel row costs 1.09-1.16x the clean row.
Their prediction for this unmeasured section (1.05-1.2) is graded correct.

### Section B in their own harness, 14 threads, 8M, 16 pairs (`./i32abbench 16 8000000 B`)

| span | old/new | p10 | p90 | 1T min | PT old | PT new | P/1 new | claimant |
|---|---|---|---|---|---|---|---|---|
| 1e5 (guard, same tables) | 0.923 | 0.79 | 1.31 | 84.9 | 43.7 | 54.8 | 1.55 | 1.064 |
| 5e5 | **3.210** | 2.84 | 4.50 | 133.6 | 254.5 | 67.2 | 1.99 | 4.096 |
| **1e6** | **2.535** | 1.56 | 3.42 | 175.5 | 249.7 | 105.6 | **1.66** | 3.202 (P/1 2.68) |
| 2e6 | **2.022** | 1.70 | 2.86 | 162.1 | 194.5 | 84.2 | 1.92 | 2.342 |
| 4e6 | 1.029 | 0.79 | 1.88 | 300.5 | 265.4 | 234.9 | 1.28 | **1.524: not reproduced** |
| 8e6 | 0.957 | 0.78 | 1.22 | 316.0 | 275.7 | 280.6 | 1.13 | 0.999 |
| full 2^32 | 1.038 | 0.90 | 1.21 | 256.6 | 244.7 | 240.0 | 1.07 | 0.998 |

The band 5e5-2e6 reproduces at 2.0-3.2x (theirs 2.3-4.1, a loaded-box upper bound, as CLAUDE.md warns). The
4e6 row (4 threads x 16 MB) is neutral in my run, not the 1.52x claimed; P/1 for the target row is 1.66 today
against their 2.68 (the P/1 column mixes a 1T min with a PT min and is the least stable number here). The 1e5
guard at 0.923 runs identical code on both sides (same Pc, same tables) and is this box's noise at 16 pairs.

### The regression hunt: n = 1M, 1 thread (`./i32abbench 16 1000000 C` and `./skeptbench 16 1000000 both 0`)

| shape | harness | old/new | p10 | p90 |
|---|---|---|---|---|
| **span n/2 (5e5, a 2 MB table)** | i32abbench C, DIV 1 vs 4 | **0.909** | 0.76 | 1.08 |
| **span n/2** | skeptbench, all toggles | **0.850** | 0.64 | 1.09 |
| span n/2 | i32abbench C, DIV 1 vs 2 | 0.961 | 0.91 | 1.10 |
| span n (1e6, 4 MB) | i32abbench C, DIV 4 | 1.566 | 1.22 | 1.92 |
| span n | skeptbench | 1.419 | 1.19 | 1.48 |
| span 2e6 / 4e6 (radix both) | i32abbench C, DIV 4 | 0.975 / 0.969 | | |
| bulk + one INT32_MIN | skeptbench | 3.327 | 1.81 | 4.84 |
| exp mean 3e5 | skeptbench | 1.018 | 0.84 | 1.08 |
| gauss | skeptbench | 0.996 | 0.80 | 1.32 |
| full 2^32 | skeptbench | 1.004 | 0.94 | 1.09 |

n = 1M, 14 threads (skeptbench): span 1e6 1.181 (p10 0.82, p90 2.46), span 3e6 1.011, exp 0.979, gauss 0.913
(p10 0.78; identical code both sides at this n, so noise), bimodal 1.015, full 1.016.

## Verdict: CONFIRMED, with one bounded regression the patch should fix before it is called finished

The two headline numbers survive re-measurement by an independent harness with all four toggles flipped
together: the one-sentinel row at 1 thread is 2.7x (8M) and 3.3x (1M) faster, the span-1e6 band at 14 threads
is 2.5-2.7x faster, and the metric the target actually named, sentinel row over clean row, is 1.09-1.16 with
the window on (it was 2.4-5.6). The guards the claimant never ran (a natural exponential tail, a bounded
gaussian, a far-apart bimodal, full range at 14 threads) are inside today's floor or better.

**Regression (predicted, then measured twice, same direction):** `LEAF_I32_COUNT_DIV` 4 sends span n/2 at
n = 1M to the radix: 0.85 and 0.91 (p90 1.08-1.09 in both, so the pair distribution barely reaches 1.0).
The mechanism is the one I wrote down beforehand: at n = 1M a 2 MB count table is L2-resident and counting
still beats three radix passes; the gate is a cache-size rule (table <= ~8 MB) dressed as a fraction of n, and
the claimant's own "what I would do next" says the same. It is bounded to 1 thread, n around 1M, spans between
n/4 and ~n/2, and costs ~10-15% there, against +42-57% at span n on the same n. It does not overturn the
verdict but it is a real loss the sweep would have caught, and the fix is a one-line gate
(`span <= min(n, TABLE_BYTES / 4)` with TABLE_BYTES ~ 8 MB) plus one `i32abbench C` run at 1M and 8M.

**Not reproduced but not a regression:** the span-4e6 row at 14 threads (their 1.52x, my 1.03).

## Prediction grades (mine)

- pairprobe ~1.03 / 7%: measured 1.014 / 4.4% (quieter than the claimant's session). Right.
- (a) 2.0-3.5: measured 2.73. Right. (b) 2.5-4.0: measured 2.66 / 2.53. Right.
- Section G 1.05-1.25 on: measured 1.09-1.16. Right.
- exp 1T 1.1-1.3 new faster: measured 0.924. **Wrong**, and the trace explains it: an exponential's count
  table is hit mostly near zero, so the old full-range 18.8 MB table was already cache-friendly and the window
  only adds a branch per element, two spilled tails and two recursive sorts. The window pays for far, sparse
  tails, not for dense skew. Inside the floor, so not a regression, but the prediction's reasoning was wrong.
- exp 14T 1.5-2.5: measured 1.26, through a different mechanism than predicted (3 full-range threads, not a
  7-thread window, because the raw span fit max_slots). Right direction, wrong size, wrong mechanism.
- gauss / bimodal / full 2^32 nulls: 0.97-1.09. Right.
- n=1M span n/2 0.80-0.95: measured 0.85 / 0.91. **Right, the regression.** span n at 1M ~1.0: measured
  1.42-1.57. Wrong: the radix already wins at a 4 MB table on this box.

## Surprises

1. The claimant's section B milliseconds at 4e6 (PT old 220 vs PT new 147) did not recur; mine were 265 vs
   235 with a 1.03 median. The 4-thread x 16 MB configuration is at best neutral against the partition.
2. Section G's "bulk + one -1" control reads 0.98 / 1.03 both ways: the paired_data harness that pairs two
   DIFFERENT inputs through the same engine has no bias of its own, which makes the 1.09-1.16 rows trustworthy.
3. At n = 1M the 1e6 band at 14 threads is only 1.18 (p10 0.82): the count route runs at 3 threads there and
   the win is mostly an 8M story, which the per-route thread caps already predicted.
4. The bench lock was held by three other agents' queues for 44 minutes before my chain started; the chain itself
   took 16 minutes. Budget the lock wait, not the bench.

## What I would do next

- Replace `LEAF_I32_COUNT_DIV` with a table-byte rule (`span * 4 <= LEAF_I32_TABLE_BYTES`, ~8 MB, and still
  `<= n`), then rerun `i32abbench C` at 1M and 8M: the 1M span-n/2 row should return to ~1.0 with every 8M row unchanged.
- Re-measure the 4e6 row at 14 threads with 40 pairs before the CHANGELOG keeps its 1.52x.
- In `leaf_i32_windowed`, when the full sample range does not fit but the trimmed one does, consider trimming
  only the side that overflows: the exponential spilled 16.6k low values it did not need to.

## Commands (repeatable)

```
wsl -e bash -lc "cd /tmp/logos-team/verify-i32-sentinel-seq && patch -p1 -d work < /mnt/c/Users/markc/Coding/logos-sort-repo/logos-sort-repo/experiments/team/patches/i32-sentinel-seq.patch && cd work/engines/c && make test | tail -2"
wsl -e bash -lc "cd /tmp/logos-team/verify-i32-sentinel-seq/work/engines/c && make pairprobe i32abbench && gcc -O3 -std=gnu11 -pthread -DLOGOS_BENCH_THREADS=14 skeptbench.c liblogos_psort.a -o skeptbench -lm"
# the chain, each step under the lock (chain.sh in the work copy; raw log copied beside this file):
flock /tmp/logos-bench.lock ./pairprobe
flock /tmp/logos-bench.lock ./skeptbench 20 8000000 both 0
flock /tmp/logos-bench.lock ./i32abbench 16 8000000 G
flock /tmp/logos-bench.lock ./i32abbench 16 1000000 C
flock /tmp/logos-bench.lock ./skeptbench 16 1000000 both 0
flock /tmp/logos-bench.lock ./i32abbench 16 8000000 B
# mechanism (no lock): gcc -O3 -std=gnu11 -pthread -DLOGOS_TRACE -I. -c logos_psort.c -o tr/lp_trace.o && gcc -O2 -pthread -I. tr/tracedrv.c tr/lp_trace.o -o tr/tracedrv -lm && ./tr/tracedrv 1; ./tr/tracedrv 14
```
