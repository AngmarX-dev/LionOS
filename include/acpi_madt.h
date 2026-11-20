#ifndef LIONOS_ACPI_MADT_H
#define LIONOS_ACPI_MADT_H

#include <stdint.h>

#define LIONOS_MADT_MAX_CPUS 16u
#define LIONOS_MADT_MAX_IOAPICS 8u
#define LIONOS_MADT_MAX_OVERRIDES 16u

typedef struct acpi_madt_cpu {
    uint32_t apic_id;
    uint32_t processor_uid;
    uint32_t flags;
    uint8_t x2apic;
} acpi_madt_cpu_t;

typedef struct acpi_madt_ioapic {
    uint32_t address;
    uint32_t gsi_base;
    uint8_t id;
} acpi_madt_ioapic_t;

typedef struct acpi_madt_override {
    uint32_t gsi;
    uint16_t flags;
    uint8_t bus;
    uint8_t source;
} acpi_madt_override_t;

typedef struct acpi_madt_info {
    uint64_t lapic_address;
    uint32_t flags;
    uint32_t cpu_count;
    uint32_t ioapic_count;
    uint32_t override_count;
    uint8_t lapic_address_overridden;
    acpi_madt_cpu_t cpus[LIONOS_MADT_MAX_CPUS];
    acpi_madt_ioapic_t ioapics[LIONOS_MADT_MAX_IOAPICS];
    acpi_madt_override_t overrides[LIONOS_MADT_MAX_OVERRIDES];
} acpi_madt_info_t;

/* Parse and validate a complete ACPI APIC (MADT) table without unaligned loads. */
int acpi_madt_parse(const uint8_t *table, uint32_t available,
                    acpi_madt_info_t *out);

#endif
