"""The benchmark harness: warm + median timing, separate ingest, verification."""
import os
import time
from dataclasses import dataclass, field
from typing import Optional, List

import numpy as np

from .datasets import generate
from .contenders import build_contenders, DuckdbContender


def _core_count():
    try:
        return len(os.sched_getaffinity(0))
    except AttributeError:  # pragma: no cover - non-Linux
        return os.cpu_count() or 1


def _verify(out, reference):
    if out is None:
        return None
    out = np.asarray(out)
    if out.shape[0] != reference.shape[0]:
        return False
    return bool(np.array_equal(out, reference))


@dataclass
class Row:
    key: str
    name: str
    kind: str
    sort_ms: Optional[float] = None
    ingest_ms: float = 0.0
    correct: Optional[bool] = None
    skipped: Optional[str] = None


@dataclass
class Result:
    n: int
    dtype: str
    distribution: str
    repeats: int
    cores: int
    rows: List[Row] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    def ranked(self):
        done = [r for r in self.rows if r.sort_ms is not None]
        return sorted(done, key=lambda r: r.sort_ms)

    def fastest(self):
        r = self.ranked()
        return r[0] if r else None

    def to_dict(self):
        return {
            "n": self.n, "dtype": self.dtype, "distribution": self.distribution,
            "repeats": self.repeats, "cores": self.cores,
            "results": [vars(r) for r in self.rows], "notes": list(self.notes),
        }

    def format_table(self):
        header = (f"sortbench  n={self.n:,}  dtype={self.dtype}  "
                  f"distribution={self.distribution}  repeats={self.repeats}  "
                  f"cores={self.cores}")
        lines = [header, "=" * len(header)]
        ranked = self.ranked()
        best = ranked[0].sort_ms if ranked else None
        colw = max((len(r.name) for r in self.rows), default=12)
        lines.append(f"  {'engine'.ljust(colw)}   {'sort':>10}   {'vs best':>8}   "
                     f"{'ingest':>9}   {'kind':>8}   ok")
        for r in ranked:
            rel = f"{r.sort_ms / best:.1f}x" if best else "-"
            ing = f"{r.ingest_ms:.0f}ms" if r.ingest_ms else "-"
            ok = "ok" if r.correct else ("FAIL" if r.correct is False else "?")
            lines.append(f"  {r.name.ljust(colw)}   {r.sort_ms:8.1f}ms   {rel:>8}   "
                         f"{ing:>9}   {r.kind:>8}   {ok}")
        for r in self.rows:
            if r.sort_ms is None:
                lines.append(f"  {r.name.ljust(colw)}   {'skipped':>10}   "
                             f"({r.skipped})")
        for note in self.notes:
            lines.append("  note: " + note)
        return "\n".join(lines)

    def print_table(self):
        print(self.format_table())


def benchmark(n, dtype="int32", distribution="uniform", contenders=None,
              repeats=5, warmup=1, seed=7, verify=True, duckdb_threads=None,
              extra=None):
    """Benchmark sort contenders on one dataset and return a :class:`Result`.

    Parameters
    ----------
    n, dtype, distribution, seed : dataset specification (see ``datasets.generate``).
    contenders : list of keys (``numpy``, ``builtin``, ``logos``, ``sqlite``,
        ``duckdb``) or ``None`` for all.
    repeats : timed runs per contender; the median is reported.
    warmup : untimed runs before timing (JIT / cache warm).
    verify : if True, fetch each result (untimed) and check it matches a
        reference sort exactly.
    duckdb_threads : pass an int to force DuckDB's thread count.
    extra : optional ``{name: callable}`` of custom sorts. Each callable takes a
        numpy array and returns the sorted array; timed like an in-memory sort.
    """
    data = generate(n, dtype, distribution, seed)
    reference = np.sort(data)
    chosen = build_contenders(contenders, duckdb_threads)
    cores = _core_count()
    result = Result(n=n, dtype=dtype, distribution=distribution,
                    repeats=repeats, cores=cores)

    def time_contender(c):
        if not c.available():
            result.rows.append(Row(c.key, c.name, c.kind, skipped="not installed"))
            return
        if not c.supports(dtype):
            result.rows.append(Row(c.key, c.name, c.kind, skipped=f"no {dtype}"))
            return
        state, ingest = c.prepare(data)
        try:
            for _ in range(warmup):
                res = c.sort(state)
            times = []
            for _ in range(repeats):
                t0 = time.perf_counter()
                res = c.sort(state)
                times.append(time.perf_counter() - t0)
            times.sort()
            med = times[len(times) // 2]
            correct = _verify(c.result_array(state, res), reference) if verify else None
        finally:
            c.cleanup(state)
        result.rows.append(Row(c.key, c.name, c.kind, sort_ms=med * 1000.0,
                               ingest_ms=ingest * 1000.0, correct=correct))

    for c in chosen:
        time_contender(c)

    # custom user-supplied sorts (treated as in-memory contenders)
    for cname, fn in (extra or {}).items():
        master = data.copy()
        for _ in range(warmup):
            res = fn(master.copy())
        times = []
        for _ in range(repeats):
            arr = master.copy()
            t0 = time.perf_counter()
            res = fn(arr)
            times.append(time.perf_counter() - t0)
        times.sort()
        correct = _verify(res, reference) if verify else None
        result.rows.append(Row("custom:" + cname, cname, "custom",
                               sort_ms=times[len(times) // 2] * 1000.0,
                               correct=correct))

    if cores == 1:
        result.notes.append(
            "single core: DuckDB cannot parallelize ORDER BY here; its real-world "
            "edge (multi-core, out-of-core, indexes, persistence) is not exercised.")
    dk = DuckdbContender(duckdb_threads)
    if dk.available():
        result.notes.append(f"DuckDB threads = {dk.thread_setting()}")
    result.notes.append("sort timed on resident data; ingest shown separately; "
                        "databases materialize the sort in-engine (CREATE TABLE AS).")
    return result


def sweep(n, dtype="int32", distributions=None, **kwargs):
    """Run :func:`benchmark` across several distributions; return ``{dist: Result}``."""
    from .datasets import DISTRIBUTIONS
    distributions = distributions or list(DISTRIBUTIONS)
    return {d: benchmark(n, dtype=dtype, distribution=d, **kwargs) for d in distributions}
