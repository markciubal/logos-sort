// argsort-tests.js — property checks for argSort / sortWithIndices.
//
// Four invariants, checked against a reference stable argsort (explicit
// (value, original-index) tie-break; NaN last):
//   1. result is a valid permutation of 0..n-1
//   2. input array is not modified
//   3. indices order the values ascending
//   4. stability: equal values keep increasing original index
// Exact agreement with the reference implies all four; they are also checked
// directly so a failure names the broken invariant.

function refArgsort(arr) {
  const idx = Array.from({ length: arr.length }, (_, i) => i);
  idx.sort((a, b) => {
    const x = arr[a], y = arr[b];
    const xn = x !== x, yn = y !== y;
    if (xn && yn) return a - b;
    if (xn) return 1;
    if (yn) return -1;
    if (x < y) return -1;
    if (x > y) return 1;
    return a - b;
  });
  return idx;
}

function makeRng(seed) {
  let x = seed >>> 0;
  return () => { x = (x * 1664525 + 1013904223) >>> 0; return x / 0x100000000; };
}

const GENS = {
  int32Unique:   (r, n) => Array.from({ length: n }, () => (r() * 2e9 - 1e9) | 0),
  int32FewDist:  (r, n) => Array.from({ length: n }, () => ((r() * 7) | 0) - 3),
  floatsUnique:  (r, n) => Array.from({ length: n }, () => r() * 2e6 - 1e6),
  floatsDupes:   (r, n) => Array.from({ length: n }, () => (((r() * 50) | 0) + 0.5) * (r() < 0.5 ? -1 : 1)),
  powerLaw:      (r, n) => Array.from({ length: n }, () => Math.pow(r() + 1e-9, -1.5)),
  bigInts:       (r, n) => Array.from({ length: n }, () => Math.round(r() * 9e15 - 4.5e15)),
  nanInf:        (r, n) => { const a = Array.from({ length: n }, () => r() * 100 - 50); for (let i = 0; i < Math.max(1, n >> 6); i++) { a[(r() * n) | 0] = NaN; a[(r() * n) | 0] = Infinity; a[(r() * n) | 0] = -Infinity; } return a; },
  negZero:       (r, n) => Array.from({ length: n }, () => (r() < 0.3 ? -0 : (r() < 0.5 ? 0 : r() * 10 - 5))),
  sorted:        (r, n) => Array.from({ length: n }, (_, i) => i * 2),
  strings:       (r, n) => Array.from({ length: n }, () => { let s = ''; for (let i = 0; i < 5; i++) s += String.fromCharCode(97 + Math.floor(r() * 26)); return s; }),
  stringDupes:   (r, n) => { const pool = Array.from({ length: Math.max(2, n >> 4) }, () => { let s = ''; for (let i = 0; i < 6; i++) s += String.fromCharCode(97 + Math.floor(r() * 26)); return s; }); return Array.from({ length: n }, () => pool[(r() * pool.length) | 0]); },
};

function runArgsortTests(argSort, opts = {}) {
  const trials = opts.trials || 10;
  const sizes = opts.sizes || [3, 47, 48, 100, 1000, 5000];
  let checks = 0, failures = 0;
  const failNames = new Set();
  for (const [gname, gen] of Object.entries(GENS)) {
    for (const n of sizes) {
      for (let t = 0; t < trials; t++) {
        const r = makeRng(0x9E3779B9 ^ (n * 31 + t * 7 + gname.length));
        const arr = gen(r, n);
        const before = arr.slice();
        const got = argSort(arr);
        const want = refArgsort(arr);
        checks++;
        let bad = null;
        if (got.length !== arr.length) bad = 'length';
        if (!bad) { const seen = new Uint8Array(arr.length); for (let i = 0; i < got.length; i++) { if (got[i] >= arr.length || seen[got[i]]) { bad = 'not-a-permutation'; break; } seen[got[i]] = 1; } }
        if (!bad) { for (let i = 0; i < arr.length; i++) { const a = arr[i], b = before[i]; if (a !== b && !(a !== a && b !== b)) { bad = 'input-mutated'; break; } } }
        if (!bad) { for (let i = 0; i < got.length; i++) if (got[i] !== want[i]) { bad = 'order-or-stability'; break; } }
        if (bad) { failures++; failNames.add(gname + ':' + bad + '(n=' + n + ')'); }
      }
    }
  }
  console.log('# argSort property results (' + checks + ' total checks, ' + failures + ' failures)');
  if (failures > 0) console.log('  failing: ' + Array.from(failNames).join(', '));
  else console.log('  ✓ permutation, immutability, ordering, stability hold across all generators × sizes');
  return { totalChecks: checks, totalFailures: failures };
}

module.exports = { runArgsortTests, refArgsort };
