//! The unstructured leaf: an AVX2 quicksort for `f64` (four doubles per vector).
//!
//! The Rust twin of the C engine's in-house kernel (`avx2_qsort_f64` in `logos_psort.c`, the
//! x86-simd-sort design): the partition splits each vector with one compare, one table-driven
//! permute and two plain stores; ranges of at most 64 elements go to straight-line bitonic
//! networks (generated: `gen_nets.py`) that read and write the range directly with masked loads
//! for the partial vector; the pivot is the middle of the lane medians of five sampled vectors.
//!
//! The FIRST partition of a sort also carries the NaN / -0.0 scan, so no pass precedes the sort
//! (the C engine's `av_qsort_fused`). A NaN declines (the array is then a permutation of the
//! input and the caller sorts it by total order); negative zeros are turned into +0.0, counted,
//! and put back at the head of the zeros afterwards, because `min` / `max` cannot tell them apart.

#![allow(clippy::missing_safety_doc)]

use core::arch::x86_64::*;
use core::mem::MaybeUninit;

#[repr(C, align(32))]
struct Al<T>(T);

/// For mask m (bit j = lane j goes right): lanes NOT in m first, then the lanes in m, as dword
/// indices for vpermd on the float view. One permutation serves both stores.
static LUT: Al<[[u32; 8]; 16]> = Al(build_lut());
const fn build_lut() -> [[u32; 8]; 16] {
    let mut t = [[0u32; 8]; 16];
    let mut m = 0;
    while m < 16 {
        let mut pos = 0;
        let mut pass = 0;
        while pass < 2 {
            let mut j = 0;
            while j < 4 {
                if (m >> j) & 1 == pass {
                    t[m][pos] = 2 * j as u32;
                    t[m][pos + 1] = 2 * j as u32 + 1;
                    pos += 2;
                }
                j += 1;
            }
            pass += 1;
        }
        m += 1;
    }
    t
}
/// KMASK[k]: the first k lanes; KMASK_HI[k]: the last k lanes.
static KMASK: Al<[[i64; 4]; 5]> = Al([[0, 0, 0, 0], [-1, 0, 0, 0], [-1, -1, 0, 0], [-1, -1, -1, 0], [-1, -1, -1, -1]]);
static KMASK_HI: Al<[[i64; 4]; 5]> = Al([[0, 0, 0, 0], [0, 0, 0, -1], [0, 0, -1, -1], [0, -1, -1, -1], [-1, -1, -1, -1]]);

/// The value, through an empty assembly block: the optimiser can no longer see where it came from.
/// Used on the permuted copy inside a compare-exchange. Left to itself LLVM notices that `v` and a
/// permutation of `v` have symmetric minima and maxima, narrows the pair to 128-bit halves and
/// rebuilds the vector with extra shuffles: half again as many port-5 operations as the three
/// written here, and port 5 is what a network waits for (measured: the networks 0.77-0.90 of the
/// C engine's, which GCC compiles as written).
macro_rules! opaque {
    ($x:expr) => {{
        let mut x: __m256d = $x;
        core::arch::asm!("/* {0} */", inout(ymm_reg) x, options(pure, nomem, nostack, preserves_flags));
        x
    }};
}
macro_rules! ce {
    ($v:ident, $t:expr, $m:literal) => {{
        let t = opaque!($t);
        $v = opaque!(_mm256_blend_pd::<$m>(_mm256_min_pd($v, t), _mm256_max_pd($v, t)));   /* or the next permute is folded into this blend */
    }};
}

#[inline]
#[target_feature(enable = "avx2")]
unsafe fn rev(v: __m256d) -> __m256d {
    _mm256_permute4x64_pd::<0x1B>(v)
}
/// Four lanes -> ascending.
#[inline]
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn sort1(mut v: __m256d) -> __m256d {
    ce!(v, _mm256_permute_pd::<0x5>(v), 0xA);
    ce!(v, rev(v), 0xC);
    ce!(v, _mm256_permute_pd::<0x5>(v), 0xA);
    v
}
/// A four-lane bitonic sequence -> ascending.
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn merge1(mut v: __m256d) -> __m256d {
    ce!(v, _mm256_permute4x64_pd::<0x4E>(v), 0xC);
    ce!(v, _mm256_permute_pd::<0x5>(v), 0xA);
    v
}
/// Vector i of a range of n elements: whole, partial (padded with +inf) or all +inf.
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn ld(a: *const f64, n: usize, i: usize) -> __m256d {
    let o = 4 * i;
    if o + 4 <= n {
        _mm256_loadu_pd(a.add(o))
    } else if o >= n {
        _mm256_set1_pd(f64::INFINITY)
    } else {
        let k = _mm256_load_si256(KMASK.0.get_unchecked(n - o).as_ptr() as *const __m256i);
        _mm256_blendv_pd(_mm256_set1_pd(f64::INFINITY), _mm256_maskload_pd(a.add(o), k), _mm256_castsi256_pd(k))
    }
}
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn st(a: *mut f64, n: usize, i: usize, v: __m256d) {
    let o = 4 * i;
    if o + 4 <= n {
        _mm256_storeu_pd(a.add(o), v)
    } else if o < n {
        _mm256_maskstore_pd(a.add(o), _mm256_load_si256(KMASK.0.get_unchecked(n - o).as_ptr() as *const __m256i), v)
    }
}

include!("kernel_nets.rs");

/// Sorts a[0..n), n <= 64, no NaN.
#[inline]
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn base(a: *mut f64, n: usize) {
    if n <= 1 {
    } else if n <= 4 {
        st(a, n, 0, sort1(ld(a, n, 0)));
    } else if n <= 8 {
        net8(a, n)
    } else if n <= 16 {
        net16(a, n)
    } else if n <= 32 {
        net32(a, n)
    } else {
        net64(a, n)
    }
}

/// What one partition pass learns besides the split.
pub(crate) struct Part {
    pub(crate) sm: f64,
    pub(crate) bg: f64,
    pub(crate) nan: bool,
    pub(crate) negz: bool,
}

/// One vector into both ends. `$store` is `plain` (both free windows hold four slots) or `masked` (exact).
macro_rules! part_vec {
    ($a:expr, $l:ident, $r:ident, $v:expr, $piv:ident, $mn:ident, $mx:ident, $nanacc:ident, $nzacc:ident, $sign:ident, GT = $gt:ident, FL = $fl:ident, $store:ident) => {{
        let v = $v;
        let cm = if $gt { _mm256_cmp_pd::<_CMP_GT_OQ>(v, $piv) } else { _mm256_cmp_pd::<_CMP_GE_OQ>(v, $piv) };
        let m = _mm256_movemask_pd(cm) as usize;
        let k = m.count_ones() as usize;
        let p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256(LUT.0.get_unchecked(m).as_ptr() as *const __m256i)));
        part_vec!(@$store $a, $l, $r, p, k);
        $l += 4 - k;
        $r -= k;
        $mn = _mm256_min_pd($mn, v);
        $mx = _mm256_max_pd($mx, v);
        if $fl {
            $nanacc = _mm256_or_pd($nanacc, _mm256_cmp_pd::<_CMP_UNORD_Q>(v, v));
            $nzacc = _mm256_or_si256($nzacc, _mm256_cmpeq_epi64(_mm256_castpd_si256(v), $sign));
        }
    }};
    (@plain $a:expr, $l:ident, $r:ident, $p:ident, $k:ident) => {
        _mm256_storeu_pd($a.add($l), $p);
        _mm256_storeu_pd($a.add($r).wrapping_sub(4), $p);
    };
    (@masked $a:expr, $l:ident, $r:ident, $p:ident, $k:ident) => {
        _mm256_maskstore_pd($a.add($l), _mm256_load_si256(KMASK.0.get_unchecked(4 - $k).as_ptr() as *const __m256i), $p);
        _mm256_maskstore_pd($a.add($r).wrapping_sub(4), _mm256_load_si256(KMASK_HI.0.get_unchecked($k).as_ptr() as *const __m256i), $p);
    };
}

/// An empty assembly block: something the optimiser cannot speculate, so the `if` around it is
/// compiled as a branch and never as a conditional move.
#[inline(always)]
pub(crate) fn keep_branch() {
    unsafe { core::arch::asm!("", options(nomem, nostack, preserves_flags)) };
}

const SMALL_N: usize = 512;
const UNROLL: usize = 8;

/// Partitions a[0..n): returns the first index of the right part (`>= pivot`, or `> pivot` with GT).
/// Tracks the range's minimum and maximum, and with FL whether it holds a NaN or a negative zero.
#[inline]
#[target_feature(enable = "avx2,popcnt")]
pub(crate) unsafe fn partition<const GT: bool, const FL: bool>(a: *mut f64, n: usize, pivot: f64, out: &mut Part) -> usize {
    let piv = _mm256_set1_pd(pivot);
    let sign = _mm256_set1_epi64x(i64::MIN);
    let mut mn = _mm256_set1_pd(f64::INFINITY);
    let mut mx = _mm256_set1_pd(f64::NEG_INFINITY);
    let mut nanacc = _mm256_setzero_pd();
    let mut nzacc = _mm256_setzero_si256();
    let (mut sm, mut bg) = (f64::INFINITY, f64::NEG_INFINITY);
    let (mut snan, mut snz) = (false, false);
    let split;
    if n < SMALL_N {
        // Small, L1-resident: stream through once, writing every permuted vector to both ends of a
        // stack buffer; the last two vectors use masked stores because their stores can overlap.
        let mut tmp: MaybeUninit<Al<[f64; SMALL_N]>> = MaybeUninit::uninit();
        let t = tmp.as_mut_ptr() as *mut f64;
        let (mut l, mut r, nv) = (0usize, n, n >> 2);
        let mut i = 0;
        while i + 2 < nv {
            part_vec!(t, l, r, _mm256_loadu_pd(a.add(4 * i)), piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, plain);
            i += 1;
        }
        while i < nv {
            part_vec!(t, l, r, _mm256_loadu_pd(a.add(4 * i)), piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, masked);
            i += 1;
        }
        for i in (nv << 2)..n {
            let x = *a.add(i);
            if x < sm { sm = x; }
            if x > bg { bg = x; }
            if FL { snan |= x != x; snz |= x.to_bits() == 1u64 << 63; }
            let right = if GT { x > pivot } else { x >= pivot };
            if right { r -= 1; *t.add(r) = x; } else { *t.add(l) = x; l += 1; }
        }
        core::ptr::copy_nonoverlapping(t, a, n);
        split = l;
    } else {
        let (mut lo, mut hi) = (0usize, n);
        while (hi - lo) & 3 != 0 {
            let x = *a.add(lo);
            if x < sm { sm = x; }
            if x > bg { bg = x; }
            if FL { snan |= x != x; snz |= x.to_bits() == 1u64 << 63; }
            let right = if GT { x > pivot } else { x >= pivot };
            if right { hi -= 1; *a.add(lo) = *a.add(hi); *a.add(hi) = x; } else { lo += 1; }
        }
        // UNROLL vectors are held back at each end, so that a store never overruns unread data,
        // and UNROLL are loaded per end-choice, so that branch is paid once per 32 elements.
        let mut vl = [_mm256_setzero_pd(); UNROLL];
        let mut vr = [_mm256_setzero_pd(); UNROLL];
        for i in 0..UNROLL {
            vl[i] = _mm256_loadu_pd(a.add(lo + 4 * i));
            vr[i] = _mm256_loadu_pd(a.add(hi - 4 * UNROLL + 4 * i));
        }
        let (mut left, mut right) = (lo + 4 * UNROLL, hi - 4 * UNROLL);
        let (mut l, mut r) = (lo, hi);
        while right - left >= 4 * UNROLL {
            // This must stay a BRANCH. As a conditional move (which is what LLVM makes of it unaided)
            // the eight loads below wait for the previous block's whole chain of popcounts; as a
            // predicted branch they are issued ahead of it. Measured: 0.85 of the C kernel -> level.
            let src;
            if r - right < left - l {
                keep_branch();
                right -= 4 * UNROLL;
                src = right;
            } else {
                src = left;
                left += 4 * UNROLL;
            }
            let mut v = [_mm256_setzero_pd(); UNROLL];
            for i in 0..UNROLL { v[i] = _mm256_loadu_pd(a.add(src + 4 * i)); }
            for i in 0..UNROLL {
                part_vec!(a, l, r, v[i], piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, plain);
            }
        }
        while right > left {
            let v;
            if r - right < left - l {
                keep_branch();
                right -= 4;
                v = _mm256_loadu_pd(a.add(right));
            } else {
                v = _mm256_loadu_pd(a.add(left));
                left += 4;
            }
            part_vec!(a, l, r, v, piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, plain);
        }
        for i in 0..UNROLL { part_vec!(a, l, r, vl[i], piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, masked); }
        for i in 0..UNROLL { part_vec!(a, l, r, vr[i], piv, mn, mx, nanacc, nzacc, sign, GT = GT, FL = FL, masked); }
        split = l;
    }
    let mut t = [0f64; 4];
    _mm256_storeu_pd(t.as_mut_ptr(), mn);
    for q in t { if q < sm { sm = q; } }
    _mm256_storeu_pd(t.as_mut_ptr(), mx);
    for q in t { if q > bg { bg = q; } }
    out.sm = sm;
    out.bg = bg;
    if FL {
        out.nan = snan || _mm256_movemask_pd(nanacc) != 0;
        out.negz = snz || _mm256_movemask_pd(_mm256_castsi256_pd(nzacc)) != 0;
    }
    split
}

/// The pivot of a[0..n), n > 64: five vectors spread over the range, the median of each lane (a
/// nine-comparator network across the vectors), then the upper middle of those four medians.
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn pivot(a: *const f64, n: usize) -> f64 {
    let step = (n - 4) / 4;
    let mut v0 = _mm256_loadu_pd(a);
    let mut v1 = _mm256_loadu_pd(a.add(step));
    let mut v2 = _mm256_loadu_pd(a.add(2 * step));
    let mut v3 = _mm256_loadu_pd(a.add(3 * step));
    let mut v4 = _mm256_loadu_pd(a.add(4 * step));
    macro_rules! cx {
        ($x:ident, $y:ident) => {{
            let t = _mm256_min_pd($x, $y);
            $y = _mm256_max_pd($x, $y);
            $x = t;
        }};
    }
    cx!(v0, v1); cx!(v3, v4); cx!(v2, v4); cx!(v2, v3); cx!(v1, v4);
    cx!(v0, v3); cx!(v0, v2); cx!(v1, v3); cx!(v1, v2);
    let _ = (v0, v1, v3, v4);
    let m = sort1(v2);
    let mut t = [0f64; 4];
    _mm256_storeu_pd(t.as_mut_ptr(), m);
    t[2]
}

#[target_feature(enable = "avx2,popcnt")]
pub(crate) unsafe fn qsort_rec(mut a: *mut f64, mut n: usize, mut depth: u32) {
    let mut st = Part { sm: 0.0, bg: 0.0, nan: false, negz: false };
    while n > 64 {
        if depth == 0 {
            core::slice::from_raw_parts_mut(a, n).sort_unstable_by(f64::total_cmp);
            return;
        }
        depth -= 1;
        let p = pivot(a, n);
        let m = partition::<false, false>(a, n, p, &mut st);
        if st.sm == st.bg { return; }
        if m == 0 {
            // the pivot is the minimum: split off the run of its equals
            let m = partition::<true, false>(a, n, p, &mut st);
            a = a.add(m);
            n -= m;
            continue;
        }
        if p == st.bg { n = m; continue; }
        if m < n - m {
            qsort_rec(a, m, depth);
            a = a.add(m);
            n -= m;
        } else {
            qsort_rec(a.add(m), n - m, depth);
            n = m;
        }
    }
    base(a, n);
}

#[inline]
fn depth_for(n: usize) -> u32 {
    2 * (usize::BITS - n.leading_zeros()) + 8
}

/// -0.0 -> +0.0 over the range; returns how many there were.
#[target_feature(enable = "avx2")]
unsafe fn negz_strip(a: *mut f64, n: usize) -> usize {
    let mut c = 0;
    for i in 0..n {
        if (*a.add(i)).to_bits() == 1u64 << 63 {
            *a.add(i) = 0.0;
            c += 1;
        }
    }
    c
}
/// The first `c` zeros of the sorted range become -0.0 again.
unsafe fn negz_restore(a: *mut f64, n: usize, c: usize) {
    let s = core::slice::from_raw_parts_mut(a, n);
    let z = s.partition_point(|&x| x < 0.0);
    for x in &mut s[z..z + c] {
        *x = -0.0;
    }
}

/// Sorts `a` ascending. Returns false, with `a` a permutation of its input, if it holds a NaN.
/// Requires AVX2 (the caller checks the CPU).
#[target_feature(enable = "avx2,popcnt")]
pub unsafe fn sort(a: &mut [f64]) -> bool {
    let n = a.len();
    let p = a.as_mut_ptr();
    if n <= 64 {
        let (mut nan, mut nz) = (false, false);
        for &x in a.iter() {
            nan |= x != x;
            nz |= x.to_bits() == 1u64 << 63;
        }
        if nan { return false; }
        let c = if nz { negz_strip(p, n) } else { 0 };
        base(p, n);
        if c > 0 { negz_restore(p, n, c); }
        return true;
    }
    let depth = depth_for(n);
    let mut st = Part { sm: 0.0, bg: 0.0, nan: false, negz: false };
    let pv = pivot(p, n);
    let m = partition::<false, true>(p, n, pv, &mut st);
    if st.nan { return false; }
    if st.negz {
        let c = negz_strip(p, n);
        qsort_rec(p, n, depth);
        negz_restore(p, n, c);
        return true;
    }
    if st.sm == st.bg { return true; }
    if m == 0 || pv == st.bg {
        qsort_rec(p, n, depth);
    } else {
        qsort_rec(p, m, depth);
        qsort_rec(p.add(m), n - m, depth);
    }
    true
}

/// One partition pass: everything less than `pivot` to the front (a NaN goes there too); returns
/// how many. For the two-halves route.
#[target_feature(enable = "avx2,popcnt")]
pub unsafe fn split(a: &mut [f64], pivot: f64) -> usize {
    let mut st = Part { sm: 0.0, bg: 0.0, nan: false, negz: false };
    partition::<false, false>(a.as_mut_ptr(), a.len(), pivot, &mut st)
}
