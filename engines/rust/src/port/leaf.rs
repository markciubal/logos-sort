//! C lines 851-1980 and 2739-3144 of `logos_psort.c`: the AVX2 leaf's entry points and scans, flash, coarse, the skew and
//! quantization probes, the leaf's run detection and merges, `sort_fn`, the float32 narrowing and `leaf_f64`.
//!
//! The AVX2 quicksort proper (the bitonic networks, `av_partition`, `av_qsort_rec`) is `crate::kernel`; what the C has around
//! it and `kernel.rs` has not (the fused first partition, the scan-verdict protocol, the C's own pivot) is ported here.
use super::*;

// ---- C 851-852 ---------------------------------------------------------------------------------------------------------
pub(crate) static mut g_avx2: i32 = -1;
#[inline]
pub(crate) unsafe fn avx2_leaf_on() -> i32 {                                 // LOGOS_NO_AVX2 unset
    let mut v = g_avx2;
    if v < 0 { v = (is_x86_feature_detected!("avx2") && is_x86_feature_detected!("fma")) as i32; g_avx2 = v; }
    v
}

// ---- C 856-875: the partition's tables (the networks that also read them are crate::kernel's own copies) ---------------
#[repr(C, align(32))]
pub(crate) struct av_al32<T>(pub(crate) T);
pub(crate) static av_lut: av_al32<[[u32; 8]; 16]> = av_al32([
    [0, 1, 2, 3, 4, 5, 6, 7],
    [2, 3, 4, 5, 6, 7, 0, 1],
    [0, 1, 4, 5, 6, 7, 2, 3],
    [4, 5, 6, 7, 0, 1, 2, 3],
    [0, 1, 2, 3, 6, 7, 4, 5],
    [2, 3, 6, 7, 0, 1, 4, 5],
    [0, 1, 6, 7, 2, 3, 4, 5],
    [6, 7, 0, 1, 2, 3, 4, 5],
    [0, 1, 2, 3, 4, 5, 6, 7],
    [2, 3, 4, 5, 0, 1, 6, 7],
    [0, 1, 4, 5, 2, 3, 6, 7],
    [4, 5, 0, 1, 2, 3, 6, 7],
    [0, 1, 2, 3, 4, 5, 6, 7],
    [2, 3, 0, 1, 4, 5, 6, 7],
    [0, 1, 2, 3, 4, 5, 6, 7],
    [0, 1, 2, 3, 4, 5, 6, 7],
]);
pub(crate) static av_kmask: av_al32<[[i64; 4]; 5]> = av_al32([[0, 0, 0, 0], [-1, 0, 0, 0], [-1, -1, 0, 0], [-1, -1, -1, 0], [-1, -1, -1, -1]]);
pub(crate) static av_kmask_hi: av_al32<[[i64; 4]; 5]> = av_al32([[0, 0, 0, 0], [0, 0, 0, -1], [0, 0, -1, -1], [0, -1, -1, -1], [-1, -1, -1, -1]]);
pub(crate) const AV_UNROLL: usize = 8;                                       // C 849 (base.rs's range ends at 850; needed here)
pub(crate) const AV_SMALL_N: usize = 512;
pub(crate) const AV_PIV_SMALL: usize = 512;
pub(crate) const AV_VPIVOT: i32 = 2;
pub(crate) const AV_FUSE_MIN_N: usize = 65536;

/// C 1215. one vector, plain stores: valid only while both free windows hold >= 4 slots
#[inline]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_part_vec(a: *mut f64, l: *mut usize, r: *mut usize, v: __m256d, piv: __m256d, gt: i32, mn: *mut __m256d, mx: *mut __m256d) {
    let cm = if gt != 0 { _mm256_cmp_pd::<_CMP_GT_OQ>(v, piv) } else { _mm256_cmp_pd::<_CMP_GE_OQ>(v, piv) };
    let m = _mm256_movemask_pd(cm);
    let k = (m as u32).count_ones() as i32;
    let p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256(av_lut.0[m as usize].as_ptr() as *const __m256i)));
    _mm256_storeu_pd(a.wrapping_add(*l), p);
    _mm256_storeu_pd(a.wrapping_add(*r).wrapping_sub(4), p);
    *l += (4 - k) as usize; *r -= k as usize;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}
/// C 1225. one vector, masked stores: exact, for the reserved vectors at the end
#[inline]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_part_vec_m(a: *mut f64, l: *mut usize, r: *mut usize, v: __m256d, piv: __m256d, gt: i32, mn: *mut __m256d, mx: *mut __m256d) {
    let cm = if gt != 0 { _mm256_cmp_pd::<_CMP_GT_OQ>(v, piv) } else { _mm256_cmp_pd::<_CMP_GE_OQ>(v, piv) };
    let m = _mm256_movemask_pd(cm);
    let k = (m as u32).count_ones() as i32;
    let p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256(av_lut.0[m as usize].as_ptr() as *const __m256i)));
    _mm256_maskstore_pd(a.wrapping_add(*l), _mm256_load_si256(av_kmask.0[(4 - k) as usize].as_ptr() as *const __m256i), p);                    /* first 4-k lanes */
    _mm256_maskstore_pd(a.wrapping_add(*r).wrapping_sub(4), _mm256_load_si256(av_kmask_hi.0[k as usize].as_ptr() as *const __m256i), p);       /* last k lanes land at r-k..r */
    *l += (4 - k) as usize; *r -= k as usize;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}

/// C 1305: five vectors from spread positions, a 9-comparator network across them, the middle of the four lane medians.
#[inline]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_pivot_vec(a: *const f64, n: usize) -> f64 {
    let d = (n - 4) / 5;
    let (mut v0, mut v1, mut v2) = (_mm256_loadu_pd(a), _mm256_loadu_pd(a.add(d)), _mm256_loadu_pd(a.add(2 * d)));
    let (mut v3, mut v4) = (_mm256_loadu_pd(a.add(3 * d)), _mm256_loadu_pd(a.add(4 * d)));
    macro_rules! AV_MM { ($x:ident, $y:ident) => {{ let lo_ = _mm256_min_pd($x, $y); $y = _mm256_max_pd($x, $y); $x = lo_; }}; }
    AV_MM!(v0, v3); AV_MM!(v1, v4); AV_MM!(v0, v2); AV_MM!(v1, v3); AV_MM!(v0, v1);
    AV_MM!(v2, v4); AV_MM!(v1, v2); AV_MM!(v3, v4); AV_MM!(v2, v3);
    let mut t = [0f64; 4];
    _mm256_storeu_pd(t.as_mut_ptr(), crate::kernel::sort1(v2));               // av_sort1
    t[2]
}
/// C 1314. AV_VPIVOT == 2: every range takes the vector pivot (n > 64 always here); the older pivots behind it are dead.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_pivot(a: *const f64, n: usize) -> f64 { av_pivot_vec(a, n) }

/// C 1350, literal: the C's pivot (av_pivot) and its depth fallback (the scalar introsort). The partition and the base-case
/// networks are `crate::kernel`'s, which are the C's av_partition / av_base.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_qsort_rec(a: *mut f64, n: usize, depth: i32) {
    let (mut a, mut n, mut depth) = (a, n, depth);
    let mut st = crate::kernel::Part { sm: 0.0, bg: 0.0, nan: false, negz: false };
    while n > 64 {
        let d0 = depth; depth -= 1;
        if d0 == 0 { quicksort(a, 0, n as i64 - 1, 2 * log2i(n)); return }
        let p = av_pivot(a, n);
        let mut m = crate::kernel::partition::<false, false>(a, n, p, &mut st);
        if st.sm == st.bg { return }                            /* all equal */
        if m == 0 {                                             /* pivot is the minimum: split off the run of equals */
            m = crate::kernel::partition::<true, false>(a, n, p, &mut st);
            a = a.add(m); n -= m; continue;
        }
        if p == st.bg { n = m; continue }                       /* right part is all == pivot */
        if m < n - m { av_qsort_rec(a, m, depth); a = a.add(m); n -= m }
        else         { av_qsort_rec(a.add(m), n - m, depth); n = m }
    }
    crate::kernel::base(a, n);
}

/// C 1367.
#[inline]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_has_nan(a: *const f64, n: usize) -> i32 {
    let mut acc = _mm256_setzero_pd();
    let mut i = 0usize;
    while i + 4 <= n { let v = _mm256_loadu_pd(a.add(i)); acc = _mm256_or_pd(acc, _mm256_cmp_pd::<_CMP_UNORD_Q>(v, v)); i += 4; }
    let mut any = _mm256_movemask_pd(acc);
    while i < n { any |= (*a.add(i) != *a.add(i)) as i32; i += 1; }
    (any != 0) as i32
}
/// C 1379. NaN (bit 0) and -0.0 (bit 1): ONE compare per block, "equal to zero or unordered"; a scalar pass tells them apart
/// only when something was flagged.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_scan_nan_negzero(a: *const f64, n: usize) -> i32 {
    let (mut acc, zero) = (_mm256_setzero_pd(), _mm256_setzero_pd());
    let mut i = 0usize;
    while i + 4 <= n { acc = _mm256_or_pd(acc, _mm256_cmp_pd::<_CMP_EQ_UQ>(_mm256_loadu_pd(a.add(i)), zero)); i += 4; }
    let mut flagged = (_mm256_movemask_pd(acc) != 0) as i32;
    while i < n { flagged |= (*a.add(i) != *a.add(i)) as i32 | (*a.add(i) == 0.0) as i32; i += 1; }
    if flagged == 0 { return 0 }
    let (mut nan, mut negz) = (0i32, 0i32);
    for k in 0..n { let x = *a.add(k); nan |= (x != x) as i32; negz |= (x == 0.0 && x.is_sign_negative()) as i32; }
    nan | (negz << 1)
}
/// C 1401.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct sf_prof_t { pub(crate) mn: f64, pub(crate) mx: f64, pub(crate) sc: i32 }
/// C 1402: the leaf's profile pass, vectorized: min, max and the NaN-or-zero flag. sc = 0: neither; -1: something flagged.
#[inline(never)]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn sf_profile_avx2(a: *const f64, n: usize, mn0: f64, mx0: f64) -> sf_prof_t {
    let mut m0 = _mm256_set1_pd(mn0); let (mut m1, mut m2, mut m3) = (m0, m0, m0);
    let mut x0 = _mm256_set1_pd(mx0); let (mut x1, mut x2, mut x3) = (x0, x0, x0);
    let (mut acc, zero) = (_mm256_setzero_pd(), _mm256_setzero_pd());
    let mut i = 0usize;
    while i + 16 <= n {
        let (v0, v1, v2, v3) = (_mm256_loadu_pd(a.add(i)), _mm256_loadu_pd(a.add(i + 4)), _mm256_loadu_pd(a.add(i + 8)), _mm256_loadu_pd(a.add(i + 12)));
        m0 = _mm256_min_pd(v0, m0); m1 = _mm256_min_pd(v1, m1); m2 = _mm256_min_pd(v2, m2); m3 = _mm256_min_pd(v3, m3);
        x0 = _mm256_max_pd(v0, x0); x1 = _mm256_max_pd(v1, x1); x2 = _mm256_max_pd(v2, x2); x3 = _mm256_max_pd(v3, x3);
        acc = _mm256_or_pd(acc, _mm256_or_pd(_mm256_or_pd(_mm256_cmp_pd::<_CMP_EQ_UQ>(v0, zero), _mm256_cmp_pd::<_CMP_EQ_UQ>(v1, zero)),
                                             _mm256_or_pd(_mm256_cmp_pd::<_CMP_EQ_UQ>(v2, zero), _mm256_cmp_pd::<_CMP_EQ_UQ>(v3, zero))));
        i += 16;
    }
    m0 = _mm256_min_pd(_mm256_min_pd(m0, m1), _mm256_min_pd(m2, m3)); x0 = _mm256_max_pd(_mm256_max_pd(x0, x1), _mm256_max_pd(x2, x3));
    let mut flagged = (_mm256_movemask_pd(acc) != 0) as i32;
    let (mut mn, mut mx) = (mn0, mx0);
    let mut t = [0f64; 4];
    _mm256_storeu_pd(t.as_mut_ptr(), m0); for q in 0..4 { if t[q] < mn { mn = t[q]; } }
    _mm256_storeu_pd(t.as_mut_ptr(), x0); for q in 0..4 { if t[q] > mx { mx = t[q]; } }
    while i < n { let v = *a.add(i); if v < mn { mn = v; } else if v > mx { mx = v; } flagged |= (v != v) as i32 | (v == 0.0) as i32; i += 1; }
    sf_prof_t { mn, mx, sc: if flagged != 0 { -1 } else { 0 } }
}

/// C 1452. the kernel proper, NaN-free and every zero +0.0: the in-house recursion
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_sort_core(a: *mut f64, n: usize, P: i32) {
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
}
/// C 1461. the -0.0 made +0.0 before the sort go back at the start of the zero run
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_negz_restore(a: *mut f64, n: usize, nneg: usize) {
    let (mut lo, mut hi) = (0usize, n);                                      /* first element >= 0.0: the zero run starts here */
    while lo < hi { let mid = lo + (hi - lo) / 2; if *a.add(mid) < 0.0 { lo = mid + 1; } else { hi = mid; } }
    for k in 0..nneg { *a.add(lo + k) = -0.0; }
}
pub(crate) static g_fuse_parts: AtomicUsize = AtomicUsize::new(0);           /* logos_twist_last(15): fused first partitions */
pub(crate) static g_fuse_declines: AtomicUsize = AtomicUsize::new(0);        /* logos_twist_last(16): NaN declines after one */

/// C 1475. av_partition's loop for a whole range of at least AV_FUSE_MIN_N, gt = 0, with ONE compare and one OR per vector
/// added: the NaN-or-zero flag. *flag = 1 when a NaN or a zero went by.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_partition_fl(a: *mut f64, n: usize, pivot: f64, smallest: *mut f64, biggest: *mut f64, flag: *mut i32) -> usize {
    const U: usize = AV_UNROLL;
    let (mut lo, mut hi) = (0usize, n);
    let (mut sm, mut bg) = (*smallest, *biggest);
    let mut fs = 0i32;
    while (hi - lo) & 3 != 0 {                                               /* scalar until the length is a multiple of 4 */
        let x = *a.add(lo); if x < sm { sm = x; } if x > bg { bg = x; } fs |= (x != x) as i32 | (x == 0.0) as i32;
        if x >= pivot { hi -= 1; *a.add(lo) = *a.add(hi); *a.add(hi) = x; } else { lo += 1; }
    }
    let piv = _mm256_set1_pd(pivot);
    let (mut mn, mut mx, mut nz, zero) = (_mm256_set1_pd(sm), _mm256_set1_pd(bg), _mm256_setzero_pd(), _mm256_setzero_pd());
    let (mut l, mut r) = (lo, hi);
    let mut vl = [_mm256_setzero_pd(); U]; let mut vr = [_mm256_setzero_pd(); U];
    for i in 0..U { vl[i] = _mm256_loadu_pd(a.add(lo + 4 * i)); vr[i] = _mm256_loadu_pd(a.add(hi - 4 * U + 4 * i)); }
    let (mut left, mut right) = (lo + 4 * U, hi - 4 * U);
    while right - left >= 4 * U {
        let mut v = [_mm256_setzero_pd(); U];
        // keep_branch: GCC compiles this choice as a predicted branch; LLVM unaided makes it a conditional move (kernel.rs)
        if r - right < left - l { crate::kernel::keep_branch(); right -= 4 * U; for i in 0..U { v[i] = _mm256_loadu_pd(a.add(right + 4 * i)); } }
        else                    { for i in 0..U { v[i] = _mm256_loadu_pd(a.add(left + 4 * i)); } left += 4 * U; }
        for i in 0..U { nz = _mm256_or_pd(nz, _mm256_cmp_pd::<_CMP_EQ_UQ>(v[i], zero)); av_part_vec(a, &mut l, &mut r, v[i], piv, 0, &mut mn, &mut mx); }
    }
    while right > left {
        let v;
        if r - right < left - l { crate::kernel::keep_branch(); right -= 4; v = _mm256_loadu_pd(a.add(right)); }
        else                    { v = _mm256_loadu_pd(a.add(left)); left += 4; }
        nz = _mm256_or_pd(nz, _mm256_cmp_pd::<_CMP_EQ_UQ>(v, zero)); av_part_vec(a, &mut l, &mut r, v, piv, 0, &mut mn, &mut mx);
    }
    for i in 0..U { nz = _mm256_or_pd(nz, _mm256_cmp_pd::<_CMP_EQ_UQ>(vl[i], zero)); av_part_vec_m(a, &mut l, &mut r, vl[i], piv, 0, &mut mn, &mut mx); }
    for i in 0..U { nz = _mm256_or_pd(nz, _mm256_cmp_pd::<_CMP_EQ_UQ>(vr[i], zero)); av_part_vec_m(a, &mut l, &mut r, vr[i], piv, 0, &mut mn, &mut mx); }
    let mut t = [0f64; 4];
    _mm256_storeu_pd(t.as_mut_ptr(), mn); for i in 0..4 { if t[i] < sm { sm = t[i]; } }
    _mm256_storeu_pd(t.as_mut_ptr(), mx); for i in 0..4 { if t[i] > bg { bg = t[i]; } }
    *smallest = sm; *biggest = bg; *flag = fs | (_mm256_movemask_pd(nz) != 0) as i32;
    l
}
/// C 1513. LEVER C: the kernel's first partition is the profile pass. A NaN is found AFTER one partition has moved data, so a
/// decline returns a PERMUTATION of the input; only callers that accept that pass sc_in = -2.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn av_qsort_fused(a: *mut f64, n: usize) -> i32 {
    let (mut sm, mut bg, p) = (f64::INFINITY, f64::NEG_INFINITY, av_pivot(a, n));
    let mut fl = 0i32;
    g_fuse_parts.fetch_add(1, Relaxed);
    let m = av_partition_fl(a, n, p, &mut sm, &mut bg, &mut fl);
    let mut nneg = 0usize;
    if fl != 0 {                                                             /* a NaN or a zero went by: tell them apart, as the scan did */
        let (mut nan, mut negz) = (0i32, 0i32);
        for k in 0..n { let x = *a.add(k); nan |= (x != x) as i32; negz |= (x == 0.0 && x.is_sign_negative()) as i32; }
        if nan != 0 { g_fuse_declines.fetch_add(1, Relaxed); return 0 }
        if negz != 0 { for k in 0..n { let x = *a.add(k); if x == 0.0 && x.is_sign_negative() { *a.add(k) = 0.0; nneg += 1; } } }
    }
    if !(sm == bg) {                                                         /* all equal: sorted. Else av_qsort_rec's top level, then the parts */
        if m == 0 { av_sort_core(a, n, 1); }                                 /* the pivot was the minimum: the ordinary recursion splits the equals off */
        else if p == bg { av_sort_core(a, m, 1); }                           /* the right part is all == pivot */
        else { av_sort_core(a, m, 1); av_sort_core(a.add(m), n - m, 1); }
    }
    if nneg != 0 { av_negz_restore(a, n, nneg); }
    1
}
/// C 1536. sc_in: -1 = no verdict: scan here, decline (on a NaN) before anything moves. -2 = none, and the caller accepts a
/// permuted array on a decline. >= 0: what av_scan_nan_negzero would return.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn avx2_qsort_f64_ps(a: *mut f64, n: usize, P: i32, sc_in: i32) -> i32 {
    if n < 2 { return 1 }
    if sc_in == -2 && P == 1 && n >= AV_FUSE_MIN_N && g_no_partfuse == 0 { return av_qsort_fused(a, n) }
    let sc = if sc_in >= 0 { sc_in } else { av_scan_nan_negzero(a, n) };
    if sc & 1 != 0 { return 0 }
    let mut nneg = 0usize;
    if sc & 2 != 0 { for i in 0..n { let x = *a.add(i); if x == 0.0 && x.is_sign_negative() { *a.add(i) = 0.0; nneg += 1; } } }
    av_sort_core(a, n, P);
    if nneg != 0 { av_negz_restore(a, n, nneg); }
    1
}
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn avx2_qsort_f64(a: *mut f64, n: usize) -> i32 { avx2_qsort_f64_ps(a, n, 1, -1) }
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn avx2_qsort_f64_sc(a: *mut f64, n: usize, sc: i32) -> i32 { avx2_qsort_f64_ps(a, n, 1, sc) }

/// `(size_t)x` for a double as GCC compiles it on x86-64 (cvttsd2si, with the 2^63 fix-up): a NaN or an out-of-range value
/// reads 0x8000000000000000, where Rust's `as` would read 0 or saturate. Only where the C casts with no guard in front.
#[inline(always)]
unsafe fn leaf_d2sz(x: f64) -> usize {
    if x >= 9223372036854775808.0 { (_mm_cvttsd_si64(_mm_set_sd(x - 9223372036854775808.0)) as u64 ^ (1u64 << 63)) as usize }
    else { _mm_cvttsd_si64(_mm_set_sd(x)) as usize }
}

// ---- C 1599: flash sort, ping-pong: scatters a -> buf and finishes IN buf. Returns buf. ---------------------------------
pub(crate) unsafe fn flash_sort(a: *mut f64, n: usize, mn: f64, mx: f64, buf: *mut f64, c: *mut ctx_t) -> *mut f64 {
    let mut m = n / FLASH_M_DIV; if m < 2 { m = 2; }
    let scale = (m - 1) as f64 / (mx - mn);
    /* The SCALE must be finite, not only the range (a bucket of subnormals: classes / range overflows to +inf). */
    if !scale.is_finite() { quicksort(a, 0, n as i64 - 1, 2 * log2i(n)); return a }
    let counts = (*c).aux;
    memset(counts, 0, (m + 1) * size_of::<u32>());
    for i in 0..n { let idx = leaf_d2sz((*a.add(i) - mn) * scale); *counts.wrapping_add(idx.wrapping_add(1)) += 1; }
    for i in 1..=m { *counts.add(i) += *counts.add(i - 1); }
    for i in 0..n { let v = *a.add(i); let idx = leaf_d2sz((v - mn) * scale); let p = counts.wrapping_add(idx); *buf.add(*p as usize) = v; *p += 1; }
    insertion_sort(buf, 0, n as i64 - 1);
    buf
}

/// C 1631, WC_SCATTER: the write-combining scatter (runtime-gated on ctx->wc_on), else a plain scatter with a prefetch.
#[inline(always)]
pub(crate) unsafe fn wc_scatter(LO: i64, HI: i64, IDX: *const u16, SRC: *const f64, buf: *mut f64, H: *mut usize, k: usize, c: *mut ctx_t) {
    if (*c).wc_on != 0 {
        let wc_ = (*c).wc; let wcn_ = (*c).wcn;
        let mut i = LO;
        while i <= HI {
            let b_ = *IDX.offset(i as isize) as usize;
            if i + PF_DIST as i64 <= HI { _mm_prefetch::<_MM_HINT_NTA>(buf.wrapping_add(*H.add(*IDX.offset((i + PF_DIST as i64) as isize) as usize)) as *const i8); }
            *wc_.add(b_ * 8 + *wcn_.add(b_) as usize) = *SRC.offset(i as isize);
            *wcn_.add(b_) += 1;
            if *wcn_.add(b_) == 8 { memcpy(buf.add(*H.add(b_)), wc_.add(b_ * 8), 64); *H.add(b_) += 8; *wcn_.add(b_) = 0; }
            i += 1;
        }
        for b_ in 0..k { if *wcn_.add(b_) != 0 { memcpy(buf.add(*H.add(b_)), wc_.add(b_ * 8), *wcn_.add(b_) as usize * size_of::<f64>()); *H.add(b_) += *wcn_.add(b_) as usize; *wcn_.add(b_) = 0; } }
    } else {
        let mut i = LO;
        while i <= HI {
            if i + PF_DIST as i64 <= HI { _mm_prefetch::<_MM_HINT_NTA>(buf.wrapping_add(*H.add(*IDX.offset((i + PF_DIST as i64) as isize) as usize)) as *const i8); }
            let h = H.add(*IDX.offset(i as isize) as usize);
            *buf.add(*h) = *SRC.offset(i as isize); *h += 1;
            i += 1;
        }
    }
}

// ---- C 1652-1670: a registered SIMD sort (logos_set_simd_sort); NULL unless a caller registers one ----------------------
pub(crate) static mut g_simd_f64: Option<unsafe fn(*mut f64, usize)> = None;
pub(crate) static mut g_simd_i32: Option<unsafe fn(*mut i32, usize)> = None;
pub(crate) static mut g_simd_user_f64: Option<unsafe fn(*mut f64, usize)> = None;
pub(crate) unsafe fn simd_f64_signed(a: *mut f64, n: usize) {
    let mut nneg = 0usize;
    for i in 0..n { let x = *a.add(i); if x == 0.0 && x.is_sign_negative() { *a.add(i) = 0.0; nneg += 1; } }
    (g_simd_user_f64.unwrap())(a, n);
    if nneg != 0 {
        let (mut lo, mut hi) = (0usize, n);
        while lo < hi { let mid = lo + (hi - lo) / 2; if *a.add(mid) < 0.0 { lo = mid + 1; } else { hi = mid; } }
        for k in 0..nneg { *a.add(lo + k) = -0.0; }
    }
}
pub(crate) unsafe fn logos_set_simd_sort(f64_: Option<unsafe fn(*mut f64, usize)>, i32_: Option<unsafe fn(*mut i32, usize)>) {
    g_simd_user_f64 = f64_; g_simd_f64 = if f64_.is_some() { Some(simd_f64_signed) } else { None }; g_simd_i32 = i32_;
}
pub(crate) static mut g_simd_bkcount: Option<unsafe fn(*const f64, i64, i64, f64, f64, i32, *mut u16, *mut usize)> = None;
pub(crate) static mut g_simd_orand: Option<unsafe fn(*const f64, usize, *mut u64, *mut u64)> = None;
pub(crate) unsafe fn logos_set_simd_kernels(bkc: Option<unsafe fn(*const f64, i64, i64, f64, f64, i32, *mut u16, *mut usize)>, oa: Option<unsafe fn(*const f64, usize, *mut u64, *mut u64)>) {
    g_simd_bkcount = bkc; g_simd_orand = oa;
}

/// C 1672. Coarse value-bucket sort, PING-PONG. Sorts src[lo..hi] using dst[lo..hi] as scratch and returns the buffer (src or
/// dst) holding the sorted range.
pub(crate) unsafe fn coarse_bucket_sort(src: *mut f64, dst: *mut f64, lo: i64, hi: i64, depth: i32, mut mn: f64, mut mx: f64, c: *mut ctx_t) -> *mut f64 {
    let len = hi - lo + 1;
    if len <= COARSE_SMALL as i64 { insertion_sort(src, lo, hi); return src }
    if depth >= COARSE_MAXD as i32 { quicksort(src, lo, hi, 2 * log2i(len as usize)); return src }
    if mn != mn { mn = *src.offset(lo as isize); mx = *src.offset(lo as isize); let mut i = lo + 1; while i <= hi { let v = *src.offset(i as isize); if v < mn { mn = v; } else if v > mx { mx = v; } i += 1; } }
    if mn == mx { return src }
    let k: usize = if (len as usize) < COARSE_K { if (len >> 1) as usize > 2 { (len >> 1) as usize } else { 2 } } else { COARSE_K };
    let (C, S, H) = ((*c).C[depth as usize], (*c).S[depth as usize], (*c).H[depth as usize]);
    let scale = (k - 1) as f64 / (mx - mn);
    /* a subnormal-wide range: the scale overflows, the minimum computes 0 * inf = NaN (see flash_sort) */
    if !scale.is_finite() { quicksort(src, lo, hi, 2 * log2i(len as usize)); return src }
    let idx = (*c).idx; let a = src; let buf = dst;
    memset(C, 0, k * size_of::<usize>());
    let mut i = lo;
    while i <= hi { let mut b = leaf_d2sz((*a.offset(i as isize) - mn) * scale); if b >= k { b = k - 1; } *idx.offset(i as isize) = b as u16; *C.add(b) += 1; i += 1; }
    *S = lo as usize;
    for i in 0..k { *S.add(i + 1) = *S.add(i) + *C.add(i); }
    memcpy(H, S, k * size_of::<usize>());
    wc_scatter(lo, hi, idx, a, buf, H, k, c);                                /* src -> dst */
    let land = (*c).land[depth as usize]; let (mut in_dst, mut in_src) = (0i64, 0i64);
    for b in 0..k {
        let blo = *S.add(b) as i64; let bhi = *S.add(b + 1) as i64 - 1; let clen = bhi - blo + 1;
        if clen <= 1 { *land.add(b) = 1; in_dst += clen; continue }
        if clen <= COARSE_SMALL as i64 { insertion_sort(dst, blo, bhi); *land.add(b) = 1; in_dst += clen; continue }
        if avx2_take(c, clen as usize) != 0 && avx2_qsort_f64(dst.offset(blo as isize), clen as usize) != 0 { *land.add(b) = 1; in_dst += clen; continue }
        let r = coarse_bucket_sort(dst, src, blo, bhi, depth + 1, f64::NAN, f64::NAN, c);   /* swapped */
        if r == dst { *land.add(b) = 1; in_dst += clen; } else { *land.add(b) = 0; in_src += clen; }
    }
    if in_dst >= in_src {
        for b in 0..k { if *land.add(b) == 0 { memcpy(dst.add(*S.add(b)), src.add(*S.add(b)), (*S.add(b + 1) - *S.add(b)) * size_of::<f64>()); } }
        return dst
    }
    for b in 0..k { if *land.add(b) != 0 { memcpy(src.add(*S.add(b)), dst.add(*S.add(b)), (*S.add(b + 1) - *S.add(b)) * size_of::<f64>()); } }
    src
}

/// C 1713. IEEE-754 total-order key of the high word.
#[inline]
pub(crate) unsafe fn hi_key(v: f64) -> u32 { let u = v.to_bits(); let h = (u >> 32) as u32; if h & 0x80000000 != 0 { !h } else { h | 0x80000000 } }

/// C 1717. Exponent-bit coarse (heavy tails), same ping-pong contract as coarse_bucket_sort.
pub(crate) unsafe fn coarse_bits_f64(src: *mut f64, dst: *mut f64, lo: i64, hi: i64, depth: i32, have_key: i32, mut mn_key: u32, mut mx_key: u32, c: *mut ctx_t) -> *mut f64 {
    let len = hi - lo + 1;
    if depth == 0 { if let Some(f) = g_simd_f64 { f(src.offset(lo as isize), len as usize); return src } }   /* SIMD leaf (heavy tails) */
    if len <= COARSE_SMALL as i64 { insertion_sort(src, lo, hi); return src }
    if depth >= COARSE_MAXD as i32 { quicksort(src, lo, hi, 2 * log2i(len as usize)); return src }
    if have_key == 0 { mn_key = hi_key(*src.offset(lo as isize)); mx_key = mn_key; let mut i = lo + 1; while i <= hi { let kk = hi_key(*src.offset(i as isize)); if kk < mn_key { mn_key = kk; } else if kk > mx_key { mx_key = kk; } i += 1; } }
    if mn_key == mx_key { return coarse_bucket_sort(src, dst, lo, hi, depth, f64::NAN, f64::NAN, c) }   /* same high word: value bucketing */
    let k: usize = if (len as usize) < COARSE_K { if (len >> 1) as usize > 2 { (len >> 1) as usize } else { 2 } } else { COARSE_K };
    let (C, S, H) = ((*c).C[depth as usize], (*c).S[depth as usize], (*c).H[depth as usize]);
    let scale = (k - 1) as f64 / (mx_key as f64 - mn_key as f64);
    let idx = (*c).idx; let a = src; let buf = dst;
    memset(C, 0, k * size_of::<usize>());
    let mut i = lo;
    while i <= hi { let mut b = ((hi_key(*a.offset(i as isize)) as f64 - mn_key as f64) * scale) as usize; if b >= k { b = k - 1; } *idx.offset(i as isize) = b as u16; *C.add(b) += 1; i += 1; }
    *S = lo as usize;
    for i in 0..k { *S.add(i + 1) = *S.add(i) + *C.add(i); }
    memcpy(H, S, k * size_of::<usize>());
    wc_scatter(lo, hi, idx, a, buf, H, k, c);
    let land = (*c).land[depth as usize]; let (mut in_dst, mut in_src) = (0i64, 0i64);
    for b in 0..k {
        let blo = *S.add(b) as i64; let bhi = *S.add(b + 1) as i64 - 1; let clen = bhi - blo + 1;
        if clen <= 1 { *land.add(b) = 1; in_dst += clen; continue }
        if clen <= COARSE_SMALL as i64 { insertion_sort(dst, blo, bhi); *land.add(b) = 1; in_dst += clen; continue }
        if avx2_take(c, clen as usize) != 0 && avx2_qsort_f64(dst.offset(blo as isize), clen as usize) != 0 { *land.add(b) = 1; in_dst += clen; continue }
        let r = coarse_bits_f64(dst, src, blo, bhi, depth + 1, 0, 0, 0, c);
        if r == dst { *land.add(b) = 1; in_dst += clen; } else { *land.add(b) = 0; in_src += clen; }
    }
    if in_dst >= in_src {
        for b in 0..k { if *land.add(b) == 0 { memcpy(dst.add(*S.add(b)), src.add(*S.add(b)), (*S.add(b + 1) - *S.add(b)) * size_of::<f64>()); } }
        return dst
    }
    for b in 0..k { if *land.add(b) != 0 { memcpy(src.add(*S.add(b)), dst.add(*S.add(b)), (*S.add(b + 1) - *S.add(b)) * size_of::<f64>()); } }
    src
}

thread_local! { static skew_probe_samp: core::cell::UnsafeCell<[f64; 8192]> = const { core::cell::UnsafeCell::new([0.0; 8192]) }; }
/// C 1754. sampled skew probe: max value-linear bin / ideal; > 8 => bits keying
pub(crate) unsafe fn skew_probe(a: *const f64, n: usize) -> f64 {
    const M: usize = 8192; const KC: usize = 256;
    let samp: *mut f64 = skew_probe_samp.with(|s| s.get() as *mut f64);
    let mut seed: u32 = 0x9e3779b9u32 ^ (n as u32);
    let (mut smn, mut smx) = (f64::INFINITY, f64::NEG_INFINITY);
    for i in 0..M { seed = seed.wrapping_mul(1664525).wrapping_add(1013904223); let v = *a.add((seed as f64 / 4294967296.0 * n as f64) as usize); *samp.add(i) = v; if v < smn { smn = v; } if v > smx { smx = v; } }
    if !(smx > smn) { return 1.0 }
    if !(smx - smn).is_finite() { return KC as f64 }   /* every finite value falls in one value-linear bin: maximal skew */
    let sc = KC as f64 / (smx - smn); let mut Hh = [0i32; KC];
    if !sc.is_finite() { return KC as f64 }            /* a subnormal-wide sample range: the same overflow, the same answer */
    for i in 0..M { let mut b = ((*samp.add(i) - smn) * sc) as i64; if b >= KC as i64 { b = KC as i64 - 1; } else if b < 0 { b = 0; } Hh[b as usize] += 1; }
    let mut mxb = 0i32; for b in 0..KC { if Hh[b] > mxb { mxb = Hh[b]; } }
    mxb as f64 / (M as f64 / KC as f64)
}

// ---- run detection + galloping merges ----------------------------------------------------------------------------------
/// C 1772. Flat [start,end,...] run list (malloc'd) or NULL on bail; records the observed average run length in
/// ctx->last_bail_avg_run. Prefix-based bail.
pub(crate) unsafe fn detect_runs_limited(a: *mut f64, n: usize, max_runs: usize, min_avg: usize, n_runs: *mut usize, c: *mut ctx_t) -> *mut usize {
    let res = malloc(2 * (max_runs + 2) * size_of::<usize>()) as *mut usize;
    let (mut R, mut i) = (0usize, 0usize);
    while i < n {
        let mut j = i + 1;
        if j < n {
            if *a.add(j) >= *a.add(i) { while j < n && *a.add(j) >= *a.add(j - 1) { j += 1; } }
            else { while j < n && *a.add(j) < *a.add(j - 1) { j += 1; }
                   let (mut l, mut r) = (i, j - 1); while l < r { let t = *a.add(l); *a.add(l) = *a.add(r); *a.add(r) = t; l += 1; r -= 1; } }
        }
        *res.add(2 * R) = i; *res.add(2 * R + 1) = j - 1; R += 1;
        if R > max_runs { (*c).last_bail_avg_run = j as f64 / R as f64; free(res); *n_runs = 0; return null_mut() }
        if min_avg != 0 && R >= 256 && j < R * min_avg { (*c).last_bail_avg_run = j as f64 / R as f64; free(res); *n_runs = 0; return null_mut() }
        i = j;
    }
    *n_runs = R; res
}

/// C 1791.
pub(crate) unsafe fn merge_gallop(a: *mut f64, buf: *mut f64, a1: usize, b1: usize, a2: usize, b2: usize) {
    if *a.add(b1) <= *a.add(a2) { return }
    const MG0: i32 = 2;
    let (mut p, mut q, mut w) = (a1, a2, a1); let (mut p_wins, mut q_wins, mut mg) = (0i32, 0i32, MG0);
    while p <= b1 && q <= b2 {
        if *a.add(p) <= *a.add(q) { *buf.add(w) = *a.add(p); w += 1; p += 1; p_wins += 1; q_wins = 0; } else { *buf.add(w) = *a.add(q); w += 1; q += 1; q_wins += 1; p_wins = 0; }
        if p_wins >= mg { let t = *a.add(q); let p0 = p; let (mut s, mut j) = (1usize, p); while j + s <= b1 && *a.add(j + s - 1) <= t { j += s; s <<= 1; }
            let mut L = j; let mut R = if b1 + 1 < j + s { b1 + 1 } else { j + s }; while L < R { let m = (L + R) >> 1; if *a.add(m) <= t { L = m + 1; } else { R = m; } }
            while p < L { *buf.add(w) = *a.add(p); w += 1; p += 1; }
            p_wins = 0; if p - p0 < MIN_GALLOP { mg += 1; } else if mg > MG0 { mg -= 1; } }
        else if q_wins >= mg { let t = *a.add(p); let q0 = q; let (mut s, mut j) = (1usize, q); while j + s <= b2 && *a.add(j + s - 1) < t { j += s; s <<= 1; }
            let mut L = j; let mut R = if b2 + 1 < j + s { b2 + 1 } else { j + s }; while L < R { let m = (L + R) >> 1; if *a.add(m) < t { L = m + 1; } else { R = m; } }
            while q < L { *buf.add(w) = *a.add(q); w += 1; q += 1; }
            q_wins = 0; if q - q0 < MIN_GALLOP { mg += 1; } else if mg > MG0 { mg -= 1; } }
    }
    while p <= b1 { *buf.add(w) = *a.add(p); w += 1; p += 1; }
    while q <= b2 { *buf.add(w) = *a.add(q); w += 1; q += 1; }
    memcpy(a.add(a1), buf.add(a1), (b2 - a1 + 1) * size_of::<f64>());
}

/// C 1811. 4-way tournament merge with galloping on the run that keeps winning
pub(crate) unsafe fn merge_4way_hybrid(a: *mut f64, buf: *mut f64, a1: usize, b1: usize, a2: usize, b2: usize, a3: usize, b3: usize, a4: usize, b4: usize) {
    if *a.add(b1) <= *a.add(a2) && *a.add(b2) <= *a.add(a3) && *a.add(b3) <= *a.add(a4) { return }
    const INF: f64 = f64::INFINITY;
    let mut p = [a1, a2, a3, a4]; let e = [b1, b2, b3, b4];
    let mut v = [*a.add(a1), *a.add(a2), *a.add(a3), *a.add(a4)];
    let mut w = a1; let mut wc = [0i32; 4];
    while p[0] <= e[0] || p[1] <= e[1] || p[2] <= e[2] || p[3] <= e[3] {
        let lv = if v[0] <= v[1] { v[0] } else { v[1] }; let lw = if v[0] <= v[1] { 0usize } else { 1 };
        let rv = if v[2] <= v[3] { v[2] } else { v[3] }; let rw = if v[2] <= v[3] { 2usize } else { 3 };
        let win = if lv <= rv { lw } else { rw }; let wv = if lv <= rv { lv } else { rv };
        *buf.add(w) = wv; w += 1;
        p[win] += 1; v[win] = if p[win] <= e[win] { *a.add(p[win]) } else { INF };
        for x in 0..4 { wc[x] = if x == win { wc[x] + 1 } else { 0 }; }
        if wc[win] >= MIN_GALLOP as i32 {
            let mut t = INF; for x in 0..4 { if x != win && v[x] < t { t = v[x]; } }
            let strict = (win != 0) as i32;                                  /* run 0 uses <=, others < (stability of ties) */
            if t == INF { while p[win] <= e[win] { *buf.add(w) = *a.add(p[win]); w += 1; p[win] += 1; } v[win] = INF; }
            else {
                let (mut s, mut j, ee) = (1usize, p[win], e[win]);
                if strict != 0 { while j + s <= ee && *a.add(j + s - 1) < t { j += s; s <<= 1; } }
                else           { while j + s <= ee && *a.add(j + s - 1) <= t { j += s; s <<= 1; } }
                let mut L = j; let mut R = if ee + 1 < j + s { ee + 1 } else { j + s };
                if strict != 0 { while L < R { let m = (L + R) >> 1; if *a.add(m) < t { L = m + 1; } else { R = m; } } }
                else           { while L < R { let m = (L + R) >> 1; if *a.add(m) <= t { L = m + 1; } else { R = m; } } }
                while p[win] < L { *buf.add(w) = *a.add(p[win]); w += 1; p[win] += 1; }
                v[win] = if p[win] <= e[win] { *a.add(p[win]) } else { INF };
            }
            wc[win] = 0;
        }
    }
    memcpy(a.add(a1), buf.add(a1), (b4 - a1 + 1) * size_of::<f64>());
}

pub(crate) const MERGE_STACK_RUNS: usize = 64;
/// C 1845. The two work lists live on the stack up to MERGE_STACK_RUNS (the C's failed-allocation fold is not ported).
pub(crate) unsafe fn merge_all_runs(a: *mut f64, buf: *mut f64, runs: *const usize, n_runs: usize) {
    let mut stackA = [0usize; 2 * (MERGE_STACK_RUNS + 4)]; let mut stackB = [0usize; 2 * (MERGE_STACK_RUNS + 4)];
    let capw = 2 * (n_runs + 4);
    let (mut A, mut B) = (stackA.as_mut_ptr(), stackB.as_mut_ptr());
    let (mut heapA, mut heapB): (*mut usize, *mut usize) = (null_mut(), null_mut());
    if n_runs > MERGE_STACK_RUNS {
        heapA = malloc(capw * size_of::<usize>()) as *mut usize; heapB = malloc(capw * size_of::<usize>()) as *mut usize;
        A = heapA; B = heapB;
    }
    memcpy(A, runs, 2 * n_runs * size_of::<usize>());
    let (mut cur, mut next, mut cur_n) = (A, B, n_runs);
    while cur_n > 1 {
        let mut next_n = 0usize;
        if cur_n >= QUADWAY_THRESHOLD {
            let mut k = 0usize;
            while k < cur_n {
                let num = if cur_n - k < 4 { cur_n - k } else { 4 }; let r = cur.add(2 * k) as *const usize;
                k += 4;
                if num == 1 { *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(1); next_n += 1; continue }
                if num == 2 { merge_gallop(a, buf, *r, *r.add(1), *r.add(2), *r.add(3)); *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(3); next_n += 1; continue }
                if num == 3 { merge_gallop(a, buf, *r, *r.add(1), *r.add(2), *r.add(3)); merge_gallop(a, buf, *r, *r.add(3), *r.add(4), *r.add(5));
                              *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(5); next_n += 1; continue }
                merge_4way_hybrid(a, buf, *r, *r.add(1), *r.add(2), *r.add(3), *r.add(4), *r.add(5), *r.add(6), *r.add(7));
                *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(7); next_n += 1;
            }
        } else {
            let mut k = 0usize;
            while k < cur_n {
                let r = cur.add(2 * k) as *const usize;
                let last = k + 1 >= cur_n;
                k += 2;
                if last { *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(1); next_n += 1; continue }
                merge_gallop(a, buf, *r, *r.add(1), *r.add(2), *r.add(3));
                *next.add(2 * next_n) = *r; *next.add(2 * next_n + 1) = *r.add(3); next_n += 1;
            }
        }
        let t = cur; cur = next; next = t; cur_n = next_n;
    }
    free(heapA); free(heapB);
}

// ---- C 1903-1925: the counting routes' thread caps -----------------------------------------------------------------------
pub(crate) const COUNT_MIN_PER_THREAD: usize = 350000;
pub(crate) const COUNT_MIN_THREADS: usize = 3;
pub(crate) const COUNT_MAX_THREADS: i32 = 0;
pub(crate) const g_count_max: i32 = COUNT_MAX_THREADS;                       // LOGOS_COUNT_MAX unset
#[inline]
pub(crate) unsafe fn count_threads(n: usize, P: i32) -> i32 { let mut pm = n / COUNT_MIN_PER_THREAD; if pm < COUNT_MIN_THREADS { pm = COUNT_MIN_THREADS; } if P as usize > pm { pm as i32 } else { P } }
#[inline]
pub(crate) unsafe fn count_threads_hash(n: usize, P: i32) -> i32 { let mut r = count_threads(n, P); if g_count_max > 0 && r > g_count_max { r = g_count_max; } r }

// ---- C 1935-1971: the relocation family's constants and switches (rdesc_t is the prelude's; RUN_TWIST on is knot.rs's) ----
pub(crate) const RELOC_MIN_N: usize = 4096;
pub(crate) const RELOC_MIN_AVG_RUN_SEQ: usize = 256;
pub(crate) const RELOC_MIN_AVG_RUN_PAR: usize = MIN_AVG_RUN_FOR_MERGE;
pub(crate) static mut g_reloc_avg: usize = RELOC_MIN_AVG_RUN_SEQ;            /* set on the calling thread where relocation starts */
pub(crate) const g_reloc_nocopy: i32 = 0;                                    // LOGOS_RELOC_NOCOPY unset
pub(crate) const g_stray_on: i32 = 1;                                        // LOGOS_NO_STRAY / LOGOS_KNOT_LIST unset
pub(crate) const g_stray_par: i32 = 1;                                       // LOGOS_STRAY_SEQ unset
pub(crate) const STRAY_DIV_PAR: usize = 64;
pub(crate) const RELOC_CHECK_MIN_SEEN: usize = 4096;
pub(crate) const g_stray_div: usize = 16;                                    // LOGOS_STRAY_DIV unset
pub(crate) const g_onerun_off: i32 = 0;                                      // LOGOS_NO_ONERUN unset
pub(crate) const g_onerun_copy: i32 = 0;                                     // LOGOS_ONERUN_COPY unset
pub(crate) static mut g_onerun_count: usize = 0;                             /* logos_twist_last(12): sorts finished as one strand */
pub(crate) const RELOC_MIN_PER_THREAD: usize = 500000;

// ---- C 2739: quantized-float radix (typed path): the probe; radix_f64_full itself is behind g_quant_radix and not ported --
#[inline]
pub(crate) unsafe fn key_parts(v: f64, tl: *mut u32, th: *mut u32) {
    let u = v.to_bits(); let lo = u as u32; let hi = (u >> 32) as u32;
    if hi >> 31 != 0 { *tl = !lo; *th = !hi; } else { *tl = lo; *th = hi | 0x80000000; }
}
pub(crate) const g_quant_strided: i32 = 0;                                   // LOGOS_QUANT_STRIDED unset
pub(crate) const g_quant_radix: i32 = 0;                                     // LOGOS_QUANT_RADIX unset
/// C 2755.
pub(crate) unsafe fn quant_probe_f64(a: *const f64, n: usize) -> i32 {
    let m = if n < 4096 { n } else { 4096 }; let step = n / m;
    let (mut or_h, mut and_h, mut or_l, mut and_l) = (0u32, 0xFFFFFFFFu32, 0u32, 0xFFFFFFFFu32);
    if g_quant_strided != 0 { let mut p = 0usize; for i in 0..m { let (mut tl, mut th) = (0u32, 0u32); key_parts(*a.add(p), &mut tl, &mut th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; p += step; } }
    else { for i in 0..m { let p = SAMPLE_POS(i, m, n); let (mut tl, mut th) = (0u32, 0u32); key_parts(*a.add(if p < n { p } else { n - 1 }), &mut tl, &mut th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; } }
    let mut passes = 0i32;
    for p in 0..8 { let hi_word = p >= 4; let sh = (p & 3) << 3; let o = if hi_word { or_h } else { or_l }; let an = if hi_word { and_h } else { and_l }; if ((o >> sh) & 0xFF) != ((an >> sh) & 0xFF) { passes += 1; } }
    passes
}

/// C 2792. flags: 1 all-integer, 2 ascending, 4 descending, in the window
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct sf_pre_t { pub(crate) mn: f64, pub(crate) mx: f64, pub(crate) settled: i32, pub(crate) flags: i32 }
/// C 2793. sort_fn's scalar loop on the first k elements: settled = it cleared all-integer, ascending AND descending.
#[inline(never)]
pub(crate) unsafe fn sf_prefix(a: *const f64, k: usize) -> sf_pre_t {
    let (mut mn, mut mx) = (*a, *a);
    let mut all_int = ((*a).is_finite() && *a == (*a).floor() && !(*a == 0.0 && (*a).is_sign_negative())) as i32;
    let (mut asc, mut desc) = (1i32, 1i32);
    for i in 1..k {
        let v = *a.add(i);
        if v < mn { mn = v; } else if v > mx { mx = v; }
        if all_int != 0 && !(v.is_finite() && v == v.floor() && !(v == 0.0 && v.is_sign_negative())) { all_int = 0; }
        if asc != 0 && v < *a.add(i - 1) { asc = 0; }
        if desc != 0 && v > *a.add(i - 1) { desc = 0; }
    }
    sf_pre_t { mn, mx, settled: (all_int == 0 && asc == 0 && desc == 0) as i32, flags: all_int | asc << 1 | desc << 2 }
}

// ---- C 2814: the dispatcher (port of sort_fn) ----------------------------------------------------------------------------
/// Returns the buffer holding the sorted result: a (most paths) or buf (ping-pong paths).
pub(crate) unsafe fn sort_fn(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 {
    if n < 2 { return a }
    let (mut mn, mut mx) = (*a, *a);
    /* A -0.0 is not an integer here: the counting and int32 radix paths REBUILD values from integers. */
    let mut all_int = ((*a).is_finite() && *a == (*a).floor() && !(*a == 0.0 && (*a).is_sign_negative())) as i32;
    let (mut asc, mut desc, mut sc, mut deferred, mut settled) = (1i32, 1i32, -1i32, 0i32, 0i32);   /* sc: the AVX2 kernel's NaN / -0.0 verdict, -1 = unknown */
    if n >= SF_VEC_MIN_N && g_no_frontfuse == 0 && avx2_leaf_on() != 0 {
        let mut pre = sf_prefix(a, SF_PREFIX);
        /* THE FOUNDER EFFECT: three more windows at n/4, n/2 and 3n/4 are asked and their flags ANDed. */
        if pre.settled == 0 && n >= 4 * SF_PREFIX {
            let mut fl = pre.flags; let (mut wmn, mut wmx) = (pre.mn, pre.mx);
            let mut w = 1i32;
            while w <= 3 && fl != 0 {
                let q = sf_prefix(a.add((n / 4) * (w as usize) - SF_PREFIX / 2), SF_PREFIX);
                fl &= q.flags; wmn = if q.mn < wmn { q.mn } else { wmn }; wmx = if q.mx > wmx { q.mx } else { wmx };
                w += 1;
            }
            if fl == 0 { pre.settled = 1; pre.mn = wmn; pre.mx = wmx; g_sf_windows.fetch_add(1, Relaxed); }   // LOGOS_SF_FRONT_ONLY unset
        }
        if pre.settled != 0 {
            settled = 1; all_int = 0; asc = 0; desc = 0; mn = pre.mn; mx = pre.mx;
            if n > AVX2_FLASH_MAX_N && n >= AV_FUSE_MIN_N && (*c).seq != 0 && g_no_partfuse == 0 && mx > mn { deferred = 1; }
            else { let pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; }
        }
    }
    if settled == 0 {
        for i in 1..n {
            let v = *a.add(i);
            if v < mn { mn = v; } else if v > mx { mx = v; }
            if all_int != 0 && !(v.is_finite() && v == v.floor() && !(v == 0.0 && v.is_sign_negative())) { all_int = 0; }
            if asc != 0 && v < *a.add(i - 1) { asc = 0; }
            if desc != 0 && v > *a.add(i - 1) { desc = 0; }
        }
    }
    let all_int32 = (all_int != 0 && mn >= -2147483648.0 && mx <= 2147483647.0) as i32;
    if asc != 0 { return a }
    if desc != 0 { let (mut l, mut r) = (0usize, n - 1); while l < r { let t = *a.add(l); *a.add(l) = *a.add(r); *a.add(r) = t; l += 1; r -= 1; } return a }
    if all_int != 0 {
        let span = mx - mn + 1.0;
        if span <= COUNTING_SORT_K as f64 * n as f64 {
            /* the context's aux holds n counters; a span above n (COUNTING_SORT_K > 1) needs its own array */
            let sp = span as usize; let B: *mut u32 = if sp <= n { (*c).aux } else { malloc(sp * size_of::<u32>()) as *mut u32 };
            memset(B, 0, sp * size_of::<u32>());
            for i in 0..n { *B.add((*a.add(i) - mn) as usize) += 1; }
            let mut w = 0usize;
            for v in 0..sp { let mut cnt = *B.add(v); let val = v as f64 + mn; while cnt > 0 { cnt -= 1; *a.add(w) = val; w += 1; } }
            if B != (*c).aux { free(B); }
            return a
        }
        if all_int32 != 0 && n >= 64 { if let Some(f) = g_simd_f64 { f(a, n); return a }
            lsd_radix_int32(a, n, buf, c); return a }
    }
    if n <= INSERTION_SORT_THRESHOLD {
        if n >= 4 && n <= 16 && g_no_smallnet == 0 { net_sort_small(a, n as i64); } else { insertion_sort(a, 0, n as i64 - 1); }
        return a }

    let mut run_cap = n / MIN_AVG_RUN_FOR_MERGE; if run_cap < MAX_RUNS_FOR_MERGE { run_cap = MAX_RUNS_FOR_MERGE; }
    let mut n_runs = 0usize; let runs = detect_runs_limited(a, n, run_cap, MIN_AVG_RUN_FOR_MERGE, &mut n_runs, c);
    if !runs.is_null() {
        if n_runs > 1 {
            let r = runs_relocate_f64(a, n, buf, runs, n_runs);              /* J5: disjoint runs -> memcpy into buf, no merge */
            if !r.is_null() { free(runs); return r }
            if n_runs <= MERGE_MAX_RUNS { if merge_runs_vec(a, buf, runs, n_runs) == 0 { merge_all_runs(a, buf, runs, n_runs); } free(runs); return a }
        }
        free(runs);   /* too many overlapping runs for a k-way merge: the leaf below is cheaper */
    }

    let sample_size = if n < 40 { n } else { 40 }; let mut sample_step = n / sample_size; if sample_step < 1 { sample_step = 1; }
    let (mut inv, mut comps) = (0usize, 0usize);
    { let mut i = 0usize; while i + sample_step < n { if *a.add(i) > *a.add(i + sample_step) { inv += 1; } comps += 1; i += sample_step; } }
    let nearly = (comps > 0 && inv as f64 / comps as f64 <= NEARLY_SORTED_INV_RATIO) as i32; (*c).nearly = nearly;
    /* 64 strided pairs EIGHT apart tell dense short-reach disorder from near-sortedness. */
    let mut dense = 0i32;
    if nearly != 0 && n >= 1024 && g_no_dense_probe == 0 { let st = n / 64; let mut d = 0usize; let mut i = 0usize; while i + 8 < n { d += (*a.add(i) > *a.add(i + 8)) as usize; i += st; } dense = (d >= 16) as i32; }
    if n >= 65536 { TRACE!("leaf probe: n={} inv={}/{} nearly={} dense={} -> policy nearly={}", n, inv, comps, nearly, dense, (*c).nearly); }
    if nearly != 0 && dense == 0 {
        /* The momentum variant has an 8n move budget and falls through when it is exceeded. */
        if n <= MOMENTUM_THRESHOLD { if insertion_sort_momentum(a, 0, n as i64 - 1) != 0 { return a } }
        else if (*c).last_bail_avg_run > 0.0 && (*c).last_bail_avg_run <= MOMENTUM_MAX_AVG_RUN as f64 {
            if insertion_sort_momentum(a, 0, n as i64 - 1) != 0 { return a }
            /* aborted: not actually near-sorted; fall through */
        }
    }
    /* Low-discrepancy probe (order from chaos, M3): two contiguous windows of 64, the three-gap test and the coverage band. */
    (*c).lowdisc = 0;
    if n >= FLASH_SORT_THRESHOLD && mx > mn {
        let mut ld = 1i32;
        let mut w = 0i32;
        while w < 2 && ld != 0 {
            let mut win = [0f64; 64]; let base = (n / 3) * ((w + 1) as usize) - 32;
            for i in 0..64 { win[i] = *a.add(base + i); }
            insertion_sort(win.as_mut_ptr(), 0, 63);
            let (mut gmin, mut gmax) = (f64::INFINITY, 0f64);
            for i in 1..64 { let g = win[i] - win[i - 1]; if g < gmin { gmin = g; } if g > gmax { gmax = g; } }
            if !(gmin > 0.0) || gmax > LOWDISC_GAP_RATIO * gmin { ld = 0; }
            else {
                /* the coverage test needs the RANGE, and a deferred profile has only the windows' extremes */
                if deferred != 0 { let pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; deferred = 0; }
                let span = win[63] - win[0]; let rng = mx - mn;
                if span < LOWDISC_MIN_COVER * rng && span >= LOWDISC_LONG_COVER * rng { ld = 0; }   /* a SHORT ramp: the kernel (LOGOS_LOWDISC_LOCAL unset) */
            }
            w += 1;
        }
        (*c).lowdisc = ld;
        if ld != 0 { g_lowdisc_fires.fetch_add(1, Relaxed); }
    }
    /* AVX2 leaf, taken here only where FLASH SORT IS NOT AVAILABLE or the window's local-order probe says unordered. */
    if n >= 32 && mx > mn && (n < FLASH_SORT_THRESHOLD || n > AVX2_FLASH_MAX_N || g_avx2_over_flash != 0 || (g_band_flash == 0 && band_local_order(a, n) == 0))
        && avx2_take(c, n) != 0 && avx2_qsort_f64_sc(a, n, if deferred != 0 { -2 } else { sc }) != 0 { return a }
    if deferred != 0 { let pr = sf_profile_avx2(a, n, mn, mx); mn = pr.mn; mx = pr.mx; sc = pr.sc; deferred = 0; }   /* not taken, or a NaN declined it: the pass, late */
    if n >= FLASH_SORT_THRESHOLD && mx > mn && !(mx - mn).is_finite() {
        /* An infinite range: the exponent-bit coarse path keys on the bit pattern; it needs a NaN-free array. */
        let mut inf_nan = 0i32; for i in 0..n { if *a.add(i) != *a.add(i) { inf_nan = 1; break } }
        if inf_nan == 0 && n >= TYPED_MIN_N && ctx_coarse_ready(c) != 0 { return coarse_bits_f64(a, buf, 0, n as i64 - 1, 0, 0, 0, 0, c) }
        quicksort(a, 0, n as i64 - 1, 2 * log2i(n));
        return a
    }
    if n >= FLASH_SORT_THRESHOLD && mx > mn {
        if let Some(f) = g_simd_f64 { f(a, n); return a }                    /* SIMD leaf for large unstructured data */
        const SS: usize = 64;
        let mut samples = [0f64; SS]; let s_stride = n / SS;
        for i in 0..SS { samples[i] = *a.add(i * s_stride); }
        insertion_sort(samples.as_mut_ptr(), 0, SS as i64 - 1);
        let mut max_gap = 0f64; let mut max_gap_idx = -1i32;
        for i in 1..SS { let g = samples[i] - samples[i - 1]; if g > max_gap { max_gap = g; max_gap_idx = i as i32; } }
        let mut gaps = [0f64; SS - 1]; for i in 0..SS - 1 { gaps[i] = samples[i + 1] - samples[i]; }
        insertion_sort(gaps.as_mut_ptr(), 0, SS as i64 - 2);
        let median_gap = gaps[(SS - 1) >> 1]; let range = mx - mn; let bucket_width = range / n as f64;
        let mut has_tight = 0i32; { let mut i = 0usize; while i + 6 < SS { if samples[i + 6] - samples[i] < bucket_width { has_tight = 1; break } i += 1; } }
        let is_multimodal = median_gap > 0.0 && max_gap >= 10.0 * median_gap && max_gap >= 0.2 * range && has_tight != 0 && max_gap_idx >= 4 && max_gap_idx <= SS as i32 - 4;
        if is_multimodal {
            let pivot = (samples[(max_gap_idx - 1) as usize] + samples[max_gap_idx as usize]) / 2.0;
            let (mut i, mut j) = (0i64, n as i64 - 1);
            /* ONE predicate, x < pivot, for both scans: a NaN goes right. */
            while i <= j { while i <= j && *a.offset(i as isize) < pivot { i += 1; } while i <= j && !(*a.offset(j as isize) < pivot) { j -= 1; } if i < j { let t = *a.offset(i as isize); *a.offset(i as isize) = *a.offset(j as isize); *a.offset(j as isize) = t; i += 1; j -= 1; } }
            if i > 1 { let r = sort_fn(a, i as usize, buf, c); if r != a { memcpy(a, buf, i as usize * size_of::<f64>()); } }
            if (i as usize) < n { let r = sort_fn(a.offset(i as isize), n - i as usize, buf, c); if r != a.offset(i as isize) { memcpy(a.offset(i as isize), buf, (n - i as usize) * size_of::<f64>()); } }
            return a
        }
        if n < COARSE_MIN_N && (samples[47] - samples[16]) / range >= FLASH_SAFETY_RATIO { return flash_sort(a, n, mn, mx, buf, c) }
        /* Flash declined this range, so the alternative is coarse -- and against coarse the AVX2 quicksort wins. */
        if g_avx2_over_flash == 0 && avx2_take(c, n) != 0 && avx2_qsort_f64_sc(a, n, sc) != 0 { return a }
        if n >= TYPED_MIN_N && skew_probe(a, n) > SKEW_PROBE_MAX as f64 {
            let mut has_nan = 0i32; for i in 0..n { if *a.add(i) != *a.add(i) { has_nan = 1; break } }
            if has_nan == 0 && ctx_coarse_ready(c) != 0 { return coarse_bits_f64(a, buf, 0, n as i64 - 1, 0, 0, 0, 0, c) }
        }
        if ctx_coarse_ready(c) != 0 { return coarse_bucket_sort(a, buf, 0, n as i64 - 1, 0, mn, mx, c) }
    }
    quicksort(a, 0, n as i64 - 1, 2 * log2i(n));
    a
}

// ---- C 3062: FLOAT32 NARROWING -------------------------------------------------------------------------------------------
pub(crate) const F32_MIN_N: usize = 65536;
pub(crate) const g_no_f32: i32 = 0;                                          // LOGOS_NO_F32 unset
pub(crate) static g_f32_sorts: AtomicUsize = AtomicUsize::new(0);            /* logos_twist_last(22): sorts the float32 path finished */
#[inline]
pub(crate) unsafe fn f32_key(f: f32) -> u32 { let u = f.to_bits(); if u >> 31 != 0 { !u } else { u | 0x80000000 } }
#[inline]
pub(crate) unsafe fn f32_unkey(k: u32) -> f64 { let u = if k >> 31 != 0 { k & 0x7FFFFFFF } else { !k }; f32::from_bits(u) as f64 }
/// C 3077. The keys are narrowed IN PLACE (key i written over the bytes of a[i / 2], which was read already) and the passes
/// ping-pong between the input's own storage and buf. A decline hands back a[] as it came.
pub(crate) unsafe fn f32_radix_try(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> i32 {
    let twobuf = false;                                                      // LOGOS_F32_TWOBUF unset
    let src: *mut u32 = if twobuf { (*c).aux } else { a as *mut u32 }; let dst: *mut u32 = buf as *mut u32;
    let (mut mn, mut mx) = (0xFFFFFFFFu32, 0u32);
    for i in 0..n {
        let x = *a.add(i); let f = x as f32;
        if f as f64 != x {                                                   /* not a float, or a NaN: decline with a[] as it came */
            if !twobuf { let mut k = i; while k > 0 { k -= 1; *a.add(k) = f32_unkey(*src.add(k)); } }
            return 0
        }
        let k = f32_key(f); *src.add(i) = k; if k < mn { mn = k; } if k > mx { mx = k; }
    }
    let range = mx.wrapping_sub(mn);
    if range == 0 {                                                          /* one value (both zeros would differ in key): sorted */
        if !twobuf { let mut k = n; while k > 0 { k -= 1; *a.add(k) = f32_unkey(*src.add(k)); } }
        g_f32_sorts.fetch_add(1, Relaxed); return 1
    }
    let bits = 32 - range.leading_zeros() as i32;
    let np: i32 = if bits <= 11 { 1 } else if bits <= 22 { 2 } else { 3 }; let w = (bits + np - 1) / np; let mask: u32 = (1u32 << w) - 1; let nc = 1usize << w;
    let cnt = calloc(np as usize * (nc + 1), size_of::<u32>()) as *mut u32;
    let (c0, c1, c2) = (cnt, cnt.add(nc + 1), cnt.wrapping_add(2 * (nc + 1)));
    let h: *mut u32 = if !twobuf && (np & 1) != 0 { dst } else { src };       /* where the biased keys go: the last pass must not read a[] */
    if np == 1      { for i in 0..n { let k = (*src.add(i)).wrapping_sub(mn); *h.add(i) = k; *c0.add(k as usize + 1) += 1; } }
    else if np == 2 { for i in 0..n { let k = (*src.add(i)).wrapping_sub(mn); *h.add(i) = k; *c0.add((k & mask) as usize + 1) += 1; *c1.add((k >> w) as usize + 1) += 1; } }
    else            { for i in 0..n { let k = (*src.add(i)).wrapping_sub(mn); *h.add(i) = k; *c0.add((k & mask) as usize + 1) += 1; *c1.add(((k >> w) & mask) as usize + 1) += 1; *c2.add((k >> (2 * w)) as usize + 1) += 1; } }
    for p in 0..np { let cc = cnt.add(p as usize * (nc + 1)); for b in 1..=nc { *cc.add(b) += *cc.add(b - 1); } }
    let mut from = h; let mut to = if h == dst { src } else { dst };
    for p in 0..np {
        let cc = cnt.add(p as usize * (nc + 1)); let sh = p * w;
        if p == np - 1 { for i in 0..n { let k = *from.add(i); let s = cc.add(((k >> sh) & mask) as usize); *a.add(*s as usize) = f32_unkey(k.wrapping_add(mn)); *s += 1; } }
        else { for i in 0..n { if i + PF_DIST < n { _mm_prefetch::<_MM_HINT_NTA>(to.wrapping_add(*cc.add(((*from.add(i + PF_DIST) >> sh) & mask) as usize) as usize) as *const i8); }
                   let k = *from.add(i); let s = cc.add(((k >> sh) & mask) as usize); *to.add(*s as usize) = k; *s += 1; }
               let tmp = from; from = to; to = tmp; }
    }
    free(cnt);
    g_f32_sorts.fetch_add(1, Relaxed);
    1
}
/// C 3115. typed-double entry (port of sort_fn.f64); returns the buffer holding the result
pub(crate) unsafe fn leaf_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 {
    if n < TYPED_MIN_N { return sort_fn(a, n, buf, c) }
    if g_quant_radix != 0 && n <= RADIX64_MAX_N && quant_probe_f64(a, n) <= QUANT_PROBE_MAX {
        TRACE!("leaf_f64: n={} quantized -> float radix", n);
        unreachable!("radix_f64_full is behind g_quant_radix (LOGOS_QUANT_RADIX) and is not ported");
    }
    if (*c).seq != 0 && n >= F32_MIN_N && g_no_f32 == 0 && f32_radix_try(a, n, buf, c) != 0 { return a }   /* every value a float: 4-byte keys */
    if skew_probe(a, n) <= SKEW_PROBE_MAX as f64 { return sort_fn(a, n, buf, c) }
    TRACE!("leaf_f64: n={} skewed (probe {:.1}) -> the skewed branch", n, skew_probe(a, n));
    /* Where the kernel is going to run, its scan answers this branch's "is there a NaN?" too. */
    if g_no_frontfuse == 0 && avx2_take(c, n) != 0 {
        if g_no_partfuse == 0 {                                              /* lever C: the kernel's first partition asks it (av_qsort_fused) */
            if avx2_qsort_f64_sc(a, n, -2) != 0 { return a }
            return sort_fn(a, n, buf, c)                                     /* declined: a NaN, met in that partition; the same multiset, permuted */
        }
        let sc = av_scan_nan_negzero(a, n);
        if sc & 1 != 0 { return sort_fn(a, n, buf, c) }
        if avx2_qsort_f64_sc(a, n, sc) != 0 { return a }
    }
    let mut has_nan = 0i32; for i in 0..n { if *a.add(i) != *a.add(i) { has_nan = 1; break } }
    if has_nan != 0 { return sort_fn(a, n, buf, c) }
    if avx2_take(c, n) != 0 && avx2_qsort_f64(a, n) != 0 { return a }
    if ctx_coarse_ready(c) != 0 { return coarse_bits_f64(a, buf, 0, n as i64 - 1, 0, 0, 0, 0, c) }
    quicksort(a, 0, n as i64 - 1, 2 * log2i(n)); a                           /* no memory for the tables */
}

// ---- tests ---------------------------------------------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
        fn below(&mut self, k: usize) -> usize { (self.next() % k as u64) as usize }
    }
    fn bits_sorted(v: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = v.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    /// ascending under `<` (NaN-free data) and a permutation of the input's bit patterns
    fn check(name: &str, input: &[f64], out: &[f64]) {
        assert_eq!(input.len(), out.len(), "{name}: length");
        for i in 1..out.len() { assert!(!(out[i] < out[i - 1]), "{name}: descent at {i}: {} then {}", out[i - 1], out[i]); }
        assert!(bits_sorted(input) == bits_sorted(out), "{name}: not a permutation of the input's bit patterns");
    }
    unsafe fn mk_ctx(cap: usize, seq: i32, wc_on: i32) -> ctx_t {
        let mut c: ctx_t = core::mem::zeroed();
        c.cap = cap; c.pool_ix = -1; c.seq = seq; c.wc_on = wc_on;
        c.aux = malloc((cap + 1) * size_of::<u32>()) as *mut u32;
        c.idx = malloc((if cap != 0 { cap } else { 1 }) * size_of::<u16>()) as *mut u16;
        if wc_on != 0 { c.wc = malloc(COARSE_K * 8 * size_of::<f64>()) as *mut f64; c.wcn = calloc(COARSE_K, 1); }
        c
    }
    unsafe fn rm_ctx(c: &mut ctx_t) {
        free(c.aux); free(c.idx); free(c.wc); free(c.wcn);
        for d in 0..COARSE_MAXD { free(c.C[d]); free(c.S[d]); free(c.H[d]); free(c.land[d]); }
    }
    fn avx2() -> bool { unsafe { avx2_leaf_on() != 0 } }

    /// the shapes: NaN-free, with duplicates, signed zeros and infinities among them
    fn shapes(n: usize, seed: u64) -> Vec<(&'static str, Vec<f64>)> {
        let mut r = Rng(seed | 1);
        let mut v: Vec<(&'static str, Vec<f64>)> = Vec::new();
        v.push(("uniform", (0..n).map(|_| r.unit()).collect()));
        v.push(("uniform_signed", (0..n).map(|_| r.unit() * 2e6 - 1e6).collect()));
        v.push(("heavy_tail", (0..n).map(|_| { let u = r.unit().max(1e-300); let s = if r.next() & 1 == 0 { 1.0 } else { -1.0 }; s * (1.0 / u).powf(1.5) }).collect()));
        v.push(("lognormal", (0..n).map(|_| ((r.unit() + r.unit() + r.unit() + r.unit() - 2.0) * 6.0).exp()).collect()));
        v.push(("bimodal", (0..n).map(|_| if r.next() & 1 == 0 { r.unit() } else { 1e9 + r.unit() }).collect()));
        v.push(("clustered", (0..n).map(|_| (r.below(16) as f64) * 1e6 + r.unit() * 1e-3).collect()));
        v.push(("few_distinct", (0..n).map(|_| r.below(5) as f64 * 1.5).collect()));
        v.push(("dups_frac", (0..n).map(|_| r.below(1000) as f64 * 0.37).collect()));
        v.push(("int_small", (0..n).map(|_| r.below(100) as f64 - 50.0).collect()));
        v.push(("int_wide", (0..n).map(|_| (r.next() as u32 as i32) as f64).collect()));
        v.push(("int_big", (0..n).map(|_| (r.next() >> 11) as f64).collect()));
        v.push(("sorted", (0..n).map(|i| i as f64 + 0.5).collect()));
        v.push(("reversed", (0..n).map(|i| (n - i) as f64 + 0.5).collect()));
        v.push(("near_sorted", { let mut x: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect(); for _ in 0..n / 100 { let (i, j) = (r.below(n), r.below(n)); x.swap(i, j); } x }));
        v.push(("kinks", { let mut x: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect(); let mut i = 3; while i + 1 < n { x.swap(i, i + 1); i += 97; } x }));
        v.push(("jitter", (0..n).map(|i| i as f64 + r.unit() * 40.0).collect()));
        v.push(("blocks", { let nb = 8usize; let bl = (n / nb).max(1); (0..n).map(|i| { let b = (i / bl * 5 + 3) % nb.max(1); b as f64 * 1e7 + (i % bl) as f64 + 0.25 }).collect() }));
        v.push(("runs4", (0..n).map(|i| { let q = (n / 4).max(1); (i % q) as f64 * 4.0 + (i / q) as f64 + 0.5 }).collect()));
        v.push(("sawtooth", (0..n).map(|i| (i % 37) as f64 + 0.5).collect()));
        v.push(("weyl", (0..n).map(|i| { let f = i as f64 * 0.6180339887498949; f - f.floor() }).collect()));
        v.push(("signed_zeros", (0..n).map(|_| match r.below(4) { 0 => -0.0, 1 => 0.0, 2 => r.unit() - 0.5, _ => -1.0 }).collect()));
        v.push(("zeros_in_noise", (0..n).map(|_| match r.below(50) { 0 => -0.0, 1 => 0.0, _ => r.unit() * 2.0 - 1.0 }).collect()));
        v.push(("infinities", (0..n).map(|_| match r.below(40) { 0 => f64::INFINITY, 1 => f64::NEG_INFINITY, _ => r.unit() * 2e3 - 1e3 }).collect()));
        v.push(("inf_first", { let mut x: Vec<f64> = (0..n).map(|_| r.unit()).collect(); x[0] = f64::NEG_INFINITY; x }));
        v.push(("subnormals", (0..n).map(|_| f64::from_bits(r.next() >> 14)).collect()));
        v.push(("huge_range", (0..n).map(|_| if r.next() & 1 == 0 { 1.7e308 * r.unit() } else { -1.7e308 * r.unit() }).collect()));
        v.push(("floats", (0..n).map(|_| (r.unit() * 2000.0 - 1000.0) as f32 as f64).collect()));
        v.push(("constant", vec![3.25; n]));
        v
    }

    #[test]
    fn scans() { let _port_guard = crate::port::port_test_lock();
        if !avx2() { return }
        unsafe {
            let mut r = Rng(7);
            for &n in &[0usize, 1, 3, 4, 5, 17, 64, 1000, 4099] {
                let base: Vec<f64> = (0..n).map(|_| r.unit() + 0.25).collect();
                assert_eq!(av_has_nan(base.as_ptr(), n), 0);
                assert_eq!(av_scan_nan_negzero(base.as_ptr(), n), 0);
                if n == 0 { continue }
                for &pos in &[0usize, n / 2, n - 1] {
                    let mut x = base.clone(); x[pos] = f64::NAN;
                    assert_eq!(av_has_nan(x.as_ptr(), n), 1);
                    assert_eq!(av_scan_nan_negzero(x.as_ptr(), n), 1);
                    x[pos] = -0.0; assert_eq!(av_scan_nan_negzero(x.as_ptr(), n), 2);
                    assert_eq!(av_has_nan(x.as_ptr(), n), 0);
                    x[pos] = 0.0; assert_eq!(av_scan_nan_negzero(x.as_ptr(), n), 0);
                    if n >= 2 { x[pos] = -0.0; x[(pos + 1) % n] = f64::NAN; assert_eq!(av_scan_nan_negzero(x.as_ptr(), n), 3); }
                }
                // the profile pass: min, max, the flag
                let (mut mn, mut mx) = (base[0], base[0]);
                for &v in &base { if v < mn { mn = v; } if v > mx { mx = v; } }
                let pr = sf_profile_avx2(base.as_ptr(), n, base[0], base[0]);
                assert_eq!((pr.mn, pr.mx, pr.sc), (mn, mx, 0));
                let mut x = base.clone(); x[n - 1] = 0.0;
                assert_eq!(sf_profile_avx2(x.as_ptr(), n, x[0], x[0]).sc, -1);
                x[n - 1] = f64::NAN;
                let pr = sf_profile_avx2(x.as_ptr(), n, x[0], x[0]);
                assert_eq!(pr.sc, -1);
                if n > 1 { let (mut m2, mut x2) = (x[0], x[0]); for &v in &x[..n - 1] { if v < m2 { m2 = v; } if v > x2 { x2 = v; } } assert_eq!((pr.mn, pr.mx), (m2, x2)); }
            }
            // sf_prefix
            let asc: Vec<f64> = (0..64).map(|i| i as f64).collect();
            let p = sf_prefix(asc.as_ptr(), 64); assert_eq!((p.settled, p.flags, p.mn, p.mx), (0, 3, 0.0, 63.0));
            let desc: Vec<f64> = (0..64).map(|i| 64.5 - i as f64).collect();
            let p = sf_prefix(desc.as_ptr(), 64); assert_eq!((p.settled, p.flags), (0, 4));
            let mix: Vec<f64> = (0..64).map(|i| ((i * 37) % 64) as f64 + 0.5).collect();
            let p = sf_prefix(mix.as_ptr(), 64); assert_eq!((p.settled, p.flags, p.mn, p.mx), (1, 0, 0.5, 63.5));
            let negz = [-0.0f64, 1.0, 2.0]; assert_eq!(sf_prefix(negz.as_ptr(), 3).flags & 1, 0);
        }
    }

    #[test]
    fn kernel_entries() { let _port_guard = crate::port::port_test_lock();
        if !avx2() { return }
        unsafe {
            for &n in &[0usize, 1, 2, 5, 33, 64, 65, 200, 511, 512, 513, 4096, 70001, 300000] {
                for (name, v) in shapes(n.max(1), 0x51 + n as u64) { let v = v[..n].to_vec();
                    for &sc_in in &[-1i32, -2] {
                        let mut x = v.clone();
                        assert_eq!(avx2_qsort_f64_sc(x.as_mut_ptr(), n, sc_in), 1, "{name} n={n}");
                        check(&format!("avx2_qsort_f64_sc({sc_in}) {name} n={n}"), &v, &x);
                    }
                    // a verdict handed in: what the scan would return
                    let mut x = v.clone();
                    let sc = av_scan_nan_negzero(x.as_ptr(), n);
                    assert_eq!(avx2_qsort_f64_sc(x.as_mut_ptr(), n, sc), 1);
                    check(&format!("avx2_qsort_f64_sc(scan) {name} n={n}"), &v, &x);
                }
            }
            // a NaN: the scanning entry declines with the array untouched; the fused entry declines with a permutation
            let n = 100000usize; let mut r = Rng(99);
            let mut v: Vec<f64> = (0..n).map(|_| r.unit() - 0.5).collect(); v[n / 3] = f64::NAN; v[7] = -0.0;
            let mut x = v.clone();
            assert_eq!(avx2_qsort_f64(x.as_mut_ptr(), n), 0);
            assert!(v.iter().zip(&x).all(|(p, q)| p.to_bits() == q.to_bits()));
            let d0 = g_fuse_declines.load(Relaxed); let p0 = g_fuse_parts.load(Relaxed);
            assert_eq!(avx2_qsort_f64_sc(x.as_mut_ptr(), n, -2), 0);
            assert!(g_fuse_declines.load(Relaxed) > d0 && g_fuse_parts.load(Relaxed) > p0);
            assert!(bits_sorted(&v) == bits_sorted(&x));
            // below AV_FUSE_MIN_N the -2 caller gets the scan: untouched on a decline
            let m = 5000usize; let mut y: Vec<f64> = v[..m].to_vec(); y[11] = f64::NAN; let y0 = y.clone();
            assert_eq!(avx2_qsort_f64_sc(y.as_mut_ptr(), m, -2), 0);
            assert!(y0.iter().zip(&y).all(|(p, q)| p.to_bits() == q.to_bits()));
        }
    }

    #[test]
    fn fused_partition() { let _port_guard = crate::port::port_test_lock();
        if !avx2() { return }
        unsafe {
            let mut r = Rng(0xABCDEF);
            for &n in &[65536usize, 65537, 65539, 100003] {
                for case in 0..4 {
                    let mut v: Vec<f64> = (0..n).map(|_| r.unit() * 4.0 - 2.0 + 1e-9).collect();
                    match case { 1 => v[n - 5] = 0.0, 2 => v[n / 2] = -0.0, 3 => v[3] = f64::NAN, _ => {} }
                    let x0 = v.clone();
                    let p = av_pivot(v.as_ptr(), n);
                    let (mut sm, mut bg, mut fl) = (f64::INFINITY, f64::NEG_INFINITY, 0i32);
                    let m = av_partition_fl(v.as_mut_ptr(), n, p, &mut sm, &mut bg, &mut fl);
                    assert!(bits_sorted(&x0) == bits_sorted(&v), "partition is a permutation, n={n} case={case}");
                    assert_eq!(fl, (case != 0) as i32, "flag, n={n} case={case}");
                    if case != 3 {
                        assert!(v[..m].iter().all(|&x| x < p) && v[m..].iter().all(|&x| x >= p), "split, n={n} case={case}");
                        let (mut mn, mut mx) = (f64::INFINITY, f64::NEG_INFINITY);
                        for &x in &x0 { if x < mn { mn = x; } if x > mx { mx = x; } }
                        assert_eq!((sm, bg), (mn, mx));
                        assert!(m > n / 8 && m < n - n / 8, "the pivot is near the middle: {m} of {n}");
                    }
                }
            }
            // all equal, and the pivot the minimum
            let n = 70000usize;
            let mut v = vec![2.5f64; n]; let v0 = v.clone();
            assert_eq!(av_qsort_fused(v.as_mut_ptr(), n), 1); check("fused constant", &v0, &v);
            let mut v: Vec<f64> = (0..n).map(|i| if i % 9 == 0 { 7.0 } else { 1.0 }).collect(); let v0 = v.clone();
            assert_eq!(av_qsort_fused(v.as_mut_ptr(), n), 1); check("fused two values", &v0, &v);
            let mut v: Vec<f64> = (0..n).map(|i| if i % 9 == 0 { -0.0 } else if i % 9 == 1 { 0.0 } else { (i % 5) as f64 - 2.0 }).collect(); let v0 = v.clone();
            assert_eq!(av_qsort_fused(v.as_mut_ptr(), n), 1); check("fused signed zeros", &v0, &v);
        }
    }

    #[test]
    fn flash_and_coarse() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            for &wc in &[0i32, 1] {
                for &n in &[2usize, 3, 65, 100, 2048, 5000, 70000, 600000] {
                    for (name, v) in shapes(n, 0xF1A5 + n as u64) {
                        let (mut mn, mut mx) = (v[0], v[0]);
                        for &x in &v { if x < mn { mn = x; } else if x > mx { mx = x; } }
                        let mut buf = vec![0f64; n];
                        if mx > mn && (mx - mn).is_finite() && n <= 70000 && !(name == "clustered" || name == "bimodal" || name == "heavy_tail" || name == "lognormal") && wc == 0 {
                            let mut x = v.clone(); let mut c = mk_ctx(n, 1, 0);
                            let r = flash_sort(x.as_mut_ptr(), n, mn, mx, buf.as_mut_ptr(), &mut c);
                            check(&format!("flash_sort {name} n={n}"), &v, core::slice::from_raw_parts(r, n));
                            rm_ctx(&mut c);
                        }
                        for &seq in &[0i32, 1] {
                            if mx > mn && (mx - mn).is_finite() {
                                let mut x = v.clone(); let mut c = mk_ctx(n, seq, wc);
                                assert_eq!(ctx_coarse_ready(&mut c), 1);
                                let r = coarse_bucket_sort(x.as_mut_ptr(), buf.as_mut_ptr(), 0, n as i64 - 1, 0, mn, mx, &mut c);
                                assert!(r == x.as_mut_ptr() || r == buf.as_mut_ptr());
                                check(&format!("coarse_bucket_sort {name} n={n} seq={seq} wc={wc}"), &v, core::slice::from_raw_parts(r, n));
                                // an unknown range (NaN, NaN) is found by the function itself
                                let mut x = v.clone();
                                let r = coarse_bucket_sort(x.as_mut_ptr(), buf.as_mut_ptr(), 0, n as i64 - 1, 0, f64::NAN, f64::NAN, &mut c);
                                check(&format!("coarse_bucket_sort(NaN range) {name} n={n}"), &v, core::slice::from_raw_parts(r, n));
                                rm_ctx(&mut c);
                            }
                            let mut x = v.clone(); let mut c = mk_ctx(n, seq, wc);
                            assert_eq!(ctx_coarse_ready(&mut c), 1);
                            let r = coarse_bits_f64(x.as_mut_ptr(), buf.as_mut_ptr(), 0, n as i64 - 1, 0, 0, 0, 0, &mut c);
                            check(&format!("coarse_bits_f64 {name} n={n} seq={seq} wc={wc}"), &v, core::slice::from_raw_parts(r, n));
                            rm_ctx(&mut c);
                        }
                    }
                }
            }
            // a sub-range [lo, hi] leaves the rest alone
            let n = 9000usize; let mut r = Rng(5); let v: Vec<f64> = (0..n).map(|_| r.unit()).collect();
            let mut x = v.clone(); let mut buf = vec![-1f64; n]; let mut c = mk_ctx(n, 0, 0); ctx_coarse_ready(&mut c);
            let (lo, hi) = (1000usize, 7999usize);
            let res = coarse_bucket_sort(x.as_mut_ptr(), buf.as_mut_ptr(), lo as i64, hi as i64, 0, f64::NAN, f64::NAN, &mut c);
            check("coarse sub-range", &v[lo..=hi], core::slice::from_raw_parts(res.add(lo), hi - lo + 1));
            assert!(x[..lo] == v[..lo] && x[hi + 1..] == v[hi + 1..]);
            rm_ctx(&mut c);
        }
    }

    #[test]
    fn keys_and_probes() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            // hi_key and key_parts are monotone in the value
            let vals = [f64::NEG_INFINITY, -1e300, -2.5, -1.0, -1e-300, -0.0, 0.0, 1e-300, 1.0, 2.5, 1e300, f64::INFINITY];
            for w in vals.windows(2) {
                assert!(hi_key(w[0]) <= hi_key(w[1]));
                let (mut l0, mut h0, mut l1, mut h1) = (0u32, 0u32, 0u32, 0u32);
                key_parts(w[0], &mut l0, &mut h0); key_parts(w[1], &mut l1, &mut h1);
                assert!(((h0 as u64) << 32 | l0 as u64) < ((h1 as u64) << 32 | l1 as u64));
            }
            for &f in &[-3.5f32, -0.0, 0.0, 1.0, f32::INFINITY, f32::NEG_INFINITY, 1e-40] { assert_eq!(f32_unkey(f32_key(f)).to_bits(), (f as f64).to_bits()); }
            assert!(f32_key(-0.0) < f32_key(0.0) && f32_key(-1.0) < f32_key(-0.0) && f32_key(1.0) < f32_key(2.0));
            // the skew probe: uniform is flat, a heavy tail is skewed, a constant is 1, an infinite range is maximal
            let n = 100000usize; let mut r = Rng(31);
            let u: Vec<f64> = (0..n).map(|_| r.unit()).collect();
            assert!(skew_probe(u.as_ptr(), n) < 2.0);
            let h: Vec<f64> = (0..n).map(|_| (1.0 / r.unit().max(1e-300)).powf(2.0)).collect();
            assert!(skew_probe(h.as_ptr(), n) > SKEW_PROBE_MAX as f64);
            let k = vec![4.0f64; n]; assert_eq!(skew_probe(k.as_ptr(), n), 1.0);
            let mut w = u.clone(); for x in w.iter_mut().step_by(2) { *x = f64::INFINITY; } assert_eq!(skew_probe(w.as_ptr(), n), 256.0);
            let s: Vec<f64> = (0..n).map(|_| f64::from_bits(r.next() >> 14)).collect(); assert_eq!(skew_probe(s.as_ptr(), n), 256.0);
            // the quantization probe counts varying key bytes
            let q: Vec<f64> = (0..n).map(|_| r.below(200) as f64).collect();
            let pq = quant_probe_f64(q.as_ptr(), n); assert!(pq >= 1 && pq <= 3, "{pq}");
            assert!(quant_probe_f64(u.as_ptr(), n) >= 6);
            assert_eq!(quant_probe_f64(k.as_ptr(), n), 0);
            // the thread caps
            assert_eq!(count_threads(100000, 14), 3); assert_eq!(count_threads(100000, 2), 2);
            assert_eq!(count_threads(3_500_000, 14), 10); assert_eq!(count_threads_hash(35_000_000, 14), 14);
        }
    }

    #[test]
    fn runs_and_merges() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0xBEEF);
            let mut c = mk_ctx(16, 1, 0);
            // detection: ascending runs kept, descending runs reversed in place, the list flat [start, end]
            let v = [1.0, 2.0, 2.0, 5.0, 4.0, 3.0, 1.0, 7.0, 8.0, 0.5];
            let mut x = v.to_vec(); let mut nr = 0usize;
            let runs = detect_runs_limited(x.as_mut_ptr(), x.len(), 32, 0, &mut nr, &mut c);
            assert!(!runs.is_null());
            let list: Vec<usize> = core::slice::from_raw_parts(runs, 2 * nr).to_vec();
            assert_eq!(list, vec![0, 3, 4, 6, 7, 8, 9, 9]);
            assert_eq!(x, vec![1.0, 2.0, 2.0, 5.0, 1.0, 3.0, 4.0, 7.0, 8.0, 0.5]);
            free(runs);
            // the bails: too many runs, and a short average once 256 runs are held
            let mut y: Vec<f64> = (0..4000).map(|i| ((i * 7919) % 4000) as f64).collect();
            let runs = detect_runs_limited(y.as_mut_ptr(), y.len(), 32, 0, &mut nr, &mut c);
            assert!(runs.is_null() && nr == 0 && c.last_bail_avg_run > 0.0);
            let mut y: Vec<f64> = (0..40000).map(|i| (i % 3) as f64).collect();
            let runs = detect_runs_limited(y.as_mut_ptr(), y.len(), 100000, 4096, &mut nr, &mut c);
            assert!(runs.is_null() && c.last_bail_avg_run <= 3.0);
            rm_ctx(&mut c);

            // merges of k sorted runs: lengths random, heavy ties, signed zeros, infinities
            for &k in &[2usize, 3, 4, 5, 7, 8, 15, 16, 17, 33, 64, 65, 100] {
                for trial in 0..6 {
                    let mut v: Vec<f64> = Vec::new(); let mut runs: Vec<usize> = Vec::new();
                    for _ in 0..k {
                        let len = 1 + r.below(if trial % 2 == 0 { 40 } else { 3000 });
                        let mut run: Vec<f64> = (0..len).map(|_| match trial {
                            0 | 1 => r.unit(),
                            2 | 3 => r.below(20) as f64,
                            // +inf only where merge_4way_hybrid is not reached: its exhausted-run sentinel IS +inf, and an exhausted run then
                            // wins every tie against a live run's real +inf and the loop never ends (the C's defect, ported as it is)
                            4 => match r.below(6) { 0 => -0.0, 1 => 0.0, 2 => if k < QUADWAY_THRESHOLD && k != 4 { f64::INFINITY } else { 1e300 }, 3 => f64::NEG_INFINITY, _ => r.unit() - 0.5 },
                            _ => (r.below(1000) as f64).sqrt(),
                        }).collect();
                        run.sort_by(|p, q| p.partial_cmp(q).unwrap());
                        runs.push(v.len()); v.extend_from_slice(&run); runs.push(v.len() - 1);
                    }
                    let n = v.len();
                    let mut x = v.clone(); let mut buf = vec![0f64; n];
                    merge_all_runs(x.as_mut_ptr(), buf.as_mut_ptr(), runs.as_ptr(), k);
                    check(&format!("merge_all_runs k={k} trial={trial}"), &v, &x);
                    if k == 2 {
                        let mut x = v.clone();
                        merge_gallop(x.as_mut_ptr(), buf.as_mut_ptr(), runs[0], runs[1], runs[2], runs[3]);
                        check("merge_gallop", &v, &x);
                    }
                    if k == 4 {
                        let mut x = v.clone();
                        merge_4way_hybrid(x.as_mut_ptr(), buf.as_mut_ptr(), runs[0], runs[1], runs[2], runs[3], runs[4], runs[5], runs[6], runs[7]);
                        check("merge_4way_hybrid", &v, &x);
                    }
                }
            }
            // a long winner (the gallop), and runs already in order (the early return)
            let v: Vec<f64> = (0..5000).map(|i| i as f64).chain((0..10).map(|i| i as f64 * 500.0 + 0.5)).collect();
            let mut x = v.clone(); let mut buf = vec![0f64; v.len()];
            merge_gallop(x.as_mut_ptr(), buf.as_mut_ptr(), 0, 4999, 5000, 5009); check("gallop sparse", &v, &x);
            let v: Vec<f64> = (0..100).map(|i| i as f64).collect(); let mut x = v.clone();
            merge_4way_hybrid(x.as_mut_ptr(), buf.as_mut_ptr(), 0, 24, 25, 49, 50, 74, 75, 99); assert_eq!(x, v);
        }
    }

    #[test]
    fn float32_narrowing() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0xF32);
            for &n in &[1usize, 2, 100, 70000, 300000] {
                for case in 0..6 {
                    let v: Vec<f64> = (0..n).map(|_| match case {
                        0 => (r.unit() * 2e6 - 1e6) as f32 as f64,                       // three passes
                        1 => r.below(1000) as f64,                                        // few bits... of a float's key
                        2 => (r.below(8) as f64) * 0.25,
                        3 => match r.below(5) { 0 => -0.0, 1 => 0.0, 2 => f64::INFINITY, 3 => f64::NEG_INFINITY, _ => (r.unit() - 0.5) as f32 as f64 },
                        4 => 1.5,
                        _ => 1.0 + r.below(1500) as f64 * (f32::EPSILON as f64),         // one or two passes: a narrow key range
                    }).collect();
                    let mut x = v.clone(); let mut buf = vec![0f64; n]; let mut c = mk_ctx(n, 1, 0);
                    let s0 = g_f32_sorts.load(Relaxed);
                    assert_eq!(f32_radix_try(x.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c), 1, "case {case} n={n}");
                    assert!(g_f32_sorts.load(Relaxed) > s0);
                    check(&format!("f32_radix_try case={case} n={n}"), &v, &x);
                    // one value that is not a float (or a NaN) declines with a[] as it came, bit for bit
                    if n >= 2 {
                        for &bad in &[0.1f64, f64::NAN] {
                            for &pos in &[0usize, n / 2, n - 1] {
                                let mut y = v.clone(); y[pos] = bad; let y0 = y.clone();
                                assert_eq!(f32_radix_try(y.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c), 0);
                                assert!(y0.iter().zip(&y).all(|(p, q)| p.to_bits() == q.to_bits()), "decline restores, case {case} n={n} pos={pos}");
                            }
                        }
                    }
                    rm_ctx(&mut c);
                }
            }
        }
    }

    unsafe fn run_leaf(which: i32, v: &[f64], seq: i32) -> Vec<f64> {
        let n = v.len();
        let mut x = v.to_vec(); let mut buf = vec![0f64; n.max(1)]; let mut c = mk_ctx(n, seq, 0);
        let r = if which == 0 { sort_fn(x.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c) } else { leaf_f64(x.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c) };
        assert!(r == x.as_mut_ptr() || r == buf.as_mut_ptr());
        let out = core::slice::from_raw_parts(r, n).to_vec();
        rm_ctx(&mut c);
        out
    }

    #[test]
    fn sort_fn_shapes() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            for &n in &[0usize, 1, 2, 3, 7, 16, 24, 25, 63, 64, 100, 1000, 1024, 2047, 2048, 5000, 40000, 65536, 70000, 300000, 600000] {
                for (name, v) in shapes(n.max(1), 0x5F + n as u64) {
                    let v = &v[..n];
                    for &seq in &[1i32, 0] {
                        let out = run_leaf(0, v, seq);
                        check(&format!("sort_fn {name} n={n} seq={seq}"), v, &out);
                    }
                }
            }
        }
    }

    #[test]
    fn leaf_f64_shapes() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            for &n in &[1000usize, 65535, 65536, 65537, 100000, 600000] {
                for (name, v) in shapes(n, 0x1EAF + n as u64) {
                    for &seq in &[1i32, 0] {
                        let out = run_leaf(1, &v, seq);
                        check(&format!("leaf_f64 {name} n={n} seq={seq}"), &v, &out);
                    }
                }
            }
        }
    }

    #[test]
    fn nan_keeps_the_multiset() { let _port_guard = crate::port::port_test_lock();
        // with a NaN no order is promised: a return, and a permutation of the input's bit patterns
        unsafe {
            let mut r = Rng(0x7A7);
            for &n in &[30usize, 3000, 70000, 300000] {
                for (name, mut v) in shapes(n, 0x44 + n as u64) {
                    for _ in 0..1 + n / 100 { let i = r.below(n); v[i] = f64::from_bits(0x7FF8_0000_0000_0000 | (r.next() & 0xFFFF)); }
                    for which in 0..2 { for &seq in &[1i32, 0] {
                        let out = run_leaf(which, &v, seq);
                        assert!(bits_sorted(&v) == bits_sorted(&out), "{name} n={n} which={which} seq={seq}: not a permutation");
                    } }
                }
            }
        }
    }

    #[test]
    fn route_witnesses() { let _port_guard = crate::port::port_test_lock();
        if !avx2() { return }
        unsafe {
            // a sequential, unstructured range above the flash window: the profile is deferred to the kernel's first partition
            let n = 200000usize; let mut r = Rng(0x17);
            let v: Vec<f64> = (0..n).map(|_| r.unit() - 0.5).collect();
            let p0 = g_fuse_parts.load(Relaxed);
            let out = run_leaf(0, &v, 1); check("deferred profile", &v, &out);
            assert!(g_fuse_parts.load(Relaxed) > p0, "sort_fn at seq = 1 took the fused first partition");
            // every value a float, sequential: the float32 radix
            let f: Vec<f64> = v.iter().map(|&x| x as f32 as f64).collect();
            let s0 = g_f32_sorts.load(Relaxed);
            let out = run_leaf(1, &f, 1); check("float32 route", &f, &out);
            assert!(g_f32_sorts.load(Relaxed) > s0);
            // a Weyl sequence: the low-discrepancy probe fires and the context says so
            let w: Vec<f64> = (0..n).map(|i| { let f = i as f64 * 0.6180339887498949; f - f.floor() }).collect();
            let mut x = w.clone(); let mut buf = vec![0f64; n]; let mut c = mk_ctx(n, 1, 0);
            let l0 = g_lowdisc_fires.load(Relaxed);
            let res = sort_fn(x.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c);
            check("weyl", &w, core::slice::from_raw_parts(res, n));
            assert!(c.lowdisc == 1 && g_lowdisc_fires.load(Relaxed) > l0);
            rm_ctx(&mut c);
        }
    }
}

// ---- what this file calls from the others (PORTING.md) -------------------------------------------------------------------
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    // base.rs (C 496-850)
    pub(crate) const g_no_dense_probe: i32 = 0;
    pub(crate) const g_no_frontfuse: i32 = 0;
    pub(crate) const g_no_partfuse: i32 = 0;
    pub(crate) const g_no_smallnet: i32 = 0;
    pub(crate) const g_avx2_over_flash: i32 = 0;
    pub(crate) const g_band_flash: i32 = 0;
    pub(crate) static g_lowdisc_fires: AtomicUsize = AtomicUsize::new(0);
    pub(crate) static g_sf_windows: AtomicUsize = AtomicUsize::new(0);
    pub(crate) const AVX2_LEAF_MAX_N: usize = usize::MAX;
    #[inline(always)]
    pub(crate) unsafe fn avx2_take(c: *mut ctx_t, n: usize) -> i32 { ((*c).seq != 0 && (*c).nearly == 0 && (*c).lowdisc == 0 && avx2_leaf_on() != 0 && n <= AVX2_LEAF_MAX_N) as i32 }
    pub(crate) unsafe fn log2i(mut n: usize) -> i32 { let mut l = 0; loop { n >>= 1; if n == 0 { break } l += 1; } l }
    #[inline]
    pub(crate) unsafe fn band_local_order(a: *const f64, n: usize) -> i32 {
        let mut d = 0usize;
        for w in 0..4usize { let lo = 1 + (n - 34) * w / 4; for i in lo..lo + 32 { d += (*a.add(i) < *a.add(i - 1)) as usize; } }
        (d * 3 < 128) as i32
    }
    pub(crate) unsafe fn insertion_sort(a: *mut f64, lo: i64, hi: i64) {
        let mut i = lo + 1;
        while i <= hi { let k = *a.offset(i as isize); let mut j = i - 1; while j >= lo && *a.offset(j as isize) > k { *a.offset(j as isize + 1) = *a.offset(j as isize); j -= 1; } *a.offset(j as isize + 1) = k; i += 1; }
    }
    pub(crate) unsafe fn insertion_sort_momentum(a: *mut f64, lo: i64, hi: i64) -> i32 {
        let budget = 8 * (hi - lo + 1); let (mut work, mut momentum) = (0i64, 1i64);
        let at = |p: i64| a.offset(p as isize);
        let mut i = lo + 1;
        while i <= hi {
            let k = *at(i);
            if *at(i - 1) <= k { momentum = if momentum > 1 { momentum >> 1 } else { 1 }; i += 1; continue }
            let (mut j, mut step) = (i - 1, momentum);
            while step > 1 && (j - step < lo || *at(j - step) <= k) { step >>= 1; }
            while j - step >= lo && *at(j - step) > k { j -= step; step <<= 1; }
            let (mut left, mut right) = (if j - step < lo { lo } else { j - step }, j);
            while left < right { let mid = (left + right) >> 1; if *at(mid) > k { right = mid; } else { left = mid + 1; } }
            let dist = i - left;
            work += dist;
            if work > budget { return 0 }
            let mut p = i; while p > left { *at(p) = *at(p - 1); p -= 1; }
            *at(left) = k; momentum = dist;
            i += 1;
        }
        1
    }
    /// a stand-in with the C signature: any sort that returns on a NaN and keeps the multiset
    pub(crate) unsafe fn quicksort(a: *mut f64, lo: i64, hi: i64, d: i32) {
        if hi > lo { core::slice::from_raw_parts_mut(a.offset(lo as isize), (hi - lo + 1) as usize).sort_unstable_by(f64::total_cmp); }
    }
    #[inline(never)]
    pub(crate) unsafe fn net_sort_small(a: *mut f64, n: i64) { insertion_sort(a, 0, n - 1) }
    pub(crate) unsafe fn lsd_radix_int32(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) { quicksort(a, 0, n as i64 - 1, 0) }
    pub(crate) unsafe fn ctx_coarse_ready(c: *mut ctx_t) -> i32 {
        if !(*c).C[0].is_null() { return 1 }
        for d in 0..COARSE_MAXD {
            (*c).land[d] = malloc(COARSE_K);
            (*c).C[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
            (*c).S[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
            (*c).H[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
        }
        1
    }
    // knot.rs (C 1981-2738)
    pub(crate) unsafe fn runs_relocate_f64(a: *mut f64, n: usize, buf: *mut f64, runs: *const usize, n_runs: usize) -> *mut f64 { null_mut() }
    #[inline(never)]
    pub(crate) unsafe fn merge_runs_vec(a: *mut f64, buf: *mut f64, runs: *const usize, k: usize) -> i32 { 0 }
}
#[cfg(not(feature = "port_all"))]
use missing::*;
