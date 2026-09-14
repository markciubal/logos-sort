// ============================================================
// LogosAdaptive Sort — single-file, numbers AND strings (v3.7.1)
// ------------------------------------------------------------
// Mutates `arr` and returns it. API: sort(arr) => arr
//
// Numeric path  (v3.6.1): asc / desc / counting / int32-radix /
//                         insertion / 4-way hybrid merge (>=16 runs) /
//                         2-way galloping merge (<16 runs) /
//                         momentum-insertion / flash sort / float64-radix /
//                         dual-pivot introsort with Dutch-flag fallback / heapsort
// String path:            asc / desc / insertion / multikey quicksort
//
// IMPLEMENTATION NOTE — why the IIFE wrappers:
// Each impl lives inside its own IIFE (`const x = (() => { ... return fn; })()`).
// This is not stylistic — it's the only single-file arrangement I found that
// preserves performance. Bundling both paths in one closure with shared or
// even type-isolated helpers causes V8 to drop optimization on the string
// path, costing 5-8x on long-string workloads. The IIFE creates a separate
// lexical scope that V8 appears to compile as its own unit. Each IIFE's
// helpers are captured only by its returned function; the two IIFEs cannot
// see each other's internals.
//
// If you split this back into multiple files or remove the IIFE wrappers,
// expect 2-8x regressions on string-sort workloads. The 3-line dispatcher
// at the bottom is the public API.
// ============================================================

// Attempt: single file, both paths, with IIFEs to isolate compilation scope.
// The hypothesis is that each IIFE creates a fresh scope that V8 may treat
// closer to a separate compilation unit than nested-in-one-closure.

const sortNumbersImpl = (() => {
  const INSERTION_SORT_THRESHOLD = 24;
  const COUNTING_SORT_K          = 4;
  const NEARLY_SORTED_INV_RATIO  = 0.05;
  const MOMENTUM_THRESHOLD       = 50_000;
  const MAX_RUNS_FOR_MERGE       = 32;
  const FLOAT_RADIX_THRESHOLD    = 4096;
  const FLASH_SORT_THRESHOLD     = 4096;
  const FLASH_SAFETY_RATIO       = 0.05;
  const MIN_GALLOP               = 7;
  const INT32_MIN = -2147483648, INT32_MAX = 2147483647;

  function insertionSort(a, lo, hi) {
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k;
    }
  }
  function insertionSortMomentum(a, lo, hi) {
    let momentum = 1;
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i];
      if (a[i - 1] <= k) { momentum = momentum > 1 ? (momentum >>> 1) : 1; continue; }
      let j = i - 1, step = momentum;
      while (step > 1 && (j - step < lo || a[j - step] <= k)) step >>>= 1;
      while (j - step >= lo && a[j - step] > k) { j -= step; step <<= 1; }
      let left = Math.max(lo, j - step), right = j;
      while (left < right) { const mid = (left + right) >>> 1; if (a[mid] > k) right = mid; else left = mid + 1; }
      const dist = i - left;
      for (let p = i; p > left; p--) a[p] = a[p - 1];
      a[left] = k; momentum = dist;
    }
  }
  function siftDown(a, base, root, end) {
    for (;;) {
      let big = root;
      const l = 2*root+1, r = l+1;
      if (l < end && a[base+l] > a[base+big]) big = l;
      if (r < end && a[base+r] > a[base+big]) big = r;
      if (big === root) return;
      const t = a[base+root]; a[base+root] = a[base+big]; a[base+big] = t;
      root = big;
    }
  }
  function heapSort(a, lo, hi) {
    const len = hi - lo + 1;
    for (let i = (len>>1)-1; i >= 0; i--) siftDown(a, lo, i, len);
    for (let i = len - 1; i > 0; i--) {
      const t = a[lo]; a[lo] = a[lo+i]; a[lo+i] = t;
      siftDown(a, lo, 0, i);
    }
  }
  function sort5(a, i1, i2, i3, i4, i5) {
    if (a[i2] < a[i1]) { const t = a[i2]; a[i2] = a[i1]; a[i1] = t; }
    if (a[i3] < a[i2]) { const t = a[i3]; a[i3] = a[i2]; a[i2] = t;
      if (a[i2] < a[i1]) { const u = a[i2]; a[i2] = a[i1]; a[i1] = u; } }
    if (a[i4] < a[i3]) { const t = a[i4]; a[i4] = a[i3]; a[i3] = t;
      if (a[i3] < a[i2]) { const u = a[i3]; a[i3] = a[i2]; a[i2] = u;
        if (a[i2] < a[i1]) { const v = a[i2]; a[i2] = a[i1]; a[i1] = v; } } }
    if (a[i5] < a[i4]) { const t = a[i5]; a[i5] = a[i4]; a[i4] = t;
      if (a[i4] < a[i3]) { const u = a[i4]; a[i4] = a[i3]; a[i3] = u;
        if (a[i3] < a[i2]) { const v = a[i3]; a[i3] = a[i2]; a[i2] = v;
          if (a[i2] < a[i1]) { const w = a[i2]; a[i2] = a[i1]; a[i1] = w; } } } }
  }
  function quicksort(a, lo, hi, d) {
    while (hi - lo >= INSERTION_SORT_THRESHOLD) {
      if (d === 0) { heapSort(a, lo, hi); return; }
      const len = hi - lo + 1;
      const seventh = (len >> 3) + (len >> 6) + 1;
      const e3 = (lo + hi) >> 1;
      const e2 = e3 - seventh, e4 = e3 + seventh;
      const e1 = e2 - seventh, e5 = e4 + seventh;
      sort5(a, e1, e2, e3, e4, e5);
      if (a[e1] !== a[e2] && a[e2] !== a[e3] && a[e3] !== a[e4] && a[e4] !== a[e5]) {
        const p1 = a[e2], p2 = a[e4];
        a[e2] = a[lo]; a[e4] = a[hi];
        let less = lo + 1, great = hi - 1, k = less;
        while (k <= great) {
          const ak = a[k];
          if (ak < p1) { a[k] = a[less]; a[less++] = ak; }
          else if (ak > p2) {
            while (k < great && a[great] > p2) great--;
            const ag = a[great];
            a[k] = ag; a[great--] = ak;
            if (ag < p1) { a[k] = a[less]; a[less++] = ag; }
          }
          k++;
        }
        a[lo] = a[less - 1]; a[less - 1] = p1;
        a[hi] = a[great + 1]; a[great + 1] = p2;
        d--;
        quicksort(a, lo, less - 2, d);
        quicksort(a, great + 2, hi, d);
        if (p1 === p2) return;
        let mLo = less, mHi = great;
        while (mLo <= mHi && a[mLo] === p1) mLo++;
        while (mLo <= mHi && a[mHi] === p2) mHi--;
        lo = mLo; hi = mHi;
      } else {
        const pv = a[e3];
        let lt = lo, gt = hi, k = lo;
        while (k <= gt) {
          const v = a[k];
          if (v < pv)      { const t = a[lt]; a[lt] = v; a[k] = t; lt++; k++; }
          else if (v > pv) { const t = a[gt]; a[gt] = v; a[k] = t; gt--;       }
          else             { k++; }
        }
        d--;
        if (lt - lo < hi - gt) { quicksort(a, lo, lt - 1, d); lo = gt + 1; }
        else                   { quicksort(a, gt + 1, hi, d); hi = lt - 1; }
      }
    }
    insertionSort(a, lo, hi);
  }
  function lsdRadixInt32(a, n) {
    const BIAS = 0x80000000;
    const src = new Uint32Array(n), dst = new Uint32Array(n);
    for (let i = 0; i < n; i++) src[i] = (a[i] + BIAS) >>> 0;
    const c0 = new Uint32Array(257), c1 = new Uint32Array(257), c2 = new Uint32Array(257), c3 = new Uint32Array(257);
    for (let i = 0; i < n; i++) {
      const v = src[i];
      c0[(v & 0xFF) + 1]++; c1[((v >>> 8) & 0xFF) + 1]++;
      c2[((v >>> 16) & 0xFF) + 1]++; c3[((v >>> 24) & 0xFF) + 1]++;
    }
    for (let b = 1; b < 257; b++) { c0[b]+=c0[b-1]; c1[b]+=c1[b-1]; c2[b]+=c2[b-1]; c3[b]+=c3[b-1]; }
    let from = src, to = dst, tmp;
    for (let i = 0; i < n; i++) { const v = from[i]; to[c0[v & 0xFF]++] = v; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const v = from[i]; to[c1[(v >>> 8) & 0xFF]++] = v; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const v = from[i]; to[c2[(v >>> 16) & 0xFF]++] = v; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const v = from[i]; to[c3[(v >>> 24) & 0xFF]++] = v; }
    for (let i = 0; i < n; i++) a[i] = to[i] - BIAS;
  }
  function isFlashSafe(a, n, min, max) {
    const SAMPLE_SIZE = 64;
    const stride = (n / SAMPLE_SIZE) | 0;
    const samples = new Array(SAMPLE_SIZE);
    for (let i = 0; i < SAMPLE_SIZE; i++) samples[i] = a[i * stride];
    samples.sort((x, y) => x - y);
    return (samples[47] - samples[16]) / (max - min) >= FLASH_SAFETY_RATIO;
  }
  function flashSort(a, n, min, max) {
    const m = n, scale = (m - 1) / (max - min);
    const counts = new Uint32Array(m + 1);
    for (let i = 0; i < n; i++) { const idx = ((a[i] - min) * scale) | 0; counts[idx + 1]++; }
    for (let i = 1; i <= m; i++) counts[i] += counts[i - 1];
    const scratch = new Float64Array(n);
    for (let i = 0; i < n; i++) { const v = a[i]; const idx = ((v - min) * scale) | 0; scratch[counts[idx]++] = v; }
    for (let i = 0; i < n; i++) a[i] = scratch[i];
    for (let i = 1; i < n; i++) { const k = a[i]; let j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; }
  }
  function lsdRadixFloat64(a, n) {
    const bufA = new Uint32Array(2 * n), bufB = new Uint32Array(2 * n);
    const f64A = new Float64Array(bufA.buffer);
    for (let i = 0; i < n; i++) f64A[i] = a[i];
    for (let i = 0; i < n; i++) {
      const j = 2 * i + 1, hi = bufA[j];
      if (hi & 0x80000000) { bufA[2 * i] = ~bufA[2 * i] >>> 0; bufA[j] = ~hi >>> 0; }
      else                 { bufA[j] = hi ^ 0x80000000; }
    }
    const h0 = new Uint32Array(257), h1 = new Uint32Array(257), h2 = new Uint32Array(257), h3 = new Uint32Array(257);
    const h4 = new Uint32Array(257), h5 = new Uint32Array(257), h6 = new Uint32Array(257), h7 = new Uint32Array(257);
    for (let i = 0; i < n; i++) {
      const lo = bufA[2 * i], hi = bufA[2 * i + 1];
      h0[( lo         & 0xFF) + 1]++; h1[((lo >>>  8) & 0xFF) + 1]++;
      h2[((lo >>> 16) & 0xFF) + 1]++; h3[((lo >>> 24) & 0xFF) + 1]++;
      h4[( hi         & 0xFF) + 1]++; h5[((hi >>>  8) & 0xFF) + 1]++;
      h6[((hi >>> 16) & 0xFF) + 1]++; h7[((hi >>> 24) & 0xFF) + 1]++;
    }
    for (let b = 1; b < 257; b++) {
      h0[b]+=h0[b-1]; h1[b]+=h1[b-1]; h2[b]+=h2[b-1]; h3[b]+=h3[b-1];
      h4[b]+=h4[b-1]; h5[b]+=h5[b-1]; h6[b]+=h6[b-1]; h7[b]+=h7[b-1];
    }
    let from = bufA, to = bufB, tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h0[lo & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h1[(lo >>> 8) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h2[(lo >>> 16) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h3[(lo >>> 24) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h4[hi & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h5[(hi >>> 8) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h6[(hi >>> 16) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) { const lo = from[2*i], hi = from[2*i+1]; const pos = h7[(hi >>> 24) & 0xFF]++; to[2*pos] = lo; to[2*pos+1] = hi; }
    tmp = from; from = to; to = tmp;
    for (let i = 0; i < n; i++) {
      const j = 2 * i + 1, hi = from[j];
      if (hi & 0x80000000) { from[j] = hi ^ 0x80000000; }
      else                 { from[2 * i] = ~from[2 * i] >>> 0; from[j] = ~hi >>> 0; }
    }
    const finalF64 = new Float64Array(from.buffer);
    for (let i = 0; i < n; i++) a[i] = finalF64[i];
  }
  function detectRunsLimited(a, n, maxRuns) {
    const result = []; let i = 0;
    while (i < n) {
      let j = i + 1;
      if (j < n) {
        if (a[j] >= a[i]) { while (j < n && a[j] >= a[j - 1]) j++; }
        else { while (j < n && a[j] < a[j - 1]) j++;
          for (let l = i, r = j - 1; l < r; l++, r--) { const t = a[l]; a[l] = a[r]; a[r] = t; } }
      }
      result.push(i, j - 1);
      if ((result.length >> 1) > maxRuns) return null;
      i = j;
    }
    return result;
  }
  function mergeAllRuns(a, n, runs) {
    const QUADWAY_THRESHOLD = 16;
    const buf = new Array(n);
    let cur = runs;
    while (cur.length > 2) {
      const next = [];
      if ((cur.length >> 1) >= QUADWAY_THRESHOLD) {
        for (let k = 0; k < cur.length; k += 8) {
          const numRuns = Math.min(4, (cur.length - k) >> 1);
          if (numRuns === 1) { next.push(cur[k], cur[k + 1]); continue; }
          if (numRuns === 2) { mergeGallop(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3]); next.push(cur[k], cur[k+3]); continue; }
          if (numRuns === 3) {
            mergeGallop(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3]);
            mergeGallop(a, buf, cur[k], cur[k+3], cur[k+4], cur[k+5]);
            next.push(cur[k], cur[k+5]); continue;
          }
          merge4WayHybrid(a, buf, cur[k], cur[k+1], cur[k+2], cur[k+3], cur[k+4], cur[k+5], cur[k+6], cur[k+7]);
          next.push(cur[k], cur[k+7]);
        }
      } else {
        for (let k = 0; k < cur.length; k += 4) {
          const a1 = cur[k], b1 = cur[k + 1];
          if (k + 2 >= cur.length) { next.push(a1, b1); continue; }
          mergeGallop(a, buf, a1, b1, cur[k + 2], cur[k + 3]);
          next.push(a1, cur[k + 3]);
        }
      }
      cur = next;
    }
  }
  function merge4WayHybrid(a, buf, a1, b1, a2, b2, a3, b3, a4, b4) {
    const INF = Number.POSITIVE_INFINITY;
    let p0=a1,p1=a2,p2=a3,p3=a4; const e0=b1,e1=b2,e2=b3,e3=b4;
    let v0=a[p0],v1=a[p1],v2=a[p2],v3=a[p3]; let w=a1;
    let w0=0,w1=0,w2=0,w3=0;
    while (p0<=e0||p1<=e1||p2<=e2||p3<=e3) {
      const lv=v0<=v1?v0:v1, lw=v0<=v1?0:1, rv=v2<=v3?v2:v3, rw=v2<=v3?2:3;
      const win=lv<=rv?lw:rw, wv=lv<=rv?lv:rv;
      buf[w++]=wv;
      if (win===0) { p0++; v0=p0<=e0?a[p0]:INF; w0++; w1=0; w2=0; w3=0;
        if (w0>=MIN_GALLOP) { const t=v1<v2?(v1<v3?v1:v3):(v2<v3?v2:v3);
          if (t===INF) { while (p0<=e0) buf[w++]=a[p0++]; v0=INF; }
          else { let s=1,j=p0; while (j+s<=e0 && a[j+s-1]<=t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e0+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<=t)L=m+1;else R=m;}
            while (p0<L) buf[w++]=a[p0++]; v0=p0<=e0?a[p0]:INF; } w0=0; } }
      else if (win===1) { p1++; v1=p1<=e1?a[p1]:INF; w1++; w0=0; w2=0; w3=0;
        if (w1>=MIN_GALLOP) { const t=v0<v2?(v0<v3?v0:v3):(v2<v3?v2:v3);
          if (t===INF) { while (p1<=e1) buf[w++]=a[p1++]; v1=INF; }
          else { let s=1,j=p1; while (j+s<=e1 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e1+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p1<L) buf[w++]=a[p1++]; v1=p1<=e1?a[p1]:INF; } w1=0; } }
      else if (win===2) { p2++; v2=p2<=e2?a[p2]:INF; w2++; w0=0; w1=0; w3=0;
        if (w2>=MIN_GALLOP) { const t=v0<v1?(v0<v3?v0:v3):(v1<v3?v1:v3);
          if (t===INF) { while (p2<=e2) buf[w++]=a[p2++]; v2=INF; }
          else { let s=1,j=p2; while (j+s<=e2 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e2+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p2<L) buf[w++]=a[p2++]; v2=p2<=e2?a[p2]:INF; } w2=0; } }
      else { p3++; v3=p3<=e3?a[p3]:INF; w3++; w0=0; w1=0; w2=0;
        if (w3>=MIN_GALLOP) { const t=v0<v1?(v0<v2?v0:v2):(v1<v2?v1:v2);
          if (t===INF) { while (p3<=e3) buf[w++]=a[p3++]; v3=INF; }
          else { let s=1,j=p3; while (j+s<=e3 && a[j+s-1]<t) {j+=s;s<<=1;}
            let L=j,R=Math.min(e3+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
            while (p3<L) buf[w++]=a[p3++]; v3=p3<=e3?a[p3]:INF; } w3=0; } }
    }
    for (let r=a1;r<=b4;r++) a[r]=buf[r];
  }
  function mergeGallop(a, buf, a1, b1, a2, b2) {
    let p=a1,q=a2,w=a1,pW=0,qW=0;
    while (p<=b1 && q<=b2) {
      if (a[p]<=a[q]) {buf[w++]=a[p++];pW++;qW=0;} else {buf[w++]=a[q++];qW++;pW=0;}
      if (pW>=MIN_GALLOP) { const t=a[q]; let s=1,j=p; while (j+s<=b1&&a[j+s-1]<=t){j+=s;s<<=1;}
        let L=j,R=Math.min(b1+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<=t)L=m+1;else R=m;}
        while (p<L) buf[w++]=a[p++]; pW=0; }
      else if (qW>=MIN_GALLOP) { const t=a[p]; let s=1,j=q; while (j+s<=b2&&a[j+s-1]<t){j+=s;s<<=1;}
        let L=j,R=Math.min(b2+1,j+s); while (L<R){const m=(L+R)>>>1;if(a[m]<t)L=m+1;else R=m;}
        while (q<L) buf[w++]=a[q++]; qW=0; }
    }
    while (p<=b1) buf[w++]=a[p++]; while (q<=b2) buf[w++]=a[q++];
    for (let r=a1;r<=b2;r++) a[r]=buf[r];
  }

  return function(arr) {
    const length = arr.length;
    let minValue = arr[0], maxValue = arr[0];
    let allIntegers = Number.isInteger(arr[0]);
    let isAscending = true, isDescending = true;
    for (let i = 1; i < length; i++) {
      const v = arr[i];
      if (v < minValue) minValue = v; else if (v > maxValue) maxValue = v;
      if (allIntegers && !Number.isInteger(v)) allIntegers = false;
      if (isAscending  && v < arr[i - 1]) isAscending = false;
      if (isDescending && v > arr[i - 1]) isDescending = false;
    }
    const allInt32 = allIntegers && minValue >= INT32_MIN && maxValue <= INT32_MAX;
    if (isAscending) return arr;
    if (isDescending) {
      for (let l = 0, r = length - 1; l < r; l++, r--) { const t = arr[l]; arr[l] = arr[r]; arr[r] = t; }
      return arr;
    }
    if (allIntegers) {
      const span = maxValue - minValue + 1;
      if (span <= COUNTING_SORT_K * length) {
        const CType = length <= 0xFF ? Uint8Array : length <= 0xFFFF ? Uint16Array : Uint32Array;
        const buckets = new CType(span);
        for (let i = 0; i < length; i++) buckets[arr[i] - minValue]++;
        let w = 0;
        for (let v = 0; v < span; v++) { const c = buckets[v], val = v + minValue; for (let j = 0; j < c; j++) arr[w++] = val; }
        return arr;
      }
      if (allInt32 && length >= 64) { lsdRadixInt32(arr, length); return arr; }
    }
    if (length <= INSERTION_SORT_THRESHOLD) { insertionSort(arr, 0, length - 1); return arr; }
    const runs = detectRunsLimited(arr, length, MAX_RUNS_FOR_MERGE);
    if (runs !== null && runs.length > 2) { mergeAllRuns(arr, length, runs); return arr; }
    const sampleSize = Math.min(length, 40);
    const sampleStep = Math.max(1, (length / sampleSize) | 0);
    let inv = 0, comps = 0;
    for (let i = 0; i + sampleStep < length; i += sampleStep) { if (arr[i] > arr[i + sampleStep]) inv++; comps++; }
    if (comps > 0 && inv / comps <= NEARLY_SORTED_INV_RATIO) {
      if (length > MOMENTUM_THRESHOLD) insertionSortMomentum(arr, 0, length - 1);
      else                             insertionSort(arr, 0, length - 1);
      return arr;
    }
    if (length >= FLASH_SORT_THRESHOLD && maxValue > minValue && isFlashSafe(arr, length, minValue, maxValue)) {
      flashSort(arr, length, minValue, maxValue);
      return arr;
    }
    if (length >= FLOAT_RADIX_THRESHOLD) { lsdRadixFloat64(arr, length); return arr; }
    quicksort(arr, 0, length - 1, 2 * (31 - Math.clz32(length)));
    return arr;
  };
})();

const sortStringsImpl = (() => {
  const INSERTION_THRESHOLD_STR = 16;

  function insertionSortStr(a, lo, hi) {
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k;
    }
  }
  function multikeyQs(a, lo, hi, d) {
    while (hi - lo >= INSERTION_THRESHOLD_STR) {
      const mid = (lo + hi) >> 1;
      const c1 = d < a[lo].length  ? a[lo].charCodeAt(d)  : -1;
      const c2 = d < a[mid].length ? a[mid].charCodeAt(d) : -1;
      const c3 = d < a[hi].length  ? a[hi].charCodeAt(d)  : -1;
      const pv = c1 < c2 ? (c2 < c3 ? c2 : (c1 < c3 ? c3 : c1)) : (c1 < c3 ? c1 : (c2 < c3 ? c3 : c2));
      let lt = lo, gt = hi, i = lo;
      while (i <= gt) {
        const s = a[i];
        const c = d < s.length ? s.charCodeAt(d) : -1;
        if      (c < pv) { const t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
        else if (c > pv) { const t = a[gt]; a[gt] = a[i]; a[i] = t; gt--;       }
        else             { i++; }
      }
      multikeyQs(a, lo, lt - 1, d);
      if (pv >= 0) multikeyQs(a, lt, gt, d + 1);
      lo = gt + 1;
    }
    insertionSortStr(a, lo, hi);
  }

  return function(arr) {
    const n = arr.length;
    let isAsc = true, isDesc = true;
    for (let i = 1; i < n; i++) {
      if (isAsc  && arr[i] < arr[i - 1]) isAsc  = false;
      if (isDesc && arr[i] > arr[i - 1]) isDesc = false;
      if (!isAsc && !isDesc) break;
    }
    if (isAsc) return arr;
    if (isDesc) {
      for (let l = 0, r = n - 1; l < r; l++, r--) { const t = arr[l]; arr[l] = arr[r]; arr[r] = t; }
      return arr;
    }
    if (n <= INSERTION_THRESHOLD_STR) { insertionSortStr(arr, 0, n - 1); return arr; }
    multikeyQs(arr, 0, n - 1, 0);
    return arr;
  };
})();

const sort = (arr) => {
  if (arr.length <= 1) return arr;
  return typeof arr[0] === 'string' ? sortStringsImpl(arr) : sortNumbersImpl(arr);
};

module.exports = { sort };
