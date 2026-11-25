#include <stdint.h>
#include <stdio.h>
#include "acpi_madt.h"

#define BUFFER_SIZE 256u
static uint8_t table[BUFFER_SIZE];

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4u, (uint32_t)(v >> 32));
}
static uint32_t checksum(uint8_t *p, uint32_t length)
{
    uint8_t sum = 0u;
    p[9] = 0u;
    for (uint32_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + p[i]);
    p[9] = (uint8_t)(0u - sum);
    return length;
}
static uint32_t begin_table(void)
{
    for (uint32_t i = 0u; i < BUFFER_SIZE; ++i)
        table[i] = 0u;
    table[0] = 'A'; table[1] = 'P'; table[2] = 'I'; table[3] = 'C';
    put32(table + 36u, 0xFEE00000u);
    put32(table + 40u, 1u);
    return 44u;
}
static uint32_t add_entry(uint32_t offset, uint8_t type, uint8_t length)
{
    table[offset] = type;
    table[offset + 1u] = length;
    return offset + length;
}
static int finish_parse(uint32_t length, acpi_madt_info_t *out)
{
    put32(table + 4u, length);
    checksum(table, length);
    return acpi_madt_parse(table, length, out);
}
static int expect_failure(uint32_t length)
{
    acpi_madt_info_t out;
    return finish_parse(length, &out) != 0 ? 0 : -1;
}
static uint32_t add_cpu_entry(uint32_t offset, uint8_t uid,
                                  uint8_t apic_id, uint32_t flags)
{
    offset = add_entry(offset, 0u, 8u);
    table[offset - 6u] = uid;
    table[offset - 5u] = apic_id;
    put32(table + offset - 4u, flags);
    return offset;
}

int main(void)
{
    acpi_madt_info_t info;
    uint32_t n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 1u; table[n - 5u] = 2u;
    put32(table + n - 4u, 1u);
    n = add_entry(n, 9u, 16u);
    put32(table + n - 12u, 0x102u);
    put32(table + n - 8u, 1u);
    put32(table + n - 4u, 3u);
    n = add_entry(n, 1u, 12u);
    table[n - 10u] = 4u;
    put32(table + n - 8u, 0xFEC00000u);
    put32(table + n - 4u, 0u);
    n = add_entry(n, 2u, 10u);
    table[n - 8u] = 0u; table[n - 7u] = 1u;
    put32(table + n - 6u, 1u);
    put16(table + n - 2u, 0xFu);
    n = add_entry(n, 5u, 12u);
    put64(table + n - 8u, 0xFEE01000u);
    n = add_entry(n, 0x7Fu, 4u);
    if (finish_parse(n, &info) != 0 || info.cpu_count != 2u ||
        info.ioapic_count != 1u || info.override_count != 1u ||
        info.cpus[0].apic_id != 2u || info.cpus[1].apic_id != 0x102u ||
        info.cpus[1].x2apic != 1u || info.ioapics[0].address != 0xFEC00000u ||
        info.overrides[0].gsi != 1u || info.lapic_address != 0xFEE01000u ||
        info.lapic_address_overridden != 1u)
        return 1;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    put32(table + 4u, n);
    checksum(table, n);
    table[0] = 'X';
    if (expect_failure(n) != 0) return 2;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    put32(table + 4u, n);
    checksum(table, n);
    table[n - 1u] ^= 1u;
    if (acpi_madt_parse(table, n, &info) == 0) return 3;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    if (expect_failure(n - 1u) != 0) return 4;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    table[n] = 2u; table[n + 1u] = 0u;
    if (expect_failure(n + 2u) != 0) return 5;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    n = add_entry(n, 9u, 16u);
    put32(table + n - 12u, 1u);
    put32(table + n - 8u, 1u);
    put32(table + n - 4u, 2u);
    if (expect_failure(n) != 0) return 6;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    n = add_entry(n, 1u, 12u);
    table[n - 10u] = 1u;
    put32(table + n - 8u, 0xFEC00001u);
    if (expect_failure(n) != 0) return 7;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    n = add_entry(n, 2u, 10u);
    table[n - 8u] = 0u; table[n - 7u] = 1u;
    put32(table + n - 6u, 1u);
    put16(table + n - 2u, 2u);
    if (expect_failure(n) != 0) return 8;

    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    if (finish_parse(n, 0) == 0) return 9;


    /* An oversized declared table must not cause a read beyond the buffer. */
    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    put32(table + 4u, n + 1u);
    checksum(table, n);
    if (acpi_madt_parse(table, n, &info) == 0) return 10;

    /* Duplicate IOAPIC identities, MMIO addresses, or GSI bases are rejected. */
    n = begin_table();
    n = add_entry(n, 0u, 8u);
    table[n - 6u] = 0u; table[n - 5u] = 1u;
    put32(table + n - 4u, 1u);
    n = add_entry(n, 1u, 12u);
    table[n - 10u] = 2u;
    put32(table + n - 8u, 0xFEC00000u);
    n = add_entry(n, 1u, 12u);
    table[n - 10u] = 2u;
    put32(table + n - 8u, 0xFEC01000u);
    put32(table + n - 4u, 24u);
    if (expect_failure(n) != 0) return 11;

    /* The output object is not partially overwritten on malformed input. */
    info.cpu_count = 0xA5A5A5A5u;
    n = begin_table();
    if (finish_parse(n, &info) == 0 || info.cpu_count != 0xA5A5A5A5u)
        return 12;


    /* Online-capable but disabled processors must not enter the online CPU set. */
    n = begin_table();
    n = add_cpu_entry(n, 0xEEu, 0xEEu, 2u);
    n = add_cpu_entry(n, 1u, 2u, 1u);
    if (finish_parse(n, &info) != 0 || info.cpu_count != 1u ||
        info.cpus[0].processor_uid != 1u || info.cpus[0].apic_id != 2u)
        return 13;

    /* The parser accepts the complete supported CPU capacity. */
    n = begin_table();
    for (uint32_t i = 0u; i < LIONOS_MADT_MAX_CPUS; ++i)
        n = add_cpu_entry(n, (uint8_t)i, (uint8_t)(i + 32u), 1u);
    if (finish_parse(n, &info) != 0 ||
        info.cpu_count != LIONOS_MADT_MAX_CPUS ||
        info.cpus[LIONOS_MADT_MAX_CPUS - 1u].processor_uid !=
            (LIONOS_MADT_MAX_CPUS - 1u) ||
        info.cpus[LIONOS_MADT_MAX_CPUS - 1u].apic_id !=
            (LIONOS_MADT_MAX_CPUS + 31u))
        return 14;

    /* One more enabled processor must fail without overrunning the output. */
    n = begin_table();
    for (uint32_t i = 0u; i < LIONOS_MADT_MAX_CPUS + 1u; ++i)
        n = add_cpu_entry(n, (uint8_t)i, (uint8_t)(i + 32u), 1u);
    if (expect_failure(n) != 0)
        return 15;


    /* x2APIC entries retain their full 32-bit identifiers. */
    n = begin_table();
    n = add_entry(n, 9u, 16u);
    put32(table + n - 12u, 0x87654321u);
    put32(table + n - 8u, 1u);
    put32(table + n - 4u, 0x12345678u);
    if (finish_parse(n, &info) != 0 || info.cpu_count != 1u ||
        info.cpus[0].x2apic != 1u ||
        info.cpus[0].apic_id != 0x87654321u ||
        info.cpus[0].processor_uid != 0x12345678u)
        return 16;

    /* Duplicate processor UIDs are rejected across legacy and x2APIC entries. */
    n = begin_table();
    n = add_cpu_entry(n, 7u, 2u, 1u);
    n = add_entry(n, 9u, 16u);
    put32(table + n - 12u, 0x12345678u);
    put32(table + n - 8u, 1u);
    put32(table + n - 4u, 7u);
    if (expect_failure(n) != 0)
        return 17;

    puts("ACPI MADT parser tests passed");
    return 0;
}
