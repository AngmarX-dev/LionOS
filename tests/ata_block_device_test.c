#include <stdint.h>
#include <stdio.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"

#define TEST_SECTORS 4u
#define SECTOR_SIZE 512u

static uint8_t mock_disk[TEST_SECTORS][SECTOR_SIZE];
static int fail_lba = -1;
static uint32_t mock_sector_count = TEST_SECTORS;

uint32_t ata_sector_count(void) {
    return mock_sector_count;
}

int ata_read_sector(uint32_t lba, void *buffer) {
    uint8_t *dst = (uint8_t *)buffer;
    if (!buffer || lba >= TEST_SECTORS || (int)lba == fail_lba)
        return -1;
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i)
        dst[i] = mock_disk[lba][i];
    return 0;
}

static int expect(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

int main(void) {
    struct block_device device = {0};
    uint8_t output[2u * SECTOR_SIZE];
    int failed = 0;

    for (uint32_t sector = 0u; sector < TEST_SECTORS; ++sector)
        for (uint32_t byte = 0u; byte < SECTOR_SIZE; ++byte)
            mock_disk[sector][byte] = (uint8_t)(sector * 17u + byte);

    mock_sector_count = TEST_SECTORS;
    failed += expect(ata_block_device_init(&device) == 0,
                     "provider initializes after ATA identify");
    failed += expect(device.sector_size == SECTOR_SIZE &&
                     device.sector_count == TEST_SECTORS,
                     "provider publishes sector geometry");
    failed += expect(block_device_read(&device, 1u, 2u, output,
                                       sizeof(output)) == 0,
                     "multi-sector read succeeds");
    for (uint32_t i = 0u; i < sizeof(output); ++i) {
        uint8_t expected = mock_disk[1u + (i / SECTOR_SIZE)][i % SECTOR_SIZE];
        if (output[i] != expected) {
            failed += expect(0, "read data matches backing sectors");
            break;
        }
    }
    failed += expect(block_device_read(&device, TEST_SECTORS - 1u, 2u,
                                       output, sizeof(output)) < 0,
                     "out-of-range request rejected");
    fail_lba = 2;
    failed += expect(block_device_read(&device, 1u, 2u, output,
                                       sizeof(output)) < 0,
                     "backend I/O failure propagated");
    failed += expect(block_device_read(&device, 0u, 1u, output,
                                       SECTOR_SIZE - 1u) < 0,
                     "undersized destination rejected");
    mock_sector_count = 0u;
    failed += expect(ata_block_device_init(&device) < 0,
                     "unidentified device rejected");
    failed += expect(ata_block_device_init(0) < 0,
                     "null descriptor rejected");

    if (failed)
        return 1;
    puts("ATA block-device provider: PASS");
    return 0;
}
