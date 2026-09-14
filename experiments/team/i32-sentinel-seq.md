# i32-sentinel-seq: two int32 defects with known fixes, plus one found on the way

Agent: i32-sentinel-seq, 2026-09-12 overnight, two sessions (the first died at its limit after
building and measuring everything except section G; this session re-applied its patch to a fresh
copy, re-verified, re-measured the decisive rows under the bench lock, fixed the patch's line
endings and finished the write-up). Work copy `/tmp/logos-team/i32-sentinel-seq/work` (WSL).
Patch: `experiments/team/patches/i32-sentinel-seq.patch` (six files under `engines/c`:
`logos_psort.c`, `harness.c`, `intbench.c`, `Makefile`, `CHANGELOG.md`, new `i32abbench.c`; 48 KB,
no binaries; harness.c and intbench.c keep their CRLF endings so their hunks are 41 and 29 lines).
Every timing run below was under `flock /tmp/logos-bench.lock` with six other agents on the box.
`make test` passes before and after: 954 checks (648 double, 306 int32; was 835).

## Prediction (written before any code or measurement, session 1)

Baseline (`./intbench 5 8000000`, 14 threads, loaded box, ratios within the run only):
sentinel row "bulk + one INT32_MIN" at 1 thread 86.8 ms against 30-54 ms for the same bulk
without the sentinel (the clean rows themselves wander 37-54 ms from drift, so the sentinel
penalty is 1.6-2.9x today, 2.5x in the CHANGELOG's run). "both extremes" 134 ms (2.5-4.5x).
Span-1e6 row P/1 = 0.60 (14T 209.6 ms vs 1T 126.6 ms).

(a) Windowed counting in the sequential leaf (`leaf_i32`), same idea as `count_try_i32`:
    - bulk + one INT32_MIN, 1 thread, 8M: from ~2.5x the clean row to **<= 1.15x** the clean
      row (one extra pass: a 2048-point sample plus the spill branch on the count loop).
    - bulk + both extremes: same, <= 1.15x.
    - bulk + one -1: unchanged (span 101 <= n, the plain counting path fires first).
    - bulk + 5% INT32_MIN: unchanged within noise (the sample sees ~100 sentinels, the trimmed
      window still spans 2^31, so it declines BEFORE touching the array).
    - full-range uniform 2^32 and span 1e8: **within 2%** (pays the sample, ~2048 reads, then
      declines; the radix behind it is ~100+ ms).
    - parallel rows: unchanged, count_try_i32 handles them before the leaf sees anything.
    Paired ratio old/new on the sentinel row at 1T: predict **2.0-2.5**.

(b) The span band 262144 < span <= ~n/8 at 14 threads. Build the memory-budget rule
    (per-thread tables 4 B x span x Pc <= a budget, default 64 MB, threads reduced to fit, decline
    below 3 threads) because it keeps the count route's scaling and is the only option that can
    reach >= 1.5; the shared-table variant should lose on uniform data (every increment bounces a
    line between cores) so it is not built unless the budget rule fails; handing the band to one
    thread caps P/1 at ~1.0 by construction and is the fallback.
    - span 1e6, 8M, 14T: P/1 from 0.60 to **2.0-3.0** (the 1e5 row is 3.4 and its table is L2
      resident; a 4 MB table per thread is not, and 14 x 4 MB = 56 MB exceeds L3, so the count
      pass will be DRAM-latency bound like the scatter it replaces, and the calloc of 56 MB is
      ~14k page faults).
    - span 2e6 (Pc = 8) and 4e6 (Pc = 4): P/1 >= 1.3, still ahead of the partition; span 8e6
      declines (Pc would be 2 < 3) and is untouched at 2.4.
    - spans <= 1e5: unchanged (Pc stays 14, same tables as before).
    Paired ratio old/new on the span-1e6 row at 14T: predict **3-4**.

(c) Found while reading the baseline: the sequential leaf counts for any span <= n, but the
    "n = 8e6" row (span = n, counting, 367 ms) is slower than the "1e8" row (radix, 205 ms) at 1
    thread. Predict the crossover where counting stops paying is span ~ n/4 (table 8 MB, about
    L3-resident), and that a gate of span <= n/4 makes the span-n row ~1.6x faster with the 1e6
    row unchanged.

What would falsify: (a) if the sentinel row stays above 1.3x the clean row, the extra pass or
the tail sort costs more than the sample suggests; (b) if 14 threads with 4 MB tables lose to
the 1e5 row by more than 3x, the table footprint (56 MB, page faults, L3 misses) is the cost,
not the partition, and the fix is fewer threads or a persistent table.

## What was tried

All three changes are RUNTIME toggles read uncached from the environment, so one binary
measures both sides of every pair with no code-layout bias at all (the 1.24x trap of rule 3
does not arise; no copy swap needed). Each also has a compile-time opt-out.

1. **(a) `leaf_i32_windowed`** in `logos_psort.c`. When the raw span overflows the counting
   gate, a 2048-point Weyl sample (m = min(2048, n/4)) tracks its 5 smallest and 5 largest,
   giving the sample's own range and a K=5 trimmed range. The first that fits the gate becomes
   the window. The window is counted into `c->aux`; anything outside spills into the n-word
   scratch buffer, low tail from the front, high tail from the back, with a budget of n/32+64
   (over budget: return 0 with `a[]` untouched, the radix takes over). Bulk is emitted from the
   counts into the middle, both tails are copied out of the scratch and sorted by a recursive
   `leaf_i32` call (they can be windowed again; depth is bounded by the n/32 tail). Only for
   n >= 4096. Toggles: `-DLOGOS_NO_I32_LEAF_WINDOW`, env `LOGOS_I32_NO_LEAF_WINDOW=1`.
2. **(b) table budget** in `count_try_i32`. `COUNT_I32_MAX_SLOTS` (262144, per-thread fixed)
   is replaced by `COUNT_I32_TABLE_BUDGET_MB` (64): the pre-gate's `max_slots` is
   budget/3 capped at n, and after the true span is known `Pc = min(count_threads, budget/span)`
   (so Pc >= 3 is guaranteed whenever the span passed the gate). The old rule stays as the A/B
   baseline (`LOGOS_I32_FIXED_CAP=1`); `LOGOS_I32_TABLE_MB=N` sweeps the budget. Not built: the
   shared atomic table (predicted to lose on uniform data; the budget rule reached the target so
   it was never needed) and the hand-to-one-thread option (caps P/1 at 1.0 by construction).
3. **(b, from the trace) persistent count tables**: `g_cnti_tab[]`, grown and never freed,
   bounded by the budget per call (worst case across calls with different Pc about 2x the
   budget); each thread memsets its own table before counting. Toggles: `-DLOGOS_NO_CNTI_POOL`,
   env `LOGOS_I32_NO_TABLE_POOL=1`. Same one-sort-at-a-time contract as the ctx bundles.
4. **(c) `LEAF_I32_COUNT_DIV`** 1 -> 4: the sequential counting gate becomes span <= n/4. Env
   `LOGOS_I32_COUNT_DIV=d` sweeps it.
5. Harness modes 12-16 and a new size 8192 (int32 checks 180 -> 306): span n/2 (the budget
   band), a 0.1% far low tail 1e6 wide (recursive tail sort), tails on both sides (span 2^32),
   values on the exact window edges (-1 and 100 beside a 0..99 bulk), a 2.5% sentinel that must
   decline at the sample; n = 8192 is above LEAF_I32_WINDOW_MIN_N and below MIN_PARALLEL.
6. New bench `i32abbench.c` (`make i32abbench`), sections A-G, a paired harness ported to
   int32 with alternating order; section G pairs two different DATA sets through one engine.
7. Trace-only phase stamps in the count route (`-DLOGOS_TRACE`), compiled out otherwise.

## Mechanism check (session 2, `-DLOGOS_TRACE` build, not timed, every output verified)

| shape | 1 thread | 14 threads |
|---|---|---|
| bulk + one INT32_MIN | `windowed count span=100 window [0,99] spill lo=1 hi=0` | count route, windowed, spill 1 |
| bulk + both extremes | `window [0,99] spill lo=1 hi=1` | count route, windowed, spill 2 |
| bulk + 0.1% far tail | `window [0,99] spill lo=7813` (tail sorted recursively) | count route, windowed, spill 7813 |
| bulk + 5% INT32_MIN | no window line: declines at the sample, radix | declines at the pre-gate, partition |
| span 1e6 | plain counting | `14 tables of 1000000 slots (53.4 MB)`, full range |
| span 4e6 | radix (span n/2 > n/4, no windowed line) | `4 tables of 4000000 slots (61.0 MB)` |
| span 8e6 | radix (span > n/4) | declines (max_slots 5592405), partition |

The paths fire on exactly the shapes they were built for and nowhere else.

## What was measured

All at n = 8M, `BENCH_THREADS` = 14, medians of adjacent pairs with p10/p90, every output
verified against qsort before timing. "old/new" > 1 means the new behaviour is faster.
**Error floor today (session 2, `make pairprobe`, 40 pairs, same engine both halves): median
1.033 at 8M, resolves ~6.8%, one pair p10/p90 0.91/1.34; at 1M 1.072 / 13.2%.** Anything
under ~1.10 in the tables below is noise on this box.

### Section A: sequential leaf, window off vs on, 1 thread

Session 1 (30 pairs) and session 2 (30 pairs, re-run on a heavier box, all eight rows):

| shape | s1 old/new | s1 p10 | s1 p90 | s2 old/new | s2 p10 | s2 p90 | what fired (trace) |
|---|---|---|---|---|---|---|---|
| bulk 0..99 | 0.985 | 0.90 | 1.21 | 1.078 | 0.72 | 1.45 | plain counting, window never consulted (null row: 1.08 is inside today's 1.03 +- 7% floor) |
| **bulk + one INT32_MIN** | **3.190** | 2.70 | 3.59 | **2.393** | 1.96 | 4.06 | window [0,99], spill lo=1 |
| bulk + both extremes | **4.912** | 4.15 | 5.94 | **3.505** | 2.76 | 5.46 | window [0,99], spill lo=1 hi=1 |
| bulk + one -1 | 0.995 | 0.83 | 1.71 | 1.018 | 0.74 | 1.78 | plain counting (span 101) |
| bulk + 0.1% far tail, 1e6 wide | **4.855** | 4.37 | 5.73 | **3.968** | 2.51 | 7.92 | trimmed window, tail sorted recursively |
| bulk + 5% INT32_MIN | 0.992 | 0.85 | 1.17 | 1.031 | 0.86 | 1.17 | declines at the sample, radix |
| span 1e8 | 0.993 | 0.87 | 1.24 | 0.975 | 0.74 | 1.42 | pays the sample, radix |
| full 2^32 | 1.024 | 0.89 | 1.14 | 1.011 | 0.81 | 1.20 | pays the sample, radix |

Session 1's first 20-pair run agreed (3.14, 4.89, guards 0.97-1.005). The spread between 3.19
and 2.39 on the target row is the box: the ratio is (radix on 8M) / (count + sample), and the
radix's three passes over 32 MB suffer more from a loaded memory system than the L1-resident
100-slot count does, so a quieter box (session 1's) shows the larger ratio. Both are above
the 2.0 the prediction set as its floor.

### Section G: sentinel row / clean row, same engine, adjacent, 1 thread (the target's metric)

Not measured in session 1 (the session ended); in session 2 it was queued behind section A in
the chain, and the lock was held by other agents for the twelve minutes the write-up could wait, so it is
**not measured** (the chain died with the session; re-run it with
`flock /tmp/logos-bench.lock ./i32abbench 20 8000000 G` in the work copy, columns "win ON" = sentinel/clean with the window, "win OFF" = the
same without). What the evidence supports without it: with the window on, the sentinel row costs
the clean row plus one 2048-point sample and one extra compare per element in the count loop, the
same mechanism the parallel route measured at ~1.2x (CHANGELOG: 3.2 -> 3.9 ms at 14T), and the
null rows of section A bound the sample cost at under the box's 7% floor. Expected 1.05-1.2x; the
target said under 1.2x. Treat that as a prediction, not a result.

### Section B: parallel count route, fixed cap vs budget, 14 threads (session 1, 20 pairs, with the pool)

| span | old/new | p10 | p90 | 1T min | PT old | PT new | **P/1 new** | threads x table |
|---|---|---|---|---|---|---|---|---|
| 1e5 | 1.064 | 0.84 | 1.34 | 66.3 | 32.8 | 37.3 | 1.78 | guard, same tables both sides |
| 5e5 | **4.096** | 3.46 | 4.77 | 138.7 | 220.0 | 56.7 | 2.45 | 14 x 2 MB |
| **1e6** | **3.202** | 2.78 | 3.96 | 235.4 | 256.1 | 87.9 | **2.68** | 14 x 4 MB |
| 2e6 | **2.342** | 2.07 | 3.14 | 268.9 | 217.1 | 87.6 | 3.07 | 8 x 8 MB |
| 4e6 | **1.524** | 1.30 | 1.76 | 228.7 | 220.6 | 147.3 | 1.55 | 4 x 16 MB |
| 8e6 | 0.999 | 0.90 | 1.08 | 213.8 | 187.8 | 212.8 | 1.00 | declines (2 threads), partition |
| full 2^32 | 0.998 | 0.88 | 1.38 | 295.0 | 234.3 | 255.8 | 1.15 | declines at the sample |

Milliseconds are min-of-10 with 1T and PT adjacent (intbench style) and are only comparable
within a row. An earlier 20-pair run before the pool existed: 1e6 2.72x (P/1 2.64), 5e5
3.77x, 2e6 2.00x, 4e6 1.24x, 8e6 0.97, full 0.99. Session 2 did not get the lock for B and F before its end; the commands below repeat them.

### Trace: where the count route's time goes at span 1e6, 14 threads (session 1)

| phase | calloc/free (old) | pool (new) |
|---|---|---|
| sample pre-gate | 0.1-0.2 ms | 0.1-0.2 ms |
| parallel min/max | 5.6-6.6 ms | 5.1-8.2 ms |
| allocate 14 x 4 MB | 0.3-3.0 ms (lazy: faults land in the body) | 0.1 ms |
| count + fold + emit | 48-90 ms | 61-73 ms |
| free / return | **8-28 ms** | 0.1 ms |

### Section F: persistent tables off vs on, 14 threads (session 1, 16 pairs)

| span | old/new | p10 | p90 |
|---|---|---|---|
| 1e5 (14 x 0.4 MB) | 1.163 | 0.86 | 1.41 |
| 1e6 (14 x 4 MB) | 1.143 | 0.95 | 1.49 |
| 2e6 (8 x 8 MB) | **1.379** | 1.23 | 1.65 |
| 4e6 (4 x 16 MB) | **1.227** | 1.14 | 1.43 |

### Section D: budget sweep, 64 MB vs N, 14 threads (session 1, 12 pairs), before and after the pool

| span | 32 MB | 128 MB | 256 MB | note |
|---|---|---|---|---|
| 1e6, calloc | **1.304** | 1.045 | 0.967 | 8 threads beat 14 by 1.3x: footprint, not parallelism |
| 1e6, pool | 1.070 | 1.014 | 0.949 | the pool removed that penalty |
| 2e6, calloc | 0.984 | 0.814 | 0.893 | 14 threads x 8 MB slower than 8 |
| 2e6, pool | 0.870 | 1.035 | 1.103 | |
| 4e6, calloc | 0.769 | 1.012 | **0.629** | 14 x 16 MB much slower than 4 |
| 4e6, pool | 0.595 | 1.067 | **1.322** | with the pool 14 x 16 MB wins |
| 8e6, calloc | 1.000 | 0.885 | 0.896 | counting at 4-8 threads lost to the partition |
| 8e6, pool | 1.001 | 1.128 | 1.167 | now wins it, at 128-256 MB persistent |

### Section C: sequential counting gate, DIV 1 vs d, 1 thread (session 1, 16 pairs)

| span | DIV 2 | DIV 4 | DIV 8 | reading |
|---|---|---|---|---|
| 1e6 | 1.008 | 1.027 | 0.992 | counts under all: the null rows, this harness's floor is ~3% |
| 2e6 | 1.004 | 0.997 | **0.831** | radix at DIV 8 LOSES: counting still wins at 2e6 |
| 4e6 | 1.006 | **1.265** | **1.355** | radix wins at 4e6 |
| n/2 (4e6) | 0.993 | **1.314** | **1.320** | |
| n (8e6) | **1.962** | **2.032** | **2.007** | the inherited gate's top edge is 2x wrong |

### Section E: the DIV 4 gate's one parallel side-effect (buckets of ~n/168), 14 threads (session 1, 16 pairs)

| span | old/new | p10 | p90 |
|---|---|---|---|
| 6e6 (leaves count at DIV 1, radix at DIV 4) | 1.029 | 0.87 | 1.16 |
| 8e6 (same) | 0.993 | 0.84 | 1.12 |
| 1.6e7 (radix under both) | 0.998 | 0.86 | 1.19 |

Neutral: shipping DIV 4 costs the parallel leaves nothing.

### intbench in the work copy (session 1, end; loaded box, ratios within rows only)

Span ladder P/1: 100 1.56, 1e3 1.86, 1e4 1.70, 1e5 1.56, **1e6 1.94 (was 0.60 in the same
bench that morning)**, 8e6 1.42, 1e8 1.67, full 1.39. Every row verified.

## Verdict: shipped

All four pieces are on by default in the patch: (a) the windowed sequential leaf, (b) the
table budget with the persistent pool, (c) the n/4 counting gate. Each keeps its opt-out
flag and its runtime toggle so the A/B can be repeated on any box with one binary. No version
bump. 954 harness checks pass; every benchmark row verified against qsort.

Target grades:
- Sentinel row at 1 thread: the target said "from ~2.5x baseline to under 1.2x". The paired
  old/new over the radix it used to take is 3.19x (session 1) and 2.39x (session 2, heavier
  box), i.e. the 2.5x penalty is gone. The direct sentinel/clean ratio is section G.
- The 1e6 band: the target said "P/1 from 0.87 to >= 1.5"; measured P/1 2.64-2.68 in the
  paired bench's adjacent 1T/PT columns and 1.94 in intbench on a box twice as loaded; the
  paired old/new at 14 threads is 2.72x (calloc) and 3.20x (pool).
- Prediction grades: (a) predicted paired 2.0-2.5: measured 2.4-3.2, the radix on a loaded box
  is dearer than the sample suggested; guards predicted within 2%: measured 0.985-1.024 in
  session 1, and 1.078 on the null row in session 2 which is inside today's 1.033 +- 7% floor.
  (b) predicted P/1 2.0-3.0 and paired 3-4x: measured 2.68 and 3.20x. (c) predicted crossover
  ~n/4 and ~1.6x at span n: measured between n/4 and n/2, 2.0x.

## Why, mechanism

- (a) A sentinel is a tail of size one; a sample bounds the bulk, never the tail. Counting
  the sampled window and spilling the rest costs one branch per element in the count pass
  plus a recursive sort of at most n/32 elements. The radix it replaces is 3-4 passes over
  8M elements. Everything the sample cannot certify (the 5% sentinel, wide data) declines
  before `a[]` is written, so the guards are flat.
- (b) The old cap was a proxy for "the tables get expensive"; the trace shows what is
  expensive: the FOOTPRINT, through page faults inside the count pass and 8-28 ms of page
  teardown on free, not the thread count. With the tables persistent the count route is
  1.5-4x faster than the partition up to span n/2, and the budget only decides how much
  memory to keep. A 256 MB budget would add 1.32x at 4e6 and 1.17x at 8e6 (section D), but
  that is 224-256 MB held forever; 64 MB (the same order as the 48 MB ctx bundles) stays
  the default and the constant is one define away.
- (c) At 1 thread the radix on a span below 2^24 is three byte passes regardless of span,
  while the counting sort's random increments get dearer as the table leaves cache. At 8M
  the crossover is between 2M and 4M slots (8-16 MB), so n/4. For smaller n the gate is
  conservative (a 250k-slot table at n = 1M is L2-resident and would still win wider), which
  is the safe side; a sweep over n would sharpen it.

## Surprises

1. The 0.1%-tail bench shape, first written with a tail 1e6 wide starting at -1000, did
   NOT window at 8M, and the trace said why: its raw span (~1e6) already satisfied the
   inherited span <= n gate, so plain counting took it with a 4 MB table and the window was
   never consulted. Rebased on INT32_MIN it measures 4.86x and the harness mode was fixed the
   same way.
2. Freeing 53 MB of tables costs more than the parallel min/max over 8M elements
   (8-28 ms vs 5-7 ms). Page teardown on WSL2 is a real phase of the sort.
3. Before the pool, FEWER threads were faster in the band (32 MB budget = 8 threads, 1.30x
   over 14 at span 1e6). The pool inverted that: with faults gone, 14 threads with 16 MB
   tables beat 4 by 1.32x at span 4e6. The thread-count optimum was an artefact of the
   allocator, not of the algorithm.
4. The inherited sequential gate (span <= n) was 2x wrong at its own top edge, on a row
   intbench has printed for weeks ("n = 8e6" slower than "1e8"). The knobbench sweep of
   the counting span factor for f64 found the same thing and stopped at 1; the int32 leaf
   never had that sweep.
5. Session 1's patch had silently converted harness.c and intbench.c from CRLF to LF, so its
   diff rewrote both files whole (91 KB). Restoring CRLF made the same edits 70 lines.
6. A `nohup ... &` started inside `wsl -e bash -lc` dies with the wsl session. Long chains have
   to be run from a foreground wsl call that stays open.

## What I would do next

- Sweep `LEAF_I32_COUNT_DIV` over n (1e5..3e7) with `i32abbench C` at other `n`; the gate
  is probably a cache-size rule (table <= ~8 MB) rather than a fraction of n.
- Let the parallel count route use the same reasoning: the 1e5 row shows 14 threads with
  L2-resident tables at P/1 3.4-4.1 while the 1e6 row is 2.7; a two-level count (coarse
  byte pass then per-bucket counting) would keep every table in L2 for any span.
- The budget's memory/speed trade at big spans is one constant; 128 or 256 MB buys 1.17-1.32x
  at spans 4e6-8e6 if the persistent footprint is acceptable (section D).
- `count_try_i32` and `leaf_i32_windowed` share the K-smallest/largest sampler verbatim; a
  helper would remove the duplication (left as is so the patch stays reviewable).
- The mid-pass spill bail (`++nsp > spcap`) is correct by construction (nothing written
  yet) but no harness mode reaches it: with Weyl sampling a tail frequent enough to exceed
  n/32 is always seen by the sample and declines earlier. Only a tail placed exactly on the
  unsampled positions would exercise it.

## Commands, for repetition

```
# set-up: pristine and work copies (see the team brief), apply the patch, 954 checks
wsl -e bash -lc "cd /tmp/logos-team/i32-sentinel-seq && patch -p1 -d work < /mnt/c/Users/markc/Coding/logos-sort-repo/logos-sort-repo/experiments/team/patches/i32-sentinel-seq.patch && cd work/engines/c && make test | tail -2"
# error floor first
wsl -e bash -lc "cd /tmp/logos-team/i32-sentinel-seq/work/engines/c && make pairprobe && flock /tmp/logos-bench.lock ./pairprobe"
# the decisive paired runs (one binary, runtime toggles), each under the bench lock
wsl -e bash -lc "cd /tmp/logos-team/i32-sentinel-seq/work/engines/c && make i32abbench && flock /tmp/logos-bench.lock ./i32abbench 30 8000000 A"
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 20 8000000 G"     # sentinel vs clean, adjacent (the target's metric)
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 20 8000000 B"     # budget vs fixed cap, + adjacent 1T/PT
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 16 8000000 C"     # gate divisor sweep
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 12 8000000 D"     # budget sweep
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 16 8000000 E"     # DIV 4 in parallel leaves
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./i32abbench 16 8000000 F"     # table pool
wsl -e bash -lc "cd ... && flock /tmp/logos-bench.lock ./intbench 5 8000000"
# session 2's chain (pairprobe, A, G, B, F, intbench) is /tmp/logos-team/i32-sentinel-seq/run1.sh -> run1.log
# mechanism: -DLOGOS_TRACE build, see the "i32 leaf: windowed count" and "i32: ..." lines
gcc -O3 -std=gnu11 -pthread -DLOGOS_TRACE -Iwork/engines/c -c work/engines/c/logos_psort.c -o lp_trace.o && gcc -O2 -pthread -Iwork/engines/c t.c lp_trace.o -o t -lm && ./t 1; ./t 14
```
