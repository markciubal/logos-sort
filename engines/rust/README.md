# logos-sort (Rust)

> **Parity status (read first; 2026-11-06).** This Rust port is the **v2 engine**:
> parallel sample sort + coarse interpolation leaf, plus (v0.3) native int32 and a
> strict in-place mode. It is **well behind the C engine**, which has the JS-faithful
> dispatcher and, since September 2026, some thirty structure-reading routes added
> one paired A/B at a time: run relocation and the knot route, the short-reach route,
> the near-sorted parallel route, lattice and counting routes with a verified perfect
> hash, an AVX2 quicksort leaf level with x86-simd-sort's, a histogram-free parallel
> partition with uniformity certificates, strings on 8-byte prefix keys, a lazy sorted
> view, a persistent worker pool. The matrix is `../c/PARITY.md`; the record is
> `../c/CHANGELOG.md` and `../../FINDINGS.md`. Bringing those to Rust is a multi-stage
> port, not started. Use the C engine for the fastest, most complete implementation;
> this crate is the safe-Rust reference and the in-place experiment.

## v0.3.0
- **Native i32** path (was already present) and a **strict in-place mode**
  (`sort_f64_inplace`, `sort_i32_inplace`): cycle-following MSD radix, ~1n
  memory vs the buffered path's ~2n, in pure safe Rust (the permutation is
  slice swaps, so safety is compiler-checked). Slower than buffered — the
  memory-ceiling tool, matching the C in-place variant. Verified against
  `sort_unstable` across shapes and sizes.

## Build / test / bench

```sh
cargo test --release      # correctness vs sort_unstable, all shapes x threads
cargo run --release --example bench   # the fair fight (needs rayon; dev-dependency only)
```

## Safety story

Safe Rust everywhere except one block. The histogram phase shares a read-only
borrow; the per-bucket sort phase hands threads disjoint `&mut` slices via
`split_at_mut`, so its data-race freedom is **compiler-checked**. Only the
scatter — interleaved disjoint writes into the output — needs `unsafe`, and the
disjointness argument (per-thread, per-bucket ranges from the prefix-sum
construction) is documented at the site. Degenerate inputs (non-finite spans,
extreme concentration, depth exhaustion) fall back to `sort_unstable` itself,
so the worst case is the standard library's own.

Results never depend on the thread count (tested). NaN ordering is unspecified.

## The fair fight: `examples/bench.rs`

Races logos (1 thread) vs `sort_unstable` (ipnsort) and logos (P threads) vs
**`rayon::par_sort_unstable`** at the same thread count, on the same seven
data shapes as the C harness, verifying every output against the reference.
`rayon` is a dev-dependency only — the library itself has no dependencies.

**Important caveat:** this Rust engine is the *v2* design (parallel sample
sort + coarse interpolation leaf). It has **not** received the v3 JS-faithful
dispatcher that the C engine has (run-merge, galloping, momentum, skew probe,
float radix, ...). Measured on the single-core sandbox, 8M, sequential column:

| shape | sort_unstable | logos (v2) | |
|---|---|---|---|
| int_wide | 313 ms | 246 ms | 1.27× |
| uniform | 278 ms | 258 ms | 1.08× |
| near_sorted | 139 ms | 148 ms | 0.94× |
| heavy_tail | 301 ms | 356 ms | 0.84× |
| bimodal | 309 ms | 400 ms | 0.77× |
| int32 | 222 ms | 291 ms | 0.76× |
| quantized | 131 ms | 226 ms | 0.58× |
| few_distinct | 57 ms | 152 ms | 0.38× |

Against `qsort` this leaf looked like 6×; against ipnsort it wins 2 of 8
shapes. The shapes it loses are exactly the ones the v3 leaf handles (run
merging, skew, quantization). Porting v3 to Rust is the prerequisite for a
meaningful parallel result here; until then treat the C `parbench` as the
flagship's fair fight.

## Earlier results (single-core sandbox, 8M uniform, best of 5)

| sort | f64 | i32 |
|---|---|---|
| `sort_unstable` (stdlib) | 355.9 ms | 294.8 ms |
| logos, 1 thread | **265.2 ms** (1.34× faster) | 315.9 ms (1.07× slower) |
| logos, 2 / 4 / 8 threads | 405 / 483 / 596 ms | — |

Read honestly:

- **f64: logos wins 1.34×** single-threaded. Interpolation (place by value)
  beats even ipnsort-class comparison sorting when every comparison goes
  through a `total_cmp` call.
- **i32: logos loses 1.07×** — self-inflicted. `sort_i32` widens to f64
  (exact, but two O(n) passes + an 8n allocation), while `sort_unstable` on
  `i32` runs its best discipline (direct `Ord`, no comparator indirection).
  A native integer interpolation leaf (i64 arithmetic, half the memory traffic
  of f64) is the obvious fix and would likely flip this row.
- **threads>1 slower here is correct**: the sandbox has one core, so those rows
  measure coordination overhead, not parallelism. On real cores the projection
  from the C version applies (~2× at 2, ~4× at 4, flattening at the memory-
  bandwidth wall around 8 on one socket).
- Rust sequential (265 ms) vs the C version (234 ms): ~13% behind, mostly
  bounds checks in the two hot loops; closable with documented
  `get_unchecked`, deliberately not done yet — safe first, fast second.
