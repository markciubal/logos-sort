// ============================================================
//  logos-sort 0.2.2 — single-expression build
//  The whole file evaluates to the in-place sort function.
//    const sort = /* paste this file */;
//    sort(arr);            // numbers or strings, ascending, in place, returns arr
//    sort.sortInplace(arr) // same engine, explicit name
//  Dispatch (one prescan, cheapest path first):
//    sorted/reverse -> O(n) early-exit; small n -> insertion;
//    strings -> multikey (3-way) radix quicksort;
//    int32 (n>=256) -> sign-corrected LSD radix;
//    other numbers -> introsort (3-way quicksort + heapsort guard).
//  Engine is byte-identical to the published 0.2.x package.
// ============================================================
(() => {
'use strict';

// ============================================================
// logos-sort — in-place adaptive sort (standalone build)
// ------------------------------------------------------------
// Licensed under the Logos Sort Ethical Source License v1.1.
// Dual-licensed: free for personal, research, educational, non-profit, and
// open-source use; commercial use requires a paid license. See LICENSE for
// full terms and prohibited uses. NOT an OSI-approved open source license.
//
// Dispatch strategy:
//   + Pre-scan detects already-sorted / reverse-sorted input -> O(n) exit.
//   + Small arrays  -> insertion sort.
//   + Strings       -> three-way (multikey) radix quicksort, which reads each
//                      character about once and exploits shared prefixes. This
//                      is the path that consistently beats a comparison sort
//                      on string data.
//   + int32 numbers -> LSD radix sort (non-comparison, sign-corrected).
//   + other numbers -> introsort (quicksort + heapsort fallback).
//
// Sorts in place and returns the same array (like Array.prototype.sort, but
// without the lexicographic-coercion footgun on numbers).
// ============================================================

const INSERTION_THRESHOLD = 24;
const RADIX_THRESHOLD = 256;

/* ------------------------------------------------------------------ */
/* Insertion sorts                                                     */
/* ------------------------------------------------------------------ */

function insertionSortNum(a, lo, hi) {
  for (let i = lo + 1; i <= hi; i++) {
    const v = a[i];
    let j = i - 1;
    while (j >= lo && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

function insertionSortStr(a, lo, hi) {
  for (let i = lo + 1; i <= hi; i++) {
    const v = a[i];
    let j = i - 1;
    while (j >= lo && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

/* ------------------------------------------------------------------ */
/* Numeric: introsort (quicksort + heapsort fallback)                  */
/* ------------------------------------------------------------------ */

function med3Num(a, x, y, z) {
  const ax = a[x], ay = a[y], az = a[z];
  if (ax < ay) {
    if (ay < az) return y;
    return ax < az ? z : x;
  } else {
    if (ax < az) return x;
    return ay < az ? z : y;
  }
}

function siftDown(a, lo, start, end) {
  let root = start;
  while (true) {
    let child = lo + 2 * (root - lo) + 1;
    if (child > end) break;
    if (child + 1 <= end && a[child] < a[child + 1]) child++;
    if (a[root] < a[child]) {
      const t = a[root]; a[root] = a[child]; a[child] = t;
      root = child;
    } else break;
  }
}

function heapSortNum(a, lo, hi) {
  const n = hi - lo + 1;
  for (let start = lo + ((n >> 1) - 1); start >= lo; start--) siftDown(a, lo, start, hi);
  for (let end = hi; end > lo; end--) {
    const t = a[lo]; a[lo] = a[end]; a[end] = t;
    siftDown(a, lo, lo, end - 1);
  }
}

function introsortNum(a, lo, hi, depth) {
  while (hi - lo > INSERTION_THRESHOLD) {
    if (depth === 0) { heapSortNum(a, lo, hi); return; }
    depth--;
    const mid = lo + ((hi - lo) >> 1);
    const m = med3Num(a, lo, mid, hi);
    const pivot = a[m];
    // three-way partition (handles many duplicates without O(n^2))
    let lt = lo, gt = hi, i = lo;
    while (i <= gt) {
      const v = a[i];
      if (v < pivot)      { const t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
      else if (v > pivot) { const t = a[gt]; a[gt] = a[i]; a[i] = t; gt--; }
      else                { i++; }
    }
    // recurse on smaller side, loop on larger (bounded stack)
    if (lt - lo < hi - gt) {
      introsortNum(a, lo, lt - 1, depth);
      lo = gt + 1;
    } else {
      introsortNum(a, gt + 1, hi, depth);
      hi = lt - 1;
    }
  }
  insertionSortNum(a, lo, hi);
}

/* ------------------------------------------------------------------ */
/* Numeric: int32 LSD radix sort (sign-corrected)                      */
/* ------------------------------------------------------------------ */

function radixSortInt32(a, lo, hi) {
  const n = hi - lo + 1;
  const src = new Int32Array(n);
  const dst = new Int32Array(n);
  // bias by 0x80000000 so signed ints sort as unsigned
  for (let i = 0; i < n; i++) src[i] = (a[lo + i] ^ 0x80000000) | 0;

  let from = src, to = dst;
  for (let shift = 0; shift < 32; shift += 8) {
    const count = new Int32Array(257);
    for (let i = 0; i < n; i++) count[((from[i] >>> shift) & 0xff) + 1]++;
    for (let i = 0; i < 256; i++) count[i + 1] += count[i];
    for (let i = 0; i < n; i++) to[count[(from[i] >>> shift) & 0xff]++] = from[i];
    const tmp = from; from = to; to = tmp;
  }
  // 4 passes -> result is back in `from` (src). Undo bias, write back.
  for (let i = 0; i < n; i++) a[lo + i] = (from[i] ^ 0x80000000) | 0;
}

/* ------------------------------------------------------------------ */
/* Strings: three-way (multikey) radix quicksort                       */
/* ------------------------------------------------------------------ */

function multikeyQs(a, lo, hi, d) {
  while (hi - lo > INSERTION_THRESHOLD) {
    const mid = lo + ((hi - lo) >> 1);
    const c1 = d < a[lo].length  ? a[lo].charCodeAt(d)  : -1;
    const c2 = d < a[mid].length ? a[mid].charCodeAt(d) : -1;
    const c3 = d < a[hi].length  ? a[hi].charCodeAt(d)  : -1;
    const pv = c1 < c2 ? (c2 < c3 ? c2 : (c1 < c3 ? c3 : c1))
                       : (c1 < c3 ? c1 : (c2 < c3 ? c3 : c2));
    let lt = lo, gt = hi, i = lo;
    while (i <= gt) {
      const s = a[i];
      const c = d < s.length ? s.charCodeAt(d) : -1;
      if      (c < pv) { const t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
      else if (c > pv) { const t = a[gt]; a[gt] = a[i]; a[i] = t; gt--;      }
      else             { i++; }
    }
    multikeyQs(a, lo, lt - 1, d);
    if (pv >= 0) multikeyQs(a, lt, gt, d + 1);
    lo = gt + 1;
  }
  insertionSortStr(a, lo, hi);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

function sortInplace(arr) {
  const n = arr.length;
  if (n <= 1) return arr;

  const isString = typeof arr[0] === 'string';

  // Single pre-scan: detect sorted/reverse-sorted, and (for numbers) whether
  // every element is a 32-bit integer.
  let isAsc = true, isDesc = true, allInt32 = !isString;
  for (let i = 0; i < n; i++) {
    if (i > 0) {
      if (isAsc  && arr[i] < arr[i - 1]) isAsc  = false;
      if (isDesc && arr[i] > arr[i - 1]) isDesc = false;
    }
    if (allInt32) {
      const v = arr[i];
      if ((v | 0) !== v) allInt32 = false;
    }
    if (!isAsc && !isDesc && !allInt32) break;
  }

  if (isAsc) return arr;
  if (isDesc) {
    for (let l = 0, r = n - 1; l < r; l++, r--) {
      const t = arr[l]; arr[l] = arr[r]; arr[r] = t;
    }
    return arr;
  }

  if (n <= INSERTION_THRESHOLD) {
    if (isString) insertionSortStr(arr, 0, n - 1);
    else          insertionSortNum(arr, 0, n - 1);
    return arr;
  }

  if (isString) {
    multikeyQs(arr, 0, n - 1, 0);
    return arr;
  }

  if (allInt32 && n >= RADIX_THRESHOLD) {
    radixSortInt32(arr, 0, n - 1);
    return arr;
  }

  introsortNum(arr, 0, n - 1, 2 * (31 - Math.clz32(n)));
  return arr;
}

// `sort` is exposed as an alias of the in-place engine in this standalone
// build (the multi-module package distinguishes a buffered `sort`).

sortInplace.sort = sortInplace;
sortInplace.sortInplace = sortInplace;
return sortInplace;
})()
