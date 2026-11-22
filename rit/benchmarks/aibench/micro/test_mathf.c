// Compare rit/mathf.h's expf/tanhf with the host libm (glibc) on every float.
//   gcc -O2 -ffp-contract=off rit/benchmarks/aibench/micro/test_mathf.c -I piton/verif/diag/assembly/include/riscv/ariane -lm
// Prints how many inputs differ and the largest difference in ulps, then the
// results for the integer inputs 0..255 (the values the AIBench ports use).
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define expf   rit_expf
#define expm1f rit_expm1f
#define tanhf  rit_tanhf
#include "rit/mathf.h"
#undef expf
#undef expm1f
#undef tanhf

static int64_t ulps(float a, float b)
{
    if (isnan(a) && isnan(b)) return 0;
    if (isnan(a) || isnan(b)) return INT64_MAX;
    int32_t ia, ib; memcpy(&ia, &a, 4); memcpy(&ib, &b, 4);
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    int64_t d = (int64_t)ia - ib; return d < 0 ? -d : d;
}

static void sweep(const char *name, float (*mine)(float), float (*ref)(float))
{
    uint64_t diff = 0; int64_t worst = 0; uint32_t worst_at = 0;
    for (uint64_t i = 0; i <= 0xffffffffull; i++) {
        uint32_t b = (uint32_t)i; float x; memcpy(&x, &b, 4);
        float m = mine(x), r = ref(x);
        if (memcmp(&m, &r, 4) != 0 && !(isnan(m) && isnan(r))) {
            diff++;
            int64_t u = ulps(m, r);
            if (u > worst) { worst = u; worst_at = b; }
        }
    }
    printf("%-6s all 2^32 inputs: %llu differ from libm, max %lld ulp (at 0x%08x)\n",
           name, (unsigned long long)diff, (long long)worst, worst_at);
    int ints = 0;
    for (int v = 0; v < 256; v++) {
        float a = (float)v, b = -(float)v;
        if (memcmp((float[]){mine(a)}, (float[]){ref(a)}, 4) || memcmp((float[]){mine(b)}, (float[]){ref(b)}, 4)) ints++;
    }
    printf("%-6s integers -255..255: %d differ\n", name, ints);
}

static float glibc_expf(float x) { return expf(x); }
static float glibc_tanhf(float x) { return tanhf(x); }

int main(void)
{
    sweep("expf", rit_expf, glibc_expf);
    sweep("tanhf", rit_tanhf, glibc_tanhf);
    return 0;
}
