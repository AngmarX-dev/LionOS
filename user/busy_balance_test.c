#include <stdint.h>

/*
 * Bounded, non-yielding CPU workload for SMP busy-destination balancing.
 * The timer must preempt this process while it remains the AP's running task.
 */
int main(void) {
    volatile uint32_t value = 0x4C494F4Eu;
    for (uint32_t i = 0u; i < 128000000u; ++i)
        value = (value * 1664525u) + 1013904223u + i;
    (void)value;
    return 0;
}
