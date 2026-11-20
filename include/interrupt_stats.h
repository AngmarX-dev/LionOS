#ifndef LIONOS_INTERRUPT_STATS_H
#define LIONOS_INTERRUPT_STATS_H

#include <stdint.h>
#include "cpu.h"

#define LIONOS_INTERRUPT_STATS_VECTORS 256u

/* Lock-free counters: safe for interrupt context and concurrent CPUs. */
void interrupt_stats_record(uint32_t cpu, uint32_t vector);
uint32_t interrupt_stats_count(uint32_t cpu, uint32_t vector);
uint32_t interrupt_stats_total(uint32_t cpu);

#endif
