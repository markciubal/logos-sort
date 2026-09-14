"""Dataset generators for sort benchmarking.

The distributions are chosen to stress different sort strategies, so the same
benchmark reveals *where* each engine wins rather than a single average:

  uniform       baseline; favors radix / counting
  normal        clustered around a mean
  powerlaw      heavy right tail; collapses naive interpolation bucketing
  duplicates    small value range; favors counting
  few_distinct  few distinct values over a wide range (low cardinality)
  sorted        already ordered; favors O(n) early-exit
  reverse       reverse ordered; early-exit + reverse
  near_sorted   mostly ordered with local jitter; favors adaptive merges
"""
import numpy as np

DTYPES = ("int32", "float64")
DISTRIBUTIONS = (
    "uniform", "normal", "powerlaw", "duplicates",
    "few_distinct", "sorted", "reverse", "near_sorted",
)

_I32_MIN, _I32_MAX = -(2 ** 31), 2 ** 31 - 1


def _as_dtype(a, dtype):
    if dtype == "int32":
        return np.clip(a, _I32_MIN, _I32_MAX).astype(np.int32, copy=False)
    return a.astype(np.float64, copy=False)


def generate(n, dtype="int32", distribution="uniform", seed=7):
    """Return a numpy array of length ``n`` with the given dtype and shape.

    ``dtype`` is one of :data:`DTYPES`; ``distribution`` one of
    :data:`DISTRIBUTIONS`. Generation is deterministic in ``seed``.
    """
    if dtype not in DTYPES:
        raise ValueError(f"dtype must be one of {DTYPES}, got {dtype!r}")
    if distribution not in DISTRIBUTIONS:
        raise ValueError(f"distribution must be one of {DISTRIBUTIONS}, got {distribution!r}")
    if n < 0:
        raise ValueError("n must be non-negative")
    rng = np.random.default_rng(seed)

    if distribution == "uniform":
        if dtype == "int32":
            return rng.integers(_I32_MIN, _I32_MAX, n, dtype=np.int64).astype(np.int32)
        return rng.random(n) * 2e9 - 1e9
    if distribution == "normal":
        return _as_dtype(rng.normal(0.0, 1e6, n), dtype)
    if distribution == "powerlaw":
        return _as_dtype((rng.pareto(1.2, n) + 1.0) * 1000.0, dtype)
    if distribution == "duplicates":
        a = rng.integers(0, 256, n, dtype=np.int64)
        return a.astype(np.int32) if dtype == "int32" else a.astype(np.float64)
    if distribution == "few_distinct":
        k = max(2, n // 100000)
        pool = rng.integers(_I32_MIN, _I32_MAX, k, dtype=np.int64)
        a = pool[rng.integers(0, k, n)]
        return a.astype(np.int32) if dtype == "int32" else a.astype(np.float64)
    if distribution == "sorted":
        return np.sort(generate(n, dtype, "uniform", seed))
    if distribution == "reverse":
        return np.ascontiguousarray(generate(n, dtype, "sorted", seed)[::-1])
    if distribution == "near_sorted":
        a = generate(n, dtype, "sorted", seed)
        m = max(1, n // 100)
        i = rng.integers(0, n, m); j = rng.integers(0, n, m)
        tmp = a[i].copy(); a[i] = a[j]; a[j] = tmp
        return a
    raise AssertionError("unreachable")
