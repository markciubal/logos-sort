# Three paradigm shifts, measured

The question: on today's CPU, logos-sort's philosophy (address, don't compare)
loses to vectorized SIMD sorting. Which of three proposed paradigm shifts would
flip the result? Each was implemented and benchmarked against the SIMD baseline
(Intel x86-simd-sort, AVX-512), n = 8M, every output verified. Source:
`paradigm.cpp`. Raw run: `paradigm-output.txt`.

## The one-line answer

**Don't change the sort — change the deliverable.** #1 (make the addressing
itself vectorize) fails, as the memory-wall thesis predicts. #2 and #3 win by
*not producing a sorted array at all* — answering the order or analytics query
the scatter was only ever a means to.

---

## #1 — Vectorize the addressing (the operation SIMD avoids). FAILS.

Can the two operations logos depends on — the scatter and the histogram — be
made to vectorize on a CPU?

| operation | scalar | AVX-512 | result |
|---|---|---|---|
| permutation scatter `out[perm[i]]=a[i]` | 52.8 ms | 50.4 ms (`vpscatter`) | **1.05× — no help** |
| histogram `count[bucket]++` | 3.9 ms | 6.0 ms (`vpconflictd`) | **0.65× — slower** |

`vpscatter` exists but decomposes to serial stores in hardware — a 5% difference,
noise. The conflict-detection histogram (the textbook "vectorized histogram") is
*slower*, because it needs a gather + a scatter per step, both of which are the
very memory operations it was trying to avoid. **Confirmed: the addressing
paradigm cannot win on a CPU by materializing a result.** The scatter is
load-bearing and does not vectorize. This is the wall, restated from the inside.

---

## #2 — Never materialize the permutation: answer ORDER queries. WINS, narrowly.

Build a model that answers `rank(x)` / `select(k)` from `(value, count)` over the
*distinct* values only — one no-scatter pass plus a tiny sort of the distinct
set — versus SIMD-sorting the array and binary-searching. Workload: build + 2M
rank queries.

| distinct values | sort + bsearch | order-model | result |
|---|---|---|---|
| 64 | 51.3 ms | 32.6 ms | **1.57× — model wins** |
| 4,096 | 69.7 ms | 124 ms | 0.56× — model loses |
| 200,000 | 97.4 ms | 227 ms | 0.43× — model loses |

The model skips the scatter entirely and wins **when distinct ≪ n**. As
cardinality rises it degenerates (building it *is* a sort of the distinct set),
and loses. So this pivot is real but narrow: it wins exactly in logos-sort's
existing niche — low-cardinality data — and only when the deliverable is order
queries rather than the array. Honest verdict: a niche win, not a general one.

---

## #3 — Distribution-awareness as the product: analytics without sorting. WINS, decisively.

Most real "sorts" are a step toward an analytics answer — a quantile, a top-k, a
range count. None of those needs a sorted array. Answer them from a one-pass
profile instead:

| query | sort-first baseline | one-pass profile | speedup | exact? |
|---|---|---|---|---|
| quantiles p50/p95/p99 | 100 ms (full sort) | **1.15 ms** (16k strided sample) | **87×** | approx (0.4% / 2.4% / 6.8% error) |
| top-100 | 69.5 ms (`nth_element`) | **10.2 ms** (one-pass min-heap) | **7×** | **exact** |
| count-in-range | 97.5 ms (sort + bsearch) | **2.35 ms** (one scan) | **42×** | **exact** |

Two of the three are **exact** and still 7–42× faster; the quantile one is
approximate but within a few percent for 87×. None scatters. The point isn't
that logos beats a well-chosen algorithm — it's that the "sort first" reflex is
usually wrong: the sorted array was never the goal, and the moment you stop
materializing it, the scatter (and the SIMD sort's whole advantage) disappears
from the problem.

---

## Conclusion — the paradigm shift that actually pivots the result

Ranked by what the measurements support:

1. **#3 is the real pivot.** Reframe logos-sort from "a faster sort" to "a
   one-pass order/analytics engine." Its distribution-awareness — which SIMD
   fundamentally lacks — becomes the product, and the scatter that sinks it
   never has to happen. 7–87× where it applies, and it applies to a large share
   of what sorting is actually used for.
2. **#2 is a narrower, real pivot** for order queries on low-cardinality data —
   logos's existing niche, now expressed as a queryable model instead of a sort.
3. **#1 is a dead end on a CPU** (confirmed, both sub-tests). It would only pay
   off on hardware where scatter is a first-class parallel primitive — a GPU or
   processing-in-memory — which is a different machine, not a different sort.

The synthesis stands: **SIMD wins the race to a sorted array; logos-sort's bet is
that the sorted array is often the wrong finish line.** These numbers say the bet
pays — but only once you stop trying to win the race SIMD is winning, and change
what you're racing toward.
