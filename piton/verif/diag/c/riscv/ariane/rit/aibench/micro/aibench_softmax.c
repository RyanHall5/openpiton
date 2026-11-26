// AIBench Micro DC-AI-M12 (Softmax), Pthreads version, ported to run bare-metal
// on OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/softmax.cpp + softmax.h + tensor.h. run-pthread.sh has no
// softmax entry; this uses the same settings as its other tensor kernels:
// ./softmax <images>/ 12 227 227 100 (NHWC, one flat tensor<float>).
// softmax<T>::run() is two parallel phases with a join between them:
//   pre:  each thread replaces its elements with exp(x) and adds them up, then
//         adds its partial sum to the shared `sum` under a std::mutex;
//   next: each thread divides its elements by `sum`.
// Both phases split the elements the same way (contiguous ranges, last thread
// takes the remainder). pthreadFunction_pre/_next below are the originals line
// for line.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * Input: hart 0 fills the array with pseudo-random 0..255 values (images
//     are 0..255 too). As with real images, exp(x) overflows to infinity for
//     x > 88, so `sum` is infinite and the output is 0 or NaN. That's what
//     the original computes on image data; the memory traffic is the same as
//     for any input (every element read and written in both phases).
//   * The mutex is rit/aibench.h's lock: one atomic to take it, one to release
//     it, spinning on a cached load if it's held (Linux would sleep instead).
//     `sum` and the lock sit in one 64-byte block, as `sum` and `sum_mutex` sit
//     next to each other in the softmax object.
//   * The join between the phases, and thread create/join around run(), are
//     barriers. expf: rit/mathf.h (same algorithm as glibc's).
//   * One batch of BATCH images (default 1). C at -O2 instead of C++ at -O0.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS.
// After the ROI hart 0 checks every element and prints a checksum to compare
// with the original built on x86 (rit/benchmarks/aibench/micro/).

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"
#include "rit/mathf.h"

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/micro/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults.
#define AIB_REF_CHECKSUM 0xc6c354bfc0155f45ull
#define AIB_REF_CHECKSUM_ARIANE AIB_REF_CHECKSUM   // Ariane gives the original's output
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

static float data[NELEM] __attribute__((aligned(64)));   // softmax's copy of the tensor (logits)

// softmax's members `sum` and `sum_mutex`, side by side
static struct {
    float sum;
    aib_lock_t sum_mutex;
} sm __attribute__((aligned(64)));

// ================ AIBench softmax<T>::pthreadFunction_pre / _next ================
static void pthreadFunction_pre(int pos_start, int pos_end)
{
    float temp_sum = 0;
    for(int pos = pos_start; pos < pos_end; ++pos)
    {
        data[pos] = expf(data[pos]);
        temp_sum += data[pos];
    }
    aib_lock(&sm.sum_mutex);          // lock_guard<mutex> guard(sum_mutex);
    sm.sum += temp_sum;
    aib_unlock(&sm.sum_mutex);
}

static void pthreadFunction_next(int pos_start, int pos_end)
{
    for(int pos = pos_start; pos < pos_end; ++pos)
    {
        if(sm.sum != 0)
        data[pos] = data[pos] / sm.sum;
    }
}
// ===================================================================================

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench Micro Softmax (DC-AI-M12), bare-metal port: NHWC %d x %d x %d x %d = %ld elements, %d threads of %d harts\n",
               BATCH, IMG_H, IMG_W, CHANNELS, NELEM, nThreads, nharts);
        if (setup_ok) {
            aib_fill_pixels(data, NELEM);
            sm.sum = 0;                    // constructor: sum = 0
        } else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: softmax<T>::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    int mine = aib_range(hart, nThreads, NELEM, &start, &end);
    if (mine)
        pthreadFunction_pre(start, end);
    bench_barrier(hart, nharts);           // join pthreads_pre
    if (mine)
        pthreadFunction_next(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    // expected sum: each thread's partial sum in its own element order, added
    // in thread order (the original adds them in whatever order threads finish;
    // with these inputs the sum is infinite either way)
    static float e[256];
    for (int v = 0; v < 256; v++)
        e[v] = expf((float)v);
    float want_sum = 0;
    uint32_t seed = 12345;
    for (int t = 0; t < nThreads; t++) {
        long s0, s1;
        aib_range(t, nThreads, NELEM, &s0, &s1);
        float part = 0;
        for (long i = s0; i < s1; i++) {
            seed = seed * 1103515245u + 12345u;
            part += e[(seed >> 16) & 0xff];
        }
        want_sum += part;
    }
    int errors = 0;
    if (f2u(sm.sum) != f2u(want_sum)) {
        printf("Error: sum %08x want %08x\n", f2u(sm.sum), f2u(want_sum));
        errors++;
    }
    seed = 12345;
    for (long i = 0; i < NELEM; i++) {
        seed = seed * 1103515245u + 12345u;
        int x = (seed >> 16) & 0xff;
        float want = want_sum != 0 ? e[x] / want_sum : e[x];
        float got = data[i];
        int same = (got != got && want != want) || f2u(got) == f2u(want);
        if (!same) {
            if (errors < 5)
                printf("Error at %ld (x=%d): got %08x want %08x\n", i, x, f2u(got), f2u(want));
            errors++;
        }
    }
    aib_print_stats(nharts);
    printf("sum: %08x\n", f2u(sm.sum));
    errors = aib_finish(aib_checksum(data, NELEM), errors, AIB_REF_SIZE, AIB_REF_CHECKSUM, AIB_REF_CHECKSUM_ARIANE);
    return errors ? 2 : 0;
}
