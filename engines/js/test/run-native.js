// test/run-native.js
//
// Tests the native addon: correctness across distributions and sizes,
// plus a bench against the pure-JS path.
//
// Requires the addon to be built. Run `npm run rebuild` first if needed.

const lib = require('..');
const { runTests } = require('./property-tests-harness.js');

if (!lib.nativeAvailable) {
  console.error('✗ native addon not available');
  if (lib.nativeLoadError) console.error('  load error:', lib.nativeLoadError.message);
  console.error('  build it with: npm run rebuild');
  process.exit(1);
}

console.log('logos-sort native addon test');
console.log('  C library version:', lib.nativeVersion);
console.log();

// ---------- 1. correctness across distributions ----------
function makeRng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; }; }
function isSorted(a) { for (let i = 1; i < a.length; i++) if (a[i-1] > a[i]) return false; return true; }
function multisetEqual(a, b) {
  if (a.length !== b.length) return false;
  const m = new Map();
  for (const v of a) m.set(v, (m.get(v) || 0) + 1);
  for (const v of b) { const c = m.get(v); if (!c) return false; if (c === 1) m.delete(v); else m.set(v, c - 1); }
  return m.size === 0;
}

const gens = {
  random:   (rng, n) => { const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = rng() * 1e6; return a; },
  sorted:   (rng, n) => { const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = i; return a; },
  reverse:  (rng, n) => { const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = n - i; return a; },
  equal:    (rng, n) => { const v = rng(); const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = v; return a; },
  skewed:   (rng, n) => { const a = new Float64Array(n); for (let i = 0; i < n; i++) { const u = Math.max(rng(), 1e-6); a[i] = Math.pow(u, -1.5); } return a; },
  fewUnique: (rng, n) => { const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = (rng() * 8) | 0; return a; },
};
const sizes = [0, 1, 2, 24, 25, 100, 1000, 10000, 100000];

console.log('Correctness (Float64Array path):');
let failed = 0;
for (const [name, gen] of Object.entries(gens)) {
  for (const n of sizes) {
    const rng = makeRng(name.charCodeAt(0) * 17 + n);
    const a = gen(rng, n);
    const orig = new Float64Array(a);
    lib.sortNativeF64(a);
    const ok = isSorted(a) && multisetEqual(Array.from(a), Array.from(orig));
    if (!ok) { console.log('  ✗ ' + name + ' n=' + n); failed++; }
  }
}
console.log('  ' + (failed === 0 ? '✓ all 54 cases pass' : '✗ ' + failed + ' failures'));
console.log();

console.log('Correctness (Array path):');
failed = 0;
for (const [name, gen] of Object.entries(gens)) {
  for (const n of sizes) {
    const rng = makeRng(name.charCodeAt(0) * 17 + n);
    const a = Array.from(gen(rng, n));
    const orig = a.slice();
    lib.sortNativeNumbers(a);
    const ok = isSorted(a) && multisetEqual(a, orig);
    if (!ok) { console.log('  ✗ ' + name + ' n=' + n); failed++; }
  }
}
console.log('  ' + (failed === 0 ? '✓ all 54 cases pass' : '✗ ' + failed + ' failures'));
console.log();

// ---------- 2. error handling ----------
console.log('Error handling:');
try { lib.sortNativeF64([1,2,3]); console.log('  ✗ should reject regular Array'); failed++; }
catch (e) { console.log('  ✓ sortNativeF64 rejects regular Array:', e.message); }
try { lib.sortNativeNumbers('hello'); console.log('  ✗ should reject string'); failed++; }
catch (e) { console.log('  ✓ sortNativeNumbers rejects non-array:', e.message); }
try { lib.sortNativeF64(new Uint8Array([1,2,3])); console.log('  ✗ should reject Uint8Array'); failed++; }
catch (e) { console.log('  ✓ sortNativeF64 rejects Uint8Array:', e.message); }
console.log();

if (failed > 0) { console.error('FAILED'); process.exit(1); }
console.log('✓ native addon: all correctness checks pass');
