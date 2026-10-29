#!/usr/bin/env python3
"""Safely create metadata for LionOS's dedicated 8 MiB QEMU test disk.

This script only touches build/lionos-disk.img (or the path explicitly passed
by Makefile). It preserves an existing LionFS v2 image and refuses to
initialize any non-empty, unrecognized image.
"""
import os
import struct
import sys

IMAGE_SIZE = 8 * 1024 * 1024
SECTOR_SIZE = 512
SUPER_LBA = 0
DIR_LBA = 1
DIR_SECTORS = 5
VERSION = 2
SIGNATURE = b"LIONFS1"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: init_disk_image.py IMAGE_PATH", file=sys.stderr)
        return 2

    path = sys.argv[1]
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    created = not os.path.exists(path)

    if created:
        with open(path, "wb") as image:
            image.truncate(IMAGE_SIZE)

    if os.path.getsize(path) != IMAGE_SIZE:
        print(
            f"refusing to initialize {path}: expected an {IMAGE_SIZE}-byte "
            "QEMU test image",
            file=sys.stderr,
        )
        return 1

    with open(path, "r+b") as image:
        contents = image.read()
        superblock = contents[SUPER_LBA * SECTOR_SIZE:
                              (SUPER_LBA + 1) * SECTOR_SIZE]

        if (superblock[:7] == SIGNATURE and
                struct.unpack_from("<I", superblock, 8)[0] == VERSION):
            print(f"LionFS v{VERSION} image preserved: {path}")
            return 0

        if any(contents):
            print(
                f"refusing to overwrite non-empty unrecognized image: {path}",
                file=sys.stderr,
            )
            return 1

        superblock = bytearray(SECTOR_SIZE)
        superblock[:7] = SIGNATURE
        struct.pack_into("<II", superblock, 8, VERSION, 0)
        image.seek(SUPER_LBA * SECTOR_SIZE)
        image.write(superblock)
        image.seek(DIR_LBA * SECTOR_SIZE)
        image.write(bytes(DIR_SECTORS * SECTOR_SIZE))
        image.flush()
        os.fsync(image.fileno())

    print(f"Initialized blank {IMAGE_SIZE}-byte LionFS v{VERSION} QEMU image: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
