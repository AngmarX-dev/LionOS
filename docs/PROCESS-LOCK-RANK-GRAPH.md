# Process-Lock Rank Graph: Source Audit

## Scope and status

This document records a source-level lock-domain inventory for the process,
scheduler, wait, VFS, memory, and paging paths on
`feature/graphics-engine-v2` at parent commit
`96cdcc2a570a956fbfc9a2cfb96fc60f77183ecb`.

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

### Process job-control access to TTY state

- `process_tty_signal_foreground()` and
  `process_tty_check_foreground()` hold `process_lock` while calling
  `tty_get_foreground_pgid()`. The latter acquires `console_tty.lock`,
  so the observed nested edge is `process_lock → TTY state lock`.
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
IPC lock ────────────────────────┼──> process_lock ──> runqueue lock(s)
AP probe wait lock ─────────────┘          ├──> memory_lock
                                           └──> TTY state lock

kernel MMIO mapping ──> kernel_map_lock ──> TLB shootdown
high-map slot bookkeeping ──> high_map_lock
  (released) → kernel_map_lock → (released) → high_map_lock
  (released) → memory_lock for final unpin

TTY output_lock ──> TTY input_lock ──> TTY state lock
       └─────────────────────────────> TTY state lock

vfs_lock ──> filesystem backend calls
    └──────> heap_lock ──> memory_lock
```

The graph intentionally omits any edge that was not established by the
inspected call paths. In particular, it is not safe to infer a reverse edge
from this diagram's absence of a line.

## Outstanding audit items

1. Enumerate every spinlock and IRQ-save lock acquisition across all kernel
   source files, including drivers, interrupt paths, console/framebuffer,
   networking, storage, and device teardown.
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
