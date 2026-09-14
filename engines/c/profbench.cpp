/* profbench.cpp — where logos_sort_f64 spends its time, and where it stalls.
 *
 * Runs the engine (compiled with -DLOGOS_PROFILE) on the parbench shapes plus
 * the block-permuted shapes, at 1 thread and at P threads, keeps the profile of
 * the fastest of R runs per cell, verifies every output against std::sort, and
 * writes profile-report.html:
 *   - a treemap per (shape, threads) of all thread-time: every phase's working
 *     time (leaf sorts split by the path each bucket took), time blocked at
 *     barriers ("stalled"), and idle time after a thread ran out of work
 *     ("imbalance") — so "percent of compute" is read directly off the area;
 *   - a per-thread stacked bar for the P-thread run (busy by phase, stalled, idle);
 *   - a phase table with wall time, share, thread utilisation, and for the
 *     streaming phases the achieved GB/s against the memcpy calibration
 *     (a phase far below streaming bandwidth is latency-bound, i.e. stalling on
 *     memory, not on compute).
 * Build: make profile   (g++, no OpenMP needed).  Overrides: LOGOS_THREADS, LOGOS_N.
 */
#include "logos_psort.h"
#include "bench_common.h"
#include "bench_sorts.h"
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <parallel/algorithm>
#include <omp.h>
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
    size_t n = a.size(), blk = (n + nb - 1) / nb; std::vector<size_t> perm(nb);
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
    else if (shape == "mod_card_30k") for (auto &v : a) v = (double)(r.next() % 30000) * 1.5;
    else if (shape == "block_perm") gen_blocks(a, r, 64, 0);
    else if (shape == "block_overlap") gen_blocks(a, r, 64, 8);
    return a;
}

/* base = the fair-fight opponent at the same thread count (std::sort at 1T, __gnu_parallel::sort at P);
 * xss = x86-simd-sort at the same thread count (OpenMP threads set to 1 or P) when built with -DHAVE_XSS;
 * ips4o = IPS4o (sequential at 1T, parallel at P) when built with -DHAVE_IPS4O; 0 when absent */
struct Cell { std::string shape; int threads; bool ok; logos_profile_t prof; double base_ms; const char *base_name; double xss_ms; double ips4o_ms; double radix_ms, tim_ms; /* 1T cells only */ };

static std::string jnum(double v) { char b[32]; snprintf(b, sizeof b, "%.4g", v); return b; }

int main() {
    int P = bench_threads(logos_num_cpus());   /* bench_common.h: LOGOS_THREADS, else BENCH_THREADS, else physical cores */
    size_t N = 8000000; if (const char *e = getenv("LOGOS_N")) { double v = atof(e); if (v >= 4096) N = (size_t)v; }
    const int R = 5;                       /* min of 5 (make noiseprobe: median/min 1.07 at 1T, 1.26 at 20T over 12 reps on this box) */
    const char *shapes[] = { "uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide", "mod_card_30k", "block_perm", "block_overlap" };
    if (!logos_profile_get()) { fprintf(stderr, "engine was not built with -DLOGOS_PROFILE (use make profile)\n"); return 2; }

    omp_set_num_threads(P);
    /* calibration: streaming bandwidth (memcpy 64MB), a 1M single-thread logos sort, and parbench's std::sort 1M */
    double memcpy_ms = 1e300, sort1m_ms = 1e300, std1m_ms = 1e300; size_t cal_n = 8000000;
    { std::vector<double> c1(cal_n), c2(cal_n); Rng cr{3}; for (auto &v : c1) v = cr.f();
      for (int i = 0; i < 5; i++) { double t = nowms(); memcpy(c2.data(), c1.data(), cal_n * 8); double ms = nowms() - t; if (ms < memcpy_ms) memcpy_ms = ms; }
      for (int i = 0; i < 3; i++) { std::copy(c1.begin(), c1.begin() + 1000000, c2.begin()); double t = nowms(); logos_sort_f64(c2.data(), 1000000, 1); double ms = nowms() - t; if (ms < sort1m_ms) sort1m_ms = ms; }
      for (int i = 0; i < 3; i++) { std::copy(c1.begin(), c1.begin() + 1000000, c2.begin()); double t = nowms(); std::sort(c2.begin(), c2.begin() + 1000000); double ms = nowms() - t; if (ms < std1m_ms) std1m_ms = ms; } }
    double memcpy_gbps = (cal_n * 8.0 * 2) / (memcpy_ms * 1e6);   /* read + write */
    printf("logos-sort profile + fair fight, n = %zu, best of %d, P = %d threads\n", N, R, P);
    printf("calibration (1T): memcpy 64MB %.2f ms (%.1f GB/s read+write), std::sort 1M %.1f ms, logos 1M uniform %.1f ms  <- >15%% drift from quiet = loaded\n\n", memcpy_ms, memcpy_gbps, std1m_ms, sort1m_ms);

    std::vector<Cell> cells; int bad = 0;
    printf("  %-14s %3s %10s %10s %6s   %-22s %s\n", "shape", "thr", "logos", "opponent", "x", "route", "top phases");
    for (const char *sh : shapes) {
        auto base = gen(sh, N); std::vector<double> ref = base; std::sort(ref.begin(), ref.end());
        int tset[2] = { 1, P };
        for (int ti = 0; ti < (P > 1 ? 2 : 1); ti++) {
            int t = tset[ti]; Cell c; c.shape = sh; c.threads = t; c.ok = true; c.prof.total_ms = 1e300;
            for (int r = 0; r < R; r++) {
                std::vector<double> a = base; logos_profile_reset(); logos_sort_f64(a.data(), a.size(), t);
                const logos_profile_t *pf = logos_profile_get();
                if (r == 0 && a != ref) c.ok = false;
                if (pf->total_ms < c.prof.total_ms) c.prof = *pf;
            }
            /* the fair-fight opponent at the same thread count, same data, verified (parbench's rule: 1T vs 1T, P vs P) */
            omp_set_num_threads(t);                /* every opponent runs at the cell's thread count: 1T vs 1T, P vs P */
            c.base_ms = 1e300; c.base_name = t == 1 ? "std::sort" : "__gnu_parallel::sort"; c.xss_ms = 0; c.ips4o_ms = 0; c.radix_ms = 0; c.tim_ms = 0;
            if (t == 1) {   /* textbook single-threaded baselines (bench_sorts.h) */
                c.radix_ms = 1e300; for (int r = 0; r < R; r++) { std::vector<double> a = base; double t0 = nowms(); radix_sort_f64(a.data(), a.size()); double ms = nowms() - t0; if (ms < c.radix_ms) c.radix_ms = ms; if (r == 0 && a != ref) c.ok = false; }
                c.tim_ms = 1e300;   for (int r = 0; r < R; r++) { std::vector<double> a = base; double t0 = nowms(); timsort_f64(a.data(), a.size());    double ms = nowms() - t0; if (ms < c.tim_ms) c.tim_ms = ms;   if (r == 0 && a != ref) c.ok = false; }
            }
            for (int r = 0; r < R; r++) {
                std::vector<double> a = base; double t0 = nowms();
                if (t == 1) std::sort(a.begin(), a.end()); else __gnu_parallel::sort(a.begin(), a.end());
                double ms = nowms() - t0; if (ms < c.base_ms) c.base_ms = ms; if (r == 0 && a != ref) c.ok = false;
            }
#ifdef HAVE_XSS
            { c.xss_ms = 1e300; for (int r = 0; r < R; r++) { std::vector<double> a = base; double t0 = nowms(); x86simdsortStatic::qsort(a.data(), a.size(), false, false); double ms = nowms() - t0; if (ms < c.xss_ms) c.xss_ms = ms; if (r == 0 && a != ref) c.ok = false; } }
#endif
#ifdef HAVE_IPS4O
            { c.ips4o_ms = 1e300; for (int r = 0; r < R; r++) { std::vector<double> a = base; double t0 = nowms(); if (t == 1) ips4o::sort(a.begin(), a.end()); else ips4o::parallel::sort(a.begin(), a.end()); double ms = nowms() - t0; if (ms < c.ips4o_ms) c.ips4o_ms = ms; if (r == 0 && a != ref) c.ok = false; } }
#endif
            if (!c.ok) bad++;
            /* console line: the two biggest phases */
            int p1 = -1, p2 = -1;
            for (int ph = 0; ph < LOGOS_PH_N; ph++) { double v = c.prof.phase_ms[ph]; if (p1 < 0 || v > c.prof.phase_ms[p1]) { p2 = p1; p1 = ph; } else if (p2 < 0 || v > c.prof.phase_ms[p2]) p2 = ph; }
            printf("  %-14s %3d %8.1fms %8.1fms %5.2fx   %-22s %s %.0f%%, %s %.0f%%%s\n", sh, t, c.prof.total_ms, c.base_ms, c.base_ms / c.prof.total_ms, logos_profile_route_names[c.prof.route],
                   logos_profile_phase_names[p1], 100 * c.prof.phase_ms[p1] / c.prof.total_ms,
                   p2 >= 0 ? logos_profile_phase_names[p2] : "-", p2 >= 0 ? 100 * c.prof.phase_ms[p2] / c.prof.total_ms : 0.0, c.ok ? "" : "  <-- MISMATCH");
            cells.push_back(c);
        }
    }

    /* ---- JSON ---- */
    std::string j = "[";
    for (size_t ci = 0; ci < cells.size(); ci++) {
        const Cell &c = cells[ci]; const logos_profile_t &p = c.prof; int T = p.nthreads;
        j += (ci ? ",{" : "{");
        j += "\"shape\":\"" + c.shape + "\",\"threads\":" + std::to_string(T) + ",\"n\":" + std::to_string(p.n) + ",\"ok\":" + (c.ok ? "true" : "false");
        j += ",\"total\":" + jnum(p.total_ms) + ",\"base\":" + jnum(c.base_ms) + ",\"baseName\":\"" + c.base_name + "\",\"xss\":" + jnum(c.xss_ms) + ",\"ips4o\":" + jnum(c.ips4o_ms) + ",\"radix\":" + jnum(c.radix_ms) + ",\"tim\":" + jnum(c.tim_ms);
        j += std::string(",\"route\":\"") + logos_profile_route_names[p.route] + "\",\"phases\":[";
        for (int ph = 0; ph < LOGOS_PH_N; ph++) {
            j += (ph ? ",{" : "{"); j += "\"name\":\"" + std::string(logos_profile_phase_names[ph]) + "\",\"wall\":" + jnum(p.phase_ms[ph]) + ",\"busy\":[";
            for (int t = 0; t < T; t++) j += (t ? "," : "") + jnum(p.busy_ms[t][ph]);
            j += "],\"wait\":[";
            for (int t = 0; t < T; t++) j += (t ? "," : "") + jnum(p.wait_ms[t][ph]);
            j += "],\"cpu\":[";
            for (int t = 0; t < T; t++) j += (t ? "," : "") + jnum(p.cpu_ms[t][ph]);
            j += "]}";
        }
        j += "],\"paths\":[";
        bool first = true;
        for (int pa = 1; pa < LOGOS_PATH_N; pa++) {
            double ms = 0; size_t calls = 0, elems = 0;
            for (int t = 0; t < T; t++) { ms += p.path_ms[t][pa]; calls += p.path_calls[t][pa]; elems += p.path_elems[t][pa]; }
            if (!calls) continue;
            j += (first ? "{" : ",{"); first = false;
            j += "\"name\":\"" + std::string(logos_profile_path_names[pa]) + "\",\"ms\":" + jnum(ms) + ",\"calls\":" + std::to_string(calls) + ",\"elems\":" + std::to_string(elems) + "}";
        }
        j += "]}";
    }
    j += "]";
    const char *xss_isa =
#if defined(HAVE_XSS) && defined(__AVX512F__)
        "AVX-512";
#elif defined(HAVE_XSS)
        "AVX2";
#else
        "";
#endif
    char meta[400]; snprintf(meta, sizeof meta, "{\"threads\":%d,\"n\":%zu,\"memcpyMs\":%.3f,\"memcpyGbps\":%.2f,\"sort1mMs\":%.2f,\"std1mMs\":%.2f,\"ncpus\":%d,\"xss\":%s,\"xssIsa\":\"%s\",\"ips4o\":%s}", P, N, memcpy_ms, memcpy_gbps, sort1m_ms, std1m_ms, logos_num_cpus(),
#ifdef HAVE_XSS
             "true",
#else
             "false",
#endif
             xss_isa,
#ifdef HAVE_IPS4O
             "true");
#else
             "false");
#endif

    /* ---- HTML ---- */
    static const char *TMPL = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>logos-sort profile</title>
<style>
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;--ring:rgba(11,11,11,.10);
--c-leaf:#2a78d6;--c-scatter:#eb6834;--c-hist:#1baf7a;--c-probe:#eda100;--c-near:#e87ba4;--c-reloc:#008300;--c-serial:#4a3aa7;--c-copy:#e34948;
--l1:#86b6ef;--l2:#5598e7;--l3:#2a78d6;--l4:#1c5cab;--l5:#104281;--l6:#0d366b;--stall:#898781;--preempt:#55544f;--idle:#dcdbd4;--ontext:#fff;--c-radix:#eda100;--c-tim:#e87ba4}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--ring:rgba(255,255,255,.10);
--c-leaf:#3987e5;--c-scatter:#d95926;--c-hist:#199e70;--c-probe:#c98500;--c-near:#d55181;--c-reloc:#008300;--c-serial:#9085e9;--c-copy:#e66767;
--l1:#86b6ef;--l2:#5598e7;--l3:#3987e5;--l4:#256abf;--l5:#1c5cab;--l6:#184f95;--stall:#898781;--preempt:#b3b1a9;--idle:#33332f;--c-radix:#c98500;--c-tim:#d55181}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--ring:rgba(255,255,255,.10);
--c-leaf:#3987e5;--c-scatter:#d95926;--c-hist:#199e70;--c-probe:#c98500;--c-near:#d55181;--c-reloc:#008300;--c-serial:#9085e9;--c-copy:#e66767;
--l1:#86b6ef;--l2:#5598e7;--l3:#3987e5;--l4:#256abf;--l5:#1c5cab;--l6:#184f95;--stall:#898781;--preempt:#b3b1a9;--idle:#33332f;--c-radix:#c98500;--c-tim:#d55181}
*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding-block:0 3rem;padding-inline:clamp(16px,3vw,32px)}
.wrap{max-width:1100px;margin:0 auto}h1{font-size:22px;margin:28px 0 4px}h2{font-size:16px;margin:30px 0 8px}h3{font-size:14px;margin:0;font-weight:600}
.sub{color:var(--ink2);margin:0 0 14px;font-size:13px}.warn{color:var(--ink);background:var(--surface);border-left:3px solid #fab219;padding:8px 12px;margin:10px 0}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0 16px}
button.f{font:inherit;font-size:13px;padding:4px 12px;border:1px solid var(--axis);background:var(--surface);color:var(--ink);border-radius:999px;cursor:pointer}button.f[aria-pressed="true"]{background:var(--ink);color:var(--page);border-color:var(--ink)}
.legend{display:flex;gap:6px 14px;flex-wrap:wrap;font-size:12px;color:var(--ink2);margin:6px 0 4px}.sw{display:inline-block;width:10px;height:10px;border-radius:2px;vertical-align:-1px;margin-right:5px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px;background:var(--surface)}th,td{padding:5px 8px;text-align:right;border-bottom:1px solid var(--grid)}th{color:var(--muted);font-weight:500;font-size:12px}td:first-child,th:first-child{text-align:left}
.tw{overflow-x:auto;border:1px solid var(--grid);border-radius:6px;margin:0 0 14px}
.card{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:14px 16px;margin:0 0 18px}
.head{display:flex;gap:16px;flex-wrap:wrap;align-items:baseline;margin-bottom:8px}.head .k{color:var(--muted);font-size:12px}.head .v{font-size:15px;font-weight:600}
svg{display:block;max-width:100%;height:auto}.tm text{font-size:11px;pointer-events:none;fill:var(--ink);stroke:var(--surface);stroke-width:3px;paint-order:stroke;stroke-linejoin:round}.tm rect{stroke:var(--surface);stroke-width:2px}
.tt{position:fixed;pointer-events:none;background:var(--ink);color:var(--page);font-size:12px;padding:6px 9px;border-radius:4px;max-width:280px;z-index:9;display:none;line-height:1.35}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:820px){.grid2{grid-template-columns:1fr}}
.bars text{font-size:11px;fill:var(--muted)}.bars rect{stroke:var(--surface);stroke-width:2px}
details{margin-top:8px}summary{cursor:pointer;color:var(--ink2);font-size:13px}
</style></head><body><div class="wrap">
<h1>logos-sort — where the time goes</h1>
<p class="sub" id="sub"></p><div id="cal"></div>
<div class="legend" id="legend"></div>
<div class="row" id="filters" role="group" aria-label="thread count"></div>
<h2>Fair fight on the same data</h2>
<p class="sub">Single-threaded sorts against each other, parallel sorts against each other; never one thread against many. Lower is better; the label is milliseconds.</p>
<div class="legend" id="fflegend"></div>
<div class="grid2"><div id="ff1"></div><div id="ffP"></div></div>
<h2>Summary</h2><div class="tw"><table id="sum"><thead><tr><th>shape</th><th>threads</th><th>logos</th><th>std::sort at 1 thread / __gnu_parallel::sort at P</th><th>logos vs it</th><th>x86-simd-sort</th><th>IPS⁴o</th><th>LSD radix (1T)</th><th>timsort (1T)</th><th>route</th><th>working</th><th>preempted</th><th>stalled at barriers</th><th>idle / imbalance</th><th>largest phase</th></tr></thead><tbody></tbody></table></div>
<div id="cards"></div>
<p class="sub">Treemap area is thread-time (threads × wall). "Working" is CPU time the threads actually received inside a phase; "preempted" is wall time inside a phase that the OS gave to another process (a loaded machine shows up here, not as engine cost); "stalled" is time blocked at a barrier waiting for the slowest thread; "idle" is time after a thread ran out of work (or, on serial phases, the other threads). GB/s uses the engine's known bytes per phase (histogram 9n, scatter 17n, copyback 16n, relocation probe 8n, relocation move 32n) against the memcpy calibration; a streaming phase far below that bandwidth is latency-bound. Every output verified against std::sort; best of 3 per cell.</p>
</div><div class="tt" id="tt"></div>
<script>
const DATA=__DATA__,META=__META__;
const CAT={prescan:'probe','lowcard probe':'probe','modcard probe':'probe','near-sorted':'near',relocate:'reloc','relocate probe':'reloc',sample:'serial',histogram:'hist',combine:'serial',scatter:'scatter','leaf sorts':'leaf',copyback:'copy',other:'serial'};
const CNAME={leaf:'leaf sorts (per bucket)',scatter:'scatter',hist:'histogram',probe:'probes (prescan, cardinality)',near:'near-sorted merge',reloc:'run relocation',serial:'serial steps (sample, combine, alloc)',copy:'copyback',stall:'stalled at barrier',preempt:'preempted by the OS',idle:'idle / imbalance'};
const COL={leaf:'var(--c-leaf)',scatter:'var(--c-scatter)',hist:'var(--c-hist)',probe:'var(--c-probe)',near:'var(--c-near)',reloc:'var(--c-reloc)',serial:'var(--c-serial)',copy:'var(--c-copy)',stall:'var(--stall)',preempt:'var(--preempt)',idle:'var(--idle)'};
const LEAFRAMP=['var(--l3)','var(--l4)','var(--l2)','var(--l5)','var(--l1)','var(--l6)'];
const SERIAL=new Set(['prescan','sample','other','copyback']);
const BYTES={histogram:9,scatter:17,copyback:16,relocate:32,'relocate probe':8};
const f1=v=>v>=100?v.toFixed(0):v>=10?v.toFixed(1):v.toFixed(2);const pct=v=>(100*v).toFixed(0)+'%';
const esc=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;');
document.getElementById('sub').textContent=`n = ${(META.n/1e6).toFixed(0)}M doubles · ${META.threads} threads (${META.ncpus} online) · ${DATA.length} cells`;
document.getElementById('cal').innerHTML=`<div class="warn">Calibration (1T): memcpy 64 MB ${META.memcpyMs.toFixed(2)} ms (${META.memcpyGbps.toFixed(1)} GB/s), std::sort 1M ${META.std1mMs.toFixed(1)} ms, logos 1M uniform ${META.sort1mMs.toFixed(1)} ms. Quiet 20-core WSL2 reference ≈ 2.5 ms / ≈ 50 ms / ≈ 30 ms; more than 15% drift means a loaded machine and shares are more trustworthy than absolute times.</div>`;
document.getElementById('legend').innerHTML=Object.keys(CNAME).map(k=>`<span><i class="sw" style="background:${COL[k]}"></i>${CNAME[k]}</span>`).join('');
document.getElementById('fflegend').innerHTML=`<span><i class="sw" style="background:${COL.leaf}"></i>logos</span><span><i class="sw" style="background:${COL.stall}"></i>std::sort at 1 thread / __gnu_parallel::sort at ${META.threads}</span>`+(META.xss?`<span><i class="sw" style="background:${COL.scatter}"></i>x86-simd-sort (${META.xssIsa}), same thread count</span>`:'')+(META.ips4o?`<span><i class="sw" style="background:${COL.hist}"></i>IPS⁴o, same thread count</span>`:'')+`<span><i class="sw" style="background:var(--c-radix)"></i>LSD radix (1 thread)</span><span><i class="sw" style="background:var(--c-tim)"></i>timsort (1 thread)</span>`;
/* grouped horizontal bars: logos emphasised, the opponent in gray as context (one hue + gray) */
function fairfight(cells,label){if(!cells.length)return '';const nser=2+(cells.some(c=>c.xss>0)?1:0)+(cells.some(c=>c.ips4o>0)?1:0)+(cells.some(c=>c.radix>0)?1:0)+(cells.some(c=>c.tim>0)?1:0);const rowh=nser*12+6,W=520,L=100,H=cells.length*rowh+24;
 const mx=Math.max(...cells.flatMap(c=>[c.total,c.base,c.xss||0,c.ips4o||0,c.radix||0,c.tim||0]));const sc=(W-L-64)/mx;
 let g=`<svg class="bars" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" role="img" aria-label="${label}">`;
 cells.forEach((c,i)=>{const y=i*rowh+4;const rows=[['logos',c.total,COL.leaf],[c.baseName,c.base,COL.stall]];if(c.xss>0)rows.push(['x86-simd-sort '+META.xssIsa,c.xss,COL.scatter]);if(c.ips4o>0)rows.push(['IPS4o',c.ips4o,COL.hist]);if(c.radix>0)rows.push(['LSD radix',c.radix,'var(--c-radix)']);if(c.tim>0)rows.push(['timsort',c.tim,'var(--c-tim)']);
  g+=`<text x="${L-8}" y="${y+rows.length*6+4}" text-anchor="end" style="fill:var(--ink)">${esc(c.shape)}</text>`;
  rows.forEach((r,k)=>{const w=Math.max(1,r[1]*sc),yy=y+k*12;g+=`<rect x="${L}" y="${yy}" width="${w}" height="10" rx="2" fill="${r[2]}" data-t="${esc(c.shape)}, ${esc(r[0])}: ${f1(r[1])} ms"/><text x="${L+w+5}" y="${yy+9}">${f1(r[1])}</text>`;});});
 g+=`<text x="${L}" y="${H-4}">${label}</text></svg>`;return g;}
document.getElementById('ff1').innerHTML=fairfight(DATA.filter(c=>c.threads===1),'1 thread, ms');
document.getElementById('ffP').innerHTML=fairfight(DATA.filter(c=>c.threads>1),`${META.threads} threads, ms`);
/* per-cell accounting: thread-time by category, leaf split by path */
/* working = CPU time the threads actually received inside work sections; preempted = wall time in those sections the OS gave to someone else */
function account(c){const T=c.threads,total=c.total,tt=T*total;let busy=0,cpu=0,wait=0;const cat={};
 for(const ph of c.phases){const k=CAT[ph.name];let b=ph.busy.reduce((a,v)=>a+v,0),w=ph.wait.reduce((a,v)=>a+v,0),u=ph.cpu.reduce((a,v)=>a+v,0);
  if(SERIAL.has(ph.name)){b+=ph.wall;u+=ph.wall;} /* the caller does serial phases alone; assume it kept the CPU */
  busy+=b;cpu+=u;wait+=w;cat[k]=(cat[k]||0)+u;}
 const preempt=Math.max(0,busy-cpu),idle=Math.max(0,tt-busy-wait);return{tt,busy,cpu,preempt,wait,idle,cat};}
/* squarified treemap */
function squarify(items,x,y,w,h){const out=[];const tot=items.reduce((a,i)=>a+i.v,0);if(tot<=0||w<=0||h<=0)return out;
 let list=items.filter(i=>i.v>0).map(i=>({...i,a:i.v/tot*w*h})).sort((a,b)=>b.a-a.a);
 let cx=x,cy=y,cw=w,ch=h;
 while(list.length){let row=[],best=Infinity;const vert=cw>=ch;const side=vert?ch:cw;
  for(let i=0;i<list.length;i++){row.push(list[i]);const s=row.reduce((a,r)=>a+r.a,0),mn=Math.min(...row.map(r=>r.a)),mx=Math.max(...row.map(r=>r.a));
   const worst=Math.max(side*side*mx/(s*s),s*s/(side*side*mn));if(worst<=best)best=worst;else{row.pop();break;}}
  const s=row.reduce((a,r)=>a+r.a,0),thick=s/side;
  let off=0;for(const r of row){const len=r.a/thick;if(vert)out.push({...r,x:cx,y:cy+off,w:thick,h:len});else out.push({...r,x:cx+off,y:cy,w:len,h:thick});off+=len;}
  if(vert){cx+=thick;cw-=thick;}else{cy+=thick;ch-=thick;}list=list.slice(row.length);}
 return out;}
function treemap(c,acc){const W=520,H=300;const items=[];
 for(const k in acc.cat)if(acc.cat[k]>0)items.push({k,v:acc.cat[k],name:CNAME[k],fill:COL[k]});
 if(acc.preempt>0)items.push({k:'preempt',v:acc.preempt,name:CNAME.preempt,fill:COL.preempt});
 if(acc.wait>0)items.push({k:'stall',v:acc.wait,name:CNAME.stall,fill:COL.stall});if(acc.idle>0)items.push({k:'idle',v:acc.idle,name:CNAME.idle,fill:COL.idle});
 let g=`<svg class="tm" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" role="img" aria-label="treemap of thread-time">`;
 for(const r of squarify(items,0,0,W,H)){const share=r.v/acc.tt;
  if(r.k==='leaf'&&c.paths.length){const pathTot=c.paths.reduce((a,p)=>a+p.ms,0)||1;const kids=c.paths.map((p,i)=>({k:'path',v:p.ms/pathTot*r.v,name:p.name,fill:LEAFRAMP[i%LEAFRAMP.length],calls:p.calls,elems:p.elems}));
   const inner=squarify(kids,r.x+3,r.y+3,Math.max(0,r.w-6),Math.max(0,r.h-6));
   g+=`<rect x="${r.x}" y="${r.y}" width="${r.w}" height="${r.h}" fill="${r.fill}" data-t="${esc(r.name)}: ${f1(r.v)} thread-ms (${pct(share)})"/>`;
   for(const q of inner){const qs=q.v/acc.tt;g+=`<rect x="${q.x}" y="${q.y}" width="${q.w}" height="${q.h}" fill="${q.fill}" data-t="${esc(q.name)}: ${f1(q.v)} thread-ms (${pct(qs)}), ${q.calls} calls, ${(q.elems/1e6).toFixed(2)}M elements"/>`;
    if(q.w>q.name.length*6.5+8&&q.h>16)g+=`<text x="${q.x+5}" y="${q.y+13}">${esc(q.name)} ${pct(qs)}</text>`;}
   if(r.w>60&&r.h>30)g+=`<text x="${r.x+5}" y="${r.y+r.h-6}" font-weight="600">leaf sorts ${pct(share)}</text>`;}
  else{const dark=r.k!=='idle';g+=`<rect x="${r.x}" y="${r.y}" width="${r.w}" height="${r.h}" fill="${r.fill}" data-t="${esc(r.name)}: ${f1(r.v)} thread-ms (${pct(share)})"/>`;
   const lab=`${r.name.split(' (')[0]} ${pct(share)}`;if(r.w>lab.length*6.5+8&&r.h>16)g+=`<text x="${r.x+5}" y="${r.y+13}">${esc(lab)}</text>`;}}
 return g+'</svg>';}
function threadbars(c){const T=c.threads;if(T<2)return '';const W=520,L=40,rowh=12,H=T*rowh+22;const sc=(W-L-8)/c.total;
 const order=['prescan','lowcard probe','modcard probe','near-sorted','relocate probe','relocate','sample','histogram','combine','scatter','leaf sorts','copyback','other'];
 let g=`<svg class="bars" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" role="img" aria-label="per-thread time">`;
 for(let t=0;t<T;t++){let x=L,y=t*rowh;const segs=[];
  for(const nm of order){const ph=c.phases.find(p=>p.name===nm);if(!ph)continue;let b=ph.busy[t]||0,u=ph.cpu[t]||0;if(SERIAL.has(nm)&&t===0){b+=ph.wall;u+=ph.wall;}const w=ph.wait[t]||0,pe=Math.max(0,b-u);
   if(u>0)segs.push({v:u,fill:COL[CAT[nm]],t:`${nm} ${f1(u)} ms working`});if(pe>0.02)segs.push({v:pe,fill:COL.preempt,t:`${nm} ${f1(pe)} ms preempted`});if(w>0)segs.push({v:w,fill:COL.stall,t:`stalled after ${nm} ${f1(w)} ms`});}
  const used=segs.reduce((a,s)=>a+s.v,0);if(c.total-used>0.05)segs.push({v:c.total-used,fill:COL.idle,t:`idle ${f1(c.total-used)} ms`});
  g+=`<text x="${L-6}" y="${y+10}" text-anchor="end">t${t}</text>`;
  for(const s of segs){const w=s.v*sc;g+=`<rect x="${x}" y="${y+1}" width="${w}" height="${rowh-2}" fill="${s.fill}" data-t="thread ${t}: ${esc(s.t)}"/>`;x+=w;}}
 g+=`<text x="${L}" y="${H-4}">0</text><text x="${W-8}" y="${H-4}" text-anchor="end">${f1(c.total)} ms wall</text></svg>`;return g;}
function phasetable(c){const rows=c.phases.filter(p=>p.wall>0.005).map(p=>{const ser=SERIAL.has(p.name)?p.wall:0,b=p.busy.reduce((a,v)=>a+v,0)+ser,u=p.cpu.reduce((a,v)=>a+v,0)+ser,w=p.wait.reduce((a,v)=>a+v,0);
 const util=p.wall>0?u/(c.threads*p.wall):0;const by=BYTES[p.name];const gb=by?(by*c.n)/(p.wall*1e6):0;
 return `<tr><td>${p.name}</td><td>${f1(p.wall)}</td><td>${pct(p.wall/c.total)}</td><td>${pct(util)}</td><td>${b-u>0.02?f1(b-u):'–'}</td><td>${w>0.005?f1(w):'–'}</td><td>${gb?f1(gb)+' GB/s ('+pct(gb/META.memcpyGbps)+')':'–'}</td></tr>`;}).join('');
 return `<details><summary>phase table</summary><div class="tw"><table><thead><tr><th>phase</th><th>wall ms</th><th>share</th><th>CPU utilisation</th><th>preempted ms</th><th>stalled ms</th><th>streaming rate vs memcpy</th></tr></thead><tbody>${rows}</tbody></table></div></details>`;}
function render(filter){const cards=document.getElementById('cards');const sum=document.querySelector('#sum tbody');let ch='',sh='';
 for(const c of DATA){if(filter!=='all'&&String(c.threads)!==filter)continue;const acc=account(c);
  const big=c.phases.reduce((a,p)=>p.wall>a.wall?p:a,c.phases[0]);
  const sp=c.base/c.total;
  const vs=(v)=>v>0?`${f1(v)} ms (${(v/c.total).toFixed(2)}×)`:'–';
  sh+=`<tr><td>${c.shape}${c.ok?'':' ✗'}</td><td>${c.threads}</td><td>${f1(c.total)} ms</td><td>${f1(c.base)} ms</td><td>${sp.toFixed(2)}×</td><td>${vs(c.xss)}</td><td>${vs(c.ips4o)}</td><td>${vs(c.radix)}</td><td>${vs(c.tim)}</td><td>${c.route}</td><td>${pct(acc.cpu/acc.tt)}</td><td>${pct(acc.preempt/acc.tt)}</td><td>${pct(acc.wait/acc.tt)}</td><td>${pct(acc.idle/acc.tt)}</td><td>${big.name} ${pct(big.wall/c.total)}</td></tr>`;
  ch+=`<div class="card"><div class="head"><h3>${c.shape} · ${c.threads} thread${c.threads>1?'s':''}</h3><span><span class="k">total </span><span class="v">${f1(c.total)} ms</span></span><span><span class="k">vs ${esc(c.baseName)} </span><span class="v">${f1(c.base)} ms (${sp.toFixed(2)}×)</span></span>${c.xss>0?`<span><span class="k">vs x86-simd-sort </span><span class="v">${vs(c.xss)}</span></span>`:''}${c.ips4o>0?`<span><span class="k">vs IPS⁴o </span><span class="v">${vs(c.ips4o)}</span></span>`:''}${c.radix>0?`<span><span class="k">vs LSD radix </span><span class="v">${vs(c.radix)}</span></span>`:''}${c.tim>0?`<span><span class="k">vs timsort </span><span class="v">${vs(c.tim)}</span></span>`:''}<span><span class="k">route </span><span class="v">${c.route}</span></span><span><span class="k">working </span><span class="v">${pct(acc.cpu/acc.tt)}</span></span><span><span class="k">preempted </span><span class="v">${pct(acc.preempt/acc.tt)}</span></span><span><span class="k">stalled </span><span class="v">${pct(acc.wait/acc.tt)}</span></span><span><span class="k">idle </span><span class="v">${pct(acc.idle/acc.tt)}</span></span>${c.ok?'':'<span class="v">OUTPUT MISMATCH</span>'}</div>
   <div class="grid2"><div>${treemap(c,acc)}</div><div>${threadbars(c)||'<p class="sub">single thread: no per-thread bars</p>'}</div></div>${phasetable(c)}</div>`;}
 cards.innerHTML=ch;sum.innerHTML=sh;}
const fl=document.getElementById('filters');const opts=[['all','all'],['1','1 thread'],[String(META.threads),META.threads+' threads']];
opts.forEach(([v,l])=>{const b=document.createElement('button');b.className='f';b.textContent=l;b.dataset.v=v;b.setAttribute('aria-pressed',v==='all');b.onclick=()=>{fl.querySelectorAll('button').forEach(x=>x.setAttribute('aria-pressed',x===b));render(v);};fl.appendChild(b);});
render('all');
const tt=document.getElementById('tt');document.addEventListener('mousemove',e=>{const r=e.target.closest&&e.target.closest('[data-t]');if(r){tt.style.display='block';tt.textContent=r.dataset.t;tt.style.left=(e.clientX+12)+'px';tt.style.top=(e.clientY+12)+'px';}else tt.style.display='none';});
</script></body></html>
)HTML";
    std::string html(TMPL);
    size_t p = html.find("__DATA__"); if (p != std::string::npos) html.replace(p, 8, j);
    p = html.find("__META__"); if (p != std::string::npos) html.replace(p, 8, std::string(meta));
    FILE *f = fopen("profile-report.html", "w");
    if (f) { fwrite(html.data(), 1, html.size(), f); fclose(f); printf("\nwrote profile-report.html\n"); }
    printf("RESULT: %s\n", bad ? "OUTPUT MISMATCHES — do not trust the profile" : "all outputs verified against std::sort");
    return bad ? 1 : 0;
}
