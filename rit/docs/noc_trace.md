# NoC flit-traffic tracing and analysis (noc_trace_mon + noctrace)

Two parts:

| Part | What it is |
|---|---|
| `piton/verif/env/manycore/noc_trace_mon.v.pyv` | A testbench monitor that writes one CSV line for **every flit crossing every router port**, on all three NoCs. Off unless you pass `+noc_trace`. |
| `rit/noctrace/` | Python package that reads simulator output, rebuilds every packet's source, destination, path and latency, and writes CSV tables + plots: traffic over time (total, per core, per path, per link, per NoC, per message type). |

The Python side reads two formats today:

* **`noc_trace.csv`** from the monitor above. Complete and exact: sees injection, ejection, tile-local traffic and chipset traffic, so latency is available.
* **`sims.log`** from OpenPiton's built-in router monitor (`+enable_router_mon`, already in `manycore_network_mon.v.pyv`). No RTL change needed, but it only logs flits *leaving* a router on N/S/E/W. So: no tile-local traffic, no chipset-to-chip flits, sources are inferred, and there is no latency. Router-to-router link counts are still exact.

Both were checked against ground truth (see *Self-test*).

---

## 1. The monitor is already in this branch

`piton/verif/env/manycore/noc_trace_mon.v.pyv` is listed in `manycore.flist` (and `.flist.jtag`) and instantiated in `manycore_top.v.pyv`, outside `MINIMAL_MONITORING`. It's compiled into every model you build from this branch, and it stays idle unless you run with `+noc_trace`.

If you built a model **before** pulling this branch, rebuild it, because older models don't contain the monitor.

## 2. Build a multi-tile model

A 1x1 build has no tile-to-tile links; use 2x2 or bigger. Environment and VCS fix flags are covered in [`rhel9_vcs_setup.md`](rhel9_vcs_setup.md):

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane 2>&1 | tee build_2x2.log
```

`-config_rtl=MINIMAL_MONITORING` is fine with `noc_trace` (it's outside that guard), but it removes the built-in router monitor.

## 3. Run with tracing on

```bash
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane \
  -rtl_timeout 10000000 \
  -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=hello_2x2.csv
```

* The trace lands in the run directory (`$PITON_ROOT/build/`). Give each run its own `+noc_trace_file=` name, or it overwrites `noc_trace.csv`.
* Add `-sim_run_args=+enable_router_mon` if you also want the built-in monitor's lines in `sims.log`.
* Even a single-threaded program makes mesh traffic: OpenPiton's L2 is split into slices across the tiles, so core 0's misses travel to other tiles' L2 slices, and memory traffic goes through tile 0 to the chipset.
* File size is about 40 bytes per flit per channel. Long runs get big; gzip works (`noctrace` reads `.csv.gz`).

## 4. Analyze

`install_deps.sh` installs the Python packages, and the env script puts `rit/` on `PYTHONPATH`, so this works from any directory:
```bash
python3 -m noctrace $PITON_ROOT/build/hello_2x2.csv -o ~/noc_results
```
Write results **outside the repo** (or into a git-ignored `results/` folder). See *Sharing data* below.

Several runs at once (different programs and/or builds) also writes `results/runs_summary.csv` comparing them:
```bash
python3 -m noctrace runA.csv runB.csv runC.csv --name hello --name matmul --name stream -o results
```

Old `sims.log` from a run with `+enable_router_mon` (give the mesh size if the log doesn't state it):
```bash
python3 -m noctrace $PITON_ROOT/build/sims.log --x-tiles 2 --y-tiles 2 -o results
```

Useful options: `--bin N` (time-bin width in cycles; default is run length / 100), `--noc 1,3`, `--start/--end` (cycle window), `--export-events` (full per-flit table), `--no-plots`, `--clock-period` (sims.log only; normally inferred).

### What you get, per run (`results/<run>/`)

| File | Contents |
|---|---|
| `summary.txt` / `.json` | totals, injection rate, per-NoC packets/flits/hops/latency, busiest link, warnings |
| `total_over_time.csv` | per bin and NoC: injected flits, router-to-router flit-hops, all-channel flits |
| `per_source_over_time.csv` | flits injected by each tile (and offchip) per bin and NoC |
| `per_dest_over_time.csv` | flits addressed to each destination per bin |
| `per_path.csv` | per (NoC, src, dst): packets, flits, flit-hops, hops, mean/max latency |
| `per_path_over_time.csv` | per (NoC, src, dst) per bin |
| `per_link.csv` | per directed channel: flits, packets, utilization (flits/cycle) |
| `per_link_over_time.csv` | per channel per bin, with utilization |
| `per_msg_type.csv` | packets/flits/latency by OpenPiton message type (LOAD_REQ, DATA_ACK, ...) |
| `path_matrix.csv` | src x dst flit matrix |
| `packets.csv` | every packet: src, dst, type, length, inject/eject cycle, latency, full path |
| `*.png` | traffic over time, per-source heatmap, link-load map per NoC, path matrix, top paths |

### Definitions

* **Injected flits**: each flit counted once, on the first channel of its packet. This is what the tiles put into the network.
* **Flit-hops**: each flit counted on every router-to-router channel it crosses. This is what loads the links. A 3-hop packet of 5 flits = 5 injected flits, 15 flit-hops.
* **Utilization**: flits on a channel / cycles. 1.0 is the maximum (one 64-bit flit per cycle).
* **Latency**: from the header entering the network at the source port to the tail leaving at the destination port, in cycles.
* **Cycle**: rising edges of the chip clock since time 0, counted by the monitor. For sims.log, `$time / clock period`.
* **Names**: `tile5(1,1)` is tile 5 at x=1, y=1 (flat id = x + y * x_tiles). `r5(1,1)` is that tile's router. y grows southward, as in the RTL.

## How packets are rebuilt

1. Each flit event maps to the directed channel it crossed (tile to router, router to router, router to tile, chip to/from offchip).
2. OpenPiton's routers are wormhole with no virtual channels, so a channel carries whole packets back to back: header flit, then `length` body flits (bits [29:22] of the header).
3. Hops are joined into end-to-end packets using the routers' own rule from `dynamic_input_route_request_calc.v`: X first, then Y, then the header's fbits at the destination. Off-chip destinations go to (0,0) and leave west. The next hop has to be on the channel that rule predicts, carry identical flits, and start no earlier.

Header layout, message-type names and routing all come from the OpenPiton source (`define.h.pyv`, `dynamic_input_*`). If your version changes message types, `noctrace.header.load_msg_types(path_to_define.h.pyv)` reloads them.

## Using it from Python

```python
# with openpiton_env.sh sourced (or sys.path.insert(0, "<repo>/rit"))
from noctrace import load_run, metrics

run = load_run("hello_2x2.csv")
run.packets          # one row per packet
run.events           # one row per flit per channel (cycle, noc, link_name, pkt, src, dst, ...)
run.links            # one row per channel
metrics.total_over_time(run, bin_cycles=500)
```

## Adding another simulator's output

Write one function that returns a pandas DataFrame with columns `cycle, time, noc, x, y, dir, port, flit` (`dir` = `in`/`out` at router (x,y), `port` = N/E/S/W/P, `flit` = 64-bit int) plus a `meta` dict with `x_tiles`, `y_tiles`, `has_p_ports`. Register it in `ADAPTERS` in `rit/noctrace/adapters.py`. Everything downstream is unchanged. A CSV with those column names already works as-is.

Packet rebuilding (step 3 above) assumes OpenPiton's header layout and XY routing. A different NoC (other header format, VCs, adaptive routing) needs its own `decode_cols`/`next_node`. The per-link and over-time counts don't depend on that.

## Self-test

`rit/test/run_test.sh [X] [Y] [packets]` builds an X x Y mesh of the **real OpenPiton router RTL** (`dynamic_node_top_wrap`, 3 NoCs per tile) in Icarus Verilog. It drives random credit-based traffic from every tile and a chipset stub on tile 0 west, runs the generated `noc_trace_mon` unchanged, and also prints the built-in router monitor's exact line format. Then it checks the reconstruction against the testbench's own record of what it injected:

```bash
rit/test/run_test.sh 3 2 60      # needs iverilog; no VCS or env script required
```

It passes on 3x2, 2x3 and 4x4 (12,375 packets). Every channel's flit count matches XY routing, every (src, dst) count matches, and every latency is measured.

## Sharing data

Traces and results stay **out of Git**: a trace is about 40 bytes per flit per channel, so long runs reach hundreds of MB. `build/` is already ignored, and `rit/.gitignore` ignores `results/`, `noc_analysis/` and trace files. Put traces and results in the team's shared storage, and record with each one:
- the branch commit (`git rev-parse --short HEAD`)
- the build command (mesh size, `-config_rtl` options)
- the test program and run arguments

That's what makes a result reproducible. Small summaries (`summary.json`, a few plots) are fine to commit when they back up a write-up.

## Limits

* Not yet confirmed inside a full OpenPiton VCS build. The monitor's hierarchical paths (`` `TILEn.user_dynamic_networkK.validIn_X``) follow `cross_module.h.pyv` and `tile.v.pyv`, and the generated code compiles and runs against the real router RTL, but your first 2x2 VCS build is the real check.
* 2D-mesh builds only (the xbar network config is skipped).
* Builds with fewer tiles than x_tiles * y_tiles: edges next to a missing tile aren't traced.
* If a tile's clock is gated off, its router isn't sampled correctly (the monitor runs on the ungated chip clock).
* Speed is about 8 s per 300k flit events end to end on a laptop-class core, which is fine for single programs. Use `--start/--end` or `--noc` on very long runs.
