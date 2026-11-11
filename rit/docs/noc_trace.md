# NoC traffic tracing: user guide

Record every flit moving through OpenPiton's network-on-chip during a simulation, then turn the recording into tables and plots: traffic over time, per core, per path, per link, and latency.

Two parts:

| Part | Where | What it does |
|---|---|---|
| **`noc_trace_mon`**, the trace monitor | `piton/verif/env/manycore/noc_trace_mon.v.pyv` | Testbench code compiled into every model you build. When a run has `+noc_trace`, it writes one CSV line for every flit crossing every router port. |
| **`noctrace`**, the analyzer | `rit/noctrace/` | Python tool that reads a trace and writes the tables and plots. |

This guide walks through using them. Every option, file and column is listed in the **[reference](noctrace_reference.md)**, and `python3 -m noctrace --help` prints all the options.

---

## Workflow at a glance

| Step | Command | How often | Produces |
|---|---|---|---|
| 1. Install tools | `bash rit/setup/install_deps.sh` | once per account | toolchain + VCS fixes in `~/tools`, `~/opt`, `~/.local` |
| 2. Load environment | `source rit/setup/openpiton_env.sh` | every new terminal | `sims`, `pbuild`, `PITON_ROOT`, Python path |
| 3. Build a model | `pbuild ... -build_id=<id>` | once per mesh size, and after pulling RTL/testbench changes | `build/manycore/<id>/simv`, with the monitor inside |
| 4. Run with tracing | `sims ... -vcs_run <test> -sim_run_args=+noc_trace ...` | once per experiment | `build/<trace>.csv` |
| 5. Analyze | `python3 -m noctrace <trace>.csv -o ~/noc_results` | once per trace | `~/noc_results/<trace>/`: tables and plots |

Steps 1–2 are covered in [`rhel9_vcs_setup.md`](rhel9_vcs_setup.md).

---

## Step 3: Build

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane 2>&1 | tee build_2x2.log
```

- **No extra option is needed for tracing.** On this branch the monitor is compiled into every model automatically. `pbuild` is `sims ... -vcs_build` plus the RHEL 9 fix arguments.
- **Use at least 2x2.** A 1x1 has no tile-to-tile links.
- **Rebuild after pulling** changes to RTL or the testbench. A model only contains the code that existed when it was built. Use a new `-build_id` to keep the old model.
- `-config_rtl=MINIMAL_MONITORING` is fine; the monitor still works.

To check whether an existing model has the monitor:
```bash
grep -rlq noc_trace_mon $PITON_ROOT/build/manycore/<build_id>/ && echo yes || echo "no: rebuild"
```

## Step 4: Run with tracing on

```bash
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane \
     -rtl_timeout 10000000 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=hello_2x2_r1.csv
```

| Run argument | Required | Meaning |
|---|---|---|
| `-sim_run_args=+noc_trace` | **yes** | turns tracing on (off by default) |
| `-sim_run_args=+noc_trace_file=NAME` | recommended | output file name (default `noc_trace.csv`, overwritten every run) |
| `-sim_run_args=+enable_router_mon` | no | also writes OpenPiton's built-in router lines into `sims.log` (not needed with the trace) |

Each `-sim_run_args=` carries **one** plusarg. The trace is written to the directory you ran `sims` from.

**Name traces after what they are**, e.g. `<app>_<mesh>_r<round>.csv` (`hello_2x2_r1.csv`, `bodytrack_4x4_r5.csv`). The results folder is named after the file, so the name tells you which run each set of results came from.

**Check it worked:**
```bash
grep -E "noc_trace_mon|PASS|FAIL" sims.log   # "noc_trace_mon: writing flit trace to hello_2x2_r1.csv" and PASS
head -6 hello_2x2_r1.csv                      # 4 "#" lines, the column header, then data rows
```

## Step 5: Analyze

```bash
python3 -m noctrace hello_2x2_r1.csv -o ~/noc_results
cat ~/noc_results/hello_2x2_r1/summary.txt
```

This takes seconds to about a minute, depending on trace size. Results go to `~/noc_results/hello_2x2_r1/`. Write results **outside the repo**.

### What you get

| File | What it answers |
|---|---|
| `summary.txt` | Start here: totals, per-NoC packets/flits/hops/latency, busiest link, warnings |
| `traffic_over_time.png`, `total_over_time.csv` | How much traffic is on each NoC over time? |
| `signatures/core<N>.png`, `signatures.csv` | How much does each core send over time? (one plot per core) |
| `signatures/total.png`, `signatures_total.csv` | The same, all cores added together |
| `signatures_all_cores.png` | All cores side by side on one scale |
| `injection_by_source.png`, `per_source_over_time.csv` | Who's sending, and when? (heatmap, includes off-chip) |
| `path_matrix.png`, `per_path.csv`, `top_paths.png` | Who sends to whom, how much, and how fast? |
| `link_map.png`, `per_link.csv` | Which links are busiest? |
| `per_msg_type.csv` | Which coherence messages dominate? |
| `packets.csv` | Every packet: source, destination, type, path, latency |

All columns are described in the [reference, §4](noctrace_reference.md#4-output-files).

### Viewing results from the lab machine

- **Text:** `cat`/`less` the `summary.txt` and CSVs.
- **Plots and CSVs:** in MobaXterm's **SFTP** sidebar, enter `/home/<you>/noc_results/<trace>/` in the path box. Double-click a PNG to open it on your PC, or drag files to your desktop. CSVs open in Excel.

---

## Recipes

**Flits per 10,000 cycles, per core and total, labeled for slides:**
```bash
python3 -m noctrace hello_2x2_r1.csv --bin 10000 --window 10000 --signature-unit flits \
        --app hello_world --round 1 -o ~/noc_results
```
`--bin` sets the bin size for the over-time tables and plots; `--window` sets it for the per-core signatures. Plots are titled "hello_world — Core N, Round 1".

**Choose what a core's line counts:**
```bash
... --signature-metric injected   # packets/flits the core's tile sent (default)
... --signature-metric ejected    # packets/flits delivered to the core's tile
... --signature-metric router     # everything passing through the core's router
```

**Compare several runs:** list all the traces, and give `--app`/`--round` once per trace, in the same order:
```bash
python3 -m noctrace bt_4x4_r1.csv bt_4x4_r2.csv bt_4x4_r3.csv \
        --app bodytrack --app bodytrack --app bodytrack --round 1 --round 2 --round 3 \
        --window 10000 -o ~/noc_results
cat ~/noc_results/runs_summary.csv      # one row per run
```
Use the **same `--window`/`--bin` for every run** you compare. Windows and bins are aligned to cycle 0, so the same cycle falls in the same window in every run.

**Compare mesh sizes:** build each size under its own `-build_id`, run the same test with tracing on each, then analyze the traces together as above.

**Zoom into part of a run:**
```bash
python3 -m noctrace run.csv --start 1000000 --end 1200000 --bin 1000 -o ~/noc_zoom
```

**One NoC only:**
```bash
python3 -m noctrace run.csv --noc 2 -o ~/noc_results_noc2
```

**Find the slowest packets:**
```bash
python3 -c "import pandas as pd; p=pd.read_csv('packets.csv'); print(p.nlargest(10,'latency')[['noc','src','dst','msg_name','flits','inject_cycle','latency']].to_string())"
```

**Your own analysis in Python:** see the [reference, §6](noctrace_reference.md#6-python-api).

**An old `sims.log` instead of a trace** (needs `+enable_router_mon` on that run; incomplete, see below):
```bash
python3 -m noctrace sims.log --x-tiles 2 --y-tiles 2 -o ~/noc_results
```

---

## Why the trace instead of `sims.log`

OpenPiton's built-in router monitor (`+enable_router_mon`) only logs flits leaving a router toward another router.

| Question | `sims.log` | trace |
|---|---|---|
| How busy is each router-to-router link? | ✅ exact | ✅ exact |
| How much traffic did each core send? | ⚠️ guessed | ✅ exact |
| Who sent what to whom? | ⚠️ partly, guessed | ✅ exact |
| Core ↔ its own L2 slice | ❌ invisible | ✅ |
| Memory responses from the chipset | ❌ missing | ✅ |
| Latency | ❌ | ✅ |

In the self-test, `sims.log` saw 1,010 of 1,170 packets.

---

## Sharing data

Traces and results stay **out of Git**: a trace is about 40 bytes per flit per channel, so long runs reach hundreds of MB. `build/` is git-ignored, and so are `results/`, `noc_analysis/` and trace files under `rit/`. Put traces and results in the team's shared storage, and record with each one:
- the branch commit (`git rev-parse --short HEAD`)
- the build command (mesh size, `-config_rtl` options)
- the test program and run arguments

Small summaries (`summary.json`, a few plots) are fine to commit when they back up a write-up.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| No `noc_trace_mon: writing...` line, no CSV | Model built before the monitor existed, or `+noc_trace` missing | Check the model (Step 3) and rebuild if needed; add `-sim_run_args=+noc_trace` |
| `noc_trace_mon: ERROR could not open ...` | Bad path or no write permission | Use a plain file name; run from `$PITON_ROOT/build` |
| CSV has only the header | Run ended before any NoC traffic, or the run failed | Check `sims.log` for PASS/FAIL |
| `No module named noctrace` | Env script not sourced | `source ~/openpiton/rit/setup/openpiton_env.sh` |
| `No module named pandas` / `matplotlib` | Python packages missing | `bash rit/setup/install_deps.sh` (step 8) |
| `ValueError: Multi-dimensional indexing ...` while plotting | Old `noctrace` with the lab's old system matplotlib | `git pull` (fixed) |
| `mesh size unknown; pass --x-tiles and --y-tiles` | `sims.log` input | Add `--x-tiles N --y-tiles N` |
| `no router-monitor lines found` | `sims.log` from a run without `+enable_router_mon` | Use a trace, or rerun with that plusarg |
| Warning `header(s) point outside the mesh` | Wrong mesh size given | Fix `--x-tiles`/`--y-tiles` |
| A few "cut off" / "never seen leaving" warnings | Traffic in flight when the run ended | Normal |

---

## Self-test

`rit/test/run_test.sh [X] [Y] [packets]` checks the whole flow without VCS. It builds an X×Y mesh of OpenPiton's real router RTL in Icarus Verilog, drives random traffic, runs the generated monitor unchanged, and compares `noctrace`'s results with what the testbench actually sent. It also checks that the docs cover every option and output column.

```bash
rit/test/run_test.sh 3 2 60      # needs iverilog
```

## Limits

- 2D-mesh builds only (not `xbar_config`).
- Builds with fewer tiles than x_tiles × y_tiles: edges next to a missing tile aren't traced.
- A tile whose clock is gated off isn't sampled correctly (the monitor uses the ungated chip clock).
- Speed: about 8 s per 300k flit events end to end. Use `--start/--end` or `--noc` on very long runs.
- Packet rebuilding assumes OpenPiton's header format and XY routing. Another simulator's output needs a reader ([reference, §7](noctrace_reference.md#7-adding-another-input-format)).
