#ifndef LIONOS_EXT2_VFS_H
#define LIONOS_EXT2_VFS_H

#include <stdint.h>

int ext2_vfs_init(void);
int ext2_vfs_path(const char *path);
int ext2_vfs_exists(const char *path);
uint32_t ext2_vfs_size(const char *path);
int ext2_vfs_read(const char *path, uint32_t offset,
                  void *buffer, uint32_t length);
int ext2_vfs_boot_test(void);

#endif
