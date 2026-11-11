"""noctrace: analyze NoC flit traffic from OpenPiton simulations.

    python3 -m noctrace TRACE [TRACE ...] [options]

Each TRACE is one simulation's output: a noc_trace CSV written by
noc_trace_mon (run with +noc_trace), or a sims.log from a run with
+enable_router_mon. Results for each trace go to OUT/<name>/, where <name>
is the trace's file name (or --name). With several traces,
OUT/runs_summary.csv compares them side by side.

Full reference: rit/docs/noctrace_reference.md
"""
import argparse
import json
import os
import sys

import pandas as pd

from . import load_run, metrics

EXAMPLES = """examples:
  # analyze one trace with default settings
  python3 -m noctrace hello_2x2.csv -o ~/noc_results

  # flits per 10k-cycle window, per core and total, titled for a slide
  python3 -m noctrace hello_2x2.csv --bin 10000 --window 10000 \\
          --signature-unit flits --app hello_world --round 1 -o ~/noc_results

  # compare three runs (one --app/--round per file, in file order)
  python3 -m noctrace a.csv b.csv c.csv --app bs --app bs --app bs \\
          --round 1 --round 2 --round 3 -o ~/noc_results

  # only NoC2, only cycles 100k-500k
  python3 -m noctrace run.csv --noc 2 --start 100000 --end 500000

  # old sims.log from a run with +enable_router_mon
  python3 -m noctrace sims.log --x-tiles 2 --y-tiles 2

full reference: rit/docs/noctrace_reference.md
"""


def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="python3 -m noctrace", description=__doc__.split("Full reference")[0],
        epilog=EXAMPLES, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", metavar="TRACE",
                    help="one or more simulation output files (noc_trace .csv / .csv.gz, or sims.log)")

    g = ap.add_argument_group("output")
    g.add_argument("-o", "--out", default="noc_analysis", metavar="DIR",
                   help="directory for results; each trace gets DIR/<name>/ (default: ./noc_analysis)")
    g.add_argument("--name", action="append", metavar="NAME",
                   help="results folder name for a trace, instead of its file name; "
                        "repeat once per TRACE, in order")
    g.add_argument("--no-plots", action="store_true",
                   help="write CSV/JSON/TXT only, skip the PNG plots (faster)")
    g.add_argument("--export-events", action="store_true",
                   help="also write events.csv.gz: every flit on every channel, decoded (large)")

    g = ap.add_argument_group("time binning")
    g.add_argument("--bin", type=int, default=0, metavar="CYCLES",
                   help="bin width for the *_over_time tables and plots "
                        "(default: run length / 100)")
    g.add_argument("--window", type=int, default=0, metavar="CYCLES",
                   help="window width for the per-core signatures (default: same as --bin). "
                        "Use the same value for every run you compare")

    g = ap.add_argument_group("per-core signatures")
    g.add_argument("--signature-metric", default="injected",
                   choices=["injected", "ejected", "router"],
                   help="what each core's line counts: injected = packets its tile sent "
                        "(default), ejected = packets delivered to it, router = packets "
                        "passing through its router")
    g.add_argument("--signature-unit", default="packets", choices=["packets", "flits"],
                   help="what the signature plots show per window (default: packets). "
                        "signatures.csv always has both")
    g.add_argument("--app", action="append", metavar="NAME",
                   help="application name for plot titles and the CSVs' app column; "
                        "repeat once per TRACE. Does not change the folder name")
    g.add_argument("--round", action="append", metavar="LABEL",
                   help="round/repetition label for plot titles and the CSVs' round column; "
                        "repeat once per TRACE")

    g = ap.add_argument_group("filtering")
    g.add_argument("--noc", default="", metavar="LIST",
                   help="only these NoCs, comma-separated, e.g. 2 or 1,3 (default: all)")
    g.add_argument("--start", type=int, metavar="CYCLE",
                   help="drop flits before this cycle")
    g.add_argument("--end", type=int, metavar="CYCLE",
                   help="drop flits after this cycle")

    g = ap.add_argument_group("input format (usually automatic)")
    g.add_argument("--format", default="auto", choices=["auto", "noc_trace", "router_mon"],
                   help="input type: noc_trace = noc_trace_mon CSV, router_mon = sims.log "
                        "with +enable_router_mon (default: detect)")
    g.add_argument("--x-tiles", type=int, metavar="N",
                   help="mesh width; needed for sims.log if the log doesn't state it "
                        "(noc_trace CSVs record it themselves)")
    g.add_argument("--y-tiles", type=int, metavar="N", help="mesh height (as --x-tiles)")
    g.add_argument("--clock-period", type=int, metavar="T",
                   help="sims.log only: sim time units per clock cycle (default: inferred)")
    g.add_argument("--chip-id", type=int, default=0, metavar="ID",
                   help="this chip's id; packets to other ids count as off-chip (default: 0)")
    return ap.parse_args(argv)


def analyze_one(path, name, a, app=None, rnd=None):
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
    win = a.window or b
    sig = metrics.signature(run, win, a.signature_metric)
    sig.insert(0, "round", rnd if rnd is not None else "")
    sig.insert(0, "app", app or "")
    w(sig, "signatures.csv")
    tot = sig.groupby(["app", "round", "window", "cycle_start"], as_index=False)[["packets", "flits"]].sum()
    w(tot, "signatures_total.csv")
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
        plots.signature_plots(sig, win, out, app=app, rnd=rnd, metric=a.signature_metric,
                              unit=a.signature_unit)
    s["signature_window_cycles"] = win
    s["signature_metric"] = a.signature_metric
    s["signature_unit"] = a.signature_unit
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(s, f, indent=2)
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
        app = a.app[i] if a.app and i < len(a.app) else None
        rnd = a.round[i] if a.round and i < len(a.round) else None
        # output folder follows the trace file (or --name); --app/--round only label
        name = names[i] if i < len(names) else _default_name(f, a.files)
        s, out = analyze_one(f, name, a, app, rnd)
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
