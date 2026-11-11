"""Readers that turn simulator output into the canonical flit-event table.

Canonical flit events (one row per flit seen on one router port):

    cycle   int    clock cycle the flit crossed the port
    time    float  simulator time if the source has it (else NaN)
    noc     int    physical network (OpenPiton: 1, 2, 3)
    x, y    int    router where it was observed
    dir     str    'in' (entering the router) or 'out' (leaving it)
    port    str    N / E / S / W (mesh links) or P (the tile's own port)
    flit    uint64 raw 64-bit flit

Plus a meta dict: x_tiles, y_tiles, source, clock_period, chip_id, has_p_ports...

To support another simulator, write one function that returns
(DataFrame with those columns, meta) and register it in ADAPTERS. Nothing
downstream needs to change.
"""
import io
import math
import os
import re
from functools import reduce

import numpy as np
import pandas as pd

CANON_COLS = ["cycle", "time", "noc", "x", "y", "dir", "port", "flit"]


def _open_text(path):
    if path.endswith(".gz"):
        import gzip
        return gzip.open(path, "rt", errors="replace")
    return open(path, "r", errors="replace")


def _finish(df):
    df["cycle"] = df["cycle"].astype(np.int64)
    df["noc"] = df["noc"].astype(np.int16)
    df["x"] = df["x"].astype(np.int16)
    df["y"] = df["y"].astype(np.int16)
    df["dir"] = df["dir"].astype(str)
    df["port"] = df["port"].astype(str)
    df["flit"] = df["flit"].astype(np.uint64)
    df = df.reset_index(drop=True)
    df["seq"] = np.arange(len(df), dtype=np.int64)  # file order, tie-breaker
    return df


# --------------------------------------------------------------------------
# 1) noc_trace CSV (our noc_trace_mon.v), or any CSV with the canonical columns
# --------------------------------------------------------------------------
def sniff_noc_trace(path, head):
    if "# noc_trace" in head:
        return True
    first = [l for l in head.splitlines() if l and not l.startswith("#")][:1]
    return bool(first) and all(c in first[0].split(",") for c in
                               ["cycle", "noc", "x", "y", "dir", "port", "flit"])


def read_noc_trace(path, opts):
    meta = {"source": "noc_trace", "has_p_ports": True, "has_offchip_in": True}
    with _open_text(path) as f:
        comment = []
        for line in f:
            if not line.startswith("#"):
                break
            comment.append(line)
    for line in comment:
        for k, v in re.findall(r"(\w+)=(\S+)", line):
            meta[k] = int(v) if v.isdigit() else v
    df = pd.read_csv(path, comment="#", dtype={"flit": str, "dir": str, "port": str})
    if "time" not in df.columns:
        df["time"] = np.nan
    df["flit"] = [int(s, 16) for s in df["flit"]]
    df["dir"] = df["dir"].str.strip().str.lower()
    df["port"] = df["port"].str.strip().str.upper()
    return _finish(df[CANON_COLS].copy()), meta


# --------------------------------------------------------------------------
# 2) OpenPiton sims.log with the built-in router monitor (+enable_router_mon)
#    line:  "   32500: TILE0 noc1 router out E data: 0x0000040400c7c000 "
#    Logs only flits leaving routers on N/S/E/W. No P ports, nothing coming
#    in from the chipset.
# --------------------------------------------------------------------------
ROUTER_MON_RE = re.compile(
    r"^\s*(\d+)\s*:\s*TILE(\d+)\s+noc(\d)\s+router\s+(out|in)\s+([NSEWP])\s+data:\s*0x([0-9a-fA-F]+)")
XT_RE = re.compile(r"x_tiles\s*=\s*(\d+)")
YT_RE = re.compile(r"y_tiles\s*=\s*(\d+)")


def sniff_router_mon(path, head):
    # the router lines may start far into a long sims.log, so also accept *.log
    return (any(ROUTER_MON_RE.match(l) for l in head.splitlines())
            or path.endswith((".log", ".log.gz")))


def read_router_mon(path, opts):
    meta = {"source": "openpiton_router_mon", "has_p_ports": False, "has_offchip_in": False}
    rows = []
    xt = yt = None
    with _open_text(path) as f:
        for line in f:
            m = ROUTER_MON_RE.match(line)
            if m:
                rows.append(m.groups())
                continue
            if xt is None:
                mx = XT_RE.search(line)
                if mx:
                    xt = int(mx.group(1))
            if yt is None:
                my = YT_RE.search(line)
                if my:
                    yt = int(my.group(1))
    if not rows:
        raise ValueError(
            f"{path}: no router-monitor lines found. Rerun the sim with "
            "-sim_run_args=+enable_router_mon (and a build without "
            "-config_rtl=MINIMAL_MONITORING), or use the noc_trace monitor.")
    xt = opts.get("x_tiles") or xt
    yt = opts.get("y_tiles") or yt
    if not xt:
        raise ValueError(f"{path}: can't tell the mesh width. Pass --x-tiles N (and --y-tiles N).")
    t = np.array([int(r[0]) for r in rows], dtype=np.int64)
    tile = np.array([int(r[1]) for r in rows], dtype=np.int64)
    df = pd.DataFrame({
        "time": t.astype(float),
        "noc": [int(r[2]) for r in rows],
        "x": tile % xt,
        "y": tile // xt,
        "dir": [r[3] for r in rows],
        "port": [r[4] for r in rows],
        "flit": [int(r[5], 16) for r in rows],
    })
    if not yt:
        yt = int(tile.max() // xt + 1)
    period = opts.get("clock_period") or infer_period(t)
    t0 = opts.get("time_zero", 0)
    df["cycle"] = np.floor((t - t0) / period).astype(np.int64)
    meta.update(x_tiles=int(xt), y_tiles=int(yt), clock_period=period)
    return _finish(df[CANON_COLS].copy()), meta


def infer_period(times):
    """Clock period = GCD of the gaps between distinct sample times (all
    samples sit on the same clock edge, so every gap is a whole number of
    periods)."""
    u = np.unique(times)
    if len(u) < 2:
        return 1
    d = np.diff(u)
    d = d[d > 0][:200000]
    g = int(reduce(math.gcd, (int(v) for v in np.unique(d))))
    return max(g, 1)


ADAPTERS = {
    "noc_trace": (sniff_noc_trace, read_noc_trace),
    "router_mon": (sniff_router_mon, read_router_mon),
}


def load_events(path, fmt="auto", **opts):
    """Read one run's output file into (events DataFrame, meta)."""
    if fmt == "auto":
        with _open_text(path) as f:
            head = f.read(20000)
        for name, (sniff, _) in ADAPTERS.items():
            if sniff(path, head):
                fmt = name
                break
        else:
            raise ValueError(f"{path}: unrecognised format; pass --format")
    df, meta = ADAPTERS[fmt][1](path, opts)
    for k in ("x_tiles", "y_tiles"):
        if opts.get(k):
            meta[k] = opts[k]
    if not meta.get("x_tiles") or not meta.get("y_tiles"):
        raise ValueError(f"{path}: mesh size unknown; pass --x-tiles and --y-tiles")
    meta.setdefault("chip_id", opts.get("chip_id", 0))
    meta["file"] = os.path.abspath(path)
    meta["format"] = fmt
    return df, meta
