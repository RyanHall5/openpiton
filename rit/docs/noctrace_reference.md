# NoC tracing reference

The complete list of every switch, option, file and column. For a walkthrough, start with the user guide, [`noc_trace.md`](noc_trace.md).

**Contents**
1. [Trace monitor (`noc_trace_mon`)](#1-trace-monitor-noc_trace_mon): run-time switches, build integration
2. [Trace file format](#2-trace-file-format)
3. [`noctrace` command line](#3-noctrace-command-line): every option
4. [Output files](#4-output-files): every file and column
5. [Naming and definitions](#5-naming-and-definitions)
6. [Python API](#6-python-api)
7. [Adding another input format](#7-adding-another-input-format)

---

## 1. Trace monitor (`noc_trace_mon`)

`piton/verif/env/manycore/noc_trace_mon.v.pyv`

### Run-time switches (plusargs)

Passed through `sims` with `-sim_run_args=`, **one plusarg per `-sim_run_args`**:

| Plusarg | Default | Effect |
|---|---|---|
| `+noc_trace` | off | Turn the trace on. Without it the monitor does nothing and writes no file. |
| `+noc_trace_file=NAME` | `noc_trace.csv` | Output file name. Relative names land in the directory `sims` was run from (normally `$PITON_ROOT/build/`). An existing file with the same name is **overwritten**. |
| `+enable_router_mon` | off | Not part of this monitor. Turns on OpenPiton's built-in router monitor, which prints router-to-router flits into `sims.log`. Only useful for the `router_mon` input format (§3). |

```bash
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane \
     -rtl_timeout 10000000 -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=hello_2x2_r1.csv
```

When tracing starts, the run's output (and `sims.log`) shows `noc_trace_mon: writing flit trace to <NAME>`. If the file can't be opened, it shows `noc_trace_mon: ERROR could not open <NAME>, tracing disabled` and the simulation continues without a trace.

### Build integration

There's no build option for the monitor. Every model built from this branch includes it:

| File | Change |
|---|---|
| `piton/verif/env/manycore/noc_trace_mon.v.pyv` | the monitor (a PyHP template) |
| `piton/verif/env/manycore/manycore.flist`, `manycore.flist.jtag` | lists `noc_trace_mon.v` so it gets compiled |
| `piton/verif/env/manycore/manycore_top.v.pyv` | instantiates it: `noc_trace_mon noc_trace_mon (`CHIP_INT_CLK);` |

- During `-vcs_build`, `sims` runs PyHP on the template with the build's `-x_tiles`/`-y_tiles`, generating code for exactly that mesh. The generated Verilog is written next to the template as `piton/verif/env/manycore/noc_trace_mon.tmp.v` (git-ignored, overwritten by each build), and the build's file list in `build/manycore/<build_id>/` points to it.
- To check whether a model includes the monitor: `grep -rlq noc_trace_mon $PITON_ROOT/build/manycore/<build_id>/ && echo yes || echo "no: rebuild"`.
- The instance sits outside `MINIMAL_MONITORING`, so `-config_rtl=MINIMAL_MONITORING` builds still have it.
- It's skipped for the crossbar network config (`xbar_config`). Mesh builds only.
- **A model built before this file existed doesn't contain it. Rebuild after pulling changes to it.**
- It's verification-only code: it uses hierarchical references and `$fdisplay`, and is never synthesized. It only reads signals, so it can't change the design's behavior.

### What it samples

Every falling edge of the chip clock (`CHIP_INT_CLK`), for every tile and each of the three NoC routers (`user_dynamic_network0/1/2` = NoC1/2/3), it writes one line for each port whose `valid` is high:

| Logged | Meaning |
|---|---|
| `in` on N, E, S, W | a flit entering this router from a neighbor (a hop), or from the chipset (tile 0, W) |
| `in` on P | the tile (core / L1.5 / L2) injecting a flit |
| `out` on P | the router delivering a flit to its tile |
| `out` on N, E, S, W **at the mesh edge only** | a flit leaving the chip (normally tile 0, W → chipset) |

Each physical channel is logged exactly once. OpenPiton's NoC uses credit-based (`valid`/`yummy`) flow control, so `valid` high on a clock edge means exactly one flit was transferred.

---

## 2. Trace file format

```
# noc_trace v1
# source=openpiton noc_trace_mon
# x_tiles=2 y_tiles=2 num_tiles=4
# cycle counts rising edges of the chip clock from time 0; time is $time in sim units
cycle,time,noc,x,y,dir,port,flit
11,32500,1,0,0,in,P,0000040400c7c000
```

Lines starting with `#` are metadata. `noctrace` reads `x_tiles` and `y_tiles` from them.

| Column | Type | Meaning |
|---|---|---|
| `cycle` | int | rising edges of the chip clock since time 0 when the flit was seen |
| `time` | int | simulator `$time` (in the testbench's time units) |
| `noc` | 1, 2, 3 | physical network |
| `x`, `y` | int | coordinates of the router that saw the flit (tile id = x + y·x_tiles; y grows southward) |
| `dir` | `in` / `out` | entering or leaving that router |
| `port` | N, E, S, W, P | router port (P = the tile's own port) |
| `flit` | 16 hex digits | the raw 64-bit flit |

**Header flit layout** (first flit of each packet; from `piton/design/include/define.h.pyv`):

| Bits | Field |
|---|---|
| 63:50 | destination chip id (≠ this chip → off-chip) |
| 49:42 | destination x |
| 41:34 | destination y |
| 33:30 | fbits (final direction at the destination; 0 or bit 3 set = deliver to the tile) |
| 29:22 | payload length = number of body flits after the header |
| 21:14 | message type (names in `rit/noctrace/header.py`) |
| 13:6 | MSHR id |
| 5:0 | options |

Any CSV with the columns `cycle,noc,x,y,dir,port,flit` (and optionally `time`) in this meaning is accepted. Pass `--x-tiles`/`--y-tiles` if it has no `#` header.

---

## 3. `noctrace` command line

```
python3 -m noctrace TRACE [TRACE ...] [options]
```

Run with the env script sourced (`source ~/openpiton/rit/setup/openpiton_env.sh`), which puts `rit/` on `PYTHONPATH`. `python3 -m noctrace --help` prints the same information as this section.

**Options repeated per trace:** `--name`, `--app` and `--round` can each be given several times. The n-th value applies to the n-th TRACE. A trace without a value uses the default.

### Arguments

| Argument | Meaning |
|---|---|
| `TRACE` (one or more) | Simulation output files. Each is analyzed separately. Accepts a noc_trace CSV (`.csv` or `.csv.gz`) or a `sims.log`. |

### Output

| Option | Argument | Default | Meaning |
|---|---|---|---|
| `-o`, `--out` | DIR | `./noc_analysis` | Results directory. Each trace's results go to `DIR/<name>/`. |
| `--name` | NAME | trace file name without extension | Folder name for a trace's results. Repeat per trace. If two traces share a file name, the parent folder name is prepended. |
| `--no-plots` | — | off | Skip PNGs; write CSV/JSON/TXT only. Faster. |
| `--export-events` | — | off | Also write `events.csv.gz`: every flit on every channel, with its link, packet, source and destination. Large. |

### Time binning

| Option | Argument | Default | Meaning |
|---|---|---|---|
| `--bin` | CYCLES | run length / 100 | Bin width for all `*_over_time` tables and `traffic_over_time.png`/`injection_by_source.png`. Bins are aligned to cycle 0: bin k covers cycles `[k·CYCLES, (k+1)·CYCLES)`. |
| `--window` | CYCLES | same as `--bin` | Window width for the per-core signatures (`signatures*.csv`, `signatures/*.png`). Same alignment as `--bin`. **Use the same value for every run you compare.** |

### Per-core signatures

| Option | Argument | Default | Meaning |
|---|---|---|---|
| `--signature-metric` | `injected` / `ejected` / `router` | `injected` | What each core's signature counts: `injected` = packets/flits its tile sent into the network; `ejected` = packets/flits delivered to its tile; `router` = packets/flits passing through its router (sent, received or forwarded). |
| `--signature-unit` | `packets` / `flits` | `packets` | What the signature **plots** show. `signatures.csv` always has both columns. |
| `--app` | NAME | none | Application name for plot titles and the `app` column. Repeat per trace. Doesn't change the folder name. |
| `--round` | LABEL | none | Round/repetition label for plot titles and the `round` column. Repeat per trace. |

### Filtering

| Option | Argument | Default | Meaning |
|---|---|---|---|
| `--noc` | LIST | all | Only these NoCs, comma-separated: `2`, `1,3`. Everything (tables, plots, summary) then covers only those NoCs. |
| `--start` | CYCLE | none | Drop flits before this cycle. |
| `--end` | CYCLE | none | Drop flits after this cycle. |

Filtering happens before packets are rebuilt, so a packet cut by `--start`/`--end` shows up as a warning (truncated, or "never seen leaving").

### Input format

| Option | Argument | Default | Meaning |
|---|---|---|---|
| `--format` | `auto` / `noc_trace` / `router_mon` | `auto` | `noc_trace` = CSV from `noc_trace_mon`; `router_mon` = `sims.log` from a run with `+enable_router_mon`. `auto` picks `noc_trace` for files with the `# noc_trace` header or the canonical columns, and `router_mon` for files containing router-monitor lines or ending in `.log`. |
| `--x-tiles` | N | from the trace header | Mesh width. Needed for `sims.log`, which usually doesn't state it. |
| `--y-tiles` | N | from the trace header | Mesh height. |
| `--clock-period` | T | inferred | `sims.log` only: sim time units per clock cycle. Inferred as the GCD of the gaps between log timestamps. |
| `--chip-id` | ID | 0 | This chip's id. Packets addressed to another id count as off-chip. |

### `sims.log` (`router_mon`) limits

`sims.log` only contains flits leaving a router toward another router (or off-chip). So:
- tile-local traffic (core ↔ its own L2 slice) is invisible
- sources are inferred from the first router a packet leaves
- chipset → chip flits are missing
- latency is unavailable

Router-to-router link counts are still exact. `summary.txt` repeats these limits as a warning.

---

## 4. Output files

Everything for one trace is in `DIR/<name>/`. All CSVs have a header row. Cycles are chip-clock cycles.

### Summary

| File | Contents |
|---|---|
| `summary.txt` | Human-readable: file, format, mesh, cycle range, bin size, packet/flit totals, injection rate, per-NoC packets/flits/average hops/latency (mean, p95, max), busiest router-to-router link, warnings. |
| `summary.json` | The same, machine-readable. Keys: `file`, `format`, `mesh`, `first_cycle`, `last_cycle`, `cycles`, `packets`, `injected_flits`, `mesh_flit_hops`, `injection_rate_flits_per_cycle`, `per_noc` (per NoC: `packets`, `flits`, `avg_mesh_hops`, `latency_mean`, `latency_p95`, `latency_max`), `busiest_mesh_link` (`noc`, `from`, `to`, `flits`, `utilization`), `warnings`, `bin_cycles`, `signature_window_cycles`, `signature_metric`, `signature_unit`, and `clock_period` for sims.log. |
| `DIR/runs_summary.csv` | Only with 2+ traces: one row per trace. Columns: `run` plus the scalar `summary.json` keys, and `noc<N>_<field>` for each per-NoC field. |

### Over time (bins of `--bin` cycles)

`bin_start` = first cycle of the bin. Bins with no traffic are included as zeros.

| File | Columns |
|---|---|
| `total_over_time.csv` | `bin_start`, `noc`, `injected_flits` (flits that entered the network in the bin), `mesh_flit_hops` (flit crossings of router-to-router links), `all_channel_flits` (flit crossings of every channel: inject, eject, mesh, off-chip) |
| `per_source_over_time.csv` | `bin_start`, `noc`, `src`, `injected_flits`: flits each source (tile or `offchip`) sent |
| `per_dest_over_time.csv` | `bin_start`, `noc`, `dst`, `flits`: flits addressed to each destination, counted when they entered the network |
| `per_path_over_time.csv` | `bin_start`, `noc`, `src`, `dst`, `injected_flits` (only bins where the path had traffic) |
| `per_link_over_time.csv` | `bin_start`, `noc`, `link` (`from->to`), `kind`, `flits`, `utilization` (= flits / bin width) (only bins where the link had traffic) |

### Per-core signatures (windows of `--window` cycles)

| File | Columns / contents |
|---|---|
| `signatures.csv` | `app`, `round`, `core`, `window` (index k), `cycle_start` (= k·window), `packets`, `flits`. Every core × every window from 0 to the last cycle, zeros included. What's counted depends on `--signature-metric`. |
| `signatures_total.csv` | `app`, `round`, `window`, `cycle_start`, `packets`, `flits`: all cores added together. Off-chip traffic isn't a core and isn't included. |
| `signatures/core<N>.png` | One line plot per core: "\<app\> — Core N, Round R", number of packets (or flits) per window. |
| `signatures/total.png` | The same for all cores combined. |
| `signatures_all_cores.png` | Every core side by side on a shared y-axis. |

### Totals per path, link and message type

| File | Columns |
|---|---|
| `per_path.csv` | `noc`, `src`, `dst`, `packets`, `flits`, `mesh_hops` (median router-to-router hops), `latency_mean`, `latency_max` (cycles), `src_inferred` (true if the source was guessed: sims.log input), `flit_hops` (flits × hops). Sorted by flits, highest first. |
| `per_link.csv` | `noc`, `flits`, `packets`, `from`, `to`, `kind`, `from_x`, `from_y`, `to_x`, `to_y` (−1 = off-chip), `utilization` (flits / cycles in the run). One row per directed channel that carried traffic. |
| `per_msg_type.csv` | `noc`, `msg_type` (number), `msg_name`, `packets`, `flits`, `latency_mean` |
| `path_matrix.csv` | Source × destination flit totals over all NoCs; rows are sources, columns destinations (every tile plus `offchip`). |
| `packets.csv` | One row per packet: `pkt` (id), `noc`, `src`, `src_node`, `src_inferred`, `dst`, `dst_node`, `msg_type`, `msg_name`, `length` (body flits from the header), `flits` (flits seen), `mesh_hops`, `channels` (channels crossed incl. inject/eject), `inject_cycle` (header entered the network), `eject_cycle` (tail left; −1 if not seen), `latency` (eject − inject, empty if either end wasn't seen), `delivered`, `truncated` (trace ended mid-packet), `path` (every node it passed, `tile0(0,0) > r0(0,0) > r1(1,0) > tile1(1,0)`) |
| `events.csv.gz` | Only with `--export-events`. One row per flit per channel: `cycle`, `noc`, `x`, `y`, `dir`, `port`, `link_name`, `link_kind`, `pkt`, `src`, `dst`, `msg_name`, `flit` (hex) |

### Plots

| File | Shows |
|---|---|
| `traffic_over_time.png` | Per NoC: injected flits/cycle (top) and router-to-router flit-hops/cycle (bottom), per bin |
| `injection_by_source.png` | Heatmap: sources (rows) × bins (columns), injected flits/cycle |
| `link_map.png` | The mesh per NoC, each directed link colored and sized by utilization; the top 3 are labeled |
| `path_matrix.png` | Source × destination heatmap of flits |
| `top_paths.png` | The 15 source → destination pairs with the most flits |
| `signatures/*.png`, `signatures_all_cores.png` | See per-core signatures above |

### Warnings (in `summary.txt` / `summary.json`)

| Warning | Meaning |
|---|---|
| `N packet(s) cut off at the end of the trace` | The trace ended (or `--end` cut it) mid-packet. A handful is normal. |
| `N hop(s) entered a router but were never seen leaving it` | Packets in flight when the trace ended. A handful is normal; many means a monitor gap or a filter. |
| `N header(s) point outside the XxY mesh` | Wrong `--x-tiles`/`--y-tiles`, or the trace started mid-packet. |
| `source has no tile-port (P) events ...` | Input is a `sims.log`; see the limits in §3. |

---

## 5. Naming and definitions

| Term | Meaning |
|---|---|
| `tileN(x,y)` | Tile N at column x, row y (N = x + y·x_tiles). As a source or destination: its core, L1.5 and L2 slice. |
| `rN(x,y)` | Tile N's router. |
| `offchip` | Anything past the mesh edge: the chipset (memory, I/O), reached through tile 0's west port. |
| core N | The core in tile N (one per tile in OpenPiton). |
| NoC1/2/3 | OpenPiton's three physical networks, each with its own router per tile. |
| link `kind` | `inject` (tile → router), `eject` (router → tile), `mesh` (router → router), `to_offchip`, `from_offchip` |
| packet | One header flit + `length` body flits |
| **injected flits** | Each flit counted once, when it enters the network. What the sources generate. |
| **flit-hops** | Each flit counted on every router-to-router link it crosses. What loads the links. |
| utilization | Flits on a channel ÷ cycles. Maximum 1.0 (one 64-bit flit per cycle). |
| **latency** | From the header entering the network to the tail leaving it. Includes time spent waiting for a slow source or a busy destination. |
| hops (`mesh_hops`) | Router-to-router links crossed. A tile talking to its own L2 slice = 0. |

**Routing.** Packets are rebuilt using the routers' own rule (`dynamic_input_route_request_calc.v`): X first, then Y, then the header's fbits at the destination. Off-chip destinations route to (0,0) and leave west.

---

## 6. Python API

With the env script sourced (or `sys.path.insert(0, "<repo>/rit")`):

```python
from noctrace import load_run, metrics

run = load_run("hello_2x2.csv")            # same keyword options as the CLI: x_tiles=, y_tiles=,
                                           # clock_period=, chip_id=, and fmt="auto"|"noc_trace"|"router_mon"
run.packets      # DataFrame, columns as packets.csv
run.events       # DataFrame, one row per flit per channel (columns as events.csv.gz, plus link ids)
run.hops         # DataFrame, one row per packet per channel
run.links        # DataFrame, columns as per_link.csv
run.meta         # dict: x_tiles, y_tiles, format, file, warnings, ...
run.mesh         # Mesh(X, Y): .X, .Y, .label(node), .xy(node)
run.cycle_span() # cycles from first to last flit

metrics.total_over_time(run, bin_cycles)          # = total_over_time.csv
metrics.per_source_over_time(run, bin_cycles)     # = per_source_over_time.csv
metrics.per_dest_over_time(run, bin_cycles)
metrics.per_path(run)
metrics.per_path_over_time(run, bin_cycles, top=None)
metrics.per_link(run)
metrics.per_link_over_time(run, bin_cycles)
metrics.per_msg_type(run)
metrics.path_matrix(run, noc=None)
metrics.signature(run, window, metric="injected")  # = signatures.csv minus app/round
metrics.summary(run)                               # = summary.json
```

Example: flits sent by core 2 per 10k cycles, NoC1 only:
```python
import pandas as pd
from noctrace import load_events, Run, metrics
ev, meta = load_events("hello_2x2.csv")
run = Run(ev[ev["noc"] == 1], meta)
sig = metrics.signature(run, 10000)
print(sig[sig["core"] == 2][["cycle_start", "flits"]])
```

---

## 7. Adding another input format

Readers live in `rit/noctrace/adapters.py`. A reader returns:
- a DataFrame with columns `cycle, time, noc, x, y, dir, port, flit` (meanings as in §2; `time` may be NaN)
- a `meta` dict with at least `x_tiles`, `y_tiles` and `has_p_ports` (False if the source never logs P-port flits)

Register the reader in `ADAPTERS` with a "sniff" function that recognizes its files, and every table and plot works unchanged.

Packet rebuilding (`rit/noctrace/model.py`) assumes OpenPiton's header layout and XY routing. A different NoC needs its own `decode_cols` and `next_node`. The per-link and over-time flit counts don't depend on either.
