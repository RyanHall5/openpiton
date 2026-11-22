// AIBench Micro DC-AI-M6 (MaxPooling), Pthreads version, ported to run bare-metal on
// OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/max_pool.cpp + max_pool.h, run as in run-pthread.sh
// ("maxpool"):  ./max_pool <images>/ NCHW 12 227 227 100
// i.e. NCHW data, 227x227 BGR images, and the settings in its main(): ksize
// {1,1,3,3}, strides {1,1,2,2}, SAME padding. run() gives each thread whole
// images (jobs = images in the batch, contiguous ranges, last thread takes the
// remainder). max_poolSingleImage() below is the NCHW + SAME branch of the
// original, line for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * Batch: jobs are whole images, so with one image only the last thread
//     would work. The default is BATCH = 2: one image per hart on the 2x1.
//   * Flat arrays: the original keeps images and outputs in nested
//     std::vectors (every row its own heap block). Here they are flat C arrays
//     with the same index order, so each thread reads and writes the same
//     elements in the same order; the per-row pointer loads aren't reproduced.
//   * Input: pseudo-random 0..255 values instead of JPEG pixels.
//     Pixels are never negative, so (as with images) the padding's 0 never
//     wins a max; the branch is still taken the same way for every window.
//   * The original prints one line from each thread when it starts
//     (cout << "current thread's batchId_start..."); the port doesn't.
//   * Threads: barriers instead of std::thread create/join. C at -O2 instead
//     of C++ at -O0.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS.
// After the ROI hart 0 recomputes every output value with an independent
// reference and prints a checksum to compare with the original built on x86
// (rit/benchmarks/aibench/micro/).

#include <stdint.h>
#include <stdio.h>
#include <float.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"

#pragma GCC optimize ("fp-contract=off")

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/micro/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0xeaa376b4109ed366ull
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

// ---- shapes, as max_pool's constructor works them out ----
#define IN_C        3
#define KSIZE_H     3                                  // ksize = {1, 1, 3, 3} (NCHW)
#define KSIZE_W     3
#define STRIDE_C    1                                  // strides = {1, 1, 2, 2}
#define STRIDE_H    2
#define STRIDE_W    2
#define OUT_H       ((IMG_H + STRIDE_H - 1) / STRIDE_H) // ceil(in / stride), SAME padding
#define OUT_W       ((IMG_W + STRIDE_W - 1) / STRIDE_W)
#define PAD_NEED_H  ((OUT_H - 1) * STRIDE_H + KSIZE_H - IMG_H)
#define PAD_NEED_W  ((OUT_W - 1) * STRIDE_W + KSIZE_W - IMG_W)
#define PAD_TOP     (PAD_NEED_H / 2)
#define PAD_LEFT    (PAD_NEED_W / 2)

// input[b][c][h][w], output[b][c][h][w]
static float input [BATCH][IN_C][IMG_H][IMG_W] __attribute__((aligned(64)));
static float output[BATCH][IN_C][OUT_H][OUT_W] __attribute__((aligned(64)));

// max_pool's member variables
static const int input_heightSize = IMG_H, input_widthSize = IMG_W, input_channelsSize = IN_C;
static const int ksize_height = KSIZE_H, ksize_width = KSIZE_W;
static const int stride_height = STRIDE_H, stride_width = STRIDE_W, stride_channels = STRIDE_C;
static const int output_heightSize = OUT_H, output_widthSize = OUT_W;
static const int pad_top = PAD_TOP, pad_left = PAD_LEFT;
static const float FLOAT_MIN = - FLT_MAX;      // max_pool.h

// ================ AIBench max_pool::max_poolSingleImage, NCHW + SAME branch ================
static void max_poolSingleImage(float (*input)[IMG_H][IMG_W], float (*output)[OUT_H][OUT_W])
{
    /* bound of input in padded_input */
    int bound_top = pad_top;
    int bound_down = pad_top + input_heightSize - 1;
    int bound_left = pad_left;
    int bound_right = pad_left + input_widthSize - 1;

    for(int pos_height = 0; pos_height < output_heightSize; ++pos_height)
    {
        for(int pos_width = 0; pos_width < output_widthSize; ++pos_width)
        {
            /* max_pool for each channel of a single filter */
            for(int pos_channel = 0; pos_channel < input_channelsSize; ++pos_channel)
            {
                int offset_height = pos_height * stride_height;
                int offset_width = pos_width * stride_width;
                int offset_channel = pos_channel * stride_channels;
                /* max number among current window */
                float maxNumber = FLOAT_MIN;
                for(int h = 0; h < ksize_height; ++h)
                for(int w = 0; w < ksize_width; ++w)
                {
                    /* position in padded_input */
                    int newPos_height = h + offset_height;
                    int newPos_width = w + offset_width;
                    /* check weather current point is in padded area */
                    if(newPos_height >= bound_top && newPos_height <= bound_down && newPos_width >= bound_left && newPos_width <= bound_right)
                    {
                        if(input[offset_channel][newPos_height - pad_top][newPos_width - pad_left] > maxNumber)
                            maxNumber = input[offset_channel][newPos_height - pad_top][newPos_width - pad_left];
                    }
                    else
                    {
                        if(0 > maxNumber)
                            maxNumber = 0;
                    }
                }
                output[pos_channel][pos_height][pos_width] = maxNumber;
            }

        }
    }
}

// ================ max_pool::pthreadFunction ================
static void pthreadFunction(int batchId_start, int batchId_end)
{
    for(int batchId = batchId_start; batchId < batchId_end; ++batchId)
    {
        max_poolSingleImage(input[batchId], output[batchId]);
    }
}
// ==============================================================================

// independent reference for the check: integer pixel values, zero padding
static float ref_value(int b, int c, int oh, int ow)
{
    int32_t best = 0, sum = 0, first = 1;
    for (int h = 0; h < KSIZE_H; h++)
        for (int w = 0; w < KSIZE_W; w++) {
            int y = oh * STRIDE_H + h - PAD_TOP, x = ow * STRIDE_W + w - PAD_LEFT;
            int32_t v = (y < 0 || y >= IMG_H || x < 0 || x >= IMG_W) ? 0 : (int32_t)input[b][c][y][x];
            if (first || v > best) best = v;
            first = 0;
            sum += (y < 0 || y >= IMG_H || x < 0 || x >= IMG_W) ? 0 : v;
        }
    (void)sum;
    return (float)best;          // padding counts as 0 in the max
}

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench Micro MaxPooling (DC-AI-M6), bare-metal port: NCHW, SAME, ksize 3x3, strides 1,1,2,2\n");
        printf("batch %d x %d x %d x %d -> %d x %d x %d x %d, %d jobs (images), %d threads of %d harts\n",
               BATCH, IN_C, IMG_H, IMG_W, BATCH, IN_C, OUT_H, OUT_W, BATCH, nThreads, nharts);
        if (setup_ok) {
            aib_fill_pixels(&input[0][0][0][0], (long)BATCH * IN_C * IMG_H * IMG_W);
            for (int b = 0; b < BATCH; b++)          // constructor: output = zeros
                for (int c = 0; c < IN_C; c++)
                    for (int y = 0; y < OUT_H; y++)
                        for (int x = 0; x < OUT_W; x++)
                            output[b][c][y][x] = 0;
        } else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: max_pool::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, BATCH, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    int errors = 0;
    for (int b = 0; b < BATCH; b++)
        for (int c = 0; c < IN_C; c++)
            for (int y = 0; y < OUT_H; y++)
                for (int x = 0; x < OUT_W; x++) {
                    float want = ref_value(b, c, y, x), got = output[b][c][y][x];
                    if (f2u(got) != f2u(want)) {
                        if (errors < 5)
                            printf("Error at b%d c%d (%d,%d): got %08x want %08x\n", b, c, y, x, f2u(got), f2u(want));
                        errors++;
                    }
                }
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(&output[0][0][0][0], (long)BATCH * IN_C * OUT_H * OUT_W), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM);
    return errors ? 2 : 0;
}
