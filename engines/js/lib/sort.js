module.exports = (() => {
// ============================================================
// LogosAdaptive Sort (buffered engine)
// ------------------------------------------------------------
// Numbers: counting / int32-radix / insertion / 4-way hybrid merge /
//          2-way galloping merge / momentum insertion /
//          multimodal gap-partition + recurse /
//          coarse 2-level value-bucket sort / dual-pivot introsort / heapsort.
// Strings: TimSort-style run detection + merge (probe-gated) /
//          MSD-at-depth radix (after common-prefix skip) /
//          insertion sort for small ranges.
//
// Float path — coarse 2-level value-bucketing, size-gated against flash sort.
// A cache-driven crossover governs the choice: below ~500k elements flash's
// single n-bucket scatter stays cache-resident and is faster; above it the
// n-bucket count array exceeds cache and coarse's K=2048 cache-resident buckets
// win. So the dispatcher uses flash for small, flash-safe inputs and coarse for
// large inputs (and for small inputs flash handles poorly — near-duplicate /
// skew — which the IQR check routes to coarse too). Coarse scatters into cache-
// resident buckets and recurses, recomputing min/max per sub-range so it adapts
// to skew and near-duplicates where single-level flash degrades. quicksort is
// the within-bucket fallback at the depth cap (O(n log n) worst case).
//
// Merge fast path — predictive free-merge in mergeGallop / merge4WayHybrid /
// mergeStr: before merging two adjacent sorted runs, if the right run's first
// element is already >= the left run's last element the runs are in merged
// order, so return without copying.
//
// String path — a single unified MSD-at-depth algorithm: compute the common
// prefix once, then MSD radix one character at a time from that depth (each
// pass splits into 256 buckets in O(n), equivalent to ~8 levels of multikey
// QS's 3-way partition), recurse on each non-empty bucket, insertion-sort <=16.
//
// Auxiliary memory: pool 12 bytes/n (numeric paths). String MSD-at-depth
// allocates one Array(n) scratch + small Int32Array(258) counts per recursive
// call; multimodal partition recurses via O(n) slices. Otherwise in-place.
// ============================================================


const pool = {
  cap: 0,
  u32A: null,          // Uint32Array(2*cap) — int32-radix src + alias backing for f64Buf
  u32B: null,          // Uint32Array(cap+1) — int32-radix dst AND flash counts
  f64Buf: null,        // Float64Array view of u32A — flash/coarse scatter AND merge buf
  counts: null,        // Uint32Array view of u32B — flash sort counts
  idx: null,           // Uint16Array view of u32B — coarse bucket-index cache (u32B is idle on the coarse path)
};
// 4 byte-histograms for the int32 LSD radix (low->high byte). The float radix
// (8 histograms) was removed as dead code, halving the fixed histogram memory.
const HIST0 = new Uint32Array(257), HIST1 = new Uint32Array(257);
const HIST2 = new Uint32Array(257), HIST3 = new Uint32Array(257);

// Per-depth bucket buffers for the coarse 2-level value-bucket sort (float
// path). K cache-resident buckets per level keep the scatter targets in
// L1/L2; recursion handles within-bucket order. Constant memory (~200KB),
// allocated once. Depth is tiny in practice (2-3); the cap is a safety belt.
const COARSE_K = 2048, COARSE_MAXD = 4, COARSE_SMALL = 64;
const COARSE_C = [], COARSE_S = [], COARSE_H = [];
for (let d = 0; d < COARSE_MAXD; d++) {
  COARSE_C.push(new Int32Array(COARSE_K));
  COARSE_S.push(new Int32Array(COARSE_K + 1));
  COARSE_H.push(new Int32Array(COARSE_K));
}

function ensurePool(n) {
  if (n <= pool.cap) return;
  const cap = Math.max(n, pool.cap * 2 || 1024);
  pool.u32A = new Uint32Array(2 * cap);   // 2*cap u32 = cap f64 (f64Buf); also holds radix src[0..cap)
  pool.u32B = new Uint32Array(cap + 1);   // radix dst[0..cap) AND flash counts[0..cap]
  pool.f64Buf = new Float64Array(pool.u32A.buffer, 0, cap);
  pool.counts = new Uint32Array(pool.u32B.buffer, 0, cap + 1);
  pool.idx = new Uint16Array(pool.u32B.buffer, 0, cap);  // shares u32B; valid since coarse never uses counts
  pool.cap = cap;
}

const sortNumbersImpl = (() => {
  const INSERTION_SORT_THRESHOLD = 24;
  // Counting fires only when span <= COUNTING_SORT_K * length. A regret audit
  // measured the counting-vs-radix crossover at span ~1.2n (counting wins
  // below, radix above by up to ~1.9x at 4n). The prior value of 4 misrouted
  // the whole (1.3n, 4n] band to counting; 1 matches the crossover, and erring
  // low is safe (misrouting to radix costs ~1.16x at the boundary vs up to 2x
  // the other way). Also halves counting's worst-case bucket allocation.
  const COUNTING_SORT_K          = 1;
  const NEARLY_SORTED_INV_RATIO  = 0.05;
  const MOMENTUM_THRESHOLD       = 50_000;
  const MAX_RUNS_FOR_MERGE       = 32;
  // The run-merge path also fires for many-run data when the runs are LONG
  // (average length >= this): long natural runs let the galloping merge skip
  // their in-order stretches, so a sorted array with sparse far-displaced
  // disruptions (which makes ~n/avglen runs and would otherwise hit momentum
  // insertion at O(n*displacement)) merges in ~one effective pass. Short-run
  // data (local jitter, random) stays below this and routes to insertion or a
  // distribution sort, where a log(R)-pass merge would lose.
  const MIN_AVG_RUN_FOR_MERGE    = 256;
  // Momentum (large-n) insertion is only safe for genuine local jitter, where
  // every element is a short hop from home (total displacement ~ O(n)). That
  // regime has very short natural runs. If the run detector bailed with an
  // average run length above this, the data has sparse far-displaced
  // disruptions (a few elements moved a long way) — momentum would be
  // O(n*displacement) and explode — so route it to a distribution sort.
  const MOMENTUM_MAX_AVG_RUN     = 8;
  const FLASH_SORT_THRESHOLD     = 4096;
  const FLASH_SAFETY_RATIO       = 0.05;
  const FLASH_SHIFT_BUDGET       = 16;        // flash sort's closing insertion pass bails to the coarse sort past this many shifted slots per element
  // Below this length flash sort's single n-bucket scatter stays cache-resident
  // and beats coarse bucketing (up to ~2.9x at 100k); above it the n-bucket
  // count array exceeds cache and coarse's K-bucket scatter wins (audit-derived
  // crossover ~600k; set conservatively in flash's clear-win zone).
  const COARSE_MIN_N             = 500_000;
  const MIN_GALLOP               = 7;
  const INT32_MIN = -2147483648, INT32_MAX = 2147483647;

  // Plain insertion sort over a[lo..hi]. The universal small-range / leaf
  // finisher: every other path falls back here once a sub-range is small enough
  // that O(n^2) on it is cheaper than another distribution pass. Stable, in place.
  function insertionSort(a, lo, hi) {
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k;
    }
  }
  // Plain insertion sort with the momentum sort's budget: true if it finished, false once the shifts pass 8n, leaving a
  // valid permutation for a distribution sort. Below MOMENTUM_THRESHOLD the nearly-sorted branch used the UNBUDGETED
  // plain sort: 45,000 sorted elements with 1% random swaps took 47 ms, 20x the 2.3 ms they take now, and noise of +-400
  // took 24 ms (2026-09-20). The galloping momentum sort was tried here first and lost 20-40% on displacements of a few
  // places, which is what this branch mostly sees; plain shifts are the cheapest there is when the hop is short.
  function insertionSortBudget(a, lo, hi) {
    const budget = 8 * (hi - lo + 1); let work = 0;
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      if (a[j] <= k) continue;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k; work += i - 1 - j;
      if (work > budget) return false;
    }
    return true;
  }
  // "Momentum" insertion sort for large, nearly-sorted data with only local
  // jitter. Plain insertion walks left one slot at a time; this remembers how far
  // the previous element had to travel ("momentum"), gallops left by that distance
  // in exponentially shrinking/growing steps, then binary-searches the exact slot.
  // When every element is a short hop from home, total work stays ~O(n). The
  // dispatcher only sends genuine local jitter here — far-displaced disruptions
  // would make it O(n*displacement), so those are routed to a distribution sort.
  function insertionSortMomentum(a, lo, hi) {
    // Returns true if it completed, false if it aborted because total shift work
    // exceeded ~8n — the signal that the data is NOT actually near-sorted (e.g.
    // disorder concentrated in a sub-block that fooled the inversion sample and
    // the prefix-derived run-length estimate). Genuine near-sorted data moves each
    // element O(1) and never approaches the cap; the counter is free on that path.
    // On abort the caller re-sorts the (partially permuted, still valid) range with
    // a distribution sort, turning the old O(n*displacement) cliff into O(n).
    const budget = 8 * (hi - lo + 1);
    let work = 0, momentum = 1;
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i];
      if (a[i - 1] <= k) { momentum = momentum > 1 ? (momentum >>> 1) : 1; continue; }
      let j = i - 1, step = momentum;
      while (step > 1 && (j - step < lo || a[j - step] <= k)) step >>>= 1;
      while (j - step >= lo && a[j - step] > k) { j -= step; step <<= 1; }
      let left = Math.max(lo, j - step), right = j;
      while (left < right) { const mid = (left + right) >>> 1; if (a[mid] > k) right = mid; else left = mid + 1; }
      const dist = i - left;
      work += dist;
      if (work > budget) return false;          // not near-sorted -> abort to distribution sort
      for (let p = i; p > left; p--) a[p] = a[p - 1];
      a[left] = k; momentum = dist;
    }
    return true;
  }
  // Restore the max-heap property by sifting the element at `root` down its
  // subtree (children at 2i+1, 2i+2). `base` offsets into a[lo..]. Helper for heapSort.
  function siftDown(a, base, root, end) {
    for (;;) {
      let big = root;
      const l = 2*root+1, r = l+1;
      if (l < end && a[base+l] > a[base+big]) big = l;
      if (r < end && a[base+r] > a[base+big]) big = r;
      if (big === root) return;
      const t = a[base+root]; a[base+root] = a[base+big]; a[base+big] = t;
      root = big;
    }
  }
  // In-place heapsort over a[lo..hi]: build a max-heap bottom-up, then repeatedly
  // swap the max to the end and re-sift the shrinking heap. Guaranteed O(n log n);
  // used only as quicksort's escape hatch when the depth budget is exhausted.
  function heapSort(a, lo, hi) {
    const len = hi - lo + 1;
    for (let i = (len>>1)-1; i >= 0; i--) siftDown(a, lo, i, len);
    for (let i = len - 1; i > 0; i--) {
      const t = a[lo]; a[lo] = a[lo+i]; a[lo+i] = t;
      siftDown(a, lo, 0, i);
    }
  }
  // Branchy sorting network that orders 5 elements in place. Used to choose the
  // two pivots for dual-pivot quicksort from 5 evenly spaced samples.
  function sort5(a, i1, i2, i3, i4, i5) {
    if (a[i2] < a[i1]) { const t = a[i2]; a[i2] = a[i1]; a[i1] = t; }
    if (a[i3] < a[i2]) { const t = a[i3]; a[i3] = a[i2]; a[i2] = t;
      if (a[i2] < a[i1]) { const u = a[i2]; a[i2] = a[i1]; a[i1] = u; } }
    if (a[i4] < a[i3]) { const t = a[i4]; a[i4] = a[i3]; a[i3] = t;
      if (a[i3] < a[i2]) { const u = a[i3]; a[i3] = a[i2]; a[i2] = u;
        if (a[i2] < a[i1]) { const v = a[i2]; a[i2] = a[i1]; a[i1] = v; } } }
    if (a[i5] < a[i4]) { const t = a[i5]; a[i5] = a[i4]; a[i4] = t;
      if (a[i4] < a[i3]) { const u = a[i4]; a[i4] = a[i3]; a[i3] = u;
        if (a[i3] < a[i2]) { const v = a[i3]; a[i3] = a[i2]; a[i2] = v;
          if (a[i2] < a[i1]) { const w = a[i2]; a[i2] = a[i1]; a[i1] = w; } } } }
  }
  // Dual-pivot introsort over a[lo..hi]; `d` is the remaining recursion-depth
  // budget and at 0 it bails to heapsort (the introsort guard against O(n^2)).
  // Sorts 5 samples and, if they're distinct, uses two as pivots for a 3-region
  // partition (<p1 | between | >p2); otherwise falls back to a single 3-way
  // Dutch-flag partition that fast-paths duplicate-heavy data. Recurses on the
  // smaller side and loops on the larger to bound the stack; small ranges finish
  // with insertionSort. This is the general comparison / within-bucket fallback.
  function quicksort(a, lo, hi, d) {
    while (hi - lo >= INSERTION_SORT_THRESHOLD) {
      if (d === 0) { heapSort(a, lo, hi); return; }
      const len = hi - lo + 1;
      const seventh = (len >> 3) + (len >> 6) + 1;
      const e3 = (lo + hi) >> 1;
      const e2 = e3 - seventh, e4 = e3 + seventh;
      const e1 = e2 - seventh, e5 = e4 + seventh;
      sort5(a, e1, e2, e3, e4, e5);
      if (a[e1] !== a[e2] && a[e2] !== a[e3] && a[e3] !== a[e4] && a[e4] !== a[e5]) {
        const p1 = a[e2], p2 = a[e4];
        a[e2] = a[lo]; a[e4] = a[hi];
        let less = lo + 1, great = hi - 1, k = less;
        while (k <= great) {
          const ak = a[k];
          if (ak < p1) { a[k] = a[less]; a[less++] = ak; }
          else if (ak > p2) {
            while (k < great && a[great] > p2) great--;
            const ag = a[great];
            a[k] = ag; a[great--] = ak;
            if (ag < p1) { a[k] = a[less]; a[less++] = ag; }
          }
          k++;
        }
        a[lo] = a[less - 1]; a[less - 1] = p1;
        a[hi] = a[great + 1]; a[great + 1] = p2;
        d--;
        quicksort(a, lo, less - 2, d);
        quicksort(a, great + 2, hi, d);
        if (p1 === p2) return;
        let mLo = less, mHi = great;
        while (mLo <= mHi && a[mLo] === p1) mLo++;
        while (mLo <= mHi && a[mHi] === p2) mHi--;
        lo = mLo; hi = mHi;
      } else {
        const pv = a[e3];
        let lt = lo, gt = hi, k = lo;
        while (k <= gt) {
          const v = a[k];
          if (v < pv)      { const t = a[lt]; a[lt] = v; a[k] = t; lt++; k++; }
          else if (v > pv) { const t = a[gt]; a[gt] = v; a[k] = t; gt--;       }
          else             { k++; }
        }
        d--;
        if (lt - lo < hi - gt) { quicksort(a, lo, lt - 1, d); lo = gt + 1; }
        else                   { quicksort(a, gt + 1, hi, d); hi = lt - 1; }
      }
    }
    insertionSort(a, lo, hi);
  }
  // LSD radix sort for int32-range integers (the integer path when the value
  // span is too wide for counting sort). Values are biased to unsigned so byte
  // order matches signed order, then sorted one byte at a time, least-significant
  // first, with stable counting passes. Three things are folded in: the +BIAS
  // copy-in is fused into the histogram pass; a single OR/AND over all values
  // flags bytes that are constant array-wide so those scatter passes are skipped
  // (a constant byte carries no ordering); and the passes are unrolled/monomorphic
  // so V8 keeps them fast (a generic byte-indexed loop regressed full-range data).
  function lsdRadixInt32(a, n) {
    ensurePool(n);
    const BIAS = 0x80000000;
    const src = pool.u32A, dst = pool.u32B;
    HIST0.fill(0); HIST1.fill(0); HIST2.fill(0); HIST3.fill(0);
    const c0 = HIST0, c1 = HIST1, c2 = HIST2, c3 = HIST3;
    // Fused copy-in + histogram + constant-byte detection (OR/AND of all values).
    let orA = 0, andA = 0xFFFFFFFF;
    for (let i = 0; i < n; i++) {
      const v = (a[i] + BIAS) >>> 0; src[i] = v; orA |= v; andA &= v;
      c0[(v & 0xFF) + 1]++; c1[((v >>> 8) & 0xFF) + 1]++;
      c2[((v >>> 16) & 0xFF) + 1]++; c3[((v >>> 24) & 0xFF) + 1]++;
    }
    // A byte that is identical across every element contributes no ordering;
    // skip its whole scatter pass (free to detect, free to act). Narrow-range
    // integers (IDs, timestamps, counts) commonly use only 2-3 of the 4 bytes.
    const diff = (orA ^ andA) >>> 0;
    const a0 = (diff & 0xFF) !== 0, a1 = (diff & 0xFF00) !== 0;
    const a2 = (diff & 0xFF0000) !== 0, a3 = (diff & 0xFF000000) !== 0;
    // The last active pass writes (value - BIAS) straight into a[], fusing the
    // un-bias copy-out into the final scatter (one fewer full pass; ~+30% on
    // full-range int32, neutral on narrow ranges that already skipped passes).
    const last = a3 ? 3 : a2 ? 2 : a1 ? 1 : a0 ? 0 : -1;
    if (last < 0) { for (let i = 0; i < n; i++) a[i] = src[i] - BIAS; return; } // all-equal (defensive)
    let from = src, to = dst, tmp;
    if (a0) { for (let b = 1; b < 257; b++) c0[b]+=c0[b-1];
      if (last === 0) { for (let i = 0; i < n; i++) { const v = from[i]; a[c0[v & 0xFF]++] = v - BIAS; } return; }
      for (let i = 0; i < n; i++) { const v = from[i]; to[c0[v & 0xFF]++] = v; } tmp = from; from = to; to = tmp; }
    if (a1) { for (let b = 1; b < 257; b++) c1[b]+=c1[b-1];
      if (last === 1) { for (let i = 0; i < n; i++) { const v = from[i]; a[c1[(v >>> 8) & 0xFF]++] = v - BIAS; } return; }
      for (let i = 0; i < n; i++) { const v = from[i]; to[c1[(v >>> 8) & 0xFF]++] = v; } tmp = from; from = to; to = tmp; }
    if (a2) { for (let b = 1; b < 257; b++) c2[b]+=c2[b-1];
      if (last === 2) { for (let i = 0; i < n; i++) { const v = from[i]; a[c2[(v >>> 16) & 0xFF]++] = v - BIAS; } return; }
      for (let i = 0; i < n; i++) { const v = from[i]; to[c2[(v >>> 16) & 0xFF]++] = v; } tmp = from; from = to; to = tmp; }
    for (let b = 1; b < 257; b++) c3[b]+=c3[b-1]; // a3 is the last active byte here
    for (let i = 0; i < n; i++) { const v = from[i]; a[c3[(v >>> 24) & 0xFF]++] = v - BIAS; }
  }
  // Cheap safety probe for flash sort: take 64 strided samples, sort them, and
  // check that the interquartile span is a meaningful fraction of the full range.
  // If the middle 50% is squashed into almost no range (heavy skew / near-dups),
  // flash's linear bucketing would pile most values into a few buckets, so the
  // caller routes such inputs to the coarse bucket sort instead.
  function isFlashSafe(a, n, min, max) {
    const SAMPLE_SIZE = 64;
    const stride = (n / SAMPLE_SIZE) | 0;
    const samples = new Array(SAMPLE_SIZE);
    for (let i = 0; i < SAMPLE_SIZE; i++) samples[i] = a[i * stride];
    samples.sort((x, y) => x - y);
    return (samples[47] - samples[16]) / (max - min) >= FLASH_SAFETY_RATIO;
  }
  // Flash sort: single-pass distribution sort for well-spread float data. Map
  // each value linearly onto one of n buckets by (v-min)/range, counting-sort into
  // them with one scatter, then a near-linear insertion pass cleans up the small
  // local disorder interpolation leaves. Fast while the n-bucket count array stays
  // cache-resident (small n); above ~500k the dispatcher prefers coarse bucketing.
  function flashSort(a, n, min, max) {
    ensurePool(n);
    const m = n, scale = (m - 1) / (max - min);
    // A range narrower than ~1e-305 (subnormals) makes (m - 1) / range overflow to Infinity; then x * Infinity | 0 and
    // 0 * Infinity | 0 are both 0, EVERY element lands in bucket 0, and the insertion pass below is O(n^2): 300k subnormals
    // took 77 s (found 2026-09-20 from the C engine, where the same overflow returned UNSORTED output). Same for a scale of 0.
    if (!(scale > 0 && scale < Infinity)) { quicksort(a, 0, n - 1, 2 * (31 - Math.clz32(n))); return; }
    const counts = pool.counts;
    for (let i = 0; i <= m; i++) counts[i] = 0;
    for (let i = 0; i < n; i++) { const idx = ((a[i] - min) * scale) | 0; counts[idx + 1]++; }
    for (let i = 1; i <= m; i++) counts[i] += counts[i - 1];
    const scratch = pool.f64Buf;
    for (let i = 0; i < n; i++) { const v = a[i]; const idx = ((v - min) * scale) | 0; scratch[counts[idx]++] = v; }
    for (let i = 0; i < n; i++) a[i] = scratch[i];
    // One scatter and then ONE insertion pass: that pass is quadratic in the largest bucket. A dense cluster of distinct
    // values inside a wide range (100k values near 0 among 200k spread over +-1e9) sits in ONE of the n buckets, and the
    // dispatcher's safety probe cannot see it when it lies between the quartiles it compares: 300k elements took 13-21 s
    // (found 2026-09-20). So the pass carries a displacement budget, like the momentum insertion: healthy data shifts well
    // under one slot per element, and past FLASH_SHIFT_BUDGET slots per element the array (a valid permutation at every
    // step) goes to the coarse sort, which rescans each bucket's own range and recurses. Scanning the counts for a large
    // bucket instead was measured first and cost 5-11% on healthy data; the budget is one subtraction per element.
    let budget = FLASH_SHIFT_BUDGET * n;
    for (let i = 1; i < n; i++) {
      const k = a[i]; let j = i - 1;
      if (a[j] > k) {                                     // most elements are already in place: the budget is only touched when one moves
        do { a[j + 1] = a[j]; j--; } while (j >= 0 && a[j] > k);
        a[j + 1] = k;
        budget -= i - 1 - j;
        if (budget < 0) { coarseBucketSort(a, 0, n - 1, 0, min, max); return; }
      }
    }
  }
  // Coarse 2-level value-bucket sort (the float path). Instead of flash sort's
  // single scatter into n cache-hostile buckets, scatter into K≈2048 cache-
  // resident buckets and recurse. Measured win comes from doing FEWER scatter
  // passes, each cache-friendly — the opposite end of the tradeoff from float
  // radix (8 passes). Recomputing min/max per sub-range makes it adapt to skew
  // and near-duplicates (where single-level flash degrades). pool.f64Buf is the
  // shared scatter buffer: every scatter is copied back before any recursion,
  // so siblings and parent/child never collide. quicksort is the within-bucket
  // fallback at the depth cap, guaranteeing O(n log n) worst case.
  // Micro-opts: (1) the top call reuses the dispatcher's already-computed
  // global min/max instead of rescanning n elements (pass NaN to force a rescan,
  // which recursive calls do); (2) tiny child buckets are insertion-sorted inline
  // rather than via a recursive call that immediately bottoms out (~300k such
  // calls on uniform 1M). Net ~+11% on uniform, neutral on skew/near-dup.
  function coarseBucketSort(a, lo, hi, depth, mn, mx) {
    const len = hi - lo + 1;
    if (len <= COARSE_SMALL) { insertionSort(a, lo, hi); return; }
    if (depth >= COARSE_MAXD) { quicksort(a, lo, hi, 2 * (31 - Math.clz32(len))); return; }
    if (mn !== mn) { // NaN sentinel => rescan this range (recursive calls pass NaN)
      mn = a[lo]; mx = a[lo];
      for (let i = lo + 1; i <= hi; i++) { const v = a[i]; if (v < mn) mn = v; else if (v > mx) mx = v; }
    }
    if (mn === mx) return;
    const k = len < COARSE_K ? Math.max(2, len >> 1) : COARSE_K;
    const C = COARSE_C[depth], S = COARSE_S[depth], H = COARSE_H[depth];
    const scale = (k - 1) / (mx - mn);
    if (!(scale > 0 && scale < Infinity)) { quicksort(a, lo, hi, 2 * (31 - Math.clz32(len))); return; }   // see flashSort
    const idx = pool.idx;   // cache each element's bucket here (Uint16 view of the idle u32B)
    for (let i = 0; i < k; i++) C[i] = 0;
    // Count pass also records the bucket index so the scatter pass can read it
    // back (a cheap typed load) instead of re-doing the float math on the boxed
    // value. ~+8-13% on the coarse path, zero extra memory (u32B is unused here).
    for (let i = lo; i <= hi; i++) { let b = ((a[i] - mn) * scale) | 0; if (b >= k) b = k - 1; idx[i] = b; C[b]++; }
    S[0] = lo;
    for (let i = 0; i < k; i++) S[i + 1] = S[i] + C[i];
    for (let i = 0; i < k; i++) H[i] = S[i];
    const buf = pool.f64Buf;
    for (let i = lo; i <= hi; i++) { const b = idx[i]; buf[H[b]++] = a[i]; }
    for (let i = lo; i <= hi; i++) a[i] = buf[i];
    for (let b = 0; b < k; b++) {
      const blo = S[b], bhi = S[b + 1] - 1, clen = bhi - blo + 1;
      if (clen <= 1) continue;
      if (clen <= COARSE_SMALL) insertionSort(a, blo, bhi);          // inline leaf
      else coarseBucketSort(a, blo, bhi, depth + 1, NaN, NaN);       // child rescans
    }
  }
  // Bits-keyed coarse for skewed Float64Array input. Buckets on the IEEE-754
  // total-order transform of the high word (sign-flip for +, full flip for -),
  // which is monotonic in value AND ~linear in log-magnitude — so heavy-tailed
  // data (where value-linear bucketing collapses into one bucket) is balanced for
  // free. Leaves/ties resolve by full value comparison, so correctness holds even
  // though the high word ignores the low mantissa. `bitsView` is the Uint32 view
  // of the array set by the sortF64 entry. Only reached via the skew gate.
  let bitsView = null;
  function coarseBitsF64(a, lo, hi, depth, mnK, mxK) {
    const len = hi - lo + 1;
    if (len <= COARSE_SMALL) { insertionSort(a, lo, hi); return; }
    if (depth >= COARSE_MAXD) { quicksort(a, lo, hi, 2 * (31 - Math.clz32(len))); return; }
    const bv = bitsView;
    if (mnK !== mnK) {
      let h = bv[2 * lo + 1]; let k0 = (h & 0x80000000) ? (~h >>> 0) : ((h | 0x80000000) >>> 0);
      mnK = k0; mxK = k0;
      for (let i = lo + 1; i <= hi; i++) { const hh = bv[2 * i + 1]; const kk = (hh & 0x80000000) ? (~hh >>> 0) : ((hh | 0x80000000) >>> 0); if (kk < mnK) mnK = kk; else if (kk > mxK) mxK = kk; }
    }
    if (mnK === mxK) { quicksort(a, lo, hi, 2 * (31 - Math.clz32(len))); return; } // same high word: sort by value
    const k = len < COARSE_K ? Math.max(2, len >> 1) : COARSE_K;
    const C = COARSE_C[depth], S = COARSE_S[depth], H = COARSE_H[depth];
    const scale = (k - 1) / (mxK - mnK);
    const idx = pool.idx;
    for (let i = 0; i < k; i++) C[i] = 0;
    for (let i = lo; i <= hi; i++) { const hh = bv[2 * i + 1]; const kk = (hh & 0x80000000) ? (~hh >>> 0) : ((hh | 0x80000000) >>> 0); let b = ((kk - mnK) * scale) | 0; if (b >= k) b = k - 1; idx[i] = b; C[b]++; }
    S[0] = lo;
    for (let i = 0; i < k; i++) S[i + 1] = S[i] + C[i];
    for (let i = 0; i < k; i++) H[i] = S[i];
    const buf = pool.f64Buf;
    for (let i = lo; i <= hi; i++) { const b = idx[i]; buf[H[b]++] = a[i]; }
    for (let i = lo; i <= hi; i++) a[i] = buf[i];
    for (let b = 0; b < k; b++) {
      const blo = S[b], bhi = S[b + 1] - 1, clen = bhi - blo + 1;
      if (clen <= 1) continue;
      if (clen <= COARSE_SMALL) insertionSort(a, blo, bhi);
      else coarseBitsF64(a, blo, bhi, depth + 1, NaN, NaN);
    }
  }
  // Cheap skew probe: sample the array, bin value-linearly into 256 coarse bins,
  // return max-bin / ideal. High (>~8) means value bucketing would collapse and
  // bits-keying should be used instead. ~tens of microseconds, sample-based.
  function skewProbe(a, n) {
    const m = 8192, KC = 256;
    const samp = new Float64Array(m); let seed = (0x9e3779b9 ^ n) >>> 0;
    let smn = Infinity, smx = -Infinity;
    for (let i = 0; i < m; i++) { seed = (seed * 1664525 + 1013904223) >>> 0; const v = a[(seed / 4294967296 * n) | 0]; samp[i] = v; if (v < smn) smn = v; if (v > smx) smx = v; }
    if (!(smx > smn)) return 1;
    const sc = KC / (smx - smn), Hh = new Int32Array(KC);
    for (let i = 0; i < m; i++) { let b = ((samp[i] - smn) * sc) | 0; if (b >= KC) b = KC - 1; else if (b < 0) b = 0; Hh[b]++; }
    let mxb = 0; for (let b = 0; b < KC; b++) if (Hh[b] > mxb) mxb = Hh[b];
    return mxb / (m / KC);
  }
  // Side-channel: average run length observed by the most recent
  // detectRunsLimited bail. Lets the nearly-sorted branch tell genuine local
  // jitter (tiny runs -> momentum insertion is cheap and safe) apart from
  // sparse far-displaced disruptions (longer runs -> momentum is O(n*disp) and
  // explodes, so route to a distribution sort instead).
  let lastBailAvgRun = 0;
  // Scan the array into maximal ascending runs (descending runs are reversed in
  // place so every run ends ascending), returned as a flat [start,end, ...] list.
  // Bails to null if there are more than maxRuns runs, or once a sample shows the
  // average run is shorter than minAvg — both mean "not worth merging". Either bail
  // records the observed average run length in lastBailAvgRun for the dispatcher.
  function detectRunsLimited(a, n, maxRuns, minAvg) {
    const result = []; let i = 0;
    while (i < n) {
      let j = i + 1;
      if (j < n) {
        if (a[j] >= a[i]) { while (j < n && a[j] >= a[j - 1]) j++; }
        else { while (j < n && a[j] < a[j - 1]) j++;
          for (let l = i, r = j - 1; l < r; l++, r--) { const t = a[l]; a[l] = a[r]; a[r] = t; } }
      }
      result.push(i, j - 1);
      const R = result.length >> 1;
      if (R > maxRuns) { lastBailAvgRun = j / R; return null; }
      // Cheap early bail: once we have a sample of runs and their average
      // length is already below the merge threshold, this is short-run data
      // (jitter/random) — stop now (only a small prefix scanned) instead of
      // running to the cap.
      if (minAvg && R >= 256 && j < R * minAvg) { lastBailAvgRun = j / R; return null; }
      i = j;
    }
    return result;
  }
  // Bottom-up merge of the detected runs into one sorted array. Each round merges
  // groups of runs and roughly halves the count: many runs use a 4-way merge (fewer
  // total passes over memory), few runs a simple pairwise 2-way merge. Reuses
  // pool.f64Buf as the merge buffer (mutually exclusive with flashSort's use).
  function mergeAllRuns(a, n, runs) {
    ensurePool(n);
    const QUADWAY_THRESHOLD = 16;
    // Merge buffer now lives in pool.f64Buf (a Float64Array view).
    // Mutually exclusive with flashSort, which also uses pool.f64Buf.
    const buf = pool.f64Buf;
    let cur = runs;
    while (cur.length > 2) {
      const next = [];
      if ((cur.length >> 1) >= QUADWAY_THRESHOLD) {
        for (let k = 0; k < cur.length; k += 8) {
          const numRuns = Math.min(4, (cur.length - k) >> 1);
          if (numRuns === 1) { next.push(cur[k], cur[k + 1]); continue; }
          if (numRuns === 2) { mergeGallop(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3]); next.push(cur[k], cur[k+3]); continue; }
          if (numRuns === 3) {
            mergeGallop(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3]);
            mergeGallop(a, buf, cur[k], cur[k+3], cur[k+4], cur[k+5]);
            next.push(cur[k], cur[k+5]); continue;
          }
          merge4WayHybrid(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3], cur[k+4], cur[k+5], cur[k+6], cur[k+7]);
          next.push(cur[k], cur[k+7]);
        }
      } else {
        for (let k = 0; k < cur.length; k += 4) {
          const a1 = cur[k], b1 = cur[k + 1];
          if (k + 2 >= cur.length) { next.push(a1, b1); continue; }
          mergeGallop(a, buf, a1, b1, cur[k + 2], cur[k + 3]);
          next.push(a1, cur[k + 3]);
        }
      }
      cur = next;
    }
  }
  // Merge four adjacent sorted runs in one pass via a 4-way tournament (emit the
  // smallest run-head each step). Opens with a predictive free-merge check (if the
  // runs are already end-to-end ordered, return untouched). When one run wins
  // MIN_GALLOP times running, switch to galloping — binary-search how far it stays
  // ahead and bulk-copy that stretch. Writes through buf, then copies back.
  function merge4WayHybrid(a, buf, a1, b1, a2, b2, a3, b3, a4, b4) {
    if (a[b1] <= a[a2] && a[b2] <= a[a3] && a[b3] <= a[a4]) return;  // predictive free-merge
    const INF = Number.POSITIVE_INFINITY;
    let p0=a1,p1=a2,p2=a3,p3=a4; const e0=b1,e1=b2,e2=b3,e3=b4;
    let v0=a[p0],v1=a[p1],v2=a[p2],v3=a[p3]; let w=a1;
    let w0=0,w1=0,w2=0,w3=0;
    while (p0<=e0||p1<=e1||p2<=e2||p3<=e3) {
      const lv=v0<=v1?v0:v1, lw=v0<=v1?0:1, rv=v2<=v3?v2:v3, rw=v2<=v3?2:3;
      const win=lv<=rv?lw:rw, wv=lv<=rv?lv:rv;
      buf[w++]=wv;
      if (win===0) { p0++; v0=p0<=e0?a[p0]:INF; w0++; w1=0; w2=0; w3=0;
        if (w0>=MIN_GALLOP) { const t=v1<v2?(v1<v3?v1:v3):(v2<v3?v2:v3);
          if (t===INF) { while (p0<=e0) buf[w++]=a[p0++]; v0=INF; }
          else { let s=1,j=p0; while (j+s<=e0 && a[j+s-1]<=t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e0+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<=t)L=m+1;else R=m;}
            while (p0<L) buf[w++]=a[p0++]; v0=p0<=e0?a[p0]:INF; } w0=0; } }
      else if (win===1) { p1++; v1=p1<=e1?a[p1]:INF; w1++; w0=0; w2=0; w3=0;
        if (w1>=MIN_GALLOP) { const t=v0<v2?(v0<v3?v0:v3):(v2<v3?v2:v3);
          if (t===INF) { while (p1<=e1) buf[w++]=a[p1++]; v1=INF; }
          else { let s=1,j=p1; while (j+s<=e1 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e1+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p1<L) buf[w++]=a[p1++]; v1=p1<=e1?a[p1]:INF; } w1=0; } }
      else if (win===2) { p2++; v2=p2<=e2?a[p2]:INF; w2++; w0=0; w1=0; w3=0;
        if (w2>=MIN_GALLOP) { const t=v0<v1?(v0<v3?v0:v3):(v1<v3?v1:v3);
          if (t===INF) { while (p2<=e2) buf[w++]=a[p2++]; v2=INF; }
          else { let s=1,j=p2; while (j+s<=e2 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e2+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p2<L) buf[w++]=a[p2++]; v2=p2<=e2?a[p2]:INF; } w2=0; } }
      else { p3++; v3=p3<=e3?a[p3]:INF; w3++; w0=0; w1=0; w2=0;
        if (w3>=MIN_GALLOP) { const t=v0<v1?(v0<v2?v0:v2):(v1<v2?v1:v2);
          if (t===INF) { while (p3<=e3) buf[w++]=a[p3++]; v3=INF; }
          else { let s=1,j=p3; while (j+s<=e3 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e3+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p3<L) buf[w++]=a[p3++]; v3=p3<=e3?a[p3]:INF; } w3=0; } }
    }
    for (let r=a1;r<=b4;r++) a[r]=buf[r];
  }
  // Merge two adjacent sorted runs with TimSort-style galloping. Predictive
  // free-merge first; otherwise merge element by element, and when one side wins
  // mg times in a row, gallop ahead in exponentially growing steps to skip its
  // in-order stretch. mg is adaptive with an eager start (2): failed gallops from
  // a low threshold are cheap probes and back off fast, while blocky data gets
  // skipped almost immediately — measured never-worse than the fixed default
  // across interleave/density/overlap scenarios. Writes through buf, then copies
  // back to a[a1..b2].
  function mergeGallop(a, buf, a1, b1, a2, b2) {
    if (a[b1] <= a[a2]) return;  // predictive free-merge: runs already in order
    const MG0=2; let p=a1,q=a2,w=a1,pW=0,qW=0,mg=MG0;
    while (p<=b1 && q<=b2) {
      if (a[p]<=a[q]) {buf[w++]=a[p++];pW++;qW=0;} else {buf[w++]=a[q++];qW++;pW=0;}
      if (pW>=mg) { const t=a[q],p0=p; let s=1,j=p; while (j+s<=b1&&a[j+s-1]<=t){j+=s;s<<=1;}
        let L=j,R=Math.min(b1+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<=t)L=m+1;else R=m;}
        while (p<L) buf[w++]=a[p++]; pW=0; if(p-p0<MIN_GALLOP)mg++;else if(mg>MG0)mg--; }
      else if (qW>=mg) { const t=a[p],q0=q; let s=1,j=q; while (j+s<=b2&&a[j+s-1]<t){j+=s;s<<=1;}
        let L=j,R=Math.min(b2+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
        while (q<L) buf[w++]=a[q++]; qW=0; if(q-q0<MIN_GALLOP)mg++;else if(mg>MG0)mg--; }
    }
    while (p<=b1) buf[w++]=a[p++]; while (q<=b2) buf[w++]=a[q++];
    for (let r=a1;r<=b2;r++) a[r]=buf[r];
  }

  // Numeric dispatcher — the core of the algorithm. A single O(n) pass gathers
  // min, max, integer-ness, and ascending/descending flags, then routes the data
  // to the cheapest path matching its detected shape:
  //   * already ascending  -> return as-is;  descending -> reverse in place
  //   * all-integer, narrow span -> counting sort;  wider int32 span -> LSD radix
  //   * few/long natural runs -> galloping run-merge (covers near-sorted, including
  //     sparse far-displaced disruptions)
  //   * nearly sorted by inversion sample -> (momentum) insertion sort
  //   * large well-spread float -> flash (small n) or coarse bucket (large n),
  //     with a multimodal gap check that splits clustered data first
  //   * otherwise -> dual-pivot introsort
  // The routing thresholds are the audit-tuned constants at the top of this IIFE.
  function sortFn(arr) {
    const length = arr.length;
    let minValue = arr[0], maxValue = arr[0];
    let allIntegers = Number.isInteger(arr[0]);
    let isAscending = true, isDescending = true;
    for (let i = 1; i < length; i++) {
      const v = arr[i];
      if (v < minValue) minValue = v; else if (v > maxValue) maxValue = v;
      if (allIntegers && !Number.isInteger(v)) allIntegers = false;
      if (isAscending  && v < arr[i - 1]) isAscending = false;
      if (isDescending && v > arr[i - 1]) isDescending = false;
    }
    const allInt32 = allIntegers && minValue >= INT32_MIN && maxValue <= INT32_MAX;
    if (isAscending) return arr;
    if (isDescending) {
      for (let l = 0, r = length - 1; l < r; l++, r--) { const t = arr[l]; arr[l] = arr[r]; arr[r] = t; }
      return arr;
    }
    if (allIntegers) {
      // Counting sort: when the integer span is within a small factor of n, one
      // bucket per distinct value gives O(n + span). Bucket element type is sized
      // to n so the count array stays as small as possible.
      const span = maxValue - minValue + 1;
      if (span <= COUNTING_SORT_K * length) {
        const CType = length <= 0xFF ? Uint8Array : length <= 0xFFFF ? Uint16Array : Uint32Array;
        const buckets = new CType(span);
        for (let i = 0; i < length; i++) buckets[arr[i] - minValue]++;
        let w = 0;
        for (let v = 0; v < span; v++) { const c = buckets[v], val = v + minValue; for (let j = 0; j < c; j++) arr[w++] = val; }
        return arr;
      }
      if (allInt32 && length >= 64) { lsdRadixInt32(arr, length); return arr; }
    }
    if (length <= INSERTION_SORT_THRESHOLD) { insertionSort(arr, 0, length - 1); return arr; }
    // Run-merge path: detect natural runs up to a cap that scales with n, so
    // it fires both for genuinely few-run data and for many-but-long-run data
    // (near-sorted with far-displaced disruptions). Short-run data exceeds the
    // cap and bails (after scanning only a small prefix) to the paths below.
    let runCap = (length / MIN_AVG_RUN_FOR_MERGE) | 0;
    if (runCap < MAX_RUNS_FOR_MERGE) runCap = MAX_RUNS_FOR_MERGE;
    const runs = detectRunsLimited(arr, length, runCap, MIN_AVG_RUN_FOR_MERGE);
    if (runs !== null && runs.length > 2) { mergeAllRuns(arr, length, runs); return arr; }
    const sampleSize = Math.min(length, 40);
    const sampleStep = Math.max(1, (length / sampleSize) | 0);
    let inv = 0, comps = 0;
    for (let i = 0; i + sampleStep < length; i += sampleStep) { if (arr[i] > arr[i + sampleStep]) inv++; comps++; }
    // The 40 pairs bound inversions at ONE stride, length/40. Disorder that is dense but shorter than the stride (an event log
    // with bounded latency, a series jittered around its own order) passes them, and an insertion sort then moves every
    // element its whole displacement: 45,000 elements with noise of +-400 took 24 ms against 2.4 for random data, because
    // below MOMENTUM_THRESHOLD the insertion sort here was the PLAIN one, with no budget (found 2026-09-20 from the C
    // engine, which had the same probe). 64 strided pairs EIGHT apart tell the two apart: the budget below is a mean
    // displacement of 8, and an element displaced further is usually inverted with its eighth neighbour. And the small
    // case (from FLASH_SORT_THRESHOLD up) now runs a BUDGETED plain insertion sort, which falls through to a distribution
    // sort when it gives up.
    let denseDisorder = false;
    if (comps > 0 && inv / comps <= NEARLY_SORTED_INV_RATIO && length >= FLASH_SORT_THRESHOLD) {
      const st = (length / 64) | 0; let d = 0;
      for (let i = 0; i + 8 < length; i += st) if (arr[i] > arr[i + 8]) d++;
      denseDisorder = d >= 16;
    }
    if (comps > 0 && inv / comps <= NEARLY_SORTED_INV_RATIO && !denseDisorder) {
      // Under FLASH_SORT_THRESHOLD nothing changes: the fallback there is a quicksort, which an insertion sort beats up to ~25
      // shifts per element, and the unbudgeted worst case is a few milliseconds (measured: budget and probe both LOST 10-30%
      // on 2000-3000 elements).
      if (length < FLASH_SORT_THRESHOLD) { insertionSort(arr, 0, length - 1); return arr; }
      if (length <= MOMENTUM_THRESHOLD) { if (insertionSortBudget(arr, 0, length - 1)) return arr; }
      else
      // Large n: only use momentum insertion for genuine local jitter (short
      // runs). Sparse far-displaced disruptions look nearly-sorted to the
      // inversion sample but would make momentum O(n*displacement); let them
      // fall through to the distribution sort below.
      if (lastBailAvgRun > 0 && lastBailAvgRun <= MOMENTUM_MAX_AVG_RUN) {
        if (insertionSortMomentum(arr, 0, length - 1)) return arr;
        // Momentum aborted: the "near-sorted" verdict was wrong (disorder
        // concentrated in a block that fooled the sample). Array is partially
        // permuted but still a valid permutation — fall through to flash/coarse.
      }
    }
    if (length >= FLASH_SORT_THRESHOLD && maxValue > minValue) {
      // Sample-based gap detection (multimodal-aware). Replaces the
      // standalone isFlashSafe call — we now do both checks (IQR safety
      // AND gap-vs-median ratio) from a single set of sorted samples.
      //
      // Why: flash sort's linear interpolation passes the IQR check on
      // bimodal data (Q1 lies in one peak, Q3 in the other, IQR ratio ≈
      // 1.0), but then dumps each cluster into ~one bucket each, leaving
      // the insertion-sort cleanup pass to do O(n²) work within each.
      // Catching the gap before flash sort fires turns O(n²) into O(n).
      const SAMPLE_SIZE = 64;
      const sStride = (length / SAMPLE_SIZE) | 0;
      const samples = new Array(SAMPLE_SIZE);
      for (let i = 0; i < SAMPLE_SIZE; i++) samples[i] = arr[i * sStride];
      samples.sort((x, y) => x - y);
      let maxGap = 0, maxGapIdx = -1;
      for (let i = 1; i < SAMPLE_SIZE; i++) {
        const g = samples[i] - samples[i - 1];
        if (g > maxGap) { maxGap = g; maxGapIdx = i; }
      }
      // Median gap — use copy-and-sort (small array, cheap)
      const gapsCopy = new Array(SAMPLE_SIZE - 1);
      for (let i = 0; i < SAMPLE_SIZE - 1; i++) gapsCopy[i] = samples[i + 1] - samples[i];
      gapsCopy.sort((a, b) => a - b);
      const medianGap = gapsCopy[gapsCopy.length >> 1];
      const range = maxValue - minValue;
      // Bimodal signal alone isn't enough — wide clusters (e.g. trimodal
      // with σ=10) have big inter-peak gaps but flash sort handles them
      // fine because each cluster spans many buckets. The catastrophic
      // case is NARROW clusters: σ smaller than bucket_width, so a whole
      // cluster's worth of values gets dumped into one bucket and the
      // insertion cleanup pass goes O(n²).
      //
      // Diagnostic for "would flash sort fail here": find any 6
      // consecutive sorted samples that span less than one bucket-width.
      // That means a real cluster packs many values into ~one bucket.
      const bucketWidth = range / length;
      let hasTightCluster = false;
      const TIGHT_WINDOW = 6;
      for (let i = 0; i + TIGHT_WINDOW < SAMPLE_SIZE; i++) {
        if (samples[i + TIGHT_WINDOW] - samples[i] < bucketWidth) { hasTightCluster = true; break; }
      }
      const isMultimodal =
        medianGap > 0 &&
        maxGap >= 10 * medianGap &&
        maxGap >= 0.2 * range &&
        hasTightCluster &&
        maxGapIdx >= 4 && maxGapIdx <= SAMPLE_SIZE - 4;
      if (isMultimodal) {
        const pivot = (samples[maxGapIdx - 1] + samples[maxGapIdx]) / 2;
        // In-place Hoare partition: arr[0..p-1] < pivot, arr[p..] not < pivot. ONE predicate for both scans: the pair it
        // replaces, arr[i] < pivot and arr[j] >= pivot, BOTH fail on a NaN, so when the two scans met on one neither index
        // moved again and sort() never returned: [lows..., NaN, highs...] with 6000 elements hung forever (found 2026-09-20
        // from the C engine, where the same loop spun a parallel sort about one run in fifteen). A NaN now goes right.
        let i = 0, j = length - 1;
        while (i <= j) {
          while (i <= j && arr[i] <  pivot) i++;
          while (i <= j && !(arr[j] < pivot)) j--;
          if (i < j) { const t = arr[i]; arr[i] = arr[j]; arr[j] = t; i++; j--; }
        }
        // Recurse on each half via slice/copy. Memory cost: O(n) temporarily,
        // only paid on the (rare) multimodal path. Recursion handles trimodal+
        // by re-detecting gaps in each half.
        if (i > 1) {
          const left = arr.slice(0, i);
          sortFn(left);
          for (let k = 0; k < i; k++) arr[k] = left[k];
        }
        if (i < length) {
          const right = arr.slice(i);
          sortFn(right);
          for (let k = 0; k < length - i; k++) arr[i + k] = right[k];
        }
        return arr;
      }
      // Audit finding: flash's single n-bucket scatter stays cache-resident
      // below ~500k and beats coarse there (up to ~2.9x); above it the n-bucket
      // count array exceeds cache and coarse's cache-friendly K-bucket scatter
      // wins (up to ~2.6x). So use flash for small, flash-safe inputs and
      // coarse for large inputs — and for small inputs flash handles poorly
      // (near-duplicate/skew), where the IQR check sends them to coarse too.
      if (length < COARSE_MIN_N && (samples[47] - samples[16]) / range >= FLASH_SAFETY_RATIO) {
        flashSort(arr, length, minValue, maxValue);
        return arr;
      }
      ensurePool(length);
      // Boxed-skew rescue: heavy-tailed input makes value-keyed coarse degrade
      // super-linearly (bucket collapse -> deep recursion -> depth-cap quicksort).
      // The sampled probe is ~free; when it trips, copy into a pooled
      // Float64Array, run the bits-keyed coarse (log-magnitude bucketing
      // balances tails), copy back. Two sequential passes buy a structural
      // multiple at large n. The copy-in doubles as the NaN guard: any NaN
      // falls back to the value path (lazy f64Data: only skewed callers pay).
      if (length >= 65536 && skewProbe(arr, length) > 8) {
        if (!pool.f64Data || pool.f64Data.length < length) pool.f64Data = new Float64Array(pool.cap);
        const td = pool.f64Data;
        let hasNaN = false;
        for (let i = 0; i < length; i++) { const v = arr[i]; if (v !== v) { hasNaN = true; break; } td[i] = v; }
        if (!hasNaN) {
          bitsView = new Uint32Array(td.buffer, 0, length * 2);
          coarseBitsF64(td, 0, length - 1, 0, NaN, NaN);
          bitsView = null;
          for (let i = 0; i < length; i++) arr[i] = td[i];
          return arr;
        }
      }
      coarseBucketSort(arr, 0, length - 1, 0, minValue, maxValue);
      return arr;
    }
    quicksort(arr, 0, length - 1, 2 * (31 - Math.clz32(length)));
    return arr;
  }
  // Typed fast path for Int32Array: assumes int32 values, so it skips the
  // per-element Number.isInteger detection the generic dispatcher runs, and
  // goes straight to counting (narrow span) or the byte-skip radix — both
  // operating directly on the typed buffer. ~1.4x over a boxed Array on radix.
  sortFn.sortI32 = function(a) {
    const n = a.length;
    if (n < 2) return a;
    ensurePool(n);
    let mn = a[0], mx = a[0], asc = true, desc = true;
    for (let i = 1; i < n; i++) { const v = a[i]; if (v < mn) mn = v; else if (v > mx) mx = v; if (asc && v < a[i-1]) asc = false; if (desc && v > a[i-1]) desc = false; }
    if (asc) return a;
    if (desc) { for (let l = 0, r = n - 1; l < r; l++, r--) { const t = a[l]; a[l] = a[r]; a[r] = t; } return a; }
    const span = mx - mn + 1;
    if (span >= 1 && span <= COUNTING_SORT_K * n) {        // span <= n <= cap, so pool.counts fits
      const C = pool.counts;
      for (let i = 0; i <= span; i++) C[i] = 0;
      for (let i = 0; i < n; i++) C[a[i] - mn]++;
      let w = 0;
      for (let v = 0; v < span; v++) { let c = C[v]; const val = v + mn; while (c-- > 0) a[w++] = val; }
      return a;
    }
    lsdRadixInt32(a, n);
    return a;
  };
  // Full LSD radix for Float64Array: up to 8 byte passes over the IEEE-754
  // total-order transform, computed on-the-fly from a u32 view (original
  // doubles are what gets scattered — transformed bit patterns are never stored,
  // avoiding NaN canonicalization; NaN keys sort last, structurally safe).
  // Constant bytes (or==and, exact, from the stats pass) skip their pass, which
  // is the whole point: quantized / low-mantissa-entropy data (dyadic fractions,
  // sensor ticks) collapses to ~3-4 passes and beats coarse bucketing by
  // 12-25% below n~8M. Gated by quantProbeF64 + n (coarse wins above ~8M).
  const RADIX_H64 = []; for (let p = 0; p < 8; p++) RADIX_H64.push(new Uint32Array(257));
  function radixF64Full(a, n) {
    const va = new Uint32Array(a.buffer, a.byteOffset, 2 * n);
    const buf = pool.f64Buf;
    const vb = new Uint32Array(buf.buffer, 0, 2 * n);
    for (let p = 0; p < 8; p++) RADIX_H64[p].fill(0);
    let orH = 0, andH = 0xFFFFFFFF, orL = 0, andL = 0xFFFFFFFF;
    for (let i = 0; i < n; i++) {
      const lo = va[2 * i], hi = va[2 * i + 1], s = hi >>> 31;
      let tl, th;
      if (s) { tl = (~lo) >>> 0; th = (~hi) >>> 0; } else { tl = lo; th = (hi | 0x80000000) >>> 0; }
      orH = (orH | th) >>> 0; andH = (andH & th) >>> 0;
      orL = (orL | tl) >>> 0; andL = (andL & tl) >>> 0;
      RADIX_H64[0][(tl & 0xFF) + 1]++; RADIX_H64[1][((tl >>> 8) & 0xFF) + 1]++;
      RADIX_H64[2][((tl >>> 16) & 0xFF) + 1]++; RADIX_H64[3][((tl >>> 24) & 0xFF) + 1]++;
      RADIX_H64[4][(th & 0xFF) + 1]++; RADIX_H64[5][((th >>> 8) & 0xFF) + 1]++;
      RADIX_H64[6][((th >>> 16) & 0xFF) + 1]++; RADIX_H64[7][((th >>> 24) & 0xFF) + 1]++;
    }
    let from = a, to = buf, vf = va, vt = vb;
    for (let p = 0; p < 8; p++) {
      const hiWord = p >= 4, sh = (p & 3) << 3;
      const or_ = hiWord ? orH : orL, and_ = hiWord ? andH : andL;
      if (((or_ >>> sh) & 0xFF) === ((and_ >>> sh) & 0xFF)) continue;  // constant byte
      const h = RADIX_H64[p];
      for (let b = 1; b < 257; b++) h[b] += h[b - 1];
      if (hiWord) {
        for (let i = 0; i < n; i++) {
          const hh = vf[2 * i + 1], th = (hh >>> 31) ? (~hh) >>> 0 : (hh | 0x80000000) >>> 0;
          const b = (th >>> sh) & 0xFF, w = h[b]; h[b] = w + 1; to[w] = from[i];
        }
      } else {
        for (let i = 0; i < n; i++) {
          const hh = vf[2 * i + 1], ll = vf[2 * i], tl = (hh >>> 31) ? (~ll) >>> 0 : ll;
          const b = (tl >>> sh) & 0xFF, w = h[b]; h[b] = w + 1; to[w] = from[i];
        }
      }
      let t = from; from = to; to = t; t = vf; vf = vt; vt = t;
    }
    if (from !== a) for (let i = 0; i < n; i++) a[i] = from[i];
  }
  // Sampled OR/AND probe: predicts how many radix passes the data needs.
  // Quantized data shows constant mantissa bytes even in a small sample; a
  // sample can only under-count active bytes, and a mispick costs at most
  // ~1.4x (radix is never catastrophic), so a 4096 stride sample suffices.
  const _qpF = new Float64Array(1); const _qpU = new Uint32Array(_qpF.buffer);
  function quantProbeF64(a, n) {
    const m = n < 4096 ? n : 4096, step = (n / m) | 0;
    let orH = 0, andH = 0xFFFFFFFF, orL = 0, andL = 0xFFFFFFFF;
    for (let i = 0, p = 0; i < m; i++, p += step) {
      _qpF[0] = a[p];
      const lo = _qpU[0], hi = _qpU[1], s = hi >>> 31;
      let tl, th;
      if (s) { tl = (~lo) >>> 0; th = (~hi) >>> 0; } else { tl = lo; th = (hi | 0x80000000) >>> 0; }
      orH = (orH | th) >>> 0; andH = (andH & th) >>> 0;
      orL = (orL | tl) >>> 0; andL = (andL & tl) >>> 0;
    }
    let passes = 0;
    for (let p = 0; p < 8; p++) {
      const hiWord = p >= 4, sh = (p & 3) << 3;
      const or_ = hiWord ? orH : orL, and_ = hiWord ? andH : andL;
      if (((or_ >>> sh) & 0xFF) !== ((and_ >>> sh) & 0xFF)) passes++;
    }
    return passes;
  }
  // Typed-float entry: large + skewed Float64Array -> bits-keyed coarse (balances
  // heavy tails for free); everything else falls through to the normal dispatcher.
  sortFn.f64 = function(a) {
    const n = a.length;
    if (n < 65536) return sortFn(a);                 // small: normal path
    // Quantized data (few active key bytes) -> full radix while it beats coarse
    // (measured crossover back to coarse ~8-12M). Distribution-blind & NaN-safe.
    if (n <= 8388608 && quantProbeF64(a, n) <= 4) {
      ensurePool(n);
      radixF64Full(a, n);
      return a;
    }
    if (skewProbe(a, n) <= 8) return sortFn(a);      // not skewed: value-keyed paths win
    ensurePool(n);
    bitsView = new Uint32Array(a.buffer, a.byteOffset, n * 2);
    coarseBitsF64(a, 0, n - 1, 0, NaN, NaN);
    bitsView = null;
    return a;
  };
  return sortFn;
})();

const sortStringsImpl = (() => {
  const INSERTION_THRESHOLD_STR = 16;
  const MAX_RUNS_FOR_MERGE_STR  = 32;

  // Plain string insertion sort (full lexicographic compare via JS <,>). Leaf
  // finisher for the string run-merge path on small inputs.
  function insertionSortStr(a, lo, hi) {
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k;
    }
  }
  // Comparison helper: lexicographic compare of two strings starting at depth d.
  // Chars 0..d-1 are assumed equal (we never call with non-shared earlier chars).
  function cmpAt(a, b, d) {
    const la = a.length, lb = b.length;
    let i = d;
    while (i < la && i < lb) {
      const ca = a.charCodeAt(i), cb = b.charCodeAt(i);
      if (ca !== cb) return ca - cb;
      i++;
    }
    return la - lb;
  }
  // Insertion sort starting at depth d. Used as leaf for small buckets.
  function insertionSortAt(a, lo, hi, d) {
    for (let i = lo + 1; i <= hi; i++) {
      const s = a[i]; let j = i - 1;
      while (j >= lo && cmpAt(a[j], s, d) > 0) { a[j + 1] = a[j]; j--; }
      a[j + 1] = s;
    }
  }
  // MSD radix sort one character at depth d, then recurse on each non-empty
  // bucket. Bucket 0 = end-of-string (already sorted relative to itself).
  // Buckets 1..256 = chars 0..255 (shifted +1 to give end-of-string priority).
  function msdAt(a, lo, hi, d, buf) {
    if (hi - lo <= INSERTION_THRESHOLD_STR) { insertionSortAt(a, lo, hi, d); return; }
    const counts = new Int32Array(258);
    let orC = 0;
    // LCP skip: advance d through any character shared by every string in the
    // range (none ending there), counting only — this avoids scattering the whole
    // range through a common prefix or a run of duplicates. On divergence (or when
    // a string ends) `counts` already holds the histogram for the real split.
    // orC piggybacks an OR of the charcodes seen at the split depth: the byte
    // histogram can only represent codes <= 255, so anything larger routes the
    // whole range to a native comparison sort (correct for full UTF-16).
    for (;;) {
      counts.fill(0); orC = 0;
      for (let i = lo; i <= hi; i++) {
        const s = a[i];
        if (d < s.length) { const c = s.charCodeAt(d); orC |= c; counts[c + 1]++; }
        else counts[0]++;
      }
      if (counts[0] !== 0) break;            // a string ends at depth d -> must split here
      let nz = 0;
      for (let c = 1; c < 258; c++) { if (counts[c] !== 0 && ++nz > 1) break; }
      if (nz === 1 && orC <= 255) { d++; continue; } // every string shares one byte-char -> skip the scatter
      break;                                  // characters diverge -> split at this depth
    }
    if (orC > 255) {
      // Non-Latin-1 character at the split depth: the 258-bucket histogram has
      // already dropped its counts (out-of-range typed-array writes are silently
      // discarded), so bucketing here would lose elements. Native sort is
      // UTF-16-correct; chars before d are equal so a full compare is safe.
      const seg = a.slice(lo, hi + 1);
      seg.sort();
      for (let i = lo; i <= hi; i++) a[i] = seg[i - lo];
      return;
    }
    const starts = new Int32Array(258);
    for (let i = 1; i < 258; i++) starts[i] = starts[i - 1] + counts[i - 1];
    const cursors = new Int32Array(258);
    for (let i = 0; i < 258; i++) cursors[i] = starts[i];
    for (let i = lo; i <= hi; i++) {
      const s = a[i];
      const c = d < s.length ? s.charCodeAt(d) + 1 : 0;
      buf[cursors[c]++] = s;
    }
    const range = hi - lo + 1;
    for (let i = 0; i < range; i++) a[lo + i] = buf[i];
    // Skip bucket 0 — strings that ended at depth d need no further sorting.
    for (let c = 1; c < 258; c++) {
      if (counts[c] > 1) {
        msdAt(a, lo + starts[c], lo + starts[c] + counts[c] - 1, d + 1, buf);
      }
    }
  }
  // Packed-prefix radix: pack 4 chars (biased +1 so end-of-string=0 < NUL=1 <
  // 'a'...) into one uint32 key per string, LSD-radix the keys carrying the
  // strings alongside, then recurse 4 chars deeper inside equal-key groups.
  // Replaces per-character charCodeAt count+scatter levels with integer byte ops;
  // constant bytes in each 4-char window skip their scatter pass outright (the
  // radix OR/AND trick), which makes shared prefixes cheap in 4-char strides.
  // Guard: any charcode >= 255 in the window overflows the biased byte -> orC
  // trips and the range falls back to msdAt (whose own guard handles full UTF-16).
  // Gated by the dispatcher to distinct-heavy data; duplicate-heavy data is
  // faster on msdAt's LCP-skip.
  const PH0 = new Uint32Array(257), PH1 = new Uint32Array(257), PH2 = new Uint32Array(257), PH3 = new Uint32Array(257);
  function packedMsdAt(a, lo, hi, d, buf) {
    const n = hi - lo + 1;
    if (n <= 24) { insertionSortAt(a, lo, hi, d); return; }
    const K1 = pool.u32A, K2 = pool.u32B;
    PH0.fill(0); PH1.fill(0); PH2.fill(0); PH3.fill(0);
    let orC = 0;
    for (let i = lo; i <= hi; i++) {
      const s = a[i], L = s.length;
      const c0 = d < L ? s.charCodeAt(d) + 1 : 0;
      const c1 = d + 1 < L ? s.charCodeAt(d + 1) + 1 : 0;
      const c2 = d + 2 < L ? s.charCodeAt(d + 2) + 1 : 0;
      const c3 = d + 3 < L ? s.charCodeAt(d + 3) + 1 : 0;
      orC |= c0 | c1 | c2 | c3;
      const k = ((c0 << 24) | (c1 << 16) | (c2 << 8) | c3) >>> 0;
      K1[i] = k;
      PH0[(k & 0xFF) + 1]++; PH1[((k >>> 8) & 0xFF) + 1]++;
      PH2[((k >>> 16) & 0xFF) + 1]++; PH3[((k >>> 24) & 0xFF) + 1]++;
    }
    if (orC > 255) { msdAt(a, lo, hi, d, buf); return; }  // biased byte overflow -> safe path
    let from = a, to = buf, kf = K1, kt = K2, t;
    const HS = [PH0, PH1, PH2, PH3];
    for (let p = 0; p < 4; p++) {
      const H = HS[p], sh = p << 3;
      let nz = 0;
      for (let b = 1; b < 257; b++) { if (H[b] !== 0 && ++nz > 1) break; }
      if (nz <= 1) continue;                                // constant byte -> skip pass
      for (let b = 1; b < 257; b++) H[b] += H[b - 1];
      for (let i = lo; i <= hi; i++) { const k = kf[i]; const w = lo + H[(k >>> sh) & 0xFF]; H[(k >>> sh) & 0xFF]++; to[w] = from[i]; kt[w] = k; }
      t = from; from = to; to = t; t = kf; kf = kt; kt = t;
    }
    if (from !== a) for (let i = lo; i <= hi; i++) a[i] = from[i];
    // Equal-key groups: low byte 0 means every string ended inside the window
    // (identical strings) -> done; otherwise all are length >= d+4 -> recurse.
    // Recursion writes K1/K2 only within its own subrange, so reading kf at
    // indices past the recursed group remains valid.
    let i = lo;
    while (i <= hi) {
      const k = kf[i]; let j = i + 1;
      while (j <= hi && kf[j] === k) j++;
      if (j - i > 1 && (k & 0xFF) !== 0) packedMsdAt(a, i, j - 1, d + 4, buf);
      i = j;
    }
  }
  // Top-level wrapper: MSD radix from depth 0. msdAt's LCP-skip advances through
  // any common prefix itself, so no separate root prefix scan is needed. The radix
  // step splits into 256 buckets in one pass, much faster than multikey QS's
  // log2(256) = 8 levels of 3-way partitioning.
  function multikeyQsRoot(a, lo, hi) {
    const range = hi - lo + 1;
    const buf = new Array(range);
    msdAt(a, lo, hi, 0, buf);
  }
  // String run detection: split into maximal ascending runs (reversing descending
  // ones in place), bail to null past maxRuns runs. Mirrors detectRunsLimited.
  function detectRunsStr(a, n, maxRuns) {
    const result = []; let i = 0;
    while (i < n) {
      let j = i + 1;
      if (j < n) {
        if (a[j] >= a[i]) { while (j < n && a[j] >= a[j - 1]) j++; }
        else { while (j < n && a[j] < a[j - 1]) j++;
          for (let l = i, r = j - 1; l < r; l++, r--) { const t = a[l]; a[l] = a[r]; a[r] = t; } }
      }
      result.push(i, j - 1);
      if ((result.length >> 1) > maxRuns) return null;
      i = j;
    }
    return result;
  }
  // Merge two adjacent sorted string runs: predictive free-merge, then a simple
  // 2-way merge. Writes through buf, then copies back to a[a1..b2].
  // Adaptive galloping merge for strings. After one run wins minGallop times in a
  // row, exponentially+binary search how far it stays ahead and bulk-copy that
  // stretch — turning long in-order blocks from O(block) compares into O(log block).
  // minGallop rises when a gallop finds little (so it backs off on fine interleave)
  // and falls when it pays. Stability via asymmetric <= (left) / < (right).
  function mergeStr(a, buf, a1, b1, a2, b2) {
    if (a[b1] <= a[a2]) return;  // predictive free-merge: already in order
    const MG=7; let p=a1,q=a2,w=a1,pW=0,qW=0,mg=MG;
    while (p<=b1 && q<=b2) {
      if (a[p]<=a[q]) {buf[w++]=a[p++];pW++;qW=0;} else {buf[w++]=a[q++];qW++;pW=0;}
      if (pW>=mg) { const t=a[q],p0=p; let s=1,j=p; while (j+s<=b1&&a[j+s-1]<=t){j+=s;s<<=1;}
        let L=j,R=Math.min(b1+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<=t)L=m+1;else R=m;}
        while (p<L) buf[w++]=a[p++]; pW=0; if(p-p0<MG)mg++;else if(mg>1)mg--; }
      else if (qW>=mg) { const t=a[p],q0=q; let s=1,j=q; while (j+s<=b2&&a[j+s-1]<t){j+=s;s<<=1;}
        let L=j,R=Math.min(b2+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
        while (q<L) buf[w++]=a[q++]; qW=0; if(q-q0<MG)mg++;else if(mg>1)mg--; }
    }
    while (p<=b1) buf[w++]=a[p++];
    while (q<=b2) buf[w++]=a[q++];
    for (let r=a1;r<=b2;r++) a[r]=buf[r];
  }
  // Bottom-up pairwise 2-way merge of the detected string runs into sorted order.
  function mergeAllRunsStr(a, n, runs) {
    const buf = new Array(n);
    let cur = runs;
    while (cur.length > 2) {
      const next = [];
      for (let k = 0; k < cur.length; k += 4) {
        const a1 = cur[k], b1 = cur[k + 1];
        if (k + 2 >= cur.length) { next.push(a1, b1); continue; }
        mergeStr(a, buf, a1, b1, cur[k + 2], cur[k + 3]);
        next.push(a1, cur[k + 3]);
      }
      cur = next;
    }
  }
  // (Old 2-char MSD prefix radix was here; subsumed by msdAt above.)
  // String dispatcher: one pass checks fully ascending/descending (return or
  // reverse). A cheap probe of the first ~64 adjacencies decides whether the data
  // looks mostly sorted; if so, attempt run-merge. Otherwise fall through to the
  // unified MSD-at-depth radix (common-prefix skip + one bucketing pass per char).
  return function(arr) {
    const n = arr.length;
    let isAsc = true, isDesc = true;
    for (let i = 1; i < n; i++) {
      if (isAsc  && arr[i] < arr[i - 1]) isAsc  = false;
      if (isDesc && arr[i] > arr[i - 1]) isDesc = false;
      if (!isAsc && !isDesc) break;
    }
    if (isAsc) return arr;
    if (isDesc) {
      for (let l = 0, r = n - 1; l < r; l++, r--) { const t = arr[l]; arr[l] = arr[r]; arr[r] = t; }
      return arr;
    }
    if (n <= INSERTION_THRESHOLD_STR) { insertionSortStr(arr, 0, n - 1); return arr; }
    const PROBE_SIZE = n < 64 ? n - 1 : 64;
    let probeSorted = 0;
    for (let i = 0; i < PROBE_SIZE; i++) if (arr[i] <= arr[i + 1]) probeSorted++;
    if (probeSorted * 4 >= PROBE_SIZE * 3) {
      const runs = detectRunsStr(arr, n, MAX_RUNS_FOR_MERGE_STR);
      if (runs !== null && runs.length > 2) {
        mergeAllRunsStr(arr, n, runs);
        return arr;
      }
    }
    // MSD-at-depth path: compute common prefix once, then radix-sort one
    // char at a time from there. Handles every shape: shared-prefix workloads
    // (URLs, paths, log lines), random short strings, and mixed-length data.
    // Replaces both the old 2-char MSD prefix radix and the multikey QS
    // fallback with one simpler unified algorithm.
    // MSD-at-depth path. Sampled distinct-ratio gate picks the variant:
    // distinct-heavy data (random keys, IDs, URLs-with-unique-suffix) wins on the
    // packed-prefix radix (integer byte ops, 4 chars per level, measured
    // 1.4-1.9x); duplicate-heavy data wins on msdAt whose LCP-skip collapses
    // repeated strings (packed measured 0.75x there). A ~free Set over a stride
    // sample separates them: high distinct ratio -> packed, else msdAt.
    if (n >= 512) {
      const m = n < 4096 ? 128 : 512;
      const step = (n / m) | 0;
      const seen = new Set();
      for (let i = 0, p = 0; i < m; i++, p += step) seen.add(arr[p]);
      if (seen.size >= m * 0.9) {
        ensurePool(n);
        const buf = new Array(n);
        packedMsdAt(arr, 0, n - 1, 0, buf);
        return arr;
      }
    }
    multikeyQsRoot(arr, 0, n - 1);
    return arr;
  };
})();

// Public entry point. Length-<=1 arrays return as-is; otherwise route by element
// type: strings to the MSD/merge string sorter, everything else to the numeric
// dispatcher. preallocate(n) warms the shared buffer pool; releaseBuffers() frees it.
const sort = (arr) => {
  if (arr.length <= 1) return arr;
  return typeof arr[0] === 'string' ? sortStringsImpl(arr) : sortNumbersImpl(arr);
};
sort.preallocate = (n) => { ensurePool(n); };
sort.releaseBuffers = () => {
  pool.u32A = null; pool.u32B = null;
  pool.f64Buf = null; pool.counts = null; pool.idx = null;
  pool.cap = 0;
};
// Typed-array entry points. At large n a boxed Array is not an option (8 bytes/elem
// plus object overhead and GC), so callers hold values in TypedArrays — and typed
// reads/writes also avoid the boxing tax. sortF64 routes a Float64Array (or any
// numeric TypedArray) through the numeric dispatcher; sortI32 takes the lean
// integer path that assumes int32 values and skips the integer-detection pass.
// Both sort in place and return the same array.
sort.sortF64 = (a) => sortNumbersImpl.f64(a);
sort.sortI32 = (a) => sortNumbersImpl.sortI32(a);

return sort;
})()
