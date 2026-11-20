#include <stdint.h>
#include "interrupt_stats.h"

static volatile uint32_t interrupt_counts[LIONOS_INTERRUPT_STATS_CPUS]
                                         [LIONOS_INTERRUPT_STATS_VECTORS];

void interrupt_stats_record(uint32_t cpu, uint32_t vector)
{
    if (cpu >= LIONOS_INTERRUPT_STATS_CPUS ||
        vector >= LIONOS_INTERRUPT_STATS_VECTORS)
        return;

    (void)__atomic_fetch_add(&interrupt_counts[cpu][vector], 1u,
                             __ATOMIC_RELAXED);
}

uint32_t interrupt_stats_count(uint32_t cpu, uint32_t vector)
{
    if (cpu >= LIONOS_INTERRUPT_STATS_CPUS ||
        vector >= LIONOS_INTERRUPT_STATS_VECTORS)
        return 0u;

    return __atomic_load_n(&interrupt_counts[cpu][vector],
                           __ATOMIC_RELAXED);
}

uint32_t interrupt_stats_total(uint32_t cpu)
{
    if (cpu >= LIONOS_INTERRUPT_STATS_CPUS)
        return 0u;

    uint32_t total = 0u;
    for (uint32_t vector = 0u;
         vector < LIONOS_INTERRUPT_STATS_VECTORS; ++vector)
        total += interrupt_stats_count(cpu, vector);

    return total;
}
