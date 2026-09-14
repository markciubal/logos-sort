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
  const LIMIT_MS = 300;   // cliff was ~2400ms; healthy ~40ms
  let failures = 0;
  for (const [name, gen] of cases) {
    sort(gen()); // warm
    let best = Infinity, good = true;
    for (let k = 0; k < 3; k++) { const a = gen(); const t0 = performance.now(); sort(a); const dt = performance.now() - t0; if (dt < best) best = dt; if (!ok(a)) good = false; }
    const pass = good && best < LIMIT_MS;
    if (!pass) failures++;
    console.log('  ' + (pass ? '✓' : '✗') + ' ' + name.padEnd(20) + best.toFixed(0) + 'ms ' + (good ? '' : '(INCORRECT) ') + '(limit ' + LIMIT_MS + 'ms)');
  }
  console.log(failures === 0 ? '  ✓ no performance cliffs' : '  ✗ ' + failures + ' perf-cliff regression(s)');
  return { totalFailures: failures };
}
module.exports = { runPerfGuards };
