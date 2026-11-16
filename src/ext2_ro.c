#include <stdint.h>
#include "block_device.h"
#include "ext2_ro.h"

#define EXT2_MAGIC 0xef53u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT2_FEATURE_COMPAT_ALLOWED 0x0038u
#define EXT2_INODE_FLAG_INDEX 0x00001000u
#define EXT2_MODE_TYPE 0xf000u
#define EXT2_MODE_REG 0x8000u
#define EXT2_MODE_DIR 0x4000u
#define EXT2_DIRECT_BLOCKS 12u
#define EXT2_DIRENT_HEADER 8u

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out || a > UINT64_MAX - b) return -1;
    *out = a + b;
    return 0;
}

static uint64_t div_u64_u32(uint64_t value,
                             uint32_t divisor,
                             uint32_t *remainder) {
    uint64_t quotient = 0u;
    uint64_t rem = 0u;
    if (divisor == 0u) {
        if (remainder) *remainder = 0u;
        return 0u;
    }
    for (uint32_t bit = 64u; bit != 0u; --bit) {
        rem = (rem << 1) | ((value >> (bit - 1u)) & 1u);
        if (rem >= divisor) {
            rem -= divisor;
            quotient |= (uint64_t)1u << (bit - 1u);
        }
    }
    if (remainder) *remainder = (uint32_t)rem;
    return quotient;
}

static uint64_t div_ceil_u64_u32(uint64_t value, uint32_t divisor) {
    uint32_t remainder;
    uint64_t quotient = div_u64_u32(value, divisor, &remainder);
    return quotient + (remainder != 0u ? 1u : 0u);
}

static int mul_u64_u32(uint64_t a, uint32_t b, uint64_t *out) {
    uint64_t low;
    uint64_t high;
    uint64_t shifted_high;
    if (!out) return -1;
    low = (uint64_t)(uint32_t)a * b;
    high = (uint64_t)(uint32_t)(a >> 32) * b;
    if (high > UINT32_MAX) return -1;
    shifted_high = high << 32;
    if (add_u64(shifted_high, low, out) < 0) return -1;
    return 0;
}

static int read_fs_block(const struct ext2_ro_fs *fs,
                         uint32_t block,
                         void *buffer,
                         uint32_t capacity) {
    uint64_t first_sector;
    uint64_t sectors;
    if (!fs || !fs->device || !buffer || capacity < fs->block_size ||
        block >= fs->blocks_count ||
        fs->device->sector_size == 0u ||
        fs->block_size % fs->device->sector_size != 0u)
        return -1;
    if (mul_u64_u32(block, fs->block_size / fs->device->sector_size,
                &first_sector) < 0)
        return -1;
    sectors = fs->block_size / fs->device->sector_size;
    if (sectors == 0u || sectors > UINT32_MAX) return -1;
    return block_device_read(fs->device, first_sector, (uint32_t)sectors,
                             buffer, capacity);
}

static int read_group_desc(const struct ext2_ro_fs *fs,
                           uint32_t group,
                           uint8_t *scratch,
                           uint32_t scratch_size,
                           uint32_t *inode_table) {
    uint32_t per_block;
    uint32_t block;
    uint32_t index;
    uint64_t gdt_start;
    uint64_t desc_offset;
    uint64_t inode_table_blocks;
    uint64_t table_end;
    uint64_t group_start;
    uint64_t group_end;

    if (!fs || !scratch || !inode_table || group >= fs->group_count ||
        scratch_size < fs->block_size)
        return -1;
    gdt_start = (uint64_t)(fs->first_data_block + 1u);
    per_block = fs->block_size / 32u;
    if (per_block == 0u) return -1;
    if (gdt_start + group / per_block > UINT32_MAX) return -1;
    block = (uint32_t)(gdt_start + group / per_block);
    index = group % per_block;
    if (read_fs_block(fs, block, scratch, scratch_size) < 0) return -1;
    desc_offset = (uint64_t)index * 32u;
    *inode_table = le32(scratch + (uint32_t)desc_offset + 8u);
    if (*inode_table < fs->first_data_block ||
        *inode_table >= fs->blocks_count)
        return -1;
    if (mul_u64_u32(fs->inodes_per_group, fs->inode_size,
                &inode_table_blocks) < 0)
        return -1;
    if (add_u64(inode_table_blocks, fs->block_size - 1u,
                &table_end) < 0)
        return -1;
    inode_table_blocks = div_ceil_u64_u32(inode_table_blocks,
                                          fs->block_size);
    if (add_u64(*inode_table, inode_table_blocks, &table_end) < 0 ||
        table_end > fs->blocks_count ||
        mul_u64_u32(group, fs->blocks_per_group, &group_start) < 0 ||
        add_u64(group_start, fs->first_data_block, &group_start) < 0 ||
        add_u64(group_start, fs->blocks_per_group, &group_end) < 0)
        return -1;
    if (group_end > fs->blocks_count) group_end = fs->blocks_count;
    if (*inode_table < group_start || table_end > group_end)
        return -1;
    return 0;
}

int ext2_ro_mount(struct ext2_ro_fs *fs,
                  const struct block_device *device,
                  void *scratch_buffer,
                  uint32_t scratch_size) {
    uint8_t *scratch = (uint8_t *)scratch_buffer;
    uint8_t super[1024];
    struct ext2_ro_fs candidate = {0};
    uint64_t super_sector;
    uint64_t device_bytes;
    uint64_t group_blocks;
    uint64_t group_inodes;
    uint32_t log_block_size;
    uint32_t revision;
    uint32_t compat;
    uint32_t incompat;
    uint32_t ro_compat;
    uint32_t first_inode_table;

    if (!fs || !device || !scratch ||
        device->sector_size == 0u ||
        device->sector_size > 1024u ||
        (1024u % device->sector_size) != 0u ||
        scratch_size < EXT2_RO_BLOCK_MAX ||
        !device->ops || !device->ops->read)
        return -1;

    super_sector = 1024u / device->sector_size;
    if (block_device_read(device, super_sector,
                          1024u / device->sector_size,
                          super, sizeof(super)) < 0)
        return -1;
    if (le16(super + 56u) != EXT2_MAGIC) return -1;

    revision = le32(super + 76u);
    if (revision > 1u) return -1;
    log_block_size = le32(super + 24u);
    if (log_block_size > 2u) return -1;

    candidate.block_size = 1024u << log_block_size;
    candidate.blocks_count = le32(super + 4u);
    candidate.inodes_count = le32(super + 0u);
    candidate.first_data_block = le32(super + 20u);
    candidate.blocks_per_group = le32(super + 32u);
    candidate.inodes_per_group = le32(super + 40u);
    candidate.inode_size = revision == 0u ? 128u : le16(super + 88u);
    compat = le32(super + 92u);
    incompat = le32(super + 96u);
    ro_compat = le32(super + 100u);

    if (candidate.block_size < 1024u ||
        candidate.block_size > EXT2_RO_BLOCK_MAX ||
        candidate.blocks_count <= candidate.first_data_block ||
        candidate.inodes_count < 2u ||
        candidate.blocks_per_group == 0u ||
        candidate.inodes_per_group == 0u ||
        candidate.inode_size < EXT2_RO_INODE_SIZE ||
        candidate.inode_size > candidate.block_size ||
        (candidate.inode_size & 3u) != 0u ||
        candidate.block_size % candidate.inode_size != 0u ||
        candidate.block_size % device->sector_size != 0u)
        return -1;
    if (candidate.first_data_block !=
        (candidate.block_size == 1024u ? 1u : 0u))
        return -1;
    if ((compat & ~EXT2_FEATURE_COMPAT_ALLOWED) != 0u ||
        (incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u ||
        (ro_compat & ~EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) != 0u)
        return -1;

    group_blocks = div_ceil_u64_u32(
        (uint64_t)candidate.blocks_count - candidate.first_data_block,
        candidate.blocks_per_group);
    group_inodes = div_ceil_u64_u32(candidate.inodes_count,
                                    candidate.inodes_per_group);
    if (group_blocks == 0u || group_blocks != group_inodes ||
        group_blocks > UINT32_MAX)
        return -1;
    candidate.group_count = (uint32_t)group_blocks;

    if (mul_u64_u32(device->sector_count, device->sector_size,
                &device_bytes) < 0 ||
        mul_u64_u32(candidate.blocks_count, candidate.block_size,
                &candidate.filesystem_bytes) < 0 ||
        candidate.filesystem_bytes > device_bytes)
        return -1;

    candidate.device = device;
    candidate.incompat_features = incompat;
    candidate.ro_compat_features = ro_compat;

    /* Validate the first group's inode table before publishing the mount. */
    if (read_group_desc(&candidate, 0u, scratch, scratch_size,
                        &first_inode_table) < 0)
        return -1;
    *fs = candidate;
    return 0;
}

int ext2_ro_read_inode(const struct ext2_ro_fs *fs,
                       uint32_t inode_number,
                       struct ext2_ro_inode *inode,
                       void *scratch_buffer,
                       uint32_t scratch_size) {
    uint8_t *scratch = (uint8_t *)scratch_buffer;
    uint32_t group;
    uint32_t index;
    uint32_t inode_table;
    uint64_t byte_index;
    uint64_t block_offset;
    uint32_t within;
    if (!fs || !inode || !scratch || inode_number == 0u ||
        inode_number > fs->inodes_count || scratch_size < fs->block_size)
        return -1;
    group = (inode_number - 1u) / fs->inodes_per_group;
    index = (inode_number - 1u) % fs->inodes_per_group;
    if (group >= fs->group_count ||
        read_group_desc(fs, group, scratch, scratch_size, &inode_table) < 0)
        return -1;
    if (mul_u64_u32(index, fs->inode_size, &byte_index) < 0) return -1;
    block_offset = div_u64_u32(byte_index, fs->block_size, &within);
    if (block_offset > UINT32_MAX ||
        inode_table > UINT32_MAX - (uint32_t)block_offset ||
        within + EXT2_RO_INODE_SIZE > fs->block_size)
        return -1;
    if (read_fs_block(fs, inode_table + (uint32_t)block_offset,
                      scratch, scratch_size) < 0)
        return -1;
    inode->mode = le16(scratch + within);
    inode->size = le32(scratch + within + 4u);
    inode->flags = le32(scratch + within + 32u);
    for (uint32_t i = 0; i < 15u; ++i)
        inode->blocks[i] = le32(scratch + within + 40u + i * 4u);
    if ((inode->mode & EXT2_MODE_TYPE) != EXT2_MODE_REG &&
        (inode->mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR)
        return -1;
    if ((inode->flags & EXT2_INODE_FLAG_INDEX) != 0u)
        return -1;
    return 0;
}

static int map_file_block(const struct ext2_ro_fs *fs,
                          const struct ext2_ro_inode *inode,
                          uint32_t logical,
                          uint32_t *physical,
                          uint8_t *scratch,
                          uint32_t scratch_size) {
    uint32_t pointers_per_block;
    uint32_t indirect_index;
    uint32_t block;
    if (logical < EXT2_DIRECT_BLOCKS) {
        block = inode->blocks[logical];
    } else {
        logical -= EXT2_DIRECT_BLOCKS;
        pointers_per_block = fs->block_size / 4u;
        if (logical >= pointers_per_block || inode->blocks[12] == 0u)
            return -1;
        if (read_fs_block(fs, inode->blocks[12], scratch, scratch_size) < 0)
            return -1;
        indirect_index = logical;
        block = le32(scratch + indirect_index * 4u);
    }
    if (block == 0u || block >= fs->blocks_count) return -1;
    *physical = block;
    return 0;
}

int ext2_ro_read_file(const struct ext2_ro_fs *fs,
                      const struct ext2_ro_inode *inode,
                      uint32_t offset,
                      void *buffer,
                      uint32_t length,
                      void *scratch_buffer,
                      uint32_t scratch_size) {
    uint8_t *scratch = (uint8_t *)scratch_buffer;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t remaining;
    uint32_t copied = 0u;
    if (!fs || !inode || (!buffer && length != 0u) || !scratch ||
        length > INT32_MAX || scratch_size < fs->block_size ||
        (inode->mode & EXT2_MODE_TYPE) != EXT2_MODE_REG ||
        offset > inode->size)
        return -1;
    if (length == 0u || offset == inode->size) return 0;
    remaining = inode->size - offset;
    if (length > remaining) length = remaining;
    while (copied < length) {
        uint32_t file_pos = offset + copied;
        uint32_t logical = file_pos / fs->block_size;
        uint32_t within = file_pos % fs->block_size;
        uint32_t chunk = fs->block_size - within;
        uint32_t physical;
        if (chunk > length - copied) chunk = length - copied;
        if (map_file_block(fs, inode, logical, &physical,
                           scratch, scratch_size) < 0 ||
            read_fs_block(fs, physical, scratch, scratch_size) < 0)
            return -1;
        for (uint32_t i = 0; i < chunk; ++i)
            out[copied + i] = scratch[within + i];
        copied += chunk;
    }
    return (int)copied;
}

int ext2_ro_lookup(const struct ext2_ro_fs *fs,
                   const struct ext2_ro_inode *directory,
                   const char *name,
                   uint32_t *inode_number,
                   void *scratch_buffer,
                   uint32_t scratch_size) {
    uint8_t *scratch = (uint8_t *)scratch_buffer;
    uint32_t name_length = 0u;
    uint32_t offset = 0u;
    if (!fs || !directory || !name || !inode_number || !scratch ||
        scratch_size < fs->block_size ||
        (directory->mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR)
        return -1;
    while (name[name_length]) {
        if (name_length >= EXT2_RO_NAME_MAX) return -1;
        ++name_length;
    }
    if (name_length == 0u) return -1;
    while (offset < directory->size) {
        uint32_t logical = offset / fs->block_size;
        uint32_t block_offset = offset % fs->block_size;
        uint32_t physical;
        uint32_t inode;
        uint16_t record_length;
        uint16_t entry_name_length;
        uint32_t minimum;
        if (map_file_block(fs, directory, logical, &physical,
                           scratch, scratch_size) < 0 ||
            read_fs_block(fs, physical, scratch, scratch_size) < 0)
            return -1;
        if (block_offset + EXT2_DIRENT_HEADER > fs->block_size)
            return -1;
        inode = le32(scratch + block_offset);
        record_length = le16(scratch + block_offset + 4u);
        if ((fs->incompat_features & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u)
            entry_name_length = scratch[block_offset + 6u];
        else
            entry_name_length = le16(scratch + block_offset + 6u);
        minimum = (EXT2_DIRENT_HEADER + entry_name_length + 3u) & ~3u;
        if (record_length < EXT2_DIRENT_HEADER ||
            (record_length & 3u) != 0u ||
            record_length > fs->block_size - block_offset ||
            entry_name_length > EXT2_RO_NAME_MAX ||
            minimum > record_length ||
            record_length > directory->size - offset)
            return -1;
        if ((fs->incompat_features & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u &&
            scratch[block_offset + 7u] > 7u)
            return -1;
        if (inode != 0u) {
            if (inode > fs->inodes_count) return -1;
            for (uint32_t n = 0; n < entry_name_length; ++n) {
                uint8_t ch = scratch[block_offset + EXT2_DIRENT_HEADER + n];
                if (ch == 0u || ch == (uint8_t)'/') return -1;
            }
            if (entry_name_length == name_length) {
                uint32_t i;
                for (i = 0; i < name_length; ++i)
                    if (scratch[block_offset + EXT2_DIRENT_HEADER + i] !=
                        (uint8_t)name[i])
                        break;
                if (i == name_length) {
                    *inode_number = inode;
                    return 0;
                }
            }
        }
        offset += record_length;
    }
    return -1;
}

int ext2_ro_readdir(const struct ext2_ro_fs *fs,
                    const struct ext2_ro_inode *directory,
                    uint32_t *offset,
                    struct ext2_ro_dirent *entry,
                    void *scratch_buffer,
                    uint32_t scratch_size) {
    uint8_t *scratch = (uint8_t *)scratch_buffer;

    if (!fs || !directory || !offset || !entry || !scratch ||
        scratch_size < fs->block_size ||
        (directory->mode & EXT2_MODE_TYPE) != EXT2_MODE_DIR ||
        *offset > directory->size || (*offset & 3u) != 0u)
        return -1;

    while (*offset < directory->size) {
        uint32_t current = *offset;
        uint32_t logical = current / fs->block_size;
        uint32_t block_offset = current % fs->block_size;
        uint32_t physical;
        uint32_t inode;
        uint16_t record_length;
        uint16_t name_length;
        uint32_t minimum;
        uint8_t file_type = 0u;

        if (map_file_block(fs, directory, logical, &physical,
                           scratch, scratch_size) < 0 ||
            read_fs_block(fs, physical, scratch, scratch_size) < 0 ||
            block_offset > fs->block_size - EXT2_DIRENT_HEADER)
            return -1;

        inode = le32(scratch + block_offset);
        record_length = le16(scratch + block_offset + 4u);
        if ((fs->incompat_features & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u) {
            name_length = scratch[block_offset + 6u];
            file_type = scratch[block_offset + 7u];
        } else {
            name_length = le16(scratch + block_offset + 6u);
        }

        minimum = (EXT2_DIRENT_HEADER + (uint32_t)name_length + 3u) & ~3u;
        if (record_length < EXT2_DIRENT_HEADER ||
            (record_length & 3u) != 0u ||
            record_length > fs->block_size - block_offset ||
            record_length > directory->size - current ||
            name_length > EXT2_RO_NAME_MAX || minimum > record_length ||
            (file_type > 7u))
            return -1;

        if (inode != 0u) {
            if (inode > fs->inodes_count || name_length == 0u)
                return -1;
            for (uint32_t i = 0u; i < name_length; ++i) {
                uint8_t ch = scratch[block_offset + EXT2_DIRENT_HEADER + i];
                if (ch == 0u || ch == (uint8_t)'/')
                    return -1;
                entry->name[i] = (char)ch;
            }
            entry->name[name_length] = '\0';
            for (uint32_t i = (uint32_t)name_length + 1u;
                 i <= EXT2_RO_NAME_MAX; ++i)
                entry->name[i] = '\0';
            entry->inode = inode;
            entry->type = file_type;
            *offset = current + record_length;
            return 1;
        }

        *offset = current + record_length;
    }
    return 0;
}
