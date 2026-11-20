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

    puts("IOAPIC read-only probe validation tests passed");
    return 0;
}
