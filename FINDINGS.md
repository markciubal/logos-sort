# FINDINGS — consolidated measured results

All numbers below were produced under the methodology in CLAUDE.md (verified
output, calibration-checked, like-vs-like threading). Where a run was loaded,
that is noted. Machine for parallel figures: 20-thread single-socket box under
WSL2; sandbox figures are a 1-core AVX-512 Xeon.

## 1. The fair fight (engines/c, parbench-full, 20 threads, 8M f64)

Latest complete run (calibration showed load; treat margins as ±20–30%, but the
per-shape winner pattern is stable across runs):

| shape | gnu_par | logos | logos+SIMD | x86simd MT | ips4o | winner |
|---|---|---|---|---|---|---|
| uniform | 165 | 60.5 | 92.0 | **34.2** | 138 | x86simd |
| near_sorted | 104 | 26.2 | 76.7 | **24.8** | 121 | x86simd ≈ logos |
| few_distinct | 81 | **7.2** | 6.9 | 9.5 | 116 | **logos** |
| heavy_tail | 159 | 71.7 | 97.5 | **23.6** | 117 | x86simd |
| bimodal | 173 | 68.7 | 86.9 | **23.9** | 140 | x86simd |
| quantized | 118 | **9.2** | 8.6 | 23.3 | 133 | **logos (2.5×)** |
| int_wide | 167 | 58.7 | 87.6 | **22.7** | 135 | x86simd |
(ms)

Readings: IPS⁴o — the SOTA parallel *comparison* sort — loses to everything on
this f64 workload. logos+SIMD-leaf is worse than plain logos on 5/7 shapes
(measured twice): bolting SIMD onto logos's partition is a dead end. logos wins
exactly where it avoids the scatter.

## 2. Single-threaded vs x86-simd-sort (1-core sandbox, 8M)

logos loses every shape 1.1–3.8× (uniform 0.32×, heavy_tail 0.26×, few_distinct
0.30× — x86-simd-sort has its own low-cardinality path). With the SIMD leaf fully
wired, logos+SIMD reaches 0.72–0.98× of x86-simd-sort: parity at best, never
beyond (dispatch/probe overhead before hand-off is the ceiling).

## 3. Engine-internal milestones (engines/c CHANGELOG has full detail)

- 3.4.0 bucket-index cache + destination prefetch: 20T floor 65 → 41–49 ms.
  Hyperthreading helped both sorts → scatter is memory-LATENCY-bound.
- 3.6.0 lattice counting (quantized 109→52 ms seq), near-sorted index-partition
  merge (28→22.5 ms 20T), scratch pool (~13% = the page-fault floor, first
  measured), ping-pong (removed 37% traffic, moved time ~12% → floor is NOT
  bandwidth).
- Strict in-place mode (~1n memory): 1.6–6× slower than buffered — the
  RAM-ceiling tool, not a speed choice.
- argSort/permutations/outliers/multikey-QS strings in logos_extra.c.

## 3b. Disjoint-run relocation (J5, 2026-09-10, engines/c `make j5bench`)

Block-permuted data (shuffled sorted blocks with disjoint value ranges) is
relocated by memcpy from sorted run descriptors instead of merged. Interleaved
A/B, 8M, best of 5, on a box whose calibration line read ~1.8× off quiet:

| shape | 1T | 20T |
|---|---|---|
| block_perm64 | 2.82× | 2.03× |
| block_perm1k | 3.39× | 2.18× |
| block_overlap (25% of data in 2-run chains) | 2.34× | 1.89× |
| segmented, few_runs, near_sorted, uniform | 0.96–0.99× | 0.95–1.05× |

Predicted before measuring: 1T 2.5–4×, 20T 1.5–2.5×, guards ±3%. Two measured
wrong turns on the way (an interval gate that never fired because ascending
block boundaries fuse runs around holes; a sequential probe whose thread spawns
cost the parallel path) are in the CHANGELOG and the CLAUDE.md ledger.

## 3c. Per-phase profile (`make profile`, 2026-09-10) — the histogram is the parallel bottleneck

First per-phase, per-thread breakdown of the engine (CPU received vs wall,
barrier stall, idle, leaf path per bucket). 8M, 20 threads, loaded box
(calibration ~2× off quiet), CPU thread-ms:

| shape | histogram | scatter | leaf sorts | histogram share of wall |
|---|---|---|---|---|
| uniform | 836 (~100 ns/elem) | 88 | 313 | 63% |
| heavy_tail | 564 | 114 | 376 | 49% |
| bimodal | — | — | — | 70% |
| int_wide | — | — | — | 62% |

Preempted time in those phases was ≤4%, so this is engine CPU, not load. The
cost is `bucket_of`'s dependent-load binary search over the 79 splitters; per
element it exceeds the whole 1-thread coarse sort (~29 ns). 3.4.0's
bucket-index cache moved the search from the scatter to the histogram rather
than removing it. Predicted fix: a direct-index splitter table, 1.5–2× on every
unstructured MULTI row. parbench now also carries block_perm / block_overlap
(20T: 6.1× / 4.6× vs gnu_par).

## 3d. Splitter-lookup crux and the self-sampling pass (experiments/paradigm/splitter_crux.cpp)

Bucket id per element, 8M, one thread, ids verified identical to the engine's search:

| shape | engine search | value table | key table | self-chosen (trial on 1/32, commit) |
|---|---|---|---|---|
| uniform | 43.6 ns | 4.7 | 19.0 | 5.3 |
| int_wide | 53.9 | 2.7 | 11.4 | 5.3 |
| heavy_tail | 55.3 | 24.8 | 11.6 | 13.3 |
| bimodal | 53.2 | 26.2 | 17.5 | 16.6 |
| clustered | 61.7 | 5.1 | 18.0 | 5.3 |

Building the table from first-half hit counts instead of the splitters gained
nothing (the splitters are quantiles already). Letting the pass time both tables
on its first chunks and commit to the winner never chose wrong. Design note:
`SELF_SAMPLING.md`.

## 1h. Full rerun, everything shipped (parbench-full, 14 threads, 2026-09-11 late; box hot, see §3t)

Ratios within each row are the trustworthy part; the milliseconds are not
comparable with any other run.

| shape | logos | x86-simd-sort | IPS⁴o | logos / x86simd | logos / IPS⁴o |
|---|---|---|---|---|---|
| uniform | 30.4 | 38.4 | 76.7 | 1.26× | 2.52× |
| near_sorted | 13.9 | 30.1 | 41.9 | 2.17× | 3.01× |
| few_distinct | 10.3 | 14.0 | 8.1 | 1.36× | 0.79× |
| heavy_tail | 36.1 | 34.1 | 63.8 | **0.94×** | 1.77× |
| bimodal | 35.8 | 72.3 | 143.6 | **2.02×** | 4.01× |
| quantized | 18.5 | 22.6 | 25.5 | 1.22× | 1.38× |
| int_wide | 24.8 | 33.8 | 65.1 | 1.36× | 2.62× |
| block_perm | 13.0 | 28.3 | 41.0 | 2.18× | 3.15× |
| block_overlap | 16.3 | 39.2 | 49.9 | 2.40× | 3.06× |

Single-threaded, logos leads on near_sorted (2.39×), quantized (1.08×) and the
two block shapes (4.8× and 4.6×), and trails on uniform (0.76×), few_distinct
(0.74×), heavy_tail (0.80×), bimodal (0.81×) and int_wide (0.95×). It beats
the LSD radix and timsort on all nine.

Readings, stated with the caution §3t requires:

- **bimodal is the clear change**: 2.02× over x86-simd-sort here against 1.01×
  in §1f, which is the direction and roughly the size the per-mode certificate
  predicted (§3s measured 1.25–1.38× on the shape itself). Part of that margin
  is x86-simd-sort reading 72.3 ms where it read 19.4 in a cooler run, so the
  ratio is inflated by whatever makes the opponent thermally sensitive; the
  A/B in §3s, not this row, is the evidence for the feature.
- **heavy_tail is the one loss**, 0.94×, against 1.02× in §1f. It has swung
  between 18.8 and 36.1 ms across today's runs while nothing touching it
  changed, so it is the row this box measures worst. It is the honest open
  question, and §3q already says the exponent map did not fix it.
- Everything else sits where §1f put it, within what a 5× machine wander
  allows.

## 1f. Fair fight after the September 11 package (parbench-full, 14 threads, calibration memcpy 4.2 ms, std::sort-1M 79.8 ms — about 1.2× off quiet)

After the histogram-free partition, the sample-once dispatcher, the near-sorted
rebuild, the per-route thread caps, the perfect-hash counting lookup, the
key-space certificate and the balance and duplicate guards.

Multi-threaded, all columns at 14 threads (ms):

| shape | gnu_par | **logos** | logos+SIMD leaf | x86-simd-sort | IPS⁴o | logos / x86simd |
|---|---|---|---|---|---|---|
| uniform | 84.5 | **14.8** | 12.9 | 18.6 | 28.3 | 1.26× |
| near_sorted | 51.6 | **8.2** | 8.9 | 20.0 | 18.3 | 2.44× |
| few_distinct | 26.2 | **4.7** | 4.9 | 9.1 | 5.0 | 1.94× |
| heavy_tail | 86.2 | **18.8** | 16.9 | 19.1 | 30.5 | 1.02× |
| bimodal | 86.0 | **19.2** | 18.9 | 19.4 | 30.1 | 1.01× |
| quantized | 53.9 | **7.3** | 6.5 | 12.7 | 11.9 | 1.74× |
| int_wide | 84.5 | **13.1** | 15.7 | 19.1 | 29.6 | 1.46× |
| block_perm | 55.1 | **8.6** | 8.4 | 19.9 | 21.2 | 2.31× |
| block_overlap | 55.9 | **9.8** | 9.5 | 20.3 | 19.5 | 2.07× |

**This is the first run in which logos leads both parallel opponents on all
nine shapes.** Against x86-simd-sort the margins are 1.01–1.02× on the two
that were losses in §1e (heavy_tail, bimodal), 1.26–1.94× on the random and
counting shapes, and 2.0–2.4× on near-sorted and block-permuted data; against
IPS⁴o every row, including few_distinct (4.7 vs 5.0) and near_sorted (8.2 vs
18.3), which IPS⁴o had led in §1d.

Single-threaded (ms): logos wins near_sorted (39.7 vs 82.5), quantized (32.9
vs 36.7) and the two block shapes (17.3 / 19.7 vs ~80), and loses uniform
(104.6 vs 75.7), heavy_tail (98.2 vs 75.8), bimodal (93.3 vs 77.4),
few_distinct (14.8 vs 10.1) and int_wide by a hair (78.3 vs 75.7). The
few-distinct gap, 2.6× in §1e, is now 1.47× — that is the perfect-hash
counting lookup (§3p). logos beats the LSD radix and timsort on all nine.

Caveat: std::sort-1M read 79.8 ms against a quiet ~65, so the box was about
1.2× loaded and every column paid it; read the ratios, not the times.

Rerun after the per-mode certificate (§3s) on a 1.5×-loaded box (std::sort-1M
98.4 ms), so **this table stands and the rerun is provisional**: bimodal moved
from parity to a lead, 18.6 ms against x86-simd-sort's 20.1 (1.08×, was 19.2
against 19.4), which is the direction §3s predicts. In the same run heavy_tail
read 24.1 against 18.8 here, about 1.2× beyond the run's general load factor
and outside the band it has held all day, so it is not believed either way. A
quiet rerun of this table is owed; the per-mode change cannot touch heavy_tail,
which declines the certificate at mode KS 0.589 and takes the key table
exactly as before.

## 1e. Fair fight with the AVX2 sequential leaf (parbench-full, 2026-09-11, 14 threads, calibration memcpy 4.5 ms, std::sort-1M 72 ms)

Same box and bench as §1d, after the AVX2 leaf (§3g) shipped for sequential
sorts. Single-threaded (ms):

| shape | std::sort | logos | radix | timsort | x86simd AVX2 | logos / x86simd (§1d → now) |
|---|---|---|---|---|---|---|
| uniform | 576 | 105 | 175 | 835 | **80** | 0.62 → 0.76 |
| near_sorted | 104 | **41** | 137 | 85 | 82 | 2.0 → 1.97 |
| few_distinct | 155 | 27 | 98 | 280 | **10** | 0.37 → 0.39 |
| heavy_tail | 567 | 96 | 167 | 842 | **77** | 0.55 → 0.81 |
| bimodal | 565 | 95 | 140 | 848 | **77** | 0.49 → 0.81 |
| quantized | 363 | **35** | 102 | 620 | 38 | 1.0 → 1.07 |
| int_wide | 576 | 85 | 149 | 862 | **79** | 0.87 → 0.93 |
| block_perm | 344 | **18** | 194 | 31 | 87 | 4.8 → 4.85 |
| block_overlap | 339 | **21** | 186 | 31 | 88 | 4.1 → 4.15 |

Readings: the single-threaded gap to the vectorized quicksort on random data
closed from 1.6–2.0× to 1.23–1.31×; what remains is the AVX2 leaf's own
distance from x86-simd-sort's (92–104 vs 79 ms on the same data, §3g), not
the dispatcher. logos now beats the LSD radix on all nine shapes (bimodal was
the one loss in §1d). The parallel section is unchanged within noise (logos
19.3 / 21.4 / 23.7 ms on uniform / heavy_tail / bimodal against 18.3 / 20.2 /
22.2 in §1d, x86-simd-sort 20.1 / 20.8 / 19.0), as the sequential-only gate
intends.

Re-run after the chaos package (Weyl sampling, low-discrepancy probe,
per-mode tables; calibration memcpy 3.1 ms): bimodal 14T 21.6 ms (was 23.7)
against x86-simd-sort 19.2 and IPS⁴o 30.6; every other row inside its band
(uniform 18.6, heavy_tail 21.3, int_wide 18.0; 1T uniform 101, heavy_tail
105, bimodal 100). The standard shapes never see the package's large wins,
which is the point of the zoo (§3h).

Re-run after the near-sorted route (§3j; calibration memcpy 4.9 ms, loaded):
near_sorted 14T logos 9.7 ms against x86-simd-sort 21.8 and IPS⁴o 20.4, the
row IPS⁴o had led since §1d (logos 20.4 there). Every other row inside its
band for a loaded run (uniform 22.1, heavy_tail 23.8, bimodal 20.8, blocks
9.2 / 10.1). logos now leads x86-simd-sort at 14 threads on near_sorted 2.2×,
quantized 1.7×, few_distinct 1.5× and the block shapes 2.3–2.6×, and is
within noise of it on the four random shapes.

Re-run after the histogram-free partition (§3m; LOADED run: memcpy 3.9 ms
but std::sort-1M 87 ms and gnu_par 109–175 ms against 86–91 quiet, so read
ratios within the run and remember that load flatters logos): logos 14T
uniform 17.0 ms against x86-simd-sort 22.9, int_wide 20.1 against 36.1,
near_sorted 10.2 against 24.7, heavy_tail 26.2 against 25.2, bimodal 32.7
against 25.2; IPS⁴o 27–71 on every row but few_distinct (7.2 vs 8.0). The
interleaved A/B in §3m (uniform 1.31×, int_wide 1.41× over the old partition
on the same data) is the trustworthy statement of what changed; a quiet
re-run of this table is owed (a second attempt the same evening also hit a
load burst, memcpy 7.5 ms, and was discarded; the scaling report likewise
still predates §3m).

## 1d. Fair fight at the physical core count, with textbook baselines (parbench-full, 2026-09-10, i9-13900H, 14 threads, calibration quiet: memcpy 4.1 ms, std::sort-1M 64 ms)

Same box as §1c, after every bench moved to `BENCH_THREADS` = 14 (the noise
probe's quietest count) and after the two single-threaded textbook baselines
from `bench_sorts.h` were wired in: an LSD radix sort (11-bit digits, six
passes, digit skip) and a CPython-style timsort. This is the quietest
calibration line this box has produced, so the absolute times are usable.

Single-threaded (ms; bold = best of the row):

| shape | std::sort | logos | radix | timsort | x86simd AVX2 |
|---|---|---|---|---|---|
| uniform | 660 | 128 | 188 | 854 | **79** |
| near_sorted | 108 | **41** | 139 | 87 | 83 |
| few_distinct | 156 | 27 | 96 | 281 | **10** |
| heavy_tail | 572 | 140 | 165 | 830 | **77** |
| bimodal | 558 | 158 | 137 | 843 | **77** |
| quantized | 359 | **37** | 108 | 599 | **37** |
| int_wide | 558 | 86 | 145 | 833 | **75** |
| block_perm | 323 | **17** | 184 | 27 | 81 |
| block_overlap | 333 | **21** | 186 | 28 | 87 |

Multi-threaded, all at 14 threads (ms):

| shape | gnu_par | logos | logos+SIMD leaf | x86simd MT | IPS⁴o |
|---|---|---|---|---|---|
| uniform | 88.6 | 18.3 | **16.3** | 18.3 | 30.9 |
| near_sorted | 53.1 | 19.8 | 29.4 | 19.1 | **18.7** |
| few_distinct | 27.7 | 5.8 | 6.0 | 8.1 | **4.8** |
| heavy_tail | 85.9 | 20.2 | **18.2** | 18.4 | 30.3 |
| bimodal | 85.7 | 22.2 | 20.3 | **19.0** | 30.6 |
| quantized | 56.7 | **7.4** | 7.5 | 13.0 | 12.1 |
| int_wide | 86.1 | **16.5** | 16.8 | 17.8 | 31.3 |
| block_perm | 57.3 | **8.3** | 8.5 | 20.5 | 20.2 |
| block_overlap | 57.1 | **9.2** | 9.2 | 20.6 | 20.5 |

Readings:

- **The baselines place the engine.** logos beats the plain LSD radix on 8 of
  9 shapes single-threaded (1.2× on heavy_tail, 1.5× uniform, 3.6× few_distinct,
  11× on the block shapes) and loses only bimodal (137 vs 158 ms: the radix is
  distribution-blind, and bimodal is the shape where logos's splitter tables
  resolve worst). A textbook radix is a fair floor for a "distribution-aware
  radix": everything logos gains over it is the dispatcher. Timsort is the
  other floor: 0.55-0.77× of std::sort on unstructured data, 12× on the block
  shapes, where it finds the sorted runs but has to merge them; logos's
  relocation (J5) copies them instead and is 1.3-1.6× faster still.
- **At 14 threads the machine is quieter and the margins shrink.** logos vs
  x86-simd-sort AVX2: uniform 1.00×, heavy_tail 0.91×, bimodal 0.86×, int_wide
  1.08×, near_sorted 0.96×, few_distinct 1.40×, quantized 1.76×, blocks 2.2-2.5×.
  §1c's 20-thread run (loaded 2.5×) overstated logos on the random shapes
  because a scatter-bound sort loses less to preemption than a compute-bound
  one; at the physical core count on a quiet box the honest reading is parity
  on random data, a clear win on structured data.
- **IPS⁴o wins two rows at 14 threads** (near_sorted by 6%, few_distinct by
  17%), which it never did at 20: its barrier-heavy pipeline was the one that
  suffered most from SMT preemption. logos still wins the other seven, by 1.5×
  on the random shapes and 2.2-2.5× on the block and quantized shapes.
- **Control: 20 threads on the same quiet box** (prediction: logos gains ~10%
  from SMT, the SIMD sort nothing). Wrong on both: uniform logos 18.5 ms, SIMD
  19.5 ms, IPS⁴o 31.7 ms, all within noise of the 14-thread row, and the
  heavy_tail row caught a load burst (gnu_par 154, IPS⁴o 133 ms) that 14
  threads never showed. Thread count is not what separated §1c from §1d; the
  load was. Between the loaded and the quiet run logos sped up 1.4×, the SIMD
  sort 1.7×, gnu_par 1.6× and IPS⁴o 3.5×: barrier-synchronised sorts lose more
  to preemption than logos's chunk-parallel phases, so a loaded box flatters
  logos. This is a real property (graceful degradation under load) but not a
  speed win, and every earlier parallel ratio from a loaded run is an upper
  bound on logos.
- **logos+SIMD leaf is now the best column on uniform and heavy_tail**
  (16.3 and 18.2 ms) and within noise on bimodal; it loses near_sorted by 1.5×.
  Still not the default: the win is 1.1× on three shapes and the loss is 1.5×
  on one, and it needs the external dependency.

## 1c. Full fair fight on AVX2 hardware (parbench-full, 2026-09-10, i9-13900H, 20 threads, loaded ~2.5×) — SUPERSEDED by §1d

**Read §1d first.** This run's calibration line was 2.5× off quiet, and §1d shows
the opponents lost more to that load than logos did: the "7 of 9 over the SIMD
sort, every row over IPS⁴o" below is a load artefact. Kept as the record of
what the loaded box showed and why rule 2 exists.

First run with x86-simd-sort and IPS⁴o on this box. The CPU has no AVX-512, so
x86-simd-sort runs its AVX2 kernels; on the AVX-512 box of §1 those kernels
were roughly 1.5–2× faster, so the SIMD column here is a floor for that sort,
not its best. Single-threaded columns run with OpenMP threads set to 1 (a fix:
parbench's earlier "1T" SIMD column ran with P threads).

| shape | std::sort 1T | logos 1T | x86simd 1T | gnu_par 20T | logos 20T | logos+SIMD leaf | x86simd 20T | IPS⁴o 20T |
|---|---|---|---|---|---|---|---|---|
| uniform | 1087 | 217 | **143** | 145 | **26.4** | 24.2 | 30.7 | 109 |
| near_sorted | 183 | **72** | 150 | 89 | **26.8** | 59.9 | 40.7 | 93 |
| few_distinct | 297 | 54 | **15.7** | 49 | **9.4** | 9.0 | 13.2 | 18.9 |
| heavy_tail | 1065 | 276 | **148** | 148 | 36.7 | 28.1 | **35.3** | 72 |
| bimodal | 1074 | 294 | **135** | 155 | 41.3 | 26.4 | **24.4** | 82 |
| quantized | 667 | **62** | 65 | 93 | **11.4** | 10.2 | 16.9 | 49 |
| int_wide | 1021 | **142** | 146 | 168 | **25.1** | 36.5 | 31.0 | 114 |
| block_perm | 590 | **28** | 147 | 104 | **17.5** | 12.7 | 40.8 | 63 |
| block_overlap | 619 | **38** | 145 | 102 | **12.0** | 14.5 | 36.3 | 56 |
(ms; bold = best of its section, logos vs x86simd)

Readings: at 20 threads logos beats the AVX2 SIMD sort on 7 of 9 shapes (uniform
1.16×, int_wide 1.24×, quantized 1.5×, blocks 2.3–3×), ties heavy_tail, loses
bimodal 0.59×. IPS⁴o loses to logos on every row. Single-threaded, the SIMD sort
wins the three random shapes by ~2× and few_distinct by 3.5× (its own
low-cardinality path); logos wins near_sorted, the block shapes, and ties
quantized and int_wide. The logos+SIMD-leaf column (AVX2 quicksort per bucket)
now beats plain logos on the three random shapes, reversing the AVX-512-era
finding that it was worse on 5 of 7; not shipped as default, unmeasured on
AVX-512.

## 1b. Fair fight after J5 + H0b (parbench, 2026-09-10, loaded box: calibration ~2× off quiet)

| shape | std::sort 1T | logos 1T | | gnu_par 20T | logos 20T | |
|---|---|---|---|---|---|---|
| uniform | 1093 ms | 256 | 4.27× | 197.5 | 24.3 | **8.14×** |
| near_sorted | 231 | 82 | 2.82× | 115.8 | 33.4 | **3.47×** |
| few_distinct | 189 | 50 | 3.77× | 30.7 | 8.3 | **3.71×** |
| heavy_tail | 1103 | 278 | 3.97× | 167.4 | 43.9 | **3.82×** |
| bimodal | 1062 | 314 | 3.39× | 184.6 | 54.4 | **3.39×** |
| quantized | 778 | 37 | 20.8× | 110.3 | 16.8 | **6.55×** |
| int_wide | 1032 | 108 | 9.59× | 156.8 | 31.5 | **4.97×** |
| block_perm | 581 | 33 | 17.8× | 122.5 | 17.2 | **7.13×** |
| block_overlap | 550 | 40 | 13.9× | 58.1 | 13.1 | **4.45×** |

9 wins, 0 losses, every parallel row ≥3.4× (the 3.6.0 table had every row ≥1.74×).
Absolute times are inflated by load; the within-run ratios are what to read.
The x86-simd-sort and IPS⁴o columns are not re-measured (no checkouts on this box).

## 3h. Order from chaos: the zoo A/B (2026-09-11, `make chaosbench`, calibration memcpy 3.8 ms)

Five engines in one binary (baseline = the three features off; each other
column adds exactly one), fifteen shapes, best of 5, ms:

| shape | 1T base | +weyl | +lowdisc | +modes | +netpivot | 14T base | +weyl | +lowdisc | +modes | +netpivot |
|---|---|---|---|---|---|---|---|---|---|---|
| uniform | 100.5 | 101.2 | 102.5 | 101.6 | 100.2 | 19.9 | 20.0 | 21.8 | 20.6 | 20.9 |
| heavy_tail | 98.2 | 95.7 | 97.5 | 97.5 | 97.9 | 21.3 | 20.3 | 21.2 | 22.2 | 21.4 |
| bimodal | 92.3 | 93.6 | 93.0 | 93.6 | 92.5 | 22.8 | 22.2 | 21.1 | **19.9** | 21.7 |
| near_sorted | 78.8 | 79.2 | 77.2 | 75.0 | 78.6 | 22.1 | 21.6 | 20.4 | 20.0 | 19.9 |
| logistic_map | 95.6 | 94.8 | 97.6 | 97.2 | 96.6 | 19.5 | 19.6 | 19.9 | 19.2 | 19.5 |
| lorenz_x | 100.9 | 101.3 | 99.8 | 102.6 | 101.0 | 18.8 | 19.4 | 19.1 | 18.7 | 18.7 |
| collatz | 49.2 | **39.4** | 47.4 | 47.9 | 46.8 | 13.1 | **11.1** | 11.7 | 13.4 | 12.4 |
| prime_gaps | 162.9 | 174.0 | 168.2 | 176.2 | 158.0 | 21.5 | 23.7 | 23.5 | 23.0 | 21.6 |
| weyl | 99.5 | 100.1 | **79.6** | 99.5 | 101.1 | 20.1 | 18.4 | 19.8 | 19.9 | 20.8 |
| van_der_corput | 100.3 | 101.0 | **77.9** | 101.4 | 102.4 | 176.6 | **19.2** | 164.1 | 167.0 | 175.4 |
| benford | 96.5 | 95.3 | 96.0 | 95.6 | 95.6 | 20.0 | 19.9 | 20.5 | 20.5 | 20.3 |
| brownian | 102.2 | 108.6 | 102.9 | 103.6 | 108.0 | 18.3 | 16.6 | 17.8 | 18.0 | 18.1 |
| zipf | 47.2 | 49.0 | 47.9 | 48.8 | 47.2 | 17.2 | 17.9 | 17.0 | 16.7 | 15.9 |
| jitter_ms | 106.6 | 104.8 | 104.5 | 102.6 | 100.5 | 16.8 | 17.2 | 17.1 | 17.3 | 18.0 |
| sawtooth_stride | 545.7 | 560.6 | 543.1 | 549.1 | 553.9 | 546.1 | **38.0** | 557.9 | 563.2 | 560.6 |

Graded: Weyl sampling (predicted 3–5× on the aliased shape, nothing
elsewhere) gave 14× on sawtooth_stride, an unpredicted 9× on van der Corput,
and no consistent loss (prime_gaps and brownian move inside bands that
every column shares). The collatz column's 1.18–1.25× was chased down
afterwards and is not the sampler's: both samples see ~280 distinct values;
the Weyl sample's lower maximum gave the lattice counter a narrower window
that met an out-of-window tail value 6.5 ms sooner. The real defect (the
abort itself, 5–12 ms wasted at 1T) is fixed in §3i. The low-discrepancy probe (predicted
45–60 ms on weyl) gave 78–80: coarse at 8M costs more than estimated, but
1.25–1.29× is real and it fired nowhere else. Per-mode tables (predicted
5–8% on bimodal) gave 15%. The network pivot (predicted 2–3%) gave noise.
Open: sawtooth_stride at 1 thread is 545 ms on every column because its 5376
overlapping runs go to the k-way run merge; the parallel path handles it in
38 ms. That is a dispatcher gap the standard shapes never exposed.

## 1g. Scaling after the package (scale-full, quiet: memcpy 3.7 ms, std::sort-1M 80.9 ms)

The regenerated scaling report (1e4 to 3e7, every shape, both panels) agrees
with §1f: at 10M and 14 threads logos leads x86-simd-sort and IPS⁴o on the
block shapes by 2.2× and 2.9× (12.7 ms against 28.4 and 36.9) and holds its
lead down the size ladder. The thread policy and the per-route caps put the
parallel call ahead of the single-threaded one everywhere from 1M up; five
rows at 316k remain where one thread is still faster by 0.2–0.5 ms
(few_distinct, bimodal, quantized and the two block shapes), which is the
parallel entry's residual setup with three threads, unchanged from §3k and
still not worth a third cap.

## 3t. What the reference box actually does (2026-09-11)

Every benchmark today printed a calibration line saying the machine was
loaded, however quiet it measured beforehand. It is not an external process.
Sampled immediately after the 15-minute benchmark chain and then every two
seconds while idle:

| seconds idle after the run | compute (xorshift chain) | memcpy 64 MB |
|---|---|---|
| 1 | 202.0 ms | 35.1 ms |
| 5 | 130.4 | 13.9 |
| 12 | 101.9 | 10.2 |
| 20 | 48.1 | 4.1 |
| 26 | **37.0** | **3.66** |
| 26–50 (plateau, bursty) | 37–62 | 3.7–6.0 |

Compute recovers 5.5× and memcpy 9.6× over 26 seconds, smoothly, and the two
move in lockstep (their ratios track within 0.1), so this is not core-clock
throttling alone. WSL2 exposes no temperature, no cpufreq and zero steal time,
so package power limiting and host scheduling of the VM are equally consistent
and the mechanism cannot be identified from inside.

The consequences matter more than the cause:

- A benchmark that runs for minutes spends nearly all of it in the slow state,
  so its calibration line will always read loaded. Waiting for a quiet box
  before starting one is wasted time — the idle reading does not survive.
- Absolute milliseconds are not comparable across runs, and not comparable
  across rows within a run either: the machine wanders by 5× on a timescale of
  tens of seconds. In this run's own table, x86-simd-sort read 72.3 ms on
  bimodal against 19.4 ms for the same work in an earlier run.
- What stays valid is a comparison between contenders that alternate at fine
  grain inside one row, which is exactly what parbench's speedup columns are,
  since their baseline is measured in the same row at the same moment. This is
  why the project's interleaving discipline was right, and it is now the only
  thing the numbers support.

## 3s. A certificate per mode (2026-09-11, `make tailbench`)

The question was whether bimodal data, once identified, can be merged and
treated as uniform. The profile said the target was right: bimodal at 14
threads spends 10.2 ms in the scatter against uniform's 3.5, while the leaf
phases match (11.9 against 11.1). The modes make the address calculation
harder, not the sorting.

Whether the transform works depends on something "bimodal" does not tell you:
whether each mode is uniform *inside*. Measured on a 5376-point sample, with
modes found the way the engine finds them (gaps between quantile splitters
more than ten times the median gap), worst mode's distance from uniform:

| data | modes | worst mode |
|---|---|---|
| two uniform bands (the bench's bimodal) | 2 | **0.023** |
| three uniform bands | 3 | **0.020** |
| two gaussians | 4 | 0.303 |
| three uneven clusters | 8 | 0.316 |
| wide gaussian mixture | 1 (humps overlap, no gulf) | 0.288 |

A sample certifies below 0.027. So the transform is right for piecewise-flat
data and wrong for mixtures of humps, and the engine now decides which by
testing rather than assuming.

| shape | 14T, map off | 14T, map on | layout A | layout B |
|---|---|---|---|---|
| bimodal (two uniform bands) | 21.7 | **17.3** | 1.38× | **1.25×** |
| three uniform bands | 22.2 | **18.1** | 1.33× | **1.23×** |
| two gaussians | 23.4 | 22.7 | 1.01× | 1.03× (declines: mode KS 0.164) |
| heavy_tail, uniform, quantized, near_sorted, int_wide | | | 1.00–1.07× | 1.00–1.07× (untouched) |
| every shape at 1 thread | | | 1.00–1.06× | 1.00–1.06× (the route is parallel-only: identical code) |

Layout A put the feature in copies 1 and 3, layout B in copies 0 and 2; the gain follows the feature in
both, which is the control required for any claim under 1.25× (§3r). Layout B was the quieter run
(calibration 3.2 and 3.4 ms) and is the number to quote.

Prediction and grade: I predicted bimodal's total 22.9 → about 17 ms, roughly
1.3×. Measured 21.7 → 17.3, 1.25× on the quieter layout and 1.38× on the
other. Right in both size and mechanism.

## 3ba. The leaf's kernel is decided by its ALTERNATIVE: the AVX2 quicksort steps aside for flash between 2048 and 65536, and flash starts at 2048 (2026-09-23, `make bandbench`)

The question that started it: most sorting happens at n <= 10000, so is there a brute-force method for that range
that parallelises?

**The parallel half is arithmetic, and it is not close.** Creating and joining 13 workers costs 0.564 ms (§3ay);
the whole 10,000-element sort costs 0.108 ms. Spawn alone is 5.2x the work, so even a perfect 14-way split loses
5x. `effective_threads` already caps at one thread per 100,000 elements and a probe confirms it holds exactly:
a 14-thread request against a 1-thread request reads 0.97, 1.00, 0.90, 1.00, 0.99 at 100 / 1000 / 3162 / 10000 /
31623 and first differs at **100,000** (1.16). Nothing to build; the policy is right.

**The premise was also wrong, which is worth recording.** In the scale report (uniform, one thread, ns per element)
logos is 13.6 at n=100 against `std::sort`'s 27.3, and 16.4 at 10,000 against 88.2 -- **2.0x to 5.4x ahead** across
the whole range in question. The loss is confined to n <= 32. And x86-simd-sort is the FASTEST contender at small n
(1.3 ns/element at n=32), not a slow one.

**But the ladder the question prompted found two defects.** Laddering `LOGOS_NO_AVX2` at one thread on uniform
doubles, min of 600 reps, the settings alternated across processes:

| n | AVX2 on | AVX2 off | |
|---|---|---|---|
| 512 - 3000 | 5.87 - 7.70 | 8.61 - **32.84** | AVX2 wins 1.31x, and **3.50-4.26x at 2048-3000** |
| 4096 - 32768 | 9.87 - 10.01 | 7.38 - 9.16 | **AVX2 LOSES 1.09-1.34x** |
| 65536 | 10.09 | 10.06 | tie |
| 131072 - 2M | 10.80 - 11.93 | 12.04 - 15.53 | AVX2 wins 1.11-1.38x |

**Read the columns the right way round**: the "on" column is smooth and monotone (5.87 -> 11.93); the "off" column
is the jagged one. The AVX2 kernel is well-behaved throughout. Both defects are about *what it is being compared to*,
and the code agrees with the ladder at both boundaries -- 4096 is `FLASH_SORT_THRESHOLD`, 65536 is `TYPED_MIN_N`:

| range | reached by | the alternative | winner |
|---|---|---|---|
| n < 4096 | `sort_fn` | the scalar `quicksort` at the bottom of `sort_fn` | AVX2 |
| 4096 .. ~65536 | `sort_fn` | `flash_sort` | **flash** |
| above | `leaf_f64` (its own gate) | `coarse_bits_f64` | AVX2 |

`sort_fn`'s gate sat IN FRONT of the flash/coarse decision, so it took the whole middle row. It now fires only where
flash is not the alternative, with a second copy on the coarse branch after flash has declined, bounded above by a new
`AVX2_FLASH_MAX_N` = 65536. And below 4096 there is no flash at all, so the fall-through is that scalar quicksort,
which has a cliff: 2048 and 3000 elements cost 27.5 and 36.3 ns each against 6.3 and 6.7 through flash.

**Measured** (`make bandbench`: one binary, two engine copies, 80 alternating blocks, built twice with the copies
swapped so layout cancels; one thread). The order, in the shipped configuration (flash available from 2048):
**2048 1.303, 3000 1.227, 4096 1.185, 6000 1.133**, 8192 1.037; guards 512 0.979, 1024 0.992, 131072 1.008, 2M 0.988.
(Measured before the threshold change, with flash from 4096, the same rows read 4096 1.237 and 6000 1.169.)
**16384 to 65536 is NEUTRAL and is not claimed**: two runs straddle one -- 1.005-1.040 in the first and 0.964-0.979 in
the second, with the orientations agreeing inside each run, which is drift between runs and not an effect. The window
keeps its top at 65536 because that is where the STRUCTURE changes (above `TYPED_MIN_N` a range reaches `sort_fn` only
through the skew branch) and because 131072 is a measured loss for flash -- a structural bound that measures neutral,
not a curve fitted to the wins. The threshold 4096 -> 2048: **2048 1.286** (orientations 1.283 /
1.289), **3000 1.352** (1.322 / 1.382), everything else 0.909-1.016; with `LOGOS_NO_AVX2=1`, **2048 4.583 and 3000
5.526** with every other row 0.986-1.016 -- so that cliff is live on any build without AVX2+FMA and on every non-x86
target, where it is a 4-6x penalty.

**Four predictions graded, three wrong.**
1. *The 14-thread small-n dip is OpenMP spin-wait* (`__gnu_parallel::sort` runs first in the row and libgomp busy-waits
   after a parallel region). WRONG: measured directly against `OMP_WAIT_POLICY=passive`, 0.87-1.06. The dip was also
   misread to begin with -- the report reads 0.69-1.11, not the 0.32 that started the chase.
2. *Skipping AVX2 wherever flash is chosen.* WRONG at the top: 131072 read **0.914** on the ladder and **1.123** in
   callbench, independently. Ranges that large reach `sort_fn` through `leaf_f64`'s skew branch, where flash is still
   selected below `COARSE_MIN_N` (500000). A window needs BOTH ends measured.
3. *`FLASH_SORT_THRESHOLD = 512`, the obvious floor.* WRONG: **0.795 at n = 512** and 0.939 at 1024 with the AVX2 leaf
   on, in both orientations. A flash sort needs the elements to amortise its classification pass. 2048 is the crossover.
4. *The band A/B, at 1.18-1.34x.* Right in size and in mechanism (1.24x at 4096, 1.17x at 6000).

**The methodology point, which cost the most time.** `make callbench` narrowed to the band read **0.989-1.017 on every
cell** and would have closed the question. Its "random" shape is `(double)(nr() >> 11)` -- integer-valued doubles
spanning 2^53, costing 25.7 ns per element at n = 4096 where uniform doubles cost 8.3, i.e. it never takes these paths;
and its default ladder steps 1000 -> 5000 -> 50000, straight over the window. **A bench that does not reach the code
cannot clear it.** Check a row's absolute cost against the path you believe it took before trusting its flatness.
`bandbench.c` exists for that reason rather than a wider `SIZES`, and it pairs any two leaf settings at the small end.

**Verified.** `make test` green on all three settings (shipped, `LOGOS_AVX2_OVER_FLASH=1`, `LOGOS_NO_AVX2=1`),
2,258-2,260 in-process resource checks each with ~1,992 injected allocation failures. `make sanitize` clean, and its
four `nan is outside the range` reports in `flash_sort` / `coarse_bucket_sort` / `skew_probe` are **exactly the four the
committed engine produces**, so sending more data to flash did not widen 0h (e). `make callbench` against the committed
engine: every cancelled cell 0.933-1.040. Eleven flag variants compile; `psort_f64`'s frame is 42,576 bytes, unchanged.

## 3az. Knowing the data FORMAT: nothing for numeric timestamps, 4.9-8.5x for date TEXT (2026-09-22, `make dateprobe`, `make datestrprobe`)

The proposal: detect that the values are dates and exploit the format, not just the distribution. The engine
receives a bare `double *`, so "these are dates" is a DATA fact and must be certified like any other -- but a
format would hand over the STEP and the ORIGIN for free, which is what the int32 type handed `count_try_i32`
when it went 37.6 -> 3.8 ms. The dispatcher reads as though there is a hole: for integer-valued doubles it tries
a counting sort when `span <= COUNTING_SORT_K * n`, then the int32 radix when the values fit +-2.1e9, then falls
through to the general leaf. Epoch SECONDS over a day (span 86,400) clears the first gate; epoch MILLISECONDS
over the same day (span 86.4M = 10.3n, and 1.76e12 is outside int32) clears neither. Same data, different unit.

**Measured against a sort TOLD the origin and the step** (subtract, divide, LSD-radix only the bytes that vary),
8.4M values, one thread:

| shape | logos 1T | logos 14T | std::sort | told 1T | told/logos |
|---|---|---|---|---|---|
| epoch_sec_day (1 s, 1 day) | 306.5 | 49.5 | 1901.1 | 655.7 | 0.47 |
| epoch_ms_day (1 ms, 1 day) | 362.7 | 50.2 | 1928.7 | 468.9 | **0.77** |
| epoch_ms_hour (1 ms, 1 hour) | 216.0 | 30.5 | 1645.0 | 416.6 | 0.52 |
| epoch_us_min (1 us, 1 min) | 213.4 | 42.4 | 1593.1 | 467.0 | 0.46 |
| day_serial (1 day, 10 yr) | 85.8 | 16.9 | 1021.1 | 257.1 | 0.33 |
| packed_ymd (YYYYMMDD) | 38.9 | 13.1 | 447.2 | 163.5 | 0.24 |
| excel_serial (day + fraction) | -- | -- | -- | **WRONG OUTPUT** | -- |

**Every ratio is below 1: the told sort is 1.3-4.2x SLOWER than the engine.** The prediction was 2.0-4.0x
AVAILABLE on epoch_ms_day; it measured 0.77. And the format approach is not merely slower on one row -- it is
WRONG on `excel_serial`, because a day-plus-fraction at 1/86400 has no exact integer representation, so the
round trip does not return a permutation.

The trace says why the hole is not a hole (`./datetrace`): the high-resolution shapes all take the general
partition (`chao1` 74k to 2.1M, so the counting routes correctly decline) and the partition handles them well
(leaf imbalance 1.01-1.04x); `day_serial`, at 3,650 distinct, takes the LATTICE route at both thread counts.
So the engine already exploits every exploitable part of being a date -- quantization and low cardinality --
by certifying them, and for millisecond timestamps there is nothing left to exploit: 8.4M values over 86.4M
slots is dense numeric data, and the label does not reduce its information content.

**That bounds the whole idea.** The most format knowledge can save is the DETECTION, and detection is the shared
sample at 1-4% of a parallel call -- under the resolving floor, which is why S7 (stopping the sample early) is
parked for the same reason.

**Where it does pay: when the dates are TEXT.** A fixed-width ISO-8601 timestamp has a monotone map to an
integer that the format hands over, and lexicographic order on ISO-8601 IS chronological order, so comparing 24
characters per comparison is waste. 2M records, one thread: `std::sort` on `std::string` 1724 ms, `std::sort` on
fixed 24-byte records with `memcmp` 989 ms, and parse-to-key + `logos_argsort_f64` + one gather **203 ms**
(parse 26.3 + argsort 129.2 + gather 47.8) -- **8.48x and 4.87x**, which is inside the predicted 8-25x and 3-8x.
The first cut placed the records with `std::sort` on indices and read only 2.67x, with placement 618 of 702 ms:
**the format buys the KEY, and the key is worth nothing without a fast argsort behind it.**

The general statement, which is not about dates: **a format is worth what it saves in COMPARISON WIDTH, not in
distribution knowledge.** For numbers the engine already infers the distribution and the width is already 8
bytes, so there is nothing to give. For text the format collapses a 24-byte comparison to an 8-byte one, and
that is the whole win.

## 3ay. Where the lattice route's idle threads are: thread CREATION and a bandwidth-bound emit, not the serial merge (2026-09-22, `make profile` with four lattice phases, `make spawnprobe`)

Open direction 1 asked for one thing: finer profiler phases, because the route was ONE phase and an occupancy
average cannot say which part of it the idle threads are in. The route is now split into map / merge / emit /
teardown (PROF-only), and the answer is not what any of the three standing suspects predicted.

**First, the entry's premise was stale.** It recorded 6.0-6.9 of 14 threads busy. Re-measured today, after this
session shipped per-worker tables and the map chunk queue, the lumped route reads **8.42 (quantized) and 8.93
(mod_card_30k)**; few_distinct, on the hash route, is 5.69 and is now the worse case. In every cell all 14 threads
do work, **none** wait at a barrier, and cpu/busy is 0.99-1.00, so nothing is preempted.

**Split, at 14 threads:**

| phase | quantized wall | share | concurrency | mod_card_30k | concurrency |
|---|---|---|---|---|---|
| lattice map | 4.482 ms | 68% | **10.72** | 5.567 ms | **11.32** |
| lattice merge | 0.063 ms | 1.0% | serial | 0.494 ms | serial |
| lattice emit | 1.843 ms | 28% | **3.17** | 2.371 ms | **3.03** |
| lattice teardown | 0.009 ms | 0.14% | serial | 0.014 ms | serial |

**Two suspects died here, and one of them was mine.** The serial merge is **1.0-5.8%** of the route, not the ~55%
the occupancy average once implied nor the ~10% the earlier trace put it at. The teardown -- freeing 14 tables of
16 MB, which a trace build timed at 2 ms and which I was about to attack with the int32 route's persistent-pool fix
-- is **9 to 14 MICROseconds, 0.14%**. That 2 ms was an artifact of the trace build. **A trace build says which path
ran and what share each phase took; it does not price a phase against a normal build, and I nearly shipped a fix
for 0.14% of a route on the strength of one.**

**The emit is where the threads are idle, and it is not an imbalance.** Every thread is handed exactly n/P output
positions, so the work is equal by construction. A faithful standalone replica (`make spawnprobe`: 14 threads each
filling n/14 doubles into a pre-faulted array) reproduces the phase at 1.597 ms against the engine's 1.843, and
splits it:

- create + join 13 workers with **no work at all**: **0.564 ms, 35% of the phase**
- the same fill on ONE thread: 4.314 ms, so 14 threads buy only **2.7x**

67 MB of stores in 1.03 ms is 65 GB/s, which is the DDR5 limit on this box. So the emit is **memory-bandwidth
bound**: there is no more parallelism in it to extract, and better chunking cannot help. What CAN be removed is the
0.564 ms of spawn and join, which the lattice route pays TWICE (map and emit) -- about **1.13 ms of a 6.58 ms
call, 17%**. That is open direction 0h (a), where a persistent worker pool is already noted as a change to two
functions (`par_run` / `par_spawn`) rather than to 23 sites, and this is the first per-phase number that prices it.

**What is left open on this route:** the map at concurrency 10.72-11.32 of 14 still leaves ~20%, and its inner loop
touches TWO 400 KB arrays at the same random index per element (`keys[idx]` to verify, then `cnts[idx]++`), which
is two cache lines where an interleaved 16-byte record would be one. That is a separate, untested lever on the
phase that is 68% of the route.

## 3ax. Branchless sorting networks for 4 to 16 elements: 1.03x at n = 4, 1.58x at n = 16 (2026-09-22, `make callbench`)

The leaf finished every short range with insertion sort. On unstructured data of a dozen elements that is about n^2/4
comparisons of which roughly half mispredict, and a mispredict is ~15 cycles where a compare is one. A Batcher odd-even
mergesort network for each size from 4 to 16 replaces the branches with straight-line compare-exchanges.

**The networks are generated, and proved before they are written.** `gen_small_net.py` emits one straight-line network
per size (424 comparators in all) and refuses to write any network that fails the 0-1 principle -- a comparator network
sorts every input if and only if it sorts every sequence of zeros and ones, so 2^n binary inputs per size is a complete
proof. The compare-exchange is one predicate and two selects rather than a min and a max, because `min(x,y)` and
`max(x,y)` both return the same operand when one side is a NaN and LOSE the other, which would break the permutation
`logos_sort_f64` promises. **Read from the assembly**, at the engine's own flags: 424 `vcmpltsd` + 424 `vblendvpd` +
424 `vmaxsd`, and exactly ONE branch in the whole function -- the jump table that selects the size. `net_sort_small` is
`noinline` and its frame is 8 bytes, so it adds nothing to the dispatcher's stack-probed prologue.

**Measured** (`make callbench`, two binaries with the copies swapped so layout cancels, two runs; work/base, below 1 is
the win):

| random n | 3 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 | 24 |
|---|---|---|---|---|---|---|---|---|---|---|
| run 1 | 0.981 | 0.968 | 0.943 | 0.817 | 0.750 | 0.748 | 0.722 | **0.631** | 0.996 | 0.995 |
| run 2 | 0.992 | 0.941 | 0.918 | 0.810 | 0.753 | 0.750 | 0.716 | **0.635** | 0.995 | 0.991 |

So **1.03-1.06x at n = 4 rising monotonically to 1.57-1.58x at n = 16**, and the two sizes just outside the range
(18 and 24) read 0.99 in both runs -- the control that says the effect is where the code changed and nowhere else.
Every SORTED row is 0.97-1.04 at every size, as predicted: the prescan returns a sorted array before the leaf is
reached, so a network never runs on one. The rows at 1000, 5000 and 50000 are 0.98-1.02, because a large sort takes the
flash, coarse or AVX2 leaf and the scalar quicksort base case barely runs. **This is a small-end result (open direction
0g), not a result at scale, and it should not be quoted as one.**

**Grading the prediction, which was written first and was half wrong.** It said 1.5-3x across 4-16. The top of the range
landed (1.58x at 16); the bottom did not (1.03x at 4). The error was treating the range as uniform when the effect is
the ratio of comparators to mispredicted branches, and that grows with n. The stop rule inherited the same flaw -- one
bar ("clear 1.15") across a range where the effect obviously scales -- so it is met from n = 8 up and missed at n = 4-7.
It ships anyway because nothing regresses and every size in range improves in both runs and both orientations, and the
miss is recorded here rather than quietly rounded away.

**A process failure worth more than the result.** The first correctness run reported "all correctness checks passed" for
both settings from a STALE harness: the verification script sent `make` output to `/dev/null`, the new harness section
failed to compile (a heredoc turned the `\n` in two `printf` strings into real newlines), and `make` left the previous
binary in place. The tell was a timestamp -- the binary predated the source by six minutes -- and the section was
reporting nothing because it was not in the binary at all. Built properly it is **368 checks, 0 failures**, covering
n = 2 to 24 past both edges of the network range, with shapes the rest of the harness never sorts at that size: a NaN,
two NaNs, signed zeros only, infinities with a subnormal, and heavy ties. **Never send a build's output to `/dev/null`
in a script that then claims the tests passed.**

**And the frame check found the mechanism for an older open item.** CLAUDE.md prints the dispatcher frame every round
for a reason. At the engine's own flags (`-O3 -fPIC`, no `-march`), `psort_f64` is **36,640 bytes in the committed
engine and 42,576 in the working tree**. Stack-clash protection probes every 4096-byte page of the frame on EVERY call,
so that is 9 probes against 11 -- two extra probes on a sorted call that costs about 17 ns. Open direction 0g records
an untraced drift of exactly that size: sorted calls of 3 to 24 elements reading 1.08-1.14 against the committed
engine. **This round is not the cause** (its own callbench rows are 0.97-1.04 sorted and 1.00 at n <= 3, and
`net_sort_small` is `noinline` with an 8-byte frame); the growth is cumulative, from the routes this session added to
the dispatcher -- `nearsorted_parallel_f64` went 8,656 to 33,056 bytes and `reach_from_sample` at 32,848 is new. No
large callee disappeared between the two builds, so nothing newly inlined: `psort_f64` simply carries more locals.
The fix 0g already names is unchanged and now worth more: a THIN FRONT, with the prescan and insertion sort in a
function with a small frame and the large body behind a call.

**Two measurement traps, both mine, both worth the space.** `-fstack-usage` writes its `.su` next to the `-o` target,
so compiling to `/dev/null` writes nothing at all and compiling to `/tmp/x.o` leaves a STALE `logos_psort.su` in the
source directory -- which is what I read first, and it happened to be a `-march=native` build, so the number looked
plausible and was from the wrong compilation. And the frame must be measured at the ENGINE's flags: `-march=native`
changes inlining and gives a different answer.

## 3aw. The threads on this box are not equal: a work queue pays on a COMPUTE phase and measures nothing on a MEMCPY phase (2026-09-22, `make capbench` with CAP_SWITCH)

The hypothesis came from the hardware. This box is an i9-13900H: 6 performance cores and 8 efficient ones. The engine
hands every thread an equal slice (`lo = t*n/P`), so a phase ends when the SLOWEST core does, and `make profile` showed
that shape -- 6.0-6.9 of 14 threads busy inside the lattice map with NONE waiting (a finished thread is neither), and
6.6 busy with 3.6 WAITING at relocation's barrier. **The kill test came first, and from profile data already on disk**:
per-thread busy inside the statically chunked phases is BIMODAL (quantized lattice map 4.40 against 2.99 ms, a ratio of
1.47; mod_card 1.44; the few_distinct hash map 2.09; relocation tiles 1.39) where the queue-driven leaf reads 1.04. Six
fast cores carrying a phase is what that looks like.

Two sites then got the same treatment -- claim the next chunk from an atomic counter instead of taking a slice -- and
they measured opposite results.

**The lattice MAP phase: 0.90-0.97 on 12 of 12 cells at 4M and up, both runs. SHIPPED.**

| 60 pairs, t(queue)/t(static), below 1 is the win | 1M | 2M | 4M | 8M | 16M |
|---|---|---|---|---|---|
| quantized (lattice) | 1.03 / 0.99 | 0.95 / 0.96 | 0.95 / 0.90 | 0.95 / 0.93 | 0.96 / 0.97 |
| mod_card_30k (lattice) | 1.00 / 0.99 | 0.95 / 0.99 | 0.90 / 0.92 | 0.95 / 0.95 | 0.97 / 0.97 |
| few_distinct (hash route, untouched) | 0.98 / 0.99 | 0.97 / 1.03 | 1.00 / 0.97 | 1.02 / 1.00 | 1.01 / 0.99 |

A NULL switch on the same rows in the same build reads 0.97-1.04 and never falls below 0.97, so the separation is real
at about **1.05x**. Chunks are n / 4P, never under 4096; a worker whose thread was never created simply never claims and
the rest drain the queue, which is strictly safer than the slice it replaced. The prediction was 0.75-0.90 -- too
optimistic by about half, the second round running that this knob has been overestimated.

**Relocation output tiles (phases 1 and 3): 0.96-1.04 on every cell of both runs. REVERTED.**
Predicted 0.88-0.97 at 4M and up, from the same bimodal evidence plus the 3.6 threads measured waiting. Measured
block_perm 1.00 0.98 1.00 1.00 1.01 and 1.02 0.99 1.04 1.00 1.01; block_overlap 1.02 0.98 0.98 1.01 1.01 and 1.00 0.99
1.01 0.99 1.00. Four tiles per thread, never under 32768 elements, the first overlapping run found by binary search (it
had been a scan from run 0, O(R) per tile, affordable for one tile per thread but not for a queue of them). Correct
throughout -- the harness passes on both settings, and faultprobe injects 1,926 allocation failures and 48
thread-creation failures with 0 bad, reaching `reloc_worker` itself once the knot route is switched off -- and worth
nothing.

**Why the same change wins in one place and not the other, which is the actual result.** The lattice map is COMPUTE:
every element is hashed or indexed into that thread's own private table, so a faster core really does finish more
elements and equalising the work equalises the finish. Relocation's phases 1 and 3 are `memcpy` over disjoint ranges,
where the bottleneck is one shared memory controller and not the cores, so handing a fast core extra tiles cannot raise
the aggregate. That agrees with the three earlier memory-system results in the ledger (ping-pong, huge pages and
streaming stores, all measured zero). **The generalisation: a work queue buys balance only where the CORES are the
bottleneck; on a bandwidth-bound phase the slices were never the problem.** It also says, without another round, that
the knot worker's A1 and C phases -- the same static tiles over the same kind of copy -- are not worth converting.

And the negative moves the target. If equalising the tiles changes nothing, the 3.6 threads the profile shows waiting
are not waiting for a slow tile. They are at the SECOND barrier, where phase 2 merges overlapping chains: that phase is
already queue-driven, but one long chain is a single indivisible task, so one thread merges it while thirteen wait.
Relocation's serial middle, already listed as open, wants a merge-path split of ONE chain -- not better tiling.

## 3av. The lattice route: the merge was not the problem, the table setup was (2026-09-22, `make capbench` with CAP_SWITCH)

The Amdahl round said the counting routes leave 7-8 of 14 threads with nothing to do and blamed the serial merge between
their two parallel phases. Building that merge in parallel settled it: it is CORRECT (harness, sanitizers, TSan and 546
injected allocation failures all clean) and **1.02-1.12x slower**, because the trace puts the merge at about 2 ms of a
route whose map phase is 11.6-14.1 ms. The merge is ~10% of the route, Amdahl caps the prize there, and an extra spawn
plus a second pass over the slots eat it. The lesson is about the instrument: a thread-occupancy average says how many
threads are busy, not which phase they are busy in, and inferring a phase's share from it is not the same as timing it.

What did pay, on the same route, was the phase the trace actually named: the caller's setup loop allocated all P tables
and memset `slots` keys for each of them before the spawn — 5.6 MB of serial writes at 8M and 14 threads, inside the
phase that is ~80% of the route. Each worker now takes its own table. Measured over 60 pairs and two runs at 1M-16M,
twenty of twenty cells land on the winning side, centred on 0.97 against a null switch centred on 1.00, and the trace
confirms the mechanism (map phase 8.8-11.3 ms against 10.9-13.8). It ships at about 1.03x on the lattice shapes. The
prediction was 0.83-0.95, so it was too optimistic by half; the stop rule's bar was missed and the change was shipped on
the mechanism plus the null control, which is recorded so the judgement can be argued with.

## 3au. Where the parallel speedup stops: the serial merge, the static tiles, and two remedies that did not pay (2026-09-22, `make amprobe`, `make capbench`, `make profile`)

Amdahl's law bounds a parallel sort by its serial fraction, so the question is which fraction each route actually has.
Two instruments agree at 8M and 14 threads. The partition is healthy: 13.0 of 14 threads busy in the leaf, 11.3 in the
scatter, a Karp-Flatt serial fraction of 0.055-0.09 from three threads up, and perfect packing would buy only ~1.16x.
The counting and lattice routes spend 98-99% of the wall in one phase where 6.0-6.9 of 14 threads are busy and NONE are
waiting: the rest have no work, because a serial merge sits between two parallel phases and grows with P (the lattice
merge walks slots x P), which is why e rises 0.06 -> 0.23 as P goes 2 -> 14. Relocation has 3.6 of 14 threads WAITING at
a barrier, from static output tiles. And kinked data sorts fastest on ONE thread (6.84 ms) at every thread count: 15.6
at 2, 7.2 at 14.

Two remedies were built and measured, and both were rejected by their own stop rules. A thread CEILING for the counting
routes: no global value passes, because the lattice map phase is real parallel work (a ceiling of 3 costs it 0.53-0.87,
6 and 8 cost 0.72-0.90 at 8M and 16M) while the hash route stops scaling near 8; a hash-only ceiling then read about 1.0
at 4M and 1.1 at 8M, inside the resolving floor. A GATE against the near-sorted route at low thread counts: the route is
worth 1.80-1.97x at two threads on the shapes it was built for, and switching it off helps only the kinks shape (1.18x
at 2 threads) while costing 1.10x at 14. The lesson is the useful part: idle threads are not fixed by withholding
threads. The only remedy the profile endorses is to give those threads work -- a parallel merge for the counting routes
(0a) and a task queue for relocation's placement.

## 3at. Uniform data as a complete computation: the chunks are cheap, the merge is not (2026-09-22, `make unibench`)

The proposal: certify the distribution from the sample, then sort P chunks with a parallel SIMD quicksort and merge them,
instead of partitioning. Built with x86-simd-sort's AVX2 kernel per chunk and a bottom-up tree whose every merge is split
by merge path, so no level runs on one thread. At 8M and 14 threads the chunk sorts alone run at **0.76-0.86 of the
shipped engine's whole time** — the premise is right — and the merge then adds about three times the engine's runtime,
putting the pipeline at 3.3-4.2x on every shape including uniform. The mechanism is measured rather than argued: the tree
moves 3.86n on every shape, and that same movement costs 15-18 ms when neighbouring chunks hold disjoint value ranges
(near_sorted, block_perm: std::merge degenerates to a copy) against 87 ms when every chunk spans the whole range (uniform:
every output element comes from a comparison). A merge on uniform data costs 5.8x what moving the same bytes costs. The
engine's answer on the same certificate is one multiply-add per element into an equi-width grid with no comparisons, which
is why it wins. One defect found on the way, by the output check and not by the clock: `logos_sort_f64` may not be called
concurrently from several threads, because the context pool and bundle table are plain globals.

## 3as. The handoff pipeline: merge order decides, and the specified micro-kink never fires (2026-09-22; implementation REMOVED 2026-09-23, in git history before that commit -- it was `make tsbench` modes `moebius`, `moebanat`)

A handoff specification of "what remains to implement" in a Moebius hybrid: bail out under 16, swap 2-3 element dips inline, stitch the
runs with a galloping merge behind an O(1) boundary check. Built literally and in a repaired form, and paired with the engine at 1M: the
literal form is behind on 14 of 16 shapes, the repaired on 13. Three measurements worth keeping. **The micro-kink branch never fires on
the shape it targets**: a dip's first element belongs to the preceding ascending run, so the descending run is one element, never two or
three; the engine's own repair asks both neighbours instead and swaps the pair back inside the run. **The bail-out as written is a no-op**,
since it sorts a run the scan has just proved sorted; as Timsort's minrun it is worth 2x on noisy data. **Merge order is the whole
difference**: the same runs and the same merge kernel cost 100,999 merged elements per element stitched left to right into a growing
prefix (58-68 s on a ring buffer) and 4.6 merged bottom-up (24 ms). And the O(1) boundary check skipped 0.0-1.6% of boundaries, which
is the third independent measurement of that idea in this bench. What it found: the run detector's block of eight was scalar compares
(read from the disassembly), and with vector ones one kinked strand sorts 1.30-1.32x faster at one thread and shuffled blocks
1.27-1.30x. What is still open: eight sorted logs concatenated, where the pipeline reads 0.80-0.86 over three runs.

## 3ar. The triple-reversal weld: an O(1) test in front of an O(prefix) merge (2026-09-22; implementation REMOVED 2026-09-23, in git history before that commit -- it was `make tsbench` modes `weld`, `weldanat`)

An outside pipeline: condense into runs, sort the runs by their min edge, move them into that order in place by triple reversals, and
weld the seams left to right, skipping a seam when the left max is at most the right min. Built as proposed (the rotations scheduled
by a rotation merge over whole blocks so they cost O(n log^2 K) at worst) and paired with the engine at 1M: faster on a sorted array
and on one kinked strand, slower on the other fourteen shapes by 2x to 4,600x, and 2.6-368x behind at 14 threads. The boundary test
bypassed none of its seams on any shape. A maximal run swallows the next block whenever that block starts higher, so a unit spans a
hole; the prefix's maximum reaches the top of the range within a few units; and from then on every seam fails the test and merges
into most of the prefix. The test is O(1), the merge behind it is not: 1,720 merged elements per element on a late batch, 98,000 on
uniform data. The triple reversals themselves are correct and in O(1) space, and cost 1.2-3x against a gather on the block shapes.
What the rows it won found: the dispatcher's prescan, commented as vectorizable, was a scalar compare chain at the engine's flags
(vector compares: sorted 1.7-2.2x), and the run detector walked eight scalar steps after every wrong-way step (a jump to the step:
kinks 1.05-1.08x). After both, the weld reads 1.93 against the engine on sorted and 0.94 on kinks.

## 3aq. A trend under the wave: collected when the wave is clean, not when it is noisy (2026-09-22, `make sineprobe` mode `tilt`)

The engine has no trend model; it sees a trend as runs that overlap less. On noise-free waves that is enough: as a linear trend grows
from nothing to the slope at which the wave stops falling, the cost falls from about 1.0 of uniform to 0.2 at one thread, and past
that point the array is sorted. The user's line from the wave's minimum to its maximum moved routes rather than structure: it gave a
crest-started wave a ninth run (0.55 of uniform, past the few-runs knot gate) and took the exact repeats out of a 32-cycle wave (1.2).
With noise of +-50 the trend bought nothing, and three routes misfired on the upper half of the ladder: the wave route re-entered the
whole dispatcher for each of 65 pieces the short-reach route declined (1.42x at one thread); at 14 threads the partition's
uniformity certificate passed a grid whose buckets overflowed their slack (631,108 spills, a pass thrown away, 1.39-1.54x); and the
near-sorted route accepted 21% strays (1.40-1.48x). The engine's min-to-max interpolation is its uniform grid, and on a tilted wave
that is the piece that fails: the wave bunches values where the curve flattens, and a KS distance of 1% of n does not bound one
bucket's count within the 60% the regions allow.

## 3ap. cos() in place of sin(): the phase costs nothing; a zero at a[0] and the few-runs knot gate do (2026-09-22, `make sineprobe` modes `cos`, `fewgate`, `legs`)

A cosine with the same slope is the sine moved a quarter period, and at one thread it sorts the same (0.96-1.00, same routes). At 14
threads it did not, and the phase was not why: a sloped sine starts at zero, so the always-sampled a[0] and a dozen values near it
are tiny or negative, and the partition's key grid, which works in total-order key space where the neighbourhood of zero spans
hundreds of binades, went coarse and was rejected. The scatter fell to the value table, and the cosine, which starts at its crest,
was 0.85-0.88 of the sine's time. A wave with a cosine descending leg and a sine ascending leg shows it without a slope: the order
that starts at exactly 0.0 reads 1.07-1.17 at 14 threads. One value is enough.

Without a slope the ends set the run count. A four-cycle cosine has eight runs and the sine nine, and the knot route's cost gate is
three times stricter at eight runs or fewer, on the assumption that the run merge is the fallback there. On these arrays it is the
leaf: the cosine cost 1.66-1.72x, and lifting the gate on noise-free waves of 3 to 8 overlapping runs is worth 1.6-2.9x. Neither
behaviour was changed; both are open directions with their measurements.

## 3ao. The edge sort: sorting blocks by their min edge feeds nothing; its three wins were three engine defects (2026-09-22, `make tsbench` modes `edge`, `edgeanat`)

A proposal: condense the array into natural runs (twisting the descending ones), sort the K runs by their first element, lay them out
in that order, and hand the result, "an ascending sine wave", to the boundary-bypass merge sort of §3an. Built exactly so, with a
trimmed bottom-up merge as its strongest form, on sixteen timestamp shapes. On every shape with noise in it K was 41% of N (runs of
2.4), the block sort cost 15-47% of the pipeline, the rearranged array still left elements 38 to 6,608 positions from home, and the
hybrid still merged every element 18 times: the steps made it slower on 15 of 16 shapes. The idea's premise fails twice. A run's min
edge describes the run only if the run stops where the data's blocks stop; a maximal run swallows the next block's first element, and
a sensor dropout pairs with a real reading, so sorting by min edge moves whole blocks, or real readings, to the wrong end. The
engine's relocation plan sorts the same descriptors, but only after splitting runs at gaps (J5's first lesson), and places each
element once instead of laying the blocks out and merging again.

Three rows lost to the proposal, and each was a defect in the engine, not a strength of the idea. On a sorted array the engine took
2.2x a plain scan: its prescan, which is the whole sort there, tested twice per element; it now reads sixteen at a time once the
direction is known (2.2x on sorted, 1.4x on reversed, 1.3-1.4x on a late batch). On one kinked strand the engine spent 20.7 of 34 ms
putting back 820 strays through three full passes; they now go back by one memmove per segment, the short-reach route's method (2.4x).
And a tie ended a descending run, as a stable sort's must, though nothing here is stable: timestamps with 1.25% ties cut every
descending block into pieces under the relocation floor (6.0x at one thread, 1.3x at 14). After the fixes, against edge+trim:
0.94-0.95 (was 0.44) on sorted, 1.39-1.43 (was 0.59-0.60) on kinks and 2.73-2.79 (was 0.40-0.41) on the tied blocks.

This round's callbench, run against the committed engine as well, found a regression none of its own switches caused: every call of
2 to 10 elements cost 4-5x the committed engine's. The dispatcher's frame had grown from 35.9 KB to 60.5 KB with no stack array
added, because GCC had begun inlining the near-sorted route (a 33 KB frame, called once), and stack-clash protection probes every
page of the frame on every call. Kept out of line with the stream route, the frame is 28.3 KB and a sorted pair costs 19 ns again,
not 85. An inlining decision is not a property of the code one edits: print the frame after every round.

## 3an. Server timestamps with slight knots: the boundary bypass never fires on latency; one strand and one seam fixed (2026-09-22, `make tsbench`)

An outside write-up credited a "Moebius hybrid" (reverse the small descending runs, then a merge sort that skips any merge whose halves
are already in order) with O(N) sorting of data that is "macroscopically ascending but locally noisy", naming sensor telemetry and log
merging. Built exactly as described and run on thirteen timestamp shapes, its O(1) check skipped **none** of 262,143 merges on any shape
with network latency in it: the jitter reaches farther than the 32-element blocks the recursion bottoms out at, so no two adjacent halves
are ever in order, and the hybrid merged every element 18 times, level with std::sort. It skipped every merge on sorted data and on
adjacent swaps, the one shape where the reversal is the whole job.

The engine beat the hybrid on 12 of 13 shapes (3-6x on latency, 3.5x on batches, 8x on noise). The thirteenth found a gap: one sorted
strand with 1% adjacent swaps lost 9x to the hybrid and 1.1x to std::sort, because the detector repaired the kinks, lifted the tangled
ones as strays, left one run, and relocation declines a single run. Finishing that case (sorted, or strays merged back) made it 5.5x
faster; the hybrid still leads there by 1.6x, two passes against five. A second gap, at 14 threads: the short-reach route sorted every
block of a noisy ring buffer before seeing the seam; its fences now decline first (0.74 -> 0.94 against the order routes off).

What the order-reading routes buy on server timestamps, measured as the engine with and without them: 1.4-1.9x at one thread on
every latency shape and 5x on kinks, batches and late batches; at 14 threads 1.3-1.7x on jitter, skew, kinks and batches, and nothing
on the shapes whose strays make the short-reach route decline. Millisecond clocks are the weak row (1.6x over std::sort): 84k distinct
values on a 1 ms lattice that no counting route takes.

## 3am. Round six: strays lifted out of strands, a wave cut into its pieces, a seam in the fences (2026-09-22, `make unknotprobe`)

Asked where the slowdowns still are and what else the knot view offers, six ideas were built and paired against their own
switches. Three shipped, all for sequential sorts. **Strays inside strands**: the run detector lifts late records out of runs in
five forms (the last found because a run that STARTS with a stray has no body for the gap test, the unknotting probe's Culprit in a
new place), closes the holes in one pass, and merges the sorted strays back in place of the copy-back: blocks with 0.1% strays 4.0x,
1% 3.0-3.3x. **The wave route**: a seasonal series with drift is monotone at the sample's scale and noisy at the element's; put back
in position order, the sample names the pieces, each piece is sorted where it lies, and the knot sweep merges only where pieces
cross. 1.11-1.24x, and only where the sample prices the crossings low: a swing of two periods lost 0.76 before that gate. **The
seam**: a noisy ring buffer shows in the short-reach route's block medians before any block is sorted, as one drop across the
range; one rotation, then the route: 1.12-1.16x.

Two reverted. A Weyl pivot for the AVX2 quicksort read 1.00 on every row, so the wave's cost there is still unexplained, now with
two diagnoses struck off. Direct three- and four-way merges read 0.52-0.68: one pass with a longer dependency chain loses to three
short ones. Two measured: the copy-back pass is worth 1.21-1.35x on block shapes to anything that could return a description
instead of an array, and the longest increasing subsequence of the sample separates one strand with strays from many strands.

Two things the first cuts taught. A side structure allocated before the route knows it needs it cost clean inputs 4-6% (a 1 MB list
per sort); take it at the first use. And every relaxation of what counts as a run needs the NaN rule restated, because the one form
that tested neither element it lifted was the one that lifted a NaN.

## 3al. The unknotting problem as a sort: the unknotter loses, the sphere wins, and a cliff under it (2026-09-21, `make unknotprobe`)

Guided by Kauffman and Lambropoulou's "Hard Unknots and Collapsing Tangles": the array closed into a ring (a circular doubly linked
list) is the diagram, strands are monotone arcs with an orientation bit, two strands cross where their value intervals overlap, and
Reidemeister moves are local rewrites (R1 the kink, R2 a merge of two ring neighbours touching only their overlap, R3 an O(1) slide).
Built literally, the unknotter is slower than the engine on 15 rows of 16 at one thread (0.009 on noise to 0.37 on near_sorted),
like the literal list before it: a list pays a node written and walked per element before any move. Its counts still say
something: the paper's hard state (no two ring neighbours cross, yet the diagram is not split) appeared exactly once, on the
shuffled staircase, and what made local moves affordable was the paper's own escape, a move that complicates first: cutting a
strand at a gap (the Culprit's long arc) took the nodes walked per element from 21.5 to 0.00 on near_sorted.

The transfer that won is R0. On the sphere a rotation is free, and in the engine a sorted array read from the wrong slot (a ring
buffer, a wrapped log) cost 2.0x the sorted array at one thread and 2.6x at 14, because the tail route merged the second run in
element by element. The loop-back reads the strand across the wrap: the prescan's prefix plus an ascending suffix whose last
element is no larger than the first, rotated to the front once. 1.56-1.63x at one thread and 1.90-2.03x at 14 on the ring buffer,
1.73-1.75x / 1.46-1.51x on a rotated sorted prefix with a random tail, 2.07-2.14x at one thread on a 30% prefix + noise + 40% suffix,
guards 0.97-1.02 (three runs). Its first cut re-entered the whole dispatcher and read 0.89: on a sort three or four passes long, one redundant
pass is the difference between a loss and a win. The fix that hinted the prescan where to start then cost every sorted call of 2
to 32 elements 10-16% (`make callbench`: a loop whose start is a variable is a different loop), so the remainder goes to the tail
route directly instead.

The probe's worst row was not about rotation at all. The run detector behind relocation and the knot route gave up at an average
run under 4096 elements, the run MERGE's constant, inherited by two routes that copy runs; a shape whose runs were exactly 4096
passed by nothing and, rotated, failed by 12 elements (5.8x). At one thread every array of disjoint blocks averaging 256 to 4095
elements had been sorted in full. The floor there is now the knot route's own 256: 3.6-7.6x on those shapes, guards 0.98-1.01;
at 14 threads every lower floor measured mixed, so it stays.

## 3ak. The proposed Moebius DLL sort, implemented as given: slower on every shape (2026-09-21, `make dllbench`, `experiments/moebius_dll/`)

An outside conversation proposed turning the array into a doubly linked list, reading descending runs back through their `.prev`
pointers, and merging the runs pairwise, and said it should be faster than the engine on sine-wave data. Implemented as given in
JavaScript and translated to C twice (as written, and in the conversation's own relinking form), each paired with the engine on the
same data. **No row clears 1.05 anywhere.** C at one thread: 0.03-0.51 as written, 0.01-0.23 relinked; the clean and noisy sines
0.16-0.29. JavaScript against the JS engine: 0.07-0.30. The only win is against V8's own `Array.prototype.sort` on uniform data
(1.18-1.33), because V8 calls a comparator per compare. It is a natural merge sort with a linked list in front: O(n log R), the list
costing a node per element before anything is sorted, and its one real idea, reading a descending run backwards, is already in the
engine as the twist.

## 3aj. A galloping merge for a sparse strand over dense blocks; the dense-integer bitmap's budget 4n -> 16n (2026-09-21, `make knotbench`, `make mcbench`)

**The galloping two-strand merge.** One sparse strand spanning every value over dense disjoint blocks makes every block a
two-strand crossing of a few sparse elements and the whole block. The knot route already took it, then merged every element one
compare at a time: 2.4-2.7x the same array with nothing crossing, at one thread (the ceiling). Now eight branch-free steps while
the strands alternate, and when one strand still wins eight places ahead, a gallop and one bulk copy. One binary, 8.4M: **1 in 65
2.45x, descending 2.56x, 1 in 513 2.70x, 1 in 4097 2.70x at one thread; 1.18-1.31x at 14.** It recovers the whole ceiling. The
first cut ran the lookahead on every crossing and cost 2-5% on the staircase where most elements merge; now only crossings whose
pieces differ fourfold in length take the galloping kernel, and the staircases read 1.00. Same output bytes as the branch-free
merge, which the harness checks row by row.

**The dense-integer bitmap's budget.** It accepted distinct integers spanning up to 4n, a bound set from a guess about the cache.
A ladder with the gate lifted says the bitmap beats the leaf (the int32 radix) by 5.0x at 6n, 3.3x at 16n, 2.9x at 24n and still
narrowly out to 64n, and that what the wider gate must give back costs more as the map grows (one planted repeat 0.89 at 16n, 0.73 at
64n). The budget is now 16n: **3.7-5.0x at 8.4M and 6.0-8.7x at 1M on spans 6n to 16n**, every other row 0.98-1.05, the planted
repeat 0.88-0.94. The predictions (1.4-1.8x at 6n, a loss by 32n) were far too low, and the harness had never reached this gate: its
"span 8n must decline" row was a sorted array, which the prescan returns first.

## 3ai. Three changes taken from an outside write-up: kinks repaired, a gate fix that measured nothing, four shapes in the scale report (2026-09-21)

An outside conversation about a "Moebius unknot sort" was read against the engine. No sort was timed in it, and most of it was
already here or already measured false here. Three things were taken.

**Kinks (Reidemeister move I): won.** An adjacent pair swapped inside a run that otherwise goes on ended the run, and an array
of sorted blocks with a few thousand of them paid for a full sort. The detector now asks both neighbours at a run break and,
if they agree, swaps the pair back where it stands; the strand stays contiguous, so the knot plan is untouched. Ceiling first
(a pass told where the kinks are): 4.0-5.6x at one thread. A/B, one binary (`./strandprobe 40 8388608 kinkab`): **5.8x at 0.1%
kinks, 5.0x at 1%, 3.2x at 4.8% on blocks; 4.2 / 3.8 / 2.7 on staircases; 1.24-1.52x at 14 threads up to 1%**; the same with
every other block twisted; guards 0.98-1.01 over three runs.

**The noisy sloped sine's gate fix: built, neutral (1.02), reverted, and the diagnosis was wrong.** §3af item (f) blamed
`AVX2_TAKE`'s `!c->nearly` for sending the wave to the coarse leaf, on a profile build's 126 ms against 87. Paired in a normal
build the two leaves cost the same on this shape. With the AVX2 leaf switched off the wave reads 1.05 against uniform and its
own values shuffled read 1.05; with it on, 1.19 and 1.01. **The AVX2 quicksort is what is slower on this order, by 1.17x.**
Not traced; its pivot is the median of 63 strided samples, and 32 cycles against 63 strides alternates crest and trough.

**The scale report** now carries `block_twist`, `block_kinked`, `sine_slope` and `reach_mirror` beside the nine standard shapes.
At 10M, inside each row: logos against x86-simd-sort's AVX2 kernels reads 25.5 against 176 ms on block_twist, 39.4 against 181
on block_kinked and 127 against 156 on reach_mirror at one thread, and **251 against 162 on sine_slope: logos loses that row by
1.55x**, which is the open item above seen from outside.

## 3ah. Round four, twist and knot: a scan in blocks of eight, one orientation bit for the whole array (2026-09-21, `make twistbench`, `make mirrorprobe`, `make strandprobe`)

`experiments/TWIST_AND_KNOT.md` ranked seven avenues. Its order: the free classifier, then two stop-or-go measurements, then builds.

**Built, the run detector's scan eight elements at a time.** The profile put the scan at 47-49% of the run routes at one thread
(1.1 ns per element for a read). The twist had left it read-only, so eight elements are taken at once whenever a bound proves
the scalar test accepts each: the run list is identical by construction, and the harness checks that it is. Same binary, 60
pairs, 8.4M: **1.43-1.49x at one thread on block shapes, 1.33-1.36 on x20 staircases, 1.19 on x80, 1.16-1.22 at 14 threads**,
uniform 1.00. The 14-thread prediction (1.05-1.15) was low.

**Which moved an old verdict.** Against the PURE partition (the relocation family compiled out, both copy orientations, layout
cancelled) the run routes at 14 threads read 0.95 and a tie in §3ac. Now: **block_perm 1.077, stairs_x20 1.165; 5.77x and 4.74x
at one thread.** The parallel run routes are a win again.

**Built, one orientation bit for the whole array.** Sequential sorts, behind relocation: when a directional gate fires on the
MIRROR image (a descending suffix of half the array, the reach ladder turned round, streams that descend in arrival order) the
array is reversed once and the dispatcher entered again. `LOGOS_NO_MIRROR=1` against it, one binary: mirrored short reach
**2.07x**, mirrored 95% prefix **2.20x**, mirrored streams **2.76-2.81x**; every guard, mirrored or not, 0.98-1.02 (three cells
needed 120 pairs to say so). The prefix prediction (2.3-2.5) was low: the suffix scan is a read the ceiling never paid. The
writhe could not pre-gate the stream probe: four mirrored streams read 0.88 at 1,048,576 elements and under 0.60 at 1,000,003,
because whether a sampled pair falls inside one stream depends on the lag modulo the stream count. A harness row that named
what must happen found it.

**Measured, not built.** Persistence (the share of consecutive sample-pair directions that agree) reads 0.33 on noise and
0.95-1.00 on anything stranded, in either direction or both, where the inversion rate reads 0.00-0.05 for a triangle wave, a
sloped sine and noise alike: 16 of 16 shapes classed as predicted, rule fixed in advance. And sorted blocks with 1% late records
sort **2.0-2.2x** faster when told which positions are strays, through a told sort that wastes two serial passes of its own
(its null row is 0.61), so a route that lifts strays inside the scan has about 3x to collect at one thread. Next build.

**The literal linked list, built because it was asked for, and measured.** The knot route was first described as "convert the
array to a linked list"; what shipped is an array of run descriptors. The literal version (`LOGOS_KNOT_LIST=1`: a 32-bit link per
element, twisted runs linked backwards, crossings untied by relinking, the list walked out) is **0.50-0.57 of the shipped route's
speed at one thread on blocks and x20 staircases, 0.73-0.75 on x80, 0.62-0.69 at 14 threads**, for 4 more bytes per element. Its
one strength is where merges dominate (a relinking merge moves no data); its one free lunch is the twist (links that point the
other way). The guessed walk (next = cur +- 1, the load only checking) first measured the same as the plain pointer walk,
because the compiler continued from the loaded link on the path where the two compare equal; laundering the comparison made
it worth 0.43 -> 0.51. Kept opt-in, with a harness section that asserts the list was the path.

**Found on the way.** Every multi-copy bench had stopped linking: the Makefile's symbol-rename list never got last round's new
public symbol. "Every flag builds" had compiled the engine and never re-linked a bench.

## 3ag. The Moebius twist: a descending run is consumed as it lies (2026-09-20/21, `make twistbench`)

The run routes (relocation, the knot route) used to swap every descending run end for end the moment the detector found it. Now
the run's descriptor carries one orientation bit and nothing is reversed: a lone piece is placed by a backward copy, a crossing is
merged with the twisted cursor walking down (an up-run beside a down-run is a bitonic pair, and this is its two-ended merge), and
the knot plan cuts in logical coordinates. The detector no longer writes to the array, and a descending run that straddles a
detection chunk's boundary is one strand instead of two.

**Ceiling first.** An array with descending blocks against the same array with them already ascending prices the reversal
passes exactly, because after the reversals the two are byte-identical: 1.07-1.17 at one thread, 1.02-1.12 at 14 (8.4M, 40
pairs, null 1.014 / 1.017). My one-thread prediction (1.03-1.08) was wrong and low: the run routes cost 2.3 ns per element, so
a swap pass is a sixth of the sort.

**A/B, one binary, one engine copy, switched by `LOGOS_NO_TWIST=1`** (120 pairs, 8.4M; above 1 = the twist is faster):

| shape | 1 thread | 14 threads |
|---|---|---|
| 64 disjoint blocks, every one twisted | **1.149** | **1.114** |
| every other one | **1.068** | 1.015 |
| 1024 blocks of 8k, all twisted | **1.144** | **1.094** |
| staircase, 20% crossing, all twisted | **1.108** | **1.093** |
| staircase, 20% crossing, every other | **1.056** | 1.044 |
| staircase, 80% crossing, all / every other | 1.014 / 1.020 | 1.093 / 1.045 |
| guards: blocks none twisted, staircase none twisted, uniform | 0.998, 0.984, 1.026 | 0.994, 0.987, 1.006 |

It recovers 84-97% of its ceiling at one thread, and ties where four elements in five are merged (there the merge is the sort).
The 14-thread cells have p10-p90 of 0.81-1.33 tonight: read their first decimal only.

**One wrong turn inside it.** The first two-ended merge was one loop with a signed stride per strand, four cursor updates per
element against the untwisted merge's two, and the staircase with every other block twisted read 1.000 and 1.002 against a
ceiling of 1.067. One loop per orientation pair: 1.05-1.06 over the strided loop on x20 staircases, 1.10-1.11 on x80, in the
same binary (`./twistbench 120 8388608 merge`), as predicted.

**Probed afterwards: where else does an orientation bit pay?** (`make mirrorprobe`, nothing built.) Not in the leaf's run
detector: descending teeth against ascending ones read 1.02-1.06, because what reaches it is sorted by a 16 ns/element leaf.
But ONE bit for the whole array does. Every cheap route reads order in one direction, and its own input mirrored (a
newest-first log with late arrivals) fires no gate: short reach costs **2.0-2.1x** its mirror image at one thread and 1.4x at
14, a sorted prefix plus tail **2.7x**, interleaved streams **3.1-3.3x**. One reversal pass in front keeps 90-95% of that at one
thread (1.9-3.1x) and costs 5-10% on a false positive, so it belongs behind relocation, decided by running the directional
gates on the mirrored SAMPLE. Mirrored block shapes read 0.98-1.02: the twist made them free. One wrong prediction became a
lead: at 14 threads a 95% sorted prefix sorts 1.8x SLOWER than its mirror, and the tail route is not why (it still wins 1.075).

**What the named harness rows found.** `logos_twist_last()` reports the twisted strands each route placed, and the row that
expected plain relocation at 1,000,003 elements and 8 threads got nobody: a detection chunk began 7 elements before a block's
end, a body under `RELOC_MIN_BODY` has no gap test, the 7 fused with the next block into a run whose interval hid holes, and
relocation declined. Older than the twist, masked by the knot route, not fixed. CHANGELOG has the full account, the
verification, and a reading of `make profile-full` that ranks what is left to trim per element.

## 3af. Disorder of short reach: index blocks sorted where they lie; and what the hunt for it found in the near-sorted gates and the general leaf (2026-09-20, `make reachhunt`, `make reachbench`, `make nsgatebench`)

The question was abstract: the knot route worked, so what else looks like run detection and unknotting? Six analogies were written
down with predictions (`experiments/SHORT_REACH.md`). The first step was a ceiling bench, not a route. One analogy was then built
and ships, two were measured and are not built, three are unmeasured, and the hunt found defects in code that was already
shipping: in the near-sorted route's gates, in the general leaf's nearly-sorted probe, and in a harness row. The order of events
matters here, so this section keeps it.

**Ceilings first (`make reachhunt`).** `reachhunt.cpp` includes the zoo of twenty unchanged and adds informed sorts that are TOLD
the structure. Paired rows, n = 2^23, ratio = t(logos) / t(method):

| shape | method | 1 thread | 14 threads |
|---|---|---|---|
| event_ts_latency (displacement <= 4096) | index blocks sorted in place + seam repair | 1.28-1.31 | 1.18-1.22 |
| window_shuffle (2048-element windows) | the same | 1.34-1.37 | 1.22-1.24 |
| window_shuffle | cuts (everything before <= everything after), told | 1.64 | 0.30 |
| window_shuffle | cuts, found blind | 1.11 | 0.24 |
| coarse_key_groups (random inside groups of 2k-200k) | cuts, told | 1.43 | 1.11 |
| regime_switch (3 value bands, reach 512 inside a dwell) | chunk relocation, told the segments, then blocks | 2.0-2.1 | 1.3-1.5 |
| latency + 0.01% far strays | blocks + seam repair, which must decline | 0.60 | 0.41 |

Seven predictions were graded there; two were right. The informative misses: cuts told at 14 threads were predicted 1.3-1.8x and
measured 0.30x, because 2048-element ranges fall under the leaf's flash threshold and a parallel sort sends them to a scalar
quicksort; blind cut detection was predicted at 8-12 ms and costs 35, because a suffix minimum and a running maximum are
dependency chains; and the chunk-relocation ceiling was predicted 1.2-1.5x and measured 2.0x for a reason that is not its merit:
the engine runs three-band data at twice its uniform time. **A ceiling measures the baseline's weakness too.** Cuts are a special
case of blocks (a block boundary that happens to be a cut has an empty seam), so they are not built. The stop rule had been written
down in advance: no parallel route unless blocks beat the 14-thread time on both zoo shapes. They did, by 18-24%.

**The route (`reach_try_f64`).** Cut the array into index blocks of 32768 (more when the reach needs it, 131072 at most), sort every
block in place in parallel, then repair the seams: for sorted blocks L and R, the elements of L above R[0] and the elements of R
below the last of L are merged in place, and what crosses a seam rightward is matched by what crosses leftward, so nothing else
moves. No n-sized buffer is asked for. The ceiling bench could only CHECK that every block keeps an untouched middle (the condition
under which seams are independent) and decline otherwise. The route GUARANTEES it: with m(k) the median of sorted block k, an
element of block k below m(k-1) or above m(k+1) is a stray and is set aside first, so x(k) counts elements below m(k), y(k+1)
counts elements above it, and m(k) is in neither.

**Strays decide the cost, and a sample cannot see them.** A few are sorted apart and put back by one in-place shift pass: between
two strays every element moves by the same amount, so the pass is a list of `memmove`s, left-movers first from the left, then
right-movers from the right. (The first cut merged them through a second buffer: +36 ms at one thread for ONE stray.) A block that
holds a stray is split three ways before it is sorted, because one zero in a block of timestamps stretches the flash sort's
value-linear map until every real element lands in one class. How many strays are worth putting back depends on the thread count:
the shift is serial, about 0.7 ns per element at most, against a saving of ~7 ns per element at one thread and ~0.4 at fourteen. Up
to four threads strays are simply put back; wider sorts hold eight probe blocks against their fences first (the estimated medians
of their neighbours) and decline when two break one, which costs 0.05-0.1 ms and moves nothing. A rule that stopped the block phase
when strays kept turning up was measured dominated and removed (strays in 28% of the blocks at 14 threads: 1.24x against the
partition when put back, 0.78x when the phase stopped itself).

**The gate reads the sample, never the array.** Sample indices i and i+L sit |frac(L phi)| n positions apart; for the Fibonacci lags
144, 233, 377, 610, 987 and 1597 that is 0.31%, 0.19%, 0.12%, 0.073%, 0.045% and 0.028% of n, a ladder of distances. The reach
estimate is the smallest rung from which no distance shows more than 2% inverted pairs. Random data fails the first rung, which the
shared sample has already read, so it pays nothing. Four 4096-element windows must then show dense adjacent descents; sparse
disorder on sorted data is left to the near-sorted route.

**The block sort was the surprise, twice.** The first cut sorted blocks with the engine's general leaf and measured 1.15-1.53x at
one thread. (a) The general leaf's 40-pair probe calls a block nearly sorted when the reach is under its stride of n/40, tries an
insertion sort that gives up after 8n moves, and keeps the quicksort away: going straight to a kernel was 1.97x at 14 threads on
latency < 512. (b) On these blocks a flash sort beats the AVX2 quicksort at one thread by 1.26-1.81x: classification is nearly the
identity on locally ordered data, so count, scatter and insertion pass all stay where the cache already is. The route's flash counts
its classes before it scatters and hands a block with a crowded class to the general leaf (a block that straddles a quiet hour holds
two clusters, which the leaf's quantile-spread check passes). (c) A second look found the first cut's "small span means integers"
test sending every series with unit spacing to the general leaf: 293 ms on 64-element windows at one thread, 76 through the kernel.
(d) Disorder that reaches two or three places is an insertion sort's; four-apart pairs tell it (noise of +-2 at one thread: 0.81
with flash alone, 1.10 with the insertion kernel). Block size: 16384 is neutral against 32768, 65536 is 0.85-1.00.

Route off / route on in one binary (`LOGOS_NO_REACH`). 8.4M with 30 pairs on a day whose null test read 6.0% at 8M; 30M with 12
pairs; 1M with 120 pairs:

| shape | 8.4M, 1 thread | 8.4M, 14 threads | 30M: 1 / 14 | 1M: 1 / 14 | 8.4M at 4 / 7 threads |
|---|---|---|---|---|---|
| latency_4096 (event log, latency < 4096 ms) | **1.91** | **1.31** | 2.53 / 1.51 | 1.00 / 1.00 (*) | 1.61 / 1.63 |
| window_2048 (shuffled inside 2048-element windows) | **2.22** | **1.38** | 2.72 / 1.79 | 1.94 / 1.38 | 1.79 / 1.82 |
| jitter_1000 (index + noise of +-1000) | **2.05** | **1.33** | 2.13 / 1.52 | 1.81 / 1.25 | 1.50 / 1.43 |
| latency_512 | **2.38** | **1.49** | 2.86 / 1.70 | 2.21 / 1.38 | 1.82 / 1.68 |
| window_64 (`TRUTH_TABLE` J6's shape) | 2.46 | 1.48 | 2.86 / 1.83 | 2.45 / 1.34 | |
| jitter_2, jitter_8, jitter_30 | 1.10, 2.54, 2.17 | 1.80, 2.35, 1.31 | 1.09, 2.56 / 1.67, 1.55 | 1.12, 2.60 / 1.44, 2.08 | |
| ticks_dup (jittered one-second ticks: heavy duplicates) | 2.80 | 1.02 (a counting route wins the dispatch) | | | |
| one_stray (latency_4096 + one far element) | 1.81 | 1.14 | 2.44 / 1.31 | | 1.49 / 1.39 |
| strays in 0.001%, 0.01%, 0.1%, 1% of elements | 1.77, 1.57, 1.49, 1.23 | 1.26, 1.00, 1.01, 1.04 | 2.21 / 1.14 (0.01%) | | 1.28 / 1.01 (0.01%) |
| 0.05% zeros; 8, 84, 840 zeros; 84 late events | 1.58; 2.28, 2.35, 1.54; 1.77 | 0.99; 0.96, 0.97, 0.99; 1.10 | 1.41 / 1.00 (0.05%) | | 1.15 / 1.01 (0.05%) |
| latency_512, noise of +-100, +-20, +-8, +-2, each + 0.01% strays | 1.84, 1.81, 2.33, 2.32, 1.56 | 0.99-1.02 | | | |
| reach_40000 (needs blocks above the cap: declined from the sample) | 1.01 | 0.96 | | | |
| uniform, near_1pct, coarse_groups, regime_switch, stairs_x20, block_perm64, heavy_tail, few_distinct | 0.99-1.03 | 0.94-1.02 (few_distinct read 1.18 with p10/p90 of 0.68/1.46) | 1.02 / 0.98 (uniform) | 1.00 / 1.01 (uniform) | |

(*) a reach of 4096 is longer than the first rung of a 1M ladder (0.31% of n is 3,100 positions), so the gate declines from the sample.

**Defect 1: the shared sample's inversion rate read 7 pairs, not ~1900.** Sample indices i and i+144 are 0.31% of n apart and the
LATER index is the EARLIER position (frac(144 phi) = 0.99689). The `p1 <= p0` test meant to drop the few pairs that wrap around
the array dropped all 1,897 adjacent pairs and kept the 7 wrapped ones, a whole array apart. It passed gray-code, sawtooth,
block-permuted, staircase and grouped-run data as nearly sorted (true rates 8-79%) and turned away one genuinely near-sorted input
in seven. Found because the short-reach gate wanted the same statistic at other lags and the pair counts came back 7 and 3.
**Defect 2: the near-sorted pre-gate had no poison recovery**, and the route it guards has: on sorted data with random swaps it
declined 12 of 40 seeds at 1% swaps and 38 of 40 at 2%, so the route was mostly unreachable on the data it was built for.
**Defect 3, what the two cost together:** sorted runs grouped by a coarse key ran 5.8x off the ceiling at 14 threads, because the
7-pair statistic let the near-sorted route run, it rejected 73% of the array and declined after 57 ms. Found only because the
ceiling bench's late decline handed the engine an array of sorted blocks. The gate now reads every pair in position order and the
pre-gate runs the route's own scan on its four windows (`LOGOS_NS_GATE_OLD=1` restores both):

| shape (8.4M, gates as they were / repaired) | 1 thread | 14 threads |
|---|---|---|
| groups_of_runs (sorted 33k blocks inside coarse-key groups) | 1.00 | **3.82** |
| sorted + 1% random swaps, four seeds (the 7-pair statistic turned two away; the pre-gate turned the other two away) | 0.95-1.01 | **1.36, 1.39, 1.30, 1.48** |
| sorted + 2% swaps, two seeds (newly admitted: true rate ~3.5%) | 1.00-1.01 | **1.37, 1.23** |
| sorted + 0.5%, 3%, 5%, 10% swaps (100 pairs) | 0.99-1.02 | 1.01, 1.01, 0.98, 1.00 |
| block_perm64, stairs_x20, sawtooth, gray_code, sorted_runs_256, uniform, heavy_tail, few_distinct | 0.99-1.00 | 0.97-1.04 (30 pairs); 0.98-1.01 (100 pairs) |

**What the ROUTE is worth, not the gates** (`NSG_ROUTE=1 ./nsgatebench`: the route switched off by `LOGOS_NO_NEARSORTED` against the repaired
gates, 14 threads, 100-300 pairs). The old pre-gate had turned 38 of 40 inputs with 2% swaps away whatever their size, so nobody had
seen this table:

| swaps | 1M | 1.5M | 2M | 3M | 4M | 6M | 8.4M |
|---|---|---|---|---|---|---|---|
| 0.5% | 1.36 | 1.19 | 1.48 | 1.66 | 1.69 | 1.75 | 2.12 |
| 1%, two seeds | 1.21, 1.28 | 1.07, 1.12 | 1.25, 1.25 | 1.42, 1.37 | 1.38, 1.43 | 1.36, 1.73 | 1.38, 1.41 |
| 2%, two seeds | 0.98, 1.04 | **0.84, 0.87** | 0.93, 1.15 | 0.94, 0.94 | 0.99, 0.98 | 1.33, 1.41 | 1.12, 1.13 |
| 3% | 1.00 | **0.75** | **0.80** | 1.00 | 1.01 | 1.02 | 1.09 |

The route has three spawn phases and sorts its strays once, which a few million elements do not pay back unless the strays are few. So
the gate is **2.5% below 6M elements and 5% from there on** (`NS_INV_RATIO_SMALL`, `NS_WIDE_MIN_N`); with the gate lifted, 5% swaps lose
0.71 at 8.4M. With it, gates as they were / repaired at 1.5M, 2M and 4M (300 pairs): 1% swaps 1.00-1.36, 0.5% swaps 0.98-1.61, 2% and 3%
swaps 1.00-1.02, groups_of_runs 2.76-3.81.

**A harness row had been passing because of the broken gate.** The tail route's parallel NaN row demanded identical BYTES from two
runs, on the premise that a declined tail route leaves both on one deterministic path. That held only while the 7-pair statistic sent
its input (a tenth of it a random tail, which reads 5.1%) to the near-sorted route. Read properly it goes to the partition, where the
order inside a bucket depends on thread timing, and with a NaN present two runs of the SAME configuration differ (21 of 40 tries).
It showed first as one unexplained failure under ThreadSanitizer, whose target pipes through `tail -3` and cut the FAIL line off;
looping the plain harness caught it on the first pass. The row now checks a return and a permutation of the input's bits.

**Defect 4: the general leaf wastes 8n moves on dense, short disorder.** The same 40-pair probe runs in every partition leaf and
in the sequential dispatcher. Data the short-reach route declines (strays at 14 threads, arrays under 2^18, int32 never) still meets
it. When the probe says nearly sorted, 64 strided pairs EIGHT apart now decide whether the insertion sort is tried at all: its
budget is a mean displacement of 8, and an element displaced further is usually inverted with its eighth neighbour
(`LOGOS_NO_DENSE_PROBE=1` restores the old leaf):

| shape (8.4M, old leaf / new leaf, the short-reach route on both sides, 30 pairs) | 1 thread | 14 threads |
|---|---|---|
| latency_512 + 0.01% strays (the route declines it at 14 threads: the partition's leaves meet it) | the route takes it | **1.36** |
| noise of +-100 + strays | the route takes it | **1.70** |
| noise of +-20 + strays | the route takes it | **1.87** |
| noise of +-8 and +-2 + strays (inside the insertion sort's budget: it must still run) | | 1.00, 1.01 |
| regime_switch | 1.17 | 1.16 |
| coarse_groups, reach_40000 | 1.10, 1.07 | 0.99, 1.00 |
| the other 24 rows | 0.98-1.01 | 0.96-1.08 (a 6% day); 0.99-1.06 on six guards with 100 pairs |
| the three stray rows at ONE thread, measured before the route took strays there (24 pairs) | 1.32, 1.31, 1.28 | |
| with the route OFF on both sides (four configurations interleaved, minima): 64-element windows | 384 -> 160 ms | 39.7 -> 21.4 ms |
| the same: noise of +-30 | 497 -> 221 ms | 46.5 -> 23.9 ms |

**What the route weighs and what it does when resources run out.** `make memprobe` (10M, peak traced heap of one cold call on top of the input): **0.10 bytes per element at one thread and 1.33 at fourteen**, against 15.5 and 23.2 on the partition, and 0 after `logos_release_buffers()`; at 1M, 1.1 and 11.2 (ten threads' scratch and contexts over thirty blocks), against 15.9 and 28.5. `make faultprobe`: 480 failed allocations and 12 failed
thread creations over the two short-reach cases, none ending badly (20 allocation sites, one worker function); the whole sweep on the
final engine, twice: 8,170 and 8,166 failed allocations (the count moves by a few with thread timing) and 234 failed thread creations each, 0 bad. Every branch of the
route is asserted BY NAME in the harness through `logos_reach_last()`: sorted, sorted with strays shifted in, declined at the gate,
by the probe, by the plan. The first version of that section passed with the route never firing at one thread, because the gate's
sample is 384 points there and the ladder needs 2048; and the diagnostic itself reported the previous sort's answer until it was
reset at the top of the dispatcher rather than at the route.

**What one call costs at the small end** (`make callbench`, new: a baseline engine against the working tree on 0 to 50,000 elements,
random and sorted, two binaries with the copies swapped). Every A/B of the round starts at a million, so this was the only way to
see that the round AS BUILT cost every small call: **1.09-1.14 at n = 0, 1 and 2**, 1.03-1.09 at 3 and 5. Three causes, three
different verdicts. (1) MEASURED AND FIXED: a gate helper with two 16 KB arrays was inlined into the dispatcher, whose prologue
probes every page of its frame on every call (stack-clash protection); 8 probes became 10, which alone is 1.09-1.14 at n = 0 and 1,
all eight orientation readings between 1.07 and 1.16. `noinline`: 0.98-1.01. (2) NOT RESOLVED, REMOVED ANYWAY: resetting the
thread-local diagnostic on every sort put two `__tls_get_addr` calls (thread-locals are function calls under `-fPIC`) on a path
that had none; predicted +20-35% on a sorted triple, measured +5% in one orientation and -14% in the other. The reset now starts at
the route's floor of 2^18 elements and a smaller sort leaves the answer alone, which the header states and the harness asserts.
(3) A WIN THAT IS LAYOUT, NOT SHIPPED: the leaf's new probe loop, inline, made a sort of 100 random doubles THAT NEVER RUNS IT 5%
slower; deleted, 2.5% faster; out of line, 12% faster, in two bench binaries. But the round as built, with the same inline loop,
read 0.90. Rows from 10 to 100 elements read 0.78 to 1.13 across nine builds with identical logic on that path, reproducibly:
**reproducible is not real**, it is a property of one build. A small-call row counts at n <= 5, in both orientations and two
runs, with a mechanism visible in the assembly. The bench also gave a ceiling: entering the dispatcher costs ~5.6 ns (a thread-local
read, the call, nine probed pages of a 36 KB frame), so `logos_sort_f64` now returns at once when n < 2 (**8.5 -> 2.9 ns**, 0.29 in
both orientations), and every call of 2 to 24 elements still pays it (open direction 0g).

**Still open, found here.** (a) A sentinel the SAMPLE happens to contain defeats the partition's certificates: with 840 zeros in 8.4M
jittered timestamps the trace shows KS 0.99, a rejected key grid and a table scatter; the int32 path windows its sample against
exactly this and the f64 partition does not. (b) Three value bands 2^40 apart run at twice the uniform time at both thread counts.
(c) The lazy view's bucket sorter sends ranges under 4096 elements to a scalar quicksort when it runs parallel (0.30x on 2048-element
ranges). (d) The route is f64 only, its ladder is fractions of n, and blocks are capped at 131072, so above ~117M elements the gate
declines even when the reach is short; its stray shift is serial. (e) Candidates 4 to 6 of the brainstorm are unmeasured. (f) **Two scales of order at once**: a sine wave with a slope plus noise
of +-50 elements costs **1.22-1.31x a sort of random data at one thread** (`make sineprobe`, paired against a uniform reference,
40 pairs, two runs, null 0.99-1.00), and nothing at 14 threads. The control is the finding: the SAME noise on a plain slope reads
0.59-0.63, because the short-reach route takes it, so the cost belongs to the WAVE and not to the noise, and it tracks the reach
rather than the swing. A third control found the CAUSE: the same VALUES with the index order destroyed read 1.01, level with random, so the cost is the
ORDER and the array is slower than the same numbers shuffled. A profile build says the natural order takes the COARSE leaf while the
shuffled multiset and uniform take the AVX2 quicksort leaf, because `AVX2_TAKE` requires `!c->nearly` and the 40-pair probe calls a
sloped wave nearly sorted. The dense-disorder probe added in this same round is exactly the statistic that would overrule it, and it
gates only the insertion sort; extending it is predicted at ~1.45x here and neutral on near-sorted shapes, unbuilt and un-A/B'd.
A composed route is bounded by measurement at 2.1x (the control's level) to 3.7x (the clean wave's), 2.6x after that gate fix.

## 3ae. An outside review, re-tested here: what a sort weighs, what it does when a resource runs out, subnormals, NaN, and the scaling exponent (2026-09-20, `make memprobe`, `make faultprobe`, `make slopebench`, `make sanitize`, `make tsan`)

A publication-readiness review of a sibling line of this engine (v3.10, developed separately, measured on another host) named three
gaps: edge-domain validation (NaNs, infinities, allocation failures, extreme sizes), buffered memory that is understated and "may
approach O(Pn) under pessimistic parallel allocation", and novelty. None of it was assumed to hold in this tree. Each was turned into
a measurement here, and the pattern of the results is the finding: **every condition the harness had never produced found a defect**
(an infinity, subnormals, a NaN payload, a failed allocation, a failed thread creation). The timing discipline of this project is
strict and its correctness harness is large, and both only ever ran the engine with every resource available, on finite, normal,
NaN-free numbers. Most fallbacks did work; but the ones that did not had never had the chance to show it.

**What a sort weighs.** Peak traced heap of ONE COLD sort in a fresh child process (the malloc family interposed; the pools make
every later call look cheap), bytes per element on top of the 8-byte input, 10M elements:

| | 1 thread | 14 threads, before | 14 threads, after |
|---|---|---|---|
| uniform, heavy_tail, near_sorted | 15.5 | 34.9 | **23.2** |
| block_perm (relocation) | 15.5 | 9.0 | 9.0 |
| few_distinct, quantized (counting) | 0.0 | 0.0-0.2 | 0.0-0.2 |
| **dominant_90**: one value holds 90%, the rest distinct | 15.5 | **104.5** | **17.8** |
| `logos_sort_f64_inplace` | 0 | 0 | 0 |

| still held after `logos_release_buffers()` | before | after |
|---|---|---|
| any partition shape, 1 thread | 7.5 | 0 |
| uniform, 14 threads | 14.0 | 0 |
| dominant_90, 14 threads | **95.2** | 0 |

The header said "~9n bytes of scratch". Three defects sat behind the numbers. Every parallel leaf loop sized ALL P contexts by the
LARGEST bucket, so one bucket no splitter can divide cost P times n, the review's O(Pn) exactly: a gigabyte of scratch for an 80 MB
array. The histogram-free partition allocated its slack-sized regions (12.8 bytes per element) twice and freed the first copy, because
glibc never returns a 64-byte-aligned block from mmap. And `logos_release_buffers()` freed 9 of the 23 bytes per element it held, 9 of
104 on the dominant shape, and none of the int32 counting route's table pool (up to 64 MB). Contexts now fit a typical bucket and an oversized one borrows room while it is sorted (the buckets in
hand at one moment are disjoint, so they sum to at most n); the regions are asked for once, aligned; release returns everything.
Speed: neutral. The final engine against the committed one, paired, both copy orientations so that the code-layout term cancels,
  8M, 30 pairs each, run twice: 0.99 to 1.04 on nine rows (uniform, heavy_tail, dominant_90, block_perm, near_sorted,
  few_distinct and quantized at 14 threads, uniform and block_perm at one), and after the last fix of the round 0.98 to 1.06 on
  eleven, with bimodal at both thread counts (0.998 and 1.035: its leaves take the gap-split path that fix touched). At or inside
  the ~4% floor; the staircase row reads 1.40 and 1.43, which is the knot route. An earlier check of the memory fixes alone, on a hot box, read 0.96 to 1.07 with the same spread on the one-thread rows
  that change cannot touch.
Prediction for 14 threads: 9.6. Measured 34.9. I forgot the partition's regions exist.

**The bound, and a check on it.** A context costs 6 bytes per element of capacity (a 4-byte count and a 2-byte index) and is
grown with 25% headroom: 7.5. With B = 12P buckets and m the largest bucket, the old engine peaked at about 9n (scratch and index
cache) + 25.6n (the regions, twice) + 7.5 P m: 35.6n on uniform (measured 34.9) and, with the partition declining on a dominant
value, 9n + 7.5 x 14 x 0.9n = 103.5n (measured 104.5). Now the contexts cost 7.5 x P x min(m, n/(6P) + 4096), about 1.3n, plus m
once when a bucket has to borrow: 9n + 12.8n + 1.3n = 23.1n on the partition (measured 23.2) and 9n + 1.3n + 7.5 x 0.9n = 17.0n
when it declines (measured 17.8). **O(n) with a constant under 24, whatever P is, where it had been O(P m).**

**What it does when a resource runs out.** `faultprobe` interposes malloc, calloc, realloc, posix_memalign and pthread_create and
fails the k-th call the engine makes in one cold call (or every call from the k-th on), each run in a forked child so that a crash
or a hang is a result. Four entries, 19 cases, 1 and 4 threads:

| one cold call, one fault injected | runs | came back sorted | wrong output | crashed | hung |
|---|---|---|---|---|---|
| an allocation, 1 thread, before | 1,500 | 1,239 | **261** | 0 | 0 |
| an allocation, 4 threads, before | 6,816 | 4,862 | **1,880** | **74** | 0 |
| a thread creation, 4 threads, before | 186 | 32 | **45** | 2 | **107** |
| all of them, after | 7,008 | **7,008** | 0 | 0 | 0 |

(The allocation runs fell from 8,316 to 6,822 because the k-way merge no longer allocates. Three targeted sweeps reach what the
main one does not, and are clean too, 1,226 allocation and 48 creation faults: the hash-count route forced with
`LOGOS_MCGATE_OLD=1`, relocation without the knot route with `LOGOS_NO_KNOT=1`, and the partition's spill path forced with
`-DHF_SLACK=0.2`. With those, the creation of every one of the 18 worker functions in the default build has been failed at least
once, and 128 distinct allocation sites. The final engine's own sweep, with the in-place entries and a 70-run chain added, 23
cases: 7,894 injected failures, 0 bad, 19 worker functions, 132 allocation sites.)

Four defects, none of them in code anybody had marked as risky. (1) No `pthread_create` result was read at any of 23 sites. A failed
creation HUNG the partition (a barrier waiting for a participant that does not exist) and returned WRONG OUTPUT, silently, from the
counting, lattice, near-sorted, relocation and sorted-prefix routes, which joined a garbage handle and read results nobody wrote.
This is the likelier failure on Linux: a pid limit makes pthread_create fail while malloc still succeeds. (2) `merge_all_runs`
returned with its runs unmerged when two small lists could not be allocated: unsorted output, no error, from three routes. (3) A
context that failed to allocate was still handed to the leaf by every worker, because the flag that recorded the failure was only
read by the caller. (4) The sequential path freed a stack context that had never been initialised whenever the scratch buffer
failed first. That one was found by reading, while chasing the harness crash caused by (3); restored in a scratch copy it aborts
the in-process harness (`munmap_chunk(): invalid pointer`) and passes every forked one-thread run, because a fresh child's deep
stack is zeros. That is the argument for keeping both forms of the probe. The fixes are structural, not checks bolted on:
an orphaned worker runs on the calling thread (17 sites), or is dropped from the barrier whose count became atomic (3 sites, all
queue-driven), or has its static output tile covered by the caller (relocation, knot), or makes the route decline (int32 counting);
the merge's lists moved to the stack with an allocation-free fold behind them; contexts are all or nothing and `ctx_fit` refuses an
empty one. The harness repeats a sample of the probe on every `make test` (about 2,150 checks), so these paths are no longer code that
only a separate tool ever runs. Predicted 5-15% of allocation faults bad; measured 27%, because one defect sat behind hundreds of
allocations. Predicted every creation fault bad; measured 154 of 186, the rest passing on a garbage handle that happened not to fault.

**Subnormals came back unsorted.** NaN-free data with a third of its values subnormal, 300k elements or more, 4 or 8 threads: 2 to 3
descents in the output, on the committed engine too. A bucket whose range is narrower than about 1e-305 makes `classes / range`
overflow to +inf; every element computes `x * inf` and the minimum `0 * inf = NaN`, whose garbage cast the coarse leaf clamped into
the LAST bucket. The infinity fix of the day before guarded the RANGE; what must be finite is the SCALE. The same leaves had been
quadratic there (43 ms of leaf time for 300k elements, 7 after).

**NaN.** Order with a NaN present stays unspecified, and the probe shows how much: the non-NaN values came back out of order in 246
of 672 runs, because a NaN fails every compare and a prescan takes `[1, NaN, 0.5]` for ascending. The floor that is now promised and
tested is: no crash, no out-of-bounds access, and an output that is a permutation of the input's bit patterns. That floor was broken
in 16 runs (the spill sort rewrote signalling, negative and payload NaNs as the canonical one). `logos_sort_f64_nan_last` is the
entry with a defined order, at the price of one sequential pass.

**A sort that did not return.** The floor had a hole: the gap-split partition scanned with `a[i] < pivot` from the left and
`a[j] >= pivot` from the right, a NaN fails both, and when the two scans met on one neither moved again. In C it was intermittent,
because the order inside a bucket depends on thread timing: about one harness run in fifteen, one worker at 100% and the caller
asleep in a join, and about one sort in thirty on heavy-tailed data with 1% NaN at 4 threads. Catching it took a harness that can
loop one section and a hunter that signals the spinning thread so its own handler prints the backtrace. In JavaScript, where the
loop came from, it is deterministic: `[lows..., NaN, highs...]`, 6,000 elements, never returns, in the published package. One
predicate for both scans fixes both. **An 'unspecified order' contract still has to say the call returns, and that is a thing to
test, under a watchdog.**

**The scaling exponent** (`make slopebench`).

- The review read a scaling exponent off two sizes through the cost per element: with c = t/n, t ~ n^p gives
  p = 1 + ln(c_50M / c_2M) / ln 25. The scale report holds those two numbers, but on this box its rows are measured minutes
  apart and the machine's speed drifts by a multiple in that time (methodology 2), so a slope read off two rows carries the
  drift. `slopebench` measures it the way this project measures everything else: ONE 50M sort adjacent to TWENTY-FIVE 2M sorts
  of the same shape (the same generator, the same number of elements through the same work buffer), order alternating, median
  of 8 pairs. The ratio of the two times IS c_50M / c_2M.
- Two calibrations, because a new instrument is a claim. The null (50M on both sides, true growth 0) read +2.0%, +1.7%, +0.4%
  and -0.8%: the floor is about 2%. And `std::sort` on uniform must show at least the comparison-count slope, log2(50M) /
  log2(2M) = 1.222, p = 1.062: measured +23.5% and +22.3% in two runs, p = 1.066 and 1.063.
- Growth of the cost per element from 2M to 50M, two full runs (first / second):

| | std::sort, 1 thread | logos, 1 thread | logos in-place, 1 thread | logos, 14 threads | in-place, 14 threads |
|---|---|---|---|---|---|
| uniform | +23.5 / +22.3% | +13.4 / +16.3% | +21.2 / +21.6% | +34.3 / +26.4% | -20.6% |
| heavy_tail | +21.4 / +21.0 | +19.3 / +17.3 | +34.3 / +38.2 | +7.4 / +19.9 | -15.9 |
| bimodal | +24.0 / +21.7 | +14.0 / +17.6 | +22.7 / +21.3 | -14.4 / -14.7 | +0.4 |
| near_sorted | +27.1 / +29.8 | **+29.3 / +33.4** | +1.5 / +2.8 | **-59.5 / -53.8** | -22.5 |
| few_distinct | +13.0 / +15.2 | **+0.5 / -1.7** | +0.8 / +0.3 | -30.7 / -39.4 | -1.5 |
| quantized | +1.8 / +3.0 | **-0.1 / -0.8** | +4.6 / +3.9 | -40.2 / -56.7 | -15.6 |
| block_perm | +37.5 / +31.3 | **-4.4 / +0.2** | +18.1 / +37.8 | -50.0 / -37.8 | -29.1 |

- How far to trust it. The one-thread cells reproduce within about 3 points, with one exception that does not (block_perm
  in-place, 18 against 38, with p10-p90 ranges that do not overlap: unexplained, so that cell is not evidence). The 14-thread
  cells reproduce within 10 to 17 points. What follows is only what is larger than that.
- One thread. logos grows LESS than `std::sort` on every random shape (13-19% against 21-24%, p about 1.04-1.05 against 1.06),
  and the counting and relocation routes are exactly linear (p = 1.00 on few_distinct, quantized and block_perm, where
  `std::sort` pays 13-15%, 2-3% and 31-38%). near_sorted is the one buffered route that grows faster than `std::sort` (+29-33%).
  The in-place sort grows like a comparison sort (+21% on uniform; the reviewed sibling measured +9.0% for its own) and at 50M
  costs 3.6 to 21 times the buffered sort per element.
- Fourteen threads: two opposite effects, and the prediction ("about flat") caught neither. Uniform gets 26-34% DEARER per
  element: 2M doubles are 16 MB and sit in this CPU's 24 MB last-level cache, 50M do not. The structured routes get 31-60%
  CHEAPER: at 2M near_sorted costs 2.2-2.8 times per element what it costs at 50M, quantized 1.7-2.3, block_perm 1.7-2.1,
  few_distinct 1.5-1.6. At 2M a parallel call on those routes is mostly FIXED cost, of the order of 1 to 4 ms per call. That is
  a lead, not a result. What a spawn costs here: creating and joining 13 empty workers takes 0.77 ms at the median (0.56 to 1.02), about 60 us a
  thread, so a route with two spawn phases pays 1.5 ms of a 4.6 ms near_sorted call at 2M: about a third of that fixed cost,
  and nothing at 50M. Every `pthread_create` now goes through `par_run` / `par_spawn`, which is where a persistent worker pool
  would go. It has not been built: it changes the library's threading model, and it wants its own A/B at 1M to 4M.
- Predictions, graded. `std::sort` +22-30%: RIGHT. Buffered logos at one thread +5-15%: RIGHT on uniform (13, 16), low on
  heavy_tail (17-19). In-place +10-25%: RIGHT on uniform. Counting shapes flat: RIGHT. The null within 3%: RIGHT. Fourteen
  threads "about flat, -5 to +10%": WRONG on six shapes of seven, in both directions.

**The in-place sort, in parallel** (`make ipbench`). The review's headline pair was in-place against buffered. Here the in-place
entries took an `nthreads` and ignored it, no harness check had ever called them, and the slope bench priced the gap at one
thread: 3.6 to 21 times the buffered sort per element at 50M. A placement pass is cycle-following and does not split, but its
256 buckets are disjoint ranges, so ranges became tasks: one pass on a large range, its large children back on a fixed
4,096-entry stack, small ones sorted where they lie. At 14 threads against its own one-thread form: 2.50x at 8.4M and 2.85x at
50M on uniform, 2.5-2.6x on the other random shapes, 1.1-1.6x on the structured ones, no row below 1 at the 2^20 threshold. The
first pass stays on one thread, a serial share of 30 to 35%, and that is the whole table. It remains 6 to 24 times slower than
the buffered sort at the same 14 threads: zero scratch (0 against 23 bytes per element) is bought with time. 408 harness checks,
which passed on their first run, and 40 injected failures, 0 bad. Predicted 2-3x on uniform (right), 1.0x on few_distinct
(wrong on its premise: five values differ in four bytes, but make a task tree at most four wide), 3-6x behind buffered (9.8x at
8.4M, 6.0x at 50M).

**Sanitizers, whole harness, first time.** ASan + UBSan + `float-cast-overflow` over all checks: no memory error, three NaN-to-index
casts reported (clamped, left alone, per-element loops). ThreadSanitizer: 0 data races (needs `setarch -R` on this kernel).
`float-cast-overflow` is not part of `-fsanitize=undefined` in GCC, and it found every one of the infinity and subnormal defects.

**Boundaries, leak checking, and flags nobody builds.** Three more places that had never run. A boundary sweep (every n from 0 to
300, every power of two from 2^9 to 2^21 and the 100k / 350k / 500k multiples from below, at and above; both types, the buffered
and the in-place entries): 12,058 checks, which passed on their first run. Leak checking had been switched OFF in the sanitizer
target, and the first run with it on found 313 KB leaked in 28 allocations: when the dispatcher's sorted sample is not handed to
the partition (a NaN in it, another size, or it could not be extended), it was freed only on the fallback label, 11 KB per such
sort, in the committed engine too. And `-DLOGOS_NO_SPLITTER_TABLE` did not compile, so `make stbench`, the A/B for the largest
parallel win in the project, had not built since the per-mode certificate was added; with `-DLOGOS_NO_RELOCATE` the day before
that is two A/B switches found rotten. A compile-only pass over the Makefile now builds all 49 targets that have a compile
step. ThreadSanitizer covers the thread-creation faults as well: under it the harness keeps that half of its resource section
and forwards to TSan's own interceptor: 186 failed creations, 0 failures, 0 data races.

**The other engines, on the same inputs.** All three share the design behind the infinity and subnormal defects, a value-linear
bucket map whose scale is `classes / range`. JavaScript was correct on all 126 edge runs and had two QUADRATIC cliffs. A
subnormal-wide range makes the scale Infinity, and `x * Infinity | 0` is 0 in JavaScript, so nothing goes out of bounds or comes
back unsorted, but every element lands in bucket 0 and flash sort's closing insertion pass is O(n^2): 300,001 subnormals took 77 s
(42 ms after). The second is not about subnormals at all: flash sort is one scatter and one insertion pass, so a dense cluster
of distinct values inside a wide range sits in ONE bucket, and the safety probe compares two quartiles, between which such a
cluster hides: 13-21 s for 300k elements (about 30 ms after). The fix is a displacement budget on that pass, about 3% on healthy
data (the null of two identical modules reads 1.000); scanning the counts instead cost 5-11% and was rejected. Rust passes 252
edge sorts in 2.7 s (`engines/rust/tests/edge_values.rs`): it guards the range and not the scale, and is saved by saturating
casts and a concentration bail. **The same overflow is a wrong answer in C, a quadratic stall in JavaScript and nothing in
Rust: what a defect looks like is a property of the language's conversion rules, not of the algorithm.**

**Novelty: a first pass at the literature.** The review's third issue was a novelty claim without a literature review. Titles,
venues and years below were checked online on 2026-09-20; the papers themselves were not re-read, so treat this as the list to
start from, not as the review.
- **Relocation (J5) is adaptivity to a known measure, Block.** Carlsson, Levcopoulos and Petersson, "Sublinear merging and natural
  mergesort", Algorithmica 9:629-648 (1993): Block(X) counts the elements not followed by their successor in sorted order, so 64
  shuffled sorted blocks have Block = 63, and their Adaptive Mergesort is optimal for it in comparisons. What J5 adds is
  engineering (memcpy from run descriptors, a sample gate, gap-splitting), not a measure.
- **The knot route's "crossing" is close kin to Osc.** Levcopoulos and Petersson, "Heapsort - adapted for presorted files" (WADS
  1989) and "Adaptive Heapsort" (Journal of Algorithms, 1993): Cross(x_i) is the number of edges of the sequence's line plot cut by
  the horizontal line through x_i, and Osc is the total. The knot cost (mass times ceil(log2 k) over the value intervals where k
  strands are alive) is a run-level, interval form of that same sweep. The map of these measures is Estivill-Castro and Wood, "A
  survey of adaptive sorting algorithms", ACM Computing Surveys 24(4), 1992.
- **Run-adaptive merge policies are a different axis.** Munro and Wild, "Nearly-optimal mergesorts" (ESA 2018; Powersort, CPython's
  merge policy since 3.11), choose the ORDER of merges from run LENGTHS. J5 and the knot route look at VALUE intervals to avoid
  merges altogether. The two compose; neither subsumes the other.
- **Parallel in-place sorting is well trodden.** IPS4o (Axtmann, Witt, Ferizovic, Sanders, ESA 2017) distributes in place by block
  permutations; PARADIS (Cho, Brand, Bordawekar, Finkler, Kulandaisamy, Puri, PVLDB 8(12):1518-1529, 2015) parallelises the radix
  permutation itself by speculative permutation plus repair; Regions Sort (Obeya, Kahssay, Fan, Shun, SPAA 2019) is its
  theoretically efficient successor. The parallel in-place sort added here is none of these: it parallelises only below the first
  pass, and says so.
- What this means for the tree: nothing here should be called a new measure or a complexity result. The defensible claims are the
  engineering ones already made (certificates, routing, the measured niche), and the knot write-up already says the unknotting
  analogy buys a cost model and nothing more.

## 3ad. The small end of the scale: n = 0, 1, 3, 5, 10, ns per element on a linear axis, and what they found (2026-09-20, `make scale-full`)

The scale report now runs seventeen sizes, 0 to 10M (0, 1, 3, 5, 10, then half-decades from 32), and every panel carries a second
chart: nanoseconds per element on a value axis that is always linear, for all nine shapes at 1 and 14 threads. Below 10k a size is
timed as a batch of up to 4096 distinct instances sorted back to back, because one such sort is shorter than the clock can resolve
and repeating one input trains the branch predictor. n = 0 is the cost of a call. Calibration memcpy 3.17 ms, std::sort 1M 65.3 ms.

**Per call at the requested sizes** (uniform, one thread, ns):

| n | std::sort | logos | x86-simd-sort | IPS4o | LSD radix | timsort |
|---|---|---|---|---|---|---|
| 0 | 0.3 | 8.2 | 2.6 | 1.9 | 0.6 | 1.8 |
| 1 | 1.8 | 7.4 | 2.3 | 2.2 | 0.6 | 1.7 |
| 3 | 5.3 | 48.0 | 14.3 | 4.5 | 7005 | 26.9 |
| 5 | 9.5 | 85.3 | 17.5 | 12.0 | 6883 | 40.6 |
| 10 | 66.8 | 136.1 | 27.8 | 71.0 | 7399 | 144.5 |

At 14 threads the same sizes read 7.4, 7.4, 52.8, 91.1 and 177.4 ns for logos against 0.6, 3.9, 12.4, 18.9 and 69.3 for
`__gnu_parallel::sort`: every parallel contender falls back to one thread there. Logos asked for 14 threads costs 5 to 30% more than
asked for one at these sizes (52.8 against 48.0 ns at n = 3, 177 against 136 at n = 10); the cause is not measured.

**Where logos starts to win** (one thread, logos / std::sort): 9x slower at n = 3 to 5, 2x at 10 to 32, then ahead
for good from n >= 100 on uniform, heavy_tail, bimodal, quantized and int_wide (0.50 at 100, 0.24 at 1000, 0.17 at 10M), from 316 on
few_distinct and the block shapes, from 1000 on near_sorted. **Per element, logos is cheapest at about a thousand elements** (7.5 ns on
uniform), not at the large end (13.4 at 10M).

**A step in the dispatcher between 25 and 76 elements.** A finer ladder (`bump_probe`, committed engine identical):

| n | 24 | 25 | 32 | 48 | 64 | 72 | 80 | 88 | 96 | 128 |
|---|---|---|---|---|---|---|---|---|---|---|
| logos ns per element | 17.7 | 36.3 | 24.3 | 25.7 | 25.3 | 26.7 | 15.0 | 10.8 | 10.7 | 9.8 |
| average runs in the input | 10 | 11 | 14 | 20 | 27 | 30 | 33 | 37 | 40 | 53 |

Predicted before it ran, and both edges landed: it starts at 25, where insertion sort hands over to run detection, and ends near 80,
where random data exceeds the 32 runs `detect_runs_limited` allows and it bails at once. In between the engine detects runs of two or
three elements, builds a relocation plan, has it declined, and only then sorts: 72 random doubles take 1.9 us and 96 take 1.0. Open.

**Two regressions of the knot route, a day old, found only by these sizes.** Its switches were read with `getenv` on every call
(three sorted elements: 6 ns -> 42), and its cost gate let the plan run on runs of two or three elements, where nearly every
element is a run endpoint so almost nothing crosses and the plan itself is the cost (random n = 32: 781 -> 2431 ns). Every A/B bench
here starts at 1M and the harness checks answers, not times, so neither could be seen. Fixed (switches read from 512 elements up; an
average run of at least 256 before the route is asked) and confirmed in one binary against the committed engine: 0.93-1.03 from
n = 0 to 3162.

**On drawing it.** A linear axis has one enemy, a fixed cost divided by a tiny n: the radix baseline's 7 us per call is 2300 ns per
element at n = 3 and flattened every other line in the first rendering. The ns/element axis opens at three times the 85th percentile
of what is drawn; the linear time chart, where the largest n towers by right, is capped only when one series peaks four times above
every other. Captions name what runs off the top; legend and tables keep the true values.

## 3ac. The knot route: comparisons only where runs cross; an infinity crashed the parallel sort; relocation no longer beats the partition at 14 threads (2026-09-19/20, `make knotbench`)

**The idea.** Read each run as a strand, the run descriptors sorted by first value as a linked list the sorted output walks along, and
a crossing as a value interval in which two or more strands both hold elements. Relocation (J5) merges every element of a chain of
overlapping runs and declines the array once chains hold half of it. The knot route sweeps the 2R strand endpoints in value order,
cuts each strand where the set of live strands changes, copies every piece that lies alone and merges only the pieces that share an
interval: O(R log R) plan, O(n) copies, O(crossing mass x log k) comparisons. Pieces are counted by the elements they hold, so a fused
run whose interval hides a hole reads correctly without gap-splitting. The target is a staircase: sorted blocks whose value ranges each
overlap the next, where one chain covers everything and J5 declines. `stairs_xNN` has NN% of its elements inside a crossing.

**Against the shipped engine** (`LOGOS_NO_KNOT` off vs on in one binary, paired, 40 pairs, floor 3.2% at 8M and 4.5% at 1M):

| route off / route on | cost | 8.4M, 1 thr | 8.4M, 14 thr | 3.1M, 14 thr | 1M, 14 thr | predicted at 8.4M (1 thr; 14 thr) |
|---|---|---|---|---|---|---|
| block_perm64 (no crossings) | 0 | 1.000 | 0.986 | 1.019 | 1.002 | 1.00; 1.00 |
| block_overlap | 0.03n | **1.099** | **1.131** | 1.095 | 1.027 | 1.10-1.25; 1.03-1.15 |
| stairs_x10 | 0.10n | **4.44** | **1.20** | 1.23 | 1.38 | 2.2-3.5; 1.5-2.2 |
| stairs_x20 | 0.20n | **4.05** | **1.32** | 1.20 | 1.33 | 2.0-3.2; 1.4-2.0 |
| stairs_x40 | 0.39n | **3.42** | **1.35** | 1.21 | 1.40 | 1.8-2.8; 1.1-1.6 |
| stairs_x60 | 0.59n | **2.93** | **1.31** | 1.27 | 1.44 | 1.6-2.5; 0.95-1.3 |
| stairs_x80 | 0.79n | **2.62** | **1.39** | 1.18 | 1.29 | 1.4-2.2; 0.85-1.15 |
| stairs_x100 | 0.98n | **2.64** | **1.50** | 1.30 | 1.44 | 1.3-2.0; 0.75-1.05 |
| triple (three strands live) | 1.57n | **2.58** | 1.24 (declined) | 1.29 | 1.30 | 0.9-1.5; 0.6-0.95 |
| quad (3-4 live) | 1.96n | **2.68** | 1.09 (declined) | 1.04 | 1.02 | |
| k8 (8 live) | 2.95n | **1.40** | 1.15 (declined) | 1.21 | 1.28 | |
| stairs_ord_x20 (in value order) | 0.20n | **4.02** | 1.02 | 1.00 | 1.01 | 2.0-3.2; 1.00 |
| zigzag_x20 (every other block descends) | 0.20n | **3.73** | 1.05 | 0.99 | 0.98 | 2.0-3.2; 1.4-2.0 |
| shards14_x20 | 0.19n | **1.85** | 1.09 | 0.98 | 0.98 | 2.0-3.2; 1.3-1.9 |
| uniform, few_runs, segmented, near_1pct | | 1.000-1.015 | 0.988-1.055 | 0.988-1.027 | 0.971-1.030 | 1.00 |

The rows marked declined still win, because the knot route's sample gate declines in half a millisecond where J5 scans the whole
array before its chain gate says no. shards14 at one thread is 1.8x when the shipped fallback would have been the run merge (8 fused
runs) and 3.6x when it would have been the sequential leaf (9): the route itself takes 23 ms for every shuffle.

**Against the pure partition**, which is what the route itself is worth. On a shuffled staircase the shipped relocation scans for
2.8 ms and then bails (traced), and that sits in the baseline above. Two engine copies, this engine against `-DLOGOS_NO_RELOCATE`,
both orientations, layout cancelled, 8.4M, 30 pairs:

| engine / pure partition | 1 thread | 14 threads |
|---|---|---|
| block_perm64 (J5's own target, no knot work) | 4.47 | **0.95** |
| block_overlap (J5's own) | 4.37 | **0.92** |
| stairs_x10, x20 | 4.14, 3.80 | 0.96, 0.97 |
| stairs_x60, x100 | 2.78, 2.55 | 1.03, 1.11 |
| triple (accepted in that build) | 2.43 | **0.89** |
| zigzag_x20, shards14_x20 | 3.54, 1.77 | 1.01, 1.08 |

At one thread the route is worth 1.8-4.1x. At 14 threads it ties the partition on two-strand crossings and lost 11% on three strands,
so the parallel gate is 1.0n; declining three strands early raised that row against the shipped engine from 1.16 to 1.24. In absolute
terms (one process, minimum of 11, 8.4M, 14 threads): uniform random 15.4 ms, the staircase with the route off 11.6, on 9.0.
**Relocation's own parallel win is gone**: 1.9-2.2x at 20 threads on 2026-09-10, 0.92-0.95 against today's partition. J5 did not
change; the partition did, and the comparison was never re-run. Not acted on: see the open directions in CLAUDE.md.

**What was wrong along the way.** The sweep first cut every endpoint below its value, which put each strand's last element at the
head of the next interval and read 98.5% crossing mass on a staircase built with 10%; the trace build caught it before any timing.
The gate was first J5's n/2, on a predicted break-even of 60-100% crossing at 14 threads; lifted, the two-strand ladder never breaks
even against the shipped engine, and the n/2 gate charged 4-9% for late declines on the shapes just past it. The placement inherited
relocation's one-thread-per-500k cap, measured on copies; at 1M it merged on 2 threads what the partition sorted on 10 and the
heavily crossed rows lost (0.84-0.99) until the cap counted work instead of elements (1.26-1.43).

**Infinities.** The knot harness's infinite-endpoint shape segfaulted with the route off. Uniform doubles with `a[0] = -INFINITY` at 4
threads crash the committed engine in `hf_scatter`; position 0 is always a sample point. Eight of ten shapes crashed. One cause in four
places: a value-linear map over an infinite range computes `inf * 0 = NaN`, and NaN fails every compare, so the uniformity certificate
measured a distance of 0 and certified the data, `isfinite` passed a splitter-table scale of `cells / inf = 0`, and the leaf's flash and
coarse buckets cast NaN to a class. Fixed off the per-element paths; 720 sweep checks clean under ASan, UBSan and
`float-cast-overflow`, 90 of them now in the harness. The whole change against the committed engine, layout cancelled: 0.987-1.021 on
five standard shapes at 1 and 14 threads.

## 3ab. heavy_tail was not slower than before; the key grid makes it faster (2026-09-13, `make kgbench`)

**The complaint.** heavy_tail benchmarked "very slow" in the scale report: 104 ms at 14 threads and 10M against uniform's
19.9. **The profile said otherwise.** Twelve interleaved sorts each in one profiled process: the scatter cost heavy_tail 57.3
CPU ns per element and uniform 28.1, a ratio of 2.04x, identical to the 2026-09-11 profile. Uniform is certified and buckets
with one multiply; heavy_tail is certified by nothing and pays the key splitter table, whose dependent memory loads varied
2.4x run to run on identical data. The report's rows are measured minutes apart, so it caught one at its best and the other
at its worst.

**The fix the profile pointed at.** A key grid: equal-width cells in total-order key space, bucket edges snapped to the cells
holding sample quantiles, one byte load per lookup, kept only if the balance check passes on the sample.

| grid off / grid on, 14 threads | 8.4M | 1M | predicted |
|---|---|---|---|
| heavy_tail (Pareto 1.2) | **1.36** | **1.83** | 1.2-1.35 |
| lognormal (sigma 2) | **1.40** | **1.67** | 1.1-1.3 |
| pareto_0.8 | **1.33** | **1.75** | 1.1-1.3 |
| exponential | **1.32** | **1.54** | 1.1-1.25 |
| bimodal, mixed-sign heavy tails, uniform | 1.00-1.01 | 0.99-1.00 | 1.00 |
| clustered, int_wide | 0.99-1.00 | 1.01-1.03 | 0.95-1.2 |
| every shape at 1 thread | 0.98-1.05 | 0.98-1.01 | 1.00 |

Predicted 1.2-1.35x on heavy_tail at 14 threads; measured 1.36x at 8.4M and 1.83x at 1M. The exponent map had tried the idea
with one linear piece per binade and lost, because a heavy tail decays inside each binade; the grid resolves hundreds of cells
per binade and that resolution is the whole difference.

## 3aa. The hash-count gate, a signed-zero defect, and what the oracle got wrong (2026-09-12, `make cardgate`, `make zerodiag`)

**The ground truth came first.** Every shape the old gate admitted, run with the route on and off in one binary. At 14
threads the hash map lost on 7 of 8, up to 5.3x. The new gate, against the old:

| shape | old gate / new gate, 14 threads | 1 thread |
|---|---|---|
| hash_60k (60k equal-odds doubles) | **4.46** | 0.95 (gated out before too) |
| hash_30k | **2.45** | 1.02 |
| prices_cents (lognormal cents) | **2.70** | 1.20 |
| user_ids_zipf (200k sparse ids, Zipf) | **1.88** | 1.07 |
| mantissa16 (65536-value lattice) | **1.40** | 0.99 |
| hash_5k | 1.23 | 1.19 |
| yyyymmdd | 1.10 | 1.02 |
| zipf_pareto (where the route wins) | 1.01 | 1.07 |
| guards: uniform, few_5 | 1.01-1.02 | 1.00-1.01 |

**At one thread the fallback decides.** The hash map beats the leaf's float radix and loses to its AVX2 quicksort, so the
gate asks the leaf's own byte probe of the sample. Version 2 on fresh shapes:

| fresh shape | sample Chao1 | v1 / v2 | v2 / route off |
|---|---|---|---|
| Zipf over 2000 integers below 2^16 | 567 | **1.76** | 0.57 |
| Zipf over 20000 integers below 2^20 | 857 | **1.84** | 0.60 |
| Zipf over 3000 halves below 2^14 | 499 | **2.02** | 0.52 |
| whole numbers, exponential, mean 20 | 103 | 1.02 | 1.00 |
| equal odds over 8000 integers (Chao1 above 3300) | 5343 | 0.99 | 0.97 |
| prices to the cent (AVX2 fallback) | 4857 | 0.98 | 1.01 |
| ratings 1.0-5.0 by halves (low-cardinality route) | 9 | 0.99 | 0.98 |

**Signed zeros.** A byte compare against qsort misjudged two correct sorts; a proper check then found four places that
really did turn -0.0 into +0.0 or swap the signs of equal zeros:

| shape, 1 thread | n | -0.0 in | -0.0 out | +0.0 in | +0.0 out |
|---|---|---|---|---|---|
| integers near zero | 65535 | 763 | **0** | 742 | 1505 |
| few distinct {-0, 0, 1, 2} | 4095 | 992 | **0** | 1001 | 1993 |
| integers holding only -0.0 | 32768 | 744 | **0** | 0 | 744 |
| random doubles + 1% zeros | 2000 | 15 | **23** | 16 | **8** |
| tenths near zero | 1000 | 5 | 4 | 4 | 5 |
| any shape | 65536 and 300001 | | unchanged | | unchanged |

Fixed, with the vectorized check folded into the NaN scan's one compare: uniform 1.001 and heavy_tail 0.991 at one thread, every 14-thread row within 4% (24 pairs, layout cancelled).

## 3z. Both routes, one step further: a parallel tail merge and a stream lookup table (2026-09-12)

**The tail route at 14 threads.** Scan and copy in parallel, sort the tail in scratch, cut the prefix at P-1
positions, search each cut in the tail, merge every piece straight into the array.

| shape | 14 threads, sequential form (first cut) | parallel form, run 1 | parallel form, run 2 |
|---|---|---|---|
| 99% sorted prefix | 0.866 | 1.014 | 1.006 |
| 95% prefix | 0.811 | 1.166 | **1.172** |
| 90% prefix | not run | 1.227 | **1.215** |
| 80% prefix | 0.578 | 1.157 | **1.170** |
| 50% prefix | 0.336 | 1.034 | 1.047 |
| guards: 49% prefix, uniform, near-sorted, sorted, reverse | 0.96-1.01 | 0.99-1.03 | 0.98-1.02 |

Predicted a win at 99% and a loss on large tails; measured the opposite on both ends. The gate the prediction
justified is gone, and parallel sorts take the route at every prefix of half or more.

**The stream lookup.** An exact 4096-cell table in place of a compare per boundary, table against compares:

| streams | run 1 | run 2 | run 3 (table forced) |
|---|---|---|---|
| 4 equal | 0.810 | 0.802 | 0.794 |
| 8, 1% perturbed | 0.678 | 0.663 | 0.705 |
| 8 shards at rates 1/(s+1) | 1.096 | 1.042 | 1.112 |
| 12 equal | | | 1.013 |
| 16 equal | 1.208 | 1.234 | 1.197 |
| 60 equal | **2.744** | **2.715** | **2.788** |

The table wins from 16 streams and loses at 4 and 8, so it runs from 12. As shipped, route off against on:

| shape, lookup used | 20 pairs | 30 pairs | before the table (earlier run) |
|---|---|---|---|
| 4 equal streams, compares | 2.686 | **3.209** | 3.033 |
| 8 shards at rates 1/(s+1), compares | 1.889 (p10-p90 1.29-2.86) | **2.709** | 2.634 |
| 8 streams, 1% perturbed, compares | 2.351 | **2.357** | 2.353 |
| 12 equal streams, table | 1.221 (p90 2.69) | **2.636** | not run |
| 16 equal streams, table | 2.374 | **2.646** | 2.155 |
| 60 equal streams, table | **3.152** | 2.866 | 1.197 |
| every shape at 14 threads | 0.94-1.04 | 0.95-1.03 | 0.97-1.03 |
| guards at 1 thread | 0.98-1.02 | 0.98-1.02 | 0.97-1.02 |

**A trace build inverted the answer**: it timed the table 1.3x faster on the perturbed shape, the normal build
0.7x. Trace builds show which path runs; only the normal build shows how fast.

## 3y. The routes the probe pointed at (2026-09-12, `make prefixbench`, `make streamsbench`)

The held-out quadtree test (3x) named two structures the engine left several times over on the table at one
thread. Both routes are now built. Each is one binary switched at runtime, measured with the paired harness.

**Sorted prefix plus an unsorted tail.** The prescan walks an ascending prefix to its first descent, so the
prefix length is free; sort only the tail and merge from the back.

| shape | 1 thread | 14 threads, first cut | 14 threads, shipped |
|---|---|---|---|
| 99% sorted prefix | **4.29-4.55x** | 0.866 | 1.000 |
| 95% prefix, the zoo's shape | **3.57-3.95x** | 0.811 | 0.994 |
| 80% prefix | 2.09-2.20x | 0.578 | 1.019 |
| 50% prefix, the gate's edge | 1.12-1.21x | 0.336 | 1.020 |
| guards: 49% prefix, uniform, near-sorted, sorted, reverse | 0.97-1.01 | 0.96-1.01 | 1.00-1.01 |

The first cut regressed at fourteen threads because its NaN scan and merge are single-threaded, the same
mistake the dense-integer route made. It now runs only in sequential sorts. Predicted 6-9x and 1.3-1.8x on
the 95% prefix; measured 3.6-4.0x and, at fourteen threads, a regression that the restriction removed.

**Interleaved ascending streams.** A stable scatter keeps arrival order inside each bucket, so if the
buckets are the streams, every bucket is already sorted. Streams come from gulfs in the sample; ascent is
checked on a position-ordered sample; any bucket out of order falls back to the leaf.

| shape | 1 thread | 14 threads |
|---|---|---|
| 4 equal streams | **3.03x** | 1.01 |
| shard IDs, 8 shards at rates 1/(s+1), the zoo's shape | **2.63x** | 1.00 |
| 8 streams with 1% of values perturbed | 2.35x | 1.00 |
| 16 equal streams | 2.16x | 1.00 |
| 60 streams, near the 64-mode cap | 1.20x | 1.03 |
| guards: uniform, bimodal, clustered, heavy_tail, few_distinct, sorted_runs | 0.97-1.02 | 0.97-1.03 |

Predicted 4-6x on shard IDs, measured 2.63x, with the ordering across stream counts predicted correctly. That
ordering is the diagnosis: the speedup falls as the stream count rises, tracking a per-element compare against
every boundary. A one-multiply bucket reached 5.9x in the zoo, so a direct-index stream lookup is the next step.

## 3x. Routing on a quadtree over (index, value): the held-out test (2026-09-12, `make zroute`)

Every element becomes the point (position, value). Convert a sample's positions and values to ranks and
count the occupied cells of a 2^l x 2^l grid at each level, dividing by 2^l. Sorted data reads 1.00 at every
scale, and the profile's shape over l pictures the arrangement.

The rule that turns a profile into a class was fixed in advance from six training shapes, confirmed to
reproduce all six as coded (`make ztrain`), and then applied without change to the chaos explorer's
twenty-shape zoo. Predictions were written before the run.

| | predicted in advance | measured |
|---|---|---|
| the probe's own output matched the prediction | | 17 of 20 shapes |
| arrangement shapes classified correctly | about 4 of 8 | **4 of 8** |
| value-set shapes where it rightly abstained | 10 of 10 | 9 of 10 |

The decisive result is where the misses landed:

| arrangement shape | gap at 1 thread | gap at 14 threads | profile read | correct |
|---|---|---|---|---|
| sorted_plus_tail | **9.36x OPPORTUNITY** | **1.79x OPPORTUNITY** | tail | yes |
| shard_ids_disjoint | **5.91x OPPORTUNITY** | 0.76x redundant | k-overlap, k=9 (8 streams) | yes |
| sorted_runs_16 | 0.65x redundant | 0.55x redundant | k-overlap, k=16.5 | yes |
| reverse_near_sort | 0.99x redundant | 0.65x redundant | bounded, descending | yes |
| event_ts_latency | 0.28x moot | 0.20x moot | unknown | no: disorder of 4096 is two sample spacings |
| window_shuffle | 0.33x moot | 0.18x moot | sorted | no: disorder of 2048 is one sample spacing |
| sorted_runs_256 | 0.40x moot | 0.30x moot | random | no: 256 pieces saturate the sample ceiling |
| shard_ids_blocks | 0.86x moot | 1.19x moot | tail | no: a false tail |

On every arrangement shape where the engine left 1.2x or more against a hand-routed ceiling, the profile
named the structure correctly: **blind on 0 of 3.** All four misses were on shapes the engine already beats
its ceiling on.

Four limits bound that. The evidence is thin, three opportunities. An opportunity is a classification, not a
speedup: neither the sorted-prefix-plus-tail route nor a sequential stable scatter exists yet to take one. The
tail rule is too loose, firing on shard_ids_blocks and on Gray-code order; on Gray code the probe was right
that arrangement exists, since consecutive codes differ by one bit, but the label was wrong, and it was not
retuned on this zoo. And the blind spot below the sample spacing is structural: order finer than n/m needs a
linear scan.

## 3w. The parallel int32 path asks what the type already answers (2026-09-11, `make intbench`)

An int32 array is a lattice by construction. The step is exactly 1, the base is the
minimum, and unlike the f64 case there is no tolerance to pick, no NaN to screen, and no
possibility of two distinct values landing on one slot, because `int32 -> uint32` is a
bijection. None of that has to be discovered. It follows from the type.

The sequential leaf uses it: `leaf_i32` runs a counting sort when the span fits and a
byte-skip LSD radix otherwise. `psort_i32` does not. It runs prescan, then samples, then
sorts that sample with libc `qsort`, then builds splitters, histograms and scatters, for
every input it is given. It has no counting, lattice, low-cardinality, relocation or
near-sorted route.

Racing one thread against fourteen on a span ladder, 8M elements, min of 7, the two
thread counts measured adjacently:

| span (max - min) | 1 thread | 14 threads | P/1 |
|---|---|---|---|
| 100 (categories) | 37.8 ms | 134.2 ms | **0.28** |
| 1e3 (scores) | 31.6 | 95.8 | **0.33** |
| 1e4 | 30.3 | 91.5 | **0.33** |
| 1e5 (zip-ish) | 44.9 | 91.7 | **0.49** |
| 1e6 | 76.0 | 77.4 | 0.98 |
| n = 8e6 | 168.3 | 64.3 | 2.62 |
| 1e8 (> n) | 105.2 | 62.5 | 1.68 |
| full 2^32 | 132.0 | 80.1 | 1.65 |

**FIXED, same day.** `count_try_i32` gives the parallel path the counting route it
lacked, and it asks nothing: parallel min/max, per-thread tables filled from an atomic
chunk queue, each thread folding the P tables over a slot range it owns alone, then
emitting. Slot order is value order, so each thread's output is one contiguous run and
there is no merge.

| span | P/1 before | P/1 after | 14T after |
|---|---|---|---|
| 100 (categories) | 0.34 | **2.88** | 3.8 ms |
| 1e3 (scores) | 0.33 | **3.97** | 3.3 |
| 1e4 | 0.34 | **4.66** | 2.8 |
| 1e5 (zip-ish) | 0.46 | **3.33** | 5.1 |
| 1e6 | 0.88 | 0.87 | declines at the pre-gate |
| n = 8e6 | 2.37 | 2.64 | untouched |
| 1e8 | 1.78 | 1.73 | untouched |
| full 2^32 | 1.79 | 1.81 | untouched |

Prediction and grade: I predicted 14 threads would fall from about 36 ms to 6-12 ms and
P/1 to between 1.0 and 2.0. Measured 2.8-5.1 ms and 2.9-4.7, so the prediction was too
conservative in both, because the counting sort's write pass is sequential per thread and
scales better than the scatter it replaced.

Two things the first cut got wrong, both already in the ledger:

- **The span must be 64-bit.** INT32_MAX minus INT32_MIN is -1 in 32-bit arithmetic, so a
  `span <= budget` gate written in 32 bits passes on the widest possible input and then
  sizes a table from -1. The sequential leaf was already correct; the trap was latent for
  new code and the harness now pins it.
- **Pre-gate on the caller.** The first cut ran the full parallel min/max and only then
  declined, which cost the span-1e6 row (0.88 down to 0.55). A sample's span is a LOWER
  bound on the true span, so a sample that already overflows the table proves the array
  does, with no false negatives. Probe on the calling thread before paying for threads,
  which is the J5 lesson verbatim.

**The sentinel case, fixed the same day.** The gate read raw min/max, so one far sentinel
destroyed it. Bulk of 100 distinct values: 3.2 ms at 14 threads. Add a single
INT32_MIN and it is 36.3 ms, **11x for one element out of eight million**. Adding 5%
INT32_MIN is 34.6 ms, and both extremes 37.4 ms. The sign itself is free, as predicted:
zero-straddling data (3.2 ms) and all-negative data (3.3 ms) match the baseline exactly,
because the bias map is affine and the byte-skip already discovers whether the top bit
varies. The fix is not about negatives, it is about tails. `count_try_i32` now takes its table
window from the SAMPLE instead of the extremes. The pre-gate already tracks the five
smallest and five largest sample points, which gives both the plain min/max gate and a
trimmed window in one pass. When the true span overflows the table the route no longer
gives up: it counts the window, spills everything outside to per-thread lists, and sorts
those as their own runs. The tails sit outside the window on both sides and slot order is
value order, so the output is low-tail, then bulk, then high-tail, with no merge.

| shape, 14 threads | before, x baseline | after, x baseline |
|---|---|---|
| bulk + one INT32_MIN | **11.3x** | **1.19x** |
| bulk + both extremes | 11.7x | 1.25x |
| bulk + one -1 (near the bulk) | 1.06x | 1.04x |
| bulk + 5% INT32_MIN | 10.8x | declines, by design |
| zero-straddling, all-negative | 1.00x | 1.00x |

Ratios are against the baseline row of the same run, since the box drifts 5-10x between
runs. In absolute terms the sentinel row went from 36.3 ms to 5.0 ms while its own
baseline moved from 3.2 to 5.5.

Two limits, both deliberate. The spill budget is n/32 and the trim is five points out of
2048, so this absorbs sentinels rarer than about 0.25% and a 5% sentinel still declines to
the partition. A tail worth more than a thirty-second of the array is not a tail, and the
general answer for a frequent-but-distant value is gulf-splitting with a second table,
which the f64 path has and this one does not. Separately, the 1-thread path is untouched:
`leaf_i32` still gates on raw min/max and still pays 59.9 ms against a 24.4 ms baseline on
those shapes.

The crossover is at a span of about n/8. Below it the parallel path pays a sample, a
histogram and a full scatter of 8M elements to do work that is two passes over a count
array, and fourteen threads finish later than one core would have.

This is the f64/i32 divergence showing its bill. The certificates, the histogram-free
partition, the sample-once dispatcher, the fast sample, the context pool, the AVX2 leaf
and the removal of libc `qsort` from every sampled path were all built on the f64
dispatcher. The i32 parallel path predates all of them.

Two things it can assume rather than measure, both provable rather than probable:

- **The lattice.** Step 1, base at the minimum, gate `span <= budget`. The prescan
  already walks the whole array looking for ascending and descending runs, so min and max
  cost nothing extra there.
- **One certificate, not two.** The order-preserving int32 key is
  `(uint32_t)x ^ 0x80000000u`, which over the signed range is exactly `x + 2^31`: monotone
  and affine. The Kolmogorov-Smirnov statistic is invariant under a monotone affine map,
  so the value-space and key-space certificates return the identical number for integers.
  For doubles `total_key` is logarithmic and the two tests genuinely differ, which is the
  entire reason mode 7 earns its keep.

The general rule this sharpens: **a type fact may be assumed, a data fact must be
certified.** The test is whether a counterexample is representable at all, not whether it
seems unlikely. The exponent map (§3q) failed by getting this backwards, treating "binades
are uniform inside" as structural when it was merely plausible.

## 3v. Scheduling the leaves by size: a good idea with no target (2026-09-11, `make lptbench`)

The parallel partition hands leaves to threads with one atomic increment each. Whichever
leaf a thread picks up last decides when everyone finishes, and the leaves are handed out
in value order, not size order. Classical scheduling says that is the wrong order: greedy
self-scheduling in an arbitrary order has a makespan of at most (2 - 1/P) times optimal,
while longest-processing-time-first tightens it to (4/3 - 1/(3P)), and both bounds are
tight. The sizes are known exactly at that point, so ordering them is free.

Measured with the paired harness, one binary switched at runtime so there is no
code-layout bias, 24 pairs at 8M on 14 threads:

| shape | ratio (>1 means largest-first wins) |
|---|---|
| uniform (control) | 1.046 |
| Pareto a=1.2 | 0.951 |
| exponential | 1.005 |
| lognormal | 0.999 |
| bimodal | 0.980 |
| clustered x16 | 1.011 |
| one fat region | 0.987 |

Everything inside the floor. Rather than call it noise, the mechanism was traced, and it
gives a clean reason. **The splitters are sample quantiles, and equalising the buckets is
precisely what a quantile splitter does.** Largest leaf over the mean:

| shape | imbalance |
|---|---|
| uniform | 1.10x |
| Pareto a=1.2 | 1.48x |
| (what the balance check would allow) | 3.0x |

Feed that to the greedy bound, W/P + L(1 - 1/P), with 168 buckets on 14 threads, so 12
leaves per thread. Uniform: 12 + 1.10 x 0.93 = 13.02 against an optimum of 12, so the
ENTIRE straggler cost is 8% of the leaf phase. Pareto: 11%. Largest-first recovers only
part of that, leaving a ceiling of 1-3% of one phase and less of the sort, which is under
what this box resolves. The measurement and the theory agree.

The lesson is worth more than the feature: **measure the imbalance before optimising the
schedule.** The partition had already solved the load-balancing problem upstream, and a
scheduler can only recover slack that exists. Kept opt-in as `LOGOS_LPT=1`.

Where the same argument says the idea SHOULD pay: the counting and lattice routes carve
static per-thread ranges rather than quantile buckets, and their imbalance was measured at
1.3-2x (FINDINGS 3j). That is a real target and it is untouched.

## 3u. Measuring smaller than the noise: the paired A/B (2026-09-11, `make pairprobe`)

Two defects had left the resolving floor at about 1.25x: separately compiled
copies differ by up to 1.24x from code layout (3r), and the machine itself runs
5-10x slower hot than cold (3t). Three ways of fixing the machine were tried
before the one that worked.

**Pinning the benchmark to specific CPUs: rejected, and the reason is
structural.** `taskset -c 0-7` at 8 threads cut the 1-thread max/min spread
from 1.40 to 1.13, which looked decisive. It did not reproduce: the same pinned
configuration gave 1.13 then 1.32 on consecutive runs, while unpinned 12-thread
gave 1.12. The test that settles it is a single thread pinned to CPUs 12-19,
which on a 6P+8E part should be the slow efficiency cores. It ran *faster* than
unpinned. WSL2's CPUs are virtual: the Windows hypervisor places them on
physical cores as it likes, so `taskset` constrains a number, not a core.
cgroup v2 has the cpuset controller enabled but writing the tree needs root,
and it would hit the same boundary anyway.

**Changing the thread count: no change.** The earlier reading that 14 threads
is best came from separate runs, which the drift result invalidates. Re-run
with the counts rotated inside each rep so every count sees the same thermal
state (20 reps, 8M):

| threads | min (ms) | max - min |
|---|---|---|
| 8 | 26.36 | 2.18 |
| 10 | 23.01 | 1.80 |
| 12 | **19.16** | 1.93 |
| 14 | 19.24 | 2.22 |
| 16 | 20.35 | 1.54 |
| 20 | 21.21 | 1.30 |

12 and 14 tie on the minimum, and the spread now favours MORE threads, the
opposite of the un-interleaved conclusion. That spread is mostly drift, which
is the wrong thing to optimise. BENCH_THREADS stays 14.

**Pairing the variants: adopted.** If the machine's speed wanders on a scale of
tens of seconds, the fix is to make the two measurements adjacent rather than
to make the machine steady. `bench_paired()` sorts one half of an array with A
and the other half with B back to back, alternates which goes first each rep
(the first half runs 2-4% fast-or-slow depending on destination page state, and
alternating cancels it), and reports the median of tA/tB with p10 and p90.

`make pairprobe` runs that harness with the same engine on both sides, so the
true ratio is 1.000 and the output is purely this box's error:

```
paired null test: identical engine in both halves, 40 pairs, 14 threads
  size          median       p10       p90    resolves
  1000000        1.006     0.800     1.308        6.1%
  8000000        1.028     0.933     1.188        3.7%
```

Read that as: one pair tells you almost nothing (p90 is 19-31% off a ratio that
is exactly 1), the median of 40 resolves about 3.7% at 8M, and error falls as
1/sqrt(pairs) so 160 pairs buys about 2%. The floor goes from ~1.25x to ~1.04x,
which is what makes the small tweaks worth chasing at all. Methodology rule 3
amended; run `pairprobe` the way you read a calibration line, before trusting
any paired number.

## 3r. The interleaved A/B has a per-copy layout bias (2026-09-11)

Every A/B here compiles N copies of the engine with different macros, renames
their public symbols and races them inside one process. That controls for
machine load, which was its purpose. It does not control for the copies.

The null test — four copies, identical source, identical flags, only the
symbol names differing — gave columns that differed by up to 1.16× on a quiet
box, with one copy systematically fastest across unrelated shapes; a second
build put the spread at 1.24× (int_wide). The cause is code and data layout:
symbol length changes function alignment. It is deterministic, so reps and
best-of-N do not remove it.

The working control is to permute which copy carries which variant and
require the effect to follow the variant. Applied to the key-space
certificate: 1.32× with the feature in copies 1 and 3, 1.15× with it in
copies 0 and 2. It follows the feature, so the effect is real, and its size
is about 1.2–1.3× rather than the 1.45–1.50× three uncontrolled runs gave.

This is now rule 3 of the methodology, and TRUTH_TABLE §N2 lists which of the
day's results sit above the band (they stand) and which sit below it (their
end-to-end ratios are unconfirmed, though the mechanisms measured directly by
trace are unaffected).

## 3q. Structures for a heavy tail (2026-09-11, `make tailbench`)

Asked what structure to pivot to when a tail is recognised, the sample
answers it directly. The Kolmogorov-Smirnov distance from uniform, measured
on a 5376-point sample of each distribution, in value space and in the
order-preserving key (which is monotone in the logarithm):

| data | uniformity in value space | in key space | map the partition now uses |
|---|---|---|---|
| uniform | 0.013 (passes) | — | equi-width grid on the value |
| Benford / log-uniform | 0.746 | **0.018 (passes)** | equi-width grid on the **key** |
| Pareto α = 1.2 | 0.985 | 0.692 | key splitter table (exponent map rejected by the balance check at 4.8×) |
| lognormal, exponential | 0.9+ | 0.7+ | key splitter table |
| one value holding 50% | — | — | declines before scattering |

(a sample of 5376 certifies below 0.027)

So the log domain rescues log-uniform data completely and barely dents a true
Pareto, because the log of a Pareto is exponential, not uniform. Three things
shipped from this and one was rejected.

**Key-space certificate.** Benford at 14 threads, about 1.2–1.3× under the
copy-swap control (1.32× with the feature in copies 1 and 3, 1.15× with it in
copies 0 and 2). Three uncontrolled runs had said 1.45–1.50×; that figure was
reading the bench's per-copy layout bias as well (§3r). A trace confirms the
mechanism: Benford is the only shape in the suite that certifies in key space
(0.018 against a 0.027 limit) and it then buckets with one multiply and spills
nothing. It cannot fire where the value certificate fires, so uniform data is
untouched by construction.

**Balance check.** The partition's trial times the lookup, not the partition,
and selected a map that sent 63% of the data to the spill list and then
declined the whole partition after a full pass. Every candidate map is now
applied to the sample and dropped if any bucket would take more than three
times its share, at a cost of about 16 thousand lookups. On Pareto it fires
at 4.8× and the sort falls back to the key table with zero spills.

**Dominant-duplicate guard.** One value holding half the data now declines
the partition in 2.3 ms from the sample, rather than after scattering 8
million elements.

**Exponent-histogram map: rejected, opt-in.** See the ledger (C25): correct
after its index was fixed, and 0.91× and 0.80× of the key table on the two
shapes that select it.

Measurement caveat: the box ran between 2× and 6× off its quiet calibration
all day (2.9 ms quiet, 17.6 during the decisive run, 6.4 immediately after).
Columns running identical code spread 12–18%, so only the Benford gain and
the exponent map's losses clear that band; nothing else in the run is
claimed, and a quiet re-run of `tailbench` is owed.

## 3p. A cardinality cliff, and the counting route's lookup (2026-09-11, `make cardbench`)

Asked why logos loses few-distinct data to x86-simd-sort by 2.6× at one
thread, a kernel microbenchmark of the per-element lookup found something
larger. The low-cardinality route scans a small table linearly up to 32
distinct values and binary-searches above it, and its gate admits 64:

| table size | shipped lookup | SIMD rank | perfect hash | + 4 counters | direct index |
|---|---|---|---|---|---|
| 5 | 2.02 ns | 1.71 | 1.07 | 0.92 | 0.80 |
| 64 | 30.84 ns | n/a | 1.09 | 0.96 | 0.82 |
| 256 | 36.35 ns | n/a | n/a | n/a | 0.79 |

End to end at 8M, before the fix: 5 distinct 29.1 ms at one thread, 33
distinct 227.9, 50 distinct 236.5, 200 distinct 35.6, 1000 distinct 35.1.
Data with 33 to 64 distinct values sorted 6.5× slower than data with 200,
because above 64 the gate declines and the cheaper moderate-cardinality
route takes over. No shape in any bench had a cardinality in that window.

Shipped: a multiply-shift perfect hash over the distinct values the gate's
sample already found, into a byte table, with every lookup verified by a key
compare so a value the sample missed falls into the old incremental table.

Interleaved, best of 5, rotated order, n = 8M (calibration memcpy 4.0 ms; the run was still ~2× off
quiet, so the guard rows carry a ±10% band and only the large ratios are decisive):

| shape | thr | scan (shipped before) | perfect hash | ratio |
|---|---|---|---|---|
| 33 distinct, lattice | 1 / 14 | 255.8 / 30.4 | **14.7 / 7.4** | 17.4× / 4.1× |
| 50 distinct, lattice | 1 / 14 | 276.1 / 36.5 | **18.0 / 5.8** | 15.3× / 6.3× |
| 64 distinct, lattice | 1 / 14 | 309.7 / 36.9 | **16.3 / 5.6** | 19.0× / 6.6× |
| 33 distinct, arbitrary | 1 / 14 | 253.3 / 31.5 | **15.9 / 7.7** | 15.9× / 4.1× |
| 50 distinct, arbitrary | 1 / 14 | 286.4 / 34.4 | **16.5 / 6.2** | 17.4× / 5.6× |
| 64 distinct, arbitrary | 1 / 14 | 317.4 / 74.9 | **15.6 / 13.1** | 20.3× / 5.7× |
| 5 distinct, lattice | 1 / 14 | 29.2 / 9.1 | **15.0 / 7.8** | 1.95× / 1.17× |
| 5 distinct, arbitrary | 1 / 14 | 30.2 / 8.4 | **16.4 / 6.8** | 1.84× / 1.23× |
| 200 distinct (gate declines, other route) | 1 / 14 | 34.5 / 10.3 | 36.0 / 8.7 | 0.96× / 1.18× |
| quantized, mod_card_30k, uniform, near_sorted | 1 / 14 | | | 0.91–1.12×, inside the run's band |

Variant C, how wide the gate should be now that the lookup is O(1), is **unresolved**. Its two builds
widened the gate and the hash table together, which confounds them, and the machine degraded during the
run (memcpy 2.9 → 38 ms in bursts), so its column is not reported. What is established: the multiplier
search finds a collision-free table on the first or second attempt for every cardinality the shipped
gate admits (verified for 5, 33, 50 and 64 keys at 12 bits, on a lattice and off it), and fails at 200
keys in a 4096-entry table, so any future widening must grow the table with the gate.


Rejected: the lattice's direct index ahead of the scan (§ledger C24) — the
cheapest kernel in isolation, 24× the table footprint, and it lost.

## 3o. The serial fixed costs (2026-09-11, `make probebench`)

Is the probe parallelised? No, and it should not be: threads are created
per phase and a pthread_create costs about what the stage-1 sample costs.
The trace (uniform, 14 threads) put the serial work before the workers at
about 2 ms per call regardless of n (5376 random reads taken one miss at a
time, two qsorts, the near-sorted and relocation gates) and the serial step
between scatter and leaves at 0.8 to 1.8 ms (fourteen context allocations
sized to the largest bucket, fresh every call): 9% of an 8M sort, 21% of a
1M one. Three changes: issue every prefetch of a sample stage before the
first read (the line-fill buffers overlap the misses), sort the sample with
the AVX2 quicksort, and keep the contexts' buffers in a persistent table
indexed in order of use within one top-level sort.

Trace after (same load): serial work before the workers ~0.9 ms per call
(stage-1 sample 0.15 ms; the remainder is the two gates that still read the
array and the KS distance), scatter-to-leaves step 0.09 ms.

SHIPPED as the default: the trace shows the mechanism and the quiet half of the end-to-end A/B confirms 1.07-1.18x at 1M and 1.02-1.10x at 8M on the parallel rows with the 1-thread rows unchanged; the persistent bundles cost about 6 bytes per element of the largest capacity ever requested (48 MB after an 8M single-threaded call), the same order as the existing buffer pool.

| shape | n | thr | neither | fast sample | ctx pool | both | both / neither |
|---|---|---|---|---|---|---|---|
| uniform | 1M | 14 / 7 | 3.45 / 3.44 | 3.16 / 3.21 | 3.32 / 3.21 | **3.05 / 3.12** | 1.13 / 1.10× |
| near_sorted | 1M | 14 / 7 | 2.27 / 2.02 | 2.18 / 1.83 | 2.27 / 1.91 | **2.05** / 2.06 | 1.11 / 0.98× |
| few_distinct | 1M | 14 / 7 | 1.77 / 1.60 | 1.52 / 1.45 | 1.57 / 1.50 | **1.50** / 1.47 | 1.18 / 1.09× |
| quantized | 1M | 14 / 7 | 2.76 / 2.48 | 2.65 / 2.29 | 2.41 / 2.50 | 2.99 / **2.21** | 0.92 / 1.12× |
| uniform | 8M | 14 / 7 | 18.17 / 25.55 | **17.46** / 24.35 | 18.64 / 24.84 | 17.73 / 25.44 | 1.02 / 1.00× (fast sample alone 1.04 / 1.05×) |
| near_sorted | 8M | 14 / 7 | 10.78 / 10.14 | 10.49 / 10.02 | **9.89** / 9.95 | 10.25 / 10.55 | 1.05 / 0.96× |
| quantized | 8M | 14 / 7 | 10.38 / 10.58 | **9.41** / 10.11 | 10.20 / 10.79 | 10.34 / 11.31 | 1.00 / 0.94× (fast sample alone 1.10 / 1.05×) |
| every shape | 1M and 8M | 1 | | | | | 0.98 to 1.06×: identical work, as the trace predicts |
| block_perm, zipf, heavy_tail, int_wide | 8M | | | | | | the box reloaded during these rows (heavy_tail 14T at 62 ms against 21 quiet); not read |


## 3n. Sample once, decide once (2026-09-11, `make sodobench`)

The parallel entry drew five samples and answered each gate with one
statistic. Now one sorted sample in two stages answers all of them: 2048
Weyl points for the gates (distinct count, Chao1 estimate of the true
cardinality from singletons and doubletons, NaN, an inversion rate over
~1900 Fibonacci-lag pairs), extended to 5376 with the KS certificate only
when the partition is reached, which then reuses it. The cheapest-first
order is unchanged because a cost model from the measured per-route costs
reproduces it; the change is in the gates. A first cut that drew all 5376
up front lost 5–16% on few_distinct and 10% on block_perm at 1M (the
cheapest routes paying for the partition's sample); the two-stage draw
removed that.

SHIPPED as the default (-DLOGOS_NO_SODO restores the per-gate samples): no loss on any shape once the two-stage draw was in, 1.07-1.39x on zipf and 1.07-1.22x on the 1M rows of the partition shapes, neutral at 8M; every run of the three was on a loaded box, ratios within rows only.

| shape | n | thr | per-gate samples | sample once | ratio |
|---|---|---|---|---|---|
| zipf | 8M | 14 / 7 | 27.4 / 31.1 | **23.2 / 27.1** | 1.18 / 1.15× (loaded run; 1.39× and 1.12–1.19× in two earlier runs) |
| zipf | 1M | 14 / 7 | 12.8 / 11.1 | **12.0 / 10.4** | 1.07× (1.47–1.52× in the first run) |
| near_sorted | 1M | 14 / 7 | 7.7 / 6.5 | **6.4 / 5.5** | 1.22 / 1.17× |
| jitter_ms | 1M | 14 / 7 | 3.3 / 2.9 | **2.7 / 2.6** | 1.20 / 1.11× |
| uniform | 1M | 14 / 7 | 3.5 / 4.5 | **3.2 / 3.8** | 1.08 / 1.17× |
| int_wide | 1M | 14 / 7 | 2.9 / 3.0 | **2.5 / 2.7** | 1.12 / 1.09× |
| heavy_tail, collatz, sawtooth, quantized, mod_card | 1M | | | | 1.03 to 1.31× at 14 threads |
| all shapes | 8M | | | | 0.92 to 1.18×, inside the loaded-run band |
| few_distinct | 8M | 14 | 7.35 / 6.42 min, 9.04 / 8.05 median (30 alternating reps) | 6.74 / 7.03 min, 8.73 / 7.93 median | noise; the 0.78–0.87 in the 5-rep tables was load |


## 3m. Histogram-free partition and the uniformity certificate (2026-09-11, `make hfbench`)

The parallel partition counted every element into a per-chunk histogram
before scattering it, so that each thread could write to exact offsets. That
pass was 20–34% of the parallel wall on the random shapes. The sample already
predicts the bucket sizes: each bucket's count is n times the spacing between
two consecutive sample quantiles, a Beta(32, m−31) variable with an 18%
standard deviation (the DKW bound on the CDF, 3.7% of n, is six bucket widths
and was the wrong statistic in the conversation that led here). So the
scatter now writes into regions of (n/K)·1.6 in a persistent buffer, threads
reserve 256-element blocks with one atomic per block through a per-thread
write-combining stage, overflow spills to a per-thread list merged in at the
leaf, and above n/16 spills the routine declines to the histogram path.

The certificate: a Kolmogorov-Smirnov distance between the sorted sample and
the line through its extremes, free since the sample is sorted. Below 2/√m
the partition buckets on an equi-width grid with one multiply and no tables.

Four engines interleaved, best of 5, loaded box (memcpy 4.9 ms), ms:

| shape | thr | histogram (old) | histogram-free + KS | histogram-free, tables only | slack 0.3 | shipped / old |
|---|---|---|---|---|---|---|
| uniform | 14 / 7 | 26.7 / 38.5 | **20.4 / 27.2** | 22.6 / 34.6 | 20.6 / 29.4 | 1.31 / 1.42× |
| int_wide | 14 / 7 | 24.8 / 35.8 | **17.5 / 25.4** | 22.3 / 29.5 | 19.0 / 24.0 | 1.41 / 1.41× |
| int_1e6 | 14 / 7 | 17.8 / 31.0 | **12.3 / 24.1** | 16.0 / 25.4 | 12.9 / 24.0 | 1.45 / 1.29× |
| jitter_ms | 14 / 7 | 20.4 / 35.6 | **16.5 / 23.2** | 18.0 / 30.5 | 16.8 / 27.2 | 1.24 / 1.53× |
| logistic_map | 14 / 7 | 26.2 / 36.3 | 21.8 / **32.4** | **21.6** / 34.5 | 23.1 / 35.0 | 1.20 / 1.12× |
| clustered | 14 / 7 | 24.2 / 32.0 | **21.2 / 29.0** | 23.2 / 30.1 | 23.7 / 31.0 | 1.14 / 1.10× |
| heavy_tail | 14 / 7 | 26.2 / 39.5 | 25.9 / **34.4** | 27.6 / 35.7 | 27.4 / 36.9 | 1.01 / 1.15× |
| bimodal | 14 / 7 | 28.2 / 35.3 | 27.3 / **32.5** | **24.5** / 33.5 | 26.9 / 36.7 | 1.03 / 1.09× |
| dup_1000, zipf, near_sorted, quantized | 14 / 7 | | | | | 0.94 to 1.07 (other routes or spills declined: identical code within noise) |

Both paths pass the 685-check harness. The prediction of 4–6 ms off a 19 ms
uniform sort came out as 6.3 ms off 26.7 on a loaded box. The certificate is
worth 10–20% by itself on the shapes it fires on (uniform, int_wide,
jitter_ms) and never fires on the others.

## 3l. The inherited JS-era constants, swept (2026-09-11, `make knobbench`)

Fourteen dispatcher thresholds inherited from the JS engine, never re-tuned
in C. Each swept over four values (the current one and three others), four
engines interleaved in one binary, 19 shapes, 1 thread and 14, best of 3,
n = 8M. The noise band of this bench is about ±10% at 14 threads and ±5% at
1, with a ~3–5% bias against the first column (it ran first in every rep;
the order now rotates).

| constant (JS-era value) | values swept | verdict | evidence |
|---|---|---|---|
| MIN_AVG_RUN_FOR_MERGE (256) | 64, 1024, 4096; then 16384, 65536, 262144 | **4096 shipped** | sawtooth_stride 1T 553 → 138 ms (its 5376 runs go to the AVX2 leaf); 262144 broke relocation on 64-block data (17 → 52 ms) because the same cap feeds run detection |
| MERGE_MAX_RUNS (new) | ∞, 32, 8, 3 | **8 shipped** | 64 overlapping runs 1T 283 → 116 ms; a 4-way merge still beats the leaf (89 vs 120), so 3 lost that row |
| LOWCARD_CAP (1024) | 512, 256, 128 | **256 shipped** | zipf 1T 47 → 33 ms, 14T 15 → 11: it passes the 64-sample gate yet has thousands of distinct values and aborted late; 256 aborts early. Data with 256–1024 distinct values ran 4.4× slower on this path than on the hash anyway (collatz at gate 1024) |
| COUNTING_SORT_K (1) | 2, 4, 8 | keep 1 | K=2 made int_span2n 1T 2.2× slower, K=8 int_span8n 4×: a count array above n is random access into 64+ MB; the JS reason was memory, the C reason is cache. Counting is cheap only when the increments are sequential (near-sorted integers) |
| LOWCARD_SAMPLE_MAX (64) | 16, 256, 1024 | keep 64 | 1024 accepts collatz and runs it 4.4× slower; 16 gains only what LOWCARD_CAP now gains |
| NEARLY_SORTED_INV_RATIO (0.05) | 0.02, 0.1, 0.2 | keep | 0.02 lost 23% on near_sorted_f64; 0.1 and 0.2 gained nothing |
| QUANT_PROBE_MAX (4) | 2, 5, 6 | keep 4 | 5 and 6 send integer data to the 8-pass float radix (near_sorted_int 1T 40 → 147 ms); 2 within noise of 4 |
| FLASH_SORT_THRESHOLD (4096) | 512, 16384, 65536 | keep | 65536 sends 33k buckets to the scalar introsort, 3× slower at 14T; 512 and 16384 noise |
| FLASH_SAFETY_RATIO (0.05) | 0.01, 0.2, 0.5 | keep | 0.5 sends buckets to coarse instead of flash, uniform 14T 22 → 30 ms; 0.01 and 0.2 no consistent gain |
| MOMENTUM_THRESHOLD (50000) | 5000, 500000, ∞ | keep | noise; ∞ burns the 8n budget on near_sorted_f64 (0.91×) |
| SKEW_PROBE_MAX (8) | 4, 16, 32 | keep | noise at every value |
| TYPED_MIN_N (65536) | 16384, 262144, 1M | keep | noise |
| INSERTION_SORT_THRESHOLD (24) | 16, 48, 96 | keep | within the bench's ±5% first-column bias; nothing separable |
| COARSE_SMALL (64) | 32, 128, 256 | keep | same |

Reading: the inherited constants were not a bounded-language compromise.
Eleven of fourteen are right or indistinguishable from right in C. The three
that were wrong are all gates on the k-way run merge and on a counting
attempt that aborted late, which are the two places where the JS cost model
(merges cheap relative to everything else, no cache hierarchy to speak of)
differs most from C's. The counting span factor is the clearest case of a
value that is right for the wrong reason: JS kept it at 1 for memory, C
keeps it at 1 because a count array larger than n is random access into
64 MB or more and loses 2 to 4× to sequential radix passes.

## 3k. Is one thread ever faster than the parallel call? Per-route thread caps (2026-09-11, `make smallbench`)

Yes, it was: in the scaling report's 100k–1M band on the structured shapes
(block_perm 316k 1.0 vs 1.4 ms, 1M 2.1 vs 2.5–3.0; the counting shapes at 1M
ran ten threads where four were 1.5–1.8× faster). Below 32k the bench's
0.1 ms resolution says nothing, and at 100k every column runs identical code.

The cause is that the routes have different parallel cost shapes. The
partition route wants every thread (three at 316k beat one by 1.3×).
Relocation's parallel form has a serial plan and a spawn and loses below
about 500k elements per thread. The counting routes merge P tables, a cost
proportional to P and not n, so four threads beat ten at 1M while three
still beat one at 316k. A global threshold cannot express that: a global
250k policy was measured and rejected, and a counting cap without a floor
cost quantized 22% at 316k. Shipped: relocation at most one thread per 500k;
counting routes at most one per 350k, never fewer than 3; global policy
unchanged. Neither cap binds at 8M.

Confirmation run, four engines interleaved, P-thread call, ms:

| shape | n | 1T | P-call before | P-call now | now / before |
|---|---|---|---|---|---|
| block_perm | 1M | 2.10 | 2.47 | **1.88** | 1.31× |
| block_perm | 3.16M | 6.82 | 6.34 | **3.76** | 1.69× |
| block_overlap | 1M | 2.24 | 2.43 | **2.01** | 1.21× |
| few_distinct | 1M | 3.10 | 2.03 | **1.21** | 1.68× |
| few_distinct | 3.16M | 9.75 | 3.65 | **3.09** | 1.18× |
| quantized | 1M | 3.93 | 2.47 | **1.93** | 1.28× |
| quantized | 3.16M | 13.19 | 4.63 | **4.01** | 1.15× |
| mod_card_30k | 1M | 4.99 | 3.83 | **2.77** | 1.38× |
| mod_card_30k | 3.16M | 14.08 | 6.32 | **4.98** | 1.27× |
| uniform, near_sorted, heavy_tail, int_wide | 316k to 8M | | | | 0.94 to 1.20× (caps not binding; noise band) |
| all shapes | 8M | | | | identical code, 0.93 to 1.15× (noise) |
| block shapes | 316k | 0.64 / 0.74 | 0.71 / 0.69 | 0.76 / 0.78 | the residual: ~0.1 ms of parallel-entry setup with 3 threads |

## 3j. The structured routes did not scale; the near-sorted route rebuilt (2026-09-11, `make nsbench`)

Each sort's own speedup from 1 to 14 threads, from the profile report's
cells (same sort, same shape):

| shape | logos | x86-simd-sort | IPS⁴o | logos's route at 14T |
|---|---|---|---|---|
| uniform, heavy_tail, bimodal, int_wide | 4.6 to 5.5× | 4.0 to 4.2× | 7.0 to 7.4× | partition (leaf balanced) |
| near_sorted | **1.8×** | 4.2× | 7.3× | chunk-sort then P-way merge |
| block_perm, block_overlap | **2.1 to 2.2×** | 4.0 to 4.4× | 6.8× | relocation, waits of 2 to 3.5 ms |
| quantized, mod_card_30k | 3.7 to 4.4× | 2.6 to 3.4× | 6.2 to 6.7× | lattice, busy 3.0 to 5.8 ms per thread |
| few_distinct | 9.1× | 1.1× | 5.7× | counting |

The single-thread wins on structured data come from doing less work. The
parallel versions of those routes did not keep that advantage. On
near_sorted the 14 threads together spent about 200 ms of thread time
against 36 ms for the one-thread algorithm, for two reasons found by the
per-phase trace: the chunk leaf was a different algorithm (chunking by index
gives every chunk the whole value range, so the dispatcher's ratio gates
picked the float radix or bits-coarse at 16–30 ns/elem instead of the
counting sort at 6), and the 14-way heap merge is comparison work with a
mispredicting branch per element.

The rebuilt route (band-first): thresholds are window medians at the chunk
boundaries (sample quantiles were ~n/448 positions off and turned 589k
elements into strays); one greedy pass per chunk keeps an element iff it is
in band, not below the last kept value and not above both next neighbours,
with poison recovery (a displaced-high element that slips in is popped after
16 consecutive "below the maximum" rejections; without it 88% of 8M was
rejected); the strays are sorted once and cut by the thresholds; each band
does one two-way merge. Along the way a pre-existing hazard surfaced: a
nearly-sorted 48k bucket sent to plain insertion sort is quadratic in
displacement (333 ms); the momentum insertion with its 8n budget now takes
that case.

Four engines interleaved, best of 5, ms (loaded run, memcpy 4.6 ms):

| shape | thr | old route | new route | shipped before | new / shipped |
|---|---|---|---|---|---|
| near_sorted_int (1%) | 14 | 21.8 | **10.3** | 23.5 | 2.27× |
| near_sorted_int (1%) | 7 | 28.2 | **9.9** | 28.2 | 2.85× |
| near_sorted (chaos, f64) | 14 | 21.7 | **14.2** (9.6 on a quieter run) | 21.6 | 1.52 to 2.05× |
| near_sorted (chaos, f64) | 7 | 25.6 | **18.2** | 25.9 | 1.42× |
| near_sorted_f64 (0.1%) | 14 / 7 | 24.1 / 33.1 | 23.8 / 24.9 | 24.5 / 30.2 | 1.03 / 1.21× |
| near_sorted_int (5%) | 14 / 7 | 13.8 / 29.8 | 13.8 / 31.6 | 14.1 / 29.5 | 1.02 / 0.93× |
| local_shuffle_1000 | 14 / 7 | 26.5 / 34.8 | **17.4 / 23.6** | 25.7 / 35.5 | 1.48 / 1.50× |
| brownian, uniform, sawtooth_stride, block_perm | 14 / 7 | | | | 0.98 to 1.07× |

Open on the same finding: lattice, counting and relocation still carve the
array into static per-thread ranges (busy 3.0–5.8 ms per thread on lattice,
2.0–4.5 on relocation) and relocation has a serial middle the threads wait
on (2–3.5 ms). Those are the next two.

## 3i. Lattice window minimum size (2026-09-11, `make latbench`)

The moderate-cardinality path tries a direct-index lattice counter before
its hash counter. Its window was the 2048-sample's range plus an eighth, and
the map pass aborted on the first value outside it, discarding everything
scanned so far. Collatz stopping times (0..664) have a tail the sample never
reaches (sample max 413–444); the abort came 19–44% into the array and cost
5–12 ms at 1 thread. Tables under 4096 slots are now widened to 4096.

| shape | 1T old | 1T new | ratio | 14T old | 14T new | ratio |
|---|---|---|---|---|---|---|
| collatz | 42.9 | **32.8** | 1.31× | 10.1 | **7.2** | 1.40× |
| quantized | 36.6 | 37.0 | 0.99 | 6.8 | 7.4 | 0.92 (identical code; band 6.8–8.6 today) |
| few_distinct | 29.8 | 30.6 | 0.98 | 7.0 | 6.5 | 1.09 |
| mod_card_30k | 36.0 | 37.1 | 0.97 | 12.6 | 13.0 | 0.97 |
| zipf, prime_gaps, jitter_ms, uniform, heavy_tail, bimodal, near_sorted | | | 1.00–1.08 | | | 0.97–1.09 |

A proportional margin (a full span each side) was measured first: collatz
1.23× / 1.55× but quantized 0.87× at 14T, because its 4096-slot table
tripled and each thread zeroes and scans it. Rejected; the minimum-size rule
keeps large tables as they were.

## 3g. AVX2 leaf and model splitter table (2026-09-11, `make simdbench`, `make stbench`)

Two structural imports from other parallel sorts, both built, both A/B'd with
four (three) engines interleaved in one binary on identical data, best of 5.

**AVX2 vectorized leaf** (x86-simd-sort design in house: permute-compress
partition with plain stores, 8-vector end-choice, streaming small partition,
generated 32/64 bitonic networks). Shipped policy: sequential sorts only, not
when nearly sorted. Final run (calibration memcpy 3.3 ms):

| shape | 1T scalar | 1T AVX2 (shipped) | ratio | 14T scalar | 14T AVX2 also in parallel leaf | ratio |
|---|---|---|---|---|---|---|
| uniform | 123.3 | 102.6 | **1.20×** | 19.2 | 22.9 | 0.84× |
| heavy_tail | 147.8 | 100.0 | **1.48×** | 20.4 | 23.0 | 0.89× |
| bimodal | 162.1 | 101.9 | **1.59×** | 22.8 | 26.9 | 0.85× |
| int_wide | 92.7 | 88.1 | 1.05× (radix path, noise) | 17.3 | 18.6 | 0.93× |
| clustered | 121.8 | 102.6 | **1.19×** | 18.7 | 21.4 | 0.87× |
| negatives | 126.8 | 100.8 | **1.26×** | 18.6 | 22.3 | 0.83× |
| near_sorted | 81.3 | 83.3 | 0.98× (gated to coarse) | 21.2 | 24.3 | 0.87× |
| quantized | 32.8 | 33.8 | 0.97× (identical code) | 8.1 | 8.6 | 0.94× |
| few_distinct | 27.3 | 26.5 | 1.03× (identical) | 7.0 | 7.0 | 1.00× |
| block_perm | 17.8 | 17.4 | 1.02× (identical) | 9.2 | 9.2 | 1.00× |

The shipped engine's 14T column is identical code to scalar (all within
noise, not shown). The parallel-leaf variant lost 5–17% here and 10–16% at 6
threads: an L2-resident 33k bucket already sorts at ~9 ns/elem with
flash/coarse, the vectorized quicksort is 8.5 at 1 thread and slower under
14 on this hybrid CPU. Before the nearly-sorted gate, near_sorted 1T was
99 vs 81 ms: the top hook stepped aside but coarse's bucket hook fired on the
4k-element buckets, where coarse's near-sequential scatter still wins.

Engineering path (component probe, 8M uniform): first cut 268 ms (2.3× slower
than scalar), one partition pass 1.41 ns/elem, network loops not unrolled.
Unroll 8 + plain stores: 0.39 ns/elem. Straight-line networks: 1.9 ns/elem.
Streaming partition under 512: 0.71 vs 1.23 ns/elem on 256-element ranges.
128 network: 3.0 ns/elem, rejected. Result 92–104 ms vs x86-simd-sort 79.

**Model splitter table** (linear bucket-index model per value cell, third
trial candidate). Two 14-thread runs, model vs value+key tables:

| shape | run 1 | run 2 |
|---|---|---|
| uniform | 1.09× | 0.93× |
| heavy_tail | 0.88× (ungated) | 1.01× (merr ≤ 1 gate) |
| bimodal | 1.02× | 0.95× |
| int_wide | 1.09× | 1.01× |
| clustered | 1.04× | 1.00× |
| negatives | 1.06× | 0.99× |
| 7-thread rows | 0.98–1.05× | 0.96–1.03× |

Noise. Opt-in `-DLOGOS_MODEL_TABLE`, recorded in the ledger. The prediction
(bimodal histogram 7.3 → 5.5 ms) was wrong: bimodal's residual histogram
cost is not the child-table dereference.

## 3f. The package: 12 buckets/thread, branch-free leaf, interleaved counters, line repair (`make theorybench`)

Six interleaved variants, 8M, best of 5, loaded box (identical-code guard rows
spread ±12% at 30 ms, ±25% at 15 ms). Package vs previous defaults:

| shape | 20T | 10T |
|---|---|---|
| uniform | 1.28× | 1.15× |
| heavy_tail | 1.24× | 1.24× |
| bimodal | 1.47× | 1.24× |
| int_wide | 0.98× | 1.08× |
| int_1e6 | 1.16× | 1.42× |
| clustered | 1.02× | 1.28× |
| never-partition routes | 0.98–1.17× | 1.04–1.11× |

Bucket count alone gives most of it (a 33k-element leaf stays in L2). The three
smaller parts are unattributed here; each has a `-DLOGOS_NO_*` flag. Int-leaf
digit narrowing was mixed and is opt-in only.

## 3e. Direct-index splitter tables shipped (H0b, 2026-09-10, `make stbench`)

The histogram's bucket lookup is now an O(1) table; each thread trials a
value-space and a key-space table on its first two chunks and commits to the
faster. Interleaved A/B, 8M, best of 5, loaded box (calibration ~2× off quiet):

| shape | 20T | 10T |
|---|---|---|
| uniform | 1.98× | 1.72× |
| heavy_tail | 2.03× | 2.63× |
| bimodal | 1.58× | 1.21× |
| int_wide | 2.99× | 1.35× |
| clustered | 1.89× | 2.12× |
| negatives | 2.13× | 1.97× |
| shapes that never partition | noise | noise |

Predicted 1.4–1.9×; measured higher. 685 checks pass. The x86-simd-sort rows
in §1 predate this change and must be re-measured before being quoted.

## 4. Paradigm experiments (experiments/paradigm, vs x86-simd-sort)

| paradigm | result |
|---|---|
| #1 vectorize the addressing (vpscatter, vpconflictd) | **fails** (1.05×, 0.65×) |
| #2 order model, no materialization (rank/select) | wins 1.57× at 64 distinct; loses at ≥4096 |
| #3 analytics from a one-pass profile | quantiles **87×**, top-k **7×** (exact), range-count **42×** (exact) |

Conclusion: SIMD wins the race to a sorted array; logos's bet pays only when the
deliverable is order/structure, not the array.

## 5. Restructurings tried against the SIMD gap — all measured negative

- Parallel merge sort: merge is 15–26× costlier than the scatter (a merge is
  comparison; the top scatter is already ~bandwidth). experiments/paradigm/MERGE_EXPERIMENT.md
- Cache-conscious COARSE_K: +11% in isolation, 0.58–0.90× integrated (per-level
  overhead). experiments/paradigm/SCATTER_EXPERIMENT.md
- Full negative ledger: CLAUDE.md.

## 6. What "functionally useful" honestly means here

Numeric-only, in-memory, small niche as a sort. The durable value is (a) the
structured-data wins vs SIMD, (b) argSort making it a records-by-key sort,
(c) the order/analytics paradigm, and (d) the reproducible methodology and
documented negatives.
