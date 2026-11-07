#!/usr/bin/env python3
"""Combine two ledgerbench runs taken in opposite copy orientations into one layout-free effect per row.

ledgerbench prints ROW<TAB>shape<TAB>threads<TAB>median(t_lg0 / t_lg1)<TAB>p10<TAB>p90<TAB>verified. Run A has the BASELINE in lg0
and the variant in lg1; run B has them swapped. Then sqrt(A / B) is the variant's speedup (> 1: the variant is faster) with the
copies' code-layout bias cancelled, and sqrt(A * B) is that bias. Same arithmetic as tools/ledger_rerun.py.

usage: python tools/pair_orient.py run_a.txt run_b.txt
"""
import math
import sys


def rows(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("ROW\t"):
                continue
            _, shape, threads, med, p10, p90, ok = line.rstrip("\n").split("\t")
            out[(shape, int(threads))] = (float(med), ok.strip() == "1")
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = rows(sys.argv[1]), rows(sys.argv[2])
    keys = sorted(set(a) & set(b), key=lambda k: (k[1], k[0]))
    if not keys:
        print("no rows in common")
        return 1
    print("%-12s %3s %9s %9s %9s %9s" % ("shape", "T", "orient A", "orient B", "speedup", "layout"))
    bad = 0
    for k in keys:
        (ra, oka), (rb, okb) = a[k], b[k]
        if not (oka and okb):
            print("%-12s %3d  VERIFY FAILED" % k)
            bad = 1
            continue
        print("%-12s %3d %9.3f %9.3f %9.3f %9.3f" % (k[0], k[1], ra, rb, math.sqrt(ra / rb), math.sqrt(ra * rb)))
    return bad


if __name__ == "__main__":
    sys.exit(main())
