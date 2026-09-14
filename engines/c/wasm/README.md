# logos-sort → threaded WebAssembly

How to run the C parallel sort in a browser or Node as WebAssembly, using all
available cores. **None of this was compiled or run in the environment it shipped
from** (no Emscripten/clang there) — it is the standard, correct recipe; build and
test it on your machine. The concurrency model itself *is* proven: the sibling
`logos-sort-parallel/` package runs the identical parallel sample sort under Node
worker_threads + SharedArrayBuffer today.

## The idea in one picture

```
main thread  ──(SharedArrayBuffer holds the data)──┐
                                                    │
   posts {ptr, n, nthreads} to a sort worker        │  shared linear memory
                                                    ▼  (WebAssembly.Memory, shared)
 sort worker ── calls wasm logos_sort_f64() ──► spawns pthreads == Web Workers
                                                    │  each sorts a disjoint bucket
                                                    ▼
                    result is in the same shared memory; main thread reads it
```

Two layers: **cores** come from Web Workers over shared memory; **speed** comes
from the compiled C leaf. Because `logos_psort.c` uses pthreads, Emscripten maps
`pthread_create` to pooled Workers automatically — no algorithm changes, only
build flags (see `build-wasm.sh`).

## Build

```sh
cd wasm
sh build-wasm.sh      # needs emcc on PATH (see the script header)
```

Produces `logos_psort.{node,web}.mjs`, a `.wasm`, and a pthread worker file.

## Use it — Node

```js
import createLogosSort from './logos_psort.node.mjs';
const M = await createLogosSort();

function sort_f64(arr /* Float64Array */, nthreads) {
  const n = arr.length;
  const ptr = M._malloc(n * 8);                 // allocate in the wasm heap
  M.HEAPF64.set(arr, ptr >> 3);                 // copy JS -> wasm
  M.ccall('logos_sort_f64', null,
          ['number', 'number', 'number'], [ptr, n, nthreads]);
  // re-read HEAPF64: the sort's internal malloc may have grown (detached) memory
  arr.set(M.HEAPF64.subarray(ptr >> 3, (ptr >> 3) + n));
  M._free(ptr);
}

const a = Float64Array.from({ length: 8_000_000 }, () => Math.random() * 1e9);
sort_f64(a, M._logos_num_cpus());
```

On Node this can run on the main thread: `pthread_join` uses `Atomics.wait`, which
Node permits off the render loop.

## Use it — browser (the part with real gotchas)

### 1. You MUST serve with these two headers

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without them `SharedArrayBuffer` is `undefined` and threads silently don't work.
Minimal dev server:

```js
// serve.mjs — node serve.mjs, then open http://localhost:8080
import http from 'node:http'; import { readFile } from 'node:fs/promises';
http.createServer(async (req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  try {
    const body = await readFile('.' + (req.url === '/' ? '/index.html' : req.url));
    const ext = req.url.split('.').pop();
    res.setHeader('Content-Type', { html:'text/html', mjs:'text/javascript',
      js:'text/javascript', wasm:'application/wasm' }[ext] || 'application/octet-stream');
    res.end(body);
  } catch { res.statusCode = 404; res.end('not found'); }
}).listen(8080);
```

### 2. Run the sort in a Worker, not the main thread

The browser main thread may not block on `Atomics.wait`, and `pthread_join`
blocks — so calling the sort from the main thread deadlocks. Do it in a Worker:

```js
// sort-worker.mjs
import createLogosSort from './logos_psort.web.mjs';
const M = await createLogosSort();
onmessage = (e) => {
  const { sab, n, nthreads } = e.data;        // sab: a SharedArrayBuffer of n*8 bytes
  const ptr = M._malloc(n * 8);
  M.HEAPF64.set(new Float64Array(sab), ptr >> 3);
  M.ccall('logos_sort_f64', null, ['number','number','number'], [ptr, n, nthreads]);
  new Float64Array(sab).set(M.HEAPF64.subarray(ptr >> 3, (ptr >> 3) + n));
  M._free(ptr);
  postMessage('done');
};
```

```js
// main.mjs
const w = new Worker('./sort-worker.mjs', { type: 'module' });
const sab = new SharedArrayBuffer(n * 8);
new Float64Array(sab).set(myData);
w.postMessage({ sab, n, nthreads: navigator.hardwareConcurrency });
w.onmessage = () => { /* new Float64Array(sab) is now sorted */ };
```

(For zero-copy you can also point the wasm heap directly at `sab`, but the
malloc/copy path above is simpler and robust to memory growth.)

## Choosing the core count

`navigator.hardwareConcurrency` (browser) or `logos_num_cpus()` (Node) is the
ceiling. Two caveats: it counts hyperthreads (which share execution units, so the
useful count is often lower for a bandwidth-bound sort), and other tabs/apps
compete. Passing `hardwareConcurrency` is a reasonable default; benchmark 1
below that if you want headroom.

## The one code change that matters for scaling

`coarse_f64` calls `calloc`/`malloc` per invocation for its bucket counters.
Emscripten's allocator is thread-safe but **lock-contended**: with P threads
sorting buckets at once, those allocations serialize on the allocator lock and
throttle scaling. Fix: give each worker a **per-thread scratch arena** (reused
across recursive calls) instead of allocating per call. Sketch:

```c
/* one arena per bucket-sorting thread, sized MAX_DEPTH * (2*COARSE_K+1) size_t */
static void coarse_f64(double *a, double *tmp, size_t n, int depth, size_t *arena);
/* inside: use arena at a depth-indexed offset for cnt/cur; no malloc/free */
```

This is the highest-value change for threaded WASM specifically. It's correct in
principle but untested — verify it before relying on it.

## Honest performance expectations

- **Per-core, WASM is ~1.5–2× slower than native C.** No auto-vectorization
  parity, mandatory bounds checks. Expect the 234 ms native sequential to land
  around ~350–470 ms in wasm — roughly the JS engine's league, but now
  parallelizable.
- **Scaling matches the native projection minus overhead:** ~2× toward 4 cores,
  flattening after because the coarse sort is **memory-bandwidth-bound** and cores
  share one bus. On a single socket, expect ~4–6× at 8 cores, not 8×. This is the
  wall behind the whole project; WASM doesn't change it.
- **Marshalling cost is real:** copying a JS array into the wasm heap and back is
  two O(n) passes. If the data lives its whole life in the shared heap, skip them.
- **Startup cost:** worker/pool spawn is tens of ms; amortize it by creating the
  module once and reusing it, not per sort.

## What does NOT port

Strings (the multikey/MSD string engine) are not in the C version and are a
separate, harder parallelization. Numbers only (f64, i32) here.
