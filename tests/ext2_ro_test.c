#include <stdint.h>
#include <stdio.h>
#include "block_device.h"
#include "ext2_ro.h"

#define IMAGE_BLOCKS 32u
#define MAX_BLOCK_SIZE 4096u
#define IMAGE_BYTES (IMAGE_BLOCKS * MAX_BLOCK_SIZE)
#define SECTOR_SIZE 512u

struct disk_image {
    uint8_t bytes[IMAGE_BYTES];
    uint32_t sector_count;
    int fail_reads;
};

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int image_read(void *ctx, uint64_t first, uint32_t count, void *buffer) {
    struct disk_image *im = (struct disk_image *)ctx;
    uint64_t offset, bytes;
    if (!im || !buffer || im->fail_reads || first > im->sector_count ||
        count > im->sector_count - first) return -1;
    offset = first * SECTOR_SIZE;
    bytes = (uint64_t)count * SECTOR_SIZE;
    if (offset > IMAGE_BYTES || bytes > IMAGE_BYTES - offset) return -1;
    for (uint32_t i = 0; i < (uint32_t)bytes; ++i)
        ((uint8_t *)buffer)[i] = im->bytes[(uint32_t)offset + i];
    return 0;
}
static void make_image(struct disk_image *im, uint32_t bs) {
    uint32_t first = bs == 1024u ? 1u : 0u;
    uint32_t gdt = first + 1u;
    uint8_t *sb, *gd, *it, *root, *data, *indirect, *indirect_data;
    for (uint32_t i = 0; i < IMAGE_BYTES; ++i) im->bytes[i] = 0;
    im->sector_count = IMAGE_BLOCKS * (bs / SECTOR_SIZE);
    im->fail_reads = 0;
    sb = im->bytes + 1024u;
    gd = im->bytes + (uint64_t)gdt * bs;
    it = im->bytes + 5u * bs;
    root = im->bytes + 6u * bs;
    data = im->bytes + 7u * bs;
    indirect = im->bytes + 8u * bs;
    indirect_data = im->bytes + 9u * bs;

    put32(sb + 0u, 8u); put32(sb + 4u, IMAGE_BLOCKS);
    put32(sb + 20u, first);
    put32(sb + 24u, bs == 1024u ? 0u : (bs == 2048u ? 1u : 2u));
    put32(sb + 32u, IMAGE_BLOCKS - first); put32(sb + 40u, 8u);
    put32(sb + 76u, 1u); put16(sb + 88u, 128u);
    put32(sb + 96u, 2u); put32(sb + 100u, 1u);
    put16(sb + 56u, 0xef53u); put32(gd + 8u, 5u);

    put16(it + 128u, 0x41edu); put32(it + 132u, bs);
    put32(it + 168u, 6u);
    put16(it + 256u, 0x81a4u); put32(it + 260u, 5u);
    put32(it + 296u, 7u);
    put16(it + 384u, 0x81a4u); put32(it + 388u, 12u * bs + 3u);
    put32(it + 468u, 10u); put32(it + 472u, 8u);

    put32(root, 2u); put16(root + 4u, 12u); root[6] = 1u; root[8] = '.';
    put32(root + 12u, 2u); put16(root + 16u, 12u); root[18] = 2u;
    root[20] = '.'; root[21] = '.';
    put32(root + 24u, 3u); put16(root + 28u, 16u); root[30] = 5u;
    root[32] = 'h'; root[33] = 'e'; root[34] = 'l'; root[35] = 'l'; root[36] = 'o';
    put16(root + 44u, (uint16_t)(bs - 40u));
    data[0] = 'h'; data[1] = 'e'; data[2] = 'l'; data[3] = 'l'; data[4] = 'o';
    im->bytes[10u * bs + bs - 1u] = 'D';
    put32(indirect, 9u);
    indirect_data[0] = 'I'; indirect_data[1] = 'N'; indirect_data[2] = 'D';
}
static int expect(int ok, const char *label) {
    if (!ok) { (void)fprintf(stderr, "FAIL: %s\n", label); return 1; }
    return 0;
}
static int triple_indirect_geometry(uint32_t bs) {
    static const struct block_device_ops ops = { image_read };
    struct disk_image im;
    struct block_device dev;
    struct ext2_ro_fs fs;
    struct ext2_ro_inode file;
    uint8_t scratch[MAX_BLOCK_SIZE];
    uint8_t out[3];
    uint32_t pointers_per_block = bs / 4u;
    uint64_t double_capacity =
        (uint64_t)pointers_per_block * pointers_per_block;
    uint64_t triple_start =
        12u + pointers_per_block + double_capacity;
    uint64_t logical_block = triple_start + double_capacity +
        (uint64_t)2u * pointers_per_block + 3u;
    uint64_t offset;

    make_image(&im, bs);
    if (bs == 4096u) {
        /* Enable ext2 LARGE_FILE and encode the high inode-size word. */
        put32(im.bytes + 1024u + 100u, 3u);
        put32(im.bytes + 5u * bs + 384u + 4u,
              (uint32_t)(logical_block * bs + 3u));
        put32(im.bytes + 5u * bs + 384u + 108u,
              (uint32_t)((logical_block * bs + 3u) >> 32));
    }
    dev.ops = &ops;
    dev.context = &im;
    dev.sector_size = SECTOR_SIZE;
    dev.sector_count = im.sector_count;
    if (ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0 ||
        ext2_ro_read_inode(&fs, 4u, &file, scratch, sizeof(scratch)) < 0)
        return 1;
    if (bs == 4096u && file.size != logical_block * bs + 3u)
        return 1; /* Verify high inode-size word decoding, not just reads. */

    /* inode[14] -> root(10) -> middle(11) -> leaf(12) -> data(14). */
    file.blocks[14] = 10u;
    file.size = logical_block * bs + 3u;
    put32(im.bytes + (uint64_t)10u * bs + 4u, 11u);
    put32(im.bytes + (uint64_t)11u * bs + 8u, 12u);
    put32(im.bytes + (uint64_t)12u * bs + 12u, 14u);
    im.bytes[(uint64_t)14u * bs] = 'T';
    im.bytes[(uint64_t)14u * bs + 1u] = 'R';
    im.bytes[(uint64_t)14u * bs + 2u] = 'I';

    offset = logical_block * bs;
    if (ext2_ro_read_file(&fs, &file, offset, out, sizeof(out),
                          scratch, sizeof(scratch)) != 3 ||
        out[0] != 'T' || out[1] != 'R' || out[2] != 'I')
        return 1;

    /* Reject an invalid inode-level triple-indirect root. */
    file.blocks[14] = IMAGE_BLOCKS;
    if (ext2_ro_read_file(&fs, &file, offset, out, 1u,
                          scratch, sizeof(scratch)) >= 0)
        return 1;
    file.blocks[14] = 10u;

    /* Reject an invalid root-to-middle pointer. */
    put32(im.bytes + (uint64_t)10u * bs + 4u, IMAGE_BLOCKS);
    if (ext2_ro_read_file(&fs, &file, offset, out, 1u,
                          scratch, sizeof(scratch)) >= 0)
        return 1;

    /* Restore root, then reject an invalid middle-to-leaf pointer. */
    put32(im.bytes + (uint64_t)10u * bs + 4u, 11u);
    put32(im.bytes + (uint64_t)11u * bs + 8u, IMAGE_BLOCKS);
    if (ext2_ro_read_file(&fs, &file, offset, out, 1u,
                          scratch, sizeof(scratch)) >= 0)
        return 1;

    /* Restore middle table, then reject an invalid final data pointer. */
    put32(im.bytes + (uint64_t)11u * bs + 8u, 12u);
    put32(im.bytes + (uint64_t)12u * bs + 12u, IMAGE_BLOCKS);
    if (ext2_ro_read_file(&fs, &file, offset, out, 1u,
                          scratch, sizeof(scratch)) >= 0)
        return 1;

    return 0;
}

static int valid_geometry(uint32_t bs) {
    static const struct block_device_ops ops = { image_read };
    struct disk_image im;
    struct block_device dev;
    struct ext2_ro_fs fs;
    struct ext2_ro_inode root, file, boundary;
    struct ext2_ro_dirent dirent;
    uint8_t scratch[EXT2_RO_BLOCK_MAX], out[8] = {0};
    uint32_t found = 0u;
    uint32_t dir_offset = 0u;
    int saw_dot = 0, saw_dotdot = 0, saw_hello = 0;
    int dir_result;
    int fails = 0;
    make_image(&im, bs);
    dev.ops = &ops; dev.context = &im;
    dev.sector_count = im.sector_count; dev.sector_size = SECTOR_SIZE;
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) == 0,
                    "mount valid 1/2/4 KiB geometry");
    if (fails) return fails;
    fails += expect(fs.block_size == bs, "retain block size");
    fails += expect(ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch)) == 0,
                    "read root inode");
    do {
        dir_result = ext2_ro_readdir(&fs, &root, &dir_offset, &dirent,
                                     scratch, sizeof(scratch));
        if (dir_result > 0) {
            if (dirent.inode == 2u && dirent.name[0] == '.' &&
                dirent.name[1] == '\0')
                saw_dot = 1;
            if (dirent.inode == 2u && dirent.name[0] == '.' &&
                dirent.name[1] == '.' && dirent.name[2] == '\0')
                saw_dotdot = 1;
            if (dirent.inode == 3u && dirent.name[0] == 'h' &&
                dirent.name[1] == 'e' && dirent.name[2] == 'l' &&
                dirent.name[3] == 'l' && dirent.name[4] == 'o' &&
                dirent.name[5] == '\0')
                saw_hello = 1;
        }
    } while (dir_result > 0);
    fails += expect(dir_result == 0 && saw_dot && saw_dotdot && saw_hello,
                    "enumerate validated directory entries through EOF");
    fails += expect(ext2_ro_readdir(&fs, &root, &dir_offset, &dirent,
                                    scratch, sizeof(scratch)) == 0,
                    "directory enumeration remains at EOF");
    dir_offset = 1u;
    fails += expect(ext2_ro_readdir(&fs, &root, &dir_offset, &dirent,
                                    scratch, sizeof(scratch)) < 0,
                    "reject unaligned directory cursor");
    fails += expect(ext2_ro_lookup(&fs, &root, "hello", &found, scratch,
                                   sizeof(scratch)) == 0 && found == 3u,
                    "lookup directory entry");
    fails += expect(ext2_ro_read_inode(&fs, 3u, &file, scratch, sizeof(scratch)) == 0,
                    "read regular inode");
    fails += expect(ext2_ro_read_file(&fs, &file, 0u, out, 5u, scratch,
                                      sizeof(scratch)) == 5 &&
                    out[0] == 'h' && out[4] == 'o', "read direct data");
    fails += expect(ext2_ro_read_inode(&fs, 4u, &boundary, scratch,
                                       sizeof(scratch)) == 0,
                    "read direct/indirect boundary inode");
    fails += expect(ext2_ro_read_file(&fs, &boundary, 12u * bs - 1u, out, 1u,
                                      scratch, sizeof(scratch)) == 1 && out[0] == 'D',
                    "read last byte of direct region");
    fails += expect(ext2_ro_read_file(&fs, &boundary, 12u * bs, out, 3u,
                                      scratch, sizeof(scratch)) == 3 &&
                    out[0] == 'I' && out[2] == 'D', "read singly-indirect data");
    fails += expect(ext2_ro_read_file(&fs, &boundary, 12u * bs + 2u, out, 8u,
                                      scratch, sizeof(scratch)) == 1 && out[0] == 'D',
                    "clip read at EOF");

    /* Double-indirect: inode block[13] -> root table -> leaf table -> data. */
    put32(im.bytes + (uint64_t)10u * bs, 11u);
    put32(im.bytes + (uint64_t)11u * bs, 12u);
    im.bytes[(uint64_t)12u * bs] = 'D';
    im.bytes[(uint64_t)12u * bs + 1u] = 'B';
    im.bytes[(uint64_t)12u * bs + 2u] = 'L';
    boundary.blocks[13] = 10u;
    boundary.size = (12u + (bs / 4u)) * bs + 3u;
    fails += expect(ext2_ro_read_file(&fs, &boundary,
                                      (12u + (bs / 4u)) * bs, out, 3u,
                                      scratch, sizeof(scratch)) == 3 &&
                    out[0] == 'D' && out[1] == 'B' && out[2] == 'L',
                    "read double-indirect data at first leaf entry");
    return fails;
}
int main(void) {
    if (triple_indirect_geometry(1024u) ||
        triple_indirect_geometry(2048u) ||
        triple_indirect_geometry(4096u)) {
        (void)fprintf(stderr, "EXT2 TRIPLE-INDIRECT TEST: FAIL\n");
        return 1;
    }

    static const struct block_device_ops ops = { image_read };
    struct disk_image im;
    struct block_device dev;
    struct ext2_ro_fs fs;
    struct ext2_ro_inode root, file;
    uint8_t scratch[EXT2_RO_BLOCK_MAX], out[8] = {0};
    uint32_t found = 0u;
    int fails = 0;

    fails += valid_geometry(1024u);
    fails += valid_geometry(2048u);
    fails += valid_geometry(4096u);
    make_image(&im, 1024u);
    dev.ops = &ops; dev.context = &im;
    dev.sector_count = im.sector_count; dev.sector_size = SECTOR_SIZE;
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) == 0,
                    "mount baseline fixture");
    if (fails) return 1;

    fails += expect(ext2_ro_read_inode(&fs, 0u, &root, scratch, sizeof(scratch)) < 0,
                    "reject inode zero");
    fails += expect(ext2_ro_read_inode(&fs, 9u, &root, scratch, sizeof(scratch)) < 0,
                    "reject inode beyond filesystem");

    make_image(&im, 1024u); put16(im.bytes + 1080u, 0u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject bad magic");
    make_image(&im, 1024u); put32(im.bytes + 1048u, 3u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject block-size log");
    make_image(&im, 1024u); put32(im.bytes + 1044u, 0u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject first-data geometry");
    make_image(&im, 1024u); put32(im.bytes + 1056u, 0u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject zero blocks/group");
    make_image(&im, 1024u); put32(im.bytes + 1064u, 0u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject zero inodes/group");
    make_image(&im, 1024u); put16(im.bytes + 1112u, 130u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject misaligned inode size");
    make_image(&im, 1024u); put32(im.bytes + 1028u, IMAGE_BLOCKS + 1u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject fs beyond device");
    make_image(&im, 1024u); put32(im.bytes + 1116u, 1u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject compat feature");
    make_image(&im, 1024u); put32(im.bytes + 1120u, 0x80000000u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject incompat feature");
    make_image(&im, 1024u); put32(im.bytes + 1124u, 4u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject ro-compat feature");
    make_image(&im, 1024u); put32(im.bytes + 2056u, IMAGE_BLOCKS);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject inode table beyond fs");
    make_image(&im, 1024u); put32(im.bytes + 2056u, 0u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "reject inode table before group");
    make_image(&im, 1024u); im.fail_reads = 1;
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0, "propagate backend read failure");

    make_image(&im, 1024u);
    (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 3u, &file, scratch, sizeof(scratch));
    file.blocks[0] = IMAGE_BLOCKS;
    fails += expect(ext2_ro_read_file(&fs, &file, 0u, out, 1u, scratch,
                                      sizeof(scratch)) < 0, "reject invalid direct block");
    make_image(&im, 1024u);
    (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 4u, &file, scratch, sizeof(scratch));
    file.blocks[12] = IMAGE_BLOCKS;
    fails += expect(ext2_ro_read_file(&fs, &file, 12u * 1024u, out, 1u,
                                      scratch, sizeof(scratch)) < 0, "reject invalid indirect block");
    make_image(&im, 1024u);
    (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 4u, &file, scratch, sizeof(scratch));
    file.size = (12u + 256u) * 1024u + 1u;
    file.blocks[13] = IMAGE_BLOCKS;
    fails += expect(ext2_ro_read_file(&fs, &file, (12u + 256u) * 1024u,
                                      out, 1u, scratch, sizeof(scratch)) < 0,
                    "reject out-of-range double-indirect root");

    make_image(&im, 1024u);
    (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 4u, &file, scratch, sizeof(scratch));
    file.size = (12u + 256u) * 1024u + 1u;
    file.blocks[13] = 10u;
    put32(im.bytes + 10u * 1024u, 11u);
    put32(im.bytes + 11u * 1024u, IMAGE_BLOCKS);
    fails += expect(ext2_ro_read_file(&fs, &file, (12u + 256u) * 1024u,
                                      out, 1u, scratch, sizeof(scratch)) < 0,
                    "reject out-of-range double-indirect leaf data pointer");

    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put16(im.bytes + 6u * 1024u + 4u, 0u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject zero rec_len");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put16(im.bytes + 6u * 1024u + 4u, 6u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject unaligned rec_len");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put16(im.bytes + 6u * 1024u + 4u, 8u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject undersized rec_len");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put16(im.bytes + 6u * 1024u + 4u, 1028u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject rec_len crossing block");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put32(im.bytes + 6u * 1024u, 9u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject dirent inode out of range");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    im.bytes[6u * 1024u + 7u] = 8u;
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject invalid dirent type");
    make_image(&im, 1024u); (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    im.bytes[6u * 1024u + 8u] = '/';
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject slash in name");
    make_image(&im, 1024u);
    im.bytes[1120u] = 0u; /* FILETYPE disabled: name_len is 16-bit. */
    (void)ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch));
    (void)ext2_ro_read_inode(&fs, 2u, &root, scratch, sizeof(scratch));
    put16(im.bytes + 6u * 1024u + 6u, 256u);
    fails += expect(ext2_ro_lookup(&fs, &root, "missing", &found, scratch,
                                   sizeof(scratch)) < 0, "reject name length above 255");

    make_image(&im, 1024u);
    put32(im.bytes + 1100u + 76u, 0u);
    put32(im.bytes + 1100u + 100u, 3u);
    fails += expect(ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0,
                    "reject LARGE_FILE feature on revision zero");

    make_image(&im, 1024u);
    put32(im.bytes + 5u * 1024u + 384u + 108u, 1u);
    if (ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0)
        fails += expect(0, "mount regular LARGE_FILE fixture");
    else
        fails += expect(ext2_ro_read_inode(&fs, 4u, &file, scratch,
                                           sizeof(scratch)) < 0,
                        "reject high file-size word without LARGE_FILE");

    make_image(&im, 1024u);
    put32(im.bytes + 1124u, 3u);
    put32(im.bytes + 5u * 1024u + 128u + 108u, 1u);
    if (ext2_ro_mount(&fs, &dev, scratch, sizeof(scratch)) < 0)
        fails += expect(0, "mount large-directory fixture");
    else
        fails += expect(ext2_ro_read_inode(&fs, 2u, &root, scratch,
                                           sizeof(scratch)) < 0,
                        "reject large directory size");

    if (fails) {
        (void)fprintf(stderr, "EXT2 READ-ONLY PARSER TEST: %d failure(s)\n", fails);
        return 1;
    }
    (void)puts("EXT2 READ-ONLY PARSER TEST: PASS");
    return 0;
}
