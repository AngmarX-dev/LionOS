#include <stdint.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"

#define ATA_LOGICAL_SECTOR_SIZE 512u

static int ata_block_read_common(uint64_t first_sector,
                                 uint32_t sector_count,
                                 void *buffer,
                                 uint32_t capacity,
                                 int (*read_sector_fn)(uint32_t, void *)) {
    uint8_t *destination = (uint8_t *)buffer;

    if (!destination || !read_sector_fn || sector_count == 0u ||
        capacity == 0u || first_sector > UINT32_MAX ||
        first_sector >= capacity ||
        (uint64_t)sector_count > (uint64_t)capacity - first_sector)
        return -1;

    for (uint32_t i = 0u; i < sector_count; ++i) {
        uint64_t lba = first_sector + (uint64_t)i;
        uint64_t offset = (uint64_t)i * ATA_LOGICAL_SECTOR_SIZE;
        if (lba > UINT32_MAX || offset > UINT32_MAX ||
            read_sector_fn((uint32_t)lba, destination + (uint32_t)offset) < 0)
            return -1;
    }
    return 0;
}

static int ata_master_read_adapter(void *context, uint64_t first_sector,
                                   uint32_t sector_count, void *buffer) {
    (void)context;
    return ata_block_read_common(first_sector, sector_count, buffer,
                                 ata_sector_count(), ata_read_sector);
}

static int ata_slave_read_adapter(void *context, uint64_t first_sector,
                                  uint32_t sector_count, void *buffer) {
    (void)context;
    return ata_block_read_common(first_sector, sector_count, buffer,
                                 ata_slave_sector_count(),
                                 ata_slave_read_sector);
}

static const struct block_device_ops ata_master_block_ops = {
    .read = ata_master_read_adapter
};

static const struct block_device_ops ata_slave_block_ops = {
    .read = ata_slave_read_adapter
};

int ata_block_device_init(struct block_device *device) {
    uint32_t sectors;

    if (!device)
        return -1;
    sectors = ata_sector_count();
    if (sectors == 0u)
        return -1;

    device->ops = &ata_master_block_ops;
    device->context = 0;
    device->sector_count = sectors;
    device->sector_size = ATA_LOGICAL_SECTOR_SIZE;
    return 0;
}

int ata_slave_block_device_init(struct block_device *device) {
    uint32_t sectors;

    if (!device)
        return -1;
    sectors = ata_slave_sector_count();
    if (sectors == 0u)
        return -1;

    device->ops = &ata_slave_block_ops;
    device->context = 0;
    device->sector_count = sectors;
    device->sector_size = ATA_LOGICAL_SECTOR_SIZE;
    return 0;
}
