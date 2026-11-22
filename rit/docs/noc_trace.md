# NoC traffic tracing: user guide

Record every flit moving through OpenPiton's network-on-chip during a simulation, then turn the recording into tables and plots: traffic over time, per core, per path, per link, and latency.

Three parts, and you only type commands for one of them (`nocrun`):

| Part | Where | What it does |
|---|---|---|
| **`nocrun`** | `rit/bin/nocrun` | Starts a run in its own folder with tracing on, and analyzes the trace when the run ends. Details: [`runs.md`](runs.md). |
| **`noc_trace_mon`**, the trace monitor | `piton/verif/env/manycore/noc_trace_mon.v.pyv` | Testbench code compiled into every model you build. It writes one CSV line for every flit crossing every router port. |
| **`noctrace`**, the analyzer | `rit/noctrace/` | The Python program `nocrun` runs on each trace to make the tables and plots. |

For copy-paste commands see **[command templates](command_templates.md)**. Every analysis option, output file and column is listed in the **[reference](noctrace_reference.md)**.

---

## Workflow at a glance

| Step | Command | How often | Produces |
|---|---|---|---|
| 1. Install tools | `bash rit/setup/install_deps.sh` | once per account | toolchain in `~/tools`, `~/opt`, `~/.local` |
| 2. Load environment | `source rit/setup/openpiton_env.sh` | every new terminal | `pbuild`, `nocrun`, `PITON_ROOT` |
| 3. Build a model | `pbuild ... -build_id=<id>` | once per mesh size, and after pulling RTL/testbench changes | `build/manycore/<id>/simv`, with the monitor inside |
| 4. Run | `nocrun start <program>.c` | once per experiment | `~/noc_runs/<env>/<suite>/<run>/`: trace, output, analysis |
| 5. Other window sizes | `nocrun analyze <run> -w <N>` | as needed | `<run folder>/analysis/w<N>/` |

Steps 1–2 are covered in [`rhel9_vcs_setup.md`](rhel9_vcs_setup.md).

---

## Step 3: Build

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane -config_rtl=MINIMAL_MONITORING 2>&1 | tee build_2x1.log
```

- **No extra option is needed for tracing.** On this branch the monitor is compiled into every model automatically.
- **Name the model `ariane_<X>x<Y>`.** `nocrun` uses that name by default for the mesh you ask for.
- **Rebuild after pulling** changes to RTL or the testbench. A model only contains the code that existed when it was built.
- `MINIMAL_MONITORING` keeps `sims.log` small; the trace monitor still works.

## Step 4: Run

```bash
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=16 --name bs16 --fg   # small test, wait for it
nocrun start parsec_blackscholes.c                                      # full run, in the background
nocrun status                                                           # what's running / just finished
```

Tracing is on by default. When the run ends, `nocrun` compresses the trace into the run folder and analyzes it at 10,000-cycle windows. All `start` options are in [`runs.md`](runs.md).

**Check it worked:** `cat <run folder>/run_info.txt` shows PASS/FAIL, cycles, the ROI cycles per core and the trace size; `sims_summary.log` has the lines where the trace window opened and closed.

### Tracing only part of a run

Benchmarks spend a long time setting up before the part you care about, so the monitor can trace only a window of the run:

- **Programs with ROI markers** (the benchmarks in `piton/verif/diag/c/riscv/ariane/rit/`, or anything that calls `noc_roi_begin()` / `noc_roi_end()` from `rit/bench.h`): nothing to add. The trace covers only the region of interest. Add `--full-trace` to trace the whole run anyway.
- **Any program, by cycle:** `--trace-start <cycle>` and/or `--trace-stop <cycle>`.

Outside the window nothing is written. The analysis counts cycles from where the window opened (cycle 0 = ROI start), so signatures from different runs line up at the ROI. A few packets crossing the network exactly when the window opens or closes are traced on only part of their route, and show up as "never seen leaving" warnings; that's expected. Details: [reference, §1 Trace window](noctrace_reference.md#trace-window).

## Step 5: Look at the results

Everything for a run is in its folder, `~/noc_runs/<env>/<suite>/<date>_<name>_<mesh>_r<N>/` (e.g. `~/noc_runs/baremetal/aibench_micro/2026-11-22_relu_2x1_r1/`; [runs.md](runs.md#where-results-go)). The analysis is in `analysis/w<N>/`, one folder per window size:

```bash
cat ~/noc_runs/baremetal/<suite>/<run>/analysis/w10000/summary.txt
nocrun analyze <run> -w 80000          # another window size -> analysis/w80000/
nocrun compare <run> <run> -w 20000    # several runs together -> ~/noc_runs/compare/...
```

A run name can be shortened to any unique part of it (`nocrun analyze convolution -w 80000`).

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
- **Plots and CSVs:** in MobaXterm's **SFTP** sidebar, enter `/home/<you>/noc_runs/baremetal/<suite>/<run>/analysis/w10000/` in the path box. Double-click a PNG to open it on your PC, or drag files to your desktop. CSVs open in Excel.

---

## Recipes

Anything after `--` in `nocrun analyze` or `nocrun compare` is passed to the analysis unchanged. Give each variant its own `--tag` (the folder name under `analysis/`) so it doesn't replace the plain `w<N>` results.

**Count flits instead of packets in the signatures:**
```bash
nocrun analyze <run> -w 10000 --tag w10000_flits -- --signature-unit flits
```

**Choose what a core's line counts:**
```bash
... -- --signature-metric injected   # packets/flits the core's tile sent (default)
... -- --signature-metric ejected    # packets/flits delivered to the core's tile
... -- --signature-metric router     # everything passing through the core's router
```

**Zoom into part of a run** (cycles counted from the ROI start):
```bash
nocrun analyze <run> -w 1000 --tag zoom_1M -- --start 1000000 --end 1200000
```

**One NoC only:**
```bash
nocrun analyze <run> -w 10000 --tag w10000_noc2 -- --noc 2
```

**Compare runs:** use the **same window for every run** you compare. Windows are aligned to cycle 0, so the same cycle falls in the same window in every run.
```bash
nocrun compare blackscholes convolution -w 20000     # one folder per run + runs_summary.csv
```

**Find the slowest packets:**
```bash
python3 -c "import pandas as pd; p=pd.read_csv('packets.csv'); print(p.nlargest(10,'latency')[['noc','src','dst','msg_name','flits','inject_cycle','latency']].to_string())"
```

**Your own analysis in Python:** see the [reference, §6](noctrace_reference.md#6-python-api).

---

## Sharing data

Run folders stay **out of Git**: a trace is about 40 bytes per flit per channel, so long runs reach hundreds of MB. Put run folders in the team's shared storage. Each one already records how it was made (`run_info.txt`: commit, model, VCS, program, options, and `job.sh`: the exact commands).

Small summaries (`summary.json`, a few plots) are fine to commit when they back up a write-up.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `run_info.txt` shows no trace | Model built before the monitor existed, or the trace window never opened (ROI markers never reached, `--trace-start` past the end of the run) | `grep -rlq noc_trace_mon $PITON_ROOT/build/manycore/<id>/ && echo yes`; rebuild if no. Check `sims_summary.log` for `window opened` and PASS/FAIL |
| `noc_trace_mon: ERROR could not open ...` | No write permission in the runs folder | Check `$NOC_RUNS` points somewhere you can write |
| Trace has only the header | Run ended before any NoC traffic, or the run failed | Check `sims_summary.log` for PASS/FAIL |
| `nocrun: command not found`, `No module named noctrace` | Env script not sourced | `source ~/openpiton/rit/setup/openpiton_env.sh` |
| `No module named pandas` / `matplotlib` | Python packages missing | `bash rit/setup/install_deps.sh` (step 8) |
| A few "cut off" / "never seen leaving" warnings | Traffic in flight at a window edge or when the run ended | Normal |

---

## Self-test

`rit/test/run_test.sh [X] [Y] [packets]` checks the trace monitor and the analysis without VCS. It builds an X×Y mesh of OpenPiton's real router RTL in Icarus Verilog, drives random traffic, runs the generated monitor unchanged, and compares the analysis with what the testbench actually sent. It then reruns with trace windows (cycle limits, and ROI marker PCs from stand-in Ariane cores) and checks that every packet inside a window is traced completely and nothing outside is. It also checks that the reference covers every option and output column.

```bash
rit/test/run_test.sh 3 2 60      # needs iverilog
```

## Limits

- 2D-mesh builds only (not `xbar_config`).
- ROI markers (begin/end PCs) need Ariane builds and physical addresses (bare-metal programs); cycle limits work with any core. Under Linux, programs run at virtual addresses, so the markers need a different trigger.
- Builds with fewer tiles than x_tiles × y_tiles: edges next to a missing tile aren't traced.
- A tile whose clock is gated off isn't sampled correctly (the monitor uses the ungated chip clock).
- Analysis speed: about 8 s per 300k flit events. Use `--start/--end` or `--noc` (after `--`) on very long runs.
- Packet rebuilding assumes OpenPiton's header format and XY routing.
