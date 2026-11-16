#include <stdint.h>
#include "ata.h"

#define ATA_LBA28_SECTOR_LIMIT 0x10000000u
#define ATA_IDENTIFY_LBA_BIT  (1u << 9)

int ata_identify_parse(const uint16_t words[256], uint32_t *sector_count) {
    uint32_t sectors;

    if (!words || !sector_count)
        return -1;
    if ((words[49] & ATA_IDENTIFY_LBA_BIT) == 0u)
        return -1;

    sectors = (uint32_t)words[60] | ((uint32_t)words[61] << 16);
    if (sectors == 0u)
        return -1;

    if (sectors > ATA_LBA28_SECTOR_LIMIT)
        sectors = ATA_LBA28_SECTOR_LIMIT;

    *sector_count = sectors;
    return 0;
}
