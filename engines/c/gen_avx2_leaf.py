"""gen_avx2_leaf.py -- regenerates the AVX2 leaf block of logos_psort.c (the
section between "/* ---- AVX2 vectorized leaf" and its "#pragma GCC pop_options").
The bitonic networks are emitted as straight-line code because gcc did not
fully unroll the loop form (188 of ~670 min/max instructions, 2.6x slower).
Run from engines/c:  python gen_avx2_leaf.py
"""
import os
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# ---- permutation LUTs: for mask m (bit j = lane j goes right), pack lanes NOT in m to the front (L)
#      and lanes in m to the END (R); expressed as 8 dword indices for vpermd on the float view
def row(lanes_front, lanes_back):
    order = lanes_front + lanes_back
    idx = []
    for j in order: idx += [2 * j, 2 * j + 1]
    return "{ " + ", ".join(str(i) for i in idx) + " }"
lutL, lutR = [], []
for m in range(16):
    right = [j for j in range(4) if m >> j & 1]
    left = [j for j in range(4) if not (m >> j & 1)]
    lutL.append(row(left, right))            # left-going lanes packed to the front
    lutR.append(row(left, right))            # right-going lanes packed to the END: same order! left first, then right
LUT = "static const uint32_t av_lut[16][8] __attribute__((aligned(32))) = {\n    " + ",\n    ".join(lutL) + "\n};\n"
# note: one permutation serves both: lanes not-in-m first, then lanes in-m; the L store reads the first 4-k lanes,
# the R store (at r-4) has its valid lanes in the last k positions.

# ---- straight-line bitonic networks over K vectors
def sortvecs(K):
    out = []
    for i in range(K): out.append(f"v{i} = av_sort1(v{i});")
    m = 2
    while m <= K:
        for b in range(0, K, m):
            # reverse second run: vector order then lanes
            for i in range(m // 4):
                x, y = b + m // 2 + i, b + m - 1 - i
                out.append(f"t = v{x}; v{x} = v{y}; v{y} = t;")
            for i in range(m // 2):
                x = b + m // 2 + i
                out.append(f"v{x} = av_rev(v{x});")
            s = m // 2
            while s >= 1:
                for i in range(m):
                    if (i & s) == 0:
                        x, y = b + i, b + i + s
                        out.append(f"t = _mm256_min_pd(v{x}, v{y}); v{y} = _mm256_max_pd(v{x}, v{y}); v{x} = t;")
                s >>= 1
            for i in range(m): out.append(f"v{b + i} = av_merge1(v{b + i});")
        m <<= 1
    return out

def netfn(K):
    n = 4 * K
    body = "\n    ".join(sortvecs(K))
    decl = ", ".join(f"v{i}" for i in range(K))
    loads = " ".join(f"v{i} = _mm256_load_pd(tmp + {4*i});" for i in range(K))
    stores = " ".join(f"_mm256_store_pd(tmp + {4*i}, v{i});" for i in range(K))
    return f'''static void av_sortnet{n}(double *a, size_t n) {{          /* n <= {n}, no NaN; padded with +inf */
    __m256d {decl}, t; double tmp[{n}] __attribute__((aligned(32)));
    memcpy(tmp, a, n * sizeof(double)); for (size_t i = n; i < {n}; i++) tmp[i] = INFINITY;
    {loads}
    {body}
    {stores}
    memcpy(a, tmp, n * sizeof(double));
}}
'''

block = r'''/* ---- AVX2 vectorized leaf: compress-store partition + bitonic networks ------
 * The x86-simd-sort design, in-house and AVX2-only (4 doubles per vector): an
 * in-place quicksort whose partition splits each vector with one compare, one
 * table-driven permute and two plain stores (the permute packs left-going
 * lanes to the front and right-going lanes to the back, so the same vector is
 * stored at the left cursor and at the right cursor minus four; the lanes that
 * land wrong are garbage in already-consumed space and are overwritten later).
 * AV_UNROLL vectors are loaded per end-choice so the data-dependent "which end
 * next" branch is paid once per 32 elements, not per four (two vectors per
 * end below 512 elements). Base case: 32- or 64-element bitonic
 * networks as straight-line code (generated: gen_avx2_leaf.py).
 * It takes only the UNSTRUCTURED leaf: the places sort_fn would run flash /
 * coarse / bits-coarse (and leaf_f64's skewed branch). Counting, int32 radix,
 * quantized radix, run relocation / merge, momentum and the gap split keep
 * their paths, which is the difference from the x86-simd-sort hook
 * (g_simd_f64), which replaces them all. Runtime-gated on cpuid (avx2+fma)
 * and LOGOS_NO_AVX2=1 in the environment; -DLOGOS_NO_AVX2_LEAF compiles it
 * out; -DAVX2_LEAF_MAX_N=N caps the size it takes whole (above it coarse
 * partitions first and hands its buckets down); -DLOGOS_AVX2_INT32 also routes
 * int32-valued buckets here instead of the LSD radix. Declines (returns 0)
 * when the range holds a NaN. A/B: make simdbench. */
#if !defined(LOGOS_NO_AVX2_LEAF) && defined(__GNUC__) && defined(__x86_64__)
#define LOGOS_AVX2_LEAF 1
#ifndef AVX2_LEAF_MAX_N
#define AVX2_LEAF_MAX_N SIZE_MAX
#endif
/* Policy (measured, make simdbench): the AVX2 leaf takes a range only in a SEQUENTIAL sort and
 * only when sort_fn's inversion probe did not call the data nearly sorted. In the parallel leaf
 * (33k-element L2-resident buckets, 14 threads on a hybrid P+E CPU) it lost 5-28% on the random
 * shapes; single-threaded it won 1.2-1.6x. On nearly-sorted data coarse's near-sequential
 * scatter beats a quicksort by 1.23x. -DLOGOS_AVX2_PARALLEL lifts the thread gate. */
#ifdef LOGOS_AVX2_PARALLEL
#define AVX2_TAKE(c, n) (!(c)->nearly && !(c)->lowdisc && avx2_leaf_on() && (size_t)(n) <= AVX2_LEAF_MAX_N)
#else
#define AVX2_TAKE(c, n) ((c)->seq && !(c)->nearly && !(c)->lowdisc && avx2_leaf_on() && (size_t)(n) <= AVX2_LEAF_MAX_N)
#endif
#ifndef AV_UNROLL
#define AV_UNROLL 8                /* measured: 4 and 16 both ~10% slower (probe) */
#endif
static int g_avx2 = -1;
static inline int avx2_leaf_on(void) { int v = g_avx2; if (v < 0) { v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && !getenv("LOGOS_NO_AVX2"); g_avx2 = v; } return v; }
#pragma GCC push_options
#pragma GCC target("avx2,fma,popcnt")
#include <immintrin.h>
''' + LUT + r'''static const int64_t av_kmask[5][4] __attribute__((aligned(32))) = { { 0, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, -1, 0, 0 }, { -1, -1, -1, 0 }, { -1, -1, -1, -1 } };
static const int64_t av_kmask_hi[5][4] __attribute__((aligned(32))) = { { 0, 0, 0, 0 }, { 0, 0, 0, -1 }, { 0, 0, -1, -1 }, { 0, -1, -1, -1 }, { -1, -1, -1, -1 } };
#define AV_CE(v, t, m) ((v) = _mm256_blend_pd(_mm256_min_pd((v), (t)), _mm256_max_pd((v), (t)), (m)))
static inline __attribute__((always_inline)) __m256d av_rev(__m256d v) { return _mm256_permute4x64_pd(v, 0x1B); }
static inline __attribute__((always_inline)) __m256d av_sort1(__m256d v) {      /* 4 lanes -> ascending */
    __m256d t;
    t = _mm256_permute_pd(v, 0x5); AV_CE(v, t, 0xA);
    t = av_rev(v);                 AV_CE(v, t, 0xC);
    t = _mm256_permute_pd(v, 0x5); AV_CE(v, t, 0xA);
    return v;
}
static inline __attribute__((always_inline)) __m256d av_merge1(__m256d v) {     /* 4-lane bitonic -> ascending */
    __m256d t;
    t = _mm256_permute4x64_pd(v, 0x4E); AV_CE(v, t, 0xC);
    t = _mm256_permute_pd(v, 0x5);      AV_CE(v, t, 0xA);
    return v;
}
''' + netfn(8) + netfn(16) + r'''/* one vector, plain stores: valid only while both free windows hold >= 4 slots */
static inline __attribute__((always_inline)) void av_part_vec(double *a, size_t *l, size_t *r, __m256d v, __m256d piv, int gt, __m256d *mn, __m256d *mx) {
    __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
    int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
    __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
    _mm256_storeu_pd(a + *l, p);
    _mm256_storeu_pd(a + *r - 4, p);
    *l += (size_t)(4 - k); *r -= (size_t)k;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}
/* one vector, masked stores: exact, for the reserved vectors at the end */
static inline __attribute__((always_inline)) void av_part_vec_m(double *a, size_t *l, size_t *r, __m256d v, __m256d piv, int gt, __m256d *mn, __m256d *mx) {
    __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
    int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
    __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
    _mm256_maskstore_pd(a + *l, _mm256_load_si256((const __m256i *)av_kmask[4 - k]), p);       /* first 4-k lanes */
    _mm256_maskstore_pd(a + *r - 4, _mm256_load_si256((const __m256i *)av_kmask_hi[k]), p);    /* last k lanes land at r-k..r */
    *l += (size_t)(4 - k); *r -= (size_t)k;
    *mn = _mm256_min_pd(*mn, v); *mx = _mm256_max_pd(*mx, v);
}
#ifndef AV_SMALL_N
#define AV_SMALL_N 512
#endif
/* small ranges (< AV_SMALL_N, L1-resident): stream through the input once, writing every permuted
 * vector to both ends of a stack buffer, then copy back. No end-choice branch, no reserved vectors;
 * the last two vectors use masked stores because their two stores can overlap. */
static size_t av_partition_small(double *a, size_t n, double pivot, int gt, double *smallest, double *biggest) {
    double tmp[AV_SMALL_N] __attribute__((aligned(32)));
    __m256d piv = _mm256_set1_pd(pivot), mn = _mm256_set1_pd(*smallest), mx = _mm256_set1_pd(*biggest);
    size_t l = 0, r = n, i = 0, nv = n >> 2;
    for (; i + 2 < nv; i++) {
        __m256d v = _mm256_loadu_pd(a + 4 * i);
        __m256d cm = gt ? _mm256_cmp_pd(v, piv, _CMP_GT_OQ) : _mm256_cmp_pd(v, piv, _CMP_GE_OQ);
        int m = _mm256_movemask_pd(cm), k = __builtin_popcount((unsigned)m);
        __m256d p = _mm256_castsi256_pd(_mm256_permutevar8x32_epi32(_mm256_castpd_si256(v), _mm256_load_si256((const __m256i *)av_lut[m])));
        _mm256_storeu_pd(tmp + l, p); _mm256_storeu_pd(tmp + r - 4, p);
        l += (size_t)(4 - k); r -= (size_t)k;
        mn = _mm256_min_pd(mn, v); mx = _mm256_max_pd(mx, v);
    }
    for (; i < nv; i++) { __m256d v = _mm256_loadu_pd(a + 4 * i); av_part_vec_m(tmp, &l, &r, v, piv, gt, &mn, &mx); }
    double sm = *smallest, bg = *biggest;
    for (i = nv << 2; i < n; i++) {                            /* scalar tail into the middle gap */
        double x = a[i]; if (x < sm) sm = x; if (x > bg) bg = x;
        int right = gt ? (x > pivot) : (x >= pivot);
        if (right) tmp[--r] = x; else tmp[l++] = x;
    }
    memcpy(a, tmp, n * sizeof(double));
    double t[4]; _mm256_storeu_pd(t, mn); for (int q = 0; q < 4; q++) if (t[q] < sm) sm = t[q];
    _mm256_storeu_pd(t, mx); for (int q = 0; q < 4; q++) if (t[q] > bg) bg = t[q];
    *smallest = sm; *biggest = bg; return l;
}
/* partitions a[lo..hi): returns the first index of the right part; tracks min and max of the range */
static size_t av_partition(double *a, size_t lo, size_t hi, double pivot, int gt, double *smallest, double *biggest) {
    if (hi - lo < AV_SMALL_N) return lo + av_partition_small(a + lo, hi - lo, pivot, gt, smallest, biggest);
    double sm = *smallest, bg = *biggest;
    while ((hi - lo) & 3) {                                   /* scalar until the length is a multiple of 4 */
        double x = a[lo]; if (x < sm) sm = x; if (x > bg) bg = x;
        int right = gt ? (x > pivot) : (x >= pivot);
        if (right) { hi--; a[lo] = a[hi]; a[hi] = x; } else lo++;
    }
    if (hi == lo) { *smallest = sm; *biggest = bg; return lo; }
    __m256d piv = _mm256_set1_pd(pivot), mn = _mm256_set1_pd(sm), mx = _mm256_set1_pd(bg);
    size_t l = lo, r = hi;
    /* U reserved vectors per end (so stores never overrun unread data), U loads per end-choice */
#define AV_PART_UNROLLED(U) do {         __m256d vl[U], vr[U];         for (int i = 0; i < (U); i++) { vl[i] = _mm256_loadu_pd(a + lo + 4 * i); vr[i] = _mm256_loadu_pd(a + hi - 4 * (U) + 4 * i); }         size_t left = lo + 4 * (U), right = hi - 4 * (U);         while (right - left >= 4 * (U)) {             __m256d v[U];             if (r - right < left - l) { right -= 4 * (U); for (int i = 0; i < (U); i++) v[i] = _mm256_loadu_pd(a + right + 4 * i); }             else                      { for (int i = 0; i < (U); i++) v[i] = _mm256_loadu_pd(a + left + 4 * i); left += 4 * (U); }             for (int i = 0; i < (U); i++) av_part_vec(a, &l, &r, v[i], piv, gt, &mn, &mx);         }         while (right > left) {                                /* remainder, one vector at a time (windows stay >= 4) */             __m256d v;             if (r - right < left - l) { right -= 4; v = _mm256_loadu_pd(a + right); }             else                      { v = _mm256_loadu_pd(a + left); left += 4; }             av_part_vec(a, &l, &r, v, piv, gt, &mn, &mx);         }         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vl[i], piv, gt, &mn, &mx);         for (int i = 0; i < (U); i++) av_part_vec_m(a, &l, &r, vr[i], piv, gt, &mn, &mx); } while (0)
    if (hi - lo == 4)      { __m256d v = _mm256_loadu_pd(a + lo); av_part_vec_m(a, &l, &r, v, piv, gt, &mn, &mx); }
    else if (hi - lo < 16) AV_PART_UNROLLED(1);
    else if (hi - lo < 512) AV_PART_UNROLLED(2);
    else AV_PART_UNROLLED(AV_UNROLL);
#undef AV_PART_UNROLLED
    double t[4]; _mm256_storeu_pd(t, mn); for (int i = 0; i < 4; i++) if (t[i] < sm) sm = t[i];
    _mm256_storeu_pd(t, mx); for (int i = 0; i < 4; i++) if (t[i] > bg) bg = t[i];
    *smallest = sm; *biggest = bg; return l;
}
#ifndef AV_PIV_SMALL
#define AV_PIV_SMALL 512
#endif
static double av_pivot(const double *a, size_t n) {
#ifdef AV_PIVOT_NET
    /* OPT-IN, measured neutral (make chaosbench: 0.94-1.06x at 1T, noise): 64 strided samples sorted by
       the bitonic network (~120 ns) as the pivot instead of 15 insertion-sorted samples. */
    if (n >= 4096) {
        double s[64] __attribute__((aligned(32)));
        for (int i = 0; i < 64; i++) s[i] = a[(size_t)((double)(n - 1) * (i + 0.5) / 64)];
        av_sortnet64(s, 64);
        return s[32];
    }
#endif
    if (n < AV_PIV_SMALL) { double x = a[0], y = a[n / 2], z = a[n - 1]; return x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y)); }
    int k = n < 65536 ? 15 : 63; double s[63];
    for (int i = 0; i < k; i++) s[i] = a[(size_t)((double)(n - 1) * (i + 0.5) / k)];
    for (int i = 1; i < k; i++) { double v = s[i]; int j = i; while (j > 0 && s[j - 1] > v) { s[j] = s[j - 1]; j--; } s[j] = v; }
    return s[k / 2];
}
#ifdef AV_STATS
static size_t av_stat_calls, av_stat_elems;
#define AV_STAT(n) (av_stat_calls++, av_stat_elems += (n))
#else
#define AV_STAT(n) ((void)0)
#endif
static void av_qsort_rec(double *a, size_t n, int depth) {
    while (n > 64) {
        if (depth-- == 0) { quicksort(a, 0, (long)n - 1, 2 * log2i(n)); return; }
        AV_STAT(n);
        double sm = INFINITY, bg = -INFINITY, p = av_pivot(a, n);
        size_t m = av_partition(a, 0, n, p, 0, &sm, &bg);
        if (sm == bg) return;                                  /* all equal */
        if (m == 0) {                                          /* pivot is the minimum: split off the run of equals */
            m = av_partition(a, 0, n, p, 1, &sm, &bg);
            a += m; n -= m; continue;
        }
        if (p == bg) { n = m; continue; }                      /* right part is all == pivot */
        if (m < n - m) { av_qsort_rec(a, m, depth); a += m; n -= m; }
        else           { av_qsort_rec(a + m, n - m, depth); n = m; }
    }
    if (n > 32) av_sortnet64(a, n); else if (n > 1) av_sortnet32(a, n);   /* a 128 network measured 3.0 vs 1.9 ns/elem: one more level + 64 is cheaper */
}
static int av_has_nan(const double *a, size_t n) {
    __m256d acc = _mm256_setzero_pd(); size_t i = 0;
    for (; i + 4 <= n; i += 4) { __m256d v = _mm256_loadu_pd(a + i); acc = _mm256_or_pd(acc, _mm256_cmp_pd(v, v, _CMP_UNORD_Q)); }
    int any = _mm256_movemask_pd(acc);
    for (; i < n; i++) any |= (a[i] != a[i]);
    return any != 0;
}
/* 1 = sorted in place; 0 = declined (NaN present), caller keeps its scalar path */
static int avx2_qsort_f64(double *a, size_t n) {
    if (n < 2) return 1;
    if (av_has_nan(a, n)) return 0;
    av_qsort_rec(a, n, 2 * log2i(n) + 8);
    return 1;
}
#pragma GCC pop_options
#endif

'''
s = open("logos_psort.c", encoding="utf-8").read()
i0 = s.index("/* ---- AVX2 vectorized leaf")
i1 = s.index("#pragma GCC pop_options\n#endif\n\n", i0) + len("#pragma GCC pop_options\n#endif\n\n")
s = s[:i0] + block + s[i1:]
open("logos_psort.c", "w", encoding="utf-8").write(s)
print("avx2 block regenerated:", len(block), "bytes")
