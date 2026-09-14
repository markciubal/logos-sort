"""Sort contenders.

Every contender implements the same small protocol so the timing harness can
treat them identically and fairly:

  prepare(data)      -> (state, ingest_seconds)
        Put the data into the contender's *native* form. For numpy that is a
        no-op; for the list-based sorts it is ``tolist()``; for a database it is
        the table insert. The time to do this is reported separately as
        "ingest" and is NOT part of the measured sort.

  sort(state)        -> result_handle        [THIS is what gets timed]
        Produce a fully sorted result from the already-native state. Must be
        repeatable (called once to warm, then several times for the median),
        so it never consumes or mutates ``state`` destructively.

  result_array(state, result) -> np.ndarray  [untimed, for verification only]

  cleanup(state)
        Release any resources (e.g. close a database connection).

Timing only ``sort`` on already-native data is the crux of a fair comparison:
it measures the sort kernel, not data marshalling. A database that must first
ingest rows pays that cost once, visibly, in a separate column.
"""
import time
import sqlite3
import numpy as np

from ._logos import logos_sort

try:
    import duckdb
    _HAS_DUCKDB = True
except Exception:  # pragma: no cover - optional dependency
    _HAS_DUCKDB = False


class Contender:
    key = "base"
    name = "base"
    kind = "memory"  # "memory" or "database"

    def available(self):
        return True

    def supports(self, dtype):
        return True

    def prepare(self, data):
        return data, 0.0

    def sort(self, state):  # pragma: no cover - abstract
        raise NotImplementedError

    def result_array(self, state, result):
        return np.asarray(result)

    def cleanup(self, state):
        pass


# ---- in-memory contenders ----

class NumpyContender(Contender):
    key, name = "numpy", "numpy.sort"

    def sort(self, state):
        return np.sort(state)

    def result_array(self, state, result):
        return result


class _ListContender(Contender):
    """Base for sorts that operate on a Python list; ingest = tolist()."""
    def prepare(self, data):
        t0 = time.perf_counter()
        lst = data.tolist()
        return lst, time.perf_counter() - t0

    def result_array(self, state, result):
        return np.asarray(result)


class BuiltinContender(_ListContender):
    key, name = "builtin", "builtin sorted()"

    def sort(self, state):
        return sorted(state)


class LogosContender(_ListContender):
    key, name = "logos", "logos (pure-python)"

    def sort(self, state):
        return logos_sort(state)


# ---- database contenders ----

class SqliteContender(Contender):
    key, name, kind = "sqlite", "SQLite ORDER BY", "database"

    def prepare(self, data):
        con = sqlite3.connect(":memory:")
        con.execute("CREATE TABLE t(x)")
        t0 = time.perf_counter()
        con.executemany("INSERT INTO t VALUES (?)", ((v,) for v in data.tolist()))
        ingest = time.perf_counter() - t0
        return {"con": con}, ingest

    def sort(self, state):
        con = state["con"]
        con.execute("DROP TABLE IF EXISTS s")
        con.execute("CREATE TABLE s AS SELECT x FROM t ORDER BY x")
        return "s"

    def result_array(self, state, result):
        rows = state["con"].execute("SELECT x FROM s").fetchall()
        return np.array([r[0] for r in rows])

    def cleanup(self, state):
        state["con"].close()


class DuckdbContender(Contender):
    key, name, kind = "duckdb", "DuckDB ORDER BY", "database"

    def __init__(self, threads=None):
        self.threads = threads

    def available(self):
        return _HAS_DUCKDB

    def prepare(self, data):
        con = duckdb.connect()
        if self.threads is not None:
            con.execute(f"PRAGMA threads={int(self.threads)}")
        con.register("v", {"x": data})
        t0 = time.perf_counter()
        con.execute("CREATE TABLE t AS SELECT x FROM v")
        ingest = time.perf_counter() - t0
        return {"con": con}, ingest

    def sort(self, state):
        con = state["con"]
        con.execute("DROP TABLE IF EXISTS s")
        con.execute("CREATE TABLE s AS SELECT x FROM t ORDER BY x")
        return "s"

    def result_array(self, state, result):
        return state["con"].execute("SELECT x FROM s").fetchnumpy()["x"]

    def cleanup(self, state):
        state["con"].close()

    def thread_setting(self):
        if not self.available():
            return None
        con = duckdb.connect()
        try:
            if self.threads is not None:
                con.execute(f"PRAGMA threads={int(self.threads)}")
            return con.execute("SELECT current_setting('threads')").fetchone()[0]
        finally:
            con.close()


def build_contenders(keys=None, duckdb_threads=None):
    """Return contender instances for the requested keys (default: all)."""
    registry = [
        NumpyContender(),
        BuiltinContender(),
        LogosContender(),
        SqliteContender(),
        DuckdbContender(duckdb_threads),
    ]
    if keys is None:
        return registry
    by_key = {c.key: c for c in registry}
    unknown = [k for k in keys if k not in by_key]
    if unknown:
        raise ValueError(f"unknown contender(s): {unknown}; known: {sorted(by_key)}")
    return [by_key[k] for k in keys]


def list_contenders():
    """Return ``[(key, name, kind, available)]`` for all known contenders."""
    return [(c.key, c.name, c.kind, c.available()) for c in build_contenders()]
