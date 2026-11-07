# reports/ — generated HTML that lives with the project

The three pages are outputs; regenerate them rather than editing them. Each is ONE self-contained file (uPlot is
vendored under `engines/c/vendor/` and inlined), so it can be sent as it is.

| file | what it is | state | regenerate with |
|---|---|---|---|
| `scale-report.html` | the fair fight against n: time and ns per element for every contender on 13 numeric and 4 string inputs, 1 and 14 threads, n = 0 to 10M; a memory circle per point (peak heap of one cold call, bytes per element); the batch/single seam at 10k marked and measured; an accordion that shows each input with demo data; charts that zoom and pan. Since 2026-11-05 it opens for a reader outside the project: what logos is, "At the largest size" (logos against the fastest other sort per input, computed in the page from DATA), the contenders with the instruction set each ran, the method, then the charts and how to reproduce. Contenders: `std::sort` / `__gnu_parallel::sort`, x86-simd-sort (AVX2; its OpenMP sort at 14 threads), Google's VQSort (Highway 1.4.0, AVX2, one thread only), IPS⁴o (given 14 threads), an LSD radix, timsort, logos and logos in-place | measured 2026-11-05 21:47 UTC (`META.measuredAt`); i9-13900H, GCC 13.3.0, 14 threads; calibration memcpy-64MB 4.9 ms / std::sort-1M 116 ms, the box's usual loaded state, so read ratios within a row. At 10M logos is the fastest sort measured on 10 of 17 inputs at one thread and on 17 of 17 at 14 threads (FINDINGS 3bt). Published as a private claude.ai artifact: the same file without its document wrapper | `cd engines/c && make scale-full` (about 3 minutes at the 10M ceiling; `LOGOS_MAX_N=5e7` restores the old ceiling; `LOGOS_SCALE_REUSE=<report> ./scalebench` re-renders measured data through the current template without measuring) |
| `profile-report.html` | the last `make profile-full` run: per-phase treemaps, per-thread stall bars (CPU received, preempted, barrier-stalled, idle), the leaf path per bucket, and fair-fight bars with x86-simd-sort and IPS⁴o on the same data | last regenerated 2026-10-07; its own header carries the calibration line of that run | `cd engines/c && make profile` (or `make profile-full` with the opponents; copies here) |
| `truth-table.html` | the styled rendering of `../TRUTH_TABLE.md`: every claim graded, the avenues, the theory-vs-optimum gaps, and section S, the 2026-11-06 re-grading of the September claims | regenerated 2026-11-06 | `python tools/truth_table_html.py` (from anywhere) |

Also here, not generated: `team-findings.md` (the 2026-09-12 overnight round of seven agents in isolated copies of the
tree; the write-ups are in `experiments/team/`), and `playground/` with the `typesafe-*.json` files (2026-09-23): a
paste kit, the project's state as text with the truth table's claims turned into batches of structured questions and
an answer key, built to evaluate a question-answering tool against the project's own record. None of them is an input
to the build.

## Reading the pages

Benchmarks default to `BENCH_THREADS` = 14, the reference box's physical core count, because `make noiseprobe` showed
that is where its minimum is lowest and quietest; every contender runs at that same count, and since 2026-11-05 every
parallel opponent is GIVEN that count (IPS⁴o had been taking all 20 hardware threads in earlier scale reports, so their
IPS⁴o P-thread line carries that caveat). Memory in the scale report is bytes ASKED FOR (peak traced heap); resident
memory is far lower on the one-thread AVX2 path (`make rssprobe`). IPS⁴o's memory column is size-dependent (a fixed
~30 MB buffer: 300 bytes per element at 100k, 30 at 1M, 3 at 10M). One known defect: the x86-simd-sort key-value series'
memory column on `strings: urls` reads 1.8e14 bytes per element (a tracer defect); VQSort's key-value column is the
comparison to use there. The 14-thread ratios against barrier-heavy rivals (IPS⁴o, `__gnu_parallel::sort`) are upper
bounds until a quiet re-run: they lose more to a loaded box than logos does, and `scalebench` does not yet set
`OMP_WAIT_POLICY=PASSIVE` the way `scoreboard` does.

A scaling run measures each size at a different moment, so load corrupts it unevenly and its own calibration line
cannot warn you per row: check the box with `make noiseprobe` before regenerating, and read the calibration line first.
A profile report's calibration line says whether the machine was loaded; if it was, read shares and within-run ratios,
not absolute times.

`engines/c/scaling-report.html` (from `make scaling`) is a fourth report; it is written next to its benchmark and not
copied here.

## After changing a report's template

Re-render without measuring (`LOGOS_SCALE_REUSE=<report.html> ./scalebench`) and run BOTH checkers from Windows (WSL
has no node and no Chrome): `node tools/check_report.js <report>` executes the page's script under a stub DOM with a
validating uPlot stub that DRIVES every chart's zoom and pan (it rounds a log window the way uPlot does and refuses a
log axis without its own ticks), and `python tools/browser_check.py <report>` drives the first panel in real headless
Chrome / Edge with synthetic wheel, key, mouse and touch events in six modes (`--shot out.png` for a screenshot). The
second found two defects in uPlot's log axes that the first had passed, and a screenshot once caught a flattened chart
both had passed (engines/c/CHANGELOG.md). `make profile` and `make scalebench` run the stub checker automatically when
node is installed. If a page shows headings but no charts, the stub checker prints what rendered, or the exception.
