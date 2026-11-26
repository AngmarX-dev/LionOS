#include <stdint.h>
#include "user_api.h"

/*
 * Keep busy-balance candidates READY/RUNNING long enough for every active
 * destination CPU to enter the production scheduler under high-vCPU QEMU.
 * This workload is separate from the shorter idle-steal migration probe.
 */
#define BUSY_CANDIDATE_ROUNDS 64u
#define BUSY_CANDIDATE_ITERATIONS 2000000u

int main(void) {
    volatile uint32_t state = 0x4C494F4Eu;
    for (uint32_t round = 0u; round < BUSY_CANDIDATE_ROUNDS; ++round) {
        for (uint32_t i = 0u; i < BUSY_CANDIDATE_ITERATIONS; ++i)
            state = (state * 1664525u) + 1013904223u + i;
        (void)lion_yield();
    }
    (void)state;
    return 0;
}
