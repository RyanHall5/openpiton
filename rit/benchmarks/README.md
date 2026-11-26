# Benchmarks on the 2x1 Ariane model

Bare-metal ports of the benchmarks the team picked for comparison with the Genesys2 FPGA: PARSEC blackscholes and the AIBench Micro kernels (Pthreads versions). Parsa and Denis run the same C files on the board, so sim and FPGA run identical code.

| Program | Benchmark | AIBench source | Default input | ROI instructions per hart (QEMU, 2 harts) |
|---|---|---|---|---|
| **`rit/parsec/`** | | | | |
| `parsec_blackscholes.c` | PARSEC 3.0 blackscholes | | 4096 options (simsmall), 1 run | ~0.9 M |
| **`rit/aibench/micro/`** | **AIBench Micro (DC-AI-M)** | | | |
| `aibench_convolution.c` | M1 Convolution | conv2d | 1 x 227x227 NCHW, 3x3x3x2 filter | 6.8 M |
| `aibench_fully_connected.c` | M2 Fully Connected (matrix multiply) | matmul | 1 x 64x64 NHWC: [64x192]·[192x64] | 2.8 M |
| `aibench_relu.c` | M3 ReLU | relu | 1 x 227x227 NHWC | 0.39 M |
| `aibench_sigmoid.c` | M4 Sigmoid | sigmoid | 1 x 227x227 NHWC | 3.5 M |
| `aibench_tanh.c` | M5 Tanh | tanh | 1 x 227x227 NHWC | 2.5 M |
| `aibench_max_pooling.c` | M6 MaxPooling | max_pool | 2 x 227x227 NCHW, 3x3, stride 2 | 5.0 M |
| `aibench_avg_pooling.c` | M7 AvgPooling | avg_pool | 2 x 227x227 NCHW, 3x3, stride 2 | 4.6 M |
| `aibench_batch_norm.c` | M9 BatchNorm | batch_norm | 2 x 227x227 NHWC | 1.9 M |
| `aibench_dropout.c` | M10 Dropout | dropout | 1 x 32x32 NHWC | ~12 M (mostly the reseeding, see below) |
| `aibench_element_multiply.c` | M11 Element-wise multiply | multiply | 1 x 227x227 NHWC | 0.62 M |
| `aibench_softmax.c` | M12 Softmax | softmax | 1 x 227x227 NHWC | 3.4 M |
| `aibench_element_add.c` | element-wise add (in AIBench's Pthreads folder, not one of the 12) | add | 1 x 227x227 NHWC | 0.62 M |

**Names:** files are grouped by suite and category (`rit/<suite>/<category>/`) and named `<suite>_<benchmark>.c` with the benchmark's own name. The suite prefix is there because sims finds a program by its file name alone anywhere under `piton/verif/diag`, and refuses to run if two files share a name. New suites get their own folder (e.g. `rit/aibench/component/`). Results land in the matching folder automatically (`~/noc_runs/baremetal/aibench_micro/...`, see [runs.md](../docs/runs.md#where-results-go)). Until 2026-11-22 some files had AIBench's source names (`aibench_conv2d.c`, `aibench_matmul.c`, ...).

AIBench's DC-AI-M8 (CosineNorm) has no Pthreads version, so there's nothing to port.

Every port runs one thread per hart, splits the work the way the pthreads code does, and marks its region of interest (ROI) so the NoC trace covers only the kernel, not the setup or the result check.

**Why bare metal and not Linux:** an RTL simulation of a Linux boot is hundreds of millions of cycles (days) before a benchmark even starts. The bare-metal ports start the kernel within a few hundred thousand cycles. What they leave out is the OS: no timer interrupts, scheduler, page tables or TLB misses in the traffic. See [Comparing with the FPGA](#comparing-with-the-fpga).

## Files

```
piton/verif/diag/c/riscv/ariane/rit/
├── parsec/
│   └── parsec_blackscholes.c  PARSEC kernel unchanged + bare-metal scaffolding
└── aibench/
    └── micro/
        └── aibench_<name>.c   one per AIBench Micro kernel: the original loop + scaffolding
piton/verif/diag/assembly/include/riscv/ariane/rit/
├── bench.h                    ROI markers, barrier, cycle counters, fixed-point print
├── aibench.h                  shared by the AIBench ports: input, job split, ROI, lock, checksum
├── aib_rand.h                 glibc's srand()/rand() (dropout)
├── math.h                     exp()/log() (blackscholes; musl = Arm optimized-routines, as glibc)
├── mathf.h                    expf() and tanhf() (sigmoid, softmax, tanh; musl)
└── bs_option_data.h           PARSEC's option table (generated)
rit/benchmarks/
├── README.md                        you are here
├── aibench/micro/all.txt            the AIBench Micro ports as a nocrun batch list
├── aibench/micro/check_ports.sh     compares every port with the ORIGINAL AIBench code (x86 + QEMU)
├── aibench/micro/aib_host.cpp       runs an original AIBench kernel on x86 (used by check_ports.sh)
├── aibench/micro/host/loadImage.h   stand-in for AIBench's OpenCV image loader
├── aibench/micro/test_mathf.c       expf/tanhf against glibc on all 2^32 inputs
├── aibench/micro/test_rand.c        aib_rand.h against glibc's rand()
├── parsec/blackscholes/gen_option_data.py   regenerates bs_option_data.h from a PARSEC input file
└── qemu/run_qemu.sh                 functional check in QEMU before spending hours in VCS
```

The programs live under `verif/diag` because that's where `sims` looks for tests; the headers sit in the Ariane include directory because `sims` copies the test file into the run directory before compiling it, so the include path is the only way to reach them.

## Running on the lab machine

Use `nocrun` ([`../docs/runs.md`](../docs/runs.md)): every run gets its own folder under `~/noc_runs/`, keeps running if you log out, and is analyzed when it ends.

### 1. Build the model

```bash
source ~/openpiton/rit/setup/openpiton_env.sh
cd $PITON_ROOT/build
pbuild -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -ariane -config_rtl=MINIMAL_MONITORING 2>&1 | tee build_ariane_2x1.log
```

The programs aren't part of the model, so adding or changing a benchmark doesn't need a rebuild.

### 2. Smoke tests (a minute or two each)

Tiny inputs, to check each program compiles, links and passes before the long runs:

```bash
nocrun start parsec_blackscholes.c -D NUM_OPTIONS=16 --name bs16 --scratch --fg
nocrun start aibench_relu.c -D IMG_H=16 -D IMG_W=16 --name relu16 --scratch --fg
```

Any `aibench_*.c` works the same way with `-D IMG_H=16 -D IMG_W=16` (dropout: 8x8). `--scratch` keeps these out of your real results (`~/noc_runs/scratch/`, its own index).

**Pass looks like:** `status: PASS` and `num errors: 0` in the `run_info.txt` printed at the end. `sims_summary.log` has `noc_trace_mon: window opened at cycle N` and `closed at cycle M`. If the compile fails, `program/rv64_cc.log` has the compiler output.

### 3. Full runs

One at a time, or all of them as a batch:

```bash
nocrun start aibench_sigmoid.c          # -> ~/noc_runs/baremetal/aibench_micro/<date>_sigmoid_2x1_r1/
nocrun batch rit/benchmarks/aibench/micro/all.txt -j 2   # all 12 AIBench Micro ports, 2 at a time
nocrun status
```

See [runs.md: Running several at once](../docs/runs.md#running-several-at-once) for how many to run together, and what that does to wall time.

**Measured on the lab machine (2x1, VCS V-2023.12, default sizes, 2026-11-22/23):**

| | ROI cycles (slowest hart) | whole run, cycles | wall time | packets in ROI |
|---|---|---|---|---|
| blackscholes simsmall, 1 run | 2.09 M | 3.3 M | 20 min | 81,659 |
| relu | 2.27 M | 21.6 M | 1.9 h | 132,468 |
| fully_connected | 14.80 M | 24.3 M | 1.8 h | 1,627,261 |
| tanh | 4.94 M | 26.5 M | 2.5 h | 194,776 |
| sigmoid | 8.56 M | 30.0 M | 2.7 h | 196,572 |
| element_multiply | 4.99 M | 35.3 M | 2.9 h | 463,582 |
| element_add | 4.99 M | 35.3 M | 3.0 h | 463,568 |
| softmax | 8.97 M | 34.6 M | 3.2 h | 389,841 |
| dropout | 33.29 M | 33.9 M | 3.9 h | 254,665 |
| convolution | 13.40 M | 46.3 M | 4.2 h | 301,257 |
| avg_pooling | 12.55 M | 52.3 M | 4.7 h | 400,546 |
| batch_norm | 11.28 M | 60.7 M | 5.2 h | 836,078 |
| max_pooling | 13.87 M | 60.5 M | 5.4 h | 400,337 |

All twelve AIBench runs together: 41 hours one after another. The simulator ran 2,800–3,600 cycles/s on these. Most of each run is outside the ROI: hart 0 building the input and checking the output, one hart at a time. Running three at once didn't slow them measurably: element_multiply (with two others) and element_add (alone) do identical work and took 10,561 s and 10,653 s.

### 4. Check the results

Every AIBench port prints, at the end of `uart.log` (and `nocrun` copies into `run_info.txt` and `runs.csv`):

```
Checksum: 0x9bb2151206d00020
Checksum matches the original AIBench output
Num Errors: 0
```

- **`Num Errors: 0`:** hart 0 recomputed the output after the ROI (every element, or for fully_connected 16 rows) and found no differences.
- **`Checksum matches the original AIBench output`:** the checksum of the whole output equals the checksum of the *original* AIBench kernel's output for the same input (built on x86 by `aibench/micro/check_ports.sh`). QEMU gives this for every kernel.
- **`Checksum matches the OpenPiton+Ariane result`:** the output equals what this hardware computes, which for **sigmoid and avg_pooling** differs from the original in a few elements because Ariane's divider sometimes rounds 1 ulp low ([below](#arianes-divider-isnt-correctly-rounded)). That's the expected result for those two on the VCS model (and on the Genesys2, if it runs the same Ariane).
- Each port has both values built in for its default size. With other sizes it prints `No reference checksum for this size` and only the recomputation applies.
- **dropout** has a reference only for one thread: with two, which elements are kept depends on timing (see below), so its checksum varies between runs.

`nocrun list` shows a `reference` column (`matches original` / `matches Ariane hw` / `DIFFERS` / `none for this size`) for every run.

### 5. Analyze

Each run is analyzed at a 10,000-cycle window when it ends (`<run>/analysis/w10000/`). Other sizes, and several runs together:

```bash
nocrun analyze sigmoid -w 80000
nocrun compare relu sigmoid tanh -w 20000
nocrun list --suite aibench_micro          # just these runs
```

Cycles count from the ROI start.

## Options

Passed with `-D NAME=VALUE` to `nocrun start` (or `-gcc_args=-DNAME=VALUE` to `sims`).

| Program | Define | Default | Meaning |
|---|---|---|---|
| all | `NTHREADS` | number of harts | threads to use (the rest idle at the barrier) |
| blackscholes | `NUM_OPTIONS` | 4096 | input size: 4 = test, 16 = simdev, 4096 = simsmall, 16384 = simmedium, 65536 = simlarge |
| blackscholes | `NUM_RUNS` | 1 | repetitions of each thread's slice; PARSEC uses 100 |
| blackscholes | `BS_DUMP` | off | print every price after the check |
| blackscholes | `RIT_USE_LIBM` | off | use the toolchain's libm for exp/log instead of `rit/math.h` |
| AIBench | `BATCH` | 1 (pools, batch_norm: 2) | images in the batch |
| AIBench | `IMG_H`, `IMG_W` | 227, 227 (matmul 64, dropout 32) | image size |
| matmul | `MM_CHECK_ROWS` | 16 | output rows hart 0 recomputes after the ROI |
| dropout | `AIB_EPOCH` | 1763856000 | the value `time(NULL)` returns |

Changing any size turns off the built-in reference checksum (the recomputation still runs).

## What the ports keep and change

### The rule

The goal is NoC traffic that represents the original program. Traffic comes from the memory accesses each core makes: which data, in what order, and how the cores share it. So the ports keep exactly:

- **the kernel loop**, copied from the original line for line (same variables, same loop order, same arithmetic);
- **the data layout**, flat arrays in the original's index order (NHWC or NCHW as the original uses);
- **the work split**: the same jobs, dealt to threads in the same contiguous ranges, the last thread taking the remainder;
- **the sharing** between threads: shared variables, locks, and shared library state where the original has them (softmax's sum, dropout's random-number generator);
- **what's in memory when the kernel starts**: hart 0 does the setup the original's main thread does just before `run()` (copying operands, zeroing outputs), so the same data is freshly written when the ROI starts.

What they replace is what needs an operating system, and each replacement is chosen to add as little NoC traffic as its Linux counterpart:

| Original (Linux) | Port (bare metal) | Effect on traffic |
|---|---|---|
| OpenCV reads JPEG images (pixels 0..255) | hart 0 fills the input with pseudo-random 0..255 values | same data size and range; outside the ROI |
| `std::thread` create / `join()` | each hart runs its range between two barriers | the barrier is a few atomic operations on two cache lines |
| 1000 images, batches of 100 | one batch (`BATCH` images) | less work, same access pattern per image |
| `std::mutex` (softmax), glibc's internal lock (dropout) | `aib_lock`: atomic swap to take, atomic swap to release | same atomics; a waiting thread spins on a cached load (Linux sleeps) |
| glibc `srand`/`rand` (dropout) | `aib_rand.h`: glibc's algorithm, shared state, one lock | identical numbers and the same shared state |
| `time(NULL)` (dropout) | one load of a fixed variable | glibc reads the vDSO time page: also just loads |
| `std::exp`/`std::tanh` on floats | `mathf.h`: musl's `expf` (same algorithm as glibc) and `tanhf` | table of 32 entries; identical results on the inputs used |
| `std::vector` per row (convolution, pooling, batch_norm) | flat arrays | no per-row pointer loads; same elements in the same order |
| C++ at `-O0` | C at `-O2` (what `sims` uses) | fewer instructions, so the same traffic arrives faster |

Two compiler details keep the arithmetic identical to the original: multiply and add stay separate instructions (`#pragma GCC optimize ("fp-contract=off")`, as at `-O0`; a fused multiply-add rounds differently), and the same float types are used throughout.

### Per kernel

- **relu, sigmoid, tanh** (`tensor.h` kernels): one flat array, modified in place. Pixels are never negative, so ReLU reads every element and writes none, exactly like the original on images. sigmoid and tanh call `expf`/`tanhf` once per element.
- **add, multiply:** two operand arrays (each `tensor` copies the data, so they're separate arrays with the same values) and an output array that starts as a copy of operand 1. **AIBench's `multiplyOperator` adds** (`data1 + data2`); the port keeps that, so it runs the same code as the original. Its output equals add's.
- **matmul:** operand 1 is the batch as [N·H, W·C], operand 2 the same values as [W·C, N·H] (a separate copy). Each thread computes whole output rows; every dot product walks operand 2 *down a column* (stride N·H floats), which touches a new cache line on almost every access. The thread count is capped at the number of rows, as in the original. **Size:** the work is (N·H)²·W·C multiply-adds: 35 M for one 227x227 image (more than a day in VCS) and 3.5·10¹¹ for AIBench's batch of 100, so the default is one 64x64 image.
- **softmax:** two phases with a join between them. Phase 1: each thread replaces its elements with `expf(x)`, sums them, and adds its partial sum to the shared `sum` under a mutex. Phase 2: each thread divides its elements by `sum`. `sum` and the lock share one 64-byte line, as `sum` and `sum_mutex` sit side by side in the softmax object. On 0..255 pixels `expf` overflows above 88, so `sum` is infinite and the output is 0 or NaN, which is what the original computes on images too; every element is still read and written in both phases.
- **dropout:** for *every element*, `randFloat()` calls `srand(time(NULL))` and then `rand()`. In glibc both take the process-wide random-number lock, and `srand` rebuilds the shared 31-word generator state (31 divisions plus 310 discarded outputs, ~7,800 instructions). So the threads mostly take turns on one shared state, which is this benchmark's traffic, and it's all reproduced. Two consequences:
  - **Size:** one 227x227 image would be 1.2 billion instructions (about a week in VCS), so the default is one 32x32 image (3,072 elements).
  - **Varying output:** `srand` and `rand` take the lock separately, so another thread's `rand()` can land in between and a thread gets the second number after the reseed instead of the first. Which elements are kept therefore depends on timing; the original does the same (on x86 its checksum changes with 3 or 4 threads). Every element is written whether kept or dropped, so the traffic doesn't depend on it.
- **batch_norm:** normalizes `scale·(x − mean)/variance + offset` with mean 0, variance 1, offset 1, scale 1, epsilon 1, as `thread_Batch_norm()` sets them up. Mean and variance are arrays read for every pixel.
- **max_pool, avg_pool:** NCHW, 3x3 windows, stride 2, SAME padding (`run-pthread.sh` and the originals' `main()`). The innermost loop is over channels, so consecutive windows jump between the three channel planes. avg_pool always divides by 9, also at the padded edges, as the original.
- **batch_norm, max_pool, avg_pool:** jobs are whole images, so with one image only the last thread would work; the default batch is 2 (one image per hart).
- **convolution** (AIBench `conv2d`): see its file header. Ported before the others: same rules, but without the `fp-contract` pragma (its sums are exact integers either way) and without a built-in reference checksum (its own check recomputes every pixel).

### blackscholes

**Same as PARSEC:** `CNDF()`, `BlkSchlsEqEuroNoDiv()` and the `bs_thread()` loop are PARSEC's code unchanged, including its float/double mix and the order threads walk their slices. The input arrays have PARSEC's layout (five float arrays back to back in one 64-byte-aligned buffer, `otype` in another) and exactly the values PARSEC's `fscanf("%f")` reads from `in_<N>.txt` (every PARSEC input is the same 1000-row table repeated, which is what `bs_option_data.h` holds). The work split is PARSEC's: thread t does options `[t*N/T, (t+1)*N/T)`.

**Different:** no file reading (hart 0 builds the arrays from the table before the ROI), no thread creation or join (every hart calls `bs_thread()`; barriers before and after), and PARSEC's `ERR_CHK` comparison against the DerivaGem reference values runs after the ROI instead of inside the loop. `exp`/`log` come from `rit/math.h`: musl's copy of Arm's optimized-routines code, the same algorithm and tables glibc uses on Linux, compiled into the program because a libm built without `-mcmodel=medany` can't link at Ariane's 0x80000000 load address.

**Checked:** in QEMU on 1, 2 and 4 harts; all 4096 simsmall prices are within 1.4e-5 of PARSEC's own output built with GCC on x86 (float rounding differences), and `Num Errors: 0` against the reference values, same as PARSEC.

## Ariane's divider isn't correctly rounded

On the 2026-11-22 runs, sigmoid and avg_pooling passed their own checks but their checksums differed from the original's, while QEMU matched it exactly. Both divide. `rit/tests/rit_fdiv_check.c` checks the hardware's `fdiv.s`/`fdiv.d` against a correctly rounded reference computed with integer arithmetic only (no FPU). On the VCS model (2026-11-23):

| Divisions | wrong |
|---|---|
| `float(s) / 9`, s = 1..2295 (every avg_pooling division) | 56 (2.4%) |
| `1 / (1 + expf(-v))`, v = 0..255 (every sigmoid division) | 1 |
| 4,000 random floats | 360 (9%) |
| 2,000 random doubles | 93 (4.7%) |
| `expf(-v)` against glibc | 0 differ |

Every wrong quotient shown was **1 ulp too low** (e.g. 33/9: correct `0x406aaaab`, hardware `0x406aaaaa`): the divider sometimes doesn't round up when round-to-nearest requires it. It happens with the default (dynamic) rounding mode and with round-to-nearest-even written into the instruction, so it's the divider (Ariane's FPU, `fpnew` with the `div_sqrt_mvp` unit in OpenPiton's 2020 Ariane), not a setting. CVA6, Ariane's current name, has bug reports of this kind.

What it means:
- **Traffic:** none. A quotient off by one bit is stored to the same address at the same time.
- **Results:** kernels that divide get outputs that differ from x86 in the last bit of a few elements. Their own checks pass because they divide on the same hardware. The ports therefore accept two references: the original's output, and this hardware's.
- **FPGA:** the Genesys2 runs the same RTL, so with the same Ariane it should produce exactly the VCS checksums. Correct checksums on the board would mean its Ariane is a different version.
- **blackscholes** divides in double precision too; its check allows PARSEC's tolerance, so it isn't affected.

## How the ports were checked

1. **Against the original code:** `aibench/check_ports.sh` builds each ORIGINAL AIBench kernel on x86 (its own header and `.cpp`, unchanged; only the OpenCV image loader is replaced, by `aibench/host/loadImage.h`), feeds it exactly the port's input, and compares the output checksum with the port running in QEMU. Three sizes per kernel (including the default) and 1, 2, 3 and 4 threads: all 128 cases pass, 122 bit-for-bit identical and 6 dropout cases with several threads where the output depends on timing (the original's does too).
2. **The math:** `aibench/test_mathf.c` compares `mathf.h` with glibc 2.39 on all 2³² float inputs. `expf`: 2 inputs differ, by 1 ulp (glibc uses an FMA variant on that CPU). `tanhf`: 1 in 250 differ, by at most 3 ulp (glibc's is a different implementation). On the integer inputs the kernels use, both are identical to glibc.
3. **The random numbers:** `aibench/test_rand.c` compares `aib_rand.h` with glibc's `srand`/`rand`: 110,000 numbers, all identical.
4. **In every run:** the recomputation after the ROI and the built-in reference checksum (see [Check the results](#4-check-the-results)).

```bash
AIBENCH=~/DC_AIBench_Micro rit/benchmarks/aibench/micro/check_ports.sh            # all kernels, ~1 minute
AIBENCH=~/DC_AIBench_Micro rit/benchmarks/aibench/micro/check_ports.sh matmul     # one (AIBench source name)
```

These need g++, `riscv64-unknown-elf-gcc` and `qemu-system-riscv64` (not on the lab machines; any Linux box or WSL), and AIBench: `git clone https://github.com/BenchCouncil/DC_AIBench_Micro` (the ports are from commit fb2e73d).

## Comparing with the FPGA

These runs give NoC traffic for the benchmark kernel on the same hardware (OpenPiton + Ariane, 2x1, the Genesys2 configuration). If the board runs these same bare-metal programs, sim and board run identical code and their traffic should match closely. Compared with the original benchmarks under Linux:

- **No OS traffic.** Under Linux there are timer interrupts, the scheduler, page-table walks on TLB misses, and the thread create/join calls. None of that is here.
- **Input size.** One batch instead of 1000 images (smaller still for matmul and dropout), and PARSEC simsmall with 1 run. Per-window signatures compare in shape; totals don't.
- **Instruction stream.** C at `-O2` instead of C++ at `-O0`: the same data in the same order, reached with fewer instructions.
- **Waiting.** A thread waiting at a barrier or for a lock spins on a cached line, which makes little or no NoC traffic once the line is in its cache. On Linux it would sleep in the kernel.

## Checking changes in QEMU first

`rit/benchmarks/qemu/run_qemu.sh` builds a test the way `sims` does (same flags, same `crt.S`, `syscalls.c` and linker script) and runs it in QEMU on N harts, in seconds instead of hours.

```bash
rit/benchmarks/qemu/run_qemu.sh aibench_relu.c 2
QEMU_ICOUNT=1 rit/benchmarks/qemu/run_qemu.sh aibench_convolution.c 2      # ROI counts = instructions
```

QEMU isn't cycle-accurate and has no NoC, so it only answers "does it build, run on N harts and get the right answer". Instruction counts with `QEMU_ICOUNT=1` are exact (except where a hart spins waiting), which is a good basis for estimating sim time. `QEMU_KEEP=1` keeps the build folder; `QEMU_ARGS=...` passes extra options to QEMU (e.g. `-d int` to log traps).

## Porting another benchmark

1. **Find the region of interest:** the compute the benchmark exists to measure, separate from file reading, setup and thread management.
2. **Copy that code unchanged** into `piton/verif/diag/c/riscv/ariane/rit/<suite>/<category>/<suite>_<name>.c`, as C (the folders decide where its results go). Keep variable names and loop order so it can be compared line by line.
3. **Replace the OS parts** using `rit/bench.h` and `rit/aibench.h`: input built in memory by hart 0, each hart runs its thread's share (`aib_range`), barriers instead of create/join, `aib_lock` for mutexes. For each replacement ask what memory traffic the original part makes and keep it (shared variables stay shared, a lock stays a lock).
4. **Mark the ROI** with `aib_roi_begin`/`aib_roi_end` (or `noc_roi_begin`/`noc_roi_end`), and check the output after it.
5. **Verify against the original:** build the original on x86 with the same input and compare outputs (`aibench/aib_host.cpp` and `check_ports.sh` show how), on 1, 2 and more threads.
6. **Size it:** `QEMU_ICOUNT=1` gives instructions per hart; at roughly 2 cycles per instruction and 3,000–4,500 cycles/s, pick an input that runs in hours, not days.
7. **Smoke test on the model** with a tiny input (`--scratch`), then the full run.
