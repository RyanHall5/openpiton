// rit/bench.h: helpers for running multithreaded benchmarks bare-metal on
// OpenPiton+Ariane (one thread per hart), with NoC-trace ROI markers.
//
// Include it from exactly one C file (the test program): it defines functions
// with top-level asm. sims compiles a C test as a single file plus crt.S and
// syscalls.c, so that is always the case for diags under verif/diag/c.
//
//   main(argc, argv):  argv[0][0] = this hart's id, argv[0][1] = number of harts
//                      (set by syscalls.c _init); every hart runs main().
//
//   noc_roi_begin() / noc_roi_end()
//       Mark the region of interest. sims finds the noc_roi_begin_mark /
//       noc_roi_end_mark labels in the program and passes their addresses to
//       noc_trace_mon, which then traces only between the two (run with
//       -sim_run_args=+noc_trace; +noc_trace_full traces the whole run).
//       Call each from ONE hart, after a barrier. Each marker spins 16 times on
//       its label so the monitor can't miss the commit.
//
//   bench_barrier(nharts)   sense-reversing barrier for all harts (AMO based)
//   read_mcycle(), read_minstret()
//   print_fixed(x, digits)  print a double with a fixed number of decimals
//                           (the bare-metal printf has no %f)

#ifndef RIT_BENCH_H
#define RIT_BENCH_H

#include <stdint.h>
#include <stdio.h>
#include "util.h"

#define RIT_MAX_HARTS 64

// ---------------------------------------------------------------- ROI markers
__asm__(
    "    .text\n"
    "    .globl  noc_roi_begin\n"
    "    .type   noc_roi_begin, @function\n"
    "    .p2align 2\n"
    "noc_roi_begin:\n"
    "    fence\n"
    "    li      t0, 16\n"
    "    .globl  noc_roi_begin_mark\n"
    "noc_roi_begin_mark:\n"
    "    addi    t0, t0, -1\n"
    "    bnez    t0, noc_roi_begin_mark\n"
    "    ret\n"
    "    .size   noc_roi_begin, .-noc_roi_begin\n"
    "    .globl  noc_roi_end\n"
    "    .type   noc_roi_end, @function\n"
    "    .p2align 2\n"
    "noc_roi_end:\n"
    "    fence\n"
    "    li      t0, 16\n"
    "    .globl  noc_roi_end_mark\n"
    "noc_roi_end_mark:\n"
    "    addi    t0, t0, -1\n"
    "    bnez    t0, noc_roi_end_mark\n"
    "    ret\n"
    "    .size   noc_roi_end, .-noc_roi_end\n");

void noc_roi_begin(void);
void noc_roi_end(void);

// ---------------------------------------------------------------- counters
static inline uint64_t read_mcycle(void)   { uint64_t v; __asm__ volatile("csrr %0, mcycle"   : "=r"(v)); return v; }
static inline uint64_t read_minstret(void) { uint64_t v; __asm__ volatile("csrr %0, minstret" : "=r"(v)); return v; }

// ---------------------------------------------------------------- barrier
// The count and the sense flag sit in separate 64-byte lines (one L2 line each).
static volatile uint32_t rit_bar_count[16] __attribute__((aligned(64)));
static volatile uint32_t rit_bar_sense[16] __attribute__((aligned(64)));
static uint32_t rit_bar_local[RIT_MAX_HARTS * 16] __attribute__((aligned(64)));  // one line per hart

static void bench_barrier(int hart, int nharts)
{
    uint32_t s = !rit_bar_local[hart * 16];
    rit_bar_local[hart * 16] = s;
    uint32_t arrived;
    ATOMIC_FETCH_OP(arrived, rit_bar_count[0], 1, add, w);
    if (arrived == (uint32_t)(nharts - 1)) {
        rit_bar_count[0] = 0;
        __asm__ volatile("fence" ::: "memory");
        rit_bar_sense[0] = s;
    } else {
        while (rit_bar_sense[0] != s)
            ;
    }
    __asm__ volatile("fence" ::: "memory");
}

// ---------------------------------------------------------------- printing
// %f isn't supported by the bare-metal printf, so print x as fixed point.
static void print_fixed(double x, int digits)
{
    if (x < 0) { printf("-"); x = -x; }
    uint64_t scale = 1;
    for (int i = 0; i < digits; i++) scale *= 10;
    uint64_t v = (uint64_t)(x * (double)scale + 0.5);
    uint64_t ip = v / scale, fp = v % scale;
    char buf[24];
    int n = 0;
    for (int i = 0; i < digits; i++) { buf[n++] = '0' + fp % 10; fp /= 10; }
    printf("%lu", (unsigned long)ip);
    if (digits) {
        printf(".");
        while (n) printf("%c", buf[--n]);
    }
}

// ---------------------------------------------------------------- libm support
// sims links with -nostdlib -lm, so libm's errno handling has no libc to call.
int *__errno(void)
{
    static int rit_errno;
    return &rit_errno;
}

#endif // RIT_BENCH_H
