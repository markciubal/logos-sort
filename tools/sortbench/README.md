# sortbench

Benchmark in-memory sort engines against database `ORDER BY` — **fairly** and
**distribution-aware**. It answers the question "should I sort this in a JS/Python
array, with numpy, or hand it to a database?" with measurements instead of folklore.

It grew out of a concrete finding: on a single-core machine, a good in-memory
sort *beats* DuckDB and SQLite by 3–24× for a plain `ORDER BY`, because a
database's real edge (multi-core parallelism, out-of-core data, indexes,
persistence) isn't exercised by a one-column in-memory sort. sortbench makes that
comparison easy, honest, and repeatable — and makes it just as easy to see the
cases where the database *should* win.

## Install

```sh
pip install sortbench            # numpy only
pip install sortbench[duckdb]    # add the DuckDB contender
```

SQLite is built into Python; DuckDB is optional (skipped if absent).

## Usage

```python
import sortbench as sb

# one dataset, all contenders
sb.benchmark(n=10_000_000, dtype="int32", distribution="uniform").print_table()

# plug in your own sort (any callable: np.ndarray -> sorted np.ndarray)
import numpy as np
sb.benchmark(1_000_000, extra={"mysort": lambda a: np.sort(a)}).print_table()

# compare across data shapes at once
for dist, result in sb.sweep(1_000_000, dtype="int32").items():
    result.print_table()
```

Example output:

```
sortbench  n=10,000,000  dtype=int32  distribution=uniform  repeats=5  cores=1
  engine                  sort    vs best    ingest      kind   ok
  numpy.sort            71.4ms       1.0x         -    memory   ok
  logos-sort (yours)   239.0ms       3.3x         -    custom   ok
  DuckDB ORDER BY     1255.2ms      17.6x      20ms  database   ok
  SQLite ORDER BY     5661.2ms      79.3x    8385ms  database   ok
  note: single core: DuckDB cannot parallelize ORDER BY here ...
```

## What it measures, and why it's fair

The hard part of this comparison is not running it — it's running it *fairly*.
sortbench bakes in the methodology:

- **Sort only.** Each engine first converts the data to its native form
  (numpy array, Python list, or a database table). That cost is reported in a
  separate **ingest** column and is *not* part of the sort time. Only the sort
  on already-resident data is timed.
- **In-engine materialization.** Databases sort with
  `CREATE TABLE s AS SELECT x FROM t ORDER BY x`, so the sort is measured inside
  the engine without Python row-marshalling polluting the number.
- **Warm + median.** A warmup run precedes several timed runs; the median is
  reported (container CPUs are noisy).
- **Verified.** Every result is checked against a reference sort (untimed). A
  fast wrong answer is marked `FAIL`, not celebrated.
- **Honest about the machine.** The core count is detected and printed; on a
  single core a note spells out that the databases' parallelism advantage is
  absent, so you don't over-read the result.

## Distributions

The same engines rank differently on different data, so a single "uniform random"
number hides the real story. `distribution=` accepts:

`uniform`, `normal`, `powerlaw` (heavy tail), `duplicates` (small range),
`few_distinct` (low cardinality over a wide range), `sorted`, `reverse`,
`near_sorted`. Generate raw data directly with `sortbench.generate(n, dtype, distribution)`.

## Contenders

| key | engine | notes |
|---|---|---|
| `numpy` | `numpy.sort` | C kernel (radix for ints); the practical ceiling |
| `builtin` | `sorted()` | CPython Timsort |
| `logos` | bundled pure-Python LogosSort | slow at scale by design; competitive only on duplicate-heavy ints |
| `sqlite` | SQLite `ORDER BY` | in-memory row store |
| `duckdb` | DuckDB `ORDER BY` | columnar OLAP; set `duckdb_threads=` to study scaling |

Add your own with `extra={"name": callable}` — ideal for wiring in a C
extension, a Rust binding, or a subprocess call to a sort in another language.

## API

- `benchmark(n, dtype, distribution, contenders, repeats, warmup, seed, verify, duckdb_threads, extra)` → `Result`
- `sweep(n, dtype, distributions, **kwargs)` → `{distribution: Result}`
- `generate(n, dtype, distribution, seed)` → `numpy.ndarray`
- `list_contenders()` → `[(key, name, kind, available)]`
- `Result`: `.print_table()`, `.format_table()`, `.ranked()`, `.fastest()`, `.to_dict()`, `.rows`, `.notes`

## License

MIT.
