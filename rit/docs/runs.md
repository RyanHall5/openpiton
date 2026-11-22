# Running simulations with `nocrun`

`rit/bin/nocrun` starts simulations so that every run keeps its own results: nothing is overwritten, every result records how it was made, and runs survive logging out. It wraps the same `sims` command you'd type by hand.

The env script (`rit/setup/openpiton_env.sh`) puts `nocrun` on your `PATH`. If you use an older copy of the env script, add these two lines to it:
```bash
export PATH=$PITON_ROOT/rit/bin:$PATH
export NOC_RUNS=$HOME/noc_runs
```

## Where results go

Runs are sorted by **environment** (bare metal now, Linux later) and **suite**. The suite comes from the program's folder under `piton/verif/diag/c/riscv/ariane/rit/`, so nothing needs to be chosen by hand: `rit/aibench/micro/aibench_relu.c` → `aibench_micro`, `rit/parsec/parsec_blackscholes.c` → `parsec`. Programs outside `rit/` (`hello_world.c`, ...) go to `other`.

```
~/noc_runs/                                     ($NOC_RUNS)
├── runs.csv                                    index of every run: env, suite, status, cycles, checksum, ...
├── baremetal/
│   ├── aibench_micro/
│   │   ├── 2026-11-22_relu_2x1_r1/
│   │   │   ├── run_info.txt                    what was run and how it ended (run.json = same, for scripts)
│   │   │   ├── job.sh                          the exact commands; rerun with: bash job.sh
│   │   │   ├── uart.log                        the program's output (Num Errors, ROI cycles, checksum)
│   │   │   ├── trace.csv.gz                    the NoC trace, compressed (noctrace reads .gz directly)
│   │   │   ├── sims_summary.log                important lines of sims.log + its last 300 lines
│   │   │   ├── program/                        diag.c, diag.exe, symbol.tbl, diag.dump, rv64_cc.log
│   │   │   ├── analysis/w10000/                noctrace results at window/bin 10000
│   │   │   ├── analysis/w80000/                ...and at any other size you ask for
│   │   │   └── logs/                           only with --keep-logs
│   │   └── 2026-11-22_sigmoid_2x1_r1/ ...
│   └── parsec/
│       └── 2026-11-18_blackscholes_2x1_r1/
├── linux/                                      (later: the same <suite>/ folders)
├── scratch/                                    smoke tests and experiments (--scratch)
│   ├── runs.csv                                its own index
│   └── baremetal/<suite>/...
├── batches/<date>_<time>_<list>/               nocrun batch logs
└── compare/<time>_w<N>/                        nocrun compare output
```

The run folder is named `<date>_<name>_<mesh>_r<N>`. The name defaults to the program name without `parsec_` / `aibench_` (`aibench_max_pooling.c` → `max_pooling`), and N counts up for each run of the same name and mesh in that suite. Use `--name` for runs that differ from the default, e.g. `--name relu_64x64`.

**Scratch:** add `--scratch` to `nocrun start` (and to `list`, `analyze`, `status`, ...) for smoke tests and experiments. They get the same layout under `~/noc_runs/scratch/` with their own `runs.csv`, so they never mix with real results; delete the whole folder whenever you like.

**Finding runs:** every command takes a run by any unique part of its folder name (`nocrun analyze relu_2x1_r1`), wherever it is in the layout. `nocrun list --suite aibench_micro` / `--env baremetal` filters the index.

**From the old layout:** runs made before 2026-11-22 sat directly in `~/noc_runs/`. `nocrun reorganize` shows where each would go (full runs to `baremetal/<suite>/`, runs with non-default sizes, checkpoint tests or a status other than PASS to `scratch/`, and `~/noc_runs_scratch` merged into `scratch/`); `nocrun reorganize --yes` moves them and rebuilds both indexes. Runs of renamed programs get the new names (`conv2d` → `convolution`), with the old name in `run_info.txt` as `renamed from`. Nothing is deleted. A checkpoint inside a moved folder may not restore afterwards.

**What's not kept, and why:** `sims.log` (about 300 bytes per simulated cycle without `MINIMAL_MONITORING`) and Ariane's per-instruction traces `trace_hart_*.log` (about 70 bytes per instruction per core). Together that's over 1 GB per million cycles. `nocrun` points the instruction traces at `/dev/null` before the run, and after the run keeps only the important lines of `sims.log`. Add `--keep-logs` when you're debugging a run and need everything (gzipped, in `logs/`). `mem.image` is deleted too; it's rebuilt from `program/diag.exe` whenever needed.

## Commands

### Start a run

```bash
nocrun start parsec_blackscholes.c                    # simsmall, 1 run
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=16  # smoke test
nocrun start aibench_convolution.c -D IMG_H=64 -D IMG_W=64 --name convolution_64x64 --note "size test"
nocrun start aibench_relu.c -D IMG_H=16 -D IMG_W=16 --scratch --fg   # smoke test, kept in scratch/
```

It prints the run folder and returns right away. The run carries on in the background, **safe to log out**: it's started in its own session (`setsid`), so closing your SSH session can't send it the hang-up signal that killed an early run. Add `--fg` to run in your terminal and wait.

| Option | Default | Meaning |
|---|---|---|
| `-m`, `--mesh XxY` | `2x1` | tiles |
| `-b`, `--build-id ID` | `ariane_<mesh>` | which model (`build/manycore/<ID>/simv`) |
| `-D NAME[=VALUE]` | | compile-time define, repeatable (`-gcc_args=-D...` for sims) |
| `--scratch` | off | put the run in `~/noc_runs/scratch/` (own index) |
| `--name NAME` | from the program | run name in the folder name |
| `--app NAME` | the name, capitalized | label in plot titles |
| `--note TEXT` | | saved in `run_info.txt` |
| `-w`, `--window N` | 10000 | window/bin size for the automatic analysis |
| `--rtl-timeout N` | 20,000,000 | fail if a core finishes no instruction for N cycles (a hung core). For a model built before the `pc_cmp` fix it's set to 2e9 automatically, because those models stop *every* run after N cycles. Linux runs: use 2e9 (idle cores sit in `wfi`). |
| `--max-cycle N` | 2e9 | hard limit on run length |
| `--no-trace` / `--full-trace` | | no NoC trace / trace the whole run, ignoring ROI markers |
| `--trace-start N`, `--trace-stop N` | | trace only these cycles |
| `--save-at CYCLE` | | save a checkpoint at this cycle (see below) |
| `--sim-arg +PLUSARG` | | extra run-time plusarg, repeatable |
| `--sims-arg=-OPTION` | | extra `sims` option, repeatable (write it with `=`) |
| `--keep-logs` | off | keep `sims.log` and instruction traces |
| `--fg` | off | run in this terminal |

### Check on runs

```bash
nocrun status          # runs in progress (with how far their trace has got), then the last 5 finished
nocrun list            # the whole index;  nocrun list --suite aibench_micro  for some of it
tail -f <run folder>/sims.log          # watch a run live (nocrun status shows where it is)
```

`status` shows `STOPPED` for a run whose job died without finishing (machine rebooted, killed by hand). Run `nocrun finish <run>` to tidy it up and record what's there.

`nocrun reindex` rebuilds `runs.csv` from the run folders (each folder's `run.json` is the real record; `runs.csv` is just a summary of them). Use it if the index gets out of step, or after deleting run folders.

### Analyze at other window sizes

```bash
nocrun analyze blackscholes_2x1_r1 -w 80000          # -> <run>/analysis/w80000/
nocrun analyze blackscholes_2x1_r1 -w 5000 --tag w5000_noc2 -- --noc 2
```

A run name can be shortened to any unique part of it. Each window size gets its own folder, so results at different sizes sit side by side. Anything after `--` is passed to `noctrace` unchanged (`--noc`, `--start`/`--end`, `--signature-unit flits`, ...; see [`noctrace_reference.md`](noctrace_reference.md)).

### Compare runs

```bash
nocrun compare blackscholes_2x1_r1 convolution_2x1_r1 -w 20000
```

Writes one result folder per run plus `runs_summary.csv` (one row per run) to `~/noc_runs/compare/<time>_w<N>/`.

### Import results from before `nocrun`

```bash
cd ~/openpiton/build
nocrun import --name blackscholes --trace blackscholes_2x1_r1.csv --uart blackscholes_2x1_r1.uart.log \
       --sims-log blackscholes_2x1_r1.sims.log --program parsec_blackscholes.c --build-id ariane_2x1 \
       --note "first full run, 2026-11-15"
```

Creates a run folder (dated from the trace file), compresses the trace, keeps only the important lines of the log, analyzes it and adds it to `runs.csv`. The original files are left where they are; delete them yourself once you've checked the imported run.

## Checkpoints: save a run, continue it later

A checkpoint is the simulator's entire state written to a file at one cycle: every register and memory in the chip, and the C memory model behind it. Continuing from it skips everything before that cycle. That's what makes Linux runs practical: boot once (days), save, then start every benchmark from the booted state.

```bash
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=64 --name cktest --save-at 50000
nocrun restore cktest_2x1_r1          # new run folder: continues from cycle 50,000
```

- `--save-at N` adds `+rit_save_at=N` to the run. The testbench module `rit_ckpt` (`piton/verif/env/manycore/rit_ckpt.v.pyv`) calls VCS's `$save` at cycle N, writing `checkpoint.vcs` into the run folder, **and the run keeps going**. So the same run is also the reference to check the restore against.
- `nocrun restore RUN` makes a new run folder and runs `simv -r <checkpoint>` there. The restored run picks up at cycle N and runs to the end like the original.
- VCS writes `checkpoint.vcs` plus a folder `checkpoint.vcs.FILES/`; keep both. `run_info.txt` shows their total size.
- **Use VCS 2023.12 (the default).** With VCS 2017, restores die at once with `Fatal error: glibc detected an invalid stdio handle`: on restore, the C library's open-file handles are invalid, because RHEL 9's C library scrambles and randomizes internal pointers in a way the 2017 release (built for RHEL 6) doesn't handle. See [Which VCS](#which-vcs).
- **Address randomization (ASLR) is turned off for these runs** (`setarch -R`, only for that process; no root needed), so the C library loads at the same address when the checkpoint is restored. It was added while chasing the VCS 2017 failure and is kept as a precaution; `--aslr` leaves it on.
- **A checkpoint only works with the exact `simv` that saved it.** Rebuilding the model makes old checkpoints useless (`nocrun restore` warns).
- **Files opened before the save** may not be usable after a restore. The NoC trace monitor opens its file when its window first opens, so save before the window opens (before the ROI) and the trace is opened fresh in the restored run, in the restored run's folder.

**Checking a restore:** the restored run must reproduce the original from cycle N on. Compare the traces and the program output:
```bash
cd ~/noc_runs
zcmp *_cktest_2x1_r1/trace.csv.gz *_cktest_2x1_r2/trace.csv.gz && echo "traces identical"
diff <(grep -E "hart|Errors|Sum" *_cktest_2x1_r1/uart.log) <(grep -E "hart|Errors|Sum" *_cktest_2x1_r2/uart.log)
```

`fake_uart.log` is opened by the testbench at time 0, before the save. Whether the restored run can still write to it is one of the things this test shows.

## Running several at once

Each run has its own folder, so runs can go in parallel, all using the same model. Limits:
- **Don't rebuild a model while runs are using it.** Build a new `-build_id` instead.
- **VCS licenses:** each running simulation holds one, from a pool shared across RIT. If none is free, the run fails at startup with a license error. Don't take more than you need.
- **The lab machine is shared:** check `nproc` and `uptime` and leave cores for others. Each run uses one CPU core and, with `MINIMAL_MONITORING`, about 0.1–0.5 GB of RAM.

### What running together does (and doesn't) change

- **The results don't change.** A simulation is deterministic: the same program on the same model gives the same cycle counts and the same trace whether it runs alone or next to ten others. Sim cycles, ROI cycles and the NoC data are unaffected.
- **Wall time can change.** Runs on the same machine share its memory bandwidth and caches, and the CPU's clock drops a little when more cores are busy, so each run's cycles/s can be lower than alone. With a few runs on a machine with many cores the effect is usually small (a few percent to ~20%); it grows as the machine fills up or other users load it.
- **So for time estimates, use cycles, not hours.** `sim cycles` is exact; `speed (cycles/s)` says how fast that run went. A run's isolated time ≈ its sim cycles ÷ the speed of a run that ran alone. Every `run_info.txt` records `other sims running` (simulations on the machine, any user, at start and end) and the load average, so you can tell which runs had company. `nocrun list` shows the same as `others_running`.
- **To measure the slowdown on your machine:** start the same smoke test alone, then four copies at once, and compare their `speed (cycles/s)`.

### Batches

`nocrun batch` starts a list of runs as a queue, at most `-j N` at a time, in the background:

```bash
nocrun batch rit/benchmarks/aibench/micro/all.txt -j 2   # 12 runs, 2 at a time
nocrun status                                        # batch progress, then each run
cat ~/noc_runs/batches/<batch>/batch.log             # when each run started and finished
```

- The list is a text file with one run per line, written as the arguments to `nocrun start` (`#` starts a comment):
  ```
  aibench_relu.c
  aibench_fully_connected.c -D IMG_H=96 -D IMG_W=96 --name fully_connected_96x96
  ```
- Every program in the list is checked before anything starts, so a typo doesn't stop the batch halfway.
- `-j 1` (the default) runs them one after another: isolated wall times, total time = the sum. `-j 2`–`-j 4` finishes much sooner with each run slightly slower (see above).
- The batch survives logging out, like a single run. Its folder, `~/noc_runs/batches/<date>_<time>_<list name>/`, has the list, `batch.log` and each run's console output. Each run's `run_info.txt` names its batch.
- **Stop a whole batch** (the queue and its running simulations): `kill -- -$(cat ~/noc_runs/batches/<batch>/batch.pid)`. Runs that were stopped show as `STOPPED` in `nocrun status`; `nocrun finish <run>` records what they got to.

## Which VCS

Two VCS versions are installed on the lab machines:

| | VCS 2017.12 | VCS V-2023.12-SP2-1 (default) |
|---|---|---|
| where | `/tools/rhel6/synopsys/vcs_mx/current` | `/tools/synopsys-el9/vcs/V-2023.12-SP2-1-T-20240806` (W-2024.09 is there too but incomplete: no `bin/`) |
| built for | RHEL 6 | EL9 (what the workstations run) |
| needs | the three RHEL 9 workarounds (old `ld`, `pthread_yield` stub, patched UVM) | none |
| checkpoints | **restore fails**: `Fatal error: glibc detected an invalid stdio handle`, even with address randomization off | **work**: the restored run's trace is byte-for-byte identical to the original's |
| results | blackscholes smoke test: 176,671 cycles | identical: 176,671 cycles, same speed |
| select with | `RIT_VCS=2017 source ~/openpiton/rit/setup/openpiton_env.sh` | `source ~/openpiton/rit/setup/openpiton_env.sh` |

Use a **new terminal** when switching: sourcing the env script twice in one shell keeps the old `PATH` entries. A model has to run under the VCS that built it. `pbuild` records the builder in `build/manycore/<build_id>/rit_vcs_home`, `nocrun` warns on a mismatch, and every `run_info.txt` shows the VCS used.

## The model for long runs

Build the model with the current branch and `MINIMAL_MONITORING`:
```bash
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane -config_rtl=MINIMAL_MONITORING
```

- `MINIMAL_MONITORING` turns off the testbench monitors that print every L2 and L1.5 transaction into `sims.log` (`Control signals:`, `L15_MON`, ...). The NoC trace monitor and pass/fail detection still work.
- The `pc_cmp` fix makes `-rtl_timeout` detect a hung core instead of ending every run after that many cycles. Before it, Ariane tiles had three "threads" that never finish an instruction, so their timeout always ran out.
- The checkpoint module (`rit_ckpt`) is included.
