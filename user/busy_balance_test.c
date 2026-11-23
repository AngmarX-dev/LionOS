#include <stdint.h>
#include "user_api.h"

/*
 * Bounded CPU workload for SMP busy-destination balancing. Periodic voluntary
 * yields force scheduler decisions while the AP always has runnable work.
 */
int main(void) {
    volatile uint32_t value = 0x4C494F4Eu;
    for (uint32_t i = 0u; i < 200000000u; ++i) {
        value = (value * 1664525u) + 1013904223u + i;
        if ((i & 0x3FFFFu) == 0u)
            (void)lion_yield();
    }
    (void)value;
    return 0;
}
