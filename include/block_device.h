#ifndef LIONOS_BLOCK_DEVICE_H
#define LIONOS_BLOCK_DEVICE_H

#include <stdint.h>

/*
 * Read-only logical block-device contract.
 *
 * A backend owns its context and must serialize hardware access when needed.
 * sector_count is the number of addressable logical sectors, not the last LBA.
 * This interface deliberately has no write operation.
 */

#define BLOCK_DEVICE_MAX_SECTOR_SIZE 4096u

struct block_device;

struct block_device_ops {
    int (*read)(void *context,
                uint64_t first_sector,
                uint32_t sector_count,
                void *buffer);
};

struct block_device {
    const struct block_device_ops *ops;
    void *context;
    uint64_t sector_count;
    uint32_t sector_size;
};

int block_device_read(const struct block_device *device,
                      uint64_t first_sector,
                      uint32_t sector_count,
                      void *buffer,
                      uint32_t buffer_capacity);

#endif
