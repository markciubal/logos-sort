#!/usr/bin/env node
//
// logos-sort-proxy-demo
// ---------------------
// Demonstrates sortWithProxy on a realistic "expensive comparator" scenario:
// ranking N documents by quality, where the EXPENSIVE comparator is a
// simulated LLM judge (slow, costs money) and the CHEAP proxy is a fast
// heuristic score (e.g. embedding similarity, keyword density, a small model).
//
//   npx logos-sort-proxy-demo [n]
//
// Shows how many expensive "LLM calls" you save vs sorting with the judge
// directly.

const { sortWithProxy } = require('..');

const n = parseInt(process.argv[2], 10) || 500;

// Build N documents. Each has a hidden "true quality" the judge can assess,
// and a cheap heuristic score that correlates with quality but is noisy.
function makeRng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; }; }
const rng = makeRng(20260527);
function gauss() { const u1 = rng() || 1e-10, u2 = rng(); return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2); }

const docs = [];
for (let i = 0; i < n; i++) {
  const trueQuality = rng() * 100;
  // Cheap heuristic: correlated with true quality but noisy (proxy)
  const heuristicScore = trueQuality + gauss() * 12;
  docs.push({ id: i, trueQuality, heuristicScore });
}

// The EXPENSIVE comparator: a simulated LLM judge. Each call is "slow/costly".
// We count calls to show the savings. (In reality this would be an API call.)
let judgeCalls = 0;
function llmJudge(a, b) {
  judgeCalls++;
  // The judge is accurate (uses true quality). Real judges are noisier; see
  // the README note on intransitive comparators.
  return a.trueQuality < b.trueQuality ? -1 : a.trueQuality > b.trueQuality ? 1 : 0;
}

console.log('logos-sort-proxy-demo');
console.log('Ranking ' + n + ' documents by quality.\n');

// Baseline: sort with the judge directly (every comparison is an LLM call).
judgeCalls = 0;
const baseline = docs.slice().sort(llmJudge);
const baselineCalls = judgeCalls;

// Two-tier: cheap heuristic proxy + expensive judge only where needed.
const { sorted, oracleCalls, strategy } = sortWithProxy(
  docs,
  doc => doc.heuristicScore,   // cheap proxy
  llmJudge                      // expensive oracle
);

// Verify both produce the same ranking.
let identical = baseline.length === sorted.length;
for (let i = 0; i < baseline.length && identical; i++) {
  if (baseline[i].id !== sorted[i].id) identical = false;
}

console.log('  Sort directly with the judge:');
console.log('    LLM judge calls: ' + baselineCalls);
console.log('');
console.log('  Sort with cheap proxy + judge (sortWithProxy):');
console.log('    LLM judge calls: ' + oracleCalls + '   (strategy: ' + strategy + ')');
console.log('');
const saved = baselineCalls - oracleCalls;
const pct = ((saved / baselineCalls) * 100).toFixed(1);
console.log('  Saved ' + saved + ' expensive calls (' + pct + '% reduction).');
console.log('  Same final ranking: ' + (identical ? 'yes ✓' : 'NO ✗'));
console.log('');
if (typeof saved === 'number' && saved > 0) {
  // Illustrative cost framing
  const costPerCall = 0.01; // pretend each LLM call costs 1 cent
  console.log('  At $' + costPerCall.toFixed(2) + '/call, that is $' +
    (saved * costPerCall).toFixed(2) + ' saved on this single ranking.');
}
