"""Files that [kvx/table] reads, written from Python with no extra packages.

    pdkv.save_csv("data.csv", {"time": times, "x": xs})      # times as datetime, ISO text or numbers
    pdkv.save_npy("data.npy", {"x": xs, "y": ys})             # fast to read, float64 columns
    pdkv.from_obspy(stream, "quake.csv")                       # ObsPy Stream or Trace to a CSV with a time column
"""
import csv
import datetime as _dt
import math
import struct

__all__ = ["save_csv", "save_npy", "from_obspy"]


def _columns(columns):
    """dict, or list of (name, values) pairs -> list of (name, list)"""
    items = list(columns.items()) if isinstance(columns, dict) else list(columns)
    if not items:
        raise ValueError("no columns")
    out = [(str(name), list(values)) for name, values in items]
    n = len(out[0][1])
    for name, values in out:
        if len(values) != n:
            raise ValueError("column %r has %d values, expected %d" % (name, len(values), n))
    return out


def _iso(t):
    if isinstance(t, _dt.datetime):
        if t.tzinfo is not None:
            t = t.astimezone(_dt.timezone.utc).replace(tzinfo=None)
        s = t.strftime("%Y-%m-%dT%H:%M:%S")
        if t.microsecond:
            s += (".%06d" % t.microsecond).rstrip("0")
        return s + "Z"
    return t


def _cell(v):
    if v is None:
        return ""
    if isinstance(v, (_dt.datetime, _dt.date)) and not isinstance(v, _dt.datetime):
        return v.isoformat()
    if isinstance(v, _dt.datetime):
        return _iso(v)
    if isinstance(v, float):
        return "" if math.isnan(v) else repr(v)
    if hasattr(v, "item"):          # numpy scalars
        return _cell(v.item())
    return v


def save_csv(path, columns):
    """Write columns to a CSV file for [kvx/table]. Dates (datetime objects)
    become ISO text, NaN and None become empty cells."""
    cols = _columns(columns)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([name for name, _ in cols])
        for i in range(len(cols[0][1])):
            w.writerow([_cell(values[i]) for _, values in cols])
    return path


def save_npy(path, columns):
    """Write numeric columns as a 2-D float64 .npy file (no numpy needed).
    Column names are not stored: [kvx/table] calls them col1, col2 ..."""
    cols = _columns(columns)
    n, m = len(cols[0][1]), len(cols)
    header = "{'descr': '<f8', 'fortran_order': False, 'shape': (%d, %d), }" % (n, m)
    pad = 64 - ((10 + len(header) + 1) % 64)
    header = header + " " * (pad % 64) + "\n"
    with open(path, "wb") as f:
        f.write(b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header)) + header.encode("latin1"))
        for i in range(n):
            f.write(struct.pack("<%dd" % m, *[float(values[i]) if values[i] is not None else float("nan")
                                             for _, values in cols]))
    return path


def _timestamp(t):
    if hasattr(t, "timestamp") and not callable(t.timestamp):
        return float(t.timestamp)          # obspy UTCDateTime
    if isinstance(t, _dt.datetime):
        return t.replace(tzinfo=_dt.timezone.utc).timestamp() if t.tzinfo is None else t.timestamp()
    return float(t)


def from_obspy(data, path, decimate=1):
    """Write an ObsPy Stream (or one Trace) to a file [kvx/table] reads.
    One column per trace (named like the trace id, e.g. IU.ANMO.00.BHZ) plus a
    time column from the first trace. Traces are cut to the shortest one.
    decimate keeps every nth sample. Ends in .npy -> numbers only, no time."""
    traces = list(data) if not hasattr(data, "stats") else [data]
    if not traces:
        raise ValueError("no traces")
    step = max(1, int(decimate))
    n = min(len(tr.data) for tr in traces)
    t0 = _timestamp(traces[0].stats.starttime)
    dt = float(traces[0].stats.delta)
    idx = range(0, n, step)
    cols = []
    if not str(path).lower().endswith(".npy"):
        cols.append(("time", [_dt.datetime(1970, 1, 1) + _dt.timedelta(seconds=t0 + i * dt) for i in idx]))
    used = set()
    for tr in traces:
        name = getattr(tr, "id", None) or "%s.%s" % (tr.stats.station, tr.stats.channel)
        while name in used:
            name += "_"
        used.add(name)
        cols.append((name, [float(tr.data[i]) for i in idx]))
    if str(path).lower().endswith(".npy"):
        return save_npy(path, cols)
    return save_csv(path, cols)
