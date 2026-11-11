"""Turn flit events into links, per-hop packets and end-to-end packets.

Steps
1. links:  each event -> the directed channel it crossed
           (tile->router inject, router->router, router->tile eject, chip<->offchip)
2. hops:   each channel carries whole packets back to back (wormhole, no
           interleaving), so a header flit plus its `length` body flits is one
           packet crossing one channel.
3. chains: follow each packet hop to hop. The next channel is computed from the
           header with the router's own XY routing rule, and the matching hop on
           that channel must carry the same flits (hash of all flits) and start
           no earlier. That gives source, destination, path and latency.
"""
import hashlib
from collections import defaultdict

import numpy as np
import pandas as pd

from .header import MSG_TYPES, OFFCHIP_ORIGIN_TYPES, final_dir

DIRV = {"N": (0, -1), "S": (0, 1), "E": (1, 0), "W": (-1, 0)}
KIND_ROUTER, KIND_TILE, KIND_OFF = "router", "tile", "offchip"


class Mesh:
    """Node numbering: routers 0..NT-1, tiles NT..2NT-1, offchip 2NT."""

    def __init__(self, x_tiles, y_tiles):
        self.X, self.Y = int(x_tiles), int(y_tiles)
        self.NT = self.X * self.Y
        self.OFF = 2 * self.NT
        self.NN = 2 * self.NT + 1

    def router(self, x, y):
        return x + y * self.X

    def tile(self, x, y):
        return self.NT + x + y * self.X

    def kind(self, node):
        if node == self.OFF:
            return KIND_OFF
        return KIND_ROUTER if node < self.NT else KIND_TILE

    def xy(self, node):
        if node == self.OFF:
            return (-1, -1)
        f = node % self.NT
        return (f % self.X, f // self.X)

    def label(self, node):
        if node == self.OFF:
            return "offchip"
        f = node % self.NT
        x, y = f % self.X, f // self.X
        return (f"r{f}" if node < self.NT else f"tile{f}") + f"({x},{y})"

    def endpoint_label(self, node):
        """Label for a traffic source/destination (tiles and offchip only)."""
        if node == self.OFF:
            return "offchip"
        f = node % self.NT
        return f"tile{f}({f % self.X},{f // self.X})"

    def neighbor(self, x, y, d):
        dx, dy = DIRV[d]
        nx, ny = x + dx, y + dy
        if 0 <= nx < self.X and 0 <= ny < self.Y:
            return self.router(nx, ny)
        return self.OFF


# ------------------------------------------------------------------ 1. links
def assign_links(ev, mesh):
    x = ev["x"].to_numpy().astype(np.int64)
    y = ev["y"].to_numpy().astype(np.int64)
    port = ev["port"].to_numpy()
    is_in = (ev["dir"].to_numpy() == "in")
    here = x + y * mesh.X
    other = np.full(len(ev), -1, dtype=np.int64)
    p_mask = port == "P"
    other[p_mask] = mesh.NT + here[p_mask]
    for d, (dx, dy) in DIRV.items():
        m = port == d
        nx, ny = x[m] + dx, y[m] + dy
        ok = (nx >= 0) & (nx < mesh.X) & (ny >= 0) & (ny < mesh.Y)
        other[m] = np.where(ok, nx + ny * mesh.X, mesh.OFF)
    ev["from_node"] = np.where(is_in, other, here)
    ev["to_node"] = np.where(is_in, here, other)
    ev["link"] = ev["from_node"] * mesh.NN + ev["to_node"]
    return ev


def link_kind(from_node, to_node, mesh):
    fk, tk = mesh.kind(from_node), mesh.kind(to_node)
    if fk == KIND_TILE:
        return "inject"
    if tk == KIND_TILE:
        return "eject"
    if tk == KIND_OFF:
        return "to_offchip"
    if fk == KIND_OFF:
        return "from_offchip"
    return "mesh"


# ------------------------------------------------------------------- 2. hops
def build_hops(ev):
    """Segment each channel's flit stream into packets. Adds ev['hop']."""
    ev.sort_values(["noc", "link", "cycle", "seq"], inplace=True, kind="mergesort")
    ev.reset_index(drop=True, inplace=True)
    flits = ev["flit"].to_numpy(dtype=np.uint64)
    cyc = ev["cycle"].to_numpy()
    hop_of = np.empty(len(ev), dtype=np.int64)
    keys = (ev["noc"].to_numpy().astype(np.int64) << 40) | ev["link"].to_numpy()
    bounds = np.flatnonzero(np.diff(keys)) + 1
    starts = np.concatenate([[0], bounds])
    ends = np.concatenate([bounds, [len(ev)]])
    recs = []
    h = 0
    for s, e in zip(starts, ends):
        i = s
        while i < e:
            L = int((int(flits[i]) >> 22) & 0xFF)
            j = i + L
            trunc = j >= e
            if trunc:
                j = e - 1
            hop_of[i:j + 1] = h
            sig = hashlib.blake2b(flits[i:j + 1].tobytes(), digest_size=8).hexdigest()
            recs.append((h, i, int(flits[i]), cyc[i], cyc[j], j - i + 1, L + 1, trunc, sig))
            h += 1
            i = j + 1
    ev["hop"] = hop_of
    hops = pd.DataFrame(recs, columns=["hop", "first_ev", "hdr", "head_cycle", "tail_cycle",
                                       "nflits", "expected_flits", "truncated", "sig"])
    first = ev.loc[hops["first_ev"], ["noc", "link", "from_node", "to_node"]].reset_index(drop=True)
    hops = pd.concat([hops, first], axis=1)
    hops["hdr"] = hops["hdr"].astype(np.uint64)
    return ev, hops


def decode_cols(hops):
    hdr = hops["hdr"].to_numpy(dtype=np.uint64)
    hops["dst_chip"] = ((hdr >> np.uint64(50)) & np.uint64(0x3FFF)).astype(np.int64)
    hops["dst_x"] = ((hdr >> np.uint64(42)) & np.uint64(0xFF)).astype(np.int64)
    hops["dst_y"] = ((hdr >> np.uint64(34)) & np.uint64(0xFF)).astype(np.int64)
    hops["fbits"] = ((hdr >> np.uint64(30)) & np.uint64(0xF)).astype(np.int64)
    hops["length"] = ((hdr >> np.uint64(22)) & np.uint64(0xFF)).astype(np.int64)
    hops["msg_type"] = ((hdr >> np.uint64(14)) & np.uint64(0xFF)).astype(np.int64)
    hops["mshr"] = ((hdr >> np.uint64(6)) & np.uint64(0xFF)).astype(np.int64)
    return hops


# ----------------------------------------------------------------- routing
def next_node(router_node, dst_chip, dx, dy, fbits, mesh, chip_id=0):
    """OpenPiton dynamic_input_route_request_calc: X first, then Y, then fbits."""
    x, y = mesh.xy(router_node)
    if dst_chip != chip_id:
        tx, ty = 0, 0          # OFF_CHIP_NODE_X/Y
    else:
        tx, ty = dx, dy
    if tx > x:
        d = "E"
    elif tx < x:
        d = "W"
    elif ty < y:
        d = "N"
    elif ty > y:
        d = "S"
    else:
        d = final_dir(fbits)
    if d == "P":
        return mesh.tile(x, y)
    return mesh.neighbor(x, y, d)


def dst_node(dst_chip, dx, dy, fbits, mesh, chip_id=0):
    if dst_chip != chip_id:
        return mesh.OFF
    if not (0 <= dx < mesh.X and 0 <= dy < mesh.Y):
        return -1  # bad header
    d = final_dir(fbits)
    if d == "P":
        return mesh.tile(dx, dy)
    return mesh.neighbor(dx, dy, d)


# ------------------------------------------------------------------ 3. chains
def chain_packets(hops, mesh, chip_id=0):
    n = len(hops)
    to_node = hops["to_node"].to_numpy()
    from_node = hops["from_node"].to_numpy()
    noc = hops["noc"].to_numpy()
    sig = hops["sig"].to_numpy()
    head = hops["head_cycle"].to_numpy()
    dchip, dxs, dys, fbs = (hops[c].to_numpy() for c in ("dst_chip", "dst_x", "dst_y", "fbits"))

    # candidates: (noc, from, to, sig) -> hops sorted by head cycle
    cand = defaultdict(list)
    order = np.argsort(head, kind="mergesort")
    for i in order:
        cand[(noc[i], from_node[i], to_node[i], sig[i])].append(i)
    ptr = defaultdict(int)

    succ = np.full(n, -1, dtype=np.int64)
    has_pred = np.zeros(n, dtype=bool)
    expected_next = np.full(n, -1, dtype=np.int64)
    for i in order:
        if to_node[i] >= mesh.NT:          # delivered to a tile or offchip
            continue
        nxt = next_node(to_node[i], dchip[i], dxs[i], dys[i], fbs[i], mesh, chip_id)
        expected_next[i] = nxt
        key = (noc[i], to_node[i], nxt, sig[i])
        lst = cand.get(key)
        if not lst:
            continue
        p = ptr[key]
        while p < len(lst) and head[lst[p]] < head[i]:
            p += 1
        if p < len(lst):
            j = lst[p]
            succ[i] = j
            has_pred[j] = True
            p += 1
        ptr[key] = p
    hops["succ"] = succ
    hops["expected_next"] = expected_next
    hops["has_pred"] = has_pred
    return hops


def build_packets(hops, mesh, meta):
    chip_id = meta.get("chip_id", 0)
    succ = hops["succ"].to_numpy()
    pkt_of_hop = np.full(len(hops), -1, dtype=np.int64)
    rows = []
    starts = np.flatnonzero(~hops["has_pred"].to_numpy())
    H = hops
    fn, tn = H["from_node"].to_numpy(), H["to_node"].to_numpy()
    for pid, s in enumerate(starts):
        chain = [s]
        while succ[chain[-1]] >= 0:
            chain.append(succ[chain[-1]])
        pkt_of_hop[chain] = pid
        first, last = chain[0], chain[-1]
        # source
        src = fn[first]
        inferred = False
        if src < mesh.NT:      # first seen leaving a router: injected there (router_mon)
            inferred = True
            if H.at[first, "msg_type"] in OFFCHIP_ORIGIN_TYPES and src == mesh.router(0, 0):
                src = mesh.OFF
            else:
                src = mesh.tile(*mesh.xy(src))
        dst = dst_node(H.at[first, "dst_chip"], H.at[first, "dst_x"], H.at[first, "dst_y"],
                       H.at[first, "fbits"], mesh, chip_id)
        # delivered: seen leaving the network, or (no P-port events) seen
        # arriving at the router whose next step is the destination tile
        delivered = (tn[last] >= mesh.NT) or (
            not meta.get("has_p_ports", True) and dst >= mesh.NT and dst != mesh.OFF
            and H.at[last, "expected_next"] == dst)
        observed_inject = fn[first] >= mesh.NT
        nodes = [fn[first]] + [tn[c] for c in chain]
        mesh_hops = sum(1 for c in chain if fn[c] < mesh.NT and tn[c] < mesh.NT)
        mt = int(H.at[first, "msg_type"])
        rows.append({
            "pkt": pid,
            "noc": int(H.at[first, "noc"]),
            "src": mesh.endpoint_label(src),
            "src_node": int(src),
            "src_inferred": inferred,
            "dst": mesh.endpoint_label(dst) if dst >= 0 else "invalid",
            "dst_node": int(dst),
            "msg_type": mt,
            "msg_name": MSG_TYPES.get(mt, f"type{mt}"),
            "length": int(H.at[first, "length"]),
            "flits": int(H.at[first, "nflits"]),
            "mesh_hops": mesh_hops,
            "channels": len(chain),
            "inject_cycle": int(H.at[first, "head_cycle"]),
            "eject_cycle": int(H.at[last, "tail_cycle"]) if delivered else -1,
            "latency": int(H.at[last, "tail_cycle"] - H.at[first, "head_cycle"])
                       if (delivered and observed_inject) else np.nan,
            "delivered": bool(delivered),
            "truncated": bool(H.loc[chain, "truncated"].any()),
            "path": " > ".join(mesh.label(v) for v in nodes),
        })
    hops["pkt"] = pkt_of_hop
    return pd.DataFrame(rows), hops


class Run:
    """One simulation run, loaded and reconstructed.

    run.events  : one row per flit per channel  (cycle, noc, link, pkt, src, dst, ...)
    run.hops    : one row per packet per channel
    run.packets : one row per end-to-end packet
    run.links   : one row per channel that carried traffic
    run.meta    : mesh size, source format, clock period, warnings
    """

    def __init__(self, events, meta):
        self.meta = dict(meta)
        self.meta.setdefault("warnings", [])
        self.mesh = Mesh(meta["x_tiles"], meta["y_tiles"])
        m = self.mesh
        ev = assign_links(events.copy(), m)
        ev, hops = build_hops(ev)
        hops = decode_cols(hops)
        hops = chain_packets(hops, m, meta.get("chip_id", 0))
        packets, hops = build_packets(hops, m, meta)

        ev = ev.merge(hops[["hop", "pkt"]], on="hop", how="left")
        pk = packets.set_index("pkt")
        ev["src"] = pk["src"].reindex(ev["pkt"]).to_numpy()
        ev["dst"] = pk["dst"].reindex(ev["pkt"]).to_numpy()
        ev["msg_name"] = pk["msg_name"].reindex(ev["pkt"]).to_numpy()
        kinds = {}
        labels = {}
        for lk in ev["link"].unique():
            f, t = divmod(int(lk), m.NN)
            kinds[lk] = link_kind(f, t, m)
            labels[lk] = f"{m.label(f)}->{m.label(t)}"
        ev["link_kind"] = ev["link"].map(kinds)
        ev["link_name"] = ev["link"].map(labels)
        # first channel of each packet = where it entered the network
        first_hop = hops.loc[~hops["has_pred"], "hop"]
        ev["is_first_channel"] = ev["hop"].isin(set(first_hop))
        ev.sort_values(["cycle", "seq"], inplace=True, kind="mergesort")
        ev.reset_index(drop=True, inplace=True)
        self.events, self.hops, self.packets = ev, hops, packets
        self._checks()

    def _checks(self):
        w = self.meta["warnings"]
        m, h = self.mesh, self.hops
        if h["truncated"].any():
            w.append(f"{int(h['truncated'].sum())} packet(s) cut off at the end of the trace")
        bad = ((h["dst_chip"] == self.meta.get("chip_id", 0)) &
               ((h["dst_x"] >= m.X) | (h["dst_y"] >= m.Y)))
        if bad.any():
            w.append(f"{int(bad.sum())} header(s) point outside the {m.X}x{m.Y} mesh: "
                     "wrong --x-tiles/--y-tiles, or the trace started mid-packet")
        observable_next = h["expected_next"] < m.NT          # next channel is router->router
        if self.meta.get("has_p_ports", True):
            observable_next = h["expected_next"] >= 0
        else:   # router_mon logs router->offchip too
            observable_next = observable_next | (h["expected_next"] == m.OFF)
        lost = (h["to_node"] < m.NT) & (h["succ"] < 0) & observable_next
        if lost.any():
            w.append(f"{int(lost.sum())} hop(s) entered a router but were never seen leaving it "
                     "(trace ended while in flight, or a monitor gap)")
        if not self.meta.get("has_p_ports", True):
            w.append("source has no tile-port (P) events: tile-local traffic is invisible, "
                     "sources are inferred from the first router a packet leaves, and latency "
                     "is unavailable. Use the noc_trace monitor for exact numbers.")

    @property
    def links(self):
        m = self.mesh
        g = self.events.groupby(["noc", "link"]).agg(
            flits=("flit", "size"), packets=("pkt", "nunique")).reset_index()
        f, t = np.divmod(g["link"].to_numpy(), m.NN)
        g["from"] = [m.label(v) for v in f]
        g["to"] = [m.label(v) for v in t]
        g["kind"] = [link_kind(a, b, m) for a, b in zip(f, t)]
        g["from_x"] = [m.xy(v)[0] for v in f]
        g["from_y"] = [m.xy(v)[1] for v in f]
        g["to_x"] = [m.xy(v)[0] for v in t]
        g["to_y"] = [m.xy(v)[1] for v in t]
        span = self.cycle_span()
        g["utilization"] = g["flits"] / max(span, 1)
        return g.drop(columns="link")

    def cycle_span(self):
        c = self.events["cycle"]
        return int(c.max() - c.min() + 1) if len(c) else 0
