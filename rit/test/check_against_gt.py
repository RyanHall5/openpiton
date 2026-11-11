#!/usr/bin/env python3
"""Compare noctrace's reconstruction with the testbench ground truth (gt.csv).

Expected link loads are computed independently here: each injected packet is
walked X-first-then-Y from its source to its destination.
"""
import sys
from collections import Counter

import pandas as pd

sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else ".")
from noctrace import load_run  # noqa: E402

B = sys.argv[2] if len(sys.argv) > 2 else "test/build"
X, Y = (int(v) for v in (sys.argv[3], sys.argv[4])) if len(sys.argv) > 4 else (3, 2)

gt = [l.strip().split(",") for l in open(f"{B}/gt.csv") if l.startswith("inj")]
inj = pd.DataFrame(gt, columns=["k", "time", "noc", "src", "dst", "len", "type", "hdr"])
for c in ["noc", "src", "dst", "len", "type"]:
    inj[c] = inj[c].astype(int)


def lab(t):
    return "offchip" if t < 0 else f"tile{t}({t % X},{t // X})"


def walk(src, dst):
    """Channels (as from->to labels) for one packet, independent of noctrace."""
    def r(x, y): return f"r{x + y * X}({x},{y})"
    if src < 0:
        x, y = 0, 0
        ch = ["offchip->" + r(0, 0)]
    else:
        x, y = src % X, src // X
        ch = [f"{lab(src).replace('tile', 'tile')}->{r(x, y)}"]
    tx, ty = (0, 0) if dst < 0 else (dst % X, dst // X)
    while x != tx:
        nx = x + (1 if tx > x else -1)
        ch.append(f"{r(x, y)}->{r(nx, y)}"); x = nx
    while y != ty:
        ny = y + (1 if ty > y else -1)
        ch.append(f"{r(x, y)}->{r(x, ny)}"); y = ny
    ch.append(f"{r(x, y)}->offchip" if dst < 0 else f"{r(x, y)}->{lab(dst)}")
    return ch


exp_links = Counter()
for p in inj.itertuples():
    for c in walk(p.src, p.dst):
        exp_links[(p.noc, c)] += p.len + 1
exp_paths = Counter((p.noc, lab(p.src), lab(p.dst)) for p in inj.itertuples())

ok = True


def check(name, cond, detail=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + (f"  {detail}" if detail else ""))
    ok &= bool(cond)


# ---------------------------------------------------------------- noc_trace
r = load_run(f"{B}/noc_trace.csv")
got_links = Counter({(n, l): c for (n, l), c in
                     r.events.groupby(["noc", "link_name"]).size().items()})
check("noc_trace: packet count", len(r.packets) == len(inj), f"{len(r.packets)} vs {len(inj)}")
check("noc_trace: every channel's flit count matches XY routing", got_links == exp_links,
      f"{len(exp_links)} channels")
got_paths = Counter((p.noc, p.src, p.dst) for p in r.packets.itertuples())
check("noc_trace: (noc, src, dst) packet counts", got_paths == exp_paths)
check("noc_trace: no inferred sources", not r.packets["src_inferred"].any())
lat = r.packets["latency"]
check("noc_trace: all latencies measured and positive", lat.notna().all() and (lat > 0).all(),
      f"min {lat.min():.0f}  mean {lat.mean():.1f}  max {lat.max():.0f} cycles")
check("noc_trace: no warnings", not r.meta["warnings"], str(r.meta["warnings"]))

# --------------------------------------------------------------- router_mon
m = load_run(f"{B}/sims.log")
mesh_exp = Counter({k: v for k, v in exp_links.items()
                    if not k[1].startswith(("tile", "offchip")) and "->tile" not in k[1]})
got_m = Counter({(n, l): c for (n, l), c in m.events.groupby(["noc", "link_name"]).size().items()})
check("router_mon: router->router and ->offchip flit counts match", got_m == mesh_exp,
      f"{len(mesh_exp)} channels")
def logged(c):  # router_mon logs flits leaving a router on N/S/E/W only
    return c.startswith("r") and "->tile" not in c


vis = inj[[any(logged(c) for c in walk(p.src, p.dst)) for p in inj.itertuples()]]
exp_vis = Counter((p.noc, lab(p.src), lab(p.dst)) for p in vis.itertuples())
got_vis = Counter((p.noc, p.src, p.dst) for p in m.packets.itertuples())
check("router_mon: inferred (noc, src, dst) counts for packets that touch a logged channel",
      got_vis == exp_vis, f"{sum(got_vis.values())} vs {sum(exp_vis.values())} packets")
PERIOD = int(sys.argv[5]) if len(sys.argv) > 5 else 2500
check("router_mon: clock period inferred", m.meta["clock_period"] == PERIOD, str(m.meta["clock_period"]))

print("\nALL PASS" if ok else "\nSOME CHECKS FAILED")
sys.exit(0 if ok else 1)
