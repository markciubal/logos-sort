# logos-sort

A distribution-aware adaptive numeric sort engine — C (the parallel flagship), Rust, JavaScript and Python — with the
benchmarks, the experiments and the record of what was measured, including everything that failed.

**If you are a Claude instance continuing this work, read [`CLAUDE.md`](CLAUDE.md) first.** It carries the measurement
discipline, the ledger of ideas that were built and measured to fail, and the open directions. The narrative of every
round is [`FINDINGS.md`](FINDINGS.md); every claim's grade is [`TRUTH_TABLE.md`](TRUTH_TABLE.md); the queue of ideas is
[`experiments/IDEAS_LOG.md`](experiments/IDEAS_LOG.md); the commercial reading of all of it is [`bizdev/`](bizdev/).

## The thesis, and where it stands on 2026-11-06

*Addressing beats comparing.* Read the data's structure from one sample — cardinality, order, skew, quantization, runs,
how far anything is displaced — and dispatch to the cheapest route that fits: counting and lattice routes that place
every element by arithmetic, run routes that move sorted stretches whole and merge only where they cross, a near-sorted
route, and under all of them a parallel partition with an AVX2 quicksort leaf for data that has no structure at all.

Measured on the reference box (i9-13900H, 6 performance + 8 efficient cores, AVX2, GCC 13.3, WSL2; 14 benchmark threads;
every output verified) in [`reports/scale-report.html`](reports/scale-report.html) of 2026-11-05 and `make scoreboard`:

| against | one thread | fourteen threads |
|---|---|---|
| `std::sort` (scalar) | 7.2–8.6× on random doubles, 4–5.5× nearly sorted, 21–39× on few-distinct / quantized / block shapes | every row |
| `__gnu_parallel::sort` (OpenMP) | — | every row from 1M up |
| **IPS⁴o** (state-of-the-art parallel comparison sort, given 14 threads) | — | fastest on 13 of 13 numeric inputs at 3.16M and 10M (1.07–7.15×), 12 of 13 at 1M; **loses on 5–8 of 13 at 100k–316k**, where the thread policy runs one thread |
| **Intel x86-simd-sort** (AVX2 vectorized quicksort) | **level on random doubles** (0.99–1.05 on uniform / heavy_tail / bimodal / clustered; int_wide 0.86 behind); ahead wherever there is structure: near_sorted 3.65×, quantized 2.42×, few_distinct 1.45× (10M); sorted 35×, shuffled sorted blocks 8× (8.4M) | ahead on every numeric input at 10M (the random shapes 1.07–1.78×) |
| **Google VQSort** (Highway 1.4.0, AVX2; one thread only) | 1.14–10× ahead on all 17 inputs; VQSort is 1.36–1.42× behind x86-simd-sort here and reads no order | — |
| timsort (the best rival on sorted blocks) | shuffled sorted blocks 2.6–3.6× at 10M | — |
| strings, against `std::sort` with `strcmp` | 2.4–24× (log lines 4.7×, uuids 12×) | 1.7–6× against `__gnu_parallel::sort` |
| strings, against the same 8-byte-prefix scheme on x86-simd-sort's key-value sort | log lines 2.1× at less scratch, urls 1.4×, uuids 0.96, **words 0.84** | |

At 10M and one thread logos is the fastest sort measured on 10 of 17 inputs, level on 5, slower on 2 (int_wide and
strings: words); at 14 threads on 17 of 17. Memory is part of the picture: the partition path asks for ~16 bytes per
element at one thread (and touches under 0.1 of them), the counting routes under 1, the block routes 9, strings 24 per
string; x86-simd-sort asks 0, IPS⁴o 3 at 10M and 30 at 1M. Ratios are read within a row: the box runs loaded inside a
long benchmark, and barrier-heavy rivals lose more to that than logos does, so the 14-thread ratios are upper bounds.
The September 2026 numbers on a 20-core AVX-512 box (logos 2–3× behind x86-simd-sort on unstructured data) are in
FINDINGS §1–2; that box has not been re-run.

**The honest one-liner:** on random doubles the engine is level with the best vector sort on one thread and ahead with
threads; it wins by multiples only where the data carries structure it can read — and that turned out to be most of the
shapes real data takes: sorted batches appended out of order, timestamps with jitter, quantized values, few distinct
keys, newest-first logs. Where the deliverable is an **order or a query** rather than a sorted array
([`experiments/paradigm/`](experiments/paradigm/), the lazy view `engines/c/logos_view.h`) the no-scatter model wins by
larger multiples; that remains the project's most promising direction ([`PARADIGM.md`](experiments/paradigm/PARADIGM.md))
and still has no fair `querybench`.

## What the C engine is

About 11,000 lines in one translation unit, libc + pthreads + libm, built so far only with GCC 13 on x86-64 Linux. One
shared sample on Weyl positions answers every gate. The routes, cheapest first: a thin front for calls of up to 2047
elements; a vector prescan (sorted, reversed, a sorted prefix + tail, a head + sorted suffix, a ring buffer read from the
wrong slot); the counting routes (dense integers by bitmap, the lattice with a direct index and an entry for lattices
whose sample shows no repeats, a verified perfect hash for few values, a hash for moderate cardinality, a dominant value
filled in around the rest); the short-reach route (index blocks sorted where they lie, seams repaired); the run routes
(relocation and the knot route, descending runs consumed as they lie, kinks swapped back, strays lifted, the mirror,
interleaved streams, the wave, the cuts, runs of equal values); the near-sorted parallel route behind the kink and strand
gates; and the histogram-free partition with uniformity certificates, a key grid for heavy tails, a mode grid, splitter
tables chosen by trial, 12 buckets per thread and a persistent worker pool, over a leaf that is an in-house AVX2
quicksort (within 1.07–1.12× of x86-simd-sort's kernel) with flash sort where local order survives. Also: C strings on
8-byte prefix keys through the same machinery, a batch entry for many arrays, stable argsort and permutations, a lazy
sorted view, strict in-place variants, and a NaN entry with a defined order. Every route was A/B'd into place in one
binary; the engine ships zero changes that failed their A/B, and the ones that did are the ledger in `CLAUDE.md`.

## Layout

- [`engines/c/`](engines/c/) — **start here.** `logos_psort.c` (the engine), `logos_extra.c`, `logos_view.h`, the
  harness (54,832 checks in 48 sections on 2026-11-06), a Makefile of 129 targets: the sanitizers, the fault probe, the
  scoreboard, the scale report and about a hundred paired benches and probes, each the A/B of a change that shipped or
  did not. Its `README.md` has the API and how the dispatcher decides; `CHANGELOG.md` every change with its measurement;
  `PARITY.md` the matrix across languages.
- [`engines/rust/`](engines/rust/), [`engines/js/`](engines/js/), [`engines/python/`](engines/python/) — the other
  implementations: Rust is the v2 engine (behind C on numerics); JS is the original (npm `logos-sort`), single-threaded,
  with its own string engine and `sortWithProxy`; Python is the sequential reference.
- `engines/x86-simd-sort/`, `engines/ips4o/`, `engines/highway/` — the opponents, cloned beside the engine and ignored
  by git (`make scoreboard`, `make scale-full`, `make parbench-full` need them; `CLAUDE.md` has the clone lines).
- [`experiments/`](experiments/) — [`paradigm/`](experiments/paradigm/) (the "change the deliverable" experiments and
  the merge / scatter negatives), the round documents with their written predictions (`ORDER_FROM_CHAOS.md`,
  `OUTSIDE_THE_BOX.md`, `SHORT_REACH.md`, `TWIST_AND_KNOT.md`, `HARD_UNKNOTS.md`, `OTHER_FIELDS.md`), `moebius_dll/`,
  `team/`, and [`IDEAS_LOG.md`](experiments/IDEAS_LOG.md), the intake queue ranked by measured upside.
- [`bizdev/`](bizdev/) — the business-development assessor's folder: the state in commercial terms, the claims graded
  green / amber / red from the report's own data, a buyer map, the engineering gaps ranked by commercial value, the IP
  questions, a paper plan.
- [`tools/`](tools/) — `sortbench/` (Python: numpy / sorted / SQLite / DuckDB against logos), `truth_table_html.py`,
  `check_report.js` and `browser_check.py` (the report checkers; they run from Windows, WSL has no node or Chrome),
  `ledger_rerun.py`.
- [`reports/`](reports/) — generated HTML kept with the project, one self-contained file each: `scale-report.html` (the
  fair fight against n, written for a reader outside the project), `profile-report.html`, `truth-table.html`.
- [`TRUTH_TABLE.md`](TRUTH_TABLE.md) — every claim graded; its section S re-grades the September claims as of 2026-11-06.
- [`archive/`](archive/) — single-file JS builds and early design docs.

## Quick start

```sh
cd engines/c
make test            # 54,832 correctness checks must pass (about 3.5 minutes); exit code != 0 on any failure
make parbench        # the fair fight vs std::sort / __gnu_parallel::sort, no external deps
make scoreboard      # who is fastest: 17 shapes, 1 and 14 threads (needs the three opponent clones)
make scale-full      # time against n, 17 inputs, 1 and 14 threads -> ../../reports/scale-report.html
```

The toolchain is GCC + make, g++ with OpenMP and libtbb for the opponents; on Windows that means WSL2. The report
checkers (`node tools/check_report.js`, `python tools/browser_check.py`) run from the Windows side.

## Methodology in one breath

Verify every output before timing (ascending under `<` AND the bit-pattern multiset); compare variants only inside one
binary, paired on the two halves of one array with the order alternated, and swap which copy holds which variant for any
claim under 1.25× (code layout alone moves a copy 1.24×); never compare one thread to P; A/B the full sort, never trust
a microbenchmark; predict in writing before measuring and grade the prediction; run what has never run (fail every
allocation and thread creation; feed infinities, subnormals, NaNs, sentinels); let every test name the path it expects;
and record what failed. Every result in this repository was produced that way.

## State of the code

`LOGOS_PSORT_VERSION` is 3.6.0, and about thirty-five shipped, A/B'd changes sit above it in `engines/c/CHANGELOG.md` as
"(unreleased)"; cutting 3.7.0 is pending. Known limits a user meets first: one `logos_sort_*` call at a time per process
(the pools are globals), f64 and i32 only (no i64, no key-payload pair sort), not stable, GCC 13 / x86-64 Linux only, no
CI; `bizdev/GAPS.md` ranks them. License: `engines/c/LICENSE` and `engines/js/LICENSE` (the Logos Sort Ethical Source
License v1.1, not OSI-approved; `bizdev/IP.md` discusses it).
