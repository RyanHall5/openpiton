// AIBench Micro DC-AI-M10 (Dropout), Pthreads version, ported to run
// bare-metal on OpenPiton+Ariane with one thread per hart.
//
// Source: github.com/BenchCouncil/DC_AIBench_Micro (commit fb2e73d),
// Pthread/Pthread/dropout.cpp + dropout.h + tensor.h. run-pthread.sh has no
// dropout entry; this uses the settings of its other tensor kernels
// (NHWC images flattened into one tensor<float>), keep_prob 0.4 as in
// dropout.cpp. run() splits the elements into contiguous ranges, the last
// thread taking the remainder; pthreadFunction() and randFloat() below are the
// originals line for line.
//
// Note what randFloat() does: srand(time(NULL)) and then rand(), for EVERY
// element. In glibc both calls take the process-wide random-number lock, and
// srand() rebuilds the shared 31-word state (31 divide steps plus 310
// discarded outputs). So almost all the work is in the random-number
// generator, every thread works on the same shared state under one lock, and
// within one second every element gets the same "random" number. All of that
// is reproduced here, because it is what generates this benchmark's memory and
// NoC traffic.
//
// Differences from the Linux build, and why (rit/benchmarks/README.md):
//   * srand/rand are rit/aibench.h's copy of glibc's (same algorithm, same
//     results, one shared state behind one lock).
//   * time(NULL) is one load of a fixed value (AIB_EPOCH), where glibc reads
//     the vDSO's time data: also just loads, but here every run reseeds with
//     the same value. Every element is written whether it's kept or dropped,
//     so the traffic doesn't depend on which.
//   * Which elements are kept depends on how the threads interleave (another
//     thread's rand() can land between a thread's srand() and rand()), as on
//     Linux. The checksum therefore varies between runs with more than one
//     thread; the check accepts 0 or x/keep_prob for every element.
//   * The lock spins on a cached load while it's held, where Linux would put
//     the waiting thread to sleep (rit/aibench.h, aib_lock).
//   * Input: pseudo-random 0..255 values instead of JPEG pixels.
//   * Size: about 7,800 instructions per element (the reseeding), and the
//     threads mostly take turns because the work is under the lock. One
//     227x227 image would be 1.2 billion instructions, about a week in VCS, so
//     the default is one 32x32 image (3,072 elements, ~24 M instructions).
//     Set IMG_H/IMG_W to change it.
//   * Threads: barriers instead of std::thread create/join. C at -O2.
//
// Options: BATCH, IMG_H, IMG_W, NTHREADS, AIB_EPOCH.
// After the ROI hart 0 checks every element and prints a checksum to compare
// with the original built on x86 (rit/benchmarks/aibench/micro/).

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/aibench.h"

// Checksum of the ORIGINAL AIBench kernel's output for the default size and
// this input (rit/benchmarks/aibench/micro/check_ports.sh). Applies only when
// BATCH, IMG_H and IMG_W are left at their defaults and only one thread runs
// (with more, which elements are kept depends on timing).
#define AIB_REF_CHECKSUM 0x6e431751526de325ull
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
#define IMG_H 32
#endif
#ifndef IMG_W
#define IMG_W 32
#endif
#define CHANNELS 3
#define NELEM ((long)BATCH * IMG_H * IMG_W * CHANNELS)

static float data[NELEM] __attribute__((aligned(64)));   // dropout's copy of the tensor
static float keep_prob = 0.4f;                            // dropout's member, set by the constructor

// ================ AIBench dropout.h: randFloat, dropout<T>::pthreadFunction ================
static float randFloat(void)
{
    aib_srand(aib_time());            // srand(time(NULL));

    return (float)(aib_rand() / (float)AIB_RAND_MAX);
}

static void pthreadFunction(int pos_start, int pos_end)
{
    for(int pos = pos_start; pos < pos_end; ++pos)
    {
        float keep = randFloat();
        if(keep < keep_prob)
            data[pos] *= 1 / keep_prob;
        else
            data[pos] = 0;
    }
}
// ==========================================================================================

static volatile int setup_ok;
static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

int main(int argc, char **argv)
{
    int hart = argv[0][0], nharts = argv[0][1];
    int nThreads = aib_threads(nharts);

    if (hart == 0) {
        setup_ok = nThreads >= 1 && nThreads <= nharts;
        printf("AIBench Micro Dropout (DC-AI-M10), bare-metal port: NHWC %d x %d x %d x %d = %ld elements, keep_prob 0.4, %d threads of %d harts\n",
               BATCH, IMG_H, IMG_W, CHANNELS, NELEM, nThreads, nharts);
        if (setup_ok)
            aib_fill_pixels(data, NELEM);
        else
            printf("ERROR: NTHREADS must be 1..%d\n", nharts);
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest: dropout<T>::run ----
    aib_roi_begin(hart, nharts);
    long start, end;
    if (aib_range(hart, nThreads, NELEM, &start, &end))
        pthreadFunction(start, end);
    aib_roi_end(hart, nharts);

    // ---- check, outside the ROI ----
    if (hart != 0)
        return 0;
    // With one thread, every element gets the first number after srand(time),
    // so the output is fixed. With several, srand() and rand() take the lock
    // separately, so another thread's rand() can slip in between and a thread
    // gets the 2nd (or later) number instead: which elements are kept depends
    // on the timing, exactly as with glibc on Linux. So the check accepts
    // either outcome for each element, and requires the fixed one only for a
    // single thread.
    float keep1 = randFloat();
    printf("time %u: first rand()/RAND_MAX after srand = %08x, which %s the element\n",
           aib_time(), f2u(keep1), keep1 < keep_prob ? "keeps" : "drops");
    int errors = 0;
    long kept = 0;
    uint32_t seed = 12345;
    for (long i = 0; i < NELEM; i++) {
        seed = seed * 1103515245u + 12345u;
        float x = (float)((seed >> 16) & 0xff);
        float scaled = x * (1 / keep_prob);
        int is_kept = f2u(data[i]) == f2u(scaled) && x != 0;
        int ok = f2u(data[i]) == 0 || f2u(data[i]) == f2u(scaled);
        if (ok && nThreads == 1 && x != 0 && is_kept != (keep1 < keep_prob))
            ok = 0;
        kept += is_kept;
        if (!ok) {
            if (errors < 5)
                printf("Error at %ld (x=%d): got %08x, want 0 or %08x\n", i, (int)x, f2u(data[i]), f2u(scaled));
            errors++;
        }
    }
    printf("kept %ld of %ld elements\n", kept, NELEM);
    aib_print_stats(nharts);
    errors = aib_finish(aib_checksum(data, NELEM), errors, AIB_REF_SIZE && nThreads == 1, AIB_REF_CHECKSUM, AIB_REF_CHECKSUM_ARIANE);
    return errors ? 2 : 0;
}
