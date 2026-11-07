/* tsbench.cpp — server timestamps with slight knots: the engine, std::sort, and the "Moebius hybrid" as an outside write-up
 * described it (2026-09-22), on the shapes a log collector or a sensor actually produces. Every output is compared bit for bit
 * with a sorted reference before a time counts.
 *
 * THE CLAIM UNDER TEST. The write-up: reverse the small descending "tangles" in one scan (Reidemeister move I), then merge with a
 * boundary check, `if (a[mid] <= a[mid+1])` skip the merge; on data that is "macroscopically ascending but locally noisy" the check
 * proves the halves already in order, the merge phase costs one comparison per chunk, and the sort is O(N). Implemented here
 * exactly so: `hybrid` = a scan that reverses every strictly descending run, then a top-down merge sort (insertion sort below 32)
 * whose merge is skipped when the left half's last element is no larger than the right half's first. Its scratch is allocated
 * once and kept, as the engine keeps its pool. Mode `anatomy` counts what the boundary check actually skips on each shape.
 *
 * THE SHAPES (8.4M events at ~100k events a second, timestamps in microseconds near 1.7e15, so 84 seconds of log):
 *   sorted                 one server, written in send order
 *   jitter                 the log is written in ARRIVAL order and records SEND time: network latency exponential, mean 0.5 ms
 *   jitter + retransmits   the same, and 0.1% of events arrive 200 ms late (a retransmit timeout)
 *   kinks                  sorted, then 1% of adjacent pairs swapped (two events a microsecond apart, logged the wrong way round)
 *   8 servers, clock skew  each event from one of 8 servers whose clocks are off by up to +-20 ms; arrival order by true time
 *   64 batches, overlapping agents flush sorted batches whose time windows overlap the next by 10%, arriving in any order
 *   ring buffer, jitter    the jitter shape dumped from a circular buffer starting at slot 0.37 n
 *   newest first, jitter   the jitter shape read back to front (a log shown newest first)
 *   rocket telemetry       altitude rising ~ t^1.5, sensor noise +-3 ft (hundreds of positions), 0.05% dropouts reading 0.0
 *   ms clocks (ties)       the jitter shape with timestamps truncated to the millisecond: ~84k distinct values
 *   late batch appended    95% in order, then 5% late events from the last tenth of the time range
 *   uniform (CONTROL)      the write-up's own "cryptographic hashes, random arrays"
 *   jitter, SHUFFLED       the jitter shape's values in random order: the same multiset with no order at all
 *   64 blocks, half desc   (round two) the sorted log cut into 64 blocks, shuffled, every other block written descending
 *   64 blocks overlap 50%  (round two) as the batches, but each window overlaps the next by half
 *   8 logs concatenated    (round two) each event goes to one of 8 servers; the 8 sorted server logs are appended one after another
 *
 * THE COLUMNS (medians of paired ratios, order alternated inside every pair, [p10 p90]; above 1 = logos is faster):
 *   vs std::sort    t(std::sort) / t(logos), one thread
 *   vs hybrid       t(hybrid) / t(logos), one thread
 *   knot view 1T    t(logos with every route that reads ORDER switched off) / t(logos), one thread: what the topological routes
 *                   (short reach, knot, kinks, strays, seam, wave, mirror, loop-back, tail, streams, near-sorted, twist, fast
 *                   scan) buy on each shape. Plain relocation and the prescan have no switch and stay on both sides.
 *   knot view PT    the same at P threads.
 *
 * PREDICTIONS, written before the first run.
 *   vs std::sort: sorted 30-100x; jitter 6-15x; retransmits 5-12x; kinks 10-30x; clock skew 5-12x; batches 8-20x; ring 6-15x;
 *     newest first 6-15x; telemetry 5-12x; ms clocks 8-20x; late batch 10-25x; uniform 2.5-4x; shuffled 2.5-4x.
 *   vs hybrid: sorted 1.5-4x; kinks 1.0-3x (the reversal pass repairs an adjacent swap, then every boundary check passes: the
 *     one shape where the write-up's mechanism is the whole story); jitter, retransmits, clock skew, telemetry 5-12x (latency
 *     spreads every boundary, so no check passes above the jitter's scale and every level merges in full); batches 4-10x;
 *     ring 8-20x; newest first 8-15x; ms clocks 10-30x; late batch 3-8x; uniform and shuffled 4-8x.
 *   knot view 1T: sorted 1.00; jitter 1.9-2.5; retransmits 1.5-2.2; kinks 4-6; clock skew 1.5-2.5; batches 2.5-4; ring 2.0-2.5;
 *     newest first 1.8-2.2; telemetry 1.5-2.2; ms clocks 1.00 (the lattice counts them, order unread); late batch 3-4.5;
 *     uniform, shuffled 1.00.
 *   knot view PT: jitter 1.3-1.5; retransmits 1.1-1.4; kinks 1.2-1.5; clock skew 1.2-1.5; batches 1.1-1.2; ring 1.0-1.3;
 *     newest first 1.0-1.4; telemetry 1.2-1.4; late batch 1.1-1.2; the rest 1.00.
 *   anatomy: the boundary check passes on nearly every merge of sorted and kinks, and on almost none above the block size of the
 *     jitter on the latency shapes, so the hybrid's merged elements per n read about log2(n) - log2(32) = 18 there and about 0 on
 *     sorted and kinks.
 *
 * MODE `fixes`, after the first two runs found two losses (the kinks row 0.11 against the hybrid and 0.90 against std::sort; the ring
 * buffer 0.74 at P threads with the order routes on). PREDICTIONS, written before they ran:
 *   ONERUN (LOGOS_NO_ONERUN=1 against shipped): kinks 5-8x at one thread (the leaf's full sort replaced by the detector's scan, the
 *     strays' sort and one merge), 1.00 at P threads (there the strand arrives in pieces and the knot route already had it);
 *     sorted, jitter, batches, late batch, uniform 0.98-1.02.
 *   SEAM at P threads (LOGOS_NO_SEAM=1 against shipped): ring buffer 1.25-1.40 at P (the block phase no longer run and thrown away;
 *     the order-routes-off control read 1/0.74 = 1.35), 1.10-1.20 at one thread (the seam's rotation, as before); jitter,
 *     retransmits, clock skew, uniform 0.98-1.02.
 *
 * MEASURED (2026-09-22, 8.4M, 20 pairs, three runs of `all`; `fixes` 40 pairs, two runs). ANATOMY, exactly as predicted: the boundary
 * check skipped 100% of the 262,143 merges on sorted and kinks, 99.7% on batches (which still merged 7.8 elements per element: the
 * few merges that fail are the top ones), 95% on the late batch, and 0% on every shape with latency in it, where the hybrid merged
 * 18.00 elements per element: a full merge sort. Jitter reaches ~50 positions, wider than the 32-element blocks the recursion bottoms
 * out at, so no two adjacent halves are ever in order. `all`, before the two fixes (runs 1 and 2) / after (run 3):
 *   shape                  vs std::sort        vs hybrid          knot view 1T       knot view PT
 *   sorted                 9.74 9.55 9.73      1.19 1.12 1.12     1.01 1.01 1.00     1.00 1.00 1.00
 *   jitter                 4.67 5.03 4.46      4.87 5.00 4.44     1.63 1.68 1.56     1.36 1.37 1.34
 *   + retransmits          3.41 3.47 3.25      3.28 3.26 3.02     1.21 1.18 1.13     0.98 1.01 0.98
 *   kinks                  0.90 0.90 4.95      0.11 0.11 0.61     0.93 0.93 5.07     1.42 1.40 1.42
 *   clock skew             5.97 5.94 5.24      5.75 5.87 5.05     1.56 1.57 1.39     1.32 1.32 1.34
 *   batches                16.2 16.0 15.7      3.79 3.45 3.54     5.63 5.60 5.63     1.75 1.74 1.74
 *   ring buffer            4.17 4.26 3.87      4.14 4.20 3.89     1.52 1.52 1.43     0.75 0.74 0.94
 *   newest first           4.24 4.28 3.95      4.82 4.94 4.53     1.55 1.59 1.46     0.99 1.02 0.99
 *   rocket telemetry       5.32 5.32 4.84      5.34 5.33 4.95     1.92 1.87 1.64     0.96 0.97 0.96
 *   ms clocks              1.66 1.65 1.62      1.58 1.59 1.57     1.00 1.01 1.00     0.99 0.99 1.00
 *   late batch             6.42 6.57 6.40      2.17 2.20 2.15     5.26 5.30 5.24     1.19 1.18 1.21
 *   uniform                6.29 6.26 6.32      7.80 7.77 7.60     1.00 0.99 0.99     1.06 0.98 0.99
 *   shuffled               6.80 6.86 6.51      8.37 8.60 7.98     1.00 1.00 1.00     1.01 1.03 1.01
 * `fixes`: ONERUN kinks 5.47 / 5.45 at one thread, 1.03 / 1.01 at P; guards 0.99-1.01. SEAM ring buffer 1.11 / 1.13 at one thread and
 * 1.34 / 1.30 at P; guards 0.99-1.02. Both SHIPPED.
 * GRADED. Anatomy right. Against std::sort LOW almost everywhere a structure was predicted to pay (sorted 9.7 against 30-100, the
 * latency shapes 3.3-6.0 against 5-15: introsort is itself fast on nearly sorted input) and HIGH on uniform and shuffled (6.3-6.9
 * against 2.5-4: the AVX2 leaf); WRONG on kinks (0.90) and on ms clocks (1.6 against 8-20: no counting route takes 84k distinct
 * values). Against the hybrid LOW on the structured shapes (3-6 against 5-12; ring and newest first 4-5 against 8-20) and WRONG on
 * kinks, where the hybrid wins: 9x before the one-strand fix, 1.6x after it. Knot view LOW on the latency shapes at one thread
 * (1.4-1.7 against 1.8-2.5), HIGH on batches and the late batch (5.2-5.6 against 2.5-4.5), WRONG on kinks until the fix, and WRONG
 * at P threads on retransmits, telemetry (the reach route's probes see strays and decline: 0.96-1.01) and the ring buffer (0.74
 * until the fix).
 *
 * ROUND TWO (2026-09-22): THE EDGE SORT, as proposed. (1) Condense: scan for natural runs and reverse every strictly descending
 * one in place (the Moebius twist), leaving K ascending blocks. (2) Edge sort: treat each block as a black box with a min edge (its
 * first element) and a max edge (its last) and sort the K blocks by min edge, O(K log K), claimed trivial because K is much smaller
 * than N. (3) Physically rearrange the blocks in that order, claimed to forge "an ascending sine wave": a macroscopic ascent with
 * inversions only where one block's range overlaps the next. (4) Run the hybrid above on it, the shape it is said to thrive on.
 * Implemented exactly so as `edge`. The block list keeps its capacity between calls, and when the min edges already ascend in array
 * order the sort and the rearrangement are skipped (a generous reading: nobody copies an array onto itself). `edge+trim` is the
 * strongest form of the same idea this bench can write: the same blocks in the same order, merged bottom-up with their neighbours,
 * each merge skipped when the edges say the pair is disjoint and otherwise cut down by two binary searches to the part that overlaps
 * (Timsort's trimming). Mode `edgeanat` measures the claims one by one on a single run: K and the mean block length, the edge sort's
 * share of the pipeline, how many neighbouring blocks overlap after the rearrangement, how far elements still are from their sorted
 * place (mean, max, and the mean of log2(1 + d): the bits per element any finish must still spend), and what the hybrid and the
 * trimmed merge then do. A descending run is taken NON-strictly (a tie continues it): a first smoke run at 100k took them strictly,
 * as the hybrid's own reversal does, and the ~1.25% ties in these timestamps (two events in one quarter microsecond) cut every
 * descending block into ~80-element pieces. The predictions below were written before that smoke run. Three shapes join the table for it: 64 sorted blocks shuffled with every other one descending (the
 * proposal's own best case), 64 blocks overlapping their neighbours by half, and 8 server logs concatenated (every block spans the
 * whole range).
 *
 * PREDICTIONS for round two, written before its first run (1 thread, 8.4M; the proposal is sequential, so no P column).
 *   logos vs edge, t(edge) / t(logos): sorted 3-8; the latency shapes (jitter, retransmits, skew, ring, newest, telemetry) 5-10;
 *     ms clocks 1.8-3; kinks 1.2-2.0; batches 3-6; late batch 3-6; blocks 2-4; stairs 2-4; concat 1.0-2.5; uniform, shuffled 8-13.
 *   logos vs edge+trim: sorted 0.9-1.5; latency shapes 6-15 (blocks of two or three leave ~22 levels of merging and no insertion
 *     base); ms clocks 2-4; kinks 0.7-1.3 (each merge moves two elements); batches 1.1-1.6; late batch 1.5-4; blocks 1.0-1.4;
 *     stairs 1.2-2.0; concat 0.8-1.6; uniform and shuffled 10-20.
 *   edge vs hybrid, t(hybrid) / t(edge), above 1 = steps 1-3 help the hybrid: blocks 1.3-2.5, the one shape where the rearranged
 *     array is sorted; batches and stairs 0.85-1.15; everything else 0.6-0.95 (the steps add passes and remove no merges).
 *   edge+trim vs std::sort, t(std::sort) / t(edge+trim): sorted 5-15; kinks 5-15; batches 5-12; blocks 5-12; stairs 3-8; late
 *     batch 2-6; concat 1.5-4; ms clocks 0.5-1.0; latency shapes 0.4-0.8; uniform and shuffled 0.3-0.6.
 *   edgeanat: mean block length 2-3 on the latency shapes, uniform and shuffled (K is 35-50% of N, not "drastically smaller"),
 *     3-20 on ms clocks, 90-110 on kinks, 40-60 on the late batch, n/64-n/40 on the three block shapes and batches, n/8 on concat.
 *     The edge sort 10-35% of the pipeline where blocks average 2-3, under 1% where they average 100 or more. Neighbouring blocks
 *     overlapping after the rearrangement: 0% on blocks and sorted, 100% on kinks, batches, stairs and concat, 85-100% on the
 *     latency shapes. Mean displacement afterwards 0 on blocks, 5-50 positions on the latency shapes (2-5 bits), n/10-n/4 on
 *     uniform. The hybrid's merged/n afterwards 15-18 on the latency shapes (a full merge sort here is 18), 5-7 on batches and
 *     stairs, 3 on concat, 0 on blocks and kinks.
 *   Stop rule: two runs of 16 pairs. The proposal wins a shape if column 1 or 2 reads under 1.0 in both runs; a row under 0.9 in
 *     column 2 is an engine defect to trace, as the kinks row was. Nothing from the proposal ships unless it beats the engine's own
 *     route on some shape by 1.1 in both runs with every guard intact.
 *
 * ROUND TWO MEASURED (2026-09-22, 8.4M, one thread, two runs of 16 pairs, run 1 / run 2). Every output checked.
 *   shape                          logos vs edge   logos vs edge+trim   edge vs hybrid   edge+trim vs std::sort
 *   sorted                          1.54 / 1.55      0.45 / 0.44          0.72 / 0.72      22.3 / 22.5
 *   jitter                          7.80 / 7.83      7.32 / 7.19          0.64 / 0.63      0.70 / 0.69
 *   + retransmits                   5.91 / 5.91      5.39 / 5.42          0.60 / 0.59      0.69 / 0.68
 *   kinks                           0.89 / 0.89      0.59 / 0.60          0.68 / 0.70      8.40 / 8.40
 *   clock skew                      9.25 / 7.27      8.47 / 7.02          0.66 / 0.68      0.72 / 0.73
 *   batches                         4.18 / 4.30      2.01 / 1.94          0.89 / 0.90      8.35 / 8.48
 *   ring buffer                     7.12 / 4.26      6.69 / 4.04          0.67 / 0.64      0.69 / 0.70
 *   newest first                    7.44 / 4.55      6.81 / 4.07          0.73 / 0.72      0.68 / 0.68
 *   rocket telemetry                7.66 / 6.78      7.14 / 6.09          0.71 / 0.71      0.75 / 0.75
 *   ms clocks                       2.47 / 2.45      2.02 / 2.07          0.63 / 0.62      0.82 / 0.81
 *   late batch                      2.83 / 2.83      2.97 / 3.09          0.76 / 0.75      2.24 / 2.23
 *   uniform                         8.52 / 8.84      8.72 / 8.87          0.86 / 0.85      0.67 / 0.68
 *   shuffled                        9.87 / 9.60      9.96 / 9.69          0.89 / 0.87      0.68 / 0.69
 *   64 blocks, half desc            0.76 / 0.76      0.41 / 0.40          1.26 / 1.23      8.57 / 8.34
 *   64 blocks overlapping 50%       2.56 / 2.46      1.22 / 1.26          0.87 / 0.90      11.2 / 11.2
 *   8 logs concatenated             1.44 / 1.44      1.25 / 1.27          0.89 / 0.89      2.24 / 2.23
 * edgeanat (one run): K is 41% of N on every shape with noise in it (blocks of 2.4), 3.46M blocks at 8.4M; the edge sort is 15-34%
 * of the pipeline there (47.5% on newest first); after the rearrangement neighbouring blocks overlap 78-100% on every shape but
 * sorted, and elements still sit 38 positions from their place on jitter, 649 on clock skew, 6,608 on telemetry (each dropout's
 * reversed pair carries a real reading to the front), 1.25M on the 64 shuffled blocks (a maximal run swallows the next block's
 * first element, and its min edge then places the whole block wrong) and n/4 on uniform; the hybrid still merges every element 18
 * times on every latency shape.
 * GRADED. Anatomy: right on block lengths, the edge sort's share, the overlaps on noisy shapes, jitter's displacement and the
 * hybrid's full merge; WRONG on everything the proposal claims for blocks (0 displacement and 0 merges predicted; 1.25M and 6.47
 * measured) and on skew and telemetry's displacement (predicted 5-50). Timing: the edge sort 1.4-9.9x behind the engine on 13 of 16
 * shapes, inside the predictions on the latency shapes and the controls; it makes the hybrid SLOWER on 15 of 16 (0.59-0.90). WRONG,
 * all against the engine: sorted (edge+trim 2.2x ahead, predicted a tie), kinks (1.7x ahead, predicted 0.7-1.3) and the tied
 * blocks (2.5x ahead, predicted the engine 1.0-1.4x ahead). Three engine defects, each traced and fixed below.
 * AFTER THE FIXES (the same modes, two runs of 16 pairs): logos vs edge+trim on sorted 0.94-0.95 (was 0.44), on kinks 1.39-1.43 (was 0.59-0.60), on the tied
 * blocks 2.73-2.79 (was 0.40-0.41); kinks against the hybrid 1.61-1.67 (was 0.61); against the edge sort as written: sorted 3.75-3.86 (was 1.55), kinks 2.26-2.34 (was 0.89), the tied blocks 4.96-5.06 (was 0.76); kinks against std::sort 11.1-11.2 (was 4.95).
 *
 * THE PRESCAN IN BLOCKS (found by round two's sorted row: the engine took 2.2x one plain scan on a sorted array, where the whole sort
 * IS the dispatcher's prescan). Mode `prescan`: LOGOS_NO_FASTPRESCAN=1 (the scalar line) against the blocks, same binary, 1 and P
 * threads. PREDICTIONS, written before the first run: sorted 2.0-3.5x at both thread counts (the prescan is serial and is the whole
 * sort); sorted then reversed 1.3-2.0x (the reversal pass stays); the late batch, whose prescan walks the 95% prefix, 1.05-1.25 at one
 * thread and 1.1-1.4 at P (the tail route's parallel form leaves the serial prescan a larger share); kinks, batches, jitter and
 * uniform 0.98-1.02 (their prescan stops within the first run). Stop rule: two runs of 40 pairs; ship if sorted clears 1.5 in both
 * and no guard reads under 0.97 in either.
 * MEASURED (run 1 / run 2): sorted 2.21 / 2.22 at one thread, 2.23 / 2.30 at P; sorted then reversed 1.41 / 1.40, 1.41 / 1.41; the
 * late batch 1.31 / 1.30, 1.43 / 1.44; kinks, batches, jitter, uniform 0.99-1.06. GRADED: sorted and reversed right; the late batch
 * ABOVE (1.30 against 1.05-1.25, 1.44 against 1.1-1.4): the prefix walk was a larger share of the tail route than assumed. SHIPPED.
 *
 * TIES IN A DESCENDING RUN (found by round two's block row: the engine 2.4x behind the trimmed edge sort, because ~1.25% ties cut
 * every descending block into ~80-element pieces under the relocation floor). Mode `ties`: LOGOS_DESC_STRICT=1 (the old strict test)
 * against ties continuing a descending run, same binary, 1 and P threads. PREDICTIONS, written before the first run: 64 blocks half
 * descending 3-8x at one thread (a full leaf sort becomes relocation) and 1.5-4x at P; the 50% staircase and the batches (every block
 * ascending) 0.98-1.02; sorted, kinks, jitter and uniform 0.98-1.02. Stop rule: two runs of 40 pairs; ship if the block row clears 2
 * at one thread in both and no guard reads under 0.97 in either.
 * MEASURED: the tied blocks 6.00 / 6.10 at one thread, 1.32 / 1.35 at P; guards 0.98-1.03, except ONE P-thread row per run under the
 * rule (batches 0.91 in run 1, stairs 0.95 in run 2, each 0.99-1.02 in the other run). Neither shape has a descending run, so the
 * changed lines never execute there; at 120 pairs the switch read 0.99-1.01 on both and a switch nobody reads (mode `switch`,
 * TS_SWITCH=LOGOS_NULL_NOTHING) 0.99-1.00: noise. GRADED: one thread right, P threads LOW (1.3 against 1.5-4). SHIPPED.
 *
 * ONE STRAND, STRAYS PUT BACK IN PLACE (found by round two's kinks row: the engine 1.7x behind the trimmed edge sort; timed by phase,
 * the finish was 20.7 of 34 ms, three full passes for 820 strays one or two positions from home). Mode `onefix`: LOGOS_ONERUN_COPY=1
 * (the three passes) against one memmove per segment, same binary, 1 and P threads. PREDICTIONS, written before the first run: kinks
 * 1.8-2.4x at one thread (the detector's 13-14 ms stays); one strand with 0.1% strays scattered across the whole range 1.3-2.0x (they
 * travel far, so the memmoves cover most of the array once); both 1.00 at P threads (strays are lifted at one thread only); sorted,
 * blocks, jitter and uniform 0.98-1.02. Stop rule: two runs of 40 pairs; ship if kinks clears 1.5 at one thread in both and no guard
 * reads under 0.97 in either.
 * MEASURED: kinks 2.36 / 2.37 at one thread, 1.02 / 1.05 at P; far strays 1.78 / 1.74, 1.00 / 1.02; guards 0.98-1.06. GRADED: right on
 * both. SHIPPED.
 *
 * Mode `switch`: TS_SWITCH=<NAME> ./tsbench 120 8388608 switch <filter> pairs that one switch against the shipped engine on every
 * shape the filter names. With a NAME the engine never reads (TS_SWITCH=LOGOS_NULL_NOTHING) both sides run the same code and the
 * column is this box's floor for the row: the check behind a guard that dips once.
 *
 * ROUND THREE (2026-09-22): THE TRIPLE-REVERSAL WELD, as proposed -- condense into natural runs, meta-sort them by min edge,
 * permute them IN PLACE by triple reversals ((A^R B^R)^R = BA), then weld the seams left to right behind an O(1) boundary check.
 * LOST, and the implementation was REMOVED on 2026-09-23 (it is in git history before that commit; the verdict is in the ledger
 * and FINDINGS 3ar). The numbers, 1,048,576 elements, one thread: 2 to 4,600x behind the engine on 14 of 16 shapes (latency
 * shapes 11-32x, a late 5% batch 390-465x, uniform 4,079-4,639x), 2.6-368x at 14 threads, and the O(1) check bypassed 0.0% of
 * seams on EVERY shape. Why: a maximal run swallows the next block wherever it starts higher (K = 32 on 64 staircase blocks), so
 * units span holes and the prefix maximum reaches the top of the range at once; the test then always fails and the merge behind
 * it is O(prefix), i.e. quadratic. The rotations themselves are correct and cost 1.2-3x against a gather on the block shapes.
 * What it left behind is below: it lost on two rows so narrowly that tracing them found two engine defects, both shipped.
 * TRACED (the stop rule's two rows; scratchpad probe, paired, one thread, 40 pairs, 1M and 8.4M):
 *   sorted: the engine's whole sort is prescan_blocks (a copy of it reads 1.00-1.03 against the engine). A plain branchy scan is
 *     1.03-1.07x faster, SSE2 compares 1.46-1.69x, AVX2 compares 1.70-2.07x: the block loop compiled to sixteen scalar comisd /
 *     seta / or steps with a serial OR chain. Its comment said "a loop the compiler can vectorize"; at the engine's flags (no
 *     -march) it never was.
 *   kinks: a plain insertion sort is 1.46-1.64x faster than the engine. The trace build puts the relocation detector's scan at 89%
 *     of the sort (1.12 of 1.26 ms: 10,050 kinks swapped back, 100 strays, one strand). After a block of eight fails, the scan runs
 *     EIGHT scalar steps, each with a gap-test call, before it tries a block again: a kink every ~100 elements pays that each time.
 * THE TWO FIXES, predictions written before either is built (mode `switch`, 40 pairs, 8.4M and 1M, two runs each):
 *   A. The prescan with vector compares: AVX2 blocks of 32 where the CPU has AVX2 and FMA, SSE2 blocks of 16 otherwise (baseline
 *      x86-64, no dispatch), the scalar OR of sixteen elsewhere. Same ordered compares, so a NaN still answers both tests false.
 *      LOGOS_PRESCAN_OR16=1 = the old block, LOGOS_PRESCAN_SSE2=1 = SSE2 on an AVX2 CPU. Sorted: 1.6-2.0x over the old block at 1
 *      and P threads (the prescan is sequential either way); SSE2 1.4-1.7x. Late batch (the prescan reads 95% before the tail
 *      route) 1.03-1.10x at one thread. Everything else stops within a few elements: 0.98-1.02. make callbench: nothing (256 up).
 *   B. The detector after a DIRECTION break: when a block of eight fails only because one step goes the wrong way, the steps before
 *      it are proven by the same bound (a prefix of the block has a smaller largest step, a smaller count and a range no smaller),
 *      so the scan jumps to the break and tries a block again after two scalar steps; a block that fails the gap bound keeps the
 *      eight. The run list is identical by construction, as before; so is the stray bookkeeping, once a fast advance also clears
 *      the one-step "do not look back" flag a lifted stray sets (it could land eight elements late). LOGOS_FASTSCAN_HOLD8=1 = the
 *      old hold. Kinks 1.08-1.20x at one thread, nothing at P (the near-sorted route takes them there); the block shapes, whose
 *      breaks are block ends, 0.98-1.03; guards 0.98-1.02. The harness compares the run list's kink count and twisted strands
 *      across the shipped scan, the scalar scan and the old hold.
 *   Stop rule: each ships only if its target rows clear 1.05 in both runs and no guard falls below 0.97 in both.
 * THE TWO FIXES MEASURED (t(switch set) / t(shipped), 40 pairs, run 1 / run 2; above 1 = the fix is faster):
 *   A. LOGOS_PRESCAN_OR16, 8.4M: sorted 1.81 / 1.72 at one thread, 1.79 / 1.67 at 14; 1M: 2.22 / 2.17 and 2.23 / 2.06; late batch
 *      1.13 / 1.09 and 1.13 / 1.18; the fourteen other shapes 0.98-1.03 in both runs. LOGOS_PRESCAN_SSE2 (AVX2 against SSE2): sorted
 *      1.23 / 1.27 and 1.24 / 1.23, late batch 1.04 / 1.06 and 1.07 / 1.05. GRADED: sorted RIGHT at 8.4M (1.6-2.0), ABOVE at 1M (the
 *      array sits in the last-level cache there and the compare chain was all of the cost); SSE2 over the old block works out at
 *      1.35-1.47, the bottom of 1.4-1.7; late batch a little ABOVE (1.09-1.18 against 1.03-1.10). SHIPPED.
 *   B. LOGOS_FASTSCAN_HOLD8: kinks 1.08 / 1.05 at one thread at 8.4M and 1.08 / 1.06 at 1M; 1.06 [1.00 1.13] at 120 pairs beside a
 *      null switch that read 1.00 [0.93 1.07]; 14 threads 0.97-1.02 (the near-sorted route takes kinks there); the fifteen other
 *      shapes 0.97-1.05. GRADED: at the bottom edge of 1.08-1.20. SHIPPED (it clears 1.05 on all four rows and the mechanism is
 *      plain). After both, the weld against the engine (16 pairs, 1M): sorted 1.93 (was 0.88-0.92), kinks 0.94 (was
 *      0.87). The kinks row is under the 1.1x the stop rule traces; a plain insertion sort still does it 1.46-1.64x faster than the
 *      engine did before the fix (open direction 0m (a5)).
 *
 * ROUND FOUR (2026-09-22): THE LOGOS-MOEBIUS HYBRID, as handed off -- scan for runs; reverse a descending run in place, except
 * one of 2 or 3, swapped inline (Reidemeister I); sort any run under 16 with a fallback; stitch with a galloping merge behind an
 * O(1) boundary check. Built twice, literal and repaired, and both LOST; the implementation was REMOVED on 2026-09-23 (git
 * history before that commit; the verdict is in the ledger and FINDINGS 3as). Behind the engine on 14 of 16 shapes literal and
 * 13 of 16 repaired (block shapes 1.7-2.8x, jitter 5.9-7.0x, a late batch 502-543x, uniform 4,759-7,342x, a ring buffer and a
 * newest-first log 6,382-16,213x). The specified micro-kink branch FIRES 0 TIMES on the shape it was written for (1% adjacent
 * swaps): a dip's first element belongs to the PRECEDING ascending run, so the descending run is one element, never 2 or 3 --
 * which is why the engine's own repair asks BOTH neighbours instead (`LOGOS_NO_KINK`, shipped, 5.8x/5.0x/3.2x, and untouched by
 * this removal). Merge ORDER decided the rest: the same runs and kernel cost 100,999 merged elements per element into one growing
 * prefix and 4.6 bottom-up. It too found one engine defect, shipped, recorded below.
 * THE FIX IT FOUND (predictions written before the first timed run; `TS_SWITCH=LOGOS_SCANBLOCK_SCALAR ./tsbench 40 8388608 switch`,
 * t(the scalar block test) / t(shipped), above 1 = the vector test is faster): the run detector's block of eight compiled to thirteen
 * scalar subsd / maxsd / comisd (read from the disassembly, as the prescan's did), so the eight steps are now computed two at a time
 * with SSE2 or four at a time with AVX2, and the helper advances over as many blocks as pass, one call per run rather than per block.
 *   kinks (1% adjacent swaps) 1.15-1.35 at one thread; the block shapes 1.10-1.25 at one thread and 1.05-1.15 at 14; the latency
 *   shapes 1.00-1.05 (runs of 2.4 elements never reach the block path's 16-element body); sorted and the controls 1.00.
 *   Stop rule: ship if kinks or a block shape clears 1.05 in both runs and no guard reads under 0.97 in both.
 * THE FIX MEASURED (t(scalar block test) / t(shipped), 40 pairs at 8.4M, run 1 / run 2): kinks 1.32 / 1.30 at one thread and
 *   1.09 / 1.04 at 14; 64 blocks half descending 1.27 / 1.30 and 1.16 / 1.09; 64 batches 1.12 / 1.06 and 1.04 / 1.06; 64 blocks
 *   overlapping 1.07 / 1.07 and 1.05 / 1.04; the twelve other shapes 0.97-1.02 in both runs. GRADED: kinks RIGHT (1.15-1.35),
 *   the block shapes RIGHT at both thread counts, the latency shapes and the controls RIGHT (1.00). SHIPPED. After it, the
 *   pipeline's kinks row reads 1.04 (literal, was 0.79-0.83) and 1.20 (repaired, was 0.89-0.92).
 *   Still ahead of the engine, and traced next: 8 concatenated logs, 0.80-0.86 over three runs literal (The trace names two causes (`./tstrace 1 1048576 once concat`): the knot route declines at its few-runs gate (8 runs, each spanning the range, crossing cost 3,030 = 3.0n against a gate of 1.0n, which is open direction 0m (h), where lifting it to 3.0n measured 1.6-2.9x on shapes of this kind), and then the SEAM route MISFIRES: it reads the eight log boundaries as a noisy ring buffer's single drop, rotates the whole array and re-enters, after which the leaf sorts it anyway (18.2 ms of an 18.3 ms sort). Two levers: that gate, with its own A/B against interleaved runs, and the seam gate's false positive.).
 *
 * usage: ./tsbench [pairs] [n] [all | anatomy | fixes | once | edge | edgeanat | prescan | ties | onefix | switch] [row filter]     (once: one logos sort per shape, for make tstrace)
 */
#include "logos_psort.h"
#include "bench_common.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <thread>
#include <atomic>

static uint64_t RS = 88172645463325252ull;
static inline uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static inline double fr() { return (double)(nr() >> 11) / (double)(1ull << 53); }
static double now_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---------------------------------------------------------------- the hybrid, exactly as described */
static double *g_hbuf = nullptr; static size_t g_hcap = 0;
static size_t g_bypass = 0, g_merges = 0, g_merged = 0, g_reversed = 0;
static void reverse_tangles(double *a, size_t n) {                  /* every strictly descending run, reversed where it lies */
    size_t i = 0;
    while (i + 1 < n) {
        if (a[i + 1] < a[i]) { size_t j = i + 1; while (j + 1 < n && a[j + 1] < a[j]) j++;
            for (size_t l = i, r = j; l < r; l++, r--) { double t = a[l]; a[l] = a[r]; a[r] = t; }
            g_reversed += j - i + 1; i = j + 1; }
        else i++;
    }
}
static void hyb_rec(double *a, size_t lo, size_t hi) {
    size_t n = hi - lo;
    if (n <= 32) { for (size_t i = lo + 1; i < hi; i++) { double v = a[i]; size_t j = i; while (j > lo && a[j - 1] > v) { a[j] = a[j - 1]; j--; } a[j] = v; } return; }
    size_t mid = lo + n / 2;
    hyb_rec(a, lo, mid); hyb_rec(a, mid, hi);
    g_merges++;
    if (a[mid - 1] <= a[mid]) { g_bypass++; return; }                /* the O(1) boundary check: the halves already in order */
    size_t nl = mid - lo; std::memcpy(g_hbuf, a + lo, nl * sizeof(double));
    size_t i = 0, j = mid, o = lo;
    while (i < nl && j < hi) { if (a[j] < g_hbuf[i]) a[o++] = a[j++]; else a[o++] = g_hbuf[i++]; }
    while (i < nl) a[o++] = g_hbuf[i++];
    g_merged += n;
}
static void hybrid_sort(double *a, size_t n) {
    if (n < 2) return;
    if (g_hcap < n) { std::free(g_hbuf); g_hbuf = (double *)std::malloc(n * sizeof(double)); g_hcap = g_hbuf ? n : 0; if (!g_hbuf) { std::sort(a, a + n); return; } }
    reverse_tangles(a, n);
    hyb_rec(a, 0, n);
}

/* ---------------------------------------------------------------- round two: the edge sort, exactly as proposed, and its trimmed form */
struct blk_t { double mn; size_t lo, len; };
static std::vector<blk_t> g_blk;
static std::vector<size_t> g_bd, g_bd2;
static double *g_ebuf = nullptr; static size_t g_ecap = 0;
static size_t g_tbypass = 0, g_tmerges = 0, g_tmoved = 0; static int g_rearranged = 0;
static int g_phase_on = 0; static double g_ph[4] = { 0, 0, 0, 0 };   /* edgeanat, one run: condense, edge sort, rearrange, finish (ms) */
static int edge_bufs(size_t n) {
    if (g_hcap < n) { std::free(g_hbuf); g_hbuf = (double *)std::malloc(n * sizeof(double)); g_hcap = g_hbuf ? n : 0; }
    if (g_ecap < n) { std::free(g_ebuf); g_ebuf = (double *)std::malloc(n * sizeof(double)); g_ecap = g_ebuf ? n : 0; }
    return g_hbuf && g_ebuf;
}
/* steps 1-3: condense into ascending blocks (a strictly descending run is twisted where it lies), sort the blocks by their min edge,
   rearrange them in memory in that order */
static void edge_arrange(double *a, size_t n) {
    double t0 = g_phase_on ? now_ms() : 0;
    g_blk.clear();
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        if (j < n && a[j] < a[i]) { while (j < n && a[j] <= a[j - 1]) j++; std::reverse(a + i, a + j); }   /* a tie continues it */
        else while (j < n && a[j] >= a[j - 1]) j++;
        g_blk.push_back({ a[i], i, j - i }); i = j;
    }
    double t1 = g_phase_on ? now_ms() : 0;
    auto by_min = [](const blk_t &x, const blk_t &y) { return x.mn < y.mn; };
    g_rearranged = !std::is_sorted(g_blk.begin(), g_blk.end(), by_min);
    if (g_rearranged) std::sort(g_blk.begin(), g_blk.end(), by_min);
    double t2 = g_phase_on ? now_ms() : 0;
    if (g_rearranged) {
        size_t o = 0;
        for (auto &b : g_blk) { std::memcpy(g_ebuf + o, a + b.lo, b.len * sizeof(double)); b.lo = o; o += b.len; }
        std::memcpy(a, g_ebuf, n * sizeof(double));
    }
    if (g_phase_on) { double t3 = now_ms(); g_ph[0] = t1 - t0; g_ph[1] = t2 - t1; g_ph[2] = t3 - t2; }
}
/* two neighbouring sorted blocks: skipped when the edges say disjoint, else only the overlapping middle is merged */
static void trim_merge(double *a, size_t lo, size_t mid, size_t hi) {
    g_tmerges++;
    if (a[mid - 1] <= a[mid]) { g_tbypass++; return; }
    size_t l = (size_t)(std::upper_bound(a + lo, a + mid, a[mid]) - a);       /* left elements <= the right block's min stay */
    size_t r = (size_t)(std::lower_bound(a + mid, a + hi, a[mid - 1]) - a);   /* right elements >= the left block's max stay */
    size_t nl = mid - l; std::memcpy(g_hbuf, a + l, nl * sizeof(double));
    size_t i = 0, j = mid, o = l;
    while (i < nl && j < r) { if (a[j] < g_hbuf[i]) a[o++] = a[j++]; else a[o++] = g_hbuf[i++]; }
    while (i < nl) a[o++] = g_hbuf[i++];
    g_tmoved += r - l;
}
static void edge_trim_finish(double *a, size_t n) {               /* the blocks lie in array order either way: merge neighbours bottom-up */
    g_bd.clear(); for (auto &b : g_blk) g_bd.push_back(b.lo); g_bd.push_back(n);
    while (g_bd.size() > 2) {
        size_t K = g_bd.size() - 1; g_bd2.clear();
        for (size_t k = 0; k + 1 < K; k += 2) { trim_merge(a, g_bd[k], g_bd[k + 1], g_bd[k + 2]); g_bd2.push_back(g_bd[k]); }
        if (K & 1) g_bd2.push_back(g_bd[K - 1]);
        g_bd2.push_back(n); g_bd.swap(g_bd2);
    }
}
static void edge_finish(double *a, size_t n, int trim) {
    double t0 = g_phase_on ? now_ms() : 0;
    if (trim) edge_trim_finish(a, n); else { reverse_tangles(a, n); hyb_rec(a, 0, n); }   /* step 4: the hybrid, as described */
    if (g_phase_on) g_ph[3] = now_ms() - t0;
}
static void edge_sort(double *a, size_t n, int trim) {
    if (n < 2) return;
    if (!edge_bufs(n)) { std::sort(a, a + n); return; }
    edge_arrange(a, n);
    edge_finish(a, n, trim);
}

/* ---------------------------------------------------------------- the shapes */
enum { T_SORTED, T_JITTER, T_RETX, T_KINKS, T_SKEW, T_BATCH, T_RING, T_NEWEST, T_ROCKET, T_MSCLK, T_LATE, T_UNIFORM, T_SHUF,
       T_BLOCKS, T_STAIRS, T_CONCAT, T_N };
static const char *TN[T_N] = { "sorted (one server)", "jitter (latency 0.5 ms)", "jitter + 0.1% retransmits", "kinks (1% adjacent swaps)",
    "8 servers, clock skew +-20 ms", "64 batches, overlapping", "ring buffer, jitter", "newest first, jitter", "rocket telemetry + dropouts",
    "ms clocks (ties), jitter", "late batch appended (5%)", "uniform (CONTROL)", "jitter, SHUFFLED (CONTROL)",
    "64 blocks shuffled, half desc", "64 blocks overlapping 50%", "8 server logs concatenated" };
struct ev_t { double arrive, value; };
static void by_arrival(std::vector<ev_t> &e, double *a) {
    std::sort(e.begin(), e.end(), [](const ev_t &x, const ev_t &y) { return x.arrive < y.arrive; });
    for (size_t i = 0; i < e.size(); i++) a[i] = e[i].value;
}
static void send_times(double *t, size_t n) { double c = 1.7e15; for (size_t i = 0; i < n; i++) { c += -std::log(1.0 - fr()) * 10.0; t[i] = c; } }
static void gen(double *a, size_t n, int s) {
    RS = 1234567ull + (uint64_t)s * 7919ull;
    std::vector<double> t(n); send_times(t.data(), n);
    std::vector<ev_t> e;
    switch (s) {
    case T_SORTED: std::memcpy(a, t.data(), n * 8); break;
    case T_JITTER: case T_RETX: case T_RING: case T_NEWEST: case T_MSCLK: case T_SHUF:
        e.resize(n);
        for (size_t i = 0; i < n; i++) { double lat = -std::log(1.0 - fr()) * 500.0; if (s == T_RETX && fr() < 0.001) lat += 200000.0; e[i].arrive = t[i] + lat; e[i].value = t[i]; }
        by_arrival(e, a);
        if (s == T_MSCLK) for (size_t i = 0; i < n; i++) a[i] = std::floor(a[i] / 1000.0) * 1000.0;
        if (s == T_RING) { size_t r = n * 37 / 100; std::vector<double> c(a, a + n); for (size_t i = 0; i < n; i++) a[i] = c[(i + r) % n]; }
        if (s == T_NEWEST) std::reverse(a, a + n);
        if (s == T_SHUF) for (size_t i = n - 1; i > 0; i--) { size_t j = nr() % (i + 1); std::swap(a[i], a[j]); }
        break;
    case T_KINKS: std::memcpy(a, t.data(), n * 8); for (size_t k = 0; k < n / 100; k++) { size_t x = nr() % (n - 1); std::swap(a[x], a[x + 1]); } break;
    case T_SKEW: {
        double skew[8]; for (int k = 0; k < 8; k++) skew[k] = (fr() * 2.0 - 1.0) * 20000.0;
        e.resize(n);
        for (size_t i = 0; i < n; i++) { int k = (int)(nr() % 8); e[i].arrive = t[i] + (-std::log(1.0 - fr()) * 200.0); e[i].value = t[i] + skew[k]; }
        by_arrival(e, a);
    } break;
    case T_BATCH: case T_STAIRS: {                                     /* 64 agents: window w covers [w, w + f) window-widths of time */
        double f = s == T_STAIRS ? 1.5 : 1.1;
        size_t nb = 64, blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
        double span = t[n - 1] - t[0], W = span / (double)nb;
        for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk], j = i % blk; a[i] = t[0] + (double)b * W + (double)j * (f * W / (double)blk) + fr() * 0.5; }
    } break;
    case T_ROCKET:
        for (size_t i = 0; i < n; i++) { double x = (double)i / (double)n; a[i] = 50000.0 * std::pow(x, 1.5) + (fr() * 2.0 - 1.0) * 3.0; if (fr() < 0.0005) a[i] = 0.0; }
        break;
    case T_LATE: {
        size_t k = n - n / 20; std::memcpy(a, t.data(), k * 8);
        double t0 = t[0] + 0.9 * (t[k - 1] - t[0]), t1 = t[k - 1];
        for (size_t i = k; i < n; i++) a[i] = t0 + fr() * (t1 - t0);
    } break;
    case T_BLOCKS: {                                                   /* the sorted log in 64 blocks, shuffled, every other one descending */
        size_t nb = 64, o = 0; std::vector<size_t> perm(nb); for (size_t k = 0; k < nb; k++) perm[k] = k;
        for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[nr() % (k + 1)]);
        for (size_t k = 0; k < nb; k++) { size_t b = perm[k], s0 = b * n / nb, s1 = (b + 1) * n / nb;
            std::memcpy(a + o, t.data() + s0, (s1 - s0) * 8); if (k & 1) std::reverse(a + o, a + o + (s1 - s0)); o += s1 - s0; }
    } break;
    case T_CONCAT: {                                                   /* 8 server logs, each sorted, appended one after another */
        std::vector<std::vector<double>> sv(8); for (size_t i = 0; i < n; i++) sv[nr() % 8].push_back(t[i]);
        size_t o = 0; for (auto &v : sv) { std::memcpy(a + o, v.data(), v.size() * 8); o += v.size(); }
    } break;
    default: for (size_t i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    }
}

/* ---------------------------------------------------------------- paired timing */
enum { K_LOGOS, K_LOGOS_OFF, K_STD, K_HYB, K_LOGOS_ENV, K_EDGE, K_EDGET };
static const char *g_env = nullptr;   /* K_LOGOS_ENV: this one switch set around the sort */
static const char *OFF[] = { "LOGOS_NO_REACH", "LOGOS_NO_KNOT", "LOGOS_NO_KINK", "LOGOS_NO_STRAY", "LOGOS_NO_SEAM", "LOGOS_NO_WAVE", "LOGOS_NO_MIRROR",
                             "LOGOS_NO_LOOPBACK", "LOGOS_NO_TAIL", "LOGOS_NO_STREAMS", "LOGOS_NO_NEARSORTED", "LOGOS_NO_TWIST", "LOGOS_NO_FASTSCAN" };
static void run(int k, double *w, size_t n, int P) {
    if (k == K_LOGOS) logos_sort_f64(w, n, P);
    else if (k == K_LOGOS_OFF) { for (const char *e : OFF) setenv(e, "1", 1); logos_sort_f64(w, n, P); for (const char *e : OFF) unsetenv(e); }
    else if (k == K_LOGOS_ENV) { const char *v = std::getenv("TS_VALUE"); setenv(g_env, v ? v : "1", 1); logos_sort_f64(w, n, P); unsetenv(g_env); }   /* TS_VALUE: the switch value, "1" if unset */
    else if (k == K_STD) std::sort(w, w + n);
    else if (k == K_EDGE) edge_sort(w, n, 0);
    else if (k == K_EDGET) edge_sort(w, n, 1);
    else hybrid_sort(w, n);
}
/* median of t(kind a) / t(kind b), with every output checked against ref */
static double pair(const double *src, const double *ref, double *w, size_t n, int ka, int kb, int P, int reps, double *p10, double *p90, int *ok) {
    std::vector<double> r(reps); *ok = 1;
    for (int i = 0; i < reps; i++) {
        double t[2] = { 0, 0 };
        for (int s = 0; s < 2; s++) {
            int side = (i & 1) ? 1 - s : s;
            std::memcpy(w, src, n * 8);
            double t0 = now_ms(); run(side ? kb : ka, w, n, P); t[side] = now_ms() - t0;
            if (std::memcmp(w, ref, n * 8)) *ok = 0;
        }
        r[i] = t[1] > 0 ? t[0] / t[1] : 1.0;
    }
    std::sort(r.begin(), r.end());
    *p10 = r[(size_t)(0.1 * reps)]; *p90 = r[(size_t)(0.9 * reps)];
    return (reps & 1) ? r[reps / 2] : 0.5 * (r[reps / 2 - 1] + r[reps / 2]);
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? std::atoi(argv[1]) : 20; size_t n = argc > 2 ? (size_t)std::strtoull(argv[2], nullptr, 10) : 8388608;
    const char *mode = argc > 3 ? argv[3] : "all"; const char *filt = argc > 4 ? argv[4] : nullptr;
    int P = bench_threads(logos_num_cpus()); (void)bench_paired; (void)bench_threads_source; if (reps < 4) reps = 4;
    std::vector<double> s(n), ref(n), w(n);
    if (!std::strcmp(mode, "once")) {
        for (int k = 0; k < T_N; k++) { if (filt && !std::strstr(TN[k], filt)) continue; gen(s.data(), n, k);
            for (int pi = 0; pi < 2; pi++) { int T = pi ? P : 1; std::fprintf(stderr, "#### %s, %d thread(s)\n", TN[k], T); std::memcpy(w.data(), s.data(), n * 8); logos_sort_f64(w.data(), n, T); } }
        return 0;
    }
    if (!std::strcmp(mode, "fixes")) {                /* the two fixes this bench led to, each against its own switch, same binary */
        struct { int shape; const char *env; } R[] = { { T_KINKS, "LOGOS_NO_ONERUN" }, { T_SORTED, "LOGOS_NO_ONERUN" }, { T_JITTER, "LOGOS_NO_ONERUN" },
            { T_BATCH, "LOGOS_NO_ONERUN" }, { T_LATE, "LOGOS_NO_ONERUN" }, { T_UNIFORM, "LOGOS_NO_ONERUN" },
            { T_RING, "LOGOS_NO_SEAM" }, { T_JITTER, "LOGOS_NO_SEAM" }, { T_RETX, "LOGOS_NO_SEAM" }, { T_SKEW, "LOGOS_NO_SEAM" }, { T_UNIFORM, "LOGOS_NO_SEAM" } };
        std::printf("THE TWO FIXES, n = %zu, %d pairs, P = %d: t(switch set) / t(shipped); above 1 = the fix is faster.\n\n  %-30s %-18s | %-22s | %-22s\n", n, reps, P, "shape", "switch", "1 thread", "P threads");
        for (auto &r : R) { if (filt && !std::strstr(TN[r.shape], filt) && !std::strstr(r.env, filt)) continue;
            gen(s.data(), n, r.shape); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end()); g_env = r.env;
            double lo1, hi1, loP, hiP; int ok1, okP;
            double c1 = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, 1, reps, &lo1, &hi1, &ok1), cP = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, P, reps, &loP, &hiP, &okP);
            std::printf("  %-30s %-18s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f]%s\n", TN[r.shape], r.env + 9, c1, lo1, hi1, ok1 ? " " : "X", cP, loP, hiP, okP ? " " : "X"); std::fflush(stdout); }
        return 0;
    }
    if (!std::strcmp(mode, "switch")) {               /* one switch, named by TS_SWITCH, against the shipped engine */
        g_env = std::getenv("TS_SWITCH"); if (!g_env) { std::fprintf(stderr, "set TS_SWITCH=<NAME>\n"); return 2; }
        std::printf("SWITCH %s, n = %zu, %d pairs, P = %d: t(switch set) / t(shipped).\n\n  %-30s | %-22s | %-22s\n", g_env, n, reps, P, "shape", "1 thread", "P threads");
        for (int k = 0; k < T_N; k++) { if (filt && !std::strstr(TN[k], filt)) continue;
            gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            double lo1, hi1, loP, hiP; int ok1, okP;
            double c1 = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, 1, reps, &lo1, &hi1, &ok1), cP = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, P, reps, &loP, &hiP, &okP);
            std::printf("  %-30s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f]%s\n", TN[k], c1, lo1, hi1, ok1 ? " " : "X", cP, loP, hiP, okP ? " " : "X"); std::fflush(stdout); }
        return 0;
    }
    if (!std::strcmp(mode, "onefix")) {               /* one strand's strays put back in place, against the three passes, same binary */
        struct { int shape, far; } R[] = { { T_KINKS, 0 }, { T_SORTED, 1 }, { T_SORTED, 0 }, { T_BLOCKS, 0 }, { T_JITTER, 0 }, { T_UNIFORM, 0 } };
        g_env = "LOGOS_ONERUN_COPY";
        std::printf("ONE STRAND, STRAYS PUT BACK IN PLACE, n = %zu, %d pairs, P = %d: t(three passes) / t(in place); above 1 = in place is faster.\n\n  %-30s | %-22s | %-22s\n", n, reps, P, "shape", "1 thread", "P threads");
        for (auto &r : R) { if (filt && !std::strstr(TN[r.shape], filt)) continue;
            gen(s.data(), n, r.shape);
            if (r.far) { double lo = s[0], span = s[n - 1] - s[0]; for (size_t q2 = 0; q2 < n / 1000; q2++) s[nr() % n] = lo + fr() * span; }
            std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            double lo1, hi1, loP, hiP; int ok1, okP;
            double c1 = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, 1, reps, &lo1, &hi1, &ok1), cP = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, P, reps, &loP, &hiP, &okP);
            std::printf("  %-22s%-8s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f]%s\n", TN[r.shape], r.far ? ", 0.1% far" : "", c1, lo1, hi1, ok1 ? " " : "X", cP, loP, hiP, okP ? " " : "X"); std::fflush(stdout); }
        return 0;
    }
    if (!std::strcmp(mode, "ties")) {                 /* ties continue a descending run, against the strict test, same binary */
        const int R[] = { T_BLOCKS, T_STAIRS, T_BATCH, T_SORTED, T_KINKS, T_JITTER, T_UNIFORM };
        g_env = "LOGOS_DESC_STRICT";
        std::printf("TIES IN A DESCENDING RUN, n = %zu, %d pairs, P = %d: t(strict) / t(ties continue); above 1 = the change is faster.\n\n  %-30s | %-22s | %-22s\n", n, reps, P, "shape", "1 thread", "P threads");
        for (int sh : R) { if (filt && !std::strstr(TN[sh], filt)) continue;
            gen(s.data(), n, sh); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            double lo1, hi1, loP, hiP; int ok1, okP;
            double c1 = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, 1, reps, &lo1, &hi1, &ok1), cP = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, P, reps, &loP, &hiP, &okP);
            std::printf("  %-30s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f]%s\n", TN[sh], c1, lo1, hi1, ok1 ? " " : "X", cP, loP, hiP, okP ? " " : "X"); std::fflush(stdout); }
        return 0;
    }
    if (!std::strcmp(mode, "prescan")) {              /* the dispatcher's prescan in blocks against its scalar line, same binary */
        struct { int shape, rev; } R[] = { { T_SORTED, 0 }, { T_SORTED, 1 }, { T_LATE, 0 }, { T_KINKS, 0 }, { T_BATCH, 0 }, { T_JITTER, 0 }, { T_UNIFORM, 0 } };
        g_env = "LOGOS_NO_FASTPRESCAN";
        std::printf("THE PRESCAN IN BLOCKS, n = %zu, %d pairs, P = %d: t(scalar line) / t(blocks); above 1 = the blocks are faster.\n\n  %-30s | %-22s | %-22s\n", n, reps, P, "shape", "1 thread", "P threads");
        for (auto &r : R) { if (filt && !std::strstr(TN[r.shape], filt)) continue;
            gen(s.data(), n, r.shape); if (r.rev) std::reverse(s.begin(), s.end());
            std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            double lo1, hi1, loP, hiP; int ok1, okP;
            double c1 = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, 1, reps, &lo1, &hi1, &ok1), cP = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_ENV, K_LOGOS, P, reps, &loP, &hiP, &okP);
            std::printf("  %-22s%-8s | %7.2f [%5.2f %5.2f]%s | %7.2f [%5.2f %5.2f]%s\n", TN[r.shape], r.rev ? ", REVERSED" : "", c1, lo1, hi1, ok1 ? " " : "X", cP, loP, hiP, okP ? " " : "X"); std::fflush(stdout); }
        return 0;
    }
    if (!std::strcmp(mode, "edge")) {                 /* round two: the edge sort against the engine, the hybrid and std::sort, ONE thread */
        std::printf("THE EDGE SORT, n = %zu, %d pairs, ONE thread (the proposal is sequential). Median of paired ratios [p10 p90].\n", n, reps);
        std::printf("t(edge)/t(logos) and t(edge+trim)/t(logos): above 1 = logos faster. t(hybrid)/t(edge): above 1 = steps 1-3 help the hybrid.\n");
        std::printf("t(std::sort)/t(edge+trim): above 1 = the trimmed form beats std::sort. X = an output failed its check.\n\n");
        std::printf("  %-30s | %-22s | %-22s | %-22s | %-22s\n", "shape", "logos vs edge", "logos vs edge+trim", "edge vs hybrid", "edge+trim vs std::sort");
        for (int k = 0; k < T_N; k++) {
            if (filt && !std::strstr(TN[k], filt)) continue;
            gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            double c[4], lo[4], hi[4]; int ok[4];
            c[0] = pair(s.data(), ref.data(), w.data(), n, K_EDGE, K_LOGOS, 1, reps, &lo[0], &hi[0], &ok[0]);
            c[1] = pair(s.data(), ref.data(), w.data(), n, K_EDGET, K_LOGOS, 1, reps, &lo[1], &hi[1], &ok[1]);
            c[2] = pair(s.data(), ref.data(), w.data(), n, K_HYB, K_EDGE, 1, reps, &lo[2], &hi[2], &ok[2]);
            c[3] = pair(s.data(), ref.data(), w.data(), n, K_STD, K_EDGET, 1, reps, &lo[3], &hi[3], &ok[3]);
            std::printf("  %-30s |", TN[k]);
            for (int q = 0; q < 4; q++) std::printf(" %7.2f [%5.2f %5.2f]%s |", c[q], lo[q], hi[q], ok[q] ? " " : "X");
            std::printf("\n"); std::fflush(stdout);
        }
        return 0;
    }
    if (!std::strcmp(mode, "edgeanat")) {             /* round two: each claim measured on one run of each shape */
        std::printf("THE EDGE SORT'S ANATOMY, n = %zu, one run per shape, both finishes checked against the reference.\n", n);
        std::printf("K = blocks after condensing; moved = whether the min edges needed a rearrangement; share = the edge sort's part of the\n");
        std::printf("edge pipeline's time; overlap = neighbouring blocks whose ranges overlap after the rearrangement; d = distance from sorted place.\n\n");
        std::printf("  %-30s %10s %10s %5s %7s %8s %10s %10s %8s %10s %10s\n", "shape", "K", "n / K", "moved", "share", "overlap",
                    "mean d", "max d", "log2(1+d)", "hyb m/n", "trim m/n");
        std::vector<double> x(n);
        for (int k = 0; k < T_N; k++) {
            if (filt && !std::strstr(TN[k], filt)) continue;
            gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            if (!edge_bufs(n)) return 1;
            std::memcpy(w.data(), s.data(), n * 8); g_phase_on = 1; edge_arrange(w.data(), n);
            size_t K = g_blk.size(), ov = 0;
            for (size_t b = 0; b + 1 < K; b++) if (w[g_blk[b].lo + g_blk[b].len - 1] > w[g_blk[b + 1].lo]) ov++;
            double sd = 0, sl = 0; size_t md = 0;
            for (size_t i = 0; i < n; i++) {
                size_t lo = (size_t)(std::lower_bound(ref.begin(), ref.end(), w[i]) - ref.begin());
                size_t hi = (size_t)(std::upper_bound(ref.begin(), ref.end(), w[i]) - ref.begin()) - 1;
                size_t d = i < lo ? lo - i : (i > hi ? i - hi : 0);
                sd += (double)d; sl += std::log2(1.0 + (double)d); if (d > md) md = d;
            }
            std::memcpy(x.data(), w.data(), n * 8);
            g_merges = g_bypass = g_merged = 0; edge_finish(w.data(), n, 0);
            double ph_total = g_ph[0] + g_ph[1] + g_ph[2] + g_ph[3], share = ph_total > 0 ? g_ph[1] / ph_total : 0;
            int okh = !std::memcmp(w.data(), ref.data(), n * 8);
            g_tmerges = g_tbypass = g_tmoved = 0; edge_finish(x.data(), n, 1);
            int okt = !std::memcmp(x.data(), ref.data(), n * 8); g_phase_on = 0;
            std::printf("  %-30s %10zu %10.1f %5s %6.1f%% %7.1f%% %10.1f %10zu %8.2f %10.2f %10.2f%s\n", TN[k], K, (double)n / (double)K,
                        g_rearranged ? "yes" : "no", 100.0 * share, 100.0 * (double)ov / (double)(K > 1 ? K - 1 : 1), sd / (double)n, md,
                        sl / (double)n, (double)g_merged / (double)n, (double)g_tmoved / (double)n, okh && okt ? "" : "  WRONG");
            std::fflush(stdout);
        }
        return 0;
    }
    if (!std::strcmp(mode, "anatomy")) {
        std::printf("THE HYBRID'S ANATOMY, n = %zu: what its boundary check skips on each shape (one run each, output checked).\n\n", n);
        std::printf("  %-32s %12s %12s %10s %14s\n", "shape", "merges", "bypassed", "bypass %", "merged / n");
        for (int k = 0; k < T_N; k++) {
            if (filt && !std::strstr(TN[k], filt)) continue;
            gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
            std::memcpy(w.data(), s.data(), n * 8); g_bypass = g_merges = g_merged = g_reversed = 0; hybrid_sort(w.data(), n);
            int ok = !std::memcmp(w.data(), ref.data(), n * 8);
            std::printf("  %-32s %12zu %12zu %9.1f%% %14.2f%s\n", TN[k], g_merges, g_bypass, 100.0 * (double)g_bypass / (double)(g_merges ? g_merges : 1), (double)g_merged / (double)n, ok ? "" : "  WRONG");
        }
        return 0;
    }
    std::printf("SERVER TIMESTAMPS WITH SLIGHT KNOTS, n = %zu, %d pairs, P = %d. Median of paired ratios [p10 p90]; ABOVE 1 = LOGOS IS FASTER.\n", n, reps, P);
    std::printf("knot view = logos with every order-reading route switched off, against logos as shipped. X = an output failed its check.\n\n");
    std::printf("  %-30s | %-22s | %-22s | %-22s | %-22s\n", "shape", "vs std::sort (1T)", "vs hybrid (1T)", "knot view (1T)", "knot view (PT)");
    for (int k = 0; k < T_N; k++) {
        if (filt && !std::strstr(TN[k], filt)) continue;
        gen(s.data(), n, k); std::memcpy(ref.data(), s.data(), n * 8); std::sort(ref.begin(), ref.end());
        double c[4], lo[4], hi[4]; int ok[4];
        c[0] = pair(s.data(), ref.data(), w.data(), n, K_STD, K_LOGOS, 1, reps, &lo[0], &hi[0], &ok[0]);
        c[1] = pair(s.data(), ref.data(), w.data(), n, K_HYB, K_LOGOS, 1, reps, &lo[1], &hi[1], &ok[1]);
        c[2] = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_OFF, K_LOGOS, 1, reps, &lo[2], &hi[2], &ok[2]);
        c[3] = pair(s.data(), ref.data(), w.data(), n, K_LOGOS_OFF, K_LOGOS, P, reps, &lo[3], &hi[3], &ok[3]);
        std::printf("  %-30s |", TN[k]);
        for (int q = 0; q < 4; q++) std::printf(" %7.2f [%5.2f %5.2f]%s |", c[q], lo[q], hi[q], ok[q] ? " " : "X");
        std::printf("\n"); std::fflush(stdout);
    }
    std::printf("\nEvery output compared bit for bit with a std::sort of the input.\n");
    return 0;
}
