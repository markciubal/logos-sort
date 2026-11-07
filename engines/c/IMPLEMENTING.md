# Implementing logos-sort in C — a step-by-step guide

> **Scope, 2026-11-06.** This guide describes the ORIGINAL core: a parallel sample sort
> over an interpolation leaf, the ~220-line design `logos_psort.c` started from. The
> shipped engine is that core with a dispatcher of some thirty structure-reading routes
> in front of it and runs to about 11,000 lines; its public API is `logos_psort.h`, its
> history `CHANGELOG.md`, its measurements `../../FINDINGS.md`, and `README.md` here says
> how it decides. The steps below are still the right order to build the core from
> scratch, and §6's heavy-tail limitation is solved in the shipped engine (the key grid,
> the exponent-bit coarse path, the quantile partition). Where this guide and the source
> disagree, the source is current.

This walks through building the algorithm from scratch, then verifying and
benchmarking it against `qsort`. The finished reference implementation is
`logos_psort.c`; the harness is `harness.c`.

## 0. The idea, in one paragraph

Don't compare values — *place* them. Read the min and max, divide that range
into K slots, and compute each value's slot with arithmetic: `slot = (v − min) ×
K / (max − min)`. One pass to count slot sizes, one pass to scatter values into
their slots, then recurse into each slot until slots are tiny and insertion sort
finishes them. For well-spread data this touches memory far fewer times than a
comparison sort's `n log n` comparisons. To use many cores, first split the data
into P value-balanced buckets using *sampled quantiles* (rank-based, so it's
robust to skew), then sort each bucket on its own core — the buckets are
independent, so no locks are needed.

## 1. Files and build

```
logos_psort.h   public API
logos_psort.c   the engine
harness.c       correctness (vs qsort) + benchmark (vs qsort)
Makefile
```

Build flags: `-O3 -pthread`. Link `-lpthread -lm`. Any C99 compiler with POSIX
threads (Linux, mac_os, WSL). No other dependencies.

## 2. Implement, in this order

Build bottom-up; each step is testable on its own.

### Step 1 — the leaf primitives

```c
static void insertion_sort(double *a, long lo, long hi);  /* tiny arrays */
static void heap_sort(double *a, long lo, long hi);       /* in-place O(n log n) safety net */
```

Insertion sort finishes slots below ~64 elements (cheap, cache-friendly).
Heapsort is the *escape hatch*: whenever interpolation can't help, sorting must
still terminate in `O(n log n)` with no extra memory. Write it so it can never
be skipped — it's what makes the worst case bounded.

### Step 2 — the coarse interpolation sort (the heart)

Signature: `coarse_bucket_sort(a, lo, hi, depth, mn, mx, buf, ctx)`; sorts
`a[0..n)` in place using scratch `tmp` (≥ n), result left in `a`.

1. **Guards first.** `n < 64` → insertion. `depth == 0` → heapsort.
2. **Min/max in one pass.** If `range = max − min` is not `> 0` or not finite
   (all-equal, or inf/NaN contamination) → return if all-equal, else heapsort.
3. **Slot index — the one piece with real pitfalls:**
   ```c
   long b = (long)((v - mn) * scale);          /* scale = K / range */
   if (b < 0) b = 0; else if ((size_t)b >= K) b = K - 1;
   ```
   `v − mn ≥ 0` so negatives shouldn't occur, but clamp anyway; and `v == max`
   yields exactly `K` after rounding, so the upper clamp is *required*.
4. **Concentration guard.** After counting, if one slot holds more than 87.5% of
   the elements, interpolation isn't splitting the data — fall back to heapsort
   rather than recurse unproductively. (This is what triggers on heavy tails;
   see §6.)
5. **Count → prefix-sum → scatter into `tmp`.** Keep a separate cursor array;
   the prefix sums give slot start offsets, the cursors advance as you scatter.
6. **Recurse per slot with roles swapped:** `coarse_bucket_sort` on each sub-bucket
   sorts each slot *in `tmp`*, using the corresponding region of `a` as its
   scratch. Then `memcpy(a, tmp, n)` brings the sorted result home.

Use `K = min(n, 1024)`: 1024 slots keep the counter array in L1 cache.

### Step 3 — sampling and splitters (for the parallel top level)

Take `P × 32` strided samples, sort them (coarse sort is fine), and choose
`P − 1` splitters at the sample quantiles. Quantiles are **rank-based**, so
buckets come out balanced even when values are skewed — this is deliberately a
different mechanism from the value-based slots inside the leaf.

`bucket_of(x)` = number of splitters `≤ x` (a binary-search upper bound).

### Step 4 — the three parallel phases (pthreads, no locks)

Thread `t` owns input slice `[t·chunk, (t+1)·chunk)`.

- **Histogram:** each thread counts its slice into P buckets, writing its *own*
  row of a `P×P` count matrix.
- **Combine (serial, O(P²)):** bucket sizes → bucket start offsets → per-thread
  write cursors: `offset[t][b] = start[b] + Σ_{t' < t} count[t'][b]`.
- **Scatter:** each thread copies its slice into the output at its cursors.
  Every `(t, b)` pair writes a range disjoint from every other — that
  disjointness, guaranteed by the prefix sums, is the *entire* reason no locks
  are needed. If you change the offset math, you must re-prove it.
- **Sort buckets:** bucket `b` → thread `b`, sorted in place with
  `coarse_bucket_sort`. The input buffer is free now — reuse it as scratch.

Join between phases (a `pthread_join` loop is a barrier). Spawning threads per
phase is simplest; a persistent pool is the standard later optimization.

### Step 5 — public wrappers

`logos_sort_f64` calls the engine directly. `logos_sort_i32` widens to
`double` (every `int32` is exactly representable), sorts, narrows back — a
correct shortcut costing two O(n) passes; a native integer leaf is the upgrade.

### Step 6 — verify before you benchmark

Never benchmark an unverified sort: a fast wrong answer is worse than a slow
right one. The harness compares against `qsort` element-for-element **and**
checks an XOR checksum of the raw bits (permutation-invariant, so it proves
nothing was lost or invented), for every shape × size × thread count. Only when
that passes do the timings mean anything.

## 3. Run it

```sh
make test         # builds ./harness and runs it
# or manually (the harness also links the extras and the view):
cc -O3 -std=gnu11 -pthread logos_psort.c logos_extra.c logos_view.c harness.c -o harness -lpthread -lm && ./harness
```

Exit code `0` means every correctness check passed. Output has two parts:

1. `== correctness ==` — counts of checks and failures (must be 0).
2. `== benchmark ==` — per shape and size: `qsort` time, logos at 1 thread with
   its speedup `x`, and logos at N threads with its speedup.

If the mount you're in is `noexec` (some sandboxes), build in `/tmp`.

## 4. Reading the results honestly

- **Speedup columns are vs `qsort`**, a comparison sort with a function-call
  per comparison. That's a fair "generic C sort" baseline but a slow one; a
  hand-inlined pdqsort/ipnsort is a tougher opponent (see the Rust port, where
  logos still wins ~1.3× on doubles).
- **The N-thread column only shows speedup on a machine with N real cores.** On
  a single core it shows *coordination overhead* and will be slower than
  1 thread — that is correct behaviour, not a bug. Expect the parallel gain to
  flatten at the memory-bandwidth wall (~4–6× at 8 cores on one socket).
- Run on a quiet machine; use best-of-k, as the harness does.

## 5. Tuning knobs

| constant | default | effect |
|---|---|---|
| `INSERTION_N` | 64 | slot size below which insertion sort takes over |
| `COARSE_K` | 1024 | slots per interpolation level (keep counters in L1) |
| `OVERSAMPLE` | 32 | samples per bucket for splitters (more = better balance) |
| `MIN_PARALLEL` | 65536 | below this, stay sequential (thread spawn isn't free) |

Change one, re-run `make test`, keep it only if the *table* improves. Every
knob here was chosen by measurement on one machine; yours may differ. (The
shipped engine's constants were later swept four values each over nineteen
shapes, `make knobbench`, TRUTH_TABLE.md §N; a knob A/B today is a paired run
in one binary, `make roundbench` or `make twinbatch`, not two `make test` runs.)

## 6. Known limitation: heavy-tailed data

The harness deliberately includes a Pareto-tailed shape, and logos **loses to
qsort on it** (~0.7–0.8×). Cause: with most values tiny and a few enormous,
value-interpolation puts nearly everything in slot 0, the concentration guard
fires, and the whole region falls back to heapsort. Two fixes, both proven in
the JavaScript engine this was ported from:

- bucket on the **exponent bits** (i.e. in log-space) when a sampled skew probe
  detects a heavy tail — the "bits-coarse" path; or
- apply the **quantile splitting** from Step 3 recursively, since it is
  rank-based and immune to skew.

Evidence the second works is already in the table: on the heavy-tail rows the
4-thread run is *faster than 1 thread even on a single core*, because the
quantile top-level partition breaks the concentration before the leaf sees it.

## 7. Pitfalls checklist

- Clamp the slot index at both ends (§2.3). Forgetting the upper clamp is a
  one-in-a-million out-of-bounds write.
- Guard non-finite ranges; `inf − (−inf)` and NaN will otherwise produce
  `scale = 0` or NaN and degenerate bucketing.
- Thread count > n, n = 0, n = 1, all-equal input, and `malloc` failure all
  need explicit paths (the reference falls back to `qsort` if scratch can't be
  allocated).
- NaN ordering is unspecified; document it rather than promise it.
- This sort is **not stable** and handles **numbers only** (strings need a
  different engine).
