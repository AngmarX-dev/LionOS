#include "acpi_madt.h"

#define MADT_HEADER_SIZE 44u
#define MADT_FLAG_ENABLED 0x1u
#define MADT_FLAG_ONLINE_CAPABLE 0x2u
#define MADT_FLAG_VALID_MASK (MADT_FLAG_ENABLED | MADT_FLAG_ONLINE_CAPABLE)
#define MADT_ISO_POLARITY_MASK 0x3u
#define MADT_ISO_TRIGGER_MASK 0xCu
#define MADT_ISO_VALID_MASK 0xFu

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) |
           ((uint64_t)read_le32(p + 4u) << 32);
}

static int checksum_valid(const uint8_t *p, uint32_t length)
{
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0u;
}

static int aligned_page(uint64_t address)
{
    return address != 0u && (address & 0xFFFu) == 0u;
}

static int add_cpu(acpi_madt_info_t *info, uint32_t uid,
                   uint32_t apic_id, uint32_t flags, uint8_t x2apic)
{
    for (uint32_t i = 0u; i < info->cpu_count; ++i) {
        if (info->cpus[i].apic_id == apic_id ||
            info->cpus[i].processor_uid == uid)
            return -1;
    }

    if ((flags & MADT_FLAG_ENABLED) == 0u)
        return 0;
    if (info->cpu_count >= LIONOS_MADT_MAX_CPUS)
        return -1;

    acpi_madt_cpu_t *cpu = &info->cpus[info->cpu_count++];
    cpu->apic_id = apic_id;
    cpu->processor_uid = uid;
    cpu->flags = flags;
    cpu->x2apic = x2apic;
    return 0;
}

static int add_ioapic(acpi_madt_info_t *info, uint8_t id,
                      uint32_t address, uint32_t gsi_base)
{
    if (!aligned_page(address) ||
        info->ioapic_count >= LIONOS_MADT_MAX_IOAPICS)
        return -1;

    for (uint32_t i = 0u; i < info->ioapic_count; ++i) {
        const acpi_madt_ioapic_t *entry = &info->ioapics[i];
        if (entry->id == id || entry->address == address ||
            entry->gsi_base == gsi_base)
            return -1;
    }

    acpi_madt_ioapic_t *entry = &info->ioapics[info->ioapic_count++];
    entry->id = id;
    entry->address = address;
    entry->gsi_base = gsi_base;
    return 0;
}

static int add_override(acpi_madt_info_t *info, uint8_t bus,
                        uint8_t source, uint32_t gsi, uint16_t flags)
{
    uint16_t polarity = flags & MADT_ISO_POLARITY_MASK;
    uint16_t trigger = (flags & MADT_ISO_TRIGGER_MASK) >> 2;

    if ((flags & (uint16_t)~MADT_ISO_VALID_MASK) != 0u ||
        polarity == 2u || trigger == 2u ||
        info->override_count >= LIONOS_MADT_MAX_OVERRIDES)
        return -1;

    for (uint32_t i = 0u; i < info->override_count; ++i) {
        const acpi_madt_override_t *entry = &info->overrides[i];
        if (entry->bus == bus && entry->source == source)
            return -1;
    }

    acpi_madt_override_t *entry =
        &info->overrides[info->override_count++];
    entry->bus = bus;
    entry->source = source;
    entry->gsi = gsi;
    entry->flags = flags;
    return 0;
}

int acpi_madt_parse(const uint8_t *table, uint32_t available,
                    acpi_madt_info_t *out)
{
    acpi_madt_info_t parsed = {0};
    uint32_t length;
    uint32_t offset;

    if (!table || !out || available < MADT_HEADER_SIZE)
        return -1;
    if (table[0] != 'A' || table[1] != 'P' ||
        table[2] != 'I' || table[3] != 'C')
        return -1;

    length = read_le32(table + 4u);
    if (length < MADT_HEADER_SIZE || length > available ||
        !checksum_valid(table, length))
        return -1;

    parsed.lapic_address = read_le32(table + 36u);
    parsed.flags = read_le32(table + 40u);
    if (!aligned_page(parsed.lapic_address))
        return -1;

    offset = MADT_HEADER_SIZE;
    while (offset < length) {
        const uint8_t *entry;
        uint8_t type;
        uint8_t entry_length;
        uint32_t remaining = length - offset;

        if (remaining < 2u)
            return -1;
        entry = table + offset;
        type = entry[0];
        entry_length = entry[1];
        if (entry_length < 2u || (uint32_t)entry_length > remaining)
            return -1;

        switch (type) {
        case 0u: /* Processor Local APIC */
            if (entry_length < 8u ||
                (read_le32(entry + 4u) & ~MADT_FLAG_VALID_MASK) != 0u ||
                add_cpu(&parsed, entry[2], entry[3],
                        read_le32(entry + 4u), 0u) != 0)
                return -1;
            break;
        case 1u: /* I/O APIC */
            if (entry_length < 12u || entry[3] != 0u ||
                add_ioapic(&parsed, entry[2], read_le32(entry + 4u),
                           read_le32(entry + 8u)) != 0)
                return -1;
            break;
        case 2u: /* Interrupt Source Override */
            if (entry_length < 10u ||
                add_override(&parsed, entry[2], entry[3],
                             read_le32(entry + 4u),
                             read_le16(entry + 8u)) != 0)
                return -1;
            break;
        case 5u: /* Local APIC Address Override */
            if (entry_length < 12u || read_le32(entry + 2u) != 0u ||
                parsed.lapic_address_overridden != 0u)
                return -1;
            parsed.lapic_address = read_le64(entry + 4u);
            if (!aligned_page(parsed.lapic_address))
                return -1;
            parsed.lapic_address_overridden = 1u;
            break;
        case 9u: /* Processor Local x2APIC */
            if (entry_length < 16u || read_le16(entry + 2u) != 0u ||
                (read_le32(entry + 8u) & ~MADT_FLAG_VALID_MASK) != 0u ||
                add_cpu(&parsed, read_le32(entry + 12u),
                        read_le32(entry + 4u), read_le32(entry + 8u),
                        1u) != 0)
                return -1;
            break;
        default:
            /* Unknown entries are safely skipped using their validated length. */
            break;
        }
        offset += entry_length;
    }

    if (parsed.cpu_count == 0u)
        return -1;
    *out = parsed;
    return 0;
}
