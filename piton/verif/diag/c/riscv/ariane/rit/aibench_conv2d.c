// AIBench Micro DC-AI-M1 (Convolution), Pthreads version, ported to run
// bare-metal on OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro, Pthread/Pthread/conv2d.cpp,
// run as in Pthread/run-pthread.sh ("conv"):
//     ./conv2d <images>/ NCHW 12 227 227 100
// i.e. NCHW data, 227x227 BGR images, the 3x3x3x2 filter hard-coded in its main(),
// strides {1,1,2,2}, SAME padding. The work is split the same way: one job per
// (image, output channel), jobs dealt to threads in contiguous ranges, the last
// thread taking the remainder. conv2dSingleFilter() below is the NCHW + SAME
// branch of the original, line for line.
//
// Differences from the Linux build, and why:
//   * C, flat arrays: the original is C++ with nested std::vector (every row its
//     own heap block) and is built with -g and no -O. sims compiles C at -O2.
//     The loop order and the element access order are the same, so each thread
//     touches the same data in the same order; the per-row pointer loads and the
//     unoptimized instruction stream are not reproduced.
//   * Pixels: the original reads ImageNet JPEGs with OpenCV (values 0..255 as
//     float). Here hart 0 fills the images with pseudo-random 0..255 values. The
//     kernel has no data-dependent branches or addresses, so the memory and NoC
//     traffic doesn't depend on the pixel values.
//   * One batch: the original walks 1000 images in batches of 100 and starts one
//     batch thread per batch. Here BATCH images form one batch (default 1, so
//     the 2x1 model gets one job per hart).
//
// Run on the 2x1 model (both harts):
//   sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run aibench_conv2d.c \
//        -ariane -finish_mask 0x11 -rtl_timeout 10000000 \
//        -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=conv2d_2x1_r1.csv
//
// Compile-time options (-gcc_args=-DNAME=VALUE, one per -gcc_args):
//   BATCH        images per batch (default 1)
//   IMG_H/IMG_W  image size (default 227x227, as run-pthread.sh)
//   NTHREADS     threads to use, at most the number of harts (default: all harts)
//
// After the ROI, hart 0 recomputes every output pixel with an independent
// reference (integer arithmetic) and checks they match exactly: every product is
// an integer pixel value times -1/0/1, so float sums here are exact.
//
// See rit/benchmarks/README.md.

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"

#ifndef BATCH
#define BATCH 1
#endif
#ifndef IMG_H
#define IMG_H 227
#endif
#ifndef IMG_W
#define IMG_W 227
#endif

// ---- shapes, as conv2d's constructor works them out ----
#define IN_C        3
#define FILTER_H    3
#define FILTER_W    3
#define FILTER_OUT  2
#define STRIDE_C    1                                  // strides = {1, 1, 2, 2} (NCHW)
#define STRIDE_H    2
#define STRIDE_W    2
#define OUT_H       ((IMG_H + STRIDE_H - 1) / STRIDE_H) // ceil(in / stride), SAME padding
#define OUT_W       ((IMG_W + STRIDE_W - 1) / STRIDE_W)
#define PAD_NEED_H  ((OUT_H - 1) * STRIDE_H + FILTER_H - IMG_H)
#define PAD_NEED_W  ((OUT_W - 1) * STRIDE_W + FILTER_W - IMG_W)
#define PAD_TOP     (PAD_NEED_H / 2)
#define PAD_LEFT    (PAD_NEED_W / 2)

// input[b][c][h][w], filter[h][w][i][o], output[b][o][h][w]
static float input [BATCH][IN_C][IMG_H][IMG_W]                 __attribute__((aligned(64)));
static float filter[FILTER_H][FILTER_W][IN_C][FILTER_OUT]      __attribute__((aligned(64)));
static float output[BATCH][FILTER_OUT][OUT_H][OUT_W]           __attribute__((aligned(64)));

// conv2d's member variables
static const int input_heightSize = IMG_H, input_widthSize = IMG_W, input_channelsSize = IN_C;
static const int filter_heightSize = FILTER_H, filter_widthSize = FILTER_W;
static const int filter_outChannelsSize = FILTER_OUT;
static const int stride_height = STRIDE_H, stride_width = STRIDE_W, stride_channels = STRIDE_C;
static const int output_heightSize = OUT_H, output_widthSize = OUT_W;
static const int pad_top = PAD_TOP, pad_left = PAD_LEFT;

static int nThreads;

// ============ AIBench conv2d::conv2dSingleFilter, NCHW + SAME branch ============
static void conv2dSingleFilter(float (*input)[IMG_H][IMG_W], int filterOutChannelId,
                               float (*output)[OUT_H][OUT_W], int outputChannelId)
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
            float sum = 0.0;
            /* convolution for each channel of a single filter */
            for(int pos_channel = 0; pos_channel < input_channelsSize; ++pos_channel)
            {
                int offset_height = pos_height * stride_height;
                int offset_width = pos_width * stride_width;
                int offset_channel = pos_channel * stride_channels;
                /* mul-sum between each channel of a single filter and the related part in input */
                float subSum = 0.0;
                for(int h = 0; h < filter_heightSize; ++h)
                for(int w = 0; w < filter_widthSize; ++w)
                {
                    /* position in padded_input */
                    int newPos_height = h + offset_height;
                    int newPos_width = w + offset_width;
                    /* check weather current point is in padded area */
                    if(newPos_height >= bound_top && newPos_height <= bound_down && newPos_width >= bound_left && newPos_width <= bound_right)
                    {
                        subSum += input[offset_channel][newPos_height - pad_top][newPos_width - pad_left] * filter[h][w][offset_channel][filterOutChannelId];
                    }
                }
                sum += subSum;
            }
            output[outputChannelId][pos_height][pos_width] = sum;
        }
    }
}

// ============ conv2d::pthreadFunction ============
static void pthreadFunction(int jobId_start, int jobId_end)
{
    for(int jId = jobId_start; jId < jobId_end; ++jId)
    {
        int batchId = jId / filter_outChannelsSize;
        int filterId = jId % filter_outChannelsSize;
        conv2dSingleFilter(input[batchId], filterId, output[batchId], filterId);
    }
}
// ================================================================================

// the filter from AIBench conv2d.cpp main()
static const float filter_init[54] = {
     1, -1, -1,  1,  0, -1,
     1, -1,  0, -1,  1,  0,
    -1,  0, -1,  0,  0,  1,
    -1, -1,  0, -1,  1,  1,
     0,  1,  0,  0,  0,  0,
     1,  0, -1, -1,  1,  1,
    -1, -1,  1, -1,  0,  0,
    -1,  1, -1,  0, -1, -1,
     0,  0,  0,  0,  1,  0};

// independent reference for the check: integers, padded image, no bounds tests
static int32_t ref_pixel(int b, int o, int oh, int ow)
{
    int32_t s = 0;
    for (int c = 0; c < IN_C; c++)
        for (int h = 0; h < FILTER_H; h++)
            for (int w = 0; w < FILTER_W; w++) {
                int y = oh * STRIDE_H + h - PAD_TOP, x = ow * STRIDE_W + w - PAD_LEFT;
                if (y < 0 || y >= IMG_H || x < 0 || x >= IMG_W) continue;  // zero padding
                s += (int32_t)input[b][c][y][x] * (int32_t)filter_init[((h * FILTER_W + w) * IN_C + c) * FILTER_OUT + o];
            }
    return s;
}

static volatile uint64_t roi_stats[RIT_MAX_HARTS][8] __attribute__((aligned(64)));
static volatile int setup_ok;

int main(int argc, char **argv)
{
    int hart = argv[0][0];
    int nharts = argv[0][1];
    int jobsNumber = BATCH * FILTER_OUT;

    // ---- setup (original: load images with OpenCV, build the filter) ----
    if (hart == 0) {
#ifdef NTHREADS
        nThreads = NTHREADS;
#else
        nThreads = nharts;
#endif
        setup_ok = (nThreads >= 1 && nThreads <= nharts);
        if (setup_ok) {
            printf("AIBench conv2d (bare-metal port): NCHW, SAME, strides 1,1,2,2\n");
            printf("batch %d x %d x %d x %d -> %d x %d x %d x %d, %d jobs, %d threads of %d harts\n",
                   BATCH, IN_C, IMG_H, IMG_W, BATCH, FILTER_OUT, OUT_H, OUT_W,
                   jobsNumber, nThreads, nharts);
            int pos = 0;
            for (int h = 0; h < 3; ++h)
                for (int w = 0; w < 3; ++w)
                    for (int i = 0; i < 3; ++i)
                        for (int o = 0; o < 2; ++o)
                            filter[h][w][i][o] = filter_init[pos++];
            uint32_t seed = 12345;
            for (int b = 0; b < BATCH; b++)
                for (int c = 0; c < IN_C; c++)
                    for (int y = 0; y < IMG_H; y++)
                        for (int x = 0; x < IMG_W; x++) {
                            seed = seed * 1103515245u + 12345u;
                            input[b][c][y][x] = (float)((seed >> 16) & 0xff);
                        }
        } else {
            printf("ERROR: NTHREADS=%d must be 1..%d (harts)\n", nThreads, nharts);
        }
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest (original: conv2d::run) ----
    if (hart == 0)
        noc_roi_begin();
    uint64_t c0 = read_mcycle(), n0 = read_minstret();
    int jobsForEachPthread = jobsNumber / nThreads;
    if (hart < nThreads - 1)
        pthreadFunction(hart * jobsForEachPthread, (hart + 1) * jobsForEachPthread);
    else if (hart == nThreads - 1)
        pthreadFunction((nThreads - 1) * jobsForEachPthread, jobsNumber);
    roi_stats[hart][0] = read_mcycle() - c0;
    roi_stats[hart][1] = read_minstret() - n0;
    bench_barrier(hart, nharts);
    if (hart == 0)
        noc_roi_end();

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    int errors = 0;
    int64_t checksum = 0;
    for (int b = 0; b < BATCH; b++)
        for (int o = 0; o < FILTER_OUT; o++)
            for (int y = 0; y < OUT_H; y++)
                for (int x = 0; x < OUT_W; x++) {
                    int32_t want = ref_pixel(b, o, y, x);
                    float got = output[b][o][y][x];
                    if (got != (float)want) {
                        if (errors < 5)
                            printf("Error at b%d o%d (%d,%d): got %d want %d\n", b, o, y, x, (int)got, want);
                        errors++;
                    }
                    checksum += (int64_t)got * (1 + ((y * OUT_W + x) % 7));
                }
    for (int i = 0; i < nharts; i++)
        printf("hart %d: ROI cycles %lu, instructions %lu\n", i,
               (unsigned long)roi_stats[i][0], (unsigned long)roi_stats[i][1]);
    printf("Checksum: %ld\n", (long)checksum);
    printf("Num Errors: %d\n", errors);
    return errors ? 2 : 0;
}
