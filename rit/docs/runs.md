# Running simulations with `nocrun`

`rit/bin/nocrun` starts simulations so that every run keeps its own results: nothing is overwritten, every result records how it was made, and runs survive logging out. It wraps the same `sims` command you'd type by hand.

The env script (`rit/setup/openpiton_env.sh`) puts `nocrun` on your `PATH`. If you use an older copy of the env script, add these two lines to it:
```bash
export PATH=$PITON_ROOT/rit/bin:$PATH
export NOC_RUNS=$HOME/noc_runs
```

## Where results go

```
~/noc_runs/                                 ($NOC_RUNS)
├── runs.csv                                one row per run: status, cycles, packets, load, time...
├── 2026-11-17_blackscholes_2x1_r1/
│   ├── run_info.txt                        what was run and how it ended (run.json = same, for scripts)
│   ├── job.sh                              the exact commands; rerun with: bash job.sh
│   ├── uart.log                            the program's output (Num Errors, ROI cycles per hart)
│   ├── trace.csv.gz                        the NoC trace, compressed (noctrace reads .gz directly)
│   ├── sims_summary.log                    important lines of sims.log + its last 300 lines
│   ├── program/                            diag.c, diag.exe, symbol.tbl, diag.dump, rv64_cc.log
│   ├── analysis/w10000/                    noctrace results at window/bin 10000
│   ├── analysis/w80000/                    ...and at any other size you ask for
│   └── logs/                               only with --keep-logs
├── 2026-11-17_conv2d_2x1_r1/
└── compare/20261117-153000_w10000/         nocrun compare output
```

The run folder is named `<date>_<name>_<mesh>_r<N>`. The name defaults to the program name without `parsec_` / `aibench_`, and N counts up for each run of the same name and mesh.

**What's not kept, and why:** `sims.log` (about 300 bytes per simulated cycle without `MINIMAL_MONITORING`) and Ariane's per-instruction traces `trace_hart_*.log` (about 70 bytes per instruction per core). Together that's over 1 GB per million cycles. `nocrun` points the instruction traces at `/dev/null` before the run, and after the run keeps only the important lines of `sims.log`. Add `--keep-logs` when you're debugging a run and need everything (gzipped, in `logs/`). `mem.image` is deleted too; it's rebuilt from `program/diag.exe` whenever needed.

## Commands

### Start a run

```bash
nocrun start parsec_blackscholes.c -b ariane_2x1_v2                    # simsmall, 1 run
nocrun start parsec_blackscholes.c -b ariane_2x1_v2 -D NUM_OPTIONS=16  # smoke test
nocrun start aibench_conv2d.c -b ariane_2x1_v2 -D IMG_H=64 -D IMG_W=64 --note "size test"
```

It prints the run folder and returns right away. The run carries on in the background, **safe to log out**: it's started in its own session (`setsid`), so closing your SSH session can't send it the hang-up signal that killed the second conv2d run. Add `--fg` to run in your terminal and wait.

| Option | Default | Meaning |
|---|---|---|
| `-m`, `--mesh XxY` | `2x1` | tiles |
| `-b`, `--build-id ID` | `ariane_<mesh>` | which model (`build/manycore/<ID>/simv`) |
| `-D NAME[=VALUE]` | | compile-time define, repeatable (`-gcc_args=-D...` for sims) |
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
nocrun list            # the whole index;  nocrun list --grep conv2d  for some of it
tail -f ~/noc_runs/<run>/sims.log      # watch a run live
```

`status` shows `STOPPED` for a run whose job died without finishing (machine rebooted, killed by hand). Run `nocrun finish <run>` to tidy it up and record what's there.

### Analyze at other window sizes

```bash
nocrun analyze blackscholes_2x1_r1 -w 80000          # -> <run>/analysis/w80000/
nocrun analyze blackscholes_2x1_r1 -w 5000 --tag w5000_noc2 -- --noc 2
```

A run name can be shortened to any unique part of it. Each window size gets its own folder, so results at different sizes sit side by side. Anything after `--` is passed to `noctrace` unchanged (`--noc`, `--start`/`--end`, `--signature-unit flits`, ...; see [`noctrace_reference.md`](noctrace_reference.md)).

### Compare runs

```bash
nocrun compare blackscholes_2x1_r1 conv2d_2x1_r1 -w 20000
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
nocrun start parsec_blackscholes.c -b ariane_2x1_v2 -D NUM_OPTIONS=64 --name cktest --save-at 50000
nocrun restore cktest_2x1_r1          # new run folder: continues from cycle 50,000
```

- `--save-at N` adds `+rit_save_at=N` to the run. The testbench module `rit_ckpt` (`piton/verif/env/manycore/rit_ckpt.v.pyv`) calls VCS's `$save` at cycle N, writing `checkpoint.vcs` into the run folder, **and the run keeps going**. So the same run is also the reference to check the restore against.
- `nocrun restore RUN` makes a new run folder and runs `simv -r <checkpoint>` there. The restored run picks up at cycle N and runs to the end like the original.
- A checkpoint is the size of the simulator's memory: several GB.
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
- **The lab machine is shared:** check `nproc` and `uptime` and leave cores for others. Each run uses one core and about 4–8 GB of RAM.

## The model for long runs

Build the model with the current branch and `MINIMAL_MONITORING`:
```bash
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1_v2 -ariane -config_rtl=MINIMAL_MONITORING
```

- `MINIMAL_MONITORING` turns off the testbench monitors that print every L2 and L1.5 transaction into `sims.log` (`Control signals:`, `L15_MON`, ...). The NoC trace monitor and pass/fail detection still work.
- The `pc_cmp` fix makes `-rtl_timeout` detect a hung core instead of ending every run after that many cycles. Before it, Ariane tiles had three "threads" that never finish an instruction, so their timeout always ran out.
- The checkpoint module (`rit_ckpt`) is included.
