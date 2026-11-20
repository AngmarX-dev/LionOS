#include "ioapic.h"

int ioapic_probe_decode(uint8_t firmware_id, uint32_t id_register,
                        uint32_t version_register, uint32_t gsi_base,
                        ioapic_probe_info_t *out)
{
    uint32_t hardware_id;
    uint32_t version;
    uint32_t maximum_redirection;
    uint32_t redirection_count;
    uint64_t gsi_end;

    if (!out || firmware_id > 0x0Fu)
        return -1;

    hardware_id = (id_register >> 24) & 0x0Fu;
    version = version_register & 0xFFu;
    maximum_redirection = (version_register >> 16) & 0xFFu;

    /* Register indices 0x10..0xFF hold at most 120 redirection pairs. */
    if (hardware_id != firmware_id || version == 0u ||
        maximum_redirection >= LIONOS_IOAPIC_MAX_REDIRECTIONS)
        return -1;

    redirection_count = maximum_redirection + 1u;
    gsi_end = (uint64_t)gsi_base + redirection_count;
    if (gsi_end > 0x100000000ULL)
        return -1;

    ioapic_probe_info_t decoded = {0};
    decoded.gsi_base = gsi_base;
    decoded.gsi_end = gsi_end;
    decoded.redirection_count = redirection_count;
    decoded.id_register = id_register;
    decoded.version_register = version_register;
    decoded.firmware_id = firmware_id;
    decoded.hardware_id = (uint8_t)hardware_id;
    decoded.version = (uint8_t)version;
    *out = decoded;
    return 0;
}

int ioapic_gsi_ranges_validate(const ioapic_probe_info_t *entries,
                               uint32_t count)
{
    if (!entries || count == 0u ||
        count > LIONOS_IOAPIC_MAX_CONTROLLERS)
        return -1;

    for (uint32_t i = 0u; i < count; ++i) {
        if (entries[i].redirection_count == 0u ||
            entries[i].redirection_count > LIONOS_IOAPIC_MAX_REDIRECTIONS ||
            entries[i].gsi_end <= (uint64_t)entries[i].gsi_base ||
            entries[i].gsi_end > 0x100000000ULL ||
            entries[i].gsi_end - entries[i].gsi_base !=
                entries[i].redirection_count)
            return -1;

        for (uint32_t j = 0u; j < i; ++j) {
            if ((uint64_t)entries[i].gsi_base < entries[j].gsi_end &&
                (uint64_t)entries[j].gsi_base < entries[i].gsi_end)
                return -1;
        }
    }
    return 0;
}

int ioapic_isa_routes_build(const ioapic_probe_info_t *controllers,
                            uint32_t controller_count,
                            const acpi_madt_override_t *overrides,
                            uint32_t override_count,
                            uint32_t destination_apic_id,
                            ioapic_isa_route_t routes[LIONOS_IOAPIC_ISA_ROUTE_COUNT])
{
    static const uint8_t sources[LIONOS_IOAPIC_ISA_ROUTE_COUNT] = {0u, 1u, 12u};
    static const uint8_t vectors[LIONOS_IOAPIC_ISA_ROUTE_COUNT] = {32u, 33u, 44u};

    if (!controllers || !routes ||
        (override_count != 0u && !overrides) ||
        override_count > LIONOS_MADT_MAX_OVERRIDES ||
        destination_apic_id > 0xFFu ||
        ioapic_gsi_ranges_validate(controllers, controller_count) != 0)
        return -1;

    ioapic_isa_route_t planned[LIONOS_IOAPIC_ISA_ROUTE_COUNT] = {{0}};
    for (uint32_t route_index = 0u;
         route_index < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++route_index) {
        uint32_t gsi = sources[route_index];
        uint16_t flags = 0u; /* ISA defaults: active-high, edge-triggered. */
        uint32_t override_matches = 0u;
        uint32_t selected_controller = LIONOS_IOAPIC_MAX_CONTROLLERS;

        for (uint32_t i = 0u; i < override_count; ++i) {
            if (overrides[i].bus == 0u &&
                overrides[i].source == sources[route_index]) {
                ++override_matches;
                gsi = overrides[i].gsi;
                flags = overrides[i].flags;
            }
        }
        if (override_matches > 1u || (flags & (uint16_t)~0x0Fu) != 0u)
            return -1;

        uint16_t polarity = flags & 0x3u;
        uint16_t trigger = (flags >> 2) & 0x3u;
        if (polarity == 2u || trigger == 2u)
            return -1;

        uint32_t low_word = (uint32_t)vectors[route_index] |
                            LIONOS_IOAPIC_REDIR_MASK;
        if (polarity == 3u)
            low_word |= 1u << 13;
        if (trigger == 3u)
            low_word |= 1u << 15;

        for (uint32_t i = 0u; i < controller_count; ++i) {
            if ((uint64_t)gsi >= controllers[i].gsi_base &&
                (uint64_t)gsi < controllers[i].gsi_end) {
                selected_controller = i;
                break;
            }
        }
        if (selected_controller >= controller_count)
            return -1;

        uint32_t redirection_index =
            gsi - controllers[selected_controller].gsi_base;
        if (redirection_index >=
            controllers[selected_controller].redirection_count ||
            redirection_index >= LIONOS_IOAPIC_MAX_REDIRECTIONS)
            return -1;

        for (uint32_t previous = 0u; previous < route_index; ++previous)
            if (planned[previous].gsi == gsi)
                return -1;

        planned[route_index].gsi = gsi;
        planned[route_index].controller_index = selected_controller;
        planned[route_index].redirection_index = redirection_index;
        planned[route_index].low_word = low_word;
        planned[route_index].high_word = destination_apic_id << 24;
        planned[route_index].source_irq = sources[route_index];
        planned[route_index].vector = vectors[route_index];
    }

    for (uint32_t i = 0u; i < LIONOS_IOAPIC_ISA_ROUTE_COUNT; ++i)
        routes[i] = planned[i];
    return 0;
}
