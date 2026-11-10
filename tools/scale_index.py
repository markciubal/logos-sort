#!/usr/bin/env python3
"""scale_index.py -- the clean page: reports/scale-report.html -> index.html.

The scale report (`make scale-full`) is the full record: every size from an empty array up, the seam between its two
measuring modes, memory circles, zoom and pan, the shapes' demo data. This renders the SAME measurements (its DATA, META
and SHAPES blocks, nothing re-measured) as one short, static, self-contained page for a reader who wants the result:
what was raced, who won at the largest size, how each sort's total time and cost per element grow with n, what changed in
the engine lately (NOTES below: edit it when the engine changes), and how to reproduce it.

    python tools/scale_index.py [reports/scale-report.html] [index.html]

Rules kept from the report: one thread is compared with one thread and P with P, never one against many; differences
under 5% are called level; every output was checked against std::sort's before its time counted. Charts show the
single-array regime only (n >= 10,000: one array sorted repeatedly, the minimum kept); below that the report times
batches of distinct inputs, a different method with a seam the report explains and this page does not need.
No CDN, no sibling files, no fonts fetched: the page is one file, like every report here.
"""
import json, math, os, sys, html, datetime

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "reports", "scale-report.html")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "index.html")

src = open(SRC, "rb").read().decode("utf-8").replace("\r", "")
def block(tag):
    i = src.index(tag) + len(tag)
    v, _ = json.JSONDecoder().raw_decode(src, i)
    return v
DATA, META, SHAPES = block("const DATA="), block("META="), block("const SHAPES=")
P = META["threads"]
XSS_ISA, VQ_ISA = META.get("xssIsa", ""), META.get("vqIsa", "")
NAMES1 = ["std::sort", "logos", ("x86-simd-sort " + XSS_ISA).strip(), "IPS⁴o", "LSD radix", "timsort", "logos in-place", ("VQSort (Google) " + VQ_ISA).strip()]
NAMESP = ["__gnu_parallel::sort", "logos", ("x86-simd-sort " + XSS_ISA).strip(), "IPS⁴o", "", "", "logos in-place", ""]
CLS = ["s-base", "s-logos", "s-xss", "s-ips", "s-radix", "s-tim", "s-inpl", "s-vq"]
ORDER = [1, 2, 7, 3, 4, 5, 6, 0]           # the palette's slot order: adjacent colours are the validated pairs
LOGOS, LEVEL, CHART_MIN_N = 1, 0.05, 10000
MAX_N = max(r["n"] for s in DATA for r in s["rows"])
STR_MAX_N = META.get("strMaxN", META.get("maxN", MAX_N))
MEM_MAX_N = META.get("memMaxN", META.get("maxN", MAX_N))
DOCS = {e["shape"]: e for e in SHAPES}

def esc(s): return html.escape(str(s), quote=True)
def names_of(s): return s.get("names") or (NAMES1 if s["threads"] == 1 else NAMESP)
def short(nm):
    import re
    nm = re.sub(r" \(.*$", "", nm or ""); nm = re.sub(r" key-value.*$", "", nm)
    return re.sub(r" (AVX2|AVX-512|AVX3\w*|AVX10\w*|SSE\w*|NEON|SVE\w*)$", "", nm)
def fmt_n(n):
    if n >= 1e9: return ("%.2f" % (n / 1e9)).rstrip("0").rstrip(".") + "B"
    if n >= 1e6: return ("%.2f" % (n / 1e6)).rstrip("0").rstrip(".") + "M"
    if n >= 1e3: return ("%.1f" % (n / 1e3)).rstrip("0").rstrip(".") + "k"
    return str(int(n))
def fmt_ms(v):
    if v is None or v <= 0: return "–"
    if v >= 1000: return "%.1f s" % (v / 1000)
    if v >= 10: return "%.0f ms" % v
    if v >= 1: return "%.1f ms" % v
    if v >= 1e-3: return ("%.0f" if v >= 1e-2 else "%.1f") % (v * 1e3) + " µs"
    return "%.1f ns" % (v * 1e6)
def fmt_ns(v):
    if v >= 1e6: return "%.1f ms" % (v / 1e6)
    if v >= 1e3: return "%.1f µs" % (v / 1e3)
    return ("%.0f" if v >= 100 else "%.1f" if v >= 10 else "%.2f") % v + " ns"
def fmt_x(f): return ("%.1f" if f >= 10 else "%.2f") % f + "×"
def commas(n): return "{:,}".format(int(n))
def mem_ok(v): return v is not None and 0 <= v < 1e4     # nothing sorts at 10 KB per element: above that the pass misread (urls on the key-value sort, reports/README.md)
def mem_cell(v, measured):
    if not measured or not mem_ok(v): return "–"
    return ("%.1f" % v) if v >= 10 else ("%.2f" % v)
def mem_text(v, n):
    if not v or v <= 0: return "0"
    b = v * n
    tot = "%.2f GB" % (b / 2**30) if b >= 2**30 else "%.1f MB" % (b / 2**20) if b >= 2**20 else "%.0f KB" % (b / 1024)
    return ("%.1f" % v if v >= 10 else "%.2f" % v) + " B/elem (" + tot + ")"

# ---- at the largest size: logos against the fastest other sort, per input, per thread count -------------------------
def glance(which):
    out = []
    for i in range(0, len(DATA), 2):
        s = DATA[i + which]; names = names_of(s)
        rs = [r for r in s["rows"] if r["n"] > 0 and r["t"][LOGOS] > 0]
        if not rs: continue
        r = rs[-1]; bk = -1
        for k, v in enumerate(r["t"]):
            if k == LOGOS or not v > 0 or "logos" in (names[k] or ""): continue
            if bk < 0 or v < r["t"][bk]: bk = k
        if bk < 0: continue
        # memory: the largest row at which the pass recorded both (the pass may stop below the ladder's top)
        mrow = next((q for q in reversed(s["rows"]) if q["n"] >= CHART_MIN_N and q["m"][LOGOS] >= 0 and any(m > 0 for m in q["m"])), None)
        out.append(dict(shape=s["shape"], threads=s["threads"], n=r["n"], lg=r["t"][LOGOS], ot=r["t"][bk], who=short(names[bk]),
                        x=r["t"][bk] / r["t"][LOGOS], ok=r["ok"], mlg=mrow["m"][LOGOS] if mrow else 0, mot=mrow["m"][bk] if mrow else 0, mn=mrow["n"] if mrow else 0))
    return out
G1, GP = glance(0), glance(1)
def verdict(g): return "faster" if g["x"] > 1 + LEVEL else "slower" if g["x"] < 1 / (1 + LEVEL) else "level"
def counts(gs):
    c = {"faster": 0, "level": 0, "slower": 0}
    for g in gs: c[verdict(g)] += 1
    return c
TK = [0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500]
ALLG = G1 + GP
lo = min([1] + [g["x"] for g in ALLG]) / 1.08; hi = max([1] + [g["x"] for g in ALLG]) * 1.08
lo = ([t for t in TK if t <= lo] or [TK[0]])[-1]; hi = next((t for t in TK if t >= hi), TK[-1])
def X(v): return max(0, min(100, (math.log(v) - math.log(lo)) / (math.log(hi) - math.log(lo)) * 100))
def glance_rows(gs):
    rows = []
    for g in gs:
        v = verdict(g); a, b = X(1), X(g["x"]); l = min(a, b); w = max(abs(b - a), 0.6)
        val = ("<b>%s</b> faster" % fmt_x(g["x"])) if v == "faster" else ("<b>%s</b> slower" % fmt_x(1 / g["x"])) if v == "slower" else "<b>level</b> (%.2f)" % g["x"]
        tip = "%s, %s elements, %s: logos %s, %s %s%s" % (g["shape"], commas(g["n"]), "one thread" if g["threads"] == 1 else "%d threads" % g["threads"], fmt_ms(g["lg"]), g["who"], fmt_ms(g["ot"]), "" if g["ok"] else " (an output differed from std::sort’s)")
        rows.append('<div class="glr" title="%s"><span class="nm">%s</span><span class="trk"><i class="mid" style="left:%.2f%%"></i><i class="b %s" style="left:%.2f%%;width:%.2f%%"></i></span><span class="v">%s<small>%s vs %s%s<br>%s</small></span></div>'
                    % (esc(tip), esc(g["shape"]), a, {"faster": "pos", "slower": "neg", "level": "eq"}[v], l, w, val, fmt_ms(g["lg"]), fmt_ms(g["ot"]), "" if g["ok"] else " ✗", esc(g["who"])))
    ticks = [t for t in TK if lo <= t <= hi]
    axis = '<div class="glr ax" aria-hidden="true"><span></span><span class="trk">' + "".join(
        '<i class="tk%s" style="left:%.2f%%">%s×</i>' % (" l" if i == 0 else " r" if i == len(ticks) - 1 else "", X(t), (t if t >= 1 else int(round(1 / t)))) for i, t in enumerate(ticks)) + \
        '<i class="tk cap l" style="left:0%">← other faster</i><i class="tk cap r" style="left:100%">logos faster →</i></span><span></span></div>'
    return "".join(rows) + axis
def glance_table(gs):
    return "".join("<tr><td>%s</td><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s%s</td><td>%s</td><td>%s</td></tr>" % (
        esc(g["shape"]), g["threads"], fmt_n(g["n"]), fmt_ms(g["lg"]), esc(g["who"]), fmt_ms(g["ot"]), "%.2f×" % g["x"], "" if g["ok"] else " ✗",
        mem_cell(g["mlg"], g["mn"]), mem_cell(g["mot"], g["mn"])) for g in gs)
c1, cp = counts(G1), counts(GP)
nmax_g = max(g["n"] for g in ALLG); same_n = all(g["n"] == nmax_g for g in ALLG)
def part(c, n, t): return "<b>%d of %d</b> inputs %s%s" % (c["faster"], n, t, (" (level on %d, slower on %d)" % (c["level"], c["slower"])) if c["level"] or c["slower"] else "")
score = "At %s<b>%s elements</b>, logos is the fastest sort measured on %s." % ("" if same_n else "up to ", commas(nmax_g),
        " and on ".join(x for x in [part(c1, len(G1), "at one thread") if G1 else "", part(cp, len(GP), "at %d threads" % P) if GP else ""] if x))

# ---- the charts: one pair of SVGs per panel, drawn here (no library): nanoseconds per element against n (linear), and the
# TOTAL time of one sort against n (log: it spans six decades). The page shows one of the two; a switch picks which.
W, H, ML, MR, MT, MB = 560, 280, 50, 66, 12, 30
# ---- growth guides: what logos's FIRST plotted point would become if its time grew as each complexity class from there.
# Dashed, behind the data, labelled in the right margin (or where a guide leaves the top). `per_el` divides by n, so in
# that view O(n) is the flat line and O(n log n) the gently rising one. `pyr` is the chart's y map WITHOUT clamping.
GUIDES = [("O(1)", lambda r, L: 1.0), ("O(log n)", lambda r, L: L), ("O(n)", lambda r, L: r),
          ("O(n log n)", lambda r, L: r * L), ("O(n²)", lambda r, L: r * r)]
def guides(ns, vals, px, pyr, per_el):
    i0 = next((i for i, v in enumerate(vals) if v is not None), None)
    if i0 is None or i0 == len(ns) - 1: return ""
    n0, v0 = ns[i0], vals[i0]
    xr, top, bot = W - MR, MT, H - MB
    out, ends = [], []
    for name, f in GUIDES:
        pts, exit_x = [], None
        for j in range(25):
            n = n0 * (ns[-1] / n0) ** (j / 24.0); r = n / n0
            v = v0 * f(r, math.log(n) / math.log(n0)) / (r if per_el else 1.0)
            x, y = px(n), pyr(v)
            if y < top:
                if pts:
                    (xa, ya) = pts[-1]; t = (ya - top) / (ya - y) if ya != y else 0
                    exit_x = xa + (x - xa) * t; pts.append((exit_x, top))
                break
            pts.append((x, min(y, bot)))
        if len(pts) < 2: continue
        out.append('<polyline points="%s"/>' % " ".join("%.1f,%.1f" % p for p in pts))
        if exit_x is not None and exit_x < xr - 4:
            out.append('<text x="%.1f" y="%d" text-anchor="%s">%s</text>' % (((exit_x + 5, top + 20, "start") if exit_x < xr - 60 else (exit_x - 5, top + 20, "end")) + (name,)))
        else: ends.append([pts[-1][1] + 4, name])
    ends.sort()
    for i in range(1, len(ends)): ends[i][0] = max(ends[i][0], ends[i - 1][0] + 12)
    if ends and ends[-1][0] > bot:
        ends[-1][0] = bot
        for i in range(len(ends) - 2, -1, -1): ends[i][0] = min(ends[i][0], ends[i + 1][0] - 12)
    for y, name in ends: out.append('<text x="%d" y="%.1f">%s</text>' % (xr + 5, y, name))
    return '<g class="gd" aria-hidden="true">%s</g>' % "".join(out)
def tick_ms(v):
    if v >= 1000: return "%g s" % (v / 1000)
    if v >= 1: return "%g ms" % v
    if v >= 1e-3: return "%g µs" % round(v * 1e3, 6)
    return "%g ns" % round(v * 1e6, 6)
def chart_total(s, cid):
    names = names_of(s)
    rows = [r for r in s["rows"] if r["n"] >= CHART_MIN_N]
    if not rows: return ""
    ns = [r["n"] for r in rows]
    tot = {k: [r["t"][k] if r["t"][k] > 0 else None for r in rows] for k in ORDER if any(r["t"][k] > 0 for r in rows)}
    if LOGOS not in tot: return ""
    allv = [v for vs in tot.values() for v in vs if v is not None]
    l0, l1 = math.floor(math.log10(min(allv)) + 1e-9), math.ceil(math.log10(max(allv)) - 1e-9)
    if l1 <= l0: l1 = l0 + 1
    ml = 58
    x0, x1 = math.log10(ns[0]), math.log10(ns[-1])
    def px(n): return ml + (math.log10(n) - x0) / (x1 - x0 or 1) * (W - ml - MR)
    def py(v): return MT + (1 - (math.log10(v) - l0) / (l1 - l0)) * (H - MT - MB)
    g = []
    for d in range(l0, l1 + 1):
        v = 10.0 ** d
        g.append('<line class="grid" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/><text class="tk" x="%d" y="%.1f" text-anchor="end" dominant-baseline="middle">%s</text>' % (ml, W - MR, py(v), py(v), ml - 6, py(v), tick_ms(v)))
    d = math.ceil(x0)
    while d <= x1 + 1e-9:
        n = 10 ** d
        g.append('<line class="grid v" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/><text class="tk" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (px(n), px(n), MT, H - MB, px(n), H - 10, fmt_n(n)))
        d += 1
    g.append(guides(ns, tot[LOGOS], px, py, False))
    for k in ORDER:
        if k not in tot: continue
        segs, cur = [], []
        for n, v in zip(ns, tot[k]):
            if v is None:
                if cur: segs.append(cur); cur = []
            else: cur.append((px(n), py(v)))
        if cur: segs.append(cur)
        for seg in segs:
            if len(seg) == 1: g.append('<circle class="pt %s" cx="%.1f" cy="%.1f" r="3"/>' % (CLS[k], seg[0][0], seg[0][1]))
            else: g.append('<polyline class="ln %s%s" points="%s"/>' % (CLS[k], " lg" if k == LOGOS else "", " ".join("%.1f,%.1f" % p for p in seg)))
    li = max(i for i, v in enumerate(tot[LOGOS]) if v is not None)
    lx, ly = px(ns[li]), py(tot[LOGOS][li])
    g.append('<text class="lbl s-logos" x="%.1f" y="%.1f" text-anchor="end">logos %s</text>' % (lx - 4, ly + 16 if ly < H - MB - 22 else ly - 8, fmt_ms(tot[LOGOS][li])))
    g.append('<line class="cur" x1="-10" x2="-10" y1="%d" y2="%d"/>' % (MT, H - MB))
    series_json = {str(k): [None if v is None else round(v * 1e6 / n, 3) for n, v in zip(ns, tot[k])] for k in tot}
    name_json = {str(k): short(names[k]) for k in tot}
    return ('<svg class="ch m-tot" id="%s" viewBox="0 0 %d %d" role="img" aria-label="%s, %s: total time of one sort against n" '
            'data-n="%s" data-s="%s" data-nm="%s" data-ml="%d" data-mr="%d">%s</svg>') % (
        cid, W, H, esc(s["shape"]), "one thread" if s["threads"] == 1 else "%d threads" % s["threads"], esc(json.dumps(ns)), esc(json.dumps(series_json)), esc(json.dumps(name_json)), ml, MR, "".join(g))
def nice_step(cap):
    raw = cap / 4.5; mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if m * mag >= raw: return m * mag
    return 10 * mag
def chart(s, cid):
    names = names_of(s)
    rows = [r for r in s["rows"] if r["n"] >= CHART_MIN_N]
    if not rows: return "", []
    ns = [r["n"] for r in rows]
    ser = {k: [(r["t"][k] * 1e6 / r["n"]) if r["t"][k] > 0 else None for r in rows] for k in ORDER if any(r["t"][k] > 0 for r in rows)}
    if LOGOS not in ser: return "", []
    last = {k: next((v for v in reversed(vs) if v is not None), None) for k, vs in ser.items()}
    rivals = sorted(((v, k) for k, v in last.items() if k != LOGOS and v is not None))
    cap = max(4 * max(v for v in ser[LOGOS] if v is not None), 1.15 * (rivals[min(2, len(rivals) - 1)][0] if rivals else 0))
    cap = min(cap, max(v for vs in ser.values() for v in vs if v is not None))
    step = nice_step(cap); cap = math.ceil(cap / step) * step
    x0, x1 = math.log10(ns[0]), math.log10(ns[-1])
    def px(n): return ML + (math.log10(n) - x0) / (x1 - x0 or 1) * (W - ML - MR)
    def py(v): return MT + (1 - min(v, cap) / cap) * (H - MT - MB)
    g = []
    yt = 0.0
    while yt <= cap + 1e-9:
        g.append('<line class="grid" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/><text class="tk" x="%d" y="%.1f" text-anchor="end" dominant-baseline="middle">%s</text>' % (ML, W - MR, py(yt), py(yt), ML - 6, py(yt), ("%g" % yt)))
        yt += step
    d = math.ceil(x0)
    while d <= x1 + 1e-9:
        n = 10 ** d
        g.append('<line class="grid v" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/><text class="tk" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (px(n), px(n), MT, H - MB, px(n), H - 10, fmt_n(n)))
        d += 1
    g.append(guides(ns, ser[LOGOS], px, lambda v: MT + (1 - v / cap) * (H - MT - MB), True))
    off = []
    for k in ORDER:
        if k not in ser: continue
        # a point above the cap is a triangle at the top, never a clamped point: a series that lies wholly above the
        # axis must vanish from the plot rather than run along its top edge as if it measured exactly the cap
        segs, cur = [], []
        for n, v in zip(ns, ser[k]):
            if v is None or v > cap:
                if cur: segs.append(cur); cur = []
                if v is not None: g.append('<path class="clip %s" d="M%.1f %.1fl-3.5 6h7z"/>' % (CLS[k], px(n), MT + 1))
            else: cur.append((px(n), py(v)))
        if cur: segs.append(cur)
        for seg in segs:
            if len(seg) == 1: g.append('<circle class="pt %s" cx="%.1f" cy="%.1f" r="3"/>' % (CLS[k], seg[0][0], seg[0][1]))
            else: g.append('<polyline class="ln %s%s" points="%s"/>' % (CLS[k], " lg" if k == LOGOS else "", " ".join("%.1f,%.1f" % p for p in seg)))
        if last[k] is not None and last[k] > cap: off.append((last[k], short(names[k])))
    lx, ly = px(ns[-1]), py(last[LOGOS])
    ltot = next((r["t"][LOGOS] for r in reversed(rows) if r["t"][LOGOS] > 0), None)
    g.append('<text class="lbl s-logos" x="%.1f" y="%.1f" text-anchor="end">logos %s · %s in all</text>' % (lx - 2, ly - 6 if ly > MT + 14 else ly + 14, fmt_ns(last[LOGOS]), fmt_ms(ltot)))
    g.append('<line class="cur" x1="-10" x2="-10" y1="%d" y2="%d"/>' % (MT, H - MB))
    off.sort(reverse=True)
    series_json = {str(k): [None if v is None else round(v, 3) for v in ser[k]] for k in ser}
    name_json = {str(k): short(names[k]) for k in ser}
    svg = ('<svg class="ch m-ns" id="%s" viewBox="0 0 %d %d" role="img" aria-label="%s, %s: nanoseconds per element against n" '
           'data-n="%s" data-s="%s" data-nm="%s" data-ml="%d" data-mr="%d">%s</svg>') % (
        cid, W, H, esc(s["shape"]), "one thread" if s["threads"] == 1 else "%d threads" % s["threads"], esc(json.dumps(ns)), esc(json.dumps(series_json)), esc(json.dumps(name_json)), ML, MR, "".join(g))
    return svg, off

def panel_table(s):
    names = names_of(s); rows = [r for r in s["rows"] if r["n"] >= CHART_MIN_N]
    cols = [k for k in ORDER if any(r["t"][k] > 0 for r in rows)]
    head = "<tr><th>n</th>" + "".join("<th><i class=\"sw %s\"></i>%s</th>" % (CLS[k], esc(short(names[k]))) for k in cols) + "<th>best other ÷ logos</th></tr>"
    body = []
    for r in rows:
        others = [r["t"][k] for k in cols if k != LOGOS and r["t"][k] > 0 and "logos" not in (names[k] or "")]
        best = min(others) if others else None
        body.append("<tr><td>%s</td>%s<td>%s%s</td></tr>" % (fmt_n(r["n"]), "".join("<td>%s</td>" % fmt_ms(r["t"][k]) for k in cols),
                    ("%.2f×" % (best / r["t"][LOGOS])) if best and r["t"][LOGOS] > 0 else "–", "" if r["ok"] else " ✗"))
    mrow = next((q for q in reversed(rows) if any(m > 0 for m in q["m"])), None)
    if mrow:
        body.append("<tr class=\"mem\"><td>peak heap at %s</td>%s<td></td></tr>" % (fmt_n(mrow["n"]), "".join("<td>%s</td>" % ((mem_cell(mrow["m"][k], True) + " B/el") if mrow["t"][k] > 0 and mem_ok(mrow["m"][k]) else "–") for k in cols)))
    return '<div class="tw"><table>%s%s</table></div>' % (head, "".join(body))

sections = []
for i in range(0, len(DATA), 2):
    s1, sp = DATA[i], DATA[i + 1]; shape = s1["shape"]; doc = DOCS.get(shape, {})
    sv1, off1 = chart(s1, "c%d" % i); svp, offp = chart(sp, "c%d" % (i + 1))
    if not sv1 and not svp: continue
    if sv1: sv1 += chart_total(s1, "t%d" % i)
    if svp: svp += chart_total(sp, "t%d" % (i + 1))
    def offtxt(off): return ('<p class="off m-ns">off the top at the largest n: ' + ", ".join("%s %s" % (esc(nm), fmt_ns(v)) for v, nm in off) + "</p>") if off else ""
    sid = "in-" + shape.replace(" ", "-").replace(":", "")
    sections.append('''<section class="input" id="%s">
<h3>%s <span class="brief">%s</span></h3>
<div class="pair">
<figure><figcaption>one thread each <span class="m-ns">&middot; ns per element</span><span class="m-tot">&middot; total time of one sort</span></figcaption>%s%s</figure>
<figure><figcaption>%d threads each <span class="m-ns">&middot; ns per element</span><span class="m-tot">&middot; total time of one sort</span></figcaption>%s%s</figure>
</div>
<details><summary>what the input is, and the total time of every sort at every size</summary><p class="desc">%s</p>%s%s</details>
</section>''' % (sid, esc(shape), esc(doc.get("brief", "")), sv1, offtxt(off1), P, svp, offtxt(offp), esc(doc.get("desc", "")),
                 '<h4>one thread each</h4>' + panel_table(s1) if sv1 else "", '<h4>%d threads each</h4>' % P + panel_table(sp) if svp else ""))

# ---- what changed lately: prose kept HERE, beside the page it goes on. Every figure is a paired A/B of the engine against
# itself with the one change switched off (FINDINGS 3bz, 3ca), NOT a reading of this page's charts. Edit when the engine changes.
NOTES_TITLE = "What changed in the engine this month"
NOTES = [
 ("More threads for mid-sized random data.",
  "Between about 300,000 and a million elements, data with no structure used to get one thread per 100,000 elements. It now gets one per 25,000: "
  "<b>1.6&ndash;2.0&times;</b> faster at 316,000 elements, 1.3&ndash;1.4&times; at 500,000 and 1.1&times; at a million when %(P)d threads are asked for. Structured inputs are untouched."),
 ("Popular values are counted, not moved.",
  "When a few values each hold a few percent of a column (the busiest zip codes, the best-selling product in a foreign key), the parallel sort used to fall back to a slower two-pass path. "
  "It now counts those values as it passes them and writes them out in one fill: <b>zip_codes 1.66&times;</b> and <b>fk_zipf 1.71&times;</b> at ten million elements and %(P)d threads. "
  "Against the other sorts at 50 million, zip_codes went from 1.75&times; behind IPS&#8308;o in the previous measurement to level with it; fk_zipf narrowed but still trails it by 1.4&times;."),
 ("A counting table that knows when to give up.",
  "On one thread, a column that looked like it had about a thousand distinct values and really had forty thousand was counted in a hash table that outgrew the cache. "
  "The table now stops at 8,192 values and hands over: zip_codes <b>2.0&times;</b> at 100,000 elements, 1.35&times; at 316,000."),
 ("A parallel argsort.",
  "<code>logos_argsort_f64_mt</code> returns the permutation that sorts a column (what sorting records by a key needs) on many threads: <b>3&ndash;10&times;</b> the one-thread "
  "argsort at %(P)d threads from 500,000 to 30 million keys, the identical permutation, 12 bytes of scratch per key instead of 20. It is not one of the charts below."),
]
notes_html = "".join("<li><b>%s</b> %s</li>" % (t, d % dict(P=P)) for t, d in NOTES)

# ---- the page -----------------------------------------------------------------------------------------------------------
at = META.get("measuredAt")
when = datetime.datetime.fromtimestamp(at / 1000, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC") if at else "date not recorded"
sizes_all = sorted({r["n"] for s in DATA for r in s["rows"]})
n_inputs = len(DATA) // 2
who = [("s-logos", "logos-sort", "the engine being measured: C, AVX2 kernels, its own parallel partition. In violet, its strict in-place entry, which allocates no scratch at all.", ""),
       ("s-xss", "x86-simd-sort", "Intel’s vectorized quicksort library, built for %s; at %d threads its OpenMP version." % (XSS_ISA or "this CPU", P), "https://github.com/intel/x86-simd-sort"),
       ("s-vq", "VQSort", "Google’s vectorized quicksort from Highway%s; it has no parallel form, so it appears at one thread only." % ((" " + META["vqVersion"]) if META.get("vqVersion") else ""), "https://github.com/google/highway/tree/master/hwy/contrib/sort"),
       ("s-ips", "IPS⁴o", "In-place Parallel Super Scalar Samplesort (KIT), a state-of-the-art comparison sort: sequential at one thread, parallel at %d." % P, "https://github.com/ips4o/ips4o"),
       ("s-base", "std::sort", "GCC’s introsort; at %d threads __gnu_parallel::sort, its OpenMP mode. Every other output is checked against it." % P, ""),
       ("s-radix", "LSD radix, timsort", "two reference implementations written for the benchmark (timsort in green), one thread only.", "")]
if not META.get("vqsort"): who = [w for w in who if w[1] != "VQSort"]
who_html = "".join('<div class="who"><i class="sw %s"></i><div><b>%s</b> %s%s</div></div>' % (c, esc(nm), esc(d), (' <a href="%s" rel="noopener">source</a>' % l) if l else "") for c, nm, d, l in who)
legend = "".join('<span><i class="sw %s"></i>%s</span>' % (CLS[k], esc("std::sort (1T) / __gnu_parallel::sort (%dT)" % P if k == 0 else NAMES1[k] + (" (1 thread only)" if k in (4, 5, 7) else ""))) for k in ORDER if k < 2 or (k == 2 and META.get("xss")) or (k == 3 and META.get("ips4o")) or (k == 7 and META.get("vqsort")) or 4 <= k <= 6)
cal = META.get("memcpyMs"), META.get("std1mMs")
numeric_max = max(r["n"] for s in DATA for r in s["rows"] if not s["shape"].startswith("strings"))
string_max = max([r["n"] for s in DATA for r in s["rows"] if s["shape"].startswith("strings")] or [0])
range_txt = "from an empty array to %s elements" % commas(numeric_max) + ((", strings to %s" % commas(string_max)) if string_max and string_max != numeric_max else "")

page = r'''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>logos-sort benchmarks</title>
<meta name="description" content="logos-sort against x86-simd-sort, VQSort, IPS4o and std::sort on %(ninputs)d inputs, one thread and %(P)d, %(range)s: every output verified.">
<style>
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;--line:#d9d8d0;
--s-logos:#2a78d6;--s-base:#898781;--s-xss:#eb6834;--s-vq:#1baf7a;--s-ips:#eda100;--s-radix:#e87ba4;--s-tim:#008300;--s-inpl:#6250d6;--pos:#2a78d6;--neg:#e34948;--mid:#c3c2b7}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--line:#2c2c2a;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-vq:#199e70;--s-ips:#c98500;--s-radix:#d55181;--s-tim:#008300;--s-inpl:#9085e9;--pos:#3987e5;--neg:#e66767;--mid:#383835}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--line:#2c2c2a;
--s-logos:#3987e5;--s-base:#898781;--s-xss:#d95926;--s-vq:#199e70;--s-ips:#c98500;--s-radix:#d55181;--s-tim:#008300;--s-inpl:#9085e9;--pos:#3987e5;--neg:#e66767;--mid:#383835}
*{box-sizing:border-box}
body{margin:0;background:var(--page);color:var(--ink);font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;padding-block:0 4rem;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
body::before{content:"";position:fixed;inset:0 0 auto;height:env(safe-area-inset-top,0px);z-index:9;background:var(--page)}
.wrap{max-width:1040px;margin:0 auto}
header{padding-block:2.4rem 1.2rem;border-bottom:1px solid var(--line)}
.eyebrow{font-size:.78rem;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 .4rem}
h1{font-size:clamp(1.7rem,3.6vw,2.4rem);line-height:1.1;margin:0 0 .7rem;letter-spacing:-.01em;text-wrap:balance}
h2{font-size:1.25rem;margin:2.2rem 0 .7rem;letter-spacing:-.005em;text-wrap:balance}
h3{font-size:1.02rem;margin:1.6rem 0 .5rem;display:flex;flex-wrap:wrap;gap:.3rem .7rem;align-items:baseline}
h3 .brief{font-weight:400;color:var(--ink2);font-size:.9rem}
h4{font-size:.8rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:1rem 0 .4rem;font-weight:600}
p{max-width:70ch;margin:0 0 .8rem}
.lede{font-size:1.05rem;color:var(--ink2)}
.meta{font-size:.86rem;color:var(--ink2);display:flex;flex-wrap:wrap;gap:.3rem 1.2rem;margin:0}
a{color:var(--s-logos)}
.score{font-size:1.05rem;max-width:none}
.gl{display:grid;grid-template-columns:1fr 1fr;gap:1.2rem 2rem;margin:1rem 0 .6rem}
@media (max-width:800px){.gl{grid-template-columns:1fr}}
.glc h3{margin:0 0 .5rem;font-size:.82rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted)}
.glr{display:grid;grid-template-columns:7.4rem 1fr 9.6rem;gap:.6rem;align-items:center;font-size:.84rem;padding:.17rem 0;min-width:0}
.glr .nm{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.glr .trk{position:relative;height:16px;background:var(--surface);border:1px solid var(--grid);border-radius:3px;min-width:0}
.glr .mid{position:absolute;top:-3px;bottom:-3px;width:2px;background:var(--mid);transform:translateX(-1px)}
.glr .b{position:absolute;top:3px;bottom:3px;border-radius:2px}.glr .b.pos{background:var(--pos)}.glr .b.neg{background:var(--neg)}.glr .b.eq{background:var(--mid)}
.glr .v{font-variant-numeric:tabular-nums;white-space:nowrap}.glr .v small{display:block;color:var(--muted);font-size:.74rem;white-space:normal}
.glr.ax{padding-top:.3rem}.glr.ax .trk{height:1.3rem;background:none;border:0}
.glr.ax .tk{position:absolute;top:0;transform:translateX(-50%%);font-size:.7rem;color:var(--muted);white-space:nowrap}
.glr.ax .tk.l{transform:none}.glr.ax .tk.r{transform:translateX(-100%%)}.glr.ax .tk.cap{top:.95rem;font-size:.68rem}
@media (max-width:560px){
.glr{grid-template-columns:1fr auto;grid-template-areas:"nm v" "trk trk";gap:.15rem .6rem;padding:.35rem 0}
.glr .nm{grid-area:nm}.glr .v{grid-area:v;text-align:right}.glr .v small{display:inline;margin-left:.3rem}.glr .trk{grid-area:trk}
.glr.ax{grid-template-columns:1fr;grid-template-areas:none;padding-top:0}.glr.ax span:empty{display:none}.glr.ax .trk{grid-area:auto}}
.hint{font-size:.84rem;color:var(--ink2)}
details{margin:.6rem 0 0}summary{cursor:pointer;color:var(--ink2);font-size:.9rem}
.tw{overflow-x:auto;border:1px solid var(--grid);border-radius:6px;margin:.5rem 0 1rem;background:var(--surface)}
table{border-collapse:collapse;width:100%%;font-size:.84rem;font-variant-numeric:tabular-nums;white-space:nowrap}
th,td{padding:.32rem .6rem;text-align:right;border-bottom:1px solid var(--grid)}th{color:var(--muted);font-weight:500;font-size:.76rem}
td:first-child,th:first-child{text-align:left}tr:last-child td{border-bottom:0}tr.mem td{color:var(--ink2);font-style:italic}
.sw{display:inline-block;width:12px;height:3px;vertical-align:middle;margin-right:.45rem;border-radius:2px}
.s-logos{--c:var(--s-logos)}.s-base{--c:var(--s-base)}.s-xss{--c:var(--s-xss)}.s-ips{--c:var(--s-ips)}.s-radix{--c:var(--s-radix)}.s-tim{--c:var(--s-tim)}.s-inpl{--c:var(--s-inpl)}.s-vq{--c:var(--s-vq)}
.sw{background:var(--c)}
.legend{display:flex;flex-wrap:wrap;gap:.3rem 1.1rem;font-size:.82rem;color:var(--ink2);margin:.6rem 0 .2rem}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:1rem}
@media (max-width:800px){.pair{grid-template-columns:1fr}}
figure{margin:0;min-width:0;background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:.6rem .7rem .3rem}
figcaption{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 .2rem}
svg.ch{display:block;width:100%%;height:auto;max-width:100%%;overflow:visible;touch-action:pan-y}
svg.ch .grid{stroke:var(--grid);stroke-width:1}svg.ch .grid.v{stroke-dasharray:2 3}
svg.ch .tk{fill:var(--muted);font-size:13.5px;font-family:inherit}
svg.ch .ln{fill:none;stroke:var(--c);stroke-width:2;stroke-linejoin:round;stroke-linecap:round}svg.ch .ln.lg{stroke-width:3}
svg.ch .clip,svg.ch .pt{fill:var(--c)}figcaption span{text-transform:none;letter-spacing:0}svg.ch .lbl{fill:var(--c);font-size:12.5px;font-weight:600;font-family:inherit;paint-order:stroke;stroke:var(--surface);stroke-width:3px}
svg.ch .cur{stroke:var(--axis);stroke-width:1;stroke-dasharray:3 3}
svg.ch .gd polyline{fill:none;stroke:var(--muted);stroke-width:1;stroke-dasharray:5 4}
svg.ch .gd text{fill:var(--muted);font-size:11px;font-family:inherit;paint-order:stroke;stroke:var(--surface);stroke-width:3px}
body[data-guides="off"] svg.ch .gd{display:none}
.gdt{display:inline-flex;gap:.4rem;align-items:center;cursor:pointer}.gdt input{margin:0;accent-color:var(--s-logos)}
.off{font-size:.76rem;color:var(--muted);margin:.2rem 0 .3rem}
.m-tot,svg.ch.m-tot{display:none}body[data-metric="tot"] .m-ns{display:none}body[data-metric="tot"] svg.ch.m-tot{display:block}body[data-metric="tot"] span.m-tot{display:inline}
.pick{display:flex;flex-wrap:wrap;gap:.5rem .9rem;align-items:center;margin:.9rem 0 .2rem;font-size:.86rem;color:var(--ink2);position:sticky;top:env(safe-area-inset-top,0px);z-index:5;background:var(--page);padding:.45rem 0}
.seg{display:inline-flex;border:1px solid var(--axis);border-radius:7px;overflow:hidden}
.seg button{font:inherit;font-size:.84rem;padding:.32rem .85rem;background:var(--surface);color:var(--ink2);border:0;cursor:pointer}
.seg button+button{border-left:1px solid var(--axis)}
.seg button[aria-pressed="true"]{background:var(--ink);color:var(--page)}
.seg button:focus-visible{outline:2px solid var(--s-logos);outline-offset:-2px}
ul.notes{padding-left:1.2rem;max-width:74ch;font-size:.94rem}ul.notes li{margin:0 0 .55rem}code{font-size:.88em;background:var(--surface);border:1px solid var(--grid);border-radius:4px;padding:0 .25rem}
.desc{font-size:.9rem;color:var(--ink2);max-width:80ch}
#tip{position:fixed;z-index:20;pointer-events:none;background:var(--surface);color:var(--ink);border:1px solid var(--axis);border-radius:6px;padding:.4rem .6rem;font-size:.8rem;box-shadow:0 4px 18px rgba(0,0,0,.14);max-width:min(320px,calc(100vw - 32px))}
#tip table{font-size:.78rem;width:auto}#tip td{padding:.1rem .45rem;border:0}#tip .h{font-weight:600;margin:0 0 .2rem}
.who{display:flex;gap:.7rem;align-items:baseline;margin:0 0 .55rem;font-size:.92rem}.who .sw{flex:none;position:relative;top:-2px}
ul.method{padding-left:1.2rem;max-width:72ch;font-size:.92rem}ul.method li{margin:0 0 .35rem}
.cal{background:var(--surface);border-left:3px solid #fab219;padding:.5rem .8rem;margin:.6rem 0;font-size:.88rem;max-width:72ch}
pre{background:var(--surface);border:1px solid var(--grid);border-radius:6px;padding:.7rem .9rem;overflow-x:auto;font-size:.82rem;line-height:1.45}
footer{margin-top:3rem;padding-top:1rem;border-top:1px solid var(--line);font-size:.84rem;color:var(--ink2)}
@media (prefers-reduced-motion:no-preference){html{scroll-behavior:smooth}}
</style></head><body><div class="wrap">
<header>
<p class="eyebrow">Benchmark</p>
<h1>logos-sort against the fastest open-source sorts</h1>
<p class="lede">logos-sort reads the shape of its input before it sorts. When the data has structure &mdash; stretches already in order, a handful of distinct values, numbers on a fixed grid, sorted blocks in the wrong order &mdash; it places elements by address instead of comparing them. This page times it against Intel&rsquo;s x86-simd-sort, Google&rsquo;s VQSort, IPS&#8308;o and the C++ standard library on %(ninputs)d inputs, %(range)s, on one thread and on %(P)d.</p>
<p class="meta"><span>%(cpu)s</span><span>%(compiler)s</span><span>%(P)d threads for the parallel runs (%(ncpus)d logical CPUs)</span><span>measured %(when)s</span><span><a href="reports/scale-report.html">the full report</a>: every size, zoomable charts, memory per sort</span></p>
</header>

<h2 id="largest">At the largest size</h2>
<p class="score">%(score)s</p>
<div class="gl">
<div class="glc" role="group" aria-label="one thread each"><h3>1 thread each</h3>%(g1)s</div>
<div class="glc" role="group" aria-label="%(P)d threads each"><h3>%(P)d threads each</h3>%(gp)s</div>
</div>
<p class="hint">Each bar compares logos with the fastest <em>other</em> sort on that input at the largest size measured for it. Right of the line logos is faster by the factor shown; left, the other sort is. Differences under 5%% are shown as level: that is within this machine&rsquo;s run-to-run noise. Under each factor are the two total times: logos first, then the other sort.</p>
<details><summary>the same comparison as a table, with peak heap per element</summary><div class="tw"><table><thead><tr><th>input</th><th>threads</th><th>n</th><th>logos</th><th>fastest other</th><th>its time</th><th>other &divide; logos</th><th>logos B/elem</th><th>other B/elem</th></tr></thead><tbody>%(gt1)s%(gtp)s</tbody></table></div>
<p class="hint">Peak heap of one cold call, in bytes per element, at the largest size the memory pass measured (%(memn)s): logos allocates scratch on most routes, the vector quicksorts sort in place. A dash is a reading the pass could not take (the key-value sort on urls returns an impossible figure there; the full report carries the raw value).</p></details>

<h2 id="why">Where the speed comes from</h2>
<p>Most of the inputs have structure that real data often has: logs that arrive nearly in time order, columns with few distinct values, prices on a grid of cents, shards concatenated out of order. logos detects it from a small sample and takes a route that does less work than any comparison sort can: it counts, moves whole runs, or merges only where runs overlap. On those inputs the margin generally grows with n.</p>
<p>Uniformly random numbers have no structure to use. On one thread logos then runs its own vectorized quicksort, the same design as x86-simd-sort and VQSort, so those rows are a race between kernels; on many threads it splits the values into ranges across all cores and sorts each range. The uniform, heavy_tail, bimodal and int_wide inputs are that case, and among the practical ones the latencies, the coordinates and the foreign keys.</p>

<h2 id="changes">%(notestitle)s</h2>
<ul class="notes">%(notes)s</ul>
<p class="hint">Each figure above comes from timing the engine against itself with that one change switched off, in alternating pairs on the same data. The charts below were measured with all of them in.</p>

<h2 id="curves">How long each sort takes as the input grows</h2>
<p>One pair of charts per input: sorts that each use one thread on the left, sorts that each use %(P)d threads on the right. Two views of the same measurements. <b>Per element</b> divides each time by n, so a flat line is linear scaling and a rising one is the n log n of a comparison sort (or a cache being outgrown); its axis opens to show logos and its nearest rivals, a triangle at the top marks a point above it, and the line beneath the chart names the sorts that run off the top. <b>Total time</b> is how long one whole sort took, on a logarithmic axis because it runs from microseconds to tens of seconds; every sort fits, and a gap of one gridline is ten times. Hover or touch a chart in either view to read both figures for every sort at that n; the label on logos&rsquo;s line gives its last point. Sizes start at 10,000, where each point is one array sorted repeatedly with the minimum kept; the <a href="reports/scale-report.html">full report</a> goes down to an empty array.</p>
<div class="pick"><span id="pickl">Charts show</span><span class="seg" role="group" aria-labelledby="pickl"><button type="button" data-m="ns" aria-pressed="true">time per element</button><button type="button" data-m="tot" aria-pressed="false">total time</button></span><label class="gdt"><input type="checkbox" id="gd" checked> growth guides, O(1) to O(n&sup2;)</label></div>
<p class="hint">The dashed grey lines are growth guides. Each starts at logos&rsquo;s time at the smallest size plotted and shows where that time would go if it grew as O(1), O(log n), O(n), O(n log n) or O(n&sup2;) from there. In the total-time view they fan out upward: O(n) climbs one gridline per tenfold n, O(n&sup2;) two and leaves the top. In the per-element view everything is divided by n, so O(n) is the flat line, O(n log n) rises gently, O(n&sup2;) shoots off the top and O(1) and O(log n) fall to the axis. A measured line that tracks a guide grows at that rate over this range; one that bends away from O(n) late is usually the data outgrowing a cache, not a change of algorithm.</p>
<div class="legend">%(legend)s</div>
%(sections)s

<h2 id="who">The contenders</h2>
%(who)s
<p class="hint">On strings the vector sorts sort numbers, not text: their string lines are their key-value sorts driven by the same 8-byte-prefix scheme logos uses, so those compare the kernels under one method. std::sort, __gnu_parallel::sort and IPS&#8308;o compare with strcmp.</p>

<h2 id="method">How it was measured</h2>
<ul class="method">
<li><b>Every output is checked</b> against std::sort&rsquo;s on the same input before its time counts. A mismatch marks the row with &#10007;; there are none unless one is shown.</li>
<li><b>Same input, best of several runs.</b> Each size is sorted by every contender in turn from an identical copy: the fastest of 5 runs up to a million elements, of 3 up to ten million, 1 above.</li>
<li><b>One thread against one thread, %(P)d against %(P)d.</b> %(P)d is the physical core count; every parallel sort is given that many threads, IPS&#8308;o included.</li>
<li><b>Strings stop at %(strmax)s elements and the memory pass at %(memn)s.</b> A billion strings do not fit the machine, and per element the memory figures are flat from ten million up.</li>
<li><b>The machine is a laptop under WSL2, not a lab.</b> Absolute times move by tens of percent between runs; the sorts at one size run seconds apart, so the comparisons within a chart hold, and differences under 5%% count as level.</li>
</ul>
<div class="cal">How busy the machine was, measured on one thread before the run: copying 64 MB took %(memcpy)s ms and std::sort of a million doubles %(std1m)s ms. At rest it reads about 3 ms and 60 ms; well above that every absolute time is inflated, while the comparisons between sorts in one chart still hold.</div>

<h2 id="repro">Reproducing it</h2>
<p>One command on Linux with GCC and an x86-64 CPU with AVX2. From a checkout of <a href="https://github.com/markciubal/logos-sort">logos-sort</a>, fetch the opponents beside the engine, then build and run:</p>
<pre>cd engines
git clone --depth 1 https://github.com/intel/x86-simd-sort
git clone --depth 1 https://github.com/ips4o/ips4o
git clone --depth 1 --branch 1.4.0 https://github.com/google/highway
sudo apt install libtbb-dev                     # IPS4o links TBB
cd c &amp;&amp; LOGOS_MAX_N=%(maxn)s LOGOS_MAX_SEC=600 LOGOS_STR_MAX_N=%(strmaxraw)s LOGOS_SCALE_MEM_MAX_N=%(memmaxraw)s make scale-full
python ../../tools/scale_index.py ../../reports/scale-report.html ../../index.html   # this page</pre>
<p>The run prints every time as it goes and exits non-zero if any output differs from std::sort&rsquo;s. Absolute times depend on the machine; the comparisons within each chart are the result.</p>
<footer>Generated by tools/scale_index.py from reports/scale-report.html (measured %(when)s). The project&rsquo;s own account of what was tried and what failed is in its CLAUDE.md, FINDINGS.md and TRUTH_TABLE.md.</footer>
</div>
<div id="tip" hidden></div>
<script>
(function(){
 const tip=document.getElementById('tip');
 const fmtNs=v=>v>=1e6?(v/1e6).toFixed(1)+' ms':v>=1e3?(v/1e3).toFixed(1)+' µs':(v>=100?v.toFixed(0):v>=10?v.toFixed(1):v.toFixed(2))+' ns';
 const fmtMs=v=>v>=1000?(v/1000).toFixed(v>=10000?1:2)+' s':v>=10?v.toFixed(0)+' ms':v>=1?v.toFixed(1)+' ms':v>=1e-3?(v*1e3).toFixed(v>=1e-2?0:1)+' µs':(v*1e6).toFixed(1)+' ns';
 const fmtN=n=>n>=1e9?+(n/1e9).toFixed(2)+'B':n>=1e6?+(n/1e6).toFixed(2)+'M':n>=1e3?+(n/1e3).toFixed(1)+'k':String(n);
 const CL={0:'s-base',1:'s-logos',2:'s-xss',3:'s-ips',4:'s-radix',5:'s-tim',6:'s-inpl',7:'s-vq'};
 document.querySelectorAll('svg.ch').forEach(svg=>{
  const ns=JSON.parse(svg.dataset.n),S=JSON.parse(svg.dataset.s),NM=JSON.parse(svg.dataset.nm),ml=+svg.dataset.ml,mr=+svg.dataset.mr,W=svg.viewBox.baseVal.width;
  const cur=svg.querySelector('.cur'),x0=Math.log10(ns[0]),x1=Math.log10(ns[ns.length-1]);
  const px=n=>ml+(Math.log10(n)-x0)/((x1-x0)||1)*(W-ml-mr);
  function at(ev){const r=svg.getBoundingClientRect(),x=(ev.clientX-r.left)/r.width*W;let bi=0,bd=1e9;ns.forEach((n,i)=>{const d=Math.abs(px(n)-x);if(d<bd){bd=d;bi=i;}});return bi;}
  function show(ev){const i=at(ev),n=ns[i];cur.setAttribute('x1',px(n));cur.setAttribute('x2',px(n));
   const rows=Object.keys(S).map(k=>[k,S[k][i]]).filter(r=>r[1]!=null).sort((a,b)=>a[1]-b[1]);const lg=S['1']?S['1'][i]:null;
   tip.innerHTML='<div class="h">'+svg.getAttribute('aria-label').split(':')[0]+' · n = '+Number(n).toLocaleString('en-US')+'</div><table>'+rows.map(r=>'<tr><td><i class="sw '+CL[r[0]]+'"></i>'+NM[r[0]]+'</td><td>'+fmtMs(r[1]*n/1e6)+'</td><td>'+fmtNs(r[1])+'/el</td><td>'+(lg&&r[0]!='1'?(r[1]/lg).toFixed(2)+'× logos':'')+'</td></tr>').join('')+'</table>';
   tip.hidden=false;const tw=tip.offsetWidth,th=tip.offsetHeight;let L=ev.clientX+14,T=ev.clientY+14;if(L+tw>innerWidth-8)L=ev.clientX-tw-14;if(T+th>innerHeight-8)T=ev.clientY-th-14;tip.style.left=Math.max(8,L)+'px';tip.style.top=Math.max(8,T)+'px';}
  function hide(){tip.hidden=true;cur.setAttribute('x1',-10);cur.setAttribute('x2',-10);}
  svg.addEventListener('pointermove',show);svg.addEventListener('pointerdown',show);svg.addEventListener('pointerleave',hide);
 });
 document.addEventListener('scroll',()=>{tip.hidden=true;},{passive:true});
 const btns=[...document.querySelectorAll('.seg button')];
 function pick(m,save){document.body.dataset.metric=m;btns.forEach(b=>b.setAttribute('aria-pressed',b.dataset.m===m?'true':'false'));tip.hidden=true;if(save){try{localStorage.setItem('logos-index-metric',m);}catch(e){}}}
 btns.forEach(b=>b.addEventListener('click',()=>pick(b.dataset.m,true)));
 let m0='ns';try{const v=localStorage.getItem('logos-index-metric');if(v==='tot'||v==='ns')m0=v;}catch(e){}
 pick(m0,false);
 const gd=document.getElementById('gd');
 function guides(on,save){document.body.dataset.guides=on?'on':'off';gd.checked=on;if(save){try{localStorage.setItem('logos-index-guides',on?'on':'off');}catch(e){}}}
 gd.addEventListener('change',()=>guides(gd.checked,true));
 let g0=true;try{if(localStorage.getItem('logos-index-guides')==='off')g0=false;}catch(e){}
 guides(g0,false);
})();
</script>
</body></html>
''' % dict(ninputs=n_inputs, P=P, range=range_txt, cpu=esc(META.get("cpu", "")), compiler=esc(META.get("compiler", "")), ncpus=META.get("ncpus", 0), when=when,
           score=score, notes=notes_html, notestitle=NOTES_TITLE, g1=glance_rows(G1), gp=glance_rows(GP), gt1=glance_table(G1), gtp=glance_table(GP), memn=fmt_n(min(MEM_MAX_N, MAX_N)),
           legend=legend, sections="\n".join(sections), who=who_html, strmax=fmt_n(min(STR_MAX_N, MAX_N)),
           memcpy=("%.2f" % cal[0]) if cal[0] else "?", std1m=("%.1f" % cal[1]) if cal[1] else "?",
           maxn=("%g" % META.get("maxN", MAX_N)).replace("e+0", "e").replace("+", ""), strmaxraw=("%g" % STR_MAX_N).replace("e+0", "e").replace("+", ""), memmaxraw=("%g" % MEM_MAX_N).replace("e+0", "e").replace("+", ""))

open(OUT, "wb").write(page.encode("utf-8"))
print("wrote %s: %d bytes, %d inputs, n to %s (strings to %s, memory to %s), %d charts" % (OUT, len(page.encode("utf-8")), n_inputs, fmt_n(MAX_N), fmt_n(min(STR_MAX_N, MAX_N)), fmt_n(min(MEM_MAX_N, MAX_N)), page.count('<svg class="ch ')))
