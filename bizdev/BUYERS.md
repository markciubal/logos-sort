# BUYERS: who sorts what, and whether they would switch

Compiled 2026-11-06 from public sources (every market fact carries a URL; "unverified" means looked for and not
found) and from CLAIMS.md. Verdicts use the decision frame at the end: speed at their block size, scratch within
their budget, payload and type support, stability, ISAs, license fit, maintenance risk (one author), and what they
give up. "Yes if" always points at a GAPS.md item. Under the current license (IP.md) every "integrate" verdict is
"no"; the verdicts below assume the license question (QUESTIONS q5) is answered with an OSI license, and say so.

## Summary

| segment | best target | verdict | the binding gap |
|---|---|---|---|
| open-source analytics engines | DuckDB; Velox | integrate, **yes if** GAPS 0, 1, 4 and a measured win over their own kernel on their rows | license; reentrancy; key plus payload |
| | ClickHouse | integrate by reimplementation at best; their own data says a kernel win is worth 1.0-1.55x at SQL level | license; types (GAPS 5); the k-way merge they did not change |
| | Polars, DataFusion / arrow-rs | ignore until there is a Rust engine at parity and an OSI license | license (cargo-deny / ASF Category X); language |
| | Arrow C++ | no: requires a stable sort | R6 |
| proprietary engines | Snowflake, Databricks, Firebolt | hire; internals unknown; two of three are moving to Graviton, so ARM is a precondition for any integration | GAPS 9; no public kernel to benchmark against |
| | SingleStore, Kinetica | ignore | x86-only but unknown internals; Kinetica's license bars third-party code |
| standard libraries and runtimes | x86-simd-sort (now under the numpy GitHub org) and through it NumPy, OpenJDK, PyTorch | integrate: the structure-detecting front end as a contribution to the library ("logos as router, SIMD as engine") | license must be BSD-3-compatible (GAPS 0); the fused front end exists (`-DLOGOS_XSS_LEAF`) |
| | libstdc++, libc++, Rust std, Go, .NET, CPython | no: comparator-generic, portable-only, or stable-only by policy | R6, R8 |
| ETL and log tooling | Spark, Lucene / Elasticsearch / OpenSearch, Loki, Vector | ignore: JVM, Go or Rust; no C consumer for the log-line win | language |
| HFT | none found | ignore: order books are kept ordered by structure, not sorted | |
| genomics | samtools / htslib | ignore: I/O-bound radix on 100-byte records; and the author's license Section 2(c) bars part of the field | license (both directions) |
| GPU and CPU vendors | NVIDIA CCCL, AMD AOCL, Arm libraries | ignore: GPU has no logos code; AMD and Arm ship no sort | |
| | Google Highway (VQSort) | integrate only as a portability layer for logos's own kernels (GAPS 9), not as a buyer | |

## 1. Open-source analytics engines

### DuckDB (MIT)

- What they sort: since 1.4 (PR #17584, blog 2025-09-24) each thread runs vergesort (run detection) then ska_sort
  (MSD radix on the first 64-bit word of the key) then pdqsort, and a parallel Merge Path k-way merge
  (https://duckdb.org/2025/09/24/sorting-again.html, code https://github.com/duckdb/duckdb/tree/main/src/common/sort).
  Keys are one binary-comparable blob from `create_sort_key`; keys under 16 bytes are two uint64 plus a payload
  pointer; rows live in 256 KiB pages. A double becomes a 64-bit total-order key, the same key logos uses.
- Block size: the vector size is 2048 (https://duckdb.org/docs/current/internals/vector) but the sort operator sorts
  everything a thread has buffered, so the unit is "a thread's share of the partition until the memory limit".
- Memory and spill: external sort by design (https://duckdb.org/2021/08/27/external-sorting.html); the buffer
  manager accounts for allocations, so bytes asked for count, not bytes touched.
- Stability: ORDER BY "may not use a stable algorithm" (https://duckdb.org/docs/2.0/sql/dialect/order_preservation.html). Unstable is fine.
- ISAs: linux-amd64/arm64, osx-universal, windows-amd64/arm64 (https://duckdb.org/docs/installation/index). An x86-only
  kernel must be behind an `#ifdef` with the existing path as the fallback; acceptable but weaker.
- License fit: MIT core that is "dependency-free" with dependencies in extensions
  (https://duckdb.org/2024/03/22/dependency-management); vendored sorts are pdqsort (zlib), ska_sort (BSL-1.0), vergesort
  (MIT). A source-available or commercial license cannot be vendored.
- How they evaluate: their own microbenchmarks and TPC-H lineitem at SF1-100 (the 1.4 blog), historically against
  ClickHouse, HyPer and pandas (https://duckdb.org/science/sorting). The blog states the new sort is 30% slower at one
  thread than the old one: a public, specific opening.
- Reimplement or license: they vendor permissive header libraries and otherwise reimplement.
- Entry: GitHub Discussions and issues (https://github.com/duckdb/duckdb/discussions).
- Would they switch? **Yes if** (a) the license is OSI (GAPS 0), (b) the engine is reentrant (GAPS 1; one sort per
  pipeline thread), (c) it sorts (64-bit key, pointer) rows (GAPS 4), and (d) a benchmark on their rows beats
  vergesort + ska_sort + pdqsort, which our report does not contain: our near_sorted 3.65x is against a vector
  quicksort with no run detection, and vergesort has run detection. See PITCH-duckdb.md.

### ClickHouse (Apache-2.0, CLA)

- What they sort: `::sort` is ipnsort (a C++ port of Rust's `sort_unstable`), `::stableSort` is driftsort, both
  since PR #106650 merged 2026-06-18 (https://github.com/ClickHouse/ClickHouse/pull/106650); pdqsort kept for
  non-contiguous iterators; `partial_sort` / `nth_element` are miniselect's Floyd-Rivest
  (https://github.com/ClickHouse/ClickHouse/blob/master/base/base/sort.h). For arithmetic columns with 256 <= n <= 2^32
  and no LIMIT, `ColumnVector::getPermutation` uses an LSD radix on (key, index) records with floats by sign flip
  and NaN last (https://github.com/ClickHouse/ClickHouse/blob/master/src/Columns/ColumnVector.cpp,
  https://github.com/ClickHouse/ClickHouse/blob/master/src/Common/RadixSort.h). Output is a permutation of row indices.
- Block size: `max_block_size` default 65,409 rows (https://clickhouse.com/docs/reference/settings/session-settings/max).
  At 65k our 14-thread engine runs one thread and our one-thread rows at 100k are 0.75-0.90 of x86-simd-sort on
  random data (00-STATE 6.3).
- Stability: ties "returned in an arbitrary and non-deterministic order"
  (https://clickhouse.com/docs/sql-reference/statements/select/order-by); a stable path exists separately.
- ISAs: Linux x86_64 full; AArch64, macOS, FreeBSD best-effort (https://clickhouse.com/support/platforms).
- License fit: contrib goes in `contrib/` as a submodule with its license listed in `system.licenses`
  (https://clickhouse.com/docs/development/contrib); no written allowlist, but every vendored sort is permissive.
- How they evaluate: per-PR `performance_comparison` CI and ClickBench (https://github.com/ClickHouse/ClickBench/).
  Their own number for the ipnsort/driftsort change: kernel 3x over pdqsort on random u64, but SQL-level 1.1-1.55x
  single-threaded and about 1.0x at full parallelism, "limited by the unchanged k-way merge" (PR #106650). A
  radix + pdqsort change (#35961) was merged and then reverted after wrong results.
- Reimplement or license: they port (ipnsort from Rust to C++) and vendor permissive code; they would reimplement.
- Entry: GitHub issues and PRs.
- Would they switch? **No** as a library; **yes if** reimplemented under their license on their types (GAPS 0, 5:
  Int64 / UInt64 / Float32 columns, not only f64 and i32) and reentrant (GAPS 1). Their own measurement says a
  faster kernel is worth at most 1.55x at the SQL level and nothing when parallel, so the buyer that notices is
  the single-threaded ORDER BY on structured columns (dates, low-cardinality keys), which is CLAIMS G1-G3.

### Polars (MIT)

- Sorts with `slice::sort_unstable_by` or rayon `par_sort_unstable_by` (Rust std ipnsort), arg_sort on (idx, value)
  pairs, multi-column through a byte-comparable row encoding reimplemented from arrow-rs (#5443)
  (https://docs.rs/polars-core/0.53.0/x86_64-unknown-linux-gnu/src/polars_core/chunked_array/ops/sort/mod.rs.html,
  https://github.com/pola-rs/polars/blob/main/crates/polars-row/src/lib.rs). `maintain_order` default false.
- Morsel: 100,000 rows (`POLARS_IDEAL_MORSEL_SIZE`,
  https://docs.rs/crate/polars-utils/latest/source/src/ideal_morsel_size.rs).
- ISAs: wheels for linux x86_64/aarch64, macOS x86_64/arm64, Windows amd64/arm64; an `rtcompat` build for non-AVX2 CPUs
  (https://docs.pola.rs/user-guide/installation/).
- License fit: cargo-deny allowlist (Apache-2.0, BSD, MIT, ISC, Zlib, BSL-1.0, CC0, Unicode, CDLA-Permissive)
  enforced in CI (https://raw.githubusercontent.com/pola-rs/polars/main/deny.toml).
- How they evaluate: PDS-H (https://github.com/pola-rs/polars-benchmark), their 2024 benchmark post.
- Would they switch? **No** today: Rust-native, a C dependency with pthreads and globals would not pass review, and
  the Rust engine in this tree is v2 and behind. **Yes if** GAPS 0 and a Rust port at parity (GAPS 14), which is a
  project in itself. Ignore for now.

### Apache DataFusion / arrow-rs (Apache-2.0, ASF)

- `ExternalSorter` uses arrow-ord `lexsort_to_indices` + `take`, `sort_unstable_by` and `select_nth_unstable_by` for
  limits, spills to disk; the arrow-row format carries memcmp keys for merges
  (https://github.com/apache/datafusion/tree/main/datafusion/physical-plan/src/sorts,
  https://docs.rs/arrow-ord/latest/src/arrow_ord/sort.rs.html). Batch size 8192, `sort_in_place_threshold_bytes` 1 MiB
  (https://datafusion.apache.org/user-guide/configs.html).
- An MSD radix attempt (#21525) was closed 2026-04-14 with "no improvement" (https://github.com/apache/datafusion/pull/21525).
- License fit: ASF Category X excludes source-available licenses (https://www.apache.org/legal/resolved.html).
- Would they switch? **No**: Rust, ASF license policy, and their own radix experiment found nothing to gain at
  8192-row batches. Ignore.

### Velox (Apache-2.0, Meta CLA)

- `PrefixSort`: a fixed-width normalized key (int16/32/64, float, double, timestamp, hugeint, string prefix up to 16 B)
  plus a row pointer, compared 8 bytes at a time after `bswap64`, sorted by their own three-way quicksort ("we can
  not use std::sort directly"), the rest through `RowContainer::compare`; timsort on the spill path
  (https://github.com/facebookincubator/velox/blob/main/velox/exec/PrefixSort.cpp,
  https://facebookincubator.github.io/velox/configs.html: `prefixsort_normalized_key_max_bytes` 128,
  `prefixsort_min_rows` 128). Row-based sort measured 1.9-3.9x over columnar
  (https://meta-velox.netlify.app/blog/why-row-based-sort).
- Block: `SortBuffer` sorts all buffered rows; output batches 1024-10,000 rows.
- ISAs: Linux x86_64 and AArch64, macOS; x86 minimum SSE4.2, default AVX2 (https://github.com/facebookincubator/velox/blob/main/README.md).
- How they evaluate: `OrderByBenchmark`, `PrefixSortBenchmark`, `RowContainerSortBenchmark`
  (https://github.com/facebookincubator/velox/tree/main/velox/exec/benchmarks).
- Would they switch? **Yes if** GAPS 0, 1 and 4: their (normalized key, row pointer) record is exactly the pair
  the string prefix-key path already sorts, so a key-value entry would plug in; the benchmark to run is
  `PrefixSortBenchmark` with logos sorting the (key, pointer) array. Second target after DuckDB.

### Apache Arrow C++ (Apache-2.0)

- `sort_indices` is `std::stable_sort` on uint64 indices, a counting sort for small integer ranges, and documented
  as a stable sort (https://github.com/apache/arrow/blob/main/cpp/src/arrow/compute/kernels/vector_array_sort.cc,
  https://arrow.apache.org/docs/python/generated/pyarrow.compute.sort_indices.html).
- Would they switch? **No**: stability is the contract (CLAIMS R6).

## 2. Proprietary engines

Public information is thin; nothing names a sort algorithm.

| engine | what is public | ISAs | license posture | verdict |
|---|---|---|---|---|
| Snowflake | SIGMOD'16 paper: columnar, vectorized, spilling; top-k pruning (https://www.cs.purdue.edu/homes/csjgwang/CloudNativeDB/SnowflakeSIGMOD16.pdf, https://docs.snowflake.com/en/user-guide/querying-top-k-pruning-optimization) | moved warehouses to Graviton (https://aws.amazon.com/blogs/apn/how-snowflake-optimized-its-virtual-warehouses-for-sustainability-using-aws-graviton/) | proprietary; CLA for OSS drivers | **hire**: no kernel to benchmark against; ARM first (GAPS 9) |
| Databricks Photon | SIGMOD'22: vectorized C++, no sort description; ties non-deterministic (https://www.databricks.com/wp-content/uploads/2022/07/Photon-A-Fast-Query-Engine-for-Lakehouse-Systems.pdf) | x86-64 and Graviton2/3 (https://docs.databricks.com/aws/en/compute/graviton) | proprietary | **hire**; ARM first |
| SingleStore | LLVM codegen; columnstore sort key is an index; AVX2 on encoded data (https://www.singlestore.com/blog/memsql-processing-shatters-trillion-rows-per-second-barrier/) | x86_64 only, SSE4.1/AVX2 (https://docs.singlestore.com/db/v9.1/reference/configuration-reference/cluster-configuration/system-requirements-and-recommendations.md) | proprietary; OSS component list has no sort library | ignore until a contact exists |
| Firebolt | ClickHouse-derived; Firebolt Core under Elastic License 2.0; NOTICE lists pdqsort, kxsort, miniselect (https://raw.githubusercontent.com/firebolt-db/firebolt-core/main/NOTICE.md) | amd64 and arm64; managed service on Graviton (https://www.firebolt.io/blog/firebolt-arm-rollout) | source-available themselves | as ClickHouse: reimplementation; **yes if** GAPS 0, 1, 5, 9 |
| Kinetica | GPU engine; CPU builds exist | ARM64, x86-64 AVX2, ppc64le | Developer License bars third-party code without consent (https://www.kinetica.com/wp-content/uploads/2024/11/Kinetica-Developer-License-Agreement-Dec-10-2021.pdf) | ignore |

## 3. Standard libraries and runtimes

| target | sort today | why not / why | verdict |
|---|---|---|---|
| libstdc++ | introsort (https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/include/bits/stl_algo.h) | comparator-generic, portable, FSF assignment | no |
| libc++ | introsort partly from pdqsort, bitset partition, branchless small sorts, no SIMD by policy (https://github.com/llvm/llvm-project/blob/main/libcxx/include/__algorithm/sort.h; https://danlark.org/2022/04/20/changing-stdsort-at-googles-scale-and-beyond/) | ideas are reimplemented in tree; a non-Apache dependency needs Board approval (https://llvm.org/docs/DeveloperPolicy.html) | no |
| Rust std | driftsort / ipnsort since 1.81 (https://github.com/rust-lang/rust/pull/124032) | "no architecture-specific code"; license allowlist (https://github.com/rust-lang/rust/blob/master/src/tools/tidy/src/deps.rs) | no |
| Go | pdqsort since 1.19 (https://go.dev/doc/go1.19) | pure Go, reimplemented from C++/Rust | no |
| Java / OpenJDK | DualPivotQuicksort; **AVX-512 and AVX2 intrinsics from x86-simd-sort in JDK 22/23** (https://github.com/openjdk/jdk/pull/14227), Linux x86_64 only | the one runtime that took a SIMD sort library, contributed by Intel as HotSpot intrinsics | integrate only through x86-simd-sort (below) |
| .NET | introsort; clean-room ports of non-permissive code "will generally not be accepted" (https://github.com/dotnet/runtime/blob/main/CONTRIBUTING.md) | managed, no SIMD in Array.Sort | no |
| NumPy | np.sort uses x86-simd-sort (AVX-512 in 1.25, AVX2 in 2.0) and Highway VQSort on AArch64 (2.0) as git submodules (https://numpy.org/doc/stable/release/1.25.0-notes.html, https://github.com/numpy/numpy/pull/24018, https://github.com/numpy/numpy/pull/25045); maintainers "uncomfortable adding yet another submodule" | the structure routes (near-sorted, quantized, few-distinct) are exactly where np.sort's current kernels do nothing special; the path in is the library NumPy already vendors | integrate via x86-simd-sort |
| CPython | timsort with powersort merge policy (https://github.com/python/cpython/pull/28108); stable guaranteed | stable only | no |

### x86-simd-sort (BSD-3; repository now reports `numpy/x86-simd-sort`, https://api.github.com/repos/intel/x86-simd-sort)

- APIs: qsort, qselect, partial, key-value, argsort, object sort; 16/32/64-bit and `_Float16`; AVX-512 and AVX2 with
  runtime dispatch (https://github.com/intel/x86-simd-sort/blob/main/src/README.md). Adopted by NumPy, OpenJDK, PyTorch
  (merged then reverted, status unverified), GRAPE, OceanBase (README). DCO, no CLA. Maintainers: sterrettm2
  (Intel), r-devulap (Raghuveer Devulapalli, now IBM).
- What logos adds to it: a sample-once front end that routes nearly sorted, quantized and low-cardinality inputs to
  counting and run paths before the quicksort (CLAIMS G1-G3, 1.2-3.7x at one thread with near-zero scratch), with
  the quicksort unchanged on random data (R1: level). The fused form already exists in this tree
  (`-DLOGOS_XSS_LEAF`, FINDINGS 3bi: level with the bare library call on random data).
- Would they take it? **Yes if** GAPS 0 (BSD-3-compatible), the contribution is a front end inside their dispatch,
  and the win reproduces in their benchmark harness on their shapes. This is the integration with the largest
  downstream reach (NumPy, OpenJDK) for the least new code.

### Google Highway / VQSort (Apache-2.0 or BSD-3; Google CLA)

- 27 targets including NEON, SVE, RVV, AVX-512 (https://github.com/google/highway); key-value variants; unstable;
  adopted by NumPy on AArch64 (https://github.com/numpy/numpy/pull/24018). On this box its AVX2 build is the slowest
  vector sort in the report (CLAIMS G10).
- Verdict: not a buyer; a possible portability layer for logos's own kernels (GAPS 9) if ARM is wanted without
  hand-written NEON.

## 4. ETL and log tooling

| target | sort today | verdict |
|---|---|---|
| Apache Spark | `UnsafeInMemorySorter` with (pointer, 8-byte prefix) records; LSD radix on prefixes when `spark.sql.sort.enableRadixSort` (default), else TimSort (https://github.com/apache/spark/blob/master/core/src/main/java/org/apache/spark/util/collection/unsafe/sort/RadixSort.java; https://issues.apache.org/jira/browse/SPARK-14724) | ignore: JVM; ASF policy |
| Lucene / Elasticsearch / OpenSearch | in-house Java sorters: `MSBRadixSorter`, `StableMSBRadixSorter`, `IntroSorter`, `TimSorter` (https://lucene.apache.org/core/api/core/org/apache/lucene/util/Sorter.html) | ignore: Java |
| Grafana Loki | loser-tree merge of entries (https://raw.githubusercontent.com/grafana/loki/main/pkg/iter/entry_iterator.go); AGPL | ignore: Go |
| Vector.dev | MPL-2.0, cargo-deny allowlist (https://raw.githubusercontent.com/vectordotdev/vector/master/deny.toml); nothing public on sorting | ignore: Rust |

The log-line claim (CLAIMS G8, 2.1x at less scratch) has no C or C++ consumer in this map. A buyer for it would
be a C++ log indexer or a columnar string sort inside an engine (DuckDB and Velox sort string prefixes as part of
their key blobs: GAPS 4 and 5 are the path).

## 5. HFT

Nothing public: open-source matching engines keep books ordered by structure (radix trees, `std::multimap`,
ordered vectors with `lower_bound`; https://isocpp.org/blog/2024/09/cppcon-2024-when-nanoseconds-matter-ultrafast-trading-systems-in-cpp-david);
kdb+ `asc` picks an algorithm by type and distribution (https://code.kx.com/q/ref/asc/). No firm has written about
SIMD or radix sorting. Ignore.

## 6. Genomics

samtools sort runs a hand-written LSD byte radix per thread on coordinate order and a k-way heap merge
(https://raw.githubusercontent.com/samtools/samtools/develop/bam_sort.c; MIT; linux-aarch64 and osx-arm64 builds).
Records are ~100+ bytes and the step is I/O-bound. No bioinformatics pipeline using x86-simd-sort or VQSort was found;
the closest is RADULS in KMC3 (https://ar5iv.labs.arxiv.org/html/1703.00687). Separately, the author's license
Section 2(c) prohibits population-ancestry classification and forensic databases, which are mainstream uses of
samtools. Ignore.

## 7. GPU and CPU vendors

| vendor | sort | verdict |
|---|---|---|
| NVIDIA CCCL (CUB) | `DeviceRadixSort` with onesweep, stable, key-value, 29.4 GKey/s on A100 (https://nvidia.github.io/cccl/unstable/cub/api/structcub_1_1DeviceRadixSort.html, https://arxiv.org/abs/2206.01784) | ignore: logos has no GPU code (CLAUDE.md direction 3) |
| Intel (x86-simd-sort) | see section 3 | integrate |
| AMD AOCL | no sort routine in any component (https://docs.amd.com/r/en-US/57404-AOCL-user-guide/AOCL-User-Guide-57404) | ignore |
| Arm Compute Library / Arm NN / ArmPL | no sort, argsort or top-k operator (https://arm-software.github.io/ComputeLibrary/latest/operators_list.xhtml) | ignore |

## 8. Written dependency-license policies (why GAPS 0 ranks first)

| project | rule | source |
|---|---|---|
| ASF (Arrow, DataFusion, Spark, Lucene) | Category X may not be included: GPL/AGPL/LGPL, SSPL, BSL 1.1, Commons Clause, FSL, source-available licenses | https://www.apache.org/legal/resolved.html |
| Rust | tidy allowlist (MIT, Apache-2.0, ISC, Zlib, BSD...); no exception crates in the runtime | https://raw.githubusercontent.com/rust-lang/rust/master/src/tools/tidy/src/deps.rs |
| Polars, Vector | cargo-deny allowlist, CI fails otherwise | https://raw.githubusercontent.com/pola-rs/polars/main/deny.toml |
| .NET runtime | copied code must be permissive and listed in THIRD-PARTY-NOTICES | https://github.com/dotnet/runtime/blob/main/CONTRIBUTING.md |
| LLVM | other licenses only with Foundation Board approval | https://llvm.org/docs/DeveloperPolicy.html |
| NumPy | "compatibly licensed" bundles; NEP 54 on submodule licensing | https://numpy.org/neps/nep-0054-simd-cpp-highway.html |
| DuckDB, ClickHouse, Velox, Go, CPython, GCC | no written allowlist found; every vendored sort in them is permissive | section 1 |

## 9. Published results of a sort library replacing an engine's own (what a buyer expects to see)

- ClickHouse #106650 (2026): kernel 3x, SQL level 1.1-1.55x single-threaded, ~1.0x parallel.
- DuckDB 1.4 (2025): 1000M ascending 15.3 -> 1.48 s, random 17.6 -> 6.49 s, TPC-H SF100 lineitem 274 -> 81 s, one
  thread 30% slower (https://duckdb.org/2025/09/24/sorting-again.html).
- NumPy 1.25 / 2.0: 15x / 9x on 16/64-bit with AVX-512, 6x argsort, 16x on AArch64 with VQSort.
- OpenJDK 22: int ~7.4x, double ~4.5x from x86-simd-sort intrinsics (https://github.com/openjdk/jdk/pull/14227).
- DataFusion #21525 radix: no improvement at 8192-row batches; closed.
- Spark SPARK-14724: radix 15.5x on the prefix-sort microbenchmark.

The pattern: libraries get in when they are permissive, when the contributor does the integration, and when the
win shows on the engine's own benchmark. A 3x kernel became 1.0-1.55x at ClickHouse's SQL level; expect the same
discount everywhere.

## Decision frame (applied above)

Would they switch? Judge on: speed at their block size (ClickHouse 65k, Polars 100k, DataFusion 8k, DuckDB and
Velox a thread's buffered share); scratch within their budget (allocations are accounted, so bytes asked for);
payload and type support (every one sorts (key, rowid) or (key, pointer); int64 timestamps are universal); stability
(Arrow C++ and CPython require it; the others have a separate stable path); portability (every engine ships ARM
builds; three proprietary engines run on Graviton); license fit (section 8); maintenance risk (one author, no CI, one
compiler); what they give up (a vendored permissive sort they already tuned). Under those tests no target says
"yes" today; DuckDB, Velox and x86-simd-sort say "yes if" with GAPS 0, 1, 4 as the ifs.
