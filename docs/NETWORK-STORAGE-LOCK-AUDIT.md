# Network and Storage Lock Call-Site Audit

## Scope and status

This bounded source audit records network and storage synchronization
observations on `feature/graphics-engine-v2` at parent commit
`edf9d192c4983b09b6033daceb05cb045dc88af7`.

Inspected files:

- `src/net.c`
- `src/ata.c`
- `src/ata_block.c`
- `src/block_device.c`
- `src/diskfs.c`
- `src/ext2_vfs.c`
- `src/vfs.c`
- `src/pci.c`

This is a source-level audit only. It does not prove deadlock freedom,
interrupt safety, or safe concurrent network operation. No runtime code is
changed by this document.

## Network

### Loopback queue

- `net_lock` is initialized in `net_init()`.
- `net_send()`, `net_recv()`, and `net_pending()` acquire it with
  IRQ-save semantics while accessing the fixed-size loopback packet queue.
- The lock protects that queue only. It is not acquired by the physical
  RTL8139 transmit or receive helpers.

### RTL8139 polling state

The inspected physical path has shared mutable state:

- `rtl_tx_index` selects one of the shared TX slots.
- `rtl_tx[]` contains the shared transmit buffers.
- `rtl_rx_offset` tracks consumption in the shared RX ring.
- `rtl_rx[]` is the shared receive ring.
- `ip_id` is incremented while constructing IPv4 packets.
- `send_tcp()` and several receive helpers use static frame buffers.

`net_ping()` and `net_http_get()` both reach `rtl_tx_frame()` and
`rtl_poll()`. No common lock protects these shared physical-path objects in
the inspected source. Concurrent callers can therefore race over TX slot
selection/buffer contents, RX ring consumption, packet identifiers, and
static packet buffers. This is an unresolved concurrency defect candidate;
the audit does not claim that a particular race was reproduced.

The inspected RTL initialization writes zero to the device interrupt mask
register and the current data path polls the controller. No RTL8139 interrupt
handler was identified in the inspected files. Polling does not prevent two
process contexts from entering the network helpers concurrently.

## ATA and VFS

- `ata_io_lock` serializes ATA channel command sequences using atomic
  test-and-set/release operations. It is held across device status polling
  and the 256-word PIO data transfer. The lock implementation does not
  disable local interrupts itself.
- The inspected ATA functions do not call process, VFS, or filesystem
  callbacks while holding `ata_io_lock`. No ATA IRQ handler was found in
  the inspected source set.
- VFS entry points acquire `vfs_lock` with
  `spinlock_irqsave_acquire()`. Read/write and metadata operations call
  filesystem backends before releasing that lock.
- DiskFS backend operations call `ata_read_sector()` or
  `ata_write_sector()`, establishing the observed nested order
  `vfs_lock → ata_io_lock`.
- Since `vfs_lock` is IRQ-save, a disk-backed VFS operation retains local
  interrupt disablement while ATA status polling and PIO transfer loops run.
  This can increase interrupt latency. The source review did not establish
  a deadlock or a specific missed-interrupt failure.
- DiskFS uses shared metadata and a shared sector scratch buffer. Ext2 VFS
  uses a shared scratch buffer. The inspected VFS backend operations run
  under `vfs_lock`, which currently serializes these shared structures.
  The generic `block_device_read()` validates and dispatches through the
  backend operation pointer; it does not add synchronization of its own.
- Ext2 VFS initialization and ATA device initialization occur through
  initialization paths; no runtime unmount/teardown protocol was established
  by this audit.

The existing VFS lock also protects descriptor and open-file object state.
Moving backend I/O outside that lock cannot be done safely as a mechanical
lock-scope change: it needs an explicit reference/lifetime protocol and
separate serialization for the shared filesystem scratch and metadata.

## PCI configuration access

The PCI configuration mechanism uses the shared CF8/CFC address/data ports.
The inspected generic PCI accessors and the RTL driver's private PCI
accessors do not share a serialization lock. Their observed use is in
initialization paths; overlapping concurrent access was not demonstrated.
Keep this as an unresolved cross-driver boundary until all PCI config callers
and initialization ordering are audited.

## Existing tests and gaps

The inspected test tree includes ATA identify parsing and block-device
adapter tests. It does not include a dedicated concurrent RTL8139 TX/RX test
or a regression that measures VFS/ATA lock hold time and interrupt latency.
The existing QEMU networking and storage checks do not establish physical
hardware behavior or arbitrary concurrent access.

## Review boundary

The observed edge `vfs_lock → ata_io_lock` and the unprotected physical
RTL8139 shared state extend the partial process lock graph. This document
does not prove a total lock order, IRQ/NMI suitability, or deadlock freedom.
Do not reduce `vfs_lock`, alter ATA locking, or claim concurrent RTL8139
safety until object lifetimes, driver ownership, and dedicated regressions
have been designed and verified.
