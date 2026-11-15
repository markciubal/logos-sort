//! Scratch buffers kept between calls. A fresh 64 MB allocation is 16,000 page faults, which on
//! the cheap routes is a third of the sort; the C engine keeps its scratch for the same reason.
//! Unlike the C pool this one may be used from several threads at once: a buffer is taken out
//! under a lock and handed back, and a second concurrent sort simply allocates its own.

use std::alloc::{alloc, dealloc, Layout};
use std::sync::Mutex;

pub(crate) struct Buf {
    pub ptr: *mut u8,
    pub cap: usize,
}
unsafe impl Send for Buf {}

const KEEP: usize = 3;
static POOL: Mutex<Vec<Buf>> = Mutex::new(Vec::new());

fn layout(cap: usize) -> Layout {
    Layout::from_size_align(cap.max(64), 64).unwrap()
}

/// A buffer of at least `bytes` bytes, 64-byte aligned, contents unspecified.
pub(crate) fn acquire(bytes: usize) -> Buf {
    {
        let mut p = POOL.lock().unwrap_or_else(|e| e.into_inner());
        // the smallest pooled buffer that is large enough
        let mut best: Option<usize> = None;
        for (i, b) in p.iter().enumerate() {
            if b.cap >= bytes && best.map_or(true, |j| b.cap < p[j].cap) { best = Some(i); }
        }
        if let Some(i) = best { return p.swap_remove(i); }
    }
    let cap = bytes.max(64);
    let ptr = unsafe { alloc(layout(cap)) };
    if ptr.is_null() { std::alloc::handle_alloc_error(layout(cap)); }
    Buf { ptr, cap }
}

pub(crate) fn release(b: Buf) {
    let mut p = POOL.lock().unwrap_or_else(|e| e.into_inner());
    if p.len() >= KEEP {
        // keep the larger ones
        let (mut small, mut cap) = (usize::MAX, b.cap);
        for (i, q) in p.iter().enumerate() {
            if q.cap < cap { small = i; cap = q.cap; }
        }
        let drop = if small == usize::MAX { b } else { std::mem::replace(&mut p[small], b) };
        unsafe { dealloc(drop.ptr, layout(drop.cap)) };
    } else {
        p.push(b);
    }
}

/// Frees every buffer the engine keeps between calls.
pub fn release_buffers() {
    let mut p = POOL.lock().unwrap_or_else(|e| e.into_inner());
    for b in p.drain(..) {
        unsafe { dealloc(b.ptr, layout(b.cap)) };
    }
}
