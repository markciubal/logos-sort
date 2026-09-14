# logos-sort

> In the beginning was the Word.

An adaptive, **distribution-aware** sort for JavaScript. Instead of running one
algorithm for everything, it takes a cheap profile of the data — a single free
pass plus a few thousand sampled reads — and routes to the strategy that fits.
It sorts numbers *numerically* (no lexicographic coercion), beats the built-in
`Array.prototype.sort()` across most shapes, and adds tools the built-in lacks:
indirect sort (`argSort`), typed-array fast paths, and anomaly detection.

```js
const { sort, argSort, sortF64 } = require('logos-sort');

sort([5, 2, 9, 1, 5, 6]);                 // => [1, 2, 5, 5, 6, 9]
sort(['banana', 'apple', 'cherry']);      // => ['apple', 'banana', 'cherry']
argSort([30, 10, 20]);                     // => Uint32Array [1, 2, 0]
sortF64(new Float64Array([3.5, 1.5, 2.5]));// typed-array fast path
```

## Install

```sh
npm install logos-sort
```

Pure JavaScript; works out of the box on Node ≥ 14, no build step. Optional
native C acceleration is available via `npm run build` (see below) but is never
required — the library falls back to the JS engine automatically.

## Why it's fast

Sorting at scale is bound by **memory traffic**, not arithmetic — the cost is the
scatter, writing each element to its sorted home in cache-missing order. Two ideas
drive the wins:

- **Addressing beats comparing.** A comparison extracts one bit of order per
  operation (the `n log n` wall); using a key's bits as an array index (counting
  / radix) is `O(n·w)` and sidesteps that wall entirely. The engine reaches for
  an addressing sort whenever the keys are fixed-width.
- **Measure cheaply, then commit.** Strategy is chosen by statistics that are
  free (piggybacked on a scan already happening) or sampled (a few thousand
  strided reads). A probe picks a *whole algorithm*; nothing adapts per-element
  in a hot loop.

On the development machine (single core), representative results: large numeric
arrays sort ~3.5–4.5× faster than `Array.prototype.sort()`; strings ~2–5×
depending on shape; heavy-tailed `Float64Array` up to ~2.5× faster than the
engine's own generic path; `argSort` 8–18× faster than a sort-then-rebuild
approach. Your numbers will differ — benchmark on your data.

## API

### `sort(arr)` → `arr`

Sorts an array of all-numbers or all-strings ascending, in place, and returns the
same reference. Element type is detected from `arr[0]`. Uses a reusable internal
buffer pool (amortized across calls); call `sort.releaseBuffers()` to free it.

### `sortInplace(arr)` → `arr`

Same contract, but uses only a few KB of fixed scratch instead of a per-call
pool. Slightly slower on most workloads; for memory-constrained contexts.

### `sortF64(arr)` → `arr`  ·  `sortI32(arr)` → `arr`

Typed-array fast paths (also `sort.sortF64` / `sort.sortI32`). `sortF64` sorts a
`Float64Array` in place; for heavy-tailed and quantized data it routes to bucket-
on-exponent-bits and an 8-pass float radix respectively, gated by sampled probes.
`sortI32` sorts an `Int32Array`, skipping integer detection to go straight to
counting/radix. Typed reads avoid the boxed-`Array` tax and are the only viable
representation at very large n.

### `argSort(arr)` → `Uint32Array`

Returns a permutation `p` where `p[i]` is the original index of the element at
sorted position `i`; the input is **not** modified. Stable. For numbers it carries
the index through an LSD radix (no value→index map), making it dramatically faster
than the usual sort-a-copy-then-rebuild idiom.

```js
const idx = argSort([30, 10, 20]);        // Uint32Array [1, 2, 0]
// reorder a parallel array the same way:
const names = applyPermutation(['c', 'a', 'b'], idx); // ['a', 'b', 'c']
```

### `sortWithIndices(arr)` → `{ sorted, indices }`

Sort and also return the permutation used.

### `applyPermutation(arr, perm)` · `inversePermutation(perm)` · `restoreOrder(sorted, perm)`

Permutation utilities for re-painting parallel arrays and undoing a sort.

### `findOutliers(arr, { budget? })` → `{ sorted, outliers, budget }`

Detects anomalous values by their *resistance to sorting*: elements that must
travel further than `budget` (default `max(3, √n)`) to reach their sorted
position are flagged. `outliers` are indices into the original array.

### `sortWithProxy(items, proxyKey, oracleCmp)` → `{ sorted, oracleCalls, strategy }`

Two-tier sort for **expensive** comparisons (LLM-as-judge, human ranking, costly
assays): orders first by a cheap `proxyKey`, then refines with the expensive
`oracleCmp`, minimizing expensive calls. Output is always exactly ordered by
`oracleCmp`.

## How it dispatches

A single pre-scan (min, max, all-integers?, ascending?, descending?) feeds a
decision tree; each step returns if it fits or falls through. Numbers:

- **already sorted / reversed** → returned (or reversed) in `O(n)`.
- **integers, small span** → counting sort (`O(n + span)`, no comparisons).
- **int32** → 4-pass LSD radix; a free OR/AND accumulator skips any byte that
  never varies.
- **near-sorted** → run-merge with adaptive galloping (wins big on segmented
  data), or displacement-budgeted momentum insertion for local jitter.
- **large, multimodal** → gap-split partition (prevents flash/coarse `O(n²)` on
  tight clusters).
- **large, well-spread** → flash sort (small n) or coarse bucket sort (large n).
- **large, heavy-tailed** → bucket on exponent bits (`sortF64`), detected by a
  sampled skew probe.
- **everything else** → three-way introsort with a heapsort depth guard.

Strings use multikey/MSD radix with **per-level longest-common-prefix skip**
(collapses shared prefixes and duplicate runs), a **packed-prefix radix** for
distinct-heavy data (4 chars per `uint32`, gated by a sampled distinct-ratio),
and an **adaptive galloping merge** for near-sorted/segmented input. Non-Latin-1
characters fall back to a UTF-16-correct path automatically.

## Optional native acceleration

The package ships C sources and an N-API binding but does **not** compile on
install. To build the optional addon:

```sh
npm run build      # requires node-gyp + a C toolchain
```

When present, `sortNativeF64` / `sortNativeNumbers` become available and
`nativeAvailable` is `true`; otherwise the JS engine is used transparently.
Native only reliably wins for `Float64Array` at medium sizes — measure first.

## Tests

```sh
npm test           # property suite + perf-cliff guards
npm run test:proxy # sortWithProxy correctness
```

The property suite checks permutation validity, ordering, reference agreement,
and (for `argSort`) stability and input immutability across generators that
exercise every dispatch branch, plus performance-cliff regression guards.

## Notes & limits

- Pass an array that is **all numbers** or **all strings**; the path is chosen
  from the first element.
- **Ascending only**; reverse for descending.
- `sort`/`sortInplace` are **not stable** for equal elements; `argSort` **is**.
- Numbers compare numerically — unlike `Array.prototype.sort()`'s string coercion.

## License

Logos Sort Ethical Source License v1.1 — free for personal, research,
educational, non-profit, and open-source use; commercial use requires a paid
license. **Not** OSI-approved. See [LICENSE](LICENSE).
