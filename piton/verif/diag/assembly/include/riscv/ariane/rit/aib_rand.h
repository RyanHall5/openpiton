// rit/aib_rand.h: glibc's srand()/rand() for the bare-metal AIBench ports.
// Included by rit/aibench.h, which defines aib_lock_t, aib_lock() and
// aib_unlock() first. rit/benchmarks/aibench/micro/test_rand.c includes it on x86
// (with a no-op lock) and checks the results against the real glibc.
//
// glibc's srand()/rand() (stdlib/random.c, random_r.c): the default TYPE_3
// additive generator, state of 31 words, one process-wide state protected by
// one lock. srand() reseeds it (31 multiply/divide steps, then 310 discarded
// outputs); rand() advances it one step. Results are identical to glibc's for
// the same seed (checked on x86, rit/benchmarks/aibench/micro/). The table, the
// state pointers and the lock live in separate places, as in glibc (randtbl
// and unsafe_state in .data, the lock in .bss).
#ifndef RIT_AIB_RAND_H
#define RIT_AIB_RAND_H

#include <stdint.h>

#define AIB_RAND_DEG 31
#define AIB_RAND_SEP 3
#define AIB_RAND_MAX 2147483647

static int32_t aib_randtbl[AIB_RAND_DEG + 1];   // [0] = type word, then the state
static struct {
    int32_t *fptr, *rptr, *state;
    int rand_type, rand_deg, rand_sep;
    int32_t *end_ptr;
} aib_unsafe_state = {
    &aib_randtbl[AIB_RAND_SEP + 1], &aib_randtbl[1], &aib_randtbl[1],
    3, AIB_RAND_DEG, AIB_RAND_SEP, &aib_randtbl[AIB_RAND_DEG + 1]
};
static aib_lock_t aib_rand_lock;

static void aib_random_r(int32_t *result)
{
    int32_t *state = aib_unsafe_state.state;
    int32_t *fptr = aib_unsafe_state.fptr;
    int32_t *rptr = aib_unsafe_state.rptr;
    int32_t *end_ptr = aib_unsafe_state.end_ptr;
    uint32_t val;

    val = *fptr += (uint32_t)*rptr;
    /* Chucking least random bit.  */
    *result = val >> 1;
    ++fptr;
    if (fptr >= end_ptr) {
        fptr = state;
        ++rptr;
    } else {
        ++rptr;
        if (rptr >= end_ptr)
            rptr = state;
    }
    aib_unsafe_state.fptr = fptr;
    aib_unsafe_state.rptr = rptr;
}

static void aib_srandom_r(unsigned int seed)
{
    int32_t *state = aib_unsafe_state.state;
    int32_t word;
    int32_t *dst;
    int kc;

    /* We must make sure the seed is not 0.  Take arbitrarily 1 in this case.  */
    if (seed == 0)
        seed = 1;
    state[0] = seed;
    dst = state;
    word = seed;
    kc = aib_unsafe_state.rand_deg;
    for (long i = 1; i < kc; ++i) {
        /* This does:
             state[i] = (16807 * state[i - 1]) % 2147483647;
           but avoids overflowing 31 bits.  */
        long hi = word / 127773;
        long lo = word % 127773;
        word = 16807 * lo - 2836 * hi;
        if (word < 0)
            word += 2147483647;
        *++dst = word;
    }
    aib_unsafe_state.fptr = &state[aib_unsafe_state.rand_sep];
    aib_unsafe_state.rptr = &state[0];
    kc *= 10;
    while (--kc >= 0) {
        int32_t discard;
        aib_random_r(&discard);
    }
}

static void aib_srand(unsigned int seed)
{
    aib_lock(&aib_rand_lock);
    aib_srandom_r(seed);
    aib_unlock(&aib_rand_lock);
}

static int aib_rand(void)
{
    int32_t retval;
    aib_lock(&aib_rand_lock);
    aib_random_r(&retval);
    aib_unlock(&aib_rand_lock);
    return retval;
}

#endif // RIT_AIB_RAND_H
