module.exports = (() => {
// ============================================================
// LogosAdaptive Sort — v3.8-inplace (strictly in-place)
// ------------------------------------------------------------
// Strict in-place. Three pattern detectors:
//
//   + Cyclic rotation detection (numeric & string). The asc/desc scan
//     also counts descents; if exactly one and arr[n-1] <= arr[0], the
//     array is a rotated sorted sequence — fix with three in-place
//     reverses (O(n)) instead of O(n log n) sorting. Detection adds
//     zero overhead (piggybacks on the existing scan).
//
//   + Common-prefix start-depth for strings. Before dispatching, walk
//     the array once computing the longest common prefix vs arr[0].
//     Pass that length as the starting depth so the radix/quicksort
//     skips characters every comparison would have re-read otherwise.
//
//   + American Flag Sort one-shot at the divergence depth (v3.8). A
//     single in-place 256-way bucket split via cycle permutation captures
//     the big algorithmic win from buffered v3.8's MSD-at-depth without
//     needing a scratch array. After the one AFS pass, multikey QS
//     handles within-bucket recursion — this avoids AFS's overhead on
//     patterned data (logLines, near-duplicates) while keeping its
//     speed on diverse data (URLs, paths, short strings). Constant
//     aux memory (three Int32Array(258) at module scope, ~3KB total).
//
// Bench vs prior in-place version (v3.7.8-inplace) at n=100k:
//   urls:     1.14× faster  paths:    1.08× faster
//   logLines: 1.09× faster  withDups: 1.25× faster
//   shortStr: ~tied         numeric:  unchanged
//
// Dropped vs initial sketch: dominant-value Dutch flag partition.
// Profiling showed it's redundant with the dual-pivot introsort's
// existing 3-way partitioning.
//
// Auxiliary memory: three pre-allocated Int32Array(258) buffers shared
// across all sort calls (~3KB total, allocated once at module load).
// Zero allocations during sorting beyond the JS call stack.
// ============================================================

const INSERTION_THRESHOLD = 24;

// ---------- shared in-place helper ----------
function reverseRange(a, lo, hi) {
  while (lo < hi) {
    const t = a[lo]; a[lo] = a[hi]; a[hi] = t;
    lo++; hi--;
  }
}

// ---------- numeric in-place helpers ----------
function insertionSortNum(a, lo, hi) {
  for (let i = lo + 1; i <= hi; i++) {
    const k = a[i]; let j = i - 1;
    while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
}
function siftDownNum(a, base, root, end) {
  for (;;) {
    let big = root;
    const l = 2 * root + 1, r = l + 1;
    if (l < end && a[base + l] > a[base + big]) big = l;
    if (r < end && a[base + r] > a[base + big]) big = r;
    if (big === root) return;
    const t = a[base + root]; a[base + root] = a[base + big]; a[base + big] = t;
    root = big;
  }
}
function heapSortNum(a, lo, hi) {
  const len = hi - lo + 1;
  for (let i = (len >> 1) - 1; i >= 0; i--) siftDownNum(a, lo, i, len);
  for (let i = len - 1; i > 0; i--) {
    const t = a[lo]; a[lo] = a[lo + i]; a[lo + i] = t;
    siftDownNum(a, lo, 0, i);
  }
}
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
function introsortNum(a, lo, hi, depthLeft) {
  while (hi - lo >= INSERTION_THRESHOLD) {
    if (depthLeft === 0) { heapSortNum(a, lo, hi); return; }
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
      depthLeft--;
      introsortNum(a, lo, less - 2, depthLeft);
      introsortNum(a, great + 2, hi, depthLeft);
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
      depthLeft--;
      if (lt - lo < hi - gt) { introsortNum(a, lo, lt - 1, depthLeft); lo = gt + 1; }
      else                   { introsortNum(a, gt + 1, hi, depthLeft); hi = lt - 1; }
    }
  }
  insertionSortNum(a, lo, hi);
}

// ---------- string in-place helpers ----------
function insertionSortStr(a, lo, hi) {
  for (let i = lo + 1; i <= hi; i++) {
    const k = a[i]; let j = i - 1;
    while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
}
function multikeyQs(a, lo, hi, d) {
  while (hi - lo >= INSERTION_THRESHOLD) {
    const mid = (lo + hi) >> 1;
    const c1 = d < a[lo].length  ? a[lo].charCodeAt(d)  : -1;
    const c2 = d < a[mid].length ? a[mid].charCodeAt(d) : -1;
    const c3 = d < a[hi].length  ? a[hi].charCodeAt(d)  : -1;
    const pv = c1 < c2 ? (c2 < c3 ? c2 : (c1 < c3 ? c3 : c1)) : (c1 < c3 ? c1 : (c2 < c3 ? c3 : c2));
    let lt = lo, gt = hi, i = lo;
    while (i <= gt) {
      const s = a[i];
      const c = d < s.length ? s.charCodeAt(d) : -1;
      if      (c < pv) { const t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
      else if (c > pv) { const t = a[gt]; a[gt] = a[i]; a[i] = t; gt--;       }
      else             { i++; }
    }
    multikeyQs(a, lo, lt - 1, d);
    if (pv >= 0) multikeyQs(a, lt, gt, d + 1);
    lo = gt + 1;
  }
  insertionSortStr(a, lo, hi);
}

// AFS one-shot: 256-way in-place bucket split at depth d via cycle
// permutation, then dispatches each bucket to multikey QS for the rest.
// Captures the big v3.8 win at the divergence depth (256-way split in
// one pass beats multikey QS's ~8 levels of 3-way partition) without
// the recursion overhead that hurts AFS on patterned/duplicate-heavy
// data. Uses three pre-allocated 258-int buffers — constant aux memory,
// not proportional to n.
const afsCounts = new Int32Array(258);
const afsStarts = new Int32Array(258);
const afsHeads  = new Int32Array(258);
function afsOneShot(arr, lo, hi, d) {
  if (hi - lo < 256) {
    // Range too small to justify the 256-bucket overhead — straight to multikey.
    multikeyQs(arr, lo, hi, d);
    return;
  }
  afsCounts.fill(0);
  for (let i = lo; i <= hi; i++) {
    const s = arr[i];
    afsCounts[d < s.length ? s.charCodeAt(d) + 1 : 0]++;
  }
  afsStarts[0] = lo;
  for (let c = 1; c < 258; c++) afsStarts[c] = afsStarts[c - 1] + afsCounts[c - 1];
  for (let c = 0; c < 258; c++) afsHeads[c] = afsStarts[c];
  // In-place cycle permutation: for each bucket c, pick up the value at
  // its head, follow swaps until landing on a value that belongs in c.
  for (let c = 0; c < 258; c++) {
    const end = afsStarts[c] + afsCounts[c];
    while (afsHeads[c] < end) {
      let val = arr[afsHeads[c]];
      let target = d < val.length ? val.charCodeAt(d) + 1 : 0;
      while (target !== c) {
        const dstHead = afsHeads[target];
        const tmp = arr[dstHead];
        arr[dstHead] = val;
        val = tmp;
        afsHeads[target] = dstHead + 1;
        target = d < val.length ? val.charCodeAt(d) + 1 : 0;
      }
      arr[afsHeads[c]] = val;
      afsHeads[c]++;
    }
  }
  // Recurse via multikey QS for each non-empty bucket (skip bucket 0 =
  // end-of-string; those strings are already in their final relative order).
  for (let c = 1; c < 258; c++) {
    if (afsCounts[c] > 1) {
      multikeyQs(arr, afsStarts[c], afsStarts[c] + afsCounts[c] - 1, d + 1);
    }
  }
}

// Pattern 2: common prefix detection. Walks the array once, shrinking
// prefixLen against arr[0]'s prefix. Returns the common-prefix length.
// Bails fast (return 0) as soon as the shared prefix collapses.
function commonPrefixLen(a, n) {
  if (n < 2) return 0;
  const first = a[0];
  let prefixLen = first.length;
  if (prefixLen === 0) return 0;
  for (let i = 1; i < n; i++) {
    const s = a[i];
    const lim = prefixLen < s.length ? prefixLen : s.length;
    let j = 0;
    while (j < lim && first.charCodeAt(j) === s.charCodeAt(j)) j++;
    prefixLen = j;
    if (prefixLen === 0) return 0;
  }
  return prefixLen;
}

// Pattern 1: structural order detection. Single-pass scan catches:
//   ascending  → done in place
//   descending → reverse in place
//   rotation   → three in-place reverses
//   random     → caller proceeds with normal sort
// Returns true if handled, false otherwise. Replaces the prior
// asc/desc-only scan with no extra cost (same single pass, slightly
// more state tracked).
function tryStructuralOrder(a, n) {
  let isAsc = true, isDesc = true;
  let descents = 0, firstDescentPos = -1;
  for (let i = 1; i < n; i++) {
    const prev = a[i - 1], curr = a[i];
    if (curr < prev) {
      isAsc = false;
      descents++;
      if (descents === 1) firstDescentPos = i;
    } else if (curr > prev) {
      isDesc = false;
    }
    if (descents > 1 && !isDesc) break;
  }
  if (isAsc) return true;
  if (isDesc) { reverseRange(a, 0, n - 1); return true; }
  if (descents === 1 && a[n - 1] <= a[0]) {
    // Valid cyclic rotation. Three-reverse to canonical sorted order.
    reverseRange(a, 0, firstDescentPos - 1);
    reverseRange(a, firstDescentPos, n - 1);
    reverseRange(a, 0, n - 1);
    return true;
  }
  return false;
}

// ---------- dispatcher ----------
const sort = (arr) => {
  const n = arr.length;
  if (n <= 1) return arr;
  const isString = typeof arr[0] === 'string';

  if (tryStructuralOrder(arr, n)) return arr;

  if (n <= INSERTION_THRESHOLD) {
    if (isString) insertionSortStr(arr, 0, n - 1);
    else          insertionSortNum(arr, 0, n - 1);
    return arr;
  }

  if (isString) {
    // Pattern 2: skip the shared prefix; AFS one-shot does the 256-way split
    // at the divergence depth, multikey QS handles within-bucket recursion.
    const startDepth = commonPrefixLen(arr, n);
    afsOneShot(arr, 0, n - 1, startDepth);
  } else {
    introsortNum(arr, 0, n - 1, 2 * (31 - Math.clz32(n)));
  }
  return arr;
};

return sort;
})()
