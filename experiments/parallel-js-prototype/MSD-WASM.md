# Integrated large-n architecture: MSD partition + parallel WASM radix

Combines all three levers into the 1B+ design. Int32 path. Verified correct (P=4).

## Files
- kernel.c / kernel.wasm  -- shared-memory WASM kernels: bias, partition_top,
  radix_u32 (byte-skip LSD), unbias. Build:
    clang --target=wasm32 -O3 -fno-builtin -nostdlib -matomics -mbulk-memory \
      -Wl,--no-entry -Wl,--import-memory -Wl,--shared-memory -Wl,--max-memory=1073741824 \
      -Wl,--export=bias -Wl,--export=unbias -Wl,--export=partition_top -Wl,--export=radix_u32 \
      -o kernel.wasm kernel.c
- msd_st.js        -- single-threaded pipeline (correctness + 6x vs V8 @ 4M)
- parallel-msd.js  -- P workers share ONE WebAssembly.Memory; each sorts a disjoint
                      contiguous group of top-byte buckets with radix_u32.

## Why this design
1. MSD-partition by top byte -> 256 contiguous buckets.
2. Workers own disjoint bucket groups -> the parallel sort phase has ZERO barriers
   (vs 14 in the LSD-parallel version) and is NUMA-local (each worker touches only
   its own memory region).
3. The kernel's byte-skip auto-drops the now-constant top byte -> each bucket sorts
   only its lower 3 bytes, no special-casing.

## Scaling
- Only true serial step: the partition offset-scan (~0.03%). bias/partition/unbias
  are all parallelizable (left serial in the demo for clarity).
- Practical ceiling is memory bandwidth; MSD minimizes the gating (no barrier stalls,
  per-worker-local traffic).
- Stacked estimate on large-n int data: ~6x (WASM radix vs V8) x ~3-6x (bandwidth-
  limited parallel) ~= 20-30x over a consumer Array.sort(), before the bandwidth wall.

## Not done
Float coarse path + counting in WASM (recursion + index-cache) -- larger separate port.
Single-core sandbox shows correctness + structure, not raw speedup.
