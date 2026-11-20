#include <stdint.h>
#include "ioapic.h"
#include "apic_base.h"
#include "cpu.h"
#include "paging.h"
#include "memory.h"
#include "debug.h"
#include "lapic.h"
#include "pic.h"

#define IOAPIC_REG_ID 0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_REG_REDIR_BASE 0x10u
#define IOAPIC_IOWIN_DWORD 4u
#define IOAPIC_REDIR_LOW_VERIFY_MASK 0x0001AFFFu
#define IOAPIC_REDIR_LOW_FIELDS_MASK \
    (IOAPIC_REDIR_LOW_VERIFY_MASK & ~LIONOS_IOAPIC_REDIR_MASK)
#define IOAPIC_REDIR_HIGH_DEST_MASK 0xFF000000u

static ioapic_probe_info_t controllers[LIONOS_IOAPIC_MAX_CONTROLLERS];
static ioapic_isa_route_t isa_routes[LIONOS_IOAPIC_ISA_ROUTE_COUNT];
static volatile uint32_t controller_count;
static volatile uint32_t probe_complete;
static volatile uint32_t isa_routes_active;

static uint32_t ioapic_read_register(volatile uint32_t *mmio,
                                     uint32_t register_index)
{
    /* Select an IOAPIC register through IOREGSEL before reading IOWIN. */
    mmio[0] = register_index & 0xFFu;
    __asm__ volatile("" ::: "memory");
    uint32_t value = mmio[IOAPIC_IOWIN_DWORD];
    __asm__ volatile("" ::: "memory");
    return value;
}

static void ioapic_write_register(volatile uint32_t *mmio,
                                  uint32_t register_index, uint32_t value)
{
    mmio[0] = register_index & 0xFFu;
    __asm__ volatile("" ::: "memory");
    mmio[IOAPIC_IOWIN_DWORD] = value;
    __asm__ volatile("" ::: "memory");
}

static int ioapic_program_route_masked(
    const ioapic_probe_info_t *controllers,
    uint32_t controller_count,
    const ioapic_isa_route_t *route)
{
    volatile uint32_t *mmio;
    uint32_t low_register;
    uint32_t high_register;
    uint32_t old_low;
    uint32_t read_low;
    uint32_t read_high;

    if (!controllers || !route || route->controller_index >= controller_count ||
        route->redirection_index >= LIONOS_IOAPIC_MAX_REDIRECTIONS)
        return -1;

    mmio = (volatile uint32_t *)(uintptr_t)
        controllers[route->controller_index].virtual_address;
    low_register = IOAPIC_REG_REDIR_BASE + route->redirection_index * 2u;
    high_register = low_register + 1u;
    if (high_register > 0xFFu)
        return -1;

    /* Mask the existing entry before changing its destination or vector. */
    old_low = ioapic_read_register(mmio, low_register);
    ioapic_write_register(mmio, low_register,
                          old_low | LIONOS_IOAPIC_REDIR_MASK);
    ioapic_write_register(mmio, high_register, route->high_word);
    ioapic_write_register(mmio, low_register,
                          route->low_word | LIONOS_IOAPIC_REDIR_MASK);

    read_low = ioapic_read_register(mmio, low_register);
    read_high = ioapic_read_register(mmio, high_register);
    if ((read_low & IOAPIC_REDIR_LOW_VERIFY_MASK) !=
        (route->low_word & IOAPIC_REDIR_LOW_VERIFY_MASK) ||
        (read_low & LIONOS_IOAPIC_REDIR_MASK) == 0u ||
        (read_high & IOAPIC_REDIR_HIGH_DEST_MASK) !=
        (route->high_word & IOAPIC_REDIR_HIGH_DEST_MASK))
        return -1;

    return 0;
}

static int ioapic_route_set_mask(const ioapic_isa_route_t *route,
                                 uint32_t masked)
{
    uint32_t count = __atomic_load_n(&controller_count, __ATOMIC_ACQUIRE);
    volatile uint32_t *mmio;
    uint32_t low_register;
    uint32_t high_register;
    uint32_t low_word;
    uint32_t read_low;
    uint32_t read_high;

    if (!route || route->controller_index >= count ||
        route->redirection_index >= LIONOS_IOAPIC_MAX_REDIRECTIONS)
        return -1;

    mmio = (volatile uint32_t *)(uintptr_t)
        controllers[route->controller_index].virtual_address;
    low_register = IOAPIC_REG_REDIR_BASE + route->redirection_index * 2u;
    high_register = low_register + 1u;
    if (high_register > 0xFFu)
        return -1;

    low_word = ioapic_read_register(mmio, low_register);
    if (masked)
        low_word |= LIONOS_IOAPIC_REDIR_MASK;
    else
        low_word &= ~LIONOS_IOAPIC_REDIR_MASK;
    ioapic_write_register(mmio, low_register, low_word);

    read_low = ioapic_read_register(mmio, low_register);
    read_high = ioapic_read_register(mmio, high_register);
    if ((read_low & IOAPIC_REDIR_LOW_FIELDS_MASK) !=
            (route->low_word & IOAPIC_REDIR_LOW_FIELDS_MASK) ||
        ((read_low & LIONOS_IOAPIC_REDIR_MASK) != 0u) != (masked != 0u) ||
        (read_high & IOAPIC_REDIR_HIGH_DEST_MASK) !=
            (route->high_word & IOAPIC_REDIR_HIGH_DEST_MASK))
        return -1;

    return 0;
}

static int ioapic_mask_all_isa_routes(void)
{
    int result = 0;
    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i)
        if (ioapic_route_set_mask(&isa_routes[i], 1u) != 0)
            result = -1;
    return result;
}

int ioapic_activate_isa_routes(void)
{
    uint32_t flags;
    uint32_t bsp_apic_id;

    if (__atomic_load_n(&isa_routes_active, __ATOMIC_ACQUIRE) != 0u)
        return 0;
    if (cpu_current_index() != 0u ||
        !__atomic_load_n(&probe_complete, __ATOMIC_ACQUIRE))
        return -1;

    bsp_apic_id = lapic_id();
    if (bsp_apic_id == 0xFFFFFFFFu || bsp_apic_id > 0xFFu)
        return -1;
    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i)
        if ((isa_routes[i].high_word >> 24) != bsp_apic_id)
            return -1;

    /* The BSP must not accept either PIC or IOAPIC interrupts mid-handoff. */
    __asm__ volatile("pushfl; popl %0" : "=r"(flags) : : "memory");
    if ((flags & (1u << 9)) != 0u)
        return -1;

    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i) {
        if (ioapic_route_set_mask(&isa_routes[i], 0u) != 0)
            return ioapic_mask_all_isa_routes() == 0 ? -1 : -2;
    }

    /*
     * All ISA routes target the BSP. With BSP IF clear, enabling them before
     * masking the PIC cannot run an ISR from either controller. If PIC masking
     * fails, restore the masked IOAPIC state before returning to PIC mode.
     */
    int pic_result = pic_mask_all();
    if (pic_result != 0) {
        if (ioapic_mask_all_isa_routes() != 0)
            return -2;
        return pic_result == -2 ? -2 : -1;
    }

    __atomic_store_n(&isa_routes_active, 1u, __ATOMIC_RELEASE);
    debug_write("LIONOS:IOAPIC-ISA-ROUTES-ACTIVE\n");
    return 0;
}

int ioapic_isa_routes_active(void)
{
    return __atomic_load_n(&isa_routes_active, __ATOMIC_ACQUIRE) != 0u;
}

int ioapic_init(void)
{
    acpi_madt_info_t madt;
    cpu_memory_type_state_t memory_types;
    ioapic_probe_info_t discovered[LIONOS_IOAPIC_MAX_CONTROLLERS] = {{0}};
    ioapic_isa_route_t planned_routes[LIONOS_IOAPIC_ISA_ROUTE_COUNT] = {{0}};
    uint32_t mapping_flags;
    uint32_t destination_apic_id;

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

    destination_apic_id = lapic_id();
    if (destination_apic_id == 0xFFFFFFFFu ||
        ioapic_isa_routes_build(discovered, madt.ioapic_count,
                                madt.overrides, madt.override_count,
                                destination_apic_id, planned_routes) != 0)
        return -1;

    /*
     * Prepare only masked redirection entries. PIC remains the active
     * interrupt source until a later, separately verified handoff.
     */
    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i) {
        if (ioapic_program_route_masked(discovered, madt.ioapic_count,
                                        &planned_routes[i]) != 0)
            return -1;
    }

    for (uint32_t i = 0u; i < madt.ioapic_count; ++i)
        controllers[i] = discovered[i];
    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i)
        isa_routes[i] = planned_routes[i];

    __atomic_store_n(&controller_count, madt.ioapic_count, __ATOMIC_RELEASE);
    __atomic_store_n(&probe_complete, 1u, __ATOMIC_RELEASE);

    debug_write("LIONOS:IOAPIC-ISA-ROUTES-PREPARED-MASKED\n");
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
