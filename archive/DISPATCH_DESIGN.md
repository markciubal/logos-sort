# Sort Dispatch Design Notes

A focused record of how we approached algorithm selection in this sort
library, what alternatives were considered, and why the current design
landed where it did. Intended as a reference for anyone revisiting this
question — or for ourselves before we re-explore something we already
ruled out.

Companion to `sort_expression.js`, `sort_expression_inplace.js`,
`argsort.js`, and `find_outliers.js`.

---

## The problem

A multi-algorithm sort has to choose, at runtime, which of its constituent
algorithms to use on a given input. Different algorithms win on different
data:

- Flash sort: O(n) on numeric data with bounded range
- MSD radix / AFS: O(n × L) on strings, dominates when value space is small
- Insertion sort: O(n) on nearly-sorted, O(n²) on random
- Introsort: O(n log n) general fallback, robust against adversaries
- TimSort-style run merging: O(n) on data with pre-existing runs

The dispatch question: which one to invoke when, without paying so much
to decide that the savings are lost.

---

## Five approaches considered

### 1. Static structural heuristics (shipped)

Decide based on facts known at call time without examining data content:
the type of values (`typeof arr[0]`), the array length, and the result
of a single short scan (asc/desc check piggybacks on min/max).

Properties:
- Zero dispatch cost when the structural facts are clear
- Always-correct on the distributions the heuristics were tuned for
- Silently wrong on unfamiliar distributions

This is what `sort_expression.js` v3.8 ships. The dispatch tree is
roughly:

```
type === 'string'?
  → string IIFE:
      asc/desc/rotation fast path (O(n) scan, free piggyback)
      n < 16? insertion sort
      run-detection probe → TimSort-style merging if structure found
      otherwise → MSD-at-depth (post common-prefix skip)

type === 'number':
  → numeric IIFE:
      asc/desc/rotation fast path
      n < THRESHOLD? insertion sort
      multimodal partition probe
      flash sort (Float64 / Int32 specialization)
      → introsort fallback (depth-limited, heapsort safety net)
```

Each branch costs O(1) or O(n) to evaluate; none costs more than a single
pass over the data, and most pieces of evaluation piggyback on a pass
the algorithm itself needs anyway.

### 2. Statistical fingerprinting (rejected)

Take a small sample, compute statistics (entropy, order ratio, inversion
density), use those to predict the right algorithm.

Tested probes (from `topology.js` experiment):

| Metric | Probe accuracy | Notes |
|---|---|---|
| Inversion density | ~96% accurate | Pairwise comparisons in a sample are an unbiased estimator of pairwise comparisons in the whole |
| Order ratio (longest ascending subseq) | ~50% accurate | Sampling can't see long-range structure; sawtooth/rotated/bimodal all look "half-ascending" in any stride sample |
| Shannon entropy | Bounded by sample size | 64-element sample can't represent entropy above log2(64) = 6 bits |

The order-ratio failures are instructive: a stride sample of sawtooth
data shows ~50% ascending pairs (locally consecutive values are
ascending), even though the *global* longest-ascending-subsequence is 1.
The sample sees local correlation that doesn't extrapolate.

**Why this was rejected**: the metrics that ARE accurate (inversion
density) duplicate signals our existing run detection already gets for
free. The metrics that WOULD be informative (order, entropy) can't be
measured cheaply enough to use as a routing input.

### 3. Runtime racing (rejected for general use)

Bench candidate algorithms on a small sample at dispatch time, run the
winner on the full data. Tested at sample sizes 100, 1000, 10000 on
n=100,000.

Results (`racing.js`):

| Sample size | Correct winner | Probe cost | Overhead vs oracle |
|---|---|---|---|
| 100 | 3/8 distributions | <1ms | Catastrophic when wrong (up to 100,000× — insertion sort default at small n) |
| **1000** | **7/8 distributions** | **~1ms** | **20–50%** |
| 10,000 | 7/7 distributions | ~50ms | 300–4000% |

The deeper insight: **complexity classes only manifest at scale**. At
n=100 every algorithm runs in under a millisecond and insertion sort
wins everything; that result doesn't extrapolate to n=100,000 where it
catastrophically loses. The sample needs to be large enough for
asymptotic behavior to show through the constant-factor noise, but
small enough that the probe doesn't cost more than the sort. The window
exists around n=1000 for our distributions, but the resulting overhead
(20–50%) is pure cost against our existing heuristics, which are
already correct without any probe.

**Where racing remains viable**:
- Custom comparators (no static heuristic possible)
- n very large (>1M, probe overhead becomes negligible)
- Non-numeric, non-string data types

We don't have these APIs yet. When we do, racing is the natural
fallback for the "we have no heuristic to apply" case.

### 4. Voting + selective racing (rejected, instructive)

Run multiple cheap heuristics in parallel, each votes for an algorithm.
If 3+ agree, dispatch directly. If they split, race the disagreers on
a sample.

Implemented four probes in `voting.js`:
- `probeOrder`: monotonic stretches in a stride sample
- `probeRange`: min/max in a stride sample
- `probeDiversity`: distinct values in a stride sample
- `probeInversions`: pairwise inversions in a stride sample

Results were mixed in an interesting way:

| Distribution | Vote outcome | Result |
|---|---|---|
| uniformRandom | 4/4 flashsort | +1% overhead ✓ |
| sorted | 2-1-1 split → race → insertion | +43% ✓ |
| bimodal | 3/4 flashsort | +1% ✓ |
| manyDuplicates | 4/4 flashsort | +8% ✓ |
| **fewBigOutliers** | **3/4 flashsort (CONFIDENT WRONG)** | **+25,498%** |
| nearlySorted95 | race outcome noisy across trials | +2594% (timing-noise dependent) |

Two distinct failure modes emerged:

**Correlated probes amplify shared blind spots.** All four probes
stride-sampled the same array. The 10 outliers in `fewBigOutliers`
(values 1e10 among ~100k values near 1) had ~1% probability of
appearing in any single 64-element sample, so all four probes missed
them, all four confidently voted flashsort, and the dispatcher
confidently chose the worst possible algorithm. **Voting doesn't catch
errors when the heuristics share a sensor.**

**Race noise at the resolution point.** When votes split, the race
itself becomes the arbiter. At sample size 1000, the timing difference
between competitive algorithms is within noise floor — so the race
coin-flips between equally-plausible options, and the chosen algorithm
varies across calls. For `nearlySorted95`, this meant occasionally
selecting insertion sort (660ms) over flashsort (3ms), bringing the
average to a useless number.

**Why this was rejected**: voting adds dispatch complexity without
catching the failures we'd want it to catch (the correlated-probe
problem) and amplifies timing noise into algorithm noise (the split-vote
problem). For our well-tuned heuristics, it's pure cost.

**What the experiment was useful for**: confirming that *uncorrelated*
heuristics would be needed for voting to add value — and that producing
truly uncorrelated probes requires either expensive full-data analysis
or fundamentally different sensors (e.g., timing vs value vs ordering).
The cheap-and-uncorrelated regime turns out to be small.

### 5. Hybrid: structural triggers for racing, heuristics elsewhere

Use cheap structural cues to decide *when* to race:

- Custom comparator passed → race (no heuristic possible)
- Unknown data type → race
- n > 1M → race (probe cost negligible)
- Standard numeric/string with n in normal range → heuristic

This is the design candidate if/when we add a custom comparator API.
Detection is free because the trigger comes from the call structure,
not from data content.

---

## The topology framing

The exploration that justified rejecting (2) and (3) used a "landscape"
metaphor: treat inversions as potential energy, sortedness as proximity
to the ground state. Different distributions have different landscape
shapes:

```
              ENTROPY (value diversity)
              low ←──────────────────→ high

ORDER  high   trivially sorted        smooth basin
  ↑           (insertion sort wins)   (insertion / run merge)
  │
  ↓           bumpy plain             rugged basin
       low    (flashsort dominates)   (flashsort or comparison sort)
```

The diagonal — high order + low entropy, low order + high entropy —
doesn't appear in natural data. Two cheap measurements (entropy +
order) give enough information to pick the right algorithm for ~90%
of real distributions.

Our existing dispatcher exploits this implicitly:
- Order detection (asc/desc/runs) catches the high-order regime
- Type detection + range checks catch the low-entropy regime
- Default to flashsort (numeric) or MSD-at-depth (strings) for the rest

What it doesn't catch: the off-diagonal corner cases. `fewBigOutliers`
is a "high-order, looks-bounded-but-isn't" pathology. Our sort would
fall through to flashsort and lose ~250× on this input. No simple
probe catches this; only outlier detection (which is itself
O(n × sqrt(n))) would.

---

## Principles extracted

A few rules emerged from these experiments that we'd want to follow
going forward:

**1. Static structural triggers beat content-based triggers.**
Detect by what the call IS (type, size, presence of comparator), not
by what the data CONTAINS. Content inspection costs roughly as much
as just running the algorithm.

**2. The detection problem recurses.**
"When should we race?" requires detecting uncertainty, which requires
examining data, which costs as much as racing. There's no
free-information shortcut. We must either accept the cost of detection
(make racing always-on for some structural class) or accept the
heuristic's blind spots.

**3. Uncorrelated heuristics is the hard part.**
Multiple cheap heuristics that all sample the same way aren't four
heuristics — they're one heuristic with three redundant copies. Real
diversity requires different sensors (timing, value distribution,
ordering, neighbor relationships) and most of those individually cost
more than a single good heuristic.

**4. One-line wins beat architectural wins.**
The optimizations that shipped were small: predictive free-merge (3
lines), common-prefix skip (~20 lines), MSD-at-depth replacing
multikey QS (~50 lines). The architectural attempts that DIDN'T ship
include WASM TimSort, cache-blocked numeric sort, burstsort with
Map-based trie, voting+racing dispatch. Generalization: the JS engine
is already doing more for us than custom data structures or compilation
pipelines can recover.

**5. Negative results have documentation value.**
Future maintainers will be tempted by the same ideas we tried. Recording
*why* something was rejected — with the actual numbers — saves them the
exploration. This document is that record for the dispatch question
specifically.

---

## When to revisit

The design choices here are stable for the workloads we tested. They'd
be worth revisiting if:

- We add a **custom comparator API**. Racing becomes the natural
  fallback because no heuristic can apply.
- We add **non-numeric, non-string data types**. Same reason.
- We need to handle **adversarial inputs**. The fewBigOutliers
  pathology shows our current dispatcher can confidently lose 250× on
  crafted inputs. If sort is exposed to untrusted data, we need a
  safety check — perhaps a per-element wall-clock budget that aborts
  and tries another algorithm if the chosen path is running too slow.
- We measure **real-world workloads** and find a distribution class
  we haven't characterized. The right response there is usually to
  add a new heuristic, not change the dispatch architecture.

---

## File map

- `sort_expression.js` — main sort, v3.8, buffered
- `sort_expression_inplace.js` — v3.8-inplace with American Flag Sort
- `argsort.js` — index-permutation sort with adaptive duplicate detection
- `find_outliers.js` — anomaly detection via sort resistance
- `DISPATCH_DESIGN.md` — this document
