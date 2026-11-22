// AIBench Micro (element-wise add; not one of the 12 numbered workloads), Pthreads version, ported to run bare-metal on
// OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/add.cpp + add.h + tensor.h. run-pthread.sh has
// no add entry; same settings as its other tensor kernels: ./add <images>/ 12
// 227 227 100.
// add.cpp flattens the NHWC batch into a vector and builds two
// tensor<float> operands from it (each tensor copies the data, so they are two
// separate arrays holding the same values); addOperator's constructor makes the
// output tensor as a copy of operand 1. run() splits the elements into
// contiguous ranges, the last thread taking the remainder. pthreadFunction()
// below is addOperator<T>::pthreadFunction line for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * Input: hart 0 fills operand 1 with pseudo-random 0..255 values and
//     copies it to operand 2 and to the output, instead of reading JPEGs.
//   * One batch of BATCH images (default 1) instead of 1000 in batches of 100.
//   * Threads: every hart runs its range between two barriers instead of
//     std::thread create/join. C at -O2 instead of C++ at -O0.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS.
// After the ROI hart 0 checks every element and prints a checksum to compare
// with the original built on x86 (rit/benchmarks/aibench/).

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0x84dd9d0b5e3a2224ull
#if !defined(BATCH) && !defined(IMG_H) && !defined(IMG_W)
#define AIB_REF_SIZE 1
#else
#define AIB_REF_SIZE 0
#endif

#ifndef BATCH
#define BATCH 1
#endif
#ifndef IMG_H
#define IMG_H 227
#endif
#ifndef IMG_W
#define IMG_W 227
#endif
#define CHANNELS 3
#define NELEM ((long)BATCH * IMG_H * IMG_W * CHANNELS)

// tensor1, tensor2 and the output tensor's data, in the order they're made
static float data1[NELEM]       __attribute__((aligned(64)));
static float data2[NELEM]       __attribute__((aligned(64)));
static float data_output[NELEM] __attribute__((aligned(64)));

// ================ AIBench addOperator<T>::pthreadFunction ================
static void pthreadFunction(int pos_start, int pos_end)
{
    for(int pos = pos_start; pos < pos_end; ++pos)
        data_output[pos] = data1[pos] + data2[pos];
}
// ====================================================================

static volatile int setup_ok;

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench add (bare-metal port): NHWC %d x %d x %d x %d = %ld elements, %d threads of %d harts\n",
               BATCH, IMG_H, IMG_W, CHANNELS, NELEM, nThreads, nharts);
        if (setup_ok) {
            aib_fill_pixels(data1, NELEM);
            for (long i = 0; i < NELEM; i++)
                data2[i] = data1[i];
            for (long i = 0; i < NELEM; i++)
                data_output[i] = data1[i];   // output = new tensor<T>(operand1)
        } else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: addOperator<T>::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, NELEM, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    int errors = 0;
    uint32_t seed = 12345;
    for (long i = 0; i < NELEM; i++) {
        seed = seed * 1103515245u + 12345u;
        float x = (float)((seed >> 16) & 0xff);
        float want = x + x;
        if (data_output[i] != want) {
            if (errors < 5)
                printf("Error at %ld: got %d want %d\n", i, (int)data_output[i], (int)want);
            errors++;
        }
    }
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(data_output, NELEM), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM);
    return errors ? 2 : 0;
}
