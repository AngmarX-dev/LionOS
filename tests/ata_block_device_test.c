#include <stdint.h>
#include <stdio.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"

#define TEST_SECTORS 4u
#define SECTOR_SIZE 512u

static uint8_t mock_disk[TEST_SECTORS][SECTOR_SIZE];
static uint8_t mock_slave_disk[TEST_SECTORS][SECTOR_SIZE];
static int fail_lba = -1;
static uint32_t mock_sector_count = TEST_SECTORS;
static uint32_t mock_slave_sector_count = TEST_SECTORS;

uint32_t ata_sector_count(void) {
    return mock_sector_count;
}

uint32_t ata_slave_sector_count(void) {
    return mock_slave_sector_count;
}

int ata_read_sector(uint32_t lba, void *buffer) {
    uint8_t *dst = (uint8_t *)buffer;
    if (!buffer || lba >= TEST_SECTORS || (int)lba == fail_lba)
        return -1;
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i)
        dst[i] = mock_disk[lba][i];
    return 0;
}

int ata_slave_read_sector(uint32_t lba, void *buffer) {
    uint8_t *dst = (uint8_t *)buffer;
    if (!buffer || lba >= TEST_SECTORS || (int)lba == fail_lba)
        return -1;
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i)
        dst[i] = mock_slave_disk[lba][i];
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
    struct block_device master = {0};
    struct block_device slave = {0};
    uint8_t output[2u * SECTOR_SIZE];
    int failed = 0;

    for (uint32_t sector = 0u; sector < TEST_SECTORS; ++sector) {
        for (uint32_t byte = 0u; byte < SECTOR_SIZE; ++byte) {
            mock_disk[sector][byte] = (uint8_t)(sector * 17u + byte);
            mock_slave_disk[sector][byte] = (uint8_t)(0xA0u + sector + byte);
        }
    }

    mock_sector_count = TEST_SECTORS;
    mock_slave_sector_count = TEST_SECTORS;
    failed += expect(ata_block_device_init(&master) == 0,
                     "primary-master provider initializes");
    failed += expect(ata_slave_block_device_init(&slave) == 0,
                     "primary-slave provider initializes independently");
    failed += expect(master.sector_size == SECTOR_SIZE &&
                     master.sector_count == TEST_SECTORS,
                     "master geometry is published");
    failed += expect(slave.sector_size == SECTOR_SIZE &&
                     slave.sector_count == TEST_SECTORS,
                     "slave geometry is published");
    failed += expect(block_device_read(&master, 1u, 2u, output,
                                       sizeof(output)) == 0,
                     "master multi-sector read succeeds");
    failed += expect(output[0] == mock_disk[1][0],
                     "master data comes from master backend");
    failed += expect(block_device_read(&slave, 1u, 2u, output,
                                       sizeof(output)) == 0,
                     "slave multi-sector read succeeds");
    failed += expect(output[0] == mock_slave_disk[1][0],
                     "slave data comes from slave backend");
    failed += expect(output[0] != mock_disk[1][0],
                     "master and slave data paths remain distinct");
    failed += expect(block_device_read(&slave, TEST_SECTORS - 1u, 2u,
                                       output, sizeof(output)) < 0,
                     "slave out-of-range request rejected");
    fail_lba = 2;
    failed += expect(block_device_read(&master, 1u, 2u, output,
                                       sizeof(output)) < 0,
                     "master backend error propagated");
    failed += expect(block_device_read(&slave, 1u, 2u, output,
                                       sizeof(output)) < 0,
                     "slave backend error propagated");
    fail_lba = -1;
    failed += expect(block_device_read(&master, 0u, 1u, output,
                                       SECTOR_SIZE - 1u) < 0,
                     "undersized destination rejected");
    mock_slave_sector_count = 0u;
    failed += expect(ata_slave_block_device_init(&slave) < 0,
                     "unidentified slave rejected");
    mock_sector_count = 0u;
    failed += expect(ata_block_device_init(&master) < 0,
                     "unidentified master rejected");
    failed += expect(ata_slave_block_device_init(0) < 0 &&
                     ata_block_device_init(0) < 0,
                     "null descriptors rejected");

    if (failed)
        return 1;
    puts("ATA master/slave block-device providers: PASS");
    return 0;
}
