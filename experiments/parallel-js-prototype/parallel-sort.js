'use strict';
// parallel-sort.js — parallel sample sort over SharedArrayBuffer + worker_threads,
// using the existing logos-sort engine as each worker's per-bucket local sort.
//
// Algorithm (classic sample sort):
//   1. SAMPLE the (unsorted) input; sort the small sample; pick P-1 splitters at
//      its quantiles. Good splitters => balanced buckets.                [serial, tiny]
//   2. HISTOGRAM: each worker counts, over its slice of input, how many elements
//      fall in each of the P buckets.                                   [parallel]
//   3. COMBINE the P*P count matrix into bucket sizes, bucket start offsets, and
//      each worker's per-bucket write cursor (prefix sums).             [serial, tiny]
//   4. SCATTER: each worker copies its input slice into the output buffer at its
//      cursors — output is now partitioned into P contiguous, unsorted buckets. [parallel]
//   5. SORT each bucket in place (bucket b -> worker b) with logos-sort.[parallel]
//   Output buckets are value-ordered and each is sorted => globally sorted.
//
// Every element is sorted exactly once (phase 5); phases 2 and 4 are two extra
// linear passes. The serial glue (1,3) is O(P^2 + sample). So the parallel
// fraction is high and the design scales until memory bandwidth saturates.
const { Worker } = require('worker_threads');
const path = require('path');
const engine = require('../logos-sort');

const WORKER = path.join(__dirname, 'sort-worker.js');
const SERIAL_THRESHOLD = 1 << 16;   // below this, parallel overhead isn't worth it

function spawn(P, inSab, outSab, countsSab) {
  const ws = [];
  const ready = [];
  for (let id = 0; id < P; id++) {
    const w = new Worker(WORKER, { workerData: { inSab, outSab, countsSab, id, P } });
    ws.push(w);
    ready.push(new Promise((res) => w.once('message', (m) => res(m))));  // first msg = {ready}
  }
  return { ws, ready: Promise.all(ready) };
}

function barrier(ws, msgFor) {
  return Promise.all(ws.map((w, i) => new Promise((res) => { w.once('message', res); w.postMessage(msgFor(i)); })));
}

async function parallelSortF64(data, P, t = {}) {
  const n = data.length;
  if (P <= 1 || n < SERIAL_THRESHOLD) { const s = now(); engine.sortF64(data); t.serialFallback = now() - s; return data; }

  let s;
  // 1. sample + splitters
  s = now();
  const oversample = 32, m = Math.min(n, P * oversample);
  const samp = new Float64Array(m);
  const step = Math.max(1, Math.floor(n / m));
  for (let i = 0, p = 0; i < m; i++, p += step) samp[i] = data[p < n ? p : n - 1];
  engine.sortF64(samp);
  const splitters = new Float64Array(P - 1);
  for (let b = 1; b < P; b++) splitters[b - 1] = samp[Math.floor(b * m / P)];
  t.sample = now() - s;

  // shared buffers + spawn
  s = now();
  const inSab = new SharedArrayBuffer(n * 8), outSab = new SharedArrayBuffer(n * 8), countsSab = new SharedArrayBuffer(P * P * 4);
  new Float64Array(inSab).set(data);
  const counts = new Int32Array(countsSab), out = new Float64Array(outSab);
  const { ws, ready } = spawn(P, inSab, outSab, countsSab);
  await ready;
  t.spawn = now() - s;

  const chunk = Math.ceil(n / P);
  const ranges = [];
  for (let id = 0; id < P; id++) ranges.push([id * chunk, Math.min(n, (id + 1) * chunk)]);

  // 2. histogram
  s = now();
  await barrier(ws, (i) => ({ cmd: 'hist', lo: ranges[i][0], hi: ranges[i][1], splitters }));
  t.hist = now() - s;

  // 3. combine -> bucket sizes, starts, per-worker cursors
  s = now();
  const bucketTotal = new Int32Array(P);
  for (let w = 0; w < P; w++) for (let b = 0; b < P; b++) bucketTotal[b] += counts[w * P + b];
  const bucketStart = new Int32Array(P);
  for (let b = 1; b < P; b++) bucketStart[b] = bucketStart[b - 1] + bucketTotal[b - 1];
  const woff = []; for (let w = 0; w < P; w++) woff.push(new Int32Array(P));
  for (let b = 0; b < P; b++) { let acc = bucketStart[b]; for (let w = 0; w < P; w++) { woff[w][b] = acc; acc += counts[w * P + b]; } }
  t.combine = now() - s;

  // 4. scatter
  s = now();
  await barrier(ws, (i) => ({ cmd: 'scatter', lo: ranges[i][0], hi: ranges[i][1], splitters, offsets: woff[i] }));
  t.scatter = now() - s;

  // 5. sort each bucket (bucket b -> worker b)
  s = now();
  await barrier(ws, (i) => ({ cmd: 'sortBucket', blo: bucketStart[i], bhi: bucketStart[i] + bucketTotal[i] }));
  t.bucketsort = now() - s;

  // copy back + teardown
  s = now();
  data.set(out);
  await Promise.all(ws.map((w) => w.terminate()));
  t.copyTeardown = now() - s;
  t.bucketBalance = balance(bucketTotal);
  return data;
}

function now() { return Number(process.hrtime.bigint()) / 1e6; }
function balance(sizes) { let mx = 0, sum = 0; for (const v of sizes) { if (v > mx) mx = v; sum += v; } return (mx / (sum / sizes.length)).toFixed(2); }

module.exports = { parallelSortF64 };

// ----- self-test + benchmark + projection when run directly -----
if (require.main === module) {
  (async () => {
    function rng(x) { return () => { x = (x * 1664525 + 1013904223) >>> 0; return x / 0x100000000; }; }
    const ok = (a) => { for (let i = 1; i < a.length; i++) if (a[i - 1] > a[i]) return false; return true; };
    const N = 8_000_000;
    const r = rng(7); const base = new Float64Array(N); for (let i = 0; i < N; i++) base[i] = r() * 1e9;

    // correctness across P
    for (const P of [2, 4, 8]) {
      const a = Float64Array.from(base); await parallelSortF64(a, P);
      const ref = Float64Array.from(base); ref.sort();
      let same = a.length === ref.length; for (let i = 0; same && i < a.length; i++) if (a[i] !== ref[i]) same = false;
      console.log(`correctness P=${P}: sorted=${ok(a)} matches-reference=${same}`);
    }
    console.log();

    // serial baseline
    const med = (fn, it) => { const t = []; for (let k = 0; k < it; k++) { const a = Float64Array.from(base); const s = now(); fn(a); t.push(now() - s); if (!ok(a)) throw 'bad'; } t.sort((x, y) => x - y); return t[t.length >> 1]; };
    const serial = med((a) => engine.sortF64(a), 5);
    console.log(`serial logos-sort.sortF64, n=${N / 1e6}M: ${serial.toFixed(1)}ms (single core, this box has 1)\n`);

    // parallel wall-clock + phase breakdown on THIS box (1 core -> expect slower)
    for (const P of [2, 4, 8]) {
      const t = {}; const a = Float64Array.from(base);
      const s = now(); await parallelSortF64(a, P, t); const wall = now() - s;
      const par = (t.hist || 0) + (t.scatter || 0) + (t.bucketsort || 0);
      const ser = (t.sample || 0) + (t.spawn || 0) + (t.combine || 0) + (t.copyTeardown || 0);
      console.log(`P=${P}: wall ${wall.toFixed(0)}ms  [sample ${f(t.sample)} spawn ${f(t.spawn)} hist ${f(t.hist)} combine ${f(t.combine)} scatter ${f(t.scatter)} bucketsort ${f(t.bucketsort)} copy ${f(t.copyTeardown)}]  balance ${t.bucketBalance}`);
      // projection: on P real cores the parallel phases divide by P; serial glue + spawn stay
      const projected = ser + par / P;
      console.log(`     parallelizable=${par.toFixed(0)}ms (${(par / (par + ser) * 100).toFixed(0)}%)  serial-glue=${ser.toFixed(0)}ms  =>  projected on ${P} real cores ~ ${projected.toFixed(0)}ms  (serial baseline ${serial.toFixed(0)}ms)`);
    }
    function f(x) { return (x || 0).toFixed(0) + 'ms'; }
  })().catch((e) => { console.error(e); process.exit(1); });
}
