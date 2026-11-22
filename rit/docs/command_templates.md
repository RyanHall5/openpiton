# Command templates

Copy-paste templates for the commands you'll use most. Each one has:
- a **template**, where `<name>` is a placeholder you replace and `[ ... ]` marks an optional part (leave it out, or include it without the brackets)
- a **placeholder table**
- a **real example**, filled in

All commands assume a lab account set up per [`rhel9_vcs_setup.md`](rhel9_vcs_setup.md). For the full workflow see [`noc_trace.md`](noc_trace.md) and [`runs.md`](runs.md); every analysis option and output column is in [`noctrace_reference.md`](noctrace_reference.md).

**Contents**
- [Every session](#every-session): [load environment](#load-the-environment) · [get latest code](#get-the-latest-code)
- [Building](#building): [new build](#build-a-new-model) · [check monitor](#check-a-model-includes-the-trace-monitor) · [clean a failed build](#delete-a-failed-or-outdated-build)
- [Running](#running): [start a run](#start-a-run) · [batch](#start-a-batch-of-runs) · [check on runs](#check-on-runs) · [checkpoint restore](#continue-a-run-from-its-checkpoint)
- [Analyzing](#analyzing): [window size](#another-window-size) · [compare runs](#compare-runs) · [flits / one NoC / time range](#variants-flits-one-noc-a-time-range)
- [Looking at results](#looking-at-results)
- [One-time / occasional](#one-time--occasional): [first-time setup](#first-time-setup-of-an-account) · [self-test](#self-test-no-vcs)

---

## Every session

### Load the environment

Run in every new terminal before anything else.

```bash
# template
source <repo>/rit/setup/openpiton_env.sh
```

| Placeholder | Meaning |
|---|---|
| `<repo>` | where you cloned the fork |

```bash
# example
source ~/openpiton/rit/setup/openpiton_env.sh
```

This uses VCS V-2023.12 (the EL9 build). For a model built with the old VCS 2017, open a new terminal and use `RIT_VCS=2017 source ~/openpiton/rit/setup/openpiton_env.sh` instead ([`rhel9_vcs_setup.md`](rhel9_vcs_setup.md#which-vcs)).

### Get the latest code

```bash
# template
cd <repo> && git pull
```

```bash
# example
cd ~/openpiton && git pull
```

If the pull changed anything under `piton/` (RTL or testbench), **rebuild your models** before the next run.

---

## Building

### Build a new model

One build per mesh size and configuration. The trace monitor is included automatically. Name it `ariane_<x>x<y>`: that's the model `nocrun` uses by default.

```bash
# template
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=<x_tiles> -y_tiles=<y_tiles> -build_id=<build_id> -ariane [-config_rtl=MINIMAL_MONITORING] 2>&1 | tee <build_log>
```

| Placeholder | Meaning |
|---|---|
| `<x_tiles>` | mesh width (tiles per row), e.g. `2`, `4` |
| `<y_tiles>` | mesh height (rows), e.g. `2`, `4` |
| `<build_id>` | name for this model; it's built in `build/manycore/<build_id>/`. Suggested: `ariane_<x>x<y>` |
| `[-config_rtl=MINIMAL_MONITORING]` | recommended: keeps `sims.log` small on long runs; the trace monitor still works |
| `<build_log>` | file to save the build output to |

```bash
# example: the 2x1 used for the benchmarks
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane -config_rtl=MINIMAL_MONITORING 2>&1 | tee build_ariane_2x1.log
```
```bash
# example: 4x4 with minimal monitoring
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=4 -y_tiles=4 -build_id=ariane_4x4 -ariane -config_rtl=MINIMAL_MONITORING 2>&1 | tee build_ariane_4x4.log
```

**Success:** `build/manycore/<build_id>/simv` exists and `grep "Caught a SIG" <build_log>` prints nothing.

### Check a model includes the trace monitor

```bash
# template
grep -rlq noc_trace_mon $PITON_ROOT/build/manycore/<build_id>/ && echo yes || echo "no: rebuild"
```

```bash
# example
grep -rlq noc_trace_mon $PITON_ROOT/build/manycore/ariane_2x2/ && echo yes || echo "no: rebuild"
```

### Delete a failed or outdated build

Do this before rebuilding under the same `<build_id>`; VCS otherwise reuses stale files.

```bash
# template
rm -rf $PITON_ROOT/build/manycore/<build_id>
```

```bash
# example
rm -rf $PITON_ROOT/build/manycore/ariane_2x2
```

---

## Running

Every run goes through `nocrun`: each run gets its own folder under `~/noc_runs/`, survives logging out, records exactly how it was made, and is analyzed when it ends. Details: [`runs.md`](runs.md).

Test programs are in `$PITON_ROOT/piton/verif/diag/c/riscv/ariane/` (give just the file name; `nocrun` finds it). The benchmarks are in its `rit/` folder: `parsec_blackscholes.c` and `aibench_<kernel>.c` for conv2d, matmul, relu, sigmoid, tanh, max_pool, avg_pool, batch_norm, dropout, multiply, softmax and add ([`../benchmarks/README.md`](../benchmarks/README.md)). Small tests: `hello_world.c`, `hello_world_many.c`, `add_shared_var.c`.

### Start a run

```bash
# template
nocrun start <program> [-m <XxY>] [-D <NAME>=<value> ...] [--name <name>] [--note "<text>"] \
             [--trace-start <cycle>] [--trace-stop <cycle>] [--full-trace] [--save-at <cycle>] [--fg]
```

| Placeholder | Meaning |
|---|---|
| `<program>` | test program file name, e.g. `parsec_blackscholes.c` |
| `[-m <XxY>]` | mesh, default `2x1`. Uses the model `ariane_<XxY>` (`-b <build_id>` for another) |
| `[-D <NAME>=<value>]` | compile-time setting, repeatable, e.g. `-D NUM_OPTIONS=16` (blackscholes input size) or `-D IMG_H=32 -D IMG_W=32` (conv2d image size) |
| `[--name <name>]` | run name in the folder name; default the program name without `parsec_`/`aibench_` |
| `[--note "<text>"]` | saved in `run_info.txt` |
| `[--trace-start/--trace-stop <cycle>]` | trace only these cycles. Programs with ROI markers (the benchmarks) are already traced only in their ROI |
| `[--full-trace]` | ignore the program's ROI markers and trace the whole run |
| `[--save-at <cycle>]` | save a checkpoint at this cycle; the run keeps going |
| `[--fg]` | run in this terminal and wait (default: background, safe to log out) |

```bash
# examples
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=16 --name bs16 --fg   # smoke test (~176,671 cycles), wait for it
nocrun start parsec_blackscholes.c                                      # full run: simsmall, ~20 min
nocrun start aibench_conv2d.c                                           # full run: one 227x227 image, ~4 h
nocrun start aibench_conv2d.c -D IMG_H=32 -D IMG_W=32 --name conv32     # conv2d smoke test
nocrun start hello_world_many.c --trace-start 50000 --trace-stop 150000 # cycle window on any program
```

For throwaway runs, put them in another folder so they stay out of your real index: `NOC_RUNS=~/noc_runs_scratch nocrun start ...`

### Start a batch of runs

```bash
# template
nocrun batch <list_file> [-j <at_a_time>] [--name <name>]
```

| Placeholder | Meaning |
|---|---|
| `<list_file>` | text file, one run per line, written as the arguments to `nocrun start` (`#` = comment) |
| `[-j <at_a_time>]` | runs at the same time; default 1 (one after another) |
| `[--name <name>]` | batch name; default the list file's name |

```bash
# example: every AIBench port, 2 at a time
nocrun batch rit/benchmarks/aibench/all.txt -j 2
# stop a whole batch, including its running simulations
kill -- -$(cat ~/noc_runs/batches/<batch_folder>/batch.pid)
```

Results don't depend on how many run at once; wall time does a little ([runs.md](runs.md#what-running-together-does-and-doesnt-change)).

### Check on runs

```bash
nocrun status                          # what's running, the last 5 finished
nocrun list                            # every run (runs.csv);  nocrun list --grep conv2d
cat ~/noc_runs/<run>/run_info.txt      # one run: PASS/FAIL, cycles, ROI per core, trace, checkpoint
cat ~/noc_runs/<run>/uart.log          # the program's printf output: Num Errors, ROI cycles
```

**Success:** `status` is `PASS`, `uart.log` ends with `Num Errors: 0`, and `run_info.txt` lists a trace and `analysis: w10000`. AIBench ports at their default size also show `checksum: 0x... (matches original)`.

### Continue a run from its checkpoint

```bash
# template
nocrun restore <run> [--name <name>] [--fg]
```

```bash
# example
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=64 --name cktest --save-at 50000 --fg
nocrun restore cktest --fg             # new run folder, continues from cycle 50,000
```

A checkpoint only restores into the exact model that saved it; rebuilding the model makes it useless.

---

## Analyzing

Every run is analyzed at 10,000-cycle windows when it ends. These commands make more analyses; each goes in its own folder, so nothing is overwritten. `<run>` can be any unique part of a run folder's name.

### Another window size

```bash
# template
nocrun analyze <run> [<run> ...] -w <window>
```

| Placeholder | Meaning |
|---|---|
| `<window>` | window/bin size in cycles. Results go to `<run>/analysis/w<window>/`. **Use the same size for runs you compare** |

```bash
# example
nocrun analyze conv2d_2x1_r1 -w 80000
```

### Compare runs

```bash
# template
nocrun compare <run> <run> [...] -w <window>
```

```bash
# example
nocrun compare blackscholes conv2d -w 20000
```

Writes one result folder per run plus `runs_summary.csv` (one row per run) to `~/noc_runs/compare/<time>_w<window>/`.

### Variants: flits, one NoC, a time range

Anything after `--` is passed to the analysis unchanged ([reference §3](noctrace_reference.md#3-noctrace-command-line)). Give each variant its own `--tag` (its folder name under `analysis/`).

```bash
# template
nocrun analyze <run> -w <window> --tag <tag> -- [--signature-unit flits] [--signature-metric <metric>] \
               [--noc <noc_list>] [--start <first_cycle>] [--end <last_cycle>]
```

| Placeholder | Meaning |
|---|---|
| `<tag>` | folder name under `<run>/analysis/`, e.g. `w10000_flits` |
| `--signature-unit flits` | count flits instead of packets in the per-core signatures |
| `<metric>` | `injected` (default) = what each tile sent; `ejected` = what was delivered to it; `router` = everything through its router |
| `<noc_list>` | NoCs to keep, comma-separated: `1`, `2`, `3`, `1,3` |
| `<first_cycle>`, `<last_cycle>` | cycle range to keep, counted from the ROI start |

```bash
# examples
nocrun analyze blackscholes -w 10000 --tag w10000_flits -- --signature-unit flits
nocrun analyze blackscholes -w 10000 --tag w10000_noc2 -- --noc 2
nocrun analyze blackscholes -w 1000 --tag zoom -- --start 1000000 --end 1200000
```

---

## Looking at results

```bash
# template
cat ~/noc_runs/<run_folder>/analysis/<tag>/summary.txt
ls ~/noc_runs/<run_folder>/analysis/<tag>/
```

```bash
# example
cat ~/noc_runs/2026-11-17_conv2d_2x1_r1/analysis/w10000/summary.txt
```

**Plots on your PC:** in MobaXterm's **SFTP** sidebar, enter `/home/<username>/noc_runs/<run_folder>/analysis/w10000/` in the path box and double-click a PNG. (`echo $HOME` prints your home folder.)

**Top rows of any CSV:**
```bash
# template
python3 -c "import pandas as pd; print(pd.read_csv('<csv_file>').head(<rows>).to_string())"
```
```bash
# example
cd ~/noc_runs/2026-11-17_conv2d_2x1_r1/analysis/w10000
python3 -c "import pandas as pd; print(pd.read_csv('per_path.csv').head(15).to_string())"
```

**Slowest packets:**
```bash
# example, from inside an analysis folder
python3 -c "import pandas as pd; p=pd.read_csv('packets.csv'); print(p.nlargest(10,'latency')[['noc','src','dst','msg_name','flits','inject_cycle','latency']].to_string())"
```

---

## One-time / occasional

### First-time setup of an account

```bash
# template
git clone git@github.com:RyanHall5/openpiton.git <repo>
cd <repo>
module load synopsys
bash rit/setup/install_deps.sh
source rit/setup/openpiton_env.sh
```

```bash
# example
git clone git@github.com:RyanHall5/openpiton.git ~/openpiton
cd ~/openpiton
module load synopsys
bash rit/setup/install_deps.sh
source rit/setup/openpiton_env.sh
```

The clone lands on `ryan-noc-research` (the default branch). `install_deps.sh` takes about 20 minutes the first time and is safe to re-run. Use the `https://github.com/RyanHall5/openpiton.git` URL instead if you don't have a GitHub SSH key on the machine.

### Self-test (no VCS)

Checks the trace monitor and `noctrace` on a mesh of the real router RTL in Icarus, and that the docs cover every option. Needs `iverilog`.

```bash
# template
<repo>/rit/test/run_test.sh [<x_tiles> <y_tiles> <packets_per_source>]
```

```bash
# example
~/openpiton/rit/test/run_test.sh 3 2 60
```

**Success:** every line `PASS`, with `ALL PASS`, `WINDOW ALL PASS` after each window check, and `PASS docs` at the end.
