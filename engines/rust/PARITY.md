# The Rust engines against the C engine

There are two Rust engines here, and "parity" means a different thing for each. Read the first section for `sort_f64`.

## 1. `sort_f64`: the C engine's algorithm (the port, 2026-11-11)

`src/port/` is a transliteration of `engines/c/logos_psort.c`, the f64 sort in its default build: the same routes in the
same order, the same gates, the same constants, the C's names (`PORTING.md` has the rules). **Parity here means the same
decision on every input**, and it is checked by which route ran, not by reading the code.

**Route equivalence** (`bash parity/routes.sh`, logs in `parity/logs-port/`). The same input is sorted by the C engine and
by the port in one binary; both keep the C's route witnesses (`logos_twist_last(k)` for 35 counters, `logos_reach_last()`,
`logos_mirror_count()`). At one thread the witnesses must be equal; with threads some are counts that depend on which
worker claimed which chunk, so each engine is run six times and a witness differs only if the two ranges do not meet.

| run | cells | correct output | same route |
|---|---:|---:|---:|
| 72 shapes x 18 sizes (2 to 8.4M, both sides of every size gate), 1 thread | 1,296 | 1,296 | 1,296 |
| 72 shapes x 7 sizes (100k to 8.4M), 2, 3, 4, 6, 8 and 14 threads | 3,024 | 3,024 | 3,022 |

The two cells that differed over six runs (the gallop count on four streams at 300k and 8 threads; the lattice vector
count on a column whose lattice attempt is abandoned part-way, at 1M and 8 threads) are timing-dependent counts: over
sixty runs each the two engines' ranges meet. At one thread the counts are equal to the element (19,799 strays lifted,
249,864 lattice vectors, 9,731 kinks repaired, in both).

**Tests.** `cargo test`: 85 pass. 80 are the port's unit tests (each file's kernels, routes and gates); the 7 integration
tests (38 shapes x 25 sizes x thread counts, a NaN in every shape, signed zeros, concurrent callers, release) run on
`sort_f64`, which is now the port. The C harness's 55,809 checks are NOT ported, and nothing here has run under a
sanitizer or Miri.

**Speed** (`bash parity/parity.sh`, paired, calibration 3.3 / 63-65 ms; ratio = t(C) / t(Rust), level = 0.95-1.05).

| set | n | threads | ahead | level | behind | the cells behind |
|---|---:|---:|---:|---:|---:|---|
| scoreboard (17) | 8.4M | 1 | 0 | 14 | 3 | int_wide 0.87, distinct_2 0.94, distinct_65536 0.91 |
| scoreboard | 8.4M | 14 | 0 | 12 | 5 | int_wide 0.89, distinct_2 0.93, distinct_65536 0.91, quantized 0.89, near_sorted 0.76 |
| scoreboard | 1M | 1 | 1 | 13 | 3 | |
| scoreboard | 1M | 14 | 1 | 11 | 5 | |
| held-out (16) | 8.4M | 1 | 3 | 12 | 1 | latency 0.76 |
| held-out | 8.4M | 14 | 2 | 8 | 6 | normal 0.93, lognormal 0.92, int_narrow 0.77, sawtooth 0.94, latency 0.77, infinities 0.84 |

The same algorithm, slower in places: nothing in the port has been tuned for LLVM. The earlier engine's ledger (section
2) says what to expect where it is behind: LLVM turned a predicted branch into a conditional move in the kernel's
partition and folded the networks' shuffles, 15-23% each, until both were pinned. The port's loops have had no such
pass. The cells behind are the integer radix, the counting and lattice maps, the short-reach blocks and the near-sorted
route's parallel form: the next work is a loop-by-loop comparison of the disassembly there.

**What the port leaves out** (`PORTING.md`): every environment switch (they read as unset), the out-of-memory and
thread-creation-failure fallbacks (Rust aborts on the first; the pool always runs every worker), the profile hooks, and
the paths only a switch enables (the in-place partition, the linked-list knot placement, the pool trim, the quantized
float radix, the exponent map, the optional x86-simd-sort leaf). Not ported at all: int32, strings, the lazy view, the
batch entry, argsort, the in-place sort's parallel form. `sort_f64` sorts one array at a time, as the C does; a second
caller waits.

**Found on the way.** (1) The AVX2 quicksort this crate already had was NOT the C's: its pivot sampled at (n - 4) / 4
with another network and its depth fallback was the standard sort; the port now has the C's recursion. (2) The C's
`merge_4way_hybrid` uses +inf as the sentinel of an exhausted run, so a real +inf in a live run ties with it and the loop
does not end; `merge_all_runs` reaches it only with 16 or more runs, which `sort_fn` never passes, so it is latent
(reported, not fixed, in either engine). (3) The exponent map (opt-in, off) is not monotone when the sample spans more
than 255 binades. (4) `ctx_init`'s failure path leaves `pool_ix` at 0 rather than -1. (5) Unit tests written against
stand-ins passed alone and failed joined, four times: a null context the real leaf dereferences, a thread count the real
`count_threads` caps, a plan the stand-in `runs_plan` never made. None was a defect in the port; each was the test's
premise.

## 2. `sort_f64_v3`: an independent engine at the C's speed (before the port)

Measured 2026-11-11 on the reference box (i9-13900H, 6 P + 8 E cores, AVX2, WSL2; rustc 1.99.1, GCC 13.3.0 at the C
engine's own flags). Every number below comes from `parity/parity.sh`: both engines in ONE binary, the same input sorted
in adjacent pairs with the order alternating, every output verified (ascending under `<`, and the input's bit patterns
as a multiset). The tables are generated from the bench's logs by `parity/table.py`; none is typed by hand.

Everything from here on is about `sort_f64_v3` (`engine.rs`, `par.rs`), which was `sort_f64` until the port replaced it;
re-run its tables with `logos_rs_v3_sort_f64` in place of `logos_rs_sort_f64` in `parity/parity.cpp`.

**"Parity" in this section means measured speed on the same input, cell by cell. It does not mean the same list of routes.** The C
engine is 788 KB of source and some forty routes, each added on one paired A/B over three months. This engine is about
2,600 lines written in one session to the same architecture (a prescan and one sorted sample decide; address where the
data allows it, compare only where it does not), with thirteen routes. Where a shape needs a route this engine does not
have, it is behind, and the second table says so.

## Where it stands

| run | elements | threads | ahead | level | behind | behind on |
|---|---:|---:|---:|---:|---:|---|
| board_8m | 8,388,608 | 1 | 10 | 7 | 0 | - |
| board_8m | 8,388,608 | 14 | 9 | 8 | 0 | - |
| extra_8m | 8,388,608 | 1 | 7 | 4 | 5 | prices 0.47, prefix_95 0.70, stairs 0.77, logs8 0.69, infinities 0.93 |
| extra_8m | 8,388,608 | 14 | 7 | 4 | 5 | int_narrow 0.91, prices 0.81, sawtooth 0.93, latency 0.70, signed_zeros 0.66 |
| board_1m | 1,048,576 | 1 | 8 | 8 | 1 | block_perm 0.91 |
| board_1m | 1,048,576 | 14 | 11 | 2 | 4 | uniform 0.88, int_wide 0.94, distinct_2 0.85, few_distinct 0.88 |
| extra_1m | 1,048,576 | 1 | 6 | 7 | 3 | prefix_95 0.61, stairs 0.68, logs8 0.72 |
| extra_1m | 1,048,576 | 14 | 11 | 1 | 4 | prices 0.86, sawtooth 0.95, latency 0.80, with_nan 0.88 |
| board_262k | 262,144 | 1 | 6 | 10 | 1 | block_overlap 0.91 |
| board_262k | 262,144 | 4 | 6 | 8 | 3 | distinct_2 0.60, few_distinct 0.74, sorted 0.78 |
| board_30m | 30,000,000 | 1 | 9 | 5 | 3 | heavy_tail 0.95, clustered 0.94, clustered_32 0.93 |
| board_30m | 30,000,000 | 14 | 10 | 6 | 1 | near_sorted 0.92 |

- **The scoreboard's seventeen shapes** (`engines/c/scoreboard.cpp`, the project's definition of "fastest"): ahead of or
  level with the C engine on **every cell at 8.4M** at one thread and at fourteen. This set is what the engine was
  built and tuned against, so it is the weaker evidence. **Away from 8.4M it is not clean:** at 1M and fourteen threads
  it is behind on uniform (0.88) and on the two smallest counting shapes (0.85-0.88), and at 262k and four threads on
  those two (0.60, 0.74) and on sorted (0.78): a parallel phase here wakes its workers through a mutex and a condition
  variable, and at these sizes that is a tenth to a third of the sort. At 30M at one thread three unstructured shapes
  read 0.93-0.95 (the kernel's networks, below), and near-sorted 0.92 at fourteen threads.
- **Sixteen shapes it was not tuned on** (`extra`): correct on all of them, **behind on five at each thread count at
  8.4M** (three and four at 1M). Each is a route the C engine has and this one does not, listed below with its
  mechanism. Read this set as the honest one.
- The runs are in `parity/logs-2026-11-11/`. Calibration 3.2-3.8 ms / 63-81 ms across them (quiet to slightly loaded).
- Level means the median of t(C) / t(Rust) over the pairs is within 0.95-1.05; one cell's median moves 2-4% between
  runs on this box, so a cell at 0.95 or 1.05 is at the floor, not a finding.

### The scoreboard set, 8.4M elements

`parity: n = 8388608, 16 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.3 ms, std::sort-1M 63.3 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 14T | Rust, 14T | C / Rust (p10-p90) | at 14T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| uniform | 9.23 | 9.45 | 0.98 (0.93-1.02) | level | 1.67 | 1.60 | 0.98 (0.91-1.06) | level |
| heavy_tail | 9.25 | 9.43 | 0.99 (0.95-1.06) | level | 1.76 | 1.75 | 0.98 (0.94-1.09) | level |
| bimodal | 9.52 | 9.84 | 0.96 (0.89-1.02) | level | 1.84 | 1.81 | 0.97 (0.93-1.07) | level |
| clustered | 9.24 | 9.72 | 0.96 (0.94-1.02) | level | 2.27 | 1.94 | 1.15 (1.06-1.24) | ahead |
| clustered_8 | 9.45 | 9.79 | 0.96 (0.93-1.00) | level | 2.26 | 2.07 | 1.06 (0.96-1.20) | ahead |
| clustered_32 | 9.12 | 9.67 | 0.96 (0.89-1.03) | level | 2.65 | 2.07 | 1.28 (1.21-1.30) | ahead |
| int_wide | 10.03 | 7.90 | 1.20 (1.06-1.31) | ahead | 1.57 | 1.60 | 1.04 (0.96-1.12) | level |
| distinct_2 | 0.57 | 0.55 | 1.06 (0.97-1.31) | ahead | 0.31 | 0.29 | 1.00 (0.94-1.15) | level |
| few_distinct | 0.70 | 0.62 | 1.11 (1.05-1.24) | ahead | 0.34 | 0.32 | 1.02 (0.93-1.14) | level |
| distinct_256 | 1.42 | 0.96 | 1.46 (1.33-1.65) | ahead | 0.39 | 0.31 | 1.23 (1.15-1.41) | ahead |
| distinct_65536 | 2.31 | 1.40 | 1.64 (1.42-1.79) | ahead | 1.21 | 0.54 | 2.10 (1.90-2.36) | ahead |
| quantized | 1.60 | 1.11 | 1.45 (1.32-1.54) | ahead | 0.47 | 0.34 | 1.38 (1.33-1.52) | ahead |
| near_sorted | 4.04 | 2.83 | 1.40 (1.30-1.49) | ahead | 0.93 | 0.97 | 0.97 (0.94-0.99) | level |
| sorted | 0.32 | 0.31 | 1.01 (0.81-1.23) | level | 0.17 | 0.16 | 1.03 (0.91-1.23) | level |
| reversed | 0.72 | 0.64 | 1.09 (0.99-1.17) | ahead | 0.62 | 0.38 | 1.73 (1.58-1.90) | ahead |
| block_perm | 1.29 | 1.10 | 1.18 (1.14-1.25) | ahead | 0.76 | 0.61 | 1.21 (1.14-1.31) | ahead |
| block_overlap | 1.32 | 1.17 | 1.10 (1.07-1.21) | ahead | 0.75 | 0.62 | 1.21 (1.13-1.28) | ahead |

### The set it was not tuned on, 8.4M elements

`parity: n = 8388608, 12 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.4 ms, std::sort-1M 64.8 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 14T | Rust, 14T | C / Rust (p10-p90) | at 14T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| normal | 9.76 | 9.91 | 0.98 (0.96-1.00) | level | 2.23 | 1.74 | 1.29 (1.21-1.36) | ahead |
| lognormal | 9.67 | 9.74 | 0.99 (0.95-1.02) | level | 1.67 | 1.74 | 1.00 (0.94-1.05) | level |
| int_narrow | 7.70 | 6.73 | 1.19 (1.06-1.23) | ahead | 1.18 | 1.34 | 0.91 (0.84-0.95) | **behind** |
| zipf | 3.84 | 1.24 | 3.10 (2.89-3.23) | ahead | 0.79 | 0.39 | 2.12 (1.97-2.75) | ahead |
| prices | 4.25 | 9.77 | 0.47 (0.43-0.51) | **behind** | 1.30 | 1.63 | 0.81 (0.77-0.86) | **behind** |
| sawtooth | 15.85 | 11.40 | 1.38 (1.36-1.45) | ahead | 1.90 | 1.97 | 0.93 (0.86-0.96) | **behind** |
| prefix_95 | 2.43 | 3.53 | 0.70 (0.64-0.76) | **behind** | 1.05 | 1.05 | 0.96 (0.94-1.02) | level |
| kinks | 0.75 | 0.64 | 1.21 (1.15-1.27) | ahead | 0.26 | 0.17 | 1.50 (1.39-1.62) | ahead |
| block_twist | 1.48 | 1.37 | 1.11 (1.05-1.22) | ahead | 0.79 | 0.63 | 1.23 (1.13-1.29) | ahead |
| stairs | 2.02 | 2.66 | 0.77 (0.72-0.91) | **behind** | 0.84 | 0.67 | 1.26 (1.22-1.31) | ahead |
| logs8 | 6.62 | 9.99 | 0.69 (0.66-0.73) | **behind** | 1.84 | 1.79 | 0.96 (0.93-1.06) | level |
| latency | 6.70 | 6.86 | 0.99 (0.93-1.07) | level | 0.91 | 1.36 | 0.70 (0.65-0.73) | **behind** |
| signed_zeros | 13.16 | 7.45 | 1.75 (1.55-1.87) | ahead | 2.80 | 4.16 | 0.66 (0.65-0.73) | **behind** |
| infinities | 9.36 | 10.11 | 0.93 (0.86-0.98) | **behind** | 3.03 | 1.90 | 1.60 (1.49-1.71) | ahead |
| subnormals | 10.52 | 10.23 | 1.05 (1.00-1.11) | ahead | 6.93 | 1.86 | 3.79 (3.66-3.99) | ahead |
| with_nan | 12.97 | 12.24 | 1.04 (0.99-1.10) | level | 1.74 | 1.73 | 1.02 (0.94-1.07) | level |

### Other sizes

`parity: n = 1048576, 40 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.8 ms, std::sort-1M 75.4 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 14T | Rust, 14T | C / Rust (p10-p90) | at 14T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| uniform | 7.66 | 7.18 | 1.00 (0.95-1.13) | level | 1.80 | 1.84 | 0.88 (0.77-1.00) | **behind** |
| heavy_tail | 7.58 | 7.01 | 1.02 (0.93-1.19) | level | 1.93 | 1.85 | 0.96 (0.86-1.05) | level |
| bimodal | 7.63 | 6.92 | 1.03 (0.96-1.21) | level | 2.08 | 2.16 | 0.97 (0.84-1.06) | level |
| clustered | 7.29 | 6.97 | 1.03 (0.96-1.15) | level | 2.81 | 2.19 | 1.22 (1.13-1.37) | ahead |
| clustered_8 | 7.83 | 7.24 | 1.01 (0.91-1.12) | level | 2.63 | 2.34 | 1.12 (1.01-1.20) | ahead |
| clustered_32 | 7.08 | 7.10 | 1.04 (0.94-1.16) | level | 5.20 | 2.20 | 2.30 (2.00-2.48) | ahead |
| int_wide | 8.33 | 6.67 | 1.14 (0.89-1.48) | ahead | 1.78 | 1.68 | 0.94 (0.86-1.09) | **behind** |
| distinct_2 | 0.41 | 0.37 | 1.08 (0.93-1.46) | ahead | 0.30 | 0.29 | 0.85 (0.58-1.26) | **behind** |
| few_distinct | 0.56 | 0.48 | 1.15 (1.06-1.31) | ahead | 0.34 | 0.33 | 0.88 (0.65-1.10) | **behind** |
| distinct_256 | 1.32 | 0.86 | 1.51 (1.31-1.70) | ahead | 0.69 | 0.43 | 1.32 (1.10-1.66) | ahead |
| distinct_65536 | 3.89 | 1.73 | 2.13 (1.93-2.42) | ahead | 3.52 | 2.03 | 1.60 (1.43-1.74) | ahead |
| quantized | 1.60 | 0.96 | 1.65 (1.52-1.92) | ahead | 0.85 | 0.56 | 1.40 (1.05-1.60) | ahead |
| near_sorted | 2.89 | 2.38 | 1.22 (1.13-1.41) | ahead | 1.15 | 1.05 | 1.20 (0.99-1.36) | ahead |
| sorted | 0.19 | 0.19 | 1.02 (0.92-1.12) | level | 0.22 | 0.19 | 1.10 (0.70-1.46) | ahead |
| reversed | 0.49 | 0.38 | 1.27 (1.17-1.38) | ahead | 0.60 | 0.28 | 1.60 (0.99-2.42) | ahead |
| block_perm | 1.32 | 1.28 | 0.91 (0.81-1.10) | **behind** | 0.91 | 0.49 | 1.56 (1.16-2.10) | ahead |
| block_overlap | 1.03 | 1.09 | 0.96 (0.76-1.15) | level | 0.95 | 0.51 | 1.55 (1.26-2.08) | ahead |

`parity: n = 1048576, 30 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.4 ms, std::sort-1M 72.3 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 14T | Rust, 14T | C / Rust (p10-p90) | at 14T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| normal | 7.28 | 7.75 | 1.02 (0.87-1.12) | level | 3.17 | 2.02 | 1.43 (1.30-1.65) | ahead |
| lognormal | 7.72 | 7.71 | 1.02 (0.96-1.10) | level | 2.09 | 2.01 | 0.96 (0.85-1.14) | level |
| int_narrow | 6.16 | 5.15 | 1.03 (0.77-1.37) | level | 2.25 | 1.61 | 1.23 (1.10-1.35) | ahead |
| zipf | 3.61 | 1.27 | 2.81 (2.66-2.97) | ahead | 1.63 | 0.70 | 2.08 (1.55-2.53) | ahead |
| prices | 8.61 | 6.81 | 1.13 (1.01-1.31) | ahead | 1.83 | 2.10 | 0.86 (0.73-0.94) | **behind** |
| sawtooth | 15.24 | 9.71 | 1.60 (1.48-1.80) | ahead | 2.37 | 2.40 | 0.95 (0.85-1.05) | **behind** |
| prefix_95 | 1.59 | 2.61 | 0.61 (0.53-0.70) | **behind** | 1.46 | 1.29 | 1.11 (0.95-1.26) | ahead |
| kinks | 0.57 | 0.41 | 1.33 (1.25-1.45) | ahead | 0.43 | 0.24 | 1.67 (1.20-2.17) | ahead |
| block_twist | 1.04 | 1.03 | 0.96 (0.83-1.37) | level | 0.97 | 0.51 | 1.60 (1.22-2.18) | ahead |
| stairs | 1.49 | 2.04 | 0.68 (0.60-0.76) | **behind** | 0.84 | 0.71 | 1.09 (0.88-1.28) | ahead |
| logs8 | 5.34 | 8.07 | 0.72 (0.66-0.81) | **behind** | 2.42 | 1.92 | 1.19 (0.94-1.36) | ahead |
| latency | 5.78 | 5.83 | 0.96 (0.87-1.10) | level | 1.44 | 1.78 | 0.80 (0.71-0.91) | **behind** |
| signed_zeros | 10.91 | 5.03 | 2.19 (1.92-2.40) | ahead | 4.99 | 4.03 | 1.15 (1.07-1.26) | ahead |
| infinities | 7.13 | 6.93 | 1.01 (0.98-1.17) | level | 3.25 | 2.03 | 1.48 (1.38-1.60) | ahead |
| subnormals | 7.59 | 7.09 | 1.02 (0.99-1.09) | level | 6.17 | 2.03 | 2.83 (2.61-3.07) | ahead |
| with_nan | 10.92 | 9.00 | 1.12 (1.04-1.21) | ahead | 1.93 | 2.12 | 0.88 (0.80-0.95) | **behind** |

`parity: n = 262144, 60 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.7 ms, std::sort-1M 81.3 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 4T | Rust, 4T | C / Rust (p10-p90) | at 4T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| uniform | 7.93 | 7.47 | 1.06 (0.99-1.19) | ahead | 3.50 | 3.46 | 1.01 (0.86-1.11) | level |
| heavy_tail | 7.64 | 7.32 | 1.04 (1.00-1.12) | level | 4.10 | 4.24 | 1.00 (0.96-1.11) | level |
| bimodal | 7.42 | 7.19 | 1.04 (0.99-1.12) | level | 4.07 | 4.07 | 1.02 (0.95-1.28) | level |
| clustered | 7.26 | 6.79 | 1.05 (1.00-1.11) | level | 3.86 | 3.73 | 1.02 (0.91-1.15) | level |
| clustered_8 | 6.69 | 6.03 | 1.04 (0.97-1.14) | level | 4.14 | 4.01 | 1.02 (0.94-1.11) | level |
| clustered_32 | 6.98 | 6.65 | 1.04 (0.95-1.09) | level | 4.14 | 4.14 | 0.99 (0.82-1.13) | level |
| int_wide | 8.29 | 7.83 | 1.05 (0.96-1.15) | level | 4.46 | 4.10 | 1.11 (0.99-1.24) | ahead |
| distinct_2 | 0.40 | 0.39 | 1.01 (0.93-1.11) | level | 0.22 | 0.34 | 0.60 (0.42-0.90) | **behind** |
| few_distinct | 0.60 | 0.53 | 1.14 (1.08-1.22) | ahead | 0.29 | 0.32 | 0.74 (0.48-1.16) | **behind** |
| distinct_256 | 1.45 | 0.95 | 1.52 (1.46-1.64) | ahead | 0.85 | 0.72 | 1.10 (0.95-1.28) | ahead |
| distinct_65536 | 6.78 | 6.91 | 1.04 (0.98-1.13) | level | 3.81 | 3.73 | 1.01 (0.88-1.13) | level |
| quantized | 2.02 | 1.13 | 1.78 (1.70-1.92) | ahead | 1.46 | 0.87 | 1.29 (1.04-1.77) | ahead |
| near_sorted | 3.23 | 2.48 | 1.22 (1.12-1.35) | ahead | 2.07 | 1.59 | 1.18 (0.94-1.38) | ahead |
| sorted | 0.13 | 0.12 | 1.02 (1.01-1.07) | level | 0.20 | 0.20 | 0.78 (0.69-1.03) | **behind** |
| reversed | 0.47 | 0.33 | 1.33 (1.25-1.47) | ahead | 0.50 | 0.39 | 0.97 (0.75-1.30) | level |
| block_perm | 0.89 | 0.91 | 0.97 (0.88-1.09) | level | 0.98 | 0.59 | 1.54 (1.23-1.93) | ahead |
| block_overlap | 0.78 | 0.84 | 0.91 (0.84-0.97) | **behind** | 1.05 | 0.65 | 1.54 (1.36-1.70) | ahead |

`parity: n = 30000000, 5 adjacent pairs per cell, every output verified; calibration memcpy-64MB 3.2 ms, std::sort-1M 65.3 ms`

| shape | C, 1T | Rust, 1T | C / Rust (p10-p90) | at 1T | C, 14T | Rust, 14T | C / Rust (p10-p90) | at 14T |
|---|---:|---:|---:|---|---:|---:|---:|---|
| uniform | 10.42 | 11.07 | 0.95 (0.93-0.99) | level | 1.90 | 1.92 | 0.98 (0.93-1.29) | level |
| heavy_tail | 10.54 | 11.25 | 0.95 (0.89-1.00) | **behind** | 1.96 | 2.07 | 0.97 (0.84-1.05) | level |
| bimodal | 10.53 | 10.89 | 0.97 (0.94-0.97) | level | 2.16 | 2.07 | 1.04 (1.00-1.09) | level |
| clustered | 10.40 | 11.15 | 0.94 (0.91-1.01) | **behind** | 2.64 | 2.20 | 1.16 (1.14-1.37) | ahead |
| clustered_8 | 10.83 | 10.94 | 0.99 (0.96-1.00) | level | 2.56 | 2.39 | 1.07 (0.96-1.11) | ahead |
| clustered_32 | 10.60 | 11.03 | 0.93 (0.92-0.97) | **behind** | 2.64 | 2.32 | 1.14 (1.11-1.27) | ahead |
| int_wide | 10.09 | 8.78 | 1.21 (1.06-1.25) | ahead | 1.98 | 1.87 | 1.09 (0.99-1.11) | ahead |
| distinct_2 | 0.57 | 0.57 | 1.00 (0.95-1.14) | level | 0.28 | 0.28 | 1.02 (0.95-1.05) | level |
| few_distinct | 0.70 | 0.64 | 1.19 (1.08-1.22) | ahead | 0.29 | 0.28 | 1.04 (1.03-1.10) | level |
| distinct_256 | 1.57 | 1.05 | 1.46 (1.39-1.69) | ahead | 0.35 | 0.27 | 1.31 (1.26-1.32) | ahead |
| distinct_65536 | 2.30 | 1.38 | 1.60 (1.57-1.74) | ahead | 1.38 | 0.38 | 3.76 (3.50-3.87) | ahead |
| quantized | 1.72 | 1.14 | 1.50 (1.43-1.60) | ahead | 0.37 | 0.28 | 1.34 (1.27-1.39) | ahead |
| near_sorted | 4.11 | 2.95 | 1.38 (1.30-1.48) | ahead | 0.81 | 0.87 | 0.92 (0.82-0.94) | **behind** |
| sorted | 0.32 | 0.30 | 0.98 (0.94-1.24) | level | 0.14 | 0.14 | 1.03 (1.01-1.06) | level |
| reversed | 0.78 | 0.70 | 1.11 (1.07-1.16) | ahead | 0.62 | 0.38 | 1.63 (1.58-1.69) | ahead |
| block_perm | 1.31 | 1.06 | 1.19 (1.18-1.24) | ahead | 0.73 | 0.60 | 1.22 (1.19-1.28) | ahead |
| block_overlap | 1.36 | 1.20 | 1.10 (1.07-1.19) | ahead | 0.74 | 0.59 | 1.22 (1.21-1.26) | ahead |

## What is ported, and how it differs

| route (`last_route()`) | C counterpart | this engine |
|---|---|---|
| `Sorted` / `Reversed` | the prescan in vector blocks, in parallel past a 64K head, kink repair in the run detector | AVX2 blocks of 32, parallel past a 64K head. Kinks are repaired IN THE PRESCAN: a swapped pair is swapped back, and where swaps overlap the 6, 12 or 24 elements around the descent are sorted if their neighbours then agree. Chunks leave descents within 24 of a cut for a pass over the cuts |
| `FewValues` | low-cardinality counting, up to 8 values in vector registers | the same: compares by bit pattern, a streamed emit, both in parallel |
| `Lattice` | `lattice_try_f64`, `lat1_gate`, spills | exact lattices only: every sample point must be reproduced bit for bit by `base + i * step` with a fused multiply-add, so the map needs no key table. AVX2 map over claimed chunks, off-lattice values spilled (to n/32), a window trimmed by 0.75% at each end when the full range is too many slots, a parallel emit that carries the spills |
| `HashedCounts` | the hash-count route | one thread only, up to 1,024 distinct values, open addressing (no perfect hash) |
| `IntRadix` | the int32 LSD radix | digits of 11 + 11 + 10 bits, the first scatter straight from the doubles, a digit that never varies skipped; also the bucket leaf of the partition when the sample is all int32 |
| `OneStrand` / `Runs` | relocation, the knot route, strays inside strands, the twist | ONE route. Detection copies kept elements to scratch as it scans (a cache-sized block at a time, so the scan and the first copy are one trip to memory); strays are lifted by one rule (up to 4 off the run's tail, up to 4 ahead); a run is split at a step larger than half of what it has spanned; a descending stretch is written reversed as it is copied; a stretch where strays turn dense ends the scan and the rest is handed over as strays (to n/8). The sorted strays are one more strand. The plan is the knot sweep; placement copies what lies alone and merges what crosses (scalar merge, a gallop when one side is 16x the other, a heap for three or more) |
| `Reach` | the short-reach route (index blocks sorted in place, seams repaired) | blocks of 4,096 sorted by the kernel as they are copied to scratch, then the run plan lays them back; needs an n-sized scratch, which the C route does not |
| `Halves` | `halves_try_f64` | one split at the sample's median, a kernel per side, at two threads |
| `Partition` | the histogram-free partition and its certificates, key grid, mode grid, splitter tables | histogram-free with sample-sized regions and spills, 12 buckets per thread, chunks claimed. ONE bucket map in place of the certificates: a grid of equal cells in value space or in key space (whichever settles more of the sample at its root), bucket edges snapped to cell edges, a finer grid under any crowded cell, mode selection by compares for two to four far-apart modes, and a bucket of its own for a value the sample holds twice or more |
| `Kernel` | `avx2_qsort_f64` | the same design line for line (compress-store partition, bitonic networks to 64, vector-sampled pivot, the NaN / -0.0 scan in the first partition), plus networks for 8 and 16 |
| `NanLast` | a declined kernel falls to the scalar leaf | NaNs moved to the tail, the rest sorted |

Also here and not in C: **`sort_f64` may be called from several threads at once** (the scratch pool hands buffers out
under a lock and a second caller of a busy worker pool runs sequentially; `tests/v3_engine.rs`, `concurrent_callers`).
The C engine allows one call at a time per process.

## Not ported

Everything in `engines/c/PARITY.md` that this file does not name. The ones that cost measured cells:

| C has | this engine | seen as |
|---|---|---|
| lattices whose step is not exact in binary (prices in cents): a key per slot | none: such data goes to the kernel or the partition | `prices` 0.47 at one thread, 0.81 at 14 threads |
| vector merges (two-strand in two chains, k-way) | scalar merge, heap for k >= 3 | `stairs` at one thread 0.77, `logs8` 0.69, `prefix_95` 0.70 |
| the short-reach route in place, at fourteen threads | block sort plus a full plan and placement | `latency` at 14 threads 0.70 |
| heavy values counted in the scatter, the dominant-value route | a heavy value gets its own bucket but is still moved and sorted on one thread | `signed_zeros` (half the array is zeros) at 14 threads 0.66 |
| the thin front, branchless networks below 16, founder windows, the low-discrepancy probe | below 2,048 elements: prescan, then the kernel | not measured here (`make callbench` is the C tool; nothing in this directory times a call of ten elements) |
| relocation's half placement (4 bytes per element of scratch) | a full n-sized scratch, and n/8 more for strays | memory: not measured at all (no memprobe or rssprobe twin) |
| tail / head / streams / mirror / loop-back / seam / wave / cuts / run-length / dense-integer / dominant / float32 routes | none | shapes built for them are not in either set |
| int32 entry (`logos_sort_i32`), argsort with threads, batch, the lazy view, strings on prefix keys | `sort_i32` widens to f64; the v2 extras are unchanged | - |
| fault injection (`make faultprobe`) | an allocation failure aborts (Rust's default); a worker that cannot be started is tolerated by design and UNTESTED | - |

## Where it is ahead, and what that is worth

At 8.4M: the lattice shapes (1.45-1.64x at one thread, 1.23-2.10x at fourteen), integer-valued doubles at one thread
(1.20x), near-sorted data at one thread (1.40x), the block shapes (1.10-1.21x at both), a reversed array at fourteen
threads (1.73x: the reversal is parallel), and three to thirty-two clusters at fourteen threads (1.06-1.28x). The mechanisms are narrower forms
of what C does: a lattice verified by reproduction needs no key table, but only exists for steps exact in binary; the
radix scatters straight out of the doubles; detection and the first copy are one pass. These are leads for the C
engine to A/B, not verdicts on it: the C routes handle inputs these forms decline.

## What the session found on the way (the ledger's rows)

| what | measured | why |
|---|---|---|
| LLVM compiles the partition's "which end next" as a conditional move | the kernel **0.85** of C's from 512 elements up | as a cmov the eight loads of a block wait on the previous block's popcount chain; as a predicted branch they are issued ahead. An empty `asm!` block in one arm keeps it a branch: 0.99-1.01 |
| LLVM folds the networks' permutes into their blends | the 32- and 64-element networks **0.77-0.90** | it sees that `v` and a permutation of `v` have symmetric minima and narrows to 128-bit halves with extra shuffles: half again as many port-5 operations. Both operands made opaque (empty `asm!`): the instruction mix is as written, **0.88-0.92**. Still behind at 24-64 elements (the 64-element network keeps 16 vectors live in 16 registers: 109 stack references in LLVM's code), which is the 0.93-0.98 on unstructured shapes at one thread |
| Scanning a megabyte run, then copying it | block shapes **0.63-0.70** at one thread | two trips to memory. A cache-sized block scanned and copied at once, into scratch that the placement then reads: 1.1-1.25 |
| A plan gate that counted live strands | shuffled blocks went to the KERNEL at 2.1M | ascending block boundaries fuse blocks into runs with holes; every fused run "crossed" every other. Found only by the test that names its route. The gap split, and a gate on pieces that actually cross |
| Sorting run endpoints with a partial order | a panic inside the standard sort; then **output that was not a permutation** at four threads | a NaN can sit inside a run (it is never a descent), and a run that begins with one has its first value after its last in total order: the strand closed before it opened and was never placed. Both found by the NaN test |
| A stray budget judged per chunk | a sorted prefix with a 5% tail declined at 14 threads | the tail is 70% of ONE chunk. One budget for the array (n/8), spent only after every chunk has reported: when a chunk dumped as it went, plain noise copied a fourteenth of the array first and uniform read 0.88 |
| The detector reading on through noise | a sorted prefix with a 5% tail **17 ns per element** at one thread (0.125) | its look-ahead strung the tail's noise into runs of twenty that each crossed everything, and the plan declined after a full pass. A window: strays over a quarter of the last 4,096 elements end the scan |
| A gallop threshold moved from 8x to 64x on one shape's reading | near-sorted **0.79** where it had read 1.33 | the plain merge is ~3 ns per element of output, a gallop ~50 ns per element of the short side: the gallop wins under about 6%. Set at 16x from both readings |
| Kink repair by swapping pairs | stopped 0.06% into an array with 1% random adjacent swaps | one swap in fifty overlaps another and leaves an element two places out. Windows of 6, 12, 24 |
| Tuning on the scoreboard alone | 16 of 17 ahead or level there while the held-out set, on its first run, read 8 of 16 behind at one thread and 9 at fourteen | the second set existed from the first hour and was not run until the first was green. Run it first |

## Reproduce

Everything runs inside WSL from `engines/rust`; build products go to `~/logos-rs-build` and `~/logos-rs-target`.

```sh
bash parity/test.sh                              # cargo test --release: 13 tests, ~40 s
bash parity/parity.sh [n] [pairs] [P] [filter]   # C against Rust; filter = a shape name, `extra` or `all`
bash parity/kernbench.sh [N] [pairs]             # the two AVX2 kernels alone, by chunk size
bash parity/trace.sh [n] [P] [filter]            # a --features trace build: which route ran and its phases
python parity/table.py <logs>                    # the tables above
python gen_nets.py                               # regenerates src/kernel_nets.rs
```

A trace build names the route and each phase's share of its own time; its first shape is a cold call (page faults on
fresh scratch). It is not for ranking two routes.
