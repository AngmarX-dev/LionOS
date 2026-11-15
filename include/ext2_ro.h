#ifndef LIONOS_EXT2_RO_H
#define LIONOS_EXT2_RO_H

#include <stdint.h>
#include "block_device.h"

#define EXT2_RO_BLOCK_MAX 4096u
#define EXT2_RO_NAME_MAX 255u
#define EXT2_RO_INODE_SIZE 128u

struct ext2_ro_fs {
    const struct block_device *device;
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t group_count;
    uint32_t inode_size;
    uint32_t incompat_features;
    uint32_t ro_compat_features;
    uint64_t filesystem_bytes;
};

struct ext2_ro_inode {
    uint16_t mode;
    uint32_t size;
    uint32_t flags;
    uint32_t blocks[15];
};

int ext2_ro_mount(struct ext2_ro_fs *fs,
                  const struct block_device *device,
                  void *scratch,
                  uint32_t scratch_size);
int ext2_ro_read_inode(const struct ext2_ro_fs *fs,
                       uint32_t inode_number,
                       struct ext2_ro_inode *inode,
                       void *scratch,
                       uint32_t scratch_size);
int ext2_ro_read_file(const struct ext2_ro_fs *fs,
                      const struct ext2_ro_inode *inode,
                      uint32_t offset,
                      void *buffer,
                      uint32_t length,
                      void *scratch,
                      uint32_t scratch_size);
int ext2_ro_lookup(const struct ext2_ro_fs *fs,
                   const struct ext2_ro_inode *directory,
                   const char *name,
                   uint32_t *inode_number,
                   void *scratch,
                   uint32_t scratch_size);

#endif
