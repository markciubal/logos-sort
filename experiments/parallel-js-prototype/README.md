# logos-sort parallel radix (reference design)

Worker-parallel LSD radix for int32 over SharedArrayBuffer. Demonstrates the
structure that scales logos-sort across cores. Verified correct (P=2, P=4).

## Structure (per byte pass, 4 passes)
1. **Parallel histogram** — each worker counts its chunk into a 256-bucket local
   histogram, writes the row into shared memory.
2. **Serial offset scan** — worker 0 computes, for every (worker, bucket) pair,
   the absolute write offset = global bucket start + earlier workers' counts.
   Measured at ~0.03% of total time (the only non-parallelizable step).
3. **Parallel scatter** — each worker scatters its chunk into disjoint per-bucket
   slices of the output. No atomics in the inner loop; stable by construction.

Synchronization: sense-reversing Atomics barrier (14 per sort). Workers persist
across runs via an Atomics go/done protocol (excludes spawn cost from timing).

## Scaling
- Serial fraction s = 0.0003 -> Amdahl predicts near-linear compute scaling.
- REAL ceiling is memory bandwidth: radix is memory-bound, cores share the bus,
  so expect strong-but-sublinear (~3-6x on a 4-8 core desktop). Histogram phase
  (sequential reads) scales better than scatter (random writes, bandwidth hog).
- For many-core / NUMA: partition MSD-first so each socket's buckets stay local.

## Run
    node parallel-radix.js
(Single-core machines show no speedup -- the value is correctness + the model.)
