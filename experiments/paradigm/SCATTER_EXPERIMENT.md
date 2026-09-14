# "Scatter into the thread pool" / cache-conscious scatter — measured

The idea: replace logos's fine scatter (K=2048 buckets, whose 128 KB of write-heads
spill cache and hit DRAM randomly) with a *hierarchical* scatter — a cache-resident
first level, then subdivide. "Private per-thread buckets" is the parallel instance
(each thread's buckets = a cache-resident first level, writes stay on-core).

## In isolation: it works

A minimal two-level scatter vs a direct 2048-way scatter, single-thread, 8M:

| | direct 2048 | hierarchical | ratio |
|---|---|---|---|
| 32×64 | 71.7 ms | 66.1 ms | **1.08×** |
| 64×32 | 71.7 ms | 64.7 ms | **1.11×** |
| 128×16 | 71.7 ms | 85.5 ms | 0.84× (heads spill L1) |

Real, ~10%, when the first level fits cache. So the *principle* is sound.

## Integrated into logos: it loses

Sweeping `COARSE_K` (the per-level bucket count) end-to-end, single-thread, 8M:

| shape | K=2048 (now) | 512 | 256 | 128 | 64 |
|---|---|---|---|---|---|
| uniform | 161 ms (1.00×) | 0.75× | 0.76× | 0.79× | 0.58× |
| bimodal | 211 ms (1.00×) | 0.78× | 0.65× | 0.85× | 0.72× |
| heavy_tail | 197 ms (1.00×) | 0.82× | 0.90× | 0.70× | 0.71× |

**Every cache-resident K is slower.** 2048 is optimal. Why: logos already scatters
*recursively*, and each level carries fixed overhead — ping-pong `land[]` tracking,
prefix sums, the balance check, recursion frames. Making levels smaller (to fit
cache) means *more* levels, and that per-level overhead exceeds the cache-locality
gain. The minimal microbench had none of that overhead; the real engine does.

## Verdict

Third time this session a microbench win died on integration (after SIMD
bucket-index and the winner tree). The lesson is now a pattern: **logos's engine is
pass-overhead-bound, not just memory-bound** — reducing per-pass memory cost by
adding passes doesn't pay, because each pass isn't free.

The *other* half of the idea — private per-thread buckets to avoid cross-core
traffic — is a genuine win only on **multi-socket NUMA** (writes stay on the local
memory controller). On a single socket the top-level scatter is already ~bandwidth
(~16 ms, per the merge crux), so making it private just adds a gather pass. Neither
this sandbox nor a single-socket 20-core box can show that win; it needs multiple
memory controllers.

`COARSE_K` is now `#ifndef`-guarded so it can be tuned per machine, with 2048 the
measured default. But no default change ships: the data says 2048 is right.
