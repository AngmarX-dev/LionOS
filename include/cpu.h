#ifndef LIONOS_CPU_H
#define LIONOS_CPU_H

#include <stdint.h>

#define LIONOS_MAX_CPUS 16u

#define CPU_FEATURE_SSE   (1u<<0)
#define CPU_FEATURE_SSE2  (1u<<1)
#define CPU_FEATURE_SSE3  (1u<<2)
#define CPU_FEATURE_SSSE3 (1u<<3)
#define CPU_FEATURE_SSE41 (1u<<4)
#define CPU_FEATURE_SSE42 (1u<<5)
#define CPU_FEATURE_AVX   (1u<<6)
#define CPU_FEATURE_APIC  (1u<<7)
#define CPU_FEATURE_NX    (1u<<8)


#define CPU_MTRR_MAX_VARIABLE_RANGES 16u
#define CPU_MTRR_FIXED_REGISTER_COUNT 11u

typedef struct cpu_mtrr_variable_range {
    uint64_t base;
    uint64_t mask;
} cpu_mtrr_variable_range_t;

/* Read-only snapshot captured on the BSP during cpu_init(). */
typedef struct cpu_mtrr_info {
    uint64_t capability;
    uint64_t default_type;
    uint64_t fixed[CPU_MTRR_FIXED_REGISTER_COUNT];
    cpu_mtrr_variable_range_t variable[CPU_MTRR_MAX_VARIABLE_RANGES];
    uint32_t variable_count;
    uint32_t captured_variable_count;
    uint8_t supported;
    uint8_t msr_supported;
    uint8_t enabled;
    uint8_t fixed_supported;
    uint8_t fixed_enabled;
    uint8_t write_combining_supported;
    uint8_t truncated;
    uint8_t physical_address_bits;
} cpu_mtrr_info_t;

typedef struct cpu_identification {
    char vendor[13];
    char brand[49];
    uint32_t signature;
    uint32_t max_basic_leaf;
    uint32_t max_extended_leaf;
    uint32_t feature_flags;
    uint32_t l1_data_kb;
    uint32_t l1_instruction_kb;
    uint32_t l2_kb;
    uint32_t l3_kb;
    uint16_t base_mhz;
    uint16_t max_mhz;
    uint16_t bus_mhz;
    uint16_t family;
    uint16_t model;
    uint8_t stepping;
    uint8_t physical_cores; /* 0 when a reliable physical-core count is unavailable. */
    uint8_t logical_processors;
    uint8_t apic_id;
    uint8_t x86_64;
} cpu_identification_t;

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
uint32_t cpu_feature_flags(void);
void cpu_get_identification(cpu_identification_t *out);
void cpu_mtrr_get_info(cpu_mtrr_info_t *out);
int cpu_fpu_sse_init(void);
int cpu_enable_nx(void);
uint32_t cpu_nx_enabled(void);

#endif
