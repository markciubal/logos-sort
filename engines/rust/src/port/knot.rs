//! C lines 1981-2738 of `engines/c/logos_psort.c`: run plans, the knot sweep, the two-strand and k-way merges, the
//! sequential placement, the half placement and `runs_relocate_f64`. A transliteration: see `PORTING.md`.
use super::*;

// rdesc_t is in the prelude (C line 1981).
pub(crate) const RUN_TWIST: usize = 1usize << (size_of::<usize>() * 8 - 1);
pub(crate) static mut g_twist_knot: usize = 0;      // the witness: see logos_twist_last()
pub(crate) static mut g_twist_reloc: usize = 0;
#[inline(always)] pub(crate) unsafe fn run_end(e: usize) -> usize { e & !RUN_TWIST }
#[inline(always)] pub(crate) unsafe fn run_rev(e: usize) -> i32 { ((e & RUN_TWIST) != 0) as i32 }
/// o[i] = x[cnt-1-i]: a twisted strand's piece, turned the right way round by the copy that was going to be made anyway
pub(crate) unsafe fn copy_rev(o: *mut f64, x: *const f64, cnt: usize) { let mut i = 0usize; while i < cnt { *o.add(i) = *x.add(cnt - 1 - i); i += 1; } }
#[inline]
pub(crate) unsafe fn strand_copy(o: *mut f64, x: *const f64, cnt: usize, rev: i32) { if rev != 0 { copy_rev(o, x, cnt) } else { memcpy(o, x, cnt * size_of::<f64>()) } }
pub(crate) unsafe fn cmp_rdesc(x: *const c_void, y: *const c_void) -> i32 { let a = (*(x as *const rdesc_t)).first; let b = (*(y as *const rdesc_t)).first; (a > b) as i32 - (a < b) as i32 }

/// The C's `qsort(d, R, sizeof *d, cmp_rdesc)`. NOT a literal port: a stable bottom-up merge sort over the same comparator
/// (the order of equal keys is not glibc's, and nothing may depend on it). Hand-written rather than `slice::sort_by` because
/// the comparator is not a total order when a first value is a NaN, and the standard sort may panic on that.
unsafe fn rdesc_qsort(d: *mut rdesc_t, R: usize) {
    if R < 2 { return }
    let t = malloc(R * size_of::<rdesc_t>()) as *mut rdesc_t;
    let (mut src, mut dst) = (d, t);
    let mut w = 1usize;
    while w < R {
        let mut lo = 0usize;
        while lo < R {
            let mid = if lo + w < R { lo + w } else { R }; let hi = if lo + 2 * w < R { lo + 2 * w } else { R };
            let (mut i, mut j, mut o) = (lo, mid, lo);
            while i < mid && j < hi {
                if cmp_rdesc(src.add(i) as *const c_void, src.add(j) as *const c_void) > 0 { *dst.add(o) = *src.add(j); j += 1 } else { *dst.add(o) = *src.add(i); i += 1 }
                o += 1;
            }
            while i < mid { *dst.add(o) = *src.add(i); i += 1; o += 1 }
            while j < hi { *dst.add(o) = *src.add(j); j += 1; o += 1 }
            lo = hi;
        }
        let s = src; src = dst; dst = s; w <<= 1;
    }
    if src != d { memcpy(d, src, R * size_of::<rdesc_t>()) }
    free(t);
}

/// Sorted descriptors + chains for a flat [start,end,...] run list. chain[k] is the index of chain k's first descriptor
/// (nchain+1 entries, sentinel R); *merged = elements inside chains with more than one run.
pub(crate) unsafe fn runs_plan(a: *const f64, runs: *const usize, R: usize, pd: *mut *mut rdesc_t, pchain: *mut *mut usize, pnchain: *mut usize, pmerged: *mut usize) -> i32 {
    let d = malloc(R * size_of::<rdesc_t>()) as *mut rdesc_t; let ch = malloc((R + 1) * size_of::<usize>()) as *mut usize;
    for k in 0..R {
        let s = *runs.add(2 * k); let e = run_end(*runs.add(2 * k + 1)); let rv = run_rev(*runs.add(2 * k + 1));
        let dk = &mut *d.add(k);
        dk.first = if rv != 0 { *a.add(e) } else { *a.add(s) }; dk.last = if rv != 0 { *a.add(s) } else { *a.add(e) }; dk.start = s; dk.len = e - s + 1; dk.rev = rv;
    }
    rdesc_qsort(d, R);
    let (mut nc, mut merged, mut celems, mut cruns) = (0usize, 0usize, (*d).len, 1usize); let mut cmax = (*d).last; *ch.add(nc) = 0; nc += 1;
    for k in 1..R {
        let dk = &*d.add(k);
        if dk.first < cmax { if dk.last > cmax { cmax = dk.last } celems += dk.len; cruns += 1; }   // overlaps the chain so far
        else { if cruns > 1 { merged += celems } *ch.add(nc) = k; nc += 1; cmax = dk.last; celems = dk.len; cruns = 1; }
    }
    if cruns > 1 { merged += celems }
    *ch.add(nc) = R; *pd = d; *pchain = ch; *pnchain = nc; *pmerged = merged; 1
}

// ---- the knot route: comparisons only where strands cross ----------------------------------------------------------------
pub(crate) const KNOT_COST_MAX_SEQ: f64 = 3.0;
pub(crate) const KNOT_COST_MAX_FEW: f64 = 3.0;
pub(crate) const KNOT_COST_MAX_PAR: f64 = 1.0;
pub(crate) const KNOT_GRAIN_MIN: usize = 32768;
pub(crate) const KNOT_MIN_AVG_RUN: usize = 256;
pub(crate) const KNOT_MERGE_WEIGHT: usize = 3;       // a merged element, per level, in units of a copied one (the placement's thread cap)
// env switches, unset: LOGOS_NO_KNOT, LOGOS_NO_KNOT_GALLOP, LOGOS_KNOT_MAX, LOGOS_KNOT_FEW
pub(crate) const g_knot_on: i32 = 1;
pub(crate) const g_knot_gallop: i32 = 1;
pub(crate) const g_knot_frac_seq: f64 = KNOT_COST_MAX_SEQ;
pub(crate) const g_knot_frac_few: f64 = KNOT_COST_MAX_FEW;
pub(crate) const g_knot_frac_par: f64 = KNOT_COST_MAX_PAR;
/// read once per sort on the calling thread in C; every switch it reads is unset here, so nothing is left to do
pub(crate) unsafe fn knot_env() {}
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct kpt_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) rev: i32 }                          // src: the piece's lowest address, whichever way it lies
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct kpiece_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize, pub(crate) copy: i32, pub(crate) rev: i32 }
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct kseg_t { pub(crate) p0: usize, pub(crate) k: usize, pub(crate) dst: usize, pub(crate) len: usize }   // a crossing: pieces [p0, p0+k) land at dst and must be merged
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct knot_plan_t {
    pub(crate) pc: *mut kpiece_t, pub(crate) npc: usize, pub(crate) cap_pc: usize, pub(crate) sg: *mut kseg_t, pub(crate) nsg: usize, pub(crate) cap_sg: usize,
    pub(crate) n3: usize, pub(crate) crossing: usize, pub(crate) cost: usize, pub(crate) kmax: usize, pub(crate) total: usize, pub(crate) last_single: i32,
}
pub(crate) unsafe fn knot_plan_free(K: *mut knot_plan_t) { free((*K).pc); free((*K).sg); (*K).pc = null_mut(); (*K).sg = null_mut(); }
/// Element c, counted in ASCENDING value order, of the strand that occupies a[st, st+ln).
#[inline(always)] pub(crate) unsafe fn knot_at(a: *const f64, st: usize, ln: usize, rv: i32, c: usize) -> f64 { if rv != 0 { *a.add(st + ln - 1 - c) } else { *a.add(st + c) } }
/// the lowest address of a strand's logical range [c0, c1)
#[inline(always)] pub(crate) unsafe fn knot_src(st: usize, ln: usize, rv: i32, c0: usize, c1: usize) -> usize { if rv != 0 { st + ln - c1 } else { st + c0 } }
/// Where a cut falls inside a strand, in logical coordinates: the first position in [lo,hi) that is past the cut.
/// above = 0 cuts BELOW v (elements < v stay before it), above = 1 cuts ABOVE v (elements <= v stay before it).
#[inline]
pub(crate) unsafe fn knot_cut(a: *const f64, st: usize, ln: usize, rv: i32, lo: usize, hi: usize, v: f64, above: i32) -> usize {
    let before = |x: f64| -> bool { if above != 0 { x <= v } else { x < v } };
    if lo >= hi || !before(knot_at(a, st, ln, rv, lo)) { return lo }
    let (mut j, mut s) = (lo, 1usize);
    while j + s < hi && before(knot_at(a, st, ln, rv, j + s)) { j += s; s <<= 1; }
    let mut L = j + 1; let mut R = if j + s < hi { j + s } else { hi };
    while L < R { let m = L + ((R - L) >> 1); if before(knot_at(a, st, ln, rv, m)) { L = m + 1 } else { R = m } }
    L
}
#[inline] pub(crate) unsafe fn knot_levels(k: usize) -> usize { let mut l = 0usize; while (1usize << l) < k { l += 1 } l }
pub(crate) unsafe fn knot_push_piece(K: *mut knot_plan_t, src: usize, len: usize, dst: usize, copy: i32, rev: i32) -> i32 {
    let K = &mut *K;
    if K.npc == K.cap_pc { let nc = if K.cap_pc != 0 { 2 * K.cap_pc } else { 256 }; let p = realloc(K.pc, nc * size_of::<kpiece_t>()) as *mut kpiece_t; K.pc = p; K.cap_pc = nc; }
    let q = kpiece_t { src, len, dst, copy, rev }; *K.pc.add(K.npc) = q; K.npc += 1; 1
}
pub(crate) unsafe fn knot_push_seg(K: *mut knot_plan_t, p0: usize, k: usize, dst: usize, len: usize) -> i32 {
    let K = &mut *K;
    if K.nsg == K.cap_sg { let nc = if K.cap_sg != 0 { 2 * K.cap_sg } else { 64 }; let p = realloc(K.sg, nc * size_of::<kseg_t>()) as *mut kseg_t; K.sg = p; K.cap_sg = nc; }
    let g = kseg_t { p0, k, dst, len }; *K.sg.add(K.nsg) = g; K.nsg += 1; 1
}
/// one value interval, as it will be placed: k non-empty pieces landing at K->total
pub(crate) unsafe fn knot_emit_one(K: *mut knot_plan_t, tp: *const kpt_t, k: usize, seglen: usize) -> i32 {
    if k == 1 {
        let t0 = *tp;
        if (*K).last_single != 0 && (*K).npc != 0 {                       // the same strand, still alone: extend its copy
            let p = (*K).pc.add((*K).npc - 1);
            // an untwisted strand's next piece lies after the last one in memory, a twisted strand's BEFORE it
            if (*p).rev == t0.rev && (if (*p).rev != 0 { t0.src + t0.len == (*p).src } else { (*p).src + (*p).len == t0.src }) {
                if (*p).rev != 0 { (*p).src = t0.src }
                (*p).len += t0.len; (*K).total += seglen; return 1;
            }
        }
        if knot_push_piece(K, t0.src, t0.len, (*K).total, 1, t0.rev) == 0 { return 0 }
        (*K).last_single = 1; (*K).total += seglen; return 1;
    }
    let p0 = (*K).npc; let mut o = (*K).total;
    // Crossings of three or more are STAGED (copied side by side, then merged in scratch).
    for q in 0..k { let t = *tp.add(q); if knot_push_piece(K, t.src, t.len, o, (k >= 3) as i32, t.rev) == 0 { return 0 } o += t.len; }
    if knot_push_seg(K, p0, k, (*K).total, seglen) == 0 { return 0 }
    if k >= 3 { (*K).n3 += 1 }
    (*K).last_single = 0; (*K).total += seglen; 1
}
/// A crossing larger than the grain is cut at values read from its largest piece, every piece at the same values:
/// the sub-intervals are independent merges (or plain copies, where only one piece has elements).
pub(crate) unsafe fn knot_emit(K: *mut knot_plan_t, a: *const f64, tp: *const kpt_t, sub: *mut kpt_t, cq: *mut usize, k: usize, seglen: usize, grain: usize, emit: i32) -> i32 {
    if k >= 2 { (*K).crossing += seglen; (*K).cost += seglen * knot_levels(k); if k > (*K).kmax { (*K).kmax = k } }
    if emit == 0 { (*K).total += seglen; return 1 }
    if k < 2 || seglen <= grain { return knot_emit_one(K, tp, k, seglen) }
    let m = (seglen + grain - 1) / grain; let mut big = 0usize;
    for q in 1..k { if (*tp.add(q)).len > (*tp.add(big)).len { big = q } }
    for q in 0..k { *cq.add(q) = 0 }                // cursors in each piece's logical coordinates
    let tb = *tp.add(big);
    let mut c = 1usize;
    while c <= m {
        let (mut ks, mut sl) = (0usize, 0usize);
        let mut cv = 0.0f64; if c < m { cv = knot_at(a, tb.src, tb.len, tb.rev, (tb.len / m) * c + ((tb.len % m) * c) / m) }
        for q in 0..k {
            let t = *tp.add(q);
            let e = t.len; let c1 = if c < m { knot_cut(a, t.src, t.len, t.rev, *cq.add(q), e, cv, 0) } else { e };
            if c1 > *cq.add(q) { let s = &mut *sub.add(ks); s.src = knot_src(t.src, t.len, t.rev, *cq.add(q), c1); s.len = c1 - *cq.add(q); s.rev = t.rev; ks += 1; sl += c1 - *cq.add(q); }
            *cq.add(q) = c1;
        }
        if ks != 0 && knot_emit_one(K, sub, ks, sl) == 0 { return 0 }
        c += 1;
    }
    1
}
/// d: run descriptors sorted by first value. emit = 0 measures only (crossing, cost, kmax). Returns 0 when there is no
/// plan: a NaN endpoint, or more live-strand visits than a plan is worth.
pub(crate) unsafe fn knot_plan(a: *const f64, d: *const rdesc_t, R: usize, grain: usize, emit: i32, K: *mut knot_plan_t) -> i32 {
    *K = core::mem::zeroed();
    if R < 1 { return 0 }
    for i in 0..R { let di = &*d.add(i); if di.first != di.first || di.last != di.last { return 0 } }
    let ls = malloc(R * size_of::<f64>()) as *mut f64; let cur = malloc(R * size_of::<usize>()) as *mut usize;
    let act = malloc(R * size_of::<usize>()) as *mut usize; let cq = malloc(R * size_of::<usize>()) as *mut usize;
    let tp = malloc(2 * R * size_of::<kpt_t>()) as *mut kpt_t;
    for i in 0..R { *ls.add(i) = (*d.add(i)).last; *cur.add(i) = 0; }     // cur: how much of strand i is consumed, in its logical coordinates
    sample_sort(ls, R);
    let (mut is, mut il, mut nact, mut visits) = (0usize, 0usize, 0usize, 0usize); let visit_cap = 64 * R + 65536; let mut ok = 1i32;
    // The sweep line visits 2R cuts in value order. A strand's FIRST value cuts below itself and its LAST value cuts above
    // itself; at equal values the first-cut goes first. Firsts arrive sorted in d[], lasts in ls[].
    while il < R {
        let above: i32 = (!(is < R && (*d.add(is)).first <= *ls.add(il))) as i32; let v = if above != 0 { *ls.add(il) } else { (*d.add(is)).first };
        let (mut k, mut seglen) = (0usize, 0usize);                         // the interval that ENDS at this cut
        for q in 0..nact {
            let r = *act.add(q); let dr = *d.add(r); let c0 = *cur.add(r); let c1 = knot_cut(a, dr.start, dr.len, dr.rev, c0, dr.len, v, above);
            *cur.add(r) = c1;
            if c1 > c0 { let t = &mut *tp.add(k); t.src = knot_src(dr.start, dr.len, dr.rev, c0, c1); t.len = c1 - c0; t.rev = dr.rev; k += 1; seglen += c1 - c0; }
        }
        visits += nact; if visits > visit_cap { ok = 0; break }
        if k != 0 && knot_emit(K, a, tp, tp.add(R), cq, k, seglen, grain, emit) == 0 { ok = 0; break }
        if above == 0 { *act.add(nact) = is; nact += 1; is += 1; }          // the strand whose first value this is goes live
        else {                                                              // strands ending here are fully consumed: drop them
            let mut w = 0usize; for q in 0..nact { if (*d.add(*act.add(q))).last > v { *act.add(w) = *act.add(q); w += 1 } }
            nact = w; il += 1;
        }
    }
    free(ls); free(cur); free(act); free(cq); free(tp);
    if ok == 0 { knot_plan_free(K); return 0 }
    1
}
/// both pieces span the same value interval, so they interleave: a branch-free merge, no galloping
pub(crate) unsafe fn knot_merge2(x: *const f64, nx: usize, y: *const f64, ny: usize, o: *mut f64) {
    let (mut i, mut j) = (0usize, 0usize); let mut o = o;
    while i < nx && j < ny { let xv = *x.add(i); let yv = *y.add(j); let ty = (yv < xv) as usize; *o = if ty != 0 { yv } else { xv }; o = o.add(1); j += ty; i += 1 - ty; }
    if i < nx { memcpy(o, x.add(i), (nx - i) * size_of::<f64>()) }
    else if j < ny { memcpy(o, y.add(j), (ny - j) * size_of::<f64>()) }
}
// element k, in ascending value order, of a strand of nv elements at v that lies ascending (up) or descending (dn); the copies
// of cnt of them from k on; the vector loads of elements k..k+3. The C's KG_* / KX* / KM* macros (they take nv everywhere here).
#[inline(always)] unsafe fn kg_up(v: *const f64, _nv: usize, k: usize) -> f64 { *v.add(k) }
#[inline(always)] unsafe fn kg_dn(v: *const f64, nv: usize, k: usize) -> f64 { *v.add(nv - 1 - k) }
#[inline(always)] unsafe fn kg_cpy_up(v: *const f64, _nv: usize, oo: *mut f64, k: usize, cnt: usize) { memcpy(oo, v.add(k), cnt * size_of::<f64>()) }
#[inline(always)] unsafe fn kg_cpy_dn(v: *const f64, nv: usize, oo: *mut f64, k: usize, cnt: usize) { copy_rev(oo, v.add(nv - k - cnt), cnt) }
// The same merge with either strand TWISTED: its cursor walks down instead of up. One loop per orientation pair.
macro_rules! KNOT_MERGE2_DEF {
    ($name:ident, $XAT:ident, $YAT:ident) => {
        pub(crate) unsafe fn $name(x: *const f64, nx: usize, y: *const f64, ny: usize, o: *mut f64) {
            let (mut i, mut j) = (0usize, 0usize); let mut o = o;
            while i < nx && j < ny { let xv = $XAT(x, nx, i); let yv = $YAT(y, ny, j); let ty = (yv < xv) as usize; *o = if ty != 0 { yv } else { xv }; o = o.add(1); j += ty; i += 1 - ty; }
            while i < nx { *o = $XAT(x, nx, i); o = o.add(1); i += 1; }
            while j < ny { *o = $YAT(y, ny, j); o = o.add(1); j += 1; }
        }
    };
}
KNOT_MERGE2_DEF!(knot_merge2_du, kg_dn, kg_up);
KNOT_MERGE2_DEF!(knot_merge2_ud, kg_up, kg_dn);
KNOT_MERGE2_DEF!(knot_merge2_dd, kg_dn, kg_dn);
pub(crate) const g_twist_generic: i32 = 0;        // LOGOS_TWIST_GENERIC=1: the strided loop (unset)
// THE GALLOPING TWO-STRAND MERGE. Ties go to x exactly as in the branch-free merge. Returns the number of gallops.
pub(crate) const KNOT_GALLOP_RUN: usize = 8;
pub(crate) const KNOT_GALLOP_RATIO: usize = 4;
macro_rules! KNOT_MERGE2G_DEF {
    ($name:ident, $XAT:ident, $YAT:ident, $XCPY:ident, $YCPY:ident) => {
        pub(crate) unsafe fn $name(x: *const f64, nx: usize, y: *const f64, ny: usize, o: *mut f64) -> usize {
            let (mut i, mut j, mut gallops) = (0usize, 0usize, 0usize); const G: usize = KNOT_GALLOP_RUN; let mut o = o;
            while i < nx && j < ny {
                let xv = $XAT(x, nx, i); let yv = $YAT(y, ny, j);
                if i + G <= nx && $XAT(x, nx, i + G - 1) <= yv {                 // x wins the next G, ties included: gallop x
                    let (mut lo, mut s) = (i + G - 1, G);
                    while lo + s < nx && $XAT(x, nx, lo + s) <= yv { lo += s; s <<= 1; }
                    let mut hi = if lo + s < nx { lo + s } else { nx };
                    while lo + 1 < hi { let m = lo + ((hi - lo) >> 1); if $XAT(x, nx, m) <= yv { lo = m } else { hi = m } }
                    $XCPY(x, nx, o, i, hi - i); o = o.add(hi - i); i = hi; gallops += 1; continue;
                }
                if j + G <= ny && $YAT(y, ny, j + G - 1) < xv {                  // y wins the next G, strictly: gallop y
                    let (mut lo, mut s) = (j + G - 1, G);
                    while lo + s < ny && $YAT(y, ny, lo + s) < xv { lo += s; s <<= 1; }
                    let mut hi = if lo + s < ny { lo + s } else { ny };
                    while lo + 1 < hi { let m = lo + ((hi - lo) >> 1); if $YAT(y, ny, m) < xv { lo = m } else { hi = m } }
                    $YCPY(y, ny, o, j, hi - j); o = o.add(hi - j); j = hi; gallops += 1; continue;
                }
                let mut st = 0usize;
                while st < G && i < nx && j < ny { let a_ = $XAT(x, nx, i); let b_ = $YAT(y, ny, j); let ty = (b_ < a_) as usize; *o = if ty != 0 { b_ } else { a_ }; o = o.add(1); j += ty; i += 1 - ty; st += 1; }
            }
            if i < nx { $XCPY(x, nx, o, i, nx - i) } else if j < ny { $YCPY(y, ny, o, j, ny - j) }
            gallops
        }
    };
}
KNOT_MERGE2G_DEF!(knot_merge2g_uu, kg_up, kg_up, kg_cpy_up, kg_cpy_up);
KNOT_MERGE2G_DEF!(knot_merge2g_du, kg_dn, kg_up, kg_cpy_dn, kg_cpy_up);
KNOT_MERGE2G_DEF!(knot_merge2g_ud, kg_up, kg_dn, kg_cpy_up, kg_cpy_dn);
KNOT_MERGE2G_DEF!(knot_merge2g_dd, kg_dn, kg_dn, kg_cpy_dn, kg_cpy_dn);

// THE VECTOR TWO-STRAND MERGE (LOGOS_SIMD_MERGE). A compare-exchange is one predicate and two selects (lo takes b where
// b < a, ties keep a), so a NaN or a signed zero comes out a permutation.
/// KM_CEX
#[target_feature(enable = "avx2,fma")] #[inline]
pub(crate) unsafe fn km_cex(a: __m256d, b: __m256d, lo: *mut __m256d, hi: *mut __m256d) { let m_ = _mm256_cmp_pd::<_CMP_LT_OQ>(b, a); *lo = _mm256_blendv_pd(a, b, m_); *hi = _mm256_blendv_pd(b, a, m_); }
/// a bitonic 4-vector sorted ascending: (0,2)(1,3), then (0,1)(2,3). ONE mask per pair, permuted to both of its lanes.
#[target_feature(enable = "avx2,fma")] #[inline]
pub(crate) unsafe fn km_bitonic4(v: __m256d) -> __m256d {
    let mut v = v;
    let mut s = _mm256_permute4x64_pd::<0x4E>(v); let mut m = _mm256_cmp_pd::<_CMP_LT_OQ>(s, v);   // s = lanes 2 3 0 1; m0 = v2 < v0, m1 = v3 < v1
    v = _mm256_blendv_pd(v, s, _mm256_permute4x64_pd::<0x44>(m));                                    // mask lanes 0 1 0 1
    s = _mm256_permute4x64_pd::<0xB1>(v); m = _mm256_cmp_pd::<_CMP_LT_OQ>(s, v);                   // s = lanes 1 0 3 2; m0 = v1 < v0, m2 = v3 < v2
    _mm256_blendv_pd(v, s, _mm256_permute4x64_pd::<0xA0>(m))                                         // mask lanes 0 0 2 2
}
/// a, b ascending: the four smallest ascending, the rest ascending
#[target_feature(enable = "avx2,fma")] #[inline]
pub(crate) unsafe fn km_merge8(a: __m256d, b: __m256d, lo: *mut __m256d, hi: *mut __m256d) {
    let mut l = _mm256_setzero_pd(); let mut h = _mm256_setzero_pd(); let b = _mm256_permute4x64_pd::<0x1B>(b); km_cex(a, b, &mut l, &mut h); *lo = km_bitonic4(l); *hi = km_bitonic4(h);
}
#[target_feature(enable = "avx2,fma")] #[inline]
unsafe fn km_ld_up(v: *const f64, _nv: usize, k: usize) -> __m256d { _mm256_loadu_pd(v.add(k)) }
/// elements k..k+3, ascending, of a strand stored descending
#[target_feature(enable = "avx2,fma")] #[inline]
unsafe fn km_ld_dn(v: *const f64, nv: usize, k: usize) -> __m256d { _mm256_permute4x64_pd::<0x1B>(_mm256_loadu_pd(v.add(nv - 4 - k))) }
// Two chains at once: the front chain stores the four smallest and carries the four largest, the back chain stores the four
// largest at the far end of the output and carries the four smallest. The two carries and the remainders fill the gap.
macro_rules! KNOT_MERGE2V_DEF {
    ($name:ident, $XAT:ident, $YAT:ident, $XLD:ident, $YLD:ident, $XCPY:ident, $YCPY:ident) => {
        #[target_feature(enable = "avx2,fma")]
        pub(crate) unsafe fn $name(x: *const f64, nx: usize, y: *const f64, ny: usize, o: *mut f64) {
            let (mut i, mut j, mut ib, mut jb, mut nc) = (0usize, 0usize, 0usize, 0usize, 0usize);
            let mut c = [0.0f64; 8]; let mut of = o; let mut ob = o.add(nx + ny);
            if nx >= 8 && ny >= 8 {
                let mut va = $XLD(x, nx, 0); let mut vb = $YLD(y, ny, 0); let mut wa = $XLD(x, nx, nx - 4); let mut wb = $YLD(y, ny, ny - 4);
                let mut lo = _mm256_setzero_pd(); let mut hi = _mm256_setzero_pd(); let mut lo2 = _mm256_setzero_pd(); let mut hi2 = _mm256_setzero_pd();
                i = 4; j = 4; ib = 4; jb = 4;
                loop {
                    km_merge8(va, vb, &mut lo, &mut hi); _mm256_storeu_pd(of, lo); of = of.add(4);
                    km_merge8(wa, wb, &mut lo2, &mut hi2); ob = ob.sub(4); _mm256_storeu_pd(ob, hi2);
                    if i + ib + 8 > nx || j + jb + 8 > ny { break }
                    let tf: i64 = ($XAT(x, nx, i) <= $YAT(y, ny, j)) as i64; let tb: i64 = ($XAT(x, nx, nx - 1 - ib) >= $YAT(y, ny, ny - 1 - jb)) as i64;
                    let cx = $XLD(x, nx, i); let cy = $YLD(y, ny, j); let dx = $XLD(x, nx, nx - 4 - ib); let dy = $YLD(y, ny, ny - 4 - jb);
                    va = _mm256_blendv_pd(cy, cx, _mm256_castsi256_pd(_mm256_set1_epi64x(-tf))); vb = hi; i += 4 * (tf as usize); j += 4 * ((tf == 0) as usize);
                    wa = _mm256_blendv_pd(dy, dx, _mm256_castsi256_pd(_mm256_set1_epi64x(-tb))); wb = lo2; ib += 4 * (tb as usize); jb += 4 * ((tb == 0) as usize);
                }
                {
                    let mut f = [0.0f64; 4]; let mut b = [0.0f64; 4]; _mm256_storeu_pd(f.as_mut_ptr(), hi); _mm256_storeu_pd(b.as_mut_ptr(), lo2); let (mut p, mut q) = (0usize, 0usize);   // the two carries, one sorted list of eight
                    while p < 4 && q < 4 { let tq = (b[q] < f[p]) as usize; c[nc] = if tq != 0 { b[q] } else { f[p] }; nc += 1; q += tq; p += 1 - tq; }
                    while p < 4 { c[nc] = f[p]; nc += 1; p += 1; } while q < 4 { c[nc] = b[q]; nc += 1; q += 1; }
                }
            }
            let xe = nx - ib; let ye = ny - jb; let mut t = [0.0f64; 15]; let (mut nt, mut p) = (0usize, 0usize);   // the gap [of, ob) holds the carries and both remainders
            if xe - i <= ye - j {
                let mut q = i; while p < nc && q < xe { let cv = c[p]; let xv = $XAT(x, nx, q); let ty = (xv < cv) as usize; t[nt] = if ty != 0 { xv } else { cv }; nt += 1; q += ty; p += 1 - ty; }
                while p < nc { t[nt] = c[p]; nt += 1; p += 1; } while q < xe { t[nt] = $XAT(x, nx, q); nt += 1; q += 1; }
                p = 0; while p < nt && j < ye { let tv = t[p]; let yv = $YAT(y, ny, j); let ty = (yv < tv) as usize; *of = if ty != 0 { yv } else { tv }; of = of.add(1); j += ty; p += 1 - ty; }
                while p < nt { *of = t[p]; of = of.add(1); p += 1; } if j < ye { $YCPY(y, ny, of, j, ye - j); }
            } else {
                let mut q = j; while p < nc && q < ye { let cv = c[p]; let yv = $YAT(y, ny, q); let ty = (yv < cv) as usize; t[nt] = if ty != 0 { yv } else { cv }; nt += 1; q += ty; p += 1 - ty; }
                while p < nc { t[nt] = c[p]; nt += 1; p += 1; } while q < ye { t[nt] = $YAT(y, ny, q); nt += 1; q += 1; }
                p = 0; while p < nt && i < xe { let tv = t[p]; let xv = $XAT(x, nx, i); let tk = (tv < xv) as usize; *of = if tk != 0 { tv } else { xv }; of = of.add(1); i += 1 - tk; p += tk; }
                while p < nt { *of = t[p]; of = of.add(1); p += 1; } if i < xe { $XCPY(x, nx, of, i, xe - i); }
            }
            let _ = ob;
        }
    };
}
KNOT_MERGE2V_DEF!(knot_merge2v_uu, kg_up, kg_up, km_ld_up, km_ld_up, kg_cpy_up, kg_cpy_up);
KNOT_MERGE2V_DEF!(knot_merge2v_du, kg_dn, kg_up, km_ld_dn, km_ld_up, kg_cpy_dn, kg_cpy_up);
KNOT_MERGE2V_DEF!(knot_merge2v_ud, kg_up, kg_dn, km_ld_up, km_ld_dn, kg_cpy_up, kg_cpy_dn);
KNOT_MERGE2V_DEF!(knot_merge2v_dd, kg_dn, kg_dn, km_ld_dn, km_ld_dn, kg_cpy_dn, kg_cpy_dn);

pub(crate) static mut g_simd_merge: i32 = 0;                       // the vector kernel for this sort: set per sort by simd_merge_env (C line 8595)
pub(crate) static g_simd_merges: AtomicUsize = AtomicUsize::new(0);  // logos_twist_last(13): two-strand merges taken by the vector kernel
pub(crate) static g_knot_gallops: AtomicUsize = AtomicUsize::new(0); // logos_twist_last(5): gallops made since it was last read
pub(crate) unsafe fn knot_merge2x(x: *const f64, nx: usize, xr: i32, y: *const f64, ny: usize, yr: i32, o: *mut f64) -> usize {
    if g_knot_gallop != 0 && g_twist_generic == 0 && (nx >= KNOT_GALLOP_RATIO * ny || ny >= KNOT_GALLOP_RATIO * nx) {
        if xr == 0 && yr == 0 { return knot_merge2g_uu(x, nx, y, ny, o) }
        if xr != 0 && yr != 0 { return knot_merge2g_dd(x, nx, y, ny, o) }
        return if xr != 0 { knot_merge2g_du(x, nx, y, ny, o) } else { knot_merge2g_ud(x, nx, y, ny, o) };
    }
    if g_simd_merge != 0 && g_twist_generic == 0 {
        g_simd_merges.fetch_add(1, Relaxed);
        if xr == 0 && yr == 0 { knot_merge2v_uu(x, nx, y, ny, o) } else if xr != 0 && yr != 0 { knot_merge2v_dd(x, nx, y, ny, o) }
        else if xr != 0 { knot_merge2v_du(x, nx, y, ny, o) } else { knot_merge2v_ud(x, nx, y, ny, o) }
        return 0;
    }
    if xr == 0 && yr == 0 { knot_merge2(x, nx, y, ny, o); return 0 }
    if g_twist_generic == 0 { if xr != 0 && yr != 0 { knot_merge2_dd(x, nx, y, ny, o) } else if xr != 0 { knot_merge2_du(x, nx, y, ny, o) } else { knot_merge2_ud(x, nx, y, ny, o) } return 0 }
    let mut ix: isize = if xr != 0 { nx as isize - 1 } else { 0 }; let mut iy: isize = if yr != 0 { ny as isize - 1 } else { 0 };
    let sx: isize = if xr != 0 { -1 } else { 1 }; let sy: isize = if yr != 0 { -1 } else { 1 }; let (mut i, mut j) = (0usize, 0usize); let mut o = o;
    while i < nx && j < ny {
        let xv = *x.offset(ix); let yv = *y.offset(iy); let ty = (yv < xv) as isize; *o = if ty != 0 { yv } else { xv }; o = o.add(1);
        iy += sy & -ty; ix += sx & -(1 - ty); j += ty as usize; i += (1 - ty) as usize;
    }
    while i < nx { *o = *x.offset(ix); o = o.add(1); i += 1; ix += sx; }
    while j < ny { *o = *y.offset(iy); o = o.add(1); j += 1; iy += sy; }
    0
}
// ---- the k-way merge in vectors: adjacent pairs merged level by level by knot_merge2x, ping-ponging between the array and
// the scratch, with one copy back if the result ends in the scratch. Runs must be contiguous and in order. Returns 0,
// touching nothing, where the vector kernel is off.
pub(crate) const g_runmerge_vec: i32 = 1;                               // LOGOS_RUNMERGE_SCALAR unset
pub(crate) static g_runmerge_vecs: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(32): k-way merges done in vectors
#[inline(never)]
pub(crate) unsafe fn merge_runs_vec(a: *mut f64, buf: *mut f64, runs: *const usize, k: usize) -> i32 {
    if k < 2 || g_simd_merge == 0 || g_runmerge_vec == 0 || g_twist_generic != 0 { return 0 }
    for i in 1..k { if *runs.add(2 * i) != *runs.add(2 * i - 1) + 1 { return 0 } }
    let mut stk = [0usize; MERGE_STACK_RUNS + 1];
    let bd: *mut usize = if k <= MERGE_STACK_RUNS { stk.as_mut_ptr() } else { malloc((k + 1) * size_of::<usize>()) as *mut usize };
    for i in 0..k { *bd.add(i) = *runs.add(2 * i) }
    *bd.add(k) = *runs.add(2 * k - 1) + 1;                                // run i is [bd[i], bd[i + 1])
    let (mut src, mut dst) = (a, buf); let (mut m, mut gl) = (k, 0usize);
    while m > 1 {
        let mut w = 0usize;
        let mut i = 0usize;
        while i < m {                                                     // writes go to bd[w], w <= i / 2: nothing still to be read
            if i + 1 < m { gl += knot_merge2x(src.add(*bd.add(i)), *bd.add(i + 1) - *bd.add(i), 0, src.add(*bd.add(i + 1)), *bd.add(i + 2) - *bd.add(i + 1), 0, dst.add(*bd.add(i))) }
            else { memcpy(dst.add(*bd.add(i)), src.add(*bd.add(i)), (*bd.add(i + 1) - *bd.add(i)) * size_of::<f64>()) }
            *bd.add(w) = *bd.add(i); w += 1;
            i += 2;
        }
        *bd.add(w) = *bd.add(m); m = w;
        let t = src; src = dst; dst = t;
    }
    if src != a { memcpy(a.add(*bd), src.add(*bd), (*bd.add(1) - *bd) * size_of::<f64>()) }
    if gl != 0 { g_knot_gallops.fetch_add(gl, Relaxed); }
    if bd != stk.as_mut_ptr() { free(bd) }
    g_runmerge_vecs.fetch_add(1, Relaxed);
    1
}
/// [start,end] pairs of every piece in output coordinates, the form merge_all_runs takes (used by k >= 3 crossings only)
pub(crate) unsafe fn knot_rl(K: *const knot_plan_t) -> *mut usize {
    let K = &*K;
    let rl = malloc(2 * (if K.npc != 0 { K.npc } else { 1 }) * size_of::<usize>()) as *mut usize;
    for q in 0..K.npc { let p = &*K.pc.add(q); *rl.add(2 * q) = p.dst; *rl.add(2 * q + 1) = p.dst + p.len - 1; }
    rl
}
/// sequential placement into buf. Order matters: every read of a[] (copies, two-strand merges) comes before the
/// k >= 3 merges, which use a[] as scratch.
pub(crate) unsafe fn knot_place_seq(a: *mut f64, buf: *mut f64, K: *const knot_plan_t, rl: *const usize) {
    let K = &*K;
    for q in 0..K.npc { let p = &*K.pc.add(q); if p.copy != 0 { strand_copy(buf.add(p.dst), a.add(p.src), p.len, p.rev) } }
    let mut gl = 0usize;
    for s in 0..K.nsg { let g = &*K.sg.add(s); if g.k == 2 { let x = K.pc.add(g.p0); let y = x.add(1); gl += knot_merge2x(a.add((*x).src), (*x).len, (*x).rev, a.add((*y).src), (*y).len, (*y).rev, buf.add(g.dst)); } }
    if gl != 0 { g_knot_gallops.fetch_add(gl, Relaxed); }
    if K.n3 != 0 { for s in 0..K.nsg { let g = &*K.sg.add(s); if g.k >= 3 {
        let r = rl.add(2 * g.p0);
        if merge_runs_vec(buf, a, r, g.k) == 0 { merge_all_runs(buf, a, r, g.k) }
    } } }
}
// ===================== HALF THE SCRATCH (FINDINGS 3bq) =====================
pub(crate) const HALF_MIN_N: usize = 65536;
pub(crate) static g_half_places: AtomicUsize = AtomicUsize::new(0);      // logos_twist_last(34): placements made with half a scratch
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct hpc_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize, pub(crate) rev: i32 }   // a piece: src its lowest address; dst where it lands (a copy) or is staged (k >= 3)
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct hjob_t { pub(crate) dst: usize, pub(crate) len: usize, pub(crate) p0: usize, pub(crate) k: usize }   // output [dst, dst + len): k == 1 a copy, k == 2 a merge, k >= 3 staged, then merged
#[repr(C)] #[derive(Clone, Copy)]
pub(crate) struct hord_t { pub(crate) src: usize, pub(crate) i: usize }
pub(crate) unsafe fn half_cmp(p: *const c_void, q: *const c_void) -> i32 { let x = (*(p as *const hord_t)).src; let y = (*(q as *const hord_t)).src; (x > y) as i32 - (x < y) as i32 }
/// the logical part [c0, c1) of a piece, a piece of its own
#[inline]
pub(crate) unsafe fn half_sub(x: *const hpc_t, c0: usize, c1: usize, dst: usize) -> hpc_t {
    hpc_t { src: knot_src((*x).src, (*x).len, (*x).rev, c0, c1), len: c1 - c0, dst, rev: (*x).rev }
}
/// how many of the t smallest elements of a two-piece merge come from x: the merge path, both pieces read in value order
pub(crate) unsafe fn half_path(a: *const f64, x: *const hpc_t, y: *const hpc_t, t: usize) -> usize {
    let (x, y) = (&*x, &*y);
    let mut lo = if t > y.len { t - y.len } else { 0 }; let mut hi = if t < x.len { t } else { x.len };
    while lo < hi {
        let m = lo + ((hi - lo) >> 1);
        if knot_at(a, x.src, x.len, x.rev, m) < knot_at(a, y.src, y.len, y.rev, t - m - 1) { lo = m + 1 } else { hi = m }
    }
    lo
}
pub(crate) const HALF_KMAX: usize = 64;
/// elements of x below v (le: up to v)
pub(crate) unsafe fn half_rank(a: *const f64, x: *const hpc_t, v: f64, le: i32) -> usize {
    let x = &*x;
    let (mut lo, mut hi) = (0usize, x.len);
    while lo < hi { let m = lo + ((hi - lo) >> 1); let e = knot_at(a, x.src, x.len, x.rev, m); if (if le != 0 { !(v < e) } else { e < v }) { lo = m + 1 } else { hi = m } }
    lo
}
/// the merge path for k >= 3 pieces: c[q] of the t smallest come from piece q. A binary search on VALUES.
pub(crate) unsafe fn half_pathk(a: *const f64, x: *const hpc_t, k: usize, t: usize, c: *mut usize) {
    let mut lo = [0usize; HALF_KMAX]; let mut hi = [0usize; HALF_KMAX]; let mut Lq = [0usize; HALF_KMAX]; let mut Uq = [0usize; HALF_KMAX];
    for q in 0..k { lo[q] = 0; hi[q] = (*x.add(q)).len; }
    loop {
        let (mut big, mut w, mut L, mut U) = (0usize, 0usize, 0usize, 0usize);
        for q in 0..k { if hi[q] - lo[q] > w { w = hi[q] - lo[q]; big = q; } }
        if w == 0 { break }
        let xb = &*x.add(big);
        let v = knot_at(a, xb.src, xb.len, xb.rev, lo[big] + w / 2);
        for q in 0..k { Lq[q] = half_rank(a, x.add(q), v, 0); Uq[q] = half_rank(a, x.add(q), v, 1); L += Lq[q]; U += Uq[q]; }
        if t < L { for q in 0..k { if Lq[q] < hi[q] { hi[q] = Lq[q] } } }
        else if t > U { for q in 0..k { if Uq[q] > lo[q] { lo[q] = Uq[q] } } }
        else { let mut need = t - L; for q in 0..k { let e = Uq[q] - Lq[q]; let tk = if need < e { need } else { e }; *c.add(q) = Lq[q] + tk; need -= tk; } return }
    }
    for q in 0..k { *c.add(q) = lo[q] }
}
/// one job's reads, written at out + its output offsets: out is buf for the lower half and a[] itself for the upper
#[inline]
pub(crate) unsafe fn half_piece(o: *mut f64, x: *const f64, cnt: usize, rev: i32, nt: i32) { if rev != 0 { copy_rev(o, x, cnt) } else { half_copy(o, x, cnt, nt) } }
pub(crate) unsafe fn half_job(out: *mut f64, a: *const f64, pc: *const hpc_t, j: *const hjob_t, nt: i32) -> usize {
    let j = &*j;
    let x = pc.add(j.p0);
    if j.k == 1 { half_piece(out.add(j.dst), a.add((*x).src), (*x).len, (*x).rev, nt); return 0 }
    if j.k == 2 { let x1 = x.add(1); return knot_merge2x(a.add((*x).src), (*x).len, (*x).rev, a.add((*x1).src), (*x1).len, (*x1).rev, out.add(j.dst)) }
    for q in 0..j.k { let xq = &*x.add(q); half_piece(out.add(xq.dst), a.add(xq.src), xq.len, xq.rev, nt); }
    0
}
/// a staged k >= 3 job, merged where it lies with scr[0, len) as its scratch
pub(crate) unsafe fn half_mergek(out: *mut f64, scr: *mut f64, pc: *const hpc_t, j: *const hjob_t, rl: *mut usize) {
    let j = &*j;
    for q in 0..j.k { *rl.add(2 * q) = (*pc.add(j.p0 + q)).dst - j.dst; *rl.add(2 * q + 1) = *rl.add(2 * q) + (*pc.add(j.p0 + q)).len - 1; }
    if merge_runs_vec(out.add(j.dst), scr, rl, j.k) == 0 { merge_all_runs(out.add(j.dst), scr, rl, j.k) }
}
/// step 5 with strays: the placed elements are buf[0, H) then a[H, N), st[0, S) is sorted, and the n = N + S elements are
/// merged from the top down.
pub(crate) unsafe fn half_strays(a: *mut f64, buf: *const f64, H: usize, N: usize, st: *const f64, S: usize, nt: i32) {
    let half_at = |m: usize| -> f64 { if m < H { *buf.add(m) } else { *a.add(m) } };
    let mut i = N;
    let mut q = S;
    while q > 0 {
        q -= 1;
        let v = *st.add(q); let p: usize;
        if i == 0 || !(v < half_at(i - 1)) { p = i }
        else {
            let (mut top, mut step) = (i - 1, 1usize); let mut lo: usize;      // HALF_AT(top) > v: find the first index whose element is > v
            loop { if step > top { lo = 0; break } if !(v < half_at(top - step)) { lo = top - step + 1; break } top -= step; step <<= 1; }
            let mut hi = top;
            while lo < hi { let m = lo + ((hi - lo) >> 1); if v < half_at(m) { hi = m } else { lo = m + 1 } }
            p = lo;
        }
        if i > H && p < i { let s0 = if p > H { p } else { H }; memmove(a.add(s0 + q + 1), a.add(s0), (i - s0) * size_of::<f64>()); }   // the part in a[] first:
        if p < H && p < i { let e0 = if i < H { i } else { H }; half_copy(a.add(p + q + 1), buf.add(p), e0 - p, nt); }                  // it moves right of this
        *a.add(p + q) = v; i = p;
    }
    half_copy(a, buf, if i < H { i } else { H }, nt);                     // what is left below every stray; a[H, i) is in place
    half_fence(nt);
}
/// step 2, the cheap way: only the pieces that reach H or above are copied into the gaps the lower half's sources left below
/// H, largest first, first fit. Planned before anything moves: returns 0 having moved nothing when it cannot.
pub(crate) const HALF_FIT_MAX: usize = 1024;
pub(crate) unsafe fn half_fit(a: *mut f64, pc: *mut hpc_t, ord: *const hord_t, m: usize, H: usize, N: usize, jb: *const hjob_t, nj: usize, nt: i32) -> i32 {
    if m > HALF_FIT_MAX { return 0 }
    for q in 0..nj { let j = &*jb.add(q); if j.dst < H && j.k >= 3 && j.len > N - H { return 0 } }
    let gs = malloc(4 * (m + 1) * size_of::<usize>()) as *mut usize;
    let ge = gs.add(m + 1); let mv = ge.add(m + 1); let to = mv.add(m + 1); let (mut ng, mut nm, mut prev) = (0usize, 0usize, 0usize);
    for r in 0..m {                                                       // gaps: [0, H) less every upper piece (ord is by address)
        let p = &*pc.add((*ord.add(r)).i);
        if p.src + p.len > H { *mv.add(nm) = (*ord.add(r)).i; nm += 1; }
        if p.src >= H { continue }
        if p.src > prev { *gs.add(ng) = prev; *ge.add(ng) = p.src; ng += 1; }
        prev = if p.src + p.len < H { p.src + p.len } else { H };
    }
    if prev < H { *gs.add(ng) = prev; *ge.add(ng) = H; ng += 1; }
    for x in 1..nm { let v = *mv.add(x); let mut y = x; while y > 0 && (*pc.add(*mv.add(y - 1))).len < (*pc.add(v)).len { *mv.add(y) = *mv.add(y - 1); y -= 1; } *mv.add(y) = v; }
    for x in 0..nm {
        let L = (*pc.add(*mv.add(x))).len; let mut g = 0usize;
        while g < ng && *ge.add(g) - *gs.add(g) < L { g += 1 }
        if g == ng { free(gs); return 0 }                                 // nothing has moved
        *to.add(x) = *gs.add(g); *gs.add(g) += L;
    }
    for x in 0..nm { let p = &mut *pc.add(*mv.add(x)); half_copy(a.add(*to.add(x)), a.add(p.src), p.len, nt); p.src = *to.add(x); }
    free(gs); 1
}
/// pc[] has room for 2 * HALF_KMAX + 4 more pieces and jb[] for one more job (a split adds them). st is sorted. Returns 0
/// having touched nothing in a[] or buf when no midpoint below N can be had.
pub(crate) unsafe fn half_place(a: *mut f64, N: usize, buf: *mut f64, pc: *mut hpc_t, npc: usize, jb: *mut hjob_t, nj: usize, st: *const f64, S: usize) -> i32 {
    let (mut npc, mut nj) = (npc, nj);
    let mut H = (N + 1) / 2; let mut js = usize::MAX;
    for q in 0..nj { let j = &*jb.add(q); if j.dst < H && j.dst + j.len > H { js = q; break } }
    if js != usize::MAX {
        let j = *jb.add(js); let t = H - j.dst;
        if j.k > HALF_KMAX { H = j.dst + j.len; if H >= N { return 0 } }           // too many strands to split: H moves to its end
        else if j.k >= 3 {                                                        // split by value: two staged jobs (or fewer pieces)
            let mut c = [0usize; HALF_KMAX]; let (k, p0, d0, len) = (j.k, j.p0, j.dst, j.len); let (mut nl, mut nh) = (0usize, 0usize); let lb = npc; let hb: usize; let mut o = d0;
            half_pathk(a, pc.add(p0), k, t, c.as_mut_ptr());
            for q in 0..k { if c[q] != 0 { let x = *pc.add(p0 + q); *pc.add(npc) = half_sub(&x, 0, c[q], o); npc += 1; o += c[q]; nl += 1; } }
            hb = npc; o = H;
            for q in 0..k { if c[q] < (*pc.add(p0 + q)).len { let x = *pc.add(p0 + q); *pc.add(npc) = half_sub(&x, c[q], x.len, o); npc += 1; o += x.len - c[q]; nh += 1; } }
            let lo = hjob_t { dst: d0, len: t, p0: lb, k: nl }; let hi = hjob_t { dst: H, len: len - t, p0: hb, k: nh };
            *jb.add(js) = lo; *jb.add(nj) = hi; nj += 1;
        }
        else if j.k == 1 {
            let x = *pc.add(j.p0);
            *pc.add(npc) = half_sub(&x, 0, t, j.dst); *pc.add(npc + 1) = half_sub(&x, t, x.len, H);
            let lo = hjob_t { dst: j.dst, len: t, p0: npc, k: 1 }; let hi = hjob_t { dst: H, len: x.len - t, p0: npc + 1, k: 1 };
            *jb.add(js) = lo; *jb.add(nj) = hi; nj += 1; npc += 2;
        } else {
            let x = *pc.add(j.p0); let y = *pc.add(j.p0 + 1);
            let i = half_path(a, &x, &y, t); let k = t - i; let mut base = npc; let len = j.len; let d0 = j.dst;
            let lo: hjob_t; let hi: hjob_t;
            if i != 0 && k != 0 { *pc.add(npc) = half_sub(&x, 0, i, 0); npc += 1; *pc.add(npc) = half_sub(&y, 0, k, 0); npc += 1; lo = hjob_t { dst: d0, len: t, p0: base, k: 2 }; }
            else { *pc.add(npc) = if i != 0 { half_sub(&x, 0, i, d0) } else { half_sub(&y, 0, k, d0) }; npc += 1; lo = hjob_t { dst: d0, len: t, p0: base, k: 1 }; }
            base = npc;
            if i < x.len && k < y.len { *pc.add(npc) = half_sub(&x, i, x.len, 0); npc += 1; *pc.add(npc) = half_sub(&y, k, y.len, 0); npc += 1; hi = hjob_t { dst: H, len: len - t, p0: base, k: 2 }; }
            else { *pc.add(npc) = if i < x.len { half_sub(&x, i, x.len, H) } else { half_sub(&y, k, y.len, H) }; npc += 1; hi = hjob_t { dst: H, len: len - t, p0: base, k: 1 }; }
            *jb.add(js) = lo; *jb.add(nj) = hi; nj += 1;
        }
    }
    let (mut nu, mut kmax) = (0usize, 2usize);
    for q in 0..nj { let j = &*jb.add(q); if j.k > kmax { kmax = j.k } if j.dst >= H { nu += j.k } }
    let ord = malloc((if nu != 0 { nu } else { 1 }) * size_of::<hord_t>()) as *mut hord_t; let rl = malloc(2 * kmax * size_of::<usize>()) as *mut usize;
    let (mut gl, mut m, mut w) = (0usize, 0usize, 0usize); let nt = half_nt_ok(N);
    for q in 0..nj { if (*jb.add(q)).dst < H { gl += half_job(buf, a, pc, jb.add(q), nt) } }                    // 1
    for q in 0..nj { let j = &*jb.add(q); if j.dst >= H { for r in 0..j.k { (*ord.add(m)).src = (*pc.add(j.p0 + r)).src; (*ord.add(m)).i = j.p0 + r; m += 1; } } }
    // qsort(ord, m, sizeof *ord, half_cmp): a stable sort over the same comparator (a total order on addresses)
    core::slice::from_raw_parts_mut(ord, m).sort_by(|p, q| half_cmp(p as *const hord_t as *const c_void, q as *const hord_t as *const c_void).cmp(&0));
    let fitted = half_fit(a, pc, ord, m, H, N, jb, nj, nt);                                                      // 2: gaps first
    if fitted == 0 { for r in 0..m {                                                                             // 2: else the slide
        let p = &mut *pc.add((*ord.add(r)).i);
        if p.src != w { if w + p.len <= p.src { half_copy(a.add(w), a.add(p.src), p.len, nt) } else { memmove(a.add(w), a.add(p.src), p.len * size_of::<f64>()) } }
        p.src = w; w += p.len;
    } }
    half_fence(nt);                                                                                              // what streamed is read below
    for q in 0..nj { let j = &*jb.add(q); if j.dst < H && j.k >= 3 { half_mergek(buf, if fitted != 0 { a.add(H) } else { a.add(N - H) }, pc, jb.add(q), rl) } }   // 3
    for q in 0..nj { if (*jb.add(q)).dst >= H { gl += half_job(a, a, pc, jb.add(q), nt) } }                     // 4
    half_fence(nt);
    for q in 0..nj { let j = &*jb.add(q); if j.dst >= H && j.k >= 3 { half_mergek(a, a, pc, jb.add(q), rl) } }
    if S != 0 { half_strays(a, buf, H, N, st, S, nt) } else { half_copy(a, buf, H, nt); half_fence(nt); }       // 5
    free(ord); free(rl);
    if gl != 0 { g_knot_gallops.fetch_add(gl, Relaxed); }
    g_half_places.fetch_add(1, Relaxed);
    1
}
/// the knot plan as jobs (in output order), then half_place
pub(crate) unsafe fn knot_place_half(a: *mut f64, buf: *mut f64, N: usize, K: *const knot_plan_t, st: *const f64, S: usize) -> i32 {
    let K = &*K;
    let pc = malloc((K.npc + 2 * HALF_KMAX + 4) * size_of::<hpc_t>()) as *mut hpc_t; let jb = malloc((K.npc + 1) * size_of::<hjob_t>()) as *mut hjob_t;   // at most one job per piece
    for q in 0..K.npc { let kp = &*K.pc.add(q); let p = hpc_t { src: kp.src, len: kp.len, dst: kp.dst, rev: kp.rev }; *pc.add(q) = p; }
    let (mut nj, mut s) = (0usize, 0usize);
    let mut q = 0usize;
    while q < K.npc {
        if s < K.nsg && (*K.sg.add(s)).p0 == q { let g = &*K.sg.add(s); let j = hjob_t { dst: g.dst, len: g.len, p0: q, k: g.k }; *jb.add(nj) = j; nj += 1; q += g.k; s += 1; }
        else { let kp = &*K.pc.add(q); let j = hjob_t { dst: kp.dst, len: kp.len, p0: q, k: 1 }; *jb.add(nj) = j; nj += 1; q += 1; }
    }
    let ok = half_place(a, N, buf, pc, K.npc, jb, nj, st, S);
    free(pc); free(jb); ok
}
/// two sorted pieces lying side by side, [0, d) and [d, n): the tail and head routes' vector merge with half its scratch
pub(crate) unsafe fn half_two(a: *mut f64, n: usize, d: usize, buf: *mut f64) -> i32 {
    let z = hpc_t { src: 0, len: 0, dst: 0, rev: 0 };
    let mut pc: [hpc_t; 6] = [hpc_t { src: 0, len: d, dst: 0, rev: 0 }, hpc_t { src: d, len: n - d, dst: 0, rev: 0 }, z, z, z, z];
    let mut jb: [hjob_t; 2] = [hjob_t { dst: 0, len: n, p0: 0, k: 2 }, hjob_t { dst: 0, len: 0, p0: 0, k: 0 }];
    half_place(a, n, buf, pc.as_mut_ptr(), 2, jb.as_mut_ptr(), 1, null(), 0)
}
pub(crate) unsafe fn half_on(n: usize) -> i32 { (n >= HALF_MIN_N) as i32 }     // LOGOS_PLACE_FULL unset

/// Sequential relocation for the leaf dispatcher: copies every run into buf in chain order, then merges inside multi-run
/// chains with a[] as scratch. Returns buf, or NULL when the gate declines (caller merges in place as before).
pub(crate) unsafe fn runs_relocate_f64(a: *mut f64, n: usize, buf: *mut f64, runs: *const usize, n_runs: usize) -> *mut f64 {
    let mut d: *mut rdesc_t = null_mut(); let mut ch: *mut usize = null_mut(); let mut nc = 0usize; let mut merged = 0usize;
    if runs_plan(a, runs, n_runs, &mut d, &mut ch, &mut nc, &mut merged) == 0 { return null_mut() }
    if g_knot_on != 0 && n >= KNOT_MIN_AVG_RUN * n_runs {   // finer than chains: copy what lies alone, merge only what crosses
        let mut K: knot_plan_t = core::mem::zeroed();
        if knot_plan(a, d, n_runs, n + 1, 1, &mut K) != 0 {
            let kf = if n_runs > MERGE_MAX_RUNS { g_knot_frac_seq } else { g_knot_frac_few };   // few runs: the caller's run merge is the fallback
            let go = K.total == n && (K.cost as f64 <= kf * n as f64 || merged <= n / 2);
            let mut krl: *mut usize = null_mut(); if go && K.n3 != 0 { krl = knot_rl(&K); }
            if go { knot_place_seq(a, buf, &K, krl); free(krl); knot_plan_free(&mut K); free(d); free(ch); return buf }
            free(krl); knot_plan_free(&mut K);
        }
    }
    if merged > n / 2 { free(d); free(ch); return null_mut() }
    let rl = malloc(2 * n_runs * size_of::<usize>()) as *mut usize;
    let mut o = 0usize;
    for q in 0..n_runs { let dq = &*d.add(q); *rl.add(2 * q) = o; *rl.add(2 * q + 1) = o + dq.len - 1; strand_copy(buf.add(o), a.add(dq.start), dq.len, dq.rev); o += dq.len; }
    for k in 0..nc { if *ch.add(k + 1) - *ch.add(k) > 1 { merge_all_runs(buf, a, rl.add(2 * *ch.add(k)), *ch.add(k + 1) - *ch.add(k)) } }
    free(rl); free(d); free(ch); buf
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn below(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
    }
    fn bits_sorted(v: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = v.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    fn ascending(v: &[f64]) -> bool { v.windows(2).all(|w| !(w[1] < w[0])) }
    fn simd() -> i32 { (logos_simd_kind() == 2) as i32 }

    /// a sorted strand (ascending under `<`) with ties, signed zeros and infinities
    fn strand(r: &mut Rng, n: usize, pool: usize) -> Vec<f64> {
        let mut v: Vec<f64> = (0..n).map(|_| match r.below(pool + 6) {
            0 => -0.0, 1 => 0.0, 2 => f64::NEG_INFINITY, 3 => f64::INFINITY, 4 => -0.0, 5 => 0.0,
            k => (k as f64 - 6.0 - pool as f64 / 2.0) * 0.5,
        }).collect();
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
        v
    }
    fn ref_merge(x: &[f64], y: &[f64]) -> Vec<f64> {       // ties to x, as every scalar kernel
        let (mut i, mut j) = (0, 0); let mut o = Vec::with_capacity(x.len() + y.len());
        while i < x.len() && j < y.len() { if y[j] < x[i] { o.push(y[j]); j += 1 } else { o.push(x[i]); i += 1 } }
        o.extend_from_slice(&x[i..]); o.extend_from_slice(&y[j..]); o
    }
    const CANARY: f64 = 12345.678;
    unsafe fn one_pair(xl: &[f64], yl: &[f64]) {
        let want = ref_merge(xl, yl); let wb: Vec<u64> = want.iter().map(|v| v.to_bits()).collect();
        let (nx, ny) = (xl.len(), yl.len());
        for xr in 0..2i32 { for yr in 0..2i32 {
            let xs: Vec<f64> = if xr != 0 { xl.iter().rev().cloned().collect() } else { xl.to_vec() };
            let ys: Vec<f64> = if yr != 0 { yl.iter().rev().cloned().collect() } else { yl.to_vec() };
            let (x, y) = (xs.as_ptr(), ys.as_ptr());
            for kern in 0..4 {
                if kern == 2 && simd() == 0 { continue }
                let mut out = vec![CANARY; nx + ny + 16];
                let o = out.as_mut_ptr().add(8);
                match (kern, xr, yr) {
                    (0, 0, 0) => knot_merge2(x, nx, y, ny, o), (0, 1, 0) => knot_merge2_du(x, nx, y, ny, o),
                    (0, 0, 1) => knot_merge2_ud(x, nx, y, ny, o), (0, _, _) => knot_merge2_dd(x, nx, y, ny, o),
                    (1, 0, 0) => { knot_merge2g_uu(x, nx, y, ny, o); } (1, 1, 0) => { knot_merge2g_du(x, nx, y, ny, o); }
                    (1, 0, 1) => { knot_merge2g_ud(x, nx, y, ny, o); } (1, _, _) => { knot_merge2g_dd(x, nx, y, ny, o); }
                    (2, 0, 0) => knot_merge2v_uu(x, nx, y, ny, o), (2, 1, 0) => knot_merge2v_du(x, nx, y, ny, o),
                    (2, 0, 1) => knot_merge2v_ud(x, nx, y, ny, o), (2, _, _) => knot_merge2v_dd(x, nx, y, ny, o),
                    _ => { g_simd_merge = simd(); knot_merge2x(x, nx, xr, y, ny, yr, o); }
                }
                assert!(out[..8].iter().chain(out[8 + nx + ny..].iter()).all(|v| *v == CANARY), "kernel {kern} wrote outside nx {nx} ny {ny} xr {xr} yr {yr}");
                let got = &out[8..8 + nx + ny];
                assert!(ascending(got), "kernel {kern} not ascending nx {nx} ny {ny} xr {xr} yr {yr}");
                let mut all = xl.to_vec(); all.extend_from_slice(yl);
                assert_eq!(bits_sorted(got), bits_sorted(&all), "kernel {kern} not a permutation nx {nx} ny {ny} xr {xr} yr {yr}");
                if kern < 2 { let gb: Vec<u64> = got.iter().map(|v| v.to_bits()).collect(); assert_eq!(gb, wb, "kernel {kern} bytes differ nx {nx} ny {ny} xr {xr} yr {yr}"); }
            }
        } }
    }
    #[test]
    fn merge_kernels_small() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0x9E3779B97F4A7C15);
            let nys = [0usize, 1, 2, 3, 4, 5, 7, 8, 9, 11, 12, 15, 16, 17, 23, 31, 32, 33, 47, 64, 65, 100, 127, 199, 200];
            for nx in 0..=200usize { for &ny in nys.iter() {
                let pool = [3usize, 40, 1000][r.below(3)];
                let x = strand(&mut r, nx, pool); let y = strand(&mut r, ny, pool);
                one_pair(&x, &y);
            } }
            for nx in 0..=40usize { for ny in 0..=40usize { let x = strand(&mut r, nx, 12); let y = strand(&mut r, ny, 12); one_pair(&x, &y); } }
        }
    }
    #[test]
    fn merge_kernels_large() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0xD1B54A32D192ED03);
            for &(nx, ny) in [(5000usize, 5000usize), (65536, 65537), (100003, 9), (17, 80000), (30000, 120001), (4096, 1000), (1, 50000)].iter() {
                for &pool in [5usize, 3000, 1 << 40].iter() { let x = strand(&mut r, nx, pool); let y = strand(&mut r, ny, pool); one_pair(&x, &y); }
            }
            // disjoint ranges and a sparse strand over a dense one (the gallop's shape)
            let x: Vec<f64> = (0..50000).map(|i| i as f64).collect(); let y: Vec<f64> = (0..50000).map(|i| 50000.0 + i as f64).collect();
            one_pair(&x, &y); one_pair(&y, &x);
            let s: Vec<f64> = (0..500).map(|i| (i * 100) as f64).collect(); one_pair(&x, &s); one_pair(&s, &x);
        }
    }

    /// R sorted runs laid end to end in memory (shuffled order when `shuffle`), run r spanning [r * step, r * step + width);
    /// every `twist`-th run lies descending. Returns the array and the flat run list in memory order.
    fn staircase(r: &mut Rng, R: usize, len: usize, step: f64, width: f64, quant: f64, twist: usize, shuffle: bool) -> (Vec<f64>, Vec<usize>) {
        let mut order: Vec<usize> = (0..R).collect();
        if shuffle { for i in (1..R).rev() { let j = r.below(i + 1); order.swap(i, j); } }
        let mut a = Vec::new(); let mut runs = Vec::new();
        for (slot, &k) in order.iter().enumerate() {
            let l = len / 2 + r.below(len + 1) + 1;
            let mut v: Vec<f64> = (0..l).map(|_| {
                let u = (r.next() >> 11) as f64 / (1u64 << 53) as f64;
                let mut x = k as f64 * step + u * width;
                if quant > 0.0 { x = (x / quant).floor() * quant }
                if k == 0 && x < width * 0.02 { x = if r.below(2) == 0 { -0.0 } else { 0.0 } }
                x
            }).collect();
            if k == 0 { v[0] = f64::NEG_INFINITY } if k == R - 1 { v[0] = f64::INFINITY }
            v.sort_by(|p, q| p.partial_cmp(q).unwrap());
            let rev = twist != 0 && slot % twist == 0;
            if rev { v.reverse() }
            let s = a.len(); a.extend_from_slice(&v);
            runs.push(s); runs.push((a.len() - 1) | if rev { RUN_TWIST } else { 0 });
        }
        (a, runs)
    }
    /// plan + sequential placement, plan + half placement (with and without strays), runs_relocate_f64. Returns how many
    /// half placements went through.
    unsafe fn check_place(a0: &[f64], runs: &[usize], grain: usize, r: &mut Rng) -> usize {
        g_simd_merge = simd();
        let n = a0.len(); let R = runs.len() / 2; let want = bits_sorted(a0); let mut halves = 0usize;
        let (mut d, mut ch, mut nc, mut merged) = (null_mut::<rdesc_t>(), null_mut::<usize>(), 0usize, 0usize);
        assert_eq!(runs_plan(a0.as_ptr(), runs.as_ptr(), R, &mut d, &mut ch, &mut nc, &mut merged), 1);
        for k in 1..R { assert!(!((*d.add(k)).first < (*d.add(k - 1)).first)); }
        assert_eq!(*ch.add(nc), R);
        let mut K: knot_plan_t = core::mem::zeroed();
        assert_eq!(knot_plan(a0.as_ptr(), d, R, grain, 1, &mut K), 1);
        assert_eq!(K.total, n);
        let mut M: knot_plan_t = core::mem::zeroed();
        assert_eq!(knot_plan(a0.as_ptr(), d, R, grain, 0, &mut M), 1);
        assert_eq!((M.total, M.crossing, M.cost, M.kmax, M.npc), (n, K.crossing, K.cost, K.kmax, 0));
        let rl = knot_rl(&K);
        {
            let mut a = a0.to_vec(); let mut buf = vec![0.0f64; n];
            knot_place_seq(a.as_mut_ptr(), buf.as_mut_ptr(), &K, rl);
            assert!(ascending(&buf), "knot_place_seq not ascending n {n} R {R} grain {grain}");
            assert_eq!(bits_sorted(&buf), want, "knot_place_seq not a permutation n {n} R {R} grain {grain}");
        }
        for &S in [0usize, 1, 37, n / 50 + 2].iter() {
            let mut st: Vec<f64> = (0..S).map(|_| a0[r.below(n)] + if r.below(2) == 0 { 0.25 } else { 0.0 }).collect();
            if S > 2 { st[0] = f64::NEG_INFINITY; st[1] = f64::INFINITY; }
            st.sort_by(|p, q| p.partial_cmp(q).unwrap());
            let mut a = a0.to_vec(); a.resize(n + S, CANARY); let mut buf = vec![0.0f64; n];
            let ok = knot_place_half(a.as_mut_ptr(), buf.as_mut_ptr(), n, &K, st.as_ptr(), S);
            if ok != 0 {
                halves += 1;
                let mut all = a0.to_vec(); all.extend_from_slice(&st);
                assert!(ascending(&a), "half placement not ascending n {n} R {R} grain {grain} S {S}");
                assert_eq!(bits_sorted(&a), bits_sorted(&all), "half placement not a permutation n {n} R {R} grain {grain} S {S}");
            } else {
                assert!(a[..n].iter().zip(a0.iter()).all(|(p, q)| p.to_bits() == q.to_bits()), "a declined half placement touched the array");
            }
        }
        free(rl); knot_plan_free(&mut K); free(d); free(ch);
        {
            let mut a = a0.to_vec(); let mut buf = vec![0.0f64; n];
            let o = runs_relocate_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), runs.as_ptr(), R);
            if !o.is_null() {
                assert_eq!(o, buf.as_mut_ptr());
                assert!(ascending(&buf), "runs_relocate_f64 not ascending n {n} R {R}");
                assert_eq!(bits_sorted(&buf), want, "runs_relocate_f64 not a permutation n {n} R {R}");
            }
        }
        halves
    }
    #[test]
    fn knot_staircases() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0xA0761D6478BD642F);
            let mut halves = 0usize; let mut cases = 0usize;
            for &(R, len) in [(1usize, 50usize), (2, 40), (3, 100), (5, 300), (16, 1000), (64, 500), (12, 20000)].iter() {
                for &width in [0.5f64, 1.0, 1.2, 2.0, 3.5, 1000.0].iter() {          // disjoint, touching, overlapping, several live, all live
                    for &quant in [0.0f64, 0.125].iter() { for &twist in [0usize, 1, 2, 3].iter() { for &shuffle in [false, true].iter() {
                        if len >= 20000 && (quant != 0.0 || twist == 3) { continue }
                        let (a, runs) = staircase(&mut r, R, len, 1.0, width, quant, twist, shuffle);
                        let n = a.len();
                        for &grain in [n + 1, 64, 1000].iter() { halves += check_place(&a, &runs, grain, &mut r); cases += 4; }
                    } } }
                }
            }
            assert!(halves * 2 > cases, "the half placement declined most cases: {halves} of {cases}");
        }
    }
    #[test]
    fn knot_many_strands() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut r = Rng(0xE7037ED1A0B428DB);
            // more than HALF_KMAX strands crossing over the midpoint: H moves to that crossing's end
            let (a, runs) = staircase(&mut r, 70, 200, 0.0, 1.0, 0.0, 2, false);
            let n = a.len();
            check_place(&a, &runs, n + 1, &mut r);
            // the same with every strand sharing its first and its last value: ONE crossing, the whole array, so the moved
            // midpoint is N and the half placement declines having touched nothing
            let (mut a, mut runs) = (Vec::new(), Vec::new());
            for _ in 0..70 {
                let mut v: Vec<f64> = (0..150).map(|_| (r.next() >> 11) as f64 / (1u64 << 53) as f64).collect();
                v[0] = 0.0; v[1] = 1.0; v.sort_by(|p, q| p.partial_cmp(q).unwrap());
                let s = a.len(); a.extend_from_slice(&v); runs.push(s); runs.push(a.len() - 1);
            }
            let n = a.len();
            let h = check_place(&a, &runs, n + 1, &mut r);
            assert_eq!(h, 0);
            // the same strands behind a lone block: the midpoint moves, or the crossing is split
            for &R in [9usize, 33, 70, 130].iter() {
                let (mut a, mut runs) = staircase(&mut r, R, 300, 0.0, 1.0, 0.0, 3, false);
                let s = a.len(); let l = s + 500;
                for i in 0..l { a.push(5.0 + i as f64) }
                runs.push(s); runs.push(a.len() - 1);
                let n = a.len();
                check_place(&a, &runs, n + 1, &mut r); check_place(&a, &runs, 257, &mut r);
            }
        }
    }
    #[test]
    fn half_two_pieces() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            g_simd_merge = simd();
            let mut r = Rng(0x589965CC75374CC3);
            for &n in [2usize, 3, 10, 101, 1000, 65536, 200001].iter() { for &frac in [0usize, 1, 10, 50, 90, 99, 100].iter() {
                let d = n * frac / 100;
                let pool = [4usize, 100000][r.below(2)];
                let mut a = strand(&mut r, d, pool); a.extend(strand(&mut r, n - d, pool));
                let want = bits_sorted(&a); let mut buf = vec![0.0f64; n];
                let ok = half_two(a.as_mut_ptr(), n, d, buf.as_mut_ptr());
                if ok != 0 { assert!(ascending(&a), "half_two not ascending n {n} d {d}"); }
                assert_eq!(bits_sorted(&a), want, "half_two not a permutation n {n} d {d}");
            } }
        }
    }
    #[test]
    fn plan_declines_nan() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let a = [1.0f64, 2.0, f64::NAN, 0.5, 3.0]; let runs = [0usize, 1, 2, 2, 3, 4];
            let (mut d, mut ch, mut nc, mut merged) = (null_mut::<rdesc_t>(), null_mut::<usize>(), 0usize, 0usize);
            assert_eq!(runs_plan(a.as_ptr(), runs.as_ptr(), 3, &mut d, &mut ch, &mut nc, &mut merged), 1);
            let mut K: knot_plan_t = core::mem::zeroed();
            assert_eq!(knot_plan(a.as_ptr(), d, 3, 6, 1, &mut K), 0);
            free(d); free(ch);
        }
    }
}

#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    pub(crate) const MERGE_STACK_RUNS: usize = 64;                                  // C line 1844 (leaf.rs)
    /// C line 4816 (base.rs). Stand-in: NaNs swapped to the tail, the rest sorted; returns the NaN count.
    pub(crate) unsafe fn sample_sort(s: *mut f64, m: usize) -> usize {
        let mut k = m; let mut i = 0usize;
        while i < k { if *s.add(i) != *s.add(i) { let v = *s.add(i); k -= 1; *s.add(i) = *s.add(k); *s.add(k) = v; } else { i += 1 } }
        core::slice::from_raw_parts_mut(s, k).sort_by(|a, b| a.partial_cmp(b).unwrap());
        m - k
    }
    /// C line 1845 (leaf.rs). Stand-in: the runs ([start, end] pairs, contiguous, in order) folded in left to right.
    pub(crate) unsafe fn merge_all_runs(a: *mut f64, buf: *mut f64, runs: *const usize, n_runs: usize) {
        let lo = *runs;
        for k in 1..n_runs {
            let mid = *runs.add(2 * k); let hi = *runs.add(2 * k + 1) + 1;
            let (mut i, mut j, mut o) = (lo, mid, lo);
            while i < mid && j < hi { if *a.add(j) < *a.add(i) { *buf.add(o) = *a.add(j); j += 1 } else { *buf.add(o) = *a.add(i); i += 1 } o += 1; }
            while i < mid { *buf.add(o) = *a.add(i); i += 1; o += 1; }
            while j < hi { *buf.add(o) = *a.add(j); j += 1; o += 1; }
            memcpy(a.add(lo), buf.add(lo), (hi - lo) * size_of::<f64>());
        }
    }
    /// C lines 6464-6466 (reloc.rs): place_copy / place_fence / place_nt_ok behind them. Stand-ins: a plain copy, no streaming.
    pub(crate) unsafe fn half_copy(dst: *mut f64, src: *const f64, k: usize, nt: i32) { memcpy(dst, src, k * size_of::<f64>()) }
    pub(crate) unsafe fn half_fence(nt: i32) {}
    pub(crate) unsafe fn half_nt_ok(n: usize) -> i32 { 0 }
}
#[cfg(not(feature = "port_all"))]
use missing::*;
