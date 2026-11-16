#include <stdint.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"
#include "debug.h"
#include "ext2_ro.h"
#include "ext2_vfs.h"
#include "vfs.h"

#define EXT2_VFS_PREFIX "ext2/"
#define EXT2_VFS_PREFIX_LEN 5u
#define EXT2_MODE_TYPE 0xf000u
#define EXT2_MODE_REG 0x8000u
#define EXT2_MODE_DIR 0x4000u
#define EXT2_VFS_SCRATCH_SIZE EXT2_RO_BLOCK_MAX

static struct block_device ext2_device;
static struct ext2_ro_fs ext2_filesystem;
static struct ext2_ro_inode ext2_root;
static uint8_t ext2_scratch[EXT2_VFS_SCRATCH_SIZE];
static int ext2_mounted;

static int path_name(const char *path, const char **name) {
    uint32_t length = 0u;
    if (!path || !name || !ext2_vfs_path(path))
        return -1;
    path += EXT2_VFS_PREFIX_LEN;
    if (!*path)
        return -1;
    while (path[length]) {
        if (path[length] == '/' || ++length > EXT2_RO_NAME_MAX)
            return -1;
    }
    *name = path;
    return 0;
}

int ext2_vfs_path(const char *path) {
    if (!path)
        return 0;
    for (uint32_t i = 0u; i < EXT2_VFS_PREFIX_LEN; ++i) {
        if (path[i] == 0 || path[i] != EXT2_VFS_PREFIX[i])
            return 0;
    }
    return 1;
}

static int resolve_file(const char *path, struct ext2_ro_inode *inode) {
    const char *name;
    uint32_t inode_number;
    if (!ext2_mounted || !inode || path_name(path, &name) < 0)
        return -1;
    if (ext2_ro_lookup(&ext2_filesystem, &ext2_root, name,
                       &inode_number, ext2_scratch,
                       sizeof(ext2_scratch)) < 0 ||
        ext2_ro_read_inode(&ext2_filesystem, inode_number, inode,
                           ext2_scratch, sizeof(ext2_scratch)) < 0 ||
        (inode->mode & EXT2_MODE_TYPE) != EXT2_MODE_REG)
        return -1;
    return 0;
}

int ext2_vfs_init(void) {
    struct ext2_ro_fs candidate_fs;
    struct ext2_ro_inode candidate_root;

    ext2_mounted = 0;
    if (ata_slave_init() < 0 ||
        ata_slave_block_device_init(&ext2_device) < 0 ||
        ext2_ro_mount(&candidate_fs, &ext2_device, ext2_scratch,
                      sizeof(ext2_scratch)) < 0 ||
        ext2_ro_read_inode(&candidate_fs, 2u, &candidate_root,
                           ext2_scratch, sizeof(ext2_scratch)) < 0 ||
        (candidate_root.mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR)
        return -1;

    /* candidate_fs.device points at this static device, never a stack object. */
    ext2_filesystem = candidate_fs;
    ext2_root = candidate_root;
    ext2_mounted = 1;
    return 0;
}

int ext2_vfs_exists(const char *path) {
    struct ext2_ro_inode inode;
    return resolve_file(path, &inode) == 0;
}

uint32_t ext2_vfs_size(const char *path) {
    struct ext2_ro_inode inode;
    return resolve_file(path, &inode) == 0 ? inode.size : 0u;
}

int ext2_vfs_read(const char *path, uint32_t offset,
                  void *buffer, uint32_t length) {
    struct ext2_ro_inode inode;
    if (resolve_file(path, &inode) < 0)
        return -1;
    return ext2_ro_read_file(&ext2_filesystem, &inode, offset, buffer,
                             length, ext2_scratch, sizeof(ext2_scratch));
}

int ext2_vfs_boot_test(void) {
    static const char expected[] =
        "LionOS ext2 slave read-only fixture OK\n";
    char contents[sizeof(expected)];
    struct vfs_stat st;
    int fd;

    if (ext2_vfs_init() < 0) {
        debug_write("LIONOS:EXT2-VFS-MOUNT-UNAVAILABLE\n");
        return -1;
    }
    fd = vfs_open("/ext2/README.TXT", 1u);
    if (fd < 0 ||
        vfs_read(fd, contents, (uint32_t)sizeof(contents)) !=
            (int)(sizeof(expected) - 1u) ||
        vfs_close(fd) < 0 ||
        vfs_stat_path("/ext2/README.TXT", &st) < 0 ||
        st.backend != VFS_BACKEND_EXT2_RO ||
        st.size != sizeof(expected) - 1u ||
        vfs_open("/ext2/README.TXT", 2u) != VFS_FD_INVALID ||
        vfs_remove("/ext2/README.TXT") == 0) {
        debug_write("LIONOS:EXT2-VFS-READ-ONLY-TEST-FAIL\n");
        return -1;
    }
    for (uint32_t i = 0u; i < sizeof(expected) - 1u; ++i) {
        if (contents[i] != expected[i]) {
            debug_write("LIONOS:EXT2-VFS-READ-ONLY-TEST-FAIL\n");
            return -1;
        }
    }
    debug_write("LIONOS:EXT2-VFS-READ-ONLY-TEST-OK\n");
    return 0;
}
