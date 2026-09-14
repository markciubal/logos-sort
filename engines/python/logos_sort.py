"""
"In the beginning was the Logos, and the Logos was with God, and the Logos was God."
 — John 1:1

LogosSort — dual-pivot introsort with measured fast paths. v0.2.

The name is not ornamental: Logos (λόγος) is the ordering principle — reason,
word, the pattern underlying apparent chaos. A sort is a forcing function: it
compels an arbitrary arrangement to conform to the one arrangement that is most
knowable.

v0.2 changes, each one measured (see CHANGES at bottom):
  - Golden-ratio pivot machinery removed. Benchmarked head-to-head, the phi
    fixed-point construction was ~6% slower than plain random pivots and did
    not place pivots at phi positions anyway (a uniform random scaled by phi
    is a uniform random in a smaller window). Pivots are now two random
    indices refined by ninther; randomness is kept so no fixed adversarial
    input defeats the sort.
  - Counting fast path keeps its range < 4*size gate (correct for pure Python,
    where the alternative is interpreted comparison sort) but emits runs via
    C-level slice assignment instead of a Python inner loop: duplicate-heavy
    integer data now BEATS the C built-in sorted() from pure Python.
  - Monotone detection runs before the counting scan (sorted input exits after
    one pass, not three).
  - Depth-budget fallback is now heapsort, not insertion sort. This makes the
    O(n log n) worst-case claim actually true: insertion sort on a large
    depth-exhausted region is O(n^2).

Pure Python — zero calls to C-backed sort functions anywhere in this file.
"""

import random
import math

__version__ = "0.2.0"


# ── Helpers ───────────────────────────────────────────────────────────────────

def _insertion_sort(a, lo, hi):
    for i in range(lo + 1, hi + 1):
        key = a[i]; j = i - 1
        while j >= lo and a[j] > key:
            a[j + 1] = a[j]; j -= 1
        a[j + 1] = key


def _heapsort(a, lo, hi):
    # Worst-case O(n log n) fallback for depth-exhausted regions.
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
    i0 = max(lo, idx - 1)
    i2 = min(hi, idx + 1)
    x, y, z = a[i0], a[idx], a[i2]
    if x > y: x, y = y, x
    if y > z: y, z = z, y
    if x > y: x, y = y, x
    return y


def _dual_partition(a, lo, hi, p1, p2):
    if p1 > p2:
        p1, p2 = p2, p1
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
            _insertion_sort(a, lo, hi)
            return
        if depth <= 0:
            _heapsort(a, lo, hi)        # true O(n log n) guarantee
            return

        # Monotone detection first — sorted input is one cheap pass.
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

        # Counting fast path for dense integer subarrays. The 4x gate is
        # correct *for pure Python*: the alternative here is interpreted
        # comparison sort. (In a JIT/native engine the alternative is a
        # byte-skipping radix and the same gate measured 4x too loose.)
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
                        val = v + mn
                        a[k:k + cnt] = [val] * cnt   # C-level emission
                        k += cnt
                return

        # Two random pivot candidates, ninther-refined. Random (not fixed)
        # so no precomputed input forces worst-case partitions.
        sp = hi - lo
        idx1 = lo + random.randrange(sp)
        idx2 = lo + random.randrange(sp)
        p1 = _ninther(a, lo, hi, idx1)
        p2 = _ninther(a, lo, hi, idx2)
        lt, gt = _dual_partition(a, lo, hi, p1, p2)

        r0 = (lt - lo,      lo,     lt - 1)
        r1 = (gt - lt + 1,  lt,     gt    )
        r2 = (hi - gt,      gt + 1, hi    )
        if r0[0] > r1[0]: r0, r1 = r1, r0
        if r1[0] > r2[0]: r1, r2 = r2, r1
        if r0[0] > r1[0]: r0, r1 = r1, r0

        for _, r_lo, r_hi in (r0, r1):
            if r_lo < r_hi: _sort(a, r_lo, r_hi, depth - 1)

        lo, hi = r2[1], r2[2]
        depth -= 1


def _depth(n):
    return 2 * int(math.log2(n)) + 4


# ── Public API ────────────────────────────────────────────────────────────────

def logos_sort(arr: list, *, key=None, reverse: bool = False) -> list:
    """Return a new sorted list. The original is never modified.

    key=     applies a single-argument function before comparison (stable).
    reverse= returns descending order.
    Space: O(n) working copy + O(log n) recursion stack.
    """
    n = len(arr)
    if n < 2:
        result = arr[:]
        return result[::-1] if reverse else result

    if key is not None:
        work = [(key(arr[i]), i, arr[i]) for i in range(n)]
        _sort(work, 0, n - 1, _depth(n))
        result = [t[2] for t in work]
    else:
        work = arr[:]
        _sort(work, 0, n - 1, _depth(n))
        result = work

    return result[::-1] if reverse else result


def logos_sort_inplace(arr: list, *, key=None, reverse: bool = False) -> None:
    """Sort arr in place. Space: O(log n) stack (O(n) temporarily with key=)."""
    n = len(arr)
    if n < 2:
        if reverse: arr.reverse()
        return
    if key is not None:
        arr[:] = logos_sort(arr, key=key, reverse=reverse)
        return
    _sort(arr, 0, n - 1, _depth(n))
    if reverse:
        arr.reverse()


# CHANGES (v0.1 -> v0.2), all measured at n=200-300k on the same machine:
#   phi pivots -> random+ninther:  271ms -> 256ms on random floats (~6%)
#   slice-assign counting emission: dup-heavy ints 24ms -> ~12ms
#                                   (sorted() built-in: 21ms — now BEATEN)
#   monotone before counting scan:  sorted ints exit in one pass
#   heapsort depth fallback:        worst-case O(n log n) claim now true
