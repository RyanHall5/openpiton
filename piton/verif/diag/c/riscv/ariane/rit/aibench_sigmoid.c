// AIBench Micro DC-AI-M4 (Sigmoid), Pthreads version, ported to run bare-metal on
// OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/sigmoid.cpp + sigmoid.h + tensor.h, run as in run-pthread.sh
// ("sigmoid"):  ./sigmoid <images>/ 12 227 227 100
// sigmoid.cpp loads the batch as NHWC and flattens it into one tensor<float> (a
// single std::vector, i.e. one contiguous float array); sigmoid<float>'s
// constructor copies that tensor, and run() splits the elements into
// contiguous ranges, one per thread, the last thread taking the remainder.
// pthreadFunction() below is sigmoid<T>::pthreadFunction line for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md has the
// full list for all the AIBench ports):
//   * Input: hart 0 fills the array with pseudo-random 0..255 values (images
//     are 0..255 too) instead of reading JPEGs.
//   * expf: the original calls std::exp on a float, which is glibc's expf.
//     rit/mathf.h has the same algorithm (Arm's, used by glibc and musl).
//   * One batch of BATCH images (default 1) instead of 1000 images in batches
//     of 100.
//   * Threads: every hart runs its range between two barriers instead of
//     std::thread create/join. C at -O2 instead of C++ at -O0.
//
// Options (nocrun start ... -D NAME=VALUE): BATCH, IMG_H, IMG_W, NTHREADS.
// After the ROI hart 0 checks every element and prints a checksum to compare
// with the original built on x86 (rit/benchmarks/aibench/).

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"
#include "rit/mathf.h"

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0x05ca1efda49b963cull
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

static float data[NELEM] __attribute__((aligned(64)));   // sigmoid's copy of the tensor

// ================ AIBench sigmoid<T>::pthreadFunction ================
static void pthreadFunction(int pos_start, int pos_end)
{
    for(int pos = pos_start; pos < pos_end; ++pos)
        data[pos] = 1 / (1 + expf(-data[pos]));
}
// ==================================================================

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench sigmoid (bare-metal port): NHWC %d x %d x %d x %d = %ld elements, %d threads of %d harts\n",
               BATCH, IMG_H, IMG_W, CHANNELS, NELEM, nThreads, nharts);
        if (setup_ok)
            aib_fill_pixels(data, NELEM);
        else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: sigmoid<T>::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, NELEM, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    // expected value for each possible pixel value (inputs are integers 0..255)
    static float table[256];
    for (int v = 0; v < 256; v++)
        table[v] = 1 / (1 + expf(-(float)v));
    int errors = 0;
    uint32_t seed = 12345;
    for (long i = 0; i < NELEM; i++) {
        seed = seed * 1103515245u + 12345u;
        float x = (float)((seed >> 16) & 0xff);
        float want = table[(int)x];
        if (data[i] != want) {
            if (errors < 5)
                printf("Error at %ld (x=%d): got %08x want %08x\n", i, (int)x, f2u(data[i]), f2u(want));
            errors++;
        }
    }
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(data, NELEM), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM);
    return errors ? 2 : 0;
}
