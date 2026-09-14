'use strict';
// sort-worker.js — one worker in the parallel sample sort.
// It operates on three SharedArrayBuffers handed in at construction:
//   input   (Float64Array)  the data being sorted
//   output  (Float64Array)  the destination, partitioned into P contiguous buckets
//   counts  (Int32Array, P*P)  worker w writes its per-bucket counts at row [w*P .. w*P+P)
// The main thread drives it through three barriered phases (hist -> scatter ->
// sortBucket). Phases 1-2 are linear passes; phase 3 sorts one bucket with the
// existing single-thread engine. All three are embarrassingly parallel across
// workers — the only serial glue (sampling, prefix sums) lives on the main thread.
const { parentPort, workerData } = require('worker_threads');
const engine = require('../logos-sort');

const { inSab, outSab, countsSab, id, P } = workerData;
const input = new Float64Array(inSab);
const output = new Float64Array(outSab);
const counts = new Int32Array(countsSab);

// bucket of x = number of splitters <= x (upper_bound). splitters ascending, len P-1.
function bucketOf(x, splitters) {
  let lo = 0, hi = splitters.length;
  while (lo < hi) { const m = (lo + hi) >>> 1; if (splitters[m] <= x) lo = m + 1; else hi = m; }
  return lo;
}

parentPort.on('message', (msg) => {
  if (msg.cmd === 'hist') {
    const { lo, hi, splitters } = msg;
    const local = new Int32Array(P);
    for (let i = lo; i < hi; i++) local[bucketOf(input[i], splitters)]++;
    for (let b = 0; b < P; b++) counts[id * P + b] = local[b];
    parentPort.postMessage({ done: true });
  } else if (msg.cmd === 'scatter') {
    const { lo, hi, splitters, offsets } = msg;
    const cur = Int32Array.from(offsets);           // this worker's write cursor per bucket
    for (let i = lo; i < hi; i++) { const x = input[i]; output[cur[bucketOf(x, splitters)]++] = x; }
    parentPort.postMessage({ done: true });
  } else if (msg.cmd === 'sortBucket') {
    const { blo, bhi } = msg;
    if (bhi > blo) engine.sortF64(output.subarray(blo, bhi));   // in-place on the shared buffer
    parentPort.postMessage({ done: true });
  } else if (msg.cmd === 'exit') {
    process.exit(0);
  }
});

parentPort.postMessage({ ready: true });
