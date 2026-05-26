module.exports = (() => {
// ============================================================
// LogosAdaptive Sort — v3.8 (buffered)
// ------------------------------------------------------------
// Numbers: counting / int32-radix / insertion / 4-way hybrid merge /
//          2-way galloping merge / momentum insertion /
//          multimodal gap-partition + recurse /
//          flash sort / float64-radix / dual-pivot introsort / heapsort.
// Strings: TimSort-style run detection + merge (probe-gated) /
//          MSD-at-depth radix (after common-prefix skip) /
//          insertion sort for small ranges.
//
// v3.7.8 additions (kept):
//   * Predictive free-merge fast path in mergeGallop/merge4WayHybrid/mergeStr:
//     if (a[b1] <= a[a2]) return;
//     Before merging two adjacent sorted runs, check whether the right run's
//     first element is already >= the left run's last element. If so, the
//     runs are already in merged order — return without copying anything.
//
// v3.8 changes:
//   * Replaced multikey quicksort AND the old 2-char MSD prefix radix with
//     a single unified MSD-at-depth algorithm:
//       1. Compute common prefix length across the full array (O(n × L_prefix))
//       2. Run MSD radix on one character at a time, starting at that depth
//       3. Each radix pass splits into 256 buckets in O(n) — equivalent to
//          ~log2(256) = 8 levels of multikey QS's 3-way partition in one pass
//       4. Recurse on each non-empty bucket
//       5. Insertion sort for ranges <= 16 elements
//     Wins 1.37-1.55× over the prior multikey+2-char-radix combo on every
//     string workload tested (URLs, paths, shortStr, logLines), with biggest
//     absolute savings at large n (e.g., URLs n=1M: 1016ms → 743ms).
//     Removed ~50 lines of obsolete code (msdPrefixSort, prefixCharCode,
//     prefixCounts, the 2-byte sentinel-shifted encoding, and the
//     diversity probe in the dispatcher).
//
// Auxiliary memory: pool ~16 bytes/n (numeric paths). String MSD-at-depth
// allocates one Array(n) scratch + small Int32Array(258) counts per
// recursive call; multimodal partition recurses via O(n) slices. Otherwise
// in-place.
// ============================================================


const pool = {
  cap: 0,
  u32A: null,          // Uint32Array(2*cap) — radix scratch + alias backing for f64Buf
  u32B: null,          // Uint32Array(2*cap) — radix scratch + alias backing for counts
  f64Buf: null,        // Float64Array view of u32A — flash scratch AND merge buf
  counts: null,        // Uint32Array view of u32B — flash sort counts
};
const HIST0 = new Uint32Array(257), HIST1 = new Uint32Array(257);
const HIST2 = new Uint32Array(257), HIST3 = new Uint32Array(257);
const HIST4 = new Uint32Array(257), HIST5 = new Uint32Array(257);
const HIST6 = new Uint32Array(257), HIST7 = new Uint32Array(257);

function ensurePool(n) {
  if (n <= pool.cap) return;
  const cap = Math.max(n, pool.cap * 2 || 1024);
  pool.u32A = new Uint32Array(2 * cap);
  pool.u32B = new Uint32Array(2 * cap);
  // Views alias the same memory — 2*cap Uint32 = cap Float64 = cap+1 Uint32 (with slack).
  pool.f64Buf = new Float64Array(pool.u32A.buffer, 0, cap);
  pool.counts = new Uint32Array(pool.u32B.buffer, 0, cap + 1);
  pool.cap = cap;
}

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
    ensurePool(n);
    const BIAS = 0x80000000;
    const src = pool.u32A, dst = pool.u32B;
    for (let i = 0; i < n; i++) src[i] = (a[i] + BIAS) >>> 0;
    HIST0.fill(0); HIST1.fill(0); HIST2.fill(0); HIST3.fill(0);
    const c0 = HIST0, c1 = HIST1, c2 = HIST2, c3 = HIST3;
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
    ensurePool(n);
    const m = n, scale = (m - 1) / (max - min);
    const counts = pool.counts;
    for (let i = 0; i <= m; i++) counts[i] = 0;
    for (let i = 0; i < n; i++) { const idx = ((a[i] - min) * scale) | 0; counts[idx + 1]++; }
    for (let i = 1; i <= m; i++) counts[i] += counts[i - 1];
    const scratch = pool.f64Buf;
    for (let i = 0; i < n; i++) { const v = a[i]; const idx = ((v - min) * scale) | 0; scratch[counts[idx]++] = v; }
    for (let i = 0; i < n; i++) a[i] = scratch[i];
    for (let i = 1; i < n; i++) { const k = a[i]; let j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; }
  }
  function lsdRadixFloat64(a, n) {
    ensurePool(n);
    const bufA = pool.u32A, bufB = pool.u32B;
    const f64A = new Float64Array(bufA.buffer, 0, n);
    for (let i = 0; i < n; i++) f64A[i] = a[i];
    for (let i = 0; i < n; i++) {
      const j = 2 * i + 1, hi = bufA[j];
      if (hi & 0x80000000) { bufA[2 * i] = ~bufA[2 * i] >>> 0; bufA[j] = ~hi >>> 0; }
      else                 { bufA[j] = hi ^ 0x80000000; }
    }
    HIST0.fill(0); HIST1.fill(0); HIST2.fill(0); HIST3.fill(0);
    HIST4.fill(0); HIST5.fill(0); HIST6.fill(0); HIST7.fill(0);
    const h0=HIST0,h1=HIST1,h2=HIST2,h3=HIST3,h4=HIST4,h5=HIST5,h6=HIST6,h7=HIST7;
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
    const finalF64 = new Float64Array(from.buffer, 0, n);
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
    ensurePool(n);
    const QUADWAY_THRESHOLD = 16;
    // Merge buffer now lives in pool.f64Buf (a Float64Array view).
    // Mutually exclusive with flashSort, which also uses pool.f64Buf.
    const buf = pool.f64Buf;
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
    if (a[b1] <= a[a2] && a[b2] <= a[a3] && a[b3] <= a[a4]) return;  // predictive free-merge
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
    if (a[b1] <= a[a2]) return;  // predictive free-merge: runs already in order
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

  return function sortFn(arr) {
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
    if (length >= FLASH_SORT_THRESHOLD && maxValue > minValue) {
      // Sample-based gap detection (multimodal-aware). Replaces the
      // standalone isFlashSafe call — we now do both checks (IQR safety
      // AND gap-vs-median ratio) from a single set of sorted samples.
      //
      // Why: flash sort's linear interpolation passes the IQR check on
      // bimodal data (Q1 lies in one peak, Q3 in the other, IQR ratio ≈
      // 1.0), but then dumps each cluster into ~one bucket each, leaving
      // the insertion-sort cleanup pass to do O(n²) work within each.
      // Catching the gap before flash sort fires turns O(n²) into O(n).
      const SAMPLE_SIZE = 64;
      const sStride = (length / SAMPLE_SIZE) | 0;
      const samples = new Array(SAMPLE_SIZE);
      for (let i = 0; i < SAMPLE_SIZE; i++) samples[i] = arr[i * sStride];
      samples.sort((x, y) => x - y);
      let maxGap = 0, maxGapIdx = -1;
      for (let i = 1; i < SAMPLE_SIZE; i++) {
        const g = samples[i] - samples[i - 1];
        if (g > maxGap) { maxGap = g; maxGapIdx = i; }
      }
      // Median gap — use copy-and-sort (small array, cheap)
      const gapsCopy = new Array(SAMPLE_SIZE - 1);
      for (let i = 0; i < SAMPLE_SIZE - 1; i++) gapsCopy[i] = samples[i + 1] - samples[i];
      gapsCopy.sort((a, b) => a - b);
      const medianGap = gapsCopy[gapsCopy.length >> 1];
      const range = maxValue - minValue;
      // Bimodal signal alone isn't enough — wide clusters (e.g. trimodal
      // with σ=10) have big inter-peak gaps but flash sort handles them
      // fine because each cluster spans many buckets. The catastrophic
      // case is NARROW clusters: σ smaller than bucket_width, so a whole
      // cluster's worth of values gets dumped into one bucket and the
      // insertion cleanup pass goes O(n²).
      //
      // Diagnostic for "would flash sort fail here": find any 6
      // consecutive sorted samples that span less than one bucket-width.
      // That means a real cluster packs many values into ~one bucket.
      const bucketWidth = range / length;
      let hasTightCluster = false;
      const TIGHT_WINDOW = 6;
      for (let i = 0; i + TIGHT_WINDOW < SAMPLE_SIZE; i++) {
        if (samples[i + TIGHT_WINDOW] - samples[i] < bucketWidth) { hasTightCluster = true; break; }
      }
      const isMultimodal =
        medianGap > 0 &&
        maxGap >= 10 * medianGap &&
        maxGap >= 0.2 * range &&
        hasTightCluster &&
        maxGapIdx >= 4 && maxGapIdx <= SAMPLE_SIZE - 4;
      if (isMultimodal) {
        const pivot = (samples[maxGapIdx - 1] + samples[maxGapIdx]) / 2;
        // In-place Hoare partition: arr[0..p-1] < pivot, arr[p..] >= pivot
        let i = 0, j = length - 1;
        while (i <= j) {
          while (i <= j && arr[i] <  pivot) i++;
          while (i <= j && arr[j] >= pivot) j--;
          if (i < j) { const t = arr[i]; arr[i] = arr[j]; arr[j] = t; i++; j--; }
        }
        // Recurse on each half via slice/copy. Memory cost: O(n) temporarily,
        // only paid on the (rare) multimodal path. Recursion handles trimodal+
        // by re-detecting gaps in each half.
        if (i > 1) {
          const left = arr.slice(0, i);
          sortFn(left);
          for (let k = 0; k < i; k++) arr[k] = left[k];
        }
        if (i < length) {
          const right = arr.slice(i);
          sortFn(right);
          for (let k = 0; k < length - i; k++) arr[i + k] = right[k];
        }
        return arr;
      }
      // No gap — check IQR safety inline (reuse the samples we already sorted)
      if ((samples[47] - samples[16]) / range >= FLASH_SAFETY_RATIO) {
        flashSort(arr, length, minValue, maxValue);
        return arr;
      }
    }
    if (length >= FLOAT_RADIX_THRESHOLD) { lsdRadixFloat64(arr, length); return arr; }
    quicksort(arr, 0, length - 1, 2 * (31 - Math.clz32(length)));
    return arr;
  };
})();

const sortStringsImpl = (() => {
  const INSERTION_THRESHOLD_STR = 16;
  const MAX_RUNS_FOR_MERGE_STR  = 32;

  function insertionSortStr(a, lo, hi) {
    for (let i = lo + 1; i <= hi; i++) {
      const k = a[i]; let j = i - 1;
      while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
      a[j + 1] = k;
    }
  }
  // Comparison helper: lexicographic compare of two strings starting at depth d.
  // Chars 0..d-1 are assumed equal (we never call with non-shared earlier chars).
  function cmpAt(a, b, d) {
    const la = a.length, lb = b.length;
    let i = d;
    while (i < la && i < lb) {
      const ca = a.charCodeAt(i), cb = b.charCodeAt(i);
      if (ca !== cb) return ca - cb;
      i++;
    }
    return la - lb;
  }
  // Insertion sort starting at depth d. Used as leaf for small buckets.
  function insertionSortAt(a, lo, hi, d) {
    for (let i = lo + 1; i <= hi; i++) {
      const s = a[i]; let j = i - 1;
      while (j >= lo && cmpAt(a[j], s, d) > 0) { a[j + 1] = a[j]; j--; }
      a[j + 1] = s;
    }
  }
  // MSD radix sort one character at depth d, then recurse on each non-empty
  // bucket. Bucket 0 = end-of-string (already sorted relative to itself).
  // Buckets 1..256 = chars 0..255 (shifted +1 to give end-of-string priority).
  function msdAt(a, lo, hi, d, buf) {
    if (hi - lo <= INSERTION_THRESHOLD_STR) { insertionSortAt(a, lo, hi, d); return; }
    const counts = new Int32Array(258);
    for (let i = lo; i <= hi; i++) {
      const s = a[i];
      counts[d < s.length ? s.charCodeAt(d) + 1 : 0]++;
    }
    const starts = new Int32Array(258);
    for (let i = 1; i < 258; i++) starts[i] = starts[i - 1] + counts[i - 1];
    const cursors = new Int32Array(258);
    for (let i = 0; i < 258; i++) cursors[i] = starts[i];
    for (let i = lo; i <= hi; i++) {
      const s = a[i];
      const c = d < s.length ? s.charCodeAt(d) + 1 : 0;
      buf[cursors[c]++] = s;
    }
    const range = hi - lo + 1;
    for (let i = 0; i < range; i++) a[lo + i] = buf[i];
    // Skip bucket 0 — strings that ended at depth d need no further sorting.
    for (let c = 1; c < 258; c++) {
      if (counts[c] > 1) {
        msdAt(a, lo + starts[c], lo + starts[c] + counts[c] - 1, d + 1, buf);
      }
    }
  }
  // Top-level wrapper: detect common prefix once, then MSD-at-depth from
  // that depth. The radix step splits into 256 buckets in one pass, much
  // faster than multikey QS's log2(256) = 8 levels of 3-way partitioning.
  function multikeyQsRoot(a, lo, hi) {
    let d = 0;
    const range = hi - lo + 1;
    if (range > 64) {
      const first = a[lo];
      let plen = first.length;
      if (plen > 0) {
        for (let i = lo + 1; i <= hi; i++) {
          const s = a[i];
          const lim = plen < s.length ? plen : s.length;
          let j = 0;
          while (j < lim && first.charCodeAt(j) === s.charCodeAt(j)) j++;
          plen = j;
          if (plen === 0) break;
        }
        d = plen;
      }
    }
    const buf = new Array(range);
    msdAt(a, lo, hi, d, buf);
  }
  function detectRunsStr(a, n, maxRuns) {
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
  function mergeStr(a, buf, a1, b1, a2, b2) {
    if (a[b1] <= a[a2]) return;  // predictive free-merge
    let p=a1,q=a2,w=a1;
    while (p<=b1 && q<=b2) {
      if (a[p]<=a[q]) buf[w++]=a[p++]; else buf[w++]=a[q++];
    }
    while (p<=b1) buf[w++]=a[p++];
    while (q<=b2) buf[w++]=a[q++];
    for (let r=a1;r<=b2;r++) a[r]=buf[r];
  }
  function mergeAllRunsStr(a, n, runs) {
    const buf = new Array(n);
    let cur = runs;
    while (cur.length > 2) {
      const next = [];
      for (let k = 0; k < cur.length; k += 4) {
        const a1 = cur[k], b1 = cur[k + 1];
        if (k + 2 >= cur.length) { next.push(a1, b1); continue; }
        mergeStr(a, buf, a1, b1, cur[k + 2], cur[k + 3]);
        next.push(a1, cur[k + 3]);
      }
      cur = next;
    }
  }
  // (Old 2-char MSD prefix radix was here; subsumed by msdAt above.)
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
    const PROBE_SIZE = n < 64 ? n - 1 : 64;
    let probeSorted = 0;
    for (let i = 0; i < PROBE_SIZE; i++) if (arr[i] <= arr[i + 1]) probeSorted++;
    if (probeSorted * 4 >= PROBE_SIZE * 3) {
      const runs = detectRunsStr(arr, n, MAX_RUNS_FOR_MERGE_STR);
      if (runs !== null && runs.length > 2) {
        mergeAllRunsStr(arr, n, runs);
        return arr;
      }
    }
    // MSD-at-depth path: compute common prefix once, then radix-sort one
    // char at a time from there. Handles every shape: shared-prefix workloads
    // (URLs, paths, log lines), random short strings, and mixed-length data.
    // Replaces both the old 2-char MSD prefix radix and the multikey QS
    // fallback with one simpler unified algorithm.
    multikeyQsRoot(arr, 0, n - 1);
    return arr;
  };
})();

const sort = (arr) => {
  if (arr.length <= 1) return arr;
  return typeof arr[0] === 'string' ? sortStringsImpl(arr) : sortNumbersImpl(arr);
};
sort.preallocate = (n) => { ensurePool(n); };
sort.releaseBuffers = () => {
  pool.u32A = null; pool.u32B = null;
  pool.f64Buf = null; pool.counts = null;
  pool.cap = 0;
};

return sort;
})()
