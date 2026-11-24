// rit_fdiv_check.c: does the FPU divide correctly rounded, and does the build's
// expf() give glibc's results? A diagnostic for the AIBench ports whose output
// checksum differed from the original only in the simulation (sigmoid:
// 1/(1+expf(-x)); avg_pooling: sum/9), while their own in-simulation check
// passed and QEMU matched the original exactly.
//
//   nocrun start rit_fdiv_check.c --no-trace --scratch --fg     (~10-30 min)
//
// Every result is checked against a reference that doesn't use the FPU:
//   * division: an integer long division of the mantissas, rounded to nearest
//     even (soft_divf / soft_divd below), i.e. what IEEE 754 requires;
//   * expf(-v), v = 0..255: glibc's results, computed on x86 and stored below.
// Divisions run twice: as the compiler emits them (rounding mode "dyn" =
// the fcsr setting, round-to-nearest-even) and with the rounding mode in the
// instruction (rne). Same answer expected from both, and from the reference.
//
// Test sets:
//   A  float(s) / 9.0f for s = 1..2295       (every avg_pooling division)
//   B  1.0f / (1 + expf(-v)) for v = 0..255  (every sigmoid division)
//   C  4000 random float divisions
//   D  2000 random double divisions
//   E  expf(-v), v = 0..255, against glibc
// It always ends in PASS; read the counts in uart.log.

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "rit/mathf.h"

static uint32_t f2u(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }
static float u2f(uint32_t u) { union { uint32_t u; float f; } v = { u }; return v.f; }
static uint64_t d2u(double f) { union { double f; uint64_t u; } v = { f }; return v.u; }
static double u2d(uint64_t u) { union { uint64_t u; double f; } v = { u }; return v.f; }

// Correctly rounded a/b (round to nearest, ties to even) for positive normal
// floats whose quotient is normal, using only integer arithmetic.
static float soft_divf(float a, float b)
{
    uint32_t ua = f2u(a), ub = f2u(b);
    int ea = (ua >> 23) & 0xff, eb = (ub >> 23) & 0xff;
    uint64_t ma = (ua & 0x7fffff) | 0x800000, mb = (ub & 0x7fffff) | 0x800000;
    uint64_t num = ma << 26, q = num / mb, rem = num % mb;
    int s, e;
    if (q >= (1ull << 26)) { s = 3; e = ea - eb + 127; }       // ma/mb in [1,2)
    else                   { s = 2; e = ea - eb + 126; }       // ma/mb in (1/2,1)
    uint64_t m = q >> s, low = q & ((1ull << s) - 1), half = 1ull << (s - 1);
    if (low > half || (low == half && (rem != 0 || (m & 1))))
        m++;
    if (m == (1ull << 24)) { m >>= 1; e++; }
    return u2f(((uint32_t)e << 23) | (uint32_t)(m & 0x7fffff));
}

static double soft_divd(double a, double b)
{
    uint64_t ua = d2u(a), ub = d2u(b);
    int ea = (ua >> 52) & 0x7ff, eb = (ub >> 52) & 0x7ff;
    unsigned __int128 ma = (ua & 0xfffffffffffffull) | (1ull << 52), mb = (ub & 0xfffffffffffffull) | (1ull << 52);
    unsigned __int128 num = ma << 55, q = num / mb, rem = num % mb;
    int s, e;
    if (q >= ((unsigned __int128)1 << 55)) { s = 3; e = ea - eb + 1023; }
    else                                   { s = 2; e = ea - eb + 1022; }
    unsigned __int128 m = q >> s, low = q & ((((unsigned __int128)1) << s) - 1), half = ((unsigned __int128)1) << (s - 1);
    if (low > half || (low == half && (rem != 0 || (m & 1))))
        m++;
    if (m == ((unsigned __int128)1 << 53)) { m >>= 1; e++; }
    return u2d(((uint64_t)e << 52) | ((uint64_t)m & 0xfffffffffffffull));
}

static inline float div_rne(float a, float b)
{
#ifdef __riscv
    float q;
    __asm__ volatile("fdiv.s %0, %1, %2, rne" : "=f"(q) : "f"(a), "f"(b));
    return q;
#else
    return a / b;
#endif
}
static inline double divd_rne(double a, double b)
{
#ifdef __riscv
    double q;
    __asm__ volatile("fdiv.d %0, %1, %2, rne" : "=f"(q) : "f"(a), "f"(b));
    return q;
#else
    return a / b;
#endif
}

// a/b as the compiler emits it (dynamic rounding mode); volatile so it isn't folded
static volatile float va, vb;
static volatile double vda, vdb;

static int bad_dyn, bad_rne, shown;
static void check_f(char set, float a, float b)
{
    va = a; vb = b;
    float q = va / vb, qr = div_rne(a, b), ref = soft_divf(a, b);
    if (f2u(q) != f2u(ref)) bad_dyn++;
    if (f2u(qr) != f2u(ref)) bad_rne++;
    if ((f2u(q) != f2u(ref) || f2u(qr) != f2u(ref)) && shown++ < 8)
        printf("  %c: %08x / %08x = %08x (dyn) %08x (rne), correct %08x\n", set, f2u(a), f2u(b), f2u(q), f2u(qr), f2u(ref));
}

static const uint32_t glibc_expf_neg[256] = {
0x3f800000u, 0x3ebc5ab2u, 0x3e0a9555u, 0x3d4bed86u, 0x3c960aaeu, 0x3bdcc9ffu, 0x3b227290u, 0x3a6f0b5du,
0x39afe108u, 0x39016791u, 0x383e6bceu, 0x378c1aa1u, 0x36ce2a62u, 0x3617b02au, 0x355f3638u, 0x34a43ae5u,
0x33f1aadeu, 0x3331cf19u, 0x3282d314u, 0x31c082b8u, 0x310da433u, 0x30506d87u, 0x2f995a46u, 0x2ee1a93fu,
0x2e26083cu, 0x2d7451bdu, 0x2cb3c295u, 0x2c044295u, 0x2b429f81u, 0x2a8f3216u, 0x29d2b706u, 0x291b090fu,
0x28642328u, 0x27a7daa4u, 0x26f7000fu, 0x2635bb8du, 0x2585b61du, 0x24c4c239u, 0x2410c457u, 0x235506f2u,
0x229cbc92u, 0x21e6a405u, 0x2129b229u, 0x2079b5eau, 0x1fb7ba0fu, 0x1f072dbau, 0x1e46eaf1u, 0x1d925b02u,
0x1cd75d5du, 0x1c1e74ddu, 0x1b692bebu, 0x1aab8edcu, 0x19fc7361u, 0x1939be2bu, 0x1888a975u, 0x17c919b9u,
0x1713f623u, 0x1659ba5au, 0x15a031fcu, 0x14ebbaecu, 0x142d70c9u, 0x137f388bu, 0x12bbc7f1u, 0x120a295cu,
0x114b4ea4u, 0x109595c7u, 0x0fdc1df9u, 0x0f21f3feu, 0x0e6e511eu, 0x0daf5800u, 0x0d0102bfu, 0x0c3dd771u,
0x0b8bad78u, 0x0acd89c1u, 0x0a1739fbu, 0x095e884fu, 0x08a3baf0u, 0x07f0ee94u, 0x07314490u, 0x06826d27u,
0x05bfecbau, 0x050d35d7u, 0x044fcb22u, 0x0398e2cbu, 0x02e0f96du, 0x022586e0u, 0x01739362u, 0x00b33687u,
0x0041edc4u, 0x001840fcu, 0x0008ec28u, 0x0003484bu, 0x00013521u, 0x000071b9u, 0x000029d6u, 0x00000f64u,
0x000005a9u, 0x00000215u, 0x000000c4u, 0x00000048u, 0x0000001bu, 0x0000000au, 0x00000004u, 0x00000001u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u
};

int main(int argc, char **argv)
{
    int hart = argv[0][0];
    if (hart != 0)
        return 0;
    printf("rit_fdiv_check: FPU division and expf against IEEE / glibc references\n");

    bad_dyn = bad_rne = shown = 0;
    for (int s = 1; s <= 2295; s++)
        check_f('A', (float)s, 9.0f);
    printf("A float(s)/9, s=1..2295:        %4d wrong (dyn), %4d wrong (rne) of 2295\n", bad_dyn, bad_rne);

    int a_dyn = bad_dyn, a_rne = bad_rne;
    bad_dyn = bad_rne = 0;
    for (int v = 0; v < 256; v++)
        check_f('B', 1.0f, 1 + u2f(glibc_expf_neg[v]));
    printf("B 1/(1+expf(-v)), v=0..255:     %4d wrong (dyn), %4d wrong (rne) of 256\n", bad_dyn, bad_rne);

    int b_dyn = bad_dyn, b_rne = bad_rne;
    bad_dyn = bad_rne = 0;
    uint32_t seed = 12345;
    for (int n = 0; n < 4000; n++) {
        seed = seed * 1103515245u + 12345u; uint32_t x = seed;
        seed = seed * 1103515245u + 12345u; uint32_t y = seed;
        float a = u2f(0x3f800000u | (x >> 9)) * (float)(1 + (x & 0xff));   // [1, 512)
        float b = u2f(0x3f800000u | (y >> 9)) * (float)(1 + (y & 0xff));
        check_f('C', a, b);
    }
    printf("C random float, 4000:           %4d wrong (dyn), %4d wrong (rne)\n", bad_dyn, bad_rne);

    int d_dyn = 0, d_rne = 0;
    for (int n = 0; n < 2000; n++) {
        seed = seed * 1103515245u + 12345u; uint64_t x = ((uint64_t)seed << 32);
        seed = seed * 1103515245u + 12345u; x |= seed;
        seed = seed * 1103515245u + 12345u; uint64_t y = ((uint64_t)seed << 32);
        seed = seed * 1103515245u + 12345u; y |= seed;
        double a = u2d(0x3ff0000000000000ull | (x >> 12)) * (double)(1 + (x & 0x3ff));
        double b = u2d(0x3ff0000000000000ull | (y >> 12)) * (double)(1 + (y & 0x3ff));
        vda = a; vdb = b;
        double q = vda / vdb, qr = divd_rne(a, b), ref = soft_divd(a, b);
        if (d2u(q) != d2u(ref)) d_dyn++;
        if (d2u(qr) != d2u(ref)) d_rne++;
        if ((d2u(q) != d2u(ref) || d2u(qr) != d2u(ref)) && shown++ < 12)
            printf("  D: %016lx / %016lx = %016lx (dyn) %016lx (rne), correct %016lx\n",
                   (unsigned long)d2u(a), (unsigned long)d2u(b), (unsigned long)d2u(q), (unsigned long)d2u(qr), (unsigned long)d2u(ref));
    }
    printf("D random double, 2000:          %4d wrong (dyn), %4d wrong (rne)\n", d_dyn, d_rne);

    int e_bad = 0, e_shown = 0;
    for (int v = 0; v < 256; v++) {
        float e = expf(-(float)v);
        if (f2u(e) != glibc_expf_neg[v]) {
            e_bad++;
            if (e_shown++ < 5)
                printf("  E: expf(-%d) = %08x, glibc %08x\n", v, f2u(e), glibc_expf_neg[v]);
        }
    }
    printf("E expf(-v) vs glibc, v=0..255:  %4d differ of 256\n", e_bad);

    printf("SUMMARY: division %s; expf %s\n",
           (a_dyn | a_rne | b_dyn | b_rne | bad_dyn | bad_rne | d_dyn | d_rne) ? "NOT correctly rounded on some inputs" : "correctly rounded on every input tested",
           e_bad ? "DIFFERS from glibc" : "matches glibc");
    return 0;
}
