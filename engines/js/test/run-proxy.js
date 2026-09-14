// test/run-proxy.js — correctness and call-savings tests for sortWithProxy.

const { sortWithProxy } = require('..');

function makeRng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; }; }
function gauss(rng) { const u1 = rng() || 1e-10, u2 = rng(); return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2); }

function makeItems(n, noise, seed) {
  const rng = makeRng(seed);
  const items = [];
  for (let i = 0; i < n; i++) {
    const trueVal = rng() * 1000;
    items.push({ id: i, trueVal, proxy: trueVal + gauss(rng) * noise });
  }
  return items;
}
function isSortedBy(arr, cmp) { for (let i = 1; i < arr.length; i++) if (cmp(arr[i - 1], arr[i]) > 0) return false; return true; }

const trueCmp = (a, b) => a.trueVal < b.trueVal ? -1 : a.trueVal > b.trueVal ? 1 : 0;

let failed = 0;

console.log('sortWithProxy — correctness\n');

// 1. Output is always exactly sorted, regardless of proxy quality
console.log('Output correctness across proxy quality (n=2000):');
for (const noise of [0, 3, 15, 50, 150, 500]) {
  for (const seed of [1, 2, 3]) {
    const items = makeItems(2000, noise, seed);
    const { sorted } = sortWithProxy(items, it => it.proxy, trueCmp);
    if (sorted.length !== items.length || !isSortedBy(sorted, trueCmp)) {
      console.log('  ✗ noise=' + noise + ' seed=' + seed); failed++;
    }
  }
}
console.log('  ' + (failed === 0 ? '✓ all outputs correctly sorted' : failed + ' failures'));

// 2. Multiset preservation (no items lost or duplicated)
console.log('\nMultiset preservation:');
{
  const items = makeItems(1000, 20, 7);
  const { sorted } = sortWithProxy(items, it => it.proxy, trueCmp);
  const idsIn = items.map(x => x.id).sort((a, b) => a - b);
  const idsOut = sorted.map(x => x.id).sort((a, b) => a - b);
  const ok = idsIn.length === idsOut.length && idsIn.every((v, i) => v === idsOut[i]);
  console.log('  ' + (ok ? '✓ all items preserved' : '✗ items lost or duplicated'));
  if (!ok) failed++;
}

// 3. Call savings vs a plain comparator sort
console.log('\nExpensive-call savings vs Array.prototype.sort(cmp) (n=2000):');
console.log('  proxy noise   plain sort   sortWithProxy   strategy     reduction');
for (const noise of [3, 15, 50, 150]) {
  const items = makeItems(2000, noise, 7);

  let plainCalls = 0;
  items.slice().sort((a, b) => { plainCalls++; return trueCmp(a, b); });

  const { oracleCalls, strategy } = sortWithProxy(items, it => it.proxy, trueCmp);
  const reduction = ((1 - oracleCalls / plainCalls) * 100).toFixed(0) + '%';
  console.log('  ' + String(noise).padStart(10) + '   ' + String(plainCalls).padStart(10) + '   ' +
    String(oracleCalls).padStart(13) + '   ' + strategy.padEnd(10) + '   ' + reduction.padStart(8));
}

// 4. Edge cases
console.log('\nEdge cases:');
{
  const empty = sortWithProxy([], x => x, (a, b) => a - b);
  console.log('  ' + (empty.sorted.length === 0 ? '✓' : '✗') + ' empty array');
  const single = sortWithProxy([{ trueVal: 5 }], it => it.trueVal, trueCmp);
  console.log('  ' + (single.sorted.length === 1 && single.oracleCalls === 0 ? '✓' : '✗') + ' single element (0 oracle calls)');
  const allEqual = sortWithProxy(Array.from({ length: 100 }, () => ({ trueVal: 42 })), it => it.trueVal, trueCmp);
  console.log('  ' + (allEqual.sorted.length === 100 ? '✓' : '✗') + ' all-equal');
}

console.log('');
if (failed > 0) { console.error('✗ ' + failed + ' failures'); process.exit(1); }
console.log('✓ sortWithProxy: all correctness checks pass');
