#!/usr/bin/env node
//
// logos-sort-bench
// ----------------
// Races logos-sort against V8's Array.prototype.sort across a range of
// distributions and sizes. Reports wall-clock medians.
//
//   npx logos-sort-bench               # default: quick run, numbers + strings
//   npx logos-sort-bench --quick       # ~5 seconds total
//   npx logos-sort-bench --full        # ~30 seconds, more distributions
//   npx logos-sort-bench --numbers     # numeric only
//   npx logos-sort-bench --strings     # string only
//   npx logos-sort-bench --native      # also include native F64 path (if built)
//   npx logos-sort-bench --help

const lib = require('..');

// ---------- args ----------
const args = process.argv.slice(2);
function has(flag) { return args.includes(flag); }
if (has('--help') || has('-h')) {
  console.log(`Usage: logos-sort-bench [options]

Races logos-sort against V8's Array.prototype.sort.

Options:
  --quick        Quick benchmark (~5 seconds). Default.
  --full         Full benchmark with more distributions and sizes (~30s).
  --numbers      Numeric workloads only.
  --strings      String workloads only.
  --native       Include native F64 path (requires native addon to be built).
  --help, -h     Show this help.
`);
  process.exit(0);
}
const FULL = has('--full');
const NUMBERS_ONLY = has('--numbers');
const STRINGS_ONLY = has('--strings');
const INCLUDE_NATIVE = has('--native');

// ---------- generators (no deps) ----------
function makeRng(seed) {
  let s = seed >>> 0;
  return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; };
}
function gauss(rng) {
  let u1 = rng() || 1e-10, u2 = rng();
  return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
}
const numGens = {
  random:     (rng, n) => Array.from({ length: n }, () => rng() * 1e6),
  sorted:     (rng, n) => Array.from({ length: n }, (_, i) => i),
  reverse:    (rng, n) => Array.from({ length: n }, (_, i) => n - i),
  nearlySort: (rng, n) => { const a = Array.from({ length: n }, (_, i) => i); for (let k = 0; k < Math.floor(n * 0.01); k++) { const i = (rng() * n) | 0, j = (rng() * n) | 0; const t = a[i]; a[i] = a[j]; a[j] = t; } return a; },
  manyDupes:  (rng, n) => Array.from({ length: n }, () => (rng() * 32) | 0),
  logNormal:  (rng, n) => Array.from({ length: n }, () => Math.exp(gauss(rng) * 0.8 + 5)),
};
const strGens = {
  random:     (rng, n) => Array.from({ length: n }, () => { let s = ''; for (let i = 0; i < 8; i++) s += String.fromCharCode(97 + ((rng() * 26) | 0)); return s; }),
  longPrefix: (rng, n) => Array.from({ length: n }, () => { let s = 'https://api.example.com/v2/users/'; for (let i = 0; i < 8; i++) s += String.fromCharCode(97 + ((rng() * 26) | 0)); return s; }),
  sorted:     (rng, n) => Array.from({ length: n }, (_, i) => 'item_' + String(i).padStart(8, '0')),
  manyDupes:  (rng, n) => { const words = ['alpha', 'beta', 'gamma', 'delta', 'epsilon']; return Array.from({ length: n }, () => words[(rng() * words.length) | 0]); },
};

// ---------- sizes ----------
const quickSizes = { numbers: [1000, 10000, 100000, 1000000], strings: [1000, 10000, 100000] };
const fullSizes  = { numbers: [100, 1000, 10000, 100000, 1000000], strings: [100, 1000, 10000, 100000, 500000] };
const sizes = FULL ? fullSizes : quickSizes;

// ---------- bench harness ----------
function timeIt(fn, gen, trials) {
  // Warmup
  for (let i = 0; i < Math.min(5, trials); i++) fn(gen());
  const ts = new Float64Array(trials);
  for (let i = 0; i < trials; i++) {
    const a = gen();
    const t0 = performance.now();
    fn(a);
    ts[i] = performance.now() - t0;
  }
  return Array.from(ts).sort((a, b) => a - b)[Math.floor(trials / 2)];
}
function trialsFor(n) {
  if (n <= 1000) return 50;
  if (n <= 10000) return 20;
  if (n <= 100000) return 10;
  if (n <= 1000000) return 5;
  return 3;
}

// ---------- contenders ----------
const contenders = {
  numbers: [
    { name: 'logos sort',     fn: (a) => lib.sort(a) },
    { name: 'logos inplace',  fn: (a) => lib.sortInplace(a) },
    { name: 'V8 .sort()',     fn: (a) => a.sort((x, y) => x < y ? -1 : x > y ? 1 : 0) },
  ],
  strings: [
    { name: 'logos sort',     fn: (a) => lib.sort(a) },
    { name: 'logos inplace',  fn: (a) => lib.sortInplace(a) },
    { name: 'V8 .sort()',     fn: (a) => a.sort() },
  ],
};
if (INCLUDE_NATIVE && lib.nativeAvailable) {
  contenders.numbers.push({
    name: 'logos native F64',
    fn: (a) => lib.sortNativeF64(a),
    requiresF64: true,
  });
}

// ---------- formatting ----------
function pad(s, w, right) { s = String(s); const p = Math.max(0, w - s.length); return right ? ' '.repeat(p) + s : s + ' '.repeat(p); }
function fmtMs(t) { return t.toFixed(t < 1 ? 3 : 2) + 'ms'; }

// ---------- run ----------
function runOne(kind, gen, name, n) {
  const trials = trialsFor(n);

  // Pre-generate a source array per kind so each trial uses an identical copy
  const rng = makeRng(name.charCodeAt(0) * 17 + n);
  const src = gen(rng, n);

  // Standard contenders use a fresh slice each trial
  const results = [];
  for (const c of contenders[kind]) {
    if (c.requiresF64) {
      if (kind !== 'numbers') continue;
      const f64src = new Float64Array(src);
      const t = timeIt(c.fn, () => new Float64Array(f64src), trials);
      results.push({ name: c.name, time: t });
    } else {
      const t = timeIt(c.fn, () => src.slice(), trials);
      results.push({ name: c.name, time: t });
    }
  }

  // Find winner
  results.sort((a, b) => a.time - b.time);
  const winner = results[0];

  // Print row: distribution name, n, then each contender's time
  const parts = [pad(name, 12), pad('n=' + n, 10, true)];
  for (const c of contenders[kind]) {
    const r = results.find(x => x.name === c.name);
    if (!r) { parts.push(pad('—', 11, true)); continue; }
    const ratio = r.time / winner.time;
    const marker = r === winner ? '*' : ratio > 2 ? '!' : ' ';
    parts.push(pad(fmtMs(r.time) + marker, 11, true));
  }
  console.log('  ' + parts.join('  '));
}

function header(kind) {
  const cnames = contenders[kind].map(c => c.name);
  const parts = [pad('distribution', 12), pad('size', 10, true)];
  for (const c of cnames) parts.push(pad(c, 11, true));
  console.log('  ' + parts.join('  '));
  console.log('  ' + '-'.repeat(parts.join('  ').length));
}

function run(kind, gens) {
  console.log('');
  console.log('== ' + kind.toUpperCase() + ' ==');
  console.log('  (* = fastest, ! = >2× slower than fastest)');
  console.log('');
  header(kind);
  for (const [name, gen] of Object.entries(gens)) {
    for (const n of sizes[kind]) {
      runOne(kind, gen, name, n);
    }
    console.log('');
  }
}

console.log('logos-sort-bench  (' + (FULL ? 'full' : 'quick') + ' mode)');
console.log('  node ' + process.version + ' | ' + process.platform + '/' + process.arch);
if (INCLUDE_NATIVE && !lib.nativeAvailable) console.log('  warning: --native requested but native addon not built');

if (!STRINGS_ONLY) run('numbers', numGens);
if (!NUMBERS_ONLY) run('strings', strGens);

console.log('done.');
