#!/usr/bin/env python3
"""Re-test the ledger's "neutral" and "mixed" verdicts against today's noise floor.

Those verdicts were all reached when an interleaved A/B could not resolve better than about 1.25x,
because separately compiled copies of the engine differ by up to 1.24x from code alignment alone
(CLAUDE.md rule 3). `bench_paired` now resolves about 4%. Anything once dismissed as noise in the
1.05-1.25 band was therefore never actually tested. That is the new angle the ledger's "do not
re-run" rule asks for.

Two corrections are needed and this script applies both.

  drift   Within one pair the two variants are timed back to back, so the machine's 5-10x thermal
          wander divides out. `bench_paired` does that.
  layout  Pairing does NOT fix code alignment: each variant still lives in a fixed copy. So every
          feature is built TWICE with the copies swapped. If the layout factors are L0 and L1 and
          the true baseline-over-feature ratio is E, then
              orientation A (baseline in lg0) measures  E * (L0/L1)
              orientation B (feature  in lg0) measures  (1/E) * (L0/L1)
          so  sqrt(A/B) = E  with the layout term cancelled exactly, and sqrt(A*B) = L0/L1 recovers
          the layout bias itself, which is worth seeing.

Usage:  python tools/ledger_rerun.py [--pairs 24] [--n 8000000] [--only FLAG]
"""
import math
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CDIR = os.path.join(ROOT, "engines", "c")

# (flag, name, previous verdict, flag_disables_the_feature)
# When the flag DISABLES the feature -- a shipped default being re-tested rather than an opt-in being
# tried -- it goes on the BASELINE copy instead, so "speedup > 1" always reads as "feature is faster".
FEATURES = [
    ("-DLOGOS_STREAM", "non-temporal stores in the scatter",
     "never tested: the scatter pays read-for-ownership on every line"),
    ("-DLOGOS_HUGEPAGE", "huge pages for the large buffers",
     "measured neutral 2026-09-12 at 8M and 30M; kept opt-in"),
    ("-DLOGOS_HIST_INTERLEAVE", "interleaved histogram counters", "kept opt-in, 'measured mixed'"),
    ("-DLOGOS_LINE_REPAIR", "cache-line repair in the leaf", "kept opt-in, 'measured mixed'"),
    ("-DLOGOS_INT_NARROW", "bucket-relative int32 digit narrowing", "ledger: mixed, +21%/-16%"),
    ("-DLOGOS_MODEL_TABLE", "linear model splitter table", "ledger: 0.93-1.09x, 'noise'"),
    ("-DAV_PIVOT_NET", "sorting-network pivot for the AVX2 leaf", "ledger: 0.94-1.06x, 'noise'"),
    ("-DLOGOS_ZERO_SIGN_UNSAFE", "signed-zero fix-up in the AVX2 leaf (a -0.0 test riding the NaN scan)",
     "first version (second compare + AND + OR per block) cost 7% at 1T on uniform and heavy_tail; now one compare", True),
]


def sh(cmd, timeout=3600):
    """Run inside WSL when we are on Windows, otherwise directly."""
    if sys.platform == "win32":
        full = ["wsl", "-e", "bash", "-lc", "cd /mnt/c%s && %s" % (CDIR[2:].replace("\\", "/"), cmd)]
    else:
        full = ["bash", "-lc", "cd %s && %s" % (CDIR, cmd)]
    r = subprocess.run(full, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def run_one(flag, orientation, pairs, n, off=False):
    base, feat = (flag, "") if off else ("", flag)
    lb0, lb1 = (base, feat) if orientation == 0 else (feat, base)
    rc, out = sh('make -s ledgerbench LB0="%s" LB1="%s" 2>&1 | grep -E "error" ; true' % (lb0, lb1))
    if "error" in out:
        print("  BUILD FAILED:\n" + out[:800])
        return None
    rc, out = sh("./ledgerbench %d %d" % (pairs, n))
    rows = {}
    for line in out.splitlines():
        if not line.startswith("ROW\t"):
            continue
        _, shape, threads, med, p10, p90, ok = line.split("\t")
        if ok.strip() != "1":
            print("  VERIFY FAILED on %s/%s" % (shape, threads))
            return None
        rows[(shape, int(threads))] = float(med)
    if not rows:
        print("  no rows parsed; output was:\n" + out[-800:])
        return None
    return rows


def main():
    pairs = 24
    n = 8000000
    only = None
    a = sys.argv[1:]
    for i, x in enumerate(a):
        if x == "--pairs": pairs = int(a[i + 1])
        elif x == "--n": n = int(a[i + 1])
        elif x == "--only": only = a[i + 1]

    feats = [f for f in FEATURES if only is None or only in f[0]]
    print("ledger re-run: %d features, %d pairs, n=%d, two orientations each" % (len(feats), pairs, n))
    print("speedup = sqrt(A/B) with the layout bias cancelled; >1.000 means the FEATURE is faster\n")

    verdicts = []
    for entry in feats:
        flag, name, old = entry[0], entry[1], entry[2]
        off = len(entry) > 3 and bool(entry[3])
        print("=" * 78)
        print("%s\n  %s%s\n  previously: %s" % (name, flag, " (this flag DISABLES it)" if off else "", old))
        A = run_one(flag, 0, pairs, n, off)
        B = run_one(flag, 1, pairs, n, off)
        if A is None or B is None:
            print("  SKIPPED"); continue
        keys = sorted(set(A) & set(B), key=lambda k: (k[1], k[0]))
        print("\n  %-12s %2s %10s %10s %10s   %s" % ("shape", "T", "orient A", "orient B", "speedup", "layout L0/L1"))
        best = 1.0
        for k in keys:
            ra, rb = A[k], B[k]
            if ra <= 0 or rb <= 0: continue
            speed = math.sqrt(ra / rb)
            layout = math.sqrt(ra * rb)
            print("  %-12s %2d %10.3f %10.3f %10.3f   %.3f" % (k[0], k[1], ra, rb, speed, layout))
            if abs(math.log(speed)) > abs(math.log(best)):
                best = speed
            verdicts.append((name, k[0], k[1], speed))
        print("\n  largest effect: %.3f" % best)
    print("=" * 78)
    print("\nSUMMARY (only rows outside +/-5%%, which is about this box's floor at %d pairs)" % pairs)
    hits = [v for v in verdicts if abs(v[3] - 1.0) > 0.05]
    if not hits:
        print("  none. Every re-tested feature is inside the noise floor even at the new resolution,")
        print("  which upgrades those ledger rows from 'could not resolve' to 'measured neutral'.")
    for name, shape, t, sp in sorted(hits, key=lambda v: -abs(math.log(v[3]))):
        print("  %-38s %-12s %2dT  %.3f  %s" % (name, shape, t, sp, "FEATURE FASTER" if sp > 1 else "feature slower"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
