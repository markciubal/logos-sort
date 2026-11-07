"""Build two JSON files for the TypeSafe AI (Jev) System One API from this repo's own records.

  statements.json  100 claims about logos-sort with their grade and evidence -- the "proof of speed and
                   efficiency" in a form something else can read. Extracted from TRUTH_TABLE.md so they
                   are the recorded claims verbatim, not a paraphrase, plus the results measured since.

  questions.json   100 typed questions in Jev's schema (noul / choice / score). Most carry an `expected`
                   block: this repo has ~180 settled claims WITH ground truth and, for many, the human
                   prediction that was written before the measurement. That makes it a calibration set --
                   you can score the model's probabilities against outcomes AND against the human prior,
                   which on this record was too optimistic about two thirds of the time.

`expected` is scoring data. It must NOT be sent to the model; only `question` goes in the request.

Run from the repo root:  python tools/typesafe_corpus.py
"""
import json
import os
import re

NL = chr(10)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "reports")
ENDPOINT = "POST https://api.typesafe.ai/v1/systemone"
MODEL = "jev-latest"

# The state every question is asked against: the measured context, not prose. Without it a question about
# "does X clear 1.05x" is unanswerable, because 1.05x only means something beside the resolving floor.
STATE = {
    "project": "logos-sort: a distribution-aware adaptive numeric sort engine in C. Thesis: addressing beats "
               "comparing -- detect structure (cardinality, sortedness, skew, quantization) and dispatch to a "
               "zero- or low-comparison path instead of a comparison sort.",
    "hardware": "i9-13900H, 6 performance cores + 8 efficient cores, 14 bench threads, WSL2. Single socket.",
    "method": {
        "paired_ab": "two variants run adjacently on the two halves of one array, order alternated; median of "
                     "the ratio with p10/p90. Drift cancels inside a pair, ordering bias across pairs.",
        "resolving_floor": "the same engine on both sides reads 1.000; 40 pairs resolve ~3.7% at 8M and ~6.1% "
                           "at 1M. An effect below that is not a result.",
        "layout_bias": "engine copies differing only in renamed symbols differ up to 1.24x from code alignment, "
                       "so any claim under 1.25x needs the copy assignment permuted or the mechanism traced.",
        "verification": "every benchmark checks output against a reference; ~45,899 harness property checks.",
        "calibration_line": "memcpy 64MB and std::sort 1M are printed per run; quiet is ~2.9ms/~65ms. A loaded "
                            "box flatters logos, because barrier-heavy opponents lose more to preemption.",
    },
    "competitive_position": "At 14 threads on AVX2 hardware: parity with x86-simd-sort on random shapes "
                            "(0.86-1.08x), ahead on structured (few_distinct 1.4x, quantized 1.76x, block shapes "
                            "2.2-2.5x). Loses to AVX-512 x86-simd-sort on all unstructured shapes. Loses 2-9x to "
                            "std::sort below ~100 elements.",
    "memory": "Peak heap of one cold call, bytes per element at 10M, one thread: logos 15.7, LSD radix 16.0, "
              "timsort 3.9, IPS4o 0.2, std::sort 0.0, x86-simd-sort 0.0, logos strict in-place entry 0.0.",
}


# ---------------------------------------------------------------- truth-table extraction
def clean(s):
    s = s.strip()
    s = re.sub(r"\*\*(.+?)\*\*", r"\1", s)          # bold
    s = re.sub(r"`(.+?)`", r"\1", s)                 # code ticks
    s = re.sub(r"~~(.+?)~~", r"\1", s)               # strikethrough
    s = s.replace("\\|", "|")
    return re.sub(r"\s+", " ", s).strip()


def rows_of(path):
    """Every | ID | a | b | c | row, with the section heading it sits under."""
    out, sec = [], "?"
    for line in open(path, encoding="utf-8").read().split("\n"):
        if line.startswith("## "):
            sec = line[3:].strip()
            continue
        m = re.match(r"\|\s*([A-Z]\d+[a-z]?)\s*\|(.*)$", line)
        if not m:
            continue
        # Split on the real column separator. A prose pipe is escaped in the source, so hide it behind a
        # sentinel first, or the split invents columns and every later field shifts by one.
        sent = chr(1)
        body = m.group(2).replace(chr(92) + "|", sent)
        cells = [clean(c.replace(sent, "|")) for c in body.split("|")]
        while cells and not cells[-1]:
            cells.pop()
        if len(cells) < 3:
            continue
        # every section is | id | claim | verdict | evidence |, including C (| id | idea | result | why |)
        out.append({"id": m.group(1), "section": sec, "a": cells[0], "b": cells[1], "c": " ".join(cells[2:]).strip()})
    return out


TT = rows_of(os.path.join(ROOT, "TRUTH_TABLE.md"))
BY = {}
for r in TT:
    BY.setdefault(r["section"][0], []).append(r)


def take(letter, n, pred=None):
    got = [r for r in BY.get(letter, []) if (pred is None or pred(r))]
    return got[:n]


# ---------------------------------------------------------------- results measured after the table was written
SESSION = [
    ("S01", "Branchless Batcher networks for 4-16 elements in the leaf, generated and proved by the 0-1 principle "
            "before being written", "MEASURED", "random calls 1.03-1.06x at n=4 rising monotonically to 1.57-1.58x "
            "at n=16, two runs and both copy orientations; n=18 and 24 read 0.99 as the control; sorted rows "
            "0.97-1.04. A small-end result, not a result at scale. SHIPPED"),
    ("S02", "The lattice MAP phase claims chunks from an atomic counter instead of taking equal slices",
     "MEASURED", "quantized and mod_card_30k 0.90-0.97 on 12 of 12 cells at 4M+, two runs of 60 pairs, against a "
                 "null switch on the same rows at 0.97-1.04. ~1.05x. SHIPPED"),
    ("S03", "The same work queue applied to relocation's output tiles", "FALSE",
     "0.96-1.04 on every cell of two runs against a prediction of 0.88-0.97. A work queue buys balance only where "
     "the CORES are the bottleneck; those phases are memcpy over disjoint ranges, bound by one memory controller. "
     "REVERTED"),
    ("S04", "Each lattice worker allocates its own count table rather than the caller allocating all P before the "
            "spawn", "MEASURED", "~1.03x, 20 of 20 cells on the winning side against a measured null. SHIPPED"),
    ("S05", "The lattice route's idle threads are its serial merge", "FALSE",
     "split into four profiler phases at 14 threads: map 68% at concurrency 10.72/14, merge 1.0%, emit 28% at "
     "concurrency 3.17/14, teardown 0.14%. The merge is not the problem"),
    ("S06", "The lattice route's 2ms of table teardown is worth attacking", "FALSE",
     "9-14 MICROseconds, 0.14% of the route. The 2ms came from a TRACE build; a trace build gives shares, never a "
     "price against a normal build"),
    ("S07", "The lattice emit phase's low concurrency is thread imbalance", "FALSE",
     "every thread gets exactly n/P output positions, so it cannot be. A standalone replica splits it: create+join "
     "of 13 workers is 0.564ms of 1.597ms (35%), and the remaining fill gets only 2.7x from 14 threads because "
     "67MB of stores at 65 GB/s is the DDR5 ceiling"),
    ("S08", "Knowing the data FORMAT (that values are dates) beats certifying the distribution, for NUMERIC "
            "timestamps", "FALSE",
     "a sort TOLD the origin and step is 0.24-0.77 of the engine (1.3-4.2x slower) across six date shapes, and "
     "returns WRONG OUTPUT on day-plus-fraction, which has no exact integer form. Predicted 2-4x available"),
    ("S09", "Knowing the date format pays when the dates are TEXT", "MEASURED",
     "parse to a numeric key + logos_argsort_f64 + one gather is 8.48x over std::sort on std::string and 4.87x "
     "over fixed 24-byte memcmp records, 2M records, one thread. A format is worth what it saves in COMPARISON "
     "WIDTH, not in distribution knowledge"),
    ("S10", "Projecting each element onto a sphere of radius n (x=value, y=a prime, z=the index) and sorting the "
            "projection", "FALSE",
     "22.2% of 2.0M pairs at different indices come back order-INVERTED. proj_x = R*x/sqrt(x^2+p^2+z^2), and with "
     "z to 8.4M against values to 1e4 the z^2 term dwarfs x^2, so it orders by value/index. Cost alone would NOT "
     "have killed it: 2.89 ns/element against the engine's 0.60 ns addressing multiply-add"),
    ("S11", "The scale report's memory column reproduces make memprobe from an independent code path", "MEASURED",
     "logos 15.7 bytes/element at 10M against memprobe's documented 15.5 (1.3%); the strict in-place entry and "
     "std::sort both exactly 0.0. Agreement between two independent instruments"),
    ("S12", "The dispatcher's frame grew across one session and that is the untraced small-call drift",
     "MEASURED", "psort_f64 is 36,640 bytes in the committed engine and 42,576 in the working tree; stack-clash "
                  "protection probes every 4096-byte page on every call, so 9 probes against 11 -- two extra on a "
                  "~17ns sorted call, which matches the recorded 1.08-1.14 drift"),
    ("S13", "A trace build can be used to price a phase", "FALSE",
     "it timed the lattice teardown at 2ms where a normal build measures 9-14us, and once timed a lookup table "
     "1.3x FASTER than the compares it replaced where the normal build read 0.66-0.71x"),
    ("S14", "The triple-reversal weld (condense into runs, meta-sort by min edge, in-place triple reversals, seam "
            "weld behind an O(1) check)", "FALSE",
     "2 to 4,600x behind the engine on 14 of 16 shapes at one thread, and the O(1) check bypassed 0.0% of seams on "
     "every shape: a maximal run swallows the next block, so units span holes and the merge behind the test is "
     "O(prefix), i.e. quadratic"),
    ("S15", "The handed-off Moebius hybrid's micro-kink branch (Reidemeister I on 2-3 element dips)", "FALSE",
     "fires 0 times on the shape it was written for. A dip's first element belongs to the PRECEDING ascending run, "
     "so the descending run is one element, never 2 or 3 -- which is why the engine's own repair asks BOTH "
     "neighbours instead, and that one is worth 5.8x/5.0x/3.2x"),
]

# ---------------------------------------------------------------- open items: no ground truth yet
OPEN = [
    ("O01", "A persistent worker pool replacing per-call pthread_create",
     "The lattice route pays spawn+join twice (map and emit): create+join of 13 workers measures 0.564ms of a "
     "1.597ms emit phase, about 1.13ms of a 6.58ms call. Every pthread_create already goes through par_run/"
     "par_spawn, so it is a change to two functions rather than 23 sites.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O02", "Interleaving the lattice map's keys[] and cnts[] into one 16-byte record",
     "The map is 68% of the route at concurrency 10.72/14. Its hot path touches keys[idx] then cnts[idx] at the "
     "same random index in two separate 400KB arrays: two cache lines per element where one record would be one. "
     "Caution: the ledger's 'interleaved counters' for the partition histogram measured neutral (0.95-1.04).",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O03", "A thin front for the dispatcher: prescan and insertion sort in a small-frame function, the large body "
            "behind a call",
     "psort_f64's frame is 42,576 bytes, so stack-clash protection probes 11 pages on every call including a sort "
     "of 2 elements. Entering the dispatcher costs ~5.6ns before any work.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "1.40-2.0", "above 2.0"]),
    ("O04", "An MSD radix for the f64 leaf that stops after the top four key bytes and finishes tie groups by "
            "insertion",
     "Measured: on uniform, heavy_tail and int_wide the real tie groups under a 32-bit key prefix hold at most "
     "7-10 elements, and few_distinct/quantized ties are 100% duplicates (free). But clustered collapses to 10,274 "
     "and bimodal to 512,897, because the top 32 bits resolve to ~1e-6 of the magnitude inside one binade.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O05", "Splitting one overlapping chain's merge across threads in relocation's phase 2",
     "Phase 2 is already queue-driven, but one long chain is a single indivisible task, so one thread merges it "
     "while thirteen wait at the second barrier. Equalising the output tiles changed nothing (0.96-1.04), which is "
     "what points here.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O06", "Windowing the f64 partition's key grid from trimmed quantiles instead of the raw sample",
     "One zero in the sample is enough to defeat the certificates: a sloped sine starts at 0, a[0] is always "
     "sampled, the key grid in total-order key space goes coarse because the neighbourhood of zero spans hundreds "
     "of binades, and the grid is rejected. The same wave shifted to its crest runs at 0.85-0.88 of the time.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O07", "An int32 twin of the short-reach route",
     "The f64 short-reach route (sort index blocks where they lie, repair only the seams) is 1.9-2.5x at one "
     "thread and 1.3-1.5x at 14, and weighs 0.10 bytes per element at one thread against 15.5. int32 has no such "
     "route, and no relocation either.",
     ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "above 1.40"]),
    ("O08", "A GPU/SIMT port of the counting and radix paths",
     "The thesis is that addressing beats comparing; on a CPU the scatter is bound by per-core latency (proved "
     "three times: ping-pong removed 37% of traffic for 12% of time, huge pages and streaming stores both measured "
     "exactly zero). A GPU is the one hardware where scatter is cheap.",
     ["a loss", "no better than the CPU engine", "2-5x", "5-20x", "above 20x"]),
    ("O09", "logos_query: promoting the order/analytics API (rank, select, quantiles, topk, count_in_range, "
            "distinct) to first-class entry points",
     "When the deliverable is ORDER or ANALYTICS rather than a sorted array, the no-scatter model already measures "
     "quantiles 87x, top-k 7x, range-count 42x against sort-first baselines -- but those baselines were unfair "
     "(sort-first and nth_element), and a fair querybench against radix-select and streaming heaps is still owed.",
     ["a loss", "inside the floor", "1.5-3x", "3-10x", "above 10x"]),
    ("O10", "A structure-router: probe cheaply, send structured data to logos's counting/lattice paths and "
            "everything else to x86-simd-sort wholesale",
     "logos beats x86-simd-sort only on structured data (few_distinct 1.4x, quantized 1.76x, block shapes "
     "2.2-2.5x) and loses on every unstructured shape. The probe is the shared sample, already 1-4% of a call.",
     ["a loss", "inside the floor", "1.1-1.3x", "1.3-2.0x", "above 2.0x"]),
]


def noul(instr, t, f):
    return {"type": "noul", "instructions": instr, "criteria": {"true": t, "false": f}}


def score(instr, levels):
    return {"type": "score", "instructions": instr, "criteria": levels}


def choice(instr, opts):
    return {"type": "choice", "instructions": instr, "criteria": opts}


BANDS = ["a loss (<1.00)", "inside the floor (1.00-1.04)", "1.05-1.15", "1.15-1.40", "1.40-2.0", "above 2.0"]

# ---------------------------------------------------------------- statements
statements = []
for r in take("A", 12) + take("B", 28) + take("D", 26) + take("C", 18) + take("E", 4) + take("F", 2):
    statements.append({
        "id": r["id"],
        "source": "TRUTH_TABLE.md",
        "section": r["section"],
        "statement": r["a"],
        "verdict": r["b"],
        "evidence": r["c"],
    })
for sid, claim, grade, ev in SESSION:
    statements.append({"id": sid, "source": "FINDINGS.md / CHANGELOG.md (2026-09-22/23)",
                       "section": "Measured after the truth table was last written",
                       "statement": claim, "verdict": grade, "evidence": ev})
statements = statements[:100]

# ---------------------------------------------------------------- questions
items = []

# (a) settled negatives -> noul, expected false. The ledger is the ground truth.
for r in take("C", 28):
    items.append({
        "id": "Q_C_" + r["id"],
        "kind": "calibration",
        "question": noul(
            "In a distribution-aware numeric sort engine (logos-sort), this was implemented and measured end to "
            "end: " + r["a"] + ". Judge whether it BEAT the engine as it shipped.",
            "It beat the shipped engine by more than the resolving floor (>1.05x) on its target shapes",
            "It did not: neutral, mixed, or slower"),
        "expected": {"noul": 0.0, "outcome": r["b"], "why": r["c"]},
        "provenance": "TRUTH_TABLE.md " + r["id"] + " (negative ledger)",
    })

# (b) settled positives -> noul, expected true
for r in take("D", 24):
    items.append({
        "id": "Q_D_" + r["id"],
        "kind": "calibration",
        "question": noul(
            "In logos-sort, this was built and A/B'd against the engine as it shipped: " + r["a"] +
            ". Judge whether it BEAT the engine by more than the resolving floor.",
            "It beat the shipped engine by more than the resolving floor on its target shapes",
            "It did not: neutral, mixed, or slower"),
        "expected": {"noul": 1.0, "outcome": r["b"], "why": r["c"]},
        "provenance": "TRUTH_TABLE.md " + r["id"] + " (positive results)",
    })

# (c) this session's settled results -> noul with the human prediction recorded beside the outcome
PRED = {
    "S01": "1.5-3x across n=4-16", "S02": "0.75-0.90 (i.e. 1.11-1.33x)", "S03": "0.88-0.97 (i.e. 1.03-1.14x)",
    "S05": "the serial merge was ~55% of the route", "S06": "2ms, worth a persistent pool",
    "S08": "2-4x available on epoch_ms_day", "S09": "8-25x over std::string, 3-8x over fixed records",
    "S10": "the projection would collide and cost more than the sort",
}
for sid, claim, grade, ev in SESSION:
    won = grade == "MEASURED"
    items.append({
        "id": "Q_" + sid,
        "kind": "calibration",
        "question": noul(
            "In logos-sort: " + claim + ". Judge whether this held up under a paired A/B against the shipped "
            "engine, with the resolving floor at ~3.7% and layout bias up to 1.24x on copies.",
            "It held up and shipped", "It did not hold up: neutral, slower, or the claim was false"),
        "expected": {"noul": 1.0 if won else 0.0, "outcome": grade, "why": ev,
                     "human_prediction_before_measuring": PRED.get(sid)},
        "provenance": "FINDINGS.md / CHANGELOG.md (2026-09-22/23)",
    })

# (d) effect size for settled items, as bands -- the same shape the project writes predictions in
SIZED = [
    ("Q_SZ_networks", "Branchless Batcher sorting networks for 4-16 elements replacing insertion sort in the leaf. "
                      "Rate the speedup on RANDOM calls at n=16, one thread.", 4,
     "measured 1.57-1.58x (two runs, both copy orientations)"),
    ("Q_SZ_net4", "The same networks, rated on RANDOM calls at n=4.", 1, "measured 1.03-1.06x"),
    ("Q_SZ_latqueue", "The lattice map phase claiming chunks from a counter instead of equal slices, at 14 threads "
                      "on quantized. Rate the speedup.", 2, "measured ~1.05x (0.90-0.97 on 12 of 12 cells)"),
    ("Q_SZ_tiles", "The same work queue applied to relocation's memcpy output tiles, 14 threads. Rate it.", 1,
     "measured 0.96-1.04: neutral"),
    ("Q_SZ_prescan", "Making the dispatcher's prescan compare with vectors (AVX2 blocks of 32) where its block "
                     "loop had compiled to sixteen scalar compares and a serial OR chain. Rate it on SORTED input.",
     4, "measured 1.72-2.22x"),
    ("Q_SZ_kink", "Swapping a kinked adjacent pair back where it stands inside a run (Reidemeister I), on sorted "
                  "blocks with 0.1% kinks at one thread. Rate it.", 5, "measured 5.8x"),
    ("Q_SZ_datestr", "Parsing fixed-width ISO-8601 timestamps to a numeric key and using argsort + one gather, "
                     "against std::sort on std::string. Rate it.", 5, "measured 8.48x"),
    ("Q_SZ_datenum", "Telling the sort the origin and step of NUMERIC epoch-millisecond timestamps, so it can "
                     "convert to integer keys and radix only the varying bytes. Rate it against the engine.", 0,
     "measured 0.77: the told sort is SLOWER"),
    ("Q_SZ_hugepage", "Huge pages for the engine's large buffers, at 14 threads and 8M, with the mechanism "
                      "verified firing (98MB AnonHugePages vs 0). Rate it.", 1, "measured 0.98-1.01: exactly neutral"),
    ("Q_SZ_gallop", "A galloping two-strand merge for one sparse strand spanning every value over dense blocks, "
                    "one thread, 8.4M. Rate it.", 5, "measured 2.45-2.70x"),
]
for qid, instr, lvl, why in SIZED:
    items.append({"id": qid, "kind": "calibration", "question": score(instr, BANDS),
                  "expected": {"score": lvl, "legend": BANDS[lvl], "why": why},
                  "provenance": "measured in this repo"})

# (e) mechanism questions -- where this project has been wrong most often
items.append({
    "id": "Q_MECH_lattice_idle", "kind": "calibration",
    "question": choice(
        "logos-sort's lattice counting route at 14 threads shows only 8.4 of 14 threads busy on average, with NONE "
        "waiting at a barrier and no preemption (cpu/busy = 1.00). Every thread is handed exactly n/P work in both "
        "of its parallel phases. Which is the binding constraint?",
        {"serial_merge": "the serial merge between the two parallel phases",
         "table_teardown": "freeing the per-thread count tables at the end",
         "thread_creation": "creating and joining the worker threads, paid once per parallel phase",
         "memory_bandwidth": "the emit phase saturating memory bandwidth, so extra threads cannot help",
         "thread_imbalance": "unequal work per thread inside the phases",
         "cache_pressure": "the per-thread tables being too large for L2"}),
    "expected": {"choice": "thread_creation",
                 "why": "create+join of 13 workers is 0.564ms of a 1.597ms emit phase (35%), and the rest is "
                        "bandwidth-bound (14 threads buy 2.7x; 67MB of stores at 65 GB/s). The merge is 1.0% and "
                        "the teardown 0.14%. memory_bandwidth is the defensible second answer.",
                 "acceptable_alternate": "memory_bandwidth"},
    "provenance": "make profile with the route split into four phases; make spawnprobe"})

items.append({
    "id": "Q_MECH_proposal_failure", "kind": "open",
    "question": choice(
        "An outside sorting proposal arrives for logos-sort. Given that the engine sorts at 0.8-2.1 ns/element at "
        "14 threads and already certifies cardinality, sortedness, skew and quantization from one shared sample, "
        "which failure mode will the proposal hit FIRST?",
        {"per_element_cost": "its per-element transform costs more than the whole sort",
         "destroys_order": "its transform is not monotone in the value, so the output is not sorted",
         "already_subsumed": "the engine's existing routes already exploit the same structure",
         "isolated_win_only": "the kernel wins in isolation and loses end to end",
         "gate_never_fires": "the branch it relies on fires zero times on its target shape",
         "fixed_cost_per_call": "it is free at scale but adds cost to every small call"}),
    "expected": None,
    "provenance": "the five outside proposals measured in this repo hit, respectively: already_subsumed, "
                  "destroys_order, per_element_cost, gate_never_fires, isolated_win_only"})

items.append({
    "id": "Q_MECH_small_end", "kind": "open",
    "question": choice(
        "logos-sort loses 2-9x to std::sort below ~100 elements. Entering the dispatcher costs ~5.6ns before any "
        "work, and psort_f64's stack frame is 42,576 bytes, which stack-clash protection probes a page at a time on "
        "EVERY call (11 probes). Which lever recovers the most at n = 2-10?",
        {"thin_front": "a small-frame front function, with the large body behind a call",
         "alignment": "an alignment policy (-falign-functions/-falign-loops)",
         "fewer_gates": "skipping more dispatcher gates at tiny n",
         "networks": "branchless sorting networks (already shipped for 4-16)",
         "nothing": "nothing structural; the remaining cost is irreducible"}),
    "expected": None,
    "provenance": "open direction 0g"})

# (f) the open items -> score bands, no expected answer
for oid, title, ctx, levels in OPEN:
    items.append({
        "id": "Q_OPEN_" + oid, "kind": "open",
        "question": score(title + ". Context: " + ctx + " Rate the speedup a paired A/B would measure against the "
                          "engine as it ships, remembering that the resolving floor is ~3.7% at 8M.", levels),
        "expected": None,
        "provenance": "open direction, no measurement yet"})

# (g) methodology questions -- these are the ones that generalise beyond this repo
METH = [
    ("Q_M_floor",
     "A benchmark reports a 4% speedup as the median of 40 paired runs. On the same harness, the SAME code on "
     "both sides reads 1.000 with a p90 deviation of 8-14% per pair, and 40 pairs resolve about 3.7%. Judge the "
     "claim: the 4% is reportable as a result, provided the null is stated alongside it.",
     "The claim holds: 4% is just outside the measured floor, so it is reportable with the null quoted",
     "The claim fails: 4% is inside the noise and needs more pairs or a traced mechanism"),
    ("Q_M_layout",
     "Two engine copies built from IDENTICAL source and flags, differing only in renamed symbols, measured up to "
     "1.24x apart on a quiet box, deterministically, because symbol lengths change code alignment. A new A/B now "
     "shows 1.15x. Judge the claim: that 1.15x should not be trusted until the variant-to-copy assignment is "
     "permuted or the mechanism is traced directly.",
     "The claim holds: below about 1.25x the layout bias can produce the whole effect",
     "The claim fails: 1.15x is large enough to trust as measured"),
    ("Q_M_trace",
     "A build instrumented with per-phase tracing (fprintf at each phase boundary) times a phase at 2 ms. Judge "
     "the claim: that number must not be used to decide whether the phase is worth optimising.",
     "The claim holds: the trace calls change the whole function's code generation, so such a build says WHICH "
     "path ran and what share each phase took, never a price against a normal build",
     "The claim fails: the traced build runs the same code, so its phase timing is usable"),
    ("Q_M_oracle",
     "A double-precision sort's output is compared byte-for-byte against libc qsort's output on the same input. "
     "Judge the claim: that is NOT a sound correctness oracle.",
     "The claim holds: qsort treats -0.0 and +0.0 as equal, so byte comparison flags correct sorts as wrong; "
     "check ascending under < plus the bitwise multiset instead",
     "The claim fails: identical bytes is the strongest available check"),
    ("Q_M_microbench",
     "A kernel measures 1.22x faster than the one it replaces, in isolation. Judge the claim: it should not ship "
     "until the FULL operation is A/B'd end to end.",
     "The claim holds: in this repo three kernels won in isolation and LOST when integrated",
     "The claim fails: a faster kernel makes the whole faster"),
    ("Q_M_loaded",
     "A fair-fight benchmark runs with its calibration line reading 3.5x the quiet reference, and its ratios "
     "favour the engine under test. Judge the claim: those ratios are an upper bound rather than a measurement.",
     "The claim holds: barrier-heavy opponents lose more to preemption than chunk-parallel phases do, so a loaded "
     "box flatters this engine",
     "The claim fails: every contender ran under the same load, so the ratios hold"),
    ("Q_M_fallback",
     "A resource-exhaustion fallback path is written and code-reviewed but has never executed. Judge the claim: "
     "it cannot be relied on until it has been forced to run.",
     "The claim holds: injecting failures found 2,215 of 8,316 allocation failures and 154 of 186 thread-creation "
     "failures ending in wrong output, a crash or a hang, under thousands of green checks",
     "The claim fails: the code is straightforward and was reviewed"),
    ("Q_M_stub",
     "A charting library is replaced by a hand-written stub in tests, and 216 charts pass every test the stub "
     "drives. Judge the claim: the rendering is still unverified.",
     "The claim holds: in a real browser a log axis could not be zoomed inside one decade and a pan killed the "
     "tab; a fake must reproduce the library's awkward behaviour",
     "The claim fails: 216 charts is thorough coverage"),
    ("Q_M_precision",
     "A vendor reports '193.6x faster' with no stated repetition count, measured from laptops. Judge the claim: "
     "the figure's four significant digits are not supported by its method.",
     "The claim holds: four significant figures implies about +-0.05%, which an unrepeated laptop measurement "
     "cannot support",
     "The claim fails: the figure is simply what they measured"),
    ("Q_M_statistic",
     "A routing gate is documented in a comment as reading an inversion rate over about 1900 sampled pairs. Judge "
     "the claim: that comment should not be trusted when reasoning about why the gate misfires.",
     "The claim holds: count what the loop actually reads. The same comment here described a loop reading 7 pairs "
     "a whole array apart, and it turned away 6 of 7 near-sorted inputs",
     "The claim fails: the comment documents the code's intent and is a fair summary"),
]
for qid, instr, t, f in METH:
    items.append({"id": qid, "kind": "methodology", "question": noul(instr, t, f),
                  "expected": {"noul": 1.0, "why": "the 'true' criterion is what this repo measured"},
                  "provenance": "CLAUDE.md methodology and ledger"})

assert len(items) == 100, "expected exactly 100 questions, built %d" % len(items)

# ---------------------------------------------------------------- validate against the published schema
ALLOWED = {"type", "instructions", "criteria"}


def validate(q, qid):
    """Reject anything the API would reject. It refuses unexpected properties, so the question object may
    carry ONLY type, instructions and criteria -- metadata belongs in the sidecar, not here."""
    assert isinstance(q, dict), qid
    extra = set(q) - ALLOWED
    assert not extra, (qid, "unexpected properties in a question: %s" % sorted(extra))
    assert q.get("type") in ("noul", "choice", "score"), (qid, q.get("type"))
    assert isinstance(q.get("instructions"), str) and q["instructions"].strip(), qid
    c = q.get("criteria")
    if q["type"] == "noul":
        assert isinstance(c, dict) and set(c) == {"true", "false"}, (qid, c)
        assert all(isinstance(v, str) and v.strip() for v in c.values()), qid
    elif q["type"] == "choice":
        assert isinstance(c, dict) and 1 <= len(c) <= 255, (qid, len(c) if isinstance(c, dict) else c)
        assert all(isinstance(v, (str, type(None))) for v in c.values()), qid
    else:
        assert isinstance(c, list) and 2 <= len(c) <= 10, (qid, len(c) if isinstance(c, list) else c)
        assert all(isinstance(v, str) and v.strip() for v in c), qid


def validate_qmap(qs):
    """The questions FILE: its root is the map itself. Anything beside the ids is read as a question."""
    assert isinstance(qs, dict) and qs, "the questions file must be a non-empty map of id -> Question"
    for qid, q in qs.items():
        assert isinstance(qid, str) and qid.strip(), "question ids must be non-empty strings"
        assert isinstance(q, dict), "%s must be an object (a bare map has no scalars at the root)" % qid
        validate(q, qid)


def validate_body(body):
    """The ENVELOPE, which is what was wrong last time: the body is exactly state + model + questions, and
    every value under `questions` is a Question. Anything else there is read as a question and rejected."""
    assert set(body) == {"state", "model", "questions"}, sorted(body)
    assert isinstance(body["model"], str) and body["model"], "model must be a string"
    assert isinstance(body["state"], (str, dict, list)), "state must be string | object | array"
    qs = body["questions"]
    assert isinstance(qs, dict) and qs, "questions must be a non-empty map of id -> Question"
    for qid, q in qs.items():
        assert isinstance(qid, str) and qid.strip(), "question ids must be non-empty strings"
        validate(q, qid)


for it in items:
    validate(it["question"], it["id"])
assert len({i["id"] for i in items}) == len(items), "duplicate question id"
assert len({s["id"] for s in statements}) == len(statements), "duplicate statement id"

# ---------------------------------------------------------------- write
os.makedirs(OUT, exist_ok=True)

# The questions file: a BARE MAP at the root. Anything beside the ids is read as a question and rejected.
qmap = {i["id"]: i["question"] for i in items}
validate_qmap(qmap)

# The documented HTTP envelope, kept separately for a raw POST (docs.typesafe.ai/api.md).
body = {"state": STATE, "model": MODEL, "questions": qmap}
validate_body(body)

key = {
    "about": "Scoring key for typesafe-questions.json, keyed by the same ids. NEVER send this: the schema "
             "rejects unexpected properties, so expected/kind/provenance cannot ride inside a question.",
    "kinds": {"calibration": "settled here, with ground truth -- score probabilities against outcomes",
              "open": "no measurement yet -- a prior that would order the next round",
              "methodology": "generalises beyond this repo"},
    "counts": {k: sum(1 for i in items if i["kind"] == k) for k in ("calibration", "open", "methodology")},
    "scoring": "noul returns P(true), so Brier = (returned - expected.noul)^2. For score, compare the returned "
               "level index with expected.score. For choice, expected.choice is the answer and "
               "expected.acceptable_alternate, where present, is defensible on the same evidence.",
    "answers": {i["id"]: {k: v for k, v in i.items() if k in ("kind", "expected", "provenance")} for i in items},
}

stmt_doc = {
    "about": "Claims about logos-sort with their verdict and evidence. A corpus, not a request body.",
    "verdict_field": "for A/B/D/E/F rows the recorded grade; for C rows (the negative ledger) the measured RESULT",
    "grades": {"MEASURED": "measured end to end under the paired-A/B method",
               "INFERRED": "argued from a measurement, not isolated",
               "UNTESTED": "no measurement exists",
               "FALSE": "measured and contradicted"},
    "section_columns": {
        "A. Competitive position": {"verdict": "status", "evidence": "evidence"},
        "B. Cost model": {"verdict": "status", "evidence": "evidence"},
        "C. Negative ledger": {"verdict": "the measured RESULT (every C row was implemented, measured and REJECTED)",
                               "evidence": "why it failed"},
        "D. Positive results": {"verdict": "the measured effect", "evidence": "where in the code"},
        "E. The thesis itself": {"verdict": "grade", "evidence": "note"},
        "F. Correctness envelope": {"verdict": "status", "evidence": "evidence"},
        "Measured after the truth table was last written": {"verdict": "MEASURED or FALSE",
                                                            "evidence": "the numbers and the mechanism"},
    },
    "method_note": STATE["method"],
    "count": len(statements),
    "statements": statements,
}

written = []
for name, doc in (("typesafe-questions.json", qmap),
                  ("typesafe-state.json", STATE),
                  ("typesafe-request-body.json", body),
                  ("typesafe-answers-key.json", key),
                  ("typesafe-statements.json", stmt_doc)):
    path = os.path.join(OUT, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
    written.append((name, os.path.getsize(path) / 1024.0))

# batches, each one also a bare map, in case 100 questions in a call is over an undocumented limit
bdir = os.path.join(OUT, "typesafe-questions-batches")
os.makedirs(bdir, exist_ok=True)
for old in os.listdir(bdir):
    if old.endswith(".json"):
        os.remove(os.path.join(bdir, old))
old_dir = os.path.join(OUT, "typesafe-request-batches")
if os.path.isdir(old_dir):
    for old in os.listdir(old_dir):
        os.remove(os.path.join(old_dir, old))
    os.rmdir(old_dir)
BATCH = 20
ids = list(qmap)
for b in range(0, len(ids), BATCH):
    part = {q: qmap[q] for q in ids[b:b + BATCH]}
    validate_qmap(part)
    with open(os.path.join(bdir, "batch-%02d.json" % (b // BATCH + 1)), "w", encoding="utf-8") as f:
        json.dump(part, f, indent=2, ensure_ascii=False)

# ---------------------------------------------------------------- playground-ready
# The playground takes text pasted as state and questions defined beside it. State goes flat: the docs say
# an object is accepted, but Jev is text-only and a flat blob cannot be rejected for shape.
def state_text(s):
    out = []
    for k, v in s.items():
        if isinstance(v, dict):
            out.append(k.replace("_", " ").upper())
            for k2, v2 in v.items():
                out.append("  - %s: %s" % (k2.replace("_", " "), v2))
        else:
            out.append("%s: %s" % (k.replace("_", " ").upper(), v))
    return NL.join(out)


pdir = os.path.join(OUT, "playground")
os.makedirs(pdir, exist_ok=True)
for old in os.listdir(pdir):
    os.remove(os.path.join(pdir, old))

txt = state_text(STATE)
with open(os.path.join(pdir, "state.txt"), "w", encoding="utf-8") as f:
    f.write(txt + NL)

# 10 per file: small enough to paste, and each still a bare map so it drops straight into a questions box
PB = 10
pids = list(qmap)
nfiles = 0
for b in range(0, len(pids), PB):
    part = {q: qmap[q] for q in pids[b:b + PB]}
    validate_qmap(part)
    with open(os.path.join(pdir, "questions-%02d.json" % (b // PB + 1)), "w", encoding="utf-8") as f:
        json.dump(part, f, indent=2, ensure_ascii=False)
    nfiles += 1

# one small starter: three questions, one of each type, with a known answer -- the cheapest way to confirm
# the shape is right before pasting ninety-seven more
starter = {k: qmap[k] for k in ("Q_M_trace", "Q_SZ_networks", "Q_MECH_lattice_idle")}
validate_qmap(starter)
with open(os.path.join(pdir, "questions-starter.json"), "w", encoding="utf-8") as f:
    json.dump(starter, f, indent=2, ensure_ascii=False)

with open(os.path.join(pdir, "README.txt"), "w", encoding="utf-8") as f:
    f.write(NL.join([
        "Playground paste kit.",
        "",
        "1. state.txt              paste into the state box (plain text; Jev is text-only)",
        "2. questions-starter.json three questions, one of each type, all with known answers:",
        "                          Q_M_trace expects noul ~1.0, Q_SZ_networks expects level 4",
        "                          (1.40-2.0), Q_MECH_lattice_idle expects thread_creation.",
        "                          Run this first to confirm the shape before pasting the rest.",
        "3. questions-01..%02d.json  ten at a time, each a bare map of id -> question" % nfiles,
        "",
        "Every file's ROOT is the questions map itself. Nothing may sit beside the ids: the",
        "validator reads any other top-level key as a question and rejects it.",
        "",
        "Scoring: ../typesafe-answers-key.json, keyed by the same ids. Never paste that one -",
        "it holds the answers, and the schema rejects unexpected properties anyway.",
    ]) + NL)

print("playground/: state.txt (%d chars), questions-starter.json (3), questions-01..%02d.json (%d each)"
      % (len(txt), nfiles, PB))
print("questions file root = bare map of %d ids; every value a Question, no envelope" % len(qmap))
for name, kb in written:
    print("  %-30s %7.1f KB" % (name, kb))
print("  %-30s %d files of <=%d, each a bare map" % ("typesafe-questions-batches/",
                                                     (len(ids) + BATCH - 1) // BATCH, BATCH))
print("counts by kind:", key["counts"])
