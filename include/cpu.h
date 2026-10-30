#ifndef LIONOS_CPU_H
#define LIONOS_CPU_H

#include <stdint.h>

#define LIONOS_MAX_CPUS 16u

struct cpu_info {
    uint32_t index;
    uint32_t apic_id;
    uint32_t logical_per_package;
    volatile uint32_t online;
};

void cpu_init(uint32_t multiboot_info);
uint32_t cpu_count_hint(void);
uint32_t cpu_current_index(void);
const struct cpu_info *cpu_get(uint32_t index);
void cpu_mark_online(uint32_t index, uint32_t apic_id);

#endif
