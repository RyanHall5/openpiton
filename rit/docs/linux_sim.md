# Linux in the OpenPiton+Ariane simulation: findings and plan

Goal: boot the same Linux the Genesys2 board runs, inside the 2x1 VCS simulation, save the booted state to a checkpoint, then start benchmarks from that checkpoint as often as needed, with the NoC trace covering only the benchmark.

Status (2026-11-17): planning. Run tooling, checkpoints and testbench fixes are in; the boot itself is waiting on the board's Linux image.

## What the simulation already provides

**Boot handoff: works as Linux expects.** In simulation the chipset selects the *bare-metal* boot ROM (`chipset_impl.v.pyv`: `ariane_boot_sel = 1` unless `ARIANE_SIM_LINUX_BOOT` is defined). That ROM (`piton/design/chipset/rv64_platform/bootrom/baremetal/bootrom.S`) does exactly the standard RISC-V handoff:
```asm
li   s0, DRAM_BASE        # 0x80000000
csrr a0, mhartid          # a0 = hart id
la   a1, _dtb             # a1 = device tree, built into the ROM
jr   s0
```
OpenSBI and BBL both expect this. So a boot payload (OpenSBI `fw_payload` or BBL with the kernel inside) linked at `0x80000000` should start in simulation exactly like a bare-metal test does.

**The device tree is generated for the model.** The DTB in the ROM comes from `piton/tools/bin/riscvlib.py`, the same generator the FPGA flow uses, run for the simulated configuration when the model is built (that's why the build needs `dtc`). It describes the cores, memory, CLINT (timer), PLIC (interrupts) and the UART at the simulated addresses and frequencies. The kernel should get its hardware description from this DTB, not one built into the FPGA image.

**Loading: the same path as test programs.** `sims` can take a precompiled RISC-V ELF instead of a C file (`-precompiled`; it's copied as `diag.exe`). `rv64_img` then turns it into `mem.image`, which is preloaded into the simulated DRAM. If the board team has the payload as an ELF, this is the loader. A raw `.bin` (what goes on the SD card) converts to an ELF with `objcopy`.

## What has to be built or changed

1. **Root file system inside the payload (initramfs).** There's no SD card in the simulation: the SD controller exists, but nothing models the card, and `ARIANE_SIM_LINUX_BOOT` would select a ROM that loads from it. The kernel has to carry its root file system (buildroot's `rootfs.cpio` built into the kernel). Ask whether the board image already works this way; ariane-sdk/cva6-sdk builds usually do.

2. **A real UART model.** The simulation's UART (`piton/verif/env/manycore/fake_uart.v`) was made for bare-metal printing:
   - it only handles writes (each byte goes to `fake_uart.log`)
   - **every read returns `0xEF`**: the line status register then says "data received" forever
   - **its interrupt line is tied to 0** (`chipset_impl.v.pyv`), while the device tree tells Linux the UART has an interrupt

   Kernel messages (polled) will probably print. User programs' output goes through the interrupt-driven TTY driver and would likely stall. There's also no way to send input. Plan: a small 16550-compatible model with proper status registers and interrupt, still writing `fake_uart.log`, that also reads **input from a file `uart_in.txt` in the run folder**, appended to at any time. That's how commands get "typed" into a running or restored simulation.

3. **A checkpoint at the end of boot.** `rit_ckpt` (in now) saves at a fixed cycle, but nobody knows the boot length in advance. Plan: let the UART model save a checkpoint when Linux prints a chosen string, e.g. the init script prints `RIT_READY` once booted and waits for input. Restore, append a command to `uart_in.txt`, and the benchmark runs.

4. **A trace trigger that works under Linux.** The ROI markers watch for an instruction address, but Linux programs run at virtual addresses, so that doesn't carry over. Plan: the benchmark writes a magic value to an otherwise unused UART register (the scratch register, through `/dev/mem`) at ROI begin/end. The UART model, or the trace monitor, opens and closes the window on it. That write is also a NoC packet to the UART, so the board's ILA could trigger on the same thing.

5. **Long-run hygiene (done).** `MINIMAL_MONITORING`, instruction traces to `/dev/null`, the `pc_cmp` timeout fix, `setsid` runs (`nocrun`). For Linux use `--rtl-timeout 2000000000`: idle cores sit in `wfi` and finish no instructions for long stretches.

## Cost

- Sim speed measured on the lab machine: ~2,700–4,500 cycles/s (2x1, VCS).
- Linux boot length in cycles: unknown until the first boot. Probably hundreds of millions, so roughly **1–4 days per boot** at that speed. Hence the checkpoint: boot once.
- A checkpoint is several GB.
- Possibly much faster: OpenPiton also supports Verilator, a free simulator that's often several times faster than VCS on designs like this and needs no license. Worth measuring before committing to multi-day VCS boots.

## Checkpoint findings (2026-11-17/18)

- `$save` works with VCS 2017.12: `checkpoint.vcs` + `checkpoint.vcs.FILES/`, about 59 MB for the 2x1 bare-metal run (the simulator process uses ~150 MB). The run continues normally after saving.
- **Restoring fails** with VCS 2017.12 on RHEL 9, within 3 seconds and before simulating a cycle: `Fatal error: glibc detected an invalid stdio handle`. Turning address randomization off for both runs (`setarch -R`, now automatic in `nocrun`) didn't change it. Likely cause: the checkpoint is a process image, and RHEL 9's glibc protects its internal pointers with a per-process random key (pointer mangling) and checks its file structures. A 2017 tool's restore can't satisfy that.
- Next: build the model with VCS W-2024.09 (built for EL9; `RIT_VCS=el9`) and repeat the test.
- If no VCS can restore: drop checkpoints from the plan. **One boot runs all the benchmarks back to back**, each in its own trace window. That needs nothing beyond the UART model. Other routes: Verilator's own save/restore (plus saving the C memory model ourselves), or a process checkpointer such as DMTCP.

## Needed from the board team (Parsa, Denis)

- The boot payload: OpenSBI or BBL, with the kernel and root file system. As an ELF if possible, and how it was built (repo, branch, config), so it can be rebuilt with changes (init script, benchmarks).
- Whether the root file system is an initramfs inside the kernel, or on the SD card's second partition.
- The benchmark binaries, and how they start them (command lines, input sizes, thread counts).
- Which kernel console/UART settings they use (`bootargs`, earlycon).

## Order of work

1. Checkpoint test on a bare-metal program (`nocrun start ... --save-at`, `nocrun restore`, compare traces): proves VCS checkpoints keep the memory model's contents.
2. UART model with interrupt + input file; test with a bare-metal program that reads input.
3. Load the board's payload, boot, watch `fake_uart.log`. Measure cycles to the shell.
4. Init script: print `RIT_READY`, wait for a command; UART-triggered checkpoint.
5. ROI trigger via the UART scratch register; benchmark runs from the checkpoint.
6. (In parallel, optional) Verilator build of the 2x1 and a speed comparison.
