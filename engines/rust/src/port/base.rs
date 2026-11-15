//! `base`: C lines 351-852 (contexts, small sorts, the int32 radix), 3160-3353 (the barrier, the persistent worker pool,
//! `par_*`), 4806-4829 (`sample_read`, `sample_sort`) and 5573-5700 (`big_alloc`, the buffer pool, the scratch).
//! A transliteration: see `PORTING.md`.
use super::*;
use core::sync::atomic::AtomicPtr;
use std::sync::{Condvar, Mutex, Once};

// ---- pthread's names over std (the C's lock / unlock pairs span functions, so a guard cannot stand in for them) --------
/// A mutex with pthread's lock / trylock / unlock, usable in a `static`.
pub(crate) struct pthread_mutex_t { locked: Mutex<bool>, cv: Condvar }
impl pthread_mutex_t { pub(crate) const fn new() -> Self { pthread_mutex_t { locked: Mutex::new(false), cv: Condvar::new() } } }
#[inline]
pub(crate) unsafe fn pthread_mutex_lock(m: *const pthread_mutex_t) {
    let m = &*m;
    let mut g = m.locked.lock().unwrap_or_else(|e| e.into_inner());
    while *g { g = m.cv.wait(g).unwrap_or_else(|e| e.into_inner()); }
    *g = true;
}
/// 0 when the lock was taken, as pthread_mutex_trylock.
#[inline]
pub(crate) unsafe fn pthread_mutex_trylock(m: *const pthread_mutex_t) -> i32 {
    let m = &*m;
    let mut g = m.locked.lock().unwrap_or_else(|e| e.into_inner());
    if *g { return 16 }                              // EBUSY
    *g = true; 0
}
#[inline]
pub(crate) unsafe fn pthread_mutex_unlock(m: *const pthread_mutex_t) {
    let m = &*m;
    let mut g = m.locked.lock().unwrap_or_else(|e| e.into_inner());
    *g = false; drop(g); m.cv.notify_one();
}
/// A thread handle: plain data (all zeros is valid, Copy), so arrays of them are declared as the C declares `pthread_t th[N]`.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct pthread_t { pub(crate) h: *mut c_void }
pub(crate) type worker_fn_t = unsafe fn(*mut c_void) -> *mut c_void;
struct send_t(worker_fn_t, *mut c_void);
unsafe impl Send for send_t {}
/// pthread_create(th, NULL, fn, arg). Always 0: a creation that fails aborts (PORTING.md).
pub(crate) unsafe fn pthread_create(th: *mut pthread_t, fn_: worker_fn_t, arg: *mut c_void) -> i32 {
    let s = send_t(fn_, arg);
    let h = std::thread::spawn(move || { let s = s; unsafe { (s.0)(s.1); } });
    (*th).h = Box::into_raw(Box::new(h)) as *mut c_void;
    0
}
pub(crate) unsafe fn pthread_join(th: pthread_t) {
    let h = Box::from_raw(th.h as *mut std::thread::JoinHandle<()>);
    if let Err(e) = h.join() { std::panic::resume_unwind(e) }
}

// ---- the pool trim (C lines 327-348): opt-in by LOGOS_TRIM, so no-ops here with the C's signatures -----------------------
pub(crate) const g_no_trim: i32 = 1;
pub(crate) const g_trim_hard: i32 = 0;
#[inline(always)] pub(crate) unsafe fn trim_note(t: *mut trim_t, base: *mut c_void, cap: usize, req: usize) {}
#[inline(always)] pub(crate) unsafe fn trim_reset(t: *mut trim_t) { (*t).hi = 0; }

// ---- contexts (C lines 351-493) -------------------------------------------------------------------------------------------
pub(crate) unsafe fn ctx_release(c: *mut ctx_t) {
    free((*c).aux); free((*c).idx); free((*c).wc); free((*c).wcn);
    for d in 0..COARSE_MAXD { free((*c).C[d]); free((*c).S[d]); free((*c).H[d]); free((*c).land[d]); }
}
pub(crate) unsafe fn ctx_coarse_alloc(c: *mut ctx_t) -> i32 {
    let ok = 1;
    for d in 0..COARSE_MAXD {
        (*c).land[d] = malloc(COARSE_K);
        (*c).C[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
        (*c).S[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
        (*c).H[d] = malloc((COARSE_K + 1) * size_of::<usize>()) as *mut usize;
    }
    ok
}
pub(crate) unsafe fn ctx_wc_alloc(c: *mut ctx_t) -> i32 {
    (*c).wc = malloc(COARSE_K * 8 * size_of::<f64>()) as *mut f64; (*c).wcn = calloc(COARSE_K, 1);
    1
}
pub(crate) unsafe fn ctx_alloc(c: *mut ctx_t, cap: usize, wc_on: i32) -> i32 {
    *c = core::mem::zeroed(); (*c).cap = cap; (*c).wc_on = wc_on; (*c).pool_ix = -1;
    let mut ok = 1;
    (*c).aux = malloc((cap + 1) * size_of::<u32>()) as *mut u32;
    (*c).idx = malloc((if cap != 0 { cap } else { 1 }) * size_of::<u16>()) as *mut u16;
    if ok != 0 && wc_on != 0 && ctx_wc_alloc(c) == 0 { ok = 0; }
    if ok == 0 { ctx_release(c); *c = core::mem::zeroed(); (*c).pool_ix = -1; }
    ok
}
pub(crate) const CTX_POOL_MAX: usize = 320;
pub(crate) static mut g_ctx_pool: [ctx_t; CTX_POOL_MAX] = unsafe { core::mem::zeroed() };
pub(crate) static mut g_ctx_pool_used: [i32; CTX_POOL_MAX] = [0; CTX_POOL_MAX];
pub(crate) static mut g_ctx_next: i32 = 0;
pub(crate) static mut g_ctx_trim: [[trim_t; 2]; CTX_POOL_MAX] = [[trim_t { hw: 0, hi: 0, ep: 0 }; 2]; CTX_POOL_MAX];   // aux, idx
pub(crate) unsafe fn ctx_init(c: *mut ctx_t, cap: usize, wc_on: i32) -> i32 {
    if g_ctx_next >= CTX_POOL_MAX as i32 { let r = ctx_alloc(c, cap, wc_on); (*c).pooled = 0; return r; }
    let i = g_ctx_next as usize; g_ctx_next += 1;
    let b: *mut ctx_t = (addr_of_mut!(g_ctx_pool) as *mut ctx_t).add(i);
    let used: *mut i32 = (addr_of_mut!(g_ctx_pool_used) as *mut i32).add(i);
    let tr: *mut trim_t = (addr_of_mut!(g_ctx_trim) as *mut trim_t).add(2 * i);
    if *used == 0 || (*b).cap < cap {
        if *used != 0 { ctx_release(b); }
        let want = cap + cap / 4 + 64;
        if ctx_alloc(b, want, wc_on) == 0 { *used = 0; *c = core::mem::zeroed(); return 0; }
        *used = 1; trim_reset(tr); trim_reset(tr.add(1));
    }
    trim_note(tr, (*b).aux as *mut c_void, ((*b).cap + 1) * size_of::<u32>(), (cap + 1) * size_of::<u32>());
    trim_note(tr.add(1), (*b).idx as *mut c_void, (*b).cap * size_of::<u16>(), cap * size_of::<u16>());
    if wc_on != 0 && (*b).wc.is_null() && ctx_wc_alloc(b) == 0 { *c = core::mem::zeroed(); (*c).pool_ix = -1; return 0; }
    let bcap = (*b).cap;
    *c = core::mem::zeroed();
    (*c).aux = (*b).aux; (*c).idx = (*b).idx; (*c).wc = (*b).wc; (*c).wcn = (*b).wcn;
    for d in 0..COARSE_MAXD { (*c).land[d] = (*b).land[d]; (*c).C[d] = (*b).C[d]; (*c).S[d] = (*b).S[d]; (*c).H[d] = (*b).H[d]; }
    if !(*c).wcn.is_null() { memset((*c).wcn, 0, COARSE_K); }
    (*c).cap = bcap; (*c).wc_on = wc_on; (*c).pooled = 1; (*c).pool_ix = i as i32;
    1
}
/// the coarse tables, on first entry to the coarse path. Into the bundle when the context is pooled, so the next sort has them
pub(crate) unsafe fn ctx_coarse_ready(c: *mut ctx_t) -> i32 {
    if !(*c).C[0].is_null() { return 1 }
    let b: *mut ctx_t = if (*c).pool_ix >= 0 { (addr_of_mut!(g_ctx_pool) as *mut ctx_t).add((*c).pool_ix as usize) } else { c };
    if b != c && !(*b).C[0].is_null() { for d in 0..COARSE_MAXD { (*c).land[d] = (*b).land[d]; (*c).C[d] = (*b).C[d]; (*c).S[d] = (*b).S[d]; (*c).H[d] = (*b).H[d]; } return 1; }
    if ctx_coarse_alloc(b) == 0 { return 0 }
    if b != c { for d in 0..COARSE_MAXD { (*c).land[d] = (*b).land[d]; (*c).C[d] = (*b).C[d]; (*c).S[d] = (*b).S[d]; (*c).H[d] = (*b).H[d]; } }
    1
}
pub(crate) unsafe fn ctx_free(c: *mut ctx_t) { if (*c).pooled == 0 { ctx_release(c); } }

// a context that fits the bucket at hand
pub(crate) static g_jumbo_m: pthread_mutex_t = pthread_mutex_t::new();
pub(crate) static mut g_jumbo_aux: *mut u32 = null_mut();
pub(crate) static mut g_jumbo_idx: *mut u16 = null_mut();
pub(crate) static mut g_jumbo_cap: usize = 0;
#[inline]
pub(crate) unsafe fn ctx_typical(maxb: usize, n: usize, nbk: i32) -> usize { let t = 2 * (n / (if nbk > 0 { nbk } else { 1 }) as usize) + 4096; if maxb < t { maxb } else { t } }
pub(crate) unsafe fn ctx_fit(c: *mut ctx_t, nb: usize, f: *mut ctx_fit_t) -> i32 {
    (*f).kind = 0;
    if (*c).aux.is_null() { return 0 }               // a context that could not be allocated is all zeros
    if nb <= (*c).cap { return 1 }
    (*f).aux = (*c).aux; (*f).idx = (*c).idx; (*f).cap = (*c).cap;
    if pthread_mutex_trylock(&g_jumbo_m) == 0 {
        if g_jumbo_cap < nb {
            let want = nb + nb / 4 + 64;
            let na = malloc((want + 1) * size_of::<u32>()) as *mut u32; let ni = malloc(want * size_of::<u16>()) as *mut u16;
            free(g_jumbo_aux); free(g_jumbo_idx); g_jumbo_aux = na; g_jumbo_idx = ni; g_jumbo_cap = want;
        }
        if g_jumbo_cap >= nb { (*c).aux = g_jumbo_aux; (*c).idx = g_jumbo_idx; (*c).cap = g_jumbo_cap; (*f).kind = 1; return 1; }
        pthread_mutex_unlock(&g_jumbo_m);
    }
    let ta = malloc((nb + 1) * size_of::<u32>()) as *mut u32; let ti = malloc((if nb != 0 { nb } else { 1 }) * size_of::<u16>()) as *mut u16;
    (*c).aux = ta; (*c).idx = ti; (*c).cap = nb; (*f).kind = 2;
    1
}
pub(crate) unsafe fn ctx_unfit(c: *mut ctx_t, f: *mut ctx_fit_t) {
    if (*f).kind == 0 { return }
    if (*f).kind == 2 { free((*c).aux); free((*c).idx); }
    (*c).aux = (*f).aux; (*c).idx = (*f).idx; (*c).cap = (*f).cap;
    if (*f).kind == 1 { pthread_mutex_unlock(&g_jumbo_m); }
}
/// everything the context side keeps between calls; logos_release_buffers calls it
pub(crate) unsafe fn ctx_release_all() {
    for i in 0..CTX_POOL_MAX {
        if g_ctx_pool_used[i] != 0 {
            let b = (addr_of_mut!(g_ctx_pool) as *mut ctx_t).add(i);
            ctx_release(b); *b = core::mem::zeroed(); g_ctx_pool_used[i] = 0;
        }
    }
    g_ctx_next = 0;
    pthread_mutex_lock(&g_jumbo_m);
    free(g_jumbo_aux); free(g_jumbo_idx); g_jumbo_aux = null_mut(); g_jumbo_idx = null_mut(); g_jumbo_cap = 0;
    pthread_mutex_unlock(&g_jumbo_m);
}

// ---- C lines 496-626 ------------------------------------------------------------------------------------------------------
pub(crate) const g_no_dense_probe: i32 = 0;          // LOGOS_NO_DENSE_PROBE unset
pub(crate) const g_no_fastpre: i32 = 0;              // LOGOS_NO_FASTPRESCAN unset
pub(crate) const g_no_frontfuse: i32 = 0;            // LOGOS_NO_FRONTFUSE unset
pub(crate) const g_no_partfuse: i32 = 0;             // LOGOS_NO_PARTFUSE unset
pub(crate) unsafe fn log2i(mut n: usize) -> i32 { let mut l = 0; loop { n >>= 1; if n == 0 { break } l += 1; } l }

// SMALLNET: branchless sorting networks for 4 to 16 elements (generated in C by gen_small_net.py; the same comparators here).
// NET_CE moves elements (one predicate, two selects), so a NaN leaves its pair alone and the output stays a permutation.
macro_rules! NET_CE {
    ($a:ident, $i:expr, $j:expr) => {{
        let x_ = *$a.add($i); let y_ = *$a.add($j); let c_ = y_ < x_;
        *$a.add($i) = if c_ { y_ } else { x_ }; *$a.add($j) = if c_ { x_ } else { y_ };
    }};
}
#[inline(never)]
pub(crate) unsafe fn net_sort_small(a: *mut f64, n: i64) {
    match n {
        4 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 1, 2);
        }
        5 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 1, 2); NET_CE!(a, 0, 4); NET_CE!(a, 2, 4); NET_CE!(a, 1, 2);
            NET_CE!(a, 3, 4);
        }
        6 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 1, 2); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5);
            NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4);
        }
        7 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6);
            NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6);
        }
        8 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 5, 7);
            NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5);
            NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6);
        }
        9 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 5, 7);
            NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5);
            NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 0, 8); NET_CE!(a, 4, 8); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8);
            NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8);
        }
        10 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6);
            NET_CE!(a, 5, 7); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 2, 4);
            NET_CE!(a, 3, 5); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 0, 8); NET_CE!(a, 1, 9); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9);
            NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8); NET_CE!(a, 7, 9); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8);
        }
        11 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6);
            NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6);
            NET_CE!(a, 3, 7); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 0, 8);
            NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9); NET_CE!(a, 6, 10); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8);
            NET_CE!(a, 7, 9); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8); NET_CE!(a, 9, 10);
        }
        12 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 10, 11); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3);
            NET_CE!(a, 4, 6); NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 9, 11); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 0, 4);
            NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6);
            NET_CE!(a, 9, 10); NET_CE!(a, 0, 8); NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 3, 11); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9); NET_CE!(a, 6, 10);
            NET_CE!(a, 7, 11); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8); NET_CE!(a, 7, 9); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6);
            NET_CE!(a, 7, 8); NET_CE!(a, 9, 10);
        }
        13 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 10, 11); NET_CE!(a, 0, 2); NET_CE!(a, 1, 3);
            NET_CE!(a, 4, 6); NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 9, 11); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 0, 4);
            NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 8, 12); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 10, 12); NET_CE!(a, 1, 2);
            NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 0, 8); NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 3, 11);
            NET_CE!(a, 4, 12); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9); NET_CE!(a, 6, 10); NET_CE!(a, 7, 11); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8);
            NET_CE!(a, 7, 9); NET_CE!(a, 10, 12); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12);
        }
        14 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 10, 11); NET_CE!(a, 12, 13); NET_CE!(a, 0, 2);
            NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 9, 11); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10);
            NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 8, 12); NET_CE!(a, 9, 13); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5);
            NET_CE!(a, 10, 12); NET_CE!(a, 11, 13); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 0, 8);
            NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 3, 11); NET_CE!(a, 4, 12); NET_CE!(a, 5, 13); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9); NET_CE!(a, 6, 10);
            NET_CE!(a, 7, 11); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8); NET_CE!(a, 7, 9); NET_CE!(a, 10, 12); NET_CE!(a, 11, 13); NET_CE!(a, 1, 2);
            NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12);
        }
        15 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 10, 11); NET_CE!(a, 12, 13); NET_CE!(a, 0, 2);
            NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 9, 11); NET_CE!(a, 12, 14); NET_CE!(a, 1, 2); NET_CE!(a, 5, 6);
            NET_CE!(a, 9, 10); NET_CE!(a, 13, 14); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7); NET_CE!(a, 8, 12); NET_CE!(a, 9, 13);
            NET_CE!(a, 10, 14); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 10, 12); NET_CE!(a, 11, 13); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6);
            NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 13, 14); NET_CE!(a, 0, 8); NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 3, 11); NET_CE!(a, 4, 12);
            NET_CE!(a, 5, 13); NET_CE!(a, 6, 14); NET_CE!(a, 4, 8); NET_CE!(a, 5, 9); NET_CE!(a, 6, 10); NET_CE!(a, 7, 11); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5);
            NET_CE!(a, 6, 8); NET_CE!(a, 7, 9); NET_CE!(a, 10, 12); NET_CE!(a, 11, 13); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8);
            NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 13, 14);
        }
        16 => {
            NET_CE!(a, 0, 1); NET_CE!(a, 2, 3); NET_CE!(a, 4, 5); NET_CE!(a, 6, 7); NET_CE!(a, 8, 9); NET_CE!(a, 10, 11); NET_CE!(a, 12, 13); NET_CE!(a, 14, 15);
            NET_CE!(a, 0, 2); NET_CE!(a, 1, 3); NET_CE!(a, 4, 6); NET_CE!(a, 5, 7); NET_CE!(a, 8, 10); NET_CE!(a, 9, 11); NET_CE!(a, 12, 14); NET_CE!(a, 13, 15);
            NET_CE!(a, 1, 2); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 13, 14); NET_CE!(a, 0, 4); NET_CE!(a, 1, 5); NET_CE!(a, 2, 6); NET_CE!(a, 3, 7);
            NET_CE!(a, 8, 12); NET_CE!(a, 9, 13); NET_CE!(a, 10, 14); NET_CE!(a, 11, 15); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 10, 12);
            NET_CE!(a, 11, 13); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 13, 14); NET_CE!(a, 0, 8);
            NET_CE!(a, 1, 9); NET_CE!(a, 2, 10); NET_CE!(a, 3, 11); NET_CE!(a, 4, 12); NET_CE!(a, 5, 13); NET_CE!(a, 6, 14); NET_CE!(a, 7, 15); NET_CE!(a, 4, 8);
            NET_CE!(a, 5, 9); NET_CE!(a, 6, 10); NET_CE!(a, 7, 11); NET_CE!(a, 2, 4); NET_CE!(a, 3, 5); NET_CE!(a, 6, 8); NET_CE!(a, 7, 9); NET_CE!(a, 10, 12);
            NET_CE!(a, 11, 13); NET_CE!(a, 1, 2); NET_CE!(a, 3, 4); NET_CE!(a, 5, 6); NET_CE!(a, 7, 8); NET_CE!(a, 9, 10); NET_CE!(a, 11, 12); NET_CE!(a, 13, 14);
        }
        _ => {}                                      // the caller checks the range; n outside it is a no-op, not a wrong sort
    }
}
pub(crate) const g_no_smallnet: i32 = 0;             // LOGOS_NO_SMALLNET unset
pub(crate) const g_avx2_over_flash: i32 = 0;         // LOGOS_AVX2_OVER_FLASH unset
pub(crate) const g_band_flash: i32 = 0;              // LOGOS_BAND_FLASH unset
pub(crate) static g_lowdisc_fires: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(30)
pub(crate) static g_sf_windows: AtomicUsize = AtomicUsize::new(0);      // logos_twist_last(31)
/// n >= FLASH_SORT_THRESHOLD: four windows of 32 adjacent pairs
#[inline]
pub(crate) unsafe fn band_local_order(a: *const f64, n: usize) -> i32 {
    let mut d: usize = 0;
    for w in 0..4usize { let lo = 1 + (n - 34) * w / 4; for i in lo..lo + 32 { d += (*a.add(i) < *a.add(i - 1)) as usize; } }
    (d * 3 < 128) as i32
}

// ---- leaf primitives (indices are signed so hi < lo is a legal empty range) -------------------------------------------------
macro_rules! at { ($a:ident, $i:expr) => { *$a.offset(($i) as isize) }; }
pub(crate) unsafe fn insertion_sort(a: *mut f64, lo: i64, hi: i64) {
    let mut i = lo + 1;
    while i <= hi { let k = at!(a, i); let mut j = i - 1; while j >= lo && at!(a, j) > k { at!(a, j + 1) = at!(a, j); j -= 1; } at!(a, j + 1) = k; i += 1; }
}
/// returns 1 if completed, 0 if the displacement budget (~8n) was exceeded
pub(crate) unsafe fn insertion_sort_momentum(a: *mut f64, lo: i64, hi: i64) -> i32 {
    let budget: i64 = 8 * (hi - lo + 1); let mut work: i64 = 0; let mut momentum: i64 = 1;
    let mut i = lo + 1;
    while i <= hi {
        let k = at!(a, i);
        if at!(a, i - 1) <= k { momentum = if momentum > 1 { momentum >> 1 } else { 1 }; i += 1; continue; }
        let mut j = i - 1; let mut step = momentum;
        while step > 1 && (j - step < lo || at!(a, j - step) <= k) { step >>= 1; }
        while j - step >= lo && at!(a, j - step) > k { j -= step; step <<= 1; }
        let mut left = if j - step < lo { lo } else { j - step }; let mut right = j;
        while left < right { let mid = (left + right) >> 1; if at!(a, mid) > k { right = mid; } else { left = mid + 1; } }
        let dist = i - left;
        work += dist;
        if work > budget { return 0 }
        let mut p = i; while p > left { at!(a, p) = at!(a, p - 1); p -= 1; }
        at!(a, left) = k; momentum = dist;
        i += 1;
    }
    1
}
pub(crate) unsafe fn sift_down(a: *mut f64, base: i64, mut root: i64, end: i64) {
    loop {
        let mut big = root; let l = 2 * root + 1; let r = l + 1;
        if l < end && at!(a, base + l) > at!(a, base + big) { big = l; }
        if r < end && at!(a, base + r) > at!(a, base + big) { big = r; }
        if big == root { return }
        let t = at!(a, base + root); at!(a, base + root) = at!(a, base + big); at!(a, base + big) = t;
        root = big;
    }
}
pub(crate) unsafe fn heap_sort(a: *mut f64, lo: i64, hi: i64) {
    let len = hi - lo + 1;
    let mut i = (len >> 1) - 1; while i >= 0 { sift_down(a, lo, i, len); i -= 1; }
    let mut i = len - 1; while i > 0 { let t = at!(a, lo); at!(a, lo) = at!(a, lo + i); at!(a, lo + i) = t; sift_down(a, lo, 0, i); i -= 1; }
}
macro_rules! SWP { ($a:ident, $x:expr, $y:expr) => {{ let _t = at!($a, $x); at!($a, $x) = at!($a, $y); at!($a, $y) = _t; }}; }
pub(crate) unsafe fn sort5(a: *mut f64, i1: i64, i2: i64, i3: i64, i4: i64, i5: i64) {
    if at!(a, i2) < at!(a, i1) { SWP!(a, i2, i1); }
    if at!(a, i3) < at!(a, i2) { SWP!(a, i3, i2); if at!(a, i2) < at!(a, i1) { SWP!(a, i2, i1); } }
    if at!(a, i4) < at!(a, i3) { SWP!(a, i4, i3); if at!(a, i3) < at!(a, i2) { SWP!(a, i3, i2); if at!(a, i2) < at!(a, i1) { SWP!(a, i2, i1); } } }
    if at!(a, i5) < at!(a, i4) { SWP!(a, i5, i4); if at!(a, i4) < at!(a, i3) { SWP!(a, i4, i3); if at!(a, i3) < at!(a, i2) { SWP!(a, i3, i2); if at!(a, i2) < at!(a, i1) { SWP!(a, i2, i1); } } } }
}
/// dual-pivot introsort; d = remaining depth budget, heapsort at 0
pub(crate) unsafe fn quicksort(a: *mut f64, mut lo: i64, mut hi: i64, mut d: i32) {
    while hi - lo >= INSERTION_SORT_THRESHOLD as i64 {
        if d == 0 { heap_sort(a, lo, hi); return; }
        let len = hi - lo + 1;
        let seventh = (len >> 3) + (len >> 6) + 1;
        let e3 = (lo + hi) >> 1; let e2 = e3 - seventh; let e4 = e3 + seventh; let e1 = e2 - seventh; let e5 = e4 + seventh;
        sort5(a, e1, e2, e3, e4, e5);
        if at!(a, e1) != at!(a, e2) && at!(a, e2) != at!(a, e3) && at!(a, e3) != at!(a, e4) && at!(a, e4) != at!(a, e5) {
            let p1 = at!(a, e2); let p2 = at!(a, e4);
            at!(a, e2) = at!(a, lo); at!(a, e4) = at!(a, hi);
            let mut less = lo + 1; let mut great = hi - 1; let mut k = less;
            while k <= great {
                let ak = at!(a, k);
                if ak < p1 { at!(a, k) = at!(a, less); at!(a, less) = ak; less += 1; }
                else if ak > p2 {
                    while k < great && at!(a, great) > p2 { great -= 1; }
                    let ag = at!(a, great);
                    at!(a, k) = ag; at!(a, great) = ak; great -= 1;
                    if ag < p1 { at!(a, k) = at!(a, less); at!(a, less) = ag; less += 1; }
                }
                k += 1;
            }
            at!(a, lo) = at!(a, less - 1); at!(a, less - 1) = p1;
            at!(a, hi) = at!(a, great + 1); at!(a, great + 1) = p2;
            d -= 1;
            quicksort(a, lo, less - 2, d);
            quicksort(a, great + 2, hi, d);
            if p1 == p2 { return }
            let mut m_lo = less; let mut m_hi = great;
            while m_lo <= m_hi && at!(a, m_lo) == p1 { m_lo += 1; }
            while m_lo <= m_hi && at!(a, m_hi) == p2 { m_hi -= 1; }
            lo = m_lo; hi = m_hi;
        } else {
            let pv = at!(a, e3);
            let mut lt = lo; let mut gt = hi; let mut k = lo;
            while k <= gt {
                let v = at!(a, k);
                if v < pv { let t = at!(a, lt); at!(a, lt) = v; at!(a, k) = t; lt += 1; k += 1; }
                else if v > pv { let t = at!(a, gt); at!(a, gt) = v; at!(a, k) = t; gt -= 1; }
                else { k += 1; }
            }
            d -= 1;
            if lt - lo < hi - gt { quicksort(a, lo, lt - 1, d); lo = gt + 1; }
            else { quicksort(a, gt + 1, hi, d); hi = lt - 1; }
        }
    }
    // the base case: a branchless network for 4 to 16 elements, insertion sort otherwise
    { let len = hi - lo + 1;
      if len >= 4 && len <= 16 && g_no_smallnet == 0 { net_sort_small(a.offset(lo as isize), len); } else { insertion_sort(a, lo, hi); } }
}

// ---- integer paths ----------------------------------------------------------------------------------------------------------
/// LSD radix for int32-valued doubles (the default build's form: ONE scratch array, the keys narrowed in place into a's own
/// storage; LOGOS_RADIX_TWOBUF unset). buf is read as uint32.
pub(crate) unsafe fn lsd_radix_int32(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) {
    let twobuf: i32 = 0;                                                // n >= 4096 && getenv("LOGOS_RADIX_TWOBUF")
    let mut src: *mut u32 = if twobuf != 0 { (*c).aux } else { a as *mut u32 }; let mut dst: *mut u32 = buf as *mut u32;
    let av: *const f64 = a as *const f64;
    let c0 = addr_of_mut!((*c).hist[0]) as *mut u32; let c1 = addr_of_mut!((*c).hist[1]) as *mut u32;
    let c2 = addr_of_mut!((*c).hist[2]) as *mut u32; let c3 = addr_of_mut!((*c).hist[3]) as *mut u32;
    memset(c0, 0, 257 * 4); memset(c1, 0, 257 * 4); memset(c2, 0, 257 * 4); memset(c3, 0, 257 * 4);
    let mut or_a: u32 = 0; let mut and_a: u32 = 0xFFFFFFFF;
    for i in 0..n {
        let v: u32 = ((*av.add(i)) as i32 as u32) ^ 0x80000000; *src.add(i) = v; or_a |= v; and_a &= v;
        *c0.add(((v & 0xFF) + 1) as usize) += 1; *c1.add((((v >> 8) & 0xFF) + 1) as usize) += 1;
        *c2.add((((v >> 16) & 0xFF) + 1) as usize) += 1; *c3.add((((v >> 24) & 0xFF) + 1) as usize) += 1;
    }
    let diff = or_a ^ and_a;
    let a0 = ((diff & 0xFF) != 0) as i32; let a1 = ((diff & 0xFF00) != 0) as i32; let a2 = ((diff & 0xFF0000) != 0) as i32; let a3 = ((diff & 0xFF000000) != 0) as i32;
    let last: i32 = if a3 != 0 { 3 } else if a2 != 0 { 2 } else if a1 != 0 { 1 } else if a0 != 0 { 0 } else { -1 };
    if last < 0 {                                                       // all equal: widen back, from the END (the keys are in a's storage)
        let mut i = n; while i > 0 { i -= 1; let v = *src.add(i); *a.add(i) = (v ^ 0x80000000) as i32 as f64; }
        return;
    }
    if twobuf == 0 && ((a0 + a1 + a2 + a3 - 1) & 1) == 0 { memcpy(dst, src, n * size_of::<u32>()); let t = src; src = dst; dst = t; }
    let mut from: *mut u32 = src; let mut to: *mut u32 = dst; let mut tmp: *mut u32;
    macro_rules! RPASS {
        ($cc:ident, $sh:expr, $act:ident, $idx:expr) => {
            if $act != 0 {
                for b in 1..257usize { *$cc.add(b) = (*$cc.add(b)).wrapping_add(*$cc.add(b - 1)); }
                if last == $idx {
                    for i in 0..n { let v = *from.add(i); let p = $cc.add(((v >> $sh) & 0xFF) as usize); *a.add(*p as usize) = (v ^ 0x80000000) as i32 as f64; *p = (*p).wrapping_add(1); }
                    return;
                }
                for i in 0..n {
                    if i + PF_DIST < n { _mm_prefetch::<_MM_HINT_NTA>(to.add(*$cc.add(((*from.add(i + PF_DIST) >> $sh) & 0xFF) as usize) as usize) as *const i8); }
                    let v = *from.add(i); let p = $cc.add(((v >> $sh) & 0xFF) as usize); *to.add(*p as usize) = v; *p = (*p).wrapping_add(1);
                }
                tmp = from; from = to; to = tmp;
            }
        };
    }
    RPASS!(c0, 0, a0, 0);
    RPASS!(c1, 8, a1, 1);
    RPASS!(c2, 16, a2, 2);
    for b in 1..257usize { *c3.add(b) = (*c3.add(b)).wrapping_add(*c3.add(b - 1)); }
    for i in 0..n { let v = *from.add(i); let p = c3.add(((v >> 24) & 0xFF) as usize); *a.add(*p as usize) = (v ^ 0x80000000) as i32 as f64; *p = (*p).wrapping_add(1); }
}

// ---- the AVX2 leaf's policy (C lines 833-852; the kernel itself is elsewhere) -------------------------------------------------
pub(crate) const AVX2_LEAF_MAX_N: usize = usize::MAX;
/// AVX2_TAKE(c, n)
#[inline(always)]
pub(crate) unsafe fn avx2_take(c: *const ctx_t, n: usize) -> i32 {
    ((*c).seq != 0 && (*c).nearly == 0 && (*c).lowdisc == 0 && avx2_leaf_on() != 0 && n <= AVX2_LEAF_MAX_N) as i32
}

// ---- the barrier (C lines 3155-3181) ------------------------------------------------------------------------------------------
#[inline(always)] pub(crate) fn cpu_relax() { core::hint::spin_loop() }
pub(crate) const BARRIER_SPIN: i32 = 20000;
/// Not Copy (it holds a mutex and a condition variable). Declare it zeroed or uninitialised and call barrier_init, as the C does.
#[repr(C)]
pub(crate) struct barrier_t { pub(crate) m: Mutex<()>, pub(crate) cv: Condvar, pub(crate) count: AtomicI32, pub(crate) arrived: AtomicI32, pub(crate) gen: AtomicI32 }
pub(crate) unsafe fn barrier_init(b: *mut barrier_t, n: i32) {
    core::ptr::write(b, barrier_t { m: Mutex::new(()), cv: Condvar::new(), count: AtomicI32::new(n), arrived: AtomicI32::new(0), gen: AtomicI32::new(0) });
}
/// One participant fewer. Only the thread that runs the create loop calls this, and only before its own first arrival.
pub(crate) unsafe fn barrier_drop(b: *mut barrier_t) { (*b).count.fetch_sub(1, Release); }
pub(crate) unsafe fn barrier_wait(b: *mut barrier_t) {
    let b = &*b;
    let g = b.gen.load(Acquire);
    if b.arrived.fetch_add(1, Ordering::AcqRel) + 1 == b.count.load(Acquire) {
        b.arrived.store(0, Relaxed);
        let gd = b.m.lock().unwrap_or_else(|e| e.into_inner());
        b.gen.store(g.wrapping_add(1), Release);                        // release spinners
        b.cv.notify_all();                                              // and sleepers
        drop(gd);
        return;
    }
    for _i in 0..BARRIER_SPIN { if b.gen.load(Acquire) != g { return } cpu_relax(); }
    let mut gd = b.m.lock().unwrap_or_else(|e| e.into_inner());
    while b.gen.load(Acquire) == g { gd = b.cv.wait(gd).unwrap_or_else(|e| e.into_inner()); }
    drop(gd);
}
pub(crate) unsafe fn barrier_destroy(b: *mut barrier_t) {}             // a futex mutex and condvar hold nothing to destroy

// ---- the persistent worker pool (C lines 3183-3296) ---------------------------------------------------------------------------
pub(crate) const WP_MAX: i32 = 256;
pub(crate) const WP_SPIN: i32 = 4000;
#[repr(C, align(64))]
pub(crate) struct wp_slot_t {
    pub(crate) m: Mutex<()>, pub(crate) cv: Condvar,
    pub(crate) state: AtomicI32,                     // 0 idle, 1 a task is posted, 2 quit
    pub(crate) fn_: Option<worker_fn_t>, pub(crate) arg: *mut c_void,   // written by the owner before state = 1 (release)
    pub(crate) th: pthread_t,
}
pub(crate) static mut g_wp: *mut wp_slot_t = null_mut();               // WP_MAX mailboxes, allocated once
pub(crate) static mut g_wp_n: i32 = 0;                                 // threads running; touched only by the holder of g_wp_busy
pub(crate) static g_wp_busy: AtomicI32 = AtomicI32::new(0);            // a group holds the pool
pub(crate) static g_wp_left: AtomicI32 = AtomicI32::new(0);            // the holder's tasks not yet finished
pub(crate) static g_wp_groups: AtomicUsize = AtomicUsize::new(0);      // logos_twist_last(19)
pub(crate) static g_wp_owner: AtomicPtr<c_void> = AtomicPtr::new(null_mut());   // the par_spawn group's handle array
pub(crate) static g_wp_dm: Mutex<()> = Mutex::new(());
pub(crate) static g_wp_dcv: Condvar = Condvar::new();
pub(crate) static g_wp_once: Once = Once::new();
pub(crate) unsafe fn wp_init() {
    let s = malloc(WP_MAX as usize * size_of::<wp_slot_t>()) as *mut wp_slot_t;   // the prelude's blocks are 64-byte aligned
    for i in 0..WP_MAX as usize {
        core::ptr::write(s.add(i), wp_slot_t { m: Mutex::new(()), cv: Condvar::new(), state: AtomicI32::new(0), fn_: None, arg: null_mut(), th: pthread_t { h: null_mut() } });
    }
    g_wp = s;
}
pub(crate) unsafe fn wp_main(p: *mut c_void) -> *mut c_void {
    let s = p as *mut wp_slot_t;
    loop {
        let mut st; let mut spins = 0;
        loop {
            st = (*s).state.load(Acquire);
            if st != 0 { break }
            spins += 1;
            if spins < WP_SPIN { cpu_relax(); continue; }
            let mut gd = (*s).m.lock().unwrap_or_else(|e| e.into_inner());
            loop { st = (*s).state.load(Acquire); if st != 0 { break } gd = (*s).cv.wait(gd).unwrap_or_else(|e| e.into_inner()); }
            drop(gd);
            break;
        }
        if st == 2 { return null_mut() }
        ((*s).fn_.unwrap())((*s).arg);
        (*s).state.store(0, Release);
        if g_wp_left.fetch_sub(1, Ordering::AcqRel) == 1 {
            let gd = g_wp_dm.lock().unwrap_or_else(|e| e.into_inner()); g_wp_dcv.notify_one(); drop(gd);
        }
    }
}
/// Take the pool for a group of `need` tasks. Returns how many pool threads it may use, 0 if it must create its own.
pub(crate) unsafe fn wp_begin(need: i32) -> i32 {
    if need <= 0 { return 0 }
    if g_wp_busy.swap(1, Acquire) != 0 { return 0 }                    // nested or concurrent: the old path
    g_wp_once.call_once(|| unsafe { wp_init() });
    if g_wp.is_null() { g_wp_busy.store(0, Release); return 0; }
    while g_wp_n < need && g_wp_n < WP_MAX {                            // grow
        let s = g_wp.add(g_wp_n as usize);
        (*s).state.store(0, Relaxed);
        if pthread_create(addr_of_mut!((*s).th), wp_main, s as *mut c_void) != 0 { break }
        g_wp_n += 1;
    }
    let avail = if g_wp_n < need { g_wp_n } else { need };
    if avail == 0 { g_wp_busy.store(0, Release); return 0; }
    g_wp_left.store(avail, Relaxed);
    g_wp_groups.fetch_add(1, Relaxed);
    avail
}
pub(crate) unsafe fn wp_post(i: i32, fn_: worker_fn_t, arg: *mut c_void) {
    let s = g_wp.add(i as usize);
    (*s).fn_ = Some(fn_); (*s).arg = arg;
    (*s).state.store(1, Release);
    let gd = (*s).m.lock().unwrap_or_else(|e| e.into_inner()); (*s).cv.notify_one(); drop(gd);
}
/// wait for the group, then free the pool
pub(crate) unsafe fn wp_end() {
    let mut i = 0; while i < WP_SPIN && g_wp_left.load(Acquire) != 0 { cpu_relax(); i += 1; }
    if g_wp_left.load(Acquire) != 0 {
        let mut gd = g_wp_dm.lock().unwrap_or_else(|e| e.into_inner());
        while g_wp_left.load(Acquire) != 0 { gd = g_wp_dcv.wait(gd).unwrap_or_else(|e| e.into_inner()); }
        drop(gd);
    }
    g_wp_owner.store(null_mut(), Relaxed);
    g_wp_busy.store(0, Release);
}
/// Stop and join every pool thread (logos_release_buffers). Not while a group holds the pool.
pub(crate) unsafe fn wp_shutdown() {
    if g_wp_busy.swap(1, Acquire) != 0 { return }
    for i in 0..g_wp_n as usize {
        let s = g_wp.add(i);
        (*s).state.store(2, Release);
        let gd = (*s).m.lock().unwrap_or_else(|e| e.into_inner()); (*s).cv.notify_one(); drop(gd);
    }
    for i in 0..g_wp_n as usize { pthread_join((*g_wp.add(i)).th); (*g_wp.add(i)).state.store(0, Relaxed); }
    g_wp_n = 0;
    g_wp_busy.store(0, Release);
}

// ---- par_run / par_spawn / par_join (C lines 3307-3352) -------------------------------------------------------------------------
/// workers 1..P-1 each on a pool thread (or a new one when the pool is held), worker 0 on this one (run0 = 0: the caller has
/// done worker 0 already), then join.
pub(crate) unsafe fn par_run(th: *mut pthread_t, P: i32, fn_: worker_fn_t, args: *mut c_void, stride: usize, run0: i32) {
    let avail = if P > 1 { wp_begin(P - 1) } else { 0 };
    if avail > 0 {                                                      // the pool: workers 1..avail on its threads, the rest here
        for t in 1..=avail { wp_post(t - 1, fn_, (args as *mut u8).add(t as usize * stride) as *mut c_void); }
        if run0 != 0 { fn_(args); }
        for t in avail + 1..P { fn_((args as *mut u8).add(t as usize * stride) as *mut c_void); }
        wp_end();
        return;
    }
    let mut made: i32 = 0;
    for t in 1..P {
        let w = (args as *mut u8).add(t as usize * stride) as *mut c_void;
        if pthread_create(th.add(made as usize), fn_, w) == 0 { made += 1; }
    }
    if run0 != 0 { fn_(args); }
    for i in 0..made { pthread_join(*th.add(i as usize)); }
}
/// the create loop for workers that DO meet at a barrier. Returns the number of handles to join: th[0 .. made).
pub(crate) unsafe fn par_spawn(th: *mut pthread_t, P: i32, fn_: worker_fn_t, args: *mut c_void, stride: usize, bar: *mut barrier_t, orphan: *mut u8) -> i32 {
    let avail = if P > 1 { wp_begin(P - 1) } else { 0 };
    if avail > 0 {                                                      // the pool: a task with no thread is an orphan
        for t in 1..P {
            if t <= avail { wp_post(t - 1, fn_, (args as *mut u8).add(t as usize * stride) as *mut c_void); }
            else { if !bar.is_null() { barrier_drop(bar); } if !orphan.is_null() { *orphan.add(t as usize) = 1; } }
        }
        g_wp_owner.store(th as *mut c_void, Relaxed);
        return avail;
    }
    let mut made: i32 = 0;
    for t in 1..P {
        if pthread_create(th.add(made as usize), fn_, (args as *mut u8).add(t as usize * stride) as *mut c_void) == 0 { made += 1; }
        else { if !bar.is_null() { barrier_drop(bar); } if !orphan.is_null() { *orphan.add(t as usize) = 1; } }
    }
    made
}
/// the join of a par_spawn group: the pool's group, or `made` created threads
pub(crate) unsafe fn par_join(th: *mut pthread_t, made: i32) {
    if !th.is_null() && g_wp_owner.load(Relaxed) == th as *mut c_void { wp_end(); return; }
    for i in 0..made { pthread_join(*th.add(i as usize)); }
}

// ---- the samplers (C lines 4806-4828) -----------------------------------------------------------------------------------------
pub(crate) unsafe fn sample_read(a: *const f64, n: usize, from: usize, to: usize, mfull: usize, out: *mut f64, has_nan: *mut i32) {
    for i in from..to { let q = SAMPLE_POS(i, mfull, n); _mm_prefetch::<_MM_HINT_NTA>(a.add(if q < n { q } else { n - 1 }) as *const i8); }
    for i in from..to { let q = SAMPLE_POS(i, mfull, n); let v = *a.add(if q < n { q } else { n - 1 }); *out.add(i - from) = v; if v != v { *has_nan = 1; } }
}
/// Sorts a sample in place. NaNs are swapped to the tail first (the count is returned).
pub(crate) unsafe fn sample_sort(s: *mut f64, m: usize) -> usize {
    let mut k = m;
    let mut i = 0usize;
    while i < k { if *s.add(i) != *s.add(i) { let v = *s.add(i); k -= 1; *s.add(i) = *s.add(k); *s.add(k) = v; } else { i += 1; } }
    if k >= 2 && avx2_leaf_on() != 0 && avx2_qsort_f64(s, k) != 0 { return m - k }
    if k >= 2 { quicksort(s, 0, k as i64 - 1, 2 * log2i(k)); }
    m - k
}

// ---- the buffer pool and the scratch (C lines 5573-5681) ------------------------------------------------------------------------
pub(crate) unsafe fn big_alloc(bytes: usize) -> *mut c_void { malloc(bytes) as *mut c_void }
/// HF_RELEASE()
#[inline(always)] pub(crate) unsafe fn hf_release() { free(hf_buf); hf_buf = null_mut(); hf_buf_cap = 0; }
pub(crate) static g_pool_m: pthread_mutex_t = pthread_mutex_t::new();
pub(crate) static mut g_pool_busy: i32 = 0;
pub(crate) static mut g_pool_buf: *mut c_void = null_mut();
pub(crate) static mut g_pool_cap: usize = 0;                           // bytes
pub(crate) static mut g_pool_bidx: *mut u8 = null_mut();
pub(crate) static mut g_pool_bidx_cap: usize = 0;
pub(crate) static mut g_pool_trim: [trim_t; 2] = [trim_t { hw: 0, hi: 0, ep: 0 }; 2];   // buf, bidx
/// acquire: returns 1 if the pooled buffers were handed out (caller must release), 0 if the caller must allocate
pub(crate) unsafe fn pool_acquire(buf_bytes: usize, bidx_bytes: usize, buf: *mut *mut c_void, bidx: *mut *mut u8) -> i32 {
    pthread_mutex_lock(&g_pool_m);
    if g_pool_busy != 0 { pthread_mutex_unlock(&g_pool_m); return 0; }
    if g_pool_cap < buf_bytes { free(g_pool_buf); g_pool_buf = big_alloc(buf_bytes); g_pool_cap = if !g_pool_buf.is_null() { buf_bytes } else { 0 }; trim_reset(addr_of_mut!(g_pool_trim) as *mut trim_t); }
    if bidx_bytes != 0 && g_pool_bidx_cap < bidx_bytes { free(g_pool_bidx); g_pool_bidx = big_alloc(bidx_bytes) as *mut u8; g_pool_bidx_cap = if !g_pool_bidx.is_null() { bidx_bytes } else { 0 }; trim_reset((addr_of_mut!(g_pool_trim) as *mut trim_t).add(1)); }
    if g_pool_buf.is_null() || (bidx_bytes != 0 && g_pool_bidx.is_null()) { pthread_mutex_unlock(&g_pool_m); return 0; }
    trim_note(addr_of_mut!(g_pool_trim) as *mut trim_t, g_pool_buf, g_pool_cap, buf_bytes);
    if bidx_bytes != 0 { trim_note((addr_of_mut!(g_pool_trim) as *mut trim_t).add(1), g_pool_bidx as *mut c_void, g_pool_bidx_cap, bidx_bytes); }
    g_pool_busy = 1; *buf = g_pool_buf; *bidx = if bidx_bytes != 0 { g_pool_bidx } else { null_mut() };
    pthread_mutex_unlock(&g_pool_m);
    1
}
pub(crate) unsafe fn pool_release() { pthread_mutex_lock(&g_pool_m); g_pool_busy = 0; pthread_mutex_unlock(&g_pool_m); }
/// the end of a top-level sort: the pool trim is opt-in (LOGOS_TRIM), so nothing here
pub(crate) unsafe fn trim_idle() {}
pub(crate) unsafe fn logos_release_buffers() {
    pthread_mutex_lock(&g_pool_m);
    if g_pool_busy == 0 {
        free(g_pool_buf); g_pool_buf = null_mut(); g_pool_cap = 0; free(g_pool_bidx); g_pool_bidx = null_mut(); g_pool_bidx_cap = 0;
        hf_release(); ctx_release_all();             // cnti_release_all(): the int32 counting route's tables, not in the f64 port
    }
    pthread_mutex_unlock(&g_pool_m);
    wp_shutdown();                                   // and the worker threads (malloc_trim: no equivalent over the Rust allocator)
}
/// THE SCRATCH, TAKEN WHEN A ROUTE NEEDS IT. One acquisition per sort at most, released once (scratch_drop).
pub(crate) unsafe fn scratch_get(s: *mut scratch_t) -> *mut f64 {
    if (*s).have != 0 { return (*s).dst }
    if (*s).failed != 0 { return null_mut() }
    (*s).pooled = pool_acquire((*s).n * size_of::<f64>(), (*s).bidx_n, addr_of_mut!((*s).pb), addr_of_mut!((*s).pbi));
    (*s).dst = if (*s).pooled != 0 { (*s).pb as *mut f64 } else { big_alloc((*s).n * size_of::<f64>()) as *mut f64 };
    (*s).bidx = if (*s).pooled != 0 { (*s).pbi } else if (*s).bidx_n != 0 { big_alloc((*s).bidx_n) as *mut u8 } else { null_mut() };
    if (*s).dst.is_null() { if (*s).pooled != 0 { pool_release(); } else { free((*s).bidx); } (*s).pooled = 0; (*s).bidx = null_mut(); (*s).failed = 1; return null_mut(); }
    (*s).have = 1; (*s).dst
}
pub(crate) unsafe fn scratch_drop(s: *mut scratch_t) {
    if (*s).have == 0 { return }
    if (*s).pooled != 0 { pool_release(); } else { free((*s).bidx); free((*s).dst); }
    (*s).have = 0; (*s).dst = null_mut(); (*s).bidx = null_mut();
}

#[cfg(test)]
mod tests {
    use super::*;

    struct rng_t(u64);
    impl rng_t {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
    }
    fn bits_sorted(v: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = v.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    /// ascending under `<` over the NaN-free part, and the same multiset of bit patterns
    fn check(out: &[f64], inp: &[f64], what: &str) {
        for i in 1..out.len() { assert!(!(out[i] < out[i - 1]), "{what}: descent at {i} of {}", out.len()); }
        assert_eq!(bits_sorted(out), bits_sorted(inp), "{what}: not a permutation");
    }
    /// shapes: 0 uniform, 1 heavy duplicates, 2 signed zeros and infinities among numbers, 3 sorted, 4 reversed, 5 all equal,
    /// 6 near-sorted, 7 integers
    fn gen(shape: usize, n: usize, r: &mut rng_t) -> Vec<f64> {
        let mut v = vec![0.0f64; n];
        for i in 0..n {
            v[i] = match shape {
                0 => r.unit() * 2e6 - 1e6,
                1 => (r.next() % 7) as f64 - 3.0,
                2 => match r.next() % 8 { 0 => 0.0, 1 => -0.0, 2 => f64::INFINITY, 3 => f64::NEG_INFINITY, 4 => f64::MIN_POSITIVE / 4.0, _ => r.unit() - 0.5 },
                3 => i as f64 * 0.5,
                4 => (n - i) as f64 * 0.5,
                5 => 42.5,
                6 => i as f64 + if r.next() % 50 == 0 { r.unit() * 40.0 - 20.0 } else { 0.0 },
                _ => (r.next() % 1000) as f64 - 500.0,
            };
        }
        v
    }

    #[test]
    fn base_log2i_and_typical() { let _port_guard = crate::port::port_test_lock(); unsafe {
        assert_eq!(log2i(0), 0); assert_eq!(log2i(1), 0); assert_eq!(log2i(2), 1); assert_eq!(log2i(3), 1);
        assert_eq!(log2i(1024), 10); assert_eq!(log2i(1025), 10); assert_eq!(log2i(usize::MAX), 63);
        assert_eq!(ctx_typical(1 << 30, 1_000_000, 100), 2 * 10_000 + 4096);
        assert_eq!(ctx_typical(500, 1_000_000, 100), 500);
        assert_eq!(ctx_typical(1 << 30, 1000, 0), 2 * 1000 + 4096);
    } }

    #[test]
    fn base_small_networks() { let _port_guard = crate::port::port_test_lock(); unsafe {
        let mut r = rng_t(0x9E3779B97F4A7C15);
        for n in 0..=20usize {
            for shape in 0..8 {
                for _ in 0..200 {
                    let inp = gen(shape, n, &mut r); let mut v = inp.clone();
                    net_sort_small(v.as_mut_ptr(), n as i64);
                    if n >= 4 && n <= 16 { check(&v, &inp, "net_sort_small"); }
                    else { assert_eq!(v.iter().map(|x| x.to_bits()).collect::<Vec<_>>(), inp.iter().map(|x| x.to_bits()).collect::<Vec<_>>()); }
                }
            }
            // a NaN: no order promised, but a permutation of the bit patterns
            if n >= 4 && n <= 16 {
                for _ in 0..200 {
                    let mut inp = gen(0, n, &mut r); inp[(r.next() % n as u64) as usize] = f64::from_bits(0x7FF8_0000_0000_1234);
                    let mut v = inp.clone(); net_sort_small(v.as_mut_ptr(), n as i64);
                    assert_eq!(bits_sorted(&v), bits_sorted(&inp));
                }
            }
        }
    } }

    #[test]
    fn base_small_sorts() { let _port_guard = crate::port::port_test_lock(); unsafe {
        let mut r = rng_t(12345);
        for &n in &[0usize, 1, 2, 3, 5, 16, 17, 23, 24, 25, 64, 100, 333, 1000, 5000] {
            for shape in 0..8 {
                let inp = gen(shape, n, &mut r);
                let mut v = inp.clone(); insertion_sort(v.as_mut_ptr(), 0, n as i64 - 1); check(&v, &inp, "insertion_sort");
                let mut v = inp.clone(); heap_sort(v.as_mut_ptr(), 0, n as i64 - 1); check(&v, &inp, "heap_sort");
                let mut v = inp.clone(); quicksort(v.as_mut_ptr(), 0, n as i64 - 1, 2 * log2i(n.max(1))); check(&v, &inp, "quicksort");
                let mut v = inp.clone(); quicksort(v.as_mut_ptr(), 0, n as i64 - 1, 0); check(&v, &inp, "quicksort d=0");
                let mut v = inp.clone(); quicksort(v.as_mut_ptr(), 0, n as i64 - 1, 1); check(&v, &inp, "quicksort d=1");
                let mut v = inp.clone();
                let done = insertion_sort_momentum(v.as_mut_ptr(), 0, n as i64 - 1);
                if done != 0 { check(&v, &inp, "insertion_sort_momentum"); } else { assert_eq!(bits_sorted(&v), bits_sorted(&inp)); }
                if shape == 3 || shape == 5 || shape == 6 { assert_eq!(done, 1, "momentum on shape {shape} n {n}"); }
                if shape == 4 && n >= 100 { assert_eq!(done, 0); }
                // a sub-range: the rest must not move
                if n >= 10 {
                    let mut v = inp.clone(); let (lo, hi) = (3i64, n as i64 - 4);
                    quicksort(v.as_mut_ptr(), lo, hi, 2 * log2i(n));
                    check(&v[lo as usize..=hi as usize], &inp[lo as usize..=hi as usize], "quicksort range");
                    for i in (0..3).chain(n - 3..n) { assert_eq!(v[i].to_bits(), inp[i].to_bits()); }
                }
            }
        }
        // sort5
        for _ in 0..2000 {
            let inp = gen((r.next() % 3) as usize, 9, &mut r); let mut v = inp.clone();
            sort5(v.as_mut_ptr(), 0, 2, 4, 6, 8);
            let picked = [v[0], v[2], v[4], v[6], v[8]];
            check(&picked, &[inp[0], inp[2], inp[4], inp[6], inp[8]], "sort5");
            for i in [1usize, 3, 5, 7] { assert_eq!(v[i].to_bits(), inp[i].to_bits()); }
        }
        // band_local_order
        let asc: Vec<f64> = (0..4096).map(|i| i as f64).collect();
        assert_eq!(band_local_order(asc.as_ptr(), asc.len()), 1);
        let rnd = gen(0, 4096, &mut r);
        assert_eq!(band_local_order(rnd.as_ptr(), rnd.len()), 0);
        let desc: Vec<f64> = (0..2048).map(|i| -(i as f64)).collect();
        assert_eq!(band_local_order(desc.as_ptr(), desc.len()), 0);
    } }

    #[test]
    fn base_lsd_radix_int32() { let _port_guard = crate::port::port_test_lock(); unsafe {
        let mut r = rng_t(777);
        // (low, span): one byte varying, two, three, all four, negative ranges, the extremes
        let cases: [(i64, u64); 9] = [(0, 1), (5, 200), (-100, 200), (0, 60000), (-30000, 70000), (1 << 20, 1 << 24), (i32::MIN as i64, 1u64 << 32), (-7, 3), (256, 256 * 255)];
        for &n in &[1usize, 2, 7, 100, 4095, 4096, 20000] {
            for &(low, span) in &cases {
                let inp: Vec<f64> = (0..n).map(|_| (low + (r.next() % span) as i64) as f64).collect();
                let mut v = inp.clone(); let mut buf = vec![0.0f64; n];
                let mut c: ctx_t = core::mem::zeroed();
                assert_eq!(ctx_alloc(&mut c, n, 0), 1);
                lsd_radix_int32(v.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c);
                ctx_release(&mut c);
                check(&v, &inp, "lsd_radix_int32");
                let mut want = inp.clone(); want.sort_by(|x, y| x.partial_cmp(y).unwrap());
                assert_eq!(v, want);
            }
            let inp: Vec<f64> = (0..n).map(|i| if i & 1 == 0 { i32::MAX as f64 } else { i32::MIN as f64 }).collect();
            let mut v = inp.clone(); let mut buf = vec![0.0f64; n];
            let mut c: ctx_t = core::mem::zeroed(); ctx_alloc(&mut c, n, 0);
            lsd_radix_int32(v.as_mut_ptr(), n, buf.as_mut_ptr(), &mut c); ctx_release(&mut c);
            check(&v, &inp, "lsd_radix_int32 extremes");
        }
    } }

    #[test]
    fn base_samples() { let _port_guard = crate::port::port_test_lock(); unsafe {
        let mut r = rng_t(4242);
        let n = 100_000usize; let a = gen(0, n, &mut r);
        let m = 512usize; let mut out = vec![0.0f64; m]; let mut has_nan = 0i32;
        sample_read(a.as_ptr(), n, 0, m, m, out.as_mut_ptr(), &mut has_nan);
        assert_eq!(has_nan, 0);
        for i in 0..m { assert_eq!(out[i].to_bits(), a[SAMPLE_POS(i, m, n)].to_bits()); }
        let mut out2 = vec![0.0f64; 100];
        sample_read(a.as_ptr(), n, 300, 400, m, out2.as_mut_ptr(), &mut has_nan);
        for i in 0..100 { assert_eq!(out2[i].to_bits(), a[SAMPLE_POS(300 + i, m, n)].to_bits()); }
        let mut b = a.clone(); b[SAMPLE_POS(17, m, n)] = f64::NAN;
        sample_read(b.as_ptr(), n, 0, m, m, out.as_mut_ptr(), &mut has_nan);
        assert_eq!(has_nan, 1);

        // sample_sort on the scalar path: alone, the kernel is a stub
        #[cfg(not(feature = "port_all"))]
        {
            g_avx2 = 0;
            for &m in &[0usize, 1, 2, 3, 30, 384, 2048, 5376] {
                for shape in 0..8 {
                    let inp = gen(shape, m, &mut r); let mut v = inp.clone();
                    assert_eq!(sample_sort(v.as_mut_ptr(), m), 0);
                    check(&v, &inp, "sample_sort");
                    if m >= 30 {
                        let mut inp = inp.clone();
                        let pay = [0x7FF8_0000_0000_0001u64, 0xFFF8_0000_0000_0000, 0x7FF0_0000_0000_0BAD];
                        for (j, p) in pay.iter().enumerate() { inp[j * 7 + (m - 1) * (j & 1) / 2] = f64::from_bits(*p); }
                        let nn = inp.iter().filter(|x| x.is_nan()).count();
                        let mut v = inp.clone();
                        assert_eq!(sample_sort(v.as_mut_ptr(), m), nn);
                        assert_eq!(bits_sorted(&v), bits_sorted(&inp));
                        for i in 1..m - nn { assert!(!(v[i] < v[i - 1])); }
                        for i in m - nn..m { assert!(v[i].is_nan()); }
                    }
                }
            }
            g_avx2 = -1;
        }
    } }

    // one test for everything that touches the persistent context table and the buffer pool (one sort at a time is their contract)
    #[test]
    fn base_contexts_pool_scratch() { let _port_guard = crate::port::port_test_lock(); unsafe {
        ctx_release_all();
        let mut c: [ctx_t; 3] = core::mem::zeroed();
        assert_eq!(ctx_init(&mut c[0], 1000, 0), 1);
        assert_eq!(ctx_init(&mut c[1], 2000, 1), 1);
        assert_eq!(g_ctx_next, 2);
        assert!(c[0].pooled == 1 && c[0].pool_ix == 0 && c[0].cap == 1000 + 250 + 64 && c[0].wc.is_null() && c[0].C[0].is_null());
        assert!(c[1].pooled == 1 && c[1].pool_ix == 1 && !c[1].wc.is_null() && !c[1].wcn.is_null() && c[1].wc_on == 1);
        for i in 0..=c[0].cap { *c[0].aux.add(i) = i as u32; }
        for i in 0..c[0].cap { *c[0].idx.add(i) = i as u16; }
        assert_eq!(ctx_coarse_ready(&mut c[0]), 1);
        assert!(!c[0].C[0].is_null() && c[0].C[0] == g_ctx_pool[0].C[0] && !c[0].land[COARSE_MAXD - 1].is_null());
        assert_eq!(ctx_coarse_ready(&mut c[0]), 1);
        // a borrowed fit: the jumbo, then (jumbo held) a temporary
        let mut f0: ctx_fit_t = core::mem::zeroed(); let mut f1: ctx_fit_t = core::mem::zeroed();
        assert_eq!(ctx_fit(&mut c[0], 500, &mut f0), 1); assert_eq!(f0.kind, 0);
        let old = (c[0].aux, c[0].cap);
        assert_eq!(ctx_fit(&mut c[0], 50_000, &mut f0), 1); assert_eq!(f0.kind, 1); assert!(c[0].cap >= 50_000 && c[0].aux == g_jumbo_aux);
        assert_eq!(ctx_fit(&mut c[1], 60_000, &mut f1), 1); assert_eq!(f1.kind, 2); assert_eq!(c[1].cap, 60_000);
        *c[1].aux.add(60_000) = 7; *c[1].idx.add(59_999) = 7;
        ctx_unfit(&mut c[1], &mut f1); ctx_unfit(&mut c[0], &mut f0);
        assert!(c[0].aux == old.0 && c[0].cap == old.1);
        assert_eq!(ctx_fit(&mut c[1], 60_000, &mut f1), 1); assert_eq!(f1.kind, 1); ctx_unfit(&mut c[1], &mut f1);
        let mut z: ctx_t = core::mem::zeroed();
        assert_eq!(ctx_fit(&mut z, 10, &mut f0), 0);
        ctx_free(&mut c[0]); ctx_free(&mut c[1]);
        // the next sort: the same bundles, with the coarse tables already there; a larger request grows the bundle
        g_ctx_next = 0;
        assert_eq!(ctx_init(&mut c[2], 900, 1), 1);
        assert!(c[2].pool_ix == 0 && c[2].aux == old.0 && !c[2].C[0].is_null() && !c[2].wc.is_null());
        g_ctx_next = 0;
        assert_eq!(ctx_init(&mut c[2], 100_000, 0), 1);
        assert!(c[2].cap >= 100_000 && c[2].C[0].is_null());
        // past the table: a private context, freed by ctx_free
        g_ctx_next = CTX_POOL_MAX as i32;
        let mut p: ctx_t = core::mem::zeroed();
        assert_eq!(ctx_init(&mut p, 64, 0), 1); assert!(p.pooled == 0 && p.pool_ix == -1 && p.cap == 64);
        assert_eq!(ctx_coarse_ready(&mut p), 1); assert!(!p.H[0].is_null());
        ctx_free(&mut p);
        g_ctx_next = 0;

        // the buffer pool and the scratch
        let mut s: scratch_t = core::mem::zeroed(); s.n = 10_000; s.bidx_n = 10_000;
        let d = scratch_get(&mut s);
        assert!(!d.is_null() && s.have == 1 && s.pooled == 1 && !s.bidx.is_null() && scratch_get(&mut s) == d);
        for i in 0..s.n { *d.add(i) = i as f64; *s.bidx.add(i) = i as u8; }
        let mut s2: scratch_t = core::mem::zeroed(); s2.n = 5000;                       // the pool is held: its own blocks
        let d2 = scratch_get(&mut s2);
        assert!(!d2.is_null() && d2 != d && s2.pooled == 0 && s2.bidx.is_null());
        *d2.add(4999) = 1.0;
        scratch_drop(&mut s2); assert!(s2.have == 0 && s2.dst.is_null());
        scratch_drop(&mut s); scratch_drop(&mut s);
        assert_eq!(g_pool_busy, 0);
        let mut s3: scratch_t = core::mem::zeroed(); s3.n = 100;
        let d3 = scratch_get(&mut s3);
        assert!(d3 == d && s3.pooled == 1 && s3.bidx.is_null());                        // the same block, no bucket index asked for
        scratch_drop(&mut s3);
        let (mut pb, mut pbi): (*mut c_void, *mut u8) = (null_mut(), null_mut());
        assert_eq!(pool_acquire(64, 0, &mut pb, &mut pbi), 1);
        assert_eq!(pool_acquire(64, 0, &mut pb, &mut pbi), 0);
        pool_release();
        trim_idle();
        logos_release_buffers();
        assert!(g_pool_buf.is_null() && g_pool_cap == 0 && g_jumbo_aux.is_null() && g_ctx_pool_used[0] == 0 && g_ctx_next == 0);
    } }

    // ---- the worker pool ------------------------------------------------------------------------------------------------------
    #[repr(C)]
    struct warg_t { id: i32, P: i32, hits: *const AtomicUsize, sum: *const AtomicUsize, bar: *mut barrier_t, phase: *const AtomicI32, bad: *const AtomicI32, nest: i32, pad: [u64; 3] }
    unsafe fn w_plain(p: *mut c_void) -> *mut c_void {
        let w = &*(p as *const warg_t);
        (*w.hits.add(w.id as usize)).fetch_add(1, SeqCst);
        (*w.sum).fetch_add(w.id as usize + 1, SeqCst);
        if w.nest != 0 {                                                // a nested group: the pool is held, so threads are created
            let hits: Vec<AtomicUsize> = (0..4).map(|_| AtomicUsize::new(0)).collect(); let sum = AtomicUsize::new(0);
            let mut args: Vec<warg_t> = (0..4).map(|t| warg_t { id: t, P: 4, hits: hits.as_ptr(), sum: &sum, bar: null_mut(), phase: null(), bad: w.bad, nest: 0, pad: [0; 3] }).collect();
            let mut th: [pthread_t; 4] = core::mem::zeroed();
            par_run(th.as_mut_ptr(), 4, w_plain, args.as_mut_ptr() as *mut c_void, size_of::<warg_t>(), 1);
            if sum.load(SeqCst) != 10 || hits.iter().any(|h| h.load(SeqCst) != 1) { (*w.bad).fetch_add(1, SeqCst); }
        }
        null_mut()
    }
    /// three phases separated by barriers: nobody may enter phase k + 1 before everyone has finished phase k
    unsafe fn w_barrier(p: *mut c_void) -> *mut c_void {
        let w = &*(p as *const warg_t);
        for k in 0..3 {
            if (*w.phase.add(k)).load(SeqCst) != 0 && k == 0 { /* phase counters start at zero or mid-count: nothing to assert yet */ }
            if k > 0 && (*w.phase.add(k - 1)).load(SeqCst) != w.P { (*w.bad).fetch_add(1, SeqCst); }
            (*w.hits.add(w.id as usize)).fetch_add(1, SeqCst);
            (*w.phase.add(k)).fetch_add(1, SeqCst);
            barrier_wait(w.bar);
            if (*w.phase.add(k)).load(SeqCst) != w.P { (*w.bad).fetch_add(1, SeqCst); }
        }
        null_mut()
    }
    #[test]
    fn base_par_run() { let _port_guard = crate::port::port_test_lock(); unsafe {
        for round in 0..6 {
            for P in 1..=16i32 {
                for run0 in [1i32, 0] {
                    let hits: Vec<AtomicUsize> = (0..P).map(|_| AtomicUsize::new(0)).collect(); let sum = AtomicUsize::new(0); let bad = AtomicI32::new(0);
                    let nest = (round == 3 && P % 5 == 0) as i32;
                    let mut args: Vec<warg_t> = (0..P).map(|t| warg_t { id: t, P, hits: hits.as_ptr(), sum: &sum, bar: null_mut(), phase: null(), bad: &bad, nest: (nest != 0 && t == 1) as i32, pad: [0; 3] }).collect();
                    let mut th: [pthread_t; 16] = core::mem::zeroed();
                    par_run(th.as_mut_ptr(), P, w_plain, args.as_mut_ptr() as *mut c_void, size_of::<warg_t>(), run0);
                    for t in 0..P as usize { assert_eq!(hits[t].load(SeqCst), if t == 0 && run0 == 0 { 0 } else { 1 }, "P {P} worker {t}"); }
                    let want = (P * (P + 1) / 2) as usize - if run0 == 0 { 1 } else { 0 };
                    assert_eq!(sum.load(SeqCst), want);
                    assert_eq!(bad.load(SeqCst), 0);
                }
            }
            if round == 4 { wp_shutdown(); }                           // a cold pool must grow again
        }
    } }
    #[test]
    fn base_par_spawn_barrier() { let _port_guard = crate::port::port_test_lock(); unsafe {
        for round in 0..6 {
            for P in 1..=16i32 {
                let hits: Vec<AtomicUsize> = (0..P).map(|_| AtomicUsize::new(0)).collect(); let sum = AtomicUsize::new(0); let bad = AtomicI32::new(0);
                let phase = [AtomicI32::new(0), AtomicI32::new(0), AtomicI32::new(0)];
                let mut bar: barrier_t = core::mem::zeroed();
                barrier_init(&mut bar, P);
                let mut args: Vec<warg_t> = (0..P).map(|t| warg_t { id: t, P, hits: hits.as_ptr(), sum: &sum, bar: &mut bar, phase: phase.as_ptr(), bad: &bad, nest: 0, pad: [0; 3] }).collect();
                let mut th: [pthread_t; 16] = core::mem::zeroed();
                let mut orphan = [0u8; 16];
                let made = par_spawn(th.as_mut_ptr(), P, w_barrier, args.as_mut_ptr() as *mut c_void, size_of::<warg_t>(), &mut bar, orphan.as_mut_ptr());
                assert_eq!(made, P - 1); assert!(orphan.iter().all(|&o| o == 0));
                w_barrier(args.as_mut_ptr() as *mut c_void);
                par_join(th.as_mut_ptr(), made);
                barrier_destroy(&mut bar);
                assert_eq!(bad.load(SeqCst), 0, "round {round} P {P}");
                for t in 0..P as usize { assert_eq!(hits[t].load(SeqCst), 3); }
                for k in 0..3 { assert_eq!(phase[k].load(SeqCst), P); }
            }
        }
        // a barrier with a participant dropped before anyone arrives
        let mut bar: barrier_t = core::mem::zeroed();
        barrier_init(&mut bar, 2); barrier_drop(&mut bar);
        barrier_wait(&mut bar); barrier_wait(&mut bar);
        assert_eq!(bar.gen.load(SeqCst), 2);
        // pthread_mutex: lock / trylock / unlock
        let m = pthread_mutex_t::new();
        assert_eq!(pthread_mutex_trylock(&m), 0); assert_ne!(pthread_mutex_trylock(&m), 0);
        pthread_mutex_unlock(&m); pthread_mutex_lock(&m); pthread_mutex_unlock(&m);
    } }
}

// ---- outside this file's range ----------------------------------------------------------------------------------------------
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    /// C line 1556 (the leaf file): the AVX2 quicksort, 0 = declined (a NaN).
    pub(crate) unsafe fn avx2_qsort_f64(a: *mut f64, n: usize) -> i32 { unimplemented!() }
    /// C line 3895 (the partition file): the histogram-free partition's persistent region buffer.
    pub(crate) static mut hf_buf: *mut f64 = null_mut();
    pub(crate) static mut hf_buf_cap: usize = 0;
}
#[cfg(not(feature = "port_all"))]
use missing::*;
