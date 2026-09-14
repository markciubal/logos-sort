# pagetable-reloc — relocation by page table (OUTSIDE_THE_BOX D1)

Agent: pagetable-reloc, overnight 2026-09-12. Work copy `/tmp/logos-team/pagetable-reloc/work`,
patch at `experiments/team/patches/pagetable-reloc.patch`.

## What was tried

The J5 relocation route (`relocate_try_f64`) recognises block-permuted data and memcpy's each sorted
run into its chain-ordered place in a scratch buffer, then memcpy's the whole thing back. D1 asks:
if a run is page-aligned, why copy the bytes at all — `mremap(MREMAP_FIXED)` moves the page-table
entries instead. Step 1 (mandatory, and possibly the whole job): instrument the route to report the
run-length distribution and how many bytes could actually move by page-table edit, on the bench
shapes and on block sizes of my choosing. Step 2 only if step 1 finds aligned multi-megabyte runs.

## Prediction (written before any measurement)

The arithmetic first, because it decides everything. Run q sits at source byte offset
`s = 8*start` and must land at destination byte offset `t = 8*off`. `mremap` moves whole pages, so
a page of the run can move only if `s ≡ t (mod 4096)` — the run must have the *same page phase* at
both ends. Under a block permutation of nb blocks of blk elements, source block k goes to value
block b = perm[k], so `s - t = (k - b) * 8 * blk`. That is a multiple of 4096 for every (k, b) iff
`8*blk` is a multiple of 4096, i.e. blk is a multiple of 512 elements; otherwise only when
`k == b` (a fixed point of the permutation, which needs no move at all).

- **Bench shapes** (`block_perm`, `block_overlap`; 64 blocks; n = 8,000,000 and 30,000,000):
  blk = 125,000 and 468,750 elements, so 8*blk = 1,000,000 and 3,750,000 bytes, neither a
  multiple of 4096 (residues 576 and 2160). Only fixed points align. A random permutation of 64 has
  1 fixed point in expectation, so I predict **~1/64 ≈ 1.6% of bytes 4 KB-movable and the same
  ~1.6% (minus edges) 2 MB-movable, varying with the seed from 0% to a few %**. The overlap shape
  is identical in this respect. Verdict on the bench shapes: **dead; nothing to build**.
- **Block sizes I choose** so that 8*blk is a page multiple: blk = 4096 (32 KB), 65,536 (512 KB),
  131,072 (1 MB), 524,288 (4 MB), 2,097,152 (16 MB) elements. I predict **~100% of bytes phase-
  aligned at 4 KB**, minus one partial page per run end, because malloc's mmap'd chunks start 16
  bytes into a page (the chunk header), so the array base itself is not page-aligned:
  4 KB-movable ≈ (8*blk - 4096) / (8*blk) → 87.5% at 32 KB, 99.2% at 512 KB, 99.6% at 1 MB.
  **2 MB-movable: 0% for blk < 262,144 elements** (relative phase mod 2 MB differs unless 8*blk is a
  2 MB multiple), then ≈ (8*blk - 2 MB) / (8*blk): **50% at 4 MB runs, 87.5% at 16 MB runs.**
- **Mechanism cost, before integrating anything.** mremap of a 1 MB anonymous range moves 256 PTEs
  under the mmap write lock and then shoots down the TLB on every CPU running one of this
  process's threads. With 14 spinning workers on WSL2 (Hyper-V enlightened flushes) I predict
  **15–40 µs per mremap**, against ~50–100 µs to memcpy 1 MB at one thread's bandwidth. So the
  break-even run length is around 256 KB–1 MB, not the 64 KB D1 guessed, and at 8M with 64 blocks
  the whole route needs 2 mremaps per run (out to scratch, back into the caller's array, because
  source and destination page ranges do not coincide) ≈ 128 syscalls ≈ 2–5 ms, about the same as
  the two 64 MB parallel copies it replaces. **Predicted end-to-end: neutral at 8M / 1 MB runs
  (0.9–1.2×), 1.5–2.5× at 30M-class inputs with 4 MB+ runs, IF the box's mremap cost is at the low
  end; 0.5–0.8× if the shootdown lands at the high end.** Bench shapes: exactly 1.00× because the
  page path must not fire there.

## What was measured

### Step 1: the census (`make pagebench && ./pagebench`, engine built with -DLOGOS_TRACE, 14 threads, 2 seeds)

`reloc_align_census` runs inside `relocate_try_f64` once the run descriptors and their destination
offsets exist. "movable" = bytes in whole pages strictly inside a run whose source and destination
phases agree at that granularity; "dst at a's phase" places the scratch at the caller array's page
phase (what an implementation does), "actual ptrs" uses the pool buffer as it is. Every array here is
malloc'd, and every one sits 16 bytes into a page (`a%4K=16`), as predicted.

| shape | runs | run bytes | phase-matched 4K | movable 4K | movable 2M | fixed-point runs |
|---|---|---|---|---|---|---|
| block_perm 8M/64 (bench), seed 0 | 63 | 1.0–2.0 MB | 1.6% | 1.6% | 0.0% | 1 (1.6%) |
| block_perm 8M/64 (bench), seed 1 | 62 | 1.0–2.0 MB | 0.0% | 0.0% | 0.0% | 0 |
| block_overlap 8M/64 (bench), seed 0 | 63 | 1.0–2.0 MB | 1.6% | 1.6% | 0.0% | 1 |
| block_perm 30M/64 (bench), seed 0 | 63 | 3.75–7.5 MB | 1.6% | 1.6% | 0.0% | 1 |
| block_perm 30M/64 (bench), seed 1 | 62 | 3.75–7.5 MB | 0.0% | 0.0% | 0.0% | 0 |
| block_overlap 30M/64 (bench), seed 0 | 63 | 3.75–7.5 MB | 1.6% | 1.6% | 0.0% | 1 |
| block_perm 8M/blk 65536 (512 KB) | 121–123 | 520 KB | 0.0–0.8% | 0.0–0.8% | 0.0% | 0–1 |
| block_perm 2^23/64 (1 MB) | 62–63 | 1–2 MB | 100% | 99.6% | 0.0% | 0–1 |
| block_overlap 2^23/64 (1 MB), seed 0 | 63 | 1–2 MB | 100% | 99.6% | 0.0% | 1 |
| block_perm 2^23/16 (4 MB) | 16 | 4 MB | 100% | 99.9% | 50.0% | 0–1 |
| block_perm 2^25/64 (4 MB) | 62–63 | 4–8 MB | 100% | 99.9% | 50.8–51.6% | 0–1 |
| block_overlap 2^25/64 (4 MB), seed 0 | 63 | 4–8 MB | 100% | 99.9% | 50.8% | 1 |
| block_perm 2^25/16 (16 MB) | 16 | 16 MB | 100% | 100% | 87.5% | 0–1 |

Rows missing above never reached the route: block_overlap seed 1 at 8M and 30M and block_overlap
2^23/64 and 2^25/64 seed 1 were declined by the parallel run detector's chunk gate
(`merged > seen/2` inside one chunk when the shuffle put two overlapping value-blocks next to each
other), and block_perm 8M/blk 4096 (32 KB runs, 1954 of them) never passed the detector's run cap.
The "8M/blk 65536" row is the deliberate control: 65,536 elements is a page multiple but
8,000,000/65,536 is not an integer, so `blk = 65,041` and the phases do not match — 0.8%. The
prediction (fixed points only on the bench shapes; ~100% at 4 KB and 0/50/87.5% at 2 MB for
1/4/16 MB page-multiple runs) was right on every row. No 2 MB-movable byte exists on any bench shape.

### The mechanism, measured before building anything (`./mmprobe 14`, 64 MB regions, min of 5 reps, µs per run)

`memcpy2` = one thread copies the run out and back (what the route does per byte), `pmemcpy2` = the
same two copies across 14 threads (what the parallel route does), `mremap2` = the run's mapping
moved out and back (two syscalls, zero bytes). `spin=13` keeps 13 threads busy on other CPUs so each
mremap pays the real TLB shootdown, which is the engine's situation. `hugepage=1` aligns the runs to
2 MB (and madvises THP), so a run that is a 2 MB multiple moves by PMD instead of by PTE.

| hugepage | spin | run | memcpy1 | memcpy2 | pmemcpy2 | mremap2 | mremap2 / pmemcpy2 |
|---|---|---|---|---|---|---|---|
| 0 | 0 | 64 KB | 22 | 26 | 24 | 123 | 5.11× (slower) |
| 0 | 0 | 256 KB | 128 | 161 | 105 | 150 | 1.43× |
| 0 | 0 | 1 MB | 335 | 685 | 580 | 463 | 0.80× |
| 0 | 0 | 4 MB | 1691 | 3640 | 1958 | 1326 | 0.68× |
| 0 | 0 | 16 MB | 4665 | 8055 | 5726 | 3338 | 0.58× |
| 0 | 13 | 64 KB | 33 | 39 | 46 | 248 | 5.36× |
| 0 | 13 | 256 KB | 115 | 137 | 170 | 161 | 0.95× |
| 0 | 13 | 1 MB | 421 | 717 | 594 | 1105 | 1.86× |
| 0 | 13 | 4 MB | 1309 | 2666 | 3107 | 4430 | 1.43× |
| 0 | 13 | 16 MB | 5490 | 12339 | 12769 | 6274 | 0.49× |
| 1 | 0 | 1 MB | 426 | 794 | 442 | 198 | 0.45× |
| 1 | 0 | 4 MB | 2141 | 4550 | 1865 | 114 | **0.06×** |
| 1 | 0 | 16 MB | 5438 | 11610 | 8292 | 236 | **0.03×** |
| 1 | 13 | 256 KB | 272 | 445 | 197 | 218 | 1.11× |
| 1 | 13 | 1 MB | 301 | 539 | 604 | 623 | 1.03× |
| 1 | 13 | 4 MB | 1148 | 2266 | 3362 | 195 | **0.06×** |
| 1 | 13 | 16 MB | 3686 | 25555 | 16226 | 410 | **0.03×** |

Two things decide the design. (1) A PTE-level move (4 KB alignment, hugepage=0) is not cheaper than
the parallel copy until the run is about 16 MB: with the workers alive a 1 MB run costs 1.1 ms to
move and 0.6 ms to copy twice. The 99.6% "4 KB-movable" bytes of the 1 MB-block shapes are
therefore worth nothing. (2) A PMD-level move (2 MB-aligned unit, hugepage=1) costs ~100–200 µs
per run regardless of size, 16–30× less than the copies at 4 MB and up. So the only thing worth
building is a path that moves whole 2 MB units, which the census says exist only on 2 MB-multiple
block sizes with runs of 4 MB or more, and never on the bench shapes.

### Step 2: the prototype (`-DLOGOS_PAGE_RELOC`, runtime toggle `LOGOS_PAGE_RELOC=1`; `make pagerbench`)

Built because the census did find aligned multi-megabyte runs, just not on the bench shapes. It
lives inside `relocate_try_f64` and changes nothing unless both the compile flag and the
environment variable are set, so one binary carries both variants (no code-layout bias, rule 3).

- **Phase 1 (caller thread, before the workers spawn).** For every run whose source and destination
  byte offsets agree modulo 2 MB, the 2 MB-aligned units strictly inside it are `mremap`'d
  (`MREMAP_MAYMOVE|MREMAP_FIXED`) into a scratch mapping placed at `a[]`'s 2 MB phase. Fires only
  when every chain is a single run (multi-run chains merge with `a[]` as scratch, which would then
  have holes) and at least 25% of the bytes move.
- **Workers.** memcpy the ragged ends and the non-moving runs around each run's moved window.
- **Phase 3 (caller thread, after the join).** The scratch's unit-aligned middle goes back into `a[]`
  piece by piece in destination order (each moved span and each memcpy'd gap is one VMA, and
  `mremap` needs its old range inside one VMA); the sub-unit head and tail by memcpy. About
  2R+1 syscalls in all.
- **Every vacated range is re-reserved at once** with a page-free `PROT_NONE` `MAP_FIXED` mapping.
  The first cut did not do this and crashed on the 2 MB-aligned-array rows, always on the *second*
  call and only at 14 threads: with 100% of `a[]` moved out, `a[]` was a 64 MB hole while the
  workers were spawned, glibc mmap'd their thread stacks into it, phase 3 mapped our data over the
  now-cached stacks, and the next `pthread_create` reused a stack that was our sorted data (the
  backtrace ended in `pthread_create`, si_addr nil). A hole in the caller's array is a hazard for
  every mmap in the process; reserving it costs one syscall per span and removes the hazard.
- Harness: a `block_4MB` shape (fixed 524,288-element blocks, ragged last block) across all sizes
  and threads, plus a dedicated loop at 2^22 elements with 4 MB and 6 MB blocks, 1 and 8 threads,
  toggle off and on: 879 checks, up from 835. Under `-DLOGOS_TRACE` the route prints how many
  bytes it moved and in how many syscalls; on the harness's shapes it moved 50–69% of the array in
  4–8 `mremap`s out and 11–23 in all.

### Step 2: the paired A/B (`flock /tmp/logos-bench.lock ./pagerbench 24`; 24 pairs, threads 14 and 1)

One binary, `bench_paired` with the toggle alternating inside each pair; ratio = t(memcpy) / t(page
table), so > 1 favours the page path. "fired" counts the page-path calls inside the pairs that
actually moved pages (a bug in the first cut declined every call after the first on some rows; the
per-unit fallback fixed it, and this table is the run after the fix). The three null rows are the
calibration: they should read 1.000 and read 0.94–1.02 with p10/p90 of about ±30%, so this box,
today, with six other agents building, resolves roughly ±10% at the median on this route.

| shape | thr | moved by page table | mremaps/call | ratio | [p10, p90] | fired |
|---|---|---|---|---|---|---|
| block_perm 8M/64 (bench) | 14 | 0% | 0 | 0.940 | [0.650, 1.289] | 0/24 (null) |
| block_perm 8M/64 (bench) | 1 | 0% | 0 | 0.980 | [0.680, 1.370] | 0/24 (null) |
| block_overlap 8M/64 (bench) | 14 | 0% | 0 | 1.015 | [0.858, 1.228] | 0/24 (null) |
| block_overlap 8M/64 (bench) | 1 | 0% | 0 | 0.958 | [0.660, 1.228] | 0/24 (null) |
| block_perm 2^23/64 (1 MB runs) | 14 | 0% | 0 | 0.969 | [0.804, 1.271] | 0/24 (null) |
| block_perm 2^23/64 (1 MB runs) | 1 | 0% | 0 | 0.961 | [0.804, 1.118] | 0/24 (null) |
| block_perm 2^23/16 (4 MB runs), malloc'd | 14 | 50.0% | 47 | **0.770** | [0.571, 1.130] | 24/24 |
| block_perm 2^23/16 (4 MB runs), malloc'd | 1 | 50.0% | 47 | 1.054 | [0.701, 1.679] | 24/24 |
| block_perm 2^25/64 (4 MB runs), malloc'd | 14 | 50.0% | 191 | **0.761** | [0.266, 1.489] | 24/24 |
| block_perm 2^25/64 (4 MB runs), malloc'd | 1 | 50.0% | 191 | **0.629** | [0.221, 1.900] | 24/24 |
| block_perm 2^25/16 (16 MB runs), malloc'd | 14 | 88.3% | 44 | **0.665** | [0.251, 1.744] | 24/24 |
| block_perm 2^25/16 (16 MB runs), malloc'd | 1 | 88.3% | 44 | 1.306 | [0.360, 2.968] | 24/24 |
| block_perm 2^23/16 (4 MB runs), 2 MB-aligned array | 14 | 100% | 34 | **1.288** | [0.924, 1.639] | 24/24 |
| block_perm 2^23/16 (4 MB runs), 2 MB-aligned array | 1 | 100% | 34 | **2.049** | [1.034, 2.710] | 24/24 |
| block_perm 2^25/64 (4 MB runs), 2 MB-aligned array | 14 | 100% | 128 | **1.824** | [1.230, 2.601] | 24/24 |
| block_perm 2^25/64 (4 MB runs), 2 MB-aligned array | 1 | 100% | 128 | **2.035** | [1.624, 2.905] | 24/24 |
| block_perm 2^25/16 (16 MB runs), 2 MB-aligned array | 14 | 100% | 52 | **1.623** | [1.064, 2.467] | 24/24 |
| block_perm 2^25/16 (16 MB runs), 2 MB-aligned array | 1 | 100% | 50 | **2.061** | [1.553, 4.225] | 24/24 |

An earlier run of the same table (before the per-unit fallback, so several "fired" rows were
partly memcpy pairs) read 0.40–1.46 on the malloc'd rows and 0.97–1.81 on the aligned ones; it is
superseded by this one and not used.

Reading it: on a **malloc'd array the page path loses or ties** even where half or 88% of the bytes
move (0.63–0.77 on four of six rows, the other two inside the noise), because every 2 MB unit
still costs about 2 syscalls with a TLB shootdown each (47–191 per call), the memcpy'd half is
faulted into a fresh scratch on every call, and the ragged ends split every run into three pieces.
On a **2 MB-aligned array, where every byte moves, it wins 1.3–1.8× at 14 threads and 2.0× at
1 thread, with p10 above 1.0 on five of six rows** — a real effect above today's floor, and
the only configuration in which D1's mechanism pays. Its cost then sits entirely in run detection.

## Verdict

**Negative for the target as posed, kept as an opt-in prototype.** On the bench shapes
(block_perm, block_overlap at 8M and 30M) not one byte can move by page table: the block size in
bytes is not a page multiple, so only fixed points of the permutation align (0–1.6%), and those
need no move at all. The census settled that for the cost of a printf, as D1 said it might. The
prototype built for the case where aligned runs do exist is correct (879 checks, both toggles),
loses on ordinary malloc'd arrays (0.63–0.77 at 14 threads), and wins 1.3–2.0× only when the
caller hands over a 2 MB-aligned array whose sorted blocks are 2 MB multiples. Per rule 8 it does
not ship: it stays behind `-DLOGOS_PAGE_RELOC` plus the `LOGOS_PAGE_RELOC=1` toggle, with the
measurements in the source comment. No version bump. `logos_psort.c` is touched (the census
under `LOGOS_TRACE`, the opt-in path, two fields on `reloc_arg_t`), the default build's code path
is unchanged.

## Prediction, graded

- "Bench shapes: ~1/64 of bytes 4 KB-movable, fixed points only, 0% at 2 MB": **right**, to the
  row (1.6% with one fixed point, 0.0% with none; 0.0% at 2 MB everywhere).
- "Page-multiple block sizes: ~100% at 4 KB; 0 / 50 / 87.5% at 2 MB for 1 / 4 / 16 MB runs":
  **right** (99.6–100%; 0.0 / 50.0–51.6 / 87.5%).
- "mremap 15–40 µs, break-even 256 KB–1 MB per run": **wrong in the interesting direction.** A
  PTE-level move with workers alive cost 100–1100 µs per run and does not break even below
  ~16 MB; a PMD-level move costs 100–200 µs regardless of size. D1's "nothing under 64 KB" was
  optimistic by two orders of magnitude for 4 KB alignment and pessimistic for 2 MB alignment.
- "End-to-end: neutral at 8M/1 MB runs, 1.5–2.5× at 30M-class with 4 MB+ runs if mremap is
  cheap, 0.5–0.8× if not; bench shapes exactly 1.00": **half right.** 1 MB runs never fire (no
  2 MB unit inside), the bench shapes read 0.94–1.02 (null), malloc'd 4 MB+ runs landed in the
  pessimistic branch (0.63–0.77 at 14T), and the 1.5–2.5× materialised only on 2 MB-aligned
  arrays (1.3–2.1×). The reason the prediction missed: it did not account for the caller's array
  starting 16 bytes into a page, which halves the movable bytes of a 4 MB run and turns every run
  into three pieces.

## Why, mechanism

1. **Alignment is a property of the block size in bytes, not of the data.** A block permutation
   moves run k to slot perm[k]; the displacement is `(k - perm[k]) * 8 * blk` bytes, and a page can
   move only if that displacement is a page multiple. So either `8*blk` is a multiple of the page
   (then every run qualifies) or nothing but fixed points does. The bench generator divides n by
   64 (125,000 and 468,750 elements), which is neither a 4 KB nor a 2 MB multiple, and real
   inputs of that family (merged log partitions, time-series shards) have no reason to be page
   multiples either. That is why D1's premise never holds on the shapes that matter.
2. **Even a phase-matched 4 KB run is not worth moving.** A PTE-level `mremap` walks and copies
   256 entries per MB, takes the mmap write lock, and shoots down the TLB on every CPU running a
   thread of the process; with the workers alive it measured 1.1 ms for a 1 MB run against 0.6 ms
   for copying it out and back on 14 threads. Below about 16 MB per run the copy wins. The
   "99.6% 4 KB-movable" rows of the census are therefore dead too.
3. **A PMD-level move is nearly free, and the caller controls whether it exists.** With both ends
   2 MB aligned the kernel moves one page-table page per 2 MB: 100–200 µs per run at any size.
   A malloc'd array starts 16 bytes into a page, so a 4 MB run holds one whole unit and a 16 MB run
   seven; a caller who allocates with 2 MB alignment (`posix_memalign`, or `LOGOS_HUGEPAGE`'s
   `big_alloc`) makes every 2 MB-multiple run move whole. In the prototype the relocation phases
   then take ~2–3 ms for 64 MB against 30–50 ms of run detection in front of them — the route's
   cost moves entirely into the detector, which is a read pass.
4. **The design has a hidden hazard that the copy path does not.** While pages are away the
   address range is a hole, and the kernel places any new mapping — thread stacks first of all —
   in the largest free gap. The prototype had to reserve every vacated range (`PROT_NONE`,
   `MAP_FIXED`) to be safe; that is one syscall per span and it is not optional.

## What I would do next

- Nothing on the bench shapes: there is no aligned byte to move, and no change to the engine can
  create one. D1 is closed for the block_perm / block_overlap family as generated.
- If the API ever grows a "caller promises 2 MB-aligned, THP-backed arrays" mode (the
  `LOGOS_HUGEPAGE` allocator is half of that), the page path becomes the natural relocation for
  block sizes that are 2 MB multiples, and the remaining cost is run detection. The lever there is
  the detector, not the move.
- Two refinements if that day comes: keep the scratch persistent and *swap* frames (three
  `mremap`s per piece through a temporary range) so the memcpy'd fraction is not re-faulted on
  every call, and skip the round trip through the scratch when all runs have equal length (then
  the block permutation is a permutation of 2 MB units and can be cycle-followed inside `a[]`
  with one temporary unit).

## Anything surprising

- The census took one `fprintf` and settled the question exactly as D1 predicted it might, but
  it also showed that the *pool buffer* is at a different 2 MB phase from the caller's array on
  every run (`dst%2M` ≠ `a%2M`), so even a hypothetical aligned input would move nothing through
  the existing scratch: the scratch must be placed at the array's phase, which the prototype does.
- The crash. A sort routine that unmaps part of its input, even briefly, competes with glibc for
  address space. It showed up only at 14 threads, only on the second call, only when 100% of the
  array moved — a textbook "works on my shape" bug that the aligned-array rows of the bench
  found because they were included on purpose as the best case.
- The parallel detector declines block_overlap about half the time depending on the shuffle
  (whenever two overlapping value-blocks happen to be adjacent inside one thread's chunk, its
  `merged > seen/2` gate fires at R = 2). Not this experiment's problem, but a 50/50 route choice
  on a bench shape is worth a look by whoever owns the relocation route.

## Exact commands (work copy at /tmp/logos-team/pagetable-reloc/work/engines/c, WSL)

```sh
make test                                                           # 879 checks, page path compiled out
make test CFLAGS="-O3 -std=gnu11 -pthread -Wall -Wextra -fPIC -DLOGOS_PAGE_RELOC"   # 879 checks, harness toggles the path on and off
make pagebench  && flock /tmp/logos-bench.lock ./pagebench          # step 1: the census (engine with -DLOGOS_TRACE), stderr
make mmprobe    && flock /tmp/logos-bench.lock ./mmprobe 14         # the mechanism: mremap vs memcpy per run
make pagerbench && flock /tmp/logos-bench.lock ./pagerbench 24      # step 2: paired A/B, one binary, runtime toggle
```
`pagerbench [pairs] [rowmask]` narrows to one row; `LOGOS_THREADS=N` overrides the parallel count.
Under `-DLOGOS_TRACE` every relocation prints the census and, with `LOGOS_PAGE_RELOC=1`, how many
bytes moved by page table and in how many syscalls.
