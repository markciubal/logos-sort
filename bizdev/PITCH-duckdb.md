# PITCH-duckdb: a benchmark reproduction, not a sales call

**Status: DO NOT SEND.** Blocked on QUESTIONS.md q5 (license: DuckDB vendors only permissive code) and q3 (patent
decision before any public step). Written now because CLAIMS.md has three or more green rows relevant to DuckDB
(G1 near_sorted, G2 quantized, G3 few_distinct, G4-G7 block shapes, G8 log lines; all one-thread, all on key types
DuckDB sorts as 64-bit keys) and because DuckDB's own 1.4 sort blog states a one-thread regression. Nothing here
is a claim against DuckDB's sort: our report has never run their kernel.

## Why DuckDB first

- MIT, "dependency-free" core that vendors permissive header libraries (pdqsort zlib, ska_sort BSL-1.0, vergesort
  MIT): a C engine under an OSI license can be vendored the same way.
- They publish sort benchmarks and designs (2021 external sort; 2025 "Sorting again" rewrite,
  https://duckdb.org/2025/09/24/sorting-again.html) and say in the latter that the new sort is 30% slower at one thread
  than the old one. A one-thread improvement is a conversation they have already opened.
- Their key is a binary-comparable blob; keys under 16 bytes are two uint64 plus a payload pointer. A double is one
  64-bit total-order key, which is the key logos's partition, lattice and run routes already use.
- Unstable sort is their contract (ORDER BY "may not use a stable algorithm").

## What we do not know, and must measure before anything is said

Our green rows are against x86-simd-sort, IPS4o and timsort. DuckDB's pipeline is vergesort (run detection) then
ska_sort (MSD radix on the first 64-bit word) then pdqsort. Two of our three strongest shapes are exactly what those
components target: vergesort finds the runs in near_sorted and the block shapes; ska_sort's radix handles
few_distinct and quantized without comparisons. The honest expectation is that the margins are smaller than in our
report and may be zero. Specific predictions, written before measuring:

| shape (1M and 10M, one thread) | our rival today | DuckDB kernel on the same rows | predicted logos / DuckDB | reasoning |
|---|---|---|---|---|
| near_sorted (1% random swaps) | 3.4-3.7x over x86-simd-sort | vergesort sees runs of ~100 elements, hands to ska_sort | 1.3-2.0x | vergesort requires long runs; 1% random swaps break them every ~100 elements, so most of the array falls to the radix (8 passes on 8-byte keys); logos's near-sorted route does one keep/reject pass plus a merge |
| quantized (values on a lattice) | 2.3-2.4x | ska_sort MSD radix, 2-3 effective passes on keys with few distinct bytes | 1.2-1.8x | a direct-index count beats a radix by the number of passes it skips; ska_sort already skips constant bytes |
| few_distinct | 1.2-1.5x | ska_sort, one effective pass | 0.9-1.3x (may be level) | one radix pass on 64-bit keys is close to a counting pass |
| block_perm / block_overlap / block_twist | 2.1-3.6x over timsort | vergesort detects the blocks as runs and merges them | 1.0-1.5x | vergesort plus a merge is what timsort does with a cheaper merge; logos's relocation moves disjoint blocks by memcpy and merges only crossings |
| uniform | 0.91x (1M) / 1.02x (10M) vs x86-simd-sort | ska_sort radix, 8 passes, then pdqsort below a threshold | 0.9-1.3x | an LSD/MSD radix on 64-bit random keys is bandwidth-bound at 8 passes; our partition does 2-3 passes but allocates 16 B/el |
| strings (prefix keys) | 0.84-2.1x | their key blob holds the string prefix; ska_sort on the first word | unknown | depends on how much of the key is in the first 64 bits |

If the measured ratios come in under 1.3x on near_sorted and quantized at one thread, there is no pitch; record it
in LOG.md and move DuckDB to "ignore".

## The exact benchmark to run (their workload, their kernel, our shapes)

1. Build a contender `duckdb_sort` in `engines/c/scalebench.cpp` from the three headers in
   https://github.com/duckdb/duckdb/tree/main/third_party (`pdqsort`, `ska_sort`, `vergesort`) in the order their
   `src/common/sort` applies them, on records of (uint64 key, uint64 payload) with the key being the total-order
   encoding of the double (`sort_key` as DuckDB builds it for a DOUBLE column). Sort the same 17 shapes at 100k, 316k,
   1M, 3.16M, 10M, one thread, with the existing memory pass. This needs GAPS 4 (a key-value entry) on our side, or
   the first run sorts bare keys on both sides and is labelled as such.
2. Run DuckDB itself (Python, `duckdb` package, `SET threads = 1` and `= 14`) on a table built from the same shape
   generators (export them to Parquet from `scalebench`'s `gen()`): `SELECT * FROM t ORDER BY x` with the result
   consumed, timed as min of 5. Report alongside the kernel numbers so the pipeline overhead (row layout, merge,
   output materialisation) is visible; expect the kernel difference to be discounted the way ClickHouse's was
   (3x kernel, 1.1-1.55x SQL).
3. Memory: peak traced heap per element on both sides, and resident memory, both in the table.
4. Hardware: the i9-13900H and, before anything is sent, a second machine (CLAIMS.md asterisk; QUESTIONS q10).
   DuckDB ships arm64 builds, so an Apple M-series run would be the most persuasive second machine.
5. Verification: every output checked (bitwise multiset and `<` order), as every bench here does.

## Expected result, with memory (to be replaced by measurements)

If the predictions above hold: logos 1.3-2.0x on near_sorted and 1.2-1.8x on quantized at one thread, level on
uniform and few_distinct, 1.0-1.5x on block shapes; scratch 0.4 and 0.2 B/el on the two wins against whatever
vergesort/ska_sort allocate (ska_sort is in place; vergesort uses a buffer for runs: measure). At 14 threads the
comparison is per-thread sorts merged by their Merge Path, so the parallel claim (G11, G12) does not apply; what
applies is the one-thread row.

## Form of approach

A GitHub Discussion in duckdb/duckdb (not an issue, not a PR): "Benchmark: structure-detecting pre-pass vs
vergesort/ska_sort/pdqsort on 64-bit keys", with the reproduction script, the table, the memory column, the second
machine, and the shapes defined. No adjectives. State the losses in the same table. Offer the code under the
license the author has chosen (OSI, or there is no offer).

## The ask

One of their sort authors (Laurens Kuiper wrote both sort blogs) to run the script on their benchmark machine and
say whether a pre-pass that routes nearly sorted and quantized key columns before vergesort is worth a PR. The
secondary ask, if the numbers hold, is a conversation about doing that integration work, which is the consulting or
hire outcome in QUESTIONS q1.

## What would change the grade of this pitch

- A measured loss to their kernel on near_sorted or quantized: withdraw.
- GAPS 1 (reentrancy) unresolved: their executor sorts on every pipeline thread; the pitch cannot be a PR until it is.
- GAPS 9 (ARM): without it the pre-pass is x86-only, and they ship arm64 and osx-universal; acceptable behind an
  `#ifdef`, weaker.
