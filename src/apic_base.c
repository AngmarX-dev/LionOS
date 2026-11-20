#include "apic_base.h"

int lapic_base_msr_prepare(uint64_t current_msr, uint64_t physical_address,
                           uint32_t physical_address_bits,
                           uint64_t *prepared_msr)
{
    if (!prepared_msr || physical_address_bits < 32u ||
        physical_address_bits > 57u ||
        (current_msr & LIONOS_IA32_APIC_BASE_X2APIC) != 0u)
        return -1;

    uint64_t msr_field_mask =
        ((1ULL << physical_address_bits) - 1u) & ~0xFFFULL;
    uint64_t physical_mask =
        msr_field_mask & LIONOS_IA32_APIC_BASE_ADDRESS_MASK;
    if (physical_address == 0u ||
        (physical_address & ~physical_mask) != 0u)
        return -1;

    *prepared_msr = (current_msr &
                     ~msr_field_mask) |
                    physical_address |
                    LIONOS_IA32_APIC_BASE_ENABLE;
    return 0;
}

int lapic_mmio_flags_from_pat(uint32_t pat_supported, uint64_t pat,
                              uint32_t *paging_flags)
{
    if (!paging_flags)
        return -1;

    /* Without PAT, PCD selects the architecturally uncached mapping type. */
    if (!pat_supported) {
        *paging_flags = 0x13u; /* Present | writable | PCD. */
        return 0;
    }

    for (uint32_t index = 0u; index < 4u; ++index) {
        uint8_t memory_type = (uint8_t)(pat >> (index * 8u));
        if (memory_type == 0u) { /* PAT type 0 is UC. */
            *paging_flags = 0x3u |
                ((index & 1u) ? 0x8u : 0u) |
                ((index & 2u) ? 0x10u : 0u);
            return 0;
        }
    }
    return -1;
}
