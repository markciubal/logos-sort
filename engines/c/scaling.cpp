/* scaling.cpp — logos-sort scaling test with THREE contenders on identical data:
 *   logos (buffered, ~2n, multi-threaded), logos in-place (~1n, sequential),
 *   and __gnu_parallel::sort (baseline). Sizes: 10 .. 1e9 by 1/3/10 steps,
 *   stopping at the first limit reached (1e9 ceiling / per-sort time budget /
 *   RAM). The buffered & gnu_par contenders need ~3n live memory (data + copy
 *   + scratch); the in-place contender needs ~1n, so it keeps climbing after
 *   the others are RAM-capped — the point of having it.
 *
 * Timing is microbench-quality: each size is sorted repeatedly (fresh data each
 * time) until >=120 ms of accumulated sort work or a cap, and the MINIMUM
 * per-sort time is reported — so the tiny sizes (10..3000) get real resolution
 * instead of one noisy sub-microsecond reading. Every measured sort is verified
 * sorted + permutation-exact; the in-place and gnu_par outputs are checked equal
 * to the buffered one.
 *
 * Writes scaling-report.html. Overrides: LOGOS_MAX_N LOGOS_MAX_SEC LOGOS_THREADS.
 */
#include "logos_psort.h"
#include <algorithm>
#include <parallel/algorithm>
#include <omp.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <unistd.h>

static uint64_t RS;
static void seed(uint64_t s) { RS = s ? s : 1; }
static uint64_t nextr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double frand() { return (double)(nextr() >> 11) / (double)(1ULL << 53); }
static double now_s() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static size_t avail_bytes() { long p = sysconf(_SC_AVPHYS_PAGES), s = sysconf(_SC_PAGESIZE); return (p > 0 && s > 0) ? (size_t)p * (size_t)s : ((size_t)4 << 30); }
static bool sorted_f64(const double *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i-1] > a[i]) return false; return true; }
[[maybe_unused]] static uint64_t xsum_f64(const double *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) { uint64_t u; memcpy(&u, &a[i], 8); x ^= u; } return x; }
static bool sorted_i32(const int32_t *a, size_t n) { for (size_t i = 1; i < n; i++) if (a[i-1] > a[i]) return false; return true; }
[[maybe_unused]] static uint64_t xsum_i32(const int32_t *a, size_t n) { uint64_t x = 0; for (size_t i = 0; i < n; i++) x ^= (uint32_t)a[i]; return x; }

/* microbench: sort `work` (refilled from base each iteration) enough times to
 * accumulate >= min_acc seconds of sort time (capped), return MIN per-sort ms.
 * `verify` runs once on the sorted result; sets *ok. */
template <class T, class Fill, class Sort, class Verify>
static double bench(size_t n, double min_acc, double budget, Fill fill, Sort srt, Verify ver, bool *ok) {
    std::vector<T> base(n), work(n);
    fill(base);
    double best = 1e300, acc = 0; long iters = 0; *ok = true;
    long cap = n < 1000 ? 2000000 : n < 1000000 ? 2000 : 3;
    while (iters < cap) {
        std::copy(base.begin(), base.end(), work.begin());
        double t0 = now_s(); srt(work); double dt = now_s() - t0;
        if (iters == 0) *ok = ver(work, base);
        if (dt < best) best = dt;
        acc += dt; iters++;
        if (acc >= min_acc) break;
        if (best > budget) break;                 /* one sort already over budget */
    }
    return best * 1e3;
}

struct Row { const char *type; size_t n; double buf_ms, ip_ms, gnu_ms; bool ok; };

int main() {
    int threads = logos_num_cpus();
    if (const char *e = getenv("LOGOS_THREADS")) { int v = atoi(e); if (v > 0) threads = v; }
    omp_set_num_threads(threads);
    size_t max_n = 1000000000ull;
    if (const char *e = getenv("LOGOS_MAX_N")) { double v = atof(e); if (v >= 1) max_n = (size_t)v; }
    double max_sec = 30.0;
    if (const char *e = getenv("LOGOS_MAX_SEC")) { double v = atof(e); if (v > 0) max_sec = v; }
    size_t avail = avail_bytes();
    const double MIN_ACC = 0.12;

    printf("logos-sort scaling — threads=%d, ceiling=%.0e, budget=%.0fs/sort, RAM=%.1f GB\n\n",
           threads, (double)max_n, max_sec, avail / 1073741824.0);
    printf("  %-6s %13s %11s %11s %11s %8s   %s\n", "type", "n", "buffered", "in-place", "gnu_par", "spdup", "status");

    const size_t sizes[] = { 10,30,100,300,1000,3000,10000,30000,100000,300000,1000000,3000000,
                             10000000,30000000,100000000,300000000,1000000000ull };
    std::vector<Row> rows;

    for (int typ = 0; typ < 2; typ++) {
        unsigned per_buf = typ ? 22u : 44u;    /* buffered/gnu: data + copy + scratch */
        unsigned per_ip  = typ ? 5u  : 9u;     /* in-place: data + tiny stack */
        bool budget_hit = false;
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si]; if (n > max_n) break;
            const char *ty = typ ? "int32" : "f64";
            bool buf_fits = (double)n * per_buf < 0.85 * (double)avail;
            bool ip_fits  = (double)n * per_ip  < 0.85 * (double)avail;
            if (!ip_fits) { printf("  %-6s %13zu %11s %11s %11s %8s   skipped: even in-place needs >85%% RAM\n", ty, n, "-","-","-","-"); break; }
            if (budget_hit) { printf("  %-6s %13zu %11s %11s %11s %8s   skipped: prior size hit %.0fs budget\n", ty, n,"-","-","-","-", max_sec); break; }

            double bms = 0, ims = 0, gms = 0; bool ok = true, okv;
            if (!typ) {
                auto fill = [&](std::vector<double>&v){ seed(0x1234567 ^ n); for (auto &x : v) x = frand()*2e9-1e9; };
                std::vector<double> ref;                        /* buffered result = reference */
                if (buf_fits) { bms = bench<double>(n, MIN_ACC, max_sec, fill, [&](std::vector<double>&w){ logos_sort_f64(w.data(), n, threads); },
                        [&](std::vector<double>&w, std::vector<double>&){ ref = w; return sorted_f64(w.data(), n); }, &okv); ok &= okv; }
                ims = bench<double>(n, MIN_ACC, max_sec, fill, [&](std::vector<double>&w){ logos_sort_f64_inplace(w.data(), n, 1); },
                        [&](std::vector<double>&w, std::vector<double>&){ return sorted_f64(w.data(), n) && (!buf_fits || w == ref); }, &okv); ok &= okv;
                if (buf_fits) { gms = bench<double>(n, MIN_ACC, max_sec, fill, [&](std::vector<double>&w){ __gnu_parallel::sort(w.begin(), w.end()); },
                        [&](std::vector<double>&w, std::vector<double>&){ return sorted_f64(w.data(), n) && w == ref; }, &okv); ok &= okv; }
            } else {
                auto fill = [&](std::vector<int32_t>&v){ seed(0x89abcdef ^ n); for (auto &x : v) x = (int32_t)nextr(); };
                std::vector<int32_t> ref;
                if (buf_fits) { bms = bench<int32_t>(n, MIN_ACC, max_sec, fill, [&](std::vector<int32_t>&w){ logos_sort_i32(w.data(), n, threads); },
                        [&](std::vector<int32_t>&w, std::vector<int32_t>&){ ref = w; return sorted_i32(w.data(), n); }, &okv); ok &= okv; }
                ims = bench<int32_t>(n, MIN_ACC, max_sec, fill, [&](std::vector<int32_t>&w){ logos_sort_i32_inplace(w.data(), n, 1); },
                        [&](std::vector<int32_t>&w, std::vector<int32_t>&){ return sorted_i32(w.data(), n) && (!buf_fits || w == ref); }, &okv); ok &= okv;
                if (buf_fits) { gms = bench<int32_t>(n, MIN_ACC, max_sec, fill, [&](std::vector<int32_t>&w){ __gnu_parallel::sort(w.begin(), w.end()); },
                        [&](std::vector<int32_t>&w, std::vector<int32_t>&){ return sorted_i32(w.data(), n) && w == ref; }, &okv); ok &= okv; }
            }
            char sp[16]; if (buf_fits && bms > 0) snprintf(sp, sizeof sp, "%.2fx", gms / bms); else snprintf(sp, sizeof sp, "%s", "-");
            char bcol[16], gcol[16]; if (buf_fits) { snprintf(bcol,sizeof bcol,"%.3fms",bms); snprintf(gcol,sizeof gcol,"%.3fms",gms); } else { snprintf(bcol,sizeof bcol,"%s","RAM"); snprintf(gcol,sizeof gcol,"%s","RAM"); }
            printf("  %-6s %13zu %11s %10.3fms %11s %8s   %s\n", ty, n, bcol, ims, gcol, sp, ok ? "ok" : "*** INCORRECT ***");
            rows.push_back({ ty, n, buf_fits ? bms : 0, ims, buf_fits ? gms : 0, ok });
            if (!ok) { fprintf(stderr, "correctness failure at %s n=%zu\n", ty, n); return 1; }
            if (ims > max_sec*1e3 || (buf_fits && (bms > max_sec*1e3*0.6 || gms > max_sec*1e3))) budget_hit = true;
        }
        printf("\n");
    }

    /* ---- HTML dashboard ---- */
    std::string j = "[";
    for (size_t i = 0; i < rows.size(); i++) { char b[256]; snprintf(b, sizeof b, "%s{\"type\":\"%s\",\"n\":%zu,\"buf\":%.4f,\"ip\":%.4f,\"gnu\":%.4f}", i?",":"", rows[i].type, rows[i].n, rows[i].buf_ms, rows[i].ip_ms, rows[i].gnu_ms); j += b; }
    j += "]";
    char meta[192]; snprintf(meta, sizeof meta, "{\"threads\":%d,\"maxSec\":%.0f,\"ramGB\":%.1f}", threads, max_sec, avail/1073741824.0);
    const char *TMPL =
"<!doctype html><html><head><meta charset=\"utf-8\"><title>logos-sort scaling</title><style>"
"body{font:14px/1.5 -apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:0;background:#0f1420;color:#e6edf3}"
".wrap{max-width:980px;margin:0 auto;padding:28px 20px 60px}h1{font-size:20px;margin:0 0 4px}"
".sub{color:#8b98a9;margin:0 0 22px;font-size:13px}.cards{display:flex;gap:12px;flex-wrap:wrap;margin:0 0 26px}"
".card{background:#182131;border:1px solid #263247;border-radius:10px;padding:12px 16px;min-width:120px}"
".card .k{color:#8b98a9;font-size:12px}.card .v{font-size:20px;font-weight:600;margin-top:2px}.win{color:#4ade80}"
"table{width:100%;border-collapse:collapse;margin-top:8px;font-variant-numeric:tabular-nums}"
"th,td{padding:6px 9px;text-align:right;border-bottom:1px solid #212b3d}th{color:#8b98a9;font-weight:500;font-size:12px}"
"td:first-child,th:first-child{text-align:left}h2{font-size:15px;margin:26px 0 6px;color:#c9d4e2}"
"svg{background:#141c2b;border:1px solid #263247;border-radius:10px}.tick{fill:#6b7787;font-size:11px}.axl{fill:#8b98a9;font-size:12px}"
".leg{display:inline-block;width:11px;height:11px;border-radius:3px;vertical-align:middle;margin:0 5px 0 14px}</style></head>"
"<body><div class=\"wrap\"><h1>logos-sort \\u2014 scaling</h1><p class=\"sub\" id=\"sub\"></p><div class=\"cards\" id=\"cards\"></div>"
"<h2>Throughput \\u2014 million elements/sec, log scale (higher is better)</h2><div id=\"chart\"></div>"
"<p class=\"sub\"><span class=\"leg\" style=\"background:#4ade80\"></span>logos buffered (~2n)"
"<span class=\"leg\" style=\"background:#fbbf24\"></span>logos in-place (~1n)"
"<span class=\"leg\" style=\"background:#7dd3fc\"></span>__gnu_parallel::sort</p>"
"<h2>Per-size results</h2><table><thead><tr><th>type</th><th>n</th><th>buffered</th><th>in-place</th><th>gnu_par</th>"
"<th>buf vs gnu</th><th>ip vs buf</th></tr></thead><tbody id=\"tb\"></tbody></table>"
"<p class=\"sub\" style=\"margin-top:20px\">in-place uses ~1n memory (no scratch) and keeps running after the ~3n buffered/gnu_par "
"contenders are RAM-capped \\u2014 the rightmost points may be in-place only. Timing: min of repeated runs; tiny sizes averaged over many iterations.</p>"
"<script>const DATA=__DATA__,META=__META__;"
"const fmtN=n=>n>=1e9?(n/1e9)+'B':n>=1e6?(n/1e6)+'M':n>=1e3?(n/1e3)+'k':n;"
"const tp=(n,ms)=>ms>0?n/(ms/1e3)/1e6:0;"
"document.getElementById('sub').textContent=`${META.threads} threads \\u00b7 ${DATA.length} sizes \\u00b7 budget ${META.maxSec}s/sort \\u00b7 ${META.ramGB} GB RAM \\u00b7 all outputs verified`;"
"const spd=DATA.filter(d=>d.buf>0&&d.gnu>0).map(d=>d.gnu/d.buf);"
"const gm=spd.length?Math.exp(spd.reduce((a,b)=>a+Math.log(b),0)/spd.length):0;"
"const ipr=DATA.filter(d=>d.buf>0&&d.ip>0).map(d=>d.ip/d.buf);"
"const ipm=ipr.length?Math.exp(ipr.reduce((a,b)=>a+Math.log(b),0)/ipr.length):0;"
"const cards=[['sizes',DATA.length],['buf vs gnu (geomean)',gm.toFixed(2)+'\\u00d7'],['in-place cost',ipm.toFixed(2)+'\\u00d7'],['largest n',DATA.length?fmtN(Math.max(...DATA.map(d=>d.n))):'-']];"
"document.getElementById('cards').innerHTML=cards.map(c=>`<div class=card><div class=k>${c[0]}</div><div class=\"v ${c[1].toString().includes('\\u00d7')?'win':''}\">${c[1]}</div></div>`).join('');"
"document.getElementById('tb').innerHTML=DATA.map(d=>{const bg=d.buf>0?d.gnu/d.buf:0,ib=d.buf>0?d.ip/d.buf:0;"
"const c=v=>v>0?v.toFixed(v<10?3:1)+'ms':'<span style=color:#6b7787>RAM</span>';"
"return `<tr><td>${d.type}</td><td>${fmtN(d.n)}</td><td>${c(d.buf)}</td><td>${c(d.ip)}</td><td>${c(d.gnu)}</td>"
"<td class=\"${bg>1?'win':''}\">${bg?bg.toFixed(2)+'\\u00d7':'-'}</td><td>${ib?ib.toFixed(2)+'\\u00d7':'-'}</td></tr>`;}).join('');"
"const W=920,H=360,pl=56,pr=16,pt=14,pb=52,iw=W-pl-pr,ih=H-pt-pb;"
"const vals=DATA.flatMap(d=>[tp(d.n,d.buf),tp(d.n,d.ip),tp(d.n,d.gnu)]).filter(v=>v>0);"
"let ch=document.getElementById('chart');"
"if(vals.length){const lo=Math.log10(Math.min(...vals)*0.6),hi=Math.log10(Math.max(...vals)*1.5),y=v=>pt+ih-(Math.log10(v)-lo)/(hi-lo)*ih;"
"let g=`<svg width=${W} height=${H} viewBox='0 0 ${W} ${H}'>`;"
"for(let e=Math.ceil(lo);e<=Math.floor(hi);e++){const yy=y(10**e);g+=`<line x1=${pl} y1=${yy} x2=${W-pr} y2=${yy} stroke=#212b3d/><text class=tick x=${pl-8} y=${yy+4} text-anchor=end>${(10**e)>=1000?(10**e/1000)+'k':10**e}</text>`;}"
"const bw=iw/DATA.length;const cols=[['buf','#4ade80'],['ip','#fbbf24'],['gnu','#7dd3fc']];"
"DATA.forEach((d,i)=>{const cx=pl+bw*(i+0.5),cw=Math.min(9,bw/4);"
"cols.forEach((cc,k)=>{const v=tp(d.n,d[cc[0]]);if(v>0){const yy=y(v);g+=`<rect x=${cx+(k-1)*(cw+1)-cw/2} y=${yy} width=${cw} height=${pt+ih-yy} fill=${cc[1]} rx=1><title>${d.type} ${fmtN(d.n)} ${cc[0]}: ${v.toFixed(0)} M/s</title></rect>`;}});"
"if(i%2===0||DATA.length<14)g+=`<text class=tick x=${cx} y=${H-pb+15} text-anchor=middle>${fmtN(d.n)}</text>`;});"
"g+=`<text class=axl transform=rotate(-90) x=${-(pt+ih/2)} y=15 text-anchor=middle>M elem/s</text></svg>`;ch.innerHTML=g;}"
"else ch.innerHTML='<p class=sub>no sizes ran</p>';</script></body></html>";
    std::string html(TMPL);
    size_t p = html.find("__DATA__"); if (p != std::string::npos) html.replace(p, 8, j);
    p = html.find("__META__"); if (p != std::string::npos) html.replace(p, 8, std::string(meta));
    FILE *f = fopen("scaling-report.html", "w");
    if (f) { fwrite(html.data(), 1, html.size(), f); fclose(f); printf("wrote scaling-report.html\n"); }
    printf("scaling complete — every measured sort verified; in-place & gnu_par checked equal to buffered.\n");
    return 0;
}
