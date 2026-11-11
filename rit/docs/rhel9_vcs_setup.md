# OpenPiton + Ariane on RIT RHEL 9 Lab Workstations (VCS 2017.12)

How to build and run OpenPiton's Ariane (RISC-V) simulation model with VCS on an RIT ICE Lab workstation running **RHEL 9**, starting from a fresh account.

## Quick start

```bash
git clone git@github.com:RyanHall5/openpiton.git ~/openpiton   # or the https URL
cd ~/openpiton && git checkout ryan-noc-research
module load synopsys
bash rit/setup/install_deps.sh        # once; ~20 min, mostly building ld 2.30
source rit/setup/openpiton_env.sh     # every new terminal
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=1 -y_tiles=1 -build_id=ariane_1x1 -ariane 2>&1 | tee build.log
sims -sys=manycore -x_tiles=1 -y_tiles=1 -build_id=ariane_1x1 -vcs_run hello_world.c -ariane -rtl_timeout 10000000
```
**Pass:** the run ends with `Simulation -> PASS (HIT GOOD TRAP)`, and `build/fake_uart.log` holds the program's `printf` output.

Tip: add `source ~/openpiton/rit/setup/openpiton_env.sh` to your `~/.bashrc` if you don't want to source it by hand.

The rest of this document explains what the scripts do and why, for when something breaks.

---

## Why this is needed

The lab's VCS is **N-2017.12-SP2**, built for RHEL 6 (`/tools/rhel6/synopsys/vcs_mx/current`). The workstations run RHEL 9 (binutils 2.35, glibc 2.34, GCC 11). That OS is far newer than this VCS supports, and it breaks the VCS build in three separate ways. OpenPiton itself is fine. The fixes work around the OS mismatch without root access or changes to the lab's VCS install.

| # | Symptom | Cause | Fix |
|---|---------|-------|-----|
| 1 | `.symtab local symbol at index N (>= sh_info of M)` / `error adding symbols: bad value` | VCS writes object files that newer `ld` rejects as malformed | Build GNU `ld` 2.30 in your home dir and put it first in `PATH` |
| 2 | `undefined reference to 'pthread_yield'` | glibc 2.34 removed `pthread_yield` for new programs; VCS's save/restore library still calls it | Link in a one-line stub that calls `sched_yield()` |
| 3 | `extended character " is not valid in an identifier` in `uvm_hdl.c` | VCS's UVM source has curly "smart quotes" that GCC 11 rejects | Use a private copy of VCS's UVM folder with the quotes fixed |

## What `install_deps.sh` does

Everything goes under `$HOME`, nothing needs root, and finished steps are skipped on re-runs.

| Step | Result | Notes |
|---|---|---|
| 1. Ariane submodule | `piton/design/chip/tile/ariane` populated | Only Ariane is initialized. u-boot's submodule URL is SSH-only and isn't needed for simulation. |
| 2. RISC-V toolchain | `~/tools/riscv64-unknown-elf-gcc-8.3.0-…/` | SiFive prebuilt GCC 8.3.0. Builds the Ariane boot ROM and test programs. Missing → `Error compiling rv64 DTS`. |
| 3. `dtc` | `~/.local/bin/dtc` | Built from source (`NO_PYTHON=1`). Compiles the Ariane device tree. Missing → `Error compiling rv64 DTS`. |
| 4. Perl `Bit::Vector` | `~/perl5/` only if missing | Most accounts already have it. |
| 5. Fix 1: `ld` 2.30 | `~/opt/binutils-2.30/`, `~/ldshim/ld` → it | RHEL 9's `ld` 2.35 enforces an ELF rule (`sh_info` = index of first non-local symbol) that VCS 2017's objects break. 2.30 predates the check. `--disable-werror` and `MAKEINFO=true` let it build with GCC 11. |
| 6. Fix 2: stub | `~/opt/vcs_compat/pthread_yield.o` | From `rit/setup/pthread_yield.c`. |
| 7. Fix 3: UVM copy | `~/opt/uvm-1.1/` | Copy of `$VCS_HOME/etc/uvm-1.1` with the curly quotes in `dpi/uvm_hdl.c` replaced. The copy stays in your home dir; it's Synopsys's licensed code, so **never commit it**. |
| 8. Python packages | pandas, numpy, matplotlib (`pip --user`) | For `noctrace`. |

## What `openpiton_env.sh` sets up

- `PITON_ROOT` is the clone the script lives in, so the clone can be anywhere.
- `VCS_HOME` is set **before** `piton_settings.bash` is sourced, because that script reads it.
- `~/ldshim` goes on `PATH` **after** `piton_settings.bash`. That script puts `/usr/bin` near the front of `PATH`, and if the shim isn't ahead of it, the system `ld` wins and Fix 1 silently stops working.
- `VCS_UVM_HOME=~/opt/uvm-1.1` (Fix 3).
- **`pbuild`** is a shell function: `sims` with `-vcs_build` and the three extra VCS arguments for Fixes 1 and 2 added. It takes the same arguments as `sims` otherwise, so `pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane` builds a 2x2. Use it for every build; runs (`-vcs_run`) use plain `sims`.
- `PITON_VCS_FIX_ARGS` holds the same arguments as one variable, for scripts that call `sims` directly:
  `-vcs_build_args=-LDFLAGS -vcs_build_args=-B$HOME/opt/binutils-2.30/bin -vcs_build_args=$HOME/opt/vcs_compat/pthread_yield.o`.
  Some VCS link steps call `ld` from `PATH` (handled by the shim); others go through `g++`, which `-B` points at ld 2.30. Each `-vcs_build_args=` passes **one** argument to VCS, which avoids quoting problems.
- `PYTHONPATH` gets `rit/`, so `python3 -m noctrace` works from anywhere.

Sourcing it repeatedly in one terminal keeps prepending the same folders to `PATH`. That's harmless; `exec bash` gives a clean shell.

**Check:**
```bash
which sims dtc riscv64-unknown-elf-gcc ld   # ld -> ~/ldshim/ld
ld -v                                       # 2.30
echo $VCS_UVM_HOME                          # ~/opt/uvm-1.1
```

## Smoke test (seconds; do this before a full build)

A one-line Verilog module exercises the same link steps as a full build:
```bash
source ~/openpiton/rit/setup/openpiton_env.sh
mkdir -p ~/scratch/vcs_ld_test && cd ~/scratch/vcs_ld_test
echo 'module t; initial $finish; endmodule' > t.v

rm -rf csrc simv.daidir simv*                 # Test A: linker + pthread fixes
vcs -full64 t.v -LDFLAGS -B$HOME/opt/binutils-2.30/bin $HOME/opt/vcs_compat/pthread_yield.o && ./simv

rm -rf csrc simv.daidir simv*                 # Test B: adds UVM (Fix 3)
vcs -full64 -sverilog -ntb_opts uvm t.v -LDFLAGS -B$HOME/opt/binutils-2.30/bin $HOME/opt/vcs_compat/pthread_yield.o && ./simv
```
**Pass:** `$finish called from file "t.v", line 1.` In test B, the UVM files should come from `~/opt/uvm-1.1/`, not `/tools/...`.

Do this in a scratch folder, **not** inside the repo: VCS drops `csrc/`, `simv.daidir/` etc. in the current directory. Always `rm -rf csrc simv.daidir simv*` before retesting, because VCS compiles incrementally and reuses leftovers from failed runs.

## Building and running

Run `sims` from `$PITON_ROOT/build`, never from the repo root. Everything under `build/` is git-ignored.

```bash
cd $PITON_ROOT/build
rm -rf manycore/ariane_2x2          # clean out an earlier failed build
pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane 2>&1 | tee build_2x2.log
sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_run hello_world.c -ariane -rtl_timeout 10000000
```
- The model ends up in `build/manycore/<build_id>/`. On success `simv` exists there and the log has no `Caught a SIG`.
- `-config_rtl=MINIMAL_MONITORING` cuts monitor output but also removes OpenPiton's built-in router monitor. The NoC trace monitor still works.
- NoC traffic tracing: see [`noc_trace.md`](noc_trace.md).

## Harmless warnings

| Warning | Why it's harmless |
|---|---|
| `Warning-[LNX_OS_VERUN] Unsupported Linux version` / `Warning-[LINX_KRNL] Unsupported Linux kernel` | VCS 2017 doesn't recognize RHEL 9, but runs anyway |
| `ld: warning: ... unsupported GNU_PROPERTY_TYPE (5) type: 0xc0008002` | RHEL 9 libraries carry a CPU-feature tag `ld` 2.30 doesn't know; it links regardless. Shows up for anything built with the shim on `PATH`. |
| `vcsMsgReport1: error while loading shared libraries: libnsl.so.1` | A 32-bit VCS helper can't load. Doesn't affect builds. |
| `rv64_platform.dts: ... Warning (alias_paths): /aliases:u-boot,dm-pre-reloc` | Newer `dtc` being picky about OpenPiton's device tree; the boot ROM still builds |
| `WARNING: Building dtc using make is deprecated` | `dtc` prefers Meson now, but `make` still works |
| `git status`: `unable to access '.../BramDwc/.gitignore': Too many levels of symbolic links` | A symlink loop in the tree; git skips it |

## Troubleshooting

| Error | Likely cause | Check / fix |
|---|---|---|
| `vcs: command not found` / `sims: command not found` | Env script not sourced in this terminal | `source ~/openpiton/rit/setup/openpiton_env.sh` |
| `dtc: No such file or directory` → `Error compiling rv64 DTS` | `dtc` missing or not on `PATH` | Re-run `install_deps.sh`; `which dtc` must show `~/.local/bin/dtc` |
| `riscv64-unknown-elf-gcc: No such file or directory` → `Error compiling rv64 DTS` | Toolchain missing or `RISCV` points elsewhere | Re-run `install_deps.sh`; check `echo $RISCV` |
| `Can't locate Bit/Vector.pm` | Perl module missing | Re-run `install_deps.sh`, then re-source the env script |
| `.symtab local symbol at index ... (>= sh_info of ...)` | System `ld` used instead of 2.30 | `which ld` must show `~/ldshim/ld` |
| Same `sh_info` error on a line starting `/usr/bin/ld:` | The `g++` link step didn't get `-B` | Build with `pbuild` (or add `$PITON_VCS_FIX_ARGS` to `sims`) |
| `undefined reference to 'pthread_yield'` | Stub not built or not passed | `ls ~/opt/vcs_compat/pthread_yield.o`; build with `pbuild` (or add `$PITON_VCS_FIX_ARGS` to `sims`) |
| `extended character “ is not valid in an identifier` (`uvm_hdl.c`) | VCS using its own UVM | `echo $VCS_UVM_HOME` must print `~/opt/uvm-1.1` |
| `simv not found in .../build/manycore/<id>` | Not built, or `-build_id` mismatch | `ls $PITON_ROOT/build/manycore/`; rebuild with the same `-build_id` |
| A fix was applied but the same error persists | VCS reused stale incremental files | `rm -rf $PITON_ROOT/build/manycore/<id>` and rebuild |

## Files outside the repo

```
~/tools/riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-centos6/   RISC-V toolchain
~/.local/bin/dtc                       device tree compiler
~/perl5/                               Bit::Vector, only if it was missing
~/opt/binutils-2.30/                   GNU ld 2.30 (Fix 1)
~/ldshim/ld -> ~/opt/binutils-2.30/bin/ld
~/opt/vcs_compat/pthread_yield.{c,o}   Fix 2
~/opt/uvm-1.1/                         patched UVM copy (Fix 3) - never commit
~/.cache/openpiton_deps/               downloads and build trees; safe to delete
```

The proper long-term fix is a newer VCS release that officially supports RHEL 8/9. If the lab installs one, Fixes 1–3 shouldn't be needed. Steps 1–4 still will be.
