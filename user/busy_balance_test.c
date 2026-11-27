#include <stdint.h>
#include "user_api.h"

/*
 * Keep each AP busy across the five-wave production balancing probe, but
 * finish the worker before the bounded QEMU integration run ends. The
 * workload is deliberately finite so the kernel can reap each worker after
 * the final wave; its budget is between the early-exit and non-terminating
 * stress budgets observed in the preceding CI runs.
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
