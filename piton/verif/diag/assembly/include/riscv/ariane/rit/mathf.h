// rit/mathf.h: single-precision expf() and tanhf() for bare-metal tests.
//
// Why this exists: sims links tests with -nostdlib -lm, and the toolchain's
// libm isn't built for Ariane's 0x80000000 load address (see rit/math.h for the
// same problem with exp/log). AIBench's sigmoid and softmax call std::exp on a
// float (= expf) and tanh calls std::tanh on a float (= tanhf).
//
// Sources, from musl (git.musl-libc.org, mirror github.com/kraj/musl, commit
// 3e699121):
//   expf    src/math/expf.c + exp2f_data.c. Arm's optimized-routines code
//           (MIT), the same algorithm and table glibc has used for expf since
//           2.27. Checked on all 2^32 inputs against glibc 2.39 on x86: 2
//           differ, by 1 ulp (glibc picks an FMA variant on that CPU).
//   tanhf   src/math/tanhf.c + expm1f.c (musl / FreeBSD msun, Sun's fdlibm
//           licence below). glibc's tanhf is a different implementation:
//           1 input in 250 differs, by at most 3 ulp. Every integer input
//           -255..255 (the pixel values the AIBench ports use) gives exactly
//           glibc's result. Test: rit/benchmarks/aibench/micro/test_mathf.c.
//
// Changes from musl: the table is renamed rit_exp2f_data; musl's internal
// helpers are written out; fp-contract is turned off so GCC doesn't fuse
// multiply-adds (musl's results, and glibc's on x86, assume unfused
// arithmetic).
//
// Include from one C file only (the functions are defined here, not static, so
// the names resolve the way the original's calls to libm do).

#ifndef RIT_MATHF_H
#define RIT_MATHF_H

#include <stdint.h>

#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")

// ---- musl libm.h helpers ----
static inline uint32_t rit_asuint(float f)     { union { float f; uint32_t i; } u = { f }; return u.i; }
static inline float    rit_asfloat(uint32_t i) { union { uint32_t i; float f; } u = { i }; return u.f; }
static inline uint64_t rit_asuint64(double f)  { union { double f; uint64_t i; } u = { f }; return u.i; }
static inline double   rit_asdouble(uint64_t i){ union { uint64_t i; double f; } u = { i }; return u.f; }
static inline float  rit_fp_barrierf(float x)  { volatile float y = x; return y; }
static inline double rit_eval_as_double(double x) { return x; }
static inline float  rit_eval_as_float(float x)   { return x; }
#define RIT_FORCE_EVALF(x) do { volatile float rit__y; rit__y = (x); (void)rit__y; } while (0)
static float rit_math_oflowf(uint32_t sign) { float y = rit_fp_barrierf(sign ? -0x1p97f : 0x1p97f) * 0x1p97f; return y; }
static float rit_math_uflowf(uint32_t sign) { float y = rit_fp_barrierf(sign ? -0x1p-95f : 0x1p-95f) * 0x1p-95f; return y; }

// ===================================================================== expf
/*
 * Single-precision e^x function.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
#define RIT_EXP2F_TABLE_BITS 5
#define RIT_EXP2F_POLY_ORDER 3
static const struct rit_exp2f_data_t {
    uint64_t tab[1 << RIT_EXP2F_TABLE_BITS];
    double shift_scaled;
    double poly[RIT_EXP2F_POLY_ORDER];
    double shift;
    double invln2_scaled;
    double poly_scaled[RIT_EXP2F_POLY_ORDER];
} rit_exp2f_data = {
  /* tab[i] = uint(2^(i/N)) - (i << 52-BITS)
     used for computing 2^(k/N) for an int |k| < 150 N as
     double(tab[k%N] + (k << 52-BITS)) */
  .tab = {
0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51,
0x3fef72b83c7d517b, 0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1,
0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585,
0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069,
0x3fef5818dcfba487, 0x3fef7c97337b9b5f, 0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
  },
  .shift_scaled = 0x1.8p+52 / (1 << RIT_EXP2F_TABLE_BITS),
  .poly = {
  0x1.c6af84b912394p-5, 0x1.ebfce50fac4f3p-3, 0x1.62e42ff0c52d6p-1,
  },
  .shift = 0x1.8p+52,
  .invln2_scaled = 0x1.71547652b82fep+0 * (1 << RIT_EXP2F_TABLE_BITS),
  .poly_scaled = {
  0x1.c6af84b912394p-5/32/32/32, 0x1.ebfce50fac4f3p-3/32/32, 0x1.62e42ff0c52d6p-1/32,
  },
};

static inline uint32_t rit_top12f(float x) { return rit_asuint(x) >> 20; }

float expf(float x)
{
    uint32_t abstop;
    uint64_t ki, t;
    double kd, xd, z, r, r2, y, s;

    xd = (double)x;
    abstop = rit_top12f(x) & 0x7ff;
    if (__builtin_expect(abstop >= rit_top12f(88.0f), 0)) {
        /* |x| >= 88 or x is nan.  */
        if (rit_asuint(x) == rit_asuint(-__builtin_inff()))
            return 0.0f;
        if (abstop >= rit_top12f(__builtin_inff()))
            return x + x;
        if (x > 0x1.62e42ep6f) /* x > log(0x1p128) ~= 88.72 */
            return rit_math_oflowf(0);
        if (x < -0x1.9fe368p6f) /* x < log(0x1p-150) ~= -103.97 */
            return rit_math_uflowf(0);
    }

    /* x*N/Ln2 = k + r with r in [-1/2, 1/2] and int k.  */
    z = rit_exp2f_data.invln2_scaled * xd;

    /* Round and convert z to int, the result is in [-150*N, 128*N] and
       ideally ties-to-even rule is used, otherwise the magnitude of r
       can be bigger which gives larger approximation error.  */
    kd = rit_eval_as_double(z + rit_exp2f_data.shift);
    ki = rit_asuint64(kd);
    kd -= rit_exp2f_data.shift;
    r = z - kd;

    /* exp(x) = 2^(k/N) * 2^(r/N) ~= s * (C0*r^3 + C1*r^2 + C2*r + 1) */
    t = rit_exp2f_data.tab[ki % (1 << RIT_EXP2F_TABLE_BITS)];
    t += ki << (52 - RIT_EXP2F_TABLE_BITS);
    s = rit_asdouble(t);
    z = rit_exp2f_data.poly_scaled[0] * r + rit_exp2f_data.poly_scaled[1];
    r2 = r * r;
    y = rit_exp2f_data.poly_scaled[2] * r + 1;
    y = z * r2 + y;
    y = y * s;
    return rit_eval_as_float(y);
}

// ===================================================================== expm1f
/* origin: FreeBSD /usr/src/lib/msun/src/s_expm1f.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
static const float
rit_ln2_hi  = 6.9313812256e-01, /* 0x3f317180 */
rit_ln2_lo  = 9.0580006145e-06, /* 0x3717f7d1 */
rit_invln2  = 1.4426950216e+00, /* 0x3fb8aa3b */
/*
 * Domain [-0.34568, 0.34568], range ~[-6.694e-10, 6.696e-10]:
 * |6 / x * (1 + 2 * (1 / (exp(x) - 1) - 1 / x)) - q(x)| < 2**-30.04
 * Scaled coefficients: Qn_here = 2**n * Qn_for_q (see s_expm1.c):
 */
rit_Q1 = -3.3333212137e-2, /* -0x888868.0p-28 */
rit_Q2 =  1.5807170421e-3; /*  0xcf3010.0p-33 */

float expm1f(float x)
{
    float y,hi,lo,c,t,e,hxs,hfx,r1,twopk;
    union {float f; uint32_t i;} u = {x};
    uint32_t hx = u.i & 0x7fffffff;
    int k, sign = u.i >> 31;

    /* filter out huge and non-finite argument */
    if (hx >= 0x4195b844) {  /* if |x|>=27*ln2 */
        if (hx > 0x7f800000)  /* NaN */
            return x;
        if (sign)
            return -1;
        if (hx > 0x42b17217) { /* x > log(FLT_MAX) */
            x *= 0x1p127f;
            return x;
        }
    }

    /* argument reduction */
    if (hx > 0x3eb17218) {           /* if  |x| > 0.5 ln2 */
        if (hx < 0x3F851592) {       /* and |x| < 1.5 ln2 */
            if (!sign) {
                hi = x - rit_ln2_hi;
                lo = rit_ln2_lo;
                k =  1;
            } else {
                hi = x + rit_ln2_hi;
                lo = -rit_ln2_lo;
                k = -1;
            }
        } else {
            k  = rit_invln2*x + (sign ? -0.5f : 0.5f);
            t  = k;
            hi = x - t*rit_ln2_hi;      /* t*ln2_hi is exact here */
            lo = t*rit_ln2_lo;
        }
        x = hi-lo;
        c = (hi-x)-lo;
    } else if (hx < 0x33000000) {  /* when |x|<2**-25, return x */
        if (hx < 0x00800000)
            RIT_FORCE_EVALF(x*x);
        return x;
    } else
        k = 0;

    /* x is now in primary range */
    hfx = 0.5f*x;
    hxs = x*hfx;
    r1 = 1.0f+hxs*(rit_Q1+hxs*rit_Q2);
    t  = 3.0f - r1*hfx;
    e  = hxs*((r1-t)/(6.0f - x*t));
    if (k == 0)  /* c is 0 */
        return x - (x*e-hxs);
    e  = x*(e-c) - c;
    e -= hxs;
    /* exp(x) ~ 2^k (x_reduced - e + 1) */
    if (k == -1)
        return 0.5f*(x-e) - 0.5f;
    if (k == 1) {
        if (x < -0.25f)
            return -2.0f*(e-(x+0.5f));
        return 1.0f + 2.0f*(x-e);
    }
    u.i = (0x7f+k)<<23;  /* 2^k */
    twopk = u.f;
    if (k < 0 || k > 56) {   /* suffice to return exp(x)-1 */
        y = x - e + 1.0f;
        if (k == 128)
            y = y*2.0f*0x1p127f;
        else
            y = y*twopk;
        return y - 1.0f;
    }
    u.i = (0x7f-k)<<23;  /* 2^-k */
    if (k < 23)
        y = (x-e+(1-u.f))*twopk;
    else
        y = (x-(e+u.f)+1)*twopk;
    return y;
}

// ===================================================================== tanhf
// musl src/math/tanhf.c
float tanhf(float x)
{
    union {float f; uint32_t i;} u = {.f = x};
    uint32_t w;
    int sign;
    float t;

    /* x = |x| */
    sign = u.i >> 31;
    u.i &= 0x7fffffff;
    x = u.f;
    w = u.i;

    if (w > 0x3f0c9f54) {
        /* |x| > log(3)/2 ~= 0.5493 or nan */
        if (w > 0x41200000) {
            /* |x| > 10 */
            t = 1 + 0/x;
        } else {
            t = expm1f(2*x);
            t = 1 - 2/(t+2);
        }
    } else if (w > 0x3e82c578) {
        /* |x| > log(5/3)/2 ~= 0.2554 */
        t = expm1f(2*x);
        t = t/(t+2);
    } else if (w >= 0x00800000) {
        /* |x| >= 0x1p-126 */
        t = expm1f(-2*x);
        t = -t/(t+2);
    } else {
        /* |x| is subnormal */
        RIT_FORCE_EVALF(x*x);
        t = x;
    }
    return sign ? -t : t;
}

#pragma GCC pop_options

#endif // RIT_MATHF_H
