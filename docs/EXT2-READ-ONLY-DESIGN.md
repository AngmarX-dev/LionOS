# ext2 Read-Only Filesystem: Stage F Design Contract

**Status:** Stage F in progress; F1 generic read-only block contract, F2 isolated ext2 parser core, F3 synthetic host fixtures, F4a ATA IDENTIFY/capacity checks, and F4b-1 primary-master provider are implemented. F4b-2 adds an independently initialized primary-channel slave ATA path and a separate read-only block-device provider. A dedicated QEMU ext2 disk, runtime ATA validation, and VFS mount integration remain pending.  
**Branch:** `feature/graphics-engine-v2`

## Storage safety invariants

- The existing LionFS/DiskFS path remains on the primary-channel master and continues to call `ata_init()`, `ata_read_sector()`, and `ata_write_sector()`.
- `ata_slave_init()` is explicit; it is never called implicitly by primary-master initialization or DiskFS startup.
- The primary-channel slave has an independent IDENTIFY-derived LBA28 capacity and read-only block-device adapter.
- The slave adapter exposes no write operation. The low-level ATA slave API added here is read-only.
- Do not probe or format an unknown disk. Do not attach the existing LionFS image as the slave ext2 fixture.
- No ext2 mount or operational claim is made until QEMU reads a separate ext2 image through the real ATA slave path and VFS integration is tested.

## F4b-2 — Independent primary-channel slave device path

The ATA PIO implementation now keeps separate sector capacities for primary master and primary slave. The master remains selected by `0xA0` for IDENTIFY and by the LBA28 master bit for data commands; the slave uses `0xB0` for IDENTIFY and the LBA28 slave bit for reads. `ata_init()` retains its existing master-only behavior. Callers must explicitly invoke `ata_slave_init()` before using `ata_slave_sector_count()` or `ata_slave_read_sector()`.

The block-device layer exposes two independent initializers: `ata_block_device_init()` for the existing master and `ata_slave_block_device_init()` for the slave. Both publish 512-byte sectors and their own IDENTIFY-derived capacities. The generic descriptor remains read-only.

Host adapter tests use distinct backing arrays to verify that master and slave reads do not cross device paths, and cover geometry, bounds, missing capacity, null descriptors, and backend failures. These tests do not emulate ATA task-file register behavior. A dedicated QEMU secondary disk and a real ATA slave IDENTIFY/read test are still required in the next integration slice.
