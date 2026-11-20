#ifndef LIONOS_APIC_BASE_H
#define LIONOS_APIC_BASE_H

#include <stdint.h>

#define LIONOS_IA32_APIC_BASE_ENABLE (1ULL << 11)
#define LIONOS_IA32_APIC_BASE_X2APIC (1ULL << 10)
#define LIONOS_IA32_APIC_BASE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

/* Prepare an xAPIC base-MSR value; reject x2APIC until an x2APIC driver exists. */
int lapic_base_msr_prepare(uint64_t current_msr, uint64_t physical_address,
                           uint32_t physical_address_bits,
                           uint64_t *prepared_msr);

/* Select a page-cache flag combination whose PAT entry is exactly UC. */
int lapic_mmio_flags_from_pat(uint32_t pat_supported, uint64_t pat,
                              uint32_t *paging_flags);

#endif
