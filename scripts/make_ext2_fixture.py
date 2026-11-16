#!/usr/bin/env python3
"""Create a deterministic ext2 image for read-only ATA-slave integration tests."""
import struct
import sys
from pathlib import Path

BLOCK_SIZE = 1024
BLOCK_COUNT = 128
IMAGE_SIZE = BLOCK_SIZE * BLOCK_COUNT
PAYLOAD = b"LionOS ext2 slave read-only fixture OK\n"
NESTED_PAYLOAD = b"LionOS nested ext2 fixture OK\n"

def put16(buf, off, value):
    struct.pack_into("<H", buf, off, value)

def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)

def make_inode(mode, size, block):
    inode = bytearray(128)
    put16(inode, 0, mode)
    put32(inode, 4, size)
    put32(inode, 28, 2)
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

    sb = 1024
    put32(image, sb + 0, 32)
    put32(image, sb + 4, BLOCK_COUNT)
    put32(image, sb + 12, 115)
    put32(image, sb + 16, 27)
    put32(image, sb + 20, 1)
    put32(image, sb + 24, 0)
    put32(image, sb + 32, 127)
    put32(image, sb + 36, 127)
    put32(image, sb + 40, 32)
    put16(image, sb + 56, 0xEF53)
    put16(image, sb + 58, 1)
    put32(image, sb + 76, 1)
    put32(image, sb + 84, 11)
    put16(image, sb + 88, 128)
    put32(image, sb + 92, 0)
    put32(image, sb + 96, 0x0002)
    put32(image, sb + 100, 0x0001)

    put32(image, 2 * BLOCK_SIZE + 8, 5)

    inode_table = 5 * BLOCK_SIZE
    image[inode_table + 128:inode_table + 256] = make_inode(0x41ED, BLOCK_SIZE, 9)
    image[inode_table + 256:inode_table + 384] = make_inode(0x81A4, len(PAYLOAD), 10)
    image[inode_table + 384:inode_table + 512] = make_inode(0x41ED, BLOCK_SIZE, 11)
    image[inode_table + 512:inode_table + 640] = make_inode(0x81A4, len(NESTED_PAYLOAD), 12)

    root = bytearray(BLOCK_SIZE)
    root[0:12] = dirent(2, 12, ".", 2)
    root[12:24] = dirent(2, 12, "..", 2)
    root[24:40] = dirent(4, 16, "subdir", 2)
    root[40:BLOCK_SIZE] = dirent(3, BLOCK_SIZE - 40, "README.TXT", 1)
    image[9 * BLOCK_SIZE:10 * BLOCK_SIZE] = root

    subdir = bytearray(BLOCK_SIZE)
    subdir[0:12] = dirent(4, 12, ".", 2)
    subdir[12:24] = dirent(2, 12, "..", 2)
    subdir[24:BLOCK_SIZE] = dirent(5, BLOCK_SIZE - 24, "nested.txt", 1)
    image[11 * BLOCK_SIZE:12 * BLOCK_SIZE] = subdir

    image[10 * BLOCK_SIZE:10 * BLOCK_SIZE + len(PAYLOAD)] = PAYLOAD
    image[12 * BLOCK_SIZE:12 * BLOCK_SIZE + len(NESTED_PAYLOAD)] = NESTED_PAYLOAD
    Path(sys.argv[1]).write_bytes(image)

if __name__ == "__main__":
    main()
