// Check rit/aib_rand.h against glibc's srand()/rand() on x86:
//   gcc -O2 rit/benchmarks/aibench/test_rand.c -I piton/verif/diag/assembly/include/riscv/ariane
// For many seeds (including the dropout port's default time value), the first
// 1000 numbers after srand(seed) must be the same.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct { volatile uint32_t word; } aib_lock_t;
static void aib_lock(aib_lock_t *l) { (void)l; }
static void aib_unlock(aib_lock_t *l) { (void)l; }
#include "rit/aib_rand.h"

int main(void)
{
    unsigned seeds[] = { 0, 1, 2, 42, 12345, 1763856000u, 1700000000u, 2147483647u, 4000000000u, 0xffffffffu };
    long checked = 0, bad = 0;
    for (unsigned i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        srand(seeds[i]); aib_srand(seeds[i]);
        for (int n = 0; n < 1000; n++, checked++)
            if (rand() != aib_rand()) bad++;
    }
    for (unsigned s = 0; s < 100000; s++) {      // the dropout pattern: reseed, take one
        srand(s * 2654435761u); aib_srand(s * 2654435761u);
        checked++;
        if (rand() != aib_rand()) bad++;
    }
    printf("RAND_MAX %d vs %d; %ld numbers compared, %ld differ\n", RAND_MAX, AIB_RAND_MAX, checked, bad);
    return bad != 0;
}
