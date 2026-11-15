//! A transliteration of `engines/c/logos_psort.c` (the f64 sort path, default build). Read `PORTING.md` first: the names,
//! types and arithmetic here are the C's on purpose, so that every input takes the same route in both engines.
//!
//! This file is the PRELUDE: allocation and memory helpers with libc's names, the trace macros, the constants of the C
//! file's first pages and the structs more than one file uses. Everything else is in the files listed in `PORTING.md`.
#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code, static_mut_refs)]
#![allow(unused_variables, unused_mut, unused_assignments, unused_imports, unused_unsafe, unused_parens, unused_macros)]
#![allow(clippy::all)]

pub(crate) use core::arch::x86_64::*;
pub(crate) use core::cell::Cell;
pub(crate) use core::ffi::c_void;
pub(crate) use core::mem::size_of;
pub(crate) use core::ptr::{addr_of, addr_of_mut, null, null_mut};
pub(crate) use core::sync::atomic::{AtomicBool, AtomicI32, AtomicIsize, AtomicU32, AtomicU64, AtomicUsize, Ordering};
pub(crate) use core::sync::atomic::Ordering::{Acquire, Relaxed, Release, SeqCst};

// ---- libc's names over the Rust allocator ---------------------------------------------------------------------------
// A block carries its size in a 64-byte header, so `free` needs no size, and every block is 64-byte aligned. An
// allocation that cannot be had aborts (the C's out-of-memory fallbacks are not ported).
const ALLOC_HDR: usize = 64;
#[inline]
unsafe fn alloc_raw(bytes: usize, zero: bool) -> *mut u8 {
    let total = bytes.checked_add(ALLOC_HDR).expect("allocation size overflow");
    let layout = std::alloc::Layout::from_size_align_unchecked(total, 64);
    let p = if zero { std::alloc::alloc_zeroed(layout) } else { std::alloc::alloc(layout) };
    if p.is_null() { std::alloc::handle_alloc_error(layout) }
    *(p as *mut usize) = total;
    p.add(ALLOC_HDR)
}
#[inline] pub(crate) unsafe fn malloc(bytes: usize) -> *mut u8 { alloc_raw(bytes, false) }
#[inline] pub(crate) unsafe fn calloc(count: usize, size: usize) -> *mut u8 { alloc_raw(count.checked_mul(size).expect("allocation size overflow"), true) }
#[inline]
pub(crate) unsafe fn free<T>(p: *mut T) {
    if p.is_null() { return }
    let b = (p as *mut u8).sub(ALLOC_HDR);
    let total = *(b as *const usize);
    std::alloc::dealloc(b, std::alloc::Layout::from_size_align_unchecked(total, 64));
}
#[inline]
pub(crate) unsafe fn realloc<T>(p: *mut T, bytes: usize) -> *mut u8 {
    if p.is_null() { return malloc(bytes) }
    let b = (p as *mut u8).sub(ALLOC_HDR);
    let total = *(b as *const usize);
    let nb = std::alloc::realloc(b, std::alloc::Layout::from_size_align_unchecked(total, 64), bytes + ALLOC_HDR);
    if nb.is_null() { std::alloc::handle_alloc_error(std::alloc::Layout::from_size_align_unchecked(bytes + ALLOC_HDR, 64)) }
    *(nb as *mut usize) = bytes + ALLOC_HDR;
    nb.add(ALLOC_HDR)
}
#[inline(always)] pub(crate) unsafe fn memcpy<T, U>(dst: *mut T, src: *const U, bytes: usize) { core::ptr::copy_nonoverlapping(src as *const u8, dst as *mut u8, bytes) }
#[inline(always)] pub(crate) unsafe fn memmove<T, U>(dst: *mut T, src: *const U, bytes: usize) { core::ptr::copy(src as *const u8, dst as *mut u8, bytes) }
#[inline(always)] pub(crate) unsafe fn memset<T>(p: *mut T, byte: i32, bytes: usize) { core::ptr::write_bytes(p as *mut u8, byte as u8, bytes) }

// ---- the phase trace (-DLOGOS_TRACE in C, the `trace` feature here): stderr timestamps, the C's texts ----------------
#[cfg(feature = "trace")]
thread_local! { pub(crate) static g_trace_t0: Cell<Option<std::time::Instant>> = Cell::new(None); }
#[cfg(feature = "trace")]
macro_rules! TRACE_START { () => { $crate::port::g_trace_t0.with(|t| t.set(Some(std::time::Instant::now()))) }; }
#[cfg(feature = "trace")]
macro_rules! TRACE {
    ($($arg:tt)*) => {{
        let ms = $crate::port::g_trace_t0.with(|t| t.get()).map(|t| t.elapsed().as_secs_f64() * 1e3).unwrap_or(0.0);
        eprintln!("[logos-rs {:9.3} ms] {}", ms, format_args!($($arg)*));
    }};
}
#[cfg(not(feature = "trace"))]
macro_rules! TRACE_START { () => {}; }
#[cfg(not(feature = "trace"))]
macro_rules! TRACE { ($($arg:tt)*) => {{ if false { let _ = format_args!($($arg)*); } }}; }

// ---- what this CPU runs (C lines 52-64): 2 = AVX2 with FMA, 1 = SSE2 ---------------------------------------------------
pub(crate) static g_simd_kind: AtomicI32 = AtomicI32::new(-1);
#[inline]
pub(crate) fn logos_simd_kind() -> i32 {
    let mut c = g_simd_kind.load(Relaxed);
    if c < 0 {
        c = if is_x86_feature_detected!("avx2") && is_x86_feature_detected!("fma") { 2 } else { 1 };
        g_simd_kind.store(c, Relaxed);
    }
    c
}

// ---- constants (C lines 116-291) ---------------------------------------------------------------------------------------
pub(crate) const PF_DIST: usize = 32;
pub(crate) const INSERTION_SORT_THRESHOLD: usize = 24;
pub(crate) const SF_VEC_MIN_N: usize = 1024;
pub(crate) const SF_PREFIX: usize = 64;
pub(crate) const MERGE_MAX_RUNS: usize = 8;
pub(crate) const SKEW_PROBE_MAX: i32 = 8;
pub(crate) const QUANT_PROBE_MAX: i32 = 4;
pub(crate) const COUNTING_SORT_K: usize = 1;
pub(crate) const NEARLY_SORTED_INV_RATIO: f64 = 0.05;
pub(crate) const MOMENTUM_THRESHOLD: usize = 50000;
pub(crate) const MAX_RUNS_FOR_MERGE: usize = 32;
pub(crate) const MIN_AVG_RUN_FOR_MERGE: usize = 4096;
pub(crate) const MOMENTUM_MAX_AVG_RUN: usize = 8;
pub(crate) const FLASH_SORT_THRESHOLD: usize = 2048;
pub(crate) const AVX2_FLASH_MAX_N: usize = 65536;
pub(crate) const FLASH_SAFETY_RATIO: f64 = 0.05;
pub(crate) const COARSE_MIN_N: usize = 500000;
pub(crate) const FLASH_M_DIV: usize = 1;
pub(crate) const MIN_GALLOP: usize = 7;
pub(crate) const COARSE_K: usize = 2048;
pub(crate) const COARSE_MAXD: usize = 4;
pub(crate) const COARSE_SMALL: usize = 64;
pub(crate) const QUADWAY_THRESHOLD: usize = 16;
pub(crate) const TYPED_MIN_N: usize = 65536;
pub(crate) const RADIX64_MAX_N: usize = 8388608;
pub(crate) const OVERSAMPLE: usize = 32;
pub(crate) const LOWDISC_GAP_RATIO: f64 = 4.0;
pub(crate) const LOWDISC_MIN_COVER: f64 = 0.5;
pub(crate) const LOWDISC_LONG_COVER: f64 = 0.05;
pub(crate) const MIN_PARALLEL: usize = 1 << 16;
pub(crate) const MIN_ELEMS_PER_THREAD: usize = 100000;
pub(crate) const BOOST_PER: usize = 25000;
pub(crate) const BOOST_MIN_N: usize = 300000;
pub(crate) const BUCKETS_PER_THREAD: i32 = 12;
pub(crate) const MAX_BUCKETS: i32 = 256;
pub(crate) const WC_MIN_THREADS: i32 = 8;
pub(crate) const TRIM_MIN_BYTES: usize = 1 << 20;

#[inline]
pub(crate) fn weyl_pos(i: usize, n: usize) -> usize {
    let mut f = i as f64 * 0.6180339887498949;
    f -= (f as u64) as f64;
    let q = (f * n as f64) as usize;
    if q < n { q } else { n - 1 }
}
#[inline(always)] pub(crate) fn SAMPLE_POS(i: usize, _m: usize, n: usize) -> usize { weyl_pos(i, n) }

#[inline(never)] pub(crate) fn min_per_thread_env() -> usize { MIN_ELEMS_PER_THREAD }       // LOGOS_MIN_PER_THREAD unset
#[inline(never)]
pub(crate) fn boost_threads(n: usize, P: i32, Preq: i32) -> i32 {                           // LOGOS_BOOST_PER / LOGOS_BOOST_MIN unset
    let (per, mn) = (BOOST_PER, BOOST_MIN_N);
    if per < 1000 || n < mn { return P }
    let mut pb = n / per;
    if pb > Preq as usize { pb = Preq as usize }
    if pb > P as usize { pb as i32 } else { P }
}
#[inline]
pub(crate) fn effective_threads(n: usize, P: i32) -> i32 {
    let mut per = MIN_ELEMS_PER_THREAD;
    if P > 1 && n >= 32768 { per = min_per_thread_env() }
    let mut pmax = n / per;
    if pmax < 1 { pmax = 1 }
    if P as usize > pmax { pmax as i32 } else { P }
}

// ---- structs more than one file uses -----------------------------------------------------------------------------------
/// C line 293: the per-thread leaf context.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ctx_t {
    pub(crate) cap: usize,
    pub(crate) aux: *mut u32,                         // cap+1: counts / radix src
    pub(crate) idx: *mut u16,                         // cap: coarse bucket index
    pub(crate) C: [*mut usize; COARSE_MAXD],
    pub(crate) S: [*mut usize; COARSE_MAXD],
    pub(crate) H: [*mut usize; COARSE_MAXD],
    pub(crate) hist: [[u32; 257]; 8],
    pub(crate) last_bail_avg_run: f64,
    pub(crate) wc: *mut f64,                          // write-combining stage: K x 8 doubles
    pub(crate) wcn: *mut u8,                          // K counters
    pub(crate) wc_on: i32,
    pub(crate) seq: i32,                              // 1 when this context sorts on the calling thread alone
    pub(crate) nearly: i32,                           // sort_fn's inversion probe said nearly sorted
    pub(crate) lowdisc: i32,                          // sort_fn's window probe found low-discrepancy data
    pub(crate) land: [*mut u8; COARSE_MAXD],          // ping-pong: which buffer each child landed in
    pub(crate) tid: i32,
    pub(crate) path: i32,
    pub(crate) pooled: i32,                           // buffers belong to the persistent bundle table
    pub(crate) pool_ix: i32,                          // that bundle's index, or -1
}
/// C line 327.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct trim_t { pub(crate) hw: usize, pub(crate) hi: usize, pub(crate) ep: u32 }
/// C line 455. kind: 0 nothing borrowed, 1 the jumbo, 2 a temporary.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ctx_fit_t { pub(crate) aux: *mut u32, pub(crate) idx: *mut u16, pub(crate) cap: usize, pub(crate) kind: i32 }
/// C line 1981: a run. rev: it lies descending.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct rdesc_t { pub(crate) first: f64, pub(crate) last: f64, pub(crate) start: usize, pub(crate) len: usize, pub(crate) rev: i32 }
/// C line 5667.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct scratch_t {
    pub(crate) n: usize, pub(crate) bidx_n: usize, pub(crate) pb: *mut c_void, pub(crate) pbi: *mut u8,
    pub(crate) dst: *mut f64, pub(crate) bidx: *mut u8, pub(crate) pooled: i32, pub(crate) have: i32, pub(crate) failed: i32,
}
/// C line 5866.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct psamp_t { pub(crate) pos: usize, pub(crate) v: f64 }
/// C line 7348: the one shared sample.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct sodo_t {
    pub(crate) m: usize, pub(crate) mfull: usize, pub(crate) distinct: usize, pub(crate) f1: usize, pub(crate) f2: usize, pub(crate) reach: usize,
    pub(crate) chao1: f64, pub(crate) inv_rate: f64, pub(crate) inv_all: f64, pub(crate) ks: f64,
    pub(crate) has_nan: i32, pub(crate) sorted: *mut f64,
}
/// C line 8445.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct pscan_t { pub(crate) first_desc: usize, pub(crate) asc: i32, pub(crate) desc: i32 }

/// The port keeps the C engine's globals (pools, context bundles, witnesses), so its unit tests take turns, and take turns
/// with every `sort_f64` another test thread makes.
#[cfg(all(test, feature = "port_all"))]
pub(crate) fn port_test_lock() -> std::sync::MutexGuard<'static, ()> { crate::port_lock() }
#[cfg(all(test, not(feature = "port_all")))]
pub(crate) fn port_test_lock() -> std::sync::MutexGuard<'static, ()> {
    static ONE: std::sync::Mutex<()> = std::sync::Mutex::new(());
    ONE.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- the files (PORTING.md has the line ranges) ------------------------------------------------------------------------
#[cfg(any(feature = "port_all", feature = "port_base"))] mod base;
#[cfg(any(feature = "port_all", feature = "port_base"))] pub(crate) use base::*;
#[cfg(any(feature = "port_all", feature = "port_leaf"))] mod leaf;
#[cfg(any(feature = "port_all", feature = "port_leaf"))] pub(crate) use leaf::*;
#[cfg(any(feature = "port_all", feature = "port_knot"))] mod knot;
#[cfg(any(feature = "port_all", feature = "port_knot"))] pub(crate) use knot::*;
#[cfg(any(feature = "port_all", feature = "port_part"))] mod part;
#[cfg(any(feature = "port_all", feature = "port_part"))] pub(crate) use part::*;
#[cfg(any(feature = "port_all", feature = "port_count"))] mod count;
#[cfg(any(feature = "port_all", feature = "port_count"))] pub(crate) use count::*;
#[cfg(any(feature = "port_all", feature = "port_reloc"))] mod reloc;
#[cfg(any(feature = "port_all", feature = "port_reloc"))] pub(crate) use reloc::*;
#[cfg(any(feature = "port_all", feature = "port_ord1"))] mod ord1;
#[cfg(any(feature = "port_all", feature = "port_ord1"))] pub(crate) use ord1::*;
#[cfg(any(feature = "port_all", feature = "port_ord2"))] mod ord2;
#[cfg(any(feature = "port_all", feature = "port_ord2"))] pub(crate) use ord2::*;
#[cfg(any(feature = "port_all", feature = "port_disp"))] mod disp;
#[cfg(any(feature = "port_all", feature = "port_disp"))] pub(crate) use disp::*;
