# logos-sort (C, multi-threaded) — self-contained build
#
#   make            build static + shared library, harness, example
#   make test       run the harness: correctness vs qsort (exit != 0 on failure) + benchmark
#   make example    build and run the usage example
#   make install    install header + libraries under $(PREFIX)   (default /usr/local)
#   make uninstall
#   make dist       source tarball
#   make clean
#
# Override e.g.:  make CC=clang CFLAGS="-O2 -pthread"   or   make install PREFIX=$HOME/.local

CC      ?= cc
CFLAGS  ?= -O3 -std=gnu11 -pthread -Wall -Wextra -fPIC
LDLIBS  ?= -lpthread -lm
PREFIX  ?= /usr/local
AR      ?= ar
CXX     ?= g++
# Default thread count for every BENCHMARK (the engine itself is unchanged): the physical core
# count of the reference box. `make noiseprobe` showed 14 threads gives the lowest and quietest
# minimum on this 20-thread hybrid i9-13900H; WSL2 reports a fictional 10x2 topology, so it is
# set here rather than detected. Override per run with LOGOS_THREADS=N or per build with
# make BENCH_THREADS=N ...   See engines/c/bench_common.h.
BENCH_THREADS ?= 14
BENCHFLAGS := -DLOGOS_BENCH_THREADS=$(BENCH_THREADS)


VERSION := 3.6.0
LIB     := liblogos_psort
SRC     := logos_psort.c
SRC2    := logos_extra.c
HDR     := logos_psort.h
OBJ     := logos_psort.o
OBJ2    := logos_extra.o

.PHONY: all test example parbench parbench-simd parbench-full pfbench scaling install uninstall dist clean

all: $(LIB).a $(LIB).so harness example-bin

$(OBJ): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -c $(SRC) -o $@

$(OBJ2): $(SRC2) $(HDR)
	$(CC) $(CFLAGS) -c $(SRC2) -o $@

OBJ3    := logos_view.o
$(OBJ3): logos_view.c logos_view.h logos_view_impl.h $(HDR)
	$(CC) $(CFLAGS) -c logos_view.c -o $@

$(LIB).a: $(OBJ) $(OBJ2) $(OBJ3)
	$(AR) rcs $@ $(OBJ) $(OBJ2) $(OBJ3)

$(LIB).so: $(OBJ) $(OBJ2) $(OBJ3)
	$(CC) $(CFLAGS) -shared -o $@ $(OBJ) $(OBJ2) $(OBJ3) $(LDLIBS)

harness: harness.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) harness.c $(LIB).a -o $@ $(LDLIBS)

example-bin: example.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) example.c $(LIB).a -o $@ $(LDLIBS)

test: harness
	./harness

# Scaling test: powers of ten up to 1e9 elements, bounded by 30s/sort and RAM.
# Override: make scaling ; LOGOS_MAX_N=1e8 LOGOS_MAX_SEC=10 ./scaling
# Scaling test + HTML dashboard, with a same-data __gnu_parallel::sort baseline
# (needs g++ + OpenMP). Writes scaling-report.html. Override with
# LOGOS_MAX_N / LOGOS_MAX_SEC / LOGOS_THREADS.
scaling: scaling.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp scaling.cpp $(LIB).a -o $@ $(LDLIBS)

# Prefetch-distance A/B: five engine variants (prefetch off, PF_DIST 16/32/64/128)
# linked into ONE binary and interleaved on identical data, so machine load cannot
# bias the comparison (it only costs precision). Run: make pfbench && ./pfbench
# RENAME(prefix): every public symbol of the engine gets the prefix, so several
# engine variants can be linked into one binary. Keep in sync with logos_psort.h.
RENAME = -Dlogos_sort_f64=$(1)_sort_f64 -Dlogos_sort_f64_batch=$(1)_sort_f64_batch -Dlogos_sort_strings_mt=$(1)_sort_strings_mt -Dlogos_sort_i32=$(1)_sort_i32 -Dlogos_num_cpus=$(1)_num_cpus -Dlogos_release_buffers=$(1)_release_buffers \
         -Dlogos_set_simd_sort=$(1)_set_simd_sort -Dlogos_set_simd_kernels=$(1)_set_simd_kernels \
         -Dlogos_sort_f64_inplace=$(1)_sort_f64_inplace -Dlogos_sort_i32_inplace=$(1)_sort_i32_inplace \
         -Dlogos_profile_reset=$(1)_profile_reset -Dlogos_profile_get=$(1)_profile_get -Dlogos_profile_phase_names=$(1)_profile_phase_names \
         -Dlogos_profile_path_names=$(1)_profile_path_names -Dlogos_profile_route_names=$(1)_profile_route_names \
         -Dlogos_partition_f64=$(1)_partition_f64 -Dlogos_partition_i32=$(1)_partition_i32 \
         -Dlogos_sort_buckets_f64=$(1)_sort_buckets_f64 -Dlogos_sort_buckets_i32=$(1)_sort_buckets_i32 \
         -Dlogos_nan_to_end_f64=$(1)_nan_to_end_f64 -Dlogos_sort_f64_nan_last=$(1)_sort_f64_nan_last -Dlogos_reach_last=$(1)_reach_last \
         -Dlogos_twist_last=$(1)_twist_last -Dlogos_mirror_count=$(1)_mirror_count -Dlogos_prescan_agree=$(1)_prescan_agree
PFV := 16 32 64 128
pfbench: pfbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_PREFETCH $(call RENAME,pf0) -c $(SRC) -o pf0.o
	$(foreach N,$(PFV),$(CC) $(CFLAGS) -DPF_DIST=$(N) $(call RENAME,pf$(N)) -c $(SRC) -o pf$(N).o;)
	$(CC) $(CFLAGS) $(BENCHFLAGS) pfbench.c pf0.o $(foreach N,$(PFV),pf$(N).o) -o $@ $(LDLIBS)

# Disjoint-run relocation (J5) A/B: the engine with and without the path, in ONE
# binary, interleaved on identical data. Run: make j5bench && ./j5bench
j5bench: j5bench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_RELOCATE $(call RENAME,rl0) -c $(SRC) -o rl0.o
	$(CC) $(CFLAGS) $(call RENAME,rl1) -c $(SRC) -o rl1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) j5bench.c rl0.o rl1.o -o $@ $(LDLIBS)

# Splitter-table (H0b) A/B: the histogram's binary search vs the self-choosing
# direct-index tables, in ONE binary, interleaved. Run: make stbench && ./stbench
stbench: stbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_SPLITTER_TABLE $(call RENAME,st0) -c $(SRC) -o st0.o
	$(CC) $(CFLAGS) $(call RENAME,st1) -c $(SRC) -o st1.o
	$(CC) $(CFLAGS) -DLOGOS_MODEL_TABLE $(call RENAME,st2) -c $(SRC) -o st2.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) stbench.c st0.o st1.o st2.o -o $@ $(LDLIBS)

# AVX2 leaf A/B: the scalar leaf (-DLOGOS_NO_AVX2_LEAF) against the in-house
# vectorized quicksort in the sequential leaf (default policy), also in the
# parallel leaf, and also routing int32 buckets. Four variants in ONE binary,
# interleaved, at 1 thread and P. Run: make simdbench && ./simdbench
simdbench: simdbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_AVX2_LEAF $(call RENAME,sb0) -c $(SRC) -o sb0.o
	$(CC) $(CFLAGS) $(call RENAME,sb1) -c $(SRC) -o sb1.o
	$(CC) $(CFLAGS) -DLOGOS_AVX2_PARALLEL $(call RENAME,sb2) -c $(SRC) -o sb2.o
	$(CC) $(CFLAGS) -DLOGOS_AVX2_PARALLEL -DLOGOS_AVX2_INT32 $(call RENAME,sb3) -c $(SRC) -o sb3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) simdbench.c sb0.o sb1.o sb2.o sb3.o -o $@ $(LDLIBS)

# The sorted VIEW: partition now, sort a bucket only when something looks at it. Measures the same
# useful work two ways (sort-then-look vs view), verifies every answer against qsort, and reports the
# cost of materialising everything. See engines/c/logos_view.h.
viewbench: viewbench.c $(LIB).a $(HDR) logos_view.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) viewbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# The int32 twin of viewbench: the same table over logos_view_*_i32, plus the small-span shape where
# the counting route sorts outright and a view can defer nothing (the honest zero row).
viewbench_i32: viewbench_i32.c $(LIB).a $(HDR) logos_view.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) viewbench_i32.c $(LIB).a -o $@ $(LDLIBS) -lm

# A profiled, renamed copy of the engine (cp_*) so the zoo benches can report which route and leaf path
# logos took. (The four partition/bucket symbols used to be missing from RENAME and were renamed here only, which
# left every other two-copy bench, ledgerbench included, failing to link. They are in RENAME now.)
CPRENAME = $(call RENAME,cp)
cp.o: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_PROFILE $(CPRENAME) -c $(SRC) -o cp.o

# Order hidden in chaos (experiments/team/chaos-explorer.md): twenty realistic shapes, logos vs a hand-routed
# sort that is TOLD the structure, plus the route/leaf path logos took. Changes no engine code.
chaoshunt: chaoshunt.cpp cp.o $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) chaoshunt.cpp cp.o $(LIB).a -o $@ $(LDLIBS)

# Ceilings for disorder of SHORT REACH (FINDINGS 3af): cuts, in-place blocks + boundary repair, links. The zoo of
# twenty is included unchanged; the new shapes live in reachhunt.cpp. Paired rows, every output verified.
reachhunt: reachhunt.cpp chaoshunt.cpp cp.o $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) reachhunt.cpp cp.o $(LIB).a -o $@ $(LDLIBS)

# The sorted-prefix-plus-tail route: paired A/B in one binary switched by LOGOS_NO_TAIL.
prefixbench: prefixbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) prefixbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# The quadtree probe's tightened tail rule, scored against the old one on FRESH shapes it has never seen.
ztail: ztail.cpp zroute.cpp chaoshunt.cpp cp.o $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) ztail.cpp cp.o $(LIB).a -o $@ $(LDLIBS)

# Interleaved ascending streams: the stable-scatter route, paired A/B in one binary (LOGOS_NO_STREAMS).
streamsbench: streamsbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) streamsbench.c $(LIB).a -o $@ $(LDLIBS) -lm
streamsprobe: streamsprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) streamsprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
cardgate: cardgate.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) cardgate.c $(LIB).a -o $@ $(LDLIBS) -lm
zerodiag: zerodiag.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) zerodiag.c $(LIB).a -o $@ $(LDLIBS) -lm
kgbench: kgbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) kgbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# How much memory does one sort take? Peak traced heap of a single cold call (malloc family interposed, a fresh child
# process per cell), per shape, 1 thread and P, buffered and in-place, plus what is still held afterwards and what is
# left after logos_release_buffers(). Includes the shape that tests O(P n): one value holding most of the array.
# MEMPROBE_LOG=<bytes> ./memprobe 10000000 prints every allocation at least that large with its call site.
memprobe: memprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) memprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
	./memprobe

# What a sort keeps RESIDENT (VmHWM / VmRSS, a fresh process per row): peak during one sort, held after it, after 32 small sorts,
# after logos_release_buffers(). memprobe counts what is ASKED for; a page nobody touches is not resident. ./rssprobe [n] [P list] [shape]
# C strings: logos_sort_strings_mt (8-byte prefix keys) against std::sort / __gnu_parallel::sort with strcmp, the old multikey
# quicksort, and the same prefix scheme driven by std::sort and by x86-simd-sort's AVX2 key-value sort. Needs the clone.
# ./stringbench [n] [reps] [P list] [shape]
strsmallbench: strsmallbench.cpp bench_strings.h $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -I. strsmallbench.cpp $(LIB).a -o $@ $(LDLIBS)
stringbench: stringbench.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -mavx2 -mfma -mbmi2 -fopenmp -DXSS_USE_OPENMP -I. -I $(XSS) stringbench.cpp $(LIB).a -o $@ $(LDLIBS)

# logos_sort_f64_batch against a loop of logos_sort_f64 over the same segments, paired: ./batchbench [pairs] [total] [P list] [row]
batchbench: batchbench.c $(LIB).a $(HDR) bench_common.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) batchbench.c $(LIB).a -o $@ $(LDLIBS) -lm

rssprobe: rssprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) rssprobe.c $(LIB).a -o $@ $(LDLIBS) -lm

# What ONE process keeps resident across a mixed sequence of sorts (2026-10-24): rssprobe starts every row fresh and cannot see
# buffers that coexist (the regions beside the pool were 17-20 bytes per element held between calls). ./mixprobe [n]
mixprobe: mixprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) mixprobe.c $(LIB).a -o $@ $(LDLIBS) -lm

# How much dearer does ONE ELEMENT get from 2M to 50M? The scaling exponent p = 1 + ln(c_hi/c_lo) / ln(n_hi/n_lo), measured
# in PAIRS: one 50M sort against twenty-five 2M sorts of the same shape, adjacent in time, order alternating, so the
# machine drift that separates two rows of the scale report cancels. std::sort is the calibration (theory: p >= 1.062).
# ./slopebench 8 50000000 50000000 uniform is the null test: same size both sides, true growth 0.
slopebench: slopebench.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) slopebench.cpp $(LIB).a -o $@ $(LDLIBS)
	./slopebench

# The strict in-place sort in parallel (placement passes as tasks): what the threads buy against its own 1-thread form, and
# what zero scratch still costs against the buffered sort at the same thread count. Paired, verified, predictions in the header.
# ./ipbench 8 50000000 uniform is the large end; ./ipbench 24 1048576 is the parallel threshold.
ipbench: ipbench.c $(LIB).a $(HDR) bench_common.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) ipbench.c $(LIB).a -o $@ $(LDLIBS)
	./ipbench

# What happens when an allocation or a thread creation FAILS in the middle of a sort? malloc, calloc, realloc,
# posix_memalign and pthread_create are interposed; each run fails the k-th call made from the engine (or every call from
# the k-th on), in a fresh child process, and checks the output. Exit status 1 on any wrong output, crash or hang.
# ./faultprobe -c <shape> <threads> one|rest|spawn-one|spawn-rest <k> prints the failing call sites (addr2line -f -e faultprobe).
faultprobe: faultprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) -g faultprobe.c $(LIB).a -o $@ $(LDLIBS) -ldl
	./faultprobe

# The whole harness under sanitizers, correctness only (LOGOS_SKIP_BENCH=1). float-cast-overflow is NOT part of
# -fsanitize=undefined in GCC, and it is the one that found the NaN-to-index casts behind the infinity and subnormal
# defects. With a NaN in the data three such casts remain in the flash and coarse leaves, clamped and memory-safe; they
# are reported, not fatal. Leak checking is ON (the pools are reachable from statics, so only a real leak is reported).
# ThreadSanitizer needs address-space randomisation off on recent kernels (setarch -R); under it the harness keeps the
# thread-creation half of its fault section, forwarding to TSan's own interceptor, so the orphan paths run under the race detector.
SAN_SRC := $(SRC) $(SRC2) logos_view.c harness.c
sanitize: $(SAN_SRC) $(HDR)
	$(CC) -O1 -g -std=gnu11 -pthread -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer $(SAN_SRC) -o harness-asan $(LDLIBS)
	LOGOS_SKIP_BENCH=1 ASAN_OPTIONS=detect_leaks=1 ./harness-asan 2>harness-asan.log | tail -3; echo "sanitizer reports: $$(grep -c 'runtime error\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer' harness-asan.log) (see harness-asan.log)"
tsan: $(SAN_SRC) $(HDR)
	$(CC) -O1 -g -std=gnu11 -pthread -fsanitize=thread $(SAN_SRC) -o harness-tsan $(LDLIBS)
	LOGOS_SKIP_BENCH=1 TSAN_OPTIONS=halt_on_error=0 setarch $$(uname -m) -R ./harness-tsan 2>harness-tsan.log | tail -3; echo "data races: $$(grep -c 'WARNING: ThreadSanitizer' harness-tsan.log) (see harness-tsan.log)"

# The knot route: runs as strands, value-range overlaps as crossings, merges only where strands cross.
# Paired A/B in one binary (LOGOS_NO_KNOT = J5 as shipped). `./knotbench 20 8388608 lift` lifts the cost gate
# (LOGOS_KNOT_MAX=8) to find the break-even.
knotbench: knotbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) knotbench.c $(LIB).a -o $@ $(LDLIBS) -lm
# The same bench over a -DLOGOS_TRACE engine: `./knottrace 5 8388608 once` prints which route took each shape.
# A trace build answers WHETHER a path runs, never how fast: do not read times off it.
knottrace: knotbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE knotbench.c $(SRC) -o $@ $(LDLIBS) -lm

# The short-reach route (sort index blocks where they lie, repair only the seams): paired A/B in one binary, switched
# at runtime by LOGOS_NO_REACH. `./reachbench 40 8388608 - latency` reruns the rows whose name contains `latency`.
reachbench: reachbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) reachbench.c $(LIB).a -o $@ $(LDLIBS) -lm
# The same bench over a -DLOGOS_TRACE engine: `./reachtrace 5 8388608 once` prints what the gate decided for each shape.
reachtrace: reachbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE reachbench.c $(SRC) -o $@ $(LDLIBS) -lm

# The proposed Moebius DLL sort (experiments/moebius_dll/): the JavaScript translated to C as written, and its own pure-C form (twist
# and merge by relinking), paired against the engine at one thread. `./dllbench [pairs] [n]`; the JS side: node --expose-gc
# experiments/moebius_dll/bench.js. Predictions and stop rule in both headers.
dllbench: dllbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) dllbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# Two probes from experiments/TWIST_AND_KNOT.md: `./strandprobe 30 8388608 strays` = sorted blocks with late records against a sort TOLD
# which positions are strays (the ceiling for lifting strays out inside the run detector); `./strandprobe 0 8388608 writhe` = writhe and
# persistence from 2,048 strided points, scored against a rule fixed in advance.
strandprobe: strandprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) strandprobe.c $(LIB).a -o $@ $(LDLIBS) -lm

# Where else could the twist go? A probe, not a route: every cheap route's shape against its own MIRROR image (one orientation
# bit for the whole array), and descending teeth that reach the LEAF's run detector. Predictions and stop rule in its header.
# Reidemeister moves on the Moebius loop (Kauffman & Lambropoulou, Hard Unknots and Collapsing Tangles): `./unknotprobe [pairs] [n] ring`
# = a shape rotated as a ring buffer against itself through the engine (R0 on the sphere); `... moves` = a literal circular doubly
# linked list cut into strands, R1/R2/R3 as local rewrites, against the engine at one thread; `... count` = what the moves did.
# Server timestamps with slight knots (network jitter, retransmits, clock skew, batches, ring buffers, newest-first logs, sensor
# telemetry, ms clocks, late batches): logos against std::sort and against the boundary-bypass 'Moebius hybrid' an outside write-up
# described, plus logos with every order-reading route off. `./tsbench 20 8388608 anatomy` = what the hybrid's boundary check skips.
# "Treat uniform data as a complete computation": P chunks sorted by the AVX2 leaf and merged (every merge split by
# merge path) against the shipped partition. ./unibench [pairs] [n] [all | anat] [row filter]
# A thread ceiling for the counting routes (their serial merge grows with P), paired against the engine as it ships.
# ./capbench [pairs] [ceiling] [row filter]
# Where the parallel speedup stops and why: a thread ladder with the Karp-Flatt serial fraction per route.
# ./amprobe [reps] [n] [ladder]
amprobe: amprobe.c $(LIB).a $(HDR) bench_common.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) amprobe.c $(LIB).a -o $@ $(LDLIBS)
# Order must never cost (2026-10-20): 24 ordered shapes nobody had run, each against ITS OWN VALUES SHUFFLED, paired, at 1 and P
# threads; above 1 = the order made the sort slower. `./orderprobe 20 4194304 1,14`; `... ab` = OP_SWITCH against shipped;
# orderprobe_trace + `./orderprobe_trace 0 4194304 14 <filter> once` names the route each shape took.
orderprobe: orderprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) orderprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
orderprobe_trace: orderprobe.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE orderprobe.c $(SRC) -o $@ $(LDLIBS) -lm
# The second invented round (2026-10-20, FINDINGS 3bn): the new tests and the A/B benches of what they found.
# symprobe: an order-isomorphism (negate and reverse; x 2^20; x 2^-30) must not change the cost; symprobe_trace + `once` names routes.
# edgeprobe: one element across each size gate must not cost 10% more per element. rleprobe: runs of equal values, told (a ceiling)
# and `ab` (LOGOS_NO_RLE). tailladder / headladder: the tail and head merges' three forms across fractions. thinbench: the thin
# front's ceiling, moved at run time in a -DLOGOS_THIN_HOOK copy of the engine.
symprobe: symprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) symprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
symprobe_trace: symprobe.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE symprobe.c $(SRC) -o $@ $(LDLIBS) -lm
edgeprobe: edgeprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) edgeprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
rleprobe: rleprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) rleprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
tailladder: tailladder.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) tailladder.c $(LIB).a -o $@ $(LDLIBS) -lm
headladder: headladder.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) headladder.c $(LIB).a -o $@ $(LDLIBS) -lm
thinbench: thinbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_THIN_HOOK thinbench.c $(SRC) -o $@ $(LDLIBS) -lm
roundbench: roundbench.c $(LIB).a $(HDR) bench_common.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) roundbench.c $(LIB).a -o $@ $(LDLIBS)
capbench: capbench.c $(LIB).a $(HDR) bench_common.h
	$(CC) $(CFLAGS) $(BENCHFLAGS) capbench.c $(LIB).a -o $@ $(LDLIBS)
unibench: unibench.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread -march=native -DHAVE_XSS -I $(XSS) $(BENCHFLAGS) unibench.cpp $(LIB).a -o $@ $(LDLIBS)
tsbench: tsbench.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) tsbench.cpp $(LIB).a -o $@ $(LDLIBS)
tstrace: tsbench.cpp $(SRC) $(SRC2) logos_view.c $(HDR) bench_common.h
	$(CC) $(CFLAGS) -DLOGOS_TRACE -c $(SRC) -o tstrace_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o tstrace_extra.o && $(CC) $(CFLAGS) -c logos_view.c -o tstrace_view.o
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) tsbench.cpp tstrace_psort.o tstrace_extra.o tstrace_view.o -o $@ $(LDLIBS)

# Where the engine sits against the comparison lower bound, shape by shape (2026-10-31): per-element cost growth 1M -> 16M in
# adjacent pairs (a comparison sort grows +20%), std::sort as the calibration, cost against uniform, and the bits each input
# carries (repeats, runs, out-of-place). ./boundprobe [pairs] [n_lo] [n_hi] [shape]; boundprobe_trace + `once [n] [shape] [P]` names the routes.
# BP_A=NAME=VALUE BP_B=NAME=VALUE [BP_THREADS=P] ./boundprobe ab [pairs] [n list] [shape]: two engine settings on the same input,
# paired (default A = LOGOS_NO_LAT1=1, B = shipped), over a lattice ladder, spill shapes and guards. FINDINGS 3br
boundprobe: boundprobe.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) boundprobe.cpp $(LIB).a -o $@ $(LDLIBS)
boundprobe_trace: boundprobe.cpp $(SRC) $(SRC2) logos_view.c $(HDR) bench_common.h
	$(CC) $(CFLAGS) -DLOGOS_TRACE -c $(SRC) -o bptrace_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o bptrace_extra.o && $(CC) $(CFLAGS) -c logos_view.c -o bptrace_view.o
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) boundprobe.cpp bptrace_psort.o bptrace_extra.o bptrace_view.o -o $@ $(LDLIBS)

# X+Y, the sort on the quadratic line (2026-11-02): the N = n^2 pairwise sums, sorted blind by the engine and by a sort TOLD X and Y
# (staircase placement into buckets of S, then the engine per bucket); cost per element and its growth 1M -> 16M in adjacent pairs.
# ./xyprobe [pairs] [n list]. FINDINGS 3bs
xyprobe: xyprobe.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) xyprobe.cpp $(LIB).a -o $@ $(LDLIBS)

# A knot solver that unties only the PRIME FACTORS (2026-11-05, FINDINGS 3bv): pinned arcs (elements already in their final place,
# found from the run list by two binary searches per run) are never moved, the tangles between them are untied in place and
# independently. Standalone, paired against the engine at 1 and P threads. ./primeprobe [pairs] [n] [P] [row filter]
primeprobe: primeprobe.cpp $(LIB).a $(HDR) bench_common.h
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native $(BENCHFLAGS) primeprobe.cpp $(LIB).a -o $@ $(LDLIBS)

unknotprobe: unknotprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) unknotprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
unknottrace: unknotprobe.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE unknotprobe.c $(SRC) -o $@ $(LDLIBS) -lm

mirrorprobe: mirrorprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) mirrorprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
mirrortrace: mirrorprobe.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE mirrorprobe.c $(SRC) -o $@ $(LDLIBS) -lm

# The Moebius twist: run descriptors that carry their orientation, so a descending run is never reversed in memory.
# `./twistbench [pairs] [n] ceiling` prices the reversal passes (an array with descending blocks against the same array
# with them already ascending), which is the most the twist could save; `... ab` is the engine against itself under
# LOGOS_NO_TWIST=1; `... merge` is the first two-ended merge (one strided loop, LOGOS_TWIST_GENERIC=1) against one loop per
# orientation pair. One binary and one engine copy, so no layout term. Predictions, stop rule and ship rule are in its header.
twistbench: twistbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) twistbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# A sine wave with a slope (a seasonal metric with drift): the three invariants of the index -> value curve (monotone
# pieces, how many cover a value, reach inside a piece), and whether that structure COSTS anything. Every shape is
# paired against a uniform reference of the same size, adjacently, order alternated: the column is t_shape/t_uniform.
# Two controls decide whether a row means anything: a null (uniform against uniform) and linear + the same noise and
# no wave. `./sineprobe [pairs] [n]`. Its first cut compared absolute times across rows and had to be rewritten.
sineprobe: sineprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) sineprobe.c $(LIB).a -o $@ $(LDLIBS) -lm
	./sineprobe
# Which route each of those shapes takes, from a -DLOGOS_TRACE engine. Routes only: never read a time from it.
sinetrace: sineprobe.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE sineprobe.c $(SRC) -o $@ $(LDLIBS) -lm

# What ONE CALL costs at the small end (n = 0 to 50000, random and sorted): a baseline engine against the working tree, in two
# binaries with the copies swapped, layout bias cancelled. Every other A/B here starts at a million and cannot see a cost per call.
# The baseline is the committed engine, or any source: make callbench CB_BASE=/path/to/old_logos_psort.c. Run it after touching
# the first page of the dispatcher or of the leaf; read its header for what a row under ~15% does and does not mean.
CB_BASE ?=
# Where the lattice route's wall goes, phase by phase (open direction 1). lattrace reads the trace boundaries;
# the four PROF phases (map / merge / emit / teardown) are what `make profile` now splits it into.
lattrace: lattrace.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE lattrace.c $(SRC) -o $@ $(LDLIBS) -lm

# How much of a short parallel phase is thread creation and joining: the emit phase measured 3.17 of 14 busy
# with equal work per thread, and 35% of it is create+join of 13 workers.
spawnprobe: spawnprobe.c
	$(CC) $(CFLAGS) spawnprobe.c -o $@ $(LDLIBS) -lm

# The sphere-projection proposal, priced per element and then checked for ORDER (it inverts 22.2% of pairs).
sphereprobe: sphereprobe.c
	$(CC) $(CFLAGS) -march=native sphereprobe.c -o $@ -lm
invprobe: invprobe.c
	$(CC) $(CFLAGS) invprobe.c -o $@ -lm

# Does knowing the data FORMAT (dates) beat certifying the distribution? dateprobe prices six numeric date
# shapes against a sort TOLD the origin and step (it loses on all six); datestrprobe prices date TEXT, where
# the format collapses a 24-byte comparison to an 8-byte key (8.5x). datetrace names the route each one takes.
dateprobe: dateprobe.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) dateprobe.cpp $(LIB).a -o $@ $(LDLIBS) -lm
datestrprobe: datestrprobe.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) datestrprobe.cpp $(LIB).a -o $@ $(LDLIBS) -lm
datetrace: dateprobe.cpp $(SRC) $(HDR)
	$(CC) $(CFLAGS) -w -DLOGOS_TRACE -c $(SRC) -o dt_trace.o
	$(CC) $(CFLAGS) -w -c logos_extra.c -o dt_extra.o
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) dateprobe.cpp dt_trace.o dt_extra.o -o $@ $(LDLIBS) -lm

# What share of the wave route is the knot sweep? LOGOS_WAVE_NOSWEEP=1 runs everything up to the sweep and stops
# (output wrong by design, as ./unknotprobe nocopy does), alternating inside one process so drift cancels.
# The shape must be unknotprobe's gen_sine or the route declines and the ratio is 1.0 for the wrong reason --
# the probe's WITNESS line says whether it fired. ./waveprobe [n] [pairs] [cycles]
waveprobe: waveprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) -I. waveprobe.c $(LIB).a -o $@ $(LDLIBS)
	./waveprobe 8388608 15 32

# Does kinked data want more cores? The kink repair is already per-worker (detect_runs_parallel gives each thread
# its own chunk), so this asks whether the SORT wants the threads. Two shapes: 64 shuffled blocks + kinks, and
# `one` = ONE sorted strand + kinks. `once [P]` sorts once for a -DLOGOS_TRACE build to name the route.
# ./kinkladder [reps] [n] [one|once] [P]
kinkladder: kinkladder.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) -I. kinkladder.c $(LIB).a -o $@ $(LDLIBS)
	./kinkladder 9 8388608
	./kinkladder 9 8388608 one

# The reach probe's verdict and the kinks repaired at P = 1, 4, 8 on the three kinked-strand shapes (the harness's
# RM_SPARSE and RM_SPARSEENDS, kinkladder's one strand): the witness for the kink gate (2026-09-26). A verdict other
# than DECLINED_KINKS on any of them means the gate did not fire and the near-sorted route took the array.
# ./kgverdict [n] [shape 0|1|2]
kgverdict: kgverdict.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) -I. kgverdict.c $(LIB).a -o $@ $(LDLIBS)
	./kgverdict 300001

# The in-house AVX2 quicksort against x86-simd-sort's, KERNELS ALONE (2026-09-29): the library built -march=native and again
# -mavx2 -mfma (the engine's own target), sizes 64 to 8M, the partition count per element against the ideal. Two binaries:
# avkernbench (networks read the range directly) and avkernbench_staged (-DAV_NET_STAGED, the old staging buffer). ./avkernbench [pairs]
# -DAV_PREFETCH (the library's one-block-ahead prefetch) measured nothing: every ratio unchanged, 2026-09-29
avkernbench: avkernbench.cpp xss_base.cpp $(SRC) $(SRC2) $(HDR)
	$(CXX) -O3 -std=gnu++17 -mavx2 -mfma -I $(XSS) -c xss_base.cpp -o xss_base.o
	$(CC) $(CFLAGS) -DLOGOS_AV_HOOK -DAV_STATS -c $(SRC) -o avk_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o avk_extra.o
	$(CC) $(CFLAGS) -DLOGOS_AV_HOOK -DAV_STATS -DAV_NET_STAGED -c $(SRC) -o avk_psort_st.o
	$(CXX) -O3 -std=gnu++17 -march=native -I $(XSS) -I. avkernbench.cpp avk_psort.o avk_extra.o xss_base.o -o avkernbench $(LDLIBS)
	$(CXX) -O3 -std=gnu++17 -march=native -I $(XSS) -I. avkernbench.cpp avk_psort_st.o avk_extra.o xss_base.o -o avkernbench_staged $(LDLIBS)

# The vector quicksort's ORDER cost (2026-10-20): the in-house kernel and the library's, alone, each shape against its own values
# shuffled, with the in-house partition count (levels) on both. ./ordkern [pairs] [n]
ordkern: ordkern.cpp xss_base.cpp $(SRC) $(SRC2) $(HDR)
	$(CXX) -O3 -std=gnu++17 -mavx2 -mfma -I $(XSS) -c xss_base.cpp -o xss_base.o
	$(CC) $(CFLAGS) -DLOGOS_AV_HOOK -DAV_STATS -c $(SRC) -o ordk_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o ordk_extra.o
	$(CXX) -O3 -std=gnu++17 -march=native -I. ordkern.cpp ordk_psort.o ordk_extra.o xss_base.o -o ordkern $(LDLIBS)

# The other-fields round (2026-10-21, experiments/OTHER_FIELDS.md): ceilings and tests in one binary (small, hub, weave, moire,
# founder, landauer; see its header). The engine is built with -DLOGOS_AV_HOOK so the bare AVX2 kernel can be timed beside it: the
# hook adds one exported function and changes no path.
fieldprobe: fieldprobe.c $(SRC) $(SRC2) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_AV_HOOK -c $(SRC) -o fp_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o fp_extra.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) fieldprobe.c fp_psort.o fp_extra.o -o $@ $(LDLIBS) -lm
fieldprobe_trace: fieldprobe.c $(SRC) $(SRC2) $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) -DLOGOS_TRACE -DLOGOS_AV_HOOK fieldprobe.c $(SRC) $(SRC2) -o $@ $(LDLIBS) -lm

# What makes each sort fast (2026-09-28): logos, x86-simd-sort and IPS4o under five controlled sweeps (order, size,
# duplicates, distribution, threads), the three alternating on identical data, every output verified. There are no
# hardware counters here (make pmuprobe says so), so the mechanism is read from how time responds to ONE changed property.
# ./whyfast [n] [reps]
whyfast: whyfast.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native -DXSS_USE_OPENMP -I $(XSS) -I $(IPS4O) -I. whyfast.cpp $(LIB).a -o $@ $(LDLIBS) -latomic -ltbb
	./whyfast
pmuprobe: pmuprobe.c
	$(CC) -O1 -std=gnu11 pmuprobe.c -o $@
	-./pmuprobe

# The router's other half (2026-09-28): x86-simd-sort's quicksort as the SEQUENTIAL unstructured leaf, in place of the
# in-house copy of it. Two engine copies in the ledger bench, one built -DLOGOS_XSS_LEAF, the shim linked beside them;
# run it in BOTH orientations (XL0 / XL1 swapped) and read sqrt(A/B). Needs the clone: XSS=../x86-simd-sort/src.
#   make xssleafbench && ./xssleafbench 40 8000000
#   make -B xssleafbench XL0=-DLOGOS_XSS_LEAF XL1= && ./xssleafbench 40 8000000
# THE HYBRID'S PARALLEL HALF (unstructured data at low thread counts goes to the library's parallel quicksort): the gate
# is XSS_PAR_MAX. Ladder it lifted, per thread count, the 1-thread rows skipped by the fourth argument:
#   make -B xssleafbench XL0="-DLOGOS_XSS_LEAF -DXSS_PAR_MAX=0" XL1="-DLOGOS_XSS_LEAF -DXSS_PAR_MAX=64"
#   for P in 2 4 8 14; do LOGOS_THREADS=$$P ./xssleafbench 24 8000000 "" $$P; done     (and the copies swapped)
XL0 ?=
XL1 ?= -DLOGOS_XSS_LEAF
xssleafbench: ledgerbench.c logos_xss.cpp $(SRC) $(HDR)
	$(CXX) -O3 -std=gnu++17 -march=native -fPIC -fopenmp -DXSS_USE_OPENMP -I $(XSS) -c logos_xss.cpp -o logos_xss.o
	$(CC) $(CFLAGS) $(XL0) $(call RENAME,lg0) -c $(SRC) -o xl0.o
	$(CC) $(CFLAGS) $(XL1) $(call RENAME,lg1) -c $(SRC) -o xl1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) -c ledgerbench.c -o xlbench.o
	$(CXX) -fopenmp xlbench.o xl0.o xl1.o logos_xss.o -o $@ $(LDLIBS)

# What logos's PROFILE PHASE costs in front of the SIMD kernel (2026-10-05): the hybrid (-DLOGOS_XSS_LEAF) against the same
# library call with one NaN/-0.0 scan in front (the floor for unvouched data) and alone (the ceiling). One engine copy, the
# library reached through the same logos_xss.o, so there is no layout term between the library rows. Prediction and stop rule
# in frontprobe.cpp. ./frontprobe [pairs] [n] [threads] [shape filter] [ab]; frontprobe_inhouse = the same over the DEFAULT engine (in-house kernel)
frontprobe: frontprobe.cpp logos_xss.cpp $(SRC) $(HDR)
	$(CXX) -O3 -std=gnu++17 -march=native -fPIC -fopenmp -DXSS_USE_OPENMP -I $(XSS) -c logos_xss.cpp -o logos_xss.o
	$(CC) $(CFLAGS) -DLOGOS_XSS_LEAF $(FP_FLAGS) -c $(SRC) -o frontprobe_psort.o
	$(CXX) -O3 -std=gnu++17 -mavx2 -mfma $(BENCHFLAGS) -I. -c frontprobe.cpp -o frontprobe.o
	$(CXX) -fopenmp frontprobe.o frontprobe_psort.o logos_xss.o -o $@ $(LDLIBS)
	$(CC) $(CFLAGS) $(FP_FLAGS) -c $(SRC) -o frontprobe_ih_psort.o
	$(CXX) -fopenmp frontprobe.o frontprobe_ih_psort.o logos_xss.o -o frontprobe_inhouse $(LDLIBS)

# WHO IS FASTEST, cell by cell (2026-10-07): logos as shipped, logos with the library leaf (-DLOGOS_XSS_LEAF), x86-simd-sort,
# Google's VQSort (one thread only, since 2026-11-05), IPS4o and std::sort / __gnu_parallel::sort, interleaved on identical data,
# every output verified, 1 and P threads, seventeen shapes. The definition of "fastest" the engine is held to. ./scoreboard [n] [reps] [threads] [shape filter]
scoreboard: scoreboard.cpp logos_xss.cpp $(SRC) $(HDR) libvqsort.a
	$(CXX) -O3 -std=gnu++17 -march=native -fPIC -fopenmp -DXSS_USE_OPENMP -I $(XSS) -c logos_xss.cpp -o logos_xss.o
	$(CC) $(CFLAGS) $(SB0) $(call RENAME,lg0) -c $(SRC) -o sb0.o
	$(CC) $(CFLAGS) -DLOGOS_XSS_LEAF $(SB1) $(call RENAME,lg1) -c $(SRC) -o sb1.o
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native -DXSS_USE_OPENMP -I $(XSS) -I $(IPS4O) -I $(HWY) -I. scoreboard.cpp sb0.o sb1.o logos_xss.o libvqsort.a -o $@ $(LDLIBS) -latomic -ltbb

# Any engine SOURCE against the working tree at scale (2026-10-05; first used for the profile-phase levers against the engine
# as it was before them): ledgerbench's five shapes at 1 and BENCH_THREADS threads, two copies, built twice with the assignment
# swapped. frontledger_a measures t(base)/t(work), frontledger_b t(work)/t(base); the layout-free effect is sqrt(a / b).
#   make frontledger FL_BASE=/path/to/old_logos_psort.c && ./frontledger_a 80 8388608 && ./frontledger_b 80 8388608
FL_BASE ?=
frontledger: ledgerbench.c $(SRC) $(HDR)
	@test -n "$(FL_BASE)" || { echo "make frontledger FL_BASE=<an older logos_psort.c>"; exit 1; }
	cp "$(FL_BASE)" frontledger_base.gen.c
	$(CC) $(CFLAGS) -w $(call RENAME,lg0) -I. -c frontledger_base.gen.c -o fl_base0.o
	$(CC) $(CFLAGS) -w $(call RENAME,lg1) -I. -c frontledger_base.gen.c -o fl_base1.o
	$(CC) $(CFLAGS) $(call RENAME,lg0) -c $(SRC) -o fl_work0.o
	$(CC) $(CFLAGS) $(call RENAME,lg1) -c $(SRC) -o fl_work1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) -c ledgerbench.c -o fl_bench.o
	$(CC) fl_bench.o fl_base0.o fl_work1.o -o frontledger_a $(LDLIBS)
	$(CC) fl_bench.o fl_work0.o fl_base1.o -o frontledger_b $(LDLIBS)

# The two-strand merge kernels in isolation (2026-09-26): the branch-free scalar merge against the AVX2 bitonic merge, paired,
# every orientation pair, two sizes, every output checked. Links an engine object built -DLOGOS_MERGE_HOOK (the hook is not a
# public symbol of the library, so RENAME is untouched). End-to-end: ./knotbench 40 8388608 simd, and TS_SWITCH=LOGOS_NO_SIMD_MERGE
# ./tsbench 40 8388608 switch. ./mergebench [pairs] [n]
mergebench: mergebench.c $(SRC) $(SRC2) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_MERGE_HOOK -c $(SRC) -o mergebench_psort.o && $(CC) $(CFLAGS) -c $(SRC2) -o mergebench_extra.o
	$(CC) $(CFLAGS) -I. mergebench.c mergebench_psort.o mergebench_extra.o -o $@ $(LDLIBS)
	./mergebench 40 8388608

# The leaf's choice between the AVX2 quicksort and flash_sort, sized (2026-09-23). Both engine copies are
# TODAY's source; they differ only in -DLOGOS_AVX2_OVER_FLASH_FORCE, which restores the order that shipped
# before this round. Built twice with the copies swapped, so the layout bias cancels. See bandbench.c for
# why callbench cannot answer this (its shape never reaches the window).
#   make bandbench                                     the shipped comparison (the two orders)
#   make bandbench BB_OLD=-DX=a BB_NEW=-DX=b           any two leaf settings, e.g. the flash threshold:
#   make bandbench BB_OLD= BB_NEW=-DFLASH_SORT_THRESHOLD=512 then LOGOS_NO_AVX2=1 ./bandbench_b 0 bandbench_a.txt
BB_OLD ?= -DLOGOS_AVX2_OVER_FLASH_FORCE
BB_NEW ?=
bandbench: bandbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BB_OLD) $(call RENAME,bb0) -I. -c $(SRC) -o bb_old0.o
	$(CC) $(CFLAGS) $(BB_OLD) $(call RENAME,bb1) -I. -c $(SRC) -o bb_old1.o
	$(CC) $(CFLAGS) $(BB_NEW) $(call RENAME,bb0) -I. -c $(SRC) -o bb_new0.o
	$(CC) $(CFLAGS) $(BB_NEW) $(call RENAME,bb1) -I. -c $(SRC) -o bb_new1.o
	$(CC) $(CFLAGS) bandbench.c bb_old0.o bb_new1.o -o bandbench_a $(LDLIBS)
	$(CC) $(CFLAGS) bandbench.c bb_new0.o bb_old1.o -o bandbench_b $(LDLIBS)
	./bandbench_a 1 > bandbench_a.txt
	./bandbench_b 0 bandbench_a.txt

# The same two copies as bandbench, measured on BATCHES OF DISTINCT INPUTS (2026-10-20): bandbench repeats one input, which lets
# a predictor learn flash's branches. Twelve shapes, both orientations: read the geometric mean of bandbatch_a and bandbatch_b.
bandbatch: bandbatch.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BB_OLD) $(call RENAME,bb0) -I. -c $(SRC) -o bb_old0.o
	$(CC) $(CFLAGS) $(BB_OLD) $(call RENAME,bb1) -I. -c $(SRC) -o bb_old1.o
	$(CC) $(CFLAGS) $(BB_NEW) $(call RENAME,bb0) -I. -c $(SRC) -o bb_new0.o
	$(CC) $(CFLAGS) $(BB_NEW) $(call RENAME,bb1) -I. -c $(SRC) -o bb_new1.o
	$(CC) $(CFLAGS) bandbatch.c bb_old0.o bb_new1.o -o bandbatch_a $(LDLIBS) -lm
	$(CC) $(CFLAGS) bandbatch.c bb_new0.o bb_old1.o -o bandbatch_b $(LDLIBS) -lm

# twinbatch (2026-10-21): any compile-time switch paired on batches of DISTINCT inputs, the copies swapped between the two
# binaries (bandbatch's method, the question left open). make twinbatch TW_OLD=<flags> TW_NEW=<flags>; see its header.
TW_OLD ?=
TW_NEW ?= -DLOGOS_AVX2_INT32
twinbatch: twinbatch.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(TW_OLD) $(call RENAME,tw0) -I. -c $(SRC) -o tw_old0.o
	$(CC) $(CFLAGS) $(TW_OLD) $(call RENAME,tw1) -I. -c $(SRC) -o tw_old1.o
	$(CC) $(CFLAGS) $(TW_NEW) $(call RENAME,tw0) -I. -c $(SRC) -o tw_new0.o
	$(CC) $(CFLAGS) $(TW_NEW) $(call RENAME,tw1) -I. -c $(SRC) -o tw_new1.o
	$(CC) $(CFLAGS) twinbatch.c tw_old0.o tw_new1.o -o twinbatch_a $(LDLIBS) -lm
	$(CC) $(CFLAGS) twinbatch.c tw_new0.o tw_old1.o -o twinbatch_b $(LDLIBS) -lm

callbench: callbench.c $(SRC) $(HDR)
	@if [ -n "$(CB_BASE)" ]; then cp "$(CB_BASE)" callbench_base.gen.c; else git show HEAD:engines/c/$(SRC) > callbench_base.gen.c; fi
	$(CC) $(CFLAGS) -w $(call RENAME,cb0) -I. -c callbench_base.gen.c -o cb_base0.o
	$(CC) $(CFLAGS) -w $(call RENAME,cb1) -I. -c callbench_base.gen.c -o cb_base1.o
	$(CC) $(CFLAGS) $(call RENAME,cb0) -I. -c $(SRC) -o cb_work0.o
	$(CC) $(CFLAGS) $(call RENAME,cb1) -I. -c $(SRC) -o cb_work1.o
	$(CC) $(CFLAGS) callbench.c cb_base0.o cb_work1.o -o callbench_a $(LDLIBS)
	$(CC) $(CFLAGS) callbench.c cb_work0.o cb_base1.o -o callbench_b $(LDLIBS)
	./callbench_a 1 > callbench_a.txt
	./callbench_b 0 callbench_a.txt

# The near-sorted route's two gates, repaired 2026-09-20 (7 sampled pairs instead of ~1900; a pre-gate without the route's
# poison recovery): paired A/B in one binary, LOGOS_NS_GATE_OLD=1 restores the gates as they were.
nsgatebench: nsgatebench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) nsgatebench.c $(LIB).a -o $@ $(LDLIBS) -lm
# the near-sorted route's STRAND gate (FINDINGS 3bw): 27 shapes, the gate's statistics beside a paired timing of SG_SWITCH
# (default LOGOS_NO_NSSTRAND; LOGOS_NO_NEARSORTED = the whole route) against the shipped engine. ./strandgate [pairs] [n] [P] [filter]
strandgate: strandgate.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) strandgate.c $(LIB).a -o $@ $(LDLIBS) -lm

# Integrity check for zroute: the classifier as coded must reproduce the six shapes it was fitted to.
ztrain: ztrain.cpp zroute.cpp chaoshunt.cpp cp.o $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) ztrain.cpp cp.o $(LIB).a -o $@ $(LDLIBS)

# The (index, value) quadtree profile scored against chaoshunt's zoo, with the classifier fixed in advance.
zroute: zroute.cpp chaoshunt.cpp cp.o $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread $(BENCHFLAGS) zroute.cpp cp.o $(LIB).a -o $@ $(LDLIBS)

# Ledger re-run: two engine copies whose flags come from LB0/LB1, measured with the PAIRED harness
# so machine drift cancels, then run again with the assignment swapped so code-layout bias cancels
# too (tools/ledger_rerun.py drives both orientations). This is how a verdict reached against the
# old ~1.25x floor gets re-tested against the ~4% one. See engines/c/ledgerbench.c.
LB0 ?=
LB1 ?=
ledgerbench: ledgerbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LB0) $(call RENAME,lg0) -c $(SRC) -o lg0.o
	$(CC) $(CFLAGS) $(LB1) $(call RENAME,lg1) -c $(SRC) -o lg1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) ledgerbench.c lg0.o lg1.o -o $@ $(LDLIBS) -lm

# PROFILE-GUIDED BUILD, paired (2026-10-08): a copy instrumented with -fprofile-generate is trained by pgotrain (fourteen shapes,
# sizes 3 to 8M, 1 and 14 threads), rebuilt with -fprofile-use, and paired by ledgerbench against a plain copy, in BOTH orientations
# (the profile belongs to the copy it was taken from, so each orientation trains its own). effect = sqrt(pgobench_a / pgobench_b).
PGO_USE = -fprofile-use -fprofile-correction -Wno-missing-profile
pgobench: pgotrain.c ledgerbench.c $(SRC) $(HDR)
	rm -f lg0.gcda lg1.gcda
	$(CC) $(CFLAGS) -fprofile-generate -fprofile-update=atomic $(call RENAME,lg1) -c $(SRC) -o lg1.o
	$(CC) $(CFLAGS) -DPGO_COPY=lg1 pgotrain.c lg1.o -o pgotrain_1 -fprofile-generate $(LDLIBS) -lm
	./pgotrain_1
	$(MAKE) -B ledgerbench LB0= LB1="$(PGO_USE)"
	./ledgerbench $(PGO_PAIRS) 8000000 > pgobench_a.txt
	rm -f lg0.gcda lg1.gcda
	$(CC) $(CFLAGS) -fprofile-generate -fprofile-update=atomic $(call RENAME,lg0) -c $(SRC) -o lg0.o
	$(CC) $(CFLAGS) -DPGO_COPY=lg0 pgotrain.c lg0.o -o pgotrain_0 -fprofile-generate $(LDLIBS) -lm
	./pgotrain_0
	$(MAKE) -B ledgerbench LB0="$(PGO_USE)" LB1=
	./ledgerbench $(PGO_PAIRS) 8000000 > pgobench_b.txt
	python3 ../../tools/pair_orient.py pgobench_a.txt pgobench_b.txt
PGO_PAIRS ?= 30

# Leaf-phase A/B: flash class count and the flash-to-coarse threshold, four
# variants in ONE binary, interleaved. Run: make leafbench && ./leafbench
leafbench: leafbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(call RENAME,lb0) -c $(SRC) -o lb0.o
	$(CC) $(CFLAGS) -DFLASH_M_DIV=4 $(call RENAME,lb1) -c $(SRC) -o lb1.o
	$(CC) $(CFLAGS) -DFLASH_M_DIV=16 $(call RENAME,lb2) -c $(SRC) -o lb2.o
	$(CC) $(CFLAGS) -DCOARSE_MIN_N=16384 $(call RENAME,lb3) -c $(SRC) -o lb3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) leafbench.c lb0.o lb1.o lb2.o lb3.o -o $@ $(LDLIBS)

# Theory A/B: int narrowing, branch-free table leaf, bucket granularity; six variants
# in ONE binary, interleaved. Run: make theorybench && ./theorybench
theorybench: theorybench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_LOOKUP_BRANCHY -DBUCKETS_PER_THREAD=4 $(call RENAME,th0) -c $(SRC) -o th0.o
	$(CC) $(CFLAGS) -DLOGOS_LOOKUP_BRANCHY $(call RENAME,th1) -c $(SRC) -o th1.o
	$(CC) $(CFLAGS) -DLOGOS_LOOKUP_BRANCHLESS $(call RENAME,th2) -c $(SRC) -o th2.o
	$(CC) $(CFLAGS) $(call RENAME,th3) -c $(SRC) -o th3.o
	$(CC) $(CFLAGS) -DLOGOS_HIST_INTERLEAVE $(call RENAME,th4) -c $(SRC) -o th4.o
	$(CC) $(CFLAGS) -DLOGOS_LINE_REPAIR $(call RENAME,th5) -c $(SRC) -o th5.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) theorybench.c th0.o th1.o th2.o th3.o th4.o th5.o -o $@ $(LDLIBS)

# Chaos zoo A/B (experiments/ORDER_FROM_CHAOS.md): baseline vs each of four
# features on its own, fifteen shapes, 1 thread and P. Run: make chaosbench && ./chaosbench
CHAOS_OFF := -DLOGOS_STRIDE_SAMPLE -DLOGOS_NO_LOWDISC -DLOGOS_NO_MODE_TABLE
chaosbench: chaosbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CHAOS_OFF) $(call RENAME,ch0) -c $(SRC) -o ch0.o
	$(CC) $(CFLAGS) -DLOGOS_NO_LOWDISC -DLOGOS_NO_MODE_TABLE $(call RENAME,ch1) -c $(SRC) -o ch1.o
	$(CC) $(CFLAGS) -DLOGOS_STRIDE_SAMPLE -DLOGOS_NO_MODE_TABLE $(call RENAME,ch2) -c $(SRC) -o ch2.o
	$(CC) $(CFLAGS) -DLOGOS_STRIDE_SAMPLE -DLOGOS_NO_LOWDISC $(call RENAME,ch3) -c $(SRC) -o ch3.o
	$(CC) $(CFLAGS) $(CHAOS_OFF) -DAV_PIVOT_NET $(call RENAME,ch4) -c $(SRC) -o ch4.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) chaosbench.c ch0.o ch1.o ch2.o ch3.o ch4.o -o $@ $(LDLIBS)

# The counting routes: the lattice slot budget (runtime toggle, one binary) and the hash route's
# serial tail. Run: make mcbench && flock /tmp/logos-bench.lock ./mcbench 24
mcbench: mcbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) mcbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# Lattice window margin A/B. Run: make latbench && ./latbench
latbench: latbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLATTICE_MARGIN_OLD $(call RENAME,lt0) -c $(SRC) -o lt0.o
	$(CC) $(CFLAGS) $(call RENAME,lt1) -c $(SRC) -o lt1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) latbench.c lt0.o lt1.o -o $@ $(LDLIBS)

# Near-sorted route A/B. Run: make nsbench && ./nsbench
nsbench: nsbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NS_OLD $(call RENAME,ns0) -c $(SRC) -o ns0.o
	$(CC) $(CFLAGS) $(call RENAME,ns1) -c $(SRC) -o ns1.o
	$(CC) $(CFLAGS) -DLOGOS_PLAIN_INSERTION $(call RENAME,ns2) -c $(SRC) -o ns2.o
	$(CC) $(CFLAGS) -DLOGOS_NS_OLD -DLOGOS_PLAIN_INSERTION $(call RENAME,ns3) -c $(SRC) -o ns3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) nsbench.c ns0.o ns1.o ns2.o ns3.o -o $@ $(LDLIBS)

# Small-n policy A/B: the shipped thread policy against a higher per-thread minimum and a relocation
# per-thread minimum, at the P-thread call, with a 1-thread reference. Run: make smallbench && ./smallbench
SM0 ?= -DRELOC_MIN_PER_THREAD=1 -DCOUNT_MIN_PER_THREAD=1
SM1 ?= -DRELOC_MIN_PER_THREAD=500000 -DCOUNT_MIN_PER_THREAD=1
SM2 ?= -DRELOC_MIN_PER_THREAD=1 -DCOUNT_MIN_PER_THREAD=350000
SM3 ?=
SMNAMES ?= -DSM_SET=0
smallbench: smallbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SM0) $(call RENAME,sm0) -c $(SRC) -o sm0.o
	$(CC) $(CFLAGS) $(SM1) $(call RENAME,sm1) -c $(SRC) -o sm1.o
	$(CC) $(CFLAGS) $(SM2) $(call RENAME,sm2) -c $(SRC) -o sm2.o
	$(CC) $(CFLAGS) $(SM3) $(call RENAME,sm3) -c $(SRC) -o sm3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) $(SMNAMES) smallbench.c sm0.o sm1.o sm2.o sm3.o -o $@ $(LDLIBS)

# Inherited-knob sweep: four engines with different values of one JS-era constant (KB1..KB3 vs the
# baseline KB0), 17 shapes, 1 thread and P. Run: make knobbench KB1=-DX=.. KB2=.. KB3=.. && ./knobbench base v1 v2 v3
KB0 ?=
KB1 ?=
KB2 ?=
KB3 ?=
knobbench: knobbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(KB0) $(call RENAME,kb0) -c $(SRC) -o kb0.o
	$(CC) $(CFLAGS) $(KB1) $(call RENAME,kb1) -c $(SRC) -o kb1.o
	$(CC) $(CFLAGS) $(KB2) $(call RENAME,kb2) -c $(SRC) -o kb2.o
	$(CC) $(CFLAGS) $(KB3) $(call RENAME,kb3) -c $(SRC) -o kb3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) knobbench.c kb0.o kb1.o kb2.o kb3.o -o $@ $(LDLIBS)

# Histogram-free partition A/B: the old histogram + exact-offset scatter (-DLOGOS_NO_HISTFREE) against the
# shipped sample-sized regions with block reservation and spills, without the KS certificate, and with tighter
# slack. Run: make hfbench && ./hfbench
hfbench: hfbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_HISTFREE $(call RENAME,hf0) -c $(SRC) -o hf0.o
	$(CC) $(CFLAGS) $(call RENAME,hf1) -c $(SRC) -o hf1.o
	$(CC) $(CFLAGS) -DHF_NO_KS $(call RENAME,hf2) -c $(SRC) -o hf2.o
	$(CC) $(CFLAGS) -DHF_SLACK=0.3 $(call RENAME,hf3) -c $(SRC) -o hf3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) hfbench.c hf0.o hf1.o hf2.o hf3.o -o $@ $(LDLIBS)

# Sample-once dispatcher A/B: the old per-gate samples (-DLOGOS_NO_SODO) vs the shipped one two-stage sample.
# Run: make sodobench && ./sodobench
sodobench: sodobench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_SODO $(call RENAME,so0) -c $(SRC) -o so0.o
	$(CC) $(CFLAGS) $(call RENAME,so1) -c $(SRC) -o so1.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) sodobench.c so0.o so1.o -o $@ $(LDLIBS)

# Probe and context A/B: the per-call contexts and plain sample (both off) vs the fast sample (batched
# prefetch + AVX2 sort), vs the persistent context bundles, vs both (shipped). Run: make probebench && ./probebench
probebench: probebench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_NO_SAMPLE_FAST -DLOGOS_NO_CTX_POOL $(call RENAME,pb0) -c $(SRC) -o pb0.o
	$(CC) $(CFLAGS) -DLOGOS_NO_CTX_POOL $(call RENAME,pb1) -c $(SRC) -o pb1.o
	$(CC) $(CFLAGS) -DLOGOS_NO_SAMPLE_FAST $(call RENAME,pb2) -c $(SRC) -o pb2.o
	$(CC) $(CFLAGS) $(call RENAME,pb3) -c $(SRC) -o pb3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) probebench.c pb0.o pb1.o pb2.o pb3.o -o $@ $(LDLIBS)

# Low-cardinality lookup A/B/C. Four engines, flags overridable: CD0..CD3 and labels on the command line.
# Run: make cardbench && ./cardbench shipped phash lattice1st both
CD0 ?= -DLOGOS_NO_LC_PHASH
CD1 ?=
CD2 ?= -DLOWCARD_SAMPLE_MAX=128 -DLC_PHASH_BITS=13
CD3 ?= -DLOWCARD_SAMPLE_MAX=200 -DLC_PHASH_BITS=14
cardbench: cardbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CD0) $(call RENAME,cd0) -c $(SRC) -o cd0.o
	$(CC) $(CFLAGS) $(CD1) $(call RENAME,cd1) -c $(SRC) -o cd1.o
	$(CC) $(CFLAGS) $(CD2) $(call RENAME,cd2) -c $(SRC) -o cd2.o
	$(CC) $(CFLAGS) $(CD3) $(call RENAME,cd3) -c $(SRC) -o cd3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) cardbench.c cd0.o cd1.o cd2.o cd3.o -o $@ $(LDLIBS)

# Heavy-tail structure A/B: none of the three, key-space certificate, exponent-histogram map, both.
# Run: make tailbench && ./tailbench
TB0 ?= -DLOGOS_NO_KEY_KS
TB1 ?=
TB2 ?= -DLOGOS_NO_KEY_KS -DLOGOS_EXMAP
TB3 ?= -DLOGOS_EXMAP
tailbench: tailbench.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(TB0) $(call RENAME,tb0) -c $(SRC) -o tb0.o
	$(CC) $(CFLAGS) $(TB1) $(call RENAME,tb1) -c $(SRC) -o tb1.o
	$(CC) $(CFLAGS) $(TB2) $(call RENAME,tb2) -c $(SRC) -o tb2.o
	$(CC) $(CFLAGS) $(TB3) $(call RENAME,tb3) -c $(SRC) -o tb3.o
	$(CC) $(CFLAGS) $(BENCHFLAGS) tailbench.c tb0.o tb1.o tb2.o tb3.o -o $@ $(LDLIBS)

# What can this machine resolve today? Paired null test; read it before trusting any A/B under 1.25x.
pairprobe: pairprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) pairprobe.c $(LIB).a -o $@ $(LDLIBS)

# Leaf dispatch order: value order vs largest-first (longest-processing-time-first).
# One binary, switched by LOGOS_NO_LPT at runtime, measured with the paired harness.
lptbench: lptbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) lptbench.c $(LIB).a -o $@ $(LDLIBS) -lm

# What the int32 path knows (type: lattice of step 1) vs what it still asks. 1 thread vs P on a
# span ladder; where P/1 < 1 the parallel path is losing to one core for want of a counting route.
intbench: intbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) intbench.c $(LIB).a -o $@ $(LDLIBS)

# int32 paired A/B (experiments/team/i32-sentinel-seq.md): the sequential leaf's windowed counting, the
# parallel count route's table budget and the sequential counting gate, each a RUNTIME toggle so one
# binary measures both sides with no code-layout bias. Run: make i32abbench && ./i32abbench [pairs] [n] [A|B|C]
i32abbench: i32abbench.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) $(BENCHFLAGS) i32abbench.c $(LIB).a -o $@ $(LDLIBS)

# Per-phase profile -> profile-report.html (treemaps of thread-time by phase and
# leaf path, per-thread busy/stalled/idle bars, streaming rate vs memcpy). The
# engine is rebuilt with -DLOGOS_PROFILE into its own object; the normal library
# is untouched. Run: make profile      Overrides: LOGOS_THREADS, LOGOS_N.
logos_psort_prof.o: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DLOGOS_PROFILE -c $(SRC) -o $@
profbench: profbench.cpp logos_psort_prof.o $(OBJ2) $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp $(BENCHFLAGS) profbench.cpp logos_psort_prof.o $(OBJ2) -o $@ $(LDLIBS)
# The report is written here and, when the repo layout is present, copied to
# ../../reports/profile-report.html so it lives with the project (make clean
# removes only the local copy).
REPORTS := ../../reports
profile: profbench
	./profbench
	@if command -v node >/dev/null 2>&1; then node ../../tools/check_report.js profile-report.html | tail -1; else echo "(node not found: skipping the render check, tools/check_report.js)"; fi
	@if [ -d $(REPORTS) ]; then cp profile-report.html $(REPORTS)/profile-report.html && echo "copied to $(REPORTS)/profile-report.html"; fi
# Same report with x86-simd-sort's OpenMP sort as a third contender on the parallel rows
# (needs the header-only lib and an AVX-512 CPU, like parbench-simd).
profile-simd: profbench.cpp logos_psort_prof.o $(OBJ2) $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native -DHAVE_XSS -DXSS_USE_OPENMP -I $(XSS) $(BENCHFLAGS) profbench.cpp logos_psort_prof.o $(OBJ2) -o profbench $(LDLIBS)
	./profbench
	@if [ -d $(REPORTS) ]; then cp profile-report.html $(REPORTS)/profile-report.html && echo "copied to $(REPORTS)/profile-report.html"; fi
# Full profile: x86-simd-sort (AVX-512 where available, else its AVX2 kernels) and IPS4o
# (needs libtbb-dev) beside every cell. Clone both next to engines/c (XSS, IPS4O below).
profile-full: profbench.cpp logos_psort_prof.o $(OBJ2) $(HDR)
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native -DHAVE_XSS -DXSS_USE_OPENMP -DHAVE_IPS4O -I $(XSS) -I $(IPS4O) $(BENCHFLAGS) profbench.cpp logos_psort_prof.o $(OBJ2) -o profbench $(LDLIBS) -latomic -ltbb
	./profbench
	@if command -v node >/dev/null 2>&1; then node ../../tools/check_report.js profile-report.html | tail -1; fi
	@if [ -d $(REPORTS) ]; then cp profile-report.html $(REPORTS)/profile-report.html && echo "copied to $(REPORTS)/profile-report.html"; fi

# Time against n for every contender, every shape, 1T and P: scale-report.html. Sizes 0, 1, 3, 5, 10,
# then half-decades from 32 to LOGOS_MAX_N; below 10k a size is timed as a batch of distinct inputs.
# Per panel two charts: time per sort (linear, log-log toggle) and ns per element, whose value axis is
# ALWAYS linear. make scalebench = std::sort / gnu_par / logos; make scale-full adds x86-simd-sort,
# IPS4o and (2026-11-05) Google's VQSort at one thread (HWY=../highway, see libvqsort.a). The report opens with a summary
# written for a reader outside the project (logos against the fastest other sort at the largest n, the contenders, the
# method) and keeps the detailed charts below. Overrides: LOGOS_MAX_N (default 1e7 since 2026-09-23, for turnaround; =5e7 restores the old ceiling,
# whose 31.6M and 50M rows were most of the run), LOGOS_MIN_N (10000 = the old ladder), LOGOS_THREADS,
# LOGOS_MAX_SEC (default 20). LOGOS_SCALE_REUSE=<report.html> ./scalebench re-renders that report's
# measurements through the current template and measures nothing. The node and browser checks below
# are skipped where neither is installed (WSL here): run them from Windows, tools/check_report.js and
# tools/browser_check.py, on the written report. The charts zoom and pan (the template's IX block); both
# checkers DRIVE them, the second in a real browser, which is where two uPlot log-axis defects showed.
scalebench: scalebench.cpp $(LIB).a $(HDR) uplot_embed.h
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp $(BENCHFLAGS) scalebench.cpp $(LIB).a -o $@ $(LDLIBS)
	./scalebench
	@if command -v node >/dev/null 2>&1; then node ../../tools/check_report.js scale-report.html | tail -2; fi
	@if command -v python3 >/dev/null 2>&1; then python3 ../../tools/browser_check.py scale-report.html; elif command -v python >/dev/null 2>&1; then python ../../tools/browser_check.py scale-report.html; fi
	@if [ -d $(REPORTS) ]; then cp scale-report.html $(REPORTS)/scale-report.html && echo "copied to $(REPORTS)/scale-report.html"; fi
scale-full: scalebench.cpp $(LIB).a $(HDR) uplot_embed.h bench_strings.h libvqsort.a
	$(CXX) -O3 -std=gnu++17 -pthread -fopenmp -march=native -DHAVE_XSS -DXSS_USE_OPENMP -DHAVE_IPS4O -DHAVE_VQSORT -I $(XSS) -I $(IPS4O) -I $(HWY) $(BENCHFLAGS) scalebench.cpp $(LIB).a libvqsort.a -o scalebench $(LDLIBS) -latomic -ltbb
	./scalebench
	@if command -v node >/dev/null 2>&1; then node ../../tools/check_report.js scale-report.html | tail -2; fi
	@if command -v python3 >/dev/null 2>&1; then python3 ../../tools/browser_check.py scale-report.html; elif command -v python >/dev/null 2>&1; then python ../../tools/browser_check.py scale-report.html; fi
	@if [ -d $(REPORTS) ]; then cp scale-report.html $(REPORTS)/scale-report.html && echo "copied to $(REPORTS)/scale-report.html"; fi

# Measure the measurement: repeat one sort at several thread counts, report min/median/max,
# spread, preempted share and CPU utilisation. Run: make noiseprobe && ./noiseprobe [reps]
noiseprobe: noiseprobe.c $(LIB).a $(HDR)
	$(CC) $(CFLAGS) noiseprobe.c $(LIB).a -o $@ $(LDLIBS)

# Optional fair-fight benchmark vs std::sort and GCC's OpenMP __gnu_parallel::sort
# (needs g++ with OpenMP; not part of `all`). Run: make parbench && ./parbench
parbench: parbench.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -fopenmp $(BENCHFLAGS) parbench.cpp $(LIB).a -o $@ $(LDLIBS)

# Same fight, plus an Intel x86-simd-sort (AVX-512) column. Needs the header-only
# library and an AVX-512 CPU. Fetch it, then: make parbench-simd XSS=/path/to/x86-simd-sort/src
XSS ?= ../x86-simd-sort/src
parbench-simd: parbench.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -march=native -fopenmp -DHAVE_XSS -DXSS_USE_OPENMP -I $(XSS) $(BENCHFLAGS) parbench.cpp logos_simd.cpp $(LIB).a -o parbench $(LDLIBS)

# Full fair fight: adds IPS4o (the SOTA parallel comparison sort). Needs x86-simd-sort,
# IPS4o headers, and TBB. Set XSS and IPS4O to the include dirs.
IPS4O ?= ../ips4o/include
# Google's VQSort, the vectorized quicksort in the Highway library (google/highway, Apache-2.0), 2026-11-05. Fetch it beside the
# other opponents: git clone --depth 1 --branch 1.4.0 https://github.com/google/highway.git ../highway. There is no cmake on the
# reference box, so the eight translation units the sort needs (the dispatcher, the f64 and 128-bit key-value instantiations,
# run-time target detection) are compiled straight into libvqsort.a, about 25 s. Dynamic dispatch picks the instruction set at run
# time: AVX2 here, the same class as x86-simd-sort's kernels on this box. VQSort has no multi-threaded form, so the benches use it
# at one thread only.
HWY ?= ../highway
VQ_SRC := $(addprefix $(HWY)/hwy/,abort.cc targets.cc per_target.cc print.cc aligned_allocator.cc) \
          $(addprefix $(HWY)/hwy/contrib/sort/,vqsort.cc vqsort_f64a.cc vqsort_kv128a.cc)
libvqsort.a: $(VQ_SRC)
	rm -f vq_*.o
	for f in $(VQ_SRC); do $(CXX) -O2 -std=gnu++17 -fPIC -I $(HWY) -c $$f -o vq_$$(basename $$f .cc).o || exit 1; done
	ar rcs $@ vq_*.o
parbench-full: parbench.cpp $(LIB).a $(HDR)
	$(CXX) -O3 -std=gnu++17 -march=native -fopenmp -DHAVE_XSS -DXSS_USE_OPENMP -DHAVE_IPS4O -I $(XSS) -I $(IPS4O) $(BENCHFLAGS) parbench.cpp logos_simd.cpp $(LIB).a -o parbench $(LDLIBS) -latomic -ltbb

example: example-bin
	./example-bin

install: $(LIB).a $(LIB).so $(HDR)
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(HDR) $(DESTDIR)$(PREFIX)/include/
	install -m 644 $(LIB).a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(LIB).so $(DESTDIR)$(PREFIX)/lib/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/include/$(HDR) $(DESTDIR)$(PREFIX)/lib/$(LIB).a $(DESTDIR)$(PREFIX)/lib/$(LIB).so

dist: clean
	mkdir -p logos-sort-c-$(VERSION)
	cp $(SRC) $(SRC2) logos_simd.cpp $(HDR) harness.c example.c parbench.cpp pfbench.c j5bench.c stbench.c leafbench.c theorybench.c profbench.cpp scalebench.cpp noiseprobe.c bench_common.h scaling.cpp Makefile README.md IMPLEMENTING.md CHANGELOG.md PARITY.md LICENSE .gitignore README.md IMPLEMENTING.md CHANGELOG.md LICENSE .gitignore logos-sort-c-$(VERSION)/
	cp -r wasm logos-sort-c-$(VERSION)/
	tar czf logos-sort-c-$(VERSION).tar.gz logos-sort-c-$(VERSION)
	rm -rf logos-sort-c-$(VERSION)

clean:
	rm -f reachhunt reachbench reachtrace nsgatebench sineprobe sinetrace twistbench mirrorprobe strandprobe dllbench mirrortrace callbench_a callbench_b callbench_a.txt callbench_base.gen.c $(OBJ) $(OBJ2) $(LIB).a $(LIB).so harness example-bin parbench pfbench j5bench stbench st0.o st1.o leafbench lb0.o lb1.o lb2.o lb3.o theorybench th0.o th1.o th2.o th3.o th4.o th5.o intbench i32abbench scalebench scale-report.html noiseprobe profbench logos_psort_prof.o profile-report.html scaling scaling-report.html pf0.o pf16.o pf32.o pf64.o pf128.o rl0.o rl1.o logos-sort-c-*.tar.gz prefixbench streamsbench streamsprobe cardgate zerodiag kgbench knotbench knottrace memprobe faultprobe slopebench ipbench harness-asan harness-tsan harness-asan.log harness-tsan.log
