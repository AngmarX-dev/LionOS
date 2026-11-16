#include <stdint.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"

#define ATA_LOGICAL_SECTOR_SIZE 512u

static int ata_block_read(void *context,
                          uint64_t first_sector,
                          uint32_t sector_count,
                          void *buffer) {
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t capacity = ata_sector_count();

    (void)context;
    if (!destination || sector_count == 0u || capacity == 0u)
        return -1;
    if (first_sector > UINT32_MAX ||
        first_sector >= capacity ||
        (uint64_t)sector_count > (uint64_t)capacity - first_sector)
        return -1;

    for (uint32_t i = 0u; i < sector_count; ++i) {
        uint64_t lba = first_sector + (uint64_t)i;
        if (lba > UINT32_MAX ||
            ata_read_sector((uint32_t)lba,
                            destination + (i * ATA_LOGICAL_SECTOR_SIZE)) < 0)
            return -1;
    }

    return 0;
}

static const struct block_device_ops ata_block_ops = {
    .read = ata_block_read
};

int ata_block_device_init(struct block_device *device) {
    uint32_t sectors;

    if (!device)
        return -1;

    sectors = ata_sector_count();
    if (sectors == 0u)
        return -1;

    device->ops = &ata_block_ops;
    device->context = 0;
    device->sector_count = sectors;
    device->sector_size = ATA_LOGICAL_SECTOR_SIZE;
    return 0;
}
