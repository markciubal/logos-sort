# PAPER: venue plan, missing experiments, outline

The state first: there is no paper, no preprint and no public repository, and the one benchmark machine is a
laptop-class hybrid CPU under WSL2 whose calibration line reads loaded. What exists is unusual on the other side:
about 2,000 lines of recorded negative results with predictions graded against measurements, paired A/B
methodology that cancels drift and code layout, a per-shape comparison-bound analysis (FINDINGS 3br), and a set
of structured-data routes that beat the strongest AVX2 vector sort by 2-7x at one thread. A reviewer will accept
the methodology and reject the single machine. Before any submission the patent decision in IP.md Section 3 has to
be made, because a preprint starts the disclosure clock on everything in the C engine.

## 1. Which paper

Three candidates, in the order the data supports them.

| paper | thesis in one sentence | evidence already in the tree | venue fit |
|---|---|---|---|
| P1. Experimental study: "Where does structure detection still beat a vector quicksort on a SIMD CPU?" | A sample-once dispatcher with certificates wins 2-7x on nearly sorted, quantized, low-cardinality and block-structured inputs and is level on random data; twelve structural restructurings of the random case were measured negative, and the per-shape comparison bound explains why. | scale report (13 numeric + 4 string shapes, 8 contenders, 1 and 14 threads, memory per point); FINDINGS 3br (bits per element, ns per bit, growth 1M to 16M); the negative ledger; `make whyfast` (controlled sweeps over order, size, duplicates, distribution, threads); paired A/B with layout cancellation (3r, 3u) | SEA, ALENEX, ESA track B |
| P2. Algorithmic: "Runs as strands: relocation and the knot route" | Natural runs whose value intervals are disjoint are placed by copy, runs that cross are merged only on the crossing; descending runs are consumed as they lie; a parallel form and a measured gate. 2.1-3.6x over timsort at one thread on block inputs, a tie against the engine's own partition at 14 threads. | FINDINGS 3b (J5), 3ac (knot), 3ag/3ah (twist, orientation bit), 3aj (gallop), 3am (strays), 3bc (vector merge), 3bv (prime-factor solver, standalone) | ESA, SEA; SPAA only if the parallel form is made to win (today it ties, 0f) |
| P3. Systems: "Sort less: a lazy sorted view and order queries for analytics" | Partition now, sort a bucket when something reads it; rank, select, quantiles, top-k and range counts without materialising the order; measured against sort-then-look and against engine operators. | `logos_view.h` and `make viewbench` (1.5-2.9x on median, rank, top-k); experiments/paradigm (quantiles 87x, top-k 7x against an AVX-512 sort, old box) | VLDB (PVLDB rolling), ICDE, DaMoN workshop; needs the query API built (GAPS 12) and a comparison to DuckDB/ClickHouse operators |

Recommendation: P1 first. It is the paper the tree already contains; it carries P2's routes as its strongest
section; and it is the document every buyer conversation needs (a public, reproducible, honest benchmark). P3 is a
second paper after the API exists.

## 2. Venues (typical windows; verify each on the venue's call for papers before planning)

| venue | what it is | typical deadline window | typical event | fit for P1 / P2 / P3 |
|---|---|---|---|---|
| SEA (Symposium on Experimental Algorithms) | experimental algorithmics, LIPIcs | around February | June-July | P1 strong, P2 good |
| ALENEX (Algorithm Engineering and Experiments, with SODA) | experimental algorithmics | around July for the following January | January | P1 strong, P2 good; the next reachable cycle is mid-2027 |
| ESA (European Symposium on Algorithms), Track B | algorithm engineering | around April | September | P2 good, P1 good |
| SPAA (Parallelism in Algorithms and Architectures) | parallel algorithms | around January-February | June-July | P2 only if the parallel knot route beats the partition; today it ties (0f) |
| PVLDB / VLDB | database systems; rolling monthly submissions to PVLDB | 1st of each month | the following VLDB | P3 |
| ICDE | database systems | two rounds, around June and October | April-May | P3 |
| DaMoN (Data Management on New Hardware, with SIGMOD) | workshop; hardware-conscious data processing | around March | June | P1's SIMD-versus-structure finding fits as a short paper; a low-cost first publication |
| arXiv preprint | | any time | | starts the disclosure clock (IP.md); post only after the patent decision |

## 3. Experiments the current report lacks, per venue

| gap | why a reviewer asks | needed for | GAPS item |
|---|---|---|---|
| A second machine, ideally two: an AVX-512 server part (the old 20-core box) and an ARM machine (Apple M-series or Graviton) | one laptop under WSL2 with a loaded calibration line is not a result; the hybrid P/E core mix confounds the 14-thread numbers | P1, P2, P3 | 7g, 9 |
| A quiet-box run with the calibration line in the header, and the noise floor (`make pairprobe`) reported beside the ratios | the methodology section promises it; the current report does not show it | P1 | 7c |
| Cold-cache measurement at 1M | engines' blocks arrive cold; the report's rows at 10k and above are LLC-warm repeats of one array | P1, P3 | 7d |
| Stable and adaptive comparison rivals: fluxsort, crumsort, quadsort, glidesort, driftsort/ipnsort, powersort (CPython), Learned Sort | P2's claims are against timsort only; the run-structured competition is quadsort/glidesort/driftsort, and the distribution-aware competition is Learned Sort | P1, P2 | 7e |
| A parallel radix sort and a parallel stable sort as rivals at 14 threads | IPS4o and gnu_parallel are comparison sorts; the 14-thread wins on int_wide and quantized need a radix opponent | P1 | 7e |
| Key-value (payload) and int64 panels | reviewers from the database side ask what the engine does on (key, rowid) pairs | P1, P3 | 4, 5 |
| Scaling beyond 20 hardware threads (a 64-core cloud instance) | the Karp-Flatt analysis (3au) stops at 14 | P1, P2 | hardware |
| Worst-case and adversarial inputs with a stated bound | an adaptive sort's reviewers want its worst input | P1, P2 | 8 |
| A reproducibility package: pinned opponent commits, one build script, a Dockerfile, the shape generators documented as they are in the report's accordion | artifact evaluation at SEA/ESA/VLDB | all | 11 |
| For P3: comparison against engine-native operators (DuckDB `quantile_cont`, ClickHouse `quantileExact`, approximate variants) and nth_element-class baselines, not only sort-then-look | the P3 claim is operator-level | P3 | 12 |

## 4. Outline for P1

1. Introduction. The claim and its limits in the first paragraph: on an AVX2 CPU a vector quicksort is level with
   or better than every structural restructuring on random doubles (table of the twelve negatives); the remaining
   lever is detecting structure before sorting, which wins 2-7x on four families of real input and costs nothing
   measurable when it declines.
2. Method. Verify before timing; one binary, paired halves, alternating order; layout cancellation by copy swap;
   predictions written before measurements; the negative ledger as a first-class result; the noise floor measured
   first (`pairprobe`). This section is the paper's distinct contribution to experimental practice.
3. The dispatcher. One two-stage Weyl sample; certificates (KS uniformity, per-mode, lattice step, cardinality
   by Chao1, inversion ladders at Fibonacci lags); the route order and why it is cheapest-first; the thread policy.
4. The routes, each with its ceiling measurement, its A/B and its guard shapes: counting and lattice (with spills),
   near-sorted, relocation and the knot route (P2's material), short reach, the Moebius twist and the orientation
   bit, run-length, head and tail, the two-halves route at two threads, the string prefix-key sort.
5. The parallel partition: histogram-free with slack-sized regions, direct-index splitter tables, the key grid for
   heavy tails, 12 buckets per thread; where its speedup stops (Karp-Flatt per route).
6. The comparison bound per shape (3br): bits per element, ns per bit, growth from 1M to 16M; where the engine sits
   on the bound and where it goes under it.
7. Results. The scale report's shapes at 1M and 10M, 1 and 14 threads, with memory per element on both sides; the
   seam between batch and repeated measurement; the small end.
8. What did not work and why: the ledger's structural rows (SIMD scatter, huge pages, streaming stores, parallel
   merge, cache-conscious coarse, parallel SIMD leaf, the linked-list and unknotting formulations).
9. Threats to validity: one machine, WSL2, hybrid cores, warm cache at 1M, loaded calibration; what a second machine
   changed (to be filled).
10. Related work: pdqsort, timsort/powersort, IPS4o, VQSort, x86-simd-sort, fluxsort/crumsort/quadsort, glidesort,
    driftsort, Learned Sort, flashsort, adaptive sorting measures.

## 5. What to do first

1. IP decision (IP.md Section 3): file or not, before anything is public.
2. GAPS 7 (report fixes, quiet run, cold mode, new rivals) and a second machine: these are the paper's minimum.
3. Write P1 from FINDINGS and CHANGELOG, which already contain the text; the work is cutting, not writing.
4. Target the nearest SEA or ESA-B window that the second-machine run can meet; a DaMoN short paper is the
   fallback if the machine is late.
