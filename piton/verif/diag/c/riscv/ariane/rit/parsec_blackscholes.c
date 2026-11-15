// PARSEC blackscholes (pthreads version), ported to run bare-metal on
// OpenPiton+Ariane with one thread per hart.
//
// The pricing kernel (CNDF, BlkSchlsEqEuroNoDiv) and the per-thread loop in
// bs_thread() are PARSEC 3.0's code unchanged, including its float/double mix,
// so the instruction stream and memory accesses inside the region of interest
// match the Linux build. What changes is only the scaffolding around it:
//
//   PARSEC (Linux, pthreads)                 this port (bare metal)
//   fscanf() the input file in main()        hart 0 builds the same arrays from
//                                            the embedded table (rit/bs_option_data.h),
//                                            exactly the values fscanf would read
//   __parsec_roi_begin() / _end()            noc_roi_begin() / noc_roi_end(): the
//                                            NoC trace covers only this region
//   CREATE_WITH_ARG / WAIT_FOR_END           every hart runs bs_thread(hart id);
//                                            a barrier before and after
//   write prices.txt, ERR_CHK                hart 0 checks every price against
//                                            DerivaGem's reference (PARSEC's
//                                            ERR_CHK test, 1e-4) after the ROI
//
// Run on the 2x1 model (both harts):
//   sims -sys=manycore -x_tiles=2 -y_tiles=1 -build_id=ariane_2x1 -vcs_run parsec_blackscholes.c \
//        -ariane -finish_mask 0x11 -rtl_timeout 10000000 \
//        -sim_run_args=+noc_trace -sim_run_args=+noc_trace_file=blackscholes_2x1_r1.csv
//
// Compile-time options (-gcc_args=-DNAME=VALUE, one per -gcc_args):
//   NUM_OPTIONS   number of options (default 4096 = PARSEC simsmall;
//                 4 = test, 16 = simdev, 16384 = simmedium, 65536 = simlarge)
//   NUM_RUNS      times each thread repeats its slice (PARSEC: 100; default 1)
//   NTHREADS      threads to use, at most the number of harts (default: all harts)
//   RIT_USE_LIBM  use the toolchain's libm for exp()/log() instead of rit/math.h
//   BS_DUMP       print every price after the check (for comparing with Linux)
//
// See rit/benchmarks/README.md.

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include "util.h"
#include "rit/bench.h"
#include "rit/math.h"            // exp() and log(): see that file for why
#include "rit/bs_option_data.h"

#ifndef NUM_OPTIONS
#define NUM_OPTIONS 4096
#endif
#ifndef NUM_RUNS
#define NUM_RUNS 1
#endif

//Precision to use for calculations
#define fptype float

// ---------------------------------------------------------------------------
// PARSEC globals. PARSEC mallocs these; here they're static, with the same
// layout: sptprice/strike/rate/volatility/otime back to back in one buffer
// aligned to a 64-byte line, otype in a second line-aligned buffer.
// ---------------------------------------------------------------------------
#define LINESIZE 64

static fptype buffer[5 * NUM_OPTIONS] __attribute__((aligned(LINESIZE)));
static int    buffer2[NUM_OPTIONS]    __attribute__((aligned(LINESIZE)));
static fptype prices_buf[NUM_OPTIONS] __attribute__((aligned(LINESIZE)));

fptype *prices;
int numOptions;

int    * otype;
fptype * sptprice;
fptype * strike;
fptype * rate;
fptype * volatility;
fptype * otime;
int numError = 0;
int nThreads;

// ======================= PARSEC kernel, unchanged ==========================
////////////////////////////////////////////////////////////////////////////////
// Cumulative Normal Distribution Function
// See Hull, Section 11.8, P.243-244
#define inv_sqrt_2xPI 0.39894228040143270286

fptype CNDF ( fptype InputX )
{
    int sign;

    fptype OutputX;
    fptype xInput;
    fptype xNPrimeofX;
    fptype expValues;
    fptype xK2;
    fptype xK2_2, xK2_3;
    fptype xK2_4, xK2_5;
    fptype xLocal, xLocal_1;
    fptype xLocal_2, xLocal_3;

    // Check for negative value of InputX
    if (InputX < 0.0) {
        InputX = -InputX;
        sign = 1;
    } else
        sign = 0;

    xInput = InputX;

    // Compute NPrimeX term common to both four & six decimal accuracy calcs
    expValues = exp(-0.5f * InputX * InputX);
    xNPrimeofX = expValues;
    xNPrimeofX = xNPrimeofX * inv_sqrt_2xPI;

    xK2 = 0.2316419 * xInput;
    xK2 = 1.0 + xK2;
    xK2 = 1.0 / xK2;
    xK2_2 = xK2 * xK2;
    xK2_3 = xK2_2 * xK2;
    xK2_4 = xK2_3 * xK2;
    xK2_5 = xK2_4 * xK2;

    xLocal_1 = xK2 * 0.319381530;
    xLocal_2 = xK2_2 * (-0.356563782);
    xLocal_3 = xK2_3 * 1.781477937;
    xLocal_2 = xLocal_2 + xLocal_3;
    xLocal_3 = xK2_4 * (-1.821255978);
    xLocal_2 = xLocal_2 + xLocal_3;
    xLocal_3 = xK2_5 * 1.330274429;
    xLocal_2 = xLocal_2 + xLocal_3;

    xLocal_1 = xLocal_2 + xLocal_1;
    xLocal   = xLocal_1 * xNPrimeofX;
    xLocal   = 1.0 - xLocal;

    OutputX  = xLocal;

    if (sign) {
        OutputX = 1.0 - OutputX;
    }

    return OutputX;
}

//////////////////////////////////////////////////////////////////////////////////////
fptype BlkSchlsEqEuroNoDiv( fptype sptprice,
                            fptype strike, fptype rate, fptype volatility,
                            fptype time, int otype, float timet )
{
    fptype OptionPrice;

    // local private working variables for the calculation
    fptype xStockPrice;
    fptype xStrikePrice;
    fptype xRiskFreeRate;
    fptype xVolatility;
    fptype xTime;
    fptype xSqrtTime;

    fptype logValues;
    fptype xLogTerm;
    fptype xD1;
    fptype xD2;
    fptype xPowerTerm;
    fptype xDen;
    fptype d1;
    fptype d2;
    fptype FutureValueX;
    fptype NofXd1;
    fptype NofXd2;
    fptype NegNofXd1;
    fptype NegNofXd2;

    xStockPrice = sptprice;
    xStrikePrice = strike;
    xRiskFreeRate = rate;
    xVolatility = volatility;

    xTime = time;
    xSqrtTime = sqrt(xTime);

    logValues = log( sptprice / strike );

    xLogTerm = logValues;


    xPowerTerm = xVolatility * xVolatility;
    xPowerTerm = xPowerTerm * 0.5;

    xD1 = xRiskFreeRate + xPowerTerm;
    xD1 = xD1 * xTime;
    xD1 = xD1 + xLogTerm;

    xDen = xVolatility * xSqrtTime;
    xD1 = xD1 / xDen;
    xD2 = xD1 -  xDen;

    d1 = xD1;
    d2 = xD2;

    NofXd1 = CNDF( d1 );
    NofXd2 = CNDF( d2 );

    FutureValueX = strike * ( exp( -(rate)*(time) ) );
    if (otype == 0) {
        OptionPrice = (sptprice * NofXd1) - (FutureValueX * NofXd2);
    } else {
        NegNofXd1 = (1.0 - NofXd1);
        NegNofXd2 = (1.0 - NofXd2);
        OptionPrice = (FutureValueX * NegNofXd2) - (sptprice * NegNofXd1);
    }

    return OptionPrice;
}

//////////////////////////////////////////////////////////////////////////////////////
// PARSEC's pthreads worker (ERR_CHK moved out of the timed loop, see main)
int bs_thread(void *tid_ptr) {
    int i, j;
    fptype price;
    int tid = *(int *)tid_ptr;
    int start = tid * (numOptions / nThreads);
    int end = start + (numOptions / nThreads);

    for (j=0; j<NUM_RUNS; j++) {
        for (i=start; i<end; i++) {
            /* Calling main function to calculate option value based on
             * Black & Scholes's equation.
             */
            price = BlkSchlsEqEuroNoDiv( sptprice[i], strike[i],
                                         rate[i], volatility[i], otime[i],
                                         otype[i], 0);
            prices[i] = price;
        }
    }

    return 0;
}
// ===================== end of PARSEC kernel ================================

// per-hart ROI cycle/instruction counts, one 64-byte line each
static volatile uint64_t roi_stats[RIT_MAX_HARTS][8] __attribute__((aligned(64)));
static volatile int setup_ok;

int main(int argc, char **argv)
{
    int hart  = argv[0][0];
    int nharts = argv[0][1];
    int i;

    // ---- setup (PARSEC: main thread reads the input file) ----
    if (hart == 0) {
        numOptions = NUM_OPTIONS;
#ifdef NTHREADS
        nThreads = NTHREADS;
#else
        nThreads = nharts;
#endif
        setup_ok = (nThreads >= 1 && nThreads <= nharts && nThreads <= numOptions);
        if (setup_ok) {
            printf("PARSEC blackscholes (bare-metal port)\n");
            printf("Num of Options: %d\n", numOptions);
            printf("Num of Runs: %d\n", NUM_RUNS);
            printf("Threads: %d of %d harts\n", nThreads, nharts);

            sptprice   = buffer;
            strike     = sptprice + numOptions;
            rate       = strike + numOptions;
            volatility = rate + numOptions;
            otime      = volatility + numOptions;
            otype      = buffer2;
            prices     = prices_buf;
            for (i = 0; i < numOptions; i++) {
                int k = i % BS_TABLE_ROWS;
                otype[i]      = bs_table[k].type;
                sptprice[i]   = bs_table[k].s;
                strike[i]     = bs_table[k].strike;
                rate[i]       = bs_table[k].r;
                volatility[i] = bs_table[k].v;
                otime[i]      = bs_table[k].t;
            }
        } else {
            printf("ERROR: NTHREADS=%d must be 1..%d (harts) and <= NUM_OPTIONS\n", nThreads, nharts);
        }
    }
    bench_barrier(hart, nharts);
    if (!setup_ok)
        return 1;

    // ---- region of interest ----
    if (hart == 0)
        noc_roi_begin();
    uint64_t c0 = read_mcycle(), n0 = read_minstret();
    if (hart < nThreads) {
        int tid = hart;
        bs_thread(&tid);
    }
    roi_stats[hart][0] = read_mcycle() - c0;
    roi_stats[hart][1] = read_minstret() - n0;
    bench_barrier(hart, nharts);
    if (hart == 0)
        noc_roi_end();

    // ---- check (PARSEC's ERR_CHK, outside the ROI) ----
    if (hart != 0)
        return 0;
    int checked = (numOptions / nThreads) * nThreads;   // PARSEC skips the remainder too
    double sum = 0;
    for (i = 0; i < checked; i++) {
        fptype priceDelta = bs_table[i % BS_TABLE_ROWS].dgref - prices[i];
        if (fabs(priceDelta) >= 1e-4) {
            if (numError < 5) {
                printf("Error on %d. Computed=", i);
                print_fixed(prices[i], 5);
                printf(", Ref=");
                print_fixed(bs_table[i % BS_TABLE_ROWS].dgref, 5);
                printf("\n");
            }
            numError++;
        }
        sum += prices[i];
    }
    for (i = 0; i < nharts; i++)
        printf("hart %d: ROI cycles %lu, instructions %lu\n", i,
               (unsigned long)roi_stats[i][0], (unsigned long)roi_stats[i][1]);
    printf("Sum of prices: ");
    print_fixed(sum, 4);
    printf("\n");
#ifdef BS_DUMP
    for (i = 0; i < checked; i++) {
        printf("price %d ", i);
        print_fixed(prices[i], 6);
        printf("\n");
    }
#endif
    printf("Num Errors: %d\n", numError);
    return numError ? 2 : 0;
}
