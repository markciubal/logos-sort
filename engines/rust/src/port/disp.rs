//! C lines 8794-9458 and 10097-10349: the dispatcher (`psort_f64`), the route witnesses (`logos_twist_last`,
//! `logos_reach_last`), the thin front and the entry (`logos_sort_f64`). See PORTING.md.
//!
//! Not ported here: the profile hooks, the environment switches read at the top of the dispatcher (each flag is a const
//! with its unset value in the file that owns it), the optional x86-simd-sort half (LOGOS_XSS_LEAF) and the
//! out-of-memory fallback (`qsort`).
use super::*;

pub(crate) static mut g_thin_calls: usize = 0;

/// C line 8794.
#[inline(never)]
pub(crate) unsafe fn psort_f64(a: *mut f64, n: usize, P: i32, vstart: *mut usize, vnbk: *mut i32, vmin: *mut f64, vmax: *mut f64) {
    let mut P = P;
    if !vnbk.is_null() { *vnbk = 0 }
    if P < 1 { P = 1 }
    if n < 2 { return }
    if P as usize > n { P = n as i32 }
    let Preq = P;
    P = effective_threads(n, P);
    TRACE_START!(); TRACE!("psort_f64 n={} P={}", n, P);
    if n >= MIN_PARALLEL { simd_merge_env() }                      /* C line 8810: the switches read from 65,536 elements up; only this one is not a const here */
    if n >= REACH_MIN_N as usize && ns_depth.with(|d| d.get()) == 0 { g_reach_last.with(|r| r.set(LOGOS_REACH_NOT_TRIED)) }

    /* whole-array early exits */
    {
        let (mut asc, mut desc) = (1i32, 1i32); let mut first_desc = n;
        if n >= PRESCAN_FAST_MIN as usize {
            let ps = if P > 1 && n >= PRESCAN_PAR_MIN as usize { prescan_par(a, n, P) } else { prescan_blocks(a, n) };
            asc = ps.asc; desc = ps.desc; first_desc = ps.first_desc;
        } else {
            let mut i = 1usize;
            while i < n && (asc != 0 || desc != 0) {
                if asc != 0 && *a.add(i) < *a.add(i - 1) { asc = 0; first_desc = i }
                if *a.add(i) > *a.add(i - 1) { desc = 0 }
                i += 1;
            }
        }
        if asc != 0 { return }
        if desc != 0 {
            let (mut l, mut r) = (0usize, n - 1);
            while l < r { let t = *a.add(l); *a.add(l) = *a.add(r); *a.add(r) = t; l += 1; r -= 1 }
            return;
        }
        /* the loop-back: an ascending suffix that continues into the ascending prefix across the wrap is one strand */
        if n >= LOOPBACK_MIN_N as usize && g_looped == 0 && vstart.is_null() {
            let s = loopback_gate(a, n, first_desc, (P > 1 && n >= MIN_PARALLEL) as i32);
            if s != 0 {
                let known = n - s + first_desc;                     /* after the rotation [0, known) ascends: suffix, then prefix */
                TRACE!("loop-back: prefix {}, suffix {} continues into it across the wrap: rotated ({} known ascending)", first_desc, n - s, known);
                rotate_left_f64(a, n, s); g_loop_count += 1;
                if known >= n { return }                            /* the whole ring was one strand */
                g_looped = 1;
                if tail_merge_try_f64(a, n, known, P) != 0 {
                    TRACE!("loop-back: tail route took the remainder ({})", n - known);
                    g_looped = 0; return;
                }
                psort_f64(a, n, P, null_mut(), null_mut(), null_mut(), null_mut());        /* re-enter plainly */
                g_looped = 0;
                return;
            }
        }
        /* a sorted prefix with an unsorted tail; an unsorted head before a sorted suffix */
        if first_desc >= n / 2 && n >= TAIL_MIN_N as usize && tail_merge_try_f64(a, n, first_desc, P) != 0 {
            TRACE!("tail: sorted prefix {}, tail {} sorted and merged", first_desc, n - first_desc);
            return;
        }
        if first_desc < n / 2 && n >= TAIL_MIN_N as usize && (P == 1 || n < MIN_PARALLEL) && head_merge_try_f64(a, n, P) != 0 {
            TRACE!("head: an unsorted head before a sorted suffix, sorted and merged forward");
            return;
        }
    }
    TRACE!("prescan done");
    let mut SO: sodo_t = core::mem::zeroed(); let mut sodo_ok = 0i32;
    let Pb = if P > 1 && Preq > P { boost_threads(n, P, Preq) } else { P };      /* the unstructured partition's own floor */
    {
        let mut nbk0 = P * BUCKETS_PER_THREAD;
        if nbk0 > MAX_BUCKETS { nbk0 = P * (if MAX_BUCKETS / P > 0 { MAX_BUCKETS / P } else { 1 }) }
        if n >= 4096 { sodo_ok = sodo_sample(a, n, nbk0 as usize * OVERSAMPLE, &mut SO) }
    }
    if sodo_ok != 0 { TRACE!("sodo: m={} distinct={} chao1={:.0} inv={:.3} inv_all={:.3} reach={} ks={:.4} nan={}", SO.m, SO.distinct, SO.chao1, SO.inv_rate, SO.inv_all, SO.reach, SO.ks, SO.has_nan) }
    /* dense consecutive integers with no repeats: presence bits. Sequential sorts only. */
    if (P == 1 || n < MIN_PARALLEL) && sodo_ok != 0 && SO.has_nan == 0 && dense_int_try_f64(a, n, SO.sorted, SO.m) != 0 {
        free(SO.sorted);
        return;
    }
    if sodo_ok != 0 && SO.has_nan == 0 && SO.distinct <= LOWCARD_MAX_SCAN as usize && SO.chao1 <= (LOWCARD_CAP / 2) as f64
        && lowcard_try_f64(a, n, P, SO.sorted, SO.m) != 0 { free(SO.sorted); return }
    {
        let gate = (if P >= WC_MIN_THREADS { MODCARD_GATE_MT } else { MODCARD_GATE }) as f64 * SO.m as f64;
        if sodo_ok != 0 && SO.has_nan == 0 && n >= MODCARD_MIN_N as usize && SO.distinct as f64 <= gate && SO.chao1 <= MODCARD_CAP as f64
            && modcard_try_f64(a, n, P, SO.sorted, SO.m, mc_hash_allowed(SO.chao1, P, n, SO.sorted, SO.m)) != 0 { free(SO.sorted); return }
        /* a lattice the sample cannot see repeat (lat1_gate); only where the block above was not entered */
        if sodo_ok != 0 && SO.has_nan == 0 && n >= MODCARD_MIN_N as usize && SO.distinct as f64 > gate
            && lat1_gate(SO.sorted, SO.m, n, if n < MIN_PARALLEL { 1 } else { P }) != 0
            && lattice_try_f64(a, n, if n < MIN_PARALLEL { 1 } else { count_threads(n, P) }, SO.sorted, SO.m) != 0 {
            g_lat1_sorts.fetch_add(1, Relaxed);
            free(SO.sorted); return;
        }
    }
    /* one value holding a large share of the array */
    if sodo_ok != 0 && SO.has_nan == 0 && dominant_try_f64(a, n, P, SO.sorted, SO.m) != 0 { free(SO.sorted); return }
    /* short reach: every element near its final place */
    if sodo_ok != 0 && SO.reach != 0 && reach_try_f64(a, n, P, SO.reach) != 0 { free(SO.sorted); return }
    if g_reach_seam != 0 {                                          /* the reach route found a rotation in its fences: rotate once, enter again */
        let sm = g_reach_seam; g_reach_seam = 0;
        if sodo_ok != 0 { free(SO.sorted); SO.sorted = null_mut() }
        TRACE!("seam: rotated by {}", sm);
        rotate_left_f64(a, n, sm); g_seam_count += 1; g_seamed = 1;
        psort_f64(a, n, P, null_mut(), null_mut(), null_mut(), null_mut());
        g_seamed = 0;
        return;
    }

    if P == 1 || n < MIN_PARALLEL {
        let mut st_bnd = [0.0f64; STREAMS_MAX_MODES as usize]; let (mut st_lo, mut st_hi) = (0.0f64, 0.0f64); let mut st_nm = 0i32;   /* read the modes before the sample is freed */
        if sodo_ok != 0 && SO.has_nan == 0 && n >= STREAMS_MIN_N as usize {
            st_nm = streams_modes(SO.sorted, SO.m, st_bnd.as_mut_ptr());
            if st_nm >= 2 { st_lo = *SO.sorted; st_hi = *SO.sorted.add(SO.m - 1) }
        }
        if sodo_ok != 0 { free(SO.sorted); SO.sorted = null_mut() }
        let mut pb: *mut c_void = null_mut(); let mut pbi: *mut u8 = null_mut();
        let pooled = pool_acquire(n * size_of::<f64>(), 0, &mut pb, &mut pbi);
        let buf: *mut f64 = if pooled != 0 { pb as *mut f64 } else { big_alloc(n * size_of::<f64>()) as *mut f64 };
        let mut c: ctx_t = core::mem::zeroed();
        ctx_init(&mut c, n, 0);
        c.seq = 1;
        TRACE!("seq: buffers ready (pooled={})", pooled);
        macro_rules! seq_done { () => {{ if pooled != 0 { pool_release() } else { free(buf) } ctx_free(&mut c); return; }}; }
        if n >= RELOC_MIN_N as usize && relocate_try_f64(a, n, 1, buf, null_mut()) != 0 { TRACE!("seq: relocated, done"); seq_done!() }   /* J5 */
        TRACE!("seq: relocation declined");
        /* the whole array's orientation bit: see mirror_gate */
        let mirror_ask = sodo_ok != 0 && (SO.inv_all >= MIRROR_INV_MIN as f64 || st_nm >= 2);
        if sodo_ok != 0 && SO.has_nan == 0 && mirror_ask && n >= MIRROR_MIN_N as usize && g_mirrored == 0 && vstart.is_null() {
            let mg = mirror_gate(a, n, SO.inv_all, st_bnd.as_mut_ptr(), st_nm);
            TRACE!("mirror: inv_all={:.3}, gate {} (1 descending suffix, 2 reach, 3 streams)", SO.inv_all, mg);
            if mg != 0 {
                if pooled != 0 { pool_release() } else { free(buf) }
                ctx_free(&mut c);
                let (mut l, mut r) = (0usize, n - 1);
                while l < r { let x = *a.add(l); *a.add(l) = *a.add(r); *a.add(r) = x; l += 1; r -= 1 }
                g_mirror_count += 1; g_mirrored = 1;
                psort_f64(a, n, P, null_mut(), null_mut(), null_mut(), null_mut());          /* re-enter */
                g_mirrored = 0;
                return;
            }
        }
        if st_nm >= 2 && streams_try_f64(a, n, buf, &mut c, st_bnd.as_mut_ptr(), st_nm, st_lo, st_hi) != 0 { seq_done!() }
        /* the sample in position order: the seam route, the wave route, the cuts route */
        if sodo_ok != 0 && SO.has_nan == 0 && n >= WAVE_MIN_N as usize && vstart.is_null() && g_seamed == 0 {
            let ps = sample_by_pos(a, n, SODO_GATE_M as usize);
            let wavey = SO.inv_all <= WAVE_INV_MAX as f64;            /* the seam and the wave need a mostly ascending sample */
            if !ps.is_null() {
                let seam = if wavey { seam_gate(a, n, ps, SODO_GATE_M as usize) } else { 0 };
                if seam != 0 {
                    TRACE!("seam: a noisy ring buffer, seam at {}: rotated", seam);
                    free(ps); if pooled != 0 { pool_release() } else { free(buf) } ctx_free(&mut c);
                    rotate_left_f64(a, n, seam); g_seam_count += 1; g_seamed = 1;
                    psort_f64(a, n, P, null_mut(), null_mut(), null_mut(), null_mut());
                    g_seamed = 0; return;
                }
                if wavey && wave_try_f64(a, n, buf, ps, SODO_GATE_M as usize) != 0 { free(ps); seq_done!() }
                /* after the wave route: a sloped sine has cuts at its troughs, and the wave route is the measured win there */
                if cuts_try_f64(a, n, buf, &mut c, ps, SODO_GATE_M as usize) != 0 { free(ps); seq_done!() }
                free(ps);
            }
        }
        /* runs of equal values, LAST before the leaf: every order route has declined by now */
        if rle_try_f64(a, n) != 0 { seq_done!() }
        let r = leaf_f64(a, n, buf, &mut c);
        TRACE!("seq: leaf done (result in {})", if r == a { "a" } else { "buf" });
        if r != a { memcpy(a, buf, n * size_of::<f64>()) }          /* only if parity left it in scratch */
        TRACE!("seq: copyback done");
        seq_done!()
    }

    let mut j: job_t = core::mem::zeroed();
    j.P = P; j.nbk = P * BUCKETS_PER_THREAD;
    if j.nbk > MAX_BUCKETS { j.nbk = P * (if MAX_BUCKETS / P > 0 { MAX_BUCKETS / P } else { 1 }) }
    j.nsp = j.nbk - 1; j.src = a; j.n = n;
    j.wc_on = (P >= WC_MIN_THREADS) as i32;
    /* chunk grid: ~4 chunks per thread at minimum, never larger than CHUNK_MAX elements */
    j.csize = (n + P as usize * 4 - 1) / (P as usize * 4); if j.csize > CHUNK_MAX as usize { j.csize = CHUNK_MAX as usize } if j.csize < 1 { j.csize = 1 }
    j.nchunk = (n + j.csize - 1) / j.csize;
    let mut sc: scratch_t = core::mem::zeroed();                     /* j.dst and j.bidx stay null until a route asks (scratch_get) */
    sc.n = n; sc.bidx_n = if j.nbk <= 256 { n } else { 0 };
    /* KINKS go past near-sorted to the detector */
    let kinks_seen = (g_reach_last.with(|r| r.get()) == LOGOS_REACH_DECLINED_KINKS) as i32;
    if kinks_seen != 0 { TRACE!("par: nearsorted skipped, the reach probe saw sparse KINKS -> relocation") }
    if kinks_seen == 0 && nearsorted_parallel_f64(a, n, P, null_mut(), &mut sc, if sodo_ok != 0 { SO.inv_all } else { -1.0 }) != 0 {
        scratch_drop(&mut sc);
        if sodo_ok != 0 { free(SO.sorted) }
        return;
    }
    TRACE!("par: nearsorted declined");
    if n >= RELOC_MIN_N as usize && relocate_try_f64(a, n, P, null_mut(), &mut sc) != 0 {   /* J5: block-permuted data */
        TRACE!("par: relocated, done");
        scratch_drop(&mut sc);
        if sodo_ok != 0 { free(SO.sorted) }
        return;
    }
    TRACE!("par: relocation declined");
    /* the unstructured partition's own thread floor: threads, not buckets */
    if Pb > P && sodo_ok != 0 && SO.has_nan == 0 && vnbk.is_null() && sample_unstructured(a, n, &mut SO) != 0 {
        TRACE!("par: unstructured -> partition threads {} -> {} (buckets stay {})", P, Pb, j.nbk);
        P = Pb; j.P = P; g_boosts.fetch_add(1, SeqCst);
        j.wc_on = (P >= WC_MIN_THREADS) as i32;
        j.csize = (n + P as usize * 4 - 1) / (P as usize * 4); if j.csize > CHUNK_MAX as usize { j.csize = CHUNK_MAX as usize } if j.csize < 1 { j.csize = 1 }
        j.nchunk = (n + j.csize - 1) / j.csize;
    }
    if vnbk.is_null() && sodo_ok != 0 && SO.has_nan == 0 && halves_try_f64(a, n, P, &mut sc, &mut SO) != 0 {   /* two threads: one split, two sequential leaves */
        TRACE!("par: no structure found at P=2 -> two halves, sequential leaves");
        scratch_drop(&mut sc);
        free(SO.sorted);
        return;
    }
    let sp = malloc(j.nsp as usize * size_of::<f64>()) as *mut f64;
    j.counts = calloc(j.nchunk * j.nbk as usize, size_of::<usize>()) as *mut usize;
    j.offs = malloc(j.nchunk * j.nbk as usize * size_of::<usize>()) as *mut usize;
    j.bsize = calloc(j.nbk as usize, size_of::<usize>()) as *mut usize;
    j.bstart = calloc(j.nbk as usize, size_of::<usize>()) as *mut usize;
    let wa = malloc(P as usize * size_of::<warg_t>()) as *mut warg_t;
    let th = malloc(P as usize * size_of::<pthread_t>()) as *mut pthread_t;
    let ctxs = calloc(P as usize, size_of::<ctx_t>()) as *mut ctx_t;

    let mut m = j.nbk as usize * OVERSAMPLE; if m > n { m = n }
    let mut samp: *mut f64 = null_mut();
    if sodo_ok != 0 && SO.mfull == m && sodo_extend(a, n, &mut SO) != 0 && SO.has_nan == 0 { samp = SO.sorted; SO.sorted = null_mut() }   /* the one sample, extended and sorted */
    else if sodo_ok != 0 { free(SO.sorted); SO.sorted = null_mut() }
    if samp.is_null() {
        samp = malloc(m * size_of::<f64>()) as *mut f64;
        for i in 0..m { let q = SAMPLE_POS(i, m, n); *samp.add(i) = *a.add(if q < n { q } else { n - 1 }) }
        sample_sort(samp, m);
    }
    for b in 1..j.nbk as usize { *sp.add(b - 1) = *samp.add(b * m / j.nbk as usize) }
    j.bt_ok = bt_init(&mut j.bt, sp, j.nsp, *samp, *samp.add(m - 1));   /* O(1) bucket lookup for the histogram */
    /* Uniformity certificate: the Kolmogorov-Smirnov distance of the sorted sample from the straight line through its extremes */
    j.ks_ok = 0; j.ks_d = 1.0; j.ks_lo = *samp; j.ks_hi = *samp.add(m - 1); j.ks_scale = 0.0;
    if m >= 256 && j.ks_hi > j.ks_lo && (j.ks_hi - j.ks_lo).is_finite() && (1.0 / (j.ks_hi - j.ks_lo)).is_finite() {
        let mut D = 0.0f64; let inv = 1.0 / (j.ks_hi - j.ks_lo);
        for i in 0..m {
            let F = (*samp.add(i) - j.ks_lo) * inv;
            let mut e1 = F - i as f64 / m as f64; let mut e2 = (i + 1) as f64 / m as f64 - F;
            if e1 < 0.0 { e1 = -e1 } if e2 < 0.0 { e2 = -e2 } if e1 > D { D = e1 } if e2 > D { D = e2 }
        }
        j.ks_d = D; j.ks_ok = (D <= HF_KS_MAX as f64 / (m as f64).sqrt()) as i32; j.ks_scale = j.nbk as f64 * inv;
        TRACE!("par: KS distance {:.4} (limit {:.4}) -> {}", D, HF_KS_MAX as f64 / (m as f64).sqrt(), if j.ks_ok != 0 { "uniform" } else { "not uniform" });
    }
    /* the same test in KEY space */
    j.ks_key_ok = 0; j.kk_lo = total_key(*samp); j.kk_hi = total_key(*samp.add(m - 1)); j.kk_scale = 0.0;
    if j.ks_ok == 0 && m >= 256 && j.kk_hi > j.kk_lo {
        let span = (j.kk_hi - j.kk_lo) as f64; let mut D = 0.0f64;
        for i in 0..m {
            let F = (total_key(*samp.add(i)) - j.kk_lo) as f64 / span;
            let mut e1 = F - i as f64 / m as f64; let mut e2 = (i + 1) as f64 / m as f64 - F;
            if e1 < 0.0 { e1 = -e1 }
            if e2 < 0.0 { e2 = -e2 }
            if e1 > D { D = e1 }
            if e2 > D { D = e2 }
        }
        j.ks_key_ok = (D <= HF_KS_MAX as f64 / (m as f64).sqrt()) as i32; j.kk_scale = j.nbk as f64 / span;
        TRACE!("par: KS distance in key space {:.4} -> {}", D, if j.ks_key_ok != 0 { "log-uniform" } else { "not log-uniform" });
    }
    let mut hvmask: *mut u8 = null_mut();                            /* sample points that are heavy values: the balance check leaves them out */
    /* exponent-histogram map, and the largest share a single value holds in the sample */
    {
        let mut cnt = [0usize; 256]; let (mut run, mut maxrun) = (1usize, 1usize);
        j.ex_e0 = total_key(*samp) >> EX_SHIFT;                      /* entries are binades above the lowest sampled */
        for i in 0..m {
            let q = total_key(*samp.add(i)) >> EX_SHIFT;
            let mut e: u32 = if q <= j.ex_e0 { 0 } else { (q - j.ex_e0) as u32 }; if e > 255 { e = 255 }
            cnt[e as usize] += 1;
            if i != 0 && *samp.add(i) == *samp.add(i - 1) { run += 1; if run > maxrun { maxrun = run } } else { run = 1 }
        }
        j.dup_frac = maxrun as f64 / m as f64;
        j.hv = hv_build(samp, m, j.nbk, &mut hvmask);
        let mut cum = 0usize; let mut prev = 0u32; let mut nonempty = 0i32;
        for e in 0..256usize {
            let mut base = (j.nbk as f64 * cum as f64 / m as f64) as u32;
            if base > (j.nbk - 1) as u32 { base = (j.nbk - 1) as u32 }
            if base < prev { base = prev }
            cum += cnt[e];
            let mut next = (j.nbk as f64 * cum as f64 / m as f64) as u32;
            if next > j.nbk as u32 { next = j.nbk as u32 }
            if next < base { next = base }
            j.ex[e].base = base; j.ex[e].span = next - base;
            j.ex[e].slope = if j.ex[e].span != 0 { j.ex[e].span as f64 / EX_SCALE as f64 } else { 0.0 };
            if cnt[e] != 0 { nonempty += 1 }
            prev = base;
        }
        j.ex_ok = (nonempty >= 2) as i32;
        j.ex_ok = 0;                                                 /* opt-in (LOGOS_EXMAP), measured negative */
        TRACE!("par: exponent map over {} binades, largest single value {:.2}% of the sample", nonempty, j.dup_frac * 100.0);
    }
    /* the key grid: cells of equal width in key space, each bucket edge snapped to the cell of a sample quantile */
    j.kg_ok = 0;
    if m >= 1024 && (*samp).is_finite() && (*samp.add(m - 1)).is_finite() && j.nbk >= 2 && j.nbk <= 256 {
        let (klo, khi) = (total_key(*samp), total_key(*samp.add(m - 1)));
        if khi > klo {
            let span = khi - klo; let mut sh = 0i32;
            while (span >> sh) >= (1u64 << KG_BITS) { sh += 1 }
            let mut cells = (span >> sh) as u32 + 1; if cells > (1u32 << KG_BITS) { cells = 1u32 << KG_BITS }
            j.kg_lo = klo; j.kg_shift = sh; j.kg_cells = cells;
            let mut c = 0u32; let mut b = 0i32;
            for q in 1..j.nbk as usize {                            /* edge q: the cell of the q/nbk sample quantile */
                let iq = q * m / j.nbk as usize;
                let dq = (total_key(*samp.add(iq)) - klo) >> sh; let cq = if dq < cells as u64 { dq as u32 } else { cells - 1 };
                while c < cq { j.kg_tab[c as usize] = b as u8; c += 1 }
                b = q as i32;
            }
            while c < cells { j.kg_tab[c as usize] = b as u8; c += 1 }
            j.kg_ok = 1;
            TRACE!("par: key grid {} cells, shift {}", cells, sh);
        }
    }
    /* per-mode certificate: up to MD_MAX modes, from the splitters */
    j.md_ok = 0; j.md_nm = 0;
    let _ = md_certify(&mut j, sp, j.nsp, samp, m);
    /* A map is only usable if the partition it produces is balanced: apply each certified map to the sample and drop it
       if any bucket would take more than HF_BAL_MAX times its share. */
    {
        let mut load = [0u32; MAX_BUCKETS as usize]; let lim = HF_BAL_MAX as f64 * m as f64 / j.nbk as f64;
        macro_rules! BAL_OK { ($expr:expr, $flag:expr) => {{ if $flag != 0 {
            for b_ in 0..j.nbk as usize { load[b_] = 0 }
            for i in 0..m { if !hvmask.is_null() && *hvmask.add(i) != 0 { continue } let x_ = *samp.add(i); let b_: i32 = ($expr)(x_) as i32; if b_ >= 0 && b_ < j.nbk { load[b_ as usize] += 1 } }
            let mut mx_ = 0u32; for b_ in 0..j.nbk as usize { if load[b_] > mx_ { mx_ = load[b_] } }
            if mx_ as f64 > lim { $flag = 0; TRACE!("par: a map was rejected, one bucket would take {:.1}x its share", mx_ as f64 * j.nbk as f64 / m as f64); } } }}; }
        let jp: *const job_t = &j;
        BAL_OK!(|x: f64| hf_lookup(jp, x, 6, 0), j.ks_ok);          /* the certified grids exist only there */
        BAL_OK!(|x: f64| hf_lookup(jp, x, 7, 0), j.ks_key_ok);
        BAL_OK!(|x: f64| hf_lookup_md(jp, x), j.md_ok);
        BAL_OK!(|x: f64| hf_lookup_ex(jp, x), j.ex_ok);
        BAL_OK!(|x: f64| hf_lookup_kg(jp, x), j.kg_ok);
    }
    free(hvmask);
    free(samp); j.sp = sp;
    /* the AVX2 quicksort as the partition's leaf, where buckets are past L2 and the sample looks unstructured */
    j.seq_leaf = (sodo_ok != 0 && SO.has_nan == 0 && n / j.nbk as usize >= SEQLEAF_MIN_BUCKET as usize && avx2_leaf_on() != 0
                  && sample_unstructured(a, n, &mut SO) != 0) as i32;
    TRACE!("par: leaves {} (mean bucket {})", if j.seq_leaf != 0 { "take the sequential leaf" } else { "keep the parallel leaf" }, n / j.nbk as usize);
    macro_rules! par_free { () => {{
        bt_free(&mut j.bt);
        free(j.mdx); j.mdx = null_mut();
        free(j.hv); j.hv = null_mut();
        free(sp); free(j.counts); free(j.offs); free(j.bsize); free(j.bstart); free(wa); free(th); free(ctxs);
    }}; }
    if hf_partition(&mut j, P, ctxs, th, (!vnbk.is_null()) as i32, vstart, vnbk, vmin, vmax) != 0 {
        TRACE!("par: histfree done");
        for t in 0..P as usize { ctx_free(ctxs.add(t)) }
        scratch_drop(&mut sc);
        par_free!();
        return;
    }
    TRACE!("par: histfree declined, histogram path");
    scratch_get(&mut sc);                                            /* the histogram path scatters into the scratch: taken now, not before */
    j.dst = sc.dst; j.bidx = sc.bidx;

    barrier_init(&mut j.bar, P); j.hnext.store(0, SeqCst); j.snext.store(0, SeqCst); j.next.store(0, SeqCst);
    for t in 0..P as usize { (*wa.add(t)).j = &mut j; (*wa.add(t)).t = t as i32; (*wa.add(t)).ctx = ctxs.add(t) }
    let hist_made = par_spawn(th, P, worker, wa as *mut c_void, size_of::<warg_t>(), &mut j.bar, null_mut());       /* queue-driven throughout */
    do_hist(&mut j);                                                 /* main = worker 0 */
    barrier_wait(&mut j.bar);
    TRACE!("par: histogram done");
    /* combine: bucket sizes, starts, per-chunk write cursors (serial, O(nchunk*nbk)) */
    let nbk = j.nbk as usize;
    for ch in 0..j.nchunk { for b in 0..nbk { *j.bsize.add(b) += *j.counts.add(ch * nbk + b) } }
    for b in 1..nbk { *j.bstart.add(b) = *j.bstart.add(b - 1) + *j.bsize.add(b - 1) }
    for b in 0..nbk { let mut acc = *j.bstart.add(b); for ch in 0..j.nchunk { *j.offs.add(ch * nbk + b) = acc; acc += *j.counts.add(ch * nbk + b) } }
    let mut maxb = 1usize; for b in 0..nbk { if *j.bsize.add(b) > maxb { maxb = *j.bsize.add(b) } }
    /* a typical bucket; a larger one borrows (ctx_fit) */
    for t in 0..P as usize { let _ = ctx_init(ctxs.add(t), ctx_typical(maxb, n, j.nbk), j.wc_on); }
    barrier_wait(&mut j.bar);
    do_scatter(&mut j);
    barrier_wait(&mut j.bar);
    TRACE!("par: scatter done");
    do_buckets(&mut j, ctxs);
    par_join(th, hist_made);
    TRACE!("par: buckets done");
    barrier_destroy(&mut j.bar);
    for t in 0..P as usize { ctx_free(ctxs.add(t)) }
    scratch_drop(&mut sc);
    par_free!();
}

/// C line 10105: the route witnesses. Only the f64 sort's are here; the string and argsort ones read 0.
pub(crate) unsafe fn logos_twist_last(which: i32) -> usize {
    macro_rules! take { ($g:expr) => {{ let v = $g; $g = 0; return v as usize; }}; }
    if which == -5 { return avx2_leaf_on() as usize }
    if which == 15 { return g_fuse_parts.swap(0, SeqCst) as usize }
    if which == 17 { return g_lcvec_elems.swap(0, SeqCst) as usize }
    if which == 16 { return g_fuse_declines.swap(0, SeqCst) as usize }
    if which == 19 { return g_wp_groups.swap(0, SeqCst) as usize }
    if which == 20 { take!(g_thin_calls) }
    if which == 21 { return 0 }                                      /* the pool trim is not ported */
    if which == 22 { return g_f32_sorts.swap(0, SeqCst) as usize }
    if which == 26 { take!(g_halves) }
    if which == 27 { return 0 }
    if which == 29 { return g_cuts_sorts.swap(0, SeqCst) as usize }
    if which == 30 { return g_lowdisc_fires.swap(0, SeqCst) as usize }
    if which == 31 { return g_sf_windows.swap(0, SeqCst) as usize }
    if which == 33 { return g_dom_sorts.swap(0, SeqCst) as usize }
    if which == 32 { return g_runmerge_vecs.swap(0, SeqCst) as usize }
    if which == 34 { return g_half_places.swap(0, SeqCst) as usize }
    if which == 35 { return g_hf_pooled.swap(0, SeqCst) as usize }
    if which == 36 { return g_lat1_sorts.swap(0, SeqCst) as usize }
    if which == 37 { return g_lat_spilled.swap(0, SeqCst) as usize }
    if which == 41 { return g_hf_heavy.swap(0, SeqCst) as usize }
    if which == 42 { return 0 }
    if which == 40 { return g_mc_capped.swap(0, SeqCst) as usize }
    if which == 39 { return g_boosts.swap(0, SeqCst) as usize }
    if which == 38 { return g_ns_strand.swap(0, SeqCst) as usize }
    if which == 28 { return g_rle_sorts.swap(0, SeqCst) as usize }
    if which == -9 || which == -10 { return 0 }
    if which == -11 { take!(g_tail_kind) }
    if which == -12 { take!(g_head_kind) }
    if which == 25 { return 0 }
    if which == 24 { return 0 }                                      /* the in-place partition is not ported */
    if which == 23 { return g_latvec_vecs.swap(0, SeqCst) as usize }
    if which == -7 { return avx2_leaf_on() as usize }
    if which == -8 { return 1 }
    if which == -6 { return 1 }
    if which == -13 { return thin_ceiling() }
    if which == 18 { return g_mdgrid_scatters.swap(0, SeqCst) as usize }
    if which == -2 { return 1 }
    if which == 3 { return 0 }                                       /* the literal linked list is not ported */
    if which == -4 { return (logos_simd_kind() == 2) as usize }
    if which == 13 { return g_simd_merges.swap(0, SeqCst) as usize }
    if which == 5 { return g_knot_gallops.swap(0, SeqCst) as usize }
    if which == 4 { take!(g_kink_count) }
    if which == -3 { return 1 }
    if which == 6 { take!(g_dense_count) }
    if which == 7 { take!(g_loop_count) }
    if which == 8 { take!(g_wave_count) }
    if which == 9 { take!(g_seam_count) }
    if which == 10 { take!(g_stray_count) }
    if which == 12 { take!(g_onerun_count) }
    if which < 0 { g_twist_knot = 0; g_twist_reloc = 0; return 0 }
    if which != 0 { g_twist_reloc } else { g_twist_knot }
}
/// C line 10253.
pub(crate) unsafe fn logos_reach_last() -> i32 { g_reach_last.with(|r| r.get()) }

// ---- the thin front (C lines 10260-10327) ----------------------------------------------------------------------------
pub(crate) const THIN_MAX_N: usize = 2047;
pub(crate) const THIN_KINK_MIN_N: usize = 64;
pub(crate) fn thin_ceiling() -> usize { THIN_MAX_N }
pub(crate) const THIN_LIMIT: usize = THIN_MAX_N;

#[inline(never)]
pub(crate) unsafe fn thin_core_f64(a: *mut f64, n: usize) {         /* no global state */
    let (mut asc, mut desc) = (1i32, 1i32);
    if n >= PRESCAN_FAST_MIN as usize { let ps = prescan_kind(a, n, prescan_cpu()); asc = ps.asc; desc = ps.desc }   /* the dispatcher's vector scan */
    else {
        let mut i = 1usize;
        while i < n && (asc != 0 || desc != 0) { if *a.add(i) < *a.add(i - 1) { asc = 0 } if *a.add(i) > *a.add(i - 1) { desc = 0 } i += 1 }
    }
    if asc != 0 { return }
    if desc != 0 {
        let (mut l, mut r) = (0usize, n - 1);
        while l < r { let t = *a.add(l); *a.add(l) = *a.add(r); *a.add(r) = t; l += 1; r -= 1 }
        return;
    }
    if n >= 4 && n <= 16 && g_no_smallnet == 0 { net_sort_small(a, n as i64); return }
    if n >= THIN_KINK_MIN_N {
        /* KINKS: eight windows of 32 adjacent pairs; when every descent they see is kink-shaped, a budgeted insertion sort
           finishes it. The first descent that is NOT kink-shaped ends the look. */
        let mut d = 0usize; let mut clean = 1i32;
        let mut w = 0usize;
        while w < 8 && clean != 0 {
            let lo = 2 + (n - 36) * w / 8;
            let mut i = lo;
            while i < lo + 32 {
                if *a.add(i) < *a.add(i - 1) {
                    if !(*a.add(i) >= *a.add(i - 2) && *a.add(i - 1) <= *a.add(i + 1)) { clean = 0; break }
                    d += 1;
                }
                i += 1;
            }
            w += 1;
        }
        if clean != 0 && d > 0 && insertion_sort_momentum(a, 0, n as i64 - 1) != 0 { return }
    }
    if n > 16 && avx2_leaf_on() != 0 && avx2_qsort_f64(a, n) != 0 { return }   /* from 17 its vector networks beat insertion sort 4x */
    if n <= INSERTION_SORT_THRESHOLD { insertion_sort(a, 0, n as i64 - 1); return }
    quicksort(a, 0, n as i64 - 1, 2 * log2i(n));
}
#[inline]
pub(crate) unsafe fn thin_sort_f64(a: *mut f64, n: usize) { g_thin_calls += 1; thin_core_f64(a, n) }   /* one call deep */

/// C line 10328. One top-level sort at a time, as in C: the caller (lib.rs) serializes.
pub(crate) unsafe fn logos_sort_f64(a: *mut f64, n: usize, nthreads: i32) {
    if n < 2 { return }
    if n <= THIN_LIMIT { thin_sort_f64(a, n); return }               /* the thin front: no dispatcher frame, no pool, no context */
    /* above 2^32 - 1 elements the C hands over to the in-place radix; lib.rs never sends such an array here */
    if ns_depth.with(|d| d.get()) == 0 { g_ctx_next = 0 }            /* one top-level sort at a time: the bundle cursor restarts */
    psort_f64(a, n, nthreads, null_mut(), null_mut(), null_mut(), null_mut());
}
