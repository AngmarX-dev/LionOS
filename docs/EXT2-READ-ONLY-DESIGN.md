# ext2 Read-Only Filesystem: Stage F Design Contract

**Status:** F1–F4c implemented; F5 nested paths and directory enumeration implemented, awaiting final CI verification. Hardware testing remains pending.  
**Branch:** `feature/graphics-engine-v2`  
**Audit baseline:** `094675cd6b30af326724ecbd216f075182485a71`

## 1. Findings from the current storage path

The current tree has a usable but narrow ATA PIO sector interface:

- `ata_read_sector(uint32_t lba, void *buffer)` reads one 512-byte sector.
- `ata_write_sector` also exists and is used by the existing persistent LionFS/DiskFS.
- The ATA implementation programs LBA28 task-file registers and now obtains primary-master capacity through IDENTIFY. The primary-master read-only adapter is available, but a separate ext2 device path, partition discovery, and VFS mount integration do not yet exist.
- The current QEMU disk fixture is an 8 MiB raw image. LionFS metadata occupies LBA 0 onward (superblock at LBA 0, directory at LBA 1..5, data beginning at LBA 6); it is not an MBR/GPT-partitioned ext2 disk.
- VFS currently selects RAMFS or LionFS/DiskFS. It has no mount namespace or filesystem-driver registration interface.
- The existing ATA path must not be assumed safe for arbitrary media until IDENTIFY/capacity handling and LBA range checks are implemented.

**Safety consequence:** never run an ext2 probe against the current LionFS test image, never format or write an unknown device, and do not report ext2 mounted/available until a separate ext2 image has been read through a validated block-device path and the VFS integration is exercised.

## 2. Required implementation order

### F1 — Block-device contract

**Implementation progress:** `include/block_device.h` and `src/block_device.c` now define a read-only per-device descriptor with logical sector size, total sector count, opaque backend context, and a bounded multi-sector read callback. `block_device_read` rejects null/incomplete descriptors, zero or unsupported sector sizes, zero-length reads, out-of-range LBAs, arithmetic/32-bit buffer-size overflow, and undersized destination buffers before invoking the backend. There is deliberately no write API.

The descriptor is caller-owned; backend context lifetime is owned by the backend/provider. Backend serialization remains the provider's responsibility. A global device registry is not part of this first slice. The primary-master ATA provider is implemented separately in F4b-1. The existing ATA implementation is not yet registered because it has no IDENTIFY-derived capacity and currently exposes only unchecked 32-bit LBA calls.

A host-side contract test in `tests/block_device_test.c` covers valid reads, returned data, device-boundary rejection, malformed descriptors, maximal LBA and oversized byte ranges. The test is integrated into `make test` as `block-device-check`.

The first filesystem consumer must use only this interface. The primary-master ATA provider is now bounded by IDENTIFY-derived capacity. Do not use it to probe the existing LionFS disk; ext2 must use a separate device.

### F2 — ext2 parser core (no VFS mount yet)

**Implementation progress (2026-11-16):** `include/ext2_ro.h` and `src/ext2_ro.c` now provide a standalone read-only parser over the generic block-device contract. It validates the superblock, supported feature masks, group geometry, inode-table bounds and filesystem-vs-device size before publishing a mounted parser context. It reads regular-file/directory inodes, supports direct and one singly-indirect data-block level, reads regular-file ranges, and performs single-component directory-name lookup. Parsing uses explicit little-endian loads and checked 64-bit calculations; the parser has no write path and is not connected to VFS or ATA. Initial deterministic memory-image tests are integrated into `make test`. The host parser test, kernel link/build, Phase 26 QEMU suite, USB xHCI regression and ISO/USB image checks passed all four required workflows on commit `f53ab75553580417fde06c33f97eb6746f5797a4`. This is parser-core evidence only, not an ext2 VFS mount or runtime disk test.

`EXT2_RO` deliberately rejects unsupported feature flags, indexed directories, symlinks, non-regular/non-directory inode types, sparse file blocks, and double/triple-indirect addressing. It also limits the returned read length to the signed `int` API range.

Implement an isolated read-only ext2 reader over the block-device contract:

- Read the 1024-byte superblock at filesystem byte offset 1024, independent of the filesystem block size.
- Validate magic, revision, block size (1024/2048/4096), inode size, group geometry, counts, feature masks, and all offset/range arithmetic before using metadata.
- Initially support regular files and directories, direct block pointers, and one singly-indirect level. Reject double/triple indirect files and unsupported inode types/features explicitly.
- Validate group descriptor and inode-table locations against the volume bounds.
- Validate every data/indirect block index before I/O.
- Parse directory records with strict `rec_len`, name-length, alignment, and block-boundary checks; support the FILETYPE directory format only when its feature bit is set.
- Do not follow symlinks in the initial slice.
- Never write sectors, replay journals, repair metadata, or silently accept unsupported ext3/ext4 features.

Use explicit little-endian decoding rather than unaligned structure casts. Use checked 64-bit intermediate arithmetic for byte offsets and sector/block conversions, then narrow only after bounds checks.

### F3 — Host-side image tests

**Implementation progress (2026-11-16; CI pending):** `tests/ext2_ro_test.c` now builds deterministic memory-backed ext2 fixtures for 1 KiB, 2 KiB and 4 KiB block sizes. Coverage includes root/inode lookup, direct-to-singly-indirect boundaries, EOF clipping, invalid superblock geometry and feature masks, inode-table bounds, backend read failures, invalid direct/indirect references, unsupported double-indirect access, and malformed directory record/name fields. The fixtures remain host-only and never access physical disks. The first CI attempt exposed incorrect fixture block-pointer/EOF offsets; those fixture values were corrected in a follow-up commit. This progress is not considered validated until all required workflows pass on the same final SHA.

Create small deterministic ext2 images with host-side tooling and test the parser through a memory-backed block device. Tests must cover:

- valid superblocks for each supported block size;
- valid root directory and regular-file reads;
- direct and singly-indirect block lookup;
- truncated images and out-of-range block pointers;
- invalid group/inode geometry and inode sizes;
- unsupported incompat/ro-compat features;
- malformed directory record lengths, names and block crossings;
- I/O callback failures and short/zero-length edge cases.

The test harness must not require privileged access or modify user disks. Host tooling may create temporary fixtures only.

### F4a — ATA IDENTIFY capacity and bounded LBA28 access

**Implementation progress (2026-11-16):** the primary-master ATA PIO path now issues IDENTIFY DEVICE, requires LBA support, parses the reported LBA28 sector count, and rejects reads/writes outside the detected capacity before issuing a data command. The sector transfer copies bytes explicitly, so caller buffers do not need 16-bit alignment. A host-side parser test covers valid capacity, missing LBA support, zero capacity, null arguments, and clamping to the LBA28 address space. This slice does not add a registered block-device provider, partition discovery, a separate ext2 QEMU image, or VFS mounting. Runtime ATA IDENTIFY behavior remains subject to the pending QEMU/CI gate.

### F4b-1 — ATA read-only block-device provider

**Implementation progress (2026-11-16):** added `include/ata_block.h` and `src/ata_block.c`. The provider publishes the already-identified ATA primary-master device through the F1 read-only block-device contract (512-byte sectors and IDENTIFY-derived capacity) and adapts bounded multi-sector requests to the existing single-sector ATA read routine. The ATA PIO command path now serializes IDENTIFY/read/write command sequences with an SMP spin lock. The host test uses a mock ATA sector backend and checks geometry, multi-sector data integrity, range rejection, backend error propagation, undersized buffers, zero detected capacity, and null descriptors. `make test` now includes `ata-block-device-check`.

**Important limitation:** this provider currently describes the same primary-master disk used by LionFS. It is not mounted as ext2 and must not be used to probe the LionFS image. A separate ATA device selection path and a dedicated ext2 QEMU disk are still required. The adapter has not yet been wired into boot-time VFS or ext2 initialization.

### F4b — Separate ATA device, QEMU ext2 image, and VFS integration

Remaining work:

- add an independently selectable ATA device path (for example, primary-channel slave) without changing the primary-master LionFS path;
- add a dedicated QEMU ext2 disk image, separate from `build/lionos-disk.img` used by LionFS persistence;
- test IDENTIFY, sector reads, and ext2 parsing against that dedicated image through the ATA provider;
- add a read-only VFS mount/backend integration with explicit mount lifetime and error propagation.

Keep existing LionFS persistence behavior unchanged. Do not change xHCI source.

## 3. Initial support boundary

The first implementation is **ext2 read-only**, not ext3/ext4 compatibility. It must reject unsupported features rather than guessing. The first milestone does not promise permissions, ownership enforcement, timestamps, symlinks, hard-link semantics, sparse files, large-file support, directory mutation, journaling, or write support unless separately implemented and tested.

## 4. Acceptance gates

Stage F is complete only when all of the following are true:

1. Generic block-device bounds and error tests pass (contract, host test and CI verification complete).
2. Host-side ext2 parser tests pass on the initial valid and malformed synthetic image cases; broader corruption/geometry coverage remains in F3.
3. ATA capacity/range checks pass without regressing LionFS persistence.
4. QEMU reads a separate ext2 image through the real ATA backend.
5. VFS can expose ext2 files read-only without changing RAMFS/LionFS behavior.
6. All required GitHub Actions workflows are green on the final commit SHA.

A successful compile, a parsed superblock, or a host-only test is not evidence that ext2 is mounted or operational on hardware.

### F4b-2 — Independent primary-channel slave device path

**Implementation progress:** the ATA PIO implementation now keeps separate IDENTIFY-derived sector capacities for primary master and primary slave. The master remains selected by `0xA0` for IDENTIFY and by the LBA28 master bit for data commands; the slave uses `0xB0` for IDENTIFY and the LBA28 slave bit for reads. `ata_init()` retains its existing master-only behavior. Callers must explicitly invoke `ata_slave_init()` before using `ata_slave_sector_count()` or `ata_slave_read_sector()`.

The block-device layer exposes two independent initializers: `ata_block_device_init()` for the existing master and `ata_slave_block_device_init()` for the slave. Both publish 512-byte sectors and their own IDENTIFY-derived capacities. The generic descriptor remains read-only; the new slave API does not expose writes.

Host adapter tests use distinct backing arrays to verify that master and slave reads do not cross device paths, and cover geometry, bounds, missing capacity, null descriptors, and backend failures. These tests do not emulate ATA task-file register behavior. A dedicated QEMU secondary disk and a real ATA slave IDENTIFY/read test are still required.

**F4b-3 / F4c — Separate ext2 disk and read-only VFS mount (implemented):** a deterministic ext2 image is attached as the primary-channel slave, separate from the LionFS primary-master disk. QEMU exercises ATA slave initialization, the block-device adapter, ext2 mount, root lookup, file reads, and a VFS read-only smoke test. The QEMU fixture is attached with a temporary snapshot overlay; the source image remains unchanged. This is a parser/VFS integration test, not a production disk partitioning or hardware qualification.

### F5 — Nested paths and directory enumeration

**Implemented and previously CI-verified:** the ext2 parser exposes `ext2_ro_readdir()` iteration. It validates cursor alignment, record lengths, block boundaries, inode numbers, name lengths, file types, and NUL-terminates/clears returned names. The VFS adapter resolves multi-component paths beneath `/ext2`, exposes directory-aware `stat`, and adds `vfs_readdir()` with `1` for an entry, `0` for end-of-directory, and `-1` for invalid paths or malformed data. The fixture now contains `/subdir/nested.txt`; host parser tests cover enumeration at 1/2/4 KiB block sizes, and QEMU smoke tests cover root/subdirectory enumeration, nested file reads, and read-only enforcement.

**Scope:** F5 provides kernel VFS enumeration for ext2. Symlinks, directory mutation, indexed directories, and multi-level indirect blocks remain unsupported. All ext2 access remains read-only.

### F6 — Userland read-only directory streams

**Implementation progress (CI pending):** syscall `LIONOS_SYS_READDIR` (44) exposes one validated ext2 directory entry per call using a user-owned cursor and `struct lion_dirent`. The kernel validates FS capability, bounded path strings, writable/COW-resolvable output pages, and non-overlapping cursor/entry buffers before invoking VFS. Return values are `1` for an entry, `0` for end-of-directory, and `-1` for failure.

The native POSIX-inspired layer adds `opendir()`, `readdir()`, and `closedir()` using a bounded 64-byte path and the existing per-process 32 KiB user heap. `ls` now includes the mounted `/ext2` root alongside the existing RAMFS/LionFS listing. Userland integration tests cover root and nested directory enumeration, entry types, repeated EOF, invalid directory paths, null output pointers, and unaligned cursors. Because errno is not implemented, `readdir()` returns null for both EOF and errors. This is a LionOS-specific subset, not full POSIX directory-stream conformance.
