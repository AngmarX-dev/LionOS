#include <stdint.h>
#include "user_api.h"

/*
 * Keep each AP busy for the complete five-wave production balancing probe.
 * Explicit scheduler IPIs can now preempt a partial quantum, so retain a
 * larger bounded work budget while candidates are admitted in five waves.
 * The workload remains finite so the integration test can reap it normally.
 */
#define BUSY_BALANCE_ITERATIONS 4000000000u
#define BUSY_BALANCE_YIELD_MASK 0x3FFFFu

int main(void) {
    volatile uint32_t value = 0x4C494F4Eu;
    for (uint32_t i = 0u; i < BUSY_BALANCE_ITERATIONS; ++i) {
        value = (value * 1664525u) + 1013904223u + i;
        if ((i & BUSY_BALANCE_YIELD_MASK) == 0u)
            (void)lion_yield();
    }
    (void)value;
    return 0;
}
