//! C lines 3354-4277, 4473-4640, 8599-8663 (md_certify), 8674-8793: the splitter tables, the bucket maps, the histogram path's
//! phases, heavy values, the histogram-free partition, the two-halves route. A transliteration: see PORTING.md.
use super::*;

/// C line 3354.
#[inline]
pub(crate) unsafe fn bucket_of(x: f64, sp: *const f64, nsp: i32) -> i32 {
    if nsp <= 32 { let mut b = 0i32; for i in 0..nsp { b += (*sp.add(i as usize) <= x) as i32; } return b; }
    let mut p = sp; let mut len = nsp;
    while len > 0 { let half = len >> 1; let go = *p.add(half as usize) <= x; p = p.add(if go { half as usize + 1 } else { 0 }); len = if go { len - half - 1 } else { half }; }
    p.offset_from(sp) as i32
}

/// C line 4641 (declared at 3383 and 3744 for this section).
#[inline]
pub(crate) unsafe fn total_key(v: f64) -> u64 { let u = v.to_bits(); if (u >> 63) != 0 { !u } else { u | 0x8000000000000000u64 } }

// ---- direct-index splitter tables (H0b) ----------------------------------------------------------------------------------
pub(crate) const BT_ROOT_CELLS: i32 = 4096;
pub(crate) const BT_CHILD_CELLS: i32 = 64;
pub(crate) const BT_MAX_CHILD: i32 = 256;
pub(crate) const BT_MAX_DEPTH: i32 = 8;
pub(crate) const BT_MAX_NODES: i32 = 512;
pub(crate) const BT_MODE_GAP: f64 = 10.0;
pub(crate) const BT_MODE_CELLS: i32 = 2048;
pub(crate) const BT_MODE_MAXSP: usize = 1024;
pub(crate) const BT_MAX_ENTS: i32 = BT_ROOT_CELLS + BT_MAX_NODES * BT_MAX_CHILD;
#[inline]
pub(crate) unsafe fn bt_child_cells(nsplit: i32) -> i32 { let c = 4 * nsplit; if c < BT_CHILD_CELLS { BT_CHILD_CELLS } else if c > BT_MAX_CHILD { BT_MAX_CHILD } else { c } }
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct bt_ent_t { pub(crate) lo: i16, pub(crate) hi: i16, pub(crate) child: i32 }
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct bt_vnode_t { pub(crate) lo: f64, pub(crate) scale: f64, pub(crate) cells: i32, pub(crate) base: i32 }
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct bt_knode_t { pub(crate) klo: u64, pub(crate) shift: i32, pub(crate) cells: i32, pub(crate) base: i32 }
/// model cell (the model table itself is opt-in, -DLOGOS_MODEL_TABLE, and not ported; bt_t keeps its fields)
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct bt_ment_t { pub(crate) slo: f64, pub(crate) slope: f32, pub(crate) lo: i16, pub(crate) hi: i16 }
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct bt_t {
    pub(crate) sp: *const f64, pub(crate) nsp: i32, pub(crate) ksp: *mut u64, pub(crate) vlo: f64, pub(crate) vhi: f64, pub(crate) klo: u64, pub(crate) khi: u64,
    pub(crate) vn: *mut bt_vnode_t, pub(crate) nvn: i32, pub(crate) ve: *mut bt_ent_t, pub(crate) nve: i32,      // value-space table
    pub(crate) kn: *mut bt_knode_t, pub(crate) nkn: i32, pub(crate) ke: *mut bt_ent_t, pub(crate) nke: i32,      // key-space table
    pub(crate) me: *mut bt_ment_t, pub(crate) mscale: f64, pub(crate) merr: i32,                                 // model table (H0c)
    pub(crate) nm: i32, pub(crate) mroot: [i32; 8], pub(crate) mfirst: [i32; 8], pub(crate) mlast: [i32; 8], pub(crate) mlo: [f64; 8], pub(crate) mhi: [f64; 8],   // mode table (M2)
    pub(crate) vok: i32, pub(crate) kok: i32, pub(crate) mok: i32, pub(crate) dok: i32,
}
pub(crate) unsafe fn bt_cnt_le_v(t: *const bt_t, x: f64) -> i32 { let (mut lo, mut hi) = (0i32, (*t).nsp); while lo < hi { let m = (lo + hi) >> 1; if *(*t).sp.add(m as usize) <= x { lo = m + 1 } else { hi = m } } lo }
pub(crate) unsafe fn bt_cnt_lt_v(t: *const bt_t, x: f64) -> i32 { let (mut lo, mut hi) = (0i32, (*t).nsp); while lo < hi { let m = (lo + hi) >> 1; if *(*t).sp.add(m as usize) < x { lo = m + 1 } else { hi = m } } lo }
pub(crate) unsafe fn bt_cnt_le_k(t: *const bt_t, k: u64) -> i32 { let (mut lo, mut hi) = (0i32, (*t).nsp); while lo < hi { let m = (lo + hi) >> 1; if *(*t).ksp.add(m as usize) <= k { lo = m + 1 } else { hi = m } } lo }
pub(crate) unsafe fn bt_cnt_lt_k(t: *const bt_t, k: u64) -> i32 { let (mut lo, mut hi) = (0i32, (*t).nsp); while lo < hi { let m = (lo + hi) >> 1; if *(*t).ksp.add(m as usize) < k { lo = m + 1 } else { hi = m } } lo }
pub(crate) unsafe fn bt_build_v(t: *mut bt_t, vlo: f64, vhi: f64, cells: i32, depth: i32) -> i32 {
    if (*t).nvn >= BT_MAX_NODES || (*t).nve + cells > BT_MAX_ENTS || !(vhi > vlo) { return -1 }
    let id = (*t).nvn; (*t).nvn += 1; let nd: *mut bt_vnode_t = (*t).vn.add(id as usize); (*nd).lo = vlo; (*nd).scale = cells as f64 / (vhi - vlo); (*nd).cells = cells; (*nd).base = (*t).nve; (*t).nve += cells;
    // a scale of ZERO is not a scale (an infinite range): no value table, the trial falls to the key-space table
    if !(*nd).scale.is_finite() || !((*nd).scale > 0.0) { return -1 }
    for c in 0..cells {
        let clo = vlo + c as f64 / (*nd).scale; let chi = if c + 1 == cells { vhi } else { vlo + (c + 1) as f64 / (*nd).scale };
        let w = clo.abs() * 1e-13 + chi.abs() * 1e-13 + 1e-300;            // conservative widening
        let mut e: bt_ent_t = core::mem::zeroed(); e.lo = bt_cnt_le_v(t, clo - w) as i16; e.hi = bt_cnt_lt_v(t, chi + w) as i16; e.child = -1;
        if e.hi < e.lo { e.hi = e.lo }
        if e.hi as i32 - e.lo as i32 > 1 && depth < BT_MAX_DEPTH {
            let slo = *(*t).sp.add(e.lo as usize); let shi = *(*t).sp.add(e.hi as usize - 1);
            if shi > slo { let ch = bt_build_v(t, slo, shi, bt_child_cells(e.hi as i32 - e.lo as i32), depth + 1); if ch < 0 { return -1 } e.child = ch; }
        }
        *(*t).ve.add(((*nd).base + c) as usize) = e;
    }
    id
}
pub(crate) unsafe fn bt_build_k(t: *mut bt_t, klo: u64, khi: u64, cells: i32, depth: i32) -> i32 {
    if (*t).nkn >= BT_MAX_NODES || (*t).nke + cells > BT_MAX_ENTS || !(khi > klo) { return -1 }
    let id = (*t).nkn; (*t).nkn += 1; let nd: *mut bt_knode_t = (*t).kn.add(id as usize); (*nd).klo = klo; (*nd).cells = cells; (*nd).base = (*t).nke; (*t).nke += cells;
    { let mut s = 0i32; let span = khi - klo; while s < 63 && (span >> s) >= cells as u64 { s += 1 } (*nd).shift = s; }
    for c in 0..cells {
        let clo: u128 = klo as u128 + ((c as u128) << (*nd).shift); let chi: u128 = klo as u128 + (((c + 1) as u128) << (*nd).shift);
        let mut e: bt_ent_t = core::mem::zeroed(); e.child = -1;
        if clo > khi as u128 { e.lo = (*t).nsp as i16; e.hi = e.lo; *(*t).ke.add(((*nd).base + c) as usize) = e; continue }   // past the top key: top bucket
        e.lo = bt_cnt_le_k(t, clo as u64) as i16;
        e.hi = (if chi > u64::MAX as u128 { (*t).nsp } else { bt_cnt_lt_k(t, chi as u64) }) as i16;
        if e.hi < e.lo { e.hi = e.lo }
        if e.hi as i32 - e.lo as i32 > 1 && depth < BT_MAX_DEPTH {
            let slo = *(*t).ksp.add(e.lo as usize); let shi = *(*t).ksp.add(e.hi as usize - 1);
            if shi > slo { let ch = bt_build_k(t, slo, shi, bt_child_cells(e.hi as i32 - e.lo as i32), depth + 1); if ch < 0 { return -1 } e.child = ch; }
        }
        *(*t).ke.add(((*nd).base + c) as usize) = e;
    }
    id
}
pub(crate) unsafe fn bt_free(t: *mut bt_t) { free((*t).ksp); free((*t).vn); free((*t).ve); free((*t).kn); free((*t).ke); free((*t).me); *t = core::mem::zeroed(); }
/// C's nextafter(x, INFINITY), the only form this section uses (private: not a C name of this file)
#[inline]
fn nextafter_up(x: f64) -> f64 {
    if x.is_nan() || x == f64::INFINITY { return x }
    if x == 0.0 { return f64::from_bits(1) }
    let u = x.to_bits();
    f64::from_bits(if x > 0.0 { u + 1 } else { u - 1 })
}
/// vlo/vhi: the sample's min and max (every splitter lies inside). Returns 1 if at least one table built.
#[inline(never)]
pub(crate) unsafe fn bt_init(t: *mut bt_t, sp: *const f64, nsp: i32, vlo: f64, vhi: f64) -> i32 {
    *t = core::mem::zeroed(); (*t).sp = sp; (*t).nsp = nsp; (*t).vlo = vlo; (*t).vhi = vhi;
    if nsp < 2 || !(vhi > vlo) { return 0 }
    (*t).vn = malloc(BT_MAX_NODES as usize * size_of::<bt_vnode_t>()) as *mut bt_vnode_t; (*t).ve = malloc(BT_MAX_ENTS as usize * size_of::<bt_ent_t>()) as *mut bt_ent_t;
    (*t).kn = malloc(BT_MAX_NODES as usize * size_of::<bt_knode_t>()) as *mut bt_knode_t; (*t).ke = malloc(BT_MAX_ENTS as usize * size_of::<bt_ent_t>()) as *mut bt_ent_t; (*t).ksp = malloc(nsp as usize * size_of::<u64>()) as *mut u64;
    (*t).vok = (bt_build_v(t, vlo, nextafter_up(vhi), BT_ROOT_CELLS, 0) == 0) as i32;
    for i in 0..nsp as usize { *(*t).ksp.add(i) = total_key(*sp.add(i)); }
    (*t).klo = total_key(vlo); (*t).khi = total_key(vhi); (*t).nkn = 0; (*t).nke = 0;
    (*t).kok = ((*t).khi > (*t).klo && bt_build_k(t, (*t).klo, (*t).khi, BT_ROOT_CELLS, 0) == 0) as i32;
    // Firmament split: one value table per mode, modes = gaps between consecutive splitters larger than BT_MODE_GAP times the
    // median gap (at most 8 modes, the largest gaps win). Built only when there are >= 2 modes.
    (*t).nm = 0; (*t).dok = 0;
    if (*t).vok != 0 && nsp >= 8 {
        let mut gaps = [0f64; BT_MODE_MAXSP]; let ng: i32 = if (nsp - 1) < BT_MODE_MAXSP as i32 { nsp - 1 } else { BT_MODE_MAXSP as i32 };
        for i in 0..ng as usize { gaps[i] = *sp.add(i + 1) - *sp.add(i); }
        let mut sorted = [0f64; BT_MODE_MAXSP]; memcpy(sorted.as_mut_ptr(), gaps.as_ptr(), ng as usize * size_of::<f64>()); sample_sort(sorted.as_mut_ptr(), ng as usize);
        let med = sorted[(ng / 2) as usize];
        let mut cut = [0i32; 8]; let mut nc = 0i32;                   // indices i where the gap sp[i]..sp[i+1] is a gulf
        if med > 0.0 { for i in 0..ng { if gaps[i as usize] > BT_MODE_GAP * med {
            if nc < 7 { cut[nc as usize] = i; nc += 1; }
            else { let mut wmin = 0i32; for q in 1..nc { if gaps[cut[q as usize] as usize] < gaps[cut[wmin as usize] as usize] { wmin = q } } if gaps[i as usize] > gaps[cut[wmin as usize] as usize] { cut[wmin as usize] = i } }
        } } }
        if nc >= 1 {
            cut[..nc as usize].sort_unstable();                       // qsort(cut, nc, sizeof(int), cmp_int)
            let mut first = 0i32; let mut ok = 1i32;
            let mut k = 0i32;
            while k <= nc && ok != 0 {
                let last = if k < nc { cut[k as usize] } else { nsp - 1 };             // splitters first..last belong to mode k
                let ku = k as usize;
                (*t).mlo[ku] = *sp.add(first as usize); (*t).mhi[ku] = *sp.add(last as usize); (*t).mfirst[ku] = first; (*t).mlast[ku] = last; (*t).mroot[ku] = -1;
                if last > first && *sp.add(last as usize) > *sp.add(first as usize) { (*t).mroot[ku] = bt_build_v(t, *sp.add(first as usize), nextafter_up(*sp.add(last as usize)), BT_MODE_CELLS, 0); if (*t).mroot[ku] < 0 { ok = 0 } }
                first = last + 1;
                k += 1;
            }
            if ok != 0 { (*t).nm = nc + 1; (*t).dok = 1; }
        }
    }
    if (*t).vok == 0 && (*t).kok == 0 && (*t).mok == 0 && (*t).dok == 0 { bt_free(t); return 0 }
    1
}
// Leaf of a table lookup (C's BT_LEAF, the default form): `style` 0 = branchy root, 1 = branch-free root.
macro_rules! BT_LEAF {
    ($t:ident, $e:ident, $depth:ident, $style:ident, $resolved:ident, $li:ident => $cmp:expr) => {
        if ($style == 0 || !$resolved.is_null()) && $depth == 0 && (*$e).lo == (*$e).hi { if !$resolved.is_null() { *$resolved += 1; } return (*$e).lo as i32; }
        let span_ = (*$e).hi as i32 - (*$e).lo as i32;
        if span_ <= 1 { let $li: i32 = if ((*$e).lo as i32) < (*$t).nsp { (*$e).lo as i32 } else { (*$t).nsp - 1 }; return (*$e).lo as i32 + (((span_ > 0) as i32) & (($cmp) as i32)); }
    };
}
#[inline(always)]
pub(crate) unsafe fn bt_lookup_v(t: *const bt_t, x: f64, style: i32, resolved: *mut usize) -> i32 {
    if !(x >= (*t).vlo) { return 0 }                            // below every splitter, or NaN (bucket_of gives 0 too)
    if x >= (*t).vhi { return (*t).nsp }
    let mut nd: *const bt_vnode_t = (*t).vn; let mut depth = 0i32;
    loop {
        let f = (x - (*nd).lo) * (*nd).scale; let c: i32 = if f >= (*nd).cells as f64 { (*nd).cells - 1 } else if f > 0.0 { f as i32 } else { 0 };
        let e: *const bt_ent_t = (*t).ve.add(((*nd).base + c) as usize);
        BT_LEAF!(t, e, depth, style, resolved, i => *(*t).sp.add(i as usize) <= x);
        if (*e).child >= 0 { let ch: *const bt_vnode_t = (*t).vn.add((*e).child as usize); if x < (*ch).lo { return (*e).lo as i32 } if x >= *(*t).sp.add((*e).hi as usize - 1) { return (*e).hi as i32 } nd = ch; depth += 1; continue }
        return (*e).lo as i32 + bucket_of(x, (*t).sp.add((*e).lo as usize), (*e).hi as i32 - (*e).lo as i32);
    }
}
#[inline(always)]
pub(crate) unsafe fn bt_lookup_k(t: *const bt_t, x: f64, style: i32, resolved: *mut usize) -> i32 {
    let k = total_key(x);
    if k < (*t).klo { return 0 }
    if k >= (*t).khi { return (*t).nsp }
    let mut nd: *const bt_knode_t = (*t).kn; let mut depth = 0i32;
    loop {
        let d = k - (*nd).klo; let mut c = (d >> (*nd).shift) as i32; if c >= (*nd).cells { c = (*nd).cells - 1 }
        let e: *const bt_ent_t = (*t).ke.add(((*nd).base + c) as usize);
        BT_LEAF!(t, e, depth, style, resolved, i => *(*t).ksp.add(i as usize) <= k);
        if (*e).child >= 0 { let ch: *const bt_knode_t = (*t).kn.add((*e).child as usize); if k < (*ch).klo { return (*e).lo as i32 } if k >= *(*t).ksp.add((*e).hi as usize - 1) { return (*e).hi as i32 } nd = ch; depth += 1; continue }
        return (*e).lo as i32 + bucket_of(x, (*t).sp.add((*e).lo as usize), (*e).hi as i32 - (*e).lo as i32);
    }
}
/// C: `#define bt_lookup_m bt_lookup_v` (no -DLOGOS_MODEL_TABLE)
#[inline(always)]
pub(crate) unsafe fn bt_lookup_m(t: *const bt_t, x: f64, style: i32, resolved: *mut usize) -> i32 { bt_lookup_v(t, x, style, resolved) }
#[inline(always)]
pub(crate) unsafe fn bt_lookup_d(t: *const bt_t, x: f64, style: i32, resolved: *mut usize) -> i32 {
    if !(x >= (*t).vlo) { return 0 }
    if x >= (*t).vhi { return (*t).nsp }
    let mut k = 0usize; for q in 1..(*t).nm as usize { k += (x >= (*t).mlo[q]) as usize; }            // branch-free mode pick, nm <= 8
    if x >= (*t).mhi[k] { return (*t).mlast[k] + 1 }                               // in the gulf after mode k (or on its last splitter)
    if x < (*t).mlo[k] { return 0 }                                                // below the first splitter (k == 0 only)
    let mut nd: *const bt_vnode_t = (*t).vn.add((*t).mroot[k] as usize); let mut depth = 0i32;
    loop {
        let f = (x - (*nd).lo) * (*nd).scale; let c: i32 = if f >= (*nd).cells as f64 { (*nd).cells - 1 } else if f > 0.0 { f as i32 } else { 0 };
        let e: *const bt_ent_t = (*t).ve.add(((*nd).base + c) as usize);
        BT_LEAF!(t, e, depth, style, resolved, i => *(*t).sp.add(i as usize) <= x);
        if (*e).child >= 0 { let ch: *const bt_vnode_t = (*t).vn.add((*e).child as usize); if x < (*ch).lo { return (*e).lo as i32 } if x >= *(*t).sp.add((*e).hi as usize - 1) { return (*e).hi as i32 } nd = ch; depth += 1; continue }
        return (*e).lo as i32 + bucket_of(x, (*t).sp.add((*e).lo as usize), (*e).hi as i32 - (*e).lo as i32);
    }
}
/// milliseconds on a monotonic clock (C: clock_gettime(CLOCK_MONOTONIC))
pub(crate) unsafe fn bt_now() -> f64 {
    static T0: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    T0.get_or_init(std::time::Instant::now).elapsed().as_secs_f64() * 1e3
}

/// Exponent-histogram bucket map (heavy tails). Built by the dispatcher; `ex_ok` is forced to 0 in the default build.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ex_ent_t { pub(crate) base: u32, pub(crate) span: u32, pub(crate) slope: f64 }
pub(crate) const EX_SHIFT: u32 = 52;
pub(crate) const EX_MASK: u64 = 0x000FFFFFFFFFFFFFu64;
pub(crate) const EX_SCALE: f64 = 4503599627370496.0;
pub(crate) const HF_BAL_MAX: f64 = 3.0;
pub(crate) const KG_BITS: u32 = 14;
pub(crate) const CHUNK_MAX: usize = 65536;
/// C line 3665. Not Copy: it holds atomics and a barrier. All zeros is its initial state (the dispatcher memsets it).
#[repr(C)]
pub(crate) struct job_t {
    pub(crate) P: i32, pub(crate) nbk: i32, pub(crate) nsp: i32, pub(crate) src: *mut f64, pub(crate) dst: *mut f64, pub(crate) n: usize, pub(crate) sp: *const f64,
    pub(crate) nchunk: usize, pub(crate) csize: usize,                 // chunk grid
    pub(crate) counts: *mut usize, pub(crate) offs: *mut usize,        // nchunk x nbk
    pub(crate) bstart: *mut usize, pub(crate) bsize: *mut usize,       // nbk
    pub(crate) bar: barrier_t, pub(crate) hnext: AtomicUsize, pub(crate) snext: AtomicUsize, pub(crate) next: AtomicUsize, pub(crate) wc_on: i32,
    pub(crate) bidx: *mut u8,                                          // per-element bucket index, filled by the histogram pass (nbk <= 256)
    pub(crate) bt: bt_t, pub(crate) bt_ok: i32,                        // direct-index splitter tables for the histogram
    pub(crate) ks_ok: i32, pub(crate) ks_d: f64, pub(crate) ks_lo: f64, pub(crate) ks_hi: f64, pub(crate) ks_scale: f64,   // uniformity certificate from the sorted sample
    pub(crate) ks_key_ok: i32, pub(crate) kk_lo: u64, pub(crate) kk_hi: u64, pub(crate) kk_scale: f64,                     // the same test in KEY space
    pub(crate) ex_ok: i32, pub(crate) ex_e0: u64, pub(crate) dup_frac: f64, pub(crate) ex: [ex_ent_t; 256],                // exponent map and the largest single-value share
    pub(crate) md_ok: i32, pub(crate) md_nm: i32, pub(crate) md_first: [i32; 8], pub(crate) md_last: [i32; 8], pub(crate) md_lo: [f64; 8], pub(crate) md_hi: [f64; 8], pub(crate) md_scale: [f64; 8],   // per-mode linear map
    pub(crate) mdx: *mut mdx_t,                                        // the same map past MD_LIN_MAX modes, with an O(1) mode grid (heap)
    pub(crate) hv: *mut hv_t,                                          // heavy values the histogram-free scatter counts instead of moving (heap)
    pub(crate) kg_ok: i32, pub(crate) kg_shift: i32, pub(crate) kg_lo: u64, pub(crate) kg_cells: u32, pub(crate) kg_tab: [u8; 1 << KG_BITS],   // key grid: bucket per key-space cell
    pub(crate) seq_leaf: i32,                                          // the leaves may take the sequential leaf's AVX2 quicksort
}
// THE MODE GRID (C line 3686)
pub(crate) const MD_MAX: usize = 64;
pub(crate) const MD_LIN_MAX: i32 = 8;
pub(crate) const MD_GRID_CELLS: i32 = 4096;
pub(crate) static g_mdgrid_scatters: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(18)
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct mdx_t {
    pub(crate) nm: i32, pub(crate) cells: i32, pub(crate) glo: f64, pub(crate) gscale: f64,
    pub(crate) first: [i32; MD_MAX], pub(crate) last: [i32; MD_MAX], pub(crate) lo: [f64; MD_MAX + 1], pub(crate) hi: [f64; MD_MAX], pub(crate) scale: [f64; MD_MAX],   // lo[nm] = NaN
    pub(crate) grid: [u8; MD_GRID_CELLS as usize],
}
#[inline(always)]
pub(crate) unsafe fn md_cell(g: *const mdx_t, x: f64) -> i32 {
    let t = (x - (*g).glo) * (*g).gscale;                    // a NaN lands in cell 0
    if t >= 0.0 { if t < (*g).cells as f64 { t as i32 } else { (*g).cells - 1 } } else { 0 }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct warg_t { pub(crate) j: *mut job_t, pub(crate) t: i32, pub(crate) ctx: *mut ctx_t }

// one chunk of the histogram with a given bucket function; fills the bucket-index cache when present (C's HIST_CHUNK)
macro_rules! HIST_CHUNK {
    ($lo:ident, $hi:ident, $bx:ident, $c:ident, $i:ident => $bucket:expr) => {
        if !$bx.is_null() { for $i in $lo..$hi { let b: i32 = $bucket; *$bx.add($i) = b as u8; *$c.add(b as usize) += 1; } }
        else { for $i in $lo..$hi { let b: i32 = $bucket; *$c.add(b as usize) += 1; } }
    };
}
/// The key grid (mode 10): one subtract, one shift, one byte load.
#[inline(always)]
pub(crate) unsafe fn hf_lookup_kg(j: *const job_t, x: f64) -> i32 {
    let k = total_key(x);
    let d: u64 = if k > (*j).kg_lo { (k - (*j).kg_lo) >> (*j).kg_shift } else { 0 };
    (*j).kg_tab[(if d < (*j).kg_cells as u64 { d } else { (*j).kg_cells as u64 - 1 }) as usize] as i32
}
#[inline(always)]
pub(crate) unsafe fn hf_lookup_md(j: *const job_t, x: f64) -> i32 {
    let g: *const mdx_t = (*j).mdx;
    if !g.is_null() {                                         // the mode grid: one cell, at most one compare
        let mut k = (*g).grid[md_cell(g, x) as usize] as usize; k += (x >= (*g).lo[k + 1]) as usize;   // lo[nm] is a NaN: never passed, not even by +inf
        if !(x >= (*g).lo[k]) { return (*g).first[k] }       // below the mode, or a NaN: a real bucket either way
        if x >= (*g).hi[k] { return (*g).last[k] + 1 }
        let b = (*g).first[k] + 1 + ((x - (*g).lo[k]) * (*g).scale[k]) as i32;
        return if b > (*g).last[k] { (*g).last[k] } else { b };
    }
    let mut k = 0usize;
    for q in 1..(*j).md_nm as usize { k += (x >= (*j).md_lo[q]) as usize; }
    // inside the gulf below this mode -- or a NaN
    if !(x >= (*j).md_lo[k]) { return (*j).md_first[k] }
    if x >= (*j).md_hi[k] { return (*j).md_last[k] + 1 }
    let b = (*j).md_first[k] + 1 + ((x - (*j).md_lo[k]) * (*j).md_scale[k]) as i32;
    if b > (*j).md_last[k] { (*j).md_last[k] } else { b }
}
/// the exponent map as a standalone inline, so the histogram path can trial it too
#[inline(always)]
pub(crate) unsafe fn hf_lookup_ex(j: *const job_t, x: f64) -> i32 {
    let k = total_key(x); let q = k >> EX_SHIFT;
    let mut e: u32 = if q <= (*j).ex_e0 { 0 } else { (q - (*j).ex_e0) as u32 }; if e > 255 { e = 255 }
    let en: *const ex_ent_t = addr_of!((*j).ex[e as usize]);
    let mut off: u32 = 0;
    if (*en).span != 0 { let f = (k & EX_MASK) as f64 * (*en).slope; off = if f > 0.0 { f as u32 } else { 0 }; if off >= (*en).span { off = (*en).span - 1 } }
    ((*en).base + off) as i32
}
pub(crate) unsafe fn do_hist(j: *mut job_t) {
    // mode: 0 = trial (one chunk per candidate table: 1 = value, 2 = key, 4 = model), then the fastest; 3 = search (no tables).
    let mut cand = [0i32; 5]; let mut ncand = 0usize;
    if (*j).bt_ok != 0 && (*j).bt.vok != 0 { cand[ncand] = 1; ncand += 1; }
    if (*j).bt_ok != 0 && (*j).bt.kok != 0 { cand[ncand] = 2; ncand += 1; }
    if (*j).bt_ok != 0 && (*j).bt.mok != 0 && (*j).bt.merr <= 1 { cand[ncand] = 4; ncand += 1; }
    if (*j).bt_ok != 0 && (*j).bt.dok != 0 { cand[ncand] = 5; ncand += 1; }   // per-mode value tables, only when >= 2 modes were found
    let mut mode: i32 = if ncand == 0 { 3 } else if ncand == 1 { cand[0] } else { 0 }; let mut trial = 0usize; let mut style = 0i32;
    let mut tt = [0f64; 9]; let mut res = [0usize; 9]; let mut len = [1usize; 9];
    let resp: *mut usize = res.as_mut_ptr();
    let bt: *const bt_t = addr_of!((*j).bt);
    loop {
        let ch = (*j).hnext.fetch_add(1, SeqCst);
        if !(ch < (*j).nchunk) { break }
        let lo = ch * (*j).csize; let mut hi = lo + (*j).csize; if hi > (*j).n { hi = (*j).n }
        let c: *mut usize = (*j).counts.add(ch * (*j).nbk as usize); for b in 0..(*j).nbk as usize { *c.add(b) = 0; }
        let bx: *mut u8 = (*j).bidx; let src: *const f64 = (*j).src;
        let use_: i32 = if mode == 0 { cand[trial] } else { mode }; let t0 = if mode == 0 { bt_now() } else { 0.0 };
        if mode == 0 {                                  // trial chunks: probe one candidate, count root resolutions
            if use_ == 1 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_v(bt, *src.add(i), 0, resp.add(1))); }
            else if use_ == 2 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_k(bt, *src.add(i), 0, resp.add(2))); }
            else if use_ == 5 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_d(bt, *src.add(i), 0, resp.add(5))); }
            else { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_m(bt, *src.add(i), 0, resp.add(4))); }
            len[use_ as usize] = hi - lo; tt[use_ as usize] = bt_now() - t0; trial += 1;
            if trial == ncand {
                mode = cand[0]; for q in 1..ncand { if tt[cand[q] as usize] < tt[mode as usize] { mode = cand[q] } }
                let p = *resp.add(mode as usize) as f64 / len[mode as usize] as f64;
                style = if p > 0.85 || p < 0.15 { 0 } else { 1 };
                TRACE!("hist trial: value {:.3} key {:.3} model {:.3} modes {:.3} ms -> mode {}, root-resolved {:.2}, style {}", tt[1], tt[2], tt[4], tt[5], mode, p, style);
            }
        }
        else if use_ == 1 { if style != 0 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_v(bt, *src.add(i), 1, null_mut())); } else { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_v(bt, *src.add(i), 0, null_mut())); } }
        else if use_ == 2 { if style != 0 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_k(bt, *src.add(i), 1, null_mut())); } else { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_k(bt, *src.add(i), 0, null_mut())); } }
        else if use_ == 4 { if style != 0 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_m(bt, *src.add(i), 1, null_mut())); } else { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_m(bt, *src.add(i), 0, null_mut())); } }
        else if use_ == 5 { if style != 0 { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_d(bt, *src.add(i), 1, null_mut())); } else { HIST_CHUNK!(lo, hi, bx, c, i => bt_lookup_d(bt, *src.add(i), 0, null_mut())); } }
        else { HIST_CHUNK!(lo, hi, bx, c, i => bucket_of(*src.add(i), (*j).sp, (*j).nsp)); }
    }
}
pub(crate) unsafe fn do_scatter(j: *mut job_t) {
    let nb = (*j).nbk; let mut st: *mut f64 = null_mut(); let mut sc: *mut u8 = null_mut();
    if (*j).wc_on != 0 { st = malloc(nb as usize * 8 * size_of::<f64>()) as *mut f64; sc = calloc(nb as usize, 1); }
    loop {
        let ch = (*j).snext.fetch_add(1, SeqCst);
        if !(ch < (*j).nchunk) { break }
        let lo = ch * (*j).csize; let mut hi = lo + (*j).csize; if hi > (*j).n { hi = (*j).n }
        let o: *mut usize = (*j).offs.add(ch * (*j).nbk as usize); let bx: *const u8 = (*j).bidx;
        if !st.is_null() {
            for i in lo..hi { let x = *(*j).src.add(i); let b: usize = if !bx.is_null() { *bx.add(i) as usize } else { bucket_of(x, (*j).sp, (*j).nsp) as usize };
                if !bx.is_null() && i + PF_DIST < hi { _mm_prefetch::<_MM_HINT_NTA>((*j).dst.add(*o.add(*bx.add(i + PF_DIST) as usize)) as *const i8); }
                *st.add(b * 8 + *sc.add(b) as usize) = x; *sc.add(b) += 1; if *sc.add(b) == 8 { memcpy((*j).dst.add(*o.add(b)), st.add(b * 8), 64); *o.add(b) += 8; *sc.add(b) = 0; } }
            for b in 0..nb as usize { if *sc.add(b) != 0 { memcpy((*j).dst.add(*o.add(b)), st.add(b * 8), *sc.add(b) as usize * size_of::<f64>()); *o.add(b) += *sc.add(b) as usize; *sc.add(b) = 0; } }
        } else if !bx.is_null() {
            for i in lo..hi { if i + PF_DIST < hi { _mm_prefetch::<_MM_HINT_NTA>((*j).dst.add(*o.add(*bx.add(i + PF_DIST) as usize)) as *const i8); } let ob = o.add(*bx.add(i) as usize); *(*j).dst.add(*ob) = *(*j).src.add(i); *ob += 1; }
        } else {
            for i in lo..hi { let x = *(*j).src.add(i); let ob = o.add(bucket_of(x, (*j).sp, (*j).nsp) as usize); *(*j).dst.add(*ob) = x; *ob += 1; }
        }
    }
    free(st); free(sc);
}
pub(crate) unsafe fn do_buckets(j: *mut job_t, ctx: *mut ctx_t) {
    loop {
        let b = (*j).next.fetch_add(1, SeqCst);
        if !(b < (*j).nbk as usize) { break }
        let s = *(*j).bstart.add(b); let nb = *(*j).bsize.add(b);
        let simd = g_simd_f64;
        if simd.is_some() && nb > 1 { (simd.unwrap())((*j).dst.add(s), nb); memcpy((*j).src.add(s), (*j).dst.add(s), nb * size_of::<f64>()); }  // parallel-SIMD leaf
        else { let mut r: *mut f64 = (*j).dst.add(s);
               if nb > 1 { let mut cf: ctx_fit_t = core::mem::zeroed();
                   if ctx_fit(ctx, nb, &mut cf) != 0 { r = leaf_f64((*j).dst.add(s), nb, (*j).src.add(s), ctx); ctx_unfit(ctx, &mut cf); }
                   else { quicksort((*j).dst.add(s), 0, nb as i64 - 1, 2 * log2i(nb)); } }      // no context this large: a sort that needs none
               if r != (*j).src.add(s) { memcpy((*j).src.add(s), (*j).dst.add(s), nb * size_of::<f64>()); } }
    }
}

// ---- histogram-free partition ("trust the sample") -----------------------------------------------------------------------
pub(crate) const HF_KS_MAX: f64 = 2.0;
pub(crate) const HF_SLACK: f64 = 0.6;
pub(crate) const HF_BLOCK: usize = 256;
pub(crate) const HF_SPILL_DIV: usize = 16;
pub(crate) static mut hf_buf: *mut f64 = null_mut();
pub(crate) static mut hf_buf_cap: usize = 0;                 // persistent, like the pool: page faults once
pub(crate) static g_hf_pooled: AtomicUsize = AtomicUsize::new(0);      // logos_twist_last(35)
pub(crate) static mut hf_trim: trim_t = trim_t { hw: 0, hi: 0, ep: 0 };
// HEAVY VALUES ARE COUNTED, NOT MOVED (C line 3898)
pub(crate) const HV_MAX: usize = 64;
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct hv_t { pub(crate) n: i32, pub(crate) rest: f64, pub(crate) v: [f64; HV_MAX], pub(crate) b: [u64; HV_MAX], pub(crate) tot: [usize; HV_MAX], pub(crate) lo: [u8; MAX_BUCKETS as usize], pub(crate) hi: [u8; MAX_BUCKETS as usize] }
pub(crate) static g_hf_heavy: AtomicUsize = AtomicUsize::new(0);       // logos_twist_last(41)
#[inline]
pub(crate) unsafe fn hv_runlen(samp: *const f64, m: usize, i: usize) -> usize {
    let x = (*samp.add(i)).to_bits(); let mut e = i + 1;
    while e < m { let y = (*samp.add(e)).to_bits(); if y != x { break } e += 1; }
    e - i
}
/// the heavy values of a sorted sample, in value order; *maskp marks their sample points (for the balance check). NULL: none.
#[inline(never)]
pub(crate) unsafe fn hv_build(samp: *const f64, m: usize, nbk: i32, maskp: *mut *mut u8) -> *mut hv_t {
    *maskp = null_mut();
    if m < 256 || nbk < 2 || nbk > MAX_BUCKETS { return null_mut() }           // LOGOS_NO_HFHEAVY unset
    let mut cut = m / (4 * nbk as usize); if cut < 6 { cut = 6 }
    loop {                                                            // at most HV_MAX values: raise the cut until they fit
        let mut nr = 0usize;
        let mut i = 0usize; while i < m { let L = hv_runlen(samp, m, i); if L >= cut && *samp.add(i) == *samp.add(i) { nr += 1 } i += L; }
        if nr == 0 { return null_mut() }
        if nr <= HV_MAX { break }
        cut += 1 + cut / 8;
    }
    let hv: *mut hv_t = calloc(1, size_of::<hv_t>()) as *mut hv_t; let mask: *mut u8 = calloc(m, 1);
    let mut rest = 1usize;
    let mut i = 0usize;
    while i < m {
        let L = hv_runlen(samp, m, i);
        if L >= cut && *samp.add(i) == *samp.add(i) && ((*hv).n as usize) < HV_MAX { let q = (*hv).n as usize; (*hv).v[q] = *samp.add(i); (*hv).b[q] = (*samp.add(i)).to_bits(); (*hv).n += 1; memset(mask.add(i), 1, L); }
        else if L > rest { rest = L }
        i += L;
    }
    (*hv).rest = rest as f64 / m as f64;
    *maskp = mask;
    hv
}
/// out[0 .. no) holds a bucket's other elements, sorted; its heavy values q in [lo, hi) go in where they belong, from the top down
pub(crate) unsafe fn hv_insert(out: *mut f64, no: usize, hv: *const hv_t, lo: i32, hi: i32) {
    let mut w = no; for q in lo..hi { w += (*hv).tot[q as usize]; }
    let mut e = no;
    let mut q = hi - 1;
    while q >= lo {
        let v = (*hv).v[q as usize]; let c = (*hv).tot[q as usize];
        let (mut a, mut b) = (0usize, e); while a < b { let mid = (a + b) >> 1; if *out.add(mid) <= v { a = mid + 1 } else { b = mid } }   // first element above v
        let up = e - a;
        if up != 0 && w != e { memmove(out.add(w - up), out.add(a), up * size_of::<f64>()); }
        w -= up; e = a;
        for i in (w - c)..w { *out.add(i) = v; }
        w -= c;
        q -= 1;
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct hf_spill_t { pub(crate) v: f64, pub(crate) b: u16 }
/// C line 3958. Not Copy (atomics, a barrier); all zeros is its initial state.
#[repr(C)]
pub(crate) struct hf_t {
    pub(crate) j: *mut job_t, pub(crate) P: i32, pub(crate) cap: usize, pub(crate) region: usize, pub(crate) rb: *mut f64,   // region = cap rounded so buckets start 64-byte aligned
    pub(crate) cur: *mut AtomicUsize, pub(crate) ovf: *mut AtomicUsize,          // per bucket: reservation cursor, first overflowing position (usize::MAX = none)
    pub(crate) mode: i32, pub(crate) style: i32,
    pub(crate) cnt: *mut usize, pub(crate) spo: *mut usize, pub(crate) spc: *mut usize, pub(crate) bstart: *mut usize, pub(crate) spv: *mut hf_spill_t, pub(crate) S: usize,     // after the scatter
    pub(crate) bar: barrier_t, pub(crate) next: AtomicUsize, pub(crate) bnext: AtomicUsize, pub(crate) declined: i32,
    pub(crate) order: *mut i32,                  // leaf dispatch order; identity, or largest-first when LPT is on
    pub(crate) lazy: i32,                        // 1: place the buckets but do not sort them (logos_partition_f64)
    pub(crate) vmin: *mut f64, pub(crate) vmax: *mut f64,         // lazy: per-bucket extremes, computed while the bucket is copied
    pub(crate) hv: *mut hv_t,                    // heavy values counted in the scatter (NULL: none)
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct hf_arg_t { pub(crate) h: *mut hf_t, pub(crate) t: i32, pub(crate) ctx: *mut ctx_t, pub(crate) sp: *mut hf_spill_t, pub(crate) spn: usize, pub(crate) spcap: usize, pub(crate) hvc: [usize; HV_MAX] }

#[inline]
pub(crate) unsafe fn hf_lpt_on() -> i32 { 0 }                 // LOGOS_LPT unset
#[inline(always)]
pub(crate) unsafe fn hf_lookup(j: *const job_t, x: f64, mode: i32, style: i32) -> i32 {
    if mode == 6 {                                                    // certified uniform: equi-width grid, one multiply, no table
        if !(x >= (*j).ks_lo) { return 0 }
        if x >= (*j).ks_hi { return (*j).nbk - 1 }
        let b = ((x - (*j).ks_lo) * (*j).ks_scale) as i32; return if b >= (*j).nbk { (*j).nbk - 1 } else { b };
    }
    if mode == 10 { return hf_lookup_kg(j, x) }                       // key grid: one byte load, no compares
    if mode == 7 {                                                    // certified uniform IN KEY SPACE (log-uniform data)
        let k = total_key(x);
        if k <= (*j).kk_lo { return 0 }
        if k >= (*j).kk_hi { return (*j).nbk - 1 }
        let b = ((k - (*j).kk_lo) as f64 * (*j).kk_scale) as i32; return if b >= (*j).nbk { (*j).nbk - 1 } else { b };
    }
    if mode == 9 { return hf_lookup_md(j, x) }                        // every mode certified uniform inside itself
    if mode == 8 {                                                    // exponent-histogram map: approximate inverse CDF
        let k = total_key(x); let q = k >> EX_SHIFT;
        let mut e: u32 = if q <= (*j).ex_e0 { 0 } else { (q - (*j).ex_e0) as u32 }; if e > 255 { e = 255 }
        let en: *const ex_ent_t = addr_of!((*j).ex[e as usize]);
        let mut off: u32 = 0;
        if (*en).span != 0 { let f = (k & EX_MASK) as f64 * (*en).slope; off = if f > 0.0 { f as u32 } else { 0 }; if off >= (*en).span { off = (*en).span - 1 } }
        return ((*en).base + off) as i32;
    }
    if mode == 1 { return bt_lookup_v(addr_of!((*j).bt), x, style, null_mut()) }
    if mode == 2 { return bt_lookup_k(addr_of!((*j).bt), x, style, null_mut()) }
    if mode == 5 { return bt_lookup_d(addr_of!((*j).bt), x, style, null_mut()) }
    if mode == 4 { return bt_lookup_m(addr_of!((*j).bt), x, style, null_mut()) }
    bucket_of(x, (*j).sp, (*j).nsp)
}
/// serial trial: time each candidate table on three 16k windows as a histogram, keep the fastest
pub(crate) unsafe fn hf_trial(h: *mut hf_t) {
    let j: *mut job_t = (*h).j; (*h).mode = 3; (*h).style = 0;
    if (*j).ks_ok != 0 { (*h).mode = 6; return }                      // the Kolmogorov-Smirnov certificate replaces the trial
    if (*j).ks_key_ok != 0 { (*h).mode = 7; return }                  // uniform in key space: log-uniform data, same one multiply
    if (*j).md_ok != 0 { (*h).mode = 9; return }                      // uniform inside every mode: compares then one multiply
    if (*j).kg_ok != 0 { (*h).mode = 10; return }                     // balanced on the sample: the key grid, before any table trial (LOGOS_NO_KGRID unset)
    let mut cand = [0i32; 5]; let mut nc = 0usize;
    if (*j).bt_ok != 0 && (*j).bt.vok != 0 { cand[nc] = 1; nc += 1; }
    if (*j).bt_ok != 0 && (*j).bt.kok != 0 { cand[nc] = 2; nc += 1; }
    if (*j).bt_ok != 0 && (*j).bt.mok != 0 && (*j).bt.merr <= 1 { cand[nc] = 4; nc += 1; }
    if (*j).bt_ok != 0 && (*j).bt.dok != 0 { cand[nc] = 5; nc += 1; }
    if (*j).ex_ok != 0 { cand[nc] = 8; nc += 1; }
    if nc == 0 { return }
    let c: *mut usize = calloc((*j).nbk as usize, size_of::<usize>()) as *mut usize;
    let mut best = 1e300f64; let W: usize = if (*j).n < 3 * 16384 { (*j).n / 3 } else { 16384 };
    let bt: *const bt_t = addr_of!((*j).bt);
    for q in 0..nc {
        let mut res: usize = 0; let resp: *mut usize = &mut res; let t0 = bt_now();
        for w in 0..3usize { let lo = ((*j).n / 4) * (w + 1) - W / 2; let hi = lo + W;
            for i in lo..hi { let x = *(*j).src.add(i); let b = if cand[q] == 1 { bt_lookup_v(bt, x, 0, resp) } else if cand[q] == 2 { bt_lookup_k(bt, x, 0, resp) } else if cand[q] == 5 { bt_lookup_d(bt, x, 0, resp) } else if cand[q] == 8 { hf_lookup_ex(j, x) } else { bt_lookup_m(bt, x, 0, resp) }; *c.add(b as usize) += 1; } }
        let dt = bt_now() - t0;
        if dt < best { best = dt; (*h).mode = cand[q]; let p = *resp as f64 / (3 * W) as f64; (*h).style = if p > 0.85 || p < 0.15 { 0 } else { 1 }; }
    }
    free(c);
}
pub(crate) unsafe fn hf_spill_push(w: *mut hf_arg_t, v: f64, b: i32) {
    if (*w).spn == (*w).spcap { let nc = if (*w).spcap != 0 { (*w).spcap * 2 } else { 4096 }; let p = realloc((*w).sp, nc * size_of::<hf_spill_t>()) as *mut hf_spill_t; (*w).sp = p; (*w).spcap = nc; }
    (*(*w).sp.add((*w).spn)).v = v; (*(*w).sp.add((*w).spn)).b = b as u16; (*w).spn += 1;
}
// The streaming copies (hf_stream_copy, _mm256_stream_pd) are behind -DLOGOS_STREAM, which the default build does not define:
// the live forms are these two.
#[inline]
pub(crate) unsafe fn hf_stream_fence() {}
#[inline]
pub(crate) unsafe fn hf_copy(dst: *mut f64, src: *const f64, k: usize) { memcpy(dst, src, k * size_of::<f64>()); }

/// flush k staged elements of bucket b: reserve exactly k slots, write, or spill the block
pub(crate) unsafe fn hf_flush(w: *mut hf_arg_t, b: i32, blk: *const f64, k: usize) {
    let h: *mut hf_t = (*w).h; let p = (*(*h).cur.add(b as usize)).fetch_add(k, SeqCst);
    if p + k <= (*h).cap { hf_copy((*h).rb.add(b as usize * (*h).region + p), blk, k); return }
    let _ = (*(*h).ovf.add(b as usize)).compare_exchange(usize::MAX, p, SeqCst, SeqCst);   // first overflow marks the filled end
    for i in 0..k { hf_spill_push(w, *blk.add(i), b); }
}
pub(crate) unsafe fn hf_scatter(w: *mut hf_arg_t) {
    let h: *mut hf_t = (*w).h; let j: *mut job_t = (*h).j; let nb = (*j).nbk; let mode = (*h).mode; let style = (*h).style; let hv: *const hv_t = (*h).hv;
    let st: *mut f64 = malloc(nb as usize * HF_BLOCK * size_of::<f64>()) as *mut f64; let sc: *mut u16 = calloc(nb as usize, size_of::<u16>()) as *mut u16;
    loop {
        let ch = (*h).next.fetch_add(1, SeqCst);
        if !(ch < (*j).nchunk && (*h).declined == 0) { break }
        let lo = ch * (*j).csize; let mut hi = lo + (*j).csize; if hi > (*j).n { hi = (*j).n } let src: *const f64 = (*j).src;
        if !hv.is_null() { for i in lo..hi {                           // a bucket that holds a heavy value: its copies are counted here and never staged
            let x = *src.add(i); let b = hf_lookup(j, x, mode, style) as usize;
            let mut q = (*hv).lo[b] as u32; let qe = (*hv).hi[b] as u32;
            if q != qe { let xb = x.to_bits(); while q < qe { if (*hv).b[q as usize] == xb { break } q += 1; } if q < qe { (*w).hvc[q as usize] += 1; continue } }
            *st.add(b * HF_BLOCK + *sc.add(b) as usize) = x;
            *sc.add(b) += 1; if *sc.add(b) as usize == HF_BLOCK { hf_flush(w, b as i32, st.add(b * HF_BLOCK), HF_BLOCK); *sc.add(b) = 0; }
        } }
        else { for i in lo..hi {
            let x = *src.add(i); let b = hf_lookup(j, x, mode, style) as usize;
            *st.add(b * HF_BLOCK + *sc.add(b) as usize) = x;
            *sc.add(b) += 1; if *sc.add(b) as usize == HF_BLOCK { hf_flush(w, b as i32, st.add(b * HF_BLOCK), HF_BLOCK); *sc.add(b) = 0; }
        } }
    }
    for b in 0..nb as usize { if *sc.add(b) != 0 { hf_flush(w, b as i32, st.add(b * HF_BLOCK), *sc.add(b) as usize); *sc.add(b) = 0; } }
    hf_stream_fence();        // weakly ordered stores must land before the barrier hands off the regions
    free(st); free(sc);
}
/// Longest-processing-time-first leaf dispatch (measured neutral, ships OFF: hf_lpt_on).
pub(crate) unsafe fn hf_order(ord: *mut i32, sz: *const usize, k: i32) {
    for i in 0..k { *ord.add(i as usize) = i; }
    if hf_lpt_on() == 0 { return }
    let s = |x: i32| -> usize { *sz.add(*ord.add(x as usize) as usize) };
    let mut st = k / 2 - 1;
    while st >= 0 {                                                   // heapify: max-heap keyed on size
        let mut r = st;
        loop { let mut c = 2 * r + 1; if c >= k { break }
            if c + 1 < k && s(c + 1) > s(c) { c += 1 }
            if s(c) <= s(r) { break }
            let t = *ord.add(r as usize); *ord.add(r as usize) = *ord.add(c as usize); *ord.add(c as usize) = t; r = c; }
        st -= 1;
    }
    let mut e = k - 1;
    while e > 0 {                                                     // pop: leaves ord[] ascending
        let t = *ord.add(0); *ord.add(0) = *ord.add(e as usize); *ord.add(e as usize) = t;
        let mut r = 0i32;
        loop { let mut c = 2 * r + 1; if c >= e { break }
            if c + 1 < e && s(c + 1) > s(c) { c += 1 }
            if s(c) <= s(r) { break }
            let t2 = *ord.add(r as usize); *ord.add(r as usize) = *ord.add(c as usize); *ord.add(c as usize) = t2; r = c; }
        e -= 1;
    }
    let (mut i, mut q) = (0i32, k - 1);
    while i < q { let t = *ord.add(i as usize); *ord.add(i as usize) = *ord.add(q as usize); *ord.add(q as usize) = t; i += 1; q -= 1; }
}
pub(crate) unsafe fn hf_leaves(w: *mut hf_arg_t) {
    let h: *mut hf_t = (*w).h; let j: *mut job_t = (*h).j;
    loop {
        let bi = (*h).bnext.fetch_add(1, SeqCst);
        if !(bi < (*j).nbk as usize) { break }
        let b = *(*h).order.add(bi) as usize;
        let reg: *mut f64 = (*h).rb.add(b * (*h).region); let out: *mut f64 = (*j).src.add(*(*h).bstart.add(b)); let nb = *(*h).cnt.add(b); let ns = *(*h).spc.add(b);
        let hv: *const hv_t = (*h).hv; let hq0: i32 = if !hv.is_null() { (*hv).lo[b] as i32 } else { 0 }; let hq1: i32 = if !hv.is_null() { (*hv).hi[b] as i32 } else { 0 };
        if (*h).lazy != 0 {
            // A view only needs the buckets PLACED, not ordered inside: copy the region and append the spills.
            if !(*h).vmin.is_null() {
                let mut mn = f64::INFINITY; let mut mx = f64::NEG_INFINITY;
                for i in 0..nb { let x = *reg.add(i); *out.add(i) = x; mn = if x < mn { x } else { mn }; mx = if x > mx { x } else { mx }; }
                for i in 0..ns { let x = (*(*h).spv.add(*(*h).spo.add(b) + i)).v; *out.add(nb + i) = x; mn = if x < mn { x } else { mn }; mx = if x > mx { x } else { mx }; }
                { let mut o = nb + ns; for q in hq0..hq1 { let v = (*hv).v[q as usize]; let c = (*hv).tot[q as usize]; for i in 0..c { *out.add(o + i) = v; } o += c; if c != 0 { mn = if v < mn { v } else { mn }; mx = if v > mx { v } else { mx }; } } }
                *(*h).vmin.add(b) = mn; *(*h).vmax.add(b) = mx;
            } else {
                memcpy(out, reg, nb * size_of::<f64>());
                for i in 0..ns { *out.add(nb + i) = (*(*h).spv.add(*(*h).spo.add(b) + i)).v; }
                { let mut o = nb + ns; for q in hq0..hq1 { let v = (*hv).v[q as usize]; let c = (*hv).tot[q as usize]; for i in 0..c { *out.add(o + i) = v; } o += c; } }
            }
            continue;
        }
        if ns == 0 {
            let r: *mut f64 = if nb > 1 { leaf_f64(reg, nb, out, (*w).ctx) } else { reg };
            if r != out { memcpy(out, r, nb * size_of::<f64>()); }
            if hq0 != hq1 { hv_insert(out, nb, hv, hq0, hq1); }
        } else {
            let sv: *mut f64 = malloc((if ns != 0 { ns } else { 1 }) * size_of::<f64>()) as *mut f64;
            for i in 0..ns { *sv.add(i) = (*(*h).spv.add(*(*h).spo.add(b) + i)).v; }
            sample_sort(sv, ns);
            let r: *mut f64 = if nb > 1 { leaf_f64(reg, nb, out, (*w).ctx) } else { reg };
            if r != reg { memcpy(reg, r, nb * size_of::<f64>()); }                    // sorted region back in the buffer, out is free
            let mut x: *const f64 = reg; let xe: *const f64 = reg.add(nb); let mut y: *const f64 = sv; let ye: *const f64 = sv.add(ns); let mut o: *mut f64 = out;
            while x < xe && y < ye { if *y < *x { *o = *y; o = o.add(1); y = y.add(1); } else { *o = *x; o = o.add(1); x = x.add(1); } }
            if x < xe { let k = xe.offset_from(x) as usize; memcpy(o, x, k * size_of::<f64>()); o = o.add(k); }
            if y < ye { memcpy(o, y, ye.offset_from(y) as usize * size_of::<f64>()); }
            free(sv);
            if hq0 != hq1 { hv_insert(out, nb + ns, hv, hq0, hq1); }
        }
    }
}
pub(crate) unsafe fn hf_worker(p: *mut c_void) -> *mut c_void {
    let w: *mut hf_arg_t = p as *mut hf_arg_t; let h: *mut hf_t = (*w).h;
    hf_scatter(w);
    barrier_wait(addr_of_mut!((*h).bar)); barrier_wait(addr_of_mut!((*h).bar));                    // main: sizes, spills, prefix, contexts
    if (*h).declined == 0 { hf_leaves(w); }
    null_mut()
}
// THE IN-PLACE PARTITION (C lines 4242-4472, ipp_*): opt-in by LOGOS_PART_INPLACE=1, not ported.
pub(crate) const g_part_inplace: i32 = 0;
/// returns 1 if the array was sorted, 0 if declined (a[] untouched; the histogram path follows)
pub(crate) unsafe fn hf_partition(j: *mut job_t, P: i32, ctxs: *mut ctx_t, th: *mut pthread_t, lazy: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) -> i32 {
    let n = (*j).n; let nbk = (*j).nbk;
    // One value holding more than a region can hold overflows it however the splitters fall: the sample knows, decline here.
    let hv: *mut hv_t = if lazy == 0 && g_part_inplace != 0 { null_mut() } else { (*j).hv };                // the in-place partition moves every element
    let dupf: f64 = if !hv.is_null() { (*hv).rest } else { (*j).dup_frac };
    if dupf * nbk as f64 > 1.0 + HF_SLACK { TRACE!("hf: declined up front, one value is {:.1}% of the data", dupf * 100.0); return 0 }
    let mut h_: hf_t = core::mem::zeroed(); let h: *mut hf_t = &mut h_; (*h).j = j; (*h).P = P;
    // if (!lazy && g_part_inplace) { hf_trial; ipp_partition ... }: g_part_inplace is 0, the block permutation is not ported
    (*h).cap = ((n / nbk as usize + 1) as f64 * (1.0 + HF_SLACK)) as usize + HF_BLOCK;
    (*h).region = ((*h).cap + 7) & !7usize;
    let need = (*h).region * nbk as usize;
    // THE POOL'S BLOCK FIRST: the regions borrow the pool's block, 64-byte aligned inside it, and give it back before returning;
    // the buffer of their own remains for when the pool is busy. (LOGOS_HF_OWN unset)
    let mut hpb: *mut c_void = null_mut(); let mut hpbi: *mut u8 = null_mut();
    let hpooled: i32 = (pool_acquire(need * size_of::<f64>() + 64, 0, &mut hpb, &mut hpbi) != 0) as i32;
    if hpooled != 0 { (*h).rb = ((hpb as usize + 63) & !63usize) as *mut f64; g_hf_pooled.fetch_add(1, Relaxed); }
    else {
        // grown, never preserved: the regions are overwritten before they are read
        if need > hf_buf_cap {
            free(hf_buf); hf_buf = null_mut(); hf_buf_cap = 0;
            let nbuf: *mut f64 = malloc(need * size_of::<f64>()) as *mut f64;       // posix_memalign(64): the prelude's blocks are 64-byte aligned
            hf_buf = nbuf; hf_buf_cap = need; trim_reset(addr_of_mut!(hf_trim));
        }
        trim_note(addr_of_mut!(hf_trim), hf_buf as *mut c_void, hf_buf_cap * size_of::<f64>(), need * size_of::<f64>());
        (*h).rb = hf_buf;
    }
    TRACE!("hf: regions in {}", if hpooled != 0 { "the pool's block" } else { "their own buffer" });
    (*h).cur = malloc(nbk as usize * size_of::<AtomicUsize>()) as *mut AtomicUsize; (*h).ovf = malloc(nbk as usize * size_of::<AtomicUsize>()) as *mut AtomicUsize;
    (*h).cnt = calloc(nbk as usize, size_of::<usize>()) as *mut usize; (*h).spo = calloc(nbk as usize + 1, size_of::<usize>()) as *mut usize; (*h).spc = calloc(nbk as usize, size_of::<usize>()) as *mut usize; (*h).bstart = calloc(nbk as usize + 1, size_of::<usize>()) as *mut usize;
    (*h).order = malloc(nbk as usize * size_of::<i32>()) as *mut i32;
    let wa: *mut hf_arg_t = calloc(P as usize, size_of::<hf_arg_t>()) as *mut hf_arg_t;
    let mut sorted = 0i32;
    {
        for b in 0..nbk as usize { core::ptr::write((*h).cur.add(b), AtomicUsize::new(0)); core::ptr::write((*h).ovf.add(b), AtomicUsize::new(usize::MAX)); }
        (*h).lazy = lazy; (*h).vmin = if lazy != 0 { vmin } else { null_mut() }; (*h).vmax = if lazy != 0 { vmax } else { null_mut() };
        hf_trial(h);
        TRACE!("hf: trial mode {} style {}, cap {} per bucket", (*h).mode, (*h).style, (*h).cap);
        if !hv.is_null() {                                            // each heavy value's bucket under the map just chosen; the maps are monotone, so value order is bucket order
            (*hv).lo = [0u8; MAX_BUCKETS as usize]; (*hv).hi = [0u8; MAX_BUCKETS as usize]; let mut pb = -1i32; let mut okm = 1i32;
            for q in 0..(*hv).n as usize { let b = hf_lookup(j, (*hv).v[q], (*h).mode, (*h).style); if b < pb || b < 0 || b >= nbk { okm = 0; break } if b != pb { (*hv).lo[b as usize] = q as u8 } (*hv).hi[b as usize] = (q + 1) as u8; pb = b; (*hv).tot[q] = 0; }
            if okm != 0 { (*h).hv = hv; g_hf_heavy.fetch_add(1, Relaxed); TRACE!("hf: {} heavy values counted in the scatter, the largest left is {:.2}% of the sample", (*hv).n, (*hv).rest * 100.0); }
            else if (*j).dup_frac * nbk as f64 > 1.0 + HF_SLACK { (*h).declined = 1 }   // a map that is not monotone: cannot happen; never count on it
        }
        if (*h).mode == 9 && !(*j).mdx.is_null() { g_mdgrid_scatters.fetch_add(1, Relaxed); }   // logos_twist_last(18)
        barrier_init(addr_of_mut!((*h).bar), P); (*h).next = AtomicUsize::new(0); (*h).bnext = AtomicUsize::new(0);
        for t in 0..P as usize { (*wa.add(t)).h = h; (*wa.add(t)).t = t as i32; (*wa.add(t)).ctx = ctxs.add(t); }
        let hf_made = par_spawn(th, P, hf_worker, wa as *mut c_void, size_of::<hf_arg_t>(), addr_of_mut!((*h).bar), null_mut());   // queue-driven throughout
        hf_scatter(wa);
        barrier_wait(addr_of_mut!((*h).bar));
        TRACE!("par: histfree scatter done");
        // sizes, spills by bucket, output offsets
        let mut S = 0usize; for t in 0..P as usize { S += (*wa.add(t)).spn; }
        if (*h).declined == 0 && S <= n / HF_SPILL_DIV {
            (*h).spv = malloc((if S != 0 { S } else { 1 }) * size_of::<hf_spill_t>()) as *mut hf_spill_t;
            {
                for b in 0..nbk as usize { let o = (*(*h).ovf.add(b)).load(SeqCst); let c = (*(*h).cur.add(b)).load(SeqCst); *(*h).cnt.add(b) = if o == usize::MAX { c } else { o }; }
                for t in 0..P as usize { for i in 0..(*wa.add(t)).spn { *(*h).spc.add((*(*wa.add(t)).sp.add(i)).b as usize) += 1; } }
                for b in 0..nbk as usize { *(*h).spo.add(b + 1) = *(*h).spo.add(b) + *(*h).spc.add(b); }
                { let fill: *mut usize = calloc(nbk as usize, size_of::<usize>()) as *mut usize;
                  for t in 0..P as usize { for i in 0..(*wa.add(t)).spn { let b = (*(*wa.add(t)).sp.add(i)).b as usize; *(*h).spv.add(*(*h).spo.add(b) + *fill.add(b)) = *(*wa.add(t)).sp.add(i); *fill.add(b) += 1; } } free(fill); }
                let mut maxb = 1usize; let mut lsz = [0usize; MAX_BUCKETS as usize];
                if !(*h).hv.is_null() { for q in 0..(*(*h).hv).n as usize { let mut c = 0usize; for t in 0..P as usize { c += (*wa.add(t)).hvc[q]; } (*(*h).hv).tot[q] = c; } }
                for b in 0..nbk as usize { let z = *(*h).cnt.add(b) + *(*h).spc.add(b); let mut hz = 0usize; if !(*h).hv.is_null() { for q in (*(*h).hv).lo[b] as usize..(*(*h).hv).hi[b] as usize { hz += (*(*h).hv).tot[q]; } }
                    lsz[b] = z; *(*h).bstart.add(b + 1) = *(*h).bstart.add(b) + z + hz; if z > maxb { maxb = z } }   // a leaf sorts the others; the heavy runs are filled in
                hf_order((*h).order, lsz.as_ptr(), nbk);                          // biggest leaf first: the last one picked up sets the finish time
                TRACE!("par: leaf sizes max {} mean {} -> imbalance {:.2}x", lsz[*(*h).order as usize], n / nbk as usize, lsz[*(*h).order as usize] as f64 * nbk as f64 / n as f64);
                if *(*h).bstart.add(nbk as usize) != n { (*h).declined = 1 }
                { let mut t = 0; while (*h).declined == 0 && t < P { if ctx_init(ctxs.add(t as usize), maxb, 0) == 0 { (*h).declined = 1 } t += 1; } }
                { let mut t = 0; while (*h).declined == 0 && t < P { (*ctxs.add(t as usize)).seq = (*j).seq_leaf; t += 1; } }   // the sequential leaf's policy, AVX2 quicksort included
                (*h).S = S;
            }
        } else { (*h).declined = 1 }
        TRACE!("par: histfree spills {} ({})", S, if (*h).declined != 0 { "declined" } else { "ok" });
        barrier_wait(addr_of_mut!((*h).bar));
        if (*h).declined == 0 { hf_leaves(wa); }
        par_join(th, hf_made);
        barrier_destroy(addr_of_mut!((*h).bar));
        sorted = ((*h).declined == 0) as i32;
        if sorted != 0 && lazy != 0 && !vstart.is_null() && !vnbk.is_null() {                        // hand the boundaries to the view
            for b in 0..=nbk as usize { *vstart.add(b) = *(*h).bstart.add(b); }
            *vnbk = nbk;
        }
        if sorted == 0 { for t in 0..P as usize { ctx_free(ctxs.add(t)); } }   // the histogram path re-inits them
    }
    for t in 0..P as usize { free((*wa.add(t)).sp); }
    free(wa); free((*h).cur); free((*h).ovf); free((*h).cnt); free((*h).spo); free((*h).spc); free((*h).bstart); free((*h).spv); free((*h).order);
    if hpooled != 0 { pool_release(); }
    sorted
}
/// the histogram path's worker (C line 4602)
pub(crate) unsafe fn worker(p: *mut c_void) -> *mut c_void {
    let w: *mut warg_t = p as *mut warg_t; let j: *mut job_t = (*w).j;
    do_hist(j);
    barrier_wait(addr_of_mut!((*j).bar)); barrier_wait(addr_of_mut!((*j).bar));          // second barrier: main's serial combine
    do_scatter(j);
    barrier_wait(addr_of_mut!((*j).bar));
    do_buckets(j, (*w).ctx);
    null_mut()
}

// C lines 4628-4640: the low-cardinality route's constants (their code is count.rs's)

/// C line 8603. The per-mode certificate from the SPLITTERS (see mdx_t): modes at the gulfs, each tested for uniformity on its own
/// sample slice; up to MD_LIN_MAX modes go to the compare lookup in the job, more to the grid. Returns 1 if every mode certified.
#[inline(never)]
pub(crate) unsafe fn md_certify(j: *mut job_t, sp: *const f64, nsp: i32, samp: *const f64, m: usize) -> i32 {
    if nsp < 8 || nsp > MAX_BUCKETS { return 0 }
    let mut gaps = [0f64; MAX_BUCKETS as usize]; let mut srt = [0f64; MAX_BUCKETS as usize]; let ng = nsp - 1;
    for i in 0..ng as usize { gaps[i] = *sp.add(i + 1) - *sp.add(i); }
    memcpy(srt.as_mut_ptr(), gaps.as_ptr(), ng as usize * size_of::<f64>()); sample_sort(srt.as_mut_ptr(), ng as usize);
    let med = srt[(ng / 2) as usize];
    if !(med > 0.0) { return 0 }
    let mut cut = [0i32; MD_MAX]; let mut nc = 0i32;                  // the widest MD_MAX - 1 gulfs
    for i in 0..ng { if gaps[i as usize] > BT_MODE_GAP * med {
        if nc < MD_MAX as i32 - 1 { cut[nc as usize] = i; nc += 1; }
        else { let mut wmin = 0i32; for q in 1..nc { if gaps[cut[q as usize] as usize] < gaps[cut[wmin as usize] as usize] { wmin = q } } if gaps[i as usize] > gaps[cut[wmin as usize] as usize] { cut[wmin as usize] = i } }
    } }
    if nc < 1 { return 0 }
    cut[..nc as usize].sort_unstable();                               // qsort(cut, nc, sizeof(int), cmp_int)
    let nm = nc + 1; let mut fi = [0i32; MD_MAX]; let mut la = [0i32; MD_MAX]; let mut lo = [0f64; MD_MAX + 1]; let mut hi = [0f64; MD_MAX]; let mut sc = [0f64; MD_MAX];
    { let mut first = 0i32; for k in 0..nm as usize { let last = if (k as i32) < nc { cut[k] } else { nsp - 1 }; fi[k] = first; la[k] = last; lo[k] = *sp.add(first as usize); hi[k] = *sp.add(last as usize); first = last + 1; } }
    let mut at = 0usize;
    for k in 0..nm as usize {                                         // uniform inside each mode
        if la[k] <= fi[k] || !(hi[k] > lo[k]) || !(hi[k] - lo[k]).is_finite() || !(1.0 / (hi[k] - lo[k])).is_finite() { return 0 }
        while at < m && *samp.add(at) < lo[k] { at += 1 }
        let from = at;
        while at < m && *samp.add(at) < hi[k] { at += 1 }
        let cnt = at - from;
        if cnt < 64 { return 0 }
        let mut D = 0f64; let inv = 1.0 / (hi[k] - lo[k]);
        for i in 0..cnt {
            let F = (*samp.add(from + i) - lo[k]) * inv; let mut e1 = F - i as f64 / cnt as f64; let mut e2 = (i + 1) as f64 / cnt as f64 - F;
            if e1 < 0.0 { e1 = -e1 }
            if e2 < 0.0 { e2 = -e2 }
            if e1 > D { D = e1 }
            if e2 > D { D = e2 }
        }
        if D > HF_KS_MAX / (cnt as f64).sqrt() { TRACE!("par: mode {} of {} is not uniform inside (KS {:.3})", k, nm, D); return 0 }
        sc[k] = (la[k] - fi[k]) as f64 / (hi[k] - lo[k]);
    }
    if nm <= MD_LIN_MAX {
        for k in 0..nm as usize { (*j).md_first[k] = fi[k]; (*j).md_last[k] = la[k]; (*j).md_lo[k] = lo[k]; (*j).md_hi[k] = hi[k]; (*j).md_scale[k] = sc[k]; }
        (*j).md_nm = nm; (*j).md_ok = 1;
        TRACE!("par: all {} modes certify uniform inside themselves (compares)", nm);
        return 1;
    }
    let g: *mut mdx_t = malloc(size_of::<mdx_t>()) as *mut mdx_t;
    (*g).nm = nm; (*g).cells = MD_GRID_CELLS; (*g).glo = lo[0];
    let span = hi[nm as usize - 1] - lo[0];
    if !(span > 0.0) || !span.is_finite() { free(g); return 0 }
    (*g).gscale = MD_GRID_CELLS as f64 / span;
    if !(*g).gscale.is_finite() || !((*g).gscale > 0.0) { free(g); return 0 }
    for k in 0..nm as usize { (*g).first[k] = fi[k]; (*g).last[k] = la[k]; (*g).lo[k] = lo[k]; (*g).hi[k] = hi[k]; (*g).scale[k] = sc[k]; }
    (*g).lo[nm as usize] = f64::NAN;                                  // the compare past the last boundary is false for every x, +inf included
    // grid[c] = boundaries lo[1..nm-1] whose own cell lies BELOW c; two boundaries in one cell: no grid
    let mut q = 1i32;
    for c in 0..MD_GRID_CELLS {
        while q < nm && md_cell(g, (*g).lo[q as usize]) < c { q += 1 }
        (*g).grid[c as usize] = (q - 1) as u8;
    }
    for b in 2..nm as usize { if md_cell(g, (*g).lo[b]) == md_cell(g, (*g).lo[b - 1]) { TRACE!("par: mode grid declined, two boundaries in one cell"); free(g); return 0 } }
    (*j).mdx = g; (*j).md_nm = nm; (*j).md_ok = 1;
    TRACE!("par: all {} modes certify uniform inside themselves (mode grid, {} cells)", nm, MD_GRID_CELLS);
    1
}

// IS THE SAMPLE UNSTRUCTURED? (C line 8665)
pub(crate) const HALVES_INV_MIN: f64 = 0.25;
pub(crate) const HALVES_FALL_MIN: f64 = 0.30;
pub(crate) const SEQLEAF_MIN_BUCKET: usize = 96000;
pub(crate) unsafe fn sample_unstructured(a: *const f64, n: usize, S: *const sodo_t) -> i32 {
    if n < 4096 || (*S).m < 64 { return 0 }
    if (*S).inv_all < HALVES_INV_MIN { TRACE!("unstructured? the sample is ordered (inv_all {:.3}): no", (*S).inv_all); return 0 }
    let mg = if (*S).mfull < SODO_GATE_M { (*S).mfull } else { SODO_GATE_M };      // distinct and Chao1 count the gate points only, before the extension
    if (*S).distinct < mg && (*S).chao1 < 0.5 * n as f64 { TRACE!("unstructured? the sample repeats (Chao1 {:.0}): no", (*S).chao1); return 0 }
    let (mut falls, mut pairs) = (0usize, 0usize);
    for w in 0..64i32 {
        let at = ((n - 34) as f64 * (((w + 1) as f64 * 0.6180339887498949) % 1.0)) as usize;
        for i in at..at + 32 { falls += (*a.add(i + 1) < *a.add(i)) as usize; pairs += 1; }
    }
    if (falls as f64) < HALVES_FALL_MIN * pairs as f64 { TRACE!("unstructured? adjacent order ({} of {} pairs fall): no", falls, pairs); return 0 }
    1
}
// TWO HALVES, TWO CORES (C line 8698)
pub(crate) const HALVES_MIN_N: usize = 131072;
pub(crate) const HALVES_BAL: f64 = 0.125;
pub(crate) static g_boosts: AtomicUsize = AtomicUsize::new(0);         // logos_twist_last(39)
pub(crate) static mut g_halves: usize = 0;                   // logos_twist_last(26): sorts the two-halves route finished
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct hv_arg_t { pub(crate) a: *mut f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) m: usize, pub(crate) s0: usize, pub(crate) s1: usize, pub(crate) piv: f64, pub(crate) buf: *mut f64, pub(crate) c: *mut ctx_t, pub(crate) phase: i32 }
pub(crate) unsafe fn hv_worker(p: *mut c_void) -> *mut c_void {
    let q: *mut hv_arg_t = p as *mut hv_arg_t; let a: *mut f64 = (*q).a;
    if (*q).phase == 0 {                                     // this thread's half, partitioned in place around the pivot
        let mut sm = f64::INFINITY; let mut bg = f64::NEG_INFINITY; let mut fl = 0i32;
        (*q).m = (*q).lo + av_partition_fl(a.add((*q).lo), (*q).hi - (*q).lo, (*q).piv, &mut sm, &mut bg, &mut fl);
    } else if (*q).phase == 1 {                              // its share of the block trade: pair i is (lo + i, hi - 1 - i)
        for i in (*q).s0..(*q).s1 { let x = (*q).lo + i; let y = (*q).hi - 1 - i; let t = *a.add(x); *a.add(x) = *a.add(y); *a.add(y) = t; }
    } else {                                                 // its part, by the sequential leaf
        let len = (*q).hi - (*q).lo;
        if len > 1 { let r = leaf_f64(a.add((*q).lo), len, (*q).buf.add((*q).lo), (*q).c); if r != a.add((*q).lo) { memcpy(a.add((*q).lo), r, len * size_of::<f64>()); } }
    }
    null_mut()
}
#[inline(never)]
pub(crate) unsafe fn halves_try_f64(a: *mut f64, n: usize, P: i32, sc: *mut scratch_t, S: *const sodo_t) -> i32 {
    let samp: *const f64 = (*S).sorted; let m = (*S).m;
    if P != 2 || n < HALVES_MIN_N || samp.is_null() || m < 64 || avx2_leaf_on() == 0 { return 0 }     // LOGOS_NO_HALVES unset
    if sample_unstructured(a, n, S) == 0 { return 0 }                 // order and repeats stay with the partition
    let piv = *samp.add(m / 2);
    let (mut lb, mut hb) = (0usize, m);                      // sample points below the pivot
    while lb < hb { let mid = lb + (hb - lb) / 2; if *samp.add(mid) < piv { lb = mid + 1 } else { hb = mid } }
    let share = lb as f64 / m as f64;
    if !(share >= 0.5 - HALVES_BAL && share <= 0.5 + HALVES_BAL) { TRACE!("halves: the median splits the sample {:.3} / {:.3}, declined", share, 1.0 - share); return 0 }
    let buf: *mut f64 = scratch_get(sc);
    if buf.is_null() { return 0 }
    let mut q: [hv_arg_t; 2] = core::mem::zeroed(); let mut th: [pthread_t; 2] = core::mem::zeroed(); let h = n / 2;
    q[0].a = a; q[1].a = a; q[0].piv = piv; q[1].piv = piv; q[0].buf = buf; q[1].buf = buf;
    q[0].lo = 0; q[0].hi = h; q[1].lo = h; q[1].hi = n;
    par_run(th.as_mut_ptr(), 2, hv_worker, q.as_mut_ptr() as *mut c_void, size_of::<hv_arg_t>(), 1);
    let m0 = q[0].m; let m1 = q[1].m - h;                    // left counts: [0, m0) and [h, h + m1) hold x < pivot (or NaN)
    let A = h - m0; let B = m1; let k = if A < B { A } else { B }; let L = m0 + m1;
    for t in 0..2usize { q[t].phase = 1; q[t].lo = m0; q[t].hi = h + m1; q[t].s0 = k * t / 2; q[t].s1 = k * (t + 1) / 2; }
    if k != 0 { par_run(th.as_mut_ptr(), 2, hv_worker, q.as_mut_ptr() as *mut c_void, size_of::<hv_arg_t>(), 1); }
    TRACE!("halves: pivot {:.6e}, parts {} / {} (sample said {:.3}), {} pairs traded", piv, L, n - L, share, k);
    let mut c: [ctx_t; 2] = core::mem::zeroed();
    let cap = if L > n - L { L } else { n - L };
    if ctx_init(&mut c[0], cap, 0) == 0 { return 0 }         // the array is a permutation of what came in: the caller sorts that
    if ctx_init(&mut c[1], cap, 0) == 0 { ctx_free(&mut c[0]); return 0 }
    c[0].seq = 1; c[1].seq = 1;
    for t in 0..2usize { q[t].phase = 2; q[t].c = &mut c[t]; }
    q[0].lo = 0; q[0].hi = L; q[1].lo = L; q[1].hi = n;
    par_run(th.as_mut_ptr(), 2, hv_worker, q.as_mut_ptr() as *mut c_void, size_of::<hv_arg_t>(), 1);
    ctx_free(&mut c[0]); ctx_free(&mut c[1]);
    g_halves += 1;
    1
}
// A LATTICE THE SAMPLE CANNOT SEE REPEAT (C line 8766)
#[cfg(not(feature = "port_all"))]
use missing::*;

#[cfg(test)]
mod tests {
    use super::*;

    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());     // hf_buf is a global, as in C: one partition at a time

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
    }
    const SHAPES: [&str; 9] = ["uniform", "heavy", "clustered", "clusters16", "zipf", "loguniform", "edges", "dups", "normal"];
    fn gen(shape: &str, n: usize, seed: u64) -> Vec<f64> {
        let mut r = Rng(seed | 1); let mut v = Vec::with_capacity(n);
        for i in 0..n {
            let u = r.unit();
            v.push(match shape {
                "uniform" => u * 1e6 - 3e5,
                "heavy" => (1.0 / (1.0 - u * 0.999999)).powf(1.5),
                "clustered" => ((r.next() % 3) as f64) * 1e9 + u * 1000.0,
                "clusters16" => ((r.next() % 16) as f64) * 1e7 + u * 1000.0,
                "zipf" => { let k = (1.0 / (1.0 - u * 0.9999)) as u64; (k % 50000) as f64 }
                "loguniform" => (u * 40.0 - 20.0).exp(),
                "edges" => match r.next() % 16 { 0 => 0.0, 1 => -0.0, 2 => f64::INFINITY, 3 => f64::NEG_INFINITY, 4 => 5e-324, 5 => -5e-324, _ => u * 200.0 - 100.0 },
                "dups" => ((r.next() % 1000) as f64) * 0.25,
                "normal" => { let mut s = 0.0; for _ in 0..12 { s += r.unit(); } s - 6.0 }
                _ => i as f64,
            });
        }
        v
    }
    fn check_sorted_perm(out: &[f64], inp: &[f64], what: &str) {
        for i in 1..out.len() { assert!(!(out[i] < out[i - 1]), "{what}: not ascending at {i}: {} then {}", out[i - 1], out[i]); }
        let mut x: Vec<u64> = out.iter().map(|v| v.to_bits()).collect(); let mut y: Vec<u64> = inp.iter().map(|v| v.to_bits()).collect();
        x.sort_unstable(); y.sort_unstable();
        assert!(x == y, "{what}: not a permutation of the input's bit patterns");
    }

    #[test]
    fn total_key_is_total_order() { let _port_guard = crate::port::port_test_lock();
        let mut v = vec![f64::NEG_INFINITY, -1e300, -1.0, -5e-324, -0.0, 0.0, 5e-324, 1.0, 1e300, f64::INFINITY, f64::NAN, -f64::NAN];
        v.extend(gen("uniform", 500, 7)); v.extend(gen("loguniform", 500, 9)); v.extend(gen("edges", 200, 11));
        unsafe { for &a in &v { for &b in &v { assert_eq!(total_key(a).cmp(&total_key(b)), a.total_cmp(&b), "{a} {b}"); } } }
    }

    #[test]
    fn bucket_of_is_the_linear_scan() { let _port_guard = crate::port::port_test_lock();
        for &nsp in &[1usize, 7, 31, 32, 33, 100, 255] {
            let mut sp = gen("dups", nsp, nsp as u64); sp.sort_by(|a, b| a.total_cmp(b));
            let mut xs = gen("dups", 2000, 99); xs.extend_from_slice(&sp); xs.push(f64::INFINITY); xs.push(f64::NEG_INFINITY); xs.push(-1.0);
            for &x in &xs { let lin = sp.iter().filter(|&&s| s <= x).count() as i32; unsafe { assert_eq!(bucket_of(x, sp.as_ptr(), nsp as i32), lin); } }
        }
    }

    /// every table lookup returns bucket_of's bucket (signed zeros aside: the key table orders -0.0 below +0.0)
    #[test]
    fn table_lookups_agree_with_bucket_of() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let (mut nv, mut nk, mut nd) = (0, 0, 0);
            for shape in ["uniform", "heavy", "clustered", "clusters16", "zipf", "loguniform", "dups", "normal"] {
                for &nbk in &[24i32, 48, 168, 256] {
                    let m = nbk as usize * OVERSAMPLE;
                    let mut samp = gen(shape, m, 1000 + nbk as u64); samp.sort_by(|a, b| a.total_cmp(b));
                    let nsp = nbk - 1; let sp: Vec<f64> = (1..nbk as usize).map(|b| samp[b * m / nbk as usize]).collect();
                    let mut t: bt_t = core::mem::zeroed();
                    if bt_init(&mut t, sp.as_ptr(), nsp, samp[0], samp[m - 1]) == 0 { continue }
                    let mut xs = gen(shape, 20000, 5); xs.extend_from_slice(&samp); xs.extend_from_slice(&sp);
                    for &s in &sp { xs.push(nextafter_up(s)); xs.push(-nextafter_up(-s)); }
                    xs.extend_from_slice(&[f64::INFINITY, f64::NEG_INFINITY, samp[0], samp[m - 1], samp[0] - 1.0, samp[m - 1] + 1.0, 1e-300, -1e-300, 1e300, -1e300]);
                    let mut res = 0usize;
                    for &x in &xs {
                        let want = bucket_of(x, sp.as_ptr(), nsp);
                        for style in 0..2 { for rp in [null_mut(), &mut res as *mut usize] {
                            if t.vok != 0 { assert_eq!(bt_lookup_v(&t, x, style, rp), want, "value table, {shape} nbk {nbk} x {x:e} style {style}"); nv += 1; }
                            if t.kok != 0 && x != 0.0 { assert_eq!(bt_lookup_k(&t, x, style, rp), want, "key table, {shape} nbk {nbk} x {x:e} style {style}"); nk += 1; }
                            if t.dok != 0 { assert_eq!(bt_lookup_d(&t, x, style, rp), want, "mode table, {shape} nbk {nbk} x {x:e} style {style}"); nd += 1; }
                        } }
                    }
                    // a NaN lands in a real bucket
                    if t.vok != 0 { assert_eq!(bt_lookup_v(&t, f64::NAN, 0, null_mut()), 0); }
                    if t.dok != 0 { assert_eq!(bt_lookup_d(&t, f64::NAN, 1, null_mut()), 0); }
                    if t.kok != 0 { let b = bt_lookup_k(&t, f64::NAN, 1, null_mut()); assert!(b >= 0 && b <= nsp); }
                    bt_free(&mut t);
                }
            }
            assert!(nv > 0 && nk > 0 && nd > 0, "a table was never built: value {nv} key {nk} modes {nd}");
        }
    }

    /// the dispatcher's job set-up (C lines 9101-9106, 9190-9371), for the tests
    unsafe fn job_new(a: *mut f64, n: usize, P: i32, nbk_force: i32) -> (*mut job_t, *mut f64, *mut f64, usize) {
        let j: *mut job_t = calloc(1, size_of::<job_t>()) as *mut job_t;
        (*j).P = P; (*j).nbk = P * BUCKETS_PER_THREAD; if (*j).nbk > MAX_BUCKETS { (*j).nbk = P * (if MAX_BUCKETS / P > 0 { MAX_BUCKETS / P } else { 1 }) }
        if nbk_force > 0 { (*j).nbk = nbk_force }
        (*j).nsp = (*j).nbk - 1; (*j).src = a; (*j).n = n;
        (*j).wc_on = (P >= WC_MIN_THREADS) as i32;
        (*j).csize = (n + P as usize * 4 - 1) / (P as usize * 4); if (*j).csize > CHUNK_MAX { (*j).csize = CHUNK_MAX } if (*j).csize < 1 { (*j).csize = 1 }
        (*j).nchunk = (n + (*j).csize - 1) / (*j).csize;
        let nbk = (*j).nbk as usize;
        let sp: *mut f64 = malloc((*j).nsp as usize * 8) as *mut f64;
        (*j).counts = calloc((*j).nchunk * nbk, 8) as *mut usize; (*j).offs = malloc((*j).nchunk * nbk * 8) as *mut usize;
        (*j).bsize = calloc(nbk, 8) as *mut usize; (*j).bstart = calloc(nbk, 8) as *mut usize;
        let mut m = nbk * OVERSAMPLE; if m > n { m = n }
        let samp: *mut f64 = malloc(m * 8) as *mut f64;
        for i in 0..m { let q = SAMPLE_POS(i, m, n); *samp.add(i) = *a.add(if q < n { q } else { n - 1 }); }
        sample_sort(samp, m);
        for b in 1..nbk { *sp.add(b - 1) = *samp.add(b * m / nbk); }
        (*j).sp = sp;
        (j, sp, samp, m)
    }
    unsafe fn job_maps(j: *mut job_t, sp: *mut f64, samp: *mut f64, m: usize) {
        (*j).bt_ok = bt_init(addr_of_mut!((*j).bt), sp, (*j).nsp, *samp, *samp.add(m - 1));
        (*j).ks_ok = 0; (*j).ks_d = 1.0; (*j).ks_lo = *samp; (*j).ks_hi = *samp.add(m - 1); (*j).ks_scale = 0.0;
        if m >= 256 && (*j).ks_hi > (*j).ks_lo && ((*j).ks_hi - (*j).ks_lo).is_finite() && (1.0 / ((*j).ks_hi - (*j).ks_lo)).is_finite() {
            let mut D = 0f64; let inv = 1.0 / ((*j).ks_hi - (*j).ks_lo);
            for i in 0..m { let F = (*samp.add(i) - (*j).ks_lo) * inv; let e1 = (F - i as f64 / m as f64).abs(); let e2 = ((i + 1) as f64 / m as f64 - F).abs(); if e1 > D { D = e1 } if e2 > D { D = e2 } }
            (*j).ks_d = D; (*j).ks_ok = (D <= HF_KS_MAX / (m as f64).sqrt()) as i32; (*j).ks_scale = (*j).nbk as f64 * inv;
        }
        (*j).ks_key_ok = 0; (*j).kk_lo = total_key(*samp); (*j).kk_hi = total_key(*samp.add(m - 1)); (*j).kk_scale = 0.0;
        if (*j).ks_ok == 0 && m >= 256 && (*j).kk_hi > (*j).kk_lo {
            let span = ((*j).kk_hi - (*j).kk_lo) as f64; let mut D = 0f64;
            for i in 0..m { let F = (total_key(*samp.add(i)) - (*j).kk_lo) as f64 / span; let e1 = (F - i as f64 / m as f64).abs(); let e2 = ((i + 1) as f64 / m as f64 - F).abs(); if e1 > D { D = e1 } if e2 > D { D = e2 } }
            (*j).ks_key_ok = (D <= HF_KS_MAX / (m as f64).sqrt()) as i32; (*j).kk_scale = (*j).nbk as f64 / span;
        }
        let mut hvmask: *mut u8 = null_mut();
        {   let mut cnt = [0usize; 256]; let (mut run, mut maxrun) = (1usize, 1usize);
            (*j).ex_e0 = total_key(*samp) >> EX_SHIFT;
            for i in 0..m {
                let q = total_key(*samp.add(i)) >> EX_SHIFT;
                let mut e: u32 = if q <= (*j).ex_e0 { 0 } else { (q - (*j).ex_e0) as u32 }; if e > 255 { e = 255 }
                cnt[e as usize] += 1;
                if i != 0 && *samp.add(i) == *samp.add(i - 1) { run += 1; if run > maxrun { maxrun = run } } else { run = 1 }
            }
            (*j).dup_frac = maxrun as f64 / m as f64;
            (*j).hv = hv_build(samp, m, (*j).nbk, &mut hvmask);
            let mut cum = 0usize; let mut prev = 0u32; let mut nonempty = 0;
            for e in 0..256usize {
                let mut base = ((*j).nbk as f64 * cum as f64 / m as f64) as u32;
                if base > ((*j).nbk - 1) as u32 { base = ((*j).nbk - 1) as u32 }
                if base < prev { base = prev }
                cum += cnt[e];
                let mut next = ((*j).nbk as f64 * cum as f64 / m as f64) as u32;
                if next > (*j).nbk as u32 { next = (*j).nbk as u32 }
                if next < base { next = base }
                (*j).ex[e].base = base; (*j).ex[e].span = next - base;
                (*j).ex[e].slope = if (*j).ex[e].span != 0 { (*j).ex[e].span as f64 / EX_SCALE } else { 0.0 };
                if cnt[e] != 0 { nonempty += 1 }
                prev = base;
            }
            (*j).ex_ok = (nonempty >= 2) as i32;
        }
        (*j).kg_ok = 0;
        if m >= 1024 && (*samp).is_finite() && (*samp.add(m - 1)).is_finite() && (*j).nbk >= 2 && (*j).nbk <= 256 {
            let klo = total_key(*samp); let khi = total_key(*samp.add(m - 1));
            if khi > klo {
                let span = khi - klo; let mut sh = 0i32;
                while (span >> sh) >= (1u64 << KG_BITS) { sh += 1 }
                let mut cells = (span >> sh) as u32 + 1; if cells > (1u32 << KG_BITS) { cells = 1u32 << KG_BITS }
                (*j).kg_lo = klo; (*j).kg_shift = sh; (*j).kg_cells = cells;
                let mut c = 0u32; let mut b = 0i32;
                for q in 1..(*j).nbk {
                    let iq = q as usize * m / (*j).nbk as usize;
                    let dq = (total_key(*samp.add(iq)) - klo) >> sh; let cq = if dq < cells as u64 { dq as u32 } else { cells - 1 };
                    while c < cq { (*j).kg_tab[c as usize] = b as u8; c += 1 }
                    b = q;
                }
                while c < cells { (*j).kg_tab[c as usize] = b as u8; c += 1 }
                (*j).kg_ok = 1;
            }
        }
        (*j).md_ok = 0; (*j).md_nm = 0;
        md_certify(j, sp, (*j).nsp, samp, m);
        {   let lim = HF_BAL_MAX * m as f64 / (*j).nbk as f64;
            let jc: *const job_t = j; let nbk = (*j).nbk;
            let bal = move |mode: i32| -> bool {
                let mut load = [0u32; MAX_BUCKETS as usize];
                for i in 0..m { if !hvmask.is_null() && *hvmask.add(i) != 0 { continue } let x = *samp.add(i);
                    let b = match mode { 6 => hf_lookup(jc, x, 6, 0), 7 => hf_lookup(jc, x, 7, 0), 9 => hf_lookup_md(jc, x), 8 => hf_lookup_ex(jc, x), _ => hf_lookup_kg(jc, x) };
                    if b >= 0 && b < nbk { load[b as usize] += 1 } }
                let mx = load[..nbk as usize].iter().copied().max().unwrap();
                !(mx as f64 > lim)
            };
            if (*j).ks_ok != 0 && !bal(6) { (*j).ks_ok = 0 }
            if (*j).ks_key_ok != 0 && !bal(7) { (*j).ks_key_ok = 0 }
            if (*j).md_ok != 0 && !bal(9) { (*j).md_ok = 0 }
            if (*j).ex_ok != 0 && !bal(8) { (*j).ex_ok = 0 }
            if (*j).kg_ok != 0 && !bal(10) { (*j).kg_ok = 0 }
        }
        free(hvmask);
    }
    unsafe fn job_free(j: *mut job_t, sp: *mut f64) {
        bt_free(addr_of_mut!((*j).bt)); free((*j).mdx); free((*j).hv);
        free(sp); free((*j).counts); free((*j).offs); free((*j).bsize); free((*j).bstart); free(j);
    }
    /// which: 0 = as the dispatcher goes (hf_partition, then the histogram path on a decline), 1 = the histogram path alone,
    /// 2 = hf_partition with the table trial forced (no certificate, no key grid). Returns 1 when hf_partition sorted it.
    unsafe fn drive(a: *mut f64, n: usize, P: i32, which: i32, exmap: bool) -> i32 {
        let (j, sp, samp, m) = job_new(a, n, P, 0);
        job_maps(j, sp, samp, m);
        if !exmap { (*j).ex_ok = 0 }                                   // the default build
        if which == 2 { (*j).ks_ok = 0; (*j).ks_key_ok = 0; (*j).md_ok = 0; (*j).kg_ok = 0; }
        free(samp);
        let wa: *mut warg_t = malloc(P as usize * size_of::<warg_t>()) as *mut warg_t; let th: *mut pthread_t = calloc(P as usize, size_of::<pthread_t>()) as *mut pthread_t;
        let ctxs: *mut ctx_t = calloc(P as usize, size_of::<ctx_t>()) as *mut ctx_t;
        let mut done = 0;
        if which != 1 { done = hf_partition(j, P, ctxs, th, 0, null_mut(), null_mut(), null_mut(), null_mut()); }
        if done == 0 {
            let dst: *mut f64 = malloc(n * 8) as *mut f64; let bidx: *mut u8 = malloc(n);
            (*j).dst = dst; (*j).bidx = bidx;
            barrier_init(addr_of_mut!((*j).bar), P); (*j).hnext = AtomicUsize::new(0); (*j).snext = AtomicUsize::new(0); (*j).next = AtomicUsize::new(0);
            for t in 0..P as usize { (*wa.add(t)).j = j; (*wa.add(t)).t = t as i32; (*wa.add(t)).ctx = ctxs.add(t); }
            let hist_made = par_spawn(th, P, worker, wa as *mut c_void, size_of::<warg_t>(), addr_of_mut!((*j).bar), null_mut());
            do_hist(j);
            barrier_wait(addr_of_mut!((*j).bar));
            let nbk = (*j).nbk as usize;
            for ch in 0..(*j).nchunk { for b in 0..nbk { *(*j).bsize.add(b) += *(*j).counts.add(ch * nbk + b); } }
            for b in 1..nbk { *(*j).bstart.add(b) = *(*j).bstart.add(b - 1) + *(*j).bsize.add(b - 1); }
            for b in 0..nbk { let mut acc = *(*j).bstart.add(b); for ch in 0..(*j).nchunk { *(*j).offs.add(ch * nbk + b) = acc; acc += *(*j).counts.add(ch * nbk + b); } }
            let mut maxb = 1usize; for b in 0..nbk { if *(*j).bsize.add(b) > maxb { maxb = *(*j).bsize.add(b) } }
            for t in 0..P as usize { ctx_init(ctxs.add(t), maxb, (*j).wc_on); }
            barrier_wait(addr_of_mut!((*j).bar));
            do_scatter(j);
            barrier_wait(addr_of_mut!((*j).bar));
            do_buckets(j, ctxs);
            par_join(th, hist_made);
            barrier_destroy(addr_of_mut!((*j).bar));
            free(dst); free(bidx);
        }
        for t in 0..P as usize { ctx_free(ctxs.add(t)); }
        free(wa); free(th); free(ctxs);
        job_free(j, sp);
        done
    }

    /// the approximate maps are monotone and stay inside [0, nbk): all a partition needs
    #[test]
    fn grids_are_monotone_and_in_range() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let mut fired = [0usize; 5];
            for shape in SHAPES {
                for &P in &[2i32, 4, 14] {
                    let n = 200_000usize; let mut a = gen(shape, n, 77 + P as u64);
                    let (j, sp, samp, m) = job_new(a.as_mut_ptr(), n, P, 0);
                    job_maps(j, sp, samp, m);
                    let mut xs = a.clone(); xs.extend_from_slice(&[f64::INFINITY, f64::NEG_INFINITY, 0.0, -0.0, 1e308, -1e308]); xs.sort_by(|x, y| x.total_cmp(y));
                    let nbk = (*j).nbk;
                    for (ix, &(mode, ok)) in [(6, (*j).ks_ok), (7, (*j).ks_key_ok), (9, (*j).md_ok), (8, (*j).ex_ok), (10, (*j).kg_ok)].iter().enumerate() {
                        if ok == 0 { continue }
                        fired[ix] += 1;
                        // the exponent map clamps every binade past the 255th above the lowest sampled into ONE entry, so it is monotone
                        // only where the data spans no more than that (see the report: a sample holding both signs, or a zero, does not)
                        let mono = mode != 8 || (total_key(a.iter().copied().fold(f64::NEG_INFINITY, f64::max)) >> EX_SHIFT) - (*j).ex_e0 <= 255;
                        let mut pb = 0i32;
                        for &x in &xs { if mode == 8 && !(x.is_finite() && x.abs() < 1e307 && x != 0.0) { continue }
                            let b = hf_lookup(j, x, mode, 0); assert!(b >= 0 && b < nbk, "mode {mode} {shape} P {P}: bucket {b} for {x:e}"); if mono { assert!(b >= pb, "mode {mode} {shape} P {P}: not monotone at {x:e} ({pb} then {b})"); } pb = b; }
                        let b = hf_lookup(j, f64::NAN, mode, 0); assert!(b >= 0 && b < nbk, "mode {mode}: a NaN went to bucket {b}");
                        if mode == 8 { for &x in &xs { assert_eq!(hf_lookup_ex(j, x), hf_lookup(j, x, 8, 0)); } }
                    }
                    // the exact modes through hf_lookup are bucket_of
                    for mode in [1, 2, 5, 3] {
                        if (*j).bt_ok == 0 && mode != 3 { continue }
                        if (mode == 1 && (*j).bt.vok == 0) || (mode == 2 && (*j).bt.kok == 0) || (mode == 5 && (*j).bt.dok == 0) { continue }
                        for &x in xs.iter().step_by(7) { if x == 0.0 { continue } assert_eq!(hf_lookup(j, x, mode, 1), bucket_of(x, sp, (*j).nsp), "mode {mode} {shape}"); }
                    }
                    free(samp); job_free(j, sp);
                }
            }
            assert!(fired.iter().all(|&f| f > 0), "a map never certified on any shape: grid, key grid, modes, exponent, kg = {fired:?}");
        }
    }

    #[test]
    fn mode_grid_past_eight_modes() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let n = 400_000usize; let mut a = gen("clusters16", n, 4242);
            let (j, sp, samp, m) = job_new(a.as_mut_ptr(), n, 14, 0);
            job_maps(j, sp, samp, m);
            assert!((*j).md_ok != 0 && !(*j).mdx.is_null() && (*j).md_nm == 16, "16 flat clusters must certify through the grid: ok {} nm {}", (*j).md_ok, (*j).md_nm);
            let g = (*j).mdx;
            for &x in a.iter().step_by(3) { let mut k = 0usize; for q in 1..(*g).nm as usize { k += (x >= (*g).lo[q]) as usize; } let got = (*g).grid[md_cell(g, x) as usize] as usize + (x >= (*g).lo[(*g).grid[md_cell(g, x) as usize] as usize + 1]) as usize; assert_eq!(got, k, "the grid's mode for {x}"); }
            free(samp); job_free(j, sp);
            // three clusters: the compare form
            let mut a = gen("clustered", n, 4243);
            let (j, sp, samp, m) = job_new(a.as_mut_ptr(), n, 14, 0);
            job_maps(j, sp, samp, m);
            assert!((*j).md_ok != 0 && (*j).mdx.is_null() && (*j).md_nm == 3, "three flat clusters: ok {} nm {}", (*j).md_ok, (*j).md_nm);
            free(samp); job_free(j, sp);
        }
    }

    #[test]
    fn heavy_values_build_and_insert() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            // a sorted sample: 3 heavy values (bit-distinct zeros among them) in a uniform rest
            let mut samp: Vec<f64> = gen("uniform", 4000, 3).iter().map(|x| x.abs() + 1.0).collect();
            samp.extend(std::iter::repeat(-0.0).take(300)); samp.extend(std::iter::repeat(0.0).take(200)); samp.extend(std::iter::repeat(777.5).take(500)); samp.extend(std::iter::repeat(5.0).take(3));
            samp.sort_by(|a, b| a.total_cmp(b));
            let m = samp.len(); let mut mask: *mut u8 = null_mut();
            let hv = hv_build(samp.as_ptr(), m, 48, &mut mask);
            assert!(!hv.is_null() && !mask.is_null());
            assert_eq!((*hv).n, 3);
            assert_eq!(((*hv).v[0].to_bits(), (*hv).v[1].to_bits(), (*hv).v[2]), ((-0.0f64).to_bits(), 0.0f64.to_bits(), 777.5));
            for q in 0..3 { assert_eq!((*hv).b[q], (*hv).v[q].to_bits()); }
            for i in 0..m { let heavy = samp[i] == 0.0 || samp[i] == 777.5; assert_eq!(*mask.add(i) != 0, heavy, "mask at {i}"); }
            assert_eq!((*hv).rest, 3.0 / m as f64);
            // hv_insert: the others sorted, the heavy runs filled in
            (*hv).tot[0] = 11; (*hv).tot[1] = 0; (*hv).tot[2] = 29;
            let mut others: Vec<f64> = samp.iter().copied().filter(|&x| !(x == 0.0 || x == 777.5)).collect(); others.extend_from_slice(&[-3.0, -2.0, 777.5 - 1e-9, 777.5 + 1e-9]); others.sort_by(|a, b| a.total_cmp(b));
            let no = others.len(); let mut out = others.clone(); out.resize(no + 40, f64::NAN);
            hv_insert(out.as_mut_ptr(), no, hv, 0, 3);
            let mut want = others.clone(); want.extend(std::iter::repeat(-0.0).take(11)); want.extend(std::iter::repeat(777.5).take(29));
            check_sorted_perm(&out, &want, "hv_insert");
            // nothing heavy: null; too many heavy runs: the cut rises until at most HV_MAX are left
            free(hv); free(mask);
            let flat = { let mut v = gen("uniform", 4000, 8); v.sort_by(|a, b| a.total_cmp(b)); v };
            assert!(hv_build(flat.as_ptr(), flat.len(), 48, &mut mask).is_null() && mask.is_null());
            let mut many: Vec<f64> = Vec::new(); for k in 0..200 { many.extend(std::iter::repeat(k as f64).take(20 + k)); }
            let hv = hv_build(many.as_ptr(), many.len(), 256, &mut mask);
            assert!(!hv.is_null() && (*hv).n as usize <= HV_MAX && (*hv).n > 0);
            for q in 1..(*hv).n as usize { assert!((*hv).v[q] > (*hv).v[q - 1]); }
            free(hv); free(mask);
        }
    }

    #[test]
    fn hf_order_identity_by_default() { let _port_guard = crate::port::port_test_lock();
        unsafe { let sz = [5usize, 1, 9, 3]; let mut ord = [0i32; 4]; hf_order(ord.as_mut_ptr(), sz.as_ptr(), 4); assert_eq!(ord, [0, 1, 2, 3]); }
    }

    #[test]
    fn partition_end_to_end() { let _port_guard = crate::port::port_test_lock();
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            let (mut hf_sorted, mut total) = (0, 0);
            let heavy0 = g_hf_heavy.load(SeqCst);
            for shape in SHAPES {
                for &P in &[2i32, 4, 14] {
                    for &n in &[1usize << 16, 300_007] {
                        for which in 0..3 {
                            let inp = gen(shape, n, 31 * n as u64 + P as u64); let mut a = inp.clone();
                            let done = drive(a.as_mut_ptr(), n, P, which, false);
                            check_sorted_perm(&a, &inp, &format!("{shape} n {n} P {P} which {which} (hf sorted: {done})"));
                            if which != 1 { hf_sorted += done; total += 1; }
                        }
                    }
                }
            }
            assert!(hf_sorted * 2 > total, "the histogram-free partition declined most inputs: {hf_sorted} of {total}");
            assert!(g_hf_heavy.load(SeqCst) > heavy0, "no partition counted a heavy value");
            // the exponent map through the trial (opt-in in C; its lookup is ported)
            for shape in ["heavy", "loguniform"] { let inp = gen(shape, 250_000, 5); let mut a = inp.clone(); drive(a.as_mut_ptr(), inp.len(), 4, 2, true); check_sorted_perm(&a, &inp, &format!("{shape} with the exponent map")); }
        }
    }

    /// the view's form: buckets placed, not sorted, with their boundaries and extremes
    #[test]
    fn partition_lazy_places_buckets() { let _port_guard = crate::port::port_test_lock();
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            for shape in ["uniform", "heavy", "zipf", "clusters16"] {
                let n = 300_000usize; let P = 4; let inp = gen(shape, n, 12345); let mut a = inp.clone();
                let (j, sp, samp, m) = job_new(a.as_mut_ptr(), n, P, 0);
                job_maps(j, sp, samp, m); (*j).ex_ok = 0; free(samp);
                let th: *mut pthread_t = calloc(P as usize, size_of::<pthread_t>()) as *mut pthread_t; let ctxs: *mut ctx_t = calloc(P as usize, size_of::<ctx_t>()) as *mut ctx_t;
                let nbk = (*j).nbk as usize; let mut vstart = vec![0usize; nbk + 1]; let mut vnbk = 0i32; let mut vmin = vec![0f64; nbk]; let mut vmax = vec![0f64; nbk];
                let done = hf_partition(j, P, ctxs, th, 1, vstart.as_mut_ptr(), &mut vnbk, vmin.as_mut_ptr(), vmax.as_mut_ptr());
                if done != 0 {
                    assert_eq!(vnbk as usize, nbk); assert_eq!(vstart[0], 0); assert_eq!(vstart[nbk], n);
                    let mut prev = f64::NEG_INFINITY;
                    for b in 0..nbk {
                        assert!(vstart[b] <= vstart[b + 1]);
                        let s = &a[vstart[b]..vstart[b + 1]];
                        if s.is_empty() { assert!(vmin[b] == f64::INFINITY && vmax[b] == f64::NEG_INFINITY); continue }
                        let mn = s.iter().copied().fold(f64::INFINITY, f64::min); let mx = s.iter().copied().fold(f64::NEG_INFINITY, f64::max);
                        assert_eq!((mn, mx), (vmin[b], vmax[b]), "{shape}: bucket {b}'s extremes");
                        assert!(mn >= prev, "{shape}: bucket {b} starts below the one before"); prev = mx;
                    }
                    a.sort_by(|x, y| x.total_cmp(y)); check_sorted_perm(&a, &inp, shape);
                } else { assert!(a.iter().zip(&inp).all(|(x, y)| x.to_bits() == y.to_bits()), "{shape}: a decline must leave the array untouched"); }
                free(th); free(ctxs); job_free(j, sp);
            }
        }
    }

    #[test]
    fn halves_route() { let _port_guard = crate::port::port_test_lock();
        unsafe {
            let h0 = g_halves;
            for shape in ["uniform", "normal", "heavy", "edges"] {
                for &n in &[HALVES_MIN_N, 400_001] {
                    let inp = gen(shape, n, 9 + n as u64); let mut a = inp.clone();
                    let m = 768usize; let mut samp: Vec<f64> = (0..m).map(|i| a[SAMPLE_POS(i, m, n)]).collect(); samp.sort_by(|x, y| x.total_cmp(y));
                    let mut S: sodo_t = core::mem::zeroed(); S.m = m; S.mfull = m; S.distinct = m; S.chao1 = 1e18; S.inv_all = 0.5; S.sorted = samp.as_mut_ptr();
                    let mut sc: scratch_t = core::mem::zeroed(); sc.n = n;
                    assert_eq!(halves_try_f64(a.as_mut_ptr(), n, 4, &mut sc, &S), 0);
                    let r = halves_try_f64(a.as_mut_ptr(), n, 2, &mut sc, &S);
                    free(sc.dst);
                    if r != 0 { check_sorted_perm(&a, &inp, &format!("halves {shape} n {n}")); }
                    else { a.sort_by(|x, y| x.total_cmp(y)); check_sorted_perm(&a, &inp, &format!("halves declined {shape} n {n}")); }
                    if shape == "uniform" || shape == "normal" { assert_eq!(r, 1, "{shape} n {n} must take the two halves"); }
                }
            }
            assert!(g_halves > h0);
            // an ordered sample, a repeating one, adjacent order: declines
            let n = 200_000usize; let a = gen("uniform", n, 1);
            let mut S: sodo_t = core::mem::zeroed(); S.m = 768; S.mfull = 768; S.distinct = 768; S.chao1 = 1e18; S.inv_all = 0.5;
            assert_eq!(sample_unstructured(a.as_ptr(), n, &S), 1);
            S.inv_all = 0.1; assert_eq!(sample_unstructured(a.as_ptr(), n, &S), 0); S.inv_all = 0.5;
            S.distinct = 700; S.chao1 = 5000.0; assert_eq!(sample_unstructured(a.as_ptr(), n, &S), 0); S.distinct = 768;
            let saw: Vec<f64> = (0..n).map(|i| (i % 1000) as f64).collect();
            assert_eq!(sample_unstructured(saw.as_ptr(), n, &S), 0);
        }
    }
}
