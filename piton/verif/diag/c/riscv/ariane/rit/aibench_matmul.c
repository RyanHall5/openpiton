// AIBench Micro DC-AI-M2 (Fully Connected = matrix multiply), Pthreads version,
// ported to run bare-metal on OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/matmul.cpp + matmul.h + tensor.h, run as in run-pthread.sh
// ("matmul"):  ./matmul <images>/ 12 227 227 100
// matmul.cpp flattens the NHWC batch into one vector and builds two 2-D
// tensors from it (each tensor copies the data):
//   operand1: shape [N*H, W*C]      (R rows of K values)
//   operand2: shape [W*C, N*H]      (the same values read as K rows of R)
// and multiplies them: output [N*H, N*H] (R x R). run() caps the thread count
// at R, splits the R output rows into contiguous ranges (last thread takes the
// remainder), and each thread computes whole rows: for every output element,
// a dot product walking operand1 along a row and operand2 DOWN a column
// (stride R floats). pthreadFunction() below is matmul<T>::pthreadFunction
// line for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * Size: the work is R*R*K multiply-adds. With AIBench's batch of 100
//     227x227 images that's 22700 x 22700 x 681 (a 2 GB output); even one
//     227x227 image is 35 M multiply-adds, more than a day in VCS. The default
//     here is one 64x64 image: R = 64, K = 192. Set IMG_H/IMG_W/BATCH to
//     change it (R = BATCH*IMG_H, K = IMG_W*3).
//   * Input: pseudo-random 0..255 values instead of JPEG pixels.
//   * Multiply and add stay separate instructions (fp-contract off), as at
//     -O0 on Linux, so results match the original exactly (a fused
//     multiply-add rounds differently).
//   * Threads: barriers instead of std::thread create/join. C at -O2.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS, MM_CHECK_ROWS (rows hart 0
// recomputes after the ROI, spread evenly; default 16). The checksum covers
// the whole output and, at the default size, is compared with the original's.

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"

#pragma GCC optimize ("fp-contract=off")

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0xda742bde69e2d87bull
#if !defined(BATCH) && !defined(IMG_H) && !defined(IMG_W)
#define AIB_REF_SIZE 1
#else
#define AIB_REF_SIZE 0
#endif

#ifndef BATCH
#define BATCH 1
#endif
#ifndef IMG_H
#define IMG_H 64
#endif
#ifndef IMG_W
#define IMG_W 64
#endif
#define CHANNELS 3
#define R ((long)BATCH * IMG_H)          // shape1[0] = shape2[1]
#define K ((long)IMG_W * CHANNELS)       // shape1[1] = shape2[0]
#ifndef MM_CHECK_ROWS
#define MM_CHECK_ROWS 16
#endif

// operand1 [R][K], operand2 [K][R], output [R][R]
static float data1[R * K] __attribute__((aligned(64)));
static float data2[K * R] __attribute__((aligned(64)));
static float data[R * R]  __attribute__((aligned(64)));
static const int shape1[2] = { R, K }, shape2[2] = { K, R }, shape[2] = { R, R };

// ================ AIBench matmul<T>::pthreadFunction ================
static void pthreadFunction(int pos_start, int pos_end)
{
    for(int row1 = pos_start; row1 < pos_end; ++row1)
    {
        int head1 = row1 * shape1[1];
        for(int col2 = 0; col2 < shape2[1]; ++col2)
        {
            float sum = 0;
            for(int pos = 0; pos < shape1[1]; ++pos)
            {
                sum += data1[head1 + pos] * data2[col2 + pos * shape2[1]];
            }
            data[row1 * shape[1] + col2] = sum;
        }
    }
}
// =====================================================================

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);
    if (nThreads > shape[0])                  // pthreadNumber = min(pthreadNumber, shape[0])
        nThreads = shape[0];

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench matmul (bare-metal port): [%ld x %ld] x [%ld x %ld] from NHWC %d x %d x %d x %d, %d threads of %d harts\n",
               R, K, K, R, BATCH, IMG_H, IMG_W, CHANNELS, nThreads, nharts);
        if (setup_ok) {
            aib_fill_pixels(data1, R * K);
            for (long i = 0; i < R * K; i++)
                data2[i] = data1[i];
            for (long i = 0; i < R * R; i++)
                data[i] = 0;                  // constructor: data.resize(shape[0] * shape[1])
        } else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: matmul<T>::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, R, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI: recompute MM_CHECK_ROWS rows, spread evenly ----
    if (hart != 0)
        return 0;
    int errors = 0;
    long rows = MM_CHECK_ROWS < 1 ? 1 : (MM_CHECK_ROWS > R ? R : MM_CHECK_ROWS);
    for (long n = 0; n < rows; n++) {
        long r = n * R / rows;
        for (long c = 0; c < R; c++) {
            float s = 0;
            for (long k = 0; k < K; k++)
                s += data1[r * K + k] * data1[k * R + c];   // data2 holds the same values as data1
            if (f2u(data[r * R + c]) != f2u(s)) {
                if (errors < 5)
                    printf("Error at (%ld,%ld): got %08x want %08x\n", r, c, f2u(data[r * R + c]), f2u(s));
                errors++;
            }
        }
    }
    printf("checked %ld of %ld rows\n", rows, R);
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(data, R * R), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM);
    return errors ? 2 : 0;
}
