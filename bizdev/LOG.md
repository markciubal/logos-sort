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

## 2026-11-06, later — the strand gate lands; the tree is committed; the project's documents are refreshed

**Engine change:** the near-sorted parallel route's strand gate (FINDINGS 3bw, `make strandgate`). At more than one
thread the route had been claiming sorted runs that lie IN ORDER and overlap their neighbours (log batches appended
as they close; a few neighbours swapped or late), sorting a tenth of the array as strays and running 1.6-3.9x slower
than the engine with the route off. A gate that tells strands from strays by scale now declines them: 1.7-3.9x at 14
threads on six such inputs at 8.4M, 1.6-3.3x at 1M, 2.1-5.3x at 2-4 threads; 21 guard inputs 0.95-1.07.

**Effect on the claims:** none change grade. No scale-report input has that shape (its near_sorted is random swaps;
its block inputs are shuffled, which the relocation and knot routes take), and the report's own inputs were among the
guards (roundbench 0.98-1.05, tsbench 0.97-1.05). It becomes a green candidate only if an "in-order batches" input is
added to the report (harness work of the GAPS 7 kind), and it is a buyer-shaped input: log segments concatenated in
arrival order, sorted on many threads.

**Repository facts changed:** everything is committed (5 commits; the last, 2026-11-06, carries FINDINGS 3bd-3bw, the
probes and this folder; 00-STATE §4 updated). The README, PARITY.md, TRUTH_TABLE.md (a section S re-grading the
September claims), the reports index and the C engine's README were brought current the same day. The version string
stays 3.6.0 with every change since September listed in CHANGELOG as "(unreleased)"; a 3.7.0 cut is the author's call.

## 2026-11-08 — the ladder to 316M, and the one-page index

**Asked:** "Create an index.html with a cleaned up version of scale-report. also run up to 1b elements." The bench was run
to 316M elements (strings and the memory pass to 100M) on a quiet box — calibration 3.23 ms / 66.8 ms, the first run whose
14-thread ratios are not upper bounds. 1e9 did not fit WSL2's 31 GB cap (the bench holds three arrays of n plus the sort's
scratch, ~40 GB at 1e9) and is the author's call (a `.wslconfig`). FINDINGS 3bx.

**What the top size says.** At 316M logos is the fastest sort measured on 9 of 17 inputs at one thread (level on 3, slower
on 5) and 16 of 17 at 14 threads; against std::sort 6.7–7.6× on random doubles and 17–33× on the structured inputs;
against IPS⁴o at 14 threads 1.5–4.9× on all 13 numeric inputs, and 13 of 13 at every size from 3.16M up (11 of 13 at 1M,
5 of 13 at 100k–316k, unchanged). The structured routes' cost per element is flat from 10M to 316M (quantized 1.70 → 1.72
ns, block_perm 1.36 → 1.45) while every comparison sort rises 10–30%: the margins GROW with n, which is the buyer-facing
sentence this run adds.

**What moved against the claims** (nothing at 10M, where CLAIMS.md states them): three one-thread cells at scale. uniform
0.87 of x86-simd-sort (level to 10M; the in-house kernel grows 25% per element over the range, the library's 10%);
reach_mirror 0.62 (1.44 at 10M; the short-reach gate's finest sample lag is 0.028% of n and cannot see ±60 at 316M);
strings: logs 0.92 of the key-value sort at 100M (2.26 at 10M). Each is an OPEN engineering item (IDEAS_LOG 18) and each is
a claim to state with its size from now on: "one-thread parity on random doubles" is measured to 10M, "the back-to-front
log win" to 100M, "the log-line string route's 2.1–2.3×" at 10M.

**Deliverable:** `index.html` at the repository root, a one-page static reading of the report for a reader outside the
project (no library, both themes, phone-checked, its hover layer tested under a stub DOM), regenerated by `make scale-full`;
a CLAIMS.md history row.

## 2026-11-08, later — thirteen buyer-shaped inputs, run to 50M

**Asked:** "create more data shapes and cap at 50m items sorted. make more common examples of data that needs to be sorted, be
practical." The bench gained the columns a buyer actually sorts — event timestamps in arrival order (Poisson stream, network
latency), prices in cents, primary keys after deletions, Zipf foreign keys, ZIP codes, ages, a year of sensor readings, request
latencies, latitudes, log batches appended in order (the strand gate's shape, asked for on 2026-11-06), shard IDs, a sorted table
with a day's rows appended, order dates; and file paths, email addresses and ISO dates as text — and was run to 50M on all 33 on
a quiet box (2.85 / 56.0 ms, 1 h 21 min, 0 mismatches). FINDINGS 3by. The 316M report was overwritten by this run; its console log
is `reports/scale-run-316m-2026-11-08.log` and its numbers are in FINDINGS 3bx.

**What it says in commercial terms.** At 50M logos is the fastest sort measured on 25 of 33 inputs at one thread and 29 of 33 at
14 (re-measured 2026-11-10 with the Zipf fix in: 24 and 29, zip_codes level with IPS⁴o, fk_zipf 0.72; FINDINGS 3cb). On the buyer-shaped inputs at one thread, against the fastest rival: event timestamps 2.1×, prices 2.4×, keys 3.2×, ZIP codes
2.5×, ages 1.9×, sensor readings 1.9×, latencies 2.9×, shard IDs 1.9×, appended rows 2.5×, order dates 2.1×; at 14 threads event
timestamps 4.1×, order dates 4.6×, log batches 3.9×, ages 3.0×, appended rows 2.5×. The string columns 1.2–1.6× at one thread and
1.8–2.0× at 14. **Two buyer-shaped inputs are LOST at 14 threads: Zipf foreign keys 0.61 and ZIP codes 0.57 of IPS⁴o**, at every
size from 1M, and the mechanism is one gap: a value holding 5–6% of a column is too big for the partition's regions and too small
for the dominant-value route (20%), so it fills one bucket and one thread sorts it while thirteen wait. Every Zipf key column is in
that gap. It is the single most buyer-relevant engine item now (IDEAS_LOG 19 (a); predicted 1.5–2.2× when fixed). Second: at
100k–316k on one thread the counting routes are chosen where the vector quicksort would win by 2–5× (ZIP codes 0.18, prices 0.45):
GAPS 6's 100k–1M regime, from a new side (19 (b)).

**Effect on the claims:** a CLAIMS.md row. The 10M claims stand; "categorical / key columns at P threads" is RED until 19 (a) ships;
the practical wins above are green candidates on one machine.
