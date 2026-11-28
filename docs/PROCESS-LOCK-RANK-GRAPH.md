# Process-Lock Rank Graph: Source Audit

## Scope and status

This document records a source-level lock-domain inventory for the process,
scheduler, wait, VFS, memory, paging, and selected input-driver paths on
`feature/graphics-engine-v2` at parent commit
`edf9d192c4983b09b6033daceb05cb045dc88af7`.

It extends the bounded wait-call-site audit; it is **not** a complete
repository-wide lock-rank proof. It does not establish that all callbacks,
drivers, or future code obey one total order. No runtime synchronization
behavior is changed by this document, and it does not authorize reducing,
splitting, or removing `process_lock`.

The audit distinguishes:
- **Observed nested edge:** both locks are held at the same time on an
  inspected path.
- **Sequential handoff:** one lock is released before another domain is
  entered; this is not a nested lock edge.
- **Unresolved:** source evidence is insufficient to claim a total order.

## Lock domains and observed edges

### Process lifecycle and scheduler

- `process_lock → runqueues[cpu].lock`: process-state transitions publish or
  remove READY queue membership while the process lock is held.
- `process_lock → runqueues[first].lock → runqueues[second].lock`: migration
  and whole-queue validation acquire two queue locks in ascending CPU-index
  order, then release them in reverse order.
- The inspected run-queue helpers do not call back into process APIs. A
  run-queue-lock holder must not acquire `process_lock`.
- The scheduler interrupt path disables local interrupts and uses a
  non-blocking try-acquire for `process_lock`. A failed attempt defers the
  scheduling decision; it does not spin in interrupt context.

**Contract:** process state, CPU ownership, and queue membership remain one
serialized transaction. Per-CPU queues do not make process lifecycle
operations concurrent while this lock remains authoritative.

### Wait channels and subsystem locks

The central `process_sleep_on_kind()` handoff acquires `process_lock` while
the caller's wait-object lock is still held. This preserves the
check-to-sleep atomicity needed to prevent lost wakeups.

| Observed nested edge | Source path | Wake behavior |
|---|---|---|
| `futex_bucket.lock → process_lock` | Futex wait and futex wake | Futex wake deliberately retains the bucket lock while publishing READY |
| `pipe_lock → process_lock` | Blocking pipe read/write | Normal pipe wake paths release `pipe_lock` before `process_wakeup()` |
| `console_tty.input_lock → process_lock` | Blocking TTY read | Input producers release `input_lock` before waking readers |
| `ipc_lock → process_lock` | Blocking IPC receive | IPC send/receive release `ipc_lock` before waking the channel |
| AP probe-local wait lock → `process_lock` | Test-only AP lifecycle wait | BSP wake is issued outside the process-lock critical section |

The futex edge is intentionally different from pipe, TTY, and IPC wake paths:
`futex_wake()` keeps its bucket lock while `process_futex_wakeup()` takes
`process_lock`. Any future futex change must preserve or deliberately redesign
this ordering together with the wait registration protocol.

### TTY internal lock ordering

The inspected TTY implementation has these nested orders:

- `output_lock → input_lock` in echo flushing and session release.
- `input_lock → tty.lock` in session ownership, input, and configuration
  paths.
- `output_lock → tty.lock` while validating ownership before output.

The inspected paths release locks in reverse order. TTY signal delivery and
reader wakeups occur after the relevant input/state locks are released.
No reverse `tty.lock → input_lock` or `tty.lock → output_lock` edge was found
in the inspected `src/tty.c` call sites. This is a bounded file-level
observation, not a proof covering every caller or future callback.

### Keyboard and mouse input-driver lock boundaries

The inspected input paths establish two additional wait-lock edges:

- `keyboard_buffer_lock → process_lock`: `keyboard_read_blocking()`
  enters `process_sleep_on()` with the keyboard ring lock held. The producer
  `push_char()` forwards the byte to the TTY before taking
  `keyboard_buffer_lock`, then releases the ring lock before calling
  `process_wakeup()`. The forwarding path therefore does not nest the
  keyboard ring lock with TTY or process locks.
- `mouse_event_lock → process_lock`: `mouse_read_event()` enters
  `process_sleep_on()` with the event-ring lock held. The producer
  `mouse_event_push()` publishes the event under the ring lock, releases it,
  and only then calls `process_wakeup()`.
- The PS/2 path reaches `mouse_event_push()` from
  `mouse_handle_ps2_byte()`; the USB path is polled by `mouse_poll()`.
  The inspected `xhci_mouse_poll()` dispatches USB keyboard reports into
  `keyboard_handle_usb_report()`; no xHCI spinlock acquisition was found in
  the inspected driver. This does not establish that every xHCI state access
  is safe for concurrent callers.
- `tty_input_byte()` nests `console_tty.input_lock → console_tty.lock`
  when it snapshots foreground state for Ctrl-C. It releases both locks
  before calling `process_tty_signal_foreground()`. Ordinary input wakeups
  also occur after `input_lock` is released. Blocking TTY reads retain the
  documented `input_lock → process_lock` wait handoff.
- The inspected keyboard and mouse ring accessors do not call process APIs
  while their ring locks are held, except for the deliberate
  `process_sleep_on()` handoff. No reverse `process_lock → keyboard ring
  lock` or `process_lock → mouse event lock` edge was found in these files.

The host `keyboard_tty_test` covers translated-byte forwarding, canonical
input, and Ctrl-C behavior, but it is not a concurrent ring-lock test. The
inspected tree has no dedicated host mouse-event-ring concurrency regression.
These findings are bounded to the listed call paths; they do not complete the
driver, interrupt, or callback lock graph.

### Networking and storage lock boundaries

The bounded follow-up audit inspected `src/net.c`, `src/ata.c`,
`src/ata_block.c`, `src/block_device.c`, `src/diskfs.c`,
`src/ext2_vfs.c`, and the relevant VFS call paths in `src/vfs.c`.

- `net_lock` protects the in-memory loopback packet queue used by
  `net_send()`, `net_recv()`, and `net_pending()`. It does not protect
  the RTL8139 polling path.
- The physical RTL8139 path shares `rtl_tx_index`, the TX slot buffers,
  `rtl_rx_offset`, and the RX ring between `rtl_tx_frame()` and
  `rtl_poll()` callers without a common lock. `net_ping()` and
  `net_http_get()` both enter these helpers. `send_tcp()` and several
  receive helpers also use static frame buffers. Concurrent physical
  network syscalls therefore have unresolved shared-state races; the
  current source audit does not prove safe concurrent use.
- RTL8139 is configured for polling in the inspected initialization path
  (`REG_IMR` is written with zero). This avoids claiming an observed
  network IRQ callback, but does not serialize concurrent process callers.
- ATA channel commands are serialized by `ata_io_lock`, implemented with
  atomic test-and-set/release. The lock is held across status polling and
  256-word PIO transfers. The inspected ATA path does not call process or
  VFS callbacks while holding that lock, and no ATA interrupt handler was
  found in the inspected files. The lock itself does not save/restore IRQ
  state.
- VFS public read/write/stat operations acquire `vfs_lock` using the
  IRQ-save interface and invoke filesystem backends before releasing it.
  DiskFS reads/writes reach ATA sector operations, establishing the nested
  edge `vfs_lock → ata_io_lock`. Consequently, disk-backed VFS I/O can
  retain `vfs_lock` with local interrupts disabled while ATA polling and
  PIO transfer loops run. This is a latency and interrupt-context concern;
  it is not evidence of a demonstrated deadlock. Any lock-scope redesign
  must first preserve descriptor/backend lifetime and serialize the shared
  DiskFS/Ext2 scratch state.
- The DiskFS metadata and sector scratch buffer, and the Ext2 VFS scratch
  buffer, are shared globals. The inspected VFS backend calls are made
  while `vfs_lock` is held, which currently serializes those shared
  structures. `block_device_read()` dispatches through the backend
  operation pointer and does not provide independent synchronization.
- PCI configuration accesses use the shared CF8/CFC mechanism. The
  inspected `src/pci.c` accessors and RTL driver's private accessors do
  not share a lock. Their current boot-time initialization order was not
  shown to overlap; concurrent PCI configuration access remains an
  unresolved boundary, not a confirmed runtime race.

The host test inventory includes ATA identify parsing and block-device
adapter tests, but no dedicated concurrent RTL8139 TX/RX or VFS/ATA
contention regression was found in the inspected tree. These findings are
bounded source observations, not a complete driver/interrupt lock proof.

### Console, framebuffer, exception output, and PCI configuration

A bounded follow-up audit inspected `src/console.c`, `src/framebuffer.c`,
`src/idt.c`, `src/pci.c`, and the RTL8139 PCI accessors in `src/net.c`
at parent commit
`8ba463757e2f9b5a70cb08108127cd6374dc6066`. See
[the detailed audit](CONSOLE-FRAMEBUFFER-INTERRUPT-PCI-LOCK-AUDIT.md).

- The console cursor (`row`, `col`), color, and output-mode globals are
  accessed without a common lock. `console_write()` emits a message
  character-by-character rather than atomically.
- `interrupt_dispatch()` calls console routines for exception reporting,
  including the page-fault dump path. This establishes exception-context
  console output; concurrent framebuffer drawing from a hard IRQ was not
  established by this inspection.
- The framebuffer renderer has shared desktop/layer/clip/cache state without
  a common lock. Layer begin/end saves and restores global clip state, so
  concurrent renderer callers require an explicit ownership protocol.
- A framebuffer map failure after earlier page mappings returns without
  rolling back those mappings. No general framebuffer teardown API was
  identified in the inspected implementation.
- Generic PCI configuration access in `src/pci.c` and RTL8139-private access
  in `src/net.c` both use CF8/CFC without a shared transaction lock. The
  inspected call paths are initialization-oriented and do not prove current
  overlap, but the address-write/data-read sequence is vulnerable to
  interleaving if concurrent callers are introduced.

These are unresolved shared-state and lifecycle boundaries, not observed
nested lock edges. Do not add a blocking console lock or independent
per-driver PCI locks mechanically; define exception/panic output semantics,
renderer ownership, and one shared PCI transaction layer first.

### Process job-control access to TTY state

- `process_tty_set_foreground()` holds `process_lock` while calling both
  `tty_get_foreground_pgid()` and `tty_set_foreground_pgid()`;
  `process_tty_signal_foreground()` and `process_tty_check_foreground()`
  also call `tty_get_foreground_pgid()` while holding `process_lock`.
  Each TTY accessor acquires `console_tty.lock`, so these paths establish
  the nested edge `process_lock → TTY state lock`.
- In the inspected `src/tty.c` paths, process callbacks do not retain the
  TTY state lock: `tty_input_byte()` releases both the state and input
  locks before calling `process_tty_signal_foreground()`, and reader
  wakeups/session notifications occur after their TTY locks are released.
  `tty_read()` and `tty_write()` perform foreground checks before
  taking their input/output locks.
- No reverse `TTY state lock → process_lock` nested edge was found in
  the inspected TTY file. This is a bounded call-site observation; other
  callers and indirect callbacks still require repository-wide review.

### VFS, heap, and filesystem backends

- `vfs_lock` protects descriptor tables and open-file objects.
- VFS read/write paths can call `kmalloc()`/`kfree()` while `vfs_lock` is
  held. Heap metadata is serialized by `heap_lock`, and the physical allocator
  owns page state, so the observed nested chain is
  `vfs_lock → heap_lock → memory_lock` through
  `page_alloc_contiguous()`/`page_free()`.
- VFS operations also call RAMFS, diskfs, and read-only Ext2 backend functions
  while `vfs_lock` is held. The inspected backend files do not declare
  spinlocks, but this observation does not prove that all lower-level device
  paths are lock-free.
- Process TTY reads/writes take a reference to the open-file object, release
  `vfs_lock`, and only then call the TTY layer. This is a sequential handoff,
  not a `vfs_lock → TTY lock` nested edge.
- Fork and exit paths release `process_lock` before VFS/pipe descriptor
  cloning or teardown in the inspected paths. Their correctness still depends
  on the unpublished-child and sibling-retirement lifetime protocol.

**Heap serialization:** `heap_lock` protects the global allocation table and
is held across PMM allocation/free, establishing the nested edge
`heap_lock → memory_lock`. The PMM does not call back into the heap, so the
inspected allocator path has no reverse `memory_lock → heap_lock` edge. A host
pthread regression covers concurrent allocation/free, table capacity, page
rounding, invalid/double frees, and idempotent initialization. Boot calls
`heap_init()` immediately after `paging_init()` and before framebuffer/device
initialization, because framebuffer setup may allocate from the heap. This does not
certify every external caller's interrupt or callback context; that remains
part of the repository-wide caller audit.

### Syscall allocation and blocking boundaries

The inspected syscall paths allocate temporary buffers before entering
potentially blocking subsystem operations. In each listed path, the allocator
lock is acquired only inside `kmalloc()`/`kfree()` and is released before
those functions return; retaining the allocated buffer across a wait does not
retain `heap_lock`.

| Syscall path | Allocation and subsequent operation | Lock/lifetime observation |
|---|---|---|
| `SYS_READ` | Allocates a user-read buffer, then calls `keyboard_read_blocking()` | `kmalloc()` returns before the blocking keyboard read; `kfree()` follows the read/copy-out path |
| `LIONOS_SYS_FREAD` on TTY | Allocates the syscall buffer, then calls `vfs_read_for_process()` | The TTY branch takes an open-file reference and releases `vfs_lock` before `tty_read_blocking()`; it reacquires the VFS lock only to drop the reference after return |
| `LIONOS_SYS_IPC_RECV` | Allocates the receive buffer, then calls `ipc_recv()` | The heap lock is not retained by the allocation across IPC wait; IPC's own wait-lock ordering is documented above |
| `LIONOS_SYS_NET_RECV` | Allocates the receive buffer, then calls `net_recv()` | The heap lock is released before entering the network receive path; this audit makes no claim about every network-driver lock or callback |

These are call-site observations from the inspected syscall dispatcher and
VFS implementation, not a guarantee that all operations are non-blocking or
that every subsystem callback is safe in every interrupt context. The buffer
remains allocated and owned by the syscall path until its operation returns;
that storage lifetime is distinct from holding the allocator's spinlock.

### Physical memory and paging

- `memory_lock` protects low/high physical-page allocation state, reference
  counts, high-page pin records, and free-page accounting.
- Process lifecycle paths can call page allocation/free while holding
  `process_lock`; the observed nested edge is
  `process_lock → memory_lock`. They also call user-address-space paging APIs
  while holding `process_lock`, but the inspected user mapping, COW, and
  address-space teardown paths do not establish a nested edge to the kernel
  MMIO/high-map locks described below.
- Kernel MMIO mapping operations hold `kernel_map_lock` while updating PTEs
  and performing the required TLB invalidation/shootdown.
- High-memory mapping uses `high_map_lock` for virtual-slot ownership and
  `memory_lock` for physical-page pinning. The inspected implementation
  performs pin/unpin outside the high-map critical section.
- High-map unmapping marks a slot under `high_map_lock`, releases it, performs
  the PTE/TLB operation under `kernel_map_lock`, releases that lock, then
  reacquires `high_map_lock` to retire the slot and finally unpins the page.
  These are ordered phases, not one nested three-lock chain.
- The inspected paging map functions do not acquire `memory_lock` while
  holding `kernel_map_lock`; the high-page pin is acquired before entering
  `high_map_lock`.

These observations do not cover every address-space operation, page-table
callback, TLB-shootdown caller, or driver-specific mapping path. Those remain
open for the complete graph.

## Consolidated observed graph

The following edges are supported by the inspected source paths:

```text
futex bucket lock ───────────────┐
pipe lock ───────────────────────┤
TTY input lock ──────────────────┤
keyboard buffer lock ────────────┤
mouse event lock ────────────────┤
IPC lock ────────────────────────┼──> process_lock ──> runqueue lock(s)
AP probe wait lock ─────────────┘          ├──> memory_lock
                                           └──> TTY state lock

kernel MMIO mapping ──> kernel_map_lock ──> TLB shootdown
high-map slot bookkeeping ──> high_map_lock
  (released) → kernel_map_lock → (released) → high_map_lock
  (released) → memory_lock for final unpin

TTY output_lock ──> TTY input_lock ──> TTY state lock
       └─────────────────────────────> TTY state lock

vfs_lock ──> filesystem backend calls ──> ata_io_lock
    └──────> heap_lock ──> memory_lock

net_lock ──> loopback packet queue only
RTL8139 TX/RX shared state ──> no common lock observed
```

The graph intentionally omits any edge that was not established by the
inspected call paths. In particular, it is not safe to infer a reverse edge
from this diagram's absence of a line.

## Driver initialization and teardown boundaries

A bounded lifecycle review inspected ATA/block-device, keyboard, mouse,
RTL8139, PCI enumeration, and interrupt dispatch code at parent commit
`afbefc15a77ba42d5e815fd51d6e65030be2b47d`. See
[the driver lifecycle audit](DRIVER-LIFECYCLE-TEARDOWN-AUDIT.md).

- The reviewed ATA and block-device APIs expose initialization and I/O but no
  general channel shutdown, device detach, or unregister operation. The ATA
  channel lock is released on the inspected I/O error and success paths.
- The PS/2 mouse initialization path marks the controller absent on an early
  configuration-response failure but does not restore the prior i8042
  configuration in that branch. Later defaults/enable failures take a
  different keyboard-rearm path.
- The timer interrupt calls `keyboard_poll()`, which delegates to
  `keyboard_irq_handler()`; the dedicated keyboard vector also invokes that
  handler. Input-controller drain ownership across these entry paths needs an
  explicit contract.
- The timer path may call `mouse_poll()`; USB polling and retry service are
  separate stateful paths. The xHCI implementation was intentionally not
  inspected in this audit.
- The RTL8139 driver has static RX/TX buffers and no reviewed shutdown,
  quiescence, or unregister protocol. PCI enumeration repopulates a static
  device table and does not document concurrent re-enumeration or reader
  lifetime semantics.

These are API and cleanup gaps identified by source inspection. They do not
prove that current boot-time initialization races or that physical hot-unplug
is supported. Any future detach implementation must stop new users, drain
in-flight operations, and preserve controller/DMA ownership until quiescence.

## Outstanding audit items

1. Continue enumerating spinlock and IRQ-save acquisitions across the
   remaining kernel source files. Keyboard/mouse, RTL8139/ATA/VFS, bounded console/framebuffer/PCI, and
   selected driver lifecycle paths are now covered; remaining drivers,
   interrupt callback paths, xHCI lifecycle, and complete device teardown
   still require review.
2. Trace every call made while each lock is held, including indirect calls
   through VFS backends, paging, allocator, TLB shootdown, and callbacks.
3. Extend the heap caller-context audit across every driver and callback;
   the allocation table now has IRQ-save serialization and a concurrent host
   regression, but interrupt/NMI suitability is not claimed.
4. Establish whether any caller retains a borrowed `struct process *` across
   `process_lock` release; define stable references or generation-checked
   handles before permitting such use.
5. Document a total lock order and add debug lock-rank assertions only after
   the complete graph has been verified.
6. Extend concurrent regressions to VFS close versus process exit,
   wait/wake/timeout, fork/exec/exit, and deferred reclamation. Heap allocation
   failure recovery, capacity, and concurrent allocation/free now have a
   dedicated host regression; kernel-context coverage remains outstanding.
7. Keep `process_lock` unchanged until the graph, object-lifetime protocol,
   and production-kernel acceptance criteria are complete.

## Review boundary

This document is a source audit, not an implementation or a passing test.
It does not claim deadlock freedom, starvation freedom, safe lock-scope
reduction, or production scheduler parallelism. The global process lock and
current conservative migration restrictions remain in place.
