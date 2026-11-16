# ext2 Read-Only Filesystem: Stage F Design Contract

**Status:** F1–F10 implementation and CI validation in progress; physical hardware qualification remains pending.  
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

**Scope:** F5 provides kernel VFS enumeration for ext2. Symlinks, directory mutation, and indexed directories remain unsupported. All ext2 access remains read-only.

### F6 — Userland read-only directory streams

**Implemented and CI-verified on `8377a6d203daecda59c2d78d09685efa05d3c380`:** syscall `LIONOS_SYS_READDIR` (44) exposes one validated ext2 directory entry per call using a user-owned cursor and `struct lion_dirent`. The kernel validates FS capability, bounded path strings, writable/COW-resolvable output pages, and non-overlapping cursor/entry buffers before invoking VFS. Return values are `1` for an entry, `0` for end-of-directory, and `-1` for failure.

The native POSIX-inspired layer adds `opendir()`, `readdir()`, and `closedir()` using a bounded 64-byte path and the existing per-process 32 KiB user heap. `ls` now includes the mounted `/ext2` root alongside the existing RAMFS/LionFS listing. Userland integration tests cover root and nested directory enumeration, entry types, repeated EOF, invalid directory paths, null output pointers, and unaligned cursors. Because errno is not implemented, `readdir()` returns null for both EOF and errors. This is a LionOS-specific subset, not full POSIX directory-stream conformance.

### F7 — Double-indirect block mapping

**Implemented and CI-verified on `9d9ce8c1a26df5d3a40b74fd8093633d6cbf0131`:** ext2 file and directory block mapping supports the twelve direct pointers, the singly-indirect pointer, and the doubly-indirect pointer. The mapper bounds the logical block against the double-indirect capacity, validates the root and leaf pointer blocks through the bounded block reader, and rejects zero or out-of-range data pointers. Triple-indirect mapping is implemented in F8; see the following section.

Host parser tests exercise a double-indirect data read for 1 KiB, 2 KiB, and 4 KiB filesystem block sizes. Negative tests cover an out-of-range double-indirect root and an out-of-range leaf data pointer. Existing direct/single-indirect, directory traversal, malformed-record, and backend-error tests remain in place. QEMU runtime coverage of a double-indirect fixture and real-device qualification are still outstanding.

### F8 — Triple-indirect block mapping

**Implemented and CI-verified on `f6567a9afcd9199a37e6fdf306bdfc862532eb30`:** ext2 file and directory block mapping now follows all three levels of the inode's triple-indirect pointer. Every intermediate pointer block and final data block is range-checked through the bounded block reader. Capacity arithmetic uses 64-bit bounds, while quotient/remainder calculations use the existing freestanding-safe 64-by-32 division helper to avoid introducing compiler runtime dependencies into the kernel.

Host tests build sparse pointer trees and read data through triple-indirect mapping with 1 KiB and 2 KiB block sizes. Negative tests reject invalid root-to-middle, middle-to-leaf, and leaf-to-data pointers. QEMU runtime coverage and real-device qualification remain outstanding.

### F9 — ext2 large-file parser support

**Implemented and CI-verified on `fb3ebd909738ec65ccfbc66957c07c361bfadf35`:** the parser now recognizes the ext2 `LARGE_FILE` read-only-compatible feature, decodes the high 32-bit regular-file size word, and carries 64-bit inode sizes, logical block indexes, and read offsets internally. Triple-indirect reads with 4 KiB blocks are included in host fixtures using a sparse pointer tree beyond the 4 GiB boundary. Directory sizes remain constrained to the existing 32-bit directory cursor contract. The existing VFS deliberately rejects regular files larger than `UINT32_MAX` rather than narrowing their sizes or exposing truncated EOF; 64-bit VFS descriptors, `stat` UAPI, and userland seek APIs require a separate versioned ABI change. QEMU large-file runtime coverage and hardware qualification remain outstanding.


### F10 — QEMU triple-indirect VFS integration

**Implementation progress (CI pending):** the deterministic ATA-slave ext2 fixture now includes a sparse regular file whose only allocated data block is reached through the inode's triple-indirect pointer. With 1 KiB filesystem blocks, the test data resides at logical block `12 + 256 + 65536` (byte offset 67,383,296), while the physical image remains small because the intervening file range is sparse. The kernel boot test validates the file's VFS metadata and reads the payload at that offset through the ext2 VFS adapter. Both normal SMP boots must emit `LIONOS:EXT2-VFS-TRIPLE-INDIRECT-TEST-OK`; the existing LionFS persistence, directory, read-only, and GUI regression checks remain required. This is QEMU integration coverage for triple-indirect addressing, not a 4 GiB VFS test or physical-hardware qualification.

F10 acceptance: host parser tests pass; QEMU reads the triple-indirect payload through the ATA primary-slave block device and ext2 VFS on both boots; all required GitHub Actions workflows are green on the same final SHA. Stage F remains read-only and does not add symlink traversal, indexed directories, directory mutation, ext3 journaling, or write support.


### G1 — Read-only symbolic-link target parsing

**Implementation scope:** ext2 inode parsing recognizes symbolic-link inode types and retains the inode's 512-byte sector count plus the 60-byte inline `i_block` payload. `ext2_ro_read_symlink()` reads fast symlink targets inline when `i_blocks == 0`, and slow targets through the existing bounded data-block mapper when sectors are allocated. It validates inode type, target length, destination capacity including the trailing NUL, minimum allocated data sectors, data-block bounds, and embedded NUL bytes. This stage returns the target text only; it does not follow links during VFS path resolution, detect cycles, or mutate the filesystem. The ext2 backend remains read-only.


### G2 — Bounded symbolic-link traversal in the VFS

**Implementation scope:** ext2 VFS path resolution now follows symbolic links in both intermediate and final path components. Relative targets are resolved against the containing directory; absolute targets are interpreted from the ext2 mount root. The resolver normalizes dot components, prevents traversal above the ext2 root, restarts lookup after link expansion, tracks followed inode numbers to reject cycles, and enforces an eight-link traversal limit. Target expansion and intermediate buffers are bounded by the VFS path limit. No write operation or symlink mutation is introduced.

The deterministic QEMU ext2 fixture contains a fast relative link to the root README, a slow relative link to a nested file (target length greater than the 60-byte inline area), and a self-referential link. Boot integration tests require successful reads through both valid links and rejection of the cyclic link on both QEMU boots. The existing read-only, nested-directory, triple-indirect, LionFS persistence, GUI, and xHCI regression gates remain required.

**Limitations:** this is a bounded ext2-only resolver, not full POSIX pathname semantics. The VFS currently normalizes user paths before backend resolution, so lexical dot/dot-dot handling occurs before symlink traversal for caller-supplied paths. `lstat`, link creation/removal, permissions, and mount-namespace semantics remain out of scope.


### G3 — Read-only `readlink` and `lstat` interfaces

**Implementation scope:** the ext2 VFS now has a no-follow final-component resolver mode. Intermediate symbolic links are still followed, while the final inode is returned unchanged. `ext2_vfs_lstat()` exposes the final inode's size and directory/symlink type; `ext2_vfs_readlink()` returns up to the caller's requested number of target bytes without appending a NUL byte. The generic VFS exposes `vfs_lstat_path()` and `vfs_readlink()`, and userland receives `lion_lstat()` and `lion_readlink()` through new system-call numbers 46 and 45. The UAPI version is incremented to 7. All operations remain read-only and require the existing filesystem process capability.

**Validation:** the ext2 boot fixture checks that `lstat` identifies the final link itself, ordinary `stat` follows it, and `readlink` returns the exact target bytes. The ext2 namespace fails closed instead of falling through to another backend when an ext2 lookup fails. Link targets remain bounded by the current 64-byte VFS path buffer; this is not yet a full POSIX implementation. Kernel/QEMU and host tests plus all required CI workflows must pass on the same final commit.

### G4 — Symbolic-link API edge-case regression coverage

**Implementation scope:** extend the QEMU VFS boot regression tests for the G3 read-only readlink and lstat interfaces without changing filesystem semantics or the syscall ABI. Verify that readlink rejects a regular file, returns only the requested prefix for a short destination (without requiring a NUL terminator), and that lstat reports a final cyclic link itself while ordinary stat rejects following that cycle. Also verify that an intermediate symlink is followed before the final regular-file metadata is returned.

**Acceptance:** the new edge-case marker must be emitted on both normal QEMU boots, the existing ext2/LionFS/GUI/xHCI regressions must remain intact, and all required GitHub Actions workflows must pass on the same final commit SHA. No ext2 writes, link mutation, or changes to the resolver traversal limit are included.

### G5 — Ring-3 symbolic-link syscall integration

**Implementation scope:** extend the existing Ring-3 VFS integration program to exercise `lion_lstat()` and `lion_readlink()` through `int 0x80`, not merely through kernel-side VFS calls. The test validates final-component no-follow metadata, target-following `stat`, exact target bytes, short-buffer truncation without a terminator, rejection of regular files and invalid user arguments, cyclic-link behavior, and an intermediate symlink leading to a regular file. The kernel emits a dedicated success/failure marker from the user process exit status.

**Acceptance:** both normal QEMU boots must emit `LIONOS:USER-EXT2-SYMLINK-SYSCALL-TEST-OK`; existing VFS, COW, userland utility, GUI, persistence, and xHCI checks remain required. No syscall numbers, UAPI layouts, filesystem write semantics, or symlink traversal limits change.

### G6 — System-call user-memory and path-boundary hardening

**Implementation scope:** extend the existing Ring-3 symlink syscall regression test without changing syscall numbers, UAPI layouts, VFS semantics, or path limits. Verify that lstat rejects a user path that has no NUL terminator within the fixed VFS path buffer, that readlink rejects a capacity larger than the syscall copy limit, and that both readlink output and lstat metadata pointers are rejected when their requested ranges cross the user/kernel address boundary. Boundary pointers are chosen so validation must reject the range before dereferencing it.

**Acceptance:** the existing LIONOS:USER-EXT2-SYMLINK-SYSCALL-TEST-OK marker must still be emitted on both normal QEMU boots; all previous filesystem, persistence, memory, GUI, and xHCI regression checks remain required. The implementation must preserve bounded user-memory validation and must not write to user buffers before all relevant argument checks succeed.

### G7 — Symbolic-link traversal depth boundary

**Implementation scope:** extend the deterministic ext2 image with a chain of eight distinct fast symbolic links that resolves successfully to the README, plus a ninth-link entry that exceeds the existing resolver limit. The resolver limit remains unchanged. The boot regression verifies that the eight-link chain resolves to the expected regular-file metadata and that the nine-link chain is rejected. No filesystem mutation or resolver policy change is included.

**Acceptance:** both QEMU boot logs must contain LIONOS:EXT2-VFS-SYMLINK-DEPTH-TEST-OK; the existing symlink, userland syscall, ext2, persistence, GUI, and xHCI checks must continue to pass on the same final commit SHA.
