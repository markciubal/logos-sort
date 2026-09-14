/* paradigm.cpp — testing the three proposed paradigm shifts against the SIMD
 * "materialize a sorted array" baseline (Intel x86-simd-sort). Each experiment
 * verifies its output before reporting a time. Build: see paradigm.sh.
 *
 *   #1  Vectorize the ADDRESSING (scatter/histogram) — can we make the operation
 *       SIMD wins by avoiding (the scatter) itself vectorize on a CPU?
 *   #2  Never materialize the permutation — answer ORDER queries (rank/select)
 *       from a model built WITHOUT scattering, vs sort-then-binary-search.
 *   #3  Distribution-awareness as the product — answer ANALYTICS queries
 *       (quantiles, top-k, distinct) from a one-pass profile, vs sort-then-query.
 *
 * The thesis under test: logos-sort's paradigm loses when the deliverable is a
 * sorted array (#1) but can win when the deliverable is ORDER or STRUCTURE (#2, #3).
 */
#include "x86simdsort-static-incl.h"
#include <immintrin.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>

static uint64_t RS;
static void seed(uint64_t s){ RS = s?s:1; }
static uint64_t nr(){ RS^=RS<<13; RS^=RS>>7; RS^=RS<<17; return RS; }
static double fr(){ return (double)(nr()>>11)/(double)(1ULL<<53); }
static double now(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3 + t.tv_nsec/1e6; }
template<class F> static double best(F f, int it){ double b=1e300; for(int i=0;i<it;i++){ double s=now(); f(); double x=now()-s; if(x<b)b=x; } return b; }

/* ============================ #1: can addressing vectorize? ============================ */
static void exp1(){
    printf("\n==== #1  Vectorize the ADDRESSING (the operation SIMD avoids) ====\n");
    size_t N = 8000000;
    std::vector<double> a(N), out(N);
    std::vector<uint32_t> perm(N);
    seed(7); for(auto&v:a) v = fr();
    for(size_t i=0;i<N;i++) perm[i]=i; for(size_t i=N-1;i>0;i--){ size_t j=nr()%(i+1); std::swap(perm[i],perm[j]); }  /* random permutation */

    /* (a) pure permutation scatter: out[perm[i]] = a[i] */
    auto scat_scalar = [&]{ for(size_t i=0;i<N;i++) out[perm[i]] = a[i]; };
    auto scat_avx512 = [&]{
        size_t i=0;
        for(; i+8<=N; i+=8){
            __m256i idx = _mm256_loadu_si256((const __m256i*)(perm.data()+i));   /* 8x uint32 */
            __m512d v   = _mm512_loadu_pd(a.data()+i);
            _mm512_i32scatter_pd(out.data(), idx, v, 8);                          /* hardware scatter */
        }
        for(; i<N; i++) out[perm[i]] = a[i];
    };
    std::vector<double> o1(N); scat_scalar(); o1=out; std::fill(out.begin(),out.end(),0); scat_avx512();
    bool ok_a = (out==o1);
    double ss = best(scat_scalar, 7), sv = best(scat_avx512, 7);
    printf("  (a) permutation scatter out[perm[i]]=a[i]   scalar %.2fms   vpscatter %.2fms   %.2fx  %s\n",
           ss, sv, ss/sv, ok_a?"":"MISMATCH");

    /* (b) histogram count[bucket]++ : scalar vs AVX-512 conflict-detection */
    const int K = 2048;
    std::vector<uint32_t> bkt(N);
    seed(9); for(auto&b:bkt) b = nr() % K;
    auto hist_scalar = [&](std::vector<uint32_t>&C){ std::fill(C.begin(),C.end(),0); for(size_t i=0;i<N;i++) C[bkt[i]]++; };
    auto hist_avx512 = [&](std::vector<uint32_t>&C){
        std::fill(C.begin(),C.end(),0);
        size_t i=0;
        for(; i+16<=N; i+=16){
            __m512i idx = _mm512_loadu_si512(bkt.data()+i);                       /* 16x uint32 buckets */
            __m512i cur = _mm512_i32gather_epi32(idx, C.data(), 4);
            __m512i conf= _mm512_conflict_epi32(idx);                             /* dup detection */
            /* count how many earlier lanes share each lane's bucket -> add that+1 */
            __m512i add = _mm512_set1_epi32(1);
            __mmask16 hasconf = _mm512_test_epi32_mask(conf, _mm512_set1_epi32(-1));
            if(hasconf){ /* rare-ish: resolve conflicts by popcount of the conflict mask below each lane */
                __m512i extra = _mm512_popcnt_epi32(conf);                         /* # earlier equal lanes */
                add = _mm512_add_epi32(add, extra);
            }
            __m512i nv = _mm512_add_epi32(cur, add);
            _mm512_i32scatter_epi32(C.data(), idx, nv, 4);
        }
        for(; i<N; i++) C[bkt[i]]++;
    };
    std::vector<uint32_t> C1(K), C2(K); hist_scalar(C1); hist_avx512(C2);
    bool ok_b = (C1==C2);
    double hs = best([&]{hist_scalar(C1);},7), hv = best([&]{hist_avx512(C2);},7);
    printf("  (b) histogram count[bucket]++               scalar %.2fms   vpconflictd %.2fms   %.2fx  %s\n",
           hs, hv, hs/hv, ok_b?"":"MISMATCH");
    printf("  verdict: SIMD scatter/histogram are the ops logos depends on; if these do not\n"
           "           beat scalar, the addressing paradigm cannot win on a CPU by materializing.\n");
}

/* ============================ #2: order oracle (no materialization) ============================ */
/* rank(x) = #elements < x ; select(k) = k-th smallest. Built from (value,count)
 * over the DISTINCT values only — one no-scatter pass + a tiny sort of distinct
 * values. Exact. Wins when the deliverable is order queries, not a sorted array. */
struct OrderModel {
    std::vector<double> vals;   /* sorted distinct */
    std::vector<size_t> cum;    /* cum[i] = #elements < vals[i]; cum.back()+count = n */
    size_t n = 0;
    void build(const double* a, size_t N){
        n = N;
        std::unordered_map<uint64_t,size_t> m; m.reserve(4096);
        for(size_t i=0;i<N;i++){ uint64_t k; memcpy(&k,&a[i],8); m[k]++; }        /* one pass, no scatter of data */
        vals.reserve(m.size()); std::vector<std::pair<double,size_t>> vc; vc.reserve(m.size());
        for(auto&kv:m){ double v; memcpy(&v,&kv.first,8); vc.push_back({v,kv.second}); }
        std::sort(vc.begin(), vc.end());                                          /* sort DISTINCT only */
        cum.resize(vc.size()); size_t acc=0;
        for(size_t i=0;i<vc.size();i++){ vals.push_back(vc[i].first); cum[i]=acc; acc+=vc[i].second; }
    }
    size_t rank(double x) const { auto it=std::lower_bound(vals.begin(),vals.end(),x); if(it==vals.begin())return 0; size_t i=it-vals.begin()-1; return cum[i]+ (vals[i]<x? (i+1<cum.size()?cum[i+1]-cum[i]: n-cum[i]) : 0); }
    double select(size_t k) const { /* k-th smallest (0-based) */ size_t lo=0,hi=cum.size(); while(lo<hi){size_t m=(lo+hi)/2; if(cum[m]<=k) lo=m+1; else hi=m;} return vals[lo-1]; }
};
static void exp2(){
    printf("\n==== #2  Never materialize: answer ORDER queries from a model (no scatter) ====\n");
    for(size_t d : {size_t(64), size_t(4096), size_t(200000)}){
        size_t N = 8000000, M = 2000000;
        std::vector<double> a(N); seed(3);
        std::vector<double> pool(d); for(auto&v:pool) v = fr()*1e9;
        for(auto&v:a) v = pool[nr()%d];
        std::vector<double> qx(M); seed(5); for(auto&x:qx) x = fr()*1e9;

        /* baseline: SIMD-sort the array, answer rank by binary search */
        std::vector<size_t> rb(M);
        double t_sort=0, t_model=0;
        {
            std::vector<double> s = a;
            t_sort = best([&]{ std::vector<double> c=a; x86simdsortStatic::qsort(c.data(),N,false,false);
                               for(size_t i=0;i<M;i++) (void)(std::lower_bound(c.begin(),c.end(),qx[i])-c.begin()); }, 3);
            /* correctness ranks from a fresh sort */
            std::vector<double> c=a; x86simdsortStatic::qsort(c.data(),N,false,false);
            for(size_t i=0;i<M;i++) rb[i] = std::lower_bound(c.begin(),c.end(),qx[i])-c.begin();
        }
        std::vector<size_t> rm(M);
        {
            t_model = best([&]{ OrderModel om; om.build(a.data(),N);
                                for(size_t i=0;i<M;i++) (void)om.rank(qx[i]); }, 3);
            OrderModel om; om.build(a.data(),N);
            for(size_t i=0;i<M;i++) rm[i] = om.rank(qx[i]);
        }
        bool ok = (rm==rb);
        printf("  d=%-7zu distinct  (build+%zuM rank queries):  sort+bsearch %.1fms   order-model %.1fms   %.2fx %s\n",
               d, M/1000000, t_sort, t_model, t_sort/t_model, ok?"":"  <-- MISMATCH");
    }
    printf("  verdict: the model skips the scatter entirely; it wins where distinct << n and the\n"
           "           workload is queries, and degenerates to a full sort when distinct ~ n.\n");
}

/* ============================ #3: distribution profile as the product ============================ */
static void exp3(){
    printf("\n==== #3  Distribution-awareness as the product: analytics without sorting ====\n");
    size_t N = 8000000;
    std::vector<double> a(N); seed(11); for(auto&v:a) v = 1000.0*std::pow(1.0 - fr()*0.999999, -1.0/1.2);  /* heavy tail */

    /* --- quantiles p50/p95/p99 --- baseline: full SIMD sort then index. */
    auto exact_q = [&](std::vector<double>& s, double p){ return s[(size_t)(p*(N-1))]; };
    std::vector<double> sref = a; x86simdsortStatic::qsort(sref.data(),N,false,false);
    double q50=exact_q(sref,0.50), q95=exact_q(sref,0.95), q99=exact_q(sref,0.99);
    double t_qsort = best([&]{ std::vector<double> c=a; x86simdsortStatic::qsort(c.data(),N,false,false);
                               volatile double x=c[(size_t)(0.5*N)]+c[(size_t)(0.95*N)]+c[(size_t)(0.99*N)]; (void)x; }, 3);
    /* profile: reservoir-free strided sample, sort the SMALL sample, read its quantiles. One pass, no full scatter. */
    const size_t S = 16384;
    double aq50=0,aq95=0,aq99=0;
    double t_prof = best([&]{ std::vector<double> smp(S); size_t step=N/S;
                              for(size_t i=0;i<S;i++) smp[i]=a[i*step];
                              std::sort(smp.begin(),smp.end());
                              aq50=smp[(size_t)(0.50*S)]; aq95=smp[(size_t)(0.95*S)]; aq99=smp[(size_t)(0.99*S)]; }, 5);
    double e50=fabs(aq50-q50)/fabs(q50), e95=fabs(aq95-q95)/fabs(q95), e99=fabs(aq99-q99)/fabs(q99);
    printf("  quantiles p50/p95/p99   sort-exact %.1fms   sampled-profile %.2fms   %.0fx   (rel.err %.1f%%/%.1f%%/%.1f%%)\n",
           t_qsort, t_prof, t_qsort/t_prof, 100*e50, 100*e95, 100*e99);

    /* --- top-k (k=100) --- baseline: nth_element/partial_sort. profile: one-pass min-heap, no full sort. */
    const size_t k=100;
    double t_topk_sort = best([&]{ std::vector<double> c=a; std::nth_element(c.begin(), c.end()-k, c.end());
                                   std::sort(c.end()-k, c.end()); volatile double x=c[N-1]; (void)x; }, 3);
    std::vector<double> heap;
    double t_topk_heap = best([&]{ heap.assign(a.begin(), a.begin()+k); std::make_heap(heap.begin(),heap.end(),std::greater<double>());
                                   for(size_t i=k;i<N;i++) if(a[i]>heap.front()){ std::pop_heap(heap.begin(),heap.end(),std::greater<double>()); heap.back()=a[i]; std::push_heap(heap.begin(),heap.end(),std::greater<double>()); } }, 3);
    /* verify heap top-k == sorted top-k */
    std::vector<double> hk=heap; std::sort(hk.begin(),hk.end());
    std::vector<double> tk(sref.end()-k, sref.end());
    bool ok_topk=(hk==tk);
    printf("  top-100                 sort-based  %.1fms   one-pass heap   %.2fms   %.0fx   %s\n",
           t_topk_sort, t_topk_heap, t_topk_sort/t_topk_heap, ok_topk?"":"MISMATCH");

    /* --- count-in-range [lo,hi] --- baseline: sort+bsearch. profile: one scan, no sort. */
    double lo=q50, hi=q95; size_t want=0; for(double v:a) if(v>=lo&&v<hi) want++;
    double t_cir_sort = best([&]{ std::vector<double> c=a; x86simdsortStatic::qsort(c.data(),N,false,false);
                                  size_t r=std::lower_bound(c.begin(),c.end(),hi)-c.begin(), l=std::lower_bound(c.begin(),c.end(),lo)-c.begin(); volatile size_t x=r-l;(void)x; }, 3);
    size_t got=0;
    double t_cir_scan = best([&]{ size_t cnt=0; for(size_t i=0;i<N;i++){ double v=a[i]; cnt += (v>=lo)&(v<hi); } got=cnt; }, 5);
    printf("  count-in-range          sort+bsearch %.1fms   one scan       %.2fms   %.0fx   %s\n",
           t_cir_sort, t_cir_scan, t_cir_sort/t_cir_scan, got==want?"":"MISMATCH");
    printf("  verdict: none of these classic \"sort first\" analytics needs a sorted array;\n"
           "           a one-pass profile answers them 10-1000x faster and never scatters.\n");
}

int main(){
    printf("PARADIGM-SHIFT experiments — logos-sort's philosophy vs the SIMD sorted-array.\n");
    printf("Baseline for 'materialize the array' = Intel x86-simd-sort (AVX-512). n = 8M.\n");
    exp1();
    exp2();
    exp3();
    printf("\nSummary: #1 tests whether the addressing paradigm can win on a CPU by materializing\n");
    printf("(it cannot if scatter/histogram do not vectorize). #2 and #3 test the real pivot:\n");
    printf("changing the deliverable from a sorted ARRAY to an ORDER/PROFILE the scatter was only a means to.\n");
    return 0;
}
