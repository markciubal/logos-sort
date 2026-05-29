# logos-sort

Adaptive sort for JavaScript. Dispatches across counting sort, integer
radix, flash sort, dual-pivot introsort, multikey quicksort, MSD radix at
depth, and heapsort fallback based on input shape detected from a single
cheap probe.

## Installation

```bash
npm install logos-sort
```

## Usage

ES modules (TypeScript, bundlers, `"type": "module"` in Node 22.12+):

```js
import { sort, sortInplace, argSort, applyPermutation, findOutliers } from 'logos-sort';
```

CommonJS:

```js
const { sort, sortInplace, argSort, applyPermutation, findOutliers } = require('logos-sort');
```

Default import also works — it resolves to `sort`:

```js
import sort from 'logos-sort';
```

Examples:

```js
// Numbers and strings, ascending order
sort([3, 1, 4, 1, 5, 9, 2, 6]);
// → [1, 1, 2, 3, 4, 5, 6, 9]

sort(['banana', 'apple', 'cherry']);
// → ['apple', 'banana', 'cherry']

// In-place with minimal scratch memory
sortInplace(largeArray);

// Indirect sort: get the permutation without modifying input
const perm = argSort([3, 1, 4, 1, 5]);
// → Uint32Array [1, 3, 0, 2, 4]

// Apply the same permutation to a parallel array
const names  = ['c', 'a', 'd', 'a', 'e'];
const scores = [3,   1,   4,   1,   5  ];
const p      = argSort(scores);
applyPermutation(names, p);
// → ['a', 'a', 'c', 'd', 'e']

// Anomaly detection via sort resistance
findOutliers([10, 11, 12, 13, 9999, 14, 15, -50, 16]);
// → { sorted: [...], outliers: [4, 7], budget: 3 }
```

## API

### `sort(arr)` → `arr`

Sorts numbers or strings in ascending order. Mutates and returns the input.
Auto-detects element type from `arr[0]`. Uses an internal buffer pool for speed
(amortized over many calls). Call `sort.releaseBuffers()` to free if needed.

### `sortInplace(arr)` → `arr`

Same contract as `sort` but uses only ~3KB of pre-allocated scratch buffers
(no per-call allocation). Slightly slower on most workloads. Use when memory
is constrained or when allocating large auxiliary arrays is undesirable.

### `argSort(arr)` → `Uint32Array`

Returns a permutation `p` such that `applyPermutation(arr, p)` is sorted.
Does not modify the input.

### `sortWithIndices(arr)` → `{ sorted, indices }`

Convenience: returns both the sorted values and the permutation.

### `applyPermutation(arr, perm)` → `Array`

Returns a new array `out` where `out[i] = arr[perm[i]]`.

### `inversePermutation(perm)` → `Uint32Array`

Returns `inv` such that `inv[perm[i]] === i` for all `i`.

### `restoreOrder(sorted, perm)` → `Array`

Undoes a sort: given sorted values and the permutation from `argSort`,
returns the original (unsorted) ordering.

### `findOutliers(arr, options?)` → `{ sorted, outliers, budget }`

Detects elements that resist sorting — values that need to migrate further
than the budget to reach their final position. Returns indices in the
*original* (unsorted) input. Default budget is `max(3, sqrt(n))`; pass
`{ budget: N }` to override.

## Design

**Profile first, then sort.** For arrays of more than a few hundred
elements, the *shape* of the data dominates the constant factor of any
`O(n log n)` algorithm. A million random `int32`s and a million
32-character strings are both `n log n` under a comparison sort, but the
constants differ by an order of magnitude. logos-sort spends a few
microseconds measuring the input first, then dispatches to an algorithm
whose constants are tuned for that shape.

The entry probe scans a sub-sample and records:

- **type and bit-width** — integer vs float; range of integer keys
- **existing order** — ascending, descending, and equal-adjacent counts
- **value distribution** — uniform, skewed, or multimodal (decides flash
  sort vs introsort)
- **run length** — long pre-sorted runs trigger TimSort-style merging
  instead of a full re-sort

Probe cost scales sub-linearly in `n` and is bounded by a small constant
even at the largest sizes. It pays for itself many times over by avoiding
the wrong algorithm. For very small `n` (< ~32), the dispatcher
short-circuits straight to insertion sort without probing.

This is the introsort idea extended: instead of falling back to heapsort
only when quicksort recursion goes bad, *predict the right algorithm up
front*. Most real-world data is structured — sorted streams, bounded
integer IDs, sensor readings, log timestamps — and structured data is
where specialized paths beat any general-purpose comparison sort.

For numbers, the dispatcher routes among:

* counting sort — small bounded integer ranges
* int32 radix — wide integer keys, no negatives in bad mix
* flash sort — uniform / normal continuous numerics
* dual-pivot introsort with heapsort fallback — skewed distributions
* TimSort-style run merging — when existing ordered runs are detected
* insertion sort — small ranges, and final cleanup on bucket-style sorts

For strings: MSD-at-depth radix after common-prefix skip, with multikey QS
recursion and insertion sort on small ranges.

The companion `findOutliers` exploits *displacement during sort* as an
anomaly signal: well-behaved values move modestly, outliers move far.
Works on already-mostly-sorted streams of sensor data, transactions, log
entries.

## Benchmarks

All four sorts measured under identical conditions in a single
`npm run bench` invocation. Node v24, best-of-5 runs, single-threaded,
`n = 1,000,000` unless noted. Absolute timings vary with machine load;
the speedup ratios are the stable signal.

### vs V8 (TimSort)

| workload | `Array.prototype.sort` (TimSort) | `logosSort` | speedup |
|---|---:|---:|---:|
| random ints (range 2³⁰) | 1276 ms | 116 ms | **11×** |
| small-range ints (range 256) | 526 ms | 26 ms | **21×** |
| random floats | 1090 ms | 52 ms | **21×** |
| nearly sorted (1k swaps) | 62 ms | 20 ms | **3.2×** |
| reversed | 43 ms | 14 ms | **3.0×** |
| short strings (`n = 200k`) | 129 ms | 41 ms | **3.1×** |

### vs pdqsort (pattern-defeating quicksort)

pdqsort is the strongest general-purpose comparison sort and is the
default in the C++ and Rust standard libraries. The apples-to-apples
comparison from logos-sort is `sortInplace`, which uses the same
~constant-scratch memory model as pdqsort (no per-call buffer pool, ~3 KB
of pre-allocated scratch).

The pdqsort numbers below come from a **reference JS port** included in
`benchmark.js` (insertion sort for small ranges, median-of-three / ninther
pivot, Hoare partition, heapsort fallback on bad recursion depth). It
captures the algorithmic shape but omits block partitioning and full
pattern detection. Native C++/Rust pdqsort will be substantially faster in
absolute terms; the numbers below are for relative comparison only.

| workload | TimSort | `logosSortInplace` | pdqsort (JS) | inplace vs pdqsort |
|---|---:|---:|---:|---:|
| random ints (range 2³⁰) | 1276 ms | 333 ms | 400 ms | **1.20×** |
| small-range ints (range 256) | 526 ms | 95 ms | 140 ms | **1.47×** |
| random floats | 1090 ms | 181 ms | 184 ms | **1.02×** |
| nearly sorted (1k swaps) | 62 ms | 49 ms | 30 ms | 0.61× |
| reversed | 43 ms | 15 ms | 102 ms | **6.67×** |
| short strings (`n = 200k`) | 129 ms | 89 ms | 192 ms | **2.16×** |

`logosSortInplace` beats reference pdqsort on **5 of 6 workloads**. The
exception is nearly-sorted input, which is exactly where pdqsort's pattern
detection is engineered to dominate.

On workloads with no detectable structure (uniformly random `double`s, no
presortedness, no bounded range), the in-place path lands on dual-pivot
introsort — conceptually similar to pdqsort — and the random-floats row
above shows the expected near-parity (1.02×). Where logos-sort pulls ahead
is the same place any adaptive sort does: leaving the comparison-sort
regime entirely. No comparison sort, pdqsort included, can beat these
asymptotic floors:

| workload | comparison-sort lower bound | logos-sort path |
|---|---|---|
| bounded integer keys | Ω(n log n) | counting sort — **O(n + range)** |
| wide integer keys (32-bit) | Ω(n log n) | int32 radix — **O(n · k/8)** |
| uniform / normal floats | Ω(n log n) | flash sort — **O(n)** average |
| pre-sorted or partially-sorted | Ω(n) | run-merge — **O(n)** |
| many duplicates | Ω(n log n) | counting / bucket — **O(n)** |

For these inputs, the question isn't "how fast a comparison sort?" — it's
which non-comparison algorithm fits the shape. logos-sort picks one for
you, so application code doesn't have to know in advance.

Note: the buffered `sort` entry point trades ~16 bytes/element of
auxiliary memory for substantially higher throughput (see the V8 table
above). Prefer it unless your workload is memory-constrained.

## Property tests

A test harness in `test/` checks seven invariants across thousands of randomly
generated inputs:

1. Multiset preservation (output has same elements as input)
2. Monotonicity (output is ordered)
3. Idempotence (`sort(sort(x)) === sort(x)`)
4. Determinism (same input → same output)
5. Permutation invariance (`sort(shuffle(x)) === sort(x)`)
6. Merge consistency (`sort(a++b)` matches `merge(sort(a), sort(b))`)
7. Agreement with `Array.prototype.sort`

Run `npm test`.

## License

**Logos Sort Ethical Source License v1.1** — a source-available
license with two layers of restriction. See `LICENSE` for full text.

**Layer 1 — Prohibited Uses (absolute, no license available):**
military applications, mass surveillance, non-consensual human genetic
research, and systems that violate human rights as defined in the UN
Universal Declaration of Human Rights.

**Layer 2 — Dual licensing for commercial use:**
the default grant is free for everyone whose use falls into one of the
*Permitted Free Uses* (Section 4). Other commercial use requires a paid
commercial license (Section 3).

**Permitted Free Uses (no commercial license needed):**
- Personal and hobbyist use
- **Research use** — academic, scientific, or industrial research whose
  findings are intended to be made publicly available; applies to
  corporate R&D labs, not just universities
- Educational use at accredited institutions
- Non-profit use in charitable mission
- Internal evaluation (up to 90 days)
- Government use for openly-available public services
- Dependency of OSI-licensed open-source projects (downstream commercial
  use of the combined work still requires a commercial license)

**Commercial use** — incorporating into a sold/licensed product,
revenue-generating service, or paid government service — requires a
commercial license. Contact the author to negotiate terms.

**This is not an OSI-approved open source license.** If you require an
OSI-approved license, this library is not for you.
