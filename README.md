# logos-sort

Adaptive sort for JavaScript and C. Dispatches across counting sort, integer
radix, flash sort, dual-pivot introsort, multikey quicksort, MSD radix at
depth, and heapsort fallback based on input shape detected from a single
cheap probe.

## Installation

```bash
npm install logos-sort
```

## Usage

```js
const { sort, sortInplace, argSort, findOutliers } = require('logos-sort');

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
const { applyPermutation } = require('logos-sort');
const names  = ['c', 'a', 'd', 'a', 'e'];
const scores = [3,   1,   4,   1,   5  ];
const p      = argSort(scores);
applyPermutation(names, p);
// → ['a', 'a', 'c', 'd', 'e']

// Anomaly detection via sort resistance
const { findOutliers } = require('logos-sort');
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

Each sort path is chosen by a cheap entry probe (asc/desc/equal counts, run
detection, type/range checks). For numbers, the dispatcher routes among:

* counting sort — small bounded integer ranges
* int32 radix — wide integer keys, no negatives in bad mix
* flash sort — uniform/normal continuous numerics
* dual-pivot introsort with heapsort fallback — skewed distributions
* TimSort-style run merging — when existing ordered runs are detected
* insertion sort — small ranges, and final cleanup on bucket-style sorts

For strings: MSD-at-depth radix after common-prefix skip, with multikey QS
recursion and insertion sort on small ranges.

The companion `findOutliers` exploits *displacement during sort* as an
anomaly signal: well-behaved values move modestly, outliers move far. Works
on already-mostly-sorted streams of sensor data, transactions, log entries.

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

## C library

A reference C implementation of the core dispatcher pattern (flash sort +
introsort + insertion sort for `double`) lives in `c/`. See `c/README.md`.
The C library is a *subset* of the JS library — it ports the core numeric
path, not the string algorithms or all numeric specializations.

```bash
cd c && make && ./test
```

## License

**LogosAdaptive Sort Ethical Source License v1.1** — a source-available
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
