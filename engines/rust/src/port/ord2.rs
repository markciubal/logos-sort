//! C lines 7929-8682 of `logos_psort.c`: interleaved streams, the mirror gate, the loop-back and its rotation, the sample in
//! position order, the cuts route, the seam gate, the wave route, the dispatcher's prescan (blocks and parallel),
//! `simd_merge_env`, `md_certify`, and the constants in front of `sample_unstructured`. A transliteration: see PORTING.md.
use super::*;

// ===================== interleaved ascending streams: a stable scatter is already a sort (C 7907) =====================
pub(crate) const STREAMS_MAX_MODES: usize = 64;
pub(crate) const STREAMS_GAP: f64 = 10.0;
pub(crate) const STREAMS_MIN_N: usize = 65536;

/// Mode boundaries from a sorted sample: a gulf is a gap wider than STREAMS_GAP times the median gap, cut at its midpoint.
/// Returns the number of modes, or 0 when there is nothing to use.
pub(crate) unsafe fn streams_modes(samp: *const f64, m: usize, bnd: *mut f64) -> i32 {
    if samp.is_null() || m < 64 { return 0 }
    let g = malloc((m - 1) * size_of::<f64>()) as *mut f64;
    let mut i: usize = 0;
    while i + 1 < m { *g.add(i) = *samp.add(i + 1) - *samp.add(i); i += 1 }
    sample_sort(g, m - 1);                                   // the engine's own sort: no libc qsort here
    let med = *g.add((m - 1) / 2);
    free(g);
    if !(med > 0.0) { return 0 }                             // heavy duplicates: the counting routes own that
    let thr = STREAMS_GAP * med;
    let mut nb: i32 = 0;
    let mut i: usize = 0;
    while i + 1 < m {
        let gap = *samp.add(i + 1) - *samp.add(i);
        if gap > thr { if nb + 1 >= STREAMS_MAX_MODES as i32 { return 0 } *bnd.add(nb as usize) = *samp.add(i) + 0.5 * gap; nb += 1; }
        i += 1;
    }
    if nb >= 1 { nb + 1 } else { 0 }
}
/// branch-free: a compare per boundary, summed
#[inline]
pub(crate) unsafe fn streams_mode(x: f64, bnd: *const f64, nb: i32) -> i32 {
    let mut md: i32 = 0;
    let mut q: i32 = 0;
    while q < nb { md += (x >= *bnd.add(q as usize)) as i32; q += 1 }
    md
}
pub(crate) const STREAMS_TABLE_BITS: usize = 12;
pub(crate) const STREAMS_TABLE_MIN_MODES: i32 = 12;   // below this many streams the compares are faster (make streamsbench, lookup)
/// Cell of x in an equal-width grid over the sample's range, clamped at both ends (NaN goes to cell 0).
#[inline]
pub(crate) unsafe fn streams_cell(x: f64, lo: f64, inv: f64) -> usize {
    let mut t = (x - lo) * inv;
    t = if t > 0.0 { t } else { 0.0 };
    let top = ((1u32 << STREAMS_TABLE_BITS) - 1) as f64;
    t = if t < top { t } else { top };
    t as usize
}

/// The stream route's gate (mirror = 1: do the modes DESCEND in arrival order?). 1024 jittered, strictly increasing
/// positions; more than 2% out of order, or a NaN, says no.
#[inline(never)]
pub(crate) unsafe fn streams_arrival_ok(a: *const f64, n: usize, bnd: *const f64, nb: i32, nm: i32, mirror: i32) -> i32 {
    const SM: usize = 1024;
    let stride = n / SM;
    let mut last = [0.0f64; STREAMS_MAX_MODES];
    let mut q: i32 = 0;
    while q < nm { last[q as usize] = if mirror != 0 { f64::INFINITY } else { f64::NEG_INFINITY }; q += 1 }
    let mut wrong: usize = 0;
    let mut i: usize = 0;
    while i < SM {
        let mut h: u64 = ((i + 1) as u64).wrapping_mul(0x9e3779b97f4a7c15u64); h ^= h >> 29;
        let mut p = i * stride + (if stride != 0 { (h % stride as u64) as usize } else { 0 });
        if p >= n { p = n - 1 }
        let x = *a.add(p);
        if x != x { return 0 }
        let md = streams_mode(x, bnd, nb);
        if if mirror != 0 { x > last[md as usize] } else { x < last[md as usize] } { wrong += 1; if wrong > SM / 50 { return 0 } }   // more than 2% out of order: stop reading
        last[md as usize] = x;
        i += 1;
    }
    (wrong <= SM / 50) as i32
}
/// noinline: a 13 KB frame; see nearsorted_parallel_f64.
#[inline(never)]
pub(crate) unsafe fn streams_try_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t, bnd: *const f64, nm: i32, lo: f64, hi: f64) -> i32 {
    if nm < 2 { return 0 }                                   // LOGOS_NO_STREAMS unset
    let nb = nm - 1;

    // Gate: do the modes ascend in ARRIVAL order?
    if streams_arrival_ok(a, n, bnd, nb, nm, 0) == 0 { return 0 }

    // Pass 1: mode per element, cached, and counted. a[] is not touched until the final copy.
    let mc = malloc(n) as *mut u8;
    let mut cnt = [0usize; STREAMS_MAX_MODES + 1];
    let mut bad: i32 = 0;
    const ST: usize = 1usize << STREAMS_TABLE_BITS;
    let mut tab = [0u8; ST]; let span = hi - lo; let mut inv: f64 = 0.0; let mut amb: usize = 0;
    // LOGOS_STREAMS_CMP and LOGOS_STREAMS_TABLE unset
    let mut use_tab: i32 = (nm >= STREAMS_TABLE_MIN_MODES && span > 0.0 && span < f64::INFINITY) as i32;
    let mut q: i32 = 0;
    while q < nb && use_tab != 0 { if *bnd.add(q as usize) != *bnd.add(q as usize) { use_tab = 0 } q += 1 }
    if use_tab != 0 { inv = ST as f64 / span; if !(inv < f64::INFINITY) { use_tab = 0 } }
    if use_tab != 0 {
        let mut nbc = [0u16; ST];
        let mut q: i32 = 0;
        while q < nb { let cell = streams_cell(*bnd.add(q as usize), lo, inv); nbc[cell] = nbc[cell].wrapping_add(1); q += 1 }
        let mut below: u32 = 0;
        let mut k: usize = 0;
        while k < ST { tab[k] = if nbc[k] != 0 { 255 } else { below as u8 }; below = below.wrapping_add(nbc[k] as u32); k += 1 }
        let mut i: usize = 0;
        while i < n {
            let x = *a.add(i); bad |= (x != x) as i32;
            let mut md: i32 = tab[streams_cell(x, lo, inv)] as i32;
            if md == 255 { md = streams_mode(x, bnd, nb); amb += 1; }
            *mc.add(i) = md as u8; cnt[md as usize] += 1;
            i += 1;
        }
    } else {
        let mut i: usize = 0;
        while i < n { let x = *a.add(i); bad |= (x != x) as i32; let md = streams_mode(x, bnd, nb); *mc.add(i) = md as u8; cnt[md as usize] += 1; i += 1 }
    }
    if bad != 0 { free(mc); return 0 }
    TRACE!("streams: lookup pass done (table {})", use_tab);
    let mut off = [0usize; STREAMS_MAX_MODES + 1]; let mut cur = [0usize; STREAMS_MAX_MODES];
    off[0] = 0;
    let mut q: usize = 0;
    while q < nm as usize { off[q + 1] = off[q] + cnt[q]; cur[q] = off[q]; q += 1 }

    // Pass 2: the stable scatter. Inside each bucket, elements keep the order they arrived in.
    let mut i: usize = 0;
    while i < n { let b = *mc.add(i) as usize; *buf.add(cur[b]) = *a.add(i); cur[b] += 1; i += 1 }
    free(mc);
    TRACE!("streams: scatter done");

    // Each bucket is checked, and sorted only if it is not already in order.
    let mut dirty: i32 = 0;
    let mut q: usize = 0;
    while q < nm as usize {
        let s0 = off[q]; let len = off[q + 1] - s0; let mut j: usize = 1;
        while j < len { if *buf.add(s0 + j) < *buf.add(s0 + j - 1) { break } j += 1 }
        if j < len {
            dirty += 1;
            let r = leaf_f64(buf.add(s0), len, a.add(s0), c);  // a[] is scratch until the final copy
            if r != buf.add(s0) { memcpy(buf.add(s0), r, len * size_of::<f64>()) }
        }
        q += 1;
    }
    TRACE!("streams: buckets checked");
    memcpy(a, buf, n * size_of::<f64>());
    TRACE!("streams: {} modes, arrival probe passed, {} buckets needed a sort, table {}, {} ambiguous lookups", nm, dirty, use_tab, amb);
    1
}

// ===================== one orientation bit for the WHOLE array (C 8068) =====================
pub(crate) const MIRROR_INV_MIN: f64 = 0.90;
pub(crate) const MIRROR_MIN_N: usize = 1 << 16;
pub(crate) static mut g_mirror_count: usize = 0;
pub(crate) unsafe fn logos_mirror_count(reset: i32) -> usize { let r = g_mirror_count; if reset != 0 { g_mirror_count = 0 } r }
pub(crate) static mut g_mirrored: i32 = 0;             // re-entry guard; written on the calling thread only, one top-level sort at a time
/// noinline: nothing of this may land in the dispatcher's frame (make callbench). Returns which gate fired, 0 for none.
#[inline(never)]
pub(crate) unsafe fn mirror_gate(a: *const f64, n: usize, inv_all: f64, st_bnd: *const f64, st_nm: i32) -> i32 {
    // LOGOS_NO_MIRROR unset
    // (1) the mirror image of a sorted prefix: a DESCENDING SUFFIX of at least half the array.
    if inv_all >= MIRROR_INV_MIN { let mut i = n - 1; while i > 0 && *a.add(i - 1) >= *a.add(i) { i -= 1 } if n - i >= n / 2 && n >= TAIL_MIN_N as usize { return 1 } }
    // (2) the mirror image of short reach: the same ladder, every comparison turned round
    if inv_all >= MIRROR_INV_MIN && n >= REACH_MIN_N as usize && reach_from_sample(a, n, null(), 0, 1) > 0 { return 2 }
    // (3) the mirror image of interleaved ascending streams: the modes DESCEND in arrival order
    if st_nm >= 2 && n >= STREAMS_MIN_N && streams_arrival_ok(a, n, st_bnd, st_nm - 1, st_nm, 1) != 0 { return 3 }
    0
}

// ===================== THE LOOP-BACK: R0 on the sphere (C 8109) =====================
pub(crate) const LOOPBACK_MIN_N: usize = 4096;
pub(crate) static mut g_loop_count: usize = 0;   // logos_twist_last(7); calling thread, one top-level sort at a time (the pool's contract)
pub(crate) static mut g_looped: i32 = 0;         // re-entry guard
/// a := a[s..n) ++ a[0..s): the shorter part through scratch (the pool's buffer when it is free), the longer by one memmove.
/// (The C's three in-place reversals are its allocation-failure path: not ported.)
pub(crate) unsafe fn rotate_left_f64(a: *mut f64, n: usize, s: usize) {
    let k = n - s; let m = if k < s { k } else { s };
    let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut();
    let pooled = pool_acquire(m * size_of::<f64>(), 0, &mut pb, &mut pbi);
    let t: *mut f64 = if pooled != 0 { pb as *mut f64 } else { malloc(m * size_of::<f64>()) as *mut f64 };
    if k <= s { memcpy(t, a.add(s), k * size_of::<f64>()); memmove(a.add(k), a, s * size_of::<f64>()); memcpy(a, t, k * size_of::<f64>()); }
    else      { memcpy(t, a, s * size_of::<f64>()); memmove(a, a.add(s), k * size_of::<f64>()); memcpy(a.add(k), t, s * size_of::<f64>()); }
    if pooled != 0 { pool_release() } else { free(t) }
}
/// noinline: nothing of this may land in the dispatcher's frame (make callbench). d: the prescan's first descent, 0 < d < n.
/// Returns the rotation to make (s, the joined strand's start), or 0.
#[inline(never)]
pub(crate) unsafe fn loopback_gate(a: *const f64, n: usize, d: usize, par: i32) -> usize {
    if !(*a.add(n - 1) <= *a) { return 0 }                              // the end does not continue into the start (or a NaN is there)
    let mut s = n - 1; while s > d && *a.add(s - 1) <= *a.add(s) { s -= 1 }   // [s, n) ascends
    let L = n - s;
    if L + d < n / 2 { return 0 }
    if d >= n / 2 && L < n / 16 { return 0 }                            // the tail route has this already; the rotation would save too little
    if par != 0 && n - (L + d) > n / 16 { return 0 }                    // P threads: the rotation is one thread's pass
    // LOGOS_NO_LOOPBACK unset
    s
}

// ===================== the sample in POSITION order: the seam route and the wave route (C 8162) =====================
pub(crate) const WAVE_MIN_N: usize = 1 << 20;
pub(crate) const WAVE_INV_MAX: f64 = 0.45;
pub(crate) const WAVE_COST_MAX: f64 = 1.5;
/// Returns m points in position order, or null on a NaN in the sample. The caller frees.
pub(crate) unsafe fn sample_by_pos(a: *const f64, n: usize, m: usize) -> *mut psamp_t {
    let s = malloc(m * size_of::<psamp_t>()) as *mut psamp_t; let t = malloc(m * size_of::<psamp_t>()) as *mut psamp_t;
    let cnt = calloc(m + 1, size_of::<usize>()) as *mut usize; let raw = malloc(m * size_of::<f64>()) as *mut f64;
    let mut nan: i32 = 0; sample_read(a, n, 0, m, m, raw, &mut nan);
    if nan != 0 { free(s); free(t); free(cnt); free(raw); return null_mut() }
    let mut i: usize = 0;
    while i < m {
        (*t.add(i)).pos = SAMPLE_POS(i, m, n); (*t.add(i)).v = *raw.add(i);
        let mut key = ((*t.add(i)).pos as f64 / n as f64 * m as f64) as usize; if key >= m { key = m - 1 }
        *cnt.add(key + 1) += 1;
        i += 1;
    }
    let mut i: usize = 0;
    while i < m { *cnt.add(i + 1) += *cnt.add(i); i += 1 }
    let mut i: usize = 0;
    while i < m {
        let mut key = ((*t.add(i)).pos as f64 / n as f64 * m as f64) as usize; if key >= m { key = m - 1 }
        *s.add(*cnt.add(key)) = *t.add(i); *cnt.add(key) += 1;
        i += 1;
    }
    let mut i: usize = 1;
    while i < m {   // bucketed by position already: a few swaps
        let x = *s.add(i); let mut j = i;
        while j > 0 && (*s.add(j - 1)).pos > x.pos { *s.add(j) = *s.add(j - 1); j -= 1 }
        *s.add(j) = x;
        i += 1;
    }
    free(t); free(cnt); free(raw);
    s
}
pub(crate) static mut g_wave_count: usize = 0;

// ===================== cuts: data ordered by windows, unordered inside them (C 8203) =====================
pub(crate) const CUTS_MIN_N: usize = 1 << 20;
pub(crate) const CUTS_BLK: usize = 4096;
pub(crate) const CUTS_MIN_CAND: usize = 8;
pub(crate) static g_cuts_sorts: AtomicUsize = AtomicUsize::new(0);
/// four chains each way; a NaN is never selected
pub(crate) unsafe fn cuts_minmax(p: *const f64, nb: usize, mn: *mut f64, mx: *mut f64) {
    let mut l0 = *p; let mut l1 = l0; let mut l2 = l0; let mut l3 = l0;
    let mut h0 = l0; let mut h1 = l0; let mut h2 = l0; let mut h3 = l0; let mut i: usize = 0;
    while i + 4 <= nb {
        let v0 = *p.add(i); let v1 = *p.add(i + 1); let v2 = *p.add(i + 2); let v3 = *p.add(i + 3);
        l0 = if v0 < l0 { v0 } else { l0 }; l1 = if v1 < l1 { v1 } else { l1 }; l2 = if v2 < l2 { v2 } else { l2 }; l3 = if v3 < l3 { v3 } else { l3 };
        h0 = if v0 > h0 { v0 } else { h0 }; h1 = if v1 > h1 { v1 } else { h1 }; h2 = if v2 > h2 { v2 } else { h2 }; h3 = if v3 > h3 { v3 } else { h3 };
        i += 4;
    }
    while i < nb { l0 = if *p.add(i) < l0 { *p.add(i) } else { l0 }; h0 = if *p.add(i) > h0 { *p.add(i) } else { h0 }; i += 1 }
    l0 = if l1 < l0 { l1 } else { l0 }; l2 = if l3 < l2 { l3 } else { l2 }; h0 = if h1 > h0 { h1 } else { h0 }; h2 = if h3 > h2 { h3 } else { h2 };
    *mn = if l2 < l0 { l2 } else { l0 }; *mx = if h2 > h0 { h2 } else { h0 };
}
#[inline(never)]
pub(crate) unsafe fn cuts_try_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t, ps: *const psamp_t, m: usize) -> i32 {
    if n < CUTS_MIN_N { return 0 }                           // LOGOS_NO_CUTS unset
    // (1) the sample's candidate cuts
    let sm = malloc(m * size_of::<f64>()) as *mut f64;
    *sm.add(m - 1) = (*ps.add(m - 1)).v;
    let mut i = m - 1;
    while i > 0 { i -= 1; *sm.add(i) = if (*ps.add(i)).v < *sm.add(i + 1) { (*ps.add(i)).v } else { *sm.add(i + 1) } }
    let mut cand: usize = 0; let mut pm = (*ps).v;
    let mut i: usize = 0;
    while i + 1 < m { if (*ps.add(i)).v > pm { pm = (*ps.add(i)).v } cand += (pm <= *sm.add(i + 1)) as usize; i += 1 }
    free(sm);
    if cand < CUTS_MIN_CAND { return 0 }
    // LOGOS_CUTS_DENSE unset
    if cand > m / 2 { TRACE!("cuts: {} sample candidates of {} -> finer than the sample, decline", cand, m); return 0 }
    // (2) block extremes, and the exact cuts at block boundaries
    let nb = (n + CUTS_BLK - 1) / CUTS_BLK;
    let bmn = malloc(nb * size_of::<f64>()) as *mut f64; let bmx = malloc(nb * size_of::<f64>()) as *mut f64;
    let smn = malloc((nb + 1) * size_of::<f64>()) as *mut f64;
    let cut = malloc((nb + 2) * size_of::<usize>()) as *mut usize;
    let mut b: usize = 0;
    while b < nb { let lo = b * CUTS_BLK; let hi = if lo + CUTS_BLK < n { lo + CUTS_BLK } else { n }; cuts_minmax(a.add(lo), hi - lo, bmn.add(b), bmx.add(b)); b += 1 }
    *smn.add(nb) = f64::INFINITY;
    let mut b = nb;
    while b > 0 { b -= 1; *smn.add(b) = if *bmn.add(b) < *smn.add(b + 1) { *bmn.add(b) } else { *smn.add(b + 1) } }
    let mut nc: usize = 0; *cut.add(nc) = 0; nc += 1; let mut pmx = f64::NEG_INFINITY;
    let mut b: usize = 0;
    while b < nb {
        // block b sits between prefix max pmx (blocks < b) and suffix min smn[b + 1] (blocks > b)
        if b > 0 && pmx <= *smn.add(b) { if *cut.add(nc - 1) != b * CUTS_BLK { *cut.add(nc) = b * CUTS_BLK; nc += 1 } }
        else if b > 0 && pmx <= *smn.add(b + 1) && *bmx.add(b) > *smn.add(b + 1) && *bmn.add(b) < pmx {
            // (3) the window's edge is INSIDE block b: the first position p in it with max(a[.. p)) <= min(a[p ..))
            let lo = b * CUTS_BLK; let hi = if lo + CUTS_BLK < n { lo + CUTS_BLK } else { n }; let len = hi - lo;
            let rmin = buf;                                           // the leaf's scratch, free until the segments are sorted
            let mut run = *smn.add(b + 1);
            let mut q = len;
            while q > 0 { q -= 1; if *a.add(lo + q) < run { run = *a.add(lo + q) } *rmin.add(q) = run }
            let mut rmax = pmx;
            let mut q: usize = 1;
            while q < len {
                if *a.add(lo + q - 1) > rmax { rmax = *a.add(lo + q - 1) }
                if rmax <= *rmin.add(q) { if *cut.add(nc - 1) != lo + q { *cut.add(nc) = lo + q; nc += 1 } break }
                q += 1;
            }
        }
        if *bmx.add(b) > pmx { pmx = *bmx.add(b) }
        b += 1;
    }
    *cut.add(nc) = n; nc += 1;
    free(bmn); free(bmx); free(smn);
    let mut big: usize = 0;
    let mut k: usize = 0;
    while k + 1 < nc { if *cut.add(k + 1) - *cut.add(k) > big { big = *cut.add(k + 1) - *cut.add(k) } k += 1 }
    if nc - 1 < CUTS_MIN_CAND || big > n / 4 { TRACE!("cuts: {} sample candidates, {} segments, the largest {} -> decline", cand, nc - 1, big); free(cut); return 0 }
    // (4) every segment where it lies
    let mut k: usize = 0;
    while k + 1 < nc {
        let lo = *cut.add(k); let len = *cut.add(k + 1) - lo;
        k += 1;
        if len < 2 { continue }
        let r = leaf_f64(a.add(lo), len, buf, c);
        if r != a.add(lo) { memcpy(a.add(lo), r, len * size_of::<f64>()) }
    }
    TRACE!("cuts: {} sample candidates, {} segments, the largest {}: sorted where they lie", cand, nc - 1, big);
    free(cut);
    g_cuts_sorts.fetch_add(1, Relaxed);
    1
}

/// noinline: nothing of this may land in the dispatcher's frame (make callbench)
#[inline(never)]
pub(crate) unsafe fn seam_gate(a: *const f64, n: usize, s: *const psamp_t, m: usize) -> usize {
    // LOGOS_NO_SEAM unset
    let mut k: usize = 0; let mut drop = f64::NEG_INFINITY; let mut vmin = (*s).v; let mut vmax = (*s).v;
    let mut i: usize = 0;
    while i < m { if (*s.add(i)).v < vmin { vmin = (*s.add(i)).v } if (*s.add(i)).v > vmax { vmax = (*s.add(i)).v } i += 1 }
    let mut i: usize = 0;
    while i + 1 < m { let d = (*s.add(i)).v - (*s.add(i + 1)).v; if d > drop { drop = d; k = i } i += 1 }
    if !(drop >= 0.5 * (vmax - vmin)) || !drop.is_finite() { return 0 }
    if k + 1 < m / 16 || m - (k + 1) < m / 16 { return 0 }                // a rotation by under a sixteenth is not worth a pass
    let mut inv: usize = 0;                                               // adjacent pairs on the same side of the seam
    let mut i: usize = 0;
    while i + 1 < m { if i != k && (*s.add(i)).v > (*s.add(i + 1)).v { inv += 1 } i += 1 }
    if inv > m / 100 { return 0 }
    let lo = (*s.add(k)).pos; let hi = (*s.add(k + 1)).pos; let mut best = lo; let mut bd = f64::NEG_INFINITY;
    let mut p = lo;
    while p < hi { let d = *a.add(p) - *a.add(p + 1); if d > bd { bd = d; best = p } p += 1 }
    if bd > 0.0 { best + 1 } else { 0 }
}

/// pieces of the smoothed sample: sm = a median of five, dir[i] = sign(sm[i+1] - sm[i]), a piece is a stretch of one
/// direction at least WAVE_MIN_PIECE points long (shorter flips are absorbed). Returns the piece count.
pub(crate) const WAVE_MIN_PIECE: usize = 8;
pub(crate) unsafe fn wave_pieces(s: *const psamp_t, m: usize, starts: *mut usize, dirs: *mut i32, maxp: usize) -> usize {
    let sm = malloc(m * size_of::<f64>()) as *mut f64;
    let mut i: usize = 0;
    while i < m {
        let mut w5 = [0.0f64; 5]; let lo = if i >= 2 { i - 2 } else { 0 }; let hi = if i + 3 <= m { i + 3 } else { m }; let mut c: usize = 0;
        let mut j = lo;
        while j < hi { w5[c] = (*s.add(j)).v; c += 1; j += 1 }
        let mut x: usize = 1;
        while x < c { let v = w5[x]; let mut y = x; while y > 0 && w5[y - 1] > v { w5[y] = w5[y - 1]; y -= 1 } w5[y] = v; x += 1 }
        *sm.add(i) = w5[c / 2];
        i += 1;
    }
    let mut np: usize = 0; let mut cur: i32 = 0; let mut run: usize = 0; let mut st: usize = 0;
    let mut i: usize = 0;
    while i + 1 < m {
        let mut d: i32 = if *sm.add(i + 1) > *sm.add(i) { 1 } else if *sm.add(i + 1) < *sm.add(i) { -1 } else { 0 }; if d == 0 { d = cur }
        if d == cur { run += 1; i += 1; continue }
        if cur == 0 { cur = d; run = 1; st = 0; i += 1; continue }
        // a change of direction: count how long it holds
        let mut hold: usize = 0;
        let mut j = i;
        while j + 1 < m && hold < WAVE_MIN_PIECE {
            let dj: i32 = if *sm.add(j + 1) > *sm.add(j) { 1 } else if *sm.add(j + 1) < *sm.add(j) { -1 } else { 0 };
            if dj == d || dj == 0 { hold += 1 } else { break }
            j += 1;
        }
        if hold < WAVE_MIN_PIECE { i += 1; continue }                      // a wobble: the piece goes on
        if np == maxp { free(sm); return 0 }
        *starts.add(np) = st; *dirs.add(np) = cur; np += 1; st = i; cur = d; run = 1;
        i += 1;
    }
    if np == maxp { free(sm); return 0 }
    *starts.add(np) = st; *dirs.add(np) = cur; np += 1; *starts.add(np) = m;
    free(sm); np
}
#[inline(never)]
pub(crate) unsafe fn wave_try_f64(a: *mut f64, n: usize, buf: *mut f64, s: *const psamp_t, m: usize) -> i32 {
    // LOGOS_NO_WAVE unset
    const MAXP: usize = 512;
    let mut starts = [0usize; MAXP + 1]; let mut dirs = [0i32; MAXP];
    let np = wave_pieces(s, m, starts.as_mut_ptr(), dirs.as_mut_ptr(), MAXP);
    if np < 3 || np > m / WAVE_MIN_PIECE { TRACE!("wave: {} pieces -> decline", np); return 0 }
    // the pieces must be real at the element's scale too: raw adjacent sample pairs inside a piece mostly go the piece's way
    let mut agree: usize = 0; let mut tot: usize = 0;
    let mut p: usize = 0;
    while p < np {
        let mut i = starts[p];
        while i + 1 < starts[p + 1] {
            tot += 1;
            agree += (if dirs[p] > 0 { (*s.add(i + 1)).v >= (*s.add(i)).v } else { (*s.add(i + 1)).v <= (*s.add(i)).v }) as usize;
            i += 1;
        }
        p += 1;
    }
    if tot == 0 || (agree as f64) < 0.85 * tot as f64 { TRACE!("wave: {} pieces but only {} of {} pairs agree -> decline", np, agree, tot); return 0 }
    // THE CROSSING COST, from the sample: for every point, how many pieces' [min, max] cover it, priced ceil(log2 k) merge levels.
    {
        let pl = malloc(np * size_of::<f64>()) as *mut f64; let ph = malloc(np * size_of::<f64>()) as *mut f64;
        let mut p: usize = 0;
        while p < np {
            *pl.add(p) = f64::INFINITY; *ph.add(p) = f64::NEG_INFINITY;
            let mut i = starts[p];
            while i < starts[p + 1] { if (*s.add(i)).v < *pl.add(p) { *pl.add(p) = (*s.add(i)).v } if (*s.add(i)).v > *ph.add(p) { *ph.add(p) = (*s.add(i)).v } i += 1 }
            p += 1;
        }
        let mut lv: usize = 0;
        let mut i: usize = 0;
        while i < m {
            let mut k: usize = 0;
            let mut p: usize = 0;
            while p < np { k += ((*s.add(i)).v >= *pl.add(p) && (*s.add(i)).v <= *ph.add(p)) as usize; p += 1 }
            let mut l: usize = 0; while (1usize << l) < k { l += 1 } lv += l;
            i += 4;
        }
        free(pl); free(ph);
        let cost = lv as f64 / ((m + 3) / 4) as f64;
        if cost > WAVE_COST_MAX { TRACE!("wave: {} pieces, crossing cost {:.2} levels per element -> decline", np, cost); return 0 }
        TRACE!("wave: crossing cost {:.2} levels per element", cost);
    }
    // boundaries at the element's scale: the extremum between the last point of one piece and the first of the next
    let B = malloc((np + 1) * size_of::<usize>()) as *mut usize;
    *B = 0; *B.add(np) = n;
    let mut p: usize = 1;
    while p < np {
        let lo = (*s.add(starts[p] - 1)).pos; let hi = (*s.add(starts[p])).pos; let mut best = lo; let mut bv = *a.add(lo);
        if dirs[p - 1] > 0 { let mut q = lo; while q <= hi { if *a.add(q) > bv { bv = *a.add(q); best = q } q += 1 } }
        else               { let mut q = lo; while q <= hi { if *a.add(q) < bv { bv = *a.add(q); best = q } q += 1 } }
        *B.add(p) = best + 1;
        if *B.add(p) <= *B.add(p - 1) { *B.add(p) = *B.add(p - 1) + 1 }
        p += 1;
    }
    TRACE!("wave: {} pieces, {} of {} pairs agree -> sort each where it lies", np, agree, tot);
    let runs = malloc(2 * np * size_of::<usize>()) as *mut usize;
    let mut p: usize = 0;
    while p < np {
        let x = a.add(*B.add(p)); let len = *B.add(p + 1) - *B.add(p);
        if dirs[p] < 0 {
            let mut l: usize = 0; let mut r: usize = len.wrapping_sub(1);     // size_t, as the C: len - 1
            while l < r { let t = *x.add(l); *x.add(l) = *x.add(r); *x.add(r) = t; l += 1; r = r.wrapping_sub(1) }
        }
        g_reach_relax = 1; let ok = reach_try_f64(x, len, 1, 2048); g_reach_relax = 0;
        if ok == 0 {
            // NESTED_SORT(psort_f64(x, len, 1, NULL, NULL, NULL, NULL)), the macro of C line 5818 written out
            let cm_ = g_ctx_next; ns_depth.with(|d| d.set(d.get() + 1));
            psort_f64(x, len, 1, null_mut(), null_mut(), null_mut(), null_mut());
            ns_depth.with(|d| d.set(d.get() - 1)); g_ctx_next = cm_;
        }
        *runs.add(2 * p) = *B.add(p); *runs.add(2 * p + 1) = (*B.add(p + 1)).wrapping_sub(1);
        p += 1;
    }
    // LOGOS_WAVE_NOSWEEP unset
    // the sweep: the pieces are strands now; copy what lies alone, merge where they cross
    let mut d: *mut rdesc_t = null_mut(); let mut ch: *mut usize = null_mut(); let mut nc: usize = 0; let mut merged: usize = 0; let mut placed: i32 = 0;
    if runs_plan(a, runs, np, &mut d, &mut ch, &mut nc, &mut merged) != 0 {
        let mut K: knot_plan_t = core::mem::zeroed();
        if knot_plan(a, d, np, n + 1, 1, &mut K) != 0 {
            let krl: *mut usize = if K.n3 != 0 { knot_rl(&K) } else { null_mut() };
            if K.total == n && (K.n3 == 0 || !krl.is_null()) {
                if !(half_on(n) != 0 && knot_place_half(a, buf, n, &K, null(), 0) != 0) { knot_place_seq(a, buf, &K, krl); memcpy(a, buf, n * size_of::<f64>()); }
                placed = 1;                                                     // half the scratch where it can (half_place)
            }
            free(krl); knot_plan_free(&mut K);
        }
        free(d); free(ch);
    }
    if placed == 0 { merge_all_runs(a, buf, runs, np) }                    // the k-way merge: slower, still right
    TRACE!("wave: placed ({})", if placed != 0 { "knot sweep" } else { "k-way merge" });
    free(runs); free(B); g_wave_count += 1;
    1
}

// ===================== THE PRESCAN IN BLOCKS (C 8424) =====================
pub(crate) const PRESCAN_FAST_MIN: usize = 256;
// Each returns the start of the first block in which its test fires, or where whole blocks run out; the scalar line takes over there.
#[inline]
pub(crate) unsafe fn prescan_rise_or16(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 16 <= n { let mut f: i32 = 0; let mut k: usize = 0; while k < 16 { f |= (*a.add(i + k) < *a.add(i + k - 1)) as i32; k += 1 } if f != 0 { break } i += 16 }
    i
}
#[inline]
pub(crate) unsafe fn prescan_fall_or16(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 16 <= n { let mut f: i32 = 0; let mut k: usize = 0; while k < 16 { f |= (*a.add(i + k) > *a.add(i + k - 1)) as i32; k += 1 } if f != 0 { break } i += 16 }
    i
}
#[inline]
pub(crate) unsafe fn prescan_rise_sse2(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 16 <= n {
        let mut m = _mm_setzero_pd();
        let mut k: usize = 0;
        while k < 16 { m = _mm_or_pd(m, _mm_cmplt_pd(_mm_loadu_pd(a.add(i + k)), _mm_loadu_pd(a.add(i + k - 1)))); k += 2 }
        if _mm_movemask_pd(m) != 0 { break }
        i += 16;
    }
    i
}
#[inline]
pub(crate) unsafe fn prescan_fall_sse2(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 16 <= n {
        let mut m = _mm_setzero_pd();
        let mut k: usize = 0;
        while k < 16 { m = _mm_or_pd(m, _mm_cmpgt_pd(_mm_loadu_pd(a.add(i + k)), _mm_loadu_pd(a.add(i + k - 1)))); k += 2 }
        if _mm_movemask_pd(m) != 0 { break }
        i += 16;
    }
    i
}
#[inline(never)]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn prescan_rise_avx2(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 32 <= n {
        let mut m = _mm256_setzero_pd();
        let mut k: usize = 0;
        while k < 32 { m = _mm256_or_pd(m, _mm256_cmp_pd::<_CMP_LT_OQ>(_mm256_loadu_pd(a.add(i + k)), _mm256_loadu_pd(a.add(i + k - 1)))); k += 4 }
        if _mm256_movemask_pd(m) != 0 { break }
        i += 32;
    }
    i
}
#[inline(never)]
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn prescan_fall_avx2(a: *const f64, n: usize, mut i: usize) -> usize {
    while i + 32 <= n {
        let mut m = _mm256_setzero_pd();
        let mut k: usize = 0;
        while k < 32 { m = _mm256_or_pd(m, _mm256_cmp_pd::<_CMP_GT_OQ>(_mm256_loadu_pd(a.add(i + k)), _mm256_loadu_pd(a.add(i + k - 1)))); k += 4 }
        if _mm256_movemask_pd(m) != 0 { break }
        i += 32;
    }
    i
}
pub(crate) const g_pre_or16: i32 = 0;   // LOGOS_PRESCAN_OR16 unset
pub(crate) const g_pre_sse2: i32 = 0;   // LOGOS_PRESCAN_SSE2 unset
#[inline]
pub(crate) unsafe fn prescan_cpu() -> i32 { logos_simd_kind() }
#[inline]
pub(crate) unsafe fn prescan_kind(a: *const f64, n: usize, kind: i32) -> pscan_t {
    let mut r = pscan_t { first_desc: n, asc: 1, desc: 1 };
    let mut i: usize = 1;
    while i < n {                                          // until one test fires: a couple of elements on random data
        if *a.add(i) < *a.add(i - 1) { r.asc = 0; r.first_desc = i; i += 1; break }
        if *a.add(i) > *a.add(i - 1) { r.desc = 0; i += 1; break }
        i += 1;
    }
    if r.asc != 0 {                                        // rising or level so far: only a fall can change the answer
        i = if kind == 2 { prescan_rise_avx2(a, n, i) } else if kind == 1 { prescan_rise_sse2(a, n, i) } else { prescan_rise_or16(a, n, i) };
        while i < n { if *a.add(i) < *a.add(i - 1) { r.asc = 0; r.first_desc = i; break } i += 1 }
    } else {                                               // fell first: only a rise can change the answer
        i = if kind == 2 { prescan_fall_avx2(a, n, i) } else if kind == 1 { prescan_fall_sse2(a, n, i) } else { prescan_fall_or16(a, n, i) };
        while i < n { if *a.add(i) > *a.add(i - 1) { r.desc = 0; break } i += 1 }
    }
    r
}
#[inline(never)]
pub(crate) unsafe fn prescan_blocks(a: *const f64, n: usize) -> pscan_t {
    let mut kind = prescan_cpu();
    if n >= 65536 {     // the C reads its two A/B switches here (at ns_depth == 0); both are unset
        if g_pre_or16 != 0 { kind = 0 } else if g_pre_sse2 != 0 && kind > 1 { kind = 1 }
    }
    prescan_kind(a, n, kind)
}
// THE PRESCAN IN PARALLEL (C 8519)
pub(crate) const PRESCAN_PAR_MIN: usize = 1048576;
pub(crate) const PRESCAN_PAR_HEAD: usize = 65536;
pub(crate) const PRESCAN_PAR_T: usize = 8;
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct pspar_t { pub(crate) a: *const f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) found: usize, pub(crate) fall: i32, pub(crate) kind: i32 }
pub(crate) unsafe fn prescan_par_worker(p: *mut c_void) -> *mut c_void {
    let w = p as *mut pspar_t; let a = (*w).a; let mut i = (*w).lo; let hi = (*w).hi; let kind = (*w).kind; (*w).found = hi;
    if (*w).fall != 0 {                                    // rising so far: the first fall
        i = if kind == 2 { prescan_rise_avx2(a, hi, i) } else if kind == 1 { prescan_rise_sse2(a, hi, i) } else { prescan_rise_or16(a, hi, i) };
        while i < hi { if *a.add(i) < *a.add(i - 1) { (*w).found = i; break } i += 1 }
    } else {                                               // falling so far: the first rise
        i = if kind == 2 { prescan_fall_avx2(a, hi, i) } else if kind == 1 { prescan_fall_sse2(a, hi, i) } else { prescan_fall_or16(a, hi, i) };
        while i < hi { if *a.add(i) > *a.add(i - 1) { (*w).found = i; break } i += 1 }
    }
    null_mut()
}
pub(crate) unsafe fn prescan_par_run(a: *const f64, n: usize, P: i32) -> pscan_t {
    let h = prescan_blocks(a, PRESCAN_PAR_HEAD);           // also reads the kind switches, as the single scan does
    let fall: i32;
    if h.asc != 0 && h.desc == 0 { fall = 1 }
    else if h.asc == 0 && h.desc != 0 { fall = 0 }
    else if h.asc == 0 && h.desc == 0 { return pscan_t { first_desc: h.first_desc, asc: 0, desc: 0 } }   // broken both ways inside the head: final
    else { return prescan_blocks(a, n) }                   // level so far: the plain scan
    let mut kind = prescan_cpu(); if g_pre_or16 != 0 { kind = 0 } else if g_pre_sse2 != 0 && kind > 1 { kind = 1 }
    let T: i32 = if P < PRESCAN_PAR_T as i32 { P } else { PRESCAN_PAR_T as i32 };
    let mut w: [pspar_t; PRESCAN_PAR_T] = core::mem::zeroed(); let mut th: [pthread_t; PRESCAN_PAR_T] = core::mem::zeroed();
    let lo = PRESCAN_PAR_HEAD; let per = (n - lo + T as usize - 1) / T as usize;
    let mut t: i32 = 0;
    while t < T {
        let mut s = lo + t as usize * per; let mut e = s + per; if s > n { s = n } if e > n { e = n }
        w[t as usize] = pspar_t { a, lo: s, hi: e, found: e, fall, kind };
        t += 1;
    }
    par_run(th.as_mut_ptr(), T, prescan_par_worker, w.as_mut_ptr() as *mut c_void, size_of::<pspar_t>(), 1);
    let mut f = n;
    let mut t: i32 = 0;
    while t < T { if w[t as usize].found < w[t as usize].hi { f = w[t as usize].found; break } t += 1 }
    let mut r: pscan_t = core::mem::zeroed();
    if fall != 0 { r.first_desc = f; r.asc = (f == n) as i32; r.desc = 0; }                 // a rise was seen in the head
    else         { r.first_desc = h.first_desc; r.asc = 0; r.desc = (f == n) as i32; }      // the first fall was in the head
    r
}
#[inline(never)]
pub(crate) unsafe fn prescan_par(a: *const f64, n: usize, P: i32) -> pscan_t {
    // LOGOS_NO_PARPRESCAN unset
    prescan_par_run(a, n, P)
}
/// test hook: the scalar line, exactly as the dispatcher runs it, against every kind of block
pub(crate) unsafe fn logos_prescan_agree(a: *const f64, n: usize) -> i32 {
    let mut asc: i32 = 1; let mut desc: i32 = 1; let mut first_desc = n;
    let mut i: usize = 1;
    while i < n && (asc != 0 || desc != 0) { if asc != 0 && *a.add(i) < *a.add(i - 1) { asc = 0; first_desc = i } if *a.add(i) > *a.add(i - 1) { desc = 0 } i += 1 }
    if n < 2 { return 1 }
    let mut kind: i32 = 0;
    while kind <= prescan_cpu() {
        let r = prescan_kind(a, n, kind);
        if !(r.asc == asc && r.desc == desc && r.first_desc == first_desc) { return 0 }
        kind += 1;
    }
    if n >= PRESCAN_PAR_MIN {                                              // and the parallel scan, at four thread counts
        let mut P: i32 = 2;
        while P <= 16 {
            let r = prescan_par_run(a, n, P);
            if !(r.asc == asc && r.desc == desc && r.first_desc == first_desc) { return 0 }
            P *= 2;
        }
    }
    1
}

/// The vector two-strand merge's switch (C 8595). In C: `g_simd_merge = logos_simd_kind() == 2 && !getenv(LOGOS_NO_SIMD_MERGE);
/// g_runmerge_vec = !getenv(LOGOS_RUNMERGE_SCALAR)`. Both switches are unset, so the flags hold their unset values, which the
/// knot file owns: nothing to do here.
#[inline]
pub(crate) unsafe fn simd_merge_env() { g_simd_merge = (logos_simd_kind() == 2) as i32 }

/// The per-mode certificate from the SPLITTERS (see mdx_t; C 8603): modes at the gulfs, each tested for uniformity on its own
/// sample slice; up to MD_LIN_MAX modes go to the compare lookup in the job, more to the grid. Returns 1 if every mode
/// certified. noinline: its arrays stay out of the dispatcher's frame.

// IS THE SAMPLE UNSTRUCTURED? (C 8665; the function itself, sample_unstructured, is part.rs's)

// ---- what this file calls and other files define --------------------------------------------------------------------------
// The bodies are minimal stand-ins so that this file's tests run alone; with `port_all` they vanish.
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    unsafe fn stub_sort(a: *mut f64, n: usize) {
        if n > 1 { core::slice::from_raw_parts_mut(a, n).sort_unstable_by(|x, y| x.partial_cmp(y).unwrap()) }
    }
    // base.rs
    pub(crate) type pthread_t = usize;
    pub(crate) static mut g_ctx_next: i32 = 0;
    pub(crate) unsafe fn sample_read(a: *const f64, n: usize, from: usize, to: usize, mfull: usize, out: *mut f64, has_nan: *mut i32) {
        let mut i = from;
        while i < to { let q = SAMPLE_POS(i, mfull, n); let v = *a.add(if q < n { q } else { n - 1 }); *out.add(i - from) = v; if v != v { *has_nan = 1 } i += 1 }
    }
    pub(crate) unsafe fn sample_sort(s: *mut f64, m: usize) -> usize {
        let mut k = m; let mut i: usize = 0;
        while i < k { if *s.add(i) != *s.add(i) { let v = *s.add(i); k -= 1; *s.add(i) = *s.add(k); *s.add(k) = v } else { i += 1 } }
        stub_sort(s, k);
        m - k
    }
    pub(crate) unsafe fn pool_acquire(buf_bytes: usize, bidx_bytes: usize, buf: *mut *mut c_void, bidx: *mut *mut u8) -> i32 { 0 }
    pub(crate) unsafe fn pool_release() {}
    pub(crate) unsafe fn par_run(th: *mut pthread_t, P: i32, f: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, run0: i32) {
        let mut t: i32 = if run0 != 0 { 0 } else { 1 };
        while t < P { f((args as *mut u8).add(t as usize * stride) as *mut c_void); t += 1 }
    }
    // leaf.rs
    pub(crate) unsafe fn leaf_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 { stub_sort(a, n); a }
    pub(crate) unsafe fn merge_all_runs(a: *mut f64, buf: *mut f64, runs: *const usize, n_runs: usize) {
        let lo = *runs; let hi = *runs.add(2 * n_runs - 1); stub_sort(a.add(lo), hi + 1 - lo);
    }
    // knot.rs
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct kpiece_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize, pub(crate) copy: i32, pub(crate) rev: i32 }
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct kseg_t { pub(crate) p0: usize, pub(crate) k: usize, pub(crate) dst: usize, pub(crate) len: usize }
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct knot_plan_t {
        pub(crate) pc: *mut kpiece_t, pub(crate) npc: usize, pub(crate) cap_pc: usize, pub(crate) sg: *mut kseg_t, pub(crate) nsg: usize, pub(crate) cap_sg: usize,
        pub(crate) n3: usize, pub(crate) crossing: usize, pub(crate) cost: usize, pub(crate) kmax: usize, pub(crate) total: usize, pub(crate) last_single: i32,
    }
    pub(crate) unsafe fn runs_plan(a: *const f64, runs: *const usize, R: usize, pd: *mut *mut rdesc_t, pchain: *mut *mut usize, pnchain: *mut usize, pmerged: *mut usize) -> i32 { 0 }
    pub(crate) unsafe fn knot_plan(a: *const f64, d: *const rdesc_t, R: usize, grain: usize, emit: i32, K: *mut knot_plan_t) -> i32 { unimplemented!() }
    pub(crate) unsafe fn knot_plan_free(K: *mut knot_plan_t) { unimplemented!() }
    pub(crate) unsafe fn knot_rl(K: *const knot_plan_t) -> *mut usize { unimplemented!() }
    pub(crate) unsafe fn knot_place_seq(a: *mut f64, buf: *mut f64, K: *const knot_plan_t, rl: *const usize) { unimplemented!() }
    pub(crate) unsafe fn knot_place_half(a: *mut f64, buf: *mut f64, N: usize, K: *const knot_plan_t, st: *const f64, S: usize) -> i32 { unimplemented!() }
    pub(crate) unsafe fn half_on(n: usize) -> i32 { unimplemented!() }
    // part.rs
    pub(crate) const BT_MODE_GAP: f64 = 10.0;
    pub(crate) const MD_MAX: usize = 64;
    pub(crate) const MD_LIN_MAX: i32 = 8;
    pub(crate) const MD_GRID_CELLS: usize = 4096;
    pub(crate) const HF_KS_MAX: f64 = 2.0;
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct mdx_t {
        pub(crate) nm: i32, pub(crate) cells: i32, pub(crate) glo: f64, pub(crate) gscale: f64,
        pub(crate) first: [i32; 64], pub(crate) last: [i32; 64], pub(crate) lo: [f64; 65], pub(crate) hi: [f64; 64], pub(crate) scale: [f64; 64],
        pub(crate) grid: [u8; 4096],
    }
    #[inline(always)]
    pub(crate) unsafe fn md_cell(g: *const mdx_t, x: f64) -> i32 {
        let t = (x - (*g).glo) * (*g).gscale;
        if t >= 0.0 { if t < (*g).cells as f64 { t as i32 } else { (*g).cells - 1 } } else { 0 }
    }
    /// Only the fields md_certify writes; the C's job_t (line 3665) has many more.
    #[repr(C)] #[derive(Clone, Copy)]
    pub(crate) struct job_t {
        pub(crate) md_ok: i32, pub(crate) md_nm: i32, pub(crate) md_first: [i32; 8], pub(crate) md_last: [i32; 8],
        pub(crate) md_lo: [f64; 8], pub(crate) md_hi: [f64; 8], pub(crate) md_scale: [f64; 8], pub(crate) mdx: *mut mdx_t,
    }
    // ord1.rs
    pub(crate) static mut g_reach_relax: i32 = 0;
    pub(crate) static mut g_seamed: i32 = 0;
    pub(crate) static mut g_seam_count: usize = 0;
    pub(crate) static mut g_reach_seam: usize = 0;
    thread_local! { pub(crate) static ns_depth: Cell<i32> = Cell::new(0); }
    pub(crate) const REACH_MIN_N: usize = 1 << 18;
    pub(crate) const TAIL_MIN_N: usize = 4096;
    pub(crate) unsafe fn reach_from_sample(a: *const f64, n: usize, raw: *const f64, m: usize, mirror: i32) -> usize { 0 }
    pub(crate) unsafe fn reach_try_f64(a: *mut f64, n: usize, P: i32, reach: usize) -> i32 { 0 }
    // disp.rs
    pub(crate) unsafe fn psort_f64(a: *mut f64, n: usize, P: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) { stub_sort(a, n) }
}
#[cfg(not(feature = "port_all"))]
use missing::*;

#[cfg(test)]
mod tests {
    use super::*;

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15); let mut z = self.0; z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9); z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb); z ^ (z >> 31) }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
        fn below(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
        fn shuffle(&mut self, v: &mut [f64]) { for i in (1..v.len()).rev() { let j = self.below(i + 1); v.swap(i, j) } }
    }
    fn assert_sorted_perm(out: &[f64], input: &[f64]) {
        for i in 1..out.len() { assert!(!(out[i] < out[i - 1]), "descent at {}", i) }
        let mut x: Vec<u64> = out.iter().map(|v| v.to_bits()).collect(); let mut y: Vec<u64> = input.iter().map(|v| v.to_bits()).collect();
        x.sort_unstable(); y.sort_unstable();
        assert!(x == y, "not a permutation of the input's bit patterns");
    }
    fn bits(v: &[f64]) -> Vec<u64> { v.iter().map(|x| x.to_bits()).collect() }

    // ---- the prescan ----
    fn scan_ref(a: &[f64]) -> (usize, i32, i32) {
        let n = a.len(); let (mut asc, mut desc, mut fd) = (1, 1, n);
        let mut i = 1;
        while i < n && (asc != 0 || desc != 0) { if asc != 0 && a[i] < a[i - 1] { asc = 0; fd = i } if a[i] > a[i - 1] { desc = 0 } i += 1 }
        (fd, asc, desc)
    }
    fn check_scan(a: &[f64], what: &str) {
        let n = a.len(); let want = scan_ref(a);
        unsafe {
            for kind in 0..=prescan_cpu() {
                let r = prescan_kind(a.as_ptr(), n, kind);
                assert_eq!((r.first_desc, r.asc, r.desc), want, "prescan_kind {} on {} (n {})", kind, what, n);
            }
            let r = prescan_blocks(a.as_ptr(), n);
            assert_eq!((r.first_desc, r.asc, r.desc), want, "prescan_blocks on {} (n {})", what, n);
            if n >= PRESCAN_PAR_MIN {
                for &P in &[2, 3, 4, 8, 14, 16] {
                    let r = prescan_par(a.as_ptr(), n, P);
                    assert_eq!((r.first_desc, r.asc, r.desc), want, "prescan_par P {} on {} (n {})", P, what, n);
                }
            }
            assert_eq!(logos_prescan_agree(a.as_ptr(), n), 1, "logos_prescan_agree on {} (n {})", what, n);
        }
    }
    fn positions(n: usize) -> Vec<usize> {
        let mut p: Vec<usize> = vec![1, 2, 3, 15, 16, 17, 18, 31, 32, 33, 34, 47, 48, 49, 63, 64, 65, 255, 256, 257, n / 2, n - 34, n - 33, n - 32, n - 17, n - 16, n - 15, n - 3, n - 2, n - 1];
        for b in [65536usize, 65536 + 16, 65536 + 32] { for d in 0..3 { p.push(b + d - 1) } }
        if n >= PRESCAN_PAR_MIN {
            for &T in &[2usize, 3, 4, 8] {
                let per = (n - PRESCAN_PAR_HEAD + T - 1) / T;
                for t in 1..T { for d in 0..5 { p.push(PRESCAN_PAR_HEAD + t * per + d - 2) } }
            }
        }
        p.retain(|&q| q >= 1 && q < n); p.sort_unstable(); p.dedup(); p
    }
    #[test]
    fn prescan_matches_the_scalar_line() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(1);
        for &n in &[2usize, 3, 17, 255, 256, 300, 1000, 65535, 65536, 70001, PRESCAN_PAR_MIN, PRESCAN_PAR_MIN + 77] {
            let asc: Vec<f64> = (0..n).map(|i| i as f64).collect();
            let desc: Vec<f64> = (0..n).map(|i| (n - i) as f64).collect();
            let ties_up: Vec<f64> = (0..n).map(|i| (i / 3) as f64).collect();           // equal neighbours
            let ties_down: Vec<f64> = (0..n).map(|i| ((n - i) / 3) as f64).collect();
            let level = vec![1.5f64; n];
            let mut inf_up = asc.clone(); inf_up[0] = f64::NEG_INFINITY; inf_up[n - 1] = f64::INFINITY; if n > 4 { inf_up[n - 2] = f64::INFINITY }
            let mut inf_down = desc.clone(); inf_down[0] = f64::INFINITY; inf_down[n - 1] = f64::NEG_INFINITY; if n > 4 { inf_down[1] = f64::INFINITY }
            let random: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
            let zeros: Vec<f64> = (0..n).map(|i| if i % 2 == 0 { -0.0 } else { 0.0 }).collect();
            for (v, name) in [(&asc, "ascending"), (&desc, "descending"), (&ties_up, "ties up"), (&ties_down, "ties down"), (&level, "level"),
                              (&inf_up, "infinities up"), (&inf_down, "infinities down"), (&random, "random"), (&zeros, "signed zeros")] { check_scan(v, name) }
            if n < 40 { continue }
            for p in positions(n) {
                // one descent in an ascending array, one ascent in a descending one, and the same after level starts
                let mut v = asc.clone(); v[p] = v[p - 1] - 0.5; check_scan(&v, "one descent");
                let mut v = desc.clone(); v[p] = v[p - 1] + 0.5; check_scan(&v, "one ascent");
                let mut v = ties_up.clone(); v[p] = v[p - 1] - 0.5; check_scan(&v, "one descent among ties");
                let mut v = level.clone(); v[p] = 1.0; check_scan(&v, "level, then a fall");
                let mut v = level.clone(); v[p] = 2.0; check_scan(&v, "level, then a rise");
                if p + 40 < n { let mut v = asc.clone(); v[p] = v[p - 1] - 0.5; v[p + 37] = -1.0; check_scan(&v, "two descents") }
                if p + 40 < n { let mut v = desc.clone(); v[p] = v[p - 1] + 0.5; v[p + 37] = 1e18; check_scan(&v, "two ascents") }
            }
        }
    }

    // ---- rotations ----
    #[test]
    fn rotate_left_matches_a_reference() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(2);
        for n in 1usize..=12 { for s in 0..=n {
            let v: Vec<f64> = (0..n).map(|i| i as f64 + 0.25).collect();
            let mut want = v.clone(); want.rotate_left(s);
            let mut got = v.clone(); unsafe { rotate_left_f64(got.as_mut_ptr(), n, s) }
            assert_eq!(bits(&got), bits(&want), "n {} s {}", n, s);
        } }
        for &n in &[1000usize, 65537, 300001] { for &s in &[0usize, 1, 2, n / 3, n / 2, n / 2 + 1, n - 2, n - 1, n] {
            let v: Vec<f64> = (0..n).map(|_| rng.unit() - 0.5).collect();
            let mut want = v.clone(); want.rotate_left(s);
            let mut got = v.clone(); unsafe { rotate_left_f64(got.as_mut_ptr(), n, s) }
            assert_eq!(bits(&got), bits(&want), "n {} s {}", n, s);
        } }
    }

    // ---- streams ----
    unsafe fn modes_of(a: &[f64], bnd: &mut [f64; STREAMS_MAX_MODES]) -> (i32, f64, f64) {
        let m = 2048usize; let mut samp = vec![0.0f64; m]; let mut nan = 0i32;
        sample_read(a.as_ptr(), a.len(), 0, m, m, samp.as_mut_ptr(), &mut nan);
        assert_eq!(nan, 0); assert_eq!(sample_sort(samp.as_mut_ptr(), m), 0);
        (streams_modes(samp.as_ptr(), m, bnd.as_mut_ptr()), samp[0], samp[m - 1])
    }
    fn streams(n: usize, k: usize) -> Vec<f64> { (0..n).map(|i| (i % k) as f64 * 1e7 + (i / k) as f64 * 0.5).collect() }
    #[test]
    fn streams_are_taken() { let _port_guard = crate::port::port_test_lock();
        for &(n, k) in &[(200000usize, 4usize), (200003, 60), (70000, 2), (240000, 60)] {
            let input = streams(n, k);
            let mut a = input.clone(); let mut buf = vec![0.0f64; n]; let mut bnd = [0.0f64; STREAMS_MAX_MODES];
            unsafe {
                let (nm, lo, hi) = modes_of(&a, &mut bnd);
                assert_eq!(nm as usize, k, "modes of {} streams", k);
                for q in 0..k - 1 { assert!(bnd[q] > q as f64 * 1e7 + n as f64 && bnd[q] < (q + 1) as f64 * 1e7, "boundary {}", q) }
                assert_eq!(streams_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), null_mut(), bnd.as_ptr(), nm, lo, hi), 1, "{} streams", k);
            }
            assert_sorted_perm(&a, &input);
        }
    }
    #[test]
    fn streams_with_a_dirty_bucket_and_edge_values() { let _port_guard = crate::port::port_test_lock();
        // a few swapped pairs inside the streams (under the probe's 2%), values outside the sample's range, signed zeros
        for &(n, k) in &[(200000usize, 4usize), (240000, 60)] {
            let mut input = streams(n, k); let mut rng = Rng(3);
            for _ in 0..40 { let i = rng.below(n - 2 * k); input.swap(i, i + k) }
            input[5] = f64::NEG_INFINITY; input[n - 7] = f64::INFINITY; input[11] = -0.0; input[n / 2] = 0.0;
            let mut a = input.clone(); let mut buf = vec![0.0f64; n]; let mut bnd = [0.0f64; STREAMS_MAX_MODES];
            unsafe {
                let (nm, lo, hi) = modes_of(&a, &mut bnd);
                assert_eq!(nm as usize, k);
                assert_eq!(streams_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), null_mut(), bnd.as_ptr(), nm, lo, hi), 1);
            }
            assert_sorted_perm(&a, &input);
        }
    }
    #[test]
    fn streams_decline_noise() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(4); let n = 200000usize;
        let uniform: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
        let two: Vec<f64> = (0..n).map(|_| if rng.next() & 1 == 0 { rng.unit() } else { 1e6 + rng.unit() }).collect();   // two modes, no order
        let twelve: Vec<f64> = (0..n).map(|_| rng.below(12) as f64 * 1e6 + rng.unit()).collect();                      // the table's path
        for (input, name, want_modes) in [(&uniform, "uniform", 0), (&two, "two clusters", 2), (&twelve, "twelve clusters", 12)] {
            let mut a = input.clone(); let mut buf = vec![0.0f64; n]; let mut bnd = [0.0f64; STREAMS_MAX_MODES];
            unsafe {
                let (nm, lo, hi) = modes_of(&a, &mut bnd);
                assert!(nm >= want_modes && (nm == 0 || nm >= 2), "{}: {} modes", name, nm);   // noise may show a few gulfs more
                assert_eq!(streams_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), null_mut(), bnd.as_ptr(), nm, lo, hi), 0, "{} must decline", name);
            }
            assert_eq!(bits(&a), bits(input), "{}: a declined array is untouched", name);
        }
        // a NaN the probe misses: the lookup pass declines, array untouched
        let mut input = streams(n, 4); input[12345] = f64::NAN;
        let clean = streams(n, 4);
        let mut a = input.clone(); let mut buf = vec![0.0f64; n]; let mut bnd = [0.0f64; STREAMS_MAX_MODES];
        unsafe {
            let (nm, lo, hi) = modes_of(&clean, &mut bnd);
            let r = streams_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), null_mut(), bnd.as_ptr(), nm, lo, hi);
            assert_eq!(r, 0);
        }
        assert_eq!(bits(&a), bits(&input));
    }
    #[test]
    fn streams_interleaved_at_random() { let _port_guard = crate::port::port_test_lock();
        // each element comes from a stream picked at random: what a shared table of per-tenant sequences looks like
        let mut rng = Rng(12);
        for &k in &[4usize, 16] {
            let n = 300000usize; let mut next = vec![0usize; k];
            let input: Vec<f64> = (0..n).map(|_| { let q = rng.below(k); next[q] += 1; q as f64 * 1e7 + next[q] as f64 * 0.5 }).collect();
            let mut a = input.clone(); let mut buf = vec![0.0f64; n]; let mut bnd = [0.0f64; STREAMS_MAX_MODES];
            unsafe {
                let (nm, lo, hi) = modes_of(&a, &mut bnd);
                assert!(nm as usize >= k, "{} streams read as {} modes", k, nm);
                assert_eq!(streams_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), null_mut(), bnd.as_ptr(), nm, lo, hi), 1, "{} streams", k);
            }
            assert_sorted_perm(&a, &input);
        }
    }
    #[test]
    fn streams_cell_and_mode() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            assert_eq!(streams_cell(f64::NAN, 0.0, 1.0), 0);
            assert_eq!(streams_cell(-5.0, 0.0, 1.0), 0);
            assert_eq!(streams_cell(1e30, 0.0, 1.0), 4095);
            assert_eq!(streams_cell(f64::INFINITY, 0.0, 1.0), 4095);
            assert_eq!(streams_cell(7.9, 0.0, 1.0), 7);
            let b = [1.0, 2.0, 3.0];
            assert_eq!(streams_mode(0.5, b.as_ptr(), 3), 0); assert_eq!(streams_mode(2.0, b.as_ptr(), 3), 2); assert_eq!(streams_mode(9.0, b.as_ptr(), 3), 3);
            assert_eq!(streams_mode(f64::NAN, b.as_ptr(), 3), 0);
        }
    }

    // ---- the mirror gate ----
    #[test]
    fn mirror_gate_names_its_gate() { let _port_guard = crate::port::port_test_lock();
        let n = 200000usize;
        let rev: Vec<f64> = (0..n).map(|i| (n - i) as f64).collect();
        let mut rng = Rng(5); let noise: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
        let mstreams: Vec<f64> = streams(n, 4).into_iter().rev().collect();
        unsafe {
            assert_eq!(mirror_gate(rev.as_ptr(), n, 1.0, null(), 0), 1);
            assert_eq!(mirror_gate(rev.as_ptr(), n, 0.5, null(), 0), 0);
            assert_eq!(mirror_gate(noise.as_ptr(), n, 0.95, null(), 0), 0);
            let mut bnd = [0.0f64; STREAMS_MAX_MODES];
            let (nm, _, _) = modes_of(&mstreams, &mut bnd);
            assert_eq!(nm, 4);
            assert_eq!(mirror_gate(mstreams.as_ptr(), n, 0.5, bnd.as_ptr(), nm), 3);
            assert_eq!(mirror_gate(streams(n, 4).as_ptr(), n, 0.5, bnd.as_ptr(), nm), 0);
            assert_eq!(logos_mirror_count(0), g_mirror_count);
        }
    }

    // ---- the loop-back ----
    #[test]
    fn loopback_on_a_rotated_sorted_array() { let _port_guard = crate::port::port_test_lock();
        let n = 10000usize; let sorted: Vec<f64> = (0..n).map(|i| i as f64 * 1.5).collect();
        for &r in &[3000usize, 5000, 9000, 700] {
            let mut a = sorted.clone(); a.rotate_left(r);
            let d = n - r;                                         // the prescan's first descent
            unsafe {
                assert_eq!(prescan_blocks(a.as_ptr(), n).first_desc, d);
                assert_eq!(loopback_gate(a.as_ptr(), n, d, 0), d, "rotation {}", r);
                assert_eq!(loopback_gate(a.as_ptr(), n, d, 1), d, "rotation {} at P threads", r);
                rotate_left_f64(a.as_mut_ptr(), n, d);
            }
            assert_eq!(bits(&a), bits(&sorted));
        }
        unsafe {
            // a suffix under a sixteenth behind a prefix past a half: the tail route's
            let mut a = sorted.clone(); a.rotate_left(100);
            assert_eq!(loopback_gate(a.as_ptr(), n, n - 100, 0), 0);
            // the end does not continue into the start
            let mut a = sorted.clone(); a.swap(10, 11);
            assert_eq!(loopback_gate(a.as_ptr(), n, 11, 0), 0);
            // 30% prefix + noise + 40% suffix: one thread takes it, P threads do not
            let mut rng = Rng(6); let mut a = sorted.clone(); a.rotate_left(6000);       // [6000..10000) then [0..6000)
            for i in 7000..10000 { a[i] = rng.unit() * 1e4 }                            // prefix 4000 kept, then 3000 kept ... noise at the end
            a.rotate_left(3000);                                                        // noise | suffix-run | ... a general shape: only check consistency
            let d = prescan_blocks(a.as_ptr(), n).first_desc;
            let s = loopback_gate(a.as_ptr(), n, d, 0);
            if s != 0 { assert!(s > d && a[n - 1] <= a[0]); for i in s + 1..n { assert!(a[i - 1] <= a[i]) } assert!(n - s + d >= n / 2) }
            // a NaN at either end declines
            let mut a = sorted.clone(); a.rotate_left(3000); a[0] = f64::NAN;
            assert_eq!(loopback_gate(a.as_ptr(), n, 1, 0), 0);
        }
    }

    // ---- the sample in position order ----
    #[test]
    fn sample_by_pos_is_in_position_order() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(7);
        for &(n, m) in &[(100000usize, 2048usize), (1 << 20, 2048), (5000, 384), (3000, 2048)] {
            let a: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
            unsafe {
                let s = sample_by_pos(a.as_ptr(), n, m);
                assert!(!s.is_null());
                let mut want: Vec<usize> = (0..m).map(|i| SAMPLE_POS(i, m, n)).collect(); want.sort_unstable();
                for i in 0..m {
                    let p = *s.add(i);
                    assert!(p.pos < n && p.v.to_bits() == a[p.pos].to_bits());
                    if i > 0 { assert!((*s.add(i - 1)).pos <= p.pos, "positions ascend") }
                    assert_eq!(p.pos, want[i]);
                }
                free(s);
                let mut b = a.clone(); b[SAMPLE_POS(5, m, n)] = f64::NAN;
                assert!(sample_by_pos(b.as_ptr(), n, m).is_null(), "a NaN in the sample declines");
            }
        }
    }

    // ---- cuts ----
    fn windows(n: usize, w: usize, rng: &mut Rng) -> Vec<f64> {
        let mut a: Vec<f64> = (0..n).map(|i| i as f64).collect();
        let mut lo = 0; while lo < n { let hi = (lo + w).min(n); rng.shuffle(&mut a[lo..hi]); lo = hi }
        a
    }
    #[test]
    fn cuts_minmax_matches() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(8);
        for n in 1usize..40 {
            let v: Vec<f64> = (0..n).map(|_| rng.unit() - 0.5).collect();
            let (mut mn, mut mx) = (0.0, 0.0); unsafe { cuts_minmax(v.as_ptr(), n, &mut mn, &mut mx) }
            assert_eq!(mn, v.iter().cloned().fold(f64::INFINITY, f64::min)); assert_eq!(mx, v.iter().cloned().fold(f64::NEG_INFINITY, f64::max));
        }
    }
    #[test]
    fn cuts_route() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(9); let n = (1usize << 20) + 1234; let m = 2048usize;
        for &(w, must) in &[(16384usize, true), (10000, true), (65536, true), (300, false)] {
            let input = windows(n, w, &mut rng);
            let mut a = input.clone(); let mut buf = vec![0.0f64; n];
            unsafe {
                let ps = sample_by_pos(a.as_ptr(), n, m);
                g_cuts_sorts.store(0, SeqCst);
                g_ctx_next = 0; let mut c: ctx_t = core::mem::zeroed(); ctx_init(&mut c, n, 0); c.seq = 1;   // a real context, as the dispatcher passes
                let r = cuts_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c, ps, m);
                ctx_free(&mut c);
                free(ps);
                if must { assert_eq!(r, 1, "windows of {}", w) }
                if r != 0 { assert_sorted_perm(&a, &input); assert_eq!(g_cuts_sorts.load(SeqCst), 1) } else { assert_eq!(bits(&a), bits(&input)); assert_eq!(g_cuts_sorts.load(SeqCst), 0) }
            }
        }
        let input: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
        let mut a = input.clone(); let mut buf = vec![0.0f64; n];
        unsafe {
            let ps = sample_by_pos(a.as_ptr(), n, m);
            g_ctx_next = 0; let mut c: ctx_t = core::mem::zeroed(); ctx_init(&mut c, n, 0); c.seq = 1;
            assert_eq!(cuts_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c, ps, m), 0, "noise has no cuts");
            assert_eq!(cuts_try_f64(a.as_mut_ptr(), CUTS_MIN_N - 1, buf.as_mut_ptr(), &mut c, ps, m), 0);
            ctx_free(&mut c);
            free(ps);
        }
        assert_eq!(bits(&a), bits(&input));
    }

    // ---- the seam ----
    #[test]
    fn seam_gate_finds_the_rotation() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(10); let n = 1usize << 20; let m = 2048usize;
        for &r in &[n / 3, n / 2, n - n / 5] {
            for &noise in &[0.0f64, 40.0] {
                let a: Vec<f64> = (0..n).map(|i| ((i + r) % n) as f64 + noise * (rng.unit() - 0.5)).collect();
                unsafe {
                    let ps = sample_by_pos(a.as_ptr(), n, m);
                    let s = seam_gate(a.as_ptr(), n, ps, m);
                    free(ps);
                    if noise == 0.0 { assert_eq!(s, n - r) } else { assert!(s != 0 && (s as i64 - (n - r) as i64).abs() <= 64, "seam {} for rotation {}", s, n - r) }
                }
            }
        }
        let asc: Vec<f64> = (0..n).map(|i| i as f64).collect();
        let noise: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
        let mut near_end = asc.clone(); near_end.rotate_left(n - n / 64);
        for v in [&asc, &noise, &near_end] { unsafe {
            let ps = sample_by_pos(v.as_ptr(), n, m);
            assert_eq!(seam_gate(v.as_ptr(), n, ps, m), 0);
            free(ps);
        } }
    }

    // ---- the wave ----
    fn wave(n: usize, cycles: f64, swing: f64, noise: f64, rng: &mut Rng) -> Vec<f64> {
        let period = n as f64 / cycles;
        (0..n).map(|i| i as f64 + swing * period * (2.0 * core::f64::consts::PI * i as f64 / period).sin() + noise * (rng.unit() - 0.5)).collect()
    }
    #[test]
    fn wave_pieces_of_a_triangle() { let _port_guard = crate::port::port_test_lock();
        let m = 2048usize;
        let s: Vec<psamp_t> = (0..m).map(|i| { let ph = i % 512; psamp_t { pos: i * 10, v: if ph < 256 { ph as f64 } else { (512 - ph) as f64 } } }).collect();
        let mut starts = [0usize; 513]; let mut dirs = [0i32; 512];
        unsafe {
            let np = wave_pieces(s.as_ptr(), m, starts.as_mut_ptr(), dirs.as_mut_ptr(), 512);
            assert_eq!(np, 8);
            assert_eq!(starts[0], 0); assert_eq!(starts[np], m);
            for p in 0..np { assert_eq!(dirs[p], if p % 2 == 0 { 1 } else { -1 }); if p > 0 { assert!((starts[p] as i64 - (p * 256) as i64).abs() <= 3, "start {} = {}", p, starts[p]) } }
            assert_eq!(wave_pieces(s.as_ptr(), m, starts.as_mut_ptr(), dirs.as_mut_ptr(), 7), 0, "more pieces than room: 0");
            let line: Vec<psamp_t> = (0..m).map(|i| psamp_t { pos: i, v: i as f64 }).collect();
            assert_eq!(wave_pieces(line.as_ptr(), m, starts.as_mut_ptr(), dirs.as_mut_ptr(), 512), 1);
        }
    }
    #[test]
    fn wave_route() { let _port_guard = crate::port::port_test_lock();
        let mut rng = Rng(11); let n = 1usize << 20; let m = 2048usize;
        let mut took = 0;
        for &(cycles, swing, noise) in &[(32.0f64, 0.3f64, 50.0f64), (8.0, 0.3, 50.0), (16.0, 0.3, 0.0), (32.0, 3.0, 50.0)] {
            let input = wave(n, cycles, swing, noise, &mut rng);
            let mut a = input.clone(); let mut buf = vec![0.0f64; n];
            unsafe {
                let ps = sample_by_pos(a.as_ptr(), n, m);
                let before = g_wave_count;
                let r = wave_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), ps, m);
                free(ps);
                if r != 0 { took += 1; assert_sorted_perm(&a, &input); assert!(g_wave_count > before) } else { assert_eq!(bits(&a), bits(&input), "a declined array is untouched") }
                if swing == 0.3 { assert_eq!(r, 1, "{} cycles, swing {}", cycles, swing) }
                if swing == 3.0 { assert_eq!(r, 0, "a wide swing is declined on its crossing cost") }
                assert_eq!(g_reach_relax, 0); assert_eq!(ns_depth.with(|d| d.get()), 0);
            }
        }
        assert!(took >= 2);
        let input: Vec<f64> = (0..n).map(|_| rng.unit()).collect();
        let mut a = input.clone(); let mut buf = vec![0.0f64; n];
        unsafe { let ps = sample_by_pos(a.as_ptr(), n, m); assert_eq!(wave_try_f64(a.as_mut_ptr(), n, buf.as_mut_ptr(), ps, m), 0); free(ps) }
        assert_eq!(bits(&a), bits(&input));
    }

    // ---- the per-mode certificate ----
    fn clusters(k: usize, per: usize) -> Vec<f64> { (0..k * per).map(|i| (i / per) as f64 * 100.0 + (i % per) as f64 / per as f64).collect() }
    #[test]
    fn md_certify_compares_and_grid() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            // two modes: the compare lookup in the job
            let samp = clusters(2, 1024); let sp: Vec<f64> = (1..32).map(|i| samp[i * 64]).collect();
            let mut j: job_t = core::mem::zeroed();
            assert_eq!(md_certify(&mut j, sp.as_ptr(), sp.len() as i32, samp.as_ptr(), samp.len()), 1);
            assert_eq!((j.md_ok, j.md_nm), (1, 2)); assert!(j.mdx.is_null());
            assert_eq!((j.md_first[0], j.md_last[0], j.md_first[1], j.md_last[1]), (0, 14, 15, 30));
            assert_eq!((j.md_lo[0], j.md_hi[0], j.md_lo[1], j.md_hi[1]), (sp[0], sp[14], sp[15], sp[30]));
            assert_eq!(j.md_scale[1], 15.0 / (sp[30] - sp[15]));
            // twelve modes: the grid
            let samp = clusters(12, 256); let sp: Vec<f64> = (1..96).map(|i| samp[i * 32]).collect();
            let mut j: job_t = core::mem::zeroed();
            assert_eq!(md_certify(&mut j, sp.as_ptr(), sp.len() as i32, samp.as_ptr(), samp.len()), 1);
            assert_eq!((j.md_ok, j.md_nm), (1, 12)); assert!(!j.mdx.is_null());
            let g = j.mdx;
            assert!((*g).lo[12].is_nan());
            for (i, &x) in samp.iter().enumerate() {
                let q = (*g).grid[md_cell(g, x) as usize] as usize;
                let mode = q + (x >= (*g).lo[q + 1]) as usize;
                let want = (0..12).filter(|&k| x >= (*g).lo[k]).count().max(1) - 1;
                assert_eq!(mode, want, "sample {}", i);
            }
            free(g);
            // one mode (no gulf), too few splitters, a mode that is not uniform inside
            let samp = clusters(1, 2048); let sp: Vec<f64> = (1..32).map(|i| samp[i * 64]).collect();
            let mut j: job_t = core::mem::zeroed();
            assert_eq!(md_certify(&mut j, sp.as_ptr(), sp.len() as i32, samp.as_ptr(), samp.len()), 0);
            assert_eq!(md_certify(&mut j, sp.as_ptr(), 7, samp.as_ptr(), samp.len()), 0);
            let mut samp = clusters(2, 1024); for i in 0..1024 { let u = samp[i]; samp[i] = u * u * u * u }
            let sp: Vec<f64> = (1..32).map(|i| clusters(2, 1024)[i * 64]).collect();
            assert_eq!(md_certify(&mut j, sp.as_ptr(), sp.len() as i32, samp.as_ptr(), samp.len()), 0);
            assert_eq!(j.md_ok, 0);
        }
    }
}
