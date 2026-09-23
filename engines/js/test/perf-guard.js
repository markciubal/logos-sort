// perf-guard.js — regression guard for performance cliffs that correctness
// tests cannot catch. Asserts that pathological-but-valid arrangements stay
// near their healthy time, not their cliff time. Bounds are generous (≈8x
// healthy, far below the cliff) so hardware noise never trips them; only a
// real algorithmic regression does.
function rng(s){return()=>{s=(s*1664525+1013904223)>>>0;return s/0x100000000;};}
function runPerfGuards(sort) {
  const N = 1000000;
  const r0 = rng(7); const SV = new Float64Array(N);
  for (let i = 0; i < N; i++) SV[i] = r0() * 1e9; SV.sort();
  const ok = a => { for (let i = 1; i < a.length; i++) if (a[i - 1] > a[i]) return false; return true; };
  // Disorder concentrated at the FRONT of an otherwise-sorted array. This fooled
  // the inversion sample + prefix run-length estimate into momentum insertion,
  // which went O(n*disp): historically 2446ms at 10%. Healthy is ~40ms.
  const cases = [];
  for (const frac of [0.05, 0.10]) {
    cases.push(['prefix-shuffle ' + (frac * 100) + '%', () => {
      const a = Float64Array.from(SV); const m = (N * frac) | 0; const r = rng(123);
      for (let i = m - 1; i > 0; i--) { const j = (r() * (i + 1)) | 0; const t = a[i]; a[i] = a[j]; a[j] = t; }
      return a;
    }]);
  }
  // Subnormal data: a value range narrower than ~1e-305 made flash sort's scale overflow to Infinity, every element
  // landed in bucket 0 and the closing insertion pass went quadratic: 77 s for 300k plain-Array subnormals, 9 s with a
  // third of the values subnormal (2026-09-20). Healthy is ~40 ms.
  for (const [label, every] of [['subnormal only', 1], ['third subnormal', 3]]) {
    cases.push([label, () => {
      const r = rng(99), n = 300001, a = new Array(n);
      for (let i = 0; i < n; i++) a[i] = (i % every === 0) ? (r() - 0.5) * 4e-311 : r() * 2e9 - 1e9;
      return a;
    }]);
  }
  // One NaN between two tight clusters: the gap-split partition's two scans both failed on it and sort() NEVER RETURNED
  // (6000 elements were enough; 2026-09-20). This case hangs the suite if that ever comes back, which is the point.
  cases.push(['NaN between modes', () => {
    const r = rng(5), n = 200001, half = n >> 1, a = new Array(n);
    for (let i = 0; i < n; i++) a[i] = i < half ? r() : i === half ? NaN : 1e9 + r();
    return a;
  }]);
  // Below the momentum threshold the nearly-sorted branch ran a PLAIN insertion sort with no budget. Its 40-pair probe bounds
  // inversions at one stride (length/40), so disorder that is dense but shorter than the stride, or sparse but far, passed it:
  // 45,000 elements with noise of +-400 took 24 ms and with 1% random swaps 47 ms, against ~2.3 (2026-09-20). Twenty such
  // arrays per case (one call, so that the guard's single limit applies): healthy ~50 ms, the cliff ~480 and ~900.
  for (const [label, make] of [['dense short disorder', (r, i) => i + (r() * 2 - 1) * 400], ['sparse far swaps', null]]) {
    cases.push([label, () => {
      const r = rng(31), n = 45000, reps = 20, a = new Float64Array(n * reps);
      for (let q = 0; q < reps; q++) { const o = q * n;
        for (let i = 0; i < n; i++) a[o + i] = make ? make(r, i) : i + r() * 0.5;
        if (!make) for (let k = 0; k < n / 100; k++) { const x = o + ((r() * n) | 0), y = o + ((r() * n) | 0); const v = a[x]; a[x] = a[y]; a[y] = v; } }
      a.chunks = reps; return a;
    }]);
  }
  const LIMIT_MS = 300;   // cliff was ~2400ms; healthy ~40ms
  let failures = 0;
  for (const [name, gen] of cases) {
    const run = a => { if (!a.chunks) { sort(a); return ok(a); } const m = a.length / a.chunks; let g = true;
      for (let q = 0; q < a.chunks; q++) { const c = a.subarray(q * m, (q + 1) * m); sort(c); if (!ok(c)) g = false; } return g; };
    run(gen()); // warm
    let best = Infinity, good = true;
    for (let k = 0; k < 3; k++) { const a = gen(); const t0 = performance.now(); const g = run(a); const dt = performance.now() - t0; if (dt < best) best = dt; if (!g) good = false; }
    const pass = good && best < LIMIT_MS;
    if (!pass) failures++;
    console.log('  ' + (pass ? '✓' : '✗') + ' ' + name.padEnd(20) + best.toFixed(0) + 'ms ' + (good ? '' : '(INCORRECT) ') + '(limit ' + LIMIT_MS + 'ms)');
  }
  console.log(failures === 0 ? '  ✓ no performance cliffs' : '  ✗ ' + failures + ' perf-cliff regression(s)');
  return { totalFailures: failures };
}
module.exports = { runPerfGuards };
