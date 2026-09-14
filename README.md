# logos-sort

A distribution-aware adaptive numeric sort engine — in C (parallel flagship),
Rust, JavaScript, and Python — plus the benchmarks, experiments, and honest
findings that came from building it against the state of the art.

**If you are a Claude instance continuing this work, read [`CLAUDE.md`](CLAUDE.md)
first.** It carries the measurement methodology and the ledger of experiments
that were tried and failed, so you don't repeat them.

## The thesis, and where it actually stands

*Addressing beats comparing.* Detect the data's structure — cardinality,
sortedness, skew, quantization — and dispatch to a path that avoids comparison
(counting, radix, run-merge, interpolation bucketing).

That thesis is philosophically sound and empirically **narrow**. Measured on a
20-core machine, 8M doubles, every output verified:

| opponent | result |
|---|---|
| `qsort`, `std::sort` (scalar, 1T) | logos wins 2.8–12× |
| `__gnu_parallel::sort` (OpenMP, 20T) | logos wins every shape |
| **IPS⁴o** (SOTA parallel comparison sort, 20T) | logos wins every shape |
| **Intel x86-simd-sort** (AVX-512, 20T) | logos **loses** all unstructured shapes 2–3×; **wins** few-distinct and quantized (2.5×) |

So: logos beats every scalar and comparison-based sort, and is beaten by the
vectorized SIMD sort on random data — winning against it only where it avoids
*both* comparing and scattering. That structured-data niche is real and
defensible; the unstructured gap is, on a single-socket CPU, closed (see the
negative-results ledger in `CLAUDE.md` for the three restructurings that tried
and failed to change this).

The most promising direction is in [`experiments/paradigm/`](experiments/paradigm/):
when the deliverable is an **order or analytics query** rather than a sorted
array, logos's no-scatter model beats the SIMD sort by 7–87×. See
[`PARADIGM.md`](experiments/paradigm/PARADIGM.md).

## Layout

- `engines/c/` — **start here.** Engine, extras (argSort/permutations/outliers/
  strings), optional AVX-512 leaf, the fair-fight `parbench`, scaling ladder,
  685-check harness. `PARITY.md` = feature matrix across languages.
- `engines/rust/`, `engines/js/`, `engines/python/` — other implementations
  (Rust is behind C on numerics; JS has the most complete string engine).
- `experiments/paradigm/` — the "change the deliverable" experiments and the
  merge-sort / cache-scatter negative results, all with runnable code.
- `tools/sortbench/` — Python benchmarking library vs numpy / SQLite / DuckDB.
- `TRUTH_TABLE.md` — every claim graded, ranked avenues with predictions, and
  where the theory falls short of the optimal algorithm; rendered to
  `reports/truth-table.html` by `tools/truth_table_html.py`.
- `reports/` — generated HTML kept with the project: the truth table and the
  latest `make profile` report (per-phase treemaps, stalls, fair fight).
- `archive/` — single-file JS builds and early design docs.

## Quick start

```sh
cd engines/c
make test        # correctness: 685 checks must pass
make parbench    # fair fight vs std::sort / __gnu_parallel::sort (no deps)
```
Optional heavier opponents (`parbench-simd`, `parbench-full`) need
`intel/x86-simd-sort`, `ips4o/ips4o`, and `libtbb-dev`; see `CLAUDE.md`.

## Methodology in one breath

Verify every output before timing; read the calibration line and discard loaded
runs; compare variants only within one interleaved binary; never compare
1-thread to P-thread; A/B the *full sort*, never trust a microbenchmark alone;
predict before measuring; and record what failed. Every result in this repo was
produced that way, and the engine ships zero changes that failed their A/B.
