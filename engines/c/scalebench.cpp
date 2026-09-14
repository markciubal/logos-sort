/* scalebench.cpp — time against n for every contender, every shape, at 1 thread and at P.
 *
 * Sizes 1e4, 3e4, ... up to LOGOS_MAX_N (default 3e7), nine shapes. Single-threaded section:
 * std::sort, logos, x86-simd-sort (-DHAVE_XSS, OpenMP threads set to 1), IPS4o sequential
 * (-DHAVE_IPS4O). Parallel section at P threads: __gnu_parallel::sort, logos, x86-simd-sort
 * OpenMP, IPS4o parallel. Never one thread against many. std::sort's output is the reference;
 * every other output is checked equal to it. Reps shrink with n (5 / 3 / 1); the minimum is
 * reported. Writes scale-report.html: per shape, two line charts (n on x, ms on y, one line per
 * sort; linear axes by default with a log-log toggle), dashed O(n) and O(n log n) reference curves
 * anchored at logos's largest point, each series' empirical exponent (least-squares slope of log t
 * vs log n over n >= 1e5), a table view, and the calibration line. Build: make scalebench (no
 * deps) or make scale-full (both opponents). Overrides: LOGOS_MAX_N, LOGOS_THREADS, LOGOS_MAX_SEC. */
#include "logos_psort.h"
#include "uplot_embed.h"
#include "bench_common.h"
#include "bench_sorts.h"
#include <algorithm>
#include <parallel/algorithm>
#include <omp.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#ifdef HAVE_XSS
#define XSS_USE_OPENMP 1
#include "x86simdsort-static-incl.h"
#endif
#ifdef HAVE_IPS4O
#include "ips4o.hpp"
#endif

struct Rng { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double f() { return (double)(next() >> 11) / 9007199254740992.0; } };
static double nowms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void gen_blocks(std::vector<double> &a, Rng &r, size_t nb, int overlap_every) {
    size_t n = a.size(); if (nb > n) nb = n ? n : 1; size_t blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb);
    for (size_t k = 0; k < nb; k++) perm[k] = k;
    for (size_t k = nb - 1; k > 0; k--) std::swap(perm[k], perm[r.next() % (k + 1)]);
    for (size_t i = 0; i < n; i++) { size_t b = perm[i / blk]; double base = (double)b * (double)blk;
        if (overlap_every && (b % overlap_every) == 1) base -= (double)blk * 0.1;
        a[i] = base + (double)(i % blk) + r.f() * 0.5; }
}
static std::vector<double> gen(const std::string &shape, size_t n) {
    Rng r{7}; std::vector<double> a(n);
    if (shape == "uniform") for (auto &v : a) v = r.f() * 2e9 - 1e9;
    else if (shape == "near_sorted") { for (size_t i = 0; i < n; i++) a[i] = (double)i; for (size_t k = 0; k < n / 100; k++) std::swap(a[r.next() % n], a[r.next() % n]); }
    else if (shape == "few_distinct") for (auto &v : a) v = (double)(r.next() % 5) * 1e6;
    else if (shape == "heavy_tail") for (auto &v : a) v = 1000.0 * std::pow(1.0 - r.f() * 0.999999, -1.0 / 1.2);
    else if (shape == "bimodal") for (auto &v : a) v = ((r.next() & 1) ? 1e9 : -1e9) + r.f() * 10.0;
    else if (shape == "quantized") for (auto &v : a) v = (double)(r.next() % 4096) / 64.0;
    else if (shape == "int_wide") for (auto &v : a) v = (double)(int32_t)r.next();
    else if (shape == "block_perm") gen_blocks(a, r, 64, 0);
    else if (shape == "block_overlap") gen_blocks(a, r, 64, 8);
    return a;
}
template <class F> static double best_of(const std::vector<double> &base, int reps, F f, std::vector<double> &out, double budget_ms) {
    double b = 1e300;
    for (int i = 0; i < reps; i++) { std::vector<double> a = base; double t = nowms(); f(a); double ms = nowms() - t; if (ms < b) { b = ms; out.swap(a); } if (ms > budget_ms) break; }
    return b;
}
static std::string jnum(double v) { char b[32]; snprintf(b, sizeof b, "%.4g", v); return b; }

struct Row { size_t n; double t[6]; bool ok; };       /* t: [0]=std::sort or gnu_par, [1]=logos, [2]=xss, [3]=ips4o, [4]=LSD radix, [5]=timsort (1T only); 0 = absent */
struct Series { std::string shape; int threads; std::vector<Row> rows; };

int main() {
    int P = bench_threads(logos_num_cpus());   /* bench_common.h: LOGOS_THREADS, else BENCH_THREADS, else physical cores */
    double max_n = 3e7; if (const char *e = getenv("LOGOS_MAX_N")) { double v = atof(e); if (v >= 1e4) max_n = v; }
    double max_sec = 20; if (const char *e = getenv("LOGOS_MAX_SEC")) { double v = atof(e); if (v > 0) max_sec = v; }
    const char *shapes[] = { "uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide", "block_perm", "block_overlap" };
    std::vector<size_t> sizes; for (double n = 1e4; n <= max_n + 1; n *= 3.1622776601683795) sizes.push_back((size_t)std::llround(n / 1e3) * 1000);
    const char *xss_isa =
#if defined(HAVE_XSS) && defined(__AVX512F__)
        "AVX-512";
#elif defined(HAVE_XSS)
        "AVX2";
#else
        "";
#endif
    /* calibration */
    double memcpy_ms = 1e300, std1m_ms = 1e300;
    { std::vector<double> c1(8000000), c2(8000000); Rng cr{3}; for (auto &v : c1) v = cr.f();
      for (int i = 0; i < 5; i++) { double t = nowms(); memcpy(c2.data(), c1.data(), c1.size() * 8); double ms = nowms() - t; if (ms < memcpy_ms) memcpy_ms = ms; }
      for (int i = 0; i < 3; i++) { std::copy(c1.begin(), c1.begin() + 1000000, c2.begin()); double t = nowms(); std::sort(c2.begin(), c2.begin() + 1000000); double ms = nowms() - t; if (ms < std1m_ms) std1m_ms = ms; } }
    printf("logos-sort scaling: %zu sizes (1e4 .. %.0f), P = %d, budget %.0f s/sort\n", sizes.size(), max_n, P, max_sec);
    printf("calibration (1T): memcpy 64MB %.2f ms, std::sort 1M %.1f ms  <- >15%% drift from quiet = loaded\n\n", memcpy_ms, std1m_ms);

    std::vector<Series> all; int bad = 0;
    for (const char *sh : shapes) {
        Series s1{sh, 1, {}}, sp{sh, P, {}}; bool over = false;
        for (size_t n : sizes) {
            if (over) break;
            auto base = gen(sh, n); int reps = n <= 1000000 ? 5 : n <= 10000000 ? 3 : 1; double budget = max_sec * 1e3;
            std::vector<double> ref, o;
            /* ---- single-threaded ---- */
            omp_set_num_threads(1);
            Row r1{n, {0, 0, 0, 0, 0, 0}, true};
            r1.t[0] = best_of(base, reps, [](std::vector<double> &a) { std::sort(a.begin(), a.end()); }, ref, budget);
            r1.t[1] = best_of(base, reps, [](std::vector<double> &a) { logos_sort_f64(a.data(), a.size(), 1); }, o, budget); r1.ok &= (o == ref);
#ifdef HAVE_XSS
            r1.t[2] = best_of(base, reps, [](std::vector<double> &a) { x86simdsortStatic::qsort(a.data(), a.size(), false, false); }, o, budget); r1.ok &= (o == ref);
#endif
#ifdef HAVE_IPS4O
            r1.t[3] = best_of(base, reps, [](std::vector<double> &a) { ips4o::sort(a.begin(), a.end()); }, o, budget); r1.ok &= (o == ref);
#endif
            r1.t[4] = best_of(base, reps, [](std::vector<double> &a) { radix_sort_f64(a.data(), a.size()); }, o, budget); r1.ok &= (o == ref);
            r1.t[5] = best_of(base, reps, [](std::vector<double> &a) { timsort_f64(a.data(), a.size()); }, o, budget); r1.ok &= (o == ref);
            s1.rows.push_back(r1);
            /* ---- P threads ---- */
            omp_set_num_threads(P);
            Row rp{n, {0, 0, 0, 0, 0, 0}, true};
            rp.t[0] = best_of(base, reps, [](std::vector<double> &a) { __gnu_parallel::sort(a.begin(), a.end()); }, o, budget); rp.ok &= (o == ref);
            rp.t[1] = best_of(base, reps, [P](std::vector<double> &a) { logos_sort_f64(a.data(), a.size(), P); }, o, budget); rp.ok &= (o == ref);
#ifdef HAVE_XSS
            rp.t[2] = best_of(base, reps, [](std::vector<double> &a) { x86simdsortStatic::qsort(a.data(), a.size(), false, false); }, o, budget); rp.ok &= (o == ref);
#endif
#ifdef HAVE_IPS4O
            rp.t[3] = best_of(base, reps, [](std::vector<double> &a) { ips4o::parallel::sort(a.begin(), a.end()); }, o, budget); rp.ok &= (o == ref);
#endif
            sp.rows.push_back(rp);
            if (!r1.ok || !rp.ok) bad++;
            printf("  %-13s n=%-9zu 1T: std %8.1f logos %8.1f xss %8.1f ips4o %8.1f radix %8.1f tim %8.1f | %dT: gnu_par %8.1f logos %8.1f xss %8.1f ips4o %8.1f%s\n", sh, n,
                   r1.t[0], r1.t[1], r1.t[2], r1.t[3], r1.t[4], r1.t[5], P, rp.t[0], rp.t[1], rp.t[2], rp.t[3], (r1.ok && rp.ok) ? "" : "  <-- MISMATCH");
            if (r1.t[0] > budget * 0.5) over = true;        /* the slowest contender is at half the budget: stop this shape here */
        }
        all.push_back(s1); all.push_back(sp);
    }

    /* ---- JSON ---- */
    std::string j = "[";
    for (size_t si = 0; si < all.size(); si++) {
        const Series &s = all[si]; j += (si ? ",{" : "{"); j += "\"shape\":\"" + s.shape + "\",\"threads\":" + std::to_string(s.threads) + ",\"rows\":[";
        for (size_t ri = 0; ri < s.rows.size(); ri++) { const Row &r = s.rows[ri]; j += (ri ? ",{" : "{"); j += "\"n\":" + std::to_string(r.n) + ",\"t\":[" + jnum(r.t[0]) + "," + jnum(r.t[1]) + "," + jnum(r.t[2]) + "," + jnum(r.t[3]) + "," + jnum(r.t[4]) + "," + jnum(r.t[5]) + "],\"ok\":" + (r.ok ? "true" : "false") + "}"; }
        j += "]}";
    }
    j += "]";
    char meta[400]; snprintf(meta, sizeof meta, "{\"threads\":%d,\"memcpyMs\":%.3f,\"std1mMs\":%.2f,\"maxN\":%.0f,\"xss\":%s,\"xssIsa\":\"%s\",\"ips4o\":%s,\"ncpus\":%d}", P, memcpy_ms, std1m_ms, max_n,
#ifdef HAVE_XSS
             "true",
#else
             "false",
#endif
             xss_isa,
#ifdef HAVE_IPS4O
             "true",
#else
             "false",
#endif
             logos_num_cpus());

    static const char *TMPL = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>logos-sort scaling</title>
<style>
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s-logos:#2a78d6;--s-base:#898781;--s-xss:#eb6834;--s-ips:#1baf7a;--s-radix:#eda100;--s-tim:#e87ba4}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-ips:#199e70;--s-radix:#c98500;--s-tim:#d55181}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-ips:#199e70;--s-radix:#c98500;--s-tim:#d55181}
*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding-block:0 3rem;padding-inline:clamp(16px,3vw,32px)}
.wrap{max-width:1100px;margin:0 auto}h1{font-size:22px;margin:28px 0 4px}h2{font-size:16px;margin:26px 0 6px}h3{font-size:13px;margin:0 0 4px;color:var(--ink2);font-weight:600}
.sub{color:var(--ink2);margin:0 0 12px;font-size:13px}.warn{background:var(--surface);border-left:3px solid #fab219;padding:8px 12px;margin:10px 0}
.legend{display:flex;gap:6px 16px;flex-wrap:wrap;font-size:12px;color:var(--ink2);margin:6px 0 10px}.sw{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:6px;border-radius:2px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:820px){.grid2{grid-template-columns:1fr}}
.card{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:12px 14px;margin:0 0 16px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0 6px}
button.f{font:inherit;font-size:13px;padding:4px 12px;border:1px solid var(--axis);background:var(--surface);color:var(--ink);border-radius:999px;cursor:pointer}
button.f[aria-pressed="true"]{background:var(--ink);color:var(--page);border-color:var(--ink)}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px;background:var(--surface)}th,td{padding:4px 8px;text-align:right;border-bottom:1px solid var(--grid)}th{color:var(--muted);font-weight:500;font-size:12px}td:first-child,th:first-child{text-align:left}
.tw{overflow-x:auto;border:1px solid var(--grid);border-radius:6px}details{margin-top:8px}summary{cursor:pointer;color:var(--ink2);font-size:13px}
__UPLOT_CSS__
/* uPlot is themed from the same tokens as the rest of the page. It draws the plot to a canvas, so
   the stroke colours are resolved from these variables in JS at build time and the charts are
   rebuilt when the colour scheme changes. */
.uplot{width:100%}.u-wrap{margin:0 auto}
.uplot,.u-legend{font:12px system-ui,-apple-system,"Segoe UI",sans-serif;color:var(--ink2)}
.u-legend{margin:8px 0 0;text-align:left}
.u-legend th,.u-legend td{border:0;padding:1px 10px 1px 0;font-size:12px}
.u-legend tr{cursor:pointer}
.u-legend .u-series.u-off *{opacity:.35}
.u-legend .u-value{color:var(--ink);font-variant-numeric:tabular-nums;text-align:right}
.u-select{background:color-mix(in srgb,var(--s-logos) 16%,transparent)}
.u-cursor-x,.u-cursor-y{border-color:var(--axis)}
.u-cursor-pt{border-color:var(--surface)}
.hint{color:var(--muted);font-size:12px;margin:2px 0 0}
</style></head><body><div class="wrap">
<h1>logos-sort — time against n</h1>
<p class="sub" id="sub"></p><div id="cal"></div>
<div class="legend" id="legend"></div>
<div class="row" id="filters" role="group" aria-label="axis scale"></div>
<p class="sub">Drag on a chart to zoom, double-click to reset, click a legend row to hide a series. Moving the pointer reads every series at that n. The dashed grey curves are O(n) and O(n log n) references anchored at logos's largest measured point, so a series that bends above the O(n log n) curve is growing faster than a comparison sort should. The label after each series name is its empirical exponent, the least-squares slope of log t against log n over n &ge; 100k (below that fixed costs dominate). The single-threaded panel compares sorts that each use one thread; the parallel panel compares sorts that each use all threads. Every output was checked equal to std::sort's on the same data; the minimum of the repetitions is plotted. A line that stops early hit the per-sort time budget.</p>
<div id="charts"></div>
</div>
<script>__UPLOT_JS__</script>
<script>
const DATA=__DATA__,META=__META__;
const NAMES1=['std::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','LSD radix','timsort'];
const NAMESP=['__gnu_parallel::sort','logos','x86-simd-sort '+META.xssIsa,'IPS\u2074o','',''];
const CVAR=['--s-base','--s-logos','--s-xss','--s-ips','--s-radix','--s-tim'];
const cssv=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim()||'#888888';
const fmtN=n=>n==null?'':n>=1e9?(+(n/1e9).toFixed(2))+'B':n>=1e6?(+(n/1e6).toFixed(2))+'M':n>=1e3?(+(n/1e3).toFixed(1))+'k':String(+n.toFixed(2));
const f1=v=>v>=100?v.toFixed(0):v>=10?v.toFixed(1):v.toFixed(2);
const fmtMs=v=>v==null?'':v>=1000?(+(v/1000).toFixed(1))+' s':v>=0.1?(+v.toFixed(v>=10?0:1))+' ms':(+v.toFixed(3))+' ms';
let MODE='lin';
document.getElementById('sub').textContent=`${META.threads} threads (${META.ncpus} online) \u00b7 sizes 1e4 .. ${fmtN(META.maxN)} \u00b7 ${DATA.length/2} shapes`;
document.getElementById('cal').innerHTML=`<div class="warn">Calibration (1T): memcpy 64 MB ${META.memcpyMs.toFixed(2)} ms, std::sort 1M ${META.std1mMs.toFixed(1)} ms. Quiet 20-core reference \u2248 2.5 ms / \u2248 50 ms; beyond 15% the machine was loaded and ratios within one chart are more trustworthy than absolute times.</div>`;
document.getElementById('legend').innerHTML=[1,0,2,3,4,5].filter(k=>k<2||(k==2&&META.xss)||(k==3&&META.ips4o)||k>=4).map(k=>`<span><i class="sw" style="background:${cssv(CVAR[k])}"></i>${k==0?'std::sort (1T) / __gnu_parallel::sort (P)':NAMES1[k]+(k>=4?' (1 thread only)':'')}</span>`).join('')+`<span><i class="sw ref"></i>O(n) and O(n log n), anchored at logos's largest n</span>`;

/* empirical exponent: least-squares slope of log10 t vs log10 n over n >= 1e5 (fixed costs dominate below) */
function exponent(rows,k){const pts=rows.filter(r=>r.t[k]>0&&r.n>=1e5).map(r=>[Math.log10(r.n),Math.log10(r.t[k])]);if(pts.length<3)return null;
 const mx=pts.reduce((a,p)=>a+p[0],0)/pts.length,my=pts.reduce((a,p)=>a+p[1],0)/pts.length;let sxx=0,sxy=0;pts.forEach(p=>{sxx+=(p[0]-mx)**2;sxy+=(p[0]-mx)*(p[1]-my);});return sxx>0?sxy/sxx:null;}

function table(s){const names=s.threads==1?NAMES1:NAMESP;const cols=[1,0,2,3,4,5].filter(k=>s.rows.some(r=>r.t[k]>0));
 const exps=cols.map(k=>{const e=exponent(s.rows,k);return e===null?'\u2013':'n^'+e.toFixed(2);});
 return `<details><summary>table and empirical exponents</summary><div class="tw"><table><thead><tr><th>n</th>${cols.map(k=>`<th>${names[k]} ms</th>`).join('')}<th>logos vs best other</th></tr></thead><tbody>${s.rows.map(r=>{const others=cols.filter(k=>k!=1&&r.t[k]>0).map(k=>r.t[k]);const best=others.length?Math.min(...others):0;
 return `<tr><td>${fmtN(r.n)}</td>${cols.map(k=>`<td>${r.t[k]>0?f1(r.t[k]):'\u2013'}</td>`).join('')}<td>${best?(best/r.t[1]).toFixed(2)+'\u00d7':'\u2013'}${r.ok?'':' \u2717'}</td></tr>`;}).join('')}<tr><td>t \u221d (n \u2265 100k)</td>${exps.map(e=>`<td>${e}</td>`).join('')}<td></td></tr></tbody></table></div></details>`;}

/* One uPlot per panel. Series are built in draw order with logos LAST so it sits on top, and the two
   big-O references are appended as dashed series so the live legend reads them out like any other. */
function build(s,holder){
 const names=s.threads==1?NAMES1:NAMESP,lg=MODE==='log';
 const xs=s.rows.map(r=>r.n);
 const data=[xs],series=[{label:'n',value:(u,v)=>v==null?'\u2013':fmtN(v)}];
 [0,2,3,4,5,1].forEach(k=>{
  if(!s.rows.some(r=>r.t[k]>0))return;
  data.push(s.rows.map(r=>r.t[k]>0?r.t[k]:null));
  const ex=exponent(s.rows,k),bad=s.rows.some(r=>r.t[k]>0&&!r.ok);
  series.push({label:(names[k]||('series '+k))+(ex!==null?' ~n^'+ex.toFixed(2):'')+(bad?' \u2717':''),
   stroke:cssv(CVAR[k]),width:k===1?2.5:1.75,points:{show:true,size:6},
   value:(u,v)=>v==null?'\u2013':fmtMs(v)});
 });
 const lr=s.rows.filter(r=>r.t[1]>0);
 if(lr.length){const L=lr[lr.length-1];
  [['O(n)',n=>n],['O(n log n)',n=>n*Math.log2(n)]].forEach(([nm,f])=>{
   const a=L.t[1]/f(L.n);
   data.push(xs.map(n=>a*f(n)));
   series.push({label:nm,stroke:cssv('--muted'),width:1.25,dash:[5,4],points:{show:false},value:(u,v)=>v==null?'\u2013':fmtMs(v)});});}
 const ax={stroke:cssv('--muted'),grid:{stroke:cssv('--grid'),width:1},ticks:{stroke:cssv('--axis'),width:1},font:'11px system-ui',labelFont:'12px system-ui'};
 const yscale=lg?{distr:3}:{distr:1,range:(u,mn,mx)=>[0,(mx||1)*1.05]};
 const opts={
  width:Math.max(260,holder.clientWidth||520),height:300,padding:[10,10,0,0],
  scales:{x:{time:false,distr:lg?3:1},y:yscale},
  /* On a log axis uPlot passes null for every split it wants gridded but NOT labelled, so both
     formatters have to hand null straight back or they throw on the first minor tick. */
  axes:[Object.assign({},ax,{values:(u,v)=>v.map(x=>x==null?null:fmtN(x)),label:'n (elements)',labelSize:28}),
        Object.assign({},ax,{values:(u,v)=>v.map(x=>x==null?null:fmtMs(x)),size:66})],
  series,
  cursor:{drag:{x:true,y:true,setScale:true},focus:{prox:24},points:{size:9}},
  legend:{live:true},
 };
 return new uPlot(opts,data,holder);
}

const CH=[];
function render(){
 CH.forEach(c=>c.u.destroy());CH.length=0;
 const host=document.getElementById('charts');host.innerHTML='';
 const pending=[];
 for(let i=0;i<DATA.length;i+=2){
  const s1=DATA[i],sp=DATA[i+1];
  const h2=document.createElement('h2');h2.textContent=s1.shape;host.appendChild(h2);
  const g=document.createElement('div');g.className='grid2';host.appendChild(g);
  [s1,sp].forEach(s=>{
   const card=document.createElement('div');card.className='card';
   const h3=document.createElement('h3');h3.textContent=s.threads==1?'1 thread each':s.threads+' threads each';card.appendChild(h3);
   const holder=document.createElement('div');card.appendChild(holder);
   const det=document.createElement('div');det.innerHTML=table(s);card.appendChild(det);
   g.appendChild(card);
   if(s.rows.length)pending.push([s,holder]);});}
 /* instantiate only after the DOM is laid out, so clientWidth is real */
 pending.forEach(([s,holder])=>{CH.push({u:build(s,holder),holder});});
}

const fl=document.getElementById('filters');
[['lin','linear axes'],['log','log-log axes']].forEach(([v,l])=>{const b=document.createElement('button');b.className='f';b.textContent=l;b.setAttribute('aria-pressed',v===MODE);
 b.onclick=()=>{MODE=v;fl.querySelectorAll('button[data-ax]').forEach(x=>x.setAttribute('aria-pressed',x===b));render();};b.dataset.ax=v;fl.appendChild(b);});
{const b=document.createElement('button');b.className='f';b.textContent='reset zoom';b.onclick=()=>render();fl.appendChild(b);}

render();
let rt;addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(()=>CH.forEach(c=>c.u.setSize({width:Math.max(260,c.holder.clientWidth||520),height:300})),120);});
const mq=matchMedia('(prefers-color-scheme: dark)');
(mq.addEventListener?mq.addEventListener.bind(mq,'change'):mq.addListener.bind(mq))(()=>render());
</script></body></html>
)HTML";
    std::string html(TMPL);
    size_t p = html.find("__DATA__"); if (p != std::string::npos) html.replace(p, 8, j);
    p = html.find("__META__"); if (p != std::string::npos) html.replace(p, 8, std::string(meta));
    /* uPlot is inlined, not fetched: these reports are opened from disk and passed around, so the
       page must not depend on a CDN. See engines/c/vendor/README.md and tools/embed_uplot.py. */
    p = html.find("__UPLOT_CSS__"); if (p != std::string::npos) html.replace(p, 13, UPLOT_CSS);
    p = html.find("__UPLOT_JS__");  if (p != std::string::npos) html.replace(p, 12, UPLOT_JS);
    FILE *f = fopen("scale-report.html", "w");
    if (f) { fwrite(html.data(), 1, html.size(), f); fclose(f); printf("\nwrote scale-report.html\n"); }
    printf("RESULT: %s\n", bad ? "OUTPUT MISMATCHES — do not trust timings" : "all outputs verified against std::sort");
    return bad ? 1 : 0;
}
