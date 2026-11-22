// rit/aibench.h: shared scaffolding for the bare-metal AIBench Micro ports
// (piton/verif/diag/c/riscv/ariane/rit/aibench_*.c). Include after rit/bench.h,
// from one C file only.
//
// What each port gets from here, and which part of the Linux original it
// stands in for:
//
//   aib_fill_pixels()    OpenCV image loading: pseudo-random 0..255 floats, the
//                        same generator and seed as aibench_conv2d.c. Value i
//                        in memory order is step i of the generator, so a host
//                        program can build exactly the same input.
//   aib_threads()        the pthreadNumber argument: NTHREADS if defined,
//                        otherwise every hart.
//   aib_range()          AIBench's job split: jobsNumber / pthreadNumber each,
//                        the last thread also taking the remainder.
//   aib_roi_begin/end()  thread create/join around run(): barrier, ROI marker,
//                        per-hart cycle and instruction counts.
//   aib_lock/unlock()    std::mutex (softmax) and glibc's internal rand() lock
//                        (dropout). See the note at aib_lock.
//   aib_srand/aib_rand() glibc's srand()/rand(): same algorithm, same results,
//                        same shared state behind one lock (dropout).
//   aib_time()           time(NULL) (dropout). See the note there.
//   aib_checksum()       hash of an output array, to compare with the original
//                        built on x86 (rit/benchmarks/aibench/check_ports.sh).
//   aib_finish()         prints the checksum, whether it matches the original's
//                        (each port has the original's checksum for its default
//                        size), and "Num Errors".

#ifndef RIT_AIBENCH_H
#define RIT_AIBENCH_H

#include <stdint.h>
#include <stdio.h>
#include "rit/bench.h"

// ------------------------------------------------------------------ input data
static void aib_fill_pixels(float *p, long n)
{
    uint32_t seed = 12345;
    for (long i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (float)((seed >> 16) & 0xff);
    }
}

// ------------------------------------------------------------------ threads
static int aib_threads(int nharts)
{
#ifdef NTHREADS
    (void)nharts;
    return NTHREADS;
#else
    return nharts;
#endif
}

// Thread `t` of `n` gets jobs [*start, *end). Returns 0 if t has no thread
// (t >= n). Same arithmetic as every AIBench run(): jobsForEachPthread =
// jobsNumber / pthreadNumber; the last thread runs to jobsNumber.
static int aib_range(int t, int n, long jobs, long *start, long *end)
{
    if (t >= n)
        return 0;
    long each = jobs / n;
    *start = t * each;
    *end = (t == n - 1) ? jobs : (t + 1) * each;
    return 1;
}

// ------------------------------------------------------------------ ROI
static volatile uint64_t aib_stats[RIT_MAX_HARTS][8] __attribute__((aligned(64)));
static uint64_t aib_c0[RIT_MAX_HARTS * 8], aib_n0[RIT_MAX_HARTS * 8];

static void aib_roi_begin(int hart, int nharts)
{
    bench_barrier(hart, nharts);
    if (hart == 0)
        noc_roi_begin();
    aib_c0[hart * 8] = read_mcycle();
    aib_n0[hart * 8] = read_minstret();
}

static void aib_roi_end(int hart, int nharts)
{
    aib_stats[hart][0] = read_mcycle() - aib_c0[hart * 8];
    aib_stats[hart][1] = read_minstret() - aib_n0[hart * 8];
    bench_barrier(hart, nharts);
    if (hart == 0)
        noc_roi_end();
}

static void aib_print_stats(int nharts)
{
    for (int i = 0; i < nharts; i++)
        printf("hart %d: ROI cycles %lu, instructions %lu\n", i,
               (unsigned long)aib_stats[i][0], (unsigned long)aib_stats[i][1]);
}

// ------------------------------------------------------------------ checksum
// 64-bit FNV-1a over the float bit patterns, in memory order. Every NaN counts
// as 0x7fc00000 (its payload is not meaningful). Same function in the host
// programs (rit/benchmarks/aibench/aib_host.h).
static uint64_t aib_checksum(const float *p, long n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (long i = 0; i < n; i++) {
        union { float f; uint32_t u; } v = { p[i] };
        uint32_t u = (v.f != v.f) ? 0x7fc00000u : v.u;
        for (int b = 0; b < 4; b++) {
            h ^= (u >> (8 * b)) & 0xff;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

// Prints the checksum, compares it with the original AIBench kernel's checksum
// for the same input when the port has one (default size), and prints the
// "Num Errors" line. Returns the error count, plus one if the checksum differs.
static int aib_finish(uint64_t checksum, int errors, int have_ref, uint64_t ref)
{
    printf("Checksum: 0x%016lx\n", (unsigned long)checksum);
    if (have_ref) {
        if (checksum == ref)
            printf("Checksum matches the original AIBench output\n");
        else {
            printf("Checksum DIFFERS from the original AIBench output (0x%016lx)\n", (unsigned long)ref);
            errors++;
        }
    } else {
        printf("No reference checksum for this size or thread count (output checked by recomputing it)\n");
    }
    printf("Num Errors: %d\n", errors);
    return errors;
}

// ------------------------------------------------------------------ lock
// Stands in for std::mutex / glibc's internal lock. Uncontended, both are one
// atomic on the lock word to take it and one to release it, which is what this
// does. Contended, Linux puts the waiting thread to sleep in the kernel (futex)
// until the lock is released; bare metal has no kernel, so the waiter spins on
// an ordinary load of the lock word instead ("test-and-test-and-set"). That
// load hits in the waiter's own cache until the holder releases the lock, so
// waiting adds almost no NoC traffic, like sleeping.
typedef struct { volatile uint32_t word; } aib_lock_t;

static inline void aib_lock(aib_lock_t *l)
{
    for (;;) {
        uint32_t old;
        __asm__ volatile("amoswap.w.aq %0, %2, %1" : "=r"(old), "+A"(l->word) : "r"(1u) : "memory");
        if (old == 0)
            return;
        while (l->word != 0)
            ;
    }
}

static inline void aib_unlock(aib_lock_t *l)
{
    uint32_t old;
    __asm__ volatile("amoswap.w.rl %0, %2, %1" : "=r"(old), "+A"(l->word) : "r"(0u) : "memory");
    (void)old;
}

// ------------------------------------------------------------------ rand
// glibc's srand()/rand(): rit/aib_rand.h (aib_srand, aib_rand, AIB_RAND_MAX).
#include "rit/aib_rand.h"

// ------------------------------------------------------------------ time
// time(NULL). On Linux, glibc answers this from the vDSO: a few loads from a
// page of kernel time data mapped into the process, no system call and no
// device access. The bare-metal stand-in is one load of a variable that holds
// the time. It's fixed, so every run gets the same random numbers; set
// AIB_EPOCH to change it.
#ifndef AIB_EPOCH
#define AIB_EPOCH 1763856000u   // 2025-11-23 00:00:00 UTC
#endif
static volatile uint32_t aib_epoch = AIB_EPOCH;
static inline uint32_t aib_time(void) { return aib_epoch; }

#endif // RIT_AIBENCH_H
