# reports/ — generated HTML that lives with the project

Both pages are outputs; regenerate them rather than editing them.

| file | what it is | regenerate with |
|---|---|---|
| `truth-table.html` | the styled rendering of `../TRUTH_TABLE.md` (every claim graded, the avenues, the theory-vs-optimum gaps) | `python tools/truth_table_html.py` |
| `profile-report.html` | the last `make profile` run: per-phase treemaps, per-thread stall bars, fair-fight bars on the same data, phase tables | `cd engines/c && make profile` (copies here) |
| `scale-report.html` | time against n, 1e4 to 1e7, every shape, one line chart per shape for the single-threaded sorts and one for the parallel sorts (std::sort / __gnu_parallel::sort, logos, x86-simd-sort, IPS⁴o); linear axes with a log-log toggle, dashed O(n) and O(n log n) references, fitted exponents | `cd engines/c && make scale-full` (or `make scalebench` without the opponents; copies here) |

The single-threaded sections and panels also carry two textbook baselines from
`engines/c/bench_sorts.h`: an LSD radix sort and a timsort.

Benchmarks default to `BENCH_THREADS` = 14, the reference box's physical core
count, because `make noiseprobe` showed that is where its minimum is lowest and
quietest; every contender runs at that same count.

The profile report carries its own calibration line at the top; if it says the
machine was loaded, read shares and within-run ratios, not absolute times. The
checked-in copy is `make profile-full` from a loaded 20-thread i9-13900H (AVX2,
no AVX-512) under WSL2 on 2026-09-10, after the J5 relocation, the H0b splitter
tables and the 12-bucket package, with x86-simd-sort's AVX2 kernels and IPS⁴o
beside every cell at the same thread count.

State on 2026-09-11, late: all three pages are current and were taken on a
quiet box (calibration lines: profile memcpy 3.2 ms, scaling 3.7 ms and
std::sort-1M 80.9 ms). The scaling report needed four attempts; the three
discarded ones hit load bursts of 5.2 to 11.5 ms against about 3 quiet. A
scaling run measures each size at a different moment, so load corrupts it
unevenly and its own calibration line cannot warn you per row: check the box
with `make noiseprobe` or the memcpy probe before regenerating it.

`engines/c/scaling-report.html` (from `make scaling`) is the third report; it is
written next to its benchmark and not copied here.

If the profile page ever shows headings but no charts, run
`node tools/check_report.js reports/profile-report.html`: it executes the page's
script under a stub DOM and prints what rendered, or the exception. `make
profile` runs it automatically when node is installed.
