//! The parallel engine: a persistent worker pool, and the parallel forms of the routes.
//!
//!   prescan        a 64K head on the caller, the rest in chunks
//!   few values     per-thread vector counts, the emit split by output position
//!   lattice        per-thread count tables over claimed chunks, one merge, a split emit
//!   runs           per-chunk detection that compacts into scratch, one plan (the knot sweep, the
//!                  sorted strays as one more strand), placement jobs split by merge path
//!   partition      histogram-free: sample quantiles as splitters, a direct-index bucket map,
//!                  each bucket a region sized from the sample with slack, 256-element blocks
//!                  reserved with one atomic each, overflow spilled; then the kernel per bucket
//!
//! Every phase hands out work through a shared counter and the calling thread takes part, so a
//! worker that could not be started, or a pool that another sort is using, costs speed only.

use crate::engine::{self, took, trace, trace_start, Job, Lattice, Plan, Route, Run};
use crate::kernel;
use crate::pool;
use core::arch::x86_64::*;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::*};
use std::sync::{Arc, Condvar, Mutex, OnceLock};

// ---------------------------------------------------------------------------------------------
// thread policy

const MIN_PER_THREAD: usize = 100_000;
const BOOST_PER: usize = 25_000;
const BOOST_MIN_N: usize = 300_000;
const BUCKETS_PER_THREAD: usize = 12;
const MAX_BUCKETS: usize = 256;

/// At most one thread per 100,000 elements (the C engine's policy and constant).
pub(crate) fn effective_threads(n: usize, threads: usize) -> usize {
    threads.min((n / MIN_PER_THREAD).max(1))
}
/// The unstructured partition's own floor: one thread per 25,000 elements from 300,000.
fn boost_threads(n: usize, p: usize, asked: usize) -> usize {
    if n < BOOST_MIN_N { p } else { p.max(asked.min(n / BOOST_PER)) }
}

// ---------------------------------------------------------------------------------------------
// the worker pool

#[derive(Clone, Copy)]
struct JobPtr(*const (dyn Fn(usize) + Sync));
unsafe impl Send for JobPtr {}

struct State {
    job: Option<JobPtr>,
    next: usize,
    limit: usize,
    running: usize,
    quit: bool,
    workers: usize,
}
struct Shared {
    m: Mutex<State>,
    work: Condvar,
    done: Condvar,
}
static POOL: OnceLock<Arc<Shared>> = OnceLock::new();
static BUSY: AtomicBool = AtomicBool::new(false);
/// Counts posted phases. A worker with nothing to do watches it for a moment before it sleeps: a
/// sort is several phases a few hundred microseconds apart, and a sleeping thread takes tens of
/// microseconds to wake (at 1M elements that was a tenth of a counting sort).
static GEN: AtomicUsize = AtomicUsize::new(0);
const SPIN: usize = 2000;
static HANDLES: Mutex<Vec<std::thread::JoinHandle<()>>> = Mutex::new(Vec::new());

fn shared() -> &'static Arc<Shared> {
    POOL.get_or_init(|| {
        Arc::new(Shared {
            m: Mutex::new(State { job: None, next: 0, limit: 0, running: 0, quit: false, workers: 0 }),
            work: Condvar::new(),
            done: Condvar::new(),
        })
    })
}

fn worker(sh: Arc<Shared>) {
    let mut st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
    let mut seen = usize::MAX;
    loop {
        while !st.quit && !(st.job.is_some() && st.next < st.limit) {
            let g = GEN.load(Relaxed);
            if g != seen {
                seen = g;
                drop(st);
                for _ in 0..SPIN {
                    if GEN.load(Relaxed) != g { break; }
                    core::hint::spin_loop();
                }
                st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
            } else {
                st = sh.work.wait(st).unwrap_or_else(|e| e.into_inner());
            }
        }
        if st.quit {
            st.workers -= 1;
            return;
        }
        let (i, job) = (st.next, st.job.unwrap());
        st.next += 1;
        st.running += 1;
        drop(st);
        // SAFETY: `run` keeps the closure alive until `running` is back to zero.
        unsafe { (*job.0)(i) };
        st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
        st.running -= 1;
        if st.running == 0 && st.next >= st.limit { sh.done.notify_all(); }
    }
}

/// Runs f(0), ..., f(p - 1), each exactly once, on up to p threads; returns when all are done.
/// Indices are CLAIMED, by the workers and by the caller alike: with no worker at all the caller
/// runs every one. If another sort holds the pool, this one runs sequentially.
pub(crate) fn run(p: usize, f: &(dyn Fn(usize) + Sync)) {
    if p <= 1 || BUSY.swap(true, Acquire) {
        for t in 0..p { f(t); }
        return;
    }
    let sh = shared();
    {
        let mut st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
        while st.workers + 1 < p {
            let c = sh.clone();
            match std::thread::Builder::new().name("logos-sort".into()).spawn(move || worker(c)) {
                Ok(h) => {
                    st.workers += 1;
                    HANDLES.lock().unwrap_or_else(|e| e.into_inner()).push(h);
                }
                Err(_) => break,
            }
        }
        // SAFETY: only the lifetime is erased; this function does not return while a worker runs f.
        let jp: *const (dyn Fn(usize) + Sync) = unsafe { core::mem::transmute(f) };
        st.job = Some(JobPtr(jp));
        st.next = 1;
        st.limit = p;
        st.running = 0;
        GEN.fetch_add(1, Release);
    }
    sh.work.notify_all();
    f(0);
    let mut st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
    loop {
        if st.next < st.limit {
            let i = st.next;
            st.next += 1;
            drop(st);
            f(i);
            st = sh.m.lock().unwrap_or_else(|e| e.into_inner());
        } else if st.running > 0 {
            st = sh.done.wait(st).unwrap_or_else(|e| e.into_inner());
        } else {
            break;
        }
    }
    st.job = None;
    drop(st);
    BUSY.store(false, Release);
}

/// Stops the pool's threads (they are started again by the next parallel sort).
pub(crate) fn shutdown() {
    if let Some(sh) = POOL.get() {
        sh.m.lock().unwrap_or_else(|e| e.into_inner()).quit = true;
        sh.work.notify_all();
        let hs: Vec<_> = HANDLES.lock().unwrap_or_else(|e| e.into_inner()).drain(..).collect();
        for h in hs { let _ = h.join(); }
        sh.m.lock().unwrap_or_else(|e| e.into_inner()).quit = false;
    }
}

#[derive(Clone, Copy)]
struct Ptr<T>(*mut T);
unsafe impl<T> Send for Ptr<T> {}
unsafe impl<T> Sync for Ptr<T> {}

// ---------------------------------------------------------------------------------------------
// prescan

const PRESCAN_HEAD: usize = 1 << 16;
/// A chunk leaves descents this close to a cut for the pass over the cuts (a repair window is 24).
const SEAM: usize = 24;

/// True when the array was already sorted but for kinks (which are now repaired), or was
/// non-increasing and has been reversed.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn prescan(a: &mut [f64], p: usize) -> bool {
    let n = a.len();
    let head = n.min(PRESCAN_HEAD);
    let ar = Ptr(a.as_mut_ptr());
    let rest = n - head;
    if engine::scan_repair(ar.0, 0, head, n, SEAM) {
        if rest > 0 {
            let ok = AtomicBool::new(true);
            run(p, &|t| {
                let ar = ar;
                let (lo, hi) = (head + rest * t / p, head + rest * (t + 1) / p);
                if lo < hi && !engine::scan_repair(ar.0, lo, hi, n, SEAM) { ok.store(false, Relaxed); }
            });
            if !ok.load(Relaxed) { return false; }
            // every cut: the zone around it, where the chunks left descents alone, scanned whole
            for t in 0..p {
                let c = head + rest * t / p;
                if c < n && !engine::scan_repair(ar.0, c.saturating_sub(2 * SEAM), (c + 2 * SEAM).min(n), n, 0) { return false; }
            }
        }
        took(Route::Sorted);
        return true;
    }
    if engine::first_ascent(&a[..head]) != head { return false; }
    if rest > 0 {
        let ok = AtomicBool::new(true);
        run(p, &|t| {
            let ar = ar;
            // chunk t of the rest, with the element before it, so the pair across the cut is read
            let (lo, hi) = (head + rest * t / p - 1, head + rest * (t + 1) / p);
            let s = core::slice::from_raw_parts(ar.0.add(lo) as *const f64, hi - lo);
            if engine::first_ascent(s) != s.len() { ok.store(false, Relaxed); }
        });
        if !ok.load(Relaxed) { return false; }
    }
    let half = n / 2;
    run(p, &|t| {
        let ar = ar;
        for i in half * t / p..half * (t + 1) / p { core::ptr::swap(ar.0.add(i), ar.0.add(n - 1 - i)); }
    });
    took(Route::Reversed);
    true
}

// ---------------------------------------------------------------------------------------------
// the dispatcher

#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn sort(a: &mut [f64], p: usize, asked: usize) {
    let n = a.len();
    let s = match engine::sample(a, 2048) {
        Some(s) => s,
        None => {
            let k = engine::nans_last(a);
            engine::sort(&mut a[..k], asked);
            return took(Route::NanLast);
        }
    };
    if s.distinct.len() <= 8 && count_few(a, &s.distinct, p) { return took(Route::FewValues); }
    if let Some(l) = engine::lattice_gate(&s.distinct, &s.sorted, n, p) {
        if lattice_sort(a, &l, p) { return took(Route::Lattice); }
    }
    if runs_sort(a, p) { return; }
    if reach_sort(a, p) { return took(Route::Reach); }
    if p == 2 && halves_sort(a, s.sorted[s.m / 2]) { return took(Route::Halves); }
    let ints = engine::all_int32(&s.distinct);
    partition_sort(a, boost_threads(n, p, asked), ints);
    took(Route::Partition);
}

// ---------------------------------------------------------------------------------------------
// a handful of values

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn count_few(a: &mut [f64], vals: &[f64], p: usize) -> bool {
    let n = a.len();
    let ar = Ptr(a.as_mut_ptr());
    let total = Mutex::new([0usize; 8]);
    run(p, &|t| {
        let ar = ar;
        let (lo, hi) = (n * t / p, n * (t + 1) / p);
        let mut c = [0usize; 8];
        engine::count_few_range(core::slice::from_raw_parts(ar.0.add(lo) as *const f64, hi - lo), vals, &mut c);
        let mut g = total.lock().unwrap_or_else(|e| e.into_inner());
        for j in 0..8 { g[j] += c[j]; }
    });
    let cnt = *total.lock().unwrap_or_else(|e| e.into_inner());
    if cnt.iter().sum::<usize>() != n { return false; }
    // value j occupies output [start[j], start[j + 1]); each thread fills its share of the positions
    let mut start = [0usize; 9];
    for j in 0..8 { start[j + 1] = start[j] + cnt[j]; }
    let nt = n >= engine::NT_MIN_N;
    run(p, &|t| {
        let ar = ar;
        let (lo, hi) = (n * t / p, n * (t + 1) / p);
        for j in 0..vals.len() {
            let (s, e) = (start[j].max(lo), start[j + 1].min(hi));
            if s < e { engine::fill(ar.0.add(s), e - s, vals[j], nt); }
        }
        if nt { _mm_sfence(); }
    });
    true
}

// ---------------------------------------------------------------------------------------------
// the lattice route

const LAT_CHUNK: usize = 1 << 16;

#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn lattice_sort(a: &mut [f64], l: &Lattice, p: usize) -> bool {
    let n = a.len();
    let ar = Ptr(a.as_mut_ptr());
    let next = AtomicUsize::new(0);
    let failed = AtomicBool::new(false);
    let tables: Mutex<Vec<(Vec<u32>, Vec<f64>)>> = Mutex::new(Vec::new());
    let cap = n / 32;
    // the map: chunks are claimed, so a slower core takes fewer of them
    run(p, &|_| {
        let ar = ar;
        let mut cnt = vec![0u32; l.slots];
        let mut spill = Vec::new();
        loop {
            let c = next.fetch_add(1, Relaxed);
            let lo = c * LAT_CHUNK;
            if lo >= n || failed.load(Relaxed) { break; }
            let hi = (lo + LAT_CHUNK).min(n);
            if !engine::lattice_map(core::slice::from_raw_parts(ar.0.add(lo) as *const f64, hi - lo), l, cnt.as_mut_ptr(), &mut spill, cap) {
                failed.store(true, Relaxed);
                break;
            }
        }
        tables.lock().unwrap_or_else(|e| e.into_inner()).push((cnt, spill));
    });
    if failed.load(Relaxed) { return false; }
    let mut tables = tables.into_inner().unwrap_or_else(|e| e.into_inner());
    let (mut cnt, mut spill) = tables.pop().unwrap();
    for (c, s) in &tables {
        for i in 0..l.slots { cnt[i] += c[i]; }
        spill.extend_from_slice(s);
    }
    if spill.len() > cap { return false; }
    let nt = n >= engine::NT_MIN_N;
    if !spill.is_empty() { engine::sort(&mut spill, 1); }
    // the emit: slot ranges that hold about 1/p of the lattice's elements each, and with each range
    // the spills whose values fall inside it
    let on = n - spill.len();
    let mut cut = vec![0usize; p + 1];
    let mut off = vec![0usize; p + 1];
    let (mut acc, mut t) = (0usize, 1usize);
    for s in 0..l.slots {
        while t < p && acc >= on * t / p {
            cut[t] = s;
            off[t] = acc;
            t += 1;
        }
        acc += cnt[s] as usize;
    }
    while t <= p {
        cut[t] = l.slots;
        off[t] = acc;
        t += 1;
    }
    let mut sp = vec![0usize; p + 1];
    for t in 1..p {
        let v = (cut[t] as f64).mul_add(l.step, l.base);
        sp[t] = spill.partition_point(|&x| x < v);
    }
    sp[p] = spill.len();
    let cp = Ptr(cnt.as_mut_ptr());
    let spill = &spill;
    run(p, &|t| {
        let (ar, cp) = (ar, cp);
        engine::lattice_emit(ar.0.add(off[t] + sp[t]), l, cp.0, cut[t], cut[t + 1], &spill[sp[t]..sp[t + 1]], nt);
        if nt { _mm_sfence(); }
    });
    true
}

// ---------------------------------------------------------------------------------------------
// the run routes

/// A job longer than this is cut into several (by merge path), so that one long strand does not
/// leave the placement on one thread.
fn split_jobs(src: *const f64, plan: &mut Plan, max_len: usize) {
    let jobs = core::mem::take(&mut plan.jobs);
    for j in jobs {
        if j.len <= max_len {
            plan.jobs.push(j);
            continue;
        }
        let pc: Vec<(usize, usize)> = plan.pieces[j.first..j.last].to_vec();
        let big = (0..pc.len()).max_by_key(|&q| pc[q].1).unwrap();
        let parts = (j.len + max_len - 1) / max_len;
        let mut done = vec![0usize; pc.len()];
        let mut out = j.out;
        for q in 1..=parts {
            let first = plan.pieces.len();
            let mut total = 0;
            // the largest piece is cut by index, the others below the value that stands there
            let bcut = if q == parts { pc[big].1 } else { pc[big].1 * q / parts };
            for k in 0..pc.len() {
                let c = if k == big || q == parts { if k == big { bcut } else { pc[k].1 } } else {
                    let v = unsafe { *src.add(pc[big].0 + bcut) };
                    let s = unsafe { core::slice::from_raw_parts(src.add(pc[k].0), pc[k].1) };
                    done[k] + s[done[k]..].partition_point(|&x| x < v)
                };
                if c > done[k] {
                    plan.pieces.push((pc[k].0 + done[k], c - done[k]));
                    total += c - done[k];
                    done[k] = c;
                }
            }
            if total > 0 {
                plan.jobs.push(Job { first, last: plan.pieces.len(), out, len: total });
                out += total;
            }
        }
    }
}

/// Detection per chunk into the scratch (which is also the first of the two copies), one plan,
/// placement into the array. On one thread this IS the sequential run route.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn runs_sort(a: &mut [f64], p: usize) -> bool {
    let n = a.len();
    #[allow(unused_mut)]
    let mut t0 = trace_start!();
    // Strays may come to an eighth of the array, whichever chunks they are in: the unsorted tail after
    // a sorted prefix is most of ONE chunk, and that chunk's detector stops at it and hands the rest
    // over as strays. The shared count is what keeps noise from being copied: its second chunk fails.
    let budget = n / 8;
    let stray_total = AtomicUsize::new(0);
    let spare = budget + 64 * p + 64;
    let buf = pool::acquire((n + spare) * 8);
    let scr = Ptr(buf.ptr as *mut f64);
    let ar = Ptr(a.as_mut_ptr());
    let failed = AtomicBool::new(false);
    let nt = n >= engine::NT_MIN_N;
    let found: Mutex<Vec<(usize, usize, Vec<Run>, Vec<f64>)>> = Mutex::new(Vec::new());
    // detection, per chunk: the kept elements go to the same place in the scratch, compacted
    run(p, &|t| {
        let (ar, scr) = (ar, scr);
        let (lo, hi) = (n * t / p, n * (t + 1) / p);
        if failed.load(Relaxed) { return; }
        let (scanned, runs, strays) = match engine::detect_runs_to(ar.0.add(lo) as *const f64, hi - lo, scr.0.add(lo), nt, budget) {
            Some(d) => (d.scanned, d.runs.iter().map(|r| Run { start: r.start + lo, len: r.len }).collect(), d.strays),
            None => (0, Vec::new(), Vec::new()),
        };
        if nt { _mm_sfence(); }
        // nothing unread is copied yet: noise would have one chunk copy a fourteenth of the array
        // before the count said no (uniform at 14 threads read 0.88 of the C engine for that)
        let mine = strays.len() + (hi - lo - scanned);
        if stray_total.fetch_add(mine, Relaxed) + mine > budget { failed.store(true, Relaxed); }
        found.lock().unwrap_or_else(|e| e.into_inner()).push((lo, lo + scanned, runs, strays));
    });
    if failed.load(Relaxed) {
        pool::release(buf);
        return false;
    }
    let mut found = found.into_inner().unwrap_or_else(|e| e.into_inner());
    found.sort_unstable_by_key(|f| f.0);
    let mut runs: Vec<Run> = Vec::new();
    let mut strays: Vec<f64> = Vec::new();
    let ends: Vec<usize> = found.iter().skip(1).map(|f| f.0).chain([n]).collect();
    for ((_, read, r, s), end) in found.into_iter().zip(ends) {
        runs.extend(r);
        strays.extend(s);
        strays.extend_from_slice(core::slice::from_raw_parts(ar.0.add(read) as *const f64, end - read));   /* what its detector left unread */
    }
    if strays.len() > spare || runs.is_empty() {
        pool::release(buf);
        return false;
    }
    // the sorted strays are one more strand, kept past the end of the scratch
    trace!(t0, "runs: detected {} runs, {} strays of {}", runs.len(), strays.len(), n);
    if !strays.is_empty() {
        engine::sort(&mut strays, p);
        core::ptr::copy_nonoverlapping(strays.as_ptr(), scr.0.add(n), strays.len());
        runs.push(Run { start: n, len: strays.len() });
    }
    took(if runs.len() == 1 + !strays.is_empty() as usize { Route::OneStrand } else { Route::Runs });
    let mut plan = match engine::plan_runs(scr.0, &runs) {
        Some(pl) => pl,
        None => {
            pool::release(buf);
            return false;
        }
    };
    if p > 1 { split_jobs(scr.0, &mut plan, (n / (4 * p)).max(1 << 15)); }
    trace!(t0, "runs: strays sorted, {} jobs over {} pieces planned", plan.jobs.len(), plan.pieces.len());
    let next = AtomicUsize::new(0);
    run(p, &|_| {
        let (ar, scr) = (ar, scr);
        loop {
            let j = next.fetch_add(1, Relaxed);
            if j >= plan.jobs.len() { break; }
            engine::place_job(scr.0, &plan, &plan.jobs[j], ar.0, nt);
        }
        if nt { _mm_sfence(); }
    });
    trace!(t0, "runs: placed");
    pool::release(buf);
    true
}

// ---------------------------------------------------------------------------------------------
// the partition

/// The bucket map: for a value, how many splitters are not greater than it. A grid of equal-width
/// cells over the sample's range; a cell no splitter falls into names its bucket outright, a cell
/// with a few splitters is settled by comparing against them, and a cell crowded with splitters
/// (a mode, the dense end of a heavy tail) carries a finer grid of its own over just their range.
/// Uniform data pays one multiply and one load; clusters and tails pay two or three.
struct Grid {
    nodes: Vec<Node>,
    tab: Vec<u32>,
    s: Vec<f64>,
    /// When the root does nothing but hand every value to one of two to four finer grids (modes
    /// far apart, each narrow): the first splitter of modes 1.., and each mode's grid. A lookup
    /// then picks its mode with up to three compares and skips the root.
    nmodes: usize,
    bound: [f64; 3],
    start: [u32; 4],
}
#[derive(Clone, Copy)]
struct Node {
    lo: f64,
    scale: f64,
    top: f64,
    klo: u64,
    shift: u32,
    ktop: u64,
    key: bool,
    base: u32,
}
const G_SCAN: u32 = 1 << 30;
const G_CHILD: u32 = 2 << 30;
const G_ROOT_CELLS: usize = 4096;
const G_DEPTH: u32 = 4;

impl Node {
    /// Monotone in v; a NaN lands in an end cell. Cells are equal widths of VALUE, or (key) equal
    /// widths of the total-order key, which is close to equal widths of the logarithm: a heavy tail
    /// that crowds one end of a value grid spreads out over a key grid.
    #[inline(always)]
    unsafe fn cell(&self, v: f64) -> usize {
        if self.key { self.cell_in::<true>(v) } else { self.cell_in::<false>(v) }
    }
    /// The same with the space fixed at compile time, for the scatter loop.
    #[inline(always)]
    unsafe fn cell_in<const KEY: bool>(&self, v: f64) -> usize {
        if KEY {
            (engine::tkey(v).saturating_sub(self.klo) >> self.shift).min(self.ktop) as usize
        } else {
            let t = _mm_mul_sd(_mm_sub_sd(_mm_set_sd(v), _mm_set_sd(self.lo)), _mm_set_sd(self.scale));
            _mm_cvttsd_si64(_mm_min_sd(_mm_max_sd(t, _mm_setzero_pd()), _mm_set_sd(self.top))) as usize
        }
    }
}
impl Grid {
    unsafe fn build(s: &[f64], lo: f64, hi: f64, key: bool) -> Grid {
        let mut g = Grid { nodes: Vec::new(), tab: Vec::new(), s: s.to_vec(), nmodes: 0, bound: [f64::INFINITY; 3], start: [0; 4] };
        let k = g.s.len();
        g.node(0, k, lo, hi, G_ROOT_CELLS, 0, key);
        // the root's children, in order, if they are all it has: every splitter lies in one of them
        let root = g.nodes[0];
        let kids: Vec<u32> = g.tab[..root.top as usize + 1].iter().filter(|&&e| e >= G_CHILD).map(|&e| e & (G_SCAN - 1)).collect();
        if !key && (2..=4).contains(&kids.len()) {
            // splitters under a child: those in its cell of the root
            let mut firsts = Vec::new();
            let (mut covered, mut j) = (0, 0);
            for c in 0..root.top as usize + 1 {
                let j0 = j;
                while j < k && root.cell(g.s[j]) == c { j += 1; }
                if g.tab[c] >= G_CHILD {
                    firsts.push(g.s[j0]);
                    covered += j - j0;
                }
            }
            if covered == k {
                g.nmodes = kids.len();
                for (q, &kid) in kids.iter().enumerate() {
                    g.start[q] = kid;
                    if q > 0 { g.bound[q - 1] = firsts[q]; }
                }
            }
        }
        g
    }
    /// A grid of at most `cells` over [lo, hi] for splitters ja..jb; returns its index.
    unsafe fn node(&mut self, ja: usize, jb: usize, lo: f64, hi: f64, cells: usize, depth: u32, key: bool) -> u32 {
        let mut nd = Node { lo: 0.0, scale: 0.0, top: 0.0, klo: 0, shift: 0, ktop: 0, key, base: self.tab.len() as u32 };
        let mut cells = cells;
        if key {
            let (klo, khi) = (engine::tkey(lo), engine::tkey(hi));
            let span = khi.saturating_sub(klo);
            let bits = 64 - span.leading_zeros();
            let want = cells.trailing_zeros();
            nd.klo = klo;
            nd.shift = bits.saturating_sub(want);
            nd.ktop = span >> nd.shift;
            cells = nd.ktop as usize + 1;
        } else {
            let scale = cells as f64 / (hi - lo);
            if !(scale.is_finite() && scale > 0.0) || !(hi - lo).is_finite() {
                cells = 1;                       /* no usable range: one cell, settled by comparing */
            } else {
                nd.lo = lo;
                nd.scale = scale;
            }
            nd.top = (cells - 1) as f64;
        }
        let idx = self.nodes.len() as u32;
        self.nodes.push(nd);
        let base = self.tab.len();
        self.tab.resize(base + cells, 0);
        let mut j = ja;
        for c in 0..cells {
            let j0 = j;
            while j < jb && nd.cell(self.s[j]) == c { j += 1; }
            // No splitter in the cell: the bucket is known. ONE splitter: the cell goes whole to the
            // bucket below it, which moves that bucket edge to the cell edge (a bucket needs balance,
            // not an exact quantile; the C engine's key grid snaps its edges the same way). So the
            // common lookup is one load and no compare.
            let e = if j - j0 <= 1 {
                j0 as u32
            } else if j - j0 >= 4 && depth + 1 < G_DEPTH && cells > 1 && self.s[j - 1] > self.s[j0] {
                G_CHILD | self.node(j0, j, self.s[j0], self.s[j - 1], if j - j0 < 32 { 256 } else { 1024 }, depth + 1, key)
            } else {
                G_SCAN | j0 as u32 | ((j - j0) as u32) << 12
            };
            self.tab[base + c] = e;
        }
        idx
    }
    /// How many of the sample's points the root alone settles.
    unsafe fn settled(&self, samp: &[f64]) -> usize {
        samp.iter().filter(|&&v| self.tab[self.nodes[0].cell(v)] < G_SCAN).count()
    }
    /// The grid a lookup starts in: the root, or with modes the mode's own grid.
    #[inline(always)]
    unsafe fn mode(&self, v: f64) -> usize {
        *self.start.get_unchecked((v >= self.bound[0]) as usize + (v >= self.bound[1]) as usize + (v >= self.bound[2]) as usize) as usize
    }
    #[inline(always)]
    unsafe fn lookup<const KEY: bool>(&self, v: f64, first: usize) -> usize {
        let mut nd = self.nodes.get_unchecked(first);
        loop {
            let e = *self.tab.get_unchecked(nd.base as usize + nd.cell_in::<KEY>(v));
            if e < G_SCAN { return e as usize; }
            if e >= G_CHILD {
                nd = self.nodes.get_unchecked((e & (G_SCAN - 1)) as usize);
                continue;
            }
            // How many of these splitters are below v, and how many are not above it. The two differ
            // only when v IS a splitter; when it is one that the sample holds k >= 2 times (a value
            // with more than a bucket's share of the array), the midpoint is a bucket nothing else
            // can reach, so a heavy value sits alone and its neighbours are not crowded by it.
            let (j0, c) = ((e & 0xFFF) as usize, ((e >> 12) & 0xFFF) as usize);
            let (mut lt, mut le) = (0usize, 0usize);
            if c <= 8 {
                for j in j0..j0 + c {
                    let x = *self.s.get_unchecked(j);
                    lt += (x < v) as usize;
                    le += (x <= v) as usize;
                }
            } else {
                let sl = self.s.get_unchecked(j0..j0 + c);
                lt = sl.partition_point(|&x| x < v);
                le = lt;
                if lt < c && *sl.get_unchecked(lt) <= v { le += sl.get_unchecked(lt..).partition_point(|&x| x <= v); }
            }
            return j0 + (lt + le + 1) / 2;
        }
    }
}

const BLOCK: usize = 256;
const SCATTER_CHUNK: usize = 1 << 15;

struct Spill {
    bucket: Vec<(u32, u32)>,
    vals: Vec<f64>,
}

/// `ints`: the sample was all 32-bit integers, so each bucket is offered to the LSD radix on
/// 4-byte keys before the kernel (a bucket is a narrow range: one or two of its three digits vary).
#[target_feature(enable = "avx2,fma,popcnt")]
unsafe fn partition_sort(a: &mut [f64], p: usize, ints: bool) {
    let n = a.len();
    let nb = (p * BUCKETS_PER_THREAD).min(MAX_BUCKETS);
    let m = (nb * 32).min(n / 4).max(nb);
    let mut samp: Vec<f64> = (0..m).map(|i| *a.get_unchecked(engine::weyl_pos(i, n))).collect();
    if !kernel::sort(&mut samp) {
        let k = engine::nans_last(a);
        engine::sort(&mut a[..k], p);
        return;
    }
    let s: Vec<f64> = (1..nb).map(|j| samp[j * m / nb]).collect();
    #[allow(unused_mut)]
    let mut t0 = trace_start!();
    // The grid's range: the sample with half a bucket trimmed from each end, and finite. One far
    // sentinel, or an infinity, would otherwise stretch the cells until every splitter sat in one
    // (infinities at 14 threads: a binary search per element, 0.70 of the C engine).
    let trim = m / (2 * nb);
    let lo = samp[trim..].iter().copied().find(|v| v.is_finite()).unwrap_or(samp[0]);
    let hi = samp[..m - trim].iter().rev().copied().find(|v| v.is_finite()).unwrap_or(samp[m - 1]);
    let (gv, gk) = (Grid::build(&s, lo, hi, false), Grid::build(&s, lo, hi, true));
    let (sv, sk) = (gv.settled(&samp), gk.settled(&samp));
    let grid = if sk > sv + m / 16 { gk } else { gv };
    let key_space = grid.nodes[0].key;
    trace!(t0, "partition: {} threads, {} buckets; the root settles {} of {} sample points by value, {} by key: {} space, {} grids, {} modes", p, nb, sv, m, sk,
           if key_space { "key" } else { "value" }, grid.nodes.len(), grid.nmodes);
    // A region per bucket, sized by what the map sends that bucket FROM THE SAMPLE (a snapped edge
    // or a heavy value moves a bucket away from 1 / nb of the array), with 60% slack, an eighth of
    // a mean bucket, and room for every thread's last block.
    let mut mass = vec![0usize; nb];
    for &v in &samp {
        let b = if key_space { grid.lookup::<true>(v, 0) } else { grid.lookup::<false>(v, if grid.nmodes > 0 { grid.mode(v) } else { 0 }) };
        mass[b] += 1;
    }
    let cap: Vec<usize> = (0..nb).map(|b| (mass[b] * n / m) * 8 / 5 + n / (8 * nb) + BLOCK * 2).collect();
    let mut roff = vec![0usize; nb + 1];
    for b in 0..nb { roff[b + 1] = roff[b] + cap[b]; }
    let buf = pool::acquire(roff[nb] * 8);
    let reg = Ptr(buf.ptr as *mut f64);
    let ar = Ptr(a.as_mut_ptr());
    let cur: Vec<AtomicUsize> = (0..nb).map(|_| AtomicUsize::new(0)).collect();
    let fail: Vec<AtomicUsize> = (0..nb).map(|_| AtomicUsize::new(usize::MAX)).collect();
    let next = AtomicUsize::new(0);
    let spills: Mutex<Vec<Spill>> = Mutex::new(Vec::new());
    let (cap, roff) = (&cap, &roff);
    run(p, &|_| {
        let (ar, reg) = (ar, reg);
        let mut stage: Vec<f64> = Vec::with_capacity(nb * BLOCK);
        let st = stage.as_mut_ptr();
        let mut cnt = vec![0u32; nb];
        let mut sp = Spill { bucket: Vec::new(), vals: Vec::new() };
        let flush = |b: usize, k: usize, sp: &mut Spill| {
            let pos = cur[b].fetch_add(k, Relaxed);
            if pos + k <= cap[b] {
                core::ptr::copy_nonoverlapping(st.add(b * BLOCK), reg.0.add(roff[b] + pos), k);
            } else {
                fail[b].fetch_min(pos, Relaxed);
                sp.bucket.push((b as u32, k as u32));
                sp.vals.extend_from_slice(core::slice::from_raw_parts(st.add(b * BLOCK), k));
            }
        };
        loop {
            let c = next.fetch_add(1, Relaxed);
            let lo = c * SCATTER_CHUNK;
            if lo >= n { break; }
            let hi = (lo + SCATTER_CHUNK).min(n);
            macro_rules! scatter {
                ($key:literal, $first:expr) => {
                    for i in lo..hi {
                        let v = *ar.0.add(i);
                        let b = grid.lookup::<$key>(v, $first(v));
                        let k = *cnt.get_unchecked(b) as usize;
                        *st.add(b * BLOCK + k) = v;
                        if k + 1 == BLOCK {
                            flush(b, BLOCK, &mut sp);
                            *cnt.get_unchecked_mut(b) = 0;
                        } else {
                            *cnt.get_unchecked_mut(b) = (k + 1) as u32;
                        }
                    }
                };
            }
            if key_space {
                scatter!(true, |_| 0)
            } else if grid.nmodes > 0 {
                scatter!(false, |v| grid.mode(v))
            } else {
                scatter!(false, |_| 0)
            }
        }
        for b in 0..nb {
            if cnt[b] > 0 { flush(b, cnt[b] as usize, &mut sp); }
        }
        if !sp.vals.is_empty() { spills.lock().unwrap_or_else(|e| e.into_inner()).push(sp); }
    });
    // what each bucket holds: the filled part of its region, and its spills
    let spills = spills.into_inner().unwrap_or_else(|e| e.into_inner());
    trace!(t0, "partition: scattered");
    let valid: Vec<usize> = (0..nb).map(|b| cur[b].load(Relaxed).min(fail[b].load(Relaxed))).collect();
    let mut spn = vec![0usize; nb + 1];
    for sp in &spills { for &(b, k) in &sp.bucket { spn[b as usize + 1] += k as usize; } }
    for b in 0..nb { spn[b + 1] += spn[b]; }
    let mut spv: Vec<f64> = Vec::with_capacity(spn[nb]);
    if spn[nb] > 0 {
        spv.set_len(spn[nb]);
        let mut at = spn.clone();
        for sp in &spills {
            let mut o = 0;
            for &(b, k) in &sp.bucket {
                spv[at[b as usize]..at[b as usize] + k as usize].copy_from_slice(&sp.vals[o..o + k as usize]);
                at[b as usize] += k as usize;
                o += k as usize;
            }
        }
    }
    let mut off = vec![0usize; nb + 1];
    for b in 0..nb { off[b + 1] = off[b] + valid[b] + spn[b + 1] - spn[b]; }
    debug_assert_eq!(off[nb], n);
    // the leaves, largest first
    let mut order: Vec<usize> = (0..nb).collect();
    order.sort_unstable_by_key(|&b| core::cmp::Reverse(off[b + 1] - off[b]));
    let largest = off[order[0] + 1] - off[order[0]];
    trace!(t0, "partition: {} spilled; largest bucket {} against a mean of {}", spn[nb], largest, n / nb);
    let next = AtomicUsize::new(0);
    run(p, &|_| {
        let (ar, reg) = (ar, reg);
        let mut keys: Vec<u32> = Vec::new();
        if ints { keys.reserve(2 * largest); }
        loop {
            let q = next.fetch_add(1, Relaxed);
            if q >= nb { break; }
            let b = order[q];
            let len = off[b + 1] - off[b];
            if len == 0 { continue; }
            let dst = ar.0.add(off[b]);
            core::ptr::copy_nonoverlapping(reg.0.add(roff[b]) as *const f64, dst, valid[b]);
            core::ptr::copy_nonoverlapping(spv.as_ptr().add(spn[b]), dst.add(valid[b]), spn[b + 1] - spn[b]);
            let bucket = core::slice::from_raw_parts_mut(dst, len);
            if !(ints && engine::int_radix_in(bucket, keys.as_mut_ptr())) { engine::kernel_sort(bucket); }
        }
    });
    trace!(t0, "partition: leaves sorted");
    pool::release(buf);
}

// ---------------------------------------------------------------------------------------------
// two halves at two threads

/// One split at the sample's median, then the kernel on each side, one thread each. A partition
/// into 24 buckets pays a scatter that two threads do not earn back. False when the split is
/// lopsided (the array is then permuted, which is all the next route needs).
#[target_feature(enable = "avx2,fma,popcnt")]
unsafe fn halves_sort(a: &mut [f64], pivot: f64) -> bool {
    let n = a.len();
    let m = kernel::split(a, pivot);
    if m < n / 8 || m > n - n / 8 { return false; }
    let ar = Ptr(a.as_mut_ptr());
    run(2, &|t| {
        let ar = ar;
        let half = if t == 0 { core::slice::from_raw_parts_mut(ar.0, m) } else { core::slice::from_raw_parts_mut(ar.0.add(m), n - m) };
        engine::kernel_sort(half);
    });
    true
}

// ---------------------------------------------------------------------------------------------
// disorder of short reach

const REACH_BLOCK: usize = 4096;
const REACH_MIN_N: usize = 1 << 20;

#[target_feature(enable = "avx2")]
unsafe fn min_max(s: &[f64]) -> (f64, f64) {
    let (mut mn, mut mx) = (_mm256_set1_pd(f64::INFINITY), _mm256_set1_pd(f64::NEG_INFINITY));
    let mut i = 0;
    while i + 4 <= s.len() {
        let v = _mm256_loadu_pd(s.as_ptr().add(i));
        mn = _mm256_min_pd(mn, v);
        mx = _mm256_max_pd(mx, v);
        i += 4;
    }
    let (mut a, mut b) = ([0f64; 4], [0f64; 4]);
    _mm256_storeu_pd(a.as_mut_ptr(), mn);
    _mm256_storeu_pd(b.as_mut_ptr(), mx);
    let (mut lo, mut hi) = (a[0].min(a[1]).min(a[2].min(a[3])), b[0].max(b[1]).max(b[2].max(b[3])));
    while i < s.len() {
        lo = lo.min(s[i]);
        hi = hi.max(s[i]);
        i += 1;
    }
    (lo, hi)
}

/// An array that ascends at large but is disordered within a few thousand positions (timestamps
/// that arrive with bounded latency): no element is far from its place, so a scatter is wasted on
/// it. Blocks of 4,096 are sorted by the kernel as they are copied to the scratch; they are then
/// runs whose ranges overlap only their neighbours, and the run plan lays them into the array,
/// merging only the seams. Gate: sixteen probes, each asking that a block lie wholly below the
/// block eight after it. False (array untouched) when the gate, the kernel or the plan declines.
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn reach_sort(a: &mut [f64], p: usize) -> bool {
    let n = a.len();
    if n < REACH_MIN_N { return false; }
    let nb = n / REACH_BLOCK;
    for k in 0..16 {
        let c = (nb - 9) * k / 16;
        let (_, hi) = min_max(&a[c * REACH_BLOCK..(c + 1) * REACH_BLOCK]);
        let (lo, _) = min_max(&a[(c + 8) * REACH_BLOCK..(c + 9) * REACH_BLOCK]);
        if !(hi <= lo) { return false; }
    }
    #[allow(unused_mut)]
    let mut t0 = trace_start!();
    let buf = pool::acquire(n * 8);
    let scr = Ptr(buf.ptr as *mut f64);
    let ar = Ptr(a.as_mut_ptr());
    let next = AtomicUsize::new(0);
    let failed = AtomicBool::new(false);
    let span = |c: usize| (c * REACH_BLOCK, if c + 1 == nb { n } else { (c + 1) * REACH_BLOCK });
    run(p, &|_| {
        let (ar, scr) = (ar, scr);
        loop {
            let c = next.fetch_add(1, Relaxed);
            if c >= nb || failed.load(Relaxed) { break; }
            let (lo, hi) = span(c);
            core::ptr::copy_nonoverlapping(ar.0.add(lo) as *const f64, scr.0.add(lo), hi - lo);
            if !kernel::sort(core::slice::from_raw_parts_mut(scr.0.add(lo), hi - lo)) { failed.store(true, Relaxed); }
        }
    });
    let runs: Vec<Run> = (0..nb).map(|c| Run { start: span(c).0, len: span(c).1 - span(c).0 }).collect();
    let plan = if failed.load(Relaxed) { None } else { engine::plan_runs(scr.0, &runs) };
    let plan = match plan {
        Some(pl) => pl,
        None => {
            pool::release(buf);
            return false;
        }
    };
    trace!(t0, "reach: {} blocks sorted, {} jobs over {} pieces planned", nb, plan.jobs.len(), plan.pieces.len());
    let next = AtomicUsize::new(0);
    let nt = n >= engine::NT_MIN_N;
    run(p, &|_| {
        let (ar, scr) = (ar, scr);
        loop {
            let j = next.fetch_add(64, Relaxed);
            if j >= plan.jobs.len() { break; }
            for q in j..(j + 64).min(plan.jobs.len()) { engine::place_job(scr.0, &plan, &plan.jobs[q], ar.0, nt); }
        }
        if nt { _mm_sfence(); }
    });
    trace!(t0, "reach: placed");
    pool::release(buf);
    true
}
