"""sortbench — benchmark in-memory sort engines against database ORDER BY.

Quick start
-----------
    import sortbench as sb

    result = sb.benchmark(n=1_000_000, dtype="int32", distribution="uniform")
    result.print_table()

    # plug in your own sort:
    import numpy as np
    result = sb.benchmark(1_000_000, extra={"mysort": lambda a: np.sort(a)})

    # compare across data shapes:
    for dist, res in sb.sweep(1_000_000).items():
        res.print_table()

What it measures
----------------
The sort kernel, fairly: each engine first puts the data into its native form
(reported separately as "ingest"), then the sort alone is timed — warm, median
of several runs — and the result is verified against a reference sort. Databases
materialize the sort in-engine (``CREATE TABLE AS ... ORDER BY``) so no row
marshalling pollutes the number.

Contenders: numpy.sort, builtin sorted(), a bundled pure-Python LogosSort,
SQLite ORDER BY, and DuckDB ORDER BY (optional). Missing optional engines are
skipped, not fatal.
"""
from .core import benchmark, sweep, Result, Row
from .datasets import generate, DISTRIBUTIONS, DTYPES
from .contenders import list_contenders, build_contenders

__version__ = "0.1.0"
__all__ = [
    "benchmark", "sweep", "Result", "Row",
    "generate", "DISTRIBUTIONS", "DTYPES",
    "list_contenders", "build_contenders", "__version__",
]
