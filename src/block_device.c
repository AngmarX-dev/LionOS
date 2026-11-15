#include <stdint.h>
#include "block_device.h"

int block_device_read(const struct block_device *device,
                      uint64_t first_sector,
                      uint32_t sector_count,
                      void *buffer,
                      uint32_t buffer_capacity) {
    uint64_t required_bytes;

    if (!device || !device->ops || !device->ops->read || !buffer)
        return -1;

    if (device->sector_size == 0u ||
        device->sector_size > BLOCK_DEVICE_MAX_SECTOR_SIZE ||
        (device->sector_size & (device->sector_size - 1u)) != 0u ||
        device->sector_count == 0u ||
        sector_count == 0u)
        return -1;

    if (first_sector >= device->sector_count ||
        (uint64_t)sector_count > device->sector_count - first_sector)
        return -1;

    required_bytes = (uint64_t)sector_count * device->sector_size;
    if (required_bytes > UINT32_MAX ||
        (uint32_t)required_bytes > buffer_capacity)
        return -1;

    return device->ops->read(device->context,
                             first_sector,
                             sector_count,
                             buffer);
}
