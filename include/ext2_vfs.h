#ifndef LIONOS_EXT2_VFS_H
#define LIONOS_EXT2_VFS_H

#include <stdint.h>
#include "ext2_ro.h"

int ext2_vfs_init(void);
int ext2_vfs_path(const char *path);
int ext2_vfs_exists(const char *path);
int ext2_vfs_stat(const char *path, uint32_t *size, int *is_directory);
int ext2_vfs_lstat(const char *path, uint32_t *size,
                   int *is_directory, int *is_symlink);
int ext2_vfs_readlink(const char *path, char *buffer, uint32_t capacity);
uint32_t ext2_vfs_size(const char *path);
int ext2_vfs_read(const char *path, uint32_t offset,
                  void *buffer, uint32_t length);
int ext2_vfs_readdir(const char *path, uint32_t *offset,
                     struct ext2_ro_dirent *entry);
int ext2_vfs_boot_test(void);

#endif
