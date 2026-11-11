# Command templates

Copy-paste templates for the commands you'll use most. Each one has:
- a **template**, where `<name>` is a placeholder you replace and `[ ... ]` marks an optional part (leave it out, or include it without the brackets)
- a **placeholder table**
- a **real example**, filled in

All commands assume a lab account set up per [`rhel9_vcs_setup.md`](rhel9_vcs_setup.md). For details on any `noctrace` option see [`noctrace_reference.md`](noctrace_reference.md); for the full workflow see [`noc_trace.md`](noc_trace.md).

**Contents**
- [Every session](#every-session): [load environment](#load-the-environment) · [get latest code](#get-the-latest-code)
- [Building](#building): [new build](#build-a-new-model) · [check monitor](#check-a-model-includes-the-trace-monitor) · [clean a failed build](#delete-a-failed-or-outdated-build)
- [Running](#running): [sim with trace](#run-a-simulation-with-tracing) · [sim without trace](#run-a-simulation-without-tracing) · [check a run](#check-a-run-worked-and-wrote-a-trace)
- [Analyzing](#analyzing): [basic](#analyze-a-trace) · [per-core signatures](#per-core-signatures-for-slides) · [compare runs](#compare-several-runs) · [time range / one NoC](#zoom-into-a-time-range-or-one-noc) · [old sims.log](#analyze-an-old-simslog)
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

One build per mesh size and configuration. The trace monitor is included automatically.

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
| `[-config_rtl=MINIMAL_MONITORING]` | optional: less monitor output in `sims.log`; the trace monitor still works |
| `<build_log>` | file to save the build output to |

```bash
# example: 2x2
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane 2>&1 | tee build_ariane_2x2.log
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

Test programs are in `$PITON_ROOT/piton/verif/diag/c/riscv/ariane/`. List them with `ls $PITON_ROOT/piton/verif/diag/c/riscv/ariane/`. For example: `hello_world.c`, `hello_world_many.c`, `add_shared_var.c`, `accu_test.c`.

### Run a simulation with tracing

```bash
# template
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=<x_tiles> -y_tiles=<y_tiles> -build_id=<build_id> -vcs_run <test> -ariane \
     -rtl_timeout <timeout> \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=<trace>
```

| Placeholder | Meaning |
|---|---|
| `<x_tiles>`, `<y_tiles>`, `<build_id>` | must match the build you're running |
| `<test>` | test program file name, e.g. `hello_world.c` |
| `<timeout>` | timeout limit; the run is killed if it goes longer. `10000000` is plenty for small tests; raise it for long programs |
| `<trace>` | trace file to write, in `$PITON_ROOT/build/`. Suggested: `<app>_<x>x<y>_r<round>.csv`. An existing file with this name is overwritten |

```bash
# example
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane \
     -rtl_timeout 10000000 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=hello_2x2_r1.csv
```

To also get OpenPiton's built-in router lines in `sims.log`, add `-sim_run_args=+enable_router_mon`. You don't need it when tracing.

### Run a simulation without tracing

```bash
# template
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=<x_tiles> -y_tiles=<y_tiles> -build_id=<build_id> -vcs_run <test> -ariane -rtl_timeout <timeout>
```

```bash
# example
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane -rtl_timeout 10000000
```

### Check a run worked and wrote a trace

`sims.log` and `fake_uart.log` are rewritten by every run, so check right after the run.

```bash
# template
cd $PITON_ROOT/build
grep -E "noc_trace_mon|PASS|FAIL" sims.log
ls -lh <trace> && head -6 <trace>
cat fake_uart.log
```

```bash
# example
cd $PITON_ROOT/build
grep -E "noc_trace_mon|PASS|FAIL" sims.log
ls -lh hello_2x2_r1.csv && head -6 hello_2x2_r1.csv
cat fake_uart.log
```

**Success:** `noc_trace_mon: writing flit trace to <trace>`, `Simulation -> PASS (HIT GOOD TRAP)`, and the trace has data rows after the 5 header lines. `fake_uart.log` is the program's `printf` output.

---

## Analyzing

Results go to `<results_dir>/<trace name without .csv>/`. Keep `<results_dir>` outside the repo.

### Analyze a trace

All tables and plots with default settings: 100 time bins, signatures counting packets each tile sent.

```bash
# template
python3 -m noctrace <trace> -o <results_dir>
```

| Placeholder | Meaning |
|---|---|
| `<trace>` | trace file (path relative to where you are, or absolute) |
| `<results_dir>` | where results go, e.g. `~/noc_results` |

```bash
# example
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv -o ~/noc_results
```

### Per-core signatures for slides

Fixed-size windows, flits or packets per core and in total, titled with the app and round.

```bash
# template
python3 -m noctrace <trace> --bin <cycles_per_bin> --window <cycles_per_window> \
        --signature-unit <unit> [--signature-metric <metric>] \
        --app <app_name> --round <round> -o <results_dir>
```

| Placeholder | Meaning |
|---|---|
| `<cycles_per_bin>` | bin width in cycles for the over-time tables and plots, e.g. `10000` |
| `<cycles_per_window>` | window width in cycles for the per-core signatures; usually the same as the bin. **Keep it the same across runs you compare** |
| `<unit>` | `packets` or `flits`: what the signature plots count |
| `[--signature-metric <metric>]` | optional, default `injected`. `injected` = what each tile sent; `ejected` = what was delivered to it; `router` = everything through its router |
| `<app_name>` | name for titles and CSVs, e.g. `hello_world` |
| `<round>` | round label for titles and CSVs, e.g. `1` |
| `<results_dir>` | where results go |

```bash
# example
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv --bin 10000 --window 10000 \
        --signature-unit flits \
        --app hello_world --round 1 -o ~/noc_results
```
```bash
# example: counting what each tile receives, in packets
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv --bin 10000 --window 10000 \
        --signature-unit packets --signature-metric ejected \
        --app hello_world --round 1 -o ~/noc_results
```

Output: `signatures/core<N>.png`, `signatures/total.png`, `signatures_all_cores.png`, `signatures.csv`, `signatures_total.csv`, plus the standard tables and plots.

### Compare several runs

Each trace is analyzed separately, and `<results_dir>/runs_summary.csv` gets one row per run. `--app` and `--round` are given once per trace, in the same order as the traces.

```bash
# template
python3 -m noctrace <trace_1> <trace_2> [<trace_3> ...] \
        --bin <cycles_per_bin> --window <cycles_per_window> --signature-unit <unit> \
        --app <app_1> --app <app_2> [--app <app_3> ...] \
        --round <round_1> --round <round_2> [--round <round_3> ...] \
        -o <results_dir>
```

```bash
# example: three rounds of the same program
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv hello_2x2_r2.csv hello_2x2_r3.csv \
        --bin 10000 --window 10000 --signature-unit flits \
        --app hello_world --app hello_world --app hello_world \
        --round 1 --round 2 --round 3 \
        -o ~/noc_results
cat ~/noc_results/runs_summary.csv
```
```bash
# example: same program on 2x2 and 4x4
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv hello_4x4_r1.csv \
        --bin 10000 --window 10000 --signature-unit flits \
        --app hello_world_2x2 --app hello_world_4x4 \
        --round 1 --round 1 \
        -o ~/noc_results
```

### Zoom into a time range or one NoC

```bash
# template
python3 -m noctrace <trace> [--start <first_cycle>] [--end <last_cycle>] [--noc <noc_list>] \
        --bin <cycles_per_bin> --name <results_name> -o <results_dir>
```

| Placeholder | Meaning |
|---|---|
| `<first_cycle>`, `<last_cycle>` | cycle range to keep |
| `<noc_list>` | NoCs to keep, comma-separated: `1`, `2`, `3`, `1,3` |
| `<results_name>` | folder name for these results, so they don't overwrite the full-run results |

```bash
# example: cycles 1,000,000-1,200,000, 1,000-cycle bins
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv --start 1000000 --end 1200000 --bin 1000 \
        --name hello_2x2_r1_zoom -o ~/noc_results
```
```bash
# example: NoC2 only
cd $PITON_ROOT/build
python3 -m noctrace hello_2x2_r1.csv --noc 2 --bin 10000 \
        --name hello_2x2_r1_noc2 -o ~/noc_results
```

### Analyze an old sims.log

Only for runs that had `-sim_run_args=+enable_router_mon`. Less complete than a trace: no tile-local traffic, no latency, sources guessed.

```bash
# template
python3 -m noctrace <sims_log> --x-tiles <x_tiles> --y-tiles <y_tiles> --name <results_name> -o <results_dir>
```

```bash
# example
cd $PITON_ROOT/build
python3 -m noctrace sims.log --x-tiles 2 --y-tiles 2 --name hello_2x2_simslog -o ~/noc_results
```

---

## Looking at results

```bash
# template
cat <results_dir>/<results_name>/summary.txt
ls <results_dir>/<results_name>/
```

```bash
# example
cat ~/noc_results/hello_2x2_r1/summary.txt
ls ~/noc_results/hello_2x2_r1/
```

**Plots on your PC:** in MobaXterm's **SFTP** sidebar, enter `/home/<username>/noc_results/<results_name>/` in the path box and double-click a PNG. For example, for username `abc1234`: `/home/abc1234/noc_results/hello_2x2_r1/`. (`echo $HOME` prints yours.)

**Top rows of any CSV:**
```bash
# template
python3 -c "import pandas as pd; print(pd.read_csv('<csv_file>').head(<rows>).to_string())"
```
```bash
# example
python3 -c "import pandas as pd; print(pd.read_csv('~/noc_results/hello_2x2_r1/per_path.csv').head(15).to_string())"
```

**Slowest packets:**
```bash
# template
python3 -c "import pandas as pd; p=pd.read_csv('<results_dir>/<results_name>/packets.csv'); print(p.nlargest(<rows>,'latency')[['noc','src','dst','msg_name','flits','inject_cycle','latency']].to_string())"
```
```bash
# example
python3 -c "import pandas as pd; p=pd.read_csv('~/noc_results/hello_2x2_r1/packets.csv'); print(p.nlargest(10,'latency')[['noc','src','dst','msg_name','flits','inject_cycle','latency']].to_string())"
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

**Success:** every line `PASS`, ending with `ALL PASS` and `PASS docs`.
