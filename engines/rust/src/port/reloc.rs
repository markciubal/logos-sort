//! C lines 6032-6917 of `logos_psort.c`: top-level disjoint-run relocation (J5, parallel) and the knot route's parallel
//! form. The run detector (gap splits, the block-of-eight scan, kink repair, stray lifting), the join with its boundary
//! repairs, the streaming placements, the two placement workers, the stray machinery and `relocate_try_f64`.
//! A transliteration: see `PORTING.md`. Not ported (opt-in by environment only): the literal linked-list placement
//! (`lunit_t`, `list_*`, `knot_place_list`, C lines 6548-6667).
use super::*;

macro_rules! at { ($p:expr, $i:expr) => { *$p.add($i) }; }

/// C line 5818 (the default build's form), expanded here because a `macro_rules!` of another file is not in textual scope.
macro_rules! NESTED_SORT {
    ($call:expr) => {{
        let cm_ = g_ctx_next;
        ns_depth.with(|d| d.set(d.get() + 1));
        $call;
        ns_depth.with(|d| d.set(d.get() - 1));
        g_ctx_next = cm_;
    }};
}

// ---- run detection --------------------------------------------------------------------------------------------------
pub(crate) const RELOC_GAP_FACTOR: f64 = 1024.0;
pub(crate) const RELOC_MIN_BODY: usize = 16;

#[inline]
pub(crate) unsafe fn reloc_gap_split_f(a: *const f64, start: usize, last: usize, next: usize, factor: f64, min_body: usize) -> i32 {
    let cnt = last - start + 1;
    if cnt < min_body { return 0 }
    ((at!(a, next) - at!(a, last)) * cnt as f64 > factor * (at!(a, last) - at!(a, start))) as i32
}
/// would a[next] break the run [start..last]?
#[inline]
pub(crate) unsafe fn reloc_gap_split(a: *const f64, start: usize, last: usize, next: usize) -> i32 {
    reloc_gap_split_f(a, start, last, next, RELOC_GAP_FACTOR, RELOC_MIN_BODY)
}

/// Sampled pre-gate: 1024 strided elements. 1 = go on to the full probe (or undecidable), 0 = decline.
pub(crate) unsafe fn reloc_sample_gate(a: *const f64, n: usize, kfrac: f64, kfew: f64) -> i32 {
    const S: usize = 1024;
    const MAXR: usize = 16;
    const KMAXR: usize = 128;
    let maxr: usize = if kfrac > 0.0 { KMAXR } else { MAXR };
    if n < 4 * S { return 1 }
    let mut sbuf = [0.0f64; S];
    let s: *mut f64 = sbuf.as_mut_ptr();
    let step = n / S;
    for i in 0..S { at!(s, i) = at!(a, i * step); }
    let mut runs = [0usize; 2 * (KMAXR + 1)];
    let mut R: usize = 0;
    let mut i: usize = 0;
    while i < S {
        let mut j = i + 1;
        if j < S {
            if at!(s, j) >= at!(s, i) {
                while j < S && at!(s, j) >= at!(s, j - 1) && reloc_gap_split_f(s, i, j - 1, j, 4.0, 4) == 0 { j += 1; }
            } else {
                while j < S && at!(s, j) < at!(s, j - 1)
                    && !(j - i >= 4 && (at!(s, j - 1) - at!(s, j)) * (j - i) as f64 > 4.0 * (at!(s, i) - at!(s, j - 1))) { j += 1; }
                let (mut l, mut r) = (i, j - 1);
                while l < r { let t = at!(s, l); at!(s, l) = at!(s, r); at!(s, r) = t; l += 1; r -= 1; }
            }
        }
        if R == maxr { return 1 }
        runs[2 * R] = i; runs[2 * R + 1] = j - 1; R += 1; i = j;
    }
    if R < 2 { return 1 }
    let mut d: *mut rdesc_t = null_mut();
    let mut ch: *mut usize = null_mut();
    let mut nc: usize = 0;
    let mut merged: usize = 0;
    if runs_plan(s, runs.as_ptr(), R, &mut d, &mut ch, &mut nc, &mut merged) == 0 { return 1 }
    let mut go: i32 = (merged <= S / 2) as i32;
    // Most of the sample sits in chains: tangled for J5, but how much of it actually CROSSES? Up to MAXR runs the sample's
    // verdict is final, as J5's is. Beyond that the sample only DECLINES, and only when its estimate clears the gate by a tenth.
    if R > MAXR { go = 1; }
    if kfrac > 0.0 && merged > S / 2 {
        let mut K: knot_plan_t = core::mem::zeroed();
        if knot_plan(s, d, R, S + 1, 0, &mut K) != 0 {
            let kf = if R > MERGE_MAX_RUNS { kfrac } else { kfew };
            if R <= MAXR { go = (K.cost as f64 <= kf * S as f64) as i32; }
            else if K.cost as f64 > 1.1 * kf * S as f64 { go = 0; }
            TRACE!("knot: sample gate R={} crossing={} cost={} kmax={} of {} (gate {:.2}) -> {}", R, K.crossing, K.cost, K.kmax, S, kf, if go != 0 { "go" } else { "decline" });
        }
    }
    free(d); free(ch);
    TRACE!("reloc: sample gate R={} merged={}/{}", R, merged, S);
    go
}

/// LOGOS_DESC_STRICT unset: a tie continues a descending run.
pub(crate) const g_desc_ties: i32 = 1;

/// last: this chunk ends at the array's end. stray: positions lifted out of ascending runs. kfrac > 0: the knot route's
/// cost gate; twist: leave descending runs as they lie; fast: scan in blocks of eight.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct rdet_arg_t {
    pub(crate) a: *mut f64,
    pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) cap: usize, pub(crate) R: usize,
    pub(crate) runs: *mut usize,
    pub(crate) abort: *mut AtomicI32,
    pub(crate) tid: i32,
    pub(crate) kfrac: f64,
    pub(crate) twist: i32, pub(crate) fast: i32, pub(crate) kink: i32, pub(crate) ties: i32, pub(crate) simd: i32,
    pub(crate) kinks: usize,
    pub(crate) stray: *mut usize, pub(crate) nstray: usize, pub(crate) stray_cap: usize, pub(crate) sdiv: usize,
    pub(crate) last: i32,
}
/// the stray list is taken at the FIRST stray. 0 = no room: strays end runs.
pub(crate) unsafe fn stray_room(w: *mut rdet_arg_t, k: usize) -> i32 {
    if (*w).stray_cap == 0 || (*w).nstray + k > (*w).stray_cap { return 0 }
    if (*w).stray.is_null() { (*w).stray = malloc((*w).stray_cap * size_of::<usize>()) as *mut usize; }
    1
}

// why the block advance stopped
pub(crate) const RELOC_BLK_BREAK: i32 = 0;
pub(crate) const RELOC_BLK_BOUND: i32 = 1;
pub(crate) const RELOC_BLK_SHORT: i32 = 2;

pub(crate) unsafe fn reloc_blocks_up_c(a: *const f64, i: usize, mut j: usize, n: usize, ties: i32, pstate: *mut i32) -> usize {
    loop {
        if j + 8 > n { *pstate = RELOC_BLK_SHORT; return j }
        let mut dmax: f64 = 0.0;
        let mut ok: i32 = 1;
        for k in 0..8usize { let d = at!(a, j + k) - at!(a, j + k - 1); ok &= (d >= 0.0) as i32; dmax = if d > dmax { d } else { dmax }; }
        let fits = !(dmax * (j - i + 8) as f64 > RELOC_GAP_FACTOR * (at!(a, j - 1) - at!(a, i)));
        if ok != 0 && fits { j += 8; continue }
        *pstate = if ok == 0 && fits { RELOC_BLK_BREAK } else { RELOC_BLK_BOUND };
        return j;
    }
}
pub(crate) unsafe fn reloc_blocks_down_c(a: *const f64, i: usize, mut j: usize, n: usize, ties: i32, pstate: *mut i32) -> usize {
    loop {
        if j + 8 > n { *pstate = RELOC_BLK_SHORT; return j }
        let mut dmax: f64 = 0.0;
        let mut ok: i32 = 1;
        for k in 0..8usize {
            let d = at!(a, j + k - 1) - at!(a, j + k);
            ok &= ((d > 0.0) as i32) | (ties & (d == 0.0) as i32);
            dmax = if d > dmax { d } else { dmax };
        }
        let fits = !(dmax * (j - i + 8) as f64 > RELOC_GAP_FACTOR * (at!(a, i) - at!(a, j - 1)));
        if ok != 0 && fits { j += 8; continue }
        *pstate = if ok == 0 && fits { RELOC_BLK_BREAK } else { RELOC_BLK_BOUND };
        return j;
    }
}

macro_rules! RELOC_BLOCKS_SIMD {
    ($(#[$m:meta])* $NAME:ident, $VEC:ty, $LOAD:ident, $SUB:ident, $MAXV:ident, $OR:ident, |$d:ident| $CMP_BAD:expr, $MOVEMASK:ident, $ZERO:expr, $WIDE:expr, $HMAX:ident,
     |$p:ident, $k:ident| $CUR:expr, $PREV:expr, |$a:ident, $i:ident, $j:ident| $RANGE:expr) => {
        $(#[$m])*
        pub(crate) unsafe fn $NAME($a: *const f64, $i: usize, mut $j: usize, n: usize, ties: i32, pstate: *mut i32) -> usize {
            loop {
                if $j + 8 > n { *pstate = RELOC_BLK_SHORT; return $j }
                let mut acc: $VEC = $ZERO;
                let mut bad: $VEC = $ZERO;
                let $p: *const f64 = $a.add($j);
                let mut $k: usize = 0;
                while $k < 8 {
                    let $d: $VEC = $SUB($LOAD($CUR), $LOAD($PREV));
                    bad = $OR(bad, $CMP_BAD); acc = $MAXV($d, acc);
                    $k += $WIDE;
                }
                let ok = ($MOVEMASK(bad) == 0) as i32;
                let dmax: f64 = $HMAX(acc);
                let fits = !(dmax * ($j - $i + 8) as f64 > RELOC_GAP_FACTOR * ($RANGE));
                if ok != 0 && fits { $j += 8; continue }
                *pstate = if ok == 0 && fits { RELOC_BLK_BREAK } else { RELOC_BLK_BOUND };
                return $j;
            }
        }
    };
}
#[inline]
pub(crate) unsafe fn hmax2(v: __m128d) -> f64 { let h = _mm_unpackhi_pd(v, v); _mm_cvtsd_f64(_mm_max_pd(v, h)) }
RELOC_BLOCKS_SIMD!(reloc_blocks_up_sse2, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, |d| _mm_cmpnge_pd(d, _mm_setzero_pd()),
                   _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, |p, k| p.add(k), p.add(k).sub(1), |a, i, j| at!(a, j - 1) - at!(a, i));
RELOC_BLOCKS_SIMD!(reloc_blocks_down_sse2_strict, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, |d| _mm_cmpngt_pd(d, _mm_setzero_pd()),
                   _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, |p, k| p.add(k).sub(1), p.add(k), |a, i, j| at!(a, i) - at!(a, j - 1));
RELOC_BLOCKS_SIMD!(reloc_blocks_down_sse2_ties, __m128d, _mm_loadu_pd, _mm_sub_pd, _mm_max_pd, _mm_or_pd, |d| _mm_cmpnge_pd(d, _mm_setzero_pd()),
                   _mm_movemask_pd, _mm_setzero_pd(), 2, hmax2, |p, k| p.add(k).sub(1), p.add(k), |a, i, j| at!(a, i) - at!(a, j - 1));
#[inline]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn hmax4(v: __m256d) -> f64 {
    let mut lo = _mm256_castpd256_pd128(v);
    let hi = _mm256_extractf128_pd::<1>(v);
    lo = _mm_max_pd(lo, hi);
    let h = _mm_unpackhi_pd(lo, lo);
    _mm_cvtsd_f64(_mm_max_pd(lo, h))
}
RELOC_BLOCKS_SIMD!(#[target_feature(enable = "avx2,fma,popcnt")] reloc_blocks_up_avx2, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                   |d| _mm256_cmp_pd::<_CMP_NGE_UQ>(d, _mm256_setzero_pd()), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4,
                   |p, k| p.add(k), p.add(k).sub(1), |a, i, j| at!(a, j - 1) - at!(a, i));
RELOC_BLOCKS_SIMD!(#[target_feature(enable = "avx2,fma,popcnt")] reloc_blocks_down_avx2_strict, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                   |d| _mm256_cmp_pd::<_CMP_NGT_UQ>(d, _mm256_setzero_pd()), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4,
                   |p, k| p.add(k).sub(1), p.add(k), |a, i, j| at!(a, i) - at!(a, j - 1));
RELOC_BLOCKS_SIMD!(#[target_feature(enable = "avx2,fma,popcnt")] reloc_blocks_down_avx2_ties, __m256d, _mm256_loadu_pd, _mm256_sub_pd, _mm256_max_pd, _mm256_or_pd,
                   |d| _mm256_cmp_pd::<_CMP_NGE_UQ>(d, _mm256_setzero_pd()), _mm256_movemask_pd, _mm256_setzero_pd(), 4, hmax4,
                   |p, k| p.add(k).sub(1), p.add(k), |a, i, j| at!(a, i) - at!(a, j - 1));

/// LOGOS_SCANBLOCK_SCALAR unset.
pub(crate) const g_scan_scalar: i32 = 0;
#[inline]
pub(crate) unsafe fn reloc_blocks_up(a: *const f64, i: usize, j: usize, n: usize, simd: i32, pstate: *mut i32) -> usize {
    if simd == 2 { return reloc_blocks_up_avx2(a, i, j, n, 0, pstate) }
    if simd == 1 { return reloc_blocks_up_sse2(a, i, j, n, 0, pstate) }
    reloc_blocks_up_c(a, i, j, n, 0, pstate)
}
#[inline]
pub(crate) unsafe fn reloc_blocks_down(a: *const f64, i: usize, j: usize, n: usize, simd: i32, ties: i32, pstate: *mut i32) -> usize {
    if simd == 2 { return if ties != 0 { reloc_blocks_down_avx2_ties(a, i, j, n, ties, pstate) } else { reloc_blocks_down_avx2_strict(a, i, j, n, ties, pstate) } }
    if simd == 1 { return if ties != 0 { reloc_blocks_down_sse2_ties(a, i, j, n, ties, pstate) } else { reloc_blocks_down_sse2_strict(a, i, j, n, ties, pstate) } }
    reloc_blocks_down_c(a, i, j, n, ties, pstate)
}

pub(crate) unsafe fn reloc_detect_worker(p: *mut c_void) -> *mut c_void { reloc_detect_worker_body(p) }
pub(crate) unsafe fn reloc_detect_worker_body(p: *mut c_void) -> *mut c_void {
    let w = p as *mut rdet_arg_t;
    let a: *mut f64 = (*w).a;
    let n: usize = (*w).hi;
    let mut i: usize = (*w).lo;
    let mut next_check: usize = 2;
    let ties: i32 = (*w).ties;
    while i < n {
        // THE SWING AT A RUN'S HEAD (stray mode): a run that starts with a stray below it has no body for the gap test to
        // read. If the first step dwarfs the next two, the head is a stray and the run starts after it.
        if (*w).stray_cap != 0 && i + 4 < n {
            let r1 = at!(a, i + 3) - at!(a, i + 1);
            let r2 = at!(a, i + 4) - at!(a, i + 2);
            if at!(a, i + 1) <= at!(a, i + 2) && at!(a, i + 2) <= at!(a, i + 3) && r1 > 0.0 && (at!(a, i + 1) - at!(a, i)).abs() * 2.0 > RELOC_GAP_FACTOR * r1 && stray_room(w, 1) != 0 {
                at!((*w).stray, (*w).nstray) = i; (*w).nstray += 1; i += 1; continue;
            }
            if at!(a, i + 2) <= at!(a, i + 3) && at!(a, i + 3) <= at!(a, i + 4) && r2 > 0.0 && (at!(a, i + 2) - at!(a, i + 1)).abs() * 2.0 > RELOC_GAP_FACTOR * r2
                && (at!(a, i + 2) - at!(a, i)).abs() * 2.0 > RELOC_GAP_FACTOR * r2 && stray_room(w, 2) != 0 {
                at!((*w).stray, (*w).nstray) = i; (*w).nstray += 1;
                at!((*w).stray, (*w).nstray) = i + 1; (*w).nstray += 1;
                i += 2; continue;                                   // two strays at the head
            }
        }
        let mut j: usize = i + 1;
        let mut tw: usize = 0;
        if j < n {
            if at!(a, j) >= at!(a, i) {
                let mut hold: usize = 0;
                let mut nolook: i32 = 0;                           // nolook: a[j-2] is a lifted stray this step, so no test may read it
                while j < n {
                    if (*w).fast != 0 && hold == 0 && j - i >= RELOC_MIN_BODY && j + 8 <= n {
                        let mut st: i32 = 0;
                        let j2 = reloc_blocks_up(a, i, j, n, (*w).simd, &mut st);
                        if j2 != j { j = j2; nolook = 0; }
                        if st == RELOC_BLK_BREAK && (*w).fast == 2 {   // the break: the steps before it are proven, jump to it
                            let mut kf: usize = 0;
                            while at!(a, j + kf) - at!(a, j + kf - 1) >= 0.0 { kf += 1; }   // the block's own test: inf - inf stops it here too
                            if kf != 0 { j += kf; nolook = 0; }
                            hold = 2;
                        } else if st != RELOC_BLK_SHORT { hold = 8; }
                        if j >= n { break }                         // the advance ended exactly at the chunk's end: the scalar line must not read a[n]
                    }
                    let look2: i32 = (nolook == 0) as i32;
                    nolook = 0;
                    if at!(a, j) >= at!(a, j - 1) && reloc_gap_split(a, i, j - 1, j) == 0 { j += 1; if hold != 0 { hold -= 1; } }
                    // A KINK (Reidemeister move I): one adjacent pair swapped inside a run that otherwise goes on. Both
                    // neighbours must agree. Never across a chunk's end: the next chunk's thread may be writing there.
                    else if look2 != 0 && (*w).kink != 0 && j + 1 < n && j >= i + 2 && at!(a, j) < at!(a, j - 1) && at!(a, j) >= at!(a, j - 2) && at!(a, j - 1) <= at!(a, j + 1) {
                        let x = at!(a, j); at!(a, j) = at!(a, j - 1); at!(a, j - 1) = x; (*w).kinks += 1;
                    }
                    // A STRAY (see stray_compact), in three forms. Its position is recorded, the run reads on past it, and the
                    // hole is closed before the plan is made. Never the last element of a chunk.
                    else if (*w).stray_cap != 0 && j + 1 < n && j >= i + 4 && at!(a, j) < at!(a, j - 1) && at!(a, j + 1) >= at!(a, j - 1) && reloc_gap_split(a, i, j - 1, j + 1) == 0 && stray_room(w, 1) != 0 {
                        at!((*w).stray, (*w).nstray) = j; (*w).nstray += 1; j += 2; nolook = 1;
                        if hold != 0 { hold = if hold > 2 { hold - 2 } else { 0 }; }     // below the run: a[j] breaks it, a[j+1] resumes it
                    }
                    else if (*w).stray_cap != 0 && j + 1 < n && j >= i + 4 && at!(a, j) >= at!(a, j - 1) && at!(a, j + 1) >= at!(a, j - 1) && reloc_gap_split(a, i, j - 1, j + 1) == 0 && stray_room(w, 1) != 0 {
                        at!((*w).stray, (*w).nstray) = j; (*w).nstray += 1; j += 2; nolook = 1;
                        if hold != 0 { hold = if hold > 2 { hold - 2 } else { 0 }; }     // a gap up: a[j] far above, a[j+1] resumes
                    }
                    else if look2 != 0 && (*w).stray_cap != 0 && j + 1 < n && j >= i + 4 && at!(a, j) < at!(a, j - 1) && at!(a, j) >= at!(a, j - 2) && at!(a, j + 1) >= at!(a, j) && reloc_gap_split(a, i, j - 2, j) == 0 && stray_room(w, 1) != 0 {
                        at!((*w).stray, (*w).nstray) = j - 1; (*w).nstray += 1; j += 1; nolook = 1;
                        if hold != 0 { hold -= 1; }                 // above the run: a[j-1] was the stray, a[j] continues from a[j-2]
                    }
                    else if (*w).stray_cap != 0 && j + 2 < n && j >= i + 4 && at!(a, j) == at!(a, j) && at!(a, j + 1) == at!(a, j + 1) && at!(a, j + 2) >= at!(a, j - 1) && reloc_gap_split(a, i, j - 1, j + 2) == 0 && stray_room(w, 2) != 0 {
                        at!((*w).stray, (*w).nstray) = j; (*w).nstray += 1;
                        at!((*w).stray, (*w).nstray) = j + 1; (*w).nstray += 1;
                        j += 3; nolook = 1;
                        if hold != 0 { hold = if hold > 3 { hold - 3 } else { 0 }; }     // two strays side by side, a[j+2] resumes
                    }
                    else { break }
                }
            } else {
                let mut hold: usize = 0;
                while j < n {
                    if (*w).fast != 0 && hold == 0 && j - i >= RELOC_MIN_BODY && j + 8 <= n {
                        let mut st: i32 = 0;
                        let j2 = reloc_blocks_down(a, i, j, n, (*w).simd, ties, &mut st);
                        j = j2;
                        if st == RELOC_BLK_BREAK && (*w).fast == 2 {   // the break, as in an ascending run
                            let mut kf: usize = 0;
                            loop { let d = at!(a, j + kf - 1) - at!(a, j + kf); if (((d > 0.0) as i32) | (ties & (d == 0.0) as i32)) == 0 { break } kf += 1; }
                            j += kf; hold = 2;
                        } else if st != RELOC_BLK_SHORT { hold = 8; }
                        if j >= n { break }
                    }
                    if (at!(a, j) < at!(a, j - 1) || (ties != 0 && at!(a, j) == at!(a, j - 1)))
                        && !(j - i >= RELOC_MIN_BODY && (at!(a, j - 1) - at!(a, j)) * (j - i) as f64 > RELOC_GAP_FACTOR * (at!(a, i) - at!(a, j - 1))) { j += 1; if hold != 0 { hold -= 1; } }
                    else if (*w).kink != 0 && j + 1 < n && j >= i + 2 && at!(a, j) > at!(a, j - 1) && at!(a, j) < at!(a, j - 2) && at!(a, j - 1) > at!(a, j + 1) {   // the same kink in a run that lies descending
                        let x = at!(a, j); at!(a, j) = at!(a, j - 1); at!(a, j - 1) = x; (*w).kinks += 1;
                    }
                    else { break }
                }
                if (*w).twist != 0 { if j - i >= 2 { tw = RUN_TWIST; } }   // the twist: the run stays as it lies (a NaN's run of one has no direction)
                else {
                    let (mut l, mut r) = (i, j - 1);
                    while l < r { let t = at!(a, l); at!(a, l) = at!(a, r); at!(a, r) = t; l += 1; r -= 1; }
                }
            }
        }
        // A RUN OF ONE OR TWO, in stray mode, is strays ... unless it touches a chunk boundary inside the array: the join's
        // boundary repairs ask for that run. Never a NaN: it keeps the route it had.
        if (*w).stray_cap != 0 && j - i <= 2 && !(j == n && (*w).last == 0) && !(i == (*w).lo && (*w).lo > 0) && at!(a, i) == at!(a, i) && at!(a, j - 1) == at!(a, j - 1) && stray_room(w, j - i) != 0 {
            for q in i..j { at!((*w).stray, (*w).nstray) = q; (*w).nstray += 1; }
            i = j; continue;
        }
        at!((*w).runs, 2 * (*w).R) = i; at!((*w).runs, 2 * (*w).R + 1) = (j - 1) | tw; (*w).R += 1;
        let seen: usize = j - (*w).lo;
        // strays arriving faster than the cap allows: past a thirty-second of the array the density is known well enough to stop now
        if (*w).stray_cap != 0 && seen > ((*w).hi - (*w).lo) / 32 && (*w).nstray * (*w).sdiv > seen { (*(*w).abort).store(1, SeqCst); return null_mut() }
        if (*w).R >= (*w).cap || ((*w).R >= 256 && seen < (*w).R * g_reloc_avg) || (*(*w).abort).load(SeqCst) != 0 { (*(*w).abort).store(1, SeqCst); return null_mut() }
        if (*w).R == next_check {
            let mut d: *mut rdesc_t = null_mut();
            let mut ch: *mut usize = null_mut();
            let mut nc: usize = 0;
            let mut merged: usize = 0;
            if runs_plan(a, (*w).runs, (*w).R, &mut d, &mut ch, &mut nc, &mut merged) != 0 {
                TRACE!("gate [{}..{}) R={} seen={} chains={} merged={}", (*w).lo, (*w).hi, (*w).R, seen, nc, merged);
                // Not before RELOC_CHECK_MIN_SEEN elements (LOGOS_RELOC_CHECK_EARLY unset)
                let mut stop: i32 = (merged > seen / 2 && seen >= RELOC_CHECK_MIN_SEEN) as i32;
                // chains cover most of this prefix. J5 stops here; the knot route stops only if what CROSSES is too much
                if stop != 0 && (*w).kfrac > 0.0 && seen >= KNOT_MIN_AVG_RUN * (*w).R {
                    let mut K: knot_plan_t = core::mem::zeroed();
                    if knot_plan(a, d, (*w).R, seen + 1, 0, &mut K) != 0 && K.cost as f64 <= (*w).kfrac * seen as f64 { stop = 0; }
                }
                free(d); free(ch);
                if stop != 0 { (*(*w).abort).store(1, SeqCst); return null_mut() }
            }
            next_check <<= 1;
        }
        i = j;
    }
    null_mut()
}

/// logos_twist_last(4): kinks the detector swapped back, summed over the scans since it was last read
pub(crate) static mut g_kink_count: usize = 0;

/// Flat [start,end,...] list of the whole array's runs (malloc'd), or NULL when a thread's gate said "not block-permuted".
pub(crate) unsafe fn detect_runs_parallel(a: *mut f64, n: usize, mut P: i32, n_runs: *mut usize, kfrac: f64, twist: i32, fast: i32, kink: i32, pstray: *mut *mut usize, pnstray: *mut usize) -> *mut usize {
    let mut run_cap: usize = n / g_reloc_avg;
    if run_cap < MAX_RUNS_FOR_MERGE { run_cap = MAX_RUNS_FOR_MERGE; }
    if n < MIN_PARALLEL { P = 1; }
    let chunk: usize = (n + P as usize - 1) / P as usize;
    let cap: usize = run_cap / P as usize + 2;
    let w = calloc(P as usize, size_of::<rdet_arg_t>()) as *mut rdet_arg_t;
    let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let abort_flag = AtomicI32::new(0);
    *n_runs = 0;
    let ok: i32 = 1;
    for t in 0..P as usize {
        let wt = w.add(t);
        (*wt).a = a; (*wt).lo = t * chunk; (*wt).hi = if (*wt).lo + chunk > n { n } else { (*wt).lo + chunk }; (*wt).cap = cap;
        (*wt).abort = &abort_flag as *const AtomicI32 as *mut AtomicI32; (*wt).kfrac = kfrac; (*wt).twist = twist; (*wt).fast = fast; (*wt).kink = kink;
        (*wt).ties = g_desc_ties; (*wt).simd = if g_scan_scalar != 0 { 0 } else { logos_simd_kind() }; (*wt).kinks = 0; (*wt).last = (t as i32 == P - 1) as i32;
        (*wt).runs = malloc(2 * cap * size_of::<usize>()) as *mut usize;
        (*wt).stray = null_mut(); (*wt).nstray = 0; (*wt).stray_cap = 0;
    }
    if !pstray.is_null() { *pstray = null_mut(); *pnstray = 0; }
    // every worker may lift strays: each chunk gets its share of the budget, and the join concatenates the lists.
    // the cap is a share of each chunk: 1/16 at one thread, 1/64 at P > 1
    if ok != 0 && !pstray.is_null() {
        let sdiv: usize = if P > 1 { STRAY_DIV_PAR } else { g_stray_div };
        for t in 0..P as usize { let wt = w.add(t); (*wt).sdiv = sdiv; (*wt).stray_cap = ((*wt).hi - (*wt).lo) / sdiv + 1; }
    }
    if ok != 0 {
        // chunk 0 first, on the caller's thread: interleaved data fails its gate within a few runs
        reloc_detect_worker(w as *mut c_void);
        if abort_flag.load(SeqCst) == 0 {
            par_run(th, P, reloc_detect_worker, w as *mut c_void, size_of::<rdet_arg_t>(), 0);
        }
    }
    let mut runs: *mut usize = null_mut();
    let mut R: usize = 0;
    let mut bkinks: usize = 0;                                     // bkinks: kinks repaired ON chunk boundaries, at the join
    if ok != 0 && abort_flag.load(SeqCst) == 0 {
        let mut total: usize = 0;
        for t in 0..P as usize { total += (*w.add(t)).R; }
        runs = malloc(2 * (total + 1) * size_of::<usize>()) as *mut usize;
        for t in 0..P as usize {
            for k in 0..(*w.add(t)).R {
                let s: usize = at!((*w.add(t)).runs, 2 * k);
                let mut ev: usize = at!((*w.add(t)).runs, 2 * k + 1);
                let mut rv: i32 = run_rev(ev);
                let mut stitched: i32 = 0;
                // A KINK THAT BEGINS A CHUNK: the worker cannot test its first pair, so it emits the swapped pair as a
                // TWO-ELEMENT run lying the other way. If that pair, swapped back, fits between what precedes and what
                // follows, it is a kink: swap it, flip the run, and let the stitch have it. Both orientations.
                if kink != 0 && R > 0 && run_end(at!(runs, 2 * R - 1)) + 1 == s && run_end(ev) == s + 1 && rv != run_rev(at!(runs, 2 * R - 1)) && s >= 1 && s + 2 < n {
                    let kk: i32 = if rv == 0 { (at!(a, s) < at!(a, s + 1) && at!(a, s) >= at!(a, s + 2) && at!(a, s + 1) <= at!(a, s - 1)) as i32 }   // an ascending pair inside a descending strand
                                  else { (at!(a, s) > at!(a, s + 1) && at!(a, s) <= at!(a, s + 2) && at!(a, s + 1) >= at!(a, s - 1)) as i32 };        // a descending pair inside an ascending strand
                    if kk != 0 {
                        let x = at!(a, s); at!(a, s) = at!(a, s + 1); at!(a, s + 1) = x; bkinks += 1;
                        rv = (rv == 0) as i32;
                        ev = if rv != 0 { ev | RUN_TWIST } else { run_end(ev) };
                    }
                }
                // stitch across the chunk boundary: two pieces of one run, lying the same way
                if R > 0 && run_end(at!(runs, 2 * R - 1)) + 1 == s && run_rev(at!(runs, 2 * R - 1)) == rv {
                    let p0: usize = at!(runs, 2 * R - 2);
                    // A KINK ON THE BOUNDARY: the one place that sees both sides. The detector's own test, both orientations.
                    if kink != 0 && s >= 2 && s + 1 < n {
                        let kk: i32 = if rv == 0 { (at!(a, s) < at!(a, s - 1) && at!(a, s) >= at!(a, s - 2) && at!(a, s - 1) <= at!(a, s + 1)) as i32 }
                                      else { (at!(a, s) > at!(a, s - 1) && at!(a, s) < at!(a, s - 2) && at!(a, s - 1) > at!(a, s + 1)) as i32 };   // strict, as the worker's descending arm is
                        if kk != 0 { let x = at!(a, s); at!(a, s) = at!(a, s - 1); at!(a, s - 1) = x; bkinks += 1; }
                    }
                    if rv == 0 { stitched = (at!(a, s) >= at!(a, s - 1) && reloc_gap_split(a, p0, s - 1, s) == 0) as i32; }
                    else {
                        stitched = ((at!(a, s) < at!(a, s - 1) || (g_desc_ties != 0 && at!(a, s) == at!(a, s - 1)))
                            && !(s - p0 >= RELOC_MIN_BODY && (at!(a, s - 1) - at!(a, s)) * (s - p0) as f64 > RELOC_GAP_FACTOR * (at!(a, p0) - at!(a, s - 1)))) as i32;
                    }
                }
                if stitched != 0 { at!(runs, 2 * R - 1) = ev; }
                else { at!(runs, 2 * R) = s; at!(runs, 2 * R + 1) = ev; R += 1; }
            }
        }
    }
    // A KINK AT EITHER END OF THE ARRAY: the pair (0, 1) has no left neighbour to ask and the pair (n - 2, n - 1) no right
    // one, and neither is ever repaired by the worker. Ask only the neighbour that exists.
    if !runs.is_null() && kink != 0 && R >= 2 && n >= 4 {
        if run_rev(at!(runs, 1)) != 0 && at!(runs, 0) == 0 && run_end(at!(runs, 1)) == 1 && run_rev(at!(runs, 3)) == 0 && at!(runs, 2) == 2 && at!(a, 0) > at!(a, 1) && at!(a, 0) <= at!(a, 2) {
            let x = at!(a, 0); at!(a, 0) = at!(a, 1); at!(a, 1) = x; bkinks += 1;
            at!(runs, 2) = 0; memmove(runs, runs.add(2), 2 * (R - 1) * size_of::<usize>()); R -= 1;     // run 1 now starts at 0
        }
        if R >= 2 {
            let L: usize = at!(runs, 2 * R - 2);
            let E: usize = run_end(at!(runs, 2 * R - 1));
            let pe: usize = run_end(at!(runs, 2 * R - 3));
            if run_rev(at!(runs, 2 * R - 3)) == 0 && L == n - 1 && E == n - 1 && pe == n - 2 && at!(a, n - 1) < at!(a, n - 2) && at!(a, n - 1) >= at!(a, n - 3) {
                let x = at!(a, n - 1); at!(a, n - 1) = at!(a, n - 2); at!(a, n - 2) = x; bkinks += 1;
                at!(runs, 2 * R - 3) = n - 1; R -= 1;              // the ascending run now ends at n - 1: no twist bit
            }
        }
    }
    if !runs.is_null() && !pstray.is_null() {
        let mut S: usize = 0;
        for t in 0..P as usize { S += (*w.add(t)).nstray; }
        if S != 0 && P == 1 { *pstray = (*w).stray; *pnstray = S; (*w).stray = null_mut(); }
        else if S != 0 {       // P lists, each in position order and every chunk's below the next's: the concatenation IS the sorted list
            let sp = malloc(S * size_of::<usize>()) as *mut usize;
            let mut q: usize = 0;
            for t in 0..P as usize {
                let wt = w.add(t);
                if (*wt).nstray != 0 { memcpy(sp.add(q), (*wt).stray, (*wt).nstray * size_of::<usize>()); }
                q += (*wt).nstray;
            }
            *pstray = sp; *pnstray = S;
        }
    }
    for t in 0..P as usize { let wt = w.add(t); g_kink_count += (*wt).kinks; free((*wt).runs); free((*wt).stray); }
    g_kink_count += bkinks;
    free(w); free(th);
    *n_runs = R;
    runs
}

// ---- placements stream ----------------------------------------------------------------------------------------------
pub(crate) const PLACE_NT_MIN_N: usize = 1 << 22;       // 32 MB of doubles, past the 24 MB last-level cache
pub(crate) const PLACE_NT_MIN_K: usize = 512;
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn place_stream(dst: *mut f64, src: *const f64, k: usize) {
    let mut i: usize = 0;
    while i < k && ((dst.add(i) as usize) & 31) != 0 { at!(dst, i) = at!(src, i); i += 1; }
    while i + 16 <= k {
        _mm256_stream_pd(dst.add(i), _mm256_loadu_pd(src.add(i)));
        _mm256_stream_pd(dst.add(i + 4), _mm256_loadu_pd(src.add(i + 4)));
        _mm256_stream_pd(dst.add(i + 8), _mm256_loadu_pd(src.add(i + 8)));
        _mm256_stream_pd(dst.add(i + 12), _mm256_loadu_pd(src.add(i + 12)));
        i += 16;
    }
    while i + 4 <= k { _mm256_stream_pd(dst.add(i), _mm256_loadu_pd(src.add(i))); i += 4; }
    while i < k { at!(dst, i) = at!(src, i); i += 1; }
}
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn place_fence() { _mm_sfence(); }
/// LOGOS_PLACE_PLAIN unset.
pub(crate) unsafe fn place_nt_ok(n: usize) -> i32 { (n >= PLACE_NT_MIN_N && is_x86_feature_detected!("avx2")) as i32 }
#[inline]
pub(crate) unsafe fn place_copy(dst: *mut f64, src: *const f64, k: usize, nt: i32) {
    if nt != 0 && k >= PLACE_NT_MIN_K { place_stream(dst, src, k); } else { memcpy(dst, src, k * size_of::<f64>()); }
}
// for half_place (above the relocation code in the C, so declared there)
pub(crate) unsafe fn half_copy(dst: *mut f64, src: *const f64, k: usize, nt: i32) { place_copy(dst, src, k, nt); }
pub(crate) unsafe fn half_fence(nt: i32) { if nt != 0 { place_fence(); } }
pub(crate) unsafe fn half_nt_ok(n: usize) -> i32 { place_nt_ok(n) }

/// orphan: the caller only. Tiles of workers whose thread could not be created; nt: placements stream (place_copy)
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct reloc_arg_t {
    pub(crate) a: *mut f64, pub(crate) dst: *mut f64,
    pub(crate) n: usize, pub(crate) R: usize, pub(crate) nc: usize,
    pub(crate) P: i32, pub(crate) t: i32,
    pub(crate) d: *const rdesc_t,
    pub(crate) off: *const usize, pub(crate) ch: *const usize, pub(crate) rl: *const usize,
    pub(crate) next: *mut AtomicUsize,
    pub(crate) bar: *mut barrier_t,
    pub(crate) orphan: *const u8,
    pub(crate) copyback: i32,
    pub(crate) nt: i32,
}
macro_rules! TILE_IS_MINE { ($w:expr, $q:expr) => { ($q == (*$w).t || (!(*$w).orphan.is_null() && *(*$w).orphan.add($q as usize) != 0)) }; }

pub(crate) unsafe fn reloc_worker(p: *mut c_void) -> *mut c_void {
    let w = p as *mut reloc_arg_t;
    let n: usize = (*w).n;
    let P: i32 = (*w).P;
    // phase 1: fill dst[lo,hi) from the pieces of runs that land there (balanced, sequential streams)
    for tq in 0..P {
        if !TILE_IS_MINE!(w, tq) { continue }
        let lo: usize = tq as usize * n / P as usize;
        let hi: usize = (tq + 1) as usize * n / P as usize;
        let mut q: usize = 0;
        while q < (*w).R && at!((*w).off, q) + (*(*w).d.add(q)).len <= lo { q += 1; }
        while q < (*w).R && at!((*w).off, q) < hi {
            let dq = &*(*w).d.add(q);
            let offq = at!((*w).off, q);
            let ds: usize = if offq > lo { offq } else { lo };
            let de: usize = if offq + dq.len < hi { offq + dq.len } else { hi };
            let o: usize = ds - offq;                              // the run's logical range [o, o+cnt): a twisted run holds it at its far end
            let cnt: usize = de - ds;
            if dq.rev == 0 { place_copy((*w).dst.add(ds), (*w).a.add(dq.start + o), cnt, (*w).nt); }
            else { copy_rev((*w).dst.add(ds), (*w).a.add(dq.start + (dq.len - o - cnt)), cnt); }
            q += 1;
        }
    }
    if (*w).nt != 0 { place_fence(); }                            // the streamed bytes before the barrier that hands them on
    if P > 1 { barrier_wait((*w).bar); }
    // phase 2: merge inside multi-run chains; chains are disjoint dst ranges and a[] is free scratch now
    loop {
        let k: usize = (*(*w).next).fetch_add(1, SeqCst);
        if !(k < (*w).nc) { break }
        let k0: usize = at!((*w).ch, k);
        let k1: usize = at!((*w).ch, k + 1);
        if k1 - k0 > 1 { merge_all_runs((*w).dst, (*w).a, (*w).rl.add(2 * k0), k1 - k0); }
    }
    if P > 1 { barrier_wait((*w).bar); }
    // phase 3: copy back
    if (*w).copyback != 0 {
        for tq in 0..P {
            if !TILE_IS_MINE!(w, tq) { continue }
            let lo: usize = tq as usize * n / P as usize;
            let hi: usize = (tq + 1) as usize * n / P as usize;
            place_copy((*w).a.add(lo), (*w).dst.add(lo), hi - lo, (*w).nt);
        }
    }
    if (*w).nt != 0 { place_fence(); }
    null_mut()
}

// ---- the knot route's parallel placement (plan and rationale: see knot_plan) ---------------------------------------
/// orphan: as in reloc_arg_t; copyback: 0 when the caller merges the output back itself; nt: as in reloc_arg_t
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct knot_arg_t {
    pub(crate) a: *mut f64, pub(crate) dst: *mut f64,
    pub(crate) n: usize,
    pub(crate) P: i32, pub(crate) t: i32,
    pub(crate) K: *const knot_plan_t,
    pub(crate) rl: *const usize,
    pub(crate) nx2: *mut AtomicUsize, pub(crate) nx3: *mut AtomicUsize,
    pub(crate) bar: *mut barrier_t,
    pub(crate) orphan: *const u8,
    pub(crate) copyback: i32,
    pub(crate) nt: i32,
}
pub(crate) unsafe fn knot_worker(p: *mut c_void) -> *mut c_void {
    let w = p as *mut knot_arg_t;
    let K: *const knot_plan_t = (*w).K;
    let n: usize = (*w).n;
    let P: i32 = (*w).P;
    // A1: this thread's tile of the output, from the copy pieces that land in it (balanced, sequential streams)
    for tq in 0..P {
        if !TILE_IS_MINE!(w, tq) { continue }
        let lo: usize = tq as usize * n / P as usize;
        let hi: usize = (tq + 1) as usize * n / P as usize;
        let mut L: usize = 0;
        let mut H: usize = (*K).npc;
        while L < H { let m = L + ((H - L) >> 1); if (*(*K).pc.add(m)).dst + (*(*K).pc.add(m)).len <= lo { L = m + 1; } else { H = m; } }
        let mut q: usize = L;
        while q < (*K).npc && (*(*K).pc.add(q)).dst < hi {
            let pc: *const kpiece_t = (*K).pc.add(q);
            q += 1;
            if (*pc).copy == 0 { continue }
            let ds: usize = if (*pc).dst > lo { (*pc).dst } else { lo };
            let de: usize = if (*pc).dst + (*pc).len < hi { (*pc).dst + (*pc).len } else { hi };
            let o: usize = ds - (*pc).dst;
            let cnt: usize = de - ds;
            if (*pc).rev == 0 { place_copy((*w).dst.add(ds), (*w).a.add((*pc).src + o), cnt, (*w).nt); }
            else { copy_rev((*w).dst.add(ds), (*w).a.add((*pc).src + ((*pc).len - o - cnt)), cnt); }
        }
    }
    // A2: two-strand crossings, merged straight from a[] into place; a[] is only read until the barrier
    let mut gl: usize = 0;
    loop {
        let s: usize = (*(*w).nx2).fetch_add(1, SeqCst);
        if !(s < (*K).nsg) { break }
        let g: *const kseg_t = (*K).sg.add(s);
        if (*g).k != 2 { continue }
        let x: *const kpiece_t = (*K).pc.add((*g).p0);
        let y: *const kpiece_t = x.add(1);
        gl += knot_merge2x((*w).a.add((*x).src), (*x).len, (*x).rev, (*w).a.add((*y).src), (*y).len, (*y).rev, (*w).dst.add((*g).dst));
    }
    if gl != 0 { g_knot_gallops.fetch_add(gl, Relaxed); }
    if (*w).nt != 0 { place_fence(); }
    if P > 1 { barrier_wait((*w).bar); }
    // B: crossings of three or more strands, staged side by side in dst; a[] is free scratch now
    if (*K).n3 != 0 {
        loop {
            let s: usize = (*(*w).nx3).fetch_add(1, SeqCst);
            if !(s < (*K).nsg) { break }
            let g: *const kseg_t = (*K).sg.add(s);
            if (*g).k >= 3 && merge_runs_vec((*w).dst, (*w).a, (*w).rl.add(2 * (*g).p0), (*g).k) == 0 { merge_all_runs((*w).dst, (*w).a, (*w).rl.add(2 * (*g).p0), (*g).k); }
        }
        if P > 1 { barrier_wait((*w).bar); }
    }
    // C: copy back
    if (*w).copyback != 0 {
        for tq in 0..P {
            if !TILE_IS_MINE!(w, tq) { continue }
            let lo: usize = tq as usize * n / P as usize;
            let hi: usize = (tq + 1) as usize * n / P as usize;
            place_copy((*w).a.add(lo), (*w).dst.add(lo), hi - lo, (*w).nt);
        }
    }
    if (*w).nt != 0 { place_fence(); }
    null_mut()
}

// (the literal linked-list placement, C lines 6548-6667, is opt-in by LOGOS_KNOT_LIST and is not ported)

// ---- strays inside strands ------------------------------------------------------------------------------------------
/// logos_twist_last(10): strays lifted by routes that completed; calling thread
pub(crate) static mut g_stray_count: usize = 0;
pub(crate) unsafe fn stray_compact(a: *mut f64, runs: *mut usize, R: usize, sp: *const usize, S: usize, st: *mut f64) {
    let mut w: usize = 0;
    let mut q: usize = 0;                                          // one pass, left to right: every write lands below every unread element
    for k in 0..R {
        let s: usize = at!(runs, 2 * k);
        let e: usize = run_end(at!(runs, 2 * k + 1));
        let tw: usize = at!(runs, 2 * k + 1) & RUN_TWIST;
        let ns: usize = w;
        let mut p: usize = s;
        while q < S && at!(sp, q) < s { at!(st, q) = at!(a, at!(sp, q)); q += 1; }                 // strays between runs
        while q < S && at!(sp, q) <= e {
            let len = at!(sp, q) - p;
            memmove(a.add(w), a.add(p), len * size_of::<f64>()); w += len;
            at!(st, q) = at!(a, at!(sp, q)); p = at!(sp, q) + 1; q += 1;
        }
        memmove(a.add(w), a.add(p), (e + 1 - p) * size_of::<f64>()); w += e + 1 - p;
        at!(runs, 2 * k) = ns; at!(runs, 2 * k + 1) = (w - 1) | tw;
    }
    while q < S { at!(st, q) = at!(a, at!(sp, q)); q += 1; }
}
/// the inverse, when the plan declines after the holes were closed: expand from the right, strays back at their positions
pub(crate) unsafe fn stray_restore(a: *mut f64, n: usize, sp: *const usize, S: usize, st: *const f64) {
    let mut w: usize = n - S;
    let mut q: usize = S;
    let mut p: usize = n;
    while p > 0 {
        p -= 1;
        if q > 0 && at!(sp, q - 1) == p { q -= 1; at!(a, p) = at!(st, q); } else { w -= 1; at!(a, p) = at!(a, w); }
    }
}
/// the strays sorted, then merged with the placed n - S elements of dst into a: this pass replaces the copy-back
pub(crate) unsafe fn stray_finish(a: *mut f64, dst: *const f64, m: usize, st: *mut f64, S: usize) {
    NESTED_SORT!(psort_f64(st, S, 1, null_mut(), null_mut(), null_mut(), null_mut()));
    let mut i: usize = 0;
    let mut o: usize = 0;
    for q in 0..S {
        let v = at!(st, q);
        let mut lo = i;
        let mut hi = m;
        while lo < hi { let mid = lo + ((hi - lo) >> 1); if at!(dst, mid) < v { lo = mid + 1; } else { hi = mid; } }
        if lo > i { memcpy(a.add(o), dst.add(i), (lo - i) * size_of::<f64>()); o += lo - i; i = lo; }
        at!(a, o) = v; o += 1;
    }
    if i < m { memcpy(a.add(o), dst.add(i), (m - i) * size_of::<f64>()); }
}

/// ONE STRAND, STRAYS PUT BACK IN PLACE: between two events (a hole, a stray's insertion point) every element moves by
/// the SAME amount, and the array is a list of segments, each one memmove.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct sseg_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize }
pub(crate) unsafe fn strand_place_strays(a: *mut f64, n: usize, sp: *const usize, S: usize, T: *const f64, sg: *mut sseg_t, tpos: *mut usize) {
    let mut ns: usize = 0;
    let mut ti: usize = 0;
    let mut cb: usize = 0;
    for k in 0..=S {                                               // piece k: between hole k-1 and hole k
        let lo: usize = if k != 0 { at!(sp, k - 1) + 1 } else { 0 };
        let hi: usize = if k < S { at!(sp, k) } else { n };
        let mut p: usize = lo;
        if hi <= lo { continue }
        while ti < S && p < hi && at!(T, ti) <= at!(a, hi - 1) {   // this stray sorts before some element of this piece
            let mut L = p;
            let mut R = hi;
            let v = at!(T, ti);
            while L < R { let m = L + ((R - L) >> 1); if at!(a, m) < v { L = m + 1; } else { R = m; } }
            if L > p { (*sg.add(ns)).src = p; (*sg.add(ns)).len = L - p; (*sg.add(ns)).dst = cb + (p - lo) + ti; ns += 1; }
            at!(tpos, ti) = cb + (L - lo) + ti; ti += 1; p = L;
        }
        if hi > p { (*sg.add(ns)).src = p; (*sg.add(ns)).len = hi - p; (*sg.add(ns)).dst = cb + (p - lo) + ti; ns += 1; }
        cb += hi - lo;
    }
    while ti < S { at!(tpos, ti) = cb + ti; ti += 1; }             // above every strand element
    for i in 0..ns { let g = *sg.add(i); if g.dst < g.src { memmove(a.add(g.dst), a.add(g.src), g.len * size_of::<f64>()); } }
    let mut i: usize = ns;
    while i > 0 { i -= 1; let g = *sg.add(i); if g.dst > g.src { memmove(a.add(g.dst), a.add(g.src), g.len * size_of::<f64>()); } }
    for i in 0..S { at!(a, at!(tpos, i)) = at!(T, i); }
}

/// Returns 1 with a[] sorted, or 0 (a[] still a permutation of the input; the caller continues on the normal path).
/// dst, or lz to take it when something will be placed.
pub(crate) unsafe fn relocate_try_f64(a: *mut f64, n: usize, mut P: i32, mut dst: *mut f64, lz: *mut scratch_t) -> i32 {
    g_reloc_avg = if P > 1 && n >= MIN_PARALLEL { RELOC_MIN_AVG_RUN_PAR } else { RELOC_MIN_AVG_RUN_SEQ };     // LOGOS_RELOC_AVG unset
    let kpar: i32 = (P > 1 && n >= MIN_PARALLEL) as i32;
    let kfrac: f64 = if g_knot_on != 0 { if kpar != 0 { g_knot_frac_par } else { g_knot_frac_seq } } else { 0.0 };
    let kfew: f64 = if kpar != 0 { kfrac } else { g_knot_frac_few };
    if reloc_sample_gate(a, n, kfrac, kfew) == 0 { TRACE!("reloc: sample gate declined"); return 0 }
    let twist: i32 = 1;                                            // LOGOS_NO_TWIST unset
    // g_twist_generic = twist && getenv("LOGOS_TWIST_GENERIC") != NULL: 0 with the switch unset, a const in the knot file
    let fast: i32 = 2;                                             // LOGOS_NO_FASTSCAN, LOGOS_FASTSCAN_HOLD8 unset
    let kink: i32 = 1;                                             // LOGOS_NO_KINK unset
    let mut R: usize = 0;
    let mut sp: *mut usize = null_mut();
    let mut S: usize = 0;
    let stray_on: i32 = (g_stray_on != 0 && (P == 1 || n < MIN_PARALLEL || g_stray_par != 0) && ns_depth.with(|d| d.get()) == 0) as i32;
    let runs: *mut usize = detect_runs_parallel(a, n, P, &mut R, kfrac, twist, fast, kink, if stray_on != 0 { &mut sp } else { null_mut() }, &mut S);
    TRACE!("reloc: detect_runs_parallel -> {}, R={}, strays={}", if !runs.is_null() { "runs" } else { "bail" }, R, S);
    #[cfg(feature = "trace")]
    {
        // a few runs: print them, so a strand that should be one run and is not says WHERE it broke
        if !runs.is_null() && R <= 8 {
            for k in 0..R { TRACE!("reloc:   run {}: [{} .. {}] {}", k, at!(runs, 2 * k), run_end(at!(runs, 2 * k + 1)), if run_rev(at!(runs, 2 * k + 1)) != 0 { "descending" } else { "ascending" }); }
        }
    }
    if runs.is_null() { free(sp); return 0 }
    // ONE STRAND: one run and no strays is a sorted array (or its mirror); one run with strays is placed by the strays' own merge.
    if R == 1 && g_onerun_off == 0 {
        let s0: usize = at!(runs, 0);
        let e0: usize = run_end(at!(runs, 1));
        let rv0: i32 = run_rev(at!(runs, 1));
        if S == 0 && s0 == 0 && e0 == n - 1 {
            if rv0 != 0 { let (mut l, mut r) = (0usize, n - 1); while l < r { let x = at!(a, l); at!(a, l) = at!(a, r); at!(a, r) = x; l += 1; r -= 1; } }
            TRACE!("reloc: one run, {}: done", if rv0 != 0 { "descending" } else { "ascending" });
            free(runs); g_onerun_count += 1;
            return 1;
        }
        if S != 0 && S < n {
            let st1 = malloc(S * size_of::<f64>()) as *mut f64;
            if rv0 == 0 && g_onerun_copy == 0 {                    // in place: one memmove per segment (strand_place_strays)
                let sg = malloc((2 * S + 2) * size_of::<sseg_t>()) as *mut sseg_t;
                let tpos = malloc(S * size_of::<usize>()) as *mut usize;
                for q in 0..S { at!(st1, q) = at!(a, at!(sp, q)); }
                NESTED_SORT!(psort_f64(st1, S, 1, null_mut(), null_mut(), null_mut(), null_mut()));
                strand_place_strays(a, n, sp, S, st1, sg, tpos);
                TRACE!("reloc: one run and {} strays: put back in place", S);
                g_stray_count += S; g_onerun_count += 1;
                free(sg); free(tpos); free(st1); free(sp); free(runs);
                return 1;
            }
            if !dst.is_null() || { dst = scratch_get(lz); !dst.is_null() } {
                stray_compact(a, runs, 1, sp, S, st1);            // the strand now fills [0, n - S)
                strand_copy(dst, a, n - S, rv0);                   // the merge reads it from scratch and writes the array
                stray_finish(a, dst, n - S, st1, S);
                TRACE!("reloc: one run and {} strays: merged", S);
                g_stray_count += S; g_onerun_count += 1;
                free(st1); free(sp); free(runs);
                return 1;
            }
            free(st1);                                             // no scratch for the three passes: declined below
        }
    }
    if R < 2 { free(runs); free(sp); return 0 }
    // strays: close the holes before the plan reads the runs; a decline below puts them back into the holes
    let mut st: *mut f64 = null_mut();
    let mut n_eff: usize = n;
    if S != 0 {
        st = malloc(S * size_of::<f64>()) as *mut f64;
        stray_compact(a, runs, R, sp, S, st); n_eff = n - S;
    }
    macro_rules! STRAY_BAIL { () => {{ if S != 0 { stray_restore(a, n, sp, S, st); } free(st); free(sp); }}; }
    let cb: i32 = (g_reloc_nocopy == 0 && S == 0) as i32;          // the placements copy back themselves unless the strays' merge does it
    let mut d: *mut rdesc_t = null_mut();
    let mut ch: *mut usize = null_mut();
    let mut nc: usize = 0;
    let mut merged: usize = 0;
    let ok: i32 = runs_plan(a, runs, R, &mut d, &mut ch, &mut nc, &mut merged);
    free(runs);
    if ok == 0 { STRAY_BAIL!(); return 0 }
    let mut ntw: usize = 0;
    for q in 0..R { ntw += (*d.add(q)).rev as usize; }
    TRACE!("reloc: plan chains={} merged={} (gate n/2={})", nc, merged, n / 2);
    if kfrac > 0.0 && n >= KNOT_MIN_AVG_RUN * R {
        let mut Pk: i32 = if n < MIN_PARALLEL { 1 } else { P };
        let mut grain: usize = n + 1;
        if Pk > 1 { grain = n / (Pk as usize * 8); if grain < KNOT_GRAIN_MIN { grain = KNOT_GRAIN_MIN; } }
        let mut K: knot_plan_t = core::mem::zeroed();
        if knot_plan(a, d, R, grain, 1, &mut K) != 0 {
            // Thread policy: the cap counts work, not elements (a merge is compute-bound, about three copies' worth per element per level).
            { let work: usize = n + KNOT_MERGE_WEIGHT * K.cost; let mut pm: usize = work / RELOC_MIN_PER_THREAD; if pm < 1 { pm = 1; } if Pk as usize > pm { Pk = pm as i32; } }
            let kf: f64 = if R > MERGE_MAX_RUNS { kfrac } else { kfew };
            let mut go: i32 = (K.total == n_eff && (K.cost as f64 <= kf * n as f64 || merged <= n / 2)) as i32;
            if go != 0 && dst.is_null() && { dst = scratch_get(lz); dst.is_null() } { go = 0; }     // the scratch, now that something will be placed
            TRACE!("knot: R={} chains={} in-chains={} | crossing={} cost={} kmax={} pieces={} merges={} (k>=3: {}) gate {:.2} -> {}",
                   R, nc, merged, K.crossing, K.cost, K.kmax, K.npc, K.nsg, K.n3, kf, if go != 0 { "untie" } else { "decline" });
            let mut krl: *mut usize = null_mut();
            let mut kw: *mut knot_arg_t = null_mut();
            let mut kth: *mut pthread_t = null_mut();
            let mut korph: *mut u8 = null_mut();
            if go != 0 && K.n3 != 0 { krl = knot_rl(&K); }
            // (the literal linked list, opt-in by LOGOS_KNOT_LIST: unset, the array placement follows)
            // one thread: half the scratch (half_place). The strays are sorted first; a decline leaves them sorted, which the full
            // placement below and a bail (stray_restore puts them back into the holes in any order) both accept
            if go != 0 && Pk == 1 && g_reloc_nocopy == 0 && half_on(n_eff) != 0 {
                if S != 0 { NESTED_SORT!(psort_f64(st, S, 1, null_mut(), null_mut(), null_mut(), null_mut())); }
                if knot_place_half(a, dst, n_eff, &K, st, S) != 0 {
                    TRACE!("knot: placed with half a scratch (P=1), {} of {} strands twisted, {} strays", ntw, R, S);
                    g_twist_knot += ntw; g_stray_count += S;
                    free(st); free(sp); free(krl); knot_plan_free(&mut K); free(d); free(ch);
                    return 1;
                }
            }
            if go != 0 {
                kw = malloc(Pk as usize * size_of::<knot_arg_t>()) as *mut knot_arg_t;
                kth = malloc(Pk as usize * size_of::<pthread_t>()) as *mut pthread_t;
                korph = calloc(Pk as usize, 1);
            }
            if go != 0 {
                let mut kbar = core::mem::MaybeUninit::<barrier_t>::uninit();
                let nx2 = AtomicUsize::new(0);
                let nx3 = AtomicUsize::new(0);
                if Pk > 1 { barrier_init(kbar.as_mut_ptr(), Pk); }
                let knt: i32 = place_nt_ok(n);
                for t in 0..Pk {
                    let w = knot_arg_t { a, dst, n: n_eff, P: Pk, t, K: &K, rl: krl, nx2: &nx2 as *const AtomicUsize as *mut AtomicUsize, nx3: &nx3 as *const AtomicUsize as *mut AtomicUsize,
                                         bar: kbar.as_mut_ptr(), orphan: null(), copyback: cb, nt: knt };
                    *kw.add(t as usize) = w;
                }
                let kmade: i32 = par_spawn(kth, Pk, knot_worker, kw as *mut c_void, size_of::<knot_arg_t>(), if Pk > 1 { kbar.as_mut_ptr() } else { null_mut() }, korph);   // as in relocation
                (*kw).orphan = korph;
                knot_worker(kw as *mut c_void);
                par_join(kth, kmade);
                if Pk > 1 { barrier_destroy(kbar.as_mut_ptr()); }
                TRACE!("knot: copies + crossing merges + copyback done (P={}), {} of {} strands twisted, {} strays", Pk, ntw, R, S);
                g_twist_knot += ntw;
                if S != 0 { stray_finish(a, dst, n_eff, st, S); g_stray_count += S; }
                free(st); free(sp);
                free(kw); free(kth); free(korph); free(krl); knot_plan_free(&mut K); free(d); free(ch);
                return 1;
            }
            free(kw); free(kth); free(korph); free(krl); knot_plan_free(&mut K);
        }
    }
    if merged > n / 2 { free(d); free(ch); STRAY_BAIL!(); return 0 }
    let off = malloc(R * size_of::<usize>()) as *mut usize;
    let rl = malloc(2 * R * size_of::<usize>()) as *mut usize;
    { let mut o: usize = 0; for q in 0..R { at!(off, q) = o; at!(rl, 2 * q) = o; at!(rl, 2 * q + 1) = o + (*d.add(q)).len - 1; o += (*d.add(q)).len; } }
    // Relocation's parallel form has a serial middle (plan) and a spawn: at most one thread per RELOC_MIN_PER_THREAD elements.
    if n < MIN_PARALLEL { P = 1; }
    { let mut pm: usize = n / RELOC_MIN_PER_THREAD; if pm < 1 { pm = 1; } if P as usize > pm { P = pm as i32; } }
    let mut bar = core::mem::MaybeUninit::<barrier_t>::uninit();
    let next = AtomicUsize::new(0);
    if dst.is_null() && { dst = scratch_get(lz); dst.is_null() } { free(off); free(rl); free(d); free(ch); STRAY_BAIL!(); return 0 }   // no scratch: nothing placed
    let wa = malloc(P as usize * size_of::<reloc_arg_t>()) as *mut reloc_arg_t;
    let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let orphan: *mut u8 = calloc(P as usize, 1);
    if P > 1 { barrier_init(bar.as_mut_ptr(), P); }
    let rnt: i32 = place_nt_ok(n);
    for t in 0..P {
        let w = reloc_arg_t { a, dst, n: n_eff, R, nc, P, t, d, off, ch, rl, next: &next as *const AtomicUsize as *mut AtomicUsize, bar: bar.as_mut_ptr(), orphan: null(), copyback: cb, nt: rnt };
        *wa.add(t as usize) = w;
    }
    // static output tiles and a barrier: an orphan is dropped from the barrier and the caller fills and copies back its tile
    let rel_made: i32 = par_spawn(th, P, reloc_worker, wa as *mut c_void, size_of::<reloc_arg_t>(), if P > 1 { bar.as_mut_ptr() } else { null_mut() }, orphan);
    (*wa).orphan = orphan;
    reloc_worker(wa as *mut c_void);
    par_join(th, rel_made);
    TRACE!("reloc: copy+merge+copyback done (P={}), {} of {} runs twisted, {} strays", P, ntw, R, S);
    g_twist_reloc += ntw;
    if S != 0 { stray_finish(a, dst, n_eff, st, S); g_stray_count += S; }
    free(st); free(sp);
    if P > 1 { barrier_destroy(bar.as_mut_ptr()); }
    free(wa); free(th); free(orphan); free(off); free(rl); free(d); free(ch);
    1
}

// ---- outside this file's range: C signatures, minimal bodies where this file's tests need one ----------------------
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    // leaf.rs (C lines 1936-1970: defined ahead of the relocation code, in the leaf's range)
    pub(crate) const RELOC_MIN_AVG_RUN_SEQ: usize = 256;
    pub(crate) const RELOC_MIN_AVG_RUN_PAR: usize = MIN_AVG_RUN_FOR_MERGE;
    pub(crate) static mut g_reloc_avg: usize = RELOC_MIN_AVG_RUN_SEQ;
    pub(crate) const g_reloc_nocopy: i32 = 0;
    pub(crate) const g_stray_on: i32 = 1;
    pub(crate) const g_stray_par: i32 = 1;
    pub(crate) const STRAY_DIV_PAR: usize = 64;
    pub(crate) const RELOC_CHECK_MIN_SEEN: usize = 4096;
    pub(crate) const g_stray_div: usize = 16;
    pub(crate) const g_onerun_off: i32 = 0;
    pub(crate) const g_onerun_copy: i32 = 0;
    pub(crate) static mut g_onerun_count: usize = 0;
    pub(crate) const RELOC_MIN_PER_THREAD: usize = 500000;
    pub(crate) unsafe fn merge_all_runs(a: *mut f64, buf: *mut f64, runs: *const usize, n_runs: usize) { unimplemented!() }
    // knot.rs (C lines 1981-2738)
    pub(crate) const RUN_TWIST: usize = 1usize << (size_of::<usize>() * 8 - 1);
    #[inline(always)] pub(crate) unsafe fn run_end(e: usize) -> usize { e & !RUN_TWIST }
    #[inline(always)] pub(crate) unsafe fn run_rev(e: usize) -> i32 { ((e & RUN_TWIST) != 0) as i32 }
    pub(crate) static mut g_twist_knot: usize = 0;
    pub(crate) static mut g_twist_reloc: usize = 0;
    pub(crate) unsafe fn copy_rev(o: *mut f64, x: *const f64, cnt: usize) { for i in 0..cnt { *o.add(i) = *x.add(cnt - 1 - i); } }
    #[inline] pub(crate) unsafe fn strand_copy(o: *mut f64, x: *const f64, cnt: usize, rev: i32) { if rev != 0 { copy_rev(o, x, cnt); } else { memcpy(o, x, cnt * size_of::<f64>()); } }
    /// A stand-in that always answers 0 ("no plan"): the detector's checkpoints then never stop a scan and
    /// relocate_try_f64 declines every array of two or more runs. The real one is knot.rs's.
    pub(crate) unsafe fn runs_plan(a: *const f64, runs: *const usize, R: usize, pd: *mut *mut rdesc_t, pchain: *mut *mut usize, pnchain: *mut usize, pmerged: *mut usize) -> i32 { 0 }
    pub(crate) const KNOT_COST_MAX_SEQ: f64 = 3.0;
    pub(crate) const KNOT_COST_MAX_FEW: f64 = 3.0;
    pub(crate) const KNOT_COST_MAX_PAR: f64 = 1.0;
    pub(crate) const KNOT_GRAIN_MIN: usize = 32768;
    pub(crate) const KNOT_MIN_AVG_RUN: usize = 256;
    pub(crate) const KNOT_MERGE_WEIGHT: usize = 3;
    pub(crate) const g_knot_on: i32 = 1;
    pub(crate) const g_knot_frac_seq: f64 = KNOT_COST_MAX_SEQ;
    pub(crate) const g_knot_frac_few: f64 = KNOT_COST_MAX_FEW;
    pub(crate) const g_knot_frac_par: f64 = KNOT_COST_MAX_PAR;
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct kpiece_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize, pub(crate) copy: i32, pub(crate) rev: i32 }
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct kseg_t { pub(crate) p0: usize, pub(crate) k: usize, pub(crate) dst: usize, pub(crate) len: usize }
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct knot_plan_t {
        pub(crate) pc: *mut kpiece_t, pub(crate) npc: usize, pub(crate) cap_pc: usize,
        pub(crate) sg: *mut kseg_t, pub(crate) nsg: usize, pub(crate) cap_sg: usize,
        pub(crate) n3: usize, pub(crate) crossing: usize, pub(crate) cost: usize, pub(crate) kmax: usize, pub(crate) total: usize,
        pub(crate) last_single: i32,
    }
    pub(crate) unsafe fn knot_plan_free(K: *mut knot_plan_t) { unimplemented!() }
    pub(crate) unsafe fn knot_plan(a: *const f64, d: *const rdesc_t, R: usize, grain: usize, emit: i32, K: *mut knot_plan_t) -> i32 { unimplemented!() }
    pub(crate) static g_knot_gallops: AtomicUsize = AtomicUsize::new(0);
    pub(crate) unsafe fn knot_merge2x(x: *const f64, nx: usize, xr: i32, y: *const f64, ny: usize, yr: i32, o: *mut f64) -> usize { unimplemented!() }
    pub(crate) unsafe fn merge_runs_vec(a: *mut f64, buf: *mut f64, runs: *const usize, k: usize) -> i32 { unimplemented!() }
    pub(crate) unsafe fn knot_rl(K: *const knot_plan_t) -> *mut usize { unimplemented!() }
    pub(crate) unsafe fn knot_place_half(a: *mut f64, buf: *mut f64, N: usize, K: *const knot_plan_t, st: *const f64, S: usize) -> i32 { unimplemented!() }
    pub(crate) unsafe fn half_on(n: usize) -> i32 { unimplemented!() }
    // base.rs
    pub(crate) type pthread_t = usize;
    #[repr(C)]
    pub(crate) struct barrier_t { pub(crate) _opaque: [u64; 16] }
    pub(crate) unsafe fn barrier_init(b: *mut barrier_t, n: i32) { unimplemented!() }
    pub(crate) unsafe fn barrier_wait(b: *mut barrier_t) { unimplemented!() }
    pub(crate) unsafe fn barrier_destroy(b: *mut barrier_t) { unimplemented!() }
    /// A serial stand-in: workers 1..P-1 in turn on this thread, worker 0 first when run0 is set.
    pub(crate) unsafe fn par_run(th: *mut pthread_t, P: i32, fn_: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, run0: i32) {
        if run0 != 0 { fn_(args); }
        for t in 1..P as usize { fn_((args as *mut u8).add(t * stride) as *mut c_void); }
    }
    pub(crate) unsafe fn par_spawn(th: *mut pthread_t, P: i32, fn_: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, bar: *mut barrier_t, orphan: *mut u8) -> i32 { unimplemented!() }
    pub(crate) unsafe fn par_join(th: *mut pthread_t, made: i32) { unimplemented!() }
    pub(crate) unsafe fn scratch_get(s: *mut scratch_t) -> *mut f64 { unimplemented!() }
    pub(crate) static mut g_ctx_next: i32 = 0;
    // ord1.rs
    thread_local! { pub(crate) static ns_depth: Cell<i32> = Cell::new(0); }
    // disp.rs. A stand-in: the standard sort in total order (the callers here pass NaN-free strays).
    pub(crate) unsafe fn psort_f64(a: *mut f64, n: usize, P: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) {
        if n > 1 { core::slice::from_raw_parts_mut(a, n).sort_by(|x, y| x.total_cmp(y)); }
    }
}
#[cfg(not(feature = "port_all"))]
use missing::*;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    // the witnesses and g_reloc_avg are plain statics: one test at a time
    static LOCK: Mutex<()> = Mutex::new(());
    fn lock() -> std::sync::MutexGuard<'static, ()> { LOCK.lock().unwrap_or_else(|e| e.into_inner()) }

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn below(&mut self, k: usize) -> usize { (self.next() % k as u64) as usize }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
    }
    fn bits_sorted(a: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = a.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    fn ascending(a: &[f64]) -> bool { a.windows(2).all(|w| !(w[1] < w[0])) }

    struct Det { runs: Vec<(usize, usize, bool)>, strays: Vec<usize>, kinks: usize }
    /// the detector alone; None = it bailed
    unsafe fn detect(a: &mut [f64], P: i32, avg: usize, stray: bool) -> Option<Det> {
        g_reloc_avg = avg;
        g_kink_count = 0;
        let mut R: usize = 0;
        let mut sp: *mut usize = null_mut();
        let mut S: usize = 0;
        let runs = detect_runs_parallel(a.as_mut_ptr(), a.len(), P, &mut R, 0.0, 1, 2, 1, if stray { &mut sp } else { null_mut() }, &mut S);
        let kinks = g_kink_count;
        g_kink_count = 0;
        if runs.is_null() { free(sp); return None }
        let mut out = Det { runs: Vec::new(), strays: Vec::new(), kinks };
        for k in 0..R { out.runs.push((*runs.add(2 * k), run_end(*runs.add(2 * k + 1)), run_rev(*runs.add(2 * k + 1)) != 0)); }
        for q in 0..S { out.strays.push(*sp.add(q)); }
        free(runs); free(sp);
        Some(out)
    }
    /// the run list and the stray list together cover [0, n) once, and every run is in order its own way once the strays are skipped
    fn check_cover(a: &[f64], d: &Det) {
        let n = a.len();
        let mut is_stray = vec![false; n];
        for w in d.strays.windows(2) { assert!(w[0] < w[1], "stray list not ascending"); }
        for &p in &d.strays { is_stray[p] = true; }
        let mut next = 0usize;
        for &(s, e, rev) in &d.runs {
            while next < s { assert!(is_stray[next], "position {} is in no run and is not a stray", next); next += 1; }
            assert!(s == next && e >= s && e < n, "run [{}, {}] does not follow position {}", s, e, next);
            let vals: Vec<f64> = (s..=e).filter(|&p| !is_stray[p]).map(|p| a[p]).collect();
            for w in vals.windows(2) { if rev { assert!(!(w[1] > w[0]), "descending run [{}, {}] ascends", s, e); } else { assert!(!(w[1] < w[0]), "ascending run [{}, {}] descends", s, e); } }
            next = e + 1;
        }
        while next < n { assert!(is_stray[next], "position {} is in no run and is not a stray", next); next += 1; }
    }

    /// nb shuffled blocks of len elements 1e6 apart in value; `desc(b)` says which lie descending. Returns (array, block order).
    fn blocks(nb: usize, len: usize, seed: u64, desc: &dyn Fn(usize) -> bool) -> Vec<f64> {
        let mut rng = Rng(seed);
        let mut order: Vec<usize> = (0..nb).collect();
        for i in (1..nb).rev() { let j = rng.below(i + 1); order.swap(i, j); }
        let mut a = Vec::with_capacity(nb * len);
        for (slot, &b) in order.iter().enumerate() {
            let base = b as f64 * 1e6;
            for i in 0..len { let v = base + i as f64 + 0.5; a.push(v); }
            if desc(slot) { let s = slot * len; a[s..s + len].reverse(); }
        }
        a
    }

    #[test]
    fn gap_split_and_place_copy() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let a: Vec<f64> = (0..32).map(|i| i as f64).chain([1e9]).collect();
            assert_eq!(reloc_gap_split(a.as_ptr(), 0, 31, 32), 1);
            assert_eq!(reloc_gap_split(a.as_ptr(), 0, 30, 31), 0);
            assert_eq!(reloc_gap_split(a.as_ptr(), 20, 31, 32), 0);       // a body under RELOC_MIN_BODY has no gap test
            assert_eq!(reloc_gap_split_f(a.as_ptr(), 20, 31, 32, 4.0, 4), 1);
            let mut rng = Rng(7);
            for &k in &[0usize, 1, 3, 4, 15, 16, 17, 511, 512, 513, 4099] {
                let src: Vec<f64> = (0..k + 5).map(|_| rng.unit()).collect();
                for off in 0..5usize {
                    for nt in 0..2 {
                        if nt == 1 && !is_x86_feature_detected!("avx2") { continue }
                        let mut dst = vec![-1.0f64; k + 8];
                        place_copy(dst.as_mut_ptr().add(off), src.as_ptr().add(off), k, nt);
                        if nt != 0 { place_fence(); }
                        assert_eq!(&dst[off..off + k], &src[off..off + k]);
                        assert!(dst[..off].iter().all(|&x| x == -1.0) && dst[off + k..].iter().all(|&x| x == -1.0));
                    }
                }
            }
            assert_eq!(place_nt_ok(1000), 0);
            assert_eq!(half_nt_ok(1000), 0);
        }
    }

    #[test]
    fn block_scans_agree() { let _port_guard = crate::port::port_test_lock();
        // the vector forms against the scalar form: the same stop and the same reason, on steps that go on, break, tie, jump and are NaN
        unsafe {
            let mut rng = Rng(11);
            let avx2 = logos_simd_kind() == 2;
            for trial in 0..4000usize {
                let n = 24 + rng.below(120);
                let up = trial % 2 == 0;
                let mut a = vec![0.0f64; n];
                let mut v = if up { 0.0 } else { 1e7 };
                for x in a.iter_mut() {
                    let r = rng.below(200);
                    let step = if r < 3 { -1.0 } else if r < 12 { 0.0 } else if r < 14 { 1e6 } else { 1.0 + rng.unit() };
                    v += if up { step } else { -step };
                    *x = v;
                }
                if rng.below(10) == 0 { let p = rng.below(n); a[p] = f64::NAN; }
                if rng.below(20) == 0 { let p = rng.below(n - 1); a[p] = f64::INFINITY; a[p + 1] = f64::INFINITY; }
                let i = rng.below(4);
                let j = i + 1 + rng.below(20);
                let (mut s0, mut s1, mut s2) = (-1, -1, -1);
                if up {
                    let r0 = reloc_blocks_up_c(a.as_ptr(), i, j, n, 0, &mut s0);
                    let r1 = reloc_blocks_up_sse2(a.as_ptr(), i, j, n, 0, &mut s1);
                    assert_eq!((r0, s0), (r1, s1), "up sse2, trial {}", trial);
                    if avx2 { let r2 = reloc_blocks_up_avx2(a.as_ptr(), i, j, n, 0, &mut s2); assert_eq!((r0, s0), (r2, s2), "up avx2, trial {}", trial); }
                    assert_eq!(reloc_blocks_up(a.as_ptr(), i, j, n, logos_simd_kind(), &mut s2), r0);
                } else {
                    for ties in 0..2 {
                        let r0 = reloc_blocks_down_c(a.as_ptr(), i, j, n, ties, &mut s0);
                        let r1 = if ties != 0 { reloc_blocks_down_sse2_ties(a.as_ptr(), i, j, n, ties, &mut s1) } else { reloc_blocks_down_sse2_strict(a.as_ptr(), i, j, n, ties, &mut s1) };
                        assert_eq!((r0, s0), (r1, s1), "down sse2 ties {}, trial {}", ties, trial);
                        if avx2 {
                            let r2 = if ties != 0 { reloc_blocks_down_avx2_ties(a.as_ptr(), i, j, n, ties, &mut s2) } else { reloc_blocks_down_avx2_strict(a.as_ptr(), i, j, n, ties, &mut s2) };
                            assert_eq!((r0, s0), (r2, s2), "down avx2 ties {}, trial {}", ties, trial);
                        }
                        assert_eq!(reloc_blocks_down(a.as_ptr(), i, j, n, logos_simd_kind(), ties, &mut s2), r0);
                    }
                }
            }
        }
    }

    #[test]
    fn detector_finds_planted_blocks() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            const L: usize = 4096;
            // 56 blocks: the chunk edges of 4 and 14 workers fall on block edges
            for (name, desc) in [("ascending", (&|_s: usize| false) as &dyn Fn(usize) -> bool), ("every other descending", &|s: usize| s % 2 == 1), ("all descending", &|_s: usize| true)] {
                for &P in &[1, 4, 14] {
                    let mut a = blocks(56, L, 3, desc);
                    let before = bits_sorted(&a);
                    let d = detect(&mut a, P, 256, false).unwrap_or_else(|| panic!("{} P={}: bailed", name, P));
                    assert_eq!(bits_sorted(&a), before);
                    assert_eq!(d.kinks, 0, "{} P={}", name, P);
                    assert_eq!(d.runs.len(), 56, "{} P={}: {:?}", name, P, &d.runs[..d.runs.len().min(6)]);
                    for (slot, &(s, e, rev)) in d.runs.iter().enumerate() { assert_eq!((s, e, rev), (slot * L, slot * L + L - 1, desc(slot)), "{} P={} run {}", name, P, slot); }
                    check_cover(&a, &d);
                }
            }
            // 8 blocks of 7 L: the edges of 14 chunks (every 4 L) fall INSIDE blocks, so the join must stitch, both ways round
            for &P in &[1, 4, 14] {
                let mut a = blocks(8, 7 * L, 5, &|s| s % 2 == 1);
                let d = detect(&mut a, P, 256, false).expect("bailed");
                assert_eq!(d.runs.len(), 8, "P={}: {:?}", P, d.runs);
                for (slot, &(s, e, rev)) in d.runs.iter().enumerate() { assert_eq!((s, e, rev), (slot * 7 * L, (slot + 1) * 7 * L - 1, slot % 2 == 1), "P={} run {}", P, slot); }
                check_cover(&a, &d);
            }
        }
    }

    #[test]
    fn detector_repairs_planted_kinks() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            const L: usize = 4096;
            for &P in &[1, 4, 14] {
                let mut a = blocks(56, L, 9, &|s| s % 3 == 2);
                let clean = a.clone();
                let mut rng = Rng(21);
                let mut planted = 0usize;
                for slot in 0..56 {
                    let mut p = slot * L + 40 + rng.below(50);
                    while p + 40 < (slot + 1) * L { a.swap(p, p + 1); planted += 1; p += 5 + rng.below(300); }
                }
                let d = detect(&mut a, P, 256, false).expect("bailed");
                assert_eq!(d.kinks, planted, "P={}", P);
                assert_eq!(d.runs.len(), 56, "P={}", P);
                assert!(a.iter().zip(&clean).all(|(x, y)| x.to_bits() == y.to_bits()), "P={}: the kinks were not all swapped back", P);
                check_cover(&a, &d);
            }
            // one strand, kinks ON the chunk edges, on the first pair of a chunk and at both ends of the array: the join's repairs
            // With the files joined the detector's mid-scan gate asks the REAL runs_plan, and this direct call passes kfrac = 0 (no
            // knot rescue), so a two-element run that begins a chunk stops the scan at P = 4 and 14. The engine never calls it that
            // way; the same shape through the whole engine is parity/routes.sh's edge_kinks, equal to C at every thread count.
            let ps: &[i32] = if cfg!(feature = "port_all") { &[1, 3] } else { &[1, 3, 4, 14] };
            for &P in ps {
                let n = 300001usize;
                let chunk = (n + P as usize - 1) / P as usize;
                let mut a: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect();
                let mut planted = 2usize;
                a.swap(0, 1); a.swap(n - 2, n - 1);
                for t in 1..P as usize {
                    let c = t * chunk;
                    if t % 2 == 1 { a.swap(c - 1, c); } else { a.swap(c, c + 1); }      // straddling the edge; beginning the chunk
                    planted += 1;
                }
                for p in [1000usize, 1004, 77777, 250000] { a.swap(p, p + 1); planted += 1; }
                let d = detect(&mut a, P, if P > 1 { 4096 } else { 256 }, false).unwrap_or_else(|| panic!("bailed P={}", P));
                assert_eq!(d.runs, vec![(0, n - 1, false)], "P={}", P);
                assert_eq!(d.kinks, planted, "P={}", P);
                assert!(ascending(&a), "P={}", P);
            }
        }
    }

    #[test]
    fn detector_lifts_planted_strays() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            const L: usize = 4096;
            for &P in &[1, 4, 14] {
                let mut a = blocks(56, L, 13, &|_s| false);
                let mut rng = Rng(33);
                let mut planted: Vec<usize> = Vec::new();
                for slot in (0..56).step_by(2) {
                    let p = slot * L + 100 + rng.below(L - 200);
                    a[p] += if slot % 4 == 0 { -5e5 } else { 5e5 };          // far below the run; far above it
                    planted.push(p);
                }
                let before = a.clone();
                let d = detect(&mut a, P, 256, true).expect("bailed");
                assert!(a.iter().zip(&before).all(|(x, y)| x.to_bits() == y.to_bits()), "P={}: lifting a stray must not write", P);
                assert_eq!(d.strays, planted, "P={}", P);
                assert_eq!(d.kinks, 0);
                assert_eq!(d.runs.len(), 56, "P={}", P);
                for (slot, &(s, e, rev)) in d.runs.iter().enumerate() { assert_eq!((s, e, rev), (slot * L, slot * L + L - 1, false), "P={} run {}", P, slot); }
                check_cover(&a, &d);
            }
            // noise: the detector must bail, and leave a permutation
            for &P in &[1, 4, 14] {
                let mut rng = Rng(5);
                let mut a: Vec<f64> = (0..200000).map(|_| rng.unit()).collect();
                let before = bits_sorted(&a);
                assert!(detect(&mut a, P, 256, true).is_none(), "P={}", P);
                assert_eq!(bits_sorted(&a), before);
            }
        }
    }

    #[test]
    fn stray_compact_restore_finish() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut rng = Rng(17);
            for trial in 0..200usize {
                // runs in output order with gaps of strays between and inside them
                let nr = 1 + rng.below(5);
                let mut a: Vec<f64> = Vec::new();
                let mut runs: Vec<usize> = Vec::new();
                let mut sp: Vec<usize> = Vec::new();
                let mut v = 0.0;
                for r in 0..nr {
                    for _ in 0..rng.below(3) { sp.push(a.len()); a.push(-1.0 - rng.unit()); }
                    let s = a.len();
                    let len = 5 + rng.below(40);
                    for i in 0..len {
                        if i > 0 && i + 1 < len && rng.below(8) == 0 { sp.push(a.len()); a.push(1e9 * rng.unit()); }
                        v += rng.unit(); a.push(v);
                    }
                    runs.push(s); runs.push((a.len() - 1) | if r % 2 == 1 { RUN_TWIST } else { 0 });
                }
                for _ in 0..rng.below(3) { sp.push(a.len()); a.push(-1.0 - rng.unit()); }
                let n = a.len();
                let S = sp.len();
                let orig = a.clone();
                let mut st = vec![0.0f64; S.max(1)];
                let mut r2 = runs.clone();
                stray_compact(a.as_mut_ptr(), r2.as_mut_ptr(), nr, sp.as_ptr(), S, st.as_mut_ptr());
                // the runs now tile [0, n - S) and hold the non-strays in order
                let mut next = 0usize;
                for k in 0..nr { assert_eq!(r2[2 * k], next); next = run_end(r2[2 * k + 1]) + 1; assert_eq!(r2[2 * k + 1] & RUN_TWIST, runs[2 * k + 1] & RUN_TWIST); }
                assert_eq!(next, n - S, "trial {}", trial);
                assert!(ascending(&a[..n - S]));
                for q in 0..S { assert_eq!(st[q].to_bits(), orig[sp[q]].to_bits()); }
                // finish: the merge of the strays into the placed elements
                let placed: Vec<f64> = a[..n - S].to_vec();
                let mut out = vec![0.0f64; n];
                let mut st2 = st.clone();
                stray_finish(out.as_mut_ptr(), placed.as_ptr(), n - S, st2.as_mut_ptr(), S);
                assert!(ascending(&out), "trial {}", trial);
                assert_eq!(bits_sorted(&out), bits_sorted(&orig));
                // restore: the inverse of compact
                stray_restore(a.as_mut_ptr(), n, sp.as_ptr(), S, st.as_ptr());
                assert!(a.iter().zip(&orig).all(|(x, y)| x.to_bits() == y.to_bits()), "trial {}", trial);
            }
        }
    }

    #[test]
    fn strand_place_strays_sorts() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut rng = Rng(29);
            for trial in 0..300usize {
                let n = 2 + rng.below(400);
                let S = 1 + rng.below((n / 4).max(1));
                let mut pos: Vec<usize> = (0..n).collect();
                for i in (1..n).rev() { let j = rng.below(i + 1); pos.swap(i, j); }
                let mut sp: Vec<usize> = pos[..S.min(n - 1)].to_vec();
                sp.sort_unstable();
                let S = sp.len();
                let mut a = vec![0.0f64; n];
                let mut v = -3.0;
                let mut q = 0usize;
                for p in 0..n {
                    if q < S && sp[q] == p { a[p] = match rng.below(6) { 0 => f64::NEG_INFINITY, 1 => f64::INFINITY, 2 => -0.0, _ => (rng.unit() - 0.3) * 60.0 }; q += 1; }
                    else { v += if rng.below(4) == 0 { 0.0 } else { rng.unit() }; a[p] = if v.abs() < 0.2 { if rng.below(2) == 0 { 0.0 } else { -0.0 } } else { v }; }
                }
                // the strand (non-strays) must ascend under <: signed zeros compare equal
                let orig = a.clone();
                let mut T: Vec<f64> = sp.iter().map(|&p| a[p]).collect();
                T.sort_by(|x, y| x.total_cmp(y));
                let mut sg = vec![sseg_t { src: 0, len: 0, dst: 0 }; 2 * S + 2];
                let mut tpos = vec![0usize; S];
                strand_place_strays(a.as_mut_ptr(), n, sp.as_ptr(), S, T.as_ptr(), sg.as_mut_ptr(), tpos.as_mut_ptr());
                assert!(ascending(&a), "trial {}: {:?}", trial, a);
                assert_eq!(bits_sorted(&a), bits_sorted(&orig), "trial {}", trial);
            }
        }
    }

    #[test]
    fn relocate_one_strand() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            let run = |a: &mut Vec<f64>, P: i32| -> i32 {
                let mut dst = vec![0.0f64; a.len()];
                relocate_try_f64(a.as_mut_ptr(), a.len(), P, dst.as_mut_ptr(), null_mut())
            };
            for &P in &[1, 4, 14] {
                for &n in &[5000usize, 300001] {
                    // sorted, with duplicates, signed zeros and infinities
                    let mut a: Vec<f64> = (0..n).map(|i| ((i / 3) as f64) - (n / 6) as f64).collect();
                    a[0] = f64::NEG_INFINITY; a[1] = f64::NEG_INFINITY; a[n - 1] = f64::INFINITY; a[n - 2] = f64::INFINITY;
                    for x in a.iter_mut() { if *x == 0.0 { *x = -0.0; break } }
                    let before = bits_sorted(&a);
                    g_onerun_count = 0; g_stray_count = 0; g_kink_count = 0;
                    assert_eq!(run(&mut a, P), 1, "sorted n={} P={}", n, P);
                    assert!(ascending(&a)); assert_eq!(bits_sorted(&a), before);
                    // (the infinities at both ends are lifted as strays and put back, as in C: the head's swing and a gap up)
                    assert_eq!((g_onerun_count, g_kink_count), (1, 0));
                    // its mirror: one twisted strand, reversed where it lies
                    // (with ties and infinities the mirror is not one run: a tie at a run's head goes the ascending way, as in C.
                    //  Whatever the route answers, the array stays a permutation, and sorted if it says so)
                    a.reverse();
                    let r = run(&mut a, P);
                    assert_eq!(bits_sorted(&a), before);
                    if r != 0 { assert!(ascending(&a)); }
                    let mut a: Vec<f64> = (0..n).map(|i| (n - i) as f64 * 0.5).collect();
                    let before = bits_sorted(&a);
                    g_onerun_count = 0;
                    assert_eq!(run(&mut a, P), 1, "reversed n={} P={}", n, P);
                    assert!(ascending(&a)); assert_eq!(bits_sorted(&a), before);
                    assert_eq!(g_onerun_count, 1);
                    // one strand with kinks
                    let mut a: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect();
                    let before = bits_sorted(&a);
                    let mut rng = Rng(41 + n as u64);
                    let (mut p, mut planted) = (30usize, 0usize);
                    while p + 30 < n { a.swap(p, p + 1); planted += 1; p += 3 + rng.below(400); }
                    g_onerun_count = 0; g_kink_count = 0;
                    assert_eq!(run(&mut a, P), 1, "kinks n={} P={}", n, P);
                    assert!(ascending(&a)); assert_eq!(bits_sorted(&a), before);
                    assert_eq!(g_onerun_count, 1);
                    assert_eq!(g_kink_count, planted, "kinks n={} P={}", n, P);
                    // one strand with far strays: lifted, sorted apart, put back in place
                    let mut a: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect();
                    let mut planted = 0usize;
                    let mut p = 500usize;
                    while p + 500 < n { a[p] = if planted % 2 == 0 { -1e9 + p as f64 } else { 1e9 + p as f64 }; planted += 1; p += 900 + rng.below(3000); }
                    let before = bits_sorted(&a);
                    g_onerun_count = 0; g_stray_count = 0; g_kink_count = 0;
                    assert_eq!(run(&mut a, P), 1, "strays n={} P={}", n, P);
                    assert!(ascending(&a)); assert_eq!(bits_sorted(&a), before);
                    assert_eq!((g_onerun_count, g_stray_count, g_kink_count), (1, planted, 0), "strays n={} P={}", n, P);
                }
            }
            // two runs or more: with no plan (the stand-in runs_plan) the route declines and the strays go back into their holes
            let ps: &[i32] = if cfg!(feature = "port_all") { &[] } else { &[1, 4] };       // the premise is the stand-in runs_plan; joined, the route takes these
            for &P in ps {
                let mut a = blocks(56, 4096, 3, &|s| s % 2 == 1);
                for slot in (0..56).step_by(4) { a[slot * 4096 + 2000] -= 5e5; }
                let before = a.clone();
                g_stray_count = 0;
                assert_eq!(run(&mut a, P), 0);
                assert!(a.iter().zip(&before).all(|(x, y)| x.to_bits() == y.to_bits()), "P={}: a decline must leave the array as it was", P);
                assert_eq!(g_stray_count, 0);
            }
            g_onerun_count = 0; g_stray_count = 0; g_kink_count = 0;
        }
    }
}
