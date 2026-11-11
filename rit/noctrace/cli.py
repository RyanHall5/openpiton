"""noctrace command line.

    python -m noctrace RUN_FILE [RUN_FILE ...] [options]

Each RUN_FILE is one simulation's output (a noc_trace.csv from noc_trace_mon,
or a sims.log run with +enable_router_mon). Results go to OUT/<run name>/.
With several runs, OUT/runs_summary.csv compares them side by side.
"""
import argparse
import json
import os
import sys

import pandas as pd

from . import load_run, metrics


def parse_args(argv):
    ap = argparse.ArgumentParser(prog="noctrace", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="simulation output files")
    ap.add_argument("-o", "--out", default="noc_analysis", help="output directory")
    ap.add_argument("--format", default="auto", choices=["auto", "noc_trace", "router_mon"])
    ap.add_argument("--bin", type=int, default=0,
                    help="time-bin width in cycles (default: run length / 100)")
    ap.add_argument("--x-tiles", type=int, help="mesh width (if the file doesn't say)")
    ap.add_argument("--y-tiles", type=int, help="mesh height")
    ap.add_argument("--clock-period", type=int,
                    help="sim time units per cycle for sims.log (default: inferred)")
    ap.add_argument("--chip-id", type=int, default=0, help="this chip's id (default 0)")
    ap.add_argument("--noc", default="", help="only these NoCs, e.g. 1,3")
    ap.add_argument("--start", type=int, help="ignore flits before this cycle")
    ap.add_argument("--end", type=int, help="ignore flits after this cycle")
    ap.add_argument("--name", action="append", help="run name (repeat once per file)")
    ap.add_argument("--no-plots", action="store_true")
    ap.add_argument("--export-events", action="store_true",
                    help="also write the full per-flit table (large)")
    return ap.parse_args(argv)


def analyze_one(path, name, a):
    from .adapters import load_events
    from .model import Run
    ev, meta = load_events(path, a.format, x_tiles=a.x_tiles, y_tiles=a.y_tiles,
                           clock_period=a.clock_period, chip_id=a.chip_id)
    if a.noc:
        ev = ev[ev["noc"].isin([int(n) for n in a.noc.split(",")])]
    if a.start is not None:
        ev = ev[ev["cycle"] >= a.start]
    if a.end is not None:
        ev = ev[ev["cycle"] <= a.end]
    if ev.empty:
        raise SystemExit(f"{path}: no flits left after filtering")
    run = Run(ev, meta)
    span = run.cycle_span()
    b = a.bin or max(1, round(span / 100))

    out = os.path.join(a.out, name)
    os.makedirs(out, exist_ok=True)
    w = lambda df, f: df.to_csv(os.path.join(out, f), index=False)  # noqa: E731
    w(metrics.total_over_time(run, b), "total_over_time.csv")
    w(metrics.per_source_over_time(run, b), "per_source_over_time.csv")
    w(metrics.per_dest_over_time(run, b), "per_dest_over_time.csv")
    w(metrics.per_path(run), "per_path.csv")
    w(metrics.per_path_over_time(run, b), "per_path_over_time.csv")
    w(metrics.per_link(run), "per_link.csv")
    w(metrics.per_link_over_time(run, b), "per_link_over_time.csv")
    w(metrics.per_msg_type(run), "per_msg_type.csv")
    metrics.path_matrix(run).to_csv(os.path.join(out, "path_matrix.csv"))
    w(run.packets, "packets.csv")
    if a.export_events:
        cols = ["cycle", "noc", "x", "y", "dir", "port", "link_name", "link_kind",
                "pkt", "src", "dst", "msg_name"]
        e = run.events[cols].copy()
        e.insert(len(cols), "flit", [f"{int(v):016x}" for v in run.events["flit"]])
        e.to_csv(os.path.join(out, "events.csv.gz"), index=False)
    s = metrics.summary(run)
    s["bin_cycles"] = b
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(s, f, indent=2)
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write(format_summary(s))
    if not a.no_plots:
        from . import plots
        plots.all_plots(run, b, out)
    return s, out


def format_summary(s):
    L = [f"run:      {s['file']}",
         f"format:   {s['format']}   mesh {s['mesh']}   cycles {s['first_cycle']}..{s['last_cycle']}"
         f" ({s['cycles']})   bin {s.get('bin_cycles')}",
         f"packets:  {s['packets']:,}   injected flits {s['injected_flits']:,}   "
         f"router-to-router flit-hops {s['mesh_flit_hops']:,}",
         f"load:     {s['injection_rate_flits_per_cycle']} injected flits/cycle (whole chip)"]
    for noc, d in s["per_noc"].items():
        lat = (f"latency mean {d['latency_mean']}  p95 {d['latency_p95']}  max {d['latency_max']}"
               if d["latency_mean"] is not None else "latency n/a")
        L.append(f"  NoC{noc}: {d['packets']:,} pkts  {d['flits']:,} flits  "
                 f"avg {d['avg_mesh_hops']} hops  {lat}")
    if "busiest_mesh_link" in s:
        b = s["busiest_mesh_link"]
        L.append(f"busiest link: NoC{b['noc']} {b['from']} -> {b['to']}  {b['flits']:,} flits"
                 f"  ({b['utilization']} flits/cycle)")
    for x in s["warnings"]:
        L.append(f"WARNING: {x}")
    return "\n".join(L) + "\n"


def main(argv=None):
    a = parse_args(sys.argv[1:] if argv is None else argv)
    names = a.name or []
    rows = []
    for i, f in enumerate(a.files):
        name = names[i] if i < len(names) else _default_name(f, a.files)
        s, out = analyze_one(f, name, a)
        print(f"== {name}  ->  {out}")
        print(format_summary(s))
        flat = {"run": name, **{k: v for k, v in s.items() if not isinstance(v, (dict, list))}}
        for noc, d in s["per_noc"].items():
            for k, v in d.items():
                flat[f"noc{noc}_{k}"] = v
        rows.append(flat)
    if len(rows) > 1:
        p = os.path.join(a.out, "runs_summary.csv")
        pd.DataFrame(rows).to_csv(p, index=False)
        print(f"comparison of {len(rows)} runs -> {p}")


def _default_name(f, files):
    base = os.path.splitext(os.path.basename(f.replace(".gz", "")))[0]
    parent = os.path.basename(os.path.dirname(os.path.abspath(f)))
    same = sum(os.path.basename(g) == os.path.basename(f) for g in files) > 1
    return f"{parent}_{base}" if same else base


if __name__ == "__main__":
    main()
