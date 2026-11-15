"""table.py -- turns logs of parity/parity.sh into the Markdown tables of PARITY.md, so that no
number in that file is typed by hand.

    python parity/table.py board_8m.log [more logs ...]        one table per log, both thread counts side by side
    python parity/table.py --counts a.log b.log ...            one row per log: how many cells ahead / level / behind
"""
import re
import sys

ROW = re.compile(r"^\s+(\S+)\s+([\d.]+)(!?)\s+([\d.]+)(!?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(RUST|PARITY|behind)")

def parse(path):
    head, sections, cur = "", [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("parity:"):
            head = line.strip()
        m = re.match(r"^(\d+) threads?$", line.strip())
        if m:
            cur = (int(m.group(1)), [])
            sections.append(cur)
            continue
        m = ROW.match(line)
        if m and cur is not None:
            cur[1].append(dict(shape=m.group(1), c=float(m.group(2)), rust=float(m.group(4)), ratio=float(m.group(6)),
                               p10=float(m.group(7)), p90=float(m.group(8)), verdict=m.group(9), wrong=bool(m.group(3) or m.group(5))))
    n = int(re.search(r"n = (\d+)", head).group(1)) if head else 0
    return head, n, sections

def word(v):
    return {"RUST": "ahead", "PARITY": "level", "behind": "**behind**"}[v]

def table(path):
    head, n, sections = parse(path)
    print(f"`{head}`\n")
    cols = " | ".join(f"C, {p}T | Rust, {p}T | C / Rust (p10-p90) | at {p}T" for p, _ in sections)
    print("| shape | " + cols + " |")
    print("|---|" + "---:|---:|---:|---|" * len(sections))
    for i, r in enumerate(sections[0][1]):
        cells = []
        for _, rows in sections:
            q = rows[i]
            cells.append(f"{q['c']:.2f} | {q['rust']:.2f}{' WRONG' if q['wrong'] else ''} | {q['ratio']:.2f} ({q['p10']:.2f}-{q['p90']:.2f}) | {word(q['verdict'])}")
        print(f"| {r['shape']} | " + " | ".join(cells) + " |")
    print()

def counts(paths):
    print("| run | elements | threads | ahead | level | behind | behind on |")
    print("|---|---:|---:|---:|---:|---:|---|")
    for path in paths:
        head, n, sections = parse(path)
        name = path.replace("\\", "/").split("/")[-1].replace(".log", "")
        for p, rows in sections:
            a = sum(r["verdict"] == "RUST" for r in rows)
            l = sum(r["verdict"] == "PARITY" for r in rows)
            b = [f"{r['shape']} {r['ratio']:.2f}" for r in rows if r["verdict"] == "behind"]
            print(f"| {name} | {n:,} | {p} | {a} | {l} | {len(b)} | {', '.join(b) or '-'} |")
    print()

if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--counts":
        counts(args[1:])
    else:
        for a in args:
            table(a)
