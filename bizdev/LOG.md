# LOG: dated entries in commercial terms

## 2026-11-06 — Phase 0 discovery; first grading

**Engine state read:** working tree at the 2026-11-05 scale report (`META.measuredAt` 1793944050000; 13 numeric and
4 string shapes; 1 and 14 threads; contenders std::sort, x86-simd-sort AVX2, IPS4o, LSD radix, timsort, VQSort
AVX2 1.4.0, gnu_parallel, logos in-place). Four commits in git; about two months of engine work uncommitted.

**What the data says, in commercial terms.**
- At one thread on random numeric keys, the data most buyers sort, logos is level with or behind x86-simd-sort
  (uniform 0.91x at 1M, 1.02x at 10M; heavy_tail 0.88x; int_wide 0.86x; sine_slope 0.54x at 1M) while asking for
  16 B/el of scratch against 0. That is the row a buyer reads first, and it is not a reason to switch.
- The structured one-thread wins are real and quantified: near_sorted 3.4-3.7x at 0.4 B/el, quantized 2.3-2.4x at
  0-0.2 B/el, few_distinct 1.2-1.5x at 0, block shapes 2.1-3.6x over timsort at 15.6 vs 3 B/el, log lines 2.1x
  at less scratch than the rival. These describe timestamps, dimension columns, prices, log files: columns an
  analytics engine does sort, but a minority of its sorts.
- With 14 threads logos is the fastest sort in the report on all 13 numeric shapes from 3.16M elements up, 12 of
  13 at 1M, and loses to IPS4o on 5-8 of 13 shapes at 100k-316k. Buyers' blocks are 100k-1M and are sorted by
  one thread each, concurrently; the engine cannot be called concurrently today (globals). So the parallel
  result does not yet describe the deployment pattern.
- The strict in-place variant is 2.7-32x behind the best rival; it is a fallback, not a product.
- Memory in the report is bytes asked for, not resident; the one-thread AVX2 path touches under 0.1 B/el. The
  gap between those two figures is a free improvement in how the engine looks (GAPS 2).

**Corrections to the brief's seeds:** IPS4o's memory is 3 B/el only at 10M (30 at 1M, 300 at 100k: a fixed
buffer); the in-place variant's deficit is 2.7-32x, not 4-12x; quantized's memory is 0.0 at 10M and 0.2 at 1M.
Everything else confirmed.

**Report defects found:** `strings: urls` x86-simd-sort kv memory column reads 1.8e14 B/el (tracer defect);
scalebench runs OpenMP contenders without `OMP_WAIT_POLICY=PASSIVE` while scoreboard sets it; the calibration
line (memcpy 4.9 ms, std::sort-1M 116 ms) is the loaded state, so 14-thread ratios against barrier-heavy rivals
are upper bounds.

**Repository facts that change the commercial picture more than any benchmark:** a custom non-OSI "Ethical
Source" license with a commercial-license requirement and a placeholder contact; the GitHub remote is not
publicly readable (404), so the only public disclosure is the npm JS package of 2026-05-28 to 2026-06-23; one
author; GCC 13 on x86-64 Linux only; no CI; no fuzzing; numeric sorts unstable; AI co-authorship on the public
record (`package.json`).

**Claims:** first grading. 15 green (single machine, provisional), 9 amber, 9 red; a do-not-claim list of ten,
headed by "2x faster than SIMD sorts on random data".

**Ranking (GAPS.md):** 0 license decision; 1 reentrancy; 2 allocate only on a predicted win; 3 memory-budget API;
4 key plus payload; 5 int64 / (ptr,len) strings / stability; 6 the 100k-1M parallel regime; 7 harness fixes and new
rivals; 8 fuzzing and a worst-case bound; 9 ARM; 10 CI and compilers; 11 reproduction kit; 12 query API; 13 strings
at less scratch; 14 bindings. Reason for the order: no buyer can take the code while 0 and 1 stand; 2-3 convert
the memory story; 4-5 make the wins describe a buyer's data.

**Files written:** 00-STATE.md, CLAIMS.md, BUYERS.md, GAPS.md, IP.md, PAPER.md, QUESTIONS.md,
PITCH-duckdb.md (marked do-not-send: license), tools/parse_report.py.

**Needs a human decision:** the license (IP.md Section 2) and the patent question (Section 3) before anything is
made public; one hour of counsel on the AI-authorship question; whether a second machine can be had.
