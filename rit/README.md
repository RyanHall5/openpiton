# RIT NoC research additions to OpenPiton

This branch (`ryan-noc-research`) is [Princeton's OpenPiton](https://github.com/PrincetonUniversity/openpiton) plus the RIT team's additions. Everything we added lives in this `rit/` folder, except the NoC trace monitor, which has to sit in OpenPiton's testbench.

```
rit/
├── README.md                  you are here
├── setup/
│   ├── install_deps.sh        one-time install of the toolchain (+ VCS 2017 fixes with RIT_VCS=2017)
│   ├── openpiton_env.sh       source in every terminal (defines pbuild)
│   └── pthread_yield.c        source for VCS fix 2
├── docs/
│   ├── rhel9_vcs_setup.md     building and running OpenPiton + Ariane on lab machines
│   ├── command_templates.md   copy-paste templates for common commands, with examples
│   ├── noc_trace.md           NoC tracing user guide: workflow, recipes, troubleshooting
│   ├── runs.md                nocrun: run folders, options, checkpoints
│   ├── linux_sim.md           plan for booting Linux in the simulation
│   └── noctrace_reference.md  every plusarg, analysis option, output file and column
├── bin/nocrun                 start runs in their own folders, analyze, compare, checkpoint/restore
├── noctrace/                  Python package nocrun uses to analyze traces
├── benchmarks/                bare-metal benchmark ports for the 2x1 model: README, generator, QEMU check
└── test/                      self-test (Icarus, no VCS) + docs coverage check

piton/verif/env/manycore/noc_trace_mon.v.pyv   per-flit trace monitor (+noc_trace)
  (hooked up in manycore_top.v.pyv and manycore.flist / manycore.flist.jtag)
piton/tools/bin/rv64_img, piton/tools/src/sims/sims,2.0
  (pass a program's ROI marker addresses to the monitor: trace window)
piton/verif/env/manycore/rit_ckpt.v.pyv        checkpoint at a chosen cycle (+rit_save_at=N)
piton/verif/env/manycore/pc_cmp.v.pyv          (fix: Ariane tiles have one thread, so -rtl_timeout catches hangs)
piton/verif/diag/c/riscv/ariane/rit/            benchmark programs (parsec_blackscholes.c, aibench_conv2d.c)
piton/verif/diag/assembly/include/riscv/ariane/rit/   their headers (bench.h, math.h, bs_option_data.h)
```

## Documentation

| I want to... | Read |
|---|---|
| **copy a command** (build, run, analyze...) with placeholders and a real example | [`docs/command_templates.md`](docs/command_templates.md) |
| set up a lab account, build and run OpenPiton | [`docs/rhel9_vcs_setup.md`](docs/rhel9_vcs_setup.md) |
| record NoC traffic and analyze it | [`docs/noc_trace.md`](docs/noc_trace.md) (user guide) |
| look up a trace switch, analysis option, output file or column | [`docs/noctrace_reference.md`](docs/noctrace_reference.md) |
| run PARSEC blackscholes / AIBench conv2d on the 2x1 model | [`benchmarks/README.md`](benchmarks/README.md) |
| start runs without overwriting results, keep them organized, checkpoints | [`docs/runs.md`](docs/runs.md) |
| plan and findings for booting Linux in the simulation | [`docs/linux_sim.md`](docs/linux_sim.md) |

## New to this repo

```bash
git clone git@github.com:RyanHall5/openpiton.git ~/openpiton
cd ~/openpiton && git checkout ryan-noc-research
module load synopsys
bash rit/setup/install_deps.sh            # once
source rit/setup/openpiton_env.sh         # every terminal
```
Then follow [`docs/rhel9_vcs_setup.md`](docs/rhel9_vcs_setup.md) to build, and [`docs/noc_trace.md`](docs/noc_trace.md) to run and trace traffic.

## Typical NoC traffic run

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane -config_rtl=MINIMAL_MONITORING   # build once (monitor included)
nocrun start parsec_blackscholes.c                  # run in its own folder, traced and analyzed at the end
nocrun status                                       # check on it
cat ~/noc_runs/<run>/analysis/w10000/summary.txt    # results
nocrun analyze <run> -w 80000                       # another window size
```

## Working in this repo

- **Branches:** `openpiton` mirrors Princeton's code and is never committed to. Team work goes on `ryan-noc-research`, or on your own branch merged into it through a pull request.
- **Getting Princeton's updates:** add their repo as a remote once, then merge their branch:
  ```bash
  git remote add upstream https://github.com/PrincetonUniversity/openpiton.git   # once
  git fetch upstream
  git checkout openpiton && git merge --ff-only upstream/openpiton && git push origin openpiton
  git checkout ryan-noc-research && git merge openpiton
  ```
- **Add files by name.** Never run `git add -A` or `git add .`: simulation runs leave large generated files around the tree.
- **Never commit:** anything under `build/`, traces or results, the Ariane submodule pointer (`piton/design/chip/tile/ariane` shows as modified after a build; that's build output), or the patched UVM copy in `~/opt` (Synopsys's licensed code).
- Start runs with `nocrun` (they run in `~/noc_runs/`, outside the repo). Build with `pbuild` from `build/`, and do VCS smoke tests in a scratch folder, never from the repo root.
