#include <stdint.h>
#include <stdio.h>
#include "block_device.h"

#define TEST_SECTORS 4u
#define TEST_SECTOR_SIZE 512u

struct memory_disk {
    uint8_t bytes[TEST_SECTORS * TEST_SECTOR_SIZE];
    uint32_t calls;
};

static int memory_read(void *context,
                       uint64_t first_sector,
                       uint32_t sector_count,
                       void *buffer) {
    struct memory_disk *disk = (struct memory_disk *)context;
    uint64_t offset = first_sector * TEST_SECTOR_SIZE;
    uint64_t length = (uint64_t)sector_count * TEST_SECTOR_SIZE;

    if (!disk || !buffer || offset + length > sizeof(disk->bytes))
        return -1;

    for (uint32_t i = 0; i < (uint32_t)length; ++i)
        ((uint8_t *)buffer)[i] = disk->bytes[(uint32_t)offset + i];

    ++disk->calls;
    return 0;
}

static int expect(int condition, const char *name) {
    if (!condition) {
        (void)fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

int main(void) {
    static const struct block_device_ops ops = { memory_read };
    struct memory_disk disk = {{0}, 0u};
    struct block_device device = {
        &ops, &disk, TEST_SECTORS, TEST_SECTOR_SIZE
    };
    uint8_t buffer[TEST_SECTOR_SIZE * 2u];
    int failures = 0;

    for (uint32_t i = 0; i < sizeof(disk.bytes); ++i)
        disk.bytes[i] = (uint8_t)(i & 0xffu);

    failures += expect(block_device_read(&device, 1u, 2u, buffer,
                                         sizeof(buffer)) == 0,
                       "valid multi-sector read");
    failures += expect(buffer[0] == disk.bytes[TEST_SECTOR_SIZE] &&
                       buffer[sizeof(buffer) - 1u] ==
                           disk.bytes[3u * TEST_SECTOR_SIZE - 1u],
                       "read data matches backing store");
    failures += expect(disk.calls == 1u, "backend called once");

    failures += expect(block_device_read(&device, 3u, 2u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject range crossing device end");
    failures += expect(block_device_read(&device, 4u, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject first sector at device end");
    failures += expect(block_device_read(&device, 0u, 0u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject zero sector count");
    failures += expect(block_device_read(&device, 0u, 1u, 0,
                                         sizeof(buffer)) < 0,
                       "reject null buffer");
    failures += expect(block_device_read(&device, 0u, 2u, buffer,
                                         TEST_SECTOR_SIZE) < 0,
                       "reject undersized destination");
    failures += expect(block_device_read(0, 0u, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject null device");

    struct block_device malformed = device;
    malformed.sector_size = 0u;
    failures += expect(block_device_read(&malformed, 0u, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject zero sector size");
    malformed = device;
    malformed.sector_size = BLOCK_DEVICE_MAX_SECTOR_SIZE + 1u;
    failures += expect(block_device_read(&malformed, 0u, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject unsupported sector size");
    malformed = device;
    malformed.ops = 0;
    failures += expect(block_device_read(&malformed, 0u, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject missing backend operations");
    malformed = device;
    malformed.sector_count = UINT64_MAX;
    malformed.sector_size = BLOCK_DEVICE_MAX_SECTOR_SIZE;
    failures += expect(block_device_read(&malformed, 0u, UINT32_MAX, buffer,
                                         UINT32_MAX) < 0,
                       "reject byte count beyond 32-bit buffer capacity");
    failures += expect(block_device_read(&device, UINT64_MAX, 1u, buffer,
                                         sizeof(buffer)) < 0,
                       "reject maximal LBA without arithmetic wrap");

    failures += expect(disk.calls == 1u,
                       "invalid requests never reach backend");

    if (failures != 0)
        return 1;

    (void)puts("BLOCK DEVICE CONTRACT TEST: PASS");
    return 0;
}
