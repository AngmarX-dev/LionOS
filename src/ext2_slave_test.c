#include <stdint.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"
#include "debug.h"
#include "ext2_ro.h"

int ext2_slave_boot_test(void) {
    static struct block_device device;
    static struct ext2_ro_fs filesystem;
    static uint8_t scratch[EXT2_RO_BLOCK_MAX];
    static uint8_t contents[64];
    static const uint8_t expected[] = "LionOS ext2 slave read-only fixture OK\n";
    struct ext2_ro_inode root;
    struct ext2_ro_inode file;
    uint32_t inode_number = 0u;
    int bytes;

    if (ata_slave_init() < 0 ||
        ata_slave_block_device_init(&device) < 0) {
        debug_write("LIONOS:EXT2-SLAVE-DEVICE-UNAVAILABLE\n");
        return -1;
    }
    if (ext2_ro_mount(&filesystem, &device, scratch, sizeof(scratch)) < 0 ||
        ext2_ro_read_inode(&filesystem, 2u, &root, scratch,
                           sizeof(scratch)) < 0 ||
        ext2_ro_lookup(&filesystem, &root, "README.TXT", &inode_number,
                       scratch, sizeof(scratch)) < 0 ||
        inode_number != 3u ||
        ext2_ro_read_inode(&filesystem, inode_number, &file, scratch,
                           sizeof(scratch)) < 0 ||
        file.size != sizeof(expected) - 1u) {
        debug_write("LIONOS:EXT2-SLAVE-READ-ONLY-TEST-FAIL\n");
        return -1;
    }

    bytes = ext2_ro_read_file(&filesystem, &file, 0u, contents,
                             (uint32_t)sizeof(contents), scratch,
                             sizeof(scratch));
    if (bytes != (int)(sizeof(expected) - 1u)) {
        debug_write("LIONOS:EXT2-SLAVE-READ-ONLY-TEST-FAIL\n");
        return -1;
    }
    for (uint32_t i = 0u; i < sizeof(expected) - 1u; ++i) {
        if (contents[i] != expected[i]) {
            debug_write("LIONOS:EXT2-SLAVE-READ-ONLY-TEST-FAIL\n");
            return -1;
        }
    }

    debug_write("LIONOS:EXT2-SLAVE-READ-ONLY-TEST-OK\n");
    return 0;
}
