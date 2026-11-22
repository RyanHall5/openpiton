// AIBench Micro DC-AI-M9 (BatchNorm), Pthreads version, ported to run
// bare-metal on OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/batch_norm.cpp + batch_norm.h. run-pthread.sh has no
// batch_norm entry; this uses the settings of its other kernels (227x227
// NHWC images). thread_Batch_norm() builds mean[N][H][W] = 0 and
// variance[N][H][W] = 1 and normalizes with offset 1, scale 1, epsilon 1.
// run() gives each thread whole images (jobs = images in the batch,
// contiguous ranges, last thread takes the remainder). norm_SingleImage() and
// pthreadFunction() below are the originals line for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * Batch: jobs are whole images, so with one image only the last thread
//     would work. The default is BATCH = 2: one image per hart on the 2x1.
//   * Flat arrays: the original keeps images, mean, variance and output in
//     nested std::vectors (every row of every image its own heap block). Here
//     they are flat C arrays with the same index order, so each thread reads
//     and writes the same elements in the same order; the per-row pointer
//     loads of the vectors aren't reproduced.
//   * Input: pseudo-random 0..255 values instead of JPEG pixels.
//   * Threads: barriers instead of std::thread create/join. C at -O2 instead
//     of C++ at -O0.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS.
// After the ROI hart 0 checks every element and prints a checksum to compare
// with the original built on x86 (rit/benchmarks/aibench/).

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"

#pragma GCC optimize ("fp-contract=off")

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0xe9ca3de52588a46eull
#if !defined(BATCH) && !defined(IMG_H) && !defined(IMG_W)
#define AIB_REF_SIZE 1
#else
#define AIB_REF_SIZE 0
#endif

#ifndef BATCH
#define BATCH 2
#endif
#ifndef IMG_H
#define IMG_H 227
#endif
#ifndef IMG_W
#define IMG_W 227
#endif
#define CHANNELS 3

// input[n][h][w][c] (NHWC), mean/variance[n][h][w], output[n][h][w][c]
static float input   [BATCH][IMG_H][IMG_W][CHANNELS] __attribute__((aligned(64)));
static float mean    [BATCH][IMG_H][IMG_W]           __attribute__((aligned(64)));
static float variance[BATCH][IMG_H][IMG_W]           __attribute__((aligned(64)));
static float output  [BATCH][IMG_H][IMG_W][CHANNELS] __attribute__((aligned(64)));

// batch_norm's member variables (thread_Batch_norm passes offset 1, scale 1, epsilon 1)
static const float offset = 1, scale = 1, variance_epsilon = 1;
static const int input_heightSize = IMG_H, input_widthSize = IMG_W, input_channelsSize = CHANNELS;

// ================ AIBench batch_norm::norm_SingleImage / pthreadFunction ================
static void norm_SingleImage(float (*input)[IMG_W][CHANNELS], int id)
{
    for(int h = 0; h < input_heightSize; ++h)
    {
        for(int w = 0; w < input_widthSize; ++w)
        {
            for(int c = 0; c < input_channelsSize; ++c)
            {
                float v = variance[id][h][w];
                if(v == 0)
                    v = variance_epsilon;
                output[id][h][w][c] = scale * (input[h][w][c] - mean[id][h][w])/v + offset;
            }
        }
    }
}

static void pthreadFunction(int jobId_start, int jobId_end)
{
    for(int jId = jobId_start; jId < jobId_end; ++jId)
        norm_SingleImage(input[jId], jId);
}
// =========================================================================================

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench batch_norm (bare-metal port): NHWC %d x %d x %d x %d, %d jobs (images), %d threads of %d harts\n",
               BATCH, IMG_H, IMG_W, CHANNELS, BATCH, nThreads, nharts);
        if (setup_ok) {
            aib_fill_pixels(&input[0][0][0][0], (long)BATCH * IMG_H * IMG_W * CHANNELS);
            for (int n = 0; n < BATCH; n++)          // thread_Batch_norm: mean_tmp(0), variance_tmp(1)
                for (int h = 0; h < IMG_H; h++)
                    for (int w = 0; w < IMG_W; w++)
                        mean[n][h][w] = 0;
            for (int n = 0; n < BATCH; n++)
                for (int h = 0; h < IMG_H; h++)
                    for (int w = 0; w < IMG_W; w++)
                        variance[n][h][w] = 1;
            for (int n = 0; n < BATCH; n++)          // constructor: output = zeros
                for (int h = 0; h < IMG_H; h++)
                    for (int w = 0; w < IMG_W; w++)
                        for (int c = 0; c < CHANNELS; c++)
                            output[n][h][w][c] = 0;
        } else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: batch_norm::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, BATCH, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    int errors = 0;
    uint32_t seed = 12345;
    float *out = &output[0][0][0][0];
    for (long i = 0; i < (long)BATCH * IMG_H * IMG_W * CHANNELS; i++) {
        seed = seed * 1103515245u + 12345u;
        float x = (float)((seed >> 16) & 0xff);
        float want = x + 1;                      // (x - 0) / 1 * 1 + 1, exact for integers
        if (f2u(out[i]) != f2u(want)) {
            if (errors < 5)
                printf("Error at %ld: got %08x want %08x\n", i, f2u(out[i]), f2u(want));
            errors++;
        }
    }
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(out, (long)BATCH * IMG_H * IMG_W * CHANNELS), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM);
    return errors ? 2 : 0;
}
