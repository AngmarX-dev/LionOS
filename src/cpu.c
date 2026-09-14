#include <stdint.h>
#include "cpu.h"

static struct cpu_info cpus[LIONOS_MAX_CPUS];
static uint32_t cpu_hint = 1u;
static uint32_t current_cpu;

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

void cpu_init(void) {
    for (uint32_t i = 0; i < LIONOS_MAX_CPUS; ++i) {
        cpus[i].index = i;
        cpus[i].apic_id = 0xFFFFFFFFu;
        cpus[i].logical_per_package = 1u;
        cpus[i].online = 0u;
    }

    uint32_t max_leaf, ebx, ecx, edx;
    cpuid(0u, 0u, &max_leaf, &ebx, &ecx, &edx);
    (void)ebx; (void)ecx; (void)edx;

    uint32_t eax1, ebx1, ecx1, edx1;
    cpuid(1u, 0u, &eax1, &ebx1, &ecx1, &edx1);
    (void)eax1; (void)ecx1; (void)edx1;

    uint32_t logical = (ebx1 >> 16) & 0xFFu;
    if (logical == 0u) logical = 1u;
    if (logical > LIONOS_MAX_CPUS) logical = LIONOS_MAX_CPUS;

    uint32_t apic_id = (ebx1 >> 24) & 0xFFu;
    cpu_hint = logical;
    cpus[0].apic_id = apic_id;
    cpus[0].logical_per_package = logical;
    cpus[0].online = 1u;
    current_cpu = 0u;

    if (max_leaf >= 0xBu) {
        uint32_t b_eax, b_ebx, b_ecx, b_edx;
        cpuid(0xBu, 0u, &b_eax, &b_ebx, &b_ecx, &b_edx);
        (void)b_eax; (void)b_ecx; (void)b_edx;
        if (b_ebx != 0u) {
            uint32_t topology_logical = b_ebx & 0xFFFFu;
            if (topology_logical != 0u && topology_logical <= LIONOS_MAX_CPUS)
                cpu_hint = topology_logical;
        }
    }
}

uint32_t cpu_count_hint(void) { return cpu_hint; }
uint32_t cpu_current_index(void) { return current_cpu; }
const struct cpu_info *cpu_get(uint32_t index) {
    return index < LIONOS_MAX_CPUS ? &cpus[index] : 0;
}

void cpu_mark_online(uint32_t index, uint32_t apic_id) {
    if (index >= LIONOS_MAX_CPUS) return;
    cpus[index].apic_id = apic_id;
    cpus[index].logical_per_package = 1u;
    cpus[index].online = 1u;
}
