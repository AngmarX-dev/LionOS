#include <stdint.h>
#include "ioapic.h"
#include "apic_base.h"
#include "cpu.h"
#include "paging.h"
#include "memory.h"
#include "debug.h"

#define IOAPIC_REG_ID 0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_IOWIN_DWORD 4u

static ioapic_probe_info_t controllers[LIONOS_IOAPIC_MAX_CONTROLLERS];
static volatile uint32_t controller_count;
static volatile uint32_t probe_complete;

static uint32_t ioapic_read_register(volatile uint32_t *mmio,
                                     uint32_t register_index)
{
    /* IOREGSEL is the only write performed by this read-only probe. */
    mmio[0] = register_index & 0xFFu;
    __asm__ volatile("" ::: "memory");
    return mmio[IOAPIC_IOWIN_DWORD];
}

int ioapic_init(void)
{
    acpi_madt_info_t madt;
    cpu_memory_type_state_t memory_types;
    ioapic_probe_info_t discovered[LIONOS_IOAPIC_MAX_CONTROLLERS] = {{0}};
    uint32_t mapping_flags;

    if (__atomic_load_n(&probe_complete, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&controller_count, __ATOMIC_ACQUIRE) != 0u
            ? 0 : -1;

    cpu_acpi_madt_get_info(&madt);
    if (madt.ioapic_count == 0u ||
        madt.ioapic_count > LIONOS_IOAPIC_MAX_CONTROLLERS)
        return -1;

    cpu_memory_type_get_state(&memory_types);
    if (lapic_mmio_flags_from_pat(memory_types.pat_supported,
                                  memory_types.pat, &mapping_flags) != 0)
        return -1;

    for (uint32_t i = 0u; i < madt.ioapic_count; ++i) {
        const acpi_madt_ioapic_t *firmware = &madt.ioapics[i];
        uint64_t virtual_address =
            (uint64_t)LIONOS_IOAPIC_VIRT_BASE +
            (uint64_t)i * LIONOS_IOAPIC_PAGE_SIZE;

        if (firmware->address == 0u ||
            (firmware->address & (LIONOS_IOAPIC_PAGE_SIZE - 1u)) != 0u ||
            virtual_address > 0xFFFFF000ULL)
            return -1;

        /*
         * Refuse to touch an address that overlaps any RAM reported by the
         * boot memory map. Unknown/low ranges fail closed as well.
         */
        uint64_t physical_end =
            (uint64_t)firmware->address + LIONOS_IOAPIC_PAGE_SIZE;
        if (memory_range_contains_available_ram(firmware->address,
                                                physical_end) != 0)
            return -1;

        uint32_t virtual_page = (uint32_t)virtual_address;
        if (paging_map_kernel_range(virtual_page, firmware->address,
                                    LIONOS_IOAPIC_PAGE_SIZE,
                                    mapping_flags) != 0)
            return -1;

        volatile uint32_t *mmio =
            (volatile uint32_t *)(uintptr_t)virtual_page;
        uint32_t id_register =
            ioapic_read_register(mmio, IOAPIC_REG_ID);
        uint32_t version_register =
            ioapic_read_register(mmio, IOAPIC_REG_VERSION);

        if (ioapic_probe_decode(firmware->id, id_register,
                                version_register, firmware->gsi_base,
                                &discovered[i]) != 0)
            return -1;

        discovered[i].physical_address = firmware->address;
        discovered[i].virtual_address = virtual_page;
    }

    if (ioapic_gsi_ranges_validate(discovered, madt.ioapic_count) != 0)
        return -1;

    for (uint32_t i = 0u; i < madt.ioapic_count; ++i)
        controllers[i] = discovered[i];

    __atomic_store_n(&controller_count, madt.ioapic_count, __ATOMIC_RELEASE);
    __atomic_store_n(&probe_complete, 1u, __ATOMIC_RELEASE);

    debug_write("LIONOS:IOAPIC-READONLY-PROBE-OK\n");
    return 0;
}

uint32_t ioapic_count(void)
{
    if (!__atomic_load_n(&probe_complete, __ATOMIC_ACQUIRE))
        return 0u;
    return __atomic_load_n(&controller_count, __ATOMIC_ACQUIRE);
}

int ioapic_get_info(uint32_t index, ioapic_probe_info_t *out)
{
    uint32_t count = ioapic_count();
    if (!out || index >= count)
        return -1;
    *out = controllers[index];
    return 0;
}
