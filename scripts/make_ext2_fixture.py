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
TRIPLE_PAYLOAD = b"LionOS triple-indirect QEMU read OK\n"
TRIPLE_LOGICAL_BLOCK = 12 + (BLOCK_SIZE // 4) + (BLOCK_SIZE // 4) ** 2
TRIPLE_OFFSET = TRIPLE_LOGICAL_BLOCK * BLOCK_SIZE
FAST_LINK_TARGET = b"README.TXT"
SLOW_LINK_TARGET = (b"./" * 22) + b"subdir/nested.txt"

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

    triple_inode = make_inode(0x81A4, TRIPLE_OFFSET + len(TRIPLE_PAYLOAD), 0)
    put32(triple_inode, 28, 8)  # Three pointer blocks plus one data block.
    put32(triple_inode, 40 + 14 * 4, 13)  # i_block[14]: triple-indirect root.
    image[inode_table + 640:inode_table + 768] = triple_inode

    fast_link = bytearray(128)
    put16(fast_link, 0, 0xA1FF)
    put32(fast_link, 4, len(FAST_LINK_TARGET))
    fast_link[40:40 + len(FAST_LINK_TARGET)] = FAST_LINK_TARGET
    image[inode_table + 768:inode_table + 896] = fast_link

    slow_link = bytearray(128)
    put16(slow_link, 0, 0xA1FF)
    put32(slow_link, 4, len(SLOW_LINK_TARGET))
    put32(slow_link, 28, 2)
    put32(slow_link, 40, 17)
    image[inode_table + 896:inode_table + 1024] = slow_link

    loop_link = bytearray(128)
    put16(loop_link, 0, 0xA1FF)
    put32(loop_link, 4, len(b"loop-a"))
    loop_link[40:46] = b"loop-a"
    image[inode_table + 1024:inode_table + 1152] = loop_link

    dir_link = bytearray(128)
    put16(dir_link, 0, 0xA1FF)
    put32(dir_link, 4, len(b"subdir"))
    dir_link[40:46] = b"subdir"
    image[inode_table + 1152:inode_table + 1280] = dir_link

    absolute_link = bytearray(128)
    put16(absolute_link, 0, 0xA1FF)
    put32(absolute_link, 4, len(b"/README.TXT"))
    absolute_link[40:50] = b"/README.TXT"
    image[inode_table + 1280:inode_table + 1408] = absolute_link

    # Triple-indirect path: root(13) -> middle(14) -> leaf(15) -> data(16).
    put32(image, 13 * BLOCK_SIZE, 14)
    put32(image, 14 * BLOCK_SIZE, 15)
    put32(image, 15 * BLOCK_SIZE, 16)

    root = bytearray(BLOCK_SIZE)
    root[0:12] = dirent(2, 12, ".", 2)
    root[12:24] = dirent(2, 12, "..", 2)
    root[24:40] = dirent(4, 16, "subdir", 2)
    root[40:60] = dirent(3, 20, "README.TXT", 1)
    root[60:80] = dirent(7, 20, "readme-link", 7)
    root[80:100] = dirent(8, 20, "nested-link", 7)
    root[100:116] = dirent(9, 16, "loop-a", 7)
    root[116:136] = dirent(10, 20, "subdir-link", 7)
    root[136:152] = dirent(11, 16, "abs-link", 7)
    root[152:BLOCK_SIZE] = dirent(6, BLOCK_SIZE - 152, "TRIPLE.TXT", 1)
    image[9 * BLOCK_SIZE:10 * BLOCK_SIZE] = root

    subdir = bytearray(BLOCK_SIZE)
    subdir[0:12] = dirent(4, 12, ".", 2)
    subdir[12:24] = dirent(2, 12, "..", 2)
    subdir[24:BLOCK_SIZE] = dirent(5, BLOCK_SIZE - 24, "nested.txt", 1)
    image[11 * BLOCK_SIZE:12 * BLOCK_SIZE] = subdir

    image[10 * BLOCK_SIZE:10 * BLOCK_SIZE + len(PAYLOAD)] = PAYLOAD
    image[12 * BLOCK_SIZE:12 * BLOCK_SIZE + len(NESTED_PAYLOAD)] = NESTED_PAYLOAD
    image[16 * BLOCK_SIZE:16 * BLOCK_SIZE + len(TRIPLE_PAYLOAD)] = TRIPLE_PAYLOAD
    image[17 * BLOCK_SIZE:17 * BLOCK_SIZE + len(SLOW_LINK_TARGET)] = SLOW_LINK_TARGET
    Path(sys.argv[1]).write_bytes(image)

if __name__ == "__main__":
    main()
