#!/usr/bin/env python3
"""Create a deterministic, minimal ext2 image for read-only ATA-slave tests."""
import struct
import sys
from pathlib import Path

BLOCK_SIZE = 1024
BLOCK_COUNT = 128
IMAGE_SIZE = BLOCK_SIZE * BLOCK_COUNT
PAYLOAD = b"LionOS ext2 slave read-only fixture OK\n"

def put16(buf, off, value):
    struct.pack_into("<H", buf, off, value)

def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)

def make_inode(mode, size, block):
    inode = bytearray(128)
    put16(inode, 0, mode)
    put32(inode, 4, size)
    put32(inode, 28, 2)  # one 1 KiB block, in 512-byte sectors
    put32(inode, 40, block)
    return inode

def dirent(inode, rec_len, name, file_type):
    encoded = name.encode("ascii")
    entry = bytearray(rec_len)
    struct.pack_into("<I H B B", entry, 0, inode, rec_len, len(encoded), file_type)
    entry[8:8 + len(encoded)] = encoded
    return entry

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_ext2_fixture.py OUTPUT")
    image = bytearray(IMAGE_SIZE)

    # ext2 superblock at byte offset 1024.
    sb = 1024
    put32(image, sb + 0, 32)       # inodes_count
    put32(image, sb + 4, BLOCK_COUNT)
    put32(image, sb + 12, 117)     # free_blocks_count (informational)
    put32(image, sb + 16, 29)      # free_inodes_count (informational)
    put32(image, sb + 20, 1)       # first_data_block for 1 KiB blocks
    put32(image, sb + 24, 0)       # log_block_size
    put32(image, sb + 32, 127)     # blocks_per_group
    put32(image, sb + 36, 127)     # frags_per_group
    put32(image, sb + 40, 32)      # inodes_per_group
    put16(image, sb + 56, 0xEF53)
    put16(image, sb + 58, 1)       # clean filesystem
    put32(image, sb + 76, 1)       # dynamic revision
    put32(image, sb + 84, 11)      # first non-reserved inode
    put16(image, sb + 88, 128)
    put32(image, sb + 92, 0)       # compatible features
    put32(image, sb + 96, 0x0002)  # FILETYPE
    put32(image, sb + 100, 0x0001) # SPARSE_SUPER

    # One group descriptor at block 2; inode table occupies blocks 5..8.
    put32(image, 2 * BLOCK_SIZE + 8, 5)

    # Inode 2 is root directory; inode 3 is README.TXT.
    inode_table = 5 * BLOCK_SIZE
    image[inode_table + 128:inode_table + 256] = make_inode(0x41ED, 1024, 9)
    image[inode_table + 256:inode_table + 384] = make_inode(0x81A4, len(PAYLOAD), 10)

    # Root directory block 9: '.', '..', and README.TXT.
    directory = bytearray(BLOCK_SIZE)
    directory[0:12] = dirent(2, 12, ".", 2)
    directory[12:24] = dirent(2, 12, "..", 2)
    directory[24:BLOCK_SIZE] = dirent(3, BLOCK_SIZE - 24, "README.TXT", 1)
    image[9 * BLOCK_SIZE:10 * BLOCK_SIZE] = directory
    image[10 * BLOCK_SIZE:10 * BLOCK_SIZE + len(PAYLOAD)] = PAYLOAD

    Path(sys.argv[1]).write_bytes(image)

if __name__ == "__main__":
    main()
