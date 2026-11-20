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
