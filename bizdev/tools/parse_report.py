"""Parse the scale report's DATA array and print, per shape x threads x n, logos against the best rival
with peak heap per element on both sides. Run from anywhere: python parse_report.py <report.html> <out.md> <out.json>"""
import json, re, sys

src = open(sys.argv[1], "rb").read().decode("utf-8")
i = src.index("const DATA=") + len("const DATA=")
data, _ = json.JSONDecoder().raw_decode(src, i)
j = src.index("META=") + len("META=")
meta, _ = json.JSONDecoder().raw_decode(src, j)

NAMES1 = ["std::sort", "logos", "x86-simd-sort", "IPS4o", "LSD radix", "timsort", "logos in-place", "VQSort"]
NAMESP = ["gnu_parallel", "logos", "x86-simd-sort", "IPS4o", "", "", "logos in-place", ""]
LOGOS = 1
INPLACE = 6

def names_for(panel):
    if "names" in panel:
        n = []
        for s in panel["names"]:
            s = s.replace("⁴", "4")
            if s.startswith("std::sort"): n.append("std::sort")
            elif s.startswith("__gnu_parallel"): n.append("gnu_parallel")
            elif s.startswith("x86-simd-sort"): n.append("x86-simd-sort kv")
            elif s.startswith("IPS4o"): n.append("IPS4o")
            elif s.startswith("multikey"): n.append("mkqs (old logos)")
            elif s.startswith("VQSort"): n.append("VQSort kv")
            else: n.append(s)
        return n
    return NAMES1 if panel["threads"] == 1 else NAMESP

out = []
rows_md = []
SIZES = (100000, 316000, 1000000, 3162000, 10000000)
for panel in data:
    names = names_for(panel)
    shape, thr = panel["shape"], panel["threads"]
    for r in panel["rows"]:
        if r["n"] not in SIZES:
            continue
        t, mem = r["t"], r["m"]
        lt, lm = t[LOGOS], mem[LOGOS]
        rivals = [(t[i], names[i], mem[i], i) for i in range(len(t))
                  if i not in (LOGOS, INPLACE) and names[i] and t[i] and t[i] > 0]
        rivals.sort()
        bt, bn, bm, bi = rivals[0]
        second = rivals[1] if len(rivals) > 1 else None
        ratio = bt / lt
        ipt = t[INPLACE]
        ip_ratio = (bt / ipt) if ipt else None
        rec = dict(shape=shape, threads=thr, n=r["n"], ok=r["ok"], logos_ms=lt, logos_B=lm,
                   logos_ns_el=lt * 1e6 / r["n"], best=bn, best_ms=bt, best_B=bm, ratio=ratio,
                   second=second[1] if second else None, second_ms=second[0] if second else None,
                   second_B=second[2] if second else None,
                   inplace_ms=ipt, inplace_vs_best=ip_ratio,
                   all={names[i]: (t[i], mem[i]) for i in range(len(t)) if names[i]})
        out.append(rec)
        verdict = "level" if 0.95 <= ratio <= 1.05 else ("WIN" if ratio > 1.05 else "LOSS")
        rows_md.append("| %s | %d | %s | %.3f | %.1f | %s | %.3f | %.1f | %.2f | %s | %s |" % (
            shape, thr, "{:,}".format(r["n"]), lt, lm, bn, bt, bm, ratio, verdict,
            ("%.2f" % ip_ratio) if ip_ratio else "-"))

hdr = ["| shape | P | n | logos ms | logos B/el | best rival | rival ms | rival B/el | rival/logos | verdict | rival/in-place |",
       "|---|---|---|---|---|---|---|---|---|---|---|"]
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
    f.write("META: %s\n\n" % json.dumps(meta))
    f.write("\n".join(hdr + rows_md) + "\n")
with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as f:
    json.dump(dict(meta=meta, rows=out), f, indent=1)
print("panels", len(data), "rows", len(out))
print("measuredAt", meta["measuredAt"])
