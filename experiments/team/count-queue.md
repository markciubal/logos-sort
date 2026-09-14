# count-queue: dynamic chunk queues and a parallel merge for the f64 counting routes

Agent: count-queue, overnight team run 2026-09-12. Work copy /tmp/logos-team/count-queue/work
(resumed from a prior session that built the code and wrote the prediction below but measured
nothing under the lock; everything measured here is under `flock /tmp/logos-bench.lock` with the
paired harness). Patch: experiments/team/patches/count-queue.patch (work/ vs pristine/, patch -p1).

STATUS: PARTIAL. The session was cut off by the orchestrator while the decisive run was still
holding the lock. Five of the fourteen rows had landed; the rest continue to append to
/tmp/logos-team/count-queue/run1.log (WSL, survives until reboot), followed by the -DLOGOS_TRACE
mechanism run. Whoever picks this up: read that log first.

## What was tried

The three f64 counting routes (`lowcard_try_f64`, `lattice_try_f64`, `modcard_try_f64`) carved
STATIC per-thread index ranges (`chunk = (n+P-1)/P`, `lo = t*chunk`) for both the map pass and the
emit pass, and lattice/modcard merged serially (lattice: O(slots x P) "merge by index"; modcard: fold
P hash maps into one, then qsort). Ported `count_try_i32`'s atomic chunk queue:

- `cq_t`: an atomic counter over fixed-size chunks (n / (P * 8), floor 16384); map and emit both pull
  from it, so a slow (E-core or preempted) thread simply takes fewer chunks. An aborting thread drains
  the queue so the others stop.
- Each route now spawns ONCE and uses barriers between map | merge | emit instead of a spawn round
  per pass.
- Lattice merge split by slot range across threads, with a prefix over per-thread totals; gstart is
  slot-indexed (empty slots are zero-length runs the emitter skips), so no compaction pass.
- Modcard merge: each thread sorts its OWN hash map into a list (LSD radix on (key,val,cnt) records;
  glibc qsort on 24-byte records measured 13-28 ms per thread in the trace), then P-way merges one
  KEY RANGE (bounds = sample quantiles) across the P lists; thread 0 concatenates. A first design that
  split by hash share and had every thread scan every other thread's map paid 22-35 ms of remote-L2
  traffic at 14T on 20k distinct and was replaced.
- Runtime toggle, one binary: `LOGOS_CQ=0` static, `LOGOS_CQ=1` queue, unset = `LOGOS_CQ_DEFAULT`
  (now 0, opt-in). `LOGOS_CQ_CHUNKS=N` sets chunks per thread (N=1 is "static ranges but one spawn
  and the parallel merges", the control that separates the queue from the spawn/merge change).
- `make cqbench` (paired harness, `./cqbench [pairs] [n] [shape] [threads]`, `./cqbench chunks N ...`),
  `make cqtrace` (`./cqtrace trace` prints per-thread map/emit busy ms and chunks taken per route).
- `make test`: 835 checks pass with the queue on (LOGOS_CQ=1) and off (LOGOS_CQ=0).

## Prediction (written before any measurement, by the prior session)

Ratio = t(static) / t(queue), > 1 means the queue wins, paired harness, 8M:

| shape | route | 14T | 7T |
|---|---|---|---|
| few_distinct (5 values) | lowcard hash | 1.15 | 1.08 |
| quantized (4096 lattice) | lattice | 1.20 | 1.10 |
| lattice_30k (ints 0..30k) | lattice, 65k-slot merge | 1.25 | 1.12 |
| distinct_20k (20k random doubles) | modcard hash | 1.25 | 1.15 |
| lowcard_64 (64 random values) | lowcard hash | 1.12 | 1.08 |
| uniform | partition (untouched) | 1.00 | 1.00 |

Mechanism prediction: chunks per thread vary 1.3-2x with the queue on; map busy max/mean falls from
1.3-2 to under 1.1. Named failure mode: the routes are only 5-10 ms at 14T and 8M, so spawn/barrier
fixed costs may dominate any balance gain; emit is write-bandwidth bound and an E-core is not much
slower at streaming stores.

## What was measured (under the lock; PARTIAL, run cut off)

Calibration first, `flock /tmp/logos-bench.lock ./pairprobe` (same engine both sides, true ratio
1.000): 8M median 0.995, p10 0.873, p90 1.391, resolves 7.7%; 1M resolves 14%. This is a noisy
night (six agents compiling): the documented floor is 3.7% at 8M. Anything under ~1.10 is noise.

`flock /tmp/logos-bench.lock ./cqbench 40 8000000` (40 pairs, ratio = static / queue):

| shape | T | ratio | p10 | p90 | verify | vs prediction |
|---|---|---|---|---|---|---|
| few_distinct | 14 | 1.027 | 0.722 | 1.525 | ok | predicted 1.15: NOT resolved, noise |
| few_distinct | 7 | 1.203 | 0.839 | 1.824 | ok | predicted 1.08: above floor, direction right |
| quantized | 14 | 0.966 | 0.761 | 1.526 | ok | predicted 1.20: WRONG, noise or slightly negative |
| quantized | 7 | 1.225 | 0.862 | 1.660 | ok | predicted 1.10: above floor |
| lattice_30k | 14 | 1.223 | 0.855 | 1.628 | ok | predicted 1.25: on target (the 65k-slot serial merge) |
| lattice_30k 7T, distinct_20k, lowcard_64, zipf, uniform | | not reached | | | | see run1.log |

The trace run (per-thread busy time, the mechanism) had not started when the session was cut.

## Verdict: NEUTRAL on the target shapes at 14 threads, opt-in (LOGOS_CQ=1)

At 14 threads on the two parbench counting shapes the queue plus single spawn plus parallel merge
is within the 7.7% noise floor (1.03 and 0.97). The wins that did land are where a SERIAL phase was
long relative to the route: lattice_30k at 14T (1.22, its serial slot merge walks 65k slots x 14
tables) and both shapes at 7T (1.20, 1.22, where each thread's share is twice as long so a straggler
costs more absolute time, and where 7 threads on a 6P+8E box are more likely to include an E-core
with a full-size share). The prediction's named failure mode looks like the reason at 14T: the
routes are ~5-10 ms and fixed costs and the emit's bandwidth bound leave little for balance to
recover. Default left OFF per rule 8; the code stays as an opt-in with the toggle so the remaining
rows can be finished with the same binary.

## Why, mechanism (what can be said without the trace)

- The p10 of every row is below 1 and the p90 above 1.5: a single pair swings +-50% tonight, so
  even the 1.22 rows rest on the median of 40 and would need ~160 pairs for a 2% claim.
- Balance can only recover (max - mean) of the map pass; FINDINGS 3j measured the lattice map at
  3.0-5.8 ms across threads (max/mean ~1.4) on a 5-10 ms route, i.e. a ceiling of ~1.2-1.3x if the
  map were the whole route. The emit pass and the fixed costs are the rest, and the queue does
  nothing for a bandwidth-bound emit.

## What I would do next

1. Let run1.log finish (or rerun: `flock /tmp/logos-bench.lock ./cqbench 40 8000000`) and then
   `flock /tmp/logos-bench.lock ./cqtrace trace 8000000 2>&1` for the per-thread busy table; grade the
   mechanism prediction (max/mean of map busy, chunks per thread 1.3-2x spread).
2. `flock ... ./cqbench chunks 1 40 8000000`: queue(8/thread) vs queue(1/thread) isolates the dynamic
   balancing from the single-spawn + parallel-merge restructuring. If lattice_30k's 1.22 survives
   with chunks=1, it is the parallel merge, not the queue.
3. If the merge is the win, ship ONLY the parallel lattice merge (it is a pure serial-phase removal,
   no balance argument needed) and leave the queue opt-in.
4. Rerun on a quiet box: tonight's 7.7% floor is twice the documented one.

## Anything surprising

- quantized at 14T went the wrong way (0.97). The lattice path's per-thread table is 4096 slots x
  3 arrays; with 14 threads x 8 chunks the chunk is 71k elements, so the table stays hot either way,
  and the extra barriers (3 vs 2 spawn rounds' worth of joins) may cost what the balance saves.
- The same change is 1.2x at 7 threads on both shapes: the imbalance the owner's idea targets is
  real, it just matters more when each thread carries more.
- glibc qsort on 24-byte records is ~20x slower than an 8-bit LSD radix for 20k entries; that alone
  fixed a 13-28 ms per-thread cost in the first modcard merge design (trace, prior session).

## Commands (repeatable)

```
wsl -e bash -lc "cd /tmp/logos-team/count-queue/work/engines/c && make test && LOGOS_CQ=0 ./harness | tail -1 && make cqbench cqtrace pairprobe"
wsl -e bash -lc "cd /tmp/logos-team/count-queue/work/engines/c && flock /tmp/logos-bench.lock ./pairprobe"
wsl -e bash -lc "cd /tmp/logos-team/count-queue/work/engines/c && flock /tmp/logos-bench.lock ./cqbench 40 8000000"
wsl -e bash -lc "cd /tmp/logos-team/count-queue/work/engines/c && flock /tmp/logos-bench.lock ./cqtrace trace 8000000 2>&1"
wsl -e bash -lc "cd /tmp/logos-team/count-queue/work/engines/c && flock /tmp/logos-bench.lock ./cqbench chunks 1 40 8000000"
```
