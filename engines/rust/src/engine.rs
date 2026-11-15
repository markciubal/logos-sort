//! The v3 dispatcher: read the data's structure from a prescan and one sorted sample, and take the
//! cheapest route that fits. The Rust twin of `psort_f64` in the C engine (`logos_psort.c`).
//!
//! Sequential routes, in the order they are asked:
//!   prescan            sorted -> return; non-increasing -> one reversal
//!   few values         at most 8 distinct sample values: counted with vector compares, emitted
//!   lattice            every sample point on one lattice with room to spare: direct-index counts
//!   hashed counts      up to 256 distinct sample values off any lattice
//!   int32 values       doubles that are all 32-bit integers: an LSD radix on 4-byte keys
//!   runs               long ascending runs: strays lifted, pieces that lie alone copied, only
//!                      what crosses merged (the knot route), strays merged back in
//!   kernel             the AVX2 quicksort
//! Every route either finishes or leaves the array a permutation of its input for the next one.

use crate::kernel;
use crate::par;
use crate::pool::{self, Buf};
use core::arch::x86_64::*;

/// Which route finished this thread's most recent sort of at least SMALL_N elements (a witness for
/// tests and benches: a test that cannot tell which path ran tests nothing in particular). Kept
/// per calling thread, and never written by a smaller sort, so a call of a few elements does not
/// pay for a thread-local access.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Route {
    None,
    Sorted,
    Reversed,
    FewValues,
    Lattice,
    HashedCounts,
    IntRadix,
    OneStrand,
    Runs,
    Kernel,
    Partition,
    /// short-reach disorder: blocks sorted where they lie, placed by the run plan
    Reach,
    /// two threads: one split at the sample's median, a kernel on each side
    Halves,
    /// a NaN was met: NaNs moved to the tail, the rest sorted
    NanLast,
}
thread_local! {
    static ROUTE: core::cell::Cell<u8> = const { core::cell::Cell::new(0) };
}
#[inline]
pub(crate) fn took(r: Route) {
    ROUTE.with(|c| c.set(r as u8));
}
pub(crate) fn last_route() -> Route {
    const ALL: [Route; 14] = [Route::None, Route::Sorted, Route::Reversed, Route::FewValues, Route::Lattice, Route::HashedCounts, Route::IntRadix,
                              Route::OneStrand, Route::Runs, Route::Kernel, Route::Partition, Route::Reach, Route::Halves, Route::NanLast];
    ALL[ROUTE.with(|c| c.get()) as usize]
}

/// Phase timing to stderr, in builds with `--features trace`. A trace build says WHICH path ran and
/// what share of its time each phase took; it is not for ranking two routes.
#[cfg(feature = "trace")]
macro_rules! trace {
    ($t:ident, $($arg:tt)*) => {{
        eprintln!("  [logos-rs] {:8.3} ms  {}", $t.elapsed().as_secs_f64() * 1e3, format!($($arg)*));
        $t = std::time::Instant::now();
    }};
}
#[cfg(not(feature = "trace"))]
macro_rules! trace {
    ($t:ident, $($arg:tt)*) => {{
        let _ = &$t;
    }};
}
#[cfg(feature = "trace")]
macro_rules! trace_start {
    () => {
        std::time::Instant::now()
    };
}
#[cfg(not(feature = "trace"))]
macro_rules! trace_start {
    () => {
        ()
    };
}
pub(crate) use {trace, trace_start};

pub(crate) const SMALL_N: usize = 2048;
const SAMPLE_M: usize = 2048;

/// Order-preserving key of a double (IEEE total order) and its inverse.
#[inline]
pub(crate) fn tkey(v: f64) -> u64 {
    let u = v.to_bits();
    if u >> 63 != 0 { !u } else { u | 1u64 << 63 }
}
#[inline]
pub(crate) fn tkey_inv(k: u64) -> f64 {
    f64::from_bits(if k >> 63 != 0 { k & !(1u64 << 63) } else { !k })
}
#[inline]
pub(crate) fn weyl_pos(i: usize, n: usize) -> usize {
    let f = i as f64 * 0.618_033_988_749_894_9;
    let q = ((f - (f as u64) as f64) * n as f64) as usize;
    q.min(n - 1)
}

/// The kernel, or the total-order sort when the range holds a NaN.
#[inline]
pub(crate) unsafe fn kernel_sort(a: &mut [f64]) {
    if !kernel::sort(a) {
        let k = nans_last(a);
        kernel::sort(&mut a[..k]);
    }
}
/// NaNs to the tail, bit patterns intact; returns how many elements stand before them. With a NaN
/// the order is unspecified; this is simply the cheapest thing that returns a permutation (a
/// total-order comparison sort was 2.3x the kernel's time on an array with ONE NaN).
pub(crate) fn nans_last(a: &mut [f64]) -> usize {
    let (mut i, mut k) = (0, a.len());
    while i < k {
        if a[i] != a[i] {
            k -= 1;
            a.swap(i, k);
        } else {
            i += 1;
        }
    }
    k
}

/// Is a[lo..hi) ascending, once every KINK in it is put right? A kink is one adjacent pair that
/// stands the wrong way round in a run that otherwise goes on (the element before the pair is not
/// above it, the element after not below): it is swapped back. Kinks that overlap (two swaps
/// sharing an element leave one element two places out, which no single swap undoes; about one
/// random swap in fifty does) are put right by sorting the 6, 12 or 24 elements around the descent,
/// if what stands on either side of them then agrees. Returns false at the first descent that
/// neither repairs; what was repaired before it stays repaired. Only elements inside [lo, hi) are
/// read or written, so chunks may be scanned at once (with `edge` > 0: see below); `n` is the
/// array's length (its two ends have no neighbour to ask).
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn scan_repair(p: *mut f64, lo: usize, hi: usize, n: usize, edge: usize) -> bool {
    let mut i = lo + 1;
    while i < hi {
        let d = i - 1 + first_descent(core::slice::from_raw_parts(p.add(i - 1), hi - (i - 1)));
        if d >= hi { return true; }
        // A descent within `edge` of a cut it shares with another chunk is LEFT, not judged: a repair
        // there would read the neighbour's elements. The caller scans a zone around every cut afterwards.
        if (lo != 0 && d < lo + edge) || (hi != n && d + edge > hi) {
            i = d + 1;
            continue;
        }
        let left = if d >= lo + 2 { *p.add(d) >= *p.add(d - 2) } else { lo == 0 && d == 1 };
        let right = if d + 1 < hi { *p.add(d - 1) <= *p.add(d + 1) } else { hi == n };
        if left && right {
            core::ptr::swap(p.add(d - 1), p.add(d));
            i = d + 1;
            continue;
        }
        // not one kink: the 6, then 12, then 24 elements around the descent, sorted, between
        // neighbours that agree (a chain of k overlapping swaps leaves an element k places out)
        let mut mended = false;
        for half in [3usize, 6, 12] {
            let (wl, wh) = (d.saturating_sub(half).max(lo), (d + half).min(hi));
            if (wl == lo && lo != 0) || (wh == hi && hi != n) { break; }
            let mut w = [0f64; 24];
            let k = wh - wl;
            core::ptr::copy_nonoverlapping(p.add(wl), w.as_mut_ptr(), k);
            for x in 1..k {
                let (v, mut y) = (w[x], x);
                while y > 0 && w[y - 1] > v {
                    w[y] = w[y - 1];
                    y -= 1;
                }
                w[y] = v;
            }
            // (a NaN in the window fails one of these tests or leaves it a permutation: either is fine)
            if (wl == 0 || *p.add(wl - 1) <= w[0]) && (wh == n || w[k - 1] <= *p.add(wh)) && (1..k).all(|x| w[x - 1] <= w[x]) {
                core::ptr::copy_nonoverlapping(w.as_ptr(), p.add(wl), k);
                i = wh;
                mended = true;
                break;
            }
        }
        if !mended { return false; }
    }
    true
}
/// First t in [from, to) with src[t] - src[t-1] > half of (src[t-1] - first), or `to`; from >= 1.
#[target_feature(enable = "avx2")]
unsafe fn first_gap(src: *const f64, from: usize, to: usize, first: f64) -> usize {
    let (vf, half) = (_mm256_set1_pd(first), _mm256_set1_pd(0.5));
    let mut t = from;
    while t + 4 <= to {
        let (a, b) = (_mm256_loadu_pd(src.add(t - 1)), _mm256_loadu_pd(src.add(t)));
        let k = _mm256_movemask_pd(_mm256_cmp_pd::<_CMP_GT_OQ>(_mm256_sub_pd(b, a), _mm256_mul_pd(half, _mm256_sub_pd(a, vf))));
        if k != 0 { return t + k.trailing_zeros() as usize; }
        t += 4;
    }
    while t < to {
        let prev = *src.add(t - 1);
        if *src.add(t) - prev > 0.5 * (prev - first) { return t; }
        t += 1;
    }
    to
}

/// First i >= 1 with a[i] < a[i-1], or n.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn first_descent(a: &[f64]) -> usize {
    let (n, p) = (a.len(), a.as_ptr());
    let mut i = 1;
    while i < n && (p.add(i) as usize) & 31 != 0 {
        if *p.add(i) < *p.add(i - 1) { return i; }
        i += 1;
    }
    while i + 32 <= n {
        let mut acc = _mm256_setzero_pd();
        for q in 0..8 {
            acc = _mm256_or_pd(acc, _mm256_cmp_pd::<_CMP_LT_OQ>(_mm256_loadu_pd(p.add(i + 4 * q)), _mm256_loadu_pd(p.add(i + 4 * q - 1))));
        }
        if _mm256_movemask_pd(acc) != 0 { break; }
        i += 32;
    }
    while i < n {
        if *p.add(i) < *p.add(i - 1) { return i; }
        i += 1;
    }
    n
}
/// First i >= 1 with a[i] > a[i-1], or n.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn first_ascent(a: &[f64]) -> usize {
    let (n, p) = (a.len(), a.as_ptr());
    let mut i = 1;
    while i + 32 <= n {
        let mut acc = _mm256_setzero_pd();
        for q in 0..8 {
            acc = _mm256_or_pd(acc, _mm256_cmp_pd::<_CMP_GT_OQ>(_mm256_loadu_pd(p.add(i + 4 * q)), _mm256_loadu_pd(p.add(i + 4 * q - 1))));
        }
        if _mm256_movemask_pd(acc) != 0 { break; }
        i += 32;
    }
    while i < n {
        if *p.add(i) > *p.add(i - 1) { return i; }
        i += 1;
    }
    n
}

/// The shared sample: `m` values at Weyl positions, as sorted distinct total-order keys.
pub(crate) struct Sample {
    pub m: usize,
    /// every sample value, ascending (no NaN)
    pub sorted: Vec<f64>,
    /// the distinct ones, in total order
    pub distinct: Vec<f64>,
}
/// None when the sample holds a NaN.
pub(crate) unsafe fn sample(a: &[f64], m: usize) -> Option<Sample> {
    let n = a.len();
    let mut sorted: Vec<f64> = (0..m).map(|i| *a.get_unchecked(weyl_pos(i, n))).collect();
    if !kernel::sort(&mut sorted) { return None; }
    let mut keys: Vec<u64> = sorted.iter().map(|&v| tkey(v)).collect();
    keys.sort_unstable();
    keys.dedup();
    Some(Sample { m, sorted, distinct: keys.into_iter().map(tkey_inv).collect() })
}

/// Entry: `a` has at least two elements and the CPU has AVX2, FMA and POPCNT.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn sort(a: &mut [f64], threads: usize) {
    let n = a.len();
    if n <= 64 {
        kernel_sort(a);
        return;
    }
    let p = par::effective_threads(n, threads);
    if p > 1 {
        if par::prescan(a, p) { return; }
        par::sort(a, p, threads);
        return;
    }
    if scan_repair(a.as_mut_ptr(), 0, n, n, 0) {
        if n >= SMALL_N { took(Route::Sorted); }
        return;
    }
    if first_ascent(a) == n {
        a.reverse();
        if n >= SMALL_N { took(Route::Reversed); }
        return;
    }
    if n < SMALL_N {
        kernel_sort(a);
        return;
    }
    seq_routes(a);
}

/// The sequential dispatcher behind the prescan, for n >= SMALL_N.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn seq_routes(a: &mut [f64]) {
    let n = a.len();
    let s = match sample(a, if n >= 65536 { SAMPLE_M } else { 512 }) {
        Some(s) => s,
        None => {
            let k = nans_last(a);
            sort(&mut a[..k], 1);
            return took(Route::NanLast);
        }
    };
    let d = s.distinct.len();
    if d <= 8 && count_few(a, &s.distinct, 1) { return took(Route::FewValues); }
    if let Some(l) = lattice_gate(&s.distinct, &s.sorted, n, 1) {
        if lattice_sort(a, &l, 1) { return took(Route::Lattice); }
    }
    if d <= 256 && d * 4 <= s.m && count_hashed(a) { return took(Route::HashedCounts); }
    if int_radix(a, &s.distinct) { return took(Route::IntRadix); }
    if par::runs_sort(a, 1) { return; }
    if par::reach_sort(a, 1) { return took(Route::Reach); }
    if kernel::sort(a) {
        took(Route::Kernel);
    } else {
        let k = nans_last(a);
        sort(&mut a[..k], 1);
        took(Route::NanLast);
    }
}

// ---------------------------------------------------------------------------------------------
// writing a run of one value

/// `cnt` copies of `v` at `p`. With `nt` the body goes out in non-temporal stores (the caller
/// issues the fence): an emit only writes, so reading each line first is pure cost.
#[inline]
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn fill(mut p: *mut f64, mut cnt: usize, v: f64, nt: bool) {
    if cnt >= 16 {
        while (p as usize) & 31 != 0 {
            *p = v;
            p = p.add(1);
            cnt -= 1;
        }
        let vv = _mm256_set1_pd(v);
        if nt {
            while cnt >= 4 {
                _mm256_stream_pd(p, vv);
                p = p.add(4);
                cnt -= 4;
            }
        } else {
            while cnt >= 4 {
                _mm256_store_pd(p, vv);
                p = p.add(4);
                cnt -= 4;
            }
        }
    }
    while cnt > 0 {
        *p = v;
        p = p.add(1);
        cnt -= 1;
    }
}
pub(crate) const NT_MIN_N: usize = 1 << 21;

// ---------------------------------------------------------------------------------------------
// a handful of values, counted in vector registers

/// Counts of each of `vals` (at most 8, compared by bit pattern) over a[lo..hi); returns how many
/// elements matched one of them.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn count_few_range(a: &[f64], vals: &[f64], cnt: &mut [usize; 8]) -> usize {
    let (n, p, d) = (a.len(), a.as_ptr(), vals.len());
    let mut vv = [_mm256_setzero_si256(); 8];
    for j in 0..d { vv[j] = _mm256_set1_epi64x(vals[j].to_bits() as i64); }
    let mut acc = [_mm256_setzero_si256(); 8];
    let mut i = 0;
    while i + 4 <= n {
        let v = _mm256_castpd_si256(_mm256_loadu_pd(p.add(i)));
        for j in 0..d { acc[j] = _mm256_sub_epi64(acc[j], _mm256_cmpeq_epi64(v, vv[j])); }
        i += 4;
    }
    let mut total = 0;
    for j in 0..d {
        let mut t = [0u64; 4];
        _mm256_storeu_si256(t.as_mut_ptr() as *mut __m256i, acc[j]);
        cnt[j] = (t[0] + t[1] + t[2] + t[3]) as usize;
    }
    while i < n {
        let b = (*p.add(i)).to_bits();
        for j in 0..d { if vals[j].to_bits() == b { cnt[j] += 1; } }
        i += 1;
    }
    for j in 0..d { total += cnt[j]; }
    total
}

/// At most 8 distinct values in the sample: count them; if every element is one of them, write
/// the answer out. False (array untouched) when some element is none of them.
#[target_feature(enable = "avx2")]
unsafe fn count_few(a: &mut [f64], vals: &[f64], threads: usize) -> bool {
    let n = a.len();
    if threads > 1 { return par::count_few(a, vals, threads); }
    let mut cnt = [0usize; 8];
    if count_few_range(a, vals, &mut cnt) != n { return false; }
    let nt = n >= NT_MIN_N;
    let mut p = a.as_mut_ptr();
    for j in 0..vals.len() {
        fill(p, cnt[j], vals[j], nt);
        p = p.add(cnt[j]);
    }
    if nt { _mm_sfence(); }
    true
}

// ---------------------------------------------------------------------------------------------
// hashed counts for a few hundred values off any lattice

const HC_SLOTS: usize = 4096;
const HC_CAP: usize = 1024;

/// Counts by bit pattern in an open-addressed table; gives up (array untouched) past HC_CAP
/// distinct values. Sequential.
#[target_feature(enable = "avx2")]
unsafe fn count_hashed(a: &mut [f64]) -> bool {
    let n = a.len();
    let mut keys = vec![0u64; HC_SLOTS];
    let mut cnt = vec![0u32; HC_SLOTS];
    let mut distinct = 0;
    for &x in a.iter() {
        let b = x.to_bits();
        let mut h = (b.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 52) as usize;
        loop {
            if *cnt.get_unchecked(h) == 0 {
                if distinct == HC_CAP { return false; }
                distinct += 1;
                *keys.get_unchecked_mut(h) = b;
                *cnt.get_unchecked_mut(h) = 1;
                break;
            }
            if *keys.get_unchecked(h) == b {
                *cnt.get_unchecked_mut(h) += 1;
                break;
            }
            h = (h + 1) & (HC_SLOTS - 1);
        }
    }
    let mut pairs: Vec<(u64, u32)> = (0..HC_SLOTS).filter(|&h| cnt[h] != 0).map(|h| (tkey(f64::from_bits(keys[h])), cnt[h])).collect();
    pairs.sort_unstable();
    let nt = n >= NT_MIN_N;
    let mut p = a.as_mut_ptr();
    for (k, c) in pairs {
        fill(p, c as usize, tkey_inv(k), nt);
        p = p.add(c as usize);
    }
    if nt { _mm_sfence(); }
    true
}

// ---------------------------------------------------------------------------------------------
// the lattice route: values of the form base + i * step, counted by direct index

#[derive(Clone, Copy)]
pub(crate) struct Lattice {
    pub base: f64,
    pub step: f64,
    pub slots: usize,
}

/// Does the sample lie on one lattice (each value reproduced BIT FOR BIT by `base + i * step` with a
/// fused multiply-add), with at least `16 (P + 1) / 2` elements per slot? The window is the
/// sample's whole range, or, when that is too many slots, the range with 0.75% of the sample
/// trimmed from each end: a sample bounds the bulk, never the tail, and the tail is spilled.
#[target_feature(enable = "fma")]
pub(crate) unsafe fn lattice_gate(u: &[f64], sorted: &[f64], n: usize, threads: usize) -> Option<Lattice> {
    let (d, m) = (u.len(), sorted.len());
    if d < 2 { return None; }
    let mut g = f64::INFINITY;
    for i in 1..d {
        let x = u[i] - u[i - 1];
        if x < g { g = x; }
    }
    if !(g > 0.0) || !g.is_finite() { return None; }
    let fill_min = 16 * (threads + 1) / 2;
    let max_slots = (n / fill_min).min(1 << 26);
    let trim = (m * 3 / 400).max(1);
    let windows = [(u[0], u[d - 1]), (sorted[trim], sorted[m - 1 - trim])];
    for k in 1..=8usize {
        let step = g / k as f64;
        if !(step > 0.0) { return None; }
        let mut fits = false;
        for &(wlo, whi) in &windows {
            let top = ((whi - wlo) / step).round();
            if !(top >= 0.0) || top + 1.0 > max_slots as f64 { continue; }
            fits = true;
            let top = top as usize;
            // a margin below and above the window, shrunk until the lattice fits
            let mut mg = (top / 8).max(8);
            loop {
                let base = wlo - mg as f64 * step;
                let slots = top + 1 + 2 * mg;
                let mut ok = slots <= max_slots && base.is_finite();
                if ok {
                    for &x in u {
                        if x < wlo || x > whi { continue; }
                        let r = ((x - base) / step).round();
                        if !(r >= 0.0 && r < slots as f64 && r.mul_add(step, base).to_bits() == x.to_bits()) {
                            ok = false;
                            break;
                        }
                    }
                }
                if ok { return Some(Lattice { base, step, slots }); }
                if mg == 0 { break; }
                mg /= 2;
            }
        }
        if !fits { return None; }                /* a finer step only needs more slots */
    }
    None
}

/// Counts a[..] into `cnt` (one u32 per slot); elements off the lattice or outside it go to `spill`.
/// False once the spills pass `spill_cap`.
#[target_feature(enable = "avx2,fma")]
pub(crate) unsafe fn lattice_map(a: &[f64], l: &Lattice, cnt: *mut u32, spill: &mut Vec<f64>, spill_cap: usize) -> bool {
    let (n, p) = (a.len(), a.as_ptr());
    let (vb, vs, vi) = (_mm256_set1_pd(l.base), _mm256_set1_pd(l.step), _mm256_set1_pd(1.0 / l.step));
    let (vmax, vneg) = (_mm_set1_epi32(l.slots as i32 - 1), _mm_set1_epi32(-1));
    let inv = 1.0 / l.step;
    let mut i = 0;
    macro_rules! scalar {
        ($x:expr) => {{
            let x = $x;
            let t = (x - l.base) * inv;
            let mut hit = false;
            if t >= 0.0 && t < l.slots as f64 {
                let r = (t + 0.5) as usize;
                if r < l.slots && (r as f64).mul_add(l.step, l.base).to_bits() == x.to_bits() {
                    *cnt.add(r) += 1;
                    hit = true;
                }
            }
            if !hit {
                if spill.len() >= spill_cap { return false; }
                spill.push(x);
            }
        }};
    }
    while i + 4 <= n {
        let v = _mm256_loadu_pd(p.add(i));
        let i4 = _mm256_cvtpd_epi32(_mm256_mul_pd(_mm256_sub_pd(v, vb), vi));
        let r = _mm256_fmadd_pd(_mm256_cvtepi32_pd(i4), vs, vb);
        let eq = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(_mm256_castpd_si256(r), _mm256_castpd_si256(v))));
        let inr = _mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(_mm_cmpgt_epi32(i4, vmax), _mm_cmpgt_epi32(i4, vneg))));
        if eq == 15 && inr == 15 {
            let mut ix = [0u32; 4];
            _mm_storeu_si128(ix.as_mut_ptr() as *mut __m128i, i4);
            *cnt.add(ix[0] as usize) += 1;
            *cnt.add(ix[1] as usize) += 1;
            *cnt.add(ix[2] as usize) += 1;
            *cnt.add(ix[3] as usize) += 1;
        } else {
            for q in 0..4 { scalar!(*p.add(i + q)); }
        }
        i += 4;
    }
    while i < n {
        scalar!(*p.add(i));
        i += 1;
    }
    true
}

/// Writes slots [s0, s1) of the counted lattice at `out`, the sorted `spill` values merged in
/// (those below slot s1's value; the caller hands each range its own slice of the spills).
#[target_feature(enable = "avx2,fma")]
pub(crate) unsafe fn lattice_emit(mut out: *mut f64, l: &Lattice, cnt: *const u32, s0: usize, s1: usize, spill: &[f64], nt: bool) {
    let mut sp = 0;
    for s in s0..s1 {
        let c = *cnt.add(s) as usize;
        if c == 0 { continue; }
        let v = (s as f64).mul_add(l.step, l.base);
        while sp < spill.len() && spill[sp] < v {
            *out = spill[sp];
            out = out.add(1);
            sp += 1;
        }
        fill(out, c, v, nt);
        out = out.add(c);
    }
    while sp < spill.len() {
        *out = spill[sp];
        out = out.add(1);
        sp += 1;
    }
}

#[target_feature(enable = "avx2,fma,popcnt")]
unsafe fn lattice_sort(a: &mut [f64], l: &Lattice, threads: usize) -> bool {
    let n = a.len();
    if threads > 1 { return par::lattice_sort(a, l, threads); }
    let mut cnt = vec![0u32; l.slots];
    let mut spill = Vec::new();
    if !lattice_map(a, l, cnt.as_mut_ptr(), &mut spill, n / 32) { return false; }
    if !spill.is_empty() { sort(&mut spill, 1); }
    let nt = n >= NT_MIN_N;
    lattice_emit(a.as_mut_ptr(), l, cnt.as_ptr(), 0, l.slots, &spill, nt);
    if nt { _mm_sfence(); }
    true
}

// ---------------------------------------------------------------------------------------------
// doubles that are all 32-bit integers: an LSD radix on 4-byte keys, digits of 11 + 11 + 10 bits

/// Is every one of these (sample) values a 32-bit integer?
pub(crate) fn all_int32(u: &[f64]) -> bool {
    u.iter().all(|&x| x >= -2147483648.0 && x <= 2147483647.0 && x.trunc() == x)
}
#[target_feature(enable = "avx2")]
unsafe fn int_radix(a: &mut [f64], u: &[f64]) -> bool {
    let n = a.len();
    if n < 4096 || !all_int32(u) { return false; }
    let buf: Buf = pool::acquire(n * 8);
    let done = int_radix_in(a, buf.ptr as *mut u32);
    pool::release(buf);
    done
}
/// The radix itself, with `k1` a scratch of 2 n 32-bit keys. False (array untouched) on the first
/// element that is not a 32-bit integer.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn int_radix_in(a: &mut [f64], k1: *mut u32) -> bool {
    let n = a.len();
    if n < 256 { return false; }
    let mut h = vec![0u32; 2048 + 2048 + 1024];
    let (h0, h12) = h.split_at_mut(2048);
    let (h1, h2) = h12.split_at_mut(2048);
    #[inline(always)]
    unsafe fn key(x: f64) -> (u32, bool) {
        let i = _mm_cvttsd_si32(_mm_set_sd(x));
        ((i as u32) ^ 0x8000_0000, (i as f64).to_bits() == x.to_bits())
    }
    for &x in a.iter() {
        let (k, ok) = key(x);
        if !ok { return false; }
        *h0.get_unchecked_mut((k & 2047) as usize) += 1;
        *h1.get_unchecked_mut(((k >> 11) & 2047) as usize) += 1;
        *h2.get_unchecked_mut((k >> 22) as usize) += 1;
    }
    let prefix = |h: &mut [u32]| -> bool {
        let (mut s, mut one) = (0u32, false);
        for c in h.iter_mut() {
            one |= *c as usize == n;
            let t = *c;
            *c = s;
            s += t;
        }
        one
    };
    let (t0, t1, t2) = (prefix(h0), prefix(h1), prefix(h2));
    if t0 && t1 && t2 { return true; }           /* one value */
    let k2 = k1.add(n);
    let p = a.as_mut_ptr();
    // a -> k1 by the low digit (or straight across when that digit never varies)
    if t0 {
        for i in 0..n { *k1.add(i) = key(*p.add(i)).0; }
    } else {
        for i in 0..n {
            let k = key(*p.add(i)).0;
            let c = h0.get_unchecked_mut((k & 2047) as usize);
            *k1.add(*c as usize) = k;
            *c += 1;
        }
    }
    let mut cur = k1;
    if !t1 {
        for i in 0..n {
            let k = *k1.add(i);
            let c = h1.get_unchecked_mut(((k >> 11) & 2047) as usize);
            *k2.add(*c as usize) = k;
            *c += 1;
        }
        cur = k2;
    }
    #[inline(always)]
    fn val(k: u32) -> f64 { ((k ^ 0x8000_0000) as i32) as f64 }
    if t2 {
        for i in 0..n { *p.add(i) = val(*cur.add(i)); }
    } else {
        for i in 0..n {
            let k = *cur.add(i);
            let c = h2.get_unchecked_mut((k >> 22) as usize);
            *p.add(*c as usize) = val(k);
            *c += 1;
        }
    }
    true
}

// ---------------------------------------------------------------------------------------------
// the run routes: strays lifted, lone pieces copied, crossings merged

#[derive(Clone, Copy)]
pub(crate) struct Run {
    pub start: usize,
    pub len: usize,
}

const STRAY_LOOK: usize = 4;
const DETECT_BLOCK: usize = 4096;
/// A run is split at a step larger than half of everything it has spanned so far, once it holds
/// this many elements.
const GAP_MIN_RUN: usize = 256;
/// A stretch that descends for at least this many elements is taken as a run, reversed as it is copied.
const DESC_MIN: usize = 32;
/// A run of at most this many elements is not a run: its elements are strays. (Random data has
/// ascending runs of three and four all the time; a sorted prefix followed by an unsorted tail would
/// otherwise end in tens of thousands of them. At 8, a 5% random tail of 105,000 elements still
/// left 819 runs of nine or more, each crossing everything: the plan declined.)
const SHORT_RUN: usize = 16;

/// What the detector found: dst[0..kept) is `runs` (ascending, in array order, covering it exactly)
/// and `strays` holds the elements lifted out.
pub(crate) struct Detected {
    #[allow(dead_code)]
    pub kept: usize,
    /// How much of the source was read. Short of the whole when the detector gave up part-way:
    /// what it kept before that stands, and the caller decides what the unread rest is.
    pub scanned: usize,
    pub runs: Vec<Run>,
    pub strays: Vec<f64>,
}

/// One pass over src[0..n), the kept elements written to `dst` (n slots), the source left alone.
/// A descent ends a run unless it is a stray: up to STRAY_LOOK elements at the tail of the run that
/// stand above what follows, or up to STRAY_LOOK that fall below the run before it resumes (or
/// both at once). Runs of one or two elements are strays too.
/// Stops early (`scanned` short of n) when strays pass a quarter of what was scanned (noise: within
/// a hundred elements), or `stray_cap`, or when runs average under 256 elements. None: no run at all.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn detect_runs_to(src: *const f64, n: usize, dst: *mut f64, nt: bool, stray_cap: usize) -> Option<Detected> {
    detect_core(src, n, dst, nt, stray_cap)
}
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn detect_core(src: *const f64, n: usize, dst: *mut f64, nt: bool, stray_cap: usize) -> Option<Detected> {
    let mut runs: Vec<Run> = Vec::new();
    let mut strays: Vec<f64> = Vec::new();
    let (mut w, mut rs, mut i) = (0usize, 0usize, 0usize);
    let mut scanned = n;
    /* stop here: what was kept so far stands, src[i..n) is left unread */
    macro_rules! give_up {
        () => {{
            scanned = i;
            break;
        }};
    }
    /* Too many strays or runs? Over everything scanned; and over the last 4,096 elements, so that an
       unsorted stretch after a long clean one stops the scan where it begins. (Without the window
       the tail after a sorted prefix was read to its end, and the look-ahead strung its noise into
       runs of twenty that each crossed everything: 17 ns per element at one thread.) */
    let (mut win_i, mut win_s) = (0usize, 0usize);
    macro_rules! over {
        () => {{
            let mut o = strays.len() > 64 + (i >> 2) || strays.len() > stray_cap || runs.len() > 16 + (i >> 8);
            if i - win_i >= 4096 {
                o |= strays.len() - win_s > (i - win_i) >> 2;
                win_i = i;
                win_s = strays.len();
            }
            o
        }};
    }
    /* close the run [rs, w): a run of one or two elements is lifted; then, if `x` continues the
       run before it, that run is reopened */
    macro_rules! close_run {
        ($x:expr) => {{
            if w - rs <= SHORT_RUN {
                while w > rs {
                    w -= 1;
                    strays.push(*dst.add(w));
                }
                if let Some(&last) = runs.last() {
                    if $x >= *dst.add(w - 1) {
                        rs = last.start;
                        runs.pop();
                    }
                }
            } else {
                runs.push(Run { start: rs, len: w - rs });
                rs = w;
            }
        }};
    }
    while i < n {
        let x = *src.add(i);
        if i + 1 < n && *src.add(i + 1) < x && (w == rs || !(x >= *dst.add(w - 1))) {
            // THE TWIST: a stretch that goes on descending is a run lying the other way. It is written
            // reversed as it is copied (no pass of its own), a block at a time; each block is a run of
            // its own, which the plan lays end to end.
            let d = first_ascent(core::slice::from_raw_parts(src.add(i), (n - i).min(DETECT_BLOCK)));
            if d >= DESC_MIN {
                if w - rs > SHORT_RUN {
                    runs.push(Run { start: rs, len: w - rs });
                } else {
                    while w > rs {
                        w -= 1;
                        strays.push(*dst.add(w));
                    }
                }
                for q in 0..d { *dst.add(w + q) = *src.add(i + d - 1 - q); }
                runs.push(Run { start: w, len: d });
                w += d;
                rs = w;
                i += d;
                if over!() { give_up!(); }
                continue;
            }
        }
        if w > rs && !(x >= *dst.add(w - 1)) {
            // a descent. Is it a stray? Try, for j = 0..STRAY_LOOK elements taken off the tail of the run
            // (they stand above what follows), whether x itself fits, or whether the run resumes within
            // STRAY_LOOK elements after x (x and its neighbours fell below it).
            // A run of at most STRAY_LOOK elements may itself be the strays (a gap split puts a high
            // stray in a run of its own): then the tail that can be taken off reaches into the run before.
            let mut done = false;
            let mut j = 0;
            let floor = if w - rs <= STRAY_LOOK { runs.last().map_or(rs, |r| r.start) } else { rs };
            while j <= STRAY_LOOK && w - floor > j {
                let under = *dst.add(w - 1 - j);
                let mut q = i;
                while q < n && q <= i + STRAY_LOOK && !(*src.add(q) >= under) { q += 1; }
                if q < n && q <= i + STRAY_LOOK && (j > 0 || q > i) {
                    for t in 0..j { strays.push(*dst.add(w - 1 - t)); }
                    if j >= w - rs && floor != rs {
                        rs = floor;                  /* the short run is gone: the run before it goes on */
                        runs.pop();
                    }
                    w -= j;
                    for t in i..q { strays.push(*src.add(t)); }
                    i = q;
                    done = true;
                    break;
                }
                j += 1;
            }
            if done {
                if over!() { give_up!(); }
                continue;                        /* a[i] now continues the run */
            }
            // a new run starts at x
            close_run!(x);
            if over!() { give_up!(); }
            if w > rs { continue; }              /* the run before was reopened: x is tested against it */
        }
        // x starts or extends the run: take the whole ascending stretch from here
        // ... a cache-sized block at a time, so that the copy reads what the scan just read (a whole
        // megabyte-long run scanned and then copied is two trips to memory); the loop comes round
        // again for the pair across the block's end
        let mut j = i + first_descent(core::slice::from_raw_parts(src.add(i), (n - i).min(DETECT_BLOCK)));
        // GAP SPLIT. Two blocks whose boundary ascends read as one run with a hole in its values, and
        // whatever lies in the hole then counts as crossing it. A step larger than half of all the run
        // has spanned so far ends the run there. Only a block that rises that much can hold such a
        // step, so a long steady run pays for its first two blocks and no more.
        let mut split = false;
        if w - rs + (j - i) > GAP_MIN_RUN {
            let first = if w > rs { *dst.add(rs) } else { *src.add(i) };
            let before = if w > rs { *dst.add(w - 1) } else { *src.add(i) };
            if *src.add(j - 1) - before > 0.5 * (before - first) {
                let mut t = i + GAP_MIN_RUN.saturating_sub(w - rs);
                if t == i && w == rs { t += 1; }
                if t == i {
                    // the step into this block, from the last kept element
                    if *src.add(i) - before > 0.5 * (before - first) {
                        j = i;
                        split = true;
                    }
                    t += 1;
                }
                if !split && t < j {
                    let g = first_gap(src, t, j, first);
                    if g < j {
                        j = g;
                        split = true;
                    }
                }
            }
        }
        copy_nt(src.add(i), dst.add(w), j - i, nt);       /* nt by the ARRAY's size: n here is one thread's chunk */
        w += j - i;
        i = j;
        if split {
            runs.push(Run { start: rs, len: w - rs });
            rs = w;
        }
    }
    if w > rs {
        if w - rs <= SHORT_RUN && !runs.is_empty() {
            while w > rs {
                w -= 1;
                strays.push(*dst.add(w));
            }
        } else {
            runs.push(Run { start: rs, len: w - rs });
        }
    }
    if runs.is_empty() { return None; }
    Some(Detected { kept: w, runs, strays, scanned })
}

/// First index in t[from..len) whose element is greater than x, by a gallop from `from`.
#[inline]
unsafe fn upper_from(t: *const f64, from: usize, len: usize, x: f64) -> usize {
    let (mut lo, mut step) = (from, 1usize);
    let mut hi;
    loop {
        let probe = lo + step - 1;
        if probe >= len { hi = len; break; }
        if *t.add(probe) > x { hi = probe; break; }
        lo = probe + 1;
        step <<= 1;
    }
    while lo < hi {
        let mid = (lo + hi) / 2;
        if *t.add(mid) > x { hi = mid; } else { lo = mid + 1; }
    }
    lo
}
/// Two sorted ranges into `o`; returns the end of the output.
#[inline]
pub(crate) unsafe fn merge2(mut x: *const f64, xe: *const f64, mut y: *const f64, ye: *const f64, mut o: *mut f64) -> *mut f64 {
    while x < xe && y < ye {
        let (vx, vy) = (*x, *y);
        let ty = vy < vx;
        *o = if ty { vy } else { vx };
        o = o.add(1);
        x = x.add(!ty as usize);
        y = y.add(ty as usize);
    }
    let rx = xe.offset_from(x) as usize;
    core::ptr::copy_nonoverlapping(x, o, rx);
    o = o.add(rx);
    let ry = ye.offset_from(y) as usize;
    core::ptr::copy_nonoverlapping(y, o, ry);
    o.add(ry)
}

/// k sorted ranges into `o` through a binary min-heap of their heads.
unsafe fn merge_k(src: &mut [(*const f64, *const f64)], mut o: *mut f64) -> *mut f64 {
    let mut heap: Vec<usize> = (0..src.len()).filter(|&r| src[r].0 < src[r].1).collect();
    let less = |src: &[(*const f64, *const f64)], x: usize, y: usize| *src[x].0 < *src[y].0;
    let sift = |heap: &mut [usize], src: &[(*const f64, *const f64)], mut i: usize| {
        let k = heap.len();
        loop {
            let (l, r) = (2 * i + 1, 2 * i + 2);
            let mut m = i;
            if l < k && less(src, heap[l], heap[m]) { m = l; }
            if r < k && less(src, heap[r], heap[m]) { m = r; }
            if m == i { break; }
            heap.swap(i, m);
            i = m;
        }
    };
    for i in (0..heap.len() / 2).rev() { sift(&mut heap, src, i); }
    while heap.len() > 1 {
        let r = heap[0];
        *o = *src[r].0;
        o = o.add(1);
        src[r].0 = src[r].0.add(1);
        if src[r].0 == src[r].1 {
            let last = heap.pop().unwrap();
            heap[0] = last;
        }
        sift(&mut heap, src, 0);
    }
    if let Some(&r) = heap.first() {
        let c = src[r].1.offset_from(src[r].0) as usize;
        core::ptr::copy_nonoverlapping(src[r].0, o, c);
        o = o.add(c);
    }
    o
}

/// First index in t[from..len) whose element is not less than x, by a gallop from `from`.
#[inline]
unsafe fn lower_from(t: *const f64, from: usize, len: usize, x: f64) -> usize {
    let (mut lo, mut step) = (from, 1usize);
    let mut hi;
    loop {
        let probe = lo + step - 1;
        if probe >= len { hi = len; break; }
        if *t.add(probe) >= x { hi = probe; break; }
        lo = probe + 1;
        step <<= 1;
    }
    while lo < hi {
        let mid = (lo + hi) / 2;
        if *t.add(mid) >= x { hi = mid; } else { lo = mid + 1; }
    }
    lo
}

/// More pieces than this crossing in one interval: the merges would cost what a sort costs.
const CROSS_MAX: usize = 16;

/// One placement job: `pieces[first..last]` (ranges of the array, each sorted, all inside one value
/// interval that no other run enters) land, merged, at `out` in the output.
#[derive(Clone, Copy)]
pub(crate) struct Job {
    pub first: usize,
    pub last: usize,
    pub out: usize,
    pub len: usize,
}
pub(crate) struct Plan {
    pub pieces: Vec<(usize, usize)>,
    pub jobs: Vec<Job>,
}

/// The knot sweep. Runs are strands; walk the 2R strand endpoints in value order, and between two
/// consecutive endpoints cut every live strand (a first value cuts below itself, a last value
/// above). A piece that lies alone in its interval is copied, pieces that cross are merged. Block
/// boundaries that ascend fuse two blocks into one run with a hole in its values: the sweep cuts it
/// at whatever lies in the hole (the detector's gap split leaves few such runs). None when more
/// than CROSS_MAX pieces cross in one interval, or when the cuts alone would cost too much (each is
/// a binary search into memory that is not in the cache).
pub(crate) unsafe fn plan_runs(a: *const f64, runs: &[Run]) -> Option<Plan> {
    let mut ev: Vec<(f64, u8, u32)> = Vec::with_capacity(2 * runs.len());
    for (id, r) in runs.iter().enumerate() {
        // A strand must open before it closes. With a NaN at either end of a run its first value can
        // sort AFTER its last in total order: opened after its close, the strand was never placed
        // and the output was not a permutation (found by the NaN test at four threads).
        let (mut lo, mut hi) = (*a.add(r.start), *a.add(r.start + r.len - 1));
        if lo.total_cmp(&hi) == core::cmp::Ordering::Greater { core::mem::swap(&mut lo, &mut hi); }
        ev.push((lo, 0, id as u32));
        ev.push((hi, 1, id as u32));
    }
    // by TOTAL order: a run can hold a NaN (a NaN is never a descent), and the standard sort panics
    // on a comparison that is not a total order. Found by the NaN test, not by reading.
    ev.sort_unstable_by(|x, y| x.0.total_cmp(&y.0).then(x.1.cmp(&y.1)));
    let mut cur = vec![0usize; runs.len()];
    let mut active: Vec<u32> = Vec::new();
    let mut plan = Plan { pieces: Vec::new(), jobs: Vec::new() };
    let mut out = 0;
    let n: usize = runs.iter().map(|r| r.len).sum();
    let (mut searches, budget) = (0usize, 16 * runs.len() + n / 1024 + 1024);
    for &(v, kind, id) in &ev {
        if !active.is_empty() {
            searches += active.len();
            if searches > budget { return None; }
            let first = plan.pieces.len();
            let mut total = 0;
            for &q in &active {
                let r = runs[q as usize];
                let c0 = cur[q as usize];
                let c = if kind == 1 && q == id { r.len }
                        else if kind == 0 { lower_from(a.add(r.start), c0, r.len, v) }
                        else { upper_from(a.add(r.start), c0, r.len, v) };
                if c > c0 {
                    plan.pieces.push((r.start + c0, c - c0));
                    total += c - c0;
                    cur[q as usize] = c;
                }
            }
            if total > 0 {
                if plan.pieces.len() - first > CROSS_MAX { return None; }
                plan.jobs.push(Job { first, last: plan.pieces.len(), out, len: total });
                out += total;
            }
        }
        if kind == 0 {
            active.push(id);
        } else if let Some(k) = active.iter().position(|&q| q == id) {
            active.swap_remove(k);
        }
    }
    // every strand has closed and been placed whole; this holds whatever the values were
    debug_assert!(active.is_empty() && runs.iter().enumerate().all(|(q, r)| cur[q] == r.len));
    Some(plan)
}

/// A copy whose destination is not read again soon: with `nt`, in non-temporal stores (a plain
/// store fetches the destination line first). The caller issues the fence.
#[inline]
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn copy_nt(mut s: *const f64, mut d: *mut f64, mut len: usize, nt: bool) {
    if !nt || len < 2048 {
        core::ptr::copy_nonoverlapping(s, d, len);
        return;
    }
    while (d as usize) & 31 != 0 {
        *d = *s;
        s = s.add(1);
        d = d.add(1);
        len -= 1;
    }
    while len >= 16 {
        _mm256_stream_pd(d, _mm256_loadu_pd(s));
        _mm256_stream_pd(d.add(4), _mm256_loadu_pd(s.add(4)));
        _mm256_stream_pd(d.add(8), _mm256_loadu_pd(s.add(8)));
        _mm256_stream_pd(d.add(12), _mm256_loadu_pd(s.add(12)));
        s = s.add(16);
        d = d.add(16);
        len -= 16;
    }
    core::ptr::copy_nonoverlapping(s, d, len);
}

/// A long sorted range and a short one into `o`: gallop through the long one for each element of
/// the short one, and move what lies between in bulk.
#[target_feature(enable = "avx2")]
unsafe fn merge2_gallop(big: *const f64, nbig: usize, small: *const f64, nsmall: usize, mut o: *mut f64, nt: bool) {
    let mut bi = 0;
    for q in 0..nsmall {
        let x = *small.add(q);
        let u = upper_from(big, bi, nbig, x);
        copy_nt(big.add(bi), o, u - bi, nt);
        o = o.add(u - bi);
        *o = x;
        o = o.add(1);
        bi = u;
    }
    copy_nt(big.add(bi), o, nbig - bi, nt);
}

/// Places one job at `o` (the output's base).
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn place_job(a: *const f64, plan: &Plan, j: &Job, o: *mut f64, nt: bool) {
    let o = o.add(j.out);
    let pc = &plan.pieces[j.first..j.last];
    match pc.len() {
        1 => copy_nt(a.add(pc[0].0), o, pc[0].1, nt),
        2 => {
            // x is the piece that starts lower. Its head lies alone under y: copy it. Whichever piece
            // reaches higher has a tail that lies alone above the other: copy that too. Merge the rest.
            let (mut x, mut xl, mut y, mut yl) = (a.add(pc[0].0), pc[0].1, a.add(pc[1].0), pc[1].1);
            if *y < *x {
                core::mem::swap(&mut x, &mut y);
                core::mem::swap(&mut xl, &mut yl);
            }
            let head = upper_from(x, 0, xl, *y);
            copy_nt(x, o, head, nt);
            let (x, xl, o) = (x.add(head), xl - head, o.add(head));
            if xl == 0 {
                copy_nt(y, o, yl, nt);
            } else if *x.add(xl - 1) <= *y.add(yl - 1) {
                let cy = upper_from(y, 0, yl, *x.add(xl - 1));
                place_two(x, xl, y, cy, o, nt);
                copy_nt(y.add(cy), o.add(xl + cy), yl - cy, nt);
            } else {
                let cx = upper_from(x, 0, xl, *y.add(yl - 1));
                place_two(x, cx, y, yl, o, nt);
                copy_nt(x.add(cx), o.add(cx + yl), xl - cx, nt);
            }
        }
        _ => {
            // pieces that do not cross are laid end to end; otherwise the heap
            let mut ord: Vec<(usize, usize)> = pc.to_vec();
            ord.sort_unstable_by(|x, y| (*a.add(x.0)).total_cmp(&*a.add(y.0)));
            if (1..ord.len()).all(|q| *a.add(ord[q - 1].0 + ord[q - 1].1 - 1) <= *a.add(ord[q].0)) {
                let mut o = o;
                for &(st, l) in &ord {
                    copy_nt(a.add(st), o, l, nt);
                    o = o.add(l);
                }
            } else {
                let mut src: Vec<(*const f64, *const f64)> = pc.iter().map(|&(s, l)| (a.add(s), a.add(s + l))).collect();
                merge_k(&mut src, o);
            }
        }
    }
}
/// Two sorted ranges that cross: by gallop when one is at least GALLOP_RATIO times the other.
/// MEASURED, both ways: the plain merge costs about 3 ns per element of output (its two cursors
/// are one dependency chain), a gallop and a small copy about 50 ns per element of the SHORT
/// range, so the gallop wins while the short range is under about 6%. At 64 a strand with 2%
/// strays took the plain merge and read 0.79 of the C engine where the gallop reads 1.33.
const GALLOP_RATIO: usize = 16;
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn place_two(x: *const f64, xl: usize, y: *const f64, yl: usize, o: *mut f64, nt: bool) {
    if xl >= GALLOP_RATIO * yl {
        merge2_gallop(x, xl, y, yl, o, nt);
    } else if yl >= GALLOP_RATIO * xl {
        merge2_gallop(y, yl, x, xl, o, nt);
    } else {
        merge2(x, x.add(xl), y, y.add(yl), o);
    }
}
