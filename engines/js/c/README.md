# logos-sort (C)

Reference implementation of the LogosAdaptive dispatcher pattern for sorting
arrays of `double` in C99. Single source file, no dependencies beyond libc
and libm.

## Build

```sh
make            # builds liblogos_sort.a, liblogos_sort.so, and ./test
make test       # builds test only
make clean
make install    # installs to /usr/local (override PREFIX=...)
```

## Use

```c
#include <logos_sort.h>

double data[] = { 3.0, 1.0, 4.0, 1.5, 9.0, 2.7 };
size_t n = sizeof(data) / sizeof(data[0]);

if (ls_sort_double(data, n) != 0) {
    /* allocation failure — array unchanged */
}
```

Caller-managed scratch:

```c
double scratch[1024];
ls_sort_double_with_buffer(data, n, scratch, 1024);
```

Link with `-llogos_sort -lm`.

## API

```c
int  ls_sort_double(double *arr, size_t n);
int  ls_sort_double_with_buffer(double *arr, size_t n,
                                double *scratch, size_t scratch_size);
const char *ls_sort_version(void);
```

Returns 0 on success, nonzero on allocation failure (or buffer-too-small for
the `_with_buffer` variant).

## Algorithms

1. **Cheap entry probe** — single linear scan that detects already-sorted,
   reverse-sorted, all-equal, or mixed input. Sorted and all-equal return
   immediately; reverse-sorted is fixed in one O(n) reverse.
2. **Insertion sort** — for n ≤ 24 and final bucket cleanup.
3. **Flash sort (Neubert)** — bucket-distribution sort with `m = 0.43n`
   buckets, then in-place permutation, then insertion-sort cleanup within
   each bucket. The dominant path for uniform/normal distributions.
4. **Introsort (quicksort + heapsort fallback)** — taken when flash sort
   detects a catastrophic bucket imbalance (a single bucket exceeds 5% of n,
   indicating heavy skew). Quicksort with median-of-three pivot and recursion
   depth limit of `2·⌊log₂ n⌋`; falls to heapsort when the limit is hit.

## Scope vs the JS library

This C library implements only the **core numeric dispatcher**. Compared to
the JS library, the following are NOT ported:

- String sorting (MSD-at-depth radix, multikey QS, common-prefix skip)
- Counting sort for bounded integer ranges
- Int32 / Float64 radix passes
- TimSort-style run detection and 4-way merge
- Multimodal gap-partition for bimodal distributions
- argSort, findOutliers, applyPermutation

If you need any of those, the JS library covers them. Porting them to C is a
larger project — the dispatcher pattern shown here is the foundation.

## Performance

On uniform `double` arrays, vs glibc's `qsort`:

| n          | ls_sort   | qsort    | speedup |
|------------|-----------|----------|---------|
| 1,000      | 0.01 ms   | 0.06 ms  | ~7×     |
| 10,000     | 0.15 ms   | 0.90 ms  | ~6×     |
| 100,000    | 2.33 ms   | 11.50 ms | ~5×     |
| 1,000,000  | 59.24 ms  | 142.56 ms| ~2.4×   |

Speedups come from: (1) avoiding `qsort`'s function-pointer dispatch on every
comparison, (2) flash sort's O(n) bucketing vs qsort's O(n log n), (3) cache-
friendly cleanup with insertion sort on small buckets.

## NaN, Inf, signed zero

Behavior with NaN is **undefined** (the comparison `a < b` is always false
when either operand is NaN, leading to silently incorrect results — same as
`qsort`). Inf values sort correctly. Signed zero is not distinguished
from unsigned zero.

## Tests

```sh
make && ./test
```

Runs 54 correctness checks (6 distributions × 9 sizes from n=0 to n=100k)
plus a benchmark vs `qsort`.

## License

LogosAdaptive Sort Ethical Source License v1.1 (same as the JS library).
See the top-level `LICENSE` file. This is a dual-licensed source-available
license: free for personal use, research (academic and industrial),
education, non-profit use, evaluation, and open-source dependency use;
commercial use requires a paid commercial license. Prohibited uses
(military, surveillance, non-consensual human genetic research,
human-rights violations) are absolute and not subject to commercial
licensing. NOT an OSI-approved open source license.
