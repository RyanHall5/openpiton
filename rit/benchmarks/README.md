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

### 1. Pull and rebuild the 2x1 model

The trace monitor changed (trace window), so models built before this change need rebuilding. The script changes (`sims`, `rv64_img`) need no rebuild.

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd ~/openpiton && git pull
cd $PITON_ROOT/build
rm -rf manycore/ariane_2x1
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane 2>&1 | tee build_ariane_2x1.log
```

### 2. Smoke test (a minute or two each)

Tiny inputs, to check everything compiles, links and passes on the real model before the long runs:

```bash
cd $PITON_ROOT/build
sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run parsec_blackscholes.c -ariane \
     -finish_mask 0x11 -rtl_timeout 10000000 -max_cycle=2000000000 -gcc_args=-DNUM_OPTIONS=16 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=bs16_2x1_smoke.csv
grep -E "window marker|noc_trace_mon|PASS|FAIL|max cycles" sims.log
cat fake_uart.log

sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run aibench_conv2d.c -ariane \
     -finish_mask 0x11 -rtl_timeout 10000000 -max_cycle=2000000000 \
     -gcc_args=-DIMG_H=32 -gcc_args=-DIMG_W=32 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=conv32_2x1_smoke.csv
grep -E "window marker|noc_trace_mon|PASS|FAIL|max cycles" sims.log
cat fake_uart.log
```

**Pass looks like:**
- `sims: NoC trace window marker noc_roi_begin at ...` and `... noc_roi_end at ...`
- `noc_trace_mon: window opened at cycle N`, then `window closed at cycle M`
- `Simulation -> PASS (HIT GOOD TRAP)`
- in `fake_uart.log`: `Num Errors: 0` and each hart's ROI cycle and instruction counts

`-max_cycle=2000000000` lifts the testbench's default run limit (30,000 cycles in `monitor.v`) so long runs aren't cut off; it's harmless if your runs already went past it. `-finish_mask 0x11` waits for both cores to reach the end.

If the compile step fails, `rv64_cc.log` in the build directory has the compiler output.

### 3. Full runs

```bash
cd $PITON_ROOT/build
# blackscholes, PARSEC simsmall input (4096 options), 1 run
sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run parsec_blackscholes.c -ariane \
     -finish_mask 0x11 -rtl_timeout 10000000 -max_cycle=2000000000 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=blackscholes_2x1_r1.csv

# conv2d, one 227x227 image
sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run aibench_conv2d.c -ariane \
     -finish_mask 0x11 -rtl_timeout 10000000 -max_cycle=2000000000 \
     -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=conv2d_2x1_r1.csv
```

Each run overwrites `sims.log` and `fake_uart.log`, so copy them next to the trace if you want to keep them (e.g. `cp fake_uart.log blackscholes_2x1_r1.uart.log`).

**How long:** the smoke tests tell you. Divide each hart's "ROI cycles" (printed in `fake_uart.log`) by the run's wall time to get the sim speed, then scale. Rough guess from the instruction counts above, assuming Ariane averages 1.5-3 cycles per instruction on this code (not measured yet): blackscholes simsmall is roughly 2-5 million cycles (5-15 minutes at ~5,800 cycles/s); conv2d at 227x227 is roughly 7-15 million (20-45 minutes). PARSEC's own setting of 100 runs (`-gcc_args=-DNUM_RUNS=100`) is 100 times that: an overnight-plus run, best left for when we know we need it. Long runs keep going if you log out only if started with `nohup` or inside `tmux`/`screen`.

### 4. Analyze

`noctrace` counts cycles from the ROI start for these traces:

```bash
python3 -m noctrace blackscholes_2x1_r1.csv --bin 10000 --window 10000 --app Blackscholes --round 1 -o ~/noc_results
cat ~/noc_results/blackscholes_2x1_r1/summary.txt
```

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
