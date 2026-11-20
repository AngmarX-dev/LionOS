#include <stdint.h>
#include <stdio.h>
#include "ioapic.h"

static int check(int condition, int code)
{
    if (!condition) {
        printf("IOAPIC test failure: %d\n", code);
        return code;
    }
    return 0;
}

int main(void)
{
    ioapic_probe_info_t a = {0};
    ioapic_probe_info_t b = {0};
    ioapic_probe_info_t ranges[2];
    ioapic_isa_route_t routes[LIONOS_IOAPIC_ISA_ROUTE_COUNT];
    acpi_madt_override_t overrides[3] = {
        { .gsi = 2u, .flags = 0x0Fu, .bus = 0u, .source = 0u },
        { .gsi = 5u, .flags = 0x07u, .bus = 0u, .source = 1u },
        { .gsi = 28u, .flags = 0x00u, .bus = 0u, .source = 12u }
    };

    if (ioapic_probe_decode(2u, 2u << 24,
            0x20u | (23u << 16), 0u, &a) != 0)
        return check(0, 1);
    if (check(a.hardware_id == 2u && a.version == 0x20u &&
              a.redirection_count == 24u && a.gsi_base == 0u &&
              a.gsi_end == 24u, 2) != 0)
        return 2;

    if (check(ioapic_probe_decode(3u, 2u << 24,
            0x20u | (23u << 16), 0u, &b) != 0, 3) != 0)
        return 3;
    if (check(ioapic_probe_decode(2u, 2u << 24,
            23u << 16, 0u, &b) != 0, 4) != 0)
        return 4;
    if (check(ioapic_probe_decode(2u, 2u << 24,
            0x20u | (120u << 16), 0u, &b) != 0, 5) != 0)
        return 5;
    if (check(ioapic_probe_decode(2u, 2u << 24,
            0x20u | (23u << 16), UINT32_MAX, &b) != 0, 6) != 0)
        return 6;
    if (check(ioapic_probe_decode(16u, 0u, 0u, 0u, &b) != 0, 7) != 0)
        return 7;
    if (check(ioapic_probe_decode(2u, 2u << 24,
            0x20u | (23u << 16), 0u, 0) != 0, 8) != 0)
        return 8;

    if (ioapic_probe_decode(4u, 4u << 24,
            0x11u | (15u << 16), 24u, &b) != 0)
        return check(0, 9);

    ranges[0] = a;
    ranges[1] = b;
    if (check(ioapic_gsi_ranges_validate(ranges, 2u) == 0, 10) != 0)
        return 10;

    ranges[1].gsi_base = 20u;
    ranges[1].gsi_end = 36u;
    if (check(ioapic_gsi_ranges_validate(ranges, 2u) != 0, 11) != 0)
        return 11;

    ranges[1].gsi_base = 24u;
    ranges[1].gsi_end = 40u;
    ranges[1].redirection_count = 16u;
    if (check(ioapic_gsi_ranges_validate(ranges, 2u) == 0, 12) != 0)
        return 12;

    if (check(ioapic_gsi_ranges_validate(0, 1u) != 0, 13) != 0)
        return 13;
    if (check(ioapic_gsi_ranges_validate(ranges, 0u) != 0, 14) != 0)
        return 14;

    if (check(ioapic_isa_routes_build(ranges, 2u, overrides, 3u,
                                      8u, routes) == 0, 15) != 0)
        return 15;
    if (check(routes[0].gsi == 2u && routes[0].controller_index == 0u &&
              routes[0].redirection_index == 2u && routes[0].vector == 32u &&
              routes[0].low_word == (32u | (1u << 13) | (1u << 15) |
                                     LIONOS_IOAPIC_REDIR_MASK) &&
              routes[0].high_word == (8u << 24), 16) != 0)
        return 16;
    if (check(routes[1].gsi == 5u && routes[1].vector == 33u &&
              routes[1].low_word == (33u | (1u << 13) |
                                     LIONOS_IOAPIC_REDIR_MASK), 17) != 0)
        return 17;
    if (check(routes[2].gsi == 28u && routes[2].controller_index == 1u &&
              routes[2].redirection_index == 4u && routes[2].vector == 44u &&
              routes[2].low_word == (44u | LIONOS_IOAPIC_REDIR_MASK), 18) != 0)
        return 18;

    if (check(ioapic_isa_routes_build(ranges, 2u, overrides, 3u,
                                      256u, routes) != 0, 19) != 0)
        return 19;
    if (check(ioapic_isa_routes_build(ranges, 2u, overrides, 3u,
                                      8u, 0) != 0, 20) != 0)
        return 20;

    acpi_madt_override_t invalid = {
        .gsi = 2u, .flags = 0x02u, .bus = 0u, .source = 0u
    };
    if (check(ioapic_isa_routes_build(ranges, 2u, &invalid, 1u,
                                      8u, routes) != 0, 21) != 0)
        return 21;
    invalid.flags = 0x08u;
    if (check(ioapic_isa_routes_build(ranges, 2u, &invalid, 1u,
                                      8u, routes) != 0, 22) != 0)
        return 22;
    overrides[0].gsi = 5u;
    if (check(ioapic_isa_routes_build(ranges, 2u, overrides, 3u,
                                      8u, routes) != 0, 23) != 0)
        return 23;
    overrides[0].gsi = 2u;
    invalid.flags = 0x0Fu;
    invalid.gsi = 100u;
    if (check(ioapic_isa_routes_build(ranges, 2u, &invalid, 1u,
                                      8u, routes) != 0, 24) != 0)
        return 24;
    if (check(ioapic_isa_routes_build(ranges, 2u, 0, 1u,
                                      8u, routes) != 0, 25) != 0)
        return 25;

    if (check(ioapic_isa_routes_build(ranges, 2u, 0, 0u,
                                      8u, routes) == 0, 26) != 0)
        return 26;
    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i) {
        if (check(routes[i].low_word ==
                      ((uint32_t)routes[i].vector |
                       LIONOS_IOAPIC_REDIR_MASK) &&
                  routes[i].high_word == (8u << 24), 27) != 0)
            return 27;
    }
    acpi_madt_override_t non_isa = {
        .gsi = 100u, .flags = 0x0Fu, .bus = 1u, .source = 0u
    };
    if (check(ioapic_isa_routes_build(ranges, 2u, &non_isa, 1u,
                                      8u, routes) == 0 &&
              routes[0].gsi == 0u, 28) != 0)
        return 28;

    puts("IOAPIC masked ISA route planning tests passed");
    return 0;
}
