# IP: license, patent clock, authorship, prior art

No legal conclusions here. Each section ends with the questions for counsel. Facts about the repository are cited;
facts about law and other projects' licenses are from general knowledge and are marked "verify" where a date or a
clause matters.

## 1. What the tree says today

| fact | where |
|---|---|
| License: "Logos Sort Ethical Source License, Version 1.1", source-available, not OSI-approved by its own text; a written commercial license is required for Commercial Use (Section 3); prohibited uses (Section 2) are absolute and survive a paid license; 90-day internal evaluation is free (4(e)); use as a dependency of an OSI-licensed project is free but downstream commercial use still needs a license (4(g)) | `engines/c/LICENSE`, `engines/js/LICENSE` (identical) |
| Copyright line reads "(c) 2026" with no holder named | LICENSE line 2 |
| The commercial contact is a placeholder: `<commercial-licensing contact to be provided by the author>` | LICENSE Section 3.3 |
| Forks must state they derive from "LogosAdaptive Sort", a name that appears nowhere else in the tree | LICENSE Section 5(c) |
| `engines/rust` and `engines/python` carry no license file; `Cargo.toml` has no `license` field; the root has no LICENSE | repository |
| npm declares `license: SEE LICENSE IN LICENSE` | npm registry metadata |
| Authorship on the public record: `engines/js/package.json` `author` = "Claude Opus 4.6, 4.7, 4.8, Mark Ciubal"; README.md opens with instructions to "a Claude instance continuing this work" | package.json; README.md |
| Public disclosure: npm `logos-sort` 0.1.0 on 2026-05-28, through 0.3.1 on 2026-06-23 (JS engine, its C single-file engine `c/sort.c`, CHANGELOG, README). The GitHub remote returns 404 anonymously on 2026-11-06 | npm registry; `curl` |
| No blog post, preprint, talk or paper is referenced anywhere in the tree | grep |

## 2. License options and what each buyer segment does with them

The segments are those of BUYERS.md. "Takes" means the project's own policy allows the dependency; "legal" means
a corporate legal review would normally pass it.

| option | open-source engines (DuckDB MIT, ClickHouse / DataFusion / Arrow / Velox Apache-2.0, Polars MIT) | proprietary engines (Snowflake, Databricks, SingleStore, Firebolt, Kinetica) | standard libraries and runtimes (libstdc++, libc++, Rust std, Go, NumPy) | ETL / log tooling, HFT, genomics | revenue to the author |
|---|---|---|---|---|---|
| **Current: Ethical Source 1.1** | Cannot take it. Apache projects classify non-OSI and use-restricted licenses as Category X (verify at apache.org/legal/resolved.html); MIT projects will not add a dependency their users cannot redistribute commercially. | Legal rarely passes use-restricted licenses; the absolute prohibitions bind their customers (Section 3.4 says each entity needs its own license). A few might pay for a commercial license if the win were decisive; it is not on their data (CLAIMS R1). | Impossible: GPL-compatible or permissive only. | Genomics is partly a Prohibited Use by Section 2(c) (population genetic classification, forensic databases): that segment is closed by the author's own terms. HFT firms could pay; nothing in the data says they would notice. | Only through individually negotiated commercial licenses, with a placeholder contact and no price list. |
| **MIT or Apache-2.0** | Takes it. This is the only option under which DuckDB, ClickHouse, Polars, DataFusion, Velox or NumPy could vendor the engine. | Takes it; no reason to pay. | Takes it (Apache-2.0 for Rust and Go norms; MIT or BSD for C++). | Takes it. | None from the code. Revenue is consulting, a job, or a paid support arrangement. Attribution and a paper are the return. |
| **AGPL-3.0 plus a commercial license** (dual) | Cannot take it (AGPL is Category X at Apache; MIT projects will not). | Will not ship AGPL in a SaaS; a few would buy the commercial license if the engine were a clear win on their workload. | Impossible. | Mixed; internal pipelines can run AGPL code without distributing, which removes the lever. | The classic dual-license model. Works only when the product is something customers cannot avoid; a sort kernel is easy to avoid (they already have pdqsort, radix, IPS4o, x86-simd-sort). |
| **BSL 1.1 with a change date** (source-available, converts to Apache-2.0 after N years; MariaDB, HashiCorp 2023, CockroachDB until 2024 used it) | Cannot take it until the change date. | Some would evaluate; the "additional use grant" can permit everything except offering the sort as a competing service, which is a poor fit for a kernel. | Impossible. | Mixed. | Same problem as AGPL: a kernel is not a product people must run as a service. |
| **Split: core engine MIT/Apache, extras (query API, string engine, tuned parallel routes) under a commercial license** | Takes the core; would reimplement the extras rather than license them. | Could take the core, might pay for the extras if they are the winning parts. | Takes the core. | Takes the core. | Some, only if the extras hold the wins. Today the wins (near-sorted, lattice, knot, logs) are in the core engine, so the split would have to move the structured routes out, which is where the value is. |

The honest reading of the table: the data does not support a license-revenue model. The engine wins on structured
inputs by 2-7x at one thread and on everything at 14 threads from 3M up, but every buyer with a legal department
already has a free sort that is level with logos on random data, and the structured wins are on a few percent of
their columns. The outcomes the data supports are (a) a hire or a consulting engagement earned by a public,
reproducible benchmark and a paper, and (b) adoption of specific routes by an engine under a permissive license,
which is recognition rather than revenue. If license revenue is the goal, the thing to sell is not the sort; it is
the analytics/query API (GAPS 12) once it exists and wins on a buyer's workload. QUESTIONS.md question 1 asks.

Whatever is chosen: name the copyright holder, name the project consistently, put a LICENSE at the root and in
every engine directory, set `license` in Cargo.toml, and replace the placeholder contact.

## 3. Patent status and the disclosure clock

Facts found:

- First public disclosure: 2026-05-28 (npm 0.1.0). Its contents define what was disclosed then: the JavaScript
  dispatcher (prescan early exit, counting / radix, flash, coarse, run-merge with galloping, momentum insertion,
  MSD-with-LCP-skip and packed-prefix string radix, argsort, outlier detection), and `c/sort.c`. Confirm against
  `npm pack logos-sort@0.1.0` and `engines/js/CHANGELOG.md`.
- The C flagship's later inventions (disjoint-run relocation J5 2026-09-10, the histogram-free partition with a
  uniformity certificate 2026-09-11, the knot route 2026-09-13, the short-reach route 2026-09-20, the Moebius twist
  and orientation bit 2026-09-21, the string prefix-key sort 2026-10-09, the lattice entry with spills 2026-11-05, the
  prime-factor knot solver 2026-11-05) are in a repository whose GitHub remote is not publicly readable. If nothing
  else was published, they are undisclosed.
- No patent filing is recorded anywhere in the tree.

Clock (verify every date with counsel):

| item | date | note |
|---|---|---|
| US one-year grace period for the inventor's own disclosure (35 U.S.C. 102(b)(1)(A)) on the npm contents | ends **2027-05-28** | 203 days from 2026-11-06. Covers only what the npm package disclosed. |
| Most non-US jurisdictions (EPO, China) | no grace period | Rights on what the npm package disclosed are likely gone abroad; Japan, Korea, Canada, Australia have 12-month grace periods for self-disclosure (verify). |
| The undisclosed C-engine inventions | clock not started | It starts the day the repository is made public, a preprint is posted, a talk is given, or a PR is opened against another project. Decide on filing BEFORE any of those (PAPER.md and the pitch files all presume a public repository). |
| Provisional application | 12 months to convert | A US provisional preserves a date cheaply; it must describe the invention well enough to support the later claims. The FINDINGS and CHANGELOG are unusually complete disclosures. |

What might be claimable (for counsel; the author's judgment on novelty, not a search): the sample-once dispatcher
with per-route certificates (a KS uniformity certificate that licenses a table-free equi-width partition; per-mode
certificates); the histogram-free parallel partition with slack-sized regions, atomic block reservation and
spill-merge at the leaf; run relocation and the knot route (run descriptors with value intervals, placement by memcpy
where intervals are disjoint, merges only on crossings, the orientation bit that consumes descending runs as they
lie); the lattice entry with a divided step and spills; the short-reach route (index blocks sorted in place with
median fences and seam repair); the string sort on 8-byte prefix keys through a numeric radix with constant-byte
skip. Each needs a prior-art search; Section 5 lists the nearest neighbours already known.

Questions for counsel: (1) Does the AI co-authorship on the record affect inventorship (USPTO's February 2024
guidance on AI-assisted inventions requires a natural person's significant contribution to each claim; verify)?
(2) Does the npm disclosure of the JS dispatcher anticipate the C engine's routes that are refinements of the same
ideas? (3) Is a provisional worth filing before the repository goes public, and on which of the items above?
(4) Is anything here assignable to an employer under an existing employment agreement (QUESTIONS.md q7)?

## 4. Copyright, authorship and the license's foundation

A license grants rights under copyright. The US Copyright Office's position (Copyright and Artificial Intelligence,
Part 2, January 2025; verify) is that material generated by an AI system without sufficient human authorship is
not protected, while human selection, arrangement and modification can be. The tree records AI co-authorship in
`package.json` and in the README's address to future Claude instances. The commit history is one human author.

Questions for counsel: (5) Given the record, what parts of the work are protected, and can the Ethical Source
license's Section 2 and 3 restrictions be enforced against a user who copies the code? (6) Should the `author`
field and the README be left as they are (candour) or restated to describe the human authorship of design,
measurement and acceptance decisions? (7) What does the author's employment agreement say about IP created outside
work, and was any employer or university equipment or time used (QUESTIONS.md q6-q7)?

## 5. Prior art: what each neighbour does and what in logos is distinct from it

A literature search is still listed as owed in CLAUDE.md (open direction 0h (d)); nothing below is a novelty opinion.
Licenses from memory; verify before citing.

| prior art | what it does | license / where | nearest logos piece | what differs in logos |
|---|---|---|---|---|
| pdqsort (Peters, 2016; arXiv 2021) | introsort with pattern defeat: partial insertion sort for near-sorted input, equal-key partitions, shuffling on bad pivots | zlib; Rust `sort_unstable` until 1.81, Go 1.19+, Boost | the leaf quicksort's pattern handling | logos detects structure from a sample BEFORE choosing an algorithm; pdqsort adapts inside one algorithm |
| timsort (Peters, 2002) and powersort (Munro & Wild, 2018; CPython 3.11) | natural runs, descending runs reversed in place, galloping merges, merge policy by run length | PSF / Java; powersort in CPython | run detection, the Moebius twist, galloping | logos does not reverse descending runs (orientation bit), places disjoint runs by memcpy without merging (relocation), merges only where value intervals cross (knot route); the report shows 2.1-3.6x over timsort on block shapes |
| IPS4o (Axtmann, Witt, Ferizovic, Sanders; ESA 2017, JEA 2022) and IPS2Ra | in-place parallel super scalar samplesort: sample, splitters, block permutation, equal-key buckets | BSD-2; github.com/ips4o/ips4o | the parallel partition; `LOGOS_PART_INPLACE` IS IPS4o's block permutation over logos's bucket map | logos's partition is histogram-free with slack-sized regions (not in place by default), its splitters come with certificates, and the structured routes run before it |
| Super Scalar Sample Sort (Sanders & Winkel, 2004) | branch-free splitter search via a tree in an array | | `bucket_of`, replaced by the direct-index splitter table (H0b) | a direct-index table in value or key space trialled per thread |
| VQSort (Blacher, Giesen, Kuehne, Sanders, Wassenberg; 2022) | vectorized quicksort in Highway, portable across ISAs | Apache-2.0 (Highway) | the in-house AVX2 quicksort leaf | logos's kernel is AVX2-only and 1.3-1.5x faster than VQSort's AVX2 build on this box (G10); the structural difference is the dispatcher above it |
| x86-simd-sort (Intel) | AVX-512 / AVX2 quicksort, argsort and key-value sort; adopted by NumPy | BSD-3; github.com/intel/x86-simd-sort | the same leaf; the optional `-DLOGOS_XSS_LEAF` links it | on random data logos is level with it (R1); the difference is only the routes |
| fluxsort, crumsort, quadsort, blitsort (van den Hoven) | branchless stable partition sort (flux), in-place unstable (crum), adaptive stable merge (quad) | Unlicense; github.com/scandum | the leaf and the run routes | not benchmarked here (GAPS 7e); quadsort's adaptivity on runs is the closest rival to the run routes that the report lacks |
| glidesort (Peters, 2023); driftsort and ipnsort (Bergdoll, Peters; Rust 1.81 std, 2024) | stable adaptive merge-quicksort hybrids with run detection, small-sort networks, pattern defeat | MIT/Apache-2.0 | run detection, small networks (`net_sort_small`) | not benchmarked here (GAPS 7e); logos's small networks are a 4-16 element Batcher set proven by the 0-1 principle |
| Learned Sort (Kristo, Vaidya, Kraft, Kraska; SIGMOD 2020) | train a CDF model on a sample, bucket by predicted rank, fix up | code on GitHub (MIT, verify) | the uniformity certificate and the equi-width grid; the key grid for heavy tails; the splitter tables | logos refuses the model unless a certificate passes and uses exact sample quantiles otherwise (the ledger records a curvature-placed CDF losing to exact splitters) |
| Flashsort (Neubert, 1998) | classification by linear interpolation into classes, then insertion | public domain article | `flash_sort` in the leaf | used as a leaf kernel; the dispatcher around it is the difference |
| Adaptive sorting theory (Estivill-Castro & Wood, 1992; measures Inv, Runs, Rem, Exc) | sorts optimal with respect to a presortedness measure | | the comparison-bound probe (FINDINGS 3br: H_m, H_r, H_rem) | logos measures the bound per input and reports where it sits on it; no new theory |
| Regions Sort (Obeya, Kahssay, Fan, Shun; SPAA 2019), PARADIS (Cho, Brand, Bordawekar, Finkler, Kulandaisamy, Puri; VLDB 2015), RADULS | parallel in-place radix sorting; speculative permutation with repair | | the strict in-place sort's placement passes (serial first pass) | logos's in-place sort is slower than everything (R5); PARADIS is the known fix (CLAUDE.md 0h (b)) |
| DuckDB's sort (Kuiper, 2021 blog), ClickHouse's pdqsort + radix with miniselect, Arrow row format | radix on normalized fixed-width keys with payload rows; pdqsort for comparison | MIT / Apache-2.0 | the string prefix keys; key-plus-payload (GAPS 4) | buyers, not prior art; but their normalized-key radix is the direct comparator for a key-value panel |

Questions for counsel: (8) Which of the six candidate items in Section 3 survive a search against this table?
(9) Is the Moebius twist (not reversing descending runs) distinct enough from timsort's run handling and from
bidirectional merges in glidesort to claim? (10) Does the "sample once, certify, dispatch" structure read on
Learned Sort or on IPS4o's equal-bucket detection?

## 6. The university question

No reference to a university, course, thesis or advisor exists in the tree, and the git author is a personal
address. Assumption until answered: no university resources were used and no institutional IP policy applies.
QUESTIONS.md q6.

## 7. Actions that need no counsel

1. Name the copyright holder in every LICENSE; add one at the root, in `engines/rust` and `engines/python`; set
   `license` in `Cargo.toml` (or `license-file`).
2. Fix the project name in Section 5(c) or remove the clause.
3. Replace the placeholder commercial contact or remove Section 3.3 until there is one.
4. Decide the license (Section 2) before the repository is made public, a paper is posted, or a pitch is sent;
   decide the patent question (Section 3) before the same events.
