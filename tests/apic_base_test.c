#include <stdint.h>
#include <stdio.h>
#include "apic_base.h"

int main(void)
{
    const uint64_t current = (1ULL << 8) | 0xFEE00800ULL;
    const uint64_t expected = (current &
        ~LIONOS_IA32_APIC_BASE_ADDRESS_MASK) |
        0xFEE01000ULL | LIONOS_IA32_APIC_BASE_ENABLE;
    uint64_t prepared = 0xA5A5A5A5A5A5A5A5ULL;
    uint32_t flags = 0u;

    if (lapic_base_msr_prepare(current, 0xFEE01000ULL, 36u, &prepared) != 0 ||
        prepared != expected)
        return 1;
    prepared = 0xA5A5A5A5A5A5A5A5ULL;
    if (lapic_base_msr_prepare(current, 0u, 36u, &prepared) == 0 ||
        prepared != 0xA5A5A5A5A5A5A5A5ULL)
        return 2;
    if (lapic_base_msr_prepare(current, 0xFEE01001ULL, 36u, &prepared) == 0)
        return 3;
    if (lapic_base_msr_prepare(current, 0x1000000000ULL, 36u, &prepared) == 0)
        return 4;
    if (lapic_base_msr_prepare(current, 0x1000000000ULL, 48u, &prepared) != 0)
        return 5;
    if (lapic_base_msr_prepare(current, 0x0008000000000000ULL, 57u,
                               &prepared) != 0 ||
        lapic_base_msr_prepare(current, 0x0010000000000000ULL, 57u,
                               &prepared) == 0)
        return 14;
    if (lapic_base_msr_prepare(current, 0xFEE00000ULL, 31u, &prepared) == 0 ||
        lapic_base_msr_prepare(current, 0xFEE00000ULL, 58u, &prepared) == 0)
        return 6;
    if (lapic_base_msr_prepare(current | LIONOS_IA32_APIC_BASE_X2APIC,
                               0xFEE00000ULL, 36u, &prepared) == 0)
        return 7;
    if (lapic_base_msr_prepare(current, 0xFEE00000ULL, 36u, 0) == 0)
        return 8;

    if (lapic_mmio_flags_from_pat(1u, 0x0007040600070406ULL, &flags) != 0 ||
        flags != 0x1Bu)
        return 9;
    if (lapic_mmio_flags_from_pat(1u, 0x0007040605000406ULL, &flags) != 0 ||
        flags != 0x13u)
        return 10;
    if (lapic_mmio_flags_from_pat(1u, 0x0007040600070406ULL, 0) == 0)
        return 11;
    if (lapic_mmio_flags_from_pat(1u, 0x0007040605070406ULL, &flags) == 0)
        return 12;
    if (lapic_mmio_flags_from_pat(0u, 0u, &flags) != 0 || flags != 0x13u)
        return 13;

    puts("Local APIC base and MMIO cache tests passed");
    return 0;
}
