"""Traffic metrics computed from a Run. All functions return tidy DataFrames.

Two kinds of "flit count" appear and are kept separate on purpose:
  * injected flits: each flit counted once, on the first channel of its packet
    (what the cores / chipset put into the network)
  * flit-hops: each flit counted on every channel it crosses
    (what the network actually carries; loads links)
"""
import numpy as np
import pandas as pd


def _bin(ev, bin_cycles):
    return (ev["cycle"] // bin_cycles) * bin_cycles


def total_over_time(run, bin_cycles):
    ev = run.events
    b = _bin(ev, bin_cycles)
    inj = ev[ev["is_first_channel"]].groupby([b[ev["is_first_channel"]], "noc"]).size()
    hops = ev[ev["link_kind"] == "mesh"].groupby([b[ev["link_kind"] == "mesh"], "noc"]).size()
    allch = ev.groupby([b, "noc"]).size()
    df = pd.concat({"injected_flits": inj, "mesh_flit_hops": hops,
                    "all_channel_flits": allch}, axis=1).fillna(0).astype(int)
    df.index.names = ["bin_start", "noc"]
    df = df.reset_index()
    return _fill_bins(df, ["noc"], bin_cycles, run)


def _fill_bins(df, keys, bin_cycles, run):
    """Add empty bins so idle periods show up as zeros, not gaps."""
    c = run.events["cycle"]
    if not len(c):
        return df
    bins = np.arange((c.min() // bin_cycles) * bin_cycles, c.max() + 1, bin_cycles)
    idx = pd.MultiIndex.from_product([bins] + [sorted(df[k].unique()) for k in keys],
                                     names=["bin_start"] + keys)
    return (df.set_index(["bin_start"] + keys).reindex(idx, fill_value=0).reset_index())


def per_source_over_time(run, bin_cycles):
    """Flits injected by each source (tile or offchip) per bin, per NoC."""
    ev = run.events[run.events["is_first_channel"]]
    g = ev.groupby([_bin(ev, bin_cycles), "noc", "src"]).size().rename("injected_flits")
    g.index.names = ["bin_start", "noc", "src"]
    return _fill_bins(g.reset_index(), ["noc", "src"], bin_cycles, run)


def per_dest_over_time(run, bin_cycles):
    """Flits addressed to each destination, counted when they enter the network."""
    ev = run.events[run.events["is_first_channel"]]
    g = ev.groupby([_bin(ev, bin_cycles), "noc", "dst"]).size().rename("flits")
    g.index.names = ["bin_start", "noc", "dst"]
    return _fill_bins(g.reset_index(), ["noc", "dst"], bin_cycles, run)


def per_path(run):
    """One row per (noc, src, dst): packets, flits, hops, latency."""
    p = run.packets
    g = p.groupby(["noc", "src", "dst"]).agg(
        packets=("pkt", "size"), flits=("flits", "sum"),
        mesh_hops=("mesh_hops", "median"),
        latency_mean=("latency", "mean"), latency_max=("latency", "max"),
        src_inferred=("src_inferred", "any"),
    ).reset_index()
    g["flit_hops"] = (p.assign(fh=p["flits"] * p["mesh_hops"])
                      .groupby(["noc", "src", "dst"])["fh"].sum().to_numpy())
    return g.sort_values("flits", ascending=False).reset_index(drop=True)


def per_path_over_time(run, bin_cycles, top=None):
    ev = run.events[run.events["is_first_channel"]]
    g = ev.groupby([_bin(ev, bin_cycles), "noc", "src", "dst"]).size().rename("injected_flits")
    g.index.names = ["bin_start", "noc", "src", "dst"]
    g = g.reset_index()
    if top:
        keep = (g.groupby(["noc", "src", "dst"])["injected_flits"].sum()
                .nlargest(top).index)
        g = g.set_index(["noc", "src", "dst"]).loc[keep].reset_index()
    return g


def per_link(run):
    return run.links.sort_values(["noc", "flits"], ascending=[True, False]).reset_index(drop=True)


def per_link_over_time(run, bin_cycles):
    ev = run.events
    g = ev.groupby([_bin(ev, bin_cycles), "noc", "link_name", "link_kind"]).size().rename("flits")
    g.index.names = ["bin_start", "noc", "link", "kind"]
    g = g.reset_index()
    g["utilization"] = g["flits"] / bin_cycles
    return g


def per_msg_type(run):
    p = run.packets
    return (p.groupby(["noc", "msg_type", "msg_name"])
            .agg(packets=("pkt", "size"), flits=("flits", "sum"),
                 latency_mean=("latency", "mean"))
            .reset_index().sort_values(["noc", "flits"], ascending=[True, False]))


def path_matrix(run, noc=None):
    """src x dst injected-flit matrix (all NoCs unless noc given)."""
    p = run.packets if noc is None else run.packets[run.packets["noc"] == noc]
    labels = [run.mesh.endpoint_label(run.mesh.tile(i % run.mesh.X, i // run.mesh.X))
              for i in range(run.mesh.NT)] + ["offchip"]
    m = p.pivot_table(index="src", columns="dst", values="flits", aggfunc="sum", fill_value=0)
    return m.reindex(index=labels, columns=labels, fill_value=0)


SIGNATURE_METRICS = {
    "injected": "packets the core's tile sent into the network",
    "ejected": "packets delivered to the core's tile",
    "router": "packets passing through the core's router (sent, received or forwarded)",
}


def signature(run, window, metric="injected"):
    """Per-core packet counts per time window: one 'application signature'
    time series per core, in the style of packets-per-window plots.

    Columns: core, window (0, 1, 2, ...), cycle_start, packets, flits.
    Every core gets every window (zeros included), so series line up across
    cores and across runs that use the same window size. Window 0 starts at
    the run's first traced cycle. Off-chip traffic is not a core and is left
    out. In OpenPiton each tile has one core, so core N = tile N.
    """
    m = run.mesh
    if metric not in SIGNATURE_METRICS:
        raise ValueError(f"metric must be one of {list(SIGNATURE_METRICS)}")
    p = run.packets
    if metric == "injected":
        rows = p[p["src_node"].between(m.NT, 2 * m.NT - 1)]
        core = rows["src_node"] - m.NT
        cyc = rows["inject_cycle"]
        flits = rows["flits"]
    elif metric == "ejected":
        rows = p[p["dst_node"].between(m.NT, 2 * m.NT - 1) & (p["eject_cycle"] >= 0)]
        core = rows["dst_node"] - m.NT
        cyc = rows["eject_cycle"]
        flits = rows["flits"]
    else:
        # every packet leaves each router it passes through exactly once
        h = run.hops[run.hops["from_node"] < m.NT]
        core = h["from_node"]
        cyc = h["head_cycle"]
        flits = h["nflits"]
    t0 = int(run.events["cycle"].min()) if len(run.events) else 0
    t1 = int(run.events["cycle"].max()) if len(run.events) else 0
    nwin = (t1 - t0) // window + 1
    df = pd.DataFrame({"core": core.to_numpy().astype(int),
                       "window": ((cyc.to_numpy() - t0) // window).astype(int),
                       "flits": flits.to_numpy()})
    g = df.groupby(["core", "window"]).agg(packets=("flits", "size"), flits=("flits", "sum"))
    idx = pd.MultiIndex.from_product([range(m.NT), range(nwin)], names=["core", "window"])
    g = g.reindex(idx, fill_value=0).reset_index()
    g.insert(2, "cycle_start", t0 + g["window"] * window)
    return g


def summary(run):
    ev, p = run.events, run.packets
    span = run.cycle_span()
    out = {
        "file": run.meta.get("file"),
        "format": run.meta.get("format"),
        "mesh": f"{run.mesh.X}x{run.mesh.Y}",
        "first_cycle": int(ev["cycle"].min()) if len(ev) else None,
        "last_cycle": int(ev["cycle"].max()) if len(ev) else None,
        "cycles": span,
        "packets": int(len(p)),
        "injected_flits": int(p["flits"].sum()),
        "mesh_flit_hops": int((ev["link_kind"] == "mesh").sum()),
        "injection_rate_flits_per_cycle": round(p["flits"].sum() / max(span, 1), 5),
        "per_noc": {},
        "warnings": list(run.meta.get("warnings", [])),
    }
    for noc, q in p.groupby("noc"):
        lat = q["latency"].dropna()
        out["per_noc"][int(noc)] = {
            "packets": int(len(q)), "flits": int(q["flits"].sum()),
            "avg_mesh_hops": round(float(q["mesh_hops"].mean()), 3),
            "latency_mean": round(float(lat.mean()), 2) if len(lat) else None,
            "latency_p95": round(float(lat.quantile(0.95)), 2) if len(lat) else None,
            "latency_max": int(lat.max()) if len(lat) else None,
        }
    if run.meta.get("clock_period"):
        out["clock_period"] = run.meta["clock_period"]
    links = run.links
    mesh_links = links[links["kind"] == "mesh"]
    if len(mesh_links):
        top = mesh_links.nlargest(1, "flits").iloc[0]
        out["busiest_mesh_link"] = {"noc": int(top["noc"]), "from": top["from"],
                                    "to": top["to"], "flits": int(top["flits"]),
                                    "utilization": round(float(top["utilization"]), 4)}
    return out
