// benchmark.js — Logos Sort vs native TimSort
//
// Compares Array.prototype.sort (V8 TimSort) against logos-sort's adaptive
// dispatcher on workloads where the dispatcher's specialized paths (counting,
// radix, flash, adaptive run detection) tend to win.
//
// Run with:  node benchmark.js

const { sort: logosSort, sortInplace: logosSortInplace } = require('./');

// --- pdqsort: reference JS port for benchmarking ------------------------
// Faithful enough for relative comparison: insertion sort for small ranges,
// median-of-three (or ninther for large ranges) pivot, Hoare partition,
// heapsort fallback when recursion depth budget is exhausted (introsort
// fallback). NOT a port of the full Orson Peters pdqsort — omits block
// partitioning and inter-recursive pattern detection. Native C++/Rust
// pdqsort will be faster in absolute terms; this measures the algorithm
// shape rather than implementation polish.
const PDQ_INSERTION = 24;
const PDQ_NINTHER   = 128;

function pdqSwap(a, i, j) { const t = a[i]; a[i] = a[j]; a[j] = t; }
function pdqSort3(a, i, j, k) {
  if (a[j] < a[i]) pdqSwap(a, i, j);
  if (a[k] < a[j]) { pdqSwap(a, j, k); if (a[j] < a[i]) pdqSwap(a, i, j); }
}
function pdqInsertion(a, lo, hi) {
  for (let i = lo + 1; i < hi; i++) {
    const v = a[i];
    let j = i - 1;
    while (j >= lo && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}
function pdqSiftDown(a, root, end, off) {
  while (true) {
    let child = 2 * root + 1;
    if (child >= end) break;
    if (child + 1 < end && a[off + child] < a[off + child + 1]) child++;
    if (!(a[off + root] < a[off + child])) break;
    pdqSwap(a, off + root, off + child);
    root = child;
  }
}
function pdqHeapsort(a, lo, hi) {
  const n = hi - lo;
  for (let i = (n >> 1) - 1; i >= 0; i--) pdqSiftDown(a, i, n, lo);
  for (let i = n - 1; i > 0; i--) {
    pdqSwap(a, lo, lo + i);
    pdqSiftDown(a, 0, i, lo);
  }
}
function pdqInner(a, lo, hi, badAllowed) {
  while (true) {
    const len = hi - lo;
    if (len < PDQ_INSERTION) { pdqInsertion(a, lo, hi); return; }
    if (badAllowed === 0)    { pdqHeapsort(a, lo, hi);  return; }
    const mid = lo + (len >> 1);
    if (len > PDQ_NINTHER) {
      pdqSort3(a, lo, lo + 1, lo + 2);
      pdqSort3(a, mid - 1, mid, mid + 1);
      pdqSort3(a, hi - 3, hi - 2, hi - 1);
      pdqSort3(a, lo + 1, mid, hi - 2);
    } else {
      pdqSort3(a, lo, mid, hi - 1);
    }
    pdqSwap(a, lo, mid);
    const pivot = a[lo];
    let i = lo + 1, j = hi - 1;
    while (true) {
      while (i < j && a[i] < pivot) i++;
      while (j > i && !(a[j] < pivot)) j--;
      if (i >= j) break;
      pdqSwap(a, i, j);
      i++; j--;
    }
    // Place pivot at j (i==j or i>j; settle on j as boundary)
    let p = j;
    if (a[p] >= pivot) p--;
    pdqSwap(a, lo, p);
    const leftSize  = p - lo;
    const rightSize = hi - p - 1;
    const unbalanced = leftSize < (len >> 3) || rightSize < (len >> 3);
    if (unbalanced) badAllowed--;
    if (leftSize < rightSize) {
      pdqInner(a, lo, p, badAllowed);
      lo = p + 1;
    } else {
      pdqInner(a, p + 1, hi, badAllowed);
      hi = p;
    }
  }
}
function pdqsort(a) {
  if (a.length < 2) return a;
  pdqInner(a, 0, a.length, 2 * Math.floor(Math.log2(a.length)));
  return a;
}

// --- attach as Array.prototype methods so we can write `.sort` vs `.logosSort`
Object.defineProperty(Array.prototype, 'logosSort', {
  value: function () { return logosSort(this); },
  writable: true, configurable: true, enumerable: false,
});
Object.defineProperty(Array.prototype, 'logosSortInplace', {
  value: function () { return logosSortInplace(this); },
  writable: true, configurable: true, enumerable: false,
});
Object.defineProperty(Array.prototype, 'pdqsort', {
  value: function () { return pdqsort(this); },
  writable: true, configurable: true, enumerable: false,
});

// --- workload generators -------------------------------------------------
const N = 1_000_000;

function randInts(n, max = 1 << 30) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = (Math.random() * max) | 0;
  return a;
}
function smallRangeInts(n, range = 256) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = (Math.random() * range) | 0;
  return a;
}
function randFloats(n) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = Math.random() * 1e6 - 5e5;
  return a;
}
function nearlySorted(n, swaps = 1000) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = i;
  for (let k = 0; k < swaps; k++) {
    const i = (Math.random() * n) | 0;
    const j = (Math.random() * n) | 0;
    const t = a[i]; a[i] = a[j]; a[j] = t;
  }
  return a;
}
function reversed(n) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = n - i;
  return a;
}
function shortStrings(n) {
  const a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = Math.random().toString(36).slice(2, 10);
  return a;
}

const WORKLOADS = [
  { name: 'random ints (n=1M, range 2^30)', make: () => randInts(N) },
  { name: 'small-range ints (n=1M, range 256)', make: () => smallRangeInts(N) },
  { name: 'random floats (n=1M)', make: () => randFloats(N) },
  { name: 'nearly sorted (n=1M, 1k swaps)', make: () => nearlySorted(N) },
  { name: 'reversed (n=1M)', make: () => reversed(N) },
  { name: 'short strings (n=200k)', make: () => shortStrings(200_000) },
];

// --- timing --------------------------------------------------------------
function timeOnce(fn) {
  const t0 = process.hrtime.bigint();
  fn();
  const t1 = process.hrtime.bigint();
  return Number(t1 - t0) / 1e6; // ms
}

function bestOf(runs, makeInput, sortFn) {
  // Warm-up
  sortFn(makeInput());
  let best = Infinity;
  for (let i = 0; i < runs; i++) {
    const arr = makeInput();
    const ms = timeOnce(() => sortFn(arr));
    if (ms < best) best = ms;
  }
  return best;
}

// --- run -----------------------------------------------------------------
const RUNS = 5;
const numericCmp = (a, b) => a - b; // native .sort() needs this for numbers

console.log(`logos-sort vs native TimSort vs reference-JS pdqsort   (best of ${RUNS} runs, lower is better)\n`);
const pad = (s, n) => s + ' '.repeat(Math.max(0, n - s.length));
console.log(
  pad('workload', 38),
  pad('.sort (TimSort)', 18),
  pad('.logosSort', 14),
  pad('.logosSortInplace', 20),
  pad('.pdqsort (JS port)', 20),
  'speedups (vs TimSort)'
);
console.log('-'.repeat(140));

let bufWins = 0, inpWinsVsPdq = 0;
for (const w of WORKLOADS) {
  const tim     = bestOf(RUNS, w.make, a => a.sort(typeof a[0] === 'number' ? numericCmp : undefined));
  const logos   = bestOf(RUNS, w.make, a => a.logosSort());
  const inplace = bestOf(RUNS, w.make, a => a.logosSortInplace());
  const pdq     = bestOf(RUNS, w.make, a => a.pdqsort());
  const sBuf = tim / logos;
  const sInp = tim / inplace;
  const sPdq = tim / pdq;
  if (sBuf > 1) bufWins++;
  if (inplace < pdq) inpWinsVsPdq++;
  console.log(
    pad(w.name, 38),
    pad(tim.toFixed(1) + ' ms', 18),
    pad(logos.toFixed(1) + ' ms', 14),
    pad(inplace.toFixed(1) + ' ms', 20),
    pad(pdq.toFixed(1) + ' ms', 20),
    `buf ${sBuf.toFixed(2)}×, inp ${sInp.toFixed(2)}×, pdq ${sPdq.toFixed(2)}×`
  );
}

console.log('-'.repeat(140));
console.log(`Logos Sort (buffered) won ${bufWins} / ${WORKLOADS.length} workloads vs TimSort.`);
console.log(`Logos Sort (in-place) beat reference pdqsort on ${inpWinsVsPdq} / ${WORKLOADS.length} workloads.`);
console.log(`\nNote: pdqsort here is a reference JS port (see top of file). Native C++/Rust pdqsort\nwill be substantially faster in absolute terms; numbers here are for relative shape.`);
