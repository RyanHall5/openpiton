#!/usr/bin/env python3
"""Check noc_trace_mon's trace window against the testbench ground truth.

usage: check_window.py RIT BUILD_DIR X Y PERIOD TRACE EXPECTED_WINDOWS

EXPECTED_WINDOWS is "b1:e1,b2:e2,..." in cycles counted from time 0 (e may be
"end"). Checks:
  1. the trace notes exactly those windows (+-1 cycle)
  2. every channel carries whole packets only (no cut-off packets, no headers
     decoded from the middle of a packet); packets traced on only part of
     their route (crossing the network as a window opened or closed) are all
     within 50 cycles of a window edge
  3. every logged packet's header crossed each channel while a window was open
  4. every ground-truth packet that lived entirely inside a window is in the
     trace, complete, with the right source and destination, on every channel
     of its route
  5. noctrace's default alignment puts cycle 0 at the first window
"""
import sys

import numpy as np

sys.path.insert(0, sys.argv[1])
from noctrace import load_run  # noqa: E402
from noctrace.adapters import load_events, align_to_window  # noqa: E402

B, X, Y, PERIOD, TRACE, EXP = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), \
    int(sys.argv[5]), sys.argv[6], sys.argv[7]
exp = [(int(b), None if e == "end" else int(e)) for b, e in (w.split(":") for w in EXP.split(","))]

ok = True


def check(name, cond, detail=""):
    global ok
    print(("PASS " if cond else "FAIL ") + f"{TRACE.split('/')[-1]}: " + name +
          (f"  {detail}" if detail else ""))
    ok &= bool(cond)


r = load_run(TRACE)
wins = r.meta["windows"]

# 1. windows noted
same = len(wins) == len(exp) and all(
    abs(gb - eb) <= 1 and ((ge is None and ee is None) or
                           (ge is not None and ee is not None and abs(ge - ee) <= 1))
    for (gb, ge), (eb, ee) in zip(wins, exp))
check("windows noted in the trace", same, f"got {wins} expected {exp}")

# 2. whole packets only
bad = [w for w in r.meta["warnings"] if "cut off" in w or "outside" in w]
check("whole packets only on every channel (no cut-off or mis-decoded packets)", not bad, str(bad))


def in_window(c):
    return any(c >= b and (e is None or c < e) for b, e in wins)


# 2b. a packet crossing the network as a window opens or closes is traced on only
#     part of its route. That must only happen right at a window edge.
EDGE = 50
edges = [c for w in wins for c in w if c is not None]
p = r.packets
part = p[(~p["delivered"]) | (p["src_inferred"])]
far = [int(q.pkt) for q in part.itertuples()
       if not any(abs(q.inject_cycle - c) <= EDGE for c in edges)]
check("packets traced on only part of their route are all at a window edge",
      not far, f"{len(part)} of {len(p)} partial, {len(far)} away from an edge")


# 3. every packet's header on every channel was inside a window
hdr_cycles = r.hops["head_cycle"].to_numpy()
outside = [int(c) for c in hdr_cycles if not in_window(c)]
check("every logged header crossed its channel inside a window", not outside,
      f"{len(hdr_cycles)} packet-channel crossings, {len(outside)} outside")

# 4. packets that lived entirely inside a window are all there, complete
inj, ej = {}, {}
for line in open(f"{B}/gt.csv"):
    f = line.strip().split(",")
    if f[0] == "inj":
        inj[(int(f[2]), int(f[7], 16))] = (int(f[1]), int(f[3]), int(f[4]), int(f[5]))
    elif f[0] == "ej":
        ej[(int(f[2]), int(f[4], 16))] = int(f[1])
lab = lambda t: "offchip" if t < 0 else f"tile{t}({t % X},{t // X})"  # noqa: E731
inside = []
for key, (t_inj, src, dst, ln) in inj.items():
    if key not in ej:
        continue
    c0, c1 = t_inj // PERIOD, ej[key] // PERIOD
    if any(c0 >= b + 2 and (e is None or c1 <= e - 2) for b, e in wins):
        inside.append((key, src, dst, ln))
ev = r.events
missing = []
for (noc, hdr), src, dst, ln in inside:
    rows = ev[(ev["noc"] == noc) & (ev["flit"] == np.uint64(hdr))]
    pk = rows["pkt"].unique()
    if len(pk) != 1:
        missing.append((noc, hex(hdr), "not found" if len(pk) == 0 else "split"))
        continue
    p = r.packets[r.packets["pkt"] == pk[0]].iloc[0]
    if not (p["delivered"] and p["src"] == lab(src) and p["dst"] == lab(dst)
            and p["flits"] == ln + 1 and not p["src_inferred"]):
        missing.append((noc, hex(hdr), "wrong"))
check("every packet that lived inside a window is traced, complete and correct",
      inside and not missing, f"{len(inside)} packets, problems: {missing[:5]}")

# 5. default alignment
ev2, meta2 = load_events(TRACE)
al = align_to_window(ev2, meta2)
check("noctrace puts cycle 0 at the first window",
      meta2["cycle_offset"] == wins[0][0] and al["cycle"].min() >= 0,
      f"offset {meta2['cycle_offset']}, first flit at {al['cycle'].min()}")

print("WINDOW ALL PASS" if ok else "WINDOW CHECKS FAILED")
sys.exit(0 if ok else 1)
