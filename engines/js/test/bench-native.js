// test/bench-native.js — bench pure JS sort vs native addon.

const lib = require('..');
if (!lib.nativeAvailable) {
  console.error('✗ native addon not built — run `npm run rebuild` first');
  process.exit(1);
}

function timeIt(fn, gen, trials) {
  for (let i = 0; i < 5; i++) fn(gen());
  const ts = new Float64Array(trials);
  for (let i = 0; i < trials; i++) {
    const a = gen();
    const t0 = performance.now();
    fn(a);
    ts[i] = performance.now() - t0;
  }
  return Array.from(ts).sort((a, b) => a - b)[Math.floor(trials / 2)];
}

console.log('logos-sort: pure JS vs native (uniform random doubles)\n');
console.log('  n            pure JS        native F64     native Array    F64 speedup');
console.log('  ' + '-'.repeat(78));

for (const n of [100, 1000, 10000, 100000, 1000000]) {
  // Pre-generate source data, copy into fresh array each trial
  const src = new Float64Array(n);
  for (let i = 0; i < n; i++) src[i] = Math.random() * 1e6;
  const genJS  = () => Array.from(src);
  const genF64 = () => new Float64Array(src);
  const genArr = () => Array.from(src);
  const trials = n >= 1_000_000 ? 5 : 20;

  const tJS  = timeIt(lib.sort,             genJS,  trials);
  const tF64 = timeIt(lib.sortNativeF64,    genF64, trials);
  const tArr = timeIt(lib.sortNativeNumbers, genArr, trials);

  console.log('  ' + String(n).padStart(8) + '     ' +
    (tJS.toFixed(2) + ' ms').padStart(10) + '     ' +
    (tF64.toFixed(2) + ' ms').padStart(10) + '     ' +
    (tArr.toFixed(2) + ' ms').padStart(10) + '       ' +
    (tJS / tF64).toFixed(2) + 'x');
}

console.log();
console.log('Notes:');
console.log('  - native F64 is zero-copy on Float64Array — the genuine native path');
console.log('  - native Array uses N-API element-by-element copy (slow); use F64 path');
console.log('    if your data can be stored as Float64Array');
console.log('  - speedup converges to ~1x at large n: both paths are O(n log n) with');
console.log('    similar constants once the pure-JS JIT warms up');
