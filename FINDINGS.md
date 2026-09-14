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
