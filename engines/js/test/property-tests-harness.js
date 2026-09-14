module.exports = (() => {
// ============================================================
// LogosAdaptive Sort — property-based test harness
// ------------------------------------------------------------
// Tests mathematical invariants that any correct sort MUST satisfy,
// across many randomly-generated inputs. Catches bugs that pass
// example-based tests but violate the contract.
//
// runTests(sortFn, options) -> { passed, failed, details }
//
// Properties checked (the "logic"):
//   1. multiset invariant: output contains exactly the input elements
//   2. monotonic: output[i] ≤ output[i+1] for all valid i
//   3. idempotent: sort(sort(x)) deeply equals sort(x)
//   4. deterministic: same input → same output across calls
//   5. permutation-invariance: sort(shuffle(x)) deeply equals sort(x)
//   6. concat property: sort(a++b) is a permutation of sort(a)++sort(b)
//   7. v8 agreement: matches Array.prototype.sort with default comparator
//
// Input generators (the "wisdom" — tests cover the input distributions
// that historically expose bugs):
//   - empty, single, two-element (edge cases)
//   - all-equal (degenerate)
//   - already-sorted (fast-path verification)
//   - reverse-sorted (worst-case for some sorts)
//   - one-out-of-place (resistance test)
//   - random uniform
//   - many duplicates (stability-adjacent)
//   - heavy skew (log-normal, power-law)
//   - bimodal (mixed populations)
//   - strings (lexicographic comparison)
// ============================================================

function multisetEqual(a, b) {
  if (a.length !== b.length) return false;
  const ms = new Map();
  for (const v of a) ms.set(v, (ms.get(v) || 0) + 1);
  for (const v of b) {
    const c = ms.get(v);
    if (!c) return false;
    if (c === 1) ms.delete(v); else ms.set(v, c - 1);
  }
  return ms.size === 0;
}

function arrayEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function isMonotonic(a) {
  for (let i = 1; i < a.length; i++) if (a[i - 1] > a[i]) return false;
  return true;
}

// Reproducible RNG so failures are debuggable
function makeRng(seed) {
  let s = seed >>> 0;
  return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; };
}

function shuffle(arr, rng) {
  const a = arr.slice();
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    const t = a[i]; a[i] = a[j]; a[j] = t;
  }
  return a;
}

// Lexicographically-ordered fixed-width key strings, so string order matches index
// order. Used to build near-sorted string inputs that actually exercise the
// run-merge + galloping merge path (random strings go through MSD radix instead).
function strKey(i) { return 'item_' + String(i).padStart(8, '0'); }

// ---------- input generators ----------
const generators = {
  empty:        () => [],
  single:       (rng) => [Math.floor(rng() * 1000)],
  twoElement:   (rng) => [Math.floor(rng() * 1000), Math.floor(rng() * 1000)],
  allEqual:     (rng, n) => { const v = Math.floor(rng() * 100); return Array.from({length: n}, () => v); },
  sorted:       (rng, n) => Array.from({length: n}, (_, i) => i),
  reverseSorted: (rng, n) => Array.from({length: n}, (_, i) => n - i),
  oneOutOfPlace: (rng, n) => { const a = Array.from({length: n}, (_, i) => i); const k = Math.floor(rng() * n); a[k] = (rng() < 0.5 ? -1 : n * 10); return a; },
  uniform:      (rng, n) => Array.from({length: n}, () => Math.floor(rng() * 1e6)),
  uniformFloat: (rng, n) => Array.from({length: n}, () => rng() * 1e6),
  manyDupes:    (rng, n) => Array.from({length: n}, () => Math.floor(rng() * 5)),
  logNormal:    (rng, n) => Array.from({length: n}, () => Math.exp(rng() * 5)),
  powerLaw:     (rng, n) => Array.from({length: n}, () => Math.pow(rng(), -1.5) * 100),
  bimodal:      (rng, n) => Array.from({length: n}, () => rng() < 0.5 ? rng() * 10 : 1000 + rng() * 10),
  strings:      (rng, n) => Array.from({length: n}, () => { let s = ''; for (let i = 0; i < 6; i++) s += String.fromCharCode(97 + Math.floor(rng() * 26)); return s; }),
  longStrings:  (rng, n) => Array.from({length: n}, () => { let s = 'https://api.example.com/v2/'; for (let i = 0; i < 8; i++) s += String.fromCharCode(97 + Math.floor(rng() * 26)); return s; }),
  // Near-sorted string inputs -> string dispatcher routes to run-merge -> galloping
  // merge. Distinct keys (no ties), so v8-agreement holds exactly without stability.
  sortedStrings:      (rng, n) => Array.from({length: n}, (_, i) => strKey(i)),
  nearSortedStrings:  (rng, n) => { const a = Array.from({length: n}, (_, i) => strKey(i)); const m = Math.max(1, (n * 0.02) | 0); for (let k = 0; k < m; k++) { const x = (rng() * n) | 0, y = (rng() * n) | 0; const t = a[x]; a[x] = a[y]; a[y] = t; } return a; },
  blockSortedStrings: (rng, n) => { const a = Array.from({length: n}, (_, i) => strKey(i)); const nb = Math.min(16, Math.max(2, n >> 4)); const B = Math.ceil(n / nb); const parts = []; for (let i = 0; i < n; i += B) parts.push(a.slice(i, i + B)); for (let i = parts.length - 1; i > 0; i--) { const j = (rng() * (i + 1)) | 0; const t = parts[i]; parts[i] = parts[j]; parts[j] = t; } return [].concat(...parts); },
  // Mixed ASCII/non-Latin-1 strings: locks the UTF-16 fallback in the byte-radix
  // path (codes > 255 once silently corrupted output — out-of-range typed-array
  // histogram writes are discarded).
  unicodeStrings: (rng, n) => Array.from({length: n}, () => { let s = ''; for (let i = 0; i < 4; i++) s += String.fromCharCode(97 + Math.floor(rng() * 26)); if (rng() < 0.3) s += String.fromCharCode(0x100 + Math.floor(rng() * 0x4000)); return s + Math.floor(rng() * 100); }),
};

// ---------- core property checks ----------
function checkProperties(sortFn, input, rng) {
  const failures = [];
  // To preserve original for comparison, all sorts work on copies
  const original = input.slice();
  const isStr = original.length > 0 && typeof original[0] === 'string';
  const cmp = isStr ? undefined : (a, b) => a < b ? -1 : a > b ? 1 : 0;

  // Property 1: multiset invariant
  const sorted = sortFn(input.slice());
  if (!multisetEqual(sorted, original)) failures.push('multiset-violated');

  // Property 2: monotonic
  if (!isMonotonic(sorted)) failures.push('not-monotonic');

  // Property 3: idempotent
  const sortedAgain = sortFn(sorted.slice());
  if (!arrayEqual(sortedAgain, sorted)) failures.push('not-idempotent');

  // Property 4: deterministic
  const sorted2 = sortFn(input.slice());
  if (!arrayEqual(sorted2, sorted)) failures.push('not-deterministic');

  // Property 5: permutation-invariance
  if (input.length > 1) {
    const shuffled = shuffle(original, rng);
    const sortedShuffled = sortFn(shuffled);
    if (!arrayEqual(sortedShuffled, sorted)) failures.push('not-permutation-invariant');
  }

  // Property 6: concat property
  if (input.length >= 4) {
    const mid = input.length >> 1;
    const a = original.slice(0, mid), b = original.slice(mid);
    const sortedConcat = sortFn(original.slice());
    const sortedA = sortFn(a.slice());
    const sortedB = sortFn(b.slice());
    if (!multisetEqual(sortedConcat, sortedA.concat(sortedB))) failures.push('concat-violated');
  }

  // Property 7: v8 agreement (multiset-level; we don't require stability)
  const v8Sorted = original.slice().sort(cmp);
  if (!arrayEqual(sorted, v8Sorted)) failures.push('v8-disagreement');

  return failures;
}

// ---------- public runner ----------
function runTests(sortFn, options) {
  options = options || {};
  const trialsPerGenerator = options.trials || 50;
  const sizes = options.sizes || [10, 100, 1000];
  const verbose = options.verbose !== false;

  const results = {};
  let totalChecks = 0, totalFailures = 0;
  const firstFailures = {};

  for (const [genName, gen] of Object.entries(generators)) {
    results[genName] = { runs: 0, failures: 0, byProperty: {} };
    for (const size of sizes) {
      // Skip size-incompatible generators
      if (['empty'].includes(genName) && size > 0) {
        // run only once for empty
        if (size !== sizes[0]) continue;
      }
      const useSize = ['empty'].includes(genName) ? 0
                    : ['single'].includes(genName) ? 1
                    : ['twoElement'].includes(genName) ? 2
                    : size;
      for (let trial = 0; trial < trialsPerGenerator; trial++) {
        const rng = makeRng(trial * 1009 + size * 17 + genName.charCodeAt(0));
        const input = gen(rng, useSize);
        const failures = checkProperties(sortFn, input, rng);
        results[genName].runs++;
        totalChecks++;
        if (failures.length > 0) {
          results[genName].failures++;
          totalFailures++;
          for (const f of failures) {
            results[genName].byProperty[f] = (results[genName].byProperty[f] || 0) + 1;
            if (!firstFailures[f]) firstFailures[f] = { gen: genName, size: useSize, input: input.slice(0, 20), trial };
          }
        }
      }
    }
  }

  if (verbose) {
    console.log('# Property test results (' + totalChecks + ' total checks, ' + totalFailures + ' failures)');
    if (totalFailures === 0) {
      console.log('  ✓ all properties hold across all generators × sizes');
    } else {
      for (const [g, r] of Object.entries(results)) {
        if (r.failures > 0) {
          const byProp = Object.entries(r.byProperty).map(([k, v]) => k + '×' + v).join(', ');
          console.log('  ✗ ' + g.padEnd(16) + ' ' + r.failures + '/' + r.runs + ' failed: ' + byProp);
        }
      }
      console.log();
      console.log('  First-failure exemplars:');
      for (const [prop, ex] of Object.entries(firstFailures)) {
        console.log('    ' + prop + ': ' + ex.gen + '(n=' + ex.size + ', trial=' + ex.trial + ') input head: ' + JSON.stringify(ex.input));
      }
    }
  }

  return { totalChecks, totalFailures, results, firstFailures };
}

return { runTests };
})()
