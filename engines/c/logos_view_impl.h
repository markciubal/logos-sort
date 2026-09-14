/* logos_view_impl.h — the body of the sorted view, included once per element type by logos_view.c.
 *
 * The f64 and int32 views are the same bookkeeping over different engine entry points, so the code
 * lives here once and logos_view.c includes it twice (the pattern DEFINE_INPLACE_MSD uses in the
 * engine, spelled as an include so the code reads as plain C). Before including, define:
 *
 *   VT          element type                      double            int32_t
 *   VIEW        the opaque struct                  logos_view        logos_view_i32
 *   VFN(name)   public query names                 logos_view_##name logos_view_##name##_i32
 *   VCREATE     the constructor                    logos_view_create_f64 / _i32
 *   VS(name)    file-local helper names, unique per type
 *   VPART / VSORTB / VSORT   the engine's partition, bucket sorter and eager sort for VT
 *   VLO / VHI   what an empty bucket's max / min read (-inf / +inf, INT32_MIN / INT32_MAX)
 *   VNAN(x)     1 when x sorts last as NaN does; 0 for ints
 *
 * See logos_view.c for the design notes and logos_view.h for the API. */

struct VIEW {
    VT *a; size_t n;
    int nbk, threads;
    size_t *start;                   /* nbk+1 boundaries; start[0]==0, start[nbk]==n */
    VT *bmin, *bmax;                 /* per-bucket extremes, monotone after fixup (see create) */
    unsigned char *done;             /* per bucket: has it been sorted */
    unsigned char *need, *mask;      /* scratch for batch queries: the touched set, and its complement as a done-mask */
    size_t materialised;
};

VIEW *VCREATE(VT *a, size_t n, int threads) {
    if (!a || n < 2) return NULL;
    VIEW *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->a = a; v->n = n;
    v->threads = threads > 0 ? threads : logos_num_cpus();
    if (v->threads < 1) v->threads = 1;
    size_t cap = (size_t)LOGOS_MAX_VIEW_BUCKETS + 2;
    v->start = calloc(cap, sizeof(size_t));
    v->bmin = malloc(cap * sizeof(VT));
    v->bmax = malloc(cap * sizeof(VT));
    v->done = calloc(cap, 1); v->need = calloc(cap, 1); v->mask = calloc(cap, 1);
    if (!v->start || !v->bmin || !v->bmax || !v->done || !v->need || !v->mask) { VFN(free)(v); return NULL; }

    int nbk = n >= VIEW_MIN_N ? VPART(a, n, threads, v->start, v->bmin, v->bmax) : 0;
    if (nbk <= 0) {
        /* Either too small to bother, or a route in the dispatcher sorted the array outright. Either
         * way there is nothing to defer: sort if needed and model it as one finished bucket. */
        if (n < VIEW_MIN_N) VSORT(a, n, threads);
        v->nbk = 1; v->start[0] = 0; v->start[1] = n;
        v->bmin[0] = a[0]; v->bmax[0] = a[n - 1];
        v->done[0] = 1; v->materialised = 1;
        return v;
    }
    v->nbk = nbk;
    /* Empty buckets read (+inf, -inf), which breaks the monotone order a binary search needs. Give
     * an empty bucket its neighbours' values so bmax is non-decreasing and bmin is non-decreasing;
     * it holds no elements, so it can never be the answer to anything, only stepped over. */
    for (int b = 0; b < nbk; b++) if (v->start[b + 1] == v->start[b]) v->bmax[b] = b ? v->bmax[b - 1] : VLO;
    for (int b = nbk - 1; b >= 0; b--) if (v->start[b + 1] == v->start[b]) v->bmin[b] = b + 1 < nbk ? v->bmin[b + 1] : VHI;
    return v;
}

/* largest b with start[b] <= k */
static int VS(find_rank)(const VIEW *v, size_t k) {
    int lo = 0, hi = v->nbk - 1;
    while (lo < hi) { int mid = (lo + hi + 1) >> 1; if (v->start[mid] <= k) lo = mid; else hi = mid - 1; }
    return lo;
}

static void VS(force)(VIEW *v, int b) {
    if (v->done[b]) return;
    size_t s = v->start[b], c = v->start[b + 1] - s;
    if (c > 1) VSORTB(v->a + s, (const size_t[]){ 0, c }, 1, v->done + b, 1);
    v->done[b] = 1; v->materialised++;
}

/* Force a SET of buckets, need[b] != 0. One bucket (or one thread) sorts on the calling thread as a
 * single query does; several sort IN PARALLEL through the engine's bucket sorter, with every bucket
 * outside the set masked as already done. This is what a batch query has over a loop of single ones:
 * force() is already idempotent per bucket, so the batch cannot touch fewer buckets, it can only sort
 * the ones it touches side by side. -DVIEW_PAR_FORCE=0 keeps them serial (the A/B in viewbench). */
static void VS(force_set)(VIEW *v, unsigned char *need) {
    int cnt = 0;
    for (int b = 0; b < v->nbk; b++) { if (need[b] && !v->done[b]) cnt++; else need[b] = 0; }
    if (cnt == 0) return;
    if (cnt == 1 || v->threads == 1 || !VIEW_PAR_FORCE) { for (int b = 0; b < v->nbk; b++) if (need[b]) VS(force)(v, b); return; }
    for (int b = 0; b < v->nbk; b++) v->mask[b] = !need[b];
    VSORTB(v->a, v->start, v->nbk, v->mask, cnt < v->threads ? cnt : v->threads);
    for (int b = 0; b < v->nbk; b++) if (need[b]) { v->done[b] = 1; v->materialised++; }
}

VT VFN(at)(VIEW *v, size_t k) {
    if (!v || k >= v->n) return 0;
    VS(force)(v, VS(find_rank)(v, k));
    return v->a[k];
}

void VFN(range)(VIEW *v, size_t lo, size_t hi) {
    if (!v || lo >= hi || lo >= v->n) return;
    if (hi > v->n) hi = v->n;
    int b0 = VS(find_rank)(v, lo), b1 = VS(find_rank)(v, hi - 1);
    memset(v->need, 0, (size_t)v->nbk);
    for (int b = b0; b <= b1; b++) v->need[b] = 1;
    VS(force_set)(v, v->need);
}

const VT *VFN(topk)(VIEW *v, size_t k) {
    if (!v || k == 0) return NULL;
    if (k > v->n) k = v->n;
    VFN(range)(v, v->n - k, v->n);
    return v->a + v->n - k;
}

/* m ranks in one pass. The cuts are sorted so the bucket walk is monotone, one pass over the
 * boundaries with no search per cut; each touched bucket is marked once whatever number of cuts
 * share it, and the touched set is forced together. Answers come back in the caller's order. */
typedef struct { size_t k, i; } VS(cut_t);
static int VS(cmp_cut)(const void *x, const void *y) { size_t a = ((const VS(cut_t) *)x)->k, b = ((const VS(cut_t) *)y)->k; return (a > b) - (a < b); }
void VFN(ranks)(VIEW *v, const size_t *ks, size_t m, VT *out) {
    if (!v || !ks || !out || m == 0) return;
    VS(cut_t) *c = malloc(m * sizeof *c);
    if (!c) { for (size_t i = 0; i < m; i++) out[i] = VFN(at)(v, ks[i] < v->n ? ks[i] : v->n - 1); return; }
    for (size_t i = 0; i < m; i++) { c[i].k = ks[i] < v->n ? ks[i] : v->n - 1; c[i].i = i; }
    qsort(c, m, sizeof *c, VS(cmp_cut));
    memset(v->need, 0, (size_t)v->nbk);
    int b = 0;
    for (size_t t = 0; t < m; t++) { while (b + 1 < v->nbk && v->start[b + 1] <= c[t].k) b++; v->need[b] = 1; }
    VS(force_set)(v, v->need);
    for (size_t t = 0; t < m; t++) out[c[t].i] = v->a[c[t].k];
    free(c);
}

/* Quantile q in [0,1] is the element of rank round(q * (n - 1)): q=0 the minimum, q=1 the maximum,
 * q=0.5 the median (the upper one for even n). Out-of-range and NaN cuts clamp to [0,1]. */
void VFN(quantiles)(VIEW *v, const double *q, size_t m, VT *out) {
    if (!v || !q || !out || m == 0) return;
    size_t *ks = malloc(m * sizeof *ks);
    if (!ks) return;
    for (size_t i = 0; i < m; i++) { double x = q[i]; if (!(x > 0.0)) x = 0.0; if (x > 1.0) x = 1.0; ks[i] = (size_t)(x * (double)(v->n - 1) + 0.5); }
    VFN(ranks)(v, ks, m, out);
    free(ks);
}

/* Elements strictly less than x. Every bucket whose max is below x lies wholly below x; the first
 * bucket whose max reaches x is the only one that can straddle it, and only if its min is below x. */
size_t VFN(rank)(VIEW *v, VT x) {
    if (!v) return 0;
    if (VNAN(x)) return v->n;                           /* NaN sorts last in this engine */
    int lo = 0, hi = v->nbk;                            /* first b with bmax[b] >= x, or nbk */
    while (lo < hi) { int mid = (lo + hi) >> 1; if (v->bmax[mid] >= x) hi = mid; else lo = mid + 1; }
    if (lo == v->nbk) return v->n;
    if (!(v->bmin[lo] < x)) return v->start[lo];        /* nothing in bucket lo is below x */
    VS(force)(v, lo);
    const VT *p = v->a + v->start[lo]; size_t c = v->start[lo + 1] - v->start[lo], l = 0, h = c;
    while (l < h) { size_t m = (l + h) >> 1; if (p[m] < x) l = m + 1; else h = m; }
    return v->start[lo] + l;
}

size_t VFN(count_in_range)(VIEW *v, VT lo, VT hi) {
    if (!v || !(lo < hi)) return 0;
    return VFN(rank)(v, hi) - VFN(rank)(v, lo);
}

void VFN(all)(VIEW *v) {
    if (!v) return;
    VSORTB(v->a, v->start, v->nbk, v->done, v->threads);
    v->materialised = (size_t)v->nbk;
}

size_t VFN(nbuckets)(const VIEW *v) { return v ? (size_t)v->nbk : 0; }
size_t VFN(materialised)(const VIEW *v) { return v ? v->materialised : 0; }
size_t VFN(n)(const VIEW *v) { return v ? v->n : 0; }

void VFN(free)(VIEW *v) {
    if (!v) return;
    free(v->start); free(v->bmin); free(v->bmax); free(v->done); free(v->need); free(v->mask); free(v);
}

#undef VT
#undef VIEW
#undef VFN
#undef VCREATE
#undef VS
#undef VPART
#undef VSORTB
#undef VSORT
#undef VLO
#undef VHI
#undef VNAN
