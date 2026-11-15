//! C entry points, so that one binary can time this engine beside the C one (`parity/parity.cpp`).

/// # Safety
/// `a` must point to `n` doubles.
#[no_mangle]
pub unsafe extern "C" fn logos_rs_sort_f64(a: *mut f64, n: usize, nthreads: i32) {
    if a.is_null() || n < 2 {
        return;
    }
    crate::sort_f64(core::slice::from_raw_parts_mut(a, n), nthreads.max(1) as usize);
}

/// The independent v3 engine (`sort_f64_v3`).
///
/// # Safety
/// `a` must point to `n` doubles.
#[no_mangle]
pub unsafe extern "C" fn logos_rs_v3_sort_f64(a: *mut f64, n: usize, nthreads: i32) {
    if a.is_null() || n < 2 {
        return;
    }
    crate::sort_f64_v3(core::slice::from_raw_parts_mut(a, n), nthreads.max(1) as usize);
}

#[no_mangle]
pub extern "C" fn logos_rs_release_buffers() {
    crate::release_buffers();
}

/// The AVX2 kernel alone, for `parity/kernbench.cpp`. Returns 0 when it declined (a NaN, or no AVX2).
///
/// # Safety
/// `a` must point to `n` doubles.
#[no_mangle]
pub unsafe extern "C" fn logos_rs_kernel_f64(a: *mut f64, n: usize) -> i32 {
    #[cfg(target_arch = "x86_64")]
    if is_x86_feature_detected!("avx2") && is_x86_feature_detected!("popcnt") {
        return crate::kernel::sort(core::slice::from_raw_parts_mut(a, n)) as i32;
    }
    let _ = (a, n);
    0
}

// ---- the transliterated C engine (src/port), for parity/routes.cpp: the sort and the C's route witnesses --------------
/// # Safety
/// `a` must point to `n` doubles. One call at a time, as the C engine.
#[cfg(all(target_arch = "x86_64", feature = "port_all"))]
#[no_mangle]
pub unsafe extern "C" fn logos_rsport_sort_f64(a: *mut f64, n: usize, nthreads: i32) {
    if a.is_null() { return }
    if n < 2 { return }
    crate::sort_f64(core::slice::from_raw_parts_mut(a, n), if nthreads > 0 { nthreads as usize } else { 1 });
}
#[cfg(all(target_arch = "x86_64", feature = "port_all"))]
#[no_mangle]
pub unsafe extern "C" fn logos_rsport_twist_last(which: i32) -> usize { crate::port::logos_twist_last(which) }
#[cfg(all(target_arch = "x86_64", feature = "port_all"))]
#[no_mangle]
pub unsafe extern "C" fn logos_rsport_reach_last() -> i32 { crate::port::logos_reach_last() }
#[cfg(all(target_arch = "x86_64", feature = "port_all"))]
#[no_mangle]
pub unsafe extern "C" fn logos_rsport_mirror_count(reset: i32) -> usize { crate::port::logos_mirror_count(reset) }
