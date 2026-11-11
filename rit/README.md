# RIT NoC research additions to OpenPiton

This branch (`ryan-noc-research`) is [Princeton's OpenPiton](https://github.com/PrincetonUniversity/openpiton) plus the RIT team's additions. Everything we added lives in this `rit/` folder, except the NoC trace monitor, which has to sit in OpenPiton's testbench.

```
rit/
├── README.md                  you are here
├── setup/
│   ├── install_deps.sh        one-time install of toolchain + RHEL 9 / VCS 2017 fixes
│   ├── openpiton_env.sh       source in every terminal
│   └── pthread_yield.c        source for VCS fix 2
├── docs/
│   ├── rhel9_vcs_setup.md     building and running OpenPiton + Ariane on lab machines
│   └── noc_trace.md           tracing NoC flits and analyzing them with noctrace
├── noctrace/                  Python package: parse + analyze flit traces
└── test/                      self-test for the trace flow (Icarus, no VCS)

piton/verif/env/manycore/noc_trace_mon.v.pyv   per-flit trace monitor (+noc_trace)
  (hooked up in manycore_top.v.pyv and manycore.flist / manycore.flist.jtag)
```

## New to this repo

```bash
git clone git@github.com:RyanHall5/openpiton.git ~/openpiton
cd ~/openpiton && git checkout ryan-noc-research
module load synopsys
bash rit/setup/install_deps.sh            # once
source rit/setup/openpiton_env.sh         # every terminal
```
Then follow [`docs/rhel9_vcs_setup.md`](docs/rhel9_vcs_setup.md) to build and run, and [`docs/noc_trace.md`](docs/noc_trace.md) to trace traffic.

## Typical NoC traffic run

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane     # build (sims -vcs_build + fixes)
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane \
     -rtl_timeout 10000000 -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=hello_2x2.csv
python3 -m noctrace hello_2x2.csv -o ~/noc_results
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
- Run `sims` from `build/`, and VCS smoke tests from a scratch folder, never from the repo root.
