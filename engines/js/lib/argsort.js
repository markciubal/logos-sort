// argsort.js — companion to sort_expression.js
//
// Provides indirect-sort utilities: sort indices instead of values, leaving
// the original array untouched. Lets you "re-paint" the original order,
// apply the same permutation to multiple parallel arrays, or sort one
// table column by another column's values.
//
// USAGE
//   // Load sort_expression.js first (provides `sort`), then this file.
//   const indices = argSort(arr);                  // arr unchanged
//   const sorted  = applyPermutation(arr, indices);
//   const inv     = inversePermutation(indices);
//
// IMPLEMENTATION (numbers)
//   Index-carrying LSD radix: the original index rides through every scatter
//   pass alongside the key, so the permutation falls out of the sort itself —
//   no value->index Map reconstruction (the previous design; its Map cost
//   dominated and *lost* to a naive comparator at large unique n).
//   - int32 keys:  4 byte passes over sign-flipped keys.
//   - other numbers: 8 byte passes over the IEEE-754 total-order transform of
//     the full 64 bits (carried as two uint32 words — bit-exact; float64
//     buffers could silently canonicalize the NaN-pattern bit keys that
//     ordinary negative values transform into). -0 is normalized to +0 so the
//     two stay one stable tie group; NaN keys sort to the end, deterministic.
//   Constant bytes (OR==AND across all keys) skip their pass outright, so
//   same-magnitude or narrow-range data uses far fewer than the nominal
//   passes. LSD scatter order is stable, and the index rides along => the
//   documented stability guarantee holds structurally.
//   Buffers are pooled across calls (~24 bytes/n high-water; lazily grown).
//
// IMPLEMENTATION (strings)
//   Sort a copy with the fast sort, then rebuild indices through a Map
//   (queue per value when duplicates exist — stable).
//
// PERFORMANCE (n=1M, same-process A/B vs the previous Map design)
//   int32 keys, all unique:    498ms -> 28ms   (18x)
//   int32 keys, 100 distinct:   50ms -> 15ms   (3.4x)
//   float64 keys, all unique:  521ms -> 64ms   (8.1x)
//   float64 keys, power-law:   486ms -> 67ms   (7.3x)
//   strings: unchanged (Map path retained).

// When loaded as a CommonJS module, pull in the fast sort. When concatenated
// after sort_expression.js (legacy usage), `sort` is already in scope and this
// require is harmless / overridden by the outer binding.
let sort;
try { sort = require('./sort.js'); } catch (_) { /* concatenated usage: sort is global */ }

function _fillFromQueues(arr, sorted, n) {
  const queues = new Map();
  for (let i = 0; i < n; i++) {
    const v = arr[i];
    let e = queues.get(v);
    if (e === undefined) {
      e = { positions: [i], cursor: 0 };
      queues.set(v, e);
    } else {
      e.positions.push(i);
    }
  }
  const result = new Uint32Array(n);
  for (let i = 0; i < n; i++) {
    const e = queues.get(sorted[i]);
    result[i] = e.positions[e.cursor++];
  }
  return result;
}

function _fillUnique(arr, sorted, n) {
  const map = new Map();
  for (let i = 0; i < n; i++) map.set(arr[i], i);
  const result = new Uint32Array(n);
  for (let i = 0; i < n; i++) result[i] = map.get(sorted[i]);
  return result;
}

/**
 * argSort(arr) → Uint32Array of indices such that:
 *   arr[indices[0]] <= arr[indices[1]] <= ... <= arr[indices[n-1]]
 * The input arr is NOT modified.
 * Stable: for duplicate values, original positions appear in input order.
 * Numbers take the index-carrying radix paths below; strings take the
 * sort-copy + Map path. NaN keys sort to the end (deterministic permutation).
 */

// ---- pooled buffers for the index-carrying radix (lazy; ~24 bytes/n) ----
const AP = { cap: 0, w0: null, w1: null, w2: null, w3: null, i0: null, i1: null };
function apEnsure(n) {
  if (AP.cap >= n) return;
  let c = AP.cap || 1024; while (c < n) c <<= 1;
  AP.cap = c;
  AP.w0 = new Uint32Array(c); AP.w1 = new Uint32Array(c);
  AP.w2 = new Uint32Array(c); AP.w3 = new Uint32Array(c);
  AP.i0 = new Uint32Array(c); AP.i1 = new Uint32Array(c);
}
const AH = []; for (let p = 0; p < 8; p++) AH.push(new Uint32Array(257));

// int32 keys: 4 byte passes over sign-flipped keys, index carried through
// every scatter. Constant bytes (or==and) skip their pass.
function _argRadixI32(arr, n) {
  apEnsure(n);
  const KA = AP.w0, KB = AP.w1, IA = AP.i0, IB = AP.i1;
  for (let p = 0; p < 4; p++) AH[p].fill(0);
  let or_ = 0, and_ = 0xFFFFFFFF;
  for (let i = 0; i < n; i++) {
    const k = (arr[i] ^ 0x80000000) >>> 0;          // sign flip = unsigned order
    KA[i] = k; IA[i] = i; or_ = (or_ | k) >>> 0; and_ = (and_ & k) >>> 0;
    AH[0][(k & 0xFF) + 1]++; AH[1][((k >>> 8) & 0xFF) + 1]++;
    AH[2][((k >>> 16) & 0xFF) + 1]++; AH[3][((k >>> 24) & 0xFF) + 1]++;
  }
  let kf = KA, kt = KB, inf = IA, int_ = IB, t;
  for (let p = 0; p < 4; p++) {
    const sh = p << 3;
    if (((or_ >>> sh) & 0xFF) === ((and_ >>> sh) & 0xFF)) continue;   // constant byte
    const h = AH[p];
    for (let b = 1; b < 257; b++) h[b] += h[b - 1];
    for (let i = 0; i < n; i++) {
      const k = kf[i], b = (k >>> sh) & 0xFF, w = h[b]; h[b] = w + 1;
      kt[w] = k; int_[w] = inf[i];
    }
    t = kf; kf = kt; kt = t; t = inf; inf = int_; int_ = t;
  }
  const out = new Uint32Array(n); out.set(inf.subarray(0, n));
  return out;
}

// General numbers (floats, ints beyond 32 bits, ±Infinity, NaN): 8 byte passes
// over the IEEE-754 total-order transform of the full 64 bits. The transformed
// key is carried as two uint32 words — bit-exact (a Float64Array carrier could
// canonicalize the NaN-pattern bit keys that ordinary negative values transform
// into). -0 is normalized to +0 (`v + 0`) so ±0 form one stable tie group.
const _xfBuf = new Float64Array(1);
const _xfU32 = new Uint32Array(_xfBuf.buffer);
function _argRadixF64(arr, n) {
  apEnsure(n);
  const HA = AP.w0, LA = AP.w1, HB = AP.w2, LB = AP.w3, IA = AP.i0, IB = AP.i1;
  for (let p = 0; p < 8; p++) AH[p].fill(0);
  let orH = 0, andH = 0xFFFFFFFF, orL = 0, andL = 0xFFFFFFFF;
  for (let i = 0; i < n; i++) {
    _xfBuf[0] = arr[i] + 0;                          // +0 normalizes -0
    let lo = _xfU32[0], hi = _xfU32[1];
    if (hi & 0x80000000) { hi = (~hi) >>> 0; lo = (~lo) >>> 0; }   // negative: flip all
    else hi = (hi | 0x80000000) >>> 0;                              // positive: flip sign
    HA[i] = hi; LA[i] = lo; IA[i] = i;
    orH = (orH | hi) >>> 0; andH = (andH & hi) >>> 0;
    orL = (orL | lo) >>> 0; andL = (andL & lo) >>> 0;
    AH[0][(lo & 0xFF) + 1]++; AH[1][((lo >>> 8) & 0xFF) + 1]++;
    AH[2][((lo >>> 16) & 0xFF) + 1]++; AH[3][((lo >>> 24) & 0xFF) + 1]++;
    AH[4][(hi & 0xFF) + 1]++; AH[5][((hi >>> 8) & 0xFF) + 1]++;
    AH[6][((hi >>> 16) & 0xFF) + 1]++; AH[7][((hi >>> 24) & 0xFF) + 1]++;
  }
  let hf = HA, lf = LA, ht = HB, lt = LB, inf = IA, int_ = IB, t;
  for (let p = 0; p < 8; p++) {
    const hiWord = p >= 4, sh = (p & 3) << 3;
    const or_ = hiWord ? orH : orL, and_ = hiWord ? andH : andL;
    if (((or_ >>> sh) & 0xFF) === ((and_ >>> sh) & 0xFF)) continue;  // constant byte
    const h = AH[p];
    for (let b = 1; b < 257; b++) h[b] += h[b - 1];
    if (hiWord) {
      for (let i = 0; i < n; i++) {
        const b = (hf[i] >>> sh) & 0xFF, w = h[b]; h[b] = w + 1;
        ht[w] = hf[i]; lt[w] = lf[i]; int_[w] = inf[i];
      }
    } else {
      for (let i = 0; i < n; i++) {
        const b = (lf[i] >>> sh) & 0xFF, w = h[b]; h[b] = w + 1;
        ht[w] = hf[i]; lt[w] = lf[i]; int_[w] = inf[i];
      }
    }
    t = hf; hf = ht; ht = t; t = lf; lf = lt; lt = t; t = inf; inf = int_; int_ = t;
  }
  const out = new Uint32Array(n); out.set(inf.subarray(0, n));
  return out;
}

// Small inputs: stable insertion argsort (radix setup isn't worth it).
// NaNs are stably partitioned to the end first — insertion comparisons are
// all false against NaN, which would otherwise leave them in place.
function _argInsertion(arr, n) {
  const idx = new Uint32Array(n);
  let m = 0;
  for (let i = 0; i < n; i++) { const v = arr[i]; if (v === v) idx[m++] = i; }
  if (m < n) { let w = m; for (let i = 0; i < n; i++) { const v = arr[i]; if (v !== v) idx[w++] = i; } }
  for (let i = 1; i < m; i++) {
    const ii = idx[i], v = arr[ii]; let j = i - 1;
    while (j >= 0 && arr[idx[j]] > v) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = ii;
  }
  return idx;
}

function argSort(arr) {
  const n = arr.length;
  if (n <= 1) {
    const r = new Uint32Array(n);
    for (let i = 0; i < n; i++) r[i] = i;
    return r;
  }
  if (typeof arr[0] !== 'string') {
    if (n < 48) return _argInsertion(arr, n);
    let mn = arr[0], mx = arr[0], allInt = Number.isInteger(arr[0]);
    for (let i = 1; i < n; i++) {
      const v = arr[i];
      if (v < mn) mn = v; else if (v > mx) mx = v;
      if (allInt && !Number.isInteger(v)) allInt = false;
    }
    if (allInt && mn >= -2147483648 && mx <= 2147483647) return _argRadixI32(arr, n);
    return _argRadixF64(arr, n);
  }
  // Strings: sort a copy, rebuild indices via Map (queues when duplicated).
  const sorted = arr.slice();
  sort(sorted);
  let hasDup = false;
  for (let i = 1; i < n; i++) {
    if (sorted[i] === sorted[i - 1]) { hasDup = true; break; }
  }
  return hasDup ? _fillFromQueues(arr, sorted, n) : _fillUnique(arr, sorted, n);
}

/**
 * Inverse of a permutation. indices[inverse[j]] === j for all j.
 */
function inversePermutation(indices) {
  const n = indices.length;
  const inv = new Uint32Array(n);
  for (let i = 0; i < n; i++) inv[indices[i]] = i;
  return inv;
}

/**
 * Apply a permutation to any array. Returns a new array.
 *   const perm = argSort(prices);
 *   const sortedNames = applyPermutation(names, perm);
 */
function applyPermutation(arr, indices) {
  const n = arr.length;
  const result = new Array(n);
  for (let i = 0; i < n; i++) result[i] = arr[indices[i]];
  return result;
}

/**
 * Sort arr in place and return the permutation that achieved it.
 * Lets you sort now and use restoreOrder later to revert.
 */
function sortWithIndices(arr) {
  const indices = argSort(arr);
  const orig = arr.slice();
  for (let i = 0; i < arr.length; i++) arr[i] = orig[indices[i]];
  return indices;
}

/**
 * Given the indices returned by sortWithIndices (and the still-sorted arr),
 * restore arr to its original order.
 */
function restoreOrder(arr, indices) {
  const sortedCopy = arr.slice();
  for (let i = 0; i < arr.length; i++) arr[indices[i]] = sortedCopy[i];
  return arr;
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { argSort, inversePermutation, applyPermutation, sortWithIndices, restoreOrder };
}
