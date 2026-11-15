//! C lines 4628-4805, 4830-5530, 5827-5843 (lattice_spill_finish), 7416-7593, 7836-7904, 8778-8792 (lat1_gate): the counting
//! routes. Few values (a verified perfect hash, a vector count), hashed counts, lattices (direct index, spills), dense
//! integers (a bitmap), runs of equal values, one dominant value. A transliteration: see PORTING.md.
use super::*;

// ---- few values (C 4628-4925) ----------------------------------------------------------------------------------------
pub(crate) const LOWCARD_SAMPLE_MAX: usize = 64;
pub(crate) const LOWCARD_MAX_SCAN: usize = LOWCARD_SAMPLE_MAX;
pub(crate) const LC_PHASH_BITS: i32 = 12;
pub(crate) const LOWCARD_CAP: usize = 256;


#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct lcmap_t {
    pub(crate) a: *const f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) keys: *mut u64, pub(crate) vals: *mut f64,
    pub(crate) cnts: *mut usize, pub(crate) d: usize, pub(crate) abort: i32, pub(crate) tid: i32,
    pub(crate) hmul: u64, pub(crate) hkey: *const u64, pub(crate) slot: *const u8, pub(crate) sd: usize, pub(crate) hcnt: *mut usize,
    pub(crate) hvals: *const f64, pub(crate) vec: i32,
}
pub(crate) const LC_VEC_MAX: usize = 8;
pub(crate) static g_lcvec_elems: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(17)

#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn lc_vec_count(a: *const f64, lo: usize, hi: usize, vals: *const f64, sd: i32, cnt: *mut usize) -> usize {
    let mut b: [__m256i; LC_VEC_MAX] = [_mm256_setzero_si256(); LC_VEC_MAX];
    let mut c: [__m256i; LC_VEC_MAX] = [_mm256_setzero_si256(); LC_VEC_MAX];
    let sd = sd as usize;
    for j in 0..sd { b[j] = _mm256_castpd_si256(_mm256_set1_pd(*vals.add(j))); c[j] = _mm256_setzero_si256(); }
    let mut i = lo;
    while i + 8 <= hi {
        let v0 = _mm256_loadu_si256(a.add(i) as *const __m256i); let v1 = _mm256_loadu_si256(a.add(i + 4) as *const __m256i);
        let mut m0 = _mm256_setzero_si256(); let mut m1 = _mm256_setzero_si256();
        for j in 0..sd {
            let e0 = _mm256_cmpeq_epi64(v0, b[j]); let e1 = _mm256_cmpeq_epi64(v1, b[j]);
            c[j] = _mm256_sub_epi64(c[j], _mm256_add_epi64(e0, e1));
            m0 = _mm256_or_si256(m0, e0); m1 = _mm256_or_si256(m1, e1);
        }
        if _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_and_si256(m0, m1))) != 0xF {
            for j in 0..sd {
                let e0 = _mm256_cmpeq_epi64(v0, b[j]); let e1 = _mm256_cmpeq_epi64(v1, b[j]);
                c[j] = _mm256_add_epi64(c[j], _mm256_add_epi64(e0, e1));
            }
            break;
        }
        i += 8;
    }
    for j in 0..sd {
        let mut t = [0u64; 4]; _mm256_storeu_si256(t.as_mut_ptr() as *mut __m256i, c[j]);
        *cnt.add(j) += t[0].wrapping_add(t[1]).wrapping_add(t[2]).wrapping_add(t[3]) as usize;
    }
    i
}

pub(crate) unsafe fn lc_map(p: *mut c_void) -> *mut c_void { lc_map_body(p) }
pub(crate) unsafe fn lc_map_body(p: *mut c_void) -> *mut c_void {
    let m = p as *mut lcmap_t; let mut d: usize = 0;
    let keys = (*m).keys; let vals = (*m).vals; let cnts = (*m).cnts;
    if (*m).hmul != 0 {
        let mul = (*m).hmul; let hk = (*m).hkey; let slot = (*m).slot; let hc = (*m).hcnt;
        let mut i0 = (*m).lo;
        if (*m).vec != 0 { i0 = lc_vec_count((*m).a, (*m).lo, (*m).hi, (*m).hvals, (*m).sd as i32, hc); g_lcvec_elems.fetch_add(i0 - (*m).lo, Relaxed); }
        let mut i = i0;
        while i < (*m).hi {
            let x = *(*m).a.add(i); let k = total_key(x);
            let h = (k.wrapping_mul(mul) >> (64 - LC_PHASH_BITS)) as u32; let sl = *slot.add(h as usize) as usize;
            if *hk.add(sl) == k { *hc.add(sl) += 1; i += 1; continue; }
            if x != x { (*m).abort = 1; return null_mut(); }
            let mut lo = 0usize; for q in 0..d { lo += (*keys.add(q) < k) as usize; }
            if lo < d && *keys.add(lo) == k { *cnts.add(lo) += 1; i += 1; continue; }
            if d == LOWCARD_CAP { (*m).abort = 1; return null_mut(); }
            memmove(keys.add(lo + 1), keys.add(lo), (d - lo) * size_of::<u64>());
            memmove(vals.add(lo + 1), vals.add(lo), (d - lo) * size_of::<f64>());
            memmove(cnts.add(lo + 1), cnts.add(lo), (d - lo) * size_of::<usize>());
            *keys.add(lo) = k; *vals.add(lo) = x; *cnts.add(lo) = 1; d += 1;
            i += 1;
        }
        (*m).d = d; return null_mut();
    }
    let mut i = (*m).lo;
    while i < (*m).hi {
        let x = *(*m).a.add(i); if x != x { (*m).abort = 1; return null_mut(); }
        let k = total_key(x);
        let mut lo: usize;
        if d <= 32 { lo = 0; for q in 0..d { lo += (*keys.add(q) < k) as usize; } }
        else { let mut hi = d; lo = 0; while lo < hi { let mid = (lo + hi) >> 1; if *keys.add(mid) < k { lo = mid + 1 } else { hi = mid } } }
        if lo < d && *keys.add(lo) == k { *cnts.add(lo) += 1; i += 1; continue; }
        if d == LOWCARD_CAP { (*m).abort = 1; return null_mut(); }
        memmove(keys.add(lo + 1), keys.add(lo), (d - lo) * size_of::<u64>());
        memmove(vals.add(lo + 1), vals.add(lo), (d - lo) * size_of::<f64>());
        memmove(cnts.add(lo + 1), cnts.add(lo), (d - lo) * size_of::<usize>());
        *keys.add(lo) = k; *vals.add(lo) = x; *cnts.add(lo) = 1; d += 1;
        i += 1;
    }
    (*m).d = d; null_mut()
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct lcemit_t {
    pub(crate) a: *mut f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) gv: *const f64, pub(crate) gstart: *const usize,
    pub(crate) gd: usize, pub(crate) tid: i32, pub(crate) nt: i32,
}
pub(crate) const LC_NT_MIN_N: usize = 2097152;
pub(crate) const LC_NT_MIN_RUN: usize = 512;
pub(crate) unsafe fn lc_fill_nt(p: *mut f64, k: usize, val: f64) {
    let mut i = 0usize;
    while i < k && ((p.add(i) as usize) & 15) != 0 { *p.add(i) = val; i += 1; }
    let v = _mm_set1_pd(val);
    while i + 8 <= k { _mm_stream_pd(p.add(i), v); _mm_stream_pd(p.add(i + 2), v); _mm_stream_pd(p.add(i + 4), v); _mm_stream_pd(p.add(i + 6), v); i += 8; }
    while i + 2 <= k { _mm_stream_pd(p.add(i), v); i += 2; }
    while i < k { *p.add(i) = val; i += 1; }
}
pub(crate) unsafe fn lc_emit(p: *mut c_void) -> *mut c_void { lc_emit_body(p) }
pub(crate) unsafe fn lc_emit_body(p: *mut c_void) -> *mut c_void {
    let e = p as *mut lcemit_t;
    let mut v: usize;
    { let mut lo = 0usize; let mut hi = (*e).gd; while lo < hi { let mid = (lo + hi) >> 1; if *(*e).gstart.add(mid + 1) <= (*e).lo { lo = mid + 1 } else { hi = mid } } v = lo; }
    let mut pos = (*e).lo;
    if (*e).nt != 0 {
        while pos < (*e).hi && v < (*e).gd {
            let mut end = *(*e).gstart.add(v + 1); if end > (*e).hi { end = (*e).hi } let val = *(*e).gv.add(v);
            if end - pos >= LC_NT_MIN_RUN { lc_fill_nt((*e).a.add(pos), end - pos, val); pos = end; } else { while pos < end { *(*e).a.add(pos) = val; pos += 1; } }
            v += 1;
        }
        _mm_sfence();
        return null_mut();
    }
    while pos < (*e).hi && v < (*e).gd {
        let mut end = *(*e).gstart.add(v + 1); if end > (*e).hi { end = (*e).hi } let val = *(*e).gv.add(v);
        while pos < end { *(*e).a.add(pos) = val; pos += 1; }
        v += 1;
    }
    null_mut()
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct lcpair_t { pub(crate) k: u64, pub(crate) v: f64, pub(crate) c: usize }
pub(crate) unsafe fn cmp_lc_pair(x: *const c_void, y: *const c_void) -> i32 {
    let a = (*(x as *const lcpair_t)).k; let b = (*(y as *const lcpair_t)).k; (a > b) as i32 - (a < b) as i32
}
pub(crate) unsafe fn lc_sort_pairs(p: *mut lcpair_t, n: usize, tmp: *mut lcpair_t) {
    if n < 2 { return }
    if n <= 48 {
        for i in 1..n { let k = *p.add(i); let mut j = i; while j != 0 && (*p.add(j - 1)).k > k.k { *p.add(j) = *p.add(j - 1); j -= 1; } *p.add(j) = k; }
        return;
    }
    let mut or_a: u64 = 0; let mut and_a: u64 = !0u64;
    for i in 0..n { or_a |= (*p.add(i)).k; and_a &= (*p.add(i)).k; }
    let diff = or_a ^ and_a;
    let mut src = p; let mut dst = tmp;
    for b in 0..8 {
        if ((diff >> (b * 8)) & 0xFF) == 0 { continue }
        let sh = b * 8;
        let mut cnt = [0usize; 257];
        for i in 0..n { cnt[(((*src.add(i)).k >> sh) & 0xFF) as usize + 1] += 1; }
        for c in 0..256 { cnt[c + 1] += cnt[c]; }
        for i in 0..n { let s = &mut cnt[(((*src.add(i)).k >> sh) & 0xFF) as usize]; *dst.add(*s) = *src.add(i); *s += 1; }
        let t = src; src = dst; dst = t;
    }
    if src != p { memcpy(p, src, n * size_of::<lcpair_t>()); }
}

/// returns 1 if it sorted a[], 0 if it declined (a[] untouched)
pub(crate) unsafe fn lowcard_try_f64(a: *mut f64, n: usize, mut P: i32, ext: *const f64, ext_m: usize) -> i32 {
    P = count_threads_hash(n, P);
    let mut own: *mut f64 = null_mut(); let mut S: *const f64 = ext; let mut sm = ext_m;
    if ext.is_null() {
        let m = if n < 2048 { n } else { 2048 };
        own = malloc(m * size_of::<f64>()) as *mut f64;
        for i in 0..m { let q = SAMPLE_POS(i, m, n); *own.add(i) = *a.add(if q < n { q } else { n - 1 }); }
        let nan_n = sample_sort(own, m);
        let mut distinct = 1usize; let mut i = 1usize; while i < m - nan_n { if *own.add(i) != *own.add(i - 1) { distinct += 1 } i += 1; }
        if nan_n != 0 || distinct > LOWCARD_MAX_SCAN { free(own); return 0 }
        S = own; sm = m - nan_n;
    }
    let mut hmul: u64 = 0; let mut sd: usize = 0; let mut sv = [0.0f64; LOWCARD_SAMPLE_MAX]; let mut sk = [0u64; LOWCARD_SAMPLE_MAX];
    let mut slot = [0u8; 1usize << LC_PHASH_BITS];
    for i in 0..sm {
        let mut add = (sd == 0 || *S.add(i) != sv[sd - 1]) as i32;
        if add == 0 { let k = total_key(*S.add(i)); add = 1; let mut q = sd; while q > 0 { q -= 1; if sk[q] == k { add = 0; break } } }
        if add != 0 { if sd == LOWCARD_SAMPLE_MAX || sd == 255 { sd = 0; break } sv[sd] = *S.add(i); sk[sd] = total_key(*S.add(i)); sd += 1; }
    }
    if sd >= 1 {
        let mut seed: u64 = 0x9E3779B97F4A7C15u64 ^ (sd as u64);
        let mut attempt = 0;
        while attempt < 24 && hmul == 0 {
            seed = seed.wrapping_mul(6364136223846793005u64).wrapping_add(1442695040888963407u64); let mul = seed | 1; let mut ok = 1;
            memset(slot.as_mut_ptr(), 0, slot.len());
            for q in 0..sd { slot[(sk[q].wrapping_mul(mul) >> (64 - LC_PHASH_BITS)) as u32 as usize] = q as u8; }
            let mut q = 0; while q < sd && ok != 0 { if slot[(sk[q].wrapping_mul(mul) >> (64 - LC_PHASH_BITS)) as u32 as usize] != q as u8 { ok = 0 } q += 1; }
            if ok != 0 { hmul = mul }
            attempt += 1;
        }
    }
    if !own.is_null() { free(own) }
    let mut vec = 0;
    vec = (hmul != 0 && sd >= 1 && sd <= LC_VEC_MAX && avx2_leaf_on() != 0) as i32;
    TRACE!("lowcard: sample {} values, {} distinct, perfect hash {}, vector count {}", sm, sd, if hmul != 0 { "built" } else { "none" }, if vec != 0 { "on" } else { "off" });

    let Pu = P as usize;
    let chunk = (n + Pu - 1) / Pu;
    let mp = calloc(Pu, size_of::<lcmap_t>()) as *mut lcmap_t; let th = malloc(Pu * size_of::<pthread_t>()) as *mut pthread_t;
    let ok = 1;
    for t in 0..Pu {
        let w = mp.add(t);
        (*w).a = a; (*w).lo = t * chunk; (*w).hi = (*w).lo + chunk; if (*w).hi > n { (*w).hi = n }
        (*w).keys = malloc(LOWCARD_CAP * size_of::<u64>()) as *mut u64; (*w).vals = malloc(LOWCARD_CAP * size_of::<f64>()) as *mut f64; (*w).cnts = malloc(LOWCARD_CAP * size_of::<usize>()) as *mut usize;
        if hmul != 0 { (*w).hmul = hmul; (*w).hkey = sk.as_ptr(); (*w).slot = slot.as_ptr(); (*w).sd = sd; (*w).hcnt = calloc(sd, size_of::<usize>()) as *mut usize;
                       (*w).hvals = sv.as_ptr(); (*w).vec = vec; }
    }
    let mut sorted = 0;
    if ok != 0 {
        par_run(th, P, lc_map, mp as *mut c_void, size_of::<lcmap_t>(), 1);
        let mut aborted = 0; let mut total = 0usize; let mut over = 0usize;
        for t in 0..Pu { if (*mp.add(t)).abort != 0 { aborted = 1 } total += (*mp.add(t)).d + (if hmul != 0 { sd } else { 0 }); over += (*mp.add(t)).d; }
        TRACE!("lowcard: map done, {} entries the hash did not cover{}", over, if aborted != 0 { ", ABORTED" } else { "" });
        if aborted == 0 {
            let tn = if total != 0 { total } else { 1 };
            let pairs = malloc(tn * size_of::<lcpair_t>()) as *mut lcpair_t;
            let ptmp = malloc(tn * size_of::<lcpair_t>()) as *mut lcpair_t;
            let gv = malloc(tn * size_of::<f64>()) as *mut f64;
            let gcnt = malloc(tn * size_of::<usize>()) as *mut usize;
            let gstart = malloc((total + 2) * size_of::<usize>()) as *mut usize;
            {
                let mut w = 0usize;
                for t in 0..Pu { for i in 0..(*mp.add(t)).d { (*pairs.add(w)).k = *(*mp.add(t)).keys.add(i); (*pairs.add(w)).v = *(*mp.add(t)).vals.add(i); (*pairs.add(w)).c = *(*mp.add(t)).cnts.add(i); w += 1; } }
                if hmul != 0 { for t in 0..Pu { for q in 0..sd { if *(*mp.add(t)).hcnt.add(q) != 0 { (*pairs.add(w)).k = sk[q]; (*pairs.add(w)).v = sv[q]; (*pairs.add(w)).c = *(*mp.add(t)).hcnt.add(q); w += 1; } } } }
                lc_sort_pairs(pairs, w, ptmp);
                let mut gd = 0usize;
                for i in 0..w {
                    if gd > 0 && (*pairs.add(i)).k == (*pairs.add(i - 1)).k { *gcnt.add(gd - 1) += (*pairs.add(i)).c; continue; }
                    *gv.add(gd) = (*pairs.add(i)).v; *gcnt.add(gd) = (*pairs.add(i)).c; gd += 1;
                }
                *gstart = 0; for i in 0..gd { *gstart.add(i + 1) = *gstart.add(i) + *gcnt.add(i); }
                let em = malloc(Pu * size_of::<lcemit_t>()) as *mut lcemit_t;
                {
                    for t in 0..Pu { let e = em.add(t); (*e).a = a; (*e).lo = t * chunk; (*e).hi = (*e).lo + chunk; if (*e).hi > n { (*e).hi = n } (*e).gv = gv; (*e).gstart = gstart; (*e).gd = gd; (*e).nt = (n >= LC_NT_MIN_N) as i32; }
                    par_run(th, P, lc_emit, em as *mut c_void, size_of::<lcemit_t>(), 1);
                    free(em); sorted = 1;
                }
            }
            free(ptmp); free(pairs); free(gv); free(gcnt); free(gstart);
        }
    }
    for t in 0..Pu { free((*mp.add(t)).keys); free((*mp.add(t)).vals); free((*mp.add(t)).cnts); free((*mp.add(t)).hcnt); }
    free(mp); free(th);
    sorted
}

// ---- hashed counts (C 4939-5012) -------------------------------------------------------------------------------------
pub(crate) const MODCARD_CAP: usize = 65536;
pub(crate) const MODCARD_MIN_N: usize = 65536;
pub(crate) const MODCARD_GATE: f64 = 0.98;
pub(crate) const MC_HASH_CHAO1_SEQ: usize = 128;
pub(crate) const MC_HASH_CHAO1_MT: usize = 3300;
pub(crate) const MC_HASH_CHAO1_RADIX: usize = 3300;
pub(crate) const MC_HASH_SEQ_CAP: usize = 8192;
pub(crate) const LATTICE_MT_MAX_SLOTS: usize = 65536;
pub(crate) const MODCARD_GATE_MT: f64 = 0.995;
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct hmap_t { pub(crate) keys: *mut u64, pub(crate) vals: *mut f64, pub(crate) cnts: *mut usize, pub(crate) cap: usize, pub(crate) used: usize }
#[inline]
pub(crate) unsafe fn mix64(mut k: u64) -> u64 { k ^= k >> 33; k = k.wrapping_mul(0xff51afd7ed558ccdu64); k ^= k >> 33; k = k.wrapping_mul(0xc4ceb9fe1a85ec53u64); k ^= k >> 33; k }
pub(crate) unsafe fn hmap_init(h: *mut hmap_t, slots: usize) -> i32 {
    (*h).cap = slots; (*h).used = 0;
    (*h).keys = malloc(slots * size_of::<u64>()) as *mut u64; (*h).vals = malloc(slots * size_of::<f64>()) as *mut f64; (*h).cnts = malloc(slots * size_of::<usize>()) as *mut usize;
    memset((*h).keys, 0xFF, slots * size_of::<u64>());
    1
}
pub(crate) unsafe fn hmap_free(h: *mut hmap_t) { free((*h).keys); free((*h).vals); free((*h).cnts); (*h).keys = null_mut(); (*h).vals = null_mut(); (*h).cnts = null_mut(); }
pub(crate) unsafe fn hmap_grow(h: *mut hmap_t) -> i32 {
    let mut g: hmap_t = core::mem::zeroed(); if hmap_init(&mut g, (*h).cap * 2) == 0 { hmap_free(&mut g); return 0 }
    for i in 0..(*h).cap { if *(*h).keys.add(i) != u64::MAX {
        let mut q = (mix64(*(*h).keys.add(i)) as usize) & (g.cap - 1); while *g.keys.add(q) != u64::MAX { q = (q + 1) & (g.cap - 1) }
        *g.keys.add(q) = *(*h).keys.add(i); *g.vals.add(q) = *(*h).vals.add(i); *g.cnts.add(q) = *(*h).cnts.add(i); } }
    g.used = (*h).used; hmap_free(h); *h = g; 1
}
/// add `c` occurrences of value v (key k); returns 0 when aborted (cap exceeded)
pub(crate) unsafe fn hmap_add(h: *mut hmap_t, k: u64, v: f64, c: usize, maxd: usize) -> i32 {
    loop {
        let mut q = (mix64(k) as usize) & ((*h).cap - 1);
        loop {
            let kk = *(*h).keys.add(q);
            if kk == k { *(*h).cnts.add(q) += c; return 1 }
            if kk == u64::MAX {
                if (*h).used >= maxd { return 0 }
                if (*h).used * 2 >= (*h).cap { break }
                *(*h).keys.add(q) = k; *(*h).vals.add(q) = v; *(*h).cnts.add(q) = c; (*h).used += 1; return 1;
            }
            q = (q + 1) & ((*h).cap - 1);
        }
        if hmap_grow(h) == 0 { return 0 }
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct mcmap_t { pub(crate) a: *const f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) h: hmap_t, pub(crate) abort: i32, pub(crate) tid: i32, pub(crate) cap: usize }
pub(crate) static g_mc_capped: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(40)
pub(crate) unsafe fn mc_map(p: *mut c_void) -> *mut c_void { mc_map_body(p) }
pub(crate) unsafe fn mc_map_body(p: *mut c_void) -> *mut c_void {
    let m = p as *mut mcmap_t;
    let mut i = (*m).lo;
    while i < (*m).hi {
        let x = *(*m).a.add(i);
        if x != x || hmap_add(addr_of_mut!((*m).h), total_key(x), x, 1, (*m).cap) == 0 {
            (*m).abort = 1;
            TRACE!("modcard: thread gave up {} of {} elements in ({:.0}% scanned), {} distinct so far",
                   i - (*m).lo, (*m).hi - (*m).lo, 100.0 * (i - (*m).lo) as f64 / (if (*m).hi - (*m).lo != 0 { (*m).hi - (*m).lo } else { 1 }) as f64, (*m).h.used);
            return null_mut();
        }
        i += 1;
    }
    null_mut()
}

// ---- lattices (C 5031-5391) ------------------------------------------------------------------------------------------
pub(crate) const LATTICE_MAX_BYTES: usize = 64 << 20;
pub(crate) const LATTICE_MAX_SLOTS: usize = 262144;
pub(crate) const LATTICE_SLOT_BYTES: usize = size_of::<u64>() + size_of::<f64>() + size_of::<usize>();
pub(crate) const LATTICE_MIN_SLOTS: usize = 4096;
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct lslots_t { pub(crate) keys: *mut u64, pub(crate) vals: *mut f64, pub(crate) cnts: *mut usize }
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct lattice_arg_t {
    pub(crate) a: *const f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) base: f64, pub(crate) step: f64,
    pub(crate) margin: i64, pub(crate) slots: i64, pub(crate) t: lslots_t, pub(crate) abort: i32, pub(crate) tid: i32,
    pub(crate) next: *mut AtomicUsize, pub(crate) abort_all: *mut AtomicI32, pub(crate) n: usize, pub(crate) cstep: usize,
    pub(crate) sp: *mut f64, pub(crate) spn: usize, pub(crate) spalloc: usize, pub(crate) spcap: usize, pub(crate) sptot: *mut AtomicUsize,
}
pub(crate) const LAT_SPILL_DIV: usize = 32;
pub(crate) static g_lat_spilled: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(37)
pub(crate) unsafe fn lat_spill(m: *mut lattice_arg_t, x: f64) -> i32 {
    if (*m).sptot.is_null() || (*(*m).sptot).fetch_add(1, Relaxed) >= (*m).spcap { return 0 }
    if (*m).spn == (*m).spalloc {
        let mut na = if (*m).spalloc != 0 { 2 * (*m).spalloc } else { 4096 }; if na > (*m).spcap { na = (*m).spcap } if na <= (*m).spn { na = (*m).spn + 1 }
        let p = realloc((*m).sp, na * size_of::<f64>()) as *mut f64;
        (*m).sp = p; (*m).spalloc = na;
    }
    *(*m).sp.add((*m).spn) = x; (*m).spn += 1; 1
}
pub(crate) unsafe fn lattice_map(p: *mut c_void) -> *mut c_void { lattice_map_body(p) }
pub(crate) const g_lat_serial_tables: i32 = 0;   // LOGOS_LAT_SERIAL_TABLES unset
pub(crate) const g_lat_static_map: i32 = 0;      // LOGOS_LAT_STATIC_MAP unset
pub(crate) unsafe fn lattice_table_init(m: *mut lattice_arg_t) -> i32 {
    if !(*m).t.keys.is_null() { return 1 }
    (*m).t.keys = malloc((*m).slots as usize * size_of::<u64>()) as *mut u64;
    (*m).t.vals = malloc((*m).slots as usize * size_of::<f64>()) as *mut f64;
    (*m).t.cnts = calloc((*m).slots as usize, size_of::<usize>()) as *mut usize;
    memset((*m).t.keys, 0xFF, (*m).slots as usize * size_of::<u64>());
    1
}
pub(crate) unsafe fn lattice_map_body(p: *mut c_void) -> *mut c_void {
    let m = p as *mut lattice_arg_t;
    if lattice_table_init(m) == 0 { (*m).abort = 1; if !(*m).abort_all.is_null() { (*(*m).abort_all).store(1, SeqCst) } return null_mut(); }
    if (*m).next.is_null() { if lattice_map_range(m, (*m).lo, (*m).hi) == 0 { (*m).abort = 1 } return null_mut(); }
    loop {
        if (*(*m).abort_all).load(Relaxed) != 0 { return null_mut() }
        let c = (*(*m).next).fetch_add(1, SeqCst); let lo = c * (*m).cstep;
        if lo >= (*m).n { return null_mut() }
        let mut hi = lo + (*m).cstep; if hi > (*m).n { hi = (*m).n }
        if lattice_map_range(m, lo, hi) == 0 { (*m).abort = 1; (*(*m).abort_all).store(1, SeqCst); return null_mut(); }
    }
}
pub(crate) const g_no_latvec: i32 = 0;           // LOGOS_NO_LATVEC unset
pub(crate) static g_latvec_vecs: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(23)
#[inline]
pub(crate) unsafe fn lattice_step(m: *mut lattice_arg_t, x: f64, inv: f64) -> i32 {
    if x != x { return 0 }
    let q = (x - (*m).base) * inv; let qi = ((q + (if q >= 0.0 { 0.5 } else { -0.5 })) as i64) as f64;
    let mut err = q - qi; if err < 0.0 { err = -err }
    let tol = 1e-9 * (if qi >= 0.0 { qi } else { -qi }) + 1e-9;
    if err > tol { return lat_spill(m, x) }
    let idx = (qi as i64).wrapping_add((*m).margin); if idx < 0 || idx >= (*m).slots { return lat_spill(m, x) }
    let k = total_key(x); let kk = *(*m).t.keys.add(idx as usize);
    if kk == k { *(*m).t.cnts.add(idx as usize) += 1; return 1 }
    if kk != u64::MAX { return lat_spill(m, x) }
    *(*m).t.keys.add(idx as usize) = k; *(*m).t.vals.add(idx as usize) = x; *(*m).t.cnts.add(idx as usize) = 1;
    1
}
#[target_feature(enable = "avx2,fma,popcnt")]
pub(crate) unsafe fn lattice_map_vec(m: *mut lattice_arg_t, lo: usize, hi: usize, inv: f64) -> i32 {
    let vbase = _mm256_set1_pd((*m).base); let vinv = _mm256_set1_pd(inv); let e9 = _mm256_set1_pd(1e-9);
    let absm = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7fffffffffffffffi64));
    let vmarg = _mm256_set1_pd((*m).margin as f64); let vslots = _mm256_set1_pd((*m).slots as f64); let zero = _mm256_setzero_pd();
    let sbit = _mm256_set1_epi64x(0x8000000000000000u64 as i64); let z = _mm256_setzero_si256();
    let keys = (*m).t.keys as *const i64; let cnts = (*m).t.cnts;
    let mut i = lo; let mut fast = 0usize;
    while i + 4 <= hi {
        let x = _mm256_loadu_pd((*m).a.add(i));
        let q = _mm256_mul_pd(_mm256_sub_pd(x, vbase), vinv);
        let qi = _mm256_round_pd::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(q);
        let err = _mm256_and_pd(_mm256_sub_pd(q, qi), absm);
        let tol = _mm256_fmadd_pd(_mm256_and_pd(qi, absm), e9, e9);
        let qm = _mm256_add_pd(qi, vmarg);
        let ok = _mm256_and_pd(_mm256_cmp_pd::<_CMP_LE_OQ>(err, tol),
                 _mm256_and_pd(_mm256_cmp_pd::<_CMP_GE_OQ>(qm, zero), _mm256_cmp_pd::<_CMP_LT_OQ>(qm, vslots)));
        if _mm256_movemask_pd(ok) == 15 {
            let idx = _mm256_cvttpd_epi32(qm);
            let kk = _mm256_i32gather_epi64::<8>(keys, idx);
            let u = _mm256_castpd_si256(x);
            let k = _mm256_xor_si256(u, _mm256_or_si256(_mm256_cmpgt_epi64(z, u), sbit));
            if _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(kk, k))) == 15 {
                *cnts.add(_mm_cvtsi128_si32(idx) as usize) += 1; *cnts.add(_mm_extract_epi32::<1>(idx) as usize) += 1;
                *cnts.add(_mm_extract_epi32::<2>(idx) as usize) += 1; *cnts.add(_mm_extract_epi32::<3>(idx) as usize) += 1;
                fast += 1;
                i += 4;
                continue;
            }
        }
        for j in i..i + 4 { if lattice_step(m, *(*m).a.add(j), inv) == 0 { return 0 } }
        i += 4;
    }
    g_latvec_vecs.fetch_add(fast, Relaxed);
    while i < hi { if lattice_step(m, *(*m).a.add(i), inv) == 0 { return 0 } i += 1; }
    1
}
/// one chunk into this thread's own table; 0 = this data is not a lattice (the caller declines)
pub(crate) unsafe fn lattice_map_range(m: *mut lattice_arg_t, lo: usize, hi: usize) -> i32 {
    let inv = 1.0 / (*m).step;
    if g_no_latvec == 0 && avx2_leaf_on() != 0 { return lattice_map_vec(m, lo, hi, inv) }
    for i in lo..hi {
        let x = *(*m).a.add(i); if x != x { return 0 }
        let q = (x - (*m).base) * inv; let qi = ((q + (if q >= 0.0 { 0.5 } else { -0.5 })) as i64) as f64;
        let mut err = q - qi; if err < 0.0 { err = -err }
        let tol = 1e-9 * (if qi >= 0.0 { qi } else { -qi }) + 1e-9;
        if err > tol { if lat_spill(m, x) != 0 { continue } return 0 }
        let idx = (qi as i64).wrapping_add((*m).margin); if idx < 0 || idx >= (*m).slots { if lat_spill(m, x) != 0 { continue } return 0 }
        let k = total_key(x); let kk = *(*m).t.keys.add(idx as usize);
        if kk == k { *(*m).t.cnts.add(idx as usize) += 1; continue }
        if kk != u64::MAX { if lat_spill(m, x) != 0 { continue } return 0 }
        *(*m).t.keys.add(idx as usize) = k; *(*m).t.vals.add(idx as usize) = x; *(*m).t.cnts.add(idx as usize) = 1;
    }
    1
}
pub(crate) const g_lat_par_merge: i32 = 0;       // LOGOS_LAT_PAR_MERGE unset: lattice_merge_par / lattice_merge_worker not ported
pub(crate) const LATTICE_STEP_DIV: i32 = 16;
pub(crate) const LAT1_MIN_FILL: f64 = 16.0;
#[inline]
pub(crate) unsafe fn lat_fill_min(P: i32) -> f64 { LAT1_MIN_FILL * (P + 1) as f64 / 2.0 }
pub(crate) unsafe fn lattice_sample_step(samp: *const f64, m: usize, distinct: *mut usize, div: *mut i32) -> f64 {
    let smin = *samp; let mut g = f64::INFINITY; let mut d0 = 1usize;
    for i in 1..m { if *samp.add(i) != *samp.add(i - 1) { let gi = *samp.add(i) - *samp.add(i - 1); if gi < g { g = gi } d0 += 1; } }
    *distinct = d0; *div = 0;
    if !(g > 0.0) || !g.is_finite() { return 0.0 }
    let dmax = LATTICE_STEP_DIV;                                   // LOGOS_LAT_DIV1 unset
    for d in 1..=dmax {
        let step = g / d as f64; let mut i = 0usize;
        while i < m {
            let q = (*samp.add(i) - smin) / step; if !(q < 9e15) { return 0.0 }
            let qi = ((q + 0.5) as i64) as f64; let mut e = q - qi; if e < 0.0 { e = -e } if e > 1e-9 * (qi + 1.0) { break }
            i += 1;
        }
        if i == m { *div = d; return step }
    }
    0.0
}
/// returns 1 if sorted, 0 if declined (a[] untouched). samp: sorted sample of m values.
pub(crate) unsafe fn lattice_try_f64(a: *mut f64, n: usize, P: i32, samp: *const f64, m: usize) -> i32 {
    let smin = *samp; let smax = *samp.add(m - 1); let mut distinct: usize = 0; let mut div: i32 = 0;
    if !smin.is_finite() || !smax.is_finite() { return 0 }
    let step = lattice_sample_step(samp, m, &mut distinct, &mut div);
    if distinct < 3 || !(step > 0.0) { return 0 }
    if div > 1 && ((smax - smin) / step + 1.0) * lat_fill_min(P) > n as f64 { return 0 }
    let mut cap_slots = (LATTICE_MAX_BYTES / LATTICE_SLOT_BYTES / (if P < 1 { 1 } else { P }) as usize) as i64;
    if cap_slots > LATTICE_MAX_SLOTS as i64 { cap_slots = LATTICE_MAX_SLOTS as i64 }
    if cap_slots < LATTICE_MIN_SLOTS as i64 { cap_slots = LATTICE_MIN_SLOTS as i64 }
    let spanD = (smax - smin) / step; if !(spanD >= 0.0) || spanD > cap_slots as f64 { return 0 }
    let span = (spanD + 0.5) as i64 + 1;
    let mut margin = span / 8 + 64; if span + 2 * margin < LATTICE_MIN_SLOTS as i64 { margin = (LATTICE_MIN_SLOTS as i64 - span) / 2 }
    if span + 2 * margin > cap_slots { margin = (cap_slots - span) / 2; if margin < 0 { margin = 0 } }
    let slots = span + 2 * margin;
    if slots > cap_slots { return 0 }
    if P >= WC_MIN_THREADS && slots > LATTICE_MT_MAX_SLOTS as i64 { return 0 }

    let Pu = P as usize;
    let chunk = (n + Pu - 1) / Pu;
    let mp = calloc(Pu, size_of::<lattice_arg_t>()) as *mut lattice_arg_t; let th = malloc(Pu * size_of::<pthread_t>()) as *mut pthread_t;
    let mut ok = 1;
    let map_next = AtomicUsize::new(0);
    let map_abort = AtomicI32::new(0);
    let mut cstep = (n + (4 * P) as usize - 1) / (4 * P) as usize; if cstep < 4096 { cstep = 4096 }
    let sptot = AtomicUsize::new(0);
    let spill = 1;                                                 // LOGOS_LAT_NO_SPILL unset
    for t in 0..Pu {
        let w = mp.add(t);
        (*w).a = a; (*w).lo = t * chunk; (*w).hi = (*w).lo + chunk; if (*w).hi > n { (*w).hi = n }
        (*w).base = smin; (*w).step = step; (*w).margin = margin; (*w).slots = slots;
        (*w).spcap = n / LAT_SPILL_DIV; (*w).sptot = if spill != 0 { &sptot as *const AtomicUsize as *mut AtomicUsize } else { null_mut() };
        (*w).n = n; (*w).cstep = cstep; (*w).next = if g_lat_static_map != 0 { null_mut() } else { &map_next as *const AtomicUsize as *mut AtomicUsize };
        (*w).abort_all = &map_abort as *const AtomicI32 as *mut AtomicI32;
        if g_lat_serial_tables != 0 && lattice_table_init(w) == 0 { ok = 0 }
    }
    let mut sorted = 0;
    if ok != 0 {
        par_run(th, P, lattice_map, mp as *mut c_void, size_of::<lattice_arg_t>(), 1);
        TRACE!("lattice: map done ({} threads, {} slots)", P, slots);
        let mut aborted = 0; for t in 0..Pu { if (*mp.add(t)).abort != 0 { aborted = 1 } }
        if aborted == 0 {
            let gv = malloc(slots as usize * size_of::<f64>()) as *mut f64; let gstart = malloc((slots as usize + 1) * size_of::<usize>()) as *mut usize;
            let em = malloc(Pu * size_of::<lcemit_t>()) as *mut lcemit_t;
            {
                let mut gd = 0usize; let mut acc = 0usize; let mut conflict = 0;
                {                                                  // lattice_merge_par returns 0 with g_lat_par_merge == 0: the serial loop
                    gd = 0; acc = 0; conflict = 0;
                    let mut i: i64 = 0;
                    while i < slots && conflict == 0 {
                        let mut key = u64::MAX; let mut cnt = 0usize; let mut val = 0.0f64;
                        for t in 0..Pu { let kk = *(*mp.add(t)).t.keys.add(i as usize); if kk == u64::MAX { continue }
                            if key == u64::MAX { key = kk; val = *(*mp.add(t)).t.vals.add(i as usize); } else if kk != key { conflict = 1; break }
                            cnt += *(*mp.add(t)).t.cnts.add(i as usize); }
                        if cnt != 0 { *gv.add(gd) = val; *gstart.add(gd) = acc; acc += cnt; gd += 1; }
                        i += 1;
                    }
                }
                TRACE!("lattice: merge done (serial, walked {} slots x {} threads = {} reads)", slots, P, slots * P as i64);
                let mut spn = 0usize; for t in 0..Pu { spn += (*mp.add(t)).spn }
                let nl = n - spn; let ech = (nl + Pu - 1) / Pu;
                let spok = 1;
                if spn > (*mp).spn {
                    if (*mp).spalloc < spn { let p = realloc((*mp).sp, spn * size_of::<f64>()) as *mut f64; (*mp).sp = p; (*mp).spalloc = spn; }
                    if spok != 0 { let mut o = (*mp).spn; for t in 1..Pu { if (*mp.add(t)).spn != 0 { memcpy((*mp).sp.add(o), (*mp.add(t)).sp, (*mp.add(t)).spn * size_of::<f64>()); o += (*mp.add(t)).spn; } } }
                }
                if conflict == 0 && acc == nl && spok != 0 {
                    *gstart.add(gd) = acc;
                    for t in 0..Pu { let e = em.add(t); (*e).a = a; (*e).lo = t * ech; (*e).hi = (*e).lo + ech; if (*e).hi > nl { (*e).hi = nl } if (*e).lo > nl { (*e).lo = nl } (*e).gv = gv; (*e).gstart = gstart; (*e).gd = gd; (*e).nt = (n >= LC_NT_MIN_N) as i32; }
                    par_run(th, P, lc_emit, em as *mut c_void, size_of::<lcemit_t>(), 1);
                    TRACE!("lattice: emit done ({} distinct, {} spilled)", gd, spn);
                    if spn != 0 { lattice_spill_finish(a, nl, (*mp).sp, spn); g_lat_spilled.fetch_add(spn, Relaxed); }
                    sorted = 1;
                }
            }
            free(gv); free(gstart); free(em);
        }
    }
    TRACE!("lattice: freeing {} tables of {} slots ({:.1} MB total)", P, slots, P as f64 * slots as f64 * 24.0 / 1048576.0);
    for t in 0..Pu { free((*mp.add(t)).t.keys); free((*mp.add(t)).t.vals); free((*mp.add(t)).t.cnts); free((*mp.add(t)).sp); }
    free(mp); free(th);
    TRACE!("lattice: tables freed");
    sorted
}

/// C 5827: the lattice route's spills, after the emit has written the lattice's own nl elements to a[0, nl).
pub(crate) unsafe fn lattice_spill_finish(a: *mut f64, nl: usize, sp: *mut f64, spn: usize) {
    {   // NESTED_SORT(psort_f64(sp, spn, 1, NULL, NULL, NULL, NULL))
        let cm_ = g_ctx_next; ns_depth.with(|d| d.set(d.get() + 1));
        psort_f64(sp, spn, 1, null_mut(), null_mut(), null_mut(), null_mut());
        ns_depth.with(|d| d.set(d.get() - 1)); g_ctx_next = cm_;
    }
    let mut i = nl;
    let mut q = spn;
    while q > 0 {
        q -= 1;
        let v = *sp.add(q); let p: usize;
        if i == 0 || !(v < *a.add(i - 1)) { p = i }
        else {
            let mut top = i - 1; let mut step = 1usize; let mut lo: usize;
            loop { if step > top { lo = 0; break } if !(v < *a.add(top - step)) { lo = top - step + 1; break } top -= step; step <<= 1; }
            let mut hi = top;
            while lo < hi { let mid = lo + ((hi - lo) >> 1); if v < *a.add(mid) { hi = mid } else { lo = mid + 1 } }
            p = lo;
        }
        if p < i { memmove(a.add(p + q + 1), a.add(p), (i - p) * size_of::<f64>()); }
        *a.add(p + q) = v; i = p;
    }
}

/// C 8781: a lattice the sample cannot see repeat.
pub(crate) const LAT1_PMAX: i32 = 64;
#[inline(never)]
pub(crate) unsafe fn lat1_gate(s: *const f64, m: usize, n: usize, P: i32) -> i32 {
    if m < 64 { return 0 }
    let pmax = LAT1_PMAX; if P > pmax { return 0 }
    let fill = lat_fill_min(P);
    let smin = *s; let smax = *s.add(m - 1);
    if !smin.is_finite() || !smax.is_finite() { return 0 }
    let mut distinct: usize = 0; let mut div: i32 = 0; let step = lattice_sample_step(s, m, &mut distinct, &mut div);
    if distinct < 3 || !(step > 0.0) { return 0 }
    let span = (smax - smin) / step;
    (span >= 0.0 && (span + 1.0) * fill <= n as f64) as i32
}
pub(crate) static g_lat1_sorts: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(36): the dispatcher adds to it

// ---- the hash gate and the hashed-count route (C 5406-5530) ------------------------------------------------------------
pub(crate) unsafe fn sample_quant_passes(s: *const f64, m: usize) -> i32 {
    let mut or_h: u32 = 0; let mut and_h: u32 = 0xFFFFFFFF; let mut or_l: u32 = 0; let mut and_l: u32 = 0xFFFFFFFF;
    for i in 0..m { let mut tl: u32 = 0; let mut th: u32 = 0; key_parts(*s.add(i), &mut tl, &mut th); or_h |= th; and_h &= th; or_l |= tl; and_l &= tl; }
    let mut passes = 0;
    for p in 0..8 { let hi_word = (p >= 4) as i32; let sh = (p & 3) << 3; let o = if hi_word != 0 { or_h } else { or_l }; let an = if hi_word != 0 { and_h } else { and_l }; if ((o >> sh) & 0xFF) != ((an >> sh) & 0xFF) { passes += 1 } }
    passes
}
#[inline]
pub(crate) unsafe fn mc_hash_allowed(chao1: f64, P: i32, n: usize, samp: *const f64, m: usize) -> i32 {
    if P >= WC_MIN_THREADS { return (chao1 <= MC_HASH_CHAO1_MT as f64) as i32 }
    if chao1 <= MC_HASH_CHAO1_SEQ as f64 { return 1 }
    if n >= TYPED_MIN_N && n <= RADIX64_MAX_N && chao1 <= MC_HASH_CHAO1_RADIX as f64 && !samp.is_null() && m != 0 && sample_quant_passes(samp, m) <= QUANT_PROBE_MAX { 2 } else { 0 }
}
pub(crate) unsafe fn modcard_try_f64(a: *mut f64, n: usize, mut P: i32, ext: *const f64, ext_m: usize, allow_hash: i32) -> i32 {
    P = count_threads(n, P);
    let m = if !ext.is_null() { ext_m } else if n < 2048 { n } else { 2048 };
    let samp = if !ext.is_null() { ext as *mut f64 } else { malloc(m * size_of::<f64>()) as *mut f64 };
    if ext.is_null() {
        for i in 0..m { let q = SAMPLE_POS(i, m, n); *samp.add(i) = *a.add(if q < n { q } else { n - 1 }); }
        sample_sort(samp, m);
        let mut distinct = 1usize; let mut has_nan = 0;
        for i in 1..m { if *samp.add(i) != *samp.add(i - 1) { distinct += 1 } if *samp.add(i) != *samp.add(i) { has_nan = 1 } }
        let gate = if P >= WC_MIN_THREADS { MODCARD_GATE_MT } else { MODCARD_GATE };
        if has_nan != 0 || distinct as f64 > gate * m as f64 { free(samp); return 0 }
    }
    if lattice_try_f64(a, n, P, samp, m) != 0 { if ext.is_null() { free(samp) } return 1 }
    if ext.is_null() { free(samp) }
    if allow_hash == 0 { return 0 }

    let Pu = P as usize;
    let chunk = (n + Pu - 1) / Pu; let mut mc_cap = MODCARD_CAP;
    if allow_hash == 2 { let v = MC_HASH_SEQ_CAP; if v != 0 && v < mc_cap { mc_cap = v } }   // LOGOS_MC_SEQCAP unset
    let mp = calloc(Pu, size_of::<mcmap_t>()) as *mut mcmap_t; let th = malloc(Pu * size_of::<pthread_t>()) as *mut pthread_t;
    let mut ok = 1;
    for t in 0..Pu {
        let w = mp.add(t);
        (*w).a = a; (*w).lo = t * chunk; (*w).hi = (*w).lo + chunk; if (*w).hi > n { (*w).hi = n }
        (*w).cap = mc_cap;
        if hmap_init(addr_of_mut!((*w).h), 4096) == 0 { ok = 0 }
    }
    let mut sorted = 0;
    if ok != 0 {
        par_run(th, P, mc_map, mp as *mut c_void, size_of::<mcmap_t>(), 1);
        let mut aborted = 0; let mut tot = 0usize; for t in 0..Pu { if (*mp.add(t)).abort != 0 { aborted = 1 } tot += (*mp.add(t)).h.used; }
        if aborted != 0 && mc_cap < MODCARD_CAP { g_mc_capped.fetch_add(1, SeqCst); }
        TRACE!("modcard: map phase done, {} threads, aborted={}, {} entries across the tables", P, aborted, tot);
        if aborted == 0 {
            // LOGOS_MC_CONCAT unset: the global-hash merge (the concat path is not ported)
            let mut g: hmap_t = core::mem::zeroed(); let mut gok = hmap_init(&mut g, 8192);
            let mut t = 0;
            while gok != 0 && t < Pu {
                for i in 0..(*mp.add(t)).h.cap {
                    if *(*mp.add(t)).h.keys.add(i) != u64::MAX && hmap_add(&mut g, *(*mp.add(t)).h.keys.add(i), *(*mp.add(t)).h.vals.add(i), *(*mp.add(t)).h.cnts.add(i), usize::MAX) == 0 { gok = 0 }
                }
                t += 1;
            }
            if gok != 0 {
                let gd = g.used; let gn = if gd != 0 { gd } else { 1 };
                let op = malloc(gn * size_of::<lcpair_t>()) as *mut lcpair_t; let ot = malloc(gn * size_of::<lcpair_t>()) as *mut lcpair_t;
                let ov = malloc(gn * size_of::<f64>()) as *mut f64; let os_ = malloc((gd + 2) * size_of::<usize>()) as *mut usize;
                let oe = malloc(Pu * size_of::<lcemit_t>()) as *mut lcemit_t;
                {
                    let mut w = 0usize; for i in 0..g.cap { if *g.keys.add(i) != u64::MAX { (*op.add(w)).k = *g.keys.add(i); (*op.add(w)).v = *g.vals.add(i); (*op.add(w)).c = *g.cnts.add(i); w += 1; } }
                    lc_sort_pairs(op, w, ot);
                    *os_ = 0; for i in 0..w { *ov.add(i) = (*op.add(i)).v; *os_.add(i + 1) = *os_.add(i) + (*op.add(i)).c; }
                    for t in 0..Pu { let e = oe.add(t); (*e).a = a; (*e).lo = t * chunk; (*e).hi = (*e).lo + chunk; if (*e).hi > n { (*e).hi = n } (*e).gv = ov; (*e).gstart = os_; (*e).gd = w; (*e).nt = (n >= LC_NT_MIN_N) as i32; }
                    par_run(th, P, lc_emit, oe as *mut c_void, size_of::<lcemit_t>(), 1);
                    sorted = 1;
                }
                free(op); free(ot); free(ov); free(os_); free(oe);
            }
            hmap_free(&mut g);
        }
    }
    for t in 0..Pu { hmap_free(addr_of_mut!((*mp.add(t)).h)); }
    free(mp); free(th);
    sorted
}

// ---- dense integers (C 7416-7504) ------------------------------------------------------------------------------------
pub(crate) static mut g_dense_count: usize = 0;   // logos_twist_last(6)
pub(crate) const DENSE_SPAN_MULT: f64 = 16.0;
pub(crate) const DENSE_MIN_N: usize = 65536;
pub(crate) const DENSE_INT_MAX: f64 = 4503599627370496.0;
pub(crate) unsafe fn dense_gate(samp: *const f64, m: usize, n: usize, mult: f64) -> i32 {
    if m < 256 { return 0 }
    let mut rep_ = 0usize;
    for i in 0..m {
        let v = *samp.add(i);
        if !(v >= -DENSE_INT_MAX && v <= DENSE_INT_MAX) { return 0 }
        if v != (v as i64) as f64 { return 0 }
        if i != 0 && v == *samp.add(i - 1) { rep_ += 1 }
    }
    if rep_ > m / 64 { return 0 }
    let span = *samp.add(m - 1) - *samp + 1.0;
    (span > 0.0 && span <= mult * n as f64) as i32
}
pub(crate) unsafe fn dense_int_try_f64(a: *mut f64, n: usize, samp: *const f64, m: usize) -> i32 {
    if n < DENSE_MIN_N { return 0 }
    let mult = DENSE_SPAN_MULT;                                    // LOGOS_DENSE_SPAN unset
    if samp.is_null() || dense_gate(samp, m, n, mult) == 0 { return 0 }
    let base = (*samp as i64).wrapping_sub(n as i64);
    let topd = *samp.add(m - 1) + n as f64;
    if !(topd < DENSE_INT_MAX) { return 0 }
    let span = ((topd as i64).wrapping_sub(base) as u64).wrapping_add(1);
    if span as f64 > (mult + 2.0) * n as f64 { return 0 }

    let words = ((span + 63) / 64) as usize;
    let bits = calloc(words, size_of::<u64>()) as *mut u64;
    for i in 0..n {
        let v = *a.add(i);
        let k = v as i64;                                          // C: an unguarded cast (see the report); a NaN or a huge value fails the next test either way
        if k as f64 != v || (k == 0 && v.is_sign_negative()) { free(bits); return 0 }
        let d = k.wrapping_sub(base) as u64;
        if k < base || d >= span { free(bits); return 0 }
        let w = (d >> 6) as usize; let b = 1u64 << (d & 63);
        if *bits.add(w) & b != 0 { free(bits); return 0 }
        *bits.add(w) |= b;
    }
    let mut out = 0usize;
    for w in 0..words {
        let mut x = *bits.add(w);
        while x != 0 {
            let t = x.trailing_zeros() as i32;
            *a.add(out) = base.wrapping_add((w * 64 + t as usize) as i64) as f64; out += 1;
            x &= x.wrapping_sub(1);
        }
    }
    free(bits);
    TRACE!("dense-int: {} values, window {}, emitted {}", n, span, out);
    g_dense_count += (out == n) as usize;
    (out == n) as i32
}

// ---- runs of equal values (C 7520-7593) ------------------------------------------------------------------------------
pub(crate) const RLE_MIN_N: usize = 65536;
pub(crate) const RLE_EQ_MIN: f64 = 0.75;
pub(crate) static g_rle_sorts: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(28)
#[inline]
pub(crate) unsafe fn rle_key(d: f64) -> u64 { let u = d.to_bits(); if (u >> 63) != 0 { !u } else { u | 0x8000000000000000u64 } }
#[inline]
pub(crate) unsafe fn rle_unkey(k: u64) -> f64 { let u = if (k >> 63) != 0 { k & 0x7FFFFFFFFFFFFFFFu64 } else { !k }; f64::from_bits(u) }
pub(crate) unsafe fn rle_msd(k: *mut u64, v: *mut u32, k2: *mut u64, v2: *mut u32, n: usize, out2: i32) {
    if n <= 48 {
        for i in 1..n { let x = *k.add(i); let p = *v.add(i); let mut j = i; while j > 0 && *k.add(j - 1) > x { *k.add(j) = *k.add(j - 1); *v.add(j) = *v.add(j - 1); j -= 1; } *k.add(j) = x; *v.add(j) = p; }
        if out2 != 0 { memcpy(k2, k, n * size_of::<u64>()); memcpy(v2, v, n * size_of::<u32>()); }
        return;
    }
    let mut o: u64 = 0; let mut an: u64 = !0u64;
    for i in 0..n { o |= *k.add(i); an &= *k.add(i); }
    let diff = o ^ an;
    if diff == 0 { if out2 != 0 { memcpy(k2, k, n * size_of::<u64>()); memcpy(v2, v, n * size_of::<u32>()); } return; }
    let sh = (63 - diff.leading_zeros() as i32) & !7;
    let mut c = [0usize; 257];
    for i in 0..n { c[((*k.add(i) >> sh) & 0xff) as usize + 1] += 1; }
    for b in 0..256 { c[b + 1] += c[b]; }
    let st = c;
    for i in 0..n { let s = &mut c[((*k.add(i) >> sh) & 0xff) as usize]; let pos = *s; *s += 1; *k2.add(pos) = *k.add(i); *v2.add(pos) = *v.add(i); }
    for b in 0..256 { let s0 = st[b]; let m = st[b + 1] - s0; if m != 0 { rle_msd(k2.add(s0), v2.add(s0), k.add(s0), v.add(s0), m, (out2 == 0) as i32); } }
}
#[inline(never)]
pub(crate) unsafe fn rle_try_f64(a: *mut f64, n: usize) -> i32 {
    if n < RLE_MIN_N || n > u32::MAX as usize { return 0 }
    let mut eq = 0usize; let mut tot = 0usize;
    for w in 0..8usize {
        let lo = (n / 9) * (w + 1) - 1024;
        for i in lo..lo + 2048 { let x = (*a.add(i)).to_bits(); let y = (*a.add(i + 1)).to_bits(); eq += (x == y) as usize; tot += 1; }
    }
    if (eq as f64) < RLE_EQ_MIN * tot as f64 { return 0 }
    let cap = n / 3;
    let st = malloc((cap + 1) * size_of::<u32>()) as *mut u32; let v2 = malloc(cap * size_of::<u32>()) as *mut u32;
    let k = malloc(cap * size_of::<u64>()) as *mut u64; let k2 = malloc(cap * size_of::<u64>()) as *mut u64;
    let mut ok = 1;
    let mut m = 0usize;
    if ok != 0 {
        let mut prev = (*a).to_bits(); *st = 0; m = 1;
        for i in 1..n {
            let x = (*a.add(i)).to_bits();
            *st.add(m) = i as u32; m += (x != prev) as usize; prev = x;
            if m >= cap { ok = 0; break }
        }
    }
    if ok != 0 {
        *st.add(m) = n as u32;
        let v = st;
        for r in 0..m { *k.add(r) = rle_key(*a.add(*st.add(r) as usize)); *v.add(r) = (*st.add(r + 1)).wrapping_sub(*st.add(r)); }
        let mut asc = 1; let mut i = 1usize; while i < m && asc != 0 { asc = (*k.add(i - 1) <= *k.add(i)) as i32; i += 1; }
        if asc == 0 { rle_msd(k, v, k2, v2, m, 0); }
        let mut w = 0usize;
        for r in 0..m {
            let x = rle_unkey(*k.add(r)); let len = *v.add(r) as usize;
            if w + len + 8 <= n { let mut q = 0usize; while q < len { let d = a.add(w + q); *d = x; *d.add(1) = x; *d.add(2) = x; *d.add(3) = x; *d.add(4) = x; *d.add(5) = x; *d.add(6) = x; *d.add(7) = x; q += 8; } }
            else { for q in 0..len { *a.add(w + q) = x; } }
            w += len;
        }
        g_rle_sorts.fetch_add(1, Relaxed);
        TRACE!("rle: {} runs (mean {:.1}), {}", m, n as f64 / m as f64, if asc != 0 { "already in order" } else { "sorted" });
    } else { TRACE!("rle: gate passed ({} of {} pairs equal) but {} -> decline", eq, tot, "too many runs"); }
    free(st); free(v2); free(k); free(k2);
    ok
}

// ---- one dominant value (C 7836-7904) --------------------------------------------------------------------------------
pub(crate) const DOM_SHARE_PAR: f64 = 0.2;
pub(crate) const DOM_SHARE_SEQ: f64 = 0.85;
pub(crate) const DOM_MIN_N: usize = 65536;
pub(crate) static g_dom_sorts: AtomicUsize = AtomicUsize::new(0);   // logos_twist_last(33)

// mmap / munmap: std links libc on Linux, so the C's two calls are declared here (the crate has no dependencies).
#[cfg(target_os = "linux")]
mod dom_sys {
    use super::c_void;
    extern "C" {
        pub(crate) fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut c_void;
        pub(crate) fn munmap(addr: *mut c_void, len: usize) -> i32;
    }
    pub(crate) const PROT_READ: i32 = 1;
    pub(crate) const PROT_WRITE: i32 = 2;
    pub(crate) const MAP_PRIVATE: i32 = 2;
    pub(crate) const MAP_ANONYMOUS: i32 = 0x20;
    pub(crate) const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
}
#[cfg(target_os = "linux")]
pub(crate) unsafe fn dom_buf_get(bytes: usize) -> *mut f64 {
    let p = dom_sys::mmap(null_mut(), bytes, dom_sys::PROT_READ | dom_sys::PROT_WRITE, dom_sys::MAP_PRIVATE | dom_sys::MAP_ANONYMOUS, -1, 0);
    if p == dom_sys::MAP_FAILED { null_mut() } else { p as *mut f64 }
}
#[cfg(target_os = "linux")]
#[inline]
unsafe fn dom_buf_unmap(p: *mut f64, bytes: usize) { dom_sys::munmap(p as *mut c_void, bytes); }
#[cfg(not(target_os = "linux"))]
pub(crate) unsafe fn dom_buf_get(bytes: usize) -> *mut f64 { calloc(bytes, 1) as *mut f64 }
#[cfg(not(target_os = "linux"))]
#[inline]
unsafe fn dom_buf_unmap(p: *mut f64, bytes: usize) { free(p) }

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct domw_t {
    pub(crate) a: *mut f64, pub(crate) dst: *mut f64, pub(crate) lo: usize, pub(crate) hi: usize, pub(crate) ne: usize, pub(crate) lt: usize,
    pub(crate) off: usize, pub(crate) ob: usize, pub(crate) oa: usize, pub(crate) flo: usize, pub(crate) fhi: usize, pub(crate) n: usize,
    pub(crate) A: usize, pub(crate) vb: u64, pub(crate) v: f64,
}
pub(crate) unsafe fn dom_count(p: *mut c_void) -> *mut c_void {
    let w = p as *mut domw_t; let mut ne = 0usize; let mut lt = 0usize; let a = (*w).a as *const f64; let v = (*w).v; let vb = (*w).vb;
    for i in (*w).lo..(*w).hi { let x = *a.add(i); let b = x.to_bits(); ne += (b != vb) as usize; lt += (x < v) as usize; }
    (*w).ne = ne; (*w).lt = lt; null_mut()
}
pub(crate) unsafe fn dom_gather(p: *mut c_void) -> *mut c_void {
    let w = p as *mut domw_t; let d = (*w).dst.add((*w).off); let mut c = 0usize;
    for i in (*w).lo..(*w).hi { let x = *(*w).a.add(i); let b = x.to_bits(); *d.add(c) = x; c += (b != (*w).vb) as usize; }
    null_mut()
}
pub(crate) unsafe fn dom_place(p: *mut c_void) -> *mut c_void {
    let w = p as *mut domw_t; let s = (*w).dst.add((*w).off) as *const f64; let mut pb = (*w).a.add((*w).ob); let mut pa = (*w).a.add(((*w).n - (*w).A) + (*w).oa); let v = (*w).v;
    for i in 0..(*w).ne { let x = *s.add(i); if x < v { *pb = x; pb = pb.add(1); } else { *pa = x; pa = pa.add(1); } }
    for i in (*w).flo..(*w).fhi { *(*w).a.add(i) = v; }
    null_mut()
}
#[inline(never)]
pub(crate) unsafe fn dominant_try_f64(a: *mut f64, n: usize, P: i32, samp: *const f64, m: usize) -> i32 {
    if n < DOM_MIN_N || m < 64 { return 0 }
    let mut best = 1usize; let mut bi = 0usize; let mut run = 1usize;
    for i in 1..m {
        let x = (*samp.add(i)).to_bits(); let y = (*samp.add(i - 1)).to_bits();
        run = if x == y { run + 1 } else { 1 }; if run > best { best = run; bi = i; }
    }
    let share = best as f64 / m as f64; let need = if P > 1 { DOM_SHARE_PAR } else { DOM_SHARE_SEQ };
    if share < need || *samp.add(bi) != *samp.add(bi) { return 0 }
    let v = *samp.add(bi); let vb = v.to_bits();
    let T = if P > 1 { P } else { 1 }; let Tu = T as usize; let chunk = (n + Tu - 1) / Tu;
    let w = calloc(Tu, size_of::<domw_t>()) as *mut domw_t; let th = malloc(Tu * size_of::<pthread_t>()) as *mut pthread_t;
    for t in 0..Tu { let x = w.add(t); (*x).a = a; (*x).vb = vb; (*x).v = v; (*x).n = n; (*x).lo = if t * chunk < n { t * chunk } else { n }; (*x).hi = if (*x).lo + chunk < n { (*x).lo + chunk } else { n }; }
    par_run(th, T, dom_count, w as *mut c_void, size_of::<domw_t>(), 1);
    let mut k = 0usize; let mut B = 0usize; let mut A = 0usize;
    let mut slots = 0usize;
    for t in 0..Tu { let x = w.add(t); (*x).off = slots; (*x).ob = B; (*x).oa = A; k += (*x).ne; B += (*x).lt; A += (*x).ne - (*x).lt; slots += (*x).ne + 1; }
    TRACE!("dominant: share {:.3} in the sample; {} below the value, {} above, {} copies of it", share, B, A, n - k);
    if k == 0 { free(w); free(th); g_dom_sorts.fetch_add(1, Relaxed); return 1 }
    let buf = dom_buf_get(slots * size_of::<f64>());
    if buf.is_null() { free(w); free(th); return 0 }
    let V = n - k; let fc = (V + Tu - 1) / Tu;
    for t in 0..Tu { let x = w.add(t); (*x).dst = buf; (*x).A = A; (*x).flo = B + (if t * fc < V { t * fc } else { V }); (*x).fhi = B + (if t * fc + fc < V { t * fc + fc } else { V }); }
    par_run(th, T, dom_gather, w as *mut c_void, size_of::<domw_t>(), 1);
    par_run(th, T, dom_place, w as *mut c_void, size_of::<domw_t>(), 1);
    dom_buf_unmap(buf, slots * size_of::<f64>()); free(w); free(th);
    if B > 1 { psort_f64(a, B, P, null_mut(), null_mut(), null_mut(), null_mut()); }
    if A > 1 { psort_f64(a.add(n - A), A, P, null_mut(), null_mut(), null_mut(), null_mut()); }
    g_dom_sorts.fetch_add(1, Relaxed);
    1
}

// ---- tests -----------------------------------------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;
    static LOCK: Mutex<()> = Mutex::new(());
    fn lock() -> std::sync::MutexGuard<'static, ()> { LOCK.lock().unwrap_or_else(|e| e.into_inner()) }

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
        fn below(&mut self, k: usize) -> usize { ((self.next() >> 11) % k as u64) as usize }
        fn unit(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
    }
    fn shuffle(a: &mut [f64], r: &mut Rng) { for i in (1..a.len()).rev() { let j = r.below(i + 1); a.swap(i, j); } }
    /// The sample as the dispatcher's gates see it: Weyl positions, min(nbk0 * OVERSAMPLE, 2048) points, sorted.
    fn disp_sample(a: &[f64], P: i32) -> (Vec<f64>, i32) {
        let n = a.len(); let p = effective_threads(n, P);
        let mut nbk0 = p * BUCKETS_PER_THREAD; if nbk0 > MAX_BUCKETS { nbk0 = p * (if MAX_BUCKETS / p > 0 { MAX_BUCKETS / p } else { 1 }) }
        let mfull = nbk0 as usize * OVERSAMPLE; let m = mfull.min(2048).min(n);
        let mut s: Vec<f64> = (0..m).map(|i| a[SAMPLE_POS(i, mfull, n)]).collect();
        s.sort_by(|x, y| x.partial_cmp(y).unwrap());
        (s, p)
    }
    fn bits_sorted(a: &[f64]) -> Vec<u64> { let mut b: Vec<u64> = a.iter().map(|x| x.to_bits()).collect(); b.sort_unstable(); b }
    fn check_sorted_perm(inp: &[f64], out: &[f64], what: &str) {
        for i in 1..out.len() { assert!(!(out[i] < out[i - 1]), "{}: descent at {}", what, i); }
        assert!(bits_sorted(inp) == bits_sorted(out), "{}: not a permutation of the input's bit patterns", what);
    }
    fn check_perm(inp: &[f64], out: &[f64], what: &str) { assert!(bits_sorted(inp) == bits_sorted(out), "{}: not a permutation", what); }
    fn uniform(n: usize, seed: u64) -> Vec<f64> { let mut r = Rng(seed); (0..n).map(|_| r.unit() * 1e6 - 3e5).collect() }
    const PS: [i32; 3] = [1, 4, 14];

    #[test]
    fn few_values_taken() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let vals = [f64::NEG_INFINITY, -0.0, 0.0, 1.5, f64::INFINITY];
        for &n in &[150_000usize, 2_500_000] { for &P in &PS {
            let mut r = Rng(0x1234 + n as u64);
            let inp: Vec<f64> = (0..n).map(|_| vals[r.below(5)]).collect();
            let (s, p) = disp_sample(&inp, P);
            let mut a = inp.clone();
            g_lcvec_elems.store(0, SeqCst);
            let took = unsafe { lowcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()) };
            assert_eq!(took, 1, "few values n={} P={}", n, P);
            check_sorted_perm(&inp, &a, "few values");
            if logos_simd_kind() == 2 { assert!(g_lcvec_elems.load(SeqCst) > n / 2, "the vector count did not run"); }
            // and without a sample handed in (the route draws its own)
            let mut b = inp.clone();
            assert_eq!(unsafe { lowcard_try_f64(b.as_mut_ptr(), n, p, null(), 0) }, 1);
            check_sorted_perm(&inp, &b, "few values, own sample");
        } }
    }
    #[test]
    fn few_values_sample_misses_some() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 200_000; let mut r = Rng(77);
        let mut inp: Vec<f64> = (0..n).map(|_| (r.below(40)) as f64 * 1.25).collect();
        for j in 0..100 { inp[j * 1999 + 3] = 1e9 + j as f64; }     // 100 values no sample of 384 holds all of
        for &P in &PS {
            let (s, p) = disp_sample(&inp, P);
            let mut a = inp.clone();
            assert_eq!(unsafe { lowcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()) }, 1);
            check_sorted_perm(&inp, &a, "few values + missed");
        }
    }
    #[test]
    fn hashed_counts_taken_and_capped() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let mut r = Rng(99);
        let vals: Vec<f64> = (0..200).map(|_| r.unit() * 1e3 - 500.0).collect();
        for &n in &[300_000usize, 1_500_000] { for &P in &PS {
            let inp: Vec<f64> = (0..n).map(|_| vals[r.below(200)]).collect();
            let (s, p) = disp_sample(&inp, P);
            for &allow in &[1, 2] {
                let mut a = inp.clone();
                assert_eq!(unsafe { modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), allow) }, 1, "200 values n={} P={}", n, P);
                check_sorted_perm(&inp, &a, "200 values");
            }
            let mut a = inp.clone();
            assert_eq!(unsafe { modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 0) }, 0, "hash not allowed and no lattice: decline");
            assert!(a.iter().zip(&inp).all(|(x, y)| x.to_bits() == y.to_bits()));
        } }
        // 20,000 values under the sequential cap: gives up, untouched, the witness counts it
        let n = 300_000; let big: Vec<f64> = (0..20_000).map(|_| r.unit()).collect();
        let inp: Vec<f64> = (0..n).map(|_| big[r.below(20_000)]).collect();
        let (s, p) = disp_sample(&inp, 1);
        let mut a = inp.clone(); g_mc_capped.store(0, SeqCst);
        assert_eq!(unsafe { modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 2) }, 0);
        assert_eq!(g_mc_capped.load(SeqCst), 1);
        assert!(a.iter().zip(&inp).all(|(x, y)| x.to_bits() == y.to_bits()));
        let mut a = inp.clone();
        assert_eq!(unsafe { modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 1) }, 1);
        check_sorted_perm(&inp, &a, "20,000 values, uncapped");
    }
    #[test]
    fn hash_gate() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            assert_eq!(mc_hash_allowed(100.0, 1, 1 << 20, null(), 0), 1);
            assert_eq!(mc_hash_allowed(3000.0, 14, 1 << 20, null(), 0), 1);
            assert_eq!(mc_hash_allowed(4000.0, 14, 1 << 20, null(), 0), 0);
            let ints: Vec<f64> = (0..384).map(|i| (i * 7) as f64).collect();
            assert_eq!(mc_hash_allowed(1500.0, 1, 1 << 20, ints.as_ptr(), ints.len()), 2);
            let u = uniform(384, 5);
            assert_eq!(mc_hash_allowed(1500.0, 1, 1 << 20, u.as_ptr(), u.len()), 0);
        }
    }
    fn lattice_case(n: usize, f: impl Fn(usize) -> f64, what: &str) {
        for &P in &PS {
            let mut r = Rng(0xABCD ^ n as u64 ^ P as u64);
            let inp: Vec<f64> = (0..n).map(|_| f(r.below(30_000))).collect();
            let (s, p) = disp_sample(&inp, P);
            let mut a = inp.clone();
            assert_eq!(unsafe { modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 0) }, 1, "{} P={}", what, P);
            check_sorted_perm(&inp, &a, what);
            let mut b = inp.clone();
            assert_eq!(unsafe { lattice_try_f64(b.as_mut_ptr(), n, count_threads(n, p), s.as_ptr(), s.len()) }, 1, "{} direct P={}", what, P);
            check_sorted_perm(&inp, &b, what);
            if P == 1 { assert_eq!(unsafe { lat1_gate(s.as_ptr(), s.len(), n, 1) }, 1, "{}: lat1_gate", what); }
        }
    }
    #[test]
    fn lattice_exact_taken() { let _port_guard = crate::port::port_test_lock(); let _g = lock(); lattice_case(2_200_000, |k| k as f64 * 0.25 + 100.0, "exact lattice"); }
    #[test]
    fn lattice_decimal_taken() { let _port_guard = crate::port::port_test_lock(); let _g = lock(); lattice_case(2_200_000, |k| k as f64 * 0.01, "decimal lattice"); }
    #[test]
    fn lattice_spills() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 2_200_000;
        for &P in &PS {
            let mut r = Rng(31 + P as u64);
            let mut inp: Vec<f64> = (0..n).map(|_| r.below(30_000) as f64 * 0.25).collect();
            for j in 0..200 { inp[j * 10_007 + 11] = r.below(30_000) as f64 * 0.25 + 0.1; }     // off the lattice
            inp[n - 5] = -1e12; inp[n / 2] = 1e15; inp[7] = -0.0;                                // far sentinels, a second zero
            let (s, p) = disp_sample(&inp, P);
            let mut a = inp.clone(); g_lat_spilled.store(0, SeqCst);
            let took = unsafe { lattice_try_f64(a.as_mut_ptr(), n, count_threads(n, p), s.as_ptr(), s.len()) };
            // With the real count_threads (the files joined) the fill gate may decline at some thread counts, as the C does:
            // a decline must leave a permutation. Which thread counts take it is checked against C by parity/routes.sh
            // (shape lattice_offs).
            if took == 1 { assert!(g_lat_spilled.load(SeqCst) >= 200, "spills not counted"); check_sorted_perm(&inp, &a, "lattice with spills"); }
            else { let (mut x, mut y): (Vec<u64>, Vec<u64>) = (inp.iter().map(|v| v.to_bits()).collect(), a.iter().map(|v| v.to_bits()).collect()); x.sort_unstable(); y.sort_unstable(); assert_eq!(x, y, "a declined lattice is still a permutation P={}", P); }
        }
    }
    #[test]
    fn dense_integers() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 200_000; let mut r = Rng(5);
        let mut inp: Vec<f64> = (0..n).map(|i| i as f64).collect(); shuffle(&mut inp, &mut r);
        let (s, _) = disp_sample(&inp, 1);
        let mut a = inp.clone();
        let before = unsafe { g_dense_count };
        assert_eq!(unsafe { dense_int_try_f64(a.as_mut_ptr(), n, s.as_ptr(), s.len()) }, 1);
        assert_eq!(unsafe { g_dense_count }, before + 1);
        check_sorted_perm(&inp, &a, "dense integers");
        // negative base, span 8n
        let mut inp2: Vec<f64> = (0..n).map(|i| (i as i64 * 8 - 500_000) as f64).collect(); shuffle(&mut inp2, &mut r);
        let (s2, _) = disp_sample(&inp2, 1);
        let mut a2 = inp2.clone();
        assert_eq!(unsafe { dense_int_try_f64(a2.as_mut_ptr(), n, s2.as_ptr(), s2.len()) }, 1);
        check_sorted_perm(&inp2, &a2, "dense integers, span 8n");
        // one repeat, one -0.0, one half: each declines with the array untouched
        for (pos, val) in [(1000usize, inp[2000]), (1000, -0.0), (1000, 0.5), (1000, f64::NAN), (1000, 1e300)] {
            let mut c = inp.clone(); c[pos] = val; let keep = c.clone();
            assert_eq!(unsafe { dense_int_try_f64(c.as_mut_ptr(), n, s.as_ptr(), s.len()) }, 0);
            assert!(c.iter().zip(&keep).all(|(x, y)| x.to_bits() == y.to_bits()));
        }
    }
    #[test]
    fn run_lengths() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 400_000; let mut r = Rng(8);
        let mut inp = Vec::with_capacity(n);
        while inp.len() < n { let v = match r.below(50) { 0 => -0.0, 1 => 0.0, 2 => f64::INFINITY, 3 => f64::NEG_INFINITY, _ => r.unit() * 2e3 - 1e3 }; for _ in 0..8 { if inp.len() < n { inp.push(v) } } }
        let mut a = inp.clone(); g_rle_sorts.store(0, SeqCst);
        assert_eq!(unsafe { rle_try_f64(a.as_mut_ptr(), n) }, 1);
        assert_eq!(g_rle_sorts.load(SeqCst), 1);
        check_sorted_perm(&inp, &a, "runs of 8");
        // runs already in order
        let mut inp2: Vec<f64> = (0..n).map(|i| (i / 8) as f64).collect();
        let keep = inp2.clone();
        assert_eq!(unsafe { rle_try_f64(inp2.as_mut_ptr(), n) }, 1);
        check_sorted_perm(&keep, &inp2, "runs in order");
    }
    #[test]
    fn dominant_value() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        for &(P, share, n) in &[(1, 0.9, 300_000usize), (4, 0.5, 1_500_000), (14, 0.5, 1_500_000), (14, 0.25, 1_500_000)] {
            let mut r = Rng(17 + P as u64);
            let inp: Vec<f64> = (0..n).map(|i| if r.unit() < share { 7.25 } else if i % 1000 == 0 { -0.0 } else if i % 1000 == 1 { 0.0 } else { r.unit() * 200.0 - 100.0 }).collect();
            let (s, p) = disp_sample(&inp, P);
            let mut a = inp.clone(); g_dom_sorts.store(0, SeqCst);
            assert_eq!(unsafe { dominant_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()) }, 1, "dominant P={} share={}", P, share);
            assert_eq!(g_dom_sorts.load(SeqCst), 1);
            check_sorted_perm(&inp, &a, "dominant value");
        }
        // every element the value
        let n = 100_000; let inp = vec![3.5f64; n]; let (s, p) = disp_sample(&inp, 1);
        let mut a = inp.clone();
        assert_eq!(unsafe { dominant_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()) }, 1);
        check_sorted_perm(&inp, &a, "one value");
    }
    #[test]
    fn uniform_declined() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        for &n in &[200_000usize, 1_500_000] { for &P in &PS {
            let inp = uniform(n, 4242 + n as u64);
            let (s, p) = disp_sample(&inp, P);
            unsafe {
                let mut a = inp.clone();
                assert_eq!(lowcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()), 0); check_perm(&inp, &a, "lowcard decline");
                assert_eq!(lowcard_try_f64(a.as_mut_ptr(), n, p, null(), 0), 0); check_perm(&inp, &a, "lowcard decline, own sample");
                assert_eq!(modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 0), 0); check_perm(&inp, &a, "modcard decline");
                assert_eq!(modcard_try_f64(a.as_mut_ptr(), n, p, null(), 0, 1), 0); check_perm(&inp, &a, "modcard decline, own sample");
                assert_eq!(modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 2), 0); check_perm(&inp, &a, "modcard capped decline");
                assert_eq!(lattice_try_f64(a.as_mut_ptr(), n, count_threads(n, p), s.as_ptr(), s.len()), 0); check_perm(&inp, &a, "lattice decline");
                assert_eq!(lat1_gate(s.as_ptr(), s.len(), n, p), 0);
                assert_eq!(dense_int_try_f64(a.as_mut_ptr(), n, s.as_ptr(), s.len()), 0); check_perm(&inp, &a, "dense decline");
                assert_eq!(rle_try_f64(a.as_mut_ptr(), n), 0); check_perm(&inp, &a, "rle decline");
                assert_eq!(dominant_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()), 0); check_perm(&inp, &a, "dominant decline");
                assert!(a.iter().zip(&inp).all(|(x, y)| x.to_bits() == y.to_bits()), "a decline moved something");
            }
        } }
    }
    #[test]
    fn nan_declines_untouched() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        let n = 200_000; let mut r = Rng(3);
        let mut inp: Vec<f64> = (0..n).map(|_| r.below(5) as f64).collect(); inp[n - 3] = f64::NAN;
        let clean: Vec<f64> = inp.iter().cloned().filter(|x| x == x).collect();
        let (s, p) = disp_sample(&clean, 4);
        let mut a = inp.clone();
        unsafe {
            assert_eq!(lowcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len()), 0);
            assert_eq!(modcard_try_f64(a.as_mut_ptr(), n, p, s.as_ptr(), s.len(), 1), 0);
        }
        assert!(a.iter().zip(&inp).all(|(x, y)| x.to_bits() == y.to_bits()));
    }
    #[test]
    fn pieces() { let _port_guard = crate::port::port_test_lock();
        let _g = lock();
        unsafe {
            // lc_sort_pairs: the insertion case and the radix
            for &n in &[1usize, 30, 48, 49, 5000] {
                let mut r = Rng(n as u64 + 1);
                let mut p: Vec<lcpair_t> = (0..n).map(|i| lcpair_t { k: total_key(r.unit() * 100.0 - 50.0), v: i as f64, c: i }).collect();
                let mut want: Vec<u64> = p.iter().map(|x| x.k).collect(); want.sort_unstable();
                let mut tmp = p.clone();
                lc_sort_pairs(p.as_mut_ptr(), n, tmp.as_mut_ptr());
                assert!(p.iter().map(|x| x.k).eq(want.iter().cloned()));
            }
            // rle_msd, both landing sides
            for &n in &[40usize, 3000] { for out2 in 0..2 {
                let mut r = Rng(n as u64 * 3 + out2 as u64);
                let mut k: Vec<u64> = (0..n).map(|_| rle_key(r.unit() * 1e6 - 5e5)).collect();
                let mut v: Vec<u32> = (0..n as u32).collect(); let orig = k.clone();
                let mut k2 = vec![0u64; n]; let mut v2 = vec![0u32; n];
                rle_msd(k.as_mut_ptr(), v.as_mut_ptr(), k2.as_mut_ptr(), v2.as_mut_ptr(), n, out2);
                let (ok, ov) = if out2 != 0 { (&k2, &v2) } else { (&k, &v) };
                for i in 1..n { assert!(ok[i - 1] <= ok[i]); }
                for i in 0..n { assert_eq!(orig[ov[i] as usize], ok[i]); }
            } }
            for &x in &[0.0, -0.0, 1.5, -2.5, f64::INFINITY, f64::NEG_INFINITY, f64::MIN_POSITIVE] { assert_eq!(rle_unkey(rle_key(x)).to_bits(), x.to_bits()); assert_eq!(rle_key(x), total_key(x)); }
            // lattice_spill_finish
            let mut a: Vec<f64> = (0..1000).map(|i| i as f64).collect(); a.extend_from_slice(&[0.0; 6]);
            let mut sp = vec![2000.0, -5.0, 500.5, 500.5, 0.0, 999.0];
            let mut want = a[..1000].to_vec(); want.extend_from_slice(&sp); want.sort_by(|x, y| x.partial_cmp(y).unwrap());
            lattice_spill_finish(a.as_mut_ptr(), 1000, sp.as_mut_ptr(), 6);
            assert!(a == want);
            // hmap
            let mut h: hmap_t = core::mem::zeroed(); hmap_init(&mut h, 16);
            for i in 0..5000u64 { assert_eq!(hmap_add(&mut h, i % 1000, i as f64, 1, 1000), 1); }
            assert_eq!(h.used, 1000); assert_eq!(hmap_add(&mut h, 5000, 0.0, 1, 1000), 0);
            hmap_free(&mut h);
            // lattice_sample_step: a sample with no two adjacent lattice points still finds the step
            let s: Vec<f64> = [0, 3, 9, 14, 20, 27, 33].iter().map(|&k| k as f64 * 0.5).collect();
            let (mut d, mut dv) = (0usize, 0i32);
            assert_eq!(lattice_sample_step(s.as_ptr(), s.len(), &mut d, &mut dv), 0.5); assert_eq!(d, 7); assert_eq!(dv, 3);
            if logos_simd_kind() == 2 {
                let a = [1.0, -0.0, 0.0, 1.0, 0.0, 0.0, -0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 7.0, 1.0];
                let vals = [-0.0, 0.0, 1.0]; let mut cnt = [0usize; 3];
                assert_eq!(lc_vec_count(a.as_ptr(), 0, a.len(), vals.as_ptr(), 3, cnt.as_mut_ptr()), 8);
                assert_eq!(cnt, [2, 3, 3]);
            }
        }
    }
}

// ---- outside this file's range (stubs with real minimal bodies, so the tests above run) ---------------------------------
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    pub(crate) type pthread_t = usize;                                // base.rs
    /// base.rs (C 3307): here, every worker in turn on the calling thread.
    pub(crate) unsafe fn par_run(th: *mut pthread_t, P: i32, f: unsafe fn(*mut c_void) -> *mut c_void, args: *mut c_void, stride: usize, run0: i32) {
        for t in 0..P as usize { if t == 0 && run0 == 0 { continue } f((args as *mut u8).add(t * stride) as *mut c_void); }
    }
    pub(crate) const COUNT_MIN_PER_THREAD: usize = 350000;            // leaf.rs (C 1904-1925)
    pub(crate) const COUNT_MIN_THREADS: usize = 3;
    pub(crate) const COUNT_MAX_THREADS: i32 = 0;
    pub(crate) const g_count_max: i32 = COUNT_MAX_THREADS;
    #[inline] pub(crate) unsafe fn count_threads(n: usize, P: i32) -> i32 { let mut pm = n / COUNT_MIN_PER_THREAD; if pm < COUNT_MIN_THREADS { pm = COUNT_MIN_THREADS } if P as usize > pm { pm as i32 } else { P } }
    #[inline] pub(crate) unsafe fn count_threads_hash(n: usize, P: i32) -> i32 { let mut r = count_threads(n, P); if g_count_max > 0 && r > g_count_max { r = g_count_max } r }
    #[inline] pub(crate) unsafe fn avx2_leaf_on() -> i32 { (logos_simd_kind() == 2) as i32 }                      // base.rs (C 852)
    #[inline] pub(crate) unsafe fn key_parts(v: f64, tl: *mut u32, th: *mut u32) {                                 // leaf.rs (C 2739)
        let u = v.to_bits(); let lo = u as u32; let hi = (u >> 32) as u32;
        if (hi >> 31) != 0 { *tl = !lo; *th = !hi } else { *tl = lo; *th = hi | 0x80000000 }
    }
    /// base.rs (C 4816): NaNs swapped to the tail, the rest sorted; returns the NaN count.
    pub(crate) unsafe fn sample_sort(s: *mut f64, m: usize) -> usize {
        let mut k = m; let mut i = 0;
        while i < k { if *s.add(i) != *s.add(i) { let v = *s.add(i); k -= 1; *s.add(i) = *s.add(k); *s.add(k) = v; } else { i += 1 } }
        core::slice::from_raw_parts_mut(s, k).sort_unstable_by(|x, y| x.partial_cmp(y).unwrap());
        m - k
    }
    /// disp.rs (C 7620): here, the standard sort.
    pub(crate) unsafe fn psort_f64(a: *mut f64, n: usize, P: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) {
        core::slice::from_raw_parts_mut(a, n).sort_unstable_by(|x, y| x.partial_cmp(y).unwrap_or(core::cmp::Ordering::Equal));
    }
    pub(crate) static mut g_ctx_next: i32 = 0;                        // base.rs (C 404)
    thread_local! { pub(crate) static ns_depth: Cell<i32> = Cell::new(0); }   // ord1.rs (C 5812)
}
#[cfg(not(feature = "port_all"))]
use missing::*;
