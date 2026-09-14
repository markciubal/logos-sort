//! logos-sort — parallel sample sort with a coarse interpolation leaf.
//!
//! Port of the C engine (`logos-sort-c`). Two-level decomposition:
//!
//! * **top level**: partition into P value-balanced buckets in parallel
//!   (sample → splitters → parallel histogram → parallel scatter);
//! * **leaf**: each bucket is sorted in parallel by a coarse *interpolation*
//!   sort — map each value linearly to a sub-bucket (one count + one scatter
//!   pass per level) and recurse. Pathological ranges (non-finite span, extreme
//!   concentration, depth exhaustion) fall back to `slice::sort_unstable`, so
//!   the worst case is the standard library's own worst case.
//!
//! Concurrency layout: the histogram phase shares a read-only borrow; the
//! per-bucket sort phase hands each thread disjoint `&mut` slices obtained via
//! `split_at_mut` (data-race freedom is compiler-checked); only the scatter
//! phase — interleaved disjoint writes — uses `unsafe`, with the disjointness
//! argument documented at the site.
//!
//! Results never depend on the thread count. NaN ordering is unspecified.

use std::thread;

const INSERTION_N: usize = 64;
const COARSE_K: usize = 1024;
const MAX_DEPTH: u32 = 40;
const OVERSAMPLE: usize = 32;
const MIN_PARALLEL: usize = 1 << 16;

/// Sort `a` ascending using up to `nthreads` threads. In place; allocates ~8n
/// bytes of scratch. `nthreads <= 1` runs fully sequential.
pub fn sort_f64(a: &mut [f64], nthreads: usize) {
    let n = a.len();
    if n < 2 {
        return;
    }
    let p = nthreads.max(1).min(n);
    if p == 1 || n < MIN_PARALLEL {
        let mut tmp = vec![0.0f64; n];
        coarse_f64(a, &mut tmp, MAX_DEPTH);
        return;
    }

    // ---- 1. splitters from a strided sample --------------------------------
    let m = (p * OVERSAMPLE).min(n);
    let step = (n / m).max(1);
    let mut samp: Vec<f64> = (0..m).map(|i| a[(i * step).min(n - 1)]).collect();
    samp.sort_unstable_by(f64::total_cmp);
    let splitters: Vec<f64> = (1..p).map(|b| samp[(b * m) / p]).collect();

    let chunk = (n + p - 1) / p;

    // ---- 2. histogram (parallel; shared read-only borrow) ------------------
    let ar: &[f64] = a;
    let counts: Vec<Vec<usize>> = thread::scope(|s| {
        let handles: Vec<_> = (0..p)
            .map(|t| {
                let sp = &splitters;
                s.spawn(move || {
                    let lo = t * chunk;
                    let hi = ((t + 1) * chunk).min(n);
                    let mut c = vec![0usize; p];
                    for &v in &ar[lo..hi] {
                        c[sp.partition_point(|&x| x <= v)] += 1;
                    }
                    c
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });

    // ---- 3. combine: bucket sizes, starts, per-thread write cursors --------
    let mut bsize = vec![0usize; p];
    for row in &counts {
        for (b, &c) in row.iter().enumerate() {
            bsize[b] += c;
        }
    }
    let mut bstart = vec![0usize; p];
    for b in 1..p {
        bstart[b] = bstart[b - 1] + bsize[b - 1];
    }
    let mut cursors: Vec<Vec<usize>> = vec![vec![0usize; p]; p];
    for b in 0..p {
        let mut acc = bstart[b];
        for t in 0..p {
            cursors[t][b] = acc;
            acc += counts[t][b];
        }
    }

    // ---- 4. scatter (parallel; interleaved disjoint writes) ----------------
    let mut out = vec![0.0f64; n];
    {
        struct SendPtr(*mut f64);
        unsafe impl Send for SendPtr {}
        unsafe impl Sync for SendPtr {}
        let optr = SendPtr(out.as_mut_ptr());
        let ar: &[f64] = a;
        thread::scope(|s| {
            for (t, cur0) in cursors.into_iter().enumerate() {
                let sp = &splitters;
                let op = &optr;
                s.spawn(move || {
                    let mut cur = cur0;
                    let lo = t * chunk;
                    let hi = ((t + 1) * chunk).min(n);
                    for &v in &ar[lo..hi] {
                        let b = sp.partition_point(|&x| x <= v);
                        // SAFETY: thread t writes bucket b only within
                        // [cursors[t][b], cursors[t][b] + counts[t][b]) — ranges that
                        // are pairwise disjoint across (t, b) by the prefix-sum
                        // construction in step 3, and the histogram in step 2
                        // guarantees exactly counts[t][b] writes land in each. No
                        // other thread reads or writes `out` during this scope.
                        unsafe { *op.0.add(cur[b]) = v };
                        cur[b] += 1;
                    }
                });
            }
        });
    }

    // ---- 5. per-bucket sort (parallel; compiler-checked disjoint slices) ---
    thread::scope(|s| {
        let mut rest_out: &mut [f64] = &mut out;
        let mut rest_scr: &mut [f64] = a; // input contents no longer needed: reuse as scratch
        for b in 0..p {
            let (bo, ro) = std::mem::take(&mut rest_out).split_at_mut(bsize[b]);
            let (bs, rs) = std::mem::take(&mut rest_scr).split_at_mut(bsize[b]);
            rest_out = ro;
            rest_scr = rs;
            if bo.len() > 1 {
                s.spawn(move || coarse_f64(bo, bs, MAX_DEPTH));
            }
        }
    });

    a.copy_from_slice(&out);
}

/// Sort `a` ascending using up to `nthreads` threads. Every `i32` is exactly
/// representable as `f64`, so this widens, sorts, and narrows losslessly.
pub fn sort_i32(a: &mut [i32], nthreads: usize) {
    if a.len() < 2 {
        return;
    }
    let mut d: Vec<f64> = a.iter().map(|&x| x as f64).collect();
    sort_f64(&mut d, nthreads);
    for (x, &v) in a.iter_mut().zip(&d) {
        *x = v as i32;
    }
}

// ---- sequential leaf: coarse interpolation sort -----------------------------

#[inline]
fn bucket_index(v: f64, mn: f64, scale: f64, k: usize) -> usize {
    // v >= mn, so the product is non-negative; `as usize` saturates on any
    // pathological float, and min() clamps the v == mx edge (product == k).
    (((v - mn) * scale) as usize).min(k - 1)
}

fn insertion_f64(a: &mut [f64]) {
    for i in 1..a.len() {
        let key = a[i];
        let mut j = i;
        while j > 0 && a[j - 1] > key {
            a[j] = a[j - 1];
            j -= 1;
        }
        a[j] = key;
    }
}

/// Sorts `a` in place; `tmp` must be at least `a.len()`; result lands in `a`.
fn coarse_f64(a: &mut [f64], tmp: &mut [f64], depth: u32) {
    let n = a.len();
    debug_assert!(tmp.len() >= n);
    if n < INSERTION_N {
        insertion_f64(a);
        return;
    }
    if depth == 0 {
        a.sort_unstable_by(f64::total_cmp); // safety net: stdlib's worst case
        return;
    }
    let (mut mn, mut mx) = (a[0], a[0]);
    for &v in &a[1..] {
        if v < mn {
            mn = v;
        } else if v > mx {
            mx = v;
        }
    }
    let range = mx - mn;
    if !(range > 0.0) || !range.is_finite() {
        // all-equal (return as-is), or a non-finite / NaN-contaminated span:
        // defer to the standard sort rather than risk degenerate bucketing.
        if mn != mx {
            a.sort_unstable_by(f64::total_cmp);
        }
        return;
    }
    let k = COARSE_K.min(n);
    let scale = k as f64 / range;

    let mut cnt = vec![0usize; k + 1];
    for &v in a.iter() {
        cnt[bucket_index(v, mn, scale, k) + 1] += 1;
    }
    // Interpolation can't split heavily concentrated data; bail to the stdlib
    // before recursing unproductively.
    let mxb = *cnt[1..].iter().max().unwrap();
    if mxb > n - (n >> 3) {
        a.sort_unstable_by(f64::total_cmp);
        return;
    }
    for b in 0..k {
        cnt[b + 1] += cnt[b];
    }
    let mut cur: Vec<usize> = cnt[..k].to_vec();
    for &v in a.iter() {
        let b = bucket_index(v, mn, scale, k);
        tmp[cur[b]] = v;
        cur[b] += 1;
    }
    for b in 0..k {
        let (s, e) = (cnt[b], cnt[b + 1]);
        if e - s > 1 {
            coarse_f64(&mut tmp[s..e], &mut a[s..e], depth - 1);
        }
    }
    a.copy_from_slice(&tmp[..n]);
}

// ---- tests ------------------------------------------------------------------


// ===================== strict in-place sort =====================
// Cycle-following (American-flag) MSD radix. Auxiliary memory is O(256*bytes)
// on the stack — no n-sized buffer — so peak footprint is ~1n vs the buffered
// paths' ~2n. Trades sequential scatter for random-access swaps (slower;
// measured ~1.5-6x in the C port), so it is the memory-ceiling tool, not a speed
// choice. Pure safe Rust: the permutation is expressed as slice swaps, so
// data-race/bounds safety is compiler-checked. Sequential.
//
// Keys are order-preserving unsigned integers: i32 flips the sign bit; f64 uses
// the IEEE total-order transform. A one-pass OR/AND skips bytes that never vary.

#[inline] fn key_i32(v: i32) -> u32 { (v as u32) ^ 0x8000_0000 }
#[inline] fn key_f64(v: f64) -> u64 { let u = v.to_bits(); if u >> 63 != 0 { !u } else { u | 0x8000_0000_0000_0000 } }

macro_rules! define_inplace_msd {
    ($name:ident, $ety:ty, $kty:ty, $key:path) => {
        fn $name(a: &mut [$ety], shifts: &[u32], si: usize) {
            let n = a.len();
            if n <= 32 || si >= shifts.len() {
                // insertion by value
                for i in 1..n {
                    let k = a[i]; let mut j = i;
                    while j > 0 && a[j - 1] > k { a[j] = a[j - 1]; j -= 1; }
                    a[j] = k;
                }
                return;
            }
            let shift = shifts[si];
            let mut cnt = [0usize; 256];
            for &x in a.iter() { cnt[(($key(x) >> shift) & 0xff) as usize] += 1; }
            let mut bstart = [0usize; 257];
            for b in 0..256 { bstart[b + 1] = bstart[b] + cnt[b]; }
            let mut next = [0usize; 256];
            for b in 0..256 { next[b] = bstart[b]; }
            for b in 0..256 {
                while next[b] < bstart[b + 1] {
                    let tb = (($key(a[next[b]]) >> shift) & 0xff) as usize;
                    if tb == b { next[b] += 1; }
                    else { a.swap(next[b], next[tb]); next[tb] += 1; }
                }
            }
            for b in 0..256 {
                let (lo, hi) = (bstart[b], bstart[b + 1]);
                if hi - lo > 1 { $name(&mut a[lo..hi], shifts, si + 1); }
            }
        }
    };
}
define_inplace_msd!(inplace_msd_i32, i32, u32, key_i32);
define_inplace_msd!(inplace_msd_f64, f64, u64, key_f64);

fn prescan_inplace<T: PartialOrd + Copy>(a: &mut [T]) -> bool {
    // returns true if fully handled (already sorted, or reversed here)
    let n = a.len();
    let (mut asc, mut desc) = (true, true);
    for i in 1..n { if a[i] < a[i - 1] { asc = false; } if a[i] > a[i - 1] { desc = false; } if !asc && !desc { break; } }
    if asc { return true; }
    if desc { a.reverse(); return true; }
    false
}

/// Sort `a` ascending in place using ~1n memory (no n-sized scratch), at a speed
/// cost vs [`sort_f64`]. For memory-constrained large inputs. `nthreads` is
/// currently ignored (sequential).
pub fn sort_f64_inplace(a: &mut [f64], _nthreads: usize) {
    if a.len() < 2 { return; }
    if prescan_inplace(a) { return; }
    let (mut orv, mut andv) = (0u64, !0u64);
    for &x in a.iter() { let k = key_f64(x); orv |= k; andv &= k; }
    let diff = orv ^ andv;
    let mut shifts = [0u32; 8]; let mut ns = 0;
    for b in (0..8).rev() { if (diff >> (b * 8)) & 0xff != 0 { shifts[ns] = (b * 8) as u32; ns += 1; } }
    if ns == 0 { return; }
    inplace_msd_f64(a, &shifts[..ns], 0);
}

/// Sort `a` ascending in place using ~1n memory. See [`sort_f64_inplace`].
pub fn sort_i32_inplace(a: &mut [i32], _nthreads: usize) {
    if a.len() < 2 { return; }
    if prescan_inplace(a) { return; }
    let (mut orv, mut andv) = (0u32, !0u32);
    for &x in a.iter() { let k = key_i32(x); orv |= k; andv &= k; }
    let diff = orv ^ andv;
    let mut shifts = [0u32; 4]; let mut ns = 0;
    for b in (0..4).rev() { if (diff >> (b * 8)) & 0xff != 0 { shifts[ns] = (b * 8) as u32; ns += 1; } }
    if ns == 0 { return; }
    inplace_msd_i32(a, &shifts[..ns], 0);
}


// ===================== parity extras (argSort, permutations, outliers, strings) =====================

#[inline] fn arg_tkey(v: f64) -> u64 { let u = v.to_bits(); if u >> 63 != 0 { !u } else { u | 0x8000_0000_0000_0000 } }
#[inline] fn arg_ikey(v: i32) -> u32 { (v as u32) ^ 0x8000_0000 }

macro_rules! define_argsort {
    ($name:ident, $ety:ty, $kty:ty, $key:path, $bytes:expr) => {
        /// Stable permutation `p` such that `a[p[0]] <= a[p[1]] <= ...`; `a` is not
        /// modified. Index-carrying LSD radix on an order-preserving key (keys carried
        /// alongside indices, constant bytes skipped). Panics if `n >= 2^32`.
        pub fn $name(a: &[$ety]) -> Vec<u32> {
            let n = a.len();
            assert!(n < (1usize << 32), "argsort supports < 2^32 elements");
            let mut perm: Vec<u32> = (0..n as u32).collect();
            if n < 2 { return perm; }
            let mut k0: Vec<$kty> = Vec::with_capacity(n);
            let (mut orv, mut andv): ($kty, $kty) = (0, !0);
            for &x in a { let k = $key(x); k0.push(k); orv |= k; andv &= k; }
            let diff = orv ^ andv;
            let mut k1: Vec<$kty> = vec![0; n];
            let mut i1: Vec<u32> = vec![0; n];
            let (mut ks, mut kd) = (&mut k0, &mut k1);
            let (mut is, mut id) = (&mut perm, &mut i1);
            let mut swaps = 0u32;
            for p in 0..$bytes {
                let sh = p * 8u32;
                if (diff >> sh) & 0xff == 0 { continue; }
                let mut c = [0usize; 257];
                for &k in ks.iter() { c[(((k >> sh) & 0xff) as usize) + 1] += 1; }
                for b in 0..256 { c[b + 1] += c[b]; }
                for i in 0..n { let b = ((ks[i] >> sh) & 0xff) as usize; let pos = c[b]; c[b] += 1; kd[pos] = ks[i]; id[pos] = is[i]; }
                std::mem::swap(&mut ks, &mut kd);
                std::mem::swap(&mut is, &mut id);
                swaps += 1;
            }
            if swaps % 2 == 1 { i1 } else { perm }   // whichever `is` ended up pointing at
        }
    };
}
define_argsort!(argsort_f64, f64, u64, arg_tkey, 8u32);
define_argsort!(argsort_i32, i32, u32, arg_ikey, 4u32);

/// `out[i] = src[perm[i]]` — reorder any slice by a permutation (e.g. sort records
/// by a numeric key: argsort the key column, apply the permutation to the records).
pub fn apply_permutation<T: Copy>(src: &[T], perm: &[u32]) -> Vec<T> {
    perm.iter().map(|&p| src[p as usize]).collect()
}
/// `inv[perm[i]] = i`.
pub fn inverse_permutation(perm: &[u32]) -> Vec<u32> {
    let mut inv = vec![0u32; perm.len()];
    for (i, &p) in perm.iter().enumerate() { inv[p as usize] = i as u32; }
    inv
}
/// Original indices whose sorted rank differs from their position by more than
/// `budget` (0 => max(3, floor(sqrt n))). Input not modified.
pub fn find_outliers_f64(a: &[f64], budget: usize) -> Vec<u32> {
    let n = a.len(); if n == 0 { return Vec::new(); }
    let budget = if budget == 0 { ((n as f64).sqrt() as usize).max(3) } else { budget };
    let inv = inverse_permutation(&argsort_f64(a));
    (0..n).filter(|&i| { let r = inv[i] as usize; (if r > i { r - i } else { i - r }) > budget })
          .map(|i| i as u32).collect()
}

/// Multikey quicksort (Bentley–Sedgewick), lexicographic by byte — the algorithmic
/// core of the JS string engine. The JS fast paths (LCP skip, packed-prefix radix,
/// galloping run-merge) are NOT ported. Sorts `a` in place; matches `str` ordering.
pub fn sort_strings(a: &mut [&str]) {
    fn chr(s: &str, d: usize) -> i32 { s.as_bytes().get(d).map(|&b| b as i32).unwrap_or(-1) }  // -1 = end (< any byte)
    fn mkqs(a: &mut [&str], d: usize) {
        let mut lo = 0isize; let mut hi = a.len() as isize - 1; let mut d = d;
        loop {
            if hi - lo <= 16 {
                for i in (lo + 1)..=hi { let mut j = i; while j > lo && a[(j - 1) as usize] > a[j as usize] { a.swap((j - 1) as usize, j as usize); j -= 1; } }
                return;
            }
            let pv = chr(a[((lo + hi) / 2) as usize], d);
            let (mut lt, mut gt, mut i) = (lo, hi, lo);
            while i <= gt {
                let c = chr(a[i as usize], d);
                if c < pv { a.swap(lt as usize, i as usize); lt += 1; i += 1; }
                else if c > pv { a.swap(gt as usize, i as usize); gt -= 1; }
                else { i += 1; }
            }
            if lt - lo > 1 { mkqs(&mut a[lo as usize..lt as usize], d); }
            if hi - gt > 1 { mkqs(&mut a[(gt + 1) as usize..=hi as usize], d); }
            if pv < 0 { return; }          // equal region ended (past end of string)
            lo = lt; hi = gt; d += 1;      // recurse the equal region one byte deeper
        }
    }
    if a.len() > 1 { mkqs(a, 0); }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }
        fn f64(&mut self) -> f64 {
            (self.next() >> 11) as f64 / (1u64 << 53) as f64
        }
    }

    fn shapes(n: usize) -> Vec<(&'static str, Vec<f64>)> {
        let mut r = Rng(0x1234567);
        vec![
            ("uniform", (0..n).map(|_| r.f64() * 2e9 - 1e9).collect()),
            ("sorted", (0..n).map(|i| i as f64).collect()),
            ("reverse", (0..n).rev().map(|i| i as f64).collect()),
            ("few_distinct", (0..n).map(|_| (r.next() % 5) as f64 * 1e6).collect()),
            ("all_equal", vec![42.0; n]),
            ("negatives", (0..n).map(|_| r.f64() * 2e6 - 1e6).collect()),
        ]
    }

    #[test]
    fn matches_sort_unstable_all_threads() {
        for n in [0usize, 1, 2, 63, 64, 65, 1000, 100_000, 300_000] {
            for (name, data) in shapes(n) {
                let mut want = data.clone();
                want.sort_unstable_by(f64::total_cmp);
                for t in [1usize, 2, 4, 8] {
                    let mut got = data.clone();
                    sort_f64(&mut got, t);
                    assert_eq!(got, want, "shape={name} n={n} threads={t}");
                }
            }
        }
    }

    #[test]
    fn argsort_stable_and_valid() {
        for n in [0usize, 1, 2, 33, 1000, 100_000] {
            let mut r = Rng(n as u64 + 1);
            let a: Vec<f64> = (0..n).map(|_| (r.next() % 50) as f64).collect();  // ties -> stability
            let p = argsort_f64(&a);
            let mut seen = vec![false; n];
            for i in 0..n { assert!(!seen[p[i] as usize]); seen[p[i] as usize] = true; }         // permutation
            for i in 1..n { assert!(a[p[i-1] as usize] <= a[p[i] as usize]);                       // sorted
                if a[p[i-1] as usize] == a[p[i] as usize] { assert!(p[i-1] < p[i]); } }            // stable
            let out = apply_permutation(&a, &p);
            for i in 1..n { assert!(out[i-1] <= out[i]); }
            let inv = inverse_permutation(&p);
            for i in 0..n { assert_eq!(inv[p[i] as usize], i as u32); }
        }
        let ai: Vec<i32> = (0..100_000).map(|i| ((i * 2654435761u64) & 0xff) as i32).collect();
        let pi = argsort_i32(&ai);
        for i in 1..ai.len() { assert!(ai[pi[i-1] as usize] <= ai[pi[i] as usize]); }
    }

    #[test]
    fn outliers_and_strings() {
        let mut a: Vec<f64> = (0..100_000).map(|i| i as f64).collect();
        a[10] = 1e18; a[99_994] = -1e18;
        let o = find_outliers_f64(&a, 0);
        assert!(o.contains(&10) && o.contains(&99_994));
        let mut s = vec!["pear","apple","banana","apple","aardvark","zzz","app","apple pie",""];
        let mut want = s.clone(); want.sort();
        sort_strings(&mut s);
        assert_eq!(s, want);
        let mut r = Rng(3);
        let pool: Vec<String> = (0..20_000).map(|_| { let l = 1 + (r.next() % 8) as usize; (0..l).map(|_| (b'a' + (r.next() % 6) as u8) as char).collect() }).collect();
        let mut refs: Vec<&str> = pool.iter().map(|s| s.as_str()).collect();
        let mut w2 = refs.clone(); w2.sort();
        sort_strings(&mut refs);
        assert_eq!(refs, w2);
    }

    #[test]
    fn inplace_matches_sort_unstable() {
        for n in [0usize, 1, 2, 33, 257, 1000, 100_000] {
            for (name, data) in shapes(n) {
                let mut want = data.clone(); want.sort_unstable_by(f64::total_cmp);
                let mut got = data.clone(); sort_f64_inplace(&mut got, 1);
                assert_eq!(got, want, "inplace f64 shape={name} n={n}");
            }
            let mut r = Rng(9);
            let di: Vec<i32> = (0..n).map(|_| r.next() as i32).collect();
            let mut wi = di.clone(); wi.sort_unstable();
            let mut gi = di.clone(); sort_i32_inplace(&mut gi, 1);
            assert_eq!(gi, wi, "inplace i32 n={n}");
        }
    }

    #[test]
    fn i32_matches() {
        let mut r = Rng(7);
        for n in [0usize, 1, 1000, 200_000] {
            let data: Vec<i32> = (0..n).map(|_| r.next() as i32).collect();
            let mut want = data.clone();
            want.sort_unstable();
            for t in [1usize, 4] {
                let mut got = data.clone();
                sort_i32(&mut got, t);
                assert_eq!(got, want, "i32 n={n} threads={t}");
            }
        }
    }
}
