/* logos_simd.cpp — optional SIMD leaf backed by Intel x86-simd-sort (AVX-512).
 * Compile with -march=native -DHAVE_XSS -I<x86-simd-sort/src> and link into the
 * program; call logos_enable_simd() once to register it with the engine.
 * Without this TU the core is unchanged and dependency-free. */
#include "logos_psort.h"
#if defined(HAVE_XSS)
#include "x86simdsort-static-incl.h"
extern "C" void logos_simd_f64_leaf(double *a, size_t n) { x86simdsortStatic::qsort(a, n, /*hasnan*/false, /*desc*/false); }
extern "C" void logos_simd_i32_leaf(int32_t *a, size_t n) { x86simdsortStatic::qsort(a, n); }
extern "C" void logos_enable_simd(void) { logos_set_simd_sort(logos_simd_f64_leaf, logos_simd_i32_leaf); }
#else
extern "C" void logos_enable_simd(void) { /* built without SIMD: no-op */ }
#endif

/* ---- vectorized lane-parallel kernels for logos's own hot loops (tasks #1/#2).
 * These do NOT vectorize the scatter (data-dependent, bandwidth-bound); they
 * vectorize the arithmetic that feeds it: the value->bucket index (#2) and the
 * OR/AND byte-skip key reduction (#1). Registered via logos_set_simd_kernels;
 * the core falls back to scalar when unset. Compiled with -march=native. */
#if defined(HAVE_XSS) || defined(LOGOS_SIMD_KERNELS)
/* Pin the exact AVX-512 features these kernels use, independent of -march.
 * Under WSL2/virtualization -march=native can mask AVX-512 even on a capable CPU,
 * which breaks always_inline of the intrinsics; this attribute makes it explicit. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC target("avx512f,avx512dq,avx512bw,avx512vl,avx512cd")
#endif
#include <immintrin.h>
#include <cstring>
/* fills idx[lo..hi] with the value-interpolation bucket, and C[0..K) with counts.
 * Vectorized index (8 doubles/instr) fused with a scalar count (scatter-add can't
 * vectorize); one pass, no extra memory traffic. */
extern "C" void logos_simd_bucket_count(const double *a, long lo, long hi, double mn, double scale, int K, uint16_t *idx, size_t *C) {
    for (int b = 0; b < K; b++) C[b] = 0;
    __m512d vmn = _mm512_set1_pd(mn), vsc = _mm512_set1_pd(scale);
    __m512i vlo = _mm512_setzero_si512(), vhi = _mm512_set1_epi64(K - 1);
    long i = lo;
    for (; i + 8 <= hi + 1; i += 8) {
        __m512d v = _mm512_loadu_pd(a + i);
        __m512i b = _mm512_cvttpd_epi64(_mm512_mul_pd(_mm512_sub_pd(v, vmn), vsc));
        b = _mm512_min_epi64(_mm512_max_epi64(b, vlo), vhi);
        __m128i w = _mm512_cvtepi64_epi16(b);
        _mm_storeu_si128((__m128i *)(idx + i), w);
        for (int j = 0; j < 8; j++) C[idx[i + j]]++;
    }
    for (; i <= hi; i++) { long b = (long)((a[i] - mn) * scale); if (b < 0) b = 0; else if (b >= K) b = K - 1; idx[i] = (uint16_t)b; C[b]++; }
}
/* OR/AND over the IEEE total-order key. */
extern "C" void logos_simd_orand(const double *a, size_t n, uint64_t *por, uint64_t *pand) {
    const uint64_t *u = (const uint64_t *)a;
    __m512i sign = _mm512_set1_epi64((long long)0x8000000000000000ULL), ones = _mm512_set1_epi64(-1);
    __m512i vo = _mm512_setzero_si512(), va = ones; size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m512i x = _mm512_loadu_si512(u + i);
        __mmask8 neg = _mm512_movepi64_mask(x);
        __m512i k = _mm512_mask_blend_epi64(neg, _mm512_or_si512(x, sign), _mm512_xor_si512(x, ones));
        vo = _mm512_or_si512(vo, k); va = _mm512_and_si512(va, k);
    }
    uint64_t ob[8], ab[8]; _mm512_storeu_si512(ob, vo); _mm512_storeu_si512(ab, va);
    uint64_t o = 0, an = ~0ULL; for (int j = 0; j < 8; j++) { o |= ob[j]; an &= ab[j]; }
    for (; i < n; i++) { uint64_t b; memcpy(&b, &a[i], 8); uint64_t kk = (b >> 63) ? ~b : (b | 0x8000000000000000ULL); o |= kk; an &= kk; }
    *por = o; *pand = an;
}
extern "C" void logos_enable_simd_kernels(void) { logos_set_simd_kernels(NULL, logos_simd_orand);   /* #2 bucket-count measured net-negative; only #1 OR/AND registered */ }
#else
extern "C" void logos_enable_simd_kernels(void) {}
#endif
