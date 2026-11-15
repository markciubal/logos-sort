//! C lines 5700-5865, 5866-6030 (nearsorted_parallel_f64), 6965-7034 (the reach constants and ladder), 7035-7432, 7620-7849:
//! the parallel near-sorted route, the short-reach route, the shared sample (`sodo_*`), the tail and head merges.
//! A transliteration: see PORTING.md.
use super::*;
use core::mem::MaybeUninit;

// ---- the near-sorted route (C 5704-6030) -------------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ns_chunk_arg_t {
    pub(crate) a: *mut f64, pub(crate) dst: *mut f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) ctx: *mut ctx_t,
    pub(crate) tlo: f64, pub(crate) thi: f64, pub(crate) nin: usize, pub(crate) nstray: usize,
}
pub(crate) unsafe fn lower_bound_d(a: *const f64, mut lo: usize, mut hi: usize, v: f64) -> usize {
    while lo < hi { let m = lo + ((hi - lo) >> 1); if *a.add(m) < v { lo = m + 1 } else { hi = m } }
    lo
}
pub(crate) const NS_POISON: usize = 16;
/* phase 1: one greedy scan of the chunk (kept to the front of the chunk's region in dst, strays to the back) */
pub(crate) unsafe fn ns_split_sort(p: *mut c_void) -> *mut c_void {
    let w = p as *mut ns_chunk_arg_t;
    ns_split_scan(w);
    null_mut()
}
pub(crate) unsafe fn ns_split_scan(w: *mut ns_chunk_arg_t) {
    let lo = (*w).lo; let hi = (*w).hi;
    let mut f = lo; let mut b = hi; let tlo = (*w).tlo; let thi = (*w).thi; let a = (*w).a as *const f64; let dst = (*w).dst;
    let mut kmax = f64::NEG_INFINITY; let mut streak: usize = 0; let mut streak_start = lo;
    let mut i = lo;
    while i < hi {
        let v = *a.add(i);
        let n1 = if i + 1 < hi { *a.add(i + 1) } else { f64::INFINITY };
        let n2 = if i + 2 < hi { *a.add(i + 2) } else { f64::INFINITY };
        let inb = ((v >= tlo) & (v < thi)) as i32; let ge = (v >= kmax) as i32; let nb = ((v <= n1) | (v <= n2)) as i32;
        if (inb & ge & nb) != 0 { *dst.add(f) = v; f += 1; kmax = v; streak = 0; i += 1; continue; }
        if inb != 0 && ge == 0 && f > lo {
            if streak == 0 { streak_start = i; }
            streak += 1;
            if streak >= NS_POISON {
                f -= 1; let poison = *dst.add(f); kmax = if f > lo { *dst.add(f - 1) } else { f64::NEG_INFINITY };
                b += streak - 1;                                   /* un-push the streak's strays (all but this one, not pushed yet) */
                b -= 1; *dst.add(b) = poison;
                i = streak_start; streak = 0; continue;            /* re-evaluate the streak under the corrected maximum (C: i = streak_start - 1, then i++) */
            }
        } else { streak = 0; }
        b -= 1; *dst.add(b) = v;
        i += 1;
    }
    (*w).nin = f - lo; (*w).nstray = hi - b;
}
/* phase 2: one band = its sorted in-band run merged with its slice of the sorted strays, into a[] */
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ns_merge2_t {
    pub(crate) run: *const f64, pub(crate) strays: *const f64, pub(crate) nrun: usize, pub(crate) s0: usize, pub(crate) s1: usize,
    pub(crate) out: *mut f64, pub(crate) tid: i32,
}
pub(crate) unsafe fn ns_merge2(p: *mut c_void) -> *mut c_void {
    let w = p as *mut ns_merge2_t;
    let mut x = (*w).run; let xe = x.add((*w).nrun); let mut y = (*w).strays.add((*w).s0); let ye = (*w).strays.add((*w).s1); let mut o = (*w).out;
    while x < xe && y < ye { if *y < *x { *o = *y; o = o.add(1); y = y.add(1); } else { *o = *x; o = o.add(1); x = x.add(1); } }
    if x < xe { let c = xe.offset_from(x) as usize; memcpy(o, x, c * size_of::<f64>()); o = o.add(c); }
    if y < ye { memcpy(o, y, (ye.offset_from(y) as usize) * size_of::<f64>()); }
    null_mut()
}
/* returns 1 if sorted, 0 if declined (a[] untouched) */
pub(crate) const NS_STRAYS_PARALLEL: usize = 65536;
pub(crate) const NS_WIDE_MIN_N: usize = 6000000;
pub(crate) const NS_INV_RATIO_SMALL: f64 = 0.025;
thread_local! { pub(crate) static ns_depth: Cell<i32> = const { Cell::new(0) }; }
/* A NESTED sort's context bundles are free again when it returns */
macro_rules! NESTED_SORT {
    ($call:expr) => {{
        let cm_ = g_ctx_next;
        ns_depth.with(|d| d.set(d.get() + 1));
        $call;
        ns_depth.with(|d| d.set(d.get() - 1));
        g_ctx_next = cm_;
    }};
}
pub(crate) use NESTED_SORT;
/* The lattice route's spills, after the emit has written the lattice's own nl elements to a[0, nl): sorted on their own, then
 * merged in from the top. */
/* THE STRAND GATE (FINDINGS 3bw) */
pub(crate) const NS_STRAND_WIN_DESC: usize = 4;
pub(crate) const NS_STRAND_MIN_DESC: usize = 4;
pub(crate) const NS_STRAND_M: usize = 2048;
pub(crate) static g_ns_strand: AtomicUsize = AtomicUsize::new(0);
#[inline(never)]
pub(crate) unsafe fn nearsorted_parallel_f64(a: *mut f64, n: usize, P: i32, mut dst: *mut f64, lz: *mut scratch_t, mut inv_rate: f64) -> i32 {
    if P < 2 || P > 256 || ns_depth.with(|d| d.get()) > 0 { return 0; }
    let ns_gate_old: i32 = 0;                                  /* LOGOS_NS_GATE_OLD unset */
    /* LOGOS_NO_NEARSORTED unset */
    if inv_rate < 0.0 {                                        /* gate: the dispatcher's 40-point strided inversion sample */
        let ss = if n < 40 { n } else { 40 }; let mut st = n / ss; if st < 1 { st = 1; } let mut inv: usize = 0; let mut comps: usize = 0;
        let mut i = 0usize;
        while i + st < n { if *a.add(i) > *a.add(i + st) { inv += 1; } comps += 1; i += st; }
        if comps == 0 { return 0; }
        inv_rate = inv as f64 / comps as f64;
    }
    if inv_rate > (if ns_gate_old != 0 || n >= NS_WIDE_MIN_N { NEARLY_SORTED_INV_RATIO } else { NS_INV_RATIO_SMALL }) { return 0; }

    /* pre-gate: the greedy scan on four 4096-element windows */
    if n >= 4 * 4096 {
        let mut rej: usize = 0; let mut tot: usize = 0; let mut wdesc: usize = usize::MAX;
        if ns_gate_old != 0 {
            for w in 0..4usize {
                let lo = (n / 5) * (w + 1) - 2048; let hi = lo + 4096; let mut kmax = f64::NEG_INFINITY;
                for i in lo..hi { let v = *a.add(i); let n1 = *a.add(i + 1); let n2 = *a.add(i + 2); if (v >= kmax) & ((v <= n1) | (v <= n2)) { kmax = v; } else { rej += 1; } tot += 1; }
            }
        } else {
            let mut win = [0.0f64; 4096]; let mut pw: ns_chunk_arg_t = core::mem::zeroed(); wdesc = 0;
            for w in 0..4usize {
                let lo = (n / 5) * (w + 1) - 2048;
                pw.a = a.add(lo); pw.dst = win.as_mut_ptr(); pw.lo = 0; pw.hi = 4096; pw.tlo = f64::NEG_INFINITY; pw.thi = f64::INFINITY; pw.ctx = null_mut();
                ns_split_scan(&mut pw); rej += pw.nstray; tot += 4096;
                for i in lo..lo + 4096 { wdesc += (*a.add(i + 1) < *a.add(i)) as usize; }          /* the element's scale: adjacent descents */
            }
        }
        if rej * 4 > tot { return 0; }
        if wdesc <= NS_STRAND_WIN_DESC {                                              /* LOGOS_NO_NSSTRAND unset */
            let ps = sample_by_pos(a, n, NS_STRAND_M);                                /* NULL (a NaN): no verdict, the route goes on as before */
            if !ps.is_null() {
                let mut sdesc: usize = 0; for i in 1..NS_STRAND_M { sdesc += ((*ps.add(i)).v < (*ps.add(i - 1)).v) as usize; }
                free(ps);
                if sdesc >= NS_STRAND_MIN_DESC {
                    TRACE!("ns: strand gate: {} descents in the windows, {} in the position-ordered sample -> strands, declined", wdesc, sdesc);
                    g_ns_strand.fetch_add(1, SeqCst); return 0;
                }
                TRACE!("ns: strand gate: {} descents in the windows, {} in the sample -> one strand, the route goes on", wdesc, sdesc);
            }
        }
    }
    let chunk = (n + P as usize - 1) / P as usize;
    let ctxs = calloc(P as usize, size_of::<ctx_t>()) as *mut ctx_t; let ca = malloc(P as usize * size_of::<ns_chunk_arg_t>()) as *mut ns_chunk_arg_t;
    let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let mut m = P as usize * 32; if m > n { m = n; }
    let samp = malloc(m * size_of::<f64>()) as *mut f64; let thr = malloc((P as usize + 1) * size_of::<f64>()) as *mut f64;
    let mut ok: i32 = 1; let mut sorted: i32 = 0;
    for i in 0..m { let q = SAMPLE_POS(i, m, n); *samp.add(i) = *a.add(if q < n { q } else { n - 1 }); if *samp.add(i) != *samp.add(i) { ok = 0; } }
    if ok != 0 {
        /* Band thresholds at the CHUNK BOUNDARIES, not sample quantiles */
        *thr = f64::NEG_INFINITY; *thr.add(P as usize) = f64::INFINITY;
        for b in 1..P as usize {
            let c0 = b * chunk; let w0 = if c0 >= 16 { c0 - 16 } else { 0 }; let w1 = if c0 + 17 <= n { c0 + 17 } else { n }; let k = w1 - w0;
            let mut win = [0.0f64; 33]; memcpy(win.as_mut_ptr(), a.add(w0), k * size_of::<f64>()); insertion_sort(win.as_mut_ptr(), 0, k as i64 - 1); *thr.add(b) = win[k / 2];
            if *thr.add(b) != *thr.add(b) { ok = 0; }
        }
        let mut b = 1usize; while b + 1 < P as usize { if !(*thr.add(b) <= *thr.add(b + 1)) { ok = 0; } b += 1; }   /* boundaries must ascend, else this is not near-sorted */
        if ok != 0 && dst.is_null() { dst = scratch_get(lz); if dst.is_null() { ok = 0; } }   /* the scratch, taken only now */
        let mut t = 0usize;
        while ok != 0 && t < P as usize {
            let c = ca.add(t);
            (*c).a = a; (*c).dst = dst; (*c).lo = t * chunk; (*c).hi = (*c).lo + chunk; if (*c).hi > n { (*c).hi = n; } if (*c).lo > n { (*c).lo = n; }
            (*c).ctx = ctxs.add(t); (*c).tlo = *thr.add(t); (*c).thi = *thr.add(t + 1); (*c).nin = 0; (*c).nstray = 0;
            if ctx_init(ctxs.add(t), chunk, 0) == 0 { ok = 0; }
            t += 1;
        }
    }
    if ok != 0 {
        /* 1. split + sort the in-band part of every chunk, in parallel; a[] is untouched */
        TRACE!("ns: thresholds + contexts ready");
        par_run(th, P, ns_split_sort, ca as *mut c_void, size_of::<ns_chunk_arg_t>(), 1);
        let mut S: usize = 0; for t in 0..P as usize { S += (*ca.add(t)).nstray; }
        TRACE!("ns: split + in-band sorts done, {} strays", S);
        if S <= n / 4 {                                          /* more strays than that: not near-sorted, decline */
            /* 2. gather and sort the strays once (serial: ~1% of n), cut them by the same thresholds */
            let strays = malloc((if S != 0 { S } else { 1 }) * size_of::<f64>()) as *mut f64; let sscr = malloc((if S != 0 { S } else { 1 }) * size_of::<f64>()) as *mut f64;
            let sb = malloc((P as usize + 1) * size_of::<usize>()) as *mut usize; let mg = malloc(P as usize * size_of::<ns_merge2_t>()) as *mut ns_merge2_t;
            {
                let mut w: usize = 0;
                for t in 0..P as usize { let c = ca.add(t); memcpy(strays.add(w), dst.add((*c).hi - (*c).nstray), (*c).nstray * size_of::<f64>()); w += (*c).nstray; }
                if S >= 2 {
                    if S >= NS_STRAYS_PARALLEL && P >= 2 { NESTED_SORT!(logos_sort_f64(strays, S, P)); }
                    else {
                        let mut sc: ctx_t = core::mem::zeroed();
                        if ctx_init(&mut sc, S, 0) != 0 { let r = sort_fn(strays, S, sscr, &mut sc); if r != strays { memcpy(strays, r, S * size_of::<f64>()); } ctx_free(&mut sc); }
                        else { sample_sort(strays, S); }
                    }
                }
                *sb = 0; for b in 1..P as usize { *sb.add(b) = lower_bound_d(strays, 0, S, *thr.add(b)); } *sb.add(P as usize) = S;
                TRACE!("ns: strays sorted and cut");
                /* 3. one two-way merge per band, straight into a[] */
                let mut off: usize = 0;
                for t in 0..P as usize {
                    let g = mg.add(t); let c = ca.add(t);
                    (*g).run = dst.add((*c).lo); (*g).nrun = (*c).nin; (*g).strays = strays; (*g).s0 = *sb.add(t); (*g).s1 = *sb.add(t + 1); (*g).out = a.add(off); (*g).tid = t as i32;
                    off += (*c).nin + (*sb.add(t + 1) - *sb.add(t));
                }
                if off == n {
                    par_run(th, P, ns_merge2, mg as *mut c_void, size_of::<ns_merge2_t>(), 1);
                    sorted = 1;
                    TRACE!("ns: band merges done");
                }
            }
            free(strays); free(sscr); free(sb); free(mg);
        }
    }
    for t in 0..P as usize { ctx_free(ctxs.add(t)); }
    free(ctxs); free(ca); free(th); free(samp); free(thr);
    (sorted == 1) as i32
}

// ---- the short-reach route (C 6968-7329) ---------------------------------------------------------------------------------
/* file-scope statics of C lines 249-251 that this route is the main user of */
pub(crate) static mut g_reach_relax: i32 = 0;      /* the wave route's pieces: the short-reach route below its usual size and block count */
pub(crate) static mut g_seamed: i32 = 0;           /* the seam route's re-entry guard */
pub(crate) static mut g_seam_count: usize = 0;
pub(crate) const TAIL_MIN_N: usize = 4096;                 /* C line 7618 */
pub(crate) static mut g_reach_seam: usize = 0;     /* a rotation the reach route found in its fences, for the dispatcher to make */
/* logos_psort.h line 68 */
pub(crate) const LOGOS_REACH_NOT_TRIED: i32 = 0;
pub(crate) const LOGOS_REACH_SORTED: i32 = 1;
pub(crate) const LOGOS_REACH_SORTED_STRAYS: i32 = 2;
pub(crate) const LOGOS_REACH_DECLINED_GATE: i32 = 3;
pub(crate) const LOGOS_REACH_DECLINED_PROBE: i32 = 4;
pub(crate) const LOGOS_REACH_DECLINED_PLAN: i32 = 5;
pub(crate) const LOGOS_REACH_DECLINED_NOMEM: i32 = 6;
pub(crate) const LOGOS_REACH_DECLINED_KINKS: i32 = 7;

pub(crate) const REACH_MIN_N: usize = 1 << 18;
pub(crate) const REACH_TAU: f64 = 0.02;
pub(crate) const REACH_BLOCK: usize = 32768;
pub(crate) const REACH_BLOCK_MAX: usize = 131072;
pub(crate) const REACH_MIN_BLOCKS: usize = 8;
pub(crate) const REACH_DENSE: f64 = 0.20;
pub(crate) const REACH_KINK_FRAC: f64 = 0.90;
pub(crate) const REACH_KINK_MIN: usize = 3;
pub(crate) const REACH_STRAY_DIV: usize = 32;
pub(crate) const REACH_PROBES: i32 = 8;
pub(crate) const REACH_TOLERANT_P: i32 = 4;
thread_local! { pub(crate) static g_reach_last: Cell<i32> = const { Cell::new(LOGOS_REACH_NOT_TRIED) }; }
pub(crate) const REACH_LADDER_M: usize = 2048;
/* The ladder. Returns the reach estimate in elements, 0 when even the longest rung is inverted too often. */
#[inline(never)]
pub(crate) unsafe fn reach_from_sample(a: *const f64, n: usize, mut raw: *const f64, mut m: usize, mirror: i32) -> usize {
    const LAG: [u16; 6] = [144, 233, 377, 610, 987, 1597];
    let mut own = [0.0f64; REACH_LADDER_M]; let mut pos = [0usize; REACH_LADDER_M];
    if n < REACH_LADDER_M { return 0; }
    if m < REACH_LADDER_M { let mut nan: i32 = 0; sample_read(a, n, 0, REACH_LADDER_M, REACH_LADDER_M, own.as_mut_ptr(), &mut nan); if nan != 0 { return 0; } raw = own.as_ptr(); }
    m = REACH_LADDER_M;
    for i in 0..m { pos[i] = SAMPLE_POS(i, m, n); }
    let mut best: usize = 0;
    for k in 0..6 {
        let L = LAG[k] as usize; let mut inv: usize = 0; let mut comps: usize = 0; let mut dist: usize = 0;
        let mut i = 0usize;
        while i + L < m {
            let p0 = pos[i]; let p1 = pos[i + L]; if p0 == p1 { i += 1; continue; }
            let d = if p1 > p0 { p1 - p0 } else { p0 - p1 }; if d > n / 2 { i += 1; continue; }      /* a wrapped pair */
            comps += 1; dist = d;
            { let lo_ = if p1 > p0 { *raw.add(i) } else { *raw.add(i + L) }; let hi_ = if p1 > p0 { *raw.add(i + L) } else { *raw.add(i) };
              if if mirror != 0 { lo_ < hi_ } else { lo_ > hi_ } { inv += 1; } }                   /* lo_: the EARLIER position */
            i += 1;
        }
        if comps < 64 || inv as f64 > REACH_TAU * comps as f64 { break; }
        best = dist;
    }
    best
}
pub(crate) unsafe fn reach_upper_bound(a: *const f64, mut lo: usize, mut hi: usize, v: f64) -> usize {
    while lo < hi { let m = lo + ((hi - lo) >> 1); if *a.add(m) <= v { lo = m + 1 } else { hi = m } }
    lo
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct reach_seg_t { pub(crate) src: usize, pub(crate) len: usize, pub(crate) dst: usize }
/* not Clone / Copy: it holds atomics */
#[repr(C)]
pub(crate) struct reach_job_t {
    pub(crate) a: *mut f64, pub(crate) T: *mut f64, pub(crate) fence: *mut f64, pub(crate) n: usize, pub(crate) B: usize, pub(crate) S: usize,
    pub(crate) K: i32, pub(crate) go: i32, pub(crate) leafmode: i32, pub(crate) tolerant: i32,   /* go: 0 decline, 1 seams only, 2 seams, then the strays are put back */
    pub(crate) lo: *mut usize, pub(crate) hi: *mut usize, pub(crate) x: *mut usize, pub(crate) y: *mut usize,   /* per block: core [lo,hi) and the seam counts */
    pub(crate) nblk: AtomicUsize, pub(crate) nseam: AtomicUsize, pub(crate) nstrayblk: AtomicUsize,
    pub(crate) bar: *mut barrier_t,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct reach_arg_t { pub(crate) j: *mut reach_job_t, pub(crate) ctx: *mut ctx_t, pub(crate) scratch: *mut f64, pub(crate) tid: i32 }
#[inline] pub(crate) unsafe fn reach_s(j: *const reach_job_t, k: i32) -> usize { k as usize * (*j).B }
#[inline] pub(crate) unsafe fn reach_e(j: *const reach_job_t, k: i32) -> usize { if k + 1 == (*j).K { (*j).n } else { (k + 1) as usize * (*j).B } }
#[inline]
pub(crate) unsafe fn reach_est_median(p: *const f64, len: usize) -> f64 {
    let mut w = [0.0f64; 9]; let st = len / 9; for i in 0..9usize { w[i] = *p.add(i * st + st / 2); }
    insertion_sort(w.as_mut_ptr(), 0, 8); w[4]
}
/* four independent chains each way */
pub(crate) unsafe fn reach_minmax(p: *const f64, nb: usize, mn: *mut f64, mx: *mut f64) {
    let mut l0 = *p; let mut l1 = l0; let mut l2 = l0; let mut l3 = l0; let mut h0 = l0; let mut h1 = l0; let mut h2 = l0; let mut h3 = l0; let mut i: usize = 0;
    while i + 4 <= nb { let v0 = *p.add(i); let v1 = *p.add(i + 1); let v2 = *p.add(i + 2); let v3 = *p.add(i + 3);
        l0 = if v0 < l0 { v0 } else { l0 }; l1 = if v1 < l1 { v1 } else { l1 }; l2 = if v2 < l2 { v2 } else { l2 }; l3 = if v3 < l3 { v3 } else { l3 };
        h0 = if v0 > h0 { v0 } else { h0 }; h1 = if v1 > h1 { v1 } else { h1 }; h2 = if v2 > h2 { v2 } else { h2 }; h3 = if v3 > h3 { v3 } else { h3 };
        i += 4; }
    while i < nb { let v = *p.add(i); l0 = if v < l0 { v } else { l0 }; h0 = if v > h0 { v } else { h0 }; i += 1; }
    l0 = if l1 < l0 { l1 } else { l0 }; l2 = if l3 < l2 { l3 } else { l2 }; h0 = if h1 > h0 { h1 } else { h0 }; h2 = if h3 > h2 { h3 } else { h2 };
    *mn = if l2 < l0 { l2 } else { l0 }; *mx = if h2 > h0 { h2 } else { h0 };
}
pub(crate) unsafe fn reach_breaks_fence(j: *const reach_job_t, k: i32) -> i32 {
    let mut mn = 0.0f64; let mut mx = 0.0f64; reach_minmax((*j).a.add(reach_s(j, k)), reach_e(j, k) - reach_s(j, k), &mut mn, &mut mx);
    ((k > 0 && mn < *(*j).fence.add((k - 1) as usize)) || (k + 1 < (*j).K && mx > *(*j).fence.add((k + 1) as usize))) as i32
}
/* The block sort proper. Returns 0 when the block is not its kind. */
pub(crate) const REACH_FLASH_MAXPOP: u32 = 48;
pub(crate) unsafe fn reach_sort_direct(w: *mut reach_arg_t, p: *mut f64, nb: usize, mn: f64, mx: f64, mode: i32) -> i32 {
    if nb < FLASH_SORT_THRESHOLD || !(mx > mn) || !(mx - mn).is_finite() { return 0; }
    if (mx - mn) < COUNTING_SORT_K as f64 * nb as f64 {           /* integers over a small span: the general leaf counts them, which no sort beats */
        let mut ints: i32 = 1; let st8 = nb / 8; let mut q = 0usize;
        while q < 8 && ints != 0 { let v = *p.add(q * st8); ints = (v == v.floor()) as i32; q += 1; }
        if ints != 0 { return 0; }
    }
    if mode == 1 && (*(*w).ctx).seq != 0 && avx2_leaf_on() != 0 && avx2_qsort_f64(p, nb) != 0 { return 1; }
    { let st = nb / 32; let mut desc: usize = 0; let mut i = 0usize;
      while i + 1 < nb && desc == 0 { desc = (*p.add(i) > *p.add(i + 1)) as usize; i += st; }
      if desc == 0 { return 0; } }                                 /* no descent in 32 looks: perhaps sorted already */
    { let st = nb / 64; let mut d4: usize = 0; let mut i = 0usize;
      while i + 4 < nb { d4 += (*p.add(i) > *p.add(i + 4)) as usize; i += st; }
      if d4 <= 3 && insertion_sort_momentum(p, 0, nb as i64 - 1) != 0 { return 1; } }
    let m = nb; let scale = (m - 1) as f64 / (mx - mn);
    if !scale.is_finite() || !(scale > 0.0) { return 0; }
    let mut cf: ctx_fit_t = core::mem::zeroed(); if ctx_fit((*w).ctx, nb, &mut cf) == 0 { return 0; }
    let cnt = (*(*w).ctx).aux; let buf = (*w).scratch; let mut ok: i32 = 1;
    memset(cnt, 0, (m + 1) * size_of::<u32>());
    for i in 0..nb { let d = (*p.add(i) - mn) * scale; let idx = if d >= 0.0 { d as usize } else { 0 }; *cnt.add(idx + 1) += 1; }
    let mut big: u32 = 0; for i in 1..=m { if *cnt.add(i) > big { big = *cnt.add(i); } *cnt.add(i) = (*cnt.add(i)).wrapping_add(*cnt.add(i - 1)); }
    if big > REACH_FLASH_MAXPOP { ok = 0; }
    else {
        for i in 0..nb { let v = *p.add(i); let d = (v - mn) * scale; let idx = if d >= 0.0 { d as usize } else { 0 }; let c = cnt.add(idx); *buf.add(*c as usize) = v; *c = (*c).wrapping_add(1); }
        insertion_sort(buf, 0, nb as i64 - 1);
        memcpy(p, buf, nb * size_of::<f64>());
    }
    ctx_unfit((*w).ctx, &mut cf);
    ok
}
pub(crate) unsafe fn reach_sort_range(w: *mut reach_arg_t, p: *mut f64, nb: usize) {
    let mut cf: ctx_fit_t = core::mem::zeroed(); if nb < 2 { return; }
    if ctx_fit((*w).ctx, nb, &mut cf) != 0 { let r = leaf_f64(p, nb, (*w).scratch, (*w).ctx); if r != p { memcpy(p, r, nb * size_of::<f64>()); } ctx_unfit((*w).ctx, &mut cf); }
    else { quicksort(p, 0, nb as i64 - 1, 2 * log2i(nb)); }
}
pub(crate) unsafe fn reach_blocks(w: *mut reach_arg_t) {
    let j = (*w).j;
    loop {
        let k = (*j).nblk.fetch_add(1, SeqCst);
        if !(k < (*j).K as usize) { break; }
        let s = reach_s(j, k as i32); let nb = reach_e(j, k as i32) - s; let p = (*j).a.add(s);
        let mut bmn = 0.0f64; let mut bmx = 0.0f64; reach_minmax(p, nb, &mut bmn, &mut bmx);
        let flo = if k > 0 { *(*j).fence.add(k - 1) } else { f64::NEG_INFINITY }; let fhi = if k + 1 < (*j).K as usize { *(*j).fence.add(k + 1) } else { f64::INFINITY };
        if bmn < flo || bmx > fhi {                                   /* lows | core | highs, then each part on its own */
            let mut lt: usize = 0; let mut gt = nb; let mut i: usize = 0;
            while i < gt { let v = *p.add(i); if v < flo { *p.add(i) = *p.add(lt); *p.add(lt) = v; lt += 1; i += 1; } else if v > fhi { gt -= 1; *p.add(i) = *p.add(gt); *p.add(gt) = v; } else { i += 1; } }
            reach_sort_range(w, p, lt); reach_sort_range(w, p.add(gt), nb - gt);
            if gt - lt > 1 { let mut cmn = 0.0f64; let mut cmx = 0.0f64; reach_minmax(p.add(lt), gt - lt, &mut cmn, &mut cmx);
                if !((*j).leafmode != 0 && reach_sort_direct(w, p.add(lt), gt - lt, cmn, cmx, (*j).leafmode) != 0) { reach_sort_range(w, p.add(lt), gt - lt); } }
            (*j).nstrayblk.fetch_add(1, Relaxed);
        } else if !((*j).leafmode != 0 && reach_sort_direct(w, p, nb, bmn, bmx, (*j).leafmode) != 0) { reach_sort_range(w, p, nb); }
    }
}
/* One seam: the y elements at the end of core k-1 and the x at the start of core k, merged in place. */
pub(crate) unsafe fn reach_seams(w: *mut reach_arg_t) {
    let j = (*w).j; let tmp = (*w).scratch;
    loop {
        let k = (*j).nseam.fetch_add(1, SeqCst);
        if !(k < (*j).K as usize) { break; }
        if k == 0 || *(*j).y.add(k) == 0 { continue; }
        let y = *(*j).y.add(k); let x = *(*j).x.add(k); let mut o = (*j).a.add(*(*j).hi.add(k - 1) - y); let oe = (*j).a.add(*(*j).hi.add(k - 1));
        memcpy(tmp, o, y * size_of::<f64>());
        let mut p = tmp as *const f64; let pe = tmp.add(y) as *const f64; let mut r = (*j).a.add(*(*j).lo.add(k)) as *const f64; let re = r.add(x);
        while p < pe && r < re { let v; if *r < *p { v = *r; r = r.add(1); } else { v = *p; p = p.add(1); } *o = v; o = o.add(1); if o == oe { o = (*j).a.add(*(*j).lo.add(k)); } }
        while p < pe { *o = *p; o = o.add(1); p = p.add(1); if o == oe { o = (*j).a.add(*(*j).lo.add(k)); } }
    }
}
/* Between the block sorts and the seams, on the calling thread while the others wait at the barrier: medians, fences,
 * strays, seam counts. */
pub(crate) unsafe fn reach_plan(j: *mut reach_job_t) {
    let a = (*j).a; let K = (*j).K; let mut S: usize = 0;
    (*j).go = 0;
    for k in 0..K {
        let s = reach_s(j, k); let e = reach_e(j, k); let mid = s + (e - s) / 2;
        let mlo = if k > 0 { *a.add(reach_s(j, k - 1) + (reach_e(j, k - 1) - reach_s(j, k - 1)) / 2) } else { *a.add(s) };
        let mhi = if k + 1 < K { *a.add(reach_s(j, k + 1) + (reach_e(j, k + 1) - reach_s(j, k + 1)) / 2) } else { *a.add(e - 1) };
        if !(mlo <= *a.add(mid)) || !(*a.add(mid) <= mhi) { TRACE!("reach: medians do not ascend at block {} -> decline", k); return; }
        let lo = if k > 0 { lower_bound_d(a, s, e, mlo) } else { s }; let hi = if k + 1 < K { reach_upper_bound(a, s, e, mhi) } else { e };
        if !(lo <= mid && mid < hi) { TRACE!("reach: core of block {} lost its median -> decline", k); return; }
        *(*j).lo.add(k as usize) = lo; *(*j).hi.add(k as usize) = hi; S += (lo - s) + (e - hi);
    }
    if S > (*j).n / REACH_STRAY_DIV { TRACE!("reach: {} strays of {} -> decline", S, (*j).n); return; }
    *(*j).x = 0; *(*j).y = 0; *(*j).y.add(K as usize) = 0;
    for k in 1..K as usize {
        let lfirst = *a.add(*(*j).lo.add(k)); let llast = *a.add(*(*j).hi.add(k - 1) - 1); *(*j).x.add(k) = 0; *(*j).y.add(k) = 0;
        if !(lfirst < llast) { continue; }
        *(*j).y.add(k) = *(*j).hi.add(k - 1) - reach_upper_bound(a, *(*j).lo.add(k - 1), *(*j).hi.add(k - 1), lfirst);
        *(*j).x.add(k) = lower_bound_d(a, *(*j).lo.add(k), *(*j).hi.add(k), llast) - *(*j).lo.add(k);
    }
    for k in 0..K as usize { if *(*j).x.add(k) + *(*j).y.add(k + 1) >= *(*j).hi.add(k) - *(*j).lo.add(k) { TRACE!("reach: block {} keeps no middle -> decline", k); return; } }
    (*j).S = S; (*j).go = if S != 0 { 2 } else { 1 };
}
/* The strays go back in, in place: the array is a list of segments, each one memmove. */
pub(crate) unsafe fn reach_place_strays(j: *mut reach_job_t, sg: *mut reach_seg_t, tpos: *mut usize) {
    let a = (*j).a; let T = (*j).T as *const f64; let S = (*j).S; let mut ns: usize = 0; let mut ti: usize = 0; let mut cb: usize = 0;
    for k in 0..(*j).K as usize {
        let lo = *(*j).lo.add(k); let hi = *(*j).hi.add(k); let mut p = lo;
        while ti < S && p < hi && *T.add(ti) <= *a.add(hi - 1) {                /* this stray sorts before some element of this core */
            let q = lower_bound_d(a, p, hi, *T.add(ti));
            if q > p { (*sg.add(ns)).src = p; (*sg.add(ns)).len = q - p; (*sg.add(ns)).dst = cb + (p - lo) + ti; ns += 1; }
            *tpos.add(ti) = cb + (q - lo) + ti; ti += 1; p = q;
        }
        if hi > p { (*sg.add(ns)).src = p; (*sg.add(ns)).len = hi - p; (*sg.add(ns)).dst = cb + (p - lo) + ti; ns += 1; }
        cb += hi - lo;
    }
    while ti < S { *tpos.add(ti) = cb + ti; ti += 1; }                          /* above every core element (and any NaN among them) */
    for i in 0..ns { let g = *sg.add(i); if g.dst < g.src { memmove(a.add(g.dst), a.add(g.src), g.len * size_of::<f64>()); } }
    let mut i = ns; while i > 0 { i -= 1; let g = *sg.add(i); if g.dst > g.src { memmove(a.add(g.dst), a.add(g.src), g.len * size_of::<f64>()); } }
    for i in 0..S { *a.add(*tpos.add(i)) = *T.add(i); }
}
pub(crate) unsafe fn reach_worker(p: *mut c_void) -> *mut c_void {
    let w = p as *mut reach_arg_t; let j = (*w).j;
    reach_blocks(w);
    if !(*j).bar.is_null() { barrier_wait((*j).bar); barrier_wait((*j).bar); }       /* all blocks sorted; then the caller has planned */
    if (*j).go != 0 { reach_seams(w); }
    null_mut()
}
/* returns 1 if a[] is sorted; 0 if it declined, and then a[] is a permutation of what it was (possibly with blocks sorted) */
pub(crate) unsafe fn reach_try_f64(a: *mut f64, n: usize, mut P: i32, reach: usize) -> i32 {
    if reach == 0 || ns_depth.with(|d| d.get()) > 0 || (n < REACH_MIN_N && g_reach_relax == 0) { return 0; }   /* LOGOS_NO_REACH unset */
    g_reach_last.with(|c| c.set(LOGOS_REACH_DECLINED_GATE));
    let mut B = REACH_BLOCK; if 4 * reach > B { B = (4 * reach + 4095) & !4095usize; }
    if B > REACH_BLOCK_MAX || n / B < (if g_reach_relax != 0 { 2usize } else { REACH_MIN_BLOCKS }) { TRACE!("reach: {} needs blocks of {} -> decline", reach, B); return 0; }
    { let mut desc: usize = 0; let mut tot: usize = 0; let mut kinkish: usize = 0;      /* dense or sparse? four windows, the near-sorted pre-gate's */
      for w in 0..4usize { let lo = (n / 5) * (w + 1) - 2048; let hi = lo + 4096;
        for i in lo..hi { let d = (*a.add(i) > *a.add(i + 1)) as i32; desc += d as usize; tot += 1;
          /* the detector's kink test (its j is this i + 1): the inverted pair, swapped back, fits between its neighbours */
          kinkish += (d != 0 && i >= 1 && i + 2 < n && *a.add(i + 1) >= *a.add(i - 1) && *a.add(i) <= *a.add(i + 2)) as usize; } }
      let rdense = REACH_DENSE;                                                          /* LOGOS_REACH_DENSE unset */
      if (desc as f64) < rdense * tot as f64 {
          let kmin = REACH_KINK_MIN;                                                     /* LOGOS_REACH_KINK_MIN unset */
          if desc >= kmin && kinkish as f64 >= REACH_KINK_FRAC * desc as f64 {           /* LOGOS_NO_KINKGATE unset */
              TRACE!("reach: {} descents in {}, {} kink-shaped -> sparse KINKS, decline for the detector", desc, tot, kinkish);
              g_reach_last.with(|c| c.set(LOGOS_REACH_DECLINED_KINKS)); return 0; }
          TRACE!("reach: {} descents in {} ({} kink-shaped) -> sparse, decline", desc, tot, kinkish); return 0; } }
    let K = (n / B) as i32; if P > K { P = K; } if P < 1 { P = 1; }
    let maxlen = n - (K - 1) as usize * B;                              /* the last block takes the remainder, under 2B */
    let mut j: reach_job_t = core::mem::zeroed(); j.a = a; j.n = n; j.B = B; j.K = K;
    j.leafmode = 2;                                                     /* LOGOS_REACH_LEAF unset */
    j.nblk = AtomicUsize::new(0); j.nseam = AtomicUsize::new(0); j.nstrayblk = AtomicUsize::new(0);
    j.fence = malloc(K as usize * size_of::<f64>()) as *mut f64;
    j.tolerant = (P <= REACH_TOLERANT_P) as i32;
    let mut probe_bad: i32 = 0;
    let mut q: i32 = 0;
    while q < REACH_PROBES && probe_bad < 2 && j.tolerant == 0 {
        let k = ((q as usize * 2 + 1) * K as usize / (2 * REACH_PROBES) as usize) as i32;
        let mut d: i32 = -1;
        while d <= 1 { if k + d >= 0 && k + d < K { *j.fence.add((k + d) as usize) = reach_est_median(a.add(reach_s(&j, k + d)), reach_e(&j, k + d) - reach_s(&j, k + d)); } d += 2; }
        probe_bad += reach_breaks_fence(&j, k);
        q += 1;
    }
    if probe_bad >= 2 { TRACE!("reach: reach {}, but probe blocks hold strays -> decline", reach); free(j.fence); g_reach_last.with(|c| c.set(LOGOS_REACH_DECLINED_PROBE)); return 0; }
    for k in 0..K { *j.fence.add(k as usize) = reach_est_median(a.add(reach_s(&j, k)), reach_e(&j, k) - reach_s(&j, k)); }
    /* THE SEAM: the fences ascending everywhere but at ONE block, where they drop by most of the range, means a rotation. */
    if ns_depth.with(|d| d.get()) == 0 && g_seamed == 0 && g_reach_relax == 0 && K >= 4 {          /* LOGOS_NO_SEAM unset */
        let mut drops: i32 = 0; let mut kd: i32 = -1; let mut fmin = *j.fence; let mut fmax = *j.fence;
        for k in 0..K { let fk = *j.fence.add(k as usize); if fk < fmin { fmin = fk; } if fk > fmax { fmax = fk; } if k + 1 < K && *j.fence.add((k + 1) as usize) < fk { drops += 1; kd = k; } }
        if drops == 1 && kd >= 1 && kd + 2 < K && *j.fence.add(kd as usize) - *j.fence.add((kd + 1) as usize) >= 0.5 * (fmax - fmin) {
            let lo = reach_s(&j, kd); let hi = reach_e(&j, kd + 1) - 1; let mut best = lo; let mut bd = f64::NEG_INFINITY;
            for q in lo..hi { let d = *a.add(q) - *a.add(q + 1); if d > bd { bd = d; best = q; } }
            if bd > 0.0 {
                if P == 1 { g_reach_seam = best + 1; }
                TRACE!("reach: the fences drop once, at block {}: a rotation, seam {} ({})", kd, best + 1, if P == 1 { "rotate" } else { "decline" });
                free(j.fence); g_reach_last.with(|c| c.set(LOGOS_REACH_DECLINED_GATE)); return 0;
            }
        }
    }
    let meta = malloc(4 * (K as usize + 2) * size_of::<usize>()) as *mut usize; let wa = calloc(P as usize, size_of::<reach_arg_t>()) as *mut reach_arg_t;
    let ctxs = calloc(P as usize, size_of::<ctx_t>()) as *mut ctx_t; let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut();
    let pooled = pool_acquire(P as usize * maxlen * size_of::<f64>(), 0, &mut pb, &mut pbi);
    let scr = if pooled != 0 { pb as *mut f64 } else { malloc(P as usize * maxlen * size_of::<f64>()) as *mut f64 };
    let ctx_mark = g_ctx_next;
    let mut nctx: i32 = 0; let mut sorted: i32 = 0;
    g_reach_last.with(|c| c.set(LOGOS_REACH_DECLINED_NOMEM));
    while nctx < P { if ctx_init(ctxs.add(nctx as usize), maxlen, 0) == 0 { break; } nctx += 1; }
    if nctx == P {
        j.lo = meta; j.hi = meta.add(K as usize + 2); j.x = meta.add(2 * (K as usize + 2)); j.y = meta.add(3 * (K as usize + 2));
        let mut bar = MaybeUninit::<barrier_t>::uninit(); if P > 1 { barrier_init(bar.as_mut_ptr(), P); j.bar = bar.as_mut_ptr(); }
        for t in 0..P as usize { let w = wa.add(t); (*w).j = &mut j; (*w).ctx = ctxs.add(t); (*w).scratch = scr.add(t * maxlen); (*w).tid = t as i32; (*ctxs.add(t)).seq = (P == 1) as i32; }
        TRACE!("reach: reach {} -> {} blocks of {}, {} threads", reach, K, B, P);
        /* queue-driven phases that meet at a barrier */
        let made = par_spawn(th, P, reach_worker, wa as *mut c_void, size_of::<reach_arg_t>(), j.bar, null_mut());
        reach_blocks(wa);
        if !j.bar.is_null() { barrier_wait(j.bar); }
        TRACE!("reach: {} blocks sorted, {} held strays", K, j.nstrayblk.load(SeqCst));
        reach_plan(&mut j);
        g_reach_last.with(|c| c.set(if j.go != 0 { if j.go == 2 { LOGOS_REACH_SORTED_STRAYS } else { LOGOS_REACH_SORTED } } else { LOGOS_REACH_DECLINED_PLAN }));
        let mut sg: *mut reach_seg_t = null_mut(); let mut tpos: *mut usize = null_mut();
        if j.go == 2 {                                                 /* set the strays aside and sort them; everything their return needs is taken NOW */
            j.T = malloc(j.S * size_of::<f64>()) as *mut f64; tpos = malloc(j.S * size_of::<usize>()) as *mut usize; sg = malloc((K as usize + j.S + 1) * size_of::<reach_seg_t>()) as *mut reach_seg_t;
            {
                let mut w: usize = 0;
                for k in 0..K { let s = reach_s(&j, k); let e = reach_e(&j, k); let lo = *j.lo.add(k as usize); let hi = *j.hi.add(k as usize);
                    memcpy(j.T.add(w), a.add(s), (lo - s) * size_of::<f64>()); w += lo - s; memcpy(j.T.add(w), a.add(hi), (e - hi) * size_of::<f64>()); w += e - hi; }
                ns_depth.with(|d| d.set(d.get() + 1)); logos_sort_f64(j.T, j.S, if j.S >= NS_STRAYS_PARALLEL { P } else { 1 }); ns_depth.with(|d| d.set(d.get() - 1));
                TRACE!("reach: {} strays set aside and sorted", j.S);
            }
        }
        if !j.bar.is_null() { barrier_wait(j.bar); }                    /* release the workers into the seams, or home */
        if j.go != 0 { reach_seams(wa); }
        par_join(th, made);
        if !j.bar.is_null() { barrier_destroy(bar.as_mut_ptr()); }
        if j.go == 2 { reach_place_strays(&mut j, sg, tpos); }
        sorted = (j.go != 0) as i32;
        TRACE!("reach: {} ({} strays)", if sorted != 0 { "done" } else { "declined after the block phase" }, j.S);
        free(j.T); free(tpos); free(sg);
    }
    for t in 0..nctx { ctx_free(ctxs.add(t as usize)); }
    g_ctx_next = ctx_mark;                                             /* the bundles are free again */
    if pooled != 0 { pool_release(); } else { free(scr); }
    free(meta); free(wa); free(ctxs); free(th); free(j.fence);
    sorted
}

// ---- sample once, decide once (C 7347-7393) ------------------------------------------------------------------------------
pub(crate) const SODO_GATE_M: usize = 2048;
pub(crate) unsafe fn sodo_sample(a: *const f64, n: usize, mut mfull: usize, S: *mut sodo_t) -> i32 {
    *S = core::mem::zeroed(); if mfull > n { mfull = n; } let m = if mfull < SODO_GATE_M { mfull } else { SODO_GATE_M }; (*S).m = m; (*S).mfull = mfull;
    let raw = malloc(m * size_of::<f64>()) as *mut f64; (*S).sorted = malloc(mfull * size_of::<f64>()) as *mut f64;
    sample_read(a, n, 0, m, mfull, raw, &mut (*S).has_nan);
    /* inversion rate over Fibonacci-lag pairs, wrapped pairs skipped; inv_all reads EVERY pair of the lag, in position order */
    { const L: usize = 144; let mut inv: usize = 0; let mut comps: usize = 0; let mut inva: usize = 0; let mut compsa: usize = 0;
      let mut i = 0usize;
      while i + L < m { let p0 = SAMPLE_POS(i, mfull, n); let p1 = SAMPLE_POS(i + L, mfull, n); if p1 == p0 { i += 1; continue; }
          compsa += 1; if if p1 > p0 { *raw.add(i) > *raw.add(i + L) } else { *raw.add(i + L) > *raw.add(i) } { inva += 1; }
          if p1 > p0 { comps += 1; if *raw.add(i) > *raw.add(i + L) { inv += 1; } }
          i += 1; }
      (*S).inv_rate = if comps != 0 { inv as f64 / comps as f64 } else { 1.0 }; (*S).inv_all = if compsa != 0 { inva as f64 / compsa as f64 } else { 1.0 }; }
    if n >= REACH_MIN_N && (*S).has_nan == 0 && (*S).inv_all <= REACH_TAU { (*S).reach = reach_from_sample(a, n, raw, m, 0); }   /* random data stops at the first rung, already read */
    memcpy((*S).sorted, raw, m * size_of::<f64>()); free(raw);
    if sample_sort((*S).sorted, m) != 0 { (*S).has_nan = 1; }
    let s = (*S).sorted; let mut d: usize = 0; let mut f1: usize = 0; let mut f2: usize = 0;
    let mut i = 0usize;
    while i < m { let mut j = i + 1; while j < m && *s.add(j) == *s.add(i) { j += 1; } d += 1; if j - i == 1 { f1 += 1; } else if j - i == 2 { f2 += 1; } i = j; }
    (*S).distinct = d; (*S).f1 = f1; (*S).f2 = f2; (*S).chao1 = d as f64 + f1 as f64 * (if f1 > 0 { f1 - 1 } else { 0 }) as f64 / (2.0 * (f2 + 1) as f64);
    (*S).ks = 1.0;
    1
}
/* stage 2 (partition only): read the remaining points, sort them, merge with stage 1; the KS distance comes with them */
pub(crate) unsafe fn sodo_extend(a: *const f64, n: usize, S: *mut sodo_t) -> i32 {
    let m = (*S).m; let mfull = (*S).mfull; if mfull <= m { return 1; }
    let k = mfull - m; let ext = malloc(k * size_of::<f64>()) as *mut f64;
    let mut nan2: i32 = 0; sample_read(a, n, m, mfull, mfull, ext, &mut nan2); if nan2 != 0 { (*S).has_nan = 1; }
    if sample_sort(ext, k) != 0 { (*S).has_nan = 1; }
    let s = (*S).sorted; let mut i = m; let mut j = k; let mut w = mfull;              /* merge from the back, in place */
    while j > 0 { if i > 0 && *s.add(i - 1) > *ext.add(j - 1) { w -= 1; i -= 1; *s.add(w) = *s.add(i); } else { w -= 1; j -= 1; *s.add(w) = *ext.add(j); } }
    free(ext); (*S).m = mfull;
    if mfull >= 256 && *s.add(mfull - 1) > *s {
        let mut D = 0.0f64; let inv = 1.0 / (*s.add(mfull - 1) - *s);
        for q in 0..mfull { let F = (*s.add(q) - *s) * inv; let mut e1 = F - q as f64 / mfull as f64; let mut e2 = (q + 1) as f64 / mfull as f64 - F; if e1 < 0.0 { e1 = -e1; } if e2 < 0.0 { e2 = -e2; } if e1 > D { D = e1; } if e2 > D { D = e2; } }
        (*S).ks = D;
    }
    1
}

// ---- the head of the dense-integer section (C 7416-7429: definitions that fall in this file's range) -------------------

// ---- sorted prefix + tail, head + sorted suffix (C 7626-7819) --------------------------------------------------------------
pub(crate) static mut g_tail_kind: i32 = 0;      /* logos_twist_last(-11): the last sequential tail merge's form, 1 loop, 2 gallop, 3 vector */
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct tail_scan_t { pub(crate) a: *const f64, pub(crate) buf: *mut f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) bad: i32 }
pub(crate) unsafe fn tail_scan(p: *mut c_void) -> *mut c_void {
    let w = p as *mut tail_scan_t; let mut bad: i32 = 0;
    for i in (*w).lo..(*w).hi { let x = *(*w).a.add(i); bad |= (x != x) as i32; *(*w).buf.add(i) = x; }
    (*w).bad = bad;
    null_mut()
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct tail_seg_t { pub(crate) pre: *const f64, pub(crate) tail: *const f64, pub(crate) i0: usize, pub(crate) i1: usize, pub(crate) j0: usize, pub(crate) j1: usize, pub(crate) out: *mut f64 }
pub(crate) unsafe fn tail_merge_seg(p: *mut c_void) -> *mut c_void {
    let w = p as *mut tail_seg_t; let mut i = (*w).i0; let mut j = (*w).j0; let mut o = (*w).out;
    while i < (*w).i1 && j < (*w).j1 { let x = *(*w).pre.add(i); let y = *(*w).tail.add(j); let tk = (y < x) as i32; *o = if tk != 0 { y } else { x }; o = o.add(1); j += tk as usize; i += (tk == 0) as usize; }
    if i < (*w).i1 { memcpy(o, (*w).pre.add(i), ((*w).i1 - i) * size_of::<f64>()); o = o.add((*w).i1 - i); }
    if j < (*w).j1 { memcpy(o, (*w).tail.add(j), ((*w).j1 - j) * size_of::<f64>()); }
    null_mut()
}
#[inline]
pub(crate) unsafe fn tail_lb(v: *const f64, len: usize, key: f64) -> usize {
    let mut lo: usize = 0; let mut hi = len;
    while lo < hi { let mid = lo + (hi - lo) / 2; if *v.add(mid) < key { lo = mid + 1 } else { hi = mid } }
    lo
}
pub(crate) unsafe fn tail_merge_par_f64(a: *mut f64, n: usize, d: usize, P: i32) -> i32 {
    let tl = n - d;
    let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut(); let pooled = pool_acquire(n * size_of::<f64>(), 0, &mut pb, &mut pbi);
    let buf = if pooled != 0 { pb as *mut f64 } else { big_alloc(n * size_of::<f64>()) as *mut f64 };
    let sc = malloc(P as usize * size_of::<tail_scan_t>()) as *mut tail_scan_t; let sg = malloc(P as usize * size_of::<tail_seg_t>()) as *mut tail_seg_t;
    let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let mut ok: i32 = 1;
    if ok != 0 {
        for t in 0..P as usize { let s = sc.add(t); (*s).a = a; (*s).buf = buf; (*s).lo = n * t / P as usize; (*s).hi = n * (t + 1) / P as usize; (*s).bad = 0; }
        par_run(th, P, tail_scan, sc as *mut c_void, size_of::<tail_scan_t>(), 1);
        for t in 0..P as usize { if (*sc.add(t)).bad != 0 { ok = 0; } }
    }
    if ok != 0 {
        psort_f64(buf.add(d), tl, P, null_mut(), null_mut(), null_mut(), null_mut());        /* the tail may be anything: full dispatcher */
        let pre = buf as *const f64; let tail = buf.add(d) as *const f64;
        let mut j0: usize = 0;
        for t in 0..P as usize {
            let i0 = d * t / P as usize; let i1 = d * (t + 1) / P as usize;
            let mut j1 = if t == P as usize - 1 { tl } else { tail_lb(tail, tl, *pre.add(i1)) };
            if j1 < j0 { j1 = j0; }
            let g = sg.add(t);
            (*g).pre = pre; (*g).tail = tail; (*g).i0 = i0; (*g).i1 = i1; (*g).j0 = j0; (*g).j1 = j1; (*g).out = a.add(i0 + j0);
            j0 = j1;
        }
        par_run(th, P, tail_merge_seg, sg as *mut c_void, size_of::<tail_seg_t>(), 1);
    }
    if pooled != 0 { pool_release(); } else { free(buf); }
    free(sc); free(sg); free(th);
    ok
}
/* THE SEQUENTIAL MERGE, three forms by the gap g = d / tl (prefix elements per tail element) */
pub(crate) const TAIL_GALLOP_RATIO: f64 = 256.0;
pub(crate) const TAIL_VEC_RATIO: f64 = 3.0;
pub(crate) unsafe fn tail_merge_try_f64(a: *mut f64, n: usize, d: usize, P: i32) -> i32 {
    /* LOGOS_NO_TAIL unset */
    if P > 1 && n >= MIN_PARALLEL { return tail_merge_par_f64(a, n, d, P); }
    let mut bad: i32 = 0;
    for i in 0..n { bad |= (*a.add(i) != *a.add(i)) as i32; }
    if bad != 0 { return 0; }
    let branchy: i32 = 0;                                       /* LOGOS_TAIL_BRANCHY unset */
    let ggal: f64 = TAIL_GALLOP_RATIO; let gvec: f64 = TAIL_VEC_RATIO;   /* LOGOS_TAIL_GALLOP, LOGOS_TAIL_VEC unset */
    let tl = n - d;
    psort_f64(a.add(d), tl, P, null_mut(), null_mut(), null_mut(), null_mut());            /* the tail may be anything: full dispatcher, in place */
    if branchy == 0 && g_simd_merge != 0 && d as f64 <= gvec * tl as f64 {
        let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut(); let pooled = pool_acquire(n * size_of::<f64>(), 0, &mut pb, &mut pbi);
        let o = if pooled != 0 { pb as *mut f64 } else { malloc(n * size_of::<f64>()) as *mut f64 };
        {
            if !(half_on(n) != 0 && half_two(a, n, d, o) != 0) {              /* half the scratch (half_place); the full one below HALF_MIN_N */
                g_simd_merges.fetch_add(1, Relaxed);                           /* counted with the knot route's: the same kernel */
                knot_merge2v_uu(a, d, a.add(d), tl, o);
                memcpy(a, o, n * size_of::<f64>());
            }
            if pooled != 0 { pool_release(); } else { free(o); }
            g_tail_kind = 3;
            return 1;
        }
    }
    let buf = malloc(tl * size_of::<f64>()) as *mut f64;
    memcpy(buf, a.add(d), tl * size_of::<f64>());
    /* Backward merge. w == i + j throughout, so a write at w-1 never lands on an unread prefix element. */
    let mut i = d; let mut j = tl; let mut w = n;
    if branchy == 0 && d as f64 >= ggal * tl as f64 {
        while j > 0 && i > 0 {
            let y = *buf.add(j - 1);
            if !(*a.add(i - 1) > y) { w -= 1; *a.add(w) = y; j -= 1; continue; }
            let mut hi = i - 1; let mut step: usize = 1; let mut lo: usize;      /* a[hi] > y; find the first index p with a[p] > y */
            loop { if step > hi { lo = 0; break; } if !(*a.add(hi - step) > y) { lo = hi - step + 1; break; } hi -= step; step <<= 1; }
            while lo < hi { let mid = lo + ((hi - lo) >> 1); if *a.add(mid) > y { hi = mid } else { lo = mid + 1 } }
            let p = lo;                                            /* a[p .. i) > y, a[p - 1] <= y (or p == 0); ties stay prefix-first */
            memmove(a.add(p + j), a.add(p), (i - p) * size_of::<f64>());
            i = p; w = i + j; w -= 1; *a.add(w) = y; j -= 1;
        }
        if j > 0 { memcpy(a, buf, j * size_of::<f64>()); }
        g_tail_kind = 2;
    } else {
        while j > 0 {
            if i > 0 && *a.add(i - 1) > *buf.add(j - 1) { w -= 1; i -= 1; *a.add(w) = *a.add(i); }
            else                                         { w -= 1; j -= 1; *a.add(w) = *buf.add(j); }
        }
        g_tail_kind = 1;
    }
    free(buf);
    1
}
/* an unsorted head before a sorted suffix: the tail route's mirror */
pub(crate) static mut g_head_kind: i32 = 0;
#[inline(never)]
pub(crate) unsafe fn head_merge_try_f64(a: *mut f64, n: usize, P: i32) -> i32 {
    let mut s = n - 1;
    while s > 0 && *a.add(s - 1) <= *a.add(s) { s -= 1; }               /* [s, n) ascending; random data stops at once */
    if n - s < n / 2 || s == 0 { return 0; }                             /* LOGOS_NO_HEAD unset */
    let mut bad: i32 = 0; for i in 0..n { bad |= (*a.add(i) != *a.add(i)) as i32; }
    if bad != 0 { return 0; }
    let hs = s; let sl = n - s;
    let gvec: f64 = TAIL_VEC_RATIO; let ggal: f64 = TAIL_GALLOP_RATIO;   /* LOGOS_TAIL_GALLOP, LOGOS_TAIL_VEC unset */
    psort_f64(a, hs, P, null_mut(), null_mut(), null_mut(), null_mut());                  /* the head may be anything: full dispatcher, in place */
    if g_simd_merge != 0 && sl as f64 <= gvec * hs as f64 {
        let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut(); let pooled = pool_acquire(n * size_of::<f64>(), 0, &mut pb, &mut pbi);
        let o = if pooled != 0 { pb as *mut f64 } else { malloc(n * size_of::<f64>()) as *mut f64 };
        {
            if !(half_on(n) != 0 && half_two(a, n, hs, o) != 0) {             /* half the scratch, as the tail route */
                g_simd_merges.fetch_add(1, Relaxed);
                knot_merge2v_uu(a, hs, a.add(hs), sl, o);
                memcpy(a, o, n * size_of::<f64>());
            }
            if pooled != 0 { pool_release(); } else { free(o); }
            g_head_kind = 3;
            return 1;
        }
    }
    let buf = malloc(hs * size_of::<f64>()) as *mut f64;
    memcpy(buf, a, hs * size_of::<f64>());
    /* Forward merge. w == i + (j - hs) throughout, so a write at w never lands on an unread suffix element. */
    let mut i: usize = 0; let mut j = hs; let mut w: usize = 0;
    if sl as f64 >= ggal * hs as f64 {
        while i < hs && j < n {
            let y = *buf.add(i);
            if !(*a.add(j) < y) { *a.add(w) = y; w += 1; i += 1; continue; }
            let mut lo = j + 1; let mut step: usize = 1; let mut hi: usize;      /* a[j] < y; find the first index p >= j with a[p] >= y */
            loop { if lo + step > n { hi = n; break; } if !(*a.add(lo + step - 1) < y) { hi = lo + step - 1; break; } lo += step; step <<= 1; }
            while lo < hi { let mid = lo + ((hi - lo) >> 1); if *a.add(mid) < y { lo = mid + 1 } else { hi = mid } }
            let p = lo;                                            /* a[j .. p) < y <= a[p] */
            memmove(a.add(w), a.add(j), (p - j) * size_of::<f64>());
            w += p - j; j = p; *a.add(w) = y; w += 1; i += 1;
        }
        if i < hs { memcpy(a.add(w), buf.add(i), (hs - i) * size_of::<f64>()); }
        g_head_kind = 2;
    } else {
        while i < hs {
            if j < n && *a.add(j) < *buf.add(i) { *a.add(w) = *a.add(j); w += 1; j += 1; }
            else                                { *a.add(w) = *buf.add(i); w += 1; i += 1; }
        }
        g_head_kind = 1;
    }
    free(buf);
    1
}

// ---- the head of the dominant-value section (C 7822-7845: definitions that fall in this file's range) -------------------

// ---- calls outside this file's range: C signatures, minimal REAL bodies (serial workers, the standard sort) so that
// ---- this file's tests run alone. They vanish under `port_all`. ------------------------------------------------------------
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    pub(crate) type pthread_t = usize;
    pub(crate) struct barrier_t { pub(crate) count: AtomicI32, pub(crate) arrived: AtomicI32, pub(crate) gen: AtomicI32 }
    pub(crate) static mut g_ctx_next: i32 = 0;
    pub(crate) static mut g_simd_merge: i32 = 0;
    pub(crate) static g_simd_merges: AtomicUsize = AtomicUsize::new(0);

    unsafe fn std_sort(a: *mut f64, n: usize) { if n >= 2 { core::slice::from_raw_parts_mut(a, n).sort_unstable_by(|x, y| x.total_cmp(y)); } }
    pub(crate) unsafe fn psort_f64(a: *mut f64, n: usize, P: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) { std_sort(a, n) }
    pub(crate) unsafe fn logos_sort_f64(a: *mut f64, n: usize, nthreads: i32) { std_sort(a, n) }
    pub(crate) unsafe fn leaf_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 { std_sort(a, n); a }
    pub(crate) unsafe fn sort_fn(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 { std_sort(a, n); a }
    pub(crate) unsafe fn avx2_leaf_on() -> i32 { 0 }
    pub(crate) unsafe fn avx2_qsort_f64(a: *mut f64, n: usize) -> i32 { std_sort(a, n); 1 }
    pub(crate) unsafe fn quicksort(a: *mut f64, lo: i64, hi: i64, d: i32) { if hi > lo { std_sort(a.add(lo as usize), (hi - lo + 1) as usize) } }
    pub(crate) unsafe fn log2i(mut n: usize) -> i32 { let mut l = 0; loop { n >>= 1; if n == 0 { break } l += 1 } l }
    pub(crate) unsafe fn insertion_sort(a: *mut f64, lo: i64, hi: i64) {
        let mut i = lo + 1;
        while i <= hi { let v = *a.offset(i as isize); let mut j = i - 1; while j >= lo && *a.offset(j as isize) > v { *a.offset(j as isize + 1) = *a.offset(j as isize); j -= 1; } *a.offset(j as isize + 1) = v; i += 1; }
    }
    /// gives up past 8n moves, leaving a permutation
    pub(crate) unsafe fn insertion_sort_momentum(a: *mut f64, lo: i64, hi: i64) -> i32 {
        let budget = 8 * (hi - lo + 1); let mut moves: i64 = 0;
        let mut i = lo + 1;
        while i <= hi {
            let v = *a.offset(i as isize); let mut j = i - 1;
            while j >= lo && *a.offset(j as isize) > v { *a.offset(j as isize + 1) = *a.offset(j as isize); j -= 1; moves += 1; }
            *a.offset(j as isize + 1) = v;
            if moves > budget { return 0 }
            i += 1;
        }
        1
    }
    pub(crate) unsafe fn sample_read(a: *const f64, n: usize, from: usize, to: usize, mfull: usize, out: *mut f64, has_nan: *mut i32) {
        for i in from..to { let q = SAMPLE_POS(i, mfull, n); let v = *a.add(if q < n { q } else { n - 1 }); *out.add(i - from) = v; if v != v { *has_nan = 1; } }
    }
    pub(crate) unsafe fn sample_sort(s: *mut f64, m: usize) -> usize {
        let mut k = m; let mut i = 0usize;
        while i < k { if *s.add(i) != *s.add(i) { let v = *s.add(i); k -= 1; *s.add(i) = *s.add(k); *s.add(k) = v; } else { i += 1; } }
        std_sort(s, k);
        m - k
    }
    pub(crate) unsafe fn sample_by_pos(a: *const f64, n: usize, m: usize) -> *mut psamp_t {
        let s = malloc(m * size_of::<psamp_t>()) as *mut psamp_t;
        let mut nan: i32 = 0;
        for i in 0..m { let p = SAMPLE_POS(i, m, n); let v = *a.add(p); if v != v { nan = 1; } *s.add(i) = psamp_t { pos: p, v }; }
        if nan != 0 { free(s); return null_mut(); }
        core::slice::from_raw_parts_mut(s, m).sort_by_key(|x| x.pos);
        s
    }
    pub(crate) unsafe fn ctx_init(c: *mut ctx_t, cap: usize, wc_on: i32) -> i32 {
        *c = core::mem::zeroed(); (*c).cap = cap; (*c).aux = malloc((cap + 1) * size_of::<u32>()) as *mut u32; (*c).pool_ix = -1; 1
    }
    pub(crate) unsafe fn ctx_free(c: *mut ctx_t) { free((*c).aux); (*c).aux = null_mut(); }
    pub(crate) unsafe fn ctx_fit(c: *mut ctx_t, nb: usize, f: *mut ctx_fit_t) -> i32 {
        (*f).kind = 0;
        if nb > (*c).cap { (*f).aux = (*c).aux; (*f).cap = (*c).cap; (*f).kind = 2; (*c).aux = malloc((nb + 1) * size_of::<u32>()) as *mut u32; (*c).cap = nb; }
        1
    }
    pub(crate) unsafe fn ctx_unfit(c: *mut ctx_t, f: *mut ctx_fit_t) { if (*f).kind == 2 { free((*c).aux); (*c).aux = (*f).aux; (*c).cap = (*f).cap; (*f).kind = 0; } }
    /// serial: every worker on the calling thread
    pub(crate) unsafe fn par_run(th: *mut pthread_t, P: i32, f: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, run0: i32) {
        if run0 != 0 { f(args); }
        for t in 1..P as usize { f((args as *mut u8).add(t * stride) as *mut c_void); }
    }
    /// no worker gets a thread: each is dropped from the barrier, as the C does for a thread it could not create, and the caller drains the queues
    pub(crate) unsafe fn par_spawn(th: *mut pthread_t, P: i32, f: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, bar: *mut barrier_t, orphan: *mut u8) -> i32 {
        for t in 1..P as usize { if !bar.is_null() { barrier_drop(bar); } if !orphan.is_null() { *orphan.add(t) = 1; } }
        0
    }
    pub(crate) unsafe fn par_join(th: *mut pthread_t, made: i32) {}
    pub(crate) unsafe fn barrier_init(b: *mut barrier_t, n: i32) { core::ptr::write(b, barrier_t { count: AtomicI32::new(n), arrived: AtomicI32::new(0), gen: AtomicI32::new(0) }); }
    pub(crate) unsafe fn barrier_drop(b: *mut barrier_t) { (*b).count.fetch_sub(1, Release); }
    pub(crate) unsafe fn barrier_wait(b: *mut barrier_t) { assert_eq!((*b).count.load(SeqCst), 1, "the stub barrier has one participant"); (*b).gen.fetch_add(1, SeqCst); }
    pub(crate) unsafe fn barrier_destroy(b: *mut barrier_t) {}
    pub(crate) unsafe fn pool_acquire(buf_bytes: usize, bidx_bytes: usize, buf: *mut *mut c_void, bidx: *mut *mut u8) -> i32 { 0 }
    pub(crate) unsafe fn pool_release() {}
    pub(crate) unsafe fn big_alloc(bytes: usize) -> *mut c_void { malloc(bytes) as *mut c_void }
    pub(crate) unsafe fn scratch_get(s: *mut scratch_t) -> *mut f64 {
        if (*s).have != 0 { return (*s).dst; }
        (*s).dst = malloc((*s).n * size_of::<f64>()) as *mut f64; (*s).have = 1; (*s).dst
    }
    pub(crate) unsafe fn half_on(n: usize) -> i32 { 0 }
    pub(crate) unsafe fn half_two(a: *mut f64, n: usize, d: usize, buf: *mut f64) -> i32 { 0 }
    /// two ascending strands into o (scalar here)
    pub(crate) unsafe fn knot_merge2v_uu(x: *const f64, nx: usize, y: *const f64, ny: usize, o: *mut f64) {
        let (mut i, mut j, mut w) = (0usize, 0usize, 0usize);
        while i < nx && j < ny { if *y.add(j) < *x.add(i) { *o.add(w) = *y.add(j); j += 1; } else { *o.add(w) = *x.add(i); i += 1; } w += 1; }
        while i < nx { *o.add(w) = *x.add(i); i += 1; w += 1; }
        while j < ny { *o.add(w) = *y.add(j); j += 1; w += 1; }
    }
}
#[cfg(not(feature = "port_all"))]
use missing::*;

#[cfg(all(test, not(feature = "port_all")))]
mod tests {
    use super::*;
    use std::sync::Mutex;
    static LOCK: Mutex<()> = Mutex::new(());            // the routes write file-scope statics: one test at a time
    fn lock() -> std::sync::MutexGuard<'static, ()> { LOCK.lock().unwrap_or_else(|e| e.into_inner()) }

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn f(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
        fn below(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
    }
    fn bits(v: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = v.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    fn ascending(v: &[f64]) -> bool { v.windows(2).all(|w| !(w[1] < w[0])) }
    fn uniform(n: usize, seed: u64) -> Vec<f64> { let mut r = Rng(seed); (0..n).map(|_| r.f() * 1e6).collect() }
    fn sorted_swaps(n: usize, share: f64, seed: u64) -> Vec<f64> {
        let mut r = Rng(seed); let mut v: Vec<f64> = (0..n).map(|i| i as f64 + 0.5).collect();
        for _ in 0..(n as f64 * share) as usize { let (i, j) = (r.below(n), r.below(n)); v.swap(i, j); }
        v
    }
    fn jitter(n: usize, seed: u64) -> Vec<f64> { let mut r = Rng(seed); (0..n).map(|i| i as f64 + (r.f() - 0.5) * 100.0).collect() }

    // ---- the shared sample ------------------------------------------------------------------------------------------------
    fn direct_counts(a: &[f64], mfull: usize) -> (usize, usize, usize) {
        let n = a.len(); let mfull = mfull.min(n); let m = mfull.min(SODO_GATE_M);
        let mut p: Vec<f64> = (0..m).map(|i| a[SAMPLE_POS(i, mfull, n)]).collect();
        p.sort_by(|x, y| x.partial_cmp(y).unwrap());
        let (mut d, mut f1, mut f2) = (0, 0, 0); let mut i = 0;
        while i < m { let mut j = i + 1; while j < m && p[j] == p[i] { j += 1; } d += 1; if j - i == 1 { f1 += 1 } else if j - i == 2 { f2 += 1 } i = j; }
        (d, f1, f2)
    }
    fn sodo_of(a: &[f64], mfull: usize) -> sodo_t {
        unsafe {
            let mut S: sodo_t = core::mem::zeroed();
            assert_eq!(sodo_sample(a.as_ptr(), a.len(), mfull, &mut S), 1);
            let (d, f1, f2) = direct_counts(a, mfull);
            assert_eq!((S.distinct, S.f1, S.f2), (d, f1, f2));
            let chao = d as f64 + f1 as f64 * (if f1 > 0 { f1 - 1 } else { 0 }) as f64 / (2.0 * (f2 + 1) as f64);
            assert_eq!(S.chao1, chao);
            assert_eq!(S.m, mfull.min(a.len()).min(SODO_GATE_M)); assert_eq!(S.has_nan, 0); assert_eq!(S.ks, 1.0);
            assert!(ascending(core::slice::from_raw_parts(S.sorted, S.m)));
            S
        }
    }
    #[test]
    fn sodo_statistics() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 1 << 20;
        unsafe {
            // uniform: every point distinct, half the pairs inverted, no reach; the extension gives a small KS distance
            let a = uniform(n, 11);
            let mut S = sodo_of(&a, 5376);
            assert_eq!(S.distinct, 2048); assert!(S.inv_all > 0.4 && S.inv_all < 0.6, "inv_all {}", S.inv_all); assert_eq!(S.reach, 0);
            assert_eq!(sodo_extend(a.as_ptr(), n, &mut S), 1);
            assert_eq!(S.m, 5376);
            let want: Vec<f64> = (0..5376).map(|i| a[SAMPLE_POS(i, 5376, n)]).collect();
            let got = core::slice::from_raw_parts(S.sorted, 5376);
            assert!(ascending(got)); assert_eq!(bits(got), bits(&want)); assert!(S.ks < 0.05, "ks {}", S.ks);
            free(S.sorted);
            // sorted: nothing inverted; sorted with 1% swaps: nearly nothing
            let a: Vec<f64> = (0..n).map(|i| i as f64).collect();
            let S = sodo_of(&a, 5376); assert_eq!(S.inv_all, 0.0); free(S.sorted);
            let a = sorted_swaps(n, 0.01, 12);
            let S = sodo_of(&a, 5376); assert!(S.inv_all < 0.08, "inv_all {}", S.inv_all); free(S.sorted);
            // five values
            let mut r = Rng(13); let a: Vec<f64> = (0..n).map(|_| r.below(5) as f64).collect();
            let S = sodo_of(&a, 2048); assert_eq!(S.distinct, 5); assert_eq!(S.f1, 0); assert_eq!(S.chao1, 5.0); free(S.sorted);
            // periodic: a sawtooth of period 1000
            let a: Vec<f64> = (0..n).map(|i| (i % 1000) as f64).collect();
            let S = sodo_of(&a, 384); assert!(S.distinct <= 384); assert_eq!(S.m, 384); free(S.sorted);
            let S = sodo_of(&a, 5376); assert!(S.distinct <= 1000); free(S.sorted);
            // a NaN at a sample point
            let mut a = uniform(n, 14); a[SAMPLE_POS(7, 2048, n)] = f64::NAN;
            let mut S: sodo_t = core::mem::zeroed(); sodo_sample(a.as_ptr(), n, 2048, &mut S); assert_eq!(S.has_nan, 1); free(S.sorted);
            // latency jitter: the ladder finds a short reach
            let a = jitter(2_000_000, 15);
            let S = sodo_of(&a, 5376); assert!(S.reach > 0 && S.reach < 8192, "reach {}", S.reach); free(S.sorted);
        }
    }

    // ---- the near-sorted route --------------------------------------------------------------------------------------------
    unsafe fn run_ns(v: &mut [f64], P: i32, inv_rate: f64) -> i32 {
        let mut lz: scratch_t = core::mem::zeroed(); lz.n = v.len();
        let r = nearsorted_parallel_f64(v.as_mut_ptr(), v.len(), P, null_mut(), &mut lz, inv_rate);
        free(lz.dst);
        r
    }
    #[test]
    fn nearsorted_takes_swaps() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let base = sorted_swaps(2_000_000, 0.01, 21); let want = bits(&base);
        for &P in &[1, 4, 14] {
            let mut v = base.clone();
            let r = unsafe { run_ns(&mut v, P, 0.02) };
            if P == 1 { assert_eq!(r, 0); assert!(v.iter().zip(&base).all(|(x, y)| x.to_bits() == y.to_bits())); }   // P < 2: not this route's
            else { assert_eq!(r, 1, "P {}", P); assert!(ascending(&v)); assert_eq!(bits(&v), want); }
        }
        // duplicates, signed zeros and infinities among the strays
        let mut b = sorted_swaps(300_000, 0.005, 22);
        for i in (0..b.len()).step_by(7) { b[i] = (b[i] / 16.0).floor() * 16.0; }
        b.sort_by(|x, y| x.partial_cmp(y).unwrap());
        b[0] = f64::NEG_INFINITY; b[1] = -0.0; b[2] = 0.0; b[3] = -0.0; let l = b.len(); b[l - 1] = f64::INFINITY; b[l - 2] = f64::INFINITY;
        let mut r = Rng(23); for _ in 0..1500 { let (i, j) = (r.below(l), r.below(l)); b.swap(i, j); }
        let want = bits(&b);
        for &P in &[2, 4, 14] { let mut v = b.clone(); assert_eq!(unsafe { run_ns(&mut v, P, 0.01) }, 1); assert!(ascending(&v)); assert_eq!(bits(&v), want); }
    }
    #[test]
    fn nearsorted_declines() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let base = uniform(2_000_000, 24);
        for &P in &[1, 4, 14] {
            for &ir in &[-1.0, 0.0] {                                  // its own 40-point gate, then the window pre-gate
                let mut v = base.clone();
                assert_eq!(unsafe { run_ns(&mut v, P, ir) }, 0);
                assert!(v.iter().zip(&base).all(|(x, y)| x.to_bits() == y.to_bits()), "a[] must be untouched");
            }
        }
        // strands, not strays: 64 sorted runs in order, each overlapping the next -> the strand gate, counted
        let n = 2_000_000usize; let l = n / 64;
        let base: Vec<f64> = (0..n).map(|i| (i / l) as f64 + 1.2 * (i % l) as f64 / l as f64).collect();
        let before = g_ns_strand.load(SeqCst);
        for &P in &[4, 14] { let mut v = base.clone(); assert_eq!(unsafe { run_ns(&mut v, P, 0.0) }, 0); assert!(v == base); }
        assert_eq!(g_ns_strand.load(SeqCst), before + 2);
    }

    // ---- the short-reach route --------------------------------------------------------------------------------------------
    #[test]
    fn reach_takes_jitter() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let base = jitter(2_000_000, 31); let want = bits(&base);
        let reach = unsafe { let mut S: sodo_t = core::mem::zeroed(); sodo_sample(base.as_ptr(), base.len(), 5376, &mut S); free(S.sorted); S.reach };
        assert!(reach > 0);
        for &P in &[1, 4, 14] {
            let mut v = base.clone();
            let r = unsafe { reach_try_f64(v.as_mut_ptr(), v.len(), P, reach) };
            assert_eq!(r, 1, "P {}", P); assert_eq!(g_reach_last.with(|c| c.get()), LOGOS_REACH_SORTED);
            assert!(ascending(&v)); assert_eq!(bits(&v), want);
        }
        // a few far strays (zeros and late events), duplicates, signed zeros, infinities: put back, up to four threads
        let mut b = base.clone(); let n = b.len(); let mut r = Rng(32);
        for _ in 0..200 { let i = r.below(n); b[i] = if r.next() & 1 == 0 { 0.0 } else { -0.0 }; }
        for _ in 0..200 { let (i, j) = (r.below(n), r.below(n)); b[i] = b[j]; }
        b[n / 3] = f64::INFINITY; b[n / 2] = f64::NEG_INFINITY; b[n / 7] = f64::INFINITY;
        let want = bits(&b);
        for &P in &[1, 4] {
            let mut v = b.clone();
            let r = unsafe { reach_try_f64(v.as_mut_ptr(), n, P, reach) };
            assert_eq!(r, 1, "P {}", P); assert_eq!(g_reach_last.with(|c| c.get()), LOGOS_REACH_SORTED_STRAYS);
            assert!(ascending(&v)); assert_eq!(bits(&v), want);
        }
        { let mut v = b.clone(); let r = unsafe { reach_try_f64(v.as_mut_ptr(), n, 14, reach) };       // wider: sorted or declined, a permutation either way
          assert_eq!(bits(&v), want); if r == 1 { assert!(ascending(&v)); } }
    }
    #[test]
    fn reach_declines() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let base = uniform(2_000_000, 33); let want = bits(&base);
        let reach = unsafe { let mut S: sodo_t = core::mem::zeroed(); sodo_sample(base.as_ptr(), base.len(), 5376, &mut S); free(S.sorted); S.reach };
        assert_eq!(reach, 0);
        for &P in &[1, 4, 14] {
            let mut v = base.clone();
            assert_eq!(unsafe { reach_try_f64(v.as_mut_ptr(), v.len(), P, reach) }, 0);                 // the gate: no reach
            assert!(v == base);
            assert_eq!(unsafe { reach_try_f64(v.as_mut_ptr(), v.len(), P, 1000) }, 0);                  // told a reach it does not have
            let last = g_reach_last.with(|c| c.get());
            assert!(last == LOGOS_REACH_DECLINED_PLAN || last == LOGOS_REACH_DECLINED_PROBE, "P {} last {}", P, last);
            assert_eq!(bits(&v), want);
        }
        // sparse kinks on one strand: declined for the detector, untouched
        let mut k: Vec<f64> = (0..2_000_000).map(|i| i as f64 + 0.5).collect();
        for i in (0..k.len() - 1).step_by(100) { k.swap(i, i + 1); }
        let kb = k.clone();
        assert_eq!(unsafe { reach_try_f64(k.as_mut_ptr(), k.len(), 4, 600) }, 0);
        assert_eq!(g_reach_last.with(|c| c.get()), LOGOS_REACH_DECLINED_KINKS); assert!(k == kb);
    }

    // ---- tail and head ----------------------------------------------------------------------------------------------------
    fn prefix_tail(n: usize, d: usize, seed: u64) -> Vec<f64> {
        let mut v = uniform(n, seed); v[..d].sort_by(|x, y| x.partial_cmp(y).unwrap()); v
    }
    #[test]
    fn tail_merge_forms() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 2_000_000usize;
        unsafe {
            for &(share, kind, simd) in &[(0.95, 1, 0), (0.95, 1, 1), (0.9999, 2, 1), (0.5, 3, 1), (0.5, 1, 0)] {
                let d = (n as f64 * share) as usize; let base = prefix_tail(n, d, 41); let want = bits(&base);
                for &P in &[1, 4, 14] {
                    let mut v = base.clone(); g_simd_merge = simd; g_tail_kind = 0;
                    assert_eq!(tail_merge_try_f64(v.as_mut_ptr(), n, d, P), 1);
                    assert!(ascending(&v), "share {} P {}", share, P); assert_eq!(bits(&v), want);
                    if P == 1 { assert_eq!(g_tail_kind, kind); } else { assert_eq!(g_tail_kind, 0); }
                }
            }
            // small, sequential: duplicates, signed zeros, infinities
            let mut b: Vec<f64> = vec![f64::NEG_INFINITY, -3.0, -0.0, 0.0, 0.0, 1.0, 1.0, 2.0, f64::INFINITY, f64::INFINITY, 0.0, -0.0, 1.0, f64::NEG_INFINITY, f64::INFINITY, 2.0, -3.0];
            let want = bits(&b); g_simd_merge = 0;
            assert_eq!(tail_merge_try_f64(b.as_mut_ptr(), b.len(), 10, 1), 1); assert!(ascending(&b)); assert_eq!(bits(&b), want);
            // uniform random data: this route has no gate of its own (the dispatcher's is where the prescan stopped); given the first
            // descent it still sorts. What it declines is a NaN, and then a[] is untouched at every thread count.
            let base = uniform(n, 42); let want = bits(&base);
            let d = (1..n).find(|&i| base[i] < base[i - 1]).unwrap();
            for &P in &[1, 4, 14] { let mut v = base.clone(); assert_eq!(tail_merge_try_f64(v.as_mut_ptr(), n, d, P), 1); assert!(ascending(&v)); assert_eq!(bits(&v), want); }
            let mut nb = prefix_tail(n, n - n / 20, 43); nb[n - 5] = f64::NAN; let want: Vec<u64> = nb.iter().map(|x| x.to_bits()).collect();
            for &P in &[1, 4, 14] { let mut v = nb.clone(); assert_eq!(tail_merge_try_f64(v.as_mut_ptr(), n, n - n / 20, P), 0); assert!(v.iter().map(|x| x.to_bits()).eq(want.iter().copied())); }
            g_simd_merge = 0;
        }
    }
    fn head_suffix(n: usize, hs: usize, seed: u64) -> Vec<f64> {
        let mut v = uniform(n, seed); v[hs..].sort_by(|x, y| x.partial_cmp(y).unwrap());
        if v[hs - 1] <= v[hs] { v[hs - 1] = v[hs] + 1.0; }               // the suffix starts exactly at hs
        v
    }
    #[test]
    fn head_merge_forms() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 2_000_000usize;
        unsafe {
            for &(share, kind, simd) in &[(0.05, 1, 0), (0.05, 1, 1), (0.0001, 2, 1), (0.4, 3, 1), (0.4, 1, 0)] {
                let hs = (n as f64 * share) as usize; let base = head_suffix(n, hs, 51); let want = bits(&base);
                for &P in &[1, 4, 14] {
                    let mut v = base.clone(); g_simd_merge = simd; g_head_kind = 0;
                    assert_eq!(head_merge_try_f64(v.as_mut_ptr(), n, P), 1);
                    assert!(ascending(&v), "share {} P {}", share, P); assert_eq!(bits(&v), want); assert_eq!(g_head_kind, kind);
                }
            }
            let mut b: Vec<f64> = vec![0.0, f64::INFINITY, -0.0, 1.0, f64::NEG_INFINITY, f64::NEG_INFINITY, -3.0, -0.0, 0.0, 0.0, 1.0, 1.0, 2.0, f64::INFINITY, f64::INFINITY];
            let want = bits(&b); g_simd_merge = 0;
            assert_eq!(head_merge_try_f64(b.as_mut_ptr(), b.len(), 1), 1); assert!(ascending(&b)); assert_eq!(bits(&b), want);
            // uniform random data, a sorted array (no head), a NaN: declined, untouched
            let base = uniform(n, 52);
            for &P in &[1, 4, 14] { let mut v = base.clone(); assert_eq!(head_merge_try_f64(v.as_mut_ptr(), n, P), 0); assert!(v == base); }
            let mut s: Vec<f64> = (0..1000).map(|i| i as f64).collect(); assert_eq!(head_merge_try_f64(s.as_mut_ptr(), 1000, 1), 0);
            let mut nb = head_suffix(n, n / 20, 53); nb[3] = f64::NAN; let want: Vec<u64> = nb.iter().map(|x| x.to_bits()).collect();
            assert_eq!(head_merge_try_f64(nb.as_mut_ptr(), n, 1), 0); assert!(nb.iter().map(|x| x.to_bits()).eq(want.iter().copied()));
        }
    }

    // ---- the small pieces ---------------------------------------------------------------------------------------------------
    #[test]
    fn small_pieces() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            // lattice_spill_finish: nl lattice elements in place, spills merged in from the top
            let mut r = Rng(61);
            for &(nl, spn) in &[(0usize, 5usize), (1000, 0), (1000, 1), (5000, 300), (3, 4000)] {
                let mut lat: Vec<f64> = (0..nl).map(|_| (r.below(50) as f64) - 25.0).collect(); lat.sort_by(|x, y| x.partial_cmp(y).unwrap());
                let mut sp: Vec<f64> = (0..spn).map(|_| match r.below(8) { 0 => f64::INFINITY, 1 => f64::NEG_INFINITY, 2 => -0.0, 3 => 0.0, _ => r.f() * 60.0 - 30.0 }).collect();
                let mut a = lat.clone(); a.resize(nl + spn, 0.0);
                let mut all = lat.clone(); all.extend_from_slice(&sp); let want = bits(&all);
                lattice_spill_finish(a.as_mut_ptr(), nl, sp.as_mut_ptr(), spn);
                assert!(ascending(&a)); assert_eq!(bits(&a), want);
            }
            // reach_minmax, the bounds
            let v = uniform(1003, 62); let (mut mn, mut mx) = (0.0, 0.0); reach_minmax(v.as_ptr(), v.len(), &mut mn, &mut mx);
            assert_eq!(mn, v.iter().cloned().fold(f64::INFINITY, f64::min)); assert_eq!(mx, v.iter().cloned().fold(f64::NEG_INFINITY, f64::max));
            let s = [1.0, 2.0, 2.0, 2.0, 5.0];
            assert_eq!(lower_bound_d(s.as_ptr(), 0, 5, 2.0), 1); assert_eq!(reach_upper_bound(s.as_ptr(), 0, 5, 2.0), 4); assert_eq!(tail_lb(s.as_ptr(), 5, 6.0), 5);
            // the ladder, mirrored: a descending array has a reach when read back to front, none forward
            let n = 1usize << 20; let d: Vec<f64> = (0..n).map(|i| (n - i) as f64).collect();
            assert_eq!(reach_from_sample(d.as_ptr(), n, null(), 0, 0), 0); assert!(reach_from_sample(d.as_ptr(), n, null(), 0, 1) > 0);
        }
    }
}
