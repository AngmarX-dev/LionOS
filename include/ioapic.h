#ifndef LIONOS_IOAPIC_H
#define LIONOS_IOAPIC_H

#include <stdint.h>
#include "acpi_madt.h"

#define LIONOS_IOAPIC_MAX_CONTROLLERS LIONOS_MADT_MAX_IOAPICS
#define LIONOS_IOAPIC_VIRT_BASE 0xFEC00000u
#define LIONOS_IOAPIC_PAGE_SIZE 0x1000u
#define LIONOS_IOAPIC_MAX_REDIRECTIONS 120u

typedef struct ioapic_probe_info {
    uint32_t physical_address;
    uint32_t virtual_address;
    uint32_t gsi_base;
    uint64_t gsi_end;
    uint32_t redirection_count;
    uint32_t id_register;
    uint32_t version_register;
    uint8_t firmware_id;
    uint8_t hardware_id;
    uint8_t version;
} ioapic_probe_info_t;

/* Pure helpers used by host tests; no MMIO is performed. */
int ioapic_probe_decode(uint8_t firmware_id, uint32_t id_register,
                        uint32_t version_register, uint32_t gsi_base,
                        ioapic_probe_info_t *out);
int ioapic_gsi_ranges_validate(const ioapic_probe_info_t *entries,
                               uint32_t count);

/* Maps and probes MADT-listed IOAPICs read-only. Does not route interrupts. */
int ioapic_init(void);
uint32_t ioapic_count(void);
int ioapic_get_info(uint32_t index, ioapic_probe_info_t *out);

#endif
