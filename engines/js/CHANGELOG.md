# Changelog

## 0.3.0

This is the feature-rich engine line, published as 0.3.0 to supersede the lean
0.2.x standalone build as `latest`. It restores and extends the multi-strategy
engine: `argSort` and the permutation utilities, typed-array fast paths
(`sortF64` / `sortI32`), anomaly detection (`findOutliers`), the expensive-
comparator sort (`sortWithProxy`), and optional native acceleration — none of
which exist in 0.2.x. The 0.2.x line was a parallel minimal rewrite; 0.3.0 is the
continuation of the original feature line (the unpublished 0.1.3/0.1.4 work,
consolidated and version-bumped above 0.2.1).

Relative to 0.2.x, the numeric engine adds counting / flash / coarse-bucket /
bits-coarse / quantized-float-radix paths and run-merge with galloping on top of
the introsort + int32-radix core; the string engine adds per-level LCP skip,
packed-prefix radix, and an adaptive galloping merge on top of multikey
quicksort. Routing among them is chosen by sampled probes (skew, quantization,
distinct-ratio) plus free piggybacked statistics.

Notable correctness items folded in from the 0.1.x development line:
- **Non-ASCII string safety.** Strings containing characters beyond Latin-1
  (charcode > 255) are handled by a UTF-16-correct fallback in the byte-radix
  path; an OR-accumulator on the counting pass detects them at ~zero cost.
- **No front-loaded near-sorted cliff.** Momentum insertion carries a
  displacement budget and aborts to the distribution sort when the data turns out
  not to be near-sorted, capping a former O(n·displacement) worst case at O(n).
  A perf-cliff regression guard (`test/perf-guard.js`) locks it.
- **`findOutliers` no longer mutates its input.** It now sorts a copy, honoring
  the documented "input is not modified" contract; outlier indices were already
  correct (they map through original positions).
- **`argSort` rebuilt on an index-carrying radix** (no value→index map), with
  stability, `-0`/`+0` unification, deterministic NaN-last ordering, and input
  immutability — verified by a dedicated property section.

Tests: property suite (1,650 checks) across generators exercising every dispatch
branch including near-sorted, segmented, duplicate-heavy, shared-prefix, and
non-Latin-1 strings (gallop and unicode paths verified by mutation testing); a
660-check `argSort` section; `sortWithProxy` correctness; and perf-cliff guards.

### Engine details (carried from the 0.1.x development line)


**Fixed (important):**
- **Performance cliff on front-loaded near-sorted data.** When the disordered
  region of an otherwise-sorted array was concentrated near the *front*, a
  strided inversion sample plus a prefix-derived run-length estimate misjudged
  the array as locally jittery and ran momentum insertion sort, which is
  O(n·displacement) and went quadratic on a dense block (≈2.4s for a 1M float
  array 10%-shuffled at the head, vs ~40ms with the same disorder spread or at
  the tail). Momentum insertion now carries a displacement budget (~8n) and
  aborts to the distribution sort when exceeded, capping the worst case at O(n);
  the budget is free on genuine near-sorted input. Correctness was never
  affected, so the property suite did not catch it — a perf-cliff regression
  guard (test/perf-guard.js) now does.
- **Non-ASCII string corruption.** Strings containing characters beyond Latin-1
  (charcode > 255 — CJK, Cyrillic, Greek, macron vowels, etc.) could be silently
  *lost* during sorting: the byte-radix histogram discarded out-of-range
  typed-array writes, leaving `undefined` holes in the output. Ranges containing
  such characters now fall back to a UTF-16-correct comparison sort, detected by
  an OR-accumulator piggybacked on the existing counting pass (~zero cost on
  ASCII data). Affects all published versions through 0.1.2.
- `argSort` ReferenceError present in the published 0.1.2.

**Added:**
- `sortF64(Float64Array)` and `sortI32(Int32Array)` typed entry points (also as
  `sort.sortF64` / `sort.sortI32`): in-place typed sorting that skips the boxed
  representation entirely (mandatory at very large n; `sortI32` is ~1.4× the
  boxed radix path).

**Performance:**
- **Quantized-float routing (sortF64).** A sampled OR/AND probe over the
  IEEE-754 total-order transform predicts how many radix passes the data
  needs; quantized / low-mantissa-entropy data (dyadic fractions, sensor
  ticks) with ≤4 active key bytes routes to a new full float64 LSD radix
  (constant bytes skip their pass; original doubles are scattered, so the
  path is structurally NaN-safe — NaN keys sort last). Measured on dyadic
  data: +15% at 1M, +22% at 8M, ~even at 4M/16M (capped at n≤8M). Uniform
  and skewed data fall through unchanged (probe overhead unmeasurable).
- **argSort rebuilt on an index-carrying radix.** The previous design sorted a
  copy then rebuilt indices through a `Map` (n inserts + n lookups), which
  dominated the cost and could lose to a naive comparator at large unique n.
  The original index now rides through every radix scatter pass — int32 keys
  via 4 sign-flipped byte passes, all other numbers via 8 passes over the
  IEEE-754 total-order transform (carried as bit-exact uint32 word pairs) —
  with constant-byte pass skipping. Measured at n=1M (same-process): int32
  unique 18×, float unique 8.1×, power-law 7.3×, 100-distinct 3.4×. Stability
  is structural (LSD order), `-0`/`+0` form one tie group, NaN keys sort last
  deterministically, and inputs are never modified. `sortWithIndices` inherits
  the speedup. String keys keep the previous path. ~24 bytes/n pooled buffers,
  lazily grown.
- **Skew-aware float routing.** A sampled probe (~free) detects heavy-tailed
  numeric data and routes it to a bucket sort keyed on IEEE-754 bit patterns
  (~linear in log-magnitude), which balances power-law/log-normal inputs that
  collapse value-linear buckets. Applies to both `sortF64` and boxed `sort()`
  (which copies through a pooled typed buffer; the copy doubles as the NaN
  guard). Measured: 1.4× at 1M growing to 2.5–2.7× at 4–8M elements on
  power-law data; uniform data is unaffected (probe routes it to the existing
  value-keyed path).
- **Packed-prefix string radix.** Distinct-heavy string data (random keys, IDs,
  unique suffixes) packs 4 characters per element into one uint32 and
  radix-sorts integers instead of scattering per character; constant bytes skip
  their pass outright. Gated by a sampled distinct-ratio probe so
  duplicate-heavy data keeps the LCP path below. Measured at n=500k: random
  len-8 strings 1.6×, len-32 1.2× vs 0.1.3-dev; 5.1× / 3.2× vs native sort.
- **Per-level LCP skip (strings).** The MSD radix now advances through any
  character shared by every string in a range with count-only passes, scattering
  only at divergence — at every recursion level, not just the root. Few-distinct
  string workloads ~3.8× faster (now ~4–5× vs native sort); shared-prefix
  workloads (URLs, paths) also improve.
- **Galloping string run-merge.** The string merge now gallops (TimSort-style,
  with adaptive backoff): segmented near-sorted data — sorted blocks
  concatenated out of order, e.g. merged log partitions — measured 2.9× faster;
  scattered-perturbation and fully-sorted inputs unchanged. The numeric
  run-merge gallop is now also adaptive, with an eager start (threshold 2,
  backing off when gallops fail): +21% on sorted-with-scattered-swaps, +12%
  on shuffled sorted blocks at n=1M. (Gap-statistic-derived gallop routing was
  tested and rejected: endpoint gap/overlap stats predict density- and
  range-driven block structure but are blind to arrangement-driven blockiness
  — the case that matters — which the adaptive counter measures locally.)

**Tests:**
- Property suite grown 1290 → 1650 checks: near-sorted string generators
  (locking the gallop path — verified by mutation testing), and a mixed
  ASCII/non-Latin-1 generator (locking the UTF-16 fallback).
- New argSort property section (660 checks): permutation validity, input
  immutability, ordering, and stability verified against a reference stable
  argsort across 11 generators including NaN/Infinity, ±0, >32-bit integers,
  and both small-n/radix threshold edges. (It caught a NaN-handling bug in the
  small-n path during development.)

## 0.1.3 (not published to npm)
Adaptive sort with optional native C acceleration. The dispatcher inspects each
input in a single pass (min, max, integer-ness, ascending/descending) and routes
to the cheapest matching strategy:

- Integers: counting sort for narrow spans; LSD radix for wider int32 spans, with
  constant-byte skipping (a byte identical across all values contributes no
  ordering, so its scatter pass is skipped) and the bias copy-in / copy-out fused
  into the histogram and final scatter passes.
- Floats: flash sort for small, well-spread inputs; a coarse 2-level value-bucket
  sort for large inputs, with a multimodal gap check that splits clustered data
  first. The coarse path caches each element's bucket index during counting and
  reuses it during scatter.
- Near-sorted data: natural-run detection with a galloping 2-way / 4-way merge,
  plus a momentum insertion sort for local jitter.
- Strings: probe-gated run-merge for mostly-sorted input, otherwise a unified
  MSD-at-depth radix with a common-prefix skip.
- General fallback: dual-pivot introsort with a heapsort guard.

Auxiliary memory is 12 bytes/n on the numeric paths (a shared, reused buffer
pool); the comparison fallbacks sort in place. An optional native C addon
(N-API) accelerates the float64 and int32 paths.
