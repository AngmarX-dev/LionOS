#include <stdint.h>
#include "ata.h"
#include "ata_block.h"
#include "block_device.h"
#include "debug.h"
#include "ext2_ro.h"
#include "ext2_vfs.h"
#include "vfs.h"

#define EXT2_VFS_PREFIX "ext2"
#define EXT2_VFS_PREFIX_LEN 4u
#define EXT2_MODE_TYPE 0xf000u
#define EXT2_MODE_REG 0x8000u
#define EXT2_MODE_DIR 0x4000u
#define EXT2_MODE_SYMLINK 0xa000u
#define EXT2_VFS_SYMLINK_MAX 8u
#define EXT2_VFS_SCRATCH_SIZE EXT2_RO_BLOCK_MAX

static struct block_device ext2_device;
static struct ext2_ro_fs ext2_filesystem;
static struct ext2_ro_inode ext2_root;
static uint8_t ext2_scratch[EXT2_VFS_SCRATCH_SIZE];
static int ext2_mounted;

int ext2_vfs_path(const char *path) {
    if (!path)
        return 0;
    for (uint32_t i = 0u; i < EXT2_VFS_PREFIX_LEN; ++i) {
        if (path[i] == 0 || path[i] != EXT2_VFS_PREFIX[i])
            return 0;
    }
    return path[EXT2_VFS_PREFIX_LEN] == 0 ||
           path[EXT2_VFS_PREFIX_LEN] == '/';
}

static int append_text(char *dst, uint32_t capacity,
                       uint32_t *length, const char *src) {
    if (!dst || !length || !src) return -1;
    while (*src) {
        if (*length + 1u >= capacity) return -1;
        dst[(*length)++] = *src++;
    }
    dst[*length] = '\0';
    return 0;
}

static int normalize_ext2_relative(char *dst, uint32_t capacity,
                                    const char *src) {
    uint32_t out = 0u;
    uint32_t pos = 0u;
    if (!dst || !src || capacity == 0u) return -1;
    dst[0] = '\0';
    while (src[pos]) {
        uint32_t start;
        uint32_t length = 0u;
        while (src[pos] == '/') ++pos;
        if (!src[pos]) break;
        start = pos;
        while (src[pos] && src[pos] != '/') {
            ++length;
            ++pos;
        }
        if (length == 1u && src[start] == '.') continue;
        if (length == 2u && src[start] == '.' && src[start + 1u] == '.') {
            while (out && dst[out - 1u] != '/') --out;
            if (out) --out;
            dst[out] = '\0';
            continue;
        }
        if (out) {
            if (out + 1u >= capacity) return -1;
            dst[out++] = '/';
        }
        if (length >= capacity - out) return -1;
        for (uint32_t i = 0u; i < length; ++i)
            dst[out++] = src[start + i];
        dst[out] = '\0';
    }
    return 0;
}

static int resolve_path(const char *path, struct ext2_ro_inode *inode,
                        uint32_t *inode_number) {
    char pending[VFS_PATH_MAX];
    uint32_t followed[EXT2_VFS_SYMLINK_MAX];
    uint32_t followed_count = 0u;

    if (!ext2_mounted || !inode || !ext2_vfs_path(path))
        return -1;
    pending[0] = '\0';
    const char *initial = path + EXT2_VFS_PREFIX_LEN;
    if (*initial == '/') ++initial;
    if (normalize_ext2_relative(pending, sizeof(pending), initial) < 0)
        return -1;

    for (;;) {
        struct ext2_ro_inode current = ext2_root;
        uint32_t current_number = 2u;
        char resolved[VFS_PATH_MAX];
        const char *cursor = pending;
        int restart = 0;
        resolved[0] = '\0';

        while (*cursor) {
            char component[EXT2_RO_NAME_MAX + 1u];
            uint32_t length = 0u;
            uint32_t next_number;
            struct ext2_ro_inode next;

            if ((current.mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR)
                return -1;
            while (cursor[length] && cursor[length] != '/') {
                if (length >= EXT2_RO_NAME_MAX) return -1;
                component[length] = cursor[length];
                ++length;
            }
            if (length == 0u) return -1;
            component[length] = '\0';
            cursor += length;
            if (*cursor == '/') ++cursor;

            if (ext2_ro_lookup(&ext2_filesystem, &current, component,
                               &next_number, ext2_scratch,
                               sizeof(ext2_scratch)) < 0 ||
                ext2_ro_read_inode(&ext2_filesystem, next_number, &next,
                                   ext2_scratch, sizeof(ext2_scratch)) < 0)
                return -1;

            if ((next.mode & EXT2_MODE_TYPE) == EXT2_MODE_SYMLINK) {
                char target[VFS_PATH_MAX];
                char combined[VFS_PATH_MAX * 3u];
                char normalized[VFS_PATH_MAX];
                uint32_t combined_length = 0u;

                if (followed_count >= EXT2_VFS_SYMLINK_MAX)
                    return -1;
                for (uint32_t i = 0u; i < followed_count; ++i)
                    if (followed[i] == next_number) return -1;
                followed[followed_count++] = next_number;

                if (ext2_ro_read_symlink(&ext2_filesystem, &next, target,
                                         sizeof(target), ext2_scratch,
                                         sizeof(ext2_scratch)) < 0)
                    return -1;
                if (target[0] != '/') {
                    if (append_text(combined, sizeof(combined),
                                    &combined_length, resolved) < 0)
                        return -1;
                    if (combined_length &&
                        append_text(combined, sizeof(combined),
                                    &combined_length, "/") < 0)
                        return -1;
                }
                if (append_text(combined, sizeof(combined),
                                &combined_length, target) < 0)
                    return -1;
                if (*cursor) {
                    if (combined_length &&
                        append_text(combined, sizeof(combined),
                                    &combined_length, "/") < 0)
                        return -1;
                    if (append_text(combined, sizeof(combined),
                                    &combined_length, cursor) < 0)
                        return -1;
                }
                if (normalize_ext2_relative(normalized, sizeof(normalized),
                                             combined) < 0)
                    return -1;
                uint32_t i = 0u;
                do {
                    pending[i] = normalized[i];
                } while (normalized[i++] != '\0');
                restart = 1;
                break;
            }

            uint32_t resolved_length = 0u;
            while (resolved[resolved_length]) ++resolved_length;
            if (resolved_length &&
                append_text(resolved, sizeof(resolved), &resolved_length,
                            "/") < 0)
                return -1;
            if (append_text(resolved, sizeof(resolved), &resolved_length,
                            component) < 0)
                return -1;
            current = next;
            current_number = next_number;
        }

        if (restart) continue;
        *inode = current;
        if (inode_number) *inode_number = current_number;
        return 0;
    }
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

    ext2_filesystem = candidate_fs;
    ext2_root = candidate_root;
    ext2_mounted = 1;
    return 0;
}

int ext2_vfs_exists(const char *path) {
    struct ext2_ro_inode inode;
    return resolve_path(path, &inode, 0) == 0 &&
           (inode.mode & EXT2_MODE_TYPE) == EXT2_MODE_REG &&
           inode.size <= UINT32_MAX;
}

int ext2_vfs_stat(const char *path, uint32_t *size, int *is_directory) {
    struct ext2_ro_inode inode;
    if (!size || !is_directory || resolve_path(path, &inode, 0) < 0)
        return -1;
    if (((inode.mode & EXT2_MODE_TYPE) != EXT2_MODE_REG &&
         (inode.mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR) ||
        inode.size > UINT32_MAX)
        return -1;
    *size = inode.size;
    *is_directory = (inode.mode & EXT2_MODE_TYPE) == EXT2_MODE_DIR;
    return 0;
}

uint32_t ext2_vfs_size(const char *path) {
    uint32_t size = 0u;
    int is_directory = 0;
    return ext2_vfs_stat(path, &size, &is_directory) == 0 ? size : 0u;
}

int ext2_vfs_read(const char *path, uint32_t offset,
                  void *buffer, uint32_t length) {
    struct ext2_ro_inode inode;
    if (resolve_path(path, &inode, 0) < 0 ||
        (inode.mode & EXT2_MODE_TYPE) != EXT2_MODE_REG)
        return -1;
    return ext2_ro_read_file(&ext2_filesystem, &inode, offset, buffer,
                             length, ext2_scratch, sizeof(ext2_scratch));
}

int ext2_vfs_readdir(const char *path, uint32_t *offset,
                     struct ext2_ro_dirent *entry) {
    struct ext2_ro_inode inode;
    if (!offset || !entry || resolve_path(path, &inode, 0) < 0 ||
        (inode.mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR)
        return -1;
    return ext2_ro_readdir(&ext2_filesystem, &inode, offset, entry,
                           ext2_scratch, sizeof(ext2_scratch));
}

int ext2_vfs_boot_test(void) {
    static const char expected[] =
        "LionOS ext2 slave read-only fixture OK\n";
    static const char nested_expected[] =
        "LionOS nested ext2 fixture OK\n";
    static const char triple_expected[] =
        "LionOS triple-indirect QEMU read OK\n";
    const uint32_t triple_offset = (12u + 256u + 65536u) * 1024u;
    char contents[sizeof(expected)];
    char nested_contents[sizeof(nested_expected)];
    char triple_contents[sizeof(triple_expected)];
    struct vfs_dirent entry;
    struct vfs_stat st;
    uint32_t cursor = 0u;
    int saw_subdir = 0;
    int saw_readme = 0;
    int saw_nested = 0;
    int dir_result;
    int fd;

    if (ext2_vfs_init() < 0) {
        debug_write("LIONOS:EXT2-VFS-MOUNT-UNAVAILABLE\n");
        return -1;
    }

    do {
        dir_result = vfs_readdir("/ext2", &cursor, &entry);
        if (dir_result > 0) {
            if (entry.inode == 4u && entry.type == 2u)
                saw_subdir = 1;
            if (entry.inode == 3u)
                saw_readme = 1;
        }
    } while (dir_result > 0);

    if (dir_result < 0 || cursor == 0u || !saw_subdir || !saw_readme ||
        vfs_stat_path("/ext2", &st) < 0 ||
        (st.flags & VFS_STAT_DIRECTORY) == 0u ||
        vfs_stat_path("/ext2/subdir", &st) < 0 ||
        (st.flags & VFS_STAT_DIRECTORY) == 0u ||
        vfs_stat_path("/ext2/subdir/nested.txt", &st) < 0 ||
        st.backend != VFS_BACKEND_EXT2_RO ||
        st.size != sizeof(nested_expected) - 1u) {
        debug_write("LIONOS:EXT2-VFS-DIRECTORY-TEST-FAIL\n");
        return -1;
    }

    cursor = 0u;
    do {
        dir_result = vfs_readdir("/ext2/subdir", &cursor, &entry);
        if (dir_result > 0 && entry.inode == 5u &&
            entry.type == 1u && entry.name[0] == 'n')
            saw_nested = 1;
    } while (dir_result > 0);
    if (dir_result < 0 || !saw_nested) {
        debug_write("LIONOS:EXT2-VFS-DIRECTORY-TEST-FAIL\n");
        return -1;
    }

    fd = vfs_open("/ext2/README.TXT", 1u);
    if (fd < 0) {
        debug_write("LIONOS:EXT2-VFS-READ-ONLY-TEST-FAIL\n");
        return -1;
    }
    int read_result = vfs_read(fd, contents, (uint32_t)sizeof(contents));
    int close_result = vfs_close(fd);
    if (read_result != (int)(sizeof(expected) - 1u) ||
        close_result < 0 ||
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

    fd = vfs_open("/ext2/subdir/nested.txt", 1u);
    if (fd < 0) {
        debug_write("LIONOS:EXT2-VFS-NESTED-READ-FAIL\n");
        return -1;
    }
    read_result = vfs_read(fd, nested_contents,
                           (uint32_t)sizeof(nested_contents));
    close_result = vfs_close(fd);
    if (read_result != (int)(sizeof(nested_expected) - 1u) ||
        close_result < 0) {
        debug_write("LIONOS:EXT2-VFS-NESTED-READ-FAIL\n");
        return -1;
    }
    for (uint32_t i = 0u; i < sizeof(nested_expected) - 1u; ++i) {
        if (nested_contents[i] != nested_expected[i]) {
            debug_write("LIONOS:EXT2-VFS-NESTED-READ-FAIL\n");
            return -1;
        }
    }

    if (vfs_stat_path("/ext2/TRIPLE.TXT", &st) < 0 ||
        st.backend != VFS_BACKEND_EXT2_RO ||
        st.size != triple_offset + sizeof(triple_expected) - 1u ||
        ext2_vfs_read("ext2/TRIPLE.TXT", triple_offset, triple_contents,
                      (uint32_t)sizeof(triple_contents)) !=
            (int)(sizeof(triple_expected) - 1u)) {
        debug_write("LIONOS:EXT2-VFS-TRIPLE-INDIRECT-TEST-FAIL\n");
        return -1;
    }
    for (uint32_t i = 0u; i < sizeof(triple_expected) - 1u; ++i) {
        if (triple_contents[i] != triple_expected[i]) {
            debug_write("LIONOS:EXT2-VFS-TRIPLE-INDIRECT-TEST-FAIL\n");
            return -1;
        }
    }

    debug_write("LIONOS:EXT2-VFS-TRIPLE-INDIRECT-TEST-OK\n");
    debug_write("LIONOS:EXT2-VFS-DIRECTORY-TEST-OK\n");
    debug_write("LIONOS:EXT2-VFS-READ-ONLY-TEST-OK\n");
    return 0;
}
