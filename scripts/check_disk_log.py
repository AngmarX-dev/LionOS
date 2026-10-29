#!/usr/bin/env python3
"""Validate LionOS's persistent log in the dedicated raw QEMU test image."""
import os
import struct
import sys

SECTOR_SIZE = 512
DIR_LBA = 1
DIR_SECTORS = 5
DATA_LBA = 6
SECTORS_PER_FILE = 8
MAX_FILES = 32
ENTRY_SIZE = 72
MAX_FILE_SIZE = 4096
SIGNATURE = b"LIONFS1"
VERSION = 2
LOG_NAME = b"var/log/lionos.log"
SESSION_MARKER = b"----- LionOS boot session -----"
COMPLETE_MARKER = b"LionOS boot completed; starting desktop"


def fail(message: str) -> int:
    print(f"persistent log check: FAIL: {message}", file=sys.stderr)
    return 1


def main() -> int:
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--require-complete"):
        print("usage: check_disk_log.py IMAGE_PATH [--require-complete]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    require_complete = len(sys.argv) == 3
    if not os.path.isfile(path):
        return fail(f"image not found: {path}")

    with open(path, "rb") as image:
        raw = image.read()
    if len(raw) != 8 * 1024 * 1024:
        return fail("unexpected image size")
    if raw[:7] != SIGNATURE:
        return fail("LionFS signature missing")
    if struct.unpack_from("<I", raw, 8)[0] != VERSION:
        return fail("unsupported LionFS version")

    directory = raw[DIR_LBA * SECTOR_SIZE:
                    (DIR_LBA + DIR_SECTORS) * SECTOR_SIZE]
    log_data = None
    for index in range(MAX_FILES):
        entry = directory[index * ENTRY_SIZE:(index + 1) * ENTRY_SIZE]
        name = entry[:64].split(b"\0", 1)[0]
        if not name:
            continue
        size, start_lba = struct.unpack_from("<II", entry, 64)
        if size > MAX_FILE_SIZE:
            return fail(f"invalid file size for {name!r}")
        if start_lba != DATA_LBA + index * SECTORS_PER_FILE:
            return fail(f"invalid data location for {name!r}")
        if name == LOG_NAME:
            start = start_lba * SECTOR_SIZE
            log_data = raw[start:start + size]
            break

    if log_data is None:
        return fail("var/log/lionos.log entry missing")
    if not log_data:
        return fail("log file is empty")
    if SESSION_MARKER not in log_data:
        return fail(f"expected log marker missing: {SESSION_MARKER.decode()}")
    if require_complete and COMPLETE_MARKER not in log_data:
        return fail(f"expected log marker missing: {COMPLETE_MARKER.decode()}")
    print(
        "persistent log check: PASS "
        f"(var/log/lionos.log, {len(log_data)} bytes; markers verified)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
