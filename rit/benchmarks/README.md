# Benchmarks on the 2x1 Ariane model

Bare-metal ports of the benchmarks the team picked for comparison with the Genesys2 FPGA:

| Program (run with `sims ... -vcs_run <name>`) | Benchmark | Instructions per hart in the ROI, 2x1 defaults (from QEMU) |
|---|---|---|
| `parsec_blackscholes.c` | PARSEC 3.0 blackscholes, pthreads version | ~0.9 M per run (simsmall: 4096 options, 1 run) |
| `aibench_conv2d.c` | AIBench Micro DC-AI-M1 Convolution, Pthreads version | ~6.8 M (one 227x227x3 image, 2 output channels) |

Both run one thread per hart (both cores on the 2x1), split the work the same way the pthreads code does, and mark their region of interest (ROI) so the NoC trace covers only the benchmark kernel, not the setup.

**Why bare metal and not Linux:** an RTL simulation of a Linux boot is hundreds of millions of cycles before the benchmark even starts (likely 14 hours to 2 days at the ~5,800 cycles/s measured on the lab machine for the 2x1 model). The bare-metal ports start the kernel within a few hundred thousand cycles. What they leave out is the OS: no timer interrupts, scheduler, page tables or TLB misses in the traffic. See [Comparing with the FPGA](#comparing-with-the-fpga).

## Files

```
piton/verif/diag/c/riscv/ariane/rit/
├── parsec_blackscholes.c      PARSEC kernel unchanged + bare-metal scaffolding
└── aibench_conv2d.c           AIBench conv2dSingleFilter (NCHW, SAME) + scaffolding
piton/verif/diag/assembly/include/riscv/ariane/rit/
├── bench.h                    ROI markers, barrier, cycle counters, fixed-point print
├── math.h                     exp()/log() (musl = Arm optimized-routines, as glibc)
└── bs_option_data.h           PARSEC's option table (generated)
rit/benchmarks/
├── README.md                  you are here
├── blackscholes/gen_option_data.py   regenerates bs_option_data.h from a PARSEC input file
└── qemu/run_qemu.sh           functional check in QEMU before spending hours in VCS
```

The programs live under `verif/diag` because that's where `sims` looks for tests; the headers sit in the Ariane include directory because `sims` copies the test file into the run directory before compiling it, so the include path is the only way to reach them.

## Running on the lab machine

Use `nocrun` ([`../docs/runs.md`](../docs/runs.md)): every run gets its own folder under `~/noc_runs/`, keeps running if you log out, and is analyzed when it ends. The plain `sims` command each run uses is in that run's `job.sh`.

### 1. Build the model

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1_el9 -ariane -config_rtl=MINIMAL_MONITORING 2>&1 | tee build_ariane_2x1_el9.log
```

`MINIMAL_MONITORING` stops the testbench from printing every cache transaction into `sims.log`, which otherwise reached 1.5 GB for a 10M-cycle run. Use a new `-build_id` when runs are still using the old model.

### 2. Smoke test (a minute or two each)

Tiny inputs, to check everything compiles, links and passes before the long runs:

```bash
nocrun start parsec_blackscholes.c -b ariane_2x1_el9 -D NUM_OPTIONS=16 --name bs16 --fg
nocrun start aibench_conv2d.c -b ariane_2x1_el9 -D IMG_H=32 -D IMG_W=32 --name conv32 --fg
```

**Pass looks like:** `status: PASS` and `num errors: 0` in the `run_info.txt` printed at the end. `sims_summary.log` in the run folder has `NoC trace window marker ...`, `noc_trace_mon: window opened at cycle N` and `closed at cycle M`. If the compile fails, `program/rv64_cc.log` has the compiler output.

### 3. Full runs

```bash
nocrun start parsec_blackscholes.c -b ariane_2x1_el9        # PARSEC simsmall (4096 options), 1 run
nocrun start aibench_conv2d.c -b ariane_2x1_el9             # one 227x227 image
nocrun status                                              # check on them
```

**Measured on the lab machine (2x1, VCS):**

| | ROI cycles per hart | cycles per instruction | whole run | wall time |
|---|---|---|---|---|
| blackscholes simsmall, 1 run | 2.03–2.09 M | ~2.3 | 3.3 M cycles | 20 min |
| conv2d 227x227 | ~16 M (estimate: 6.8 M instructions at ~2.3) | | ~18 M | ~1.5–2 h |

The simulator runs about 2,700–4,500 cycles/s, slower while the NoC is busy. PARSEC's own setting of 100 runs (`-D NUM_RUNS=100`) is 100 times the blackscholes ROI: more than a day.

Two things that ended earlier runs, both handled by `nocrun` and the new model:
- **`FAIL(TIMEOUT)` at exactly `-rtl_timeout` cycles** (the first conv2d run): `pc_cmp` marked three nonexistent threads per Ariane tile as active, and their timeout always ran out. Fixed in `pc_cmp.v.pyv`; models built before the fix stop every run at `-rtl_timeout` cycles, and `nocrun` sets it to 2e9 for them.
- **`Received SIGHUP (signal 1), exiting.`** (the second conv2d run): VCS installs its own handler for the hang-up signal sent when an SSH session closes, which overrides `nohup`. `nocrun` starts runs in their own session (`setsid`), which never gets that signal.

### 4. Analyze

Each run is analyzed at a 10,000-cycle window when it ends (`<run>/analysis/w10000/`). Other sizes, and several runs together:

```bash
nocrun analyze blackscholes_2x1_r1 -w 80000
nocrun compare blackscholes_2x1_r1 conv2d_2x1_r1 -w 20000
```

`noctrace` counts cycles from the ROI start for these traces.

## Options

Passed with `-gcc_args=-DNAME=VALUE`, **one `-gcc_args` per define**.

| Program | Define | Default | Meaning |
|---|---|---|---|
| both | `NTHREADS` | number of harts | threads to use (the rest idle at the barrier) |
| both | `RIT_USE_LIBM` | off | blackscholes: use the toolchain's libm for exp/log instead of `rit/math.h` (conv2d uses no math functions) |
| blackscholes | `NUM_OPTIONS` | 4096 | input size: 4 = test, 16 = simdev, 4096 = simsmall, 16384 = simmedium, 65536 = simlarge |
| blackscholes | `NUM_RUNS` | 1 | repetitions of each thread's slice; PARSEC uses 100 |
| blackscholes | `BS_DUMP` | off | print every price after the check |
| conv2d | `BATCH` | 1 | images per batch (jobs = 2 x BATCH, dealt to threads in contiguous ranges) |
| conv2d | `IMG_H`, `IMG_W` | 227, 227 | image size |

## What each port keeps and changes

### blackscholes

**Same as PARSEC:** `CNDF()`, `BlkSchlsEqEuroNoDiv()` and the `bs_thread()` loop are PARSEC's code unchanged, including its float/double mix and the order threads walk their slices. The input arrays have PARSEC's layout (five float arrays back to back in one 64-byte-aligned buffer, `otype` in another) and exactly the values PARSEC's `fscanf("%f")` reads from `in_<N>.txt` (every PARSEC input is the same 1000-row table repeated, which is what `bs_option_data.h` holds). The work split is PARSEC's: thread t does options `[t*N/T, (t+1)*N/T)`.

**Different:** no file reading (hart 0 builds the arrays from the table before the ROI), no thread creation or join (every hart calls `bs_thread()`; barriers before and after), and PARSEC's `ERR_CHK` comparison against the DerivaGem reference values runs after the ROI instead of inside the loop. `exp`/`log` come from `rit/math.h`: musl's copy of Arm's optimized-routines code, the same algorithm and tables glibc uses on Linux, compiled into the program because a libm built without `-mcmodel=medany` can't link at Ariane's 0x80000000 load address.

**Checked:** in QEMU on 1, 2 and 4 harts; all 4096 simsmall prices are within 1.4e-5 of PARSEC's own output built with GCC on x86 (float rounding differences), and `Num Errors: 0` against the reference values, same as PARSEC.

### conv2d

**Same as AIBench:** `conv2dSingleFilter()` is the NCHW + SAME-padding branch of the original line for line, with `run-pthread.sh`'s settings: NCHW, 227x227x3 input, the 3x3x3x2 filter from `conv2d.cpp`'s `main()`, strides {1,1,2,2}. Jobs (one per image and output channel) are dealt to threads the same way, the last thread taking the remainder.

**Different:** the original is C++ with nested `std::vector` (every image row is its own heap block) built with `-g` and no optimization, and it reads ImageNet JPEGs through OpenCV, 1000 images in batches of 100. Here: C with flat arrays at `-O2`, one batch of `BATCH` images, pseudo-random 0..255 pixels (the kernel has no data-dependent branches or addresses, so the pixel values don't change the traffic). Each thread reads and writes the same elements in the same order; the per-row pointer loads and the unoptimized instruction stream of the original aren't reproduced.

**Checked:** the output checksum matches the original `conv2d.cpp` (compiled with g++ on x86, fed the same pixels) at 17x20, 32x32 (batch 3) and 227x227, and hart 0 recomputes every output pixel with an independent integer reference after the ROI.

## Comparing with the FPGA

These runs give NoC traffic for the benchmark kernel on the same hardware (OpenPiton + Ariane, 2x1, the Genesys2 configuration), but not the same software stack as a Linux run on the board:

- **No OS traffic.** Under Linux there are timer interrupts, the scheduler, page-table walks on TLB misses, and the pthread create/join calls. None of that is here.
- **Input size.** PARSEC on the FPGA would normally use a bigger input (simlarge/native) and 100 runs. In simulation use simsmall and 1 run unless we decide otherwise; per-time-window signatures still compare in shape, totals don't.
- **conv2d's build.** See above: same data and access order, different instruction stream.
- **Spin-waiting.** A hart that finishes first waits at a barrier by spinning on one cached line, which generates little or no NoC traffic once the line is in its cache. On Linux it would sleep in the kernel.

## Checking changes in QEMU first

`rit/benchmarks/qemu/run_qemu.sh` builds a test the way `sims` does (same flags, same `crt.S`, `syscalls.c` and linker script) and runs it in QEMU on N harts, in seconds instead of hours. It needs `riscv64-unknown-elf-gcc` and `qemu-system-riscv64` (not on the lab machines; any Linux box or WSL works).

```bash
rit/benchmarks/qemu/run_qemu.sh parsec_blackscholes.c 2 -DNUM_OPTIONS=4096
QEMU_ICOUNT=1 rit/benchmarks/qemu/run_qemu.sh aibench_conv2d.c 2      # ROI counts = instructions
```

QEMU isn't cycle-accurate and has no NoC, so it only answers "does it build, run on N harts and get the right answer". Instruction counts with `QEMU_ICOUNT=1` are exact, which is a good basis for estimating sim time.
