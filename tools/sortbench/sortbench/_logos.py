"""LogosSort (pure Python) — bundled as a benchmark contender.

This is the v0.2 dual-pivot introsort with a counting fast path and a heapsort
depth guard. It exists here so the library ships a non-trivial pure-Python sort
to compare against the C-backed builtin, numpy, and the databases. It is, by
design, far slower than any of those at large n (the interpreter tax) — its one
competitive regime is duplicate-heavy integer data, where the counting path
emits via C-level slice assignment and can beat the builtin.
"""
import random
import math

__all__ = ["logos_sort"]


def _insertion_sort(a, lo, hi):
    for i in range(lo + 1, hi + 1):
        key = a[i]; j = i - 1
        while j >= lo and a[j] > key:
            a[j + 1] = a[j]; j -= 1
        a[j + 1] = key


def _heapsort(a, lo, hi):
    n = hi - lo + 1
    def sift(root, end):
        while True:
            child = 2 * root + 1
            if child > end: return
            if child + 1 <= end and a[lo + child] < a[lo + child + 1]:
                child += 1
            if a[lo + root] < a[lo + child]:
                a[lo + root], a[lo + child] = a[lo + child], a[lo + root]
                root = child
            else:
                return
    for start in range(n // 2 - 1, -1, -1):
        sift(start, n - 1)
    for end in range(n - 1, 0, -1):
        a[lo], a[lo + end] = a[lo + end], a[lo]
        sift(0, end - 1)


def _ninther(a, lo, hi, idx):
    i0 = max(lo, idx - 1); i2 = min(hi, idx + 1)
    x, y, z = a[i0], a[idx], a[i2]
    if x > y: x, y = y, x
    if y > z: y, z = z, y
    if x > y: x, y = y, x
    return y


def _dual_partition(a, lo, hi, p1, p2):
    if p1 > p2: p1, p2 = p2, p1
    lt, gt, i = lo, hi, lo
    while i <= gt:
        v = a[i]
        if v < p1:
            a[lt], a[i] = a[i], a[lt]; lt += 1; i += 1
        elif v > p2:
            a[i], a[gt] = a[gt], a[i]; gt -= 1
        else:
            i += 1
    return lt, gt


def _sort(a, lo, hi, depth):
    while lo < hi:
        size = hi - lo + 1
        if size <= 48:
            _insertion_sort(a, lo, hi); return
        if depth <= 0:
            _heapsort(a, lo, hi); return
        if a[lo] <= a[lo + 1] <= a[lo + 2]:
            asc = True
            for i in range(lo, hi):
                if a[i] > a[i + 1]: asc = False; break
            if asc: return
        elif a[lo] >= a[lo + 1] >= a[lo + 2]:
            desc = True
            for i in range(lo, hi):
                if a[i] < a[i + 1]: desc = False; break
            if desc:
                l, r = lo, hi
                while l < r: a[l], a[r] = a[r], a[l]; l += 1; r -= 1
                return
        if isinstance(a[lo], int):
            mn = mx = a[lo]
            for i in range(lo + 1, hi + 1):
                v = a[i]
                if v < mn: mn = v
                elif v > mx: mx = v
            span = mx - mn
            if span < size * 4:
                counts = [0] * (span + 1)
                for i in range(lo, hi + 1):
                    counts[a[i] - mn] += 1
                k = lo
                for v, cnt in enumerate(counts):
                    if cnt == 1:
                        a[k] = v + mn; k += 1
                    elif cnt:
                        a[k:k + cnt] = [v + mn] * cnt; k += cnt
                return
        sp = hi - lo
        p1 = _ninther(a, lo, hi, lo + random.randrange(sp))
        p2 = _ninther(a, lo, hi, lo + random.randrange(sp))
        lt, gt = _dual_partition(a, lo, hi, p1, p2)
        r0 = (lt - lo, lo, lt - 1); r1 = (gt - lt + 1, lt, gt); r2 = (hi - gt, gt + 1, hi)
        if r0[0] > r1[0]: r0, r1 = r1, r0
        if r1[0] > r2[0]: r1, r2 = r2, r1
        if r0[0] > r1[0]: r0, r1 = r1, r0
        for _, rl, rh in (r0, r1):
            if rl < rh: _sort(a, rl, rh, depth - 1)
        lo, hi = r2[1], r2[2]; depth -= 1


def logos_sort(arr):
    """Return a new sorted list; the input is not modified."""
    n = len(arr)
    if n < 2:
        return list(arr)
    work = list(arr)
    _sort(work, 0, n - 1, 2 * int(math.log2(n)) + 4)
    return work
